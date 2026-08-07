/*
 * Fern - Cross-Architecture Keyboard Input Implementation
 * keyboard.c
 *
 * Provides a unified keyboard input layer that dispatches to the
 * correct hardware backend at compile time:
 *
 *   x86:
 *     - PS/2 keyboard via IRQ 1 (scancode set 1)
 *     - Integrates with the existing keyboard_interrupt_handler.c
 *     - Translates scancodes to ASCII using the US layout
 *
 *   ARM32 / AArch64 / RISC-V:
 *     - UART serial input via uart_getc() / uart_getc_nonblock()
 *     - Characters arrive as ASCII directly (no scancode translation)
 *
 * The internal ring buffer provides non-blocking reads and optional
 * blocking via a simple spin-wait.
 */

#include "keyboard.h"
#include "uart.h"
#include "input.h"
#include "arch.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

#define KBD_BUF_SIZE 256

static arch_keyboard_source_t g_source    = ARCH_KBD_INPUT_NONE;
static bool                    g_initialized = false;

/* Simple ring buffer for received characters */
static volatile char    g_buf[KBD_BUF_SIZE];
static volatile uint32_t g_buf_head = 0;
static volatile uint32_t g_buf_tail = 0;

static inline uint32_t buf_count(void) {
    return (g_buf_head - g_buf_tail + KBD_BUF_SIZE) % KBD_BUF_SIZE;
}

static inline bool buf_push(char c) {
    uint32_t next = (g_buf_head + 1) % KBD_BUF_SIZE;
    if (next == g_buf_tail) return false;  /* full */
    g_buf[g_buf_head] = c;
    g_buf_head = next;
    return true;
}

static inline bool buf_pop(char* c) {
    if (g_buf_head == g_buf_tail) return false;  /* empty */
    *c = g_buf[g_buf_tail];
    g_buf_tail = (g_buf_tail + 1) % KBD_BUF_SIZE;
    return true;
}

/* ------------------------------------------------------------------ */
/* x86 PS/2 scancode translation                                       */
/* ------------------------------------------------------------------ */

#if ARCH_IS_X86

/*
 * PS/2 scancode set 1 -> ASCII translation (US layout).
 * Index = scancode, value = ASCII char (0 = no mapping).
 * Only covers make codes (bit 7 clear).
 */
static const char sc1_to_ascii[128] = {
    0,  0, '1', '2', '3', '4', '5', '6',       /* 0x00-0x07 */
    '7', '8', '9', '0', '-', '=',  0,  0,       /* 0x08-0x0F (0x0E=backspace) */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',    /* 0x10-0x17 */
    'o', 'p', '[', ']',  0,  0, 'a', 's',       /* 0x18-0x1F (0x1C=enter) */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',    /* 0x20-0x27 */
    '\'', '`',  0, '\\', 'z', 'x', 'c', 'v',   /* 0x28-0x2F (0x2A=lshift) */
    'b', 'n', 'm', ',', '.', '/',  0, '*',      /* 0x30-0x37 (0x36=rshift) */
    0,  ' ',  0,  0,  0,  0,  0,  0,            /* 0x38-0x3F (0x38=lalt) */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x40-0x47 */
    0,  0, '-',  0,  0,  0, '+',  0,            /* 0x48-0x4F */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x50-0x57 */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x58-0x5F */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x60-0x67 */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x68-0x6F */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x70-0x77 */
    0,  0,  0,  0,  0,  0,  0,  0,              /* 0x78-0x7F */
};

static const char sc1_shifted[128] = {
    0,  0, '!', '@', '#', '$', '%', '^',         /* 0x00-0x07 */
    '&', '*', '(', ')', '_', '+',  0,  0,        /* 0x08-0x0F */
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',    /* 0x10-0x17 */
    'O', 'P', '{', '}',  0,  0, 'A', 'S',       /* 0x18-0x1F */
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',    /* 0x20-0x27 */
    '"', '~',  0, '|', 'Z', 'X', 'C', 'V',     /* 0x28-0x2F */
    'B', 'N', 'M', '<', '>', '?',  0, '*',      /* 0x30-0x37 */
    0,  ' ',  0,  0,  0,  0,  0,  0,            /* 0x38-0x3F */
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0, '-',  0,  0,  0, '+',  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,
};

/* Extended scancodes (0xE0 prefix) - maps second byte to special keys */
static const uint16_t ext_sc1_special[128] = {
    [0x48] = ARCH_KEY_UP,
    [0x50] = ARCH_KEY_DOWN,
    [0x4B] = ARCH_KEY_LEFT,
    [0x4D] = ARCH_KEY_RIGHT,
    [0x47] = ARCH_KEY_HOME,
    [0x4F] = ARCH_KEY_END,
    [0x49] = ARCH_KEY_PAGE_UP,
    [0x51] = ARCH_KEY_PAGE_DOWN,
    [0x52] = ARCH_KEY_INSERT,
    [0x53] = ARCH_KEY_DELETE,
};

/* PS/2 state machine */
static bool     g_ps2_extended = false;
static bool     g_ps2_shift    = false;
static bool     g_ps2_ctrl     = false;
static bool     g_ps2_alt      = false;
static bool     g_ps2_caps     = false;

static void ps2_process_scancode(uint8_t sc) {
    /* Extended prefix */
    if (sc == 0xE0) {
        g_ps2_extended = true;
        return;
    }

    bool released = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    /* Modifier key tracking */
    switch (code) {
        case 0x2A: case 0x36: /* Left/Right Shift */
            g_ps2_shift = !released;
            g_ps2_extended = false;
            return;
        case 0x1D: /* Left Ctrl */
            g_ps2_ctrl = !released;
            g_ps2_extended = false;
            return;
        case 0x38: /* Left Alt */
            g_ps2_alt = !released;
            g_ps2_extended = false;
            return;
        case 0x3A: /* Caps Lock */
            if (!released) g_ps2_caps = !g_ps2_caps;
            g_ps2_extended = false;
            return;
        default:
            break;
    }

    /* Ignore key releases */
    if (released) {
        g_ps2_extended = false;
        return;
    }

    /* Extended keys: arrows, home, end, etc. */
    if (g_ps2_extended) {
        if (code < 128 && ext_sc1_special[code] != 0) {
            /* Store special key as two-byte sequence: 0x00 + key_lo */
            uint16_t key = ext_sc1_special[code];
            buf_push(0x00);
            buf_push((char)(key & 0xFF));
        }
        g_ps2_extended = false;
        return;
    }

    /* Regular keys: scancode -> ASCII */
    char c = 0;
    if (code < 128) {
        if (g_ps2_shift)
            c = sc1_shifted[code];
        else
            c = sc1_to_ascii[code];

        /* Caps Lock toggles letters */
        if (g_ps2_caps && c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        else if (g_ps2_caps && c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
    }

    /* Special handling for non-printable keys */
    switch (code) {
        case 0x0E: c = '\b'; break;     /* Backspace */
        case 0x0F: c = '\t'; break;     /* Tab */
        case 0x1C: c = '\n'; break;     /* Enter */
        case 0x39: c = ' '; break;      /* Space */
        case 0x01: c = '\x1B'; break;   /* Escape */
        default: break;
    }

    /* Ctrl+letter -> control code */
    if (g_ps2_ctrl && c >= 'a' && c <= 'z') {
        c = c - 'a' + 1;
    }

    if (c != 0) {
        buf_push(c);
    }
}

#endif /* ARCH_IS_X86 */

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int arch_keyboard_init(void) {
    if (g_initialized)
        return 0;

    g_buf_head = 0;
    g_buf_tail = 0;

    /* Initialize the unified input event system */
    input_init();

#if ARCH_IS_X86
    /*
     * On x86, the PS/2 keyboard is typically initialized by the
     * existing keyboard_interrupt_handler.c during early boot. We
     * just note the source. If the PS/2 controller isn't present
     * (e.g., pure UEFI with USB HID only), we fall through to
     * serial input as a degraded fallback.
     */
    g_source = ARCH_KBD_INPUT_PS2;
    g_initialized = true;
    return 0;

#elif ARCH_ARM32 || ARCH_ARM64 || ARCH_RISCV64
    /*
     * On ARM/RISC-V, keyboard input comes from the serial UART.
     * The UART must already be initialised (uart_init() called
     * before arch_keyboard_init()). We enable RX interrupts if
     * supported, otherwise rely on polling.
     */
    if (uart_get_type() != UART_TYPE_NONE) {
        uart_enable_rx_irq();
        g_source = ARCH_KBD_INPUT_UART;
        g_initialized = true;
        return 0;
    }

    g_source = ARCH_KBD_INPUT_NONE;
    g_initialized = true;
    return -1;

#else
    g_source = ARCH_KBD_INPUT_NONE;
    g_initialized = true;
    return -1;
#endif
}

int arch_keyboard_getc(void) {
    if (!g_initialized) return -1;

    char c;
    /* Spin until a character is available */
    while (!buf_pop(&c)) {
        /* On UART platforms, poll for incoming data */
#if ARCH_ARM32 || ARCH_ARM64 || ARCH_RISCV64
        int ch = uart_getc_nonblock();
        if (ch >= 0) {
            buf_push((char)ch);
        }
#endif
        arch_cpu_relax();
    }
    return (int)(unsigned char)c;
}

int arch_keyboard_getc_nonblocking(void) {
    if (!g_initialized) return -1;

#if ARCH_ARM32 || ARCH_ARM64 || ARCH_RISCV64
    /* Try to pull from UART into buffer */
    int ch = uart_getc_nonblock();
    if (ch >= 0) {
        buf_push((char)ch);
    }
#endif

    char c;
    if (buf_pop(&c))
        return (int)(unsigned char)c;
    return -1;
}

void arch_keyboard_handler(char c) {
    if (!g_initialized) return;

#if ARCH_IS_X86
    /* x86: treat byte as a PS/2 scancode */
    ps2_process_scancode((uint8_t)c);
#else
    /* ARM/RISC-V: byte is already ASCII from UART */
    buf_push(c);

    /* Also feed into the unified input event system */
    input_uart_char(c);
#endif
}

arch_keyboard_source_t arch_keyboard_get_source(void) {
    return g_source;
}

bool arch_keyboard_is_available(void) {
    return g_initialized && g_source != ARCH_KBD_INPUT_NONE;
}

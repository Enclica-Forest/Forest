/*
 * Fern - Cross-Architecture Input Event Implementation
 * input.c
 *
 * Bridges architecture-specific hardware to the unified input event system.
 *
 * On x86:
 *   - PS/2 keyboard scancodes are translated to KEY_* codes via
 *     the existing keyboard.c / ps2_keyboard.c layer.
 *   - PS/2 mouse packets generate EV_REL + EV_KEY events.
 *
 * On ARM32 / AArch64 / RISC-V:
 *   - UART bytes are mapped to ASCII and reported as EV_KEY events
 *     with corresponding KEY_* codes.  This gives a minimal keyboard
 *     input path for early boot console and kernel shell.
 */

#include "input.h"
#include "arch.h"
#include "../include/debuglog.h"
#include "../include/string.h"

/* =======================================================================
 * Internal state
 * ======================================================================= */

static bool             g_input_initialized = false;
static input_ring_t     g_event_ring;
static input_device_t   g_devices[INPUT_MAX_DEVICES];
static uint32           g_device_count = 0;
static uint32           g_next_device_id = 1;

/* =======================================================================
 * ASCII -> KEY_* mapping for UART input (non-x86)
 *
 * Converts printable ASCII to Linux evdev key codes so that the
 * unified event system works identically on all architectures.
 * ======================================================================= */

#if !ARCH_IS_X86

static uint32 ascii_to_keycode(char c) {
    /* Digits */
    if (c >= '0' && c <= '9')
        return c - '0' + 0x0B;  /* KEY_0 = 0x0B */

    /* Lowercase letters */
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 0x1E;  /* KEY_A = 0x1E */

    /* Uppercase letters -> same code (shift handled separately) */
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 0x1E;

    /* Punctuation / symbols (US layout) */
    switch (c) {
        case '\x1B': return 0x01;  /* KEY_ESC */
        case '\t':   return 0x0F;  /* KEY_TAB */
        case '\n':
        case '\r':   return 0x1C;  /* KEY_ENTER */
        case ' ':    return 0x39;  /* KEY_SPACE */
        case '\b':   return 0x0E;  /* KEY_BACKSPACE */
        case '-':    return 0x0C;  /* KEY_MINUS */
        case '=':    return 0x0D;  /* KEY_EQUAL */
        case '[':    return 0x1A;  /* KEY_LEFTBRACE */
        case ']':    return 0x1B;  /* KEY_RIGHTBRACE */
        case '\\':   return 0x2B;  /* KEY_BACKSLASH */
        case ';':    return 0x27;  /* KEY_SEMICOLON */
        case '\'':   return 0x28;  /* KEY_APOSTROPHE */
        case '`':    return 0x29;  /* KEY_GRAVE */
        case ',':    return 0x33;  /* KEY_COMMA */
        case '.':    return 0x34;  /* KEY_DOT */
        case '/':    return 0x35;  /* KEY_SLASH */
        case '!':    return 0x02;  /* KEY_1 + shift */
        case '@':    return 0x03;  /* KEY_2 + shift */
        case '#':    return 0x04;  /* KEY_3 + shift */
        case '$':    return 0x05;  /* KEY_4 + shift */
        case '%':    return 0x06;  /* KEY_5 + shift */
        case '^':    return 0x07;  /* KEY_6 + shift */
        case '&':    return 0x08;  /* KEY_7 + shift */
        case '*':    return 0x09;  /* KEY_8 + shift */
        case '(':    return 0x0A;  /* KEY_9 + shift */
        case ')':    return 0x0B;  /* KEY_0 + shift */
        case '_':    return 0x0C;  /* KEY_MINUS + shift */
        case '+':    return 0x0D;  /* KEY_EQUAL + shift */
        case '{':    return 0x1A;  /* KEY_LEFTBRACE + shift */
        case '}':    return 0x1B;  /* KEY_RIGHTBRACE + shift */
        case '|':    return 0x2B;  /* KEY_BACKSLASH + shift */
        case ':':    return 0x27;  /* KEY_SEMICOLON + shift */
        case '"':    return 0x28;  /* KEY_APOSTROPHE + shift */
        case '~':    return 0x29;  /* KEY_GRAVE + shift */
        case '<':    return 0x33;  /* KEY_COMMA + shift */
        case '>':    return 0x34;  /* KEY_DOT + shift */
        case '?':    return 0x35;  /* KEY_SLASH + shift */
        default:     return 0;
    }
}

/*
 * Check if an ASCII character requires shift to produce.
 * Used to generate KEY_PRESS for shift alongside the character key.
 */
static bool needs_shift(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    switch (c) {
        case '!': case '@': case '#': case '$': case '%':
        case '^': case '&': case '*': case '(': case ')':
        case '_': case '+': case '{': case '}': case '|':
        case ':': case '"': case '~': case '<': case '>':
        case '?':
            return true;
        default:
            return false;
    }
}

#endif /* !ARCH_IS_X86 */

/* =======================================================================
 * Public API
 * ======================================================================= */

int input_init(void) {
    if (g_input_initialized)
        return 0;

    /* Initialize the global event ring buffer */
    input_ring_init(&g_event_ring, "input_global");

    g_device_count = 0;
    g_next_device_id = 1;
    memset(g_devices, 0, sizeof(g_devices));

    /* Register architecture-appropriate devices */
#if ARCH_IS_X86
    /*
     * x86: PS/2 keyboard and mouse are handled by the existing
     * ps2_keyboard.c and ps2_mouse.c drivers.  We register them
     * as known devices so the event system can identify them.
     */
    input_register_device("PS/2 Keyboard", INPUT_DEV_TYPE_KEYBOARD);
    input_register_device("PS/2 Mouse", INPUT_DEV_TYPE_MOUSE);

#elif ARCH_ARM32 || ARCH_ARM64 || ARCH_RISCV64
    /*
     * ARM/RISC-V: Input comes from the serial UART.
     * Treat each byte as a keyboard event.
     */
    input_register_device("UART Keyboard", INPUT_DEV_TYPE_KEYBOARD);

#else
    /* No input source available */
    debuglog(DEBUG_WARN, "[Input] No input source for this architecture\n");
#endif

    g_input_initialized = true;
    debuglog(DEBUG_INFO, "[Input] Initialized with %u device(s)\n", g_device_count);
    return 0;
}

uint32 input_register_device(const char* name, uint32 type) {
    if (!name || g_device_count >= INPUT_MAX_DEVICES)
        return 0;

    input_device_t* dev = &g_devices[g_device_count];
    dev->id = g_next_device_id++;
    dev->name = name;
    dev->type = type;
    dev->active = true;

    /* Assign bus type based on architecture and device type */
#if ARCH_IS_X86
    if (type & INPUT_DEV_TYPE_KEYBOARD)
        dev->bus_type = BUS_I8042;
    else if (type & INPUT_DEV_TYPE_MOUSE)
        dev->bus_type = BUS_I8042;
    else
        dev->bus_type = BUS_PCI;
#else
    dev->bus_type = BUS_RS232;
#endif

    g_device_count++;

    debuglog(DEBUG_INFO, "[Input] Registered device '%s' id=%u type=0x%x\n",
             name, dev->id, type);
    return dev->id;
}

void input_report_key(uint32 device_id, uint32 key, int32 value) {
    (void)device_id;
    if (!g_input_initialized) return;

    input_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = EV_KEY;
    event.code = (uint16)key;
    event.value = value;

    input_ring_push(&g_event_ring, &event);
}

void input_report_mouse(uint32 device_id, int32 dx, int32 dy, uint32 buttons) {
    (void)device_id;
    if (!g_input_initialized) return;

    /* Report relative movement */
    if (dx != 0) {
        input_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = EV_REL;
        event.code = REL_X;
        event.value = dx;
        input_ring_push(&g_event_ring, &event);
    }

    if (dy != 0) {
        input_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = EV_REL;
        event.code = REL_Y;
        event.value = dy;
        input_ring_push(&g_event_ring, &event);
    }

    /* Report button state changes */
    static uint32 prev_buttons = 0;
    uint32 changed = buttons ^ prev_buttons;

    if (changed & 0x01) {
        input_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = EV_KEY;
        event.code = BTN_LEFT;
        event.value = (buttons & 0x01) ? KEY_PRESS : KEY_RELEASE;
        input_ring_push(&g_event_ring, &event);
    }
    if (changed & 0x02) {
        input_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = EV_KEY;
        event.code = BTN_RIGHT;
        event.value = (buttons & 0x02) ? KEY_PRESS : KEY_RELEASE;
        input_ring_push(&g_event_ring, &event);
    }
    if (changed & 0x04) {
        input_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = EV_KEY;
        event.code = BTN_MIDDLE;
        event.value = (buttons & 0x04) ? KEY_PRESS : KEY_RELEASE;
        input_ring_push(&g_event_ring, &event);
    }

    prev_buttons = buttons;
}

bool input_get_event(input_event_t* event) {
    if (!g_input_initialized || !event) return false;
    return input_ring_pop(&g_event_ring, event);
}

bool input_has_events(void) {
    if (!g_input_initialized) return false;
    return !input_ring_is_empty(&g_event_ring);
}

uint32 input_get_device_count(void) {
    return g_device_count;
}

const input_device_t* input_get_device(uint32 device_id) {
    for (uint32 i = 0; i < g_device_count; i++) {
        if (g_devices[i].id == device_id)
            return &g_devices[i];
    }
    return NULL;
}

void input_dump_devices(void) {
    debuglog(DEBUG_INFO, "[Input] Devices (%u):\n", g_device_count);
    for (uint32 i = 0; i < g_device_count; i++) {
        const input_device_t* dev = &g_devices[i];
        const char* type_str = "unknown";
        if (dev->type == INPUT_DEV_TYPE_KEYBOARD) type_str = "keyboard";
        else if (dev->type == INPUT_DEV_TYPE_MOUSE) type_str = "mouse";
        else if (dev->type == INPUT_DEV_TYPE_TOUCH) type_str = "touchscreen";

        debuglog(DEBUG_INFO, "  [%u] id=%u name='%s' type=%s bus=0x%x active=%d\n",
                 i, dev->id, dev->name, type_str, dev->bus_type, dev->active);
    }
}

/* =======================================================================
 * UART character -> input event bridge (non-x86 architectures)
 *
 * This function is called from arch_keyboard_handler() on ARM/RISC-V
 * to convert an ASCII byte from the UART into proper input events.
 * It generates EV_KEY press + release pairs, plus shift events for
 * uppercase / shifted characters.
 * ======================================================================= */

#if !ARCH_IS_X86

void input_uart_char(char c) {
    if (!g_input_initialized || c == 0) return;

    uint32 keycode = ascii_to_keycode(c);
    if (keycode == 0) return;

    /* Generate shift press if needed */
    if (needs_shift(c)) {
        input_report_key(1, 0x2A, KEY_PRESS);  /* KEY_LEFTSHIFT */
    }

    /* Generate key press */
    input_report_key(1, keycode, KEY_PRESS);

    /* Generate key release */
    input_report_key(1, keycode, KEY_RELEASE);

    /* Generate shift release if needed */
    if (needs_shift(c)) {
        input_report_key(1, 0x2A, KEY_RELEASE);  /* KEY_LEFTSHIFT */
    }
}

#endif /* !ARCH_IS_X86 */

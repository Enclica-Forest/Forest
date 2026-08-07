/*
 * Fern - Cross-Architecture UART Interface
 * uart.h
 *
 * Unified serial console API for all supported platforms.
 * Dispatches to the correct hardware driver at runtime:
 *   - x86:     16550 COM1 (port I/O at 0x3F8)
 *   - ARM32:   PL011 (MMIO, default 0x09000000)
 *   - AArch64: PL011 (MMIO, default 0x09000000)
 *   - RISC-V:  8250-compatible (MMIO at 0x10000000)
 *
 * On platforms with a Flattened Device Tree (ARM, RISC-V), the base
 * address and UART type are queried from /chosen/stdout-path when
 * available; otherwise compile-time defaults are used.
 *
 * All functions are safe to call from early boot (before heap, MMU,
 * or interrupts are configured).
 */

#ifndef FOREST_ARCH_UART_H
#define FOREST_ARCH_UART_H

#include <stdint.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * UART hardware type identifiers
 * --------------------------------------------------------------------- */
enum uart_type {
    UART_TYPE_NONE = 0,
    UART_TYPE_16550,        /* x86 16550 / 8250 (port I/O)              */
    UART_TYPE_PL011,        /* ARM PL011 (MMIO)                         */
    UART_TYPE_8250_MMIO,    /* RISC-V 8250-compatible (MMIO)            */
};

/* -----------------------------------------------------------------------
 * UART driver vtable
 *
 * Each hardware driver fills in this table.  The unified API dispatches
 * through these pointers so there is zero overhead beyond an indirect
 * call.
 * --------------------------------------------------------------------- */
struct uart_driver {
    enum uart_type type;
    void (*init)(uint32_t baud_rate);
    void (*putc)(char c);
    int  (*getc)(void);          /* blocking; returns char or -1 on error */
    int  (*getc_nonblock)(void); /* returns char or -1 if none available  */
    void (*puts)(const char *s);
    void (*vprintf)(const char *fmt, va_list ap);
    /* Optional IRQ support (NULL if not implemented) */
    void (*enable_rx_irq)(void);
    void (*disable_rx_irq)(void);
    void (*clear_irq)(void);
    int  (*tx_ready)(void);
    int  (*rx_ready)(void);
};

/* -----------------------------------------------------------------------
 * Public API
 *
 * These are the cross-architecture entry points.  They dispatch through
 * the active driver vtable set by uart_init().
 * --------------------------------------------------------------------- */

/**
 * uart_init() - Initialise the serial console.
 *
 * Detects the UART hardware (via compile-time arch or DTB query),
 * configures it for 8N1 at the given baud rate, and installs the
 * driver vtable.
 *
 * @baud_rate: Desired baud rate (e.g. 115200).  Pass 0 for the
 *             hardware default.
 */
void uart_init(uint32_t baud_rate);

/**
 * uart_putc() - Output one character (blocking).
 */
void uart_putc(char c);

/**
 * uart_getc() - Input one character (blocking).
 *
 * Returns the received character as an unsigned char cast to int,
 * or -1 on error.
 */
int uart_getc(void);

/**
 * uart_getc_nonblock() - Non-blocking receive.
 *
 * Returns the received character, or -1 if no data is available.
 */
int uart_getc_nonblock(void);

/**
 * uart_puts() - Output a NUL-terminated string.
 *
 * Translates '\n' to "\r\n" for terminal compatibility.
 */
void uart_puts(const char *str);

/**
 * uart_printf() - Formatted output to the serial console.
 *
 * Supports: %c %s %d %u %x %X %p %% and width/zero-padding.
 */
void uart_printf(const char *fmt, ...);

/**
 * uart_vprintf() - va_list variant of uart_printf().
 */
void uart_vprintf(const char *fmt, va_list ap);

/* -----------------------------------------------------------------------
 * Optional IRQ support (NULL-check before calling)
 * --------------------------------------------------------------------- */

/** uart_enable_rx_irq() - Enable RX interrupt if the driver supports it. */
void uart_enable_rx_irq(void);

/** uart_disable_rx_irq() - Disable RX interrupt. */
void uart_disable_rx_irq(void);

/** uart_clear_irq() - Acknowledge / clear pending UART interrupts. */
void uart_clear_irq(void);

/** uart_tx_ready() - Non-zero if TX can accept a byte without blocking. */
int uart_tx_ready(void);

/** uart_rx_ready() - Non-zero if RX has data available. */
int uart_rx_ready(void);

/* -----------------------------------------------------------------------
 * Driver registration (used by arch-specific init code)
 * --------------------------------------------------------------------- */

/**
 * uart_register_driver() - Install a hardware driver vtable.
 *
 * Called once during early boot after the UART is detected.
 *
 * @drv: Pointer to a populated uart_driver struct (must stay in scope).
 */
void uart_register_driver(const struct uart_driver *drv);

/**
 * uart_get_driver() - Return the currently active driver vtable.
 *
 * Returns NULL if uart_init() has not been called yet.
 */
const struct uart_driver *uart_get_driver(void);

/**
 * uart_get_type() - Return the detected UART hardware type.
 */
enum uart_type uart_get_type(void);

#endif /* FOREST_ARCH_UART_H */

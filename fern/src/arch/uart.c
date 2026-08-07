/*
 * Fern - Cross-Architecture UART Implementation
 * uart.c
 *
 * Provides a unified serial console that dispatches to the correct
 * hardware driver at runtime.  The active driver is installed by
 * uart_init() based on compile-time architecture detection.
 *
 * Supported hardware:
 *   - x86:     16550 COM1 via port I/O (0x3F8)
 *   - ARM32:   PL011 MMIO (default 0x09000000, QEMU virt)
 *   - AArch64: PL011 MMIO (default 0x09000000, QEMU virt)
 *   - RISC-V:  8250-compatible MMIO (0x10000000, QEMU virt)
 *
 * Each architecture's driver is self-contained within the corresponding
 * #if block.  The arch-specific uart.c files (src/arm32/uart.c, etc.)
 * remain for code that calls them directly; they compile to separate
 * symbols only when used standalone.
 *
 * Generic helpers (uart_puts, uart_printf, uart_vprintf) are implemented
 * here in terms of the driver's putc, so they work with any driver.
 */

#include "uart.h"
#include "arch.h"
#include "platform.h"
#include <stdint.h>
#include <stdarg.h>

/* =======================================================================
 * Internal state
 * ======================================================================= */

static const struct uart_driver *active_driver = NULL;

/* =======================================================================
 * Default helpers – built on top of the driver's putc
 * ======================================================================= */

static void uart_puts_default(const char *s)
{
    while (*s) {
        if (*s == '\n')
            active_driver->putc('\r');
        active_driver->putc(*s++);
    }
}

/*
 * Minimal printf helpers (no heap, no floating-point).
 * Supports: %c %s %d %i %u %x %X %p %%
 * Width and zero-padding for integers (e.g. %08x).
 * 64-bit via %lu / %lx / %lld / %llu / %llx.
 */

static void print_uint(uint64_t n, uint32_t base, int upcase,
                       int min_width, char pad_char)
{
    const char *digits_lo = "0123456789abcdef";
    const char *digits_hi = "0123456789ABCDEF";
    const char *digits = upcase ? digits_hi : digits_lo;
    char buf[24];
    int  pos = 0;

    if (n == 0) {
        buf[pos++] = '0';
    } else {
        while (n) {
            buf[pos++] = digits[n % base];
            n /= base;
        }
    }

    while (pos < min_width)
        buf[pos++] = pad_char;

    for (int i = pos - 1; i >= 0; --i)
        active_driver->putc(buf[i]);
}

static void print_int(int64_t n, int min_width, char pad_char)
{
    if (n < 0) {
        active_driver->putc('-');
        print_uint((uint64_t)(-(int64_t)n), 10, 0, min_width - 1, pad_char);
    } else {
        print_uint((uint64_t)n, 10, 0, min_width, pad_char);
    }
}

static void uart_vprintf_default(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') active_driver->putc('\r');
            active_driver->putc(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        char pad_char = ' ';
        int  min_width = 0;

        if (*fmt == '0') {
            pad_char = '0';
            fmt++;
        }
        while (*fmt >= '1' && *fmt <= '9') {
            min_width = min_width * 10 + (*fmt - '0');
            fmt++;
        }

        switch (*fmt++) {
        case 'c':
            active_driver->putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            uart_puts_default(s);
            break;
        }
        case 'd':
        case 'i':
            print_int(va_arg(ap, int), min_width, pad_char);
            break;
        case 'u':
            print_uint(va_arg(ap, uint32_t), 10, 0, min_width, pad_char);
            break;
        case 'x':
            print_uint(va_arg(ap, uint32_t), 16, 0, min_width, pad_char);
            break;
        case 'X':
            print_uint(va_arg(ap, uint32_t), 16, 1, min_width, pad_char);
            break;
        case 'p':
            active_driver->putc('0');
            active_driver->putc('x');
            print_uint((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0,
                       ARCH_IS_64BIT ? 16 : 8, '0');
            break;
        case 'l':
            fmt++;
            if (*fmt == 'l') {
                fmt++;
                switch (*fmt++) {
                case 'd':
                    print_int((int64_t)va_arg(ap, long long), min_width, pad_char);
                    break;
                case 'u':
                    print_uint((uint64_t)va_arg(ap, unsigned long long), 10, 0,
                               min_width, pad_char);
                    break;
                case 'x':
                    print_uint((uint64_t)va_arg(ap, unsigned long long), 16, 0,
                               min_width, pad_char);
                    break;
                default:
                    active_driver->putc('l');
                    active_driver->putc('l');
                    active_driver->putc(fmt[-1]);
                    break;
                }
            } else {
                switch (*fmt++) {
                case 'd':
                    print_int((int64_t)va_arg(ap, long), min_width, pad_char);
                    break;
                case 'u':
                    print_uint((uint64_t)va_arg(ap, unsigned long), 10, 0,
                               min_width, pad_char);
                    break;
                case 'x':
                    print_uint((uint64_t)va_arg(ap, unsigned long), 16, 0,
                               min_width, pad_char);
                    break;
                default:
                    active_driver->putc('l');
                    active_driver->putc(fmt[-1]);
                    break;
                }
            }
            break;
        case '%':
            active_driver->putc('%');
            break;
        default:
            active_driver->putc('?');
            break;
        }
    }
}

/* =======================================================================
 * Driver registration
 * ======================================================================= */

void uart_register_driver(const struct uart_driver *drv)
{
    active_driver = drv;
}

const struct uart_driver *uart_get_driver(void)
{
    return active_driver;
}

enum uart_type uart_get_type(void)
{
    return active_driver ? active_driver->type : UART_TYPE_NONE;
}

/* =======================================================================
 * Unified public API – dispatches through the active driver
 * ======================================================================= */

void uart_init(uint32_t baud_rate)
{
    /*
     * If a driver is already registered (e.g. from arch-specific early
     * boot code), just re-init it.  Otherwise, auto-detect and register.
     */
    if (!active_driver) {
#if ARCH_X86_32 || ARCH_X86_64
        extern const struct uart_driver uart_driver_16550;
        uart_register_driver(&uart_driver_16550);
#elif ARCH_ARM32
        extern const struct uart_driver uart_driver_pl011_arm32;
        uart_register_driver(&uart_driver_pl011_arm32);
#elif ARCH_ARM64
        extern const struct uart_driver uart_driver_pl011_aarch64;
        uart_register_driver(&uart_driver_pl011_aarch64);
#elif ARCH_RISCV64
        extern const struct uart_driver uart_driver_8250_mmio;
        uart_register_driver(&uart_driver_8250_mmio);
#else
#error "uart_init: no UART driver for this architecture"
#endif
    }

    if (active_driver->init)
        active_driver->init(baud_rate);
}

void uart_putc(char c)
{
    if (active_driver && active_driver->putc)
        active_driver->putc(c);
}

int uart_getc(void)
{
    if (active_driver && active_driver->getc)
        return active_driver->getc();
    return -1;
}

int uart_getc_nonblock(void)
{
    if (active_driver && active_driver->getc_nonblock)
        return active_driver->getc_nonblock();
    return -1;
}

void uart_puts(const char *str)
{
    if (!active_driver)
        return;
    if (active_driver->puts) {
        active_driver->puts(str);
    } else if (active_driver->putc) {
        uart_puts_default(str);
    }
}

void uart_vprintf(const char *fmt, va_list ap)
{
    if (!active_driver)
        return;
    if (active_driver->vprintf) {
        active_driver->vprintf(fmt, ap);
    } else if (active_driver->putc) {
        uart_vprintf_default(fmt, ap);
    }
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
}

void uart_enable_rx_irq(void)
{
    if (active_driver && active_driver->enable_rx_irq)
        active_driver->enable_rx_irq();
}

void uart_disable_rx_irq(void)
{
    if (active_driver && active_driver->disable_rx_irq)
        active_driver->disable_rx_irq();
}

void uart_clear_irq(void)
{
    if (active_driver && active_driver->clear_irq)
        active_driver->clear_irq();
}

int uart_tx_ready(void)
{
    if (active_driver && active_driver->tx_ready)
        return active_driver->tx_ready();
    return 0;
}

int uart_rx_ready(void)
{
    if (active_driver && active_driver->rx_ready)
        return active_driver->rx_ready();
    return 0;
}

/* =======================================================================
 * x86: 16550 COM1 driver (port I/O)
 * ======================================================================= */
#if ARCH_X86_32 || ARCH_X86_64

#define COM1_PORT PLATFORM_UART0_PORT

static inline uint8_t com1_in(uint8_t reg)
{
    return x86_32_inb((uint16_t)(COM1_PORT + reg));
}

static inline void com1_out(uint8_t reg, uint8_t val)
{
    x86_32_outb((uint16_t)(COM1_PORT + reg), val);
}

#define COM_THR  0
#define COM_RBR  0
#define COM_IER  1
#define COM_FCR  2
#define COM_LCR  3
#define COM_MCR  4
#define COM_LSR  5
#define COM_DLL  0
#define COM_DLH  1

#define LSR_DR    (1U << 0)
#define LSR_THRE  (1U << 5)

static void driver_16550_init(uint32_t baud_rate)
{
    if (baud_rate == 0) baud_rate = 115200;

    com1_out(COM_IER, 0x00);
    com1_out(COM_LCR, 0x80);
    uint32_t divisor = 115200 / baud_rate;
    com1_out(COM_DLL, (uint8_t)(divisor & 0xFF));
    com1_out(COM_DLH, (uint8_t)((divisor >> 8) & 0xFF));
    com1_out(COM_LCR, 0x03);
    com1_out(COM_FCR, 0xC7);
    com1_out(COM_MCR, 0x0B);
}

static void driver_16550_putc(char c)
{
    if (c == '\n') {
        while (!(com1_in(COM_LSR) & LSR_THRE))
            arch_cpu_relax();
        com1_out(COM_THR, '\r');
    }
    while (!(com1_in(COM_LSR) & LSR_THRE))
        arch_cpu_relax();
    com1_out(COM_THR, (uint8_t)c);
}

static int driver_16550_getc(void)
{
    while (!(com1_in(COM_LSR) & LSR_DR))
        arch_cpu_relax();
    return (int)(uint8_t)com1_in(COM_RBR);
}

static int driver_16550_getc_nonblock(void)
{
    if (!(com1_in(COM_LSR) & LSR_DR))
        return -1;
    return (int)(uint8_t)com1_in(COM_RBR);
}

static int driver_16550_tx_ready(void)
{
    return (com1_in(COM_LSR) & LSR_THRE) != 0;
}

static int driver_16550_rx_ready(void)
{
    return (com1_in(COM_LSR) & LSR_DR) != 0;
}

const struct uart_driver uart_driver_16550 = {
    .type           = UART_TYPE_16550,
    .init           = driver_16550_init,
    .putc           = driver_16550_putc,
    .getc           = driver_16550_getc,
    .getc_nonblock  = driver_16550_getc_nonblock,
    .puts           = NULL,
    .vprintf        = NULL,
    .enable_rx_irq  = NULL,
    .disable_rx_irq = NULL,
    .clear_irq      = NULL,
    .tx_ready       = driver_16550_tx_ready,
    .rx_ready       = driver_16550_rx_ready,
};

#endif /* x86 */

/* =======================================================================
 * ARM32: PL011 driver
 * ======================================================================= */
#if ARCH_ARM32

#ifndef UART_BASE_ADDR
#define UART_BASE_ADDR 0x09000000UL
#endif

/* PL011 register offsets */
#define PL011_DR    0x000
#define PL011_FR    0x018
#define PL011_IBRD  0x024
#define PL011_FBRD  0x028
#define PL011_LCRH  0x02C
#define PL011_CR    0x030
#define PL011_IMSC  0x038
#define PL011_ICR   0x044

#define PL011_FR_TXFF   (1u << 5)
#define PL011_FR_RXFE   (1u << 4)
#define PL011_FR_BUSY   (1u << 3)
#define PL011_LCRH_FEN  (1u << 4)
#define PL011_LCRH_WLEN8 (3u << 5)
#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)
#define PL011_CR_RXE    (1u << 9)
#define PL011_INT_ALL   0x7FFu

static inline void pl011_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(UART_BASE_ADDR + off) = val;
}

static inline uint32_t pl011_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(UART_BASE_ADDR + off);
}

static void driver_pl011_arm32_init(uint32_t baud_rate)
{
    if (baud_rate == 0) baud_rate = 115200;

    pl011_write(PL011_CR, 0);
    while (pl011_read(PL011_FR) & PL011_FR_BUSY)
        ;
    pl011_write(PL011_LCRH, 0);
    pl011_write(PL011_ICR, PL011_INT_ALL);

    uint32_t uartclk = 24000000UL;
    uint32_t combined = (uartclk * 4u + baud_rate / 2u) / baud_rate;
    pl011_write(PL011_IBRD, combined >> 6);
    pl011_write(PL011_FBRD, combined & 0x3Fu);

    pl011_write(PL011_LCRH, PL011_LCRH_WLEN8 | PL011_LCRH_FEN);
    pl011_write(PL011_IMSC, 0);
    pl011_write(PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
}

static void driver_pl011_arm32_putc(char c)
{
    if (c == '\n') {
        while (pl011_read(PL011_FR) & PL011_FR_TXFF)
            ;
        pl011_write(PL011_DR, '\r');
    }
    while (pl011_read(PL011_FR) & PL011_FR_TXFF)
        ;
    pl011_write(PL011_DR, (uint32_t)(unsigned char)c);
}

static int driver_pl011_arm32_getc(void)
{
    while (pl011_read(PL011_FR) & PL011_FR_RXFE)
        ;
    return (int)(pl011_read(PL011_DR) & 0xFF);
}

static int driver_pl011_arm32_getc_nonblock(void)
{
    if (pl011_read(PL011_FR) & PL011_FR_RXFE)
        return -1;
    return (int)(pl011_read(PL011_DR) & 0xFF);
}

static int driver_pl011_arm32_tx_ready(void)
{
    return !(pl011_read(PL011_FR) & PL011_FR_TXFF);
}

static int driver_pl011_arm32_rx_ready(void)
{
    return !(pl011_read(PL011_FR) & PL011_FR_RXFE);
}

const struct uart_driver uart_driver_pl011_arm32 = {
    .type           = UART_TYPE_PL011,
    .init           = driver_pl011_arm32_init,
    .putc           = driver_pl011_arm32_putc,
    .getc           = driver_pl011_arm32_getc,
    .getc_nonblock  = driver_pl011_arm32_getc_nonblock,
    .puts           = NULL,
    .vprintf        = NULL,
    .enable_rx_irq  = NULL,
    .disable_rx_irq = NULL,
    .clear_irq      = NULL,
    .tx_ready       = driver_pl011_arm32_tx_ready,
    .rx_ready       = driver_pl011_arm32_rx_ready,
};

#endif /* ARM32 */

/* =======================================================================
 * AArch64: PL011 driver
 * ======================================================================= */
#if ARCH_ARM64

#define PL011_BASE_AARCH64 0x09000000UL

#define AARCH64_UARTDR    0x000
#define AARCH64_UARTFR    0x018
#define AARCH64_UARTIBRD  0x024
#define AARCH64_UARTFBRD  0x028
#define AARCH64_UARTLCRH  0x02C
#define AARCH64_UARTCR    0x030
#define AARCH64_UARTIMSC  0x038
#define AARCH64_UARTICR   0x044

#define AARCH64_UARTFR_TXFF  (1 << 5)
#define AARCH64_UARTFR_RXFE  (1 << 4)
#define AARCH64_UARTFR_BUSY  (1 << 3)
#define AARCH64_UARTLCRH_FEN (1 << 4)
#define AARCH64_UARTLCRH_WLEN8 (3 << 5)
#define AARCH64_UARTCR_UARTEN (1 << 0)
#define AARCH64_UARTCR_TXE   (1 << 8)
#define AARCH64_UARTCR_RXE   (1 << 9)

static inline void aarch64_pl011_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(PL011_BASE_AARCH64 + off) = val;
}

static inline uint32_t aarch64_pl011_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(PL011_BASE_AARCH64 + off);
}

static void driver_pl011_aarch64_init(uint32_t baud_rate)
{
    (void)baud_rate;

    aarch64_pl011_write(AARCH64_UARTCR, 0);
    while (aarch64_pl011_read(AARCH64_UARTFR) & AARCH64_UARTFR_BUSY)
        ;
    aarch64_pl011_write(AARCH64_UARTLCRH, 0);
    aarch64_pl011_write(AARCH64_UARTICR, 0x7FF);

    /* 115200 baud with 24 MHz reference clock */
    aarch64_pl011_write(AARCH64_UARTIBRD, 13);
    aarch64_pl011_write(AARCH64_UARTFBRD, 1);

    aarch64_pl011_write(AARCH64_UARTLCRH,
                        AARCH64_UARTLCRH_WLEN8 | AARCH64_UARTLCRH_FEN);
    aarch64_pl011_write(AARCH64_UARTIMSC, 0);
    aarch64_pl011_write(AARCH64_UARTCR,
                        AARCH64_UARTCR_UARTEN | AARCH64_UARTCR_TXE | AARCH64_UARTCR_RXE);
}

static void driver_pl011_aarch64_putc(char c)
{
    if (c == '\n') {
        while (aarch64_pl011_read(AARCH64_UARTFR) & AARCH64_UARTFR_TXFF)
            ;
        aarch64_pl011_write(AARCH64_UARTDR, '\r');
    }
    while (aarch64_pl011_read(AARCH64_UARTFR) & AARCH64_UARTFR_TXFF)
        ;
    aarch64_pl011_write(AARCH64_UARTDR, (uint32_t)(unsigned char)c);
}

static int driver_pl011_aarch64_getc(void)
{
    while (aarch64_pl011_read(AARCH64_UARTFR) & AARCH64_UARTFR_RXFE)
        ;
    return (int)(aarch64_pl011_read(AARCH64_UARTDR) & 0xFF);
}

const struct uart_driver uart_driver_pl011_aarch64 = {
    .type           = UART_TYPE_PL011,
    .init           = driver_pl011_aarch64_init,
    .putc           = driver_pl011_aarch64_putc,
    .getc           = driver_pl011_aarch64_getc,
    .getc_nonblock  = NULL,
    .puts           = NULL,
    .vprintf        = NULL,
    .enable_rx_irq  = NULL,
    .disable_rx_irq = NULL,
    .clear_irq      = NULL,
    .tx_ready       = NULL,
    .rx_ready       = NULL,
};

#endif /* AArch64 */

/* =======================================================================
 * RISC-V: 8250-compatible MMIO driver
 * ======================================================================= */
#if ARCH_RISCV64

#ifndef RISCV_UART_BASE
#define RISCV_UART_BASE 0x10000000UL
#endif

#define RISCV_UART_THR   0
#define RISCV_UART_IER   1
#define RISCV_UART_FCR   2
#define RISCV_UART_LCR   3
#define RISCV_UART_MCR   4
#define RISCV_UART_LSR   5

#define RISCV_UART_LSR_DR    (1u << 0)
#define RISCV_UART_LSR_THRE  (1u << 5)

#define RISCV_UART_FCR_DEFAULT (0x07)

static inline void riscv_uart_write(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(uintptr_t)(RISCV_UART_BASE + off) = val;
}

static inline uint8_t riscv_uart_read(uint32_t off)
{
    return *(volatile uint8_t *)(uintptr_t)(RISCV_UART_BASE + off);
}

static void driver_8250_mmio_init(uint32_t baud_rate)
{
    (void)baud_rate;

    riscv_uart_write(RISCV_UART_IER, 0);
    riscv_uart_write(RISCV_UART_LCR, 0x80); /* DLAB */
    riscv_uart_write(RISCV_UART_THR, 20);   /* DLL: 36864000/(16*115200)=20 */
    riscv_uart_write(RISCV_UART_IER, 0);    /* DLM */
    riscv_uart_write(RISCV_UART_LCR, 0x03); /* 8N1, DLAB=0 */
    riscv_uart_write(RISCV_UART_FCR, RISCV_UART_FCR_DEFAULT);
    riscv_uart_write(RISCV_UART_MCR, 0x03); /* RTS + DTR */
}

static void driver_8250_mmio_putc(char c)
{
    if (c == '\n') {
        while (!(riscv_uart_read(RISCV_UART_LSR) & RISCV_UART_LSR_THRE))
            ;
        riscv_uart_write(RISCV_UART_THR, '\r');
    }
    while (!(riscv_uart_read(RISCV_UART_LSR) & RISCV_UART_LSR_THRE))
        ;
    riscv_uart_write(RISCV_UART_THR, (uint8_t)c);
}

static int driver_8250_mmio_getc(void)
{
    while (!(riscv_uart_read(RISCV_UART_LSR) & RISCV_UART_LSR_DR))
        ;
    return (int)riscv_uart_read(RISCV_UART_THR);
}

static int driver_8250_mmio_getc_nonblock(void)
{
    if (!(riscv_uart_read(RISCV_UART_LSR) & RISCV_UART_LSR_DR))
        return -1;
    return (int)riscv_uart_read(RISCV_UART_THR);
}

static int driver_8250_mmio_tx_ready(void)
{
    return (riscv_uart_read(RISCV_UART_LSR) & RISCV_UART_LSR_THRE) != 0;
}

static int driver_8250_mmio_rx_ready(void)
{
    return (riscv_uart_read(RISCV_UART_LSR) & RISCV_UART_LSR_DR) != 0;
}

const struct uart_driver uart_driver_8250_mmio = {
    .type           = UART_TYPE_8250_MMIO,
    .init           = driver_8250_mmio_init,
    .putc           = driver_8250_mmio_putc,
    .getc           = driver_8250_mmio_getc,
    .getc_nonblock  = driver_8250_mmio_getc_nonblock,
    .puts           = NULL,
    .vprintf        = NULL,
    .enable_rx_irq  = NULL,
    .disable_rx_irq = NULL,
    .clear_irq      = NULL,
    .tx_ready       = driver_8250_mmio_tx_ready,
    .rx_ready       = driver_8250_mmio_rx_ready,
};

#endif /* RISC-V */

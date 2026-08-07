/*
 * uart.c - RISC-V 64 8250-compatible UART driver for Fern
 *
 * Targets QEMU -machine virt (base 0x10000000).
 * Simple 8250/16550-style MMIO UART with polling (no interrupts used).
 *
 * All functions are safe to call from early boot (before heap/MMU/interrupts).
 * All MMIO accesses use volatile pointers to prevent compiler reordering.
 *
 * Baud rate divisor (8250 TRM):
 *   DLAB=1 → write divisor to THR/IER registers (now DLL/DLM)
 *   Divisor = UARTclk / (16 × baud_rate)
 *   UARTclk = 36.864 MHz on QEMU virt
 *   For 115200: Divisor = 36864000 / (16 × 115200) = 20
 */

#include "uart.h"
#include <stdint.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * MMIO helpers
 * --------------------------------------------------------------------- */
static inline void     mmio_write(uint64_t addr, uint8_t val) {
    *(volatile uint8_t *)addr = val;
}

static inline uint8_t  mmio_read(uint64_t addr) {
    return *(volatile uint8_t *)addr;
}

/* Register address from offset */
#define UART_REG(off)  ((uint64_t)(UART_BASE_ADDR + (off)))

/* -----------------------------------------------------------------------
 * riscv64_uart_init()
 *
 * Initialise 8250 UART for 8N1, FIFO enabled, polled mode.
 * UARTclk = 36.864 MHz on QEMU virt.
 * --------------------------------------------------------------------- */
void riscv64_uart_init(void)
{
    /* Disable all interrupts */
    mmio_write(UART_REG(UART_REG_IER), 0);

    /* Set DLAB to access divisor latch */
    mmio_write(UART_REG(UART_REG_LCR), UART_LCR_DLAB);

    /* Set baud rate divisor: 36864000 / (16 × 115200) = 20
     * DLL = 20 (low byte), DLM = 0 (high byte) */
    mmio_write(UART_REG(UART_REG_THR), 20);   /* DLL */
    mmio_write(UART_REG(UART_REG_IER), 0);    /* DLM */

    /* 8 data bits, no parity, 1 stop bit (clear DLAB) */
    mmio_write(UART_REG(UART_REG_LCR), UART_LCR_8N1);

    /* Enable and reset FIFOs */
    mmio_write(UART_REG(UART_REG_FCR), UART_FCR_DEFAULT);

    /* Set RTS/DTR (modem control) */
    mmio_write(UART_REG(UART_REG_MCR), UART_MCR_RTS | UART_MCR_DTR);
}

/* -----------------------------------------------------------------------
 * riscv64_uart_tx_ready() / riscv64_uart_rx_ready()
 * --------------------------------------------------------------------- */
int riscv64_uart_tx_ready(void)
{
    return (mmio_read(UART_REG(UART_REG_LSR)) & UART_LSR_THRE) != 0;
}

int riscv64_uart_rx_ready(void)
{
    return (mmio_read(UART_REG(UART_REG_LSR)) & UART_LSR_DR) != 0;
}

/* -----------------------------------------------------------------------
 * riscv64_uart_putc() – blocking single-character transmit
 * --------------------------------------------------------------------- */
void riscv64_uart_putc(char c)
{
    /* Spin until Transmitter Holding Register is empty */
    while (!(mmio_read(UART_REG(UART_REG_LSR)) & UART_LSR_THRE))
        ;
    mmio_write(UART_REG(UART_REG_THR), (uint8_t)c);
}

/* -----------------------------------------------------------------------
 * riscv64_uart_getc() – blocking single-character receive
 * --------------------------------------------------------------------- */
char riscv64_uart_getc(void)
{
    /* Spin until Data Ready bit set in LSR */
    while (!(mmio_read(UART_REG(UART_REG_LSR)) & UART_LSR_DR))
        ;
    return (char)mmio_read(UART_REG(UART_REG_THR));
}

/* -----------------------------------------------------------------------
 * riscv64_uart_getc_nonblock() – non-blocking receive
 * --------------------------------------------------------------------- */
int riscv64_uart_getc_nonblock(void)
{
    if (!(mmio_read(UART_REG(UART_REG_LSR)) & UART_LSR_DR))
        return -1;
    return (int)mmio_read(UART_REG(UART_REG_THR));
}

/* -----------------------------------------------------------------------
 * riscv64_uart_puts() – NUL-terminated string, '\n' -> "\r\n"
 * --------------------------------------------------------------------- */
void riscv64_uart_puts(const char *str)
{
    while (*str) {
        if (*str == '\n')
            riscv64_uart_putc('\r');
        riscv64_uart_putc(*str++);
    }
}

/* -----------------------------------------------------------------------
 * riscv64_uart_printf() / riscv64_uart_vprintf()
 *
 * Minimal printf for early boot / debug.  No heap required.
 * Supports: %c  %s  %d  %u  %x  %X  %p  %%
 * Width and zero-padding supported for integers (e.g. %08x).
 * 64-bit unsigned support via %lu / %lx (or %u / %x truncated to 32-bit).
 * --------------------------------------------------------------------- */

/* Helper: print 64-bit unsigned in given base, zero-padded to min_width */
static void print_uint64(uint64_t n, uint32_t base, int upcase,
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
        riscv64_uart_putc(buf[i]);
}

/* Helper: print signed 64-bit integer */
static void print_int64(int64_t n, int min_width, char pad_char)
{
    if (n < 0) {
        riscv64_uart_putc('-');
        print_uint64((uint64_t)(-(int64_t)n), 10, 0, min_width - 1, pad_char);
    } else {
        print_uint64((uint64_t)n, 10, 0, min_width, pad_char);
    }
}

void riscv64_uart_vprintf(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') riscv64_uart_putc('\r');
            riscv64_uart_putc(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        /* Parse optional flags/width */
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
            riscv64_uart_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            riscv64_uart_puts(s);
            break;
        }
        case 'd':
        case 'i':
            print_int64(va_arg(ap, int64_t), min_width, pad_char);
            break;
        case 'u':
            print_uint64(va_arg(ap, uint64_t), 10, 0, min_width, pad_char);
            break;
        case 'x':
            print_uint64(va_arg(ap, uint64_t), 16, 0, min_width, pad_char);
            break;
        case 'X':
            print_uint64(va_arg(ap, uint64_t), 16, 1, min_width, pad_char);
            break;
        case 'p':
            riscv64_uart_puts("0x");
            print_uint64((uint64_t)va_arg(ap, void *), 16, 0, 16, '0');
            break;
        case '%':
            riscv64_uart_putc('%');
            break;
        default:
            riscv64_uart_putc('?');
            break;
        }
    }
}

void riscv64_uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    riscv64_uart_vprintf(fmt, ap);
    va_end(ap);
}

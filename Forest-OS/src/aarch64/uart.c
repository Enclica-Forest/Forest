/*
 * Fern - AArch64 PL011 UART driver
 *
 * Polled transmit/receive for early console output.
 * Baud-rate divisor calculation:
 *   Reference clock (UARTCLK) = 24,000,000 Hz (QEMU virt)
 *   Baud = 115200
 *   Divisor = UARTCLK / (16 × Baud) = 24000000 / 1843200 ≈ 13.0208...
 *   IBRD = 13
 *   FBRD = round(0.0208... × 64) = round(1.333...) = 1
 */

#include "uart.h"
#include <stdint.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                         */
/* ------------------------------------------------------------------ */

static inline void pl011_write(uint32_t offset, uint32_t val)
{
    *((volatile uint32_t *)(uintptr_t)(PL011_BASE + offset)) = val;
}

static inline uint32_t pl011_read(uint32_t offset)
{
    return *((volatile uint32_t *)(uintptr_t)(PL011_BASE + offset));
}

/* ------------------------------------------------------------------ */
/* uart_init                                                            */
/* ------------------------------------------------------------------ */
void uart_init(void)
{
    /* 1. Disable the UART */
    pl011_write(UARTCR, 0);

    /* 2. Wait for any ongoing transmission to finish */
    while (pl011_read(UARTFR) & UARTFR_BUSY)
        ;

    /* 3. Flush the transmit FIFO by disabling it */
    pl011_write(UARTLCRH, 0);

    /* 4. Clear all pending interrupts */
    pl011_write(UARTICR, 0x7FF);

    /* 5. Set baud rate: 115200 with 24 MHz reference clock */
    pl011_write(UARTIBRD, 13);   /* Integer divisor  */
    pl011_write(UARTFBRD,  1);   /* Fractional div   */

    /* 6. Configure 8N1, FIFO enabled */
    pl011_write(UARTLCRH, UARTLCRH_WLEN_8 | UARTLCRH_FEN);

    /* 7. Mask all interrupts (polled mode) */
    pl011_write(UARTIMSC, 0);

    /* 8. Enable UART with TX and RX */
    pl011_write(UARTCR, UARTCR_UARTEN | UARTCR_TXE | UARTCR_RXE);
}

/* ------------------------------------------------------------------ */
/* uart_putc                                                            */
/* ------------------------------------------------------------------ */
void uart_putc(char c)
{
    /* Block while TX FIFO is full */
    while (pl011_read(UARTFR) & UARTFR_TXFF)
        ;
    pl011_write(UARTDR, (uint32_t)(unsigned char)c);
}

/* ------------------------------------------------------------------ */
/* uart_getc                                                            */
/* ------------------------------------------------------------------ */
char uart_getc(void)
{
    while (pl011_read(UARTFR) & UARTFR_RXFE)
        ;
    return (char)(pl011_read(UARTDR) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* uart_puts                                                            */
/* ------------------------------------------------------------------ */
void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

/* ------------------------------------------------------------------ */
/* uart_printf - minimal formatted output                               */
/* ------------------------------------------------------------------ */

static void uart_put_uint(uint64_t n, int base, int uppercase)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;
    char buf[20];
    int i = 0;

    if (n == 0) {
        uart_putc('0');
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % (uint64_t)base];
        n /= (uint64_t)base;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

static void uart_put_int(int64_t n)
{
    if (n < 0) {
        uart_putc('-');
        uart_put_uint((uint64_t)(-n), 10, 0);
    } else {
        uart_put_uint((uint64_t)n, 10, 0);
    }
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') uart_putc('\r');
            uart_putc(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        switch (*fmt) {
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            uart_puts(s);
            break;
        }
        case 'd':
            uart_put_int((int64_t)va_arg(ap, int));
            break;
        case 'u':
            uart_put_uint((uint64_t)va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            uart_put_uint((uint64_t)va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            uart_put_uint((uint64_t)va_arg(ap, unsigned int), 16, 1);
            break;
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            uart_puts("0x");
            uart_put_uint(v, 16, 0);
            break;
        }
        case 'l':
            /* Handle %ld, %lu, %lx */
            fmt++;
            switch (*fmt) {
            case 'd':
                uart_put_int(va_arg(ap, long));
                break;
            case 'u':
                uart_put_uint(va_arg(ap, unsigned long), 10, 0);
                break;
            case 'x':
                uart_put_uint(va_arg(ap, unsigned long), 16, 0);
                break;
            default:
                uart_putc('l');
                uart_putc(*fmt);
                break;
            }
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
        fmt++;
    }

    va_end(ap);
}

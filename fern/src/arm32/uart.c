/*
 * uart.c - ARM PL011 UART driver for Fern ARM32
 *
 * Targets QEMU -machine virt (UARTCLK = 24 MHz, base 0x09000000).
 * Override UART_BASE_ADDR at compile time for other boards:
 *   -DUART_BASE_ADDR=0x3F201000   (Raspberry Pi 3)
 *
 * All public functions are safe to call from early boot (before the heap
 * is initialised and before interrupts are enabled).
 *
 * All MMIO accesses use volatile pointers so the compiler never
 * optimises them away.
 *
 * Baud rate formula (PL011 TRM §3.3.6):
 *   BRD     = UARTCLK / (16 x baud_rate)
 *   IBRD    = floor(BRD)
 *   FBRD    = round((BRD - IBRD) x 64)
 *
 * Integer implementation (avoids floating-point):
 *   BRD_x64 = (UARTCLK x 4 + baud_rate/2) / baud_rate   [round-to-nearest]
 *   IBRD    = BRD_x64 >> 6
 *   FBRD    = BRD_x64 & 0x3F
 *
 * Example – 115200 bps @ 24 MHz:
 *   BRD_x64 = (96000000 + 57600) / 115200 = 834
 *   IBRD    = 834 >> 6 = 13
 *   FBRD    = 834 & 0x3F = 2
 */

#include "uart.h"
#include <stdint.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * MMIO helpers
 * --------------------------------------------------------------------- */
static inline void     mmio_write(uint32_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_read(uint32_t addr) {
    return *(volatile uint32_t *)addr;
}

/* Convenience: register address from offset */
#define PL011_REG(off)  ((uint32_t)(UART_BASE_ADDR + (off)))

/* -----------------------------------------------------------------------
 * uart_init()
 *
 * Initialise the PL011 for 8N1, FIFO enabled, polled (no interrupts).
 * See the file-level comment for the baud rate formula.
 * --------------------------------------------------------------------- */
void uart_init(uint32_t baud_rate)
{
    /* 1. Disable UART before reconfiguring */
    mmio_write(PL011_REG(PL011_CR), 0);

    /* 2. Wait until any in-progress transmission completes (BUSY = 0).
     *    Required by the PL011 TRM before changing IBRD/FBRD/LCRH.
     */
    while (mmio_read(PL011_REG(PL011_FR)) & PL011_FR_BUSY)
        ;

    /* 3. Flush the RX/TX FIFOs: disable FIFO while UART is off */
    mmio_write(PL011_REG(PL011_LCRH), 0);

    /* 4. Clear any pending interrupts */
    mmio_write(PL011_REG(PL011_ICR), PL011_INT_ALL);

    /* 5. Calculate baud rate divisors.
     *
     * UARTCLK = 24 MHz; use integer arithmetic with rounding:
     *   BRD × 64 = (UARTCLK × 4 + baud_rate/2) / baud_rate
     *   IBRD     = BRD_x64 >> 6
     *   FBRD     = BRD_x64 & 0x3F
     *
     * Adding (baud_rate / 2) before division implements round-to-nearest.
     */
    uint32_t uartclk = 24000000UL;
    uint32_t combined = (uartclk * 4u + baud_rate / 2u) / baud_rate; /* BRD × 64, rounded */
    uint32_t ibrd = combined >> 6;
    uint32_t fbrd = combined & 0x3Fu;

    mmio_write(PL011_REG(PL011_IBRD), ibrd);
    mmio_write(PL011_REG(PL011_FBRD), fbrd);

    /* Line control: 8-bit, no parity, 1 stop bit, FIFO enabled */
    mmio_write(PL011_REG(PL011_LCRH),
               PL011_LCRH_WLEN8 | PL011_LCRH_FEN);

    /* Mask all interrupts */
    mmio_write(PL011_REG(PL011_IMSC), 0);

    /* Enable UART: TX + RX + UART enable */
    mmio_write(PL011_REG(PL011_CR),
               PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
}

/* -----------------------------------------------------------------------
 * uart_tx_ready() / uart_rx_ready()
 * --------------------------------------------------------------------- */
int uart_tx_ready(void)
{
    return !(mmio_read(PL011_REG(PL011_FR)) & PL011_FR_TXFF);
}

int uart_rx_ready(void)
{
    return !(mmio_read(PL011_REG(PL011_FR)) & PL011_FR_RXFE);
}

/* -----------------------------------------------------------------------
 * uart_putc() – blocking single-character transmit
 * --------------------------------------------------------------------- */
void uart_putc(char c)
{
    /* Spin until TX FIFO has space */
    while (mmio_read(PL011_REG(PL011_FR)) & PL011_FR_TXFF)
        ;
    mmio_write(PL011_REG(PL011_DR), (uint32_t)(unsigned char)c);
}

/* -----------------------------------------------------------------------
 * uart_getc() – blocking single-character receive
 * --------------------------------------------------------------------- */
char uart_getc(void)
{
    /* Spin until RX FIFO has data */
    while (mmio_read(PL011_REG(PL011_FR)) & PL011_FR_RXFE)
        ;
    return (char)(mmio_read(PL011_REG(PL011_DR)) & 0xFF);
}

/* -----------------------------------------------------------------------
 * uart_getc_nonblock() – non-blocking receive
 * --------------------------------------------------------------------- */
int uart_getc_nonblock(void)
{
    if (mmio_read(PL011_REG(PL011_FR)) & PL011_FR_RXFE)
        return -1;
    return (int)(mmio_read(PL011_REG(PL011_DR)) & 0xFF);
}

/* -----------------------------------------------------------------------
 * uart_puts() – NUL-terminated string, '\n' -> "\r\n"
 * --------------------------------------------------------------------- */
void uart_puts(const char *str)
{
    while (*str) {
        if (*str == '\n')
            uart_putc('\r');
        uart_putc(*str++);
    }
}

/* -----------------------------------------------------------------------
 * uart_enable_rx_irq() / uart_disable_rx_irq() / uart_clear_irq()
 * --------------------------------------------------------------------- */
void uart_enable_rx_irq(void)
{
    uint32_t imsc = mmio_read(PL011_REG(PL011_IMSC));
    imsc |= PL011_INT_RXI | PL011_INT_RTI;
    mmio_write(PL011_REG(PL011_IMSC), imsc);
}

void uart_disable_rx_irq(void)
{
    uint32_t imsc = mmio_read(PL011_REG(PL011_IMSC));
    imsc &= ~(PL011_INT_RXI | PL011_INT_RTI);
    mmio_write(PL011_REG(PL011_IMSC), imsc);
}

void uart_clear_irq(void)
{
    mmio_write(PL011_REG(PL011_ICR), PL011_INT_ALL);
}

/* -----------------------------------------------------------------------
 * uart_printf() / uart_vprintf()
 *
 * Minimal printf for early boot / debug.  No heap required.
 * Supports: %c  %s  %d  %u  %x  %X  %p  %%
 * Width and zero-padding supported for integers (e.g. %08x).
 * --------------------------------------------------------------------- */

/* Helper: print unsigned in given base, zero-padded to min_width digits */
static void print_uint(uint32_t n, uint32_t base, int upcase,
                       int min_width, char pad_char)
{
    const char *digits_lo = "0123456789abcdef";
    const char *digits_hi = "0123456789ABCDEF";
    const char *digits = upcase ? digits_hi : digits_lo;
    char buf[32];
    int  pos = 0;

    if (n == 0) {
        buf[pos++] = '0';
    } else {
        while (n) {
            buf[pos++] = digits[n % base];
            n /= base;
        }
    }

    /* Pad to minimum width */
    while (pos < min_width)
        buf[pos++] = pad_char;

    /* Reverse and emit */
    for (int i = pos - 1; i >= 0; --i)
        uart_putc(buf[i]);
}

static void print_int(int32_t n, int min_width, char pad_char)
{
    if (n < 0) {
        uart_putc('-');
        print_uint((uint32_t)(-(uint32_t)n), 10, 0, min_width - 1, pad_char);
    } else {
        print_uint((uint32_t)n, 10, 0, min_width, pad_char);
    }
}

void uart_vprintf(const char *fmt, va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') uart_putc('\r');
            uart_putc(*fmt++);
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
            uart_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            uart_puts(s);
            break;
        }
        case 'd':
        case 'i':
            print_int(va_arg(ap, int32_t), min_width, pad_char);
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
            uart_puts("0x");
            print_uint((uint32_t)va_arg(ap, void *), 16, 0, 8, '0');
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('?');
            break;
        }
    }
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
}

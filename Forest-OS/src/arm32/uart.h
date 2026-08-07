/*
 * uart.h - PL011 UART driver interface for Fern ARM32
 *
 * The ARM PrimeCell PL011 is the standard UART on:
 *   - QEMU  -machine virt   (base 0x09000000)
 *   - Raspberry Pi 3/4      (base 0x3F201000 / 0xFE201000)
 *
 * The base address is selected at compile time via UART_BASE_ADDR.
 * Default (QEMU virt) is 0x09000000.
 */

#ifndef ARM32_UART_H
#define ARM32_UART_H

#include <stdint.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * PL011 hardware base address
 *
 * Override at build time:
 *   -DUART_BASE_ADDR=0x3F201000   (Raspberry Pi 3)
 * --------------------------------------------------------------------- */
#ifndef UART_BASE_ADDR
#define UART_BASE_ADDR  0x09000000UL
#endif

/* -----------------------------------------------------------------------
 * PL011 register offsets (byte offsets from UART_BASE_ADDR)
 * --------------------------------------------------------------------- */
#define PL011_DR        0x000   /* Data Register (TX/RX)                */
#define PL011_RSR       0x004   /* Receive Status Register              */
#define PL011_FR        0x018   /* Flag Register                        */
#define PL011_ILPR      0x020   /* IrDA Low-Power Counter Register      */
#define PL011_IBRD      0x024   /* Integer Baud Rate Divisor            */
#define PL011_FBRD      0x028   /* Fractional Baud Rate Divisor         */
#define PL011_LCRH      0x02C   /* Line Control Register                */
#define PL011_CR        0x030   /* Control Register                     */
#define PL011_IFLS      0x034   /* Interrupt FIFO Level Select          */
#define PL011_IMSC      0x038   /* Interrupt Mask Set/Clear             */
#define PL011_RIS       0x03C   /* Raw Interrupt Status                 */
#define PL011_MIS       0x040   /* Masked Interrupt Status              */
#define PL011_ICR       0x044   /* Interrupt Clear Register             */
#define PL011_DMACR     0x048   /* DMA Control Register                 */

/* -----------------------------------------------------------------------
 * Flag Register (FR) bit definitions
 * --------------------------------------------------------------------- */
#define PL011_FR_CTS    (1u << 0)   /* Clear To Send                   */
#define PL011_FR_DSR    (1u << 1)   /* Data Set Ready                  */
#define PL011_FR_DCD    (1u << 2)   /* Data Carrier Detect             */
#define PL011_FR_BUSY   (1u << 3)   /* UART busy transmitting          */
#define PL011_FR_RXFE   (1u << 4)   /* Receive  FIFO empty             */
#define PL011_FR_TXFF   (1u << 5)   /* Transmit FIFO full              */
#define PL011_FR_RXFF   (1u << 6)   /* Receive  FIFO full              */
#define PL011_FR_TXFE   (1u << 7)   /* Transmit FIFO empty             */

/* -----------------------------------------------------------------------
 * Line Control Register (LCRH) bit definitions
 * --------------------------------------------------------------------- */
#define PL011_LCRH_BRK      (1u << 0)   /* Send break                  */
#define PL011_LCRH_PEN      (1u << 1)   /* Parity enable               */
#define PL011_LCRH_EPS      (1u << 2)   /* Even parity select          */
#define PL011_LCRH_STP2     (1u << 3)   /* Two stop bits               */
#define PL011_LCRH_FEN      (1u << 4)   /* FIFO enable                 */
#define PL011_LCRH_WLEN5    (0u << 5)   /* Word length: 5 bits         */
#define PL011_LCRH_WLEN6    (1u << 5)   /* Word length: 6 bits         */
#define PL011_LCRH_WLEN7    (2u << 5)   /* Word length: 7 bits         */
#define PL011_LCRH_WLEN8    (3u << 5)   /* Word length: 8 bits         */
#define PL011_LCRH_SPS      (1u << 7)   /* Sticky parity select        */

/* -----------------------------------------------------------------------
 * Control Register (CR) bit definitions
 * --------------------------------------------------------------------- */
#define PL011_CR_UARTEN (1u << 0)   /* UART enable                     */
#define PL011_CR_SIREN  (1u << 1)   /* SIR enable                      */
#define PL011_CR_SIRLP  (1u << 2)   /* IrDA SIR low-power mode         */
#define PL011_CR_LBE    (1u << 7)   /* Loop-back enable                */
#define PL011_CR_TXE    (1u << 8)   /* Transmit enable                 */
#define PL011_CR_RXE    (1u << 9)   /* Receive enable                  */
#define PL011_CR_DTR    (1u << 10)  /* Data Transmit Ready             */
#define PL011_CR_RTS    (1u << 11)  /* Request To Send                 */
#define PL011_CR_OUT1   (1u << 12)  /* Complement of nUARTOut1         */
#define PL011_CR_OUT2   (1u << 13)  /* Complement of nUARTOut2         */
#define PL011_CR_RTSEN  (1u << 14)  /* RTS hardware flow control       */
#define PL011_CR_CTSEN  (1u << 15)  /* CTS hardware flow control       */

/* -----------------------------------------------------------------------
 * Interrupt bit masks (IMSC / ICR)
 * --------------------------------------------------------------------- */
#define PL011_INT_OEI   (1u << 10)  /* Overrun error interrupt         */
#define PL011_INT_BEI   (1u << 9)   /* Break error interrupt           */
#define PL011_INT_PEI   (1u << 8)   /* Parity error interrupt          */
#define PL011_INT_FEI   (1u << 7)   /* Framing error interrupt         */
#define PL011_INT_RTI   (1u << 6)   /* Receive timeout interrupt       */
#define PL011_INT_TXI   (1u << 5)   /* Transmit interrupt              */
#define PL011_INT_RXI   (1u << 4)   /* Receive interrupt               */
#define PL011_INT_ALL   0x7FFu      /* All interrupt bits              */

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/**
 * uart_init() - Initialise the PL011 UART.
 *
 * @baud_rate: Desired baud rate (e.g. 115200).  The function calculates
 *             IBRD/FBRD from the UART reference clock (UARTCLK = 24 MHz
 *             on QEMU virt).
 *
 * Configures: 8 data bits, no parity, 1 stop bit, FIFO enabled.
 * All interrupts are masked; call uart_enable_rx_irq() to enable RX.
 */
void uart_init(uint32_t baud_rate);

/**
 * uart_putc() - Transmit one character (blocking).
 *
 * Spins until the TX FIFO has space, then writes @c to the data register.
 *
 * @c: Character to transmit.
 */
void uart_putc(char c);

/**
 * uart_getc() - Receive one character (blocking).
 *
 * Spins until the RX FIFO is non-empty, then reads and returns the byte.
 *
 * @return: Received character.
 */
char uart_getc(void);

/**
 * uart_getc_nonblock() - Non-blocking receive.
 *
 * @return: Received character, or -1 if the RX FIFO is empty.
 */
int  uart_getc_nonblock(void);

/**
 * uart_puts() - Transmit a NUL-terminated string.
 *
 * Calls uart_putc() for each character.  '\n' is translated to "\r\n"
 * for terminal compatibility.
 *
 * @str: NUL-terminated string to transmit.
 */
void uart_puts(const char *str);

/**
 * uart_printf() - Formatted output to UART.
 *
 * Minimal printf implementation supporting:
 *   %c  %s  %d  %u  %x  %X  %p  %%
 *
 * Intended for early boot / debug output before the heap is available.
 *
 * @fmt: printf-style format string.
 */
void uart_printf(const char *fmt, ...);

/**
 * uart_vprintf() - va_list variant of uart_printf().
 */
void uart_vprintf(const char *fmt, va_list ap);

/**
 * uart_enable_rx_irq() - Enable the RX interrupt in IMSC.
 *
 * After calling this, an IRQ fires whenever the RX FIFO passes the
 * trigger level.  The GIC must also have the UART IRQ enabled.
 */
void uart_enable_rx_irq(void);

/**
 * uart_disable_rx_irq() - Disable the RX interrupt.
 */
void uart_disable_rx_irq(void);

/**
 * uart_clear_irq() - Acknowledge / clear all pending UART interrupts.
 *
 * Call from the UART IRQ handler before reading received data.
 */
void uart_clear_irq(void);

/**
 * uart_tx_ready() - Check if TX FIFO has space.
 *
 * @return: Non-zero if at least one byte can be written without blocking.
 */
int  uart_tx_ready(void);

/**
 * uart_rx_ready() - Check if RX FIFO has data.
 *
 * @return: Non-zero if at least one byte is available to read.
 */
int  uart_rx_ready(void);

#endif /* ARM32_UART_H */

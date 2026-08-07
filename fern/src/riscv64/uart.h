/*
 * Fern - RISC-V 64 UART driver (8250-compatible MMIO serial)
 *
 * Target: QEMU -machine virt   (base 0x10000000)
 *
 * The QEMU virt machine exposes an 8250-compatible UART at 0x10000000.
 * Register layout (byte offsets from base):
 *   THR  0  Transmitter Holding Register  (write)
 *   IER  1  Interrupt Enable Register
 *   FCR  2  FIFO Control Register         (write)
 *   LCR  3  Line Control Register
 *   MCR  4  Modem Control Register
 *   LSR  5  Line Status Register          (read)
 *   MSR  6  Modem Status Register         (read)
 *   SCR  7  Scratch Register
 *
 * LSR bits:
 *   Bit 0 – Data Ready (RX FIFO non-empty)
 *   Bit 5 – THR Empty  (TX FIFO has space)
 */

#ifndef RISCV64_UART_H
#define RISCV64_UART_H

#include <stdint.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * Hardware base address
 *
 * Override at build time:
 *   -DUART_BASE_ADDR=0x10000000   (QEMU virt, default)
 * --------------------------------------------------------------------- */
#ifndef UART_BASE_ADDR
#define UART_BASE_ADDR  0x10000000UL
#endif

/* -----------------------------------------------------------------------
 * 8250-compatible register offsets
 * --------------------------------------------------------------------- */
#define UART_REG_THR    0   /* Transmitter Holding Register (write)        */
#define UART_REG_IER    1   /* Interrupt Enable Register                   */
#define UART_REG_FCR    2   /* FIFO Control Register (write)               */
#define UART_REG_LCR    3   /* Line Control Register                       */
#define UART_REG_MCR    4   /* Modem Control Register                      */
#define UART_REG_LSR    5   /* Line Status Register (read)                 */
#define UART_REG_MSR    6   /* Modem Status Register (read)                */
#define UART_REG_SCR    7   /* Scratch Register                            */

/* -----------------------------------------------------------------------
 * LSR (Line Status Register) bits
 * --------------------------------------------------------------------- */
#define UART_LSR_DR     (1u << 0)   /* Data Ready (RX FIFO non-empty)    */
#define UART_LSR_OE     (1u << 1)   /* Overrun Error                     */
#define UART_LSR_PE     (1u << 2)   /* Parity Error                      */
#define UART_LSR_FE     (1u << 3)   /* Framing Error                     */
#define UART_LSR_BI     (1u << 4)   /* Break Interrupt                   */
#define UART_LSR_THRE   (1u << 5)   /* Transmitter Holding Register Empty */
#define UART_LSR_TEMT   (1u << 6)   /* Transmitter Empty                 */
#define UART_LSR_FIFOERR (1u << 7)  /* FIFO Error                        */

/* -----------------------------------------------------------------------
 * IER (Interrupt Enable Register) bits
 * --------------------------------------------------------------------- */
#define UART_IER_ERDAI  (1u << 0)   /* Enable Received Data Available    */
#define UART_IER_ETBEI  (1u << 1)   /* Enable Transmitter Holding Empty  */
#define UART_IER_ELSI   (1u << 2)   /* Enable Receiver Line Status       */
#define UART_IER_EDSSI  (1u << 3)   /* Enable Modem Status               */

/* -----------------------------------------------------------------------
 * FCR (FIFO Control Register) bits
 * --------------------------------------------------------------------- */
#define UART_FCR_FIFOE  (1u << 0)   /* FIFO Enable                       */
#define UART_FCR_RFIFOR (1u << 1)   /* Receiver FIFO Reset               */
#define UART_FCR_XFIFOR (1u << 2)   /* Transmitter FIFO Reset            */
#define UART_FCR_TFT0   (1u << 3)   /* Trigger Level bit 0               */
#define UART_FCR_TFT1   (1u << 4)   /* Trigger Level bit 1               */
#define UART_FCR_RT0    (1u << 6)   /* Receiver Trigger Level bit 0      */
#define UART_FCR_RT1    (1u << 7)   /* Receiver Trigger Level bit 1      */

/* Convenience: FIFO enable + 16-byte trigger levels */
#define UART_FCR_DEFAULT (UART_FCR_FIFOE | UART_FCR_RFIFOR | UART_FCR_XFIFOR)

/* -----------------------------------------------------------------------
 * LCR (Line Control Register) bits
 * --------------------------------------------------------------------- */
#define UART_LCR_WLS0   (1u << 0)   /* Word Length Select bit 0          */
#define UART_LCR_WLS1   (1u << 1)   /* Word Length Select bit 1          */
#define UART_LCR_STB    (1u << 2)   /* Number of Stop Bits               */
#define UART_LCR_PEN    (1u << 3)   /* Parity Enable                     */
#define UART_LCR_EPS    (1u << 4)   /* Even Parity Select                */
#define UART_LCR_SPS    (1u << 5)   /* Stick Parity Select               */
#define UART_LCR_DLAB   (1u << 7)   /* Divisor Latch Access Bit          */

/* 8-N-1 */
#define UART_LCR_8N1    (UART_LCR_WLS0 | UART_LCR_WLS1)

/* -----------------------------------------------------------------------
 * MCR (Modem Control Register) bits
 * --------------------------------------------------------------------- */
#define UART_MCR_DTR    (1u << 0)   /* Data Terminal Ready               */
#define UART_MCR_RTS    (1u << 1)   /* Request To Send                   */
#define UART_MCR_OUT1   (1u << 2)   /* Out 1                             */
#define UART_MCR_OUT2   (1u << 3)   /* Out 2                             */
#define UART_MCR_LOOP   (1u << 4)   /* Loopback Mode                     */

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/**
 * riscv64_uart_init() - Initialise the 8250 UART for 8N1, polled mode.
 *
 * Sets baud rate divisor (115200 default), enables FIFO, disables
 * interrupts.  Safe to call before MMU or interrupts are configured.
 */
void riscv64_uart_init(void);

/**
 * riscv64_uart_putc() - Transmit one character (blocking).
 *
 * @c: Character to transmit.
 */
void riscv64_uart_putc(char c);

/**
 * riscv64_uart_puts() - Transmit a NUL-terminated string.
 *
 * '\n' is translated to "\r\n" for terminal compatibility.
 *
 * @str: NUL-terminated string.
 */
void riscv64_uart_puts(const char *str);

/**
 * riscv64_uart_getc() - Receive one character (blocking).
 *
 * Spins until RX FIFO has data, then reads and returns the byte.
 *
 * @return: Received character.
 */
char riscv64_uart_getc(void);

/**
 * riscv64_uart_getc_nonblock() - Non-blocking receive.
 *
 * @return: Received character, or -1 if RX FIFO is empty.
 */
int  riscv64_uart_getc_nonblock(void);

/**
 * riscv64_uart_printf() - Formatted output to UART.
 *
 * Minimal printf supporting: %c %s %d %u %x %X %p %%
 * Supports 64-bit integers and zero-padding.
 *
 * @fmt: printf-style format string.
 */
void riscv64_uart_printf(const char *fmt, ...);

/**
 * riscv64_uart_vprintf() - va_list variant of riscv64_uart_printf().
 */
void riscv64_uart_vprintf(const char *fmt, va_list ap);

/**
 * riscv64_uart_rx_ready() - Check if RX FIFO has data.
 *
 * @return: Non-zero if data is available.
 */
int  riscv64_uart_rx_ready(void);

/**
 * riscv64_uart_tx_ready() - Check if TX FIFO has space.
 *
 * @return: Non-zero if THR is empty (can write).
 */
int  riscv64_uart_tx_ready(void);

#endif /* RISCV64_UART_H */

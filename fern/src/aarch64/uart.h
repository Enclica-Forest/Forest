/*
 * Fern - AArch64 PL011 UART driver
 *
 * PL011 UART base address for QEMU -machine virt: 0x09000000
 * IRQ: INTID 33 (SPI #1 in GICv3 numbering, UART0)
 *
 * The PL011 is a full UART with FIFOs.  We use a minimal polled driver
 * during early boot; interrupt-driven I/O can be layered on top later.
 */
#ifndef AARCH64_UART_H
#define AARCH64_UART_H

#include <stdint.h>
#include <stdarg.h>

/* PL011 MMIO base (QEMU virt UART0) */
#define PL011_BASE          0x09000000UL

/* PL011 register offsets */
#define UARTDR              0x000   /* Data register          */
#define UARTRSR             0x004   /* Receive status / error */
#define UARTFR              0x018   /* Flag register          */
#define UARTIBRD            0x024   /* Integer baud-rate div  */
#define UARTFBRD            0x028   /* Fractional baud-rate   */
#define UARTLCRH            0x02C   /* Line control           */
#define UARTCR              0x030   /* Control register       */
#define UARTIFLS            0x034   /* Interrupt FIFO level   */
#define UARTIMSC            0x038   /* Interrupt mask set/clr */
#define UARTRIS             0x03C   /* Raw interrupt status   */
#define UARTMIS             0x040   /* Masked interrupt status*/
#define UARTICR             0x044   /* Interrupt clear        */
#define UARTDMACR           0x048   /* DMA control            */

/* Flag register bits */
#define UARTFR_TXFF         (1 << 5)    /* Transmit FIFO full  */
#define UARTFR_RXFE         (1 << 4)    /* Receive FIFO empty  */
#define UARTFR_BUSY         (1 << 3)    /* UART busy           */

/* Line control bits */
#define UARTLCRH_FEN        (1 << 4)    /* FIFO enable         */
#define UARTLCRH_WLEN_8     (3 << 5)    /* 8-bit word length   */

/* Control register bits */
#define UARTCR_UARTEN       (1 << 0)    /* UART enable         */
#define UARTCR_TXE          (1 << 8)    /* TX enable           */
#define UARTCR_RXE          (1 << 9)    /* RX enable           */

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/**
 * uart_init - Configure PL011 at 115200 baud, 8N1, FIFO enabled.
 * Reference clock assumed to be 24 MHz (QEMU virt default).
 */
void uart_init(void);

/**
 * uart_putc - Write a single character (blocks if TX FIFO full).
 */
void uart_putc(char c);

/**
 * uart_getc - Read a single character (blocks until RX FIFO non-empty).
 */
char uart_getc(void);

/**
 * uart_puts - Write a NUL-terminated string; replaces '\n' with "\r\n".
 */
void uart_puts(const char *s);

/**
 * uart_printf - Minimal printf-like formatted output to UART.
 * Supports: %c %s %d %u %x %p %% (no floating-point).
 */
void uart_printf(const char *fmt, ...);

#endif /* AARCH64_UART_H */

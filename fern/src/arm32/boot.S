/*
 * boot.S - ARM32 bare-metal boot entry for Forest OS
 *
 * Target: QEMU -machine virt -cpu cortex-a15 / Raspberry Pi
 *
 * CPU starts in SVC mode with IRQ/FIQ disabled (hardware guarantee on reset).
 * The ELF is loaded by QEMU at the address specified in the linker script
 * (default: 0x40000000).  No ROM-to-RAM copy is needed for QEMU; the data
 * copy loop handles real hardware where LMA != VMA.
 *
 * Execution flow:
 *   1. _vector_table installed as first thing in image (.text.boot)
 *   2. reset_handler: mask all interrupts, set VBAR, set per-mode stacks
 *   3. Zero BSS, copy .data from ROM to RAM (no-op on QEMU ELF loads)
 *   4. Inline PL011 UART init + early banner via early_puts
 *   5. bl kernel_main  (never returns)
 *
 * ARM processor mode encodings (CPSR[4:0]):
 *   0x10 = USR   0x11 = FIQ   0x12 = IRQ   0x13 = SVC
 *   0x17 = ABT   0x1B = UND   0x1F = SYS
 */

.syntax unified
.arch armv7-a
.cpu  cortex-a15

/* -----------------------------------------------------------------------
 * Per-mode stack sizes
 * --------------------------------------------------------------------- */
#define SVC_STACK_SIZE  8192    /* main supervisor stack                 */
#define IRQ_STACK_SIZE  4096    /* IRQ handler stack                     */
#define FIQ_STACK_SIZE  1024    /* FIQ handler stack (banked r8-r12)    */
#define ABT_STACK_SIZE  2048    /* Abort handler stack                   */
#define UND_STACK_SIZE  1024    /* Undefined instruction handler stack   */

/* -----------------------------------------------------------------------
 * CPSR mode bits (CPSR[4:0]) and interrupt mask bits
 * --------------------------------------------------------------------- */
#define MODE_USR        0x10
#define MODE_FIQ        0x11
#define MODE_IRQ        0x12
#define MODE_SVC        0x13
#define MODE_ABT        0x17
#define MODE_UND        0x1B
#define MODE_SYS        0x1F

#define CPSR_IRQ_DISABLE  (1 << 7)
#define CPSR_FIQ_DISABLE  (1 << 6)
#define CPSR_MASK_ALL     (CPSR_IRQ_DISABLE | CPSR_FIQ_DISABLE)

/* -----------------------------------------------------------------------
 * PL011 UART registers  (QEMU virt base 0x09000000)
 *
 * Baud rate: UARTCLK = 24 MHz, 115200 baud
 *   BRD_x64 = (24000000 * 4 + 57600) / 115200 = 834
 *   IBRD = 834 >> 6 = 13
 *   FBRD = 834 & 0x3F = 2
 * --------------------------------------------------------------------- */
#define UART_BASE       0x09000000
#define UART_DR         0x000       /* Data register                    */
#define UART_FR         0x018       /* Flag register                    */
#define UART_FR_TXFF    (1 << 5)    /* Transmit FIFO full               */
#define UART_FR_BUSY    (1 << 3)    /* UART busy                        */
#define UART_IBRD       0x024       /* Integer baud rate divisor        */
#define UART_FBRD       0x028       /* Fractional baud rate divisor     */
#define UART_LCRH       0x02C       /* Line control register            */
#define UART_CR         0x030       /* Control register                 */
#define UART_IMSC       0x038       /* Interrupt mask set/clear         */
#define UART_ICR        0x044       /* Interrupt clear register         */

/* LCRH: 8-bit word (WLEN=0b11), FIFO enabled (FEN=1) */
#define UART_LCRH_8N1   ((0x3 << 5) | (1 << 4))
/* CR: UART enable | TX enable | RX enable */
#define UART_CR_ENABLE  ((1 << 0) | (1 << 8) | (1 << 9))
/* Clear all interrupts */
#define UART_INT_CLR    0x7FF

/* -----------------------------------------------------------------------
 * Exception vector table.
 *
 * Placed first in the image via .section .text.boot so that the linker
 * script can position it at the load address.  Each of the eight vector
 * slots holds a PC-relative load that reaches anywhere in the 4 GB
 * address space (unlike a plain "b" which is limited to ±32 MB).
 *
 * The eight words immediately after the vector entries are an absolute
 * address table (jump table); the ldr pc instructions load from there.
 * --------------------------------------------------------------------- */
.section .text.boot, "ax"
.global _start
.global _vector_table

_start:
_vector_table:
    ldr pc, _reset_handler_addr         /* 0x00  Reset                  */
    ldr pc, _undef_handler_addr         /* 0x04  Undefined instruction  */
    ldr pc, _swi_handler_addr           /* 0x08  Supervisor Call (SWI)  */
    ldr pc, _prefetch_handler_addr      /* 0x0C  Prefetch abort         */
    ldr pc, _data_handler_addr          /* 0x10  Data abort             */
    ldr pc, _reserved_handler_addr      /* 0x14  Reserved / Hyp (v7)   */
    ldr pc, _irq_handler_addr           /* 0x18  IRQ                    */
    ldr pc, _fiq_handler_addr           /* 0x1C  FIQ                    */

/* Absolute address table – 32-bit values filled by the linker */
_reset_handler_addr:    .word reset_handler
_undef_handler_addr:    .word undef_handler
_swi_handler_addr:      .word swi_handler
_prefetch_handler_addr: .word prefetch_handler
_data_handler_addr:     .word data_handler
_reserved_handler_addr: .word reserved_handler
_irq_handler_addr:      .word irq_handler
_fiq_handler_addr:      .word fiq_handler

/* -----------------------------------------------------------------------
 * reset_handler
 *
 * Entry point after power-on / warm reset.
 * Hardware enters SVC mode with IRQ and FIQ already disabled.
 * We re-assert the mask explicitly to be safe, then proceed.
 * --------------------------------------------------------------------- */
.section .text
.global reset_handler
.type   reset_handler, %function
reset_handler:
    /* ------------------------------------------------------------------
     * Step 1: Mask all interrupts (belt-and-suspenders).
     *
     * The ARM boot protocol passes the DTB address in r2.  Save it
     * before any call clobbers registers.
     * ---------------------------------------------------------------- */
    mov  r4, r2                /* r4 = DTB address (preserved across calls) */

    mrs  r0, cpsr
    orr  r0, r0, #CPSR_MASK_ALL
    msr  cpsr_c, r0

    /* ------------------------------------------------------------------
     * Step 2: Point VBAR at our vector table.
     *
     * On Cortex-A15 the VBAR (Vector Base Address Register, CP15 c12)
     * controls where the CPU reads its exception vectors.
     * ---------------------------------------------------------------- */
    ldr  r0, =_vector_table
    mcr  p15, 0, r0, c12, c0, 0    /* VBAR = &_vector_table            */
    isb

    /* ------------------------------------------------------------------
     * Step 3: Set up stacks for every processor mode.
     *
     * We switch mode via CPSR[4:0], set SP to the top of that mode's
     * stack area (stacks grow downward), then continue to the next mode.
     * All modes keep IRQ+FIQ masked during this setup.
     * ---------------------------------------------------------------- */

    /* --- IRQ mode --- */
    ldr  r0, =_irq_stack_top
    msr  cpsr_c, #(MODE_IRQ | CPSR_MASK_ALL)
    mov  sp, r0

    /* --- FIQ mode --- */
    ldr  r0, =_fiq_stack_top
    msr  cpsr_c, #(MODE_FIQ | CPSR_MASK_ALL)
    mov  sp, r0

    /* --- Abort mode --- */
    ldr  r0, =_abt_stack_top
    msr  cpsr_c, #(MODE_ABT | CPSR_MASK_ALL)
    mov  sp, r0

    /* --- Undefined instruction mode --- */
    ldr  r0, =_und_stack_top
    msr  cpsr_c, #(MODE_UND | CPSR_MASK_ALL)
    mov  sp, r0

    /* --- SVC mode (main kernel stack – stay here) --- */
    ldr  r0, =_svc_stack_top
    msr  cpsr_c, #(MODE_SVC | CPSR_MASK_ALL)
    mov  sp, r0

    /* ------------------------------------------------------------------
     * Step 4: Zero-fill the BSS section.
     *
     * r0 = cursor (starts at _bss_start)
     * r1 = zero constant
     * r2 = end sentinel (_bss_end)
     *
     * "strlo" writes only when r0 < r2 (unsigned), then advances r0 by 4.
     * ---------------------------------------------------------------- */
    ldr  r0, =_bss_start
    ldr  r2, =_bss_end
    mov  r1, #0
bss_clear_loop:
    cmp  r0, r2
    strlo r1, [r0], #4
    blo  bss_clear_loop

    /* ------------------------------------------------------------------
     * Step 5: Copy .data section from load address (ROM/flash) to run
     *         address (RAM).
     *
     * For QEMU ELF loads the LMA == VMA so this is a zero-iteration
     * no-op; it is correct for real hardware where the image is burned
     * into flash at a different address than where it runs.
     *
     * _data_lma  – load-time (ROM) address of .data (defined by linker)
     * _data_start – run-time (RAM) address of .data
     * _data_end   – end of .data in RAM
     * ---------------------------------------------------------------- */
    ldr  r0, =_data_lma
    ldr  r1, =_data_start
    ldr  r2, =_data_end
    cmp  r0, r1                    /* skip if ROM == RAM               */
    beq  data_copy_done
data_copy_loop:
    cmp  r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo  data_copy_loop
data_copy_done:

    /* ------------------------------------------------------------------
     * Step 6: PL011 UART early initialisation (115200 8N1, no IRQs).
     *
     * Sequence mandated by the PL011 TRM §3.3.8 "Programming the UART":
     *   a. Disable UART.
     *   b. Wait for BUSY = 0.
     *   c. Flush FIFOs (write LCRH with FEN=0 while disabled).
     *   d. Clear pending interrupts.
     *   e. Set baud rate divisors.
     *   f. Set line control (8N1, FIFO on).
     *   g. Mask all interrupts.
     *   h. Enable UART (UARTEN | TXE | RXE).
     * ---------------------------------------------------------------- */
    ldr  r3, =UART_BASE

    /* a. Disable UART */
    mov  r1, #0
    str  r1, [r3, #UART_CR]

    /* b. Wait for BUSY = 0 */
uart_busy_wait:
    ldr  r1, [r3, #UART_FR]
    tst  r1, #UART_FR_BUSY
    bne  uart_busy_wait

    /* c. Flush FIFOs: write LCRH with FEN=0 */
    mov  r1, #0
    str  r1, [r3, #UART_LCRH]

    /* d. Clear all pending interrupts */
    ldr  r1, =UART_INT_CLR
    str  r1, [r3, #UART_ICR]

    /* e. Set baud rate: IBRD=13, FBRD=2  (115200 @ 24 MHz) */
    mov  r1, #13
    str  r1, [r3, #UART_IBRD]
    mov  r1, #2
    str  r1, [r3, #UART_FBRD]

    /* f. Line control: 8-bit, no parity, 1 stop bit, FIFO enabled */
    mov  r1, #UART_LCRH_8N1
    str  r1, [r3, #UART_LCRH]

    /* g. Mask all interrupts */
    mov  r1, #0
    str  r1, [r3, #UART_IMSC]

    /* h. Enable UART: TX + RX + UART enable */
    ldr  r1, =UART_CR_ENABLE
    str  r1, [r3, #UART_CR]

    /* ------------------------------------------------------------------
     * Step 7: Print early banner so we know the boot path is live.
     * ---------------------------------------------------------------- */
    ldr  r0, =early_banner
    bl   early_puts

    /* ------------------------------------------------------------------
     * Step 8: Call kernel_main(dtb_addr).
     *
     * We remain in SVC mode (CPSR[4:0] = 0x13).
     * IRQ and FIQ remain masked; kernel_main() enables them when ready.
     *
     * r0 = DTB address (saved in r4 at entry, moved to r0 for ABI)
     * ---------------------------------------------------------------- */
    mov  r0, r4
    bl   kernel_main

    /* Should never reach here – halt the CPU */
halt:
    wfi
    b    halt

.size reset_handler, . - reset_handler

/* -----------------------------------------------------------------------
 * reserved_handler
 *
 * Catches the "reserved" vector (0x14), which on ARMv7 with the
 * Virtualisation Extensions may be the Hyp trap vector.  We simply
 * print a message and spin.
 * --------------------------------------------------------------------- */
.global reserved_handler
.type   reserved_handler, %function
reserved_handler:
    ldr  r0, =msg_reserved
    bl   early_puts
1:  wfi
    b    1b
.size reserved_handler, . - reserved_handler

/* -----------------------------------------------------------------------
 * early_puts – blocking UART string output (NUL-terminated C string).
 *
 * Arguments:
 *   r0 = pointer to NUL-terminated string
 *
 * Clobbers: r0, r1, r2, r3
 * Safe to call before the C runtime is initialised (no stack frame used
 * beyond what the caller already has; LR is not touched internally).
 * --------------------------------------------------------------------- */
.global early_puts
.type   early_puts, %function
early_puts:
    ldr  r2, =UART_BASE
early_puts_loop:
    ldrb r1, [r0], #1          /* load byte, advance pointer            */
    cmp  r1, #0
    beq  early_puts_done
early_puts_wait:
    ldr  r3, [r2, #UART_FR]
    tst  r3, #UART_FR_TXFF
    bne  early_puts_wait
    str  r1, [r2, #UART_DR]
    b    early_puts_loop
early_puts_done:
    bx   lr
.size early_puts, . - early_puts

/* -----------------------------------------------------------------------
 * early_putc – blocking single-character UART output.
 *
 * Arguments:
 *   r0 = character (low 8 bits used)
 *
 * Clobbers: r1, r2
 * --------------------------------------------------------------------- */
.global early_putc
.type   early_putc, %function
early_putc:
    ldr  r2, =UART_BASE
early_putc_wait:
    ldr  r1, [r2, #UART_FR]
    tst  r1, #UART_FR_TXFF
    bne  early_putc_wait
    str  r0, [r2, #UART_DR]
    bx   lr
.size early_putc, . - early_putc

/* -----------------------------------------------------------------------
 * Read-only data
 * --------------------------------------------------------------------- */
.section .rodata

early_banner:
    .asciz "\r\nForest OS ARM32 booting...\r\n"

msg_reserved:
    .asciz "\r\n[FATAL] Reserved/Hyp exception\r\n"

/* -----------------------------------------------------------------------
 * Per-mode exception stacks  (in BSS – zero-initialised by reset_handler)
 *
 * Each stack is 8-byte aligned so that AAPCS-compliant C code can use
 * double-word push/pop instructions (stmfd/ldmfd with r4-r11) safely.
 *
 * Layout (low address first):
 *
 *   _svc_stack_bottom   _svc_stack_top   (SVC_STACK_SIZE bytes)
 *   _irq_stack_bottom   _irq_stack_top   (IRQ_STACK_SIZE bytes)
 *   _fiq_stack_bottom   _fiq_stack_top   (FIQ_STACK_SIZE bytes)
 *   _abt_stack_bottom   _abt_stack_top   (ABT_STACK_SIZE bytes)
 *   _und_stack_bottom   _und_stack_top   (UND_STACK_SIZE bytes)
 * --------------------------------------------------------------------- */
.section .bss

.balign 8
.global _svc_stack_bottom
.global _svc_stack_top
_svc_stack_bottom:
    .space SVC_STACK_SIZE
_svc_stack_top:

.balign 8
.global _irq_stack_bottom
.global _irq_stack_top
_irq_stack_bottom:
    .space IRQ_STACK_SIZE
_irq_stack_top:

.balign 8
.global _fiq_stack_bottom
.global _fiq_stack_top
_fiq_stack_bottom:
    .space FIQ_STACK_SIZE
_fiq_stack_top:

.balign 8
.global _abt_stack_bottom
.global _abt_stack_top
_abt_stack_bottom:
    .space ABT_STACK_SIZE
_abt_stack_top:

.balign 8
.global _und_stack_bottom
.global _und_stack_top
_und_stack_bottom:
    .space UND_STACK_SIZE
_und_stack_top:

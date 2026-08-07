/*
 * arm32.h - Master header for the Fern ARM32 (ARMv7-A) subsystem
 *
 * This header aggregates all ARM32-specific type definitions, CP15 register
 * accessors, mode constants, and forward declarations needed by C code in
 * src/arm32/.  Each major subsystem (UART, GIC, MMU, cache, IRQ) has its own
 * dedicated header; this file provides the glue types shared across all of them.
 *
 * Target: QEMU -machine virt -cpu cortex-a15 (also Raspberry Pi 2/3 with
 *         -DUART_BASE_ADDR override).
 *
 * Coding conventions in this subsystem:
 *   - All MMIO accesses through volatile pointers.
 *   - CP15 operations via explicit MCR/MRC inline asm.
 *   - No floating-point (kernel runs with VFP disabled until explicitly enabled).
 *   - All sizes expressed in bytes; addresses are uint32_t.
 */

#ifndef ARM32_H
#define ARM32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Subsystem headers – include them all through arm32.h for convenience
 * ========================================================================= */
#include "uart.h"
#include "gic.h"
#include "irq.h"
#include "mmu.h"
#include "cache.h"

/* =========================================================================
 * ARM32 processor mode constants  (CPSR[4:0])
 * ========================================================================= */
#define ARM32_MODE_USR  0x10U   /**< User mode                          */
#define ARM32_MODE_FIQ  0x11U   /**< FIQ mode                           */
#define ARM32_MODE_IRQ  0x12U   /**< IRQ mode                           */
#define ARM32_MODE_SVC  0x13U   /**< Supervisor mode                    */
#define ARM32_MODE_ABT  0x17U   /**< Abort mode                         */
#define ARM32_MODE_UND  0x1BU   /**< Undefined instruction mode         */
#define ARM32_MODE_SYS  0x1FU   /**< System mode (privileged USR)       */

/** Mask for the mode field in CPSR */
#define ARM32_MODE_MASK 0x1FU

/* =========================================================================
 * CPSR / SPSR bit definitions  (ARM DDI 0406C §B1.3.3)
 * ========================================================================= */
#define ARM32_CPSR_N    (1U << 31)  /**< Negative / Less Than flag      */
#define ARM32_CPSR_Z    (1U << 30)  /**< Zero flag                      */
#define ARM32_CPSR_C    (1U << 29)  /**< Carry / Borrow / Extend flag   */
#define ARM32_CPSR_V    (1U << 28)  /**< Overflow flag                  */
#define ARM32_CPSR_Q    (1U << 27)  /**< Saturation / Sticky overflow   */
#define ARM32_CPSR_E    (1U <<  9)  /**< Endianness state (0 = LE)      */
#define ARM32_CPSR_A    (1U <<  8)  /**< Async abort disable            */
#define ARM32_CPSR_I    (1U <<  7)  /**< IRQ disable                    */
#define ARM32_CPSR_F    (1U <<  6)  /**< FIQ disable                    */
#define ARM32_CPSR_T    (1U <<  5)  /**< Thumb execution state          */

/* =========================================================================
 * Saved CPU register frame  (matches the layout built by SAVE_CONTEXT in
 * exceptions.S)
 *
 * Stack layout (SP points to lowest address after SAVE_CONTEXT):
 *
 *   [sp +  0]  spsr       – interrupted CPSR
 *   [sp +  4]  r0
 *   [sp +  8]  r1
 *   ...
 *   [sp + 52]  r12
 *   [sp + 56]  lr         – exception link register (adjusted by handler)
 *
 * Total size: 15 × 4 = 60 bytes
 * ========================================================================= */

/**
 * arm_regs_t - Saved CPU register context frame.
 *
 * Passed by pointer to C exception handlers so they can inspect and
 * optionally modify the interrupted state.
 */
typedef struct arm_regs_t {
    uint32_t spsr;      /**< Saved PSR  (interrupted CPSR)              */
    uint32_t r0;        /**< General-purpose register 0                 */
    uint32_t r1;        /**< General-purpose register 1                 */
    uint32_t r2;        /**< General-purpose register 2                 */
    uint32_t r3;        /**< General-purpose register 3                 */
    uint32_t r4;        /**< General-purpose register 4                 */
    uint32_t r5;        /**< General-purpose register 5                 */
    uint32_t r6;        /**< General-purpose register 6                 */
    uint32_t r7;        /**< General-purpose register 7                 */
    uint32_t r8;        /**< General-purpose register 8                 */
    uint32_t r9;        /**< General-purpose register 9                 */
    uint32_t r10;       /**< General-purpose register 10               */
    uint32_t r11;       /**< Frame pointer (r11 by AAPCS convention)   */
    uint32_t r12;       /**< Intra-procedure-call scratch register     */
    uint32_t lr;        /**< Exception link register (adjusted PC)     */
} arm_regs_t;

/* =========================================================================
 * Linux ARM EABI syscall numbers  (uapi/asm/unistd.h)
 *
 * Used by arm32_syscall_handle() in syscall.c.
 * User-space places the syscall number in r7 before executing SWI #0.
 * ========================================================================= */
#define ARM32_NR_exit           1
#define ARM32_NR_fork           2
#define ARM32_NR_read           3
#define ARM32_NR_write          4
#define ARM32_NR_open           5
#define ARM32_NR_close          6
#define ARM32_NR_getpid        20
#define ARM32_NR_access        33
#define ARM32_NR_brk           45
#define ARM32_NR_ioctl         54
#define ARM32_NR_execve        11
#define ARM32_NR_lseek         19
#define ARM32_NR_getuid        24
#define ARM32_NR_kill          37
#define ARM32_NR_stat          106
#define ARM32_NR_fstat         108
#define ARM32_NR_lstat         107
#define ARM32_NR_waitpid        7
#define ARM32_NR_mmap2         192
#define ARM32_NR_munmap         91
#define ARM32_NR_mprotect      125
#define ARM32_NR_nanosleep     162
#define ARM32_NR_gettimeofday   78
#define ARM32_NR_socket        281
#define ARM32_NR_bind          282
#define ARM32_NR_connect       283
#define ARM32_NR_send          289
#define ARM32_NR_recv          291
#define ARM32_NR_select        142
#define ARM32_NR_sched_yield   158
#define ARM32_NR_exit_group    248
#define ARM32_NR_set_tid_address 256

/* errno-compatible error codes (POSIX subset, unsigned so negation is safe) */
#define ENOSYS  38U
#define EINVAL  22U
#define EFAULT  14U
#define ENOMEM  12U
#define EBADF    9U
#define EACCES  13U

/* =========================================================================
 * CP15 system register inline accessors
 *
 * Implemented as static inlines so they are available in any translation
 * unit that includes arm32.h without linking an extra object file.
 * ========================================================================= */

/**
 * arm32_get_cpsr - Read the Current Program Status Register.
 */
static inline uint32_t arm32_get_cpsr(void)
{
    uint32_t val;
    __asm__ volatile("mrs %0, cpsr" : "=r"(val));
    return val;
}

/**
 * arm32_get_spsr - Read the Saved Program Status Register.
 *
 * Only meaningful when called from an exception mode; in SVC mode this
 * returns the SPSR of the most recent exception entry to SVC mode.
 */
static inline uint32_t arm32_get_spsr(void)
{
    uint32_t val;
    __asm__ volatile("mrs %0, spsr" : "=r"(val));
    return val;
}

/**
 * arm32_current_mode - Extract the processor mode from CPSR.
 *
 * @return  One of the ARM32_MODE_* constants.
 */
static inline uint32_t arm32_current_mode(void)
{
    return arm32_get_cpsr() & ARM32_MODE_MASK;
}

/**
 * arm32_irq_enable - Clear the I-bit in CPSR (enable IRQ delivery).
 */
static inline void arm32_irq_enable(void)
{
    __asm__ volatile("cpsie i" : : : "memory");
}

/**
 * arm32_irq_disable - Set the I-bit in CPSR (mask IRQ delivery).
 */
static inline void arm32_irq_disable(void)
{
    __asm__ volatile("cpsid i" : : : "memory");
}

/**
 * arm32_fiq_enable - Clear the F-bit in CPSR (enable FIQ delivery).
 */
static inline void arm32_fiq_enable(void)
{
    __asm__ volatile("cpsie f" : : : "memory");
}

/**
 * arm32_fiq_disable - Set the F-bit in CPSR (mask FIQ delivery).
 */
static inline void arm32_fiq_disable(void)
{
    __asm__ volatile("cpsid f" : : : "memory");
}

/**
 * arm32_irq_save - Save CPSR and mask IRQ+FIQ atomically.
 *
 * @return  Previous CPSR value (pass to arm32_irq_restore to re-enable).
 */
static inline uint32_t arm32_irq_save(void)
{
    uint32_t flags;
    __asm__ volatile(
        "mrs  %0, cpsr\n"
        "cpsid if\n"
        : "=r"(flags) : : "memory");
    return flags;
}

/**
 * arm32_irq_restore - Restore CPSR interrupt flags saved by arm32_irq_save.
 *
 * @param flags  Value previously returned by arm32_irq_save().
 */
static inline void arm32_irq_restore(uint32_t flags)
{
    __asm__ volatile("msr cpsr_c, %0" : : "r"(flags) : "memory");
}

/**
 * arm32_wfi - Issue a Wait For Interrupt (WFI) instruction.
 *
 * Halts the CPU clock until an interrupt arrives, reducing power consumption.
 * The CPU wakes on any enabled interrupt even if IRQ/FIQ are masked in CPSR
 * (the interrupt is not delivered until the mask is cleared, but the CPU wakes).
 */
static inline void arm32_wfi(void)
{
    __asm__ volatile("wfi" : : : "memory");
}

/**
 * arm32_wfe - Issue a Wait For Event (WFE) instruction.
 *
 * Used in spin-lock implementations and CPU parking loops.
 */
static inline void arm32_wfe(void)
{
    __asm__ volatile("wfe" : : : "memory");
}

/**
 * arm32_sev - Issue a Send Event (SEV) instruction.
 *
 * Wakes all cores that are waiting in WFE (multicore synchronisation).
 */
static inline void arm32_sev(void)
{
    __asm__ volatile("sev" : : : "memory");
}

/**
 * arm32_read_dfar - Read the Data Fault Address Register (DFAR).
 *
 * Contains the virtual address that caused the most recent data abort.
 */
static inline uint32_t arm32_read_dfar(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(val));
    return val;
}

/**
 * arm32_read_dfsr - Read the Data Fault Status Register (DFSR).
 */
static inline uint32_t arm32_read_dfsr(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(val));
    return val;
}

/**
 * arm32_read_ifar - Read the Instruction Fault Address Register (IFAR).
 *
 * Contains the virtual address that caused the most recent prefetch abort.
 */
static inline uint32_t arm32_read_ifar(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(val));
    return val;
}

/**
 * arm32_read_ifsr - Read the Instruction Fault Status Register (IFSR).
 */
static inline uint32_t arm32_read_ifsr(void)
{
    uint32_t val;
    __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(val));
    return val;
}

/* =========================================================================
 * Exception handler C prototypes
 *
 * These are called by the assembly stubs in exceptions.S.  Implementations
 * live in exception_handlers.c (or another C file in src/arm32/).
 * ========================================================================= */

/**
 * syscall_handle - Dispatch an ARM EABI system call.
 *
 * Called by the SWI handler in exceptions.S.
 *
 * @regs:    Pointer to the saved register frame on the SVC stack.
 *           The C handler may modify regs->r0 to set the syscall return value.
 * @swi_num: 24-bit SWI immediate extracted from the SWI instruction word.
 *           For Linux ARM EABI syscalls this is always 0; the actual syscall
 *           number is in regs->r7.
 */
void syscall_handle(arm_regs_t *regs, uint32_t swi_num);

/**
 * undef_instr_handler - Handle an undefined-instruction exception.
 *
 * @regs:         Saved register frame.
 * @fault_addr:   Address of the undefined instruction.
 */
void undef_instr_handler(arm_regs_t *regs, uint32_t fault_addr);

/**
 * prefetch_abort_handler - Handle a prefetch abort (instruction fetch fault).
 *
 * @regs:    Saved register frame.
 * @ifar:    Instruction Fault Address Register value.
 * @ifsr:    Instruction Fault Status Register value.
 */
void prefetch_abort_handler(arm_regs_t *regs, uint32_t ifar, uint32_t ifsr);

/**
 * data_abort_handler - Handle a data abort (load/store fault).
 *
 * @regs:    Saved register frame.
 * @dfar:    Data Fault Address Register value.
 * @dfsr:    Data Fault Status Register value.
 */
void data_abort_handler(arm_regs_t *regs, uint32_t dfar, uint32_t dfsr);

/**
 * fiq_dispatch - Top-level FIQ dispatcher (called from exceptions.S).
 *
 * On QEMU virt the FIQ is rarely used; this provides a hook for custom FIQ
 * handlers (e.g. a high-priority timer).
 */
void fiq_dispatch(void);

/* =========================================================================
 * Linker-script symbols
 *
 * These are declared extern here so C code can take their addresses.
 * They have no type in C; cast to uint32_t or uint8_t* as appropriate.
 * ========================================================================= */
extern char _bss_start[];       /**< Start of BSS section (from linker) */
extern char _bss_end[];         /**< End of BSS section (from linker)   */
extern char _data_start[];      /**< Start of .data in RAM              */
extern char _data_end[];        /**< End of .data in RAM                */
extern char _data_lma[];        /**< Load address of .data (ROM/flash)  */
extern char _text_start[];      /**< Start of .text section             */
extern char _text_end[];        /**< End of .text section               */

/* Stack symbols (per-mode, defined in boot.S BSS) */
extern char _svc_stack_bottom[];
extern char _svc_stack_top[];
extern char _irq_stack_bottom[];
extern char _irq_stack_top[];
extern char _fiq_stack_bottom[];
extern char _fiq_stack_top[];
extern char _abt_stack_bottom[];
extern char _abt_stack_top[];
extern char _und_stack_bottom[];
extern char _und_stack_top[];

/* =========================================================================
 * Miscellaneous utility macros
 * ========================================================================= */

/** Round @x up to the next multiple of @align (which must be a power of 2) */
#define ARM32_ALIGN_UP(x, align)    (((x) + (align) - 1U) & ~((align) - 1U))

/** Round @x down to the previous multiple of @align (power of 2) */
#define ARM32_ALIGN_DOWN(x, align)  ((x) & ~((align) - 1U))

/** Number of elements in a C array */
#define ARM32_ARRAY_SIZE(arr)       (sizeof(arr) / sizeof((arr)[0]))

/** Minimum / maximum helpers */
#define ARM32_MIN(a, b)   ((a) < (b) ? (a) : (b))
#define ARM32_MAX(a, b)   ((a) > (b) ? (a) : (b))

/** Mark a function as not returning (for the compiler optimiser) */
#define ARM32_NORETURN  __attribute__((noreturn))

/** Prevent the compiler from reordering memory accesses across this point */
#define ARM32_COMPILER_BARRIER()  __asm__ volatile("" : : : "memory")

/** Force a variable to be read/written as a volatile MMIO access */
#define ARM32_MMIO_READ32(addr)       (*(volatile uint32_t *)(addr))
#define ARM32_MMIO_WRITE32(addr, val) ((*(volatile uint32_t *)(addr)) = (val))

#endif /* ARM32_H */

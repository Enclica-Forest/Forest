#ifndef X86_64_IST_HANDLING_H
#define X86_64_IST_HANDLING_H

#include <stdint.h>
#include <stdbool.h>

/*
 * x86-64 Interrupt Stack Table (IST) handling
 * Provides dedicated stacks for critical interrupts
 */

// IST stack indices (1-7, 0 means no IST)
#define IST_STACK_NONE          0
#define IST_STACK_DF            1  // Double Fault
#define IST_STACK_NMI           2  // Non-Maskable Interrupt
#define IST_STACK_MC            3  // Machine Check
#define IST_STACK_DEBUG         4  // Debug exceptions
#define IST_STACK_RESERVED1     5
#define IST_STACK_RESERVED2     6
#define IST_STACK_RESERVED3     7

// IST stack size (16KB per stack)
#define IST_STACK_SIZE          0x4000

// Maximum number of CPUs supported
#define MAX_CPUS                256

typedef struct {
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
} __attribute__((packed)) ist_table_t;

typedef struct {
    void *stacks[8];  // IST stacks 0-7 (0 unused)
    uintptr_t stack_tops[8];
    bool initialized;
} cpu_ist_context_t;

// IST initialization and management
int ist_init_global(void);
int ist_init_cpu(uint32_t cpu_id);
void ist_cleanup_cpu(uint32_t cpu_id);

// Stack allocation and setup
int ist_allocate_stacks(uint32_t cpu_id);
void ist_free_stacks(uint32_t cpu_id);
int ist_setup_tss_stacks(uint32_t cpu_id);

// IST stack access
uintptr_t ist_get_stack_top(uint32_t cpu_id, uint8_t ist_index);
void *ist_get_stack_base(uint32_t cpu_id, uint8_t ist_index);
size_t ist_get_stack_size(void);

// Exception handlers that use IST
void ist_double_fault_handler(void);
void ist_nmi_handler(void);
void ist_machine_check_handler(void);
void ist_debug_handler(void);

// IST configuration
int ist_configure_idt_entry(uint8_t vector, uint8_t ist_index);
int ist_set_handler_stack(uint8_t vector, uint8_t ist_index);

// Debugging and status
bool ist_is_initialized(uint32_t cpu_id);
void ist_dump_stacks(uint32_t cpu_id);
uint32_t ist_get_current_cpu(void);

// Stack overflow detection
bool ist_check_stack_overflow(uint32_t cpu_id, uint8_t ist_index);
void ist_install_guard_pages(uint32_t cpu_id);

// Per-CPU TSS management
int ist_update_tss(uint32_t cpu_id);
void ist_load_tss(uint32_t cpu_id);

/* =========================================================================
 * FPU / SSE / AVX state save-restore
 *
 * These helpers save and restore extended processor state (x87 FPU, SSE,
 * AVX) on context switch.  When ENABLE_XSAVE is on and the CPU supports
 * XSAVE, xsave/xrstor are used (covering SSE/AVX/AVX512 etc. as enabled
 * by CR4.OSXSAVE and XCR0).  Otherwise fxsave/fxrstor save the 512-byte
 * SSE state.
 *
 * Each thread is expected to carry a struct fpu_state in its
 * thread/task control block.  The scheduler calls fpu_save() on the
 * outgoing thread and fpu_restore() on the incoming thread.
 * ========================================================================= */

/** Legacy FXSAVE area (512 bytes) — used when XSAVE is unavailable. */
#define FPU_STATE_FXSAVE_SIZE   512

/** XSAVE area: 512-byte legacy header + 64-byte XSAVE header +
 *              feature-specific areas.  4096 bytes is enough for
 *              SSE/AVX/AVX512 on current CPUs. */
#define FPU_STATE_XSAVE_SIZE    4096

#if ENABLE_XSAVE
#define FPU_STATE_SIZE          FPU_STATE_XSAVE_SIZE
#else
#define FPU_STATE_SIZE          FPU_STATE_FXSAVE_SIZE
#endif

/** Aligned, zeroed FPU state buffer (per-thread).  Must be 64-byte
 *  aligned for xsave/xrstor; 16-byte is the FXSAVE minimum. */
typedef struct __attribute__((aligned(64))) fpu_state {
    uint8_t bytes[FPU_STATE_SIZE];
} fpu_state_t;

/**
 * @brief Save the current FPU/SSE/AVX state into @p state.
 *
 * Uses XSAVE when ENABLE_XSAVE is on and CR4.OSXSAVE is set; otherwise
 * falls back to FXSAVE.  The CR0.TS (task-switched) bit is set after
 * saving so the next FPU instruction traps (lazy-FPU optimisation).
 */
void fpu_save(fpu_state_t *state);

/**
 * @brief Restore FPU/SSE/AVX state from @p state.
 *
 * Uses XRSTOR or FXRSTOR as appropriate, and clears CR0.TS.
 */
void fpu_restore(const fpu_state_t *state);

/**
 * @brief Initialise an fpu_state buffer to the CPU's reset state.
 *
 * Call once when a new thread is created so its first FPU instruction
 * does not see garbage.
 */
void fpu_init_state(fpu_state_t *state);

/**
 * @brief Enable CR0.MP + CR0.NE, clear CR0.EM, and (if XSAVE supported)
 *        set CR4.OSXSAVE + CR4.OSFXSR.  Called once during boot before
 *        the first context switch.
 */
void fpu_enable_cpu(void);

#ifdef CONFIG_SMP
// SMP-specific IST functions
int ist_init_all_cpus(void);
void ist_cleanup_all_cpus(void);
#endif

#endif // X86_64_IST_HANDLING_H
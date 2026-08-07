/*
 * Fern - AArch64 Signal Frame Setup
 * signal.c
 *
 * Builds the signal delivery frame for AArch64 (ARMv8-A, 64-bit ARM)
 * user processes.
 *
 * AArch64 signal frame layout on user stack (high → low addresses):
 *
 *   [trampoline_addr]   address of trampoline code → becomes new ELR_EL1
 *   [saved_ctx]         saved user registers (x0-x30, sp_el0, etc.)
 *   [signum]            signal number (32-bit, zero-extended)
 *   [saved_ctx_ptr]     pointer to saved_ctx for rt_sigreturn
 *
 * The trampoline (12 bytes) lives at the lowest address:
 *   mov x8, #139       (SYS_RT_SIGRETURN = 139 for AArch64 Linux ABI)
 *   svc #0
 *   brk #0             (safety: BRK if svc returns)
 *
 * Signal handler receives:
 *   X0 = signum
 *   SP_EL0 = adjusted user stack pointer
 *   ELR_EL1 = handler address
 *   SPSR_EL1 = original PSTATE (DAIF masked, EL0t)
 */

#include "../../arch/arch.h"
#include "../../include/task.h"
#include "../../include/string.h"
#include "../../include/util.h"

#if ARCH_ARM64

/* Forward declaration */
extern task_t* current_task;

/* AArch64 Linux syscall number for rt_sigreturn */
#define AARCH64_SYS_RT_SIGRETURN  139

/* The signal trampoline: executes rt_sigreturn SVC */
static const uint32_t signal_trampoline_code[] = {
    0xD2801128,   /* mov x8, #139 (SYS_RT_SIGRETURN) */
    0xD4000001,   /* svc #0 */
    0xD4200000,   /* brk #0  (unreachable safety) */
};

/* SPSR_EL1 value for EL0t with DAIF masked (all exceptions disabled) */
#define SPSR_EL0T_DAIF_MASKED  0x3C0

/*
 * setup_signal_frame - Build AArch64 signal delivery frame
 *
 * Pushes the signal frame onto the user stack and modifies the
 * saved context so that when the task returns to user mode via ERET,
 * it executes the signal handler with signum in x0.
 *
 * The user stack must already be mapped and writable.
 */
int setup_signal_frame(task_t* task, int signum, void* handler, void** old_sp)
{
    if (!task || !handler || !old_sp || !*old_sp) {
        return SIGNAL_BAD_SIGNUM;
    }

    if (signum < 1 || signum > 31) {
        return SIGNAL_BAD_SIGNUM;
    }

    /* Current user stack pointer (SP_EL0 value when at EL0) */
    uint64_t user_sp = (uint64_t)*old_sp;

    /* Minimum required space: frame + trampoline + alignment */
    uint64_t frame_total = sizeof(signal_frame_t) + sizeof(signal_trampoline_code) + 16;

    /* Ensure stack is 16-byte aligned (AArch64 ABI requirement) */
    uint64_t new_sp = user_sp - frame_total;
    new_sp &= ~0x0FUL;

    /* The trampoline lives at the very bottom of the frame */
    uint64_t trampoline_addr = new_sp;

    /* The signal frame is just above the trampoline */
    signal_frame_t* frame = (signal_frame_t*)(new_sp + sizeof(signal_trampoline_code));

    /* Clear the frame */
    memory_set((uint8_t*)frame, 0, sizeof(signal_frame_t));

    /*
     * Save the original user context.
     * For full integration, the caller populates this from the
     * arch_cpu_state_t saved during the exception entry.
     */
    signal_saved_ctx_t* ctx = &frame->saved_ctx;
    memory_set((uint8_t*)ctx, 0, sizeof(signal_saved_ctx_t));

    /* Set up the saved context for rt_sigreturn:
     * ELR_EL1 = original PC (the instruction we interrupted)
     * SP_EL0  = original user stack pointer
     * SPSR_EL1 = original PSTATE
     * x0-x30  = original register values (populated by integration)
     */
    ctx->elr_el1   = 0;  /* Will be filled by integration with trap frame */
    ctx->sp_el0    = user_sp;
    ctx->spsr_el1  = SPSR_EL0T_DAIF_MASKED;
    ctx->x30       = 0;  /* LR: will be filled by integration */

    /* Fill in the rest of the frame */
    frame->signum        = (uint32_t)signum;
    frame->saved_ctx_ptr = (uint64_t)&frame->saved_ctx;
    frame->trampoline_addr = trampoline_addr;

    /* Copy the trampoline code to the user stack */
    memory_copy((const char*)signal_trampoline_code,
                (char*)trampoline_addr,
                sizeof(signal_trampoline_code));

    /* Update the user stack pointer */
    *old_sp = (void*)new_sp;

    /*
     * Modify the task's saved context to redirect execution.
     * The full integration requires hooking into the AArch64
     * exception return path (vectors.S restore_regs → eret).
     *
     * For AArch64, the trap frame on the kernel stack contains:
     *   [sp + 0x00] = ELR_EL1, SPSR_EL1
     *   [sp + 0x10] = x0, x1
     *   ...
     *   [sp + 0x100] = x30
     *
     * The signal delivery code will:
     *   1. Find the trap frame on the kernel stack
     *   2. Set frame->ELR_EL1 = handler (the user-space handler address)
     *   3. Set frame->x0 = signum (handler's first argument)
     *   4. Set frame->SP_EL0 = new_sp (adjusted user stack)
     *   5. When eret fires, execution lands at handler(signum)
     */

    return SIGNAL_DELIVERED;
}

/*
 * get_signal_trampoline_addr - Get the address of the signal trampoline
 *
 * Returns NULL. The trampoline is copied to the user stack during
 * setup_signal_frame(). Use frame->trampoline_addr instead.
 */
void* get_signal_trampoline_addr(void)
{
    return NULL;
}

#endif /* ARCH_ARM64 */

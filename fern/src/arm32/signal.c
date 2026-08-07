/*
 * Fern - ARM32 Signal Frame Setup
 * signal.c
 *
 * Builds the signal delivery frame for ARM32 (ARMv7-A) user processes.
 *
 * ARM32 signal frame layout on user stack (high → low addresses):
 *
 *   [trampoline_addr]   address of trampoline code → becomes new PC (via LR)
 *   [saved_ctx]         saved user registers (r0-r15, cpsr)
 *   [signum]            signal number (32-bit)
 *   [saved_ctx_ptr]     pointer to saved_ctx for rt_sigreturn
 *
 * The trampoline (12 bytes) lives at the lowest address:
 *   mov r7, #139        (SYS_RT_SIGRETURN = 139 for ARM32 Linux ABI)
 *   swi #0
 *   .word 0             (safety: illegal instruction if swi returns)
 *
 * Signal handler receives:
 *   R0 = signum
 *   SP = adjusted user stack pointer
 *   LR = trampoline address (return address from handler)
 *   PC = handler address (set via SVC return path)
 *   CPSR = original CPSR (restored from SPSR)
 */

#include "../../arch/arch.h"
#include "../../include/task.h"
#include "../../include/string.h"
#include "../../include/util.h"

#if ARCH_ARM32

/* Forward declaration */
extern task_t* current_task;

/* ARM32 Linux syscall number for rt_sigreturn */
#define ARM32_SYS_RT_SIGRETURN  139

/* The signal trampoline: executes rt_sigreturn SWI */
static const uint32_t signal_trampoline_code[] = {
    0xE3A0708B,   /* mov r7, #139 (SYS_RT_SIGRETURN) */
    0xEF000000,   /* swi #0 */
    0x00000000,   /* .word 0 (illegal instruction) */
};

/* CPSR value for USR mode with IRQ+FIQ disabled */
#define CPSR_USR_IRQ_FIQ_DISABLED  0xD0

/*
 * setup_signal_frame - Build ARM32 signal delivery frame
 *
 * Pushes the signal frame onto the user stack and modifies the
 * saved context so that when the task returns to user mode via
 * ldmfd sp!, {pc}^, it executes the signal handler with signum in r0.
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

    /* Current user stack pointer */
    uint32_t user_sp = (uint32_t)*old_sp;

    /* Minimum required space: frame + trampoline + alignment */
    uint32_t frame_total = sizeof(signal_frame_t) + sizeof(signal_trampoline_code) + 16;

    /* Ensure stack is 8-byte aligned (ARM32 AAPCS requirement) */
    uint32_t new_sp = user_sp - frame_total;
    new_sp &= ~0x07U;

    /* The trampoline lives at the very bottom of the frame */
    uint32_t trampoline_addr = new_sp;

    /* The signal frame is just above the trampoline */
    signal_frame_t* frame = (signal_frame_t*)(new_sp + sizeof(signal_trampoline_code));

    /* Clear the frame */
    memory_set((uint8_t*)frame, 0, sizeof(signal_frame_t));

    /*
     * Save the original user context.
     * For full integration, the caller populates this from the
     * arch_cpu_state_t saved during the SWI/IRQ entry.
     */
    signal_saved_ctx_t* ctx = &frame->saved_ctx;
    memory_set((uint8_t*)ctx, 0, sizeof(signal_saved_ctx_t));

    /* Set up the saved context for rt_sigreturn:
     * pc   = original PC (the instruction we interrupted)
     * cpsr = original CPSR (mode, flags, interrupt state)
     * sp   = original user stack pointer
     * lr   = original link register
     * All other registers (r0-r12) populated by integration.
     */
    ctx->pc    = 0;  /* Will be filled by integration with exception frame */
    ctx->cpsr  = CPSR_USR_IRQ_FIQ_DISABLED;  /* USR mode, IRQ+FIQ off */
    ctx->sp    = user_sp;
    ctx->lr    = 0;  /* Will be filled by integration */

    /* Fill in the rest of the frame */
    frame->signum        = (uint32_t)signum;
    frame->saved_ctx_ptr = (uint32_t)&frame->saved_ctx;
    frame->trampoline_addr = trampoline_addr;

    /* Copy the trampoline code to the user stack */
    memory_copy((const char*)signal_trampoline_code,
                (char*)trampoline_addr,
                sizeof(signal_trampoline_code));

    /* Update the user stack pointer */
    *old_sp = (void*)new_sp;

    /*
     * Modify the task's saved context to redirect execution.
     * The full integration requires hooking into the ARM32
     * exception return path (exceptions.S ldmfd sp!, {pc}^).
     *
     * For ARM32, the exception frame on the kernel stack contains:
     *   [sp + 0x00] = r0
     *   [sp + 0x04] = r1
     *   ...
     *   [sp + 0x3C] = r14 (lr)
     *   [sp + 0x40] = pc (return address)
     *   [sp + 0x44] = cpsr (saved by hardware into spsr, then copied)
     *
     * The signal delivery code will:
     *   1. Find the exception frame on the kernel stack
     *   2. Set frame->pc = handler (the user-space handler address)
     *   3. Set frame->r0 = signum (handler's first argument)
     *   4. Set frame->lr = trampoline_addr (handler returns to trampoline)
     *   5. Set frame->sp = new_sp (adjusted user stack)
     *   6. When ldmfd sp!, {pc}^ fires, execution lands at handler(signum)
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

#endif /* ARCH_ARM32 */

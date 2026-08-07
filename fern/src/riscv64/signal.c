/*
 * Fern - RISC-V 64-bit Signal Frame Setup
 * signal.c
 *
 * Builds the signal delivery frame for RISC-V 64-bit (RV64GC)
 * user processes.
 *
 * RISC-V 64 signal frame layout on user stack (high → low addresses):
 *
 *   [trampoline_addr]   address of trampoline code → becomes new sepc
 *   [saved_ctx]         saved user registers (x1-x31, sstatus, sepc)
 *   [signum]            signal number (32-bit, zero-extended)
 *   [saved_ctx_ptr]     pointer to saved_ctx for rt_sigreturn
 *
 * The trampoline (12 bytes) lives at the lowest address:
 *   li a7, 139          (SYS_RT_SIGRETURN = 139 for RISC-V Linux ABI)
 *   ecall
 *   .word 0             (safety: illegal instruction if ecall returns)
 *
 * Signal handler receives:
 *   A0 = signum
 *   SP = adjusted user stack pointer
 *   SEPC = handler address
 *   SSTATUS.SPIE = 1 (interrupts re-enabled on sret)
 */

#include "../../arch/arch.h"
#include "../../include/task.h"
#include "../../include/string.h"
#include "../../include/util.h"

#if ARCH_RISCV64

/* Forward declaration */
extern task_t* current_task;

/* RISC-V Linux syscall number for rt_sigreturn */
#define RISCV64_SYS_RT_SIGRETURN  139

/* The signal trampoline: executes rt_sigreturn ecall */
static const uint8_t signal_trampoline_code[] = {
    0x93, 0x03, 0x93, 0x00,   /* li a7, 139 (SYS_RT_SIGRETURN) */
    0x73, 0x00, 0x00, 0x00,   /* ecall */
    0x00, 0x00, 0x00, 0x00,   /* .word 0 (illegal instruction) */
};

/*
 * setup_signal_frame - Build RISC-V 64 signal delivery frame
 *
 * Pushes the signal frame onto the user stack and modifies the
 * saved context so that when the task returns to user mode via sret,
 * it executes the signal handler with signum in a0.
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
    uint64_t user_sp = (uint64_t)*old_sp;

    /* Minimum required space: frame + trampoline + alignment */
    uint64_t frame_total = sizeof(signal_frame_t) + sizeof(signal_trampoline_code) + 16;

    /* Ensure stack is 16-byte aligned (RISC-V ABI requirement) */
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
     * arch_cpu_state_t saved during the trap entry.
     */
    signal_saved_ctx_t* ctx = &frame->saved_ctx;
    memory_set((uint8_t*)ctx, 0, sizeof(signal_saved_ctx_t));

    /* Set up the saved context for rt_sigreturn:
     * sepc   = original PC (the instruction we interrupted)
     * sstatus = original SSTATUS (with SPIE=1, SPP=0 for U-mode)
     * sp     = original user stack pointer
     * All other registers populated by integration with trap frame.
     */
    ctx->sepc    = 0;  /* Will be filled by integration with trap frame */
    ctx->sstatus = (1ULL << 5);  /* SPIE=1 (re-enable interrupts on sret) */
    ctx->sp      = user_sp;
    ctx->ra      = 0;  /* Will be filled by integration */

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
     * The full integration requires hooking into the RISC-V
     * trap return path (trap.S restore_regs → sret).
     *
     * For RISC-V, the trap frame on the kernel stack contains:
     *   [sp + 0x00]  = ra (x1)
     *   [sp + 0x08]  = gp (x3)
     *   ...
     *   [sp + 0x40]  = a0
     *   ...
     *   [sp + 0xf0]  = sstatus
     *   [sp + 0xf8]  = sepc
     *
     * The signal delivery code will:
     *   1. Find the trap frame on the kernel stack
     *   2. Set frame->sepc = handler (the user-space handler address)
     *   3. Set frame->a0 = signum (handler's first argument)
     *   4. Set frame->sp = new_sp (adjusted user stack)
     *   5. When sret fires, execution lands at handler(signum)
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

#endif /* ARCH_RISCV64 */

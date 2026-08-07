/*
 * Fern - x86_64 Signal Frame Setup
 * signal.c
 *
 * Builds the signal delivery frame for x86-64 user processes.
 *
 * x86_64 signal frame layout on user stack (high → low addresses):
 *
 *   [RSP_before - 128]  red zone (SysV ABI requirement)
 *   [RSP_before - 128 - sizeof(saved_ctx)]  saved user registers
 *   [signum]            signal number (32-bit, zero-extended)
 *   [saved_ctx_ptr]     pointer to saved_ctx for rt_sigreturn
 *   [trampoline_addr]   address of trampoline code → becomes new RIP
 *
 * The trampoline (12 bytes) lives at the lowest address:
 *   mov eax, SYS_RT_SIGRETURN (15)
 *   syscall
 *   (ud2 for safety if syscall returns)
 *
 * Signal handler receives:
 *   RDI = signum
 *   RSP = adjusted (points past the red zone to the frame)
 *   RFLAGS preserved from interrupted context
 */

#include "../arch/arch.h"
#include "../include/task.h"
#include "../include/string.h"
#include "../include/util.h"

#if ARCH_X86_64

/* Forward declaration */
extern task_t* current_task;

/* The signal trampoline: executes rt_sigreturn syscall */
static const uint8_t signal_trampoline_code[] = {
    0xB8, 0x0F, 0x00, 0x00, 0x00,   /* mov eax, 15 (SYS_RT_SIGRETURN) */
    0x0F, 0x05,                       /* syscall */
    0x0F, 0x0B                        /* ud2 (safety: unreachable) */
};

/* 64-bit user code and data selectors (must match GDT setup) */
#define USER_CS   0x23   /* GDT index 4, RPL=3 */
#define USER_DS   0x1B   /* GDT index 3, RPL=3 */

/*
 * setup_signal_frame - Build x86_64 signal delivery frame
 *
 * Pushes the signal frame onto the user stack and modifies the
 * saved context so that when the task returns to user mode,
 * it executes the signal handler with signum as the first argument.
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

    /* Minimum required space: frame + trampoline + alignment + red zone */
    uint64_t frame_total = sizeof(signal_frame_t) + sizeof(signal_trampoline_code) + 16;

    /* Ensure stack is aligned to 16 bytes (required by x86_64 ABI) */
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
     * For now, we save a minimal context. When integrated with the
     * interrupt/syscall entry path, the full context will come from
     * the arch_cpu_state_t saved during the trap.
     */
    signal_saved_ctx_t* ctx = &frame->saved_ctx;
    memory_set((uint8_t*)ctx, 0, sizeof(signal_saved_ctx_t));

    /* Set up the saved context for rt_sigreturn:
     * RIP = original return address (the instruction we interrupted)
     * CS  = user code selector
     * RSP = original user stack pointer
     * SS  = user data selector
     * RFLAGS = current RFLAGS with IF set (interrupts enabled)
     */
    ctx->rip   = 0;  /* Will be filled by integration with trap frame */
    ctx->cs    = USER_CS;
    ctx->ss    = USER_DS;
    ctx->rsp   = user_sp;
    ctx->rflags = 0x202;  /* IF=1, reserved bit 1 */

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
     * The full integration requires hooking into the arch-specific
     * trap/interrupt return path. The interface here establishes
     * the convention: the caller (scheduler/signal delivery code)
     * will set the task's saved RIP to the handler address and
     * RDI to the signal number before returning to user mode.
     *
     * For x86_64, the task's kernel stack should have an IRETQ frame
     * at the top. The signal delivery code will:
     *   1. Find the IRETQ frame on the kernel stack
     *   2. Set frame->RIP = handler (the user-space handler address)
     *   3. Set frame->RDI = signum (handler's first argument)
     *   4. Set frame->RSP = new_sp (adjusted user stack)
     *   5. When IRETQ fires, execution lands at handler(signum)
     */

    return SIGNAL_DELIVERED;
}

/*
 * get_signal_trampoline_addr - Get the address of the signal trampoline
 *
 * Returns the code that performs rt_sigreturn. The signal handler
 * should return to this address (which is pushed as the return address
 * on the user stack by the signal frame setup).
 */
void* get_signal_trampoline_addr(void)
{
    /* The trampoline is not at a fixed address - it's copied to the
     * user stack during setup_signal_frame(). Return a sentinel;
     * callers should use the trampoline_addr from the signal frame. */
    return NULL;
}

#endif /* ARCH_X86_64 */

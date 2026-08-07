/*
 * fault.c - RISC-V 64-bit fault handler implementation for Forest OS
 *
 * Handles page faults, illegal instructions, access faults, and
 * environment calls (ecalls from U-mode → syscalls).
 *
 * Called from trap.S after the register context is saved.
 *
 * trap.S dispatches to these C functions:
 *   riscv64_exception_handler(frame, scause, sepc, stval) — for exceptions
 *   riscv64_irq_handler(frame, irq_code)                  — for interrupts
 */

#include "fault.h"
#include "mmu.h"
#include "../arch/vmm.h"
#include "../arch/task.h"
#include "../arch/uart.h"
#include <stdint.h>

/* Forward declaration from trap.S / syscall.c */
extern void riscv64_syscall_handle(uint64_t *frame);

/* -----------------------------------------------------------------------
 * Exception handler (called from trap.S for non-ecall exceptions)
 * --------------------------------------------------------------------- */

void riscv64_exception_handler(uint64_t *frame, uint64_t scause,
                               uint64_t sepc, uint64_t stval)
{
    uint64_t exc_code = scause & SCAUSE_CODE_MASK;

    switch (exc_code) {
    case RISCV_EXC_INST_PAGE_FAULT:
    case RISCV_EXC_LOAD_PAGE_FAULT:
    case RISCV_EXC_STORE_PAGE_FAULT:
        riscv64_uart_printf("[FAULT] Page fault: scause=%lu sepc=0x%lx "
                            "stval=0x%lx\n", scause, sepc, stval);
        /* TODO: demand paging / SIGSEGV delivery */
        break;

    case RISCV_EXC_INST_ACCESS_FAULT:
    case RISCV_EXC_LOAD_ACCESS_FAULT:
    case RISCV_EXC_STORE_ACCESS_FAULT:
        riscv64_uart_printf("[FAULT] Access fault: scause=%lu sepc=0x%lx "
                            "stval=0x%lx\n", scause, sepc, stval);
        break;

    case RISCV_EXC_ILLEGAL_INST:
        riscv64_uart_printf("[FAULT] Illegal instruction at sepc=0x%lx "
                            "stval=0x%lx\n", sepc, stval);
        break;

    case RISCV_EXC_BREAKPOINT:
        riscv64_uart_printf("[FAULT] Breakpoint at sepc=0x%lx\n", sepc);
        break;

    case RISCV_EXC_INST_MISALIGNED:
    case RISCV_EXC_LOAD_MISALIGNED:
    case RISCV_EXC_STORE_MISALIGNED:
        riscv64_uart_printf("[FAULT] Alignment fault: scause=%lu sepc=0x%lx "
                            "stval=0x%lx\n", scause, sepc, stval);
        break;

    default:
        riscv64_uart_printf("[FAULT] Unhandled exception: scause=%lu "
                            "sepc=0x%lx stval=0x%lx\n",
                            scause, sepc, stval);
        break;
    }

    (void)frame;
}

/* -----------------------------------------------------------------------
 * Interrupt handler (called from trap.S for interrupts)
 * --------------------------------------------------------------------- */

void riscv64_irq_handler(uint64_t *frame, uint64_t irq_code)
{
    switch (irq_code) {
    case RISCV_IRQ_S_TIMER: {
        /* Timer tick — call the cross-arch timer handler */
        extern void timer_handler(void);
        timer_handler();
        break;
    }

    case RISCV_IRQ_S_EXT: {
        /* External interrupt — forward to PLIC */
        extern void riscv64_plic_handle_irq(void);
        riscv64_plic_handle_irq();
        break;
    }

    case RISCV_IRQ_S_SOFT:
        /* Software interrupt (IPI) */
        break;

    default:
        riscv64_uart_printf("[FAULT] Unknown interrupt: irq_code=%lu\n",
                            irq_code);
        break;
    }

    (void)frame;
}

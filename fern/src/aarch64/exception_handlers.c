/*
 * Fern - AArch64 Exception (C) Handlers
 *
 * Called from the assembly vector stubs in exceptions.S.
 * Each handler receives the ESR_EL1 and FAR_EL1 values decoded by the
 * CPU and a pointer to the saved register frame on the kernel stack.
 *
 * ESR_EL1 layout:
 *   [31:26] EC  - Exception Class  (why the exception happened)
 *   [25]    IL  - Instruction Length (0 = 16-bit, 1 = 32-bit)
 *   [24:0]  ISS - Instruction Specific Syndrome
 *
 * Common EC values:
 *   0x01  WFI/WFE trapped
 *   0x07  SMC/HVC trapped
 *   0x0F  SVC from AArch32
 *   0x15  SVC from AArch64
 *   0x20  Instruction Abort (lower EL)
 *   0x21  Instruction Abort (same EL)
 *   0x24  Data Abort (lower EL)
 *   0x25  Data Abort (same EL)
 *   0x26  Stack Alignment Fault
 *   0x2C  FP/SIMD exception
 *   0x3C  BRK instruction
 */

#include "exception_handlers.h"
#include "uart.h"
#include "gic.h"
#include "timer.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* ESR_EL1 field extraction                                            */
/* ------------------------------------------------------------------ */

#define ESR_EC(esr)     (((esr) >> 26) & 0x3FU)
#define ESR_IL(esr)     (((esr) >> 25) & 0x1U)
#define ESR_ISS(esr)    ((esr) & 0x1FFFFFFU)

/* Data Abort ISS sub-fields */
#define ESR_ISS_DFSC(iss)   ((iss) & 0x3FU)    /* Data Fault Status Code */
#define ESR_ISS_WNR(iss)    (((iss) >> 6) & 1U) /* Write not Read */
#define ESR_ISS_EA(iss)     (((iss) >> 9) & 1U) /* External Abort */

/* ------------------------------------------------------------------ */
/* Register frame (matches SAVE_REGS layout in exceptions.S)           */
/* ------------------------------------------------------------------ */
struct exception_frame {
    uint64_t x[31];     /* x0–x30                                    */
    uint64_t sp_el0;    /* saved EL0 stack pointer                   */
    uint64_t elr_el1;   /* return address                            */
    uint64_t spsr_el1;  /* saved PSTATE                              */
};

/* ------------------------------------------------------------------ */
/* Forward declarations for platform-specific IRQ routing             */
/* ------------------------------------------------------------------ */

/* Timer PPI INTID (virtual timer = 27) */
#define VTIMER_INTID    27

/*
 * Registered IRQ handler table.
 * Up to 256 SPIs supported; indexed by INTID.
 */
#define MAX_IRQ_HANDLERS    256
void (*irq_handlers[MAX_IRQ_HANDLERS])(uint32_t intid) = { 0 };

/* ------------------------------------------------------------------ */
/* irq_handler_register - Register a C handler for a given INTID      */
/* ------------------------------------------------------------------ */
void irq_handler_register(uint32_t intid, void (*handler)(uint32_t))
{
    if (intid < MAX_IRQ_HANDLERS) {
        irq_handlers[intid] = handler;
    }
}

/* ------------------------------------------------------------------ */
/* handle_sync_exception                                               */
/* Called for synchronous exceptions (data/instruction aborts, SVC,   */
/* undefined instructions, etc.)                                       */
/* ------------------------------------------------------------------ */
void handle_sync_exception(uint64_t esr, uint64_t far, void *frame_ptr)
{
    struct exception_frame *frame = (struct exception_frame *)frame_ptr;
    uint32_t ec  = ESR_EC(esr);
    uint32_t iss = ESR_ISS(esr);

    switch (ec) {
    case 0x15:
        /* SVC from AArch64 – handled in exceptions.S before reaching here */
        break;

    case 0x20: /* Instruction Abort from lower EL */
    case 0x21: /* Instruction Abort from current EL */
        uart_printf("[exc] Instruction Abort: ELR=%016lx FAR=%016lx ESR=%08lx\n",
                    frame->elr_el1, far, esr);
        break;

    case 0x24: /* Data Abort from lower EL */
    case 0x25: /* Data Abort from current EL */
        uart_printf("[exc] Data Abort (%s): ELR=%016lx FAR=%016lx DFSC=0x%x\n",
                    ESR_ISS_WNR(iss) ? "write" : "read",
                    frame->elr_el1, far, ESR_ISS_DFSC(iss));
        break;

    case 0x26:
        uart_printf("[exc] Stack Pointer Alignment Fault: ELR=%016lx\n",
                    frame->elr_el1);
        break;

    case 0x2C:
        uart_printf("[exc] FP/SIMD exception: ELR=%016lx ESR=%08lx\n",
                    frame->elr_el1, esr);
        break;

    case 0x3C:
        uart_printf("[exc] BRK instruction: ELR=%016lx ISS=0x%x\n",
                    frame->elr_el1, iss);
        /* Advance PC past the BRK to allow resumption */
        frame->elr_el1 += 4;
        return;

    case 0x01:
        /* WFI/WFE trapped from lower EL – just return */
        frame->elr_el1 += 4;
        return;

    default:
        uart_printf("[exc] Unhandled sync exception EC=0x%x ISS=0x%x ELR=%016lx\n",
                    ec, iss, frame->elr_el1);
        break;
    }

    /*
     * For fatal exceptions, halt.  In a real OS this would trigger
     * process termination or a kernel panic with stack dump.
     */
    if (ec != 0x3C && ec != 0x01 && ec != 0x15) {
        uart_printf("[exc] FATAL: halting.\n");
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
}

/* ------------------------------------------------------------------ */
/* handle_irq                                                          */
/* Called with an already-acknowledged INTID from aarch64_irq_handler. */
/* Dispatches to the registered handler table.  Does NOT acknowledge   */
/* or EOI — the caller (aarch64_irq_handler) owns that cycle.         */
/* ------------------------------------------------------------------ */
void handle_irq(void *frame_ptr)
{
    (void)frame_ptr;
    /* Intentionally empty: dispatch is handled by the caller.
     * Registered handlers are invoked directly from aarch64_irq_handler
     * via the irq_handler_table. This function is retained for ABI
     * compatibility with vectors.S weak stubs. */
}

/* ------------------------------------------------------------------ */
/* handle_fiq                                                          */
/* FIQ is not used by Fern (Group-0 secure interrupts).          */
/* ------------------------------------------------------------------ */
void handle_fiq(void *frame_ptr)
{
    (void)frame_ptr;
    uart_puts("[exc] FIQ received (unexpected)\n");
    /* EOI Group-0 via ICC_EOIR0_EL1 if needed; for now just return */
}

/* ------------------------------------------------------------------ */
/* handle_serror                                                        */
/* SError (System Error) – typically caused by asynchronous external   */
/* aborts such as uncorrectable memory errors.                          */
/* ------------------------------------------------------------------ */
void handle_serror(uint64_t esr, void *frame_ptr)
{
    struct exception_frame *frame = (struct exception_frame *)frame_ptr;
    uart_printf("[exc] SError: ESR=%08lx ELR=%016lx\n", esr, frame->elr_el1);
    uart_puts("[exc] SError is fatal. System halted.\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

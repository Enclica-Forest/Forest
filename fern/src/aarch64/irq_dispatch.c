/*
 * Fern - AArch64 IRQ dispatch
 *
 * Provides the aarch64_irq_handler() symbol that overrides the weak
 * stub in vectors.S.  Vectors.S calls this for every IRQ exception
 * (el1_spx_irq, el0_aarch64_irq, etc.) after saving the register frame.
 *
 * This is the single acknowledge/dispatch/EOI entry point.
 *
 * Flow:
 *   1. Read ICC_IAR1_EL1 to acknowledge the interrupt and obtain the INTID.
 *   2. Dispatch to the appropriate handler based on INTID.
 *   3. Write ICC_EOIR1_EL1 to signal End-Of-Interrupt to the GIC.
 */

#include "gic.h"
#include "timer.h"
#include "exception_handlers.h"
#include "uart.h"
#include <stdint.h>

/* INTID constants */
#define PHYS_TIMER_INTID  30U
#define VIRT_TIMER_INTID  27U

#define MAX_IRQ_HANDLERS 256

/*
 * Registered handler table — populated by irq_handler_register().
 * Indexed by INTID.  Set by exception_handlers.c at init time.
 */
extern void (*irq_handlers[MAX_IRQ_HANDLERS])(uint32_t intid);

/*
 * aarch64_irq_handler - Top-level IRQ handler called from vectors.S.
 *
 * @frame: Pointer to the saved register frame on the kernel stack.
 *
 * Overrides the weak stub in vectors.S.  This function owns the full
 * interrupt lifecycle: acknowledge → dispatch → EOI.
 */
void aarch64_irq_handler(void *frame)
{
    /* Acknowledge: read ICC_IAR1_EL1, get INTID */
    uint32_t intid = (uint32_t)icc_iar1_el1_read();

    if (intid == GIC_INTID_SPURIOUS) {
        return;
    }

    if (intid == PHYS_TIMER_INTID || intid == VIRT_TIMER_INTID) {
        /* Timer tick: advance compare register and increment software counter */
        timer_irq_handler();
    } else if (intid < MAX_IRQ_HANDLERS && irq_handlers[intid]) {
        /* Registered handler for this INTID */
        irq_handlers[intid](intid);
    } else {
        uart_printf("[irq] Unhandled INTID %u\n", intid);
    }

    /* Signal End-Of-Interrupt */
    icc_eoir1_el1_write((uint64_t)intid);
}

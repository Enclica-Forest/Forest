/*
 * irq.c - IRQ dispatch layer for Fern ARM32
 *
 * Provides a simple per-IRQ handler table and a C-level dispatcher that is
 * called from the assembly IRQ stub in exceptions.S:
 *
 *   irq_handler (assembly):
 *     sub    lr, lr, #4          ; adjust LR for IRQ exception
 *     SAVE_CONTEXT               ; push r0-r12, lr, SPSR onto IRQ stack
 *     mov    r0, sp              ; r0 = &saved_regs (struct arm_regs_t *)
 *     bl     irq_dispatch        ; call this function
 *     RESTORE_CONTEXT_AND_RETURN ; pop and exception-return
 *
 * irq_dispatch() does NOT need the "interrupt" attribute because exceptions.S
 * has already saved and will restore the full context.  The C function is a
 * plain function call inside the exception frame.
 *
 * Design:
 *   - Static table of IRQ_MAX_IRQS function pointers, zero-initialised by BSS.
 *   - irq_register_handler() installs/removes handlers at runtime.
 *   - irq_dispatch() acknowledges the GIC, dispatches, then sends EOI.
 *   - Spurious interrupts (GIC ID 1023) are discarded without an EOI.
 *   - Unhandled (non-spurious) IRQs are reported via early_puts() and
 *     counted in a per-IRQ stray counter for debugging.
 */

#include "irq.h"
#include "gic.h"

/* -------------------------------------------------------------------------
 * Forward declaration of the UART early output helper from boot.S.
 * Only used for diagnostic messages; avoids pulling in a full UART driver.
 * --------------------------------------------------------------------- */
extern void early_puts(const char *s);

/* -------------------------------------------------------------------------
 * Dispatch table
 *
 * Indexed by interrupt number.  NULL means "no handler registered".
 * The table lives in BSS and is zeroed by the reset handler before
 * kernel_main() runs, so irq_init() is an optional belt-and-suspenders call.
 * --------------------------------------------------------------------- */
static irq_handler_t irq_table[IRQ_MAX_IRQS];

/* -------------------------------------------------------------------------
 * Stray interrupt counter (per-IRQ, wraps at 255)
 * --------------------------------------------------------------------- */
static uint8_t irq_stray_count[IRQ_MAX_IRQS];

/* -------------------------------------------------------------------------
 * Simple decimal-to-string helper for diagnostic output.
 * Writes up to 10 digits + NUL into @buf (must be at least 11 bytes).
 * Returns a pointer to the first digit character inside @buf.
 * --------------------------------------------------------------------- */
static const char *uint_to_dec(uint32_t n, char *buf, int buf_size)
{
    char *p = buf + buf_size - 1;
    *p = '\0';
    if (n == 0) {
        *--p = '0';
        return p;
    }
    while (n > 0 && p > buf) {
        *--p = (char)('0' + (n % 10u));
        n /= 10u;
    }
    return p;
}

/* -------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/*
 * irq_init - Zero the dispatch table.
 *
 * The BSS clear in boot.S already zeroes these arrays, so this is a no-op
 * in normal operation.  Provided so that code can call it explicitly if the
 * table needs to be reset at runtime (e.g., after a soft reboot).
 */
void irq_init(void)
{
    for (uint32_t i = 0; i < IRQ_MAX_IRQS; i++) {
        irq_table[i]       = (irq_handler_t)0;
        irq_stray_count[i] = 0u;
    }
}

/*
 * irq_register_handler - Install (or remove) an IRQ handler.
 */
void irq_register_handler(uint32_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_MAX_IRQS)
        return;

    irq_table[irq] = handler;
}

/*
 * irq_dispatch - C-level top-of-IRQ dispatcher.
 *
 * Called by the assembly stub in exceptions.S after the full CPU context
 * has been saved.  On return the assembly stub restores context and performs
 * an exception return (MOVS pc, lr or ldmfd …, {pc}^).
 *
 * Protocol:
 *   1. Read GICC_IAR – this simultaneously acknowledges the interrupt and
 *      causes the GIC to sample the next pending interrupt.
 *   2. Extract the interrupt ID from IAR[9:0].
 *   3. If spurious (ID == 1023), return immediately without EOI.
 *   4. Call the registered handler, or log a stray-IRQ warning.
 *   5. Write GICC_EOIR with the original IAR value (preserves the source
 *      CPU field for SGIs) to signal End-of-Interrupt.
 */
void irq_dispatch(void)
{
    /* Step 1: acknowledge and get IAR. */
    uint32_t iar = gic_acknowledge();

    /* Step 3: discard spurious interrupts. */
    if (gic_is_spurious(iar))
        return;

    /* Step 2: extract the interrupt ID. */
    uint32_t irq_id = iar & GIC_IAR_ID_MASK;

    /* Step 4: dispatch to the registered handler, or report stray IRQ. */
    if (irq_id < IRQ_MAX_IRQS && irq_table[irq_id] != (irq_handler_t)0) {
        irq_table[irq_id](irq_id);
    } else {
        /* Increment stray counter (saturate at 255). */
        if (irq_id < IRQ_MAX_IRQS) {
            if (irq_stray_count[irq_id] < 0xFFu)
                irq_stray_count[irq_id]++;
        }

        /* Emit a diagnostic message over the early UART. */
        char num_buf[11];
        early_puts("[IRQ] stray interrupt: id=");
        early_puts(uint_to_dec(irq_id, num_buf, (int)sizeof(num_buf)));
        early_puts("\r\n");
    }

    /* Step 5: End-of-Interrupt. */
    gic_end_of_interrupt(iar);
}

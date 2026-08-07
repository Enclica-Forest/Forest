#ifndef ARM32_IRQ_H
#define ARM32_IRQ_H

#include <stdint.h>

/* Maximum number of IRQ lines tracked by the dispatch table.
 * Matches GIC_MAX_IRQS in gic.h; defined independently to avoid a header
 * dependency in files that only include irq.h. */
#define IRQ_MAX_IRQS    1020u

/**
 * irq_handler_t - prototype for a C-level interrupt service routine.
 *
 * @irq: the interrupt number that fired (extracted from GICC_IAR)
 *
 * The handler is called from irq_dispatch() with IRQs still disabled in
 * CPSR (the exception entry left them masked).  The handler must not call
 * any code that re-enables IRQs unless it can tolerate nested interrupts.
 */
typedef void (*irq_handler_t)(uint32_t irq);

/**
 * irq_init - Initialise the dispatch table.
 *
 * Clears all handler slots to NULL.  Must be called once before any call to
 * irq_register_handler() or irq_dispatch().
 */
void irq_init(void);

/**
 * irq_register_handler - Register a C handler for interrupt @irq.
 *
 * @irq:     interrupt number (0 – IRQ_MAX_IRQS-1)
 * @handler: function to call when @irq fires; pass NULL to unregister
 *
 * Overwrites any previously registered handler for @irq.  The function is
 * not thread-safe; register all handlers before enabling the GIC.
 */
void irq_register_handler(uint32_t irq, irq_handler_t handler);

/**
 * irq_dispatch - Top-level IRQ dispatcher, called from the IRQ exception stub.
 *
 * Acknowledges the GIC, extracts the interrupt number, calls the registered
 * handler (or logs an unhandled IRQ warning), then signals End-of-Interrupt.
 * Spurious interrupts (ID 1023) are silently ignored.
 *
 * This function is declared with the "interrupt" attribute in the .c file so
 * that the compiler emits a correct exception return sequence when called
 * from a non-assembly stub (the assembly in exceptions.S handles the
 * context save/restore itself, so the C function does NOT use the attribute
 * – see implementation notes in irq.c).
 */
void irq_dispatch(void);

#endif /* ARM32_IRQ_H */

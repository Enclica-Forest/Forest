/*
 * Fern - Cross-Architecture Interrupt Interface
 * interrupt.h
 *
 * Unified interrupt management API across all supported architectures:
 *   x86:     PIC/APIC + IDT
 *   ARM32:   GIC + exception vectors
 *   AArch64: GICv3 + VBAR_EL1
 *   RISC-V:  PLIC + stvec
 *
 * This header provides the common interrupt registration, masking,
 * and dispatch interface used by the cross-arch subsystems.
 */

#ifndef FOREST_ARCH_INTERRUPT_H
#define FOREST_ARCH_INTERRUPT_H

#include "arch.h"
#include <stdint.h>
#include <stdbool.h>

/* Maximum number of interrupt vectors supported across all architectures */
#define ARCH_MAX_INTERRUPTS     256

/* Special IRQ numbers shared across architectures */
#define ARCH_IRQ_TIMER          0   /* Architecture maps to its own timer IRQ */
#define ARCH_IRQ_UART           1
#define ARCH_IRQ_KEYBOARD       2
#define ARCH_IRQ_MOUSE          3
#define ARCH_IRQASCADE          4
#define ARCH_IRQ_SPURIOUS       ARCH_MAX_INTERRUPTS - 1

/* Interrupt handler function type */
typedef void (*arch_irq_handler_t)(void *context);

/**
 * arch_interrupt_init - Initialize the architecture-specific interrupt controller.
 *
 * Sets up IDT/VBAR/stvec, enables the interrupt controller, and
 * registers the default handlers (timer, UART, etc.).
 * Must be called once during early boot.
 */
void arch_interrupt_init(void);

/**
 * arch_interrupt_register - Register a handler for a specific IRQ number.
 *
 * @irq:     Interrupt number (architecture-specific mapping).
 * @handler: Function to call when the IRQ fires.
 * @name:    Human-readable name for debug (may be NULL).
 *
 * Returns 0 on success, -1 if the IRQ is already registered.
 */
int arch_interrupt_register(uint32_t irq, arch_irq_handler_t handler,
                            const char *name);

/**
 * arch_interrupt_unregister - Remove a handler for a specific IRQ.
 *
 * @irq: Interrupt number to unregister.
 */
void arch_interrupt_unregister(uint32_t irq);

/**
 * arch_interrupt_enable - Enable (unmask) a specific IRQ.
 *
 * @irq: Interrupt number to enable.
 */
void arch_interrupt_enable(uint32_t irq);

/**
 * arch_interrupt_disable - Disable (mask) a specific IRQ.
 *
 * @irq: Interrupt number to disable.
 */
void arch_interrupt_disable(uint32_t irq);

/**
 * arch_interrupt_acknowledge - Acknowledge / EOI a specific IRQ.
 *
 * Must be called after handling the interrupt to allow the controller
 * to deliver the next interrupt.
 *
 * @irq: Interrupt number that was serviced.
 */
void arch_interrupt_acknowledge(uint32_t irq);

/**
 * arch_interrupt_get_active_irq - Return the currently executing IRQ number.
 *
 * Returns the IRQ number of the interrupt being serviced, or -1 if
 * no interrupt is active.
 */
int32_t arch_interrupt_get_active_irq(void);

/**
 * arch_interrupt_in_context - Check if called from an interrupt context.
 *
 * Returns true if the current execution context is an interrupt handler.
 */
bool arch_interrupt_in_context(void);

#endif /* FOREST_ARCH_INTERRUPT_H */

/*
 * Fern - AArch64 Exception Handler Declarations
 *
 * These C functions are called from the assembly stubs in exceptions.S.
 * Arguments follow the AArch64 ABI (x0, x1, x2, ...).
 */
#ifndef AARCH64_EXCEPTION_HANDLERS_H
#define AARCH64_EXCEPTION_HANDLERS_H

#include <stdint.h>

/**
 * handle_sync_exception - Synchronous exception (data/instruction abort,
 *                          BRK, alignment fault, etc.)
 * @esr:      ESR_EL1 value (Exception Syndrome Register).
 * @far:      FAR_EL1 value (Fault Address Register).
 * @frame:    Pointer to the saved register frame on the kernel stack.
 */
void handle_sync_exception(uint64_t esr, uint64_t far, void *frame);

/**
 * handle_irq - IRQ handler (Group-1 GICv3 interrupts).
 * @frame: Pointer to saved register frame.
 */
void handle_irq(void *frame);

/**
 * handle_fiq - FIQ handler (Group-0 / secure FIQ – normally unused).
 * @frame: Pointer to saved register frame.
 */
void handle_fiq(void *frame);

/**
 * handle_serror - SError (asynchronous system error) handler.
 * @esr:   ESR_EL1 value.
 * @frame: Pointer to saved register frame.
 */
void handle_serror(uint64_t esr, void *frame);

/**
 * irq_handler_register - Register a C function to handle a specific INTID.
 * @intid:   GIC interrupt ID (0-255).
 * @handler: Function to call when the interrupt fires.
 */
void irq_handler_register(uint32_t intid, void (*handler)(uint32_t intid));

#endif /* AARCH64_EXCEPTION_HANDLERS_H */

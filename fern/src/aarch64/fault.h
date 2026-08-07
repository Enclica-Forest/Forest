/*
 * Forest OS - AArch64 Fault Handler Declarations
 *
 * Data/instruction abort and synchronous exception handlers for AArch64.
 * Called from the exception vector stubs in vectors.S.
 */
#ifndef AARCH64_FAULT_H
#define AARCH64_FAULT_H

#include <stdint.h>

/**
 * aarch64_sync_handler - Generic synchronous exception dispatcher.
 * @esr_el1: ESR_EL1 value (Exception Syndrome Register).
 * @far_el1: FAR_EL1 value (Fault Address Register).
 *
 * Decodes ESR_EL1.EC and dispatches to the appropriate handler:
 *   0x15  SVC (should already be handled in vectors.S fast path)
 *   0x20  Instruction abort from lower EL
 *   0x21  Instruction abort from current EL
 *   0x24  Data abort from lower EL
 *   0x25  Data abort from current EL
 *   0x00  Unknown reason
 *   0x02  Undefined instruction
 */
void aarch64_sync_handler(uint64_t esr_el1, uint64_t far_el1);

/**
 * aarch64_data_abort_handler - Handle data abort exceptions.
 * @far_el1: Faulting virtual address (FAR_EL1).
 * @esr_el1: Exception syndrome (ESR_EL1).
 *
 * Decodes ISS.FSC to determine fault type:
 *   Translation fault (0x04-0x07): user space → demand paging, kernel → panic
 *   Permission fault  (0x0C-0x0E): kill faulting process (SIGSEGV)
 */
void aarch64_data_abort_handler(uint64_t far_el1, uint64_t esr_el1);

/**
 * aarch64_instruction_abort_handler - Handle instruction abort exceptions.
 * @far_el1: Faulting virtual address (FAR_EL1).
 * @esr_el1: Exception syndrome (ESR_EL1).
 *
 * Similar to data abort but for instruction fetch faults.
 */
void aarch64_instruction_abort_handler(uint64_t far_el1, uint64_t esr_el1);

#endif /* AARCH64_FAULT_H */

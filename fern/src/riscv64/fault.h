/*
 * fault.h - RISC-V 64-bit fault handler interface for Forest OS
 *
 * Exception codes from the RISC-V Privileged Architecture Specification:
 *   0  - Instruction address misaligned
 *   1  - Instruction access fault
 *   2  - Illegal instruction
 *   3  - Breakpoint
 *   4  - Load address misaligned
 *   5  - Load access fault
 *   6  - Store/AMO address misaligned
 *   7  - Store/AMO access fault
 *   8  - Environment call from U-mode
 *   9  - Environment call from S-mode
 *   12 - Instruction page fault
 *   13 - Load page fault
 *   15 - Store/AMO page fault
 *
 * Interrupt codes (bit 63 set):
 *   1  - Supervisor software interrupt
 *   3  - Supervisor timer interrupt
 *   9  - Supervisor external interrupt
 */

#ifndef RISCV64_FAULT_H
#define RISCV64_FAULT_H

#include <stdint.h>

/* SCAUSE bit layout */
#define SCAUSE_INTERRUPT_BIT    (1ULL << 63)
#define SCAUSE_CODE_MASK        0x7FFFFFFFFFFFFFFFULL

/* Exception codes (scause without interrupt bit) */
#define RISCV_EXC_INST_MISALIGNED      0
#define RISCV_EXC_INST_ACCESS_FAULT     1
#define RISCV_EXC_ILLEGAL_INST          2
#define RISCV_EXC_BREAKPOINT            3
#define RISCV_EXC_LOAD_MISALIGNED       4
#define RISCV_EXC_LOAD_ACCESS_FAULT     5
#define RISCV_EXC_STORE_MISALIGNED      6
#define RISCV_EXC_STORE_ACCESS_FAULT    7
#define RISCV_EXC_U_ECALL               8
#define RISCV_EXC_S_ECALL               9
#define RISCV_EXC_INST_PAGE_FAULT       12
#define RISCV_EXC_LOAD_PAGE_FAULT       13
#define RISCV_EXC_STORE_PAGE_FAULT      15

/* Interrupt codes (scause with bit 63 set) */
#define RISCV_IRQ_S_SOFT         1
#define RISCV_IRQ_S_TIMER        3
#define RISCV_IRQ_S_EXT          9

/* Virtual address boundary between user and kernel space (Sv39) */
#define RISCV_USER_SPACE_END     0x0000003FFFFFFFFFULL

#endif /* RISCV64_FAULT_H */

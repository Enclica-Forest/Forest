/*
 * Forest OS - AArch64 Data/Instruction Abort Handlers
 *
 * Handles data and instruction abort exceptions with VMM integration.
 * ESR_EL1 layout:
 *   [31:26] EC  - Exception Class
 *   [25]    IL  - Instruction Length
 *   [24:0]  ISS - Instruction Specific Syndrome
 *
 * For Data/Instruction Aborts, ISS contains:
 *   [5:0]  DFSC/IFSC - Data/Instruction Fault Status Code
 *   [6]    WNR       - Write not Read (data abort only)
 *   [9]    EA        - External Abort
 */

#include "fault.h"
#include "uart.h"
#include "mmu.h"
#include "include/task.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* ESR_EL1 field extraction                                           */
/* ------------------------------------------------------------------ */
#define ESR_EC(esr)     (((esr) >> 26) & 0x3FUL)
#define ESR_ISS(esr)    ((esr) & 0x1FFFFFFUL)

/* ISS sub-fields for Data/Instruction Aborts */
#define ISS_DFSC(iss)   ((iss) & 0x3FUL)        /* Fault Status Code   */
#define ISS_WNR(iss)    (((iss) >> 6) & 1UL)    /* Write not Read      */

/* ------------------------------------------------------------------ */
/* Exception Class (EC) values                                         */
/* ------------------------------------------------------------------ */
#define EC_SVC_AARCH64          0x15
#define EC_INSN_ABORT_LOWER     0x20
#define EC_INSN_ABORT_CURRENT   0x21
#define EC_DATA_ABORT_LOWER     0x24
#define EC_DATA_ABORT_CURRENT   0x25
#define EC_UNKNOWN              0x00
#define EC_UNDEFINED            0x02

/* ------------------------------------------------------------------ */
/* Fault Status Code (DFSC/IFSC) values                                */
/* Translation faults: level of the translation walk that failed       */
/* ------------------------------------------------------------------ */
#define FSC_TRANS_L0    0x04
#define FSC_TRANS_L1    0x05
#define FSC_TRANS_L2    0x06
#define FSC_TRANS_L3    0x07

/* Permission faults */
#define FSC_PERM_L1     0x0C
#define FSC_PERM_L2     0x0D
#define FSC_PERM_L3     0x0E

/* ------------------------------------------------------------------ */
/* Helper predicates                                                   */
/* ------------------------------------------------------------------ */
static inline int is_translation_fault(uint64_t fsc)
{
    return fsc >= FSC_TRANS_L0 && fsc <= FSC_TRANS_L3;
}

static inline int is_permission_fault(uint64_t fsc)
{
    return fsc >= FSC_PERM_L1 && fsc <= FSC_PERM_L3;
}

static inline int is_user_addr(uint64_t addr)
{
    return addr < AARCH64_USER_VA_END;
}

/* ------------------------------------------------------------------ */
/* Extern: current running task (from task.c)                          */
/* ------------------------------------------------------------------ */
extern task_t *current_task;

/* ------------------------------------------------------------------ */
/* aarch64_data_abort_handler                                         */
/* ------------------------------------------------------------------ */
void aarch64_data_abort_handler(uint64_t far_el1, uint64_t esr_el1)
{
    uint64_t ec   = ESR_EC(esr_el1);
    uint64_t iss  = ESR_ISS(esr_el1);
    uint64_t dfsc = ISS_DFSC(iss);
    int write     = ISS_WNR(iss);
    int from_lower = (ec == EC_DATA_ABORT_LOWER);

    uart_printf("[fault] Data Abort (%s from %s EL):\n",
                write ? "write" : "read",
                from_lower ? "lower" : "current");
    uart_printf("  FAR_EL1 = 0x%016lx\n", far_el1);
    uart_printf("  ESR_EL1 = 0x%016lx (EC=0x%lx, DFSC=0x%lx)\n",
                esr_el1, ec, dfsc);

    if (current_task) {
        uart_printf("  Process: %s (pid=%lu)\n",
                    current_task->name,
                    (unsigned long)current_task->id);
    }

    if (is_translation_fault(dfsc)) {
        if (from_lower && is_user_addr(far_el1)) {
            /*
             * User-space translation fault: attempt demand paging.
             * The faulting address is valid user VA space but has no
             * mapping.  In a full implementation this would allocate a
             * physical frame, map it into the process page table, and
             * return to retry the instruction.
             *
             * For now, send SIGSEGV to the faulting process.
             */
            uart_printf("  Translation fault at user VA 0x%lx "
                        "- demand paging not yet implemented\n", far_el1);
            if (current_task) {
                uart_printf("  Sending SIGSEGV to %s\n",
                            current_task->name);
                task_terminate_current(SIGSEGV);
            }
        } else {
            /*
             * Kernel-space translation fault: this is a kernel bug.
             * A mapping that the kernel expects to exist is missing.
             */
            uart_printf("  Kernel translation fault at VA 0x%lx - PANIC\n",
                        far_el1);
            for (;;) {
                __asm__ volatile("wfi");
            }
        }
    } else if (is_permission_fault(dfsc)) {
        /*
         * Permission fault: the page exists but the access type
         * (read/write/user) is not permitted.  Typically a bug in
         * the faulting process (e.g. writing to a read-only mapping).
         */
        uart_printf("  Permission fault at VA 0x%lx\n", far_el1);
        if (current_task) {
            uart_printf("  Killing %s (pid=%lu) for permission fault\n",
                        current_task->name,
                        (unsigned long)current_task->id);
            task_terminate_current(SIGSEGV);
        }
    } else {
        uart_printf("  Unhandled DFSC=0x%lx - PANIC\n", dfsc);
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
}

/* ------------------------------------------------------------------ */
/* aarch64_instruction_abort_handler                                  */
/* ------------------------------------------------------------------ */
void aarch64_instruction_abort_handler(uint64_t far_el1, uint64_t esr_el1)
{
    uint64_t ec   = ESR_EC(esr_el1);
    uint64_t iss  = ESR_ISS(esr_el1);
    uint64_t ifsc = ISS_DFSC(iss);
    int from_lower = (ec == EC_INSN_ABORT_LOWER);

    uart_printf("[fault] Instruction Abort (from %s EL):\n",
                from_lower ? "lower" : "current");
    uart_printf("  FAR_EL1 = 0x%016lx\n", far_el1);
    uart_printf("  ESR_EL1 = 0x%016lx (EC=0x%lx, IFSC=0x%lx)\n",
                esr_el1, ec, ifsc);

    if (current_task) {
        uart_printf("  Process: %s (pid=%lu)\n",
                    current_task->name,
                    (unsigned long)current_task->id);
    }

    if (is_translation_fault(ifsc)) {
        if (from_lower && is_user_addr(far_el1)) {
            uart_printf("  Translation fault at user VA 0x%lx "
                        "- demand paging not yet implemented\n", far_el1);
            if (current_task) {
                uart_printf("  Sending SIGSEGV to %s\n",
                            current_task->name);
                task_terminate_current(SIGSEGV);
            }
        } else {
            uart_printf("  Kernel instruction fault at VA 0x%lx - PANIC\n",
                        far_el1);
            for (;;) {
                __asm__ volatile("wfi");
            }
        }
    } else if (is_permission_fault(ifsc)) {
        uart_printf("  Permission fault (execute) at VA 0x%lx\n", far_el1);
        if (current_task) {
            uart_printf("  Killing %s (pid=%lu) for execute permission fault\n",
                        current_task->name,
                        (unsigned long)current_task->id);
            task_terminate_current(SIGSEGV);
        }
    } else {
        uart_printf("  Unhandled IFSC=0x%lx - PANIC\n", ifsc);
        for (;;) {
            __asm__ volatile("wfi");
        }
    }
}

/* ------------------------------------------------------------------ */
/* aarch64_sync_handler                                               */
/* ------------------------------------------------------------------ */
void aarch64_sync_handler(uint64_t esr_el1, uint64_t far_el1)
{
    uint64_t ec = ESR_EC(esr_el1);

    switch (ec) {
    case EC_SVC_AARCH64:
        /* Should have been handled in vectors.S fast path */
        uart_printf("[fault] Unexpected SVC in sync handler "
                    "(ESR=0x%lx)\n", esr_el1);
        break;

    case EC_INSN_ABORT_LOWER:
    case EC_INSN_ABORT_CURRENT:
        aarch64_instruction_abort_handler(far_el1, esr_el1);
        break;

    case EC_DATA_ABORT_LOWER:
    case EC_DATA_ABORT_CURRENT:
        aarch64_data_abort_handler(far_el1, esr_el1);
        break;

    case EC_UNKNOWN:
        uart_printf("[fault] Unknown exception: ESR=0x%016lx "
                    "FAR=0x%016lx\n", esr_el1, far_el1);
        for (;;) {
            __asm__ volatile("wfi");
        }
        break;

    case EC_UNDEFINED:
        uart_printf("[fault] Undefined instruction: ESR=0x%016lx "
                    "FAR=0x%016lx\n", esr_el1, far_el1);
        if (current_task) {
            uart_printf("  Killing %s for undefined instruction\n",
                        current_task->name);
            task_terminate_current(SIGILL);
        }
        break;

    default:
        uart_printf("[fault] Unhandled sync exception EC=0x%lx "
                    "ESR=0x%016lx FAR=0x%016lx\n",
                    ec, esr_el1, far_el1);
        for (;;) {
            __asm__ volatile("wfi");
        }
        break;
    }
}

/*
 * fault.c - ARM32 data abort / prefetch abort / undef instruction handlers
 *
 * Called from the exception stubs in exceptions.S.  Each handler receives
 * the full saved register frame and the relevant fault status/address
 * registers read from CP15.
 *
 * Current behaviour:
 *   - Decodes the DFSR/IFSR status field into a human-readable string.
 *   - Prints diagnostic info: faulting address, fault status, register PC,
 *     write-vs-read direction, and processor mode.
 *   - User-space faults (< 0x80000000): logs and halts the faulting context
 *     (real VMM demand-paging and process termination will be wired later).
 *   - Kernel-space faults (>= 0x80000000): panic — a kernel bug.
 *   - Undefined instruction: always fatal.
 *
 * Future work:
 *   - Check VMM for demand-paged mappings on user-space translation faults.
 *   - Send SIGSEGV / SIGBUS to the faulting process.
 *   - Resume execution after a successful demand-page-in.
 */

#include "arm32.h"
#include "fault.h"
#include "timer.h"

/* =========================================================================
 * DFSR/IFSR fault status code to string
 * ========================================================================= */

static const char *fault_status_str(uint32_t status)
{
    switch (status & DFSR_STATUS_MASK) {
    case FS_ADDRESS_SIZE_FAULT:     return "Address size fault";
    case FS_ALIGNMENT_FAULT:        return "Alignment fault";
    case FS_IC_CACHE_MAINT_FAULT:   return "ICache maint fault";
    case FS_TRANSLATION_FAULT_L1:   return "Translation fault (L1)";
    case FS_TRANSLATION_FAULT_L2:   return "Translation fault (L2)";
    case FS_PERMISSION_FAULT_L1:    return "Permission fault (L1)";
    case FS_PERMISSION_FAULT_L2:    return "Permission fault (L2)";
    case FS_PRECISE_EXT_ABORT_L1:   return "Precise ext abort (L1)";
    case FS_PRECISE_EXT_ABORT_L2:   return "Precise ext abort (L2)";
    case FS_DOMAIN_FAULT_L1:        return "Domain fault (L1)";
    case FS_DOMAIN_FAULT_L2:        return "Domain fault (L2)";
    case FS_ASYNC_EXT_ABORT:        return "Async ext abort";
    case FS_TLB_CONFLICT_ABORT:     return "TLB conflict abort";
    case FS_IMPRECISE_EXT_ABORT:    return "Imprecise ext abort";
    case FS_ECC_ERROR:              return "ECC error";
    default:                        return "Unknown";
    }
}

/* =========================================================================
 * Processor mode name from CPSR
 * ========================================================================= */

static const char *mode_name(uint32_t cpsr)
{
    switch (cpsr & ARM32_MODE_MASK) {
    case ARM32_MODE_USR: return "USR";
    case ARM32_MODE_SVC: return "SVC";
    case ARM32_MODE_IRQ: return "IRQ";
    case ARM32_MODE_FIQ: return "FIQ";
    case ARM32_MODE_ABT: return "ABT";
    case ARM32_MODE_UND: return "UND";
    case ARM32_MODE_SYS: return "SYS";
    default:             return "???";
    }
}

/* =========================================================================
 * Check if a fault status code is a translation or permission fault
 * ========================================================================= */

static inline bool is_translation_fault(uint32_t status)
{
    uint32_t fs = status & DFSR_STATUS_MASK;
    return fs == FS_TRANSLATION_FAULT_L1 || fs == FS_TRANSLATION_FAULT_L2;
}

static inline bool is_permission_fault(uint32_t status)
{
    uint32_t fs = status & DFSR_STATUS_MASK;
    return fs == FS_PERMISSION_FAULT_L1 || fs == FS_PERMISSION_FAULT_L2;
}

static inline bool is_domain_fault(uint32_t status)
{
    uint32_t fs = status & DFSR_STATUS_MASK;
    return fs == FS_DOMAIN_FAULT_L1 || fs == FS_DOMAIN_FAULT_L2;
}

/* =========================================================================
 * Halt — spin with WFI so the CPU doesn't burn cycles
 * ========================================================================= */

static void halt(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* =========================================================================
 * data_abort_handler
 *
 * Called from data_handler in exceptions.S:
 *   void data_abort_handler(arm_regs_t *regs, uint32_t dfar, uint32_t dfsr)
 *
 * regs = saved CPU context (PC is in regs->lr after the adjustment in asm)
 * dfar = Data Fault Address Register — the virtual address that faulted
 * dfsr = Data Fault Status Register — describes the type of fault
 * ========================================================================= */
void data_abort_handler(arm_regs_t *regs, uint32_t dfar, uint32_t dfsr)
{
    (void)regs;

    uint32_t status  = dfsr & DFSR_STATUS_MASK;
    bool     is_write = (dfsr & DFSR_WNR) != 0;
    uint32_t pc       = regs->lr;  /* already adjusted by data_handler */
    uint32_t cpsr     = regs->spsr;

    uart_printf("[fault] DATA ABORT: DFAR=0x%08x DFSR=0x%08x\n", dfar, dfsr);
    uart_printf("        Status: %s (0x%x)  %s\n",
                fault_status_str(status), status, is_write ? "WRITE" : "READ");
    uart_printf("        PC=0x%08x  mode=%s  cpsr=0x%08x\n",
                pc, mode_name(cpsr), cpsr);

    /*
     * User-space fault (< 0x80000000):
     * Candidate for demand paging or process kill.
     */
    if (dfar < ARM_USER_SPACE_LIMIT) {
        if (is_translation_fault(status) || is_domain_fault(status)) {
            /*
             * Translation/domain fault in user space.
             * The VMM demand-paging path would allocate a physical page
             * and map it here.  For now, print and halt.
             */
            uart_printf("[fault] USER SPACE translation/domain fault at 0x%08x\n",
                        dfar);
            uart_printf("[fault] Demand paging not yet implemented.\n");
        } else if (is_permission_fault(status)) {
            /*
             * Permission fault — page is mapped but access denied.
             * If the page exists with wrong permissions, send SIGSEGV.
             * For now: diagnostic + halt.
             */
            uart_printf("[fault] USER SPACE permission fault at 0x%08x\n", dfar);
            uart_printf("[fault] Would send SIGSEGV to faulting process.\n");
        } else {
            uart_printf("[fault] USER SPACE abort at 0x%08x (status=0x%x)\n",
                        dfar, status);
        }
    } else {
        /*
         * Kernel-space fault — this is always a bug.
         */
        uart_printf("[fault] *** KERNEL SPACE FAULT at 0x%08x ***\n", dfar);
        uart_printf("[fault] This is a kernel bug. Halting.\n");
    }

    halt();
}

/* =========================================================================
 * prefetch_abort_handler
 *
 * Called from prefetch_handler in exceptions.S:
 *   void prefetch_abort_handler(arm_regs_t *regs, uint32_t ifar, uint32_t ifsr)
 *
 * ifar = Instruction Fault Address Register — address of the instruction
 *        that could not be fetched
 * ifsr = Instruction Fault Status Register
 * ========================================================================= */
void prefetch_abort_handler(arm_regs_t *regs, uint32_t ifar, uint32_t ifsr)
{
    (void)regs;

    uint32_t status = ifsr & DFSR_STATUS_MASK;
    uint32_t pc     = regs->lr;  /* already adjusted by prefetch_handler */
    uint32_t cpsr   = regs->spsr;

    uart_printf("[fault] PREFETCH ABORT: IFAR=0x%08x IFSR=0x%08x\n", ifar, ifsr);
    uart_printf("        Status: %s (0x%x)\n", fault_status_str(status), status);
    uart_printf("        PC=0x%08x  mode=%s  cpsr=0x%08x\n",
                pc, mode_name(cpsr), cpsr);

    if (ifar < ARM_USER_SPACE_LIMIT) {
        if (is_translation_fault(status) || is_domain_fault(status)) {
            uart_printf("[fault] USER SPACE instruction fetch from unmapped page 0x%08x\n",
                        ifar);
            uart_printf("[fault] Demand paging not yet implemented.\n");
        } else {
            uart_printf("[fault] USER SPACE prefetch abort at 0x%08x\n", ifar);
        }
    } else {
        uart_printf("[fault] *** KERNEL SPACE instruction fault at 0x%08x ***\n",
                    ifar);
        uart_printf("[fault] This is a kernel bug. Halting.\n");
    }

    halt();
}

/* =========================================================================
 * undef_instr_handler
 *
 * Called from undef_handler in exceptions.S:
 *   void undef_instr_handler(arm_regs_t *regs, uint32_t fault_addr)
 *
 * fault_addr = address of the undefined instruction (PC adjusted by asm)
 * ========================================================================= */
void undef_instr_handler(arm_regs_t *regs, uint32_t fault_addr)
{
    uint32_t cpsr   = regs->spsr;
    uint32_t insn   = *(volatile uint32_t *)fault_addr;

    uart_printf("[fault] UNDEFINED INSTRUCTION: addr=0x%08x\n", fault_addr);
    uart_printf("        instruction word=0x%08x\n", insn);
    uart_printf("        PC=0x%08x  mode=%s  cpsr=0x%08x\n",
                fault_addr, mode_name(cpsr), cpsr);

    if (fault_addr < ARM_USER_SPACE_LIMIT) {
        uart_printf("[fault] USER SPACE executed undefined instruction.\n");
    } else {
        uart_printf("[fault] KERNEL SPACE undefined instruction. Halting.\n");
    }

    halt();
}

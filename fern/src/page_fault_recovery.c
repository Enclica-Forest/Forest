#include "include/memory.h"
#include "include/memory_region_manager.h"
#include "include/screen.h"
#include "include/panic.h"
#include "include/task.h"
#include "include/debuglog.h"
#include "include/framebuffer.h"
#include "include/page_fault_recovery.h"
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// INTELLIGENT PAGE FAULT RECOVERY SYSTEM
// =============================================================================
// Attempts to recover from page faults gracefully instead of immediately 
// crashing the system. Implements various recovery strategies based on the
// fault type and memory region.
// =============================================================================

#define MAX_RECOVERY_ATTEMPTS 5
#define RECOVERY_MAGIC 0x52454356  // "RECV"

typedef enum {
    RECOVERY_ACTION_IGNORE = 0,      // Ignore the fault and continue
    RECOVERY_ACTION_MAP_ZERO,        // Map a zero page
    RECOVERY_ACTION_MAP_GUARD,       // Map a guard page
    RECOVERY_ACTION_KILL_PROCESS,    // Terminate offending process
    RECOVERY_ACTION_PANIC,           // System panic (last resort)
    RECOVERY_ACTION_RETRY,           // Retry the operation
    RECOVERY_ACTION_REDIRECT         // Redirect to safe memory
} page_fault_recovery_action_t;

typedef struct {
    uint32_t fault_addr;
    uint32_t error_code;
    uint32_t fault_count;
    uint32_t last_recovery_action;
    bool recovery_attempted;
} fault_history_entry_t;

static struct {
    uint32_t magic;
    fault_history_entry_t history[16];
    uint32_t history_count;
    uint32_t total_recoveries;
    uint32_t total_failures;
    bool recovery_enabled;
} recovery_manager = {
    .magic = RECOVERY_MAGIC,
    .history_count = 0,
    .total_recoveries = 0,
    .total_failures = 0,
    .recovery_enabled = true
};

// =============================================================================
// RECOVERY STRATEGY FUNCTIONS
// =============================================================================

static page_fault_recovery_action_t determine_recovery_action(uint32_t fault_addr, uint32_t error_code) {
    // error_code bit 2 (0x4): set by the CPU when the fault originated in
    // user mode (CPL=3).  This works even without SMEP/SMAP/NX hardware.
    bool from_user_mode = (error_code & 0x4) != 0;

    // Check for repeated faults at same address
    for (uint32_t i = 0; i < recovery_manager.history_count; i++) {
        fault_history_entry_t* entry = &recovery_manager.history[i];
        if (entry->fault_addr == fault_addr) {
            entry->fault_count++;
            if (entry->fault_count > MAX_RECOVERY_ATTEMPTS) {
                // Too many retries: kill user task or panic kernel
                return from_user_mode ? RECOVERY_ACTION_KILL_PROCESS
                                      : RECOVERY_ACTION_PANIC;
            }
        }
    }

    // Kernel-mode faults to suspicious poison patterns are unrecoverable
    if (!from_user_mode) {
        if (fault_addr < 0x1000) {
            // Kernel null pointer dereference: panic
            return RECOVERY_ACTION_PANIC;
        }
        if ((fault_addr & 0xFF000000) == 0xAA000000 ||
            (fault_addr & 0xFF000000) == 0xDE000000 ||
            (fault_addr & 0xFF000000) == 0xCC000000) {
            return RECOVERY_ACTION_PANIC;
        }
        // Kernel fault to a critical region: panic
        if (memory_region_is_critical(fault_addr)) {
            return RECOVERY_ACTION_PANIC;
        }
        // Any other unmapped kernel address: panic
        return RECOVERY_ACTION_PANIC;
    }

    // ---- From here: fault originated in USER MODE ----

    if (fault_addr < 0x1000) {
        // User null pointer dereference — kill the task
        return RECOVERY_ACTION_KILL_PROCESS;
    }

    if ((fault_addr & 0xFF000000) == 0xAA000000) {
        // Uninitialized memory pattern in user space — kill the task
        return RECOVERY_ACTION_KILL_PROCESS;
    }

    if ((fault_addr & 0xFF000000) == 0xDE000000) {
        // Use-after-free pattern in user space — kill the task
        return RECOVERY_ACTION_KILL_PROCESS;
    }

    if ((fault_addr & 0xFF000000) == 0xCC000000) {
        // Buffer overflow pattern in user space — kill the task
        return RECOVERY_ACTION_KILL_PROCESS;
    }

    // User fault to a kernel critical region: privilege violation — kill task
    if (memory_region_is_critical(fault_addr)) {
        return RECOVERY_ACTION_KILL_PROCESS;
    }

    // User-space fault in the expected user address range: do NOT silently map
    // zero pages — that masks real bugs in the ELF loader and user programs.
    // Terminate the offending task so the error surfaces immediately.
    if (fault_addr >= MEMORY_USER_START && fault_addr < MEMORY_USER_END) {
        return RECOVERY_ACTION_KILL_PROCESS;
    }

    // User fault outside the normal user range (e.g. stack region or high
    // address): also kill the task rather than silently mapping memory.
    return RECOVERY_ACTION_KILL_PROCESS;
}

static bool execute_recovery_action(page_fault_recovery_action_t action, uint32_t fault_addr, uint32_t error_code) {
    switch (action) {
        case RECOVERY_ACTION_IGNORE:
            return true; // Just pretend it didn't happen
            
        case RECOVERY_ACTION_MAP_ZERO: {
            // Map a zero-filled page at the fault address
            uint32_t page_addr = fault_addr & ~0xFFF; // Align to page boundary
            
            // Get a free physical page
            uint32_t phys_page = pmm_alloc_frame();
            if (phys_page == 0) {
                return false; // Out of memory
            }
            
            // Map it with appropriate permissions
            uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE;
            if (fault_addr >= MEMORY_USER_START) {
                flags |= PAGE_USER;
            }
            
            memory_result_t result = vmm_map_page(vmm_get_current_page_directory(), 
                                                 page_addr, phys_page, flags);
            if (result != MEMORY_OK) {
                pmm_free_frame(phys_page);
                return false;
            }
            
            // Zero the page
            char* page_ptr = (char*)page_addr;
            for (int i = 0; i < 4096; i++) {
                page_ptr[i] = 0;
            }
            
            return true;
        }
        
        case RECOVERY_ACTION_MAP_GUARD: {
            // Map a guard page that will cause another fault if accessed
            uint32_t page_addr = fault_addr & ~0xFFF;
            
            // Map with no permissions (will fault on any access)
            memory_result_t result = vmm_map_page(vmm_get_current_page_directory(),
                                                 page_addr, 0, 0);
            return (result == MEMORY_OK);
        }
        
        case RECOVERY_ACTION_REDIRECT: {
            // Redirect to a safe memory region
            // For now, just map a zero page
            return execute_recovery_action(RECOVERY_ACTION_MAP_ZERO, fault_addr, error_code);
        }
        
        case RECOVERY_ACTION_RETRY:
            // Try the operation again (useful for temporary conditions)
            return true;
            
        case RECOVERY_ACTION_KILL_PROCESS:
            // Terminate the current user task that caused the fault.
            if (current_task && current_task->id > 1) {
                debuglog(DEBUG_ERROR,
                         "[PFR] Killing task %u (%s) for page fault at 0x%x (err=0x%x)\n",
                         current_task->id, current_task->name, fault_addr, error_code);
                // task_terminate_current() never returns to us (it schedules
                // away permanently), so the caller's post-recovery guard
                // decrement never runs for this path. Clear it now instead.
                page_fault_reset_recursion_guard();
                task_terminate_current(SIGSEGV);
                return true; // scheduler will handle cleanup
            }
            return false; // kernel task — cannot kill, fall through to panic
            
        case RECOVERY_ACTION_PANIC:
        default:
            return false; // No recovery possible
    }
}

static void record_fault_history(uint32_t fault_addr, uint32_t error_code, page_fault_recovery_action_t action, bool success) {
    if (recovery_manager.history_count < 16) {
        fault_history_entry_t* entry = &recovery_manager.history[recovery_manager.history_count];
        entry->fault_addr = fault_addr;
        entry->error_code = error_code;
        entry->fault_count = 1;
        entry->last_recovery_action = action;
        entry->recovery_attempted = true;
        recovery_manager.history_count++;
    }
    
    if (success) {
        recovery_manager.total_recoveries++;
    } else {
        recovery_manager.total_failures++;
    }
}

// =============================================================================
// PUBLIC INTERFACE
// =============================================================================

bool page_fault_attempt_recovery(uint32_t fault_addr, uint32_t error_code) {
    if (!recovery_manager.recovery_enabled) {
        return false;
    }

    if (framebuffer_mmap_handle_page_fault(fault_addr)) {
        return true;
    }
    
    page_fault_recovery_action_t action = determine_recovery_action(fault_addr, error_code);
    
    // Attempt recovery
    bool success = execute_recovery_action(action, fault_addr, error_code);
    
    // Record the attempt
    record_fault_history(fault_addr, error_code, action, success);
    
    return success;
}

void page_fault_recovery_enable(bool enable) {
    recovery_manager.recovery_enabled = enable;
}

void page_fault_recovery_get_stats(uint32_t* total_recoveries, uint32_t* total_failures) {
    if (total_recoveries) {
        *total_recoveries = recovery_manager.total_recoveries;
    }
    if (total_failures) {
        *total_failures = recovery_manager.total_failures;
    }
}

void page_fault_recovery_dump_history(void) {
    print("[PFR] Page Fault Recovery History:\n");
    print("[PFR] Total recoveries: ");
    print_dec(recovery_manager.total_recoveries);
    print("\n");
    print("[PFR] Total failures: ");
    print_dec(recovery_manager.total_failures);
    print("\n");
    
    for (uint32_t i = 0; i < recovery_manager.history_count; i++) {
        fault_history_entry_t* entry = &recovery_manager.history[i];
        print("[PFR] Fault 0x");
        print_hex(entry->fault_addr);
        print(" count: ");
        print_dec(entry->fault_count);
        print(" action: ");
        print_dec(entry->last_recovery_action);
        print("\n");
    }
}

// =============================================================================
// ADVANCED MEMORY VALIDATION
// =============================================================================

bool page_fault_validate_address_access(uint32_t addr, bool is_write) {
    // Check if address is in a valid region
    if (addr < 0x1000) {
        return false; // Null pointer access
    }
    
    // Check for corruption patterns
    if ((addr & 0xFF000000) == 0xAA000000 ||
        (addr & 0xFF000000) == 0xDE000000 ||
        (addr & 0xFF000000) == 0xCC000000 ||
        (addr & 0xFF000000) == 0x55000000) {
        return false; // Memory corruption pattern
    }
    
    // Check if it's in a critical region
    if (memory_region_is_critical(addr)) {
        if (is_write && !memory_region_is_writable(addr)) {
            return false; // Write to read-only critical region
        }
    }
    
    return true; // Appears to be valid
}

// =============================================================================
// PRE-FAULT MAPPING SYSTEM
// =============================================================================

void page_fault_premap_critical_regions(void) {
    // Pre-map critical kernel regions to prevent faults during critical operations
    // NOTE: This is optional and can be skipped if causing boot issues
    
    // Skip pre-mapping for now - it's not essential for boot
    // The page fault handler will map pages on demand
    return;
}

void page_fault_recovery_init(void) {
    recovery_manager.recovery_enabled = true;
    recovery_manager.history_count = 0;
    recovery_manager.total_recoveries = 0;
    recovery_manager.total_failures = 0;
    
    // Pre-map critical regions
    page_fault_premap_critical_regions();
}
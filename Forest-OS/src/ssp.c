#include "include/ssp.h"
#include "include/screen.h"
#include "include/memory.h"
#include "include/debuglog.h"
#include <stdint.h>
#include <stdbool.h>
#include "include/system.h"

// =============================================================================
// STACK SMASHING PROTECTION (SSP) IMPLEMENTATION
// =============================================================================
// Comprehensive protection against buffer overflows and return address attacks
// =============================================================================

// Global canary value (used by compiler-generated code)
uint32_t __stack_chk_guard = 0;

// SSP statistics and state
typedef struct ssp_stats {
    uint32_t violations_detected;
    uint32_t checks_performed;
    uint32_t false_positives;
    uint32_t return_address_violations;
} ssp_stats_t;

static ssp_stats_t ssp_stats = {0};
static bool ssp_initialized = false;
static uint32_t ssp_random_seed = SSP_CANARY_RANDOM_SEED;

// Simple PRNG for canary generation
extern uint8 kernel_start;
extern uint8 kernel_end;
extern uint8 _stack_bottom;
extern uint8 _stack_top;

static inline uint32_t ssp_kernel_start(void) {
    return (uint32_t)&kernel_start;
}

static inline uint32_t ssp_kernel_end(void) {
    return (uint32_t)&kernel_end;
}

static inline uint32_t ssp_stack_bottom(void) {
    return (uint32_t)&_stack_bottom;
}

static inline uint32_t ssp_stack_top(void) {
    return (uint32_t)&_stack_top;
}

static void ssp_log_warning(const char* msg, uint32_t value) {
    if (!debuglog_is_ready()) {
        return;
    }
    debuglog_write("[SSP] ");
    debuglog_write(msg);
    debuglog_write(": ");
    debuglog_write_hex(value);
    debuglog_write("\n");
}

static uint32_t ssp_random_next(void) {
    ssp_random_seed = ssp_random_seed * 1103515245 + 12345;
    return (ssp_random_seed >> 16) & 0x7FFF;
}

// Generate a cryptographically strong canary value
void ssp_generate_random_canary(void) {
    uint32_t entropy = 0;
    uint32_t timestamp = (uint32_t)cpu_read_tsc();
    
    // Combine multiple entropy sources
    entropy ^= timestamp;
    entropy ^= (uint32_t)&ssp_stats;  // Use address space layout
    entropy ^= ssp_random_next();
    
    // Ensure canary is never null or contains common patterns
    __stack_chk_guard = entropy;
    if (__stack_chk_guard == 0 || 
        __stack_chk_guard == 0xFFFFFFFF ||
        __stack_chk_guard == 0xDEADBEEF ||
        __stack_chk_guard == 0xFEEDFACE) {
        __stack_chk_guard = 0xCAFEBABE ^ entropy;
    }
}

// Initialize SSP system
void ssp_init(void) {
    print("[SSP] Initializing Stack Smashing Protection...\n");
    
    // Generate initial canary
    ssp_generate_random_canary();
    
    // Initialize statistics
    ssp_stats.violations_detected = 0;
    ssp_stats.checks_performed = 0;
    ssp_stats.false_positives = 0;
    ssp_stats.return_address_violations = 0;
    
    ssp_initialized = true;
    
    print("[SSP] Canary value: 0x");
    print_hex(__stack_chk_guard);
    print("\n");
}

// Get current canary value
uint32_t ssp_get_canary(void) {
    return __stack_chk_guard;
}

// Direct VGA write function for emergencies (avoid heap allocation)
// Safe to call with a corrupted stack because it only uses register-based args
// and writes directly to memory-mapped VGA.
static void ssp_emergency_write(const char *msg, int row, int col, uint8_t attr) {
    volatile uint16_t *vga = (volatile uint16_t*)0xB8000;
    int pos = row * 80 + col;

    while (*msg && col < 80) {
        vga[pos] = (attr << 8) | *msg;
        msg++;
        pos++;
        col++;
    }
}

// Write a single character to COM1 serial port using only I/O instructions.
// Safe to call unconditionally - no state, no stack use beyond the call frame.
// Note: COM1 ports (0x3F8, 0x3FD) are above 0xFF so we must use the DX form
// of inb/outb rather than the immediate form.
static void ssp_serial_putc(char c) {
    // Wait for transmit holding register empty (bit 5 of line status register).
    // COM1 line status register = 0x3FD.
    uint32_t timeout = 100000;
    while (timeout--) {
        uint8_t lsr;
        __asm__ volatile("inb %w1, %b0" : "=a"(lsr) : "Nd"((uint16_t)0x3FD));
        if (lsr & 0x20) break;
    }
    // COM1 data register = 0x3F8.
    __asm__ volatile("outb %b0, %w1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
}

static void ssp_serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') ssp_serial_putc('\r');
        ssp_serial_putc(*s++);
    }
}

// Static emergency stack used when the real kernel stack is corrupted.
// Defined here (not in stack_protection.c) so __stack_chk_fail can reference
// it without going through switch_to_emergency_stack() (which would itself
// need a valid stack to execute its return).
static uint8_t __ssp_emergency_stack[1024] __attribute__((aligned(16)));

// Handle stack smashing detection (compiler-generated calls).
// CRITICAL: this function MUST NOT rely on the (potentially corrupted) stack
// for any significant work.  The sequence is:
//   1. Disable interrupts immediately.
//   2. Atomically switch ESP/EBP to a private static emergency stack.
//      This is done with inline assembly so there is no 'call/ret' that would
//      use the corrupted stack.
//   3. Write the panic message via direct VGA and serial I/O only.
//   4. Halt forever.
// We deliberately do NOT call kernel_panic() here because that function
// allocates a large panic_context_t on the stack, calls memset/strncpy, etc.
// - all of which require a trustworthy stack.
void __attribute__((noreturn)) __stack_chk_fail(void) {
    // Step 1 - freeze execution immediately.
    __asm__ volatile("cli");

    // Step 2 - switch to the private emergency stack WITHOUT a call/ret pair.
    // After this inline block ESP and EBP point into __ssp_emergency_stack and
    // any C code below (including function calls) has a valid stack.
    uint32_t emergency_top = (uint32_t)(__ssp_emergency_stack + sizeof(__ssp_emergency_stack)) - 4u;
    __asm__ volatile(
        "mov %0, %%esp\n\t"
        "mov %%esp, %%ebp"
        :
        : "r"(emergency_top)
        : "memory"
    );

    // Step 3 - announce the violation through every safe output path.
    // VGA: paint top two rows red-on-white so it is visible on screen.
    ssp_emergency_write("*** STACK SMASHING DETECTED - KERNEL HALTED ***       ", 0, 0, 0x4F);
    ssp_emergency_write("A buffer overflow corrupted a stack canary value.      ", 1, 0, 0x4F);

    // Serial: also log to COM1 for headless / QEMU debug sessions.
    ssp_serial_write("\r\n[SSP] *** STACK SMASHING DETECTED ***\r\n");
    ssp_serial_write("[SSP] A stack canary was overwritten.  System halted.\r\n");

    // Step 4 - halt forever.  Do NOT attempt to recover or regenerate the
    // canary - the system is in an unknown state and must not continue.
    for (;;) {
        __asm__ volatile("hlt");
    }
}

// Local stack checking failure (when using local canaries).
// Also noreturn - switches to the safe emergency stack first, then
// writes a marker and delegates to the main handler.
void __attribute__((noreturn)) __stack_chk_fail_local(void) {
    // Disable interrupts and switch to the private emergency stack before
    // doing anything else, for the same reason as __stack_chk_fail().
    __asm__ volatile("cli");
    uint32_t emergency_top = (uint32_t)(__ssp_emergency_stack + sizeof(__ssp_emergency_stack)) - 4u;
    __asm__ volatile(
        "mov %0, %%esp\n\t"
        "mov %%esp, %%ebp"
        :
        : "r"(emergency_top)
        : "memory"
    );
    ssp_emergency_write("*** LOCAL STACK CORRUPTION ***", 2, 0, 0x4F);
    // __stack_chk_fail() will switch stacks again (idempotent) and halt.
    __stack_chk_fail();
}

// Manual function entry protection
uint32_t ssp_function_enter(void) {
    ssp_stats.checks_performed++;
    
    if (!ssp_initialized) {
        return 0; // Can't protect if not initialized
    }
    
    // Return current canary for this function
    return __stack_chk_guard;
}

// Manual function exit validation
void ssp_function_exit(uint32_t canary) {
    if (!ssp_initialized) {
        return;
    }
    
    if (canary != __stack_chk_guard) {
        ssp_report_violation("manual_protection", __stack_chk_guard, canary);
        __stack_chk_fail();
    }
}

// Protect return address location
void ssp_protect_return_address(uint32_t *return_addr) {
    if (!ssp_initialized || !return_addr) {
        return;
    }
    
    // Validate that this looks like a reasonable return address
    uint32_t addr = *return_addr;
    
    uint32_t code_start = ssp_kernel_start();
    uint32_t code_end = ssp_kernel_end();

    if (addr < code_start || addr >= code_end) {
        ssp_stats.return_address_violations++;
        ssp_log_warning("return address outside kernel image", addr);
        return; // Do not panic during early boot paths
    }
}

// Validate a return address
bool ssp_validate_return_address(uint32_t return_addr) {
    if (!ssp_initialized) {
        return true; // Can't validate if not initialized
    }
    
    uint32_t code_start = ssp_kernel_start();
    uint32_t code_end = ssp_kernel_end();
    
    // If kernel symbols are not properly initialized, use reasonable defaults
    if (code_start == 0 || code_end == 0 || code_start >= code_end) {
        // Fallback: assume kernel is loaded at standard location
        code_start = 0x00100000;  // 1MB (standard kernel load address)
        code_end = 0x00200000;    // 2MB (reasonable kernel size)
        ssp_log_warning("using fallback kernel bounds for return addr", return_addr);
    }

    if (return_addr < code_start || return_addr >= code_end) {
        return false;
    }
    
    // Check alignment (x86 instructions are typically aligned)
    if (return_addr & 0x3) {
        return false;
    }
    
    return true;
}

// Enhanced stack frame validation
bool ssp_validate_stack_frame(uint32_t *frame_pointer) {
    if (!ssp_initialized || !frame_pointer) {
        return false;
    }
    
    // Basic sanity checks
    uint32_t frame_addr = (uint32_t)frame_pointer;
    
    uint32_t stack_lo = ssp_stack_bottom();
    uint32_t stack_hi = ssp_stack_top();
    
    // If stack symbols are not properly initialized, use reasonable defaults
    if (stack_lo == 0 || stack_hi == 0 || stack_lo >= stack_hi) {
        // Fallback: assume kernel stack is somewhere reasonable
        stack_lo = 0x00080000;  // 512KB
        stack_hi = 0x00100000;  // 1MB
        ssp_log_warning("using fallback stack bounds", frame_addr);
    }

    if (frame_addr < stack_lo || frame_addr >= stack_hi) {
        return false;
    }
    
    // Check frame pointer alignment
    if (frame_addr & 0x3) {
        return false;
    }
    
    // Validate the saved frame pointer and return address
    uint32_t saved_ebp = frame_pointer[0];
    uint32_t return_addr = frame_pointer[1];
    
    // Saved EBP should either be null (bottom of stack) or valid pointer
    if (saved_ebp != 0) {
        if (saved_ebp < stack_lo || saved_ebp >= stack_hi) {
            return false;
        }
    }
    
    // Validate return address
    if (!ssp_validate_return_address(return_addr)) {
        ssp_stats.return_address_violations++;
        return false;
    }
    
    return true;
}

// Report SSP violation with detailed information
void ssp_report_violation(const char *function_name, uint32_t expected, uint32_t found) {
    ssp_stats.violations_detected++;
    
    // Use safe direct writing to avoid recursion
    char msg[80];
    int pos = 0;
    
    // Build message carefully
    const char *prefix = "SSP VIOLATION in ";
    while (*prefix && pos < 70) {
        msg[pos++] = *prefix++;
    }
    
    if (function_name) {
        while (*function_name && pos < 70) {
            msg[pos++] = *function_name++;
        }
    }
    
    msg[pos] = '\0';
    
    ssp_emergency_write(msg, 3, 0, 0x4F);
}

// Get SSP statistics
void ssp_get_stats(uint32_t *violations, uint32_t *checks_performed) {
    if (violations) {
        *violations = ssp_stats.violations_detected;
    }
    
    if (checks_performed) {
        *checks_performed = ssp_stats.checks_performed;
    }
}

// Mark a function as safe (debugging aid)
void ssp_mark_function_safe(const char *function_name) {
    // This is primarily for debugging - could maintain a whitelist
    // For now, just acknowledge the call
    (void)function_name; // Suppress unused parameter warning
}

// Periodic canary refresh for enhanced security.
// WARNING: Calling this at runtime is UNSAFE.  Any active stack frame that
// saved the old canary value on entry will see a mismatch on function return
// and trigger __stack_chk_fail() as a false positive.  Only call this when
// all tasks are at their outermost frame (e.g. during early boot before any
// SSP-protected function has been called, or after a full task restart).
void ssp_refresh_canary(void) {
    if (!ssp_initialized) {
        return;
    }

    // Generate new canary value.
    ssp_generate_random_canary();

    print("[SSP] Canary refreshed: 0x");
    print_hex(__stack_chk_guard);
    print("\n");
}

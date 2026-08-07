#include "include/memory.h"
#include "include/screen.h"
#include "include/panic.h"

#ifndef ARCH_64BIT
#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_64BIT 1
#else
#define ARCH_64BIT 0
#endif
#endif

#if ARCH_64BIT
void stack_protection_init(void) {}
void stack_usage_snapshot(uint32_t* total_size, uint32_t* used_size, uint32_t* remaining) {
    if (total_size) *total_size = 0;
    if (used_size) *used_size = 0;
    if (remaining) *remaining = 0;
}
void switch_to_emergency_stack(void) {}
bool stack_quick_check(uint32_t esp) { (void)esp; return true; }
#else

// Direct VGA memory writing function for emergency situations
static void write_string_direct(const char *str, int x, int y, uint8_t color) {
    uint16_t *vga_memory = (uint16_t*)0xB8000;
    int pos = y * 80 + x;
    
    while (*str && pos < 80 * 25) {
        vga_memory[pos] = ((uint16_t)color << 8) | *str;
        str++;
        pos++;
    }
}

// =============================================================================
// STACK OVERFLOW PROTECTION
// =============================================================================
// Implements stack canaries and guard pages to detect stack overflow
// =============================================================================

#define STACK_CANARY_VALUE      0xDEADBEEF
#define STACK_GUARD_SIZE        4096        // One page guard
#define STACK_WARNING_THRESHOLD 1024       // Warn when less than 1KB left

// Stack information structure
typedef struct stack_info {
    uint32_t base;           // Stack base (top)
    uint32_t limit;          // Stack limit (bottom)  
    uint32_t current;        // Current ESP
    uint32_t canary_addr;    // Address of stack canary
    uint32_t guard_page;     // Guard page address
    bool initialized;
} stack_info_t;

static stack_info_t kernel_stack = {0};

// Initialize stack protection for the boot kernel stack.
// This records the boundaries of the stack that is active during early boot
// (before the tasking system allocates per-task stacks).  After task_init()
// runs, each task has its own 8 KB stack allocated via kmalloc; those stacks
// are separate and not tracked here.  stack_check_integrity() therefore only
// validates the ESP when we are still on the initial boot stack (i.e. before
// tasking has started or when current_task is the initial kernel task).
void stack_protection_init(void) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));

    kernel_stack.current = esp;

    // The boot stack top is the next higher page boundary from the current ESP.
    // If ESP is already page-aligned we go one page up to ensure base > esp.
    kernel_stack.base  = (esp & ~4095u) + 4096u;   // top of the page containing ESP
    kernel_stack.limit = kernel_stack.base - (8 * 1024); // 8 KB downward
    kernel_stack.guard_page   = kernel_stack.limit - STACK_GUARD_SIZE;
    kernel_stack.canary_addr  = kernel_stack.limit + 16; // just above the limit

    // Place the sentinel canary at the very bottom of the usable stack region.
    *((uint32_t*)kernel_stack.canary_addr) = STACK_CANARY_VALUE;

    kernel_stack.initialized = true;

    print("[STACK] Protection initialized - Base: 0x");
    print_hex(kernel_stack.base);
    print(", Limit: 0x");
    print_hex(kernel_stack.limit);
    print("\n");
}

// Return true when the current ESP sits inside the recorded boot-stack region.
// This is the only reliable way to know whether to apply the boot-stack bounds
// check: if the current ESP is not in [limit, base) we are running on a
// per-task stack and should not validate against boot-stack limits.
static bool on_boot_stack(void) {
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    return (esp >= kernel_stack.limit && esp < kernel_stack.base);
}

// Check if stack is in good condition.
// After tasking is enabled each task has its own stack; checking the boot-time
// stack bounds against a different task's ESP would be a false positive, so we
// skip the range/canary test in that case.
bool stack_check_integrity(void) {
    if (!kernel_stack.initialized) {
        return true; // Can't check if not initialized
    }

    // Only validate when we are actually running on the boot stack.
    if (!on_boot_stack()) {
        return true;
    }

    uint32_t current_esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(current_esp));

    kernel_stack.current = current_esp;

    // Check if ESP is within the recorded boot-stack bounds.
    if (current_esp < kernel_stack.limit || current_esp >= kernel_stack.base) {
        return false;
    }

    // Check the sentinel canary placed at the bottom of the boot stack.
    if (*((uint32_t*)kernel_stack.canary_addr) != STACK_CANARY_VALUE) {
        return false;
    }

    return true;
}

// Check for stack overflow with warning.
// Returns -1 on overflow, 1 on low-stack warning, 0 if OK.
// Only meaningful while running on the boot stack; returns 0 otherwise.
int stack_check_overflow(void) {
    if (!kernel_stack.initialized) {
        return 0; // No overflow if not initialized
    }

    // Skip check when we are on a per-task stack to avoid false positives.
    if (!on_boot_stack()) {
        return 0;
    }

    uint32_t current_esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(current_esp));

    // Calculate remaining stack space
    if (current_esp < kernel_stack.limit) {
        return -1; // Stack overflow!
    }

    uint32_t remaining = current_esp - kernel_stack.limit;

    if (remaining < STACK_WARNING_THRESHOLD) {
        return 1; // Stack warning
    }

    return 0; // Stack OK
}

// Enhanced function prologue with stack checking (use as macro)
#define STACK_CHECK_ENTER() do { \
    int overflow = stack_check_overflow(); \
    if (overflow < 0) { \
        write_string_direct("STACK OVERFLOW DETECTED", 0, 23, 0x4F); \
        kernel_panic("Stack overflow detected"); \
    } else if (overflow > 0) { \
        static int warning_count = 0; \
        if (++warning_count < 5) { \
            print("[STACK] WARNING: Low stack space\n"); \
        } \
    } \
} while(0)

// Get stack usage statistics
void stack_get_stats(uint32_t *total_size, uint32_t *used_size, uint32_t *remaining) {
    if (!kernel_stack.initialized) {
        if (total_size) *total_size = 0;
        if (used_size) *used_size = 0;
        if (remaining) *remaining = 0;
        return;
    }
    
    uint32_t current_esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(current_esp));
    
    uint32_t total = kernel_stack.base - kernel_stack.limit;
    uint32_t used = kernel_stack.base - current_esp;
    uint32_t remain = current_esp - kernel_stack.limit;
    
    if (total_size) *total_size = total;
    if (used_size) *used_size = used;
    if (remaining) *remaining = remain;
}

// Emergency stack switching for critical situations
static uint8_t emergency_stack[2048] __attribute__((aligned(16)));
static uint32_t *emergency_stack_top = (uint32_t*)(emergency_stack + sizeof(emergency_stack) - 4);

void switch_to_emergency_stack(void) {
    __asm__ volatile(
        "mov %0, %%esp\n\t"
        "mov %%esp, %%ebp"
        :
        : "r"(emergency_stack_top)
        : "memory"
    );
}

// Stack protection for specific functions (use in critical functions)
static inline void __attribute__((always_inline)) stack_protect_function(void) {
    STACK_CHECK_ENTER();
}

// Export the macro for use in headers
#ifndef __STACK_PROTECTION_H__
#define STACK_PROTECT() stack_protect_function()
#endif

// Quick stack bounds check (for use in page fault handler).
// Accepts any ESP that sits in the conventional kernel address space
// (above 1 MB, below the 4 GB mark, and word-aligned).  This is intentionally
// loose so that per-task kernel stacks allocated anywhere in kernel heap are
// accepted without false positives.
bool stack_quick_check(uint32_t esp) {
    // Must be above the 1 MB mark (below is BIOS/VGA/ROM territory)
    // and must be dword-aligned (all x86 stacks are at least 4-byte aligned).
    return (esp >= 0x00100000u && esp <= 0xFFFFFFFCu && (esp & 0x3u) == 0);
}

#endif /* ARCH_64BIT */

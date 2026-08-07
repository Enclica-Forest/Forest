# Forest OS Code Style Guide

This document describes the coding conventions used across the Forest OS codebase: the Fern kernel, ForeB bootloader, userspace utilities, and the shared libc library.

---

## 1. Code Style Overview

Forest OS is written in C11 with inline assembly (NASM for x86, GCC asm for inline). The codebase targets multiple architectures (x86-32, x86-64, AArch64, RISC-V) and must remain freestanding-compatible.

Key principles:
- Clarity over cleverness. Code is read far more often than written.
- Prefer explicit names over abbreviations.
- Keep functions focused and short (under 100 lines where practical).

---

## 2. Naming Conventions

### Functions

Kernel and library functions use `lower_snake_case` with a module prefix:

```c
/* Good */
void spinlock_acquire(spinlock_t* lock);
bool kernel_get_multiboot_framebuffer(uintptr_t* addr, uint32_t* width, ...);
int  builtin_cd(int argc, char** argv);

/* Bad */
void SpinLockAcquire(spinlock_t* lock);   /* CamelCase */
void getFrameBuffer();                     /* no module prefix */
```

Legacy functions from early kernel code may use `camelCase` (`clearScreen()`, `updateCursor()`). Do not introduce new camelCase names.

Static (file-local) functions are declared `static`:

```c
static void parse_multiboot_framebuffer_early(uint32 magic, uint32 mbi_addr);
static void kernel_panic_memory_error(const char* stage, const char* reason);
```

### Variables

Local variables use `lower_snake_case`. Global variables are prefixed with `g_`:

```c
/* Locals */
uint32_t fb_pages = (map_size + 0xFFF) >> 12;
bool fully_mapped = true;

/* Globals */
static bool g_silent_boot = false;
void* g_multiboot_framebuffer = NULL;
uint32_t g_multiboot_fb_width = 0;
```

### Types

Types use `lower_snake_case` with a `_t` suffix:

```c
typedef struct {
    char label[64];
    bool ok;
} boot_log_entry_t;

typedef struct {
    atomic8_t locked;
    uint32 owner_cpu;
    const char* name;
} spinlock_t;
```

Anonymous structs are acceptable for internal structures:

```c
static struct {
    bool valid;
    uintptr_t addr;
    uint32_t width;
    uint32_t height;
} g_multiboot_framebuffer_internal = {0};
```

### Macros and Constants

Use `SCREAMING_SNAKE_CASE`. Prefer `static inline` over function-like macros for non-trivial cases:

```c
#define BOOT_LOG_CAPACITY 64
#define KERNEL_MIN_MEM_KB_NORMAL    65536u
#define MAX_LINE       4096
#define PAGE_DOWN(x) ((x) & ~((UINT64)EFI_PAGE_SIZE - 1))

/* Prefer static inline for anything non-trivial */
static inline bool spinlock_is_locked(const spinlock_t* lock) {
    return atomic_load8(&lock->locked) != 0;
}
```

Enum values use `SCREAMING_SNAKE_CASE` with a common prefix:

```c
typedef enum {
    INTCTL_NONE = 0,
    INTCTL_8259A_PIC,
    INTCTL_LOCAL_APIC,
} interrupt_controller_type_t;
```

---

## 3. Indentation and Formatting

**4 spaces** for indentation (tabs only in Makefiles where required by syntax).

Braces on the same line as the control statement (K&R style):

```c
void boot_status(const char* label, bool ok) {
    if (!ok) {
        g_boot_failed = true;
    }
    if (debuglog_is_ready()) {
        debuglog_write(ok ? "[BOOT][ OK ] " : "[BOOT][FAIL] ");
        debuglog_write(label);
    }
}
```

Note: The UEFI bootloader (`foreboots/uefi/bootx64.c`) uses Allman style for function definitions. Match the style of the file you are editing.

Target **100 characters** maximum per line. Break long lines at logical points:

```c
debuglog(DEBUG_WARN,
         "[KERNEL] Framebuffer map verify mismatch: page=%u virt=0x%08x "
         "phys=0x%08x want=0x%08x\n",
         i, (uint32_t)page_virt, (uint32_t)verify, (uint32_t)page_phys);
```

Spacing: space after keywords (`if`, `for`, `while`), spaces around binary operators, no space after casts or before pointer `*`:

```c
if (p < end && (*p == 'x' || *p == 'X')) {
    p++;
}
const char* path = (const char*)src;
```

---

## 4. Comment Style

Use `/* ... */` for block and file-header comments. `//` is acceptable for single-line annotations.

File headers:

```c
/*
 * forest-shell.c - Forest OS primary interactive shell
 *
 * A POSIX-compatible shell with builtins, job control, piping,
 * redirection, globbing, variable expansion, and line editing.
 */
```

Section dividers in long files:

```c
/* ---------------------------------------------------------------------------
 * Signal handling
 * ---------------------------------------------------------------------------*/

/* ============================================================================
 * MEMORY FUNCTIONS
 * ============================================================================ */
```

Doxygen-style for complex functions:

```c
/**
 * Reset VGA hardware to 80x25 color text mode (Mode 3).
 * Called when booting without a framebuffer (nofb mode).
 */
static void vga_text_mode_reset(void) {
```

Use `// TODO:`, `// FIXME:` for work items.

---

## 5. Header File Conventions

Every header uses `#ifndef` / `#define` / `#endif` include guards matching the filename:

```c
#ifndef SPINLOCK_H
#define SPINLOCK_H
/* ... */
#endif // SPINLOCK_H
```

Include order (blank line between groups):
1. Own header (if applicable)
2. System/standard headers (`<stdint.h>`, `<string.h>`)
3. Kernel headers (`"types.h"`, `"spinlock.h"`)
4. Subsystem headers (`"arch/debuglog.h"`)

Headers declare types, function prototypes, macros, and `static inline` functions. Do not define variables or non-inline function bodies in headers.

---

## 6. Error Handling Patterns

The kernel uses typed result enums:

```c
memory_result_t mem_result = memory_init(magic, mbi_addr);
if (mem_result != MEMORY_OK) {
    boot_status("Memory subsystem", false);
    kernel_panic_memory_error("memory_init", memory_result_to_string(mem_result));
}
```

Check pointers immediately after allocation:

```c
FILE *fp = malloc(sizeof(FILE));
if (!fp) {
    close(fd);
    errno = ENOMEM;
    return NULL;
}
```

Use `kernel_panic()` for unrecoverable errors, `boot_status()` / `boot_require()` for subsystem init:

```c
boot_require("Hardware detection (CPUID)", hw_detected, "CPUID detection failed");
```

Userspace reports errors to `stderr` and exits non-zero. The UEFI bootloader checks `EFI_STATUS` with `EFI_ERROR()`.

---

## 7. Memory Management Patterns

Always check return values. Userspace uses fail-fast wrappers:

```c
static char *xmalloc(size_t n) {
    char *p = malloc(n);
    if (!p && n > 0) {
        fprintf(stderr, "forest-shell: out of memory\n");
        exit(1);
    }
    return p;
}
```

Match every allocation with a `free`. Check for integer overflow in size calculations:

```c
if (nmemb && size > SIZE_MAX / nmemb) {
    errno = ENOMEM;
    return NULL;
}
```

Use `volatile` for memory-mapped I/O:

```c
volatile uint8_t* vfb = (volatile uint8_t*)g_multiboot_framebuffer;
```

Use designated initializers for structs:

```c
splash_config_t cfg = {
    .enabled          = true,
    .use_quiet_mode   = g_quiet_boot,
    .fade_out_duration = 500
};
```

---

## 8. Logging and Debug Output

Kernel debug log with severity levels:

```c
debuglog(DEBUG_INFO, "[KERNEL] Framebuffer mapped %u/%u pages\n", mapped_ok, fb_pages);
debuglog(DEBUG_WARN, "[KERNEL] Graphics init failed\n");
debuglog(DEBUG_ERROR, "[KERNEL] Framebuffer page %u map failed\n", i);
```

Levels: `DEBUG_DETAIL` (verbose), `DEBUG_INFO`, `DEBUG_WARN`, `DEBUG_ERROR`, `DEBUG_FATAL`.

The ForeB bootloader logs to COM1 serial:

```c
serial_puts("[*] kernel read complete, bytes=");
serial_puthex(done, 8);
serial_puts("\n");
```

Use `boot_status()` for user-visible boot progress.

---

## 9. File Organization

```
fern/src/
├── kernel.c              # Main kernel entry and boot sequence
├── include/              # Headers (types.h, spinlock.h, interrupt.h, ...)
├── arch/                 # Architecture-specific headers
├── boot.asm              # 32-bit multiboot entry
├── boot64.asm            # 64-bit long mode setup
├── interrupt_stubs.asm   # Interrupt handler stubs
└── syscall_stubs.asm     # System call entry points

userspace/
├── forest-shell/shell.c  # Single-file implementations
├── ls/ls.c
├── cat/cat.c
└── Makefile

libs/libc/src/
├── string.c, stdio.c, stdlib.c, signal.c, syscalls.c
```

Every source file begins with a brief comment describing its purpose.

---

## 10. Architecture-Specific Code

Use preprocessor guards for architecture detection:

```c
#if defined(__x86_64__)
    #define ARCH_64BIT 1
#else
    #define ARCH_64BIT 0
#endif
```

Provide no-op stubs for unsupported architectures:

```c
static inline void outb(UINT16 port, UINT8 v) {
#if FOREB_ARCH_IS_X64
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
#else
    (void)port; (void)v;
#endif
}
```

Gate architecture-specific externs and calls:

```c
#if FOREB_MULTIBOOT_SUPPORTED
extern void forebo_handoff(UINT32 entry, UINT32 mb_magic, UINT32 mb_info_ptr);
#endif
```

---

## 11. Assembly Code Conventions

NASM files use `;` for comments, descriptive labels, and `.`-prefixed local labels:

```asm
bits 32
section .multiboot
align 8

start:
    cli
    mov [saved_magic], eax
    jmp .bss_done

.bss_done:
    ; ...
```

Constants with `equ`:

```asm
MULTIBOOT2_MAGIC equ 0xE85250D6
MULTIBOOT2_ARCH equ 0x00000000
```

---

## 12. Build System Conventions

The kernel Makefile is a thin orchestrator including modular `.mk` fragments:

```makefile
include build/config.mk
include build/dirs.mk
include build/toolchain.mk
include build/flags.mk
include build/kernel-sources.mk
```

Standard freestanding flags:

```makefile
CFLAGS ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pie -fno-pic -Wall -Wextra -g -O0 \
          -mno-sse -mno-sse2 -mno-mmx
```

Build artifacts go into `build/` directories. Never pollute the source tree.

---

## 13. Common Patterns and Anti-Patterns

**Do:** Initialize variables at declaration, use explicit-width types for hardware data, cast away unused parameters with `(void)argc;`, use `size_t` for sizes, use designated initializers.

**Don't:** Use magic numbers (define named constants), silently ignore errors, use global mutable state without `static`, skip integer overflow checks in size calculations.

**Do:** Keep signal handlers minimal (only set `volatile sig_atomic_t` flags), document non-obvious design decisions, use `goto` for single-function error-cleanup paths.

**Don't:** Use `goto` across functions, introduce new camelCase names, put `*` next to the type in pointer declarations.

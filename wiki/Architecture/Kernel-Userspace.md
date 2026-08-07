# Kernel-Userspace Interface

How Forest OS bridges the gap between user applications and the kernel.

## Table of Contents

1. [Overview](#overview)
2. [System Call Mechanism](#system-call-mechanism)
3. [Register Conventions](#register-conventions)
4. [Linux-Compatible Syscall Numbers](#linux-compatible-syscall-numbers)
5. [The libc Middleman Layer](#the-libc-middleman-layer)
6. [Error Handling (errno)](#error-handling-errno)
7. [Supported Syscalls Overview](#supported-syscalls-overview)
8. [C Runtime Startup (crt0.S)](#c-runtime-startup)
9. [Linker Scripts and Memory Layout](#linker-scripts-and-memory-layout)
10. [ELF Binary Format and Loading](#elf-binary-format-and-loading)
11. [How I/O Works](#how-i-o-works)

---

## Overview

Every operating system needs a clean boundary between what user programs can do and what only the kernel can do. In Forest OS, this boundary is defined by **system calls** (syscalls) -- carefully controlled entry points that let applications ask the kernel to perform privileged operations like reading files, allocating memory, or creating processes.

The flow looks like this:

```
Application code
    |
    v
libc function (e.g., read())
    |
    v
libc syscall wrapper (inline assembly)
    |
    v
int 0x80 / syscall instruction
    |
    v
Kernel syscall handler
    |
    v
Kernel performs the operation
    |
    v
Return value in EAX/RAX
```

Applications never talk to the kernel directly. They call standard C library functions, and libc handles all the messy details: putting arguments in the right registers, issuing the right CPU instruction, translating error codes, and setting `errno`.

---

## System Call Mechanism

Forest OS supports two syscall mechanisms depending on the CPU architecture.

### x86 (32-bit): `int 0x80`

On 32-bit x86, syscalls are issued via software interrupt `0x80`. The kernel registers an interrupt handler for vector `0x80` during boot. When a user program executes `int $0x80`, the CPU switches to kernel mode and jumps to the handler.

Here's what happens in the libc's `read()` wrapper (from `libs/libc/src/syscalls.c`):

```c
static inline syscall_ret_t __syscall3(syscall_arg_t num, syscall_arg_t a1,
                                        syscall_arg_t a2, syscall_arg_t a3) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)                    // output: EAX = return value
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)  // inputs
        : "memory"
    );
    return ret;
}
```

And `read()` itself:

```c
ssize_t read(int fd, void *buf, size_t count) {
    return __syscall_ret(__syscall3(SYS_read, fd, (syscall_arg_t)buf, count));
}
```

The pattern is always the same: load the syscall number into EAX, load arguments into EBX/ECX/EDX/ESI/EDI/EBP, fire `int $0x80`, and read the result from EAX.

### x86_64 (64-bit): Still `int 0x80`

Interestingly, Forest OS uses `int $0x80` even on 64-bit builds, rather than the `syscall` instruction that Linux uses. This is a deliberate choice -- `int $0x80` is simpler to implement and works on both 32-bit and 64-bit. The kernel's syscall handler can inspect whether the interrupted code was running in 32-bit or 64-bit mode and dispatch accordingly.

The 64-bit versions use the same `int $0x80` instruction but with 64-bit registers:

```c
static inline syscall_ret_t __syscall3(syscall_arg_t num, syscall_arg_t a1,
                                        syscall_arg_t a2, syscall_arg_t a3) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}
```

The 6-argument variant is special -- it needs to push RBP, load the 6th argument into RBP, issue the syscall, then pop RBP:

```c
static inline syscall_ret_t __syscall6(syscall_arg_t num, syscall_arg_t a1, syscall_arg_t a2,
                                        syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5,
                                        syscall_arg_t a6) {
    syscall_ret_t ret;
    __asm__ __volatile__(
        "push %%rbp\n"
        "mov %[arg6], %%rbp\n"
        "int $0x80\n"
        "pop %%rbp\n"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), [arg6]"r"(a6)
        : "memory"
    );
    return ret;
}
```

---

## Register Conventions

The register layout follows the Linux i386 ABI convention for both 32-bit and 64-bit:

| Register | x86 Name | x86_64 Name | Purpose |
|----------|----------|-------------|---------|
| 1st | EBX | RDI | Syscall argument 1 |
| 2nd | ECX | RSI | Syscall argument 2 |
| 3rd | EDX | RDX | Syscall argument 3 |
| 4th | ESI | R10 | Syscall argument 4 |
| 5th | EDI | R8 | Syscall argument 5 |
| 6th | EBP | R9 | Syscall argument 6 |
| Number | EAX | RAX | Syscall number (in) / return value (out) |

The kernel-side handler captures these registers into a `syscall_frame_t` structure:

```c
// From fern/src/include/syscall.h
typedef struct {
    uint32 edi;   // Argument 5 (pusha pushes EDI first, at lowest address)
    uint32 esi;   // Argument 4
    uint32 ebp;   // Argument 6
    uint32 esp;   // Original ESP (saved by pusha)
    uint32 ebx;   // Argument 1
    uint32 edx;   // Argument 3
    uint32 ecx;   // Argument 2
    uint32 eax;   // Syscall number and return value
} syscall_frame_t;
```

Notice the order is reversed from what you might expect -- that's because `pusha` pushes EAX first (highest address) and EDI last (lowest address).

---

## Linux-Compatible Syscall Numbers

Forest OS uses **Linux x86_64 syscall numbers** as its canonical set. This is a huge deal for portability. The syscall number table in `fern/src/include/syscall.h` maps directly to Linux:

```c
enum syscall_number {
    SYS_READ        = 0,
    SYS_WRITE       = 1,
    SYS_OPEN        = 2,
    SYS_CLOSE       = 3,
    SYS_STAT        = 4,
    SYS_FSTAT       = 5,
    SYS_LSEEK       = 8,
    SYS_MMAP        = 9,
    SYS_MUNMAP      = 11,
    SYS_BRK         = 12,
    SYS_IOCTL       = 16,
    SYS_PIPE        = 22,
    SYS_DUP         = 32,
    SYS_DUP2        = 33,
    SYS_NANOSLEEP   = 35,
    SYS_GETPID      = 39,
    SYS_SOCKET      = 41,
    SYS_CONNECT     = 42,
    SYS_ACCEPT      = 43,
    SYS_FORK        = 57,
    SYS_EXECVE      = 59,
    SYS_EXIT        = 60,
    SYS_WAIT4       = 61,
    SYS_KILL        = 62,
    SYS_UNAME       = 63,
    SYS_FCNTL       = 72,
    SYS_GETCWD      = 79,
    SYS_CHDIR       = 80,
    SYS_MKDIR       = 83,
    SYS_RMDIR       = 84,
    SYS_UNLINK      = 87,
    SYS_CHMOD       = 90,
    SYS_CHOWN       = 92,
    SYS_GETUID      = 102,
    SYS_GETGID      = 104,
    SYS_GETTIMEOFDAY = 96,
    SYS_CLOCK_GETTIME = 230,
    SYS_REBOOT      = 171,
    SYS_MOUNT       = 167,
    SYS_UMOUNT2     = 168,
    // ... hundreds more
};
```

Forest OS also maintains a separate set of i386 syscall numbers for 32-bit compatibility (prefixed with `I386_SYS_`), since 32-bit Linux uses different numbers for many syscalls.

### Why This Matters

Using Linux syscall numbers means:

- **Binary compatibility** -- simple ELF programs compiled for Linux can potentially run on Forest OS (and vice versa)
- **Familiar tooling** -- developers who know Linux systems programming can apply that knowledge directly
- **Standard documentation** -- Linux man pages, tutorials, and references all apply
- **Easier porting** -- porting a Linux program to Forest OS often just requires recompilation with the Forest OS libc

### Forest OS Extensions

On top of the Linux-compatible base, Forest OS adds its own syscall numbers starting around 470+ for OS-specific features:

```c
    SYS_NETINFO           = 470,  // Network information
    SYS_MMAP_FB           = 471,  // Map framebuffer
    SYS_MUNMAP_FB         = 472,  // Unmap framebuffer
    SYS_GET_FB_INFO       = 473,  // Get framebuffer info
    SYS_POWER             = 474,  // Power management
    SYS_USERCTL           = 475,  // User management
    SYS_SOUND_PLAY        = 482,  // Play audio
    SYS_SPAWN_TASK        = 490,  // Spawn ELF from VFS
    SYS_CHVT              = 518,  // Switch virtual terminal
    SYS_SET_FB_MODE       = 529,  // Set display resolution
    SYS_GL_INIT           = 540,  // OpenGL init (Fern extension)
```

---

## The libc Middleman Layer

Applications don't make raw syscalls. They call libc functions. The libc provides several important services:

### Architecture Abstraction

Your C code looks the same whether you're on x86 or x86_64:

```c
// This works on both architectures
ssize_t bytes = read(fd, buffer, size);
```

The libc internally selects the right inline assembly for the target architecture.

### Error Translation

The kernel returns **negative** values on error (e.g., `-ENOENT` which is `-2`). The libc translates this to the POSIX convention:

```c
static inline long __syscall_ret(syscall_ret_t ret) {
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);   // Set errno to positive error code
        return -1;             // Return -1 to caller
    }
    return (long)ret;          // Return actual value on success
}
```

For functions that return pointers (like `mmap`):

```c
static inline void *__syscall_ret_ptr(syscall_ret_t ret) {
    if (ret < 0 && ret > -4096) {
        errno = (int)(-ret);
        return (void *)-1;    // MAP_FAILED
    }
    return (void *)ret;
}
```

The range check `ret > -4096` is important -- it distinguishes kernel errors from valid negative return values (like offsets).

### Data Type Conversion

The libc handles conversions between userspace types and kernel types as needed. For example, `open()` is variadic in C but the syscall takes a fixed number of arguments:

```c
int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (flags & 0x40) {  // O_CREAT
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return __syscall_ret(__syscall3(SYS_open, (syscall_arg_t)pathname, flags, mode));
}
```

### The Generic `syscall()` Function

For maximum flexibility, the libc also exposes a generic `syscall()` function:

```c
long syscall(long number, ...) {
    va_list ap;
    syscall_arg_t args[6];
    
    va_start(ap, number);
    for (int i = 0; i < 6; i++) {
        args[i] = va_arg(ap, syscall_arg_t);
    }
    va_end(ap);
    
    return __syscall_ret(__syscall6(number, args[0], args[1], args[2],
                                    args[3], args[4], args[5]));
}
```

This lets applications call any syscall by number, even if libc doesn't have a dedicated wrapper.

---

## Error Handling (errno)

Forest OS follows the standard POSIX error convention, using the same error codes as Linux.

### The errno Variable

`errno` is defined in `libs/libc/include/libc/errno.h`:

```c
extern int errno;

int *__errno_location(void);
#define errno (*__errno_location())
```

The `__errno_location()` function exists for thread-safety -- in a multithreaded environment, each thread would have its own `errno`. In Forest OS's current implementation, it returns a pointer to a global variable:

```c
int __errno_storage = 0;

int *__errno_location(void) {
    return &__errno_storage;
}
```

### Error Code Values

All error codes use Linux's exact values:

```c
#define EPERM           1   // Operation not permitted
#define ENOENT          2   // No such file or directory
#define ESRCH           3   // No such process
#define EINTR           4   // Interrupted system call
#define EIO             5   // Input/output error
#define EBADF           9   // Bad file descriptor
#define ENOMEM          12  // Cannot allocate memory
#define EACCES          13  // Permission denied
#define EEXIST          17  // File exists
#define EINVAL          22  // Invalid argument
#define ENOTDIR         20  // Not a directory
#define EISDIR          21  // Is a directory
#define ENOSPC          28  // No space left on device
// ... over 130 error codes
```

### Using perror()

The `perror()` function prints a human-readable error message:

```c
void perror(const char *s) {
    const char *err_str = strerror(errno);
    
    if (s && *s) {
        write(STDERR_FILENO, s, strlen(s));
        write(STDERR_FILENO, ": ", 2);
    }
    write(STDERR_FILENO, err_str, strlen(err_str));
    write(STDERR_FILENO, "\n", 1);
}
```

Example usage:

```c
int fd = open("/etc/config", O_RDONLY);
if (fd < 0) {
    perror("open failed");  // prints "open failed: No such file or directory"
    return 1;
}
```

---

## Supported Syscalls Overview

The libc implements wrappers for a large number of syscalls. Here's a categorized overview:

### File I/O
`read`, `write`, `open`, `openat`, `close`, `lseek`, `stat`, `fstat`, `lstat`, `access`, `fsync`, `fdatasync`, `ftruncate`, `truncate`, `dup`, `dup2`, `dup3`, `pipe`, `pipe2`, `fcntl`, `ioctl`

### Directory Operations
`mkdir`, `rmdir`, `chdir`, `fchdir`, `getcwd`, `getdents`, `getdents64`, `link`, `unlink`, `symlink`, `readlink`, `rename`

### File Permissions
`chmod`, `fchmod`, `chown`, `fchown`, `lchown`, `umask`

### Memory Management
`mmap`, `munmap`, `mprotect`, `msync`, `madvise`, `mlock`, `munlock`, `mlockall`, `munlockall`, `brk`

### Process Control
`fork`, `vfork`, `execve`, `_exit`, `wait`, `waitpid`, `getpid`, `getppid`, `getpgrp`, `setpgid`, `setsid`, `getsid`, `gettid`, `kill`, `sched_yield`, `nice`

### User/Group IDs
`getuid`, `geteuid`, `getgid`, `getegid`, `setuid`, `setgid`, `seteuid`, `setegid`, `setreuid`, `setregid`

### Signals
`sigaction`, `sigprocmask`, `sigpending`, `sigsuspend`

### Networking
`socket`, `bind`, `listen`, `accept`, `accept4`, `connect`, `shutdown`, `send`, `recv`, `sendto`, `recvfrom`, `getsockopt`, `setsockopt`, `getsockname`, `getpeername`, `socketpair`

### Time
`time`, `gettimeofday`, `settimeofday`, `clock_gettime`, `clock_settime`, `clock_getres`, `nanosleep`, `sleep`, `usleep`, `alarm`, `pause`

### System Info
`uname`, `gethostname`, `sethostname`

### Mount/Filesystem
`mount`, `umount`, `umount2`, `sync`, `chroot`

### Polling/Select
`poll`, `select`, `readv`, `writev`

### Misc
`getrandom`, `reboot`, `mknod`

### Forest OS Extensions
`poweroff`, `mmap_fb`, `munmap_fb`, `start_fb_watcher`, `stop_fb_watcher`, `fb_flush`, `read_kbd_event`, `read_mouse_event`, `poll_input`, `netinfo`, `user_syscall`

---

## C Runtime Startup

Before `main()` is ever called, there's a tiny but crucial piece of assembly code that sets up the C runtime: `userspace/crt0.S`.

```asm
/* Forest-OS userspace C runtime startup */
    .text
    .globl _start

_start:
    /* Stack already has argc, argv, envp from kernel */
    /* argc is at [esp] */
    /* argv is at [esp+4] */
    /* envp is at [esp+4+argc*4] */
    call main
    /* main returned - exit with return value */
    pushl %eax
    call exit
    /* Should never reach here */
    hlt
```

### What Happens at Startup

When the kernel loads and starts an ELF executable, it:

1. Maps the program's segments into virtual memory (text, data, bss)
2. Sets up the initial stack with:
   - `argc` (argument count)
   - `argv` (argument vector -- array of pointers to strings)
   - `envp` (environment vector -- array of pointers to KEY=VALUE strings)
3. Jumps to the ELF entry point, which is `_start`

The `_start` function:

1. Reads `argc`, `argv`, and `envp` from the stack (the kernel already placed them there)
2. Calls `main(argc, argv, envp)` -- or in this simple version, just calls `main()` (which works because `main` can read its arguments from registers/stack as set up by the caller)
3. When `main()` returns, pushes the return value (in EAX) onto the stack
4. Calls `exit()` to clean up and terminate the process
5. If `exit()` somehow returns (it shouldn't), executes `hlt` to halt the CPU

This is the minimum viable C runtime. More sophisticated implementations (like glibc's `crt1.o`) also:
- Initialize TLS (Thread-Local Storage)
- Call constructors (functions marked `__attribute__((constructor))`)
- Set up the atexit handler chain
- Initialize the I/O subsystem

Forest OS keeps it simple: the kernel does the heavy lifting, and `crt0.S` just bridges the gap to `main()`.

---

## Linker Scripts and Memory Layout

The linker scripts tell the linker how to arrange sections of the program in memory.

### 32-bit: `userspace/link.ld`

```ld
ENTRY(_start)

SECTIONS
{
    . = 0x08048000;  /* Classic Linux ELF base address */

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    _end = .;
}
```

### 64-bit: `userspace/link64.ld`

```ld
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x00400000 + SIZEOF_HEADERS;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu.*)
    }
}
```

### Memory Layout Explained

The layout from lowest to highest address:

```
0x08048000 (32-bit) or 0x00400000 (64-bit)
    |
    v
+---------------+
| ELF Header    |  (e_ident, e_entry, etc.)
+---------------+
| Program Hdrs  |  (describes loadable segments)
+---------------+
| .text         |  Read-only executable code
+---------------+
| .rodata       |  Read-only data (string literals, constants)
+---------------+
| .data         |  Initialized global/static variables
+---------------+
| .bss          |  Zero-initialized globals, COMMON symbols
+---------------+
|    heap       |  (grows upward via brk/mmap)
    ...
    ...
    ...         |  (unused virtual address space)
    ...
    ...
|    stack      |  (grows downward)
+---------------+
0xC0000000 (top of user address space on 32-bit)
```

Key points:

- **`.text`** is loaded first and is read+execute only (security: prevents code injection)
- **`.rodata`** is read-only (security: prevents modification of constants)
- **`.data`** is read-write (for mutable globals)
- **`.bss`** takes no space in the file -- the kernel just maps it as zero-filled pages
- The `_end` symbol marks the end of the BSS segment, used by `sbrk()`/`malloc()`
- On 64-bit, `.comment`, `.note*`, and `.eh_frame*` sections are discarded to reduce binary size

The base addresses are historical conventions: `0x08048000` is the classic Linux i386 ELF base, and `0x00400000` is the classic Linux x86_64 ELF base.

---

## ELF Binary Format and Loading

Forest OS loads programs in the **ELF (Executable and Linkable Format)** -- the standard binary format on Linux and most Unix-like systems.

### ELF Header Structure

Every ELF file starts with an ELF header:

```c
typedef struct {
    uint8  e_ident[16];  // Magic: 0x7F 'E' 'L' 'F', then class, data encoding, etc.
    uint16 e_type;       // ET_EXEC (2) for executables, ET_DYN (3) for shared objects
    uint16 e_machine;    // EM_386 (3) for x86, or EM_X86_64 for x86_64
    uint32 e_version;    // ELF version (1)
    uint32 e_entry;      // Entry point virtual address (_start)
    uint32 e_phoff;      // Offset to program header table
    uint32 e_shoff;      // Offset to section header table
    uint32 e_flags;      // Architecture-specific flags
    uint16 e_ehsize;     // ELF header size
    uint16 e_phentsize;  // Size of each program header entry
    uint16 e_phnum;      // Number of program headers
    uint16 e_shentsize;  // Size of each section header entry
    uint16 e_shnum;      // Number of section headers
    uint16 e_shstrndx;   // Index of section name string table
} elf32_ehdr_t;
```

### Program Headers (Segments)

Program headers describe how to load the binary into memory. The most important type is `PT_LOAD`:

```c
typedef struct {
    uint32 p_type;    // PT_LOAD (1) for loadable segments
    uint32 p_offset;  // File offset where this segment starts
    uint32 p_vaddr;   // Virtual address to load at
    uint32 p_paddr;   // Physical address (usually ignored in userspace)
    uint32 p_filesz;  // Size of segment in the file
    uint32 p_memsz;   // Size of segment in memory (>= filesz; extra is zero-filled = BSS)
    uint32 p_flags;   // PF_R (4), PF_W (2), PF_X (1)
    uint32 p_align;   // Alignment
} elf32_phdr_t;
```

### The Loading Process

When Forest OS's kernel loads an ELF executable:

1. **Validate** the ELF header (check magic bytes, class, machine type)
2. **Read program headers** -- iterate through all `PT_LOAD` segments
3. **Map memory** -- for each loadable segment, allocate physical pages and map them to the correct virtual addresses
4. **Copy data** -- copy the segment contents from the file into memory
5. **Handle BSS** -- if `p_memsz > p_filesz`, the extra bytes are zero-initialized
6. **Set up page permissions** -- read-only for `.text` and `.rodata`, read-write for `.data` and `.bss`
7. **Set the entry point** -- from `e_entry` in the ELF header (or adjusted for PIE binaries)
8. **Set up the stack** -- push `argc`, `argv`, `envp` onto the new process's stack
9. **Jump to `_start`** -- begin execution at the entry point

The kernel also supports **position-independent executables** (PIE/ET_DYN) with a load bias:

```c
typedef struct {
    bool   is_pie;       // Whether this is a position-independent executable
    uint32 load_bias;    // Offset applied to ET_DYN segments/entry
    char   interp[128];  // PT_INTERP path (dynamic linker, e.g., /lib/ld.so.1)
    bool   has_interp;   // Whether a PT_INTERP segment was present
    bool   has_dynamic;  // Whether a PT_DYNAMIC segment was present
} elf_load_info_t;
```

---

## How I/O Works

### File Descriptors

Every open file is represented by an integer **file descriptor** (fd). The first three fds are special:

```c
#define STDIN_FILENO  0   // Standard input (keyboard)
#define STDOUT_FILENO 1   // Standard output (display)
#define STDERR_FILENO 2   // Standard error (display)
```

These are inherited by child processes and are typically connected to the terminal.

### The FILE Structure

The libc wraps file descriptors in a `FILE` structure that adds buffering:

```c
typedef struct _FILE {
    int fd;             // Underlying file descriptor
    int flags;          // Mode and status flags
    char *buffer;       // Buffer pointer
    size_t buf_size;    // Buffer size
    size_t buf_pos;     // Current position in buffer
    size_t buf_len;     // Valid data length in buffer
    int error;          // Error indicator
    int eof;            // End-of-file indicator
    int unget;          // Character pushed back by ungetc (-1 if none)
} FILE;
```

### Buffering

Forest OS's stdio implements three buffering modes (though full buffering isn't fully implemented yet):

- **`_IONBF`** (0) -- Unbuffered. Every `fputc()` triggers a `write()` syscall. Good for stderr.
- **`_IOLBF`** (1) -- Line buffered. Output is flushed on newline. Good for stdout when connected to a terminal.
- **`_IOFBF`** (2) -- Fully buffered. Output is flushed when the buffer is full or on `fflush()`. Good for files.

Currently, Forest OS's stdio uses direct writes (effectively unbuffered) for simplicity:

```c
int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    ssize_t n = write(stream->fd, &ch, 1);  // One byte = one syscall
    if (n < 0) {
        stream->error = 1;
        return EOF;
    }
    return c;
}
```

### printf() Implementation

The `printf()` family is implemented with a custom format engine (no external dependencies). It supports:

- `%s` -- string
- `%c` -- character
- `%d` / `%i` -- signed decimal integer
- `%u` -- unsigned decimal integer
- `%x` / `%X` -- hexadecimal
- `%p` -- pointer (prefixed with `0x`)
- `%%` -- literal percent

The formatted output is written to a 4096-byte stack buffer, then flushed to the file descriptor:

```c
int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[4096];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    
    if (len > 0) {
        size_t to_write = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        ssize_t n = write(stream->fd, buf, to_write);
        if (n < 0) {
            stream->error = 1;
            return -1;
        }
    }
    
    return len;
}
```

### Practical Example: Shell Using Syscalls

The Forest OS shell (`userspace/forest-shell/shell.c`) shows how real programs use the syscall interface:

```c
// Reading input character by character
static int read_char(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1)
        return c;
    return -1;
}

// Executing a command
pid_t pid = fork();
if (pid == 0) {
    // Child process
    setpgid(0, 0);              // New process group
    signal(SIGINT, SIG_DFL);    // Default signal handling
    execv(cmd, argv);           // Replace with new program
    exit(127);                  // exec failed
}
if (pid > 0) {
    int st;
    waitpid(pid, &st, 0);       // Wait for child
    if (WIFEXITED(st))
        g_last_status = WEXITSTATUS(st);
}
```

### Changing Directory

```c
static int builtin_cd(int argc, char **argv) {
    const char *target;
    if (argc < 2 || streq(argv[1], "~")) {
        target = getenv("HOME");
        if (!target) target = "/";
    } else {
        target = argv[1];
    }

    if (chdir(target) != 0) {
        fprintf(stderr, "forest-shell: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    return 0;
}
```

### Reading Kernel Messages

```c
static int builtin_dmesg(int argc, char **argv) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        fd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        close(fd);
        return 0;
    }
    // ... fallback to external dmesg command
}
```

---

## Summary

The Forest OS kernel-userspace interface is designed around a few core principles:

1. **Linux compatibility** -- same syscall numbers, same error codes, same POSIX API. This makes Forest OS immediately accessible to anyone familiar with Linux systems programming.

2. **libc as middleman** -- applications never touch raw syscalls. The libc handles architecture differences, error translation, type conversions, and buffering.

3. **Simple startup** -- a 17-line `crt0.S` is all it takes to bootstrap from kernel-provided stack arguments to `main()`.

4. **Standard ELF loading** -- the kernel uses the same binary format as Linux, with the same section layout and memory mapping semantics.

5. **Extensibility** -- Forest OS adds its own syscalls (framebuffer, audio, display management) at high syscall numbers without breaking the Linux-compatible base.

This design lets Forest OS benefit from the enormous body of Linux knowledge, tools, and programming practices while maintaining the freedom to extend the system with OS-specific features.

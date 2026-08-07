# Forest OS libc

A lightweight, Linux-compatible C standard library for Forest OS.

**This is the consolidated libc** - all headers, sources, and build integration
for the entire Forest OS ecosystem (kernel, toolchain, userspace apps) live here.

## Overview

Forest OS libc provides a **middleman layer** between userspace applications and the Forest OS kernel. Instead of making raw system calls directly, applications call standard C library functions which handle:

1. **Architecture Abstraction**: System call mechanisms differ between CPU architectures (x86 uses `int 0x80`, x86_64 can use `syscall` instruction). The libc handles these details, allowing portable C code.
2. **Error Translation**: The kernel returns negative values on error (e.g., `-ENOENT`). The libc translates these to the POSIX convention: return `-1` and set `errno` to the positive error code.
3. **Data Type Conversion**: The libc converts between userspace types and kernel types as needed.
4. **Buffering and Optimization**: Functions like `printf()` buffer output to reduce system call overhead.
5. **Standard Compliance**: Applications use familiar POSIX/C standard functions, making code portable between Forest OS and Linux.

## Directory Structure

```
libs/libc/
├── include/libc/       # Header files
│   ├── sys/           # System headers (types.h, stat.h, socket.h, etc.)
│   ├── arpa/          # ARPA network headers
│   ├── netinet/       # Network protocol headers
│   ├── stdio.h        # Standard I/O
│   ├── stdlib.h       # General utilities
│   ├── string.h       # String handling
│   ├── unistd.h       # POSIX API
│   ├── errno.h        # Error numbers
│   ├── signal.h       # Signal handling
│   ├── pthread.h      # POSIX threads
│   └── ...            # Other standard headers
├── src/               # Implementation files
│   ├── syscalls.c     # Low-level system call wrappers
│   ├── string.c       # String functions
│   ├── stdlib.c       # Memory allocation, conversions
│   ├── stdio.c        # Buffered I/O
│   ├── errno.c        # Error handling
│   ├── pthread.c      # POSIX threads (stubs)
│   ├── signal.c       # Signal handling
│   ├── dirent.c       # Directory operations
│   └── assert.c       # Assertion handling
├── Makefile.inc       # Build system integration
└── README.md          # This file
```

## System Call Architecture

### How System Calls Work

When an application calls a libc function like `read()`:

```c
ssize_t bytes = read(fd, buffer, size);
```

The libc `read()` function:
1. Places arguments in the correct CPU registers
2. Places the system call number in the `eax`/`rax` register
3. Issues `int $0x80` to transfer control to the kernel
4. Receives the return value in `eax`/`rax`
5. Checks for errors (negative return = error)
6. Sets `errno` if needed and returns `-1` or the actual value

### Register Conventions (x86/x86_64)

| Register | Purpose |
|----------|---------|
| eax/rax | Syscall number (in), return value (out) |
| ebx/rdi | Argument 1 |
| ecx/rsi | Argument 2 |
| edx/rdx | Argument 3 |
| esi/r10 | Argument 4 |
| edi/r8 | Argument 5 |
| ebp/r9 | Argument 6 |

## Linux Compatibility

Forest OS libc uses **Linux-compatible system call numbers** for maximum compatibility. This means:
- Programs compiled for Forest OS can potentially run on Linux (and vice versa for simple programs)
- Developers familiar with Linux programming can use their existing knowledge
- Standard tools and documentation apply

### Supported System Calls

The libc implements wrappers for:
- **File I/O**: read, write, open, close, lseek, stat, fstat, etc.
- **Memory**: mmap, munmap, mprotect, brk
- **Process**: fork, execve, exit, wait, getpid, kill
- **Networking**: socket, bind, listen, accept, connect, send, recv
- **Time**: time, gettimeofday, nanosleep, clock_gettime
- **Signals**: sigaction, sigprocmask, kill
- **And many more...**

### Forest OS Extensions

Additional system calls for Forest OS specific features:
- `mmap_fb()` / `munmap_fb()`: Framebuffer mapping
- `poweroff()` / `reboot()`: Power management
- `read_kbd_event()` / `read_mouse_event()`: Direct input
- `netinfo()`: Network information

## Building

### Using with Forest OS Build System

Include `Makefile.inc` in your build:

```makefile
include libs/libc/Makefile.inc

# Use LIBC_INCLUDES for include paths
CFLAGS += $(LIBC_INCLUDES)

# Link against libforest.a
my_program: my_program.o $(LIBC_LIBRARY)
    $(LD) -o $@ $^
```

### Standalone Build

```bash
# Create object directory
mkdir -p obj/forestlibc

# Compile sources
for src in libs/libc/src/*.c; do
    gcc -ffreestanding -nostdinc -Ilibs/libc/include/libc \
        -c $src -o obj/forestlibc/$(basename $src .c).o
done

# Create static library
ar rcs obj/libforest.a obj/forestlibc/*.o
```

## Writing Applications

### Simple Example

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    printf("Hello from Forest OS!\n");
    
    char *str = malloc(100);
    if (str) {
        strcpy(str, "Dynamic memory works!");
        puts(str);
        free(str);
    }
    
    return 0;
}
```

### File I/O Example

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    FILE *fp = fopen("/etc/config", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            printf("%s", line);
        }
        fclose(fp);
    }
    
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        write(fd, "test", 4);
        close(fd);
    } else {
        perror("open failed");
    }
    
    return 0;
}
```

## Limitations

1. **Threading**: Full pthreads is not yet implemented. Mutex/condition variable operations work for single-threaded synchronization but actual thread creation returns `EAGAIN`.
2. **Signals**: Basic signal handling is supported, but some advanced features (sigqueue, sigwaitinfo) return `ENOSYS`.
3. **Locale**: Only the C locale is supported. Wide character and multibyte functions have minimal implementations.
4. **Floating Point**: Math functions use software implementations via Taylor series. Hardware FPU support depends on kernel configuration.

## Contributing

When adding new functionality:
1. Add the header declaration in `include/libc/`
2. Implement in the appropriate `src/*.c` file
3. If it's a syscall wrapper, add to `src/syscalls.c`
4. Follow existing code style (4-space indent, snake_case)
5. Document with comments explaining the function

## License

Forest OS libc is part of Forest OS and follows the same license terms.

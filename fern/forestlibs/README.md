# Forest Libraries (forestlibs)

This directory contains libraries designed for Forest OS that are also compatible with Linux applications.

## Components

### libc - C Standard Library

A lightweight, Linux-compatible C standard library implementation.

**Features:**
- **Linux ABI Compatible**: Uses the same system call numbers as Linux x86_64
- **POSIX Compliant**: Implements standard POSIX interfaces
- **Portable**: Supports both 32-bit and 64-bit architectures
- **Minimal Dependencies**: Freestanding implementation with no external dependencies

**Key Functionality:**
- Standard I/O (`stdio.h`): printf, scanf, fopen, fread, etc.
- String handling (`string.h`): strlen, strcpy, memcpy, etc.
- Memory management (`stdlib.h`): malloc, free, realloc
- System calls (`unistd.h`): read, write, open, close, fork, exec
- Signals (`signal.h`): signal handling and manipulation
- Time (`time.h`): time functions and structures
- Networking (`sys/socket.h`): socket operations
- Threading stubs (`pthread.h`): POSIX threads API (single-threaded stubs)

See `libc/README.md` for detailed documentation.

## Why "forestlibs"?

These libraries are:
1. **Made for Forest OS**: Designed specifically to work with the Forest OS kernel
2. **Linux Compatible**: Use the same system call interface as Linux
3. **Unix-like**: Follow POSIX standards used by Unix-like operating systems

This means:
- Applications compiled with forestlibs can be ported between Forest OS and Linux
- Developers can use familiar POSIX/C standard APIs
- Existing documentation and knowledge applies

## Directory Structure

```
forestlibs/
├── libc/                   # C Standard Library
│   ├── include/           # Public headers
│   │   ├── sys/          # System headers
│   │   ├── netinet/      # Network headers
│   │   ├── arpa/         # ARPA headers
│   │   └── *.h           # Standard C headers
│   ├── src/              # Implementation
│   │   ├── syscalls.c    # System call wrappers
│   │   ├── string.c      # String functions
│   │   ├── stdlib.c      # Standard library
│   │   ├── stdio.c       # Standard I/O
│   │   └── ...           # Other implementations
│   ├── Makefile.inc      # Build rules
│   └── README.md         # Library documentation
└── README.md             # This file
```

## Building

### Include in Forest OS Build

Add to your main Makefile:

```makefile
include forestlibs/libc/Makefile.inc
```

### Build Standalone

```bash
make forestlibc
```

This creates `obj/libforest.a` - a static library.

## Using in Applications

### Compile

```bash
# Cross-compile for Forest OS
i686-forestos-gcc -c myapp.c -Iforestlibs/libc/include

# Link
i686-forestos-ld -o myapp myapp.o -Lobj -lforest
```

### Example Application

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    // Standard I/O
    printf("Hello, Forest OS!\n");
    
    // Memory allocation
    char *buf = malloc(1024);
    if (buf) {
        // System call via libc wrapper
        ssize_t n = read(0, buf, 1024);
        if (n > 0) {
            write(1, buf, n);
        }
        free(buf);
    }
    
    return 0;
}
```

## Architecture

### System Call Flow

```
Application
    │
    ▼
┌─────────────┐
│   libc      │  ◄── Standard function interface
│ (forestlibs)│      (read, write, printf, etc.)
└─────────────┘
    │
    │  int $0x80 (syscall interrupt)
    ▼
┌─────────────┐
│   Kernel    │  ◄── Handles system calls
│ (Forest OS) │      Returns results
└─────────────┘
    │
    ▼
┌─────────────┐
│  Hardware   │
└─────────────┘
```

### libc as Middleman

The libc provides:
1. **Abstraction**: Hides architecture-specific syscall mechanisms
2. **Error Handling**: Translates kernel errors to errno
3. **Buffering**: Optimizes I/O operations (stdio)
4. **Compatibility**: Provides familiar POSIX interface

## Future Libraries

Planned additions to forestlibs:
- **libm** - Math library with hardware FPU support
- **libpthread** - Full threading support (when kernel supports it)
- **libcrypto** - Cryptographic functions
- **libz** - Compression library

## Contributing

When adding to forestlibs:
1. Follow existing code style (4-space indent, snake_case)
2. Maintain Linux/POSIX compatibility where possible
3. Document with comments
4. Test on both 32-bit and 64-bit builds
5. Update relevant README files

## License

Part of Forest OS - follows project license.

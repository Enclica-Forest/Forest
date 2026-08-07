# Forest OS Userspace Overview

The userspace layer is where all the action happens in Forest OS. While the kernel
handles low-level hardware and process management, the 45 userspace applications
provide the tools users actually interact with -- from `cat` and `ls` to the
interactive shell and graphical frontend.

Every binary in the initrd is cross-compiled for Forest OS using the dedicated
`i686-forestos` (32-bit) or `x86_64-forestos` (64-bit) toolchain. No host gcc
is used.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Building Userspace Apps](#building-userspace-apps)
3. [The C Runtime Startup](#the-c-runtime-startup)
4. [Linker Scripts](#linker-scripts)
5. [The Stubs Layer](#the-stubs-layer)
6. [All 45 Applications](#all-45-applications)
7. [Application Categories](#application-categories)
8. [Common Patterns Across Apps](#common-patterns-across-apps)
9. [App Size and Complexity](#app-size-and-complexity)
10. [How Apps Interact with the Kernel](#how-apps-interact-with-the-kernel)
11. [Placing Apps in the initrd](#placing-apps-in-the-initrd)
12. [Adding a New Application](#adding-a-new-application)

---

## Architecture Overview

Forest OS follows a classic Unix architecture:

```
+-----------------------------------------------+
|              Userspace (45 apps)               |
|  cat, ls, grep, shell, init, sudo, ...        |
+-----------------------------------------------+
|         Forest OS C Library (libc)             |
|  Standard POSIX API + syscall wrappers         |
+-----------------------------------------------+
|    Stubs Layer (forest_stubs.c)                |
|  Missing functions: getopt, fnmatch,           |
|  basename, dirname, sscanf, snprintf, etc.     |
+-----------------------------------------------+
|         C Runtime (crt0.S)                     |
|  _start -> calls main() -> calls exit()        |
+-----------------------------------------------+
|              Forest OS Kernel                  |
|  Syscalls: read, write, open, stat, fork,      |
|  exec, wait, mount, kill, reboot, etc.         |
+-----------------------------------------------+
```

All userspace code lives under `userspace/`. Each application is a subdirectory
containing a single C source file and a `Makefile`. The master `Makefile` at the
top level orchestrates building everything.

---

## Building Userspace Apps

### Toolchain

The cross-compiler is the **only** compiler used. Host gcc is never invoked for
userspace binaries. The toolchain is located at:

```
forestos-toolchain/install/bin/{i686,x86_64}-forestos-{gcc,ld,strip}
```

The master Makefile requires this toolchain and will error immediately if it's
missing:

```makefile
ifeq ($(wildcard $(TOOLCHAIN_BIN)/$(FORESTOS_TOOLCHAIN_PREFIX)-gcc),)
    $(error Forest-OS cross-toolchain not found. Build it first.)
endif
```

### Architecture Selection

Building for 32-bit (default) or 64-bit is controlled via `ARCH`:

```bash
make          # 32-bit (i686-forestos)
make ARCH=64  # 64-bit (x86_64-forestos)
```

This changes the compiler prefix, flags, and linker script automatically.

### Compiler Flags

All apps are compiled with:

| Flag | Purpose |
|------|---------|
| `-ffreestanding` | No standard library assumptions |
| `-nostdlib` | No default libs linked |
| `-fno-builtin` | Don't use compiler builtins for libc |
| `-fno-stack-protector` | No stack canaries (no runtime support) |
| `-fno-pie -fno-pic` | Position-dependent code |
| `-mno-sse -mno-sse2 -mno-mmx` | No x87/SSE (avoid FPU state issues) |
| `-O0 -g` | Debug builds, no optimization |
| `-Wall -Wextra` | All warnings enabled |

### Build Flow

```bash
cd userspace
make              # builds all 45 apps + initrd-builder
make verify       # verifies all binaries are Forest ELF
make install      # installs to build/initrd/ staging area
make initrd       # copies binaries to fern/initrd/bin/
```

The build proceeds as follows:

1. Compile `crt0.S` into `build/crt0.o`
2. Compile `forest_stubs.c` into `build/forest_stubs.o`
3. For each app: compile `app.c` into `build/app/app.o`, link with crt0 + stubs + libc
4. Strip all binaries
5. Build `initrd-builder` (host tool, uses host gcc)
6. Optionally build initrd image

---

## The C Runtime Startup

The entry point for every Forest OS userspace process is `_start`, defined in
`crt0.S`. This is a tiny 17-line assembly file:

```asm
    .text
    .globl _start

_start:
    /* Stack has argc, argv, envp placed by kernel */
    /* argc at [esp], argv at [esp+4], envp at [esp+4+argc*4] */
    call main
    /* main returned -- exit with its return value */
    pushl %eax
    call exit
    hlt
```

**What happens at boot:**

1. The kernel sets up the user stack with `argc`, `argv`, and `envp` (standard ELF aux vector layout)
2. Execution jumps to `_start`
3. `_start` calls `main(argc, argv, envp)` -- the C entry point
4. When `main` returns, `_start` pushes the return value and calls `exit()`
5. The `hlt` instruction is never reached (exit terminates the process)

This is the same pattern used by Linux and other Unix-like systems. The crt0
object is compiled once and linked into every userspace binary.

---

## Linker Scripts

Two linker scripts define the memory layout of userspace ELF binaries:

### 32-bit (`link.ld`)

```ld
ENTRY(_start)
SECTIONS {
    . = 0x08048000;    /* Classic Linux ELF base address */
    .text : { *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data : { *(.data) *(.data.*) }
    .bss : { *(.bss) *(.bss.*) *(COMMON) }
    _end = .;
}
```

### 64-bit (`link64.ld`)

```ld
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS {
    . = 0x00400000 + SIZEOF_HEADERS;
    .text : { *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data : { *(.data) *(.data.*) }
    .bss : { *(.bss) *(.bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.*) }
}
```

The 64-bit script uses `SIZEOF_HEADERS` to align the load address after ELF
headers. It also discards metadata sections (comment, note, eh_frame, gnu)
to produce smaller binaries.

---

## The Stubs Layer

Forest OS libc is still growing. To bridge the gap, `include/forest_stubs.c`
provides ~1850 lines of stub and minimal implementations for functions that
apps need but libc doesn't yet provide.

### What the stubs cover:

| Category | Functions |
|----------|-----------|
| **String** | `basename`, `dirname`, `strdup`, `strerror` |
| **Pattern matching** | `fnmatch` (full glob-style implementation) |
| **Networking** | `getaddrinfo`, `getnameinfo`, `freeaddrinfo` (loopback stubs) |
| **Option parsing** | `getopt`, `getopt_long` |
| **stdio** | `snprintf`, `fprintf`, `sscanf`, `fscanf`, `vfscanf`, `vsscanf`, `asprintf`, `vasprintf`, `getline`, `getdelim`, `fgetc`, `fgets`, `fputc`, `fputs`, `fread`, `fwrite`, `fseek`, `ftell`, `fclose`, `fdopen`, `fflush`, `setvbuf` |
| **qsort** | Insertion sort implementation |
| **Time** | `strftime`, `strptime`, `mktime`, `gmtime`, `localtime`, `asctime`, `ctime`, `difftime`, `clock` |
| **User/Group** | `getpwnam`, `getpwuid`, `getgrnam`, `getgrgid` (hardcoded root) |
| **Math** | Full trig, log, exp, pow, sqrt via `__builtin_*` compiler intrinsics |
| **Wide char** | `mbrtowc`, `wcwidth` |
| **Misc** | `mkstemp`, `time`, `readlink`, `lstat`, `isatty`, `flockfile`, `tmpfile` |

The stubs are compiled once into `forest_stubs.o` and linked into every binary.
This means all apps share the same stub implementations, keeping things
consistent.

---

## All 45 Applications

### Complete Application Table

| # | Application | Category | Lines | Description |
|---|------------|----------|-------|-------------|
| 1 | `basename` | Core Utils | 153 | Strip directory from filename |
| 2 | `blkid` | Disk Tools | 404 | Identify block device filesystem/UUID |
| 3 | `cat` | Core Utils | 212 | Concatenate and print files |
| 4 | `chmod` | Core Utils | 356 | Change file permissions |
| 5 | `chown` | Core Utils | 271 | Change file ownership |
| 6 | `cp` | Core Utils | 494 | Copy files and directories |
| 7 | `date` | System Tools | 426 | Display/set system date and time |
| 8 | `dd` | Disk Tools | 403 | Low-level copy and convert |
| 9 | `df` | Disk Tools | 461 | Report disk space usage |
| 10 | `dirname` | Core Utils | 139 | Strip last component from path |
| 11 | `du` | Disk Tools | 294 | Estimate file space usage |
| 12 | `echo` | Core Utils | ~80 | Print arguments to stdout |
| 13 | `fdisk` | Disk Tools | 262 | Partition table manipulation |
| 14 | `find` | Text Processing | 712 | Recursive file search with predicates |
| 15 | `forest-shell` | Shell | 2507 | Interactive POSIX shell |
| 16 | `forest-x11` | GUI | 1200+ | X11 client library and display server |
| 17 | `fsck` | Disk Tools | 662 | Filesystem check and repair |
| 18 | `grep` | Text Processing | 937 | Pattern matching with regex |
| 19 | `head` | Text Processing | 142 | Output first lines of file |
| 20 | `hostname` | System Tools | 262 | Display/set system hostname |
| 21 | `id` | Auth | 214 | Print user and group IDs |
| 22 | `init` | System Tools | 258 | PID 1 init process |
| 23 | `kill` | System Tools | 223 | Send signals to processes |
| 24 | `ln` | Core Utils | 194 | Create hard/symbolic links |
| 25 | `losetup` | Disk Tools | 262 | Set up loop devices |
| 26 | `ls` | Core Utils | 446 | List directory contents |
| 27 | `mkdir` | Core Utils | 172 | Create directories |
| 28 | `mkfs` | Disk Tools | 757 | Create filesystem on device |
| 29 | `mount` | Disk Tools | 494 | Mount filesystems |
| 30 | `mv` | Core Utils | 555 | Move/rename files |
| 31 | `ps` | System Tools | 553 | Report running processes |
| 32 | `pwd` | Core Utils | ~100 | Print working directory |
| 33 | `reboot` | System Tools | 123 | Reboot the system |
| 34 | `rm` | Core Utils | 326 | Remove files and directories |
| 35 | `rmdir` | Core Utils | 150 | Remove empty directories |
| 36 | `shutdown` | System Tools | 509 | Halt/poweroff the system |
| 37 | `sleep` | System Tools | 154 | Delay for a time period |
| 38 | `sort` | Text Processing | 471 | Sort lines of text |
| 39 | `su` | Auth | 662 | Substitute user identity |
| 40 | `sudo` | Auth | 721 | Execute as another user |
| 41 | `tail` | Text Processing | 340 | Output last lines of file |
| 42 | `touch` | Core Utils | 472 | Create/update file timestamps |
| 43 | `umount` | Disk Tools | 306 | Unmount filesystems |
| 44 | `uname` | System Tools | ~180 | Print system information |
| 45 | `wc` | Text Processing | 193 | Count lines, words, bytes |

---

## Application Categories

### Core Utilities (15 apps)

These are the everyday file and path manipulation tools:

`basename`, `cat`, `chmod`, `chown`, `cp`, `dirname`, `echo`, `ln`, `ls`,
`mkdir`, `mv`, `pwd`, `rm`, `rmdir`, `touch`

Most are straightforward implementations of POSIX semantics. `ls` is notably
feature-rich with color output, long listing, inode display, and sorting.

### Text Processing (6 apps)

Tools for reading, searching, and transforming text:

`find`, `grep`, `head`, `sort`, `tail`, `wc`

`grep` is the most complex at 937 lines, supporting regex, fixed-string matching,
case insensitivity, recursion, and line numbering. `find` at 712 lines supports
comprehensive predicate evaluation (name, type, size, time, permissions).

### Disk & Filesystem Tools (9 apps)

Low-level storage management:

`blkid`, `dd`, `df`, `du`, `fdisk`, `fsck`, `losetup`, `mkfs`, `mount`, `umount`

`mkfs` (757 lines) creates ext2-like filesystems with superblock, block groups,
and inode tables. `fsck` (662 lines) validates and repairs filesystem structures.

### System Tools (9 apps)

Process and system management:

`date`, `hostname`, `init`, `kill`, `ps`, `reboot`, `shutdown`, `sleep`, `uname`

`init` is PID 1 -- it mounts essential filesystems, spawns child processes, and
reaps zombies. `shutdown` supports halt, poweroff, and reboot with optional delay.

### Authentication (3 apps)

User identity and privilege:

`id`, `su`, `sudo`

`sudo` (721 lines) implements sudoers file parsing, timestamp validation,
and command execution with privilege escalation. `su` handles PAM-style
authentication (with stub implementations for now).

### Shell (1 app)

`forest-shell` -- the flagship interactive shell at 2507 lines. Features include:

- Command line editing with raw terminal input
- History (100 entries, searchable with up/down arrows)
- Aliases (128 slots)
- Job control (64 background jobs, fg/bg/wait)
- Globbing and variable expansion
- Command substitution
- Piping and redirection (>, >>, <, 2>, 2>&1)
- Signal handling (SIGINT, SIGQUIT, SIGTSTP, SIGCHLD)
- Builtins: `cd`, `exit`, `export`, `alias`, `history`, `jobs`, `fg`, `bg`, `wait`

### GUI (1 app)

`forest-x11` -- an X11 client library and display server implementation:

- X11 protocol over Unix sockets
- Window creation and management
- Drawing primitives (lines, rectangles, text)
- Color and font handling
- Event loop
- Pixmap support
- GC (graphics context) management

Built from 15 source files totaling ~2000+ lines.

---

## Common Patterns Across Apps

Every application in Forest OS follows consistent conventions:

### 1. Single-file structure

Each app is one `.c` file. No multi-file apps (except forest-x11 which is
a library/server, not a simple utility).

### 2. Forest.h header

All apps include `forest.h` which provides:

```c
#include "forest.h"  // Common defs, exit codes, helpers
```

This gives access to standard POSIX headers, utility macros (`ARRAY_SIZE`,
`MIN`, `MAX`, `ALIGN`), exit code constants, and helper functions (`eprint`,
`mode_string`, `proc_state`, `sig_name`).

### 3. Standard main() signature

```c
int main(int argc, char *argv[]) {
    // Parse options with getopt
    // Process files/arguments
    // Return exit code
}
```

### 4. Option parsing with getopt

Apps use the stub `getopt()` for consistent option parsing:

```c
int opt;
while ((opt = getopt(argc, argv, "nbsAvTEu")) != -1) {
    switch (opt) {
    case 'n': opt_number = 1; break;
    case 'b': opt_nonblank = 1; break;
    // ...
    default: usage();
    }
}
```

### 5. Error output pattern

Apps use `fprintf(stderr, ...)` for errors with the program name:

```c
fprintf(stderr, "%s: cannot open '%s': %s\n",
        progname, filename, strerror(errno));
```

### 6. Exit codes

Defined in `forest.h`:

| Code | Constant | Meaning |
|------|----------|---------|
| 0 | `EXIT_OK` | Success |
| 1 | `EXIT_USAGE` | Bad usage/arguments |
| 2 | `EXIT_FAIL` | Operation failed |
| 128 | `EXIT_ERROR` | Internal error |
| 128+N | `EXIT_SIGBASE` | Killed by signal N |

### 7. File I/O

Apps use POSIX `open()`/`read()`/`write()`/`close()` directly. Buffered I/O
via `fopen()`/`fread()`/`fwrite()` is also available through the stubs layer.

### 8. argv[0] stripping

Most apps strip leading `./` from their program name for cleaner error messages:

```c
progname = argv[0];
if (strncmp(progname, "./", 2) == 0)
    progname += 2;
```

---

## App Size and Complexity

The apps range from tiny utilities to complex system programs:

### Tiny (< 200 lines)

`basename` (153), `dirname` (139), `echo` (~80), `head` (142), `mkdir` (172),
`rmdir` (150), `sleep` (154), `reboot` (123), `wc` (193)

These are often under 200 lines and do one thing well.

### Medium (200-500 lines)

`cat` (212), `chmod` (356), `chown` (271), `cp` (494), `date` (426),
`dd` (403), `df` (461), `du` (294), `fdisk` (262), `hostname` (262),
`id` (214), `init` (258), `kill` (223), `ln` (194), `losetup` (262),
`ls` (446), `mount` (494), `ps` (553), `rm` (326), `shutdown` (509),
`sort` (471), `tail` (340), `touch` (472), `umount` (306), `uname` (~180),
`blkid` (404)

The bulk of the codebase lives here. Most are 300-500 lines.

### Large (500-1000 lines)

`mv` (555), `find` (712), `fsck` (662), `grep` (937), `mkfs` (757),
`su` (662), `sudo` (721)

These implement significant protocol parsing, filesystem logic, or
complex option processing.

### Very Large (1000+ lines)

`forest-shell` (2507), `forest-x11` (1200+ across 15 files)

The shell is by far the largest single file. It implements job control,
command parsing, variable expansion, and builtins all in one file. The
X11 implementation is spread across multiple files but still one build target.

### Total codebase

~23,000 lines of C across all userspace apps (including the 1850-line stubs
file). Without stubs, the apps themselves total ~21,000 lines.

---

## How Apps Interact with the Kernel

Forest OS apps talk to the kernel through the standard C library (libc) syscall
interface. The interaction is straightforward:

### Direct syscalls via libc

```c
// File operations
int fd = open("/etc/passwd", O_RDONLY);
ssize_t n = read(fd, buf, sizeof(buf));
write(STDOUT_FILENO, buf, n);
close(fd);

// Process management
pid_t pid = fork();
if (pid == 0) {
    execvp("/bin/ls", args);
}
waitpid(pid, &status, 0);

// Signals
kill(pid, SIGTERM);

// Filesystem
mount("proc", "/proc", "proc", 0, NULL);
stat("/tmp", &st);
```

### Available syscalls

The kernel exposes these syscall categories to userspace:

| Category | Syscalls |
|----------|----------|
| **File I/O** | `open`, `close`, `read`, `write`, `lseek`, `stat`, `fstat`, `lstat`, `access`, `unlink`, `rename`, `mkdir`, `rmdir`, `link`, `symlink`, `readlink` |
| **Process** | `fork`, `exec`, `wait`, `waitpid`, `exit`, `getpid`, `getppid`, `getuid`, `getgid`, `setuid`, `setgid` |
| **Signals** | `kill`, `signal`, `sigaction`, `sigprocmask` |
| **Filesystem** | `mount`, `umount`, `ioctl` |
| **Memory** | `mmap`, `munmap`, `brk` |
| **Time** | `clock_gettime`, `nanosleep`, `gettimeofday` |
| **System** | `uname`, `reboot`, `sethostname` |

### The stubs bridge

When libc doesn't provide a function yet, `forest_stubs.c` fills in the gap.
For example, apps use `getopt()` for option parsing -- this is implemented in
the stubs layer, not in libc. The stubs layer calls into libc for the actual
syscalls (like `read()` for reading from stdin).

---

## Placing Apps in the initrd

The initrd (initial ramdisk) is how Forest OS boots with all its userspace
tools. The process has two stages:

### 1. Building the binaries

```bash
cd userspace
make all
```

This produces ELF binaries in `build/bin/`.

### 2. Copying to the initrd

The `make initrd` target copies all binaries to the kernel's initrd staging area:

```bash
make initrd
# Copies build/bin/* to ../fern/initrd/bin/
```

### 3. The initrd builder

For creating proper initrd images, the `initrd-builder` host tool (built from
`initrd-builder/initrd_builder.c`) creates CPIO-format images:

```bash
./build/initrd-builder -o initrd.img -d build/initrd/ -z -v
```

This produces a gzipped CPIO archive that the kernel can mount as the root
filesystem at boot.

### initrd directory structure

```
initrd/
  bin/          # All userspace binaries
  sbin/         # System admin binaries (init, shutdown, etc.)
  etc/          # Configuration files
  dev/          # Device nodes
  proc/         # Mount point for /proc
  sys/          # Mount point for /sys
  tmp/          # Temporary files
  usr/bin/      # Additional binaries
  usr/sbin/     # Additional system binaries
```

---

## Adding a New Application

Adding a new app to Forest OS is straightforward. Here's the recipe:

### Step 1: Create the directory

```bash
mkdir userspace/myapp
```

### Step 2: Write the source

Create `userspace/myapp/myapp.c`:

```c
/*
 * myapp.c - Forest OS userspace myapp implementation
 * Brief description of what it does.
 */

#include "forest.h"

static const char *progname = "myapp";

static void usage(void) {
    fprintf(stderr, "Usage: %s [OPTIONS] FILE...\n", progname);
    exit(EXIT_USAGE);
}

int main(int argc, char *argv[]) {
    int opt;

    progname = argv[0];
    if (strncmp(progname, "./", 2) == 0)
        progname += 2;

    while ((opt = getopt(argc, argv, "hv")) != -1) {
        switch (opt) {
        case 'h': usage(); break;
        case 'v': printf("myapp 1.0\n"); return EXIT_OK;
        default:  usage();
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "%s: missing file argument\n", progname);
        usage();
    }

    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "%s: cannot open '%s': %s\n",
                    progname, argv[i], strerror(errno));
            continue;
        }
        // ... do work ...
        close(fd);
    }

    return EXIT_OK;
}
```

### Step 3: Create the Makefile

Create `userspace/myapp/Makefile`:

```makefile
# Forest OS - myapp
# Built via userspace/Makefile

CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= ld
STRIP   ?= strip
CFLAGS  ?= -m32 -march=i386 -ffreestanding -nostdlib -fno-builtin \
           -fno-stack-protector -fno-pie -fno-pic -Wall -Wextra -g -O0 \
           -mno-sse -mno-sse2 -mno-mmx
LDFLAGS ?= -m elf_i386 -nostdlib

BUILDDIR ?= build
BINDIR   ?= $(BUILDDIR)/bin

TARGET = myapp
SRC    = myapp.c
OBJ    = $(BUILDDIR)/$(TARGET).o
BIN    = $(BUILDDIR)/$(TARGET)

.PHONY: all clean

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OBJ): $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(EXTRA_OBJS) $(LIBS)
	$(STRIP) $@

clean:
	rm -rf $(BUILDDIR)
```

### Step 4: Register in the master Makefile

Add `myapp` to the `APPS` variable in `userspace/Makefile`:

```makefile
APPS := \
    cat echo ls cp mv mkdir rmdir touch chmod chown ln pwd basename dirname \
    grep find sort wc head tail dd df du \
    fdisk mkfs mount umount blkid losetup fsck \
    ps kill init shutdown reboot hostname uname date sleep id \
    su sudo \
    forest-shell forest-x11 \
    myapp
```

### Step 5: Build and test

```bash
cd userspace
make myapp          # build just myapp
make verify         # verify it's a valid ELF
make                # build everything
```

### Tips for new apps

- Always include `forest.h` -- it provides exit codes, helper functions, and standard headers
- Use `getopt()` for option parsing -- it's provided by the stubs layer
- Print errors to stderr with the program name prefix
- Use `EXIT_OK`, `EXIT_FAIL`, `EXIT_USAGE` for return codes
- Keep it simple: one `.c` file, one function per concern
- Check return values from syscalls and report errors

---

## Summary

Forest OS userspace is a clean, Unix-like environment built around 45 cross-compiled
applications. The architecture is simple and consistent:

- **crt0.S** provides the `_start` entry point
- **Linker scripts** define the memory layout
- **forest_stubs.c** fills gaps in libc
- **forest.h** provides common definitions
- **Master Makefile** orchestrates the build
- Each app is a single C file with a small Makefile

The result is a bootable system with a shell, filesystem tools, process management,
authentication, and even an X11 graphical frontend -- all in about 23,000 lines
of C.

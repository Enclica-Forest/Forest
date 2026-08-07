# Userspace Build System

Forest OS builds all userspace applications from a single Makefile that
cross-compiles ~50 utilities against the Forest OS libc and kernel headers.
This page explains how it works and how to extend it.

## Prerequisites

1. **Forest OS cross-toolchain** — built via `forestos-toolchain/build-toolchain.sh`
2. **GNU Make** — the build is entirely Makefile-driven
3. **Host GCC** — needed only for the `initrd-builder` host tool
4. **gzip** — optional, for compressed initrd images

The Makefile refuses to build if the cross-compiler is not found:

```
Forest-OS cross-toolchain not found at .../install/bin/i686-forestos-gcc.
Build it first: cd forestos-toolchain && ./build-toolchain.sh --arch 32
```

## Build Commands

```sh
cd forest/userspace
make                  # Build everything (default: 32-bit)
make ARCH=64          # Build for 64-bit
make cat              # Build a single application
make clean            # Remove all build artifacts
make verify           # Check all binaries are valid ELF
make initrd           # Build and copy to kernel initrd staging area
```

## Makefile Architecture

The build uses a two-level Makefile structure:

```
userspace/
├── Makefile            # Master Makefile — orchestrates everything
├── crt0.S             # C runtime startup assembly
├── link.ld            # 32-bit linker script
├── link64.ld          # 64-bit linker script
├── include/           # Shared headers and stubs
├── initrd-builder/    # Host tool for building initrd images
├── cat/               # Each app has its own directory + Makefile
│   ├── cat.c
│   └── Makefile
├── echo/
├── ls/
└── ...                # ~50 applications total
```

The master Makefile builds `crt0.o` and `forest_stubs.o` first, then delegates
to each app's sub-Makefile (`Makefile:98`):

```makefile
$(APPS): $(CRT0) $(STUBS_OBJ)
    $(MAKE) -C $(CURDIR)/$@ CC="$(CC)" LD="$(LD)" STRIP="$(STRIP)" \
        CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" CRT0="$(CRT0)" \
        EXTRA_OBJS="$(EXTRA_OBJS)" LIBS="$(LIBS)" \
        BUILDDIR="$(CURDIR)/$(BUILDDIR)/$@" BINDIR="$(BINDIR)"
```

Each sub-Makefile links with `crt0.o`, `forest_stubs.o`, libc, and libgcc:

```makefile
$(BIN): $(OBJ)
    $(LD) $(LDFLAGS) -o $@ $^ $(EXTRA_OBJS) $(LIBS)
    $(STRIP) $@
```

## C Runtime Startup (`crt0.S`)

Every userspace binary starts at `_start` (`crt0.S:7`). The kernel pushes
`argc`, `argv`, and `envp` on the stack before jumping here:

```asm
_start:
    /* argc at [esp], argv at [esp+4], envp at [esp+4+argc*4] */
    call main
    pushl %eax
    call exit
    hlt
```

This is intentionally minimal — no global constructor support, no `.init`/`.fini`
array processing, no dynamic linker. Apps are statically linked single binaries.

## Linker Scripts

### 32-bit (`link.ld`)

Loads at `0x08048000` (classic Linux ELF base). Section layout:

```
. = 0x08048000;
.text   → code
.rodata → read-only data (string literals, constants)
.data   → initialized globals
.bss    → zero-initialized globals + COMMON symbols
```

### 64-bit (`link64.ld`)

Loads at `0x00400000 + SIZEOF_HEADERS`, sets output format to `elf64-x86-64`,
and discards debug metadata via `/DISCARD/`.

Both scripts define `ENTRY(_start)`.

## Compiler Flags

The master Makefile sets these for all cross-compiled code (`Makefile:38`):

| Flag | Purpose |
|------|---------|
| `-ffreestanding` | No hosted C library assumptions |
| `-nostdlib` | Do not link standard startup files |
| `-fno-builtin` | No compiler builtins for libc calls |
| `-fno-stack-protector` | No stack canaries (no runtime support) |
| `-fno-pie -fno-pic` | Position-dependent code |
| `-mno-sse -mno-sse2 -mno-mmx` | No x87/SSE instructions |
| `-g -O0` | Debug symbols, no optimization |

Architecture flags: 32-bit uses `-m32 -march=i386` / `-m elf_i386`;
64-bit uses `-m64 -march=x86-64` / `-m elf_x86_64`.

Include paths pull in Forest OS headers from `userspace/include/`,
`libs/libc/include/libc/`, `forestos-toolchain/sysroot/usr/include/`,
and `libs/forestcore/include/`.

## The Stubs Layer

Forest OS libc is incomplete. The stubs file (`include/forest_stubs.c`) fills
the gaps with ~1500 lines of implementations for:

- **String/IO**: `basename`, `dirname`, `strdup`, `strerror`, `snprintf`,
  `fprintf`, `sscanf`, `fscanf`, `getline`, `getdelim`
- **Option parsing**: `getopt`, `getopt_long`
- **Networking**: `getaddrinfo`, `getnameinfo`, `freeaddrinfo` (loopback only)
- **Time**: `mktime`, `gmtime`, `localtime`, `strftime`, `strptime`, `clock`
- **User/Group**: `getpwnam`, `getpwuid`, `getgrnam`, `getgrgid` (hardcoded root)
- **Math**: all trig, exp/log, pow/sqrt via GCC builtins
- **FILE ops**: `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`,
  `fgetc`, `fputc`, `fgets`, `fputs`, `fdopen`, `fileno`
- **Sort**: `qsort` (insertion sort)
- **Misc**: `fnmatch`, `mbrtowc`, `wcwidth`, `isatty`, `popen`, `pclose`

The stubs object is linked into every binary automatically via `EXTRA_OBJS`.

## How Apps Are Packaged Into the Initrd

The `initrd-builder` host tool (`initrd-builder/initrd_builder.c`) creates
cpio newc format images. The master Makefile copies binaries into the kernel's
initrd staging area with `make initrd`:

```makefile
initrd: all
    @mkdir -p ../fern/initrd/bin
    @cp $(BINDIR)/* ../fern/initrd/bin/
```

The `make install` target creates a more complete staging area with
standard directories (`bin/`, `sbin/`, `etc/`, `dev/`, `proc/`, etc.).

The initrd builder is a host-compiled tool:

```makefile
HOST_CC := gcc
$(INITRD_BUILDER): $(INITRD_BUILDER_SRC)
    $(HOST_CC) $(HOST_CFLAGS) -o $@ $<
```

## Adding a New Application

### 1. Create the directory and source file

```sh
mkdir forest/userspace/myapp
```

Write `myapp/myapp.c`:

```c
#include "forest.h"
#include <stdio.h>

int main(int argc, char *argv[]) {
    printf("Hello from myapp!\n");
    return 0;
}
```

### 2. Create the sub-Makefile

`myapp/Makefile`:

```makefile
# Forest OS - myapp — built via userspace/Makefile
CC      ?= $(error Cross-compiler not found - build via userspace/Makefile)
LD      ?= $(CC)
STRIP   ?= strip
CFLAGS  ?=
LDFLAGS ?=
BUILDDIR?= build
BINDIR  ?= /usr/local/bin

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

### 3. Register in the master Makefile

Add `myapp` to the `APPS` list in `userspace/Makefile`:

```makefile
APPS := cat echo ls ... myapp
```

### 4. Build it

```sh
cd forest/userspace && make myapp
```

## Build Optimization

**Parallel builds**: `make -j$(nproc)` — Make tracks dependencies automatically;
after editing `cat/cat.c`, only `cat` is recompiled. Use `make -B` for full rebuilds.

**Stripping**: All binaries are stripped automatically (`$(STRIP) $@`).

**Compression**: For smaller initrd images:

```sh
./build/initrd-builder -o initrd.cpio.gz -d initrd/ -z -v
```

## Common Build Errors

| Error | Cause | Fix |
|-------|-------|-----|
| `cross-toolchain not found` | No cross-compiler | `cd forestos-toolchain && ./build-toolchain.sh --arch 32` |
| `undefined reference to _start` | Missing `crt0.o` | Link with `$(EXTRA_OBJS)` in sub-Makefile |
| `undefined reference to __errno_location` | Missing stubs | Ensure `EXTRA_OBJS` is passed from master |
| `multiple definition of basename` | App defines its own `basename` | Remove local definition, use `libgen.h` |
| `cannot find -lc` | Missing sysroot | `./build-toolchain.sh --arch 32 --sysroot` |
| App crashes at runtime | SSE/MMX used, wrong entry point, or `main` returns wrong type | Check CFLAGS and linker script |

## Build Directory Layout

After a full build, `build/` contains:

```
build/
├── crt0.o                    # C runtime startup object
├── forest_stubs.o            # Stubs object
├── initrd-builder            # Host tool
├── cat/                      # Per-app build directories
│   └── cat
├── echo/
│   └── echo
└── ...
```

The `build/bin/` directory collects all final binaries for easy access.

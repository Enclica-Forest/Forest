# ForestOS Cross-Toolchain

The ForestOS cross-toolchain is a self-contained source package that builds
the GCC + Binutils cross-compilers targeting the ForestOS operating system.
The output is a set of freestanding ELF cross-compilers used to build the
Fern kernel and, eventually, ForestOS userspace.

**Vocabulary**: the OS is **ForestOS**, the kernel is **Fern**, the
bootloader is **foreboots**.

---

## Why a Custom Cross-Compiler is Needed

The Fern kernel targets bare-metal x86 in freestanding mode. It does not
link against a hosted libc in the traditional sense — it uses its own linker
script, its own startup code, and its own syscall interface. A stock
`x86_64-linux-gnu-gcc` would pull in Linux-specific assumptions (dynamic
linker paths, glibc startfiles, SSP flags) that don't apply.

A custom cross-compiler gives ForestOS:

- **Clean triple isolation** — `i686-forestos-gcc` and
  `x86_64-forestos-gcc` know they're targeting ForestOS, not Linux or
  generic ELF.
- **Freestanding defaults** — the compiler is configured with
  `--without-headers --with-newlib` so it doesn't assume a hosted libc
  exists.
- **Correct predefines** — target code can detect ForestOS via
  `__forestos__`, `__ForestOS__`, and `system=forestos`.
- **Sysroot control** — the baked-in sysroot path points to a minimal
  header island that provides exactly what the kernel needs: `<stdio.h>`
  and `<forestos/syscalls.h>`.

---

## GCC and Binutils Versions

The toolchain pins two versions (see `checksums.txt`):

| Component | Version | Tarball |
|-----------|---------|---------|
| Binutils  | 2.43    | `binutils-2.43.tar.xz` |
| GCC       | 13.2.0  | `gcc-13.2.0.tar.xz` |

Both are downloaded from the official GNU mirrors and verified against
SHA-256 checksums before extraction. The checksums file is compatible with
`sha256sum -c`.

---

## Target Triples

| Architecture | Target Triple | Tool Prefix | GCC Arch Flag |
|-------------|---------------|-------------|---------------|
| 32-bit x86 | `i686-forestos` | `i686-forestos-` | `--with-arch=i686` |
| 64-bit x86 | `x86_64-forestos` | `x86_64-forestos-` | `--with-arch=x86-64` |

Both targets use ELF output format and are configured as freestanding
(no libc assumed present).

---

## Building the Toolchain from Source

### Host Dependencies

You need a Linux host with build tools and GCC prerequisite libraries:

**Debian/Ubuntu:**
```sh
sudo apt install -y build-essential flex bison gawk texinfo curl xz-utils \
    libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev
```

**Fedora/RHEL:**
```sh
sudo dnf install -y gcc gcc-c++ make flex bison gawk texinfo curl xz \
    gmp-devel mpfr-devel libmpc-devel isl-devel zlib-devel
```

**Arch:**
```sh
sudo pacman -S --needed base-devel flex bison gawk texinfo curl xz gmp mpfr libmpc
```

The required tools are: `gcc g++ make makeinfo flex bison gawk tar xz`
plus either `curl` or `wget`.

### Build Commands

Set your repo root:
```sh
export FOREST="$(git rev-parse --show-toplevel)"
```

Build both architectures (default):
```sh
cd $FOREST/forestos-toolchain
./build-toolchain.sh
```

Build a single architecture with parallelism:
```sh
./build-toolchain.sh --arch 32           # i686-forestos only
./build-toolchain.sh --arch 64 -j8       # x86_64-forestos only, 8 jobs
```

Other useful options:
```sh
./build-toolchain.sh --help              # full option list
./build-toolchain.sh --skip-deps         # skip dependency probe
./build-toolchain.sh --clean             # force full rebuild
./build-toolchain.sh --no-download       # use tarballs already in src/
```

### What the Build Does

The script (`build-toolchain.sh`) is idempotent and resumable:

1. **Checks host dependencies** (tools + GMP/MPFR/MPC headers).
2. **Downloads** binutils and GCC tarballs into `src/`, verifying SHA-256.
3. **Patches** both source trees so the real `*-forestos` triples are
   recognised (not the old `i686-elf` + symlink hack).
4. **Populates the sysroot** from `sysroot-skeleton/`, Fern kernel headers,
   and consolidated libc headers.
5. **Builds binutils** for each target (configure + make + make install).
6. **Builds GCC** for each target (`all-gcc` + `all-target-libgcc`).
7. **Verifies** each toolchain with a freestanding compile probe.
8. **Fixes include-fixed** — copies missing headers into GCC's
   `include-fixed/` directory so the `<stdio.h>` include chain resolves.

A full build takes tens of minutes and needs several GB of scratch space.

### Build Artifacts

Everything generated is `.gitignore`d. Only the source is committed:

```
forestos-toolchain/
  build-toolchain.sh       # the canonical build script
  patches/                 # triple-support patches + forestos.h
  checksums.txt            # pinned SHA-256 sums
  sysroot-skeleton/        # tracked header island
  README.md

  # all generated, gitignored:
  src/                     # downloaded + extracted sources
  build/                   # out-of-tree build directories
  install/                 # finished toolchain (install/bin/<prefix>-gcc, ...)
  sysroot/                 # assembled sysroot (headers + libc objects)
```

---

## The Sysroot Skeleton

The sysroot skeleton (`sysroot-skeleton/`) is a small, tracked header tree
that gets installed into `sysroot/usr/include/`. It exists because the
Fern kernel compiles freestanding and passes no `--sysroot` or `-isystem`
flags — the sysroot is reached **only** because the cross-GCC is configured
with `--with-sysroot=.../sysroot`.

### Skeleton Contents

```
sysroot-skeleton/
  usr/include/stdio.h               # libc island — kernel pulls <stdio.h>
  usr/include/forestos/syscalls.h   # REQUIRED; the only source of this header
  usr/include/sys/types.h           # thin wrapper -> ../libc/sys/types.h
  usr/include/libc/sys/types.h      # real POSIX type definitions
  lib/README.md                     # marks lib/ as a generated output dir
```

### Why It Exists

The kernel pulls `<stdio.h>`, which transitively requires
`<forestos/syscalls.h>` and `<sys/types.h>`. The `forestos/syscalls.h`
header has **no clean source anywhere else in the repo**, so it must be
carried here. The `sys/types.h` wrapper delegates to the consolidated libc,
which only depends on GCC freestanding builtins (`<stddef.h>`,
`<stdint.h>`).

The skeleton is installed verbatim into `sysroot/usr/include/`. During
build, `build-toolchain.sh` overlays three sources into the sysroot:

1. **`sysroot-skeleton/`** (tracked) — the critical header island.
2. **Fern kernel headers** from `fern/src/include/` — the wider set of
   userspace/wrapper headers.
3. **Consolidated libc headers** from `libs/libc/` — the single source of
   truth for all libc headers.

CRT and libc objects (`crt0.o`, `crti.o`, `crtn.o`, `libc.a`) are build
outputs, not part of the skeleton. They're arch-specific and regenerated
on every build.

---

## Kernel Headers: `forestos/syscalls.h`

The file `sysroot-skeleton/usr/include/forestos/syscalls.h` is the most
critical header in the sysroot. It defines:

- **Syscall numbers** (`SYS_READ`, `SYS_WRITE`, `SYS_OPEN`, etc.)
  that must match `src/include/syscall.h` in the Fern kernel.
- **Error codes** (`EPERM`, `ENOENT`, `ENOMEM`, etc.) that must match
  `src/include/libc/errno.h`.
- **Inline syscall wrappers** (`syscall0` through `syscall6`) using
  `int $0x80` — these are inline assembly stubs for the Fern kernel's
  interrupt-based syscall interface.
- **Framebuffer extension syscalls** (`SYS_MMAP_FB`, `SYS_FB_FLUSH`,
  `SYS_SOUND_PLAY`, etc.) for the graphics subsystem.

This header must be kept in sync with the kernel's own definitions. If you
add or change a syscall in the kernel, update it here too.

---

## Patches Applied to GCC/Binutils

Stock GNU binutils and GCC don't know about the `*-forestos` triple. The
toolchain applies several patches to make them work.

### Patches Directory

```
patches/
  config.sub.forestos.patch     # reference diff for config.sub
  config.gcc.forestos.patch     # reference diff for gcc/config.gcc
  gcc/config/forestos.h         # GCC OS-config header (copied into gcc source)
  README.md                     # documents the patch strategy
```

### What Gets Patched

| File | Purpose |
|------|---------|
| `config.sub` (both binutils & gcc) | Adds `forestos*` to the OS validation list so `config.sub i686-forestos` canonicalizes correctly. |
| `gas/configure.tgt` | Adds `i[3-7]86-*-forestos*` and `x86_64-*-forestos*` with `fmt=elf`. |
| `ld/configure.tgt` | Adds forestos targets with `elf_i386` / `elf_x86_64` emulations. |
| `bfd/config.bfd` | Adds forestos BFD vectors (`i386_elf32_vec` / `x86_64_elf64_vec`). |
| `gcc/config.gcc` | Adds `i[34567]86-*-forestos*` and `x86_64-*-forestos*` target cases. |
| `gcc/config/forestos.h` | Defines `__forestos__`, ELF startfile/endfile specs, dynamic linker path. |
| `libgcc/config.host` | Adds forestos tmake_file entries for CRT and libgcc-pic. |
| `libcody/configure` | Patches C++11 check to accept `>= 201103` (for GCC >= 14 host compilers). |

### The `forestos.h` GCC Config Header

This header (`patches/gcc/config/forestos.h`) is copied verbatim into the
GCC source tree. Key definitions:

```c
// Predefined macros for target code detection
builtin_define("__forestos__");
builtin_define("__ForestOS__");
builtin_assert("system=forestos");

// ELF startup/shutdown files
STARTFILE_SPEC "%{!shared:crt0.o%s} crti.o%s crtbegin.o%s"
ENDFILE_SPEC   "crtend.o%s crtn.o%s"

// Dynamic linker (for future shared userspace)
DYNAMIC_LINKER "/lib/ld-forestos.so.1"
```

### Patch Application Strategy

Patches are applied via **guarded, idempotent sed insertions** in
`build-toolchain.sh`, not via `patch -p1`. This is because anchor lines
drift between binutils/gcc versions. The `.patch` files in `patches/` are
reference diffs documenting exactly what the script produces.

---

## Toolchain Directory Layout

After a successful build:

```
forestos-toolchain/install/
  bin/
    i686-forestos-ld          # 32-bit linker
    i686-forestos-as          # 32-bit assembler
    i686-forestos-gcc         # 32-bit C/C++ compiler
    i686-forestos-g++         # 32-bit C++ compiler
    i686-forestos-objcopy     # binary manipulation
    i686-forestos-strip       # symbol stripping
    i686-forestos-nm          # symbol listing
    x86_64-forestos-*         # (same set for 64-bit)
  lib/gcc/
    i686-forestos/13.2.0/     # 32-bit GCC support files
      include-fixed/          # auto-processed headers
      libgcc.a                # compiler runtime library
    x86_64-forestos/13.2.0/   # 64-bit GCC support files

forestos-toolchain/sysroot/
  usr/include/
    stdio.h                   # libc island
    forestos/syscalls.h       # kernel syscall interface
    sys/types.h               # POSIX types wrapper
    libc/sys/types.h          # real type definitions
    (Fern kernel headers)     # from fern/src/include/
  usr/lib/                    # (placeholder)
  lib/                        # CRT objects (build outputs)
```

---

## Troubleshooting Toolchain Builds

### Missing Dependencies

**Symptom**: `ERROR Missing host build dependencies: tools: ... libs: ...`

**Fix**: Install the missing packages (see Host Dependencies above). Or use
`--skip-deps` if you know they're present on your system.

### Config.sub Doesn't Recognize ForestOS

**Symptom**: `Invalid configuration ... OS 'forestos' not recognized`

**Fix**: This means the `config.sub` patch wasn't applied. Ensure you're
running `build-toolchain.sh` (not building manually). The script patches
`config.sub` before configuring. If you need to patch manually:
```sh
cd $FOREST/forestos-toolchain/src/binutils-2.43
patch -p1 < ../patches/config.sub.forestos.patch
```

### Checksum Mismatch

**Symptom**: `Checksum MISMATCH for gcc-13.2.0.tar.xz`

**Fix**: Delete the corrupted tarball and re-run:
```sh
rm src/gcc-13.2.0.tar.xz
./build-toolchain.sh
```

### GCC Build Fails with C++ Errors

**Symptom**: Compile errors in GCC's own code during `all-gcc`.

**Fix**: The script forces `CXXFLAGS=-std=gnu++17` for the host compiler.
If your host GCC is very old (< 7), this may not be enough. Upgrade your
host compiler or use `--skip-deps` with a known-good host.

### Broken `<stdio.h>` Include Chain

**Symptom**: `fatal error: forestos/syscalls.h: No such file or directory`

**Fix**: This means the sysroot wasn't populated correctly. Re-run the
build — the `setup_sysroot` and `patch_include_fixed` functions should
copy the missing headers. If the issue persists, check that
`sysroot-skeleton/usr/include/forestos/syscalls.h` exists.

### Include-Fixed Headers Stale

**Symptom**: GCC compiles but can't find `forestos/syscalls.h` via the
sysroot.

**Fix**: GCC's `fixincludes` may have created broken copies in
`include-fixed/`. The build script patches this automatically, but if
you've modified headers after the build, re-run:
```sh
cp -f sysroot/usr/include/forestos/syscalls.h \
      install/lib/gcc/<target>/13.2.0/include-fixed/forestos/syscalls.h
```

### Kernel Can't Find Toolchain

**Symptom**: `make: *** [validate-toolchain] Architecture toolchain not found`

**Fix**: The Fern kernel expects the toolchain at a specific path. Either
set the environment variable:
```sh
export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain
```
Or create a symlink:
```sh
ln -s ../forestos-toolchain $FOREST/fern/forestos-toolchain
```

---

## Using the Toolchain

### Basic Compilation

```sh
export TOOLCHAIN=$FOREST/forestos-toolchain/install
export PATH=$TOOLCHAIN/bin:$PATH

# Compile a freestanding C file (32-bit)
i686-forestos-gcc -ffreestanding -nostdlib -c -o boot.o boot.c

# Compile a freestanding C file (64-bit)
x86_64-forestos-gcc -ffreestanding -nostdlib -c -o boot.o boot.c

# Assemble
i686-forestos-as -o boot.o boot.S

# Link (with your own linker script)
i686_64-forestos-ld -T link.ld -o kernel.elf boot.o main.o
```

### Checking Toolchain Info

```sh
# Verify the target triple
i686-forestos-gcc -dumpmachine
# Output: i686-forestos

# Check libgcc location
x86_64-forestos-gcc -print-libgcc-file-name
# Output: .../lib/gcc/x86_64-forestos/13.2.0/libgcc.a

# Version info
i686-forestos-gcc --version
x86_64-forestos-ld --version
```

### Fern Kernel Build Integration

The kernel's `fern/build/toolchain.mk` resolves tools as:
```
$(FORESTOS_TOOLCHAIN_DIR)/install/bin/<prefix>-{gcc,g++,ld,as,objcopy,strip,nm}
```

With the sysroot at `$(FORESTOS_TOOLCHAIN_DIR)/sysroot`.

Build the kernel:
```sh
export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain
make -C $FOREST/fern all
```

Or build for a specific architecture:
```sh
make -C $FOREST/fern all ARCH=32   # 32-bit
make -C $FOREST/fern all ARCH=64   # 64-bit
```

---

## Self-Hosting Considerations

The same `build-toolchain.sh` is host-agnostic. When build == host ==
`x86_64-forestos`, it:

1. **Skips the Debian-centric dependency probe** — the script detects a
   ForestOS host via `uname -s` or by checking if `cc -dumpmachine`
   reports a `forestos` triple.
2. **Uses the Forest-hosted GCC/binutils** to rebuild the toolchain natively.
3. **Requires the sysroot-skeleton headers** to already be installed so
   that `<stdio.h>` → `<forestos/syscalls.h>` resolves during the build.

Self-hosting is the long-term goal. The initial toolchain must be built on
a Linux host (or other supported system), but once ForestOS can run GCC,
subsequent toolchain rebuilds can happen natively.

The key prerequisite for self-hosting is that the native ForestOS GCC must
be able to compile freestanding code — which is exactly what the
cross-toolchain produces.

---

## Further Reading

- `$FOREST/fern/build/toolchain.mk` — how the kernel consumes the toolchain
- `$FOREST/fern/build/flags.mk` — kernel compiler flags (freestanding mode)
- `$FOREST/MAKE_AN_OS.md` — full build walkthrough (toolchain → kernel →
  foreboots → bootable image)
- `$FOREST/forestos-toolchain/README.md` — the original toolchain README

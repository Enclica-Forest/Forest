# Forest-OS Cross-Toolchain

This directory is a **self-contained source package** that builds the
cross-toolchain used to compile the **Fern** kernel (and, later, Forest-OS
userspace). It produces GCC + binutils targeting the Forest-OS triples:

| ARCH | Target triple      | Tool prefix           |
|------|--------------------|-----------------------|
| 32   | `i686-forestos`    | `i686-forestos-*`     |
| 64   | `x86_64-forestos`  | `x86_64-forestos-*`   |

It is designed to build reproducibly on **(a)** a stock Linux host and
**(b)** self-hosted on Forest-OS itself once Forest-OS can host GCC.

> Vocabulary: the OS is **Forest-OS**, the kernel is **Fern**, the bootloader is
> **foreboots**.

---

## What is tracked vs. generated

Only the **source** of the toolchain is committed. Everything the build produces
is `.gitignore`d and regenerated — the prebuilt GCC alone is multi-GB and would
blow past GitHub's 100 MB file limit.

```
forestos-toolchain/
  build-toolchain.sh      # the ONE canonical, idempotent build script
  patches/                # config.sub / config.gcc / forestos.h patches (real *-forestos triple)
  checksums.txt           # pinned sha256 of the binutils/gcc tarballs
  sysroot-skeleton/       # TRACKED source header tree installed into sysroot/  <-- this dir
  README.md               # (this file)

  # --- all generated, gitignored, NEVER committed ---
  src/                    # downloaded + extracted binutils/gcc sources
  build-binutils/  build-gcc/   # out-of-tree build dirs
  install/                # the finished toolchain (install/bin/<prefix>-gcc, ...)
  sysroot/                # the assembled sysroot (headers from skeleton + built CRT/libc)
```

---

## Host dependencies (Linux build)

Build tools: `gcc g++ make flex bison gawk texinfo (makeinfo) curl|wget tar xz`.
Libraries (GCC prerequisites): GMP, MPFR, MPC development headers
(`libgmp-dev libmpfr-dev libmpc-dev` on Debian/Ubuntu;
`gmp-devel mpfr-devel libmpc-devel` on Fedora;
`gmp mpfr libmpc` on Arch).

`build-toolchain.sh --skip-deps` bypasses the host dependency probe (useful on
distros the probe doesn't recognize, or when building on Forest-OS).

---

## Building on a Linux host

`$FOREST` is wherever you cloned the repo (the dir holding `fern/`, `foreboots/`,
`forestos-toolchain/`). Set it with `export FOREST="$(git rev-parse --show-toplevel)"`.

```sh
cd $FOREST/forestos-toolchain
./build-toolchain.sh                # default: both arches (i686 + x86_64-forestos)
./build-toolchain.sh --arch 32      # i686-forestos only
./build-toolchain.sh --arch 64 -j8  # x86_64 only, 8 parallel jobs
./build-toolchain.sh --help         # full option list
```

The script is **idempotent**: it skips downloads whose checksum already matches,
skips `configure` where a `Makefile` already exists, and guards patch
application with markers, so re-running resumes rather than restarting.

A full build takes tens of minutes and needs several GB of scratch space in
`src/`, `build-*/`, and `install/`.

## Building self-hosted on Forest-OS

The same `build-toolchain.sh` is host-agnostic. When build == host ==
`x86_64-forestos` it skips the Debian-centric dependency probe and uses the
Forest-hosted GCC/binutils to rebuild the toolchain natively. The
`sysroot-skeleton/` headers (see below) must already be installed so that
`<stdio.h>` -> `<forestos/syscalls.h>` resolves during the build; the skeleton
in this package provides exactly that.

---

## `sysroot-skeleton/` — the tracked header source

`build-toolchain.sh` populates the generated `sysroot/` from three sources:

1. **`sysroot-skeleton/`** (tracked here) — the small, git-safe header island
   the Fern kernel actually reaches through the sysroot, plus a placeholder for
   the generated `lib/` objects.
2. The Fern kernel headers under `fern/src/include/` (copied in for the wider
   set of userspace / wrapper headers).
3. **`libs/libc/`** (consolidated libc) — the single source of truth for all
   libc headers, overlaid into the sysroot during build.

### Why the skeleton exists

The Fern kernel compiles **freestanding** and passes **no** `--sysroot`,
`-isystem`, or sysroot `-I` on the command line (see `fern/build/flags.mk`). The
sysroot is reached **only** because the cross-GCC is configured
`--with-sysroot=$(FORESTOS_TOOLCHAIN_DIR)/sysroot`. That baked-in sysroot is
load-bearing.

The single sysroot header the kernel truly pulls is `<stdio.h>` (many kernel
`.c` files `#include <stdio.h>`; there is no top-level `src/include/stdio.h`).
`<stdio.h>` transitively requires `<forestos/syscalls.h>` and `<sys/types.h>`.
`<forestos/syscalls.h>` has **no clean source anywhere else in the repo**, so it
must be carried here or the sysroot cannot be regenerated from scratch.

### Skeleton contents (critical kernel-contract island)

```
sysroot-skeleton/
  usr/include/stdio.h               # libc island entry — kernel pulls <stdio.h>
  usr/include/forestos/syscalls.h   # REQUIRED; the ONLY source of this header in the repo
  usr/include/sys/types.h           # thin wrapper -> ../libc/sys/types.h
  usr/include/libc/sys/types.h      # the real POSIX type definitions
  lib/README.md                     # marks lib/ as a generated (gitignored) output dir
```

`libc/sys/types.h` only depends on GCC's freestanding builtins
(`<stddef.h>`, `<stdint.h>`), so the island is fully self-resolving under
`-ffreestanding` for both 32-bit and 64-bit (verified). It is installed verbatim
into `sysroot/usr/include/`.

CRT and libc objects (`crt0.o`, `crti.o`, `crtn.o`, `libc.a`) are **build
outputs**, not skeleton — see `sysroot-skeleton/lib/README.md`. They are
arch-specific and regenerated, never committed.

---

## How the Fern kernel build consumes this toolchain

`fern/build/toolchain.mk` resolves tools as
`$(FORESTOS_TOOLCHAIN_DIR)/install/bin/<prefix>-{gcc,g++,ld,as,objcopy,strip,nm}`
and expects the sysroot at `$(FORESTOS_TOOLCHAIN_DIR)/sysroot`.

- `ARCH=32` -> prefix `i686-forestos`
- `ARCH=64` -> prefix `x86_64-forestos`
  (if `install/bin/x86_64-forestos-gcc` is **missing**, the kernel Makefile
  silently falls back to the host `x86_64-linux-gnu` GCC — so build the 64-bit
  target if you want a true cross build.)

`libgcc.a` is auto-located via `$(CC) -print-libgcc-file-name`, so the cross-GCC
must ship a target `libgcc.a` (this build does).

### Path caveat (important)

`toolchain.mk` defaults `FORESTOS_TOOLCHAIN_DIR ?= $(REPO_ROOT)/forestos-toolchain`
where `REPO_ROOT` is the **fern** tree — i.e. it looks in
`fern/forestos-toolchain`, but this package actually lives one level up at
`$FOREST/forestos-toolchain`. Bridge it with either:

```sh
export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain
# --- or make a symlink once ---
ln -s ../forestos-toolchain $FOREST/fern/forestos-toolchain
```

Without one of these, `make` aborts at `validate-toolchain`
("Architecture toolchain not found").

See `$FOREST/MAKE_AN_OS.md` for the full toolchain -> kernel ->
foreboots -> bootable image walkthrough.

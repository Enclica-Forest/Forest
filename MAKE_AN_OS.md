# MAKE AN OS — Building a Bootable Forest-OS from Source

This is the end-to-end, copy-pasteable guide to turn a fresh checkout of this
repository into a bootable **Forest-OS** image and run it. Every command, path,
target, and output filename below was cross-checked against the actual build
files in this repo (`fern/Makefile`, `fern/build/*.mk`, `fern/conf.sh`,
`foreboots/Makefile`).

---

## 0. Conventions — where you cloned the repo

Paths below use **`$FOREST`** to mean *the directory you cloned this repository
into* — it holds `fern/`, `foreboots/`, `forestos-toolchain/`, and `libs/`.
It can be anywhere (`~/forest`, `~/src/Forest`, `/opt/forest`, …). Set it once
per shell so every command below is copy-pasteable:

```bash
# from anywhere inside the checkout:
export FOREST="$(git rev-parse --show-toplevel)"
# …or point it at the clone explicitly:
export FOREST="$HOME/forest"
```

Everything else is expressed relative to `$FOREST`, so nothing depends on your
username or where you put the repo.

---

## 1. The component model

**Forest-OS** (codename *ALDER*) is not a monolith. It is assembled from three
independently-named source components plus a cross-toolchain:

| Component | Name | Role | Location |
|-----------|------|------|----------|
| Kernel | **Fern** | The Forest-OS kernel. Boot artifact `fern.bin` (BIOS) / `fern.elf` / `BOOTX64.EFI` (UEFI). | `fern/` |
| Bootloader | **foreboots** (ForeB) | Native raw-MBR/BIOS **and** UEFI bootloader that loads the Fern kernel. The only userspace component. | `foreboots/` |
| C library | **forestlibs** | POSIX-oriented libc the OS/ABI targets. | `libs/libc/` |
| Cross-toolchain | **forestos-toolchain** | The `i686-forestos` / `x86_64-forestos` GCC + binutils used to compile Fern. | `forestos-toolchain/` |

The whole thing is **Forest-OS**; the kernel alone is **Fern**; the bootloader
alone is **foreboots**. Building the OS means: build the toolchain, configure
Fern, build Fern, build foreboots, embed Fern into a foreboots disk image, boot.

### Repository layout

```
$FOREST/                 <- repo root
├── fern/                           <- the Fern kernel tree (build system lives here)
│   ├── Makefile                    <- top-level build orchestrator (.DEFAULT_GOAL := help)
│   ├── conf.sh                     <- Kconfig-style configurator -> build-config.mk
│   ├── build/                      <- make fragments (toolchain.mk, iso.mk, foreb.mk, ...)
│   ├── src/                        <- kernel C sources + src/include headers
│   ├── forestlibs/                 <- other OS libraries (libc consolidated to libs/libc/)
│   ├── initrd/                     <- initrd filesystem tree (packed into initrd.tar)
│   ├── build/<ARCH>bit-<MODE>-<TYPE>/   <- kernel build outputs (generated)
│   └── forestos-toolchain -> ../forestos-toolchain   (symlink; see Troubleshooting)
├── foreboots/                      <- the foreboots (ForeB) bootloader
│   ├── Makefile
│   └── build/                      <- stage1/2/3.bin, forebo.img, esp.img, forebo.iso (generated)
├── forestos-toolchain/             <- cross-toolchain SOURCE package
│   ├── build-toolchain.sh          <- canonical toolchain builder
│   ├── install/                    <- built cross-compilers (generated, gitignored)
│   └── sysroot/                    <- target sysroot: headers + crt/libc (generated)
└── libs/                           <- shared libs (uacpi vendored, libc, forestcore)
```

---

## 2. Prerequisites (host dependencies)

### Path A — building on a stock Linux host

Install these before starting. Package names are for Debian/Ubuntu; adjust for
your distro.

**To build the cross-toolchain** (`forestos-toolchain/build-toolchain.sh`):

```bash
sudo apt install build-essential gcc g++ make flex bison gawk \
                 texinfo curl wget tar xz-utils \
                 libgmp-dev libmpfr-dev libmpc-dev
```

**To build Fern + foreboots + images + run:**

```bash
sudo apt install nasm clang lld xorriso mtools python3 \
                 qemu-system-x86 ovmf dialog
```

Notes:
- `nasm` — foreboots BIOS stages (stage1/2/3).
- `clang` + `ld.lld` (from `lld`) — the foreboots **UEFI** application
  (`BOOTX64.EFI`). The UEFI app is **not** built with the forestos GCC; this is
  a deliberate toolchain split (BIOS = nasm/GCC, UEFI app = clang/lld).
- `xorriso` + `mtools` (`mkfs.fat`, `mmd`, `mcopy`) — ISO / FAT ESP image assembly.
- `python3` — foreboots asset generation.
- `dialog` — only needed for the interactive `./conf.sh --menuconfig` TUI; the
  non-interactive config modes do not need it.
- `ovmf` — UEFI firmware for QEMU. **The firmware path differs by distro**
  (see Troubleshooting §Firmware).

### Path B — self-hosting on Forest-OS

Later, once you can boot Forest-OS, the same toolchain source package is meant to
rebuild natively (`BUILD == HOST == forestos`, target `x86_64-forestos`). Steps
2–6 are identical; only Step 1 differs — you run `build-toolchain.sh` under the
Forest-hosted GCC/binutils instead of a Linux host GCC. The sysroot headers
(`<forestos/syscalls.h>`, pulled via `<stdio.h>`) and CRT bits must already be
installed, which the toolchain package's `sysroot/` provides. See
`forestos-toolchain/README.md` for the self-host specifics.

---

## 3. Step 1 — Build the cross-toolchain

The Fern build **requires** a real `i686-forestos` / `x86_64-forestos`
cross-toolchain. Build it first.

```bash
cd $FOREST/forestos-toolchain
./build-toolchain.sh --arch both        # builds BOTH i686-forestos and x86_64-forestos
```

Useful options (see `./build-toolchain.sh --help` for the authoritative list):

| Option | Effect |
|--------|--------|
| `--arch 32` | build `i686-forestos` only |
| `--arch 64` | build `x86_64-forestos` only |
| `--arch both` | build both targets (default) |
| `--jobs N` | parallel make jobs (default: `nproc`) |
| `--skip-deps` | skip host dependency probing (use on self-host / non-Debian) |
| `--clean` | wipe build dirs and rebuild from scratch |
| `--help` | show usage |

This produces, under `forestos-toolchain/`:

```
install/bin/i686-forestos-{gcc,g++,ld,as,objcopy,strip,nm,...}
install/bin/x86_64-forestos-{gcc,g++,ld,as,objcopy,strip,nm,...}   (with --arch 64/both)
sysroot/usr/include/{stdio.h, forestos/syscalls.h, sys/types.h, ...}   (kernel header island)
sysroot/lib/...   (crt/libc bits)
```

The Fern Makefile (`build/toolchain.mk`) locates tools at
`$(FORESTOS_TOOLCHAIN_DIR)/install/bin/<prefix>-gcc`. The sysroot itself is
baked in at compiler *configure* time by `forestos-toolchain/build-toolchain.sh`
(via `--with-sysroot=$(FORESTOS_TOOLCHAIN_DIR)/sysroot`), so it is reached
implicitly at kernel-compile time and the kernel passes **no** `--sysroot`
flag of its own.

> **CRITICAL — toolchain path bridging.** `build/toolchain.mk` defaults
> `FORESTOS_TOOLCHAIN_DIR ?= $(REPO_ROOT)/forestos-toolchain`, and `REPO_ROOT`
> is the **fern** dir. So the default lookup is
> `fern/forestos-toolchain`, but the toolchain actually lives one level up at
> `$FOREST/forestos-toolchain`. You **must** bridge this once, either:
>
> ```bash
> # Option A: point the Makefile at the real location (per shell / per make run)
> export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain
>
> # Option B: create the symlink the fern tree expects (persistent)
> ln -s ../forestos-toolchain $FOREST/fern/forestos-toolchain
> ```
>
> (A `fern/forestos-toolchain -> ../forestos-toolchain` symlink already exists in
> this checkout, so Option B may already be satisfied — verify with
> `ls -l $FOREST/fern/forestos-toolchain`.) Without one of these,
> `make` aborts at `validate-toolchain` with
> `Architecture toolchain not found`.

Verify the toolchain is visible to the kernel build:

```bash
cd $FOREST/fern
make ARCH=32 show-config      # should print TOOLCHAIN i686-forestos, not a host fallback
make ARCH=64 show-config      # should print x86_64-forestos; if it says "using host
                              # x86_64 toolchain", the 64-bit cross-gcc is missing
```

> **ARCH=64 host fallback.** If `install/bin/x86_64-forestos-gcc` is absent,
> `toolchain.mk` **silently** falls back to the host `x86_64-linux-gnu` GCC
> (`FORESTOS_TOOLCHAIN_HAS_64BIT := false`). That yields a build that uses host
> headers, not a true cross build. For a real 64-bit Forest-OS, confirm
> `install/bin/x86_64-forestos-gcc` exists.

---

## 4. Step 2 — Configure the Fern kernel

Fern uses a Kconfig-style flow: `conf.sh` writes `.forestos_config` and
generates `build-config.mk`, which the Makefile reads.

```bash
cd $FOREST/fern
./conf.sh --defconfig          # == `make defconfig`; sane defaults
```

`--defconfig` defaults: `BUILD_ARCH=32`, `BUILD_BOOT_MODE=bios`,
`BUILD_TYPE=debug`, `ENABLE_FOREB_BOOTLOADER=y`.

Config modes (`./conf.sh --help` for the full list):

| Command | Equivalent make target | Purpose |
|---------|------------------------|---------|
| `./conf.sh --defconfig` | `make defconfig` | write sane defaults |
| `./conf.sh --menuconfig` | `make menuconfig` | interactive TUI (needs `dialog`) |
| `./conf.sh --oldconfig` | `make oldconfig` | re-validate existing config, regenerate |
| `./conf.sh --allnoconfig` | `make allnoconfig` | all bools off (except required-on) |
| `./conf.sh --allyesconfig` | `make allyesconfig` | all bools on |
| `./conf.sh --generate` | — | regenerate `build-config.mk` from `.forestos_config` |

Key options you may want to change (via `--menuconfig`, then Save/Generate):
`BUILD_ARCH` (32/64), `BUILD_BOOT_MODE` (bios/uefi), `BUILD_TYPE` (debug/release),
`ENABLE_FOREB_BOOTLOADER` (must be `y` to build a bootable image).

Verify the effective configuration:

```bash
make configcheck        # validate build-config.mk + print effective config
make show-config        # full dump: ARCH / BOOT_MODE / TOOLCHAIN / OUTPUT
```

> You can also override config per-invocation on the make command line —
> `ARCH=`, `BOOT_MODE=`, `BUILD_TYPE=` win for that single run without editing
> `.forestos_config` (e.g. `make ARCH=64 BOOT_MODE=uefi all`).

---

## 5. Step 3 — Build the Fern kernel

> **`make` alone does nothing useful.** `Makefile` sets
> `.DEFAULT_GOAL := help`, so a bare `make` just prints the help screen. Always
> use an explicit target.

```bash
cd $FOREST/fern

make build      # compile+link the Fern kernel binary ONLY (no bootable image)
# — or —
make all        # validate toolchain, build kernel, THEN build the bootable image
                # (this is Steps 3 + 4 + 5 in one command)
```

CLI overrides work for a single run:

```bash
make ARCH=64 BOOT_MODE=uefi BUILD_TYPE=release all
```

Kernel outputs land in `build/<ARCH>bit-<BOOT_MODE>-<BUILD_TYPE>/`:

| Config | Kernel output |
|--------|---------------|
| BIOS (default, `32/bios/debug`) | `build/32bit-bios-debug/boot/fern.bin` (the linked ELF; no separate `fern.elf` in BIOS mode) |
| UEFI (e.g. `64/uefi/debug`) | `build/64bit-uefi-debug/BOOTX64.EFI` (+ `build/64bit-uefi-debug/fern.elf`) |

An initrd tarball is also produced at
`build/<...>/boot/initrd.tar` from the `initrd/` tree.

---

## 6. Creating the initrd (Initial RAM Disk)

The initrd is a tarball of files the kernel loads into memory at boot and
mounts as the root filesystem. It contains the essential userspace programs,
libraries, configuration files, and resources your OS needs.

### 6.1 initrd directory structure

```
fern/initrd/                        <- root of the initrd tree
├── bin/                            <- userspace executables
│   ├── sh                          <- shell (must be here as /bin/sh)
│   ├── init                        <- init process (PID 1)
│   ├── ls
│   ├── cat
│   ├── echo
│   ├── mkdir
│   ├── mount
│   ├── reboot
│   └── ...
├── etc/                            <- configuration files
│   ├── hostname
│   ├── passwd
│   ├── group
│   └── fstab
├── usr/
│   ├── bin/                        <- additional programs
│   ├── lib/                        <- shared libraries
│   │   ├── libc.so
│   │   └── libm.so
│   └── share/
│       └── ...                     <- fonts, icons, wallpapers, sounds
├── dev/                            <- device nodes (usually created by kernel/devfs)
├── proc/                           <- mount point for /proc (usually empty in initrd)
├── tmp/                            <- temporary files
└── var/
    └── log/                        <- log files
```

### 6.2 Building userspace programs into the initrd

Each userspace program must be compiled with the Forest cross-toolchain and
placed into `fern/initrd/bin/`. The process:

```bash
# Set up cross-compiler
export CC="$FOREST/forestos-toolchain/install/bin/i686-forestos-gcc"
export SYSROOT="$FOREST/forestos-toolchain/sysroot"

# Compile a simple program
$CC -nostdlib -ffreestanding -I$SYSROOT/usr/include \
    -o $FOREST/fern/initrd/bin/myprogram \
    myprogram.c -L$SYSROOT/lib -lc

# Or use the consolidated libc build system
cd $FOREST/libs/libc
make DESTDIR=$FOREST/fern/initrd install
```

### 6.3 Essential initrd programs

At minimum, your initrd **must** contain:

| Program | Purpose | Why it's essential |
|---------|---------|-------------------|
| `/bin/init` | PID 1 process | Kernel executes `/init` after boot |
| `/bin/sh` | Shell | Users interact with the OS through the shell |
| `/bin/mount` | Mount filesystems | Needed to mount /proc, /dev, /tmp |
| `/bin/echo` | Print text | Used by shell scripts |
| `/bin/ls` | List files | Users need to see what's on disk |
| `/bin/cat` | Display file contents | Users need to read files |
| `/bin/mkdir` | Create directories | Needed for runtime setup |
| `/bin/reboot` | Reboot system | Users need to restart |
| `/bin/ps` | List processes | Users need to see running tasks |

### 6.4 Adding shared libraries

Shared libraries go in `/usr/lib/` within the initrd. The dynamic linker
(`/lib/ld.so` or `/lib/ld-forestos.so`) must also be present if you use
dynamic linking.

```bash
# Copy libc and other shared libs
cp $SYSROOT/lib/libc.so $FOREST/fern/initrd/usr/lib/
cp $SYSROOT/lib/libm.so $FOREST/fern/initrd/usr/lib/

# Create the dynamic linker symlink
ln -sf /usr/lib/libc.so $FOREST/fern/initrd/lib/ld-forestos.so
```

### 6.5 Adding resources (fonts, icons, wallpapers)

```bash
# Fonts (for the framebuffer console / GUI)
mkdir -p $FOREST/fern/initrd/usr/share/fonts
cp my-font.ttf $FOREST/fern/initrd/usr/share/fonts/

# Icons
mkdir -p $FOREST/fern/initrd/usr/share/icons/hicolor/48x48/apps
cp my-icon.png $FOREST/fern/initrd/initrd/usr/share/icons/hicolor/48x8/apps/

# Wallpapers
mkdir -p $FOREST/fern/initrd/usr/share/wallpapers
cp wallpaper.png $FOREST/fern/initrd/usr/share/wallpapers/

# Sound files
mkdir -p $FOREST/fern/initrd/usr/share/sounds
cp beep.wav $FOREST/fern/initrd/usr/share/sounds/
```

### 6.6 The initrd build process

The Fern Makefile automatically packs `fern/initrd/` into a tarball:

```bash
cd $FOREST/fern

# The initrd is built automatically during `make all` or `make build`:
make build    # produces build/<ARCH>bit-<MODE>-<TYPE>/boot/initrd.tar

# Or build just the initrd:
tar -C initrd -cf build/32bit-bios-debug/boot/initrd.tar .
```

The resulting `initrd.tar` is embedded into the bootable image by foreboots.
The kernel loads it at boot time and extracts it to a ramdisk.

### 6.7 initrd size limits

| Limit | Value | Notes |
|-------|-------|-------|
| Max initrd size (BIOS) | ~14 MB | ForeB loads it via INT 13h; limited by conventional memory |
| Max initrd size (UEFI) | ~128 MB | Loaded from FAT ESP; limited by available RAM |
| Recommended max | ~8 MB | Keeps boot fast; larger initrds slow boot significantly |

To check your initrd size:

```bash
ls -lh $FOREST/fern/initrd.tar
du -sh $FOREST/fern/initrd/
```

### 6.8 Customizing the initrd

You can add or remove files from the initrd tree at any time. The build
system watches for changes:

```bash
# Add a new program
cp myprogram $FOREST/fern/initrd/bin/

# Remove a program you don't need
rm $FOREST/fern/initrd/bin/unwanted-program

# Rebuild (make will detect the change and rebuild initrd.tar)
make build
```

### 6.9 initrd example: minimal shell-only initrd

```bash
cd $FOREST/fern
mkdir -p initrd/bin initrd/etc initrd/tmp initrd/proc

# Create a minimal init script
cat > initrd/bin/init << 'EOF'
#!/bin/sh
echo "Forest-OS starting..."
mount -t proc proc /proc
mount -t devfs devfs /dev
echo "Welcome to Forest-OS!"
exec /bin/sh
EOF
chmod +x initrd/bin/init

# Copy essential binaries from the toolchain sysroot
cp $FOREST/forestos-toolchain/sysroot/bin/sh initrd/bin/
cp $FOREST/forestos-toolchain/sysroot/bin/ls initrd/bin/
cp $FOREST/forestos-toolchain/sysroot/bin/cat initrd/bin/

# Build
make build
```

### 6.10 initrd example: full desktop initrd

```bash
cd $FOREST/fern

# Create a complete directory structure
mkdir -p initrd/{bin,etc,usr/{bin,lib,share/{fonts,icons,wallpapers}},dev,proc,tmp,var/log}

# Copy all userspace programs
cp $FOREST/userspace/*/initrd/bin/* initrd/bin/ 2>/dev/null

# Copy all shared libraries
cp $FOREST/forestos-toolchain/sysroot/lib/*.so initrd/usr/lib/ 2>/dev/null

# Copy resources
cp -r $FOREST/fern/tools/initrd/usr/share/* initrd/usr/share/

# Create init script with full boot sequence
cat > initrd/bin/init << 'INITEOF'
#!/bin/sh
mount -t proc proc /proc
mount -t devfs devfs /dev
mount -t tmpfs tmpfs /tmp
echo "Forest-OS $(cat /etc/hostname 2>/dev/null || echo 'v1.0')"
echo "Starting services..."
# Start display manager, network, etc.
exec /bin/sh
INITEOF
chmod +x initrd/bin/init

# Build the full OS
make all
```

---

## 7. Step 4 + Step 5 — Build foreboots and assemble the bootable image

In the Fern tree, `make iso` and `make img` **both** just delegate to
`forebo-image` (`build/iso.mk`). The one canonical command that builds foreboots
**and** embeds the current Fern kernel into a bootable disk image is:

```bash
cd $FOREST/fern
make forebo-image
```

Under the hood (`build/foreb.mk`), this runs:

```
make -C $FOREST/foreboots all
make -C $FOREST/foreboots image KERNEL=<abspath of the configured OUTPUT>
```

It is gated on `ENABLE_FOREB_BOOTLOADER=yes` (set by `--defconfig`). If disabled,
every `forebo*` target prints how to re-enable it and exits 1.

foreboots outputs (`foreboots/build/`):

| File | What it is |
|------|-----------|
| `stage1.bin` | 512-byte MBR stage 1 |
| `stage2.bin` | stage 2 (≤ 8 KiB) |
| `stage3.bin` | stage 3 (≤ 8 KiB) |
| `forebo.img` | raw BIOS disk: stage1 @ sector 0, stage2 @ 1, stage3 @ 17, kernel @ 48 |
| `esp.img` | FAT EFI System Partition: `BOOTX64.EFI` at `/EFI/BOOT/`, kernel at `/forebo/kernel.elf` |
| `forebo.iso` | hybrid BIOS+UEFI ISO (`make -C foreboots iso`, via xorriso) |

Verify the stages and MBR signature:

```bash
make forebo-check        # runs foreboots `check` (validates stage sizes + 0x55AA MBR signature)
```

You can also build foreboots directly, passing an explicit Fern kernel path:

```bash
make -C $FOREST/foreboots image \
     KERNEL=$FOREST/fern/build/32bit-bios-debug/boot/fern.bin
```

---

## 8. Step 6 — Run in QEMU / install to hardware

### Run in QEMU (easiest — builds the image if needed, then boots)

```bash
cd $FOREST/fern

make run          # honors configured BOOT_MODE. For BIOS: builds forebo-image,
                  # then boots foreboots/build/forebo.img with qemu-system-i386
                  # (-vga std -serial stdio)
make run-bios     # force BIOS path (forebo.img, qemu-system-i386)
make run-uefi     # force UEFI (keeps configured ARCH): boots foreboots/build/esp.img
                  # under OVMF (-bios /usr/share/ovmf/OVMF.fd). With the default
                  # ARCH=32 this is qemu-system-i386; ARCH=64 uses qemu-system-x86_64.
make run32        # ARCH=32 BIOS
make run64        # ARCH=64 BIOS
make debug        # same as run but with a GDB stub on :1234 (qemu -s -S)
```

You can also drive foreboots' own QEMU targets directly:

```bash
make forebo-qemu                              # from fern: build image + boot in QEMU
make -C $FOREST/foreboots qemu     # BIOS, boots forebo.img
make -C $FOREST/foreboots qemu-uefi   # UEFI/OVMF, boots esp.img
```

Boot the BIOS image by hand:

```bash
qemu-system-i386 -drive format=raw,file=$FOREST/foreboots/build/forebo.img \
                 -serial stdio -vga std
```

> There is also a `fern/run.sh` script with more flags
> (`--bios|--uefi -a 32|64 --build --dry-run --debug`), but it searches for
> images named `dist/forestos_*bit_*.{iso,img}` — a layout the current
> `make`/foreboots flow does **not** emit (outputs go to `foreboots/build/`). Use
> `make run` as the reliable path, or `run.sh -i <image>` / `run.sh --build`.

### Install to real hardware (DANGEROUS)

`foreboots`' `install-disk` writes **only** the three MBR stages to the target
device — it does **not** copy the kernel:

```bash
make -C $FOREST/foreboots install-disk DISK=/dev/sdX
```

To boot from real hardware you must also place the kernel. The simplest reliable
method is to build the full `forebo.img` (which already embeds stage1/2/3 + the
Fern kernel at sector 48) and write the whole image to the device:

```bash
# DOUBLE-CHECK the device node — this destroys existing data on /dev/sdX
sudo dd if=$FOREST/foreboots/build/forebo.img of=/dev/sdX bs=1M conv=fsync
sync
```

For UEFI hardware, write `esp.img` to a FAT partition (or use the hybrid
`forebo.iso` on removable media).

---

## 9. Userspace Applications

Forest-OS includes **44 userspace applications** in `userspace/` — a complete
set of POSIX-compatible tools for a functional operating system.

### Building userspace apps

```bash
cd $FOREST/userspace

# Build all apps (requires forestos-toolchain to be built first)
make

# Build for 64-bit
make ARCH=64
```

Each app has its own directory with `app.c` and `Makefile`. Apps are compiled
with the Forest-OS cross-compiler (`i686-forestos-gcc`) as freestanding ELF32
binaries. They use the consolidated libc (`libs/libc/`) for POSIX functions.

### Userspace app categories

| Category | Apps |
|----------|------|
| **Core utilities** | cat, echo, ls, cp, mv, rm, mkdir, rmdir, touch, chmod, chown, ln, pwd, basename, dirname |
| **Text processing** | grep, find, sort, wc, head, tail |
| **Disk/filesystem** | dd, df, du, fdisk, mkfs, mount, umount, blkid, losetup, fsck |
| **System tools** | ps, kill, init, shutdown, reboot, hostname, uname, date, sleep, id |
| **Shell & auth** | forest-shell, sudo, su |
| **Special** | initrd-builder (host tool) |

### The shell: forest-shell

`forest-shell` is the primary interactive shell. It provides:

- **19 builtins**: cd, exit, export, unset, env, set, pwd, echo, type, which, history, source, alias, unalias, jobs, fg, bg, wait, help, clear
- **Piping**: `cmd1 | cmd2 | cmd3`
- **I/O redirection**: `>`, `>>`, `<`, `2>`, `2>>`, `&>`
- **Background jobs**: `cmd &` with job control (Ctrl+Z, jobs, fg, bg)
- **Variable expansion**: `$VAR`, `${VAR}`, `$?`, `$#`, `$$`, `$@`, `$*`
- **Quoting**: single quotes (literal), double quotes (expansion), backslash escaping
- **Globbing**: `*`, `?` via opendir/readdir
- **Command substitution**: `` `cmd` ``
- **Aliases**: `alias name=command`
- **History**: arrow keys (Up/Down), last 100 commands
- **Signal handling**: Ctrl+C (SIGINT), Ctrl+\ (SIGQUIT), Ctrl+Z (SIGTSTP)

The shell is placed at `/bin/forest-shell` in the initrd. The kernel's session
manager (`session.c`) looks for `/bin/shell` — create a symlink:

```bash
# In the initrd staging area:
ln -s forest-shell initrd/bin/shell
```

### sudo and su

- `sudo` — execute commands as root with `/etc/sudoers` support, password
  authentication via `/etc/shadow`, timestamp caching, logging
- `su` — switch user identity with login shell support, environment setup

Both are installed setuid (mode 4755) so they can elevate privileges.

---

## 10. Building the initrd (Initial RAM Disk)

The initrd is a tarball of the root filesystem that the kernel loads at boot.
The kernel's ramdisk module (`fern/src/ramdisk.c`) parses it and mounts it as `/`.

### What goes in the initrd

The initrd is your root filesystem. At minimum it needs:

```
/
├── bin/            # Essential commands (cat, ls, sh, mount, etc.)
├── dev/            # Device nodes (kernel creates devtmpfs)
├── etc/            # Config files (passwd, shadow, fstab)
├── proc/           # Mount point for procfs
├── sys/            # Mount point for sysfs
├── tmp/            # Temporary files
├── usr/
│   └── bin/        # Additional commands
├── var/
│   ├── log/        # Log files
│   ├── run/        # Runtime data
│   └── tmp/        # Temp files
```

### Method 1: Use the userspace Makefile

```bash
cd $FOREST/userspace
make install

# This builds all apps and creates initrd staging at build/initrd/
# The staging area has bin/, sbin/, etc/, dev/, proc/, sys/, tmp/, usr/
```

### Method 2: Build and use initrd-builder

`initrd-builder` is a host tool that creates newc cpio initrd images:

```bash
cd $FOREST/userspace
make initrd

# Create an initrd from a directory tree:
./build/initrd-builder -d /path/to/rootfs -o /path/to/initrd.img

# With config file:
./build/initrd-builder -c config.txt -o initrd.img -v

# Config file format (one entry per line):
#   /bin/ls 0755 0 0
#   /bin/sh 0755 0 0
#   /etc 0755 0 0 dir
#   /dev/null 0666 0 0 c 1 3
```

### Method 3: Use the existing initrd directory

The Fern kernel tree has a pre-built initrd at `fern/initrd/`:

```bash
# Add your apps to the existing initrd:
cp $FOREST/userspace/build/bin/* $FOREST/fern/initrd/bin/

# The kernel Makefile creates initrd.tar from this directory:
cd $FOREST/fern
make build   # creates initrd.tar automatically
```

### Integrating userspace apps into the kernel's initrd

The simplest approach for a working system:

```bash
# 1. Build the kernel (creates initrd.tar)
cd $FOREST/fern
make build

# 2. Build userspace apps
cd $FOREST/userspace
make

# 3. Copy apps into the kernel's initrd directory
cp $FOREST/userspace/build/bin/* $FOREST/fern/initrd/bin/

# 4. Create the shell symlink (kernel looks for /bin/shell)
ln -sf forest-shell $FOREST/fern/initrd/bin/shell

# 5. Rebuild the kernel with updated initrd
cd $FOREST/fern
make build

# 6. Rebuild the bootable image (embeds new initrd)
make forebo-image

# 7. Run it
make run
```

### Initrd filesystem layout (recommended)

For a complete Forest-OS system, populate the initrd with:

```bash
# Create staging directory
ROOTFS=$FOREST/fern/initrd
mkdir -p $ROOTFS/{bin,sbin,etc,dev,proc,sys,tmp,home/root,var/{log,run,tmp}}
mkdir -p $ROOTFS/usr/{bin,lib,share}

# Copy all userspace apps
cp $FOREST/userspace/build/bin/* $ROOTFS/bin/

# Create shell symlink
ln -sf forest-shell $ROOTFS/bin/shell
ln -sf forest-shell $ROOTFS/bin/sh

# Create essential device nodes (if not using devtmpfs)
# mknod $ROOTFS/dev/null c 1 3
# mknod $ROOTFS/dev/zero c 1 5
# mknod $ROOTFS/dev/console c 5 1
# mknod $ROOTFS/dev/tty c 5 0
# mknod $ROOTFS/dev/ptmx c 5 2
# mknod $ROOTFS/dev/urandom c 1 9

# Create config files
echo "root:x:0:0:root:/root:/bin/forest-shell" > $ROOTFS/etc/passwd
echo "root::0:0:99999:7:::" > $ROOTFS/etc/shadow
echo "root:x:0:" > $ROOTFS/etc/group

# Create /etc/fstab (optional)
cat > $ROOTFS/etc/fstab << 'EOF'
proc     /proc    proc     defaults  0 0
sysfs    /sys     sysfs    defaults  0 0
tmpfs    /tmp     tmpfs    defaults  0 0
EOF
```

### How the kernel loads the initrd

1. Bootloader loads kernel + initrd into memory
2. `ramdisk_init()` in `fern/src/ramdisk.c` finds the initrd via multiboot tags
3. `parse_tar()` indexes all files in the tar archive
4. Virtual directories (`dev/`, `proc/`, `sys/`, etc.) are created automatically
5. `vfs_init()` mounts the ramdisk as the root filesystem
6. Session manager (`session.c`) searches for `/bin/shell` and loads it as PID 2+

### Kernel session flow

```
kmain()
  → ramdisk_init()     # parse initrd tar
  → vfs_init()         # mount root filesystem
  → session_run()      # session manager
    → run_session_login()  # authenticate user
    → launch_user_session()
      → load_first_elf("/bin/shell")  # find and load shell
      → task_create_elf(elf_data, elf_size, "sh")
      → task->tty_fd = session_id - 1  # link to TTY
      → task_set_foreground(shell_task)
```

The shell then reads from stdin (keyboard via TTY) and writes to stdout (TTY
display). Commands are executed via fork/exec. The kernel handles all
filesystem, memory, and process management through syscalls.

---

## 11. Quick start (TL;DR — Path A, Linux host, BIOS 32-bit)

```bash
# 0. install host deps (see §2)

# 1. build the cross-toolchain
cd $FOREST/forestos-toolchain
./build-toolchain.sh --arch both

# bridge the toolchain path (once per shell) — see §3
export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain

# 2. configure Fern
cd $FOREST/fern
./conf.sh --defconfig

# 3. build kernel
make build

# 4. build userspace apps
cd $FOREST/userspace
make

# 5. copy apps into initrd
cp $FOREST/userspace/build/bin/* $FOREST/fern/initrd/bin/
ln -sf forest-shell $FOREST/fern/initrd/bin/shell
ln -sf forest-shell $FOREST/fern/initrd/bin/sh

# 6. rebuild kernel with updated initrd
cd $FOREST/fern
make build

# 7. build bootable image
make forebo-image

# 8. run it
make run
```

---

## 12. Using the GUI build tool (createos.sh)

For a guided, menu-driven build experience, use the `createos.sh` script:

```bash
cd $FOREST
./createos.sh
```

This launches an interactive GUI (using `dialog`) that walks you through:
1. Selecting architecture (x86 32-bit, x86 64-bit, ARM32, AArch64, RISC-V 64)
2. Selecting boot mode (BIOS or UEFI)
3. Selecting build type (debug or release)
4. Enabling/disabling features (OpenGL, networking, audio, etc.)
5. Configuring the initrd (which programs to include)
6. Building everything and outputting to `output/`

See `createos.sh --help` for non-interactive options.

---

## 13. Troubleshooting

**`Architecture toolchain not found: .../install`** (build aborts at
`validate-toolchain`). The Makefile is looking at
`fern/forestos-toolchain/install` but the toolchain is at
`$FOREST/forestos-toolchain`. Fix with either
`export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain` or
`ln -s ../forestos-toolchain $FOREST/fern/forestos-toolchain`
(see §3). Also confirm Step 1 actually completed and produced
`install/bin/i686-forestos-gcc`.

**64-bit build silently uses the host compiler.** If `show-config` /
`make ARCH=64 ...` prints *"Forest OS 64-bit toolchain not found - using host
x86_64 toolchain"*, then `install/bin/x86_64-forestos-gcc` is missing. Re-run
`./build-toolchain.sh --arch 64` (or `--arch both`). The fallback compiles but is
not a true cross build.

**`fatal error: stdio.h: No such file or directory`** (or missing
`<forestos/syscalls.h>`) when compiling kernel `.c` files. The kernel pulls a
small sysroot header island (`stdio.h` → `forestos/syscalls.h` → `sys/types.h`)
via the baked-in `--with-sysroot`. This means your `forestos-toolchain/sysroot`
was not populated. Re-run `build-toolchain.sh` (it installs the sysroot header
skeleton); confirm the files exist:
`ls forestos-toolchain/sysroot/usr/include/stdio.h forestos-toolchain/sysroot/usr/include/forestos/syscalls.h`.

**`make` printed a help screen and did nothing.** That is expected — the default
goal is `help`. Use `make all` (kernel + image), `make build` (kernel only),
`make iso` / `make forebo-image` (image), or `make run`.

**`ForeB bootloader is disabled`.** `ENABLE_FOREB_BOOTLOADER` is not `yes`. Run
`./conf.sh --menuconfig` (set it `y`) then `./conf.sh --generate`, or
`./conf.sh --defconfig` (which sets it).

**`NASM assembler not found`.** Install `nasm` (foreboots BIOS stages).

**UEFI app fails to link / `ld.lld` not found.** The foreboots UEFI application
needs `clang` + `ld.lld` (package `lld`), not the forestos GCC.

**Firmware / OVMF path mismatch (UEFI).** Two different OVMF paths are used:
- `fern` `make run-uefi` uses `/usr/share/ovmf/OVMF.fd`.
- `foreboots` UEFI targets (`qemu-uefi`) use
  `/usr/share/edk2/x64/OVMF_CODE.4m.fd` + `/usr/share/edk2/x64/OVMF_VARS.4m.fd`.

Distro package names and paths differ (Debian `ovmf`, Arch `edk2-ovmf`, etc.). If
UEFI boot fails with a firmware error, install OVMF and point the run at the file
your distro actually ships (symlink or edit the path if needed).

**`dialog` errors from `./conf.sh --menuconfig`.** Install `dialog`, or use the
non-interactive modes (`--defconfig`, `--oldconfig`, `--allnoconfig`,
`--allyesconfig`).

**initrd is too large / boot fails.** Check the initrd size with
`ls -lh initrd.tar`. If it exceeds ~14 MB for BIOS or ~128 MB for UEFI,
remove unnecessary files from `initrd/`. Common offenders: large fonts,
unnecessary shared libraries, debug symbols.

**Programs segfault at boot.** Ensure all binaries in `initrd/bin/` are compiled
with the Forest cross-toolchain (`i686-forestos-gcc`), not the host GCC.
Host-compiled binaries use a different ABI and will crash.

**Shell script "#!/bin/sh" not found.** Make sure `/bin/sh` exists in the initrd
and is executable: `chmod +x initrd/bin/sh`.

**Shell can't find commands.** The shell looks for commands in
`/bin:/usr/bin:/usr/local/bin`. Ensure your userspace apps are copied to
`initrd/bin/` (or `initrd/usr/bin/`). Also verify the shell binary itself is
at `/bin/forest-shell` or `/bin/sh`.

**Shell segfaults or hangs.** Ensure the shell is compiled with the Forest
cross-toolchain (`i686-forestos-gcc`), not the host GCC. Host-compiled binaries
use a different ABI and will crash or behave unexpectedly.

**sudo/su password prompts don't work.** If `/etc/shadow` doesn't exist in the
initrd, sudo/su accept any password. Create the file with:
`echo "root::0:0:99999:7:::" > initrd/etc/shadow` (empty password = no auth
required) or `echo "root:HASH:0:0:99999:7:::"` with a proper crypt hash.

**Kernel doesn't start shell.** The session manager searches for `/bin/shell`.
Create the symlink: `ln -sf forest-shell initrd/bin/shell`. Also check that
the binary is a valid ELF32 (`file initrd/bin/forest-shell` should say ELF32).

---

## 14. Self-host on Forest-OS (Path B, summary)

Once Forest-OS boots and hosts a working `x86_64-forestos` GCC/binutils:

1. Rebuild the toolchain natively:
   `cd forestos-toolchain && ./build-toolchain.sh --arch 64 --skip-deps`
   (the script detects `BUILD == HOST == forestos` and skips Debian-specific
   dependency probing; the pre-installed sysroot headers + CRT satisfy the
   self-host contract).
2. Steps 2–6 are identical to Path A: `./conf.sh --defconfig` → `make all` →
   `make run` (or install to disk).

See `forestos-toolchain/README.md` for the authoritative self-host details.

---

## 15. Output structure

After a successful build, your output directory will contain:

```
output/                                 <- final OS output
├── forestos-bios-32bit.img            <- raw BIOS disk image (write to USB/HDD)
├── forestos-uefi-64bit.img            <- raw UEFI disk image (write to USB/HDD)
├── forestos-bios-32bit.iso            <- hybrid BIOS+UEFI ISO (burn to CD/DVD)
├── forestos-uefi-64bit.iso            <- UEFI-only ISO
├── esp.img                            <- EFI System Partition (for UEFI boot)
├── kernel/
│   ├── fern.bin                       <- kernel binary (BIOS)
│   ├── fern.elf                       <- kernel ELF (UEFI)
│   └── BOOTX64.EFI                    <- UEFI bootloader application
├── initrd/
│   └── initrd.tar                     <- initrd tarball
├── bootloader/
│   ├── stage1.bin                     <- MBR stage 1
│   ├── stage2.bin                     <- stage 2
│   └── stage3.bin                     <- stage 3
└── README.txt                         <- build info and how to boot
```

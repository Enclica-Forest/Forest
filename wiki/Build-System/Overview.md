# Forest OS Build System — Overview

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [The Build Pipeline](#the-build-pipeline)
3. [Build Dependencies Between Components](#build-dependencies-between-components)
4. [The Make-Based Build System](#the-make-based-build-system)
5. [Configuration System Integration](#configuration-system-integration)
6. [Feature Gating and Conditional Compilation](#feature-gating-and-conditional-compilation)
7. [Build Outputs and Artifacts](#build-outputs-and-artifacts)
8. [The createos.sh Interactive Build Tool](#the-createossh-interactive-build-tool)
9. [Build Troubleshooting](#build-troubleshooting)
10. [Clean Builds vs Incremental Builds](#clean-builds-vs-incremental-builds)

---

## Architecture Overview

Forest OS is assembled from three independently-built components plus a cross-toolchain:

| Component | Directory | What it produces |
|-----------|-----------|-----------------|
| **Fern** (kernel) | `fern/` | `fern.bin` (BIOS), `fern.elf` / `BOOTX64.EFI` (UEFI) |
| **ForeB** (bootloader) | `foreboots/` | `stage1/2/3.bin`, `BOOTX64.EFI`, `forebo.img`, `esp.img`, `forebo.iso` |
| **Userspace** | `userspace/` | Cross-compiled ELF binaries (`ls`, `cat`, `forest-shell`, etc.) |
| **Cross-toolchain** | `forestos-toolchain/` | `i686-forestos-gcc`, `x86_64-forestos-gcc` and friends |

The Fern kernel build system at `fern/Makefile` is the central orchestrator. It delegates to ForeB for disk image and ISO creation, and to the userspace tree for initial ramdisk contents.

```
$FOREST/
├── fern/                         # Kernel source tree + Make-based build system
│   ├── Makefile                  # Top-level orchestrator (698 lines)
│   ├── conf.sh                   # Kconfig-style configurator
│   └── build/                    # Make fragments (*.mk) + features/
├── foreboots/                    # ForeB bootloader (BIOS assembly + UEFI C)
│   ├── Makefile                  # 955 lines, BIOS + UEFI targets
│   ├── bios/                     # NASM assembly stages (1/2/3)
│   └── uefi/                     # Clang-compiled UEFI application
├── forestos-toolchain/           # Cross-toolchain source package
│   ├── build-toolchain.sh        # Canonical builder (787 lines)
│   └── install/                  # Built compilers (generated)
├── userspace/                    # 40+ cross-compiled userspace apps
│   └── Makefile
├── libs/                         # Shared libraries (libc, uacpi, qrcodegen)
├── createos.sh                   # Interactive build GUI (1012 lines)
└── MAKE_AN_OS.md                 # End-to-end build guide
```

---

## The Build Pipeline

Building Forest OS follows a strict linear pipeline. Each stage depends on the previous one.

```
┌─────────────────────────────────────────────────────────────────┐
│                    Forest OS Build Pipeline                      │
├─────────────────────────────────────────────────────────────────┤
│  1. TOOLCHAIN     forestos-toolchain/build-toolchain.sh         │
│     └─> install/bin/{i686,x86_64}-forestos-{gcc,ld,...}        │
│                                                                  │
│  2. CONFIGURATION  fern/conf.sh --defconfig / --menuconfig      │
│     └─> .forestos_config  -->  build-config.mk                  │
│                                                                  │
│  3. KERNEL BUILD   fern/make build  (ARCH=32/64, BOOT_MODE=...) │
│     └─> build/<arch>bit-<mode>-<type>/boot/fern.bin             │
│                                                                  │
│  4. USERSPACE      userspace/make all                           │
│     └─> userspace/build/bin/{ls,cat,forest-shell,...}           │
│                                                                  │
│  5. INITRD         tar -C fern/initrd -cf initrd.tar .          │
│     └─> initrd.tar (userspace binaries + scripts)               │
│                                                                  │
│  6. BOOTLOADER     fern/make iso  (or img for UEFI)            │
│     └─> foreboots/forebo.img, esp.img, forebo.iso              │
│                                                                  │
│  7. QEMU TEST      fern/make run / run-uefi / run64             │
│     └─> Boots the image in QEMU                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Quick Start

```bash
# One-liner for a working build:
cd forest/fern && make defconfig && make all

# Or use the interactive GUI:
cd forest && ./createos.sh
```

---

## Build Dependencies Between Components

```
                        ┌──────────────┐
                        │  toolchain   │
                        └──────┬───────┘
                               │
                    ┌──────────▼──────────┐
                    │    kernel (Fern)     │
                    └──────────┬──────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
    ┌─────────▼──────┐  ┌─────▼──────┐  ┌──────▼─────┐
    │   userspace    │  │   initrd    │  │   ForeB    │
    │  (40+ apps)    │  │  (tarball)  │  │  (bootloader)
    └─────────┬──────┘  └─────┬──────┘  └──────┬─────┘
              └────────────────┼────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │  bootable image      │
                    │  iso / img / esp.img │
                    └─────────────────────┘
```

Key rules:

- **Toolchain** must exist before anything else compiles.
- **Kernel** must link before ForeB can embed it into a disk image.
- **Userspace** must compile before the initrd can be packed with real binaries.
- **ForeB** embeds the kernel at a fixed disk sector offset (`KERNEL_SEEK = 48`).
- **Initrd** can be built any time but must exist before the final bootable image.

---

## The Make-Based Build System

### Fragment Architecture

The main `fern/Makefile` is a thin orchestrator — it contains only compile/link pattern rules and top-level targets. All configuration, directory layout, toolchain detection, flags, source selection, feature gating, and cleanup logic lives in `build/*.mk` fragments.

Include order matters:

```makefile
include build/config.mk           # 1.  Load configuration
include build/dirs.mk              # 2.  Directory layout
include build/toolchain.mk         # 3.  Toolchain detection
include build/flags.mk             # 4.  Compiler/linker flags
include build/kernel-sources.mk    # 5.  Source file aggregation
include build/features/memory.mk   # 6-21. Feature gates (16 fragments)
include build/features/filesystems.mk
include build/features/graphics.mk
include build/features/opengl.mk
include build/features/networking.mk
include build/features/audio.mk
include build/features/usb.mk
include build/features/storage.mk
include build/features/input.mk
include build/features/ipc.mk
include build/features/timers.mk
include build/features/interrupts.mk
include build/features/security.mk
include build/features/hardware.mk
include build/features/scheduler.mk
include build/features/compat.mk
include build/iso.mk              # 22. Initrd + bootable image plumbing
include build/foreb.mk            # 23. ForeB bootloader integration
include build/qemu-run.mk         # 24. QEMU launch targets
include build/clean.mk            # 25. Cleanup targets
```

### Key Targets

| Target | What it does |
|--------|-------------|
| `make help` | Show available targets |
| `make defconfig` | Write sane defaults |
| `make menuconfig` | Interactive TUI config editor |
| `make configcheck` | Validate and print effective config |
| `make all` | Build kernel + create bootable image |
| `make build` | Build kernel binary only |
| `make iso` / `make img` | Create bootable ISO or disk image |
| `make run` | Build and launch in QEMU |
| `make debug` | Launch with GDB stub |
| `make clean` / `make clean-all` | Remove build artifacts |

Multi-arch shortcuts: `make build32`, `make build64`, `make buildarm`, `make buildaarch64`, `make buildriscv64`, `make buildall`.

---

## Configuration System Integration

Forest OS uses a Kconfig-inspired flow:

```
.forestos_config  ──(conf.sh --generate)──>  build-config.mk  ──(include)──>  Make vars
```

### Steps

1. **Edit choices**: `./conf.sh --defconfig`, `--menuconfig`, `--oldconfig`, `--allnoconfig`, `--allyesconfig`
2. **Generate**: `./conf.sh --generate` reads `.forestos_config` and writes `build-config.mk`
3. **Build**: `make` reads `build-config.mk` automatically

### Core Choice Variables

| Variable | Valid values | Default |
|----------|-------------|---------|
| `ARCH` | `32`, `64`, `arm`, `aarch64`, `riscv64` | `32` |
| `BOOT_MODE` | `bios`, `uefi` | `bios` |
| `BUILD_TYPE` | `debug`, `release`, `optimize` | `debug` |

Plus ~170 boolean `ENABLE_*` flags and ~40 numeric tunables (heap sizes, buffer limits, etc.).

### CLI Overrides

Override any variable on the command line — CLI values win over `build-config.mk`:

```bash
make ARCH=64 BOOT_MODE=uefi BUILD_TYPE=release
```

### How config.mk Works

`build/config.mk` is the first include. It:

1. Loads `build-config.mk` via `-include` (no error if missing)
2. Validates `ARCH`, `BOOT_MODE`, `BUILD_TYPE` against valid lists
3. Forces every undefined `ENABLE_*` to `no` (no undefined variables)
4. Provides `?=` defaults for numeric tunables

You can run `make` without ever running `conf.sh` — it uses safe defaults.

---

## Feature Gating and Conditional Compilation

Feature gating uses **exclusion lists**, not inclusion lists. The system controls what source files get compiled by appending to `EXCLUDED_CSOURCES`.

### How It Works

1. `build/kernel-sources.mk` defines a base `EXCLUDED_CSOURCES` (always-excluded files).
2. Each `build/features/*.mk` fragment **appends** to `EXCLUDED_CSOURCES` when a feature is disabled.
3. `build/kernel-sources.mk` computes `CSOURCES` via `$(filter-out $(EXCLUDED_CSOURCES),$(wildcard src/*.c))`.
4. Because `CSOURCES` uses **deferred expansion** (`=` not `:=`), feature appends are picked up at rule-evaluation time.

### Example

```makefile
# In build/features/networking.mk:
ifeq ($(ENABLE_NETWORKING),no)
EXCLUDED_CSOURCES += $(SRCDIR)/net.c $(wildcard $(SRCDIR)/networking/*.c)
endif
```

Setting `ENABLE_NETWORKING=no` excludes `net.c`, `virtio_net.c`, and all `src/networking/*.c` from compilation.

### The 16 Feature Fragments

| Fragment | Controls |
|----------|---------|
| `memory.mk` | Paging, slab, COW, swap, OOM, memory protection |
| `filesystems.mk` | VFS, ext2, FAT32, exFAT, ISO9660, tmpfs, procfs |
| `graphics.mk` | Framebuffer, VGA, GPU drivers, Wayland, X11, fonts |
| `opengl.mk` | Software OpenGL renderer |
| `networking.mk` | TCP/IP stack, protocols, NIC drivers |
| `audio.mk` | Sound system (PC speaker, SB16, AC97, HDA) |
| `usb.mk` | USB host controllers (EHCI, UHCI, OHCI, xHCI) |
| `storage.mk` | ATA, AHCI, NVMe, SCSI, block devices |
| `input.mk` | PS/2 keyboard/mouse, input events |
| `ipc.mk` | IPC channels, POSIX SHM, SysV IPC |
| `timers.mk` | PIT, HPET, TSC, timer abstraction |
| `interrupts.mk` | APIC, IOAPIC, PIC, interrupt management |
| `security.mk` | Authentication, ASLR, SMEP/SMAP |
| `hardware.mk` | PCI, ACPI, serial, TTY, char devices |
| `scheduler.mk` | Process scheduler, SMP |
| `compat.mk` | Linux compatibility layer |

### Deferred Expansion Trick

Source list variables use recursive (`=`) expansion so feature fragment appends are visible when the variable is finally expanded. `EXCLUDED_CSOURCES` is `:=` (simple) for its base list; feature fragments use `+=` which appends at the point of the `+=`. The final `CSOURCES` value includes all feature exclusions.

---

## Build Outputs and Artifacts

All build outputs are keyed by the config triple `<arch>bit-<mode>-<type>`:

```
fern/
├── obj/32bit-bios-debug/            # Object files (.o)
├── build/32bit-bios-debug/          # Final binaries
│   ├── boot/fern.bin                # BIOS kernel binary
│   └── boot/initrd.tar              # Initial ramdisk
└── build/64bit-uefi-release/
    ├── fern.elf                     # ELF kernel (before PE conversion)
    └── BOOTX64.EFI                  # UEFI PE32+ application

foreboots/build/
├── stage1.bin                       # MBR (512 bytes, exactly)
├── stage2.bin                       # GUI + menu (max 8 KiB)
├── stage3.bin                       # PM ELF loader (max 8 KiB)
├── forebo.img                       # Raw disk image
├── esp.img                          # FAT EFI System Partition (~48 MiB)
└── forebo.iso                       # Hybrid BIOS+UEFI ISO
```

### BIOS Disk Layout

| Sectors | Content |
|---------|---------|
| 0 | Stage 1 (MBR, 512 bytes) |
| 1–16 | Stage 2 (8 KiB max) |
| 17–32 | Stage 3 (8 KiB max) |
| 48+ | Kernel binary |
| Computed | Initrd / multiboot module |

### UEFI ESP Layout

```
esp.img (FAT16, ~48 MiB)
├── /EFI/BOOT/BOOTX64.EFI     # ForeB's UEFI loader
├── /forebo/kernel.elf         # Fern kernel
├── /forebo/forebo.cfg         # Boot menu config
├── /forebo/bg.bmp             # Theme background
├── /forebo/icons/*.tga        # Menu icons
└── /forebo/initrd.tar         # Multiboot module
```

---

## The createos.sh Interactive Build Tool

`createos.sh` is a menu-driven build GUI that orchestrates the entire pipeline.

### Usage

```bash
./createos.sh                        # Interactive TUI
./createos.sh --quick                # Quick build with defaults
./createos.sh --arch 64 --boot uefi --type release
```

### Interactive Menu

1. **Architecture** — 32, 64, arm, aarch64, riscv64
2. **Boot Mode** — BIOS or UEFI
3. **Build Type** — debug or release
4. **Features** — OpenGL, networking, audio, SMP, X11 (toggle on/off)
5. **Initrd Style** — minimal, standard, full, custom
6. **Output Name** — customize image filename
7. **Build** / **Reset** / **Quit**

### Pipeline Steps

When you hit "Build":

1. Install prerequisites (auto-detects apt/pacman/dnf)
2. Build cross-toolchain (if not already built)
3. Build userspace apps (40+ cross-compiled binaries)
4. Build initrd (packs userspace + scripts into `initrd.tar`)
5. Configure kernel (writes `.forestos_config`, runs `conf.sh --generate`)
6. Build kernel (compiles Fern)
7. Build bootloader (ForeB + disk image/ISO)
8. Package output (copies everything to `output/`)

Skip flags: `--skip-toolchain`, `--skip-userspace`.

---

## Build Troubleshooting

**"Architecture toolchain not found"** — Bridge the toolchain path:
```bash
ln -sf ../forestos-toolchain ~/forest/fern/forestos-toolchain
# Or: export FORESTOS_TOOLCHAIN_DIR=$HOME/forest/forestos-toolchain
```

**"build-config.mk not found"** — Generate it:
```bash
cd fern && ./conf.sh --defconfig && ./conf.sh --generate
```

**"x86_64-forestos-gcc not found" (ARCH=64)** — Build the 64-bit toolchain:
```bash
cd forestos-toolchain && ./build-toolchain.sh --arch 64
```

**Stage 2 exceeds 8 KiB** — BIOS stage2 must fit in 8192 bytes. Reduce features or debug the stage2 source.

**"NASM not found"** — ForeB BIOS stages need NASM: `sudo apt install nasm`

**"clang/lld not found"** — ForeB UEFI app uses clang (deliberate toolchain split): `sudo apt install clang lld`

**Host binary in initrd** — If binaries show `/lib64/ld-linux`, they were compiled with the host compiler. Rebuild: `cd userspace && make clean && make`

**QEMU UEFI firmware not found** — OVMF path varies by distro. Check `/usr/share/edk2/` and update `foreboots/Makefile` or set `OVMF_CODE`/`OVMF_VARS`.

**Diagnostic commands:**
```bash
make configcheck     # Show effective config
make show-config     # Full config dump
make list-objects    # Object files to build
make stats           # Build statistics
make size            # Binary size
make validate-toolchain  # Verify toolchain
```

---

## Clean Builds vs Incremental Builds

### Incremental Builds (Default)

Only files whose dependencies changed are recompiled. Object files live in `obj/<arch>bit-<mode>-<type>/` — changing `ARCH`, `BOOT_MODE`, or `BUILD_TYPE` creates a separate directory, so multiple configurations coexist.

### Clean Build

```bash
make clean           # Clean current config only
make clean-all       # Clean ALL configurations
make clean-kernel    # Remove only .o files (fast, keeps deps)
make all CLEAN_BEFORE_BUILD=yes   # Force clean before every build
```

| Target | Removes |
|--------|---------|
| `clean` | `obj/<config>/` and `build/<config>/` for current config |
| `clean-all` | All `obj/`, all `build/*bit-*`, `dist/`, `*.iso` |
| `clean-kernel` | Only `.o` files in current obj dir |

ForeB: `cd foreboots && make clean`

Toolchain: `cd forestos-toolchain && ./build-toolchain.sh --arch both --clean`

### Config-Driven Object Directories

```
obj/32bit-bios-debug/      # x86 32-bit, BIOS, debug symbols
obj/64bit-uefi-release/    # x86 64-bit, UEFI, optimized
obj/arm-bios-debug/        # ARM, BIOS, debug
```

No cross-contamination — build multiple configurations in parallel safely.

---

*See also: [MAKE_AN_OS.md](../../MAKE_AN_OS.md) for the end-to-end build guide.*

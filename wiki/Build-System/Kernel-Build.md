# Forest OS Kernel Build System

A guide to building the Forest OS (Fern) kernel across architectures, boot modes, and build types.

## 1. Prerequisites

### System Packages

```bash
sudo apt install build-essential nasm qemu-system-x86 qemu-system-arm \
    qemu-system-aarch64 qemu-efi-aarch64 dialog libgmp-dev libmpfr-dev \
    libmpc-dev texinfo
```

### Cross-Compiler Toolchain

| Architecture | Required Toolchain |
|-------------|-------------------|
| 32-bit x86 | `forestos-toolchain` (built via `make toolchain`) |
| 64-bit x86 | `forestos-toolchain` (built via `make toolchain`) |
| ARMv7 | `arm-none-eabi-gcc` or `arm-linux-gnueabi-gcc` |
| AArch64 | `aarch64-linux-gnu-gcc` |
| RISC-V 64 | `riscv64-unknown-elf-gcc` |

Build or validate the Forest OS toolchain:

```bash
make toolchain            # builds for current ARCH (32 or 64)
make toolchain ARCH=64    # force 64-bit
make validate-toolchain   # check existing installation
```

ForeB (the Forest OS bootloader at `../foreboots/`) is built automatically when `ENABLE_FOREB_BOOTLOADER=yes` (default).

---

## 2. Configuration

Configuration flows through two files:

```
.forestos_config  --(conf.sh)-->  build-config.mk  --(Make)-->  build vars
```

### Quick Start

```bash
make defconfig          # write sane defaults
make menuconfig         # interactive TUI (requires 'dialog')
make configcheck        # validate + show effective config
make show-config        # full configuration dump
```

### conf.sh Modes

```bash
./conf.sh --defconfig       # sane defaults
./conf.sh --menuconfig      # interactive TUI
./conf.sh --oldconfig       # re-validate existing config
./conf.sh --allnoconfig     # all features off (except required-on)
./conf.sh --allyesconfig    # all features on
./conf.sh --generate        # regenerate build-config.mk from .forestos_config
```

### TUI Categories

The menuconfig TUI covers: General Setup (ARCH, BOOT_MODE, BUILD_TYPE), Architecture/Boot (SMP, FPU, ForeB), Memory Management (Paging, SLAB, COW, Swap), Filesystems (VFS, EXT2, FAT32, ProcFS), Graphics (Framebuffer, Drivers, Compositor), Networking, Audio, Security, USB, Storage, Input, IPC, Timers, Debug, Hardware, Interrupts, Scheduler, Build System, Compatibility, and Numeric Tunables.

### Required-On Options

These are always forced `yes`, even under `--allnoconfig`: `ENABLE_PAGING`, `ENABLE_A20`, `ENABLE_TTY`.

### CLI Overrides

Override any value for a single run:

```bash
make ARCH=64 BOOT_MODE=uefi BUILD_TYPE=release
```

---

## 3. Build Commands

### Core Targets

| Target | Description |
|--------|-------------|
| `make build` | Compile kernel binary only |
| `make all` | Build kernel + bootable image (iso or img) |
| `make iso` | Create bootable image via ForeB |
| `make img` | Create disk image via ForeB |
| `make clean` | Remove artifacts for current config |
| `make clean-all` | Remove all build artifacts |

### Build Flow

`make all` runs: `maybe-clean-before-build` -> `ensure-toolchain` -> `show-config` -> `build` -> `iso`/`img`. Use `make stats`, `make size`, `make list-objects`, `make info` for inspection.

---

## 4. Architecture-Specific Builds

### x86

```bash
make build32          # 32-bit kernel (i686)
make build64          # 64-bit kernel (x86_64)
make build32-bios     # 32-bit + BIOS
make build32-uefi     # 32-bit + UEFI
make build64-bios     # 64-bit + BIOS
make build64-uefi     # 64-bit + UEFI
```

Toolchain: `i686-forestos-gcc` / `x86_64-forestos-gcc` from `forestos-toolchain`.

### ARM/AArch64/RISC-V

```bash
make buildarm              # ARMv7
make buildaarch64          # AArch64 (BIOS)
make buildaarch64-uefi     # AArch64 (UEFI)
make buildriscv64          # RISC-V 64 (BIOS)
make buildriscv64-uefi     # RISC-V 64 (UEFI)
```

These use system cross-compilers (auto-detected). No NASM; GNU as handles `.S` files.

### Build Everything

```bash
make buildall   # all arch/boot combinations
```

### Target Tuples

| ARCH | Tuple | EFI Arch | QEMU Binary |
|------|-------|----------|-------------|
| 32 | `i686-forestos` | i386 | `qemu-system-i386` |
| 64 | `x86_64-forestos` | x86_64 | `qemu-system-x86_64` |
| arm | `arm-none-eabi` | arm | `qemu-system-arm` |
| aarch64 | `aarch64-linux-gnu` | aarch64 | `qemu-system-aarch64` |
| riscv64 | `riscv64-unknown-elf` | riscv64 | `qemu-system-riscv64` |

---

## 5. The Compilation Process

### Object Directory Structure

```
obj/$(ARCH_DIR_SUFFIX)-$(BOOT_MODE)-$(BUILD_TYPE)/
  boot.o, *.o, graphics/*.o, gl/*.o, input/*.o, fs/*.o, uacpi_*.o
```

### Common C Flags

All `.c` files compile with:

```
$(ARCH_FLAGS) -ffreestanding -nostdlib -fno-pic -fno-pie
-Wall -Wextra -fcf-protection=none
-Isrc/include -Isrc -Ilibs/uacpi/include -Ilibs/qrcodegen
```

When `WERROR=yes`, `-Werror` is appended. All enabled features become `-D` defines (e.g., `-DENABLE_PAGING`). Numeric tunables are also passed as defines (e.g., `-DVFS_MAX_PATH=256`).

### Architecture-Specific Flags

- **32-bit**: `-m32 -march=i386 -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mfpmath=387`
- **64-bit**: `-m64 -march=x86-64 -mcmodel=kernel -mno-red-zone -mno-sse -msoft-float`
- **ARM**: `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp`
- **AArch64**: `-march=armv8-a`
- **RISC-V**: `-march=rv64gc -mabi=lp64d -mcmodel=medany`

### Build Type Flags

| Type | Optimization | Debug |
|------|-------------|-------|
| debug | `-g -O0` | Yes |
| release | `-O0`, linker: `--gc-sections -s` | No |
| optimize | `-Os`, linker: `-O3 --gc-sections -s -flto` | No |

### Interrupt Handling

Files `interrupt.c` and `interrupt_handlers.c` compile with `-mgeneral-regs-only` (x86 only) to disable SIMD in interrupt context.

### Assembly

- **x86**: NASM (`-f elf32` / `-f elf64`)
- **ARM/AArch64/RISC-V**: GNU as via `$(CC) -c`

### Linking

```
$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)
```

`LDFLAGS` includes `-T $(LINKER_SCRIPT) --allow-multiple-definition`. `LIBGCC` provides soft-float helpers (`__muldf3`, etc.).

### Linker Scripts

| ARCH | BIOS | UEFI |
|------|------|------|
| 32 | `src/link.ld` | `src/link_uefi_32.ld` |
| 64 | `src/link64.ld` | `src/link_uefi_64.ld` |
| arm | `src/arm32/link.ld` | `src/link_uefi_arm.ld` |
| aarch64 | `src/aarch64/link.ld` | `src/link_uefi_aarch64.ld` |
| riscv64 | `src/riscv64/link.ld` | `src/riscv64/link_uefi.ld` |

---

## 6. Kernel ELF Output

**BIOS**: flat ELF at `build/$(CONFIG)/boot/fern.bin`

**UEFI**: intermediate ELF at `build/$(CONFIG)/fern.elf`, converted to PE32+ via `objcopy --target=efi-app-$(EFI_ARCH)` producing `BOOTX64.EFI` (or `BOOTAA64.EFI`).

---

## 7. QEMU Testing

### Run Targets

```bash
make run         # current ARCH/BOOT_MODE
make run-bios    # force BIOS boot
make run-uefi    # force UEFI boot
make run32       # QEMU i386
make run64       # QEMU x86_64 (q35)
make runarm      # QEMU ARM (cortex-a15)
make runaarch64  # QEMU AArch64 (cortex-a53)
```

### QEMU Options

Configurable via `build-config.mk`:

| Option | Default | Description |
|--------|---------|-------------|
| `QEMU_MEMORY` | 512 | Guest RAM in MB |
| `QEMU_ENABLE_KVM` | yes | KVM acceleration (auto-disabled if `/dev/kvm` missing) |
| `QEMU_NETWORK` | no | RTL8139 NIC emulation |
| `QEMU_USB` | no | EHCI USB controller |

### Debug Mode

```bash
make debug   # QEMU with GDB stub (-s -S), halted at entry
```

Connect: `gdb -ex "target remote :1234" -ex "symbol-file obj/.../fern.bin"`

### Boot Test

With `ENABLE_TESTING=yes`:

```bash
make test-boot   # 60s timeout, checks serial for "Forest" signature
```

---

## 8. ISO/IMG Creation

Forest OS uses **ForeB** (no GRUB). ForeB builds stage1+stage2 and embeds the kernel into a raw disk image.

| Target | Description |
|--------|-------------|
| `make forebo` | Build ForeB binaries |
| `make forebo-image` | Build ForeB + embed kernel |
| `make forebo-qemu` | Build + run in QEMU |
| `make forebo-check` | Verify MBR signature |

The initrd is packaged from `initrd/` into `build/$(CONFIG)/boot/initrd.tar` and embedded by ForeB. Run `make dist` to build all arches and create a tarball in `dist/`.

---

## 9. Build Optimization

### Parallel Builds

```bash
make -j$(nproc)              # use all cores
make PARALLEL_JOBS=4         # or set in build-config.mk
```

### Feature Gating

Disabling unused features reduces kernel size. Feature fragments in `build/features/*.mk` append to `EXCLUDED_CSOURCES` when features are off.

```bash
./conf.sh --allnoconfig      # minimal kernel
make menuconfig              # enable only what you need
./conf.sh --generate
make
```

### Clean Before Build

Set `CLEAN_BEFORE_BUILD=yes` in `build-config.mk` to auto-clean before every build.

---

## 10. Debug Builds

Enable in menuconfig (Debugging category):

```
ENABLE_DEBUG_SYMBOLS    = y    # -g flag
ENABLE_KERNEL_DEBUG     = y    # debug facilities
ENABLE_SERIAL_DEBUG     = y    # serial output
ENABLE_PANIC_BACKTRACES = y    # stack traces on panic
ENABLE_ASSERTIONS       = y    # runtime assertions
ENABLE_MEMORY_DEBUG     = y    # leak detection
DEBUG_LOG_LEVEL         = 5    # max verbosity
VERBOSE                 = y    # show full compiler commands
```

Build:

```bash
make BUILD_TYPE=debug
```

---

## 11. Common Build Errors and Fixes

### "build-config.mk not found"

Run `make defconfig` or `./conf.sh --defconfig` first.

### "Toolchain directory not found"

Build the toolchain: `make toolchain`, or set `FORESTOS_TOOLCHAIN_DIR=/path/to/toolchain`.

### "Cross-compiler not found" (ARM/AArch64/RISC-V)

Install system cross-compiler: `sudo apt install gcc-arm-none-eabi` / `gcc-aarch64-linux-gnu` / `gcc-riscv64-linux-gnu`.

### "NASM assembler not found"

`sudo apt install nasm` (x86/x86_64 only).

### "dialog not found"

`sudo apt install dialog` (for `make menuconfig` only; `--defconfig` works without it).

### Undefined References (e.g., `__muldf3`)

The kernel links `libgcc.a` for soft-float. Ensure your cross-compiler provides it: `$(CC) -print-libgcc-file-name`.

### Feature Dependency Warnings

`ENABLE_EXT2 forced to 'n' because parent ENABLE_VFS is 'n'` - expected behavior. Enable the parent feature first.

### "ForeB bootloader is disabled"

Set `ENABLE_FOREB_BOOTLOADER=y` in menuconfig, then `./conf.sh --generate`.

### Invalid ARCH/BOOT_MODE/BUILD_TYPE

Valid values: ARCH=`32 64 arm aarch64 riscv64`, BOOT_MODE=`bios uefi`, BUILD_TYPE=`debug release optimize`.

---

## Appendix: Configuration Files

### .forestos_config (human-readable)

```
CONFIG_ENABLE_PAGING=y
CONFIG_BUILD_TYPE=debug
```

### build-config.mk (Make-consumable, generated)

```makefile
ARCH := 32
BOOT_MODE := bios
BUILD_TYPE := debug
ENABLE_PAGING := yes
FEATURE_FLAGS := -DENABLE_PAGING -DENABLE_VFS ...
OPTIMIZATION_LEVEL := 0
DEBUG_FLAGS := -g -DDEBUG
```

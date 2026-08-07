# Interactive Build Tool

`createos.sh` is Forest OS's menu-driven GUI build tool. It guides you through configuring and building your entire OS — from toolchain to kernel to bootable disk image — using `dialog` menus. No memorizing flags, no typos.

---

## Prerequisites

The tool auto-installs missing packages via your system package manager (`apt`, `pacman`, or `dnf`).

| Package | Purpose |
|---------|---------|
| `dialog` | Interactive menu system |
| `make` | Build orchestration |
| `gcc` / `g++` | C/C++ compilation |
| `nasm` | x86 assembly |
| `clang` / `lld` | UEFI kernel compilation and linking |
| `xorriso` | ISO image creation |
| `mtools` | FAT filesystem manipulation |
| `python3` | Build scripts |

---

## How to Run

```bash
./createos.sh              # Interactive GUI mode (default)
./createos.sh --quick      # Quick build with defaults (no menus)
./createos.sh --help       # Show all flags
```

Non-interactive examples:

```bash
./createos.sh --arch 64 --boot uefi --type release
./createos.sh --arch arm --no-opengl --initrd full
```

---

## Build Workflow

```
Install Prerequisites
        │
Interactive Menus (configure build)
        │
Build Cross-Toolchain
        │
Build Userspace Apps (40 programs)
        │
Build Initrd (initial ramdisk)
        │
Configure Kernel
        │
Build Kernel
        │
Build Bootloader (ForeB + disk images)
        │
Package Output (output/)
```

Each step can be interrupted with `Ctrl+C` and resumed on the next run.

---

## Architecture Selection

```
┌──────────────────────────────────────────────────────┐
│                    Architecture                      │
│                                                      │
│   ( ) 32      x86 32-bit (i686) — most compatible   │
│   ( ) 64      x86 64-bit (x86_64) — modern PCs     │
│   ( ) arm     ARM 32-bit (Raspberry Pi)             │
│   ( ) aarch64 AArch64 64-bit (RPi 4+)              │
│   ( ) riscv64 RISC-V 64-bit (SiFive, QEMU virt)    │
│                                                      │
│              <OK>           <Cancel>                 │
└──────────────────────────────────────────────────────┘
```

| Arch | Use Case |
|------|----------|
| `32` | QEMU testing, legacy hardware (default) |
| `64` | Modern desktops, 64-bit only systems |
| `arm` | Raspberry Pi Zero/1/2/3 |
| `aarch64` | Raspberry Pi 4+, modern ARM SBCs |
| `riscv64` | SiFive boards, QEMU virt (experimental) |

---

## Boot Mode Selection

```
┌──────────────────────────────────────────────────────┐
│                      Boot Mode                       │
│                                                      │
│   ( ) bios  BIOS (Legacy MBR) — widest support      │
│   ( ) uefi  UEFI (EFI System Partition) — modern    │
│                                                      │
│              <OK>           <Cancel>                 │
└──────────────────────────────────────────────────────┘
```

| Mode | Outputs | Notes |
|------|---------|-------|
| `bios` | `forebo.img`, `forebo.iso` | Works on almost everything |
| `uefi` | `esp.img`, `BOOTX64.EFI` | Needs OVMF for QEMU testing |

---

## Build Type Selection

```
┌──────────────────────────────────────────────────────┐
│                     Build Type                       │
│                                                      │
│   ( ) debug    Symbols, no optimization, verbose     │
│   ( ) release  Optimized, no debug, smaller binary   │
│                                                      │
│              <OK>           <Cancel>                 │
└──────────────────────────────────────────────────────┘
```

Use `debug` during development, `release` for production.

---

## Feature Selection

Toggle optional subsystems. Use Space to select, Enter to confirm.

```
┌──────────────────────────────────────────────────────┐
│                       Features                       │
│                                                      │
│   [X] opengl      OpenGL 1.1 software renderer      │
│   [X] networking   TCP/IP stack + drivers            │
│   [ ] audio        PC speaker, HDA, virtio-snd       │
│   [ ] smp          Symmetric multiprocessing         │
│   [X] x11          X11 display server                │
│                                                      │
│              <OK>           <Cancel>                 │
└──────────────────────────────────────────────────────┘
```

Disabling unused features reduces kernel size and boot time.

---

## Initrd Style Selection

The initrd is what the kernel loads at boot. Choose what goes inside it.

```
┌──────────────────────────────────────────────────────┐
│                    Initrd Content                    │
│                                                      │
│   ( ) minimal   Shell scripts only (~100 KB)         │
│   ( ) standard  Compiled apps + libs (~2 MB)         │
│   ( ) full      Apps + libs + fonts + icons (~5 MB)  │
│   ( ) custom    Pick individual programs             │
│                                                      │
│              <OK>           <Cancel>                 │
└──────────────────────────────────────────────────────┘
```

Choosing `custom` opens a second dialog listing all compiled binaries for individual selection.

---

## Output Configuration

Set a custom name for your build artifacts. Output always goes to `output/`.

```
output/
├── kernel/fern.bin        # BIOS kernel binary
├── kernel/fern.elf        # ELF kernel (UEFI)
├── kernel/BOOTX64.EFI     # UEFI application
├── initrd/initrd.tar      # Initial ramdisk
├── forebo.img             # BIOS disk image
├── esp.img                # EFI System Partition
├── forebo.iso             # Hybrid ISO
└── README.txt             # Build info + QEMU commands
```

---

## Main Menu

```
╔══════════════════════════════════════════════════════╗
║               Forest OS Builder                      ║
║                                                      ║
║   1  Architecture    [ 32 ]                          ║
║   2  Boot Mode       [ bios ]                        ║
║   3  Build Type      [ debug ]                       ║
║   4  Features        [ GL NET X11 ]                  ║
║   5  Initrd          [ standard ]                    ║
║   6  Output Name     [ forestos ]                    ║
║                                                      ║
║   s  >> Show Summary <<                              ║
║   b  >> BUILD <<                                     ║
║   r  >> Reset to defaults <<                         ║
║   q  >> Quit <<                                      ║
╚══════════════════════════════════════════════════════╝
```

The menu redraws after every change so you always see current settings. Press `s` for a full summary before building.

---

## Tips and Tricks

- **Quick iteration:** `--quick` skips menus entirely — great during development
- **Skip steps:** `--skip-toolchain --skip-userspace` avoids rebuilding unchanged parts
- **Minimal server:** `--no-opengl --no-networking --no-audio --no-x11` strips the kernel to essentials
- **Build log:** All output is captured to `output/build.log` — check there on failure
- **Parallel builds:** Userspace uses `-j$(nproc)` automatically

---

## Troubleshooting

**"dialog: command not found"** — Install it: `sudo apt install dialog` (or `pacman -S dialog`)

**"Toolchain source not found"** — Verify `forestos-toolchain/build-toolchain.sh` exists in the Forest root

**"HOST BINARY detected"** — Your userspace apps were compiled for Linux, not Forest OS. Run `cd userspace && make clean && make`

**Build fails at kernel** — Check `output/build.log`. Ensure toolchain is built (`--skip-toolchain` removed) and `nasm` is installed

**UEFI build fails** — Verify `clang` and `lld` are installed

**QEMU won't boot:**
```bash
# BIOS
qemu-system-i386 -drive format=raw,file=output/forebo.img -serial stdio -vga std

# UEFI
qemu-system-x86_64 -drive format=raw,file=output/esp.img -bios /usr/share/ovmf/OVMF.fd
```

---

## Command-Line Reference

| Flag | Values | Description |
|------|--------|-------------|
| `--quick` | — | Build with defaults, skip menus |
| `--arch` | `32`, `64`, `arm`, `aarch64`, `riscv64` | Target CPU |
| `--boot` | `bios`, `uefi` | Firmware interface |
| `--type` | `debug`, `release` | Optimization level |
| `--output` | name | Output image prefix |
| `--initrd` | `minimal`, `standard`, `full`, `custom` | Initrd content |
| `--no-opengl` | — | Disable OpenGL |
| `--no-networking` | — | Disable network stack |
| `--no-audio` | — | Disable audio |
| `--no-x11` | — | Disable X11 |
| `--smp` | — | Enable multi-core |
| `--skip-toolchain` | — | Skip toolchain build |
| `--skip-userspace` | — | Skip userspace build |

---

## Related Pages

- [Build System Overview](Overview.md)
- [Toolchain Setup](Toolchain.md)
- [Kernel Build](Kernel-Build.md)

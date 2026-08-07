# Testing Forest OS with QEMU

## Table of Contents

1. [Testing with QEMU](#testing-with-qemu)
2. [QEMU Prerequisites](#qemu-prerequisites)
3. [Running in QEMU (make run)](#running-in-qemu-make-run)
4. [BIOS vs UEFI Testing](#bios-vs-uefi-testing)
5. [Debugging with QEMU](#debugging-with-qemu)
6. [Testing Different Architectures](#testing-different-architectures)
7. [Testing Graphics](#testing-graphics)
8. [Testing Networking](#testing-networking)
9. [Testing USB](#testing-usb)
10. [Automated Testing](#automated-testing)
11. [Manual Testing Checklist](#manual-testing-checklist)
12. [Performance Testing](#performance-testing)
13. [Bug Reporting](#bug-reporting)

---

## Testing with QEMU

QEMU is Forest OS's primary testing environment. Two ways to launch:

- **`make run`** — Defined in `build/qemu-run.mk`. Builds ForeB bootloader + kernel, then launches QEMU.
- **`./run.sh`** — Standalone script with extra options (`--gdb`, `--monitor`, `--dry-run`, `--sound`).

Both respect `build-config.mk` and allow CLI overrides.

---

## QEMU Prerequisites

```bash
# Install QEMU and NASM
sudo apt install qemu-system-x86 qemu-system-arm qemu-system-aarch64 nasm

# KVM (highly recommended for performance)
ls -la /dev/kvm
sudo usermod -aG kvm $USER     # re-login after

# UEFI firmware (for UEFI testing)
sudo apt install ovmf          # Provides /usr/share/ovmf/OVMF.fd
```

KVM is auto-enabled when `/dev/kvm` exists and `QEMU_ENABLE_KVM=yes` (default).

---

## Running in QEMU (make run)

```bash
cd forest/fern
make run                # Build and run with current config
make run-bios           # Force BIOS boot
make run-uefi           # Force UEFI boot
make run32              # 32-bit BIOS
make run64              # 64-bit BIOS
make runarm             # ARM32 (virt/cortex-a15)
make runaarch64         # AArch64 (virt/cortex-a53)
```

`make run` builds ForeB + disk image, selects the correct QEMU binary, and launches with serial on stdio.

### run.sh Options

```bash
./run.sh                         # Default test
./run.sh --bios -a 32            # 32-bit BIOS
./run.sh --uefi -m 1024          # UEFI with 1GB RAM
./run.sh --debug                 # Debug output on serial
./run.sh --dry-run               # Show QEMU command without running
./run.sh --gdb                   # GDB debugging stub
./run.sh --monitor               # QEMU monitor on stdio
./run.sh --sound hda             # Specific sound device
./run.sh --serial /tmp/serial.log  # Save serial to file
```

### Configuration Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `QEMU_MEMORY` | `512` | Guest memory in MB |
| `QEMU_ENABLE_KVM` | `yes` | KVM acceleration |
| `QEMU_NETWORK` | `yes` | Network device |
| `QEMU_USB` | `yes` | USB controller |
| `ENABLE_TESTING` | `no` | Automated boot test |

Override: `make QEMU_MEMORY=1024 QEMU_NETWORK=no run`

---

## BIOS vs UEFI Testing

### BIOS

Uses ForeB raw-MBR disk image (`forebo.img`), kernel at sector 48:

```bash
make BOOT_MODE=bios run
# qemu-system-x86_64 -machine q35 -cpu qemu64 -m 512M -enable-kvm \
#   -drive format=raw,file=foreboots/build/forebo.img -serial stdio -vga std
```

### UEFI

Uses EFI System Partition (`esp.img`) with OVMF firmware:

```bash
make BOOT_MODE=uefi run
# qemu-system-x86_64 -machine q35 -cpu qemu64 -m 512M -enable-kvm \
#   -bios /usr/share/ovmf/OVMF.fd -drive format=raw,file=foreboots/build/esp.img \
#   -serial stdio -vga std
```

### ESP Layout

```
esp.img (FAT16, ~48 MiB)
├── /EFI/BOOT/BOOTX64.EFI   # ForeB UEFI loader
├── /forebo/kernel.elf       # Fern kernel
├── /forebo/forebo.cfg       # Boot menu config
├── /forebo/initrd.tar       # Initial ramdisk
```

---

## Debugging with QEMU

### Serial Console

```bash
make run                                    # Serial on stdio (default)
./run.sh --serial /tmp/serial.log           # Save to file
./run.sh --debug                            # Debug mode with serial
```

### GDB Debugging

```bash
./run.sh --gdb        # Starts QEMU with -gdb tcp::1234 -S (paused)
make debug            # Same via make

# In another terminal:
gdb
(gdb) target remote localhost:1234
(gdb) symbol-file build/64bit-debug/fern.elf
(gdb) break kernel_main
(gdb) continue
(gdb) bt              # Backtrace
(gdb) info registers
```

### QEMU Monitor

```bash
./run.sh --monitor
# Commands: info registers, info mem, info block, info network,
# info usb, screendump file.ppm, quit
```

### Debug Build

```bash
make BUILD_TYPE=debug run    # Includes -g debug symbols
```

---

## Testing Different Architectures

| Arch | QEMU Binary | Machine/CPU | Boot Method |
|------|------------|-------------|-------------|
| `ARCH=32` | `qemu-system-i386` | `-cpu qemu32` | ForeB disk image |
| `ARCH=64` | `qemu-system-x86_64` | `-machine q35 -cpu qemu64` | ForeB disk image |
| `ARCH=arm` | `qemu-system-arm` | `-machine virt -cpu cortex-a15` | `-kernel <binary>` |
| `ARCH=aarch64` | `qemu-system-aarch64` | `-machine virt -cpu cortex-a53` | `-kernel <binary>` |
| `ARCH=riscv64` | `qemu-system-riscv64` | (defaults) | `-kernel <binary>` |

ARM/AArch64/RISC-V boot with `-kernel` directly (no ForeB), serial-only by default.

```bash
make buildall    # Build all arch combinations
```

---

## Testing Graphics

x86 defaults to `-vga std` (framebuffer). ARM uses `-nographic`.

```bash
./run.sh                      # Graphics (default for x86)
./run.sh --nographic          # Serial only

# Canopy DE: -device VGA,vgamem_mb=128
```

### Screenshot Capture

```bash
# From QEMU monitor (--monitor flag)
screendump /tmp/screenshot.ppm
convert /tmp/screenshot.ppm /tmp/screenshot.png
```

Config flags: `make menuconfig` -> Graphics (VGA Text, VESA, Bochs BGA, VMware SVGA, Intel HD, Framebuffer, Wayland, X11, Window Manager, Fonts).

---

## Testing Networking

Enabled when `QEMU_NETWORK=yes` and `ENABLE_NETWORKING=yes`:

```bash
-netdev user,id=net0 -device rtl8139,netdev=net0
make QEMU_NETWORK=no run   # Disable
```

| Driver | QEMU Device | Config |
|--------|------------|--------|
| RTL8139 | `-device rtl8139` | `ENABLE_DRIVER_RTL8139=yes` |
| E1000 | `-device e1000` | `ENABLE_DRIVER_E1000=yes` |
| NE2000 | `-device ne2k_pci` | `ENABLE_DRIVER_NE2000=yes` |

Port forwarding: `-netdev user,id=net0,hostfwd=tcp::8080-:80`

---

## Testing USB

USB enabled by default with EHCI (`-device usb-ehci`).

```bash
make QEMU_USB=no run   # Disable
```

Config flags: Enable USB, EHCI, UHCI, OHCI, xHCI, USB HID, USB Hub, USB Mass Storage.

---

## Automated Testing

### Boot Test

```bash
make menuconfig   # Process -> Enable Testing = yes
make test-boot    # 60s timeout, scans for "Forest" in serial output
```

Custom signature: `make QEMU_BOOT_SIGNATURE="Forest OS" test-boot`
Custom log: `make QEMU_TEST_LOG=/tmp/test.log test-boot`

### CI Script

```bash
#!/bin/bash
cd forest/fern && make defconfig && make all && make test-boot
```

---

## Manual Testing Checklist

**Boot:** BIOS boot, UEFI boot, boot menu interactive, kernel loads, shell prompt appears.

**Input:** Serial console, PS/2 keyboard/mouse, USB keyboard/mouse (if enabled).

**Filesystem:** VFS initializes, FAT32/ext2/ISO9660 mount, files read/write.

**Networking:** NIC initializes, DHCP, DNS, TCP, UDP (if enabled).

**Graphics:** Framebuffer/VGA, fonts render, display/window manager (if enabled).

**USB:** Controller initializes, devices enumerate, HID and mass storage (if enabled).

**Stress:** 64MB memory, 2GB memory, multiple processes, I/O under load.

---

## Performance Testing

```bash
make QEMU_MEMORY=64 run       # Minimal memory
make QEMU_MEMORY=2048 run     # Large memory
make QEMU_ENABLE_KVM=no run   # No KVM (for comparison)

make size                     # Binary size
make stats                    # Source/object counts

./run.sh --serial /tmp/boot-time.log   # Boot timing
grep -i "time\|ms" /tmp/boot-time.log
```

Profiling: `make menuconfig` -> Memory -> Enable Memory Debug/Stats, Scheduler -> Enable Profiling.

---

## Bug Reporting

### Include in Reports

1. `make show-config` output
2. `./run.sh --dry-run` (exact QEMU command)
3. Serial output: `./run.sh --serial /tmp/bug.log`
4. Steps to reproduce
5. Expected vs actual behavior

### Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| No boot | Missing ForeB | `make` to build |
| Black screen | Graphics off | `ENABLE_GRAPHICS=yes` |
| No network | NIC not configured | `ENABLE_NETWORKING=yes` |
| Slow perf | No KVM | Check `/dev/kvm` |
| OVMF not found | Wrong path | Check `/usr/share/ovmf/` |

### Kernel Panic

```bash
make menuconfig   # Security -> Enable Panic Backtraces
make BUILD_TYPE=debug run
gdb build/64bit-debug/fern.elf -ex "bt"
```

---

*See also: [Build System Overview](../Build-System/Overview.md) | [Kernel Configuration](../Kernel/Kernel-Configuration.md)*

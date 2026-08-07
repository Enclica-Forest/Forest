# Forest-OS Troubleshooting Guide

Solutions for common build, boot, and runtime issues.

---

## Table of Contents

1. [Build Issues](#1-build-issues)
2. [Boot Issues](#2-boot-issues)
3. [Kernel Panics and Crashes](#3-kernel-panics-and-crashes)
4. [Memory Issues](#4-memory-issues)
5. [Filesystem Issues](#5-filesystem-issues)
6. [Networking Issues](#6-networking-issues)
7. [Graphics Issues](#7-graphics-issues)
8. [USB Issues](#8-usb-issues)
9. [Performance Issues](#9-performance-issues)
10. [Development Environment Issues](#10-development-environment-issues)
11. [QEMU-Specific Issues](#11-qemu-specific-issues)
12. [Hardware Compatibility Issues](#12-hardware-compatibility-issues)
13. [Debug Techniques and Tools](#13-debug-techniques-and-tools)

---

## 1. Build Issues

### 1.1 Toolchain Problems

**`Architecture toolchain not found: .../install`**

The Makefile looks in `fern/forestos-toolchain/` but the toolchain lives at
`$FOREST/forestos-toolchain/`. Fix with one of:

```bash
export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain
# OR create the symlink:
ln -s ../forestos-toolchain $FOREST/fern/forestos-toolchain
```

**64-bit build silently uses the host compiler**

If `make ARCH=64 show-config` says "using host x86_64 toolchain", the
64-bit cross-compiler is missing. Rebuild: `./build-toolchain.sh --arch 64`.

**`fatal error: stdio.h: No such file or directory`**

The sysroot skeleton was not populated. Rebuild the toolchain:

```bash
cd $FOREST/forestos-toolchain && ./build-toolchain.sh --arch both
ls sysroot/usr/include/stdio.h sysroot/usr/include/forestos/syscalls.h
```

**Toolchain build fails with missing dependencies**

```bash
sudo apt install build-essential gcc g++ make flex bison gawk \
    texinfo curl wget tar xz-utils libgmp-dev libmpfr-dev libmpc-dev
# Or skip the probe on non-Debian:
./build-toolchain.sh --arch both --skip-deps
```

### 1.2 Kernel Build Failures

**`make` prints help and does nothing**

The default goal is `help`. Use an explicit target: `make all`, `make build`,
`make iso`, or `make run`.

**`ForeB bootloader is disabled`**

`ENABLE_FOREB_BOOTLOADER` is not `yes`. Fix:

```bash
cd $FOREST/fern
./conf.sh --defconfig   # or --menuconfig to set it manually
./conf.sh --generate
```

**`build-config.mk not found` warning**

Run `./conf.sh --defconfig && ./conf.sh --generate` in the fern directory.

**Linker error: `cannot find -lgcc`**

The cross-compiler cannot locate `libgcc.a`. Rebuild the toolchain with
`--clean`. Verify: `i686-forestos-gcc -print-libgcc-file-name` should output
an absolute path.

**NASM not found**

ForeB BIOS stages require NASM: `sudo apt install nasm`.

**UEFI link fails / `ld.lld` not found**

The UEFI app uses clang + ld.lld: `sudo apt install clang lld`.

### 1.3 Userspace Build Failures

**Programs segfault at boot**

All binaries must be compiled with `i686-forestos-gcc`, not the host GCC.
Verify: `file initrd/bin/ls` should say "ELF 32-bit LSB executable, Intel
80386". Host-compiled binaries use a different ABI and crash.

**Shell can't find commands**

Ensure apps are in `initrd/bin/` and the shell exists at `/bin/sh`:

```bash
cp $FOREST/userspace/build/bin/* $FOREST/fern/initrd/bin/
ln -sf forest-shell $FOREST/fern/initrd/bin/sh
chmod +x $FOREST/fern/initrd/bin/sh
```

**Kernel doesn't start shell**

The session manager looks for `/bin/shell`. Create the symlink:

```bash
ln -sf forest-shell $FOREST/fern/initrd/bin/shell
```

### 1.4 Bootloader Build Failures

**Stage sizes exceed limits**

Stage1 must be exactly 512 bytes; stages 2/3 must be <= 8192 bytes.
Check: `make forebo-check`.

**OVMF firmware path mismatch (UEFI)**

Two different OVMF paths are used by different parts of the build. Install
the correct package:

| Distro | Package |
|--------|---------|
| Debian/Ubuntu | `sudo apt install ovmf` |
| Arch | `sudo pacman -S edk2-ovmf` |
| Fedora | `sudo dnf install edk2-ovmf` |

Verify paths exist: `ls /usr/share/ovmf/OVMF.fd` and
`ls /usr/share/edk2/x64/OVMF_CODE.4m.fd`.

---

## 2. Boot Issues

### 2.1 BIOS Boot Problems

**Blank screen after BIOS boot**

Check the disk image exists (`foreboots/build/forebo.img`), the MBR has the
0x55AA signature, and the kernel ELF is at sector 48:

```bash
xxd -s 510 -l 2 foreboots/build/forebo.img      # should show 55aa
xxd -s $((48*512)) -l 4 foreboots/build/forebo.img  # should show 7f454c46 (ELF)
```

### 2.2 UEFI Boot Problems

**QEMU hangs at firmware screen**

Ensure OVMF variables file is writable. The build copies it to
`foreboots/build/OVMF_VARS.local.fd`. If missing, rebuild: `make forebo-image`.

**`BOOTX64.EFI` not found on ESP**

Verify: `mdir -i foreboots/build/esp.img ::/EFI/BOOT/`.

### 2.3 QEMU Boot Problems

**QEMU exits immediately**

Verify the disk image exists: `ls -la foreboots/build/forebo.img`.
If missing: `cd $FOREST/fern && make forebo-image`. Also ensure
`qemu-system-i386` is installed: `sudo apt install qemu-system-x86`.

---

## 3. Kernel Panics and Crashes

**`Kernel heap allocation failed`**

Heap exhausted. Default limits: 4 MiB initial, 128 MiB max. Increase QEMU
memory or the heap limit:

```bash
make run QEMU_MEMORY=1024
# Or in config: increase KERNEL_HEAP_MAX_SIZE
```

**Page fault at address 0x...**

Null pointer dereference or use-after-free. Enable debug features:

```bash
./conf.sh --menuconfig
# ENABLE_MEMORY_PROTECTION=yes, ENABLE_MEMORY_DEBUG=yes, ENABLE_GUARD_PAGES=yes
```

**Triple fault**

Exception during exception handling. Causes: corrupt IDT or stack overflow.
Increase kernel stack: `./conf.sh --menuconfig` -> `KERNEL_STACK_SIZE=32768`.

**Kernel hangs without output**

Use debug mode with GDB: `make debug`, then connect with
`gdb -ex "target remote :1234" -ex "break kmain" -ex "continue"`. Ensure
serial is enabled in config.

---

## 4. Memory Issues

**OOM kills**

The OOM killer runs when `ENABLE_OOM_KILLER=yes`. Increase memory or reduce
features. Check serial output for OOM messages.

**Memory corruption detected**

With `ENABLE_MEMORY_CORRUPTION_DETECTION=yes`, corruption is reported.
Common causes: buffer overflows, use-after-free, double-free. Enable
`DEBUG_LOG_LEVEL=5` for detail.

---

## 5. Filesystem Issues

**Mount fails with "unknown filesystem type"**

The filesystem driver may not be enabled. Check: `make show-config | grep -i fs`.
Enable the needed driver (e.g., `ENABLE_FAT32=yes`, `ENABLE_EXT2=yes`).

**initrd too large for BIOS boot**

BIOS initrd limit is ~14 MiB. Check: `ls -lh initrd.tar`. Remove large files
(fonts, debug symbols, unused programs). Recommended max: ~8 MiB.

---

## 6. Networking Issues

**No network interface detected**

Enable in config: `ENABLE_NETWORKING=yes` and a driver (e.g.,
`ENABLE_DRIVER_RTL8139=yes`). QEMU needs: `make run QEMU_NETWORK=yes`
(adds `-netdev user,id=net0 -device rtl8139,netdev=net0`).

**DHCP/DNS fails**

Ensure `ENABLE_DHCP=yes`, `ENABLE_DNS=yes`, `ENABLE_ARP=yes` in config.

---

## 7. Graphics Issues

**No display in QEMU**

Ensure `ENABLE_GRAPHICS=yes` and `ENABLE_FRAMEBUFFER=yes`. QEMU defaults to
`-vga std`. For UEFI, GOP framebuffer is used.

**Garbled framebuffer output**

Resolution mismatch. Defaults: 1024x768x32. Adjust in config:
`DISPLAY_DEFAULT_WIDTH`, `DISPLAY_DEFAULT_HEIGHT`, `DISPLAY_DEFAULT_BPP`.

---

## 8. USB Issues

**USB devices not detected**

Enable: `ENABLE_USB=yes` and a controller (e.g., `ENABLE_USB_EHCI=yes`).
For QEMU: `make run QEMU_USB=yes` (adds `-device usb-ehci`).

**USB keyboard/mouse not working in UEFI**

UEFI QEMU includes USB HID devices by default. Ensure `ENABLE_USB_HID=yes`.

---

## 9. Performance Issues

**Slow in QEMU**

Enable KVM: `QEMU_ENABLE_KVM=yes` in config (requires `/dev/kvm`).
Increase memory: `make run QEMU_MEMORY=1024`. Use native CPU:
`make run QEMU_OPTS="-cpu host"`.

**Slow boot**

Large initrds slow boot. Keep under ~8 MiB. Check:
`du -sh $FOREST/fern/initrd/`.

---

## 10. Development Environment Issues

**`dialog` errors from `./conf.sh --menuconfig`**

Install: `sudo apt install dialog`. Or use non-interactive modes:
`--defconfig`, `--oldconfig`, `--allnoconfig`, `--allyesconfig`.

**Stale build configuration**

After changing config, always: `./conf.sh --generate` then `make clean && make build`.

**Build parallelism**

Override: `make build PARALLEL_JOBS=4` or set `BUILD_PARALLEL_JOBS` in config.

---

## 11. QEMU-Specific Issues

**`qemu-system-i386: command not found`**

Install: `sudo apt install qemu-system-x86`.

**Serial output not visible**

Ensure `-serial stdio` is in the QEMU command. `make run` includes this by
default.

**Audio not working**

Enable in config: `ENABLE_AUDIO=yes` and a device (e.g., `ENABLE_SOUND_AC97=yes`).

**KVM not available**

Requires hardware virtualization support and `/dev/kvm` access:

```bash
ls -la /dev/kvm
sudo usermod -aG kvm $USER   # then re-login
```

---

## 12. Hardware Compatibility Issues

**BIOS boot fails on real hardware**

Write the complete disk image (not just stages):

```bash
sudo dd if=foreboots/build/forebo.img of=/dev/sdX bs=1M conv=fsync && sync
```

**UEFI boot fails on real hardware**

Write `esp.img` to a FAT partition or use the hybrid ISO on removable media.
Disable Secure Boot in firmware settings unless `ENABLE_UEFI_SECURE_BOOT=yes`.

**ARM/AArch64 QEMU is slow**

Expected -- runs under TCG (no cross-arch KVM). Allow ~20-30 seconds for
the menu to render.

---

## 13. Debug Techniques and Tools

### Serial Debug Output

All kernel messages go to serial (COM1 at 38400 baud):

```bash
make run                        # serial to terminal (default)
make run QEMU_OPTS="-serial file:/tmp/forestos.log"  # save to file
```

### GDB Kernel Debugging

```bash
# Terminal 1:
make debug    # QEMU with GDB stub on :1234

# Terminal 2:
gdb -ex "file build/32bit-bios-debug/boot/fern.elf" \
    -ex "target remote :1234" \
    -ex "break kmain" \
    -ex "continue"
```

### Automated Boot Test

```bash
make test-boot   # 60s timeout, checks for "Forest" in serial
```

### Disassembly and Inspection

```bash
make -C $FOREST/foreboots disasm
readelf -h build/32bit-bios-debug/boot/fern.elf
objdump -d build/32bit-bios-debug/boot/fern.elf | head -100
```

### Useful Debug Config Options

| Option | Effect |
|--------|--------|
| `DEBUG_LOG_LEVEL=5` | Maximum log verbosity |
| `ENABLE_ASSERTIONS=yes` | Kernel assertions |
| `ENABLE_MEMORY_DEBUG=yes` | Memory tracking |
| `ENABLE_MEMORY_PROTECTION=yes` | Guard pages + NX |
| `ENABLE_LOCK_DEBUGGING=yes` | Deadlock detection |
| `ENABLE_PANIC_BACKTRACES=yes` | Stack traces on panic |
| `ENABLE_PANICUI=yes` | Graphical crash screen |

---

## Quick Fix Sequence

When things go wrong, this resolves most issues:

```bash
cd $FOREST/forestos-toolchain && ./build-toolchain.sh --arch both
export FORESTOS_TOOLCHAIN_DIR=$FOREST/forestos-toolchain
cd $FOREST/fern
./conf.sh --defconfig && ./conf.sh --generate
make clean && make all && make run
```

---

## Getting Help

- Check serial output for kernel messages
- `make show-config` to verify effective configuration
- `make configcheck` to see which features are enabled
- Review `MAKE_AN_OS.md` for the full build walkthrough
- Enable verbose logging (`DEBUG_LOG_LEVEL=5`) and rebuild

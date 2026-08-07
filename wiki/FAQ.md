# Forest OS - Frequently Asked Questions

Welcome to the Forest OS FAQ! Here you'll find answers to the most common questions about this unix-like operating system.

---

## 1. What is Forest OS?

Forest OS (codename **ALDER**) is a unix-like, POSIX-oriented operating system built from scratch. It consists of three main components:

- **Fern** — the kernel
- **foreboots** (ForeB) — the bootloader (BIOS and UEFI)
- **libc** — a POSIX-oriented C standard library

Forest OS is deliberately kernel-oriented: it focuses on providing a solid foundation rather than bundling a graphical desktop or a full userspace application suite. You build it, boot it, and customize it for your needs.

## 2. Is it a Linux distribution?

No. Forest OS is **not** a Linux distribution. It is an independent operating system with its own kernel (Fern), its own bootloader (foreboots), and its own C library. While it aims for POSIX compatibility and uses Linux-compatible system call numbers, it is entirely separate from the Linux kernel and Linux-based systems.

## 3. Why build a custom OS?

People build custom operating systems for many reasons:

- **Learning**: Understanding how an OS works from the ground up is invaluable for systems programming
- **Control**: Having full control over every component, from bootloader to userspace
- **Experimentation**: Testing new ideas in OS design without the constraints of existing systems
- **Education**: Teaching operating system concepts in a practical, hands-on way
- **Fun**: There's something deeply satisfying about booting code you wrote yourself

Forest OS provides a clean, well-documented starting point for anyone interested in OS development.

## 4. What hardware does it run on?

Forest OS currently supports x86 architecture:

- **32-bit x86** (i686) — BIOS boot
- **64-bit x86_64** — BIOS and UEFI boot

The UEFI path also compiles for **AArch64** (ARM64) and **RISC-V 64**, though x86 remains the primary development target.

For real hardware, any x86 PC with BIOS or UEFI firmware should work. In practice, testing in QEMU is recommended first.

## 5. How do I build it?

### Prerequisites

On Debian/Ubuntu:

```bash
# For the cross-toolchain
sudo apt install build-essential gcc g++ make flex bison gawk \
                 texinfo curl wget tar xz-utils \
                 libgmp-dev libmpfr-dev libmpc-dev

# For Fern + foreboots + images
sudo apt install nasm clang lld xorriso mtools python3 \
                 qemu-system-x86 ovmf dialog
```

### Build Steps

```bash
# 1. Clone the repo
git clone <repo-url> forest
cd forest

# 2. Set the forest root
export FOREST="$(pwd)"

# 3. Build the cross-toolchain
cd forestos-toolchain
./build-toolchain.sh --arch both
cd ..

# 4. Configure Fern
cd fern
./conf.sh --defconfig

# 5. Build the kernel
make build

# 6. Build userspace apps
cd ../userspace
make

# 7. Copy apps into initrd
cp build/bin/* ../fern/initrd/bin/
ln -sf forest-shell ../fern/initrd/bin/shell
ln -sf forest-shell ../fern/initrd/bin/sh

# 8. Rebuild kernel with updated initrd
cd ../fern
make build

# 9. Build bootable image
make forebo-image

# 10. Run it!
make run
```

## 6. How do I run it in QEMU?

After building, use these commands from the `fern/` directory:

```bash
# Run with default settings (honors configured BOOT_MODE)
make run

# Force BIOS boot
make run-bios

# Force UEFI boot
make run-uefi

# 32-bit BIOS
make run32

# 64-bit BIOS
make run64

# Debug mode (GDB stub on :1234)
make debug
```

You can also use the `createos.sh` script for a guided, menu-driven build experience:

```bash
cd $FOREST
./createos.sh
```

## 7. How do I install it on real hardware?

**Warning: This will destroy existing data on the target device!**

### For BIOS systems:

```bash
# Write the raw disk image to a USB drive or HDD
sudo dd if=$FOREST/foreboots/build/forebo.img of=/dev/sdX bs=1M conv=fsync
sync
```

### For UEFI systems:

Write `esp.img` to a FAT partition, or use the hybrid `forebo.iso` on removable media.

Always double-check the device node before writing. The `forebo.img` file contains the bootloader stages and the Fern kernel embedded together.

## 8. What filesystems are supported?

Forest OS supports:

- **FAT** — via the UEFI firmware for the EFI System Partition
- **ext2/3/4** — read-only support in the UEFI shell and recovery tools
- **btrfs** — detection and subvolume/snapshot listing (read-only, best-effort)
- **devtmpfs** — kernel-created device filesystem
- **procfs** — process information filesystem
- **sysfs** — kernel object filesystem
- **tmpfs** — temporary filesystem in RAM

The root filesystem is typically an initrd (tar archive) loaded at boot time.

## 9. Does it have networking?

Forest OS includes networking headers and system call wrappers in the libc (`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`), but networking support is still in development. The kernel provides the foundation, and full network stack implementation is an ongoing area of work.

## 10. Does it have a GUI?

Forest OS is deliberately kernel-oriented and does **not** bundle a graphical desktop. However:

- The **bootloader** (foreboots) has a graphical boot menu with a forest theme, mouse support, and windowed tools on UEFI
- The kernel supports **framebuffer** output
- You can build and add your own graphical applications using the framebuffer APIs

The focus is on providing a solid foundation that you can build upon.

## 11. Can I run Linux binaries?

Not directly. Forest OS uses its own ABI and system call interface, though it uses Linux-compatible system call numbers. Simple, statically-linked programs might work with recompilation, but:

- Programs must be compiled with the Forest OS cross-toolchain (`i686-forestos-gcc` / `x86_64-forestos-gcc`)
- Dynamic linking requires the Forest OS libc and dynamic linker
- Complex Linux-specific programs will need porting

The goal is POSIX compatibility, not Linux binary compatibility.

## 12. How does it compare to other hobby OSes?

| Feature | Forest OS | Linux | Minix | HelenOS | Redox |
|---------|-----------|-------|-------|---------|-------|
| License | GPLv3 | GPLv2 | BSD | BSD | MIT |
| Architecture | x86 (32/64) | Multi-arch | Multi-arch | Multi-arch | x86, ARM |
| UEFI support | Yes | Yes | Limited | Yes | Yes |
| POSIX compat | Partial | Full | Partial | Partial | Partial |
| Focus | Learning/Custom | Production | Microkernel research | Research | Modern Rust OS |

Forest OS stands out with its clean design, well-documented codebase, and focus on being a practical learning tool.

## 13. Is it production ready?

No. Forest OS is a hobby/educational operating system. It is not intended for production use. There are many areas that need work:

- Full POSIX compliance
- Network stack
- Device drivers
- Memory management optimizations
- Security hardening

It is, however, excellent for learning, experimenting, and as a foundation for your own OS projects.

## 14. How can I contribute?

Forest OS is an open project and contributions are welcome:

1. **Read the documentation** — Start with `MAKE_AN_OS.md` and the component READMEs
2. **Try building it** — Get it running in QEMU and explore
3. **Pick an area** — Kernel, bootloader, libc, userspace apps, documentation
4. **Start small** — Fix a bug, add a feature, improve documentation
5. **Submit changes** — Follow the existing code style and conventions

Areas that especially need help:
- Device drivers
- Network stack implementation
- More userspace applications
- Documentation improvements
- Testing on real hardware

## 15. What's the license?

Forest OS is licensed under the **GNU General Public License v3 (GPLv3)**. This means:

- You can freely use, modify, and distribute the code
- Changes must be released under the same license
- Source code must be made available
- There is no warranty

Some vendored libraries (like uacpi) have their own licenses (MIT in uacpi's case).

## 16. Where can I find help?

- **Documentation**: Check the `wiki/` directory for detailed guides
- **Component READMEs**: Each component has its own README with specific information
  - `fern/README.md` — Kernel overview
  - `foreboots/README.md` — Bootloader details
  - `libs/libc/README.md` — C library documentation
- **MAKE_AN_OS.md**: The complete build guide from start to finish
- **AGENTS.md**: Build system reference (if present)
- **Source code**: The code is well-commented and designed to be read

For specific issues, check the troubleshooting section in `MAKE_AN_OS.md` for common problems and solutions.

---

## Additional Resources

- [Build Guide](../MAKE_AN_OS.md) — Complete build instructions
- [Kernel Documentation](../fern/README.md) — Fern kernel details
- [Bootloader Documentation](../foreboots/README.md) — ForeB bootloader guide
- [C Library Documentation](../libs/libc/README.md) — Forest OS libc reference

---

*Last updated: August 2026*

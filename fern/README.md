# Forest-OS

Forest-OS (codename **ALDER**) is a unix-like, POSIX-oriented operating system.
It is composed of two independently-named components:

- **Fern** — the kernel (analogy: "Linux" is the kernel, not the whole distro).
- **foreboots** (ForeB) — the bootloader, and the only userspace component.

Together — Fern + foreboots + a POSIX-oriented C library (**forestlibs**) — they
build a bootable ISO. Forest-OS is deliberately kernel-oriented: it does **not**
bundle a graphical desktop or a userspace application suite.

## Components

| Component | Name | Role |
|-----------|------|------|
| Kernel | **Fern** | The Forest-OS kernel; boot artifact `fern.elf` (`fern.bin` for BIOS). |
| Bootloader | **foreboots** (ForeB) | BIOS/CSM + native UEFI bootloader; the only userspace. |
| C library | **forestlibs** | POSIX-oriented libc the OS/ABI targets. |

The repository contains the Fern kernel sources, the forestlibs C library, the
foreboots bootloader, and the build tooling required to produce a bootable
image.

## Building

Forest-OS uses a Kconfig-style configuration flow (`conf.sh` →
`build-config.mk`) driven by the top-level `Makefile`. See `AGENTS.md` for the
full config/build-system reference.

1. **Configure**

   ```bash
   make defconfig        # sane kernel-only defaults (Fern + foreboots)
   ```

2. **Build the ISO**

   ```bash
   make iso
   ```

   The Fern kernel (`fern.elf`), initrd, and ISO image are written under the
   build output directory.

3. **Run in QEMU (optional)**

   ```bash
   make run
   ```

The build validates that a cross-toolchain is present before compiling. Point
`FORESTOS_TOOLCHAIN_DIR` at your toolchain if it is not in the default location.

# Forest OS Wiki

Welcome to the Forest OS wiki. Forest OS (codename **ALDER**) is a custom-built, Unix-like operating system written entirely from scratch. It is **not** a Linux distribution — it is an independent OS with its own kernel, bootloader, C library, toolchain, and userspace.

---

## What is Forest OS?

Forest OS is built from three core components:

| Component | Description |
|-----------|-------------|
| **Fern** | The kernel — handles memory, processes, filesystems, networking, graphics, audio, and hardware |
| **ForeB** | The bootloader — BIOS 3-stage loader and UEFI application with a graphical forest-themed menu |
| **ForestOS Toolchain** | Cross-compiler (GCC 13.2.0) targeting `i686-forestos` and `x86_64-forestos` |

Together with a POSIX C library, shared libraries, and 44 userspace applications, they produce a bootable ISO/IMG that runs on real hardware or QEMU.

---

## Quick Navigation

### Core Components
- [Architecture Overview](Architecture/Overview.md) — How everything fits together
- [Boot Flow](Architecture/Boot-Flow.md) — From power-on to login prompt
- [Kernel-Userspace Interface](Architecture/Kernel-Userspace.md) — System calls and conventions
- [Component Communication](Architecture/Component-Communication.md) — How parts talk to each other

### The Kernel (Fern)
- [Kernel Overview](Kernel/Overview.md) — Fern kernel at a glance
- [Memory Management](Kernel/Memory-Management.md) — Physical/virtual memory, paging, COW, swap
- [Process Management](Kernel/Process-Management.md) — Fork/exec, ELF loading, signals, SMP
- [Virtual Filesystem](Kernel/Virtual-Filesystem.md) — VFS layer, devfs, procfs, sysfs
- [Filesystems](Kernel/Filesystems.md) — FAT, ext2/3/4, ISO9660, and more
- [Networking](Kernel/Networking.md) — TCP/IP stack, Ethernet drivers
- [Graphics](Kernel/Graphics.md) — Framebuffer, drivers, font rendering
- [OpenGL](Kernel/OpenGL.md) — Software OpenGL 1.1 renderer
- [Audio](Kernel/Audio.md) — Sound system and drivers
- [USB](Kernel/USB.md) — USB controller and device drivers
- [Hardware Abstraction](Kernel/Hardware-Abstraction.md) — PCI, ACPI, timers, interrupts
- [Session Management](Kernel/Session-Management.md) — TTY sessions, GUI sessions
- [Security](Kernel/Security.md) — SMEP/SMAP, stack protection, VMM security
- [Multi-Architecture](Kernel/Multi-Architecture.md) — x86, x86_64, ARM, RISC-V support
- [Kernel Configuration](Kernel/Kernel-Configuration.md) — conf.sh and .forestos_config

### The Bootloader (ForeB)
- [Bootloader Overview](Bootloader/Overview.md) — ForeB at a glance
- [BIOS Boot](Bootloader/BIOS-Boot.md) — 3-stage MBR bootloader
- [UEFI Boot](Bootloader/UEFI-Boot.md) — Native UEFI loader
- [Boot Menu](Bootloader/Boot-Menu.md) — Graphical forest-themed menu
- [Configuration](Bootloader/Configuration.md) — forebo.cfg reference
- [Recovery Tools](Bootloader/Recovery-Tools.md) — Disk tools, shell, rescue mode
- [Theme Customization](Bootloader/Theme-Customization.md) — Backgrounds, icons, colors

### Libraries
- [Library Overview](Libraries/Overview.md) — Library ecosystem
- [libc](Libraries/libc.md) — POSIX C standard library
- [ForestCore](Libraries/ForestCore.md) — Low-level runtime helpers
- [LeafGFX](Libraries/LeafGFX.md) — Graphics library
- [LeafUI](Libraries/LeafUI.md) — UI framework
- [Third-Party Libraries](Libraries/Third-Party.md) — uACPI, qrcodegen

### Userspace
- [Userspace Overview](Userspace/Overview.md) — All 44 applications
- [Shell](Userspace/Shell.md) — forest-shell interactive shell
- [Core Utilities](Userspace/Core-Utilities.md) — cat, ls, cp, and friends
- [System Tools](Userspace/System-Tools.md) — ps, kill, init, shutdown
- [Disk Tools](Userspace/Disk-Tools.md) — fdisk, mkfs, mount, and more
- [X11 Server](Userspace/X11-Server.md) — forest-x11 display server
- [Initrd Builder](Userspace/Initrd-Builder.md) — Creating initial RAM disks

### Build System
- [Build System Overview](Build-System/Overview.md) — How the build works
- [Toolchain](Build-System/Toolchain.md) — Cross-compiler setup
- [Kernel Build](Build-System/Kernel-Build.md) — Building the kernel
- [Userspace Build](Build-System/Userspace-Build.md) — Building userspace apps
- [ISO Creation](Build-System/ISO-Creation.md) — Creating bootable media
- [Interactive Build](Build-System/Interactive-Build.md) — createos.sh GUI builder

### Development
- [Contributing](Development/Contributing.md) — How to contribute
- [Code Style](Development/Code-Style.md) — Coding conventions
- [Testing](Development/Testing.md) — Testing with QEMU
- [Troubleshooting](Development/Troubleshooting.md) — Common issues and fixes

### Reference
- [FAQ](FAQ.md) — Frequently asked questions

---

## Tech Stack at a Glance

| Layer | Technology |
|-------|-----------|
| Languages | C, x86/ARM/RISC-V Assembly, Bash, Python |
| Build System | GNU Make, Kconfig-style configuration |
| Kernel Compiler | GCC 13.2.0 (cross-compiled) |
| UEFI Compiler | Clang + ld.lld |
| Assembler | NASM (BIOS), GNU as (ARM/RISC-V) |
| Target Architectures | x86 (32-bit), x86_64, ARM32, AArch64, RISC-V 64 |
| License | GPLv3 |

---

## Getting Started

See the [Build System Overview](Build-System/Overview.md) for prerequisites and build instructions, or jump straight to the [Quick Start](Build-System/Overview.md#quick-start) section.

---

*This wiki documents Forest OS as of the current development state. The project is actively developed by [Enclica](https://github.com/Enclica-Forest/Forest).*

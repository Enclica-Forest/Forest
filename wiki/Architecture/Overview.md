# Architecture Overview

Welcome to the Forest OS architecture overview. This page explains how Forest OS is structured, what its components are, and how they all fit together into a working operating system.

---

## What is Forest OS?

Forest OS (codename **ALDER**) is a Unix-like, POSIX-oriented operating system written entirely from scratch. It is **not** a Linux distribution — it has its own kernel, bootloader, C library, cross-toolchain, and userspace applications.

Forest OS is kernel-oriented by design. The kernel (Fern) is the centerpiece, handling everything from memory management and process scheduling to graphics, audio, networking, and USB. The bootloader (ForeB) boots the kernel on both BIOS and UEFI firmware. A POSIX C library and 44 userspace applications round out the system into something you can actually use.

The project targets five architectures (x86 32-bit, x86_64, ARM32, AArch64, RISC-V 64) and produces bootable ISO/IMG files that run on real hardware or QEMU.

---

## The Three Core Components

Forest OS is assembled from three independently-named components plus a cross-toolchain:

| Component | Name | Role | Location |
|-----------|------|------|----------|
| Kernel | **Fern** | The Forest-OS kernel. Handles memory, processes, filesystems, networking, graphics, audio, hardware drivers, and system calls. | `fern/` |
| Bootloader | **ForeB** (foreboots) | BIOS 3-stage MBR loader and native UEFI application. Loads the Fern kernel and hands off with a Multiboot1-compatible contract. | `foreboots/` |
| C Library | **libc** | POSIX-oriented C standard library. Provides the system call interface, standard C functions, and error translation for userspace apps. | `libs/libc/` |
| Cross-Toolchain | **ForestOS Toolchain** | GCC 13.2.0 cross-compiler targeting `i686-forestos` and `x86_64-forestos`. Builds the kernel, libc, and userspace apps. | `forestos-toolchain/` |

Together — Fern + ForeB + libc + the toolchain — they build a bootable ISO. The whole thing is **Forest OS**; the kernel alone is **Fern**; the bootloader alone is **ForeB**.

### Repository Layout

```
$FOREST/                          # repo root
├── fern/                         # the Fern kernel tree
│   ├── Makefile                  # top-level build orchestrator
│   ├── conf.sh                   # Kconfig-style configurator
│   ├── build/                    # make fragments (toolchain.mk, iso.mk, foreb.mk, ...)
│   ├── src/                      # kernel C sources + src/include headers
│   │   ├── kernel.c              # kernel entry point (kmain)
│   │   ├── arch/                 # architecture abstraction layer
│   │   └── include/              # 230+ header files
│   ├── initrd/                   # initrd filesystem tree
│   └── forestos-toolchain -> ../forestos-toolchain  (symlink)
├── foreboots/                    # the ForeB bootloader
│   ├── stage1.asm / stage2.asm / stage3.asm  (BIOS stages)
│   └── uefi/                     # UEFI loader (bootx64.c, ui.c, shell.c, ...)
├── forestos-toolchain/           # cross-toolchain source + build scripts
│   ├── build-toolchain.sh        # canonical toolchain builder
│   ├── install/                  # built cross-compilers (generated)
│   └── sysroot/                  # target sysroot: headers + crt/libc
├── libs/                         # shared libraries
│   ├── libc/                     # POSIX C standard library
│   ├── forestcore/               # low-level kernel runtime helpers
│   ├── leafgfx/                  # userspace graphics library
│   ├── leafui/                   # userspace UI framework
│   ├── uacpi/                    # third-party ACPI implementation
│   └── qrcodegen/                # QR code generation
├── userspace/                    # 44 userspace applications
├── wiki/                         # this wiki
├── createos.sh                   # interactive build tool (GUI)
└── MAKE_AN_OS.md                 # end-to-end build guide
```

---

## High-Level Architecture Diagram

Here's how the major pieces of Forest OS relate to each other:

```
┌─────────────────────────────────────────────────────────────────┐
│                        Userspace Layer                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────────┐  │
│  │ forest-  │ │ cat, ls  │ │ init,    │ │ forest-x11       │  │
│  │ shell    │ │ echo,    │ │ mount,   │ │ (X11 server)     │  │
│  │ (44 apps)│ │ grep, ...│ │ sudo     │ │                  │  │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────────┬─────────┘  │
│       │             │            │                 │             │
│  ┌────▼─────────────▼────────────▼─────────────────▼──────────┐ │
│  │                    libc (POSIX C Library)                  │ │
│  │         stdio, stdlib, string, signal, pthread, ...        │ │
│  │              System Call Wrappers (int 0x80 / syscall)      │ │
│  └────────────────────────┬───────────────────────────────────┘ │
└───────────────────────────┼─────────────────────────────────────┘
                            │ syscalls
┌───────────────────────────┼─────────────────────────────────────┐
│                        Fern Kernel                              │
│  ┌────────────────────────▼───────────────────────────────────┐ │
│  │                   System Call Interface                     │ │
│  └───┬──────┬──────┬──────┬──────┬──────┬──────┬────────────┘ │
│      │      │      │      │      │      │      │              │
│  ┌───▼──┐┌──▼───┐┌─▼────┐┌▼────┐┌▼────┐┌▼────┐┌▼──────────┐ │
│  │Memory││Procs ││VFS   ││Net  ││GFX  ││Audio││Session    │ │
│  │Mgmt  ││Mgmt  ││Layer ││Stack││Mgr  ││Snd  ││Manager    │ │
│  │      ││      ││      ││TCP/ ││FB   ││HDA  ││TTY/Login  │ │
│  │PMM   ││ELF   ││devfs ││IP   ││TTF  ││SB16 ││           │ │
│  │VMM   ││fork  ││procfs││UDP  ││BMP  ││PCSpk││           │ │
│  │COW   ││exec  ││sysfs ││DNS  ││GL   ││AC97 ││           │ │
│  │Swap  ││SMP   ││FAT   ││VirtIO││Wayland││     ││           │ │
│  └───┬──┘└──┬───┘└─┬────┘└┬────┘└┬────┘└┬────┘└┬──────────┘ │
│      │      │      │      │      │      │      │              │
│  ┌───▼──────▼──────▼──────▼──────▼──────▼──────▼────────────┐ │
│  │              Hardware Abstraction Layer (HAL)              │ │
│  │  PCI/PCIe · ACPI · PIC/APIC · PIT/HPET · PS/2 · USB     │ │
│  │  ATA/AHCI/NVMe · VirtIO · Framebuffer · Serial · CMOS   │ │
│  └──────────────────────────┬───────────────────────────────┘ │
│                             │                                  │
│  ┌──────────────────────────▼───────────────────────────────┐ │
│  │           Multi-Architecture Abstraction (arch/)          │ │
│  │  x86_32 · x86_64 · ARM32 · AArch64 · RISC-V 64         │ │
│  └──────────────────────────────────────────────────────────┘ │
└────────────────────────────┬────────────────────────────────────┘
                             │ Multiboot1 handoff
┌────────────────────────────▼────────────────────────────────────┐
│                     ForeB Bootloader                            │
│  ┌──────────────────────┐  ┌────────────────────────────────┐  │
│  │  BIOS Path (NASM)    │  │  UEFI Path (freestanding C)    │  │
│  │  stage1 → stage2     │  │  bootx64.c                     │  │
│  │    → stage3          │  │  GOP framebuffer + menu         │  │
│  │  VBE framebuffer     │  │  GetMemoryMap → ExitBootServices│ │
│  │  E820 memory map     │  │  ELF parse → 32-bit PM handoff │  │
│  └──────────────────────┘  └────────────────────────────────┘  │
│  Both paths: EAX=0x2BADB002, EBX=&multiboot_info_t @ 0x1800   │
└─────────────────────────────────────────────────────────────────┘
```

---

## How the Components Relate

### Kernel ↔ Bootloader Interface

ForeB hands off to Fern using the Multiboot1 specification. When the kernel's entry point runs, it finds:

- `EAX` = `0x2BADB002` (Multiboot1 magic)
- `EBX` = pointer to `multiboot_info_t` at physical address `0x1800`

The kernel reads memory map, framebuffer info, initrd location, and command line from this struct. The kernel cannot tell whether it was booted by BIOS or UEFI — the handoff contract is identical.

ForeB also builds a richer native struct at `0x1000` (`foreboots_boot_info`) with 64-bit memory map data, CPU capability info, and kernel image metadata. This is a forward-looking extension point.

### Kernel ↔ Userspace Interface

Userspace applications communicate with the kernel through system calls. The libc provides POSIX-compatible wrappers:

- **x86 32-bit**: `int 0x80` interrupt
- **x86_64**: `syscall` instruction
- **ARM/AArch64/RISC-V**: architecture-specific mechanisms

The libc translates between userspace conventions (return -1 + set `errno`) and kernel conventions (return negative error code). It uses Linux-compatible system call numbers for maximum portability.

### Library Layer

Between the kernel and userspace applications sits a library ecosystem:

```
Userspace Apps
     │
     ├── leafgfx  (framebuffer, images, fonts, input)
     ├── leafui   (UI widgets, glass theme)
     ├── libc     (POSIX C standard library)
     └── forestcore (low-level runtime, MMIO, audio helpers)
```

The kernel also uses some of these libraries internally (forestcore for low-level helpers, uacpi for ACPI parsing).

---

## The Build Pipeline

Building Forest OS follows a clear pipeline. Each stage produces artifacts consumed by the next:

```
Step 1: Cross-Toolchain
  forestos-toolchain/build-toolchain.sh
  Output: install/bin/{i686,x86_64}-forestos-{gcc,as,ld,...}
          sysroot/usr/include/stdio.h, forestos/syscalls.h, ...

Step 2: Kernel Configuration
  fern/conf.sh --defconfig  (or --menuconfig)
  Output: .forestos_config → build-config.mk

Step 3: Kernel Compilation
  make build (or make all)
  Output: build/32bit-bios-debug/boot/fern.bin  (BIOS)
          build/64bit-uefi-debug/fern.elf       (UEFI)

Step 4: Initrd Creation
  fern/initrd/ → tar → initrd.tar (packed automatically by make)

Step 5: Bootloader Build + Image Assembly
  make forebo-image
  Output: foreboots/build/forebo.img   (BIOS disk image)
          foreboots/build/esp.img      (UEFI EFI System Partition)
          foreboots/build/forebo.iso   (hybrid BIOS+UEFI ISO)

Step 6: Boot!
  make run        (auto-detects configured boot mode)
  make run-bios   (force BIOS in QEMU)
  make run-uefi   (force UEFI/OVMF in QEMU)
```

### Toolchain → libc → Kernel → Bootloader → Userspace → ISO

The dependency chain is strictly linear:

1. **Toolchain first** — everything else is compiled with `i686-forestos-gcc` or `x86_64-forestos-gcc`. No host fallback for real builds.
2. **libc second** — the kernel and userspace both need POSIX headers and system call wrappers.
3. **Kernel third** — compiled with the cross-toolchain against the sysroot headers.
4. **Bootloader fourth** — takes the kernel ELF and wraps it into a bootable image. The BIOS path uses NASM; the UEFI path uses clang + ld.lld.
5. **Userspace fifth** — 44 apps compiled with the same cross-toolchain, placed into `fern/initrd/bin/`.
6. **ISO last** — the initrd is repacked with the updated userspace, and foreboots embeds everything into a bootable ISO/IMG.

---

## Target Architectures

Forest OS targets five CPU architectures. The kernel has a clean architecture abstraction layer (`fern/src/arch/`) that isolates architecture-specific code behind a common interface.

| Architecture | Bits | Toolchain | Status | Notes |
|-------------|------|-----------|--------|-------|
| **x86 (IA-32)** | 32 | `i686-forestos-gcc` | Primary | Full feature set. ForeB BIOS path. Multiboot1 handoff. |
| **x86_64 (AMD64)** | 64 | `x86_64-forestos-gcc` | Primary | Full feature set. ForeB UEFI path. Kernel self-transitions to long mode. |
| **ARM32 (ARMv7-A)** | 32 | `arm-none-eabi-gcc` | In progress | Architecture headers exist. Uses `arm-none-eabi` or `arm-linux-gnueabi` cross-compiler. |
| **AArch64 (ARMv8-A)** | 64 | `aarch64-linux-gnu-gcc` | In progress | ForeB UEFI app builds as `BOOTAA64.EFI`. Verified under QEMU with OVMF. |
| **RISC-V 64 (RV64GC)** | 64 | `riscv64-linux-gnu-gcc` | In progress | C code compiles to RISC-V objects. Bootable PE pending toolchain support. |

### Architecture Abstraction Layer

The `fern/src/arch/` directory provides a unified interface across all architectures:

```
fern/src/arch/
├── arch.h               # Main detection header (detects arch via compiler macros)
├── arch_ops.c           # Common architecture operations
├── interrupt.{c,h}      # Interrupt handling (architecture dispatch)
├── memory.{c,h}         # Physical/virtual memory management
├── task.{c,h}           # Process/thread context switching
├── syscall.{c,h}        # System call entry points
├── timer.{c,h}          # Timer abstraction
├── pci.{c,h}            # PCI bus access
├── framebuffer.{c,h}    # Framebuffer setup
├── uart.{c,h}           # Serial/UART console
├── x86_32/              # x86-specific: boot, paging, interrupts
├── x86_64/              # x86_64-specific: long mode, 64-bit paging
├── aarch64/             # ARM64-specific: exception levels, MMU
├── arm32/               # ARM32-specific: supervisor mode, MMU
└── riscv64/             # RISC-V-specific: S-mode, Sv39/Sv48
```

Each architecture defines the same set of functions (context switch, page table management, interrupt dispatch) but with architecture-specific implementations. The rest of the kernel includes only `arch/arch.h` and calls the common interface.

---

## The Library Ecosystem

Forest OS ships with six libraries under `libs/`. These serve both the kernel and userspace applications.

### libc — POSIX C Standard Library

The consolidated C library providing the standard POSIX interface. All Forest OS applications link against this.

| Feature | Status |
|---------|--------|
| File I/O (read, write, open, close, stat) | Working |
| Memory (malloc, free, mmap, brk) | Working |
| Process (fork, exec, wait, exit) | Working |
| Signals (sigaction, kill) | Working |
| Networking (socket, bind, listen, connect) | Working |
| Threads (pthread) | Stubs (single-thread sync works) |
| String functions | Working |
| printf/scanf family | Working |

The libc uses **Linux-compatible system call numbers** for maximum portability. It translates between userspace conventions (return -1 + set `errno`) and kernel conventions (return negative error code).

### ForestCore — Low-Level Runtime Helpers

Packages the kernel runtime pieces that don't belong in the exported C library:

- `types.h` — Forest-specific type definitions
- `system.h` — System-level helpers
- `net.h` — Network helpers
- MMIO/I/O port access wrappers
- Audio helpers
- String and utility functions

The kernel uses forestcore internally for low-level operations.

### LeafGFX — Graphics Library

A userspace graphics library for framebuffer-based applications:

- Framebuffer access and management
- Image loading (BMP, TGA)
- Font rendering (TTF rasterizer, bitmap fonts)
- Input handling (keyboard, mouse)
- Animation support
- Shadow effects and modern rendering

This is what userspace GUI apps use to draw to the screen.

### LeafUI — UI Framework

A modern UI framework built on top of LeafGFX:

- Glass/translucent theme system
- Widget primitives (buttons, panels, text input)
- Color constants and theming
- Event handling
- Drawing primitives (rect, circle, line, text)

LeafUI provides the visual building blocks for Forest OS's desktop-like interface.

### uACPI — ACPI Implementation

A third-party ACPI implementation pulled in as a subtree. The kernel uses this for:

- ACPI table parsing (RSDP, RSDT, XSDT, DSDT)
- Power management
- Device enumeration
- Interrupt routing

### qrcodegen — QR Code Generation

A small library for generating QR codes. Used by the kernel's boot splash and potentially by userspace applications.

---

## Userspace Layer

Forest OS includes **44 userspace applications** in `userspace/` — a complete set of POSIX-compatible tools for a functional operating system.

### Application Categories

| Category | Applications |
|----------|-------------|
| **Core Utilities** | cat, echo, ls, cp, mv, rm, mkdir, rmdir, touch, chmod, chown, ln, pwd, basename, dirname |
| **Text Processing** | grep, find, sort, wc, head, tail |
| **Disk/Filesystem** | dd, df, du, fdisk, mkfs, mount, umount, blkid, losetup, fsck |
| **System Tools** | ps, kill, init, shutdown, reboot, hostname, uname, date, sleep, id |
| **Shell & Auth** | forest-shell, sudo, su |
| **Display Server** | forest-x11 (X11 server) |
| **Build Tools** | initrd-builder (host tool for creating initrd images) |

### The Shell: forest-shell

`forest-shell` is the primary interactive shell with:

- 19 builtins (cd, exit, export, unset, env, set, pwd, echo, type, which, history, source, alias, unalias, jobs, fg, bg, wait, help, clear)
- Piping (`cmd1 | cmd2 | cmd3`)
- I/O redirection (`>`, `>>`, `<`, `2>`, `2>>`, `&>`)
- Background jobs with job control (Ctrl+Z, jobs, fg, bg)
- Variable expansion (`$VAR`, `${VAR}`, `$?`, `$#`, `$$`, `$@`, `$*`)
- Quoting (single, double, backslash escaping)
- Globbing (`*`, `?`)
- Command substitution (backticks)
- Aliases
- History (arrow keys, last 100 commands)
- Signal handling (Ctrl+C, Ctrl+\, Ctrl+Z)

### How Userspace Boots

1. ForeB loads the kernel ELF + initrd tarball into memory
2. Kernel parses the initrd and mounts it as the root filesystem
3. Session manager (`session.c`) searches for `/bin/shell`
4. The shell is loaded as PID 2+ and linked to a TTY
5. The user interacts with the OS through the shell

---

## Hardware Support Overview

Forest OS has broad hardware support through its modular driver architecture.

### Storage Controllers

| Driver | Type | Status |
|--------|------|--------|
| ATA/IDE | Legacy parallel ATA | Working |
| AHCI | SATA controller | Working |
| NVMe | Non-volatile memory express | Working |
| VirtIO | Virtual I/O (QEMU/KVM) | Working |
| FDC | Floppy disk controller | Working |
| SCSI | SCSI controller | Working |

### Graphics

| Driver | Type | Status |
|--------|------|--------|
| VESA VBE | BIOS framebuffer | Working |
| GOP | UEFI framebuffer | Working |
| VGA text | 80x25 text mode | Working |
| BGA | Bochs graphics adapter | Working |

### Input Devices

| Driver | Type | Status |
|--------|------|--------|
| PS/2 keyboard | AT keyboard controller | Working |
| PS/2 mouse | IntelliMouse protocol | Working |
| USB HID | Human interface devices | Working |
| Gameport | Legacy joystick port | Working |

### Networking

| Driver | Type | Status |
|--------|------|--------|
| VirtIO Net | Virtual NIC (QEMU) | Working |
| RTL8139 | Realtek NIC | Working |
| e1000 | Intel PRO/1000 | Working |

### Audio

| Driver | Type | Status |
|--------|------|--------|
| PC Speaker | Piezoelectric buzzer | Working |
| Sound Blaster 16 | ISA sound card | Working |
| AC'97 | Audio codec | Working |
| HDA | High Definition Audio | Working |
| OPL3 | FM synthesis | Working |
| VirtIO Sound | Virtual audio (QEMU) | Working |
| USB Audio | USB sound devices | Working |

### System

| Driver | Type | Status |
|--------|------|--------|
| PCI/PCIe | Bus enumeration | Working |
| ACPI | Power management, device config | Working |
| APIC | Advanced Programmable Interrupt Controller | Working |
| PIC 8259 | Legacy interrupt controller | Working |
| PIT | Programmable Interval Timer | Working |
| HPET | High Precision Event Timer | Working |
| CMOS RTC | Real-time clock | Working |
| PS/2 controller | Keyboard/mouse controller | Working |
| USB (EHCI/OHCI/UHCI/XHCI) | USB host controllers | Working |
| Serial (16550) | UART console/debug | Working |
| Parallel port | Legacy printer port | Working |
| VirtualBox Guest | Guest additions | Working |

### Kernel Safety Features

The kernel includes several security and stability features:

- **SMEP/SMAP** — Supervisor Mode Execution/Access Prevention (x86)
- **Stack protection** — Stack canaries and guard pages
- **Memory corruption detection** — Heap validation and integrity checks
- **Page fault recovery** — Graceful handling of invalid memory accesses
- **Watchdog** — PS/2 controller watchdog for hangs
- **Panic UI** — Full graphical panic screen with effects

---

## Boot Flow Summary

Here's the complete journey from power-on to login prompt:

```
Power On / Reset
       │
       ▼
┌──────────────────────────────────────────────────┐
│ BIOS / UEFI Firmware                             │
│ Performs POST, finds bootable device              │
└───────────────────────┬──────────────────────────┘
                        │
           ┌────────────┴────────────┐
           │                         │
           ▼                         ▼
┌─────────────────────┐  ┌──────────────────────────┐
│ ForeB BIOS Path     │  │ ForeB UEFI Path           │
│ stage1 (MBR, 512B)  │  │ bootx64.c (EFI app)       │
│   → relocate        │  │   → GOP framebuffer       │
│   → LBA probe       │  │   → GetMemoryMap          │
│   → load stage2     │  │   → Graphical menu        │
│                     │  │   → Load kernel from ESP  │
│ stage2 (8 KiB)      │  │   → ExitBootServices     │
│   → A20 enable      │  │   → Parse ELF            │
│   → E820 memory map │  │   → Tear down long mode  │
│   → VBE framebuffer │  │   → 32-bit PM handoff    │
│   → Boot menu       │  └────────────┬─────────────┘
│   → Load kernel     │               │
│   → Build multiboot │               │
│                     │               │
│ stage3 (8 KiB)      │               │
│   → Parse ELF       │               │
│   → Copy segments   │               │
│   → Jump to kernel  │               │
└─────────┬───────────┘               │
          │                           │
          └───────────┬───────────────┘
                      │
                      ▼
┌──────────────────────────────────────────────────┐
│ Fern Kernel Entry (32-bit protected mode)         │
│ EAX = 0x2BADB002, EBX = &multiboot_info_t        │
│                                                    │
│ 1. Parse multiboot info (memory, framebuffer, etc)│
│ 2. Initialize GDT, IDT, PIC                       │
│ 3. Set up physical memory manager (PMM)           │
│ 4. Set up virtual memory manager (VMM)            │
│ 5. Initialize kernel heap                        │
│ 6. Mount initrd as root filesystem               │
│ 7. Initialize drivers (PCI, ATA, USB, etc)       │
│ 8. Set up TTY / framebuffer console              │
│ 9. Start session manager                          │
└───────────────────────┬──────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────┐
│ Session Manager                                    │
│ - Authenticate user                               │
│ - Load /bin/shell (PID 2+)                        │
│ - Link to TTY session                             │
│ - Set as foreground process                       │
└───────────────────────┬──────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────┐
│ Login Prompt / Shell Ready                        │
│ User can now interact with Forest OS              │
└──────────────────────────────────────────────────┘
```

---

## Kernel Subsystem Map

For a more detailed look at what lives inside Fern, here's a map of the major kernel subsystems:

| Subsystem | Key Files | What It Does |
|-----------|-----------|-------------|
| **Memory** | `pmm.c`, `vmm.c`, `kheap.c`, `mm_cow.c`, `mm_swap.c` | Physical/virtual memory, paging, copy-on-write, swap |
| **Processes** | `task.c`, `thread.c`, `elf.c`, `syscall.c` | Fork/exec, ELF loading, context switching, system calls |
| **VFS** | `vfs.c`, `fat.c`, `devfs.c`, `procfs.c`, `sysfs.c` | Virtual filesystem layer, FAT32, device/proc/sys nodes |
| **Graphics** | `graphics_init.c`, `tty.c`, `splash.c`, `wayland_*.c` | Framebuffer, TTY, boot splash, Wayland compositor |
| **Audio** | `sound.c`, `sound_hda.c`, `sound_sb16.c`, `sound_ac97.c` | Sound system, HDA/SB16/AC97 drivers |
| **Networking** | `net.c`, `virtio_net.c`, `ip.c`, `tcp.c`, `udp.c` | TCP/IP stack, VirtIO NIC, DNS, DHCP |
| **USB** | `usb.c`, `xhci_hc.c`, `ehci_hc.c`, `usb_hid.c` | USB host controllers, HID devices |
| **Input** | `ps2_keyboard.c`, `ps2_mouse.c`, `input_event.c` | PS/2 keyboard/mouse, input event system |
| **Interrupts** | `idt.c`, `interrupt.c`, `apic.c`, `ioapic.c` | IDT setup, interrupt routing, APIC/IOAPIC |
| **ACPI** | `acpi.c`, `uacpi_port.c` | ACPI table parsing, power management |
| **Session** | `session.c`, `tty.c`, `shell_loader.c` | Login sessions, TTY management, shell launch |
| **SMP** | `smp.c`, `ipi_smp_coordination.c` | Symmetric multiprocessing, IPI |
| **Security** | `smep_smap.c`, `ssp.c`, `stack_protection.c` | SMEP/SMAP, stack canaries, memory protection |

---

## Configuration System

Forest OS uses a Kconfig-style configuration flow:

```bash
./conf.sh --defconfig          # sane defaults (32-bit, BIOS, debug)
./conf.sh --menuconfig         # interactive TUI (needs dialog)
./conf.sh --allnoconfig        # minimal (all features off)
./conf.sh --allyesconfig       # maximal (all features on)
```

Configuration lives in `.forestos_config` and generates `build-config.mk`. Key options:

| Option | Values | Default |
|--------|--------|---------|
| `BUILD_ARCH` | `32`, `64`, `arm`, `aarch64`, `riscv64` | `32` |
| `BUILD_BOOT_MODE` | `bios`, `uefi` | `bios` |
| `BUILD_TYPE` | `debug`, `release` | `debug` |
| `ENABLE_FOREB_BOOTLOADER` | `yes`, `no` | `yes` |

Feature flags (enable/disable subsystems):

| Feature | Config Key |
|---------|-----------|
| Graphics | `CONFIG_GRAPHICS` |
| Networking | `CONFIG_NETWORKING` |
| Audio | `CONFIG_AUDIO` |
| USB | `CONFIG_USB` |
| OpenGL | `CONFIG_OPENGL` |
| SMP | `CONFIG_SMP` |
| X11 Server | `CONFIG_X11` |

---

## What Makes Forest OS Different?

A few things set Forest OS apart from other hobby OS projects:

1. **Multi-architecture from day one** — Not x86-only with plans for ARM. Five architectures with a clean abstraction layer.

2. **Real bootloader** — ForeB is not "use GRUB and call it done." It's a custom BIOS 3-stage loader AND a native UEFI application with a graphical forest-themed menu, mouse support, a window manager, an interactive shell, and recovery tools.

3. **POSIX-compatible userspace** — 44 applications that actually work, including a real shell with piping, job control, and aliases. Not just `echo` and `halt`.

4. **Linux-compatible syscalls** — Programs compiled for Forest OS use the same syscall numbers as Linux. This makes porting existing software much easier.

5. **Modern kernel features** — Copy-on-write, swap, page fault recovery, SMP, Wayland compositor, USB stack, and a graphical panic screen. This isn't just a "hello world" kernel.

6. **Build from scratch toolchain** — The cross-compiler is built from GCC source, not borrowed from a Linux distribution. This ensures true independence.

7. **Self-hosting capable** — Once Forest OS boots, it can rebuild its own toolchain natively (`BUILD == HOST == forestos`).

---

*This wiki documents Forest OS as of the current development state. The project is actively developed.*

# Fern Kernel Overview

Fern is the kernel of Forest OS (codename **ALDER**), a Unix-like, POSIX-oriented operating system. Fern handles everything from physical memory management and process scheduling to device drivers, graphics, networking, and audio -- all in a single monolithic binary (`fern.elf` for ELF, `fern.bin` for BIOS).

Forest OS is deliberately kernel-oriented: it does **not** bundle a graphical desktop or a userspace application suite. The kernel ships alongside **ForeB** (the bootloader) and a consolidated POSIX-oriented C library, which together produce a bootable ISO.

---

## Design Philosophy

Fern follows a **monolithic kernel** design, meaning all kernel subsystems -- drivers, filesystems, memory management, networking -- run in a single privileged address space. This is a deliberate choice: it keeps inter-subsystem communication fast (direct function calls instead of IPC), simplifies DMA and memory sharing, and matches the POSIX/Linux heritage the project targets.

Key design principles:

- **POSIX-oriented**: The kernel targets POSIX interfaces (signals, termios, process groups, sysv IPC, shared memory, epoll, inotify) so that familiar Unix userspace software can eventually run on Forest OS.
- **Configurable via feature flags**: Nearly every subsystem can be independently enabled or disabled at build time. This keeps the kernel adaptable -- from a tiny embedded profile (8 MB RAM minimum) to a full-featured desktop build.
- **Multi-architecture**: Supports x86 (32-bit and 64-bit), ARM32, AArch64, and RISC-V 64-bit. Architecture-specific code lives in dedicated subdirectories under `src/`.
- **Defensive boot**: The boot sequence is carefully ordered to detect failures early, display rich status output (or a splash screen), and gracefully fall back when hardware is unavailable (e.g., framebuffer failure drops to VGA text mode).
- **Self-contained**: The kernel includes its own ELF loader, initrd parser, and userspace boot path, making it bootable without external dependencies beyond the bootloader.

---

## Kernel Subsystems

Fern is organized into a large number of subsystems, each independently gated by build-time feature flags. The table below provides a quick reference:

| Subsystem | Key Files | Description |
|---|---|---|
| **Memory Management** | `memory.c`, `pmm.c`, `pmm_enhanced.c`, `bitmap_pmm.c`, `kheap.c`, `enhanced_heap.c`, `vmm.c`, `mm_*.c` | Physical page allocator (bitmap PMM), virtual memory manager, kernel heap, SLAB allocator, copy-on-write, swap, page cache, OOM killer, memory reclaim, TLB management |
| **Process Management** | `task.c`, `thread.c`, `syscall.c`, `syscall64.c`, `job_control.c` | Process/thread creation and scheduling, context switching, syscall dispatch (int 0x80 / syscall), POSIX signals, job control |
| **Scheduler** | `smp.c`, `smp_interrupt_distribution.c`, `ipi_smp_coordination.c` | SMP-aware scheduling, per-CPU idle tasks, priority levels, IPI-based TLB shootdown |
| **Virtual Filesystem** | `vfs.c`, `fs.c`, `fs_internal.c`, `symlink.c`, `ustar.c` | VFS layer, mount points, path resolution, symlink support, USTAR initrd format |
| **Filesystem Drivers** | `fat.c`, `exfat.c`, `iso9660.c`, `ext2` (in `fs.c`), `udf.c`, `lean.c`, `yaffs.c`, `jffs2.c`, `ffs_amiga.c`, `zdsfs.c` | FAT32, exFAT, ISO9660, ext2, UDF, Lean, YAFFS, JFFS2, Amiga FFS, ZDS filesystem drivers |
| **Virtual Filesystems** | `procfs.c`, `sysfs.c`, `devfs.c`, `device_fs.c`, `ramdisk.c` | /proc, /sys, /dev filesystems, RAM disk block device |
| **Block Devices** | `block_devices.c`, `ata.c`, `ahci.c`, `nvme.c`, `scsi.c`, `fdc.c`, `loop_devices` | ATA/IDE, AHCI (SATA), NVMe, SCSI, floppy, loop device support |
| **Interrupt Handling** | `interrupt.c`, `interrupt_handlers.c`, `idt.c`, `pic_8259a.c`, `apic.c`, `ioapic.c`, `nmi.c`, `msi_support.c` | IDT setup, legacy PIC, Local APIC, IOAPIC, MSI, NMI, interrupt priority, EOI management, vector allocation |
| **Timers** | `timer.c`, `pit.c`, `hpet.c`, `apic_timer.c`, `tsc_calibration.c`, `epoch.c`, `timer_abstraction.c` | PIT, HPET, APIC timer, TSC calibration, unified timer abstraction, UNIX epoch time |
| **PCI/PCIe** | `pci.c`, `pcie.c` | PCI bus enumeration, PCIe configuration space access |
| **Graphics** | `graphics_manager`, `framebuffer_dbuf.c`, `vga_text`, `vesa`, `bochs_bga`, `intel_hd`, `nvidia_gpu`, `amd_gpu` | Graphics subsystem manager, framebuffer double-buffering, VGA text/graphics, VESA, Bochs/BGA, Intel HD, NVIDIA, AMD GPU drivers |
| **Display Management** | `display_manager.c`, `mode_state.c`, `hotkey.c`, `splash.c` | Display mode management, hotkey system, boot splash screen |
| **Compositor / Wayland** | `wayland_compositor.c`, `wayland_protocol.c`, `wayland_server.c`, `wayland_shell.c`, `wayland_xdg.c`, `wayland_dmabuf.c`, `wayland_input.c` | In-kernel Wayland display server, XDG shell protocol, DMA-BUF sharing |
| **X11** | `x11_server.c`, `xdg.c` | X11 server compatibility layer |
| **Input** | `ps2_keyboard.c`, `ps2_mouse.c`, `ps2_controller.c`, `input_mux.c`, `keyboard_layout.c`, `kb.c` | PS/2 keyboard and mouse drivers, input event multiplexer, keyboard layouts |
| **USB** | `usb.c`, `ehci_hc.c`, `uhci_hc.c`, `ohci_hc.c`, `xhci_hc.c`, `usb_hid.c`, `usb_hub.c` | USB stack: EHCI, UHCI, OHCI, xHCI host controllers, HID devices, hub support |
| **Audio** | `sound.c`, `sound_mixer.c`, `sound_sb16.c`, `sound_ac97.c`, `sound_hda.c`, `sound_opl3.c`, `sound_pc_speaker.c`, `virtio_snd.c` | Sound Blaster 16, AC'97, HD Audio, OPL3, PC speaker, VirtIO sound, audio mixer, PCM devices |
| **Networking** | `net.c`, `networking/tcp.c`, `networking/udp.c`, `networking/arp.c`, `networking/icmp.c`, `networking/dhcp.c`, `networking/dns.c`, `virtio_net.c` | TCP/IP stack, ARP, ICMP, DHCP, DNS, VirtIO network, E1000/RTL8139/NE2000 drivers |
| **IPC** | `ipc.c`, `semaphore.c`, `barrier.c`, `sysv_sem.c`, `sysv_msg.c`, `posix_shm.c`, `epoll.c`, `inotify.c`, `eventfd.c` | IPC channels, SysV semaphores/messages, POSIX shared memory, epoll, inotify, eventfd |
| **DBus** | `dbus.c`, `dbus_bus.c`, `dbus_codec.c`, `dbus_session.c`, `dbus_system.c` | D-Bus message bus protocol implementation |
| **Security** | `auth.c`, `session.c`, `smep_smap.c`, `stack_protection.c`, `ssp.c`, `fault_prevention.c` | User authentication, session management, SMEP/SMAP hardware protection, stack canaries, stack smashing protection |
| **ACPI** | `acpi.c`, `acpi_enhanced.c`, `acpi_quirks.c`, `uacpi_port.c`, `acpi_interrupt_routing.c` | ACPI table discovery (via uACPI), power management, interrupt routing, ACPI quirks |
| **Hardware Detection** | `hardware.c`, `driver.c`, `driver_registry.c`, `driver_bus.c` | CPUID detection, driver registry, driver bus abstraction |
| **ELF Loader** | `elf.c`, `ldso.c` | ELF binary loading, dynamic linker (ld.so) |
| **Panic / Debug** | `panic.c`, `panicui.c`, `stacktrace.c`, `debuglog.c`, `lock_debug.c`, `memory_corruption.c` | Kernel panic handler with graphical UI, stack traces, debug logging, lock debugging |
| **TTY** | `tty.c`, `tty_devices.c`, `tty_render.c`, `console` | Framebuffer TTY with ANSI escape sequence support, virtual terminals, console |
| **Virtualization** | `virtualbox_guest.c`, `vm_detect.c`, `vm_guest.c` | VirtualBox Guest Additions, hypervisor detection, VM guest support |
| **Timer Devices** | `timer_dev.c`, `timer_abstraction.c`, `time_enhanced.c` | Device nodes for timers (/dev/timer, /dev/rtc), timer abstraction layer |
| **GPU Acceleration** | `gl/*.c` | Software OpenGL renderer |
| **OpenGL** | `src/gl/` | Software OpenGL implementation |

---

## Source Directory Layout

The kernel source tree at `fern/src/` is organized as follows:

```
fern/
├── build/
│   ├── features/          # Feature-flag .mk fragments (memory, graphics, networking, ...)
│   ├── config.mk          # Core build configuration (ARCH, BOOT_MODE, etc.)
│   ├── dirs.mk            # Output directory definitions
│   ├── flags.mk           # Compiler/linker flags
│   ├── kernel-sources.mk  # Source file selection and exclusion logic
│   ├── toolchain.mk       # Cross-toolchain detection
│   ├── iso.mk             # ISO image creation
│   ├── foreb.mk           # ForeB bootloader build
│   ├── qemu-run.mk        # QEMU launch targets
│   └── clean.mk           # Cleanup targets
├── src/
│   ├── kernel.c           # kmain() and boot sequence -- the entry point
│   ├── boot.asm           # 32-bit boot stub (Multiboot)
│   ├── boot64.asm         # 64-bit boot stub
│   ├── gdt.c / gdt64.c    # Global Descriptor Table
│   ├── idt.c / idt64.c    # Interrupt Descriptor Table
│   ├── context_switch.asm # Process context switch
│   ├── interrupt_stubs.asm # Interrupt entry stubs
│   ├── include/           # All kernel headers (231 headers)
│   │   ├── memory.h       # Memory subsystem API
│   │   ├── task.h         # Process/task API
│   │   ├── vfs.h          # Virtual filesystem API
│   │   ├── interrupt.h    # Interrupt system API
│   │   ├── graphics/      # Graphics subsystem headers
│   │   ├── usb/           # USB subsystem headers
│   │   └── sys/           # POSIX-style system headers
│   ├── graphics/          # Graphics drivers (vesa, bochs, intel, nvidia, amd, font)
│   │   └── drivers/       # Individual GPU drivers
│   ├── gl/                # Software OpenGL renderer
│   ├── input/             # Input subsystem (keyboard, mouse, input mux)
│   ├── networking/        # Network protocol implementations
│   ├── usb/               # USB subsystem internals
│   ├── uefi/              # UEFI boot support
│   ├── bios/              # BIOS boot support
│   ├── arch/              # Architecture-specific wrappers
│   ├── arm32/             # ARM 32-bit architecture code
│   ├── aarch64/           # ARM 64-bit architecture code
│   ├── riscv64/           # RISC-V 64-bit architecture code
│   ├── x86_64/            # x86-64 architecture code
│   └── crossarcinterpret/ # Cross-architecture interpreter
├── libs/
│   ├── forestcore/        # Low-level kernel runtime helpers
│   ├── libc/              # POSIX C standard library
│   ├── leafgfx/           # LeafGFX graphics library (BMP, TTF, animation)
│   ├── leafui/            # LeafUI widget framework
│   ├── uacpi/             # Third-party ACPI implementation (subtree)
│   └── qrcodegen/         # QR code generation library
├── forestlibs/            # Exported libc snapshot
├── initrd/                # Initial ramdisk filesystem template
│   ├── bin/               # /bin (init, shell, utilities)
│   ├── dev/               # /dev (device nodes)
│   ├── etc/               # /etc (configuration)
│   ├── proc/              # /proc (process information)
│   ├── tmp/               # /tmp (temporary files)
│   ├── usr/               # /usr (shared resources, icons, fonts)
│   └── var/               # /var (variable data)
├── Makefile               # Top-level build orchestrator
├── conf.sh                # Configuration generator (Kconfig-style)
└── build-config.mk        # Generated build configuration
```

---

## Feature Flags

Fern uses a Kconfig-style configuration system. `conf.sh` generates `build-config.mk` from `.forestos_config`, and each `ENABLE_*` boolean controls whether a subsystem's source files are compiled. The `build/features/*.mk` fragments implement the source gating.

### Memory Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_PAGING` | yes | Virtual memory, page tables, VMM, page fault handlers |
| `ENABLE_SLAB` | yes | SLAB allocator for small object allocation |
| `ENABLE_COW` | yes | Copy-on-Write for fork() and memory efficiency |
| `ENABLE_SWAP` | yes | Swap-out/swap-in for overcommit support |
| `ENABLE_PAGE_CACHE` | yes | Page cache for file-backed pages |
| `ENABLE_OOM_KILLER` | yes | Out-of-memory killer to recover from memory exhaustion |
| `ENABLE_MEMORY_RECLAIM` | yes | Background page reclamation / shrinker |
| `ENABLE_TLB_SHOOTDOWN` | yes | Inter-processor TLB invalidation via IPI |
| `ENABLE_GUARD_PAGES` | yes | Guard pages to detect stack/heap overflow |
| `ENABLE_ASLR` | no | Address Space Layout Randomization |
| `ENABLE_NX_BIT` | yes | No-execute bit enforcement (DEP) |
| `ENABLE_MEMORY_PROTECTION` | yes | Memory protection (NX, SMEP, SMAP, PAT) |
| `ENABLE_MEMORY_CORRUPTION_DETECTION` | yes | Runtime memory corruption detection |

### Graphics Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_GRAPHICS` | yes | Master toggle for graphics subsystem |
| `ENABLE_FRAMEBUFFER` | yes | Linear framebuffer support |
| `ENABLE_VGA_TEXT` | yes | VGA text mode console |
| `ENABLE_VESA` | yes | VESA VBE mode switching |
| `ENABLE_BOCHS_BGA` | yes | Bochs/BGA graphics adapter |
| `ENABLE_INTEL_HD` | yes | Intel HD Graphics driver |
| `ENABLE_NVIDIA_GPU` | yes | NVIDIA GPU driver |
| `ENABLE_AMD_GPU` | yes | AMD GPU driver |
| `ENABLE_VMWARE_SVGA` | yes | VMware SVGA driver |
| `ENABLE_COMPOSITOR` | yes | In-kernel display compositor |
| `ENABLE_WAYLAND_SERVER` | yes | Wayland protocol server |
| `ENABLE_X11_SERVER` | yes | X11 protocol server |
| `ENABLE_FONT_RENDERER` | yes | Bitmap font rendering |
| `ENABLE_TRUETYPE` | yes | TrueType font rasterization |
| `ENABLE_DOUBLE_BUFFERING` | yes | Double-buffered framebuffer |
| `ENABLE_SPLASH_SCREEN` | yes | Boot splash screen |
| `ENABLE_PANICUI` | yes | Graphical kernel panic UI |
| `ENABLE_DISPLAY_MANAGER` | yes | Display mode management |
| `ENABLE_CLIPBOARD` | yes | Clipboard support |
| `ENABLE_DRAG_DROP` | yes | Drag-and-drop support |
| `ENABLE_LEAFGFX` | yes | LeafGFX rendering library |
| `ENABLE_GPU_ACCEL` | yes | GPU acceleration support |

### Networking Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_NETWORKING` | no | Master networking toggle |
| `ENABLE_TCP` | no | TCP protocol stack |
| `ENABLE_UDP` | no | UDP protocol stack |
| `ENABLE_ARP` | no | ARP protocol |
| `ENABLE_ICMP` | no | ICMP (ping) |
| `ENABLE_DHCP` | no | DHCP client |
| `ENABLE_DNS` | no | DNS resolver |
| `ENABLE_ETHERNET` | no | Ethernet frame handling |
| `ENABLE_DRIVER_E1000` | no | Intel E1000 NIC driver |
| `ENABLE_DRIVER_RTL8139` | no | Realtek RTL8139 NIC driver |
| `ENABLE_DRIVER_NE2000` | no | NE2000 NIC driver |

### Audio Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_AUDIO` | no | Master audio toggle |
| `ENABLE_SOUND_SB16` | no | Sound Blaster 16 driver |
| `ENABLE_SOUND_SBPRO` | no | Sound Blaster Pro driver |
| `ENABLE_SOUND_AC97` | no | AC'97 audio driver |
| `ENABLE_SOUND_HDA` | no | HD Audio driver |
| `ENABLE_SOUND_ENSONIQ` | no | Ensoniq audio driver |
| `ENABLE_SOUND_OPL3` | no | OPL3 FM synthesis |
| `ENABLE_SOUND_PC_SPEAKER` | no | PC speaker beep |
| `ENABLE_SOUND_USB` | no | USB audio |
| `ENABLE_AUDIO_WAV` | no | WAV file playback |
| `ENABLE_AUDIO_VORBIS` | no | Ogg Vorbis playback |

### USB Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_USB` | no | Master USB toggle |
| `ENABLE_USB_EHCI` | no | EHCI (USB 2.0) host controller |
| `ENABLE_USB_UHCI` | no | UHCI (USB 1.x) host controller |
| `ENABLE_USB_OHCI` | no | OHCI (USB 1.x) host controller |
| `ENABLE_USB_XHCI` | no | xHCI (USB 3.0) host controller |
| `ENABLE_USB_HID` | no | USB HID (keyboard, mouse) |
| `ENABLE_USB_HUB` | no | USB hub support |
| `ENABLE_USB_MASS_STORAGE` | no | USB mass storage |

### Storage Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_ATA` | yes | ATA/IDE PIO controller |
| `ENABLE_AHCI` | yes | AHCI (SATA) controller |
| `ENABLE_NVME` | yes | NVMe SSD controller |
| `ENABLE_SCSI` | no | SCSI controller |
| `ENABLE_FDC` | no | Floppy disk controller |
| `ENABLE_BLOCK_DEVICES` | yes | Block device abstraction layer |
| `ENABLE_LOOP_DEVICES` | yes | Loopback block devices |

### Filesystem Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_VFS` | yes | Master VFS toggle |
| `ENABLE_FAT32` | yes | FAT32 filesystem |
| `ENABLE_EXFAT` | yes | exFAT filesystem |
| `ENABLE_EXT2` | yes | ext2 filesystem |
| `ENABLE_ISO9660` | yes | ISO 9660 (CD-ROM) |
| `ENABLE_TMPFS` | yes | tmpfs (RAM-based) |
| `ENABLE_DEVFS` | yes | /dev filesystem |
| `ENABLE_PROCFS` | yes | /proc filesystem |
| `ENABLE_SYSFS` | yes | /sys filesystem |
| `ENABLE_SYMLINKS` | yes | Symbolic link support |

### Interrupt Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_PIC_8259A` | yes | Legacy 8259A PIC |
| `ENABLE_LOCAL_APIC` | yes | Local APIC (per-CPU) |
| `ENABLE_IOAPIC` | yes | I/O APIC |
| `ENABLE_NMI_HANDLER` | yes | Non-Maskable Interrupt handler |
| `ENABLE_MSI` | no | Message Signaled Interrupts |
| `ENABLE_INTERRUPT_PRIORITY` | yes | Interrupt priority management |
| `ENABLE_INTERRUPT_STATISTICS` | yes | Interrupt statistics tracking |
| `ENABLE_INTERRUPT_VECTOR_ALLOCATION` | yes | Dynamic vector allocation |
| `ENABLE_INTERRUPT_EOI_MANAGEMENT` | yes | End-of-Interrupt management |

### Scheduler / Process Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_ELF_LOADER` | yes | ELF binary loader |
| `ENABLE_LDSO` | yes | Dynamic linker (ld.so) |
| `ENABLE_SMP` | yes | Symmetric multi-processing |
| `ENABLE_IDLE_TASK` | yes | CPU idle task |
| `ENABLE_FPU` | yes | Floating-point unit support |
| `ENABLE_JOB_CONTROL` | yes | POSIX job control (SIGTSTP, etc.) |
| `ENABLE_SIGNALS` | yes | POSIX signals |
| `ENABLE_X86_IST` | yes | x86-64 Interrupt Stack Table |

### Security Features

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_SMEP_SMAP` | yes | Supervisor Mode Execution/Access Prevention |
| `ENABLE_STACK_PROTECTION` | yes | Stack overflow detection |
| `ENABLE_AUTH` | yes | User authentication |
| `ENABLE_SESSION_MANAGEMENT` | yes | Login session management |
| `ENABLE_LOCK_DEBUGGING` | yes | Lock contention debugging |
| `ENABLE_FAULT_PREVENTION` | yes | Page fault prevention |
| `ENABLE_MEMORY_VALIDATION` | yes | Memory access validation |

### Hardware / Compatibility

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_PCI` | yes | PCI bus support |
| `ENABLE_PCIE` | yes | PCIe configuration space |
| `ENABLE_ACPI` | yes | ACPI tables and power management |
| `ENABLE_SERIAL` | yes | Serial port (COM1) |
| `ENABLE_PARALLEL` | no | Parallel port |
| `ENABLE_A20` | yes | A20 gate (16-bit memory wrap) |
| `ENABLE_VIRTUALBOX_GUEST` | yes | VirtualBox Guest Additions |
| `ENABLE_LINUX_COMPAT` | yes | Linux syscall compatibility |
| `ENABLE_UNIX_COMPAT` | yes | Unix compatibility layer |
| `ENABLE_POSIX_SIGNALS` | yes | POSIX signal semantics |
| `ENABLE_POSIX_TERMIOS` | yes | POSIX terminal I/O |

---

## Kernel Initialization Sequence

The boot sequence starts at `startk()` (`src/kernel.c:1367`) after the bootloader (ForeB or GRUB) transfers control. The sequence is carefully ordered to ensure each subsystem can depend on its prerequisites:

### Phase 1: Early Boot (pre-VMM)

1. **Disable interrupts** -- Prevent any interrupt-driven code from running before the IDT is ready.
2. **Save multiboot info** -- Multiboot magic and MBI address are saved immediately to globals to survive stack corruption.
3. **GDT initialization** -- Sets up the Global Descriptor Table for flat-memory model.
4. **Early interrupt init** -- Minimal IDT setup with safe interrupt stubs.
5. **Debug logging init** -- Early serial/file-based debug log.
6. **Parse kernel command line** -- Extracts `quiet`, `silent`, `embedded`, `nofb`, `livecd`, `video=WIDTHxHEIGHTxBPP` tokens from multiboot.
7. **Parse multiboot framebuffer** -- Extracts framebuffer physical address, dimensions, and BPP from Multiboot1/2 tags *before* VMM init.
8. **Display early info** -- Prints kernel version, CPU info, and framebuffer details.

### Phase 2: Memory Subsystem

9. **Init system** -- Sets up display configuration defaults.
10. **Full interrupt init** -- Completes IDT, PIC/APIC setup.
11. **FPU initialization** -- Enables floating-point hardware if present.
12. **Syscall init** -- Installs the syscall entry point (int 0x80 / syscall).
13. **Hardware detection** -- CPUID-based feature detection.
14. **Driver core** -- Initializes the driver registry and bus abstraction.
15. **Memory validation** -- Validates memory map integrity.
16. **Memory init** -- `memory_init()` parses the multiboot memory map, sets up physical-to-virtual mapping, and enables paging.
17. **Framebuffer finalization** -- Maps the framebuffer to virtual address 0xF0000000 after VMM is live.
18. **Reserve initrd** -- Marks initrd memory region as reserved in the PMM.
19. **Framebuffer console** -- Initializes the framebuffer TTY with ANSI support (or falls back to VGA text mode).
20. **GL software renderer** -- Optionally initializes the software OpenGL renderer.
21. **Memory region manager** -- Intelligent region tracking.
22. **Page fault recovery** -- Graceful page fault handling.
23. **Bitmap PMM** -- Bitmap-based physical memory manager initialization with test.
24. **Enhanced Memory v2.0**:
    - Memory layout analysis
    - Memory statistics
    - Paging mode manager
    - TLB management
    - Memory protection (NX, SMEP, SMAP, PAT)
    - Stack protection (canaries)
    - Stack smashing protection (SSP)
    - Secure VMM
    - Memory corruption detection
25. **Enhanced heap allocator** -- Configurable heap with corruption detection and fragmentation mitigation.
26. **Advanced memory features** -- Copy-on-Write, Swap, memory statistics snapshot.

### Phase 3: Core Kernel

27. **Task management** -- `tasks_init()` creates the process table and scheduler.
28. **ACPI discovery** -- Parses ACPI tables via uACPI (with timeout protection).
29. **SMP CPU discovery** -- Reads ACPI MADT; Application Processors are started via INIT-SIPI-SIPI sequence.
30. **PCI enumeration** -- Discovers PCI/PCIe devices.
31. **ATA detection** -- Probes legacy ATA/IDE controllers.
32. **VirtualBox Guest** -- Detects and initializes VirtualBox Guest Additions.

### Phase 4: Devices and Filesystems

33. **Input event multiplexer** -- Sets up the input event pipeline.
34. **Hotkey manager** -- Global hotkey registration.
35. **DevFS** -- /dev filesystem with device nodes:
    - Input devices (/dev/kbd, /dev/mouse)
    - Timer devices (/dev/timer, /dev/rtc)
    - Framebuffer info (/dev/fb_width, etc.)
    - PCI devices (/dev/pci/*)
    - Block devices (/dev/sd*, /dev/loop*)
36. **USB subsystem** -- USB host controller and device enumeration.
37. **PS/2 controller** -- Reset, self-test, and initialization.
38. **PS/2 keyboard** -- Driver init, IRQ handler registration (IRQ1).
39. **PS/2 mouse** -- Driver init, IRQ handler registration (IRQ12).
40. **PS/2 watchdog** -- Hotplug recovery for PS/2 disconnects.

### Phase 5: Filesystem and Userspace

41. **Initrd** -- Parses the USTAR initrd image from memory.
42. **VFS mount** -- Mounts the root filesystem from the initrd.
43. **Timer init** -- 100 Hz PIT/APIC timer for task scheduling.
44. **Epoch init** -- Wall-clock time from RTC hardware.
45. **Sound subsystem** -- Audio driver initialization (if enabled).
46. **Lock debugging** -- Lock contention tracking.
47. **ELF loader validation** -- Self-test of the ELF loader.

### Phase 6: Finalization

48. **Enable interrupts** -- `sti` allows timer IRQ to fire, enabling the scheduler.
49. **Splash cleanup** -- Stops the boot splash and fades out.
50. **TTY mode switch** -- Exits boot mode, switches framebuffer TTY to normal operation.
51. **Idle loop** -- Enters `hlt`-based idle loop (kernel-only build).

---

## Interrupt and Exception Handling

Fern implements a comprehensive interrupt architecture that scales from the legacy 8259A PIC to the modern APIC/IOAPIC system:

### Interrupt Controllers

- **8259A PIC**: Legacy dual-chip interrupt controller for basic IRQ routing (IRQ 0-15).
- **Local APIC**: Per-CPU interrupt controller for IPIs and timer interrupts.
- **IOAPIC**: System-level interrupt routing for device IRQs.

### IDT Setup

The Interrupt Descriptor Table supports up to 256 entries:
- **Vectors 0-31**: CPU exceptions (divide error, page fault, general protection, etc.)
- **Vectors 32+**: Hardware IRQs (offset by `IRQ_BASE_OFFSET = 32`)
- **Vector 0x80**: System call entry (int 0x80 for 32-bit, `syscall` for 64-bit)
- **Vector 0xFF**: Spurious interrupt handler

### Interrupt Features

- **Vector allocation**: Dynamic allocation of interrupt vectors for device drivers.
- **Priority management**: Per-interrupt priority levels for nested handling.
- **EOI management**: Proper End-of-Interrupt signaling to PIC/APIC.
- **Statistics**: Runtime interrupt count tracking and profiling.
- **Stack switching**: Automatic stack switch on interrupt entry.
- **NMI handling**: Non-maskable interrupt support for hardware errors.

### Exception Handling

CPU exceptions are dispatched through the IDT to handlers in `exceptions.c`. Page faults go through a recovery system (`page_fault_recovery.c`) that can handle:
- Copy-on-write faults (for `fork()`)
- Demand paging (lazy allocation)
- User-space memory violations

---

## Kernel Internal Libraries

### `libs/forestcore/` -- ForestCore Runtime

Low-level kernel runtime helpers that don't belong in the exported C library:
- `types.h`, `system.h`, `net.h` -- Forest-specific type definitions and system helpers
- MMIO and I/O port access wrappers
- Audio, string, and utility functions

### `libs/libc/` -- C Standard Library

The POSIX-oriented C library targeting the Forest OS ABI. Aligned with ISO C headers; provides the userspace API surface.

### `libs/leafgfx/` -- LeafGFX Graphics Library

A self-contained 2D graphics library used by the graphics subsystem:
- BMP image loading (`leafgfx_bmp.c`)
- TrueType font rendering (`leafgfx_ttf.c`, `leafgfx_ttf_raster.c`)
- Bitmap font rendering (`leafgfx_font.c`)
- Animation support (`leafgfx_anim.c`)
- Input handling (`leafgfx_input.c`)
- Modern rendering pipeline (`leafgfx_modern.c`)

### `libs/leafui/` -- LeafUI Widget Framework

Header-only UI widget framework for building graphical interfaces within the kernel (panic UI, login screen, etc.).

### `libs/uacpi/` -- uACPI

Third-party ACPI implementation pulled in as a subtree. Provides table parsing, AML evaluation, and ACPI device enumeration.

### `libs/qrcodegen/` -- QR Code Generator

Minimal QR code generation library, likely used for boot-time display or debugging.

### `forestlibs/` -- Exported Libraries

Snapshot of the exported libc for distribution to userspace.

---

## The Initrd Filesystem

The initial ramdisk (initrd) is the root filesystem loaded into memory by the bootloader. It provides the minimal environment needed to boot to a working system:

```
initrd/
├── bin/          # Essential binaries (init, shell, utilities)
├── dev/          # Device nodes (populated by DevFS at runtime)
├── etc/          # Configuration files (/etc/livecd marker, auth config)
├── proc/         # Mount point for procfs (/proc)
├── tmp/          # Temporary files
├── usr/          # Shared resources
│   └── share/
│       └── images/icons/   # Icon theme (hicolor scalable SVGs)
└── var/          # Variable data
```

The initrd is packed as a USTAR archive and embedded in the kernel image or loaded by the bootloader as a Multiboot module. The kernel's `ramdisk_init()` function locates it in memory (via multiboot info), and `vfs_init()` mounts it as the root filesystem.

The initrd includes a comprehensive icon theme with scalable SVG icons for:
- Application types (executables, scripts, documents, archives)
- File types (text, images, audio, video, fonts)
- Device categories (drives, printers, cameras, audio)
- Filesystem locations (home, desktop, trash, folders)

---

## Supported Architectures

Fern supports multiple CPU architectures through a combination of architecture-specific source directories, conditional compilation, and linker scripts:

| Architecture | ARCH Value | Toolchain | Boot Support |
|---|---|---|---|
| **x86 32-bit** | `32` | `i686-forestos` | BIOS (Multiboot), UEFI |
| **x86-64** | `64` | `x86_64-forestos` | BIOS (Multiboot), UEFI |
| **ARM32** | `arm` | `arm-none-eabi` | BIOS (placeholder), UEFI |
| **AArch64** | `aarch64` | `aarch64-linux-gnu` | BIOS, UEFI |
| **RISC-V 64** | `riscv64` | `riscv64-unknown-elf` | BIOS, UEFI |

### How Multi-Arch is Handled

1. **Architecture detection**: `build/config.mk` reads the `ARCH` variable and sets `TARGET_ARCH` and `TARGET_TUPLE` accordingly.

2. **Source selection**: Architecture-specific source files are organized in dedicated directories:
   - `src/aarch64/` -- AArch64-specific code (boot, interrupts, timer, sound stubs)
   - `src/arm32/` -- ARM 32-bit code
   - `src/riscv64/` -- RISC-V 64-bit code
   - `src/x86_64/` -- x86-64 specific code (IST handling)
   - `src/arch/` -- Cross-architecture wrapper layer

3. **Conditional compilation**: `#ifdef __x86_64__`, `#if ARCH_64BIT`, and similar guards in C code select architecture-appropriate implementations. For example, `kernel.c` uses 64-bit page table manipulation on x86-64 and 32-bit paging on i686.

4. **Linker scripts**: Different linker scripts handle memory layout differences:
   - `link.ld` -- 32-bit BIOS
   - `link64.ld` -- 64-bit BIOS
   - `link_uefi_32.ld`, `link_uefi_64.ld` -- UEFI variants
   - `link_uefi_aarch64.ld` -- AArch64 UEFI

5. **Assembly**: Architecture-specific assembly files (`boot.asm`, `boot64.asm`, `context_switch.asm`, `interrupt_stubs.asm`) provide the lowest-level entry points and context switch code. The Makefile selects the correct variant based on `ARCH`.

6. **Build targets**: Top-level Make targets (`build32`, `build64`, `buildarm`, `buildaarch64`, `buildriscv64`) recursively invoke make with the appropriate `ARCH=` override.

7. **Cross-architecture interpreter**: An optional subsystem (`ENABLE_CROSSARC_INTERPRETER`) can interpret instructions from other architectures, enabling binary compatibility scenarios.

---

## Key Design Decisions and Trade-offs

### Monolithic vs. Microkernel

Fern chose a **monolithic** design. While microkernels (like Minix or seL4) offer better isolation, a monolithic design:
- Makes DMA and buffer sharing between drivers trivial
- Avoids the performance overhead of IPC for frequent operations
- Aligns with the Linux/Unix heritage the project targets
- Is significantly simpler to implement for a small team

The trade-off is that a buggy driver can crash the whole kernel, which is why Fern invests heavily in memory protection (SMEP/SMAP, stack canaries, page fault recovery, guard pages).

### In-Kernel Display Server

Fern includes an in-kernel Wayland compositor and X11 server. This is unusual -- most Unix-like OSes run display servers in userspace. The rationale:
- Simplifies the boot-to-desktop path (no userspace needed for graphics)
- Provides a graphical panic UI and login screen without userspace dependencies
- The kernel is the only thing that needs to run for a working system

The trade-off is increased kernel complexity and attack surface. This is mitigated by making the display server optional via feature flags.

### Configurable Feature Flags

With 180+ feature flags, Fern can be built as a minimal embedded kernel (~8 MB RAM) or a full-featured desktop kernel. Each flag gates specific source files via `build/features/*.mk`, keeping compile times reasonable for minimal builds.

### Embedded Mode

The `embedded` kernel command-line token activates reduced memory profiles:
- Minimum RAM drops from 64 MB to 8 MB
- Max heap reduced from 128 MB to 8 MB
- Heap expansion increment reduced from 64 KB to 16 KB
- Corruption detection and fragmentation mitigation disabled
- Graphics memory pool skipped

### No Root Filesystem in Kernel

Fern ships without a userspace. The kernel boots, initializes all subsystems, and enters an idle loop. This keeps the kernel self-contained and testable independently. Userspace (ForeB, libc, applications) is layered on top separately.

### Defensive Boot Sequence

The boot sequence includes:
- Timeout protection for ACPI initialization
- Automatic fallback from framebuffer to VGA text mode
- PS/2 controller flush to clear stale firmware bytes
- Multiple memory validation stages
- Graceful handling of missing hardware (e.g., no mouse, no sound)

### Debug-Friendly

The kernel includes extensive debugging support: `CONFIG_DEBUG_BOOT` for verbose boot logging, lock debugging to detect deadlocks, memory corruption detection, stack trace support, a graphical panic UI, serial debug output, and multiple log levels.

---

## Building the Kernel

```bash
make defconfig        # Configure with sane defaults
make build            # Build the kernel binary
make iso              # Create bootable ISO
make run              # Launch in QEMU
make menuconfig       # Interactive config TUI
make configcheck      # Validate current config
```

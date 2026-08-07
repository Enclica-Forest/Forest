# Kernel Configuration

Forest OS uses a Kconfig-inspired, text-file-driven configuration system for the Fern kernel. Every subsystem -- from memory management and filesystems to graphics drivers and networking protocols -- can be independently enabled or disabled at build time.

The configuration system lives in `fern/` and revolves around three key files:

| File | Role |
|---|---|
| `conf.sh` | Configuration tool (TUI + batch modes) |
| `.forestos_config` | Active configuration (human-readable key-value) |
| `build-config.mk` | Generated Make variables consumed by the build |

---

## How the System Works

The configuration flow is simple:

```
.forestos_config  --(conf.sh --generate)--> build-config.mk
build-config.mk   --(Make includes)------> Make variables (ENABLE_*, ARCH, etc.)
build/features/*.mk                       Source-file gating (EXCLUDED_CSOURCES)
```

You never edit `build-config.mk` by hand. It is always regenerated from `.forestos_config` by `conf.sh --generate`. The Make system includes `build/config.mk` first, which loads `build-config.mk` and guarantees every boolean option has a `yes` or `no` value. Then `build/features/*.mk` fragments use these values to include or exclude source files from the kernel build.

---

## conf.sh -- The Configuration Tool

`conf.sh` is the main entry point for all configuration. It supports both interactive (TUI) and batch (non-interactive) modes.

### Modes at a Glance

| Mode | Command | What It Does |
|---|---|---|
| **Interactive TUI** | `./conf.sh` or `--menuconfig` | Opens a dialog-based menu for browsing and toggling options |
| **Sane defaults** | `./conf.sh --defconfig` | Writes `.forestos_config` with built-in defaults, then generates `build-config.mk` |
| **Re-validate** | `./conf.sh --oldconfig` | Loads existing `.forestos_config`, validates values, regenerates `build-config.mk` |
| **Regenerate** | `./conf.sh --generate` | Reads `.forestos_config` as-is, produces `build-config.mk` |
| **All off** | `./conf.sh --allnoconfig` | Sets every bool to `n` (except required-on options) |
| **All on** | `./conf.sh --allyesconfig` | Sets every bool to `y` |
| **Save/Load** | `--save [file]` / `--load [file]` | Save or load config to/from a specific file |

### Typical Workflow

```bash
./conf.sh --defconfig       # sane defaults
./conf.sh --menuconfig      # tweak interactively
./conf.sh --generate        # regenerate build-config.mk
make                        # build
```

Or via Makefile shortcuts: `make defconfig`, `make menuconfig`, `make oldconfig`, `make configcheck`.

### How Each Mode Works

**--defconfig**: Loads defaults from `CONFIG_DB`, enforces required-on options, resolves dependency chains, saves `.forestos_config`, generates `build-config.mk`.

**--menuconfig**: Loads defaults + existing config, presents a dialog TUI with 20+ categories and sub-menus. Dependencies enforced live.

**--generate**: Reads `.forestos_config` as-is, validates all values (choices, ints, bools), enforces dependencies, produces `build-config.mk` with core choices, feature/process booleans, numeric tunables, `FEATURE_FLAGS`, and derived build flags.

---

## The .forestos_config File Format

`.forestos_config` is a plain text file with `CONFIG_NAME=value` lines. Comments start with `#`.

```ini
# Target Architecture
BUILD_ARCH=32

# Virtual Memory Paging
ENABLE_PAGING=y

# Default framebuffer width
DISPLAY_DEFAULT_WIDTH=1024
```

Key points:
- Keys are prefixed with `CONFIG_` in the file (stripped internally).
- Boolean values: `y` or `n`.
- Choice values: strings like `32`, `64`, `bios`, `uefi`, `debug`.
- Integer values: non-negative numbers.

---

## Feature Classes

Options in `conf.sh` have a **class** (6th field in `CONFIG_DB`):

- **`feature`** (default): Emitted as `NAME := yes/no` AND `-DNAME` in `FEATURE_FLAGS`. C code uses `#ifdef ENABLE_FOO`.
- **`process`**: Emitted as `NAME := yes/no` only. No `-D` define. Controls build behavior (e.g., `VERBOSE`, `WERROR`).

---

## Complete Feature Flag Reference

### Core Choices

| Flag | Type | Default | Description |
|---|---|---|---|
| `BUILD_ARCH` | choice | `32` | Target: `32`, `64` |
| `BUILD_BOOT_MODE` | choice | `bios` | Boot firmware: `bios`, `uefi` |
| `BUILD_TYPE` | choice | `debug` | Optimization: `debug`, `release`, `optimize` |
| `BUILD_PARALLEL_JOBS` | int | `0` | Parallel make jobs (0=auto) |

### Architecture and Boot

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_SMP` | `y` | -- | Symmetric multiprocessing |
| `ENABLE_PAE` | `n` | -- | Physical Address Extension (>4GB RAM, 32-bit) |
| `ENABLE_FPU` | `y` | -- | Floating-point unit |
| `ENABLE_X86_IST` | `y` | -- | Interrupt Stack Table (64-bit) |
| `ENABLE_CROSSARC_INTERPRETER` | `n` | -- | Cross-architecture ELF interpreter |
| `ENABLE_FOREB_BOOTLOADER` | `y` | -- | ForeB bootloader |
| `ENABLE_UEFI_SECURE_BOOT` | `n` | -- | UEFI Secure Boot |

### Memory Management

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_PAGING` | `y` | -- | Virtual memory with page tables (**required**) |
| `ENABLE_SLAB` | `y` | -- | Kernel object allocator |
| `ENABLE_MEMORY_PROTECTION` | `y` | -- | Memory access protection |
| `ENABLE_GUARD_PAGES` | `y` | -- | Stack overflow detection |
| `ENABLE_ASLR` | `n` | -- | Address Space Layout Randomization |
| `ENABLE_NX_BIT` | `y` | -- | No-Execute bit support |
| `ENABLE_COW` | `n` | -- | Copy-on-Write for fork() |
| `ENABLE_SWAP` | `n` | -- | Virtual memory swapping |
| `ENABLE_PAGE_CACHE` | `n` | -- | Unified page cache |
| `ENABLE_OOM_KILLER` | `n` | -- | Out-of-Memory killer |
| `ENABLE_MEMORY_RECLAIM` | `n` | -- | LRU-based memory reclaim |
| `ENABLE_MEMORY_STATS` | `n` | -- | Memory statistics |
| `ENABLE_TLB_SHOOTDOWN` | `n` | -- | SMP TLB shootdown via IPI |
| `ENABLE_MEMORY_CORRUPTION_DETECTION` | `y` | -- | Heap corruption canaries |

### Filesystems

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_VFS` | `y` | -- | Virtual File System layer |
| `ENABLE_EXT2` | `y` | VFS | EXT2 filesystem |
| `ENABLE_FAT32` | `y` | VFS | FAT32 filesystem |
| `ENABLE_EXFAT` | `y` | VFS | exFAT filesystem |
| `ENABLE_ISO9660` | `y` | VFS | ISO9660 (CD-ROM) |
| `ENABLE_UDF` | `n` | VFS | UDF (DVD/Blu-ray) |
| `ENABLE_LEAN` | `n` | VFS | LEAN filesystem |
| `ENABLE_YAFFS` | `n` | VFS | YAFFS (NAND flash) |
| `ENABLE_JFFS2` | `n` | VFS | JFFS2 (flash) |
| `ENABLE_FFS_AMIGA` | `n` | VFS | Amiga Fast File System |
| `ENABLE_ZDSFS` | `n` | VFS | IBM z/OS dataset FS |
| `ENABLE_PROCFS` | `y` | VFS | `/proc` virtual filesystem |
| `ENABLE_SYSFS` | `y` | VFS | `/sys` virtual filesystem |
| `ENABLE_DEVFS` | `y` | VFS | `/dev` device filesystem |
| `ENABLE_TMPFS` | `y` | VFS | Temporary in-memory FS |
| `ENABLE_RAMDISK` | `y` | VFS | Ramdisk from initrd |
| `ENABLE_SYMLINKS` | `y` | VFS | Symbolic link support |

### Graphics and Display

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_GRAPHICS` | `y` | -- | Graphics subsystem |
| `ENABLE_VESA` | `y` | Graphics | VESA framebuffer driver |
| `ENABLE_FRAMEBUFFER` | `y` | Graphics | Framebuffer support |
| `ENABLE_VGA_TEXT` | `y` | -- | VGA text mode driver |
| `ENABLE_CONSOLE` | `y` | -- | Kernel console output |
| `ENABLE_BOCHS_BGA` | `y` | Graphics | Bochs/QEMU/VBox graphics |
| `ENABLE_VMWARE_SVGA` | `y` | Graphics | VMware SVGA-II driver |
| `ENABLE_INTEL_HD` | `y` | Graphics | Intel integrated graphics |
| `ENABLE_NVIDIA_GPU` | `y` | Graphics | NVIDIA driver |
| `ENABLE_AMD_GPU` | `y` | Graphics | AMD/ATI driver |
| `ENABLE_GPU_ACCEL` | `y` | Graphics | GPU hardware acceleration |
| `ENABLE_FONT_RENDERER` | `y` | Graphics | Kernel font renderer |
| `ENABLE_TRUETYPE` | `y` | Graphics | TrueType font support |
| `ENABLE_WINDOW_MANAGER` | `y` | Graphics | Kernel window manager |
| `ENABLE_COMPOSITOR` | `y` | Graphics | Display compositor |
| `ENABLE_DOUBLE_BUFFERING` | `y` | Graphics | Double buffering |
| `ENABLE_SPLASH_SCREEN` | `y` | Graphics | Boot splash screen |
| `ENABLE_PANICUI` | `y` | Graphics | Graphical panic screen |
| `ENABLE_DISPLAY_MANAGER` | `y` | Graphics | Display manager (CGDM) |
| `ENABLE_WAYLAND_SERVER` | `y` | Graphics | Wayland protocol |
| `ENABLE_X11_SERVER` | `y` | Graphics | X11 display server |
| `ENABLE_CLIPBOARD` | `y` | Graphics | Clipboard support |
| `ENABLE_DRAG_DROP` | `y` | Graphics | Drag and drop |
| `ENABLE_LEAFGFX` | `y` | Graphics | LeafGFX graphics library |
| `ENABLE_HARDWARE_DETECT` | `y` | Graphics | GPU auto-detection |
| `ENABLE_VSYNC` | `y` | Graphics | Vertical sync |
| `ENABLE_OPENGL` | `y` | -- | OpenGL software renderer |

### Networking (all off by default)

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_NETWORKING` | `n` | -- | TCP/IP stack |
| `ENABLE_ETHERNET` | `n` | Networking | Ethernet support |
| `ENABLE_TCP` | `n` | Networking | TCP protocol |
| `ENABLE_UDP` | `n` | Networking | UDP protocol |
| `ENABLE_DHCP` | `n` | Networking | DHCP client |
| `ENABLE_DNS` | `n` | Networking | DNS resolver |
| `ENABLE_ARP` | `n` | Networking | ARP protocol |
| `ENABLE_ICMP` | `n` | Networking | ICMP (ping) |
| `ENABLE_DRIVER_E1000` | `n` | Networking | Intel E1000 NIC |
| `ENABLE_DRIVER_RTL8139` | `n` | Networking | Realtek RTL8139 NIC |
| `ENABLE_DRIVER_NE2000` | `n` | Networking | NE2000 NIC |

### Audio (all off by default)

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_AUDIO` | `n` | -- | Audio subsystem |
| `ENABLE_SOUND_SB16` | `n` | Audio | Sound Blaster 16 |
| `ENABLE_SOUND_SBPRO` | `n` | Audio | Sound Blaster Pro |
| `ENABLE_SOUND_AC97` | `n` | Audio | AC97 audio |
| `ENABLE_SOUND_HDA` | `n` | Audio | Intel HDA |
| `ENABLE_SOUND_ENSONIQ` | `n` | Audio | Ensoniq AudioPCI |
| `ENABLE_SOUND_OPL3` | `n` | Audio | Yamaha OPL3 FM synth |
| `ENABLE_SOUND_PC_SPEAKER` | `y` | Audio | PC speaker |
| `ENABLE_SOUND_USB` | `n` | Audio | USB audio class |
| `ENABLE_AUDIO_WAV` | `y` | Audio | WAV file playback |
| `ENABLE_AUDIO_VORBIS` | `n` | Audio | Ogg Vorbis decoder |

### Security

| Flag | Default | Description |
|---|---|---|
| `ENABLE_SMEP_SMAP` | `y` | Supervisor Mode Execution/Access Prevention |
| `ENABLE_STACK_PROTECTION` | `y` | Stack canaries |
| `ENABLE_LOCK_DEBUGGING` | `y` | Lock contention and deadlock detection |
| `ENABLE_AUTH` | `y` | User authentication |
| `ENABLE_SESSION_MANAGEMENT` | `y` | TTY session management |
| `ENABLE_KERNEL_WATCHDOG` | `y` | Per-task watchdog |
| `ENABLE_FAULT_PREVENTION` | `y` | Fault handler stacks and recovery |
| `ENABLE_MEMORY_VALIDATION` | `y` | Memory address validation |

### USB (off by default)

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_USB` | `n` | -- | USB subsystem |
| `ENABLE_USB_UHCI` | `y` | USB | USB 1.0 UHCI controller |
| `ENABLE_USB_OHCI` | `y` | USB | USB 1.0 OHCI controller |
| `ENABLE_USB_EHCI` | `y` | USB | USB 2.0 EHCI controller |
| `ENABLE_USB_XHCI` | `y` | USB | USB 3.0 xHCI controller |
| `ENABLE_USB_HID` | `y` | USB | USB keyboard/mouse |
| `ENABLE_USB_HUB` | `y` | USB | USB hub support |
| `ENABLE_USB_MASS_STORAGE` | `y` | USB | USB mass storage |

### Storage

| Flag | Default | Description |
|---|---|---|
| `ENABLE_ATA` | `y` | IDE storage controller |
| `ENABLE_AHCI` | `y` | AHCI SATA controller |
| `ENABLE_NVME` | `y` | NVMe SSD controller |
| `ENABLE_SCSI` | `n` | SCSI controller |
| `ENABLE_FDC` | `n` | Floppy disk controller |
| `ENABLE_BLOCK_DEVICES` | `y` | Block device subsystem |
| `ENABLE_LOOP_DEVICES` | `y` | Loop device support |

### Input Devices

| Flag | Default | Parent | Description |
|---|---|---|---|
| `ENABLE_PS2` | `y` | -- | PS/2 controller |
| `ENABLE_PS2_KEYBOARD` | `y` | PS/2 | PS/2 keyboard |
| `ENABLE_PS2_MOUSE` | `y` | PS/2 | PS/2 mouse |
| `ENABLE_PS2_WATCHDOG` | `y` | PS/2 | PS/2 hotplug watchdog |
| `ENABLE_GAMEPORT` | `y` | -- | Gameport/joystick |
| `ENABLE_INPUT_EVENT_SYSTEM` | `y` | -- | Unified input events |

### IPC

| Flag | Default | Description |
|---|---|---|
| `ENABLE_IPC` | `y` | IPC subsystem |
| `ENABLE_DBUS` | `y` | D-Bus message protocol |
| `ENABLE_SEMAPHORES` | `y` | Kernel semaphores |
| `ENABLE_BARRIERS` | `y` | Barrier synchronization |
| `ENABLE_SYSV_SEM` | `y` | System V semaphore sets |
| `ENABLE_SYSV_MSG` | `y` | System V message queues |
| `ENABLE_POSIX_SHM` | `y` | POSIX shared memory |
| `ENABLE_EPOLL` | `y` | epoll I/O multiplexing |
| `ENABLE_INOTIFY` | `y` | inotify file monitoring |
| `ENABLE_EVENTFD` | `y` | eventfd notification |
| `ENABLE_SIGNALFD` | `y` | signalfd interface |
| `ENABLE_TIMERFD` | `y` | timerfd interface |

### Timers

| Flag | Default | Description |
|---|---|---|
| `ENABLE_PIT` | `y` | Programmable Interval Timer |
| `ENABLE_HPET` | `n` | High Precision Event Timer |
| `ENABLE_APIC_TIMER` | `y` | Local APIC timer |
| `ENABLE_CMOS_RTC` | `y` | Real-time clock |
| `ENABLE_TSC` | `y` | Time Stamp Counter |
| `ENABLE_TIMER_ABSTRACTION` | `y` | Unified timer layer |
| `ENABLE_EPOCH_TIME` | `y` | Unix epoch time |

### Interrupts

| Flag | Default | Description |
|---|---|---|
| `ENABLE_PIC_8259A` | `y` | Legacy PIC driver |
| `ENABLE_LOCAL_APIC` | `y` | Local APIC driver |
| `ENABLE_IOAPIC` | `y` | I/O APIC driver |
| `ENABLE_MSI` | `n` | Message Signaled Interrupts |
| `ENABLE_NMI_HANDLER` | `y` | Non-Maskable Interrupt handler |
| `ENABLE_INTERRUPT_PRIORITY` | `y` | Interrupt priority management |
| `ENABLE_INTERRUPT_STATISTICS` | `y` | Interrupt statistics |
| `ENABLE_INTERRUPT_VECTOR_ALLOCATION` | `y` | Dynamic vector allocation |
| `ENABLE_INTERRUPT_EOI_MANAGEMENT` | `y` | Centralized EOI management |

### Scheduler

| Flag | Default | Description |
|---|---|---|
| `ENABLE_ELF_LOADER` | `y` | ELF binary loader |
| `ENABLE_LDSO` | `y` | Dynamic linker (ld.so) |
| `ENABLE_JOB_CONTROL` | `y` | POSIX job control |
| `ENABLE_SIGNALS` | `y` | POSIX signal delivery |
| `ENABLE_IDLE_TASK` | `y` | Dedicated idle task |

### Hardware

| Flag | Default | Description |
|---|---|---|
| `ENABLE_PCI` | `y` | PCI bus support |
| `ENABLE_PCIE` | `y` | PCIe capability detection |
| `ENABLE_ACPI` | `y` | ACPI support |
| `ENABLE_SERIAL` | `y` | Serial port (UART) |
| `ENABLE_PARALLEL` | `n` | Parallel port (LPT) |
| `ENABLE_A20` | `y` | A20 address line (**required**) |
| `ENABLE_VIRTUALBOX_GUEST` | `n` | VirtualBox Guest Additions |
| `ENABLE_CHAR_DEVICES` | `y` | `/dev/null`, `/dev/zero`, etc. |
| `ENABLE_TTY` | `y` | Terminal subsystem (**required**) |

### Debugging

| Flag | Default | Description |
|---|---|---|
| `ENABLE_DEBUG_SYMBOLS` | `y` | Debug info in kernel |
| `ENABLE_KERNEL_DEBUG` | `y` | Kernel debugging facilities |
| `ENABLE_SERIAL_DEBUG` | `y` | Serial port debug output |
| `ENABLE_PANIC_BACKTRACES` | `y` | Stack traces on panic |
| `ENABLE_ASSERTIONS` | `y` | Runtime assertion checking |
| `ENABLE_MEMORY_DEBUG` | `n` | Memory leak detection |

### Compatibility

| Flag | Default | Description |
|---|---|---|
| `ENABLE_LINUX_COMPAT` | `y` | Linux binary compatibility |
| `ENABLE_UNIX_COMPAT` | `y` | Unix/POSIX compatibility |
| `ENABLE_POSIX_SIGNALS` | `y` | POSIX signal handling |
| `ENABLE_POSIX_TERMIOS` | `y` | POSIX terminal I/O |
| `ENABLE_PLATFORM_DETECTION` | `y` | Runtime platform detection |
| `HYPERVISOR_DETECTION` | `y` | VM detection |
| `ROOT_AUTOLOGIN` | `n` | Auto-login as root at boot |

### Build System

| Flag | Default | Description |
|---|---|---|
| `QEMU_MEMORY` | `512` | QEMU guest RAM (MB) |
| `QEMU_ENABLE_KVM` | `y` | KVM acceleration |
| `QEMU_NETWORK` | `y` | Network emulation |
| `QEMU_USB` | `y` | USB emulation |
| `GENERATE_CHECKSUMS` | `y` | SHA256 checksums |
| `ENABLE_TESTING` | `n` | QEMU boot tests after build |
| `CLEAN_BEFORE_BUILD` | `y` | Clean before building |
| `VERBOSE` | `n` | Full compiler commands |
| `WERROR` | `n` | Warnings as errors |

### Numeric Tunables

| Tunable | Default | Description |
|---|---|---|
| `VFS_MAX_PATH` | `256` | Maximum path length |
| `VFS_MAX_OPEN_FILES` | `256` | Max open files per process |
| `VFS_MAX_MOUNTS` | `16` | Maximum mount points |
| `DISPLAY_DEFAULT_WIDTH` | `1024` | Default framebuffer width |
| `DISPLAY_DEFAULT_HEIGHT` | `768` | Default framebuffer height |
| `DISPLAY_DEFAULT_BPP` | `32` | Default bits per pixel |
| `NET_MAX_SOCKETS` | `16` | Maximum concurrent sockets |
| `TCP_MAX_CONNECTIONS` | `16` | Maximum TCP connections |
| `TCP_WINDOW_SIZE` | `8192` | TCP receive window size |
| `AUDIO_MAX_STREAMS` | `8` | Maximum concurrent audio streams |
| `AUDIO_RING_BUFFER_SIZE` | `32768` | Audio ring buffer (bytes) |
| `AUDIO_DEFAULT_SAMPLE_RATE` | `44100` | Default sample rate (Hz) |
| `SECURITY_AUTH_MAX_USERS` | `16` | Maximum registered users |
| `SECURITY_AUTH_MAX_GROUPS` | `8` | Maximum user groups |
| `SECURITY_MAX_TTY_SESSIONS` | `22` | Maximum TTY sessions |
| `USB_MAX_CONTROLLERS` | `8` | Maximum USB controllers |
| `USB_MAX_DEVICES` | `128` | Maximum USB devices |
| `MAX_BLOCK_DEVICES` | `64` | Maximum block devices |
| `IPC_MAX_CHANNELS` | `32` | Maximum IPC channels |
| `POSIX_SHM_MAX_OBJECTS` | `64` | Maximum POSIX SHM objects |
| `PIT_DEFAULT_FREQUENCY` | `1000` | PIT tick frequency (Hz) |
| `DEBUG_LOG_LEVEL` | `3` | Kernel log verbosity (0-5) |
| `MAX_STACK_FRAMES` | `32` | Max frames in backtrace |
| `PANIC_MAX_STACK_FRAMES` | `16` | Max frames in panic backtrace |
| `SERIAL_BAUD_RATE` | `38400` | Serial port baud rate |
| `TTY_MAX_VIRTUAL_TTYS` | `64` | Maximum virtual terminals |
| `INTERRUPT_MAX_NESTING_DEPTH` | `8` | Max nested IRQ depth |
| `SCHED_PRIORITY_LEVELS` | `8` | Scheduler priority levels |
| `MAX_PROCESSES` | `4096` | Maximum concurrent processes |
| `USER_STACK_PAGES` | `32` | Pages per user stack |
| `MAX_PIPES` | `16` | Maximum pipe objects |
| `MAX_PTYS` | `16` | Maximum pseudo-terminals |
| `KERNEL_HEAP_INITIAL_SIZE` | `4194304` | Initial kernel heap (bytes) |
| `KERNEL_HEAP_MAX_SIZE` | `134217728` | Maximum kernel heap (bytes) |
| `KERNEL_STACK_SIZE` | `16384` | Per-task kernel stack (bytes) |

---

## How Configuration Affects the Build

### Source Gating

The `build/features/*.mk` fragments control compilation via `EXCLUDED_CSOURCES`:

1. `build/kernel-sources.mk` defines `CSOURCES` as all `.c` files in `src/`.
2. Each feature fragment appends to `EXCLUDED_CSOURCES` when its feature is `no`.
3. `CSOURCES` is computed as `$(filter-out $(EXCLUDED_CSOURCES), $(wildcard ...))`.

Parent-child relationships are transitive: disabling `ENABLE_VFS=no` disables all filesystem children.

### Feature Flags in C Code

When a `feature`-class option is `yes`, `FEATURE_FLAGS` includes `-DENABLE_FOO`. C code uses conditional compilation:

```c
#ifdef ENABLE_EXT2
    // ext2 mount support
#endif
```

---

## Default vs Custom Configuration

Running `./conf.sh --defconfig` writes `.forestos_config` with sensible defaults:
- **On**: Paging, SLAB, protection, guard pages, VFS, EXT2/FAT32/exFAT, full graphics stack, ATA/AHCI/NVMe, all security features, all debug features.
- **Off**: ASLR, COW, swap, networking, audio, USB, SCSI, floppy, HPET, MSI.

### Required-On Options

These cannot be turned off, even with `--allnoconfig`: `ENABLE_PAGING`, `ENABLE_A20`, `ENABLE_TTY`.

### Dependency System

Features have parent dependencies (3rd field in `CONFIG_DB`). Disabling a parent forces all children to `n` transitively. This prevents impossible configurations like EXT2 without VFS.

---

## Configuration Presets (makeconfigs/)

The `fern/makeconfigs/` directory mirrors `build/` with its own `config.mk`, `dirs.mk`, `flags.mk`, and `features/` subdirectory. It can serve as a reference implementation, alternative configuration base, or starting point for custom builds.

---

## Configuration Validation

- **conf.sh**: Validates choices against allowed values, ints non-negative, bools `y`/`n`. Invalid values reset to defaults.
- **build/config.mk**: Validates `ARCH` (`32 64 arm aarch64 riscv64`), `BOOT_MODE` (`bios uefi`), `BUILD_TYPE` (`debug release optimize`). Any undefined boolean forced to `no`.
- **configcheck**: `make configcheck` prints effective config and counts enabled features.

---

## Tips for Optimal Configuration

**Development**: `BUILD_TYPE=debug` + `ENABLE_DEBUG_SYMBOLS` + `ENABLE_SERIAL_DEBUG` + `VERBOSE=yes`.

**Minimal embedded**: `--allnoconfig`, then enable only paging, VFS, console.

**Desktop**: `--defconfig`, then enable networking, audio, USB, OpenGL.

**Production**: Disable debug features, use `BUILD_TYPE=optimize`, reduce heap sizes for constrained hardware.

| Profile | Key Settings |
|---|---|
| Minimal | `--allnoconfig` + paging, VFS, console |
| Default | `--defconfig` |
| Desktop | `--defconfig` + networking, audio, USB, OpenGL |
| Server | `--defconfig` + networking, no graphics/audio |
| Development | `--defconfig` + debug build, serial debug |
| Maximum | `--allyesconfig` (everything on, largest binary) |

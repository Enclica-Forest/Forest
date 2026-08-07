#!/bin/bash
# =============================================================================
# FOREST OS CONFIGURATION TUI (conf.sh) v4.0
# =============================================================================
# Interactive Terminal User Interface for Forest-OS build configuration
# Kernel-oriented (Fern) options across build + numeric tunable categories
#
# 6-field CONFIG_DB schema:
#   <type>:<default>:<parent|none>:<description>:<help>:<class|feature>
#   - type:   bool | choice | int
#   - class:  feature (default) | process  (only for bool)
#             feature  -> emitted as <NAME> := yes/no AND -D<NAME> in FEATURE_FLAGS
#             process  -> emitted as <NAME> := yes/no only (no -D define)
#   - 5-field entries default to class=feature (backward compatible)
# =============================================================================

set -euo pipefail

SCRIPT_VERSION="4.0.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/.forestos_config"
BUILD_CONFIG_FILE="$SCRIPT_DIR/build-config.mk"

TEMPFILE=$(mktemp)
MENUFILE=$(mktemp)
trap 'rm -f "$TEMPFILE" "$MENUFILE"' EXIT

# =============================================================================
# CONFIGURATION DATABASE
# =============================================================================
declare -A CONFIG_DB
declare -A CONFIG_VALUES
declare -A CHOICE_OPTIONS

# Helper to access fields safely (5 or 6 fields; missing class => feature)
# field(n): 1=type 2=default 3=parent 4=descr 5=help 6=class
cfg_field() {
    local name="$1" n="$2"
    local info="${CONFIG_DB[$name]}"
    local val
    val="$(echo "$info" | cut -d: -f"$n")"
    if [[ "$n" == "6" && -z "$val" ]]; then
        val="feature"
    fi
    echo "$val"
}

# --- 0. General Setup ---
CONFIG_DB[BUILD_ARCH]="choice:32:none:Target Architecture:Choose the target architecture for Forest OS"
CONFIG_DB[BUILD_BOOT_MODE]="choice:bios:none:Boot Mode:Select the boot firmware interface"
CONFIG_DB[BUILD_TYPE]="choice:debug:none:Build Type:Select compilation optimization and debug level"
CONFIG_DB[BUILD_PARALLEL_JOBS]="int:0:none:Parallel Build Jobs:Number of parallel make jobs (0=auto):process"

# --- 1. Architecture and Boot ---
CONFIG_DB[ENABLE_SMP]="bool:y:none:SMP Support:Enable symmetric multiprocessing for multiple CPU cores"
CONFIG_DB[ENABLE_PAE]="bool:n:none:PAE (32-bit only):Enable Physical Address Extension for >4GB RAM on 32-bit"
CONFIG_DB[ENABLE_FPU]="bool:y:none:FPU Support:Enable floating-point unit support"
CONFIG_DB[ENABLE_X86_IST]="bool:y:none:x86_64 IST:Enable Interrupt Stack Table for critical exceptions on 64-bit"
CONFIG_DB[ENABLE_CROSSARC_INTERPRETER]="bool:n:none:Cross-Arch Interpreter:Enable crossarcinterpret for foreign-architecture ELF binaries"
CONFIG_DB[ENABLE_FOREB_BOOTLOADER]="bool:y:none:ForeB Bootloader:ForeB is the Forest-OS bootloader (raw-MBR/UEFI)"
CONFIG_DB[ENABLE_UEFI_SECURE_BOOT]="bool:n:none:UEFI Secure Boot:Enable UEFI Secure Boot verification"

# --- 2. Memory Management ---
CONFIG_DB[ENABLE_PAGING]="bool:y:none:Virtual Memory Paging:Enable virtual memory management with page tables"
CONFIG_DB[ENABLE_SLAB]="bool:y:none:SLAB Allocator:Enable efficient kernel object allocation"
CONFIG_DB[ENABLE_MEMORY_PROTECTION]="bool:y:none:Memory Protection:Enable memory access protection mechanisms"
CONFIG_DB[ENABLE_GUARD_PAGES]="bool:y:none:Guard Pages:Enable guard pages for stack overflow detection"
CONFIG_DB[ENABLE_ASLR]="bool:n:none:ASLR:Enable Address Space Layout Randomization"
CONFIG_DB[ENABLE_NX_BIT]="bool:y:none:NX Bit:Enable No-Execute bit support"
CONFIG_DB[ENABLE_COW]="bool:y:none:Copy-on-Write:Enable COW for efficient fork()"
CONFIG_DB[ENABLE_SWAP]="bool:y:none:Swap Support:Enable virtual memory swapping to disk"
CONFIG_DB[ENABLE_PAGE_CACHE]="bool:y:none:Page Cache:Enable unified page cache for file I/O"
CONFIG_DB[ENABLE_OOM_KILLER]="bool:y:none:OOM Killer:Enable Out-of-Memory killer"
CONFIG_DB[ENABLE_MEMORY_RECLAIM]="bool:y:none:Memory Reclaim:Enable LRU-based memory reclaim"
CONFIG_DB[ENABLE_MEMORY_STATS]="bool:y:none:Memory Stats:Enable memory statistics and debugging"
CONFIG_DB[ENABLE_TLB_SHOOTDOWN]="bool:y:none:TLB Shootdown:Enable SMP TLB shootdown via IPI"
CONFIG_DB[ENABLE_MEMORY_CORRUPTION_DETECTION]="bool:y:none:Memory Corruption Detection:Enable heap corruption detection with canaries"

# --- 3. Filesystems ---
CONFIG_DB[ENABLE_VFS]="bool:y:none:Virtual File System:Enable the VFS layer"
CONFIG_DB[ENABLE_EXT2]="bool:y:ENABLE_VFS:EXT2:Enable EXT2 file system support"
CONFIG_DB[ENABLE_FAT32]="bool:y:ENABLE_VFS:FAT32:Enable FAT32 file system support"
CONFIG_DB[ENABLE_EXFAT]="bool:y:ENABLE_VFS:exFAT:Enable exFAT file system support"
CONFIG_DB[ENABLE_ISO9660]="bool:y:ENABLE_VFS:ISO9660:Enable ISO9660 (CD-ROM) file system"
CONFIG_DB[ENABLE_UDF]="bool:n:ENABLE_VFS:UDF:Enable UDF (DVD/Blu-ray) file system"
CONFIG_DB[ENABLE_LEAN]="bool:n:ENABLE_VFS:LEAN FS:Enable LEAN file system"
CONFIG_DB[ENABLE_YAFFS]="bool:n:ENABLE_VFS:YAFFS:Enable YAFFS (NAND flash) file system"
CONFIG_DB[ENABLE_JFFS2]="bool:n:ENABLE_VFS:JFFS2:Enable JFFS2 (flash) file system"
CONFIG_DB[ENABLE_FFS_AMIGA]="bool:n:ENABLE_VFS:Amiga FFS:Enable Amiga Fast File System"
CONFIG_DB[ENABLE_ZDSFS]="bool:n:ENABLE_VFS:z/OS DS FS:Enable IBM z/OS dataset filesystem"
CONFIG_DB[ENABLE_PROCFS]="bool:y:ENABLE_VFS:ProcFS:Enable /proc virtual filesystem"
CONFIG_DB[ENABLE_SYSFS]="bool:y:ENABLE_VFS:SysFS:Enable /sys virtual filesystem"
CONFIG_DB[ENABLE_DEVFS]="bool:y:ENABLE_VFS:DevFS:Enable /dev device filesystem"
CONFIG_DB[ENABLE_TMPFS]="bool:y:ENABLE_VFS:TmpFS:Enable temporary in-memory filesystem"
CONFIG_DB[ENABLE_RAMDISK]="bool:y:ENABLE_VFS:Ramdisk:Enable ramdisk filesystem from initrd"
CONFIG_DB[ENABLE_SYMLINKS]="bool:y:ENABLE_VFS:Symbolic Links:Enable symbolic link support"

# --- 4. Graphics and Display ---
CONFIG_DB[ENABLE_GRAPHICS]="bool:y:none:Graphics Subsystem:Enable graphics and display"
CONFIG_DB[ENABLE_VESA]="bool:y:ENABLE_GRAPHICS:VESA:Enable VESA framebuffer driver"
CONFIG_DB[ENABLE_FRAMEBUFFER]="bool:y:ENABLE_GRAPHICS:Framebuffer:Enable framebuffer support"
CONFIG_DB[ENABLE_VGA_TEXT]="bool:y:none:VGA Text:Enable VGA text mode driver"
CONFIG_DB[ENABLE_CONSOLE]="bool:y:none:Console:Enable kernel console output"
CONFIG_DB[ENABLE_BOCHS_BGA]="bool:y:ENABLE_GRAPHICS:Bochs BGA:Enable Bochs/QEMU/VirtualBox graphics driver"
CONFIG_DB[ENABLE_VMWARE_SVGA]="bool:y:ENABLE_GRAPHICS:VMware SVGA:Enable VMware SVGA-II graphics driver"
CONFIG_DB[ENABLE_INTEL_HD]="bool:y:ENABLE_GRAPHICS:Intel HD:Enable Intel integrated graphics driver"
CONFIG_DB[ENABLE_NVIDIA_GPU]="bool:y:ENABLE_GRAPHICS:NVIDIA:Enable NVIDIA graphics driver"
CONFIG_DB[ENABLE_AMD_GPU]="bool:y:ENABLE_GRAPHICS:AMD/ATI:Enable AMD/ATI graphics driver"
CONFIG_DB[ENABLE_GPU_ACCEL]="bool:y:ENABLE_GRAPHICS:GPU Acceleration:Enable GPU hardware acceleration"
CONFIG_DB[ENABLE_FONT_RENDERER]="bool:y:ENABLE_GRAPHICS:Font Renderer:Enable kernel font renderer"
CONFIG_DB[ENABLE_TRUETYPE]="bool:y:ENABLE_GRAPHICS:TrueType:Enable TrueType font support"
CONFIG_DB[ENABLE_WINDOW_MANAGER]="bool:y:ENABLE_GRAPHICS:Window Manager:Enable kernel window manager"
CONFIG_DB[ENABLE_COMPOSITOR]="bool:y:ENABLE_GRAPHICS:Compositor:Enable display compositor"
CONFIG_DB[ENABLE_DOUBLE_BUFFERING]="bool:y:ENABLE_GRAPHICS:Double Buffering:Enable double buffering"
CONFIG_DB[ENABLE_SPLASH_SCREEN]="bool:y:ENABLE_GRAPHICS:Splash Screen:Enable boot splash screen"
CONFIG_DB[ENABLE_PANICUI]="bool:y:ENABLE_GRAPHICS:Panic UI:Enable graphical panic screen"
CONFIG_DB[ENABLE_DISPLAY_MANAGER]="bool:y:ENABLE_GRAPHICS:Display Manager:Enable display manager (CGDM)"
CONFIG_DB[ENABLE_WAYLAND_SERVER]="bool:y:ENABLE_GRAPHICS:Wayland Server:Enable Wayland display server protocol"
CONFIG_DB[ENABLE_X11_SERVER]="bool:y:ENABLE_GRAPHICS:X11 Server:Enable X11 display server"
CONFIG_DB[ENABLE_CLIPBOARD]="bool:y:ENABLE_GRAPHICS:Clipboard:Enable clipboard support"
CONFIG_DB[ENABLE_DRAG_DROP]="bool:y:ENABLE_GRAPHICS:Drag and Drop:Enable drag and drop support"
CONFIG_DB[ENABLE_LEAFGFX]="bool:y:ENABLE_GRAPHICS:LeafGFX:Enable LeafGFX userspace graphics library"
CONFIG_DB[ENABLE_HARDWARE_DETECT]="bool:y:ENABLE_GRAPHICS:HW Detect:Enable GPU hardware auto-detection"
CONFIG_DB[ENABLE_VSYNC]="bool:y:ENABLE_GRAPHICS:VSync:Enable vertical sync support"

# --- 5. Networking ---
CONFIG_DB[ENABLE_NETWORKING]="bool:n:none:Networking Stack:Enable TCP/IP networking stack"
CONFIG_DB[ENABLE_ETHERNET]="bool:n:ENABLE_NETWORKING:Ethernet:Enable Ethernet support"
CONFIG_DB[ENABLE_TCP]="bool:n:ENABLE_NETWORKING:TCP:Enable TCP protocol"
CONFIG_DB[ENABLE_UDP]="bool:n:ENABLE_NETWORKING:UDP:Enable UDP protocol"
CONFIG_DB[ENABLE_DHCP]="bool:n:ENABLE_NETWORKING:DHCP:Enable DHCP client"
CONFIG_DB[ENABLE_DNS]="bool:n:ENABLE_NETWORKING:DNS:Enable DNS resolver"
CONFIG_DB[ENABLE_ARP]="bool:n:ENABLE_NETWORKING:ARP:Enable ARP protocol"
CONFIG_DB[ENABLE_ICMP]="bool:n:ENABLE_NETWORKING:ICMP:Enable ICMP (ping) support"
CONFIG_DB[ENABLE_DRIVER_E1000]="bool:n:ENABLE_NETWORKING:E1000 NIC:Enable Intel E1000 NIC driver"
CONFIG_DB[ENABLE_DRIVER_RTL8139]="bool:n:ENABLE_NETWORKING:RTL8139 NIC:Enable Realtek RTL8139 NIC driver"
CONFIG_DB[ENABLE_DRIVER_NE2000]="bool:n:ENABLE_NETWORKING:NE2000 NIC:Enable NE2000 NIC driver"

# --- 6. Audio ---
CONFIG_DB[ENABLE_AUDIO]="bool:n:none:Audio Subsystem:Enable audio subsystem"
CONFIG_DB[ENABLE_SOUND_SB16]="bool:n:ENABLE_AUDIO:Sound Blaster 16:Enable SB16 driver"
CONFIG_DB[ENABLE_SOUND_SBPRO]="bool:n:ENABLE_AUDIO:Sound Blaster Pro:Enable SB Pro driver"
CONFIG_DB[ENABLE_SOUND_AC97]="bool:n:ENABLE_AUDIO:AC97:Enable AC97 audio driver"
CONFIG_DB[ENABLE_SOUND_HDA]="bool:n:ENABLE_AUDIO:Intel HDA:Enable Intel HDA driver"
CONFIG_DB[ENABLE_SOUND_ENSONIQ]="bool:n:ENABLE_AUDIO:Ensoniq:Enable Ensoniq AudioPCI driver"
CONFIG_DB[ENABLE_SOUND_OPL3]="bool:n:ENABLE_AUDIO:OPL3:Enable Yamaha OPL3 FM synth"
CONFIG_DB[ENABLE_SOUND_PC_SPEAKER]="bool:y:ENABLE_AUDIO:PC Speaker:Enable PC speaker driver"
CONFIG_DB[ENABLE_SOUND_USB]="bool:n:ENABLE_AUDIO:USB Audio:Enable USB audio class driver"
CONFIG_DB[ENABLE_AUDIO_WAV]="bool:y:ENABLE_AUDIO:WAV Support:Enable WAV file playback"
CONFIG_DB[ENABLE_AUDIO_VORBIS]="bool:n:ENABLE_AUDIO:Vorbis:Enable Ogg Vorbis decoder"

# --- 7. Security ---
CONFIG_DB[ENABLE_SMEP_SMAP]="bool:y:none:SMEP/SMAP:Enable Supervisor Mode Execution/Access Prevention"
CONFIG_DB[ENABLE_STACK_PROTECTION]="bool:y:none:Stack Protection:Enable stack canaries"
CONFIG_DB[ENABLE_LOCK_DEBUGGING]="bool:y:none:Lock Debugging:Enable lock contention and deadlock detection"
CONFIG_DB[ENABLE_AUTH]="bool:y:none:Authentication:Enable user authentication subsystem"
CONFIG_DB[ENABLE_SESSION_MANAGEMENT]="bool:y:none:Session Management:Enable TTY session management"
CONFIG_DB[ENABLE_KERNEL_WATCHDOG]="bool:y:none:Kernel Watchdog:Enable per-task watchdog monitoring"
CONFIG_DB[ENABLE_FAULT_PREVENTION]="bool:y:none:Fault Prevention:Enable fault handler stacks and recovery"
CONFIG_DB[ENABLE_MEMORY_VALIDATION]="bool:y:none:Memory Validation:Enable memory address validation"

# --- 8. USB ---
CONFIG_DB[ENABLE_USB]="bool:n:none:USB Subsystem:Enable USB support"
CONFIG_DB[ENABLE_USB_UHCI]="bool:y:ENABLE_USB:UHCI:Enable USB 1.0 UHCI host controller"
CONFIG_DB[ENABLE_USB_OHCI]="bool:y:ENABLE_USB:OHCI:Enable USB 1.0 OHCI host controller"
CONFIG_DB[ENABLE_USB_EHCI]="bool:y:ENABLE_USB:EHCI:Enable USB 2.0 EHCI host controller"
CONFIG_DB[ENABLE_USB_XHCI]="bool:y:ENABLE_USB:xHCI:Enable USB 3.0 xHCI host controller"
CONFIG_DB[ENABLE_USB_HID]="bool:y:ENABLE_USB:USB HID:Enable USB keyboard/mouse"
CONFIG_DB[ENABLE_USB_HUB]="bool:y:ENABLE_USB:USB Hub:Enable USB hub support"
CONFIG_DB[ENABLE_USB_MASS_STORAGE]="bool:y:ENABLE_USB:USB Storage:Enable USB mass storage"

# --- 9. Storage ---
CONFIG_DB[ENABLE_ATA]="bool:y:none:ATA/ATAPI:Enable IDE storage controller"
CONFIG_DB[ENABLE_AHCI]="bool:y:none:AHCI (SATA):Enable AHCI SATA controller"
CONFIG_DB[ENABLE_NVME]="bool:y:none:NVMe:Enable NVMe SSD controller"
CONFIG_DB[ENABLE_SCSI]="bool:n:none:SCSI:Enable SCSI controller"
CONFIG_DB[ENABLE_FDC]="bool:n:none:Floppy:Enable floppy disk controller"
CONFIG_DB[ENABLE_BLOCK_DEVICES]="bool:y:none:Block Devices:Enable block device subsystem"
CONFIG_DB[ENABLE_LOOP_DEVICES]="bool:y:none:Loop Devices:Enable loop device support"

# --- 10. Input Devices ---
CONFIG_DB[ENABLE_PS2]="bool:y:none:PS/2:Enable PS/2 controller"
CONFIG_DB[ENABLE_PS2_KEYBOARD]="bool:y:ENABLE_PS2:PS/2 Keyboard:Enable PS/2 keyboard"
CONFIG_DB[ENABLE_PS2_MOUSE]="bool:y:ENABLE_PS2:PS/2 Mouse:Enable PS/2 mouse"
CONFIG_DB[ENABLE_PS2_WATCHDOG]="bool:y:ENABLE_PS2:PS/2 Watchdog:Enable PS/2 hotplug watchdog"
CONFIG_DB[ENABLE_GAMEPORT]="bool:y:none:Gameport:Enable gameport/joystick"
CONFIG_DB[ENABLE_INPUT_EVENT_SYSTEM]="bool:y:none:Input Events:Enable unified input event subsystem"

# --- 11. IPC ---
CONFIG_DB[ENABLE_IPC]="bool:y:none:IPC:Enable inter-process communication"
CONFIG_DB[ENABLE_DBUS]="bool:y:none:D-Bus:Enable D-Bus message protocol"
CONFIG_DB[ENABLE_SEMAPHORES]="bool:y:none:Semaphores:Enable kernel semaphores"
CONFIG_DB[ENABLE_BARRIERS]="bool:y:none:Barriers:Enable barrier synchronization"
CONFIG_DB[ENABLE_SYSV_SEM]="bool:y:none:SysV Semaphores:Enable System V semaphore sets"
CONFIG_DB[ENABLE_SYSV_MSG]="bool:y:none:SysV Msg Queues:Enable System V message queues"
CONFIG_DB[ENABLE_POSIX_SHM]="bool:y:none:POSIX SHM:Enable POSIX shared memory"
CONFIG_DB[ENABLE_EPOLL]="bool:y:none:epoll:Enable epoll I/O multiplexing"
CONFIG_DB[ENABLE_INOTIFY]="bool:y:none:inotify:Enable inotify file monitoring"
CONFIG_DB[ENABLE_EVENTFD]="bool:y:none:eventfd:Enable eventfd notification"
CONFIG_DB[ENABLE_SIGNALFD]="bool:y:none:signalfd:Enable signalfd interface"
CONFIG_DB[ENABLE_TIMERFD]="bool:y:none:timerfd:Enable timerfd interface"

# --- 12. Timers ---
CONFIG_DB[ENABLE_PIT]="bool:y:none:PIT:Enable Programmable Interval Timer"
CONFIG_DB[ENABLE_HPET]="bool:y:none:HPET:Enable High Precision Event Timer"
CONFIG_DB[ENABLE_APIC_TIMER]="bool:y:none:APIC Timer:Enable Local APIC timer"
CONFIG_DB[ENABLE_CMOS_RTC]="bool:y:none:CMOS RTC:Enable real-time clock"
CONFIG_DB[ENABLE_TSC]="bool:y:none:TSC:Enable Time Stamp Counter"
CONFIG_DB[ENABLE_TIMER_ABSTRACTION]="bool:y:none:Timer Abstraction:Enable unified timer layer"
CONFIG_DB[ENABLE_EPOCH_TIME]="bool:y:none:Epoch Time:Enable Unix epoch time"

# --- 13. Debug ---
CONFIG_DB[ENABLE_DEBUG_SYMBOLS]="bool:y:none:Debug Symbols:Include debug info in kernel"
CONFIG_DB[ENABLE_KERNEL_DEBUG]="bool:y:none:Kernel Debug:Enable kernel debugging facilities"
CONFIG_DB[ENABLE_SERIAL_DEBUG]="bool:y:none:Serial Debug:Enable serial port debug output"
CONFIG_DB[ENABLE_PANIC_BACKTRACES]="bool:y:none:Panic Backtraces:Enable stack traces on panic"
CONFIG_DB[ENABLE_ASSERTIONS]="bool:y:none:Assertions:Enable runtime assertion checking"
CONFIG_DB[ENABLE_MEMORY_DEBUG]="bool:y:none:Memory Debug:Enable memory leak detection"

# --- 14. Hardware ---
CONFIG_DB[ENABLE_PCI]="bool:y:none:PCI Bus:Enable PCI bus support"
CONFIG_DB[ENABLE_PCIE]="bool:y:none:PCIe:Enable PCIe capability detection"
CONFIG_DB[ENABLE_ACPI]="bool:y:none:ACPI:Enable ACPI support"
CONFIG_DB[ENABLE_SERIAL]="bool:y:none:Serial Port:Enable serial port (UART)"
CONFIG_DB[ENABLE_PARALLEL]="bool:n:none:Parallel Port:Enable parallel port (LPT)"
CONFIG_DB[ENABLE_A20]="bool:y:none:A20 Gate:Enable A20 address line management"
CONFIG_DB[ENABLE_VIRTUALBOX_GUEST]="bool:y:none:VBox Guest:Enable VirtualBox Guest Additions"
CONFIG_DB[ENABLE_CHAR_DEVICES]="bool:y:none:Char Devices:Enable /dev/null, /dev/zero, etc."
CONFIG_DB[ENABLE_TTY]="bool:y:none:TTY:Enable terminal subsystem"

# --- 15. Interrupts ---
CONFIG_DB[ENABLE_PIC_8259A]="bool:y:none:8259A PIC:Enable legacy PIC driver"
CONFIG_DB[ENABLE_LOCAL_APIC]="bool:y:none:Local APIC:Enable Local APIC driver"
CONFIG_DB[ENABLE_IOAPIC]="bool:y:none:I/O APIC:Enable I/O APIC driver"
CONFIG_DB[ENABLE_MSI]="bool:n:none:MSI:Enable Message Signaled Interrupts"
CONFIG_DB[ENABLE_NMI_HANDLER]="bool:y:none:NMI Handler:Enable Non-Maskable Interrupt handler"
CONFIG_DB[ENABLE_INTERRUPT_PRIORITY]="bool:y:none:IRQ Priority:Enable interrupt priority management"
CONFIG_DB[ENABLE_INTERRUPT_STATISTICS]="bool:y:none:IRQ Stats:Enable interrupt statistics"
CONFIG_DB[ENABLE_INTERRUPT_VECTOR_ALLOCATION]="bool:y:none:IRQ Vectors:Enable dynamic vector allocation"
CONFIG_DB[ENABLE_INTERRUPT_EOI_MANAGEMENT]="bool:y:none:IRQ EOI:Enable centralized EOI management"

# --- 16. Scheduler ---
CONFIG_DB[ENABLE_ELF_LOADER]="bool:y:none:ELF Loader:Enable ELF binary loader"
CONFIG_DB[ENABLE_LDSO]="bool:y:none:Dynamic Linker:Enable ld.so shared library support"
CONFIG_DB[ENABLE_JOB_CONTROL]="bool:y:none:Job Control:Enable POSIX job control"
CONFIG_DB[ENABLE_SIGNALS]="bool:y:none:Signals:Enable POSIX signal delivery"
CONFIG_DB[ENABLE_IDLE_TASK]="bool:y:none:Idle Task:Enable dedicated idle task"

# --- 19. Build System ---
CONFIG_DB[QEMU_MEMORY]="int:512:none:QEMU Memory:QEMU guest RAM in MB"
CONFIG_DB[QEMU_ENABLE_KVM]="bool:y:none:QEMU KVM:Enable KVM acceleration:process"
CONFIG_DB[QEMU_NETWORK]="bool:y:none:QEMU Network:Enable network emulation:process"
CONFIG_DB[QEMU_USB]="bool:y:none:QEMU USB:Enable USB emulation:process"
CONFIG_DB[GENERATE_CHECKSUMS]="bool:y:none:Checksums:Generate SHA256 checksums:process"
CONFIG_DB[ENABLE_TESTING]="bool:n:none:Auto Testing:Run QEMU boot tests after build:process"
CONFIG_DB[CLEAN_BEFORE_BUILD]="bool:y:none:Clean First:Clean before building:process"
CONFIG_DB[VERBOSE]="bool:n:none:Verbose Build:Show full compiler commands:process"
CONFIG_DB[WERROR]="bool:n:none:Warnings as Errors:Treat compiler warnings as errors (-Werror):process"

# --- 20. Compatibility ---
CONFIG_DB[ENABLE_LINUX_COMPAT]="bool:y:none:Linux Compat:Enable Linux binary compatibility"
CONFIG_DB[ENABLE_UNIX_COMPAT]="bool:y:none:Unix Compat:Enable Unix/POSIX compatibility"
CONFIG_DB[ENABLE_POSIX_SIGNALS]="bool:y:none:POSIX Signals:Enable POSIX signal handling"
CONFIG_DB[ENABLE_POSIX_TERMIOS]="bool:y:none:POSIX Termios:Enable POSIX terminal I/O"
CONFIG_DB[ENABLE_PLATFORM_DETECTION]="bool:y:none:Platform Detection:Enable runtime platform detection"
CONFIG_DB[HYPERVISOR_DETECTION]="bool:y:none:Hypervisor Detection:Enable VM detection"
CONFIG_DB[ROOT_AUTOLOGIN]="bool:n:none:Root Autologin:Auto-login as root at boot:feature"

# --- 21. Numeric Tunables (Makefile ?= defaults) ---
CONFIG_DB[VFS_MAX_PATH]="int:256:none:VFS Max Path:Maximum path length:feature"
CONFIG_DB[VFS_MAX_OPEN_FILES]="int:256:none:VFS Max Open Files:Max open files per process:feature"
CONFIG_DB[VFS_MAX_MOUNTS]="int:16:none:VFS Max Mounts:Maximum mount points:feature"
CONFIG_DB[DISPLAY_DEFAULT_WIDTH]="int:1024:none:Display Default Width:Default framebuffer width:feature"
CONFIG_DB[DISPLAY_DEFAULT_HEIGHT]="int:768:none:Display Default Height:Default framebuffer height:feature"
CONFIG_DB[DISPLAY_DEFAULT_BPP]="int:32:none:Display Default BPP:Default bits per pixel:feature"
CONFIG_DB[NET_MAX_SOCKETS]="int:16:none:Net Max Sockets:Maximum concurrent sockets:feature"
CONFIG_DB[TCP_MAX_CONNECTIONS]="int:16:none:TCP Max Connections:Maximum concurrent TCP connections:feature"
CONFIG_DB[TCP_WINDOW_SIZE]="int:8192:none:TCP Window Size:TCP receive window size:feature"
CONFIG_DB[AUDIO_MAX_STREAMS]="int:8:none:Audio Max Streams:Maximum concurrent audio streams:feature"
CONFIG_DB[AUDIO_RING_BUFFER_SIZE]="int:32768:none:Audio Ring Buffer Size:Audio ring buffer in bytes:feature"
CONFIG_DB[AUDIO_DEFAULT_SAMPLE_RATE]="int:44100:none:Audio Default Sample Rate:Default sample rate in Hz:feature"
CONFIG_DB[SECURITY_AUTH_MAX_USERS]="int:16:none:Security Auth Max Users:Maximum registered users:feature"
CONFIG_DB[SECURITY_AUTH_MAX_GROUPS]="int:8:none:Security Auth Max Groups:Maximum user groups:feature"
CONFIG_DB[SECURITY_MAX_TTY_SESSIONS]="int:22:none:Security Max TTY Sessions:Maximum TTY sessions:feature"
CONFIG_DB[USB_MAX_CONTROLLERS]="int:8:none:USB Max Controllers:Maximum USB host controllers:feature"
CONFIG_DB[USB_MAX_DEVICES]="int:128:none:USB Max Devices:Maximum USB devices:feature"
CONFIG_DB[MAX_BLOCK_DEVICES]="int:64:none:Max Block Devices:Maximum block devices:feature"
CONFIG_DB[IPC_MAX_CHANNELS]="int:32:none:IPC Max Channels:Maximum IPC channels:feature"
CONFIG_DB[POSIX_SHM_MAX_OBJECTS]="int:64:none:POSIX SHM Max Objects:Maximum POSIX SHM objects:feature"
CONFIG_DB[PIT_DEFAULT_FREQUENCY]="int:1000:none:PIT Default Frequency:PIT tick frequency in Hz:feature"
CONFIG_DB[DEBUG_LOG_LEVEL]="int:3:none:Debug Log Level:Kernel log verbosity (0-5):feature"
CONFIG_DB[MAX_STACK_FRAMES]="int:32:none:Max Stack Frames:Max frames in backtrace:feature"
CONFIG_DB[PANIC_MAX_STACK_FRAMES]="int:16:none:Panic Max Stack Frames:Max frames in panic backtrace:feature"
CONFIG_DB[SERIAL_BAUD_RATE]="int:38400:none:Serial Baud Rate:Default serial port baud rate:feature"
CONFIG_DB[TTY_MAX_VIRTUAL_TTYS]="int:64:none:TTY Max Virtual TTYs:Maximum virtual terminals:feature"
CONFIG_DB[INTERRUPT_MAX_NESTING_DEPTH]="int:8:none:Interrupt Max Nesting Depth:Max nested IRQ depth:feature"
CONFIG_DB[SCHED_PRIORITY_LEVELS]="int:8:none:Sched Priority Levels:Number of scheduler priority levels:feature"
CONFIG_DB[MAX_PROCESSES]="int:4096:none:Max Processes:Maximum concurrent processes:feature"
CONFIG_DB[USER_STACK_PAGES]="int:32:none:User Stack Pages:Pages per user stack:feature"
CONFIG_DB[MAX_PIPES]="int:16:none:Max Pipes:Maximum pipe objects:feature"
CONFIG_DB[MAX_PTYS]="int:16:none:Max PTYs:Maximum pseudo-terminals:feature"
CONFIG_DB[KERNEL_HEAP_INITIAL_SIZE]="int:4194304:none:Kernel Heap Initial Size:Initial kernel heap in bytes:feature"
CONFIG_DB[KERNEL_HEAP_MAX_SIZE]="int:134217728:none:Kernel Heap Max Size:Maximum kernel heap in bytes:feature"
CONFIG_DB[KERNEL_STACK_SIZE]="int:16384:none:Kernel Stack Size:Per-task kernel stack in bytes:feature"

# =============================================================================
# CHOICE OPTIONS
# =============================================================================
CHOICE_OPTIONS[BUILD_ARCH]="32 \"32-bit (i686)\" 64 \"64-bit (x86_64)\""
CHOICE_OPTIONS[BUILD_BOOT_MODE]="bios \"BIOS/Legacy Boot\" uefi \"UEFI Boot\""
CHOICE_OPTIONS[BUILD_TYPE]="debug \"Debug Build\" release \"Release Build\" optimize \"Optimized Build\""

# =============================================================================
# CATEGORY REGISTRY (ordered for TUI + traversal)
# =============================================================================
# Each entry: "index:title:opt1 opt2 ..."
declare -a CATEGORIES=(
"0:General Setup:BUILD_ARCH BUILD_BOOT_MODE BUILD_TYPE BUILD_PARALLEL_JOBS"
"1:Architecture and Boot:ENABLE_SMP ENABLE_PAE ENABLE_FPU ENABLE_X86_IST ENABLE_CROSSARC_INTERPRETER ENABLE_FOREB_BOOTLOADER ENABLE_UEFI_SECURE_BOOT"
"2:Memory Management:ENABLE_PAGING ENABLE_SLAB ENABLE_MEMORY_PROTECTION ENABLE_GUARD_PAGES ENABLE_ASLR ENABLE_NX_BIT ENABLE_COW ENABLE_SWAP ENABLE_PAGE_CACHE ENABLE_OOM_KILLER ENABLE_MEMORY_RECLAIM ENABLE_MEMORY_STATS ENABLE_TLB_SHOOTDOWN ENABLE_MEMORY_CORRUPTION_DETECTION"
"3:Filesystems:ENABLE_VFS ENABLE_EXT2 ENABLE_FAT32 ENABLE_EXFAT ENABLE_ISO9660 ENABLE_UDF ENABLE_LEAN ENABLE_YAFFS ENABLE_JFFS2 ENABLE_FFS_AMIGA ENABLE_ZDSFS ENABLE_PROCFS ENABLE_SYSFS ENABLE_DEVFS ENABLE_TMPFS ENABLE_RAMDISK ENABLE_SYMLINKS"
"4:Graphics and Display:ENABLE_GRAPHICS ENABLE_VESA ENABLE_FRAMEBUFFER ENABLE_VGA_TEXT ENABLE_CONSOLE ENABLE_BOCHS_BGA ENABLE_VMWARE_SVGA ENABLE_INTEL_HD ENABLE_NVIDIA_GPU ENABLE_AMD_GPU ENABLE_GPU_ACCEL ENABLE_FONT_RENDERER ENABLE_TRUETYPE ENABLE_WINDOW_MANAGER ENABLE_COMPOSITOR ENABLE_DOUBLE_BUFFERING ENABLE_SPLASH_SCREEN ENABLE_PANICUI ENABLE_DISPLAY_MANAGER ENABLE_WAYLAND_SERVER ENABLE_X11_SERVER ENABLE_CLIPBOARD ENABLE_DRAG_DROP ENABLE_LEAFGFX ENABLE_HARDWARE_DETECT ENABLE_VSYNC"
"5:Networking:ENABLE_NETWORKING ENABLE_ETHERNET ENABLE_TCP ENABLE_UDP ENABLE_DHCP ENABLE_DNS ENABLE_ARP ENABLE_ICMP ENABLE_DRIVER_E1000 ENABLE_DRIVER_RTL8139 ENABLE_DRIVER_NE2000"
"6:Audio:ENABLE_AUDIO ENABLE_SOUND_SB16 ENABLE_SOUND_SBPRO ENABLE_SOUND_AC97 ENABLE_SOUND_HDA ENABLE_SOUND_ENSONIQ ENABLE_SOUND_OPL3 ENABLE_SOUND_PC_SPEAKER ENABLE_SOUND_USB ENABLE_AUDIO_WAV ENABLE_AUDIO_VORBIS"
"7:Security:ENABLE_SMEP_SMAP ENABLE_STACK_PROTECTION ENABLE_MEMORY_CORRUPTION_DETECTION ENABLE_LOCK_DEBUGGING ENABLE_AUTH ENABLE_SESSION_MANAGEMENT ENABLE_KERNEL_WATCHDOG ENABLE_FAULT_PREVENTION ENABLE_MEMORY_VALIDATION"
"8:USB:ENABLE_USB ENABLE_USB_UHCI ENABLE_USB_OHCI ENABLE_USB_EHCI ENABLE_USB_XHCI ENABLE_USB_HID ENABLE_USB_HUB ENABLE_USB_MASS_STORAGE"
"9:Storage:ENABLE_ATA ENABLE_AHCI ENABLE_NVME ENABLE_SCSI ENABLE_FDC ENABLE_BLOCK_DEVICES ENABLE_LOOP_DEVICES"
"10:Input Devices:ENABLE_PS2 ENABLE_PS2_KEYBOARD ENABLE_PS2_MOUSE ENABLE_PS2_WATCHDOG ENABLE_GAMEPORT ENABLE_INPUT_EVENT_SYSTEM"
"11:IPC:ENABLE_IPC ENABLE_DBUS ENABLE_SEMAPHORES ENABLE_BARRIERS ENABLE_SYSV_SEM ENABLE_SYSV_MSG ENABLE_POSIX_SHM ENABLE_EPOLL ENABLE_INOTIFY ENABLE_EVENTFD ENABLE_SIGNALFD ENABLE_TIMERFD"
"12:Timers:ENABLE_PIT ENABLE_HPET ENABLE_APIC_TIMER ENABLE_CMOS_RTC ENABLE_TSC ENABLE_TIMER_ABSTRACTION ENABLE_EPOCH_TIME"
"13:Debugging:ENABLE_DEBUG_SYMBOLS ENABLE_KERNEL_DEBUG ENABLE_SERIAL_DEBUG ENABLE_PANIC_BACKTRACES ENABLE_ASSERTIONS ENABLE_MEMORY_DEBUG"
"14:Hardware:ENABLE_PCI ENABLE_PCIE ENABLE_ACPI ENABLE_SERIAL ENABLE_PARALLEL ENABLE_A20 ENABLE_VIRTUALBOX_GUEST ENABLE_CHAR_DEVICES ENABLE_TTY"
"15:Interrupts:ENABLE_PIC_8259A ENABLE_LOCAL_APIC ENABLE_IOAPIC ENABLE_MSI ENABLE_NMI_HANDLER ENABLE_INTERRUPT_PRIORITY ENABLE_INTERRUPT_STATISTICS ENABLE_INTERRUPT_VECTOR_ALLOCATION ENABLE_INTERRUPT_EOI_MANAGEMENT"
"16:Scheduler:ENABLE_ELF_LOADER ENABLE_LDSO ENABLE_JOB_CONTROL ENABLE_SIGNALS ENABLE_IDLE_TASK"
"19:Build System:QEMU_MEMORY QEMU_ENABLE_KVM QEMU_NETWORK QEMU_USB GENERATE_CHECKSUMS ENABLE_TESTING CLEAN_BEFORE_BUILD VERBOSE WERROR"
"20:Compatibility:ENABLE_LINUX_COMPAT ENABLE_UNIX_COMPAT ENABLE_POSIX_SIGNALS ENABLE_POSIX_TERMIOS ENABLE_PLATFORM_DETECTION HYPERVISOR_DETECTION ROOT_AUTOLOGIN"
"21:Numeric Tunables:VFS_MAX_PATH VFS_MAX_OPEN_FILES VFS_MAX_MOUNTS DISPLAY_DEFAULT_WIDTH DISPLAY_DEFAULT_HEIGHT DISPLAY_DEFAULT_BPP NET_MAX_SOCKETS TCP_MAX_CONNECTIONS TCP_WINDOW_SIZE AUDIO_MAX_STREAMS AUDIO_RING_BUFFER_SIZE AUDIO_DEFAULT_SAMPLE_RATE SECURITY_AUTH_MAX_USERS SECURITY_AUTH_MAX_GROUPS SECURITY_MAX_TTY_SESSIONS USB_MAX_CONTROLLERS USB_MAX_DEVICES MAX_BLOCK_DEVICES IPC_MAX_CHANNELS POSIX_SHM_MAX_OBJECTS PIT_DEFAULT_FREQUENCY DEBUG_LOG_LEVEL MAX_STACK_FRAMES PANIC_MAX_STACK_FRAMES SERIAL_BAUD_RATE TTY_MAX_VIRTUAL_TTYS INTERRUPT_MAX_NESTING_DEPTH SCHED_PRIORITY_LEVELS MAX_PROCESSES USER_STACK_PAGES MAX_PIPES MAX_PTYS KERNEL_HEAP_INITIAL_SIZE KERNEL_HEAP_MAX_SIZE KERNEL_STACK_SIZE"
)

# Options required-on even under allnoconfig (cannot be turned off):
REQUIRED_ON=( ENABLE_PAGING ENABLE_A20 ENABLE_TTY )

# =============================================================================
# WARNING LOG
# =============================================================================
declare -a WARNINGS

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================
show_error() { dialog --title "Error" --msgbox "$1" 10 50 2>/dev/null || echo "ERROR: $1"; exit 1; }
show_info() { dialog --title "$1" --msgbox "$2" 15 70 2>/dev/null || echo "$1: $2"; }
check_dialog() {
    if ! command -v dialog >/dev/null 2>&1; then
        echo "Error: dialog not found. Install: sudo apt-get install dialog"
        echo "Non-TUI modes (--defconfig/--generate/--oldconfig/--allnoconfig/--allyesconfig) still work."
    fi
}

# =============================================================================
# CONFIGURATION MANAGEMENT
# =============================================================================
load_defaults() {
    for config_name in "${!CONFIG_DB[@]}"; do
        CONFIG_VALUES[$config_name]="$(cfg_field "$config_name" 2)"
    done
}

load_config() {
    local config_file="${1:-$CONFIG_FILE}"
    [[ ! -f "$config_file" ]] && return 1
    local line key value name
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -z "$line" || "$line" =~ ^# ]] && continue
        IFS='=' read -r key value <<< "$line"
        [[ -z "$key" ]] && continue
        if [[ "$key" =~ ^CONFIG_(.+)$ ]]; then
            name="${BASH_REMATCH[1]}"
            if [[ -n "${CONFIG_DB[$name]:-}" ]]; then
                CONFIG_VALUES[$name]="$value"
            fi
        fi
    done < "$config_file"
    return 0
}

# Validation: choice values must match CHOICE_OPTIONS; ints must be non-negative.
validate_values() {
    local err=0
    for name in "${!CONFIG_DB[@]}"; do
        local ttype
        ttype="$(cfg_field "$name" 1)"
        local v="${CONFIG_VALUES[$name]:-}"
        if [[ "$ttype" == "choice" ]]; then
            local allowed
            allowed="$(echo "${CHOICE_OPTIONS[$name]}" | grep -oE '^[^ ]+| [^"]+' | tr -d ' ' | tr '\n' ' ')"
            if ! echo " $allowed " | grep -q " $v "; then
                echo "WARN: $name has invalid choice '$v'; resetting to default." >&2
                CONFIG_VALUES[$name]="$(cfg_field "$name" 2)"
                err=1
            fi
        elif [[ "$ttype" == "int" ]]; then
            if ! [[ "$v" =~ ^[0-9]+$ ]]; then
                echo "WARN: $name has invalid int '$v'; resetting to default." >&2
                CONFIG_VALUES[$name]="$(cfg_field "$name" 2)"
                err=1
            fi
        elif [[ "$ttype" == "bool" ]]; then
            if [[ "$v" != "y" && "$v" != "n" ]]; then
                echo "WARN: $name has invalid bool '$v'; resetting to default." >&2
                CONFIG_VALUES[$name]="$(cfg_field "$name" 2)"
                err=1
            fi
        fi
    done
    return $err
}

# Dependency enforcement: if parent is 'n', force all children 'n' (transitive).
# Records warnings in WARNINGS array. Re-runs until stable.
enforce_dependencies() {
    WARNINGS=()
    local changed=1
    while [[ $changed -eq 1 ]]; do
        changed=0
        for name in "${!CONFIG_DB[@]}"; do
            local parent
            parent="$(cfg_field "$name" 3)"
            [[ -z "$parent" || "$parent" == "none" ]] && continue
            # parent must be a known bool option
            [[ -z "${CONFIG_DB[$parent]:-}" ]] && continue
            local pval="${CONFIG_VALUES[$parent]:-n}"
            local cval="${CONFIG_VALUES[$name]:-n}"
            if [[ "$pval" == "n" && "$cval" == "y" ]]; then
                CONFIG_VALUES[$name]="n"
                WARNINGS+=("$name forced to 'n' because parent $parent is 'n'")
                changed=1
            fi
        done
    done
}

# Required-on enforcement: ensure required options stay 'y' even under allnoconfig.
enforce_required() {
    for name in "${REQUIRED_ON[@]}"; do
        if [[ "${CONFIG_VALUES[$name]:-n}" == "n" ]]; then
            CONFIG_VALUES[$name]="y"
            WARNINGS+=("$name kept 'y' (required-on)")
        fi
    done
}

save_config() {
    local config_file="${1:-$CONFIG_FILE}"
    {
        echo "#"
        echo "# Forest-OS Configuration"
        echo "# Generated by conf.sh v$SCRIPT_VERSION on $(date)"
        echo "#"
        echo
        for config_name in $(printf '%s\n' "${!CONFIG_VALUES[@]}" | sort); do
            local description
            description="$(cfg_field "$config_name" 4)"
            echo "# $description"
            echo "CONFIG_$config_name=${CONFIG_VALUES[$config_name]}"
            echo
        done
    } > "$config_file"
}

# =============================================================================
# BUILD CONFIG GENERATOR — produces a CLEAN build-config.mk per spec contract
# =============================================================================
generate_build_config() {
    # First enforce dependencies + validate so emitted file is consistent.
    validate_values || true
    enforce_dependencies
    enforce_required

    local arch="${CONFIG_VALUES[BUILD_ARCH]}"
    local boot_mode="${CONFIG_VALUES[BUILD_BOOT_MODE]}"
    local build_type="${CONFIG_VALUES[BUILD_TYPE]}"
    local parallel_jobs="${CONFIG_VALUES[BUILD_PARALLEL_JOBS]:-0}"

    local bool_feature_vars=""
    local bool_process_vars=""
    local int_vars=""
    local feature_flags=""

    # Iterate in deterministic (sorted) order over keys EXCEPT the three CORE
    # choice vars, which are emitted separately up top.
    for name in $(printf '%s\n' "${!CONFIG_VALUES[@]}" | sort); do
        case "$name" in
            BUILD_ARCH|BUILD_BOOT_MODE|BUILD_TYPE) continue ;;
        esac
        local value="${CONFIG_VALUES[$name]}"
        local ttype cls
        ttype="$(cfg_field "$name" 1)"
        cls="$(cfg_field "$name" 6)"

        if [[ "$ttype" == "bool" ]]; then
            local mkval
            if [[ "$value" == "y" ]]; then
                mkval="yes"
            else
                mkval="no"
            fi
            if [[ "$cls" == "process" ]]; then
                bool_process_vars+="$name := $mkval"$'\n'
            else
                bool_feature_vars+="$name := $mkval"$'\n'
                # ROOT_AUTOLOGIN is handled explicitly by the Makefile as
                # -DENABLE_ROOT_AUTOLOGIN (spec: NOT in FEATURE_FLAGS).
                if [[ "$value" == "y" && "$name" != "ROOT_AUTOLOGIN" ]]; then
                    feature_flags+=" -D$name"
                fi
            fi
        elif [[ "$ttype" == "int" ]]; then
            int_vars+="$name := $value"$'\n'
        fi
    done

    # Derived build flags from BUILD_TYPE
    local opt_level debug_flags
    case "$build_type" in
        debug)    opt_level=0; debug_flags="-g -DDEBUG" ;;
        release)  opt_level=2; debug_flags="-DNDEBUG" ;;
        optimize) opt_level=3; debug_flags="-DNDEBUG" ;;
        *)        opt_level=0; debug_flags="-g -DDEBUG" ;;
    esac

    {
        echo "# ============================================================================="
        echo "# FOREST OS BUILD CONFIGURATION (Generated by conf.sh v$SCRIPT_VERSION)"
        echo "# DO NOT EDIT — regenerate via ./conf.sh --generate"
        echo "# ============================================================================="
        echo
        echo "# Core"
        echo "ARCH := $arch"
        echo "BOOT_MODE := $boot_mode"
        echo "BUILD_TYPE := $build_type"
        echo "PARALLEL_JOBS := $parallel_jobs"
        echo
        echo "# Feature booleans (gate C code via #ifdef)"
        printf "%s" "$bool_feature_vars"
        echo
        echo "# Process booleans (build/runtime behavior; NOT -D defines)"
        printf "%s" "$bool_process_vars"
        echo
        echo "# Numeric tunables"
        printf "%s" "$int_vars"
        echo
        echo "# Compiler feature defines — ONLY class=feature bools that are 'yes'"
        echo "FEATURE_FLAGS :=${feature_flags}"
        echo
        echo "# Derived build flags"
        echo "OPTIMIZATION_LEVEL := $opt_level"
        echo "DEBUG_FLAGS := $debug_flags"
        echo
        echo "# Generated on $(date)"
    } > "$BUILD_CONFIG_FILE"
}

# =============================================================================
# MENU FUNCTIONS
# =============================================================================
show_category_menu() {
    local category="$1"
    local title="$2"
    shift 2
    local options=("$@")
    local menu_items="" count=1 opt info ttype descr value disp

    for opt in "${options[@]}"; do
        info="${CONFIG_DB[$opt]:-}"
        [[ -z "$info" ]] && continue
        ttype="$(echo "$info" | cut -d: -f1)"
        descr="$(echo "$info" | cut -d: -f4)"
        value="${CONFIG_VALUES[$opt]:-n}"
        disp=""
        case "$ttype" in
            bool) disp=$([[ "$value" == "y" ]] && echo "[*]" || echo "[ ]") ;;
            choice|int) disp="($value)" ;;
        esac
        menu_items+=" $count \"$disp $descr\""
        ((count++))
    done
    eval "dialog --title \"$title\" --menu \"Select option to configure:\" 22 76 16 $menu_items" 2>"$TEMPFILE"
    return $?
}

configure_bool_option() {
    local config_name="$1"
    local description help_text current_value initial
    description="$(cfg_field "$config_name" 4)"
    help_text="$(cfg_field "$config_name" 5)"
    current_value="${CONFIG_VALUES[$config_name]:-n}"
    initial=""
    [[ "$current_value" == "y" ]] && initial="--defaultno"
    if dialog --title "Configure: $description" $initial --yesno "$help_text\n\nEnable this option?" 12 60; then
        CONFIG_VALUES[$config_name]="y"
    else
        CONFIG_VALUES[$config_name]="n"
    fi
    enforce_dependencies
    if [[ ${#WARNINGS[@]} -gt 0 ]]; then
        local w
        for w in "${WARNINGS[@]}"; do dialog --title "Dependency notice" --msgbox "$w" 8 60 2>/dev/null || true; done
    fi
}

configure_choice_option() {
    local config_name="$1"
    local description help_text choices
    description="$(cfg_field "$config_name" 4)"
    help_text="$(cfg_field "$config_name" 5)"
    choices="${CHOICE_OPTIONS[$config_name]}"
    eval "dialog --title \"Configure: $description\" --menu \"$help_text\" 15 60 8 $choices" 2>"$TEMPFILE"
    if [[ $? -eq 0 ]]; then
        CONFIG_VALUES[$config_name]=$(cat "$TEMPFILE")
    fi
}

configure_int_option() {
    local config_name="$1"
    local description help_text current_value new_value
    description="$(cfg_field "$config_name" 4)"
    help_text="$(cfg_field "$config_name" 5)"
    current_value="${CONFIG_VALUES[$config_name]}"
    dialog --title "Configure: $description" --inputbox "$help_text\n\nCurrent: $current_value" 12 60 "$current_value" 2>"$TEMPFILE"
    if [[ $? -eq 0 ]]; then
        new_value=$(cat "$TEMPFILE")
        if [[ "$new_value" =~ ^[0-9]+$ ]]; then
            CONFIG_VALUES[$config_name]="$new_value"
        else
            show_error "Invalid value. Please enter a non-negative integer."
        fi
    fi
}

configure_category() {
    local title="$1"; shift
    local options=("$@")
    local choice opt ttype
    while true; do
        if ! show_category_menu "$(echo "$title" | tr ' ' '_')" "$title" "${options[@]}"; then return; fi
        choice=$(cat "$TEMPFILE")
        [[ -z "$choice" ]] && return
        opt="${options[$((choice-1))]}"
        [[ -z "${CONFIG_DB[$opt]:-}" ]] && continue
        ttype="$(cfg_field "$opt" 1)"
        case "$ttype" in
            bool) configure_bool_option "$opt" ;;
            choice) configure_choice_option "$opt" ;;
            int) configure_int_option "$opt" ;;
        esac
    done
}

# =============================================================================
# CATEGORY CONFIGURATOR DISPATCH
# =============================================================================
configure_category_by_index() {
    local idx="$1"
    local entry title opts
    for entry in "${CATEGORIES[@]}"; do
        if [[ "$entry" == "$idx:"* ]]; then
            title="${entry#*:}"
            title="${title%%:*}"
            opts="${entry#*:*:*}"
            configure_category "$title" $opts
            return
        fi
    done
}

# =============================================================================
# NON-INTERACTIVE MODES
# =============================================================================
do_allnoconfig() {
    load_defaults
    for name in "${!CONFIG_DB[@]}"; do
        local t
        t="$(cfg_field "$name" 1)"
        if [[ "$t" == "bool" ]]; then
            CONFIG_VALUES[$name]="n"
        fi
    done
    enforce_required
    enforce_dependencies
    save_config
    generate_build_config
    echo "allnoconfig: written $CONFIG_FILE + $BUILD_CONFIG_FILE"
}

do_allyesconfig() {
    load_defaults
    for name in "${!CONFIG_DB[@]}"; do
        local t
        t="$(cfg_field "$name" 1)"
        if [[ "$t" == "bool" ]]; then
            CONFIG_VALUES[$name]="y"
        fi
    done
    enforce_required   # no-op here, all already y, but keep invariant
    enforce_dependencies
    save_config
    generate_build_config
    echo "allyesconfig: written $CONFIG_FILE + $BUILD_CONFIG_FILE"
}

do_defconfig() {
    load_defaults
    enforce_required
    enforce_dependencies
    save_config
    generate_build_config
    echo "defconfig: written $CONFIG_FILE + $BUILD_CONFIG_FILE"
}

do_oldconfig() {
    if ! load_config "$CONFIG_FILE"; then
        echo "No existing .forestos_config; running defconfig."
        do_defconfig
        return
    fi
    validate_values || true
    enforce_required
    enforce_dependencies
    save_config
    generate_build_config
    echo "oldconfig: re-validated $CONFIG_FILE + regenerated $BUILD_CONFIG_FILE"
}

do_generate() {
    if [[ ! -f "$CONFIG_FILE" ]]; then
        echo "No .forestos_config found; running defconfig first."
        do_defconfig
        return
    fi
    load_config "$CONFIG_FILE"
    validate_values || true
    enforce_dependencies
    enforce_required
    generate_build_config
    echo "Generated: $BUILD_CONFIG_FILE"
}

print_help() {
    cat <<EOF
Forest-OS Configuration System v$SCRIPT_VERSION

Usage: $0 [mode]

Modes:
  (default)       Interactive TUI menu (same as --menuconfig)
  --help, -h       Show this help
  --defaults       Load built-in defaults and save .forestos_config
  --defconfig      sane defaults: write .forestos_config + regenerate build-config.mk
  --menuconfig     Interactive TUI (same as bare invocation)
  --oldconfig      Load existing .forestos_config, re-validate, regenerate build-config.mk
  --allnoconfig    All bools=n (except required-on), save + generate
  --allyesconfig   All bools=y, save + generate
  --generate       Regenerate build-config.mk from current .forestos_config
  --save [file]    Save current values to .forestos_config (or [file])
  --load [file]    Load values from .forestos_config (or [file])
EOF
}

# =============================================================================
# MAIN PROGRAM
# =============================================================================
check_dialog

# Start from defaults, then overlay existing config if present.
load_defaults
load_config "$CONFIG_FILE" || true

case "${1:---menuconfig}" in
    --help|-h)      print_help; exit 0 ;;
    --defaults)     load_defaults; enforce_required; enforce_dependencies; save_config; generate_build_config; echo "Defaults loaded"; exit 0 ;;
    --defconfig)    do_defconfig; exit 0 ;;
    --menuconfig)   : ;;  # fall through to TUI
    --oldconfig)    do_oldconfig; exit 0 ;;
    --allnoconfig)  do_allnoconfig; exit 0 ;;
    --allyesconfig) do_allyesconfig; exit 0 ;;
    --generate)     do_generate; exit 0 ;;
    --save)         save_config "${2:-$CONFIG_FILE}"; echo "Saved to ${2:-$CONFIG_FILE}"; exit 0 ;;
    --load)         load_config "${2:-$CONFIG_FILE}" && echo "Loaded" || echo "Failed"; exit $? ;;
    "")             : ;;  # bare = menuconfig
    *)              echo "Unknown mode: $1" >&2; print_help; exit 2 ;;
esac

# --- TUI (menuconfig) ---
if ! command -v dialog >/dev/null 2>&1; then
    echo "Error: dialog not installed. Run a non-TUI mode (e.g. --defconfig)." >&2
    exit 1
fi

while true; do
    # Build menu items from CATEGORIES registry
    menu_items=""
    for entry in "${CATEGORIES[@]}"; do
        idx="${entry%%:*}"
        rest="${entry#*:}"
        title="${rest%%:*}"
        menu_items+=" $idx \"$title\""
    done
    menu_items+=" 90 \"Save Configuration\" 91 \"Generate Build Config\" 92 \"Exit\""

    eval "dialog --title \"Forest-OS Configuration v$SCRIPT_VERSION\" --menu \
        \"Configure build options for Forest-OS (Fern kernel).\" 30 72 32 $menu_items" 2>"$TEMPFILE" || break

    choice=$(cat "$TEMPFILE")
    case "$choice" in
        90) save_config; show_info "Saved" "Configuration saved to:\n$CONFIG_FILE" ;;
        91) generate_build_config; show_info "Generated" "Build config generated:\n$BUILD_CONFIG_FILE" ;;
        92) break ;;
        *)  configure_category_by_index "$choice" ;;
    esac
done

clear
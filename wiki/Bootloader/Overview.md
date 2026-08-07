# ForeB — Forest OS Bootloader

ForeB is the default bootloader for Forest OS. It boots the kernel from both
legacy BIOS/CSM and native UEFI firmware, replacing GRUB entirely while staying
fully Multiboot1-compatible with the kernel.

## What ForeB Does

At its core, ForeB is the first piece of software that runs when you turn on
your computer. It shows a menu, lets you pick an OS (or a boot mode), then
hands off to the kernel with everything it needs — memory map, framebuffer
info, command line, initrd.

The key insight is that no matter which firmware your machine has, the kernel
gets the exact same register state and memory layout. The kernel literally
cannot tell whether it was booted by BIOS or UEFI. That's the invariant
everything else is built around.

## Design Philosophy: Dual Firmware, Single Handoff

ForeB has two completely independent front-ends that share one kernel handoff
contract:

- **BIOS/CSM path** — three NASM assembly stages (`stage1`, `stage2`,
  `stage3`) that run in real mode and protected mode, reading the kernel from
  raw disk sectors via INT 13h.
- **UEFI (native) path** — a self-contained 64-bit EFI application
  (`BOOTX64.EFI`) written in freestanding C (no gnu-efi, no libc, no CRT)
  that reads the kernel from the EFI System Partition's FAT filesystem.

Both paths converge on the same final state:

```
CPU mode:     32-bit protected mode, paging OFF, interrupts OFF
EAX:          0x2BADB002 (Multiboot1 magic)
EBX:          0x1800 (pointer to multiboot_info_t)
EIP:          kernel ELF entry point
PICs:         both fully masked
```

The kernel's own entry code (`boot.asm` / `boot64.asm`) handles the
long-mode transition. ForeB never enters the kernel in 64-bit mode — the UEFI
loader, which runs in long mode, tears it back down to 32-bit protected mode
before the jump.

## BIOS Boot Path (Assembly Stages)

The BIOS path is a classic staged loader:

| Stage  | File         | Mode        | Size   | Role |
|--------|-------------|-------------|--------|------|
| Stage 1 | `stage1.asm` | 16-bit real | 512 B | MBR: relocate, LBA-probe, load stage 2 |
| Stage 2 | `stage2.asm` | 16-bit real | 8 KiB | A20, E820 memory map, VBE, boot menu, disk I/O, multiboot info |
| Stage 3 | `stage3.asm` | 32-bit PM   | 8 KiB | Parse kernel ELF, copy PT_LOAD segments, jump to entry |

### Disk Layout

```
Sector 0        Stage 1 (MBR + partition table + 0xAA55)
Sectors 1..16   Stage 2 (8 KiB)
Sectors 17..32  Stage 3 (8 KiB)
Sector 48+      Kernel ELF image
Past kernel     Optional initrd / multiboot module
```

Disk reads use INT 13h LBA extensions (AH=42h) with automatic CHS fallback.
A20 is enabled via three methods in sequence: fast A20 (port 0x92), keyboard
controller (port 0x64/0x60), and BIOS INT 15h AX=2401h, with verification
after each.

### The 8 KiB Budget

BIOS stages 2 and 3 are each hard-capped at 8 KiB. The `make check` target
enforces this — if stage 2 or 3 exceeds 8192 bytes, the build fails. This
keeps the BIOS path lean but means features like the 8x16 font, background
images, icons, animations, and the interactive shell are UEFI-only.

## UEFI Boot Path (Freestanding EFI App)

The UEFI loader is a single `BOOTX64.EFI` PE32+ application. It is completely
freestanding — no gnu-efi, no libc, no CRT. It defines the EFI structs it
needs inline.

### Boot Sequence

1. **GOP framebuffer** — locate the Graphics Output Protocol, record the
   framebuffer base, resolution, and pitch (always 32bpp RGB).
2. **Load kernel** — open the ESP's Simple File System, read
   `\forebo\kernel.elf` into an `AllocatePages` buffer.
3. **Graphical menu** — draw the forest-themed menu directly on the GOP
   framebuffer (shared font + theme), poll `ReadKeyStroke` for Up/Down/Enter.
   The menu completes before `GetMemoryMap` so no allocation invalidates the
   map key.
4. **Reserve low memory** — `AllocatePages` the fixed ForeB low-RAM region
   (`0x1000..0x27FF`) and kernel destination (`0x100000`) so they survive
   `ExitBootServices`.
5. **Memory map** — snapshot the EFI memory map, convert each descriptor to
   E820-style entries (Conventional/BootServices → 1 usable, ACPIReclaim → 3,
   else → 2 reserved).
6. **`ExitBootServices`** — the firmware stops owning memory.
7. **Load progress bar** — read the kernel in chunks, advance an in-place
   progress bar on the GOP framebuffer per chunk (direct writes, no `ConOut`,
   no scroll).
8. **Build handoff structs** — parse the ELF, copy PT_LOAD segments, write
   `multiboot_info_t` at `0x1800`, mmap arrays at `0x1400`/`0x1100`,
   `foreboots_boot_info` at `0x1000`.
9. **Tear down long mode** — load a 32-bit flat GDT, leave long-mode
   submode, clear `CR0.PG`/`EFER.LME`/`CR4.PAE`, far-jump into 32-bit CS.
10. **Mask PICs, jump** — `out 0x21,0xFF` / `out 0xA1,0xFF`, then jump to
    `e_entry` with `EAX=0x2BADB002`, `EBX=0x1800`.

### Cross-Architecture Support

The UEFI C tree compiles for three architectures from one source:

- **x86-64**: `BOOTX64.EFI` — fully bootable with Multiboot1 handoff.
- **aarch64**: `BOOTAA64.EFI` — verified bootable under QEMU aa64 OVMF.
  No Multiboot handoff (x86-only), but exposes UI + shell + Linux boot +
  chainload + filesystems + recovery.
- **riscv64**: compiles to objects and links a static-PIE ELF, but cannot
  produce a PE32+ EFI app with current toolchains (needs edk2 GenFw).

## The Multiboot1 Handoff Contract

ForeB builds a standard Multiboot1 info struct at physical address `0x1800`
and passes it to the kernel in `EBX`. The kernel reads these flag bits:

| Flag | Value | What It Provides |
|------|-------|-----------------|
| `MB_FLAG_MEM` | 0x001 | `mem_lower` (640 KiB), `mem_upper` (KiB above 1 MiB) |
| `MB_FLAG_CMDLINE` | 0x004 | Physical pointer to command line string |
| `MB_FLAG_MMAP` | 0x040 | `mmap_addr`, `mmap_length` (Multiboot1 mmap entries) |
| `MB_FLAG_BOOTLOADER` | 0x200 | `boot_loader_name` ("ForeB") |
| `MB_FLAG_FRAMEBUFFER` | 0x1000 | `framebuffer_addr` (64-bit), pitch, width, height, bpp, type |
| `MB_FLAG_MODS` | 0x008 | `mods_count`, `mods_addr` (initrd, if present) |

ForeB also builds a richer, ForeB-native `foreboots_boot_info` struct at
`0x1000` (magic `0x464F5242`, version 2.0). This is a forward-looking extension
point — the kernel can optionally read it for 64-bit memory map data, CPU
capability info, and other details that Multiboot1 doesn't carry.

### Low-Memory Map

```
0x0500   Disk Address Packet (DAP)
0x0600   Stage 1 relocated MBR
0x1000   foreboots_boot_info (152 B)
0x1100   ForeB E820 mmap array (32 × 24 B)
0x1400   Multiboot1 mmap array
0x1800   multiboot_info_t (kernel handoff)
0x2000   VBE controller info
0x2200   VBE mode info
0x5000   Stage 3 (8 KiB)
0x8000   Stage 2 (8 KiB)
0x10000  Kernel ELF buffer (up to 256 KiB)
0x70000  Real-mode stack
```

## Build System

The Makefile handles both BIOS and UEFI targets from a single invocation:

### BIOS Targets

```
make all          # stage1/2/3 binaries
make check        # verify sizes + MBR/stage2 signatures
make image        # forebo.img (raw disk with kernel + module)
make iso          # hybrid ISO (BIOS + UEFI bootable)
make qemu         # test the disk image in QEMU
```

### UEFI Targets

```
make uefi         # BOOTX64.EFI (clang + ld.lld)
make esp          # FAT ESP image with BOOTX64.EFI + kernel + config + assets
make qemu-uefi    # boot ESP under OVMF (edk2) in QEMU
```

### Hybrid ISO

`make iso-hybrid` (aliased as `make iso`) produces a single medium that boots
on either firmware: BIOS machines use the El Torito no-emulation catalog
(ForeB stages), UEFI machines use the embedded ESP and load `BOOTX64.EFI`.

### Overriding Config

Every tunable in `config.h` is wrapped in `%ifndef`, so you can override from
the build command line:

```bash
make NASMFLAGS='-DFOREB_DEFAULT_WIDTH=1280 -DFOREB_DEFAULT_HEIGHT=720 -DFOREB_DEFAULT_TIMEOUT=3'
make NASMFLAGS='-DFOREB_FORCE_LONG_MODE=1'
```

### Host Tools

ForeB ships several host tools:

- **`forb-install`** — C++ installer for writing ForeB to real disks.
- **`forb-customizer`** — Qt6 GUI for customizing the bootloader.
- **`forb-config`** — Python CLI for config management.
- **`forb-mkrescue`** — drop-in replacement for `grub-mkrescue`.
- **`tools/gen_assets.py`** — generates background BMP and icon TGAs from the
  theme header.

## Configuration File (`forebo.cfg`)

The UEFI path reads `\forebo\forebo.cfg` from the ESP. It's a grub.cfg-like
format: comments start with `#`, blank lines are ignored, and unknown keys are
silently skipped (so old configs keep working with new ForeB builds).

### Global Settings

```
timeout=60              # seconds before auto-booting the default
default=0               # 0-based index, or a title path like "CachyOS/linux-cachyos"
remember_last=1         # persist last booted entry in UEFI variable
background=/forebo/bg.bmp
theme=forest            # named palette: forest, midnight, nord, dracula, gruvbox, ...
```

### Menu Entry Blocks

```
menuentry "Forest OS" {
    type=forest
    kernel=/forebo/kernel.elf
    module=/forebo/initrd.tar
    icon=os
    cmdline=""
}
```

### Entry Types

| Type | What It Does |
|------|-------------|
| `forest` | Multiboot1 Forest kernel handoff (x86_64 only) |
| `linux` | EFI-stub vmlinuz + initrd via LoadImage/StartImage |
| `chainload` | LoadImage/StartImage another EFI bootloader |
| `shell` | Open the interactive ForeB shell window |
| `recovery` | Open the Recovery / disk-tools window |
| `tools` | Open the GUI Tools launcher (Disk Info, GPT Viewer, etc.) |
| `setup` / `firmware` | Reboot into firmware/UEFI setup screen |
| `settings` | Open the live Theme/Settings editor |
| `reboot` | Firmware reset |

### Submenus

Limine-style collapsible submenus are supported:

```
submenu "CachyOS" {
    icon=arch
    menuentry "linux-cachyos" {
        type=linux
        vmlinuz=/vmlinuz-linux-cachyos
        initrd=/initramfs-linux-cachyos.img
        cmdline="root=UUID=<uuid> rw"
    }
}
```

Submenus nest up to 8 levels deep. The panel title shows a breadcrumb
(`ForeB > CachyOS > linux-cachyos`).

### Theme and Customization

The config exposes extensive theming: named color presets, per-element color
overrides (`color_bg`, `color_fg`, `color_accent`, etc.), menu layout presets
(`menu_style=classic`), window chrome styles (`window_skin=beveled`), visual
effects (`fx_glass`, `fx_blur`), PC speaker audio, and custom images for
backgrounds, panels, title bars, buttons, and cursors.

## Supported Boot Methods

### Forest OS (Multiboot1)

The primary boot method. ForeB loads the Forest kernel ELF from the ESP
(UEFI) or raw disk sectors (BIOS), copies each `PT_LOAD` segment to its
physical address, zero-fills BSS, and jumps to the entry point with
Multiboot1 registers. Both ARCH=32 and ARCH=64 kernels work — the 64-bit
kernel does its own long-mode transition after entry.

### Linux (EFI-Stub)

On UEFI, ForeB can boot a standard Linux `vmlinuz` as an EFI-stub PE app:
`LoadImage`/`StartImage` with the kernel cmdline as `LoadOptions` and the
initrd exposed via the Linux initrd `LoadFile2` media protocol
(`LINUX_EFI_INITRD_MEDIA_GUID`). This works on x64, aarch64, and riscv64
UEFI.

### Chainload

ForeB can chainload another EFI bootloader — say, GRUB from a live USB stick
or the Windows Boot Manager. If `chain=` names a file, ForeB loads it
directly. If `chain=` is empty, ForeB auto-scans all volumes (including USB)
for standard EFI loaders (`\EFI\BOOT\BOOTX64.EFI`, `\EFI\*\grubx64.efi`)
and boots the first it finds.

## Filesystem Support

### UEFI Path

- **FAT (ESP)** — read via firmware's Simple File System protocol.
- **ext2/3/4** — read-only (superblock + inode + extent tree), used by the
  shell and recovery tools for `ls` and `cat`.
- **btrfs** — detection + subvolume/snapshot listing (read-only, best-effort).

### BIOS Path

- **Raw disk** — INT 13h LBA reads with CHS fallback.
- No filesystem support; everything is accessed by sector offset.

## The Graphical Boot Menu

Both firmware paths render the same forest-themed graphical menu — pixel-for-
pixel identical. The menu features:

- Dark-green background with a tree logo
- `[ Boot Menu ]` panel with highlighted selection bar
- Footer hint line (`[Up/Down] Navigate  [Enter] Boot  [Esc] Reset`)
- Amber auto-boot countdown timer
- In-place load progress bar (no scrolling status text)
- Partial-repaint UI (only the countdown/progress is redrawn per frame)

### BIOS Menu

Renders in 8bpp VBE mode using the 8x8 ROM font. After the user selects a
framebuffer-capable entry, stage 2 re-selects the kernel framebuffer from a
32bpp chain: 1920×1080 → 1280×720 → 1024×768 → 800×600 → 640×480 → VGA
text mode 03h.

### UEFI Menu

Renders in 32bpp GOP using a crisp 8x16 font (with 2× scaling). Features
double buffering (tear-free), mouse support (USB only — OVMF doesn't surface
PS/2 pointers), movable windows via a tiny compositor, scrolling menu with
scrollbar, sliding selection highlight, background images (BMP/TGA), per-entry
icons (TGA with alpha), fade-in/parallax animations, and an interactive shell
(press `c`).

### Shared UI Assets

Both renderers use the same font, palette, and geometry:

- `include/font8x8.h` — 8×8 glyph set (BIOS)
- `include/font8x16.h` — 8×16 glyph set (UEFI)
- `include/forebo_theme.h` — 13 forest-theme colors as RGB888 + layout constants
- `UI_SPEC.md` — authoritative UI specification

## Recovery and Rescue Tools

The UEFI path includes a full set of recovery and diagnostic tools, accessible
from the menu or the interactive shell:

### Shell Commands

`ls` / `cat` / `hexdump`, block-device `lsblk` / `read` / `write` (destructive
writes gated behind a literal `yes`), `drives`, `modules`, UEFI variable
inspection (`efivars`, `bootvars`, `getvar`, `setvar`), `background`, `boot`,
`reboot`, `exit`.

### Recovery Tools

- **`gpt`** — parse and print GPT partition tables
- **`parts`** — list partitions per BlockIo protocol
- **`fsprobe`** — identify filesystem by magic bytes
- **`rescue`** — copy sectors, skip bad ones
- **`fatfix`** — FAT boot-sector check and repair
- **Best-effort data recovery** — undelete scan, carve by magic

### GUI Tools Launcher

A `type=tools` entry opens a windowed launcher with ~10 self-contained tools:
Disk Info, GPT Viewer, Partition Browser, File Browser (ESP), Hex Viewer,
Memory Map, EFI Variables, Boot Manager, System/Firmware Info, Theme/Settings,
and Key Tester. Each runs as a movable, z-ordered, focusable window in the
compositor.

### Firmware Setup

A `type=setup` entry (or the shell `setup`/`firmware` command) reboots into
the firmware setup screen by setting the `OsIndications` runtime variable
(`EFI_OS_INDICATIONS_BOOT_TO_FW_UI`) and calling `ResetSystem(EfiResetCold)`.

## BIOS vs UEFI Comparison

| Aspect | BIOS/CSM Path | UEFI (native) Path |
|--------|--------------|-------------------|
| Loader | `stage1`/`stage2`/`stage3` (NASM) | `BOOTX64.EFI` (freestanding C) |
| Entry mode | 16-bit real → 32-bit PM | 64-bit long → torn down to 32-bit PM |
| Framebuffer | VBE (INT 10h) | GOP (Graphics Output Protocol) |
| Memory map | INT 15h E820 | `GetMemoryMap` → E820 conversion |
| Kernel source | Raw disk sectors (48+) | ESP FAT file `\forebo\kernel.elf` |
| Menu font | 8×8 | 8×16 |
| Background/images | No | BMP/TGA support |
| Mouse support | No | USB pointer protocols |
| Shell | No | Yes (press `c`) |
| Config file | No (compile-time only) | `forebo.cfg` on ESP |
| Modules | 1 (initrd) | Multiple |
| Recovery tools | No | Full set |
| **Kernel entry** | 32-bit PM, `EAX=0x2BADB002`, `EBX=0x1800` | **Identical** |

## Limitations and Future Work

### Current Limitations

- **BIOS initrd**: The BIOS path supports exactly one module (an initrd). Use
  the UEFI path for multiple modules.
- **Kernel buffer**: The kernel ELF is loaded to a 256 KiB buffer at `0x10000`.
  Larger kernels require raising `KERNEL_MAX_SECTORS`.
- **BIOS font**: The 8×8 font is a hard constraint for the 8 KiB stage budget.
  The 8×16 font, background images, icons, animations, and shell are UEFI-only.
- **UEFI no-fb**: There's no VGA-text equivalent under UEFI. The BIOS `nofb`
  path (EGA text mode 02h) has no direct UEFI counterpart.
- **USB-only mouse**: OVMF doesn't surface PS/2 pointers to UEFI apps, so
  ForeB supports USB pointers only.
- **RISC-V PE**: Cannot produce a bootable `BOOTRISCV64.EFI` without edk2
  BaseTools or a custom binutils build.

### Future Work

- **Long-mode trampoline**: `FOREB_FORCE_LONG_MODE=1` is available but off
  by default. It builds identity-mapped page tables and enters the kernel in
  64-bit mode for future kernels with native 64-bit entry points.
- **More filesystem drivers**: The read-only ext2/3/4 and btrfs support could
  grow write capability or additional filesystem types.
- **ARM/RISC-V handoff**: The non-x86 UEFI builds already expose UI, shell,
  Linux boot, chainload, and recovery. A non-Multiboot handoff protocol for
  those architectures could be added.

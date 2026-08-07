# ForeB UEFI Bootloader

## Overview

ForeB's UEFI loader is a **self-contained EFI application** -- no gnu-efi, no libc, no standard C library. It links directly against UEFI firmware services via hand-written protocol structs, builds a graphical boot menu on a GOP linear framebuffer, and boots Forest OS via a Multiboot1-compatible handoff that reproduces the exact same machine state as the BIOS stage3 path.

The loader lives in `foreboots/uefi/` and compiles to `BOOTX64.EFI` (the default UEFI boot path). It runs on x86_64 and aarch64 UEFI platforms, with the Forest multiboot1 handoff gated to x86_64 only.

---

## Architecture

### Self-Contained, Freestanding

The entire UEFI loader is built with:

```
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
      -mno-red-zone -mno-mmx -mno-sse -Wall -Wextra -std=c11 -Iinclude
```

There is **no libc** and **no gnu-efi**. The loader provides its own `memset` and `memcpy` implementations (clang still emits calls for struct operations). All string, formatting, and I/O helpers are freestanding and file-static to avoid symbol clashes between translation units.

### Module Organization

The loader is split across ~40 source files in a logical directory structure:

```
uefi/
  bootx64.c          Main EFI entry point (efi_main)
  ui.c               GOP framebuffer UI (primitives, double-buffering, theme engine)
  core/
    config.c          forebo.cfg parser + ESP I/O helpers
    image.c           BMP/TGA decode + scaled alpha blit
    modules.c         Multiboot1 module (initrd) loader
    input.c           Mouse/pointer polling + cursor sprite
    wm.c              Window manager compositor
    anim.c            Particle layer, fade, spinner, progress bar
    statusbar.c       Status bar overlay
    errorbox.c        Error dialog boxes
    diskio.c          Disk I/O abstraction
  tools/
    shell.c           Interactive framebuffer shell
    tools.c           GUI Tools launcher window
    basic.c           BASIC interpreter
    tools_*.c         Various GUI tool windows
  boot/
    linux.c           Linux EFI-stub boot
    chain.c           EFI chainload
    chainload.c       Chainload helpers
  fs/
    fs_ext.c          Read-only ext2/3/4 driver
    fs_btrfs.c        Btrfs detection
  recovery/
    recovery.c        Recovery tools
    fwsetup.c         Firmware setup reboot
    clone.c           Clone drive tool
    undelete.c        Undelete/carve tool
    settings_nv.c     Durable NV variable persistence
    uefi_settings.c   UEFI variable viewer/editor
  standalone/
    audio.c           PC-speaker UI tones
    sysmon.c          System monitor
    imgview.c         Image viewer
    clock.c           Clock display
    calc.c            Calculator
```

### Serial Debug Logging

The loader initializes a 16550 COM1 port at 115200 baud for debug logging. This is critical because:
1. The firmware text console (`ConOut`) forces a full-screen `memmove` per newline on hi-res GOP -- the "slow scroll bug"
2. After `ExitBootServices`, the firmware console is gone entirely; only port I/O survives

All `serial_puts()` calls go to COM1 at ASCII speed, independent of the framebuffer.

---

## Graphics Output Protocol (GOP) Setup

The loader acquires the GOP framebuffer early in `efi_main`:

```c
EFI_STATUS st = gBS->LocateProtocol(&gGopGuid, NULL, (VOID **)&gop);
fb_base  = (UINT64)gop->Mode->FrameBufferBase;
fb_w     = mi->HorizontalResolution;
fb_h     = mi->VerticalResolution;
fb_pitch = mi->PixelsPerScanLine * 4;
fb_pixfmt = (UINT32)mi->PixelFormat;   // honor RGBX vs BGRX byte order
```

Key details:
- **Pixel format**: Supports both `PixelBlueGreenRedReserved8BitPerColor` (BGRX, x86 default) and `PixelRedGreenBlueReserved8BitPerColor` (RGBX). The `g_swap_rb` flag handles byte-order conversion at store time.
- **Write-combining**: Best-effort, the loader asks the CPU Architectural Protocol to map the framebuffer as `EFI_MEMORY_WC` so the back-to-front copy in `ui_present()` streams through WC buffers instead of stalling on UC MMIO.
- **Auto-scaling**: On panels >= 1080p, the 8x16 bitmap font doubles to 16x32 cells so text stays legible at high DPI.

---

## Double-Buffered Rendering

The loader allocates an off-screen RAM back buffer (`g_back`) with a tight `width * 4` stride. Every draw primitive writes through `g_fb` which aliases either the back buffer (when available) or VRAM directly (fallback).

### Dirty-Rectangle Presentation

The key performance fix: `ui_present()` only copies scanlines that actually changed. Per-scanline span arrays track `[min, max)` column extents for the current and previous frames. On real hardware with uncached VRAM, this reduces a ~4 MB full-screen copy to a few KB of cursor/particle rows -- the difference between smooth and choppy on real hardware.

The copy uses `rep movsq` (Enhanced REP MOVSB, available since Ivy Bridge) for large spans and a 32-bit word loop for small ones, with an `sfence` to drain WC buffers.

### VSync Gating

The loader probes VGA input-status register 0x3DA bit 3 for a live retrace signal. If found, full-screen flips are gated on vblank to prevent tearing. Partial flips (cursor/particle only) are never gated -- they are tiny and their tear is invisible.

### Scene Caching

The animated menu maintains three cached buffers:
1. **Background cache** (`g_bgcache`): The scaled background image or forest theme, restored each frame
2. **Scene cache** (`g_scenecache`): Background + menu panel + breadcrumb + icons + effects (CRT vignette/scanlines). Rebuilt only when menu content changes; on idle frames restored with one `memcpy`
3. **Per-window cache** (`wm_window.cache`): Snapshot of each window's visible pixels. Restored on idle frames instead of calling the expensive draw callback

---

## forebo.cfg Configuration

The config parser lives in `core/config.c` and reads `\forebo.cfg` from the ESP root. It uses a tolerant lexer + iterative block-stack parser that handles:

### Config Structure

```cfg
# Global settings
timeout = 10
default = 0
background = /forebo/background.tga
theme = midnight
color_bg = #0B1020
color_accent = 0x6AA9FF
remember_last = 1

# Menu entries
menuentry "Forest OS" {
    type = forest
    kernel = /forebo/kernel.elf
    icon = os
    cmdline = root=/dev/sda1
    module = /forebo/initrd.img
}

menuentry "Arch Linux" {
    type = linux
    vmlinuz = /EFI/arch/vmlinuz-linux
    initrd = /EFI/arch/initramfs-linux.img
    cmdline = root=UUID=xxxx
}

submenu "Advanced" {
    menuentry "Recovery" {
        type = shell
    }
}
```

### Supported Entry Types

| Type | Description |
|------|-------------|
| `forest` | Forest OS multiboot1 handoff (x86_64 only) |
| `linux` | Linux EFI-stub boot (PE image) |
| `chainload` / `chain` | Chainload another EFI app |
| `windows` / `win` | Alias for chainload with Windows Boot Manager path |
| `shell` | Open the interactive framebuffer shell |
| `recovery` | Open the Recovery/Disk Tools window |
| `tools` | Open the GUI Tools launcher |
| `settings` / `theme` | Open the live theme/style editor |
| `setup` / `firmware` | Reboot into UEFI firmware setup |
| `uefi_settings` | UEFI variable viewer/editor panel |
| `reboot` | Machine reset |

### Theme System

The config supports 11 named color themes: `forest`, `midnight`, `nord`, `dracula`, `gruvbox`, `solarized`, `amber`, `matrix`, `rose`, `ocean`, `mono`. Individual `color_*` keys override specific palette entries on top of the chosen theme.

### Menu Style Presets

30+ menu style presets are available: `classic`, `minimal`, `terminal`, `flat`, `modern`, `card`, `neon`, `outline`, `underline`, `invert`, `brackets`, `sidebar-left`, `sidebar-right`, `banner-top`, `dock-bottom`, `fullscreen`, `centered`, `compact`, `spacious`, `retro`, `glass`, `hacker`, `ribbon`, `framed`, `dashed`, `spotlight`, `pill`, `boxed`, `ghost`, `elegant`.

Each preset defines layout position, selection style, border type, corner radius, accent strip, dividers, gradient, shadow, title bar, icons, scrollbar, and caret visibility. Fine-grained `menu_*` overrides apply on top.

### Submenu Navigation

Configs can nest `submenu` blocks up to 8 levels deep. The menu renders one level at a time with a breadcrumb trail ("ForeB > CachyOS > Snapshot 906"). Keyboard: Enter/Right descends, Esc/Left ascends. Mouse: click a submenu row to descend.

### Default Resolution

The `default=` key accepts either an integer (N-th top-level row) or a title path (`"Advanced/Recovery"`). A "Descend Rule" automatically resolves submenu defaults to their first child. The `remember_last=1` option persists the last-booted entry index in a UEFI NV variable under a private GUID.

---

## Graphical Boot Menu

### Rendering Pipeline

Each frame composites layers back-to-front:
1. **Background**: `forebo.cfg` background image (BMP/TGA) scaled to screen, or the built-in forest theme (three-layer tree silhouette with gradient sky)
2. **Particles**: Subtle falling-leaves layer (anim.c), tinted with the theme accent color
3. **Menu panel**: Centered (or positioned per style) with gradient fill, shadow, border, and accent strip
4. **Icons**: Per-entry TGA/BMP icons blitted with alpha blending into each row's gutter
5. **Windows**: Any open WM windows composited over the menu
6. **Cursor**: Custom TGA/BMP sprite or built-in recolorable arrow

### Keyboard Controls

- **Up/Down**: Navigate entries (with smooth highlight bar slide animation)
- **Page Up/Down, Home/End**: Fast navigation
- **Enter**: Boot selected entry or descend into submenu
- **Right/Left**: Descend/ascend submenus
- **Esc**: Reboot (top level) or ascend submenu
- **c**: Open interactive shell
- **a**: Open About window
- **s**: Open Settings/Theme editor
- **u**: Open UEFI variable settings
- **Mouse wheel**: Free viewport scroll

### Mouse Support

The menu supports full mouse interaction:
- **Hover**: Highlights the row under the cursor
- **Click selected**: Activates the entry (boot/descend/shell)
- **Click unselected**: Selects the row
- **Scrollbar drag**: Draggable thumb for fast scrolling

### Countdown Timer

An auto-boot countdown (configurable via `timeout`) displays in the bottom-right corner with an animated progress bar. Any keypress or mouse interaction cancels the countdown.

---

## Mouse / Pointer Support

`core/input.c` provides a comprehensive pointer subsystem supporting three backends:

### 1. Absolute Pointer (USB Tablet)

The primary path under OVMF/QEMU. The loader:
1. Calls `ConnectController` recursively on ALL handles to force-bind USB HID drivers
2. Enumerates every handle carrying `EFI_ABSOLUTE_POINTER_PROTOCOL_GUID`
3. Scales the device's coordinate range to screen pixels
4. Picks the "live" device (one that reports non-origin positions) as authoritative

### 2. Simple Pointer (USB Mouse)

Relative deltas from `EFI_SIMPLE_POINTER_PROTOCOL`. The loader drains the entire queued report burst per frame (not just one `GetState` call) and accumulates deltas. The historic bug of dividing by `Mode->Resolution` is fixed -- raw deltas are used as pixel offsets.

### 3. Direct i8042 PS/2 Mouse (x86 only)

A firmware-independent fallback that directly programs the 8042 controller:
- Enables the auxiliary device (command 0xA8)
- Clears the "mouse clock disable" bit in the controller command byte
- Sends the IntelliMouse magic knock (200, 100, 80) to detect scroll wheel
- Polls status port 0x64 for aux-tagged bytes and integrates 3/4-byte packets

This is often the **only** working pointer on real hardware where OVMF exposes no working EFI pointer protocol (only the dead ConSplitter aggregate).

### Cursor Rendering

Two cursor modes:
- **Custom sprite**: A TGA/BMP image loaded from `img_cursor=` or `cursor=` in the config, drawn with alpha blending at 1:1 or scaled by `ui_scale()`
- **Built-in arrow**: A 12x19 pixel map with body (theme color) and outline (dark), drawn via `fill_rect` with run-length coalescing

---

## Window Manager

`core/wm.c` implements a tiny compositor supporting:

### Window Features

- **Title bar**: With focused/unfocused color states, truncation with "..."
- **Close box**: Red `[x]` button, or Esc key closes the focused window
- **Draggable**: Title bar drag with boundary clamping (window stays partially visible)
- **Z-ordering**: Click-to-raise, back-to-front compositing
- **Content caching**: Each window snapshots its visible pixels; on idle frames the cache is restored with one `memcpy` instead of calling the draw callback
- **Occlusion culling**: Top-down pass computes visible regions; fully covered windows are skipped entirely

### Window Skins

Three visual styles configurable via `window_skin=`:
- **Flat**: Thin outline, no 3D effects
- **Beveled**: Classic raised-edge 3D look with highlight/shadow
- **Glass**: Frosted backdrop (backdrop blur + translucent tint)

### Built-in Windows

- **About ForeB**: Version info, demonstrates the WM
- **Recovery/Disk Tools**: Block device inventory, launches shell tools
- **Settings/Theme editor**: Live customization of colors, styles, widgets
- **UEFI Settings**: View/edit UEFI runtime variables
- **Error boxes**: Report boot failures
- **GUI Tools launcher**: Grid of tool windows (Disk Info, GPT Viewer, etc.)

### Button Widget

A reusable button widget supports flat, raised, pill, outline, ghost, and glass styles with hover/press/disabled states, custom face images, and focus rings.

---

## Interactive Shell

`tools/shell.c` provides a full framebuffer-based shell with a scrolling text console (256 lines of scrollback, in-place line editing with caret).

### ESP File Operations

- **ls [path]**: List directory contents with size and type
- **cat <file>**: Print file contents (up to 128 KiB)
- **hexdump <file> [len]**: Hex+ASCII dump (default 256 bytes)
- **cd [dir]**: Change working directory (relative/absolute, with `.`, `..` support)
- **pwd**: Print current directory
- **background <file>**: Live-set the menu background image

### Block Device Operations

- **lsblk**: List all `EFI_BLOCK_IO_PROTOCOL` devices with block size, last LBA, size, flags
- **read <dev> <lba> [n]**: Read and hexdump n sectors (up to 64)
- **write <dev> <lba> <file>**: Destructive sector write with confirmation gate
- **drives**: List all `SimpleFileSystem` volumes with label, total/free space
- **devices**: Full hardware inventory -- keyboards, pointers, storage (with transport type via device path walking), audio note
- **inputtest**: Live keyboard + pointer echo for diagnostics

### UEFI Variable Operations

- **efivars**: Enumerate all UEFI runtime variables with name, GUID, attributes, size
- **bootvars**: Show Boot#### and BootOrder global variables
- **getvar <name> [guid]**: Read and display one variable (hex dump for binary, string for CHAR16)
- **setvar <name> <guid> <hex>**: Write a variable (with confirmation)

### Recovery / Disk Tools

- **gpt <dev>**: Parse and display the GPT header, disk GUID, partition table with type names (EFI System, Linux filesystem, MS basic data, etc.)
- **parts**: All block devices with GPT/MBR partition summary
- **fsprobe <dev>**: Identify filesystem by magic bytes (FAT32, ext2/3/4, btrfs, XFS, NTFS, exFAT, LUKS, ISO9660, Linux swap)
- **rescue <src> <dst> [skip-bad]**: Sector-level copy with bad-block skip and zero-fill, progress reporting
- **fatfix <dev>**: Check/repair FAT boot sector signature, FSInfo, backup boot sector
- **scan <dev>**: Best-effort file-magic carve scan (JPEG, PNG, GIF, PDF, ZIP, GZIP, ELF, BMP, RIFF)

### Filesystem-Specific Commands

- **ext-ls <dev> [path]**: List ext2/3/4 directory (read-only)
- **ext-cat <dev> <path>**: Print ext2/3/4 file contents (read-only, 1 MiB cap)
- **btrfs-snaps <dev>**: List btrfs subvolumes/snapshots

### Shell Meta-Commands

- **config**: Reload `forebo.cfg` from the ESP
- **boot [idx|title]**: Boot an entry by index or title
- **modules [add <path>]**: List/append module paths for the current entry
- **clear**: Clear the screen
- **reboot**: Machine reset
- **exit**: Return to the menu
- **setup / firmware**: Reboot into UEFI firmware setup
- **tools**: Open the GUI Tools launcher
- **basic**: Launch an interactive BASIC interpreter

---

## Boot Methods

### Forest OS (Multiboot1 Handoff)

The primary boot path for Forest OS:

1. **Load kernel ELF** from the ESP path (`/forebo/kernel.elf` default) with animated chunked progress bar (256 KiB per read)
2. **Parse ELF**: Supports both ELF32 and ELF64, discovers entry point and PT_LOAD segment bounds
3. **Reserve physical memory**: `AllocatePages(AllocateAddress)` for the kernel load range and the fixed low-RAM region (0x1000..0x7FFF)
4. **Build boot info**: Populates `multiboot_info` at 0x1800 and `foreboots_boot_info` at 0x1000 with framebuffer, memory map, cmdline, module wiring
5. **Load modules**: `modules_load()` reads ESP files into page-allocated memory below 4 GiB and wires them into the mb_module array
6. **ExitBootServices**: Spins `GetMemoryMap` -> `ExitBootServices` in a retry loop (up to 16 attempts) with nothing but port-I/O serial between calls
7. **Stage segments**: Copies PT_LOAD segments to their p_paddr, zeros BSS tails
8. **Build E820**: Converts EFI memory map to E820 arrays at 0x1100 (forebo) and 0x1400 (multiboot)
9. **Handoff**: `forebo_handoff(entry, MULTIBOOT1_MAGIC, mbi_addr)` -- tears down long mode to 32-bit PM and jumps to the kernel

### Linux EFI-Stub Boot

Boots a Linux kernel via the EFI-stub mechanism (`boot/linux.c`):

1. Read the vmlinuz PE image from the ESP
2. `LoadImage` from the source buffer (not file path)
3. Set command line via `LoadOptions` (CHAR16)
4. If an initrd is configured, publish it via `EFI_LOAD_FILE2_PROTOCOL` with the `LINUX_EFI_INITRD_MEDIA_GUID` vendor device path
5. `StartImage` -- returns only on failure

### Chainloading

Loads and starts another EFI application (`boot/chain.c`):

- **Explicit path**: `chain = /EFI/whatever/loader.efi` -- tries ForeB's ESP first, then all volumes
- **Auto-scan**: Without a `chain=` path, scans every `SimpleFileSystem` volume for standard loader paths:
  - `\EFI\BOOT\BOOTX64.EFI`
  - `\EFI\grub\grubx64.efi`
  - `\EFI\ubuntu\grubx64.efi`
  - `\EFI\debian\grubx64.efi`
  - `\EFI\fedora\grubx64.efi`
  - `\EFI\arch\grubx64.efi`
  - `\EFI\Microsoft\Boot\bootmgfw.efi`
  - Various shim paths

Builds a full device path (volume's device path + `MEDIA_FILEPATH` node) so the target loader gets a proper `DeviceHandle`. Falls back to loading from a source buffer when a volume has no device path.

---

## Filesystem Support

### FAT via Firmware

The ESP is accessed through `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` -- firmware handles FAT12/16/32 natively. All kernel, initrd, config, and image loading goes through this path.

### ext2/3/4 Read-Only Driver

`fs/fs_ext.c` provides a self-contained, read-only ext2/3/4 driver for the shell:

- **Superblock**: Parses magic (0xEF53), dynamic block size (1K-64K), 64-bit feature for descriptor sizing
- **Inode read**: Supports 128-1024 byte inodes, 64-bit file sizes, all 15 i_block entries
- **Block mapping**: Both classic indirect block maps (direct/single/double/triple) AND ext4 extent trees (ext4_extent_header/idx/extent with recursive index descent)
- **Directory walk**: Linear scan of ext4_dir_entry_2 records (no htree acceleration, but correct on all volumes)
- **Path resolution**: Absolute paths from root inode

Honest limitations: no journal replay, no htree, no inline_data, no symlink following, no bigalloc. This is a recovery/diagnostic driver, not a read-write filesystem.

### Btrfs Detection

`fs/fs_btrfs.c` probes the superblock at offset 0x10040 for `_BHRfS_M` magic and can list subvolumes/snapshots (read-only).

---

## Recovery / Disk Tools

### GPT Viewer

The `gpt <dev>` shell command parses the GPT header at LBA 1, validates the `EFI PART` signature, reads the partition entry array, and displays:
- Disk GUID
- Usable LBA range
- Partition count and size
- Partition type names (from a lookup table of known GUIDs)
- Partition names (CHAR16 at offset 56 in each entry)

Falls back to MBR detection (0x55AA signature at bytes 510-511) when no GPT is found.

### Partition Browser

`parts` enumerates all Block I/O handles, displays device type (disk/partition), size, block size, and flags (removable, read-only, no-media). For whole disks, attempts GPT parse, then MBR peek.

### Hex Viewer

`read <dev> <lba> [n]` reads up to 64 sectors and displays them in a classic hex dump format with offset, hex bytes (grouped by 8), and ASCII representation.

### Sector Rescue

`rescue <src> <dst> [skip-bad]` performs a sector-level copy with:
- Device-to-device or device-to-file destinations
- Bad block skip mode: reads sectors individually on error, zero-fills unreadable ones
- Progress reporting (every 5%)
- Esc to abort
- Hard confirmation gate ("Type 'yes' to proceed")

### Filesystem Probe

`fsprobe <dev>` reads the first ~65 KiB of a device and identifies the filesystem by magic bytes. Supports: NTFS, exFAT, FAT32, FAT12/16, ext2/3/4 (with feature flag analysis), btrfs, XFS, LUKS, ISO9660, Linux swap.

### File Carve Scanner

`scan <dev>` performs a best-effort file-magic carve over the first 128 MiB of a device, searching for: JPEG (`FF D8 FF`), PNG (`89PNG`), GIF (`GIF8`), PDF (`%PDF`), ZIP (`PK\x03\x04`), GZIP (`1F 8B`), ELF (`7FELF`), BMP (`BM`), RIFF (`RIFF`). Reports hit counts and first-seen offsets.

---

## Module and Initrd Loading

`core/modules.c` handles multiboot1 module loading:

1. For each `module = /path/to/initrd.img` in the config entry:
   - Opens the file from the ESP
   - Queries file size via `GetInfo(EFI_FILE_INFO)`
   - Allocates pages below 4 GiB via `AllocatePages(AllocateMaxAddress, EfiLoaderData)`
   - Reads the file in 256 KiB chunks

2. Populates the `mb_module[]` array at 0x1900 with `mod_start`/`mod_end` addresses and copies the path string into the low string pool

3. The first module is also mirrored into `foreboots_boot_info` as the primary initrd

4. After ExitBootServices, the kernel reads the module array from the fixed address via `MB_FLAG_MODS` in the multiboot info struct

---

## ExitBootServices and Kernel Handoff

The transition from firmware to kernel is the most critical phase:

1. **Pre-EBS**: All UI drawing switches to pure MMIO stores (framebuffer writes still work after ExitBootServices). A fade-to-black animation plays.

2. **GetMemoryMap loop**: The loader over-provisions the map buffer once, then spins `GetMemoryMap` -> `ExitBootServices` up to 16 times. Between calls, **nothing** touches firmware memory -- only port-I/O serial logging. This avoids the classic map key invalidation bug.

3. **Post-EBS segment staging**: Copies each ELF PT_LOAD segment to its physical address, zeros BSS tails. The framebuffer is still writable, so `ui_progress()` and `ui_status()` continue to draw (pure MMIO stores, no firmware calls).

4. **E820 build**: Converts the EFI memory descriptor array to E820 format at fixed low-memory addresses (0x1100 for forebo, 0x1400 for multiboot).

5. **Structure completion**: Fills memory-map-dependent fields in `multiboot_info` (mem_upper, mmap_addr/length) and `foreboots_boot_info` (mem_upper, mmap_count/addr).

6. **Long mode teardown**: `forebo_handoff(kentry, MULTIBOOT1_MAGIC, mbi_addr)` -- an assembly trampoline (`handoff64to32.asm`) clears CR0.PG, tears down to 32-bit protected mode, loads a GDT, and jumps to the kernel ELF entry with EAX=0x2BADB002, EBX=mbi_addr.

---

## BMP/TGA Image Loading

`core/image.c` provides self-contained BMP and TGA decoders:

### BMP Support

- **BITMAPINFOHEADER** (40+ bytes)
- 24-bit and 32-bit color depths
- BI_RGB (uncompressed) and BI_BITFIELDS (channel masks)
- Bottom-up (default) and top-down (negative height)
- Row padding to 4-byte boundary
- Alpha detection: 32-bit BMPs with all-zero alpha channels are forced opaque

### TGA Support

- **Type 2** (uncompressed true-color) and **Type 10** (RLE compressed)
- 24-bit and 32-bit color depths
- Top-down and bottom-up (descriptor bit 5)
- RLE packets with run-length encoding

### Blitters

- **`img_blit_scaled`**: Nearest-neighbor scaling with fixed-point 16.16 stepping (two divisions total per blit)
- **`img_blit_alpha`**: Per-pixel alpha compositing over the framebuffer
- **`img_blit_alpha_scaled`**: Combined scaling + alpha blending
- All blitters intersect with the screen bounds AND the active UI clip rect
- R/B channel swap handled at store time when the framebuffer is RGBX

### Usage in the Loader

- **Background image**: Scaled to fill the screen on every menu repaint
- **Per-entry icons**: Alpha-blitted into each menu row's gutter, scaled to fit the row height
- **Custom cursor**: Drawn at 1:1 or scaled by `ui_scale()`
- **Window chrome**: Custom titlebar, panel, window face, and button images replace the drawn look

---

## Additional Features

### PC-Speaker Audio

Configurable UI tones via `pcspeaker = on` and per-event frequency/duration keys (`audio_nav_freq`, `audio_select_ms`, etc.). Tones play on navigation, selection, open, error, and back events.

### Firmware Setup

`fw_boot_to_setup()` sets the `OsIndications` UEFI variable to request firmware setup on next boot, then resets. Falls back to a plain reboot when unsupported.

### NV Persistence

`settings_nv.c` persists Settings/Theme edits to UEFI NV variables so they survive reboots, loaded before the menu runs.

### Status Bar

A thin status bar at the top of the screen shows mouse connectivity status and contextual messages during boot operations.

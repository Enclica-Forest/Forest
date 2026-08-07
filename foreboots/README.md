# ForeB - Forest Bootloader

ForeB is the default bootloader for Forest OS on **both BIOS/CSM and native
UEFI** firmware. It replaces GRUB while remaining **fully Multiboot1-compatible**
with the kernel: the kernel is handed `EAX=0x2BADB002`, `EBX=&multiboot_info_t`
exactly as GRUB does, so no kernel changes are required.

Two independent firmware front-ends share one kernel handoff contract:

- **BIOS/CSM path** — NASM real-mode/protected-mode stages (`stage1`/`stage2`/
  `stage3`) documented below. Kernel is read from raw disk sectors.
- **UEFI (native) path** — a self-contained 64-bit EFI application
  (`uefi/bootx64.c`) that uses GOP + `GetMemoryMap` + `ExitBootServices`, loads
  the kernel from the ESP FAT filesystem, tears long mode back down to 32-bit
  protected mode, and performs the **identical** Multiboot1 handoff. See
  [UEFI (native) boot](#uefi-native-boot) and [`uefi/README.md`](uefi/README.md).

Both paths jump to the kernel ELF entry in 32-bit PM with the same register
state and the same `multiboot_info_t` at `0x1800`; the kernel cannot tell which
firmware booted it.

## Stages

| Stage    | File        | Mode            | Size    | Load address | Role                          |
|----------|-------------|-----------------|---------|--------------|-------------------------------|
| Stage 1  | `stage1.asm`| 16-bit real     | 512 B   | `0x7C00`->`0x600` (reloc) | MBR: relocate, LBA-probe, load stage2 |
| Stage 2  | `stage2.asm`| 16-bit real     | 8 KiB   | `0x8000`     | A20, E820, VBE, menu, disk load, build multiboot info, enter PM |
| Stage 3  | `stage3.asm`| 32-bit PM (opt. 64) | 8 KiB | `0x5000`   | Parse kernel ELF, copy PT_LOAD segments, hand off to kernel |

### Disk layout (LBA sectors, 512 B each)

| Sectors   | Content                |
|-----------|------------------------|
| 0         | stage1 (MBR + partition table + `0xAA55`) |
| 1..16     | stage2 (8 KiB)         |
| 17..32    | stage3 (8 KiB)         |
| 48+       | kernel ELF image       |
| (past kernel) | optional initrd / multiboot module (`BIOS_INITRD_SECTOR`) |

All offsets are tunable in `config.h` and overridable via NASM `-D`. The BIOS
initrd sector is `FOREB_INITRD_START_SECTOR` (0 = disabled in `config.h`); the
`Makefile` computes a safe default just past the kernel and passes it to stage2.

## Boot protocol (kernel handoff)

ForeB loads the kernel ELF (ELF32 for ARCH=32, ELF64 for ARCH=64), copies each
`PT_LOAD` segment to its physical address (`p_paddr`), zero-fills BSS
(`p_memsz - p_filesz`), and jumps to the ELF entry point with:

```
EAX = 0x2BADB002   (MULTIBOOT1_MAGIC)
EBX = &multiboot_info_t   (at MULTIBOOT_INFO_ADDR = 0x1800)
EIP = kernel ELF entry (e.g. 0x100000)
```

The kernel is entered in **32-bit protected mode** with flat segments. This
matches the Forest OS kernel's `src/boot.asm` (ARCH=32) and `src/boot64.asm`
(ARCH=64) entry: both begin in 32-bit PM and read `EAX`/`EBX`; the 64-bit
kernel then performs its own long-mode transition. **ForeB does not need to
switch to long mode for the current kernel.**

### `multiboot_info_t` fields populated

ForeB builds a standard Multiboot1 info struct (matching
`src/include/multiboot.h`) with these `flags` bits set as applicable:

| Flag              | Value     | Fields                                          |
|-------------------|-----------|-------------------------------------------------|
| `MB_FLAG_MEM`     | `0x001`   | `mem_lower` (640), `mem_upper` (KiB above 1 MiB)|
| `MB_FLAG_CMDLINE` | `0x004`   | `cmdline` (physical pointer)                    |
| `MB_FLAG_MMAP`    | `0x040`   | `mmap_addr`, `mmap_length` (Multiboot1 entries) |
| `MB_FLAG_BOOTLOADER` | `0x200`| `boot_loader_name` (physical pointer)           |
| `MB_FLAG_FRAMEBUFFER`| `0x1000`| `framebuffer_addr` (64-bit), `pitch`, `width`, `height`, `bpp`, `type` |
| `MB_FLAG_MODS`    | `0x008`   | `mods_count`=1, `mods_addr` (initrd, if enabled)|

`framebuffer_type` is `0` (indexed) for 8 bpp and `1` (RGB) for >=16 bpp. The
"no framebuffer" menu entry sets `framebuffer_type=2` (EGA text) and clears the
flag so the kernel falls back to the VGA text console.

## `foreboots_boot_info` structure

ForeB also builds a richer, ForeB-native struct at `BOOT_INFO_ADDRESS`
(`0x1000`). It is the bootloader's internal representation and a forward-looking
extension point (the kernel may optionally read it for 64-bit memory map data,
CPU capability info, etc.). The C layout below **must stay in sync** with the
NASM `struc foreboots_boot_info` in `config.h`.

```c
#define FOREB_BOOT_INFO_MAGIC  0x464F5242  /* "FORB" */
#define FOREB_BOOT_INFO_VER    0x00020000  /* v2.0 */

/* foreboots_boot_info.flags bits */
#define FOREB_BIF_MMAP         0x00000001
#define FOREB_BIF_FRAMEBUFFER  0x00000002
#define FOREB_BIF_CMDLINE      0x00000004
#define FOREB_BIF_LONG_MODE    0x00000008
#define FOREB_BIF_CPUID        0x00000010
#define FOREB_BIF_PAE          0x00000020
#define FOREB_BIF_INITRD       0x00000040
#define FOREB_BIF_NO_FB        0x00000080
#define FOREB_BIF_SAFE         0x00000100

struct foreboots_mmap_entry {
    uint64_t base;        /* physical base address          */
    uint64_t length;      /* region length                  */
    uint32_t type;        /* E820 type: 1=usable, 2=reserved, ... */
    uint32_t acpi;        /* ACPI 3.0 extended attributes   */
} __attribute__((packed));  /* 24 bytes */

struct foreboots_boot_info {
    uint32_t magic;                 /* FOREB_BOOT_INFO_MAGIC            */
    uint32_t version;               /* FOREB_BOOT_INFO_VER              */
    uint32_t flags;                 /* FOREB_BIF_* bitmask              */
    uint32_t boot_disk;             /* BIOS boot drive number (DL)      */
    uint32_t cmdline;               /* physical addr of cmdline string  */
    uint32_t boot_loader_name;      /* physical addr of bootloader name */
    uint32_t mem_lower;             /* KiB of usable memory below 1 MiB */
    uint32_t mem_upper;             /* KiB of usable memory above 1 MiB */
    uint32_t mmap_count;            /* number of valid mmap entries     */
    uint32_t mmap_addr;             /* physical addr of mmap array      */
    /* Framebuffer (valid when flags & FOREB_BIF_FRAMEBUFFER) */
    uint64_t framebuffer_addr;      /* 64-bit physical LFB address      */
    uint32_t framebuffer_pitch;     /* bytes per scanline               */
    uint32_t framebuffer_width;     /* pixels                           */
    uint32_t framebuffer_height;    /* pixels                           */
    uint32_t framebuffer_bpp;       /* bits per pixel                   */
    uint32_t framebuffer_type;      /* 0=indexed, 1=RGB, 2=EGA text     */
    uint16_t vbe_mode;              /* VBE mode number set (0 if text)  */
    uint16_t vbe_pad;
    /* CPU capability detection */
    uint32_t cpuid_available;
    uint32_t long_mode_available;
    uint32_t pae_available;
    /* Kernel image info (filled by stage2, used by stage3) */
    uint32_t kernel_load_addr;      /* physical addr of raw ELF buffer  */
    uint32_t kernel_size;           /* bytes loaded                     */
    uint32_t kernel_entry;          /* ELF e_entry (set by stage3)      */
    uint32_t kernel_is64bit;        /* 1 = ELF64, 0 = ELF32 (stage3)    */
    /* Initrd */
    uint32_t initrd_addr;           /* physical addr (0 if none)        */
    uint32_t initrd_size;
    /* Selected boot entry + mode flags */
    uint32_t boot_entry;            /* ENTRY_* selected in menu         */
    uint32_t no_framebuffer;        /* 1 = text-mode boot requested     */
    uint32_t safe_mode;             /* 1 = safe-mode boot requested     */
    uint32_t reserved[8];           /* growth room                     */
} __attribute__((packed));  /* 152 bytes */
```

The E820 memory map (`foreboots_mmap_entry[32]`) is stored at
`FOREB_MMAP_ADDRESS` (`0x1100`); the Multiboot1-compatible mmap array is built
at `MB_MMAP_ADDRESS` (`0x1400`).

## Low-memory map

| Address      | Use                                              |
|--------------|--------------------------------------------------|
| `0x0500`     | Disk Address Packet (DAP)                        |
| `0x0520`     | E820 scratch entry                               |
| `0x0600`     | stage1 relocated MBR                             |
| `0x1000`     | `foreboots_boot_info` (152 B)                    |
| `0x1100`     | ForeB E820 mmap array (32 x 24 B)                |
| `0x1400`     | Multiboot1 mmap array (32 x 24 B)                |
| `0x1800`     | `multiboot_info_t` (kernel handoff)              |
| `0x2000`     | VBE controller info (512 B)                      |
| `0x2200`     | VBE mode info (256 B)                            |
| `0x2400`     | DAC palette buffer (768 B)                       |
| `0x5000`     | stage3 (8 KiB)                                   |
| `0x8000`     | stage2 (8 KiB)                                   |
| `0x10000`    | kernel ELF file buffer (up to 256 KiB)           |
| `0x70000`    | real-mode stack (ss=0x7000, sp=0xFFFE)           |

## Boot menu

Both firmware paths render the **same forest-themed graphical menu** — drawn
directly to the framebuffer (BIOS: VBE LFB; UEFI: GOP), pixel-for-pixel
identical: dark-green background, tree logo, `[ Boot Menu ]` panel, highlighted
selection bar, footer hint line, and an amber auto-boot countdown. Navigation:
**Up/Down** to move, **Enter** to boot, **Esc** to reset the countdown.

The menu is a partial-repaint UI: the whole menu is painted once, then only the
countdown box (and, during load, the progress bar) is re-filled in place — the
screen never scrolls. On BIOS, if VBE is unavailable a text-mode fallback is
used. See [Shared UI assets](#shared-ui-assets).

### In-place load progress bar

Neither path prints scrolling status text while loading the kernel. Instead a
fixed **progress bar rectangle** inside the menu panel is advanced in place as
the kernel is read/staged, and all verbose diagnostics go to **serial (COM1)
only**. On UEFI this specifically avoids the slow per-newline `ConOut` scroll
that a hi-res GOP-backed firmware text console incurs (each newline memmoves the
whole screen); the loader draws the bar with direct framebuffer writes and never
calls `ConOut` on the load path.

### Shared UI assets

Both renderers share the exact same font, palette, and geometry so the two
firmwares look identical:

- [`include/font8x8.h`](include/font8x8.h) — the 768-byte public-domain
  `font8x8_basic` 8x8 glyph set (ASCII 32..127), rendered **LSB-first**.
- [`include/forebo_theme.h`](include/forebo_theme.h) — the 13 forest-theme
  colors as RGB888 and the menu layout/geometry constants.
- [`UI_SPEC.md`](UI_SPEC.md) — the authoritative UI specification (colors,
  coordinates, draw order) both paths implement.

The BIOS path indexes these colors through the 8bpp DAC palette; the UEFI path
writes the RGB888 values straight to the 32bpp GOP framebuffer (no palette).

| # | Entry                         | Cmdline  | Video           |
|---|-------------------------------|----------|-----------------|
| 0 | Forest OS (default)           | `""`     | 32bpp VBE chain |
| 1 | Forest OS (no framebuffer)    | `nofb`   | VGA text 03h    |
| 2 | Forest OS (safe mode)         | `safe`   | 32bpp VBE chain |
| 3 | Reboot                        | -        | - (INT 19h)     |

## VBE mode selection

Stage 2 enumerates all VBE modes from the controller's mode list and picks the
first exact match (width x height x bpp) with a linear framebuffer (packed or
direct color model). The **GUI menu** is rendered in an 8bpp mode (800x600x8 ->
640x480x8) because the built-in pixel/font primitives are 8bpp. After the user
selects a framebuffer-capable entry, stage 2 re-selects the **kernel**
framebuffer mode from the 32bpp chain:

```
1920x1080x32 -> 1280x720x32 -> 1024x768x32 -> 800x600x32 -> 640x480x32
-> VGA text mode 03h
```

The chosen mode's `PhysBasePtr`, pitch, width, height, bpp are recorded in
`foreboots_boot_info` and propagated to the kernel's `multiboot_info_t`.

## 32-bit vs 64-bit kernel paths

- **Default (FOREB_FORCE_LONG_MODE=0):** stage 3 enters the kernel in 32-bit
  PM with `EAX=0x2BADB002`, `EBX=&multiboot_info_t`, `EIP=e_entry`. This works
  for both ARCH=32 and ARCH=64 Forest OS kernels, because the 64-bit kernel's
  `boot64.asm` performs its own CPUID/paging/long-mode transition after entry.
- **Optional long-mode trampoline (FOREB_FORCE_LONG_MODE=1):** if the CPU
  supports long mode (detected in stage 2 via CPUID) and the kernel is ELF64,
  stage 3 builds identity-mapped page tables for the low 2 MiB (PML4/PDPT/PD at
  `0x10000`), enables PAE + EFER.LME + paging, loads a 64-bit GDT, and
  far-jumps to the kernel entry in 64-bit mode. This is intended for future
  kernels with a native 64-bit entry point; it is **off by default** and falls
  back to the 32-bit PM handoff if long mode is unavailable.

## UEFI (native) boot

On UEFI firmware ForeB runs as a self-contained 64-bit EFI application,
`uefi/bootx64.c`, built to `BOOTX64.EFI` and placed on the EFI System
Partition (ESP) as `\EFI\BOOT\BOOTX64.EFI` (the removable-media default). It is
**freestanding** — no gnu-efi, no libc, no CRT — and defines the EFI structs it
needs inline.

### How the UEFI path works

1. **Firmware entry** — `efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE *)` runs in
   64-bit long mode with firmware paging on and boot services available.
2. **Load the kernel** — open the ESP's Simple File System and read the kernel
   ELF from `\forebo\kernel.elf` into an `AllocatePages` buffer. The same ELF is
   used by BIOS and UEFI (ELF32 for ARCH=32, ELF64 for ARCH=64).
3. **Framebuffer via GOP** — locate the Graphics Output Protocol and record
   `Mode->FrameBufferBase`, `HorizontalResolution`, `VerticalResolution`,
   `PixelsPerScanLine * 4` (pitch), 32 bpp. GOP is always RGB, so
   `framebuffer_type = 1`.
   1a. **Graphical menu** — before touching the memory map, draw the forest
   menu directly on the GOP framebuffer (shared font + theme, see
   [Shared UI assets](#shared-ui-assets)) and poll `ConIn->ReadKeyStroke` +
   `gBS->Stall` for Up/Down/Enter to pick the boot entry. The menu completes
   **before** `GetMemoryMap` so no allocation invalidates the map key. The
   framebuffer draw + progress code lives in `uefi/ui.c`.
4. **Reserve low memory** — `AllocatePages` (or verify conventional) the fixed
   ForeB low-RAM region (`0x1000`..`0x27FF`) and the kernel `PT_LOAD`
   destination (e.g. `0x100000`) so they survive `ExitBootServices`.
5. **`GetMemoryMap`** — snapshot the EFI memory map, convert each descriptor to
   an E820-style `foreboots_mmap_entry` / Multiboot1 `mb_mmap_entry`
   (`EfiConventionalMemory`/`BootServices*`/`Loader*` -> 1 usable;
   `ACPIReclaim` -> 3; `ACPIMemoryNVS` -> 4; else -> 2 reserved), and derive
   `mem_lower` (640) / `mem_upper` (sum of usable RAM above 1 MiB, KiB).
6. **`ExitBootServices`** — call with the map key returned by the final
   `GetMemoryMap`; after this the firmware no longer owns memory.
6a. **Load progress bar** — the kernel file is read in chunks and an in-place
   progress bar is advanced on the GOP framebuffer per chunk (direct writes, no
   `ConOut`, no scroll). Detailed load logs stay on serial only.
7. **Build handoff structs** — parse the ELF and copy each `PT_LOAD` (filesz
   bytes, then zero the BSS tail) to `p_paddr`; write `multiboot_info_t` at
   `0x1800`, the mmap arrays at `0x1400`/`0x1100`, `foreboots_boot_info` at
   `0x1000`, and NUL-terminated `cmdline` / `boot_loader_name` strings in
   identity-mapped low RAM.
8. **Tear down long mode** — load a 32-bit flat GDT (null / `0x08` code
   `0x00CF9A..FFFF` / `0x10` data `0x00CF92..FFFF`), leave the long-mode
   submode, clear `CR0.PG`, clear `EFER.LME`, clear `CR4.PAE`, and far-jump into
   32-bit CS. **The loader does not re-enter long mode** — the 64-bit kernel does
   its own long-mode transition after entry, exactly as on BIOS.
9. **Mask the PICs and hand off** — `out 0x21, 0xFF` / `out 0xA1, 0xFF`, then
   jump to the ELF `e_entry` with `EAX=0x2BADB002`, `EBX=0x1800`, interrupts off,
   paging off, flat 32-bit segments. Byte-for-byte the same as `stage3.asm`.

### `uefi/` file layout

| File                | Role                                                        |
|---------------------|-------------------------------------------------------------|
| `uefi/bootx64.c`    | The EFI loader: inline EFI structs, ELF parse, mmap build, long-mode teardown, Multiboot handoff |
| `uefi/efi.h`        | Inline EFI type/protocol definitions (GOP, `SimpleTextInput`, Block I/O, Runtime variable fns, `LocateHandleBuffer`) |
| `uefi/ui.c`         | GOP-drawn forest menu + progress bar (now an **8x16** font w/ 2x scaling, background-image + icon blit) |
| `uefi/image.c`      | BMP + TGA decoders and the background / alpha-icon blitters |
| `uefi/config.c`     | `forebo.cfg` parser -> `struct forebo_config` |
| `uefi/modules.c`    | `module`/`module2` loader -> multiboot1 `mods` array (`MB_FLAG_MODS`) |
| `uefi/shell.c`      | Interactive GOP shell (press `c` at the menu, or the *ForeB Shell* entry); see `SHELL.md` |
| `uefi/anim.c`       | Menu fade-in / parallax + animated spinner and smooth load bar |
| `uefi/ui.c` (double buffer) | All draws now target an off-screen RAM back buffer; `ui_present()` blits it to VRAM once per frame (tear-free) |
| `uefi/input.c`      | Mouse/pointer layer: `EFI_SIMPLE_POINTER` + `EFI_ABSOLUTE_POINTER`, cursor sprite composited each frame |
| `uefi/wm.c`         | Tiny window manager/compositor: draggable, z-ordered, focusable windows (titlebar drag, close box) |
| `uefi/linux.c` / `boot_linux.c` | Boot an EFI-stub `vmlinuz` (LoadImage/StartImage) with cmdline as LoadOptions + initrd via `LINUX_EFI_INITRD_MEDIA` LoadFile2 |
| `uefi/chain.c` / `chainload.c` | Enumerate all SimpleFS/BlockIo volumes (incl. USB) and chainload another EFI loader (`\EFI\BOOT\BOOTX64.EFI`, `grubx64.efi`) |
| `uefi/fs_ext.c`     | Read-only ext2/3/4 (superblock + inode + extent tree): list / cat for the shell + recovery |
| `uefi/fs_btrfs.c`   | btrfs detection + subvolume/snapshot listing (read-only, best-effort) |
| `uefi/arch.h`       | Arch abstraction: x86-only Multiboot handoff is gated on `FOREB_MULTIBOOT_SUPPORTED`; UI/shell/linux/chainload build on all three arches |
| `uefi/efi_ext.h`    | Extended protocol decls: pointer protocols, device paths, LoadFile2, Disk I/O, LoadImage/StartImage typedefs |
| `uefi/README.md`    | Self-contained-EFI approach + clang/lld build notes         |
| `BOOTX64.EFI` / `BOOTAA64.EFI` | Build output (PE32+ EFI app: all `uefi/*.o` + handoff trampoline on x64), staged to the ESP |

The loader `#include`s the shared `include/boot_protocol.h` so the struct
layouts, magic values, and flag bits are identical to the NASM side (which
`%include`s `include/boot_protocol.inc`). Do not redefine those symbols.

### UEFI build / run

```
make uefi           # clang -target x86_64-unknown-windows ... -> BOOTX64.EFI
make esp            # stage BOOTX64.EFI + kernel.elf into a FAT ESP image
make qemu-uefi      # boot the ESP under OVMF (edk2) in QEMU
make iso-hybrid     # hybrid ISO: El Torito (BIOS) + EFI System Partition (UEFI)
make qemu-uefi-iso  # boot the hybrid ISO under OVMF in QEMU
```

The UEFI module set is **auto-discovered**: the Makefile globs every `uefi/*.c`
(except the loader and `ui.c`, which have dedicated rules) and links them into
`BOOTX64.EFI`, so new feature modules build with no Makefile edits. The MS-ABI
target is compiled with `-mno-stack-arg-probe` (no CRT `__chkstk` in this
freestanding link). To stage a real Linux payload for a `type=linux` entry:
`make esp VMLINUZ=/boot/vmlinuz LINUX_INITRD=/boot/initrd.img` (otherwise the ESP
gets a `\forebo\vmlinuz.README` placeholder).

#### Cross-architecture UEFI (aarch64 + riscv64)

ForeB's UEFI C tree compiles for three UEFI arches from one source (`uefi/arch.h`).
The x86-only Forest Multiboot handoff is `#ifdef`-guarded, so the non-x86 builds
link **without** the NASM trampoline and expose UI + shell + Linux boot +
chainload + filesystems + recovery.

```
make uefi-aa64      # clang -target aarch64-unknown-windows -> BOOTAA64.EFI (real PE)
make esp-aa64       # FAT ESP with BOOTAA64.EFI + forebo.cfg + assets
make qemu-aa64      # boot BOOTAA64.EFI under aa64 OVMF (/usr/share/edk2/aa64)
make uefi-riscv     # compile all uefi/*.c to RISC-V objects + link a PIE ELF
make uefi-all       # x64 + aa64 PE, then the riscv objects (never fails)
```

`BOOTAA64.EFI` is a fully bootable ARM64 EFI app — verified to reach the ForeB
menu (forest background, mouse cursor, and the Linux/chainload/shell/recovery
entries) under `qemu-system-aarch64 -M virt -cpu cortex-a57` with the
`/usr/share/edk2/aa64/QEMU_EFI.fd` firmware. The ESP is attached as a **removable
USB** volume (`qemu-xhci` + `usb-storage,bootindex=0`) so OVMF's BDS reliably
auto-boots `\EFI\BOOT\BOOTAA64.EFI`; cross-arch runs use TCG (no KVM) so the menu
takes ~20-30 s to render.

**riscv64 packaging boundary (honest note):** every `uefi/*.c` compiles to RISC-V
objects and links to a static-PIE ELF (proving the C is portable), but a bootable
`BOOTRISCV64.EFI` **PE** cannot be produced with the installed toolchain — clang/lld
have no RISC-V COFF/PE backend and this binutils `objcopy` has no `pei-riscv64`
target. Producing `BOOTRISCV64.EFI` needs edk2 `GenFw` (BaseTools) or a binutils
built with RISC-V PE support; `make uefi-riscv` prints these steps.

`make esp` stages the loader to `\EFI\BOOT\BOOTX64.EFI`, the kernel to
`\forebo\kernel.elf`, and (this upgrade) the config `\forebo\forebo.cfg`, the
background `\forebo\bg.bmp`, per-entry icons `\forebo\icons\*.tga`, and the
sample module `\forebo\initrd.tar`. The background/icons are (re)generated from
the theme by `tools/gen_assets.py` during the `esp`/`image` asset stage. The
`qemu-uefi*` targets run with OVMF firmware,
copying `OVMF_VARS` to a writable temp so the pflash is not modified:

```
qemu-system-x86_64 -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=<copy>/OVMF_VARS.4m.fd \
  -drive format=raw,file=<esp-or-iso> -serial stdio -no-reboot
```

`make iso-hybrid` produces a single medium that boots on either firmware: BIOS
machines use the El Torito no-emulation catalog (ForeB stages); UEFI machines
use the embedded ESP and load `BOOTX64.EFI`.

## UEFI compositor, boot methods, filesystems & recovery (this upgrade)

The native UEFI path gains a desktop-like front end and several real boot
methods. All of it is x86 **and** aarch64 (see the cross-arch notes above).

- **Double buffering (foundational).** Every `ui_*` draw now targets an
  off-screen RAM back buffer; a single `ui_present()` blits it to the GOP front
  buffer (VRAM) per frame, so the menu, animations, cursor and windows are
  tear-free. The public draw API is unchanged — `put_pixel`/`fill_rect` were
  redirected and everything above them (glyphs, panels, progress bar) follows.
- **Mouse (fixed tracking).** `uefi/input.c` now enumerates pointer devices with
  `LocateHandleBuffer` over **every** handle exposing
  `EFI_ABSOLUTE_POINTER_PROTOCOL` / `EFI_SIMPLE_POINTER_PROTOCOL`, resets and
  polls **all** of them each frame and merges the result (instead of binding a
  single arbitrary `LocateProtocol` instance, which under OVMF often picked a
  ConSplitter aggregate that never reported motion — the old "cursor visible but
  frozen" bug). Absolute devices (QEMU `usb-tablet`) map their `Mode` range onto
  the screen; relative devices (`usb-mouse`) accumulate raw deltas directly.
  **Honesty note:** OVMF (no CSM) does **not** surface a PS/2 pointer to UEFI
  apps, so ForeB supports **USB** pointers only (`-device usb-tablet` /
  `-device usb-mouse`); PS/2 mice are documented as unsupported rather than
  faked. `make qemu-uefi` provisions a `usb-tablet` so the cursor tracks.
- **Movable windows.** `uefi/wm.c` is a small compositor over the double buffer:
  draggable (titlebar), z-ordered, focusable windows with a close box. Menu tools
  (About, Shell, Recovery, the GUI **Tools** launcher) open as windows.
- **Scrolling menu + scrollbar.** The boot-menu panel is now a proper viewport:
  only the entries that fit inside the panel are drawn, a scrollbar track/thumb is
  painted on the panel's right edge, and the viewport follows the selection —
  moving the highlight off-screen scrolls the list (keyboard Up/Down, plus mouse
  wheel where a pointer device reports a Z axis). This fixes the 9+-entry
  overflow where *Recovery*/*Reboot* used to spill below the panel.
- **Sliding selection highlight.** Changing the selected row animates the green
  highlight bar: it slides from the old row to the new row over a few frames using
  the double buffer, instead of snapping.
- **GUI Tools launcher (`type=tools`).** A `Tools` menu entry opens a launcher
  window listing ~10 self-contained windowed tools — Disk Info, GPT Viewer,
  Partition Browser, File Browser (ESP), Hex Viewer, Memory Map, EFI Variables,
  Boot Manager, System/Firmware Info, Theme/Settings and Key Tester — each a
  `wm.c` window with its own draw + event callback (see
  [`GUI_TOOLS.md`](GUI_TOOLS.md) and `uefi/tools.h`). They stack on the desktop
  (up to `WM_MAX_WINDOWS`) and are driven by the same mouse + keyboard.
- **Firmware/UEFI setup (`type=setup`).** A `Firmware Setup (UEFI)` entry (and the
  shell `setup`/`firmware` command) reboots into the firmware setup screen: it
  checks the `OsIndicationsSupported` bit `EFI_OS_INDICATIONS_BOOT_TO_FW_UI`,
  read-modify-writes the `OsIndications` runtime variable, then
  `ResetSystem(EfiResetCold)` (`uefi/fwsetup.c`). If the firmware does not
  advertise support it says so and stays in the menu.
- **Linux boot (`type=linux`).** A `vmlinuz` is treated as an EFI-stub PE app:
  `LoadImage`/`StartImage` with the kernel cmdline as `LoadOptions` and the initrd
  exposed via the Linux initrd `LoadFile2` media protocol
  (`LINUX_EFI_INITRD_MEDIA_GUID`). Config: `type=linux`, `vmlinuz=`, `initrd=`,
  `cmdline=`.
- **USB boot / chainload (`type=chainload`).** Enumerate **all**
  SimpleFileSystem/BlockIo volumes (including USB), find another EFI bootloader
  (`\EFI\BOOT\BOOTX64.EFI` or `\EFI\*/grubx64.efi`) and `LoadImage`+`StartImage`
  it — i.e. switch to GRUB from a live USB. `chain=` names a specific target, or
  leave it empty to auto-scan.
- **Filesystems.** Read-only **ext2/3/4** (superblock + inode + extent tree →
  list/cat) for the shell and recovery, plus **btrfs** detection and
  subvolume/snapshot listing (read-only, best-effort). FAT is via firmware.
- **Recovery / disk-fix tools.** In the shell and a Recovery window: `gpt`
  (parse+print GPT), `parts` (partitions per BlockIo), `fsprobe` (identify fs by
  magic), `rescue` (copy sectors, skip bad), `fatfix` (FAT boot-sector/backup
  check+repair), and best-effort data-recovery (undelete scan, carve by magic).
  Destructive ops are gated behind an explicit `yes` confirmation.
- **Shell & Recovery as menu entries.** In addition to pressing `c`, the default
  config ships a built-in **ForeB Shell** and **Recovery / Disk Tools** entry
  that open those windows directly from the menu.
- **Reboot fix.** *Reboot* / `Esc` now calls `RuntimeServices->ResetSystem`
  (`EfiResetWarm`, escalating to `EfiResetCold`) so the machine actually
  restarts. The `-no-reboot` flag was removed from the **interactive** QEMU
  targets (it is kept only for headless verify runs), so QEMU no longer exits as
  if shut down.
- **Max customization.** `forebo.cfg` exposes theme colours, cursor, animation
  on/off, window skin, default entry and more (see `include/forebo_cfg.h`).

## Ultimate customization (config, modules, images, animation, shell)

This upgrade adds a rich, config-driven UEFI experience while keeping the BIOS
path within its 8 KiB-per-stage budget. Feature coverage per firmware:

| Feature                         | UEFI | BIOS | Notes |
|---------------------------------|:----:|:----:|-------|
| Crisp **8x16 font** (+2x scale) | yes  | no   | UEFI renderer only; BIOS keeps its 8x8 ROM font (size budget). |
| **`forebo.cfg`** menu/config    | yes  | no   | Parsed from the ESP by `uefi/config.c`. |
| **Module / initrd** loading     | yes (many) | yes (one) | Multiboot1 `mods` array + `MB_FLAG_MODS`. |
| **Background image** (BMP/TGA)  | yes  | no   | `uefi/image.c`; BIOS keeps its gradient + tree logo. |
| Per-entry **icons** (TGA alpha) | yes  | no   | Drawn in each menu row's gutter. |
| **Animation** (fade/parallax/spinner) | yes | no | Off the 10 ms menu tick; MMIO spinner post-ExitBootServices. |
| Interactive **shell** (`c`)     | yes  | no   | GOP text shell; see [`SHELL.md`](SHELL.md). |

### Config file — `forebo.cfg`

A grub.cfg-like file staged at `\forebo\forebo.cfg` and parsed before the menu
is drawn (`uefi/config.c` -> `struct forebo_config`, see
[`include/forebo_cfg.h`](include/forebo_cfg.h)). Globals: `timeout=`,
`default=`, `remember_last=`, `background=`. Each `menuentry "Title" { ... }`
block takes
`kernel=`, `module=` / `module2=` (repeatable; `module2` is an accepted alias),
`cmdline="..."`, `background=`, `icon=`. Paths are ESP-absolute (`/` or `\`).
Malformed lines are skipped, so a partial file still boots. Worked example:
[`forebo.cfg`](forebo.cfg). Capacity: up to **64** entry/submenu rows,
**256**-byte paths and **256**-byte command lines (titles stay 64 bytes).

**Submenus (Limine-style).** `submenu "Title" { ... }` groups entries under a
collapsible level; blocks nest up to 8 deep and `icon=` inside a submenu block
sets the submenu row's own icon. Rows are stored flat with a `parent` link;
the menu renders one level at a time (Enter/Right descends, Esc/Left goes
back) with a `ForeB > CachyOS > Snapshot 906` breadcrumb in the panel title.
Configs without submenus render exactly as before.

**`default=`** accepts two forms: an integer counts only top-level rows
(submenu rows included in that count), and a title path like
`default=CachyOS/linux-cachyos` (quote it when it contains spaces) is matched
case-sensitively, one segment per submenu level. A resolved default that lands
on a submenu descends to its first child; any resolution failure falls back to
the first top-level non-submenu row. The countdown always boots the resolved
default, whichever level you are browsing.

**`remember_last=1`** persists the last booted forest/linux/chainload entry's
flat index in the UEFI variable `ForeBLastEntry` (vendor GUID
`{46524542-4F4F-5442-8001-466F72654231}`, attributes NV|BS) on every boot and
uses it — with the same descend/fallback rules — as the default on the next
boot. Any variable error keeps the config `default=`.

This upgrade adds a per-entry **`type=`** discriminator —
`forest` (default, x86 Multiboot), `linux` (EFI-stub `vmlinuz=` + `initrd=` +
`cmdline=`), `chainload` (`chain=` path, or empty to auto-scan USB), `shell`,
`recovery`, `tools` (the GUI tools launcher), `setup`/`firmware` (reboot into
firmware setup), `reboot` — plus global customization keys (theme colours,
cursor, `animations=on|off`, window skin, `default=`). The parser tolerates
unknown keys, so old configs keep working. Full schema:
[`include/forebo_cfg.h`](include/forebo_cfg.h).

**`icon=` short names.** `icon=` now accepts a bare **name** in addition to a
full ESP path: `icon=arch` resolves to `/forebo/icons/arch.tga` (a value already
containing a `/`, `\`, `.tga` or `.bmp` is used verbatim). Resolution lives in
`uefi/config.c` (`icon_resolve()`), mirrored by `tools_icon_path()` in
`uefi/tools.h` so menu entries and the GUI tool registry share one naming
convention. Names shipped by `tools/gen_assets.py`: `os`, `text`, `safe`, `gear`,
`settings`, `shield`, `reboot`, and the distro/hardware set below.

### Modules / initrd (both firmwares)

- **UEFI:** `uefi/modules.c` loads each `module`/`module2` file into pages that
  survive `ExitBootServices`, builds an `mb_module[]` array, and sets
  `mods_count` / `mods_addr` with `MB_FLAG_MODS`.
- **BIOS:** `stage2.asm` registers **one** module (an initrd) read from a
  configured disk sector into `INITRD_LOAD_PHYS`, then publishes it via the same
  multiboot `mods` array **and** `foreboots_boot_info.initrd_addr/size`
  (`FOREB_BIF_INITRD`). Enable it by assembling stage2 with
  `-DFOREB_INITRD_START_SECTOR=<n>` (the `Makefile` computes a safe default just
  past the kernel and dd's the sample there — see below). Adds only ~a dozen
  bytes; stage2 stays under 8 KiB.

### Backgrounds & icons

`uefi/image.c` decodes uncompressed 24/32-bit **BMP** and type-2 **TGA** (with
alpha) into a linear BGRA buffer, scales the background to fill the GOP
framebuffer, and alpha-blends icons into menu rows. Formats and recommended
sizes: [`ASSETS.md`](ASSETS.md). Default assets are generated (no binaries
checked in) by `tools/gen_assets.py` from `include/forebo_theme.h`.

**Icon set (all 32x32 TGA with 8-bit alpha).** Beyond the original
`os`/`text`/`safe`/`gear`/`shield`/`reboot` marks, `tools/gen_assets.py` now
paints a **distro + hardware** set referenceable by short name from `icon=`:
`ubuntu`, `debian`, `arch`, `fedora`, `mint`, `tux` (generic Linux penguin),
`windows`, `grub`, `usb`, `disk`, `settings` and `terminal`. The default
`forebo.cfg` uses them — Linux→`tux`, Chainload GRUB→`grub`, Boot removable→`usb`,
Shell→`terminal`, Recovery/Tools→`gear`, Firmware Setup→`settings`. Add your own
by writing a painter function and registering it in the `ICONS` dict; `make esp`
regenerates and stages every `assets/icons/*.tga` onto `::/forebo/icons/`.

### Interactive shell

Press `c` at the menu to drop into a GOP-rendered text shell (`uefi/shell.c`)
with `ls`/`cat`/`hexdump`, block-device `lsblk`/`read`/`write` (the destructive
`write` is gated behind a literal `yes`), `drives`, `modules`, UEFI-variable
`efivars`/`bootvars`/`getvar`/`setvar`, `background`, `boot`, `reboot`, `exit`.
Full command reference and safety contract: [`SHELL.md`](SHELL.md).

### Assets & Makefile wiring

`make esp` (and `make image`/`iso`) run `tools/gen_assets.py` to produce
`assets/bg.bmp` + `assets/icons/*.tga`, build a sample `assets/initrd.tar` if
absent, copy `forebo.cfg` + assets onto the ESP, and dd the sample module onto
the BIOS image at `BIOS_INITRD_SECTOR`. Override with, e.g.,
`make image BIOS_INITRD_SECTOR=0` (disable the BIOS module) or
`INITRD=/path/to/real-initrd.tar`.

## BIOS vs UEFI

The two front-ends differ only in how they gather platform data; the kernel
entry contract is identical.

| Aspect              | BIOS/CSM path                          | UEFI (native) path                       |
|---------------------|----------------------------------------|------------------------------------------|
| Loader              | `stage1`/`stage2`/`stage3` (NASM)      | `uefi/bootx64.c` (freestanding EFI app)  |
| Firmware entry mode | 16-bit real mode                       | 64-bit long mode                         |
| Framebuffer source  | VBE (INT 10h, `PhysBasePtr`)           | GOP (`FrameBufferBase`)                  |
| Memory map source   | E820 (INT 15h `AX=E820h`)              | `GetMemoryMap` (EFI descriptors)         |
| Kernel source       | raw disk sectors (48+)                 | ESP FAT file `\forebo\kernel.elf`        |
| Memory teardown     | none (real mode owns RAM)              | `ExitBootServices` before low-RAM writes |
| Pre-handoff CPU     | already 32-bit PM                      | drop long mode -> 32-bit PM              |
| **Kernel entry**    | 32-bit PM, `EAX=0x2BADB002`, `EBX=0x1800`, PICs masked | **identical** |
| `foreboots_boot_info`| `0x1000` (v2.0)                       | `0x1000` (v2.0)                          |
| `multiboot_info_t`  | `0x1800` (EBX)                         | `0x1800` (EBX)                           |

## Disk I/O

Stage 1 and stage 2 read via INT 13h **LBA extensions** (AH=42h) with an
automatic **CHS fallback** (AH=02h) if the BIOS does not support LBA. Reads
are chunked to 63 sectors per INT 13h call to respect BIOS limits. The boot
drive number is read from `foreboots_boot_info.boot_disk` (stored from BIOS DL
by stage 1).

## A20

Stage 2 enables A20 via three methods, in order, with a verification after
each: fast A20 (port `0x92`), keyboard controller (port `0x64`/`0x60`), and
BIOS INT 15h `AX=0x2401`. A wrap test (`0xFFFF:0x0510` vs `0x0000:0x0500`)
confirms the line is on.

## Building

```
make all                 # stage1.bin, stage2.bin, stage3.bin
make check               # verify sizes + MBR/stage2 signatures
make image KERNEL=../build/32bit-bios-debug/boot/kernel.bin
make iso                 # El Torito no-emulation ISO (xorriso)
make qemu                # test the disk image in QEMU
make qemu-iso            # test the ISO in QEMU
```

### Overriding config from the build command line

Every tunable in `config.h` is wrapped in `%ifndef`, so the main build can
override it with NASM `-D`:

```
make NASMFLAGS='-DFOREB_DEFAULT_WIDTH=1280 -DFOREB_DEFAULT_HEIGHT=720 -DFOREB_DEFAULT_TIMEOUT=3'
make NASMFLAGS='-DFOREB_FORCE_LONG_MODE=1'
```

## Main build integration

The top-level Makefile drives ForeB via these targets (no changes to the root
Makefile / `build-config.mk` / `conf.sh` are made by ForeB itself):

- `make forebo`        - build stage1/2/3
- `make forebo-image`  - build ForeB + raw disk image with the current kernel
- `make forebo-qemu`   - test in QEMU
- `make forebo-check`  - verify sizes + signatures
- `make forebo-clean`  - remove ForeB outputs

The integration step (separate from this directory) wires
`ENABLE_FOREB_BOOTLOADER` so that `make iso` / `make img` produces ForeB-based
media instead of GRUB-based media, and copies `Grub/forebo.cfg` into the image.

## Limitations / notes

- ForeB now boots on **both BIOS/CSM and native UEFI** firmware; GRUB is no
  longer required on either. The two front-ends share one kernel handoff
  contract (see [BIOS vs UEFI](#bios-vs-uefi)).
- The UEFI loader keeps the GOP RGB framebuffer (`framebuffer_type=1`); there is
  no VGA-text guarantee under UEFI, so the BIOS `nofb`/EGA-text (type 2) path
  has no direct UEFI equivalent — a UEFI `nofb` boot still hands over the GOP
  framebuffer.
- The BIOS GUI menu renders in 8bpp; the kernel framebuffer is 32bpp (re-selected
  after the menu). This keeps the 8 KiB stage 2 within budget.
- The kernel ELF is loaded to a 256 KiB buffer at `0x10000`; larger kernels
  require raising `KERNEL_MAX_SECTORS` / `KERNEL_LOAD_SEG`.
- BIOS initrd/module: `config.h` still defaults `FOREB_INITRD_START_SECTOR=0`
  (a bare `nasm` build registers no module, preserving the proven handoff). The
  `Makefile` turns it on for `image`/`iso` builds by computing a sector just past
  the kernel and dd'ing the sample `initrd.tar` there; `stage2.asm` reads it and
  publishes it as multiboot module 0. Set `BIOS_INITRD_SECTOR=0` to opt out.
  The BIOS path supports exactly **one** module; use the UEFI path (`forebo.cfg`
  `module=`/`module2=`) for multiple modules.
- The BIOS menu font stays 8x8: the 8x16 font + background/icon/animation/shell
  features are UEFI-only because the 8 KiB stage2/stage3 budget has no room for
  them (`make check` enforces the cap).

# ForeB UEFI loader (`uefi/`)

The native-UEFI front-end of ForeB. A single freestanding EFI application,
`bootx64.c`, that loads the Forest OS kernel on 64-bit UEFI firmware and hands
it off with the **exact same** Multiboot1 contract as the BIOS `stage3.asm`
path. See the top-level [`../README.md`](../README.md#uefi-native-boot) for the
end-to-end boot flow and the BIOS-vs-UEFI comparison.

## Self-contained EFI (no gnu-efi)

gnu-efi is intentionally **not** a dependency. `bootx64.c` defines every EFI
type, GUID, and protocol it uses inline (`EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`,
`EFI_GRAPHICS_OUTPUT_PROTOCOL`, `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`,
`EFI_MEMORY_DESCRIPTOR`, ...). This keeps the loader buildable with only clang +
lld and byte-compatible with the shared boot protocol.

- **No libc / no CRT.** Freestanding only; no runtime startup, no `main`.
- **Entry point:** `EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)`.
- **Shared layout:** the loader `#include`s [`../include/boot_protocol.h`](../include/boot_protocol.h)
  so `foreboots_boot_info`, `multiboot_info_t`, the `FOREB_*` / `MB_FLAG_*`
  constants, and the fixed physical addresses (`0x1000` / `0x1400` / `0x1800`,
  etc.) match the NASM side (`../include/boot_protocol.inc`) exactly. **Do not
  redefine those symbols here.**
- **`-fshort-wchar`:** EFI strings are UTF-16; the ABI is Microsoft x64, so the
  app is compiled as a PE32+ Windows target.
- **No red zone / no MMX / no SSE:** firmware calls and the low-level handoff run
  with a strict register/stack discipline.

## What the loader does

1. Query GOP for the framebuffer (base/width/height/pitch, 32 bpp, RGB).
2. **Graphical menu** — draw the forest-themed boot menu directly on the GOP
   framebuffer and poll `ConIn->ReadKeyStroke` (+ `gBS->Stall`) for
   Up/Down/Enter to pick the entry. This runs **before** `GetMemoryMap` so no
   allocation invalidates the map key. Menu draw code is in `ui.c`.
3. Read `\forebo\kernel.elf` from the ESP via the Simple File System protocol,
   in chunks, advancing an **in-place progress bar** on the framebuffer per
   chunk (direct writes only — no `ConOut`, so no per-newline console scroll).
4. `AllocatePages` for the fixed ForeB low-RAM region and the kernel `PT_LOAD`
   destination so they survive `ExitBootServices`.
5. `GetMemoryMap`, convert EFI descriptors to E820-style entries, derive
   `mem_lower`/`mem_upper`.
6. `ExitBootServices` with the current map key.
7. Copy `PT_LOAD` segments to their `p_paddr` (filesz, then zero the BSS tail);
   populate `multiboot_info_t` (`0x1800`), the mmap arrays, and
   `foreboots_boot_info` (`0x1000`).
8. Tear down long mode -> 32-bit protected mode, load a flat GDT, mask both PICs.
9. Jump to the ELF `e_entry` with `EAX=0x2BADB002`, `EBX=0x1800`, paging off,
   interrupts off. The 64-bit kernel re-enters long mode on its own.

## Graphical UI (`ui.c`)

The user-visible UI is a full **graphical** menu and a load progress bar drawn
straight to the GOP framebuffer — never through the firmware text console
(`ConOut`), whose per-newline scroll on a hi-res GOP console memmoves the whole
screen and was the source of the slow load display. All status is drawn in
place; verbose logs go to **serial (COM1) only**.

`ui.c` shares the BIOS renderer's theme so both firmwares look consistent, but
the UEFI path now renders text with a **crisp 8x16 font** (10x more legible than
the old 8x8 cell) plus optional integer **2x scaling** for hi-res GOP modes:

- [`../include/font8x16.h`](../include/font8x16.h) — the 8x16 bitmap glyph set
  used by the UEFI renderer (`draw_char` iterates 16 rows; each glyph pixel is a
  `scale`x`scale` block, so `scale=2` yields a 16x32 cell on large screens). The
  BIOS path keeps its own 8x8 ROM font (see the size note below).
- [`../include/forebo_theme.h`](../include/forebo_theme.h) — the forest colors as
  RGB888 (written directly to the 32bpp framebuffer; no palette under UEFI) plus
  the shared menu geometry. `FOREB_GLYPH_H` is now `16`.
- [`../UI_SPEC.md`](../UI_SPEC.md) — the authoritative UI spec both paths follow.

Keyboard input uses `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` (`gST->ConIn`), whose
`EFI_INPUT_KEY`, `Reset`, and `ReadKeyStroke` definitions are provided inline in
[`efi.h`](efi.h). Arrow scan codes: Up `0x01`, Down `0x02`; Enter is
`UnicodeChar == 0x0D`. Polling loops on `EFI_NOT_READY` with `gBS->Stall`.

## Build (clang + lld)

Every `uefi/*.c` compiles with the **same** freestanding recipe. Besides the
loader (`bootx64.c`) and the renderer (`ui.c`), the feature modules linked into
`BOOTX64.EFI` are:

| Module        | Role |
|---------------|------|
| `image.c`     | BMP (24/32-bit) + TGA (type 2) decoders and the background / alpha icon blitters. |
| `config.c`    | `forebo.cfg` parser -> `struct forebo_config` (see `../include/forebo_cfg.h`). |
| `modules.c`   | Loads each `module`/`module2` file and builds the multiboot1 `mods` array. |
| `shell.c`     | The interactive GOP shell (press `c`, or the *ForeB Shell* menu entry). See [`../SHELL.md`](../SHELL.md). |
| `anim.c`      | Menu fade-in / parallax and the animated spinner + smooth load bar. |
| `input.c`     | Mouse: enumerates **all** `EFI_SIMPLE_POINTER` + `EFI_ABSOLUTE_POINTER` handles via `LocateHandleBuffer`, polls/merges them each frame; cursor sprite. USB only (see note). |
| `wm.c`        | Window manager/compositor over the double buffer (drag, z-order, close box). |
| `tools.c`     | Windowed GUI **tools** registry + launcher (`type=tools`): Disk Info, GPT Viewer, Partition/File Browser, Hex Viewer, Memory Map, EFI Variables, Boot Manager, System Info, Settings, Key Tester. Also `tools_icon_path()`. See [`../GUI_TOOLS.md`](../GUI_TOOLS.md) + `tools.h`. |
| `fwsetup.c`   | Firmware/UEFI **setup** entry (`type=setup`): sets `OsIndications` (`BOOT_TO_FW_UI`) after checking `OsIndicationsSupported`, then `ResetSystem(EfiResetCold)`. |
| `linux.c` / `boot_linux.c` | EFI-stub `vmlinuz` boot: LoadImage/StartImage + `LoadFile2` initrd (`LINUX_EFI_INITRD_MEDIA`). |
| `chain.c` / `chainload.c`  | Enumerate volumes (incl. USB) + chainload another EFI loader (GRUB). |
| `fs_ext.c`    | Read-only ext2/3/4 (superblock + inode + extent tree): list / cat. |
| `fs_btrfs.c`  | btrfs detect + subvolume/snapshot listing (read-only, best-effort). |

> **Every `uefi/*.c` is auto-discovered by the Makefile** (`wildcard uefi/*.c`
> minus `bootx64.c`/`ui.c`) and compiled + linked into `BOOTX64.EFI` with the
> shared clang recipe — `tools.c` and `fwsetup.c` needed no new build rules, just
> their headers added to the module dependency set.
>
> **Pointer reality (honest).** OVMF exposes a UEFI pointer only for **USB**
> devices — `usb-tablet` → `EFI_ABSOLUTE_POINTER` (absolute, mapped to the
> screen) and `usb-mouse` → `EFI_SIMPLE_POINTER` (relative deltas). It does
> **not** surface a PS/2 mouse to UEFI apps (no CSM), so ForeB supports USB
> pointers only and does not pretend PS/2 works. `make qemu-uefi` adds a
> `usb-tablet`; add `-device usb-mouse` to exercise the relative path.

`ui.c` is now **double-buffered**: all drawing lands in an off-screen RAM back
buffer and `ui_present()` blits it to the GOP front buffer once per frame, so the
cursor, windows and animations are tear-free. The public draw API is unchanged.

```
# Each C module (same flags); then the NASM handoff trampoline; then link all.
# -mno-stack-arg-probe suppresses MS-ABI __chkstk (no CRT in this freestanding
# link); -Iuefi resolves arch.h / efi_ext.h.
CFLAGS="-target x86_64-unknown-windows -ffreestanding -fshort-wchar \
        -mno-red-zone -mno-mmx -mno-sse -mno-stack-arg-probe \
        -Wall -Wextra -std=c11 -Iinclude -Iuefi"
for f in uefi/*.c; do clang $CFLAGS -c $f -o ${f%.c}.o; done
nasm -f win64 -I. -Iinclude uefi/handoff64to32.asm -o uefi/handoff64to32.o

ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
       -out:BOOTX64.EFI uefi/*.o
```

`ld.lld -flavor link` selects the MS-link driver so it emits a PE32+ EFI
application. The result, `BOOTX64.EFI`, is placed on the ESP at
`\EFI\BOOT\BOOTX64.EFI` (removable-media default). The `Makefile` **auto-discovers**
every `uefi/*.c` via a wildcard (a static-pattern `uefi/%.o: uefi/%.c` rule), so
new modules compile and link with no Makefile edits.

### Cross-arch build (aarch64 + riscv64)

`uefi/arch.h` gates the x86-only Multiboot handoff on `FOREB_MULTIBOOT_SUPPORTED`,
so the same C builds for all three UEFI arches (UI + shell + Linux boot +
chainload + filesystems + recovery everywhere; Forest Multiboot on x86 only).

```
make uefi-aa64   # aarch64-unknown-windows -> BOOTAA64.EFI (real, bootable PE)
make qemu-aa64   # boot it under /usr/share/edk2/aa64 OVMF (reaches the menu)
make uefi-riscv  # compile all uefi/*.c to RISC-V objects + PIE ELF (see boundary)
make uefi-all    # x64 + aa64 PE, then riscv objects
```

For aarch64, drop the x86-only `-mno-mmx -mno-sse` and add `-mgeneral-regs-only`.
riscv64 compiles to ELF only — clang/lld have no RISC-V PE backend, so a bootable
`BOOTRISCV64.EFI` needs edk2 `GenFw` to convert ELF→PE (documented by the target).

## Build / run from the Makefile

```
make uefi           # produce BOOTX64.EFI (the two commands above)
make esp            # FAT ESP image: BOOTX64.EFI + \forebo\kernel.elf
make qemu-uefi      # boot the ESP under OVMF (edk2) in QEMU
make iso-hybrid     # hybrid BIOS + UEFI ISO
make qemu-uefi-iso  # boot the hybrid ISO under OVMF
make screenshots    # headless-boot both paths + grab menu PPMs (dev helper)
```

OVMF firmware (Arch `edk2` package):
`/usr/share/edk2/x64/OVMF_CODE.4m.fd` + `OVMF_VARS.4m.fd`. The `qemu-uefi*`
targets copy `OVMF_VARS` to a writable temp so the shipped pflash is untouched.

## Ultimate customization (this upgrade)

Everything below runs in the **pre-`ExitBootServices`** window, where Boot
Services, Simple File System, Block I/O, and Runtime variable services are all
still live. The ESP layout the loader expects (all staged by `make esp`):

```
\EFI\BOOT\BOOTX64.EFI        the loader
\forebo\kernel.elf           the multiboot1 kernel
\forebo\forebo.cfg           the config file (below)
\forebo\bg.bmp               default menu background (BMP/TGA)
\forebo\icons\*.tga          per-entry icons (32-bit TGA w/ alpha)
\forebo\initrd.tar           sample multiboot module
```

### Configuration file — `\forebo\forebo.cfg`

A grub.cfg-like file parsed by `config.c` into `struct forebo_config`
([`../include/forebo_cfg.h`](../include/forebo_cfg.h)) **before** the menu is
drawn. Global keys `timeout=`, `default=`, `background=`; per-block
`menuentry "Title" { ... }` with `kernel=`, `module=` / `module2=` (repeatable,
aliases), `cmdline="..."`, `background=`, `icon=`. Paths are ESP-absolute and
accept `/` or `\`. A missing or malformed config falls back to sane defaults
(the parser skips bad lines and never faults). See the worked example in
[`../forebo.cfg`](../forebo.cfg).

### Boot menu — scrolling viewport + sliding highlight

The menu panel is a **viewport**: only the entries that fit are drawn, a
scrollbar track/thumb is painted on the panel's right edge, and the viewport
**follows the selection** — moving the highlight past the visible window scrolls
the list (keyboard Up/Down, mouse wheel where the pointer reports a Z axis). This
fixes the 9+-entry overflow that used to spill *Recovery*/*Reboot* below the box.
Changing the selection **slides** the green highlight bar from the old row to the
new row over a few frames using the double buffer. The icon and hit-test geometry
mirror the same viewport offset so clicks and icons stay aligned while scrolled.

### Boot methods — `type=` (forest / linux / chainload / shell / recovery / tools / setup / reboot)

Each `menuentry` carries a `type=` (default `forest`). Beyond the x86 Forest
Multiboot path, the pure-UEFI methods work on x86 **and** aarch64:

- **`type=linux`** (`linux.c` / `boot_linux.c`) — `vmlinuz=` is an EFI-stub PE
  loaded with `LoadImage`/`StartImage`; `cmdline=` becomes the image
  `LoadOptions`; `initrd=` is handed to the stub through a `LoadFile2` handle
  published under `LINUX_EFI_INITRD_MEDIA_GUID` (the exact vendor device path the
  Linux stub searches for).
- **`type=chainload`** (`chain.c` / `chainload.c`) — scan every
  SimpleFileSystem/BlockIo volume (including USB), locate another EFI loader
  (`\EFI\BOOT\BOOTX64.EFI`, `\EFI\*/grubx64.efi`) named by `chain=` or auto-found,
  build its device path and `LoadImage`+`StartImage` it — e.g. hand off to GRUB
  on a live USB.
- **`type=shell` / `type=recovery`** — open the interactive shell or the Recovery
  window directly from the menu (also reachable with `c`). Recovery exposes
  `gpt`, `parts`, `fsprobe`, `rescue`, `fatfix` and best-effort data recovery;
  destructive ops require an explicit `yes`.
- **`type=tools`** (`tools.c`) — open the windowed **GUI Tools launcher**: a
  `wm.c` window listing ~10 self-contained inspector tools (Disk Info, GPT Viewer,
  Partition Browser, File Browser, Hex Viewer, Memory Map, EFI Variables, Boot
  Manager, System/Firmware Info, Theme/Settings, Key Tester). Each opens as its
  own window with a draw + event callback; they stack up to `WM_MAX_WINDOWS`. Full
  per-tool spec: [`../GUI_TOOLS.md`](../GUI_TOOLS.md).
- **`type=setup`** / `firmware` (`fwsetup.c`) — reboot into the firmware/UEFI
  **setup screen**: check the `OsIndicationsSupported` `BOOT_TO_FW_UI` bit,
  read-modify-write the `OsIndications` runtime variable, then
  `ResetSystem(EfiResetCold)`. Also a shell `setup`/`firmware` command. If the
  firmware does not advertise support it reports that and stays in the menu.
- **`type=reboot`** — `RuntimeServices->ResetSystem(EfiResetWarm, ...)`, escalating
  to `EfiResetCold`; the machine restarts (the old *reboot exits QEMU* bug was the
  `-no-reboot` flag on the interactive targets, now removed).

### Filesystems — read-only ext2/3/4 + btrfs (`fs_ext.c`, `fs_btrfs.c`)

`fs_ext.c` walks an ext2/3/4 superblock + inode table + extent tree over
`EFI_BLOCK_IO` to list directories and cat files; `fs_btrfs.c` detects a btrfs
superblock and lists subvolumes/snapshots (read-only, best-effort). Both are used
by the shell and Recovery window. FAT is handled by firmware Simple File System.

### Modules (initrd) — multiboot1 `mods`

`modules.c` loads every `module`/`module2` path of the chosen entry into pages
that survive `ExitBootServices`, fills an `mb_module[]` array, and sets
`multiboot_info_t.mods_count` / `mods_addr` with `MB_FLAG_MODS`. Up to
`FOREB_CFG_MAX_MODULES` per entry. The BIOS path registers **one** module from a
configured disk sector (see [`../README.md`](../README.md) and
`FOREB_INITRD_START_SECTOR`); UEFI supports several from ESP files.

### Background image + icons

`image.c` decodes uncompressed **BMP** (24/32-bit, bottom-up or top-down,
4-byte-padded rows) and **TGA** (type 2 true-color, 24/32-bit, origin-bit
honored) into a linear `BGRA` buffer. `ui_set_background()` scales-blits it to
fill the framebuffer in place of the procedural gradient; per-entry `icon=`
images are alpha-blended into each menu row's left gutter. Formats + sizes are
specified in [`../ASSETS.md`](../ASSETS.md); defaults are generated by
[`../tools/gen_assets.py`](../tools/gen_assets.py) straight from the theme.

`icon=` accepts a **short name** (`icon=arch`) that `config.c` (`icon_resolve()`,
mirrored by `tools_icon_path()`) rewrites to `/forebo/icons/<name>.tga`; a value
with a separator or image extension is used verbatim. `gen_assets.py` ships an
extended 32x32-alpha set — `os`, `text`, `safe`, `gear`, `settings`, `shield`,
`reboot` plus **distro/hardware** glyphs `ubuntu`, `debian`, `arch`, `fedora`,
`mint`, `tux`, `windows`, `grub`, `usb`, `disk`, `terminal`. The default menu
uses them (Linux→`tux`, Chainload→`grub`, Boot removable→`usb`, Shell→`terminal`,
Recovery/Tools→`gear`, Firmware Setup→`settings`).

### Animation

`anim.c` drives, off the existing 10 ms `gBS->Stall` menu cadence: a menu
fade-in, a subtle parallax/particle drift on the background, an animated spinner,
and a smoothly interpolated load bar. The staging spinner used **after**
`ExitBootServices` is pure-MMIO (no Boot Services), matching `ui_progress`.

### Interactive shell — press `c`

Pressing `c` at the menu opens a text shell rendered on the GOP framebuffer
(`shell.c`). Commands: `help`, `ls`, `cat`, `hexdump`, `lsblk`, `read`,
`write` (destructive, gated behind a literal `yes` confirmation), `drives`,
`modules`, `efivars`/`bootvars`, `getvar`/`setvar`, `background`, `boot`,
`reboot`, `exit`. Full reference and the `write` safety contract are in
[`../SHELL.md`](../SHELL.md). This required extending [`efi.h`](efi.h) with
`EFI_BLOCK_IO_PROTOCOL`, real `GetVariable`/`GetNextVariableName` typedefs, the
`EFI_GLOBAL_VARIABLE` GUID, and `LocateHandleBuffer`.

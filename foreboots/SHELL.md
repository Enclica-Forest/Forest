# ForeB Interactive Shell

ForeB's UEFI build includes a small text shell rendered directly on the GOP
framebuffer (8x16 font). It runs in the **pre-ExitBootServices** window, so
Boot Services, the Simple File System, Block I/O, and Runtime variable
services are all live.

## Entering / leaving

- At the boot menu, press **`c`** to open the shell.
- Type **`exit`** (or press **Esc**) to return to the boot menu.
- The countdown is paused while the shell is open.

Input: line-based. Backspace edits the current line; **Enter** runs it.
Output scrolls in the framebuffer console. Commands and paths are
case-insensitive for command names; ESP paths accept `/` or `\`.

---

## Commands

Notation: `<required>`  `[optional]`.

| Command | Syntax | Description |
|---|---|---|
| `help` | `help [cmd]` | List commands, or detail one. |
| `ls` | `ls [path]` | List a directory on the ESP (default `\`). Shows name, size, dir flag. |
| `cat` | `cat <file>` | Print a text file's contents from the ESP. |
| `hexdump` | `hexdump <file> [len]` | Hex+ASCII dump of a file (default first 256 bytes). |
| `lsblk` | `lsblk` | Enumerate `EFI_BLOCK_IO_PROTOCOL` devices: index, block size, last LBA, removable/present flags. |
| `read` | `read <dev> <lba> [count]` | Dump `count` (default 1) raw sectors from block device `<dev>` (index from `lsblk`) starting at `<lba>`. Read-only. |
| `write` | `write <dev> <lba> <file>` | **DESTRUCTIVE.** Write a file's bytes onto device `<dev>` at `<lba>`. Gated - see below. |
| `drives` | `drives` | List `SIMPLE_FILE_SYSTEM` volumes (mountable filesystems), separate from raw block devices. |
| `devices` | `devices` | Hardware inventory: every input device (keyboards, mice/trackpads, touch) and storage device with its type (NVMe SSD, SATA disk, USB storage, SD/eMMC, optical, partition). Aliases: `lsdev`, `hw`. Read-only. |
| `inputtest` | `inputtest` | Live input tester: echoes each keystroke (char + scancode) and pointer motion/buttons as they happen. **Press `c` to cancel** (Esc also aborts via the key echo). Alias: `testinput`. |
| `modules` | `modules [add <path>]` | List the modules staged for the next boot; `add <path>` appends one (ESP file) to the current entry's module list. |
| `efivars` / `bootvars` | `efivars` | Enumerate UEFI variables via `GetNextVariableName`/`GetVariable`. `bootvars` filters to `Boot####` / `BootOrder` under the global GUID. |
| `getvar` | `getvar <name> [guid]` | Print one UEFI variable's attributes + hex value. |
| `setvar` | `setvar <name> <guid> <hex>` | Set a UEFI variable (advanced; validates hex). |
| `background` | `background <file>` | Load a BMP/TGA from the ESP and set it as the live menu background. |
| `boot` | `boot [entry]` | Boot a menu entry now (index or title match); default = current selection. |
| `reboot` | `reboot` | Warm-reset the machine via `RuntimeServices->ResetSystem`. |
| `exit` | `exit` | Leave the shell, return to the boot menu. |

---

## Safety: the `write` command

`write` performs a **raw, unrecoverable** sector overwrite of a block device.
It can corrupt partition tables, filesystems, or the OS. It is therefore
gated behind an explicit interactive confirmation:

1. `write <dev> <lba> <file>` first prints a summary:
   - target device index + its block size and `LastBlock`,
   - the destination LBA range that will be overwritten,
   - the source file and byte/sector count.
2. It then prompts:

       This will OVERWRITE <N> sector(s) on dev <D> at LBA <L>.
       This CANNOT be undone. Type 'yes' to proceed:

3. The write proceeds **only** if the user types the literal word `yes`
   (exact, lowercase) and presses Enter. **Any** other input - `y`, `YES`,
   Enter, Esc, anything - **aborts** with no write performed.
4. Additional guards:
   - Refuses if the device reports `ReadOnly` in its `EFI_BLOCK_IO_MEDIA`.
   - Refuses if `<lba> + count` exceeds `LastBlock` (out-of-range).
   - Writes whole sectors only; a short final sector is zero-padded and the
     tail is reported.

No other command modifies media. `read`, `lsblk`, `drives`, `ls`, `cat`,
`hexdump`, `efivars`, and `getvar` are all read-only. `setvar` modifies
firmware **variables** (not media) and prints what it will set before doing
so.

---

## Notes for implementers

- The shell needs the following added to `uefi/efi.h` (see recon):
  `EFI_BLOCK_IO_PROTOCOL` + `EFI_BLOCK_IO_MEDIA` (+ its GUID) for
  `lsblk`/`read`/`write`; real function-pointer typedefs for
  `GetVariable`/`GetNextVariableName` (+ `EFI_GLOBAL_VARIABLE` GUID) for the
  var commands; `LocateHandleBuffer` for enumeration; and a few extra scan
  codes (`SCAN_LEFT`/`SCAN_RIGHT`) - backspace arrives as `UnicodeChar 0x08`.
- All shell I/O is pure GOP framebuffer drawing (`ui.c` primitives) plus
  `ConIn->ReadKeyStroke`; it must run **before** `ExitBootServices`.
- Config edits made in the shell (`modules add`, `background`) mutate the
  in-memory `struct forebo_config` (see `include/forebo_cfg.h`) and take
  effect on the next `boot`.

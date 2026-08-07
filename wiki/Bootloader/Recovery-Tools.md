# Recovery Tools

Forest Bootloader ships with a full suite of pre-boot recovery and diagnostic tools, accessible from the boot menu or the interactive shell. Everything runs **before** ExitBootServices -- no OS kernel required. All tools are freestanding (no libc), render directly to the GOP framebuffer, and work on any UEFI system.

There are two interfaces: the **Recovery Window** (GUI with buttons and a log panel) and the **Interactive Shell** (text-based command line). Destructive operations are only reachable through the shell, which gates every dangerous action behind a typed `yes` confirmation.

## Accessing Recovery Tools

### From the Boot Menu

The default `forebo.cfg` ships two built-in entries:

- **Recovery** (`type=recovery`) -- Opens the graphical Recovery Tools window with read-only diagnostic buttons and a launcher for the shell.
- **ForeB Shell** (`type=shell`) -- Drops you directly into the interactive text shell.

Add them to your config if missing:

```
menuentry "Recovery" { type = recovery }
menuentry "ForeB Shell" { type = shell }
```

### From the Recovery Window

The Recovery Window provides a mouse-driven GUI with buttons on the left (List Disks, Next Disk, GPT View, FS Probe, Chainload USB, Open Shell, Close) and an output log on the right. Click **Open Shell** to access the full command set.

### From the Shell

Type `help` to see all available commands. The shell prompt shows your current ESP working directory:

```
forb:\> help
```

## The Interactive Shell

The shell is a text console rendered on the GOP framebuffer with 256 lines of scrollback, a blinking caret, and line editing (insert, delete, Home, End, arrow keys). Press **Esc** to leave.

### Navigation Commands

| Command | Description |
|---------|-------------|
| `cd <dir>` | Change working directory on the ESP |
| `cd /` | Go to ESP root |
| `pwd` | Print current working directory |
| `clear` / `cls` | Clear the screen |

Both `/` and `\` separators are accepted. `..` goes up a directory.

## File Operations

### Listing Files

```
ls [path]
dir [path]
```

Lists files and directories. Directories are marked with `<DIR>` and files show their byte size.

### Viewing File Contents

```
cat <file>
type <file>
```

Prints a text file from the ESP (up to 128 KB).

### Hex Dump

```
hexdump <file> [length]
xxd <file> [length]
```

Hex + ASCII dump of an ESP file. Default 256 bytes, max 64 KB. Output shows offset, hex bytes (8+8 grouped), and printable ASCII:

```
00000000  23 20 66 6F 72 65 62 6F  2E 63 66 67 20 63 6F 6E  |# forebo.cfg con|
00000010  66 69 67 75 72 61 74 69  6F 6E 0A 0A 74 69 6D 65  |figuration..time|
```

## Block Device Operations

### List Block Devices

```
lsblk
```

Lists all EFI_BLOCK_IO devices with block size, last LBA, size in MiB, and flags (removable, no-media, read-only, partition):

```
dev  blocksz    lastLBA      size      flags
[0]  512        625142447    298GiB    part
[1]  2048       3639295      7109MiB   removable
```

### List Filesystem Volumes

```
drives
```

Enumerates SimpleFileSystem volumes (FAT32/ESP), showing label, total/free space, and read-only status.

### Read Raw Sectors

```
read <dev> <lba> [count]
```

Hex-dumps up to 64 sectors starting at the given LBA. Useful for inspecting boot sectors, partition headers, or superblocks.

### Write Raw Sectors

```
write <dev> <lba> <file>
```

**DESTRUCTIVE** -- writes an ESP file to raw sectors. Requires typed `yes`. Refuses writes past end-of-device or to read-only devices.

## UEFI Variable Operations

```
efivars              # enumerate all variables with GUIDs
bootvars             # BootCurrent, BootNext, BootOrder, Boot#### options
getvar <name> [guid] # read one variable (attributes + hex dump)
setvar <name> <guid> <hex>  # write a variable (NON_VOLATILE)
```

`bootvars` is especially useful for debugging boot order -- it shows every `Boot####` option with its human-readable description.

## GPT Partition Table Viewer

### View GPT on a Device

```
gpt <dev>
```

Parses the GPT header and partitions. Shows disk GUID, usable LBA range, and each partition with type, LBA range, size, and name:

```
GPT header valid
  disk GUID {12345678-ABCD-1234-ABCD-123456789ABC}
  usable LBA 34 .. 625142411   entries 128 x 128B
  [0] EFI System  LBA 40 .. 675839  (330 MiB)  "EFI"
  [1] Linux filesystem  LBA 675840 .. 625142411  (298000 MiB)  "root"
  2 partition(s)
```

Recognized types: EFI System, Linux filesystem, Linux swap, MS basic data, MS reserved, Linux LVM, BIOS boot.

### Partition Summary

```
parts
```

Lists all block devices (disks and partitions). For whole disks, attempts GPT first, then falls back to MBR detection.

## Partition Browser (GUI)

The **Partition Browser** is a windowed tool (open with `tools` in the shell). It provides a scrollable device list, automatic GPT parsing, filesystem probing, and Hex Viewer integration. Click a partition to view its raw sectors in the hex viewer, or browse FAT/ESP directories.

## Hex Viewer (GUI)

The **Hex Viewer** displays binary data in the classic `offset  hex...  |ascii|` layout. Supports three data sources:

- **Raw disk sector** -- any LBA on any block device
- **ESP file** -- browse and view files
- **Memory/variable data** -- UEFI variable contents

Navigation: arrows, Page Up/Down, Home/End, mouse wheel, and Prev/Next buttons. Handles up to 64 KB with 16 bytes per line.

## Sector Rescue (Raw Disk Copy)

```
rescue <srcdev> <dstfile|dstdev> [skip-bad]
```

**DESTRUCTIVE on destination** -- copies an entire device sector-by-sector. Requires typed `yes`.

```
rescue 0 1            # device 0 -> device 1
rescue 0 backup.img   # device 0 -> \backup.img on ESP
rescue 0 1 skip-bad   # zero-fill bad sectors, keep copying
```

Features: corruption-tolerant with `skip-bad`, live progress with bad sector count, Esc to abort, safety checks for size/read-only/block-size.

## Filesystem Probing

```
fsprobe <dev>
```

Reads the first ~64 KB and identifies the filesystem by on-disk magic:

| Filesystem | Detection |
|-----------|-----------|
| NTFS | MBR sig + "NTFS    " @ offset 3 |
| exFAT | MBR sig + "EXFAT   " @ offset 3 |
| FAT32 | "FAT32   " @ offset 82 |
| FAT12/16 | "FAT12/FAT16/FAT" @ offset 54 |
| ext2/3/4 | Magic 0xEF53 @ offset 1080 (with feature detection for ext3/ext4) |
| btrfs | "_BHRfS_M" @ offset 0x10040 |
| XFS | "XFSB" @ offset 0 |
| LUKS | "LUKS\xba\xbe" @ offset 0 |
| ISO9660 | "CD001" @ offset 32769 |
| Linux swap | "SWAPSPACE2" @ offset 4086 |
| MBR/bootable | MBR 0x55AA without specific FS magic |

## Data Carving Scanner

```
scan <dev>
carve <dev>
```

**Read-only** -- scans up to 128 MiB for file signatures: JPEG, PNG, GIF, PDF, ZIP, GZIP, ELF, BMP, RIFF. Reports count and first offset per type:

```
scanning 128 MiB (Esc aborts)...
  50%
  100%
carve summary:
  JPEG : 47 hit(s), first @ 0x00000000001A4200
  PNG : 3 hit(s), first @ 0x00000000003B1000
  Note: heuristic signature scan, not a full undelete.
```

## Undelete / File Recovery (GUI)

The **Undelete** tool (from the Tools launcher) has two modes toggled with **Tab**:

**Browse Mode** walks the real filesystem directory tree:
- ext2/3/4 -- full directory traversal via the built-in ext driver
- FAT/ESP -- firmware SimpleFileSystem protocol
- btrfs -- subvolume and snapshot listing

**Carve Mode** signature-scans the raw device for deleted files (JPEG/PNG/PDF/ZIP/GIF/BMP/TGA), using corruption-tolerant I/O. Bad sectors are zero-filled; scanning continues.

**Preview** decodes BMP/TGA images inline or shows a hex dump of other files. **Recover** (press `R`) saves the selected file to `\forebo\recovered\NNNN.<ext>` on the ESP -- always read-only on the source.

## Other Useful Commands

| Command | Description |
|---------|-------------|
| `memmap` | EFI memory map summary (total/usable RAM) |
| `devices` / `lsdev` | Hardware inventory with transport type (NVMe, SATA, USB, etc.) |
| `inputtest` | Live keyboard/mouse echo. Press `c` to cancel. |
| `modules [add <path>]` | List or add kernel modules for the current boot entry |
| `background <file>` | Set a background image for the boot menu |
| `config` / `reload` | Reload `forebo.cfg` from the ESP |
| `boot [idx\|title]` | Boot a specific entry by index or title |
| `setup` / `firmware` | Reboot into UEFI firmware setup |
| `tools` | Open the GUI Tools launcher |
| `basic` | Interactive BASIC interpreter |
| `reboot` | Reboot the machine |
| `exit` | Return to the boot menu |

## Filesystem Support (ext2/3/4, btrfs)

### ext2/3/4 Driver

The built-in ext driver (`fs_ext.c`) is a read-only recovery driver supporting:

- Superblock parsing -- dynamic block size (1 KiB--64 KiB), 64-bit features, dynamic inode size
- Block mapping -- indirect block maps (direct/single/double/triple) and ext4 extent trees
- Directory walk -- linear scan with full path resolution
- Volume label -- reads from superblock

Limitations: no journal replay, no htree acceleration, no inline_data, no symlink following, no bigalloc.

### btrfs Support

Lists subvolumes and snapshots. Shell commands:

```
ext-ls <dev> [path]     # list a directory on an ext2/3/4 volume
ext-cat <dev> <path>    # print a file from an ext2/3/4 volume (max 1 MiB)
btrfs-snaps <dev>       # list btrfs subvolumes/snapshots
```

## Drive Clone

The **Clone Drive** tool (from the GUI Tools launcher) copies an entire device to another device or to `\forebo\clone.img` on the ESP. Corruption-tolerant with live progress and Esc abort. Safety checks prevent cloning to a smaller device or the same device.

## Use Cases

### Boot Partition Won't Mount

```
lsblk              # find the ESP device
fsprobe 1          # confirm it's FAT32
cat \forebo.cfg    # read the config directly
```

### Investigating a Failed Install

```
lsblk              # identify disks
gpt 0              # check GPT layout
fsprobe 0          # identify filesystems
parts              # overview of everything
```

### Recovering Deleted Files

```
tools              # open GUI, click Undelete, select device, Tab to Carve
                   # select an item, click Recover
```

### Damaged Media

```
rescue 0 1 skip-bad    # clone with bad-sector tolerance
scan 0                  # find surviving file signatures
```

### FAT Boot Sector Corruption

```
fatfix 1                # check and repair boot signature + backup
```

### Reading ext4 Without an OS

```
ext-ls 2 /              # list root directory
ext-cat 2 /etc/fstab    # read a specific file
```

### Debugging Boot Order

```
bootvars                # see BootCurrent, BootNext, BootOrder
getvar Boot0001         # inspect a specific boot option
```

## Safety Notes

- **Destructive commands** (`write`, `rescue`, `fatfix`) always ask for typed confirmation.
- **Read-only operations** (`ls`, `cat`, `hexdump`, `read`, `gpt`, `fsprobe`, `scan`, `ext-ls`, `ext-cat`) never modify anything.
- The **Undelete** and **Clone** tools are read-only on the source device.
- All tools run before ExitBootServices -- changes affect the disk directly.
- Press **Esc** at any time to abort a running operation.
- The shell's `exit` returns to the boot menu without changes.

# Forest OS Kernel: Filesystems

Forest OS supports a wide range of filesystem types, from ubiquitous consumer formats to niche retro-computing and embedded systems. The filesystem layer is built on a VFS (Virtual File System) abstraction that decouples individual drivers from path resolution, mount management, and the syscall interface.

---

## Supported Filesystems

| Filesystem | Source File | Read | Write | Status |
|------------|-------------|------|-------|--------|
| FAT12/16/32 | `fat.c` | Yes | Yes | Production |
| exFAT | `exfat.c` | Probe/Mount only | No | Early |
| ISO 9660 | `iso9660.c` | Probe/Mount only | No | Early |
| UDF | `udf.c` | Probe/Mount only | No | Early |
| JFFS2 | `jffs2.c` | Probe/Mount only | No | Early |
| YAFFS | `yaffs.c` | Probe/Mount only | No | Early |
| LEAN | `lean.c` | Probe/Mount only | No | Early |
| Amiga FFS | `ffs_amiga.c` | Probe/Mount only | No | Early |
| z/OS Datasets | `zdsfs.c` | Probe/Mount only | No | Early |
| initrd (ustar) | `vfs.c` / `ustar.c` | Yes | No | Production |
| tmpfs | `fs.c` | Yes | Yes | Production |
| procfs | `procfs.c` | Yes | Partial | Partial |
| sysfs | `sysfs.c` | Yes | Partial | Partial |
| devfs | `devfs.c` | Yes | N/A | Production |
| ext2/3/4 | `fs.c` | Planned | No | Planned |
| symlinks | `symlink.c` | Yes | N/A | Production |

All drivers are gated by `ENABLE_VFS=yes` in `fern/build/features/filesystems.mk`. Individual drivers have independent toggles (`ENABLE_FAT32`, `ENABLE_EXFAT`, `ENABLE_ISO9660`, etc.).

---

## FAT12 / FAT16 / FAT32

**Source:** `fern/src/fat.c` (844 lines) and `fern/src/include/fat.h`

The FAT driver is the most mature filesystem in Forest OS, handling all three classic FAT variants through a single code path.

### Boot Sector Parsing

The driver reads the BIOS Parameter Block (BPB) from sector 0 and determines FAT type by cluster count:

- **< 4085 clusters** → FAT12
- **< 65525 clusters** → FAT16
- **≥ 65525 clusters** → FAT32

It validates the boot signature (`0xAA55`) and extracts `bytes_per_sector`, `sectors_per_cluster`, `reserved_sectors`, `num_fats`, `root_entries`, `total_sectors`, and `sectors_per_fat`.

### Cluster Chain Traversal

`fat_get_next_cluster()` follows cluster chains via the standard linear offset calculation:

- **FAT12:** `cluster + (cluster / 2)` bytes offset, 12-bit entries packed in pairs
- **FAT16:** `cluster * 2` bytes offset, 16-bit entries
- **FAT32:** `cluster * 4` bytes offset, 28-bit entries (upper 4 bits reserved)

End-of-chain markers: `0xFF8+` (FAT12), `0xFFF8+` (FAT16), `0x0FFFFFF8+` (FAT32).

### Directory and Write Support

Short filenames use classic 8.3 encoding. LFN entries are defined in the header (`fat_lfn_entry_t`) but not yet resolved in lookup.

Full write support includes:

- **`fat_alloc_cluster()`** — scans for free FAT entries
- **`fat_free_cluster_chain()`** — zeroes cluster chains
- **`fat_node_mkdir()`** — allocates cluster, writes `.` and `..` entries
- **`fat_node_unlink()`** — marks directory entry deleted (`0xE5`) and frees clusters

### VFS Integration

Each FAT node stores a `fat_node_t` in `vfs_node_t->internal_data`. The VFS operation table is set at `fat.c:496-501`:

```c
node->read = fat_node_read;
node->open = fat_node_open;
node->close = fat_node_close;
node->readdir = fat_node_readdir;
node->finddir = fat_node_finddir;
node->unlink = fat_node_unlink;
node->mkdir = fat_node_mkdir;
```

Registration: `fat_register()` → `vfs_register_filesystem()`.

---

## exFAT

**Source:** `fern/src/exfat.c` (76 lines) and `fern/src/include/exfat.h`

Early-stage driver that can probe and mount but not traverse files. Validates the `"EXFAT   "` identifier and parses the boot sector, including shift-based sector/cluster sizing (`1 << shift`).

**Limitations:** No `get_root`, no directory entry parsing, no file I/O. Probe confidence: 90.

---

## ISO 9660 (CD-ROM)

**Source:** `fern/src/iso9660.c` (70 lines) and `fern/src/include/iso9660.h`

Detects and parses the Primary Volume Descriptor (sector 16, type 1, identifier `"CD001"`). The header fully defines `iso_primary_vd_t`, `iso_dir_record_t`, and `iso_path_table_entry_t` structures.

**Limitations:** No directory record traversal, no Rock Ridge/Joliet extensions. Probe confidence: 100.

---

## UDF (Universal Disk Format)

**Source:** `fern/src/udf.c` (72 lines) and `fern/src/include/udf.h`

Recognizes UDF via `"NSR02"` or `"NSR03"` descriptors at sector 16. Defines `udf_tag_t`, `udf_descriptor_t`, and `udf_anchor_vdp_t` structures.

**Limitations:** Fixed 2048-byte blocks, no extent traversal. Probe confidence: 85.

---

## JFFS2 (Journaling Flash File System v2)

**Source:** `fern/src/jffs2.c` (66 lines) and `fern/src/include/jffs2.h`

Designed for NOR flash. Detects via magic `0x1985` (LE) or `0x2001` (BE). Uses page-level I/O (256-byte pages, 16-byte OOB) rather than block-level I/O.

Defines `jffs2_inode_node_t` (with compression fields) and `jffs2_dirent_node_t` (variable-length name). Probe confidence: 75.

---

## YAFFS (Yet Another Flash File System)

**Source:** `fern/src/yaffs.c` (66 lines) and `fern/src/include/yaffs.h`

For NAND flash. Validates `yaffs_obj_header_t` type field. Uses page + OOB architecture (512-byte pages, 16-byte OOB) with separate `read_page` and `read_oob` callbacks. Supports file, directory, symlink, hardlink, and special object types. Probe confidence: 70.

---

## LEAN

**Source:** `fern/src/lean.c` (80 lines) and `fern/src/include/lean.h`

A lightweight flash filesystem. Scans blocks 1–32 for magic `0x4E41454C`. The `lean_inode_t` supports up to 6 direct extents, indirect block chains, fork support, and full POSIX timestamps. Probe confidence: 80.

---

## Amiga FFS (Fast File System)

**Source:** `fern/src/ffs_amiga.c` (82 lines) and `fern/src/include/ffs_amiga.h`

Targets Amiga OS. Validates root block type `0x444F` and Amiga-style checksum (16-bit ones-complement sum). Defines `ffs_root_block_t` with hash table for directory entries. Probe confidence: 65.

---

## z/OS Dataset Filesystem (ZDSFS)

**Source:** `fern/src/zdsfs.c` (51 lines) and `fern/src/include/zdsfs.h`

Reads mainframe DASD dataset structures using CCHHR (Cylinder/Head/Record) addressing. Validates `format == 0xF1` in DSCB1 records. The `dscb1_t` structure mirrors IBM's DSCB format. Probe confidence: 60.

---

## initrd (Initial RAM Disk)

**Source:** `fern/src/vfs.c` and `fern/src/ustar.c`

Forest OS's root filesystem at boot. A read-only ramdisk loaded from a ustar archive. Features path normalization with `..`/`.` support, case-insensitive lookup, automatic PATH prefix resolution (`bin/`, `usr/bin/`, `sbin/`, `usr/sbin/`), and ELF extension auto-resolution.

---

## tmpfs

**Source:** `fern/src/fs.c` (inline in VFS core)

In-memory filesystem with full read/write support. The only filesystem currently supporting hard links (`vfs_link()`). Gated by `ENABLE_TMPFS`. No persistence across reboots.

---

## Virtual Filesystems

- **procfs** (`procfs.c`) — Exposes kernel/process info. Integration with `vfs_open()` is commented out.
- **sysfs** (`sysfs.c`) — Device/driver information. Also commented out in VFS layer.
- **devfs** (`devfs.c`) — Device nodes at `/dev/`. Fully integrated; `vfs_open("/dev/...")` routes directly to `devfs_open()`.

---

## Filesystem Driver Registration

Forest OS uses two registration mechanisms:

### VFS-Level Registration

Each driver calls `vfs_register_filesystem()` with a `vfs_filesystem_t` struct:

```c
int fat_register(void) {
    vfs_filesystem_t* vfs_fs = enhanced_heap_alloc(sizeof(vfs_filesystem_t), ...);
    vfs_fs->name = "fat";
    vfs_fs->probe = fat_probe;
    vfs_fs->mount = fat_mount;
    vfs_fs->umount = fat_umount;
    vfs_fs->get_root = fat_get_root;
    return vfs_register_filesystem(vfs_fs);
}
```

The function (`vfs.c:830-838`) prepends to a singly-linked list. Lookup by name via `vfs_get_filesystem()`.

### Architecture-Level Registration

`arch/fs.c` provides a second registry via `arch_fs_register_type()` for `arch_fs_type_t` structs. This handles block-device-based filesystems needing architecture-specific probing. `arch_fs_try_mount()` bridges the two registries.

### Mount Flow

1. `vfs_mount(device, mountpoint, fstype, ...)` looks up the filesystem by name
2. Calls `fs->mount()` to initialize the superblock
3. Calls `fs->get_root()` for the root VFS node
4. Creates a `vfs_mount_t` entry in the mount table

---

## Driver Architecture

```
┌─────────────────────────────────────────────────┐
│              Syscall Layer                       │
│         vfs_open / vfs_read / vfs_write          │
├─────────────────────────────────────────────────┤
│               VFS Core                           │
│  vfs.c  │  Mount table  │  Path resolution       │
│         │  Deferred ops  │  Symlink resolution    │
├─────────┴───────────────┴────────────────────────┤
│             Filesystem Drivers                   │
│  fat.c │ exfat.c │ iso9660.c │ udf.c │ lean.c   │
├─────────────────────────────────────────────────┤
│             Block Device Layer                   │
│  arch/fs.c  │  AHCI  │  virtio-blk  │  ATA      │
├─────────────────────────────────────────────────┤
│             Hardware                             │
│  PCI  │  DTB  │  MMIO  │  Flash NAND/NOR        │
└─────────────────────────────────────────────────┘
```

### Callback Interface

Each filesystem driver provides:

| Callback | Purpose |
|----------|---------|
| `probe` | Detect filesystem, return confidence (0–100) |
| `mount` | Parse superblock, initialize state |
| `umount` | Free state, flush dirty data |
| `get_root` | Return VFS node for root directory |

VFS nodes add: `read`, `write`, `open`/`close`, `readdir`, `finddir`, `unlink`, `mkdir`.

### I/O Abstraction

Block-based drivers use sector callbacks:

```c
typedef uint32_t (*fat_read_sector_fn)(void* dev_data, uint64_t lba, uint8_t* buffer);
```

Flash drivers use page-level I/O:

```c
typedef uint64_t (*jffs2_read_page_fn)(void* dev_data, uint64_t offset, uint8_t* buffer);
```

---

## Deferred Operations

Forest OS supports deferred filesystem operations for open files. When a file is deleted while open, the VFS marks it `VFS_DELETED` and queues a `VFS_DEFERRED_DELETE`. Actual deletion occurs when the last handle closes (`fat_node_close()` checks `open_count == 0 && flags & VFS_DELETED`). The queue is processed by `vfs_process_deferred_ops()`.

---

## Performance Characteristics

| Filesystem | Sequential Read | Random Access | Write | Memory | Notes |
|------------|----------------|---------------|-------|--------|-------|
| FAT12/16/32 | Good | Poor | Moderate | Low | FAT table not cached; cluster chains traversed per-read |
| ISO 9660 / UDF | Excellent | Moderate | N/A | Very Low | Designed for sequential optical media |
| JFFS2 / YAFFS | Moderate | Moderate | Good (append) | Moderate | Optimized for flash endurance |
| tmpfs | Fast | Fast | Fast | Variable | In-memory only; no persistence |
| initrd | Fast | Fast | N/A | Fixed | In-memory after boot; read-only |

FAT random access is poor because each seek must re-traverse the cluster chain from the FAT. Flash filesystems trade raw throughput for wear leveling and power-fail safety.

---

## Build Configuration

`fern/build/features/filesystems.mk` provides granular control:

```makefile
ENABLE_VFS=yes        # Master switch for entire FS layer
ENABLE_FAT32=yes      # FAT12/16/32 driver
ENABLE_EXFAT=yes      # exFAT driver
ENABLE_ISO9660=yes    # ISO 9660 driver
ENABLE_UDF=yes        # UDF driver
ENABLE_LEAN=yes       # LEAN driver
ENABLE_YAFFS=yes      # YAFFS driver
ENABLE_JFFS2=yes      # JFFS2 driver
ENABLE_FFS_AMIGA=yes  # Amiga FFS driver
ENABLE_ZDSFS=yes      # z/OS dataset driver
ENABLE_TMPFS=yes      # tmpfs (in VFS core)
ENABLE_PROCFS=yes     # procfs
ENABLE_SYSFS=yes      # sysfs
ENABLE_DEVFS=yes      # devfs
ENABLE_RAMDISK=yes    # ramdisk support
ENABLE_SYMLINKS=yes   # symbolic link support
```

Setting `ENABLE_VFS=no` excludes all filesystem sources. Individual flags only matter when VFS is enabled.

---

## Future Directions

1. **ext2/3/4** — Build system references `ENABLE_EXT2` but no source file exists yet.
2. **exFAT completion** — Directory entry parsing and file reads are the main gaps.
3. **ISO 9660 file traversal** — Volume descriptor parsing is complete; directory walking is next.
4. **Flash filesystems** — JFFS2 and YAFFS need inode tree construction and file I/O.
5. **procfs/sysfs integration** — Implementations exist but are commented out in VFS.
6. **FAT LFN support** — `fat_lfn_entry_t` is defined but not wired into directory lookup.

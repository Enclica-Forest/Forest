# Disk Tools

Forest OS provides a full suite of disk management utilities covering data copying, filesystem creation, mounting, partition inspection, and integrity checking. All tools are implemented in userspace and interact with the kernel through standard Linux syscalls.

## Overview

The disk tool suite in `/home/bluet/forest/userspace/` includes:

| Tool | Purpose | Source |
|------|---------|--------|
| `dd` | Data conversion and block-level copying | `dd/dd.c` |
| `df` | Filesystem disk space usage reporting | `df/df.c` |
| `du` | Directory space usage estimation | `du/du.c` |
| `fdisk` | MBR partition table inspection | `fdisk/fdisk.c` |
| `mkfs` | Filesystem creation (FAT32, ext2) | `mkfs/mkfs.c` |
| `mount` | Attach filesystems to the tree | `mount/mount.c` |
| `umount` | Detach filesystems from the tree | `umount/umount.c` |
| `blkid` | Block device identification | `blkid/blkid.c` |
| `losetup` | Loop device management | `losetup/losetup.c` |
| `fsck` | Filesystem integrity checking | `fsck/fsck.c` |

---

## Data Conversion and Copying (dd)

`dd` copies data between files and devices at the block level. It is the workhorse for creating disk images, writing boot sectors, and raw device operations.

### Usage

```
dd if=INPUT of=OUTPUT [bs=BYTES] [count=N] [skip=N] [seek=N] [conv=CONV] [iflag=FLAGS] [oflag=FLAGS]
```

### Key Parameters

| Parameter | Description |
|-----------|-------------|
| `bs=BYTES` | Block size for both read and write (default 512) |
| `ibs=BYTES` | Input block size |
| `obs=BYTES` | Output block size |
| `count=N` | Copy only N input blocks |
| `skip=N` | Skip N blocks at start of input |
| `seek=N` | Skip N blocks at start of output |
| `if=FILE` | Input file (default stdin) |
| `of=FILE` | Output file (default stdout) |
| `conv=noerror` | Continue past read errors |
| `conv=sync` | Pad incomplete blocks with NUL |
| `conv=notrunc` | Do not truncate output file |
| `conv=excl` | Fail if output already exists |
| `conv=fdatasync` | Flush output data before completing |
| `iflag=direct` | Use O_DIRECT (bypass page cache) |
| `iflag=dsync` | Use O_DSYNC for data writes |
| `oflag=append` | Open output in append mode |

### Examples

```bash
# Create a 1MB disk image file
dd if=/dev/zero of=disk.img bs=1M count=1

# Write an ISO to a USB device
dd if=forest.iso of=/dev/sdb bs=4M

# Read the MBR of a disk
dd if=/dev/sda of=mbr.bin bs=512 count=1

# Copy with direct I/O for benchmarking
dd if=/dev/sda of=/dev/sdb bs=1M iflag=direct oflag=direct
```

### Implementation Notes

- Uses `xread()`/`xwrite()` wrappers that handle partial reads/writes and `EINTR` interruptions.
- Supports human-friendly size suffixes: `k`/`K` (1024), `m`/`M` (1024²), `g`/`G` (1024³).
- Signal handling allows graceful `SIGINT` interrupts with partial progress stats.
- Reports throughput in MB/s after completion.

---

## Disk Usage Reporting (df, du)

### df — Filesystem Space Usage

`df` reads `/proc/mounts` (or `/etc/mtab`) to enumerate mounted filesystems, then calls `statfs()` on each mount point to retrieve block counts.

#### Usage

```
df [-hHkmaTP] [-t TYPE] [-x TYPE] [--total] [file ...]
```

#### Key Options

| Option | Description |
|--------|-------------|
| `-h` | Human-readable sizes (powers of 1024) |
| `-H` | SI units (powers of 1000) |
| `-k` | Show sizes in 1K blocks (default) |
| `-m` | Show sizes in 1M blocks |
| `-T` | Print filesystem type column |
| `-t TYPE` | Include only filesystems of TYPE |
| `-x TYPE` | Exclude filesystems of TYPE |
| `-a` | Show pseudo-filesystems (proc, sysfs, etc.) |
| `-P` | POSIX output format |
| `--total` | Print a grand total line |

#### Filtering

`df` automatically hides pseudo-filesystems (proc, sysfs, devtmpfs, tmpfs, cgroup, etc.) unless `-a` is passed. The `should_show()` function checks the filesystem type string against a hardcoded list of virtual filesystems.

#### Kernel Interface

`df` makes a direct `syscall(SYS_statfs, path, &buf)` call. The returned `struct statfs` contains:
- `f_blocks` — total data blocks
- `f_bfree` — free blocks (including reserved)
- `f_bavail` — free blocks available to non-root
- `f_bsize` — optimal transfer block size
- `f_files` — total inodes
- `f_ffree` — free inodes

#### Example

```
Filesystem        1024-blocks     Used Available Use% Mounted on
/dev/sda1          102400000  51200000  51200000   50% /
tmpfs                 512000       128    511872    1% /tmp
```

---

### du — Directory Space Usage

`du` recursively walks directories using `opendir()`/`readdir()` and `stat()` to accumulate block counts.

#### Usage

```
du [-hHkmsacd:LxP] [--total] [--max-depth=N] [file ...]
```

#### Key Options

| Option | Description |
|--------|-------------|
| `-h` | Human-readable (1024-based) |
| `-H` | Human-readable (1000-based) |
| `-k` | Kilobytes (default) |
| `-m` | Megabytes |
| `-s` | Summarize — show only totals |
| `-a` | Show sizes for all files, not just directories |
| `-c` | Print grand total |
| `-d N` | Limit recursion depth to N |
| `-L` | Follow symbolic links |
| `-P` | Do not follow symlinks (default) |
| `-x` | Stay on the same filesystem |

#### Kernel Interface

Uses `lstat()` (or `stat()` with `-L`) to get `st_blocks` from `struct stat`. Each filesystem block is 512 bytes, so the tool divides `st_blocks / 2` to get 1K block counts.

---

## Partition Management (fdisk)

`fdisk` reads and displays MBR (Master Boot Record) partition tables. It supports listing partitions, querying partition sizes, and identifying partition types.

### Usage

```
fdisk [-lpsv] [-s PARTITION] device
```

| Option | Description |
|--------|-------------|
| `-l` | List all partitions on the device |
| `-p` | Print in parseable CSV format |
| `-s PARTITION` | Print size of partition N in sectors |
| `-v` | Print version |

### MBR Layout

The tool reads the 512-byte MBR structure:

```
Offset  Size  Description
0       446   Boot code
446     16    Partition entry 1
462     16    Partition entry 2
478     16    Partition entry 3
494     16    Partition entry 4
510     2     Signature (0xAA55)
```

Each partition entry contains:
- Boot indicator (0x80 = active/bootable)
- CHS addresses (not used in LBA mode)
- Partition type byte
- LBA start sector and sector count

### Supported Partition Types

The tool recognizes 50+ partition types including:
- `0x83` — Linux
- `0x82` — Linux swap
- `0x0C` — FAT32 LBA
- `0x07` — NTFS
- `0xEE` — EFI GPT
- `0xA5` — FreeBSD
- `0xA6` — OpenBSD

### Example

```
Disk /dev/sda: 500107862 sectors
Device     Boot   Start      End  Sectors  Size    Type
/dev/sda1  *            0  2097151  2097152    1.0 GiB Linux
/dev/sda2          2097152 10485759  8388608    4.0 GiB Linux swap
/dev/sda3         10485760 97677311 87191551   41.5 GiB Linux
```

---

## Filesystem Creation (mkfs)

`mkfs` creates filesystems on block devices or regular files. It currently supports FAT32 and ext2.

### Usage

```
mkfs [-t fat32|ext2] [-b BLOCK_SIZE] [-i INODES] [-L LABEL] [-v] [-f] device
```

| Option | Description |
|--------|-------------|
| `-t TYPE` | Filesystem type: `fat32`/`vfat` or `ext2` |
| `-b SIZE` | Block size (must be power of 2) |
| `-i COUNT` | Number of inodes |
| `-L LABEL` | Volume label (up to 11 chars for FAT32) |
| `-v` | Verbose output |
| `-f` | Force (skip existing partition check) |

### FAT32 Creation

The FAT32 creator:
1. Calculates cluster geometry based on volume size
2. Writes the Volume Boot Record (VBR) with a complete BPB (BIOS Parameter Block)
3. Initializes the FSInfo sector with free cluster hints
4. Creates both FAT copies with media type and root directory markers
5. Writes an empty root directory at cluster 2
6. Updates the MBR partition type to 0x0C (FAT32 LBA)

Cluster sizing adapts to volume size: 1 sector/cluster for < 1GB, scaling up to 64 sectors/cluster for > 32GB.

### ext2 Creation

The ext2 creator:
1. Writes the superblock at offset 1024 with magic `0xEF53`
2. Initializes block group descriptors
3. Creates block and inode bitmaps marking system blocks as used
4. Writes the inode table with a root inode (mode 040755)
5. Creates the root directory with `.` and `..` entries
6. Sets MBR partition type to 0x83 (Linux)

### Kernel Interface

`mkfs` uses:
- `stat()` to determine device size (for regular files)
- `open()` with `O_WRONLY` (or `O_RDWR` with `-f`)
- `lseek()` + `read()`/`write()` for raw sector I/O

---

## Mounting and Unmounting (mount, umount)

### mount — Attach Filesystems

`mount` attaches a filesystem to the directory tree using the `mount()` syscall.

#### Usage

```
mount [-t TYPE] [-o OPTIONS] [-rvwfn] [-a] device mountpoint
```

| Option | Description |
|--------|-------------|
| `-t TYPE` | Filesystem type |
| `-o OPTIONS` | Comma-separated mount options |
| `-r` | Mount read-only |
| `-w` | Mount read-write |
| `-v` | Verbose output |
| `-f` | Fake mount (don't actually mount) |
| `-n` | Don't write to /etc/mtab |
| `-a` | Mount all entries from /etc/fstab |

#### Supported Mount Options

The option parser translates string options to kernel `MS_*` flags:

| Option | Flag | Description |
|--------|------|-------------|
| `ro` | `MS_RDONLY` | Read-only |
| `rw` | — | Read-write |
| `nosuid` | `MS_NOSUID` | Ignore SUID/SGID bits |
| `nodev` | `MS_NODEV` | No device nodes |
| `noexec` | `MS_NOEXEC` | No program execution |
| `sync` | `MS_SYNCHRONOUS` | Synchronous I/O |
| `remount` | `MS_REMOUNT` | Remount with new flags |
| `bind` | `MS_BIND` | Bind mount |
| `noatime` | `MS_NOATIME` | No access time updates |
| `relatime` | `MS_RELATIME` | Relative access time |

#### Loop Device Support

When mounting a regular file (e.g., a disk image), `mount` automatically:
1. Scans `/dev/loop0` through `/dev/loop7` for a free device
2. Opens the loop device and the backing file
3. Calls `ioctl(fd, LOOP_SET_FD, filefd)` to associate them
4. Performs the mount on the loop device

#### fstab Integration

`mount -a` reads `/etc/fstab` and mounts all entries not marked `noauto`. Each line is parsed as: `device mountpoint fstype options dump pass`.

#### mtab Updates

After a successful mount, the tool writes an entry to `/etc/mtab` with the device, mountpoint, filesystem type, and options string.

---

### umount — Detach Filesystems

`umount` detaches filesystems using the `umount2()` syscall.

#### Usage

```
umount [-a] [-r] [-f] [-n] [-v] [-l] [mountpoint...]
```

| Option | Description |
|--------|-------------|
| `-a` | Unmount all (except /, /proc, /sys, /dev, /run) |
| `-r` | Remount read-only before unmounting |
| `-f` | Force unmount |
| `-n` | Don't write to /etc/mtab |
| `-v` | Verbose output |
| `-l` | Lazy unmount (MNT_DETACH) |

#### Lazy Unmount

The `-l` flag uses `MNT_DETACH`, which immediately disconnects the filesystem from the namespace. Any in-progress I/O continues in the background, and resources are cleaned up when all references are released. This is useful for unmounting busy filesystems.

#### mtab Cleanup

`umount` removes the corresponding entry from `/etc/mtab` by:
1. Reading all mtab entries into memory
2. Writing all non-matching entries to a temp file
3. Atomically renaming the temp file over the original

---

## Block Device Identification (blkid)

`blkid` probes block devices to identify their filesystem type, UUID, and label. It reads magic numbers and metadata structures directly from disk.

### Usage

```
blkid [-o FORMAT] [-p] [-s TAG] [-t TAG] [device ...]
```

| Option | Description |
|--------|-------------|
| `-o FORMAT` | Output format: `full`, `value`, `device`, `export` |
| `-p` | Low-level probing mode |
| `-s TAG` | Show only a specific tag |
| `-t TAG` | Search for devices with tag |

### Detection Methods

The probe chain tries each detection method in order:

1. **ext2/3/4** — Reads superblock at offset 1024, checks magic `0xEF53`, then examines feature flags to distinguish ext2 (none), ext3 (`INCOMPAT_JOURNAL`), and ext4 (`RO_COMPAT_LARGE_FILE`)
2. **FAT** — Reads boot sector, checks jump instruction (0xEB or 0xE9), calculates total clusters to determine FAT12/FAT16/FAT32, reads volume serial and label from extended BPB
3. **NTFS** — Checks "NTFS" signature at offset 3, follows MFT entry to extract volume name
4. **ISO 9660** — Reads "CD001" signature at offset 0x8001
5. **Swap** — Checks for "SWAPSPACE2" or "SWAP1" magic at offset 4086

### Output Formats

```
# full (default)
/dev/sda1: TYPE="ext4" UUID="A1B2-C3D4-E5F6-7890-ABCDEF012345" LABEL="root"

# value
ext4

# device
/dev/sda1

# export (shell-sourceable)
DEVNAME=/dev/sda1
TYPE=ext4
UUID=A1B2-C3D4-E5F6-7890-ABCDEF012345
LABEL=root
```

### Device Scanning

Without arguments, `blkid` probes a built-in list: `/dev/sda` through `/dev/sdd`, `/dev/hda`-`hdb`, `/dev/vda`-`vdc`.

---

## Loop Device Management (losetup)

`losetup` manages loop devices — block devices that provide a block-level interface to regular files.

### Usage

```
losetup [options] file [device]
losetup -a
losetup -d device
losetup -f
```

| Option | Description |
|--------|-------------|
| `-a`, `--all` | List all configured loop devices |
| `-d DEV`, `--detach DEV` | Detach a loop device |
| `-f`, `--find` | Find the first unused loop device |
| `-o OFFSET`, `--offset N` | Start at byte offset N |
| `--sizelimit N` | Maximum size of loop device |
| `-r`, `--read-only` | Set up read-only loop device |
| `-P`, `--partscan` | Scan for partitions |
| `--show` | Print device name on success |

### Kernel Interface

Loop devices are controlled entirely through `ioctl()` calls:

| ioctl | Purpose |
|-------|---------|
| `LOOP_SET_FD` | Associate a file descriptor with the loop device |
| `LOOP_CLR_FD` | Detach the backing file |
| `LOOP_SET_STATUS64` | Set offset, size limit, read-only flag |
| `LOOP_GET_STATUS64` | Query current loop device status |

### Loop Status Structure

The `loop_info64` structure returned by `LOOP_GET_STATUS64` contains:
- `lo_device` — device number (0 if not configured)
- `lo_inode` — inode number of backing file
- `lo_offset` — byte offset into backing file
- `lo_sizelimit` — maximum size (0 = no limit)
- `lo_flags` — `LO_FLAGS_READ_ONLY`, etc.
- `lo_file_name` — path to backing file

### Examples

```bash
# Create a loop device for a disk image
losetup /dev/loop0 disk.img

# Create with an offset (e.g., skip MBR to access partition)
losetup -o 1048576 --show disk.img

# List all active loop devices
losetup -a

# Find and use the first free loop device
losetup -f --show disk.img

# Detach a loop device
losetup -d /dev/loop0
```

---

## Filesystem Checking (fsck)

`fsck` examines filesystems for structural integrity and optionally repairs errors. It supports ext2/ext3/ext4 and FAT32.

### Usage

```
fsck [-t TYPE] [-a] [-y] [-n] [-r] [-f] [-p] [-C] [-v] device
```

| Option | Description |
|--------|-------------|
| `-t TYPE` | Filesystem type hint |
| `-a` | Auto-fix errors |
| `-y` | Answer yes to all repair questions |
| `-n` | No-fixes mode (read-only check) |
| `-r` | Interactive repair |
| `-f` | Force check even if filesystem appears clean |
| `-p` | Preen mode (automatic safe repairs) |
| `-C` | Show progress |
| `-v` | Verbose output |

### Detection

If no `-t` is specified, `fsck` auto-detects the filesystem type:
1. Reads sector 0 and checks for FAT boot signature (0xEB/0xE9 jump instruction)
2. Reads superblock at offset 1024 and checks for ext2 magic `0xEF53`
3. Distinguishes ext2/ext3/ext4 via feature flags

### ext2/3/4 Checks

The checker performs these validations:

1. **Superblock validation** — magic number, block size (1024/2048/4096/8192), inode/block counts, free count consistency
2. **Block group descriptors** — validates block bitmap, inode bitmap, and inode table block references
3. **Inode allocation** — checks inode table structure
4. **Block allocation** — checks block bitmap consistency
5. **Directory structure** — validates directory entry chains

### FAT32 Checks

1. **Boot sector** — validates BPB fields (bytes per sector, sectors per cluster, FAT count), checks boot signature (0x55AA)
2. **FAT table** — reads first FAT sector, verifies media type signature
3. **Cluster chains** — basic chain validation
4. **Directory entries** — entry structure validation

### Error Handling

- `errors_found` and `errors_fixed` counters track progress
- `ask_user()` prompts for confirmation in interactive mode
- Auto-fix mode (`-a`) corrects safe issues without prompting
- Read-only mode (`-n`) reports issues without modification

---

## Kernel Syscall Interactions

All disk tools ultimately communicate with the kernel through a small set of syscalls:

| Syscall | Used By | Purpose |
|---------|---------|---------|
| `open()` | All tools | Open devices and files |
| `read()` / `write()` | dd, mkfs, fsck, blkid | Raw block I/O |
| `pread()` | blkid | Positioned reads at specific offsets |
| `lseek()` | dd, mkfs, fsck | Seek to specific positions |
| `close()` | All tools | Release file descriptors |
| `stat()` / `lstat()` | du, mkfs, fsck | Get file metadata |
| `statfs()` | df | Get filesystem statistics |
| `mount()` | mount | Attach filesystem to VFS tree |
| `umount2()` | umount, mount | Detach filesystem from VFS tree |
| `ioctl()` | losetup, mount | Loop device control |
| `mkdir()` | mount | Create mount points |
| `opendir()` / `readdir()` | du | Directory traversal |
| `signal()` | dd | Handle SIGINT gracefully |
| `syscall(SYS_statfs, ...)` | df | Direct syscall for statfs |

### Direct Syscall Usage

`df` uses `syscall(SYS_statfs, path, buf)` instead of the libc wrapper, giving it direct control over the kernel interface. This is useful in minimal environments where libc wrappers may not be available.

---

## Implementation Highlights

### Error Resilience

- `dd` continues past read errors with `conv=noerror` and can pad incomplete blocks with `conv=sync`
- `fsck` separates error detection from correction, allowing read-only auditing
- `umount` uses `MNT_DETACH` for lazy unmounting of busy filesystems

### Portable Conventions

- All tools use POSIX `getopt()`/`getopt_long()` for argument parsing
- Size parsing supports suffixes (k, M, G) consistently across tools
- Output formatting follows standard conventions for scriptability

### Filesystem Support Matrix

| Tool | ext2 | ext3 | ext4 | FAT12 | FAT16 | FAT32 | NTFS | iso9660 | swap |
|------|------|------|------|-------|-------|-------|------|---------|------|
| blkid | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| fsck | ✓ | ✓ | ✓ | — | — | ✓ | — | — | — |
| mkfs | ✓ | — | — | — | — | ✓ | — | — | — |
| fdisk | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | — | ✓ |

### Design Patterns

- **Sector-level I/O** — All filesystem creators/checkers use direct `lseek()` + `read()`/`write()` rather than higher-level abstractions, giving full control over byte positioning
- **Signal safety** — `dd` uses a volatile flag and `signal()` handler to allow graceful SIGINT interruption without data corruption
- **Atomic mtab updates** — `umount` writes to a temp file and renames atomically, preventing corruption of `/etc/mtab` on crash
- **Automatic loop detection** — `mount` scans `/dev/loop*` for free devices when mounting image files

---

## Quick Reference

| Command | What It Does |
|---------|--------------|
| `dd if=/dev/zero of=test.img bs=1M count=100` | Create a 100MB zeroed image |
| `dd if=disk.img of=/dev/sdb bs=4M` | Write image to USB device |
| `df -h` | Show disk usage in human-readable format |
| `df -t ext4` | Show only ext4 filesystems |
| `du -sh /home` | Total size of /home |
| `du --max-depth=1 -h` | One-level directory sizes |
| `fdisk -l /dev/sda` | List partitions on sda |
| `fdisk -s /dev/sda1` | Size of partition 1 in sectors |
| `mkfs -t fat32 -L MYDISK /dev/sdb1` | Create FAT32 filesystem |
| `mkfs -t ext2 -L root /dev/sda1` | Create ext2 filesystem |
| `mount /dev/sda1 /mnt` | Mount partition to /mnt |
| `mount -o loop disk.img /mnt` | Mount image file via loop |
| `mount -a` | Mount all from fstab |
| `mount` | List mounted filesystems |
| `umount /mnt` | Unmount /mnt |
| `umount -a` | Unmount all non-essential filesystems |
| `umount -l /mnt` | Lazy unmount |
| `blkid /dev/sda1` | Identify filesystem type and UUID |
| `blkid -o export /dev/sda1` | Export format for scripts |
| `losetup -f --show disk.img` | Create loop device for image |
| `losetup -a` | List active loop devices |
| `losetup -d /dev/loop0` | Detach loop device |
| `fsck /dev/sda1` | Check filesystem integrity |
| `fsck -a /dev/sda1` | Auto-fix filesystem errors |
| `fsck -n /dev/sda1` | Read-only check (no fixes) |

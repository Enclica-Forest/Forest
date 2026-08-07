# Forest OS Initrd Builder

The initrd (initial RAM disk) is the first filesystem the Forest OS kernel mounts
during boot. It contains all userspace tools needed to get the system running.
The initrd builder is the host-side tool that packages these files into a
bootable CPIO image.

---

## Table of Contents

1. [What is an Initrd?](#what-is-an-initrd)
2. [The Initrd Builder Tool](#the-initrd-builder-tool)
3. [Initrd Format (CPIO)](#initrd-format-cpio)
4. [Directory Structure](#directory-structure)
5. [Creating Custom Initrds](#creating-custom-initrds)
6. [Essential Binaries](#essential-binaries)
7. [Configuration Files](#configuration-files)
8. [How the Kernel Mounts the Initrd](#how-the-kernel-mounts-the-initrd)
9. [Tips for Minimizing Initrd Size](#tips-for-minimizing-initrd-size)
10. [Debugging Initrd Issues](#debugging-initrd-issues)

---

## What is an Initrd?

An initrd is an archive loaded into memory by the bootloader before the kernel
starts. The kernel mounts it as a temporary root filesystem, allowing boot to
continue without a real disk.

In Forest OS, the initrd is the **permanent** root filesystem. It contains
`/init` (PID 1), core utilities, the shell, and libc. When the kernel boots,
it extracts the initrd into RAM and runs `/init`:

```
Bootloader -> Load kernel + initrd into RAM
  -> Kernel extracts initrd as rootfs
  -> Kernel exec /init (PID 1)
  -> init mounts /proc, /dev
  -> init starts the shell
```

---

## The Initrd Builder Tool

Located at `userspace/initrd-builder/`. This is a **host** tool (not
cross-compiled).

### Building

```bash
cd forest/userspace/initrd-builder
make
# Produces build/initrd-builder
sudo make install  # optional, installs to /usr/local/bin
```

### Usage

```
initrd-builder [OPTIONS]
  -o FILE    Output file (required)
  -d DIR     Source directory (required)
  -c FILE    Config file listing files/dirs to include
  -s SIZE    Maximum size in bytes
  -z         Enable gzip compression
  -v         Verbose output
  -f         Force overwrite
```

### Examples

```bash
# Basic build
./build/initrd-builder -o initrd.img -d forest/fern/initrd -v -f

# Compressed build
./build/initrd-builder -o initrd.img -d my-initrd -z -f

# Build with config file
./build/initrd-builder -o initrd.img -d my-initrd -c files.conf -f
```

### How It Works

1. Recursively walks the source directory, collecting entries via `lstat()`
2. Sorts entries alphabetically for consistent output
3. Writes a 110-byte CPIO newc header per entry (magic `070701`)
4. Writes file data padded to 4-byte alignment
5. Writes `TRAILER!!!` to mark end of archive
6. Optionally pipes through `gzip -9`

---

## Initrd Format (CPIO)

Forest OS uses the **cpio newc** format -- the standard for Linux initrds.

### CPIO Newc Header (110 bytes)

```
Offset  Size  Field
  0      6    Magic ("070701")
  6      8    Inode number (hex)
 14      8    Mode (hex, includes file type)
 22      8    UID
 30      8    GID
 38      8    Nlink
 46      8    Mtime
 54      8    File size
 62      8    Major dev
 70      8    Minor dev
 78      8    Rdev major
 86      8    Rdev minor
 94      8    Namesize (includes null terminator)
102      8    Checksum (0 for newc)
```

After the header: null-terminated filename (padded to 4 bytes), then file
data (also padded to 4 bytes). Archive ends with `TRAILER!!!`.

### Why CPIO?

- Kernel parses it natively
- Handles device nodes, symlinks, special files
- Standard format used by Linux, dracut, mkinitcpio
- gzip-compressed cpio is universally supported

---

## Directory Structure

The default initrd tree in `fern/initrd/`:

```
initrd/
├── bin/          # Core utilities (cat, ls, grep, init, sh)
├── dev/          # Device nodes (empty; kernel populates)
├── etc/          # Configuration files
├── proc/         # Mount point for /proc
├── tmp/          # Temporary storage (tmpfs mount point)
├── usr/
│   ├── bin/      # Additional binaries
│   ├── lib/      # Libraries (libc.a)
│   └── share/    # Shared data
└── var/
    └── log/      # Log directory
```

| Directory | Purpose |
|-----------|---------|
| `bin/` | Essential single-user binaries |
| `dev/` | Device nodes; mount point for devtmpfs |
| `etc/` | passwd, group, fstab, init config |
| `proc/` | Mount point for procfs |
| `tmp/` | Mount point for tmpfs |
| `usr/lib/` | Static libraries (libc.a) |

---

## Creating Custom Initrds

### Step 1: Prepare the Tree

```bash
cp -r forest/fern/initrd my-initrd
cp my-new-tool my-initrd/bin/
```

### Step 2: Create Device Nodes (if needed)

```bash
sudo mknod my-initrd/dev/console c 5 1
sudo mknod my-initrd/dev/null c 1 3
```

### Step 3: Build

```bash
./build/initrd-builder -o my-initrd.img -d my-initrd -v -f
```

### Using a Config File

For precise control, create a config file:

```
# Format: path [mode] [uid] [gid] [type]
bin/init 0755 0 0 file
bin/sh 0755 0 0 file
bin/ls 0755 0 0 file
etc/passwd 0644 0 0 file
dev/console 0600 0 0 char
dev/null 0666 0 0 char
proc 0755 0 0 dir
tmp 1777 0 0 dir
```

Then build:

```bash
./build/initrd-builder -o my-initrd.img -d my-initrd -c my-initrd.conf -f
```

---

## Essential Binaries

The Forest OS initrd ships 40 utilities in `bin/`:

| Binary | Purpose | Binary | Purpose |
|--------|---------|--------|---------|
| `init` | PID 1, starts shell | `mount` | Mount filesystems |
| `sh` | Interactive shell | `umount` | Unmount filesystems |
| `ls` | List files | `cat` | Display file contents |
| `cp` | Copy files | `mv` | Move/rename files |
| `rm` | Remove files | `mkdir` | Create directories |
| `grep` | Search patterns | `find` | Find files |
| `echo` | Print text | `ps` | List processes |
| `kill` | Send signals | `reboot` | Reboot system |
| `chmod` | Change permissions | `ln` | Create links |
| `df` / `du` | Disk usage | `head` / `tail` | Show file lines |
| `sort` / `wc` | Text processing | `sleep` | Delay execution |
| `date` / `hostname` | System info | `id` / `uname` | User/kernel info |

---

## Configuration Files

Minimum required files in `/etc/`:

### `/etc/passwd`

```
root:x:0:0:root:/root:/bin/sh
```

### `/etc/group`

```
root:x:0:
```

### `/etc/fstab`

```
# device  mountpoint  type  options  dump  pass
proc      /proc       proc  defaults 0     0
tmpfs     /tmp        tmpfs defaults 0     0
```

Any app-specific config can go in `/etc/` as well:

```
# /etc/init.conf
SHELL=/bin/sh
LOGLEVEL=2
```

---

## How the Kernel Mounts the Initrd

### Boot Flow

1. Bootloader loads kernel + initrd into memory
2. Kernel decompresses itself
3. Kernel parses the CPIO archive from memory
4. Kernel creates a rootfs (ramfs/devtmpfs) and extracts files
5. Kernel execs `/init` as PID 1

### Kernel Configuration

```
CONFIG_BLK_DEV_INITRD=y
CONFIG_RD_GZIP=y          # compressed initrds
CONFIG_RD_CPIO=y          # cpio format
```

If `/init` is missing, the kernel panics with "No init found."

---

## Tips for Minimizing Initrd Size

### 1. Strip Binaries

```bash
i686-forestos-strip --strip-all mytool
# Typically reduces size by 50-80%
```

### 2. Static Linking

Avoids needing shared libraries:

```bash
i686-forestos-gcc -static -o mytool mytool.c
```

### 3. Use Compression

```bash
./build/initrd-builder -o initrd.img -d my-initrd -z -f
```

### 4. Audit Contents

```bash
du -sh my-initrd/bin/* | sort -rh | head -20
```

### 5. Use Config Files

Include only what you need instead of entire directories.

### 6. Target 32-bit

32-bit binaries are smaller:

```bash
i686-forestos-gcc -static -o mytool mytool.c
```

### Size Reference

| Scenario | Size |
|----------|------|
| Uncompressed, 40 utilities | 2-4 MB |
| gzip compressed | 800 KB - 1.5 MB |
| Compressed + stripped | 500 KB - 1 MB |

---

## Debugging Initrd Issues

### Kernel Panics: "No init found"

```bash
# Verify /init is in the archive
cpio -t < initrd.img | grep -E '^(\./)?init$'

# Check permissions
cpio -id < initrd.img
ls -la init
file init
```

### Inspecting an Initrd

```bash
# List contents
cpio -t < initrd.img

# Extract to examine
mkdir /tmp/initrd-extract
cd /tmp/initrd-extract
cpio -id < /path/to/initrd.img

# For compressed initrds
gzip -dc initrd.img | cpio -id
```

### Boot Hangs

- Is `/init` a valid ELF binary? (`file my-initrd/bin/init`)
- Does it match the kernel architecture?
- Is `/proc` in the initrd? (needed by `ps`, `kill`)
- Is `/dev` in the initrd? (needed for device operations)

### Debug Boot Parameters

```
init=/bin/sh    # Boot directly to shell
loglevel=7      # Verbose kernel messages
debug           # Enable debug output
```

### Common Errors

| Error | Cause | Fix |
|-------|-------|-----|
| "No init found" | Missing /init | Include init binary in initrd root |
| "Permission denied" | Wrong mode bits | Check permissions in config |
| "Exec format error" | Wrong arch | Rebuild with correct cross-compiler |
| "No such file" | Missing library | Static-link or include library |
| Kernel panic | init crashed | Test init binary on host first |

---

## Summary

- **CPIO newc format** -- simple header-per-file, 4-byte aligned
- **Directory structure** -- standard Unix layout (bin, etc, dev, proc, tmp, usr)
- **Config files** -- `/etc/passwd`, `/etc/group`, `/etc/fstab` for basic operation
- **Always compress** -- use `-z` for production builds
- **Static linking** -- avoids library issues in the minimal environment

The initrd is the heart of Forest OS. Every user interaction starts with the
kernel extracting this archive and running `/init`.

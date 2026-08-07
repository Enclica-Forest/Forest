# Virtual Filesystem (VFS)

The Virtual Filesystem is Forest OS's abstraction layer that lets different filesystems -- ramdisk archives, device nodes, process info, kernel objects -- all live under one unified namespace. It's what makes `/dev/kbd`, `/proc/cpuinfo`, and `/bin/init` feel like they belong to the same tree, even though they come from completely different backends.

Think of VFS as a universal translator. Every filesystem speaks its own dialect (tar headers, device registers, in-memory data structures), but VFS translates them all into a common language of open, read, write, close, readdir, and finddir.

## Architecture Overview

The VFS lives in `vfs.c` and `vfs.h`, with supporting code in `fs.c`, `fs_internal.c`, and each filesystem's own `.c` file. Here's how the pieces fit together:

```
User process
    |
    v
vfs_open() / vfs_read() / vfs_write() / vfs_close()
    |
    v
VFS layer (vfs.c)
    |-- path normalization & resolution
    |-- mount table lookup (find_mount)
    |-- dispatch to filesystem-specific operations
    |
    +--> initrd (ramdisk tar archive)  -- root filesystem
    +--> devfs (device nodes)          -- /dev/*
    +--> procfs (process info)         -- /proc/*
    +--> sysfs (kernel objects)        -- /sys/*
    +--> (future: fat, ext2, tmpfs)    -- other mount points
```

The key insight: VFS doesn't know or care *how* a filesystem stores data. It only cares that each filesystem provides a set of function pointers (read, write, readdir, finddir, etc.) that VFS can call. This is classic polymorphism in C -- a struct of function pointers is essentially a vtable.

## The VFS Node (`vfs_node_t`)

Everything in the VFS is represented as a `vfs_node_t`. Files, directories, device nodes, symlinks -- they're all the same struct with different `flags` and function pointers. Here's the core of it (from `vfs.h:82`):

```c
struct vfs_node {
    char name[128];
    uint32 mask;
    uint32 uid;
    uint32 gid;
    uint32 flags;       // VFS_FILE, VFS_DIRECTORY, VFS_CHARDEVICE, etc.
    uint32 inode;
    uint32 length;      // file size in bytes

    // Device support fields
    uint16 major;       // Major device number (driver class)
    uint16 minor;       // Minor device number (device instance)
    uint32 open_count;  // Reference count for multiple openers

    // Core file operations (function pointers)
    uint32 (*read)(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
    uint32 (*write)(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
    void (*open)(vfs_node_t* node, uint32 flags);
    void (*close)(vfs_node_t* node);
    bool (*readdir)(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent);
    vfs_node_t* (*finddir)(vfs_node_t* node, const char* name);

    // Extended operations
    int (*ioctl)(vfs_node_t* node, uint32 request, void* arg);
    int (*poll)(vfs_node_t* node, uint32 events);

    // Private data -- each filesystem stores its own stuff here
    void* internal_data;
};
```

When you call `vfs_read()`, it simply does `node->read(node, offset, size, buffer)`. If the node came from the ramdisk, that function copies bytes from the tar archive. If it came from devfs, it reads from a device buffer. The caller never knows the difference.

Node type flags tell VFS what kind of object it's dealing with:

| Flag | Value | Meaning |
|------|-------|---------|
| `VFS_FILE` | 0x01 | Regular file |
| `VFS_DIRECTORY` | 0x02 | Directory |
| `VFS_CHARDEVICE` | 0x03 | Character device |
| `VFS_BLOCKDEVICE` | 0x04 | Block device |
| `VFS_PIPE` | 0x05 | Pipe |
| `VFS_SYMLINK` | 0x06 | Symbolic link |
| `VFS_MOUNTPOINT` | 0x08 | Mount point |
| `VFS_DELETED` | 0x10 | Marked for deferred deletion |

## The Directory Entry (`vfs_dirent_t`)

When listing directory contents, VFS uses `vfs_dirent_t` (from `vfs.h:75`):

```c
struct vfs_dirent {
    char name[256];
    uint32 inode;
    uint8 type;     // VFS_FILE or VFS_DIRECTORY
};
```

The `readdir()` function is called with an index (0, 1, 2, ...) and fills in the next entry. Index 0 is always `.`, index 1 is always `..`, and then the actual contents follow. This is how `ls` works -- it just calls `readdir()` in a loop until it returns false.

## Mount Points and Mount Operations

Forest OS uses a mount table (`vfs_mount_t`) to track which filesystem is mounted where. Each mount entry looks like this (from `vfs.h:145`):

```c
struct vfs_mount {
    char mountpoint[256];    // e.g., "/dev", "/proc", "/sys"
    char device[256];        // device name (optional)
    vfs_filesystem_t* fs;    // filesystem type
    void* fs_data;           // filesystem-specific data
    vfs_node_t* root;        // root node of mounted filesystem
    struct vfs_mount* next;  // linked list
};
```

The mount table is a simple linked list. When VFS needs to open a path, it calls `find_mount()` to find the longest-matching mount point. For example, opening `/dev/kbd` matches the `/dev` mount, so VFS delegates to devfs.

### How Mounting Works

1. A filesystem registers itself via `vfs_register_filesystem()`, adding a `vfs_filesystem_t` to the global registry.
2. Something calls `vfs_mount(device, mountpoint, fstype, ...)`.
3. VFS looks up the filesystem type, calls its `mount()` callback to initialize.
4. VFS calls `get_root()` to get the root node of the new filesystem.
5. A new `vfs_mount_t` is created and linked into the mount table.

The filesystem type structure (`vfs_filesystem_t` in `vfs.h:127`) defines the operations each filesystem must implement:

```c
struct vfs_filesystem {
    const char* name;
    uint32 (*probe)(void* dev_data, ...);
    bool (*mount)(void* dev_data, ..., void** sb_out);
    bool (*umount)(void* sb);
    vfs_node_t* (*get_root)(void* sb);
    int (*mkdir)(void* sb, const char* path, uint32 mode);
    int (*rmdir)(void* sb, const char* path);
    int (*unlink)(void* sb, const char* path);
    // ... more operations
};
```

### Deferred Operations

VFS supports *deferred operations* -- when a file is deleted while it still has open handles, the actual deletion is queued and executed later when all handles close. This is similar to how Linux handles `unlink()` on busy files. The deferred op queue (in `vfs.c:939`) processes pending deletes, renames, truncates, and syncs when `vfs_process_deferred_ops()` is called.

## Path Resolution

When you call `vfs_open("/dev/kbd", 0)`, here's what happens (simplified from `vfs.c:486`):

1. **Relative path resolution**: If the path doesn't start with `/`, VFS prepends the current task's `cwd` (current working directory). So `open("config.txt", ...)` becomes `/home/user/config.txt`.

2. **Normalization**: `vfs_normalize_lookup_path()` collapses duplicate slashes, resolves `.` and `..`, and produces a clean canonical path.

3. **Mount dispatch**: `find_mount()` walks the mount table to find which filesystem owns this path. `/dev/kbd` matches the `/dev` mount, so VFS hands off to devfs.

4. **Filesystem lookup**: The filesystem's `finddir()` (or equivalent) is called to locate the actual node.

5. **Node creation**: A `vfs_node_t` is created with the appropriate function pointers wired up.

The ramdisk path resolver (`vfs_find_file_flexible()` in `vfs.c:127`) is particularly clever -- it tries exact matches first, then searches common prefixes like `bin/`, `usr/bin/`, `sbin/`, and even appends `.elf` extensions. This means you can just type `ls` and it'll find `bin/ls.elf`.

## devfs -- The Device Filesystem

`devfs.c` implements the classic UNIX `/dev` directory. Every hardware device gets a node under `/dev` -- keyboards, mice, framebuffers, timers, serial ports, and more.

### Device Registration

Drivers register their devices by calling `devfs_register_device()`:

```c
devfs_register_device("kbd", DEV_TYPE_CHAR,
                      DEV_MAJOR_INPUT, DEV_MINOR_KBD,
                      &g_kbd_evdev_ops, &g_kbd_event_ring);
```

This creates a device node with:
- A name (what appears under `/dev`)
- A type (character or block)
- Major/minor numbers (for device identification)
- A `dev_ops_t` struct with read/write/open/close/ioctl/poll function pointers
- Private driver data

### Major/Minor Numbers

Forest OS uses the traditional UNIX major/minor numbering scheme:

| Major | Purpose |
|-------|---------|
| 1 | Memory devices (`/dev/null`, `/dev/zero`) |
| 4 | TTY devices |
| 5 | Console |
| 8 | SCSI/USB storage |
| 13 | Input devices (keyboard, mouse) |
| 29 | Framebuffer |
| 180 | USB devices |

### How Device Reads Work

When a process reads from `/dev/kbd`:
1. `devfs_open("kbd", ...)` finds the device node in the device list.
2. `devfs_make_device_node()` wraps the device in a VFS node with the device's read/write/ioctl/poll functions.
3. `vfs_read()` calls `node->read()` which calls `kbd_evdev_read()`.
4. `kbd_evdev_read()` pops events from the keyboard ring buffer.

### Input Devices

The input subsystem is particularly well-developed. It supports two interfaces:

- **Legacy**: `/dev/mouse` (ImPS/2 format), `/dev/keyboard` (raw scancodes)
- **evdev-style**: `/dev/kbd`, `/dev/mouse`, `/dev/input/event0`, `/dev/input/event1`

The evdev interface uses `input_event_t` structures and ring buffers for interrupt-safe event passing from hardware IRQ handlers to userspace readers.

### Udev-like Event Queue

devfs maintains a uevent queue (`devfs_uevent_queue_t`) that emits events when devices are added or removed. This is a skeleton of Linux's udev/netlink interface -- userspace can poll `devfs_uevent_pop()` to learn about device hotplug events. Events carry an action (`add`/`remove`), subsystem (`input`/`block`/`tty`), device path, and major/minor numbers.

### Framebuffer Devices

The framebuffer subsystem exposes graphics hardware through several device nodes:

- `/dev/fb_width`, `/dev/fb_height`, `/dev/fb_pitch`, `/dev/fb_bpp` -- display metadata
- `/dev/fb_addr`, `/dev/fb_size` -- framebuffer memory address and size
- `/dev/fb_mmap` -- direct memory-mapped access to framebuffer pixels
- `/dev/fb_double_buffer`, `/dev/fb_swap` -- double-buffering control
- `/dev/cursor_pos`, `/dev/cursor_visible` -- hardware cursor control

Reading `/dev/fb_width` returns the screen width as a 4-byte little-endian integer. Writing to `/dev/fb_mmap` puts pixels directly into video memory. The framebuffer info is cached with a spinlock-protected snapshot to avoid repeated calls to the graphics manager.

## procfs -- The Process Filesystem

`procfs.c` implements `/proc`, the virtual filesystem that exposes process and system information. This is essential for Linux compatibility -- tools like `ps`, `top`, `free`, and `uptime` all read from `/proc`.

### Static Entries

The root of `/proc` contains these information files:

| File | Content |
|------|---------|
| `/proc/cpuinfo` | CPU model, flags, cache size |
| `/proc/meminfo` | Memory usage statistics |
| `/proc/uptime` | System uptime in seconds |
| `/proc/loadavg` | Load averages and process count |
| `/proc/stat` | CPU accounting and process counts |
| `/proc/version` | Kernel version string |
| `/proc/self` | Symlink to current process's `/proc/<pid>` |

### Process Directories

Each running process gets a directory `/proc/<pid>/` containing:

- `status` -- process name, PID, state, UID/GID, memory stats (Linux-compatible format)
- `cmdline` -- command line (NUL-separated, like Linux)
- `maps` -- memory map (text, data, heap, stack, vDSO)
- `environ` -- environment variables
- `fd/` -- directory of file descriptors (symlinks to `/dev/stdin`, `/dev/tty`, etc.)
- `fdinfo/` -- file descriptor metadata (position, flags, inode)

The `/proc/self` symlink always points to the calling process's directory -- so a process can read its own status without knowing its PID.

### Dynamic Process Tracking

procfs maintains a dynamic list (`g_procfs_dynamic[]`) of process entries. When a task is created, `procfs_add_process(pid)` is called; when it exits, `procfs_remove_process(pid)` cleans it up. The `procfs_finddir()` function can also fall back to searching the task list directly if the dynamic list is stale.

### Read Callbacks

Each procfs file has a read callback that generates content on-the-fly. For example, `proc_read_cpuinfo()` formats CPU information using `snprintf()`, and `proc_read_process_maps()` generates a memory map from the task's heap and stack addresses. The data isn't stored on disk -- it's synthesized fresh every time someone reads the file.

### Writable Files

procfs is mostly read-only, but `/proc/tty_options` is writable. Writing `advanced=0` or `blink=1` toggles TTY display options in real time. The parser accepts comma, semicolon, or newline-separated `key=value` pairs.

## sysfs -- The System Filesystem

`sysfs.c` implements `/sys`, which exposes kernel object information in a hierarchical directory structure. This mirrors Linux's sysfs, providing a tree of directories and files that describe the system's hardware and kernel configuration.

### Directory Hierarchy

```
/sys/
  block/         -- block devices (loop0, sda)
  bus/           -- bus types (pci, usb, platform)
  class/         -- device classes (block, input, net, tty)
  dev/           -- device number -> device symlinks
  devices/       -- actual device tree (system/, virtual/, platform/)
  firmware/      -- firmware information
  kernel/        -- kernel parameters (release, version, osrelease)
  module/        -- loaded modules (kernel, vfs, sysfs)
  power/         -- power management
```

Key files include `/sys/kernel/kernel_release` (version string), `/sys/dev/char/*` and `/sys/dev/block/*` (device number to path symlinks), and `/sys/module/` (synthetic module info). sysfs is read-only -- writing is a no-op.

## The Ramdisk (initrd) Filesystem

The ramdisk is Forest OS's root filesystem. It's a tar archive loaded into memory at boot time by the bootloader as a Multiboot module. The ramdisk code (`ramdisk.c`) parses this tar archive and makes its contents available through the VFS.

### Tar Format Parsing

The tar parser (`parse_tar()` in `ramdisk.c:410`) reads POSIX ustar headers -- 512-byte blocks with filename, size (octal), and type flag (`'0'`=file, `'5'`=directory). File data follows each header, padded to 512-byte boundaries. Two consecutive zero blocks signal the end of the archive. The parser concatenates `prefix/` and `filename` fields, strips leading `./`, and points `files[i].data` directly into the tar memory (zero-copy). After parsing, it adds parent directories and virtual directories (`dev`, `proc`, `sys`, `tmp`, etc.).

### The File Table

Parsed files are stored in a dynamically-growing array:

```c
typedef struct {
    const char* name;     // path like "bin/init"
    const uint8* data;    // pointer into tar archive memory
    uint32 size;
    bool is_dir;
} ramdisk_file_t;
```

The VFS root node's `finddir()` and `readdir()` callbacks directly iterate this array. When you `open("/bin/init", ...)`, the VFS calls `ramdisk_find("bin/init")`, which does a linear scan of the file table.

### Path Lookup Flexibility

The ramdisk path resolver is surprisingly smart. If you ask for `shell`, it searches:
1. Exact match: `shell`
2. Common prefixes: `bin/shell`, `usr/bin/shell`, `sbin/shell`, `usr/sbin/shell`
3. With `.elf` extension: `shell.elf`, `bin/shell.elf`, etc.
4. Case-insensitive fallback (scans all entries)

This means the shell can find executables without requiring full paths.

### Virtual Directories

After parsing the tar, the ramdisk adds "virtual" directories that don't exist in the archive but are expected by userspace:

```
dev/        dev/pts/    dev/shm/    proc/
sys/        run/        run/lock/   tmp/
mnt/        media/      var/        var/run/
var/tmp/    var/lock/    var/cache/  var/log/
var/spool/  var/lib/
```

These are empty directories in the file table, but they provide mount points for devfs, procfs, and sysfs.

### Boot Sequence

The bootloader loads the tar archive as a Multiboot module. `ramdisk_init()` finds it (Multiboot1 or Multiboot2, or cached bounds from early memory detection), identity-maps the pages, reserves them in the PMM, and calls `parse_tar()`. Then `vfs_init()` creates the root VFS node wired to the ramdisk's readdir/finddir callbacks, making it the root filesystem at `/`.

## File Operations

### open

`vfs_open(path, flags)` (in `vfs.c:486`) does the heavy lifting:
1. Resolves relative paths against the task's `cwd`.
2. Normalizes the path (collapses `..`, removes duplicate slashes).
3. Checks for special mount points (`/dev`, `/proc`, `/sys`).
4. Falls back to ramdisk lookup with flexible path matching.
5. Creates a `vfs_node_t` and calls the node's `open()` callback.

Open flags include `VFS_READ`, `VFS_WRITE`, `VFS_CREATE`, `VFS_APPEND`, `VFS_TRUNC`, `VFS_NONBLOCK`, and `VFS_EXCL`.

### read

`vfs_read(node, offset, size, buffer)` delegates to `node->read()`. The read function copies data from wherever the filesystem stores it into the caller's buffer and returns the number of bytes actually read. Reading past the end of a file returns 0.

### write

`vfs_write(node, offset, size, buffer)` calls `node->write()`. For the ramdisk, this always returns 0 (read-only). For device files like `/dev/fb_mmap`, writes go directly to hardware memory.

### close

`vfs_close(node)` calls the node's `close()` callback and then frees the node memory. The root node is never freed.

### seek

Forest OS doesn't have an explicit `vfs_seek()` function. Instead, callers pass the desired offset directly to `vfs_read()` and `vfs_write()`. The file descriptor layer (see below) maintains a current position that advances with each read/write.

### ioctl

`vfs_ioctl(node, request, arg)` dispatches to `node->ioctl()` for character and block devices. This is how userspace configures devices -- setting LED states on keyboards, querying mouse resolution, getting framebuffer parameters, or controlling the TTY.

### poll

`vfs_poll(node, events)` checks whether a device is ready for I/O. For input devices, it checks if the ring buffer has events. Regular files are always considered ready. This supports non-blocking I/O and `select()`/`poll()` system calls.

## Directory Operations

### readdir

`vfs_readdir(node, index, dirent)` fills in the `vfs_dirent_t` at position `index`. The convention is:
- Index 0: `.` (current directory)
- Index 1: `..` (parent directory)
- Index 2+: actual entries

The function returns `true` if an entry exists at that index, `false` otherwise. Callers loop from 0 until `false` to enumerate a directory.

### finddir

`vfs_finddir(node, name)` looks up a child by name and returns a new `vfs_node_t` for it. This is how path components are resolved -- `open("/dev/kbd")` calls `finddir` on the `/dev` node with name `"kbd"`.

### mkdir and rmdir

`vfs_mkdir(path, mode)` and `vfs_rmdir(path)` delegate to the owning filesystem's `mkdir()` and `rmdir()` methods. Currently only writable filesystems (like tmpfs, when implemented) support these operations.

## Inodes and Device Numbers

Forest OS uses a simplified inode model. The `inode` field in `vfs_node_t` is a unique identifier within a filesystem, but it's not as严格 as Linux's inode system. For the ramdisk, inodes are just array indices. For procfs, they're computed from the entry type and PID.

Device numbers use the standard UNIX major:minor encoding:

```c
#define MKDEV(major, minor)  (((uint32)(major) << 16) | ((minor) & 0xFFFF))
#define MAJOR(dev)           (((dev) >> 16) & 0xFFFF)
#define MINOR(dev)           ((dev) & 0xFFFF)
```

The `vfs_stat()` function (in `vfs.c:1336`) returns a `vfs_stat` structure with inode, mode, size, and timestamps -- enough for `ls -l` to work.

## File Descriptors

File descriptors in Forest OS are managed at the task level. Each `task_t` (defined in `task.h`) has:

- `tty_fd` -- the task's controlling terminal file descriptor
- `cwd[256]` -- current working directory path

The process info exposed through `/proc/<pid>/fd/` shows:
- FD 0: `/dev/stdin`
- FD 1: `/dev/stdout`
- FD 2: `/dev/stderr`
- FD 3+: `/dev/tty` or anonymous inodes

The `fdinfo` directory provides additional metadata per descriptor: current file position, flags (like `O_LARGEFILE`), mount ID, and inode number.

## Filesystem Registration and Discovery

### Registration

Filesystems register themselves by allocating a `vfs_filesystem_t` and calling `vfs_register_filesystem()`:

```c
vfs_filesystem_t* procfs_fs = kmalloc(sizeof(vfs_filesystem_t));
procfs_fs->name = "proc";
procfs_fs->mount = NULL;           // no block device needed
procfs_fs->get_root = procfs_vfs_get_root;
vfs_register_filesystem(procfs_fs);
```

This adds the filesystem to a global linked list. When `vfs_mount()` is called with a filesystem type name, it walks this list to find the matching `vfs_filesystem_t`.

### Discovery

Currently, filesystems are registered explicitly during kernel initialization:
- `ramdisk_init()` -- parses the tar archive
- `devfs_init()` -- creates the device filesystem
- `procfs_init()` -- registers and mounts procfs at `/proc`
- `sysfs_init()` -- registers and mounts sysfs at `/sys`

There's no automatic probing or block-device-based discovery yet. The `probe()` method in `vfs_filesystem_t` exists but is not called automatically -- it's a hook for future implementation when block devices (disks, USB drives) are supported.

### Build-Time Gating

The build system (`filesystems.mk`) provides granular control over which filesystems are compiled:

```makefile
ENABLE_VFS=yes        # master switch for the entire FS layer
ENABLE_FAT32=yes      # FAT filesystem
ENABLE_EXT2=yes       # ext2 (built into fs.c)
ENABLE_DEVFS=yes      # device filesystem
ENABLE_PROCFS=yes     # process filesystem
ENABLE_SYSFS=yes      # system filesystem
ENABLE_RAMDISK=yes    # initrd tar parser
```

Setting `ENABLE_VFS=no` excludes all filesystem sources from the build. Individual filesystems can be disabled independently.

## The fs.c Layer

There's a secondary abstraction in `fs.c`/`fs.h` designed for traditional on-disk filesystems (FAT32, ext2, ISO9660). It defines `fs_superblock_t`, `fs_inode_t`, and `fs_ops_t` with sector-level I/O operations (`probe`, `mount`, `iget`, `iread`, `readdir`). The VFS layer (`vfs.c`) handles virtual/pseudo filesystems, while `fs.c` is intended for block-device formats. Eventually, `fs.c` filesystems will be registered with VFS to appear in the unified namespace.

## Summary

Forest OS's VFS is a clean, minimal UNIX virtual filesystem implementation:

- **Unified namespace**: different filesystem types coexist under one directory tree
- **Polymorphic dispatch**: function pointers in `vfs_node_t` let each filesystem implement its own behavior
- **Mount table**: dynamic mounting/unmounting at any path, with longest-match lookup
- **Path resolution**: normalization, relative-to-absolute conversion, mount point dispatch
- **Device integration**: major/minor numbers, ioctl, poll for hardware access
- **Virtual filesystems**: procfs, sysfs, devfs provide kernel/userspace interfaces
- **Initrd root**: tar archive parsing with flexible path lookup as the boot filesystem
- **Deferred operations**: safe deletion of files with open handles

No dentry cache, no page cache, no complex locking -- just the essentials to run userspace programs with Linux-compatible interfaces.

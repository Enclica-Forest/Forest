/*
 * sys/mount.h - Mount flags
 *
 * POSIX-compatible mount flag definitions for Fern libc.
 */
#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mount flags (Linux compatible) */
#define MS_RDONLY        1
#define MS_NOSUID        2
#define MS_NODEV         4
#define MS_NOEXEC        8
#define MS_SYNCHRONOUS   16
#define MS_REMOUNT       32
#define MS_MANDLOCK      64
#define MS_DIRSYNC       128
#define MS_NOATIME       1024
#define MS_NODIRATIME    2048
#define MS_BIND          4096
#define MS_MOVE          8192
#define MS_REC           16384
#define MS_VERBOSE       32768
#define MS_SILENT        32768
#define MS_POSIXACL      (1 << 16)
#define MS_UNBINDABLE    (1 << 17)
#define MS_PRIVATE       (1 << 18)
#define MS_SLAVE         (1 << 19)
#define MS_SHARED        (1 << 20)
#define MS_RELATIME      (1 << 21)
#define MS_KERNMOUNT     (1 << 22)
#define MS_I_VERSION     (1 << 23)
#define MS_STRICTATIME   (1 << 24)
#define MS_LAZYTIME      (1 << 25)

/* umount2 flags */
#define MNT_FORCE        1
#define MNT_DETACH       2
#define MNT_EXPIRE       4
#define MNT_NOWAIT       8

/* Mount/umount functions */
int mount(const char *source, const char *target,
          const char *filesystemtype, unsigned long mountflags,
          const void *data);
int umount2(const char *target, int flags);
int umount(const char *target);

/* Legacy name */
#define umount(target) umount2(target, 0)

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MOUNT_H */

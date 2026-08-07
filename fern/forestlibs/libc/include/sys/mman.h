/*
 * sys/mman.h - Memory management declarations
 * 
 * POSIX compatible memory mapping for Fern libc.
 */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

/* Protection flags */
#define PROT_NONE       0x00    /* Page cannot be accessed */
#define PROT_READ       0x01    /* Page can be read */
#define PROT_WRITE      0x02    /* Page can be written */
#define PROT_EXEC       0x04    /* Page can be executed */
#define PROT_GROWSDOWN  0x01000000  /* Extend change to start of growsdown vma */
#define PROT_GROWSUP    0x02000000  /* Extend change to end of growsup vma */

/* Mapping flags */
#define MAP_SHARED      0x01    /* Share changes */
#define MAP_PRIVATE     0x02    /* Changes are private */
#define MAP_TYPE        0x0F    /* Mask for type of mapping */
#define MAP_FIXED       0x10    /* Interpret addr exactly */
#define MAP_ANONYMOUS   0x20    /* Don't use a file */
#define MAP_ANON        MAP_ANONYMOUS   /* Alias */

/* Linux-specific mapping flags */
#define MAP_GROWSDOWN   0x00100 /* Stack-like segment */
#define MAP_DENYWRITE   0x00800 /* ETXTBSY */
#define MAP_EXECUTABLE  0x01000 /* Mark it as an executable */
#define MAP_LOCKED      0x02000 /* Lock the mapping */
#define MAP_NORESERVE   0x04000 /* Don't check for reservations */
#define MAP_POPULATE    0x08000 /* Populate (prefault) pagetables */
#define MAP_NONBLOCK    0x10000 /* Don't block on IO */
#define MAP_STACK       0x20000 /* Allocation is for a stack */
#define MAP_HUGETLB     0x40000 /* Create a huge page mapping */
#define MAP_SYNC        0x80000 /* Perform synchronous page faults for the mapping */
#define MAP_FIXED_NOREPLACE 0x100000 /* MAP_FIXED which doesn't unmap underlying mapping */

/* Return value on error */
#define MAP_FAILED      ((void *)-1)

/* Flags for msync */
#define MS_ASYNC        1       /* Sync memory asynchronously */
#define MS_SYNC         4       /* Synchronous memory sync */
#define MS_INVALIDATE   2       /* Invalidate the caches */

/* Flags for mlockall */
#define MCL_CURRENT     1       /* Lock all currently mapped pages */
#define MCL_FUTURE      2       /* Lock all pages that become mapped */
#define MCL_ONFAULT     4       /* Lock pages when they are faulted in */

/* Flags for madvise */
#define MADV_NORMAL     0       /* No further special treatment */
#define MADV_RANDOM     1       /* Expect random page references */
#define MADV_SEQUENTIAL 2       /* Expect sequential page references */
#define MADV_WILLNEED   3       /* Will need these pages */
#define MADV_DONTNEED   4       /* Don't need these pages */
#define MADV_FREE       8       /* Free pages only if memory pressure */
#define MADV_REMOVE     9       /* Remove these pages & resources */
#define MADV_DONTFORK   10      /* Don't inherit across fork */
#define MADV_DOFORK     11      /* Do inherit across fork */
#define MADV_MERGEABLE  12      /* KSM may merge identical pages */
#define MADV_UNMERGEABLE 13     /* KSM may not merge identical pages */
#define MADV_HUGEPAGE   14      /* Worth backing with hugepages */
#define MADV_NOHUGEPAGE 15      /* Not worth backing with hugepages */
#define MADV_DONTDUMP   16      /* Exclude from a core dump */
#define MADV_DODUMP     17      /* Clear MADV_DONTDUMP */
#define MADV_WIPEONFORK 18      /* Zero memory on fork, child only */
#define MADV_KEEPONFORK 19      /* Undo MADV_WIPEONFORK */
#define MADV_COLD       20      /* Deactivate these pages */
#define MADV_PAGEOUT    21      /* Reclaim these pages */

/* Flags for mremap */
#define MREMAP_MAYMOVE  1       /* Caller can move the mapping */
#define MREMAP_FIXED    2       /* Fifth argument is new address */

/* Memory mapping functions */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t length, int flags);
int madvise(void *addr, size_t length, int advice);
int posix_madvise(void *addr, size_t length, int advice);

/* Memory remapping */
void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);

/* Memory locking */
int mlock(const void *addr, size_t len);
int mlock2(const void *addr, size_t len, unsigned int flags);
int munlock(const void *addr, size_t len);
int mlockall(int flags);
int munlockall(void);

/* Memory residence */
int mincore(void *addr, size_t length, unsigned char *vec);

/* Shared memory */
int shm_open(const char *name, int oflag, mode_t mode);
int shm_unlink(const char *name);

/* POSIX typed memory (not implemented) */
int posix_mem_offset(const void *addr, size_t len, off_t *off,
                     size_t *contig_len, int *fildes);
int posix_typed_mem_get_info(int fildes, struct posix_typed_mem_info *info);
int posix_typed_mem_open(const char *name, int oflag, int tflag);

/* Fern specific framebuffer mapping */
void *mmap_fb(size_t *width, size_t *height, size_t *pitch);
int munmap_fb(void *addr);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */

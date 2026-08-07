/*
 * syscall.c - ARM32 EABI system call dispatcher for Fern
 *
 * Called from swi_handler in exceptions.S with the full CPU register frame:
 *
 *   void syscall_handle(arm_regs_t *regs, uint32_t swi_num)
 *
 * ARM Linux EABI syscall convention:
 *   r7  = syscall number (Linux OABI used r0; EABI uses r7 for compatibility
 *         with Thumb-2 which has no room in the SWI opcode for a number)
 *   r0  = arg0  /  return value (written back via regs->r0)
 *   r1  = arg1
 *   r2  = arg2
 *   r3  = arg3
 *   r4  = arg4  (caller saved across the exception in the frame)
 *   r5  = arg5
 *   r6  = arg6  (rarely used; only a few syscalls need 7 args)
 *
 * The SWI immediate (@swi_num) is always 0 for EABI (Linux compatible)
 * userspace.  It is non-zero only for the legacy OABI ABI and for private
 * Fern kernel-internal calls.
 *
 * Return value:
 *   The C handler stores its return value in regs->r0.  A negative value
 *   in the range [-4095, -1] is interpreted by userspace as an errno code
 *   (POSIX convention; glibc EABI wrapper negates and stores to errno).
 *
 * Error codes use unsigned negation to stay in the C99-defined range for
 *   uint32_t and avoid signed overflow UB:
 *     return (uint32_t)(-(int32_t)ENOSYS)
 *   which equals 0xFFFFFFDA for ENOSYS=38.
 *
 * Fern internal syscall range:
 *   Numbers >= ARM32_NR_FOREST_BASE (0xF000) are Fern private extensions.
 */

#include "arm32.h"

/* =========================================================================
 * Fern private syscall base and numbers
 * ========================================================================= */
#define ARM32_NR_FOREST_BASE    0xF000U
#define ARM32_NR_FOREST_POWER   (ARM32_NR_FOREST_BASE + 0x01U)
#define ARM32_NR_FOREST_NETINFO (ARM32_NR_FOREST_BASE + 0x02U)

/* =========================================================================
 * Forward declarations of the kernel-generic syscall implementations.
 *
 * These live in src/syscall.c (or src/syscall_table.c) and operate on
 * kernel-internal types.  We declare only the subset used by the ARM32 path.
 * All pointer arguments arrive as uint32_t from the 32-bit register file and
 * are cast at the call site.
 * ========================================================================= */
extern int sys_exit(int status)         ARM32_NORETURN;
extern int sys_read(int fd, void *buf, int count);
extern int sys_write(int fd, const void *buf, int count);
extern int sys_open(const char *path, int flags, int mode);
extern int sys_close(int fd);
extern int sys_brk(unsigned long addr);
extern int sys_getpid(void);
extern int sys_fork(void);
extern int sys_execve(const char *path, char *const argv[], char *const envp[]);
extern int sys_waitpid(int pid, int *status, int options);
extern int sys_kill(int pid, int sig);
extern int sys_lseek(int fd, long offset, int whence);
extern int sys_getuid(void);
extern int sys_access(const char *path, int mode);
extern int sys_ioctl(int fd, unsigned long req, unsigned long arg);
extern int sys_mmap(unsigned long addr, unsigned long len,
                    int prot, int flags, int fd, long offset);
extern int sys_munmap(unsigned long addr, unsigned long len);
extern int sys_mprotect(unsigned long addr, unsigned long len, int prot);
extern int sys_nanosleep(const void *req, void *rem);
extern int sys_gettimeofday(void *tv, void *tz);
extern int sys_socket(int domain, int type, int protocol);
extern int sys_bind(int fd, const void *addr, int addrlen);
extern int sys_connect(int fd, const void *addr, int addrlen);
extern int sys_sched_yield(void);

/* Fern extensions */
extern int sys_power(int action);
extern int sys_netinfo(void *buf, int max_entries);

/* =========================================================================
 * Helper: negative errno result as uint32_t
 * ========================================================================= */
static inline uint32_t errno_result(uint32_t err)
{
    /* Returns the two's-complement negation of err as a uint32_t.
     * Equivalent to the C expression (uint32_t)(-(int32_t)err) but written
     * without invoking signed overflow UB. */
    return ~err + 1U;
}

/* =========================================================================
 * syscall_handle
 *
 * Top-level SWI dispatcher.  Called by swi_handler in exceptions.S after
 * the full register context has been saved onto the SVC stack.
 *
 * @regs:    Pointer to saved register frame (struct arm_regs_t).
 *           Modify regs->r0 to set the syscall return value.
 * @swi_num: 24-bit SWI immediate (0 for all EABI/Linux-compat syscalls).
 * ========================================================================= */
void syscall_handle(arm_regs_t *regs, uint32_t swi_num)
{
    /* Extract syscall arguments from the saved register frame.
     * r7 = syscall number (EABI convention).
     * r0..r6 = up to 7 arguments. */
    uint32_t nr   = regs->r7;
    uint32_t arg0 = regs->r0;
    uint32_t arg1 = regs->r1;
    uint32_t arg2 = regs->r2;
    uint32_t arg3 = regs->r3;

    /* r4-r6 are not captured in the lightweight SAVE_CONTEXT macro used in
     * exceptions.S (which pushes r0-r12).  They are already in the frame
     * via the stmfd {r0-r12, lr} – r4 through r12 are all there.
     * Access via the frame struct fields for clarity. */
    uint32_t arg4 = regs->r4;
    uint32_t arg5 = regs->r5;
    uint32_t arg6 = regs->r6;

    /* Suppress unused-variable warnings for rarely-used high args */
    (void)arg4; (void)arg5; (void)arg6;
    (void)swi_num;  /* reserved for OABI / Forest private use below    */

    int32_t  ret = 0;

    /* ------------------------------------------------------------------
     * Handle Fern private syscalls first (swi_num != 0 or nr in
     * the private range).
     * ---------------------------------------------------------------- */
    if (nr >= ARM32_NR_FOREST_BASE) {
        switch (nr) {
        case ARM32_NR_FOREST_POWER:
            ret = sys_power((int)arg0);
            break;
        case ARM32_NR_FOREST_NETINFO:
            ret = sys_netinfo((void *)arg0, (int)arg1);
            break;
        default:
            ret = (int32_t)errno_result(ENOSYS);
            break;
        }
        regs->r0 = (uint32_t)ret;
        return;
    }

    /* ------------------------------------------------------------------
     * Standard Linux ARM EABI syscall table.
     *
     * Only the subset relevant to an early-stage OS is handled inline
     * here.  Unrecognised numbers fall through to the default case which
     * returns -ENOSYS.
     * ---------------------------------------------------------------- */
    switch (nr) {

    /* ---- Process lifecycle ---- */
    case ARM32_NR_exit:
    case ARM32_NR_exit_group:
        sys_exit((int)arg0);
        /* Not reached */
        ret = 0;
        break;

    case ARM32_NR_fork:
        ret = sys_fork();
        break;

    case ARM32_NR_execve:
        ret = sys_execve((const char *)arg0,
                         (char *const *)(uintptr_t)arg1,
                         (char *const *)(uintptr_t)arg2);
        break;

    case ARM32_NR_waitpid:
        ret = sys_waitpid((int)arg0, (int *)(uintptr_t)arg1, (int)arg2);
        break;

    case ARM32_NR_getpid:
        ret = sys_getpid();
        break;

    case ARM32_NR_getuid:
        ret = sys_getuid();
        break;

    case ARM32_NR_kill:
        ret = sys_kill((int)arg0, (int)arg1);
        break;

    case ARM32_NR_sched_yield:
        ret = sys_sched_yield();
        break;

    case ARM32_NR_set_tid_address:
        /* Stub: return current PID as TID (single-threaded kernel). */
        ret = sys_getpid();
        break;

    /* ---- File I/O ---- */
    case ARM32_NR_read:
        ret = sys_read((int)arg0,
                       (void *)(uintptr_t)arg1,
                       (int)arg2);
        break;

    case ARM32_NR_write:
        ret = sys_write((int)arg0,
                        (const void *)(uintptr_t)arg1,
                        (int)arg2);
        break;

    case ARM32_NR_open:
        ret = sys_open((const char *)(uintptr_t)arg0,
                       (int)arg1,
                       (int)arg2);
        break;

    case ARM32_NR_close:
        ret = sys_close((int)arg0);
        break;

    case ARM32_NR_lseek:
        ret = sys_lseek((int)arg0, (long)(int32_t)arg1, (int)arg2);
        break;

    case ARM32_NR_access:
        ret = sys_access((const char *)(uintptr_t)arg0, (int)arg1);
        break;

    case ARM32_NR_ioctl:
        ret = sys_ioctl((int)arg0, (unsigned long)arg1, (unsigned long)arg2);
        break;

    /* ---- Memory management ---- */
    case ARM32_NR_brk:
        ret = sys_brk((unsigned long)arg0);
        break;

    case ARM32_NR_mmap2:
        /* mmap2: offset is in 4 KB pages, not bytes */
        ret = sys_mmap((unsigned long)arg0,
                       (unsigned long)arg1,
                       (int)arg2,
                       (int)arg3,
                       (int)(int32_t)arg4,
                       (long)arg5 * 4096L);
        break;

    case ARM32_NR_munmap:
        ret = sys_munmap((unsigned long)arg0, (unsigned long)arg1);
        break;

    case ARM32_NR_mprotect:
        ret = sys_mprotect((unsigned long)arg0,
                           (unsigned long)arg1,
                           (int)arg2);
        break;

    /* ---- Time ---- */
    case ARM32_NR_nanosleep:
        ret = sys_nanosleep((const void *)(uintptr_t)arg0,
                            (void *)(uintptr_t)arg1);
        break;

    case ARM32_NR_gettimeofday:
        ret = sys_gettimeofday((void *)(uintptr_t)arg0,
                               (void *)(uintptr_t)arg1);
        break;

    /* ---- Networking ---- */
    case ARM32_NR_socket:
        ret = sys_socket((int)arg0, (int)arg1, (int)arg2);
        break;

    case ARM32_NR_bind:
        ret = sys_bind((int)arg0,
                       (const void *)(uintptr_t)arg1,
                       (int)arg2);
        break;

    case ARM32_NR_connect:
        ret = sys_connect((int)arg0,
                          (const void *)(uintptr_t)arg1,
                          (int)arg2);
        break;

    /* ---- Unknown ---- */
    default:
        /* Return -ENOSYS as per POSIX / Linux convention. */
        ret = (int32_t)errno_result(ENOSYS);
        break;
    }

    /* Write return value back into r0 of the saved frame so that
     * RESTORE_CONTEXT_AND_RETURN in exceptions.S delivers it to userspace. */
    regs->r0 = (uint32_t)ret;
}

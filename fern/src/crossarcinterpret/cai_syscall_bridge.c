/*
 * cai_syscall_bridge.c - Syscall translation for the cross-architecture interpreter
 *
 * Design
 * ------
 * Linux guest programs call the kernel via:
 *   i386    : INT 0x80   nr=eax  a0=ebx a1=ecx a2=edx a3=esi a4=edi a5=ebp  ret→eax
 *   x86-64  : SYSCALL    nr=rax  a0=rdi a1=rsi a2=rdx a3=r10 a4=r8  a5=r9   ret→rax
 *   ARM32   : SWI #0     nr=r7   a0=r0  a1=r1  a2=r2  a3=r3  a4=r4  a5=r5   ret→r0
 *   AArch64 : SVC #0     nr=x8   a0=x0  a1=x1  a2=x2  a3=x3  a4=x4  a5=x5   ret→x0
 *
 * Two entry points are provided:
 *
 *  cai_syscall_dispatch(ctx, syscall_nr)
 *    Primary entry point used by the arch-specific step functions.  Reads
 *    arguments from the correct guest registers inside cai_context_t, translates
 *    pointer args through the context's region table, dispatches to Fern
 *    sys_* functions, and returns the result.
 *
 *  cai_syscall_dispatch_ex(arch, args, guest_as)
 *    Standalone version used by higher-level callers and unit tests.  Accepts
 *    a pre-populated cai_syscall_args_t and a cai_address_space_t; does not
 *    require a cai_context_t.  Internally both paths share the same dispatch
 *    table via an internal helper do_dispatch().
 *
 * Pointer translation
 * -------------------
 * Guest programs pass guest virtual addresses as syscall arguments.  The bridge
 * translates these to host kernel addresses via:
 *   - cai_mem_gva_to_host()  when working with a cai_context_t
 *   - cai_as_translate()     when working with a cai_address_space_t
 *
 * Error values
 * ------------
 * Negative returns follow the Fern convention: -ERRNO.
 */

#include "crossarcinterpret.h"
#include "cai_syscall_bridge.h"
#include "cai_memory.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../syscall_table.h"

/* =========================================================================
 * errno constants not in errno_defs.h
 * ========================================================================= */

#ifndef EFAULT
#define EFAULT   14
#endif
#ifndef ENOSYS
#define ENOSYS   38
#endif
#ifndef EBADF
#define EBADF     9
#endif
#ifndef EACCES
#define EACCES   13
#endif

/* =========================================================================
 * Guest Linux syscall numbers per ABI
 * ========================================================================= */

/* --- i386 (INT 0x80) --- */
#define LX_I386_exit       1
#define LX_I386_read       3
#define LX_I386_write      4
#define LX_I386_open       5
#define LX_I386_close      6
#define LX_I386_lseek      19
#define LX_I386_getpid     20
#define LX_I386_brk        45
#define LX_I386_mmap       90
#define LX_I386_munmap     91
#define LX_I386_stat       106
#define LX_I386_fstat      108
#define LX_I386_uname      122
#define LX_I386_mprotect   125
#define LX_I386_readv      145
#define LX_I386_writev     146
#define LX_I386_nanosleep  162
#define LX_I386_exit_group 252

/* --- x86-64 (SYSCALL) --- */
#define LX_X64_read        0
#define LX_X64_write       1
#define LX_X64_open        2
#define LX_X64_close       3
#define LX_X64_stat        4
#define LX_X64_fstat       5
#define LX_X64_lseek       8
#define LX_X64_mmap        9
#define LX_X64_mprotect    10
#define LX_X64_munmap      11
#define LX_X64_brk         12
#define LX_X64_readv       19
#define LX_X64_writev      20
#define LX_X64_nanosleep   35
#define LX_X64_getpid      39
#define LX_X64_uname       63
#define LX_X64_exit        60
#define LX_X64_exit_group  231

/* --- ARM32 (SWI #0) --- */
#define LX_ARM32_exit       1
#define LX_ARM32_read       3
#define LX_ARM32_write      4
#define LX_ARM32_open       5
#define LX_ARM32_close      6
#define LX_ARM32_lseek      19
#define LX_ARM32_getpid     20
#define LX_ARM32_brk        45
#define LX_ARM32_mmap       90
#define LX_ARM32_munmap     91
#define LX_ARM32_stat       106
#define LX_ARM32_fstat      108
#define LX_ARM32_uname      122
#define LX_ARM32_mprotect   125
#define LX_ARM32_readv      145
#define LX_ARM32_writev     146
#define LX_ARM32_nanosleep  162
#define LX_ARM32_exit_group 248

/* --- AArch64 (SVC #0) --- */
#define LX_AA64_read        63
#define LX_AA64_write       64
#define LX_AA64_openat     1024   /* AArch64 uses openat; treat dirfd=AT_FDCWD */
#define LX_AA64_close       57
#define LX_AA64_fstat       80
#define LX_AA64_lseek       62
#define LX_AA64_mmap        222
#define LX_AA64_mprotect    226
#define LX_AA64_munmap      215
#define LX_AA64_brk         214
#define LX_AA64_readv        65
#define LX_AA64_writev       66
#define LX_AA64_nanosleep   101
#define LX_AA64_getpid      172
#define LX_AA64_uname       160
#define LX_AA64_exit         93
#define LX_AA64_exit_group   94

/* =========================================================================
 * Argument extraction helpers (fill cai_syscall_args_t from guest registers)
 * ========================================================================= */

static void extract_args_x86_32(cai_context_t *ctx, cai_syscall_args_t *a)
{
    a->num     = (int64_t)ctx->cpu.x86_32.eax;
    a->args[0] = (int64_t)ctx->cpu.x86_32.ebx;
    a->args[1] = (int64_t)ctx->cpu.x86_32.ecx;
    a->args[2] = (int64_t)ctx->cpu.x86_32.edx;
    a->args[3] = (int64_t)ctx->cpu.x86_32.esi;
    a->args[4] = (int64_t)ctx->cpu.x86_32.edi;
    a->args[5] = (int64_t)ctx->cpu.x86_32.ebp;
}

static void extract_args_x86_64(cai_context_t *ctx, cai_syscall_args_t *a)
{
    a->num     = (int64_t)ctx->cpu.x86_64.rax;
    a->args[0] = (int64_t)ctx->cpu.x86_64.rdi;
    a->args[1] = (int64_t)ctx->cpu.x86_64.rsi;
    a->args[2] = (int64_t)ctx->cpu.x86_64.rdx;
    a->args[3] = (int64_t)ctx->cpu.x86_64.r10;
    a->args[4] = (int64_t)ctx->cpu.x86_64.r8;
    a->args[5] = (int64_t)ctx->cpu.x86_64.r9;
}

static void extract_args_arm32(cai_context_t *ctx, cai_syscall_args_t *a)
{
    a->num     = (int64_t)ctx->cpu.arm32.r[7];
    a->args[0] = (int64_t)ctx->cpu.arm32.r[0];
    a->args[1] = (int64_t)ctx->cpu.arm32.r[1];
    a->args[2] = (int64_t)ctx->cpu.arm32.r[2];
    a->args[3] = (int64_t)ctx->cpu.arm32.r[3];
    a->args[4] = (int64_t)ctx->cpu.arm32.r[4];
    a->args[5] = (int64_t)ctx->cpu.arm32.r[5];
}

static void extract_args_aarch64(cai_context_t *ctx, cai_syscall_args_t *a)
{
    a->num     = (int64_t)ctx->cpu.aarch64.x[8];
    a->args[0] = (int64_t)ctx->cpu.aarch64.x[0];
    a->args[1] = (int64_t)ctx->cpu.aarch64.x[1];
    a->args[2] = (int64_t)ctx->cpu.aarch64.x[2];
    a->args[3] = (int64_t)ctx->cpu.aarch64.x[3];
    a->args[4] = (int64_t)ctx->cpu.aarch64.x[4];
    a->args[5] = (int64_t)ctx->cpu.aarch64.x[5];
}

/* =========================================================================
 * cai_syscall_args_from_regs (public helper)
 * ========================================================================= */

void cai_syscall_args_from_regs(cai_guest_arch_t arch,
                                 const uint64_t regs[7],
                                 cai_syscall_args_t *out)
{
    if (!out)
        return;
    out->num     = (int64_t)regs[0];
    out->args[0] = (int64_t)regs[1];
    out->args[1] = (int64_t)regs[2];
    out->args[2] = (int64_t)regs[3];
    out->args[3] = (int64_t)regs[4];
    out->args[4] = (int64_t)regs[5];
    out->args[5] = (int64_t)regs[6];
    (void)arch;
}

/* =========================================================================
 * Guest pointer translation
 *
 * Two variants: one that uses cai_mem_gva_to_host (context-based), and one
 * that uses cai_as_translate (standalone address-space-based).
 * Both are used by do_dispatch() depending on which is non-NULL.
 * ========================================================================= */

static void *xlat_ctx(cai_context_t *ctx, uint64_t gva, size_t len,
                      uint32_t flags)
{
    return (void *)cai_mem_gva_to_host(ctx, gva, len, flags);
}

static void *xlat_as(cai_address_space_t *as, uint64_t gva, size_t len)
{
    if (!as) return NULL;
    return cai_as_translate(as, gva, len);
}

/* Unified helper: prefer ctx if available, otherwise fall back to as. */
static void *xlat(cai_context_t *ctx, cai_address_space_t *as,
                  uint64_t gva, size_t len, uint32_t flags)
{
    if (ctx)
        return xlat_ctx(ctx, gva, len, flags);
    return xlat_as(as, gva, len);
}

/* =========================================================================
 * Shared syscall body: do_dispatch
 *
 * Both cai_syscall_dispatch and cai_syscall_dispatch_ex converge here.
 * @ctx and @as may each be NULL but not both at once (ctx takes priority for
 * pointer translation).
 * ========================================================================= */

static int64_t do_dispatch(cai_arch_t arch,
                            const cai_syscall_args_t *a,
                            cai_context_t *ctx,
                            cai_address_space_t *as)
{
    uint64_t nr  = (uint64_t)a->num;
    uint64_t a0  = (uint64_t)a->args[0];
    uint64_t a1  = (uint64_t)a->args[1];
    uint64_t a2  = (uint64_t)a->args[2];
    uint64_t a3  = (uint64_t)a->args[3];
    uint64_t a4  = (uint64_t)a->args[4];
    uint64_t a5  = (uint64_t)a->args[5];

    debuglog(DEBUG_DETAIL,
             "cai_bridge: arch=%d nr=%llu a0=0x%llx a1=0x%llx a2=0x%llx\n",
             (int)arch,
             (unsigned long long)nr,
             (unsigned long long)a0,
             (unsigned long long)a1,
             (unsigned long long)a2);

    switch (arch) {

    /* ================================================================
     * i386 / x86-32
     * ============================================================== */
    case CAI_ARCH_X86_32:
        switch ((uint32_t)nr) {

        case LX_I386_exit:
        case LX_I386_exit_group:
            if (ctx) {
                ctx->running   = false;
                ctx->exit_code = (int)(int32_t)a0;
            }
            return (int64_t)(int32_t)a0;

        case LX_I386_read: {
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_read((uint32_t)a0,
                                      (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_I386_write: {
            if (a2 == 0) return 0;
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_READ);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_write((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_I386_open: {
            void *hpath = xlat(ctx, as, a0, 1, CAI_MEM_READ);
            if (!hpath) return (int64_t)(-EFAULT);
            return (int64_t)sys_open((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)a1, (uint32_t)a2);
        }

        case LX_I386_close:
            return (int64_t)sys_close((uint32_t)a0);

        case LX_I386_lseek:
            return (int64_t)sys_lseek((uint32_t)a0, (int32_t)a1, (uint32_t)a2);

        case LX_I386_brk:
            return (int64_t)sys_brk((uint32_t)a0);

        case LX_I386_mmap:
            return (int64_t)sys_mmap((uint32_t)a0, (uint32_t)a1,
                                      (uint32_t)a2, (uint32_t)a3,
                                      (uint32_t)a4, (uint32_t)a5);

        case LX_I386_munmap:
            return (int64_t)sys_munmap((uint32_t)a0, (uint32_t)a1);

        case LX_I386_mprotect:
            return (int64_t)sys_mprotect((uint32_t)a0, (uint32_t)a1,
                                          (uint32_t)a2);

        case LX_I386_getpid:
            return (int64_t)sys_getpid();

        case LX_I386_stat: {
            void *hpath = xlat(ctx, as, a0, 1, CAI_MEM_READ);
            void *hbuf  = xlat(ctx, as, a1, 64, CAI_MEM_WRITE);
            if (!hpath || !hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_stat((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)(uintptr_t)hbuf);
        }

        case LX_I386_fstat: {
            void *hbuf = xlat(ctx, as, a1, 64, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_fstat((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf);
        }

        case LX_I386_uname: {
            void *hbuf = xlat(ctx, as, a0, 390, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_uname((uint32_t)(uintptr_t)hbuf);
        }

        case LX_I386_nanosleep: {
            void *hreq = xlat(ctx, as, a0, 8, CAI_MEM_READ);
            void *hrem = a1 ? xlat(ctx, as, a1, 8, CAI_MEM_WRITE) : NULL;
            if (!hreq) return (int64_t)(-EFAULT);
            return (int64_t)sys_nanosleep((uint32_t)(uintptr_t)hreq,
                                           (uint32_t)(uintptr_t)hrem);
        }

        case LX_I386_writev: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 8, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_writev((uint32_t)a0,
                                        (uint32_t)(uintptr_t)hiov,
                                        (uint32_t)a2);
        }

        case LX_I386_readv: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 8, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_readv((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hiov,
                                       (uint32_t)a2);
        }

        default:
            debuglog(DEBUG_WARN,
                     "cai_bridge: unimplemented i386 syscall %llu\n",
                     (unsigned long long)nr);
            return (int64_t)(-ENOSYS);
        }

    /* ================================================================
     * x86-64
     * ============================================================== */
    case CAI_ARCH_X86_64:
        switch (nr) {

        case LX_X64_exit:
        case LX_X64_exit_group:
            if (ctx) {
                ctx->running   = false;
                ctx->exit_code = (int)(int32_t)a0;
            }
            return (int64_t)(int32_t)a0;

        case LX_X64_read: {
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_read((uint32_t)a0,
                                      (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_X64_write: {
            if (a2 == 0) return 0;
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_READ);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_write((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_X64_open: {
            void *hpath = xlat(ctx, as, a0, 1, CAI_MEM_READ);
            if (!hpath) return (int64_t)(-EFAULT);
            return (int64_t)sys_open((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)a1, (uint32_t)a2);
        }

        case LX_X64_close:
            return (int64_t)sys_close((uint32_t)a0);

        case LX_X64_lseek:
            return (int64_t)sys_lseek((uint32_t)a0, (int32_t)a1, (uint32_t)a2);

        case LX_X64_brk:
            return (int64_t)sys_brk((uint32_t)a0);

        case LX_X64_mmap:
            return (int64_t)sys_mmap((uint32_t)a0, (uint32_t)a1,
                                      (uint32_t)a2, (uint32_t)a3,
                                      (uint32_t)a4, (uint32_t)a5);

        case LX_X64_munmap:
            return (int64_t)sys_munmap((uint32_t)a0, (uint32_t)a1);

        case LX_X64_mprotect:
            return (int64_t)sys_mprotect((uint32_t)a0, (uint32_t)a1,
                                          (uint32_t)a2);

        case LX_X64_getpid:
            return (int64_t)sys_getpid();

        case LX_X64_stat: {
            void *hpath = xlat(ctx, as, a0, 1, CAI_MEM_READ);
            void *hbuf  = xlat(ctx, as, a1, 144, CAI_MEM_WRITE);
            if (!hpath || !hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_stat((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)(uintptr_t)hbuf);
        }

        case LX_X64_fstat: {
            void *hbuf = xlat(ctx, as, a1, 144, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_fstat((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf);
        }

        case LX_X64_uname: {
            void *hbuf = xlat(ctx, as, a0, 390, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_uname((uint32_t)(uintptr_t)hbuf);
        }

        case LX_X64_nanosleep: {
            void *hreq = xlat(ctx, as, a0, 16, CAI_MEM_READ);
            void *hrem = a1 ? xlat(ctx, as, a1, 16, CAI_MEM_WRITE) : NULL;
            if (!hreq) return (int64_t)(-EFAULT);
            return (int64_t)sys_nanosleep((uint32_t)(uintptr_t)hreq,
                                           (uint32_t)(uintptr_t)hrem);
        }

        case LX_X64_writev: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 16, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_writev((uint32_t)a0,
                                        (uint32_t)(uintptr_t)hiov,
                                        (uint32_t)a2);
        }

        case LX_X64_readv: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 16, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_readv((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hiov,
                                       (uint32_t)a2);
        }

        default:
            debuglog(DEBUG_WARN,
                     "cai_bridge: unimplemented x86-64 syscall %llu\n",
                     (unsigned long long)nr);
            return (int64_t)(-ENOSYS);
        }

    /* ================================================================
     * ARM32
     * ============================================================== */
    case CAI_ARCH_ARM32:
        switch ((uint32_t)nr) {

        case LX_ARM32_exit:
        case LX_ARM32_exit_group:
            if (ctx) {
                ctx->running   = false;
                ctx->exit_code = (int)(int32_t)a0;
            }
            return (int64_t)(int32_t)a0;

        case LX_ARM32_read: {
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_read((uint32_t)a0,
                                      (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_ARM32_write: {
            if (a2 == 0) return 0;
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_READ);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_write((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_ARM32_open: {
            void *hpath = xlat(ctx, as, a0, 1, CAI_MEM_READ);
            if (!hpath) return (int64_t)(-EFAULT);
            return (int64_t)sys_open((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)a1, (uint32_t)a2);
        }

        case LX_ARM32_close:
            return (int64_t)sys_close((uint32_t)a0);

        case LX_ARM32_lseek:
            return (int64_t)sys_lseek((uint32_t)a0, (int32_t)a1, (uint32_t)a2);

        case LX_ARM32_brk:
            return (int64_t)sys_brk((uint32_t)a0);

        case LX_ARM32_mmap:
            return (int64_t)sys_mmap((uint32_t)a0, (uint32_t)a1,
                                      (uint32_t)a2, (uint32_t)a3,
                                      (uint32_t)a4, (uint32_t)a5);

        case LX_ARM32_munmap:
            return (int64_t)sys_munmap((uint32_t)a0, (uint32_t)a1);

        case LX_ARM32_mprotect:
            return (int64_t)sys_mprotect((uint32_t)a0, (uint32_t)a1,
                                          (uint32_t)a2);

        case LX_ARM32_getpid:
            return (int64_t)sys_getpid();

        case LX_ARM32_stat: {
            void *hpath = xlat(ctx, as, a0, 1, CAI_MEM_READ);
            void *hbuf  = xlat(ctx, as, a1, 64, CAI_MEM_WRITE);
            if (!hpath || !hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_stat((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)(uintptr_t)hbuf);
        }

        case LX_ARM32_fstat: {
            void *hbuf = xlat(ctx, as, a1, 64, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_fstat((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf);
        }

        case LX_ARM32_uname: {
            void *hbuf = xlat(ctx, as, a0, 390, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_uname((uint32_t)(uintptr_t)hbuf);
        }

        case LX_ARM32_nanosleep: {
            void *hreq = xlat(ctx, as, a0, 8, CAI_MEM_READ);
            void *hrem = a1 ? xlat(ctx, as, a1, 8, CAI_MEM_WRITE) : NULL;
            if (!hreq) return (int64_t)(-EFAULT);
            return (int64_t)sys_nanosleep((uint32_t)(uintptr_t)hreq,
                                           (uint32_t)(uintptr_t)hrem);
        }

        case LX_ARM32_writev: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 8, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_writev((uint32_t)a0,
                                        (uint32_t)(uintptr_t)hiov,
                                        (uint32_t)a2);
        }

        case LX_ARM32_readv: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 8, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_readv((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hiov,
                                       (uint32_t)a2);
        }

        default:
            debuglog(DEBUG_WARN,
                     "cai_bridge: unimplemented ARM32 syscall %llu\n",
                     (unsigned long long)nr);
            return (int64_t)(-ENOSYS);
        }

    /* ================================================================
     * AArch64
     * ============================================================== */
    case CAI_ARCH_AARCH64:
        switch (nr) {

        case LX_AA64_exit:
        case LX_AA64_exit_group:
            if (ctx) {
                ctx->running   = false;
                ctx->exit_code = (int)(int32_t)a0;
            }
            return (int64_t)(int32_t)a0;

        case LX_AA64_read: {
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_read((uint32_t)a0,
                                      (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_AA64_write: {
            if (a2 == 0) return 0;
            void *hbuf = xlat(ctx, as, a1, (size_t)a2, CAI_MEM_READ);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_write((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf, (uint32_t)a2);
        }

        case LX_AA64_openat: {
            /* openat(dirfd, path, flags, mode) – ignore dirfd, use path */
            void *hpath = xlat(ctx, as, a1, 1, CAI_MEM_READ);
            if (!hpath) return (int64_t)(-EFAULT);
            return (int64_t)sys_open((uint32_t)(uintptr_t)hpath,
                                      (uint32_t)a2, (uint32_t)a3);
        }

        case LX_AA64_close:
            return (int64_t)sys_close((uint32_t)a0);

        case LX_AA64_lseek:
            return (int64_t)sys_lseek((uint32_t)a0, (int32_t)a1, (uint32_t)a2);

        case LX_AA64_brk:
            return (int64_t)sys_brk((uint32_t)a0);

        case LX_AA64_mmap:
            return (int64_t)sys_mmap((uint32_t)a0, (uint32_t)a1,
                                      (uint32_t)a2, (uint32_t)a3,
                                      (uint32_t)a4, (uint32_t)a5);

        case LX_AA64_munmap:
            return (int64_t)sys_munmap((uint32_t)a0, (uint32_t)a1);

        case LX_AA64_mprotect:
            return (int64_t)sys_mprotect((uint32_t)a0, (uint32_t)a1,
                                          (uint32_t)a2);

        case LX_AA64_getpid:
            return (int64_t)sys_getpid();

        case LX_AA64_fstat: {
            void *hbuf = xlat(ctx, as, a1, 144, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_fstat((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hbuf);
        }

        case LX_AA64_uname: {
            void *hbuf = xlat(ctx, as, a0, 390, CAI_MEM_WRITE);
            if (!hbuf) return (int64_t)(-EFAULT);
            return (int64_t)sys_uname((uint32_t)(uintptr_t)hbuf);
        }

        case LX_AA64_nanosleep: {
            void *hreq = xlat(ctx, as, a0, 16, CAI_MEM_READ);
            void *hrem = a1 ? xlat(ctx, as, a1, 16, CAI_MEM_WRITE) : NULL;
            if (!hreq) return (int64_t)(-EFAULT);
            return (int64_t)sys_nanosleep((uint32_t)(uintptr_t)hreq,
                                           (uint32_t)(uintptr_t)hrem);
        }

        case LX_AA64_writev: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 16, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_writev((uint32_t)a0,
                                        (uint32_t)(uintptr_t)hiov,
                                        (uint32_t)a2);
        }

        case LX_AA64_readv: {
            void *hiov = xlat(ctx, as, a1, (size_t)a2 * 16, CAI_MEM_READ);
            if (!hiov) return (int64_t)(-EFAULT);
            return (int64_t)sys_readv((uint32_t)a0,
                                       (uint32_t)(uintptr_t)hiov,
                                       (uint32_t)a2);
        }

        default:
            debuglog(DEBUG_WARN,
                     "cai_bridge: unimplemented AArch64 syscall %llu\n",
                     (unsigned long long)nr);
            return (int64_t)(-ENOSYS);
        }

    default:
        debuglog(DEBUG_ERROR, "cai_bridge: unknown arch %d\n", (int)arch);
        return (int64_t)(-EINVAL);
    }
}

/* =========================================================================
 * Public entry point 1: context-based (called by arch step functions)
 *
 * Signature declared in crossarcinterpret.h:
 *   int64_t cai_syscall_dispatch(cai_context_t *ctx, uint64_t syscall_nr);
 *
 * The @syscall_nr parameter is present for historical compatibility; the
 * actual number is always re-read from the appropriate guest register inside
 * do_dispatch (via extract_args_*).
 * ========================================================================= */

int64_t cai_syscall_dispatch(cai_context_t *ctx, uint64_t syscall_nr)
{
    if (!ctx)
        return (int64_t)(-EINVAL);

    cai_syscall_args_t a;

    switch (ctx->target_arch) {
    case CAI_ARCH_X86_32:  extract_args_x86_32 (ctx, &a); break;
    case CAI_ARCH_X86_64:  extract_args_x86_64 (ctx, &a); break;
    case CAI_ARCH_ARM32:   extract_args_arm32  (ctx, &a); break;
    case CAI_ARCH_AARCH64: extract_args_aarch64(ctx, &a); break;
    default:
        return (int64_t)(-EINVAL);
    }

    /* Override with the parameter for any callers that pass it explicitly */
    if (syscall_nr != 0)
        a.num = (int64_t)syscall_nr;

    return do_dispatch(ctx->target_arch, &a, ctx, NULL);
}

/* =========================================================================
 * Public entry point 2: standalone (called by high-level code / tests)
 * ========================================================================= */

int64_t cai_syscall_dispatch_ex(cai_guest_arch_t arch,
                                 const cai_syscall_args_t *args,
                                 cai_address_space_t *guest_as)
{
    if (!args)
        return (int64_t)(-EINVAL);
    return do_dispatch((cai_arch_t)arch, args, NULL, guest_as);
}

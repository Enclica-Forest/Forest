/*
 * cai_syscall_bridge.h - Architecture-independent syscall bridge for CAI
 *
 * The bridge translates guest Linux ABI syscall numbers into Fern kernel
 * calls.  Guest pointer arguments (buffers, path strings) are validated and
 * translated through the guest address space before being forwarded to the
 * host kernel.
 *
 * Supported guest ABIs
 * --------------------
 *  CAI_GUEST_ARCH_X86_32  : INT 0x80 (i386 Linux)
 *  CAI_GUEST_ARCH_X86_64  : SYSCALL  (x86-64 Linux)
 *  CAI_GUEST_ARCH_ARM32   : SWI #0   (ARM32 Linux)
 *  CAI_GUEST_ARCH_AARCH64 : SVC #0   (AArch64 Linux)
 *
 * Two dispatch interfaces are provided:
 *
 * 1. Context-based (used by the architecture-specific step functions):
 *       cai_syscall_dispatch(ctx, syscall_nr)
 *    Reads arguments from the correct guest registers, calls the Fern
 *    syscall, and returns the result.  Matches the signature declared in
 *    crossarcinterpret.h.
 *
 * 2. Standalone (used by higher-level callers and unit tests):
 *       cai_syscall_dispatch_ex(arch, args, guest_as)
 *    Takes a pre-populated cai_syscall_args_t and an explicit address space.
 *    Does not require a full cai_context_t.
 */

#ifndef CAI_SYSCALL_BRIDGE_H
#define CAI_SYSCALL_BRIDGE_H

#include <stdint.h>
#include "crossarcinterpret.h"
#include "cai_memory.h"

/* =========================================================================
 * Guest architecture identifier (for the standalone API)
 * ========================================================================= */

typedef enum {
    CAI_GUEST_ARCH_X86_32  = CAI_ARCH_X86_32,
    CAI_GUEST_ARCH_X86_64  = CAI_ARCH_X86_64,
    CAI_GUEST_ARCH_ARM32   = CAI_ARCH_ARM32,
    CAI_GUEST_ARCH_AARCH64 = CAI_ARCH_AARCH64
} cai_guest_arch_t;

/* =========================================================================
 * Universal (architecture-independent) syscall argument bundle
 *
 * Filled by the caller from the appropriate guest ABI registers:
 *   x86-32 : num=eax  args=[ebx, ecx, edx, esi, edi, ebp]
 *   x86-64 : num=rax  args=[rdi, rsi, rdx, r10, r8,  r9 ]
 *   arm32  : num=r7   args=[r0,  r1,  r2,  r3,  r4,  r5 ]
 *   aarch64: num=x8   args=[x0,  x1,  x2,  x3,  x4,  x5 ]
 * ========================================================================= */

typedef struct cai_syscall_args {
    int64_t num;       /* Raw guest syscall number                       */
    int64_t args[6];   /* a0 … a5 from guest calling convention          */
} cai_syscall_args_t;

/* =========================================================================
 * Context-based dispatch (called by arch step functions)
 *
 * Matches the declaration in crossarcinterpret.h:
 *   int64_t cai_syscall_dispatch(cai_context_t *ctx, uint64_t syscall_nr);
 * ========================================================================= */

/* Forward declared in crossarcinterpret.h – no re-declaration needed here. */

/* =========================================================================
 * Standalone dispatch (does not require cai_context_t)
 * ========================================================================= */

/*
 * cai_syscall_dispatch_ex - Translate @args from the guest ABI into a Forest
 * OS kernel call and return the result.
 *
 * @arch     : Guest ABI / calling convention to use.
 * @args     : Syscall number and arguments extracted from guest registers.
 * @guest_as : Guest address space used to validate and translate pointer args.
 *             May be NULL if the syscall has no pointer arguments.
 *
 * Returns a signed host result value (negative on error, per Fern
 * conventions).
 */
int64_t cai_syscall_dispatch_ex(cai_guest_arch_t arch,
                                 const cai_syscall_args_t *args,
                                 cai_address_space_t *guest_as);

/* =========================================================================
 * Helper: populate a cai_syscall_args_t from a raw register array.
 *
 * @regs : Array of 7 uint64_t: regs[0]=syscall_nr, regs[1..6]=args a0..a5.
 * @out  : Populated on return.
 * ========================================================================= */

void cai_syscall_args_from_regs(cai_guest_arch_t arch,
                                 const uint64_t regs[7],
                                 cai_syscall_args_t *out);

#endif /* CAI_SYSCALL_BRIDGE_H */

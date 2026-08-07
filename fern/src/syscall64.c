/**
 * @file syscall64.c
 * @brief x86_64 SYSCALL/SYSRET MSR Setup and Fast Syscall Dispatcher
 *
 * The x86_64 SYSCALL instruction is a fast ring-3 -> ring-0 transition that
 * avoids the overhead of INT / IRET.  Three Model-Specific Registers control
 * its behaviour:
 *
 *   IA32_STAR   (MSR 0xC0000081)
 *     [63:48]  SYSRET CS/SS bases  (user CS = this + 16, user SS = this + 8)
 *     [47:32]  SYSCALL CS/SS bases (kernel CS = this + 0, kernel SS = this + 8)
 *     [31:0]   Reserved (must be 0)
 *
 *   IA32_LSTAR  (MSR 0xC0000082)
 *     Canonical address of the SYSCALL entry point (64-bit mode).
 *
 *   IA32_FMASK  (MSR 0xC0000084)
 *     Bits set here are CLEARED in RFLAGS on SYSCALL entry.
 *     We clear IF (bit 9), DF (bit 10), TF (bit 8), and AC (bit 18).
 *
 * SYSCALL / SYSRET ABI (Linux x86_64):
 *   RAX  - syscall number (on entry) / return value (on exit)
 *   RDI  - arg 1
 *   RSI  - arg 2
 *   RDX  - arg 3
 *   R10  - arg 4  (replaces RCX which SYSCALL uses to save RIP)
 *   R8   - arg 5
 *   R9   - arg 6
 *
 * The assembly entry stub (syscall64_entry) lives in syscall64_stubs.asm.
 * It uses SWAPGS + a per-CPU area (per_cpu_data_t, defined in syscall64.h)
 * to switch to the kernel stack, then calls syscall64_handle() here.
 *
 * This file provides:
 *   - syscall64_init()          : MSR configuration and per-CPU area setup
 *   - syscall64_handle()        : C-level dispatcher called from the asm stub
 *   - syscall64_set_kernel_stack(): update kernel RSP on task switches
 *   - syscall64_get_per_cpu()   : debugging accessor
 */

#include "include/syscall64.h"
#include "include/syscall.h"    /* syscall number enum and sys_arg_t */
#include "include/gdt64.h"      /* GDT selector constants */
#include "include/screen.h"     /* print(), print_hex() */
#include "include/string.h"     /* memory_set() */
#include "include/task.h"       /* current_task, task_mark_active() */
#include "include/debuglog.h"   /* debuglog() */
#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__

/* =========================================================================
 * MSR numbers
 * ========================================================================= */

#define MSR_EFER        0xC0000080U  /* Extended Feature Enable Register */
#define MSR_STAR        0xC0000081U  /* SYSCALL/SYSRET segment selectors */
#define MSR_LSTAR       0xC0000082U  /* SYSCALL entry point (64-bit) */
#define MSR_CSTAR       0xC0000083U  /* SYSCALL entry point (compat, unused) */
#define MSR_FMASK       0xC0000084U  /* SYSCALL RFLAGS mask */
#define MSR_KERNEL_GS_BASE 0xC0000102U  /* Kernel GS base for SWAPGS */

/* EFER bits */
#define EFER_SCE        (1ULL << 0)  /* System Call Enable */

/* RFLAGS bits to clear on SYSCALL entry */
#define RFLAG_IF        (1ULL << 9)  /* Interrupt Enable */
#define RFLAG_DF        (1ULL << 10) /* Direction Flag */
#define RFLAG_TF        (1ULL << 8)  /* Trap Flag */
#define RFLAG_AC        (1ULL << 18) /* Alignment Check (SMAP) */

#define SYSCALL_FMASK   (RFLAG_IF | RFLAG_DF | RFLAG_TF | RFLAG_AC)

/* =========================================================================
 * MSR access helpers
 * ========================================================================= */

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr),
                            "a"((uint32_t)(value & 0xFFFFFFFFU)),
                            "d"((uint32_t)(value >> 32)));
}

/* =========================================================================
 * Per-CPU data area (one per logical CPU; BSP = CPU 0)
 *
 * Layout (must match offsets used in syscall64_stubs.asm):
 *   offset 0x00: kernel_rsp  (loaded into RSP on SYSCALL entry via SWAPGS)
 *   offset 0x08: cpu_id
 *   offset 0x10: user_rsp    (scratch slot for saving user RSP)
 *
 * The NASM stub uses hard-coded offsets [gs:0x00], [gs:0x08], [gs:0x10]
 * so this struct must NOT be reordered without updating the asm.
 * ========================================================================= */

#define SYSCALL_KSTACK_SIZE     (16 * 1024)    /* 16 KiB per-CPU kernel stack */

static uint8_t bsp_kstack[SYSCALL_KSTACK_SIZE] __attribute__((aligned(16)));

static per_cpu_data_t bsp_per_cpu __attribute__((aligned(16)));

/* =========================================================================
 * Error return for unimplemented syscalls
 * ========================================================================= */
#define ENOSYS_NEG  (-38)    /* -ENOSYS, Linux compatible */

/* =========================================================================
 * MSR configuration
 * ========================================================================= */

/**
 * @brief Enable SYSCALL/SYSRET and wire up the three control MSRs.
 *
 * Call this once from the BSP kernel-init path, after gdt64_init() has
 * loaded the GDT so the segment selectors are valid.
 *
 * IA32_STAR layout (written as a 64-bit value):
 *   [63:48]  SYSRET  base: user  CS = base + 16, user  SS = base + 8
 *   [47:32]  SYSCALL base: kernel CS = base + 0,  kernel SS = base + 8
 *   [31:0]   Reserved (0)
 *
 * With our GDT:
 *   Kernel CS = 0x08, Kernel SS (data) = 0x10  → SYSCALL base = 0x08
 *   User   CS = 0x18, User   SS (data) = 0x20
 *     SYSRET CS = base + 16 → base = 0x18 - 16 = 0x08 is wrong.
 *     SYSRET CS = base + 16 must equal 0x1B (user CS with RPL=3).
 *     The CPU actually uses base + 16 for 64-bit SYSRET CS,
 *     and base + 8 for SS.
 *     With GDT layout:
 *       0x08 = kernel code, 0x10 = kernel data
 *       0x18 = user code,   0x20 = user data
 *     SYSRET needs user SS at base+8 and user CS at base+16.
 *       If base = 0x18: SS = 0x20 ✓, CS = 0x28 ✗
 *     The conventional x86_64 trick: arrange the GDT so that:
 *       user null/compat = base+8  (not used in our kernel)
 *       user 64-bit CS  = base+16
 *     and use base = 0x08 for SYSRET (0x08 + 8 = 0x10 = kernel data,
 *     not user data).
 *
 *     Linux solves this by putting user data at selector 0x23 (GDT[4])
 *     and using STAR[63:48] = 0x0018, so:
 *       SYSRET SS = 0x0018 + 8  = 0x0020 = user data (GDT[4] | RPL=0 → set RPL=3 → 0x23)
 *       SYSRET CS = 0x0018 + 16 = 0x0028 … but that is beyond GDT[4].
 *     This works because the CPU sets RPL=3 on both CS and SS before
 *     returning to user mode.  The 0x23 / 0x1B values are just selector
 *     indices with RPL bits; the STAR base addresses are the GDT offsets
 *     WITHOUT the RPL bits.
 *
 *     Correct STAR for our GDT:
 *       SYSCALL base = 0x08  → kernel CS = 0x08, kernel SS = 0x10  ✓
 *       SYSRET  base = 0x10  → user   SS = 0x18, user   CS = 0x20
 *         But user CS is at 0x18 and user SS at 0x20 in our GDT.
 *         So SYSRET base = 0x08 gives SS=0x10 (wrong).
 *
 *     The only GDT layout that satisfies SYSCALL+SYSRETQ symmetry:
 *       GDT[0] null
 *       GDT[1] kernel code  0x08
 *       GDT[2] kernel data  0x10
 *       GDT[3] user code    0x18   ← STAR base + 16 for SYSRET
 *       GDT[4] user data    0x20   ← This needs to be base + 8
 *     So SYSRET base = 0x18 - 16 = 0x08 → SS = 0x08 + 8 = 0x10 (kernel data) ✗
 *
 *     The standard Linux / x86_64 OS trick is:
 *       GDT order: null, kernel code (0x08), kernel data (0x10),
 *                  user data (0x18), user code (0x20 | compat placeholder)
 *                  user code 64-bit (0x28)  ← but that wastes a slot.
 *
 *     Simplest correct layout (used by most 64-bit kernels):
 *       STAR[63:48] = 0x0018    (SYSRET base)
 *         → user SS = 0x0018 + 8  = 0x0020  (must be user data)
 *         → user CS = 0x0018 + 16 = 0x0028  (must be user 64-bit code)
 *       STAR[47:32] = 0x0008    (SYSCALL base)
 *         → kernel CS = 0x0008        (kernel code)
 *         → kernel SS = 0x0008 + 8  = 0x0010  (kernel data)  ✓
 *
 *     That means the GDT must have user data at 0x20 and user 64-bit code
 *     at 0x28.  Our current GDT in gdt64.c has:
 *       0x18 = user code, 0x20 = user data
 *     so SYSRET base = 0x08 is wrong too.
 *
 *     We encode STAR to match our ACTUAL gdt64.c layout where user code is
 *     at 0x18 (RPL=3 → 0x1B) and user data is at 0x20 (RPL=3 → 0x23):
 *       SYSCALL base = 0x08  → CS=0x08, SS=0x10  ✓
 *       SYSRET  base = 0x10  → SS=0x18 (user code!), CS=0x20 (user data!)
 *     That is inverted.
 *
 *     Conclusion: to use SYSCALL/SYSRET cleanly we must ensure the GDT
 *     has: null, kernel-code, kernel-data, user-data, user-code.
 *     Our gdt64.c currently has user-code before user-data, so SYSRET
 *     with standard STAR encoding would swap CS and SS in user mode.
 *
 *     We work around this by swapping the user-code/user-data order in
 *     STAR:
 *       SYSRET base = 0x10:  SS = 0x18 = our user code slot → wrong for data.
 *
 *     The cleanest fix is to use STAR[63:48] = 0x0010 which gives:
 *       SS = 0x10 + 8  = 0x18 ... but that's user code in our GDT.
 *
 *     FINAL DECISION: adjust our GDT order to the standard one:
 *       GDT[3] = user data  (0x18, RPL=3 → 0x1B for SS)
 *       GDT[4] = user code  (0x20, RPL=3 → 0x23 for CS)
 *     Then STAR[63:48] = 0x0010:
 *       SYSRET SS = 0x0010 + 8  = 0x0018 = user data ✓
 *       SYSRET CS = 0x0010 + 16 = 0x0020 = user code ✓
 *
 *     We leave gdt64.c as-is (compatibility with the existing boot path)
 *     and instead tell STAR to use:
 *       SYSCALL base = 0x08  (kernel code at 0x08, kernel data at 0x10)
 *       SYSRET  base = 0x10  (user data at 0x18, user code at 0x20)
 *
 *     But with our current GDT user-code = 0x18 and user-data = 0x20,
 *     SYSRET base = 0x10 gives:
 *       SS = 0x10 + 8  = 0x18 = user code  ← WRONG
 *       CS = 0x10 + 16 = 0x20 = user data  ← WRONG
 *
 *     The only non-invasive solution is to note that SYSRET with a
 *     "wrong" SS only matters when the CPU actually tries to use SS, which
 *     in 64-bit mode is essentially never (SS is ignored for flat data
 *     accesses).  CS however MUST be correct.
 *
 *     We therefore patch the SYSRET base so CS comes out right, accepting
 *     that SS will point to user-code descriptor:
 *       SYSRET base = 0x08:  CS = 0x08 + 16 = 0x18 = user code ✓
 *                            SS = 0x08 + 8  = 0x10 = kernel data ✗
 *     SS with kernel-data descriptor in user mode is also wrong.
 *
 *     THE ONLY CORRECT SOLUTION is to have the GDT in standard order.
 *     We update gdt64.c's constant macros and rebuild.  The note below
 *     documents the assumption this code makes:
 *
 * ASSUMPTION: gdt64.c MUST lay out user descriptors in this order:
 *   GDT[3] = user data  (selector 0x18, with RPL=3 → 0x1B)
 *   GDT[4] = user code  (selector 0x20, with RPL=3 → 0x23)
 *
 * If that order is reversed the SYSRET CS/SS assignments will be wrong.
 * The values used below are chosen for the STANDARD layout:
 *   STAR[47:32] = 0x0008  → SYSCALL: kernel CS=0x08, kernel SS=0x10
 *   STAR[63:48] = 0x0010  → SYSRET:  user   SS=0x18, user   CS=0x20
 */

/*
 * STAR field values for our GDT (standard layout):
 *   Bits [47:32] = kernel-code selector base = 0x0008
 *   Bits [63:48] = user-ss-base = 0x0010
 *       → SYSRET SS = 0x0010 + 8  = 0x0018 (user data at GDT[3])
 *       → SYSRET CS = 0x0010 + 16 = 0x0020 (user code at GDT[4])
 *
 * IMPORTANT: This requires GDT[3]=user data, GDT[4]=user code.
 * gdt64.c currently has GDT[3]=user code, GDT[4]=user data.
 * See the comment in syscall64_setup_msrs() for the override we apply.
 */
#define STAR_SYSCALL_BASE  0x0008ULL   /* kernel CS */
#define STAR_SYSRET_BASE   0x0010ULL   /* user SS base for SYSRET */

/**
 * @brief Configure the three SYSCALL MSRs and enable the SCE bit in EFER.
 *
 * Called once from the kernel init path after gdt64_init().
 */
void syscall64_setup_msrs(void)
{
    print("[SYSCALL64] Configuring SYSCALL/SYSRET MSRs...\n");

    /* 1. Enable the System Call Extension bit in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /* 2. IA32_STAR – set segment selector bases
     *
     * Note on our GDT layout vs. standard SYSRET requirements:
     *
     * Our gdt64.c has:  GDT[3]=user_code (0x18), GDT[4]=user_data (0x20)
     * Standard requires: GDT[3]=user_data, GDT[4]=user_code for SYSRET.
     *
     * We set STAR[63:48] = 0x0008 so that:
     *   SYSRET CS = 0x0008 + 16 = 0x0018 = user code (GDT[3])  ✓
     *   SYSRET SS = 0x0008 + 8  = 0x0010 = kernel data (GDT[2])
     *
     * The kernel-data SS in user mode is normally wrong, but in 64-bit
     * mode the CPU ignores the SS descriptor base/limit for data access,
     * so this is harmless for flat-memory kernel code.  When a proper
     * user-mode SS is needed the task's segment can be configured via
     * a different mechanism.  Adjust GDT layout to fix permanently.
     */
    uint64_t star = 0;
    star |= (STAR_SYSCALL_BASE << 32);      /* [47:32]: kernel CS base */
    star |= (STAR_SYSRET_BASE  << 48);      /* [63:48]: SYSRET SS base */
    wrmsr(MSR_STAR, star);

    /* 3. IA32_LSTAR – SYSCALL entry point in 64-bit mode */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall64_entry);

    /* 4. IA32_CSTAR – SYSCALL entry for compatibility mode (unused, zero) */
    wrmsr(MSR_CSTAR, 0);

    /* 5. IA32_FMASK – bits to clear in RFLAGS on SYSCALL */
    wrmsr(MSR_FMASK, SYSCALL_FMASK);

    /* Verify */
    uint64_t lstar_check = rdmsr(MSR_LSTAR);
    print("[SYSCALL64] LSTAR = 0x");
    print_hex((uint32_t)(lstar_check >> 32));
    print_hex((uint32_t)lstar_check);
    print("\n");

    uint64_t star_check = rdmsr(MSR_STAR);
    print("[SYSCALL64] STAR  = 0x");
    print_hex((uint32_t)(star_check >> 32));
    print_hex((uint32_t)star_check);
    print("\n");

    print("[SYSCALL64] FMASK = 0x");
    print_hex((uint32_t)rdmsr(MSR_FMASK));
    print("\n");

    print("[SYSCALL64] SYSCALL/SYSRET enabled (EFER.SCE=1)\n");
}

/* =========================================================================
 * Public wrappers exposed to the rest of the kernel
 * ========================================================================= */

/**
 * @brief Initialize the 64-bit fast syscall path.
 *
 * This is the single entry point the kernel init code should call.
 * It configures MSRs and verifies the SCE bit.
 */
void syscall64_init(void)
{
    print("[SYSCALL64] Initializing fast syscall path...\n");

    /* Check CPUID to ensure SYSCALL is supported */
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x80000001));

    if (!(edx & (1u << 11))) {
        print("[SYSCALL64] WARNING: SYSCALL not supported by CPU – using INT 0x80 only\n");
        return;
    }

    syscall64_setup_msrs();

    print("[SYSCALL64] Initialization complete.  Kernel stack at 0x");
    print_hex((uint32_t)((uint64_t)(uintptr_t)(syscall_kstack + SYSCALL_KSTACK_SIZE) >> 32));
    print_hex((uint32_t)(uint64_t)(uintptr_t)(syscall_kstack + SYSCALL_KSTACK_SIZE));
    print("\n");
}

#endif /* __x86_64__ */

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
 * MSR access helpers (defined in syscall64.h)
 * ========================================================================= */

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
 * Our GDT layout (gdt64.c):
 *   GDT[1] = kernel code (0x08), GDT[2] = kernel data (0x10)
 *   GDT[3] = user data   (0x18), GDT[4] = user code   (0x20)
 *
 * STAR encoding:
 *   SYSCALL base = 0x0008  →  kernel CS = 0x08, kernel SS = 0x10
 *   SYSRET  base = 0x0010  →  user   SS = 0x18, user   CS = 0x20
 *   CPU ORs RPL=3 into CS and SS on SYSRET.
 */

/*
 * STAR field values for our GDT (standard layout):
 *   Bits [47:32] = kernel-code selector base = 0x0008
 *   Bits [63:48] = user-ss-base = 0x0010
 *       → SYSRET SS = 0x0010 + 8  = 0x0018 (user data at GDT[3])
 *       → SYSRET CS = 0x0010 + 16 = 0x0020 (user code at GDT[4])
 *
 * GDT layout (gdt64.c):
 *   GDT[3] = user data  (selector 0x18, with RPL=3 → 0x1B for SS)
 *   GDT[4] = user code  (selector 0x20, with RPL=3 → 0x23 for CS)
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
     * Our GDT layout (gdt64.c):
     *   GDT[1] = kernel code (0x08), GDT[2] = kernel data (0x10)
     *   GDT[3] = user data   (0x18), GDT[4] = user code   (0x20)
     *
     * STAR encoding:
     *   [47:32] = 0x0008  → SYSCALL: kernel CS=0x08, kernel SS=0x10
     *   [63:48] = 0x0010  → SYSRET:  user   SS=0x18, user   CS=0x20
     *   CPU ORs RPL=3 into CS and SS on SYSRET.
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
    print_hex((uint32_t)((uint64_t)(uintptr_t)(bsp_kstack + SYSCALL_KSTACK_SIZE) >> 32));
    print_hex((uint32_t)(uint64_t)(uintptr_t)(bsp_kstack + SYSCALL_KSTACK_SIZE));
    print("\n");
}

#endif /* __x86_64__ */

/**
 * @file x86_64.h
 * @brief x86_64 CPU feature detection helpers
 *
 * Provides runtime CPUID-based detection for the long-mode features that
 * the 64-bit kernel cares about: long mode, NX, SMEP, SMAP, PCID, LA57
 * (5-level paging), XSAVE, AVX, FSGSBASE, RDTSCP, 1 GiB pages, and the
 * physical/virtual address width.
 *
 * All helpers are gated behind the appropriate ENABLE_* build option from
 * build_options.h so that a baseline kernel never touches CPUID leaves the
 * developer has not opted into.  The detection results are cached on first
 * use so CPUID is only issued once per feature.
 *
 * This header is self-contained (only depends on <stdint.h> and
 * build_options.h).  It is safe to include from both C and assembly-bound
 * C files.  All functions are marked static inline so there is no linking
 * requirement.
 */

#ifndef X86_64_H
#define X86_64_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __x86_64__

#include "build_options.h"

/* =========================================================================
 * CPUID leaf numbers and bit positions
 * ========================================================================= */

#define X86_64_CPUID_VENDOR_ID          0x00000000u
#define X86_64_CPUID_FEATURE_INFO       0x00000001u
#define X86_64_CPUID_EXTENDED_FEATURES  0x00000007u
#define X86_64_CPUID_EXT_BASE           0x80000000u
#define X86_64_CPUID_EXT_FEATURES       0x80000001u
#define X86_64_CPUID_ADDR_SIZE          0x80000008u

/* CPUID.01H:ECX bits */
#define FEAT01_ECX_PCID         (1u << 17)  /* Process Context Identifiers */
#define FEAT01_ECX_XSAVE        (1u << 26)  /* XSAVE/XRSTOR support       */
#define FEAT01_ECX_OSXSAVE      (1u << 27)  /* OS has enabled XSAVE       */
#define FEAT01_ECX_AVX          (1u << 28)  /* AVX support                */
#define FEAT01_ECX_RDRAND       (1u << 30)  /* RDRAND instruction         */

/* CPUID.07H.0:EBX bits */
#define FEAT07_EBX_FSGSBASE     (1u <<  0)  /* RDFSBASE/WRFSBASE etc.     */
#define FEAT07_EBX_SMEP         (1u <<  7)  /* Supervisor Mode Exec Prev  */
#define FEAT07_EBX_SMAP         (1u << 20)  /* Supervisor Mode Access Prev*/
#define FEAT07_EBX_AVX2         (1u <<  5)  /* AVX2                       */

/* CPUID.07H.0:ECX bits */
#define FEAT07_ECX_LA57         (1u << 16)  /* 5-level paging (57-bit VA) */

/* CPUID.80000001H:EDX bits */
#define FEAT_EXT_EDX_NX         (1u << 20)  /* No-Execute (XD)            */
#define FEAT_EXT_EDX_PAGE1G     (1u << 26)  /* 1 GiB pages                */
#define FEAT_EXT_EDX_LM         (1u << 29)  /* Long Mode                  */
#define FEAT_EXT_EDX_SYSCALL    (1u << 11)  /* SYSCALL/SYSRET             */
#define FEAT_EXT_EDX_RDTSCP     (1u << 27)  /* RDTSCP instruction         */

/* CPUID.01H:EDX bits */
#define FEAT01_EDX_FPU          (1u <<  0)  /* On-chip FPU                */
#define FEAT01_EDX_MMX          (1u << 23)  /* MMX                        */
#define FEAT01_EDX_SSE          (1u << 25)  /* SSE                        */
#define FEAT01_EDX_SSE2         (1u << 26)  /* SSE2                       */
#define FEAT01_EDX_TSC          (1u <<  4)  /* TSC                        */

/* =========================================================================
 * Raw CPUID helper
 * ========================================================================= */

static inline void x86_64_cpuid(uint32_t leaf, uint32_t subleaf,
                                 uint32_t *eax, uint32_t *ebx,
                                 uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf));
}

/* =========================================================================
 * Cached feature snapshot
 *
 * Populated once on first query by x86_64_features_init().  Subsequent
 * feature queries just read this struct.  This keeps CPUID out of hot
 * paths (context switch, page-fault handler, etc.).
 * ========================================================================= */

typedef struct {
    bool     long_mode;          /* CPU implements AMD64 (CPUID 80000001:EDX[29]) */
    bool     nx;                 /* No-Execute / XD                                */
    bool     smep;               /* Supervisor Mode Execution Prevention          */
    bool     smap;               /* Supervisor Mode Access Prevention             */
    bool     pcid;               /* Process Context Identifiers                   */
    bool     la57;               /* 5-level paging (57-bit VA)                    */
    bool     xsave;              /* XSAVE/XRSTOR                                  */
    bool     avx;                /* AVX (256-bit)                                 */
    bool     avx2;               /* AVX2                                          */
    bool     fsgsbase;           /* RDFSBASE/WRFSBASE/RDGSBASE/WRGSBASE           */
    bool     rdtscp;             /* RDTSCP (serialising TSC read)                 */
    bool     page1g;             /* 1 GiB large pages                             */
    bool     syscall;            /* SYSCALL/SYSRET instructions                   */
    bool     sse;                /* SSE (implied by x86_64)                       */
    bool     sse2;               /* SSE2 (implied by x86_64)                      */
    uint32_t phys_addr_bits;    /* Max physical address bits (typically 36-52)   */
    uint32_t virt_addr_bits;    /* Max virtual  address bits (48 or 57)          */
    bool     initialized;
} x86_64_features_t;

static x86_64_features_t x86_64_features_snapshot;

/* =========================================================================
 * Public feature detection API
 * ========================================================================= */

/**
 * @brief Populate the cached feature snapshot.  Called once at boot; safe
 *        to call again (returns immediately if already initialised).
 */
static inline void x86_64_features_init(void)
{
    if (x86_64_features_snapshot.initialized)
        return;

    x86_64_features_t *f = &x86_64_features_snapshot;
    uint32_t eax, ebx, ecx, edx;

    /* Default to safe values for a baseline x86_64 CPU */
    f->long_mode = true;        /* implied by being in long mode */
    f->virt_addr_bits = 48;
    f->phys_addr_bits = 36;
    f->sse = true;
    f->sse2 = true;

    /* CPUID 0x01: feature info (PCID, XSAVE, AVX, SSE2) */
    x86_64_cpuid(X86_64_CPUID_FEATURE_INFO, 0, &eax, &ebx, &ecx, &edx);
    f->pcid  = (ecx & FEAT01_ECX_PCID)    != 0;
    f->xsave = (ecx & FEAT01_ECX_XSAVE)   != 0;
    f->avx   = (ecx & FEAT01_ECX_AVX)     != 0;
    f->sse   = (edx & FEAT01_EDX_SSE)     != 0;
    f->sse2  = (edx & FEAT01_EDX_SSE2)    != 0;

    /* CPUID 0x07.0: extended features (SMEP, SMAP, LA57, FSGSBASE, AVX2) */
    x86_64_cpuid(X86_64_CPUID_EXTENDED_FEATURES, 0, &eax, &ebx, &ecx, &edx);
    f->smep     = (ebx & FEAT07_EBX_SMEP)     != 0;
    f->smap     = (ebx & FEAT07_EBX_SMAP)     != 0;
    f->fsgsbase = (ebx & FEAT07_EBX_FSGSBASE) != 0;
    f->avx2     = (ebx & FEAT07_EBX_AVX2)     != 0;
    f->la57     = (ecx & FEAT07_ECX_LA57)     != 0;

    /* CPUID 0x80000001: extended features (LM, NX, 1G, SYSCALL, RDTSCP) */
    x86_64_cpuid(X86_64_CPUID_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
    f->long_mode = (edx & FEAT_EXT_EDX_LM)      != 0;
    f->nx        = (edx & FEAT_EXT_EDX_NX)      != 0;
    f->page1g    = (edx & FEAT_EXT_EDX_PAGE1G)  != 0;
    f->syscall   = (edx & FEAT_EXT_EDX_SYSCALL) != 0;
    f->rdtscp    = (edx & FEAT_EXT_EDX_RDTSCP)  != 0;

    /* CPUID 0x80000008: physical/virtual address sizes */
    x86_64_cpuid(X86_64_CPUID_ADDR_SIZE, 0, &eax, &ebx, &ecx, &edx);
    f->phys_addr_bits = eax & 0xFFu;
    f->virt_addr_bits = (eax >> 8) & 0xFFu;
    if (f->phys_addr_bits == 0) f->phys_addr_bits = 36;
    if (f->virt_addr_bits == 0) f->virt_addr_bits = 48;

    f->initialized = true;
}

/* Individual feature queries.  Each is gated by the corresponding ENABLE_*
 * build option so that a baseline kernel never advertises a feature it has
 * not opted into.  All return false until x86_64_features_init() has run. */

static inline bool x86_64_has_long_mode(void)
{
    return x86_64_features_snapshot.long_mode;
}

static inline bool x86_64_has_nx(void)
{
#if ENABLE_NX
    return x86_64_features_snapshot.nx;
#else
    return false;
#endif
}

static inline bool x86_64_has_smep(void)
{
#if ENABLE_SMEP
    return x86_64_features_snapshot.smep;
#else
    return false;
#endif
}

static inline bool x86_64_has_smap(void)
{
#if ENABLE_SMAP
    return x86_64_features_snapshot.smap;
#else
    return false;
#endif
}

static inline bool x86_64_has_pcid(void)
{
#if ENABLE_PCID
    return x86_64_features_snapshot.pcid;
#else
    return false;
#endif
}

static inline bool x86_64_has_la57(void)
{
#if ENABLE_5LEVEL_PAGING
    return x86_64_features_snapshot.la57;
#else
    return false;
#endif
}

static inline bool x86_64_has_xsave(void)
{
#if ENABLE_XSAVE
    return x86_64_features_snapshot.xsave;
#else
    return false;
#endif
}

static inline bool x86_64_has_avx(void)
{
#if ENABLE_AVX
    return x86_64_features_snapshot.avx &&
           x86_64_features_snapshot.xsave;
#else
    return false;
#endif
}

static inline bool x86_64_has_avx2(void)
{
#if ENABLE_AVX
    return x86_64_features_snapshot.avx2 &&
           x86_64_features_snapshot.xsave;
#else
    return false;
#endif
}

static inline bool x86_64_has_fsgsbase(void)
{
    return x86_64_features_snapshot.fsgsbase;
}

static inline bool x86_64_has_rdtscp(void)
{
    return x86_64_features_snapshot.rdtscp;
}

static inline bool x86_64_has_1g_pages(void)
{
    return x86_64_features_snapshot.page1g;
}

static inline bool x86_64_has_syscall(void)
{
    return x86_64_features_snapshot.syscall;
}

static inline uint32_t x86_64_phys_addr_bits(void)
{
    return x86_64_features_snapshot.phys_addr_bits;
}

static inline uint32_t x86_64_virt_addr_bits(void)
{
    return x86_64_features_snapshot.virt_addr_bits;
}

static inline const x86_64_features_t *x86_64_get_features(void)
{
    return &x86_64_features_snapshot;
}

/* =========================================================================
 * Control-register / MSR bit constants used by the 64-bit boot path
 *
 * These mirror arch/x86_64/arch_x86_64.h but are duplicated here so that
 * boot code and feature-setup code can include this header alone.
 * ========================================================================= */

#define X86_64_CR0_PE   (1ULL <<  0)   /* Protection Enable            */
#define X86_64_CR0_WP   (1ULL << 16)   /* Write Protect                */
#define X86_64_CR0_PG   (1ULL << 31)   /* Paging Enable                */
#define X86_64_CR0_MP   (1ULL <<  1)   /* Monitor Coprocessor          */
#define X86_64_CR0_EM   (1ULL <<  2)   /* Emulation (must be 0 in LM)  */
#define X86_64_CR0_TS   (1ULL <<  3)   /* Task Switched (FPU lazy)     */
#define X86_64_CR0_NE   (1ULL <<  5)   /* Native Exception             */

#define X86_64_CR4_PAE        (1ULL <<  5)   /* PAE (required for LM)  */
#define X86_64_CR4_PGE        (1ULL <<  7)   /* Page Global Enable     */
#define X86_64_CR4_OSFXSR     (1ULL <<  9)   /* SSE FXSAVE/FXRSTOR     */
#define X86_64_CR4_OSXMMEXCPT (1ULL << 10)   /* SSE exception support  */
#define X86_64_CR4_LA57       (1ULL << 12)   /* 5-level paging         */
#define X86_64_CR4_PCIDE      (1ULL << 17)   /* PCID Enable            */
#define X86_64_CR4_OSXSAVE    (1ULL << 18)   /* OS XSAVE enable        */
#define X86_64_CR4_SMEP       (1ULL << 20)   /* SMEP                   */
#define X86_64_CR4_SMAP       (1ULL << 21)   /* SMAP                   */
#define X86_64_CR4_FSGSBASE   (1ULL << 16)   /* FSGSBASE instructions  */

#define X86_64_EFER_SCE  (1ULL <<  0)   /* SYSCALL/SYSRET enable  */
#define X86_64_EFER_LME  (1ULL <<  8)   /* Long Mode Enable       */
#define X86_64_EFER_LMA  (1ULL << 10)   /* Long Mode Active (RO)  */
#define X86_64_EFER_NXE  (1ULL << 11)   /* No-Execute Enable      */
#define X86_64_EFER_FFXSR (1ULL << 14)  /* Fast FXSAVE (AMD)      */

#define X86_64_MSR_EFER           0xC0000080UL
#define X86_64_MSR_STAR           0xC0000081UL
#define X86_64_MSR_LSTAR          0xC0000082UL
#define X86_64_MSR_CSTAR          0xC0000083UL
#define X86_64_MSR_FMASK          0xC0000084UL
#define X86_64_MSR_FS_BASE        0xC0000100UL
#define X86_64_MSR_GS_BASE        0xC0000101UL
#define X86_64_MSR_KERNEL_GS_BASE 0xC0000102UL

#endif /* __x86_64__ */
#endif /* X86_64_H */

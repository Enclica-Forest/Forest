/*
 * Fern - x86-64 (AMD64) Architecture Header
 * arch_x86_64.h
 *
 * Defines structures, macros, and inline helpers for the x86-64 long-mode
 * architecture.  Included automatically by arch.h when ARCH_X86_64 == 1.
 * Do NOT include this file directly.
 *
 * Covers:
 *   - 64-bit GDT / IDT / TSS
 *   - Extended GP registers (rax-r15) and rip/rflags
 *   - RFLAGS bit definitions
 *   - Control registers (CR0/CR3/CR4)
 *   - MSR access (rdmsr / wrmsr inlines)
 *   - SYSCALL / SYSRET support macros
 *   - FS.base / GS.base / SWAPGS manipulation
 *   - PML4 / PDP / PD / PT 4-level page table types
 *   - CPUID
 *   - TSC
 */

#ifndef FOREST_ARCH_X86_64_H
#define FOREST_ARCH_X86_64_H

#ifndef FOREST_ARCH_H
#error "Include arch.h, not arch_x86_64.h directly."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * GDT - Global Descriptor Table (64-bit long mode)
 *
 * In long mode most segment limits/bases are ignored; only DPL, P, and
 * the L (long-mode code) / D bits matter.  The TSS descriptor is 16 bytes.
 * ========================================================================= */

/* Standard Fern 64-bit selectors */
#define X86_64_SEG_NULL      0x00
#define X86_64_SEG_KCODE     0x08   /* RPL=0, 64-bit code  */
#define X86_64_SEG_KDATA     0x10   /* RPL=0, data (64-bit data segs are flat) */
#define X86_64_SEG_UDATA     0x18   /* RPL=3, user data     (SYSRET base+8) */
#define X86_64_SEG_UCODE64   0x20   /* RPL=3, 64-bit user code (SYSRET base+16) */
#define X86_64_SEG_TSS       0x28   /* 16-byte TSS descriptor (occupies two slots) */

/* Access byte bits (same as 32-bit, plus L and D bits in the flags nibble) */
#define GDT64_ACCESS_PRESENT  (1U << 7)
#define GDT64_ACCESS_DPL0     (0U << 5)
#define GDT64_ACCESS_DPL3     (3U << 5)
#define GDT64_ACCESS_SEGMENT  (1U << 4)
#define GDT64_ACCESS_EXEC     (1U << 3)
#define GDT64_ACCESS_DC       (1U << 2)
#define GDT64_ACCESS_RW       (1U << 1)
#define GDT64_ACCESS_ACCESSED (1U << 0)

/* Flags nibble bits */
#define GDT64_FLAG_GRAN_4K    (1U << 3)  /* 4 KB granularity */
#define GDT64_FLAG_DB         (1U << 2)  /* Default op-size 32-bit (D bit) */
#define GDT64_FLAG_LONG       (1U << 1)  /* 64-bit code segment (L bit) */

/**
 * x86_64_gdt_entry_t - Standard 8-byte GDT descriptor.
 *
 * For the TSS, two consecutive entries form a 16-byte descriptor; use
 * x86_64_gdt_tss_entry_t below.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_hi_flags;
    uint8_t  base_hi;
} x86_64_gdt_entry_t;

/**
 * x86_64_gdt_tss_entry_t - 16-byte system-descriptor for the TSS.
 *
 * The low 8 bytes are identical to a normal GDT entry; the high 8 bytes
 * extend the 32-bit base to 64 bits.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;         /* 0x89 = present, ring-0, 64-bit available TSS */
    uint8_t  limit_hi_flags;
    uint8_t  base_high24_31;
    uint32_t base_high32_63;
    uint32_t reserved;
} x86_64_gdt_tss_entry_t;

/**
 * x86_64_gdt_descriptor_t - Operand for LGDT / SGDT.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} x86_64_gdt_descriptor_t;

static inline void x86_64_lgdt(const x86_64_gdt_descriptor_t *desc)
{
    __asm__ volatile ("lgdt %0" :: "m"(*desc) : "memory");
}

static inline void x86_64_sgdt(x86_64_gdt_descriptor_t *desc)
{
    __asm__ volatile ("sgdt %0" : "=m"(*desc));
}

/* Helper: encode a standard flat 64-bit code/data descriptor as a uint64 */
static inline uint64_t x86_64_gdt_make_desc(uint8_t access, uint8_t flags)
{
    /* Base and limit are ignored in 64-bit mode; set limit to 0xFFFFF */
    uint64_t desc = 0;
    desc |= (uint64_t)0xFFFF;                           /* limit 15:0  */
    /* base 15:0 = 0, mid = 0 */
    desc |= (uint64_t)access           << 40;
    desc |= (uint64_t)(0x0F)           << 48;           /* limit 19:16 */
    desc |= (uint64_t)(flags & 0x0F)   << 52;
    /* base 31:24 = 0 */
    return desc;
}

/* =========================================================================
 * IDT - Interrupt Descriptor Table (64-bit)
 * ========================================================================= */

/* 64-bit gate types */
#define IDT64_GATE_INT   0x0E   /* 64-bit interrupt gate */
#define IDT64_GATE_TRAP  0x0F   /* 64-bit trap gate */

#define IDT64_PRESENT    (1U << 7)
#define IDT64_DPL0       (0U << 5)
#define IDT64_DPL3       (3U << 5)

/**
 * x86_64_idt_entry_t - 16-byte 64-bit IDT gate descriptor.
 */
typedef struct __attribute__((packed)) {
    uint16_t offset_0_15;
    uint16_t selector;
    uint8_t  ist;            /* Interrupt Stack Table index (bits 2:0); 0 = none */
    uint8_t  type_attr;      /* P | DPL | 0 | gate-type */
    uint16_t offset_16_31;
    uint32_t offset_32_63;
    uint32_t reserved;
} x86_64_idt_entry_t;

/**
 * x86_64_idt_descriptor_t - Operand for LIDT / SIDT.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} x86_64_idt_descriptor_t;

static inline void x86_64_idt_set_gate(x86_64_idt_entry_t *e,
                                        uint64_t handler,
                                        uint16_t sel,
                                        uint8_t  type_attr,
                                        uint8_t  ist)
{
    e->offset_0_15  = (uint16_t)(handler & 0xFFFF);
    e->selector     = sel;
    e->ist          = ist & 0x07;
    e->type_attr    = type_attr;
    e->offset_16_31 = (uint16_t)((handler >> 16) & 0xFFFF);
    e->offset_32_63 = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    e->reserved     = 0;
}

static inline void x86_64_lidt(const x86_64_idt_descriptor_t *desc)
{
    __asm__ volatile ("lidt %0" :: "m"(*desc) : "memory");
}

static inline void x86_64_sidt(x86_64_idt_descriptor_t *desc)
{
    __asm__ volatile ("sidt %0" : "=m"(*desc));
}

/* =========================================================================
 * TSS - 64-bit Task State Segment
 * ========================================================================= */

/**
 * x86_64_tss_t - 64-bit TSS.
 *
 * The important fields are rsp0 (kernel stack pointer on privilege elevation)
 * and ist1-ist7 (Interrupt Stack Table stacks for NMI, #DF, MCE, etc.).
 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;          /* Kernel stack (ring 0) */
    uint64_t rsp1;          /* Unused by Fern */
    uint64_t rsp2;          /* Unused by Fern */
    uint64_t reserved1;
    uint64_t ist[7];        /* ist[0] = IST1 ... ist[6] = IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    /* Offset to I/O permission bitmap */
} x86_64_tss_t;

/* IST slot assignments used by Fern exception handlers */
#define X86_64_IST_NMI      1   /* Non-Maskable Interrupt */
#define X86_64_IST_DF       2   /* Double Fault (#DF) */
#define X86_64_IST_MCE      3   /* Machine Check (#MC) */
#define X86_64_IST_BP       4   /* Breakpoint (#BP) optional dedicated stack */
#define X86_64_IST_SS       5   /* Stack-Segment Fault (#SS) */

/* =========================================================================
 * RFLAGS (64-bit extension of EFLAGS)
 * ========================================================================= */

typedef uint64_t rflags_t;

/* Low 32 bits are identical to EFLAGS */
#define RFLAGS_CF    (1ULL <<  0)
#define RFLAGS_PF    (1ULL <<  2)
#define RFLAGS_AF    (1ULL <<  4)
#define RFLAGS_ZF    (1ULL <<  6)
#define RFLAGS_SF    (1ULL <<  7)
#define RFLAGS_TF    (1ULL <<  8)
#define RFLAGS_IF    (1ULL <<  9)
#define RFLAGS_DF    (1ULL << 10)
#define RFLAGS_OF    (1ULL << 11)
#define RFLAGS_IOPL0 (0ULL << 12)
#define RFLAGS_IOPL3 (3ULL << 12)
#define RFLAGS_IOPL_MASK (3ULL << 12)
#define RFLAGS_NT    (1ULL << 14)
#define RFLAGS_RF    (1ULL << 16)
#define RFLAGS_VM    (1ULL << 17)
#define RFLAGS_AC    (1ULL << 18)
#define RFLAGS_VIF   (1ULL << 19)
#define RFLAGS_VIP   (1ULL << 20)
#define RFLAGS_ID    (1ULL << 21)

static inline rflags_t x86_64_read_rflags(void)
{
    rflags_t f;
    __asm__ volatile ("pushfq; popq %0" : "=r"(f) :: "memory");
    return f;
}

static inline void x86_64_write_rflags(rflags_t f)
{
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

static inline rflags_t x86_64_irq_save(void)
{
    rflags_t f = x86_64_read_rflags();
    __asm__ volatile ("cli" ::: "memory");
    return f;
}

static inline void x86_64_irq_restore(rflags_t f)
{
    x86_64_write_rflags(f);
}

/* =========================================================================
 * Control registers
 * ========================================================================= */

/* CR0 bits (same as 32-bit) */
#define CR0_64_PE   (1ULL <<  0)
#define CR0_64_WP   (1ULL << 16)
#define CR0_64_PG   (1ULL << 31)

/* CR4 bits relevant for 64-bit */
#define CR4_64_PAE        (1ULL <<  5)
#define CR4_64_PGE        (1ULL <<  7)
#define CR4_64_OSFXSR     (1ULL <<  9)
#define CR4_64_OSXMMEXCPT (1ULL << 10)
#define CR4_64_UMIP       (1ULL << 11)
#define CR4_64_SMEP       (1ULL << 20)
#define CR4_64_SMAP       (1ULL << 21)
#define CR4_64_PKE        (1ULL << 22)
#define CR4_64_LA57       (1ULL << 12)  /* 5-level paging */
#define CR4_64_PCIDE      (1ULL << 17)
#define CR4_64_OSXSAVE    (1ULL << 18)
#define CR4_64_FSGSBASE   (1ULL << 16)

static inline uint64_t x86_64_read_cr0(void)
{
    uint64_t v; __asm__ volatile ("movq %%cr0, %0" : "=r"(v)); return v;
}
static inline void x86_64_write_cr0(uint64_t v)
{
    __asm__ volatile ("movq %0, %%cr0" :: "r"(v) : "memory");
}
static inline uint64_t x86_64_read_cr2(void)
{
    uint64_t v; __asm__ volatile ("movq %%cr2, %0" : "=r"(v)); return v;
}
static inline uint64_t x86_64_read_cr3(void)
{
    uint64_t v; __asm__ volatile ("movq %%cr3, %0" : "=r"(v)); return v;
}
static inline void x86_64_write_cr3(uint64_t v)
{
    __asm__ volatile ("movq %0, %%cr3" :: "r"(v) : "memory");
}
static inline uint64_t x86_64_read_cr4(void)
{
    uint64_t v; __asm__ volatile ("movq %%cr4, %0" : "=r"(v)); return v;
}
static inline void x86_64_write_cr4(uint64_t v)
{
    __asm__ volatile ("movq %0, %%cr4" :: "r"(v) : "memory");
}

static inline void x86_64_invlpg(uint64_t vaddr)
{
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}
static inline void x86_64_flush_tlb(void)
{
    x86_64_write_cr3(x86_64_read_cr3());
}

/* =========================================================================
 * MSR access
 * ========================================================================= */

static inline uint64_t x86_64_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void x86_64_wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr"
                      :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* MSR numbers */
#define MSR64_EFER           0xC0000080UL
#define MSR64_STAR           0xC0000081UL  /* SYSCALL: CS/SS selectors */
#define MSR64_LSTAR          0xC0000082UL  /* SYSCALL: 64-bit target RIP */
#define MSR64_CSTAR          0xC0000083UL  /* SYSCALL: compat-mode target RIP */
#define MSR64_SFMASK         0xC0000084UL  /* SYSCALL: RFLAGS mask */
#define MSR64_FS_BASE        0xC0000100UL
#define MSR64_GS_BASE        0xC0000101UL
#define MSR64_KERNEL_GS_BASE 0xC0000102UL  /* Swapped in by SWAPGS */
#define MSR64_TSC            0x00000010UL
#define MSR64_APIC_BASE      0x0000001BUL
#define MSR64_PAT            0x00000277UL  /* Page Attribute Table */
#define MSR64_MTRRCAP        0x000000FEUL

/* EFER bits */
#define EFER_SCE   (1ULL <<  0)   /* SYSCALL/SYSRET Enable */
#define EFER_LME   (1ULL <<  8)   /* Long Mode Enable */
#define EFER_LMA   (1ULL << 10)   /* Long Mode Active (read-only) */
#define EFER_NXE   (1ULL << 11)   /* No-Execute Enable */
#define EFER_SVME  (1ULL << 12)   /* SVM Enable (AMD) */
#define EFER_FFXSR (1ULL << 14)   /* Fast FXSAVE/FXRSTOR (AMD) */
#define EFER_TCE   (1ULL << 15)   /* Translation Cache Extension (AMD) */

/* =========================================================================
 * FS.base / GS.base manipulation
 *
 * Use RDFSBASE/WRFSBASE if CR4.FSGSBASE is set (faster than MSR path);
 * fall back to MSR otherwise.  Fern enables FSGSBASE early in boot.
 * ========================================================================= */

static inline uint64_t x86_64_read_fs_base(void)
{
    uint64_t base;
    __asm__ volatile ("rdfsbase %0" : "=r"(base));
    return base;
}

static inline void x86_64_write_fs_base(uint64_t base)
{
    __asm__ volatile ("wrfsbase %0" :: "r"(base));
}

static inline uint64_t x86_64_read_gs_base(void)
{
    uint64_t base;
    __asm__ volatile ("rdgsbase %0" : "=r"(base));
    return base;
}

static inline void x86_64_write_gs_base(uint64_t base)
{
    __asm__ volatile ("wrgsbase %0" :: "r"(base));
}

/**
 * x86_64_swapgs - Exchange GS.base with MSR_KERNEL_GS_BASE.
 *
 * Used on syscall/interrupt entry (user→kernel) and exit (kernel→user).
 */
static inline void x86_64_swapgs(void)
{
    __asm__ volatile ("swapgs" ::: "memory");
}

/* =========================================================================
 * SYSCALL / SYSRET
 * ========================================================================= */

/**
 * x86_64_syscall_init - Configure MSRs for SYSCALL/SYSRET dispatch.
 *
 * @kernel_cs:   Code segment selector for the kernel (STAR[47:32]).
 * @user_cs32:   32-bit compat user CS (STAR[63:48]); SYSRET uses user_cs32|3
 *               for CS and user_cs32+8 for SS.
 * @syscall_rip: Address of the syscall entry point (LSTAR).
 * @flags_mask:  Bits to clear in RFLAGS on SYSCALL (SFMASK).
 */
static inline void x86_64_syscall_init(uint16_t kernel_cs,
                                        uint16_t user_cs32,
                                        uint64_t syscall_rip,
                                        uint64_t flags_mask)
{
    /* Enable SCE + NXE in EFER */
    uint64_t efer = x86_64_rdmsr(MSR64_EFER);
    efer |= EFER_SCE | EFER_NXE;
    x86_64_wrmsr(MSR64_EFER, efer);

    /* STAR: [63:48] user CS base, [47:32] kernel CS */
    uint64_t star = ((uint64_t)user_cs32 << 48) | ((uint64_t)kernel_cs << 32);
    x86_64_wrmsr(MSR64_STAR,   star);
    x86_64_wrmsr(MSR64_LSTAR,  syscall_rip);
    x86_64_wrmsr(MSR64_SFMASK, flags_mask);
}

/* =========================================================================
 * 4-level page table types (4 KB granule, 48-bit VA)
 *
 * Virtual address layout:
 *   [47:39] PML4 index (9 bits)
 *   [38:30] PDP  index (9 bits)
 *   [29:21] PD   index (9 bits)
 *   [20:12] PT   index (9 bits)
 *   [11:0]  Page offset (12 bits)
 * ========================================================================= */

#define X86_64_PAGE_SHIFT      12
#define X86_64_PAGE_SIZE       (1ULL << X86_64_PAGE_SHIFT)
#define X86_64_PAGE_MASK       (~(X86_64_PAGE_SIZE - 1))

#define X86_64_PGTBL_ENTRIES   512   /* 2^9 entries per level */

/* Page table entry flags */
#define X86_64_PTE_PRESENT     (1ULL <<  0)
#define X86_64_PTE_RW          (1ULL <<  1)
#define X86_64_PTE_USER        (1ULL <<  2)
#define X86_64_PTE_PWT         (1ULL <<  3)   /* Write-through */
#define X86_64_PTE_PCD         (1ULL <<  4)   /* Cache disable */
#define X86_64_PTE_ACCESSED    (1ULL <<  5)
#define X86_64_PTE_DIRTY       (1ULL <<  6)
#define X86_64_PTE_PS          (1ULL <<  7)   /* Huge page (PDP=1 GB, PD=2 MB) */
#define X86_64_PTE_GLOBAL      (1ULL <<  8)
#define X86_64_PTE_NX          (1ULL << 63)   /* No-Execute (requires EFER.NXE) */
#define X86_64_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL  /* PFN field [51:12] */

typedef uint64_t x86_64_pml4e_t;   /* PML4 entry (level 4) */
typedef uint64_t x86_64_pdpe_t;    /* Page Directory Pointer entry (level 3) */
typedef uint64_t x86_64_pde_t;     /* Page Directory entry (level 2) */
typedef uint64_t x86_64_pte_t;     /* Page Table entry (level 1) */

/* Virtual address decomposition */
#define X86_64_VA_PML4_IDX(va)  (((uint64_t)(va) >> 39) & 0x1FF)
#define X86_64_VA_PDP_IDX(va)   (((uint64_t)(va) >> 30) & 0x1FF)
#define X86_64_VA_PD_IDX(va)    (((uint64_t)(va) >> 21) & 0x1FF)
#define X86_64_VA_PT_IDX(va)    (((uint64_t)(va) >> 12) & 0x1FF)
#define X86_64_VA_OFFSET(va)    ( (uint64_t)(va)         & 0xFFF)

/* Canonical address check: bits 63:48 must be sign extension of bit 47 */
static inline bool x86_64_is_canonical(uint64_t va)
{
    int64_t sva = (int64_t)va;
    return (sva == (sva << 16 >> 16));
}

/* Kernel / user virtual address ranges (common x86-64 layout) */
#define X86_64_USER_SPACE_END    0x00007FFFFFFFFFFFULL
#define X86_64_KERNEL_SPACE_START 0xFFFF800000000000ULL

/* =========================================================================
 * CPUID (64-bit; same instruction, wider GP registers)
 * ========================================================================= */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} x86_64_cpuid_result_t;

static inline x86_64_cpuid_result_t x86_64_cpuid(uint32_t leaf, uint32_t subleaf)
{
    x86_64_cpuid_result_t r;
    __asm__ volatile (
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf)
    );
    return r;
}

/* Extended CPUID leaves */
#define CPUID64_EXTENDED_FEATURES   0x07U  /* ECX=0: SMEP, SMAP, AVX512 etc */
#define CPUID64_EXTENDED_INFO       0x80000001U
#define CPUID64_BRAND_STR_0         0x80000002U
#define CPUID64_BRAND_STR_1         0x80000003U
#define CPUID64_BRAND_STR_2         0x80000004U

/* Leaf 7 / subleaf 0 EBX bits */
#define CPUID7_EBX_FSGSBASE  (1U <<  0)
#define CPUID7_EBX_TSX_HLE   (1U <<  4)
#define CPUID7_EBX_AVX2      (1U <<  5)
#define CPUID7_EBX_SMEP      (1U <<  7)
#define CPUID7_EBX_TSX_RTM   (1U << 11)
#define CPUID7_EBX_AVX512F   (1U << 16)
#define CPUID7_EBX_RDSEED    (1U << 18)
#define CPUID7_EBX_SMAP      (1U << 20)
#define CPUID7_EBX_CLFLUSHOPT (1U << 23)

/* Extended info (leaf 0x80000001) EDX bits */
#define CPUID_EXT_EDX_NX     (1U << 20)   /* Execute Disable (NX) */
#define CPUID_EXT_EDX_PAGE1G (1U << 26)   /* 1 GB pages */
#define CPUID_EXT_EDX_LM     (1U << 29)   /* Long Mode */

/* =========================================================================
 * TSC
 * ========================================================================= */

static inline uint64_t x86_64_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/** Serialising TSC read: ensures all prior loads/stores complete first. */
static inline uint64_t x86_64_rdtscp(uint32_t *aux)
{
    uint32_t lo, hi, a;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(a));
    if (aux) *aux = a;
    return ((uint64_t)hi << 32) | lo;
}

/* =========================================================================
 * Port I/O (identical to 32-bit; kept here for completeness)
 * ========================================================================= */

static inline uint8_t x86_64_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "dN"(port));
    return val;
}
static inline void x86_64_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "dN"(port));
}
static inline uint16_t x86_64_inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "dN"(port));
    return val;
}
static inline void x86_64_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "dN"(port));
}
static inline uint32_t x86_64_inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "dN"(port));
    return val;
}
static inline void x86_64_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" :: "a"(val), "dN"(port));
}
static inline void x86_64_io_wait(void)
{
    x86_64_outb(0x80, 0x00);
}

#endif /* FOREST_ARCH_X86_64_H */

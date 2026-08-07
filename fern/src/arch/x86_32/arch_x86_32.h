/*
 * Fern - x86 32-bit (i686 / IA-32) Architecture Header
 * arch_x86_32.h
 *
 * Defines structures, macros, and inline helpers specific to the IA-32
 * architecture.  This file is included automatically by arch.h when
 * ARCH_X86_32 == 1; do NOT include it directly.
 *
 * Covers:
 *   - GDT / LDT entry and descriptor types
 *   - IDT gate descriptor
 *   - TSS (Task State Segment)
 *   - EFLAGS bit definitions
 *   - Segment register helpers
 *   - CR0 / CR2 / CR3 / CR4 control register inlines
 *   - eflags_t helpers (save/restore around critical sections)
 *   - Port I/O inlines (inb/outb/inw/outw/inl/outl)
 *   - CPUID helper
 */

#ifndef FOREST_ARCH_X86_32_H
#define FOREST_ARCH_X86_32_H

#ifndef FOREST_ARCH_H
#error "Include arch.h, not arch_x86_32.h directly."
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * GDT - Global Descriptor Table
 * ========================================================================= */

/* Segment selector bit fields */
#define SEG_RPL_MASK    0x0003  /* Requested Privilege Level mask */
#define SEG_TI_GDT      0x0000  /* Table indicator: GDT */
#define SEG_TI_LDT      0x0004  /* Table indicator: LDT */

/* Standard Fern segment selectors (matches gdt.h / gdt.c) */
#define X86_32_SEG_NULL          0x00
#define X86_32_SEG_KCODE         0x08  /* RPL=0, kernel code */
#define X86_32_SEG_KDATA         0x10  /* RPL=0, kernel data/stack */
#define X86_32_SEG_UCODE         0x18  /* RPL=3, user code  (with RPL=3 → 0x1B) */
#define X86_32_SEG_UDATA         0x20  /* RPL=3, user data  (with RPL=3 → 0x23) */
#define X86_32_SEG_TSS           0x28  /* TSS descriptor */

/* GDT access-byte bits */
#define GDT_ACCESS_PRESENT       (1U << 7)
#define GDT_ACCESS_DPL_SHIFT     5
#define GDT_ACCESS_DPL_RING0     (0U << 5)
#define GDT_ACCESS_DPL_RING3     (3U << 5)
#define GDT_ACCESS_SEGMENT       (1U << 4)  /* 0 = system, 1 = code/data */
#define GDT_ACCESS_EXEC          (1U << 3)  /* code segment */
#define GDT_ACCESS_DC            (1U << 2)  /* direction / conforming */
#define GDT_ACCESS_RW            (1U << 1)  /* readable (code) / writable (data) */
#define GDT_ACCESS_ACCESSED      (1U << 0)

/* GDT flags nibble (upper 4 bits of the granularity byte) */
#define GDT_FLAG_GRAN_4K         (1U << 3)  /* 1 = 4 KB granularity */
#define GDT_FLAG_SIZE_32         (1U << 2)  /* 1 = 32-bit protected mode */
#define GDT_FLAG_LONG_MODE       (1U << 1)  /* 1 = 64-bit code seg (unused in 32-bit) */

/**
 * x86_32_gdt_entry_t - Raw 8-byte GDT / LDT descriptor.
 *
 * Fields match the IA-32 manual layout (Vol. 3A, Figure 3-8).
 */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;     /* Bits  0-15 of segment limit */
    uint16_t base_low;      /* Bits  0-15 of base address  */
    uint8_t  base_mid;      /* Bits 16-23 of base address  */
    uint8_t  access;        /* Access byte (P | DPL | S | type bits) */
    uint8_t  limit_hi_flags;/* [7:4] flags, [3:0] limit bits 19-16 */
    uint8_t  base_hi;       /* Bits 24-31 of base address  */
} x86_32_gdt_entry_t;

/**
 * x86_32_gdt_descriptor_t - Operand for LGDT / SGDT.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit;         /* Size of GDT in bytes minus 1 */
    uint32_t base;          /* Linear address of GDT */
} x86_32_gdt_descriptor_t;

/** Encode a flat GDT entry at compile time. */
#define X86_32_GDT_ENTRY(base, limit, access_byte, flags_nibble)        \
    {                                                                    \
        .limit_low      = (uint16_t)((limit) & 0xFFFF),                 \
        .base_low       = (uint16_t)((base)  & 0xFFFF),                 \
        .base_mid       = (uint8_t)(((base)  >> 16) & 0xFF),            \
        .access         = (uint8_t)(access_byte),                        \
        .limit_hi_flags = (uint8_t)((((limit) >> 16) & 0x0F) |          \
                          (((flags_nibble) & 0x0F) << 4)),               \
        .base_hi        = (uint8_t)(((base)  >> 24) & 0xFF),            \
    }

static inline void x86_32_gdt_set_entry(x86_32_gdt_entry_t *e,
                                         uint32_t base, uint32_t limit,
                                         uint8_t access, uint8_t flags)
{
    e->limit_low      = (uint16_t)(limit & 0xFFFF);
    e->base_low       = (uint16_t)(base  & 0xFFFF);
    e->base_mid       = (uint8_t)((base  >> 16) & 0xFF);
    e->access         = access;
    e->limit_hi_flags = (uint8_t)(((limit >> 16) & 0x0F) | ((flags & 0x0F) << 4));
    e->base_hi        = (uint8_t)((base  >> 24) & 0xFF);
}

static inline void x86_32_lgdt(const x86_32_gdt_descriptor_t *desc)
{
    __asm__ volatile ("lgdt %0" :: "m"(*desc) : "memory");
}

static inline void x86_32_sgdt(x86_32_gdt_descriptor_t *desc)
{
    __asm__ volatile ("sgdt %0" : "=m"(*desc));
}

/* =========================================================================
 * IDT - Interrupt Descriptor Table
 * ========================================================================= */

/* IDT gate types (for 32-bit protected mode) */
#define IDT_GATE_TASK32     0x05   /* 32-bit task gate      */
#define IDT_GATE_INT16      0x06   /* 16-bit interrupt gate */
#define IDT_GATE_TRAP16     0x07   /* 16-bit trap gate      */
#define IDT_GATE_INT32      0x0E   /* 32-bit interrupt gate */
#undef IDT_GATE_TRAP32
#define IDT_GATE_TRAP32     0x0F   /* 32-bit trap gate      */

#undef IDT_PRESENT
#define IDT_PRESENT         (1U << 7)
#define IDT_DPL_SHIFT       5
#define IDT_DPL_RING0       (0U << 5)
#define IDT_DPL_RING3       (3U << 5)

/**
 * x86_32_idt_entry_t - 8-byte IDT gate descriptor.
 */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;    /* Bits  0-15 of handler address */
    uint16_t selector;      /* Code segment selector */
    uint8_t  reserved;      /* Must be zero */
    uint8_t  type_attr;     /* P | DPL | 0 | gate-type */
    uint16_t offset_high;   /* Bits 16-31 of handler address */
} x86_32_idt_entry_t;

/**
 * x86_32_idt_descriptor_t - Operand for LIDT / SIDT.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} x86_32_idt_descriptor_t;

static inline void x86_32_idt_set_gate(x86_32_idt_entry_t *e,
                                        uint32_t handler,
                                        uint16_t sel,
                                        uint8_t  type_attr)
{
    e->offset_low  = (uint16_t)(handler & 0xFFFF);
    e->selector    = sel;
    e->reserved    = 0;
    e->type_attr   = type_attr;
    e->offset_high = (uint16_t)((handler >> 16) & 0xFFFF);
}

static inline void x86_32_lidt(const x86_32_idt_descriptor_t *desc)
{
    __asm__ volatile ("lidt %0" :: "m"(*desc) : "memory");
}

static inline void x86_32_sidt(x86_32_idt_descriptor_t *desc)
{
    __asm__ volatile ("sidt %0" : "=m"(*desc));
}

/* =========================================================================
 * TSS - Task State Segment (hardware, 32-bit)
 * ========================================================================= */

/**
 * x86_32_tss_t - IA-32 hardware TSS (104 bytes).
 *
 * Fern only uses the TSS for ring-0 stack switching; the I/O permission
 * bitmap base is set to sizeof(x86_32_tss_t) to deny all I/O from user mode.
 */
typedef struct __attribute__((packed)) {
    uint16_t prev_tss;      /* Previous TSS selector (for task chaining) */
    uint16_t _res0;
    uint32_t esp0;          /* Kernel stack pointer (ring 0) */
    uint16_t ss0;           /* Kernel stack segment (ring 0) */
    uint16_t _res1;
    uint32_t esp1;          /* Unused by Fern */
    uint16_t ss1;
    uint16_t _res2;
    uint32_t esp2;          /* Unused */
    uint16_t ss2;
    uint16_t _res3;
    uint32_t cr3;           /* Page directory base (not used by sw TSS switch) */
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint16_t es;  uint16_t _res4;
    uint16_t cs;  uint16_t _res5;
    uint16_t ss;  uint16_t _res6;
    uint16_t ds;  uint16_t _res7;
    uint16_t fs;  uint16_t _res8;
    uint16_t gs;  uint16_t _res9;
    uint16_t ldt; uint16_t _res10;
    uint16_t trap;          /* Debug trap flag */
    uint16_t iomap_base;    /* Offset to I/O permission bitmap */
} x86_32_tss_t;

/* =========================================================================
 * EFLAGS - Extended Flags Register
 * ========================================================================= */

/** eflags_t is just an alias for uint32_t; named for self-documentation. */
typedef uint32_t eflags_t;

#define EFLAGS_CF    (1U <<  0)  /* Carry Flag */
#define EFLAGS_PF    (1U <<  2)  /* Parity Flag */
#define EFLAGS_AF    (1U <<  4)  /* Auxiliary Carry Flag */
#define EFLAGS_ZF    (1U <<  6)  /* Zero Flag */
#define EFLAGS_SF    (1U <<  7)  /* Sign Flag */
#define EFLAGS_TF    (1U <<  8)  /* Trap Flag (single-step) */
#define EFLAGS_IF    (1U <<  9)  /* Interrupt Enable Flag */
#define EFLAGS_DF    (1U << 10)  /* Direction Flag */
#define EFLAGS_OF    (1U << 11)  /* Overflow Flag */
#define EFLAGS_IOPL0 (0U << 12)  /* I/O Privilege Level (ring 0) */
#define EFLAGS_IOPL3 (3U << 12)  /* I/O Privilege Level (ring 3) */
#define EFLAGS_IOPL_MASK (3U << 12)
#define EFLAGS_NT    (1U << 14)  /* Nested Task Flag */
#define EFLAGS_RF    (1U << 16)  /* Resume Flag */
#define EFLAGS_VM    (1U << 17)  /* Virtual-8086 Mode */
#define EFLAGS_AC    (1U << 18)  /* Alignment Check (CR0.AM must be set) */
#define EFLAGS_VIF   (1U << 19)  /* Virtual Interrupt Flag */
#define EFLAGS_VIP   (1U << 20)  /* Virtual Interrupt Pending */
#define EFLAGS_ID    (1U << 21)  /* CPUID instruction supported */

/** Read the current EFLAGS value. */
static inline eflags_t x86_32_read_eflags(void)
{
    eflags_t f;
    __asm__ volatile ("pushfl; popl %0" : "=r"(f) :: "memory");
    return f;
}

/** Write EFLAGS (use with care – clobbers all status bits). */
static inline void x86_32_write_eflags(eflags_t f)
{
    __asm__ volatile ("pushl %0; popfl" :: "r"(f) : "memory", "cc");
}

/** Save EFLAGS and disable interrupts atomically; returns old EFLAGS. */
static inline eflags_t x86_32_irq_save(void)
{
    eflags_t f = x86_32_read_eflags();
    __asm__ volatile ("cli" ::: "memory");
    return f;
}

/** Restore EFLAGS (re-enable interrupts if they were on before). */
static inline void x86_32_irq_restore(eflags_t f)
{
    x86_32_write_eflags(f);
}

/* =========================================================================
 * Segment register read helpers
 * ========================================================================= */

static inline uint16_t x86_32_read_cs(void)
{
    uint16_t cs;
    __asm__ volatile ("movw %%cs, %0" : "=r"(cs));
    return cs;
}

static inline uint16_t x86_32_read_ds(void)
{
    uint16_t ds;
    __asm__ volatile ("movw %%ds, %0" : "=r"(ds));
    return ds;
}

static inline uint16_t x86_32_read_es(void)
{
    uint16_t es;
    __asm__ volatile ("movw %%es, %0" : "=r"(es));
    return es;
}

static inline uint16_t x86_32_read_fs(void)
{
    uint16_t fs;
    __asm__ volatile ("movw %%fs, %0" : "=r"(fs));
    return fs;
}

static inline uint16_t x86_32_read_gs(void)
{
    uint16_t gs;
    __asm__ volatile ("movw %%gs, %0" : "=r"(gs));
    return gs;
}

static inline uint16_t x86_32_read_ss(void)
{
    uint16_t ss;
    __asm__ volatile ("movw %%ss, %0" : "=r"(ss));
    return ss;
}

static inline void x86_32_write_ds(uint16_t sel)
{
    __asm__ volatile ("movw %0, %%ds" :: "r"(sel) : "memory");
}

static inline void x86_32_write_es(uint16_t sel)
{
    __asm__ volatile ("movw %0, %%es" :: "r"(sel) : "memory");
}

static inline void x86_32_write_fs(uint16_t sel)
{
    __asm__ volatile ("movw %0, %%fs" :: "r"(sel) : "memory");
}

static inline void x86_32_write_gs(uint16_t sel)
{
    __asm__ volatile ("movw %0, %%gs" :: "r"(sel) : "memory");
}

static inline void x86_32_write_ss(uint16_t sel)
{
    __asm__ volatile ("movw %0, %%ss" :: "r"(sel) : "memory");
}

/* =========================================================================
 * Control register inlines  (CR0, CR2, CR3, CR4)
 * ========================================================================= */

/* CR0 bit definitions */
#define CR0_PE  (1U <<  0)   /* Protection Enable */
#define CR0_MP  (1U <<  1)   /* Monitor Coprocessor */
#define CR0_EM  (1U <<  2)   /* x87 FPU Emulation */
#define CR0_TS  (1U <<  3)   /* Task Switched */
#define CR0_ET  (1U <<  4)   /* Extension Type (always 1 on 486+) */
#define CR0_NE  (1U <<  5)   /* Numeric Error */
#define CR0_WP  (1U << 16)   /* Write Protect (kernel cannot write to RO pages) */
#define CR0_AM  (1U << 18)   /* Alignment Mask */
#define CR0_NW  (1U << 29)   /* Not Write-through */
#define CR0_CD  (1U << 30)   /* Cache Disable */
#define CR0_PG  (1U << 31)   /* Paging Enable */

/* CR4 bit definitions */
#define CR4_VME        (1U <<  0)  /* Virtual-8086 Mode Extensions */
#define CR4_PVI        (1U <<  1)  /* Protected-mode Virtual Interrupts */
#define CR4_TSD        (1U <<  2)  /* Time Stamp Disable (RDTSC only at CPL 0) */
#define CR4_DE         (1U <<  3)  /* Debugging Extensions */
#define CR4_PSE        (1U <<  4)  /* Page Size Extensions (4 MB pages) */
#define CR4_PAE        (1U <<  5)  /* Physical Address Extension */
#define CR4_MCE        (1U <<  6)  /* Machine Check Enable */
#define CR4_PGE        (1U <<  7)  /* Page Global Enable */
#define CR4_PCE        (1U <<  8)  /* Performance-monitoring Counter Enable */
#define CR4_OSFXSR     (1U <<  9)  /* OS FXSAVE/FXRSTOR support */
#define CR4_OSXMMEXCPT (1U << 10)  /* OS XMM Exception support */
#define CR4_UMIP       (1U << 11)  /* User-Mode Instruction Prevention */
#define CR4_VMXE       (1U << 13)  /* VMX Enable */
#define CR4_SMXE       (1U << 14)  /* SMX Enable */
#define CR4_FSGSBASE   (1U << 16)  /* RDFSBASE/WRFSBASE enable */
#define CR4_PCIDE      (1U << 17)  /* PCID Enable */
#define CR4_OSXSAVE    (1U << 18)  /* XSAVE/XRSTOR Enable */
#define CR4_SMEP       (1U << 20)  /* Supervisor Mode Execution Prevention */
#define CR4_SMAP       (1U << 21)  /* Supervisor Mode Access Prevention */
#define CR4_PKE        (1U << 22)  /* Protection Key Enable */

static inline uint32_t x86_32_read_cr0(void)
{
    uint32_t val;
    __asm__ volatile ("movl %%cr0, %0" : "=r"(val));
    return val;
}

static inline void x86_32_write_cr0(uint32_t val)
{
    __asm__ volatile ("movl %0, %%cr0" :: "r"(val) : "memory");
}

static inline uint32_t x86_32_read_cr2(void)
{
    uint32_t val;
    __asm__ volatile ("movl %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint32_t x86_32_read_cr3(void)
{
    uint32_t val;
    __asm__ volatile ("movl %%cr3, %0" : "=r"(val));
    return val;
}

static inline void x86_32_write_cr3(uint32_t val)
{
    __asm__ volatile ("movl %0, %%cr3" :: "r"(val) : "memory");
}

static inline uint32_t x86_32_read_cr4(void)
{
    uint32_t val;
    __asm__ volatile ("movl %%cr4, %0" : "=r"(val));
    return val;
}

static inline void x86_32_write_cr4(uint32_t val)
{
    __asm__ volatile ("movl %0, %%cr4" :: "r"(val) : "memory");
}

/** Flush TLB for a single virtual address. */
static inline void x86_32_invlpg(uint32_t vaddr)
{
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/** Flush the entire TLB by reloading CR3. */
static inline void x86_32_flush_tlb(void)
{
    x86_32_write_cr3(x86_32_read_cr3());
}

/* =========================================================================
 * Port I/O
 * ========================================================================= */

static inline uint8_t x86_32_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

static inline void x86_32_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "dN"(port));
}

static inline uint16_t x86_32_inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

static inline void x86_32_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "dN"(port));
}

static inline uint32_t x86_32_inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

static inline void x86_32_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" :: "a"(val), "dN"(port));
}

/** Brief I/O delay (~1 µs on modern hardware) by writing to port 0x80. */
static inline void x86_32_io_wait(void)
{
    x86_32_outb(0x80, 0x00);
}

/* =========================================================================
 * MSR access (present on Pentium and later)
 * ========================================================================= */

static inline uint64_t x86_32_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void x86_32_wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile ("wrmsr"
                      :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* Common MSR numbers */
#define MSR_EFER            0xC0000080UL  /* Extended Feature Enable Register */
#define MSR_STAR            0xC0000081UL  /* SYSCALL target CS/SS */
#define MSR_LSTAR           0xC0000082UL  /* SYSCALL target RIP (long mode) */
#define MSR_CSTAR           0xC0000083UL  /* SYSCALL target RIP (compat mode) */
#define MSR_SFMASK          0xC0000084UL  /* SYSCALL RFLAGS mask */
#define MSR_FS_BASE         0xC0000100UL  /* FS segment base */
#define MSR_GS_BASE         0xC0000101UL  /* GS segment base */
#define MSR_KERNEL_GS_BASE  0xC0000102UL  /* KernelGS base (swapgs) */
#define MSR_TSC             0x00000010UL  /* Time Stamp Counter */
#define MSR_APIC_BASE       0x0000001BUL  /* APIC base address */

/* =========================================================================
 * CPUID
 * ========================================================================= */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} x86_32_cpuid_result_t;

static inline x86_32_cpuid_result_t x86_32_cpuid(uint32_t leaf, uint32_t subleaf)
{
    x86_32_cpuid_result_t r;
    __asm__ volatile (
        "cpuid"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(leaf), "c"(subleaf)
    );
    return r;
}

/* CPUID leaf 1 feature bits (EDX) */
#define CPUID1_EDX_FPU      (1U <<  0)
#define CPUID1_EDX_VME      (1U <<  1)
#define CPUID1_EDX_PSE      (1U <<  3)
#define CPUID1_EDX_TSC      (1U <<  4)
#define CPUID1_EDX_MSR      (1U <<  5)
#define CPUID1_EDX_PAE      (1U <<  6)
#define CPUID1_EDX_APIC     (1U <<  9)
#define CPUID1_EDX_SEP      (1U << 11)  /* SYSENTER/SYSEXIT */
#define CPUID1_EDX_MTRR     (1U << 12)
#define CPUID1_EDX_PGE      (1U << 13)
#define CPUID1_EDX_MCA      (1U << 14)
#define CPUID1_EDX_CMOV     (1U << 15)
#define CPUID1_EDX_PAT      (1U << 16)
#define CPUID1_EDX_PSE36    (1U << 17)
#define CPUID1_EDX_MMX      (1U << 23)
#define CPUID1_EDX_FXSR     (1U << 24)
#define CPUID1_EDX_SSE      (1U << 25)
#define CPUID1_EDX_SSE2     (1U << 26)
#define CPUID1_EDX_HTT      (1U << 28)

/* CPUID leaf 1 feature bits (ECX) */
#define CPUID1_ECX_SSE3     (1U <<  0)
#define CPUID1_ECX_PCLMUL   (1U <<  1)
#define CPUID1_ECX_SSSE3    (1U <<  9)
#define CPUID1_ECX_SSE41    (1U << 19)
#define CPUID1_ECX_SSE42    (1U << 20)
#define CPUID1_ECX_POPCNT   (1U << 23)
#define CPUID1_ECX_AES      (1U << 25)
#define CPUID1_ECX_AVX      (1U << 28)
#define CPUID1_ECX_RDRAND   (1U << 30)

/* =========================================================================
 * TSC
 * ========================================================================= */

static inline uint64_t x86_32_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* =========================================================================
 * Paging helpers (4 KB pages, 32-bit flat / PAE not included here)
 * ========================================================================= */

#define X86_32_PAGE_SHIFT    12
#define X86_32_PAGE_SIZE     (1U << X86_32_PAGE_SHIFT)   /* 4096 bytes */
#define X86_32_PAGE_MASK     (~(X86_32_PAGE_SIZE - 1))

/* Page Directory / Table entry flags */
#define X86_32_PTE_PRESENT   (1U <<  0)
#define X86_32_PTE_RW        (1U <<  1)
#define X86_32_PTE_USER      (1U <<  2)
#define X86_32_PTE_PWT       (1U <<  3)  /* Write-through */
#define X86_32_PTE_PCD       (1U <<  4)  /* Cache disable */
#define X86_32_PTE_ACCESSED  (1U <<  5)
#define X86_32_PTE_DIRTY     (1U <<  6)
#define X86_32_PTE_PS        (1U <<  7)  /* Page size (PDE only: 4 MB) */
#define X86_32_PTE_GLOBAL    (1U <<  8)
#define X86_32_PTE_ADDR_MASK 0xFFFFF000U /* Bits 31-12 hold PFN */

typedef uint32_t x86_32_pde_t;  /* Page Directory Entry */
typedef uint32_t x86_32_pte_t;  /* Page Table Entry */

#define X86_32_PDE_COUNT     1024
#define X86_32_PTE_COUNT     1024

/* Derive PD / PT indices from a virtual address */
#define X86_32_VA_PD_IDX(va)   (((uint32_t)(va)) >> 22)
#define X86_32_VA_PT_IDX(va)  ((((uint32_t)(va)) >> 12) & 0x3FF)
#define X86_32_VA_OFFSET(va)   (((uint32_t)(va)) & 0xFFF)

#endif /* FOREST_ARCH_X86_32_H */

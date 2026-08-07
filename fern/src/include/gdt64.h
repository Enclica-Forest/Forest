/**
 * @file gdt64.h
 * @brief 64-bit Global Descriptor Table Interface
 *
 * Provides GDT and TSS setup for x86_64 long mode:
 *   - Segment selectors and their values
 *   - TSS structure layout (x86_64 104-byte TSS, Intel Vol. 3A §7.7)
 *   - IST (Interrupt Stack Table) indices and stack management
 *   - Public API for GDT init, TSS load, and kernel-stack update
 *
 * GDT layout (64-bit, canonical SYSCALL/SYSRET layout):
 *   Index 0  offset 0x00  null descriptor
 *   Index 1  offset 0x08  kernel code  (DPL=0, 64-bit, L=1)
 *   Index 2  offset 0x10  kernel data  (DPL=0)
 *   Index 3  offset 0x18  user code32  (DPL=3, 32-bit compat, L=0)
 *   Index 4  offset 0x20  user data    (DPL=3)
 *   Index 5  offset 0x28  user code64  (DPL=3, 64-bit, L=1)
 *   Index 6  offset 0x30  TSS low   \  16-byte system descriptor
 *   Index 7  offset 0x38  TSS high  /
 *
 * 64-bit descriptor format (each entry is 8 bytes):
 *   Bits 63:56  Base[31:24]
 *   Bit  55     G  (Granularity: 0=byte, 1=4 KB)
 *   Bit  54     D/B or L (L=1 for 64-bit code)
 *   Bit  53     L  (Long mode: 1 for 64-bit code segment)
 *   Bit  52     AVL
 *   Bits 51:48  Limit[19:16]
 *   Bit  47     P  (Present)
 *   Bits 46:45  DPL
 *   Bit  44     S  (1=code/data, 0=system)
 *   Bits 43:40  Type
 *   Bits 39:32  Base[23:16]
 *   Bits 31:16  Base[15:0]
 *   Bits 15:0   Limit[15:0]
 */

#ifndef GDT64_H
#define GDT64_H

#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__

/* =========================================================================
 * GDT Segment Selectors (canonical SYSCALL/SYSRET layout)
 *
 * The GDT is arranged so that SYSCALL/SYSRET compute the correct CS/SS
 * selectors for both kernel and user mode with a single STAR value:
 *
 *     STAR[47:32] = GDT64_KERNEL_CS  = 0x08
 *       SYSCALL:  kernel CS = 0x08,            kernel SS = 0x08 + 8 = 0x10
 *     STAR[63:48] = GDT64_STAR_USER_BASE = 0x18
 *       SYSRET:   user   SS = 0x18 + 8  = 0x20 (user data),  RPL=3 -> 0x23
 *                 user   CS = 0x18 + 16 = 0x28 (user code64), RPL=3 -> 0x2B
 *
 * GDT slot layout (each entry is 8 bytes; TSS spans two slots = 16 bytes):
 *     Index 0  0x00  null
 *     Index 1  0x08  kernel code   (DPL=0, L=1, type=0xA)
 *     Index 2  0x10  kernel data   (DPL=0, type=0x2)
 *     Index 3  0x18  user code32   (DPL=3, L=0, type=0xA)  [compat mode]
 *     Index 4  0x20  user data     (DPL=3, type=0x2)
 *     Index 5  0x28  user code64   (DPL=3, L=1, type=0xA)
 *     Index 6  0x30  TSS low    \
 *     Index 7  0x38  TSS high   /  16-byte 64-bit TSS descriptor
 *
 * The 32-bit user code slot (index 3) exists so that compat-mode
 * userspace (32-bit binaries on a 64-bit kernel) can be supported via
 * int 0x80.  When ENABLE_COMPAT_INT80 is off the slot is still present
 * but never referenced by SYSRET.
 * ========================================================================= */

#define GDT64_NULL_SEL          0x00    /* Null selector                   */
#define GDT64_KERNEL_CS         0x08    /* Kernel code  (index 1, RPL=0)   */
#define GDT64_KERNEL_DS         0x10    /* Kernel data  (index 2, RPL=0)   */
#define GDT64_KERNEL_SS         GDT64_KERNEL_DS  /* SS == DS in long mode */
#define GDT64_USER_CS32         0x18    /* User 32-bit code (index 3)      */
#define GDT64_USER_CS32_SEL     0x1B    /*   with RPL=3                    */
#define GDT64_USER_DS           0x20    /* User data        (index 4)      */
#define GDT64_USER_DS_SEL       0x23    /*   with RPL=3                    */
#define GDT64_USER_CS64         0x28    /* User 64-bit code (index 5)      */
#define GDT64_USER_CS64_SEL     0x2B    /*   with RPL=3                    */
#define GDT64_USER_CS           GDT64_USER_CS64_SEL  /* default user CS    */
#define GDT64_TSS               0x30    /* TSS selector    (index 6)       */

/* Aliases used internally by the flush routine */
#define GDT64_KERNEL_CODE_SEL   GDT64_KERNEL_CS
#define GDT64_KERNEL_DATA_SEL   GDT64_KERNEL_DS
#define GDT64_TSS_SEL           GDT64_TSS

/* STAR field values for SYSCALL/SYSRET (see comment block above) */
#define GDT64_STAR_SYSCALL_BASE 0x08U   /* STAR[47:32]: kernel CS base    */
#define GDT64_STAR_SYSRET_BASE  0x18U   /* STAR[63:48]: user SS/CS base   */

/* Number of 8-byte GDT slots (TSS occupies 2 adjacent slots) */
#define GDT64_ENTRY_COUNT       8

/* =========================================================================
 * IST Indices (1-based as the CPU requires; 0 means "use current stack")
 * ========================================================================= */

#define GDT64_IST_DOUBLE_FAULT  1   /* #DF double fault        */
#define GDT64_IST_NMI           2   /* NMI                     */
#define GDT64_IST_MACHINE_CHECK 3   /* #MC machine check       */
#define GDT64_IST_DEBUG         4   /* #DB debug               */

/* IST stack size: 16 KiB per stack */
#define GDT64_IST_STACK_SIZE    (16u * 1024u)
#define GDT64_IST_STACKS_COUNT  4   /* Number of IST stacks pre-allocated  */

/* =========================================================================
 * Data structures
 * ========================================================================= */

/**
 * @brief x86_64 Task State Segment (104 bytes, Intel Vol. 3A §7.7)
 *
 * All reserved fields must be zero.  The hardware ignores them on task
 * switches, but their value affects future compatibility.
 *
 * The IST table is 1-indexed from the CPU's perspective; ist[0] maps to
 * TSS.IST1, ist[6] maps to TSS.IST7.  Index 0 (no IST switch) is not
 * stored in the TSS.
 */
typedef struct __attribute__((packed)) tss64 {
    uint32_t reserved0;     /* 0x00 – must be zero                        */
    uint64_t rsp0;          /* 0x04 – ring-0 stack pointer (ring transition)*/
    uint64_t rsp1;          /* 0x0C – ring-1 stack pointer (unused)       */
    uint64_t rsp2;          /* 0x14 – ring-2 stack pointer (unused)       */
    uint64_t reserved1;     /* 0x1C – must be zero                        */
    uint64_t ist[7];        /* 0x24 – IST1..IST7 (ist[0]=IST1)           */
    uint64_t reserved2;     /* 0x5C – must be zero                        */
    uint16_t reserved3;     /* 0x64 – must be zero                        */
    uint16_t iopb_offset;   /* 0x66 – I/O permission bitmap offset        */
} tss64_t;

/**
 * @brief GDTR pseudo-descriptor (used with LGDT / SGDT)
 */
typedef struct __attribute__((packed)) {
    uint16_t limit;         /* GDT size in bytes minus 1   */
    uint64_t base;          /* Linear base address of GDT  */
} gdtr64_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialize the 64-bit GDT.
 *
 * Builds all seven descriptor slots (null, kernel CS/DS, user CS/DS, TSS
 * low + high), installs the GDTR with LGDT, reloads all segment registers
 * via a far return, and finally issues LTR to load the task register.
 *
 * Pre-allocates four static IST stacks (#DF, NMI, #MC, #DB) and wires them
 * into the internal TSS.
 *
 * @param kernel_stack_top  Initial RSP0 value: top of the kernel stack that
 *                          the CPU loads when transitioning from ring-3 to
 *                          ring-0 via interrupt or SYSCALL.
 */
void gdt64_init(uintptr_t kernel_stack_top);

/**
 * @brief Fill the TSS descriptor slots for the supplied TSS, set RSP0, and
 *        execute LTR to make this TSS the active one.
 *
 * Use this when you want to supply your own tss64_t (e.g. per-CPU TSS on
 * SMP).  gdt64_init() calls this internally with the statically allocated
 * TSS.
 *
 * @param tss           Pointer to a zero-initialised tss64_t.
 * @param kernel_stack  RSP0 to store in tss->rsp0.
 */
void gdt64_load_tss(tss64_t *tss, uint64_t kernel_stack);

/**
 * @brief Set one IST entry in a TSS.
 *
 * @param tss       TSS to modify.
 * @param ist_num   IST index, 1-based (1..7).  Matches the IST field in
 *                  an IDT gate.  Passing 0 is a no-op.
 * @param stack_top Top-of-stack address for this IST (stack grows down).
 */
void gdt64_set_ist(tss64_t *tss, int ist_num, uint64_t stack_top);

/**
 * @brief Update RSP0 in the active (internal) TSS.
 *
 * Must be called on every context switch to a user-space task so the CPU
 * knows which kernel stack to use when an interrupt or exception fires in
 * user mode.
 *
 * @param stack_top  New RSP0 value (top of per-task kernel stack).
 */
void gdt64_set_kernel_stack(uintptr_t stack_top);

/**
 * @brief Return a pointer to the internally-managed TSS.
 */
tss64_t *gdt64_get_tss(void);

/**
 * @brief Return the linear address of the GDT.
 */
uint64_t gdt64_get_base(void);

/**
 * @brief Return the GDT limit (sizeof(gdt) - 1).
 */
uint16_t gdt64_get_limit(void);

#endif /* __x86_64__ */
#endif /* GDT64_H */

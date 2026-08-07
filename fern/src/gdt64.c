/**
 * @file gdt64.c
 * @brief 64-bit Global Descriptor Table Implementation
 *
 * Sets up the minimal GDT required by an x86_64 kernel running in long mode.
 *
 *   Slot  Selector  Content
 *   ----  --------  -------
 *     0   0x00      Null descriptor (mandatory)
 *     1   0x08      Kernel code  (64-bit, DPL=0, L=1, type=0xA)
 *     2   0x10      Kernel data  (DPL=0, type=0x2)
 *     3   0x18      User data    (DPL=3, type=0x2)     ← STAR SYSRET base+8
 *     4   0x20      User code    (64-bit, DPL=3, L=1)  ← STAR SYSRET base+16
 *     5   0x28      TSS low  8 bytes  \  Together a 16-byte system descriptor
 *     6   0x30      TSS high 8 bytes  /
 *
 * 64-bit code/data segment encoding (8 bytes each):
 *   Bits 63:56  Base[31:24]          (always 0 in 64-bit mode)
 *   Bit  55     G  – Granularity     (1 = 4 KB, but limit is ignored in 64-bit)
 *   Bit  54     D/B                  (must be 0 when L=1)
 *   Bit  53     L  – Long mode       (1 for 64-bit code segments)
 *   Bit  52     AVL                  (available for OS use)
 *   Bits 51:48  Limit[19:16]         (ignored in 64-bit mode for CS/DS)
 *   Bit  47     P  – Present         (1 = valid segment)
 *   Bits 46:45  DPL                  (0=kernel, 3=user)
 *   Bit  44     S  – Segment type    (1 = code/data)
 *   Bits 43:40  Type                 (0xA = execute/read code, 0x2 = read/write data)
 *   Bits 39:32  Base[23:16]          (always 0)
 *   Bits 31:16  Base[15:0]           (always 0)
 *   Bits 15:0   Limit[15:0]          (always 0xFFFF; ignored in 64-bit mode)
 *
 * 64-bit TSS system descriptor spans two 8-byte GDT slots (16 bytes total):
 *   Lower 8 bytes (slot N):
 *     [15:0]   Limit[15:0]
 *     [39:16]  Base[23:0]
 *     [47:40]  Access byte: 0x89 = P=1, DPL=0, S=0, Type=9 (64-bit TSS Available)
 *     [51:48]  Limit[19:16]
 *     [55:52]  Flags: G=0, DB=0, L=0, AVL=0
 *     [63:56]  Base[31:24]
 *   Upper 8 bytes (slot N+1):
 *     [31:0]   Base[63:32]
 *     [63:32]  Reserved (must be 0)
 *
 * References:
 *   Intel 64-IA-32 SDM Vol. 3A §3.4.5 (segment descriptors)
 *   Intel 64-IA-32 SDM Vol. 3A §7.2.3 (64-bit TSS and descriptors)
 */

#include "include/gdt64.h"
#include "include/screen.h"     /* print(), print_hex() */
#include "include/string.h"     /* memset() */

#ifdef __x86_64__

/* =========================================================================
 * Segment descriptor raw values
 *
 * Encoding:  0x00 AF 9A 00 00 00 FF FF
 *             ^^   ^^  ^^             ^^^^
 *             |    |   |              Limit[15:0] = 0xFFFF (ignored in 64-bit)
 *             |    |   Access byte
 *             |    Flags nibble:  G=1, DB=0, L=1, AVL=0 → 0xA
 *             Base high byte (0 for flat)
 *
 * Access byte for kernel code (0x9A):  P=1, DPL=0, S=1, Type=0xA (execute/read)
 * Access byte for kernel data (0x92):  P=1, DPL=0, S=1, Type=0x2 (read/write)
 * Access byte for user   code (0xFA):  P=1, DPL=3, S=1, Type=0xA (execute/read)
 * Access byte for user   data (0xF2):  P=1, DPL=3, S=1, Type=0x2 (read/write)
 *
 * Flags nibble (bits 55:52): G=1 (bit55), DB=0 (bit54), L=1 (bit53), AVL=0 (bit52)
 *   → 0b1010 = 0xA; stored in bits [55:52] of the 64-bit descriptor.
 *   In the raw constant this nibble occupies the upper nibble of byte index 6
 *   (counting from bit 0): byte6 = 0xAF → bits[55:48] = (flags<<4)|limit[19:16].
 *
 * Data descriptors have L=0, DB=1 (32-bit default size) but in 64-bit long
 * mode the processor ignores D/B and L for data segments.  Setting them to
 * 0xAF (same as code) is safe and commonly done.
 * ========================================================================= */

#define SEG_KERNEL_CODE  0x00AF9A000000FFFFULL   /* DPL=0, L=1, execute/read */
#define SEG_KERNEL_DATA  0x00AF92000000FFFFULL   /* DPL=0, read/write        */
#define SEG_USER_CODE    0x00AFFA000000FFFFULL   /* DPL=3, L=1, execute/read */
#define SEG_USER_DATA    0x00AFF2000000FFFFULL   /* DPL=3, read/write        */

/* =========================================================================
 * Static storage
 * ========================================================================= */

/* The GDT itself – must remain at a stable physical address. */
static uint64_t s_gdt[GDT64_ENTRY_COUNT] __attribute__((aligned(16)));

/* GDTR pseudo-descriptor filled by gdt64_init(). */
static gdtr64_t s_gdtr;

/*
 * The internally-managed TSS.
 *
 * gdt64_init() uses this; callers that need a separate TSS (e.g. per-CPU on
 * SMP) should allocate their own tss64_t and call gdt64_load_tss() directly.
 */
static tss64_t s_tss __attribute__((aligned(64)));

/*
 * Static IST stacks – 16 KiB each, 16-byte aligned.
 * Allocated in BSS so they exist before the heap is initialised.
 * The CPU uses the *top* address (stacks grow downward).
 */
static uint8_t s_ist_df [GDT64_IST_STACK_SIZE] __attribute__((aligned(16))); /* IST1 #DF  */
static uint8_t s_ist_nmi[GDT64_IST_STACK_SIZE] __attribute__((aligned(16))); /* IST2 NMI  */
static uint8_t s_ist_mc [GDT64_IST_STACK_SIZE] __attribute__((aligned(16))); /* IST3 #MC  */
static uint8_t s_ist_db [GDT64_IST_STACK_SIZE] __attribute__((aligned(16))); /* IST4 #DB  */

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * @brief Encode a 64-bit TSS system descriptor into two consecutive GDT slots.
 *
 * The 64-bit TSS descriptor is 16 bytes wide and occupies two adjacent 8-byte
 * GDT entries.  The lower 8 bytes hold limit, base[23:0], the access byte
 * (0x89 = Present, DPL=0, Type=9 "64-bit TSS Available"), limit[19:16], and
 * base[31:24].  The upper 8 bytes hold base[63:32] and must have the upper
 * 32 bits zeroed.
 *
 * @param gdt   Pointer to the first slot (index N) in the GDT array.
 * @param slot  GDT slot index for the low half; slot+1 receives the high half.
 * @param base  Linear address of the TSS structure.
 * @param limit Size of the TSS minus 1.
 */
static void encode_tss_descriptor(uint64_t *gdt, int slot,
                                   uint64_t base, uint32_t limit)
{
    uint64_t low = 0;

    /* Limit[15:0] → bits [15:0] */
    low |= (uint64_t)(limit & 0xFFFFu);

    /* Base[23:0]  → bits [39:16] */
    low |= (uint64_t)(base & 0xFFFFFFULL) << 16;

    /* Access byte → bits [47:40]:
     *   0x89 = 1000_1001b
     *   P=1, DPL=00, S=0 (system), Type=1001 (64-bit TSS Available) */
    low |= (uint64_t)0x89ULL << 40;

    /* Limit[19:16] → bits [51:48] */
    low |= (uint64_t)((limit >> 16) & 0x0Fu) << 48;

    /* Base[31:24]  → bits [63:56] */
    low |= (uint64_t)((base >> 24) & 0xFFULL) << 56;

    gdt[slot]     = low;
    gdt[slot + 1] = (base >> 32) & 0xFFFFFFFFULL; /* Base[63:32], upper 32 = 0 */
}

/**
 * @brief Reload all segment registers and jump through the new CS.
 *
 * Executes LGDT to install the new descriptor table, reloads DS/ES/FS/GS/SS
 * with the kernel data selector, performs a far return (LRETQ) to reload CS
 * from the kernel code selector, and finally issues LTR to activate the TSS.
 *
 * This is an internal function; it must be called with s_gdtr already filled.
 */
static void gdt64_flush(void)
{
    uint16_t kdata = GDT64_KERNEL_DATA_SEL;
    uint64_t kcode = (uint64_t)GDT64_KERNEL_CODE_SEL;
    uint16_t tss   = GDT64_TSS_SEL;

    __asm__ __volatile__ (
        /* 1. Load the new GDTR */
        "lgdt %[desc]\n\t"

        /* 2. Reload data-class segment registers */
        "movw %[ds], %%ds\n\t"
        "movw %[ds], %%es\n\t"
        "movw %[ds], %%fs\n\t"
        "movw %[ds], %%gs\n\t"
        "movw %[ds], %%ss\n\t"

        /* 3. Far return to reload CS: push new CS, then the return RIP,
         *    then execute LRETQ which pops RIP+CS from the stack. */
        "pushq %[cs]\n\t"
        "leaq  1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"

        /* 4. Load the Task Register */
        "ltr  %[tss]\n\t"

        :   /* no C outputs */
        :   [desc] "m"  (s_gdtr),
            [ds]   "r"  (kdata),
            [cs]   "r"  (kcode),
            [tss]  "r"  (tss)
        :   "rax", "memory"
    );
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Set one IST entry in the given TSS.
 *
 * @param tss       TSS to modify.
 * @param ist_num   1-based IST index (1..7).  0 is silently ignored.
 * @param stack_top Top-of-stack address for this IST slot (stack grows down).
 */
void gdt64_set_ist(tss64_t *tss, int ist_num, uint64_t stack_top)
{
    if (!tss || ist_num < 1 || ist_num > 7)
        return;

    /* tss->ist[] is 0-indexed; ist_num is 1-based. */
    tss->ist[ist_num - 1] = stack_top;
}

/**
 * @brief Write the TSS descriptor into the GDT, set RSP0, and execute LTR.
 *
 * Encodes @p tss into GDT slots 5 and 6 (the two slots reserved for the
 * 64-bit TSS descriptor), stores @p kernel_stack in tss->rsp0, and executes
 * LTR with GDT64_TSS_SEL.
 *
 * @param tss           Pointer to a tss64_t that has been zero-initialised.
 * @param kernel_stack  RSP0 value: top of the kernel stack used on privilege
 *                      transitions from ring-3 to ring-0.
 */
void gdt64_load_tss(tss64_t *tss, uint64_t kernel_stack)
{
    if (!tss)
        return;

    /* Set the kernel-entry stack pointer. */
    tss->rsp0 = kernel_stack;

    /* IOPB beyond the TSS limit → all I/O from ring-3 traps to the kernel. */
    tss->iopb_offset = (uint16_t)sizeof(tss64_t);

    /* Encode the descriptor into GDT slots 5 (low) and 6 (high). */
    encode_tss_descriptor(s_gdt, 5,
                          (uint64_t)(uintptr_t)tss,
                          (uint32_t)(sizeof(tss64_t) - 1u));

    /* Reload the TSS register so the CPU picks up the new descriptor. */
    uint16_t tss_sel = GDT64_TSS_SEL;
    __asm__ __volatile__("ltr %0" :: "r"(tss_sel) : "memory");
}

/**
 * @brief Initialize the 64-bit GDT, TSS, and IST stacks.
 *
 * Builds all seven GDT entries, pre-allocates four static IST stacks,
 * fills the internal TSS, loads the GDTR, reloads segment registers, and
 * activates the TSS via LTR.
 *
 * @param kernel_stack_top  Initial RSP0: top of the boot-time kernel stack.
 */
void gdt64_init(uintptr_t kernel_stack_top)
{
    print("[GDT64] Initializing 64-bit GDT and TSS...\n");

    /* Zero the internal TSS before filling it. */
    memset(&s_tss, 0, sizeof(s_tss));

    /* ------------------------------------------------------------------
     * Build the GDT entries
     * ------------------------------------------------------------------ */
    s_gdt[0] = 0;                /* Index 0: null descriptor (mandatory) */
    s_gdt[1] = SEG_KERNEL_CODE;  /* Index 1: kernel code  (0x08) */
    s_gdt[2] = SEG_KERNEL_DATA;  /* Index 2: kernel data  (0x10) */
    s_gdt[3] = SEG_USER_DATA;    /* Index 3: user data    (0x18) — SYSRET SS */
    s_gdt[4] = SEG_USER_CODE;    /* Index 4: user code    (0x20) — SYSRET CS */
    /* Slots 5 and 6 are filled by gdt64_load_tss() below. */

    /* ------------------------------------------------------------------
     * Populate the GDTR pseudo-descriptor before encoding the TSS, so the
     * flush routine can issue LGDT immediately after.
     * ------------------------------------------------------------------ */
    s_gdtr.limit = (uint16_t)(sizeof(s_gdt) - 1u);
    s_gdtr.base  = (uint64_t)(uintptr_t)s_gdt;

    /* ------------------------------------------------------------------
     * Wire up the static IST stacks.
     * The CPU reads the *top* of each stack (stacks grow downward), so
     * IST = base_address + stack_size.
     * ------------------------------------------------------------------ */
    gdt64_set_ist(&s_tss, GDT64_IST_DOUBLE_FAULT,
                  (uint64_t)(uintptr_t)(s_ist_df  + GDT64_IST_STACK_SIZE));
    gdt64_set_ist(&s_tss, GDT64_IST_NMI,
                  (uint64_t)(uintptr_t)(s_ist_nmi + GDT64_IST_STACK_SIZE));
    gdt64_set_ist(&s_tss, GDT64_IST_MACHINE_CHECK,
                  (uint64_t)(uintptr_t)(s_ist_mc  + GDT64_IST_STACK_SIZE));
    gdt64_set_ist(&s_tss, GDT64_IST_DEBUG,
                  (uint64_t)(uintptr_t)(s_ist_db  + GDT64_IST_STACK_SIZE));

    /* ------------------------------------------------------------------
     * Encode the TSS descriptor into GDT slots 5+6, set RSP0, flush.
     * gdt64_load_tss() only issues LTR; the full LGDT + CS reload is
     * done by gdt64_flush() which runs immediately after.
     * ------------------------------------------------------------------ */

    /* First encode the TSS descriptor and set RSP0 / IOPB. */
    s_tss.rsp0       = (uint64_t)kernel_stack_top;
    s_tss.iopb_offset = (uint16_t)sizeof(tss64_t);
    encode_tss_descriptor(s_gdt, 5,
                          (uint64_t)(uintptr_t)&s_tss,
                          (uint32_t)(sizeof(s_tss) - 1u));

    /* Now flush: LGDT, reload segment regs, far-return to reload CS, LTR. */
    gdt64_flush();

    /* ------------------------------------------------------------------
     * Debug output
     * ------------------------------------------------------------------ */
    print("[GDT64] GDT loaded: base=0x");
    print_hex((uint32_t)((uint64_t)(uintptr_t)s_gdt >> 32));
    print_hex((uint32_t)(uintptr_t)s_gdt);
    print(", limit=0x");
    print_hex(s_gdtr.limit);
    print("\n");

    print("[GDT64] TSS at 0x");
    print_hex((uint32_t)((uint64_t)(uintptr_t)&s_tss >> 32));
    print_hex((uint32_t)(uintptr_t)&s_tss);
    print(", RSP0=0x");
    print_hex((uint32_t)((uint64_t)kernel_stack_top >> 32));
    print_hex((uint32_t)kernel_stack_top);
    print("\n");

    print("[GDT64] IST stacks: IST1=#DF, IST2=NMI, IST3=#MC, IST4=#DB\n");
    print("[GDT64] Initialization complete\n");
}

/**
 * @brief Set an IST entry in the internally-managed TSS.
 *
 * Convenience wrapper around gdt64_set_ist() that operates on the
 * static TSS managed by gdt64_init().  Called during context switch
 * to point an IST slot at a per-task stack.
 *
 * @param ist_num   IST index, 1-based (1..7).
 * @param stack_top Top-of-stack address for this IST slot.
 */
void gdt64_update_ist(int ist_num, uint64_t stack_top)
{
    gdt64_set_ist(&s_tss, ist_num, stack_top);
}

/**
 * @brief Update RSP0 in the internally-managed TSS.
 *
 * Must be called on every context switch to a user-space task.
 *
 * @param stack_top  New RSP0 value (top of per-task kernel stack).
 */
void gdt64_set_kernel_stack(uintptr_t stack_top)
{
    s_tss.rsp0 = (uint64_t)stack_top;
}

/**
 * @brief Return a pointer to the internally-managed TSS.
 */
tss64_t *gdt64_get_tss(void)
{
    return &s_tss;
}

/**
 * @brief Return the linear address of the GDT.
 */
uint64_t gdt64_get_base(void)
{
    return s_gdtr.base;
}

/**
 * @brief Return the GDT limit (sizeof(gdt) - 1).
 */
uint16_t gdt64_get_limit(void)
{
    return s_gdtr.limit;
}

#endif /* __x86_64__ */

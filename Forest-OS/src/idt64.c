/**
 * @file idt64.c
 * @brief 64-bit Interrupt Descriptor Table Implementation
 *
 * Builds and loads the 256-entry x86_64 IDT.  Each entry is a 16-byte
 * interrupt or trap gate descriptor (Intel Vol. 3A §6.14.1).
 *
 * The implementation:
 *   1. Declares all 256 external ASM stub symbols (isr_stub_N / irq_stub_N).
 *   2. Installs interrupt gates for CPU exceptions 0–31.
 *   3. Applies IST overrides on #DF (IST1), NMI (IST2), #MC (IST3), #DB (IST4).
 *   4. Installs interrupt gates for hardware IRQs 32–47.
 *   5. Installs a DPL=3 gate for the legacy int 0x80 syscall path.
 *   6. Installs a handler for the APIC spurious vector (0xFF).
 *   7. Executes LIDT.
 *
 * IST indices must match the stacks set up in gdt64.c:
 *   IST1 → #DF    IST2 → NMI    IST3 → #MC    IST4 → #DB
 *
 * External symbols expected from the assembler stubs file
 * (interrupt_stubs.asm or equivalent):
 *   isr_stub_0 … isr_stub_31     CPU exception entry points
 *   irq_stub_0 … irq_stub_15     Hardware IRQ entry points
 *   nmi_handler                  Dedicated NMI handler
 *   double_fault_handler         Dedicated #DF handler
 *   machine_check_handler        Dedicated #MC handler
 *   syscall_handler              Legacy int 0x80 entry point
 *   spurious_irq_handler         APIC spurious vector handler
 */

#include "include/idt64.h"
#include "include/gdt64.h"
#include "include/screen.h"   /* print(), print_hex() */
#include "include/string.h"   /* memset() */

#ifdef __x86_64__

/* =========================================================================
 * External assembly stub declarations
 *
 * Defined in src/interrupt_stubs.asm (or equivalent for the 64-bit build).
 * Each stub saves registers, pushes a vector number, and calls the C
 * dispatch function interrupt_common_handler().
 * ========================================================================= */

/* CPU exception stubs – vectors 0–31 */
extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_2(void);
extern void isr_stub_3(void);
extern void isr_stub_4(void);
extern void isr_stub_5(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_9(void);
extern void isr_stub_10(void);
extern void isr_stub_11(void);
extern void isr_stub_12(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_15(void);
extern void isr_stub_16(void);
extern void isr_stub_17(void);
extern void isr_stub_18(void);
extern void isr_stub_19(void);
extern void isr_stub_20(void);
extern void isr_stub_21(void);
extern void isr_stub_22(void);
extern void isr_stub_23(void);
extern void isr_stub_24(void);
extern void isr_stub_25(void);
extern void isr_stub_26(void);
extern void isr_stub_27(void);
extern void isr_stub_28(void);
extern void isr_stub_29(void);
extern void isr_stub_30(void);
extern void isr_stub_31(void);

/* Hardware IRQ stubs – vectors 32–47 */
extern void irq_stub_0(void);
extern void irq_stub_1(void);
extern void irq_stub_2(void);
extern void irq_stub_3(void);
extern void irq_stub_4(void);
extern void irq_stub_5(void);
extern void irq_stub_6(void);
extern void irq_stub_7(void);
extern void irq_stub_8(void);
extern void irq_stub_9(void);
extern void irq_stub_10(void);
extern void irq_stub_11(void);
extern void irq_stub_12(void);
extern void irq_stub_13(void);
extern void irq_stub_14(void);
extern void irq_stub_15(void);

/* Dedicated critical-exception handlers (may be the same as ISR stubs
 * but are kept separate to support IST stack switching) */
extern void nmi_handler(void);
extern void double_fault_handler(void);
extern void machine_check_handler(void);

/* Legacy int 0x80 syscall gate and APIC spurious vector */
extern void syscall_handler(void);
extern void spurious_irq_handler(void);

/* =========================================================================
 * Static IDT storage
 * ========================================================================= */

/* The IDT itself.  256 × 16 bytes = 4096 bytes (exactly one page). */
static idt64_entry_t idt64[IDT64_ENTRIES] __attribute__((aligned(16)));

/* IDTR pseudo-descriptor */
static idtr64_t idtr64;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * @brief Encode one IDT gate entry.
 *
 * @param entry     Pointer to the IDT slot to fill.
 * @param handler   Linear address of the handler stub.
 * @param selector  Code-segment selector (GDT64_KERNEL_CODE_SEL).
 * @param type_attr Gate attribute byte.
 * @param ist       IST index (0 = no IST switch).
 */
static void encode_gate(idt64_entry_t *entry, uintptr_t handler,
                         uint16_t selector, uint8_t type_attr, uint8_t ist)
{
    entry->offset_low  = (uint16_t)(handler & 0xFFFF);
    entry->selector    = selector;
    entry->ist         = ist & 0x07;        /* only 3 bits are valid */
    entry->type_attr   = type_attr;
    entry->offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    entry->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    entry->reserved    = 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Install or replace a gate entry (full parameter set).
 */
void idt64_set_gate(uint8_t vector, uintptr_t handler,
                    uint16_t selector, uint8_t type_attr, uint8_t ist)
{
    encode_gate(&idt64[vector], handler, selector, type_attr, ist);
}

/**
 * @brief Install a kernel interrupt gate (clears IF).
 */
void idt64_set_kernel_interrupt(uint8_t vector, uintptr_t handler)
{
    encode_gate(&idt64[vector], handler,
                GDT64_KERNEL_CODE_SEL, IDT64_KERNEL_INT_GATE, 0);
}

/**
 * @brief Install a kernel trap gate (preserves IF).
 */
void idt64_set_kernel_trap(uint8_t vector, uintptr_t handler)
{
    encode_gate(&idt64[vector], handler,
                GDT64_KERNEL_CODE_SEL, IDT64_KERNEL_TRAP_GATE, 0);
}

/**
 * @brief Install a user-accessible gate (DPL=3, clears IF).
 */
void idt64_set_user_gate(uint8_t vector, uintptr_t handler)
{
    encode_gate(&idt64[vector], handler,
                GDT64_KERNEL_CODE_SEL, IDT64_USER_INT_GATE, 0);
}

/**
 * @brief Return a pointer to an IDT entry.
 */
idt64_entry_t *idt64_get_entry(uint8_t vector)
{
    return &idt64[vector];
}

/**
 * @brief Execute LIDT with the current IDT base and limit.
 */
void idt64_load(void)
{
    __asm__ __volatile__("lidt %0" :: "m"(idtr64) : "memory");
}

/**
 * @brief Initialize and load the full 64-bit IDT.
 */
void idt64_init(void)
{
    print("[IDT64] Initializing 64-bit IDT...\n");

    /* Zero all entries */
    memset(idt64, 0, sizeof(idt64));

    /* Set up IDTR */
    idtr64.limit = (uint16_t)(sizeof(idt64) - 1);
    idtr64.base  = (uint64_t)(uintptr_t)idt64;

    /* ------------------------------------------------------------------
     * Exception handlers (vectors 0–31)
     *
     * Default: interrupt gate at DPL=0.
     * Special cases are overridden individually below.
     * ------------------------------------------------------------------ */

    /* Pointer table for the 32 exception stubs */
    static void (*const exc_stubs[32])(void) = {
        isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3,
        isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7,
        isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11,
        isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
        isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
        isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
        isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
        isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
    };

    for (int i = 0; i < 32; i++) {
        encode_gate(&idt64[i],
                    (uintptr_t)exc_stubs[i],
                    GDT64_KERNEL_CODE_SEL,
                    IDT64_KERNEL_INT_GATE,
                    0);
    }

    /* ------------------------------------------------------------------
     * Critical exceptions with dedicated handlers + IST stacks
     *
     * Using a separate stack prevents the CPU from triple-faulting when
     * the exception fires on a corrupted or overflowed kernel stack.
     * IST indices must match the TSS fields set up in gdt64.c.
     * ------------------------------------------------------------------ */

    /* #DF Double Fault – IST1 */
    encode_gate(&idt64[EXC_DOUBLE_FAULT],
                (uintptr_t)double_fault_handler,
                GDT64_KERNEL_CODE_SEL,
                IDT64_KERNEL_INT_GATE,
                GDT64_IST_DOUBLE_FAULT);

    /* NMI Non-Maskable Interrupt – IST2 */
    encode_gate(&idt64[EXC_NMI],
                (uintptr_t)nmi_handler,
                GDT64_KERNEL_CODE_SEL,
                IDT64_KERNEL_INT_GATE,
                GDT64_IST_NMI);

    /* #MC Machine Check – IST3 */
    encode_gate(&idt64[EXC_MACHINE_CHECK],
                (uintptr_t)machine_check_handler,
                GDT64_KERNEL_CODE_SEL,
                IDT64_KERNEL_INT_GATE,
                GDT64_IST_MACHINE_CHECK);

    /* #DB Debug – IST4, trap gate so IF is not cleared (single-step works) */
    encode_gate(&idt64[EXC_DEBUG],
                (uintptr_t)isr_stub_1,
                GDT64_KERNEL_CODE_SEL,
                IDT64_KERNEL_TRAP_GATE,
                GDT64_IST_DEBUG);

    /* ------------------------------------------------------------------
     * Breakpoint (#BP, vector 3) – trap gate at DPL=3 so user space can
     * raise it with INT 3.
     * ------------------------------------------------------------------ */
    encode_gate(&idt64[EXC_BREAKPOINT],
                (uintptr_t)isr_stub_3,
                GDT64_KERNEL_CODE_SEL,
                IDT64_USER_INT_GATE,        /* DPL=3, interrupt gate */
                0);

    /* ------------------------------------------------------------------
     * Hardware IRQ stubs – vectors 32–47 (standard PIC mapping)
     * ------------------------------------------------------------------ */
    static void (*const irq_stubs[16])(void) = {
        irq_stub_0,  irq_stub_1,  irq_stub_2,  irq_stub_3,
        irq_stub_4,  irq_stub_5,  irq_stub_6,  irq_stub_7,
        irq_stub_8,  irq_stub_9,  irq_stub_10, irq_stub_11,
        irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15,
    };

    for (int i = 0; i < 16; i++) {
        encode_gate(&idt64[VEC_IRQ_BASE + i],
                    (uintptr_t)irq_stubs[i],
                    GDT64_KERNEL_CODE_SEL,
                    IDT64_KERNEL_INT_GATE,
                    0);
    }

    /* ------------------------------------------------------------------
     * Legacy int 0x80 syscall gate (DPL=3 so user space can invoke it)
     * ------------------------------------------------------------------ */
    encode_gate(&idt64[VEC_SYSCALL],
                (uintptr_t)syscall_handler,
                GDT64_KERNEL_CODE_SEL,
                IDT64_USER_INT_GATE,
                0);

    /* ------------------------------------------------------------------
     * APIC spurious vector (0xFF) – must be a present gate
     * ------------------------------------------------------------------ */
    encode_gate(&idt64[VEC_SPURIOUS],
                (uintptr_t)spurious_irq_handler,
                GDT64_KERNEL_CODE_SEL,
                IDT64_KERNEL_INT_GATE,
                0);

    /* Load the IDTR */
    idt64_load();

    print("[IDT64] IDT loaded: base=0x");
    print_hex((uint32_t)((uint64_t)(uintptr_t)idt64 >> 32));
    print_hex((uint32_t)(uint64_t)(uintptr_t)idt64);
    print(", limit=");
    print_hex(idtr64.limit);
    print("\n");
    print("[IDT64] IST assignments: #DF=IST1, NMI=IST2, #MC=IST3, #DB=IST4\n");
    print("[IDT64] Initialization complete\n");
}

#endif /* __x86_64__ */

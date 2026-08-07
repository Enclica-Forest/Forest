/*
 * Fern - Cross-Architecture Interrupt Management
 * interrupt.c
 *
 * Implements the unified interrupt API declared in interrupt.h.
 * Dispatches to the architecture-specific controller backends:
 *   x86:     PIC/APIC + IDT (via ioapic.c / apic.c / pic_8259a.c)
 *   ARM32:   GICv2 + exception vectors (arm32/gic.c)
 *   AArch64: GICv3 + VBAR_EL1 (aarch64/gic.c)
 *   RISC-V:  PLIC + stvec (riscv64/plic.c)
 */

#include "interrupt.h"
#include "arch.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---- Per-IRQ state ---- */
static arch_irq_handler_t irq_handlers[ARCH_MAX_INTERRUPTS];
static const char *irq_names[ARCH_MAX_INTERRUPTS];
static uint32_t irq_active_count[ARCH_MAX_INTERRUPTS];
static int current_irq = -1;

/* ---- Architecture-specific init (defined per-arch) ---- */
#if ARCH_IS_X86
extern void x86_interrupt_init_idt(void);
extern void x86_pic_init(void);
extern void x86_ioapic_init(void);
extern void x86_ioapic_unmask_irq(uint32_t irq);
extern void x86_ioapic_mask_irq(uint32_t irq);
extern void x86_ioapic_eoi(uint32_t irq);
#elif ARCH_ARM32
extern void arm32_gic_init(void);
#elif ARCH_ARM64
extern void aarch64_gic_init(void);
#elif ARCH_RISCV64
extern void riscv64_plic_init(void);
#endif

/* ---- Public API ---- */

void arch_interrupt_init(void)
{
    memset(irq_handlers, 0, sizeof(irq_handlers));
    memset(irq_names, 0, sizeof(irq_names));
    memset(irq_active_count, 0, sizeof(irq_active_count));
    current_irq = -1;

#if ARCH_IS_X86
    x86_interrupt_init_idt();
    x86_pic_init();
    x86_ioapic_init();
#elif ARCH_ARM32
    arm32_gic_init();
#elif ARCH_ARM64
    aarch64_gic_init();
#elif ARCH_RISCV64
    riscv64_plic_init();
#endif
}

int arch_interrupt_register(uint32_t irq, arch_irq_handler_t handler,
                            const char *name)
{
    if (irq >= ARCH_MAX_INTERRUPTS || !handler)
        return -1;
    if (irq_handlers[irq])
        return -1; /* already registered */

    irq_handlers[irq] = handler;
    irq_names[irq] = name;
    irq_active_count[irq] = 0;
    return 0;
}

void arch_interrupt_unregister(uint32_t irq)
{
    if (irq < ARCH_MAX_INTERRUPTS) {
        irq_handlers[irq] = NULL;
        irq_names[irq] = NULL;
    }
}

void arch_interrupt_enable(uint32_t irq)
{
    if (irq >= ARCH_MAX_INTERRUPTS)
        return;

#if ARCH_IS_X86
    extern void x86_ioapic_unmask_irq(uint32_t irq);
    x86_ioapic_unmask_irq(irq);
#elif ARCH_ARM32
    extern void arm32_gic_enable_irq(uint32_t irq);
    arm32_gic_enable_irq(irq);
#elif ARCH_ARM64
    extern void aarch64_gic_enable_irq(uint32_t irq);
    aarch64_gic_enable_irq(irq);
#elif ARCH_RISCV64
    extern void riscv64_plic_enable_irq(uint32_t irq);
    riscv64_plic_enable_irq(irq);
#endif
}

void arch_interrupt_disable(uint32_t irq)
{
    if (irq >= ARCH_MAX_INTERRUPTS)
        return;

#if ARCH_IS_X86
    extern void x86_ioapic_mask_irq(uint32_t irq);
    x86_ioapic_mask_irq(irq);
#elif ARCH_ARM32
    extern void arm32_gic_disable_irq(uint32_t irq);
    arm32_gic_disable_irq(irq);
#elif ARCH_ARM64
    extern void aarch64_gic_disable_irq(uint32_t irq);
    aarch64_gic_disable_irq(irq);
#elif ARCH_RISCV64
    extern void riscv64_plic_disable_irq(uint32_t irq);
    riscv64_plic_disable_irq(irq);
#endif
}

void arch_interrupt_acknowledge(uint32_t irq)
{
    if (irq >= ARCH_MAX_INTERRUPTS)
        return;

#if ARCH_IS_X86
    extern void x86_ioapic_eoi(uint32_t irq);
    x86_ioapic_eoi(irq);
#elif ARCH_ARM32
    extern void arm32_gic_eoi(uint32_t irq);
    arm32_gic_eoi(irq);
#elif ARCH_ARM64
    extern void aarch64_gic_eoi(uint32_t irq);
    aarch64_gic_eoi(irq);
#elif ARCH_RISCV64
    extern void riscv64_plic_complete_irq(uint32_t irq);
    riscv64_plic_complete_irq(irq);
#endif
}

int32_t arch_interrupt_get_active_irq(void)
{
    return current_irq;
}

bool arch_interrupt_in_context(void)
{
    return current_irq >= 0;
}

/**
 * arch_interrupt_dispatch - Common IRQ dispatch entry point.
 *
 * Called from each architecture's ISR stub after identifying the IRQ number.
 * Looks up the registered handler, invokes it, and acknowledges the interrupt.
 *
 * @irq: Hardware IRQ number (architecture-specific numbering).
 */
void arch_interrupt_dispatch(uint32_t irq)
{
    if (irq >= ARCH_MAX_INTERRUPTS)
        return;

    int32_t prev_irq = current_irq;
    current_irq = (int32_t)irq;
    irq_active_count[irq]++;

    if (irq_handlers[irq])
        irq_handlers[irq](NULL);

    arch_interrupt_acknowledge(irq);
    current_irq = prev_irq;
}

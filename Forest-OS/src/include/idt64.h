/**
 * @file idt64.h
 * @brief 64-bit Interrupt Descriptor Table Interface
 *
 * Provides the 256-entry x86_64 IDT:
 *   - 16-byte interrupt/trap gate descriptors
 *   - LIDT wrapper
 *   - Gate installation API (interrupt gate, trap gate, IST override)
 *   - Exception vector constants
 *   - SYSCALL/SYSRET MSR setup declaration
 */

#ifndef IDT64_H
#define IDT64_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __x86_64__

/* =========================================================================
 * IDT dimensions
 * ========================================================================= */

#define IDT64_ENTRIES           256         /* Full 256-vector table */
#define IDT64_EXCEPTION_COUNT   32          /* CPU exceptions 0–31 */

/* =========================================================================
 * x86_64 Exception Vectors
 * ========================================================================= */

#define EXC_DIVIDE_ERROR        0
#define EXC_DEBUG               1
#define EXC_NMI                 2
#define EXC_BREAKPOINT          3
#define EXC_OVERFLOW            4
#define EXC_BOUND_RANGE         5
#define EXC_INVALID_OPCODE      6
#define EXC_DEVICE_NOT_AVAIL    7
#define EXC_DOUBLE_FAULT        8
/* 9 = coprocessor segment overrun (legacy, not used on modern CPUs) */
#define EXC_INVALID_TSS         10
#define EXC_SEG_NOT_PRESENT     11
#define EXC_STACK_FAULT         12
#define EXC_GENERAL_PROTECTION  13
#define EXC_PAGE_FAULT          14
/* 15 = reserved */
#define EXC_X87_FP_ERROR        16
#define EXC_ALIGNMENT_CHECK     17
#define EXC_MACHINE_CHECK       18
#define EXC_SIMD_FP_ERROR       19
#define EXC_VIRT_EXCEPTION      20
#define EXC_CTRL_PROTECTION     21
/* 22–27 = reserved */
#define EXC_HYPERVISOR_INJ      28
#define EXC_VMM_COMM            29
#define EXC_SECURITY            30
/* 31 = reserved */

/* Vectors 32–255: hardware IRQs and software interrupts */
#define VEC_IRQ_BASE            32
#define VEC_SYSCALL             0x80        /* Legacy int 0x80 syscall gate */
#define VEC_SPURIOUS            0xFF        /* APIC spurious vector */

/* =========================================================================
 * Gate type / attribute byte encoding
 *
 * Byte layout:  [P | DPL(2) | 0 | TYPE(4)]
 *   P   = Present
 *   DPL = Descriptor Privilege Level (0 = kernel, 3 = user)
 *   TYPE: 0xE = 64-bit interrupt gate (clears IF on entry)
 *         0xF = 64-bit trap gate      (preserves IF on entry)
 * ========================================================================= */

#define IDT64_ATTR_PRESENT      (1u << 7)
#define IDT64_ATTR_DPL_KERNEL   (0u << 5)
#define IDT64_ATTR_DPL_USER     (3u << 5)
#define IDT64_ATTR_INT_GATE     0x0Eu       /* 64-bit interrupt gate */
#define IDT64_ATTR_TRAP_GATE    0x0Fu       /* 64-bit trap gate */

/* Convenience composites */
#define IDT64_KERNEL_INT_GATE   (IDT64_ATTR_PRESENT | IDT64_ATTR_DPL_KERNEL | IDT64_ATTR_INT_GATE)
#define IDT64_KERNEL_TRAP_GATE  (IDT64_ATTR_PRESENT | IDT64_ATTR_DPL_KERNEL | IDT64_ATTR_TRAP_GATE)
#define IDT64_USER_INT_GATE     (IDT64_ATTR_PRESENT | IDT64_ATTR_DPL_USER   | IDT64_ATTR_INT_GATE)

/* =========================================================================
 * Data structures
 * ========================================================================= */

/**
 * @brief 64-bit IDT gate descriptor (16 bytes, Intel Vol. 3A §6.14.1)
 *
 * Bit layout:
 *   [15:0]   offset_low   (bits 0–15 of handler address)
 *   [31:16]  selector     (code-segment selector)
 *   [34:32]  ist          (IST index, 0 = no IST switch)
 *   [39:35]  zeros
 *   [47:40]  type_attr    (gate type + DPL + Present)
 *   [63:48]  offset_mid   (bits 16–31 of handler address)
 *   [95:64]  offset_high  (bits 32–63 of handler address)
 *   [127:96] reserved     (must be zero)
 */
typedef struct {
    uint16_t offset_low;        /* handler[15:0]  */
    uint16_t selector;          /* code segment   */
    uint8_t  ist;               /* bits[2:0] = IST index; bits[7:3] = 0 */
    uint8_t  type_attr;         /* type + DPL + P */
    uint16_t offset_mid;        /* handler[31:16] */
    uint32_t offset_high;       /* handler[63:32] */
    uint32_t reserved;          /* must be zero   */
} __attribute__((packed)) idt64_entry_t;

/**
 * @brief IDTR pseudo-descriptor used with LIDT / SIDT
 */
typedef struct {
    uint16_t limit;             /* IDT size - 1            */
    uint64_t base;              /* Linear address of IDT   */
} __attribute__((packed)) idtr64_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Fully initialize the 64-bit IDT.
 *
 * Installs stubs for all 256 vectors, applies IST overrides for critical
 * exceptions (#DF, NMI, #MC, #DB), and executes LIDT.
 *
 * Must be called after gdt64_init() because it reads the kernel CS selector
 * from the GDT.
 */
void idt64_init(void);

/**
 * @brief Install or replace a gate entry (full parameter set).
 *
 * @param vector    IDT vector number (0–255).
 * @param handler   Linear address of the handler (assembly stub).
 * @param selector  Code-segment selector (normally GDT64_KERNEL_CODE_SEL).
 * @param type_attr Gate type / attribute byte (use IDT64_KERNEL_INT_GATE etc.).
 * @param ist       IST index (0 = none, 1–7 = use TSS.ISTn stack).
 */
void idt64_set_gate(uint8_t vector, uintptr_t handler,
                    uint16_t selector, uint8_t type_attr, uint8_t ist);

/**
 * @brief Convenience wrapper for idt64_set_gate() that uses the kernel code
 *        selector and lets the caller specify only the vector, handler, gate
 *        type, and IST index.
 *
 * @param vector   IDT vector number (0–255).
 * @param handler  Linear address of the handler stub.
 * @param type     One of IDT64_ATTR_INT_GATE / IDT64_ATTR_TRAP_GATE plus
 *                 optional IDT64_ATTR_DPL_USER and IDT64_ATTR_PRESENT.
 * @param ist      IST index (0 = none, 1–7).
 */
static inline void idt64_set_gate_simple(uint8_t vector, uintptr_t handler,
                                          uint8_t type, uint8_t ist)
{
    idt64_set_gate(vector, handler, GDT64_KERNEL_CODE_SEL,
                   type | IDT64_ATTR_PRESENT, ist);
}

/* =========================================================================
 * 64-bit trap frame
 *
 * Matches the SAVE_CONTEXT_64 layout in src/interrupt_stubs.s so that C
 * dispatchers can read saved registers directly off the stack.  The
 * assembly stubs sub-allocate CONTEXT_SIZE bytes on the stack, store the
 * GP registers in the order below, then push an error code (or zero), the
 * CPU-saved frame (RIP, CS, RFLAGS, RSP, SS), the vector number, and a
 * timestamp.  C code receives a pointer to this struct as its first arg.
 * ========================================================================= */

typedef struct __attribute__((packed)) arch64_trap_frame {
    uint64_t r15;           /* +0x00  */
    uint64_t r14;           /* +0x08  */
    uint64_t r13;           /* +0x10  */
    uint64_t r12;           /* +0x18  */
    uint64_t r11;           /* +0x20  */
    uint64_t r10;           /* +0x28  */
    uint64_t r9;            /* +0x30  */
    uint64_t r8;            /* +0x38  */
    uint64_t rdi;           /* +0x40  */
    uint64_t rsi;           /* +0x48  */
    uint64_t rbp;           /* +0x50  */
    uint64_t rbx;           /* +0x58  */
    uint64_t rdx;           /* +0x60  */
    uint64_t rcx;           /* +0x68  */
    uint64_t rax;           /* +0x70  */
    uint64_t ds;            /* +0x78  */
    uint64_t es;            /* +0x80  */
    uint64_t fs;            /* +0x88  */
    uint64_t gs;            /* +0x90  */
    uint64_t error_code;    /* +0x98  */
    uint64_t rip;           /* +0xA0  CPU-saved return RIP           */
    uint64_t cs;            /* +0xA8  CPU-saved return CS             */
    uint64_t rflags;        /* +0xB0  CPU-saved RFLAGS                */
    uint64_t rsp;           /* +0xB8  CPU-saved return RSP            */
    uint64_t ss;            /* +0xC0  CPU-saved return SS             */
    uint64_t vector;        /* +0xC8  software-pushed vector number   */
    uint64_t timestamp;     /* +0xD0  RDTSC timestamp (latency stat)  */
} arch64_trap_frame_t;

/**
 * @brief Install an interrupt gate (clears IF) at DPL=0.
 *
 * Thin wrapper around idt64_set_gate() for the common kernel-interrupt case.
 */
void idt64_set_kernel_interrupt(uint8_t vector, uintptr_t handler);

/**
 * @brief Install a trap gate (preserves IF) at DPL=0.
 */
void idt64_set_kernel_trap(uint8_t vector, uintptr_t handler);

/**
 * @brief Install an interrupt gate at DPL=3 (accessible from user space).
 *
 * Used for the legacy int 0x80 syscall path and breakpoints.
 */
void idt64_set_user_gate(uint8_t vector, uintptr_t handler);

/**
 * @brief Reload the IDTR from the current IDT table.
 *
 * Useful after SMP AP bringup where each CPU must load the shared IDT.
 */
void idt64_load(void);

/**
 * @brief Return a pointer to the raw IDT entry for a given vector.
 *
 * Allows external code (e.g. IST setup) to tweak individual entries
 * without re-installing the whole gate.
 *
 * @param vector  IDT vector number (0–255).
 * @return Pointer to the gate, or NULL if vector is out of range.
 */
idt64_entry_t *idt64_get_entry(uint8_t vector);

#endif /* __x86_64__ */
#endif /* IDT64_H */

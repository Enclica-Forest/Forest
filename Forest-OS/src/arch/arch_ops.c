/*
 * Fern - Architecture Operations
 * arch_ops.c
 *
 * Implements the architecture-neutral ops declared in arch.h and the
 * platform helpers declared in platform.h.  This file is compiled once
 * per build, with the appropriate ARCH_* and PLATFORM_* macros set by
 * the build system.
 *
 * Functions provided:
 *   arch_init()               - Arch-specific early init dispatch
 *   arch_get_name()           - ASCII arch string
 *   arch_get_page_size()      - Native page size (bytes)
 *   arch_supports_feature()   - Query CPU capability
 *   platform_get_name()       - ASCII platform string
 *   platform_uart_init()      - Board UART init
 *   platform_uart_putc()      - UART character output
 *   platform_uart_getc()      - UART character input
 */

#include "arch.h"
#include "platform.h"

/* =========================================================================
 * Internal helpers (arch-specific init stubs)
 * ========================================================================= */

/* ---- x86_32 ---- */
#if ARCH_X86_32

static void arch_init_x86_32(void)
{
    /*
     * Enable the x87 FPU (CR0.EM = 0, CR0.MP = 1, CR0.NE = 1).
     * The GDT, IDT, and paging are already set up by the assembly entry
     * point; here we just configure features that must be done in C.
     */
    uint32_t cr0 = x86_32_read_cr0();
    cr0 &= ~CR0_EM;   /* clear emulation flag — we have a real FPU */
    cr0 |=  CR0_MP;   /* monitor coprocessor */
    cr0 |=  CR0_NE;   /* native FPU error reporting */
    x86_32_write_cr0(cr0);

    /*
     * Enable OSFXSR and OSXMMEXCPT in CR4 if SSE is present (CPUID.1.EDX).
     * This lets us use FXSAVE/FXRSTOR instead of FSAVE/FRSTOR.
     */
    x86_32_cpuid_result_t cpuid1 = x86_32_cpuid(1, 0);
    if (cpuid1.edx & CPUID1_EDX_FXSR) {
        uint32_t cr4 = x86_32_read_cr4();
        cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
        x86_32_write_cr4(cr4);
    }
}

/* ---- x86_64 ---- */
#elif ARCH_X86_64

static void arch_init_x86_64(void)
{
    /*
     * Verify we are in 64-bit long mode (sanity check).
     * Enable NX bit, SYSCALL, and SWAPGS-required features.
     */
    uint64_t efer = x86_64_rdmsr(MSR64_EFER);
    /* LMA (Long Mode Active) should already be 1 if we got here */
    efer |= EFER_NXE;   /* enable Execute Disable (NX) page attribute */
    x86_64_wrmsr(MSR64_EFER, efer);

    /* Enable FSGSBASE so RDFSBASE/WRFSBASE work from ring-0 */
    uint64_t cr4 = x86_64_read_cr4();
    cr4 |= CR4_64_FSGSBASE;
    cr4 |= CR4_64_OSFXSR | CR4_64_OSXMMEXCPT;  /* SSE support */
    x86_64_write_cr4(cr4);

    /* Optionally enable SMEP + SMAP if the CPU supports them */
    x86_64_cpuid_result_t c7 = x86_64_cpuid(7, 0);
    if (c7.ebx & CPUID7_EBX_SMEP) cr4 |= CR4_64_SMEP;
    if (c7.ebx & CPUID7_EBX_SMAP) cr4 |= CR4_64_SMAP;
    x86_64_write_cr4(cr4);
}

/* ---- ARM 32 ---- */
#elif ARCH_ARM32

static void arch_init_arm32(void)
{
    /*
     * Bring up minimal ARM MMU prerequisites:
     *   - Install the exception vector table (assumed already placed at
     *     PLATFORM_KERNEL_LOAD_PA - 0x1000 or wherever the boot stub put it).
     *   - Set DACR: domain 0 = client (permissions checked).
     *   - Enable I-cache and branch prediction in SCTLR.
     *
     * Full MMU enable is done later in the memory manager after the page
     * tables are populated.
     */

    /* Set domain 0 to client, all others to no-access */
    arm32_write_dacr(DACR_DOMAIN(0, DACR_CLIENT));

    /* Enable I-cache and branch prediction (safe before MMU is on) */
    uint32_t sctlr = arm32_read_sctlr();
    sctlr |= SCTLR_I;   /* instruction cache */
    sctlr |= SCTLR_Z;   /* branch prediction */
    arm32_write_sctlr(sctlr);
    arm32_isb();
}

/* ---- AArch64 ---- */
#elif ARCH_ARM64

static void arch_init_aarch64(void)
{
    /*
     * Enable GICv3 CPU interface system registers before any interrupt
     * setup code runs.
     */
    uint64_t sre = aarch64_icc_sre_el1_read();
    sre |= ICC_SRE_EL1_SRE | ICC_SRE_EL1_DFB | ICC_SRE_EL1_DIB;
    aarch64_icc_sre_el1_write(sre);
    aarch64_isb();

    /*
     * Set a permissive priority mask (all interrupts allowed) and
     * enable GIC Group 1 interrupts at EL1.
     */
    aarch64_icc_pmr_write(0xFF);
    aarch64_icc_bpr1_write(0);
    aarch64_icc_igrpen1_write(ICC_IGRPEN1_ENABLE);
    aarch64_isb();

    /* Enable I-cache and data cache in SCTLR_EL1 */
    uint64_t sctlr = aarch64_read_sctlr_el1();
    sctlr |= SCTLR_EL1_I | SCTLR_EL1_C;
    aarch64_write_sctlr_el1(sctlr);
    aarch64_isb();
}

#endif /* arch init stubs */

/* =========================================================================
 * 1. arch_init
 * ========================================================================= */

void arch_init(void)
{
#if ARCH_X86_32
    arch_init_x86_32();
#elif ARCH_X86_64
    arch_init_x86_64();
#elif ARCH_ARM32
    arch_init_arm32();
#elif ARCH_ARM64
    arch_init_aarch64();
#endif
}

/* =========================================================================
 * 2. arch_get_name
 * ========================================================================= */

const char *arch_get_name(void)
{
    return ARCH_NAME;
}

/* =========================================================================
 * 3. arch_get_page_size
 * ========================================================================= */

size_t arch_get_page_size(void)
{
    return 4096;
}

/* =========================================================================
 * 4. arch_supports_feature
 *
 * Feature detection is performed at most once and the result cached in a
 * static variable to avoid repeated CPUID / system-register reads.
 * ========================================================================= */

/*
 * We represent the cached feature set as a bitmask of ARCH_FEAT_* bits.
 * 0 = not yet queried.  We use a separate "queried" flag because a
 * feature mask of 0 is a valid (if unlikely) result.
 */
static unsigned int cached_features = 0;
static int          features_detected = 0;

#if ARCH_X86_32 || ARCH_X86_64

static unsigned int detect_features_x86(void)
{
    unsigned int f = 0;

#if ARCH_X86_32
    x86_32_cpuid_result_t c1 = x86_32_cpuid(1, 0);
    x86_32_cpuid_result_t c7 = x86_32_cpuid(7, 0);
#else
    x86_64_cpuid_result_t c1 = x86_64_cpuid(1, 0);
    x86_64_cpuid_result_t c7 = x86_64_cpuid(7, 0);
#endif

    /* FPU */
    if (c1.edx & CPUID1_EDX_FPU)
        f |= ARCH_FEAT_FPU;

    /* SIMD (SSE2 as a baseline) */
    if (c1.edx & CPUID1_EDX_SSE2)
        f |= ARCH_FEAT_SIMD;

    /* AVX-512 (x86-64 only; leaf 7) */
#if ARCH_X86_64
    if (c7.ebx & CPUID7_EBX_AVX512F)
        f |= ARCH_FEAT_AVX512;
    /* TSX */
    if ((c7.ebx & CPUID7_EBX_TSX_HLE) || (c7.ebx & CPUID7_EBX_TSX_RTM))
        f |= ARCH_FEAT_TSX;
#endif

    /* MMU: always present on x86 protected mode */
    f |= ARCH_FEAT_MMU;

    /* SMP: CPUID.1.EDX[HTT] or APIC present implies potential SMP */
    if (c1.edx & CPUID1_EDX_APIC)
        f |= ARCH_FEAT_SMP;

    /* 64-bit atomics: CMPXCHG8B (EDX bit 8) or native 64-bit */
#if ARCH_X86_64
    f |= ARCH_FEAT_ATOMIC64;
#else
    /* CPUID.1.EDX[8] = CMPXCHG8B */
    if (c1.edx & (1U << 8))
        f |= ARCH_FEAT_ATOMIC64;
#endif

    /* AES-NI (ECX bit 25 of leaf 1) */
    if (c1.ecx & CPUID1_ECX_AES)
        f |= ARCH_FEAT_CRYPTO;

    /* Hardware virtualisation: check VMX (Intel) via ECX bit 5 */
    if (c1.ecx & (1U << 5))
        f |= ARCH_FEAT_VIRT;

    /* Unaligned memory access is efficient on modern x86 */
    f |= ARCH_FEAT_UNALIGNED_MEM;

    return f;
}

#elif ARCH_ARM32

static unsigned int detect_features_arm32(void)
{
    unsigned int f = 0;

    /* ARM MMU is always present on Cortex-A */
    f |= ARCH_FEAT_MMU;

    /* SMP: MPIDR.U == 0 means it is part of an MP cluster */
    uint32_t mpidr = arm32_read_mpidr();
    if (!(mpidr & (1U << 30)))  /* bit 30 = Uniprocessor flag */
        f |= ARCH_FEAT_SMP;

    /* FPU / VFP: ID_ISAR0 coprocessor support */
    uint32_t isar0 = ARM32_MRC(c0, 0, c2, 0);
    if (isar0 & 0xF0000000UL)   /* bits 31:28 = divide instructions */
        f |= ARCH_FEAT_FPU;

    /* NEON: MVFR1 bit 7:4 = Advanced SIMD */
    uint32_t mvfr1 = ARM32_MRC(c10, 7, c8, 1);
    if ((mvfr1 >> 4) & 0xF)
        f |= ARCH_FEAT_SIMD | ARCH_FEAT_NEON;

    /* Thumb-2 is always available on ARMv7-A */
    f |= ARCH_FEAT_THUMB;

    /* 64-bit atomics via LDREXD/STREXD (ARMv7 guaranteed) */
    f |= ARCH_FEAT_ATOMIC64;

    /* Unaligned access: SCTLR.A = 0 (not enforced) → unaligned OK */
    f |= ARCH_FEAT_UNALIGNED_MEM;

    return f;
}

#elif ARCH_ARM64

static unsigned int detect_features_aarch64(void)
{
    unsigned int f = 0;

    /* MMU always present on AArch64 */
    f |= ARCH_FEAT_MMU;

    /* 64-bit atomics always present */
    f |= ARCH_FEAT_ATOMIC64;

    /* Unaligned access is efficient on Cortex-A */
    f |= ARCH_FEAT_UNALIGNED_MEM;

    /* SMP: MPIDR.U == 0 */
    uint64_t mpidr = aarch64_read_mpidr_el1();
    if (!(mpidr & (1ULL << 30)))
        f |= ARCH_FEAT_SMP;

    /* FPU and NEON/SIMD: always present on ARMv8-A */
    f |= ARCH_FEAT_FPU | ARCH_FEAT_SIMD | ARCH_FEAT_NEON;

    /* SVE: ID_AA64PFR0_EL1.SVE [35:32] != 0 */
    uint64_t pfr0 = aarch64_read_id_aa64pfr0_el1();
    if ((pfr0 >> 32) & 0xF)
        f |= ARCH_FEAT_SVE;

    /* AES / crypto: ID_AA64ISAR0_EL1.AES [7:4] */
    uint64_t isar0 = aarch64_read_id_aa64isar0_el1();
    if ((isar0 >> 4) & 0xF)
        f |= ARCH_FEAT_CRYPTO;

    /* CRC32: ID_AA64ISAR0_EL1.CRC32 [19:16] */
    if ((isar0 >> 16) & 0xF)
        f |= ARCH_FEAT_CRC32;

    /* Virtualization: EL2 available? ID_AA64PFR0_EL1.EL2 [11:8] */
    if ((pfr0 >> 8) & 0xF)
        f |= ARCH_FEAT_VIRT;

    return f;
}

#endif /* feature detection */

bool arch_supports_feature(uint32_t feature)
{
    if (!features_detected) {
#if ARCH_X86_32 || ARCH_X86_64
        cached_features = detect_features_x86();
#elif ARCH_ARM32
        cached_features = detect_features_arm32();
#elif ARCH_ARM64
        cached_features = detect_features_aarch64();
#endif
        features_detected = 1;
    }
    return !!(cached_features & feature);
}

/* =========================================================================
 * 5. platform_get_name
 * ========================================================================= */

const char *platform_get_name(void)
{
    return PLATFORM_NAME;
}

/* =========================================================================
 * 6. platform_uart_init / putc / getc
 *
 * These are minimal implementations sufficient for early boot debug output.
 * The full drivers (ns16550, PL011) are in the device layer; these stubs
 * use direct MMIO / port I/O without any locking or FIFO management.
 * ========================================================================= */

/* ---- x86: 16550-compatible UART on COM1 (port I/O) ---- */
#if PLATFORM_QEMU_X86

#define UART_THR   0x00   /* Transmitter Holding Buffer (write) */
#define UART_RBR   0x00   /* Receiver Buffer Register   (read)  */
#define UART_IER   0x01   /* Interrupt Enable */
#define UART_FCR   0x02   /* FIFO Control */
#define UART_LCR   0x03   /* Line Control */
#define UART_MCR   0x04   /* Modem Control */
#define UART_LSR   0x05   /* Line Status */
#define UART_DLL   0x00   /* Divisor Latch Low  (LCR.DLAB=1) */
#define UART_DLH   0x01   /* Divisor Latch High (LCR.DLAB=1) */

#define UART_LSR_DR    (1U << 0)   /* Data Ready */
#define UART_LSR_THRE  (1U << 5)   /* Transmit Holding Register Empty */

static inline uint8_t uart_inb(uint8_t reg)
{
    return x86_32_inb((uint16_t)(PLATFORM_UART0_PORT + reg));
}
static inline void uart_outb(uint8_t reg, uint8_t val)
{
    x86_32_outb((uint16_t)(PLATFORM_UART0_PORT + reg), val);
}

void platform_uart_init(void)
{
    uart_outb(UART_IER, 0x00);          /* Disable all interrupts */
    uart_outb(UART_LCR, 0x80);          /* Enable DLAB */
    uart_outb(UART_DLL, 0x03);          /* 38400 baud (divisor = 3) */
    uart_outb(UART_DLH, 0x00);
    uart_outb(UART_LCR, 0x03);          /* 8N1, DLAB=0 */
    uart_outb(UART_FCR, 0xC7);          /* Enable FIFO, clear, 14-byte threshold */
    uart_outb(UART_MCR, 0x0B);          /* DTR + RTS + OUT2 */
}

void platform_uart_putc(char c)
{
    /* Emit LF as CRLF for serial terminals */
    if (c == '\n')
        platform_uart_putc('\r');
    while (!(uart_inb(UART_LSR) & UART_LSR_THRE))
        arch_cpu_relax();
    uart_outb(UART_THR, (uint8_t)c);
}

char platform_uart_getc(void)
{
    while (!(uart_inb(UART_LSR) & UART_LSR_DR))
        arch_cpu_relax();
    return (char)uart_inb(UART_RBR);
}

/* ---- QEMU ARM virt / Raspberry Pi: PL011 UART ---- */
#elif PLATFORM_QEMU_ARM || PLATFORM_RASPI3 || PLATFORM_RASPI4

/*
 * PL011 register offsets (in bytes from the MMIO base).
 * The base is defined as PLATFORM_UART0_MMIO_BASE in platform.h.
 */
#define PL011_DR     0x000   /* Data Register */
#define PL011_RSR    0x004   /* Receive Status / Error Clear */
#define PL011_FR     0x018   /* Flag Register */
#define PL011_IBRD   0x024   /* Integer Baud Rate Divisor */
#define PL011_FBRD   0x028   /* Fractional Baud Rate Divisor */
#define PL011_LCR_H  0x02C   /* Line Control */
#define PL011_CR     0x030   /* Control Register */
#define PL011_IMSC   0x038   /* Interrupt Mask Set/Clear */
#define PL011_ICR    0x044   /* Interrupt Clear Register */

#define PL011_FR_TXFF  (1U <<  5)  /* Transmit FIFO Full */
#define PL011_FR_RXFE  (1U <<  4)  /* Receive FIFO Empty */
#define PL011_FR_BUSY  (1U <<  3)  /* UART Busy */

#define PL011_LCR_FEN  (1U <<  4)  /* FIFO Enable */
#define PL011_LCR_WLEN8 (3U <<  5) /* 8-bit word length */

#define PL011_CR_UARTEN (1U <<  0) /* UART Enable */
#define PL011_CR_TXE    (1U <<  8) /* Transmit Enable */
#define PL011_CR_RXE    (1U <<  9) /* Receive Enable */

/* Cast MMIO base to volatile uint32_t pointer */
#define PL011_REG(offset) \
    (*((volatile uint32_t *)((uint8_t *)(uintptr_t)PLATFORM_UART0_MMIO_BASE + (offset))))

void platform_uart_init(void)
{
    /* Disable UART */
    PL011_REG(PL011_CR) = 0;

    /* Wait for any current transmission to finish */
    while (PL011_REG(PL011_FR) & PL011_FR_BUSY)
        arch_cpu_relax();

    /* Clear all pending interrupts */
    PL011_REG(PL011_ICR) = 0x7FF;

    /*
     * Set baud rate to 115200.
     * UART clock is assumed to be 48 MHz (QEMU virt / RPi).
     *   Divisor = 48000000 / (16 * 115200) = 26.041...
     *   IBRD = 26, FBRD = round(0.041... * 64) = 3
     */
    PL011_REG(PL011_IBRD) = 26;
    PL011_REG(PL011_FBRD) = 3;

    /* 8-bit, no parity, 1 stop, FIFO enabled */
    PL011_REG(PL011_LCR_H) = PL011_LCR_FEN | PL011_LCR_WLEN8;

    /* Mask all interrupts */
    PL011_REG(PL011_IMSC) = 0;

    /* Enable UART, TX, RX */
    PL011_REG(PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
}

void platform_uart_putc(char c)
{
    if (c == '\n')
        platform_uart_putc('\r');
    while (PL011_REG(PL011_FR) & PL011_FR_TXFF)
        arch_cpu_relax();
    PL011_REG(PL011_DR) = (uint32_t)(uint8_t)c;
}

char platform_uart_getc(void)
{
    while (PL011_REG(PL011_FR) & PL011_FR_RXFE)
        arch_cpu_relax();
    return (char)(uint8_t)(PL011_REG(PL011_DR) & 0xFF);
}

#endif /* platform UART */

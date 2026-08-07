/*
 * Fern - Platform / Board Detection Header
 * platform.h
 *
 * Provides compile-time and run-time platform identification, board-specific
 * MMIO base addresses, and per-platform memory layouts.
 *
 * Supported platforms:
 *   PLATFORM_QEMU_X86   - QEMU Standard PC (i440FX) — x86_32 / x86_64
 *   PLATFORM_QEMU_ARM   - QEMU versatilepb / virt   — arm32
 *   PLATFORM_RASPI3     - Raspberry Pi 3 (BCM2837)  — arm32 / aarch64
 *   PLATFORM_RASPI4     - Raspberry Pi 4 (BCM2711)  — aarch64
 *
 * Usage:
 *   Select a platform at build time by defining one of:
 *     -DPLATFORM_QEMU_X86
 *     -DPLATFORM_QEMU_ARM
 *     -DPLATFORM_RASPI3
 *     -DPLATFORM_RASPI4
 *
 *   If none is defined, the best guess is derived from the target arch.
 */

#ifndef FOREST_PLATFORM_H
#define FOREST_PLATFORM_H

#include "arch.h"

/* =========================================================================
 * 1. Platform selection / auto-detection
 * ========================================================================= */

/* Explicit flags take priority; at most one may be set. */
#if defined(PLATFORM_QEMU_X86) + defined(PLATFORM_QEMU_ARM) + \
    defined(PLATFORM_RASPI3)   + defined(PLATFORM_RASPI4)    > 1
#error "Only one PLATFORM_* macro may be defined at a time."
#endif

/* Auto-detect if nothing was specified */
#if !defined(PLATFORM_QEMU_X86) && !defined(PLATFORM_QEMU_ARM) && \
    !defined(PLATFORM_RASPI3)   && !defined(PLATFORM_RASPI4)

#   if ARCH_X86_32 || ARCH_X86_64
#       define PLATFORM_QEMU_X86  1
#   elif ARCH_ARM32
#       define PLATFORM_QEMU_ARM  1
#   elif ARCH_ARM64
#       define PLATFORM_RASPI4    1
#   else
#       error "Fern: cannot auto-detect platform for this architecture."
#   endif
#endif

/* Ensure each macro is defined (as 0) so code can use #if without errors */
#ifndef PLATFORM_QEMU_X86
#   define PLATFORM_QEMU_X86  0
#endif
#ifndef PLATFORM_QEMU_ARM
#   define PLATFORM_QEMU_ARM  0
#endif
#ifndef PLATFORM_RASPI3
#   define PLATFORM_RASPI3    0
#endif
#ifndef PLATFORM_RASPI4
#   define PLATFORM_RASPI4    0
#endif

/* Convenience groups */
#define PLATFORM_IS_X86   (PLATFORM_QEMU_X86)
#define PLATFORM_IS_QEMU  (PLATFORM_QEMU_X86 || PLATFORM_QEMU_ARM)
#define PLATFORM_IS_RASPI (PLATFORM_RASPI3   || PLATFORM_RASPI4)
#define PLATFORM_IS_ARM   (PLATFORM_QEMU_ARM || PLATFORM_IS_RASPI)

/* =========================================================================
 * 2. Platform name string
 * ========================================================================= */

#if   PLATFORM_QEMU_X86
#   define PLATFORM_NAME   "qemu-x86"
#elif PLATFORM_QEMU_ARM
#   define PLATFORM_NAME   "qemu-arm"
#elif PLATFORM_RASPI3
#   define PLATFORM_NAME   "raspi3"
#elif PLATFORM_RASPI4
#   define PLATFORM_NAME   "raspi4"
#endif

/* =========================================================================
 * 3. MMIO Base Addresses
 *
 * All addresses are physical (before MMU is enabled, or in the identity /
 * device MMIO window after it is enabled).
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * QEMU x86 (i440FX / Q35 PC)
 * The standard PC I/O address space is sparse; important MMIO regions are:
 *   ISA I/O ports: 0x000 - 0x3FF  (IN/OUT, not MMIO)
 *   VGA framebuffer MMIO: 0xA0000 - 0xBFFFF
 *   BIOS ROM: 0xE0000 - 0xFFFFF
 *   APIC MMIO: 0xFEE00000 (local), 0xFEC00000 (I/O APIC)
 * We primarily use port I/O for device access on x86.
 * ------------------------------------------------------------------------- */
#if PLATFORM_QEMU_X86

#define PLATFORM_LAPIC_MMIO_BASE     0xFEE00000UL  /* Local APIC */
#define PLATFORM_IOAPIC_MMIO_BASE    0xFEC00000UL  /* I/O APIC */
#define PLATFORM_HPET_MMIO_BASE      0xFED00000UL  /* HPET (if present) */
#define PLATFORM_VGA_FB_BASE         0x000A0000UL  /* VGA framebuffer */
#define PLATFORM_BIOS_ROM_BASE       0x000E0000UL

/* Conventional PCI config space (CF8/CFC port-I/O method) */
#define PLATFORM_PCI_CONFIG_ADDR_PORT  0xCF8U
#define PLATFORM_PCI_CONFIG_DATA_PORT  0xCFCU

/* PCI ECAM base (QEMU places it at 0xB0000000 for Q35, absent for i440FX) */
#define PLATFORM_PCI_ECAM_BASE       0xB0000000UL

#define PLATFORM_UART0_PORT          0x3F8U   /* COM1 (port I/O) */
#define PLATFORM_UART1_PORT          0x2F8U   /* COM2 */

/* =========================================================================
 * QEMU ARM virt machine
 *
 * The QEMU "virt" board exposes:
 *   UART (PL011):   0x09000000
 *   GIC Dist:       0x08000000
 *   GIC CPU if:     0x08010000
 *   GIC Redist:     0x080A0000  (GICv3)
 *   Virtio-MMIO:    0x0A000000 - 0x0A003FFF  (32 slots × 512 B)
 *   Platform flash: 0x00000000 - 0x07FFFFFF
 *   RAM:            0x40000000 - ...
 * ========================================================================= */
#elif PLATFORM_QEMU_ARM

#define PLATFORM_UART0_MMIO_BASE     0x09000000UL  /* PL011 UART0 */
#define PLATFORM_UART1_MMIO_BASE     0x09040000UL  /* PL011 UART1 (if present) */

#define PLATFORM_GIC_DIST_BASE       0x08000000UL
#define PLATFORM_GIC_CPU_BASE        0x08010000UL  /* GICv2 CPU interface */
#define PLATFORM_GIC_VCPU_BASE       0x08020000UL
#define PLATFORM_GIC_REDIST_BASE     0x080A0000UL  /* GICv3 redistributor */

#define PLATFORM_VIRTIO_MMIO_BASE    0x0A000000UL
#define PLATFORM_VIRTIO_MMIO_SIZE    0x00000200UL  /* 512 bytes per device */
#define PLATFORM_VIRTIO_MMIO_COUNT   32

#define PLATFORM_PCI_ECAM_BASE       0x3F000000UL
#define PLATFORM_PCI_MEM_BASE        0x10000000UL

/* =========================================================================
 * Raspberry Pi 3 (BCM2837 SoC)
 *
 * Peripheral base: 0x3F000000 (mapped from VideoCore bus 0x7E000000)
 * GIC-400 base:    0x40000000 (ARM-local peripherals)
 * RAM:             0x00000000 - 0x3EFFFFFF (up to ~1 GB usable)
 * ========================================================================= */
#elif PLATFORM_RASPI3

#define PLATFORM_PERIPH_BASE         0x3F000000UL

#define PLATFORM_UART0_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x201000UL) /* PL011 */
#define PLATFORM_UART1_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x215000UL) /* Mini UART */

#define PLATFORM_GPIO_BASE           (PLATFORM_PERIPH_BASE + 0x200000UL)
#define PLATFORM_SPI0_BASE           (PLATFORM_PERIPH_BASE + 0x204000UL)
#define PLATFORM_I2C0_BASE           (PLATFORM_PERIPH_BASE + 0x205000UL)
#define PLATFORM_I2C1_BASE           (PLATFORM_PERIPH_BASE + 0x804000UL)
#define PLATFORM_PWM_BASE            (PLATFORM_PERIPH_BASE + 0x20C000UL)
#define PLATFORM_DMA_BASE            (PLATFORM_PERIPH_BASE + 0x007000UL)
#define PLATFORM_IRQ_BASE            (PLATFORM_PERIPH_BASE + 0x00B200UL)
#define PLATFORM_TIMER_BASE          (PLATFORM_PERIPH_BASE + 0x003000UL) /* BCM system timer */
#define PLATFORM_MBOX_BASE           (PLATFORM_PERIPH_BASE + 0x00B880UL) /* Mailbox */
#define PLATFORM_USB_BASE            (PLATFORM_PERIPH_BASE + 0x980000UL) /* DWC2 OTG */
#define PLATFORM_EMMC_BASE           (PLATFORM_PERIPH_BASE + 0x300000UL) /* EMMC/SD */

/* ARM-local peripherals (Cortex-A53 timers, mailboxes, GIC) */
#define PLATFORM_ARM_LOCAL_BASE      0x40000000UL
#define PLATFORM_GIC_DIST_BASE       (PLATFORM_ARM_LOCAL_BASE + 0x00041000UL)
#define PLATFORM_GIC_CPU_BASE        (PLATFORM_ARM_LOCAL_BASE + 0x00042000UL)

#define PLATFORM_MBOX_READ           (PLATFORM_MBOX_BASE + 0x00UL)
#define PLATFORM_MBOX_WRITE          (PLATFORM_MBOX_BASE + 0x20UL)
#define PLATFORM_MBOX_STATUS         (PLATFORM_MBOX_BASE + 0x18UL)
#define PLATFORM_MBOX_FULL           (1UL << 31)
#define PLATFORM_MBOX_EMPTY          (1UL << 30)

/* =========================================================================
 * Raspberry Pi 4 (BCM2711 SoC)
 *
 * Peripheral base: 0xFE000000 (AXI bus; VideoCore bus 0x7E000000)
 * GIC-400 base:    0xFF840000
 * RAM:             0x00000000 - up to 8 GB (mapped in stages)
 * ========================================================================= */
#elif PLATFORM_RASPI4

#define PLATFORM_PERIPH_BASE         0xFE000000UL

#define PLATFORM_UART0_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x201000UL) /* PL011 */
#define PLATFORM_UART1_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x215000UL) /* Mini UART */
#define PLATFORM_UART2_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x201400UL)
#define PLATFORM_UART3_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x201600UL)
#define PLATFORM_UART4_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x201800UL)
#define PLATFORM_UART5_MMIO_BASE     (PLATFORM_PERIPH_BASE + 0x201A00UL)

#define PLATFORM_GPIO_BASE           (PLATFORM_PERIPH_BASE + 0x200000UL)
#define PLATFORM_SPI0_BASE           (PLATFORM_PERIPH_BASE + 0x204000UL)
#define PLATFORM_I2C0_BASE           (PLATFORM_PERIPH_BASE + 0x205000UL)
#define PLATFORM_I2C1_BASE           (PLATFORM_PERIPH_BASE + 0x804000UL)
#define PLATFORM_DMA_BASE            (PLATFORM_PERIPH_BASE + 0x007000UL)
#define PLATFORM_TIMER_BASE          (PLATFORM_PERIPH_BASE + 0x003000UL)
#define PLATFORM_MBOX_BASE           (PLATFORM_PERIPH_BASE + 0x00B880UL)
#define PLATFORM_USB_BASE            (PLATFORM_PERIPH_BASE + 0x980000UL) /* xHCI */
#define PLATFORM_EMMC_BASE           (PLATFORM_PERIPH_BASE + 0x300000UL)
#define PLATFORM_EMMC2_BASE          (PLATFORM_PERIPH_BASE + 0x340000UL)
#define PLATFORM_PCIE_BASE           0x600000000ULL   /* 64-bit PCIe MMIO */

/* GIC-400 (BCM2711 uses GICv2) */
#define PLATFORM_GIC_DIST_BASE       0xFF841000UL
#define PLATFORM_GIC_CPU_BASE        0xFF842000UL
#define PLATFORM_GIC_VCPU_BASE       0xFF844000UL
#define PLATFORM_GIC_REDIST_BASE     0xFF846000UL

/* ARM-local peripherals */
#define PLATFORM_ARM_LOCAL_BASE      0xFF800000UL

#define PLATFORM_MBOX_READ           (PLATFORM_MBOX_BASE + 0x00UL)
#define PLATFORM_MBOX_WRITE          (PLATFORM_MBOX_BASE + 0x20UL)
#define PLATFORM_MBOX_STATUS         (PLATFORM_MBOX_BASE + 0x18UL)
#define PLATFORM_MBOX_FULL           (1UL << 31)
#define PLATFORM_MBOX_EMPTY          (1UL << 30)

#endif /* platform MMIO */

/* =========================================================================
 * 4. Memory layout per platform
 *
 * These constants define the physical address space seen by the kernel.
 * Virtual layout is a separate concern handled in mm_layout.h / vmm.
 * ========================================================================= */

#if PLATFORM_QEMU_X86

/* x86 BIOS places the first 640 KB as conventional RAM; 1 MB gap follows */
#define PLATFORM_RAM_START           0x00100000UL   /* 1 MB — where the kernel loads */
#define PLATFORM_RAM_END_MAX         0xFFFFF000UL   /* Upper bound of 32-bit PA */
#define PLATFORM_KERNEL_LOAD_PA      0x00100000UL   /* 1 MB physical load address */
#define PLATFORM_INITRD_PA           0x00800000UL   /* 8 MB — initrd default */

/* Device MMIO window in the identity-mapped VA space (64-bit kernel) */
#define PLATFORM_DEVMEM_VIRT_BASE    0xFFFFFFFF80000000ULL

#elif PLATFORM_QEMU_ARM

#define PLATFORM_RAM_START           0x40000000UL
#define PLATFORM_RAM_END_MAX         0xC0000000UL   /* Up to 2 GB on virt */
#define PLATFORM_KERNEL_LOAD_PA      0x40080000UL   /* 512 KB into RAM */
#define PLATFORM_INITRD_PA           0x42000000UL

#elif PLATFORM_RASPI3

#define PLATFORM_RAM_START           0x00000000UL
#define PLATFORM_RAM_END_MAX         0x3C000000UL   /* 960 MB usable (GPU takes rest) */
#define PLATFORM_KERNEL_LOAD_PA      0x00080000UL   /* 512 KB load address */
#define PLATFORM_INITRD_PA           0x08000000UL   /* 128 MB */

#elif PLATFORM_RASPI4

#define PLATFORM_RAM_START           0x00000000UL
#define PLATFORM_RAM_END_MAX         0xFC000000UL   /* Up to ~4 GB (4B model) */
#define PLATFORM_KERNEL_LOAD_PA      0x00080000UL
#define PLATFORM_INITRD_PA           0x08000000UL

#endif /* memory layout */

/* =========================================================================
 * 5. Common derived constants
 * ========================================================================= */

#define PLATFORM_PAGE_SIZE   4096U
#define PLATFORM_PAGE_SHIFT  12U
#define PLATFORM_PAGE_MASK   (~(PLATFORM_PAGE_SIZE - 1))

/* Stack size for early boot (before scheduler) */
#define PLATFORM_BOOT_STACK_SIZE   (16U * 1024U)   /* 16 KB */

/* =========================================================================
 * 6. Run-time platform query (implemented in arch_ops.c)
 * ========================================================================= */

/**
 * platform_get_name - Return a short ASCII string identifying the platform.
 *
 * Returns one of: "qemu-x86", "qemu-arm", "raspi3", "raspi4".
 */
const char *platform_get_name(void);

/**
 * platform_uart_init - Initialise the primary UART for early boot output.
 *
 * Must be called before any printk / debug output.
 */
void platform_uart_init(void);

/**
 * platform_uart_putc - Write a single character to the primary UART.
 *
 * Busy-waits for the transmit FIFO to be ready.
 */
void platform_uart_putc(char c);

/**
 * platform_uart_getc - Read a character from the primary UART.
 *
 * Blocks until a character is available.  Returns the character read.
 */
char platform_uart_getc(void);

#endif /* FOREST_PLATFORM_H */

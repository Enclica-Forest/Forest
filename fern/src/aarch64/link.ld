/*
 * Forest OS - AArch64 Linker Script
 * Target: QEMU -machine virt -cpu cortex-a53
 *
 * Kernel loads at physical 0x40080000 (QEMU DRAM starts at 0x40000000)
 * but is linked at 0xFFFF000000080000 (higher-half virtual address).
 *
 * The boot stub runs with the MMU off at the physical address, so
 * .text.boot must be at the physical address too.  After the MMU is
 * enabled the kernel jumps to the higher-half symbols.
 *
 * Memory layout:
 *   PA 0x40080000 → VA 0xFFFF000000080000  kernel image start
 *
 * QEMU -machine virt memory map (relevant portions):
 *   0x00000000 – 0x07FFFFFF  MMIO (GIC, UART, etc.)
 *   0x40000000 – 0xBFFFFFFF  RAM (up to 2 GB; default 128 MB)
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* Physical load address (QEMU DRAM offset + 512KB) */
PHYS_BASE = 0x40080000;

/* Virtual kernel base (higher half, bit-63 set) */
KERN_VIRT_BASE = 0xFFFF000000080000;

/* Offset to add to a physical address to get the kernel virtual address */
VIRT_TO_PHYS = KERN_VIRT_BASE - PHYS_BASE;

SECTIONS
{
    /* ---------------------------------------------------------------- */
    /* Boot stub – runs with MMU off, so linked at physical address.    */
    /* Placed first so it is at exactly PHYS_BASE.                      */
    /* ---------------------------------------------------------------- */
    . = PHYS_BASE;
    _phys_start = .;

    .text.boot : AT(PHYS_BASE) {
        KEEP(*(.text.boot))
    }

    /* ---------------------------------------------------------------- */
    /* Exception vector table – must be 2KB-aligned.                    */
    /* Placed at the physical address; VBAR_EL1 is set to its VA.       */
    /* ---------------------------------------------------------------- */
    . = ALIGN(2048);
    .text.vectors : {
        KEEP(*(.text.vectors))
    }

    /* ---------------------------------------------------------------- */
    /* Rest of the kernel image – linked at the virtual (higher-half)   */
    /* address but loaded at the physical address.                       */
    /* ---------------------------------------------------------------- */
    . += VIRT_TO_PHYS;  /* Switch to virtual addressing */

    . = ALIGN(4096);
    _text_start = .;

    .text : AT(ADDR(.text) - VIRT_TO_PHYS) {
        *(.text .text.* .gnu.linkonce.t.*)
    }

    . = ALIGN(4096);
    _text_end = .;

    /* ---------------------------------------------------------------- */
    /* Read-only data                                                    */
    /* ---------------------------------------------------------------- */
    .rodata : AT(ADDR(.rodata) - VIRT_TO_PHYS) {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    }

    . = ALIGN(4096);

    /* ---------------------------------------------------------------- */
    /* Initialisation arrays (ctors / dtors)                            */
    /* ---------------------------------------------------------------- */
    .init_array : AT(ADDR(.init_array) - VIRT_TO_PHYS) {
        __init_array_start = .;
        KEEP(*(.init_array SORT(.init_array.*)))
        __init_array_end = .;
    }

    .fini_array : AT(ADDR(.fini_array) - VIRT_TO_PHYS) {
        __fini_array_start = .;
        KEEP(*(.fini_array SORT(.fini_array.*)))
        __fini_array_end = .;
    }

    /* ---------------------------------------------------------------- */
    /* Writable data                                                     */
    /* ---------------------------------------------------------------- */
    . = ALIGN(4096);
    _data_start = .;

    .data : AT(ADDR(.data) - VIRT_TO_PHYS) {
        *(.data .data.* .gnu.linkonce.d.*)
    }

    /* ---------------------------------------------------------------- */
    /* BSS (zero-initialised) – zeroed by boot.S before kernel_main()  */
    /* ---------------------------------------------------------------- */
    . = ALIGN(4096);
    _bss_start = .;

    .bss : AT(ADDR(.bss) - VIRT_TO_PHYS) {
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
    }

    . = ALIGN(8);
    _bss_end = .;

    /* ---------------------------------------------------------------- */
    /* Kernel stack (16KB, 16-byte aligned per AArch64 ABI)            */
    /* ---------------------------------------------------------------- */
    . = ALIGN(4096);
    _stack_bottom = .;
    . += 16384;         /* 16 KB */
    . = ALIGN(16);      /* AArch64 ABI requires 16-byte SP alignment   */
    _stack_top = .;

    . = ALIGN(4096);
    _kernel_end = .;

    /* ---------------------------------------------------------------- */
    /* Initrd (ramdisk) placement                                        */
    /* QEMU -machine virt loads initrd at a physical address above the   */
    /* kernel.  With 128 MB RAM starting at PA 0x40000000 and kernel at  */
    /* PA 0x40080000, QEMU places the initrd around PA 0x48000000.      */
    /* These symbols are defined at the virtual address corresponding    */
    /* to that physical location (identity-mapped in early boot).        */
    /* Override with -Wl,--defsym=_initrd_start=0x... at link time.      */
    /* ---------------------------------------------------------------- */
    _initrd_start = 0x48000000;
    _initrd_end   = 0x48000000;

    /* ---------------------------------------------------------------- */
    /* Discard unwanted sections                                        */
    /* ---------------------------------------------------------------- */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.gnu.warning)
        *(.gnu_debuglink)
        *(.gnu.version*)
    }
}

/* Provide kernel image size for use in C code */
_kernel_size = _kernel_end - KERN_VIRT_BASE;

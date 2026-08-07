/*
 * Forest OS - RISC-V 64-bit Linker Script
 * Target: QEMU -machine virt -bios opensbi
 *
 * OpenSBI loads the kernel at 0x80200000 in physical memory.
 * The kernel is linked at a higher-half virtual address.
 *
 * Memory layout:
 *   PA 0x80200000 → VA 0xFFFFFFC080200000  kernel image start
 *
 * QEMU -machine virt RISC-V memory map (relevant portions):
 *   0x00000000 – 0x0000FFFF  ROM (reset vector)
 *   0x00100000 – 0x0FFFFFFF  MMIO (UART, CLINT, etc.)
 *   0x80000000 – 0xBFFFFFFF  DRAM (up to 1 GB; default 128 MB)
 *
 * OpenSBI loads at 0x80000000 (2 MB), so our kernel starts at 0x80200000.
 */

OUTPUT_FORMAT("elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

/* Physical load address (OpenSBI passes here) */
PHYS_BASE = 0x80200000;

/* Virtual kernel base (higher half, SV39 canonical) */
KERN_VIRT_BASE = 0xFFFFFFC080200000;

/* Offset to add to a physical address to get the kernel virtual address */
VIRT_TO_PHYS = KERN_VIRT_BASE - PHYS_BASE;

SECTIONS
{
    /* ---------------------------------------------------------------- */
    /* Boot stub – runs with MMU off, linked at physical address.       */
    /* Placed first so it is at exactly PHYS_BASE.                      */
    /* ---------------------------------------------------------------- */
    . = PHYS_BASE;
    _phys_start = .;

    .text.boot : AT(PHYS_BASE) {
        KEEP(*(.text.boot))
    }

    /* ---------------------------------------------------------------- */
    /* Trap vector table – must be 4-byte aligned (Direct mode).         */
    /* ---------------------------------------------------------------- */
    . = ALIGN(4);
    .text.trap : {
        KEEP(*(.text.trap))
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
    /* Kernel stack (16KB, 16-byte aligned per RISC-V ABI)             */
    /* ---------------------------------------------------------------- */
    . = ALIGN(4096);
    _stack_bottom = .;
    . += 16384;         /* 16 KB */
    . = ALIGN(16);      /* RISC-V ABI requires 16-byte SP alignment   */
    _stack_top = .;

    . = ALIGN(4096);
    _kernel_end = .;

    /* ---------------------------------------------------------------- */
    /* Discard unwanted sections                                        */
    /* ---------------------------------------------------------------- */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.gnu.warning)
        *(.gnu_debuglink)
        *(.gnu.version*)
    }
}

/* Provide kernel image size for use in C code */
_kernel_size = _kernel_end - KERN_VIRT_BASE;

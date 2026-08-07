/*
 * Forest OS - RISC-V 64-bit UEFI Linker Script
 *
 * Produces a PE32+ image loadable by UEFI firmware on RISC-V 64-bit.
 * Position-independent (ImageBase = 0) so UEFI can relocate it.
 *
 * Memory layout (virtual addresses):
 *   0x0000  .uefi_pehdr   PE/COFF header (file offset 0, size 0x200)
 *   0x1000  .text          _start entry + UEFI boot code (file offset 0x200)
 *   ...     .rodata        read-only data
 *   ...     .data          initialised data
 *   ...     .bss           zero-initialised data + 16 KB stack
 *
 * Symbols consumed by uefi_boot.S PE header:
 *   _image_base (0), _text_start (0x1000), _text_size, _bss_size, _image_size
 */

OUTPUT_FORMAT("elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0;
    _image_base = .;

    /* PE/COFF header at VA 0, file offset 0 */
    .uefi_pehdr : {
        KEEP(*(.data.uefi_pehdr))
    }

    /* Align to SectionAlignment (4 KB) for the .text section */
    . = ALIGN(0x1000);
    _text_start = .;

    .text : {
        *(.text .text.*)
    }

    . = ALIGN(0x1000);
    _text_end = .;
    _text_size = _text_end - _text_start;

    .rodata : {
        *(.rodata .rodata.*)
    }

    .init_array : {
        __init_array_start = .;
        KEEP(*(.init_array SORT(.init_array.*)))
        __init_array_end = .;
    }

    .fini_array : {
        __fini_array_start = .;
        KEEP(*(.fini_array SORT(.fini_array.*)))
        __fini_array_end = .;
    }

    . = ALIGN(0x1000);
    _data_start = .;

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(0x1000);
    __bss_start = .;

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(8);
    __bss_end = .;

    _bss_size = __bss_end - __bss_start;

    . = ALIGN(0x1000);
    __stack_bottom = .;
    . += 16384;
    . = ALIGN(16);
    __stack_top = .;

    . = ALIGN(0x1000);
    _image_end = .;

    _image_size = . - _image_base;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.gnu.warning)
        *(.gnu_debuglink)
        *(.gnu.version*)
    }
}

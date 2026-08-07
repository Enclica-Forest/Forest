/* =============================================================================
 * UEFI AArch64 Linker Script for Forest OS
 * =============================================================================
 * Creates an ELF image suitable for conversion to UEFI PE32+ via objcopy.
 * The PE/COFF conversion (--target=efi-app-aarch64) will produce the
 * final binary; .reloc is generated automatically by objcopy.
 * =============================================================================
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x0;
    kernel_start = .;

    .text : ALIGN(4096) {
        _text_start = .;
        *(.text.startup)
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        _text_end = .;
    }

    .rodata : ALIGN(4096) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
        _rodata_end = .;
    }

    .data : ALIGN(4096) {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4096);
        _data_end = .;
    }

    .bss : ALIGN(4096) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    }

    . = ALIGN(4096);
    .stack : {
        _stack_bottom = .;
        . += 16384; /* 16KB stack */
        _stack_top = .;
    }

    kernel_end = .;

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.gnu.warning)
        *(.gnu_debuglink)
        *(.gnu.version*)
    }
}

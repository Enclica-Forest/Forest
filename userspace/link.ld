/* Forest-OS userspace linker script */
ENTRY(_start)

SECTIONS
{
    . = 0x08048000;  /* Classic Linux ELF base address */

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    _end = .;
}

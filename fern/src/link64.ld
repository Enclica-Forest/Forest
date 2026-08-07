/* 64-bit linker script for Forest OS
 *
 * Links the kernel at a high canonical address (0xFFFFFFFF80100000) so
 * user space owns the entire lower half of the virtual address space.
 * The multiboot header and the 32-bit boot trampoline stay in a small
 * low-memory region (VMA == LMA == 0x100000) so GRUB/ForeB can load and
 * jump to them before paging is enabled.  Once long mode + paging are
 * active, the boot stub calls the high-half C entry point (startk).
 *
 * Layout (Virtual -> Physical load address):
 *
 *   Low boot region  (VMA == LMA == 0x00100000):
 *     .multiboot        multiboot1/2 header (first 8 KiB of file)
 *     .text.boot        32-bit trampoline + early long-mode stub
 *     .bss.boot         boot-time page tables, low stacks
 *
 *   High-half kernel  (VMA = 0xFFFFFFFF80100000, LMA continues):
 *     .text             kernel C code
 *     .rodata           read-only data / const tables
 *     .data             initialised data
 *     .eh_frame         unwind tables
 *     .bss              zero-initialised kernel data
 *     .stack            per-CPU boot stack (16 KiB)
 *
 * All sections are 4 KiB-aligned so the page tables can apply distinct
 * permissions (NX on .data/.bss, RO on .rodata, etc.) once paging is
 * refined.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(start)

SECTIONS
{
    /* ------------------------------------------------------------------
     * Low-memory boot region (VMA == LMA == 0x100000)
     * GRUB loads the file here and jumps to `start`.
     * ------------------------------------------------------------------ */
    . = 0x00100000;
    kernel_start = .;

    /* Multiboot header — must appear within the first 32 KiB of the
     * file so GRUB/ForeB can find it. */
    .multiboot ALIGN(8) : {
        KEEP(*(.multiboot))
        KEEP(*(.multiboot.*))
    }

    /* 32-bit boot trampoline + early long-mode stub. */
    .text.boot ALIGN(16) : {
        *(.text.boot)
        *(.text.boot.*)
    }

    /* Boot-time data that must be accessible before paging is enabled
     * (initial PML4/PDPT/PD, low boot stack, GDT for the trampoline). */
    .data.boot ALIGN(16) : {
        *(.data.boot)
        *(.data.boot.*)
    }
    .rodata.boot ALIGN(16) : {
        *(.rodata.boot)
        *(.rodata.boot.*)
    }
    .bss.boot ALIGN(16) : {
        _boot_bss_start = .;
        *(.bss.boot)
        *(.bss.boot.*)
        _boot_bss_end = .;
    }

    /* Physical end of the low boot region.  The high-half sections
     * continue loading contiguously in the file from this address. */
    boot_low_end_phys = .;
    . = ALIGN(4096);
    boot_low_end_phys_aligned = .;

    /* ------------------------------------------------------------------
     * High-half kernel region (VMA = 0xFFFFFFFF80100000)
     * LMA continues from boot_low_end_phys_aligned so the bootloader
     * lays the image out contiguously in physical memory.
     * ------------------------------------------------------------------ */
    . = 0xFFFFFFFF80100000;
    kernel_high_vma = .;

    .text ALIGN(4096) : AT(boot_low_end_phys_aligned) {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4096) : {
        *(.data)
        *(.data.*)
    }

    .eh_frame ALIGN(8) : { *(.eh_frame) }

    .bss ALIGN(4096) : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _bss_end = .;
    }

    . = ALIGN(4096);
    .stack : {
        _stack_bottom = .;
        . += 16384;             /* 16 KiB boot/kernel stack */
        _stack_top = .;
    }

    kernel_end = .;

    /* ------------------------------------------------------------------
     * Discard sections we don't need in a kernel.
     * ------------------------------------------------------------------ */
    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame_hdr)
    }
}

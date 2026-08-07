/*
 * link.ld - ARM32 bare-metal linker script for Forest OS
 *
 * Target: QEMU -machine virt -cpu cortex-a15
 *         (also suitable for Raspberry Pi with adjusted LOAD_ADDRESS)
 *
 * Memory map
 * ----------
 *
 *  0x00000000 – 0x00000FFF  Exception vector table (copied from _vector_table)
 *  0x40000000 – 0x3FFFFFFF  RAM (QEMU virt: 1 GiB starting at 0x40000000)
 *
 * Kernel load address: 0x40008000
 *   Same as the ARM Linux kernel conventional load address.
 *   QEMU U-Boot / direct kernel mode also uses this address.
 *   The lower 32 KB (0x40000000 – 0x40007FFF) are left free for the
 *   ATAGS / FDT blob that the bootloader places there.
 *
 * Section layout (all at LOAD_ADDRESS + offset)
 * -----------------------------------------------
 *  .vectors   – Exception vector table (placed first so it can be copied
 *               to 0x00000000 by reset_handler, or VBAR can point at it)
 *  .text      – Kernel code
 *  .rodata    – Read-only data
 *  .data      – Initialised read-write data
 *  .bss       – Zero-initialised data  (boot.S clears this)
 *  .stack     – Per-mode stacks  (already in .bss via boot.S arrays)
 *
 * Symbol exports used by boot.S
 * --------------------------------
 *  _bss_start, _bss_end     – BSS clear loop bounds
 *  _data_start, _data_end   – RAM destination for .data copy
 *  _data_lma                – ROM/flash source for .data (== _data_start
 *                             when running from RAM, as in QEMU)
 *  kernel_start, kernel_end – Overall image bounds (useful for MMU setup)
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(reset_handler)

/* -----------------------------------------------------------------------
 * Load address: standard ARM bare-metal / Linux kernel convention.
 * Override at link time: ld --defsym LOAD_ADDRESS=0x80008000
 * --------------------------------------------------------------------- */
LOAD_ADDRESS = DEFINED(LOAD_ADDRESS) ? LOAD_ADDRESS : 0x40008000;

MEMORY
{
    /*
     * RAM: QEMU virt machine maps 1 GiB of RAM starting at 0x40000000.
     * We place the kernel at offset 0x8000 within this region.
     *
     * Adjust ORIGIN/LENGTH for real hardware:
     *   Raspberry Pi 3/4: ORIGIN = 0x00080000, LENGTH = 512M (or more)
     */
    RAM (rwx) : ORIGIN = 0x40008000, LENGTH = 128M
}

SECTIONS
{
    . = LOAD_ADDRESS;
    kernel_start = .;

    /* ------------------------------------------------------------------
     * .text.boot – Exception vector table + early boot code
     *
     * boot.S places the vector table in .text.boot so it lands at the
     * very start of the image.  VBAR is pointed here by reset_handler
     * (MCR p15, 0, r0, c12, c0, 0) — no copy to 0x00000000 needed.
     *
     * This section must come first.  KEEP() prevents --gc-sections from
     * discarding it (no direct C references to the vector labels).
     * ---------------------------------------------------------------- */
    .text.boot : AT(LOAD_ADDRESS)
    {
        KEEP(*(.text.boot))
        KEEP(*(.vectors))       /* also absorb any .vectors fragments    */
    } > RAM

    /* ------------------------------------------------------------------
     * .text – All other executable code
     * ---------------------------------------------------------------- */
    .text : ALIGN(4)
    {
        *(.text*)
        *(.gnu.linkonce.t*)
    } > RAM

    /* ------------------------------------------------------------------
     * .rodata – Read-only data (strings, constants, tables)
     * ---------------------------------------------------------------- */
    .rodata : ALIGN(4)
    {
        *(.rodata*)
        *(.gnu.linkonce.r*)
    } > RAM

    /* ARM exception unwind tables (needed if -funwind-tables is used) */
    .ARM.extab : ALIGN(4) { *(.ARM.extab*) } > RAM
    .ARM.exidx  : ALIGN(4)
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > RAM

    /* ------------------------------------------------------------------
     * .data – Initialised read-write data
     *
     * LMA (load address) and VMA (virtual address) are the same when
     * running directly from RAM (QEMU ELF load).  When running from ROM
     * the linker script would separate them; boot.S handles the copy.
     * ---------------------------------------------------------------- */
    . = ALIGN(4096);        /* page-align the data segment              */
    _data_lma   = LOADADDR(.data);   /* source: where data lives in ROM */
    _data_start = .;

    .data : ALIGN(4)
    {
        *(.data*)
        *(.gnu.linkonce.d*)
    } > RAM

    _data_end = .;

    /* ------------------------------------------------------------------
     * .bss – Zero-initialised data
     *
     * boot.S zeroes [_bss_start, _bss_end) before calling kernel_main().
     * ---------------------------------------------------------------- */
    . = ALIGN(4096);
    _bss_start = .;

    .bss : ALIGN(4)
    {
        *(.bss*)
        *(.gnu.linkonce.b*)
        *(COMMON)
    } > RAM

    . = ALIGN(4);
    _bss_end = .;

    /* ------------------------------------------------------------------
     * Heap start marker – used by a sbrk() / kmalloc() implementation.
     * ---------------------------------------------------------------- */
    . = ALIGN(4096);
    _heap_start = .;

    /* ------------------------------------------------------------------
     * Stack area
     *
     * The per-mode stacks are allocated as .bss arrays in boot.S; the
     * linker absorbs them into the .bss section above.  We define
     * _stack_top as the top of available RAM for the initial SVC stack
     * in case boot.S is replaced with a version that uses this symbol.
     * ---------------------------------------------------------------- */

    kernel_end = .;

    /*
     * _stack_top: place at the very top of the RAM region.
     * 16 KB is the minimum for a bare-metal kernel SVC stack.
     * (boot.S reserves its own static stacks; this symbol is a fallback.)
     */
    _stack_top = ORIGIN(RAM) + LENGTH(RAM);

    /* ------------------------------------------------------------------
     * Discard C++ exception handling and other unneeded sections
     * ---------------------------------------------------------------- */
    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.note*)
        *(.comment)
        *(.gnu.warning)
    }
}

/*
 * Verify the image fits inside the allocated RAM region.
 * The linker emits an error if kernel_end exceeds the RAM limit.
 */
ASSERT(kernel_end <= ORIGIN(RAM) + LENGTH(RAM) - 16384,
       "ERROR: kernel image too large for RAM region (need 16 KB headroom for stack)")

/*
 * Forest OS - AArch64 UEFI PE Entry Point
 *
 * PE/COFF stub for UEFI on AArch64.  The UEFI firmware loads this image
 * as a PE32+ executable and jumps to _start with:
 *   x0 = EFI_HANDLE (ImageHandle)
 *   x1 = EFI_SYSTEM_TABLE*
 *
 * This stub sets up the stack, zeroes BSS, and calls uefi_main_c()
 * which does the real initialisation in C.
 */

    .section ".text.startup", "ax"
    .global _start
    .type   _start, %function

_start:
    /* Zero BSS before touching any C globals */
    adrp    x2, _bss_start
    add     x2, x2, :lo12:_bss_start
    adrp    x3, _bss_end
    add     x3, x3, :lo12:_bss_end
    cmp     x2, x3
    b.ge    .Lbss_done
.Lbss_loop:
    str     xzr, [x2], #8
    cmp     x2, x3
    b.lt    .Lbss_loop
.Lbss_done:

    /* Set up stack (16-byte aligned per AArch64 AAPCS) */
    adrp    x4, _stack_top
    add     x4, x4, :lo12:_stack_top
    mov     sp, x4

    /* x0 = ImageHandle, x1 = SystemTable — already in place */
    bl      uefi_main_c

    /* Should never return; park if it does */
.Lhalt:
    wfi
    b       .Lhalt

    .size _start, . - _start

    .extern _bss_start
    .extern _bss_end
    .extern _stack_top
    .extern uefi_main_c

/*
 * Forest OS - RISC-V 64-bit UEFI PE Entry Point
 *
 * First code executed by UEFI firmware after loading the PE32+ image.
 * Saves firmware arguments (ImageHandle, SystemTable) and hands off
 * to uefi_main_c() in C.
 *
 * Linker symbols consumed from link_uefi.ld:
 *   _image_base, _text_start, _text_size, _bss_size, _image_size
 */

/* ================================================================== */
/* PE/COFF Header for RISC-V 64-bit                                   */
/* Placed at file offset 0 via link_uefi.ld (.data.uefi_pehdr).       */
/* All symbol references are resolved by the linker as RVAs from      */
/* _image_base.                                                       */
/* ================================================================== */
    .section ".data.uefi_pehdr", "aw", %progbits
    .align  8

/* -- DOS Header (64 bytes) ------------------------------------------- */
    .short  0x5A4D
    .space  58
    .long   0x40

/* -- PE Signature "PE\0\0" ------------------------------------------- */
    .long   0x00004550

/* -- IMAGE_FILE_HEADER (20 bytes) ------------------------------------ */
    .short  0x50                /* Machine = IMAGE_FILE_MACHINE_RISCV64  */
    .short  2                   /* NumberOfSections (.uefi_pehdr+.text)  */
    .long   0                   /* TimeDateStamp                         */
    .long   0                   /* PointerToSymbolTable                  */
    .long   0                   /* NumberOfSymbols                       */
    .short  0xF0                /* SizeOfOptionalHeader (240 bytes)      */
    .short  0x22                /* Characteristics = EXEC|LARGE_ADDR    */

/* -- IMAGE_OPTIONAL_HEADER64 (PE32+) --------------------------------- */
    .short  0x20B               /* Magic = PE32+                         */
    .byte   0                   /* MajorLinkerVersion                    */
    .byte   0                   /* MinorLinkerVersion                    */
    .long   _text_size          /* SizeOfCode                            */
    .long   0                   /* SizeOfInitializedData                 */
    .long   _bss_size           /* SizeOfUninitializedData               */
    .long   _text_start - _image_base /* AddressOfEntryPoint (RVA)      */
    .long   _text_start - _image_base /* BaseOfCode (RVA)              */
    .quad   _image_base         /* ImageBase                             */
    .long   0x1000              /* SectionAlignment = 4KB                */
    .long   0x200               /* FileAlignment = 512B                  */
    .short  0                   /* MajorOperatingSystemVersion           */
    .short  0                   /* MinorOperatingSystemVersion           */
    .short  0                   /* MajorImageVersion                     */
    .short  0                   /* MinorImageVersion                     */
    .short  0                   /* MajorSubsystemVersion                 */
    .short  0                   /* MinorSubsystemVersion                 */
    .long   0                   /* Win32VersionValue                     */
    .long   _image_size         /* SizeOfImage                           */
    .long   0x200               /* SizeOfHeaders                         */
    .long   0                   /* CheckSum (unused for UEFI)            */
    .short  10                  /* Subsystem = EFI_APPLICATION            */
    .short  0                   /* DllCharacteristics                    */
    .quad   0x100000            /* SizeOfStackReserve  = 1MB             */
    .quad   0x100000            /* SizeOfStackCommit   = 1MB             */
    .quad   0x100000            /* SizeOfHeapReserve   = 1MB             */
    .quad   0x100000            /* SizeOfHeapCommit    = 1MB             */
    .long   0                   /* LoaderFlags                           */
    .long   16                  /* NumberOfRvaAndSizes                   */
    .space  128                 /* Data directories (all unused)         */

/* -- Section Header 0: ".uefi_pehdr" (header at VA 0) --------------- */
    .byte   0x2E,0x75,0x65,0x66,0x69,0x5F,0x70,0x65  /* ".uefi_pe"   */
    .byte   0x68,0x64,0x72,0x00,0x00,0x00,0x00,0x00  /* "hdr\0\0\0\0" */
    .long   0x200               /* VirtualSize                          */
    .long   0                   /* VirtualAddress = 0                   */
    .long   0x200               /* SizeOfRawData                        */
    .long   0                   /* PointerToRawData = file offset 0     */
    .long   0                   /* PointerToRelocations                 */
    .long   0                   /* PointerToLinenumbers                 */
    .short  0                   /* NumberOfRelocations                  */
    .short  0                   /* NumberOfLinenumbers                  */
    .long   0x40000040          /* Characteristics = INITIALIZED|READ   */

/* -- Section Header 1: ".text" (code at VA 0x1000) ------------------ */
    .byte   0x2E, 0x74, 0x65, 0x78, 0x74, 0x00, 0x00, 0x00
    .long   _text_size          /* VirtualSize                          */
    .long   _text_start - _image_base  /* VirtualAddress (RVA 0x1000)   */
    .long   _text_size          /* SizeOfRawData                        */
    .long   0x200               /* PointerToRawData = file offset 0x200 */
    .long   0                   /* PointerToRelocations                 */
    .long   0                   /* PointerToLinenumbers                 */
    .short  0                   /* NumberOfRelocations                  */
    .short  0                   /* NumberOfLinenumbers                  */
    .long   0x60000020          /* Characteristics = CODE|EXEC|READ     */

/* ================================================================== */
/* Entry point – first in .text so it sits at _text_start (VA 0x1000) */
/* ================================================================== */
    .section ".text", "ax", %progbits
    .global _start
    .type   _start, %function

_start:
    mv      s0, a0
    mv      s1, a1

    la      t0, __bss_start
    la      t1, __bss_end
    bgeu    t0, t1, .Lbss_done
.Lbss_loop:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    bltu    t0, t1, .Lbss_loop
.Lbss_done:

    la      sp, __stack_top
    andi    sp, sp, ~0xF

    mv      a0, s0
    mv      a1, s1
    call    uefi_main_c

.Lhalt:
    wfi
    j       .Lhalt

    .size _start, . - _start

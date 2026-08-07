; =============================================================================
; ForeB - Forest Bootloader
; include/boot_protocol.inc - NASM mirror of the boot handoff ABI
; =============================================================================
; This file is the assembly-side companion to include/boot_protocol.h. It keeps
; the BIOS stages (stage1/2/3.asm) and the native UEFI C loader in agreement on
; the SAME magic values, flag bits, fixed physical addresses and struct layouts.
;
; POLICY: config.h is the authoritative definition of every constant and struc.
; This file DOES NOT re-define anything config.h already provides — it %includes
; config.h (idempotent via its own FOREB_CONFIG_H guard) and then adds only the
; handful of UEFI-oriented aliases/derived values that config.h does not carry.
; Every added symbol is wrapped in %ifndef so a -D override or a second include
; is always safe.
;
; Usage from a stage that already sits next to config.h:
;     %include "config.h"
;     %include "include/boot_protocol.inc"   ; or "boot_protocol.inc" per -I path
; =============================================================================

%ifndef FOREB_BOOT_PROTOCOL_INC
%define FOREB_BOOT_PROTOCOL_INC

; -----------------------------------------------------------------------------
; Pull in the single source of truth. config.h guards itself with
; %ifndef FOREB_CONFIG_H, so including it here is harmless if a stage already
; did. NASM resolves "config.h" against its -I search path (the Makefile passes
; -I. so the project root is searched). A consumer that wants ONLY this file's
; standalone definitions (no config.h) can pass -DFOREB_NO_AUTO_CONFIG_INCLUDE.
; -----------------------------------------------------------------------------
%ifndef FOREB_CONFIG_H
  %ifndef FOREB_NO_AUTO_CONFIG_INCLUDE
    %include "config.h"
  %endif
%endif

; =============================================================================
; The following symbols mirror config.h EXACTLY. They are re-stated here ONLY
; guarded by %ifndef so that a consumer which includes boot_protocol.inc WITHOUT
; config.h (e.g. a minimal tool, or a build that sets FOREB_NO_AUTO_CONFIG_INCLUDE)
; still gets identical values. When config.h is present these blocks are inert.
; =============================================================================

; --- Magic values (config.h 270-284) ---------------------------------------
%ifndef FOREB_BOOT_INFO_MAGIC
%define FOREB_BOOT_INFO_MAGIC   0x464F5242      ; "FORB"
%endif
%ifndef FOREB_BOOT_INFO_VER
%define FOREB_BOOT_INFO_VER     0x00020000      ; v2.0
%endif
%ifndef MULTIBOOT1_MAGIC
%define MULTIBOOT1_MAGIC        0x2BADB002      ; EAX at handoff
%endif
%ifndef MULTIBOOT1_HEADER_MAGIC
%define MULTIBOOT1_HEADER_MAGIC 0x1BADB002
%endif
%ifndef MULTIBOOT2_MAGIC
%define MULTIBOOT2_MAGIC        0x36d76289
%endif

; --- foreboots_boot_info.flags bits (config.h 360-372) ----------------------
%ifndef FOREB_BIF_MMAP
%define FOREB_BIF_MMAP          0x00000001
%endif
%ifndef FOREB_BIF_FRAMEBUFFER
%define FOREB_BIF_FRAMEBUFFER   0x00000002
%endif
%ifndef FOREB_BIF_CMDLINE
%define FOREB_BIF_CMDLINE       0x00000004
%endif
%ifndef FOREB_BIF_LONG_MODE
%define FOREB_BIF_LONG_MODE     0x00000008
%endif
%ifndef FOREB_BIF_CPUID
%define FOREB_BIF_CPUID         0x00000010
%endif
%ifndef FOREB_BIF_PAE
%define FOREB_BIF_PAE           0x00000020
%endif
%ifndef FOREB_BIF_INITRD
%define FOREB_BIF_INITRD        0x00000040
%endif
%ifndef FOREB_BIF_NO_FB
%define FOREB_BIF_NO_FB         0x00000080
%endif
%ifndef FOREB_BIF_SAFE
%define FOREB_BIF_SAFE          0x00000100
%endif
%ifndef FOREB_BIF_KERNEL_PRELOADED
%define FOREB_BIF_KERNEL_PRELOADED 0x00000200
%endif

; --- Multiboot1 info flags (config.h 278-284) -------------------------------
%ifndef MB_FLAG_MEM
%define MB_FLAG_MEM             0x00000001
%endif
%ifndef MB_FLAG_BOOT_DEVICE
%define MB_FLAG_BOOT_DEVICE     0x00000002
%endif
%ifndef MB_FLAG_CMDLINE
%define MB_FLAG_CMDLINE         0x00000004
%endif
%ifndef MB_FLAG_MODS
%define MB_FLAG_MODS            0x00000008
%endif
%ifndef MB_FLAG_MMAP
%define MB_FLAG_MMAP            0x00000040
%endif
%ifndef MB_FLAG_BOOTLOADER
%define MB_FLAG_BOOTLOADER      0x00000200
%endif
%ifndef MB_FLAG_FRAMEBUFFER
%define MB_FLAG_FRAMEBUFFER     0x00001000
%endif

; --- E820 memory types (config.h 287-291) -----------------------------------
%ifndef E820_USABLE
%define E820_USABLE            1
%endif
%ifndef E820_RESERVED
%define E820_RESERVED         2
%endif
%ifndef E820_ACPI_RECLAIM
%define E820_ACPI_RECLAIM     3
%endif
%ifndef E820_ACPI_NVS
%define E820_ACPI_NVS         4
%endif
%ifndef E820_BAD
%define E820_BAD              5
%endif

; --- framebuffer_type values (shared by both structs) -----------------------
%ifndef FOREB_FB_INDEXED
%define FOREB_FB_INDEXED      0
%endif
%ifndef FOREB_FB_RGB
%define FOREB_FB_RGB          1
%endif
%ifndef FOREB_FB_EGA_TEXT
%define FOREB_FB_EGA_TEXT     2
%endif

; --- Fixed physical layout (config.h 166-211) -------------------------------
%ifndef BOOT_INFO_ADDRESS
%define BOOT_INFO_ADDRESS     0x00001000
%endif
%ifndef FOREB_MMAP_ADDRESS
%define FOREB_MMAP_ADDRESS    0x00001100
%endif
%ifndef MB_MMAP_ADDRESS
%define MB_MMAP_ADDRESS       0x00001400
%endif
%ifndef MULTIBOOT_INFO_ADDR
%define MULTIBOOT_INFO_ADDR   0x00001800
%endif
%ifndef FOREB_MMAP_MAX
%define FOREB_MMAP_MAX        32
%endif
%ifndef KERNEL_LOAD_PHYS
%define KERNEL_LOAD_PHYS      0x00010000
%endif
%ifndef INITRD_LOAD_PHYS
%define INITRD_LOAD_PHYS      0x00040000
%endif

; --- Menu entry indices (config.h 138-142) ----------------------------------
%ifndef ENTRY_DEFAULT
%define ENTRY_DEFAULT         0
%endif
%ifndef ENTRY_NOFB
%define ENTRY_NOFB            1
%endif
%ifndef ENTRY_SAFE
%define ENTRY_SAFE            2
%endif
%ifndef ENTRY_REBOOT
%define ENTRY_REBOOT          3
%endif

; --- ELF constants (config.h 449-454) ---------------------------------------
%ifndef ELFCLASS32
%define ELFCLASS32            1
%endif
%ifndef ELFCLASS64
%define ELFCLASS64            2
%endif
%ifndef ELFDATA2LSB
%define ELFDATA2LSB           1
%endif
%ifndef EM_386
%define EM_386                3
%endif
%ifndef EM_X86_64
%define EM_X86_64             62
%endif
%ifndef PT_LOAD
%define PT_LOAD               1
%endif

; =============================================================================
; Struct layouts. config.h already declares these `struc`s; re-declaring a
; NASM struc is an error, so we only emit them when config.h was NOT included.
; This keeps boot_protocol.inc self-sufficient for standalone consumers while
; never colliding with config.h.
; =============================================================================
%ifndef FOREB_CONFIG_H

struc foreboots_boot_info
    .magic              resd 1      ;   0
    .version            resd 1      ;   4
    .flags              resd 1      ;   8
    .boot_disk          resd 1      ;  12
    .cmdline            resd 1      ;  16
    .boot_loader_name   resd 1      ;  20
    .mem_lower          resd 1      ;  24
    .mem_upper          resd 1      ;  28
    .mmap_count         resd 1      ;  32
    .mmap_addr          resd 1      ;  36
    .framebuffer_addr   resq 1      ;  40
    .framebuffer_pitch  resd 1      ;  48
    .framebuffer_width  resd 1      ;  52
    .framebuffer_height resd 1      ;  56
    .framebuffer_bpp    resd 1      ;  60
    .framebuffer_type   resd 1      ;  64
    .vbe_mode           resw 1      ;  68
    .vbe_pad            resw 1      ;  70
    .cpuid_available    resd 1      ;  72
    .long_mode_available resd 1     ;  76
    .pae_available      resd 1      ;  80
    .kernel_load_addr   resd 1      ;  84
    .kernel_size        resd 1      ;  88
    .kernel_entry       resd 1      ;  92
    .kernel_is64bit     resd 1      ;  96
    .initrd_addr        resd 1      ; 100
    .initrd_size        resd 1      ; 104
    .boot_entry         resd 1      ; 108
    .no_framebuffer     resd 1      ; 112
    .safe_mode          resd 1      ; 116
    .reserved           resd 8      ; 120..151
endstruc

struc foreboots_mmap_entry
    .base               resq 1      ;  0
    .length             resq 1      ;  8
    .type               resd 1      ; 16
    .acpi               resd 1      ; 20
endstruc

struc mb_mmap_entry
    .size               resd 1      ;  0 (= 20)
    .addr_low           resd 1      ;  4
    .addr_high          resd 1      ;  8
    .len_low            resd 1      ; 12
    .len_high           resd 1      ; 16
    .type               resd 1      ; 20
endstruc

struc mb_info
    .flags              resd 1      ;   0
    .mem_lower          resd 1      ;   4
    .mem_upper          resd 1      ;   8
    .boot_device        resd 1      ;  12
    .cmdline            resd 1      ;  16
    .mods_count         resd 1      ;  20
    .mods_addr          resd 1      ;  24
    .syms               resb 16     ;  28
    .mmap_length        resd 1      ;  44
    .mmap_addr          resd 1      ;  48
    .drives_length      resd 1      ;  52
    .drives_addr        resd 1      ;  56
    .config_table       resd 1      ;  60
    .boot_loader_name   resd 1      ;  64
    .apm_table          resd 1      ;  68
    .vbe_control_info   resd 1      ;  72
    .vbe_mode_info      resd 1      ;  76
    .vbe_mode           resw 1      ;  80
    .vbe_interface_seg  resw 1      ;  82
    .vbe_interface_off  resw 1      ;  84
    .vbe_interface_len  resw 1      ;  86
    .framebuffer_addr   resq 1      ;  88
    .framebuffer_pitch  resd 1      ;  96
    .framebuffer_width  resd 1      ; 100
    .framebuffer_height resd 1      ; 104
    .framebuffer_bpp    resb 1      ; 108
    .framebuffer_type   resb 1      ; 109
    .framebuffer_color_info resw 1  ; 110
endstruc

struc mb_module
    .mod_start          resd 1      ;  0
    .mod_end            resd 1      ;  4
    .string             resd 1      ;  8
    .reserved           resd 1      ; 12
endstruc

%endif ; not FOREB_CONFIG_H (structs)

; --- Struct sizes (config.h 442-446) ----------------------------------------
; Declared AFTER the strucs so that on modern NASM (which auto-defines
; <name>_size at endstruc) these %ifndef guards simply confirm the value
; rather than shadowing the token while the struc is being parsed.
%ifndef foreboots_boot_info_size
%define foreboots_boot_info_size   152
%endif
%ifndef foreboots_mmap_entry_size
%define foreboots_mmap_entry_size  24
%endif
%ifndef mb_mmap_entry_size
%define mb_mmap_entry_size         24
%endif
%ifndef mb_info_size
%define mb_info_size               112
%endif
%ifndef mb_module_size
%define mb_module_size             16
%endif

; =============================================================================
; Handoff GDT descriptors (kernel entry contract). These are pure constants
; config.h does not define; provide them once here for any stage or trampoline
; that needs to build the identical 32-bit flat GDT.
; =============================================================================
%ifndef FOREB_GDT_CODE32
%define FOREB_GDT_CODE32   0x00CF9A000000FFFF   ; sel 0x08: 32-bit flat code
%endif
%ifndef FOREB_GDT_DATA32
%define FOREB_GDT_DATA32   0x00CF92000000FFFF   ; sel 0x10: 32-bit flat data
%endif
%ifndef FOREB_GDT_CODE64
%define FOREB_GDT_CODE64   0x00AF9A000000FFFF   ; optional long-mode code
%endif
%ifndef FOREB_GDT_DATA64
%define FOREB_GDT_DATA64   0x00CF92000000FFFF   ; optional long-mode data
%endif
%ifndef FOREB_SEL_CODE32
%define FOREB_SEL_CODE32   0x08
%endif
%ifndef FOREB_SEL_DATA32
%define FOREB_SEL_DATA32   0x10
%endif

%endif ; FOREB_BOOT_PROTOCOL_INC

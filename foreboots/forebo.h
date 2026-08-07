; =============================================================================
; ForeB - Forest Bootloader
; forebo.h (NASM include) - Shared Macros and Definitions
; =============================================================================
; Include this file after config.h in stage2.asm

%ifndef FOREBO_H
%define FOREBO_H

; =============================================================================
; MACRO: print_str
; Print a null-terminated string in real mode via INT 10h AH=0Eh (teletype)
; Usage: print_str label_of_string
; Clobbers: AX, BX, SI
; =============================================================================
%macro print_str 1
    push si
    mov  si, %1
    call _print_str_impl
    pop  si
%endmacro

; =============================================================================
; MACRO: print_char
; Print a single ASCII character in real mode
; Usage: print_char 'X'
; Clobbers: AX, BX
; =============================================================================
%macro print_char 1
    mov  al, %1
    mov  ah, 0x0E
    mov  bh, 0
    mov  bl, 0x0F
    int  0x10
%endmacro

; =============================================================================
; MACRO: plot_pixel
; Write a byte (color index) to the linear framebuffer in 8bpp mode.
; This macro is expanded inline; for real code call the subroutine.
; Usage: plot_pixel x_reg, y_reg, color_reg
;        where x_reg, y_reg are 16-bit registers holding coordinates,
;        color_reg is an 8-bit register holding the palette index.
; ES must point to the LFB segment (set once before calling).
; Clobbers: EDI
; =============================================================================
%macro plot_pixel 3
    ; edi = y * SCREEN_WIDTH + x
    movzx edi, word %2          ; edi = y
    imul  edi, SCREEN_WIDTH     ; edi = y * width
    movzx eax, word %1          ; eax = x
    add   edi, eax              ; edi = y*w + x
    mov   byte [es:edi], %3    ; write color
%endmacro

; =============================================================================
; MACRO: fill_rect
; Fill a rectangular region with a color.
; Parameters: x, y, w, h, color  (all immediate or registers)
; This calls the fill_rect subroutine with parameters on stack.
; Clobbers: EAX, ECX, EDX, EDI
; =============================================================================
%macro fill_rect 5
    push word %5
    push word %4
    push word %3
    push word %2
    push word %1
    call _fill_rect_impl
    add  sp, 10
%endmacro

; =============================================================================
; MACRO: draw_hline
; Draw a horizontal line.
; Parameters: x, y, length, color
; =============================================================================
%macro draw_hline 4
    push word %4
    push word %3
    push word %2
    push word %1
    call _draw_hline_impl
    add  sp, 8
%endmacro

; =============================================================================
; MACRO: draw_vline
; Draw a vertical line.
; Parameters: x, y, length, color
; =============================================================================
%macro draw_vline 4
    push word %4
    push word %3
    push word %2
    push word %1
    call _draw_vline_impl
    add  sp, 8
%endmacro

; =============================================================================
; MACRO: draw_rect_outline
; Draw a hollow rectangle (border only).
; Parameters: x, y, w, h, color
; =============================================================================
%macro draw_rect_outline 5
    push word %5
    push word %4
    push word %3
    push word %2
    push word %1
    call _draw_rect_outline_impl
    add  sp, 10
%endmacro

; =============================================================================
; MACRO: draw_text
; Draw a string of text at pixel position using the built-in 8x8 font.
; Parameters: x, y, color, string_label
; =============================================================================
%macro draw_text 4
    push word %3
    push word %2
    push word %1
    push word %4
    call _draw_text_impl
    add  sp, 8
%endmacro

; =============================================================================
; MACRO: delay_ticks
; Spin-wait for approximately N timer ticks (18.2 Hz BIOS timer).
; Parameters: tick_count (immediate)
; =============================================================================
%macro delay_ticks 1
    push ax
    push cx
    push dx
    mov  cx, %1
    call _delay_ticks_impl
    pop  dx
    pop  cx
    pop  ax
%endmacro

; =============================================================================
; Structure: VBE Controller Info (returned by INT 10h AX=4F00h)
; Total: 512 bytes, placed at VBEINFO_OFF
; =============================================================================
struc VBEControllerInfo
    .Signature      resb 4      ; "VESA"
    .Version        resw 1      ; VBE version (e.g. 0x0300 = VBE 3.0)
    .OEMStringPtr   resd 1      ; Far pointer to OEM string
    .Capabilities   resd 1
    .VideoModePtr   resd 1      ; Far pointer to video mode list
    .TotalMemory    resw 1      ; in 64 KiB blocks
    .OEMSoftwareRev resw 1
    .OEMVendorNamePtr resd 1
    .OEMProductNamePtr resd 1
    .OEMProductRevPtr  resd 1
    .Reserved       resb 222
    .OEMData        resb 256
endstruc

; =============================================================================
; Structure: VBE Mode Info (returned by INT 10h AX=4F01h)
; Total: 256 bytes, placed at VBEMODEINFO_OFF
; =============================================================================
struc VBEModeInfo
    .ModeAttributes     resw 1      ; Mode attributes
    .WinAAttributes     resb 1
    .WinBAttributes     resb 1
    .WinGranularity     resw 1      ; Window granularity (KiB)
    .WinSize            resw 1      ; Window size (KiB)
    .WinASegment        resw 1
    .WinBSegment        resw 1
    .WinFuncPtr         resd 1
    .BytesPerScanLine   resw 1
    ; VBE 1.2+
    .XResolution        resw 1
    .YResolution        resw 1
    .XCharSize          resb 1
    .YCharSize          resb 1
    .NumberOfPlanes     resb 1
    .BitsPerPixel       resb 1
    .NumberOfBanks      resb 1
    .MemoryModel        resb 1
    .BankSize           resb 1
    .NumberOfImagePages resb 1
    .Reserved1          resb 1
    ; Direct color info
    .RedMaskSize        resb 1
    .RedFieldPosition   resb 1
    .GreenMaskSize      resb 1
    .GreenFieldPosition resb 1
    .BlueMaskSize       resb 1
    .BlueFieldPosition  resb 1
    .RsvdMaskSize       resb 1
    .RsvdFieldPosition  resb 1
    .DirectColorModeInfo resb 1
    ; VBE 2.0+
    .PhysBasePtr        resd 1      ; Physical address of linear framebuffer
    .Reserved2          resd 1
    .Reserved3          resw 1
    ; VBE 3.0+
    .LinBytesPerScanLine resw 1
    .BnkNumberOfImagePages resb 1
    .LinNumberOfImagePages resb 1
    .LinRedMaskSize     resb 1
    .LinRedFieldPosition resb 1
    .LinGreenMaskSize   resb 1
    .LinGreenFieldPosition resb 1
    .LinBlueMaskSize    resb 1
    .LinBlueFieldPosition resb 1
    .LinRsvdMaskSize    resb 1
    .LinRsvdFieldPosition resb 1
    .MaxPixelClock      resd 1
    .Reserved4          resb 190
endstruc

; =============================================================================
; Structure: Disk Address Packet (for INT 13h AH=42h extended read)
; =============================================================================
struc DAP
    .Size       resb 1          ; Size of DAP (0x10)
    .Zero       resb 1          ; Must be 0
    .Count      resw 1          ; Number of sectors to read
    .Offset     resw 1          ; Destination offset
    .Segment    resw 1          ; Destination segment
    .LBALow     resd 1          ; LBA low 32 bits
    .LBAHigh    resd 1          ; LBA high 32 bits (usually 0)
endstruc

; =============================================================================
; Structure: Multiboot 1 Info (passed to kernel in EBX)
; =============================================================================
struc MBInfo
    .flags          resd 1      ; flags
    .mem_lower      resd 1      ; available memory below 1MB (KiB)
    .mem_upper      resd 1      ; available memory above 1MB (KiB)
    .boot_device    resd 1      ; BIOS boot device
    .cmdline        resd 1      ; pointer to kernel command line
    .mods_count     resd 1      ; number of boot modules
    .mods_addr      resd 1      ; physical address of module list
    .syms           resb 16     ; symbol table or elf section header info
    .mmap_length    resd 1      ; memory map length
    .mmap_addr      resd 1      ; memory map address
    .drives_length  resd 1
    .drives_addr    resd 1
    .config_table   resd 1
    .boot_loader_name resd 1    ; pointer to bootloader name string
    .apm_table      resd 1
    .vbe_control_info resd 1
    .vbe_mode_info  resd 1
    .vbe_mode       resw 1
    .vbe_interface_seg resw 1
    .vbe_interface_off resw 1
    .vbe_interface_len resw 1
endstruc

%endif ; FOREBO_H

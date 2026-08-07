; =============================================================================
; ForeB - Forest Bootloader  Stage 2
; stage2.asm - Extended loader: A20, E820, VBE mode select, boot menu, disk
;              load of stage3 + kernel, multiboot1 info build, PM handoff.
; =============================================================================
; Build:
;   nasm -f bin -o stage2.bin stage2.asm
;
; Maximum size: STAGE2_SECTOR_COUNT * 512 = 8192 bytes (16 sectors).
; Loaded at physical address 0x8000 (segment 0x0800, offset 0x0000).
; Stage 1 verifies word at [0x8002] == 0xFEB1 then far-jumps to 0x0800:0x0000.
;
; Segment convention throughout stage2:
;   DS = 0x0800  (stage2 code/data: screen_*, strings, font, GUI state)
;   ES = 0x0000  (low memory: boot_info, mmap, multiboot_info, VBE info, DAP)
; Functions that need DS=0 (E820, CPUID, multiboot build) switch DS temporarily.
;
; Stage 2 responsibilities:
;   1. Enable A20 (fast A20 + keyboard controller + BIOS fallbacks)
;   2. Initialize foreboots_boot_info struct at BOOT_INFO_ADDRESS
;   3. Detect CPUID / PAE / long-mode capability
;   4. Collect E820 memory map (up to 32 entries)
;   5. Select an 8bpp VBE mode for the GUI menu (fallback to text)
;   6. Load stage3 + kernel ELF (+ optional initrd) from disk via INT 13h
;      LBA extensions (AH=42h) with CHS (AH=02h) fallback
;   7. Present boot menu (VBE graphical or text) with timeout + arrow keys
;   8. On selection: re-select the kernel framebuffer mode (32bpp chain) or
;      text mode, set cmdline / no-framebuffer / safe-mode flags, reboot
;   9. Build a standard multiboot_info_t at MULTIBOOT_INFO_ADDR (GRUB-compat)
;  10. Switch to 32-bit protected mode and jump to stage3 (ELF loader)
;
; The kernel is an ELF linked at 0x100000, entered in 32-bit PM with
; EAX=0x2BADB002, EBX=&multiboot_info_t (exactly as GRUB does it). stage3
; parses the ELF, copies PT_LOAD segments, and performs that handoff.
; =============================================================================

[BITS 16]
[ORG 0x0000]                    ; We run at 0x0800:0x0000 (physical 0x8000)

%include "config.h"
%include "forebo.h"
%include "forebo64.h"

; Fixed physical addresses accessed with ES=0 / DS=0
boot_info       equ BOOT_INFO_ADDRESS
foreb_mmap      equ FOREB_MMAP_ADDRESS
mb_mmap         equ MB_MMAP_ADDRESS
mb_info_addr    equ MULTIBOOT_INFO_ADDR

; =============================================================================
; Stage 2 header: short jump + magic + version
; =============================================================================
stage2_start:
    jmp  short stage2_main
    dw   0xFEB1                       ; checked by stage1 (word at offset 2)
    db  "ForeB v2.0", 0

; =============================================================================
; MAIN ENTRY POINT
; =============================================================================
stage2_main:
    push cs
    pop  ds                          ; DS = 0x0800 (stage2 data)
    xor  ax, ax
    mov  es, ax                      ; ES = 0x0000 (low memory)
    mov  ax, 0x7000
    mov  ss, ax
    mov  sp, 0xFFFE
    sti

%if FOREB_SERIAL_DEBUG
    serial_print str_serial_stage2   ; proof-of-progress on -serial stdio
%endif

    mov  [drive_number], dl          ; BIOS boot drive from stage1

    call init_boot_info              ; Step 1
    call enable_a20                  ; Step 2
    detect_long_mode                 ; Step 3 (macro)
    e820_collect                     ; Step 4 (macro)
    call compute_mem_totals

    ; Graphical forest menu. Drawing to the VBE LFB does NOT work from real /
    ; unreal mode on QEMU, so the menu is drawn from short 16-bit PROTECTED-MODE
    ; excursions (see run_in_pm) while keyboard/timer polling stays in real mode
    ; via INT 16h/INT 1Ah. If no suitable VBE mode is found, vesa_menu falls back
    ; to the plain text menu. The graphical mode is torn down (text 03h restored,
    ; framebuffer info cleared) before the kernel handoff, so the multiboot
    ; handoff is byte-identical to the proven text-mode path.
    mov  byte [vesa_ok], 0

    ; Boot menu (graphical VBE, text fallback)
    call vesa_menu                   ; AX = selected entry
apply_entry:
    call launch_boot_entry           ; set flags / re-select mode / reboot
    call load_kernel                 ; Step 8b: stream kernel ELF to memory
    call load_initrd                 ; Step 8c: optional multiboot module (initrd)
    call vesa_teardown               ; restore text 03h + clear fb before handoff
    call build_multiboot_info        ; Step 9
    call enter_pm_and_jump_stage3    ; Step 10
    jmp  $

; ============================================================================
; init_boot_info: zero the struct and fill static fields
; ============================================================================
init_boot_info:
    pushad
    push es
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  edi, boot_info
    mov  ecx, foreboots_boot_info_size
    xor  eax, eax
    rep  stosb
    mov  dword [boot_info + foreboots_boot_info.magic], FOREB_BOOT_INFO_MAGIC
    mov  dword [boot_info + foreboots_boot_info.version], FOREB_BOOT_INFO_VER
    mov  al, [cs:drive_number]       ; CS=0x0800, stage2 data
    mov  [boot_info + foreboots_boot_info.boot_disk], eax
    mov  dword [boot_info + foreboots_boot_info.kernel_load_addr], KERNEL_LOAD_PHYS
    mov  eax, str_bl_name
    add  eax, 0x8000
    mov  [boot_info + foreboots_boot_info.boot_loader_name], eax
    pop  ds
    pop  es
    popad
    ret

; ============================================================================
; A20 enable: fast A20 (port 0x92) -> keyboard controller -> BIOS INT 15h
; ============================================================================
enable_a20:
    in   al, 0x92
    or   al, 0x02
    and  al, 0xFE                    ; never toggle reset bit
    out  0x92, al
    call a20_check
    jnz  .a20_done
    call a20_enable_kbc
    call a20_check
    jnz  .a20_done
    mov  ax, 0x2401
    int  0x15
    call a20_check
    jnz  .a20_done
    mov  si, str_a20warn
    call bios_print
.a20_done:
    ret

; a20_check: returns ZF=1 if A20 is OFF (wrapped), ZF=0 if ON. Preserves regs.
a20_check:
    push es
    push ds
    push ax
    push bx
    xor  ax, ax
    mov  ds, ax                      ; DS = 0
    mov  byte [0x0500], 0x55         ; marker at 0x0000:0x0500
    dec  ax
    mov  es, ax                      ; ES = 0xFFFF
    mov  bl, [es:0x0510]             ; 0xFFFF:0x0510 wraps to 0x500 if A20 off
    mov  bh, [0x0500]               ; 0x0000:0x0500 (DS=0)
    cmp  bl, bh                      ; equal => A20 OFF
    pop  bx
    pop  ax
    pop  ds
    pop  es
    ret

a20_enable_kbc:
    cli
    call kbc_wait_in
    mov  al, 0xD0
    out  0x64, al
    call kbc_wait_out
    in   al, 0x60
    push ax
    call kbc_wait_in
    mov  al, 0xD1
    out  0x64, al
    call kbc_wait_in
    pop  ax
    or   al, 0x02
    out  0x60, al
    call kbc_wait_in
    mov  al, 0xAE                    ; re-enable keyboard
    out  0x64, al
    sti
    ret
kbc_wait_in:
    in   al, 0x64
    test al, 0x02
    jnz  kbc_wait_in
    ret
kbc_wait_out:
    in   al, 0x64
    test al, 0x01
    jz   kbc_wait_out
    ret

; ============================================================================
; compute_mem_totals: derive mem_upper (KiB above 1MB) from the E820 map.
; mem_lower is fixed at 640 (conventional low memory).
; ============================================================================
compute_mem_totals:
    push eax
    push ecx
    push edx
    push esi
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  esi, foreb_mmap
    mov  ecx, [boot_info + foreboots_boot_info.mmap_count]
    xor  edx, edx
.cmt_loop:
    test ecx, ecx
    jz   .cmt_done
    cmp  dword [esi + foreboots_mmap_entry.type], E820_USABLE
    jne  .cmt_next
    cmp  dword [esi + foreboots_mmap_entry.base + 4], 0
    jne  .cmt_upper
    cmp  dword [esi + foreboots_mmap_entry.base], 0x100000
    jae  .cmt_upper
    jmp  .cmt_next
.cmt_upper:
    mov  eax, [esi + foreboots_mmap_entry.length]
    shr  eax, 10
    add  edx, eax
.cmt_next:
    add  esi, foreboots_mmap_entry_size
    dec  ecx
    jmp  .cmt_loop
.cmt_done:
    mov  [boot_info + foreboots_boot_info.mem_upper], edx
    mov  dword [boot_info + foreboots_boot_info.mem_lower], 640
    pop  ds
    pop  esi
    pop  edx
    pop  ecx
    pop  eax
    ret

%if 1  ; ===== VBE graphical code — disabled (text mode only) =====
; ============================================================================
; preference table at [BX] (DS=0x0800). Each entry: width, height, bpp (words);
; table terminated by a zero width. Fills screen_* (stage2 data) and the
; boot_info framebuffer fields (ES=0). Sets vesa_ok and FOREB_BIF_FRAMEBUFFER.
; On failure: sets text mode 03h and clears framebuffer info.
; ============================================================================
setup_vesa:
    push es
    push ax
    xor  ax, ax
    mov  es, ax                      ; ES = 0 for VBE info / boot_info
    mov  di, VBEINFO_OFF
    mov  dword [es:di], 0x32454256   ; "VBE2"
    mov  ax, 0x4F00
    int  0x10
    cmp  ax, 0x004F
    jne  .svs_fail
    cmp  dword [es:VBEINFO_OFF], 0x41534556   ; "VESA"
    jne  .svs_fail
.svs_pref:
    movzx eax, word [bx]            ; width  (DS=0x0800, BX=table offset)
    test  eax, eax
    jz    .svs_fail
    movzx ecx, word [bx + 2]        ; height
    movzx edx, word [bx + 4]        ; bpp
    call  find_vbe_mode             ; CF=0 on success
    jnc   .svs_done
    add   bx, 6
    jmp   .svs_pref
.svs_done:
    mov  byte [vesa_ok], 1
    or   dword [es:boot_info + foreboots_boot_info.flags], FOREB_BIF_FRAMEBUFFER
    pop  ax
    pop  es
    ret
.svs_fail:
    mov  byte [vesa_ok], 0
    mov  ax, 0x0003
    int  0x10
    and  dword [es:boot_info + foreboots_boot_info.flags], 0xFFFFFFFD
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr + 4], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_width], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_height], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_bpp], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_type], 2
    pop  ax
    pop  es
    ret

; find_vbe_mode: search the VBE mode list for EAX=width, ECX=height, EDX=bpp
; with a linear framebuffer. Sets the mode + screen_* + boot_info fb fields.
; DS=0x0800, ES=0. CF=0 on success. Preserves all regs (pushad).
find_vbe_mode:
    mov  [fvm_w], ax
    mov  [fvm_h], cx
    mov  [fvm_b], dl
    push es
    push fs
    pushad
    xor  ax, ax
    mov  es, ax
    mov  eax, [es:VBEINFO_OFF + 14]      ; VideoModePtr far (off:seg)
    mov  [fvm_mpptr_off], ax
    shr  eax, 16
    mov  [fvm_mpptr_seg], ax
.fvm_loop:
    mov  fs, [fvm_mpptr_seg]
    mov  bx, [fvm_mpptr_off]
    mov  ax, [fs:bx]                     ; mode number
    cmp  ax, 0xFFFF
    je   .fvm_notfound
    add  word [fvm_mpptr_off], 2
    mov  cx, ax                          ; CX = mode number
    mov  di, VBEMODEINFO_OFF
    mov  ax, 0x4F01
    int  0x10
    cmp  ax, 0x004F
    jne  .fvm_loop
    test word [es:VBEMODEINFO_OFF + VBEModeInfo.ModeAttributes], VBE_ATTR_LFB
    jz   .fvm_loop
    test word [es:VBEMODEINFO_OFF + VBEModeInfo.ModeAttributes], VBE_ATTR_GRAPHICS
    jz   .fvm_loop
    mov  al, [es:VBEMODEINFO_OFF + VBEModeInfo.MemoryModel]
    cmp  al, VBE_MODEL_PACKED
    je   .fvm_model_ok
    cmp  al, VBE_MODEL_DIRECT
    jne  .fvm_loop
.fvm_model_ok:
    mov  ax, [es:VBEMODEINFO_OFF + VBEModeInfo.XResolution]
    cmp  ax, [fvm_w]
    jne  .fvm_loop
    mov  ax, [es:VBEMODEINFO_OFF + VBEModeInfo.YResolution]
    cmp  ax, [fvm_h]
    jne  .fvm_loop
    mov  al, [es:VBEMODEINFO_OFF + VBEModeInfo.BitsPerPixel]
    cmp  al, [fvm_b]
    jne  .fvm_loop
    mov  eax, [es:VBEMODEINFO_OFF + VBEModeInfo.PhysBasePtr]
    test eax, eax
    jz   .fvm_loop
    mov  [lfb_phys_addr], eax            ; save before clobbering EAX
    ; save geometry
    mov  ax, [es:VBEMODEINFO_OFF + VBEModeInfo.XResolution]
    mov  [screen_width], ax
    mov  ax, [es:VBEMODEINFO_OFF + VBEModeInfo.YResolution]
    mov  [screen_height], ax
    mov  al, [es:VBEMODEINFO_OFF + VBEModeInfo.BitsPerPixel]
    mov  [screen_bpp], al
    mov  ax, [es:VBEMODEINFO_OFF + VBEModeInfo.BytesPerScanLine]
    mov  [screen_pitch], ax
    ; set the mode (LFB flag)
    or   cx, BIOS_VBE_LINEAR
    mov  bx, cx
    mov  ax, 0x4F02
    int  0x10
    cmp  ax, 0x004F
    jne  .fvm_loop
    ; fill boot_info framebuffer fields (ES=0)
    mov  eax, [lfb_phys_addr]
    mov  [es:boot_info + foreboots_boot_info.framebuffer_addr], eax
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr + 4], 0
    movzx eax, word [screen_pitch]
    mov  [es:boot_info + foreboots_boot_info.framebuffer_pitch], eax
    movzx eax, word [screen_width]
    mov  [es:boot_info + foreboots_boot_info.framebuffer_width], eax
    movzx eax, word [screen_height]
    mov  [es:boot_info + foreboots_boot_info.framebuffer_height], eax
    movzx eax, byte [screen_bpp]
    mov  [es:boot_info + foreboots_boot_info.framebuffer_bpp], eax
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_type], 1
    cmp  byte [screen_bpp], 8
    ja   .fvm_typeset
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_type], 0
.fvm_typeset:
    mov  [es:boot_info + foreboots_boot_info.vbe_mode], cx
    mov  [current_vesa_mode], cx
    popad
    pop  fs
    pop  es
    clc
    ret
.fvm_notfound:
    popad
    pop  fs
    pop  es
    stc
    ret

; ============================================================================
; Unreal Mode: FS = 4GB data segment for LFB access
; ============================================================================
enable_unreal_mode:
    cli
    push eax
    push ds
    sidt [saved_idtr]
    lgdt [gdt_descriptor]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    mov  ax, 0x10
    mov  fs, ax
    and  eax, 0xFFFFFFFE
    mov  cr0, eax
    jmp  .flush
.flush:
    pop  ds
    push cs
    pop  ds
    lidt [saved_idtr]
    mov  byte [unreal_ok], 1
    pop  eax
    sti
    ret

align 8
gdt_start:
    dq  0x0000000000000000
    dw  0xFFFF, 0x0000, 0x00, 0x9A, 0x00, 0x00   ; 16-bit code (placeholder)
    dw  0xFFFF, 0x0000, 0x00, 0x92, 0xCF, 0x00   ; 32-bit flat data (sel 0x10)
gdt_end:
gdt_descriptor:
    dw  gdt_end - gdt_start - 1
    dd  gdt_start + 0x8000
saved_idtr:
    dw  0
    dd  0

; ============================================================================
; DAC Palette: forest colors at DAC registers 16..28 (8bpp mode only)
; ============================================================================
program_palette:
    cmp  byte [screen_bpp], 8
    jne  .pp_done
    push es
    xor  ax, ax
    mov  es, ax
    mov  di, PALETTE_BUF
    mov  byte [es:di+0], 6
    mov  byte [es:di+1], 11
    mov  byte [es:di+2], 6
    mov  byte [es:di+3], 7
    mov  byte [es:di+4], 13
    mov  byte [es:di+5], 7
    mov  byte [es:di+6], 10
    mov  byte [es:di+7], 20
    mov  byte [es:di+8], 10
    mov  byte [es:di+9], 5
    mov  byte [es:di+10], 25
    mov  byte [es:di+11], 5
    mov  byte [es:di+12], 20
    mov  byte [es:di+13], 50
    mov  byte [es:di+14], 15
    mov  byte [es:di+15], 45
    mov  byte [es:di+16], 55
    mov  byte [es:di+17], 45
    mov  byte [es:di+18], 25
    mov  byte [es:di+19], 32
    mov  byte [es:di+20], 25
    mov  byte [es:di+21], 55
    mov  byte [es:di+22], 40
    mov  byte [es:di+23], 5
    mov  byte [es:di+24], 63
    mov  byte [es:di+25], 63
    mov  byte [es:di+26], 63
    mov  byte [es:di+27], 1
    mov  byte [es:di+28], 2
    mov  byte [es:di+29], 1
    mov  byte [es:di+30], 15
    mov  byte [es:di+31], 7
    mov  byte [es:di+32], 2
    mov  byte [es:di+33], 7
    mov  byte [es:di+34], 30
    mov  byte [es:di+35], 7
    mov  byte [es:di+36], 15
    mov  byte [es:di+37], 45
    mov  byte [es:di+38], 15
    mov  ax, 0x1012
    mov  bx, 16
    mov  cx, 13
    mov  dx, PALETTE_BUF
    int  0x10
    pop  es
.pp_done:
    ret

; ============================================================================
; Pixel primitives (LFB via FS:[phys] in unreal mode; 8bpp menu mode)
; ============================================================================
plot_px:
    push edx
    movzx edx, word [screen_pitch]
    imul  ebx, edx
    add   ebx, eax
    add   ebx, [lfb_phys_addr]
    mov   [fs:ebx], cl
    pop   edx
    ret

fill_box:
    push eax
    push ebx
    push ecx
    push edx
    push edi
    movzx ebx, word [fb_y]
    movzx ecx, word [fb_h]
    add   ecx, ebx
.fb_row:
    cmp   ebx, ecx
    jge   .fb_done
    movzx edi, word [screen_pitch]
    mov   edx, ebx
    imul  edx, edi
    movzx edi, word [fb_x]
    add   edx, edi
    add   edx, [lfb_phys_addr]
    movzx edi, word [fb_w]
    mov   al, [fb_c]
.fb_px:
    test  edi, edi
    jz    .fb_next_row
    mov   [fs:edx], al
    inc   edx
    dec   edi
    jmp   .fb_px
.fb_next_row:
    inc  ebx
    jmp  .fb_row
.fb_done:
    pop  edi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    ret

rect_outline:
    push ax
    push bx
    push cx
    push dx
    mov  ax, [fb_x]
    mov  bx, [fb_y]
    mov  cx, [fb_w]
    mov  dx, [fb_h]
    mov  word [fb_h], 1
    call fill_box
    mov  [fb_h], dx
    mov  [fb_y], bx
    add  [fb_y], dx
    dec  word [fb_y]
    mov  word [fb_h], 1
    call fill_box
    mov  [fb_y], bx
    mov  [fb_h], dx
    mov  word [fb_w], 1
    call fill_box
    mov  [fb_w], cx
    mov  [fb_x], ax
    add  [fb_x], cx
    dec  word [fb_x]
    mov  word [fb_w], 1
    call fill_box
    mov  [fb_x], ax
    mov  [fb_w], cx
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

; ============================================================================
; Text rendering: 8x8 bitmap font
; ============================================================================
draw_glyph:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi
    movzx esi, byte [dc_char]
    cmp   esi, 32
    jb    .dg_done
    sub   esi, 32
    cmp   esi, 96
    jge   .dg_done
    shl   esi, 3
    mov   ebx, 0
.dg_row:
    cmp   ebx, 8
    jge   .dg_done
    mov   ecx, 7
.dg_bit:
    mov   al, [font_data + esi + ebx] ; reload row byte: the plot path clobbers EAX
    mov   dl, al
    shr   dl, cl
    and   dl, 1
    test  dl, dl
    jz    .dg_skip_px
    movzx eax, word [dc_x]
    mov   edi, ecx                   ; font8x8_basic is LSB-first: bit N = column N
    add   eax, edi
    movzx edi, word [dc_y]
    add   edi, ebx
    push  eax
    movzx edx, word [screen_pitch]
    imul  edi, edx
    pop   eax
    add   edi, eax
    add   edi, [lfb_phys_addr]
    mov   dl, [dc_color]
    mov   [fs:edi], dl
.dg_skip_px:
    dec   ecx
    jge   .dg_bit
    inc   ebx
    jmp   .dg_row
.dg_done:
    pop  edi
    pop  esi
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    ret

gprint:
    push si
    push ax
.gp_loop:
    mov  al, [ds:si]
    test al, al
    jz   .gp_done
    mov  [dc_char], al
    call draw_glyph
    add  word [dc_x], 8
    inc  si
    jmp  .gp_loop
.gp_done:
    pop  ax
    pop  si
    ret

; ============================================================================
; High-level drawing
; ============================================================================
draw_background:
    mov  word [fb_x], 0
    mov  word [fb_y], 0
    mov  ax, [screen_width]
    mov  [fb_w], ax
    mov  ax, [screen_height]
    mov  [fb_h], ax
    mov  byte [fb_c], FOREB_BG
    call fill_box
    ret

draw_tree_logo:
    mov  word [dc_x], LOGO_X - 16
    mov  word [dc_y], LOGO_Y
    mov  byte [dc_color], FOREB_TREE2
    mov  si, tree_line_top
    call gprint
    mov  word [dc_x], LOGO_X - 24
    mov  word [dc_y], LOGO_Y + 14
    mov  byte [dc_color], FOREB_TREE3
    mov  si, tree_line_f3
    call gprint
    mov  word [dc_x], LOGO_X - 32
    mov  word [dc_y], LOGO_Y + 28
    mov  byte [dc_color], FOREB_TREE2
    mov  si, tree_line_f2
    call gprint
    mov  word [dc_x], LOGO_X - 40
    mov  word [dc_y], LOGO_Y + 42
    mov  byte [dc_color], FOREB_TREE3
    mov  si, tree_line_f1
    call gprint
    mov  word [dc_x], LOGO_X - 8
    mov  word [dc_y], LOGO_Y + 56
    mov  byte [dc_color], FOREB_TREE1
    mov  si, tree_line_trunk
    call gprint
    mov  word [dc_x], LOGO_X - 48
    mov  word [dc_y], LOGO_Y + 76
    mov  byte [dc_color], FOREB_DIM
    mov  si, tree_line_ground
    call gprint
    ret

draw_title_bar:
    mov  word [fb_x], 16
    mov  word [fb_y], 28
    mov  ax, [screen_width]
    sub  ax, 32
    mov  [fb_w], ax
    mov  word [fb_h], 2
    mov  byte [fb_c], FOREB_BORDER
    call fill_box
    mov  word [dc_x], TITLE_X
    mov  word [dc_y], TITLE_Y
    mov  byte [dc_color], FOREB_TITLE
    mov  si, str_title
    call gprint
    mov  word [dc_x], TITLE_X + 24
    mov  word [dc_y], TITLE_Y + 14
    mov  byte [dc_color], FOREB_DIM
    mov  si, str_subtitle
    call gprint
    ret

draw_menu_panel:
    mov  word [fb_x], MENU_X
    mov  word [fb_y], MENU_Y
    mov  word [fb_w], MENU_W
    mov  word [fb_h], MENU_H
    mov  byte [fb_c], FOREB_PANEL
    call fill_box
    mov  word [fb_x], MENU_X - 2
    mov  word [fb_y], MENU_Y - 2
    mov  word [fb_w], MENU_W + 4
    mov  word [fb_h], MENU_H + 4
    mov  byte [fb_c], FOREB_BORDER
    call rect_outline
    mov  word [dc_x], MENU_X + 14
    mov  word [dc_y], MENU_Y + 8
    mov  byte [dc_color], FOREB_TITLE
    mov  si, str_menu_label
    call gprint
    mov  word [fb_x], MENU_X + 6
    mov  word [fb_y], MENU_Y + 24
    mov  word [fb_w], MENU_W - 12
    mov  word [fb_h], 1
    mov  byte [fb_c], FOREB_BORDER
    call fill_box
    ret

draw_boot_entries:
    push cx
    xor  cx, cx
.dbe_loop:
    cmp  cx, BOOT_ENTRY_COUNT
    jge  .dbe_done
    call draw_one_entry
    inc  cx
    jmp  .dbe_loop
.dbe_done:
    pop  cx
    ret

draw_one_entry:
    push ax
    push bx
    push cx
    push dx
    push si
    movzx ax, cx
    mov   bx, ENTRY_HEIGHT
    mul   bx
    add   ax, ENTRY_Y_START
    cmp   cx, [selected_entry]
    jne   .doe_unselected
    mov  word [fb_x], MENU_X + 6
    mov  [fb_y], ax
    mov  word [fb_w], MENU_W - 12
    mov  word [fb_h], ENTRY_HEIGHT - 2
    mov  byte [fb_c], FOREB_SELECT
    call fill_box
.doe_unselected:
    movzx bx, cx
    mov   dx, ENTRY_HEIGHT
    mov   ax, bx
    mul   dx
    add   ax, ENTRY_Y_START
    add   ax, 10
    mov  word [dc_x], MENU_X + 16
    mov  [dc_y], ax
    cmp  cx, [selected_entry]
    jne  .doe_no_arrow
    mov  byte [dc_color], FOREB_WHITE
    mov  byte [dc_char], '>'
    call draw_glyph
    add  word [dc_x], 8
    jmp  .doe_label
.doe_no_arrow:
    add  word [dc_x], 8
.doe_label:
    movzx bx, cx
    shl   bx, 1
    mov   si, [entry_labels + bx]
    cmp   cx, [selected_entry]
    jne   .doe_dim_label
    mov   byte [dc_color], FOREB_WHITE
    jmp   .doe_draw_label
.doe_dim_label:
    mov   byte [dc_color], FOREB_TEXT
.doe_draw_label:
    call  gprint
    movzx bx, cx
    mov   dx, ENTRY_HEIGHT
    mov   ax, bx
    mul   dx
    add   ax, ENTRY_Y_START
    add   ax, 22
    mov  [dc_y], ax
    mov  word [dc_x], MENU_X + 24
    mov  byte [dc_color], FOREB_DIM
    movzx bx, cx
    shl   bx, 1
    mov   si, [entry_descs + bx]
    call  gprint
    pop  si
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

draw_footer:
    mov  word [fb_x], 16
    mov  ax, [screen_height]
    sub  ax, 42
    mov  [fb_y], ax
    mov  ax, [screen_width]
    sub  ax, 32
    mov  [fb_w], ax
    mov  word [fb_h], 1
    mov  byte [fb_c], FOREB_BORDER
    call fill_box
    mov  word [dc_x], 32
    mov  ax, [screen_height]
    sub  ax, 30
    mov  [dc_y], ax
    mov  byte [dc_color], FOREB_DIM
    mov  si, str_key_hint
    call gprint
    ret

draw_timer_display:
    push si
    mov  word [fb_x], TIMER_X - 8
    mov  word [fb_y], TIMER_Y - 2
    mov  word [fb_w], 220
    mov  word [fb_h], 14
    mov  byte [fb_c], FOREB_BG
    call fill_box
    mov  word [dc_x], TIMER_X - 8
    mov  word [dc_y], TIMER_Y
    mov  byte [dc_color], FOREB_TIMER
    mov  si, str_timer_pre
    call gprint
    mov  al, [timer_secs]
    add  al, '0'
    mov  [dc_char], al
    call draw_glyph
    add  word [dc_x], 8
    mov  byte [dc_color], FOREB_TIMER
    mov  si, str_timer_suf
    call gprint
    pop  si
    ret

; ============================================================================
; Boot menu interactive loop (VBE). Returns AX = selected entry index.
; ============================================================================
boot_menu_loop:
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  eax, [0x046C]
    pop  ds
    push cs
    pop  ds
    mov  [start_ticks], eax
    mov  byte [timer_secs], FOREB_DEFAULT_TIMEOUT
.bml_loop:
    mov  ah, 0x01
    int  0x16
    jz   .bml_timer_check
    mov  ah, 0x00
    int  0x16
    cmp  ah, KEY_UP
    je   .bml_up
    cmp  ah, KEY_DOWN
    je   .bml_down
    cmp  ah, KEY_ENTER
    je   .bml_enter
    cmp  ah, KEY_ESCAPE
    je   .bml_reset_timer
    jmp  .bml_loop
.bml_up:
    cmp  word [selected_entry], 0
    je   .bml_wrap_last
    dec  word [selected_entry]
    jmp  .bml_redraw
.bml_wrap_last:
    mov  word [selected_entry], BOOT_ENTRY_COUNT - 1
    jmp  .bml_redraw
.bml_down:
    inc  word [selected_entry]
    cmp  word [selected_entry], BOOT_ENTRY_COUNT
    jl   .bml_redraw
    mov  word [selected_entry], 0
    jmp  .bml_redraw
.bml_enter:
    mov  ax, [selected_entry]
    ret
.bml_reset_timer:
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  eax, [0x046C]
    pop  ds
    push cs
    pop  ds
    mov  [start_ticks], eax
    mov  byte [timer_secs], FOREB_DEFAULT_TIMEOUT
    jmp  .bml_loop
.bml_redraw:
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  eax, [0x046C]
    pop  ds
    push cs
    pop  ds
    mov  [start_ticks], eax
    mov  byte [timer_secs], FOREB_DEFAULT_TIMEOUT
    mov  word [pm_fn], pm_menu        ; redraw panel+entries+timer in 16-bit PM
    call run_in_pm
    jmp  .bml_loop
.bml_timer_check:
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  eax, [0x046C]
    pop  ds
    push cs
    pop  ds
    sub  eax, [start_ticks]
    xor  edx, edx
    mov  ebx, 18
    div  ebx
    mov  ecx, FOREB_DEFAULT_TIMEOUT
    sub  ecx, eax
    cmp  ecx, 0
    jle  .bml_timeout
    movzx ebx, byte [timer_secs]
    cmp  ecx, ebx
    je   .bml_loop
    mov  [timer_secs], cl
    mov  word [pm_fn], pm_timer       ; repaint only the timer box in 16-bit PM
    call run_in_pm
    jmp  .bml_loop
.bml_timeout:
    mov  ax, [selected_entry]
    ret

; ============================================================================
; vesa_menu: graphical forest boot menu. Returns AX = selected entry.
; Sets an 8bpp VBE mode, programs the forest DAC palette, draws the menu from
; 16-bit protected-mode excursions and reads keys/timer in real mode. For a boot
; entry it then paints the in-place graphical load indicator and LEAVES the
; framebuffer up so it stays visible while the kernel streams in; vesa_teardown
; (called after load_kernel, before the handoff) restores text mode 03h and
; clears the framebuffer info so the multiboot handoff is byte-identical to the
; proven text-mode path. Falls back to the text menu when no VBE mode is found.
; ============================================================================
vesa_menu:
    mov  bx, fb_prefs_menu
    call setup_vesa                  ; real mode; sets vesa_ok + mode + lfb + geom
    cmp  byte [vesa_ok], 0
    je   .vm_text
    call program_palette             ; real mode INT 10h: forest DAC (8bpp)
    mov  word [pm_fn], pm_all         ; paint full chrome once
    call run_in_pm
    call boot_menu_loop              ; AX = selected entry (real-mode input)
    cmp  ax, ENTRY_REBOOT            ; reboot entry loads nothing
    jae  .vm_ret
    push ax
    mov  word [pm_fn], pm_loading    ; in-place graphical load indicator
    call run_in_pm
    pop  ax
.vm_ret:
    ret
.vm_text:
    call text_mode_menu
    ret

; ============================================================================
; vesa_teardown: restore text mode 03h and clear framebuffer info (no-op if the
; graphical menu was never entered). Called after the kernel is loaded so the
; loading screen stays visible during the disk read, and so the multiboot
; handoff matches the proven text-mode path exactly.
; ============================================================================
vesa_teardown:
    cmp  byte [vesa_ok], 0
    je   .vt_done
    mov  ax, 0x0003
    int  0x10
    push es
    xor  cx, cx
    mov  es, cx
    and  dword [es:boot_info + foreboots_boot_info.flags], 0xFFFFFFFD
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr + 4], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_width], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_height], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_bpp], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_type], 2
    pop  es
    mov  byte [vesa_ok], 0
.vt_done:
    ret

; ============================================================================
; run_in_pm: run the 16-bit draw routine at [pm_fn] from PROTECTED mode.
; LFB stores (fs:[phys]) are invisible in real/unreal mode on QEMU but work in
; protected mode, so each repaint briefly enters 16-bit PM with:
;   CS  = 0x18 (16-bit code, base 0x8000)   -> ORG-relative code runs correctly
;   DS/ES = 0x20 (data, base 0x8000)        -> variable/string access unchanged
;   FS  = 0x10 (flat, base 0)               -> LFB at its physical address
;   SS  = 0x28 (data, base 0x70000)         -> same physical stack as real mode
; SP is never changed, so real-mode CALL/RET framing is preserved.
; ============================================================================
run_in_pm:
    pushf
    cli
    sidt [rp_idt]                    ; save real-mode IVT pointer
    lgdt [gdt_pm_desc]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x0018:.pm                  ; load 16-bit PM code selector (base 0x8000)
.pm:
    mov  ax, 0x0020
    mov  ds, ax
    mov  es, ax
    mov  ax, 0x0010
    mov  fs, ax
    mov  ax, 0x0028
    mov  ss, ax
    call [pm_fn]                     ; draw (writes LFB via fs:[phys])
    cli
    mov  ax, 0x0028                  ; 64K-limit data selectors for real return
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  eax, cr0
    and  eax, 0xFFFFFFFE
    mov  cr0, eax
    jmp  0x0800:.real                ; reload CS with a real-mode segment
.real:
    mov  ax, 0x7000
    mov  ss, ax                      ; restore real-mode stack (SP preserved)
    mov  ax, 0x0800
    mov  ds, ax
    xor  ax, ax
    mov  es, ax
    lidt [rp_idt]                    ; restore real-mode IVT
    popf
    ret

; PM draw dispatch targets (executed inside run_in_pm, 16-bit PM).
pm_all:
    call draw_background
    call draw_tree_logo
    call draw_title_bar
    call draw_menu_panel
    call draw_boot_entries
    call draw_footer
    call draw_timer_display
    ret
pm_menu:
    call draw_menu_panel
    call draw_boot_entries
    call draw_timer_display
    ret
pm_timer:
    call draw_timer_display
    ret
; In-place graphical "Loading" indicator (drawn once before the kernel is
; streamed). No per-chunk text output, so nothing ever scrolls on the BIOS path.
pm_loading:
    mov  word [fb_x], MENU_X + 6
    mov  word [fb_y], ENTRY_Y_START
    mov  word [fb_w], MENU_W - 12
    mov  word [fb_h], 150
    mov  byte [fb_c], FOREB_PANEL
    call fill_box
    mov  word [dc_x], MENU_X + 40
    mov  word [dc_y], ENTRY_Y_START + 44
    mov  byte [dc_color], FOREB_TITLE
    mov  si, str_loading
    call gprint
    mov  word [fb_x], MENU_X + 38
    mov  word [fb_y], ENTRY_Y_START + 70
    mov  word [fb_w], MENU_W - 76
    mov  word [fb_h], 18
    mov  byte [fb_c], FOREB_BORDER
    call rect_outline
    mov  word [fb_x], MENU_X + 40
    mov  word [fb_y], ENTRY_Y_START + 72
    mov  word [fb_w], MENU_W - 80
    mov  word [fb_h], 14
    mov  byte [fb_c], FOREB_SELECT
    call fill_box
    ret
%endif ; ===== end disabled VBE code =====

; ============================================================================
; Text-mode fallback menu. Returns AX = selected entry.
; ============================================================================
text_mode_menu:
    mov  ax, 0x0003
    int  0x10
    mov  ah, 0x01
    mov  cx, 0x2000
    int  0x10
.tm_redraw:
    mov  ax, 0x0003
    int  0x10
    mov  si, str_textbanner
    call bios_print
    mov  si, str_nokernel
    call bios_print
    mov  cx, 0
.tm_entries:
    cmp  cx, BOOT_ENTRY_COUNT
    jge  .tm_wait_key
    cmp  cx, [selected_entry]
    jne  .tm_no_arrow
    mov  si, str_arrow
    jmp  .tm_print_arrow
.tm_no_arrow:
    mov  si, str_noarrow
.tm_print_arrow:
    call bios_print
    movzx bx, cx
    shl   bx, 1
    mov   si, [entry_labels + bx]
    call  bios_print
    mov   si, str_crlf
    call  bios_print
    inc  cx
    jmp  .tm_entries
.tm_wait_key:
    mov  si, str_textfooter
    call bios_print
    mov  ah, 0x00
    int  0x16
    out  0xE9, al             ; DEBUG: ASCII code of key
    mov  al, ah
    out  0xE9, al             ; DEBUG: scan code of key
    push cs
    pop  ds
    cmp  ah, KEY_UP
    je   .tm_up
    cmp  ah, KEY_DOWN
    je   .tm_down
    cmp  ah, KEY_ENTER
    je   .tm_done
    jmp  .tm_wait_key
.tm_up:
    mov  al, 0x07
    call beep
    cmp  word [selected_entry], 0
    je   .tm_wait_key
    dec  word [selected_entry]
    jmp  .tm_redraw
.tm_down:
    mov  al, 0x07
    call beep
    inc  word [selected_entry]
    cmp  word [selected_entry], BOOT_ENTRY_COUNT
    jl   .tm_redraw
    mov  word [selected_entry], 0
    jmp  .tm_redraw
.tm_done:
    mov  al, 0x0D
    call beep
    mov  ax, [selected_entry]
    ret

; ============================================================================
; beep: output AL via INT 10h teletype (BEL=0x07 for standard beep)
; ============================================================================
beep:
    push ax
    push bx
    mov  ah, 0x0E
    xor  bx, bx
    int  0x10
    pop  bx
    pop  ax
    ret

%if 0  ; ===== FORBSHELL recovery shell — removed to fit the graphical menu =====
; ============================================================================
; recovery_menu: FORB recovery interface. Returns to caller on "Retry" / Esc.
; ============================================================================
recovery_menu:
    mov  ax, 0x0003
    int  0x10
    mov  word [recov_sel], 0
.rm_redraw:
    mov  ax, 0x0003
    int  0x10
    mov  si, str_recoverbanner
    call bios_print
    mov  cx, 0
.rm_entries:
    cmp  cx, 4
    jge  .rm_wait_key
    cmp  cx, [recov_sel]
    jne  .rm_no_arrow
    mov  si, str_arrow
    jmp  .rm_print
.rm_no_arrow:
    mov  si, str_noarrow
.rm_print:
    call bios_print
    movzx bx, cx
    shl   bx, 1
    mov   si, [recov_labels + bx]
    call  bios_print
    mov   si, str_crlf
    call  bios_print
    inc  cx
    jmp  .rm_entries
.rm_wait_key:
    mov  si, str_recoverfooter
    call bios_print
    mov  ah, 0x00
    int  0x16
    cmp  ah, KEY_UP
    je   .rm_up
    cmp  ah, KEY_DOWN
    je   .rm_down
    cmp  ah, KEY_ENTER
    je   .rm_select
    cmp  ah, KEY_ESCAPE
    je   .rm_exit
    jmp  .rm_wait_key
.rm_up:
    mov  al, 0x07
    call beep
    cmp  word [recov_sel], 0
    je   .rm_wait_key
    dec  word [recov_sel]
    jmp  .rm_redraw
.rm_down:
    mov  al, 0x07
    call beep
    inc  word [recov_sel]
    cmp  word [recov_sel], 4
    jl   .rm_redraw
    mov  word [recov_sel], 0
    jmp  .rm_redraw
.rm_select:
    mov  al, 0x0D
    call beep
    mov  ax, [recov_sel]
    cmp  ax, 0
    je   forbshell            ; launch FORBSHELL
    cmp  ax, 1
    je   .rm_reboot
    cmp  ax, 2
    je   .rm_about
    ret                        ; 3 = back to menu
.rm_reboot:
    mov  si, str_rebooting
    call bios_print
    int  0x19
    jmp  $
.rm_about:
    mov  si, str_about
    call bios_print
    mov  ah, 0x00
    int  0x16
    jmp  .rm_redraw
.rm_exit:
    ret

; ============================================================================
; FORBSHELL — ForeB Recovery Shell
; Full command-line interface for recovery and diagnostics.
; ============================================================================
forbshell:
    mov  ax, 0x0003
    int  0x10
    push cs
    pop  ds
    mov  al, 'S'              ; DEBUG: forbshell entered
    out  0xE9, al
    mov  byte [shell_exit], 0
    mov  si, str_shell_banner
    call bios_print
.fs_loop:
    mov  si, str_shell_prompt
    call bios_print
    mov  di, cmd_buf
    xor  cx, cx
.fs_read:
    xor  ax, ax
    int  0x16
    xchg ax, bp              ; save keypress in BP (preserve AX for key checks)
    push cs
    pop  ds                  ; fix DS (INT may clobber it)
    xchg ax, bp              ; restore keypress to AX
    cmp  ah, 0x1C
    je   .fs_enter
    cmp  ah, 0x0E
    je   .fs_bs
    cmp  al, 0x20
    jb   .fs_read
    cmp  al, 0x7E
    ja   .fs_read
    cmp  cx, 78
    jae  .fs_full
    mov  [di], al
    inc  di
    inc  cx
    mov  ah, 0x0E
    int  0x10
    push cs
    pop  ds
    jmp  .fs_read
.fs_bs:
    test cx, cx
    jz   .fs_read
    dec  di
    dec  cx
    mov  al, 0x08
    mov  ah, 0x0E
    int  0x10
    push cs
    pop  ds
    mov  al, 0x20
    int  0x10
    push cs
    pop  ds
    mov  al, 0x08
    int  0x10
    push cs
    pop  ds
    jmp  .fs_read
.fs_full:
    mov  al, 0x07
    mov  ah, 0x0E
    int  0x10
    jmp  .fs_read
.fs_enter:
    mov  byte [di], 0
    mov  si, str_crlf
    call bios_print
    cmp  byte [cmd_buf], 0
    je   .fs_loop
    call shell_dispatch
    cmp  byte [shell_exit], 1
    je   .fs_done
    jmp  .fs_loop
.fs_done:
    ret

; ============================================================================
; shell_dispatch: parse cmd_buf, find matching command, call handler
; ============================================================================
shell_dispatch:
    push ax
    push bx
    push si
    push di
    push cs
    pop  ds
    mov  si, cmd_buf
.sd_skip:
    cmp  byte [si], ' '
    jne  .sd_find
    inc  si
    jmp  .sd_skip
.sd_find:
    mov  bx, cmd_table
.sd_loop:
    mov  di, [bx]
    test di, di
    jz   .sd_unknown
    push si
    push di
    call shell_strcmp
    pop  di
    pop  si
    je   .sd_match
    add  bx, 4
    jmp  .sd_loop
.sd_match:
    call [bx + 2]
    pop  di
    pop  si
    pop  bx
    pop  ax
    ret
.sd_unknown:
    mov  si, str_unknown
    call bios_print
    mov  si, cmd_buf
    call bios_print
    mov  si, str_crlf
    call bios_print
    pop  di
    pop  si
    pop  bx
    pop  ax
    ret

; ============================================================================
; shell_strcmp: case-insensitive null-terminated compare DS:SI vs DS:DI
; Returns ZF=1 if equal. Clobbers AL, BL.
; ============================================================================
shell_strcmp:
.sc_loop:
    lodsb
    mov  bl, [di]
    inc  di
    cmp  al, 'A'
    jb   .sc_al
    cmp  al, 'Z'
    ja   .sc_al
    add  al, 32
.sc_al:
    cmp  bl, 'A'
    jb   .sc_bl
    cmp  bl, 'Z'
    ja   .sc_bl
    add  bl, 32
.sc_bl:
    cmp  al, bl
    jne  .sc_neq
    test al, al
    jz   .sc_eq
    jmp  .sc_loop
.sc_eq:
    ret
.sc_neq:
    ret

; ============================================================================
; print_hex32: print EAX as 8-digit hex (DS=0x0800, uses INT 10h)
; ============================================================================
print_hex32:
    push eax
    push bx
    push cx
    mov  cx, 8
.ph_loop:
    rol  eax, 4
    mov  bl, al
    and  bl, 0x0F
    cmp  bl, 10
    jb   .ph_dig
    add  bl, 'A' - 10
    jmp  .ph_out
.ph_dig:
    add  bl, '0'
.ph_out:
    mov  al, bl
    mov  ah, 0x0E
    int  0x10
    loop .ph_loop
    pop  cx
    pop  bx
    pop  eax
    ret

; ============================================================================
; print_dec: print AX as unsigned decimal
; ============================================================================
print_dec:
    push ax
    push bx
    push cx
    push dx
    mov  bx, 10
    xor  cx, cx
.pd_loop:
    xor  dx, dx
    div  bx
    push dx
    inc  cx
    test ax, ax
    jnz  .pd_loop
.pd_print:
    pop  ax
    add  al, '0'
    mov  ah, 0x0E
    int  0x10
    loop .pd_print
    pop  dx
    pop  cx
    pop  bx
    pop  ax
    ret

; ============================================================================
; print_hexbyte: print AL as 2-digit hex
; ============================================================================
print_hexbyte:
    push ax
    push bx
    push cx
    mov  cx, 2
.phb_loop:
    rol  al, 4
    mov  bl, al
    and  bl, 0x0F
    cmp  bl, 10
    jb   .phb_dig
    add  bl, 'A' - 10
    jmp  .phb_out
.phb_dig:
    add  bl, '0'
.phb_out:
    mov  al, bl
    push cx
    mov  ah, 0x0E
    int  0x10
    pop  cx
    loop .phb_loop
    pop  cx
    pop  bx
    pop  ax
    ret

; ============================================================================
; FORBSHELL command handlers
; ============================================================================
cmd_help:
    mov  si, str_help_text
    call bios_print
    ret

cmd_ver:
    mov  si, str_version
    call bios_print
    ret

cmd_info:
    push es
    xor  ax, ax
    mov  es, ax
    mov  si, str_info_hdr
    call bios_print
    ; Magic
    mov  si, str_info_magic
    call bios_print
    mov  eax, [es:boot_info + foreboots_boot_info.magic]
    call print_hex32
    mov  si, str_crlf
    call bios_print
    ; Memory
    mov  si, str_info_mem
    call bios_print
    mov  ax, [es:boot_info + foreboots_boot_info.mem_lower]
    call print_dec
    mov  si, str_info_k_lower
    call bios_print
    mov  ax, [es:boot_info + foreboots_boot_info.mem_upper]
    call print_dec
    mov  si, str_info_k_upper
    call bios_print
    ; Disk
    mov  si, str_info_disk
    call bios_print
    movzx eax, byte [es:boot_info + foreboots_boot_info.boot_disk]
    call print_hex32
    mov  si, str_crlf
    call bios_print
    ; E820 count
    mov  si, str_info_mmap
    call bios_print
    mov  ax, [es:boot_info + foreboots_boot_info.mmap_count]
    call print_dec
    mov  si, str_crlf
    call bios_print
    pop  es
    ret

cmd_mem:
    push es
    xor  ax, ax
    mov  es, ax
    mov  si, str_mem_hdr
    call bios_print
    movzx cx, word [es:boot_info + foreboots_boot_info.mmap_count]
    test cx, cx
    jz   .cm_none
    mov  si, foreb_mmap     ; ES:SI = mmap array at 0x1100
.cm_loop:
    push cx
    mov  si, str_mem_entry
    call bios_print
    ; Base (low 32 bits)
    mov  eax, [es:si + foreboots_mmap_entry.base]
    call print_hex32
    mov  si, str_mem_sep
    call bios_print
    ; Length (low 32 bits)
    mov  eax, [es:si + foreboots_mmap_entry.length]
    call print_hex32
    mov  si, str_mem_sep
    call bios_print
    ; Type
    movzx eax, byte [es:si + foreboots_mmap_entry.type]
    call print_hex32
    mov  si, str_crlf
    call bios_print
    add  si, foreboots_mmap_entry_size
    pop  cx
    loop .cm_loop
    pop  es
    ret
.cm_none:
    mov  si, str_mem_none
    call bios_print
    pop  es
    ret

cmd_cpu:
    push es
    xor  ax, ax
    mov  es, ax
    mov  si, str_cpu_hdr
    call bios_print
    mov  si, str_cpu_cpuid
    call bios_print
    mov  al, [es:boot_info + foreboots_boot_info.cpuid_available]
    test al, al
    jz   .cm_cpu_no
    mov  si, str_yes
    jmp  .cm_cpu_pr
.cm_cpu_no:
    mov  si, str_no
.cm_cpu_pr:
    call bios_print
    mov  si, str_crlf
    call bios_print
    mov  si, str_cpu_lm
    call bios_print
    mov  al, [es:boot_info + foreboots_boot_info.long_mode_available]
    test al, al
    jz   .cm_lm_no
    mov  si, str_yes
    jmp  .cm_lm_pr
.cm_lm_no:
    mov  si, str_no
.cm_lm_pr:
    call bios_print
    mov  si, str_crlf
    call bios_print
    mov  si, str_cpu_pae
    call bios_print
    mov  al, [es:boot_info + foreboots_boot_info.pae_available]
    test al, al
    jz   .cm_pae_no
    mov  si, str_yes
    jmp  .cm_pae_pr
.cm_pae_no:
    mov  si, str_no
.cm_pae_pr:
    call bios_print
    mov  si, str_crlf
    call bios_print
    pop  es
    ret

cmd_disk:
    push es
    xor  ax, ax
    mov  es, ax
    mov  si, str_disk_hdr
    call bios_print
    mov  si, str_disk_drive
    call bios_print
    movzx eax, byte [es:boot_info + foreboots_boot_info.boot_disk]
    call print_hex32
    mov  si, str_crlf
    call bios_print
    mov  si, str_disk_lba
    call bios_print
    ; Check LBA support via INT 13h
    mov  ah, 0x41
    mov  bx, 0x55AA
    mov  dl, [es:boot_info + foreboots_boot_info.boot_disk]
    int  0x13
    jc   .cd_no
    cmp  bx, 0xAA55
    jne  .cd_no
    mov  si, str_yes
    jmp  .cd_pr
.cd_no:
    mov  si, str_no
.cd_pr:
    call bios_print
    mov  si, str_crlf
    call bios_print
    pop  es
    ret

cmd_clear:
    mov  ax, 0x0003
    int  0x10
    ret

cmd_reboot:
    mov  si, str_rebooting
    call bios_print
    int  0x19
    jmp  $

cmd_about:
    mov  si, str_about
    call bios_print
    ret

cmd_date:
    mov  si, str_date_hdr
    call bios_print
    ; Read RTC date/time from CMOS (BCD format)
    mov  al, 0x04            ; Hours
    out  0x70, al
    in   al, 0x71
    call print_hexbyte
    mov  al, ':'
    mov  ah, 0x0E
    int  0x10
    mov  al, 0x02            ; Minutes
    out  0x70, al
    in   al, 0x71
    call print_hexbyte
    mov  al, ':'
    mov  ah, 0x0E
    int  0x10
    mov  al, 0x00            ; Seconds
    out  0x70, al
    in   al, 0x71
    call print_hexbyte
    mov  si, str_date_space
    call bios_print
    mov  al, 0x08            ; Month
    out  0x70, al
    in   al, 0x71
    call print_hexbyte
    mov  al, '/'
    mov  ah, 0x0E
    int  0x10
    mov  al, 0x07            ; Day
    out  0x70, al
    in   al, 0x71
    call print_hexbyte
    mov  al, '/'
    mov  ah, 0x0E
    int  0x10
    mov  al, 0x09            ; Year
    out  0x70, al
    in   al, 0x71
    call print_hexbyte
    mov  si, str_crlf
    call bios_print
    ret

cmd_beep:
    mov  al, 0x07
    mov  ah, 0x0E
    int  0x10
    ret

cmd_exit:
    mov  byte [shell_exit], 1
    mov  si, str_exit_msg
    call bios_print
    ret

; ============================================================================
; FORBSHELL command table (name_ptr, handler_ptr pairs, null-terminated)
; ============================================================================
cmd_table:
    dw cmd_name_help,   cmd_help
    dw cmd_name_ver,    cmd_ver
    dw cmd_name_info,   cmd_info
    dw cmd_name_mem,    cmd_mem
    dw cmd_name_cpu,    cmd_cpu
    dw cmd_name_disk,   cmd_disk
    dw cmd_name_clear,  cmd_clear
    dw cmd_name_cls,    cmd_clear
    dw cmd_name_reboot, cmd_reboot
    dw cmd_name_about,  cmd_about
    dw cmd_name_date,   cmd_date
    dw cmd_name_beep,   cmd_beep
    dw cmd_name_exit,   cmd_exit
    dw 0, 0

cmd_name_help:   db "help", 0
cmd_name_ver:    db "ver", 0
cmd_name_info:   db "info", 0
cmd_name_mem:    db "mem", 0
cmd_name_cpu:    db "cpu", 0
cmd_name_disk:   db "disk", 0
cmd_name_clear:  db "clear", 0
cmd_name_cls:    db "cls", 0
cmd_name_reboot: db "reboot", 0
cmd_name_about:  db "about", 0
cmd_name_date:   db "date", 0
cmd_name_beep:   db "beep", 0
cmd_name_exit:   db "exit", 0

; ============================================================================
; FORBSHELL strings
; ============================================================================
str_shell_banner: db 0x0D, 0x0A, "FORBSHELL v1.0 - ForeB Recovery Shell", 0x0D, 0x0A
                   db "Type 'help' for available commands.", 0x0D, 0x0A, 0x0D, 0x0A, 0
str_shell_prompt: db "forb$ ", 0
str_unknown:      db "unknown command: ", 0
str_help_text:    db "Available commands:", 0x0D, 0x0A
                   db "  help     Show this help", 0x0D, 0x0A
                   db "  ver      ForeB version", 0x0D, 0x0A
                   db "  info     Boot information", 0x0D, 0x0A
                   db "  mem      Memory map (E820)", 0x0D, 0x0A
                   db "  cpu      CPU capabilities", 0x0D, 0x0A
                   db "  disk     Disk info", 0x0D, 0x0A
                   db "  clear    Clear screen", 0x0D, 0x0A
                   db "  reboot   Reboot system", 0x0D, 0x0A
                   db "  about    About ForeB", 0x0D, 0x0A
                   db "  date     RTC date/time", 0x0D, 0x0A
                   db "  beep     Test beep", 0x0D, 0x0A
                   db "  exit     Return to menu", 0x0D, 0x0A, 0
str_version:      db "ForeB v2.0 - Forest Bootloader", 0x0D, 0x0A, 0
str_exit_msg:     db 0x0D, 0x0A, "Returning to menu...", 0x0D, 0x0A, 0
str_info_hdr:     db "ForeB Boot Info:", 0x0D, 0x0A, 0
str_info_magic:   db "  Magic: 0x", 0
str_info_mem:     db "  Memory: ", 0
str_info_k_lower: db "K lower, ", 0
str_info_k_upper: db "K upper", 0x0D, 0x0A, 0
str_info_disk:    db "  Boot disk: 0x", 0
str_info_mmap:    db "  E820 entries: ", 0
str_mem_hdr:      db "E820 Memory Map:", 0x0D, 0x0A, 0
str_mem_entry:    db "  ", 0
str_mem_sep:      db "  ", 0
str_mem_none:     db "  (no entries)", 0x0D, 0x0A, 0
str_cpu_hdr:      db "CPU Capabilities:", 0x0D, 0x0A, 0
str_cpu_cpuid:    db "  CPUID: ", 0
str_cpu_lm:       db "  Long mode: ", 0
str_cpu_pae:      db "  PAE: ", 0
str_yes:          db "yes", 0x0D, 0x0A, 0
str_no:           db "no", 0x0D, 0x0A, 0
str_disk_hdr:     db "Disk Info:", 0x0D, 0x0A, 0
str_disk_drive:   db "  Drive: 0x", 0
str_disk_lba:     db "  LBA support: ", 0
str_date_hdr:     db "RTC: ", 0
str_date_space:   db "  ", 0

; ============================================================================
; FORBSHELL data
; ============================================================================
cmd_buf:    times 80 db 0
shell_exit: db 0
%endif ; ===== end FORBSHELL recovery shell =====

; ============================================================================
; bios_print: null-terminated string SI via INT 10h teletype (DS=0x0800)
; ============================================================================
bios_print:
    push ax
    push bx
    push ds
    push cs
    pop  ds
.bp_loop:
    lodsb
    test al, al
    jz   .bp_done
    mov  ah, 0x0E
    xor  bx, bx
    mov  bl, 0x07
    int  0x10
    push cs
    pop  ds
    jmp  .bp_loop
.bp_done:
    pop  ds
    pop  bx
    pop  ax
    ret

; ============================================================================
; Disk loading: LBA reads with CHS fallback, chunked to 63 sectors per call.
;   EAX = starting LBA (relative to boot image), ECX = total sectors,
;   ESI = physical destination.
; The CD LBA base offset (stored at LBA_BASE_OFFSET by stage1) is added to
; EAX once so the read hits the correct ISO sector on El Torito no-emulation
; boot.  On hard disk the offset is 0 and this is a no-op.
; Returns CF=0 on success, CF=1 on error. Preserves EBX/EDX, advances EAX/ECX/ESI.
; ============================================================================
disk_load:
    push ebx
    push edx
    add  eax, [es:LBA_BASE_OFFSET]   ; add CD base offset (0 for hard disk)
    mov  ebx, ecx                  ; EBX = remaining sectors
.dl_loop:
    test ebx, ebx
    jz   .dl_done
    mov  ecx, ebx
    cmp  ecx, 63
    jbe  .dl_chunk_ok
    mov  ecx, 63
.dl_chunk_ok:
    push ebx
    lba_read_one (boot_info + foreboots_boot_info.boot_disk)
    pop  ebx
    jc   .dl_fail
    add  eax, ecx                  ; LBA += chunk
    mov  edx, ecx
    shl  edx, 9                    ; bytes = chunk * 512
    add  esi, edx                  ; dest += bytes
    sub  ebx, ecx                  ; remaining -= chunk
    jmp  .dl_loop
.dl_done:
    pop  edx
    pop  ebx
    clc
    ret
.dl_fail:
    pop  edx
    pop  ebx
    stc
    ret

; ============================================================================
; load_stage3 / load_kernel / load_initrd
; ============================================================================
load_stage3:
    mov  eax, STAGE3_START_SECTOR
    mov  ecx, STAGE3_SECTOR_COUNT
    mov  esi, STAGE3_LOAD_PHYS
    call disk_load
    jc   .ls3_fail
    ret
.ls3_fail:
    mov  si, str_s3err
    call bios_print
    ret                        ; non-fatal: stage3 may already be in memory

; ============================================================================
; load_kernel: stream the kernel ELF (at disk sector KERNEL_START_SECTOR)
; directly into each PT_LOAD segment's physical address. Handles kernels of
; ANY size, exactly like GRUB: instead of buffering the whole file, each
; segment is read (in <=63-sector chunks) into a low bounce buffer and copied
; up to its destination — which may be far above 1 MiB — via unreal mode
; (FS = 4 GiB flat). No 256 KiB / low-memory limit.
;
; On success: boot_info.kernel_entry / .kernel_is64bit are set and the
; FOREB_BIF_KERNEL_PRELOADED flag is raised so stage3 skips its own ELF copy.
; Requires A20 enabled. Ensures unreal mode itself. Real mode, ES=0.
; ============================================================================
; ----------------------------------------------------------------------------
; foreb_enable_unreal: put FS into "unreal mode" (base 0, 4 GiB limit) using the
; active PM handoff GDT (gdt_pm_desc, flat data selector 0x10). Briefly enters
; protected mode WITHOUT reloading CS, loads FS, then returns to real mode; the
; FS descriptor cache keeps the 4 GiB limit so `mov [fs:e**]` reaches all RAM.
; The real-mode IVT is left intact, so INT 13h keeps working afterwards.
; Interrupts are disabled across the PE window. Preserves EAX/DS.
; ----------------------------------------------------------------------------
foreb_enable_unreal:
    cli
    push eax
    push bx
    lgdt [gdt_pm_desc]
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax                    ; PE=1 (16-bit PM, CS cache unchanged)
    jmp  $+2                         ; flush prefetch queue after mode switch
    mov  bx, 0x10
    mov  fs, bx                      ; FS := flat 4 GiB data descriptor (sel 0x10)
    mov  gs, bx                      ; GS too (spare flat segment)
    mov  eax, cr0
    and  eax, 0xFFFFFFFE
    mov  cr0, eax                    ; PE=0; FS/GS caches retain 4 GiB limit
    jmp  $+2                         ; flush prefetch after returning to real mode
    pop  bx
    pop  eax
    mov  byte [unreal_ok], 1
    ret                              ; leave IF as caller set it (load runs cli'd)

; ----------------------------------------------------------------------------
; foreb_read: robust LBA disk read (INT 13h AH=42h) used by the kernel loader.
; A direct DAP read like stage1's — no AH=41h precheck / CHS fallback (that
; path in lba_read_one corrupts the sector count on some BIOSes). Reads to a
; real-mode-addressable (<1 MiB) buffer; the caller blits up via unreal mode.
;   In: EAX = LBA, CX = sector count (<=127), ESI = physical dest (<1 MiB).
;   Uses drive boot_info.boot_disk.  Returns CF=1 on error. Preserves GP regs.
; ----------------------------------------------------------------------------
foreb_read:
    pushad
    push ds
    push es
    xor  bx, bx
    mov  ds, bx                      ; DS=0 for DAP + boot_disk
    mov  es, bx
    mov  di, DAP_BUF
    mov  byte  [di + 0], 0x10
    mov  byte  [di + 1], 0
    mov  [di + 2], cx                ; sector count
    mov  edx, esi
    shr  edx, 4
    mov  ebx, esi
    and  ebx, 0x0F
    mov  [di + 4], bx                ; dest offset
    mov  [di + 6], dx                ; dest segment
    mov  [di + 8], eax               ; LBA (low 32)
    mov  dword [di + 12], 0          ; LBA (high 32)
    mov  dl, [boot_info + foreboots_boot_info.boot_disk]
    mov  si, DAP_BUF
    mov  ah, 0x42
    int  0x13                        ; CF survives the pops/popad below
    pop  es
    pop  ds
    popad
    ; INT 13h can clobber FS (wiping the unreal descriptor cache) and re-enable
    ; interrupts. Force IF off and rebuild unreal FS so the caller's subsequent
    ; [fs:e**] high-memory accesses stay valid. Preserve CF (read status).
    cli
    pushf
    call foreb_enable_unreal
    popf
    ret

load_kernel:
    push cs
    pop  ds                          ; DS = 0x0800 (stage2 data/vars + gdt_pm_desc)
    xor  ax, ax
    mov  es, ax                      ; ES = 0: boot_info writes use [es:...]
    cli                              ; keep IF off for the whole load: a real-mode
                                     ; IRQ handler touching FS would wipe unreal.

    ; Slurp the ELF header + program-header table into the low header buffer.
    ; foreb_read restores unreal FS on return, so the [fs:*] reads below (which
    ; must immediately follow, with no BIOS INT in between) resolve correctly.
    mov  eax, KERNEL_START_SECTOR
    mov  ecx, KERNEL_HDR_SECTORS
    mov  esi, KERNEL_HDR_BUF
    call foreb_read
    jc   .fail

    mov  edi, KERNEL_HDR_BUF
    cmp  dword [fs:edi], 0x464C457F  ; ELF magic 0x7F 'E' 'L' 'F'
    je   .khdr_ok
    ; Probe: firmwares that expose the WHOLE CD as the boot drive (real HW)
    ; need every LBA rebased by the El Torito boot image LBA; stage1 stored
    ; that raw 2048-sector LBA at CD_BOOT_LBA.  Retry once with base=LBA*4.
    mov  eax, [es:CD_BOOT_LBA]
    test eax, eax
    jz   .fail
    shl  eax, 2
    mov  [es:LBA_BASE_OFFSET], eax
    mov  eax, KERNEL_START_SECTOR
    mov  ecx, KERNEL_HDR_SECTORS
    mov  esi, KERNEL_HDR_BUF
    call foreb_read
    jc   .fail
    mov  edi, KERNEL_HDR_BUF
    cmp  dword [fs:edi], 0x464C457F
    jne  .fail
.khdr_ok:
    mov  al, [fs:edi + 4]            ; EI_CLASS
    cmp  al, ELFCLASS64
    je   .k64

; --- ELF32 ---
.k32:
    mov  dword [es:boot_info + foreboots_boot_info.kernel_is64bit], 0
    mov  eax, [fs:edi + Elf32_Ehdr.e_entry]
    mov  [es:boot_info + foreboots_boot_info.kernel_entry], eax
    movzx ecx, word [fs:edi + Elf32_Ehdr.e_phnum]
    mov  [k_count], ecx
    movzx eax, word [fs:edi + Elf32_Ehdr.e_phentsize]
    mov  [k_entsize], eax
    mov  eax, [fs:edi + Elf32_Ehdr.e_phoff]
    add  eax, KERNEL_HDR_BUF
    mov  [k_phdr], eax
.k32_loop:
    mov  ecx, [k_count]
    test ecx, ecx
    jz   .done
    mov  edi, [k_phdr]
    cmp  dword [fs:edi + Elf32_Phdr.p_type], PT_LOAD
    jne  .k32_next
    mov  eax, [fs:edi + Elf32_Phdr.p_offset]
    mov  edx, [fs:edi + Elf32_Phdr.p_paddr]
    mov  ecx, [fs:edi + Elf32_Phdr.p_filesz]
    mov  ebp, [fs:edi + Elf32_Phdr.p_memsz]
    call stream_segment
    jc   .fail
.k32_next:
    mov  eax, [k_phdr]
    add  eax, [k_entsize]
    mov  [k_phdr], eax
    dec  dword [k_count]
    jmp  .k32_loop

; --- ELF64 (low 32 bits of the 64-bit fields; Forest kernels live <4 GiB) ---
.k64:
    mov  dword [es:boot_info + foreboots_boot_info.kernel_is64bit], 1
    mov  eax, [fs:edi + Elf64_Ehdr.e_entry]
    mov  [es:boot_info + foreboots_boot_info.kernel_entry], eax
    movzx ecx, word [fs:edi + Elf64_Ehdr.e_phnum]
    mov  [k_count], ecx
    movzx eax, word [fs:edi + Elf64_Ehdr.e_phentsize]
    mov  [k_entsize], eax
    mov  eax, [fs:edi + Elf64_Ehdr.e_phoff]
    add  eax, KERNEL_HDR_BUF
    mov  [k_phdr], eax
.k64_loop:
    mov  ecx, [k_count]
    test ecx, ecx
    jz   .done
    mov  edi, [k_phdr]
    cmp  dword [fs:edi + Elf64_Phdr.p_type], PT_LOAD
    jne  .k64_next
    mov  eax, [fs:edi + Elf64_Phdr.p_offset]
    mov  edx, [fs:edi + Elf64_Phdr.p_paddr]
    mov  ecx, [fs:edi + Elf64_Phdr.p_filesz]
    mov  ebp, [fs:edi + Elf64_Phdr.p_memsz]
    call stream_segment
    jc   .fail
.k64_next:
    mov  eax, [k_phdr]
    add  eax, [k_entsize]
    mov  [k_phdr], eax
    dec  dword [k_count]
    jmp  .k64_loop

.done:
    or   dword [es:boot_info + foreboots_boot_info.flags], FOREB_BIF_KERNEL_PRELOADED
    mov  dword [es:boot_info + foreboots_boot_info.kernel_load_addr], KERNEL_HDR_BUF
    sti
    ret
.fail:
    sti
    mov  si, str_kernerr
    call bios_print
    ret                        ; non-fatal: continue (stage3 will report)

; ============================================================================
; stream_segment: copy p_filesz file bytes from kernel offset EAX to physical
; EDX, then zero-fill (p_memsz - p_filesz). Reads through KERNEL_BOUNCE_BUF in
; <=KERNEL_BOUNCE_SECTORS chunks and blits up via unreal FS. Never reads past
; what the segment needs.  In: EAX=file off, EDX=dest, ECX=filesz, EBP=memsz.
; DS must be 0x0800.  Returns CF=1 on disk error.
; ============================================================================
stream_segment:
    mov  [ls_dest], edx
    mov  [ls_rem], ecx
    mov  [ls_filesz], ecx
    mov  [ls_memsz], ebp
    mov  ebx, eax
    and  ebx, 511
    mov  [ls_skip], ebx              ; intra-sector offset of first byte
    shr  eax, 9
    add  eax, KERNEL_START_SECTOR
    mov  [ls_lba], eax
.seg_loop:
    mov  eax, [ls_rem]
    test eax, eax
    jz   .seg_bss
    ; sectors to read = ceil((skip + rem)/512), clamped to bounce capacity
    mov  eax, [ls_skip]
    add  eax, [ls_rem]
    add  eax, 511
    shr  eax, 9
    cmp  eax, KERNEL_BOUNCE_SECTORS
    jbe  .cnt_ok
    mov  eax, KERNEL_BOUNCE_SECTORS
.cnt_ok:
    mov  [ls_chunk], eax
    mov  ecx, eax
    mov  eax, [ls_lba]
    mov  esi, KERNEL_BOUNCE_BUF
    call foreb_read                  ; reads into low bounce (real-mode dest)
    jc   .seg_fail
    ; bytes available this round = chunk*512 - skip ; copy min(avail, rem)
    mov  eax, [ls_chunk]
    shl  eax, 9
    sub  eax, [ls_skip]
    mov  edx, [ls_rem]
    cmp  eax, edx
    jbe  .copy_ok
    mov  eax, edx
.copy_ok:
    mov  esi, KERNEL_BOUNCE_BUF
    add  esi, [ls_skip]
    mov  edi, [ls_dest]
    mov  ecx, eax
    call copy_flat                   ; fs:ESI (low) -> fs:EDI (high), ECX bytes
    add  [ls_dest], eax
    sub  [ls_rem], eax
    mov  ecx, [ls_chunk]
    add  [ls_lba], ecx
    mov  dword [ls_skip], 0
    jmp  .seg_loop
.seg_bss:
    mov  eax, [ls_memsz]
    sub  eax, [ls_filesz]            ; BSS bytes; ls_dest already at file end
    jbe  .seg_ok
    mov  ecx, eax
    mov  edi, [ls_dest]
    call zero_flat
.seg_ok:
    clc
    ret
.seg_fail:
    stc
    ret

; ============================================================================
; copy_flat: copy ECX bytes from linear ESI to linear EDI through FS (flat).
; zero_flat: zero  ECX bytes at linear EDI through FS (flat).
; Both require unreal mode (FS base 0, 4 GiB limit). Preserve all GP regs.
; ============================================================================
copy_flat:
    push eax
    push ecx
    push esi
    push edi
.cf_dw:
    cmp  ecx, 4
    jb   .cf_by
    mov  eax, [fs:esi]
    mov  [fs:edi], eax
    add  esi, 4
    add  edi, 4
    sub  ecx, 4
    jmp  .cf_dw
.cf_by:
    test ecx, ecx
    jz   .cf_done
    mov  al, [fs:esi]
    mov  [fs:edi], al
    inc  esi
    inc  edi
    dec  ecx
    jmp  .cf_by
.cf_done:
    pop  edi
    pop  esi
    pop  ecx
    pop  eax
    ret

zero_flat:
    push eax
    push ecx
    push edi
    xor  eax, eax
.zf_dw:
    cmp  ecx, 4
    jb   .zf_by
    mov  [fs:edi], eax
    add  edi, 4
    sub  ecx, 4
    jmp  .zf_dw
.zf_by:
    test ecx, ecx
    jz   .zf_done
    mov  [fs:edi], al
    inc  edi
    dec  ecx
    jmp  .zf_by
.zf_done:
    pop  edi
    pop  ecx
    pop  eax
    ret

load_initrd:
%if FOREB_INITRD_START_SECTOR == 0
    ret
%else
    mov  eax, FOREB_INITRD_START_SECTOR
    mov  ecx, INITRD_MAX_SECTORS
    mov  esi, INITRD_LOAD_PHYS
    call disk_load
    jc   .li_done
    mov  dword [es:boot_info + foreboots_boot_info.initrd_addr], INITRD_LOAD_PHYS
    mov  dword [es:boot_info + foreboots_boot_info.initrd_size], (INITRD_MAX_SECTORS * 512)
    or   dword [es:boot_info + foreboots_boot_info.flags], FOREB_BIF_INITRD
.li_done:
    ret
%endif

; ============================================================================
; launch_boot_entry: AX = entry index. Apply selection.
;   default -> re-select 32bpp kernel mode, cmdline ""
;   nofb    -> text mode 03h, clear framebuffer info, cmdline "nofb"
;   safe    -> re-select 32bpp kernel mode, cmdline "safe", safe flag
;   reboot  -> INT 19h
; ============================================================================
launch_boot_entry:
    cmp  ax, ENTRY_REBOOT
    je   .lbe_reboot
    cmp  ax, ENTRY_NOFB
    je   .lbe_nofb
    cmp  ax, ENTRY_SAFE
    je   .lbe_safe
    ; default — text mode, no VBE
    mov  si, str_cmdline_normal
    call set_cmdline
    mov  dword [es:boot_info + foreboots_boot_info.boot_entry], ENTRY_DEFAULT
    ret
.lbe_safe:
    mov  si, str_cmdline_safe
    call set_cmdline
    mov  dword [es:boot_info + foreboots_boot_info.safe_mode], 1
    or   dword [es:boot_info + foreboots_boot_info.flags], FOREB_BIF_SAFE
    mov  dword [es:boot_info + foreboots_boot_info.boot_entry], ENTRY_SAFE
    ret
.lbe_nofb:
    mov  byte [vesa_ok], 0
    mov  ax, 0x0003
    int  0x10
    and  dword [es:boot_info + foreboots_boot_info.flags], 0xFFFFFFFD
    or   dword [es:boot_info + foreboots_boot_info.flags], FOREB_BIF_NO_FB
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_addr + 4], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_width], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_height], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_bpp], 0
    mov  dword [es:boot_info + foreboots_boot_info.framebuffer_type], 2
    mov  dword [es:boot_info + foreboots_boot_info.no_framebuffer], 1
    mov  dword [es:boot_info + foreboots_boot_info.boot_entry], ENTRY_NOFB
    mov  si, str_cmdline_nofb
    call set_cmdline
    ret
.lbe_reboot:
    mov  si, str_rebooting
    call bios_print
    int  0x19
    jmp  $

; set_cmdline: SI = cmdline string offset (DS=0x0800). Store physical addr
; (0x8000 + offset) in boot_info.cmdline.
set_cmdline:
    push eax
    push ds
    xor  ax, ax
    mov  ds, ax
    movzx eax, si
    add  eax, 0x8000
    mov  [boot_info + foreboots_boot_info.cmdline], eax
    or   dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_CMDLINE
    pop  ds
    pop  eax
    ret

; ============================================================================
; build_multiboot_info: translate foreboots_boot_info + E820 map into a
; standard multiboot_info_t at MULTIBOOT_INFO_ADDR (kernel handoff, GRUB-compat)
; ============================================================================
build_multiboot_info:
    pushad
    push es
    push ds
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    ; zero multiboot_info_t
    mov  edi, mb_info_addr
    mov  ecx, mb_info_size
    xor  eax, eax
    rep  stosb
    ; flags
    mov  eax, MB_FLAG_MEM | MB_FLAG_CMDLINE | MB_FLAG_MMAP | MB_FLAG_BOOTLOADER
    test dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_FRAMEBUFFER
    jz   .bmi_nofb
    or   eax, MB_FLAG_FRAMEBUFFER
.bmi_nofb:
    test dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_INITRD
    jz   .bmi_noinitrd
    or   eax, MB_FLAG_MODS
.bmi_noinitrd:
    mov  [mb_info_addr + mb_info.flags], eax
    ; mem
    mov  eax, [boot_info + foreboots_boot_info.mem_lower]
    mov  [mb_info_addr + mb_info.mem_lower], eax
    mov  eax, [boot_info + foreboots_boot_info.mem_upper]
    mov  [mb_info_addr + mb_info.mem_upper], eax
    mov  dword [mb_info_addr + mb_info.boot_device], 0
    ; cmdline + bootloader name
    mov  eax, [boot_info + foreboots_boot_info.cmdline]
    mov  [mb_info_addr + mb_info.cmdline], eax
    mov  eax, [boot_info + foreboots_boot_info.boot_loader_name]
    mov  [mb_info_addr + mb_info.boot_loader_name], eax
    ; build multiboot1 mmap array
    mov  esi, foreb_mmap
    mov  edi, mb_mmap
    mov  ecx, [boot_info + foreboots_boot_info.mmap_count]
    xor  edx, edx
.bmi_mmap_loop:
    test ecx, ecx
    jz   .bmi_mmap_done
    mov  eax, [esi + foreboots_mmap_entry.base]
    mov  [edi + mb_mmap_entry.addr_low], eax
    mov  eax, [esi + foreboots_mmap_entry.base + 4]
    mov  [edi + mb_mmap_entry.addr_high], eax
    mov  eax, [esi + foreboots_mmap_entry.length]
    mov  [edi + mb_mmap_entry.len_low], eax
    mov  eax, [esi + foreboots_mmap_entry.length + 4]
    mov  [edi + mb_mmap_entry.len_high], eax
    mov  eax, [esi + foreboots_mmap_entry.type]
    mov  [edi + mb_mmap_entry.type], eax
    mov  dword [edi + mb_mmap_entry.size], 20
    add  esi, foreboots_mmap_entry_size
    add  edi, mb_mmap_entry_size
    inc  edx
    dec  ecx
    jmp  .bmi_mmap_loop
.bmi_mmap_done:
    mov  eax, edx
    shl  eax, 4
    lea  eax, [eax + edx*8]            ; *24
    mov  [mb_info_addr + mb_info.mmap_length], eax
    mov  dword [mb_info_addr + mb_info.mmap_addr], mb_mmap
    ; framebuffer
    test dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_FRAMEBUFFER
    jz   .bmi_fb_done
    mov  eax, [boot_info + foreboots_boot_info.framebuffer_addr]
    mov  [mb_info_addr + mb_info.framebuffer_addr], eax
    mov  eax, [boot_info + foreboots_boot_info.framebuffer_addr + 4]
    mov  [mb_info_addr + mb_info.framebuffer_addr + 4], eax
    mov  eax, [boot_info + foreboots_boot_info.framebuffer_pitch]
    mov  [mb_info_addr + mb_info.framebuffer_pitch], eax
    mov  eax, [boot_info + foreboots_boot_info.framebuffer_width]
    mov  [mb_info_addr + mb_info.framebuffer_width], eax
    mov  eax, [boot_info + foreboots_boot_info.framebuffer_height]
    mov  [mb_info_addr + mb_info.framebuffer_height], eax
    mov  al, [boot_info + foreboots_boot_info.framebuffer_bpp]
    mov  [mb_info_addr + mb_info.framebuffer_bpp], al
    mov  al, [boot_info + foreboots_boot_info.framebuffer_type]
    mov  [mb_info_addr + mb_info.framebuffer_type], al
.bmi_fb_done:
    ; initrd module
    test dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_INITRD
    jz   .bmi_initrd_done
    mov  edi, mb_module_slot
    mov  eax, [boot_info + foreboots_boot_info.initrd_addr]
    mov  [edi + mb_module.mod_start], eax
    add  eax, [boot_info + foreboots_boot_info.initrd_size]
    mov  [edi + mb_module.mod_end], eax
    mov  dword [edi + mb_module.string], 0
    mov  dword [edi + mb_module.reserved], 0
    mov  dword [mb_info_addr + mb_info.mods_count], 1
    mov  dword [mb_info_addr + mb_info.mods_addr], mb_module_slot
.bmi_initrd_done:
    pop  ds
    pop  es
    popad
    ret

mb_module_slot equ (MB_MMAP_ADDRESS + FOREB_MMAP_MAX * mb_mmap_entry_size)

; ============================================================================
; enter_pm_and_jump_stage3: load PM GDT, enable PE, far-jump to 32-bit, then
; jump to stage3 at STAGE3_LOAD_PHYS with ESI=boot_info, EDI=multiboot_info.
; Does NOT change the video mode (the chosen VBE mode / text mode is kept so
; the kernel receives the framebuffer state described in multiboot_info).
; ============================================================================
enter_pm_and_jump_stage3:
    cli
    lgdt [gdt_pm_desc]
    in   al, 0x92
    or   al, 0x02
    out  0x92, al
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    ; Far-jump to 32-bit PM. stage2 is loaded at physical 0x8000 but assembled
    ; with ORG 0, so the flat-PM (GDT base=0) entry EIP must be the PHYSICAL
    ; address (.pm32 + 0x8000), not the ORG-relative offset.
    jmp  0x0008:(.pm32 + 0x8000)

[BITS 32]
.pm32:
    mov  ax, 0x0010
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x00090000
    mov  esi, BOOT_INFO_ADDRESS       ; ESI = foreboots_boot_info*
    mov  edi, MULTIBOOT_INFO_ADDR     ; EDI = multiboot_info_t*
    ; ABSOLUTE jump: stage2 is assembled ORG 0 but runs at phys 0x8000, so a
    ; near `jmp STAGE3_LOAD_PHYS` (rel32) would land 0x8000 past stage3. Load
    ; the absolute linear address into a register and jump indirect instead.
    mov  eax, STAGE3_LOAD_PHYS
    jmp  eax                          ; -> stage3 entry (absolute)

[BITS 16]

; Protected-mode GDT (32-bit flat code/data)
align 8
gdt_pm_start:
    dq  0                             ; null (sel 0x00)
    ; 32-bit flat code (sel 0x08): base 0, limit 4 GiB, G=1 D=1, access 0x9A.
    ; NOTE: the last four fields MUST be bytes (db), not words — using dw here
    ; produced 12-byte descriptors and misaligned selector 0x10.
    dw  0xFFFF, 0x0000
    db  0x00, 0x9A, 0xCF, 0x00
    ; 32-bit flat data (sel 0x10): base 0, limit 4 GiB, G=1 D=1, access 0x92.
    dw  0xFFFF, 0x0000
    db  0x00, 0x92, 0xCF, 0x00
    ; 16-bit code (sel 0x18): base 0x8000, limit 64 KiB. Lets the ORG-0 draw
    ; code run unchanged from PM; EIP = ORG-relative offset.
    dw  0xFFFF, 0x8000
    db  0x00, 0x9A, 0x00, 0x00
    ; 16-bit data (sel 0x20): base 0x8000, limit 64 KiB. DS/ES so variable and
    ; string accesses resolve to the same linear addresses as real mode.
    dw  0xFFFF, 0x8000
    db  0x00, 0x92, 0x00, 0x00
    ; 16-bit data (sel 0x28): base 0x70000, limit 64 KiB. SS matches real-mode
    ; SS=0x7000 so SP is valid across the transition (also used for real return).
    dw  0xFFFF, 0x0000
    db  0x07, 0x92, 0x00, 0x00
gdt_pm_end:
gdt_pm_desc:
    dw  gdt_pm_end - gdt_pm_start - 1
    dd  gdt_pm_start + 0x8000

; ============================================================================
; Data
; ============================================================================
drive_number:       db 0x80
vesa_ok:            db 0
unreal_ok:          db 0

; Streaming kernel-loader scratch (see load_kernel / stream_segment).
align 4
k_phdr:     dd 0        ; linear addr of current program header (in HDR buf)
k_count:    dd 0        ; remaining program headers
k_entsize:  dd 0        ; e_phentsize
ls_lba:     dd 0        ; current absolute disk LBA for the segment
ls_skip:    dd 0        ; intra-sector byte offset (first chunk only)
ls_rem:     dd 0        ; file bytes remaining to copy for this segment
ls_filesz:  dd 0        ; segment p_filesz (for BSS size calc)
ls_memsz:   dd 0        ; segment p_memsz
ls_dest:    dd 0        ; current physical destination pointer
ls_chunk:   dd 0        ; sectors read in the current chunk
selected_entry:     dw FOREB_DEFAULT_ENTRY
timer_secs:         db FOREB_DEFAULT_TIMEOUT
start_ticks:        dd 0
current_vesa_mode:  dw 0

lfb_phys_addr:      dd 0x000A0000
screen_width:       dw 800
screen_height:      dw 600
screen_bpp:         db 8
screen_pitch:       dw 800

fb_x:               dw 0
fb_y:               dw 0
fb_w:               dw 0
fb_h:               dw 0
fb_c:               db 0
recov_sel:          dw 0

dc_x:               dw 0
dc_y:               dw 0
dc_char:            db 0
dc_color:           db 0

pm_fn:              dw 0             ; 16-bit offset of the PM draw routine
rp_idt:             dw 0, 0, 0       ; saved real-mode IDTR (sidt/lidt)

; find_vbe_mode working storage (DS=0x0800)
fvm_w:              dw 0
fvm_h:              dw 0
fvm_b:              db 0
fvm_mpptr_off:      dw 0
fvm_mpptr_seg:      dw 0

%if 1  ; ===== VBE preference tables — disabled =====
; VBE preference tables (width, height, bpp words; terminated by 0 width)
fb_prefs_menu:                      ; 8bpp for the GUI renderer
    dw 800, 600, 8
    dw 640, 480, 8
    dw 0

fb_prefs_kernel:                    ; 32bpp chain for the kernel framebuffer
    dw FOREB_DEFAULT_WIDTH, FOREB_DEFAULT_HEIGHT, FOREB_DEFAULT_BPP
    dw 1920, 1080, 32
    dw 1280, 720, 32
    dw 1024, 768, 32
    dw 800, 600, 32
    dw 640, 480, 32
    dw 0
%endif ; ===== end VBE prefs =====

; Strings
str_title:          db "ForeB - Forest Bootloader", 0
str_subtitle:       db "Forest OS Boot Manager", 0
str_menu_label:     db "[ Boot Menu ]", 0
str_key_hint:       db "[Up/Down] Navigate  [Enter] Boot  [Esc] Reset", 0
str_timer_pre:      db "Auto-boot in ", 0
str_timer_suf:      db " sec", 0
str_loading:        db "Loading Forest OS kernel...", 0

str_serial_stage2:  db "[ForeB] stage2: entry (BIOS/CSM)", 0x0D, 0x0A, 0
str_bl_name:        db "ForeB Forest Bootloader v2.0", 0
str_a20warn:        db "WARNING: A20 line could not be enabled!", 0x0D, 0x0A, 0
str_s3err:          db "ERROR: Could not load stage3!", 0x0D, 0x0A, 0
str_kernerr:        db "ERROR: Could not load kernel!", 0x0D, 0x0A, 0
str_rebooting:      db "Rebooting...", 0x0D, 0x0A, 0

str_cmdline_normal: db "", 0
str_cmdline_nofb:   db "nofb", 0
str_cmdline_safe:   db "safe", 0

str_textbanner:     db 0x0D, 0x0A
                    db "  ForeB Forest Bootloader v2.0", 0x0D, 0x0A
                    db "  ===========================", 0x0D, 0x0A, 0x0D, 0x0A
                    db "         .         .  .  .   ", 0x0D, 0x0A
                    db "          @         .    .    ", 0x0D, 0x0A
                    db "         @@@       .   .     ", 0x0D, 0x0A
                    db "        @@@@@    .    .  .   ", 0x0D, 0x0A
                    db "       @@@@@@@              ", 0x0D, 0x0A
                    db "        @@@@@      .  .  .  ", 0x0D, 0x0A
                    db "         @@@                ", 0x0D, 0x0A
                    db "          |     .   .   .   ", 0x0D, 0x0A
                    db "          |                  ", 0x0D, 0x0A
                    db 0x0D, 0x0A, 0
str_textfooter:     db 0x0D, 0x0A
                    db "  [Up/Down] Select  [Enter] Boot  [R] Recovery", 0x0D, 0x0A, 0
str_recoverbanner:  db 0x0D, 0x0A
                    db "  ForeB Recovery Menu", 0x0D, 0x0A
                    db "  ===================", 0x0D, 0x0A, 0x0D, 0x0A, 0
str_recoverfooter:  db 0x0D, 0x0A
                    db "  [Up/Down] Select  [Enter] Confirm  [Esc] Back", 0x0D, 0x0A, 0
str_recov_0:        db "FORBSHELL", 0
str_recov_1:        db "Reboot system", 0
str_recov_2:        db "About ForeB", 0
str_recov_3:        db "Back to menu", 0
str_about:          db "ForeB v2.0 - Forest Bootloader", 0x0D, 0x0A
                    db "Multiboot1-compatible BIOS loader", 0x0D, 0x0A, 0x0D, 0x0A
                    db "Stage3 at 0x5000, Stage2 at 0x8000", 0x0D, 0x0A, 0
str_nokernel:       db 0x07, 0x0D, 0x0A
                    db "  WARNING: No kernel loaded!", 0x0D, 0x0A
                    db "  Boot will fail. Press R for recovery.", 0x0D, 0x0A, 0x0D, 0x0A, 0
str_arrow:          db "  > ", 0
str_noarrow:        db "    ", 0
str_crlf:           db 0x0D, 0x0A, 0

; Boot entry labels (4 entries)
entry_label_0:      db "Forest OS (default)", 0
entry_label_1:      db "Forest OS (no framebuffer)", 0
entry_label_2:      db "Forest OS (safe mode)", 0
entry_label_3:      db "Reboot", 0

entry_desc_0:       db "Standard boot", 0
entry_desc_1:       db "VGA text mode, no VBE LFB", 0
entry_desc_2:       db "Minimal safe-mode boot", 0
entry_desc_3:       db "Restart the system", 0

entry_labels:
    dw entry_label_0
    dw entry_label_1
    dw entry_label_2
    dw entry_label_3

recov_labels:
    dw str_recov_0
    dw str_recov_1
    dw str_recov_2
    dw str_recov_3

entry_descs:
    dw entry_desc_0
    dw entry_desc_1
    dw entry_desc_2
    dw entry_desc_3

%if 1  ; ===== VBE graphical data — disabled =====
tree_line_top:      db "  /\\    ", 0
tree_line_f3:       db "  /##\\  ", 0
tree_line_f2:       db " /####\\ ", 0
tree_line_f1:       db "/######\\", 0
tree_line_trunk:    db "  ||||  ", 0
tree_line_ground:   db "^^^^^^^^", 0

; ============================================================================
; 8x8 Bitmap Font (ASCII 32..127, 96 glyphs x 8 bytes = 768 bytes)
; ============================================================================
align 4
font_data:
    db 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    db 0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00
    db 0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00
    db 0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00
    db 0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00
    db 0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00
    db 0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00
    db 0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00
    db 0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00
    db 0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00
    db 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00
    db 0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00
    db 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06
    db 0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00
    db 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00
    db 0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00
    db 0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00
    db 0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00
    db 0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00
    db 0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00
    db 0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00
    db 0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00
    db 0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00
    db 0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00
    db 0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00
    db 0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00
    db 0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00
    db 0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06
    db 0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00
    db 0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00
    db 0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00
    db 0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00
    db 0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00
    db 0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00
    db 0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00
    db 0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00
    db 0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00
    db 0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00
    db 0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00
    db 0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00
    db 0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00
    db 0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00
    db 0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00
    db 0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00
    db 0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00
    db 0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00
    db 0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00
    db 0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00
    db 0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00
    db 0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00
    db 0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00
    db 0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00
    db 0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00
    db 0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00
    db 0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00
    db 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00
    db 0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00
    db 0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00
    db 0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00
    db 0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00
    db 0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00
    db 0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00
    db 0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00
    db 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF
    db 0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00
    db 0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00
    db 0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00
    db 0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00
    db 0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00
    db 0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00
    db 0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00
    db 0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F
    db 0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00
    db 0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00
    db 0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E
    db 0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00
    db 0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00
    db 0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00
    db 0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00
    db 0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00
    db 0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F
    db 0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78
    db 0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00
    db 0x00,0x00,0x1E,0x03,0x1E,0x30,0x1F,0x00
    db 0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00
    db 0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00
    db 0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00
    db 0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00
    db 0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00
    db 0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F
    db 0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00
    db 0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00
    db 0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00
    db 0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00
    db 0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00
    db 0xFF,0x81,0xBD,0xA5,0xBD,0x81,0xFF,0x00
%endif ; ===== end disabled VBE data =====

; ============================================================================
; Stage 2 end marker and size assertion
; ============================================================================
stage2_end:

%if (stage2_end - stage2_start) > (STAGE2_SECTOR_COUNT * 512)
    %error "stage2.asm exceeds maximum size!"
%endif

times (STAGE2_SECTOR_COUNT * 512) - (stage2_end - stage2_start) db 0

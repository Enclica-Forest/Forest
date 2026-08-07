; =============================================================================
; ForeB - Forest Bootloader  Stage 1 (MBR, exactly 512 bytes)
; =============================================================================
; On CD (El Torito no-emulation), BIOS loads the full boot blob at 0x7C00
; via boot-load-size=48.  Stage1 relocates to 0x0600, then copies stage2
; (from 0x7E00) and stage3 (from 0x9E00) to their final addresses, verifies
; the stage2 magic, and jumps to 0x0800:0x0000.
; On HDD, stage1 loads stage2 via INT 13h LBA (offset 0, no CD detection).
; =============================================================================

[BITS 16]
[ORG 0x7C00]
%include "config.h"

; 64-byte header — xorriso -boot-info-table patches bytes 8-63 on CD.
        jmp  short _start
        times 62 db 0

_start:
    cli
    jmp  0x0000:_normalize_cs

_normalize_cs:
    xor  eax, eax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, STACK_TOP
    sti
    mov  [boot_drive], dl

    ; Relocate 0x7C00 -> 0x0600 (512 bytes)
    mov  si, 0x7C00
    mov  di, 0x0600
    mov  cx, 256
    rep  movsw
    jmp  0x0000:(_relocated - 0x7C00 + 0x0600)

_relocated:
    xor  eax, eax
    mov  ds, ax
    mov  es, ax
    mov  [LBA_BASE_OFFSET], eax

    ; Print boot message
    mov  si, 0x0600 + (msg_loading - 0x7C00)
    call print_str

    ; --- HDD path first: pull stage2 + stage3 off the boot disk via INT 13h
    ;     LBA extensions. On a real hard disk / USB / raw disk image the BIOS
    ;     only loaded sector 0 (this MBR), so we must read the rest ourselves.
    ;     If the read fails or the stage2 magic is absent (e.g. El Torito CD
    ;     no-emulation, where the BIOS pre-loaded the whole 48-sector blob at
    ;     0x7C00 and LBA math differs), fall back to the blob-copy path. ---
    mov  dl, [0x0600 + (boot_drive - 0x7C00)]
    mov  di, 0x0500                  ; DAP scratch (DS=0)
    mov  byte  [di + 0], 0x10        ; DAP size
    mov  byte  [di + 1], 0
    mov  word  [di + 2], STAGE2_SECTOR_COUNT
    mov  word  [di + 4], 0x0000      ; dest offset
    mov  word  [di + 6], STAGE2_LOAD_SEG   ; 0x0800 -> phys 0x8000
    mov  dword [di + 8], STAGE2_START_SECTOR
    mov  dword [di + 12], 0
    mov  si, 0x0500
    mov  ah, 0x42
    int  0x13
    jc   .try_blob
    mov  ax, [0x8002]
    cmp  ax, 0xFEB1
    jne  .try_blob
    ; stage2 landed — read stage3 (LBA 17..32) to 0x5000.
    mov  dl, [0x0600 + (boot_drive - 0x7C00)]
    mov  di, 0x0500
    mov  byte  [di + 0], 0x10
    mov  byte  [di + 1], 0
    mov  word  [di + 2], STAGE3_SECTOR_COUNT
    mov  word  [di + 4], 0x0000
    mov  word  [di + 6], STAGE3_LOAD_SEG   ; 0x0500 -> phys 0x5000
    mov  dword [di + 8], STAGE3_START_SECTOR
    mov  dword [di + 12], 0
    mov  si, 0x0500
    mov  ah, 0x42
    int  0x13
    jc   .bad_s2
    jmp  .launch

.try_blob:
    ; CD blob path: stage1@0x7C00, stage2@0x7E00, stage3@0x9E00.
    ; Copy stage3 first (no overlap), then stage2 (backward, overlap).
    mov  si, 0x9E00
    mov  di, 0x5000
    mov  cx, 8192
    rep  movsb
    std
    mov  si, 0x7E00 + 8191
    mov  di, 0x8000 + 8191
    mov  cx, 8192
    rep  movsb
    cld
    mov  ax, [0x8002]
    cmp  ax, 0xFEB1
    jne  .bad_s2

.launch:
    ; Jump to stage2 — DL = boot drive
    mov  dl, [0x0600 + (boot_drive - 0x7C00)]
    xor  dh, dh
    jmp  STAGE2_LOAD_SEG:0x0000

.bad_s2:
    mov  si, 0x0600 + (msg_bad_s2 - 0x7C00)
    jmp  error_flash

; ============================================================================
; error_flash: SI = message (physical addr, DS=0). Never returns.
; Flashing red/white screen + beep + border + message.
; ============================================================================
error_flash:
    mov  ax, 0x0003
    int  0x10
    push 0xB800
    pop  es
    mov  bl, 0x4F
.ef_loop:
    mov  bh, bl
    mov  ax, 0x0600
    xor  cx, cx
    mov  dx, 0x184F
    int  0x10
    push bx
    xor  di, di
    mov  cx, 80
    mov  al, 0xDB
    mov  ah, bl
    rep  stosw
    mov  di, 3840
    mov  cx, 80
    rep  stosw
    xor  bh, bh
    mov  dx, 0x0B0A
    mov  ah, 0x02
    int  0x10
    call print_str
    mov  al, 0x07
    mov  ah, 0x0E
    int  0x10
    pop  bx
    mov  cx, 0x0007
    mov  dx, 0xA120
    mov  ah, 0x86
    int  0x15
    xor  bl, 0x0F
    jmp  .ef_loop

; ============================================================================
; print_str: null-terminated string at DS:SI via INT 10h teletype
; ============================================================================
print_str:
    push ax
    push bx
.ps_loop:
    lodsb
    test al, al
    jz   .ps_done
    mov  ah, 0x0E
    xor  bx, bx
    mov  bl, 0x07
    int  0x10
    jmp  .ps_loop
.ps_done:
    pop  bx
    pop  ax
    ret

; =============================================================================
; Data
; =============================================================================
boot_drive:   db 0x80
msg_loading:  db "ForeB v2.0 loading...", 0x0D, 0x0A, 0
msg_bad_s2:   db "Stage2 missing!", 0

; =============================================================================
; Padding + partition table + boot signature
; =============================================================================
times 440 - ($ - $$) db 0

disk_signature: dd 0x464F5245
               dw 0x0000

partition_1:
    db 0x80, 0xFE, 0xFF, 0xFF, 0x83, 0xFE, 0xFF, 0xFF
    dd 63
    dd 0x00100000
partition_2: times 16 db 0
partition_3: times 16 db 0
partition_4: times 16 db 0

dw 0xAA55

%if ($ - $$) != 512
    %error "stage1.asm: MBR is not exactly 512 bytes!"
%endif

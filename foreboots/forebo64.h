; =============================================================================
; ForeB - Forest Bootloader
; forebo64.h (NASM include) - Extended macros & structures
; =============================================================================
; Include after config.h. Adds macros for:
;   - E820 memory map collection
;   - LBA disk read with CHS fallback
;   - CPUID / long-mode capability detection
;   - 64-bit GDT and long-mode trampoline helpers
;   - ELF header structures for stage3
;
; These extend the original macro library in forebo.h.

%ifndef FOREBO64_H
%define FOREBO64_H

; =============================================================================
; MACRO: serial_print  string_label
; Initialize COM1 (FOREB_COM1_BASE) to 8N1/115200 and print a NUL-terminated
; string via LSR (THRE) polling. Self-contained: re-runs the idempotent port
; init on each call so it works whether or not the port was set up earlier.
; Assembles correctly in both 16-bit real mode (stage2) and 32-bit protected
; mode (stage3) - the string pointer register width follows __BITS__, and the
; in/out port I/O is identical in either mode. The string must be reachable
; through DS (stage2: DS=stage2 segment; stage3: flat DS base 0).
; Preserves ALL general-purpose registers and flags.
; =============================================================================
%macro serial_print 1
    pushf
    pusha
    ; --- COM1 8N1, divisor 1 (115200 baud) ---
    mov  dx, FOREB_COM1_BASE + 1        ; IER: disable interrupts
    xor  al, al
    out  dx, al
    mov  dx, FOREB_COM1_BASE + 3        ; LCR: enable DLAB
    mov  al, 0x80
    out  dx, al
    mov  dx, FOREB_COM1_BASE + 0        ; DLL = 1
    mov  al, 0x01
    out  dx, al
    mov  dx, FOREB_COM1_BASE + 1        ; DLM = 0
    xor  al, al
    out  dx, al
    mov  dx, FOREB_COM1_BASE + 3        ; LCR: 8N1, DLAB off
    mov  al, 0x03
    out  dx, al
    mov  dx, FOREB_COM1_BASE + 2        ; FCR: enable+clear FIFO, 14-byte
    mov  al, 0xC7
    out  dx, al
    mov  dx, FOREB_COM1_BASE + 4        ; MCR: DTR|RTS|OUT2
    mov  al, 0x0B
    out  dx, al
    ; --- print loop ---
%if __BITS__ == 16
    mov  si, %1
%else
    mov  esi, %1
%endif
%%sp_next:
%if __BITS__ == 16
    mov  al, [si]
    inc  si
%else
    mov  al, [esi]
    inc  esi
%endif
    test al, al
    jz   %%sp_done
    mov  bl, al                         ; hold char while polling LSR
%%sp_wait:
    mov  dx, FOREB_COM1_BASE + 5        ; LSR
    in   al, dx
    test al, 0x20                       ; THRE (transmit holding empty)
    jz   %%sp_wait
    mov  al, bl
    mov  dx, FOREB_COM1_BASE + 0        ; THR
    out  dx, al
    jmp  %%sp_next
%%sp_done:
    popa
    popf
%endmacro

; =============================================================================
; MACRO: e820_collect
; Collect INT 15h E820 memory map into foreboots_mmap_entry array at
; FOREB_MMAP_ADDRESS. Stores up to FOREB_MMAP_MAX entries.
; On return:
;   [boot_info + foreboots_boot_info.mmap_count] = number of entries
;   [boot_info + foreboots_boot_info.mmap_addr]   = FOREB_MMAP_ADDRESS
; Clobbers: EAX, EBX, ECX, EDX, ESI, EDI, ES, flags
; Assumes DS=0 for the destination buffer (physical addresses).
; =============================================================================
%macro e820_collect 0
    push    ds
    push    es
    pushad                          ; preserve all regs; EBP = entry counter
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     edi, FOREB_MMAP_ADDRESS ; ES:DI -> current foreboots_mmap_entry slot
    xor     ebx, ebx                ; continuation value (0 = start)
    xor     ebp, ebp                ; EBP = entry count (saved across int 0x15)
%%e820_loop:
    cmp     ebp, FOREB_MMAP_MAX
    jae     %%e820_done             ; table full
    mov     eax, 0x0000E820
    mov     ecx, 24
    mov     dword [es:edi + 20], 1  ; default ACPI attr = valid, in the DEST slot
    mov     edx, 0x534D4150         ; 'SMAP'
    ; INT 15h/E820 writes the entry straight to ES:DI (= this array slot). The
    ; previous version pointed the BIOS at the array but then overwrote the slot
    ; with an uninitialised scratch buffer, yielding a 0-byte memory map.
    int     0x15
    jc      %%e820_done             ; carry = end / error
    cmp     eax, 0x534D4150         ; BIOS must return 'SMAP'
    jne     %%e820_done
    cmp     ecx, 20
    jb      %%e820_next             ; entry too small; don't count it
    inc     ebp
    add     edi, foreboots_mmap_entry_size   ; advance to the next slot
%%e820_next:
    test    ebx, ebx
    jz      %%e820_done
    jmp     %%e820_loop

%%e820_done:
    mov     [boot_info + foreboots_boot_info.mmap_count], ebp
    mov     dword [boot_info + foreboots_boot_info.mmap_addr], FOREB_MMAP_ADDRESS
    popad
    pop     es
    pop     ds
%endmacro

; =============================================================================
; MACRO: lba_read_one  drive_src
; Read ECX sectors (clamped to 63) from LBA EAX to physical address ESI using
; INT 13h AH=42h. Falls back to CHS (AH=02h, 1 sector at a time) if LBA
; extensions are unsupported by the BIOS.
;   EAX       = starting LBA
;   ECX       = sector count (clamped to 63 internally)
;   ESI       = physical destination address (seg:off derived as phys/16 : &0xF)
;   [drive_src] = memory location (DS=0 accessible) holding the BIOS drive byte
; On success CF=0; on failure CF=1. Preserves ALL caller registers.
; For reads larger than 63 sectors, call this in a loop (see stage2 disk_load).
; =============================================================================
%macro lba_read_one 1
    push    eax
    push    ecx
    push    edx
    push    esi
    push    edi
    push    ebx
    push    ebp
    push    es
    push    ds
    xor     ax, ax
    mov     ds, ax                      ; DS=0 for DAP / drive_src
    ; Save LBA + count into the DAP first (before any INT 13h clobbers EAX)
    mov     [DAP_BUF + 8], eax          ; LBA low
    mov     dword [DAP_BUF + 12], 0     ; LBA high
    mov     ebp, ecx                    ; EBP = sector count (preserve)
    cmp     ebp, 63
    jbe     %%lro_cnt_ok
    mov     ebp, 63
%%lro_cnt_ok:
    ; Destination seg:off from ESI
    mov     edx, esi
    shr     edx, 4                      ; seg
    mov     edi, esi
    and     edi, 0x000F                 ; off
    mov     word [DAP_BUF + 0], 0x0010
    mov     [DAP_BUF + 4], di
    mov     [DAP_BUF + 6], dx
    ; Check LBA extensions support
    mov     ah, 0x41
    mov     bx, 0x55AA
    mov     dl, [%1]
    int     0x13
    jc      %%lro_chs
    cmp     bx, 0xAA55
    jne     %%lro_chs
    test    cl, 0x01
    jz      %%lro_chs
    ; --- LBA path: one INT 13h AH=42h call for all EBP sectors ---
    mov     [DAP_BUF + 2], bp           ; count (word)
    mov     si, DAP_BUF
    mov     dl, [%1]
    mov     ah, 0x42
    int     0x13
    jc      %%lro_fail
    clc
    jmp     %%lro_ret
    ; --- CHS fallback: read EBP sectors, one per iteration ---
%%lro_chs:
    mov     [DAP_BUF + 2], bp           ; not used by CHS, keep for record
%%lro_chs_loop:
    test    ebp, ebp
    jz      %%lro_ok
    mov     eax, [DAP_BUF + 8]          ; current LBA
    xor     edx, edx
    mov     ecx, 63
    div     ecx                         ; eax = LBA/63, edx = sector-1
    inc     edx                         ; edx = sector
    push    edx                         ; save sector
    xor     edx, edx
    mov     ecx, 16
    div     ecx                         ; eax = cylinder, edx = head
    push    edx                         ; save head
    mov     ch, al                      ; CH = cyl low
    mov     edx, eax
    shr     edx, 8
    and     edx, 0xC0                   ; high cyl bits
    pop     eax                         ; head
    mov     dh, al                      ; DH = head
    pop     eax                         ; sector
    mov     cl, al                      ; CL = sector (low 6 bits)
    or      cl, dl                      ; CL |= high cyl bits
    ; Buffer ES:BX from ESI
    mov     eax, esi
    shr     eax, 4                      ; seg
    mov     edx, esi
    and     edx, 0x0F                   ; off
    push    es
    mov     es, ax
    mov     bx, dx                      ; BX = buffer offset
    mov     dl, [%1]                    ; DL = drive (DH preserved)
    mov     eax, 0x0201                 ; AH=02, AL=1 sector
    int     0x13
    pop     es
    jc      %%lro_fail
    inc     dword [DAP_BUF + 8]         ; LBA++
    add     esi, 512                    ; dest += 512
    dec     ebp
    jmp     %%lro_chs_loop
%%lro_ok:
    clc
    jmp     %%lro_ret
%%lro_fail:
    stc
%%lro_ret:
    pop     ds
    pop     es
    pop     ebp
    pop     ebx
    pop     edi
    pop     esi
    pop     edx
    pop     ecx
    pop     eax
%endmacro

; =============================================================================
; MACRO: detect_long_mode
; Detect CPUID and long-mode availability. Sets the cpuid_available,
; long_mode_available, pae_available fields in boot_info and the corresponding
; FOREB_BIF_* bits in flags. Falls back gracefully on 386/486.
; Clobbers: EAX, EBX, ECX, EDX, flags
; =============================================================================
%macro detect_long_mode 0
    push    ds
    pushad
    xor     ax, ax
    mov     ds, ax                      ; DS=0 to access boot_info (physical 0x1000)
    ; CPUID availability: flip ID bit (EFLAGS bit 21)
    pushfd
    pop     eax
    mov     ecx, eax
    xor     eax, 1 << 21
    push    eax
    popfd
    pushfd
    pop     eax
    push    ecx
    popfd
    cmp     eax, ecx
    je      %%no_cpuid
    ; CPUID present
    mov     dword [boot_info + foreboots_boot_info.cpuid_available], 1
    or      dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_CPUID

    ; PAE: CPUID(1) -> EDX bit 6
    mov     eax, 1
    cpuid
    test    edx, 1 << 6
    jz      %%no_pae
    mov     dword [boot_info + foreboots_boot_info.pae_available], 1
    or      dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_PAE
%%no_pae:
    ; Extended CPUID for long mode: need 0x80000001
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      %%done
    mov     eax, 0x80000001
    cpuid
    test    edx, 1 << 29                 ; LM bit
    jz      %%done
    mov     dword [boot_info + foreboots_boot_info.long_mode_available], 1
    or      dword [boot_info + foreboots_boot_info.flags], FOREB_BIF_LONG_MODE
    jmp     %%done
%%no_cpuid:
    mov     dword [boot_info + foreboots_boot_info.cpuid_available], 0
    mov     dword [boot_info + foreboots_boot_info.long_mode_available], 0
    mov     dword [boot_info + foreboots_boot_info.pae_available], 0
%%done:
    popad
    pop     ds
%endmacro

; =============================================================================
; Structure: ELF32 header (stage3 uses this to load the kernel)
; =============================================================================
struc Elf32_Ehdr
    .e_ident            resb 16
    .e_type             resw 1
    .e_machine          resw 1
    .e_version          resd 1
    .e_entry            resd 1
    .e_phoff            resd 1
    .e_shoff            resd 1
    .e_flags            resd 1
    .e_ehsize           resw 1
    .e_phentsize        resw 1
    .e_phnum            resw 1
    .e_shentsize        resw 1
    .e_shnum            resw 1
    .e_shstrndx         resw 1
endstruc

struc Elf32_Phdr
    .p_type             resd 1
    .p_offset           resd 1
    .p_vaddr            resd 1
    .p_paddr            resd 1
    .p_filesz           resd 1
    .p_memsz            resd 1
    .p_flags            resd 1
    .p_align            resd 1
endstruc

; =============================================================================
; Structure: ELF64 header (stage3 uses this to load the 64-bit kernel)
; =============================================================================
struc Elf64_Ehdr
    .e_ident            resb 16
    .e_type             resw 1
    .e_machine          resw 1
    .e_version          resd 1
    .e_entry            resq 1
    .e_phoff            resq 1
    .e_shoff            resq 1
    .e_flags            resd 1
    .e_ehsize           resw 1
    .e_phentsize        resw 1
    .e_phnum            resw 1
    .e_shentsize        resw 1
    .e_shnum            resw 1
    .e_shstrndx         resw 1
endstruc

struc Elf64_Phdr
    .p_type             resd 1
    .p_flags            resd 1
    .p_offset           resq 1
    .p_vaddr            resq 1
    .p_paddr            resq 1
    .p_filesz           resq 1
    .p_memsz            resq 1
    .p_align            resq 1
endstruc

; =============================================================================
; 64-bit GDT for the optional long-mode trampoline in stage3.
; Code: 0x00AF9A000000FFFF (L+DB+P+R/W/exec, 64-bit)
; Data: 0x00CF92000000FFFF (G+DB+P+R/W, flat 4GB)
; =============================================================================
%macro GDT64_ENTRIES 0
gdt64_start:
    dq  0x0000000000000000              ; null
gdt64_code: equ $ - gdt64_start
    dq  0x00AF9A000000FFFF              ; 64-bit code
gdt64_data: equ $ - gdt64_start
    dq  0x00CF92000000FFFF              ; 64-bit data (compatibility)
gdt64_end:
%endmacro

%endif ; FOREBO64_H

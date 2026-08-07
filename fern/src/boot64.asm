; =============================================================================
; 64-bit Boot code for Forest OS
; File: src/boot64.asm
;
; Entered in 32-bit protected mode from GRUB / ForeB.  Verifies the CPU
; supports long mode, builds initial 4-level paging tables (identity-mapping
; the first 2 MiB and mapping the kernel image at the high canonical
; address 0xFFFFFFFF80100000), enables PAE + EFER.LME + EFER.NXE, enables
; CR4.SMEP/SMAP when the CPU advertises them, enables CR0.PG, loads the
; 64-bit GDT, and far-jumps to the 64-bit entry.
;
; Build-time options (defined via -D on the NASM command line):
;   ENABLE_NX                (1) enable EFER.NXE so PAGE_NX works
;   ENABLE_SMEP              (1) set CR4.SMEP if CPU supports it
;   ENABLE_SMAP              (1) set CR4.SMAP if CPU supports it
;   ENABLE_5LEVEL_PAGING     (1) probe CR4.LA57 (5-level paging, needs PML5)
;
; The boot trampoline and its data live in .text.boot / .bss.boot which
; link64.ld places at VMA == LMA == 0x00100000, so all labels here are
; accessible before paging is enabled.  The high-half C entry (startk)
; is called after paging is active and the high-half mapping is in place.
; =============================================================================

bits 32

; -----------------------------------------------------------------------------
; Multiboot headers (must appear in the first 32 KiB of the file)
; -----------------------------------------------------------------------------
section .multiboot
align 8

; --- Multiboot2 header (primary) ---
multiboot2_header_start:
    MULTIBOOT2_MAGIC    equ 0xE85250D6
    MULTIBOOT2_ARCH     equ 0x00000000      ; x86 protected mode
    MULTIBOOT2_LENGTH   equ (multiboot2_header_end - multiboot2_header_start)
    MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_LENGTH)

    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd MULTIBOOT2_LENGTH
    dd MULTIBOOT2_CHECKSUM

    ; Framebuffer request tag (type 5)
    align 8
    dw 5        ; type
    dw 0        ; flags
    dd 20       ; size
    dd 0        ; width
    dd 0        ; height
    dd 0        ; depth

    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
multiboot2_header_end:

; --- Multiboot1 header (fallback) ---
align 4
multiboot1_header:
    MULTIBOOT1_MAGIC    equ 0x1BADB002
    MULTIBOOT1_FLAGS    equ 0x00000007      ; mem info + align + video
    MULTIBOOT1_CHECKSUM equ -(MULTIBOOT1_MAGIC + MULTIBOOT1_FLAGS)

    dd MULTIBOOT1_MAGIC
    dd MULTIBOOT1_FLAGS
    dd MULTIBOOT1_CHECKSUM
    dd 0          ; mode_type
    dd 0          ; width
    dd 0          ; height
    dd 0          ; depth

; -----------------------------------------------------------------------------
; 32-bit boot trampoline
; -----------------------------------------------------------------------------
section .text.boot
global start
extern startk
extern _stack_top
extern _bss_start
extern _bss_end

; High-half canonical base; must match link64.ld and paging64.h.
KERNEL_HIGH_VMA     equ 0xFFFFFFFF80100000
KERNEL_PHYS_BASE    equ 0x00100000
; How much of the kernel image to map at the high-half (16 MiB / 8 * 2 MiB).
KERNEL_MAP_BYTES    equ 0x01000000

start:
    cli
    ; Save multiboot info for the kernel: eax=magic, ebx=info pointer
    mov edi, eax
    mov esi, ebx

    ; --- Quick VGA heartbeat so we can see the trampoline is running ---
    mov dword [0xb8000], 0x4f4f4f42        ; "BOO"
    mov dword [0xb8004], 0x4f545f54        ; "T_6"

    ; --- 1. Verify CPUID is available (EFLAGS ID bit) ---
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid

    ; --- 2. Verify long-mode is available (CPUID 0x80000001:EDX[29]) ---
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29                ; LM bit
    jz .no_long_mode
    ; Save EDX for NX check below
    push edx

    ; --- 3. Build initial 4-level paging tables (identity + high-half) ---
    ; PML4[0]      -> PDPT_low   (identity, first 2 MiB)
    ; PML4[511]    -> PDPT_high  (high-half, KERNEL_HIGH_VMA range)
    ; PDPT_low[0]  -> PD_low
    ; PD_low[0]    -> 2 MiB page at phys 0 (identity)
    ; PDPT_high[0] -> PD_high
    ; PD_high[i]   -> 2 MiB pages at phys KERNEL_PHYS_BASE + i*2MiB,
    ;                  mapped to KERNEL_HIGH_VMA + i*2MiB, for i in [0, 8)

    mov eax, pdpt_low
    or  eax, 0x03                    ; present + writable + supervisor
    mov [pml4 + 0], eax              ; PML4[0]
    mov eax, pdpt_high
    or  eax, 0x03
    mov [pml4 + 8*511], eax          ; PML4[511]

    mov eax, pd_low
    or  eax, 0x03
    mov [pdpt_low + 0], eax          ; PDPT_low[0]

    mov eax, pd_high
    or  eax, 0x03
    mov [pdpt_high + 0], eax         ; PDPT_high[0]

    ; PD_low[0] = 2 MiB identity page at phys 0 (present + writable + PS)
    mov dword [pd_low + 0], 0x00000083

    ; PD_high[i] = 2 MiB pages mapping KERNEL_PHYS_BASE -> KERNEL_HIGH_VMA
    ; Entry value = (KERNEL_PHYS_BASE + i*2MiB) | 0x83 (P|R/W|PS)
    mov ecx, 0
    mov eax, KERNEL_PHYS_BASE | 0x83
.fill_high_pd:
    mov [pd_high + ecx*8], eax
    mov dword [pd_high + ecx*8 + 4], 0
    add eax, 0x200000                ; next 2 MiB
    inc ecx
    cmp ecx, (KERNEL_MAP_BYTES / 0x200000)
    jl .fill_high_pd

    ; --- 4. Load CR3 with the PML4 physical (== virtual, pre-paging) ---
    mov eax, pml4
    mov cr3, eax

    ; --- 5. Enable CR4.PAE (required for long mode) ---
    mov eax, cr4
    or  eax, 1 << 5                  ; PAE
    ; SMEP / SMAP (gated by build options + CPU support)
%ifdef ENABLE_SMEP
    ; Check CPUID 0x07.0:EBX[7] (SMEP) — EDX from 0x80000001 already in EDX
    ; is wrong leaf; re-query leaf 7 here.
    push eax
    push ebx
    push ecx
    push edx
    mov eax, 7
    xor ecx, ecx
    cpuid
    test ebx, 1 << 7                 ; SMEP
    pop edx
    pop ecx
    pop ebx
    jz .no_smep
    pop eax
    or  eax, 1 << 20                 ; CR4.SMEP
    jmp .after_smep
.no_smep:
    pop eax
.after_smep:
%endif
%ifdef ENABLE_SMAP
    push eax
    push ebx
    push ecx
    push edx
    mov eax, 7
    xor ecx, ecx
    cpuid
    test ebx, 1 << 20                ; SMAP
    pop edx
    pop ecx
    pop ebx
    jz .no_smap
    pop eax
    or  eax, 1 << 21                 ; CR4.SMAP
    jmp .after_smap
.no_smap:
    pop eax
.after_smap:
%endif
    ; PCIDE (gated by ENABLE_PCID and CPU support)
%ifdef ENABLE_PCID
    push eax
    push ebx
    push ecx
    push edx
    mov eax, 1
    cpuid
    test ecx, 1 << 17               ; PCID
    pop edx
    pop ecx
    pop ebx
    jz .no_pcid
    pop eax
    or  eax, 1 << 17                ; CR4.PCIDE
    jmp .after_pcid
.no_pcid:
    pop eax
.after_pcid:
%endif
    mov cr4, eax

    ; --- 6. Set EFER.LME (long-mode enable) + EFER.NXE (if NX supported) ---
    mov ecx, 0xC0000080              ; IA32_EFER
    rdmsr
    or  eax, 1 << 8                  ; LME
    ; NX: CPUID 0x80000001:EDX[20], saved on the stack earlier
    pop edx                          ; EDX from CPUID 0x80000001
    push edx
    test edx, 1 << 20                ; NX
    jz .no_nx
%ifdef ENABLE_NX
    or  eax, 1 << 11                 ; NXE
%endif
.no_nx:
    wrmsr
    pop edx                          ; discard saved CPUID EDX

    ; --- 7. Enable paging (CR0.PG) -> activates long mode ---
    mov eax, cr0
    or  eax, 1 << 31                 ; PG
    mov cr0, eax

    ; --- 8. Load the 64-bit GDT and far-jump to the 64-bit stub ---
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

.no_cpuid:
    mov al, 'C'
    jmp .error
.no_long_mode:
    mov al, 'L'
    jmp .error

.error:
    ; Red-on-white error banner; the byte in AL is the reason code.
    mov dword [0xb8000], 0x4f424f45  ; "EBO"
    mov dword [0xb8004], 0x4f204f4f  ; "OT "
    mov dword [0xb8008], 0x4f414f45  ; "EA "
    mov dword [0xb800c], 0x4f524f52  ; "RR:"
    mov byte [0xb8010], al
    mov byte [0xb8011], 0x4f
    mov ecx, 80*25*2
    mov edi, 0xb8000
.fill_screen:
    mov byte [edi], ' '
    mov byte [edi+1], 0x1f
    add edi, 2
    loop .fill_screen
    cli
.hang:
    hlt
    jmp .hang

; =============================================================================
; 64-bit entry stub
; =============================================================================
bits 64
section .text.boot

long_mode_start:
    ; Reload segment registers with the 64-bit null/data selector.
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; High-half kernel stack.
    mov rsp, _stack_top

    ; Zero the kernel .bss (high-half VMA, mapped by our PD_high).
    mov rcx, _bss_end
    sub rcx, _bss_start
    jz .bss_done
    mov rdi, _bss_start
    xor rax, rax
    rep stosq
.bss_done:

    cld

    ; Pass multiboot magic (rdi) and info pointer (rsi) to startk.
    ; The 32-bit trampoline stashed them in edi/esi; clear the high bits.
    mov eax, edi
    mov rdi, rax
    mov eax, esi
    mov rsi, rax

    ; Hand off to the high-half C entry point.
    call startk

    ; Should never return.
    cli
.halt:
    hlt
    jmp .halt

; =============================================================================
; Boot-time page tables (low BSS, accessible before paging is on)
; =============================================================================
section .bss.boot
align 4096
pml4:       resb 4096
pdpt_low:   resb 4096
pdpt_high:  resb 4096
pd_low:     resb 4096
pd_high:    resb 4096

; =============================================================================
; 64-bit GDT (loaded just before the far jump to long mode)
; =============================================================================
section .rodata.boot
align 8
gdt64:
    dq 0                                ; null descriptor
.code: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)  ; P|S|code|exec|long
.data: equ $ - gdt64
    dq (1 << 44) | (1 << 47) | (1 << 53)              ; P|S|data|write
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

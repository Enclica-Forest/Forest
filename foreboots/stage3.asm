; =============================================================================
; ForeB - Forest Bootloader  Stage 3
; stage3.asm - 32-bit protected-mode ELF loader + multiboot1 handoff
; =============================================================================
; Build:
;   nasm -f bin -o stage3.bin stage3.asm
;
; Loaded by stage2 at physical address STAGE3_LOAD_PHYS (0x5000), 16 sectors.
; Entered in 32-bit protected mode with flat segments (DS=ES=SS=0x10, base 0,
; limit 4 GB) by stage2's .pm32 trampoline, which passes:
;   ESI = physical address of foreboots_boot_info
;   EDI = physical address of multiboot_info_t (kernel handoff struct)
;
; Stage 3 responsibilities:
;   1. Parse the kernel ELF (ELF32 or ELF64) loaded at boot_info.kernel_load_addr
;   2. Copy each PT_LOAD segment to its physical address (p_paddr), zero-fill
;      the BSS (p_memsz - p_filesz)
;   3. Record the ELF entry point and ELF class in boot_info
;   4. Hand off to the kernel EXACTLY as GRUB does:
;        EAX = 0x2BADB002 (MULTIBOOT1_MAGIC)
;        EBX = &multiboot_info_t
;        EIP = kernel ELF entry
;      The Forest OS kernel enters in 32-bit PM (even the 64-bit kernel does
;      its own long-mode transition in src/boot64.asm), so this is the default
;      and correct path for both ARCH=32 and ARCH=64.
;   5. Optional long-mode trampoline (FOREB_FORCE_LONG_MODE=1): if the CPU
;      supports long mode and the kernel is ELF64, set up paging/PAE/long-mode
;      and jump to the 64-bit entry. Disabled by default (current kernel does
;      its own transition). Falls back to 32-bit PM entry if unavailable.
; =============================================================================

[BITS 32]

%include "config.h"
%include "forebo64.h"

[ORG STAGE3_LOAD_PHYS]

; =============================================================================
; Entry
; =============================================================================
stage3_start:
    mov  ebp, esi                    ; EBP = foreboots_boot_info*
    mov  ebx, edi                    ; EBX = multiboot_info_t* (handoff in EBX)

    ; --- If stage2 already streamed the kernel to its PT_LOAD segments
    ;     (arbitrary-size loader), skip ELF parsing and jump straight to the
    ;     recorded entry point. ---
    test dword [ebp + foreboots_boot_info.flags], FOREB_BIF_KERNEL_PRELOADED
    jz   .parse_elf
    mov  eax, [ebp + foreboots_boot_info.kernel_entry]
    jmp  .elf_loaded
.parse_elf:

    ; --- Load kernel ELF base ---
    mov  esi, [ebp + foreboots_boot_info.kernel_load_addr]

    ; --- ELF magic check: 0x7F 'E' 'L' 'F' ---
    cmp  dword [esi + 0], 0x464C457F
    jne  .not_elf
    mov  al, [esi + 4]               ; EI_CLASS
    cmp  al, ELFCLASS32
    je   .elf32
    cmp  al, ELFCLASS64
    je   .elf64
    jmp  .not_elf

; -----------------------------------------------------------------------------
; ELF32 program-header load
; -----------------------------------------------------------------------------
.elf32:
    mov  [s3_elfbase], esi
    movzx ecx, word [esi + Elf32_Ehdr.e_phnum]
    mov  [s3_count], ecx
    movzx eax, word [esi + Elf32_Ehdr.e_phentsize]
    mov  [s3_entsize], eax
    mov  eax, [esi + Elf32_Ehdr.e_phoff]
    add  eax, esi
    mov  [s3_phdr], eax
.ph32_loop:
    mov  ecx, [s3_count]
    test ecx, ecx
    jz   .ph32_done
    mov  edi, [s3_phdr]
    cmp  dword [edi + Elf32_Phdr.p_type], PT_LOAD
    jne  .ph32_next
    ; source = elfbase + p_offset
    mov  esi, [s3_elfbase]
    add  esi, [edi + Elf32_Phdr.p_offset]
    ; dest = p_paddr, count = p_filesz
    mov  eax, [edi + Elf32_Phdr.p_paddr]
    mov  ecx, [edi + Elf32_Phdr.p_filesz]
    mov  edx, edi                    ; save phdr pointer
    mov  edi, eax                    ; dest
    rep  movsb                       ; copy file bytes
    ; zero-fill BSS (p_memsz - p_filesz)
    mov  ecx, [edx + Elf32_Phdr.p_memsz]
    sub  ecx, [edx + Elf32_Phdr.p_filesz]
    jbe  .ph32_nobss
    mov  edi, [edx + Elf32_Phdr.p_paddr]
    add  edi, [edx + Elf32_Phdr.p_filesz]
    xor  eax, eax
    rep  stosb
.ph32_nobss:
.ph32_next:
    mov  eax, [s3_phdr]
    add  eax, [s3_entsize]
    mov  [s3_phdr], eax
    dec  dword [s3_count]
    jmp  .ph32_loop
.ph32_done:
    mov  esi, [s3_elfbase]
    mov  eax, [esi + Elf32_Ehdr.e_entry]
    mov  dword [ebp + foreboots_boot_info.kernel_is64bit], 0
    jmp  .elf_loaded

; -----------------------------------------------------------------------------
; ELF64 program-header load (uses low 32 bits; Forest OS kernel is below 4 GB)
; -----------------------------------------------------------------------------
.elf64:
    mov  [s3_elfbase], esi
    movzx ecx, word [esi + Elf64_Ehdr.e_phnum]
    mov  [s3_count], ecx
    movzx eax, word [esi + Elf64_Ehdr.e_phentsize]
    mov  [s3_entsize], eax
    mov  eax, [esi + Elf64_Ehdr.e_phoff]
    add  eax, esi
    mov  [s3_phdr], eax
.ph64_loop:
    mov  ecx, [s3_count]
    test ecx, ecx
    jz   .ph64_done
    mov  edi, [s3_phdr]
    cmp  dword [edi + Elf64_Phdr.p_type], PT_LOAD
    jne  .ph64_next
    mov  esi, [s3_elfbase]
    add  esi, [edi + Elf64_Phdr.p_offset]
    mov  eax, [edi + Elf64_Phdr.p_paddr]
    mov  ecx, [edi + Elf64_Phdr.p_filesz]
    mov  edx, edi
    mov  edi, eax
    rep  movsb
    mov  ecx, [edx + Elf64_Phdr.p_memsz]
    sub  ecx, [edx + Elf64_Phdr.p_filesz]
    jbe  .ph64_nobss
    mov  edi, [edx + Elf64_Phdr.p_paddr]
    add  edi, [edx + Elf64_Phdr.p_filesz]
    xor  eax, eax
    rep  stosb
.ph64_nobss:
.ph64_next:
    mov  eax, [s3_phdr]
    add  eax, [s3_entsize]
    mov  [s3_phdr], eax
    dec  dword [s3_count]
    jmp  .ph64_loop
.ph64_done:
    mov  esi, [s3_elfbase]
    mov  eax, [esi + Elf64_Ehdr.e_entry]
    mov  dword [ebp + foreboots_boot_info.kernel_is64bit], 1
    jmp  .elf_loaded

; -----------------------------------------------------------------------------
; Not an ELF: treat as a flat raw binary. Copy it to 0x100000 and enter there.
; -----------------------------------------------------------------------------
.not_elf:
    mov  esi, [ebp + foreboots_boot_info.kernel_load_addr]
    mov  edi, 0x00100000
    mov  ecx, [ebp + foreboots_boot_info.kernel_size]
    mov  edx, ecx
    rep  movsb
    mov  eax, 0x00100000
    mov  dword [ebp + foreboots_boot_info.kernel_is64bit], 0

; -----------------------------------------------------------------------------
; ELF loaded; EAX = kernel entry point. Hand off to the kernel.
; -----------------------------------------------------------------------------
.elf_loaded:
    mov  [ebp + foreboots_boot_info.kernel_entry], eax

%if FOREB_SERIAL_DEBUG
    serial_print str_serial_stage3   ; proof-of-progress just before kernel jump
%endif

%if FOREB_FORCE_LONG_MODE
    ; Optional long-mode trampoline. Only if the CPU supports long mode AND
    ; the kernel is 64-bit ELF. Otherwise fall through to 32-bit PM entry.
    cmp  dword [ebp + foreboots_boot_info.long_mode_available], 0
    je   .handoff32
    cmp  dword [ebp + foreboots_boot_info.kernel_is64bit], 0
    je   .handoff32
    jmp  enter_long_mode
.handoff32:
%endif

    ; --- Default 32-bit protected-mode handoff (GRUB-compatible) ---
    ; Mask every PIC IRQ before handoff, exactly as GRUB does. The kernel's
    ; early entry does `sti` with only a page-fault handler installed, so a
    ; live BIOS timer IRQ (IRQ0) would fault into an empty IDT and triple-fault
    ; before it can program the PIC/IDT itself.
    mov  al, 0xFF
    out  0xA1, al                    ; mask all slave  PIC (IRQ 8..15)
    out  0x21, al                    ; mask all master PIC (IRQ 0..7)
    ; EAX = MULTIBOOT1_MAGIC, EBX = &multiboot_info_t, EIP = kernel entry
    mov  eax, MULTIBOOT1_MAGIC
    ; EBX already holds multiboot_info_t*
    jmp  dword [ebp + foreboots_boot_info.kernel_entry]

; =============================================================================
; Optional long-mode trampoline (assembled only when FOREB_FORCE_LONG_MODE=1).
; Identity-maps the low 2 MiB (enough to reach the kernel at 0x100000), enables
; PAE + long mode, loads a 64-bit GDT, and jumps to the kernel entry in 64-bit
; mode. Page tables are built at 0x10000 (the kernel file buffer, now unused).
; =============================================================================
%if FOREB_FORCE_LONG_MODE
enter_long_mode:
    ; Build PML4 at 0x10000, PDPT at 0x11000, PD at 0x12000 (the kernel file
    ; buffer is no longer needed after ELF parsing).
    mov  dword [0x10000], 0x11000 | 0x03       ; PML4[0] -> PDPT (P|W)
    mov  dword [0x10004], 0
    mov  dword [0x11000], 0x12000 | 0x03       ; PDPT[0] -> PD (P|W)
    mov  dword [0x11004], 0
    mov  dword [0x12000], 0x00000083           ; PD[0] -> 2 MiB page @0 (P|W|PS)
    mov  dword [0x12004], 0
    mov  eax, 0x10000
    mov  cr3, eax                              ; load CR3
    mov  eax, cr4
    or   eax, 1 << 5                           ; CR4.PAE
    mov  cr4, eax
    mov  ecx, 0xC0000080                       ; EFER
    rdmsr
    or   eax, 1 << 8                           ; EFER.LME
    wrmsr
    mov  eax, cr0
    or   eax, 1 << 31                          ; CR0.PG -> activate long mode
    mov  cr0, eax
    ; Far-jump to the kernel entry via the 64-bit code segment. The CPU enters
    ; 64-bit mode at the entry point. EAX = magic, EBX = multiboot_info.
    lgdt [gdt64_desc]
    mov  eax, MULTIBOOT1_MAGIC                 ; EAX = 0x2BADB002
    ; EBX already holds multiboot_info_t*
    mov  edx, [ebp + foreboots_boot_info.kernel_entry]
    push dword gdt64_code
    push edx
    retf                                       ; -> gdt64_code:entry (64-bit)

align 8
GDT64_ENTRIES
gdt64_desc:
    dw  gdt64_end - gdt64_start - 1
    dd  gdt64_start
%endif

; =============================================================================
; Data
; =============================================================================
align 4
s3_elfbase:  dd 0
s3_phdr:     dd 0
s3_count:    dd 0
s3_entsize:  dd 0

%if FOREB_SERIAL_DEBUG
str_serial_stage3: db "[ForeB] stage3: handoff -> kernel (32-bit PM)", 0x0D, 0x0A, 0
%endif

; =============================================================================
; Size assertion
; =============================================================================
stage3_end:

%if (stage3_end - stage3_start) > (STAGE3_SECTOR_COUNT * 512)
    %error "stage3.asm exceeds maximum size!"
%endif

times (STAGE3_SECTOR_COUNT * 512) - (stage3_end - stage3_start) db 0

; =============================================================================
; ForeB - Forest Bootloader
; uefi/handoff64to32.asm  --  Long-mode (x86-64) -> 32-bit protected-mode
;                             kernel handoff trampoline for the UEFI loader.
; =============================================================================
;
; PURPOSE
; -------
; UEFI firmware runs our bootx64.c loader in *64-bit long mode* (paging ON,
; PAE ON, EFER.LMA=1, firmware-identity-mapped address space). The Forest
; kernel, however, demands the exact same entry state that the BIOS ForeB
; stage3 delivers:
;
;     * 32-bit PROTECTED MODE, paging OFF, interrupts OFF (CLI)
;     * flat GDT:  CS = 0x08 (32-bit code, base 0, limit 4 GiB)
;                  DS=ES=FS=GS=SS = 0x10 (32-bit data, base 0, limit 4 GiB)
;     * EAX = 0x2BADB002  (MULTIBOOT1_MAGIC)
;     * EBX = physical address of the multiboot_info_t (ForeB uses 0x1800)
;     * EIP = kernel ELF e_entry (e.g. 0x100000)
;     * both PICs fully masked (out 0x21,0xFF / out 0xA1,0xFF)  -- MANDATORY
;
; The 64-bit Forest kernel performs its OWN long-mode transition after entry
; (src/boot64.asm), so this trampoline must hand off in *pure 32-bit PM* and
; MUST NOT re-enter long mode.
;
; This file is the single most error-prone piece of the UEFI port. Every step
; below is annotated with the CPU-manual rationale.
;
; C-SIDE PROTOTYPE  (call this AFTER ExitBootServices, after all boot_info /
; multiboot structs are written, and after the kernel ELF PT_LOADs are copied):
;
;     void forebo_handoff(uint32_t entry,        // kernel ELF e_entry
;                         uint32_t mb_magic,     // 0x2BADB002
;                         uint32_t mb_info_ptr); // &multiboot_info_t (0x1800)
;
; Microsoft x64 calling convention (clang -target x86_64-*-windows uses it):
;     arg0 entry       -> RCX  (we consume ECX)
;     arg1 mb_magic    -> RDX  (we consume EDX)
;     arg2 mb_info_ptr -> R8   (we consume R8D)
; The function NEVER returns, so we ignore shadow space / callee-saved regs.
;
; -----------------------------------------------------------------------------
; BUILD RECIPE
; -----------------------------------------------------------------------------
; Assemble as a Win64/PE-COFF object (matches clang's -target ...-windows out)
; and link it alongside bootx64.o with lld-link:
;
;     nasm -f win64 \
;          -I/home/bluet/Forest-OS/foreboots \
;          -I/home/bluet/Forest-OS/foreboots/include \
;          uefi/handoff64to32.asm -o uefi/handoff64to32.o
;
;     clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
;           -mno-red-zone -mno-mmx -mno-sse -Wall -Wextra -std=c11 -Iinclude \
;           -c uefi/bootx64.c -o uefi/bootx64.o
;
;     ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
;            -out:BOOTX64.EFI uefi/bootx64.o uefi/handoff64to32.o
;
; NOTE on the Makefile: the current Makefile wildcards uefi/trampoline.asm as
; the optional NASM trampoline. To wire this file in, EITHER rename/symlink it
; to uefi/trampoline.asm, OR point UEFI_TRAMP_SRC at uefi/handoff64to32.asm.
; The exported symbol name (forebo_handoff) is what bootx64.c must `extern`.
;
; -----------------------------------------------------------------------------
; HARD ASSUMPTIONS (true on every UEFI machine we target; documented so the
; failure modes are obvious):
;   A1. This code, its .data (GDT + far pointers + scratch stack), and the
;       kernel entry point all live in the low 4 GiB of physical RAM. UEFI
;       loads boot applications below 4 GiB in practice (OVMF certainly does),
;       and the multiboot handoff addresses (0x1000..0x100000) are low memory.
;       Every address we truncate to 32 bits therefore stays exact.
;   A2. The firmware identity-maps physical memory (mandated by the UEFI spec
;       while boot services are active, and left in place through hand-off).
;       The MOV CR0 that clears paging, and every instruction after it, run
;       from identity-mapped pages so linear == physical across the switch.
;   A3. ExitBootServices() has already been called by the C loader, so we own
;       physical memory and no firmware timer/IRQ will fire once we CLI.
; =============================================================================

; Pull in the shared ABI (magic values, GDT descriptor qwords, selectors).
; boot_protocol.inc auto-%includes config.h (idempotent). We do NOT redefine
; any of its symbols -- we consume FOREB_GDT_CODE32/DATA32 and
; FOREB_SEL_CODE32/DATA32 below.
%include "boot_protocol.inc"

; IA32_EFER model-specific register and its Long-Mode-Enable bit.
%define IA32_EFER_MSR   0xC0000080
%define EFER_LME_BIT    0x00000100      ; bit 8

; CR0 / CR4 bit masks used during the tear-down.
%define CR0_PG          0x80000000      ; CR0 bit 31 (paging)
%define CR4_PAE         0x00000020      ; CR4 bit 5  (physical address extension)

global forebo_handoff

; =============================================================================
; .text  --  runs first in 64-bit long mode, then in 32-bit protected mode.
; =============================================================================
section .text

; -----------------------------------------------------------------------------
; forebo_handoff(entry=RCX, mb_magic=RDX, mb_info_ptr=R8)   [64-bit long mode]
; -----------------------------------------------------------------------------
bits 64
forebo_handoff:
        cli                                 ; A3: no firmware IRQs from here on.

        ; --- Stash the three arguments into registers that (a) survive the
        ;     mode switch as their low-32-bit halves and (b) are NOT clobbered
        ;     by the CR0 / CR4 / RDMSR / WRMSR dance that follows.
        ;     RDMSR/WRMSR destroy EAX/ECX/EDX; the CRn manipulation uses EAX.
        ;     So we park the live values in EBP/ESI/EBX/EDI, none of which any
        ;     later instruction touches until the final jump.
        ;       EBP <- entry        (kernel e_entry, jump target offset)
        ;       ESI <- mb_magic     (goes to EAX right before the jump)
        ;       EBX <- mb_info_ptr  (already the final EBX the kernel reads)
        mov     ebp, ecx                    ; entry   (zero-extends into RBP)
        mov     esi, edx                    ; magic
        mov     ebx, r8d                    ; info

        ; --- Compute the scratch-stack top and keep it in EDI. The kernel is
        ;     not guaranteed any particular stack (neither is GRUB), but we do
        ;     need a valid SS:ESP for the PUSH/RETF far jump below. We use our
        ;     own tiny .data stack rather than trusting the firmware RSP, whose
        ;     location is unknown post-ExitBootServices.  A1: <4 GiB.
        lea     rax, [rel handoff_stack_top]
        mov     edi, eax                    ; EDI = 32-bit &handoff_stack_top

        ; --- Patch the 32-bit compatibility-mode far-jump pointer with the
        ;     runtime address of compat_mode. We build it at runtime (via
        ;     RIP-relative LEA) instead of an absolute relocation so the image
        ;     base is irrelevant; A1 guarantees the truncation is lossless.
        lea     rax, [rel compat_mode]
        mov     [rel fp_compat], eax        ; offset field (selector prefilled)

        ; --- Load our flat GDT. The 10-byte 64-bit pseudo-descriptor needs the
        ;     linear base of the table; fill it at runtime, then LGDT.
        lea     rax, [rel gdt_table]
        mov     [rel gdt_desc + 2], rax     ; 8-byte base
        lgdt    [rel gdt_desc]

        ; --- Far jump into the 32-bit code segment (selector 0x08). Because
        ;     that descriptor has L=0 / D=1, jumping to it from long mode drops
        ;     the CPU into 32-bit COMPATIBILITY mode (still IA-32e: LMA=1, PG=1,
        ;     PAE=1 -- but now executing 32-bit code). This also flushes CS.
        ;     Encoding: FF /5 with 32-bit operand size => indirect m16:32 far
        ;     jump reading [offset:dword][selector:word] from fp_compat.
        jmp far dword [rel fp_compat]

; -----------------------------------------------------------------------------
; compat_mode  --  entered in 32-bit COMPATIBILITY mode (IA-32e still active).
; From here we tear IA-32e down to legacy 32-bit protected mode, per Intel SDM
; Vol.3 "Switching Out of IA-32e Mode Operation".
; -----------------------------------------------------------------------------
bits 32
compat_mode:
        ; (1) Deactivate IA-32e by clearing CR0.PG. Clearing PG while LMA=1 is
        ;     the sanctioned exit path: the CPU immediately sets EFER.LMA=0 and
        ;     leaves long mode. A2 guarantees this MOV and the following
        ;     instructions are identity-mapped, which the manual requires.
        mov     eax, cr0
        and     eax, ~CR0_PG                 ; CR0.PG = 0  -> paging OFF, LMA=0
        mov     cr0, eax

        ; (2) Disable Long-Mode-Enable in EFER so the machine is unambiguously
        ;     a legacy 32-bit CPU (LME=0, LMA=0). RDMSR/WRMSR clobber EAX/EDX;
        ;     our live values are safe in EBP/ESI/EBX/EDI.
        mov     ecx, IA32_EFER_MSR
        rdmsr
        and     eax, ~EFER_LME_BIT           ; clear LME (bit 8)
        wrmsr

        ; (3) Disable PAE. Not strictly required for paging-off 32-bit PM, but
        ;     it matches the clean legacy state ForeB's kernel expects and
        ;     removes any lingering long-mode paging configuration.
        mov     eax, cr4
        and     eax, ~CR4_PAE                ; CR4.PAE = 0
        mov     cr4, eax

        ; (4) Reload the data/stack segment registers with the 32-bit flat data
        ;     selector. Until now DS/ES/... still cached long-mode descriptors.
        mov     ax, FOREB_SEL_DATA32         ; 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax                       ; SS loaded; ESP set on next line
        mov     esp, edi                     ; valid flat SS:ESP for PUSH/RETF

        ; (5) Fully mask both PICs -- MANDATORY. The kernel does an early STI
        ;     with only a page-fault handler installed; a live IRQ0 here would
        ;     triple-fault. (Idempotent if the C loader already did this.)
        mov     al, 0xFF
        out     0x21, al                     ; master PIC: mask IRQ0-7
        out     0xA1, al                     ; slave  PIC: mask IRQ8-15

        ; (6) Establish the multiboot handoff registers and jump.
        mov     eax, esi                     ; EAX = MULTIBOOT1_MAGIC
        ; EBX already = mb_info_ptr (untouched since entry).

        ; Far jump to CS=0x08 : EIP=entry, i.e. into the kernel in pure 32-bit
        ; protected mode. We synthesize the far jump with PUSH selector / PUSH
        ; offset / RETF so the variable entry point lives in a register (EBP)
        ; rather than a relocated memory operand. RETF reloads CS from the
        ; flat 32-bit code descriptor, guaranteeing a correctly-flushed CS.
        push    dword FOREB_SEL_CODE32        ; 0x08
        push    ebp                           ; entry (e_entry)
        retf                                  ; -> kernel, never returns
.hang:  hlt                                   ; unreachable safety net
        jmp     .hang

; =============================================================================
; .data  --  GDT, far-jump pointer, and scratch stack. Writable so the 64-bit
;            prologue can patch the runtime base/offset fields. All of this is
;            in low RAM (A1) and identity-mapped (A2).
; =============================================================================
section .data
align 16

; Flat GDT identical to the ForeB kernel entry contract.
;   0x00 null | 0x08 32-bit flat code | 0x10 32-bit flat data
gdt_table:
        dq 0x0000000000000000               ; 0x00 null descriptor
        dq FOREB_GDT_CODE32                  ; 0x08 = 0x00CF9A000000FFFF
        dq FOREB_GDT_DATA32                  ; 0x10 = 0x00CF92000000FFFF
gdt_end:

; 64-bit LGDT pseudo-descriptor: 2-byte limit + 8-byte base (base patched).
align 8
gdt_desc:
        dw  gdt_end - gdt_table - 1          ; limit
        dq  0                                 ; base (filled at runtime)

; m16:32 far pointer for the long-mode -> compatibility-mode jump.
; offset dword patched at runtime; selector is the constant 32-bit code sel.
align 8
fp_compat:
        dd  0                                 ; offset  (&compat_mode, runtime)
        dw  FOREB_SEL_CODE32                  ; selector 0x08

; Tiny private stack: only needs room for the two PUSHes + RETF frame, but we
; give it slack. ESP is set to the top (grows down).
align 16
handoff_stack:
        times 512 db 0
handoff_stack_top:

; No executable stack.
%ifidn __?OUTPUT_FORMAT?__,elf
section .note.GNU-stack noalloc noexec nowrite progbits
%endif

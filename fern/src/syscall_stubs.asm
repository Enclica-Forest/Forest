; ============================================================================
; Forest OS - Syscall Stub (INT 0x80)
; Correct, ABI-safe syscall entry for 32-bit x86
;
; CPU stack on entry (ring3 -> ring0):
;   SS
;   ESP
;   EFLAGS
;   CS
;   EIP
;
; This frame MUST remain untouched until IRET.
; ============================================================================

%ifndef __x86_64__

BITS 32
SECTION .text
ALIGN 4

extern syscall_handle

global isr128
global isr128_resume

isr128:
    ; ------------------------------------------------------------------------
    ; Preserve userspace execution context
    ; ------------------------------------------------------------------------

    push ds
    push es
    push fs
    push gs

    pusha                   ; EAX ECX EDX EBX ESP EBP ESI EDI

    ; ------------------------------------------------------------------------
    ; Switch to kernel data segments
    ; ------------------------------------------------------------------------

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; ------------------------------------------------------------------------
    ; Call C syscall handler
    ; Pass pointer to saved register frame (top of pusha)
    ; ------------------------------------------------------------------------

    push esp                ; struct syscall_frame*
    call syscall_handle
    add esp, 4

    ; ------------------------------------------------------------------------
    ; Restore userspace execution context
    ;
    ; isr128_resume is also used as a task_switch_asm resume target for
    ; freshly cloned (fork/clone) child tasks: task_clone_current() points
    ; a synthetic popa/popf/pop-ebp/ret frame here so the scheduler resumes
    ; the child exactly like a normal syscall return to userspace, reading
    ; straight out of the child's copy of the pusha block below.
    ; ------------------------------------------------------------------------

isr128_resume:
    popa

    pop gs
    pop fs
    pop es
    pop ds

    ; ------------------------------------------------------------------------
    ; Return to userspace
    ; CPU restores: EIP, CS, EFLAGS, ESP, SS
    ; ------------------------------------------------------------------------

    iret

%else
; ============================================================================
; 64-bit build: not supported yet
; ============================================================================
BITS 64
SECTION .text
global isr128
isr128:
    hlt
    jmp isr128
%endif

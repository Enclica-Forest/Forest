; =============================================================================
; Forest OS - x86_64 SYSCALL/SYSRET Entry Stub
; File: src/syscall64_stubs.asm
;
; This file implements the low-level SYSCALL entry point for 64-bit mode.
; It is invoked directly by the CPU when userspace executes the SYSCALL
; instruction; the target RIP is set by writing the address of
; syscall64_entry into IA32_LSTAR during syscall64_init().
;
; Hardware state on SYSCALL entry (Intel Vol.2B, SYSCALL reference):
;   RCX  <- user RIP   (return address)
;   R11  <- user RFLAGS (before masking with IA32_FMASK)
;   CS   <- IA32_STAR[47:32]        (kernel code selector)
;   SS   <- IA32_STAR[47:32] + 8    (kernel data selector)
;   CPL  <- 0
;   RFLAGS bits cleared by IA32_FMASK (we clear IF and DF)
;   RSP  is NOT changed by the CPU - still points to user stack
;
; Our job:
;   1. SWAPGS to make GS point at the per_cpu_data_t (kernel context)
;   2. Save user RSP into per_cpu_data_t.user_rsp  [GS:0x10]
;   3. Load kernel RSP from per_cpu_data_t.kernel_rsp [GS:0x00]
;   4. Save callee-saved registers (as required by SysV ABI for the C handler)
;   5. Translate r10 -> rcx for the 4th argument (SysV ABI: arg4 in rcx)
;   6. Call syscall64_handle(num, a1, a2, a3, a4, a5, a6)
;   7. Restore callee-saved registers
;   8. Switch back to user stack
;   9. SWAPGS to restore user GS context
;  10. SYSRETQ to return to user mode (CPL=3, RIP=RCX, RFLAGS=R11)
;
; Linux x86_64 ABI register mapping on syscall entry:
;   rax  = syscall number
;   rdi  = arg1
;   rsi  = arg2
;   rdx  = arg3
;   r10  = arg4   (NOT rcx, which is overwritten by SYSCALL with user RIP)
;   r8   = arg5
;   r9   = arg6
;
; SysV calling convention (for syscall64_handle):
;   rdi  = arg1  (already correct)
;   rsi  = arg2  (already correct)
;   rdx  = arg3  (already correct)
;   rcx  = arg4  <- we copy r10 here
;   r8   = arg5  (already correct)
;   r9   = arg6  (already correct)
;   The syscall number (rax) is passed as the first argument by moving
;   rax into rdi before the others; instead we use a wrapper approach:
;   The prototype is:
;       int64_t syscall64_handle(uint64_t num,
;                                uint64_t a1, uint64_t a2, uint64_t a3,
;                                uint64_t a4, uint64_t a5, uint64_t a6)
;   So we need to shift all arguments one position:
;       rdi <- num (was rax)
;       rsi <- a1  (was rdi)
;       rdx <- a2  (was rsi)
;       rcx <- a3  (was rdx)
;       r8  <- a4  (was r10)
;       r9  <- a5  (was r8)
;       push a6   (was r9, passed on stack as 7th argument)
;
; Caller-saved (volatile) registers per SysV ABI:
;   rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
; Callee-saved registers we must preserve:
;   rbx, rbp, r12, r13, r14, r15
; Note: rcx and r11 hold user RIP and RFLAGS respectively (saved by SYSCALL);
; we must preserve them so SYSRETQ can restore the user context correctly.
; =============================================================================

%ifdef __x86_64__

BITS 64
SECTION .text
ALIGN 16

extern syscall64_handle

; Make the entry point visible to C code (syscall64_init writes its address
; into IA32_LSTAR).
global syscall64_entry

; -----------------------------------------------------------------------------
; syscall64_entry - SYSCALL instruction entry point
;
; This label is the value written into IA32_LSTAR.  The CPU jumps here
; directly with CPL=0 and interrupts disabled (IF cleared by IA32_FMASK).
; -----------------------------------------------------------------------------
syscall64_entry:
    ; -------------------------------------------------------------------------
    ; Step 1: Exchange GS.base with IA32_KERNEL_GS_BASE.
    ; After SWAPGS, GS points to our per_cpu_data_t:
    ;   [GS:0x00] = kernel_rsp
    ;   [GS:0x08] = cpu_id
    ;   [GS:0x10] = user_rsp  (scratch slot)
    ; -------------------------------------------------------------------------
    swapgs

    ; -------------------------------------------------------------------------
    ; Step 2: Save user RSP and switch to kernel stack.
    ; We cannot touch the user stack yet - it may be in any state.
    ; The per-CPU area provides a safe place to stash user RSP.
    ; -------------------------------------------------------------------------
    mov [gs:0x10], rsp          ; save user RSP at per_cpu_data_t.user_rsp
    mov rsp, [gs:0x00]          ; load kernel RSP from per_cpu_data_t.kernel_rsp

    ; -------------------------------------------------------------------------
    ; Step 3: Save registers that SYSRETQ needs to restore user context.
    ; rcx = user RIP (written by SYSCALL hardware, needed for SYSRETQ)
    ; r11 = user RFLAGS (written by SYSCALL hardware, needed for SYSRETQ)
    ; We also save callee-saved registers for the SysV C ABI.
    ; -------------------------------------------------------------------------
    push rcx                    ; user RIP (for SYSRETQ)
    push r11                    ; user RFLAGS (for SYSRETQ)
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; -------------------------------------------------------------------------
    ; Step 4: Build the argument frame for syscall64_handle.
    ;
    ; On entry:  rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
    ;
    ; Prototype: int64_t syscall64_handle(num, a1, a2, a3, a4, a5, a6)
    ; SysV args: rdi,     rsi,  rdx,  rcx, r8,   r9,  [rsp+8]
    ;
    ; We need to shift everything:
    ;   7th arg (a6=r9) must go on the stack first (pushed last, before call)
    ;   Then adjust registers:
    ;     rdi <- num (was rax)
    ;     rsi <- a1  (was rdi)
    ;     rdx <- a2  (was rsi)
    ;     rcx <- a3  (was rdx)
    ;     r8  <- a4  (was r10)
    ;     r9  <- a5  (was r8)
    ;   Stack: push a6 (r9 before any modification)
    ;
    ; Register juggling (careful about overwriting before reading):
    ;   r8 currently = a5 (keep)
    ;   r9 currently = a6 (push this first onto stack, then put a5 in r9)
    ;   r10 currently = a4 (move to r8 after saving r8)
    ;   rdx currently = a3 -> rcx
    ;   rsi currently = a2 -> rdx
    ;   rdi currently = a1 -> rsi
    ;   rax currently = num -> rdi
    ;
    ; Safe order:
    ;   1. push r9 (a6) onto stack for 7th argument slot
    ;   2. r9  <- r8  (a5)
    ;   3. r8  <- r10 (a4)
    ;   4. rcx <- rdx (a3)
    ;   5. rdx <- rsi (a2)
    ;   6. rsi <- rdi (a1)
    ;   7. rdi <- rax (num)
    ; -------------------------------------------------------------------------

    ; Align the stack to 16 bytes before the call.
    ; We have pushed: rcx, r11, rbp, rbx, r12, r13, r14, r15 = 8 * 8 = 64 bytes.
    ; We will push one more 8-byte value (a6) = 72 bytes below the saved frame.
    ; RSP before call must be 16-byte aligned after the return address is pushed.
    ; With one push here (a6=8 bytes) + call pushes return addr (8 bytes): 16 bytes added.
    ; Stack was 16-aligned at entry (kernel stack is set up aligned), so after
    ; 8 pushes (64 bytes) it is still 16-aligned. Then push a6 (8 bytes) makes
    ; it 8-byte aligned, call pushes 8 bytes => 16-byte aligned inside the callee.
    ; This is correct per SysV ABI.

    push r9                     ; a6 on stack (7th argument slot)

    mov r9, r8                  ; r9  <- a5
    mov r8, r10                 ; r8  <- a4
    mov rcx, rdx                ; rcx <- a3
    mov rdx, rsi                ; rdx <- a2
    mov rsi, rdi                ; rsi <- a1
    mov rdi, rax                ; rdi <- num

    ; -------------------------------------------------------------------------
    ; Step 5: Enable interrupts before calling into the C handler.
    ; Syscall entry cleared IF via IA32_FMASK. It is safe to re-enable
    ; interrupts now that we are on the kernel stack with registers saved.
    ; (Many production kernels re-enable here; we follow suit.)
    ; -------------------------------------------------------------------------
    sti

    ; -------------------------------------------------------------------------
    ; Step 6: Call the C dispatcher.
    ; -------------------------------------------------------------------------
    call syscall64_handle

    ; -------------------------------------------------------------------------
    ; Step 7: Disable interrupts before restoring user context.
    ; We must not be interrupted while switching stacks back to user mode.
    ; -------------------------------------------------------------------------
    cli

    ; Clean up the 7th argument we pushed on the stack.
    add rsp, 8

    ; -------------------------------------------------------------------------
    ; Step 8: Restore callee-saved registers and the SYSRETQ state.
    ; rax holds the syscall return value from syscall64_handle; do not clobber.
    ; -------------------------------------------------------------------------
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    pop r11                     ; user RFLAGS (for SYSRETQ)
    pop rcx                     ; user RIP    (for SYSRETQ)

    ; -------------------------------------------------------------------------
    ; Step 9: Restore user RSP from the per-CPU scratch slot.
    ; -------------------------------------------------------------------------
    mov rsp, [gs:0x10]          ; restore user RSP

    ; -------------------------------------------------------------------------
    ; Step 10: Restore user GS context and return to user mode.
    ; SWAPGS swaps GS.base back to the user value (IA32_GS_BASE).
    ; SYSRETQ restores: RIP <- RCX, RFLAGS <- R11, CPL <- 3,
    ;                   CS <- STAR[63:48]+16 | RPL=3
    ;                   SS <- STAR[63:48]+8  | RPL=3
    ; -------------------------------------------------------------------------
    swapgs
    sysretq

%else
; =============================================================================
; 32-bit build: this file provides a stub that halts.
; The 32-bit syscall path uses INT 0x80 via syscall_stubs.asm instead.
; =============================================================================
BITS 32
SECTION .text
ALIGN 4
global syscall64_entry
syscall64_entry:
    hlt
    jmp syscall64_entry
%endif

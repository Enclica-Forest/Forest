#include "include/task.h"
#include "include/kernel_safety.h"
#include "include/memory.h"
#include "include/screen.h"
#include "include/panic.h"
#include "include/util.h"
#include "include/elf.h"
#include "include/interrupt.h" // For context switching using new system
#include "include/gdt.h"
#include "include/spinlock.h"
#include "include/string.h"
#include "include/timer.h"
#include "include/ps2_mouse.h"
#include "include/mm.h"
#include "include/debuglog.h"
#include "include/auth.h"
#include "include/framebuffer.h"
#include "include/syscall.h"   // signal_is_ignored() - process-wide sigaction table
#include "include/net.h"       // net_close_all_for_task() - release sockets on exit
#include "include/semaphore.h" // semaphore_remove_task() - unlink wait node on kill
#include "job_control.h"       // job_update_state() - stop-class signal wiring

// epoll/eventfd/inotify/sysv-ipc/posix-shm cleanup-on-exit. Declared locally
// rather than via unix_compat.h: that header's other declarations pull in
// key_t/sigset_t/mode_t/off_t, which every one of these subsystems'  .c
// files currently typedefs locally instead of sharing from a common
// POSIX-types header, so actually including unix_compat.h here fails to
// compile. These six only need a bare pid, so a narrow local prototype
// avoids that mismatch entirely.
void epoll_close_all_for_task(uint32 pid);
void eventfd_close_all_for_task(uint32 pid);
void inotify_close_all_for_task(uint32 pid);
void sysv_msg_close_all_for_task(uint32 pid);
void sysv_sem_close_all_for_task(uint32 pid);
void posix_shm_close_all_for_task(uint32 pid);
#include <stddef.h>

// Architecture detection
#ifndef ARCH_64BIT
#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_64BIT 1
#else
#define ARCH_64BIT 0
#endif
#endif

// Architecture-specific stack word type
#if ARCH_64BIT
typedef uint64 stack_word_t;
#define STACK_WORD_SIZE 8
#else
typedef uint32 stack_word_t;
#define STACK_WORD_SIZE 4
#endif

// Explicit forward declaration to help compiler resolve implicit declaration
extern page_directory_t* vmm_get_current_page_directory(void);
extern string long_to_string(long n);
extern void vmm_destroy_page_directory(page_directory_t* dir);  // For deferred cleanup

#define KERNEL_STACK_SIZE 8192 // 8KB for kernel stack per task
#define USER_STACK_SIZE 32   // 32 pages, 128KB for user stack (more suitable for GUI apps)
// USER_STACK_TOP is defined in memory.h

task_t* current_task = 0;
task_t* ready_queue_head = 0;
static task_t* idle_task = 0; // Idle task that runs when no other tasks are available
static task_t* foreground_task = 0; // GUI app that gets priority scheduling
static uint32 next_task_id = 1;

static spinlock_t task_scheduler_lock = SPINLOCK_INIT("task_scheduler");

#define MAX_DEFERRED_CLEANUP 16
static task_t* deferred_cleanup_tasks[MAX_DEFERRED_CLEANUP];
static uint32_t deferred_cleanup_count = 0;
static spinlock_t deferred_cleanup_lock = SPINLOCK_INIT("deferred_cleanup");

static ipc_shm_region_t ipc_shm_regions[IPC_MAX_SHM_REGIONS];
static ipc_msg_queue_t ipc_msg_queues[IPC_MAX_MSG_QUEUES];
static spinlock_t ipc_lock = SPINLOCK_INIT("ipc");
static uint32_t ipc_next_shm_id = 1;
static uint32_t ipc_next_msg_id = 1;

// Temporary stack for initial task setup
// This will be replaced by a proper kernel stack for each task
static uint8 initial_kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(4096)));

static bool task_ptr_plausible(const task_t* task) {
    if (!task) {
        return false;
    }

    uintptr_t addr = (uintptr_t)task;
    uintptr_t heap_start = (uintptr_t)memory_get_kernel_heap_start();
    uintptr_t heap_end = heap_start + MEMORY_KERNEL_HEAP_MAX_SIZE;

    if (addr < heap_start || (addr + sizeof(task_t)) > heap_end) {
        return false;
    }

    if ((addr & (sizeof(uintptr_t) - 1)) != 0) {
        return false;
    }

    return true;
}

static bool task_sanitize_ready_queue_locked(void);

static bool task_recover_ready_queue_locked(const char* reason) {
    print("[TASK] WARNING: Recovering ready queue: ");
    print(reason ? reason : "unknown");
    print("\n");

    task_t* fallback = NULL;
    if (task_ptr_plausible(current_task)) {
        fallback = current_task;
    } else if (task_ptr_plausible(idle_task)) {
        fallback = idle_task;
        current_task = idle_task;
    }

    if (!fallback) {
        ready_queue_head = NULL;
        return false;
    }

    ready_queue_head = fallback;
    if (!task_ptr_plausible(fallback->next)) {
        fallback->next = fallback;
    }
    return true;
}

static task_t* task_pick_safe_fallback_locked(const char* reason) {
    if (reason) {
        print("[TASK] WARNING: Scheduler fallback: ");
        print(reason);
        print("\n");
    }

    if (!task_sanitize_ready_queue_locked()) {
        return NULL;
    }

    task_t* fallback = NULL;
    if (task_ptr_plausible(current_task) &&
        (current_task->state == TASK_STATE_READY || current_task->state == TASK_STATE_RUNNING)) {
        fallback = current_task;
    } else if (task_ptr_plausible(ready_queue_head) &&
               (ready_queue_head->state == TASK_STATE_READY || ready_queue_head->state == TASK_STATE_RUNNING)) {
        fallback = ready_queue_head;
    } else if (task_ptr_plausible(idle_task)) {
        fallback = idle_task;
    }

    if (!fallback) {
        return NULL;
    }

    if (!task_ptr_plausible(fallback->next)) {
        fallback->next = fallback;
    }
    fallback->state = TASK_STATE_RUNNING;
    ready_queue_head = fallback;
    current_task = fallback;
    return fallback;
}

static bool task_sanitize_ready_queue_locked(void) {
    if (!ready_queue_head) {
        return true;
    }

    if (!task_ptr_plausible(ready_queue_head)) {
        return task_recover_ready_queue_locked("ready_queue_head out of heap range");
    }

    task_t* node = ready_queue_head;
    for (int guard = 0; guard < 2048; guard++) {
        if (!task_ptr_plausible(node)) {
            return task_recover_ready_queue_locked("queue node pointer invalid");
        }

        if (!task_ptr_plausible(node->next)) {
            print("[TASK] WARNING: Re-linking corrupted next pointer in ready queue\n");
            node->next = ready_queue_head;
            return true;
        }

        node = node->next;
        if (node == ready_queue_head) {
            return true;
        }
    }

    print("[TASK] WARNING: Ready queue loop guard hit, forcing ring closure\n");
    node->next = ready_queue_head;
    return true;
}

task_t* task_find_by_pid(uint32 pid) {
    if (!ready_queue_head) {
        return NULL;
    }
    task_t* t = ready_queue_head;
    do {
        if (t->id == pid) {
            return t;
        }
        t = t->next;
    } while (t && t != ready_queue_head);
    return NULL;
}

// Read-only enumeration for the OOM killer (mm_oom.c): hands each live
// task's scoring-relevant fields to `fn` by value rather than exposing the
// task_t* itself, so a caller can't accidentally retain a pointer past the
// point where the task might be destroyed. Matches task_find_by_pid()'s
// existing convention of walking ready_queue_head without holding
// task_scheduler_lock (a read-only scan, consistent with the rest of this
// file's lookup helpers).
void task_for_each_oom_candidate(task_oom_visit_fn fn, void* ctx) {
    if (!fn || !ready_queue_head) {
        return;
    }
    task_t* t = ready_queue_head;
    do {
        fn(t->id, t->name, t->memory_used, t->is_protected, t->parent_pid == 0, ctx);
        t = t->next;
    } while (t && t != ready_queue_head);
}

uint32 task_get_real_priority(task_t* task) {
    if (!task) {
        return TASK_PRIORITY_NORMAL;
    }
    uint32 now = timer_get_ticks();
    uint32 base = task->priority;
    if (task->is_graphics_task) {
        if (base < TASK_PRIORITY_GUI) {
            base = TASK_PRIORITY_GUI;
        }
    } else if (task->has_framebuffer_mapping || task_is_foreground(task)) {
        if (base < TASK_PRIORITY_GUI) {
            base = TASK_PRIORITY_GUI;
        }
    }
    if (task->boost_expires_at > 0 && now < task->boost_expires_at) {
        base += 2;
    }
    if (base > TASK_PRIORITY_MAX) {
        base = TASK_PRIORITY_MAX;
    }
    return base;
}

static uint32 task_compute_ticks(task_t* task) {
    uint32 prio = task_get_real_priority(task);
    return 2 + (prio * 2);
}

static void ipc_init(void) {
    memory_set((uint8*)ipc_shm_regions, 0, sizeof(ipc_shm_regions));
    memory_set((uint8*)ipc_msg_queues, 0, sizeof(ipc_msg_queues));
}

// Idle task function - runs when no other tasks are available
static void idle_task_function(void) {
    for (;;) {
        // Use STI+HLT to halt but allow interrupts to wake CPU
        // This is safe because we're in kernel context with interrupts handled
        __asm__ __volatile__("sti; hlt");
    }
}

// Forward declarations for assembly functions (defined in context_switch.asm)
extern void task_switch_asm(uintptr_t* old_sp_ptr, uintptr_t new_sp_val, uintptr_t new_page_directory_phys);
extern void task_start_usermode_asm(void);  // Entry point for IRET to user mode
extern void isr128_resume(void);  // isr128's restore+iret epilogue (defined in syscall_stubs.asm)
// Note: jump_to_usermode_asm is deprecated and will trap if called

// Forward declaration for fork()'s address-space isolation (defined in mm_cow_impl.c).
// Declared directly rather than via mm_cow.h: that header's cow_init() prototype
// conflicts with a separate, differently-typed cow_init() declared in mm.h, and both
// get compiled in -- pulling in mm_cow.h here would surface that pre-existing clash.
extern page_directory_t* cow_fork_address_space(page_directory_t* parent);

// Sync kernel PDEs into a task's page directory (defined in vmm.c)
// Must be called before switching to a task to ensure all kernel heap pages
// allocated after the task was created are visible through its CR3.
extern void vmm_sync_kernel_pdes(page_directory_t* task_dir);

// Simple helpers for mapping/unmapping user pages for task-local regions
static bool task_map_user_pages(page_directory_t* dir, uint32 start, uint32 end, uint32 flags) {
    if (!dir || start >= end) {
        return false;
    }

    uint32 aligned_start = memory_align_down(start, MEMORY_PAGE_SIZE);
    uint32 aligned_end = memory_align_up(end, MEMORY_PAGE_SIZE);

    for (uint32 va = aligned_start; va < aligned_end; va += MEMORY_PAGE_SIZE) {
        uint32 frame = pmm_alloc_frame();
        if (!frame) {
            return false;
        }

        memory_result_t res = vmm_map_page(dir, va, frame, flags);
        if (res == MEMORY_ERROR_ALREADY_MAPPED) {
            pmm_free_frame(frame);
            continue;
        }

        if (res != MEMORY_OK) {
            pmm_free_frame(frame);
            return false;
        }
    }

    return true;
}

static void task_unmap_user_pages(page_directory_t* dir, uint32 start, uint32 end) {
    if (!dir || start >= end) {
        return;
    }

    uint32 aligned_start = memory_align_down(start, MEMORY_PAGE_SIZE);
    uint32 aligned_end = memory_align_up(end, MEMORY_PAGE_SIZE);

    for (uint32 va = aligned_start; va < aligned_end; va += MEMORY_PAGE_SIZE) {
        uint32 phys = vmm_get_physical_addr(dir, va);
        vmm_unmap_page(dir, va);
        if (phys) {
            pmm_free_frame(phys);
        }
    }
}

// Ensure the target task's kernel stack virtual range is mapped in task->page_directory.
// Missing pages are copied from source_pd using the same physical frames.
static bool task_ensure_kernel_stack_mapped(task_t* task, page_directory_t* source_pd) {
    if (!task || !task->page_directory || !source_pd || !task->kernel_stack_base) {
        return false;
    }

    uint32 stack_start = memory_align_down((uint32)task->kernel_stack_base, MEMORY_PAGE_SIZE);
    uint32 stack_end = memory_align_up((uint32)(task->kernel_stack_base + KERNEL_STACK_SIZE), MEMORY_PAGE_SIZE);

    for (uint32 va = stack_start; va < stack_end; va += MEMORY_PAGE_SIZE) {
        if (vmm_is_mapped(task->page_directory, va)) {
            continue;
        }

        uint32 pa = vmm_get_physical_addr(source_pd, va);
        if (!pa) {
            debuglog(DEBUG_ERROR, "[TASK] Missing source mapping for kernel stack page: va=0x%x src_pd=0x%x task=%u\n",
                     va, (uint32)source_pd, task->id);
            return false;
        }

        memory_result_t res = vmm_map_page(task->page_directory, va, pa, PAGE_PRESENT | PAGE_WRITABLE);
        if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
            debuglog(DEBUG_ERROR, "[TASK] Failed to repair kernel stack mapping: va=0x%x pa=0x%x pd=0x%x res=%d task=%u\n",
                     va, pa, (uint32)task->page_directory, res, task->id);
            return false;
        }
    }

    return true;
}

static bool validate_initial_usermode_frame(task_t* task) {
#if ARCH_64BIT
    (void)task;
    return true;
#else
    if (!task || !task->kernel_stack) {
        return false;
    }

    const uint32 expected_ret = (uint32)(uintptr_t)task_start_usermode_asm;
    static const int32 offsets[] = {0, -1, 1, -2, 2, -3, 3};
    const uint32 expected_eip = (uint32)task->usermode_entry_point;
    const uint32 expected_esp = (uint32)task->usermode_stack_top;

    for (uint32 i = 0; i < (sizeof(offsets) / sizeof(offsets[0])); i++) {
        int32 off = offsets[i];
        const uint32* frame = (const uint32*)((uintptr_t)task->kernel_stack + (off * (int32)sizeof(uint32)));

        uint32 ret_addr = frame[10];
        uint32 iret_eip = frame[11];
        uint32 iret_cs = frame[12];
        uint32 iret_eflags = frame[13];
        uint32 iret_esp = frame[14];
        uint32 iret_ss = frame[15];

        if (ret_addr != expected_ret) {
            continue;
        }
        if (iret_cs != GDT_USER_CODE_SELECTOR || iret_ss != GDT_USER_DATA_SELECTOR) {
            continue;
        }
        if (iret_eip != expected_eip || iret_esp != expected_esp) {
            continue;
        }
        if ((iret_eflags & 0x200) == 0 || (iret_eflags & 0x2) == 0) {
            continue;
        }

        if (off != 0) {
            task->kernel_stack = (uintptr_t)frame;
            debuglog(DEBUG_WARN,
                     "[TASK] Auto-aligned initial switch frame: task=%u offset=%d new_esp=0x%x\n",
                     task->id, off, (uint32)task->kernel_stack);
        }
        return true;
    }

    {
        const uint32* frame = (const uint32*)task->kernel_stack;
        debuglog(DEBUG_ERROR,
                 "[TASK] Invalid initial frame near ESP=0x%x ret[9]=0x%x ret[10]=0x%x eip[11]=0x%x cs[12]=0x%x\n",
                 (uint32)task->kernel_stack, frame[9], frame[10], frame[11], frame[12]);
    }
    return false;
#endif
}

static void setup_initial_cpu_state(task_t* task,
                                    uintptr_t entry_point,
                                    uintptr_t user_stack_top,
                                    uintptr_t kernel_stack_top) {
    // Prepare the kernel stack so task_switch_asm can restore registers and
    // eventually drop to user mode via task_start_usermode_asm.
    //
    // Method A (OSDev Wiki): Build complete stack frame once, execute IRET.
    //
    // Flow: task_switch_asm() -> POPA, POPF, POP EBP, RET
    //       -> RET jumps to task_start_usermode_asm
    //       -> task_start_usermode_asm sets DS/ES/FS/GS, then IRET
    //       -> CPU in Ring 3 at ELF entry point!

    stack_word_t* stack_ptr = (stack_word_t*)kernel_stack_top;

    #if ARCH_64BIT
    // =========================================================================
    // 64-BIT LAYOUT (high address -> low address)
    // =========================================================================
    // Stack grows DOWN. We push from kernel_stack_top downward.
    //
    // After task_switch_asm does: pop r15..rax, popfq, pop rbp, ret
    //   -> RET jumps to task_start_usermode_asm
    // task_start_usermode_asm does: set segments, iretq
    //   -> IRETQ pops RIP, CS, RFLAGS, RSP, SS and drops to ring 3
    //
    // Layout:
    //   [IRETQ frame - 5 qwords for ring change]
    //   [task_switch_asm frame: return addr, RBP, RFLAGS, 14 registers]
    // =========================================================================

    // === IRETQ frame (pushed first, ends up at highest addresses) ===
    // IRETQ pops in order: RIP, CS, RFLAGS, RSP, SS
    // So we push in reverse: SS, RSP, RFLAGS, CS, RIP
    *(--stack_ptr) = GDT_USER_DATA_SELECTOR;    // SS (user data segment, ring 3)
    *(--stack_ptr) = user_stack_top;            // RSP (user stack pointer)
    *(--stack_ptr) = 0x202;                     // RFLAGS (IF=1, reserved bit 1=1)
    *(--stack_ptr) = GDT_USER_CODE_SELECTOR;    // CS (user code segment, ring 3)
    *(--stack_ptr) = entry_point;               // RIP (entry point of ELF)

    // === Frame for task_switch_asm ===
    // task_switch_asm does: pop r15..rax, popfq, pop rbp, ret

    // Return address - RET from task_switch_asm jumps here
    *(--stack_ptr) = (stack_word_t)(uintptr_t)task_start_usermode_asm;

    // Saved RBP for 'pop rbp'
    *(--stack_ptr) = 0;

    // RFLAGS for 'popfq'. IF must stay CLEAR here: this is the outer
    // task_switch_asm trampoline frame, not the real ring-3 transition.
    // Setting IF=1 this early re-enables interrupts several instructions
    // before task_start_usermode_asm's IRETQ, so a timer IRQ can land
    // mid-trampoline on a half-restored stack. The real IRETQ frame above
    // already carries IF=1 and enables interrupts atomically on its own.
    *(--stack_ptr) = 0x002;

    // Register values for task_switch_asm's individual pops (reverse order):
    // rax, rcx, rdx, rbx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15
    *(--stack_ptr) = 0;  // RAX
    *(--stack_ptr) = 0;  // RCX
    *(--stack_ptr) = 0;  // RDX
    *(--stack_ptr) = 0;  // RBX
    *(--stack_ptr) = 0;  // RSI
    *(--stack_ptr) = 0;  // RDI
    *(--stack_ptr) = 0;  // R8
    *(--stack_ptr) = 0;  // R9
    *(--stack_ptr) = 0;  // R10
    *(--stack_ptr) = 0;  // R11
    *(--stack_ptr) = 0;  // R12
    *(--stack_ptr) = 0;  // R13
    *(--stack_ptr) = 0;  // R14
    *(--stack_ptr) = 0;  // R15

    #else
    // =========================================================================
    // 32-BIT LAYOUT (high address -> low address)
    // =========================================================================
    // Stack grows DOWN. We push from kernel_stack_top downward.
    //
    // After task_switch_asm does: popa, popf, pop ebp, ret
    //   -> RET jumps to task_start_usermode_asm
    // task_start_usermode_asm does: set DS/ES/FS/GS = 0x23, then iret
    //   -> IRET pops EIP, CS, EFLAGS, ESP, SS and drops to ring 3
    //
    // Stack layout (HIGH to LOW address):
    //   +----------------------------------+
    //   | SS = 0x23 (user data)            |  <- IRET frame (5 dwords)
    //   | ESP = user_stack_top             |
    //   | EFLAGS = 0x202                   |
    //   | CS = 0x1B (user code)            |
    //   | EIP = entry_point                |
    //   +----------------------------------+
    //   | return_addr = task_start_usermode_asm | <- for 'ret'
    //   | saved EBP = 0                    |      <- for 'pop ebp'
    //   | EFLAGS = 0x202                   |      <- for 'popf'
    //   | EAX = 0                          |  <- POPA frame (8 dwords)
    //   | ECX = 0                          |     for 'popa'
    //   | EDX = 0                          |
    //   | EBX = 0                          |
    //   | ESP = 0 (dummy, ignored)         |
    //   | EBP = 0                          |
    //   | ESI = 0                          |
    //   | EDI = 0                          |
    //   +----------------------------------+  <- task->kernel_stack points HERE
    // =========================================================================

    // === IRET frame (pushed first, ends up at highest addresses) ===
    // IRET pops in order: EIP, CS, EFLAGS, ESP, SS
    // So we push in reverse: SS, ESP, EFLAGS, CS, EIP
    *(--stack_ptr) = GDT_USER_DATA_SELECTOR;    // SS (user data segment, ring 3)
    *(--stack_ptr) = user_stack_top;            // ESP (user stack pointer)
    *(--stack_ptr) = 0x202;                     // EFLAGS (IF=1, reserved bit 1=1)
    *(--stack_ptr) = GDT_USER_CODE_SELECTOR;    // CS (user code segment, ring 3)
    *(--stack_ptr) = entry_point;               // EIP (entry point of ELF)

    // === Frame for task_switch_asm ===
    // task_switch_asm does: popa, popf, pop ebp, ret
    //
    // Stack layout (from HIGH to LOW address, i.e., order we push):
    //   [HIGH] return_addr -> for 'ret'
    //          saved_ebp   -> for 'pop ebp'
    //          eflags      -> for 'popf'
    //          8 GPRs      -> for 'popa' (EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI)
    //   [LOW]  <- task->kernel_stack points here

    // Return address - where 'ret' from task_switch_asm will jump to
    *(--stack_ptr) = (stack_word_t)(uintptr_t)task_start_usermode_asm;

    // Saved EBP for 'pop ebp' - can be 0 since we don't care about frame pointer
    *(--stack_ptr) = 0;

    // EFLAGS for 'popf'. IF must stay CLEAR here: this is the outer
    // task_switch_asm trampoline frame, not the real ring-3 transition.
    // Setting IF=1 this early re-enables interrupts several instructions
    // before task_start_usermode_asm's IRET, so a timer IRQ can land
    // mid-trampoline on a half-restored stack. The real IRET frame above
    // already carries IF=1 and enables interrupts atomically on its own.
    *(--stack_ptr) = 0x002;

    // POPA frame - popa pops: EDI, ESI, EBP, (skip ESP), EBX, EDX, ECX, EAX
    // We push in reverse order of popa: EAX first (highest), EDI last (lowest)
    *(--stack_ptr) = 0;  // EAX
    *(--stack_ptr) = 0;  // ECX
    *(--stack_ptr) = 0;  // EDX
    *(--stack_ptr) = 0;  // EBX
    *(--stack_ptr) = 0;  // ESP (dummy, skipped by POPA)
    *(--stack_ptr) = 0;  // EBP
    *(--stack_ptr) = 0;  // ESI
    *(--stack_ptr) = 0;  // EDI

    #endif

    // task->kernel_stack points to where task_switch_asm will load ESP/RSP
    // This is the BOTTOM of the frame we just constructed
    task->kernel_stack = (uintptr_t)stack_ptr;

    // DEBUG: Verify the stack contents
    debuglog(DEBUG_INFO, "[STACK_SETUP] Kernel ESP will be: 0x%x\n", (uint32)(uintptr_t)stack_ptr);
    debuglog(DEBUG_INFO, "[STACK_SETUP] User ESP (IRET frame): 0x%x\n", user_stack_top);
    debuglog(DEBUG_INFO, "[STACK_SETUP] task_start_usermode_asm addr: 0x%x\n", (uint32)(uintptr_t)task_start_usermode_asm);
    debuglog(DEBUG_INFO, "[STACK_SETUP] Return addr at offset 40: 0x%x\n", *(uint32*)((uintptr_t)stack_ptr + 40));
                                    }

                                    static stack_word_t* prepare_kernel_task_stack(void (*entry_point)(void), stack_word_t* stack_top) {
                                        if (!stack_top) {
                                            return NULL;
                                        }

                                        stack_word_t* sp = stack_top;

                                        #if ARCH_64BIT
                                        // 64-bit: task_switch_asm does: individual pops (r15-r8, rdi-rax), popfq, pop rbp, ret
                                        // Stack layout from HIGH to LOW:
                                        //   Padding (keeps RSP % 16 == 8 at task entry, like a call would)
                                        //   Return address (entry_point)
                                        //   Saved RBP (for pop rbp)
                                        //   RFLAGS (for popfq)
                                        //   RAX, RCX, RDX, RBX, RSI, RDI, R8-R15

                                        // ABI alignment padding so the task starts with RSP % 16 == 8
                                        *(--sp) = 0;

                                        // Return address for the final RET in task_switch_asm
                                        *(--sp) = (stack_word_t)(uintptr_t)entry_point;

                                        // Saved RBP for 'pop rbp'
                                        *(--sp) = 0;

                                        // RFLAGS to be restored by POPFQ. IF must stay CLEAR: this
                                        // outer frame is a trampoline (to task_start_usermode_asm for
                                        // a fresh task, or to isr128_resume for a cloned child), not
                                        // the real ring-3 transition. Setting IF=1 here re-enables
                                        // interrupts before the real IRETQ, letting a timer IRQ land
                                        // mid-unwind on a half-restored stack -- the real IRETQ frame
                                        // (or isr128_resume's copied one) already carries IF=1 and
                                        // enables interrupts atomically as part of its own IRETQ.
                                        *(--sp) = 0x002;

                                        // Values for general purpose registers (pushed in reverse pop order)
                                        *(--sp) = 0; // RAX
                                        *(--sp) = 0; // RCX
                                        *(--sp) = 0; // RDX
                                        *(--sp) = 0; // RBX
                                        *(--sp) = 0; // RSI
                                        *(--sp) = 0; // RDI
                                        *(--sp) = 0; // R8
                                        *(--sp) = 0; // R9
                                        *(--sp) = 0; // R10
                                        *(--sp) = 0; // R11
                                        *(--sp) = 0; // R12
                                        *(--sp) = 0; // R13
                                        *(--sp) = 0; // R14
                                        *(--sp) = 0; // R15
                                        #else
                                        // 32-bit: task_switch_asm does: popa, popf, pop ebp, ret
                                        // Stack layout from HIGH to LOW:
                                        //   Return address (entry_point)
                                        //   Saved EBP (for pop ebp)
                                        //   EFLAGS (for popf)
                                        //   EAX, ECX, EDX, EBX, ESP(dummy), EBP, ESI, EDI (for popa)

                                        // Return address for the final RET in task_switch_asm
                                        *(--sp) = (stack_word_t)(uintptr_t)entry_point;

                                        // Saved EBP for 'pop ebp'
                                        *(--sp) = 0;

                                        // EFLAGS to be restored by POPF. IF must stay CLEAR: this
                                        // outer frame is a trampoline (to task_start_usermode_asm for
                                        // a fresh task, or to isr128_resume for a cloned child), not
                                        // the real ring-3 transition. Setting IF=1 here re-enables
                                        // interrupts before the real IRET, letting a timer IRQ land
                                        // mid-unwind on a half-restored stack -- the real IRET frame
                                        // (or isr128_resume's copied one) already carries IF=1 and
                                        // enables interrupts atomically as part of its own IRET.
                                        *(--sp) = 0x002;

                                        // Values consumed by POPA (push in reverse order so that the first POPA writes EDI)
                                        *(--sp) = 0; // EAX
                                        *(--sp) = 0; // ECX
                                        *(--sp) = 0; // EDX
                                        *(--sp) = 0; // EBX
                                        *(--sp) = 0; // Dummy ESP
                                        *(--sp) = 0; // EBP
                                        *(--sp) = 0; // ESI
                                        *(--sp) = 0; // EDI
                                        #endif

                                        return sp;
                                    }


                                    // Helper function to create a kernel task with a function pointer
                                    static task_t* create_kernel_task(void (*entry_point)(void), const char* name) {
                                        task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
                                        if (!new_task) {
                                            print_colored("[TASK] Failed to allocate memory for kernel task: ", 0x0C, 0x00);
                                            print(name);
                                            print("\n");
                                            return 0;
                                        }

                                        memory_set((uint8*)new_task, 0, sizeof(task_t));
                                        strncpy(new_task->name, name, 31);
                                        new_task->name[31] = '\0';
                                        new_task->id = next_task_id++;
                                        new_task->pgrp = new_task->id;  // Default to own PID as process group
                                        new_task->session = new_task->id;  // Default to own PID as session
                                        new_task->tty_fd = -1;  // No controlling terminal by default
                                        new_task->parent_pid = current_task ? current_task->id : 0;
                                        new_task->signal_mask = 0;
                                        new_task->vt_index = -1;
                                        // Inherit CWD from parent, or default to "/"
                                        if (current_task && current_task->cwd[0] != '\0') {
                                            strncpy(new_task->cwd, current_task->cwd, 255);
                                            new_task->cwd[255] = '\0';
                                        } else {
                                            strncpy(new_task->cwd, "/", 2);
                                        }
                                        /*
                                         * Keep new ELF tasks non-runnable until the caller explicitly promotes
                                         * them to READY. This avoids first-run context switches from timer IRQ
                                         * context while task creation is still unwinding.
                                         */
                                        new_task->state = TASK_STATE_WAITING;
                                        new_task->page_directory = vmm_get_current_page_directory(); // Use current kernel PD
                                        new_task->is_background = false;
                                        new_task->has_framebuffer_mapping = false;
                                        new_task->is_graphics_task = false;
                                        new_task->is_protected = false;
                                        new_task->priority = TASK_PRIORITY_NORMAL;
                                        new_task->ticks_left = 0;
                                        new_task->pending_signals = 0;
                                        new_task->last_active_tick = 0;
                                        new_task->cpu_ticks_total = 0;
                                        new_task->scheduled_at_tick = timer_get_ticks();
                                        new_task->created_at_tick = new_task->scheduled_at_tick;
                                        new_task->next = 0;
                                        new_task->next_in_pgrp = 0;
                                        new_task->exit_code = 0;
                                        memory_set((uint8*)new_task->exit_reason, 0, sizeof(new_task->exit_reason));
                                        new_task->exit_reason[0] = '\0';
                                        new_task->uid = 0;
                                        new_task->gid = 0;
                                        new_task->groups_mask = 1;
                                        new_task->user_heap_base = 0;
                                        new_task->user_heap_limit = 0;
                                        new_task->user_brk = 0;
                                        new_task->original_priority = TASK_PRIORITY_NORMAL;
                                        new_task->boost_expires_at = 0;
                                        new_task->watchdog_enabled = false;
                                        new_task->consecutive_timeouts = 0;
                                        new_task->sleep_until_tick = 0;
                                        new_task->memory_quota = 0;
                                        new_task->memory_used = 0;

                                        // No ELF info for kernel tasks
                                        memory_set((uint8*)&new_task->elf_info, 0, sizeof(elf_load_info_t));

                                        // Allocate kernel stack
                                        uintptr_t kernel_stack_vaddr = (uintptr_t)kmalloc_aligned(KERNEL_STACK_SIZE, MEMORY_PAGE_SIZE);
                                        if (!kernel_stack_vaddr) {
                                            print_colored("[TASK] Failed to allocate kernel stack for task: ", 0x0C, 0x00);
                                            print(name);
                                            print("\n");
                                            kfree(new_task);
                                            return 0;
                                        }
                                        new_task->kernel_stack_base = kernel_stack_vaddr;

                                        // Set up kernel stack for entry point
                                        stack_word_t* stack_ptr = (stack_word_t*)(kernel_stack_vaddr + KERNEL_STACK_SIZE);
                                        stack_ptr = prepare_kernel_task_stack(entry_point, stack_ptr);
                                        if (!stack_ptr) {
                                            kfree((void*)kernel_stack_vaddr);
                                            kfree(new_task);
                                            return 0;
                                        }

                                        new_task->kernel_stack = (uintptr_t)stack_ptr;

                                        print("[TASK] Created kernel task '");
                                        print(name);
                                        print("' with ID: ");
                                        print(int_to_string(new_task->id));
                                        print("\n");

                                        print("[TASK] About to initialize task queue...\n");

                                        return new_task;
                                    }

                                    void tasks_init(void) {
                                        ipc_init();

                                        task_t* kernel_task = (task_t*)kmalloc(sizeof(task_t));
                                        if (!kernel_task) {
                                            kernel_panic("Failed to allocate memory for kernel task");
                                        }

                                        memory_set((uint8*)kernel_task, 0, sizeof(task_t));
                                        strncpy(kernel_task->name, "kernel", 31);
                                        kernel_task->name[31] = '\0';
                                        kernel_task->id = next_task_id++;
                                        kernel_task->state = TASK_STATE_RUNNING;

                                        kernel_task->page_directory = vmm_get_current_page_directory();

                                        print("[TASK] Accessing initial_kernel_stack...\n");
                                        kernel_task->kernel_stack_base = (uintptr_t)initial_kernel_stack;
                                        kernel_task->kernel_stack = (uintptr_t)&initial_kernel_stack[KERNEL_STACK_SIZE];
                                        kernel_task->priority = TASK_PRIORITY_NORMAL;
                                        kernel_task->ticks_left = 0;
                                        kernel_task->pending_signals = 0;
                                        kernel_task->last_active_tick = 0;
                                        kernel_task->cpu_ticks_total = 0;
                                        kernel_task->scheduled_at_tick = timer_get_ticks();
                                        kernel_task->created_at_tick = kernel_task->scheduled_at_tick;
                                        kernel_task->next = 0;
                                        kernel_task->next_in_pgrp = 0;
                                        kernel_task->exit_code = 0;
                                        memory_set((uint8*)kernel_task->exit_reason, 0, sizeof(kernel_task->exit_reason));
                                        kernel_task->uid = 0;
                                        kernel_task->gid = 0;
                                        kernel_task->groups_mask = 1;
                                        kernel_task->user_heap_base = 0;
                                        kernel_task->user_heap_limit = 0;
                                        kernel_task->user_brk = 0;
                                        kernel_task->is_background = false;
                                        kernel_task->has_framebuffer_mapping = false;
                                        // Never let this be killed - it's the single boot thread that runs
                                        // all of kmain(); task_kill() refuses protected tasks, and the
                                        // invalid-opcode fault handler relies on that to decide whether it's
                                        // safe to isolate a crash to "just kill the task" vs. a real panic.
                                        kernel_task->is_protected = true;
                                        kernel_task->original_priority = TASK_PRIORITY_NORMAL;
                                        kernel_task->boost_expires_at = 0;
                                        kernel_task->watchdog_enabled = false;
                                        kernel_task->consecutive_timeouts = 0;
                                        kernel_task->sleep_until_tick = 0;
                                        kernel_task->memory_quota = 0;
                                        kernel_task->memory_used = 0;
                                        gdt_set_kernel_stack(kernel_task->kernel_stack);

                                        memory_set((uint8*)&kernel_task->elf_info, 0, sizeof(elf_load_info_t));

                                        // Tasks can be created (via task_create_kernel(), e.g. the early boot
                                        // splash animation thread) before tasks_init() runs, back when
                                        // ready_queue_head was still NULL - that path starts its own
                                        // self-looped 1-node ready "queue". If we blindly overwrite
                                        // ready_queue_head below, any such pre-existing tasks are silently
                                        // orphaned: still alive, but never reachable by the scheduler's
                                        // round-robin walk, so anything that waits on them (e.g.
                                        // splash_stop() waiting for that thread to acknowledge fadeout)
                                        // spins forever and boot never reaches the login prompt. Preserve
                                        // that chain by splicing kernel_task/idle_task into it instead.
                                        task_t* preexisting_ready_queue = ready_queue_head;

                                        current_task = kernel_task;

                                        idle_task = create_kernel_task(idle_task_function, "idle");
                                        if (!idle_task) {
                                            kernel_panic("Failed to create idle task");
                                        }
                                        idle_task->state = TASK_STATE_WAITING;
                                        idle_task->is_protected = true; // same rationale as kernel_task above

                                        if (preexisting_ready_queue) {
                                            task_t* tail = preexisting_ready_queue;
                                            while (tail->next && tail->next != preexisting_ready_queue) {
                                                tail = tail->next;
                                            }
                                            kernel_task->next = idle_task;
                                            idle_task->next = preexisting_ready_queue;
                                            tail->next = kernel_task;
                                            debuglog(DEBUG_INFO,
                                                     "[TASK] tasks_init: preserved pre-existing ready queue starting at task %u\n",
                                                     preexisting_ready_queue->id);
                                        } else {
                                            kernel_task->next = idle_task;
                                            idle_task->next = kernel_task;
                                        }
                                        ready_queue_head = kernel_task;

                                        print("[TASK] Initialized tasking system. Kernel task ID: ");
                                        print(int_to_string(current_task->id));
                                        print(", Idle task ID: ");
                                        print(int_to_string(idle_task->id));
                                        print("\n");

                                        debug_print_ready_queue();
                                    }


                                    // NOTE: this function deliberately does NOT hold task_scheduler_lock across
                                    // the ELF load / page-mapping pipeline below. That lock is a cli/sti spinlock
                                    // (see spinlock_acquire()), so holding it for the whole function used to disable
                                    // ALL interrupts (timer + keyboard) for as long as loading+mapping a user ELF
                                    // took, freezing the entire OS (no scheduler preemption, no TTY input) until it
                                    // finished. The lock is only needed to protect two pieces of truly shared state:
                                    // the next_task_id counter and the ready_queue splice. Everything in between
                                    // (elf_load_executable, user stack/heap mapping, kernel stack setup) works on
                                    // this task's own private page directory and is safe to do with interrupts on.
                                    task_t* task_create_elf(const uint8* elf_data, size_t elf_size, const char* name) {
                                        task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
                                        if (!new_task) {
                                            debuglog(DEBUG_ERROR, "[TASK] kmalloc failed while creating task '%s'\n", name ? name : "(null)");
                                            print_colored("[TASK] Failed to allocate memory for new task: ", 0x0C, 0x00);
                                            print(name);
                                            print("\n");
                                            return 0;
                                        }

                                        memory_set((uint8*)new_task, 0, sizeof(task_t));
                                        strncpy(new_task->name, name, 31);
                                        new_task->name[31] = '\0';
                                        spinlock_acquire(&task_scheduler_lock);
                                        new_task->id = next_task_id++;
                                        spinlock_release(&task_scheduler_lock);
                                        new_task->pgrp = new_task->id;
                                        new_task->session = new_task->id;
                                        new_task->tty_fd = -1;
                                        new_task->vt_index = -1;
                                        new_task->state = TASK_STATE_WAITING;
                                        new_task->priority = TASK_PRIORITY_NORMAL;
                                        new_task->ticks_left = 0;
                                        new_task->cpu_ticks_total = 0;
                                        new_task->scheduled_at_tick = timer_get_ticks();
                                        new_task->created_at_tick = new_task->scheduled_at_tick;
                                        new_task->next = 0;
                                        new_task->next_in_pgrp = 0;
                                        new_task->exit_code = 0;
                                        memory_set((uint8*)new_task->exit_reason, 0, sizeof(new_task->exit_reason));
                                        new_task->uid = auth_active_uid();
                                        new_task->gid = auth_active_gid();
                                        new_task->groups_mask = auth_active_groups_mask();
                                        new_task->last_active_tick = 0;
                                        new_task->user_heap_base = 0;
                                        new_task->user_heap_limit = 0;
                                        new_task->user_brk = 0;
                                        new_task->is_background = false;
                                        new_task->has_framebuffer_mapping = false;
                                        new_task->is_protected = false;
                                        new_task->original_priority = TASK_PRIORITY_NORMAL;
                                        new_task->boost_expires_at = 0;
                                        new_task->watchdog_enabled = false;
                                        new_task->consecutive_timeouts = 0;
                                        new_task->sleep_until_tick = 0;
                                        new_task->memory_quota = 0;
                                        new_task->memory_used = 0;
                                        uint32 heap_base = 0;
                                        uint32 heap_limit = 0;
                                        uint32 initial_heap_end = 0;
                                        bool heap_mapped = false;

                                        // 1. Load ELF into a new page directory
                                        debuglog(DEBUG_INFO, "[TASK] About to call elf_load_executable\n");
                                        elf_load_info_t elf_info;
                                        int status = elf_load_executable(elf_data, elf_size, &elf_info);
                                        debuglog(DEBUG_INFO, "[TASK] After elf_load_executable, status=%d\n", status);
                                        if (status != 0 || !elf_info.valid || elf_info.entry_point == 0) {
                                            debuglog(DEBUG_ERROR, "[TASK] elf_load_executable failed for '%s' (status=%d, valid=%u, entry=0x%x, size=%u)\n",
                                                     name ? name : "(null)", status, elf_info.valid, elf_info.entry_point, (uint32)elf_size);
                                            const char* reason = "unknown";
                                            switch (status) {
                                                case -1: reason = "NULL data or info"; break;
                                                case -2: {
                                                    int ve = elf_validate_header((const elf32_ehdr_t*)elf_data);
                                                    reason = elf_validate_error_string(ve);
                                                    break;
                                                }
                                                case -3: reason = "program header out of bounds"; break;
                                                case -4: reason = "page directory creation failed"; break;
                                                case -5: reason = "segment mapping/copy failed"; break;
                                                case -6: reason = "ELF size out of range"; break;
                                                case -7: reason = "insufficient memory"; break;
                                            }
                                            print_colored("[TASK] Failed to load ELF for task: ", 0x0C, 0x00);
                                            print(name);
                                            print(" (");
                                            print(reason);
                                            print(")\n");
                                            kfree(new_task);
                                            return 0;
                                        }
                                        new_task->elf_info = elf_info;
                                        new_task->page_directory = (page_directory_t*)elf_info.page_directory;
                                        debuglog(DEBUG_INFO, "[TASK] Set page_directory to %p\n", (void*)elf_info.page_directory);

                                        // PDE-DIAG: dump PDE 256 of new_dir at three checkpoints
                                        {
                                            page_entry_t* pde_arr = (page_entry_t*)new_task->page_directory;
                                            debuglog(DEBUG_INFO, "[PDE_DIAG] post-elf:    pde[256] present=%u user=%u rw=%u frame=0x%x\n",
                                                     pde_arr[256].present, pde_arr[256].user, pde_arr[256].writable, (uint32)pde_arr[256].frame);
                                        }

                                        // Sync kernel PDEs to ensure task has access to all kernel resources
                                        vmm_sync_kernel_pdes(new_task->page_directory);

                                        {
                                            page_entry_t* pde_arr = (page_entry_t*)new_task->page_directory;
                                            debuglog(DEBUG_INFO, "[PDE_DIAG] post-sync:   pde[256] present=%u user=%u rw=%u frame=0x%x\n",
                                                     pde_arr[256].present, pde_arr[256].user, pde_arr[256].writable, (uint32)pde_arr[256].frame);
                                        }

                                        // 2. Allocate and map user stack for the new task
                                        // vmm_map_page takes the target directory as a parameter, so we don't need to
                                        // switch page directories. The kernel stays in its own address space.
                                        page_directory_t* task_pd = (page_directory_t*)elf_info.page_directory;

                                        debuglog(DEBUG_INFO, "[TASK] Allocating user stack for '%s' (%d pages), pd=0x%x\n", name ? name : "(null)", USER_STACK_SIZE, (uint32)task_pd);

                                        for (int i = 0; i < USER_STACK_SIZE; i++) {
                                            uint32 p_addr = pmm_alloc_frame();
                                            if (!p_addr) {
                                                debuglog(DEBUG_ERROR, "[TASK] User stack frame allocation failed for '%s' at page %d\n",
                                                         name ? name : "(null)", i);
                                                print_colored("[TASK] Failed to allocate user stack frame for task: ", 0x0C, 0x00);
                                                print(name);
                                                print("\n");
                                                goto stack_fail;
                                            }

                                            uint32 stack_va = USER_STACK_TOP - (i + 1) * MEMORY_PAGE_SIZE;
                                            memory_result_t map_res = vmm_map_page(task_pd, stack_va, p_addr,
                                                                                   PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE);
                                            if (map_res != MEMORY_OK && map_res != MEMORY_ERROR_ALREADY_MAPPED) {
                                                debuglog(DEBUG_ERROR, "[TASK] Failed to map user stack page %d for '%s' (res=%d, va=0x%x)\n",
                                                         i, name ? name : "(null)", map_res, stack_va);
                                                print_colored("[TASK] Failed to map user stack page for task: ", 0x0C, 0x00);
                                                print(name);
                                                print("\n");
                                                pmm_free_frame(p_addr);
                                                goto stack_fail;
                                            }

                                            // Zero the stack frame - using temporary mapping to access the physical address
                                            // Since PMM may allocate frames above identity mapping limit, we need proper mapping
                                            // For now, skip zeroing as the pages will be zero when allocated from PMM anyway
                                            debuglog(DEBUG_INFO, "[TASK] vmm_zero_phys skipped - PMM should return zeroed pages\n");

                                        }
                                        debuglog(DEBUG_INFO, "[TASK] User stack mapped successfully\n");

                                        // Establish a per-task heap just below the user stack with a small guard.
                                        uint32 stack_base = USER_STACK_TOP - (USER_STACK_SIZE * MEMORY_PAGE_SIZE);
                                        uint32 guard_bytes = USER_HEAP_GUARD_PAGES * MEMORY_PAGE_SIZE;
                                        heap_base = memory_align_up(elf_info.base_address + elf_info.total_size, MEMORY_PAGE_SIZE);
                                        if (heap_base < MEMORY_USER_START) {
                                            heap_base = MEMORY_USER_START;
                                        }
                                        heap_limit = (stack_base > guard_bytes) ? (stack_base - guard_bytes) : stack_base;
                                        if (heap_base >= heap_limit) {
                                            debuglog(DEBUG_ERROR, "[TASK] Heap range overlaps stack for '%s' (heap_base=0x%x, limit=0x%x)\n",
                                                     name ? name : "(null)", heap_base, heap_limit);
                                            print_colored("[TASK] Failed to reserve heap range for task\n", 0x0C, 0x00);
                                            goto stack_fail;
                                        }

                                        initial_heap_end = heap_base + MEMORY_PAGE_SIZE;
                                        if (initial_heap_end > heap_limit) {
                                            initial_heap_end = heap_limit;
                                        }

                                        if (!task_map_user_pages(task_pd,
                                            heap_base,
                                            initial_heap_end,
                                            PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)) {
                                            debuglog(DEBUG_ERROR, "[TASK] Failed to map initial heap page for '%s'\n", name ? name : "(null)");
                                        print_colored("[TASK] Failed to map initial heap page\n", 0x0C, 0x00);
                                        goto stack_fail;
                                            }
                                            heap_mapped = true;
                                            debuglog(DEBUG_INFO, "[TASK] User heap mapped: 0x%x - 0x%x\n", heap_base, initial_heap_end);

                                            // No need to switch page directories - we stayed in kernel space
                                            new_task->user_heap_base = heap_base;
                                            new_task->user_heap_limit = heap_limit;
                                            new_task->user_brk = heap_base;

                                            // 3. Allocate a kernel stack for the new task
                                            // We need 2 pages for the kernel stack (8KB)
                                            uintptr_t kernel_stack_vaddr = (uintptr_t)kmalloc_aligned(KERNEL_STACK_SIZE, MEMORY_PAGE_SIZE); // Allocate 8KB aligned
                                            if (!kernel_stack_vaddr) {
                                                debuglog(DEBUG_ERROR, "[TASK] Kernel stack allocation failed for '%s'\n", name ? name : "(null)");
                                                print_colored("[TASK] Failed to allocate kernel stack for task: ", 0x0C, 0x00);
                                                print(name);
                                                print("\n");
                                                goto stack_fail;
                                            }
                                            new_task->kernel_stack_base = kernel_stack_vaddr; // Store the base address

                                            // The stack grows downwards, so the "top" is the highest address
                                            uintptr_t kernel_stack_top = kernel_stack_vaddr + KERNEL_STACK_SIZE;

                                            // Ensure this task's page directory can see its kernel stack.
                                            // The stack is allocated in the kernel heap while the current CR3 is
                                            // still the kernel task's directory. We must mirror those mappings into
                                            // the new task CR3 before the first context switch.
                                            page_directory_t* kernel_pd = vmm_get_current_page_directory();
                                            uint32 stack_map_start = memory_align_down((uint32)kernel_stack_vaddr, MEMORY_PAGE_SIZE);
                                            uint32 stack_map_end = memory_align_up((uint32)(kernel_stack_vaddr + KERNEL_STACK_SIZE), MEMORY_PAGE_SIZE);
                                            for (uint32 va = stack_map_start; va < stack_map_end; va += MEMORY_PAGE_SIZE) {
                                                uint32 pa = vmm_get_physical_addr(kernel_pd, va);
                                                if (!pa) {
                                                    debuglog(DEBUG_ERROR, "[TASK] Kernel stack page not mapped in kernel PD: va=0x%x\n", va);
                                                    print_colored("[TASK] Failed to resolve kernel stack page mapping\n", 0x0C, 0x00);
                                                    kfree((void*)kernel_stack_vaddr);
                                                    goto stack_fail;
                                                }

                                                memory_result_t ks_res = vmm_map_page(task_pd, va, pa,
                                                                                      PAGE_PRESENT | PAGE_WRITABLE);
                                                if (ks_res != MEMORY_OK && ks_res != MEMORY_ERROR_ALREADY_MAPPED) {
                                                    debuglog(DEBUG_ERROR, "[TASK] Failed to map kernel stack into task PD: va=0x%x pa=0x%x res=%d\n",
                                                             va, pa, ks_res);
                                                    print_colored("[TASK] Failed to map kernel stack into task address space\n", 0x0C, 0x00);
                                                    kfree((void*)kernel_stack_vaddr);
                                                    goto stack_fail;
                                                }
                                            }

                                            // 3. Set up for initial user-mode entry
                                            // Build the complete kernel stack frame so task_switch_asm can:
                                            //   popa, popf, pop ebp, ret -> jumps to task_start_usermode_asm
                                            //   task_start_usermode_asm does IRET -> drops to ring 3
                                            //
                                            // This pre-built frame includes:
                                            //   - IRET frame (SS, ESP, EFLAGS, CS, EIP for user mode)
                                            //   - task_switch_asm frame (return addr, EBP, EFLAGS, 8 GPRs for POPA)
                                            //
                                            // NOTE: Initial ESP is set to USER_STACK_TOP - 4 because the stack
                                            // pages are mapped from USER_STACK_TOP - 32*4096 to USER_STACK_TOP - 4096.
                                            // The page at USER_STACK_TOP itself is NOT mapped. Starting ESP at
                                            // USER_STACK_TOP would cause an immediate page fault on the first push.
                                            setup_initial_cpu_state(new_task,
                                                                    elf_info.entry_point,
                                                                    USER_STACK_TOP - 4,
                                                                    kernel_stack_top);

                                            debuglog(DEBUG_INFO, "[TASK] setup_initial_cpu_state done: kernel_stack=0x%x\n", (uint32)new_task->kernel_stack);

                                            // Store user mode info for debugging purposes
                                            new_task->needs_usermode_entry = true;
                                            new_task->usermode_entry_point = elf_info.entry_point;
                                            // NOTE: usermode_stack_top must match the value passed to setup_initial_cpu_state
                                            // (USER_STACK_TOP - 4), otherwise frame validation/rebuild will use wrong ESP
                                            new_task->usermode_stack_top = USER_STACK_TOP - 4;

                                            debuglog(DEBUG_INFO, "[TASK] ELF entry point: 0x%x\n", elf_info.entry_point);


                                            // 4. Add the new task to the ready queue.
                                            // Brief, bounded critical section (shared list splice only) -
                                            // not the multi-step load/mapping work above.
                                            spinlock_acquire(&task_scheduler_lock);
                                            if (ready_queue_head == 0) {
                                                ready_queue_head = new_task;
                                                new_task->next = new_task; // Point to itself for a single-element circular list
                                            } else {
                                                // Find the tail of the circular list
                                                task_t* head = ready_queue_head;
                                                while (head->next != ready_queue_head) {
                                                    head = head->next;
                                                }
                                                head->next = new_task;
                                                new_task->next = ready_queue_head;
                                            }
                                            spinlock_release(&task_scheduler_lock);

                                            debuglog(DEBUG_INFO, "[TASK] Created task ID: %u (%s) ELF entry: 0x%x, kernel_stack: 0x%x, page_dir: 0x%x\n",
                                                     new_task->id, name, new_task->elf_info.entry_point,
                                                     new_task->kernel_stack, (uint32)new_task->page_directory);

                                            // Skip debug information about entry point bytes for now as it may cause page faults
                                            // TODO: Add safe memory reading function for debug output

                                            return new_task;

                                            stack_fail:
                                            // No need to switch page directories - we stayed in kernel space
                                            // Clean up allocated resources
                                            if (heap_mapped) {
                                                task_unmap_user_pages(task_pd, heap_base, initial_heap_end);
                                            }
                                            vmm_destroy_page_directory(task_pd);
                                            kfree(new_task);
                                            return 0;
                                    }

                                    // Create a kernel-level task that runs a function in kernel space
                                    task_t* task_create_kernel(void (*entry_point)(void), const char* name, uint32 stack_size) {
                                        if (!entry_point || !name) {
                                            return 0;
                                        }

                                        spinlock_acquire(&task_scheduler_lock);

                                        task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
                                        if (!new_task) {
                                            debuglog(DEBUG_ERROR, "[TASK] kmalloc failed while creating kernel task '%s'\n", name);
                                            spinlock_release(&task_scheduler_lock);
                                            return 0;
                                        }

                                        // Initialize task structure
                                        memory_set((uint8*)new_task, 0, sizeof(task_t));
                                        strncpy(new_task->name, name, 31);
                                        new_task->name[31] = '\0';
                                        new_task->id = next_task_id++;
                                        new_task->pgrp = new_task->id;
                                        new_task->session = new_task->id;
                                        new_task->tty_fd = -1;
                                        new_task->vt_index = -1;
                                        new_task->state = TASK_STATE_READY;
                                        new_task->priority = TASK_PRIORITY_NORMAL;
                                        new_task->ticks_left = 0;
                                        new_task->cpu_ticks_total = 0;
                                        new_task->scheduled_at_tick = timer_get_ticks();
                                        new_task->created_at_tick = new_task->scheduled_at_tick;
                                        new_task->exit_code = 0;
                                        memory_set((uint8*)new_task->exit_reason, 0, sizeof(new_task->exit_reason));
                                        new_task->uid = 0;
                                        new_task->gid = 0;
                                        new_task->groups_mask = 0;
                                        new_task->last_active_tick = 0;
                                        new_task->is_background = false;
                                        new_task->has_framebuffer_mapping = false;
                                        new_task->is_graphics_task = false;
                                        new_task->is_protected = false;
                                        new_task->original_priority = TASK_PRIORITY_NORMAL;
                                        new_task->boost_expires_at = 0;
                                        new_task->watchdog_enabled = false;
                                        new_task->consecutive_timeouts = 0;
                                        new_task->sleep_until_tick = 0;
                                        new_task->memory_quota = 0;
                                        new_task->memory_used = 0;
                                        new_task->next_in_pgrp = 0;

                                        // Use kernel page directory
                                        new_task->page_directory = vmm_get_current_page_directory();

                                        // Allocate kernel stack.
                                        // Scheduler/context-switch paths assume KERNEL_STACK_SIZE when setting TSS ESP0
                                        // and validating current-context stack boundaries, so force kernel tasks onto
                                        // the canonical size to prevent frame corruption from undersized stacks.
                                        if (stack_size != KERNEL_STACK_SIZE) {
                                            debuglog(DEBUG_WARN,
                                                     "[TASK] task_create_kernel('%s'): forcing stack_size %u -> %u\n",
                                                     name, stack_size, KERNEL_STACK_SIZE);
                                            stack_size = KERNEL_STACK_SIZE;
                                        }
                                        void* kernel_stack = kmalloc_aligned(stack_size, MEMORY_PAGE_SIZE);
                                        if (!kernel_stack) {
                                            debuglog(DEBUG_ERROR, "[TASK] Failed to allocate kernel stack for task '%s'\n", name);
                                            kfree(new_task);
                                            spinlock_release(&task_scheduler_lock);
                                            return 0;
                                        }

                                        new_task->kernel_stack_base = (uintptr_t)kernel_stack;

                                        // Set up kernel stack for entry point - CRITICAL: must call prepare_kernel_task_stack!
                                        // The stack frame must be properly prepared for task_switch_asm to work correctly.
                                        stack_word_t* stack_top = (stack_word_t*)((uintptr_t)kernel_stack + stack_size);
                                        stack_word_t* stack_ptr = prepare_kernel_task_stack(entry_point, stack_top);
                                        if (!stack_ptr) {
                                            debuglog(DEBUG_ERROR, "[TASK] Failed to prepare kernel stack for task '%s'\n", name);
                                            kfree(kernel_stack);
                                            kfree(new_task);
                                            spinlock_release(&task_scheduler_lock);
                                            return 0;
                                        }

                                        new_task->kernel_stack = (uintptr_t)stack_ptr;

                                        // Store entry point info for debugging purposes
                                        new_task->elf_info.entry_point = (uint32)(uintptr_t)entry_point;
                                        new_task->elf_info.valid = true;

                                        // Add to ready queue
                                        if (ready_queue_head == 0) {
                                            ready_queue_head = new_task;
                                            new_task->next = new_task; // Point to itself for a single-element circular list
                                        } else {
                                            // Find the tail of the circular list
                                            task_t* head = ready_queue_head;
                                            while (head->next != ready_queue_head) {
                                                head = head->next;
                                            }
                                            head->next = new_task;
                                            new_task->next = ready_queue_head;
                                        }

                                        print("[TASK] Created kernel task ID: ");
                                        print(int_to_string(new_task->id));
                                        print(" (");
                                        print(name);
                                        print(") entry: 0x");
                                        print_hex((uint32)entry_point);
                                        print("\n");

                                        spinlock_release(&task_scheduler_lock);
                                        return new_task;
                                    }

                                    /**
                                     * Clone the current task (for fork syscall)
                                     * Creates a child process that is a copy of the current process
                                     */
                                    task_t* task_clone_current(void) {
                                        spinlock_acquire(&task_scheduler_lock);

                                        if (!current_task) {
                                            spinlock_release(&task_scheduler_lock);
                                            return NULL;
                                        }

                                        // Allocate new task structure
                                        task_t* child_task = (task_t*)kmalloc(sizeof(task_t));
                                        if (!child_task) {
                                            debuglog(DEBUG_ERROR, "[TASK] kmalloc failed while cloning task '%s'\n",
                                                     current_task->name);
                                            spinlock_release(&task_scheduler_lock);
                                            return NULL;
                                        }

                                        // Copy task structure from parent
                                        memory_copy((const char*)current_task, (char*)child_task, sizeof(task_t));

                                        // Set child-specific fields
                                        child_task->id = next_task_id++;
                                        child_task->state = TASK_STATE_READY;
                                        child_task->next = NULL;
                                        child_task->next_in_pgrp = NULL;
                                        // Parent's wait-node (if any) still points at the parent task_t,
                                        // not this clone -- never inherit that back-pointer verbatim.
                                        child_task->waiting_semaphore = NULL;
                                        // Likewise never inherit the parent's thread-wrapper back-pointer;
                                        // a forked child is its own task, not a clone of the parent's
                                        // thread_create() wrapper.
                                        child_task->thread_wrapper = NULL;
                                        child_task->pgrp = current_task->pgrp;
                                        child_task->session = current_task->session;
                                        child_task->parent_pid = current_task->id;
                                        child_task->is_background = current_task->is_background;
                                        child_task->has_framebuffer_mapping = false;
                                        child_task->is_graphics_task = false;
                                        child_task->is_protected = false;
                                        child_task->priority = current_task->priority;
                                        child_task->original_priority = current_task->original_priority;
                                        child_task->boost_expires_at = 0;
                                        child_task->memory_quota = current_task->memory_quota;
                                        child_task->memory_used = 0;

                                        // Copy name with " (child)" suffix
                                        char child_name[32];
                                        memory_copy((const char*)current_task->name, child_name, 32);
                                        strncat(child_name, " (child)", 32 - strlen(child_name) - 1);
                                        strncpy(child_task->name, child_name, 31);
                                        child_task->name[31] = '\0';

                                        // Allocate new kernel stack for the child
                                        void* kernel_stack = kmalloc_aligned(KERNEL_STACK_SIZE, MEMORY_PAGE_SIZE);
                                        if (!kernel_stack) {
                                            debuglog(DEBUG_ERROR, "[TASK] Failed to allocate kernel stack for cloned task '%s'\n",
                                                     child_task->name);
                                            kfree(child_task);
                                            spinlock_release(&task_scheduler_lock);
                                            return NULL;
                                        }

                                        child_task->kernel_stack_base = (uintptr_t)kernel_stack;

                                        // Copy the current task's kernel stack. Every task's kernel stack
                                        // starts fresh at kernel_stack_base + KERNEL_STACK_SIZE on each
                                        // syscall entry (see isr128), so this copy carries over the live
                                        // syscall_frame_t + IRET frame for this in-flight fork()/clone()
                                        // call intact, at the same fixed offset from the top in both copies.
                                        memory_copy((const char*)current_task->kernel_stack_base, (char*)kernel_stack, KERNEL_STACK_SIZE);

                                        // Force the child's copy of the syscall return value (EAX) to 0,
                                        // matching fork()'s contract: child sees return value 0.
                                        // isr128 layout (top = kernel_stack_base + KERNEL_STACK_SIZE):
                                        //   [top-20, top)      CPU IRET frame (eip/cs/eflags/esp/ss)
                                        //   [top-36, top-20)   ds/es/fs/gs pushed by isr128
                                        //   [top-68, top-36)   syscall_frame_t (pusha block)
                                        syscall_frame_t* child_syscall_frame = (syscall_frame_t*)
                                            ((uintptr_t)kernel_stack + KERNEL_STACK_SIZE - sizeof(syscall_frame_t) - 36);
                                        child_syscall_frame->eax = 0;

                                        // Build a task_switch_asm resume frame (popa/popf/pop ebp/ret)
                                        // that lands directly on isr128_resume, right where the child's
                                        // own syscall_frame_t begins. The scheduler then resumes the child
                                        // exactly like a normal syscall return to userspace: same EIP/CS/
                                        // EFLAGS/ESP/SS the parent had at the fork() call site, eax=0.
                                        child_task->kernel_stack = (uintptr_t)
                                            prepare_kernel_task_stack(isr128_resume, (stack_word_t*)child_syscall_frame);

                                        // Give the child its own (COW-protected) address space rather than
                                        // sharing the parent's page directory outright. Plain sharing let the
                                        // parent's post-fork execution silently overwrite the shared user stack
                                        // (return addresses, locals) before the child ever ran, corrupting it --
                                        // see mm_cow_impl.c's cow_fork_address_space() for the fault-triggered
                                        // copy that now protects both sides. Falls back to plain sharing only if
                                        // COW setup itself fails (e.g. OOM), matching the prior (buggy) behavior
                                        // rather than failing the fork outright.
                                        page_directory_t* child_pd = cow_fork_address_space(current_task->page_directory);
                                        child_task->page_directory = child_pd ? child_pd : current_task->page_directory;

                                        // Reset child-specific fields
                                        child_task->exit_code = 0;
                                        memory_set((uint8*)child_task->exit_reason, 0, sizeof(child_task->exit_reason));
                                        child_task->pending_signals = 0;
                                        child_task->sleep_until_tick = 0;
                                        child_task->last_active_tick = 0;
                                        child_task->cpu_ticks_total = 0;
                                        child_task->scheduled_at_tick = timer_get_ticks();
                                        child_task->created_at_tick = child_task->scheduled_at_tick;
                                        child_task->needs_usermode_entry = false; // Child starts as if already in usermode

                                        // Add child to ready queue
                                        if (ready_queue_head == 0) {
                                            ready_queue_head = child_task;
                                            child_task->next = child_task; // Point to itself for a single-element circular list
                                        } else {
                                            // Find the tail of the circular list
                                            task_t* head = ready_queue_head;
                                            while (head->next != ready_queue_head) {
                                                head = head->next;
                                            }
                                            head->next = child_task;
                                            child_task->next = ready_queue_head;
                                        }

                                        debuglog(DEBUG_INFO, "[TASK] Cloned task '%s' (PID %u) -> child '%s' (PID %u)\n",
                                                 current_task->name, current_task->id, child_task->name, child_task->id);

                                        spinlock_release(&task_scheduler_lock);
                                        return child_task;
                                    }

                                    void task_switch(task_t* next_task) {
                                        uint32_t eflags_before = 0;
                                        bool irq_guard_acquired = false;
#if ARCH_64BIT
                                        __asm__ __volatile__("pushfq; popq %0" : "=r"(eflags_before));
#else
                                        __asm__ __volatile__("pushf; pop %0" : "=r"(eflags_before));
#endif
                                        if (eflags_before & 0x200u) {
                                            __asm__ __volatile__("cli");
                                            irq_guard_acquired = true;
                                        }

                                        if (!next_task) {
                                            print("[TASK] ERROR: Attempted to switch to null task\n");
                                            if (irq_guard_acquired) {
                                                __asm__ __volatile__("sti");
                                            }
                                            return;
                                        }

                                        if (!current_task || current_task == next_task) {
                                            if (irq_guard_acquired) {
                                                __asm__ __volatile__("sti");
                                            }
                                            return; // No switch needed or current_task is null (first switch)
                                        }

                                        if (!next_task->page_directory) {
                                            print("[TASK] ERROR: Next task has null page directory\n");
                                            if (irq_guard_acquired) {
                                                __asm__ __volatile__("sti");
                                            }
                                            return;
                                        }

                                        if (next_task->kernel_stack == 0) {
                                            print("[TASK] ERROR: Next task has invalid kernel stack\n");
                                            if (irq_guard_acquired) {
                                                __asm__ __volatile__("sti");
                                            }
                                            return;
                                        }

                                        task_t* prev_task = current_task;

                                        // Real CPU-time accounting: credit prev_task with the ticks it just
                                        // spent running (since it was last switched in), then mark next_task
                                        // as switched-in as of now. Feeds task_info_t.cpu_ticks_total for
                                        // ps/top/htop %CPU columns.
                                        uint32 switch_now_tick = timer_get_ticks();
                                        prev_task->cpu_ticks_total += (uint64)(switch_now_tick - prev_task->scheduled_at_tick);
                                        next_task->scheduled_at_tick = switch_now_tick;

                                        if (next_task->needs_usermode_entry) {
                                            bool has_valid_user_entry =
                                                (next_task->usermode_entry_point >= MEMORY_USER_START) &&
                                                (next_task->usermode_entry_point < USER_STACK_TOP) &&
                                                (next_task->usermode_stack_top > MEMORY_USER_START) &&
                                                (next_task->usermode_stack_top <= USER_STACK_TOP);

                                            if (!has_valid_user_entry) {
                                                debuglog(DEBUG_WARN,
                                                         "[TASK] Clearing spurious needs_usermode_entry on task=%u name='%s' entry=0x%x stack=0x%x\n",
                                                         next_task->id,
                                                         next_task->name,
                                                         (uint32)next_task->usermode_entry_point,
                                                         (uint32)next_task->usermode_stack_top);
                                                next_task->needs_usermode_entry = false;
                                            } else {
                                            print("[TASK] First usermode entry: entry=0x");
                                            print_hex((uint32_t)next_task->usermode_entry_point);
                                            print(" stack=0x");
                                            print_hex((uint32_t)next_task->usermode_stack_top);
                                            print("\n");

                                            if (!validate_initial_usermode_frame(next_task)) {
                                                debuglog(DEBUG_WARN,
                                                         "[TASK] Rebuilding initial switch frame for task=%u (stack_base=0x%x)\n",
                                                         next_task->id,
                                                         (uint32)next_task->kernel_stack_base);

                                                setup_initial_cpu_state(next_task,
                                                                        next_task->usermode_entry_point,
                                                                        next_task->usermode_stack_top,
                                                                        next_task->kernel_stack_base + KERNEL_STACK_SIZE);

                                                if (!validate_initial_usermode_frame(next_task)) {
                                                    kernel_panic_annotated("TASK SWITCH FRAME CORRUPTION",
                                                                          __FILE__, __LINE__, __func__);
                                                }
                                            }
                                            next_task->needs_usermode_entry = false;
                                            }
                                        }

                                        // Sync any kernel PDEs that were created after this task's page directory
                                        // was originally built (e.g. new kernel heap page tables).  The new task's
                                        // PD is a shallow copy — it shares existing page tables but misses any PDE
                                        // that was added later.  This one-pass sync makes those visible.
                                        if (next_task->page_directory != prev_task->page_directory) {
                                            vmm_sync_kernel_pdes(next_task->page_directory);
                                        }

                                        // Conservative repair path: ensure kernel stack pages are visible in the
                                        // next task's address space before we hand control to assembly.
                                        page_directory_t* source_pd = prev_task->page_directory;
                                        if (!source_pd) {
                                            source_pd = vmm_get_current_page_directory();
                                        }
                                        if (!task_ensure_kernel_stack_mapped(next_task, source_pd)) {
                                            kernel_panic_annotated("TASK SWITCH: failed to repair kernel stack mapping",
                                                                  __FILE__, __LINE__, __func__);
                                        }

                                        // Hard guard: the incoming task's saved kernel ESP must be mapped in its
                                        // own page directory before we load ESP and potentially switch CR3.
                                        if (!vmm_is_mapped(next_task->page_directory, (uint32)next_task->kernel_stack)) {
                                            debuglog(DEBUG_ERROR, "[TASK] Next task kernel stack not mapped in target PD: task=%u esp=0x%x pd=0x%x\n",
                                                     next_task->id, (uint32)next_task->kernel_stack, (uint32)next_task->page_directory);
                                            kernel_panic_annotated("TASK SWITCH: unmapped kernel stack in target CR3",
                                                                  __FILE__, __LINE__, __func__);
                                        }

                                        // For brand-new user tasks, verify the full trampoline path before
                                        // switching CR3 so we fail with a clear panic instead of triple-faulting.
                                        if (next_task->usermode_entry_point &&
                                            next_task->usermode_stack_top &&
                                            next_task->usermode_entry_point >= MEMORY_USER_START) {
                                            uint32 entry_va = memory_align_down((uint32)next_task->usermode_entry_point, MEMORY_PAGE_SIZE);
                                            uint32 user_sp_check = memory_align_down(((uint32)next_task->usermode_stack_top - sizeof(uint32)),
                                                                                     MEMORY_PAGE_SIZE);
                                            uint32 trampoline_va = memory_align_down((uint32)(uintptr_t)task_start_usermode_asm,
                                                                                     MEMORY_PAGE_SIZE);

                                            if (!vmm_is_mapped(next_task->page_directory, trampoline_va)) {
                                                debuglog(DEBUG_ERROR,
                                                         "[TASK] Trampoline not mapped in target PD: task=%u va=0x%x pd=0x%x\n",
                                                         next_task->id, trampoline_va, (uint32)next_task->page_directory);
                                                kernel_panic_annotated("TASK SWITCH: unmapped task_start_usermode_asm in target CR3",
                                                                      __FILE__, __LINE__, __func__);
                                            }
                                            if (!vmm_is_mapped(next_task->page_directory, entry_va)) {
                                                page_entry_t* pde_arr_dbg = (page_entry_t*)next_task->page_directory;
                                                uint32 pdi = entry_va / (4 * 1024 * 1024);
                                                debuglog(DEBUG_ERROR,
                                                         "[PDE_DIAG] switch-fail: pde[%u] present=%u user=%u rw=%u frame=0x%x (pd=0x%x)\n",
                                                         pdi, pde_arr_dbg[pdi].present, pde_arr_dbg[pdi].user, pde_arr_dbg[pdi].writable, (uint32)pde_arr_dbg[pdi].frame, (uint32)next_task->page_directory);
                                                debuglog(DEBUG_ERROR,
                                                         "[TASK] User entry not mapped in target PD: task=%u eip=0x%x pd=0x%x\n",
                                                         next_task->id, entry_va, (uint32)next_task->page_directory);
                                                kernel_panic_annotated("TASK SWITCH: unmapped user entry in target CR3",
                                                                      __FILE__, __LINE__, __func__);
                                            }
                                            if (!vmm_is_mapped(next_task->page_directory, user_sp_check)) {
                                                debuglog(DEBUG_ERROR,
                                                         "[TASK] User stack top not mapped in target PD: task=%u esp=0x%x pd=0x%x\n",
                                                         next_task->id, user_sp_check, (uint32)next_task->page_directory);
                                                kernel_panic_annotated("TASK SWITCH: unmapped user stack in target CR3",
                                                                      __FILE__, __LINE__, __func__);
                                            }
                                        }

                                        current_task = next_task;
                                        gdt_set_kernel_stack(next_task->kernel_stack_base + KERNEL_STACK_SIZE);

                                        // Perform the actual context switch
                                        // - Save prev_task's kernel ESP into &prev_task->kernel_stack
                                        // - Load next_task's kernel ESP from next_task->kernel_stack
                                        // - Switch to next_task->page_directory
                                        // - Restore registers and return (which for new tasks jumps to task_start_usermode_asm)

                                        // Debug: Log PDE info for the new task's kernel stack.
                                        // IMPORTANT: pde->frame is a *physical* frame number. Do NOT dereference
                                        // (pde->frame << PAGE_SHIFT) as a virtual pointer — once paging is on,
                                        // physical addresses are only accessible via their mapped virtual aliases.
                                        // Dereferencing a physical address directly will page-fault if that frame
                                        // is not identity-mapped (frames above 0x08000000 are not in Fern).
                                        (void)next_task;

                                        // NOTE: page_directory_t* in Fern IS the physical address
                                        // (vmm_create_page_directory returns phys frame ptr directly).
                                        // There is no vmm_pdir_phys() — the pointer IS the CR3 value.
                                        uintptr_t next_cr3 = (uintptr_t)next_task->page_directory;

                                        // Keep VMM software state in sync so page fault handlers
                                        // and temp-mapping helpers operate on the correct directory.
                                        vmm_set_current_directory(next_task->page_directory);

                                        task_switch_asm(&prev_task->kernel_stack,
                                                        next_task->kernel_stack,
                                                        next_cr3);

                                        // Note: For returning tasks, we reach here after they're switched back.
                                        // For new usermode tasks, we never return here - IRET jumps to userspace.
                                        //
                                        // Restore IF only when this invocation explicitly disabled interrupts at
                                        // entry. For IRQ-driven preemption (IF already 0 on entry), forcing STI
                                        // here can re-enter the scheduler before the interrupt frame unwinds.
                                        if (irq_guard_acquired) {
                                            __asm__ __volatile__("sti");
                                        }
                                    }

                                    // Helper function to validate the ready queue integrity
static bool validate_ready_queue(void) {
                                        if (!task_sanitize_ready_queue_locked()) {
                                            return false;
                                        }

                                        if (!ready_queue_head) {
                                            return true; // Empty queue is valid
                                        }

                                        task_t* current = ready_queue_head;
                                        int count = 0;
                                        do {
                                            if (!task_ptr_plausible(current)) {
                                                print("[TASK] ERROR: NULL pointer in ready queue\n");
                                                return false;
                                            }
                                            count++;
                                            if (count > 1000) { // Prevent infinite loops
                                                print("[TASK] ERROR: Ready queue appears to have infinite loop\n");
                                                return false;
                                            }
                                            current = current->next;
                                        } while (current != ready_queue_head);

                                        return true;
                                    }

                                    // Helper function to count valid runnable tasks
                                    static int count_runnable_tasks(void) {
                                        if (!ready_queue_head) return 0;

                                        int count = 0;
                                        task_t* current = ready_queue_head;
                                        do {
                                            if (current && current->state == TASK_STATE_READY) {
                                                count++;
                                            }
                                            current = current->next;
                                        } while (current && current != ready_queue_head);

                                        return count;
                                    }

                                    bool task_exists(uint32 pid) {
                                        if (!ready_queue_head) {
                                            return false;
                                        }
                                        task_t* current = ready_queue_head;
                                        do {
                                            if (current && current->id == pid) {
                                                return true;
                                            }
                                            current = current->next;
                                        } while (current && current != ready_queue_head);
                                        return false;
                                    }

                                    int32 task_get_exit_code(uint32 pid) {
                                        task_t* task = NULL;
                                        task_t* current = ready_queue_head;
                                        if (current) {
                                            do {
                                                if (current->id == pid) {
                                                    task = current;
                                                    break;
                                                }
                                                current = current->next;
                                            } while (current != ready_queue_head);
                                        }
                                        if (task && (task->state == TASK_STATE_TERMINATED || task->state == TASK_STATE_ZOMBIE)) {
                                            return task->exit_code;
                                        }
                                        return -1; // Not terminated or not found
                                    }

                                    int32 task_wait_pid(uint32 pid) {
                                        while (task_exists(pid)) {
                                            // Read the exit code and reap the child in one locked step
                                            // (task_reap_child()) instead of a separate unlocked
                                            // exists+get_exit_code peek followed by relying on some other
                                            // task_reap_zombies() sweep to eventually destroy it: that TOCTOU
                                            // let the child get destroyed (by its own exit path, or another
                                            // schedule() call) between the two reads, so this loop would see
                                            // it vanish and return -1/ECHILD instead of the real exit code.
                                            int32 exit_code = task_reap_child(pid);
                                            if (exit_code != -1) {
                                                return exit_code;
                                            }
                                            // Mark ourselves as not-runnable before yielding. The scheduler's
                                            // foreground-priority check treats READY/RUNNING as "still wants
                                            // the CPU" and would otherwise keep re-picking this (foreground)
                                            // task forever, starving the child we're waiting on.
                                            if (current_task) {
                                                current_task->state = TASK_STATE_WAITING;
                                            }
                                            task_schedule();
                                            if (current_task) {
                                                current_task->state = TASK_STATE_RUNNING;
                                            }
                                            // Explicit sti before hlt: task_schedule()/task_switch() restore IF
                                            // via local variables (irq_guard_acquired / restore_irq) frozen into
                                            // this task's suspended stack frame at the moment it first yielded
                                            // here. Those locals are seeded from spinlock_t.saved_flags, a
                                            // single shared field on task_scheduler_lock -- while this task sits
                                            // suspended (potentially through many other tasks' and the timer
                                            // IRQ's own acquire/release cycles on that same lock), that field
                                            // gets repeatedly overwritten by whoever else last acquired it, and
                                            // by the time this task's own frozen locals are consulted again
                                            // during unwind they no longer reliably reflect "were interrupts on
                                            // when I yielded". Observed in practice: IF ends up clear here, so
                                            // hlt parks the CPU forever (no maskable interrupt, i.e. no timer
                                            // tick, can ever fire to wake it) -- the shell hangs on its very
                                            // first `wait()`. sti+hlt is the standard atomic idiom (the
                                            // documented one-instruction STI delay guarantees no wakeup is
                                            // missed between the two), and makes this loop's wakeup correct
                                            // regardless of what the frozen IF-restore locals decided.
                                            __asm__ __volatile__("sti; hlt");
                                        }
                                        return -1; // Task not found
                                    }

                                    uint32 task_get_last_active_tick(uint32 pid) {
                                        uint32 tick = 0;
                                        spinlock_acquire(&task_scheduler_lock);

                                        if (ready_queue_head) {
                                            task_t* current = ready_queue_head;
                                            do {
                                                if (current && current->id == pid) {
                                                    tick = current->last_active_tick;
                                                    break;
                                                }
                                                current = current->next;
                                            } while (current && current != ready_queue_head);
                                        }

                                        spinlock_release(&task_scheduler_lock);
                                        return tick;
                                    }

                                    void task_mark_active(void) {
                                        if (!current_task) {
                                            return;
                                        }
                                        current_task->last_active_tick = timer_get_ticks();
                                    }

                                    // ============================================================================
                                    // Foreground Task API - Priority scheduling for GUI applications
                                    // ============================================================================

                                    void task_set_foreground(task_t* task) {
                                        spinlock_acquire(&task_scheduler_lock);
                                        foreground_task = task;
                                        if (task) {
                                            debuglog(DEBUG_INFO, "[TASK] Set foreground task: PID %u (%s)\n", task->id, task->name);
                                        }
                                        spinlock_release(&task_scheduler_lock);
                                    }

                                    void task_clear_foreground(void) {
                                        spinlock_acquire(&task_scheduler_lock);
                                        if (foreground_task) {
                                            debuglog(DEBUG_INFO, "[TASK] Cleared foreground task: PID %u\n", foreground_task->id);
                                        }
                                        foreground_task = NULL;
                                        spinlock_release(&task_scheduler_lock);
                                    }

                                    task_t* task_get_foreground(void) {
                                        return foreground_task;
                                    }

                                    bool task_is_foreground(task_t* task) {
                                        return task && task == foreground_task;
                                    }

                                    // Debug function to print the current state of the ready queue
                                    void debug_print_ready_queue(void) {
                                        print("[TASK] Ready queue state:\n");
                                        if (!ready_queue_head) {
                                            print("  Queue is empty\n");
                                            return;
                                        }

                                        task_t* current = ready_queue_head;
                                        int count = 0;
                                        do {
                                            if (!current) {
                                                print("  ERROR: NULL pointer in queue!\n");
                                                break;
                                            }

                                            print("  Task ");
                                            print(int_to_string(current->id));
                                            print(": state=");
                                            switch (current->state) {
                                                case TASK_STATE_RUNNING: print("RUNNING"); break;
                                                case TASK_STATE_READY: print("READY"); break;
                                                case TASK_STATE_WAITING: print("WAITING"); break;
                                                case TASK_STATE_TERMINATED: print("TERMINATED"); break;
                                                case TASK_STATE_ZOMBIE: print("ZOMBIE"); break;
                                                case TASK_STATE_SUSPENDED: print("SUSPENDED"); break;
                                                default: print("UNKNOWN"); break;
                                            }
                                            print(", next=");
                                            if (current->next) {
                                                print(int_to_string(current->next->id));
                                            } else {
                                                print("NULL");
                                            }
                                            print("\n");

                                            current = current->next;
                                            count++;
                                            if (count > 20) { // Prevent spam
                                                print("  ... (truncated after 20 tasks)\n");
                                                break;
                                            }
                                        } while (current && current != ready_queue_head);

                                        print("  Current task: ");
                                        if (current_task) {
                                            print(int_to_string(current_task->id));
                                        } else {
                                            print("NULL");
                                        }
                                        print("\n");
                                    }

                                    // Forward declaration for deferred cleanup
                                    static void task_process_deferred_cleanup(void);

                                    void task_schedule(void) {
                                        task_process_deferred_cleanup();
                                        task_reap_zombies();

                                        uint32 current_ticks = timer_get_ticks();

                                        // Poll mouse for constant movement detection when switching tasks
                                        // This ensures mouse position is always up-to-date for GUI applications
                                        ps2_mouse_poll();

                                        spinlock_acquire(&task_scheduler_lock);

                                        if (!task_sanitize_ready_queue_locked()) {
                                            if (!task_pick_safe_fallback_locked("ready queue unrecoverable before wakeup")) {
                                                spinlock_release(&task_scheduler_lock);
                                                return;
                                            }
                                        }

                                        // Wake up sleeping tasks
                                        task_t* t = ready_queue_head;
                                        if (t) {
                                            do {
                                                if (!task_ptr_plausible(t)) {
                                                    if (!task_recover_ready_queue_locked("invalid task pointer during wakeup")) {
                                                        if (!task_pick_safe_fallback_locked("wakeup scan pointer corruption")) {
                                                            spinlock_release(&task_scheduler_lock);
                                                            return;
                                                        }
                                                    }
                                                    break;
                                                }
                                                if (t->state == TASK_STATE_WAITING && t->sleep_until_tick > 0 && current_ticks >= t->sleep_until_tick) {
                                                    t->state = TASK_STATE_READY;
                                                    t->sleep_until_tick = 0;
                                                }
                                                if (!task_ptr_plausible(t->next)) {
                                                    print("[TASK] WARNING: Invalid next pointer during wakeup, repairing ring\n");
                                                    t->next = ready_queue_head;
                                                    break;
                                                }
                                                t = t->next;
                                            } while (t != ready_queue_head);
                                        }

                                        // Validate queue integrity first
                                        if (!validate_ready_queue()) {
                                            if (!task_recover_ready_queue_locked("validate_ready_queue failed")) {
                                                if (!task_pick_safe_fallback_locked("ready queue validation failure")) {
                                                    spinlock_release(&task_scheduler_lock);
                                                    return;
                                                }
                                            }
                                            if (!validate_ready_queue()) {
                                                if (!task_pick_safe_fallback_locked("ready queue remained invalid after recovery")) {
                                                    spinlock_release(&task_scheduler_lock);
                                                    return;
                                                }
                                            }
                                        }

                                        // Default disposition for the common terminate-class signals
                                        // (SIGHUP/SIGINT/SIGQUIT/SIGTERM): if pending, not blocked, and
                                        // not explicitly ignored via sigaction(SIG_IGN), fold into the
                                        // same unconditional-kill path as SIGKILL below. There's no
                                        // userspace handler-invocation machinery (no trampoline/sigreturn)
                                        // in this kernel yet, so "has a real handler installed" can't be
                                        // honored -- this at least makes Ctrl+C (SIGINT) actually terminate
                                        // a foreground job instead of silently setting a bit nothing reads.
                                        bool fatal_pending = false;
                                        uint32 fatal_signal = 0;
                                        if (current_task) {
                                            uint32 unblocked = current_task->pending_signals & ~current_task->signal_mask;
                                            if (unblocked & TASK_SIGNAL_BIT(SIGKILL)) {
                                                fatal_signal = SIGKILL;
                                            } else if ((unblocked & TASK_SIGNAL_BIT(SIGINT)) && !signal_is_ignored(SIGINT)) {
                                                fatal_signal = SIGINT;
                                            } else if ((unblocked & TASK_SIGNAL_BIT(SIGQUIT)) && !signal_is_ignored(SIGQUIT)) {
                                                fatal_signal = SIGQUIT;
                                            } else if ((unblocked & TASK_SIGNAL_BIT(SIGTERM)) && !signal_is_ignored(SIGTERM)) {
                                                fatal_signal = SIGTERM;
                                            } else if ((unblocked & TASK_SIGNAL_BIT(SIGHUP)) && !signal_is_ignored(SIGHUP)) {
                                                fatal_signal = SIGHUP;
                                            }
                                            fatal_pending = fatal_signal != 0;
                                        }
                                        if (current_task && fatal_pending) {
                                            debuglog(DEBUG_INFO, "[TASK] Terminating current task ID: %u\n", current_task->id);

                                            // Special case: if this is the only task
                                            if (current_task->next == current_task) {
                                                print("[TASK] WARNING: Last task requested termination; recovering scheduler state\n");
                                                if (task_ptr_plausible(idle_task) && idle_task != current_task) {
                                                    ready_queue_head = idle_task;
                                                    if (!task_ptr_plausible(idle_task->next)) {
                                                        idle_task->next = idle_task;
                                                    }
                                                    current_task = idle_task;
                                                    idle_task->state = TASK_STATE_RUNNING;
                                                    spinlock_release(&task_scheduler_lock);
                                                    return;
                                                }

                                                current_task->pending_signals &= ~(TASK_SIGNAL_BIT(SIGKILL) |
                                                    TASK_SIGNAL_BIT(SIGHUP) | TASK_SIGNAL_BIT(SIGINT) |
                                                    TASK_SIGNAL_BIT(SIGQUIT) | TASK_SIGNAL_BIT(SIGTERM));
                                                current_task->state = TASK_STATE_RUNNING;
                                                if (!task_ptr_plausible(current_task->next)) {
                                                    current_task->next = current_task;
                                                }
                                                ready_queue_head = current_task;
                                                spinlock_release(&task_scheduler_lock);
                                                return;
                                            }

                                            // Transition to a proper zombie (matches task_exit()'s
                                            // protocol) instead of destroying the task outright here.
                                            // The old code called task_destroy(current_task) directly,
                                            // which skips setting exit_code, skips TASK_STATE_ZOMBIE, and
                                            // skips sending SIGCHLD -- so a parent blocked in
                                            // task_wait_pid() (state=WAITING) was never woken and this
                                            // task never became something task_reap_child() could find.
                                            // Net effect: Ctrl+C (SIGINT) correctly killed the foreground
                                            // child, but the shell never got control back. 128+signal
                                            // mirrors the conventional shell exit-status encoding for a
                                            // signal-terminated process.
                                            current_task->exit_code = 128 + (int32)fatal_signal;
                                            current_task->state = TASK_STATE_ZOMBIE;
                                            current_task->pending_signals &= ~(TASK_SIGNAL_BIT(SIGKILL) |
                                                TASK_SIGNAL_BIT(SIGHUP) | TASK_SIGNAL_BIT(SIGINT) |
                                                TASK_SIGNAL_BIT(SIGQUIT) | TASK_SIGNAL_BIT(SIGTERM));
                                            task_send_signal(current_task->parent_pid, SIGCHLD);

                                            /*
                                             * IMPORTANT: keep current_task pointing at the terminating context
                                             * until task_switch() runs. Repointing current_task here can make
                                             * task_switch() think we're already on the destination task and skip
                                             * the switch, returning to the exiting task's halt loop.
                                             */
                                        }

                                        if (!ready_queue_head) {
                                            print("[TASK] WARNING: No tasks in ready queue, creating idle task\n");
                                            if (!task_pick_safe_fallback_locked("ready queue empty")) {
                                                spinlock_release(&task_scheduler_lock);
                                                return;
                                            }
                                        }

                                        // PRIORITY: Always prefer foreground task if it's runnable
                                        // This ensures GUI apps get responsive scheduling
                                        task_t* next_task = NULL;
                                        bool found_runnable = false;

                                        task_graphics_watchdog_check();

                                        task_t* best_graphics = NULL;
                                        uint32 best_graphics_prio = 0;
                                        task_t* scan = ready_queue_head;
                                        if (scan) {
                                            do {
                                                if (scan &&
                                                    (scan->state == TASK_STATE_READY || scan->state == TASK_STATE_RUNNING) &&
                                                    !(scan->pending_signals & TASK_SIGNAL_BIT(SIGKILL)) &&
                                                    (scan->is_graphics_task || scan->has_framebuffer_mapping || task_is_foreground(scan))) {
                                                    uint32 rp = task_get_real_priority(scan);
                                                    if (rp > best_graphics_prio) {
                                                        best_graphics_prio = rp;
                                                        best_graphics = scan;
                                                    }
                                                }
                                                scan = scan->next;
                                            } while (scan && scan != ready_queue_head);
                                        }

                                        if (best_graphics) {
                                            next_task = best_graphics;
                                            found_runnable = true;
                                        } else if (foreground_task &&
                                            (foreground_task->state == TASK_STATE_READY || foreground_task->state == TASK_STATE_RUNNING) &&
                                            !(foreground_task->pending_signals & TASK_SIGNAL_BIT(SIGKILL)) &&
                                            !foreground_task->is_background) {
                                            // Foreground task is runnable - use it (unless it's a background task)
                                            next_task = foreground_task;
                                        found_runnable = true;
                                            }

                                            // If no foreground or foreground not runnable, do normal round-robin
                                            if (!found_runnable) {
                                                next_task = current_task;
                                                if (!task_ptr_plausible(next_task)) {
                                                    print("[TASK] WARNING: current_task pointer invalid during scheduling, using ready_queue_head\n");
                                                    next_task = ready_queue_head;
                                                }
                                                if (!next_task) {
                                                    next_task = ready_queue_head;
                                                }
                                            }

                                            task_t* initial_scan_start = next_task;
                                            int scan_count = 0; // Prevent infinite loops

                                            // Only scan if we haven't already found a runnable task (foreground)
                                            while (!found_runnable) {
                                                if (!task_ptr_plausible(next_task)) {
                                                    if (!task_recover_ready_queue_locked("invalid scan cursor before advancing")) {
                                                        if (!task_pick_safe_fallback_locked("invalid scan cursor")) {
                                                            spinlock_release(&task_scheduler_lock);
                                                            return;
                                                        }
                                                    }
                                                    next_task = ready_queue_head;
                                                    initial_scan_start = next_task;
                                                    if (!next_task) {
                                                        break;
                                                    }
                                                }

                                                next_task = next_task->next;
                                                scan_count++;

                                                if (!task_ptr_plausible(next_task)) {
                                                    print("[TASK] WARNING: Invalid task pointer encountered while scanning ready queue, repairing\n");
                                                    if (!task_recover_ready_queue_locked("invalid next pointer during scan")) {
                                                        if (!task_pick_safe_fallback_locked("invalid scan next pointer")) {
                                                            spinlock_release(&task_scheduler_lock);
                                                            return;
                                                        }
                                                    }
                                                    next_task = ready_queue_head;
                                                    initial_scan_start = next_task;
                                                    if (!next_task) {
                                                        break;
                                                    }
                                                    continue;
                                                }

                                                // Prevent infinite scanning
                                                if (scan_count > 1000) {
                                                    print("[TASK] ERROR: Infinite loop detected in task scanning\n");
                                                    if (!task_pick_safe_fallback_locked("scan loop guard exceeded")) {
                                                        spinlock_release(&task_scheduler_lock);
                                                        return;
                                                    }
                                                    next_task = ready_queue_head;
                                                    break;
                                                }

                                                // Check if this task is runnable
                                                if (next_task->state == TASK_STATE_READY || next_task->state == TASK_STATE_RUNNING) {
                                                    found_runnable = true;
                                                    break;
                                                }

                                                if (next_task->state == TASK_STATE_ZOMBIE) {
                                                    next_task = next_task->next;
                                                    continue;
                                                }

                                                // Stop if we've scanned the whole queue
                                                if (next_task == initial_scan_start) {
                                                    break;
                                                }
                                            }

                                            // If no runnable task was found, fall back to idle task
                                            if (!found_runnable) {
                                                print("[TASK] WARNING: No runnable tasks found (");
                                                print(int_to_string(count_runnable_tasks()));
                                                print(" runnable), switching to idle task\n");

                                                // Use idle task as fallback
                                                if (idle_task && (idle_task->state == TASK_STATE_READY || idle_task->state == TASK_STATE_RUNNING)) {
                                                    next_task = idle_task;
                                                    next_task->state = TASK_STATE_RUNNING;
                                                } else if (ready_queue_head && (ready_queue_head->state == TASK_STATE_READY || ready_queue_head->state == TASK_STATE_RUNNING)) {
                                                    // Final fallback to kernel task
                                                    next_task = ready_queue_head;
                                                    print("[TASK] Falling back to kernel task\n");
                                                } else {
                                                    print("[TASK] WARNING: No runnable tasks available; continuing current context\n");
                                                    if (task_ptr_plausible(current_task)) {
                                                        next_task = current_task;
                                                        if (!task_ptr_plausible(next_task->next)) {
                                                            next_task->next = next_task;
                                                        }
                                                        if (!ready_queue_head) {
                                                            ready_queue_head = next_task;
                                                        }
                                                        next_task->state = TASK_STATE_RUNNING;
                                                        found_runnable = true;
                                                    } else if (task_ptr_plausible(ready_queue_head)) {
                                                        next_task = ready_queue_head;
                                                        next_task->state = TASK_STATE_RUNNING;
                                                        found_runnable = true;
                                                    } else if (task_ptr_plausible(idle_task)) {
                                                        next_task = idle_task;
                                                        if (!task_ptr_plausible(idle_task->next)) {
                                                            idle_task->next = idle_task;
                                                        }
                                                        if (!ready_queue_head) {
                                                            ready_queue_head = idle_task;
                                                        }
                                                        next_task->state = TASK_STATE_RUNNING;
                                                        found_runnable = true;
                                                    } else {
                                                        spinlock_release(&task_scheduler_lock);
                                                        return;
                                                    }
                                                }
                                            }

                                            // Update state of selected task
                                            if (next_task->state != TASK_STATE_RUNNING) {
                                                next_task->state = TASK_STATE_RUNNING;
                                            }

                                            /*
                                             * Close the preemption window between scheduler unlock and task_switch().
                                             * If a timer IRQ fires in that gap, nested scheduling can observe/transiently
                                             * mutate inconsistent queue state and corrupt first-switch frames.
                                             */
                                            bool restore_irq = (task_scheduler_lock.saved_flags & 0x200u) != 0;
                                            spinlock_release_noirq(&task_scheduler_lock);
                                            task_switch(next_task);
                                            if (restore_irq) {
                                                __asm__ __volatile__("sti");
                                            }
                                    }


                                    // Deallocates resources associated with a task and removes it from the ready queue
                                    void task_destroy(task_t* task) {
                                        if (!task) {
                                            return;
                                        }

                                        uintptr_t current_esp;
                                        #if ARCH_64BIT
                                        __asm__ __volatile__("mov %%rsp, %0" : "=r"(current_esp));
                                        #else
                                        __asm__ __volatile__("mov %%esp, %0" : "=r"(current_esp));
                                        #endif
                                        uintptr_t stack_start = task->kernel_stack_base;
                                        uintptr_t stack_end = stack_start + KERNEL_STACK_SIZE;
                                        bool destroying_current_context =
                                        (task == current_task) ||
                                        (current_esp >= stack_start && current_esp < stack_end);

                                        if (task == foreground_task) {
                                            debuglog(DEBUG_INFO, "[TASK] Foreground task %u being destroyed\n", task->id);
                                            foreground_task = NULL;
                                        }

                                        framebuffer_mmap_task_exit(task);

                                        // Checked regardless of current `state`: task_send_signal() can force
                                        // WAITING -> READY (bypassing the semaphore's own dequeue) and the
                                        // fatal-signal path can then force READY -> ZOMBIE without ever
                                        // resuming the task's suspended semaphore_wait() frame, so by the
                                        // time we get here `state` is no longer WAITING even though the wait
                                        // node is still live on the semaphore's queue.
                                        if (task->waiting_semaphore) {
                                            semaphore_remove_task((semaphore_t*)task->waiting_semaphore, task);
                                            task->waiting_semaphore = NULL;
                                        }

                                        task_reparent_children(task->id, 1);

                                        {
                                            spinlock_acquire(&ipc_lock);
                                            for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
                                                if (ipc_shm_regions[i].in_use && ipc_shm_regions[i].owner_pid == task->id) {
                                                    ipc_shm_regions[i].in_use = false;
                                                    ipc_shm_regions[i].ref_count = 0;
                                                    if (ipc_shm_regions[i].phys_addr) {
                                                        uint32 pages = ipc_shm_regions[i].size / MEMORY_PAGE_SIZE;
                                                        for (uint32 p = 0; p < pages; p++) {
                                                            pmm_free_frame(ipc_shm_regions[i].phys_addr + (p * MEMORY_PAGE_SIZE));
                                                        }
                                                    }
                                                }
                                            }
                                            for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
                                                if (ipc_msg_queues[i].in_use && ipc_msg_queues[i].creator_pid == task->id) {
                                                    ipc_msg_queues[i].in_use = false;
                                                }
                                            }
                                            spinlock_release(&ipc_lock);
                                        }

                                        syscall_close_all_fds_for_task(task->id);
                                        syscall_detach_all_shm_for_task(task->id);
                                        // fd-like handles from disjoint ranges the regular fd table doesn't
                                        // cover, plus SysV IPC objects that have no owner-exit cleanup by
                                        // default -- all leak permanently on process exit/crash otherwise.
                                        epoll_close_all_for_task(task->id);
                                        eventfd_close_all_for_task(task->id);
                                        inotify_close_all_for_task(task->id);
                                        sysv_msg_close_all_for_task(task->id);
                                        sysv_sem_close_all_for_task(task->id);
                                        posix_shm_close_all_for_task(task->id);

                                        debuglog(DEBUG_INFO, "[TASK] PID %u terminated%s%s%s with code %d\n",
                                            task->id,
                                            task->exit_reason[0] ? " (" : "",
                                            task->exit_reason[0] ? task->exit_reason : "",
                                            task->exit_reason[0] ? ")" : "",
                                            task->exit_code);

                                        // Remove from ready queue
                                        if (ready_queue_head == task) {
                                            if (task->next == task) {
                                                ready_queue_head = 0;
                                            } else {
                                                task_t* current = ready_queue_head;
                                                while (current->next != ready_queue_head) {
                                                    current = current->next;
                                                }
                                                ready_queue_head = task->next;
                                                current->next = ready_queue_head;
                                            }
                                        } else {
                                            task_t* current = ready_queue_head;
                                            while (current && current->next != task && current->next != ready_queue_head) {
                                                current = current->next;
                                            }
                                            if (current && current->next == task) {
                                                current->next = task->next;
                                            }
                                        }

                                        page_directory_t* active_pd = vmm_get_current_page_directory();
                                        if (task->page_directory &&
                                            task->page_directory != active_pd &&
                                            !destroying_current_context) {
                                            vmm_destroy_page_directory(task->page_directory);
                                            } else if (task->page_directory) {
                                                debuglog(DEBUG_INFO, "[TASK] Deferring page-directory cleanup for PID %u\n", task->id);

                                                spinlock_acquire(&deferred_cleanup_lock);
                                                if (deferred_cleanup_count < MAX_DEFERRED_CLEANUP) {
                                                    deferred_cleanup_tasks[deferred_cleanup_count++] = task;
                                                } else {
                                                    debuglog(DEBUG_WARN, "[TASK] WARNING: Deferred cleanup list full, leaking memory!\n");
                                                }
                                                spinlock_release(&deferred_cleanup_lock);

                                                return;
                                            }

                                            if (!destroying_current_context) {
                                                kfree((void*)task->kernel_stack_base);
                                                kfree(task);
                                            } else {
                                                debuglog(DEBUG_WARN, "[TASK] WARNING: Deferring kernel stack cleanup (in use)\n");
                                                debuglog(DEBUG_WARN, "[TASK] WARNING: Deferring task-struct cleanup (in use)\n");

                                                spinlock_acquire(&deferred_cleanup_lock);
                                                if (deferred_cleanup_count < MAX_DEFERRED_CLEANUP) {
                                                    deferred_cleanup_tasks[deferred_cleanup_count++] = task;
                                                } else {
                                                    debuglog(DEBUG_WARN, "[TASK] WARNING: Deferred cleanup list full, leaking memory!\n");
                                                }
                                                spinlock_release(&deferred_cleanup_lock);
                                            }
                                    }

                                    // Process deferred task cleanup - call after context switch when safe
                                    void task_process_deferred_cleanup(void) {
                                        spinlock_acquire(&deferred_cleanup_lock);

                                        uint32_t count = deferred_cleanup_count;
                                        deferred_cleanup_count = 0;

                                        task_t* local_list[MAX_DEFERRED_CLEANUP];
                                        for (uint32_t i = 0; i < count; i++) {
                                            local_list[i] = deferred_cleanup_tasks[i];
                                            deferred_cleanup_tasks[i] = NULL;
                                        }

                                        spinlock_release(&deferred_cleanup_lock);

                                        for (uint32_t i = 0; i < count; i++) {
                                            task_t* task = local_list[i];
                                            // Snapshot both fields ONCE, before any use: something (still not
                                            // root-caused -- suspect a reentrant timer-IRQ path concurrently
                                            // destroying/freeing this same task) can poison this exact task
                                            // struct with kfree's freed-memory pattern (mm_debug.c's
                                            // POISON_FREE) in the gap between reads. Both fields are always
                                            // page-frame-aligned when genuinely alive (page_directory via
                                            // vmm_create_page_directory(); kernel_stack_base via
                                            // kmalloc_aligned(..., MEMORY_PAGE_SIZE) in task_clone_current());
                                            // poisoned/garbage values essentially never are. If either looks
                                            // wrong, treat the whole struct as already-freed and skip it
                                            // entirely -- freeing kernel_stack_base or the struct itself off a
                                            // poisoned pointer would corrupt the heap allocator, which is worse
                                            // than leaking this one task's memory.
                                            page_directory_t* task_pd = task ? task->page_directory : NULL;
                                            uintptr_t task_kstack_base = task ? task->kernel_stack_base : 0;
                                            bool task_looks_freed = task &&
                                                (((uintptr_t)task_pd & MEMORY_PAGE_MASK) != 0 ||
                                                 (task_kstack_base & MEMORY_PAGE_MASK) != 0);
                                            if (task_looks_freed) {
                                                debuglog(DEBUG_WARN,
                                                         "[TASK] Skipping deferred cleanup for already-freed task %p (double-destroy?)\n",
                                                         (void*)task);
                                                continue;
                                            }
                                            if (task) {
                                                uint32 task_id = task->id;
                                                debuglog(DEBUG_INFO, "[TASK] Processing deferred cleanup for PID %u\n", task_id);

                                                framebuffer_mmap_task_exit(task);

                                                if (task->waiting_semaphore) {
                                                    semaphore_remove_task((semaphore_t*)task->waiting_semaphore, task);
                                                    task->waiting_semaphore = NULL;
                                                }

                                                spinlock_acquire(&ipc_lock);
                                                for (int j = 0; j < IPC_MAX_SHM_REGIONS; j++) {
                                                    if (ipc_shm_regions[j].in_use && ipc_shm_regions[j].owner_pid == task->id) {
                                                        ipc_shm_regions[j].in_use = false;
                                                        ipc_shm_regions[j].ref_count = 0;
                                                        if (ipc_shm_regions[j].phys_addr) {
                                                            uint32 pages = ipc_shm_regions[j].size / MEMORY_PAGE_SIZE;
                                                            for (uint32 p = 0; p < pages; p++) {
                                                                pmm_free_frame(ipc_shm_regions[j].phys_addr + (p * MEMORY_PAGE_SIZE));
                                                            }
                                                        }
                                                    }
                                                }
                                                for (int j = 0; j < IPC_MAX_MSG_QUEUES; j++) {
                                                    if (ipc_msg_queues[j].in_use && ipc_msg_queues[j].creator_pid == task->id) {
                                                        ipc_msg_queues[j].in_use = false;
                                                    }
                                                }
                                                spinlock_release(&ipc_lock);

                                                syscall_close_all_fds_for_task(task_id);
                                                syscall_detach_all_shm_for_task(task_id);
                                                epoll_close_all_for_task(task_id);
                                                eventfd_close_all_for_task(task_id);
                                                inotify_close_all_for_task(task_id);
                                                sysv_msg_close_all_for_task(task_id);
                                                sysv_sem_close_all_for_task(task_id);
                                                posix_shm_close_all_for_task(task_id);

                                                if (task_pd) {
                                                    vmm_destroy_page_directory(task_pd);
                                                    task->page_directory = NULL;
                                                }

                                                kfree((void*)task_kstack_base);
                                                kfree(task);

                                                debuglog(DEBUG_INFO, "[TASK] Deferred cleanup complete for PID %u\n", task_id);
                                            }
                                        }
                                    }

// Sets pending SIGKILL on `pid` alone (no descendants). Shared by task_kill()
// and the tree-propagation sweep below. Returns true if the signal was set.
static bool task_kill_one(uint32 pid) {
    task_t* task_to_kill = task_find_by_pid(pid);
    if (!task_to_kill) {
        return false;
    }

    if (task_to_kill->is_protected) {
        debuglog(DEBUG_WARN, "[TASK] Refusing to kill protected task PID %u (%s)\n",
                 pid, task_to_kill->name);
        return false;
    }

    task_to_kill->pending_signals |= TASK_SIGNAL_BIT(SIGKILL);
    return true;
}

// Propagates `sig` (already pending on `root_pid`) down the whole descendant
// tree rooted at `root_pid`: children, grandchildren, etc. Propagation is an
// iterative fixed-point sweep rather than recursion: each pass walks the
// whole task list and marks any not-yet-marked task whose parent already has
// `sig` pending, repeating until a pass makes no new marks. That bounds
// stack usage to O(1) regardless of tree depth, which matters on our 8KB
// kernel stacks. A protected task blocks propagation through it (its subtree
// is left running under it) -- same spirit as task_kill_one() refusing to
// touch protected tasks at all.
void task_signal_tree(uint32 root_pid, uint32 sig) {
    bool changed;
    do {
        changed = false;
        task_t* t = ready_queue_head;
        if (t) {
            do {
                if (t->id != root_pid &&
                    !(t->pending_signals & TASK_SIGNAL_BIT(sig)) &&
                    !t->is_protected) {
                    task_t* parent = task_find_by_pid(t->parent_pid);
                    if (parent && (parent->pending_signals & TASK_SIGNAL_BIT(sig))) {
                        t->pending_signals |= TASK_SIGNAL_BIT(sig);
                        changed = true;
                    }
                }
                t = t->next;
            } while (t != ready_queue_head);
        }
    } while (changed);
}

// Kills `pid` and its entire descendant tree. See task_signal_tree() above.
void task_kill(uint32 pid) {
    if (pid == 0) {
        return;
    }

    if (!task_kill_one(pid)) {
        return;
    }

    task_signal_tree(pid, SIGKILL);
}

void task_suspend(uint32 pid) {
    if (pid == 0) {
        return;
    }

    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->id == pid) {
                if (t->state != TASK_STATE_TERMINATED && t->state != TASK_STATE_ZOMBIE) {
                    t->state = TASK_STATE_SUSPENDED;
                    if (t == current_task) {
                        t->pending_signals |= TASK_SIGNAL_BIT(SIGSTOP);
                    }
                }
                spinlock_release(&task_scheduler_lock);
                return;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
}

void task_resume(uint32 pid) {
    if (pid == 0) {
        return;
    }

    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->id == pid) {
                if (t->state == TASK_STATE_SUSPENDED) {
                    t->state = TASK_STATE_READY;
                }
                t->pending_signals &= ~TASK_SIGNAL_BIT(SIGSTOP);
                spinlock_release(&task_scheduler_lock);
                return;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
}

/* Snapshot up to max_entries real tasks from ready_queue_head into out,
 * for sys_get_tasks() (SYS_GET_TASKS) to copy out to userspace `ps`.
 * See task_info_t's comment in task.h for why this is a plain-old-data
 * struct rather than exposing task_t (which contains raw kernel
 * pointers) directly to userspace. */
int32 task_get_all(task_info_t* out, uint32 max_entries) {
    if (!out) {
        return -1;
    }
    if (max_entries == 0) {
        return 0;
    }

    spinlock_acquire(&task_scheduler_lock);

    uint32 written = 0;
    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (written >= max_entries) {
                break;
            }

            task_info_t* dst = &out[written];
            dst->pid = t->id;
            dst->parent_pid = t->parent_pid;
            dst->pgrp = t->pgrp;
            dst->state = (int32)t->state;
            dst->last_active_tick = t->last_active_tick;
            dst->priority = task_get_real_priority(t);

            /* Include the still-running slice for whichever task is
             * current_task right now, so its %CPU isn't frozen at the value
             * from its last switch-out. */
            dst->cpu_ticks_total = t->cpu_ticks_total;
            if (t == current_task) {
                dst->cpu_ticks_total += (uint64)(timer_get_ticks() - t->scheduled_at_tick);
            }

            dst->memory_used_kb = (t->user_brk > t->user_heap_base)
                ? (uint32)((t->user_brk - t->user_heap_base) / 1024)
                : 0;
            dst->created_at_tick = t->created_at_tick;

            uint32 i;
            for (i = 0; i < sizeof(dst->name) - 1 && t->name[i]; i++) {
                dst->name[i] = t->name[i];
            }
            dst->name[i] = '\0';

            written++;
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);

    return (int32)written;
}

/* Instantaneous runnable-task count (RUNNING + READY), for sys_sysinfo()'s
 * load average fields. Not a decayed moving average like real Linux
 * loadavg -- there's no periodic sampler in this kernel to feed one -- but
 * a live snapshot beats the hardcoded 1.0/0.5/0.25 constants this used to
 * report unconditionally. */
uint32 task_count_runnable(void) {
    uint32 count = 0;

    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->state == TASK_STATE_RUNNING || t->state == TASK_STATE_READY) {
                count++;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
    return count;
}

void task_set_priority(uint32 pid, uint32 priority) {
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->id == pid) {
                t->priority = priority;
                t->original_priority = priority;
                spinlock_release(&task_scheduler_lock);
                return;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
}

uint32 task_get_id_by_name_prefix(const char* prefix) {
    if (!prefix || !prefix[0]) {
        return 0;
    }

    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (strncmp(t->name, prefix, strlen(prefix)) == 0) {
                uint32 found_id = t->id;
                spinlock_release(&task_scheduler_lock);
                return found_id;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
    return 0;
}

void task_set_protected(uint32 pid, bool protected_flag) {
    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->id == pid) {
                t->is_protected = protected_flag;
                spinlock_release(&task_scheduler_lock);
                return;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
}

void task_send_signal(int32 pid, int signal) {
    if (signal < 1 || signal > 31) {
        return;
    }

    if (pid == 0) {
        // Send to current process group
        if (!current_task) {
            return;
        }
        task_send_signal_to_pgrp(current_task->pgrp, signal);
        return;
    }

    if (pid < 0) {
        // Send to process group (absolute value of pid)
        uint32 pgrp = (uint32)-pid;
        task_send_signal_to_pgrp(pgrp, signal);
        return;
    }

    // Find the target task
    task_t* target_task = ready_queue_head;
    if (target_task) {
        do {
            if (target_task->id == (uint32)pid) {
                target_task->pending_signals |= TASK_SIGNAL_BIT(signal);
                if (target_task->state == TASK_STATE_WAITING) {
                    target_task->state = TASK_STATE_READY;
                }
                return;
            }
            target_task = target_task->next;
        } while (target_task != ready_queue_head);
    }
}

// Returns true if `task` currently has `signal` blocked (signal_mask, set
// via sys_rt_sigprocmask) or set to SIG_IGN (process-wide signal_handlers
// table, set via sys_rt_sigaction). Used to gate delivery of signals whose
// generation POSIX says must be suppressed entirely when blocked/ignored by
// the target (currently only applied to SIGTTIN/SIGTTOU - see
// task_send_signal_to_pgrp_checked()).
static bool task_signal_blocked_or_ignored(task_t* task, int signal) {
    if (!task || signal < 1 || signal > 31) {
        return false;
    }
    if (task->signal_mask & TASK_SIGNAL_BIT(signal)) {
        return true;
    }
    return signal_is_ignored(signal);
}

void task_send_signal_to_pgrp(uint32 pgrp, int signal) {
    if (signal < 1 || signal > 31) {
        return;
    }

    // Stop-class signals must actually transition the target task(s) into a
    // stopped state (via the existing task_suspend() mechanism, the same
    // one used for the SIGSTOP-like fullscreen-toggle path), not just set a
    // pending bit that nothing ever consumes.
    bool is_stop_class = (signal == SIGSTOP || signal == SIGTSTP ||
                           signal == SIGTTIN || signal == SIGTTOU);
    bool stopped_any = false;

    task_t* current = ready_queue_head;
    if (current) {
        do {
            if (current->pgrp == pgrp) {
                current->pending_signals |= TASK_SIGNAL_BIT(signal);
                if (current->state == TASK_STATE_WAITING) {
                    current->state = TASK_STATE_READY;
                }

                if (is_stop_class &&
                    current->state != TASK_STATE_TERMINATED &&
                    current->state != TASK_STATE_ZOMBIE) {
                    task_suspend(current->id);
                    stopped_any = true;
                }
            }
            current = current->next;
        } while (current != ready_queue_head);
    }

    // Make job_foreground()'s `if (cont && job->state == JOB_CTRL_STOPPED)`
    // resume branch reachable: record that this process group actually
    // stopped, if it is a tracked job.
    if (is_stop_class && stopped_any) {
        job_update_state(pgrp, JOB_CTRL_STOPPED, 0);
    }
}

// See task.h for the POSIX definition being approximated here. We consider
// pgrp non-orphaned if any of its members has a parent that is (a) still
// present in the ready queue (i.e. not yet reaped) and (b) in the same
// session but a different process group - that parent is a controlling
// shell (or equivalent) that could still fg/bg this group with SIGCONT.
// If no member has such a parent, the group is orphaned.
bool task_pgrp_is_orphaned(uint32 pgrp, uint32 session) {
    bool has_controlling_parent = false;

    spinlock_acquire(&task_scheduler_lock);

    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->pgrp == pgrp) {
                task_t* parent = task_find_by_pid(t->parent_pid);
                if (parent && parent->session == session && parent->pgrp != pgrp) {
                    has_controlling_parent = true;
                    break;
                }
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    spinlock_release(&task_scheduler_lock);
    return !has_controlling_parent;
}

signal_delivery_status_t task_send_signal_to_pgrp_checked(uint32 pgrp, int signal, uint32 checked_pid) {
    // Per POSIX, SIGTTIN/SIGTTOU must not be generated at all (to anyone in
    // the process group) if the checked task - normally the caller
    // attempting a background tty read()/write() - currently has the
    // signal blocked or set to SIG_IGN. The caller is expected to fail the
    // syscall with EIO in that case instead of raising a pointless signal.
    // Every other signal keeps the exact unconditional
    // task_send_signal_to_pgrp() behavior.
    if (signal == SIGTTIN || signal == SIGTTOU) {
        task_t* checked = task_find_by_pid(checked_pid);
        if (task_signal_blocked_or_ignored(checked, signal)) {
            return SIGNAL_DELIVERY_BLOCKED_OR_IGNORED;
        }
    }

    task_send_signal_to_pgrp(pgrp, signal);
    return SIGNAL_DELIVERY_SENT;
}

void task_yield(void) {
    // Yield CPU to next task by calling the scheduler
    task_schedule();
}

void task_shutdown_all(void) {
    if (!ready_queue_head) {
        return;
    }

    while (true) {
        // Same locking requirement as task_reap_zombies()/task_reap_child()/
        // task_wait_pid_any(): task_destroy() (and the task_reparent_children()
        // it calls internally) assumes ready_queue_head is only ever
        // walked/mutated under task_scheduler_lock. Held across both the
        // victim search and the destroy so a reentrant timer-IRQ scheduler
        // pass can't free a node this loop still holds a pointer into.
        spinlock_acquire(&task_scheduler_lock);

        task_t* victim = 0;
        task_t* iter = ready_queue_head;
        if (!iter) {
            spinlock_release(&task_scheduler_lock);
            break;
        }

        do {
            if (iter->elf_info.entry_point != 0) {
                victim = iter;
                break;
            }
            iter = iter->next;
        } while (iter && iter != ready_queue_head);

        if (!victim) {
            spinlock_release(&task_scheduler_lock);
            break;
        }

        if (victim == current_task) {
            current_task = (victim->next != victim) ? victim->next : 0;
        }

        task_destroy(victim);
        bool queue_empty = !ready_queue_head;
        spinlock_release(&task_scheduler_lock);
        if (queue_empty) {
            break;
        }
    }
}

                                    void sleep_busy(uint32 microseconds) {
                                        uint32 start_ticks = timer_get_ticks();
                                        // Assuming 100Hz timer, so 1 tick = 10ms = 10000us
                                        uint32 ticks_to_wait = microseconds / 10000;
                                        if (microseconds % 10000 != 0) {
                                            ticks_to_wait++;
                                        }

                                        uint32 end_ticks = start_ticks + ticks_to_wait;
                                        while (timer_get_ticks() < end_ticks) {
                                            // Busy wait
                                            asm volatile("pause");
                                        }
                                    }

                                    void sleep_interruptible(uint32 milliseconds) {
                                        if (!current_task || milliseconds == 0) {
                                            return;
                                        }

                                        // Assuming 100Hz timer, so 1 tick = 10ms
                                        uint32 ticks_to_sleep = milliseconds / 10;
                                        if (ticks_to_sleep == 0) {
                                            ticks_to_sleep = 1;
                                        }

                                        uint32 current_ticks = timer_get_ticks();
                                        current_task->sleep_until_tick = current_ticks + ticks_to_sleep;
                                        current_task->state = TASK_STATE_WAITING;

                                        task_schedule();
                                    }

                                    void task_terminate_current(int signal) {
                                        (void)signal;

                                        if (!current_task) {
                                            print("[TASK] Cannot terminate - no current task\n");
                                            return;
                                        }

                                        debuglog(DEBUG_INFO, "[TASK] Terminating current task (ID: %u) with signal %d\n",
                                            current_task->id, signal);

                                        // 128+signal mirrors the conventional shell exit-status encoding
                                        // for a signal-terminated process (see task_schedule()'s fatal
                                        // signal path for the same convention); without this, wait4()
                                        // always reports a signal-killed child as exit code 0.
                                        current_task->exit_code = 128 + (int32)signal;

                                        framebuffer_mmap_task_exit(current_task);
                                        net_close_all_for_task(current_task->id);
                                        // task_reparent_children() walks/writes ready_queue_head with no
                                        // locking of its own -- every other caller (task_reap_zombies(),
                                        // task_reap_child(), task_wait_pid_any(), all via task_destroy())
                                        // already holds task_scheduler_lock while touching that list. Without
                                        // it here, a timer IRQ landing mid-walk can reenter task_schedule()
                                        // -> task_reap_zombies(), which destroys/frees a node this walk is
                                        // still holding a pointer into: a genuine use-after-free, not a
                                        // hypothetical one (see the poisoned-task-struct workaround in
                                        // task_process_deferred_cleanup()).
                                        spinlock_acquire(&task_scheduler_lock);
                                        task_reparent_children(current_task->id, 1);
                                        spinlock_release(&task_scheduler_lock);

                                        {
                                            spinlock_acquire(&ipc_lock);
                                            for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
                                                if (ipc_shm_regions[i].in_use && ipc_shm_regions[i].owner_pid == current_task->id) {
                                                    ipc_shm_regions[i].in_use = false;
                                                    ipc_shm_regions[i].ref_count = 0;
                                                    if (ipc_shm_regions[i].phys_addr) {
                                                        uint32 pages = ipc_shm_regions[i].size / MEMORY_PAGE_SIZE;
                                                        for (uint32 p = 0; p < pages; p++) {
                                                            pmm_free_frame(ipc_shm_regions[i].phys_addr + (p * MEMORY_PAGE_SIZE));
                                                        }
                                                    }
                                                }
                                            }
                                            for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
                                                if (ipc_msg_queues[i].in_use && ipc_msg_queues[i].creator_pid == current_task->id) {
                                                    ipc_msg_queues[i].in_use = false;
                                                }
                                            }
                                            spinlock_release(&ipc_lock);
                                        }

                                        syscall_reset_stdio_redirect();

                                        current_task->state = TASK_STATE_ZOMBIE;
                                        // NOTE: intentionally NOT setting a pending SIGKILL here. This task
                                        // is already ZOMBIE and the scheduler skips ZOMBIE tasks when
                                        // picking the next task to run, so it will never execute again
                                        // regardless. Injecting a fake SIGKILL used to make
                                        // task_schedule()'s fatal-signal check ALSO call task_destroy() on
                                        // this same task moments after task_reap_zombies() (called earlier
                                        // in the very same task_schedule() invocation) already destroyed
                                        // it -- a same-call double-destroy, not a rare IRQ race. It also
                                        // meant the parent's wait()/waitpid() could never observe the exit
                                        // code: this task destroyed (freed) itself before its parent ever
                                        // got a chance to read task->exit_code. Leaving it as a plain
                                        // zombie lets the parent (task_wait_pid()/task_reap_child()) reap
                                        // it once it has actually consumed the exit status.
                                        task_send_signal(current_task->parent_pid, SIGCHLD);
                                        task_schedule();

                                        while (1) {
                                            __asm__ volatile("hlt");
                                        }
                                    }

                                    void task_exit(int code, const char* reason) {
                                        if (!current_task) {
                                            print("[TASK] task_exit called with no current task\n");
                                            return;
                                        }

                                        current_task->exit_code = code;
                                        memory_set((uint8*)current_task->exit_reason, 0, sizeof(current_task->exit_reason));
                                        if (reason) {
                                            for (size_t i = 0; i + 1 < sizeof(current_task->exit_reason) && reason[i]; i++) {
                                                current_task->exit_reason[i] = reason[i];
                                            }
                                        }

                                        framebuffer_mmap_task_exit(current_task);
                                        net_close_all_for_task(current_task->id);

                                        // See the matching comment in task_terminate_current(): every other
                                        // caller of task_reparent_children() holds task_scheduler_lock while
                                        // it walks/writes ready_queue_head; without it here a reentrant
                                        // timer-IRQ call into task_reap_zombies() can free a node this walk
                                        // still holds a pointer into.
                                        spinlock_acquire(&task_scheduler_lock);
                                        task_reparent_children(current_task->id, 1);
                                        spinlock_release(&task_scheduler_lock);

                                        {
                                            spinlock_acquire(&ipc_lock);
                                            for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
                                                if (ipc_shm_regions[i].in_use && ipc_shm_regions[i].owner_pid == current_task->id) {
                                                    ipc_shm_regions[i].in_use = false;
                                                    ipc_shm_regions[i].ref_count = 0;
                                                    if (ipc_shm_regions[i].phys_addr) {
                                                        uint32 pages = ipc_shm_regions[i].size / MEMORY_PAGE_SIZE;
                                                        for (uint32 p = 0; p < pages; p++) {
                                                            pmm_free_frame(ipc_shm_regions[i].phys_addr + (p * MEMORY_PAGE_SIZE));
                                                        }
                                                    }
                                                }
                                            }
                                            for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
                                                if (ipc_msg_queues[i].in_use && ipc_msg_queues[i].creator_pid == current_task->id) {
                                                    ipc_msg_queues[i].in_use = false;
                                                }
                                            }
                                            spinlock_release(&ipc_lock);
                                        }

                                        syscall_reset_stdio_redirect();

                                        debuglog(DEBUG_INFO, "[TASK] PID %u exiting with code %d%s%s%s\n",
                                            current_task->id, code,
                                            current_task->exit_reason[0] ? " (" : "",
                                            current_task->exit_reason[0] ? current_task->exit_reason : "",
                                            current_task->exit_reason[0] ? ")" : "");

                                        current_task->state = TASK_STATE_ZOMBIE;
                                        // See the matching NOTE in task_terminate_current(): no fake
                                        // pending SIGKILL here either, for the same reason (avoids the
                                        // same-call double-destroy and lets the parent's wait() actually
                                        // observe this exit code before the task is reaped).
                                        task_send_signal(current_task->parent_pid, SIGCHLD);
                                        task_schedule();

                                        while (1) {
                                            __asm__ __volatile__("hlt");
                                        }
                                    }

void task_set_framebuffer_mapping(task_t* task, bool mapped) {
    if (!task) {
        return;
    }
    task->has_framebuffer_mapping = mapped;
    if (mapped && task->priority < TASK_PRIORITY_GUI) {
        task->original_priority = task->priority;
        task->priority = TASK_PRIORITY_GUI;
        task->boost_expires_at = timer_get_ticks() + TASK_PRIORITY_BOOST_TICKS;
    }
}

void task_set_graphics_task(task_t* task, bool graphics) {
    if (!task) {
        return;
    }
    task->is_graphics_task = graphics;
    if (graphics && task->priority < TASK_PRIORITY_GUI) {
        task->original_priority = task->priority;
        task->priority = TASK_PRIORITY_GUI;
        task->boost_expires_at = timer_get_ticks() + TASK_PRIORITY_BOOST_TICKS;
    }
}

bool task_is_graphics_task(task_t* task) {
    return task && task->is_graphics_task;
}

bool task_has_framebuffer_mapping(task_t* task) {
    return task && task->has_framebuffer_mapping;
}

int32 task_set_pgrp(uint32 pid, uint32 new_pgrp) {
    spinlock_acquire(&task_scheduler_lock);
    task_t* task = task_find_by_pid(pid);
    if (!task) {
        spinlock_release(&task_scheduler_lock);
        return -1;
    }
    task->pgrp = new_pgrp;
    spinlock_release(&task_scheduler_lock);
    return 0;
}

int32 task_get_pgrp(uint32 pid) {
    spinlock_acquire(&task_scheduler_lock);
    task_t* task = task_find_by_pid(pid);
    int32 result = task ? (int32)task->pgrp : -1;
    spinlock_release(&task_scheduler_lock);
    return result;
}

int32 task_set_session(uint32 pid, uint32 new_session) {
    spinlock_acquire(&task_scheduler_lock);
    task_t* task = task_find_by_pid(pid);
    if (!task) {
        spinlock_release(&task_scheduler_lock);
        return -1;
    }
    task->session = new_session;
    spinlock_release(&task_scheduler_lock);
    return 0;
}

int32 task_get_session(uint32 pid) {
    spinlock_acquire(&task_scheduler_lock);
    task_t* task = task_find_by_pid(pid);
    int32 result = task ? (int32)task->session : -1;
    spinlock_release(&task_scheduler_lock);
    return result;
}

void task_reparent_children(uint32 old_parent, uint32 new_parent) {
    if (!ready_queue_head) {
        return;
    }
    task_t* t = ready_queue_head;
    do {
        if (t->parent_pid == old_parent) {
            t->parent_pid = new_parent;
            if (t->state == TASK_STATE_ZOMBIE) {
                task_send_signal(new_parent, SIGCHLD);
            }
        }
        t = t->next;
    } while (t && t != ready_queue_head);
}

void task_reap_zombies(void) {
    if (!ready_queue_head) {
        return;
    }
    spinlock_acquire(&task_scheduler_lock);
    task_t* t = ready_queue_head;
    do {
        task_t* next = t->next;
        // Only auto-reap orphans (parent no longer exists to wait() for
        // them). A zombie with a live parent must stay around until that
        // parent explicitly consumes the exit code via task_wait_pid()/
        // task_wait_pid_any() -- otherwise the parent's wait() races this
        // sweep (which runs inside every task_schedule() call, including
        // ones the parent itself triggers while polling) and always loses,
        // getting ECHILD instead of the real exit status.
        if (t->state == TASK_STATE_ZOMBIE) {
            bool has_parent = task_find_by_pid(t->parent_pid) != NULL;
            if (!has_parent) {
                debuglog(DEBUG_INFO, "[TASK] Reaping orphaned zombie PID %u (exit=%d)\n", t->id, t->exit_code);
                task_destroy(t);
            }
        }
        t = next;
    } while (t && t != ready_queue_head);
    spinlock_release(&task_scheduler_lock);
}

// Atomically read a zombie child's exit status and destroy it in one
// locked step, so the parent can never lose the race against a concurrent
// reap/destroy of the same task. Returns the exit code, or -1 if `pid`
// doesn't exist or hasn't exited yet (still runnable).
int32 task_reap_child(uint32 pid) {
    spinlock_acquire(&task_scheduler_lock);
    task_t* t = task_find_by_pid(pid);
    if (!t || (t->state != TASK_STATE_ZOMBIE && t->state != TASK_STATE_TERMINATED)) {
        spinlock_release(&task_scheduler_lock);
        return -1;
    }
    int32 exit_code = t->exit_code;
    task_destroy(t);
    spinlock_release(&task_scheduler_lock);
    return exit_code;
}

int32 task_wait_pid_any(int32* status_out) {
    if (!ready_queue_head || !current_task) {
        return -1;
    }
    uint32 parent_id = current_task->id;
    spinlock_acquire(&task_scheduler_lock);
    task_t* t = ready_queue_head;
    do {
        if (t->parent_pid == parent_id && t->state == TASK_STATE_ZOMBIE) {
            int32 code = t->exit_code;
            uint32 child_id = t->id;
            // Destroy while still holding the lock, in the same critical
            // section as the exit-code read (mirrors task_reap_child()).
            // Previously this released the lock and called the generic
            // task_reap_zombies() sweep to do the actual destroy, but that
            // sweep now intentionally skips zombies with a live parent (see
            // task_reap_zombies()) -- this task IS that live parent, so the
            // old code would never have actually reaped its own child.
            task_destroy(t);
            spinlock_release(&task_scheduler_lock);
            if (status_out) {
                *status_out = code;
            }
            return (int32)child_id;
        }
        t = t->next;
    } while (t && t != ready_queue_head);
    spinlock_release(&task_scheduler_lock);
    return -1;
}

int32 ipc_shm_create(const char* name, uint32 size) {
    if (!name || size == 0 || !current_task) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (!ipc_shm_regions[i].in_use) {
            uint32 pages = (size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
            uintptr_t phys = 0;
            bool ok = true;
            for (uint32 p = 0; p < pages; p++) {
                uint32 frame = pmm_alloc_frame();
                if (!frame) {
                    ok = false;
                    break;
                }
                if (p == 0) {
                    phys = frame;
                }
            }
            if (!ok) {
                spinlock_release(&ipc_lock);
                return -1;
                }
            ipc_shm_regions[i].id = ipc_next_shm_id++;
            strncpy(ipc_shm_regions[i].name, name, IPC_SHM_NAME_LEN - 1);
            ipc_shm_regions[i].name[IPC_SHM_NAME_LEN - 1] = '\0';
            ipc_shm_regions[i].owner_pid = current_task->id;
            ipc_shm_regions[i].size = pages * MEMORY_PAGE_SIZE;
            ipc_shm_regions[i].phys_addr = phys;
            ipc_shm_regions[i].ref_count = 1;
            ipc_shm_regions[i].in_use = true;
            int32 id = ipc_shm_regions[i].id;
            spinlock_release(&ipc_lock);
            return id;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_shm_open(const char* name) {
    if (!name) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (ipc_shm_regions[i].in_use && strcmp(ipc_shm_regions[i].name, name) == 0) {
            ipc_shm_regions[i].ref_count++;
            int32 id = ipc_shm_regions[i].id;
            spinlock_release(&ipc_lock);
            return id;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_shm_close(const char* name) {
    if (!name) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (ipc_shm_regions[i].in_use && strcmp(ipc_shm_regions[i].name, name) == 0) {
            if (ipc_shm_regions[i].ref_count > 0) {
                ipc_shm_regions[i].ref_count--;
            }
            if (ipc_shm_regions[i].ref_count == 0 &&
                ipc_shm_regions[i].owner_pid != (current_task ? current_task->id : 0)) {
                ipc_shm_regions[i].in_use = false;
                if (ipc_shm_regions[i].phys_addr) {
                    uint32 pages = ipc_shm_regions[i].size / MEMORY_PAGE_SIZE;
                    for (uint32 p = 0; p < pages; p++) {
                        pmm_free_frame(ipc_shm_regions[i].phys_addr + (p * MEMORY_PAGE_SIZE));
                    }
                }
            }
            spinlock_release(&ipc_lock);
            return 0;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_shm_destroy(const char* name) {
    if (!name || !current_task) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_SHM_REGIONS; i++) {
        if (ipc_shm_regions[i].in_use && strcmp(ipc_shm_regions[i].name, name) == 0) {
            if (ipc_shm_regions[i].owner_pid != current_task->id) {
                spinlock_release(&ipc_lock);
                return -1;
            }
            ipc_shm_regions[i].in_use = false;
            if (ipc_shm_regions[i].phys_addr) {
                uint32 pages = ipc_shm_regions[i].size / MEMORY_PAGE_SIZE;
                for (uint32 p = 0; p < pages; p++) {
                    pmm_free_frame(ipc_shm_regions[i].phys_addr + (p * MEMORY_PAGE_SIZE));
                }
            }
            spinlock_release(&ipc_lock);
            return 0;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_msg_create(const char* name, uint32 max_messages, uint32 msg_size) {
    if (!name || max_messages == 0 || msg_size == 0 || !current_task) {
        return -1;
    }
    if (max_messages > IPC_MAX_MESSAGES || msg_size > IPC_MSG_MAX_SIZE) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
        if (!ipc_msg_queues[i].in_use) {
            ipc_msg_queues[i].id = ipc_next_msg_id++;
            strncpy(ipc_msg_queues[i].name, name, IPC_SHM_NAME_LEN - 1);
            ipc_msg_queues[i].name[IPC_SHM_NAME_LEN - 1] = '\0';
            ipc_msg_queues[i].creator_pid = current_task->id;
            ipc_msg_queues[i].max_messages = max_messages;
            ipc_msg_queues[i].msg_size = msg_size;
            ipc_msg_queues[i].head = 0;
            ipc_msg_queues[i].tail = 0;
            ipc_msg_queues[i].count = 0;
            ipc_msg_queues[i].in_use = true;
            int32 id = ipc_msg_queues[i].id;
            spinlock_release(&ipc_lock);
            return id;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_msg_open(const char* name) {
    if (!name) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
        if (ipc_msg_queues[i].in_use && strcmp(ipc_msg_queues[i].name, name) == 0) {
            int32 id = ipc_msg_queues[i].id;
            spinlock_release(&ipc_lock);
            return id;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_msg_send(uint32 queue_id, const void* data, uint32 length) {
    if (!data || length == 0 || !current_task) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
        if (ipc_msg_queues[i].in_use && ipc_msg_queues[i].id == queue_id) {
            if (ipc_msg_queues[i].count >= ipc_msg_queues[i].max_messages) {
                spinlock_release(&ipc_lock);
                return -1;
            }
            ipc_message_t* msg = &ipc_msg_queues[i].messages[ipc_msg_queues[i].tail];
            msg->sender_pid = current_task->id;
            msg->type = 0;
            msg->length = length < IPC_MSG_MAX_SIZE ? length : IPC_MSG_MAX_SIZE;
            memory_copy((const char*)data, (char*)msg->data, msg->length);
            ipc_msg_queues[i].tail = (ipc_msg_queues[i].tail + 1) % IPC_MAX_MESSAGES;
            ipc_msg_queues[i].count++;
            spinlock_release(&ipc_lock);
            return 0;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_msg_receive(uint32 queue_id, void* buffer, uint32 buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
        if (ipc_msg_queues[i].in_use && ipc_msg_queues[i].id == queue_id) {
            if (ipc_msg_queues[i].count == 0) {
                spinlock_release(&ipc_lock);
                return -1;
            }
            ipc_message_t* msg = &ipc_msg_queues[i].messages[ipc_msg_queues[i].head];
            uint32 copy_len = msg->length < buffer_size ? msg->length : buffer_size;
            memory_copy((const char*)msg->data, (char*)buffer, copy_len);
            ipc_msg_queues[i].head = (ipc_msg_queues[i].head + 1) % IPC_MAX_MESSAGES;
            ipc_msg_queues[i].count--;
            spinlock_release(&ipc_lock);
            return (int32)copy_len;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

int32 ipc_msg_destroy(uint32 queue_id) {
    if (!current_task) {
        return -1;
    }
    spinlock_acquire(&ipc_lock);
    for (int i = 0; i < IPC_MAX_MSG_QUEUES; i++) {
        if (ipc_msg_queues[i].in_use && ipc_msg_queues[i].id == queue_id) {
            if (ipc_msg_queues[i].creator_pid != current_task->id) {
                spinlock_release(&ipc_lock);
                return -1;
            }
            ipc_msg_queues[i].in_use = false;
            spinlock_release(&ipc_lock);
            return 0;
        }
    }
    spinlock_release(&ipc_lock);
    return -1;
}

void task_graphics_watchdog_check(void) {
    if (!ready_queue_head) {
        return;
    }

    uint32 now = timer_get_ticks();
    task_t* t = ready_queue_head;

    do {
        if (t && t->is_graphics_task &&
            (t->state == TASK_STATE_READY || t->state == TASK_STATE_WAITING) &&
            t->last_active_tick > 0 &&
            (now - t->last_active_tick) >= GRAPHICS_WATCHDOG_TICKS) {

            if (t->state == TASK_STATE_WAITING) {
                t->state = TASK_STATE_READY;
            }

            if (t->priority < TASK_PRIORITY_GUI) {
                t->original_priority = t->priority;
                t->priority = TASK_PRIORITY_GUI;
                t->boost_expires_at = now + TASK_PRIORITY_BOOST_TICKS;
            }

            debuglog(DEBUG_WARN, "[TASK] Graphics watchdog: forcing schedule for '%s' (PID %u, last_active=%u, now=%u)\n",
                     t->name, t->id, t->last_active_tick, now);
        }
        t = t->next;
    } while (t && t != ready_queue_head);
}

/*
 * Fern - Cross-Architecture Task Scheduler
 * src/arch/task.c
 *
 * Implements a portable round-robin task scheduler that works across all
 * supported architectures (x86_32, x86_64, arm32, aarch64, riscv64).
 *
 * The scheduler maintains a circular doubly-linked ready queue and uses a
 * simple time-sliced round-robin policy.  Context switching is delegated
 * to arch-specific assembly (task_switch_asm) via the dispatch wrapper
 * arch_context_switch().
 *
 * This file deliberately avoids any architecture-specific intrinsics or
 * assumptions about register width -- all pointer-width-dependent values
 * use arch_word_t / arch_vaddr_t from arch.h.
 */

#include "task.h"
#include "session.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel headers (provide kmalloc, spinlock, timer, VMM, PMM, etc.) */
#include "../include/spinlock.h"
#include "../include/timer.h"
#include "../include/debuglog.h"
#include "../include/string.h"

/* Memory allocation -- provided by the kernel heap (mm.c / kmalloc.c) */
extern void* kmalloc(uint32_t size);
extern void* kmalloc_aligned(uint32_t size, uint32_t alignment);
extern void  kfree(void* ptr);

/* Physical / virtual memory management */
extern uint32_t pmm_alloc_frame(void);
extern void     pmm_free_frame(uint32_t frame);
extern void*    vmm_get_current_page_directory(void);
extern void     vmm_set_current_directory(void* dir);

/* Memory utilities */
extern void memory_set(uint8_t* dest, uint8_t val, uint32_t count);

/* ELF loader (arch-specific, provided by the kernel) */
typedef struct {
    bool        valid;
    uint32_t    entry_point;
    uint32_t    base_address;
    uint32_t    total_size;
    void*       page_directory; /* physical address / arch page table root */
} elf_load_info_t;
extern int elf_load_executable(const uint8_t* data, uint32_t size, elf_load_info_t* out);

/* GDT / TSS update (arch-specific, no-op on non-x86) */
extern void gdt_set_kernel_stack(arch_word_t stack_top);

/* ---- constants -------------------------------------------------------- */

#undef KERNEL_STACK_SIZE
#define KERNEL_STACK_SIZE    8192   /* 8 KB per task */
#define DEFAULT_TIME_SLICE  10     /* ticks per round-robin quantum */

/* ---- globals ---------------------------------------------------------- */

task_t* current_task     = NULL;
task_t* ready_queue_head = NULL;

static uint32_t next_pid      = 1;
static spinlock_t sched_lock  = SPINLOCK_INIT("sched");

/* ---- forward declarations --------------------------------------------- */

static void arch_context_switch(task_t* prev, task_t* next);

/* ========================================================================
 *  Ready queue operations (circular doubly-linked list)
 * ======================================================================== */

static void queue_insert_tail(task_t* task) {
    if (!ready_queue_head) {
        ready_queue_head  = task;
        task->next        = task;
        task->prev        = task;
    } else {
        task_t* tail = ready_queue_head->prev;
        tail->next    = task;
        task->prev    = tail;
        task->next    = ready_queue_head;
        ready_queue_head->prev = task;
    }
}

static void queue_remove(task_t* task) {
    if (task->next == task) {
        /* Only element */
        ready_queue_head = NULL;
    } else {
        task->prev->next = task->next;
        task->next->prev = task->prev;
        if (ready_queue_head == task) {
            ready_queue_head = task->next;
        }
    }
    task->next = NULL;
    task->prev = NULL;
}

/* ========================================================================
 *  Arch-specific context switch dispatch
 * ======================================================================== */

/*
 * arch_context_switch - Perform the low-level register save/restore.
 *
 * Each architecture provides task_switch_asm in its own assembly files.
 * The prototypes differ because each ABI has different callee-saved
 * registers and page-table root representation:
 *
 *   x86_32/x86_64:  void task_switch_asm(uintptr_t* old_sp, uintptr_t new_sp,
 *                                         uintptr_t new_cr3);
 *   aarch64:         void task_switch_asm(uint64_t* old_sp, uint64_t new_sp,
 *                                         void* old_fpu, void* new_fpu);
 *   arm32:           void task_switch_asm(uint32_t* old_sp, uint32_t new_sp,
 *                                         void* new_ttbr);
 *   riscv64:         void task_switch_asm(uint64_t* old_sp, uint64_t new_sp,
 *                                         uint64_t new_satp);
 *
 * The dispatch below uses arch_word_t for stack pointers (correct on all
 * targets) and an arch-appropriate type for the page-table root.
 */

#if ARCH_X86_32 || ARCH_X86_64
extern void task_switch_asm(arch_word_t* old_sp, arch_word_t new_sp,
                            uintptr_t new_cr3);
static inline void arch_context_switch(task_t* prev, task_t* next) {
    (void)prev;
    (void)next;
    /* Page-table root for x86 is the physical address of the PML4/PD. */
    uintptr_t new_cr3 = (uintptr_t)next->page_directory;
    task_switch_asm(&prev->kernel_sp, next->kernel_sp, new_cr3);
}
#elif ARCH_ARM64
extern void task_switch_asm(uint64_t* old_sp, uint64_t new_sp,
                            void* old_fpu, void* new_fpu);
static inline void arch_context_switch(task_t* prev, task_t* next) {
    /* AArch64 uses TTBR0_EL1 for the user page table and keeps the kernel
     * mappings in TTBR1.  The VMM layer manages TTBR0; here we just pass
     * the arch_priv pointer for FPU/SIMD context save/restore. */
    task_switch_asm((uint64_t*)&prev->kernel_sp, next->kernel_sp,
                    prev->arch_priv, next->arch_priv);
}
#elif ARCH_ARM32
extern void task_switch_asm(uint32_t* old_sp, uint32_t new_sp,
                            void* new_ttbr);
static inline void arch_context_switch(task_t* prev, task_t* next) {
    task_switch_asm((uint32_t*)&prev->kernel_sp, next->kernel_sp,
                    next->page_directory);
}
#elif ARCH_RISCV64
extern void task_switch_asm(uint64_t* old_sp, uint64_t new_sp,
                            uint64_t new_satp);
static inline void arch_context_switch(task_t* prev, task_t* next) {
    task_switch_asm((uint64_t*)&prev->kernel_sp, next->kernel_sp,
                    (uint64_t)next->page_directory);
}
#else
#error "task.c: no context-switch implementation for this architecture"
#endif

/* ========================================================================
 *  Task creation helpers
 * ======================================================================== */

static task_t* allocate_task(void) {
    task_t* t = (task_t*)kmalloc(sizeof(task_t));
    if (!t) {
        debuglog(DEBUG_ERROR, "[TASK] kmalloc failed for task struct\n");
        return NULL;
    }
    memory_set((uint8_t*)t, 0, sizeof(task_t));
    return t;
}

static void setup_kernel_stack(task_t* t, void (*entry)(void)) {
    /*
     * Allocate a kernel stack and build an initial frame so that the first
     * context-switch into this task drops into @entry.
     *
     * The exact frame layout is arch-specific and is normally built by
     * prepare_kernel_task_stack() in the arch-specific task.c.  For the
     * cross-arch stub we record the stack bounds and let the arch layer
     * handle the rest.  A fully functional build would call the arch's
     * stack-preparation routine here.
     */
    void* stack = kmalloc_aligned(KERNEL_STACK_SIZE, 4096);
    if (!stack) {
        debuglog(DEBUG_ERROR, "[TASK] kernel stack alloc failed for '%s'\n", t->name);
        return;
    }

    t->kernel_stack_base = (arch_word_t)stack;
    t->kernel_stack_size = KERNEL_STACK_SIZE;
    /* Top of stack = base + size (stacks grow downward on all supported archs) */
    t->kernel_sp = (arch_word_t)stack + KERNEL_STACK_SIZE;

    /*
     * Arch-specific initial frame setup.
     *
     * Each architecture's linker script / assembly provides a
     * prepare_task_frame() or equivalent that writes the correct
     * register save area onto the stack.  We call it here if available.
     *
     * For x86 this pushes an IRET frame + callee-saved GPRs.
     * For ARM64 it fills an exception-context struct.
     * For RISC-V it writes sepc + callee-saved regs.
     *
     * When building without the arch-specific assembly, the task will
     * start at whatever address is at the top of the stack (likely 0),
     * which is fine for testing the scheduler plumbing.
     */
#if ARCH_X86_32 || ARCH_X86_64
    /* x86: The arch layer's task_switch_asm expects a specific frame layout.
     * For a minimal kernel task we set up a return-to-entry frame. */
    arch_word_t* sp = (arch_word_t*)(t->kernel_sp);
    *(--sp) = 0;                     /* padding / alignment */
    *(--sp) = (arch_word_t)entry;    /* return address for RET */
    t->kernel_sp = (arch_word_t)sp;
#endif
    /* ARM64 / ARM32 / RISC-V: arch assembly handles initial frame. */
}

/* ========================================================================
 *  Public API
 * ======================================================================== */

void task_init(void) {
    debuglog(DEBUG_INFO, "[TASK] Initializing cross-arch scheduler\n");

    /* Initialize session/process group subsystem */
    session_init();

    /* Create the idle task (runs when nothing else is runnable) */
    task_t* idle = allocate_task();
    if (!idle) {
        debuglog(DEBUG_ERROR, "[TASK] Failed to create idle task\n");
        return;
    }

    memory_set((uint8_t*)idle->name, 0, TASK_NAME_MAX);
    /* name = "idle" without pulling in the full string library */
    idle->name[0] = 'i';
    idle->name[1] = 'd';
    idle->name[2] = 'l';
    idle->name[3] = 'e';

    idle->pid       = next_pid++;
    idle->ppid      = 0;
    idle->state     = TASK_STATE_READY;
    idle->priority  = TASK_PRIORITY_MIN;
    idle->time_slice = DEFAULT_TIME_SLICE;
    idle->ticks_used = 0;
    idle->page_directory = vmm_get_current_page_directory();
    idle->created_tick   = timer_get_ticks();
    idle->last_scheduled_tick = idle->created_tick;

    setup_kernel_stack(idle, NULL);

    /* Current task is the boot task -- wrap it in a task_t */
    task_t* boot = allocate_task();
    if (!boot) {
        debuglog(DEBUG_ERROR, "[TASK] Failed to create boot task\n");
        return;
    }

    memory_set((uint8_t*)boot->name, 0, TASK_NAME_MAX);
    boot->name[0] = 'b';
    boot->name[1] = 'o';
    boot->name[2] = 'o';
    boot->name[3] = 't';

    boot->pid           = next_pid++;
    boot->ppid          = 0;
    boot->state         = TASK_STATE_RUNNING;
    boot->priority      = TASK_PRIORITY_DEFAULT;
    boot->time_slice    = DEFAULT_TIME_SLICE;
    boot->ticks_used    = 0;
    boot->page_directory = vmm_get_current_page_directory();
    boot->created_tick   = timer_get_ticks();
    boot->last_scheduled_tick = boot->created_tick;

    /* Build the ready queue: boot -> idle -> boot (circular) */
    boot->next  = idle;
    boot->prev  = idle;
    idle->next  = boot;
    idle->prev  = boot;
    ready_queue_head = boot;
    current_task = boot;

    /* Assign boot task to the initial session (session 1) */
    boot->session_id     = 1;
    boot->pgrp_id        = 1;
    boot->session_leader = true;
    session_create(1);
    process_group_create(1, 1);
    process_group_add(1, boot->pid);

    debuglog(DEBUG_INFO, "[TASK] Scheduler ready  boot_pid=%u  idle_pid=%u\n",
             boot->pid, idle->pid);
}

task_t* task_create(const char* name, void* entry, uint32_t flags) {
    (void)flags;

    if (!name || !entry) {
        return NULL;
    }

    task_t* t = allocate_task();
    if (!t) {
        return NULL;
    }

    /* Copy name (bounded) */
    uint32_t i;
    for (i = 0; i < TASK_NAME_MAX - 1 && name[i]; i++) {
        t->name[i] = name[i];
    }
    t->name[i] = '\0';

    t->pid           = next_pid++;
    t->ppid          = current_task ? current_task->pid : 0;
    t->state         = TASK_STATE_READY;
    t->priority      = TASK_PRIORITY_DEFAULT;
    t->time_slice    = DEFAULT_TIME_SLICE;
    t->ticks_used    = 0;
    t->page_directory = current_task ? current_task->page_directory
                                     : vmm_get_current_page_directory();
    t->entry_point   = (arch_vaddr_t)entry;
    t->exit_code     = 0;
    t->created_tick  = timer_get_ticks();
    t->last_scheduled_tick = t->created_tick;

    /* Inherit session and process group from parent */
    if (current_task) {
        t->session_id     = current_task->session_id;
        t->pgrp_id        = current_task->pgrp_id;
        t->session_leader = false;
        /* Add to parent's process group if one exists */
        if (t->pgrp_id != 0) {
            process_group_add(t->pgrp_id, t->pid);
        }
    } else {
        t->session_id     = 0;
        t->pgrp_id        = 0;
        t->session_leader = false;
    }

    setup_kernel_stack(t, (void (*)(void))entry);

    /* Insert into ready queue */
    spinlock_acquire(&sched_lock);
    queue_insert_tail(t);
    spinlock_release(&sched_lock);

    debuglog(DEBUG_INFO, "[TASK] Created '%s' pid=%u entry=0x%x\n",
             t->name, t->pid, (uint32_t)t->entry_point);
    return t;
}

task_t* task_create_elf(const uint8_t* elf_data, uint32_t elf_size) {
    if (!elf_data || elf_size == 0) {
        return NULL;
    }

    elf_load_info_t elf_info;
    memory_set((uint8_t*)&elf_info, 0, sizeof(elf_info));

    int status = elf_load_executable(elf_data, elf_size, &elf_info);
    if (status != 0 || !elf_info.valid || elf_info.entry_point == 0) {
        debuglog(DEBUG_ERROR, "[TASK] ELF load failed (status=%d)\n", status);
        return NULL;
    }

    task_t* t = allocate_task();
    if (!t) {
        return NULL;
    }

    /* Derive name from ELF info if available, else generic */
    t->name[0] = 'e';
    t->name[1] = 'l';
    t->name[2] = 'f';
    t->name[3] = '\0';

    t->pid           = next_pid++;
    t->ppid          = current_task ? current_task->pid : 0;
    t->state         = TASK_STATE_READY;
    t->priority      = TASK_PRIORITY_DEFAULT;
    t->time_slice    = DEFAULT_TIME_SLICE;
    t->ticks_used    = 0;
    t->page_directory = elf_info.page_directory;
    t->entry_point   = elf_info.entry_point;
    t->exit_code     = 0;
    t->created_tick  = timer_get_ticks();
    t->last_scheduled_tick = t->created_tick;

    /* Inherit session and process group from parent */
    if (current_task) {
        t->session_id     = current_task->session_id;
        t->pgrp_id        = current_task->pgrp_id;
        t->session_leader = false;
        /* Add to parent's process group if one exists */
        if (t->pgrp_id != 0) {
            process_group_add(t->pgrp_id, t->pid);
        }
    } else {
        t->session_id     = 0;
        t->pgrp_id        = 0;
        t->session_leader = false;
    }

    /* Kernel stack for the new task */
    setup_kernel_stack(t, NULL);

    /* Insert into ready queue */
    spinlock_acquire(&sched_lock);
    queue_insert_tail(t);
    spinlock_release(&sched_lock);

    debuglog(DEBUG_INFO, "[TASK] Created ELF task pid=%u entry=0x%x\n",
             t->pid, (uint32_t)t->entry_point);
    return t;
}

void task_schedule(void) {
    spinlock_acquire(&sched_lock);

    if (!ready_queue_head) {
        spinlock_release(&sched_lock);
        return;
    }

    /* Advance to next task */
    task_t* prev = current_task;
    task_t* next = prev ? prev->next : ready_queue_head;

    /* Skip tasks that are not runnable (zombie / terminated / waiting) */
    uint32_t guard = 0;
    while (next &&
           next->state != TASK_STATE_READY &&
           next->state != TASK_STATE_RUNNING) {
        next = next->next;
        if (++guard > 1024) {
            /* Sanity: avoid infinite loop on corrupted queue */
            debuglog(DEBUG_WARN, "[TASK] Ready queue scan guard hit\n");
            break;
        }
    }

    if (!next || next == prev) {
        /* No switch needed */
        if (next && next->state == TASK_STATE_READY) {
            next->state = TASK_STATE_RUNNING;
        }
        spinlock_release(&sched_lock);
        return;
    }

    /* Update state */
    if (prev && prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }
    next->state = TASK_STATE_RUNNING;
    next->last_scheduled_tick = timer_get_ticks();
    next->ticks_used = 0;

    current_task = next;

    /* Update TSS / kernel stack pointer for privilege transitions (x86) */
    gdt_set_kernel_stack(next->kernel_stack_base + next->kernel_stack_size);

    spinlock_release(&sched_lock);

    /* Perform the actual context switch */
    arch_context_switch(prev, next);
}

void task_switch_to(task_t* next) {
    if (!next || next == current_task) {
        return;
    }

    spinlock_acquire(&sched_lock);

    task_t* prev = current_task;
    if (prev && prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }

    next->state = TASK_STATE_RUNNING;
    next->last_scheduled_tick = timer_get_ticks();
    next->ticks_used = 0;
    current_task = next;

    gdt_set_kernel_stack(next->kernel_stack_base + next->kernel_stack_size);

    spinlock_release(&sched_lock);

    arch_context_switch(prev, next);
}

void task_exit(int32_t code) {
    if (!current_task) {
        return;
    }

    debuglog(DEBUG_INFO, "[TASK] PID %u exiting with code %d\n",
             current_task->pid, code);

    current_task->exit_code = code;
    current_task->state = TASK_STATE_ZOMBIE;

    /* Clean up session and process group membership */
    session_remove_task(current_task->pid);

    /* Reparent children to PID 1 (init) */
    task_t* t = ready_queue_head;
    if (t) {
        do {
            if (t->ppid == current_task->pid) {
                t->ppid = 1;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }

    /* Reschedule -- this task will not be picked again until reaped */
    task_schedule();

    /* Should never reach here */
    for (;;) {
        arch_halt();
    }
}

void task_yield(void) {
    task_schedule();
}

task_t* task_find_by_pid(uint32_t pid) {
    if (!ready_queue_head) {
        return NULL;
    }

    task_t* t = ready_queue_head;
    do {
        if (t->pid == pid) {
            return t;
        }
        t = t->next;
    } while (t != ready_queue_head);

    return NULL;
}

void task_destroy(task_t* task) {
    if (!task) {
        return;
    }

    spinlock_acquire(&sched_lock);

    /* Remove from queue */
    queue_remove(task);

    /* Free kernel stack */
    if (task->kernel_stack_base) {
        kfree((void*)task->kernel_stack_base);
    }

    /* Free arch-private data (FPU context, etc.) */
    if (task->arch_priv) {
        kfree(task->arch_priv);
    }

    spinlock_release(&sched_lock);

    debuglog(DEBUG_INFO, "[TASK] Destroyed PID %u\n", task->pid);
    kfree(task);
}

# Process Management in Fern (Forest OS Kernel)

Fern, the Forest OS kernel, implements a fairly traditional Unix-like process management system. This document walks through every major subsystem -- from the data structures that describe a task, to the scheduler that decides who runs, to the lifecycle of process creation, execution, and death.

All the source code lives under `fern/src/` and its headers in `fern/src/include/`. The core files are:

| File | Purpose |
|------|---------|
| `task.c` / `task.h` | Task structures, creation, scheduling, signals, wait/exit |
| `thread.c` / `thread.h` | Kernel threading layer built on top of tasks |
| `job_control.c` / `job_control.h` | POSIX job control (fg/bg, SIGWINCH) |
| `elf.c` / `elf.h` | ELF binary loading |
| `context_switch.asm` | Low-level register save/restore and IRET to userspace |
| `smp.c` / `smp.h` | Multi-core (SMP) discovery and initialization |

---

## 1. The Task Data Structure

Everything in Fern's process model revolves around `task_t`, defined in `fern/src/include/task.h`. There is no separate "process" structure -- a task *is* a process, whether it runs in kernel mode or user mode.

```c
typedef struct task {
    char name[32];                  // Human-readable name (e.g. "shell", "idle")
    uint32 id;                      // Unique PID (monotonically assigned)
    uint32 pgrp;                    // Process group ID (for job control)
    uint32 session;                 // Session ID (for terminal ownership)
    int32 tty_fd;                   // File descriptor of controlling TTY (-1 if none)
    task_state_t state;             // Current scheduling state

    uintptr_t kernel_stack;         // Saved kernel SP (where context_switch reads from)
    uintptr_t kernel_stack_base;    // Base of the allocated kernel stack (8 KB)
    page_directory_t* page_directory; // Physical address of this task's page directory (CR3 value)

    elf_load_info_t elf_info;       // ELF loading metadata (entry point, segment layout, etc.)

    uintptr_t user_heap_base;       // User-mode heap start
    uintptr_t user_heap_limit;      // User-mode heap ceiling
    uintptr_t user_brk;             // Current brk (heap top)

    int32 exit_code;                // Exit status (set on death, read by parent's wait())
    char exit_reason[32];           // Optional human-readable exit reason

    uint32 uid, gid;                // POSIX credentials
    uint32 groups_mask;             // Supplementary groups bitmask

    uint32 priority;                // Base scheduling priority (0-7)
    uint32 ticks_left;              // Remaining time quantum (unused in current scheduler)
    uint32 pending_signals;         // Bitmask of pending signals
    uint32 sleep_until_tick;        // Wake-up tick (for sleep_interruptible)
    uint32 last_active_tick;        // Last tick this task was scheduled (for ps/top)

    uint64 cpu_ticks_total;         // Cumulative CPU time in ticks (for %CPU calculation)
    uint32 scheduled_at_tick;       // Tick when last switched in
    uint32 created_at_tick;         // Tick when task was born

    uint32 original_priority;       // Priority before any temporary boost
    uint32 boost_expires_at;        // Tick at which priority boost expires

    bool watchdog_enabled;          // Graphics watchdog (for unresponsive GUI tasks)
    uint32 consecutive_timeouts;

    bool is_background;             // Background job (gets SIGTTIN/SIGTTOU)
    bool has_framebuffer_mapping;   // Mapped the display framebuffer
    bool is_graphics_task;          // Explicitly marked as a GUI task
    bool is_protected;              // Cannot be killed (kernel/idle tasks)

    uint32 memory_quota;            // Memory limit (0 = unlimited)
    uint32 memory_used;             // Current memory consumption

    char cwd[256];                  // Current working directory path
    uint32 signal_mask;             // Blocked signals bitmask
    sigaction_t signal_handlers[32]; // Per-signal dispositions (SIG_IGN/SIG_DFL/handler)

    uint32 parent_pid;              // PID of parent process
    int32 vt_index;                 // Virtual terminal index (-1 if none)

    bool needs_usermode_entry;      // True for freshly-created user tasks (first IRET)
    uintptr_t usermode_entry_point; // ELF entry point (for initial switch frame)
    uintptr_t usermode_stack_top;   // User stack top (for initial switch frame)

    void* waiting_semaphore;        // Semaphore this task is blocked on (or NULL)
    void* thread_wrapper;           // Back-pointer to struct thread (if created via thread_create)
    void* vfp_context;              // FPU/SSE state (for context switching)

    struct task* next;              // Next task in the circular ready queue
    struct task* next_in_pgrp;      // (Reserved) next task in same process group
} task_t;
```

Key design notes:

- **No separate `task_struct` and `thread_struct`**. Unlike Linux (which has `task_struct` for threads and `thread_info` for per-CPU data), Fern uses one flat struct for everything. The `thread_wrapper` field bridges to `struct thread` when kernel threads are created via the threading API.
- **The ready queue is a circular singly-linked list** threaded through `task->next`. There are no separate run-queue arrays per priority -- priority affects *selection*, not queue placement.
- **`is_protected`** prevents the kernel, idle, and init tasks from being killed. The panic handler and fault logic check this to decide whether to crash the whole system or just the offending task.

---

## 2. Process States

Fern tracks six process states:

```c
typedef enum {
    TASK_STATE_RUNNING,     // Currently executing on a CPU
    TASK_STATE_READY,       // Runnable, waiting for CPU time
    TASK_STATE_WAITING,     // Blocked (sleeping, waiting for I/O, semaphore, etc.)
    TASK_STATE_TERMINATED,  // Dead but not yet reaped (similar to zombie)
    TASK_STATE_ZOMBIE,      // Dead, awaiting parent's wait()
    TASK_STATE_SUSPENDED    // Stopped by signal (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU)
} task_state_t;
```

The transitions look like this:

```
  task_create_*()
        |
        v
     [WAITING] ----timer/schedule----> [READY] ----picked by scheduler----> [RUNNING]
        ^                                    ^                                    |
        |                                    |                                    |
  sleep_interruptible()           signal wakes it up                    task_schedule()
  semaphore_wait()                                                           |
        |                                                                    v
     [WAITING] <--------- I/O complete ---------- [WAITING]         [ZOMBIE] or [SUSPENDED]
                                                                    (on exit or SIGSTOP)
```

A zombie task has finished execution but its parent hasn't called `waitpid()` yet. When the parent does (via `task_reap_child()`), or when the parent dies (orphaned zombies), the task is fully destroyed and its memory freed.

Suspended tasks (from SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU) are not considered runnable by the scheduler until a `SIGCONT` or `task_resume()` moves them back to READY.

---

## 3. Process Creation (fork)

Fern implements `fork()` via the `task_clone_current()` function in `task.c:1322`. There is no separate `clone()` syscall with flags -- `task_clone_current()` always does a full copy.

Here's what happens:

1. **Allocate a new `task_t`** from the kernel heap.
2. **Copy the parent's task structure** wholesale with `memory_copy()` (a byte-for-byte memcpy).
3. **Assign a new PID** from the global `next_task_id` counter.
4. **Allocate a fresh kernel stack** and copy the parent's kernel stack contents. This carries over the live syscall frame so the child returns to userspace at the same instruction the parent called `fork()`.
5. **Fix up the child's return value**: patch the saved `EAX` in the child's syscall frame to 0 (the child's fork() return value).
6. **Create a COW (Copy-on-Write) address space** via `cow_fork_address_space()`. Instead of copying all physical pages immediately, both parent and child share the same pages marked read-only. The first write to any page triggers a page fault, which then allocates a fresh copy. This is a significant optimization over a naive full copy.
7. **Set child-specific fields**: new PID, parent PID pointing to the caller, empty signal mask, reset timers, copy process group and session.
8. **Append the child to the circular ready queue** under the scheduler spinlock.
9. **Return the child PID** to the parent (the child will see 0 when it resumes in userspace).

The assembly trampoline in `context_switch.asm` ensures the child's first execution lands on `isr128_resume`, which restores the syscall frame and does `IRET` back to userspace -- identical to how a normal syscall returns, except EAX=0.

```c
// Simplified from task.c:1322
task_t* task_clone_current(void) {
    task_t* child = kmalloc(sizeof(task_t));
    memory_copy(current_task, child, sizeof(task_t));  // Whole-struct copy

    child->id = next_task_id++;
    child->state = TASK_STATE_READY;
    child->parent_pid = current_task->id;
    child->pgrp = current_task->pgrp;     // Inherit process group
    child->session = current_task->session; // Inherit session

    // COW fork of the address space
    page_directory_t* child_pd = cow_fork_address_space(current_task->page_directory);
    child->page_directory = child_pd ? child_pd : current_task->page_directory;

    // ... (kernel stack copy, frame patching, queue insertion)
    return child;
}
```

---

## 4. Program Execution (execve, ELF Loading)

When a process calls `execve()`, the kernel loads a new ELF binary into the process's address space, replacing the current program entirely. The implementation lives in `elf.c` and is invoked through `task_create_elf()`.

### ELF Loading Pipeline

1. **Validate the ELF header**: Check magic bytes, class (32-bit), endianness (little-endian), machine type (i386), and that it's either `ET_EXEC` or `ET_DYN` (PIE executables).

2. **Create a fresh page directory**: `vmm_create_page_directory()` gives the new process a clean virtual address space. The kernel PDEs are synced in so kernel heap pages remain accessible.

3. **Map loadable segments**: Walk the ELF program headers. For each `PT_LOAD` segment, allocate physical frames and map them into the new page directory at the segment's virtual address with appropriate permissions (read/write/exec bits from `p_flags`).

4. **Handle BSS**: Segments with `p_memsz > p_filesz` have a BSS region that gets zeroed pages.

5. **Map the user stack**: 32 pages (128 KB) are allocated and mapped at the top of user space (`USER_STACK_TOP`), growing downward.

6. **Map the user heap**: A heap region is established just below the stack, with a guard page gap to catch stack/heap collisions.

7. **Build the initial kernel stack frame**: `setup_initial_cpu_state()` pre-constructs an IRET frame on the kernel stack that, when `task_switch_asm` restores it, will drop the CPU into ring 3 at the ELF entry point with the user stack pointer.

8. **Enter the ready queue**: The new task is linked into the circular ready queue and will be picked up by the next scheduler tick.

### The crt0 Bootstrap

User programs link against `userspace/crt0.S`, which provides the `_start` entry point:

```asm
_start:
    /* argc, argv, envp already on the stack from the kernel */
    call main
    pushl %eax
    call exit
    hlt
```

The userspace linker script (`userspace/link.ld`) places the binary at `0x08048000` (classic Linux ELF base address). The kernel's ELF loader reads the binary's own segment addresses, so this is mainly a hint for the linker -- the actual load address comes from the ELF program headers.

---

## 5. Process Termination (exit, wait)

### Exiting

When a process finishes (either by calling `exit()`, receiving a fatal signal, or returning from `main()`), it goes through `task_exit()` or `task_terminate_current()`:

1. **Set exit code** and optional reason string.
2. **Release resources**: close framebuffer mappings, close network sockets, close all file descriptors (via `syscall_close_all_fds_for_task()`), detach IPC shared memory, clean up epoll/eventfd/inotify handles.
3. **Reparent children** to PID 1 (init) via `task_reparent_children()`. This ensures no process becomes permanently orphaned without a parent to reap it.
4. **Send SIGCHLD** to the parent process so `wait()` wakes up.
5. **Transition to ZOMBIE state** and call `task_schedule()`. The task remains in the ready queue as a zombie until reaped.

```c
void task_exit(int code, const char* reason) {
    current_task->exit_code = code;
    // ... (resource cleanup)
    current_task->state = TASK_STATE_ZOMBIE;
    task_send_signal(current_task->parent_pid, SIGCHLD);
    task_schedule();  // Never returns
    while(1) hlt;    // Safety net
}
```

### Waiting

The parent calls `task_wait_pid()` (which backs the `wait4` syscall). It busy-polls the target PID:

1. Call `task_reap_child(pid)` which atomically checks if the child is zombie, reads its exit code, and destroys it.
2. If not yet dead, set the parent's state to WAITING and yield (`task_schedule()`).
3. Wake up on the next timer tick and try again.

Orphaned zombies (whose parent has already died) are automatically reaped by `task_reap_zombies()`, which runs at the top of every `task_schedule()` call. Only orphans are reaped automatically -- zombies with a live parent are left alone so the parent can consume the exit status.

---

## 6. The Scheduler

Fern uses a **priority-aware round-robin scheduler** with a twist: GUI and graphics tasks get preferential treatment.

### How It Works

The scheduler runs inside `task_schedule()`, which is called from:
- The timer interrupt handler (preemptive scheduling)
- `sleep_interruptible()` (voluntary yield)
- `task_yield()` (explicit yield)
- `task_wait_pid()` (waiting for a child)

On each tick, the scheduler:

1. **Processes deferred cleanup** -- tasks that couldn't be freed immediately (e.g., because their kernel stack was still in use).
2. **Reaps orphaned zombies** -- automatically destroys zombies whose parent no longer exists.
3. **Wakes sleeping tasks** -- any task whose `sleep_until_tick` has elapsed transitions from WAITING to READY.
4. **Checks for fatal pending signals** -- SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGKILL on the current task cause immediate termination.
5. **Picks the next task** using this priority order:
   - **Graphics tasks** (tasks with `is_graphics_task`, `has_framebuffer_mapping`, or that are the foreground task) get top priority. The scheduler picks the one with the highest effective priority.
   - **Foreground task** -- if set, the shell can hand scheduling priority to whichever job has the terminal (via `SYS_SET_FOREGROUND_TASK`).
   - **Round-robin** -- if no foreground/graphics task is runnable, the scheduler walks the circular ready queue from the current task's `next` pointer, skipping WAITING/ZOMBIE/SUSPENDED tasks.

### Priority System

Fern defines 8 priority levels (0-7):

```c
#define TASK_PRIORITY_MAX          7
#define TASK_PRIORITY_GUI          6
#define TASK_PRIORITY_REALTIME     7
#define TASK_PRIORITY_NORMAL       3  (default for new tasks)
```

The effective priority (`task_get_real_priority()`) can be temporarily boosted:
- Tasks that have mapped the framebuffer or are graphics tasks get a floor of `TASK_PRIORITY_GUI` (6).
- A transient boost (`boost_expires_at`) adds +2 for `TASK_PRIORITY_BOOST_TICKS` ticks (50 ticks = 5 seconds at 100 Hz).

The tick quantum is `2 + (priority * 2)`, so higher-priority tasks get more CPU time per scheduling slice.

### The Idle Task

When no runnable tasks exist, the scheduler falls back to the idle task, which executes `sti; hlt` in a loop -- halting the CPU until the next interrupt. This saves power on real hardware.

---

## 7. Context Switching

Context switching is the heart of the scheduler. It happens in `task_switch()` (`task.c:1454`) and the assembly routine `task_switch_asm()` (`context_switch.asm`).

### The C Part (`task_switch`)

Before touching any registers:

1. **Save interrupt state** -- record whether IF (interrupt flag) was set, and disable interrupts if so.
2. **Account CPU time** -- credit the outgoing task with ticks since it was last scheduled in.
3. **Validate the target** -- check page directory, kernel stack, and (for fresh user tasks) verify the entire trampoline path is mapped.
4. **Sync kernel PDEs** -- ensure the target task's page directory can see all kernel heap pages.
5. **Ensure kernel stack is mapped** -- repair any missing mappings for the target's kernel stack.
6. **Update TSS** -- set the kernel stack pointer for interrupt returns (`gdt_set_kernel_stack()`).
7. **Switch CR3** -- write the new page directory's physical address to the CR3 register.
8. **Call `task_switch_asm()`** -- the assembly routine does the actual register swap.

### The Assembly Part (`task_switch_asm`)

On x86-32:
```asm
task_switch_asm:
    push ebp
    mov  ebp, esp
    pushf           ; save EFLAGS
    pusha           ; save EAX ECX EDX EBX ESP EBP ESI EDI

    ; Save current ESP to *old_sp_ptr
    mov  eax, [ebp + 8]
    mov  [eax], esp

    ; Load new ESP
    mov  esp, [ebp + 12]

    ; Switch CR3
    mov  cr3, [ebp + 16]

    ; Restore new task's context
    popa
    popf
    pop  ebp
    ret             ; jumps to new task's saved return address
```

For new user tasks, the "return address" on the kernel stack is `task_start_usermode_asm`, which sets the data segment registers and executes `IRET` to drop into ring 3 at the ELF entry point. For returning tasks, the return address is wherever they last called `task_switch_asm`.

On x86-64, the same pattern applies but saves/restores 14 general-purpose registers (rax, rcx, rdx, rbx, rsi, rdi, r8-r15) and uses `IRETQ`.

---

## 8. Signal Handling

Fern implements a subset of POSIX signals. The signal infrastructure is split between `task.c` (delivery logic) and `syscall.c` (the `sys_rt_sigaction` / `sys_rt_sigprocmask` syscalls).

### Signal Numbers

All 31 standard POSIX signals are defined (`SIGHUP` through `SIGSYS`), but the kernel only acts on a subset. The fatal signals (SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGKILL) are checked on every scheduler tick.

### Pending Signals

Each task has a `pending_signals` bitmask (one bit per signal) and a `signal_mask` bitmask (blocked signals). A signal is delivered if:

```c
uint32 unblocked = task->pending_signals & ~task->signal_mask;
```

### Signal Dispositions

The `signal_handlers[32]` array holds per-signal dispositions:

```c
typedef struct {
    void (*sa_handler)(int);  // User-provided handler, or SIG_IGN/SIG_DFL
    uint32 sa_flags;
    void (*sa_restorer)(void);
    uint32 sa_mask[2];        // 64-bit signal mask
} sigaction_t;
```

Currently, there is no user-mode signal trampoline or `sigreturn` mechanism. Instead, the kernel handles fatal signals by setting the task to ZOMBIE and sending SIGCHLD to the parent. The `signal_is_ignored()` function (in `syscall.c`) checks whether a signal has SIG_IGN disposition, which gates SIGTTIN/SIGTTOU delivery.

### Sending Signals

- `task_send_signal(pid, sig)` -- sends to a single process (or process group if pid < 0).
- `task_send_signal_to_pgrp(pgrp, sig)` -- sends to all members of a process group. Stop-class signals (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU) actually suspend the targets via `task_suspend()`.
- `task_signal_tree(root_pid, sig)` -- propagates a signal down the entire descendant tree using a fixed-point iteration (not recursion, to avoid stack overflow on 8 KB kernel stacks).

### Fatal Signal Handling

On every `task_schedule()` tick, the current task's unblocked pending signals are checked for SIGHUP/SIGINT/SIGQUIT/SIGTERM/SIGKILL. If found (and not SIG_IGN), the task transitions to ZOMBIE with exit code `128 + signal_number` and sends SIGCHLD to its parent -- exactly matching the Unix convention for signal-killed processes.

---

## 9. Job Control

Job control (the ability to suspend/resume background processes, like Ctrl+Z and `bg`/`fg` in a shell) is implemented in `job_control.c` and wired to the TTY layer.

### The Job Table

A fixed array of 64 job slots tracks active jobs:

```c
typedef struct {
    bool used;
    int job_id;           // User-visible job number (1, 2, 3, ...)
    uint32_t pgid;        // Process group ID
    task_t* leader;       // Job leader process
    job_ctrl_state_t state;  // RUNNING, STOPPED, DONE, CONTINUED
    int status;           // Exit status
    bool foreground;       // Is this job in the foreground?
    char command[256];    // Command string
} job_t;
```

### Foreground/Background Operations

- `job_foreground(job_id, cont)` -- brings a job to the foreground, sets the TTY's `fg_pgid` to the job's process group, and optionally sends SIGCONT.
- `job_background(job_id, cont)` -- moves a job to the background, returns terminal control to the shell's process group.

When a background process tries to read from the TTY, the TTY layer sends SIGTTIN to the process group (unless the signal is blocked or ignored). For writes, SIGTTOU is sent. This matches POSIX behavior exactly.

### Process Group Management

Each task has a `pgrp` (process group) and `session` field. These are managed via:

- `task_set_pgrp(pid, new_pgrp)` / `task_get_pgrp(pid)` (backed by `SYS_SET_PGRP` / `SYS_GET_PGRP_EXT`)
- `task_set_session(pid, new_session)` / `task_get_session(pid)`

A process group is considered "orphaned" (per POSIX) if no member has a parent in the same session but a different process group. Orphaned process groups get special SIGTTIN/SIGTTOU treatment.

---

## 10. Process IDs and Process Tree

### PID Assignment

PIDs are assigned from a simple global counter:

```c
static uint32 next_task_id = 1;
// In task creation:
new_task->id = next_task_id++;
```

PID 0 is never assigned (it means "current process group" in signal APIs). PID 1 is the init process. PIDs wrap around naturally when the counter overflows `uint32`.

### Parent-Child Relationships

Every task stores `parent_pid`, set at creation time:
- For `task_create_elf()`: parent is `current_task->id`
- For `task_clone_current()` (fork): parent is `current_task->id`
- For kernel tasks: parent is `current_task->id` (or 0 if no current task)

When a process dies, its children are reparented to PID 1 (init) via `task_reparent_children()`. This prevents orphaned processes from having dangling parent references.

### Process Tree Walk

There's no explicit process tree data structure. Instead, parent-child relationships are discovered by scanning the circular ready queue:

```c
// Find all children of a given PID
task_t* t = ready_queue_head;
do {
    if (t->parent_pid == target_pid) {
        // t is a child of target_pid
    }
    t = t->next;
} while (t != ready_queue_head);
```

This O(n) scan is acceptable because Fern's task count is typically small (dozens, not thousands).

---

## 11. SMP (Multi-Core) Support

Fern has basic SMP infrastructure in `smp.c` / `smp.h`, though multi-core scheduling is not yet fully wired up.

### CPU Discovery

`smp_init()` parses the ACPI MADT (Multiple APIC Description Table) to discover application processors (APs). For each enabled LAPIC entry, it:

1. Records the APIC ID and ACPI ID.
2. Marks the BSP (Bootstrap Processor) specially.
3. Tracks online CPU count.

```c
typedef struct {
    smp_cpu_info_t cpus[SMP_MAX_CPUS]; // Up to 32 CPUs
    uint32 cpu_count;                   // Total CPUs found
    uint32 online_cpus;                 // Currently online
    uint32 bsp_index;                   // Which CPU is the BSP
    uint32 bsp_apic_id;
    uint32 lapic_base;                  // Memory-mapped LAPIC register base
    bool initialized;
} smp_state_t;
```

### Current Limitations

The `scheduler.mk` file notes that advanced SMP interrupt distribution and IPI coordination files (`smp_interrupt_distribution.c`, `ipi_smp_coordination.c`) are quarantined because they reference unimplemented helpers. The core SMP discovery (`smp.c`) links fine, but the scheduler itself is single-CPU -- all tasks run on the BSP.

The infrastructure is ready for future multi-core scheduling: the task structure has per-task fields that could be extended with CPU affinity masks, and the LAPIC base address is available for sending inter-processor interrupts.

---

## 12. Thread Support

Fern has a kernel threading API (`thread.c` / `thread.h`) that wraps the task system. Threads are implemented as regular kernel tasks with a thin wrapper.

### Thread Structure

```c
struct thread {
    uint32_t tid;              // Thread ID (separate from PID)
    char name[64];
    thread_state_t state;      // CREATED, READY, RUNNING, BLOCKED, TERMINATED
    uint8_t priority;          // THREAD_PRIORITY_LOW/NORMAL/HIGH/REALTIME
    uint32_t flags;            // KERNEL, USER, JOINABLE, DETACHED
    thread_entry_t entry;      // The user's function pointer
    void *arg;                 // Argument to pass
    void *return_value;        // Return value from the function
    void *context;             // Opaque pointer to the underlying task_t
    void *stack;               // Thread stack
    size_t stack_size;
};
```

### How It Works

`thread_create()` allocates a `struct thread` and then calls `task_create_kernel()` with a `thread_trampoline` as the entry point. The trampoline:

1. Recovers the `struct thread` from `current_task->thread_wrapper`.
2. Calls the user's function pointer with the user's argument.
3. Stores the return value.
4. Calls `thread_exit()`, which delegates to `task_exit()`.

```c
static void thread_trampoline(void) {
    struct thread* self = current_task->thread_wrapper;
    void* ret = self->entry(self->arg);  // Call user's function
    self->return_value = ret;
    self->state = THREAD_STATE_TERMINATED;
    thread_exit(ret);
}
```

`thread_join()` busy-waits (via `thread_yield()`) until the thread's underlying task reaches ZOMBIE state, then `thread_destroy()` frees both the `struct thread` and the underlying `task_t`.

### Semaphores and Completions

The threading header provides inline spin-wait implementations for semaphores and completions:

```c
static inline void semaphore_up(struct semaphore *sem) {
    __sync_fetch_and_add(&sem->count, 1);
}

static inline int semaphore_down(struct semaphore *sem) {
    while (__sync_fetch_and_sub(&sem->count, 1) <= 0) {
        __sync_fetch_and_add(&sem->count, 1);
        cpu_relax();  // pause instruction
    }
    return 0;
}
```

These are basic spin-wait primitives. A production implementation would block the waiting thread on a wait queue rather than spinning.

---

## 13. Sleep, IPC, and Startup

### Sleep

Fern provides busy-wait (`sleep_busy()`) and interruptible sleep (`sleep_interruptible()`). The latter sets a `sleep_until_tick` deadline, transitions the task to WAITING, and yields. The scheduler wakes it on the next tick that passes the deadline.

### IPC

Fern has named shared memory (up to 32 regions) and message queues (up to 16 queues, 64 messages each, 256 bytes max). These are managed via `ipc_shm_create/open/close/destroy` and `ipc_msg_create/open/send/receive/destroy` in `task.c`. Physical frames are reference-counted and freed on destruction.

### The Startup Sequence

At boot, `tasks_init()` sets up the entire tasking subsystem:

1. **Create the kernel task** (PID 1-ish, but actually PID 1 in the counter). This represents the boot thread that runs `kmain()`. It's marked as `is_protected` so it can never be killed.

2. **Create the idle task** -- a kernel task running `idle_task_function()` which does `sti; hlt` in a loop.

3. **Build the initial circular ready queue**: kernel task -> idle task -> kernel task (self-loop).

4. **Preserve any pre-existing tasks** -- kernel tasks created before `tasks_init()` (e.g., splash animation threads) are spliced into the queue rather than orphaned.

5. **Initialize IPC** -- zero out the shared memory and message queue tables.

From this point, the timer interrupt drives `task_schedule()` on every tick, and user tasks are created via `task_create_elf()` as the system boots into userspace.

---

## Summary

Fern's process management follows Unix conventions closely:

| Feature | Status |
|---------|--------|
| Process creation (fork) | Full COW fork |
| Program execution (execve) | 32-bit ELF loading, PIE support |
| Process termination (exit/wait) | Zombie states, SIGCHLD, reparenting to init |
| Scheduler | Priority-aware round-robin with GUI boost |
| Signals | Pending bitmask, maskable signals, fatal signal handling |
| Job control | fg/bg, SIGTTIN/SIGTTOU, process groups, sessions |
| SMP | CPU discovery via ACPI, single-CPU scheduling |
| Threads | Kernel threads built on task system |
| IPC | Named shared memory, message queues |
| Memory isolation | Per-process page directories, COW fork |

The codebase prioritizes correctness and robustness -- the scheduler has extensive corruption detection and recovery (queue sanitization, pointer validation, deferred cleanup) that protects against the kinds of use-after-free and pointer corruption bugs that are common in OS development.

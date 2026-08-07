#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "elf.h"
#include "memory.h"

#define USER_HEAP_GUARD_PAGES 1
#define FORK_CHILD_RETURN 0xF00D

#define TASK_PRIORITY_MAX          7
#define TASK_PRIORITY_GUI          6
#undef TASK_PRIORITY_REALTIME
#define TASK_PRIORITY_REALTIME     7
#define TASK_GUI_TICK_BONUS        3
#define TASK_PRIORITY_BOOST_TICKS  50
#define GRAPHICS_WATCHDOG_TICKS    10

#define IPC_MAX_SHM_REGIONS   32
#define IPC_MAX_MSG_QUEUES    16
#define IPC_MAX_MESSAGES      64
#define IPC_MSG_MAX_SIZE      256
#define IPC_SHM_NAME_LEN      32

typedef enum {
    TASK_STATE_RUNNING,
    TASK_STATE_READY,
    TASK_STATE_WAITING,
    TASK_STATE_TERMINATED,
    TASK_STATE_ZOMBIE,
    TASK_STATE_SUSPENDED
} task_state_t;

typedef enum {
    JOB_STATE_RUNNING,
    JOB_STATE_STOPPED,
    JOB_STATE_TERMINATED
} job_state_t;

typedef struct ipc_shm_region {
    char name[IPC_SHM_NAME_LEN];
    uint32 id;
    uint32 owner_pid;
    uint32 size;
    uintptr_t phys_addr;
    uint32 ref_count;
    bool in_use;
} ipc_shm_region_t;

typedef struct ipc_message {
    uint32 sender_pid;
    uint32 type;
    uint32 length;
    uint8 data[IPC_MSG_MAX_SIZE];
} ipc_message_t;

typedef struct ipc_msg_queue {
    uint32 id;
    char name[IPC_SHM_NAME_LEN];
    uint32 creator_pid;
    uint32 max_messages;
    uint32 msg_size;
    uint32 head;
    uint32 tail;
    uint32 count;
    bool in_use;
    ipc_message_t messages[IPC_MAX_MESSAGES];
} ipc_msg_queue_t;

// Signal disposition (sa_handler/sa_flags/sa_restorer/sa_mask), one table per
// task -- see task_t::signal_handlers. Shape must stay in sync with whatever
// sys_rt_sigaction() copies to/from userspace in syscall.c.
typedef struct {
    void (*sa_handler)(int);
    uint32 sa_flags;
    void (*sa_restorer)(void);
    uint32 sa_mask[2]; // 64-bit signal mask
} sigaction_t;

typedef struct task {
    char name[32];
    uint32 id;
    uint32 pgrp;
    uint32 session;
    int32 tty_fd;
    task_state_t state;
    uintptr_t kernel_stack;
    uintptr_t kernel_stack_base;
    page_directory_t* page_directory;
    elf_load_info_t elf_info;
    uintptr_t user_heap_base;
    uintptr_t user_heap_limit;
    uintptr_t user_brk;
    int32 exit_code;
    char  exit_reason[32];
    uint32 uid;
    uint32 gid;
    uint32 groups_mask;

    uint32 priority;
    uint32 ticks_left;
    uint32 pending_signals;
    uint32 sleep_until_tick;
    uint32 last_active_tick;

    /* Real CPU-time accounting for ps/top/htop %CPU columns. cpu_ticks_total
     * is the cumulative number of timer ticks this task has spent as
     * current_task, updated in task_switch() (task.c) whenever it's switched
     * away from. scheduled_at_tick is the tick at which it was most recently
     * switched in -- the running delta (now - scheduled_at_tick) must be
     * added to cpu_ticks_total by any reader that wants live-second-accurate
     * usage for the task that's running right now. */
    uint64 cpu_ticks_total;
    uint32 scheduled_at_tick;
    uint32 created_at_tick;   /* tick at task creation -- never changes; process uptime = now - this */

    uint32 original_priority;
    uint32 boost_expires_at;
    
    bool watchdog_enabled;
    uint32 consecutive_timeouts;
    
    bool is_background;
    bool has_framebuffer_mapping;
    bool is_graphics_task;
    bool is_protected;
    
    uint32 memory_quota;
    uint32 memory_used;

    char cwd[256];
    uint32 signal_mask;
    // Per-process signal dispositions (SIG_IGN/SIG_DFL/handler), indexed by
    // signal number. Zero-initialized (SIG_DFL) by every task_t allocation
    // path (they all memory_set() the whole struct to 0 before use) and
    // inherited by fork() via its whole-struct memory_copy() of the parent
    // task_t -- both already give this field correct POSIX-like semantics
    // with no extra init/copy code. See sys_rt_sigaction()/signal_is_ignored()
    // in syscall.c, the only readers/writers.
    sigaction_t signal_handlers[32];
    uint32 parent_pid;
    int32 vt_index;

    bool needs_usermode_entry;
    uintptr_t usermode_entry_point;
    uintptr_t usermode_stack_top;

    void* waiting_semaphore;

    // Opaque back-pointer to a struct thread (thread.c), when this task was
    // created via thread_create() rather than task_create()/_kernel()
    // directly. NULL otherwise. Never dereferenced by task.c itself.
    void* thread_wrapper;

    void* vfp_context;

    struct task* next;
    struct task* next_in_pgrp;
} task_t;

// Signal definitions
#define SIGHUP          1       // Hangup
#define SIGINT          2       // Interrupt
#define SIGQUIT         3       // Quit
#define SIGILL          4       // Illegal instruction
#define SIGTRAP         5       // Trace/breakpoint trap
#define SIGABRT         6       // Aborted
#define SIGBUS          7       // Bus error
#define SIGFPE          8       // Floating point exception
#define SIGKILL         9       // Killed
#define SIGUSR1         10      // User defined signal 1
#define SIGSEGV         11      // Segmentation fault
#define SIGUSR2         12      // User defined signal 2
#define SIGPIPE         13      // Broken pipe
#define SIGALRM         14      // Alarm clock
#define SIGTERM         15      // Terminated
#define SIGSTKFLT       16      // Stack fault
#define SIGCHLD         17      // Child exited
#define SIGCONT         18      // Continued
#define SIGSTOP         19      // Stopped (signal)
#define SIGTSTP         20      // Stopped
#define SIGTTIN         21      // Stopped (tty input)
#define SIGTTOU         22      // Stopped (tty output)
#define SIGURG          23      // Urgent I/O condition
#define SIGXCPU         24      // CPU time limit exceeded
#define SIGXFSZ         25      // File size limit exceeded
#define SIGVTALRM       26      // Virtual timer expired
#define SIGPROF         27      // Profiling timer expired
#define SIGWINCH        28      // Window changed
#define SIGIO           29      // I/O possible
#define SIGPWR          30      // Power failure
#define SIGSYS          31      // Bad system call

/* Pending-signal bitmap helper (bit 0 intentionally unused). */
#define TASK_SIGNAL_BIT(sig) (1u << (sig))

void tasks_init(void);
task_t* task_create_elf(const uint8* elf_data, size_t elf_size, const char* name);
task_t* task_create_kernel(void (*entry_point)(void), const char* name, uint32 stack_size);
task_t* task_clone_current(void);
void task_destroy(task_t* task);
void task_switch(task_t* next_task);
void task_schedule(void);
void task_kill(uint32 pid);
void task_signal_tree(uint32 root_pid, uint32 sig);
void task_suspend(uint32 pid);
void task_resume(uint32 pid);
void task_set_priority(uint32 pid, uint32 priority);
uint32 task_get_id_by_name_prefix(const char* prefix);
void task_set_protected(uint32 pid, bool protected);
void task_terminate_current(int signal);
void task_exit(int code, const char* reason);
bool task_exists(uint32 pid);
int32 task_wait_pid(uint32 pid);
int32 task_reap_child(uint32 pid);
int32 task_get_exit_code(uint32 pid);
uint32 task_get_last_active_tick(uint32 pid);
void task_mark_active(void);
void debug_print_ready_queue(void);

void sleep_busy(uint32 microseconds);
void sleep_interruptible(uint32 milliseconds);
void task_shutdown_all(void);
void task_yield(void);

void task_set_foreground(task_t* task);
void task_clear_foreground(void);
task_t* task_get_foreground(void);
bool task_is_foreground(task_t* task);

void task_send_signal(int32 pid, int signal);
void task_send_signal_to_pgrp(uint32 pgrp, int signal);

/* Determine whether a process group is "orphaned" per POSIX (used by the
 * SIGTTIN/SIGTTOU EIO fallback): true unless at least one member of pgrp
 * has a parent that is still alive, in the same session, but in a
 * different process group (e.g. the controlling shell that could still
 * fg/bg this group). */
bool task_pgrp_is_orphaned(uint32 pgrp, uint32 session);

// Result of task_send_signal_to_pgrp_checked(): tells the caller whether the
// signal was actually generated, or suppressed per POSIX because it is
// blocked/ignored by the checked task. Currently only SIGTTIN/SIGTTOU honor
// this suppression (see task_send_signal_to_pgrp_checked() in task.c); all
// other signals always report SIGNAL_DELIVERY_SENT, matching the existing
// unconditional task_send_signal_to_pgrp() behavior.
typedef enum {
    SIGNAL_DELIVERY_SENT = 0,               // Signal generated as usual.
    SIGNAL_DELIVERY_BLOCKED_OR_IGNORED = 1  // Not generated: blocked/ignored by checked_pid.
} signal_delivery_status_t;

// Like task_send_signal_to_pgrp(), but for SIGTTIN/SIGTTOU first checks
// whether `checked_pid` (normally the calling task) has that signal blocked
// (signal_mask) or set to SIG_IGN (sys_rt_sigaction's signal_handlers
// table). If so, per POSIX the signal must NOT be generated to the process
// group at all, and SIGNAL_DELIVERY_BLOCKED_OR_IGNORED is returned so the
// caller (e.g. ttyN_read()/ttyN_write()) can fail the call with -EIO
// directly instead of raising a pointless signal. For any other signal, or
// when not blocked/ignored, this behaves exactly like
// task_send_signal_to_pgrp() and returns SIGNAL_DELIVERY_SENT.
signal_delivery_status_t task_send_signal_to_pgrp_checked(uint32 pgrp, int signal, uint32 checked_pid);

void task_set_framebuffer_mapping(task_t* task, bool mapped);
bool task_has_framebuffer_mapping(task_t* task);
void task_set_graphics_task(task_t* task, bool graphics);
bool task_is_graphics_task(task_t* task);
void task_graphics_watchdog_check(void);

int32 task_set_pgrp(uint32 pid, uint32 new_pgrp);
int32 task_get_pgrp(uint32 pid);
int32 task_set_session(uint32 pid, uint32 new_session);
int32 task_get_session(uint32 pid);

void task_reap_zombies(void);
void task_reparent_children(uint32 old_parent, uint32 new_parent);
task_t* task_find_by_pid(uint32 pid);
int32 task_wait_pid_any(int32* status_out);

// Read-only task enumeration for out-of-process consumers (currently the
// OOM killer, mm_oom.c) that need real per-task scoring data without
// depending on task.c internals or holding a task_t* past its lifetime.
typedef void (*task_oom_visit_fn)(uint32 pid, const char* name, uint32 memory_used,
                                   bool is_protected, bool is_init, void* ctx);
void task_for_each_oom_candidate(task_oom_visit_fn fn, void* ctx);

uint32 task_get_real_priority(task_t* task);

/* Plain-old-data snapshot of one task, copied out to userspace by
 * sys_get_tasks() (SYS_GET_TASKS) so `ps` can enumerate the real task
 * list instead of a hardcoded fake table. No pointers -- safe to
 * memory_copy() straight across the user/kernel boundary. Userspace
 * (userspace/ps.c) keeps its own field-identical mirror of this struct
 * rather than including this kernel-only header, matching the existing
 * pattern for dm_session_info_t/video_mode_t. Keep the two in sync. */
typedef struct {
    uint32 pid;
    uint32 parent_pid;
    uint32 pgrp;
    int32  state;              /* task_state_t value */
    uint32 last_active_tick;
    char   name[32];
    uint32 priority;           /* task_get_real_priority() -- effective scheduler priority */
    uint64 cpu_ticks_total;    /* cumulative scheduled ticks -- real %CPU source, see task_t */
    uint32 memory_used_kb;     /* live heap size (user_brk - user_heap_base) / 1024 */
    uint32 created_at_tick;    /* process birth tick -- process uptime = now - this */
} task_info_t;

/* Walks ready_queue_head under task_scheduler_lock and writes up to
 * max_entries task_info_t snapshots into out (which must have room for
 * at least max_entries entries). Returns the number of entries written,
 * or -1 if out is NULL. Bounded single-copy design (no size-query call
 * needed) since this kernel's task counts are small. */
int32 task_get_all(task_info_t* out, uint32 max_entries);
uint32 task_count_runnable(void);

int32 ipc_shm_create(const char* name, uint32 size);
int32 ipc_shm_open(const char* name);
int32 ipc_shm_close(const char* name);
int32 ipc_shm_destroy(const char* name);

int32 ipc_msg_create(const char* name, uint32 max_messages, uint32 msg_size);
int32 ipc_msg_open(const char* name);
int32 ipc_msg_send(uint32 queue_id, const void* data, uint32 length);
int32 ipc_msg_receive(uint32 queue_id, void* buffer, uint32 buffer_size);
int32 ipc_msg_destroy(uint32 queue_id);

extern task_t* current_task;
extern task_t* ready_queue_head;

#endif // TASK_H

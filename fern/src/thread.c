#include "include/thread.h"
#include "include/task.h"
#include "include/memory.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/util.h" // memory_set()

// Extern from task.c
extern task_t* current_task;

// Thread ID counter
static uint32_t next_tid = 1;

// struct thread (thread.h) and task_t (task.h) are two independent struct
// layouts with completely different field orders (e.g. thread's `tid` sits
// at the offset task_t reserves for `name`). This file used to reinterpret
// a task_t* as a struct thread* via raw pointer casts and write through it
// directly -- that stomped task_t's real id/state/kernel_stack/page_directory
// fields with whatever thread-struct data landed at the same offsets,
// corrupting the task's scheduling identity the moment a thread was
// created. struct thread is now always its own separate allocation, linked
// to its task_t via task_t.thread_wrapper (set once, read-only afterward).

// Trampoline actually installed as the kernel task's entry point. Recovers
// the real (entry, arg) pair through current_task->thread_wrapper -- safe
// because this only runs once the task has been scheduled in and
// current_task already points at it -- calls the caller's function, stashes
// its return value where thread_join() can find it, then hands off to the
// real task-exit path so the task is fully cleaned up and reaped like any
// other task instead of leaking as an unreapable zombie.
static void thread_trampoline(void) {
    struct thread* self = current_task ? (struct thread*)current_task->thread_wrapper : NULL;
    if (!self) {
        task_exit(-1, "thread_trampoline: missing thread_wrapper");
    }

    self->state = THREAD_STATE_RUNNING;
    void* ret = self->entry(self->arg);
    self->return_value = ret;
    self->state = THREAD_STATE_TERMINATED;

    thread_exit(ret);
}

struct thread *thread_create(const char *name, thread_entry_t entry, void *arg)
{
    if (!name || !entry) {
        return NULL;
    }

    struct thread* thread = (struct thread*)kmalloc(sizeof(struct thread));
    if (!thread) {
        return NULL;
    }
    memory_set((uint8*)thread, 0, sizeof(struct thread));

    thread->tid = next_tid++;
    strncpy(thread->name, name, sizeof(thread->name) - 1);
    thread->name[sizeof(thread->name) - 1] = '\0';
    thread->entry = entry;
    thread->arg = arg;
    thread->return_value = NULL;
    thread->state = THREAD_STATE_READY;
    thread->priority = THREAD_PRIORITY_NORMAL;
    thread->flags = THREAD_FLAG_KERNEL;

    // 8192 matches task.c's private KERNEL_STACK_SIZE macro (not exposed via
    // task.h); task_create_kernel() force-corrects any mismatched value
    // anyway, but passing the real size avoids a spurious debug warning.
    task_t* task = task_create_kernel(thread_trampoline, name, 8192);
    if (!task) {
        kfree(thread);
        return NULL;
    }

    // Safe to publish before the task ever runs: task_create_kernel() only
    // enqueues it as READY, the scheduler can't switch to it until this
    // function returns and releases the scheduler lock.
    thread->context = task;
    task->thread_wrapper = thread;

    return thread;
}

int thread_start(struct thread *thread)
{
    if (!thread || !thread->context) {
        return -1;
    }

    // Task is already started by task_create_kernel
    ((task_t*)thread->context)->state = TASK_STATE_READY;
    return 0;
}

int thread_join(struct thread *thread, void **retval)
{
    if (!thread || !thread->context) {
        return -1;
    }

    task_t* task = (task_t*)thread->context;
    // Wait for the task to reach zombie state (real task_exit() protocol),
    // matching what task_reap_zombies()/task_destroy() expect to see.
    while (task->state != TASK_STATE_ZOMBIE) {
        thread_yield();
    }

    if (retval) {
        *retval = thread->return_value;
    }

    return 0;
}

void thread_destroy(struct thread *thread)
{
    if (!thread) {
        return;
    }

    // Only valid to call once thread_join() has observed TASK_STATE_ZOMBIE
    // (the caller's documented contract, matching every existing call
    // site). task_destroy() performs the real fd/ipc/framebuffer/page-
    // directory/kernel-stack teardown; this file only owns the separate
    // `struct thread` wrapper allocation.
    if (thread->context) {
        task_destroy((task_t*)thread->context);
    }
    kfree(thread);
}

void thread_exit(void *retval)
{
    task_exit((int)(int32_t)(intptr_t)retval, "thread_exit");
    // task_exit() never returns.
}

struct thread *thread_current(void)
{
    return current_task ? (struct thread*)current_task->thread_wrapper : NULL;
}

uint32_t thread_get_tid(struct thread *thread)
{
    return thread ? thread->tid : 0;
}

void thread_yield(void)
{
    task_schedule();
}

// Semaphore operations (already implemented in header)

// Completion operations (already implemented in header)
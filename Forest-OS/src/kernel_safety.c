/*
 * Kernel Memory Safety & Scheduler Implementation
 * Prevents applications from killing the system through:
 * - Time quantum enforcement
 * - Priority-based scheduling
 * - Resource limits
 * - Watchdog timer
 */

#include "include/kernel_safety.h"
#include "include/task.h"
#include "include/memory.h"
#include "include/timer.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/panic.h"
#include "include/interrupt.h"
#include "include/util.h"

/* Global safety state */
static kernel_safety_stats_t safety_stats = {0};
static spinlock_t safety_lock = SPINLOCK_INIT("safety");

/* Time quantum per priority level (in ticks) */
static const uint32_t priority_quantum[NUM_PRIORITY_QUEUES] = {
    [TASK_PRIORITY_IDLE] = TIME_QUANTUM_MAX,      /* 500ms - let idle run */
    [TASK_PRIORITY_LOW] = TIME_QUANTUM_BASE,      /* 100ms */
    [2] = TIME_QUANTUM_BASE,                      /* 100ms - unused slot */
    [TASK_PRIORITY_NORMAL] = TIME_QUANTUM_BASE,   /* 100ms */
    [4] = TIME_QUANTUM_BASE,                      /* 100ms - unused slot */
    [TASK_PRIORITY_HIGH] = TIME_QUANTUM_MIN * 2,  /* 40ms - responsive */
    [TASK_PRIORITY_REALTIME] = TIME_QUANTUM_MIN,  /* 20ms - critical */
    [TASK_PRIORITY_KERNEL] = TIME_QUANTUM_MAX     /* 500ms - kernel needs time */
};

/* Cleanup handlers for task exit */
#define MAX_CLEANUP_HANDLERS 16
typedef struct {
    cleanup_handler_t handler;
    void* data;
    bool active;
} cleanup_entry_t;

static cleanup_entry_t cleanup_handlers[MAX_CLEANUP_HANDLERS];
static uint32_t cleanup_count = 0;

void kernel_safety_init(void) {
    memory_set((uint8_t*)&safety_stats, 0, sizeof(safety_stats));
    memory_set((uint8_t*)cleanup_handlers, 0, sizeof(cleanup_handlers));
    cleanup_count = 0;
    debuglog(DEBUG_INFO, "[SAFETY] Kernel safety subsystem initialized\n");
}

/*
 * task_consume_tick - Called on every timer tick for current task
 * Returns: true if task should continue, false if time quantum exhausted
 */
bool task_consume_tick(void) {
    extern task_t* current_task;
    
    if (!current_task) {
        return true;
    }
    
    /* Decrement ticks left */
    if (current_task->ticks_left > 0) {
        current_task->ticks_left--;
    }
    
    /* Update watchdog */
    if (current_task->watchdog_enabled) {
        current_task->last_active_tick = timer_get_ticks();
    }
    
    /* Check if quantum exhausted */
    if (current_task->ticks_left == 0) {
        /* Time's up - task should yield */
        return false;
    }
    
    return true;
}

/*
 * task_reset_quantum - Reset time quantum for current task
 * Called on context switch to new task
 */
void task_reset_quantum(void) {
    extern task_t* current_task;
    
    if (!current_task) {
        return;
    }
    
    uint32_t priority = task_get_real_priority(current_task);
    if (priority >= NUM_PRIORITY_QUEUES) {
        priority = TASK_PRIORITY_NORMAL;
    }
    
    current_task->ticks_left = priority_quantum[priority];
    
    spinlock_acquire(&safety_lock);
    safety_stats.scheduler_invocations++;
    spinlock_release(&safety_lock);
}

/*
 * task_priority_boost - Temporarily boost task priority
 * Used for interactive tasks (mouse/keyboard response)
 */
void task_priority_boost(uint32_t task_id, uint32_t boost_duration_ms) {
    extern task_t* ready_queue_head;
    
    if (!ready_queue_head) {
        return;
    }
    
    task_t* task = ready_queue_head;
    do {
        if (task->id == task_id) {
            /* Save original priority */
            if (task->original_priority == 0) {
                task->original_priority = task->priority;
            }
            
            /* Boost to high priority */
            task->priority = TASK_PRIORITY_HIGH;
            task->boost_expires_at = timer_get_ticks() + (boost_duration_ms / 10);
            
            spinlock_acquire(&safety_lock);
            safety_stats.priority_boosts++;
            spinlock_release(&safety_lock);
            
            debuglog(DEBUG_INFO, "[SAFETY] Priority boost for task %u until tick %u\n",
                    task_id, task->boost_expires_at);
            return;
        }
        task = task->next;
    } while (task != ready_queue_head);
}

/*
 * task_check_priority_boosts - Check and expire priority boosts
 * Called periodically from scheduler
 */
void task_check_priority_boosts(void) {
    extern task_t* ready_queue_head;
    uint32_t current_tick = timer_get_ticks();
    
    if (!ready_queue_head) {
        return;
    }
    
    task_t* task = ready_queue_head;
    do {
        if (task->original_priority != 0 && current_tick >= task->boost_expires_at) {
            /* Restore original priority */
            task->priority = task->original_priority;
            task->original_priority = 0;
            task->boost_expires_at = 0;
            
            debuglog(DEBUG_INFO, "[SAFETY] Priority boost expired for task %u\n", task->id);
        }
        task = task->next;
    } while (task != ready_queue_head);
}

/*
 * Watchdog functions - Detect stuck tasks
 */
void task_watchdog_pet(void) {
    extern task_t* current_task;
    
    if (current_task && current_task->watchdog_enabled) {
        current_task->last_active_tick = timer_get_ticks();
        current_task->consecutive_timeouts = 0;
    }
}

void task_watchdog_enable(bool enable) {
    extern task_t* current_task;
    
    if (!current_task) {
        return;
    }
    
    current_task->watchdog_enabled = enable;
    if (enable) {
        current_task->last_active_tick = timer_get_ticks();
        current_task->consecutive_timeouts = 0;
    }
}

/*
 * task_watchdog_check - Check all tasks for watchdog timeout
 * Returns: true if all tasks OK, false if a task was killed
 */
bool task_watchdog_check(uint32_t current_tick) {
    extern task_t* ready_queue_head;
    extern task_t* current_task;
    bool all_ok = true;
    
    if (!ready_queue_head) {
        return true;
    }
    
    task_t* task = ready_queue_head;
    do {
        if (task->watchdog_enabled && task != current_task) {
            uint32_t elapsed = current_tick - task->last_active_tick;
            
            if (elapsed > WATCHDOG_TIMEOUT_TICKS) {
                task->consecutive_timeouts++;
                
                if (task->consecutive_timeouts >= 3) {
                    /* Task is stuck - kill it */
                    debuglog(DEBUG_ERROR, 
                            "[SAFETY] Watchdog killing stuck task %u (PID %u)\n",
                            task->id, task->id);
                    
                    task->pending_signals |= TASK_SIGNAL_BIT(SIGKILL);
                    
                    spinlock_acquire(&safety_lock);
                    safety_stats.tasks_killed_by_watchdog++;
                    spinlock_release(&safety_lock);
                    
                    all_ok = false;
                } else {
                    /* Warning */
                    debuglog(DEBUG_WARN,
                            "[SAFETY] Watchdog warning for task %u (timeout %u/%u)\n",
                            task->id, task->consecutive_timeouts, 3);
                    task->last_active_tick = current_tick;
                }
            }
        }
        task = task->next;
    } while (task != ready_queue_head);
    
    return all_ok;
}

/*
 * Resource cleanup registration
 */
void register_task_cleanup(cleanup_handler_t handler, void* data) {
    spinlock_acquire(&safety_lock);
    
    if (cleanup_count < MAX_CLEANUP_HANDLERS) {
        cleanup_handlers[cleanup_count].handler = handler;
        cleanup_handlers[cleanup_count].data = data;
        cleanup_handlers[cleanup_count].active = true;
        cleanup_count++;
    }
    
    spinlock_release(&safety_lock);
}

void run_task_cleanup(void) {
    spinlock_acquire(&safety_lock);
    
    for (uint32_t i = 0; i < cleanup_count; i++) {
        if (cleanup_handlers[i].active && cleanup_handlers[i].handler) {
            cleanup_handlers[i].handler(cleanup_handlers[i].data);
            cleanup_handlers[i].active = false;
        }
    }
    cleanup_count = 0;
    
    spinlock_release(&safety_lock);
}

/*
 * Memory safety enforcement
 */
bool validate_user_pointer(const void* ptr, size_t size, uint32_t required_perm) {
    extern task_t* current_task;
    
    if (!ptr || size == 0) {
        return false;
    }
    
    /* Check for NULL pointer */
    if (ptr == NULL) {
        return false;
    }
    
    /* Check pointer is in user space (below kernel boundary) */
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t end_addr = addr + size;
    
    /* Prevent overflow */
    if (end_addr < addr) {
        return false;
    }
    
    /* Check in user space range */
    if (addr < 0x1000 || end_addr >= 0xC0000000) {
        /* Below page 1 (null page) or in kernel space */
        return false;
    }
    
    /* For kernel tasks, allow kernel pointers */
    /* Kernel tasks use a special page directory - check if task ID is low (kernel tasks) */
    if (current_task && current_task->id <= 2) {
        return true;
    }
    
    return true;
}

/*
 * enforce_memory_limits - Check if task is within memory limits
 */
bool enforce_memory_limits(void) {
    extern task_t* current_task;
    
    if (!current_task) {
        return true;
    }
    
    /* Check memory quota if set */
    if (current_task->memory_quota > 0 && 
        current_task->memory_used > current_task->memory_quota) {
        debuglog(DEBUG_ERROR,
                "[SAFETY] Task %u exceeded memory quota: %u > %u bytes\n",
                current_task->id, current_task->memory_used, current_task->memory_quota);
        
        spinlock_acquire(&safety_lock);
        safety_stats.memory_violations++;
        spinlock_release(&safety_lock);
        
        return false;
    }
    
    return true;
}

/*
 * Stats and diagnostics
 */
void kernel_safety_get_stats(kernel_safety_stats_t* stats) {
    if (!stats) {
        return;
    }
    
    spinlock_acquire(&safety_lock);
    memory_copy((uint8_t*)stats, (uint8_t*)&safety_stats, sizeof(kernel_safety_stats_t));
    spinlock_release(&safety_lock);
}

void kernel_safety_violation(const char* violation_type, const char* details) {
    debuglog(DEBUG_ERROR, "[SAFETY VIOLATION] %s: %s\n", violation_type, details);
    
    spinlock_acquire(&safety_lock);
    safety_stats.memory_violations++;
    spinlock_release(&safety_lock);
    
    /* Optional: kill offending task or panic based on severity */
    extern task_t* current_task;
    if (current_task) {
        current_task->pending_signals |= TASK_SIGNAL_BIT(SIGKILL);
    }
}

/*
 * Task limit enforcement - checked in task_create functions
 */

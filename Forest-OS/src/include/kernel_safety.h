/*
 * Kernel Memory Safety & Scheduler Improvements
 * 
 * This implements:
 * 1. Time quantum scheduling (prevents CPU hogs)
 * 2. Priority-based scheduling with multiple queues
 * 3. Automatic resource cleanup on task exit
 * 4. Memory isolation enforcement
 * 5. Watchdog timer for stuck tasks
 */

#ifndef KERNEL_SAFETY_H
#define KERNEL_SAFETY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Task priority levels - higher number = higher priority */
#define TASK_PRIORITY_IDLE      0   /* Idle task only */
#define TASK_PRIORITY_LOW       1   /* Background tasks */
#define TASK_PRIORITY_NORMAL    3   /* Regular user tasks */
#define TASK_PRIORITY_HIGH      5   /* Interactive/GUI tasks */
#define TASK_PRIORITY_REALTIME  6   /* Critical system tasks */
#define TASK_PRIORITY_KERNEL    7   /* Kernel threads */

#define NUM_PRIORITY_QUEUES     8

/* Time quantum in timer ticks (100Hz = 10ms per tick) */
#define TIME_QUANTUM_BASE       10  /* 100ms for normal priority */
#define TIME_QUANTUM_MIN        2   /* 20ms minimum */
#define TIME_QUANTUM_MAX        50  /* 500ms maximum */

/* Watchdog timeout in ticks (5 seconds) */
#define WATCHDOG_TIMEOUT_TICKS  500

/* Maximum tasks to prevent resource exhaustion */
#define MAX_TASKS_LIMIT         256

/* Memory protection flags */
#define MEM_PROT_GUARD_PAGES    0x01  /* Guard pages on stack/heap */
#define MEM_PROT_STRICT_PERM    0x02  /* Strict RWX permissions */
#define MEM_PROT_ASLR           0x04  /* Address space layout randomization */

typedef struct {
    uint32_t total_tasks_created;
    uint32_t total_tasks_destroyed;
    uint32_t tasks_killed_by_watchdog;
    uint32_t memory_violations;
    uint32_t scheduler_invocations;
    uint32_t priority_boosts;
} kernel_safety_stats_t;

typedef struct {
    uint32_t last_activity_tick;
    uint32_t consecutive_timeouts;
    bool watchdog_enabled;
} task_watchdog_t;

/* Initialize kernel safety subsystem */
void kernel_safety_init(void);

/* Update task time quantum on scheduler tick */
bool task_consume_tick(void);

/* Boost task priority temporarily (for interactive response) */
void task_priority_boost(uint32_t task_id, uint32_t boost_duration_ms);

/* Reset task time quantum (called on task switch) */
void task_reset_quantum(void);

/* Watchdog functions */
void task_watchdog_pet(void);
void task_watchdog_enable(bool enable);
bool task_watchdog_check(uint32_t current_tick);

/* Memory safety enforcement */
bool enforce_memory_limits(void);
bool validate_user_pointer(const void* ptr, size_t size, uint32_t required_perm);

/* Resource cleanup registration */
typedef void (*cleanup_handler_t)(void* data);
void register_task_cleanup(cleanup_handler_t handler, void* data);
void run_task_cleanup(void);

/* Stats */
void kernel_safety_get_stats(kernel_safety_stats_t* stats);

/* Panic on safety violation */
void kernel_safety_violation(const char* violation_type, const char* details);

#endif /* KERNEL_SAFETY_H */

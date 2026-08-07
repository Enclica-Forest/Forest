#include "include/semaphore.h"
#include "include/task.h"
#include "include/memory.h"
#include "include/panic.h"
#include "include/mm.h"

void semaphore_wait(semaphore_t* sem) {
    while (true) {
        uint32 current_count = atomic_load32(&sem->count);
        
        if (current_count > 0) {
            if (atomic_compare_and_swap32(&sem->count, current_count, current_count - 1)) {
                return;
            }
            continue;
        }
        
        spinlock_acquire(&sem->wait_lock);
        
        current_count = atomic_load32(&sem->count);
        if (current_count > 0) {
            spinlock_release(&sem->wait_lock);
            continue;
        }
        
        semaphore_wait_node_t* node = (semaphore_wait_node_t*)kmalloc(sizeof(semaphore_wait_node_t));
        if (!node) {
            spinlock_release(&sem->wait_lock);
            kernel_panic("Failed to allocate semaphore wait node");
            return;
        }
        
        node->task = current_task;
        node->next = NULL;

        // If no current task (early kernel), just spin instead of blocking
        if (!current_task) {
            kfree(node);
            spinlock_release(&sem->wait_lock);
            continue;
        }

        if (!sem->wait_queue_head) {
            sem->wait_queue_head = node;
            sem->wait_queue_tail = node;
        } else {
            sem->wait_queue_tail->next = node;
            sem->wait_queue_tail = node;
        }

        current_task->state = TASK_STATE_WAITING;
        current_task->waiting_semaphore = sem;

        spinlock_release(&sem->wait_lock);

        task_schedule();

        // We only resume here once semaphore_post() (or the task-exit
        // cleanup path) has already unlinked our node from the wait queue,
        // so it's safe to clear the back-pointer now.
        current_task->waiting_semaphore = NULL;
    }
}

void semaphore_remove_task(semaphore_t* sem, task_t* task) {
    if (!sem || !task) {
        return;
    }

    spinlock_acquire(&sem->wait_lock);

    semaphore_wait_node_t* prev = NULL;
    semaphore_wait_node_t* node = sem->wait_queue_head;
    while (node) {
        semaphore_wait_node_t* next = node->next;
        if (node->task == task) {
            if (prev) {
                prev->next = next;
            } else {
                sem->wait_queue_head = next;
            }
            if (sem->wait_queue_tail == node) {
                sem->wait_queue_tail = prev;
            }
            kfree(node);
        } else {
            prev = node;
        }
        node = next;
    }

    spinlock_release(&sem->wait_lock);
}

bool semaphore_try_wait(semaphore_t* sem) {
    uint32 current_count = atomic_load32(&sem->count);
    
    if (current_count > 0) {
        return atomic_compare_and_swap32(&sem->count, current_count, current_count - 1);
    }
    
    return false;
}

void semaphore_post(semaphore_t* sem) {
    uint32 current_count = atomic_load32(&sem->count);
    
    if (current_count < sem->max_count) {
        if (!atomic_compare_and_swap32(&sem->count, current_count, current_count + 1)) {
            return;
        }
    }
    
    spinlock_acquire(&sem->wait_lock);
    
    if (sem->wait_queue_head) {
        semaphore_wait_node_t* node = sem->wait_queue_head;
        sem->wait_queue_head = node->next;
        
        if (!sem->wait_queue_head) {
            sem->wait_queue_tail = NULL;
        }
        
        task_t* waiting_task = node->task;
        kfree(node);
        
        if (waiting_task && waiting_task->state == TASK_STATE_WAITING) {
            waiting_task->state = TASK_STATE_READY;
        }
    }
    
    spinlock_release(&sem->wait_lock);
}

uint32 semaphore_get_count(const semaphore_t* sem) {
    return atomic_load32(&sem->count);
}
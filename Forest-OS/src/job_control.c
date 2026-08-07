/**
 * Forest-OS Job Control Implementation
 * Provides POSIX job control (bg/fg) and SIGWINCH for terminal resize
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/util.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/tty.h"
#include "job_control.h"

#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef ESRCH
#define ESRCH 3
#endif

// Maximum number of jobs
#define MAX_JOBS 64

// Global job data
static job_t g_jobs[MAX_JOBS];
static spinlock_t g_job_lock;
static int g_next_job_id = 1;

// Initialize job control
void job_control_init(void) {
    spinlock_init(&g_job_lock, "job_ctrl");
    memory_set((uint8*)g_jobs, 0, sizeof(g_jobs));
    debuglog(DEBUG_INFO, "[JOB] Job control initialized\n");
}

// Create a new job from process group
int job_create(uint32_t pgid, task_t* leader, const char* cmd) {
    spinlock_acquire(&g_job_lock);
    
    // Find empty slot
    int idx = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!g_jobs[i].used) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        spinlock_release(&g_job_lock);
        return -EAGAIN;
    }
    
    g_jobs[idx].used = true;
    g_jobs[idx].job_id = g_next_job_id++;
    g_jobs[idx].pgid = pgid;
    g_jobs[idx].leader = leader;
    g_jobs[idx].state = JOB_CTRL_RUNNING;
    g_jobs[idx].status = 0;
    g_jobs[idx].foreground = false;
    
    if (cmd) {
        strncpy(g_jobs[idx].command, cmd, sizeof(g_jobs[idx].command) - 1);
    } else {
        g_jobs[idx].command[0] = '\0';
    }
    
    int result = g_jobs[idx].job_id;
    spinlock_release(&g_job_lock);
    
    return result;
}

// Find job by job ID
static job_t* find_job_by_id(int job_id) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].used && g_jobs[i].job_id == job_id) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

// Find job by process group ID
static job_t* find_job_by_pgid(uint32_t pgid) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].used && g_jobs[i].pgid == pgid) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

// Get job by ID (user-facing, takes positive job number)
int job_get(int job_id) {
    spinlock_acquire(&g_job_lock);
    job_t* job = find_job_by_id(job_id);
    int result = job ? (int)job->pgid : -1;
    spinlock_release(&g_job_lock);
    return result;
}

// Put job in foreground
int job_foreground(int job_id, bool cont) {
    spinlock_acquire(&g_job_lock);
    
    job_t* job = find_job_by_id(job_id);
    if (!job) {
        spinlock_release(&g_job_lock);
        return -ESRCH;
    }
    
    // Set terminal control
    job->foreground = true;

    // Give this job's process group control of its controlling terminal so
    // SIGTTIN/SIGTTOU can be enforced by ttyN_read()/ttyN_write().
    if (job->leader && job->leader->tty_fd >= 0 && job->leader->tty_fd < MAX_VIRTUAL_TTYS) {
        g_virtual_ttys[job->leader->tty_fd].fg_pgid = job->pgid;
    }

    // Continue stopped job if requested
    if (cont && job->state == JOB_CTRL_STOPPED) {
        job->state = JOB_CTRL_RUNNING;
        
        // Resume all processes in the job
        task_t* task = ready_queue_head;
        if (task) {
            do {
                if (task->pgrp == job->pgid) {
                    task->state = TASK_STATE_READY;
                    // Would send SIGCONT here in full implementation
                }
                task = task->next;
            } while (task != ready_queue_head);
        }
    }
    
    spinlock_release(&g_job_lock);
    return 0;
}

// Put job in background
int job_background(int job_id, bool cont) {
    spinlock_acquire(&g_job_lock);
    
    job_t* job = find_job_by_id(job_id);
    if (!job) {
        spinlock_release(&g_job_lock);
        return -ESRCH;
    }
    
    job->foreground = false;

    // Return terminal control to the calling shell's process group so the
    // backgrounded job's reads/writes get SIGTTIN/SIGTTOU per POSIX.
    if (job->leader && job->leader->tty_fd >= 0 && job->leader->tty_fd < MAX_VIRTUAL_TTYS &&
        current_task) {
        g_virtual_ttys[job->leader->tty_fd].fg_pgid = current_task->pgrp;
    }

    // Continue stopped job if requested
    if (cont && job->state == JOB_CTRL_STOPPED) {
        job->state = JOB_CTRL_RUNNING;

        // Resume all processes in the job
        task_t* task = ready_queue_head;
        if (task) {
            do {
                if (task->pgrp == job->pgid) {
                    task->state = TASK_STATE_READY;
                }
                task = task->next;
            } while (task != ready_queue_head);
        }
    }

    spinlock_release(&g_job_lock);
    return 0;
}

// Update job state (called when process terminates or stops)
void job_update_state(uint32_t pgid, job_ctrl_state_t new_state, int status) {
    spinlock_acquire(&g_job_lock);
    
    job_t* job = find_job_by_pgid(pgid);
    if (job) {
        job->state = new_state;
        job->status = status;
    }
    
    spinlock_release(&g_job_lock);
}

// Delete job
void job_delete(int job_id) {
    spinlock_acquire(&g_job_lock);
    
    job_t* job = find_job_by_id(job_id);
    if (job) {
        job->used = false;
    }
    
    spinlock_release(&g_job_lock);
}

// Get list of jobs (for jobs builtin)
int job_list(job_t* user_jobs, int max_jobs) {
    spinlock_acquire(&g_job_lock);
    
    int count = 0;
    for (int i = 0; i < MAX_JOBS && count < max_jobs; i++) {
        if (g_jobs[i].used) {
            user_jobs[count++] = g_jobs[i];
        }
    }
    
    spinlock_release(&g_job_lock);
    return count;
}

// ============================================
// SIGWINCH - Terminal Resize Handling
// ============================================

// Terminal size
static uint16_t g_term_rows = 25;
static uint16_t g_term_cols = 80;

// Set terminal size and deliver SIGWINCH to the given tty's own foreground
// process group (per-tty g_virtual_ttys[tty].fg_pgid), not some stale
// global. Each virtual tty tracks its own foreground pgid independently, so
// the caller must identify which tty is being resized.
void term_set_size(uint16_t rows, uint16_t cols, int tty) {
    g_term_rows = rows;
    g_term_cols = cols;

    // Send SIGWINCH to this tty's foreground process group
    if (tty >= 0 && tty < MAX_VIRTUAL_TTYS) {
        uint32_t fg_pgid = g_virtual_ttys[tty].fg_pgid;
        if (fg_pgid != 0) {
            task_t* task = ready_queue_head;
            if (task) {
                do {
                    if (task->pgrp == fg_pgid) {
                        task->pending_signals |= (1 << SIGWINCH);
                    }
                    task = task->next;
                } while (task != ready_queue_head);
            }
        }
    }
}

// Get terminal size
void term_get_size(uint16_t* rows, uint16_t* cols) {
    if (rows) *rows = g_term_rows;
    if (cols) *cols = g_term_cols;
}

// Handle terminal resize (called from ioctl TIOCSWINSZ)
int term_handle_resize(uint16_t rows, uint16_t cols, int tty) {
    if (rows == 0 || cols == 0) {
        return -EINVAL;
    }

    term_set_size(rows, cols, tty);
    return 0;
}

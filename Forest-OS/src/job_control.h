#ifndef JOB_CONTROL_H
#define JOB_CONTROL_H

// POSIX job control (bg/fg process groups) and SIGWINCH terminal-resize
// support. Declared here so callers outside job_control.c (tty_devices.c,
// syscall.c) can invoke this API without implicit-declaration hazards.

#include <stdbool.h>
#include <stdint.h>
#include "include/task.h"

// Job state, mirrors POSIX job-control states.
typedef enum {
    JOB_CTRL_RUNNING,
    JOB_CTRL_STOPPED,
    JOB_CTRL_DONE,
    JOB_CTRL_CONTINUED
} job_ctrl_state_t;

// Job record. Exposed (not opaque) so job_list() can copy entries out to a
// caller-provided array.
typedef struct {
    bool used;
    int job_id;           // Job ID (positive number)
    uint32_t pgid;        // Process group ID
    task_t* leader;       // Job leader process
    job_ctrl_state_t state;    // Job state
    int status;           // Exit status
    bool foreground;       // Is job in foreground?
    char command[256];    // Command name
} job_t;

// Initialize job control state.
void job_control_init(void);

// Create a new job from a process group. Returns positive job ID, or
// negative errno on failure.
int job_create(uint32_t pgid, task_t* leader, const char* cmd);

// Get a job's process group ID by job ID. Returns -1 if not found.
int job_get(int job_id);

// Put a job in the foreground (claims controlling-terminal fg_pgid so
// SIGTTIN/SIGTTOU gating in ttyN_read()/ttyN_write() tracks it). Returns 0
// on success, negative errno on failure.
int job_foreground(int job_id, bool cont);

// Put a job in the background (returns controlling-terminal fg_pgid to the
// calling shell's process group). Returns 0 on success, negative errno on
// failure.
int job_background(int job_id, bool cont);

// Update job state (called when a process in the job terminates or stops).
void job_update_state(uint32_t pgid, job_ctrl_state_t new_state, int status);

// Delete/free a job slot.
void job_delete(int job_id);

// Copy up to max_jobs active job records into user_jobs. Returns the count
// copied.
int job_list(job_t* user_jobs, int max_jobs);

// Set the terminal size and deliver SIGWINCH to the given tty's own
// foreground process group (g_virtual_ttys[tty].fg_pgid). Each virtual tty
// tracks its foreground pgid independently, so callers must identify which
// tty (index into g_virtual_ttys, see MAX_VIRTUAL_TTYS) is being resized.
void term_set_size(uint16_t rows, uint16_t cols, int tty);

// Get the current terminal size.
void term_get_size(uint16_t* rows, uint16_t* cols);

// Handle a terminal resize (e.g. from ioctl TIOCSWINSZ) on the given tty.
// Returns 0 on success, negative errno on failure.
int term_handle_resize(uint16_t rows, uint16_t cols, int tty);

#endif // JOB_CONTROL_H

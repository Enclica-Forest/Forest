/*
 * Fern - Cross-Architecture Session and Process Group Management
 * src/arch/session.h
 *
 * Provides POSIX-like session and process group management that works
 * across all supported architectures (x86_32, x86_64, arm32, aarch64, riscv64).
 *
 * Sessions group related process groups (e.g. all processes spawned from
 * a single login). Process groups bundle related processes (e.g. a shell
 * pipeline) for signal delivery and terminal access control.
 *
 * This module is architecture-independent -- it operates on task_t via
 * the cross-arch scheduler API (task_find_by_pid) and stores all state
 * in portable C structures.
 */

#ifndef FOREST_ARCH_SESSION_H
#define FOREST_ARCH_SESSION_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Limits ----------------------------------------------------------- */

#define MAX_SESSIONS            32      /* max concurrent sessions */
#define MAX_PROCESS_GROUPS      128     /* max concurrent process groups */
#define MAX_PGRP_MEMBERS        64      /* max tasks per process group */

/* ---- Error codes ------------------------------------------------------ */

#define SESSION_OK              0
#define SESSION_ERR_NOTFOUND   (-1)     /* session/pgrp does not exist */
#define SESSION_ERR_NOMEM      (-2)     /* table full */
#define SESSION_ERR_INVAL      (-3)     /* bad arguments */
#define SESSION_ERR_EXISTS     (-4)     /* already exists */
#define SESSION_ERR_FULL       (-5)     /* process group full */

/* ---- Session entry ---------------------------------------------------- */

typedef struct session_entry {
    uint32_t    session_id;     /* unique session identifier */
    uint32_t    leader_pid;     /* PID of session leader (first process) */
    bool        in_use;         /* slot is allocated */
} session_entry_t;

/* ---- Process group entry ---------------------------------------------- */

typedef struct process_group_entry {
    uint32_t    pgid;           /* process group ID (= leader PID) */
    uint32_t    session_id;     /* session this group belongs to */
    uint32_t    leader_pid;     /* PID of group leader */
    uint32_t    members[MAX_PGRP_MEMBERS]; /* member PIDs */
    uint32_t    member_count;   /* number of active members */
    bool        in_use;         /* slot is allocated */
} process_group_entry_t;

/* ---- Public API ------------------------------------------------------- */

/**
 * session_init - Initialise the session and process group subsystem.
 *
 * Must be called once during boot before any other session_* function.
 * Clears all session and process group tables.
 */
void session_init(void);

/**
 * session_create - Create a new session.
 *
 * @sid:        Session ID to assign (typically the leader's PID).
 *
 * Returns SESSION_OK on success, or a SESSION_ERR_* code on failure.
 * The caller is responsible for assigning tasks to the session afterward
 * via task_set_session_id().
 */
int session_create(uint32_t sid);

/**
 * session_destroy - Remove a session and all its process groups.
 *
 * @sid:        Session ID to destroy.
 *
 * Returns SESSION_OK on success, SESSION_ERR_NOTFOUND if not found.
 */
int session_destroy(uint32_t sid);

/**
 * session_get_leader - Get the leader PID of a session.
 *
 * @sid:        Session ID to query.
 *
 * Returns the leader PID, or 0 if the session does not exist.
 */
uint32_t session_get_leader(uint32_t sid);

/**
 * session_exists - Check if a session ID is allocated.
 */
bool session_exists(uint32_t sid);

/* ---- Process group API ------------------------------------------------ */

/**
 * process_group_create - Create a new process group.
 *
 * @pgid:       Process group ID to assign (typically the leader's PID).
 * @sid:        Session the group belongs to.
 *
 * Returns SESSION_OK on success, or a SESSION_ERR_* code.
 */
int process_group_create(uint32_t pgid, uint32_t sid);

/**
 * process_group_destroy - Remove a process group.
 *
 * @pgid:       Process group ID to destroy.
 *
 * Returns SESSION_OK on success, SESSION_ERR_NOTFOUND if not found.
 */
int process_group_destroy(uint32_t pgid);

/**
 * process_group_add - Add a process to a group.
 *
 * @pgid:       Process group ID.
 * @pid:        PID of the task to add.
 *
 * Returns SESSION_OK on success, or a SESSION_ERR_* code.
 */
int process_group_add(uint32_t pgid, uint32_t pid);

/**
 * process_group_remove - Remove a process from a group.
 *
 * @pgid:       Process group ID.
 * @pid:        PID of the task to remove.
 *
 * Returns SESSION_OK on success, SESSION_ERR_NOTFOUND if not in group.
 */
int process_group_remove(uint32_t pgid, uint32_t pid);

/**
 * process_group_get_session - Get the session a process group belongs to.
 *
 * @pgid:       Process group ID.
 *
 * Returns the session ID, or 0 if the group does not exist.
 */
uint32_t process_group_get_session(uint32_t pgid);

/**
 * process_group_get_leader - Get the leader PID of a process group.
 *
 * @pgid:       Process group ID.
 *
 * Returns the leader PID, or 0 if the group does not exist.
 */
uint32_t process_group_get_leader(uint32_t pgid);

/**
 * process_group_is_member - Check if a PID is in a process group.
 *
 * @pgid:       Process group ID.
 * @pid:        PID to check.
 *
 * Returns true if pid is a member of pgid.
 */
bool process_group_is_member(uint32_t pgid, uint32_t pid);

/* ---- Convenience: assign task to session/group ------------------------ */

/**
 * session_assign_task - Create session+group and assign task in one call.
 *
 * If the session does not exist, it is created with @pid as leader.
 * If the process group does not exist, it is created with @pid as leader.
 * The task is then assigned to both.
 *
 * This is the typical call for a new session leader (e.g. login shell).
 *
 * @pid:        PID of the task (also becomes session/group leader).
 *
 * Returns SESSION_OK on success.
 */
int session_assign_task(uint32_t pid);

/**
 * session_add_task_to_group - Add an existing task to a process group.
 *
 * The task's session_id is set to match the group's session.
 *
 * @pgid:       Process group ID.
 * @pid:        PID of the task to add.
 *
 * Returns SESSION_OK on success.
 */
int session_add_task_to_group(uint32_t pgid, uint32_t pid);

/**
 * session_remove_task - Remove a task from its session and process group.
 *
 * Called on task exit. If the task was the session leader and has no more
 * process groups, the session is destroyed. If the process group becomes
 * empty, it is destroyed.
 *
 * @pid:        PID of the exiting task.
 */
void session_remove_task(uint32_t pid);

/* ---- Query helpers ---------------------------------------------------- */

/**
 * session_get_task_session - Get the session ID a task belongs to.
 *
 * @pid:        Task PID.
 *
 * Returns session_id, or 0 if not assigned.
 */
uint32_t session_get_task_session(uint32_t pid);

/**
 * session_get_task_pgrp - Get the process group a task belongs to.
 *
 * @pid:        Task PID.
 *
 * Returns pgrp_id, or 0 if not assigned.
 */
uint32_t session_get_task_pgrp(uint32_t pid);

/**
 * session_pgrp_is_orphaned - Check if a process group is orphaned.
 *
 * A process group is orphaned if no process in the group has a parent
 * in a different process group of the same session (i.e. a "job control
 * shell" that could foreground/background it).
 *
 * @pgid:       Process group ID.
 * @sid:        Session ID.
 *
 * Returns true if the group is orphaned.
 */
bool session_pgrp_is_orphaned(uint32_t pgid, uint32_t sid);

/* ---- Debug / diagnostics ---------------------------------------------- */

/**
 * session_print_status - Print session and group tables to debug log.
 */
void session_print_status(void);

#endif /* FOREST_ARCH_SESSION_H */

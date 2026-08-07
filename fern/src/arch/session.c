/*
 * Fern - Cross-Architecture Session and Process Group Management
 * src/arch/session.c
 *
 * Architecture-independent implementation of POSIX-like sessions and
 * process groups. All state lives in fixed-size tables (no dynamic
 * allocation) so this works on any target without a heap allocator
 * dependency.
 *
 * Design:
 *   - Sessions are indexed by session_id (typically the leader PID).
 *   - Process groups are indexed by pgid (typically the leader PID).
 *   - Each process group tracks its member PIDs in a fixed array.
 *   - Tasks reference their session/pgrp via task_t::session_id and
 *     task_t::pgrp_id, set by the cross-arch task layer.
 *
 * Thread safety: callers must hold the session_lock spinlock around
 * any sequence of calls that reads-modifies session or group state.
 * Individual query helpers acquire the lock internally.
 */

#include "session.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Kernel headers */
#include "../include/spinlock.h"
#include "../include/debuglog.h"

/* ---- Static tables ---------------------------------------------------- */

static session_entry_t       session_table[MAX_SESSIONS];
static process_group_entry_t pgrp_table[MAX_PROCESS_GROUPS];
static bool                  session_subsystem_init = false;
static spinlock_t            session_lock = SPINLOCK_INIT("session");

/* ---- Internal helpers ------------------------------------------------- */

static session_entry_t* find_session(uint32_t sid) {
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (session_table[i].in_use && session_table[i].session_id == sid) {
            return &session_table[i];
        }
    }
    return NULL;
}

static process_group_entry_t* find_pgrp(uint32_t pgid) {
    for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
        if (pgrp_table[i].in_use && pgrp_table[i].pgid == pgid) {
            return &pgrp_table[i];
        }
    }
    return NULL;
}

/* ---- Public API: Session ---------------------------------------------- */

void session_init(void) {
    if (session_subsystem_init) return;

    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        session_table[i].session_id = 0;
        session_table[i].leader_pid = 0;
        session_table[i].in_use     = false;
    }
    for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
        pgrp_table[i].pgid         = 0;
        pgrp_table[i].session_id   = 0;
        pgrp_table[i].leader_pid   = 0;
        pgrp_table[i].member_count = 0;
        pgrp_table[i].in_use       = false;
        for (uint32_t j = 0; j < MAX_PGRP_MEMBERS; j++) {
            pgrp_table[i].members[j] = 0;
        }
    }

    session_subsystem_init = true;
    debuglog(DEBUG_INFO, "[SESSION] Subsystem initialized (%u sessions, %u groups)\n",
             MAX_SESSIONS, MAX_PROCESS_GROUPS);
}

int session_create(uint32_t sid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;
    if (sid == 0) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    /* Check for duplicates */
    if (find_session(sid)) {
        spinlock_release(&session_lock);
        return SESSION_ERR_EXISTS;
    }

    /* Find free slot */
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (!session_table[i].in_use) {
            session_table[i].session_id = sid;
            session_table[i].leader_pid = sid;
            session_table[i].in_use     = true;
            spinlock_release(&session_lock);
            debuglog(DEBUG_INFO, "[SESSION] Created session %u (leader PID %u)\n",
                     sid, sid);
            return SESSION_OK;
        }
    }

    spinlock_release(&session_lock);
    return SESSION_ERR_NOMEM;
}

int session_destroy(uint32_t sid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    session_entry_t* s = find_session(sid);
    if (!s) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }

    /* Remove all process groups belonging to this session */
    for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
        if (pgrp_table[i].in_use && pgrp_table[i].session_id == sid) {
            debuglog(DEBUG_INFO, "[SESSION] Destroying group %u (session %u)\n",
                     pgrp_table[i].pgid, sid);
            pgrp_table[i].in_use = false;
        }
    }

    s->in_use = false;
    spinlock_release(&session_lock);
    debuglog(DEBUG_INFO, "[SESSION] Destroyed session %u\n", sid);
    return SESSION_OK;
}

uint32_t session_get_leader(uint32_t sid) {
    if (!session_subsystem_init) return 0;

    spinlock_acquire(&session_lock);
    session_entry_t* s = find_session(sid);
    uint32_t leader = s ? s->leader_pid : 0;
    spinlock_release(&session_lock);
    return leader;
}

bool session_exists(uint32_t sid) {
    if (!session_subsystem_init) return false;

    spinlock_acquire(&session_lock);
    bool exists = find_session(sid) != NULL;
    spinlock_release(&session_lock);
    return exists;
}

/* ---- Public API: Process group ---------------------------------------- */

int process_group_create(uint32_t pgid, uint32_t sid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;
    if (pgid == 0) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    /* Session must exist */
    if (!find_session(sid)) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }

    /* Check for duplicates */
    if (find_pgrp(pgid)) {
        spinlock_release(&session_lock);
        return SESSION_ERR_EXISTS;
    }

    /* Find free slot */
    for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
        if (!pgrp_table[i].in_use) {
            pgrp_table[i].pgid         = pgid;
            pgrp_table[i].session_id   = sid;
            pgrp_table[i].leader_pid   = pgid;
            pgrp_table[i].member_count = 0;
            pgrp_table[i].in_use       = true;
            for (uint32_t j = 0; j < MAX_PGRP_MEMBERS; j++) {
                pgrp_table[i].members[j] = 0;
            }
            spinlock_release(&session_lock);
            debuglog(DEBUG_INFO, "[SESSION] Created group %u in session %u\n",
                     pgid, sid);
            return SESSION_OK;
        }
    }

    spinlock_release(&session_lock);
    return SESSION_ERR_NOMEM;
}

int process_group_destroy(uint32_t pgid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);
    process_group_entry_t* g = find_pgrp(pgid);
    if (!g) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }
    g->in_use = false;
    spinlock_release(&session_lock);
    debuglog(DEBUG_INFO, "[SESSION] Destroyed group %u\n", pgid);
    return SESSION_OK;
}

int process_group_add(uint32_t pgid, uint32_t pid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;
    if (pgid == 0 || pid == 0) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    process_group_entry_t* g = find_pgrp(pgid);
    if (!g) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }

    /* Check if already a member */
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->members[i] == pid) {
            spinlock_release(&session_lock);
            return SESSION_OK; /* already in group */
        }
    }

    /* Add to group */
    if (g->member_count >= MAX_PGRP_MEMBERS) {
        spinlock_release(&session_lock);
        return SESSION_ERR_FULL;
    }

    g->members[g->member_count] = pid;
    g->member_count++;

    /* Update task's pgrp_id if the task exists */
    extern task_t* task_find_by_pid(uint32_t pid);
    task_t* t = task_find_by_pid(pid);
    if (t) {
        t->pgrp_id = pgid;
        t->session_id = g->session_id;
    }

    spinlock_release(&session_lock);
    return SESSION_OK;
}

int process_group_remove(uint32_t pgid, uint32_t pid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    process_group_entry_t* g = find_pgrp(pgid);
    if (!g) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }

    /* Find and remove from members */
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->members[i] == pid) {
            /* Shift remaining members down */
            for (uint32_t j = i; j < g->member_count - 1; j++) {
                g->members[j] = g->members[j + 1];
            }
            g->members[g->member_count - 1] = 0;
            g->member_count--;

            /* Clear task's pgrp_id if the task exists */
            extern task_t* task_find_by_pid(uint32_t pid);
            task_t* t = task_find_by_pid(pid);
            if (t) {
                t->pgrp_id = 0;
            }

            spinlock_release(&session_lock);
            return SESSION_OK;
        }
    }

    spinlock_release(&session_lock);
    return SESSION_ERR_NOTFOUND;
}

uint32_t process_group_get_session(uint32_t pgid) {
    if (!session_subsystem_init) return 0;

    spinlock_acquire(&session_lock);
    process_group_entry_t* g = find_pgrp(pgid);
    uint32_t sid = g ? g->session_id : 0;
    spinlock_release(&session_lock);
    return sid;
}

uint32_t process_group_get_leader(uint32_t pgid) {
    if (!session_subsystem_init) return 0;

    spinlock_acquire(&session_lock);
    process_group_entry_t* g = find_pgrp(pgid);
    uint32_t leader = g ? g->leader_pid : 0;
    spinlock_release(&session_lock);
    return leader;
}

bool process_group_is_member(uint32_t pgid, uint32_t pid) {
    if (!session_subsystem_init) return false;

    spinlock_acquire(&session_lock);
    process_group_entry_t* g = find_pgrp(pgid);
    if (!g) {
        spinlock_release(&session_lock);
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->members[i] == pid) {
            found = true;
            break;
        }
    }

    spinlock_release(&session_lock);
    return found;
}

/* ---- Convenience: task assignment ------------------------------------- */

int session_assign_task(uint32_t pid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;
    if (pid == 0) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    /* Find the task */
    extern task_t* task_find_by_pid(uint32_t pid);
    task_t* t = task_find_by_pid(pid);
    if (!t) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }

    /* Create session if it doesn't exist */
    if (!find_session(pid)) {
        uint32_t i;
        for (i = 0; i < MAX_SESSIONS; i++) {
            if (!session_table[i].in_use) {
                session_table[i].session_id = pid;
                session_table[i].leader_pid = pid;
                session_table[i].in_use     = true;
                break;
            }
        }
        if (i == MAX_SESSIONS) {
            spinlock_release(&session_lock);
            return SESSION_ERR_NOMEM;
        }
    }

    /* Create process group if it doesn't exist */
    if (!find_pgrp(pid)) {
        uint32_t i;
        for (i = 0; i < MAX_PROCESS_GROUPS; i++) {
            if (!pgrp_table[i].in_use) {
                pgrp_table[i].pgid         = pid;
                pgrp_table[i].session_id   = pid;
                pgrp_table[i].leader_pid   = pid;
                pgrp_table[i].member_count = 0;
                pgrp_table[i].in_use       = true;
                break;
            }
        }
        if (i == MAX_PROCESS_GROUPS) {
            spinlock_release(&session_lock);
            return SESSION_ERR_NOMEM;
        }
    }

    /* Add task to the process group (also sets session_id on the task) */
    process_group_entry_t* g = find_pgrp(pid);
    if (g && g->member_count < MAX_PGRP_MEMBERS) {
        bool already_member = false;
        for (uint32_t i = 0; i < g->member_count; i++) {
            if (g->members[i] == pid) {
                already_member = true;
                break;
            }
        }
        if (!already_member) {
            g->members[g->member_count] = pid;
            g->member_count++;
        }
    }

    /* Set task fields */
    t->session_id     = pid;
    t->pgrp_id        = pid;
    t->session_leader = true;

    spinlock_release(&session_lock);
    debuglog(DEBUG_INFO, "[SESSION] Task %u assigned to session/group %u (leader)\n",
             pid, pid);
    return SESSION_OK;
}

int session_add_task_to_group(uint32_t pgid, uint32_t pid) {
    if (!session_subsystem_init) return SESSION_ERR_INVAL;
    if (pgid == 0 || pid == 0) return SESSION_ERR_INVAL;

    spinlock_acquire(&session_lock);

    process_group_entry_t* g = find_pgrp(pgid);
    if (!g) {
        spinlock_release(&session_lock);
        return SESSION_ERR_NOTFOUND;
    }

    /* Add to group members */
    if (g->member_count < MAX_PGRP_MEMBERS) {
        bool already_member = false;
        for (uint32_t i = 0; i < g->member_count; i++) {
            if (g->members[i] == pid) {
                already_member = true;
                break;
            }
        }
        if (!already_member) {
            g->members[g->member_count] = pid;
            g->member_count++;
        }
    }

    /* Update task fields */
    extern task_t* task_find_by_pid(uint32_t pid);
    task_t* t = task_find_by_pid(pid);
    if (t) {
        t->pgrp_id    = pgid;
        t->session_id = g->session_id;
    }

    spinlock_release(&session_lock);
    return SESSION_OK;
}

void session_remove_task(uint32_t pid) {
    if (!session_subsystem_init || pid == 0) return;

    spinlock_acquire(&session_lock);

    extern task_t* task_find_by_pid(uint32_t pid);
    task_t* t = task_find_by_pid(pid);
    uint32_t sid   = t ? t->session_id : 0;
    uint32_t pgid  = t ? t->pgrp_id    : 0;
    bool    leader = t ? t->session_leader : false;

    /* Remove from process group */
    if (pgid != 0) {
        process_group_entry_t* g = find_pgrp(pgid);
        if (g) {
            for (uint32_t i = 0; i < g->member_count; i++) {
                if (g->members[i] == pid) {
                    for (uint32_t j = i; j < g->member_count - 1; j++) {
                        g->members[j] = g->members[j + 1];
                    }
                    g->members[g->member_count - 1] = 0;
                    g->member_count--;
                    break;
                }
            }

            /* If group is now empty, destroy it */
            if (g->member_count == 0) {
                debuglog(DEBUG_INFO, "[SESSION] Group %u empty, destroying\n", pgid);
                g->in_use = false;
            }
        }
    }

    /* If this was the session leader and session has no more groups, destroy it */
    if (leader && sid != 0) {
        bool has_groups = false;
        for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
            if (pgrp_table[i].in_use && pgrp_table[i].session_id == sid) {
                has_groups = true;
                break;
            }
        }
        if (!has_groups) {
            session_entry_t* s = find_session(sid);
            if (s) {
                debuglog(DEBUG_INFO, "[SESSION] Session %u empty, destroying\n", sid);
                s->in_use = false;
            }
        }
    }

    /* Clear task fields */
    if (t) {
        t->session_id     = 0;
        t->pgrp_id        = 0;
        t->session_leader = false;
    }

    spinlock_release(&session_lock);
}

/* ---- Query helpers ---------------------------------------------------- */

uint32_t session_get_task_session(uint32_t pid) {
    if (!session_subsystem_init || pid == 0) return 0;

    extern task_t* task_find_by_pid(uint32_t pid);
    task_t* t = task_find_by_pid(pid);
    return t ? t->session_id : 0;
}

uint32_t session_get_task_pgrp(uint32_t pid) {
    if (!session_subsystem_init || pid == 0) return 0;

    extern task_t* task_find_by_pid(uint32_t pid);
    task_t* t = task_find_by_pid(pid);
    return t ? t->pgrp_id : 0;
}

bool session_pgrp_is_orphaned(uint32_t pgid, uint32_t sid) {
    if (!session_subsystem_init) return true;
    if (pgid == 0 || sid == 0) return true;

    spinlock_acquire(&session_lock);

    /* Check all tasks: a group is NOT orphaned if any member has a parent
     * that is alive, in the same session, but in a different process group.
     * That parent is the "job control shell" that could fg/bg this group. */
    extern task_t* task_find_by_pid(uint32_t pid);
    bool orphaned = true;

    for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
        if (!pgrp_table[i].in_use || pgrp_table[i].session_id != sid) {
            continue;
        }
        if (pgrp_table[i].pgid == pgid) {
            /* Check each member of this group */
            for (uint32_t j = 0; j < pgrp_table[i].member_count; j++) {
                uint32_t member_pid = pgrp_table[i].members[j];
                task_t* member = task_find_by_pid(member_pid);
                if (!member) continue;

                /* Check if member's parent is in the session but different group */
                task_t* parent = task_find_by_pid(member->ppid);
                if (parent && parent->session_id == sid && parent->pgrp_id != pgid) {
                    orphaned = false;
                    break;
                }
            }
            break;
        }
    }

    spinlock_release(&session_lock);
    return orphaned;
}

/* ---- Debug / diagnostics ---------------------------------------------- */

void session_print_status(void) {
    if (!session_subsystem_init) return;

    spinlock_acquire(&session_lock);

    debuglog(DEBUG_INFO, "[SESSION] === Session Table ===\n");
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (session_table[i].in_use) {
            debuglog(DEBUG_INFO, "[SESSION]   Session %u: leader=PID%u\n",
                     session_table[i].session_id, session_table[i].leader_pid);
        }
    }

    debuglog(DEBUG_INFO, "[SESSION] === Process Group Table ===\n");
    for (uint32_t i = 0; i < MAX_PROCESS_GROUPS; i++) {
        if (pgrp_table[i].in_use) {
            debuglog(DEBUG_INFO, "[SESSION]   Group %u: session=%u leader=PID%u members=%u\n",
                     pgrp_table[i].pgid, pgrp_table[i].session_id,
                     pgrp_table[i].leader_pid, pgrp_table[i].member_count);
        }
    }

    spinlock_release(&session_lock);
}

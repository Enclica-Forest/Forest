/**
 * Forest-OS System V Semaphores Implementation
 * Provides POSIX-style semaphore operations for inter-process synchronization
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/memory_safe.h"
#include "include/util.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/timer.h"
#include "include/errno_defs.h"

typedef int32_t key_t;

struct sembuf {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
};

union semun {
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};

struct semid_ds {
    uint32_t sem_otime;
    uint32_t sem_ctime;
};

#ifndef IPC_PRIVATE
#define IPC_PRIVATE 0
#endif
#ifndef IPC_CREAT
#define IPC_CREAT 01000
#endif
#ifndef IPC_EXCL
#define IPC_EXCL 02000
#endif
#ifndef IPC_NOWAIT
#define IPC_NOWAIT 04000
#endif
#ifndef IPC_RMID
#define IPC_RMID 0
#endif
#ifndef IPC_SET
#define IPC_SET 1
#endif
#ifndef IPC_STAT
#define IPC_STAT 2
#endif
#ifndef GETVAL
#define GETVAL 12
#endif
#ifndef SETVAL
#define SETVAL 16
#endif
#ifndef GETPID
#define GETPID 11
#endif
#ifndef GETNCNT
#define GETNCNT 14
#endif
#ifndef GETZCNT
#define GETZCNT 15
#endif
#ifndef GETALL
#define GETALL 13
#endif
#ifndef SETALL
#define SETALL 17
#endif
#ifndef SEMVMX
#define SEMVMX 32767
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EFAULT
#define EFAULT 14
#endif

// Maximum number of semaphore sets
#define MAX_SEM_SETS 64
#define MAX_SEMS_PER_SET 32
#define MAX_SEM_OPS 64

// Semaphore operation structure
typedef struct {
    int sem_num;      // Semaphore number
    short sem_op;     // Operation (positive = increment, negative = decrement, 0 = wait for zero)
    short sem_flg;    // Flags (IPC_NOWAIT, SEM_UNDO)
} semop_entry_t;

// Semaphore value structure
typedef struct {
    uint16_t semval;    // Current value
    uint16_t sempid;    // PID of last operation
    uint32_t semctime;  // Last change time
    uint16_t semncnt;   // Number of processes waiting for semval > current
    uint16_t semzcnt;   // Number of processes waiting for semval == 0
} sem_t;

// Semaphore set structure
typedef struct {
    bool used;
    key_t key;           // IPC key
    int semid;           // Semaphore set ID
    uint32_t creator_pid; // Creating task's PID, for cleanup on task exit
    uint16_t nsems;      // Number of semaphores in set
    uint16_t sem_perm;   // Permissions
    uint32_t sem_otime;  // Last semop time
    uint32_t sem_ctime;  // Last change time
    sem_t sems[MAX_SEMS_PER_SET];  // Array of semaphores
    uint32_t refcount;   // Reference count
} sem_set_t;

// Global semaphore sets
static sem_set_t g_sem_sets[MAX_SEM_SETS];
static spinlock_t g_sem_lock;
static int g_next_semid = 0;

// Initialize semaphore subsystem
void sysv_sem_init(void) {
    spinlock_init(&g_sem_lock, "sysv_sem");
    memory_set((uint8*)g_sem_sets, 0, sizeof(g_sem_sets));
    debuglog(DEBUG_INFO, "[SYSV_SEM] System V semaphores initialized\n");
}

// Find semaphore set by ID
static sem_set_t* find_sem_set_by_id(int semid) {
    for (int i = 0; i < MAX_SEM_SETS; i++) {
        if (g_sem_sets[i].used && g_sem_sets[i].semid == semid) {
            return &g_sem_sets[i];
        }
    }
    return NULL;
}

// Find semaphore set by key
static sem_set_t* find_sem_set_by_key(key_t key, bool find_empty) {
    for (int i = 0; i < MAX_SEM_SETS; i++) {
        if (g_sem_sets[i].used && g_sem_sets[i].key == key) {
            return &g_sem_sets[i];
        }
        if (find_empty && !g_sem_sets[i].used) {
            return &g_sem_sets[i];
        }
    }
    return NULL;
}

// sys_semget - Get semaphore set
int sysv_semget(key_t key, int nsems, int semflg) {
    spinlock_acquire(&g_sem_lock);
    
    sem_set_t* sem_set = NULL;
    
    // Check if semaphore set already exists
    if (key != IPC_PRIVATE) {
        sem_set = find_sem_set_by_key(key, false);
        if (sem_set) {
            // Semaphore set exists
            if ((semflg & IPC_EXCL) && (semflg & IPC_CREAT)) {
                // IPC_EXCL and IPC_CREAT specified but set exists
                spinlock_release(&g_sem_lock);
                return -EEXIST;
            }
            // Return existing set
            int result = sem_set->semid;
            spinlock_release(&g_sem_lock);
            return result;
        }
    }
    
    // Create new semaphore set
    if ((semflg & IPC_CREAT) || (semflg & IPC_EXCL)) {
        // Find empty slot
        sem_set = find_sem_set_by_key(0, true);
        if (!sem_set) {
            spinlock_release(&g_sem_lock);
            return -ENOSPC;  // No space for new semaphore set
        }
        
        // Validate nsems
        if (nsems <= 0 || nsems > MAX_SEMS_PER_SET) {
            spinlock_release(&g_sem_lock);
            return -EINVAL;
        }
        
        // Initialize semaphore set
        sem_set->used = true;
        sem_set->key = key;
        sem_set->semid = g_next_semid++;
        sem_set->creator_pid = current_task ? current_task->id : 0;
        sem_set->nsems = nsems;
        sem_set->sem_perm = (semflg & 0777);
        sem_set->sem_otime = 0;
        sem_set->sem_ctime = timer_get_ticks() / 1000;
        sem_set->refcount = 0;
        
        // Initialize individual semaphores
        for (int i = 0; i < nsems; i++) {
            sem_set->sems[i].semval = 0;
            sem_set->sems[i].sempid = 0;
            sem_set->sems[i].semctime = 0;
            sem_set->sems[i].semncnt = 0;
            sem_set->sems[i].semzcnt = 0;
        }
        
        int result = sem_set->semid;
        spinlock_release(&g_sem_lock);
        return result;
    }
    
    spinlock_release(&g_sem_lock);
    return -ENOENT;  // Semaphore set does not exist
}

// Perform semaphore operations
static int do_semop(sem_set_t* sem_set, semop_entry_t* ops, int nops, bool wait) {
    task_t* task = current_task;
    uint32_t task_id = task ? task->id : 0;
    
    // First pass: check if operations can succeed
    for (int i = 0; i < nops; i++) {
        int snum = ops[i].sem_num;
        short sem_op = ops[i].sem_op;
        
        if (snum < 0 || snum >= sem_set->nsems) {
            return -ERANGE;
        }
        
        sem_t* sem = &sem_set->sems[snum];
        
        if (sem_op > 0) {
            // Increment - always succeeds (check for overflow)
            if (sem->semval + sem_op > SEMVMX) {
                return -ERANGE;
            }
        } else if (sem_op < 0) {
            // Decrement - need enough value
            if (sem->semval < -sem_op) {
                if (ops[i].sem_flg & IPC_NOWAIT) {
                    return -EAGAIN;
                }
                // Would block
                if (wait) {
                    sem->semncnt++;
                    // In a real implementation, we'd block here
                    // For now, return EAGAIN
                    sem->semncnt--;
                    return -EAGAIN;
                }
            }
        } else {
            // Wait for zero
            if (sem->semval != 0) {
                if (ops[i].sem_flg & IPC_NOWAIT) {
                    return -EAGAIN;
                }
                // Would block
                if (wait) {
                    sem->semzcnt++;
                    sem->semzcnt--;
                    return -EAGAIN;
                }
            }
        }
    }
    
    // Second pass: perform operations
    for (int i = 0; i < nops; i++) {
        int snum = ops[i].sem_num;
        short sem_op = ops[i].sem_op;
        
        sem_t* sem = &sem_set->sems[snum];
        
        if (sem_op != 0) {
            sem->semval += sem_op;
            sem->sempid = task_id;
            sem->semctime = timer_get_ticks() / 1000;
        }
    }
    
    sem_set->sem_otime = timer_get_ticks() / 1000;
    return 0;
}

// sys_semop - Perform semaphore operations
int sysv_semop(int semid, struct sembuf* sops, int nsops) {
    if (!sops || nsops <= 0) {
        return -EINVAL;
    }
    
    // Validate user buffer
    if (!memory_probe_user_buffer(sops, nsops * sizeof(struct sembuf))) {
        return -EFAULT;
    }
    
    spinlock_acquire(&g_sem_lock);
    
    sem_set_t* sem_set = find_sem_set_by_id(semid);
    if (!sem_set) {
        spinlock_release(&g_sem_lock);
        return -EINVAL;
    }
    
    // Convert user sops to kernel format
    semop_entry_t ops[MAX_SEM_OPS];
    int nops = nsops < MAX_SEM_OPS ? nsops : MAX_SEM_OPS;
    
    for (int i = 0; i < nops; i++) {
        ops[i].sem_num = sops[i].sem_num;
        ops[i].sem_op = sops[i].sem_op;
        ops[i].sem_flg = sops[i].sem_flg;
    }
    
    int result = do_semop(sem_set, ops, nops, true);
    spinlock_release(&g_sem_lock);
    
    return result;
}

// sys_semctl - Semaphore control operations
int sysv_semctl(int semid, int semnum, int cmd, union semun* arg) {
    spinlock_acquire(&g_sem_lock);
    
    sem_set_t* sem_set = find_sem_set_by_id(semid);
    if (!sem_set) {
        spinlock_release(&g_sem_lock);
        return -EINVAL;
    }
    
    int result = 0;
    
    switch (cmd) {
        case IPC_STAT: {
            // Get semaphore set status
            if (!arg || !memory_probe_user_buffer(arg, sizeof(struct semid_ds))) {
                result = -EFAULT;
                break;
            }
            // Would fill in semid_ds structure
            result = 0;
            break;
        }
        
        case IPC_SET: {
            // Set semaphore set attributes
            if (!arg || !memory_probe_user_buffer(arg, sizeof(struct semid_ds))) {
                result = -EFAULT;
                break;
            }
            // Would set attributes
            sem_set->sem_ctime = timer_get_ticks() / 1000;
            result = 0;
            break;
        }
        
        case IPC_RMID: {
            // Remove semaphore set
            if (sem_set->refcount > 0) {
                result = -EBUSY;
            } else {
                memory_set((uint8*)sem_set, 0, sizeof(sem_set_t));
                result = 0;
            }
            break;
        }
        
        case GETVAL: {
            // Get semaphore value
            if (semnum < 0 || semnum >= sem_set->nsems) {
                result = -ERANGE;
            } else {
                result = sem_set->sems[semnum].semval;
            }
            break;
        }
        
        case SETVAL: {
            // Set semaphore value
            if (semnum < 0 || semnum >= sem_set->nsems) {
                result = -ERANGE;
            } else if (!arg) {
                result = -EFAULT;
            } else {
                // Validate value
                uint16_t val = (uint16_t)arg->val;
                if (val > SEMVMX) {
                    result = -ERANGE;
                } else {
                    sem_set->sems[semnum].semval = val;
                    sem_set->sems[semnum].sempid = current_task ? current_task->id : 0;
                    sem_set->sems[semnum].semctime = timer_get_ticks() / 1000;
                    result = 0;
                }
            }
            break;
        }
        
        case GETPID: {
            // Get PID of last operation
            if (semnum < 0 || semnum >= sem_set->nsems) {
                result = -ERANGE;
            } else {
                result = sem_set->sems[semnum].sempid;
            }
            break;
        }
        
        case GETNCNT: {
            // Get number of waiting processes
            if (semnum < 0 || semnum >= sem_set->nsems) {
                result = -ERANGE;
            } else {
                result = sem_set->sems[semnum].semncnt;
            }
            break;
        }
        
        case GETZCNT: {
            // Get number of processes waiting for zero
            if (semnum < 0 || semnum >= sem_set->nsems) {
                result = -ERANGE;
            } else {
                result = sem_set->sems[semnum].semzcnt;
            }
            break;
        }
        
        case GETALL: {
            // Get all semaphore values
            if (!arg || !memory_probe_user_buffer(arg, sem_set->nsems * sizeof(unsigned short))) {
                result = -EFAULT;
            } else {
                unsigned short* array = arg->array;
                for (int i = 0; i < sem_set->nsems; i++) {
                    array[i] = sem_set->sems[i].semval;
                }
                result = 0;
            }
            break;
        }
        
        case SETALL: {
            // Set all semaphore values
            if (!arg || !memory_probe_user_buffer(arg, sem_set->nsems * sizeof(unsigned short))) {
                result = -EFAULT;
            } else {
                unsigned short* array = arg->array;
                for (int i = 0; i < sem_set->nsems; i++) {
                    if (array[i] > SEMVMX) {
                        result = -ERANGE;
                        break;
                    }
                    sem_set->sems[i].semval = array[i];
                    sem_set->sems[i].sempid = current_task ? current_task->id : 0;
                }
                if (result != -ERANGE) {
                    sem_set->sem_ctime = timer_get_ticks() / 1000;
                    result = 0;
                }
            }
            break;
        }
        
        default:
            result = -EINVAL;
    }

    spinlock_release(&g_sem_lock);
    return result;
}

// sysv_sem_close_all_for_task - Remove every semaphore set created by `pid`
// that hasn't been explicitly removed via IPC_RMID. Same rationale as
// sysv_msg_close_all_for_task(): these sets have no owner-exit cleanup by
// default and would otherwise permanently occupy a slot out of the fixed
// MAX_SEM_SETS pool once their creator is gone.
void sysv_sem_close_all_for_task(uint32_t pid) {
    spinlock_acquire(&g_sem_lock);

    for (int i = 0; i < MAX_SEM_SETS; i++) {
        if (g_sem_sets[i].used && g_sem_sets[i].creator_pid == pid) {
            memory_set((uint8*)&g_sem_sets[i], 0, sizeof(sem_set_t));
        }
    }

    spinlock_release(&g_sem_lock);
}

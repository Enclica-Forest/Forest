/**
 * Forest-OS System V Message Queues Implementation
 * Provides POSIX-style message queue operations for inter-process communication
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

struct ipc_perm {
    key_t __key;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
};

struct msqid_ds {
    struct ipc_perm msg_perm;
    uint32_t msg_stime;
    uint32_t msg_rtime;
    uint32_t msg_ctime;
    uint32_t msg_qnum;
    uint32_t msg_qbytes;
    uint32_t msg_lspid;
    uint32_t msg_lrpid;
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
#ifndef MSG_NOERROR
#define MSG_NOERROR 010000
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
#ifndef E2BIG
#define E2BIG 7
#endif

// Maximum number of message queues
#define MAX_MSG_QUEUES 64
#define MAX_MSG_SIZE 8192
#define MAX_MSGS_PER_QUEUE 256

// Message structure
typedef struct {
    long mtype;           // Message type (must be > 0)
    char mtext[1];        // Message data (variable size)
} msg_t;

// Message queue structure
typedef struct {
    bool used;
    key_t key;           // IPC key
    int msqid;           // Message queue ID
    uint32_t creator_pid; // Creating task's PID, for cleanup on task exit
    struct msgq* msg_first;   // First message in queue
    struct msgq* msg_last;    // Last message in queue
    uint32_t qnum;       // Number of messages in queue
    uint32_t qbytes;     // Max bytes in queue
    uint16_t perms;      // Permissions
    uint32_t lspid;      // Last sender PID
    uint32_t lrpid;      // Last receiver PID
    uint32_t stime;      // Last send time
    uint32_t rtime;      // Last receive time
    uint32_t ctime;      // Last change time
} msg_queue_t;

// Internal message queue entry
typedef struct msgq {
    struct msgq* next;
    long mtype;
    uint32_t msize;      // Size of message data
    char data[MAX_MSG_SIZE];
} msgq_t;

// Global message queues
static msg_queue_t g_msg_queues[MAX_MSG_QUEUES];
static spinlock_t g_msg_lock;
static int g_next_msqid = 0;

// Initialize message queue subsystem
void sysv_msg_init(void) {
    spinlock_init(&g_msg_lock, "sysv_msg");
    memory_set((uint8*)g_msg_queues, 0, sizeof(g_msg_queues));
    debuglog(DEBUG_INFO, "[SYSV_MSG] System V message queues initialized\n");
}

// Find message queue by ID
static msg_queue_t* find_msg_queue_by_id(int msqid) {
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        if (g_msg_queues[i].used && g_msg_queues[i].msqid == msqid) {
            return &g_msg_queues[i];
        }
    }
    return NULL;
}

// Find message queue by key
static msg_queue_t* find_msg_queue_by_key(key_t key, bool find_empty) {
    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        if (g_msg_queues[i].used && g_msg_queues[i].key == key) {
            return &g_msg_queues[i];
        }
        if (find_empty && !g_msg_queues[i].used) {
            return &g_msg_queues[i];
        }
    }
    return NULL;
}

// sys_msgget - Get message queue
int sysv_msgget(key_t key, int msgflg) {
    spinlock_acquire(&g_msg_lock);
    
    msg_queue_t* msgq = NULL;
    
    // Check if message queue already exists
    if (key != IPC_PRIVATE) {
        msgq = find_msg_queue_by_key(key, false);
        if (msgq) {
            // Message queue exists
            if ((msgflg & IPC_EXCL) && (msgflg & IPC_CREAT)) {
                spinlock_release(&g_msg_lock);
                return -EEXIST;
            }
            int result = msgq->msqid;
            spinlock_release(&g_msg_lock);
            return result;
        }
    }
    
    // Create new message queue
    if ((msgflg & IPC_CREAT) || (msgflg & IPC_EXCL)) {
        msgq = find_msg_queue_by_key(0, true);
        if (!msgq) {
            spinlock_release(&g_msg_lock);
            return -ENOSPC;
        }
        
        // Initialize message queue
        msgq->used = true;
        msgq->key = key;
        msgq->msqid = g_next_msqid++;
        msgq->creator_pid = current_task ? current_task->id : 0;
        msgq->msg_first = NULL;
        msgq->msg_last = NULL;
        msgq->qnum = 0;
        msgq->qbytes = MAX_MSG_SIZE * MAX_MSGS_PER_QUEUE; // Default max size
        msgq->perms = (msgflg & 0777);
        msgq->lspid = 0;
        msgq->lrpid = 0;
        msgq->stime = 0;
        msgq->rtime = 0;
        msgq->ctime = timer_get_ticks() / 1000;
        
        int result = msgq->msqid;
        spinlock_release(&g_msg_lock);
        return result;
    }
    
    spinlock_release(&g_msg_lock);
    return -ENOENT;
}

// sys_msgsnd - Send message to queue
int sysv_msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg) {
    if (!msgp) {
        return -EFAULT;
    }
    
    // Get message type from user buffer
    if (!memory_probe_user_buffer(msgp, sizeof(long))) {
        return -EFAULT;
    }
    
    long mtype = *(const long*)msgp;
    if (mtype <= 0) {
        return -EINVAL;
    }
    
    // Validate message size
    if (msgsz > MAX_MSG_SIZE) {
        return -EINVAL;
    }
    
    spinlock_acquire(&g_msg_lock);
    
    msg_queue_t* msgq = find_msg_queue_by_id(msqid);
    if (!msgq) {
        spinlock_release(&g_msg_lock);
        return -EINVAL;
    }
    
    // Check queue size limit
    uint32_t total_size = msgq->qnum * sizeof(msgq_t) + msgsz;
    if (total_size > msgq->qbytes) {
        // Queue full
        if (msgflg & IPC_NOWAIT) {
            spinlock_release(&g_msg_lock);
            return -EAGAIN;
        }
        // Would block - for now return EAGAIN
        spinlock_release(&g_msg_lock);
        return -EAGAIN;
    }
    
    // Allocate message
    msgq_t* new_msg = (msgq_t*)kmalloc(sizeof(msgq_t));
    if (!new_msg) {
        spinlock_release(&g_msg_lock);
        return -ENOMEM;
    }
    
    // Copy message data
    new_msg->mtype = mtype;
    new_msg->msize = msgsz;
    new_msg->next = NULL;
    
    // Copy message text (skip mtype at start)
    const char* src = (const char*)msgp + sizeof(long);
    memory_copy(src, new_msg->data, msgsz);
    
    // Add to queue
    if (msgq->msg_last) {
        msgq->msg_last->next = new_msg;
        msgq->msg_last = new_msg;
    } else {
        msgq->msg_first = new_msg;
        msgq->msg_last = new_msg;
    }
    
    msgq->qnum++;
    msgq->stime = timer_get_ticks() / 1000;
    msgq->lspid = current_task ? current_task->id : 0;
    
    spinlock_release(&g_msg_lock);
    return 0;
}

// sys_msgrcv - Receive message from queue
int sysv_msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg) {
    if (!msgp) {
        return -EFAULT;
    }
    
    spinlock_acquire(&g_msg_lock);
    
    msg_queue_t* msgq = find_msg_queue_by_id(msqid);
    if (!msgq) {
        spinlock_release(&g_msg_lock);
        return -EINVAL;
    }
    
    // Check for messages
    if (!msgq->msg_first) {
        spinlock_release(&g_msg_lock);
        if (msgflg & IPC_NOWAIT) {
            return -EAGAIN;
        }
        // Would block - for now return EAGAIN
        return -EAGAIN;
    }
    
    // Find message matching type
    // msgtyp = 0: receive first message
    // msgtyp > 0: receive first message of type msgtyp
    // msgtyp < 0: receive first message with type <= |msgtyp|
    
    msgq_t* prev = NULL;
    msgq_t* msg = msgq->msg_first;
    msgq_t* found_msg = NULL;
    msgq_t* found_prev = NULL;
    
    while (msg) {
        bool match = false;
        if (msgtyp == 0) {
            match = true;
        } else if (msgtyp > 0) {
            match = (msg->mtype == msgtyp);
        } else {
            match = (msg->mtype <= -msgtyp);
        }
        
        if (match) {
            found_msg = msg;
            found_prev = prev;
            break;
        }
        
        prev = msg;
        msg = msg->next;
    }
    
    if (!found_msg) {
        spinlock_release(&g_msg_lock);
        if (msgflg & IPC_NOWAIT) {
            return -EAGAIN;
        }
        return -EAGAIN;
    }
    
    // Check message size
    if (found_msg->msize > msgsz) {
        spinlock_release(&g_msg_lock);
        if (msgflg & MSG_NOERROR) {
            // Return truncated message
        } else {
            return -E2BIG;
        }
    }
    
    // Copy message to user
    // Copy type first
    *(long*)msgp = found_msg->mtype;
    
    // Copy data
    size_t copy_size = found_msg->msize;
    if (copy_size > msgsz) {
        copy_size = msgsz;
    }
    char* dest = (char*)msgp + sizeof(long);
    memory_copy(found_msg->data, dest, copy_size);
    
    // Remove from queue
    if (found_prev) {
        found_prev->next = found_msg->next;
    } else {
        msgq->msg_first = found_msg->next;
    }
    if (found_msg == msgq->msg_last) {
        msgq->msg_last = found_prev;
    }
    
    msgq->qnum--;
    msgq->rtime = timer_get_ticks() / 1000;
    msgq->lrpid = current_task ? current_task->id : 0;
    
    // Free the message
    kfree(found_msg);
    
    spinlock_release(&g_msg_lock);
    
    return copy_size + sizeof(long);
}

// sys_msgctl - Message queue control
int sysv_msgctl(int msqid, int cmd, struct msqid_ds* buf) {
    spinlock_acquire(&g_msg_lock);
    
    msg_queue_t* msgq = find_msg_queue_by_id(msqid);
    if (!msgq) {
        spinlock_release(&g_msg_lock);
        return -EINVAL;
    }
    
    int result = 0;
    
    switch (cmd) {
        case IPC_STAT: {
            // Get message queue status
            if (!buf || !memory_probe_user_buffer(buf, sizeof(struct msqid_ds))) {
                result = -EFAULT;
                break;
            }
            // Fill in msqid_ds structure
            buf->msg_perm.__key = msgq->key;
            buf->msg_perm.uid = 0;
            buf->msg_perm.gid = 0;
            buf->msg_perm.mode = msgq->perms;
            buf->msg_qnum = msgq->qnum;
            buf->msg_qbytes = msgq->qbytes;
            buf->msg_lspid = msgq->lspid;
            buf->msg_lrpid = msgq->lrpid;
            buf->msg_stime = msgq->stime;
            buf->msg_rtime = msgq->rtime;
            buf->msg_ctime = msgq->ctime;
            result = 0;
            break;
        }
        
        case IPC_SET: {
            // Set message queue attributes
            if (!buf || !memory_probe_user_buffer(buf, sizeof(struct msqid_ds))) {
                result = -EFAULT;
                break;
            }
            msgq->perms = buf->msg_perm.mode & 0777;
            if (buf->msg_qbytes > 0) {
                msgq->qbytes = buf->msg_qbytes;
            }
            msgq->ctime = timer_get_ticks() / 1000;
            result = 0;
            break;
        }
        
        case IPC_RMID: {
            // Remove message queue
            // Free all messages
            msgq_t* msg = msgq->msg_first;
            while (msg) {
                msgq_t* next = msg->next;
                kfree(msg);
                msg = next;
            }
            memory_set((uint8*)msgq, 0, sizeof(msg_queue_t));
            result = 0;
            break;
        }
        
        default:
            result = -EINVAL;
    }
    
    spinlock_release(&g_msg_lock);
    return result;
}

// sysv_msg_close_all_for_task - Remove every message queue created by `pid`
// that hasn't been explicitly removed via IPC_RMID. Without this, a crashed
// or non-cooperative process permanently occupies a slot out of the fixed
// MAX_MSG_QUEUES pool -- the classic SysV IPC leak, since (unlike file
// descriptors) these queues have no kernel-side owner-exit cleanup by
// default.
void sysv_msg_close_all_for_task(uint32_t pid) {
    spinlock_acquire(&g_msg_lock);

    for (int i = 0; i < MAX_MSG_QUEUES; i++) {
        msg_queue_t* msgq = &g_msg_queues[i];
        if (!msgq->used || msgq->creator_pid != pid) {
            continue;
        }

        msgq_t* msg = msgq->msg_first;
        while (msg) {
            msgq_t* next = msg->next;
            kfree(msg);
            msg = next;
        }
        memory_set((uint8*)msgq, 0, sizeof(msg_queue_t));
    }

    spinlock_release(&g_msg_lock);
}

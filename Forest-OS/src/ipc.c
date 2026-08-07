#include "include/ipc.h"
#include "include/task.h"
#include "include/memory.h"
#include "include/timer.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/util.h"
#include "include/debuglog.h"

static ipc_channel_t ipc_channels[IPC_MAX_CHANNELS];
static uint32 ipc_next_channel_id = 1;
static spinlock_t ipc_global_lock = SPINLOCK_INIT("ipc_global");

bool ipc_init(void) {
    memory_set((uint8*)ipc_channels, 0, sizeof(ipc_channels));
    ipc_next_channel_id = 1;
    print("[IPC] Initialized channel-based IPC\n");
    return true;
}

static ipc_channel_t* ipc_find_channel(uint32 channel_id) {
    for (uint32 i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (ipc_channels[i].id == channel_id) {
            return &ipc_channels[i];
        }
    }
    return 0;
}

uint32 ipc_create_channel(uint32 owner_pid) {
    spinlock_acquire(&ipc_global_lock);

    for (uint32 i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (ipc_channels[i].id == 0) {
            ipc_channels[i].id = ipc_next_channel_id++;
            if (ipc_channels[i].id == 0) {
                ipc_channels[i].id = ipc_next_channel_id++;
            }
            ipc_channels[i].owner_pid = owner_pid;
            ipc_channels[i].ref_count = 1;
            ipc_channels[i].head = 0;
            ipc_channels[i].tail = 0;
            memory_set((uint8*)ipc_channels[i].messages, 0, sizeof(ipc_channels[i].messages));
            spinlock_init(&ipc_channels[i].lock, "ipc_channel");

            uint32 id = ipc_channels[i].id;
            spinlock_release(&ipc_global_lock);

            debuglog(DEBUG_INFO, "[IPC] Channel %u created by PID %u\n", id, owner_pid);
            return id;
        }
    }

    spinlock_release(&ipc_global_lock);
    print("[IPC] No free channels\n");
    return 0;
}

bool ipc_open_channel(uint32 channel_id) {
    spinlock_acquire(&ipc_global_lock);

    ipc_channel_t* ch = ipc_find_channel(channel_id);
    if (!ch || ch->id == 0) {
        spinlock_release(&ipc_global_lock);
        return false;
    }

    ch->ref_count++;
    spinlock_release(&ipc_global_lock);

    debuglog(DEBUG_INFO, "[IPC] Channel %u opened (ref_count=%u)\n", channel_id, ch->ref_count);
    return true;
}

void ipc_close_channel(uint32 channel_id) {
    spinlock_acquire(&ipc_global_lock);

    ipc_channel_t* ch = ipc_find_channel(channel_id);
    if (!ch || ch->id == 0) {
        spinlock_release(&ipc_global_lock);
        return;
    }

    if (ch->ref_count > 0) {
        ch->ref_count--;
    }

    if (ch->ref_count == 0) {
        ch->id = 0;
        debuglog(DEBUG_INFO, "[IPC] Channel %u destroyed (no more refs)\n", channel_id);
    }

    spinlock_release(&ipc_global_lock);
}

bool ipc_send(uint32 channel_id, const ipc_msg_t* msg) {
    if (!msg) {
        return false;
    }

    spinlock_acquire(&ipc_global_lock);

    ipc_channel_t* ch = ipc_find_channel(channel_id);
    if (!ch || ch->id == 0) {
        spinlock_release(&ipc_global_lock);
        return false;
    }

    spinlock_acquire(&ch->lock);
    spinlock_release(&ipc_global_lock);

    uint32 next_tail = (ch->tail + 1) % IPC_MAX_MESSAGES;
    if (next_tail == ch->head) {
        spinlock_release(&ch->lock);
        debuglog(DEBUG_WARN, "[IPC] Channel %u full, message dropped\n", channel_id);
        return false;
    }

    memory_copy((const char*)msg, (char*)&ch->messages[ch->tail], sizeof(ipc_msg_t));
    ch->tail = next_tail;

    spinlock_release(&ch->lock);

    debuglog(DEBUG_INFO, "[IPC] Sent msg type=%u on channel %u (size=%u)\n",
             msg->type, channel_id, msg->size);
    return true;
}

bool ipc_receive(uint32 channel_id, ipc_msg_t* msg, uint32 timeout_ms) {
    if (!msg) {
        return false;
    }

    spinlock_acquire(&ipc_global_lock);

    ipc_channel_t* ch = ipc_find_channel(channel_id);
    if (!ch || ch->id == 0) {
        spinlock_release(&ipc_global_lock);
        return false;
    }

    spinlock_acquire(&ch->lock);
    spinlock_release(&ipc_global_lock);

    uint32 start_tick = timer_get_ticks();
    uint32 timeout_ticks = (timeout_ms + 9) / 10;

    while (ch->head == ch->tail) {
        spinlock_release(&ch->lock);

        if (timeout_ms == 0) {
            return false;
        }

        uint32 elapsed = timer_get_ticks() - start_tick;
        if (elapsed >= timeout_ticks) {
            return false;
        }

        timer_sleep_ms(10);

        spinlock_acquire(&ipc_global_lock);
        ch = ipc_find_channel(channel_id);
        if (!ch || ch->id == 0) {
            spinlock_release(&ipc_global_lock);
            return false;
        }
        spinlock_acquire(&ch->lock);
        spinlock_release(&ipc_global_lock);
    }

    memory_copy((const char*)&ch->messages[ch->head], (char*)msg, sizeof(ipc_msg_t));
    ch->head = (ch->head + 1) % IPC_MAX_MESSAGES;

    spinlock_release(&ch->lock);

    debuglog(DEBUG_INFO, "[IPC] Received msg type=%u from channel %u (sender=PID %u)\n",
             msg->type, channel_id, msg->sender_pid);
    return true;
}

void ipc_destroy_all_for_pid(uint32 pid) {
    spinlock_acquire(&ipc_global_lock);

    for (uint32 i = 0; i < IPC_MAX_CHANNELS; i++) {
        if (ipc_channels[i].id != 0 && ipc_channels[i].owner_pid == pid) {
            debuglog(DEBUG_INFO, "[IPC] Destroying channel %u for exiting PID %u\n",
                     ipc_channels[i].id, pid);
            ipc_channels[i].id = 0;
            ipc_channels[i].ref_count = 0;
        }
    }

    spinlock_release(&ipc_global_lock);
}

long sys_ipc_create(void) {
    if (!current_task) {
        return -1;
    }

    uint32 id = ipc_create_channel(current_task->id);
    return (long)id;
}

long sys_ipc_send(uint32 channel_id, const void* user_msg, uint32 size) {
    if (!user_msg || size == 0 || !current_task) {
        return -1;
    }

    if (size > IPC_MSG_DATA_SIZE) {
        size = IPC_MSG_DATA_SIZE;
    }

    ipc_msg_t kmsg;
    memory_set((uint8*)&kmsg, 0, sizeof(ipc_msg_t));
    memory_copy((const char*)user_msg, (char*)&kmsg, size);
    kmsg.sender_pid = current_task->id;
    kmsg.size = size;

    bool ok = ipc_send(channel_id, &kmsg);
    return ok ? 0 : -1;
}

long sys_ipc_receive(uint32 channel_id, void* user_msg, uint32 timeout_ms) {
    if (!user_msg || !current_task) {
        return -1;
    }

    ipc_msg_t kmsg;
    bool ok = ipc_receive(channel_id, &kmsg, timeout_ms);
    if (!ok) {
        return -1;
    }

    memory_copy((const char*)&kmsg, (char*)user_msg, sizeof(ipc_msg_t));
    return (long)kmsg.size;
}

long sys_ipc_close(uint32 channel_id) {
    if (!current_task) {
        return -1;
    }

    ipc_close_channel(channel_id);
    return 0;
}

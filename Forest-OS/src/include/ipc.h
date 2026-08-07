#ifndef IPC_H
#define IPC_H
#include "types.h"
#include "spinlock.h"
#include <stdbool.h>
#define IPC_MAX_CHANNELS 32
#define IPC_MAX_MESSAGES 64
#define IPC_MSG_DATA_SIZE 256
typedef struct { uint32 sender_pid; uint32 type; uint32 size; char data[IPC_MSG_DATA_SIZE]; } ipc_msg_t;
typedef struct { uint32 id; uint32 owner_pid; uint32 ref_count; ipc_msg_t messages[IPC_MAX_MESSAGES]; uint32 head; uint32 tail; spinlock_t lock; } ipc_channel_t;
bool ipc_init(void);
uint32 ipc_create_channel(uint32 owner_pid);
bool ipc_open_channel(uint32 channel_id);
void ipc_close_channel(uint32 channel_id);
bool ipc_send(uint32 channel_id, const ipc_msg_t* msg);
bool ipc_receive(uint32 channel_id, ipc_msg_t* msg, uint32 timeout_ms);
void ipc_destroy_all_for_pid(uint32 pid);
long sys_ipc_create(void);
long sys_ipc_send(uint32 channel_id, const void* user_msg, uint32 size);
long sys_ipc_receive(uint32 channel_id, void* user_msg, uint32 timeout_ms);
long sys_ipc_close(uint32 channel_id);
#endif

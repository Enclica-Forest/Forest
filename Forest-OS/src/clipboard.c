#include "include/clipboard.h"
#include "include/spinlock.h"
#include "include/smep_smap.h"
#include "include/task.h"
#include "include/libc/string.h"
#include "include/errno_defs.h"
#include "include/memory.h"

#define SYSCALL_EFAULT (-14)
#define SYSCALL_EINVAL (-22)

static clipboard_entry_t g_clipboard;
static spinlock_t g_clipboard_lock;

bool clipboard_init(void) {
    memset(&g_clipboard, 0, sizeof(clipboard_entry_t));
    spinlock_init(&g_clipboard_lock, "clipboard_lock");
    return true;
}

bool clipboard_set(clipboard_type_t type, const void* data, uint32 size) {
    if (size > CLIPBOARD_MAX_SIZE) return false;
    if (!data && size > 0) return false;

    spinlock_acquire(&g_clipboard_lock);

    g_clipboard.type = type;
    g_clipboard.size = size;
    g_clipboard.owner_pid = current_task->id;

    if (data && size > 0) {
        memcpy(g_clipboard.data, data, size);
    }

    g_clipboard.valid = true;

    spinlock_release(&g_clipboard_lock);
    return true;
}

const void* clipboard_get(clipboard_type_t type, uint32* out_size) {
    spinlock_acquire(&g_clipboard_lock);

    if (!g_clipboard.valid || g_clipboard.type != type) {
        spinlock_release(&g_clipboard_lock);
        return 0;
    }

    if (out_size) {
        *out_size = g_clipboard.size;
    }

    const void* result = g_clipboard.data;
    spinlock_release(&g_clipboard_lock);
    return result;
}

void clipboard_clear(void) {
    spinlock_acquire(&g_clipboard_lock);
    g_clipboard.valid = false;
    memset(g_clipboard.data, 0, CLIPBOARD_MAX_SIZE);
    g_clipboard.size = 0;
    g_clipboard.owner_pid = 0;
    spinlock_release(&g_clipboard_lock);
}

bool clipboard_has_content(clipboard_type_t type) {
    spinlock_acquire(&g_clipboard_lock);
    bool result = g_clipboard.valid && g_clipboard.type == type;
    spinlock_release(&g_clipboard_lock);
    return result;
}

long sys_clipboard_set(clipboard_type_t type, const void* user_data, uint32 size) {
    if (size > CLIPBOARD_MAX_SIZE) return SYSCALL_EINVAL;
    if (!user_data && size > 0) return SYSCALL_EINVAL;

    uint8* local_buf = (uint8*)kmalloc(size);
    if (!local_buf) return SYSCALL_EINVAL;

    if (user_data && size > 0) {
        USER_ACCESS_BEGIN();
        safe_user_memcpy(local_buf, user_data, size);
        USER_ACCESS_END();
    }

    bool ok = clipboard_set(type, local_buf, size);
    kfree(local_buf);
    return ok ? 0 : SYSCALL_EINVAL;
}

long sys_clipboard_get(clipboard_type_t type, void* user_data, uint32* user_size) {
    if (!user_data) return SYSCALL_EFAULT;

    uint32 k_size = 0;
    const void* data = clipboard_get(type, &k_size);

    if (!data) {
        USER_ACCESS_BEGIN();
        if (user_size) {
            uint32 zero = 0;
            safe_user_memcpy(user_size, &zero, sizeof(uint32));
        }
        USER_ACCESS_END();
        return 0;
    }

    USER_ACCESS_BEGIN();
    safe_user_memcpy(user_data, data, k_size);
    if (user_size) {
        safe_user_memcpy(user_size, &k_size, sizeof(uint32));
    }
    USER_ACCESS_END();

    return k_size;
}

long sys_clipboard_clear(void) {
    clipboard_clear();
    return 0;
}

long sys_clipboard_has(clipboard_type_t type) {
    return clipboard_has_content(type) ? 1 : 0;
}

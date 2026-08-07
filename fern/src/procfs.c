/**
 * Forest-OS /proc Filesystem Implementation
 * Provides process and system information to userspace applications
 * 
 * This is required for Linux/Unix application compatibility as most
 * Linux programs check /proc for system information.
 */

#include "include/vfs.h"
#include "include/task.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/screen.h"
#include "include/smp.h"
#include "include/tty.h"
#include "include/graphics/graphics_manager.h"
#include <stdio.h>

// Forward declarations
static uint32 procfs_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static uint32 procfs_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer);
static bool procfs_readdir(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent);
static vfs_node_t* procfs_finddir(vfs_node_t* parent, const char* name);
static vfs_node_t* procfs_vfs_get_root(void* sb);

void* kmalloc(size_t size);

// Procfs node types
typedef enum {
    PROCFS_DIR,
    PROCFS_FILE,
    PROCFS_LINK
} procfs_type_t;

typedef struct {
    procfs_type_t type;
    char name[64];
    char* data;
    uint32 size;
    uint32 (*read_callback)(uint8* buffer, uint32 size, uint32 offset);
} procfs_entry_t;

#define PROCFS_RUNTIME_MAGIC 0x50524653u
#define PROCFS_RUNTIME_PID_STATUS 1u
#define PROCFS_RUNTIME_PID_CMDLINE 2u
#define PROCFS_RUNTIME_PID_MAPS 3u
#define PROCFS_RUNTIME_PID_ENVIRON 4u
#define PROCFS_RUNTIME_PID_FD_DIR 5u
#define PROCFS_RUNTIME_PID_FD_LINK 6u
#define PROCFS_RUNTIME_PID_FDINFO_DIR 7u
#define PROCFS_RUNTIME_PID_FDINFO_FILE 8u

typedef struct {
    uint32 magic;
    uint32 type;
    uint32 pid;
    uint32 aux;
} procfs_runtime_entry_t;

// Global procfs data
static bool g_procfs_initialized = false;
static vfs_node_t* g_procfs_root = NULL;

// Runtime TTY feature flags exposed through /proc/tty_options.
static bool g_tty_options_initialized = false;
static bool g_tty_opt_advanced = true;
static bool g_tty_opt_ansi = true;
static bool g_tty_opt_color = true;
static bool g_tty_opt_blink = false;
static bool g_tty_opt_status_bar = true;

static char procfs_ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static char* procfs_trim_spaces(char* s) {
    if (!s) {
        return s;
    }

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }

    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[len - 1] = '\0';
            len--;
        } else {
            break;
        }
    }

    return s;
}

static uint32 procfs_strnlen_local(const char* s, uint32 max_len) {
    if (!s) {
        return 0;
    }

    uint32 i = 0;
    while (i < max_len && s[i] != '\0') {
        i++;
    }
    return i;
}

static bool procfs_parse_pid_string(const char* s, uint32* out_pid) {
    if (!s || !out_pid || s[0] == '\0') {
        return false;
    }

    uint32 pid = 0;
    for (uint32 i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }

        pid = (pid * 10u) + (uint32)(c - '0');
    }

    *out_pid = pid;
    return true;
}

static task_t* procfs_find_task_by_pid(uint32 pid) {
    extern task_t* ready_queue_head;
    if (!ready_queue_head) {
        return NULL;
    }

    task_t* t = ready_queue_head;
    do {
        if (t->id == pid) {
            return t;
        }
        t = t->next;
    } while (t != ready_queue_head);

    return NULL;
}

static bool procfs_get_pid_from_dir_node(vfs_node_t* node, uint32* out_pid) {
    if (!node || !out_pid || !(node->flags & VFS_DIRECTORY) || !node->internal_data) {
        return false;
    }

    procfs_entry_t* entry = (procfs_entry_t*)node->internal_data;
    if (entry->type != PROCFS_DIR) {
        return false;
    }

    return procfs_parse_pid_string(entry->name, out_pid);
}

static vfs_node_t* procfs_create_pid_dir_node(uint32 pid, procfs_entry_t* backing_entry) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        return NULL;
    }

    memory_set((uint8*)node, 0, sizeof(vfs_node_t));
    node->inode = 100 + pid;
    node->flags = VFS_DIRECTORY;
    node->length = 0;
    node->readdir = procfs_readdir;
    node->finddir = procfs_finddir;

    if (backing_entry) {
        node->internal_data = backing_entry;
    } else {
        procfs_entry_t* synthetic_entry = (procfs_entry_t*)kmalloc(sizeof(procfs_entry_t));
        if (!synthetic_entry) {
            kfree(node);
            return NULL;
        }
        memory_set((uint8*)synthetic_entry, 0, sizeof(procfs_entry_t));
        synthetic_entry->type = PROCFS_DIR;
        snprintf(synthetic_entry->name, sizeof(synthetic_entry->name), "%u", pid);
        node->internal_data = synthetic_entry;
    }

    return node;
}

static bool procfs_get_pid_from_fd_dir_node(vfs_node_t* node, uint32* out_pid) {
    if (!node || !out_pid || !(node->flags & VFS_DIRECTORY) || !node->internal_data) {
        return false;
    }

    procfs_runtime_entry_t* runtime = (procfs_runtime_entry_t*)node->internal_data;
    if (runtime->magic != PROCFS_RUNTIME_MAGIC || runtime->type != PROCFS_RUNTIME_PID_FD_DIR) {
        return false;
    }

    *out_pid = runtime->pid;
    return true;
}

static bool procfs_get_pid_from_fdinfo_dir_node(vfs_node_t* node, uint32* out_pid) {
    if (!node || !out_pid || !(node->flags & VFS_DIRECTORY) || !node->internal_data) {
        return false;
    }

    procfs_runtime_entry_t* runtime = (procfs_runtime_entry_t*)node->internal_data;
    if (runtime->magic != PROCFS_RUNTIME_MAGIC || runtime->type != PROCFS_RUNTIME_PID_FDINFO_DIR) {
        return false;
    }

    *out_pid = runtime->pid;
    return true;
}

static uint32 procfs_copy_range(const char* data, uint32 data_len, uint8* buffer, uint32 size, uint32 offset) {
    if (!data || !buffer || size == 0 || offset >= data_len) {
        return 0;
    }
    uint32 remaining = data_len - offset;
    uint32 copy_len = (remaining < size) ? remaining : size;
    memory_copy(data + offset, buffer, copy_len);
    return copy_len;
}

static uint32 procfs_get_fd_count_for_pid(uint32 pid) {
    uint32 count = 3; // stdin/stdout/stderr
    task_t* target = procfs_find_task_by_pid(pid);
    if (target && target->tty_fd > 2) {
        count++;
    }
    return count;
}

static bool procfs_get_fd_at_index(uint32 pid, uint32 index, uint32* out_fd) {
    if (!out_fd) {
        return false;
    }

    if (index < 3) {
        *out_fd = index;
        return true;
    }

    task_t* target = procfs_find_task_by_pid(pid);
    if (target && target->tty_fd > 2 && index == 3) {
        *out_fd = (uint32)target->tty_fd;
        return true;
    }

    return false;
}

static bool procfs_is_valid_fd_for_pid(uint32 pid, uint32 fd) {
    if (fd < 3) {
        return true;
    }

    task_t* target = procfs_find_task_by_pid(pid);
    return (target && target->tty_fd >= 0 && (uint32)target->tty_fd == fd);
}

static const char* procfs_task_state_name(task_state_t state) {
    switch (state) {
        case TASK_STATE_RUNNING:
            return "running";
        case TASK_STATE_READY:
            return "ready";
        case TASK_STATE_WAITING:
            return "sleeping";
        case TASK_STATE_TERMINATED:
            return "zombie";
        default:
            return "unknown";
    }
}

static uint32 procfs_get_fd_target_for_pid(uint32 pid, uint32 fd, char* out, uint32 out_size) {
    if (!out || out_size == 0) {
        return 0;
    }

    if (fd == 0) {
        snprintf(out, out_size, "/dev/stdin");
    } else if (fd == 1) {
        snprintf(out, out_size, "/dev/stdout");
    } else if (fd == 2) {
        snprintf(out, out_size, "/dev/stderr");
    } else {
        task_t* target = procfs_find_task_by_pid(pid);
        if (target && target->tty_fd >= 0 && (uint32)target->tty_fd == fd) {
            snprintf(out, out_size, "/dev/tty");
        } else {
            snprintf(out, out_size, "anon_inode:[fd:%u]", fd);
        }
    }

    return (uint32)strlen(out);
}

static uint32 proc_read_process_maps(uint8* buffer, uint32 size, uint32 offset, uint32 pid) {
    char buf[768];
    task_t* target = procfs_find_task_by_pid(pid);
    const char* name = (target && target->name[0]) ? target->name : "unknown";

    uint32 text_start = 0x00400000u + ((pid & 0xFFu) * 0x10000u);
    uint32 text_end = text_start + 0x10000u;
    uint32 data_start = text_end;
    uint32 data_end = data_start + 0x10000u;
    uint32 heap_start = 0x08000000u + ((pid & 0x0Fu) * 0x20000u);
    uint32 heap_end = heap_start + 0x20000u;
    uint32 stack_start = 0xBFFDF000u;
    uint32 stack_end = 0xC0000000u;
    uint32 vdso_start = 0xB7FFF000u;
    uint32 vdso_end = vdso_start + 0x1000u;
    uint32 vvar_start = 0xB7FFE000u;
    uint32 vvar_end = vvar_start + 0x1000u;
    uint32 brk_end = heap_end;

    if (target && target->user_heap_base) {
        heap_start = (uint32)target->user_heap_base;
        brk_end = (uint32)(target->user_brk ? target->user_brk : target->user_heap_base);
        if (brk_end < heap_start) {
            brk_end = heap_start;
        }
        if (target->user_heap_limit > target->user_heap_base) {
            heap_end = (uint32)target->user_heap_limit;
        } else if (heap_end < brk_end) {
            heap_end = brk_end + 0x1000u;
        }
    }

    int len = snprintf(buf, sizeof(buf),
        "%08x-%08x r-xp 00000000 08:00 1 /bin/%s\n"
        "%08x-%08x rw-p 00010000 08:00 1 /bin/%s\n"
        "%08x-%08x rw-p 00000000 00:00 0 [heap]\n"
        "%08x-%08x rw-p 00000000 00:00 0 [brk]\n"
        "%08x-%08x rw-p 00000000 00:00 0 [stack]\n"
        "%08x-%08x r--p 00000000 00:00 0 [vvar]\n"
        "%08x-%08x r-xp 00000000 00:00 0 [vdso]\n",
        text_start, text_end, name,
        data_start, data_end, name,
        heap_start, heap_end,
        heap_start, brk_end,
        stack_start, stack_end,
        vvar_start, vvar_end,
        vdso_start, vdso_end);

    if (len < 0) {
        return 0;
    }
    return procfs_copy_range(buf, (uint32)len, buffer, size, offset);
}

static uint32 proc_read_process_environ(uint8* buffer, uint32 size, uint32 offset, uint32 pid) {
    char buf[512];
    task_t* target = procfs_find_task_by_pid(pid);
    const char* name = (target && target->name[0]) ? target->name : "unknown";
    const char* state = target ? procfs_task_state_name(target->state) : "unknown";
    uint32 uid = target ? target->uid : 0;
    uint32 gid = target ? target->gid : 0;
    uint32 session = target ? target->session : 0;
    uint32 pgrp = target ? target->pgrp : 0;

    int used = snprintf(buf, sizeof(buf), "USER=root");
    if (used < 0 || used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "HOME=/");
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "PATH=/bin:/usr/bin");
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "TERM=linux");
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "PWD=/");
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "TASK=%s", name);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "PID=%u", pid);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "PGRP=%u", pgrp);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "SESSION=%u", session);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "TASK_UID=%u", uid);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "TASK_GID=%u", gid);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';
    used += snprintf(buf + used, sizeof(buf) - (uint32)used, "TASK_STATE=%s", state);
    if (used >= (int)sizeof(buf)) {
        return 0;
    }
    buf[used++] = '\0';

    return procfs_copy_range(buf, (uint32)used, buffer, size, offset);
}

static uint32 proc_read_process_fd_link(uint8* buffer, uint32 size, uint32 offset, uint32 pid, uint32 fd) {
    char target[96];
    uint32 target_len = procfs_get_fd_target_for_pid(pid, fd, target, (uint32)sizeof(target));
    return procfs_copy_range(target, target_len, buffer, size, offset);
}

static uint32 proc_read_process_fdinfo(uint8* buffer, uint32 size, uint32 offset, uint32 pid, uint32 fd) {
    char target[96];
    char info[256];
    uint32 target_len = procfs_get_fd_target_for_pid(pid, fd, target, (uint32)sizeof(target));
    task_t* target_task = procfs_find_task_by_pid(pid);

    uint32 flags = 0x8000u; // O_LARGEFILE-like compatibility placeholder
    if (fd == 0) {
        flags |= 0x0u; // read-only
    } else {
        flags |= 0x1u; // write-capable placeholder for stdout/stderr/tty
    }

    int len = snprintf(info, sizeof(info),
        "pos:\t0\n"
        "flags:\t0%o\n"
        "mnt_id:\t0\n"
        "ino:\t%u\n"
        "peer:\t%s\n"
        "pid:\t%u\n"
        "tty:\t%d\n",
        flags,
        1000u + fd,
        target_len ? target : "unknown",
        pid,
        target_task ? (int)target_task->tty_fd : -1);
    if (len < 0) {
        return 0;
    }

    return procfs_copy_range(info, (uint32)len, buffer, size, offset);
}

static vfs_node_t* procfs_create_runtime_pid_node(uint32 pid, uint32 type, uint32 inode, uint32 flags, uint32 aux) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        return NULL;
    }

    procfs_runtime_entry_t* runtime = (procfs_runtime_entry_t*)kmalloc(sizeof(procfs_runtime_entry_t));
    if (!runtime) {
        kfree(node);
        return NULL;
    }

    runtime->magic = PROCFS_RUNTIME_MAGIC;
    runtime->type = type;
    runtime->pid = pid;
    runtime->aux = aux;

    memory_set((uint8*)node, 0, sizeof(vfs_node_t));
    node->inode = inode;
    node->flags = flags;
    node->length = 0;
    node->read = ((flags & VFS_DIRECTORY) != 0) ? NULL : procfs_read;
    node->write = NULL;
    node->readdir = ((flags & VFS_DIRECTORY) != 0) ? procfs_readdir : NULL;
    node->finddir = ((flags & VFS_DIRECTORY) != 0) ? procfs_finddir : NULL;
    node->internal_data = runtime;

    return node;
}

static bool procfs_parse_bool_value(const char* value, bool* out_value) {
    if (!value || !out_value) {
        return false;
    }

    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
        strcmp(value, "on") == 0 || strcmp(value, "yes") == 0) {
        *out_value = true;
        return true;
    }

    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
        strcmp(value, "off") == 0 || strcmp(value, "no") == 0) {
        *out_value = false;
        return true;
    }

    return false;
}

static void procfs_tty_options_sync_from_tty(void) {
    tty_runtime_options_t options;
    tty_get_runtime_options(&options);
    g_tty_opt_advanced = options.advanced_mode;
    g_tty_opt_ansi = options.ansi_processing_enabled;
    g_tty_opt_color = options.colors_enabled;
    g_tty_opt_blink = options.blink_enabled;
    g_tty_opt_status_bar = options.status_bar_enabled;
    g_tty_options_initialized = true;
}

static void procfs_apply_tty_toggle(const char* key, bool enabled) {
    procfs_tty_options_sync_from_tty();
    if (!key) {
        return;
    }

    if (strcmp(key, "status_bar") == 0 || strcmp(key, "statusbar") == 0) {
        g_tty_opt_status_bar = enabled;
        tty_set_status_bar_enabled(enabled);
        if (tty_is_ready()) {
            tty_force_redraw();
        }
        return;
    }

    if (strcmp(key, "blink") == 0) {
        g_tty_opt_blink = enabled;
        tty_set_blink_enabled(enabled);
        if (tty_is_ready()) {
            tty_force_redraw();
        }
        return;
    }

    if (strcmp(key, "ansi") == 0) {
        g_tty_opt_ansi = enabled;
        tty_set_ansi_processing_enabled(enabled);
        return;
    }

    if (strcmp(key, "color") == 0) {
        g_tty_opt_color = enabled;
        tty_set_colors_enabled(enabled);
        return;
    }

    if (strcmp(key, "advanced") == 0) {
        g_tty_opt_advanced = enabled;
        tty_set_advanced_mode(enabled);
        procfs_tty_options_sync_from_tty();
        return;
    }
}

static uint32 proc_read_tty_options(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0 || !buffer || size == 0) {
        return 0;
    }

    procfs_tty_options_sync_from_tty();

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
        "advanced=%u\n"
        "ansi=%u\n"
        "color=%u\n"
        "blink=%u\n"
        "status_bar=%u\n",
        g_tty_opt_advanced ? 1u : 0u,
        g_tty_opt_ansi ? 1u : 0u,
        g_tty_opt_color ? 1u : 0u,
        g_tty_opt_blink ? 1u : 0u,
        g_tty_opt_status_bar ? 1u : 0u);

    uint32 copy_len = (uint32)((len > (int)size) ? size : (uint32)len);
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// CPU info callback
static uint32 proc_read_cpuinfo(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[2048];
    int len = 0;
    
    // CPU processor info
    len += snprintf(buf + len, sizeof(buf) - len, "processor\t: 0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "vendor_id\t: ForestOS\n");
    len += snprintf(buf + len, sizeof(buf) - len, "model name\t: Forest-OS Virtual CPU\n");
    len += snprintf(buf + len, sizeof(buf) - len, "cpu family\t: 6\n");
    len += snprintf(buf + len, sizeof(buf) - len, "model\t\t: 0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "stepping\t: 1\n");
    len += snprintf(buf + len, sizeof(buf) - len, "microcode\t: 0x1\n");
    len += snprintf(buf + len, sizeof(buf) - len, "cpu MHz\t\t: 100.000\n");
    len += snprintf(buf + len, sizeof(buf) - len, "cache size\t: 256 KB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "physical id\t: 0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "siblings\t: 1\n");
    len += snprintf(buf + len, sizeof(buf) - len, "core id\t\t: 0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "cpu cores\t: 1\n");
    len += snprintf(buf + len, sizeof(buf) - len, "apicid\t\t: 0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "fpu\t\t: yes\n");
    len += snprintf(buf + len, sizeof(buf) - len, "fpu_exception\t: yes\n");
    len += snprintf(buf + len, sizeof(buf) - len, "cpuid level\t: 0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 mmx fxsr sse sse2 sse3\n");
    len += snprintf(buf + len, sizeof(buf) - len, "bugs\t\t:\n");
    len += snprintf(buf + len, sizeof(buf) - len, "bogomips\t: 200.00\n");
    len += snprintf(buf + len, sizeof(buf) - len, "TLB size\t: 255 4K pages\n");
    len += snprintf(buf + len, sizeof(buf) - len, "clflush size\t: 64\n");
    len += snprintf(buf + len, sizeof(buf) - len, "cache_alignment\t: 64\n");
    len += snprintf(buf + len, sizeof(buf) - len, "address sizes\t: 36 bits physical, 32 bits virtual\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Meminfo callback
static uint32 proc_read_meminfo(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[1024];
    int len = 0;
    
    // Get memory info - using placeholder values
    uint32 total_mem = 128 * 1024; // 128 MB placeholder
    uint32 free_mem = 64 * 1024;   // 64 MB free
    
    len += snprintf(buf + len, sizeof(buf) - len, "MemTotal:\t%u kB\n", total_mem);
    len += snprintf(buf + len, sizeof(buf) - len, "MemFree:\t%u kB\n", free_mem);
    len += snprintf(buf + len, sizeof(buf) - len, "MemAvailable:\t%u kB\n", free_mem);
    len += snprintf(buf + len, sizeof(buf) - len, "Buffers:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Cached:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "SwapCached:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Active:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Inactive:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Active(anon):\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Inactive(anon):\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Active(file):\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Inactive(file):\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Unevictable:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Mlocked:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "SwapTotal:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "SwapFree:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Dirty:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Writeback:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "AnonPages:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Mapped:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Shmem:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Slab:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "SReclaimable:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "SUnreclaim:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "KernelStack:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "PageTables:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "NFS_Unstable:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Bounce:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "WritebackTmp:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "CommitLimit:\t%u kB\n", total_mem);
    len += snprintf(buf + len, sizeof(buf) - len, "Committed_AS:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "VmallocTotal:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "VmallocUsed:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "VmallocChunk:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Percpu:\t\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "AnonHugePages:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "ShmemHugePages:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "ShmemPmdMapped:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "HugePages_Total:\t0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "HugePages_Free:\t0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "HugePages_Rsvd:\t0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "HugePages_Surp:\t0\n");
    len += snprintf(buf + len, sizeof(buf) - len, "Hugepagesize:\t2048 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "DirectMap4k:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "DirectMap2M:\t0 kB\n");
    len += snprintf(buf + len, sizeof(buf) - len, "DirectMap1G:\t0 kB\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Uptime callback
static uint32 proc_read_uptime(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[128];
    // timer_get_ticks() is milliseconds in current kernel builds.
    extern uint32 timer_get_ticks(void);
    uint32 ticks = timer_get_ticks();
    uint32 seconds = ticks / 1000;
    uint32 hundredths = (ticks % 1000) / 10;
    
    int len = snprintf(buf, sizeof(buf), "%u.%02u %u.%02u\n",
        seconds, hundredths, seconds, hundredths);
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Load average callback
static uint32 proc_read_loadavg(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[128];
    
    // Get process count
    extern task_t* ready_queue_head;
    uint32 proc_count = 0;
    if (ready_queue_head) {
        task_t* t = ready_queue_head;
        do {
            proc_count++;
            t = t->next;
        } while (t != ready_queue_head);
    }
    
    // Load average (simplified - just current process count / 65536)
    uint32 load1 = proc_count * 65536 / 100;
    uint32 load5 = load1;
    uint32 load15 = load1;
    
    uint32 last_pid = current_task ? current_task->id : proc_count;

    int len = snprintf(buf, sizeof(buf), "%u.%02u %u.%02u %u.%02u %u/%u %u\n",
        load1 / 65536, (load1 % 65536) * 100 / 65536,
        load5 / 65536, (load5 % 65536) * 100 / 65536,
        load15 / 65536, (load15 % 65536) * 100 / 65536,
        proc_count, proc_count, last_pid);
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Stat callback
static uint32 proc_read_stat(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[512];
    int len = 0;
    
    // CPU accounting globals are optional in current kernel builds.
    uint32 g_cpu_ticks_idle = 0;
    uint32 g_cpu_ticks_user = 0;
    uint32 g_cpu_ticks_kernel = 0;
    
    len += snprintf(buf + len, sizeof(buf) - len, "cpu  %u %u %u %u 0 0 0 0 0 0\n",
        g_cpu_ticks_user, 0, g_cpu_ticks_kernel, g_cpu_ticks_idle);
    len += snprintf(buf + len, sizeof(buf) - len, "cpu0 %u %u %u %u 0 0 0 0 0 0\n",
        g_cpu_ticks_user, 0, g_cpu_ticks_kernel, g_cpu_ticks_idle);
    
    // Get process count
    extern task_t* ready_queue_head;
    uint32 proc_count = 0;
    if (ready_queue_head) {
        task_t* t = ready_queue_head;
        do {
            proc_count++;
            t = t->next;
        } while (t != ready_queue_head);
    }
    
    len += snprintf(buf + len, sizeof(buf) - len, "intr %u 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", 0);
    len += snprintf(buf + len, sizeof(buf) - len, "ctxt %u\n", 0);
    len += snprintf(buf + len, sizeof(buf) - len, "btime %u\n", 0);
    len += snprintf(buf + len, sizeof(buf) - len, "processes %u\n", proc_count);
    len += snprintf(buf + len, sizeof(buf) - len, "procs_running %u\n", proc_count);
    len += snprintf(buf + len, sizeof(buf) - len, "procs_blocked %u\n", 0);
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Version callback
static uint32 proc_read_version(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    char buf[256];
    int len = snprintf(buf, sizeof(buf), 
        "Linux version 3.0.0-forestos (root@forestos) "
        "(gcc version 12.2.0) #1 SMP " __DATE__ " " __TIME__ "\n");
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Self symlink - points to current process
static uint32 proc_read_self_link(uint8* buffer, uint32 size, uint32 offset) {
    if (offset > 0) return 0;
    
    task_t* task = current_task;
    if (!task) {
        return 0;
    }
    
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%u", task->id);
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

static uint32 proc_read_process_cmdline(uint8* buffer, uint32 size, uint32 offset, uint32 pid) {
    if (!buffer || size == 0) {
        return 0;
    }

    char buf[64];
    uint32 len = 0;
    task_t* target = procfs_find_task_by_pid(pid);

    if (target) {
        uint32 name_len = procfs_strnlen_local(target->name, (uint32)(sizeof(target->name)));
        if (name_len > (uint32)(sizeof(buf) - 1)) {
            name_len = (uint32)(sizeof(buf) - 1);
        }
        if (name_len > 0) {
            memory_copy(target->name, buf, name_len);
        }
        // Linux-style cmdline is NUL-separated and usually NUL-terminated.
        buf[name_len] = '\0';
        len = name_len + 1;
    } else {
        buf[0] = '\0';
        len = 1;
    }

    if (offset >= len) {
        return 0;
    }

    uint32 remaining = len - offset;
    uint32 copy_len = (remaining < size) ? remaining : size;
    memory_copy(buf + offset, buffer, copy_len);
    return copy_len;
}

// Static procfs entries
static procfs_entry_t procfs_entries[] = {
    {PROCFS_FILE, "cpuinfo", NULL, 0, proc_read_cpuinfo},
    {PROCFS_FILE, "meminfo", NULL, 0, proc_read_meminfo},
    {PROCFS_FILE, "uptime", NULL, 0, proc_read_uptime},
    {PROCFS_FILE, "loadavg", NULL, 0, proc_read_loadavg},
    {PROCFS_FILE, "stat", NULL, 0, proc_read_stat},
    {PROCFS_FILE, "version", NULL, 0, proc_read_version},
    {PROCFS_FILE, "tty_options", NULL, 0, proc_read_tty_options},
    {PROCFS_LINK, "self", NULL, 0, proc_read_self_link},
};

#define MAX_PROC_ENTRIES 256

typedef struct {
    procfs_entry_t entry;
    char pid_str[16];
    bool is_process_dir;
} procfs_dynamic_entry_t;

static procfs_dynamic_entry_t g_procfs_dynamic[MAX_PROC_ENTRIES];
static uint32 g_procfs_dynamic_count = 0;

// Add a process to procfs
void procfs_add_process(uint32 pid) {
    if (g_procfs_dynamic_count >= MAX_PROC_ENTRIES) {
        return;
    }
    
    procfs_dynamic_entry_t* entry = &g_procfs_dynamic[g_procfs_dynamic_count++];
    entry->is_process_dir = true;
    snprintf(entry->pid_str, sizeof(entry->pid_str), "%u", pid);
    strncpy(entry->entry.name, entry->pid_str, sizeof(entry->entry.name) - 1);
    entry->entry.type = PROCFS_DIR;
    entry->entry.data = NULL;
    entry->entry.size = 0;
    entry->entry.read_callback = NULL;
}

// Remove a process from procfs
void procfs_remove_process(uint32 pid) {
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%u", pid);
    
    for (uint32 i = 0; i < g_procfs_dynamic_count; i++) {
        if (g_procfs_dynamic[i].is_process_dir && 
            strcmp(g_procfs_dynamic[i].pid_str, pid_str) == 0) {
            // Remove by shifting
            for (uint32 j = i; j < g_procfs_dynamic_count - 1; j++) {
                g_procfs_dynamic[j] = g_procfs_dynamic[j + 1];
            }
            g_procfs_dynamic_count--;
            return;
        }
    }
}

// Process status callback
static uint32 proc_read_process_status(uint8* buffer, uint32 size, uint32 offset, uint32 pid) {
    if (offset > 0) return 0;
    
    char buf[1024];
    int len = 0;
    
    // Find the task
    extern task_t* ready_queue_head;
    task_t* target = NULL;
    
    if (ready_queue_head) {
        task_t* t = ready_queue_head;
        do {
            if (t->id == pid) {
                target = t;
                break;
            }
            t = t->next;
        } while (t != ready_queue_head);
    }
    
    if (!target) {
        len = snprintf(buf, sizeof(buf), "Name:\tunknown\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Pid:\t%u\n", pid);
        len += snprintf(buf + len, sizeof(buf) - len, "State:\tZ\n");
    } else {
        const char* state_str = "R";
        switch (target->state) {
            case TASK_STATE_RUNNING: state_str = "R"; break;
            case TASK_STATE_READY: state_str = "R"; break;
            case TASK_STATE_WAITING: state_str = "D"; break;
            case TASK_STATE_TERMINATED: state_str = "Z"; break;
            case TASK_STATE_ZOMBIE: state_str = "Z"; break;
            case TASK_STATE_SUSPENDED: state_str = "T"; break;
            default: state_str = "U"; break;
        }
        
        len = snprintf(buf, sizeof(buf), "Name:\t%s\n", target->name);
        len += snprintf(buf + len, sizeof(buf) - len, "Pid:\t%u\n", target->id);
        len += snprintf(buf + len, sizeof(buf) - len, "PPid:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Uid:\t%u\t0\t0\t0\n", target->uid);
        len += snprintf(buf + len, sizeof(buf) - len, "Gid:\t%u\t0\t0\t0\n", target->gid);
        len += snprintf(buf + len, sizeof(buf) - len, "State:\t%s\n", state_str);
        len += snprintf(buf + len, sizeof(buf) - len, "Tgid:\t%u\n", target->id);
        len += snprintf(buf + len, sizeof(buf) - len, "Ngid:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Pid:\t%u\n", target->id);
        len += snprintf(buf + len, sizeof(buf) - len, "TracerPid:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Uid:\t%u\t%u\t%u\t%u\n", 
            target->uid, target->uid, target->uid, target->uid);
        len += snprintf(buf + len, sizeof(buf) - len, "Gid:\t%u\t%u\t%u\t%u\n",
            target->gid, target->gid, target->gid, target->gid);
        len += snprintf(buf + len, sizeof(buf) - len, "FDSize:\t32\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Groups:\t0 \n");
        len += snprintf(buf + len, sizeof(buf) - len, "NStgid:\t%u\n", target->id);
        len += snprintf(buf + len, sizeof(buf) - len, "NSpid:\t%u\n", target->id);
        len += snprintf(buf + len, sizeof(buf) - len, "NSpgid:\t%u\n", target->pgrp);
        len += snprintf(buf + len, sizeof(buf) - len, "NSsid:\t%u\n", target->session);
        len += snprintf(buf + len, sizeof(buf) - len, "VmPeak:\t%u kB\n", 0);
        len += snprintf(buf + len, sizeof(buf) - len, "VmSize:\t%u kB\n", 0);
        len += snprintf(buf + len, sizeof(buf) - len, "VmLck:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmPin:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmHWM:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmRSS:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "RssAnon:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "RssFile:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "RssShmem:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmData:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmStk:\t%u kB\n", 4096);
        len += snprintf(buf + len, sizeof(buf) - len, "VmExe:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmLib:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmPTE:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "VmSwap:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "HugetlbPages:\t0 kB\n");
        len += snprintf(buf + len, sizeof(buf) - len, "CoreDumping:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Threads:\t1\n");
        len += snprintf(buf + len, sizeof(buf) - len, "SigQ:\t0/1024\n");
        len += snprintf(buf + len, sizeof(buf) - len, "SigPnd:\t0000000000000000\n");
        len += snprintf(buf + len, sizeof(buf) - len, "ShdPnd:\t0000000000000000\n");
        len += snprintf(buf + len, sizeof(buf) - len, "SigBlk:\t0000000000000000\n");
        len += snprintf(buf + len, sizeof(buf) - len, "SigIgn:\t0000000000000000\n");
        len += snprintf(buf + len, sizeof(buf) - len, "SigCgt:\t0000000000000000\n");
        len += snprintf(buf + len, sizeof(buf) - len, "CapInh:\t0000000000000000\n");
        len += snprintf(buf + len, sizeof(buf) - len, "CapPrm:\t00000000fffffeff\n");
        len += snprintf(buf + len, sizeof(buf) - len, "CapEff:\t00000000fffffeff\n");
        len += snprintf(buf + len, sizeof(buf) - len, "CapBnd:\t00000000fffffeff\n");
        len += snprintf(buf + len, sizeof(buf) - len, "NoNewPrivs:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Seccomp:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Speculation_Store_Bypass:\tvulnerable\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Cpus_allowed:\t1\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Cpus_allowed_list:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Mems_allowed:\t1\n");
        len += snprintf(buf + len, sizeof(buf) - len, "Mems_allowed_list:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "voluntary_ctxt_switches:\t0\n");
        len += snprintf(buf + len, sizeof(buf) - len, "nonvoluntary_ctxt_switches:\t0\n");
    }
    
    uint32 copy_len = len > size ? size : len;
    memory_copy(buf, buffer, copy_len);
    return copy_len;
}

// Procfs operations
static uint32 procfs_read(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!node || !buffer) {
        return 0;
    }
    
    procfs_runtime_entry_t* runtime = (procfs_runtime_entry_t*)node->internal_data;
    if (runtime && runtime->magic == PROCFS_RUNTIME_MAGIC) {
        switch (runtime->type) {
            case PROCFS_RUNTIME_PID_STATUS:
                return proc_read_process_status(buffer, size, offset, runtime->pid);
            case PROCFS_RUNTIME_PID_CMDLINE:
                return proc_read_process_cmdline(buffer, size, offset, runtime->pid);
            case PROCFS_RUNTIME_PID_MAPS:
                return proc_read_process_maps(buffer, size, offset, runtime->pid);
            case PROCFS_RUNTIME_PID_ENVIRON:
                return proc_read_process_environ(buffer, size, offset, runtime->pid);
            case PROCFS_RUNTIME_PID_FD_LINK:
                return proc_read_process_fd_link(buffer, size, offset, runtime->pid, runtime->aux);
            case PROCFS_RUNTIME_PID_FDINFO_FILE:
                return proc_read_process_fdinfo(buffer, size, offset, runtime->pid, runtime->aux);
            default:
                return 0;
        }
    }

    procfs_entry_t* entry = (procfs_entry_t*)node->internal_data;
    if (!entry) {
        return 0;
    }
    
    // Handle special cases
    if (entry->read_callback) {
        return (uint32)entry->read_callback(buffer, size, offset);
    }
    
    // Static data
    if (entry->data && entry->size > 0) {
        if (offset >= entry->size) {
            return 0;
        }
        uint32 remaining = entry->size - offset;
        uint32 copy = remaining < size ? remaining : size;
        memory_copy(entry->data + offset, buffer, copy);
        return copy;
    }
    
    return 0;
}

static uint32 procfs_write(vfs_node_t* node, uint32 offset, uint32 size, uint8* buffer) {
    if (!node || !buffer || size == 0) {
        return 0;
    }

    // Keep existing /proc files effectively read-only except tty_options.
    procfs_entry_t* entry = (procfs_entry_t*)node->internal_data;
    if (!entry || strcmp(entry->name, "tty_options") != 0) {
        (void)offset;
        return 0;
    }

    // Parse simple key=value updates (comma/newline/semicolon separated).
    char input[256];
    uint32 copy_size = (size < (uint32)(sizeof(input) - 1)) ? size : (uint32)(sizeof(input) - 1);
    memory_copy(buffer, input, copy_size);
    input[copy_size] = '\0';

    char* cursor = input;
    while (*cursor) {
        while (*cursor == ',' || *cursor == ';' || *cursor == '\n' || *cursor == '\r' ||
               *cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        char* token_start = cursor;
        while (*cursor && *cursor != ',' && *cursor != ';' && *cursor != '\n' && *cursor != '\r') {
            cursor++;
        }
        if (*cursor) {
            *cursor = '\0';
            cursor++;
        }

        char* token = procfs_trim_spaces(token_start);
        char* eq = strchr(token, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char* key = procfs_trim_spaces(token);
        char* value = procfs_trim_spaces(eq + 1);
        if (*key == '\0' || *value == '\0') {
            continue;
        }

        for (char* p = key; *p; p++) {
            *p = procfs_ascii_tolower(*p);
        }
        for (char* p = value; *p; p++) {
            *p = procfs_ascii_tolower(*p);
        }

        bool enabled = false;
        if (!procfs_parse_bool_value(value, &enabled)) {
            continue;
        }

        // Unknown keys are intentionally ignored for robustness.
        procfs_apply_tty_toggle(key, enabled);
    }

    (void)offset;
    return size;
}

static bool procfs_readdir(vfs_node_t* node, uint32 index, vfs_dirent_t* dirent) {
    if (!dirent) {
        return false;
    }
    memory_set((uint8*)dirent, 0, sizeof(*dirent));

    uint32 parent_pid = 0;
    bool is_pid_dir = procfs_get_pid_from_dir_node(node, &parent_pid);
    if (is_pid_dir) {
        if (index == 0) {
            dirent->inode = 100 + parent_pid;
            strncpy(dirent->name, ".", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 1) {
            dirent->inode = 1;
            strncpy(dirent->name, "..", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 2) {
            dirent->inode = node->inode + 1;
            strncpy(dirent->name, "status", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 3) {
            dirent->inode = node->inode + 2;
            strncpy(dirent->name, "cmdline", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 4) {
            dirent->inode = node->inode + 3;
            strncpy(dirent->name, "maps", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 5) {
            dirent->inode = node->inode + 4;
            strncpy(dirent->name, "environ", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 6) {
            dirent->inode = node->inode + 5;
            strncpy(dirent->name, "fd", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 7) {
            dirent->inode = node->inode + 6;
            strncpy(dirent->name, "fdinfo", sizeof(dirent->name) - 1);
            return true;
        }
        return false;
    }

    uint32 fd_dir_pid = 0;
    if (procfs_get_pid_from_fd_dir_node(node, &fd_dir_pid)) {
        if (index == 0) {
            dirent->inode = node->inode;
            strncpy(dirent->name, ".", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 1) {
            dirent->inode = 100 + fd_dir_pid;
            strncpy(dirent->name, "..", sizeof(dirent->name) - 1);
            return true;
        }

        uint32 fd_index = index - 2;
        uint32 fd = 0;
        if (procfs_get_fd_at_index(fd_dir_pid, fd_index, &fd)) {
            dirent->inode = node->inode + 100 + fd;
            snprintf(dirent->name, sizeof(dirent->name), "%u", fd);
            return true;
        }
        return false;
    }

    uint32 fdinfo_dir_pid = 0;
    if (procfs_get_pid_from_fdinfo_dir_node(node, &fdinfo_dir_pid)) {
        if (index == 0) {
            dirent->inode = node->inode;
            strncpy(dirent->name, ".", sizeof(dirent->name) - 1);
            return true;
        }
        if (index == 1) {
            dirent->inode = 100 + fdinfo_dir_pid;
            strncpy(dirent->name, "..", sizeof(dirent->name) - 1);
            return true;
        }

        uint32 fd_index = index - 2;
        uint32 fd = 0;
        if (procfs_get_fd_at_index(fdinfo_dir_pid, fd_index, &fd)) {
            dirent->inode = node->inode + 200 + fd;
            snprintf(dirent->name, sizeof(dirent->name), "%u", fd);
            return true;
        }
        return false;
    }
    
    // First, handle static entries
    uint32 static_count = sizeof(procfs_entries) / sizeof(procfs_entries[0]);
    
    if (index == 0) {
        dirent->inode = 1;
        strncpy(dirent->name, ".", sizeof(dirent->name) - 1);
        return true;
    }
    
    if (index == 1) {
        dirent->inode = 2;
        strncpy(dirent->name, "..", sizeof(dirent->name) - 1);
        return true;
    }
    
    uint32 file_index = index - 2;

    if (file_index < static_count) {
        procfs_entry_t* entry = &procfs_entries[file_index];
        dirent->inode = 3 + file_index;
        strncpy(dirent->name, entry->name, sizeof(dirent->name) - 1);
        return true;
    }
    
    file_index -= static_count;
    
    // Then handle dynamic process entries
    if (file_index < g_procfs_dynamic_count) {
        procfs_dynamic_entry_t* entry = &g_procfs_dynamic[file_index];
        dirent->inode = 100 + file_index;
        strncpy(dirent->name, entry->pid_str, sizeof(dirent->name) - 1);
        return true;
    }

    return false;
}

static vfs_node_t* procfs_finddir(vfs_node_t* parent, const char* name) {
    if (!name) {
        return NULL;
    }

    // /proc/<pid>/status and /proc/<pid>/cmdline
    uint32 parent_pid = 0;
    if (procfs_get_pid_from_dir_node(parent, &parent_pid)) {
        if (strcmp(name, "status") == 0) {
            return procfs_create_runtime_pid_node(parent_pid, PROCFS_RUNTIME_PID_STATUS,
                (parent ? parent->inode + 1 : 0), VFS_FILE, 0);
        }
        if (strcmp(name, "cmdline") == 0) {
            return procfs_create_runtime_pid_node(parent_pid, PROCFS_RUNTIME_PID_CMDLINE,
                (parent ? parent->inode + 2 : 0), VFS_FILE, 0);
        }
        if (strcmp(name, "maps") == 0) {
            return procfs_create_runtime_pid_node(parent_pid, PROCFS_RUNTIME_PID_MAPS,
                (parent ? parent->inode + 3 : 0), VFS_FILE, 0);
        }
        if (strcmp(name, "environ") == 0) {
            return procfs_create_runtime_pid_node(parent_pid, PROCFS_RUNTIME_PID_ENVIRON,
                (parent ? parent->inode + 4 : 0), VFS_FILE, 0);
        }
        if (strcmp(name, "fd") == 0) {
            return procfs_create_runtime_pid_node(parent_pid, PROCFS_RUNTIME_PID_FD_DIR,
                (parent ? parent->inode + 5 : 0), VFS_DIRECTORY, 0);
        }
        if (strcmp(name, "fdinfo") == 0) {
            return procfs_create_runtime_pid_node(parent_pid, PROCFS_RUNTIME_PID_FDINFO_DIR,
                (parent ? parent->inode + 6 : 0), VFS_DIRECTORY, 0);
        }
        return NULL;
    }

    uint32 fd_dir_pid = 0;
    if (procfs_get_pid_from_fd_dir_node(parent, &fd_dir_pid)) {
        uint32 fd = 0;
        if (!procfs_parse_pid_string(name, &fd)) {
            return NULL;
        }
        if (!procfs_is_valid_fd_for_pid(fd_dir_pid, fd)) {
            return NULL;
        }
        return procfs_create_runtime_pid_node(fd_dir_pid, PROCFS_RUNTIME_PID_FD_LINK,
            (parent ? parent->inode + 100 + fd : 0), VFS_SYMLINK, fd);
    }

    uint32 fdinfo_dir_pid = 0;
    if (procfs_get_pid_from_fdinfo_dir_node(parent, &fdinfo_dir_pid)) {
        uint32 fd = 0;
        if (!procfs_parse_pid_string(name, &fd)) {
            return NULL;
        }
        if (!procfs_is_valid_fd_for_pid(fdinfo_dir_pid, fd)) {
            return NULL;
        }
        return procfs_create_runtime_pid_node(fdinfo_dir_pid, PROCFS_RUNTIME_PID_FDINFO_FILE,
            (parent ? parent->inode + 200 + fd : 0), VFS_FILE, fd);
    }

    // /proc/self path redirection to /proc/<current-pid>
    if (parent == g_procfs_root && strcmp(name, "self") == 0) {
        task_t* task = current_task;
        if (!task) {
            return NULL;
        }

        for (uint32 i = 0; i < g_procfs_dynamic_count; i++) {
            procfs_dynamic_entry_t* dyn = &g_procfs_dynamic[i];
            if (dyn->is_process_dir && procfs_parse_pid_string(dyn->pid_str, &parent_pid) &&
                parent_pid == task->id) {
                return procfs_create_pid_dir_node(task->id, &dyn->entry);
            }
        }
        return procfs_create_pid_dir_node(task->id, NULL);
    }
    
    // Check static entries
    uint32 static_count = sizeof(procfs_entries) / sizeof(procfs_entries[0]);
    for (uint32 i = 0; i < static_count; i++) {
        procfs_entry_t* entry = &procfs_entries[i];
        if (strcmp(entry->name, name) == 0) {
            vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
            if (!node) return NULL;
            
            memory_set((uint8*)node, 0, sizeof(vfs_node_t));
            node->inode = 3 + i;
            node->flags = (entry->type == PROCFS_DIR) ? VFS_DIRECTORY : VFS_FILE;
            node->length = entry->size;
            node->read = procfs_read;
            node->write = procfs_write;
            node->readdir = (entry->type == PROCFS_DIR) ? procfs_readdir : NULL;
            node->finddir = (entry->type == PROCFS_DIR) ? procfs_finddir : NULL;
            node->internal_data = entry;
            
            return node;
        }
    }
    
    // Check dynamic process entries
    for (uint32 i = 0; i < g_procfs_dynamic_count; i++) {
        procfs_dynamic_entry_t* entry = &g_procfs_dynamic[i];
        if (strcmp(entry->pid_str, name) == 0) {
            uint32 pid = 0;
            if (!procfs_parse_pid_string(entry->pid_str, &pid)) {
                return NULL;
            }
            return procfs_create_pid_dir_node(pid, &entry->entry);
        }
    }

    // Allow direct /proc/<pid> lookup if a task exists but dynamic list is stale.
    uint32 lookup_pid = 0;
    if (parent == g_procfs_root &&
        procfs_parse_pid_string(name, &lookup_pid) &&
        procfs_find_task_by_pid(lookup_pid)) {
        return procfs_create_pid_dir_node(lookup_pid, NULL);
    }
    
    return NULL;
}

static vfs_node_t* procfs_vfs_get_root(void* sb) {
    (void)sb;
    return g_procfs_root;
}

// Initialize procfs
bool procfs_init(void) {
    debuglog(DEBUG_INFO, "[PROCFS] Initializing /proc filesystem...\n");
    
    // Create root procfs node
    g_procfs_root = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!g_procfs_root) {
        debuglog(DEBUG_ERROR, "[PROCFS] Failed to allocate root node\n");
        return false;
    }
    
    memory_set((uint8*)g_procfs_root, 0, sizeof(vfs_node_t));
    g_procfs_root->inode = 1;
    g_procfs_root->flags = VFS_DIRECTORY;
    g_procfs_root->readdir = procfs_readdir;
    g_procfs_root->finddir = procfs_finddir;
    g_procfs_root->name[0] = '\0'; // Root has no name
    
    // Register procfs as a filesystem
    vfs_filesystem_t* procfs_fs = (vfs_filesystem_t*)kmalloc(sizeof(vfs_filesystem_t));
    if (!procfs_fs) {
        debuglog(DEBUG_ERROR, "[PROCFS] Failed to allocate filesystem struct\n");
        return false;
    }
    
    memory_set((uint8*)procfs_fs, 0, sizeof(vfs_filesystem_t));
    procfs_fs->name = "proc";
    procfs_fs->mount = NULL;
    procfs_fs->get_root = procfs_vfs_get_root;
    
    // Register the filesystem
    if (vfs_register_filesystem(procfs_fs) != 0) {
        debuglog(DEBUG_WARN, "[PROCFS] Failed to register procfs (may already exist)\n");
    }
    
    // Mount procfs at /proc
    if (vfs_mount("proc", "/proc", "proc", NULL, NULL, NULL, 0) != 0) {
        debuglog(DEBUG_WARN, "[PROCFS] Failed to mount at /proc, trying fallback...\n");
        // Try to create the mount point manually
    }
    
    g_procfs_initialized = true;
    debuglog(DEBUG_INFO, "[PROCFS] /proc filesystem initialized\n");
    
    return true;
}

// Get procfs root node
vfs_node_t* procfs_get_root(void) {
    return g_procfs_root;
}

/**
 * Forest-OS Linux Binary Compatibility Layer
 * Provides ability to run Linux ELF binaries directly
 * 
 * This implements the Linux syscall interface for running native Linux executables
 */

#include "include/task.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/debuglog.h"
#include "include/spinlock.h"
#include "include/elf.h"

// ============================================
// Linux-specific syscall numbers
// ============================================

// These differ from the Linux x86_64 numbers - we use i386 for 32-bit compat
#define LINUX_SYS_read              3
#define LINUX_SYS_write            4
#define LINUX_SYS_open             5
#define LINUX_SYS_close            6
#define LINUX_SYS_brk             45
#define LINUX_SYS_mmap             90
#define LINUX_SYS_mprotect         125
#define LINUX_SYS_munmap          91
#define LINUX_SYS_clone           120
#define LINUX_SYS_execve           59
#define LINUX_SYS_exit            231
#define LINUX_SYS_wait4           114
#define LINUX_SYS_kill            129
#define LINUX_SYS_uname           160
#define LINUX_SYS_getpid          172
#define LINUX_SYS_getuid          199
#define LINUX_SYS_getgid          200
#define LINUX_SYS_setuid          213
#define LINUX_SYS_setgid          214
#define LINUX_SYS_geteuid          201
#define LINUX_SYS_getegid          202
#define LINUX_SYS_getppid          110
#define LINUX_SYS_getpgrp          132
#define LINUX_SYS_setsid          147
#define LINUX_SYS_setpgid         123
#define LINUX_SYS_getpgid         132
#define LINUX_SYS_gettid          186
#define LINUX_SYS_prctl           157
#define LINUX_SYS_personality      135

// ============================================
// Linux personality flags
// ============================================

#define PER_LINUX32               0x0008
#define PER_LINUX32_SHLIB        0x0010

// Current personality
static uint32_t g_linux_personality = 0;

// ============================================
// /proc/sys/kernel values
// ============================================

static char g_kernel_osrelease[64] = "3.0.0-forest";
static char g_kernel_version[256] = "#1 SMP " __DATE__ " " __TIME__;
static char g_kernel_domainname[64] = "(none)";

// ============================================
// Linux syscall handler
// ============================================

// Forward declarations for Linux-specific syscalls
extern int32_t linux_sys_prctl(int32_t option, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5);
extern int32_t linux_sys_personality(uint32_t persona);

/**
 * Handle a Linux-specific syscall
 * This is called when a Linux binary makes a syscall that differs from Forest-OS
 */
int32_t linux_syscall_handle(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, uint32_t arg3, 
                             uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    int32_t result = 0;
    
    switch (syscall_num) {
        case LINUX_SYS_prctl:
            result = linux_sys_prctl((int32_t)arg1, arg2, arg3, arg4, arg5);
            break;
            
        case LINUX_SYS_personality:
            result = linux_sys_personality(arg1);
            break;
            
        // Linux-specific syscalls not in Forest-OS
        case LINUX_SYS_uname:
            // Already implemented in syscall.c but use Linux struct
            // Fall through to default
            break;
            
        default:
            debuglog(DEBUG_INFO, "[LINUX] Unknown Linux syscall: %u\n", syscall_num);
            result = -38; // ENOSYS
    }
    
    return result;
}

// ============================================
// prctl - Process control
// ============================================

#define PR_GET_PDEATHSIG  1
#define PR_SET_PDEATHSIG  2
#define PR_GET_DUMPABLE   3
#define PR_SET_DUMPABLE   4
#define PR_GET_UNALIGN   5
#define PR_SET_UNALIGN   6
#define PR_GET_KEEPCAPS  7
#define PR_SET_KEEPCAPS  8
#define PR_GET_FPEMU  9
#define PR_SET_FPEMU  10
#define PR_GET_FPEXC  11
#define PR_SET_FPEXC  12
#define PR_GET_TIMING   13
#define PR_SET_TIMING   14
#define PR_SET_NAME     15
#define PR_GET_NAME     16
#define PR_GET_ENDIAN   19
#define PR_SET_ENDIAN   20
#define PR_GET_SECCOMP  21
#define PR_SET_SECCOMP  22

int32_t linux_sys_prctl(int32_t option, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    task_t* task = current_task;
    
    switch (option) {
        case PR_GET_PDEATHSIG:
            // Get parent death signal
            if (arg2) {
                // Would return current pdeath signal
            }
            return 0;
            
        case PR_SET_PDEATHSIG:
            // Set parent death signal
            return 0;
            
        case PR_GET_DUMPABLE:
            // Get dumpable flag
            return task ? 1 : 0;
            
        case PR_SET_DUMPABLE:
            // Set dumpable flag
            if (task) {
                // Would set dumpable flag
            }
            return 0;
            
        case PR_SET_NAME:
            // Set process name
            if (task && arg2) {
                // Would set task name
            }
            return 0;
            
        case PR_GET_NAME:
            // Get process name
            if (task && arg2) {
                // Would get task name
            }
            return 0;
            
        case PR_GET_SECCOMP:
            // Get seccomp mode
            return 0;
            
        case PR_SET_SECCOMP:
            // Set seccomp mode
            return 0;
            
        default:
            debuglog(DEBUG_INFO, "[LINUX] Unknown prctl option: %d\n", option);
            return -22; // EINVAL
    }
}

// ============================================
// personality - Set execution domain
// ============================================

int32_t linux_sys_personality(uint32_t persona) {
    uint32_t old_personality = g_linux_personality;
    
    if (persona != 0xFFFFFFFF) {
        g_linux_personality = persona;
    }
    
    return old_personality;
}

// ============================================
// Linux ELF interpreter support
// ============================================

// Path to Linux ELF interpreter (simulated)
static char g_linux_interpreter_path[256] = "/lib/ld-linux.so.2";

// Check if we're running a Linux binary
bool linux_is_linux_binary(const uint8_t* elf_data) {
    if (!elf_data) return false;
    
    // Check ELF magic
    if (elf_data[0] != 0x7F || 
        elf_data[1] != 'E' ||
        elf_data[2] != 'L' ||
        elf_data[3] != 'F') {
        return false;
    }
    
    // For now, assume all ELF binaries are compatible
    // In reality, we'd check for Linux-specific features
    return true;
}

// Set Linux interpreter path
void linux_set_interpreter(const char* path) {
    if (path) {
        strncpy(g_linux_interpreter_path, path, sizeof(g_linux_interpreter_path) - 1);
    }
}

// Get Linux interpreter path
const char* linux_get_interpreter(void) {
    return g_linux_interpreter_path;
}

// ============================================
// Linux /proc/sys handling
// ============================================

// Get kernel.osrelease
const char* linux_get_osrelease(void) {
    return g_kernel_osrelease;
}

// Get kernel.version  
const char* linux_get_version(void) {
    return g_kernel_version;
}

// Get kernel.domainname
const char* linux_get_domainname(void) {
    return g_kernel_domainname;
}

// Set kernel.osrelease
void linux_set_osrelease(const char* str) {
    if (str) {
        strncpy(g_kernel_osrelease, str, sizeof(g_kernel_osrelease) - 1);
    }
}

// ============================================
// Linux sysctl emulation
// ============================================

// Common sysctl entries
typedef struct {
    char name[64];
    char value[128];
} linux_sysctl_entry_t;

static linux_sysctl_entry_t g_sysctl_entries[] = {
    { "kernel.osrelease", "3.0.0-forest" },
    { "kernel.version", "#1 SMP " __DATE__ " " __TIME__ },
    { "kernel.domainname", "(none)" },
    { "kernel.hostname", "forestos" },
    { "fs.file-max", "65536" },
    { "fs.nr_open", "1048576" },
    { "vm.overcommit_memory", "0" },
    { "vm.swappiness", "60" },
    { "net.ipv4.tcp_syncookies", "1" },
    { "net.ipv4.ip_local_port_range", "32768 60999" },
};

// Get sysctl value
const char* linux_sysctl_get(const char* name) {
    for (size_t i = 0; i < sizeof(g_sysctl_entries) / sizeof(g_sysctl_entries[0]); i++) {
        if (strcmp(g_sysctl_entries[i].name, name) == 0) {
            return g_sysctl_entries[i].value;
        }
    }
    return NULL;
}

// Set sysctl value
int linux_sysctl_set(const char* name, const char* value) {
    for (size_t i = 0; i < sizeof(g_sysctl_entries) / sizeof(g_sysctl_entries[0]); i++) {
        if (strcmp(g_sysctl_entries[i].name, name) == 0) {
            if (value) {
                strncpy(g_sysctl_entries[i].value, value, sizeof(g_sysctl_entries[i].value) - 1);
            }
            return 0;
        }
    }
    return -2; //ENOENT
}

// ============================================
// Linux kernel module support (stub)
// ============================================

int linux_init_module(void* module_image, uint32_t len, const char* params) {
    (void)module_image;
    (void)len;
    (void)params;
    // Would load kernel module
    debuglog(DEBUG_INFO, "[LINUX] init_module not implemented\n");
    return -38; // ENOSYS
}

int linux_delete_module(const char* name, uint32_t flags) {
    (void)name;
    (void)flags;
    debuglog(DEBUG_INFO, "[LINUX] delete_module not implemented\n");
    return -38; // ENOSYS
}

// ============================================
// Linux binary compatibility init
// ============================================

void linux_compat_init(void) {
    debuglog(DEBUG_INFO, "[LINUX] Linux binary compatibility layer initialized\n");
    debuglog(DEBUG_INFO, "[LINUX] Default interpreter: %s\n", g_linux_interpreter_path);
    debuglog(DEBUG_INFO, "[LINUX] Personality: 0x%x\n", g_linux_personality);
}

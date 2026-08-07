#include "include/session.h"
#include "include/auth.h"
#include "include/kb.h"
#include "include/tty.h"
#include "include/shell_loader.h"
#include "include/task.h"
#include "include/util.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/timer.h"
#include "include/vfs.h"
#include "include/libc/stdio.h"
#include "include/hotkey.h"
#include "include/graphics/graphics_manager.h"
#include "include/interrupt.h"
#include "include/mm.h"
#include "include/memory.h"
#include "include/elf.h"
#include "include/framebuffer.h"
#include "include/graphics_init.h"

/*
 * vga_write_error - write a short message directly to the VGA text buffer
 * at physical address 0xB8000 (white on black, attribute 0x0F).
 * This works even when the framebuffer / graphics stack is completely dead,
 * because the VGA text buffer is always identity-mapped on x86.
 *
 * At most 80 characters are written (one VGA text row).
 */
static void vga_write_error(const char* msg)
{
    if (!msg) return;
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    const uint16_t ATTR = 0x0F00; /* white on black */
    for (int i = 0; i < 80 && msg[i]; i++) {
        vga[i] = ATTR | (uint8_t)msg[i];
    }
}

#define SESSION_INPUT_MAX 64
#define SESSION_DE_PATH_MAX 256
#define DE_STARTUP_TIMEOUT_TICKS 6000
#define DE_FRAMEBUFFER_MMAP_TIMEOUT_TICKS 1000
#define GRAPHICS_TASK_ASSUME_READY_TICKS 20
#define SESSION_TRANSITION_FADE_TICKS 8
#define SESSION_TRANSITION_HALF_TICKS 4

typedef enum {
    GRAPHICS_TASK_STARTUP_OK = 0,
    GRAPHICS_TASK_STARTUP_TIMEOUT,
    GRAPHICS_TASK_STARTUP_EXITED_EARLY
} graphics_task_startup_result_t;

static tty_session_t g_tty_sessions[MAX_TTY_SESSIONS];
static bool g_sessions_initialized = false;

static bool g_fb_owner_session_active = false;
static uint32_t g_fb_owner_session = 0;

extern uint32_t g_current_tty_session;

static volatile bool g_session_switch_pending = false;
static volatile uint32_t g_session_switch_target = 0;

static const uint8* g_preloaded_de_elf = NULL;
static uint32 g_preloaded_de_elf_size = 0;

// Optional TTY status APIs (may not be present in older builds).
extern void tty_status_set_text(const char* text) __attribute__((weak));
extern void tty_set_status_text(const char* text) __attribute__((weak));
extern void tty_set_login_status(const char* text) __attribute__((weak));
extern void tty_status_set_current_user(const char* username) __attribute__((weak));
extern void tty_set_status_user(const char* username) __attribute__((weak));
extern void tty_status_clear_current_user(void) __attribute__((weak));
extern void tty_clear_status_user(void) __attribute__((weak));
extern void tty_status_set_logged_in(bool logged_in) __attribute__((weak));
extern void tty_set_status_logged_in(bool logged_in) __attribute__((weak));

static void session_update_tty_status_text(const char* text) {
    if (tty_status_set_text) {
        tty_status_set_text(text);
    } else if (tty_set_status_text) {
        tty_set_status_text(text);
    } else if (tty_set_login_status) {
        tty_set_login_status(text);
    }
}

static void session_update_tty_status_user(const char* username) {
    if (tty_status_set_current_user) {
        tty_status_set_current_user(username);
    } else if (tty_set_status_user) {
        tty_set_status_user(username);
    }

    if (tty_status_set_logged_in) {
        tty_status_set_logged_in(username && username[0] != '\0');
    } else if (tty_set_status_logged_in) {
        tty_set_status_logged_in(username && username[0] != '\0');
    }
}

static void session_clear_tty_status_user(void) {
    if (tty_status_clear_current_user) {
        tty_status_clear_current_user();
    } else if (tty_clear_status_user) {
        tty_clear_status_user();
    } else {
        session_update_tty_status_user("");
    }

    if (tty_status_set_logged_in) {
        tty_status_set_logged_in(false);
    } else if (tty_set_status_logged_in) {
        tty_set_status_logged_in(false);
    }
}

static inline void session_idle_wait(void) {
    task_schedule();
    if (irq_are_enabled()) {
        __asm__ __volatile__("hlt");
    } else {
        __asm__ __volatile__("pause");
    }
}

static void session_panic_display(const char* msg) {
    if (msg) {
        debuglog(DEBUG_ERROR, "[SESSION] PANIC: %s\n", msg);
        tty_write("FATAL: ");
        tty_write(msg);
        tty_write("\n");
    }
}

static bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool has_elf_suffix(const char* s) {
    size_t len = strlen(s);
    return (len >= 4) && strcmp(s + len - 4, ".elf") == 0;
}

static bool resolve_desktop_path(const char* raw_value, char* out_path, size_t out_size) {
    if (!raw_value || !out_path || out_size == 0) {
        return false;
    }

    while (*raw_value && is_space_char(*raw_value)) {
        raw_value++;
    }
    if (*raw_value == '\0') {
        return false;
    }

    size_t raw_len = strlen(raw_value);
    while (raw_len > 0 && is_space_char(raw_value[raw_len - 1])) {
        raw_len--;
    }
    if (raw_len == 0) {
        return false;
    }

    char value[SESSION_DE_PATH_MAX];
    if (raw_len >= sizeof(value)) {
        raw_len = sizeof(value) - 1;
    }
    memcpy(value, raw_value, raw_len);
    value[raw_len] = '\0';

    if (value[0] == '"') {
        size_t len = strlen(value);
        if (len >= 2 && value[len - 1] == '"') {
            memmove(value, value + 1, len - 2);
            value[len - 2] = '\0';
        } else {
            memmove(value, value + 1, len - 1);
            value[len - 1] = '\0';
        }
    }

    if (value[0] == '\0') {
        return false;
    }

    if (value[0] == '/') {
        strncpy(out_path, value, out_size - 1);
        out_path[out_size - 1] = '\0';
        return true;
    }

    if (strchr(value, '/')) {
        snprintf(out_path, out_size, "/%s", value);
        return true;
    }

    // Installed binaries carry no extension; strip a legacy ".elf" suffix
    // from older config values instead of appending one.
    if (has_elf_suffix(value)) {
        value[strlen(value) - 4] = '\0';
    }
    snprintf(out_path, out_size, "/usr/bin/%s", value);

    return true;
}

static bool parse_desktop_path_from_config(const char* config_data, uint32 config_size,
                                           char* out_path, size_t out_size) {
    if (!config_data || config_size == 0 || !out_path || out_size == 0) {
        return false;
    }

    const char* p = config_data;
    const char* end = config_data + config_size;

    while (p < end) {
        const char* line_start = p;
        while (p < end && *p != '\n' && *p != '\r') {
            p++;
        }
        const char* line_end = p;

        while (line_start < line_end && is_space_char(*line_start)) {
            line_start++;
        }
        while (line_end > line_start && is_space_char(line_end[-1])) {
            line_end--;
        }

        const char* value = NULL;
        if ((size_t)(line_end - line_start) > 8 && strncmp(line_start, "desktop=", 8) == 0) {
            value = line_start + 8;
        } else if ((size_t)(line_end - line_start) > 3 && strncmp(line_start, "DE=", 3) == 0) {
            value = line_start + 3;
        } else if ((size_t)(line_end - line_start) > 3 && strncmp(line_start, "de=", 3) == 0) {
            value = line_start + 3;
        }

        if (value && value < line_end) {
            char value_buf[SESSION_DE_PATH_MAX];
            size_t value_len = (size_t)(line_end - value);
            if (value_len >= sizeof(value_buf)) {
                value_len = sizeof(value_buf) - 1;
            }

            memcpy(value_buf, value, value_len);
            value_buf[value_len] = '\0';

            if (resolve_desktop_path(value_buf, out_path, out_size)) {
                return true;
            }
        }

        while (p < end && (*p == '\n' || *p == '\r')) {
            p++;
        }
    }

    return false;
}

static bool load_user_desktop_path(const auth_user_info_t* user_info,
                                   char* out_path, size_t out_size) {
    if (!user_info || !out_path || out_size == 0 || !user_info->name[0]) {
        return false;
    }

    char user_config[256];
    snprintf(user_config, sizeof(user_config), "/home/%s/.session/.conf", user_info->name);

    const uint8* config_data = NULL;
    uint32 config_size = 0;
    if (!vfs_read_file(user_config, &config_data, &config_size) || !config_data || config_size == 0) {
        return false;
    }

    return parse_desktop_path_from_config((const char*)config_data, config_size, out_path, out_size);
}

static bool load_system_desktop_path(char* out_path, size_t out_size) {
    if (!out_path || out_size == 0) {
        return false;
    }

    const uint8* sys_config_data = NULL;
    uint32 sys_config_size = 0;
    if (!vfs_read_file("/usr/share/sysconf/sys.conf", &sys_config_data, &sys_config_size) ||
        !sys_config_data || sys_config_size == 0) {
        return false;
    }

    return parse_desktop_path_from_config((const char*)sys_config_data, sys_config_size,
                                          out_path, out_size);
}

static bool load_first_elf(const char* const* paths, char* out_path, size_t out_path_size,
                           const uint8** out_data, uint32* out_size) {
    if (!paths || !out_path || out_path_size == 0 || !out_data || !out_size) {
        return false;
    }

    for (int i = 0; paths[i] != NULL; i++) {
        const uint8* elf_data = NULL;
        uint32 elf_size = 0;
        if (vfs_read_file(paths[i], &elf_data, &elf_size) && elf_data && elf_size > 0) {
            strncpy(out_path, paths[i], out_path_size - 1);
            out_path[out_path_size - 1] = '\0';
            *out_data = elf_data;
            *out_size = elf_size;
            return true;
        }
    }

    return false;
}

// Wait for a graphics task startup signal.
// Some apps may not update last_active_tick immediately, so we accept either:
//   1) observed activity tick,
//   2) process remains alive for a short stability window, or
//   3) SIGUSR1 signal received (DM_READY).
static graphics_task_startup_result_t wait_for_graphics_task_startup(uint32 pid, const char* task_name, uint32 timeout_ticks) {
    uint32 launch_tick = timer_get_ticks();

    debuglog(DEBUG_INFO, "[SESSION] Waiting for %s (PID %u) startup (timeout=%u ticks)...\n",
             task_name ? task_name : "Graphics task", pid, timeout_ticks);

    while (task_exists(pid)) {
        uint32 now = timer_get_ticks();
        uint32 elapsed = now - launch_tick;

        if (task_get_last_active_tick(pid) != 0) {
            debuglog(DEBUG_INFO, "[SESSION] %s (PID %u) active after %u ticks\n",
                     task_name ? task_name : "Graphics task", pid, elapsed);
            return GRAPHICS_TASK_STARTUP_OK;
        }

        task_t* t = task_find_by_pid(pid);
        if (t && task_has_framebuffer_mapping(t)) {
            debuglog(DEBUG_INFO, "[SESSION] %s (PID %u) mapped FB after %u ticks\n",
                     task_name ? task_name : "Graphics task", pid, elapsed);
            return GRAPHICS_TASK_STARTUP_OK;
        }

        if (t && (t->pending_signals & TASK_SIGNAL_BIT(SIGUSR1))) {
            t->pending_signals &= ~TASK_SIGNAL_BIT(SIGUSR1);
            debuglog(DEBUG_INFO, "[SESSION] %s (PID %u) sent DM_READY (SIGUSR1) after %u ticks\n",
                     task_name ? task_name : "Graphics task", pid, elapsed);
            return GRAPHICS_TASK_STARTUP_OK;
        }

        if (elapsed >= GRAPHICS_TASK_ASSUME_READY_TICKS) {
            debuglog(DEBUG_INFO, "[SESSION] %s (PID %u) assumed ready after %u ticks\n",
                     task_name ? task_name : "Graphics task", pid, elapsed);
            return GRAPHICS_TASK_STARTUP_OK;
        }

        if (elapsed > timeout_ticks) {
            debuglog(DEBUG_ERROR,
                     "[SESSION] %s (PID %u) timeout after %u ticks, terminating\n",
                     task_name ? task_name : "Graphics task", pid, elapsed);
            task_send_signal(pid, SIGTERM);
            uint32_t kill_wait = 0;
            while (task_exists(pid) && kill_wait < 50) {
                session_idle_wait();
                kill_wait++;
            }
            if (task_exists(pid)) {
                task_kill(pid);
            }
            session_panic_display("Task startup timeout!");
            return GRAPHICS_TASK_STARTUP_TIMEOUT;
        }

        session_idle_wait();
    }

    debuglog(DEBUG_INFO,
             "[SESSION] %s exited before startup activity observed\n",
              task_name ? task_name : "Graphics task");
    return GRAPHICS_TASK_STARTUP_EXITED_EARLY;
}

void session_init_all(void) {
    if (g_sessions_initialized) {
        return;
    }

    for (int i = 0; i < MAX_TTY_SESSIONS; i++) {
        g_tty_sessions[i].session_id = i + 1;
        // TTY 1 is documented (see the "TTY 1: GUI login" banner in
        // session_run()) and configured (sys.conf DE=) as the graphical
        // session; every other TTY stays a text login. Nothing previously
        // set this, so launch_user_session()'s entire GUI branch was dead
        // code and every login silently fell back to a plain shell.
        g_tty_sessions[i].type = (i == 0) ? SESSION_TYPE_GUI : SESSION_TYPE_TEXT;
        g_tty_sessions[i].state = SESSION_STATE_LOGIN;
        g_tty_sessions[i].logged_in = false;
        memset(&g_tty_sessions[i].user_info, 0, sizeof(auth_user_info_t));
        g_tty_sessions[i].shell_pid = 0;
        g_tty_sessions[i].initialized = true;
        g_tty_sessions[i].gui_suspended = false;
        g_tty_sessions[i].de_crash_count = 0;
        g_tty_sessions[i].fb_snapshot = NULL;
        g_tty_sessions[i].fb_snapshot_size = 0;
        g_tty_sessions[i].fb_snapshot_valid = false;
    }

    g_sessions_initialized = true;
    debuglog(DEBUG_INFO, "[SESSION] Initialized %d TTY sessions\n", MAX_TTY_SESSIONS);
}

tty_session_t* session_get_current(void) {
    if (!g_sessions_initialized || g_current_tty_session < 1 || g_current_tty_session > MAX_TTY_SESSIONS) {
        return NULL;
    }
    return &g_tty_sessions[g_current_tty_session - 1];
}

tty_session_t* session_get(uint32_t session_num) {
    if (!g_sessions_initialized || session_num < 1 || session_num > MAX_TTY_SESSIONS) {
        return NULL;
    }
    return &g_tty_sessions[session_num - 1];
}

/* KNOWN GAP (not fixed here, documented per review): session_switch_to(),
 * session_check_hotkey(), session_perform_transition(),
 * session_save_framebuffer(), session_restore_framebuffer(),
 * session_suspend_gui() and session_resume_gui() below form a self-consistent
 * "pending switch" pipeline that was apparently the intended way for input
 * (hotkeys/chvt) to request a TTY session switch: set a pending flag/target
 * here, have the main loop notice it via session_check_hotkey(), then run
 * session_perform_transition() to cross-fade framebuffers using the
 * save/restore snapshot helpers, suspending/resuming the outgoing/incoming
 * GUI session's process group along the way.
 *
 * In practice this pipeline is dead: session_check_hotkey() has no callers
 * anywhere in the tree, so g_session_switch_pending/g_session_switch_target
 * are never consumed. The real switch path is src/hotkey.c's
 * hotkey_handler_switch_to_tty/_vt1/_vt2/_tty_vt/_sysreq_emergency_tty,
 * which write g_current_tty_session directly and call
 * display_manager_switch_mode()/tty_force_redraw()/task_resume() themselves.
 * session_run()'s main loop (see below) only notices the switch after the
 * fact by comparing g_current_tty_session to its previous value once
 * run_session_login() returns - it does not call session_check_hotkey() or
 * session_perform_transition() either. So framebuffer snapshotting
 * (session_save_framebuffer/session_restore_framebuffer) and GUI-process
 * SIGSTOP/SIGCONT (session_suspend_gui/session_resume_gui) never run around
 * a real VT switch today.
 *
 * This was left undone rather than wired up because the two switch models
 * differ in ways that are not safe to reconcile blindly: hotkey.c suspends
 * only the WM render task (kernel-side) and switches display mode
 * synchronously and immediately, while this pipeline's session_suspend_gui()/
 * session_resume_gui() SIGSTOP/SIGCONT the *shell's whole process group* and
 * session_perform_transition() blocks the caller for
 * SESSION_TRANSITION_HALF_TICKS*2 task_yield() iterations while
 * cross-fading. Splicing that in without fully tracing every caller (chvt,
 * SysRq, F-key hotkeys, possible IRQ/interrupt-context callers of the
 * hotkey_handler_* functions) risks stalling or corrupting a VT switch that
 * works correctly today. See matching comments at each hotkey.c call site.
 */
void session_switch_to(uint32_t session_num) {
    if (session_num < 1 || session_num > MAX_TTY_SESSIONS) {
        return;
    }

    g_session_switch_pending = true;
    g_session_switch_target = session_num;
}

bool session_check_hotkey(void) {
    if (g_session_switch_pending) {
        g_session_switch_pending = false;
        return true;
    }
    return false;
}

// See the "KNOWN GAP" comment above session_switch_to(): this is currently
// never called from a real VT switch (src/hotkey.c bypasses it).
bool session_save_framebuffer(uint32_t session_num) {
    if (session_num < 1 || session_num > MAX_TTY_SESSIONS) return false;
    tty_session_t* session = &g_tty_sessions[session_num - 1];
    if (session->type != SESSION_TYPE_GUI) return false;

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || fb->size == 0) return false;

    if (!session->fb_snapshot || session->fb_snapshot_size != fb->size) {
        if (session->fb_snapshot) kfree(session->fb_snapshot);
        session->fb_snapshot = kmalloc(fb->size);
        if (!session->fb_snapshot) {
            session->fb_snapshot_size = 0;
            session->fb_snapshot_valid = false;
            return false;
        }
        session->fb_snapshot_size = fb->size;
    }

    memcpy(session->fb_snapshot, (void*)fb->virtual_addr, fb->size);
    session->fb_snapshot_valid = true;
    debuglog(DEBUG_INFO, "[SESSION] Saved framebuffer for session %u (%u bytes)\n",
             session_num, (unsigned)fb->size);
    return true;
}

// See the "KNOWN GAP" comment above session_switch_to(): only reachable
// today via the also-dead session_perform_transition(), never from a real
// VT switch (src/hotkey.c bypasses this whole pipeline).
bool session_restore_framebuffer(uint32_t session_num) {
    if (session_num < 1 || session_num > MAX_TTY_SESSIONS) return false;
    tty_session_t* session = &g_tty_sessions[session_num - 1];
    if (!session->fb_snapshot_valid || !session->fb_snapshot) return false;

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || fb->size == 0) return false;

    uint32_t copy_size = (session->fb_snapshot_size < fb->size) ?
                          session->fb_snapshot_size : fb->size;
    memcpy((void*)fb->virtual_addr, session->fb_snapshot, copy_size);
    __asm__ volatile("mfence" ::: "memory");
    debuglog(DEBUG_INFO, "[SESSION] Restored framebuffer for session %u\n", session_num);
    return true;
}

void session_free_framebuffer_snapshot(uint32_t session_num) {
    if (session_num < 1 || session_num > MAX_TTY_SESSIONS) return;
    tty_session_t* session = &g_tty_sessions[session_num - 1];
    if (session->fb_snapshot) {
        kfree(session->fb_snapshot);
        session->fb_snapshot = NULL;
        session->fb_snapshot_size = 0;
        session->fb_snapshot_valid = false;
    }
}

// See the "KNOWN GAP" comment above session_switch_to(): this is currently
// never called from a real VT switch (src/hotkey.c bypasses it and only
// suspends/resumes the WM render task, not the GUI session's process group).
void session_suspend_gui(uint32_t session_num) {
    if (session_num < 1 || session_num > MAX_TTY_SESSIONS) return;
    tty_session_t* session = &g_tty_sessions[session_num - 1];
    if (session->type != SESSION_TYPE_GUI) return;
    if (session->gui_suspended) return;
    if (session->shell_pid == 0) return;

    task_send_signal_to_pgrp(session->shell_pid, SIGSTOP);
    session->gui_suspended = true;
    debuglog(DEBUG_INFO, "[SESSION] Suspended GUI session %u (PID %u)\n",
             session_num, session->shell_pid);
}

// See the "KNOWN GAP" comment above session_switch_to(): this is currently
// never called from a real VT switch (src/hotkey.c bypasses it and only
// suspends/resumes the WM render task, not the GUI session's process group).
void session_resume_gui(uint32_t session_num) {
    if (session_num < 1 || session_num > MAX_TTY_SESSIONS) return;
    tty_session_t* session = &g_tty_sessions[session_num - 1];
    if (session->type != SESSION_TYPE_GUI) return;
    if (!session->gui_suspended) return;
    if (session->shell_pid == 0) return;

    task_send_signal_to_pgrp(session->shell_pid, SIGCONT);
    session->gui_suspended = false;
    debuglog(DEBUG_INFO, "[SESSION] Resumed GUI session %u (PID %u)\n",
             session_num, session->shell_pid);
}

bool session_check_recovery(void) {
    if (!g_sessions_initialized) return false;
    bool recovered = false;

    for (int i = 0; i < MAX_TTY_SESSIONS; i++) {
        tty_session_t* s = &g_tty_sessions[i];
        if (s->type != SESSION_TYPE_GUI) continue;
        if (s->state != SESSION_STATE_ACTIVE) continue;
        if (s->shell_pid == 0) continue;
        if (task_exists(s->shell_pid)) continue;

        debuglog(DEBUG_WARN, "[SESSION] DE crashed on session %u (was PID %u)\n",
                 s->session_id, s->shell_pid);
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "[SESSION] DE crashed (PID %u).\n", s->shell_pid);
            tty_write(msg);
        }
        s->shell_pid = 0;
        s->gui_suspended = false;

        task_clear_foreground();
        tty_set_graphics_app_active(false);
        tty_set_status_bar_visible(true);

        uint32 wm_pid = task_get_id_by_name_prefix("wm-");
        if (wm_pid != 0) {
            task_suspend(wm_pid);
        }

        if (s->de_crash_count < SESSION_RECOVERY_LIMIT) {
            s->de_crash_count++;
            s->state = SESSION_STATE_RECOVERY;
            debuglog(DEBUG_WARN, "[SESSION] DE crash %u/%u on session %u, attempting recovery\n",
                     s->de_crash_count, SESSION_RECOVERY_LIMIT, s->session_id);
            tty_write("[SESSION] Desktop environment crashed. Attempting recovery...\n");
            recovered = true;
        } else {
            s->state = SESSION_STATE_LOGIN;
            s->logged_in = false;
            memset(&s->user_info, 0, sizeof(auth_user_info_t));
            s->de_crash_count = 0;
            s->type = SESSION_TYPE_TEXT;
            session_free_framebuffer_snapshot(s->session_id);
            tty_clear();
            tty_force_redraw();
            debuglog(DEBUG_ERROR, "[SESSION] DE crash limit reached on session %u, returning to login\n",
                     s->session_id);
            tty_write("[SESSION] Desktop environment failed repeatedly. Falling back to text console.\n");
            recovered = true;
        }
    }
    return recovered;
}

void session_cleanup_session(tty_session_t* session) {
    if (!session) return;

    debuglog(DEBUG_INFO, "[SESSION] Cleaning up session %u\n", session->session_id);
    tty_write("[SESSION] Cleaning up session...\n");

    if (session->shell_pid != 0) {
        if (task_exists(session->shell_pid)) {
            task_send_signal(session->shell_pid, SIGTERM);
            uint32_t kill_wait = 0;
            while (task_exists(session->shell_pid) && kill_wait < 50) {
                session_idle_wait();
                kill_wait++;
            }
            if (task_exists(session->shell_pid)) {
                debuglog(DEBUG_WARN, "[SESSION] Force killing PID %u on session %u\n",
                         session->shell_pid, session->session_id);
                task_send_signal(session->shell_pid, SIGKILL);
                kill_wait = 0;
                while (task_exists(session->shell_pid) && kill_wait < 20) {
                    session_idle_wait();
                    kill_wait++;
                }
            }
        }
        session->shell_pid = 0;
    }

    session->gui_suspended = false;
    if (session->type == SESSION_TYPE_GUI) {
        tty_set_graphics_app_active(false);
        tty_set_status_bar_visible(true);
    }

    session->state = SESSION_STATE_LOGIN;
    session->logged_in = false;
    memset(&session->user_info, 0, sizeof(auth_user_info_t));
    session_clear_tty_status_user();
    session_update_tty_status_text("Session ended");

    tty_clear();
    tty_force_redraw();
}

void session_get_status_string(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    char tmp[32];
    for (int i = 0; i < MAX_TTY_SESSIONS; i++) {
        tty_session_t* s = &g_tty_sessions[i];
        const char* state_str;
        switch (s->state) {
            case SESSION_STATE_LOGIN:    state_str = "login"; break;
            case SESSION_STATE_ACTIVE:   state_str = "active"; break;
            case SESSION_STATE_LOGOUT:   state_str = "logout"; break;
            case SESSION_STATE_SUSPENDED:state_str = "suspended"; break;
            case SESSION_STATE_RECOVERY: state_str = "recovery"; break;
            default: state_str = "?"; break;
        }
        snprintf(tmp, sizeof(tmp), "T%u:%s%s ",
                 s->session_id, state_str,
                 s->type == SESSION_TYPE_GUI ? "(gui)" : "");
        strncat(buf, tmp, buf_size - strlen(buf) - 1);
    }
}

// Draw a brief status message on the framebuffer during transition
static void session_draw_transition_status(const char* message) {
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || fb->size == 0) return;

    uint32_t pitch = fb->pitch;
    uint32_t width = fb->width;
    uint32_t height = fb->height;

    /*
     * Derive bytes-per-pixel from pitch/width when the stride is an even
     * multiple, because VESA/QEMU sometimes reports bpp=24 but pads each
     * pixel to 4 bytes (pitch = width * 4, not width * 3).  Using bpp/8
     * directly would undercount the stride and corrupt adjacent rows.
     * Fall back to bpp/8 when the pitch is not an even multiple of width.
     */
    uint32_t bytes_pp = (fb->bpp + 7) / 8;
    if (width > 0 && pitch >= width && (pitch % width) == 0) {
        uint32_t stride = pitch / width;
        if (stride >= 1 && stride <= 4) {
            bytes_pp = stride;
        }
    }
    if (bytes_pp == 0) bytes_pp = 4;

    /*
     * Dark background: fill each row using the full pitch so every byte
     * (including any per-pixel padding) is written.  This avoids leftover
     * pixels from whatever was on screen before (e.g. the splash screen).
     */
    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row = (uint8_t*)fb->virtual_addr + y * pitch;
        for (uint32_t b = 0; b < pitch; b++) {
            row[b] = 0x0a;
        }
    }

    // Green accent bar at top
    uint32_t bar_h = height / 48;
    if (bar_h < 2) bar_h = 2;
    for (uint32_t y = 0; y < bar_h; y++) {
        uint8_t* row = (uint8_t*)fb->virtual_addr + y * pitch;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t off = x * bytes_pp;
            if (bytes_pp == 4) {
                row[off + 0] = 0x30; row[off + 1] = 0xb0;
                row[off + 2] = 0x20; row[off + 3] = 0xff;
            } else if (bytes_pp == 3) {
                row[off + 0] = 0x30; row[off + 1] = 0xb0; row[off + 2] = 0x20;
            } else if (bytes_pp == 2) {
                /* RGB565: green = 0x07E0 */
                row[off + 0] = 0xE0; row[off + 1] = 0x07;
            }
        }
    }

    (void)message;
    __asm__ volatile("mfence" ::: "memory");
}

// See the "KNOWN GAP" comment above session_switch_to(): this function has
// no callers anywhere (session_check_hotkey(), the only thing that could
// gate a call to it, is itself uncalled). The framebuffer cross-fade and
// snapshot restore it performs never run on a real VT switch today -
// src/hotkey.c switches VTs directly and immediately instead.
__attribute__((unused)) static void session_perform_transition(uint32_t from_session, uint32_t to_session) {
    tty_session_t* from = session_get(from_session);
    tty_session_t* to = session_get(to_session);

    // If either session is not a GUI session, use a simple status screen instead of
    // trying to cross-fade framebuffers (which would show garbage for text sessions)
    if (!from || !to ||
        from->type != SESSION_TYPE_GUI || to->type != SESSION_TYPE_GUI) {
        const char* msg = "Switching...";
        session_draw_transition_status(msg);
        task_yield();
        task_yield();
        return;
    }

    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || fb->size == 0) return;

    // Both are GUI sessions - check that both have valid snapshots
    if (!from->fb_snapshot_valid || !to->fb_snapshot_valid) {
        session_draw_transition_status("Switching...");
        task_yield();
        return;
    }

    uint32_t half = SESSION_TRANSITION_HALF_TICKS;
    uint32_t pitch = fb->pitch;
    uint32_t height = fb->height;

    for (uint32_t step = 0; step < half; step++) {
        uint32_t progress = (step + 1) * 256 / half;
        if (progress > 256) progress = 256;

        uint8_t* pixels = (uint8_t*)fb->virtual_addr;
        uint32_t rows_this_step = height / half;
        if (rows_this_step == 0) rows_this_step = 1;
        uint32_t row_start = step * rows_this_step;
        uint32_t row_end = row_start + rows_this_step;
        if (row_end > height) row_end = height;

        for (uint32_t row = row_start; row < row_end; row++) {
            uint8_t* row_ptr = pixels + row * pitch;
            for (uint32_t col = 0; col < fb->width; col++) {
                for (uint32_t c = 0; c < (fb->bpp / 8); c++) {
                    uint32_t byte_idx = col * (fb->bpp / 8) + c;
                    uint8_t old_val = row_ptr[byte_idx];
                    row_ptr[byte_idx] = (uint8_t)((uint32_t)old_val * (256 - progress) / 256);
                }
            }
        }
        task_yield();
    }

    session_restore_framebuffer(to_session);

    for (uint32_t step = 0; step < half; step++) {
        uint32_t progress = (step + 1) * 256 / half;
        if (progress > 256) progress = 256;

        uint8_t* pixels = (uint8_t*)fb->virtual_addr;
        uint32_t rows_this_step = height / half;
        if (rows_this_step == 0) rows_this_step = 1;
        uint32_t row_start = step * rows_this_step;
        uint32_t row_end = row_start + rows_this_step;
        if (row_end > height) row_end = height;

        for (uint32_t row = row_start; row < row_end; row++) {
            uint8_t* row_ptr = pixels + row * pitch;
            for (uint32_t col = 0; col < fb->width; col++) {
                for (uint32_t c = 0; c < (fb->bpp / 8); c++) {
                    uint32_t byte_idx = col * (fb->bpp / 8) + c;
                    uint8_t final_val = row_ptr[byte_idx];
                    uint8_t zero_val = 0;
                    row_ptr[byte_idx] = (uint8_t)((uint32_t)zero_val * (256 - progress) +
                                                  (uint32_t)final_val * progress / 256);
                }
            }
        }
        task_yield();
    }

    debuglog(DEBUG_INFO, "[SESSION] Transition from TTY %u to TTY %u complete\n",
             from_session, to_session);
}

// ============================================================================
// TTY Login
// ============================================================================

static void print_banner(void) {
    extern uint32_t g_current_tty_session;
    char banner[128];
    snprintf(banner, sizeof(banner), "\x1b[32mFern - TTY %u\x1b[0m\n", g_current_tty_session);
    tty_write_ansi(banner);
    tty_write_ansi("Type 'signup' at the username prompt to create an account.\n");
    tty_write_ansi("Press Ctrl+Alt+F1-F12 to switch TTY sessions, or use 'chvt N' (1-24).\n\n");
}

// Read input with hotkey checking - returns false if session switch occurred
static bool read_line_with_hotkey_check(char* buffer, size_t max_len, bool hidden) {
    size_t i = 0;
    uint32_t start_session = g_current_tty_session;
    uint8_t start_vt = tty_get_current_vt();

    while (1) {
        // Check for session switch or VT switch
        if (g_current_tty_session != start_session || tty_get_current_vt() != start_vt) {
            buffer[i] = '\0';
            return false;  // Session or VT switched, abort input
        }

        char ch = 0;
        if (!keyboard_poll_char(&ch)) {
            // No input available, yield and continue
            session_idle_wait();
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            tty_write("\n");
            break;
        }

        if (ch == '\b') {
            if (i > 0) {
                i--;
                tty_write("\b \b");
            }
            continue;
        }

        if (ch == 0x03) {
            // Ctrl+C: drop it silently instead of appending it to the
            // username/password buffer. Auto-repeat while the key is held
            // can enqueue several of these; letting any through corrupts
            // the credential string with an invisible control byte that
            // then mismatches on comparison. Only this byte is discarded -
            // other queued bytes (real keystrokes typed right after) are
            // left untouched.
            continue;
        }

        if (i + 1 < max_len) {
            buffer[i++] = ch;
            buffer[i] = '\0';
            if (hidden) {
                tty_write("*");
            } else {
                char echo[2] = {ch, '\0'};
                tty_write(echo);
            }
        }
    }

    buffer[i] = '\0';
    return true;  // Input completed successfully
}

static bool handle_signup(void) {
    tty_write("New username: ");
    char name[SESSION_INPUT_MAX];
    if (!read_line_with_hotkey_check(name, sizeof(name), false) || name[0] == '\0') {
        return false;
    }

    tty_write("New password: ");
    char pass[SESSION_INPUT_MAX];
    if (!read_line_with_hotkey_check(pass, sizeof(pass), true) || pass[0] == '\0') {
        return false;
    }

    auth_result_t res = auth_signup(name, pass, "users", false);

    if (res == AUTH_OK) {
        tty_write("Account created.\n");
        return true;
    }
    tty_write("Signup failed. Root login may be required.\n");
    return false;
}

// Returns: 1 = login successful, 0 = keep trying, -1 = session switched
static int prompt_tty_login_once(auth_user_info_t* out_user) {
    uint32_t start_session = g_current_tty_session;
    uint8_t start_vt = tty_get_current_vt();

    tty_write("Username: ");
    char user[SESSION_INPUT_MAX];
    if (!read_line_with_hotkey_check(user, sizeof(user), false)) {
        return -1;  // Session or VT switched
    }

    if (user[0] == '\0') {
        return 0;  // Empty input, try again
    }

    if (strcmp(user, "signup") == 0) {
        handle_signup();
        return 0;  // Continue login loop
    }

    // Check if session or VT switched during username input
    if (g_current_tty_session != start_session || tty_get_current_vt() != start_vt) {
        return -1;
    }

    tty_write("Password: ");
    char pass[SESSION_INPUT_MAX];
    if (!read_line_with_hotkey_check(pass, sizeof(pass), true)) {
        return -1;  // Session or VT switched
    }

    // Check if session or VT switched during password input
    if (g_current_tty_session != start_session || tty_get_current_vt() != start_vt) {
        return -1;
    }

    auth_result_t res = auth_login(user, pass, out_user);

    if (res == AUTH_OK) {
        session_update_tty_status_text("Login successful");
        tty_write("Login successful.\n\n");
        return 1;  // Success
    }

    session_update_tty_status_text("Invalid credentials");
    tty_write("Invalid credentials. Try again.\n");
    return 0;  // Keep trying
}

static bool prompt_tty_login(auth_user_info_t* out_user) {
    uint32_t start_session = g_current_tty_session;
    uint8_t start_vt = tty_get_current_vt();

    while (1) {
        int result = prompt_tty_login_once(out_user);
        if (result == 1) {
            return true;   // Login successful
        }
        if (result == -1) {
            // Check if just VT switched (not session)
            if (g_current_tty_session == start_session && tty_get_current_vt() != start_vt) {
                // VT switched but same session - redraw and continue
                tty_clear();
                print_banner();
                start_vt = tty_get_current_vt();
                continue;
            }
            return false;  // Session switched, exit login loop
        }
        // result == 0: keep trying
    }
}

// ============================================================================
// Desktop Environment / Shell Launch
// ============================================================================

static bool launch_user_session(auth_user_info_t* user_info, tty_session_t* session) {
    debuglog(DEBUG_INFO, "[SESSION] launch_user_session() called for user '%s' on TTY %u (type=%s)\n",
             user_info->name, session->session_id, 
             session->type == SESSION_TYPE_GUI ? "GUI" : "TEXT");
    
    // Load DE configuration from system config or user config
    char de_path[SESSION_DE_PATH_MAX];
    bool de_found = false;

    // For GUI sessions, try to launch DE; for text sessions, launch shell
    if (session->type == SESSION_TYPE_GUI &&
        (!graphics_is_initialized() || !tty_try_enable_graphics_backend() || !tty_is_ready())) {
        debuglog(DEBUG_WARN,
                 "[SESSION] Graphics backend unavailable for DE launch, falling back to shell\n");
        session->type = SESSION_TYPE_TEXT;
    }

    if (session->type == SESSION_TYPE_GUI) {
        strcpy(de_path, "/bin/shell");

        // Prefer user-specific desktop config over system-wide config.
        de_found = load_user_desktop_path(user_info, de_path, sizeof(de_path));
        if (!de_found) {
            de_found = load_system_desktop_path(de_path, sizeof(de_path));
        }
        if (!de_found) {
            strcpy(de_path, "/bin/shell");
        }

        // Try to launch the Desktop Environment
        const uint8* de_elf_data = NULL;
        uint32 de_elf_size = 0;
        bool de_loaded = false;

        // Use preloaded DE ELF if path matches (loaded during DM wait to avoid black screen)
        if (strcmp(de_path, "/bin/shell") == 0 &&
            g_preloaded_de_elf && g_preloaded_de_elf_size > 0) {
            de_elf_data = g_preloaded_de_elf;
            de_elf_size = g_preloaded_de_elf_size;
            de_loaded = true;
            debuglog(DEBUG_INFO, "[SESSION] Using pre-loaded DE ELF: %u bytes\n", de_elf_size);
            tty_write("[SESSION] Using pre-loaded desktop environment.\n");
        } else {
            de_loaded = vfs_read_file(de_path, &de_elf_data, &de_elf_size) &&
                         de_elf_data && de_elf_size > 0;
        }

        if (!de_loaded) {
            const char* de_backup_paths[] = {
                "/bin/shell",
                "/usr/bin/shell",
                "/usr/local/bin/shell",
                NULL
            };
            for (int bp = 0; de_backup_paths[bp] != NULL; bp++) {
                if (strcmp(de_backup_paths[bp], de_path) == 0) continue;
                if (vfs_read_file(de_backup_paths[bp], &de_elf_data, &de_elf_size) &&
                    de_elf_data && de_elf_size > 0) {
                    strncpy(de_path, de_backup_paths[bp], sizeof(de_path) - 1);
                    de_path[sizeof(de_path) - 1] = '\0';
                    de_loaded = true;
                    debuglog(DEBUG_INFO, "[SESSION] DE found at backup path: %s\n", de_path);
                    break;
                }
            }
        }

        if (de_loaded) {
            debuglog(DEBUG_INFO, "[SESSION] DE ELF loaded: %s (%u bytes)\n", de_path, de_elf_size);
            
            bool de_valid = true;

            // Validate ELF before loading
            if (!elf_is_valid(de_elf_data, de_elf_size)) {
                int val_err = elf_validate_header((const elf32_ehdr_t*)de_elf_data);
                debuglog(DEBUG_ERROR, "[SESSION] DE ELF invalid: %s (code=%d) at %s (%u bytes)\n",
                         elf_validate_error_string(val_err), val_err, de_path, de_elf_size);
                char err_msg[128];
                snprintf(err_msg, sizeof(err_msg), "DE ELF corrupt: %s", elf_validate_error_string(val_err));
                session_update_tty_status_text(err_msg);
                tty_write("[SESSION] Desktop environment binary is corrupt or invalid.\n");
                de_valid = false;
            }

            // Check memory availability
            if (de_valid && !elf_has_enough_memory(de_elf_data, de_elf_size)) {
                debuglog(DEBUG_ERROR, "[SESSION] Insufficient memory for DE (free=%u frames)\n",
                         pmm_get_free_frames());
                session_update_tty_status_text("Not enough memory for DE");
                tty_write("[SESSION] Insufficient memory to load desktop environment.\n");
                de_valid = false;
            }

            if (de_valid) {
                char de_task_name[32] = "desktop";
                const char* base = strrchr(de_path, '/');
                base = base ? base + 1 : de_path;
                size_t i = 0;
                while (base[i] && base[i] != '.' && i + 1 < sizeof(de_task_name)) {
                    de_task_name[i] = base[i];
                    i++;
                }
                de_task_name[i] = '\0';

                debuglog(DEBUG_INFO, "[SESSION] Creating DE task '%s' from %s (%u bytes, free=%u frames)...\n",
                         de_task_name, de_path, de_elf_size, pmm_get_free_frames());
                tty_set_graphics_app_active(true);
                tty_set_status_bar_visible(false);
                tty_write("[SESSION] Starting desktop environment...\n");
                task_t* de_task = task_create_elf(de_elf_data, de_elf_size, de_task_name);

                // Retry from backup paths if primary failed
                if (!de_task) {
                    const char* de_retry_paths[] = {
                        "/bin/shell",
                        "/usr/bin/shell",
                        "/usr/local/bin/shell",
                        NULL
                    };
                    for (int rp = 0; de_retry_paths[rp] != NULL; rp++) {
                        if (strcmp(de_retry_paths[rp], de_path) == 0) continue;
                        const uint8* retry_data = NULL;
                        uint32 retry_size = 0;
                        if (vfs_read_file(de_retry_paths[rp], &retry_data, &retry_size) &&
                            retry_data && retry_size > 0 && elf_is_valid(retry_data, retry_size) &&
                            elf_has_enough_memory(retry_data, retry_size)) {
                            de_task = task_create_elf(retry_data, retry_size, de_task_name);
                            if (de_task) {
                                debuglog(DEBUG_INFO, "[SESSION] DE retry succeeded from %s\n", de_retry_paths[rp]);
                                break;
                            }
                        }
                    }
                }
                if (de_task) {
                uint32 de_pid = de_task->id;
                debuglog(DEBUG_INFO, "[SESSION] DE task created (PID %u), preparing to launch...\n", de_pid);
                // Only now does a graphical desktop actually exist to composite - start the
                // WM render loop here rather than unconditionally at boot, so it never blits
                // over the text-mode login console (see kernel.c kmain() for the full story).
                wm_start_render_loop_task();
                task_set_graphics_task(de_task, true);
                de_task->priority = TASK_PRIORITY_REALTIME;

                // Set as foreground task for priority scheduling
                debuglog(DEBUG_INFO, "[SESSION] Setting foreground task (PID %u)...\n", de_pid);
                task_set_foreground(de_task);

                de_task->state = TASK_STATE_READY;
                session->shell_pid = de_pid;
                
                debuglog(DEBUG_INFO, "[SESSION] Scheduling DE task (PID %u)...\n", de_pid);
                task_schedule();

                // Wait for DE to complete
                debuglog(DEBUG_INFO, "[SESSION] DE running as foreground (PID %u), waiting for startup...\n", de_pid);

                debuglog(DEBUG_INFO, "[SESSION] Waiting for DE startup confirmation...\n");
                graphics_task_startup_result_t de_startup =
                    wait_for_graphics_task_startup(de_pid, "Desktop environment", DE_STARTUP_TIMEOUT_TICKS);
                if (de_startup != GRAPHICS_TASK_STARTUP_OK) {
                    if (de_startup == GRAPHICS_TASK_STARTUP_TIMEOUT) {
                        debuglog(DEBUG_ERROR, "[SESSION] DE startup FAILED (PID %u timed out)\n", de_pid);
                    } else {
                        debuglog(DEBUG_ERROR, "[SESSION] DE startup FAILED (PID %u exited early)\n", de_pid);
                    }
                    task_clear_foreground();
                    session->shell_pid = 0;
                    framebuffer_set_preserve_last_frame(false);
                    tty_set_graphics_app_active(false);
                    tty_set_status_bar_visible(true);
                    tty_clear();
                    tty_force_redraw();
                     debuglog(DEBUG_ERROR,
                             "[SESSION] DE failed to start, falling back to shell\n");
                     session->type = SESSION_TYPE_TEXT;
                     tty_write("[SESSION] Desktop environment failed. Falling back to shell.\n");
                     debuglog(DEBUG_WARN, "[SESSION] DE failed to start, falling back to shell\n");
                     uint32 wm_pid = task_get_id_by_name_prefix("wm-");
                     if (wm_pid != 0) {
                         /* Text fallback: the compositor must NOT own the framebuffer,
                          * or its render loop repaints the empty desktop over the
                          * shell's text every frame (tty_putc reaches the FB but is
                          * immediately overwritten). Mirror run_session_login(), which
                          * suspends wm-render for exactly this reason. */
                         task_suspend(wm_pid);
                         debuglog(DEBUG_INFO, "[SESSION] Suspended WM render task (PID %u) for text fallback\n", wm_pid);
                     }
                     tty_clear();
                     tty_force_redraw();
                 } else {
                     debuglog(DEBUG_INFO, "[SESSION] DE startup confirmed (PID %u), entering main loop...\n", de_pid);
                     tty_write("[SESSION] Desktop environment running.\n");

                     bool fb_mapped = false;
                     uint32 fb_mmap_wait = timer_get_ticks();
                     while (!framebuffer_has_userspace_mapping()) {
                         if ((timer_get_ticks() - fb_mmap_wait) > DE_FRAMEBUFFER_MMAP_TIMEOUT_TICKS) {
                             debuglog(DEBUG_ERROR,
                                      "[SESSION] DE framebuffer mmap timeout after %u ticks\n",
                                      DE_FRAMEBUFFER_MMAP_TIMEOUT_TICKS);
                             tty_write("[SESSION] FATAL: Desktop framebuffer failed to map.\n");
                             break;
                         }
                         if (!task_exists(de_pid)) {
                             debuglog(DEBUG_ERROR, "[SESSION] DE exited during framebuffer mmap wait\n");
                             tty_write("[SESSION] FATAL: Desktop exited during framebuffer setup.\n");
                             break;
                         }
                         session_idle_wait();
                     }
                     fb_mapped = framebuffer_has_userspace_mapping();
                     if (fb_mapped) {
                         debuglog(DEBUG_INFO, "[SESSION] DE framebuffer mapped successfully\n");
                         framebuffer_set_preserve_last_frame(false);
                         task_clear_foreground();
                         session->shell_pid = de_pid;
                         g_fb_owner_session_active = true;
                         g_fb_owner_session = session->session_id;
                         debuglog(DEBUG_INFO, "[SESSION] DE launched (PID %u)\n", de_pid);
                         return true;
                     }

                     // Framebuffer mmap failed - clean up DE and fall back to shell
                     debuglog(DEBUG_ERROR, "[SESSION] DE framebuffer mmap failed, falling back to shell\n");
                     task_clear_foreground();
                     session->shell_pid = 0;
                     if (task_exists(de_pid)) {
                         task_send_signal(de_pid, SIGTERM);
                         uint32_t kill_wait = 0;
                         while (task_exists(de_pid) && kill_wait < 30) {
                             session_idle_wait();
                             kill_wait++;
                         }
                         if (task_exists(de_pid)) {
                             task_kill(de_pid);
                         }
                     }
                     framebuffer_set_preserve_last_frame(false);
                     tty_set_graphics_app_active(false);
                     tty_set_status_bar_visible(true);
                     tty_clear();
                     tty_force_redraw();
                     session->type = SESSION_TYPE_TEXT;
                      tty_write("[SESSION] Desktop environment failed. Falling back to shell.\n");
                      uint32 wm_pid = task_get_id_by_name_prefix("wm-");
                      if (wm_pid != 0) {
                          /* Text fallback: suspend the compositor so it stops
                           * repainting the empty desktop over the shell (see
                           * matching comment on the DE-startup-fail path above). */
                          task_suspend(wm_pid);
                          debuglog(DEBUG_INFO, "[SESSION] Suspended WM render task (PID %u) for text fallback (mmap)\n", wm_pid);
                      }
                      tty_clear();
                      tty_force_redraw();
                  }
            } else {
                debuglog(DEBUG_ERROR, "[SESSION] Failed to create DE task: %s\n", de_path);
                framebuffer_set_preserve_last_frame(false);
                tty_write("[SESSION] Failed to start desktop environment.\n");
                tty_set_graphics_app_active(false);
                tty_set_status_bar_visible(true);
                tty_clear();
                tty_force_redraw();
                session->type = SESSION_TYPE_TEXT;
            }
        } else {
            debuglog(DEBUG_INFO, "[SESSION] DE not found: %s, falling back to shell\n", de_path);
            tty_write("[SESSION] Desktop environment not found. Falling back to shell.\n");
            tty_set_graphics_app_active(false);
            tty_set_status_bar_visible(true);
            tty_clear();
            tty_force_redraw();
            session->type = SESSION_TYPE_TEXT;
        }

        if (session->type == SESSION_TYPE_GUI) {
            tty_set_graphics_app_active(false);
            tty_set_status_bar_visible(true);
            tty_clear();
            tty_force_redraw();
        }
    }
    }

    (void)&&shell_fallback;
shell_fallback:
    debuglog(DEBUG_INFO, "[SESSION] Launching shell fallback for user '%s' on TTY %u\n",
             user_info->name, session->session_id);
    tty_write("[SESSION] Starting shell...\n");

    const char* shell_paths[] = {
        "/bin/shell",
        "/usr/bin/shell",
        NULL
    };
    char shell_path[SESSION_DE_PATH_MAX];
    const uint8* shell_elf_data = NULL;
    uint32 shell_elf_size = 0;
    uint32 shell_pid = 0;
    task_t* shell_task = NULL;

    if (load_first_elf(shell_paths, shell_path, sizeof(shell_path), &shell_elf_data, &shell_elf_size)) {
        shell_task = task_create_elf(shell_elf_data, shell_elf_size, "sh");
        if (shell_task) {
            shell_pid = shell_task->id;
            debuglog(DEBUG_INFO, "[SESSION] Shell launched from %s (PID %u)\n", shell_path, shell_pid);
        } else {
            debuglog(DEBUG_ERROR, "[SESSION] Failed to create shell task from %s\n", shell_path);
        }
    } else {
        debuglog(DEBUG_WARN, "[SESSION] /bin/shell not found, trying embedded shell loader\n");
        bool shell_ok = shell_launch_embedded();
        shell_pid = shell_get_last_pid();
        shell_task = shell_get_last_task();
        if (!shell_ok || shell_pid == 0) {
            shell_task = NULL;
        }
    }

    if (!shell_task || shell_pid == 0) {
        tty_write("Shell failed to start.\n");
        debuglog(DEBUG_ERROR, "[SESSION] Shell failed to start, logging out\n");
        return false;
    }

    session->shell_pid = shell_pid;

    if (shell_task) {
        // Without a controlling terminal, tcsetpgrp()/tcgetpgrp() fail with
        // ENOTTY (see sys_tcsetpgrp() in syscall.c) and the keyboard IRQ
        // handler has no g_virtual_ttys[].fg_pgid to target for Ctrl+C, so
        // job control silently does nothing end-to-end. task_create_elf()
        // already defaults pgrp/session to the shell's own PID (session
        // leader, own process group); tty_fd is the one field nothing else
        // sets. g_virtual_ttys is 0-indexed, session_id is 1-indexed.
        shell_task->tty_fd = (int32)(session->session_id - 1);
        task_set_foreground(shell_task);
        shell_task->state = TASK_STATE_READY;
        task_schedule();
    }

    // Shell launched successfully
    session->shell_pid = shell_pid;
    debuglog(DEBUG_INFO, "[SESSION] Shell launched (PID %u)\n", shell_pid);
    return true;
}

// ============================================================================
// Per-Session Login Handler
// ============================================================================

static void run_session_login(tty_session_t* session, bool autologin_root) {
    /* Suspend the WM render task so it stops writing to the framebuffer.
     * Without this, wm-render overwrites TTY clears making the display appear frozen. */
    {
        uint32 wm_pid = task_get_id_by_name_prefix("wm-");
        if (wm_pid != 0) {
            task_suspend(wm_pid);
            debuglog(DEBUG_INFO, "[SESSION] Suspended WM render task (PID %u) for TTY login\n",
                     wm_pid);
        }
    }
    tty_set_graphics_app_active(false);
    tty_force_redraw();

    if (session->state == SESSION_STATE_RECOVERY) {
        debuglog(DEBUG_INFO, "[SESSION] Recovery mode for session %u, restarting DE\n", session->session_id);
        session_update_tty_status_text("Recovering desktop environment");
            tty_write("[SESSION] Recovering desktop environment...\n");
            tty_set_graphics_app_active(false);
            tty_set_status_bar_visible(true);
            tty_clear();
            tty_force_redraw();
            session->state = SESSION_STATE_ACTIVE;
            /* Do NOT increment de_crash_count here: session_check_recovery()
             * already incremented it at crash-detection time and set
             * SESSION_STATE_RECOVERY. This branch only acts on the
             * already-updated count to relaunch the DE. */
            goto launch_session;
    }

    session->state = SESSION_STATE_LOGIN;
    session->logged_in = false;
    session_clear_tty_status_user();
    session_update_tty_status_text("Waiting for login");

    extern void tty_set_status_bar_visible(bool visible);
    tty_set_status_bar_visible(false);

    tty_clear();

    if (autologin_root && auth_force_login("root") == AUTH_OK) {
        auth_find_user("root", &session->user_info);
        session->logged_in = true;
        session_update_tty_status_text("Auto-login as root");
        tty_write_ansi("\x1b[33mAuto-login enabled for root\x1b[0m\n");
    }

    if (!session->logged_in) {
        session_update_tty_status_text("TTY login prompt");
        tty_clear();
        print_banner();
        session->logged_in = prompt_tty_login(&session->user_info);
    }

    if (!session->logged_in) {
        session_update_tty_status_text("Login interrupted");
        return;
    }

launch_session:
    session_update_tty_status_user(session->user_info.name);
    session_update_tty_status_text("Session active");
    session->state = SESSION_STATE_ACTIVE;
    tty_set_status_bar_visible(true);

    debuglog(DEBUG_INFO, "[SESSION] User '%s' logged in on TTY %u\n",
             session->user_info.name, session->session_id);
    tty_write("[SESSION] User logged in.\n");

    debuglog(DEBUG_INFO, "[SESSION] About to launch user session...\n");
    if (launch_user_session(&session->user_info, session)) {
        debuglog(DEBUG_INFO, "[SESSION] Waiting for user session (PID %u) to exit\n", session->shell_pid);
        while (task_exists(session->shell_pid)) {
            session_idle_wait();
        }
        debuglog(DEBUG_INFO, "[SESSION] User session exited\n");
        tty_write("[SESSION] Session ended.\n");
    }

    session_cleanup_session(session);
    auth_logout();
    // Drain any keystrokes typed during the just-ended session (shell
    // commands, the "exit" that ended it, etc.) before showing the next
    // login prompt. keyboard_poll_char() reads from a single global ASCII
    // ring buffer shared by every consumer (see ps2_keyboard_send_event()),
    // so without this, leftover bytes from the prior session get consumed
    // as part of the next username/password input, corrupting an otherwise
    // correct relogin attempt.
    keyboard_clear_buffers();
    session_update_tty_status_text("Logged out");
    tty_write("Session ended. Returning to login...\n\n");
    session->state = SESSION_STATE_LOGIN;

    uint32 wm_pid = task_get_id_by_name_prefix("wm-");
    if (wm_pid != 0) {
        task_resume(wm_pid);
        debuglog(DEBUG_INFO, "[SESSION] Resumed WM render task (PID %u) after session end\n", wm_pid);
    }
}

// ============================================================================
// Main Session Loop
// ============================================================================

void session_run(bool autologin_root) {
    debuglog_printf("[SESSION] session_run enter\n");
    auth_init();
    debuglog_printf("[SESSION] auth_init complete\n");

    // Initialize all TTY sessions
    session_init_all();

    // Prefer the PS/2 driver for proper scancode translation
    keyboard_set_driver_mode(KEYBOARD_DRIVER_PS2);

    // Ensure the framebuffer TTY is active and clean
    tty_try_enable_graphics_backend();
    if (tty_in_boot_mode()) {
        tty_exit_boot_mode();
    }
    tty_clear();
    tty_write_ansi("\x1b[0m");
    tty_write("Starting session manager...\n");
    debuglog_printf("[SESSION] tty prepared, entering session loop\n");

    // Drop any stale scancodes from boot
    keyboard_clear_buffers();

    debuglog(DEBUG_INFO, "[SESSION] Starting multi-TTY session manager\n");
    debuglog(DEBUG_INFO, "[SESSION] TTY 1: GUI login, TTY 2-9: Text TTY login\n");
    debuglog(DEBUG_INFO, "[SESSION] Use Ctrl+Alt+F1-F12 or 'chvt' to switch sessions\n");

    while (1) {
        session_check_recovery();

        tty_session_t* current = session_get_current();
        if (!current) {
            debuglog(DEBUG_ERROR, "[SESSION] session_get_current() returned NULL, reinitializing\n");
            vga_write_error("SESSION: session state lost - reinitializing");
            tty_write("[SESSION] Session state lost. Reinitializing...\n");
            g_sessions_initialized = false;
            session_init_all();
            g_current_tty_session = 1;
            current = session_get_current();
            if (!current) {
                /* session_init_all() unconditionally sets g_sessions_initialized
                 * and g_current_tty_session=1 is always in [1, MAX_TTY_SESSIONS],
                 * so this is unreachable. Skip this loop iteration rather than
                 * halting the machine - a transient failure here should not be
                 * unrecoverable. */
                debuglog(DEBUG_ERROR, "[SESSION] Reinitialization failed, retrying next iteration\n");
                continue;
            }
        }

        uint32_t prev_session = g_current_tty_session;
        run_session_login(current, autologin_root);

        if (g_current_tty_session != prev_session) {
            tty_session_t* new_session = session_get(g_current_tty_session);
            tty_set_graphics_app_active(false);
            tty_force_redraw();
            if (new_session) {
                debuglog(DEBUG_INFO, "[SESSION] Switched to session %u\n",
                         g_current_tty_session);
            }
        }
    }
}

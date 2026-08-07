#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "auth.h"

#define MAX_TTY_SESSIONS 22
#define SESSION_RECOVERY_LIMIT 3

typedef enum {
    SESSION_TYPE_GUI,
    SESSION_TYPE_TEXT
} session_type_t;

typedef enum {
    SESSION_STATE_LOGIN,
    SESSION_STATE_ACTIVE,
    SESSION_STATE_LOGOUT,
    SESSION_STATE_SUSPENDED,
    SESSION_STATE_RECOVERY
} session_state_t;

typedef struct {
    uint32_t session_id;
    session_type_t type;
    session_state_t state;
    bool logged_in;
    auth_user_info_t user_info;
    uint32_t shell_pid;
    bool initialized;
    bool gui_suspended;
    uint32_t de_crash_count;
    void* fb_snapshot;
    uint32_t fb_snapshot_size;
    bool fb_snapshot_valid;
} tty_session_t;

typedef enum {
    SESSION_TRANSITION_NONE,
    SESSION_TRANSITION_FADE_OUT,
    SESSION_TRANSITION_FADE_IN
} session_transition_phase_t;

void session_run(bool autologin_root);
tty_session_t* session_get_current(void);
tty_session_t* session_get(uint32_t session_num);
void session_init_all(void);
void session_switch_to(uint32_t session_num);
bool session_check_hotkey(void);
bool session_save_framebuffer(uint32_t session_num);
bool session_restore_framebuffer(uint32_t session_num);
void session_free_framebuffer_snapshot(uint32_t session_num);
void session_suspend_gui(uint32_t session_num);
void session_resume_gui(uint32_t session_num);
bool session_check_recovery(void);
void session_cleanup_session(tty_session_t* session);
void session_get_status_string(char* buf, size_t buf_size);

#endif // SESSION_H

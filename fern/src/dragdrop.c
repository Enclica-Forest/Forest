#include "include/dragdrop.h"
#include "include/debuglog.h"
#include "include/spinlock.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

static drag_drop_state_t g_drag;
static dragdrop_state_t g_state = DRAGDROP_STATE_IDLE;
static dragdrop_action_t g_action = DRAGDROP_ACTION_COPY;
static dragdrop_target_t g_targets[DRAGDROP_MAX_TARGETS];
static uint32_t g_target_count = 0;
static dragdrop_target_t* g_hovered_target = NULL;
static dragdrop_callbacks_t g_callbacks;
static dragdrop_event_fn g_event_handler = NULL;
static void* g_event_user_data = NULL;
static spinlock_t g_lock;
static int32_t g_drag_threshold_counter = 0;

void dragdrop_init(void) {
    spinlock_init(&g_lock, "dragdrop");
    memset(&g_drag, 0, sizeof(g_drag));
    memset(g_targets, 0, sizeof(g_targets));
    g_target_count = 0;
    g_state = DRAGDROP_STATE_IDLE;
    g_action = DRAGDROP_ACTION_COPY;
    g_hovered_target = NULL;
    g_event_handler = NULL;
    g_event_user_data = NULL;
    g_drag_threshold_counter = 0;
    memset(&g_callbacks, 0, sizeof(g_callbacks));
    debuglog(DEBUG_INFO, "[DRAGDROP] drag-drop subsystem initialized\n");
}

void dragdrop_shutdown(void) {
    spinlock_acquire(&g_lock);
    if (g_drag.active) {
        g_state = DRAGDROP_STATE_CANCELLED;
        if (g_callbacks.on_drag_cancel) {
            g_callbacks.on_drag_cancel();
        }
    }
    memset(&g_drag, 0, sizeof(g_drag));
    g_state = DRAGDROP_STATE_IDLE;
    g_target_count = 0;
    g_hovered_target = NULL;
    spinlock_release(&g_lock);
    debuglog(DEBUG_INFO, "[DRAGDROP] drag-drop subsystem shut down\n");
}

int dragdrop_start(uint32_t source_pid, uint32_t source_window,
                   int32_t start_x, int32_t start_y) {
    spinlock_acquire(&g_lock);

    if (g_drag.active) {
        spinlock_release(&g_lock);
        return -1;
    }

    g_drag.source_pid = source_pid;
    g_drag.target_pid = 0;
    g_drag.source_window = source_window;
    g_drag.target_window = 0;
    g_drag.start_x = start_x;
    g_drag.start_y = start_y;
    g_drag.current_x = start_x;
    g_drag.current_y = start_y;
    g_drag.active = true;
    g_drag.data_type = CLIPBOARD_TYPE_NONE;
    g_drag.data_size = 0;

    g_state = DRAGDROP_STATE_DRAGGING;
    g_action = DRAGDROP_ACTION_COPY;
    g_hovered_target = NULL;
    g_drag_threshold_counter = 0;

    if (g_callbacks.on_drag_start) {
        g_callbacks.on_drag_start(source_pid, source_window);
    }

    spinlock_release(&g_lock);
    debuglog(DEBUG_DETAIL, "[DRAGDROP] drag started from pid=%u window=%u at (%d,%d)\n",
             source_pid, source_window, start_x, start_y);
    return 0;
}

void dragdrop_update(int32_t x, int32_t y) {
    spinlock_acquire(&g_lock);

    if (!g_drag.active) {
        spinlock_release(&g_lock);
        return;
    }

    g_drag.current_x = x;
    g_drag.current_y = y;
    g_drag_threshold_counter++;

    dragdrop_target_t* found = NULL;
    for (uint32_t i = 0; i < g_target_count; ++i) {
        dragdrop_target_t* t = &g_targets[i];
        if (t->pid == g_drag.source_pid && t->window_id == g_drag.source_window) {
            continue;
        }
        if (x >= t->x && x < t->x + (int32_t)t->width &&
            y >= t->y && y < t->y + (int32_t)t->height) {
            bool accepts = false;
            switch (g_drag.data_type) {
                case CLIPBOARD_TYPE_TEXT: accepts = t->accepts_text; break;
                case CLIPBOARD_TYPE_IMAGE: accepts = t->accepts_image; break;
                case CLIPBOARD_TYPE_FILE: accepts = t->accepts_file; break;
                case CLIPBOARD_TYPE_URI: accepts = t->accepts_uri; break;
                case CLIPBOARD_TYPE_CUSTOM: accepts = t->accepts_custom; break;
                default: accepts = true; break;
            }
            if (accepts) {
                found = t;
                break;
            }
        }
    }

    if (found != g_hovered_target) {
        if (g_hovered_target && g_callbacks.on_drag_leave) {
            g_callbacks.on_drag_leave(g_hovered_target->pid,
                                      g_hovered_target->window_id);
        }
        g_hovered_target = found;
        if (g_hovered_target) {
            g_drag.target_pid = g_hovered_target->pid;
            g_drag.target_window = g_hovered_target->window_id;
            g_state = DRAGDROP_STATE_OVER_TARGET;
            if (g_callbacks.on_drag_enter) {
                g_callbacks.on_drag_enter(g_hovered_target->pid,
                                          g_hovered_target->window_id);
            }
        } else {
            g_drag.target_pid = 0;
            g_drag.target_window = 0;
            g_state = DRAGDROP_STATE_DRAGGING;
        }
    }

    spinlock_release(&g_lock);
}

int dragdrop_enter_target(uint32_t target_pid, uint32_t target_window) {
    spinlock_acquire(&g_lock);

    if (!g_drag.active) {
        spinlock_release(&g_lock);
        return -1;
    }

    for (uint32_t i = 0; i < g_target_count; ++i) {
        dragdrop_target_t* t = &g_targets[i];
        if (t->pid == target_pid && t->window_id == target_window) {
            bool accepts = false;
            switch (g_drag.data_type) {
                case CLIPBOARD_TYPE_TEXT: accepts = t->accepts_text; break;
                case CLIPBOARD_TYPE_IMAGE: accepts = t->accepts_image; break;
                case CLIPBOARD_TYPE_FILE: accepts = t->accepts_file; break;
                case CLIPBOARD_TYPE_URI: accepts = t->accepts_uri; break;
                case CLIPBOARD_TYPE_CUSTOM: accepts = t->accepts_custom; break;
                default: accepts = true; break;
            }
            if (!accepts) {
                spinlock_release(&g_lock);
                return -2;
            }

            if (g_hovered_target && g_hovered_target != t) {
                if (g_callbacks.on_drag_leave) {
                    g_callbacks.on_drag_leave(g_hovered_target->pid,
                                              g_hovered_target->window_id);
                }
            }

            g_hovered_target = t;
            g_drag.target_pid = target_pid;
            g_drag.target_window = target_window;
            g_state = DRAGDROP_STATE_OVER_TARGET;

            if (g_callbacks.on_drag_enter) {
                g_callbacks.on_drag_enter(target_pid, target_window);
            }

            spinlock_release(&g_lock);
            debuglog(DEBUG_DETAIL, "[DRAGDROP] entered target pid=%u window=%u\n",
                     target_pid, target_window);
            return 0;
        }
    }

    spinlock_release(&g_lock);
    return -3;
}

void dragdrop_leave_target(void) {
    spinlock_acquire(&g_lock);

    if (!g_drag.active || !g_hovered_target) {
        spinlock_release(&g_lock);
        return;
    }

    if (g_callbacks.on_drag_leave) {
        g_callbacks.on_drag_leave(g_hovered_target->pid,
                                  g_hovered_target->window_id);
    }

    g_hovered_target = NULL;
    g_drag.target_pid = 0;
    g_drag.target_window = 0;
    g_state = DRAGDROP_STATE_DRAGGING;

    spinlock_release(&g_lock);
}

int dragdrop_drop(int32_t drop_x, int32_t drop_y) {
    spinlock_acquire(&g_lock);

    if (!g_drag.active) {
        spinlock_release(&g_lock);
        return -1;
    }

    g_drag.current_x = drop_x;
    g_drag.current_y = drop_y;

    dragdrop_target_t* target = g_hovered_target;
    if (!target) {
        for (uint32_t i = 0; i < g_target_count; ++i) {
            dragdrop_target_t* t = &g_targets[i];
            if (t->pid == g_drag.source_pid && t->window_id == g_drag.source_window) {
                continue;
            }
            if (drop_x >= t->x && drop_x < t->x + (int32_t)t->width &&
                drop_y >= t->y && drop_y < t->y + (int32_t)t->height) {
                bool accepts = false;
                switch (g_drag.data_type) {
                    case CLIPBOARD_TYPE_TEXT: accepts = t->accepts_text; break;
                    case CLIPBOARD_TYPE_IMAGE: accepts = t->accepts_image; break;
                    case CLIPBOARD_TYPE_FILE: accepts = t->accepts_file; break;
                    case CLIPBOARD_TYPE_URI: accepts = t->accepts_uri; break;
                    case CLIPBOARD_TYPE_CUSTOM: accepts = t->accepts_custom; break;
                    default: accepts = true; break;
                }
                if (accepts) {
                    target = t;
                    break;
                }
            }
        }
    }

    if (target) {
        g_drag.target_pid = target->pid;
        g_drag.target_window = target->window_id;
        g_state = DRAGDROP_STATE_DROPPED;

        if (g_callbacks.on_drop) {
            g_callbacks.on_drop(target->pid, target->window_id,
                                g_drag.data_type, g_drag.data,
                                g_drag.data_size);
        }
        debuglog(DEBUG_INFO, "[DRAGDROP] dropped on pid=%u window=%u type=%d size=%u\n",
                 target->pid, target->window_id,
                 (int)g_drag.data_type, g_drag.data_size);
    } else {
        g_state = DRAGDROP_STATE_CANCELLED;
        if (g_callbacks.on_drag_cancel) {
            g_callbacks.on_drag_cancel();
        }
        debuglog(DEBUG_DETAIL, "[DRAGDROP] drop missed all targets, cancelled\n");
    }

    uint32_t src_pid = g_drag.source_pid;
    uint32_t src_win = g_drag.source_window;
    uint32_t tgt_pid = g_drag.target_pid;
    uint32_t tgt_win = g_drag.target_window;

    memset(&g_drag, 0, sizeof(g_drag));
    g_hovered_target = NULL;

    spinlock_release(&g_lock);

    if (g_event_handler) {
        drag_drop_state_t final_state;
        memset(&final_state, 0, sizeof(final_state));
        final_state.source_pid = src_pid;
        final_state.source_window = src_win;
        final_state.target_pid = tgt_pid;
        final_state.target_window = tgt_win;
        final_state.current_x = drop_x;
        final_state.current_y = drop_y;
        g_event_handler(&final_state, g_event_user_data);
    }

    return (target) ? 0 : -2;
}

void dragdrop_cancel(void) {
    spinlock_acquire(&g_lock);

    if (!g_drag.active) {
        spinlock_release(&g_lock);
        return;
    }

    g_state = DRAGDROP_STATE_CANCELLED;
    if (g_hovered_target && g_callbacks.on_drag_leave) {
        g_callbacks.on_drag_leave(g_hovered_target->pid,
                                  g_hovered_target->window_id);
    }
    if (g_callbacks.on_drag_cancel) {
        g_callbacks.on_drag_cancel();
    }

    memset(&g_drag, 0, sizeof(g_drag));
    g_hovered_target = NULL;

    spinlock_release(&g_lock);
    debuglog(DEBUG_DETAIL, "[DRAGDROP] drag cancelled\n");
}

int dragdrop_register_target(const dragdrop_target_t* target) {
    if (!target) {
        return -1;
    }

    spinlock_acquire(&g_lock);

    if (g_target_count >= DRAGDROP_MAX_TARGETS) {
        spinlock_release(&g_lock);
        return -2;
    }

    for (uint32_t i = 0; i < g_target_count; ++i) {
        if (g_targets[i].pid == target->pid &&
            g_targets[i].window_id == target->window_id) {
            g_targets[i] = *target;
            spinlock_release(&g_lock);
            debuglog(DEBUG_DETAIL, "[DRAGDROP] target updated pid=%u window=%u\n",
                     target->pid, target->window_id);
            return 0;
        }
    }

    g_targets[g_target_count] = *target;
    g_target_count++;

    spinlock_release(&g_lock);
    debuglog(DEBUG_DETAIL, "[DRAGDROP] target registered pid=%u window=%u\n",
             target->pid, target->window_id);
    return 0;
}

int dragdrop_unregister_target(uint32_t pid, uint32_t window_id) {
    spinlock_acquire(&g_lock);

    for (uint32_t i = 0; i < g_target_count; ++i) {
        if (g_targets[i].pid == pid && g_targets[i].window_id == window_id) {
            if (g_hovered_target == &g_targets[i]) {
                if (g_callbacks.on_drag_leave) {
                    g_callbacks.on_drag_leave(pid, window_id);
                }
                g_hovered_target = NULL;
            }
            g_targets[i] = g_targets[g_target_count - 1];
            g_target_count--;
            spinlock_release(&g_lock);
            debuglog(DEBUG_DETAIL, "[DRAGDROP] target unregistered pid=%u window=%u\n",
                     pid, window_id);
            return 0;
        }
    }

    spinlock_release(&g_lock);
    return -1;
}

int dragdrop_unregister_all_targets(uint32_t pid) {
    spinlock_acquire(&g_lock);

    uint32_t removed = 0;
    for (uint32_t i = g_target_count; i > 0; --i) {
        uint32_t idx = i - 1;
        if (g_targets[idx].pid == pid) {
            if (g_hovered_target == &g_targets[idx]) {
                if (g_callbacks.on_drag_leave) {
                    g_callbacks.on_drag_leave(g_targets[idx].pid,
                                              g_targets[idx].window_id);
                }
                g_hovered_target = NULL;
            }
            g_targets[idx] = g_targets[g_target_count - 1];
            g_target_count--;
            removed++;
        }
    }

    spinlock_release(&g_lock);
    debuglog(DEBUG_DETAIL, "[DRAGDROP] unregistered %u targets for pid=%u\n",
             removed, pid);
    return (int)removed;
}

void dragdrop_set_data(clipboard_type_t type, const char* data, uint32_t size) {
    spinlock_acquire(&g_lock);

    if (!g_drag.active) {
        spinlock_release(&g_lock);
        return;
    }

    g_drag.data_type = type;
    if (data && size > 0) {
        uint32_t copy_size = (size < DRAGDROP_MAX_DATA_SIZE) ? size : DRAGDROP_MAX_DATA_SIZE;
        memcpy(g_drag.data, data, copy_size);
        g_drag.data_size = copy_size;
    } else {
        g_drag.data_size = 0;
    }

    spinlock_release(&g_lock);
}

void dragdrop_set_action(dragdrop_action_t action) {
    spinlock_acquire(&g_lock);
    g_action = action;
    spinlock_release(&g_lock);
}

bool dragdrop_is_active(void) {
    spinlock_acquire(&g_lock);
    bool active = g_drag.active;
    spinlock_release(&g_lock);
    return active;
}

dragdrop_state_t dragdrop_get_state(void) {
    spinlock_acquire(&g_lock);
    dragdrop_state_t s = g_state;
    spinlock_release(&g_lock);
    return s;
}

const drag_drop_state_t* dragdrop_get_current(void) {
    return &g_drag;
}

dragdrop_action_t dragdrop_get_action(void) {
    spinlock_acquire(&g_lock);
    dragdrop_action_t a = g_action;
    spinlock_release(&g_lock);
    return a;
}

void dragdrop_set_callbacks(const dragdrop_callbacks_t* callbacks) {
    spinlock_acquire(&g_lock);
    if (callbacks) {
        g_callbacks = *callbacks;
    } else {
        memset(&g_callbacks, 0, sizeof(g_callbacks));
    }
    spinlock_release(&g_lock);
}

void dragdrop_set_event_handler(dragdrop_event_fn handler, void* user_data) {
    spinlock_acquire(&g_lock);
    g_event_handler = handler;
    g_event_user_data = user_data;
    spinlock_release(&g_lock);
}

static void dragdrop_parse_kv(const char* cmd, char* key, uint32_t key_size,
                               char* val, uint32_t val_size) {
    key[0] = '\0';
    val[0] = '\0';

    const char* p = cmd;
    while (*p == ' ') p++;

    uint32_t i = 0;
    while (*p && *p != ' ' && i + 1 < key_size) {
        key[i++] = *p++;
    }
    key[i] = '\0';

    while (*p == ' ') p++;

    i = 0;
    while (*p && i + 1 < val_size) {
        val[i++] = *p++;
    }
    val[i] = '\0';
}

int dragdrop_handle_ipc(const char* command, char* reply, uint32_t reply_size) {
    if (!command || !reply || reply_size < 4) {
        return -1;
    }

    char verb[64];
    char value[256];
    dragdrop_parse_kv(command, verb, sizeof(verb), value, sizeof(value));

    if (strcmp(verb, "PING") == 0) {
        snprintf(reply, reply_size, "PONG dragdrop");
        return 0;
    }

    if (strcmp(verb, "REGISTER") == 0) {
        dragdrop_target_t target;
        memset(&target, 0, sizeof(target));
        int32_t x = 0, y = 0;
        uint32_t w = 0, h = 0;
        uint32_t pid = 0, wid = 0;
        uint32_t accept_mask = 0;
        sscanf(value, "%u %u %d %d %u %u %u", &pid, &wid, &x, &y, &w, &h, &accept_mask);
        target.pid = pid;
        target.window_id = wid;
        target.x = x;
        target.y = y;
        target.width = w;
        target.height = h;
        target.accepts_text = (accept_mask & 0x01) != 0;
        target.accepts_image = (accept_mask & 0x02) != 0;
        target.accepts_file = (accept_mask & 0x04) != 0;
        target.accepts_uri = (accept_mask & 0x08) != 0;
        target.accepts_custom = (accept_mask & 0x10) != 0;
        int result = dragdrop_register_target(&target);
        if (result == 0) {
            snprintf(reply, reply_size, "OK");
        } else {
            snprintf(reply, reply_size, "ERR %d", result);
        }
        return result;
    }

    if (strcmp(verb, "UNREGISTER") == 0) {
        uint32_t pid = 0, wid = 0;
        sscanf(value, "%u %u", &pid, &wid);
        int result = dragdrop_unregister_target(pid, wid);
        if (result == 0) {
            snprintf(reply, reply_size, "OK");
        } else {
            snprintf(reply, reply_size, "ERR not-found");
        }
        return result;
    }

    if (strcmp(verb, "START") == 0) {
        uint32_t pid = 0, wid = 0;
        int32_t x = 0, y = 0;
        sscanf(value, "%u %u %d %d", &pid, &wid, &x, &y);
        int result = dragdrop_start(pid, wid, x, y);
        if (result == 0) {
            snprintf(reply, reply_size, "OK");
        } else {
            snprintf(reply, reply_size, "ERR busy");
        }
        return result;
    }

    if (strcmp(verb, "UPDATE") == 0) {
        int32_t x = 0, y = 0;
        sscanf(value, "%d %d", &x, &y);
        dragdrop_update(x, y);
        snprintf(reply, reply_size, "OK");
        return 0;
    }

    if (strcmp(verb, "DROP") == 0) {
        int32_t x = 0, y = 0;
        sscanf(value, "%d %d", &x, &y);
        int result = dragdrop_drop(x, y);
        if (result == 0) {
            snprintf(reply, reply_size, "OK");
        } else {
            snprintf(reply, reply_size, "ERR no-target");
        }
        return result;
    }

    if (strcmp(verb, "CANCEL") == 0) {
        dragdrop_cancel();
        snprintf(reply, reply_size, "OK");
        return 0;
    }

    if (strcmp(verb, "SETDATA") == 0) {
        char type_str[32] = {0};
        uint32_t size = 0;
        char data_buf[DRAGDROP_MAX_DATA_SIZE] = {0};
        sscanf(value, "%31s %u", type_str, &size);
        const char* data_start = value;
        while (*data_start && *data_start != ' ') data_start++;
        while (*data_start == ' ') data_start++;
        while (*data_start && *data_start != ' ') data_start++;
        while (*data_start == ' ') data_start++;
        while (*data_start && *data_start != ' ') data_start++;
        while (*data_start == ' ') data_start++;
        if (size > 0 && size < DRAGDROP_MAX_DATA_SIZE) {
            memcpy(data_buf, data_start, size);
        }
        clipboard_type_t ct = CLIPBOARD_TYPE_NONE;
        if (strcmp(type_str, "text") == 0) ct = CLIPBOARD_TYPE_TEXT;
        else if (strcmp(type_str, "image") == 0) ct = CLIPBOARD_TYPE_IMAGE;
        else if (strcmp(type_str, "file") == 0) ct = CLIPBOARD_TYPE_FILE;
        else if (strcmp(type_str, "uri") == 0) ct = CLIPBOARD_TYPE_URI;
        else if (strcmp(type_str, "custom") == 0) ct = CLIPBOARD_TYPE_CUSTOM;
        dragdrop_set_data(ct, data_buf, size);
        snprintf(reply, reply_size, "OK");
        return 0;
    }

    if (strcmp(verb, "STATUS") == 0) {
        bool active = dragdrop_is_active();
        dragdrop_state_t st = dragdrop_get_state();
        const char* state_str = "idle";
        switch (st) {
            case DRAGDROP_STATE_DRAGGING: state_str = "dragging"; break;
            case DRAGDROP_STATE_OVER_TARGET: state_str = "over-target"; break;
            case DRAGDROP_STATE_DROPPED: state_str = "dropped"; break;
            case DRAGDROP_STATE_CANCELLED: state_str = "cancelled"; break;
            default: break;
        }
        snprintf(reply, reply_size, "OK %s %s", active ? "active" : "inactive", state_str);
        return 0;
    }

    snprintf(reply, reply_size, "ERR unknown");
    return -1;
}

#ifndef DRAGDROP_H
#define DRAGDROP_H

#include "types.h"
#include <stdint.h>
#include <stdbool.h>

#define DRAGDROP_MAX_DATA_SIZE 4096
#define DRAGDROP_MAX_TARGETS 32
#define DRAGDROP_HOVER_THRESHOLD 8

typedef enum {
    CLIPBOARD_TYPE_NONE = 0,
    CLIPBOARD_TYPE_TEXT,
    CLIPBOARD_TYPE_IMAGE,
    CLIPBOARD_TYPE_FILE,
    CLIPBOARD_TYPE_URI,
    CLIPBOARD_TYPE_CUSTOM
} clipboard_type_t;

typedef enum {
    DRAGDROP_STATE_IDLE = 0,
    DRAGDROP_STATE_DRAGGING,
    DRAGDROP_STATE_OVER_TARGET,
    DRAGDROP_STATE_DROPPED,
    DRAGDROP_STATE_CANCELLED
} dragdrop_state_t;

typedef enum {
    DRAGDROP_ACTION_COPY = 0,
    DRAGDROP_ACTION_MOVE,
    DRAGDROP_ACTION_LINK
} dragdrop_action_t;

typedef struct {
    uint32_t source_pid;
    uint32_t target_pid;
    uint32_t source_window;
    uint32_t target_window;
    int32_t start_x, start_y;
    int32_t current_x, current_y;
    bool active;
    clipboard_type_t data_type;
    char data[DRAGDROP_MAX_DATA_SIZE];
    uint32_t data_size;
} drag_drop_state_t;

typedef struct {
    uint32_t pid;
    uint32_t window_id;
    int32_t x, y;
    uint32_t width, height;
    bool accepts_text;
    bool accepts_image;
    bool accepts_file;
    bool accepts_uri;
    bool accepts_custom;
} dragdrop_target_t;

typedef struct {
    void (*on_drag_enter)(uint32_t target_pid, uint32_t target_window);
    void (*on_drag_leave)(uint32_t target_pid, uint32_t target_window);
    void (*on_drop)(uint32_t target_pid, uint32_t target_window,
                    clipboard_type_t type, const char* data, uint32_t data_size);
    void (*on_drag_cancel)(void);
    void (*on_drag_start)(uint32_t source_pid, uint32_t source_window);
} dragdrop_callbacks_t;

typedef void (*dragdrop_event_fn)(const drag_drop_state_t* state, void* user_data);

void dragdrop_init(void);
void dragdrop_shutdown(void);

int dragdrop_start(uint32_t source_pid, uint32_t source_window,
                   int32_t start_x, int32_t start_y);
void dragdrop_update(int32_t x, int32_t y);
int dragdrop_enter_target(uint32_t target_pid, uint32_t target_window);
void dragdrop_leave_target(void);
int dragdrop_drop(int32_t drop_x, int32_t drop_y);
void dragdrop_cancel(void);

int dragdrop_register_target(const dragdrop_target_t* target);
int dragdrop_unregister_target(uint32_t pid, uint32_t window_id);
int dragdrop_unregister_all_targets(uint32_t pid);

void dragdrop_set_data(clipboard_type_t type, const char* data, uint32_t size);
void dragdrop_set_action(dragdrop_action_t action);

bool dragdrop_is_active(void);
dragdrop_state_t dragdrop_get_state(void);
const drag_drop_state_t* dragdrop_get_current(void);
dragdrop_action_t dragdrop_get_action(void);

void dragdrop_set_callbacks(const dragdrop_callbacks_t* callbacks);
void dragdrop_set_event_handler(dragdrop_event_fn handler, void* user_data);

int dragdrop_handle_ipc(const char* command, char* reply, uint32_t reply_size);

#endif /* DRAGDROP_H */

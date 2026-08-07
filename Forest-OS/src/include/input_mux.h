#ifndef INPUT_MUX_H
#define INPUT_MUX_H

#include "types.h"
#include "input_event.h"
#include "input_ring.h"
#include "display_manager.h"
#include <stdbool.h>

/*
 * Input Event Multiplexer
 *
 * Routes input events from device drivers to registered consumers
 * based on priority and focus state.
 *
 * Event flow:
 *   Device IRQ -> Driver -> input_mux_dispatch_event() -> Consumer ring buffers
 *
 * Priority levels:
 *   EXCLUSIVE: Receives all events first (hotkeys, global shortcuts)
 *   FOCUSED:   Receives events when has keyboard/mouse focus
 *   BACKGROUND: Always receives events (loggers, recorders)
 *
 * Focus:
 *   - Keyboard focus: Only one consumer at a time receives key events
 *   - Mouse focus: Only one consumer at a time receives mouse events
 *   - Focus is typically controlled by display mode switching
 */

#define INPUT_MAX_CONSUMERS     16
#define INPUT_KEY_REPEAT_MAX    256
#define INPUT_COALESCE_MAX      32

typedef enum {
    INPUT_PRIORITY_EXCLUSIVE = 0,
    INPUT_PRIORITY_FOCUSED = 1,
    INPUT_PRIORITY_BACKGROUND = 2,
    INPUT_PRIORITY_MAX = 3
} input_priority_t;

typedef bool (*input_filter_callback_t)(const input_event_t* event, void* context);
typedef void (*input_event_callback_t)(const input_event_t* event, void* context);
typedef void (*input_focus_follows_mouse_callback_t)(int32 cursor_x, int32 cursor_y, void* context);

typedef struct input_consumer {
    const char* name;
    uint32 id;

    input_priority_t priority;
    uint32 event_mask;

    input_ring_t* ring;
    input_event_callback_t event_callback;
    input_filter_callback_t filter_callback;
    void* callback_context;

    bool has_keyboard_focus;
    bool has_mouse_focus;

    bool active;
    bool registered;
    bool grabbed;

    uint32 events_received;
    uint32 events_filtered;

    struct input_consumer* next;
} input_consumer_t;

typedef struct {
    bool active_keys[INPUT_KEY_REPEAT_MAX];
    uint32 key_down_time_sec[INPUT_KEY_REPEAT_MAX];
    uint32 key_down_time_usec[INPUT_KEY_REPEAT_MAX];
    uint32 last_repeat_time_sec[INPUT_KEY_REPEAT_MAX];
    uint32 last_repeat_time_usec[INPUT_KEY_REPEAT_MAX];
    uint32 repeat_delay_ms;
    uint32 repeat_rate_ms;
} key_repeat_state_t;

typedef struct {
    bool enabled;
    float sensitivity;
    float acceleration;
    float speed_threshold;
    float max_multiplier;
} mouse_accel_state_t;

typedef struct {
    bool enabled;
    input_focus_follows_mouse_callback_t callback;
    void* context;
    uint32 cooldown_ms;
    uint32 last_switch_time_sec;
    uint32 last_switch_time_usec;
} focus_follows_mouse_state_t;

typedef struct {
    int32 accumulated_x;
    int32 accumulated_y;
    int32 accumulated_wheel;
    uint32 pending_count;
    bool active;
    bool enabled;
} mouse_coalesce_state_t;

typedef struct {
    input_consumer_t* consumers[INPUT_MAX_CONSUMERS];
    uint32 num_consumers;

    input_consumer_t* priority_lists[INPUT_PRIORITY_MAX];

    input_consumer_t* keyboard_focus;
    input_consumer_t* mouse_focus;

    input_consumer_t* grabbed_consumer;

    display_mode_t current_mode;
    input_consumer_t* mode_consumers[4];

    bool event_consumed;

    uint32 total_events_dispatched;
    uint32 total_events_dropped;

    spinlock_t lock;
    bool initialized;

    key_repeat_state_t key_repeat;
    mouse_accel_state_t mouse_accel;
    focus_follows_mouse_state_t ffm;
    mouse_coalesce_state_t coalesce;

    bool debug_mode;
} input_mux_state_t;

bool input_mux_init(void);
void input_mux_shutdown(void);
bool input_mux_is_initialized(void);

bool input_mux_register_consumer(input_consumer_t* consumer);
bool input_mux_unregister_consumer(input_consumer_t* consumer);
input_consumer_t* input_mux_find_consumer(const char* name);
void input_mux_set_consumer_active(input_consumer_t* consumer, bool active);

bool input_mux_grab(input_consumer_t* consumer);
bool input_mux_ungrab(input_consumer_t* consumer);
input_consumer_t* input_mux_get_grabbed(void);

void input_mux_dispatch_event(const input_event_t* event);
void input_mux_consume_event(void);
bool input_mux_event_was_consumed(void);

void input_mux_set_keyboard_focus(input_consumer_t* consumer);
void input_mux_set_mouse_focus(input_consumer_t* consumer);
input_consumer_t* input_mux_get_keyboard_focus(void);
input_consumer_t* input_mux_get_mouse_focus(void);

void input_mux_set_active_mode(display_mode_t mode);
void input_mux_register_mode_consumer(display_mode_t mode, input_consumer_t* consumer);
input_consumer_t* input_mux_get_mode_consumer(display_mode_t mode);

void input_mux_set_key_repeat_delay(uint32 delay_ms);
void input_mux_set_key_repeat_rate(uint32 rate_ms);
void input_mux_enable_key_repeat(bool enable);
void input_mux_generate_repeat_events(void);

void input_mux_set_mouse_acceleration(bool enabled, float sensitivity,
                                       float acceleration, float speed_threshold,
                                       float max_multiplier);

void input_mux_set_focus_follows_mouse(bool enabled,
                                        input_focus_follows_mouse_callback_t callback,
                                        void* context, uint32 cooldown_ms);

void input_mux_enable_coalescing(bool enable);
void input_mux_flush_coalesced(void);

void input_mux_set_debug_mode(bool enable);
bool input_mux_get_debug_mode(void);

void input_mux_get_stats(uint32* total_dispatched, uint32* total_dropped);
void input_mux_clear_stats(void);
void input_mux_debug_print(void);

static inline void input_consumer_init(input_consumer_t* consumer, const char* name,
                                        input_priority_t priority, uint32 event_mask) {
    if (!consumer) return;

    consumer->name = name;
    consumer->id = 0;
    consumer->priority = priority;
    consumer->event_mask = event_mask;
    consumer->ring = NULL;
    consumer->event_callback = NULL;
    consumer->filter_callback = NULL;
    consumer->callback_context = NULL;
    consumer->has_keyboard_focus = false;
    consumer->has_mouse_focus = false;
    consumer->active = true;
    consumer->registered = false;
    consumer->grabbed = false;
    consumer->events_received = 0;
    consumer->events_filtered = 0;
    consumer->next = NULL;
}

#define INPUT_MASK_KEY      (1 << EV_KEY)
#define INPUT_MASK_REL      (1 << EV_REL)
#define INPUT_MASK_ABS      (1 << EV_ABS)
#define INPUT_MASK_SYN      (1 << EV_SYN)
#define INPUT_MASK_ALL      0xFFFFFFFF

#define INPUT_MASK_KEYBOARD (INPUT_MASK_KEY | INPUT_MASK_SYN)
#define INPUT_MASK_MOUSE    (INPUT_MASK_REL | INPUT_MASK_KEY | INPUT_MASK_SYN)
#define INPUT_MASK_ALL_INPUT (INPUT_MASK_KEYBOARD | INPUT_MASK_MOUSE)

#endif /* INPUT_MUX_H */

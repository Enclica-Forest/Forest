#include "../include/input_mux.h"
#include "../include/string.h"
#include "../include/debuglog.h"

static input_mux_state_t g_input_mux = {0};
static uint32 g_next_consumer_id = 1;

static void add_to_priority_list(input_consumer_t* consumer) {
    if (!consumer || consumer->priority >= INPUT_PRIORITY_MAX) return;

    consumer->next = g_input_mux.priority_lists[consumer->priority];
    g_input_mux.priority_lists[consumer->priority] = consumer;
}

static void remove_from_priority_list(input_consumer_t* consumer) {
    if (!consumer || consumer->priority >= INPUT_PRIORITY_MAX) return;

    input_consumer_t** pp = &g_input_mux.priority_lists[consumer->priority];
    while (*pp) {
        if (*pp == consumer) {
            *pp = consumer->next;
            consumer->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static bool event_matches_mask(const input_event_t* event, uint32 mask) {
    if (event->type >= 32) return false;
    return (mask & (1 << event->type)) != 0;
}

static bool is_keyboard_key_event(const input_event_t* event) {
    return event->type == EV_KEY && !IS_MOUSE_BUTTON(event->code);
}

static bool is_mouse_event(const input_event_t* event) {
    return event->type == EV_REL ||
           (event->type == EV_KEY && IS_MOUSE_BUTTON(event->code));
}

static bool is_mouse_move_event(const input_event_t* event) {
    return event->type == EV_REL &&
           (event->code == REL_X || event->code == REL_Y);
}

static bool is_mouse_wheel_event(const input_event_t* event) {
    return event->type == EV_REL &&
           (event->code == REL_WHEEL || event->code == REL_HWHEEL);
}

static void log_event(const char* prefix, const input_event_t* event) {
    if (!g_input_mux.debug_mode) return;
    debuglog(DEBUG_DETAIL, "[InputMux] %s type=0x%x code=0x%x val=%d\n",
             prefix, event->type, event->code, event->value);
}

static void dispatch_raw_event(const input_event_t* event);

static void apply_mouse_acceleration(input_event_t* event) {
    if (!g_input_mux.mouse_accel.enabled) return;
    if (!is_mouse_move_event(event)) return;

    mouse_accel_state_t* accel = &g_input_mux.mouse_accel;
    float delta = (float)event->value;
    float abs_delta = delta < 0 ? -delta : delta;
    float speed = abs_delta;

    float multiplier = 1.0f;
    if (speed > accel->speed_threshold) {
        float excess = speed - accel->speed_threshold;
        multiplier = 1.0f + (accel->acceleration * excess * accel->sensitivity);
        if (multiplier > accel->max_multiplier) {
            multiplier = accel->max_multiplier;
        }
    } else {
        multiplier = accel->sensitivity;
    }

    event->value = (int32)(delta * multiplier);
}

static void deliver_event_to_consumer(input_consumer_t* consumer, const input_event_t* event) {
    if (!consumer || !event || !consumer->active) return;

    if (!event_matches_mask(event, consumer->event_mask)) return;

    if (consumer->filter_callback) {
        bool consumed = consumer->filter_callback(event, consumer->callback_context);
        if (consumed) {
            consumer->events_filtered++;
            g_input_mux.event_consumed = true;
            return;
        }
    }

    if (consumer->event_callback) {
        consumer->event_callback(event, consumer->callback_context);
    }

    if (consumer->ring) {
        if (!input_ring_push(consumer->ring, event)) {
            g_input_mux.total_events_dropped++;
        }
    }

    consumer->events_received++;
}

static void coalesce_event(const input_event_t* event) {
    mouse_coalesce_state_t* mc = &g_input_mux.coalesce;

    if (event->code == REL_X) {
        mc->accumulated_x += event->value;
    } else if (event->code == REL_Y) {
        mc->accumulated_y += event->value;
    } else if (event->code == REL_WHEEL || event->code == REL_HWHEEL) {
        mc->accumulated_wheel += event->value;
    }
    mc->pending_count++;
    mc->active = true;
}

static void flush_coalesced_events(void) {
    mouse_coalesce_state_t* mc = &g_input_mux.coalesce;

    if (!mc->active || mc->pending_count == 0) return;

    uint32 saved_coalesce = mc->enabled;
    mc->enabled = false;

    if (mc->accumulated_x != 0 || mc->accumulated_y != 0) {
        input_event_t rel_event;
        rel_event.tv_sec = 0;
        rel_event.tv_usec = 0;
        rel_event.type = EV_REL;

        if (mc->accumulated_x != 0) {
            rel_event.code = REL_X;
            rel_event.value = mc->accumulated_x;
            dispatch_raw_event(&rel_event);
        }
        if (mc->accumulated_y != 0) {
            rel_event.code = REL_Y;
            rel_event.value = mc->accumulated_y;
            dispatch_raw_event(&rel_event);
        }
    }

    if (mc->accumulated_wheel != 0) {
        input_event_t wheel_event;
        wheel_event.tv_sec = 0;
        wheel_event.tv_usec = 0;
        wheel_event.type = EV_REL;
        wheel_event.code = REL_WHEEL;
        wheel_event.value = mc->accumulated_wheel;
        dispatch_raw_event(&wheel_event);
    }

    mc->accumulated_x = 0;
    mc->accumulated_y = 0;
    mc->accumulated_wheel = 0;
    mc->pending_count = 0;
    mc->active = false;
    mc->enabled = saved_coalesce;
}

static uint32 time_diff_ms(uint32 sec1, uint32 usec1, uint32 sec2, uint32 usec2) {
    int64 diff_sec = (int64)sec2 - (int64)sec1;
    int64 diff_usec = (int64)usec2 - (int64)usec1;
    int64 total_ms = diff_sec * 1000 + diff_usec / 1000;
    return total_ms < 0 ? 0 : (uint32)total_ms;
}

static void check_key_repeat(void) {
    key_repeat_state_t* kr = &g_input_mux.key_repeat;
    if (!kr->repeat_delay_ms || !kr->repeat_rate_ms) return;

    for (uint32 i = 0; i < INPUT_KEY_REPEAT_MAX; i++) {
        if (!kr->active_keys[i]) continue;

        uint32 elapsed = time_diff_ms(
            kr->key_down_time_sec[i], kr->key_down_time_usec[i],
            kr->last_repeat_time_sec[i], kr->last_repeat_time_usec[i]
        );

        if (elapsed >= kr->repeat_rate_ms) {
            input_event_t repeat_event;
            repeat_event.tv_sec = kr->last_repeat_time_sec[i];
            repeat_event.tv_usec = kr->last_repeat_time_usec[i];
            repeat_event.type = EV_KEY;
            repeat_event.code = (uint16)i;
            repeat_event.value = KEY_REPEAT;

            log_event("repeat", &repeat_event);

            input_consumer_t* consumer = g_input_mux.keyboard_focus;
            if (consumer) {
                deliver_event_to_consumer(consumer, &repeat_event);
            }

            kr->last_repeat_time_sec[i] = kr->key_down_time_sec[i];
            kr->last_repeat_time_usec[i] = kr->key_down_time_usec[i] +
                (kr->repeat_rate_ms * 1000);

            while (kr->last_repeat_time_usec[i] >= 1000000) {
                kr->last_repeat_time_sec[i]++;
                kr->last_repeat_time_usec[i] -= 1000000;
            }
        }
    }
}

static void handle_key_repeat_down(const input_event_t* event) {
    key_repeat_state_t* kr = &g_input_mux.key_repeat;
    if (event->code >= INPUT_KEY_REPEAT_MAX) return;

    if (event->value == KEY_PRESS) {
        kr->active_keys[event->code] = true;
        kr->key_down_time_sec[event->code] = event->tv_sec;
        kr->key_down_time_usec[event->code] = event->tv_usec;
        kr->last_repeat_time_sec[event->code] = event->tv_sec;
        kr->last_repeat_time_usec[event->code] = event->tv_usec;
    } else if (event->value == KEY_RELEASE) {
        kr->active_keys[event->code] = false;
    }
}

static void handle_focus_follows_mouse(const input_event_t* event) {
    focus_follows_mouse_state_t* ffm = &g_input_mux.ffm;
    if (!ffm->enabled || !ffm->callback) return;
    if (event->type != EV_REL) return;

    static int32 cursor_x = 0;
    static int32 cursor_y = 0;

    if (event->code == REL_X) {
        cursor_x += event->value;
    } else if (event->code == REL_Y) {
        cursor_y += event->value;
    } else {
        return;
    }

    if (ffm->cooldown_ms > 0) {
        uint32 elapsed = time_diff_ms(
            ffm->last_switch_time_sec, ffm->last_switch_time_usec,
            event->tv_sec, event->tv_usec
        );
        if (elapsed < ffm->cooldown_ms) return;
    }

    ffm->callback(cursor_x, cursor_y, ffm->context);
    ffm->last_switch_time_sec = event->tv_sec;
    ffm->last_switch_time_usec = event->tv_usec;
}

static void dispatch_raw_event(const input_event_t* event) {
    g_input_mux.event_consumed = false;
    g_input_mux.total_events_dispatched++;

    input_event_t dispatch_event = *event;

    if (is_mouse_move_event(&dispatch_event)) {
        apply_mouse_acceleration(&dispatch_event);
    }

    if (g_input_mux.grabbed_consumer) {
        deliver_event_to_consumer(g_input_mux.grabbed_consumer, &dispatch_event);
        return;
    }

    input_consumer_t* consumer = g_input_mux.priority_lists[INPUT_PRIORITY_EXCLUSIVE];
    while (consumer) {
        deliver_event_to_consumer(consumer, &dispatch_event);
        consumer = consumer->next;
    }

    if (g_input_mux.event_consumed) return;

    bool kbd_event = is_keyboard_key_event(&dispatch_event);
    bool mse_event = is_mouse_event(&dispatch_event);

    if (kbd_event && g_input_mux.keyboard_focus) {
        deliver_event_to_consumer(g_input_mux.keyboard_focus, &dispatch_event);
    }

    if (mse_event && g_input_mux.mouse_focus) {
        deliver_event_to_consumer(g_input_mux.mouse_focus, &dispatch_event);
    }

    if (dispatch_event.type == EV_SYN) {
        if (g_input_mux.keyboard_focus) {
            deliver_event_to_consumer(g_input_mux.keyboard_focus, &dispatch_event);
        }
        if (g_input_mux.mouse_focus && g_input_mux.mouse_focus != g_input_mux.keyboard_focus) {
            deliver_event_to_consumer(g_input_mux.mouse_focus, &dispatch_event);
        }
    }

    consumer = g_input_mux.priority_lists[INPUT_PRIORITY_BACKGROUND];
    while (consumer) {
        deliver_event_to_consumer(consumer, &dispatch_event);
        consumer = consumer->next;
    }
}

/*
 * Initialization
 */

bool input_mux_init(void) {
    if (g_input_mux.initialized) return true;

    memset(&g_input_mux, 0, sizeof(g_input_mux));

    spinlock_init(&g_input_mux.lock, "input_mux");

    g_input_mux.current_mode = DISPLAY_MODE_TTY_CONSOLE;

    g_input_mux.key_repeat.repeat_delay_ms = 500;
    g_input_mux.key_repeat.repeat_rate_ms = 33;

    g_input_mux.mouse_accel.enabled = false;
    g_input_mux.mouse_accel.sensitivity = 1.0f;
    g_input_mux.mouse_accel.acceleration = 0.1f;
    g_input_mux.mouse_accel.speed_threshold = 5.0f;
    g_input_mux.mouse_accel.max_multiplier = 4.0f;

    g_input_mux.ffm.enabled = false;
    g_input_mux.ffm.callback = NULL;
    g_input_mux.ffm.context = NULL;
    g_input_mux.ffm.cooldown_ms = 100;

    g_input_mux.coalesce.enabled = false;

    g_input_mux.debug_mode = false;

    g_input_mux.initialized = true;

    debuglog(DEBUG_INFO, "[InputMux] Initialized\n");
    return true;
}

void input_mux_shutdown(void) {
    if (!g_input_mux.initialized) return;

    spinlock_acquire(&g_input_mux.lock);

    for (uint32 i = 0; i < g_input_mux.num_consumers; i++) {
        if (g_input_mux.consumers[i]) {
            g_input_mux.consumers[i]->registered = false;
            g_input_mux.consumers[i]->grabbed = false;
        }
        g_input_mux.consumers[i] = NULL;
    }

    g_input_mux.num_consumers = 0;
    g_input_mux.keyboard_focus = NULL;
    g_input_mux.mouse_focus = NULL;
    g_input_mux.grabbed_consumer = NULL;
    g_input_mux.initialized = false;

    memset(&g_input_mux.key_repeat, 0, sizeof(key_repeat_state_t));
    memset(&g_input_mux.coalesce, 0, sizeof(mouse_coalesce_state_t));

    spinlock_release(&g_input_mux.lock);

    debuglog(DEBUG_INFO, "[InputMux] Shutdown\n");
}

bool input_mux_is_initialized(void) {
    return g_input_mux.initialized;
}

/*
 * Consumer registration
 */

bool input_mux_register_consumer(input_consumer_t* consumer) {
    if (!consumer || !g_input_mux.initialized) return false;

    if (consumer->registered) {
        debuglog(DEBUG_WARN, "[InputMux] Consumer '%s' already registered\n",
                 consumer->name ? consumer->name : "unnamed");
        return false;
    }

    spinlock_acquire(&g_input_mux.lock);

    if (g_input_mux.num_consumers >= INPUT_MAX_CONSUMERS) {
        spinlock_release(&g_input_mux.lock);
        debuglog(DEBUG_ERROR, "[InputMux] Max consumers reached\n");
        return false;
    }

    consumer->id = g_next_consumer_id++;

    g_input_mux.consumers[g_input_mux.num_consumers++] = consumer;

    add_to_priority_list(consumer);

    consumer->registered = true;
    consumer->active = true;

    spinlock_release(&g_input_mux.lock);

    debuglog(DEBUG_INFO, "[InputMux] Registered consumer '%s' (id=%u, priority=%u)\n",
             consumer->name ? consumer->name : "unnamed",
             consumer->id, consumer->priority);

    return true;
}

bool input_mux_unregister_consumer(input_consumer_t* consumer) {
    if (!consumer || !g_input_mux.initialized || !consumer->registered) return false;

    spinlock_acquire(&g_input_mux.lock);

    remove_from_priority_list(consumer);

    for (uint32 i = 0; i < g_input_mux.num_consumers; i++) {
        if (g_input_mux.consumers[i] == consumer) {
            for (uint32 j = i; j < g_input_mux.num_consumers - 1; j++) {
                g_input_mux.consumers[j] = g_input_mux.consumers[j + 1];
            }
            g_input_mux.consumers[--g_input_mux.num_consumers] = NULL;
            break;
        }
    }

    if (g_input_mux.keyboard_focus == consumer) {
        g_input_mux.keyboard_focus = NULL;
    }
    if (g_input_mux.mouse_focus == consumer) {
        g_input_mux.mouse_focus = NULL;
    }
    if (g_input_mux.grabbed_consumer == consumer) {
        g_input_mux.grabbed_consumer = NULL;
    }

    for (int i = 0; i < 4; i++) {
        if (g_input_mux.mode_consumers[i] == consumer) {
            g_input_mux.mode_consumers[i] = NULL;
        }
    }

    consumer->registered = false;
    consumer->grabbed = false;

    spinlock_release(&g_input_mux.lock);

    debuglog(DEBUG_INFO, "[InputMux] Unregistered consumer '%s'\n",
             consumer->name ? consumer->name : "unnamed");

    return true;
}

input_consumer_t* input_mux_find_consumer(const char* name) {
    if (!name || !g_input_mux.initialized) return NULL;

    for (uint32 i = 0; i < g_input_mux.num_consumers; i++) {
        if (g_input_mux.consumers[i] &&
            g_input_mux.consumers[i]->name &&
            strcmp(g_input_mux.consumers[i]->name, name) == 0) {
            return g_input_mux.consumers[i];
        }
    }

    return NULL;
}

void input_mux_set_consumer_active(input_consumer_t* consumer, bool active) {
    if (!consumer || !consumer->registered) return;
    consumer->active = active;
}

/*
 * Grab / exclusive mode
 */

bool input_mux_grab(input_consumer_t* consumer) {
    if (!consumer || !consumer->registered) return false;

    spinlock_acquire(&g_input_mux.lock);

    if (g_input_mux.grabbed_consumer) {
        spinlock_release(&g_input_mux.lock);
        debuglog(DEBUG_WARN, "[InputMux] Already grabbed by '%s'\n",
                 g_input_mux.grabbed_consumer->name ? g_input_mux.grabbed_consumer->name : "unnamed");
        return false;
    }

    g_input_mux.grabbed_consumer = consumer;
    consumer->grabbed = true;

    spinlock_release(&g_input_mux.lock);

    debuglog(DEBUG_INFO, "[InputMux] Consumer '%s' grabbed input\n",
             consumer->name ? consumer->name : "unnamed");

    return true;
}

bool input_mux_ungrab(input_consumer_t* consumer) {
    if (!consumer || !consumer->registered) return false;

    spinlock_acquire(&g_input_mux.lock);

    if (g_input_mux.grabbed_consumer != consumer) {
        spinlock_release(&g_input_mux.lock);
        return false;
    }

    g_input_mux.grabbed_consumer = NULL;
    consumer->grabbed = false;

    spinlock_release(&g_input_mux.lock);

    debuglog(DEBUG_INFO, "[InputMux] Consumer '%s' released grab\n",
             consumer->name ? consumer->name : "unnamed");

    return true;
}

input_consumer_t* input_mux_get_grabbed(void) {
    return g_input_mux.grabbed_consumer;
}

/*
 * Key repeat
 */

void input_mux_set_key_repeat_delay(uint32 delay_ms) {
    g_input_mux.key_repeat.repeat_delay_ms = delay_ms;
}

void input_mux_set_key_repeat_rate(uint32 rate_ms) {
    g_input_mux.key_repeat.repeat_rate_ms = rate_ms;
}

void input_mux_enable_key_repeat(bool enable) {
    if (!enable) {
        memset(&g_input_mux.key_repeat.active_keys, 0,
               sizeof(g_input_mux.key_repeat.active_keys));
    }
}

void input_mux_generate_repeat_events(void) {
    check_key_repeat();
}

/*
 * Mouse acceleration
 */

void input_mux_set_mouse_acceleration(bool enabled, float sensitivity,
                                       float acceleration, float speed_threshold,
                                       float max_multiplier) {
    g_input_mux.mouse_accel.enabled = enabled;
    g_input_mux.mouse_accel.sensitivity = sensitivity;
    g_input_mux.mouse_accel.acceleration = acceleration;
    g_input_mux.mouse_accel.speed_threshold = speed_threshold;
    g_input_mux.mouse_accel.max_multiplier = max_multiplier;

    debuglog(DEBUG_INFO, "[InputMux] Mouse accel: %s sens=%.2f acc=%.2f thresh=%.2f max=%.2f\n",
             enabled ? "on" : "off", sensitivity, acceleration, speed_threshold, max_multiplier);
}

/*
 * Focus-follows-mouse
 */

void input_mux_set_focus_follows_mouse(bool enabled,
                                        input_focus_follows_mouse_callback_t callback,
                                        void* context, uint32 cooldown_ms) {
    g_input_mux.ffm.enabled = enabled;
    g_input_mux.ffm.callback = callback;
    g_input_mux.ffm.context = context;
    g_input_mux.ffm.cooldown_ms = cooldown_ms;

    debuglog(DEBUG_INFO, "[InputMux] Focus-follows-mouse: %s cooldown=%u ms\n",
             enabled ? "on" : "off", cooldown_ms);
}

/*
 * Event coalescing
 */

void input_mux_enable_coalescing(bool enable) {
    if (!enable && g_input_mux.coalesce.active) {
        flush_coalesced_events();
    }
    g_input_mux.coalesce.enabled = enable;

    debuglog(DEBUG_INFO, "[InputMux] Coalescing: %s\n", enable ? "on" : "off");
}

void input_mux_flush_coalesced(void) {
    flush_coalesced_events();
}

/*
 * Debug mode
 */

void input_mux_set_debug_mode(bool enable) {
    g_input_mux.debug_mode = enable;
    debuglog(DEBUG_INFO, "[InputMux] Debug mode: %s\n", enable ? "on" : "off");
}

bool input_mux_get_debug_mode(void) {
    return g_input_mux.debug_mode;
}

/*
 * Event dispatching
 */

void input_mux_dispatch_event(const input_event_t* event) {
    if (!event || !g_input_mux.initialized) return;

    input_event_t event_copy = *event;

    log_event("dispatch", &event_copy);

    if (g_input_mux.key_repeat.repeat_delay_ms > 0 &&
        g_input_mux.key_repeat.repeat_rate_ms > 0) {
        handle_key_repeat_down(&event_copy);
    }

    if (g_input_mux.ffm.enabled && is_mouse_event(&event_copy)) {
        handle_focus_follows_mouse(&event_copy);
    }

    if (g_input_mux.coalesce.enabled &&
        (is_mouse_move_event(&event_copy) || is_mouse_wheel_event(&event_copy))) {
        coalesce_event(&event_copy);
        if (g_input_mux.coalesce.pending_count < INPUT_COALESCE_MAX) {
            return;
        }
        flush_coalesced_events();
    }

    if (g_input_mux.coalesce.enabled && event_copy.type == EV_SYN) {
        flush_coalesced_events();
    }

    dispatch_raw_event(&event_copy);
}

void input_mux_consume_event(void) {
    g_input_mux.event_consumed = true;
}

bool input_mux_event_was_consumed(void) {
    return g_input_mux.event_consumed;
}

/*
 * Focus management
 */

void input_mux_set_keyboard_focus(input_consumer_t* consumer) {
    if (consumer && !consumer->registered) {
        debuglog(DEBUG_WARN, "[InputMux] Cannot set focus to unregistered consumer\n");
        return;
    }

    input_consumer_t* old_focus = g_input_mux.keyboard_focus;

    if (old_focus) {
        old_focus->has_keyboard_focus = false;
    }

    g_input_mux.keyboard_focus = consumer;

    if (consumer) {
        consumer->has_keyboard_focus = true;
        debuglog(DEBUG_INFO, "[InputMux] Keyboard focus -> '%s'\n",
                 consumer->name ? consumer->name : "unnamed");
    } else {
        debuglog(DEBUG_INFO, "[InputMux] Keyboard focus cleared\n");
    }
}

void input_mux_set_mouse_focus(input_consumer_t* consumer) {
    if (consumer && !consumer->registered) {
        debuglog(DEBUG_WARN, "[InputMux] Cannot set mouse focus to unregistered consumer\n");
        return;
    }

    input_consumer_t* old_focus = g_input_mux.mouse_focus;

    if (old_focus) {
        old_focus->has_mouse_focus = false;
    }

    g_input_mux.mouse_focus = consumer;

    if (consumer) {
        consumer->has_mouse_focus = true;
        debuglog(DEBUG_INFO, "[InputMux] Mouse focus -> '%s'\n",
                 consumer->name ? consumer->name : "unnamed");
    } else {
        debuglog(DEBUG_INFO, "[InputMux] Mouse focus cleared\n");
    }
}

input_consumer_t* input_mux_get_keyboard_focus(void) {
    return g_input_mux.keyboard_focus;
}

input_consumer_t* input_mux_get_mouse_focus(void) {
    return g_input_mux.mouse_focus;
}

/*
 * Display mode integration
 */

void input_mux_set_active_mode(display_mode_t mode) {
    g_input_mux.current_mode = mode;

    input_consumer_t* mode_consumer = NULL;
    if ((int)mode < 4) {
        mode_consumer = g_input_mux.mode_consumers[mode];
    }

    if (mode_consumer) {
        input_mux_set_keyboard_focus(mode_consumer);
        input_mux_set_mouse_focus(mode_consumer);
        debuglog(DEBUG_INFO, "[InputMux] Mode %d activated, focus -> '%s'\n",
                 mode, mode_consumer->name ? mode_consumer->name : "unnamed");
    } else {
        debuglog(DEBUG_INFO, "[InputMux] Mode %d activated, no consumer registered\n", mode);
    }
}

void input_mux_register_mode_consumer(display_mode_t mode, input_consumer_t* consumer) {
    if ((int)mode >= 4) {
        debuglog(DEBUG_ERROR, "[InputMux] Invalid mode %d for consumer registration\n", mode);
        return;
    }

    g_input_mux.mode_consumers[mode] = consumer;

    if (consumer) {
        debuglog(DEBUG_INFO, "[InputMux] Registered '%s' for mode %d\n",
                 consumer->name ? consumer->name : "unnamed", mode);
    }
}

input_consumer_t* input_mux_get_mode_consumer(display_mode_t mode) {
    if ((int)mode >= 4) return NULL;
    return g_input_mux.mode_consumers[mode];
}

/*
 * Utility functions
 */

void input_mux_get_stats(uint32* total_dispatched, uint32* total_dropped) {
    if (total_dispatched) *total_dispatched = g_input_mux.total_events_dispatched;
    if (total_dropped) *total_dropped = g_input_mux.total_events_dropped;
}

void input_mux_clear_stats(void) {
    g_input_mux.total_events_dispatched = 0;
    g_input_mux.total_events_dropped = 0;

    for (uint32 i = 0; i < g_input_mux.num_consumers; i++) {
        if (g_input_mux.consumers[i]) {
            g_input_mux.consumers[i]->events_received = 0;
            g_input_mux.consumers[i]->events_filtered = 0;
        }
    }
}

void input_mux_debug_print(void) {
    debuglog(DEBUG_INFO, "[InputMux] === State ===\n");
    debuglog(DEBUG_INFO, "  Consumers: %u/%u\n", g_input_mux.num_consumers, INPUT_MAX_CONSUMERS);
    debuglog(DEBUG_INFO, "  Events: dispatched=%u dropped=%u\n",
             g_input_mux.total_events_dispatched, g_input_mux.total_events_dropped);
    debuglog(DEBUG_INFO, "  Grabbed: %s\n",
             g_input_mux.grabbed_consumer ?
             (g_input_mux.grabbed_consumer->name ? g_input_mux.grabbed_consumer->name : "unnamed") :
             "none");
    debuglog(DEBUG_INFO, "  Keyboard focus: %s\n",
             g_input_mux.keyboard_focus ?
             (g_input_mux.keyboard_focus->name ? g_input_mux.keyboard_focus->name : "unnamed") :
             "none");
    debuglog(DEBUG_INFO, "  Mouse focus: %s\n",
             g_input_mux.mouse_focus ?
             (g_input_mux.mouse_focus->name ? g_input_mux.mouse_focus->name : "unnamed") :
             "none");
    debuglog(DEBUG_INFO, "  Current mode: %d\n", g_input_mux.current_mode);
    debuglog(DEBUG_INFO, "  Key repeat: delay=%u ms rate=%u ms\n",
             g_input_mux.key_repeat.repeat_delay_ms,
             g_input_mux.key_repeat.repeat_rate_ms);
    debuglog(DEBUG_INFO, "  Mouse accel: %s sens=%.2f acc=%.2f\n",
             g_input_mux.mouse_accel.enabled ? "on" : "off",
             g_input_mux.mouse_accel.sensitivity,
             g_input_mux.mouse_accel.acceleration);
    debuglog(DEBUG_INFO, "  Focus-follows-mouse: %s cooldown=%u ms\n",
             g_input_mux.ffm.enabled ? "on" : "off",
             g_input_mux.ffm.cooldown_ms);
    debuglog(DEBUG_INFO, "  Coalescing: %s pending=%u\n",
             g_input_mux.coalesce.enabled ? "on" : "off",
             g_input_mux.coalesce.pending_count);
    debuglog(DEBUG_INFO, "  Debug mode: %s\n",
             g_input_mux.debug_mode ? "on" : "off");

    debuglog(DEBUG_INFO, "  Registered consumers:\n");
    for (uint32 i = 0; i < g_input_mux.num_consumers; i++) {
        input_consumer_t* c = g_input_mux.consumers[i];
        if (c) {
            debuglog(DEBUG_INFO, "    [%u] '%s' priority=%u active=%d grabbed=%d mask=0x%x recv=%u filt=%u\n",
                     c->id, c->name ? c->name : "unnamed",
                     c->priority, c->active, c->grabbed, c->event_mask,
                     c->events_received, c->events_filtered);
        }
    }
}

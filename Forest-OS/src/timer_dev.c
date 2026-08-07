/**
 * Timer Device Filesystem Interface for Fern
 *
 * Exposes system timers through /dev/ interface following Unix conventions.
 * Creates device nodes: /dev/timer, /dev/rtc, /dev/hpet, /dev/pit
 */

#include "include/timer_dev.h"
#include "include/devfs.h"
#include "include/vfs.h"
#include "include/interrupt.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/spinlock.h"

/* Debug tag */
#define TIMER_DEV_TAG "[TIMER_DEV] "

/* Global device states */
static timer_dev_state_t g_timer_state = { .lock = SPINLOCK_UNLOCKED };
static timer_dev_state_t g_rtc_state = { .lock = SPINLOCK_UNLOCKED };
static timer_dev_state_t g_hpet_state = { .lock = SPINLOCK_UNLOCKED };
static timer_dev_state_t g_pit_state = { .lock = SPINLOCK_UNLOCKED };

/* Forward declarations for device operations */
static uint32_t timer_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static uint32_t timer_dev_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static void timer_dev_open(vfs_node_t *node, uint32_t flags);
static void timer_dev_close(vfs_node_t *node);
static int timer_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg);
static int timer_dev_poll(vfs_node_t *node, uint32_t events);

static uint32_t rtc_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static int rtc_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg);

static uint32_t hpet_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static int hpet_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg);

static uint32_t pit_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static int pit_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg);

/* ============================================================================
 * Timer Event Ring Buffer Operations
 * ============================================================================ */

/**
 * Initialize a timer event ring buffer
 */
void timer_event_ring_init(timer_event_ring_t *ring, const char *name)
{
    if (!ring) return;

    memset(ring->events, 0, sizeof(ring->events));
    ring->head = 0;
    ring->tail = 0;
    spinlock_init(&ring->lock, name);
    ring->wait_queue = NULL;
    ring->events_written = 0;
    ring->events_read = 0;
    ring->overflows = 0;
    ring->name = name;
}

/**
 * Push an event to the ring buffer (IRQ-safe)
 * Returns true if successful, false if buffer full
 */
bool timer_event_ring_push(timer_event_ring_t *ring, const timer_event_t *event)
{
    if (!ring || !event) return false;

    uint32_t next_head = (ring->head + 1) & TIMER_EVENT_RING_MASK;

    if (next_head == ring->tail) {
        /* Buffer full */
        ring->overflows++;
        return false;
    }

    ring->events[ring->head] = *event;
    __sync_synchronize();  /* Memory barrier */
    ring->head = next_head;
    ring->events_written++;

    return true;
}

/**
 * Pop an event from the ring buffer
 * Returns true if an event was available, false if buffer empty
 */
bool timer_event_ring_pop(timer_event_ring_t *ring, timer_event_t *event)
{
    if (!ring || !event) return false;

    if (ring->head == ring->tail) {
        /* Buffer empty */
        return false;
    }

    *event = ring->events[ring->tail];
    __sync_synchronize();  /* Memory barrier */
    ring->tail = (ring->tail + 1) & TIMER_EVENT_RING_MASK;
    ring->events_read++;

    return true;
}

/**
 * Check if ring buffer is empty
 */
bool timer_event_ring_is_empty(const timer_event_ring_t *ring)
{
    if (!ring) return true;
    return ring->head == ring->tail;
}

/**
 * Check if ring buffer is full
 */
bool timer_event_ring_is_full(const timer_event_ring_t *ring)
{
    if (!ring) return true;
    return ((ring->head + 1) & TIMER_EVENT_RING_MASK) == ring->tail;
}

/**
 * Get number of events in ring buffer
 */
uint32_t timer_event_ring_count(const timer_event_ring_t *ring)
{
    if (!ring) return 0;
    return (ring->head - ring->tail) & TIMER_EVENT_RING_MASK;
}

/* ============================================================================
 * Generic /dev/timer Device Operations
 * ============================================================================ */

/**
 * Read from /dev/timer
 * - If size == 8: returns current time in nanoseconds
 * - If size >= sizeof(timer_event_t): returns pending timer events
 */
static uint32_t timer_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    (void)offset;

    if (!buffer || size == 0) {
        return 0;
    }

    timer_dev_state_t *state = (timer_dev_state_t *)node->internal_data;
    if (!state) {
        state = &g_timer_state;
    }

    /* Reading 8 bytes returns current time in nanoseconds */
    if (size == sizeof(uint64_t)) {
        uint64_t time_ns = get_system_time_ns();
        memcpy(buffer, &time_ns, sizeof(uint64_t));
        return sizeof(uint64_t);
    }

    /* Reading multiple of event size returns pending events */
    if (size >= sizeof(timer_event_t)) {
        uint32_t bytes_read = 0;
        timer_event_t event;

        bool irq_state = irq_save_and_disable_safe();

        while (bytes_read + sizeof(timer_event_t) <= size) {
            if (!timer_event_ring_pop(&state->event_ring, &event)) {
                break;  /* No more events */
            }

            memcpy(buffer + bytes_read, &event, sizeof(timer_event_t));
            bytes_read += sizeof(timer_event_t);
        }

        irq_restore_safe(irq_state);
        return bytes_read;
    }

    return 0;
}

/**
 * Write to /dev/timer
 * Writing 8 bytes sets the periodic timer interval in nanoseconds
 * Writing 0 disables the periodic timer
 */
static uint32_t timer_dev_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    (void)offset;

    if (!buffer || size != sizeof(uint64_t)) {
        return 0;
    }

    timer_dev_state_t *state = (timer_dev_state_t *)node->internal_data;
    if (!state) {
        state = &g_timer_state;
    }

    uint64_t period_ns;
    memcpy(&period_ns, buffer, sizeof(uint64_t));

    spinlock_acquire(&state->lock);

    if (period_ns == 0) {
        /* Disable periodic timer */
        state->periodic_enabled = false;
        state->periodic_interval_ns = 0;
    } else {
        /* Set periodic timer */
        state->periodic_interval_ns = period_ns;
        state->periodic_enabled = true;

        /* Configure underlying timer hardware */
        timer_set_periodic(period_ns);
    }

    spinlock_release(&state->lock);

    debuglog(DEBUG_INFO, TIMER_DEV_TAG "Periodic timer set to %lu ns\n",
             (unsigned long)period_ns);

    return sizeof(uint64_t);
}

/**
 * Open /dev/timer
 */
static void timer_dev_open(vfs_node_t *node, uint32_t flags)
{
    (void)flags;

    timer_dev_state_t *state = (timer_dev_state_t *)node->internal_data;
    if (!state) return;

    spinlock_acquire(&state->lock);
    state->open_count++;
    spinlock_release(&state->lock);
}

/**
 * Close /dev/timer
 */
static void timer_dev_close(vfs_node_t *node)
{
    timer_dev_state_t *state = (timer_dev_state_t *)node->internal_data;
    if (!state) return;

    spinlock_acquire(&state->lock);
    if (state->open_count > 0) {
        state->open_count--;
    }
    spinlock_release(&state->lock);
}

/**
 * IOCTL handler for /dev/timer
 */
static int timer_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg)
{
    timer_dev_state_t *state = (timer_dev_state_t *)node->internal_data;
    if (!state) {
        state = &g_timer_state;
    }

    switch (request) {
        case TIMER_GETINFO: {
            if (!arg) return IOCTL_EINVAL;
            timer_info_t *info = (timer_info_t *)arg;

            struct timer_source *primary = get_primary_timer_source();
            if (!primary) {
                memset(info, 0, sizeof(timer_info_t));
                strncpy(info->name, "none", sizeof(info->name) - 1);
                info->available = false;
                return IOCTL_SUCCESS;
            }

            strncpy(info->name, primary->name ? primary->name : "unknown",
                    sizeof(info->name) - 1);
            info->name[sizeof(info->name) - 1] = '\0';
            info->frequency = primary->frequency;
            info->resolution_ns = primary->frequency > 0 ?
                                  1000000000ULL / primary->frequency : 0;
            info->num_timers = 1;
            info->capabilities = 0;

            if (primary->supports_periodic)
                info->capabilities |= TIMER_CAP_PERIODIC;
            if (primary->supports_oneshot)
                info->capabilities |= TIMER_CAP_ONESHOT;
            if (primary->high_precision)
                info->capabilities |= TIMER_CAP_HIGH_RES;
            if (primary->per_cpu)
                info->capabilities |= TIMER_CAP_PER_CPU;

            info->max_period_ns = 1000000000ULL;  /* 1 second */
            info->min_period_ns = info->resolution_ns;
            info->available = true;
            info->enabled = (primary->state == 2);  /* TIMER_STATE_ACTIVE */

            return IOCTL_SUCCESS;
        }

        case TIMER_GETTIME: {
            if (!arg) return IOCTL_EINVAL;
            *(uint64_t *)arg = get_system_time_ns();
            return IOCTL_SUCCESS;
        }

        case TIMER_SETPERIOD: {
            if (!arg) return IOCTL_EINVAL;
            uint64_t period_ns = *(uint64_t *)arg;

            spinlock_acquire(&state->lock);
            if (period_ns == 0) {
                state->periodic_enabled = false;
                state->periodic_interval_ns = 0;
            } else {
                state->periodic_interval_ns = period_ns;
                state->periodic_enabled = true;
                timer_set_periodic(period_ns);
            }
            spinlock_release(&state->lock);
            return IOCTL_SUCCESS;
        }

        case TIMER_SETALARM: {
            if (!arg) return IOCTL_EINVAL;
            timer_alarm_t *alarm = (timer_alarm_t *)arg;

            uint64_t expire_time = alarm->expire_ns;
            if (!(alarm->flags & TIMER_ALARM_ABSOLUTE)) {
                /* Convert relative to absolute */
                expire_time += get_system_time_ns();
            }

            spinlock_acquire(&state->lock);
            state->alarm_time_ns = expire_time;
            state->alarm_enabled = true;
            spinlock_release(&state->lock);

            /* Set one-shot timer for alarm */
            uint64_t current = get_system_time_ns();
            if (expire_time > current) {
                timer_set_oneshot(expire_time - current);
            }

            return IOCTL_SUCCESS;
        }

        case TIMER_CANCELALARM:
            spinlock_acquire(&state->lock);
            state->alarm_enabled = false;
            state->alarm_time_ns = 0;
            spinlock_release(&state->lock);
            return IOCTL_SUCCESS;

        case TIMER_ENABLE:
            /* Enable timer interrupts */
            return IOCTL_SUCCESS;

        case TIMER_DISABLE:
            /* Disable timer interrupts */
            return IOCTL_SUCCESS;

        case TIMER_GETRESOLUTION: {
            if (!arg) return IOCTL_EINVAL;
            struct timer_source *primary = get_primary_timer_source();
            if (primary && primary->frequency > 0) {
                *(uint64_t *)arg = 1000000000ULL / primary->frequency;
            } else {
                *(uint64_t *)arg = 1000000ULL;  /* 1ms default */
            }
            return IOCTL_SUCCESS;
        }

        case TIMER_SETONESHOT: {
            if (!arg) return IOCTL_EINVAL;
            uint64_t timeout_ns = *(uint64_t *)arg;
            timer_set_oneshot(timeout_ns);
            return IOCTL_SUCCESS;
        }

        case TIMER_GETOVERRUN: {
            if (!arg) return IOCTL_EINVAL;
            *(uint64_t *)arg = state->event_ring.overflows;
            return IOCTL_SUCCESS;
        }

        case TIMER_GETSOURCES: {
            if (!arg) return IOCTL_EINVAL;
            uint32_t sources = 0;
            if (pit_is_available()) sources |= (1 << 0);
            if (rtc_is_available()) sources |= (1 << 1);
            if (hpet_is_available()) sources |= (1 << 2);
            if (apic_is_available()) sources |= (1 << 3);
            *(uint32_t *)arg = sources;
            return IOCTL_SUCCESS;
        }

        default:
            return IOCTL_ENOTTY;
    }
}

/**
 * Poll for timer events
 */
static int timer_dev_poll(vfs_node_t *node, uint32_t events)
{
    timer_dev_state_t *state = (timer_dev_state_t *)node->internal_data;
    if (!state) {
        state = &g_timer_state;
    }

    int revents = 0;

    if (events & POLLIN) {
        /* Check if timer events are available */
        if (!timer_event_ring_is_empty(&state->event_ring)) {
            revents |= POLLIN;
        }
    }

    if (events & POLLPRI) {
        /* Check if alarm fired */
        if (state->alarm_enabled &&
            get_system_time_ns() >= state->alarm_time_ns) {
            revents |= POLLPRI;
        }
    }

    /* Timer always ready for writing */
    if (events & POLLOUT) {
        revents |= POLLOUT;
    }

    return revents;
}

/* ============================================================================
 * RTC Device Operations (/dev/rtc)
 * ============================================================================ */

/**
 * Read from /dev/rtc
 * Returns either timestamp (8 bytes) or rtc_time structure
 */
static uint32_t rtc_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    (void)node;
    (void)offset;

    if (!buffer || size == 0) {
        return 0;
    }

    if (!rtc_is_available()) {
        return 0;
    }

    /* Reading 8 bytes returns system time in nanoseconds */
    if (size == sizeof(uint64_t)) {
        uint64_t time_ns = get_system_time_ns();
        memcpy(buffer, &time_ns, sizeof(uint64_t));
        return sizeof(uint64_t);
    }

    /* Reading sizeof(rtc_time) returns structured time */
    if (size >= sizeof(struct rtc_time)) {
        struct rtc_stats stats;
        rtc_get_stats(&stats);
        memcpy(buffer, &stats.current_time, sizeof(struct rtc_time));
        return sizeof(struct rtc_time);
    }

    return 0;
}

/**
 * IOCTL handler for /dev/rtc (Linux RTC interface compatible)
 */
static int rtc_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg)
{
    (void)node;

    if (!rtc_is_available()) {
        return IOCTL_ENODEV;
    }

    switch (request) {
        case RTC_RD_TIME: {
            if (!arg) return IOCTL_EINVAL;
            struct rtc_stats stats;
            rtc_get_stats(&stats);
            memcpy(arg, &stats.current_time, sizeof(struct rtc_time));
            return IOCTL_SUCCESS;
        }

        case RTC_AIE_ON:
            /* Alarm interrupt enable - would need rtc_enable_alarm_interrupt() */
            return IOCTL_SUCCESS;

        case RTC_AIE_OFF:
            /* Alarm interrupt disable */
            return IOCTL_SUCCESS;

        case RTC_UIE_ON:
            rtc_enable_update_interrupts();
            return IOCTL_SUCCESS;

        case RTC_UIE_OFF:
            rtc_disable_update_interrupts();
            return IOCTL_SUCCESS;

        case RTC_PIE_ON:
            rtc_enable_periodic_interrupts(0x06);  /* 1024 Hz default */
            return IOCTL_SUCCESS;

        case RTC_PIE_OFF:
            rtc_disable_periodic_interrupts();
            return IOCTL_SUCCESS;

        case RTC_IRQP_READ: {
            if (!arg) return IOCTL_EINVAL;
            struct rtc_stats stats;
            rtc_get_stats(&stats);
            *(uint32_t *)arg = stats.frequency;
            return IOCTL_SUCCESS;
        }

        case RTC_IRQP_SET: {
            if (!arg) return IOCTL_EINVAL;
            uint32_t freq = *(uint32_t *)arg;
            /* Convert frequency to rate register value */
            uint8_t rate = 0x06;  /* Default 1024 Hz */
            if (freq >= 8192) rate = 0x03;
            else if (freq >= 4096) rate = 0x04;
            else if (freq >= 2048) rate = 0x05;
            else if (freq >= 1024) rate = 0x06;
            else if (freq >= 512) rate = 0x07;
            else if (freq >= 256) rate = 0x08;
            else if (freq >= 128) rate = 0x09;
            else if (freq >= 64) rate = 0x0A;
            else if (freq >= 32) rate = 0x0B;
            else if (freq >= 16) rate = 0x0C;
            else if (freq >= 8) rate = 0x0D;
            else if (freq >= 4) rate = 0x0E;
            else rate = 0x0F;  /* 2 Hz */
            rtc_enable_periodic_interrupts(rate);
            return IOCTL_SUCCESS;
        }

        default:
            return IOCTL_ENOTTY;
    }
}

/* ============================================================================
 * HPET Device Operations (/dev/hpet)
 * ============================================================================ */

/**
 * Read from /dev/hpet
 * Returns main counter value in nanoseconds
 */
static uint32_t hpet_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    (void)node;
    (void)offset;

    if (!buffer || size < sizeof(uint64_t)) {
        return 0;
    }

    if (!hpet_is_available()) {
        return 0;
    }

    uint64_t time_ns = hpet_get_time_ns();
    memcpy(buffer, &time_ns, sizeof(uint64_t));

    return sizeof(uint64_t);
}

/**
 * IOCTL handler for /dev/hpet
 */
static int hpet_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg)
{
    (void)node;

    if (!hpet_is_available()) {
        return IOCTL_ENODEV;
    }

    switch (request) {
        case HPET_INFO: {
            if (!arg) return IOCTL_EINVAL;
            hpet_info_dev_t *info = (hpet_info_dev_t *)arg;

            struct hpet_stats stats;
            hpet_get_stats(&stats);

            info->frequency = stats.frequency;
            info->num_timers = stats.num_timers;
            info->num_timers_available = 0;
            for (uint32_t i = 0; i < stats.num_timers && i < 8; i++) {
                if (!stats.timers[i].in_use) {
                    info->num_timers_available++;
                }
            }
            info->supports_64bit = stats.supports_64bit;
            info->supports_legacy = stats.supports_legacy_replacement;

            return IOCTL_SUCCESS;
        }

        case HPET_GETTIME: {
            if (!arg) return IOCTL_EINVAL;
            *(uint64_t *)arg = hpet_get_time_ns();
            return IOCTL_SUCCESS;
        }

        case HPET_EPI:
            /* Enable periodic interrupts - would need hpet_configure_periodic() */
            return IOCTL_SUCCESS;

        case HPET_DPI:
            /* Disable periodic interrupts */
            return IOCTL_SUCCESS;

        case HPET_IRQFREQ:
            /* Set IRQ frequency */
            return IOCTL_SUCCESS;

        default:
            return IOCTL_ENOTTY;
    }
}

/* ============================================================================
 * PIT Device Operations (/dev/pit)
 * ============================================================================ */

/**
 * Read from /dev/pit
 * Returns system time in nanoseconds
 */
static uint32_t pit_dev_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer)
{
    (void)node;
    (void)offset;

    if (!buffer || size < sizeof(uint64_t)) {
        return 0;
    }

    if (!pit_is_available()) {
        return 0;
    }

    uint64_t time_ns = pit_get_time_ns();
    memcpy(buffer, &time_ns, sizeof(uint64_t));

    return sizeof(uint64_t);
}

/**
 * IOCTL handler for /dev/pit
 */
static int pit_dev_ioctl(vfs_node_t *node, uint32_t request, void *arg)
{
    (void)node;

    if (!pit_is_available()) {
        return IOCTL_ENODEV;
    }

    switch (request) {
        case PIT_INFO: {
            if (!arg) return IOCTL_EINVAL;
            pit_info_dev_t *info = (pit_info_dev_t *)arg;

            struct pit_stats stats;
            pit_get_stats(&stats);

            info->base_frequency = 1193182;
            info->current_frequency = stats.current_frequency;
            info->current_divisor = stats.current_divisor;
            info->channel0_active = stats.channel0_in_use;
            info->channel2_active = stats.channel2_in_use;

            return IOCTL_SUCCESS;
        }

        case PIT_SETFREQ: {
            if (!arg) return IOCTL_EINVAL;
            /* PIT frequency setting would need pit_set_frequency() */
            return IOCTL_SUCCESS;
        }

        case PIT_GETFREQ: {
            if (!arg) return IOCTL_EINVAL;
            *(uint32_t *)arg = pit_get_frequency();
            return IOCTL_SUCCESS;
        }

        case PIT_ENABLE:
            pit_enable_system_timer();
            return IOCTL_SUCCESS;

        case PIT_DISABLE:
            pit_disable_system_timer();
            return IOCTL_SUCCESS;

        default:
            return IOCTL_ENOTTY;
    }
}

/* ============================================================================
 * Timer Event Notification (called from IRQ handlers)
 * ============================================================================ */

/**
 * Queue a timer event (called from timer interrupt handler)
 */
void timer_dev_queue_event(uint32_t event_type, uint32_t timer_id)
{
    if (!g_timer_state.initialized) {
        return;
    }

    timer_event_t event;
    event.timestamp_ns = get_system_time_ns();
    event.event_type = event_type;
    event.timer_id = timer_id;
    event.missed_events = 0;

    timer_event_ring_push(&g_timer_state.event_ring, &event);
}

/**
 * Notify timer device of a tick (convenience function)
 */
void timer_dev_notify_tick(void)
{
    timer_dev_queue_event(TIMER_EVENT_TICK, 0);
}

/* ============================================================================
 * Device Registration and Initialization
 * ============================================================================ */

/**
 * Initialize all timer devices
 */
bool timer_dev_init(void)
{
    debuglog(DEBUG_INFO, TIMER_DEV_TAG "Initializing timer devices\n");

    if (!devfs_is_initialized()) {
        debuglog(DEBUG_ERROR, TIMER_DEV_TAG "devfs not initialized\n");
        return false;
    }

    /* Initialize event rings */
    timer_event_ring_init(&g_timer_state.event_ring, "timer_events");
    timer_event_ring_init(&g_rtc_state.event_ring, "rtc_events");
    timer_event_ring_init(&g_hpet_state.event_ring, "hpet_events");
    timer_event_ring_init(&g_pit_state.event_ring, "pit_events");

    /* Device operations for /dev/timer */
    static dev_ops_t timer_ops = {
        .read = timer_dev_read,
        .write = timer_dev_write,
        .open = timer_dev_open,
        .close = timer_dev_close,
        .ioctl = timer_dev_ioctl,
        .poll = timer_dev_poll
    };

    /* Register /dev/timer - primary timer abstraction */
    if (!devfs_register_device("timer", DEV_TYPE_CHAR,
                                DEV_MAJOR_TIMER, DEV_MINOR_TIMER,
                                &timer_ops, &g_timer_state)) {
        debuglog(DEBUG_ERROR, TIMER_DEV_TAG "Failed to register /dev/timer\n");
        return false;
    }
    debuglog(DEBUG_INFO, TIMER_DEV_TAG "Registered /dev/timer\n");

    /* Register /dev/rtc if RTC is available */
    if (rtc_is_available()) {
        static dev_ops_t rtc_ops = {
            .read = rtc_dev_read,
            .write = NULL,
            .open = timer_dev_open,
            .close = timer_dev_close,
            .ioctl = rtc_dev_ioctl,
            .poll = timer_dev_poll
        };

        devfs_register_device("rtc", DEV_TYPE_CHAR,
                              DEV_MAJOR_RTC, DEV_MINOR_RTC,
                              &rtc_ops, &g_rtc_state);
        devfs_register_device("rtc0", DEV_TYPE_CHAR,
                              DEV_MAJOR_RTC, DEV_MINOR_RTC,
                              &rtc_ops, &g_rtc_state);

        debuglog(DEBUG_INFO, TIMER_DEV_TAG "Registered /dev/rtc, /dev/rtc0\n");
    }

    /* Register /dev/hpet if HPET is available */
    if (hpet_is_available()) {
        static dev_ops_t hpet_ops = {
            .read = hpet_dev_read,
            .write = NULL,
            .open = timer_dev_open,
            .close = timer_dev_close,
            .ioctl = hpet_dev_ioctl,
            .poll = timer_dev_poll
        };

        devfs_register_device("hpet", DEV_TYPE_CHAR,
                              DEV_MAJOR_HPET, DEV_MINOR_HPET,
                              &hpet_ops, &g_hpet_state);

        debuglog(DEBUG_INFO, TIMER_DEV_TAG "Registered /dev/hpet\n");
    }

    /* Register /dev/pit if PIT is available */
    if (pit_is_available()) {
        static dev_ops_t pit_ops = {
            .read = pit_dev_read,
            .write = NULL,
            .open = timer_dev_open,
            .close = timer_dev_close,
            .ioctl = pit_dev_ioctl,
            .poll = timer_dev_poll
        };

        devfs_register_device("pit", DEV_TYPE_CHAR,
                              DEV_MAJOR_TIMER, DEV_MINOR_PIT,
                              &pit_ops, &g_pit_state);

        debuglog(DEBUG_INFO, TIMER_DEV_TAG "Registered /dev/pit\n");
    }

    g_timer_state.initialized = true;
    g_rtc_state.initialized = true;
    g_hpet_state.initialized = true;
    g_pit_state.initialized = true;

    debuglog(DEBUG_INFO, TIMER_DEV_TAG "Timer devices initialized successfully\n");

    return true;
}

/**
 * Shutdown timer devices
 */
void timer_dev_shutdown(void)
{
    debuglog(DEBUG_INFO, TIMER_DEV_TAG "Shutting down timer devices\n");

    devfs_unregister_device("timer");
    devfs_unregister_device("rtc");
    devfs_unregister_device("rtc0");
    devfs_unregister_device("hpet");
    devfs_unregister_device("pit");

    g_timer_state.initialized = false;
    g_rtc_state.initialized = false;
    g_hpet_state.initialized = false;
    g_pit_state.initialized = false;
}

/**
 * Timer Device Filesystem Interface for Fern
 *
 * Provides Unix-style /dev/ interface for accessing system timers.
 * Exposes PIT, HPET, APIC Timer, and RTC through VFS device nodes.
 */

#ifndef TIMER_DEV_H
#define TIMER_DEV_H

#include "types.h"
#include "ioctl.h"
#include "spinlock.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Timer Device Major Numbers
 * Following Linux conventions where possible
 */
#define DEV_MAJOR_TIMER         10      /* /dev/timer, /dev/pit (misc devices) */
#define DEV_MAJOR_RTC           253     /* /dev/rtc */
#define DEV_MAJOR_HPET          254     /* /dev/hpet */

/* Timer device minor numbers */
#define DEV_MINOR_TIMER         0       /* /dev/timer - primary abstraction */
#define DEV_MINOR_TIMER0        1       /* /dev/timer0 */
#define DEV_MINOR_PIT           16      /* /dev/pit */
#define DEV_MINOR_RTC           0       /* /dev/rtc or /dev/rtc0 */
#define DEV_MINOR_HPET          0       /* /dev/hpet */

/*
 * Timer event structure for read() operations
 * Returns when a timer expires or alarm fires
 */
typedef struct timer_event {
    uint64_t timestamp_ns;      /* Event timestamp in nanoseconds since boot */
    uint32_t event_type;        /* TIMER_EVENT_* type */
    uint32_t timer_id;          /* Which timer generated the event */
    uint64_t missed_events;     /* Count of missed events (overruns) */
} __attribute__((packed)) timer_event_t;

/* Timer event types */
#define TIMER_EVENT_TICK        0x01    /* Periodic timer tick */
#define TIMER_EVENT_ALARM       0x02    /* Alarm expired */
#define TIMER_EVENT_ONESHOT     0x03    /* One-shot timer expired */
#define TIMER_EVENT_OVERFLOW    0x04    /* Counter overflow */

/*
 * Timer information structure (for TIMER_GETINFO ioctl)
 */
typedef struct timer_info {
    char name[32];              /* Timer name (e.g., "HPET", "PIT", "APIC") */
    uint64_t frequency;         /* Timer frequency in Hz */
    uint64_t resolution_ns;     /* Timer resolution in nanoseconds */
    uint32_t num_timers;        /* Number of available timer channels */
    uint32_t capabilities;      /* TIMER_CAP_* flags */
    uint64_t max_period_ns;     /* Maximum supported period */
    uint64_t min_period_ns;     /* Minimum supported period */
    bool available;             /* Timer hardware available */
    bool enabled;               /* Timer currently enabled */
} timer_info_t;

/* Timer capability flags */
#define TIMER_CAP_PERIODIC      (1 << 0)    /* Supports periodic mode */
#define TIMER_CAP_ONESHOT       (1 << 1)    /* Supports one-shot mode */
#define TIMER_CAP_64BIT         (1 << 2)    /* 64-bit counter */
#define TIMER_CAP_HIGH_RES      (1 << 3)    /* High resolution (< 1us) */
#define TIMER_CAP_PER_CPU       (1 << 4)    /* Per-CPU timer */
#define TIMER_CAP_ALARM         (1 << 5)    /* Supports alarms */
#define TIMER_CAP_MMAP          (1 << 6)    /* Supports mmap access */

/*
 * Timer alarm structure (for setting alarms)
 */
typedef struct timer_alarm {
    uint64_t expire_ns;         /* Expiration time (absolute or relative) */
    uint32_t flags;             /* TIMER_ALARM_* flags */
    uint32_t callback_id;       /* User-specified callback identifier */
} timer_alarm_t;

/* Timer alarm flags */
#define TIMER_ALARM_ABSOLUTE    (1 << 0)    /* expire_ns is absolute time */
#define TIMER_ALARM_RELATIVE    (0)         /* expire_ns is relative (default) */
#define TIMER_ALARM_WAKEUP      (1 << 1)    /* Wake from sleep */
#define TIMER_ALARM_REPEAT      (1 << 2)    /* Auto-repeat alarm */

/*
 * Timer ioctl commands
 * Type: 't' for generic timer
 */

/* Generic timer ioctls (for /dev/timer) */
#define TIMER_GETINFO       _IOR('t', 0x01, timer_info_t)     /* Get timer info */
#define TIMER_GETTIME       _IOR('t', 0x02, uint64_t)         /* Get current time (ns) */
#define TIMER_SETPERIOD     _IOW('t', 0x03, uint64_t)         /* Set periodic interval (ns) */
#define TIMER_SETALARM      _IOW('t', 0x04, timer_alarm_t)    /* Set alarm */
#define TIMER_CANCELALARM   _IO('t', 0x05)                    /* Cancel pending alarm */
#define TIMER_ENABLE        _IO('t', 0x06)                    /* Enable timer */
#define TIMER_DISABLE       _IO('t', 0x07)                    /* Disable timer */
#define TIMER_GETRESOLUTION _IOR('t', 0x08, uint64_t)         /* Get resolution (ns) */
#define TIMER_SETONESHOT    _IOW('t', 0x09, uint64_t)         /* Set one-shot timeout */
#define TIMER_GETOVERRUN    _IOR('t', 0x0A, uint64_t)         /* Get overrun count */
#define TIMER_SELECT        _IOW('t', 0x0B, int)              /* Select timer source */
#define TIMER_GETSOURCES    _IOR('t', 0x0C, uint32_t)         /* Get available sources bitmask */

/*
 * RTC ioctl commands (Linux-compatible)
 * Type: 'r'
 */
#define RTC_RD_TIME         _IOR('r', 0x09, struct rtc_time)  /* Read RTC time */
#define RTC_SET_TIME        _IOW('r', 0x0A, struct rtc_time)  /* Set RTC time */
#define RTC_ALM_READ        _IOR('r', 0x10, struct rtc_time)  /* Read alarm */
#define RTC_ALM_SET         _IOW('r', 0x11, struct rtc_time)  /* Set alarm */
#define RTC_RD_EPOCH        _IOR('r', 0x0D, uint32_t)         /* Read epoch */
#define RTC_SET_EPOCH       _IOW('r', 0x0E, uint32_t)         /* Set epoch */
#define RTC_AIE_ON          _IO('r', 0x01)                    /* Enable alarm interrupt */
#define RTC_AIE_OFF         _IO('r', 0x02)                    /* Disable alarm interrupt */
#define RTC_UIE_ON          _IO('r', 0x03)                    /* Enable update interrupt */
#define RTC_UIE_OFF         _IO('r', 0x04)                    /* Disable update interrupt */
#define RTC_PIE_ON          _IO('r', 0x05)                    /* Enable periodic interrupt */
#define RTC_PIE_OFF         _IO('r', 0x06)                    /* Disable periodic interrupt */
#define RTC_IRQP_READ       _IOR('r', 0x0B, uint32_t)         /* Read IRQ rate */
#define RTC_IRQP_SET        _IOW('r', 0x0C, uint32_t)         /* Set IRQ rate */

/*
 * HPET ioctl commands
 * Type: 'h'
 */
typedef struct hpet_info_dev {
    uint64_t frequency;             /* Counter frequency */
    uint32_t num_timers;            /* Number of timers */
    uint32_t num_timers_available;  /* Timers not in use */
    bool supports_64bit;
    bool supports_legacy;
} hpet_info_dev_t;

#define HPET_INFO           _IOR('h', 0x01, hpet_info_dev_t)  /* Get HPET info */
#define HPET_GETTIME        _IOR('h', 0x02, uint64_t)         /* Read main counter */
#define HPET_EPI            _IO('h', 0x03)                    /* Enable periodic */
#define HPET_DPI            _IO('h', 0x04)                    /* Disable periodic */
#define HPET_IRQFREQ        _IOW('h', 0x05, uint64_t)         /* Set IRQ frequency */

/*
 * PIT ioctl commands
 * Type: 'p'
 */
typedef struct pit_info_dev {
    uint32_t base_frequency;    /* 1193182 Hz */
    uint32_t current_frequency;
    uint16_t current_divisor;
    bool channel0_active;
    bool channel2_active;
} pit_info_dev_t;

#define PIT_INFO            _IOR('p', 0x01, pit_info_dev_t)   /* Get PIT info */
#define PIT_SETFREQ         _IOW('p', 0x02, uint32_t)         /* Set frequency */
#define PIT_GETFREQ         _IOR('p', 0x03, uint32_t)         /* Get frequency */
#define PIT_ENABLE          _IO('p', 0x04)                    /* Enable channel 0 */
#define PIT_DISABLE         _IO('p', 0x05)                    /* Disable channel 0 */

/*
 * Poll event flags for timer devices
 */
#define TIMER_POLL_DATA     POLLIN      /* Timer event available */
#define TIMER_POLL_ALARM    POLLPRI     /* Alarm fired */

/*
 * Timer event ring buffer
 * Lock-free circular buffer for queuing timer events from IRQ context
 */
#define TIMER_EVENT_RING_SIZE   64
#define TIMER_EVENT_RING_MASK   (TIMER_EVENT_RING_SIZE - 1)

typedef struct timer_event_ring {
    timer_event_t events[TIMER_EVENT_RING_SIZE];
    volatile uint32_t head;     /* Write position (modified by producer/IRQ) */
    volatile uint32_t tail;     /* Read position (modified by consumer) */
    spinlock_t lock;            /* For multi-consumer safety */
    void *wait_queue;           /* For blocking reads (future use) */
    uint32_t events_written;    /* Total events written (stats) */
    uint32_t events_read;       /* Total events read (stats) */
    uint32_t overflows;         /* Dropped events due to full buffer */
    const char *name;           /* Ring buffer name for debugging */
} timer_event_ring_t;

/*
 * Timer device state
 */
typedef struct timer_dev_state {
    bool initialized;
    timer_event_ring_t event_ring;
    uint64_t alarm_time_ns;
    bool alarm_enabled;
    bool periodic_enabled;
    uint64_t periodic_interval_ns;
    uint32_t open_count;
    spinlock_t lock;
} timer_dev_state_t;

/*
 * Function declarations
 */

/* Initialization */
bool timer_dev_init(void);
void timer_dev_shutdown(void);

/* Event ring buffer operations */
void timer_event_ring_init(timer_event_ring_t *ring, const char *name);
bool timer_event_ring_push(timer_event_ring_t *ring, const timer_event_t *event);
bool timer_event_ring_pop(timer_event_ring_t *ring, timer_event_t *event);
bool timer_event_ring_is_empty(const timer_event_ring_t *ring);
bool timer_event_ring_is_full(const timer_event_ring_t *ring);
uint32_t timer_event_ring_count(const timer_event_ring_t *ring);

/* Timer event notification (called from IRQ handlers) */
void timer_dev_queue_event(uint32_t event_type, uint32_t timer_id);
void timer_dev_notify_tick(void);

#endif /* TIMER_DEV_H */

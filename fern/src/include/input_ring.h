#ifndef INPUT_RING_H
#define INPUT_RING_H

#include "types.h"
#include "input_event.h"
#include "spinlock.h"
#include <stdbool.h>

/*
 * Input Ring Buffer
 *
 * A lock-free (for single producer) circular buffer for input events.
 * Used by device drivers to queue events for consumers.
 *
 * Thread safety:
 * - Single producer (IRQ handler) can push without lock
 * - Multiple consumers need lock for pop operations
 * - Statistics are best-effort, not atomic
 */

/* Ring buffer size - must be power of 2 for efficient masking */
#define INPUT_RING_SIZE         256
#define INPUT_RING_MASK         (INPUT_RING_SIZE - 1)

/* Ring buffer structure */
typedef struct input_ring {
    /* Event storage */
    input_event_t events[INPUT_RING_SIZE];

    /* Head: next write position (producer) */
    volatile uint32 head;

    /* Tail: next read position (consumer) */
    volatile uint32 tail;

    /* Lock for multi-consumer access */
    spinlock_t lock;

    /* Wait queue for blocking reads (task list pointer) */
    void* wait_queue;

    /* Statistics */
    uint32 events_written;      /* Total events pushed */
    uint32 events_read;         /* Total events popped */
    uint32 overflows;           /* Events dropped due to full buffer */

    /* Debug info */
    const char* name;           /* Ring buffer name for debugging */
} input_ring_t;

/*
 * Initialization
 */

/* Initialize a ring buffer */
void input_ring_init(input_ring_t* ring, const char* name);

/* Reset a ring buffer (clear all events) */
void input_ring_reset(input_ring_t* ring);

/*
 * Producer operations (typically called from IRQ handler)
 */

/* Push an event onto the ring buffer
 * Returns true if successful, false if buffer full (event dropped)
 * This is safe to call from IRQ context */
bool input_ring_push(input_ring_t* ring, const input_event_t* event);

/* Push multiple events atomically
 * Returns number of events actually pushed */
uint32 input_ring_push_batch(input_ring_t* ring, const input_event_t* events, uint32 count);

/*
 * Consumer operations (typically called from process context)
 */

/* Pop an event from the ring buffer
 * Returns true if an event was retrieved, false if buffer empty */
bool input_ring_pop(input_ring_t* ring, input_event_t* event);

/* Peek at the next event without removing it
 * Returns true if an event is available, false if buffer empty */
bool input_ring_peek(input_ring_t* ring, input_event_t* event);

/* Pop multiple events
 * Returns number of events actually popped */
uint32 input_ring_pop_batch(input_ring_t* ring, input_event_t* events, uint32 max_count);

/* Discard events without reading them
 * Returns number of events discarded */
uint32 input_ring_discard(input_ring_t* ring, uint32 count);

/*
 * Status queries
 */

/* Check if the ring buffer is empty */
static inline bool input_ring_is_empty(const input_ring_t* ring) {
    return ring->head == ring->tail;
}

/* Check if the ring buffer is full */
static inline bool input_ring_is_full(const input_ring_t* ring) {
    return ((ring->head + 1) & INPUT_RING_MASK) == ring->tail;
}

/* Get number of events currently in the buffer */
static inline uint32 input_ring_count(const input_ring_t* ring) {
    return (ring->head - ring->tail) & INPUT_RING_MASK;
}

/* Get number of free slots in the buffer */
static inline uint32 input_ring_space(const input_ring_t* ring) {
    return INPUT_RING_SIZE - 1 - input_ring_count(ring);
}

/*
 * Blocking support (for process context only)
 */

/* Wait for data to be available
 * Puts current task to sleep until data arrives
 * Only call from process context, not IRQ */
void input_ring_wait_for_data(input_ring_t* ring);

/* Wake up all tasks waiting on this ring buffer
 * Safe to call from IRQ context */
void input_ring_wake_waiters(input_ring_t* ring);

/* Set the wait queue for this ring buffer */
void input_ring_set_wait_queue(input_ring_t* ring, void* wait_queue);

/*
 * Debug and statistics
 */

/* Get overflow count (events dropped due to full buffer) */
static inline uint32 input_ring_get_overflows(const input_ring_t* ring) {
    return ring->overflows;
}

/* Clear statistics */
void input_ring_clear_stats(input_ring_t* ring);

/* Print ring buffer status (for debugging) */
void input_ring_debug_print(const input_ring_t* ring);

/*
 * Static initialization macro
 */
#define INPUT_RING_INIT(ring_name) { \
    .events = {0}, \
    .head = 0, \
    .tail = 0, \
    .lock = SPINLOCK_INIT(ring_name "_lock"), \
    .wait_queue = NULL, \
    .events_written = 0, \
    .events_read = 0, \
    .overflows = 0, \
    .name = ring_name \
}

#define DEFINE_INPUT_RING(var_name, ring_name) \
    static input_ring_t var_name = INPUT_RING_INIT(ring_name)

#endif /* INPUT_RING_H */

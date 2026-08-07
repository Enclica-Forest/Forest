#include "../include/input_ring.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/task.h"

/*
 * Input Ring Buffer Implementation
 *
 * This implements a lock-free single-producer, multi-consumer ring buffer
 * for input events. The producer (IRQ handler) doesn't need locks, while
 * consumers use spinlocks for synchronization.
 */

/* Initialize a ring buffer */
void input_ring_init(input_ring_t* ring, const char* name) {
    if (!ring) return;

    memset(ring->events, 0, sizeof(ring->events));
    ring->head = 0;
    ring->tail = 0;
    spinlock_init(&ring->lock, name ? name : "input_ring");
    ring->wait_queue = NULL;
    ring->events_written = 0;
    ring->events_read = 0;
    ring->overflows = 0;
    ring->name = name;
}

/* Reset a ring buffer (clear all events) */
void input_ring_reset(input_ring_t* ring) {
    if (!ring) return;

    spinlock_acquire(&ring->lock);
    ring->head = 0;
    ring->tail = 0;
    spinlock_release(&ring->lock);
}

/* Push an event onto the ring buffer (producer - typically IRQ handler) */
bool input_ring_push(input_ring_t* ring, const input_event_t* event) {
    if (!ring || !event) return false;

    uint32 next_head = (ring->head + 1) & INPUT_RING_MASK;

    /* Check if buffer is full */
    if (next_head == ring->tail) {
        ring->overflows++;
        return false;
    }

    /* Copy event to buffer */
    ring->events[ring->head] = *event;

    /* Memory barrier to ensure event is written before head is updated */
    __asm__ volatile("" ::: "memory");

    /* Update head pointer */
    ring->head = next_head;
    ring->events_written++;

    /* Wake up any waiting consumers */
    input_ring_wake_waiters(ring);

    return true;
}

/* Push multiple events atomically */
uint32 input_ring_push_batch(input_ring_t* ring, const input_event_t* events, uint32 count) {
    if (!ring || !events || count == 0) return 0;

    uint32 pushed = 0;
    for (uint32 i = 0; i < count; i++) {
        if (!input_ring_push(ring, &events[i])) {
            break;
        }
        pushed++;
    }

    return pushed;
}

/* Pop an event from the ring buffer (consumer) */
bool input_ring_pop(input_ring_t* ring, input_event_t* event) {
    if (!ring || !event) return false;

    spinlock_acquire(&ring->lock);

    /* Check if buffer is empty */
    if (ring->head == ring->tail) {
        spinlock_release(&ring->lock);
        return false;
    }

    /* Copy event from buffer */
    *event = ring->events[ring->tail];

    /* Memory barrier to ensure event is read before tail is updated */
    __asm__ volatile("" ::: "memory");

    /* Update tail pointer */
    ring->tail = (ring->tail + 1) & INPUT_RING_MASK;
    ring->events_read++;

    spinlock_release(&ring->lock);
    return true;
}

/* Peek at the next event without removing it */
bool input_ring_peek(input_ring_t* ring, input_event_t* event) {
    if (!ring || !event) return false;

    spinlock_acquire(&ring->lock);

    /* Check if buffer is empty */
    if (ring->head == ring->tail) {
        spinlock_release(&ring->lock);
        return false;
    }

    /* Copy event from buffer (don't update tail) */
    *event = ring->events[ring->tail];

    spinlock_release(&ring->lock);
    return true;
}

/* Pop multiple events */
uint32 input_ring_pop_batch(input_ring_t* ring, input_event_t* events, uint32 max_count) {
    if (!ring || !events || max_count == 0) return 0;

    spinlock_acquire(&ring->lock);

    uint32 popped = 0;
    while (popped < max_count && ring->head != ring->tail) {
        events[popped] = ring->events[ring->tail];
        ring->tail = (ring->tail + 1) & INPUT_RING_MASK;
        ring->events_read++;
        popped++;
    }

    spinlock_release(&ring->lock);
    return popped;
}

/* Discard events without reading them */
uint32 input_ring_discard(input_ring_t* ring, uint32 count) {
    if (!ring || count == 0) return 0;

    spinlock_acquire(&ring->lock);

    uint32 available = input_ring_count(ring);
    uint32 to_discard = (count < available) ? count : available;

    ring->tail = (ring->tail + to_discard) & INPUT_RING_MASK;
    ring->events_read += to_discard;

    spinlock_release(&ring->lock);
    return to_discard;
}

/* Wait for data to be available */
void input_ring_wait_for_data(input_ring_t* ring) {
    if (!ring) return;

    /* Simple busy-wait with HLT for now
     * TODO: Integrate with proper task wait queue */
    while (input_ring_is_empty(ring)) {
        /* Yield CPU to other tasks if scheduler is available */
        __asm__ volatile("hlt");
    }
}

/* Wake up all tasks waiting on this ring buffer */
void input_ring_wake_waiters(input_ring_t* ring) {
    if (!ring || !ring->wait_queue) return;

    /* TODO: Implement proper wait queue wakeup
     * For now, tasks use busy-wait with HLT which will be
     * woken by the next interrupt */
    (void)ring->wait_queue;
}

/* Set the wait queue for this ring buffer */
void input_ring_set_wait_queue(input_ring_t* ring, void* wait_queue) {
    if (!ring) return;
    ring->wait_queue = wait_queue;
}

/* Clear statistics */
void input_ring_clear_stats(input_ring_t* ring) {
    if (!ring) return;

    spinlock_acquire(&ring->lock);
    ring->events_written = 0;
    ring->events_read = 0;
    ring->overflows = 0;
    spinlock_release(&ring->lock);
}

/* Print ring buffer status (for debugging) */
void input_ring_debug_print(const input_ring_t* ring) {
    if (!ring) return;

    debuglog(DEBUG_INFO, "[InputRing] %s: head=%u tail=%u count=%u "
             "written=%u read=%u overflows=%u\n",
             ring->name ? ring->name : "unnamed",
             ring->head, ring->tail, input_ring_count(ring),
             ring->events_written, ring->events_read, ring->overflows);
}

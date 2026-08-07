/**
 * xHCI (eXtensible Host Controller Interface) Driver for Fern
 *
 * Implements USB 3.0/3.1 support via the xHCI controller specification.
 * Based on Intel xHCI 1.2 specification and OSDev documentation.
 */

#include "../include/usb/xhci.h"
#include "../include/usb/usb.h"
#include "../include/system.h"
#include "../include/memory.h"
#include "../include/pci.h"
#include "../include/debuglog.h"
#include "../include/debug.h"
#include <string.h>

void* kmalloc(size_t size);

// Ring sizes
#define XHCI_CMD_RING_SIZE      256
#define XHCI_EVENT_RING_SIZE    256
#define XHCI_TRANSFER_RING_SIZE 256
#define XHCI_MAX_TRANSFER_RINGS 32

// Timeouts (in microseconds)
#define XHCI_RESET_TIMEOUT      1000000
#define XHCI_CMD_TIMEOUT        5000000
#define XHCI_PORT_RESET_TIMEOUT 500000

// Memory alignment requirements
#define XHCI_DCBAA_ALIGNMENT    64
#define XHCI_RING_ALIGNMENT     64
#define XHCI_CONTEXT_ALIGNMENT  64
#define XHCI_TRB_ALIGNMENT      16

// Forward declarations
static bool xhci_wait_ready(xhci_data_t* hc, uint32 timeout);
static bool xhci_reset(xhci_data_t* hc);
static bool xhci_alloc_rings(xhci_data_t* hc);
static void xhci_free_rings(xhci_data_t* hc);
static bool xhci_alloc_dcbaa(xhci_data_t* hc);
static bool xhci_alloc_scratchpad(xhci_data_t* hc);
static void xhci_ring_doorbell(xhci_data_t* hc, uint32 slot, uint32 target);
static bool xhci_send_command(xhci_data_t* hc, xhci_trb_t* cmd, xhci_trb_t* result);
static void xhci_process_events(xhci_data_t* hc);
static xhci_trb_t* xhci_alloc_transfer_ring(xhci_data_t* hc);
static void xhci_free_transfer_ring(xhci_data_t* hc, xhci_trb_t* ring);
static int xhci_enable_slot(xhci_data_t* hc);
static bool xhci_address_device(xhci_data_t* hc, uint8 slot, uint8 port, usb_speed_t speed);
static bool xhci_configure_endpoint(xhci_data_t* hc, uint8 slot, usb_endpoint_t* ep);

// Controller operations
usb_controller_ops_t xhci_ops = {
    .init = xhci_init,
    .shutdown = xhci_shutdown,
    .reset_port = xhci_reset_port,
    .enable_port = xhci_enable_port,
    .disable_port = xhci_disable_port,
    .get_port_speed = xhci_get_port_speed,
    .port_connected = xhci_port_connected,
    .control_transfer = xhci_control_transfer,
    .bulk_transfer = xhci_bulk_transfer,
    .interrupt_transfer = xhci_interrupt_transfer,
    .poll = xhci_poll
};

/**
 * Read xHCI capability register (8-bit)
 */
static inline uint8 xhci_read_cap8(xhci_data_t* hc, uint32 offset) {
    return mmio_read8((const volatile void*)((uintptr_t)hc->cap_regs + offset));
}

/**
 * Read xHCI capability register (16-bit)
 */
static inline uint16 xhci_read_cap16(xhci_data_t* hc, uint32 offset) {
    return mmio_read16((const volatile void*)((uintptr_t)hc->cap_regs + offset));
}

/**
 * Read xHCI capability register (32-bit)
 */
static inline uint32 xhci_read_cap32(xhci_data_t* hc, uint32 offset) {
    return mmio_read32((const volatile void*)((uintptr_t)hc->cap_regs + offset));
}

/**
 * Read xHCI operational register (32-bit)
 */
static inline uint32 xhci_read_op32(xhci_data_t* hc, uint32 offset) {
    return mmio_read32((const volatile void*)((uintptr_t)hc->op_regs + offset));
}

/**
 * Write xHCI operational register (32-bit)
 */
static inline void xhci_write_op32(xhci_data_t* hc, uint32 offset, uint32 value) {
    mmio_write32((volatile void*)((uintptr_t)hc->op_regs + offset), value);
}

/**
 * Read xHCI operational register (64-bit)
 */
static inline uint64 xhci_read_op64(xhci_data_t* hc, uint32 offset) {
    uint32 lo = mmio_read32((const volatile void*)((uintptr_t)hc->op_regs + offset));
    uint32 hi = mmio_read32((const volatile void*)((uintptr_t)hc->op_regs + offset + 4));
    return ((uint64)hi << 32) | lo;
}

/**
 * Write xHCI operational register (64-bit)
 */
static inline void xhci_write_op64(xhci_data_t* hc, uint32 offset, uint64 value) {
    mmio_write32((volatile void*)((uintptr_t)hc->op_regs + offset), (uint32)value);
    mmio_write32((volatile void*)((uintptr_t)hc->op_regs + offset + 4), (uint32)(value >> 32));
}

/**
 * Read xHCI runtime register (32-bit)
 */
static inline uint32 xhci_read_rt32(xhci_data_t* hc, uint32 offset) {
    return mmio_read32((const volatile void*)((uintptr_t)hc->runtime_regs + offset));
}

/**
 * Write xHCI runtime register (32-bit)
 */
static inline void xhci_write_rt32(xhci_data_t* hc, uint32 offset, uint32 value) {
    mmio_write32((volatile void*)((uintptr_t)hc->runtime_regs + offset), value);
}

/**
 * Read xHCI runtime register (64-bit)
 */
static inline uint64 xhci_read_rt64(xhci_data_t* hc, uint32 offset) {
    uint32 lo = mmio_read32((const volatile void*)((uintptr_t)hc->runtime_regs + offset));
    uint32 hi = mmio_read32((const volatile void*)((uintptr_t)hc->runtime_regs + offset + 4));
    return ((uint64)hi << 32) | lo;
}

/**
 * Write xHCI runtime register (64-bit)
 */
static inline void xhci_write_rt64(xhci_data_t* hc, uint32 offset, uint64 value) {
    mmio_write32((volatile void*)((uintptr_t)hc->runtime_regs + offset), (uint32)value);
    mmio_write32((volatile void*)((uintptr_t)hc->runtime_regs + offset + 4), (uint32)(value >> 32));
}

/**
 * Read port status/control register
 */
static inline uint32 xhci_read_port(xhci_data_t* hc, uint8 port) {
    return xhci_read_op32(hc, XHCI_OP_PORTSC + (port * 16));
}

/**
 * Write port status/control register
 */
static inline void xhci_write_port(xhci_data_t* hc, uint8 port, uint32 value) {
    xhci_write_op32(hc, XHCI_OP_PORTSC + (port * 16), value);
}

/**
 * Ring the doorbell for a slot
 */
static void xhci_ring_doorbell(xhci_data_t* hc, uint32 slot, uint32 target) {
    mmio_write32((volatile void*)((uintptr_t)hc->doorbell_regs + (slot * 4)), target);
}

/**
 * Wait for controller to become ready
 */
static bool xhci_wait_ready(xhci_data_t* hc, uint32 timeout) {
    uint32 elapsed = 0;
    while (elapsed < timeout) {
        uint32 status = xhci_read_op32(hc, XHCI_OP_USBSTS);
        if (!(status & XHCI_STS_CNR)) {
            return true;
        }
        // Simple delay
        for (volatile int i = 0; i < 1000; i++);
        elapsed += 100;
    }
    return false;
}

/**
 * Reset the xHCI controller
 */
static bool xhci_reset(xhci_data_t* hc) {
    // Stop the controller first
    uint32 cmd = xhci_read_op32(hc, XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RS;
    xhci_write_op32(hc, XHCI_OP_USBCMD, cmd);

    // Wait for halt
    uint32 timeout = 100000;
    while (timeout > 0) {
        uint32 status = xhci_read_op32(hc, XHCI_OP_USBSTS);
        if (status & XHCI_STS_HCH) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    if (timeout == 0) {
        debug_print("xHCI: Failed to halt controller\n");
        return false;
    }

    // Reset the controller
    cmd = xhci_read_op32(hc, XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_HCRST;
    xhci_write_op32(hc, XHCI_OP_USBCMD, cmd);

    // Wait for reset to complete
    timeout = XHCI_RESET_TIMEOUT;
    while (timeout > 0) {
        cmd = xhci_read_op32(hc, XHCI_OP_USBCMD);
        if (!(cmd & XHCI_CMD_HCRST)) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    if (timeout == 0) {
        debug_print("xHCI: Reset timeout\n");
        return false;
    }

    // Wait for controller ready
    if (!xhci_wait_ready(hc, XHCI_RESET_TIMEOUT)) {
        debug_print("xHCI: Controller not ready after reset\n");
        return false;
    }

    debug_print("xHCI: Controller reset complete\n");
    return true;
}

/**
 * Allocate Device Context Base Address Array
 */
static bool xhci_alloc_dcbaa(xhci_data_t* hc) {
    // DCBAA must be aligned to 64 bytes and hold (MaxSlots + 1) pointers
    uint32 dcbaa_size = (hc->max_slots + 1) * sizeof(uint64);

    hc->dcbaa = (uint64*)kmalloc_aligned(dcbaa_size, XHCI_DCBAA_ALIGNMENT);
    if (!hc->dcbaa) {
        debug_print("xHCI: Failed to allocate DCBAA\n");
        return false;
    }

    memset(hc->dcbaa, 0, dcbaa_size);

    // Allocate device context pointers array
    hc->device_contexts = (xhci_device_context_t**)kmalloc(
        (hc->max_slots + 1) * sizeof(xhci_device_context_t*));
    if (!hc->device_contexts) {
        kfree(hc->dcbaa);
        debug_print("xHCI: Failed to allocate device contexts array\n");
        return false;
    }

    memset(hc->device_contexts, 0, (hc->max_slots + 1) * sizeof(xhci_device_context_t*));

    // Write DCBAAP
    uint64 dcbaa_phys = (uint64)(uintptr_t)hc->dcbaa;
    xhci_write_op64(hc, XHCI_OP_DCBAAP, dcbaa_phys);

    debug_print("xHCI: DCBAA allocated at 0x");
    debug_print_hex((uint32)dcbaa_phys);
    debug_print("\n");

    return true;
}

/**
 * Allocate scratchpad buffers if required
 */
static bool xhci_alloc_scratchpad(xhci_data_t* hc) {
    uint32 hcs2 = xhci_read_cap32(hc, XHCI_CAP_HCSPARAMS2);
    uint32 max_scratchpad_hi = (hcs2 >> 21) & 0x1F;
    uint32 max_scratchpad_lo = (hcs2 >> 27) & 0x1F;
    uint32 num_scratchpad = (max_scratchpad_hi << 5) | max_scratchpad_lo;

    if (num_scratchpad == 0) {
        debug_print("xHCI: No scratchpad buffers required\n");
        return true;
    }

    debug_print("xHCI: Allocating ");
    debug_print_dec(num_scratchpad);
    debug_print(" scratchpad buffers\n");

    // Allocate scratchpad buffer array
    hc->scratchpad_buffers = (uint64*)kmalloc_aligned(
        num_scratchpad * sizeof(uint64), XHCI_DCBAA_ALIGNMENT);
    if (!hc->scratchpad_buffers) {
        debug_print("xHCI: Failed to allocate scratchpad array\n");
        return false;
    }

    // Allocate individual scratchpad buffers
    for (uint32 i = 0; i < num_scratchpad; i++) {
        void* buf = kmalloc_aligned(hc->page_size, hc->page_size);
        if (!buf) {
            debug_print("xHCI: Failed to allocate scratchpad buffer\n");
            // Free previously allocated buffers
            for (uint32 j = 0; j < i; j++) {
                kfree((void*)(uintptr_t)hc->scratchpad_buffers[j]);
            }
            kfree(hc->scratchpad_buffers);
            return false;
        }
        hc->scratchpad_buffers[i] = (uint64)(uintptr_t)buf;
    }

    // Store scratchpad array pointer in DCBAA[0]
    hc->dcbaa[0] = (uint64)(uintptr_t)hc->scratchpad_buffers;

    return true;
}

/**
 * Allocate command and event rings
 */
static bool xhci_alloc_rings(xhci_data_t* hc) {
    // Allocate command ring
    hc->cmd_ring = (xhci_trb_t*)kmalloc_aligned(
        XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t), XHCI_RING_ALIGNMENT);
    if (!hc->cmd_ring) {
        debug_print("xHCI: Failed to allocate command ring\n");
        return false;
    }

    memset(hc->cmd_ring, 0, XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t));
    hc->cmd_ring_enqueue = 0;
    hc->cmd_ring_cycle = true;

    // Add link TRB at end of command ring
    xhci_trb_t* link = &hc->cmd_ring[XHCI_CMD_RING_SIZE - 1];
    link->parameter = (uint64)(uintptr_t)hc->cmd_ring;
    link->status = 0;
    link->control = (XHCI_TRB_LINK << 10) | XHCI_TRB_CTRL_C;  // Toggle cycle

    // Write command ring control register
    uint64 crcr = (uint64)(uintptr_t)hc->cmd_ring | (hc->cmd_ring_cycle ? 1 : 0);
    xhci_write_op64(hc, XHCI_OP_CRCR, crcr);

    // Allocate event ring
    hc->event_ring = (xhci_trb_t*)kmalloc_aligned(
        XHCI_EVENT_RING_SIZE * sizeof(xhci_trb_t), XHCI_RING_ALIGNMENT);
    if (!hc->event_ring) {
        debug_print("xHCI: Failed to allocate event ring\n");
        kfree(hc->cmd_ring);
        return false;
    }

    memset(hc->event_ring, 0, XHCI_EVENT_RING_SIZE * sizeof(xhci_trb_t));
    hc->event_ring_dequeue = 0;
    hc->event_ring_cycle = true;

    // Allocate Event Ring Segment Table (ERST)
    hc->erst = (xhci_erst_entry_t*)kmalloc_aligned(
        sizeof(xhci_erst_entry_t), XHCI_RING_ALIGNMENT);
    if (!hc->erst) {
        debug_print("xHCI: Failed to allocate ERST\n");
        kfree(hc->event_ring);
        kfree(hc->cmd_ring);
        return false;
    }

    // Set up ERST entry
    hc->erst[0].ring_segment_base = (uint64)(uintptr_t)hc->event_ring;
    hc->erst[0].ring_segment_size = XHCI_EVENT_RING_SIZE;
    hc->erst[0].reserved = 0;

    // Configure interrupter 0
    uint32 ir_offset = XHCI_RT_IR0;

    // Set ERSTSZ
    xhci_write_rt32(hc, ir_offset + XHCI_IR_ERSTSZ, 1);

    // Set ERDP
    uint64 erdp = (uint64)(uintptr_t)hc->event_ring;
    xhci_write_rt64(hc, ir_offset + XHCI_IR_ERDP, erdp);

    // Set ERSTBA
    uint64 erstba = (uint64)(uintptr_t)hc->erst;
    xhci_write_rt64(hc, ir_offset + XHCI_IR_ERSTBA, erstba);

    // Enable interrupts for interrupter 0
    uint32 iman = xhci_read_rt32(hc, ir_offset + XHCI_IR_IMAN);
    iman |= XHCI_IMAN_IE;
    xhci_write_rt32(hc, ir_offset + XHCI_IR_IMAN, iman);

    // Allocate transfer ring pool
    hc->transfer_ring_pool = (xhci_trb_t*)kmalloc_aligned(
        XHCI_MAX_TRANSFER_RINGS * XHCI_TRANSFER_RING_SIZE * sizeof(xhci_trb_t),
        XHCI_RING_ALIGNMENT);
    if (!hc->transfer_ring_pool) {
        debug_print("xHCI: Failed to allocate transfer ring pool\n");
        kfree(hc->erst);
        kfree(hc->event_ring);
        kfree(hc->cmd_ring);
        return false;
    }

    memset(hc->transfer_ring_pool, 0,
           XHCI_MAX_TRANSFER_RINGS * XHCI_TRANSFER_RING_SIZE * sizeof(xhci_trb_t));
    memset(hc->transfer_ring_bitmap, 0, sizeof(hc->transfer_ring_bitmap));

    debug_print("xHCI: Rings allocated - CMD=0x");
    debug_print_hex((uint32)(uintptr_t)hc->cmd_ring);
    debug_print(" EVENT=0x");
    debug_print_hex((uint32)(uintptr_t)hc->event_ring);
    debug_print("\n");

    return true;
}

/**
 * Free all ring memory
 */
static void xhci_free_rings(xhci_data_t* hc) {
    if (hc->transfer_ring_pool) {
        kfree(hc->transfer_ring_pool);
        hc->transfer_ring_pool = NULL;
    }
    if (hc->erst) {
        kfree(hc->erst);
        hc->erst = NULL;
    }
    if (hc->event_ring) {
        kfree(hc->event_ring);
        hc->event_ring = NULL;
    }
    if (hc->cmd_ring) {
        kfree(hc->cmd_ring);
        hc->cmd_ring = NULL;
    }
}

/**
 * Allocate a transfer ring from the pool
 */
static xhci_trb_t* xhci_alloc_transfer_ring(xhci_data_t* hc) {
    for (int i = 0; i < XHCI_MAX_TRANSFER_RINGS; i++) {
        int word = i / 32;
        int bit = i % 32;
        if (!(hc->transfer_ring_bitmap[word] & (1 << bit))) {
            hc->transfer_ring_bitmap[word] |= (1 << bit);
            xhci_trb_t* ring = &hc->transfer_ring_pool[i * XHCI_TRANSFER_RING_SIZE];
            memset(ring, 0, XHCI_TRANSFER_RING_SIZE * sizeof(xhci_trb_t));

            // Add link TRB at end
            xhci_trb_t* link = &ring[XHCI_TRANSFER_RING_SIZE - 1];
            link->parameter = (uint64)(uintptr_t)ring;
            link->status = 0;
            link->control = (XHCI_TRB_LINK << 10) | XHCI_TRB_CTRL_C;

            return ring;
        }
    }
    return NULL;
}

/**
 * Free a transfer ring back to the pool
 */
static void xhci_free_transfer_ring(xhci_data_t* hc, xhci_trb_t* ring) {
    if (!ring || !hc->transfer_ring_pool) return;

    int index = (ring - hc->transfer_ring_pool) / XHCI_TRANSFER_RING_SIZE;
    if (index >= 0 && index < XHCI_MAX_TRANSFER_RINGS) {
        int word = index / 32;
        int bit = index % 32;
        hc->transfer_ring_bitmap[word] &= ~(1 << bit);
    }
}

/**
 * Enqueue a TRB to the command ring
 */
static void xhci_enqueue_command(xhci_data_t* hc, xhci_trb_t* trb) {
    xhci_trb_t* dest = &hc->cmd_ring[hc->cmd_ring_enqueue];

    // Copy TRB and set cycle bit
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = trb->control;
    if (hc->cmd_ring_cycle) {
        dest->control |= XHCI_TRB_CTRL_C;
    } else {
        dest->control &= ~XHCI_TRB_CTRL_C;
    }

    // Advance enqueue pointer
    hc->cmd_ring_enqueue++;
    if (hc->cmd_ring_enqueue >= XHCI_CMD_RING_SIZE - 1) {
        // Wrap around (skip link TRB)
        hc->cmd_ring_enqueue = 0;
        hc->cmd_ring_cycle = !hc->cmd_ring_cycle;

        // Update link TRB cycle bit
        xhci_trb_t* link = &hc->cmd_ring[XHCI_CMD_RING_SIZE - 1];
        if (hc->cmd_ring_cycle) {
            link->control |= XHCI_TRB_CTRL_C;
        } else {
            link->control &= ~XHCI_TRB_CTRL_C;
        }
    }
}

/**
 * Send a command and wait for completion
 */
static bool xhci_send_command(xhci_data_t* hc, xhci_trb_t* cmd, xhci_trb_t* result) {
    // Enqueue the command
    xhci_enqueue_command(hc, cmd);

    // Ring the host controller doorbell
    xhci_ring_doorbell(hc, 0, 0);

    // Wait for command completion event
    uint32 timeout = XHCI_CMD_TIMEOUT;
    while (timeout > 0) {
        xhci_trb_t* event = &hc->event_ring[hc->event_ring_dequeue];

        // Check if event is valid (cycle bit matches)
        bool event_cycle = (event->control & XHCI_TRB_CTRL_C) != 0;
        if (event_cycle == hc->event_ring_cycle) {
            uint32 trb_type = (event->control >> 10) & 0x3F;

            if (trb_type == XHCI_TRB_CMD_COMPLETION) {
                // Copy result
                if (result) {
                    *result = *event;
                }

                // Advance dequeue pointer
                hc->event_ring_dequeue++;
                if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
                    hc->event_ring_dequeue = 0;
                    hc->event_ring_cycle = !hc->event_ring_cycle;
                }

                // Update ERDP
                uint64 erdp = (uint64)(uintptr_t)&hc->event_ring[hc->event_ring_dequeue];
                erdp |= (1 << 3);  // Event Handler Busy
                xhci_write_rt64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);

                // Check completion code
                uint8 cc = (event->status >> 24) & 0xFF;
                return (cc == XHCI_CC_SUCCESS);
            }

            // Handle other events
            hc->event_ring_dequeue++;
            if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
                hc->event_ring_dequeue = 0;
                hc->event_ring_cycle = !hc->event_ring_cycle;
            }
        }

        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    debug_print("xHCI: Command timeout\n");
    return false;
}

/**
 * Enable a device slot
 */
static int xhci_enable_slot(xhci_data_t* hc) {
    xhci_trb_t cmd = {0};
    xhci_trb_t result = {0};

    cmd.parameter = 0;
    cmd.status = 0;
    cmd.control = (XHCI_TRB_ENABLE_SLOT << 10);

    if (!xhci_send_command(hc, &cmd, &result)) {
        debug_print("xHCI: Enable Slot command failed\n");
        return -1;
    }

    uint8 slot_id = (result.control >> 24) & 0xFF;
    debug_print("xHCI: Slot ");
    debug_print_dec(slot_id);
    debug_print(" enabled\n");

    return slot_id;
}

/**
 * Address a device
 */
static bool xhci_address_device(xhci_data_t* hc, uint8 slot, uint8 port, usb_speed_t speed) {
    // Allocate input context (must be 64-byte aligned)
    uint32 ctx_size = hc->context_size;
    uint32 input_ctx_size = ctx_size * 33;  // Control + Slot + 31 endpoints

    uint8* input_ctx = (uint8*)kmalloc_aligned(input_ctx_size, XHCI_CONTEXT_ALIGNMENT);
    if (!input_ctx) {
        debug_print("xHCI: Failed to allocate input context\n");
        return false;
    }
    memset(input_ctx, 0, input_ctx_size);

    // Allocate output device context
    xhci_device_context_t* output_ctx = (xhci_device_context_t*)kmalloc_aligned(
        sizeof(xhci_device_context_t), XHCI_CONTEXT_ALIGNMENT);
    if (!output_ctx) {
        kfree(input_ctx);
        debug_print("xHCI: Failed to allocate output context\n");
        return false;
    }
    memset(output_ctx, 0, sizeof(xhci_device_context_t));

    // Set up input control context
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)input_ctx;
    icc->add_flags = (1 << 0) | (1 << 1);  // Add Slot Context and EP0
    icc->drop_flags = 0;

    // Set up slot context
    xhci_slot_context_t* slot_ctx = (xhci_slot_context_t*)(input_ctx + ctx_size);

    // Convert USB speed to xHCI speed
    uint8 xhci_speed;
    switch (speed) {
        case USB_SPEED_LOW:  xhci_speed = XHCI_SPEED_LOW; break;
        case USB_SPEED_FULL: xhci_speed = XHCI_SPEED_FULL; break;
        case USB_SPEED_HIGH: xhci_speed = XHCI_SPEED_HIGH; break;
        case USB_SPEED_SUPER: xhci_speed = XHCI_SPEED_SUPER; break;
        default: xhci_speed = XHCI_SPEED_FULL; break;
    }

    slot_ctx->route_string_and_speed = (xhci_speed << 20) | (1 << 27);  // Context entries = 1
    slot_ctx->port_and_state = ((port + 1) << 16);  // Root hub port number

    // Set up endpoint 0 context (control endpoint)
    xhci_endpoint_context_t* ep0_ctx = (xhci_endpoint_context_t*)(input_ctx + ctx_size * 2);

    // Allocate transfer ring for EP0
    xhci_trb_t* ep0_ring = xhci_alloc_transfer_ring(hc);
    if (!ep0_ring) {
        kfree(output_ctx);
        kfree(input_ctx);
        debug_print("xHCI: Failed to allocate EP0 transfer ring\n");
        return false;
    }

    // Determine max packet size based on speed
    uint16 max_packet;
    switch (speed) {
        case USB_SPEED_LOW:  max_packet = 8; break;
        case USB_SPEED_FULL: max_packet = 64; break;
        case USB_SPEED_HIGH: max_packet = 64; break;
        case USB_SPEED_SUPER: max_packet = 512; break;
        default: max_packet = 8; break;
    }

    ep0_ctx->ep_info1 = 0;
    ep0_ctx->ep_info2 = (max_packet << 16) | (4 << 3) | (3 << 1);  // MaxPacketSize, Control EP, CErr=3
    ep0_ctx->tr_dequeue_ptr = (uint64)(uintptr_t)ep0_ring | 1;  // DCS=1
    ep0_ctx->average_trb_length = 8;

    // Store output context in DCBAA
    hc->dcbaa[slot] = (uint64)(uintptr_t)output_ctx;
    hc->device_contexts[slot] = output_ctx;

    // Send Address Device command
    xhci_trb_t cmd = {0};
    xhci_trb_t result = {0};

    cmd.parameter = (uint64)(uintptr_t)input_ctx;
    cmd.status = 0;
    cmd.control = (XHCI_TRB_ADDRESS_DEVICE << 10) | (slot << 24);

    bool success = xhci_send_command(hc, &cmd, &result);

    // Free input context (no longer needed)
    kfree(input_ctx);

    if (!success) {
        uint8 cc = (result.status >> 24) & 0xFF;
        debug_print("xHCI: Address Device failed, CC=");
        debug_print_dec(cc);
        debug_print("\n");
        xhci_free_transfer_ring(hc, ep0_ring);
        kfree(output_ctx);
        hc->dcbaa[slot] = 0;
        hc->device_contexts[slot] = NULL;
        return false;
    }

    debug_print("xHCI: Device addressed on slot ");
    debug_print_dec(slot);
    debug_print("\n");

    return true;
}

/**
 * Process pending events
 */
static void xhci_process_events(xhci_data_t* hc) {
    while (true) {
        xhci_trb_t* event = &hc->event_ring[hc->event_ring_dequeue];

        // Check if event is valid
        bool event_cycle = (event->control & XHCI_TRB_CTRL_C) != 0;
        if (event_cycle != hc->event_ring_cycle) {
            break;  // No more events
        }

        uint32 trb_type = (event->control >> 10) & 0x3F;

        switch (trb_type) {
            case XHCI_TRB_PORT_STATUS_CHANGE: {
                uint8 port = ((event->parameter >> 24) & 0xFF) - 1;
                debug_print("xHCI: Port ");
                debug_print_dec(port);
                debug_print(" status change\n");

                // Clear port change bits
                uint32 portsc = xhci_read_port(hc, port);
                portsc |= XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_PRC |
                          XHCI_PORT_WRC | XHCI_PORT_OCC | XHCI_PORT_PLC | XHCI_PORT_CEC;
                // Preserve RW1S bits
                portsc &= ~(XHCI_PORT_PED);
                xhci_write_port(hc, port, portsc);
                break;
            }

            case XHCI_TRB_TRANSFER_EVENT: {
                uint8 cc = (event->status >> 24) & 0xFF;
                uint8 slot = (event->control >> 24) & 0xFF;
                uint8 ep = (event->control >> 16) & 0x1F;
                (void)slot;
                (void)ep;
                if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
                    debug_print("xHCI: Transfer error CC=");
                    debug_print_dec(cc);
                    debug_print("\n");
                }
                break;
            }

            case XHCI_TRB_HOST_CONTROLLER:
                debug_print("xHCI: Host controller event\n");
                break;

            default:
                break;
        }

        // Advance dequeue pointer
        hc->event_ring_dequeue++;
        if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
            hc->event_ring_dequeue = 0;
            hc->event_ring_cycle = !hc->event_ring_cycle;
        }
    }

    // Update ERDP
    uint64 erdp = (uint64)(uintptr_t)&hc->event_ring[hc->event_ring_dequeue];
    erdp |= (1 << 3);  // Event Handler Busy
    xhci_write_rt64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);
}

/**
 * Initialize xHCI controller
 */
bool xhci_init(usb_controller_t* controller) {
    debug_print("xHCI: Initializing controller\n");

    // Allocate controller-specific data
    xhci_data_t* hc = (xhci_data_t*)kmalloc(sizeof(xhci_data_t));
    if (!hc) {
        debug_print("xHCI: Failed to allocate controller data\n");
        return false;
    }

    memset(hc, 0, sizeof(xhci_data_t));
    controller->hcd_data = hc;

    // Map capability registers from BAR0
    hc->cap_regs = (volatile uint8*)(uintptr_t)controller->base_address;

    // Read capability register length
    hc->cap_length = xhci_read_cap8(hc, XHCI_CAP_CAPLENGTH);

    // Set up operational registers
    hc->op_regs = (volatile uint32*)(uintptr_t)(controller->base_address + hc->cap_length);

    // Read structural parameters
    uint32 hcs1 = xhci_read_cap32(hc, XHCI_CAP_HCSPARAMS1);
    hc->max_slots = hcs1 & XHCI_HCS1_MAXSLOTS_MASK;
    hc->max_interrupters = (hcs1 & XHCI_HCS1_MAXINTRS_MASK) >> XHCI_HCS1_MAXINTRS_SHIFT;
    hc->max_ports = (hcs1 & XHCI_HCS1_MAXPORTS_MASK) >> XHCI_HCS1_MAXPORTS_SHIFT;

    // Read capability parameters
    uint32 hcc1 = xhci_read_cap32(hc, XHCI_CAP_HCCPARAMS1);
    hc->has_64bit = (hcc1 & XHCI_HCC1_AC64) != 0;
    hc->context_size = (hcc1 & XHCI_HCC1_CSZ) ? 64 : 32;
    hc->xecp = (hcc1 & XHCI_HCC1_XECP_MASK) >> XHCI_HCC1_XECP_SHIFT;

    // Read doorbell and runtime register offsets
    uint32 dboff = xhci_read_cap32(hc, XHCI_CAP_DBOFF);
    uint32 rtsoff = xhci_read_cap32(hc, XHCI_CAP_RTSOFF);

    hc->doorbell_regs = (volatile uint32*)(uintptr_t)(controller->base_address + dboff);
    hc->runtime_regs = (volatile uint32*)(uintptr_t)(controller->base_address + rtsoff);

    // Read page size
    uint32 pagesize_reg = xhci_read_op32(hc, XHCI_OP_PAGESIZE);
    hc->page_size = 1 << (12 + __builtin_ffs(pagesize_reg) - 1);

    debug_print("xHCI: Capability length: 0x");
    debug_print_hex(hc->cap_length);
    debug_print("\n");
    debug_print("xHCI: Max slots: ");
    debug_print_dec(hc->max_slots);
    debug_print(", Max ports: ");
    debug_print_dec(hc->max_ports);
    debug_print(", Max interrupters: ");
    debug_print_dec(hc->max_interrupters);
    debug_print("\n");
    debug_print("xHCI: Context size: ");
    debug_print_dec(hc->context_size);
    debug_print(" bytes, 64-bit: ");
    debug_print(hc->has_64bit ? "yes" : "no");
    debug_print("\n");

    // Reset the controller
    if (!xhci_reset(hc)) {
        kfree(hc);
        return false;
    }

    // Set max enabled device slots
    uint32 config = xhci_read_op32(hc, XHCI_OP_CONFIG);
    config = (config & ~0xFF) | hc->max_slots;
    xhci_write_op32(hc, XHCI_OP_CONFIG, config);

    // Allocate DCBAA
    if (!xhci_alloc_dcbaa(hc)) {
        kfree(hc);
        return false;
    }

    // Allocate scratchpad if needed
    if (!xhci_alloc_scratchpad(hc)) {
        kfree(hc->dcbaa);
        kfree(hc);
        return false;
    }

    // Allocate command and event rings
    if (!xhci_alloc_rings(hc)) {
        kfree(hc->dcbaa);
        kfree(hc);
        return false;
    }

    // Start the controller
    uint32 cmd = xhci_read_op32(hc, XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_RS | XHCI_CMD_INTE;
    xhci_write_op32(hc, XHCI_OP_USBCMD, cmd);

    // Wait for running
    uint32 timeout = 100000;
    while (timeout > 0) {
        uint32 status = xhci_read_op32(hc, XHCI_OP_USBSTS);
        if (!(status & XHCI_STS_HCH)) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    if (timeout == 0) {
        debug_print("xHCI: Controller failed to start\n");
        xhci_free_rings(hc);
        kfree(hc->dcbaa);
        kfree(hc);
        return false;
    }

    controller->num_ports = hc->max_ports;
    controller->initialized = true;

    debug_print("xHCI: Controller initialized with ");
    debug_print_dec(hc->max_ports);
    debug_print(" ports\n");

    return true;
}

/**
 * Shutdown xHCI controller
 */
void xhci_shutdown(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) return;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    // Stop the controller
    uint32 cmd = xhci_read_op32(hc, XHCI_OP_USBCMD);
    cmd &= ~(XHCI_CMD_RS | XHCI_CMD_INTE);
    xhci_write_op32(hc, XHCI_OP_USBCMD, cmd);

    // Wait for halt
    uint32 timeout = 100000;
    while (timeout > 0) {
        uint32 status = xhci_read_op32(hc, XHCI_OP_USBSTS);
        if (status & XHCI_STS_HCH) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    // Free device contexts
    if (hc->device_contexts) {
        for (int i = 1; i <= hc->max_slots; i++) {
            if (hc->device_contexts[i]) {
                kfree(hc->device_contexts[i]);
            }
        }
        kfree(hc->device_contexts);
    }

    // Free rings
    xhci_free_rings(hc);

    // Free DCBAA
    if (hc->dcbaa) {
        kfree(hc->dcbaa);
    }

    // Free scratchpad
    if (hc->scratchpad_buffers) {
        kfree(hc->scratchpad_buffers);
    }

    kfree(hc);
    controller->hcd_data = NULL;
    controller->initialized = false;

    debug_print("xHCI: Controller shutdown complete\n");
}

/**
 * Reset a port
 */
bool xhci_reset_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    if (port >= hc->max_ports) return false;

    uint32 portsc = xhci_read_port(hc, port);

    // Check if device is connected
    if (!(portsc & XHCI_PORT_CCS)) {
        return false;
    }

    // Determine if USB3 or USB2 port
    uint8 speed = (portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
    bool is_usb3 = (speed == XHCI_SPEED_SUPER || speed == XHCI_SPEED_SUPER_PLUS);

    // Issue appropriate reset
    if (is_usb3) {
        // Warm reset for USB3
        portsc |= XHCI_PORT_WPR;
    } else {
        // Standard reset for USB2
        portsc |= XHCI_PORT_PR;
    }

    // Preserve certain bits, clear change bits
    portsc &= ~(XHCI_PORT_PED);  // Don't write 1 to PED
    xhci_write_port(hc, port, portsc);

    // Wait for reset to complete
    uint32 timeout = XHCI_PORT_RESET_TIMEOUT;
    while (timeout > 0) {
        portsc = xhci_read_port(hc, port);
        if (!(portsc & XHCI_PORT_PR)) {
            break;
        }
        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    if (timeout == 0) {
        debug_print("xHCI: Port reset timeout\n");
        return false;
    }

    // Clear reset change bits
    portsc = xhci_read_port(hc, port);
    portsc |= XHCI_PORT_PRC | XHCI_PORT_WRC;
    portsc &= ~XHCI_PORT_PED;
    xhci_write_port(hc, port, portsc);

    // Small delay for device to settle
    for (volatile int i = 0; i < 50000; i++);

    debug_print("xHCI: Port ");
    debug_print_dec(port);
    debug_print(" reset complete\n");

    return true;
}

/**
 * Enable a port
 */
bool xhci_enable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    if (port >= hc->max_ports) return false;

    // In xHCI, ports are enabled automatically after reset
    // We just need to check that the port is enabled
    uint32 portsc = xhci_read_port(hc, port);

    return (portsc & XHCI_PORT_PED) != 0;
}

/**
 * Disable a port
 */
bool xhci_disable_port(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    if (port >= hc->max_ports) return false;

    uint32 portsc = xhci_read_port(hc, port);

    // Write 1 to PED to disable the port
    portsc |= XHCI_PORT_PED;
    xhci_write_port(hc, port, portsc);

    return true;
}

/**
 * Get port speed
 */
usb_speed_t xhci_get_port_speed(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return USB_SPEED_FULL;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    if (port >= hc->max_ports) return USB_SPEED_FULL;

    uint32 portsc = xhci_read_port(hc, port);
    uint8 speed = (portsc & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;

    switch (speed) {
        case XHCI_SPEED_LOW:        return USB_SPEED_LOW;
        case XHCI_SPEED_FULL:       return USB_SPEED_FULL;
        case XHCI_SPEED_HIGH:       return USB_SPEED_HIGH;
        case XHCI_SPEED_SUPER:      return USB_SPEED_SUPER;
        case XHCI_SPEED_SUPER_PLUS: return USB_SPEED_SUPER_PLUS;
        default:                    return USB_SPEED_FULL;
    }
}

/**
 * Check if device is connected to port
 */
bool xhci_port_connected(usb_controller_t* controller, uint8 port) {
    if (!controller || !controller->hcd_data) return false;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    if (port >= hc->max_ports) return false;

    uint32 portsc = xhci_read_port(hc, port);
    return (portsc & XHCI_PORT_CCS) != 0;
}

/**
 * Perform a control transfer
 */
int xhci_control_transfer(usb_controller_t* controller, usb_device_t* device,
                          usb_setup_packet_t* setup, void* data, uint16 length) {
    if (!controller || !controller->hcd_data || !device || !setup) return -1;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    // Get slot ID from device
    uint8 slot = device->address;
    if (slot == 0 || slot > hc->max_slots || !hc->device_contexts[slot]) {
        return -1;
    }

    // Get EP0 transfer ring
    xhci_device_context_t* ctx = hc->device_contexts[slot];
    xhci_trb_t* ring = (xhci_trb_t*)(uintptr_t)(ctx->endpoints[0].tr_dequeue_ptr & ~0xF);

    if (!ring) return -1;

    // Find enqueue position in transfer ring
    uint32 enqueue = 0;
    bool cycle = true;

    // Build Setup Stage TRB
    xhci_trb_t setup_trb = {0};
    setup_trb.parameter = *(uint64*)setup;  // Copy setup packet
    setup_trb.status = 8;  // Transfer length = 8 (setup packet size)
    setup_trb.control = (XHCI_TRB_SETUP << 10) | XHCI_TRB_CTRL_IDT | XHCI_TRB_CTRL_IOC;

    // Set transfer type
    if (length > 0) {
        if (setup->bmRequestType & 0x80) {
            setup_trb.control |= XHCI_TRB_CTRL_TRT_IN_DATA;
        } else {
            setup_trb.control |= XHCI_TRB_CTRL_TRT_OUT_DATA;
        }
    } else {
        setup_trb.control |= XHCI_TRB_CTRL_TRT_NO_DATA;
    }

    if (cycle) setup_trb.control |= XHCI_TRB_CTRL_C;
    ring[enqueue++] = setup_trb;

    // Build Data Stage TRB (if needed)
    if (length > 0 && data) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter = (uint64)(uintptr_t)data;
        data_trb.status = length;
        data_trb.control = (XHCI_TRB_DATA << 10) | XHCI_TRB_CTRL_IOC;

        if (setup->bmRequestType & 0x80) {
            data_trb.control |= XHCI_TRB_CTRL_DIR;  // IN
        }

        if (cycle) data_trb.control |= XHCI_TRB_CTRL_C;
        ring[enqueue++] = data_trb;
    }

    // Build Status Stage TRB
    xhci_trb_t status_trb = {0};
    status_trb.parameter = 0;
    status_trb.status = 0;
    status_trb.control = (XHCI_TRB_STATUS << 10) | XHCI_TRB_CTRL_IOC;

    // Direction is opposite of data stage
    if (length == 0 || !(setup->bmRequestType & 0x80)) {
        status_trb.control |= XHCI_TRB_CTRL_DIR;  // IN
    }

    if (cycle) status_trb.control |= XHCI_TRB_CTRL_C;
    ring[enqueue++] = status_trb;

    // Ring the doorbell for slot/EP0
    xhci_ring_doorbell(hc, slot, 1);  // EP0 = doorbell target 1

    // Wait for completion
    uint32 timeout = XHCI_CMD_TIMEOUT;
    int result = -1;

    while (timeout > 0) {
        xhci_trb_t* event = &hc->event_ring[hc->event_ring_dequeue];

        bool event_cycle = (event->control & XHCI_TRB_CTRL_C) != 0;
        if (event_cycle == hc->event_ring_cycle) {
            uint32 trb_type = (event->control >> 10) & 0x3F;

            if (trb_type == XHCI_TRB_TRANSFER_EVENT) {
                uint8 event_slot = (event->control >> 24) & 0xFF;

                if (event_slot == slot) {
                    uint8 cc = (event->status >> 24) & 0xFF;
                    uint32 residue = event->status & 0xFFFFFF;

                    // Advance event ring
                    hc->event_ring_dequeue++;
                    if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
                        hc->event_ring_dequeue = 0;
                        hc->event_ring_cycle = !hc->event_ring_cycle;
                    }

                    // Update ERDP
                    uint64 erdp = (uint64)(uintptr_t)&hc->event_ring[hc->event_ring_dequeue];
                    erdp |= (1 << 3);
                    xhci_write_rt64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);

                    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                        result = length - residue;
                    }
                    break;
                }
            }

            // Process other events
            hc->event_ring_dequeue++;
            if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
                hc->event_ring_dequeue = 0;
                hc->event_ring_cycle = !hc->event_ring_cycle;
            }
        }

        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    return result;
}

/**
 * Perform a bulk transfer
 */
int xhci_bulk_transfer(usb_controller_t* controller, usb_device_t* device,
                       usb_endpoint_t* endpoint, void* data, uint32 length) {
    if (!controller || !controller->hcd_data || !device || !endpoint || !data) {
        return -1;
    }

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    uint8 slot = device->address;
    if (slot == 0 || slot > hc->max_slots) return -1;

    // Calculate endpoint context index (DCI)
    // DCI = (Endpoint Number * 2) + Direction
    uint8 ep_num = endpoint->address & 0x0F;
    uint8 dir = (endpoint->address & 0x80) ? 1 : 0;
    uint8 dci = (ep_num * 2) + dir;

    // Get transfer ring for this endpoint
    xhci_device_context_t* ctx = hc->device_contexts[slot];
    if (!ctx) return -1;

    xhci_trb_t* ring = (xhci_trb_t*)(uintptr_t)(ctx->endpoints[dci - 1].tr_dequeue_ptr & ~0xF);
    if (!ring) return -1;

    // Build Normal TRB for bulk transfer
    xhci_trb_t trb = {0};
    trb.parameter = (uint64)(uintptr_t)data;
    trb.status = length;
    trb.control = (XHCI_TRB_NORMAL << 10) | XHCI_TRB_CTRL_IOC | XHCI_TRB_CTRL_C;

    ring[0] = trb;

    // Ring doorbell
    xhci_ring_doorbell(hc, slot, dci);

    // Wait for completion
    uint32 timeout = XHCI_CMD_TIMEOUT;
    int result = -1;

    while (timeout > 0) {
        xhci_trb_t* event = &hc->event_ring[hc->event_ring_dequeue];

        bool event_cycle = (event->control & XHCI_TRB_CTRL_C) != 0;
        if (event_cycle == hc->event_ring_cycle) {
            uint32 trb_type = (event->control >> 10) & 0x3F;

            if (trb_type == XHCI_TRB_TRANSFER_EVENT) {
                uint8 event_slot = (event->control >> 24) & 0xFF;

                if (event_slot == slot) {
                    uint8 cc = (event->status >> 24) & 0xFF;
                    uint32 residue = event->status & 0xFFFFFF;

                    hc->event_ring_dequeue++;
                    if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
                        hc->event_ring_dequeue = 0;
                        hc->event_ring_cycle = !hc->event_ring_cycle;
                    }

                    uint64 erdp = (uint64)(uintptr_t)&hc->event_ring[hc->event_ring_dequeue];
                    erdp |= (1 << 3);
                    xhci_write_rt64(hc, XHCI_RT_IR0 + XHCI_IR_ERDP, erdp);

                    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                        result = length - residue;
                    }
                    break;
                }
            }

            hc->event_ring_dequeue++;
            if (hc->event_ring_dequeue >= XHCI_EVENT_RING_SIZE) {
                hc->event_ring_dequeue = 0;
                hc->event_ring_cycle = !hc->event_ring_cycle;
            }
        }

        for (volatile int i = 0; i < 1000; i++);
        timeout -= 100;
    }

    return result;
}

/**
 * Perform an interrupt transfer
 */
int xhci_interrupt_transfer(usb_controller_t* controller, usb_device_t* device,
                            usb_endpoint_t* endpoint, void* data, uint32 length) {
    // Interrupt transfers work similarly to bulk transfers in xHCI
    return xhci_bulk_transfer(controller, device, endpoint, data, length);
}

/**
 * Poll for controller events
 */
void xhci_poll(usb_controller_t* controller) {
    if (!controller || !controller->hcd_data) return;

    xhci_data_t* hc = (xhci_data_t*)controller->hcd_data;

    // Check for status changes
    uint32 status = xhci_read_op32(hc, XHCI_OP_USBSTS);

    if (status & XHCI_STS_HSE) {
        debug_print("xHCI: Host System Error!\n");
        // Clear the error
        xhci_write_op32(hc, XHCI_OP_USBSTS, XHCI_STS_HSE);
    }

    if (status & XHCI_STS_EINT) {
        // Clear interrupt
        xhci_write_op32(hc, XHCI_OP_USBSTS, XHCI_STS_EINT);

        // Clear interrupter pending
        uint32 iman = xhci_read_rt32(hc, XHCI_RT_IR0 + XHCI_IR_IMAN);
        if (iman & XHCI_IMAN_IP) {
            xhci_write_rt32(hc, XHCI_RT_IR0 + XHCI_IR_IMAN, iman);
        }

        // Process events
        xhci_process_events(hc);
    }

    if (status & XHCI_STS_PCD) {
        // Port Change Detected - process events will handle it
        xhci_write_op32(hc, XHCI_OP_USBSTS, XHCI_STS_PCD);
        xhci_process_events(hc);
    }
}

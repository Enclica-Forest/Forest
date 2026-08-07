/*
 * Fern - Cross-Architecture Input Event Interface
 * input.h
 *
 * Unified input event API that bridges architecture-specific hardware
 * to the kernel's input event system (input_event.h / input_ring.h).
 *
 * Supported input sources:
 *   - x86:     PS/2 keyboard (IRQ 1) + PS/2 mouse (IRQ 12)
 *   - ARM32:   PL011 UART serial input (treated as keyboard)
 *   - AArch64: PL011 UART serial input (treated as keyboard)
 *   - RISC-V:  8250 UART serial input (treated as keyboard)
 *
 * Event flow:
 *   Hardware IRQ -> arch_keyboard_handler() / ps2_mouse_irq_handler()
 *                -> input_report_key() / input_report_mouse()
 *                -> input_event_ring  (ring buffer)
 *                -> input_mux_dispatch_event()
 *                -> Consumer callbacks
 */

#ifndef FOREST_ARCH_INPUT_H
#define FOREST_ARCH_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "../include/input_event.h"
#include "../include/input_ring.h"

/* Maximum number of registered input devices */
#define INPUT_MAX_DEVICES   8

/* Input device types */
#define INPUT_DEV_TYPE_KEYBOARD  0x01
#define INPUT_DEV_TYPE_MOUSE     0x02
#define INPUT_DEV_TYPE_TOUCH     0x04
#define INPUT_DEV_TYPE_JOYSTICK  0x08

/* Input device information */
typedef struct {
    uint32 id;                  /* Unique device ID */
    const char* name;           /* Device name */
    uint32 type;                /* Device type bitmask */
    uint32 bus_type;            /* Bus type (BUS_I8042, BUS_RS232, etc.) */
    bool active;                /* Device currently active */
} input_device_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * input_init() - Initialize the input subsystem.
 *
 * Creates the global event ring buffer and registers default devices
 * based on the current architecture:
 *   - x86: registers PS/2 keyboard and PS/2 mouse
 *   - ARM/RISC-V: registers UART keyboard
 *
 * Returns 0 on success, -1 on error.
 */
int input_init(void);

/**
 * input_register_device() - Register an input device.
 *
 * @name: Human-readable device name (e.g. "PS/2 Keyboard")
 * @type: Device type bitmask (INPUT_DEV_TYPE_*)
 *
 * Returns the assigned device ID, or 0 on error.
 */
uint32 input_register_device(const char* name, uint32 type);

/**
 * input_report_key() - Report a key/button event.
 *
 * Called from architecture-specific IRQ handlers to inject key events
 * into the input event ring.
 *
 * @device_id: ID of the device generating the event
 * @key:       Key code (use KEY_* from input_event.h / ps2_keyboard.h)
 * @value:     KEY_PRESS, KEY_RELEASE, or KEY_REPEAT
 */
void input_report_key(uint32 device_id, uint32 key, int32 value);

/**
 * input_report_mouse() - Report a mouse movement/button event.
 *
 * Generates EV_REL events for dx/dy movement and EV_KEY events for
 * button state changes.
 *
 * @device_id: ID of the device generating the event
 * @dx:        Horizontal movement delta
 * @dy:        Vertical movement delta
 * @buttons:   Bitmask of pressed buttons (bit 0=left, 1=right, 2=middle)
 */
void input_report_mouse(uint32 device_id, int32 dx, int32 dy, uint32 buttons);

/**
 * input_get_event() - Retrieve the next input event.
 *
 * Non-blocking read from the global event ring.
 *
 * @event: Pointer to store the retrieved event
 *
 * Returns true if an event was retrieved, false if ring is empty.
 */
bool input_get_event(input_event_t* event);

/**
 * input_has_events() - Check if any input events are pending.
 */
bool input_has_events(void);

/**
 * input_get_device_count() - Get the number of registered devices.
 */
uint32 input_get_device_count(void);

/**
 * input_get_device() - Get device info by ID.
 *
 * @device_id: Device ID to query
 *
 * Returns pointer to device info, or NULL if not found.
 */
const input_device_t* input_get_device(uint32 device_id);

/**
 * input_dump_devices() - Print registered devices (debug).
 */
void input_dump_devices(void);

#if !ARCH_IS_X86
/**
 * input_uart_char() - Bridge UART byte to input events (non-x86).
 *
 * Converts an ASCII character from the UART into EV_KEY press/release
 * events with appropriate shift handling.  Called from
 * arch_keyboard_handler() on ARM/RISC-V platforms.
 *
 * @c: ASCII character received from UART
 */
void input_uart_char(char c);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FOREST_ARCH_INPUT_H */

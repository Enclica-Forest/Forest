/*
 * Fern - Cross-Architecture Keyboard Input Interface
 * keyboard.h
 *
 * Unified keyboard input API across all supported architectures:
 *   - x86:     PS/2 keyboard via IRQ 1 (scancode set 1)
 *   - ARM32:   PL011 UART input (polling or interrupt)
 *   - AArch64: PL011 UART input (polling or interrupt)
 *   - RISC-V:  8250 UART input (polling or interrupt)
 *
 * The interface provides both blocking and non-blocking character
 * input, suitable for early boot console and kernel shell usage.
 */

#ifndef FOREST_ARCH_KEYBOARD_H
#define FOREST_ARCH_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Keyboard input source */
typedef enum {
    ARCH_KBD_INPUT_PS2 = 0,   /* PS/2 keyboard (x86 IRQ 1) */
    ARCH_KBD_INPUT_UART,      /* Serial UART input */
    ARCH_KBD_INPUT_NONE,      /* No input source available */
} arch_keyboard_source_t;

/* Special key codes (beyond ASCII, stored in uint16 via buf) */
#define ARCH_KEY_SPECIAL_BASE   0x100
#define ARCH_KEY_UP             (ARCH_KEY_SPECIAL_BASE + 0)
#define ARCH_KEY_DOWN           (ARCH_KEY_SPECIAL_BASE + 1)
#define ARCH_KEY_LEFT           (ARCH_KEY_SPECIAL_BASE + 2)
#define ARCH_KEY_RIGHT          (ARCH_KEY_SPECIAL_BASE + 3)
#define ARCH_KEY_HOME           (ARCH_KEY_SPECIAL_BASE + 4)
#define ARCH_KEY_END            (ARCH_KEY_SPECIAL_BASE + 5)
#define ARCH_KEY_PAGE_UP        (ARCH_KEY_SPECIAL_BASE + 6)
#define ARCH_KEY_PAGE_DOWN      (ARCH_KEY_SPECIAL_BASE + 7)
#define ARCH_KEY_INSERT         (ARCH_KEY_SPECIAL_BASE + 8)
#define ARCH_KEY_DELETE          (ARCH_KEY_SPECIAL_BASE + 9)
#define ARCH_KEY_F1             (ARCH_KEY_SPECIAL_BASE + 10)
#define ARCH_KEY_F2             (ARCH_KEY_SPECIAL_BASE + 11)
#define ARCH_KEY_F3             (ARCH_KEY_SPECIAL_BASE + 12)
#define ARCH_KEY_F4             (ARCH_KEY_SPECIAL_BASE + 13)
#define ARCH_KEY_F5             (ARCH_KEY_SPECIAL_BASE + 14)
#define ARCH_KEY_F6             (ARCH_KEY_SPECIAL_BASE + 15)
#define ARCH_KEY_F7             (ARCH_KEY_SPECIAL_BASE + 16)
#define ARCH_KEY_F8             (ARCH_KEY_SPECIAL_BASE + 17)
#define ARCH_KEY_F9             (ARCH_KEY_SPECIAL_BASE + 18)
#define ARCH_KEY_F10            (ARCH_KEY_SPECIAL_BASE + 19)
#define ARCH_KEY_F11            (ARCH_KEY_SPECIAL_BASE + 20)
#define ARCH_KEY_F12            (ARCH_KEY_SPECIAL_BASE + 21)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * arch_keyboard_init() - Initialize the keyboard subsystem.
 *
 * Auto-detects the input source based on architecture:
 *   - x86: initializes PS/2 keyboard controller and enables IRQ
 *   - ARM/RISC-V: initializes UART for serial input
 *
 * Returns 0 on success, -1 if no input source is available.
 */
int arch_keyboard_init(void);

/**
 * arch_keyboard_getc() - Read a character (blocking).
 *
 * Blocks until a character is available from the active input source.
 * Returns the character as an unsigned char cast to int, or -1 on error.
 */
int arch_keyboard_getc(void);

/**
 * arch_keyboard_getc_nonblocking() - Non-blocking character read.
 *
 * Returns the character if one is available, or -1 if none.
 */
int arch_keyboard_getc_nonblocking(void);

/**
 * arch_keyboard_handler() - Process a raw scancode/byte from IRQ.
 *
 * Called from the architecture-specific IRQ handler to feed input
 * into the keyboard subsystem. Handles scancode translation on x86,
 * direct ASCII on ARM/RISC-V.
 *
 * @c: Raw input byte (scancode for x86, ASCII for UART platforms).
 */
void arch_keyboard_handler(char c);

/**
 * arch_keyboard_get_source() - Query the active input source.
 */
arch_keyboard_source_t arch_keyboard_get_source(void);

/**
 * arch_keyboard_is_available() - Check if keyboard input is ready.
 */
bool arch_keyboard_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* FOREST_ARCH_KEYBOARD_H */

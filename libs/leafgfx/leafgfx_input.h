/*
 * LeafGFX Input Handling
 *
 * Provides keyboard and mouse input handling via device files
 * for Forest OS userspace applications.
 */

#ifndef LEAFGFX_INPUT_H
#define LEAFGFX_INPUT_H

#include "leafgfx.h"

// ============================================================================
// Input Event Types (Linux evdev-compatible)
// ============================================================================

#define GFX_EV_SYN          0x00    // Synchronization event
#define GFX_EV_KEY          0x01    // Key/button event
#define GFX_EV_REL          0x02    // Relative axis event (mouse movement)
#define GFX_EV_ABS          0x03    // Absolute axis event (touchscreen)

// Relative axis codes
#define GFX_REL_X           0x00
#define GFX_REL_Y           0x01
#define GFX_REL_WHEEL       0x08

// Mouse button codes
#define GFX_BTN_LEFT        0x110
#define GFX_BTN_RIGHT       0x111
#define GFX_BTN_MIDDLE      0x112

// Special key codes
#define GFX_KEY_ESC         1
#define GFX_KEY_1           2
#define GFX_KEY_2           3
#define GFX_KEY_3           4
#define GFX_KEY_4           5
#define GFX_KEY_5           6
#define GFX_KEY_6           7
#define GFX_KEY_7           8
#define GFX_KEY_8           9
#define GFX_KEY_9           10
#define GFX_KEY_0           11
#define GFX_KEY_MINUS       12
#define GFX_KEY_EQUAL       13
#define GFX_KEY_BACKSPACE   14
#define GFX_KEY_TAB         15
#define GFX_KEY_Q           16
#define GFX_KEY_W           17
#define GFX_KEY_E           18
#define GFX_KEY_R           19
#define GFX_KEY_T           20
#define GFX_KEY_Y           21
#define GFX_KEY_U           22
#define GFX_KEY_I           23
#define GFX_KEY_O           24
#define GFX_KEY_P           25
#define GFX_KEY_LEFTBRACE   26
#define GFX_KEY_RIGHTBRACE  27
#define GFX_KEY_ENTER       28
#define GFX_KEY_LEFTCTRL    29
#define GFX_KEY_A           30
#define GFX_KEY_S           31
#define GFX_KEY_D           32
#define GFX_KEY_F           33
#define GFX_KEY_G           34
#define GFX_KEY_H           35
#define GFX_KEY_J           36
#define GFX_KEY_K           37
#define GFX_KEY_L           38
#define GFX_KEY_SEMICOLON   39
#define GFX_KEY_APOSTROPHE  40
#define GFX_KEY_GRAVE       41
#define GFX_KEY_LEFTSHIFT   42
#define GFX_KEY_BACKSLASH   43
#define GFX_KEY_Z           44
#define GFX_KEY_X           45
#define GFX_KEY_C           46
#define GFX_KEY_V           47
#define GFX_KEY_B           48
#define GFX_KEY_N           49
#define GFX_KEY_M           50
#define GFX_KEY_COMMA       51
#define GFX_KEY_DOT         52
#define GFX_KEY_SLASH       53
#define GFX_KEY_RIGHTSHIFT  54
#define GFX_KEY_LEFTALT     56
#define GFX_KEY_SPACE       57
#define GFX_KEY_CAPSLOCK    58
#define GFX_KEY_F1          59
#define GFX_KEY_F2          60
#define GFX_KEY_F3          61
#define GFX_KEY_F4          62
#define GFX_KEY_F5          63
#define GFX_KEY_F6          64
#define GFX_KEY_F7          65
#define GFX_KEY_F8          66
#define GFX_KEY_F9          67
#define GFX_KEY_F10         68
#define GFX_KEY_F11         87
#define GFX_KEY_F12         88
#define GFX_KEY_UP          103
#define GFX_KEY_LEFT        105
#define GFX_KEY_RIGHT       106
#define GFX_KEY_DOWN        108
#define GFX_KEY_DELETE      111

// ============================================================================
// Input Event Structure (Linux evdev-compatible, 16 bytes)
// ============================================================================

typedef struct {
    uint32_t tv_sec;      // Timestamp seconds
    uint32_t tv_usec;     // Timestamp microseconds
    uint16_t type;        // Event type (EV_KEY, EV_REL, etc.)
    uint16_t code;        // Event code (key code or axis)
    int32_t  value;       // Event value (1=press, 0=release, delta for REL)
} __attribute__((packed)) gfx_input_event_t;

// ============================================================================
// Mouse State
// ============================================================================

typedef struct {
    int32_t  x;           // Current X position
    int32_t  y;           // Current Y position
    int32_t  dx;          // X delta since last poll
    int32_t  dy;          // Y delta since last poll
    int32_t  wheel;       // Scroll wheel delta
    bool     left;        // Left button state
    bool     right;       // Right button state
    bool     middle;      // Middle button state
    bool     left_click;  // Left button just clicked (edge detect)
    bool     right_click; // Right button just clicked
    bool     left_release;  // Left button just released
    bool     right_release; // Right button just released
} gfx_mouse_state_t;

// ============================================================================
// Keyboard State
// ============================================================================

typedef struct {
    bool     keys[256];     // Current state of each key (by scancode)
    bool     shift;         // Either shift key is down
    bool     ctrl;          // Either ctrl key is down
    bool     alt;           // Either alt key is down
    bool     caps_lock;     // Caps lock is active
    char     last_char;     // Last character typed (0 if none)
    uint16_t last_key;      // Last key code pressed
    bool     key_pressed;   // A key was pressed this frame
    bool     key_released;  // A key was released this frame
} gfx_keyboard_state_t;

// ============================================================================
// Input System Functions
// ============================================================================

/**
 * Initialize the input system
 *
 * Opens /dev/kbd and /dev/mouse device files
 *
 * @return      0 on success, negative error code on failure
 */
int gfx_input_init(void);

/**
 * Shutdown the input system
 */
void gfx_input_shutdown(void);

/**
 * Poll for input events
 *
 * Call this once per frame to update mouse and keyboard state.
 * This function is non-blocking.
 */
void gfx_input_poll(void);

/**
 * Get current mouse state
 *
 * @return      Pointer to the current mouse state
 */
const gfx_mouse_state_t* gfx_get_mouse(void);

/**
 * Get current keyboard state
 *
 * @return      Pointer to the current keyboard state
 */
const gfx_keyboard_state_t* gfx_get_keyboard(void);

/**
 * Set mouse position (clamp to screen bounds)
 *
 * @param x     New X position
 * @param y     New Y position
 */
void gfx_set_mouse_position(int32_t x, int32_t y);

/**
 * Set mouse bounds (for clamping)
 *
 * @param min_x Minimum X coordinate
 * @param min_y Minimum Y coordinate
 * @param max_x Maximum X coordinate
 * @param max_y Maximum Y coordinate
 */
void gfx_set_mouse_bounds(int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y);

// ============================================================================
// Keyboard Helper Functions
// ============================================================================

/**
 * Check if a specific key is currently down
 *
 * @param key_code      Key code (GFX_KEY_* constant)
 * @return              true if key is down
 */
bool gfx_key_down(uint16_t key_code);

/**
 * Check if a specific key was just pressed this frame
 *
 * @param key_code      Key code
 * @return              true if key was just pressed
 */
bool gfx_key_pressed(uint16_t key_code);

/**
 * Get the last typed character (handles shift)
 *
 * @return              ASCII character, or 0 if no character typed
 */
char gfx_get_typed_char(void);

/**
 * Convert a key code to ASCII character
 *
 * @param key_code      Key code
 * @param shift         Shift modifier state
 * @return              ASCII character, or 0 if not a printable key
 */
char gfx_key_to_char(uint16_t key_code, bool shift);

// ============================================================================
// Mouse Helper Functions
// ============================================================================

/**
 * Check if left mouse button is currently down
 */
bool gfx_mouse_left_down(void);

/**
 * Check if left mouse button was just clicked
 */
bool gfx_mouse_left_clicked(void);

/**
 * Check if left mouse button was just released
 */
bool gfx_mouse_left_released(void);

/**
 * Check if right mouse button is currently down
 */
bool gfx_mouse_right_down(void);

/**
 * Check if right mouse button was just clicked
 */
bool gfx_mouse_right_clicked(void);

/**
 * Get mouse X position
 */
int32_t gfx_mouse_x(void);

/**
 * Get mouse Y position
 */
int32_t gfx_mouse_y(void);

/**
 * Check if mouse is within a rectangle
 */
bool gfx_mouse_in_rect(int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Check if mouse is within a circle
 */
bool gfx_mouse_in_circle(int32_t cx, int32_t cy, int32_t radius);

#endif // LEAFGFX_INPUT_H

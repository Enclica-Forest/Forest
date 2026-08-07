/*
 * LeafGFX Input Handling - Implementation
 *
 * Provides keyboard and mouse input handling via syscalls and device files.
 * Uses direct syscalls for more reliable input reading, with device files
 * as a fallback mechanism.
 */

#include "leafgfx_input.h"
#include "leafgfx.h"
#include <stdio.h>
#include <string.h>

// Use sysroot syscall interface (avoids kernel enum/macro conflicts)
#include <forestos/syscalls.h>

// Syscall interface
static long syscall_raw(int num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long result;
#if defined(__x86_64__)
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ __volatile__(
        "syscall"
        : "=a"(result)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
#else
    __asm__ __volatile__(
        "push %%ebp\n"
        "mov %7, %%ebp\n"
        "int $0x80\n"
        "pop %%ebp\n"
        : "=a"(result)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), "g"(a6)
        : "memory"
    );
#endif
    return result;
}

#define syscall0(n)                     syscall_raw(n, 0, 0, 0, 0, 0, 0)
#define syscall1(n, a)                  syscall_raw(n, (long)(a), 0, 0, 0, 0, 0)

// ============================================================================
// Global State
// ============================================================================

static FILE* g_kbd_dev = NULL;
static FILE* g_mouse_dev = NULL;
static bool g_use_syscalls = true;  // Prefer syscall-based input

static gfx_mouse_state_t g_mouse = {0};
static gfx_keyboard_state_t g_keyboard = {0};

// Mouse bounds
static int32_t g_mouse_min_x = 0;
static int32_t g_mouse_min_y = 0;
static int32_t g_mouse_max_x = 1920;
static int32_t g_mouse_max_y = 1080;

// Previous button states for edge detection
static bool g_mouse_left_prev = false;
static bool g_mouse_right_prev = false;
static bool g_mouse_middle_prev = false;

// Key states for edge detection
static bool g_keys_prev[256] = {0};

// ============================================================================
// Scancode to ASCII Mapping
// ============================================================================

static const char scancode_to_ascii_lower[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

static const char scancode_to_ascii_upper[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

// ============================================================================
// Initialization
// ============================================================================

int gfx_input_init(void) {
    // Initialize mouse position to center of bounds
    g_mouse.x = (g_mouse_min_x + g_mouse_max_x) / 2;
    g_mouse.y = (g_mouse_min_y + g_mouse_max_y) / 2;

    // Set bounds from screen dimensions if graphics initialized
    const gfx_framebuffer_t* fb = gfx_get_framebuffer();
    if (fb && fb->width > 0 && fb->height > 0) {
        g_mouse_max_x = fb->width - 1;
        g_mouse_max_y = fb->height - 1;
        g_mouse.x = fb->width / 2;
        g_mouse.y = fb->height / 2;
    }

    // Test if syscall-based input is working
    long poll_result = syscall0(SYS_POLL_INPUT);
    if (poll_result >= 0) {
        g_use_syscalls = true;
        printf("[LeafGFX] Using syscall-based input (SYS_POLL_INPUT works)\n");
    } else {
        g_use_syscalls = false;
        printf("[LeafGFX] Syscall input not available, using device files\n");
    }

    if (!g_use_syscalls) {
        // Open input device files only when syscall-based input is unavailable.
        g_kbd_dev = fopen("/dev/kbd", "r");
        if (!g_kbd_dev) {
            // Try alternative paths
            g_kbd_dev = fopen("/dev/keyboard", "r");
        }

        g_mouse_dev = fopen("/dev/mouse", "r");
        if (!g_mouse_dev) {
            // Try alternative paths
            g_mouse_dev = fopen("/dev/ps2mouse", "r");
            if (!g_mouse_dev) {
                g_mouse_dev = fopen("/dev/psaux", "r");
                if (!g_mouse_dev) {
                    g_mouse_dev = fopen("/dev/input/mice", "r");
                }
            }
        }
    }

    printf("[LeafGFX] Keyboard device: %s\n",
           g_kbd_dev ? "opened" : (g_use_syscalls ? "skipped (syscalls)" : "not opened"));
    printf("[LeafGFX] Mouse device: %s\n",
           g_mouse_dev ? "opened" : (g_use_syscalls ? "skipped (syscalls)" : "not opened"));

    // Clear keyboard state
    memset(g_keyboard.keys, 0, sizeof(g_keyboard.keys));
    memset(g_keys_prev, 0, sizeof(g_keys_prev));

    printf("[LeafGFX] Input init complete - mode: %s\n", g_use_syscalls ? "syscall" : "device files");
    
    return 0;  // Success even if devices not found (will use syscalls)
}

void gfx_input_shutdown(void) {
    if (g_kbd_dev) {
        fclose(g_kbd_dev);
        g_kbd_dev = NULL;
    }
    if (g_mouse_dev) {
        fclose(g_mouse_dev);
        g_mouse_dev = NULL;
    }
}

// ============================================================================
// Input Polling - Helper to process keyboard event
// ============================================================================

static void process_kbd_event(gfx_input_event_t* ev) {
    if (ev->type == GFX_EV_KEY) {
        uint16_t code = ev->code;
        bool pressed = (ev->value != 0);

        if (code < 256) {
            g_keyboard.keys[code] = pressed;
        }

        if (pressed) {
            g_keyboard.key_pressed = true;
            g_keyboard.last_key = code;

            // Update modifiers
            if (code == GFX_KEY_LEFTSHIFT || code == GFX_KEY_RIGHTSHIFT) {
                g_keyboard.shift = true;
            }
            if (code == GFX_KEY_LEFTCTRL) {
                g_keyboard.ctrl = true;
            }
            if (code == GFX_KEY_LEFTALT) {
                g_keyboard.alt = true;
            }
            if (code == GFX_KEY_CAPSLOCK) {
                g_keyboard.caps_lock = !g_keyboard.caps_lock;
            }

            // Convert to character
            char c = gfx_key_to_char(code, g_keyboard.shift ^ g_keyboard.caps_lock);
            if (c != 0) {
                g_keyboard.last_char = c;
            }
        } else {
            g_keyboard.key_released = true;

            // Update modifiers
            if (code == GFX_KEY_LEFTSHIFT || code == GFX_KEY_RIGHTSHIFT) {
                g_keyboard.shift = false;
            }
            if (code == GFX_KEY_LEFTCTRL) {
                g_keyboard.ctrl = false;
            }
            if (code == GFX_KEY_LEFTALT) {
                g_keyboard.alt = false;
            }
        }
    }
}

// ============================================================================
// Input Polling - Helper to process mouse event
// ============================================================================

static void process_mouse_event(gfx_input_event_t* ev) {
    switch (ev->type) {
        case GFX_EV_REL:
            switch (ev->code) {
                case GFX_REL_X:
                    g_mouse.dx += ev->value;
                    g_mouse.x += ev->value;
                    break;
                case GFX_REL_Y:
                    g_mouse.dy += ev->value;
                    g_mouse.y += ev->value;
                    break;
                case GFX_REL_WHEEL:
                    g_mouse.wheel += ev->value;
                    break;
            }
            break;

        case GFX_EV_KEY:
            switch (ev->code) {
                case GFX_BTN_LEFT:
                    g_mouse.left = (ev->value != 0);
                    break;
                case GFX_BTN_RIGHT:
                    g_mouse.right = (ev->value != 0);
                    break;
                case GFX_BTN_MIDDLE:
                    g_mouse.middle = (ev->value != 0);
                    break;
            }
            break;
    }
}

// ============================================================================
// Input Polling
// ============================================================================

void gfx_input_poll(void) {
    g_mouse.dx = 0;
    g_mouse.dy = 0;
    g_mouse.wheel = 0;
    g_mouse.left_click = false;
    g_mouse.right_click = false;
    g_mouse.left_release = false;
    g_mouse.right_release = false;
    g_keyboard.last_char = 0;
    g_keyboard.last_key = 0;
    g_keyboard.key_pressed = false;
    g_keyboard.key_released = false;

    g_mouse_left_prev = g_mouse.left;
    g_mouse_right_prev = g_mouse.right;
    g_mouse_middle_prev = g_mouse.middle;
    memcpy(g_keys_prev, g_keyboard.keys, sizeof(g_keys_prev));

    gfx_input_event_t ev;

    if (g_use_syscalls) {
        int mouse_events = 0;
        int kbd_events = 0;
        const int MAX_MOUSE_EVENTS = 256;
        const int MAX_KBD_EVENTS = 256;

        while (mouse_events < MAX_MOUSE_EVENTS || kbd_events < MAX_KBD_EVENTS) {
            long available = syscall0(SYS_POLL_INPUT);
            if (available <= 0) {
                break;
            }

            bool got_any = false;

            if ((available & 2) && mouse_events < MAX_MOUSE_EVENTS) {
                long result = syscall1(SYS_READ_MOUSE_EVENT, (long)&ev);
                if (result > 0) {
                    process_mouse_event(&ev);
                    mouse_events++;
                    got_any = true;
                }
            }

            if ((available & 1) && kbd_events < MAX_KBD_EVENTS) {
                long result = syscall1(SYS_READ_KBD_EVENT, (long)&ev);
                if (result > 0) {
                    process_kbd_event(&ev);
                    kbd_events++;
                    got_any = true;
                }
            }

            if (!got_any) break;
        }
    } else {
        gfx_input_event_t mouse_ev, kbd_ev;
        bool mouse_has_data = true;
        bool kbd_has_data = true;
        int total_events = 0;
        const int MAX_TOTAL_EVENTS = 64;

        while ((mouse_has_data || kbd_has_data) && total_events < MAX_TOTAL_EVENTS) {
            if (mouse_has_data && g_mouse_dev) {
                if (fread(&mouse_ev, 1, sizeof(mouse_ev), g_mouse_dev) == sizeof(mouse_ev)) {
                    process_mouse_event(&mouse_ev);
                    total_events++;
                } else {
                    mouse_has_data = false;
                }
            } else {
                mouse_has_data = false;
            }

            if (kbd_has_data && g_kbd_dev) {
                if (fread(&kbd_ev, 1, sizeof(kbd_ev), g_kbd_dev) == sizeof(kbd_ev)) {
                    process_kbd_event(&kbd_ev);
                    total_events++;
                } else {
                    kbd_has_data = false;
                }
            } else {
                kbd_has_data = false;
            }
        }
    }

    if (g_mouse.x < g_mouse_min_x) g_mouse.x = g_mouse_min_x;
    if (g_mouse.x > g_mouse_max_x) g_mouse.x = g_mouse_max_x;
    if (g_mouse.y < g_mouse_min_y) g_mouse.y = g_mouse_min_y;
    if (g_mouse.y > g_mouse_max_y) g_mouse.y = g_mouse_max_y;

    if (g_mouse.left && !g_mouse_left_prev) {
        g_mouse.left_click = true;
    }
    if (!g_mouse.left && g_mouse_left_prev) {
        g_mouse.left_release = true;
    }
    if (g_mouse.right && !g_mouse_right_prev) {
        g_mouse.right_click = true;
    }
    if (!g_mouse.right && g_mouse_right_prev) {
        g_mouse.right_release = true;
    }
}

// ============================================================================
// State Getters
// ============================================================================

const gfx_mouse_state_t* gfx_get_mouse(void) {
    return &g_mouse;
}

const gfx_keyboard_state_t* gfx_get_keyboard(void) {
    return &g_keyboard;
}

void gfx_set_mouse_position(int32_t x, int32_t y) {
    g_mouse.x = x;
    g_mouse.y = y;

    // Clamp to bounds
    if (g_mouse.x < g_mouse_min_x) g_mouse.x = g_mouse_min_x;
    if (g_mouse.x > g_mouse_max_x) g_mouse.x = g_mouse_max_x;
    if (g_mouse.y < g_mouse_min_y) g_mouse.y = g_mouse_min_y;
    if (g_mouse.y > g_mouse_max_y) g_mouse.y = g_mouse_max_y;
}

void gfx_set_mouse_bounds(int32_t min_x, int32_t min_y, int32_t max_x, int32_t max_y) {
    if (max_x < min_x) {
        int32_t tmp = min_x;
        min_x = max_x;
        max_x = tmp;
    }
    if (max_y < min_y) {
        int32_t tmp = min_y;
        min_y = max_y;
        max_y = tmp;
    }

    g_mouse_min_x = min_x;
    g_mouse_min_y = min_y;
    g_mouse_max_x = max_x;
    g_mouse_max_y = max_y;

    // Re-clamp current position
    if (g_mouse.x < g_mouse_min_x) g_mouse.x = g_mouse_min_x;
    if (g_mouse.x > g_mouse_max_x) g_mouse.x = g_mouse_max_x;
    if (g_mouse.y < g_mouse_min_y) g_mouse.y = g_mouse_min_y;
    if (g_mouse.y > g_mouse_max_y) g_mouse.y = g_mouse_max_y;
}

// ============================================================================
// Keyboard Helpers
// ============================================================================

bool gfx_key_down(uint16_t key_code) {
    if (key_code >= 256) return false;
    return g_keyboard.keys[key_code];
}

bool gfx_key_pressed(uint16_t key_code) {
    if (key_code >= 256) return false;
    return g_keyboard.keys[key_code] && !g_keys_prev[key_code];
}

char gfx_get_typed_char(void) {
    return g_keyboard.last_char;
}

char gfx_key_to_char(uint16_t key_code, bool shift) {
    if (key_code >= 128) return 0;
    return shift ? scancode_to_ascii_upper[key_code] : scancode_to_ascii_lower[key_code];
}

// ============================================================================
// Mouse Helpers
// ============================================================================

bool gfx_mouse_left_down(void) {
    return g_mouse.left;
}

bool gfx_mouse_left_clicked(void) {
    return g_mouse.left_click;
}

bool gfx_mouse_left_released(void) {
    return g_mouse.left_release;
}

bool gfx_mouse_right_down(void) {
    return g_mouse.right;
}

bool gfx_mouse_right_clicked(void) {
    return g_mouse.right_click;
}

int32_t gfx_mouse_x(void) {
    return g_mouse.x;
}

int32_t gfx_mouse_y(void) {
    return g_mouse.y;
}

bool gfx_mouse_in_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    return g_mouse.x >= x && g_mouse.x < x + w &&
           g_mouse.y >= y && g_mouse.y < y + h;
}

bool gfx_mouse_in_circle(int32_t cx, int32_t cy, int32_t radius) {
    int32_t dx = g_mouse.x - cx;
    int32_t dy = g_mouse.y - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

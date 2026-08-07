#include "include/tty.h"

#include "include/graphics/graphics_manager.h"
#include "include/graphics/font_renderer.h"
#include "include/graphics/tty_font_renderer.h"
#include "include/graphics/graphics_types.h"
#include "include/graphics/window_manager.h"
#include "include/graphics_init.h"
#include "include/debuglog.h"
#include "include/libc/stdio.h"
#include "include/splash.h"
#include "include/vfs.h"
#include "include/task.h"
#include "include/framebuffer.h"
#include "include/tty_render.h"
#include "tty_internal.h"
#include "include/sound_pcspeaker.h"

// Cross-architecture console fallback (used when x86 graphics unavailable)
#include "arch/console.h"

// Direct framebuffer crash screen functions
static void crash_draw_char(int x, int y, char c, uint32_t color);
static void crash_draw_string(int x, int y, const char* str, uint32_t color);
static void crash_clear_screen(uint32_t color);
static void crash_draw_hex(int x, int y, uint64_t value, int digits, uint32_t color);

// Cross-architecture console flush (forward declaration)
static void tty_flush_screen_arch_console(void);
#include "include/string.h"
#include "include/memory.h"
#include "include/mm.h"

typedef enum {
    TTY_BACKEND_FRAMEBUFFER = 0,
    TTY_BACKEND_ARCH_CONSOLE,   // Cross-arch fallback (framebuffer or serial)
} tty_backend_t;

typedef struct {
    char ch;
    uint8_t attr;
    uint8_t dirty;  // 1 if cell needs redraw, 0 if clean
} tty_cell_t;

typedef struct {
    tty_cell_t* cells;
    size_t cell_count;
    uint16_t cols;
    uint16_t rows;
    uint16_t cursor_x;
    uint16_t cursor_y;
    uint16_t saved_cursor_x;
    uint16_t saved_cursor_y;
    uint8_t fg;
    uint8_t bg;
    bool bold;
    bool faint;
    bool underline;
    bool blink;
    bool inverse;
    bool conceal;
    bool italic;
    bool strike;
    bool double_underline;
    bool overlined;
    bool framed;
    bool encircled;
    bool crossed_out;
    graphics_color_t true_fg;
    graphics_color_t true_bg;
    bool use_true_colors;
    bool initialized;
} vt_buffer_t;

#define TTY_VT_COUNT 24
#define TTY_FIRST_TTY_VT 3
#define TTY_LAST_TTY_VT 24

static vt_buffer_t g_vt_buffers[TTY_VT_COUNT];
static uint8_t g_current_vt = 1;
static bool g_vt_buffers_initialized = false;

static struct {
    tty_backend_t backend;
    uint16_t cols;
    uint16_t rows;
    uint16_t char_width;
    uint16_t char_height;
    uint16_t cursor_x;
    uint16_t cursor_y;
    uint8_t fg;
    uint8_t bg;
    bool bold;
    bool faint;
    bool underline;
    bool blink;
    bool inverse;
    bool conceal;
    bool italic;
    bool strike;
    bool double_underline;
    bool overlined;
    bool framed;
    bool encircled;
    bool crossed_out;
    graphics_color_t true_fg;
    graphics_color_t true_bg;
    bool use_true_colors;
    bool cursor_visible;
    uint16_t saved_cursor_x;
    uint16_t saved_cursor_y;
    bool initialized;
    bool boot_mode;     // When true, bypass framebuffer TTY for fast VGA text mode
    bool graphics_app_active;  // When true, suppress TTY output (graphical app owns the display)
    bool panic_lockdown;  // One-way latch: blocks normal runtime mutations during panic
    tty_cell_t* cells;
    size_t cell_count;
    bool alternate_screen_active;
    tty_cell_t* alternate_cells;
    size_t alternate_cell_count;
    uint16_t alternate_cursor_x;
    uint16_t alternate_cursor_y;
    uint16_t alternate_saved_cursor_x;
    uint16_t alternate_saved_cursor_y;
    uint8_t alternate_fg;
    uint8_t alternate_bg;
    bool alternate_bold;
    bool alternate_faint;
    bool alternate_underline;
    bool alternate_blink;
    bool alternate_inverse;
    bool alternate_conceal;
    bool alternate_italic;
    bool alternate_strike;
    bool alternate_double_underline;
    bool alternate_overlined;
    bool alternate_framed;
    bool alternate_encircled;
    bool alternate_crossed_out;
    graphics_color_t alternate_true_fg;
    graphics_color_t alternate_true_bg;
    bool alternate_use_true_colors;
    int mouse_mode;
    char window_title[256];
    uint16_t scroll_top;
    uint16_t scroll_bottom;
} tty_state = {
    .backend = TTY_BACKEND_FRAMEBUFFER,
    .cols = 80,
    .rows = 25,
    .char_width = 8,
    .char_height = 8,  // Match 8x8 bitmap font
    .cursor_x = 0,
    .cursor_y = 0,
    .fg = TEXT_ATTR_LIGHT_GRAY,
    .bg = TEXT_ATTR_BLACK,
    .bold = false,
    .faint = false,
    .underline = false,
    .blink = false,
    .inverse = false,
    .conceal = false,
    .italic = false,
    .strike = false,
    .double_underline = false,
    .overlined = false,
    .framed = false,
    .encircled = false,
    .crossed_out = false,
    .true_fg = {170, 170, 170, 255},
    .true_bg = {0, 0, 0, 255},
    .use_true_colors = false,
    .cursor_visible = true,
    .saved_cursor_x = 0,
    .saved_cursor_y = 0,
    .initialized = false,
    .boot_mode = true,  // Start in boot mode for fast VGA text output
    .panic_lockdown = false,
    .cells = NULL,
    .cell_count = 0,
    .alternate_screen_active = false,
    .alternate_cells = NULL,
    .alternate_cell_count = 0,
    .alternate_cursor_x = 0,
    .alternate_cursor_y = 0,
    .alternate_saved_cursor_x = 0,
    .alternate_saved_cursor_y = 0,
    .alternate_fg = TEXT_ATTR_LIGHT_GRAY,
    .alternate_bg = TEXT_ATTR_BLACK,
    .alternate_bold = false,
    .alternate_faint = false,
    .alternate_underline = false,
    .alternate_blink = false,
    .alternate_inverse = false,
    .alternate_conceal = false,
    .alternate_italic = false,
    .alternate_strike = false,
    .alternate_double_underline = false,
    .alternate_overlined = false,
    .alternate_framed = false,
    .alternate_encircled = false,
    .alternate_crossed_out = false,
    .alternate_true_fg = {170, 170, 170, 255},
    .alternate_true_bg = {0, 0, 0, 255},
    .alternate_use_true_colors = false,
    .mouse_mode = 0,
    .window_title = {0},
    .scroll_top = 0,
    .scroll_bottom = 0,
};

typedef enum {
    ANSI_STATE_NORMAL = 0,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
    ANSI_STATE_OSC,
    ANSI_STATE_DCS,
    ANSI_STATE_STRING
} ansi_state_t;

static struct {
    ansi_state_t state;
    int params[16];
    size_t param_count;
    bool param_in_progress;
    bool private_mode;
    char string_buffer[256];
    size_t string_length;
    char final_char;
    bool application_mode;
    bool bracketed_paste_mode;
    int osc_param;
    bool osc_param_parsed;
    bool has_dollar;
} ansi_parser = {
    .state = ANSI_STATE_NORMAL,
    .params = {0},
    .param_count = 0,
    .param_in_progress = false,
    .private_mode = false,
    .string_buffer = {0},
    .string_length = 0,
    .final_char = 0,
    .application_mode = false,
    .bracketed_paste_mode = false,
    .osc_param = 0,
    .osc_param_parsed = false,
    .has_dollar = false,
};

// Extended 256-color palette
static graphics_color_t tty_palette_256[256];
static bool palette_initialized = false;

// Software cursor tracking (for devices without hardware cursor support)
static bool cursor_drawn = false;
static uint16_t cursor_drawn_x = 0;
static uint16_t cursor_drawn_y = 0;

#define TTY_RESPONSE_BUF_SIZE 64
static char tty_response_buf[TTY_RESPONSE_BUF_SIZE];
static volatile uint32_t tty_response_head = 0;
static volatile uint32_t tty_response_tail = 0;

// VT transition fade state
#define VT_TRANSITION_FRAMES 6
static bool g_vt_transition_active = false;
static uint8_t g_vt_transition_frame = 0;

// Runtime feature toggles (default to advanced Unix-like behavior enabled).
static bool g_tty_advanced_mode = true;
static bool g_tty_ansi_processing_enabled = true;
static bool g_tty_colors_enabled = true;
static bool g_tty_blink_enabled = true;
static bool g_tty_blink_phase_visible = true;
static uint32_t g_tty_blink_tick_accum = 0;

/* Cursor blink state — separate from text blink so cursor can be reset on keypress */
static bool g_cursor_blink_phase = true;   /* true = cursor visible */
static uint32_t g_cursor_blink_tick_accum = 0;

/* Status bar / scroll indicator are expensive to repaint (gradient fill,
 * scaled logo blit, CPU-dot circles, several text draws) but their content
 * (clock, CPU load, modifier keys) only changes a few times a second. Every
 * scroll/insert/delete used to force a full repaint via tty_flush_screen(),
 * which is what made scrolling feel laggy. Gate the repaint behind this
 * flag and only refresh it from the 1 Hz cursor-blink tick or on an
 * explicit state change, instead of on every flush. */
static bool g_status_bar_chrome_dirty = true;

static inline bool tty_runtime_mutation_allowed(void) {
    return !tty_state.panic_lockdown;
}

void tty_enter_panic_lockdown(void) {
    // One-way transition: panic handling must be immune to concurrent runtime writes.
    tty_state.panic_lockdown = true;
    __asm__ volatile("mfence" ::: "memory");
}

bool tty_is_panic_lockdown(void) {
    return tty_state.panic_lockdown;
}

// Forward declaration needed by runtime blink API.
static void tty_flush_screen(void);

void tty_set_runtime_options(const tty_runtime_options_t* options) {
    if (!options || !tty_runtime_mutation_allowed()) {
        return;
    }

    g_tty_advanced_mode = options->advanced_mode;
    g_tty_ansi_processing_enabled = options->ansi_processing_enabled;
    g_tty_colors_enabled = options->colors_enabled;
    g_tty_blink_enabled = options->blink_enabled;
    tty_set_status_bar_visible(options->status_bar_enabled);
    if (tty_is_ready()) {
        tty_force_redraw();
    }
}

void tty_get_runtime_options(tty_runtime_options_t* out_options) {
    if (!out_options) {
        return;
    }
    out_options->advanced_mode = g_tty_advanced_mode;
    out_options->ansi_processing_enabled = g_tty_ansi_processing_enabled;
    out_options->colors_enabled = g_tty_colors_enabled;
    out_options->blink_enabled = g_tty_blink_enabled;
    out_options->status_bar_enabled = tty_is_status_bar_visible();
}

void tty_set_advanced_mode(bool enabled) {
    g_tty_advanced_mode = enabled;
    if (!enabled) {
        g_tty_ansi_processing_enabled = false;
        g_tty_colors_enabled = false;
        g_tty_blink_enabled = false;
        tty_state.blink = false;
    }
    if (tty_is_ready()) {
        tty_force_redraw();
    }
}

bool tty_is_advanced_mode_enabled(void) {
    return g_tty_advanced_mode;
}

void tty_set_ansi_processing_enabled(bool enabled) {
    g_tty_ansi_processing_enabled = enabled;
}

bool tty_is_ansi_processing_enabled(void) {
    return g_tty_ansi_processing_enabled;
}

void tty_set_colors_enabled(bool enabled) {
    g_tty_colors_enabled = enabled;
    if (!enabled) {
        tty_state.fg = TEXT_ATTR_LIGHT_GRAY;
        tty_state.bg = TEXT_ATTR_BLACK;
        tty_state.use_true_colors = false;
    }
    if (tty_is_ready()) {
        tty_force_redraw();
    }
}

bool tty_are_colors_enabled(void) {
    return g_tty_colors_enabled;
}

void tty_set_blink_enabled(bool enabled) {
    g_tty_blink_enabled = enabled;
    if (!enabled) {
        tty_state.blink = false;
        g_tty_blink_phase_visible = true;
    }
    if (tty_is_ready()) {
        tty_force_redraw();
    }
}

bool tty_is_blink_enabled(void) {
    return g_tty_blink_enabled;
}

// Login status tracking. g_login_status / g_current_user are shared with
// tty_render.c (declared extern via tty_internal.h): the status-bar drawing
// code there reads both, and the logged-out path in
// tty_set_status_bar_user_logged_in() clears g_current_user directly.
char g_login_status[TTY_INTERNAL_LOGIN_STATUS_LEN] = "Logging in...";
char g_current_user[TTY_INTERNAL_CURRENT_USER_LEN] = "";

void tty_set_login_status_text(const char* text) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    const char* src = text ? text : "";
    strncpy(g_login_status, src, sizeof(g_login_status) - 1);
    g_login_status[sizeof(g_login_status) - 1] = '\0';
    if (tty_is_status_bar_visible() && tty_is_ready()) {
        tty_draw_status_bar();
    }
}

void tty_set_current_user_text(const char* text) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    const char* src = text ? text : "";
    strncpy(g_current_user, src, sizeof(g_current_user) - 1);
    g_current_user[sizeof(g_current_user) - 1] = '\0';
    if (tty_is_status_bar_visible() && tty_is_ready()) {
        tty_draw_status_bar();
    }
}

void tty_soft_blink_tick(void) {
    if (!tty_state.initialized || !tty_state.cells || !g_tty_advanced_mode) {
        return;
    }

    /* Cursor blink: 50 ticks ≈ 500 ms on/off at 100 Hz → 1 Hz blink */
    g_cursor_blink_tick_accum++;
    if (g_cursor_blink_tick_accum >= 50) {
        g_cursor_blink_tick_accum = 0;
        g_cursor_blink_phase = !g_cursor_blink_phase;
        /* Mark cursor cell dirty so flush redraws it */
        if (tty_state.cursor_visible &&
            tty_state.cursor_x < tty_state.cols &&
            tty_state.cursor_y < tty_state.rows) {
            size_t cidx = (size_t)tty_state.cursor_y * tty_state.cols + tty_state.cursor_x;
            tty_state.cells[cidx].dirty = 1;
        }
        /* 1 Hz cadence is the right refresh rate for the clock/CPU dots too */
        g_status_bar_chrome_dirty = true;
        tty_flush_screen();
    }

    if (!g_tty_blink_enabled) {
        return;
    }

    /* Text blink (TEXT_ATTR_BLINK characters): same 25-tick rate as before */
    g_tty_blink_tick_accum++;
    if (g_tty_blink_tick_accum < 25) {
        return;
    }
    g_tty_blink_tick_accum = 0;
    g_tty_blink_phase_visible = !g_tty_blink_phase_visible;

    for (size_t i = 0; i < tty_state.cell_count; i++) {
        if (tty_state.cells[i].attr & TEXT_ATTR_BLINK) {
            tty_state.cells[i].dirty = 1;
        }
    }
    tty_flush_screen();
}

// Compatibility aliases expected by session/login code paths. Only the
// login-status/current-user text aliases live here (they call setters that
// stay in tty.c); the status-text and logged-in aliases are defined in
// tty_render.c since tty_set_status_bar_status_text() /
// tty_set_status_bar_user_logged_in() moved there.
void tty_set_login_status(const char* text) { tty_set_login_status_text(text); }
void tty_status_set_current_user(const char* username) { tty_set_current_user_text(username); }
void tty_set_status_user(const char* username) { tty_set_current_user_text(username); }
void tty_status_clear_current_user(void) { tty_set_current_user_text(""); }
void tty_clear_status_user(void) { tty_set_current_user_text(""); }

static inline void tty_response_push(char c) {
    uint32_t next = (tty_response_head + 1) % TTY_RESPONSE_BUF_SIZE;
    if (next != tty_response_tail) {
        tty_response_buf[tty_response_head] = c;
        tty_response_head = next;
    }
}

static inline void tty_response_push_str(const char* s) {
    while (*s) {
        tty_response_push(*s++);
    }
}

bool tty_read_response(char* buf, uint32_t max_len) {
    uint32_t count = 0;
    while (tty_response_tail != tty_response_head && count < max_len) {
        buf[count++] = tty_response_buf[tty_response_tail];
        tty_response_tail = (tty_response_tail + 1) % TTY_RESPONSE_BUF_SIZE;
    }
    return count > 0;
}

static void tty_init_256_palette(void) {
    if (palette_initialized) return;
    
    // Standard 16 colors (0-15)
    tty_palette_256[0]  = (graphics_color_t){0, 0, 0, 255};         // Black
    tty_palette_256[1]  = (graphics_color_t){128, 0, 0, 255};       // Dark Red
    tty_palette_256[2]  = (graphics_color_t){0, 128, 0, 255};       // Dark Green
    tty_palette_256[3]  = (graphics_color_t){128, 128, 0, 255};     // Dark Yellow
    tty_palette_256[4]  = (graphics_color_t){0, 0, 128, 255};       // Dark Blue
    tty_palette_256[5]  = (graphics_color_t){128, 0, 128, 255};     // Dark Magenta
    tty_palette_256[6]  = (graphics_color_t){0, 128, 128, 255};     // Dark Cyan
    tty_palette_256[7]  = (graphics_color_t){192, 192, 192, 255};   // Light Gray
    tty_palette_256[8]  = (graphics_color_t){128, 128, 128, 255};   // Dark Gray
    tty_palette_256[9]  = (graphics_color_t){255, 0, 0, 255};       // Bright Red
    tty_palette_256[10] = (graphics_color_t){0, 255, 0, 255};       // Bright Green
    tty_palette_256[11] = (graphics_color_t){255, 255, 0, 255};     // Bright Yellow
    tty_palette_256[12] = (graphics_color_t){0, 0, 255, 255};       // Bright Blue
    tty_palette_256[13] = (graphics_color_t){255, 0, 255, 255};     // Bright Magenta
    tty_palette_256[14] = (graphics_color_t){0, 255, 255, 255};     // Bright Cyan
    tty_palette_256[15] = (graphics_color_t){255, 255, 255, 255};   // White
    
    // 6x6x6 color cube (16-231)
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) % 6;
        int g = (i / 6) % 6;
        int b = i % 6;
        tty_palette_256[16 + i] = (graphics_color_t){
            .r = (uint8_t)(r ? 55 + r * 40 : 0),
            .g = (uint8_t)(g ? 55 + g * 40 : 0),
            .b = (uint8_t)(b ? 55 + b * 40 : 0),
            .a = 255
        };
    }
    
    // Grayscale ramp (232-255)
    for (int i = 0; i < 24; i++) {
        uint8_t level = (uint8_t)(8 + i * 10);
        tty_palette_256[232 + i] = (graphics_color_t){level, level, level, 255};
    }
    
    palette_initialized = true;
}

static graphics_color_t tty_color_from_nibble(uint8_t nibble) {
    if (!palette_initialized) {
        tty_init_256_palette();
    }
    return tty_palette_256[nibble & 0x0F];
}

static graphics_color_t __attribute__((unused)) tty_color_from_256(uint8_t index) {
    if (!palette_initialized) {
        tty_init_256_palette();
    }
    return tty_palette_256[index];
}

static uint8_t tty_current_attr(void);

static uint8_t tty_palette_best_match(uint8_t r, uint8_t g, uint8_t b, bool allow_bright) {
    if (!palette_initialized) {
        tty_init_256_palette();
    }
    
    uint32_t best_error = UINT32_MAX;
    uint8_t best_index = 7; // default to light gray
    uint8_t limit = allow_bright ? 16 : 8;

    for (uint8_t i = 0; i < limit; i++) {
        int32_t dr = (int32_t)r - (int32_t)tty_palette_256[i].r;
        int32_t dg = (int32_t)g - (int32_t)tty_palette_256[i].g;
        int32_t db = (int32_t)b - (int32_t)tty_palette_256[i].b;
        uint32_t error = (uint32_t)(dr * dr + dg * dg + db * db);
        if (error < best_error) {
            best_error = error;
            best_index = i;
        }
    }

    return best_index;
}

static uint8_t tty_map_rgb_to_attr(uint8_t r, uint8_t g, uint8_t b, bool is_background) {
    uint8_t match = tty_palette_best_match(r, g, b, true);
    if (is_background) {
        match &= 0x07; // background plane supports only base colors
    }
    return match;
}

static uint8_t tty_map_256_color(uint8_t idx, bool is_background) {
    (void)idx; (void)is_background;
    if (idx < 16) {
        return is_background ? (idx & 0x07) : (idx & 0x0F);
    }

    uint8_t r, g, b;
    if (idx >= 16 && idx <= 231) {
        uint8_t cube = (uint8_t)(idx - 16);
        r = (uint8_t)((cube / 36) % 6 * 51);
        g = (uint8_t)((cube / 6) % 6 * 51);
        b = (uint8_t)(cube % 6 * 51);
    } else {
        uint8_t gray = (uint8_t)(8 + (idx - 232) * 10);
        r = g = b = gray;
    }

    return tty_map_rgb_to_attr(r, g, b, is_background);
}

/* Compute the terminal geometry implied by the *current* graphics mode,
 * without mutating any tty_state. Shared by tty_update_dimensions_from_graphics()
 * (boot-time init, no cell buffer exists yet) and tty_handle_display_mode_change()
 * (runtime resolution switch, where the old cols/rows must still be intact
 * when tty_set_dimensions() is called so it can copy/reflow existing content). */
static bool tty_compute_dimensions_from_graphics(uint16_t* out_cols, uint16_t* out_rows,
                                                   uint16_t* out_char_w, uint16_t* out_char_h) {
    video_mode_t mode;
    if (graphics_get_current_mode(&mode) != GRAPHICS_SUCCESS) {
        return false;
    }

    // Subtract status bar height from available height
    uint32_t available_height = mode.height;
    if (available_height > TTY_STATUS_BAR_HEIGHT) {
        available_height -= TTY_STATUS_BAR_HEIGHT;
    }

    // Always derive terminal dimensions from framebuffer mode using 8x8 font metrics
    uint16_t char_w = 8;
    uint16_t char_h = 8;  // Fixed: Changed from 16 to 8 to match 8x8 font
    tty_font_t* tty_font = NULL;
    if (tty_font_load_builtin("tty-8x8", 8, &tty_font) == TTY_FONT_SUCCESS && tty_font) {
        char_w = tty_font->width;
        char_h = tty_font->height;
    }

    if (char_w == 0 || char_h == 0) {
        return false; // Avoid division by zero
    }

    uint16_t cols = (uint16_t)(mode.width / char_w);
    uint16_t rows = (uint16_t)(available_height / char_h);

    // Sanity check for reasonable terminal dimensions
    if (cols == 0 || rows == 0 || cols > 200 || rows > 200) {
        return false;
    }

    *out_cols = cols;
    *out_rows = rows;
    *out_char_w = char_w;
    *out_char_h = char_h;
    return true;
}

static void tty_update_dimensions_from_graphics(void) {
    uint16_t cols, rows, char_w, char_h;
    if (tty_compute_dimensions_from_graphics(&cols, &rows, &char_w, &char_h)) {
        tty_state.cols = cols;
        tty_state.rows = rows;
        tty_state.char_width = char_w;
        tty_state.char_height = char_h;
    }
}

static void tty_render_cell(uint16_t x, uint16_t y, char ch, uint8_t attr);
static void tty_update_cursor_visual(void);
static void tty_flush_screen(void);

static void tty_apply_cursor(void) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    graphics_set_cursor_pos(tty_state.cursor_x, tty_state.cursor_y);
    /* Reset blink so cursor is always visible right after it moves (feels responsive) */
    g_cursor_blink_phase = true;
    g_cursor_blink_tick_accum = 0;
    tty_update_cursor_visual();
}

static inline size_t tty_cell_index(uint16_t x, uint16_t y) {
    return (size_t)y * (size_t)tty_state.cols + x;
}

static void tty_redraw_cell_at(uint16_t x, uint16_t y) {
    if (!tty_state.cells || x >= tty_state.cols || y >= tty_state.rows) {
        return;
    }
    size_t idx = tty_cell_index(x, y);
    tty_cell_t cell = tty_state.cells[idx];
    tty_render_cell(x, y, cell.ch, cell.attr);
}

static void tty_update_cursor_visual(void) {
    if (!tty_state.initialized || !tty_state.cells) {
        return;
    }

    // Restore previous cursor cell if needed
    if (cursor_drawn) {
        tty_redraw_cell_at(cursor_drawn_x, cursor_drawn_y);
        cursor_drawn = false;
    }

    if (!tty_state.cursor_visible ||
        !g_cursor_blink_phase ||
        tty_state.cursor_x >= tty_state.cols ||
        tty_state.cursor_y >= tty_state.rows) {
        return;
    }

    size_t idx = tty_cell_index(tty_state.cursor_x, tty_state.cursor_y);
    tty_cell_t cell = tty_state.cells[idx];
    uint8_t fg = cell.attr & 0x0F;
    uint8_t bg = (cell.attr >> 4) & 0x0F;
    uint8_t block_attr = (fg << 4) | bg; /* swap fg/bg: solid block cursor */
    tty_render_cell(tty_state.cursor_x, tty_state.cursor_y, cell.ch ? cell.ch : ' ', block_attr);

    cursor_drawn_x = tty_state.cursor_x;
    cursor_drawn_y = tty_state.cursor_y;
    cursor_drawn = true;
}

// =============================================================================
// CRASH SCREEN FUNCTIONS - Direct framebuffer access bypassing graphics subsystem
// =============================================================================

// Simple 8x16 bitmap font for crash screen (only essential ASCII characters)
const uint16_t crash_font[95][16] = {
    // Space (32)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ! (33)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0000, 0x0000, 0x0800, 0x0800, 0x0000, 0x0000},
    // " (34)
    {0x0000, 0x0000, 0x0000, 0x2400, 0x2400, 0x2400, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // # (35)
    {0x0000, 0x0000, 0x0000, 0x2400, 0x2400, 0x7E00, 0x2400, 0x2400,
     0x2400, 0x7E00, 0x2400, 0x2400, 0x0000, 0x0000, 0x0000, 0x0000},
    // $ (36)
    {0x0000, 0x0000, 0x0800, 0x1C00, 0x2A00, 0x2800, 0x1C00, 0x0A00,
     0x0A00, 0x2800, 0x2A00, 0x1C00, 0x0800, 0x0000, 0x0000, 0x0000},
    // % (37)
    {0x0000, 0x0000, 0x0000, 0x6200, 0x9200, 0x6400, 0x0800, 0x1000,
     0x2600, 0x4900, 0x4600, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // & (38)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200, 0x1C00, 0x1D00,
     0x2500, 0x2200, 0x1D00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ' (39)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x0800, 0x0800, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ( (40)
    {0x0000, 0x0000, 0x0200, 0x0400, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0400, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000},
    // ) (41)
    {0x0000, 0x0000, 0x0800, 0x0400, 0x0200, 0x0200, 0x0200, 0x0200,
     0x0200, 0x0200, 0x0400, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000},
    // * (42)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x2A00, 0x1C00, 0x2A00,
     0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // + (43)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x0800, 0x3E00, 0x0800,
     0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // , (44)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0800, 0x0800, 0x1000, 0x0000, 0x0000, 0x0000},
    // - (45)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x3E00, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // . (46)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000},
    // / (47)
    {0x0000, 0x0000, 0x0200, 0x0200, 0x0400, 0x0400, 0x0800, 0x0800,
     0x1000, 0x1000, 0x2000, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 0 (48)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x3200, 0x2A00, 0x2600,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 1 (49)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x1800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 2 (50)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x0200, 0x0400, 0x0800,
     0x1000, 0x2000, 0x3E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 3 (51)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x0200, 0x0C00, 0x0200,
     0x0200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 4 (52)
    {0x0000, 0x0000, 0x0000, 0x0400, 0x0C00, 0x1400, 0x2400, 0x4400,
     0x3E00, 0x0400, 0x0400, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 5 (53)
    {0x0000, 0x0000, 0x0000, 0x3E00, 0x2000, 0x3C00, 0x0200, 0x0200,
     0x0200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 6 (54)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2000, 0x3C00, 0x2200, 0x2200,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 7 (55)
    {0x0000, 0x0000, 0x0000, 0x3E00, 0x0200, 0x0400, 0x0800, 0x0800,
     0x1000, 0x1000, 0x1000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 8 (56)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200, 0x1C00, 0x1C00,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // 9 (57)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200, 0x2200, 0x1E00,
     0x0200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // : (58)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x0800, 0x0000, 0x0000,
     0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ; (59)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0800, 0x0800, 0x0000, 0x0000,
     0x0800, 0x0800, 0x1000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // < (60)
    {0x0000, 0x0000, 0x0000, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000,
     0x1000, 0x0800, 0x0400, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000},
    // = (61)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x3E00, 0x0000, 0x3E00,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // > (62)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080,
     0x0100, 0x0200, 0x0400, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000},
    // ? (63)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x0200, 0x0400, 0x0800,
     0x0800, 0x0000, 0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000},
    // @ (64)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2E00, 0x2A00, 0x2E00,
     0x2A00, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // A (65)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x1400, 0x2200, 0x2200, 0x2200,
     0x3E00, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // B (66)
    {0x0000, 0x0000, 0x0000, 0x3C00, 0x2200, 0x2200, 0x3C00, 0x2200,
     0x2200, 0x2200, 0x3C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // C (67)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2000, 0x2000, 0x2000,
     0x2000, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // D (68)
    {0x0000, 0x0000, 0x0000, 0x3800, 0x2400, 0x2200, 0x2200, 0x2200,
     0x2200, 0x2400, 0x3800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // E (69)
    {0x0000, 0x0000, 0x0000, 0x3E00, 0x2000, 0x2000, 0x3C00, 0x2000,
     0x2000, 0x2000, 0x3E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // F (70)
    {0x0000, 0x0000, 0x0000, 0x3E00, 0x2000, 0x2000, 0x3C00, 0x2000,
     0x2000, 0x2000, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // G (71)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2000, 0x2000, 0x2E00,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // H (72)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200, 0x3E00, 0x2200,
     0x2200, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // I (73)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // J (74)
    {0x0000, 0x0000, 0x0000, 0x0E00, 0x0400, 0x0400, 0x0400, 0x0400,
     0x2400, 0x2400, 0x1800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // K (75)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2400, 0x2800, 0x3000, 0x2800,
     0x2400, 0x2200, 0x2100, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // L (76)
    {0x0000, 0x0000, 0x0000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000,
     0x2000, 0x2000, 0x3E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // M (77)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x3600, 0x2A00, 0x2A00, 0x2200,
     0x2200, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // N (78)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x3200, 0x2A00, 0x2600, 0x2200,
     0x2200, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // O (79)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200, 0x2200, 0x2200,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // P (80)
    {0x0000, 0x0000, 0x0000, 0x3C00, 0x2200, 0x2200, 0x2200, 0x3C00,
     0x2000, 0x2000, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // Q (81)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200, 0x2200, 0x2200,
     0x2600, 0x2200, 0x1D00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // R (82)
    {0x0000, 0x0000, 0x0000, 0x3C00, 0x2200, 0x2200, 0x2200, 0x3C00,
     0x2400, 0x2200, 0x2100, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // S (83)
    {0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2000, 0x1C00, 0x0200,
     0x0200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // T (84)
    {0x0000, 0x0000, 0x0000, 0x3E00, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // U (85)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200, 0x2200, 0x2200,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // V (86)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200, 0x2200, 0x2200,
     0x2200, 0x1400, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // W (87)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200, 0x2200, 0x2A00,
     0x2A00, 0x3600, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // X (88)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x1400, 0x0800, 0x0800,
     0x1400, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // Y (89)
    {0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200, 0x1400, 0x0800,
     0x0800, 0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // Z (90)
    {0x0000, 0x0000, 0x0000, 0x3E00, 0x0200, 0x0400, 0x0800, 0x1000,
     0x2000, 0x2000, 0x3E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // [ (91)
    {0x0000, 0x0000, 0x0E00, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // \ (92)
    {0x0000, 0x0000, 0x2000, 0x2000, 0x1000, 0x1000, 0x0800, 0x0800,
     0x0400, 0x0400, 0x0200, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000},
    // ] (93)
    {0x0000, 0x0000, 0x0E00, 0x0200, 0x0200, 0x0200, 0x0200, 0x0200,
     0x0200, 0x0200, 0x0E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ^ (94)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x1400, 0x2200, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // _ (95)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x3E00, 0x0000, 0x0000, 0x0000, 0x0000},
    // ` (96)
    {0x0000, 0x0000, 0x0000, 0x1000, 0x0800, 0x0400, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // a (97)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1C00, 0x0200, 0x1E00,
     0x2200, 0x2200, 0x1E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // b (98)
    {0x0000, 0x0000, 0x2000, 0x2000, 0x2C00, 0x3200, 0x2200, 0x2200,
     0x2200, 0x3200, 0x2C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // c (99)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2000,
     0x2000, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // d (100)
    {0x0000, 0x0000, 0x0200, 0x0200, 0x1A00, 0x2600, 0x2200, 0x2200,
     0x2200, 0x2600, 0x1A00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // e (101)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200,
     0x3E00, 0x2000, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // f (102)
    {0x0000, 0x0000, 0x0600, 0x0800, 0x0800, 0x1C00, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // g (103)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1E00, 0x2200, 0x2200,
     0x2200, 0x1E00, 0x0200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000},
    // h (104)
    {0x0000, 0x0000, 0x2000, 0x2000, 0x2C00, 0x3200, 0x2200, 0x2200,
     0x2200, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // i (105)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x0000, 0x1800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // j (106)
    {0x0000, 0x0000, 0x0000, 0x0400, 0x0000, 0x0C00, 0x0400, 0x0400,
     0x0400, 0x0400, 0x2400, 0x1800, 0x0000, 0x0000, 0x0000, 0x0000},
    // k (107)
    {0x0000, 0x0000, 0x2000, 0x2000, 0x2400, 0x2800, 0x3000, 0x2800,
     0x2400, 0x2200, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // l (108)
    {0x0000, 0x0000, 0x1800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // m (109)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2C00, 0x3200, 0x2200,
     0x2200, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // n (110)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2C00, 0x3200, 0x2200,
     0x2200, 0x2200, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // o (111)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1C00, 0x2200, 0x2200,
     0x2200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // p (112)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2C00, 0x3200, 0x2200,
     0x2200, 0x3200, 0x2C00, 0x2000, 0x2000, 0x0000, 0x0000, 0x0000},
    // q (113)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1A00, 0x2600, 0x2200,
     0x2200, 0x2600, 0x1A00, 0x0200, 0x0200, 0x0000, 0x0000, 0x0000},
    // r (114)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2E00, 0x3200, 0x2000,
     0x2000, 0x2000, 0x2000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // s (115)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1E00, 0x2000, 0x1C00,
     0x0200, 0x0200, 0x3C00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // t (116)
    {0x0000, 0x0000, 0x0000, 0x0800, 0x0800, 0x1C00, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0600, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // u (117)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200,
     0x2200, 0x2600, 0x1A00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // v (118)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200,
     0x2200, 0x1400, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // w (119)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200,
     0x2A00, 0x2A00, 0x3600, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // x (120)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2200, 0x1400, 0x0800,
     0x0800, 0x1400, 0x2200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // y (121)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2200, 0x2200, 0x2200,
     0x2200, 0x1E00, 0x0200, 0x2200, 0x1C00, 0x0000, 0x0000, 0x0000},
    // z (122)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x3E00, 0x0400, 0x0800,
     0x1000, 0x2000, 0x3E00, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // { (123)
    {0x0000, 0x0000, 0x0200, 0x0400, 0x0400, 0x0800, 0x1000, 0x0800,
     0x0400, 0x0400, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // | (124)
    {0x0000, 0x0000, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800,
     0x0800, 0x0800, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // } (125)
    {0x0000, 0x0000, 0x0800, 0x0400, 0x0400, 0x0200, 0x0100, 0x0200,
     0x0400, 0x0400, 0x0800, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ~ (126)
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1C00, 0x2200,
     0x0100, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}
};

static void crash_draw_char(int x, int y, char c, uint32_t color) {
    // Get framebuffer directly (bypass graphics subsystem)
    framebuffer_t* fb = NULL;
    if (graphics_map_framebuffer(&fb) != GRAPHICS_SUCCESS || !fb) {
        return;
    }
    if (!fb->virtual_addr) {
        graphics_unmap_framebuffer(fb);
        return;
    }

    int char_index = c - 32;
    if (char_index < 0 || char_index >= 96) {
        char_index = 0; // Use space for invalid chars
    }

    volatile uint8_t* framebuffer = (volatile uint8_t*)fb->virtual_addr;
    uint32_t bytes_per_pixel = (fb->bpp + 7) / 8;

    for (int cy = 0; cy < 16; cy++) {
        uint16_t row_bits = crash_font[char_index][cy];
        for (int cx = 0; cx < 8; cx++) {
            // Check if the bit is set in the font bitmap
            if (row_bits & (0x80 >> cx)) {
                int fb_x = x + cx;
                int fb_y = y + cy;

                if (fb_x >= 0 && fb_x < (int)fb->width && fb_y >= 0 && fb_y < (int)fb->height) {
                    size_t offset = (fb_y * fb->pitch) + (fb_x * bytes_per_pixel);
                    if (offset + bytes_per_pixel <= fb->size) {
                        // Write pixel based on bpp (handles 24bpp and 32bpp)
                        framebuffer[offset] = color & 0xFF;           // Blue
                        framebuffer[offset + 1] = (color >> 8) & 0xFF;  // Green
                        framebuffer[offset + 2] = (color >> 16) & 0xFF; // Red
                        if (bytes_per_pixel == 4) {
                            framebuffer[offset + 3] = (color >> 24) & 0xFF; // Alpha
                        }
                    }
                }
            }
        }
    }

    graphics_unmap_framebuffer(fb);
}

static void crash_draw_string(int x, int y, const char* str, uint32_t color) {
    if (!str) return;

    int current_x = x;
    while (*str) {
        crash_draw_char(current_x, y, *str, color);
        current_x += 8;
        str++;
    }
}

static void crash_clear_screen(uint32_t color) {
    framebuffer_t* fb = NULL;
    if (graphics_map_framebuffer(&fb) != GRAPHICS_SUCCESS || !fb) {
        return;
    }
    if (!fb->virtual_addr) {
        graphics_unmap_framebuffer(fb);
        return;
    }

    volatile uint8_t* framebuffer = (volatile uint8_t*)fb->virtual_addr;
    uint32_t bytes_per_pixel = (fb->bpp + 7) / 8;

    for (uint32_t y = 0; y < fb->height; y++) {
        volatile uint8_t* row = framebuffer + y * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t offset = x * bytes_per_pixel;
            row[offset] = color & 0xFF;              // Blue
            row[offset + 1] = (color >> 8) & 0xFF;   // Green
            row[offset + 2] = (color >> 16) & 0xFF;  // Red
            if (bytes_per_pixel == 4) {
                row[offset + 3] = (color >> 24) & 0xFF; // Alpha
            }
        }
    }

    graphics_unmap_framebuffer(fb);
}

static void crash_draw_hex(int x, int y, uint64_t value, int digits, uint32_t color) {
    char buffer[32];
    int len = 0;

    // Convert to hex string
    for (int i = digits - 1; i >= 0; i--) {
        uint8_t nibble = (value >> (i * 4)) & 0xF;
        buffer[len++] = nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buffer[len] = '\0';

    crash_draw_string(x, y, buffer, color);
}

// Public crash screen API
void tty_show_crash_screen(const char* title, const char* message, uint64_t eip, uint64_t error_code, uint64_t cr2) {
    tty_enter_panic_lockdown();

    // Clear screen with blue background
    crash_clear_screen(0xFF000080); // Blue background

    // Draw title in white
    crash_draw_string(10, 10, title, 0xFFFFFFFF);

    // Draw message in yellow
    crash_draw_string(10, 30, message, 0xFFFFFF00);

    // Draw register info
    crash_draw_string(10, 60, "EIP: 0x", 0xFFFFFFFF);
    crash_draw_hex(60, 60, eip, 16, 0xFFFF0000);

    crash_draw_string(10, 80, "Error Code: 0x", 0xFFFFFFFF);
    crash_draw_hex(120, 80, error_code, 8, 0xFFFF0000);

    crash_draw_string(10, 100, "CR2: 0x", 0xFFFFFFFF);
    crash_draw_hex(60, 100, cr2, 16, 0xFFFF0000);
}

static void tty_render_cell_framebuffer(uint16_t x, uint16_t y, char ch, uint8_t attr) {
    // Don't write TTY text to framebuffer when a userspace app (e.g. DM) owns it
    if (framebuffer_has_userspace_mapping()) {
        return;
    }
    // Use TTY font renderer
    tty_font_t* tty_font = NULL;
    if (tty_font_load_builtin("tty-8x8", 8, &tty_font) == TTY_FONT_SUCCESS && tty_font) {
        framebuffer_t* fb = graphics_get_framebuffer();
        if (!fb || !fb->virtual_addr) {
            return;
        }

        graphics_surface_t surface;
        surface.pixels = (void*)fb->virtual_addr;
        surface.width = fb->width;
        surface.height = fb->height;
        surface.pitch = fb->pitch;
        surface.format = fb->format;
        surface.bpp = fb->bpp;

        int32_t py_offset = TTY_STATUS_BAR_HEIGHT;
        int32_t px = (int32_t)x * tty_state.char_width;
        int32_t py = (int32_t)y * tty_state.char_height + py_offset;

        // Get colors
        graphics_color_t fg_c = tty_color_from_nibble(attr & 0x0F);
        graphics_color_t bg_c = tty_color_from_nibble((attr >> 4) & 0x0F);
        if ((attr & TEXT_ATTR_BLINK) && g_tty_blink_enabled && !g_tty_blink_phase_visible) {
            ch = ' ';
        }

        tty_font_render_char(tty_font, &surface, px, py, (uint32_t)ch, fg_c, bg_c);
        return;
    }

    // Fallback: Direct framebuffer rendering using crash_font bitmap
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr) {
        return;  // Silently fail - avoid log spam
    }

    int32_t py_offset = TTY_STATUS_BAR_HEIGHT;
    int32_t px = (int32_t)x * tty_state.char_width;
    int32_t py = (int32_t)y * tty_state.char_height + py_offset;

    // Bounds checking
    if (px < 0 || py < 0 || px + tty_state.char_width > (int32_t)fb->width || py + tty_state.char_height > (int32_t)fb->height) {
        return;
    }

    // Get colors as 32-bit packed values for fast rendering
    graphics_color_t fg_c = tty_color_from_nibble(attr & 0x0F);
    graphics_color_t bg_c = tty_color_from_nibble((attr >> 4) & 0x0F);
    if ((attr & TEXT_ATTR_BLINK) && g_tty_blink_enabled && !g_tty_blink_phase_visible) {
        ch = ' ';
    }
    uint32_t fg_color = (fg_c.a << 24) | (fg_c.r << 16) | (fg_c.g << 8) | fg_c.b;
    uint32_t bg_color = (bg_c.a << 24) | (bg_c.r << 16) | (bg_c.g << 8) | bg_c.b;

    // Get font glyph index (crash_font covers ASCII 32-126)
    int char_index = ch - 32;
    if (char_index < 0 || char_index >= 95) {
        char_index = 0; // Use space for invalid chars
    }

    // Calculate bytes per pixel from actual bpp
    uint32_t bytes_per_pixel = (fb->bpp + 7) / 8;
    volatile uint8_t* framebuffer = (volatile uint8_t*)fb->virtual_addr;

    // Render using actual font bitmap
    for (int32_t cy = 0; cy < tty_state.char_height; cy++) {
        uint16_t row_bits = crash_font[char_index][cy];
        size_t row_offset = ((uint32_t)(py + cy) * fb->pitch) + ((uint32_t)px * bytes_per_pixel);

        for (int32_t cx = 0; cx < tty_state.char_width; cx++) {
            // Check if bit is set in font bitmap (0x80 >> cx tests each bit from MSB)
            uint32_t pixel_color = (row_bits & (0x80 >> cx)) ? fg_color : bg_color;
            size_t pixel_offset = row_offset + (cx * bytes_per_pixel);
            
            // Write pixel based on bpp (handles 24bpp and 32bpp)
            framebuffer[pixel_offset] = pixel_color & 0xFF;         // Blue
            framebuffer[pixel_offset + 1] = (pixel_color >> 8) & 0xFF;  // Green
            framebuffer[pixel_offset + 2] = (pixel_color >> 16) & 0xFF; // Red
            if (bytes_per_pixel == 4) {
                framebuffer[pixel_offset + 3] = (pixel_color >> 24) & 0xFF; // Alpha (only for 32bpp)
            }
        }
    }
}

static void tty_render_cell(uint16_t x, uint16_t y, char ch, uint8_t attr) {
    // Cross-architecture console backend: skip framebuffer rendering entirely.
    // Output is handled in bulk by tty_flush_screen_arch_console().
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        return;
    }

    // Always use framebuffer rendering with 8x8 font to avoid TrueType corruption
    // The 8x8 bitmap font is more reliable for TTY output
    if (graphics_is_initialized()) {
        framebuffer_t* fb = graphics_get_framebuffer();
        if (fb && fb->virtual_addr) {
            tty_render_cell_framebuffer(x, y, ch, attr);
            return;
        }
    }
    // Fall back to graphics text mode if framebuffer not available
    graphics_write_char(x, y, ch, attr);
}

static void tty_flush_screen(void) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Don't flush TTY when a graphics app owns the display
    if (tty_state.graphics_app_active) {
        return;
    }
    
    if (!tty_state.cells || tty_state.cols == 0 || tty_state.rows == 0) {
        return;
    }

    // Cross-architecture console: full redraw (no dirty-cell optimization)
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        tty_flush_screen_arch_console();
        return;
    }

    // Status bar/scroll indicator repaint is expensive and its content
    // rarely changes between flushes — only repaint when actually dirty.
    if (g_status_bar_chrome_dirty) {
        tty_draw_status_bar();
        tty_draw_scroll_indicator();
        g_status_bar_chrome_dirty = false;
    }

    // Only render dirty cells for performance (critical for VirtualBox)
    for (uint16_t y = 0; y < tty_state.rows; y++) {
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            size_t idx = tty_cell_index(x, y);
            tty_cell_t* cell = &tty_state.cells[idx];
            if (cell->dirty) {
                tty_render_cell(x, y, cell->ch, cell->attr);
                cell->dirty = 0;  // Mark as clean after rendering
            }
        }
    }
    tty_apply_cursor();
}

// Render the entire cell buffer through the cross-architecture console.
// Used when TTY_BACKEND_ARCH_CONSOLE is active (serial or arch-level FB).
static void tty_flush_screen_arch_console(void) {
    if (!tty_state.cells || tty_state.cols == 0 || tty_state.rows == 0) {
        return;
    }

    arch_console_clear();

    static const uint32_t vga_to_rgb[16] = {
        0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
        0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
        0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
        0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF,
    };

    for (uint16_t y = 0; y < tty_state.rows; y++) {
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            size_t idx = tty_cell_index(x, y);
            tty_cell_t* cell = &tty_state.cells[idx];

            uint8_t fg = cell->attr & 0x0F;
            uint8_t bg = (cell->attr >> 4) & 0x0F;
            arch_console_set_color(vga_to_rgb[fg], vga_to_rgb[bg & 0x07]);
            arch_console_putc(cell->ch ? cell->ch : ' ');
            cell->dirty = 0;
        }
        // Newline at end of each row (arch_console handles scroll)
        if (y < tty_state.rows - 1) {
            arch_console_putc('\n');
        }
    }

    // Restore default color from current TTY state
    uint8_t cur_fg = tty_state.fg & 0x0F;
    uint8_t cur_bg = tty_state.bg & 0x07;
    arch_console_set_color(vga_to_rgb[cur_fg], vga_to_rgb[cur_bg]);
}

// Force full screen redraw (used for tty_clear and initial display)
static void tty_flush_screen_full(void) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Don't flush TTY when a graphics app owns the display
    if (tty_state.graphics_app_active) {
        return;
    }

    if (!tty_state.cells || tty_state.cols == 0 || tty_state.rows == 0) {
        return;
    }

    // Cross-architecture console: bulk-render through arch_console
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        tty_flush_screen_arch_console();
        return;
    }

    // Draw status bar first
    tty_draw_status_bar();

    // Draw scroll position indicator
    tty_draw_scroll_indicator();
    g_status_bar_chrome_dirty = false;

    for (uint16_t y = 0; y < tty_state.rows; y++) {
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            size_t idx = tty_cell_index(x, y);
            tty_cell_t* cell = &tty_state.cells[idx];
            tty_render_cell(x, y, cell->ch, cell->attr);
            cell->dirty = 0;  // Mark as clean
        }
    }
    tty_apply_cursor();
}

static bool tty_scroll_framebuffer_one_row(void) {
    // No framebuffer scroll in cross-architecture console mode
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        return false;
    }

    if (framebuffer_has_userspace_mapping()) {
        return true;
    }
    framebuffer_t* fb = graphics_get_framebuffer();
    if (!fb || !fb->virtual_addr || tty_state.char_height == 0) {
        return false;
    }

    const uint32_t start_y = TTY_STATUS_BAR_HEIGHT;
    if (start_y >= fb->height) {
        return false;
    }

    uint32_t content_height = (uint32_t)tty_state.rows * tty_state.char_height;
    if (content_height <= tty_state.char_height) {
        return false;
    }
    if (start_y + content_height > fb->height) {
        content_height = fb->height - start_y;
    }

    const uint32_t scroll_pixels = tty_state.char_height;
    if (scroll_pixels >= content_height) {
        return false;
    }

    uint8_t* const base = (uint8_t*)fb->virtual_addr + ((size_t)start_y * fb->pitch);
    const size_t move_bytes = (size_t)(content_height - scroll_pixels) * fb->pitch;
    const size_t clear_bytes = (size_t)scroll_pixels * fb->pitch;

    memmove(base, base + clear_bytes, move_bytes);
    memset(base + move_bytes, 0, clear_bytes);
    return true;
}

static bool tty_set_dimensions(uint16_t cols, uint16_t rows) {
    if (cols == 0 || rows == 0) {
        return false;
    }

    size_t new_count = (size_t)cols * (size_t)rows;
    if (tty_state.cells && new_count == tty_state.cell_count) {
        tty_state.cols = cols;
        tty_state.rows = rows;
        if (tty_state.cursor_x >= cols) tty_state.cursor_x = cols - 1;
        if (tty_state.cursor_y >= rows) tty_state.cursor_y = rows - 1;
        return true;
    }

    tty_cell_t* new_cells = (tty_cell_t*)kzalloc(new_count * sizeof(tty_cell_t));
    if (!new_cells) {
        return false;
    }

    uint8_t attr = tty_current_attr();
    for (size_t i = 0; i < new_count; i++) {
        new_cells[i].ch = ' ';
        new_cells[i].attr = attr;
        new_cells[i].dirty = 1;  // Mark new cells as dirty for initial render
    }

    if (tty_state.cells) {
        uint16_t copy_rows = tty_state.rows < rows ? tty_state.rows : rows;
        uint16_t copy_cols = tty_state.cols < cols ? tty_state.cols : cols;
        for (uint16_t y = 0; y < copy_rows; y++) {
            memcpy(&new_cells[y * cols],
                   &tty_state.cells[y * tty_state.cols],
                   copy_cols * sizeof(tty_cell_t));
        }
        kfree(tty_state.cells);
    }

    tty_state.cells = new_cells;
    tty_state.cell_count = new_count;
    tty_state.cols = cols;
    tty_state.rows = rows;
    if (tty_state.cursor_x >= cols) tty_state.cursor_x = cols - 1;
    if (tty_state.cursor_y >= rows) tty_state.cursor_y = rows - 1;
    return true;
}

/* Called by SYS_SET_FB_MODE (see syscall.c) after a successful runtime
 * graphics_set_mode()/gfx_set_mode() resolution change. Re-derives cols/rows
 * from the new framebuffer geometry and *reallocates* tty_state.cells via
 * tty_set_dimensions() (preserving as much on-screen content as fits),
 * instead of the boot-time tty_update_dimensions_from_graphics() path which
 * only overwrites tty_state.cols/rows and would leave the cell buffer sized
 * for the old resolution -- a real out-of-bounds risk on shrink and a wasted/
 * incomplete redraw on grow. */
void tty_handle_display_mode_change(void) {
    // Display mode changes are not applicable to the cross-architecture console
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        return;
    }

    uint16_t cols, rows, char_w, char_h;
    if (!tty_compute_dimensions_from_graphics(&cols, &rows, &char_w, &char_h)) {
        debuglog(DEBUG_ERROR, "TTY: could not derive dimensions for new display mode\n");
        return;
    }

    tty_state.char_width = char_w;
    tty_state.char_height = char_h;

    if (!tty_set_dimensions(cols, rows)) {
        debuglog(DEBUG_ERROR, "TTY: failed to reallocate screen buffer for new mode %ux%u chars\n",
                 cols, rows);
        return;
    }

    tty_force_redraw();
}

static uint8_t tty_current_attr(void) {
    uint8_t fg = tty_state.fg & 0x0F;
    uint8_t bg = tty_state.bg & 0x0F;

    if (!g_tty_colors_enabled || !g_tty_advanced_mode) {
        fg = TEXT_ATTR_LIGHT_GRAY;
        bg = TEXT_ATTR_BLACK;
    }

    // Bold/bright handling
    if (tty_state.bold && !(fg & TEXT_ATTR_BRIGHT)) {
        fg |= TEXT_ATTR_BRIGHT;
    }
    if (tty_state.underline && !(fg & TEXT_ATTR_BRIGHT)) {
        fg |= TEXT_ATTR_BRIGHT;
    }
    if (tty_state.faint) {
        fg &= (uint8_t)~TEXT_ATTR_BRIGHT;
    }

    // Inverse video swaps the planes
    if (tty_state.inverse) {
        uint8_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    if (tty_state.conceal) {
        fg = bg;
    }

    uint8_t attr = (uint8_t)((bg << 4) | (fg & 0x0F));
    if (tty_state.blink && g_tty_blink_enabled && g_tty_advanced_mode) {
        attr |= TEXT_ATTR_BLINK;
    }

    return attr;
}

static void tty_backend_put(char c) {
    uint8_t attr = tty_current_attr();
    if (tty_state.cursor_x >= tty_state.cols || tty_state.cursor_y >= tty_state.rows) {
        return;
    }

    size_t idx = tty_cell_index(tty_state.cursor_x, tty_state.cursor_y);
    if (tty_state.cells && idx < tty_state.cell_count) {
        tty_state.cells[idx].ch = c;
        tty_state.cells[idx].attr = attr;
    }

    tty_render_cell(tty_state.cursor_x, tty_state.cursor_y, c, attr);

    // Serial mirroring is only useful during pre-framebuffer boot.
    // Keeping it off in normal framebuffer mode avoids severe output stalls.
    if (tty_state.boot_mode) {
        debuglog_write_char(c);
    }
}

static void tty_backend_clear_line_from_cursor(void) {
    uint8_t attr = tty_current_attr();
    uint16_t y = tty_state.cursor_y;
    for (uint16_t x = tty_state.cursor_x; x < tty_state.cols; x++) {
        size_t idx = tty_cell_index(x, y);
        if (tty_state.cells && idx < tty_state.cell_count) {
            tty_state.cells[idx].ch = ' ';
            tty_state.cells[idx].attr = attr;
        }
        tty_render_cell(x, y, ' ', attr);
    }
    tty_state.cursor_x = 0;
}

static void tty_scroll_if_needed(void) {
    if (tty_state.cursor_y < tty_state.rows) {
        return;
    }

    uint8_t attr = tty_current_attr();

    if (tty_state.cells) {
        // Shift all cells up by one row using memmove
        size_t line_size = (size_t)tty_state.cols * sizeof(tty_cell_t);
        memmove(tty_state.cells,
                tty_state.cells + tty_state.cols,
                line_size * (tty_state.rows - 1));

        // Clear the last row
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            size_t idx = tty_cell_index(x, tty_state.rows - 1);
            tty_state.cells[idx].ch = ' ';
            tty_state.cells[idx].attr = attr;
            tty_state.cells[idx].dirty = 0;
        }
        tty_state.cursor_y = tty_state.rows - 1;

        // Fast path: scroll only the text region in framebuffer memory.
        if (tty_scroll_framebuffer_one_row()) {
            uint16_t y = tty_state.rows - 1;
            for (uint16_t x = 0; x < tty_state.cols; x++) {
                size_t idx = tty_cell_index(x, y);
                tty_cell_t* cell = &tty_state.cells[idx];
                tty_render_cell(x, y, cell->ch, cell->attr);
                cell->dirty = 0;
            }
            return;
        }

        // Fallback path: redraw from cell buffer if direct framebuffer scroll is unavailable.
        for (size_t i = 0; i < tty_state.cell_count; i++) {
            tty_state.cells[i].dirty = 1;
        }
        tty_flush_screen();
    } else {
        // Use graphics subsystem for scrolling
        graphics_scroll_screen(1);
        tty_state.cursor_y = tty_state.rows - 1;
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            graphics_write_char(x, tty_state.cursor_y, ' ', attr);
        }
    }
}

static void tty_handle_control(char c) {
    switch (c) {
        case '\n': // Line Feed
            tty_state.cursor_x = 0;
            tty_state.cursor_y++;
            tty_scroll_if_needed();
            break;
        case '\r': // Carriage Return
            tty_state.cursor_x = 0;
            break;
        case '\b': // Backspace
            if (tty_state.cursor_x > 0) {
                tty_state.cursor_x--;
                tty_backend_put(' ');
            }
            break;
        case '\t': // Horizontal Tab
            tty_state.cursor_x = (uint16_t)((tty_state.cursor_x + 8) & ~(uint16_t)(8 - 1));
            if (tty_state.cursor_x >= tty_state.cols) {
                tty_state.cursor_x = 0;
                tty_state.cursor_y++;
                tty_scroll_if_needed();
            }
            break;
        case '\v': // Vertical Tab
            tty_state.cursor_y++;
            tty_scroll_if_needed();
            break;
        case '\f': // Form Feed
            tty_clear();
            break;
        case '\a': // Bell - audible alert via PC speaker fallback
#if ENABLE_TTY_BELL
            snd_pcspeaker_beep();
#endif
            break;
        case 0x7F: // Delete
            // Could implement character deletion
            break;
        default:
            if (c >= 32 || c < 0) { // Printable characters
                tty_backend_put(c);
                tty_state.cursor_x++;
                if (tty_state.cursor_x >= tty_state.cols) {
                    tty_state.cursor_x = 0;
                    tty_state.cursor_y++;
                    tty_scroll_if_needed();
                }
            }
            break;
    }

    tty_apply_cursor();
}

static void tty_reset_ansi_parser(void) {
    ansi_parser.state = ANSI_STATE_NORMAL;
    ansi_parser.param_count = 0;
    ansi_parser.param_in_progress = false;
    ansi_parser.private_mode = false;
    ansi_parser.string_length = 0;
    ansi_parser.final_char = 0;
    ansi_parser.osc_param = 0;
    ansi_parser.osc_param_parsed = false;
    ansi_parser.has_dollar = false;
    memset(ansi_parser.params, 0, sizeof(ansi_parser.params));
    memset(ansi_parser.string_buffer, 0, sizeof(ansi_parser.string_buffer));
}

static void tty_enter_alternate_screen(void) {
    if (tty_state.alternate_screen_active || !tty_state.cells) {
        return;
    }

    size_t count = tty_state.cell_count;
    tty_cell_t* buf = (tty_cell_t*)kzalloc(count * sizeof(tty_cell_t));
    if (!buf) {
        return;
    }

    memcpy(buf, tty_state.cells, count * sizeof(tty_cell_t));
    tty_state.alternate_cells = buf;
    tty_state.alternate_cell_count = count;
    tty_state.alternate_cursor_x = tty_state.cursor_x;
    tty_state.alternate_cursor_y = tty_state.cursor_y;
    tty_state.alternate_saved_cursor_x = tty_state.saved_cursor_x;
    tty_state.alternate_saved_cursor_y = tty_state.saved_cursor_y;
    tty_state.alternate_fg = tty_state.fg;
    tty_state.alternate_bg = tty_state.bg;
    tty_state.alternate_bold = tty_state.bold;
    tty_state.alternate_faint = tty_state.faint;
    tty_state.alternate_underline = tty_state.underline;
    tty_state.alternate_blink = tty_state.blink;
    tty_state.alternate_inverse = tty_state.inverse;
    tty_state.alternate_conceal = tty_state.conceal;
    tty_state.alternate_italic = tty_state.italic;
    tty_state.alternate_strike = tty_state.strike;
    tty_state.alternate_double_underline = tty_state.double_underline;
    tty_state.alternate_overlined = tty_state.overlined;
    tty_state.alternate_framed = tty_state.framed;
    tty_state.alternate_encircled = tty_state.encircled;
    tty_state.alternate_crossed_out = tty_state.crossed_out;
    tty_state.alternate_true_fg = tty_state.true_fg;
    tty_state.alternate_true_bg = tty_state.true_bg;
    tty_state.alternate_use_true_colors = tty_state.use_true_colors;
    tty_state.alternate_screen_active = true;

    uint8_t attr = tty_current_attr();
    for (size_t i = 0; i < count; i++) {
        tty_state.cells[i].ch = ' ';
        tty_state.cells[i].attr = attr;
        tty_state.cells[i].dirty = 1;
    }
    tty_state.cursor_x = 0;
    tty_state.cursor_y = 0;
    tty_state.saved_cursor_x = 0;
    tty_state.saved_cursor_y = 0;
    tty_flush_screen_full();
}

static void tty_leave_alternate_screen(void) {
    if (!tty_state.alternate_screen_active || !tty_state.alternate_cells) {
        return;
    }

    size_t count = tty_state.cell_count < tty_state.alternate_cell_count
                    ? tty_state.cell_count : tty_state.alternate_cell_count;
    memcpy(tty_state.cells, tty_state.alternate_cells, count * sizeof(tty_cell_t));
    if (count < tty_state.cell_count) {
        uint8_t attr = tty_current_attr();
        for (size_t i = count; i < tty_state.cell_count; i++) {
            tty_state.cells[i].ch = ' ';
            tty_state.cells[i].attr = attr;
            tty_state.cells[i].dirty = 1;
        }
    }
    for (size_t i = 0; i < tty_state.cell_count; i++) {
        tty_state.cells[i].dirty = 1;
    }

    tty_state.cursor_x = tty_state.alternate_cursor_x < tty_state.cols
                          ? tty_state.alternate_cursor_x : tty_state.cols - 1;
    tty_state.cursor_y = tty_state.alternate_cursor_y < tty_state.rows
                          ? tty_state.alternate_cursor_y : tty_state.rows - 1;
    tty_state.saved_cursor_x = tty_state.alternate_saved_cursor_x;
    tty_state.saved_cursor_y = tty_state.alternate_saved_cursor_y;
    tty_state.fg = tty_state.alternate_fg;
    tty_state.bg = tty_state.alternate_bg;
    tty_state.bold = tty_state.alternate_bold;
    tty_state.faint = tty_state.alternate_faint;
    tty_state.underline = tty_state.alternate_underline;
    tty_state.blink = tty_state.alternate_blink;
    tty_state.inverse = tty_state.alternate_inverse;
    tty_state.conceal = tty_state.alternate_conceal;
    tty_state.italic = tty_state.alternate_italic;
    tty_state.strike = tty_state.alternate_strike;
    tty_state.double_underline = tty_state.alternate_double_underline;
    tty_state.overlined = tty_state.alternate_overlined;
    tty_state.framed = tty_state.alternate_framed;
    tty_state.encircled = tty_state.alternate_encircled;
    tty_state.crossed_out = tty_state.alternate_crossed_out;
    tty_state.true_fg = tty_state.alternate_true_fg;
    tty_state.true_bg = tty_state.alternate_true_bg;
    tty_state.use_true_colors = tty_state.alternate_use_true_colors;

    kfree(tty_state.alternate_cells);
    tty_state.alternate_cells = NULL;
    tty_state.alternate_cell_count = 0;
    tty_state.alternate_screen_active = false;
    tty_flush_screen_full();
}

static void tty_scroll_region(int count, bool up) {
    if (!tty_state.cells || count <= 0) return;

    uint16_t top = tty_state.scroll_top;
    uint16_t bottom = tty_state.scroll_bottom;
    if (bottom <= top || bottom > tty_state.rows) {
        top = 0;
        bottom = tty_state.rows;
    }

    uint8_t attr = tty_current_attr();
    uint16_t region_height = bottom - top;
    if ((uint16_t)count >= region_height) {
        for (uint16_t y = top; y < bottom; y++) {
            for (uint16_t x = 0; x < tty_state.cols; x++) {
                size_t idx = tty_cell_index(x, y);
                tty_state.cells[idx].ch = ' ';
                tty_state.cells[idx].attr = attr;
                tty_state.cells[idx].dirty = 1;
            }
        }
        tty_flush_screen();
        return;
    }

    if (up) {
        size_t line_size = (size_t)tty_state.cols * sizeof(tty_cell_t);
        memmove(&tty_state.cells[tty_cell_index(0, top)],
                &tty_state.cells[tty_cell_index(0, top + count)],
                line_size * (region_height - count));
        for (uint16_t y = bottom - count; y < bottom; y++) {
            for (uint16_t x = 0; x < tty_state.cols; x++) {
                size_t idx = tty_cell_index(x, y);
                tty_state.cells[idx].ch = ' ';
                tty_state.cells[idx].attr = attr;
                tty_state.cells[idx].dirty = 1;
            }
        }
    } else {
        size_t line_size = (size_t)tty_state.cols * sizeof(tty_cell_t);
        for (uint16_t y = bottom - count; y > top; y--) {
            memcpy(&tty_state.cells[tty_cell_index(0, y)],
                   &tty_state.cells[tty_cell_index(0, y - count)],
                   line_size);
        }
        for (uint16_t y = top; y < top + count; y++) {
            for (uint16_t x = 0; x < tty_state.cols; x++) {
                size_t idx = tty_cell_index(x, y);
                tty_state.cells[idx].ch = ' ';
                tty_state.cells[idx].attr = attr;
                tty_state.cells[idx].dirty = 1;
            }
        }
    }
    tty_flush_screen();
}

static void tty_insert_lines(int count) {
    if (!tty_state.cells || count <= 0) return;

    uint16_t top = tty_state.scroll_top;
    uint16_t bottom = tty_state.scroll_bottom;
    if (bottom <= top || bottom > tty_state.rows) {
        top = 0;
        bottom = tty_state.rows;
    }

    if (tty_state.cursor_y < top || tty_state.cursor_y >= bottom) return;

    uint8_t attr = tty_current_attr();
    uint16_t lines_below = bottom - tty_state.cursor_y;
    uint16_t shift = (uint16_t)count < lines_below ? (uint16_t)count : lines_below;

    size_t line_size = (size_t)tty_state.cols * sizeof(tty_cell_t);
    for (uint16_t y = bottom - shift; y > tty_state.cursor_y; y--) {
        memcpy(&tty_state.cells[tty_cell_index(0, y)],
               &tty_state.cells[tty_cell_index(0, y - shift)],
               line_size);
    }
    for (uint16_t y = tty_state.cursor_y; y < tty_state.cursor_y + shift; y++) {
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            size_t idx = tty_cell_index(x, y);
            tty_state.cells[idx].ch = ' ';
            tty_state.cells[idx].attr = attr;
            tty_state.cells[idx].dirty = 1;
        }
    }
    tty_flush_screen();
}

static void tty_delete_lines(int count) {
    if (!tty_state.cells || count <= 0) return;

    uint16_t top = tty_state.scroll_top;
    uint16_t bottom = tty_state.scroll_bottom;
    if (bottom <= top || bottom > tty_state.rows) {
        top = 0;
        bottom = tty_state.rows;
    }

    if (tty_state.cursor_y < top || tty_state.cursor_y >= bottom) return;

    uint8_t attr = tty_current_attr();
    uint16_t lines_below = bottom - tty_state.cursor_y;
    uint16_t shift = (uint16_t)count < lines_below ? (uint16_t)count : lines_below;

    size_t line_size = (size_t)tty_state.cols * sizeof(tty_cell_t);
    memcpy(&tty_state.cells[tty_cell_index(0, tty_state.cursor_y)],
           &tty_state.cells[tty_cell_index(0, tty_state.cursor_y + shift)],
           line_size * (lines_below - shift));
    for (uint16_t y = bottom - shift; y < bottom; y++) {
        for (uint16_t x = 0; x < tty_state.cols; x++) {
            size_t idx = tty_cell_index(x, y);
            tty_state.cells[idx].ch = ' ';
            tty_state.cells[idx].attr = attr;
            tty_state.cells[idx].dirty = 1;
        }
    }
    tty_flush_screen();
}

static void tty_insert_chars(int count) {
    if (!tty_state.cells || count <= 0) return;
    if (tty_state.cursor_x >= tty_state.cols || tty_state.cursor_y >= tty_state.rows) return;

    uint8_t attr = tty_current_attr();
    uint16_t y = tty_state.cursor_y;
    uint16_t remaining = tty_state.cols - tty_state.cursor_x;
    uint16_t shift = (uint16_t)count < remaining ? (uint16_t)count : remaining;

    for (uint16_t x = tty_state.cols - 1; x >= tty_state.cursor_x + shift; x--) {
        size_t dst = tty_cell_index(x, y);
        size_t src = tty_cell_index(x - shift, y);
        tty_state.cells[dst] = tty_state.cells[src];
        tty_state.cells[dst].dirty = 1;
    }
    for (uint16_t x = tty_state.cursor_x; x < tty_state.cursor_x + shift; x++) {
        size_t idx = tty_cell_index(x, y);
        tty_state.cells[idx].ch = ' ';
        tty_state.cells[idx].attr = attr;
        tty_state.cells[idx].dirty = 1;
    }
    tty_flush_screen();
}

static void tty_delete_chars(int count) {
    if (!tty_state.cells || count <= 0) return;
    if (tty_state.cursor_x >= tty_state.cols || tty_state.cursor_y >= tty_state.rows) return;

    uint8_t attr = tty_current_attr();
    uint16_t y = tty_state.cursor_y;
    uint16_t remaining = tty_state.cols - tty_state.cursor_x;
    uint16_t shift = (uint16_t)count < remaining ? (uint16_t)count : remaining;

    for (uint16_t x = tty_state.cursor_x; x + shift < tty_state.cols; x++) {
        size_t dst = tty_cell_index(x, y);
        size_t src = tty_cell_index(x + shift, y);
        tty_state.cells[dst] = tty_state.cells[src];
        tty_state.cells[dst].dirty = 1;
    }
    for (uint16_t x = tty_state.cols - shift; x < tty_state.cols; x++) {
        size_t idx = tty_cell_index(x, y);
        tty_state.cells[idx].ch = ' ';
        tty_state.cells[idx].attr = attr;
        tty_state.cells[idx].dirty = 1;
    }
    tty_flush_screen();
}

void tty_handle_mouse_event(int x, int y, int button, bool pressed) {
    if (tty_state.mouse_mode == 0) return;
    if (!tty_state.initialized || !tty_runtime_mutation_allowed()) return;
    if (tty_state.graphics_app_active) return;

    int cb = 0;
    if (button == 0) cb = pressed ? 0 : 32;
    else if (button == 1) cb = pressed ? 1 : 33;
    else if (button == 2) cb = pressed ? 2 : 34;
    else cb = 64;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > 222) x = 222;
    if (y > 222) y = 222;

    if (tty_state.mouse_mode == 1006) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "\x1B[<%d;%d;%d%c",
                           cb, x + 1, y + 1, pressed ? 'M' : 'm');
        for (int i = 0; i < len; i++) {
            tty_handle_control(buf[i]);
        }
    } else if (tty_state.mouse_mode == 1000) {
        char buf[8];
        buf[0] = '\x1B';
        buf[1] = '[';
        buf[2] = 'M';
        buf[3] = (char)(cb + 32);
        buf[4] = (char)(x + 33);
        buf[5] = (char)(y + 33);
        for (int i = 0; i < 6; i++) {
            tty_handle_control(buf[i]);
        }
    }
}

void tty_handle_paste_start(void) {
    if (!ansi_parser.bracketed_paste_mode) return;
    if (tty_state.graphics_app_active) return;
    const char* seq = "\x1B[200~";
    for (int i = 0; seq[i]; i++) {
        tty_handle_control(seq[i]);
    }
}

void tty_handle_paste_end(void) {
    if (!ansi_parser.bracketed_paste_mode) return;
    if (tty_state.graphics_app_active) return;
    const char* seq = "\x1B[201~";
    for (int i = 0; seq[i]; i++) {
        tty_handle_control(seq[i]);
    }
}

static void tty_handle_osc_command(void) {
    char* buf = ansi_parser.string_buffer;
    int ps = ansi_parser.osc_param;
    char* pt = NULL;

    for (size_t i = 0; i < ansi_parser.string_length; i++) {
        if (buf[i] == ';') {
            buf[i] = '\0';
            pt = &buf[i + 1];
            break;
        }
    }

    if (!pt) {
        pt = buf;
    }

    switch (ps) {
        case 0:
        case 1:
        case 2:
            if (pt[0]) {
                strncpy(tty_state.window_title, pt, sizeof(tty_state.window_title) - 1);
                tty_state.window_title[sizeof(tty_state.window_title) - 1] = '\0';
            }
            break;
        case 4: {
            int color_num = 0;
            int r = 0, g = 0, b = 0;
            const char* p = pt;
            color_num = 0;
            while (*p >= '0' && *p <= '9') {
                color_num = color_num * 10 + (*p - '0');
                p++;
            }
            if (*p == ';') p++;
            if (p[0] == 'r' && p[1] == 'g' && p[2] == 'b' && p[3] == ':') {
                p += 4;
                r = 0; while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    r = r * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0'); p++; }
                if (*p == '/') p++;
                g = 0; while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    g = g * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0'); p++; }
                if (*p == '/') p++;
                b = 0; while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    b = b * 16 + (*p >= 'a' ? *p - 'a' + 10 : *p >= 'A' ? *p - 'A' + 10 : *p - '0'); p++; }
                if (r > 255) r = r >> 8;
                if (g > 255) g = g >> 8;
                if (b > 255) b = b >> 8;
            } else {
                r = 0; while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; }
                if (*p == ';') p++;
                g = 0; while (*p >= '0' && *p <= '9') { g = g * 10 + (*p - '0'); p++; }
                if (*p == ';') p++;
                b = 0; while (*p >= '0' && *p <= '9') { b = b * 10 + (*p - '0'); p++; }
            }
            if (color_num >= 0 && color_num < 256) {
                if (!palette_initialized) {
                    tty_init_256_palette();
                }
                tty_palette_256[color_num] = (graphics_color_t){
                    (uint8_t)r, (uint8_t)g, (uint8_t)b, 255
                };
                for (size_t ci = 0; ci < tty_state.cell_count; ci++) {
                    tty_state.cells[ci].dirty = 1;
                }
            }
            break;
        }
        case 10:
            break;
        case 11:
            break;
        case 8:
            break;
        default:
            break;
    }
}

static void tty_handle_dcs_command(void) {
    // DCS sequences: ESC P ... ST
    // Used for various device control functions
    // For now, just ignore DCS sequences
    (void)ansi_parser.string_buffer; // Suppress unused warning for now
}

static uint8_t tty_color_nibble_from_ansi(int code) {
    bool bright = false;
    uint8_t base_color = 0;

    if (code >= 90 && code <= 97) {
        base_color = (uint8_t)(code - 90);
        bright = true;
    } else if (code >= 30 && code <= 37) {
        base_color = (uint8_t)(code - 30);
    } else if (code >= 100 && code <= 107) {
        base_color = (uint8_t)(code - 100);
        bright = true;
    } else if (code >= 40 && code <= 47) {
        base_color = (uint8_t)(code - 40);
    } else {
        return 0xFF;
    }

    if (bright) {
        base_color |= TEXT_ATTR_BRIGHT;
    }

    return (uint8_t)(base_color & 0x0F);
}

static void tty_handle_sgr(const int* params, size_t count) {
    if (count == 0) {
        // Reset all attributes
        tty_set_attr(MAKE_TEXT_ATTR(TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK));
        tty_state.bold = false;
        tty_state.faint = false;
        tty_state.underline = false;
        tty_state.double_underline = false;
        tty_state.blink = false;
        tty_state.inverse = false;
        tty_state.conceal = false;
        tty_state.italic = false;
        tty_state.strike = false;
        tty_state.crossed_out = false;
        tty_state.overlined = false;
        tty_state.framed = false;
        tty_state.encircled = false;
        tty_state.use_true_colors = false;
        return;
    }

    for (size_t i = 0; i < count; i++) {
        int p = params[i];
        if (p == 0) {
            // Reset all attributes
            tty_set_attr(MAKE_TEXT_ATTR(TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK));
            tty_state.bold = false;
            tty_state.faint = false;
            tty_state.underline = false;
            tty_state.double_underline = false;
            tty_state.blink = false;
            tty_state.inverse = false;
            tty_state.conceal = false;
            tty_state.italic = false;
            tty_state.strike = false;
            tty_state.crossed_out = false;
            tty_state.overlined = false;
            tty_state.framed = false;
            tty_state.encircled = false;
            tty_state.use_true_colors = false;
        } else if (p == 1) {
            tty_state.bold = true;
            tty_state.faint = false;
        } else if (p == 2) {
            tty_state.faint = true;
            tty_state.bold = false;
        } else if (p == 3) {
            tty_state.italic = true;
        } else if (p == 4) {
            tty_state.underline = true;
            tty_state.double_underline = false;
        } else if (p == 5 || p == 6) {
            tty_state.blink = g_tty_blink_enabled && g_tty_advanced_mode;
        } else if (p == 7) {
            tty_state.inverse = true;
        } else if (p == 8) {
            tty_state.conceal = true;
        } else if (p == 9) {
            tty_state.strike = true;
        } else if (p == 21) {
            tty_state.double_underline = true;
            tty_state.underline = false;
        } else if (p == 22) {
            tty_state.bold = false;
            tty_state.faint = false;
        } else if (p == 23) {
            tty_state.italic = false;
        } else if (p == 24) {
            tty_state.underline = false;
            tty_state.double_underline = false;
        } else if (p == 25) {
            tty_state.blink = false;
        } else if (p == 27) {
            tty_state.inverse = false;
        } else if (p == 28) {
            tty_state.conceal = false;
        } else if (p == 29) {
            tty_state.strike = false;
        } else if (p == 51) {
            tty_state.framed = true;
        } else if (p == 52) {
            tty_state.encircled = true;
        } else if (p == 53) {
            tty_state.overlined = true;
        } else if (p == 54) {
            tty_state.framed = false;
            tty_state.encircled = false;
        } else if (p == 55) {
            tty_state.overlined = false;
        } else if (((p >= 30 && p <= 37) || (p >= 90 && p <= 97)) && g_tty_colors_enabled && g_tty_advanced_mode) {
            uint8_t nibble = tty_color_nibble_from_ansi(p);
            if (nibble != 0xFF) {
                tty_state.fg = nibble;
                tty_state.use_true_colors = false;
            }
        } else if (((p >= 40 && p <= 47) || (p >= 100 && p <= 107)) && g_tty_colors_enabled && g_tty_advanced_mode) {
            uint8_t nibble = tty_color_nibble_from_ansi(p);
            if (nibble != 0xFF) {
                tty_state.bg = nibble & 0x07; // backgrounds limited to base palette
                tty_state.use_true_colors = false;
            }
        } else if (p == 39 && g_tty_colors_enabled && g_tty_advanced_mode) {
            tty_state.fg = TEXT_ATTR_LIGHT_GRAY;
            tty_state.use_true_colors = false;
        } else if (p == 49 && g_tty_colors_enabled && g_tty_advanced_mode) {
            tty_state.bg = TEXT_ATTR_BLACK;
            tty_state.use_true_colors = false;
        } else if ((p == 38 || p == 48) && g_tty_colors_enabled && g_tty_advanced_mode) {
            bool is_bg = (p == 48);
            if (i + 1 < count) {
                int mode = params[i + 1];
                if (mode == 5 && (i + 2) < count) {
                    uint8_t idx = (uint8_t)params[i + 2];
                    if (!palette_initialized) {
                        tty_init_256_palette();
                    }
                    if (is_bg) {
                        tty_state.true_bg = tty_palette_256[idx];
                        tty_state.bg = tty_map_256_color(idx, true);
                    } else {
                        tty_state.true_fg = tty_palette_256[idx];
                        tty_state.fg = tty_map_256_color(idx, false);
                    }
                    tty_state.use_true_colors = true;
                    i += 2;
                } else if (mode == 2 && (i + 4) < count) {
                    uint8_t r = (uint8_t)params[i + 2];
                    uint8_t g = (uint8_t)params[i + 3];
                    uint8_t b = (uint8_t)params[i + 4];
                    if (is_bg) {
                        tty_state.true_bg = (graphics_color_t){r, g, b, 255};
                        tty_state.bg = tty_map_rgb_to_attr(r, g, b, true);
                    } else {
                        tty_state.true_fg = (graphics_color_t){r, g, b, 255};
                        tty_state.fg = tty_map_rgb_to_attr(r, g, b, false);
                    }
                    tty_state.use_true_colors = true;
                    i += 4;
                } else if (mode == 4 && (i + 4) < count) {
                    int color_num = params[i + 2];
                    uint8_t r = (uint8_t)params[i + 3];
                    uint8_t g = (uint8_t)params[i + 4];
                    uint8_t b = (i + 5 < count) ? (uint8_t)params[i + 5] : 0;
                    if (color_num >= 0 && color_num < 256) {
                        if (!palette_initialized) {
                            tty_init_256_palette();
                        }
                        tty_palette_256[color_num] = (graphics_color_t){r, g, b, 255};
                        if (tty_state.cells) {
                            for (size_t ci = 0; ci < tty_state.cell_count; ci++) {
                                tty_state.cells[ci].dirty = 1;
                            }
                        }
                    }
                    i += (i + 5 < count) ? 5 : 4;
                }
            }
        }
    }
}

static void tty_handle_csi_command(char command) {
    if (command == 'm') {
        tty_handle_sgr(ansi_parser.params, ansi_parser.param_count);
        return;
    }

    if ((command == 'h' || command == 'l') && ansi_parser.private_mode) {
        for (size_t i = 0; i < ansi_parser.param_count; i++) {
            switch (ansi_parser.params[i]) {
                case 1:
                    ansi_parser.application_mode = (command == 'h');
                    break;
                case 7:
                    break;
                case 25:
                    tty_state.cursor_visible = (command == 'h');
                    break;
                case 47:
                case 1047:
                    if (command == 'h' && !tty_state.alternate_screen_active) {
                        tty_enter_alternate_screen();
                    } else if (command == 'l' && tty_state.alternate_screen_active) {
                        tty_leave_alternate_screen();
                    }
                    break;
                case 1048:
                    if (command == 'h') {
                        tty_state.saved_cursor_x = tty_state.cursor_x;
                        tty_state.saved_cursor_y = tty_state.cursor_y;
                    } else {
                        tty_state.cursor_x = tty_state.saved_cursor_x < tty_state.cols
                                              ? tty_state.saved_cursor_x : tty_state.cols - 1;
                        tty_state.cursor_y = tty_state.saved_cursor_y < tty_state.rows
                                              ? tty_state.saved_cursor_y : tty_state.rows - 1;
                        tty_apply_cursor();
                    }
                    break;
                case 1049:
                    if (command == 'h') {
                        tty_state.saved_cursor_x = tty_state.cursor_x;
                        tty_state.saved_cursor_y = tty_state.cursor_y;
                        if (!tty_state.alternate_screen_active) {
                            tty_enter_alternate_screen();
                        }
                    } else {
                        if (tty_state.alternate_screen_active) {
                            tty_leave_alternate_screen();
                        }
                        tty_state.cursor_x = tty_state.saved_cursor_x < tty_state.cols
                                              ? tty_state.saved_cursor_x : tty_state.cols - 1;
                        tty_state.cursor_y = tty_state.saved_cursor_y < tty_state.rows
                                              ? tty_state.saved_cursor_y : tty_state.rows - 1;
                        tty_apply_cursor();
                    }
                    break;
                case 1000:
                case 1002:
                case 1003:
                    tty_state.mouse_mode = (command == 'h') ? ansi_parser.params[i] : 0;
                    break;
                case 1006:
                    if (command == 'h') {
                        tty_state.mouse_mode = 1006;
                    } else {
                        if (tty_state.mouse_mode == 1006) {
                            tty_state.mouse_mode = 0;
                        }
                    }
                    break;
                case 2004:
                    ansi_parser.bracketed_paste_mode = (command == 'h');
                    break;
                default:
                    break;
            }
        }
        return;
    }

    if (command == 's') {
        tty_state.saved_cursor_x = tty_state.cursor_x;
        tty_state.saved_cursor_y = tty_state.cursor_y;
        return;
    }

    if (command == 'u') {
        tty_state.cursor_x = tty_state.saved_cursor_x < tty_state.cols ? tty_state.saved_cursor_x : (tty_state.cols - 1);
        tty_state.cursor_y = tty_state.saved_cursor_y < tty_state.rows ? tty_state.saved_cursor_y : (tty_state.rows - 1);
        tty_apply_cursor();
        return;
    }

    // Cursor movement commands
    if (command == 'A' || command == 'B' || command == 'C' || command == 'D' || 
        command == 'E' || command == 'F' || command == 'G') {
        int amount = (ansi_parser.param_count > 0 && ansi_parser.params[0] > 0) ? ansi_parser.params[0] : 1;
        switch (command) {
            case 'A': // Cursor up
                tty_state.cursor_y = (tty_state.cursor_y >= amount) ? (tty_state.cursor_y - amount) : 0;
                break;
            case 'B': // Cursor down
                tty_state.cursor_y = (tty_state.cursor_y + amount < tty_state.rows) ? (tty_state.cursor_y + amount) : (tty_state.rows - 1);
                break;
            case 'C': // Cursor right
                tty_state.cursor_x = (tty_state.cursor_x + amount < tty_state.cols) ? (tty_state.cursor_x + amount) : (tty_state.cols - 1);
                break;
            case 'D': // Cursor left
                tty_state.cursor_x = (tty_state.cursor_x >= amount) ? (tty_state.cursor_x - amount) : 0;
                break;
            case 'E': // Cursor next line
                tty_state.cursor_x = 0;
                tty_state.cursor_y = (tty_state.cursor_y + amount < tty_state.rows) ? (tty_state.cursor_y + amount) : (tty_state.rows - 1);
                break;
            case 'F': // Cursor previous line
                tty_state.cursor_x = 0;
                tty_state.cursor_y = (tty_state.cursor_y >= amount) ? (tty_state.cursor_y - amount) : 0;
                break;
            case 'G': // Cursor horizontal absolute
                if (amount > 0) amount--; // 1-based to 0-based
                tty_state.cursor_x = (amount < tty_state.cols) ? amount : (tty_state.cols - 1);
                break;
        }
        tty_apply_cursor();
        return;
    }

    if (command == 'S' || command == 'T') {
        int amount = (ansi_parser.param_count > 0 && ansi_parser.params[0] > 0) ? ansi_parser.params[0] : 1;
        tty_scroll_region(amount, command == 'S');
        tty_apply_cursor();
        return;
    }

    if (command == 'H' || command == 'f') {
        uint16_t row = 1;
        uint16_t col = 1;
        if (ansi_parser.param_count >= 1 && ansi_parser.params[0] > 0) {
            row = (uint16_t)ansi_parser.params[0];
        }
        if (ansi_parser.param_count >= 2 && ansi_parser.params[1] > 0) {
            col = (uint16_t)ansi_parser.params[1];
        }
        if (row > 0) row--;
        if (col > 0) col--;
        tty_state.cursor_y = row < tty_state.rows ? row : tty_state.rows - 1;
        tty_state.cursor_x = col < tty_state.cols ? col : tty_state.cols - 1;
        tty_apply_cursor();
        return;
    }

    if (command == 'J') {
        // 0: cursor to end, 1: start to cursor, 2: entire screen, 3: entire screen + saved lines
        int mode = (ansi_parser.param_count > 0) ? ansi_parser.params[0] : 0;
        if (mode == 2 || mode == 3) {
            tty_clear();
            tty_state.cursor_x = 0;
            tty_state.cursor_y = 0;
            tty_apply_cursor();
        } else if (mode == 0) {
            // Clear from cursor to end of screen
            uint16_t start_y = tty_state.cursor_y;
            uint16_t start_x = tty_state.cursor_x;
            for (uint16_t y = start_y; y < tty_state.rows; y++) {
                for (uint16_t x = (y == start_y ? start_x : 0); x < tty_state.cols; x++) {
                    tty_state.cursor_x = x;
                    tty_state.cursor_y = y;
                    tty_backend_put(' ');
                }
            }
            tty_state.cursor_x = start_x;
            tty_state.cursor_y = start_y;
            tty_apply_cursor();
        } else if (mode == 1) {
            // Clear from start to cursor
            uint16_t end_y = tty_state.cursor_y;
            uint16_t end_x = tty_state.cursor_x;
            for (uint16_t y = 0; y <= end_y; y++) {
                for (uint16_t x = 0; x < (y == end_y ? end_x + 1 : tty_state.cols); x++) {
                    tty_state.cursor_x = x;
                    tty_state.cursor_y = y;
                    tty_backend_put(' ');
                }
            }
            tty_state.cursor_x = end_x;
            tty_state.cursor_y = end_y;
            tty_apply_cursor();
        }
        return;
    }

    if (command == 'K') {
        int mode = (ansi_parser.param_count > 0) ? ansi_parser.params[0] : 0;
        // Per ECMA-48/VT100, EL (Erase in Line) never moves the cursor,
        // regardless of mode -- only mode 1/2 used to restore it here, so
        // mode 0 (erase-to-end-of-line, the common bare "\x1b[K") left the
        // cursor stuck at column 0 (tty_backend_clear_line_from_cursor()'s
        // own side effect) instead of back where it started.
        uint16_t original_x = tty_state.cursor_x;
        if (mode == 0) {
            tty_backend_clear_line_from_cursor();
        } else if (mode == 1) {
            // Clear from start to cursor
            tty_state.cursor_x = 0;
            tty_backend_clear_line_from_cursor();
        } else if (mode == 2) {
            // Clear entire line
            tty_state.cursor_x = 0;
            for (uint16_t x = 0; x < tty_state.cols; x++) {
                tty_backend_put(' ');
                tty_state.cursor_x++;
            }
        }
        tty_state.cursor_x = original_x < tty_state.cols ? original_x : (tty_state.cols - 1);
        tty_apply_cursor();
        return;
    }

    if (command == 'n' && ansi_parser.param_count > 0 && ansi_parser.params[0] == 6) {
        char resp[32];
        snprintf(resp, sizeof(resp), "\x1B[%d;%dR",
                 tty_state.cursor_y + 1, tty_state.cursor_x + 1);
        tty_response_push_str(resp);
        return;
    }

    if (command == 'y' && ansi_parser.has_dollar) {
        int ps = (ansi_parser.param_count > 0) ? ansi_parser.params[0] : 0;
        int mode = 0;
        if (ansi_parser.private_mode) {
            switch (ps) {
                case 1: mode = 1; break;
                case 7: mode = 1; break;
                case 25: mode = 1; break;
                case 1000: mode = tty_state.mouse_mode == 1000 ? 1 : 2; break;
                case 1002: mode = tty_state.mouse_mode == 1002 ? 1 : 2; break;
                case 1003: mode = tty_state.mouse_mode == 1003 ? 1 : 2; break;
                case 1006: mode = tty_state.mouse_mode == 1006 ? 1 : 2; break;
                case 1047: mode = tty_state.alternate_screen_active ? 1 : 2; break;
                case 1048: mode = 1; break;
                case 1049: mode = tty_state.alternate_screen_active ? 1 : 2; break;
                case 2004: mode = ansi_parser.bracketed_paste_mode ? 1 : 2; break;
                default: mode = 0; break;
            }
        } else {
            switch (ps) {
                case 4: mode = 1; break;
                case 20: mode = 2; break;
                default: mode = 0; break;
            }
        }
        char resp[32];
        snprintf(resp, sizeof(resp), "\x1B[?%d;$%dy", ps, mode);
        tty_response_push_str(resp);
        return;
    }

    if (command == 'L' || command == 'M' || command == 'P' || command == '@') {
        int amount = (ansi_parser.param_count > 0 && ansi_parser.params[0] > 0) ? ansi_parser.params[0] : 1;
        switch (command) {
            case 'L':
                tty_insert_lines(amount);
                break;
            case 'M':
                tty_delete_lines(amount);
                break;
            case 'P':
                tty_delete_chars(amount);
                break;
            case '@':
                tty_insert_chars(amount);
                break;
        }
        return;
    }

    // Scrolling region
    if (command == 'r') {
        uint16_t top = 1, bottom = tty_state.rows;
        if (ansi_parser.param_count >= 1 && ansi_parser.params[0] > 0) {
            top = (uint16_t)ansi_parser.params[0];
        }
        if (ansi_parser.param_count >= 2 && ansi_parser.params[1] > 0) {
            bottom = (uint16_t)ansi_parser.params[1];
        }
        if (top < 1) top = 1;
        if (bottom > tty_state.rows) bottom = tty_state.rows;
        if (top >= bottom) { top = 1; bottom = tty_state.rows; }
        tty_state.scroll_top = top - 1;
        tty_state.scroll_bottom = bottom;
        return;
    }

    // Set/reset modes (screen modes)
    if ((command == 'h' || command == 'l') && !ansi_parser.private_mode) {
        for (size_t i = 0; i < ansi_parser.param_count; i++) {
            switch (ansi_parser.params[i]) {
                case 4: // Insert mode
                    // Would enable/disable insert mode
                    break;
                case 20: // Automatic newline mode
                    // Would enable/disable automatic CR->CRLF
                    break;
                default:
                    break;
            }
        }
        return;
    }

    // Tab operations
    if (command == 'I') {
        // Forward tabulation
        int amount = (ansi_parser.param_count > 0 && ansi_parser.params[0] > 0) ? ansi_parser.params[0] : 1;
        for (int i = 0; i < amount; i++) {
            tty_state.cursor_x = (uint16_t)((tty_state.cursor_x + 8) & ~(uint16_t)(8 - 1));
            if (tty_state.cursor_x >= tty_state.cols) {
                tty_state.cursor_x = 0;
                tty_state.cursor_y++;
                tty_scroll_if_needed();
            }
        }
        tty_apply_cursor();
        return;
    }
    if (command == 'Z') {
        // Backward tabulation
        int amount = (ansi_parser.param_count > 0 && ansi_parser.params[0] > 0) ? ansi_parser.params[0] : 1;
        for (int i = 0; i < amount; i++) {
            if (tty_state.cursor_x >= 8) {
                tty_state.cursor_x = (uint16_t)((tty_state.cursor_x - 1) & ~(uint16_t)(8 - 1));
            } else {
                tty_state.cursor_x = 0;
            }
        }
        tty_apply_cursor();
        return;
    }
}

static void tty_process_ansi(char c) {
    switch (ansi_parser.state) {
        case ANSI_STATE_NORMAL:
            if (c == '\x1B') {
                ansi_parser.state = ANSI_STATE_ESC;
            } else {
                tty_handle_control(c);
            }
            break;

        case ANSI_STATE_ESC:
            if (c == '[') {
                ansi_parser.state = ANSI_STATE_CSI;
                ansi_parser.param_count = 0;
                ansi_parser.param_in_progress = false;
                ansi_parser.private_mode = false;
                memset(ansi_parser.params, 0, sizeof(ansi_parser.params));
            } else if (c == ']') {
                ansi_parser.state = ANSI_STATE_OSC;
                ansi_parser.string_length = 0;
                memset(ansi_parser.string_buffer, 0, sizeof(ansi_parser.string_buffer));
            } else if (c == 'P') {
                ansi_parser.state = ANSI_STATE_DCS;
                ansi_parser.string_length = 0;
                memset(ansi_parser.string_buffer, 0, sizeof(ansi_parser.string_buffer));
            } else if (c == '7') {
                // DECSC - Save cursor position
                tty_state.saved_cursor_x = tty_state.cursor_x;
                tty_state.saved_cursor_y = tty_state.cursor_y;
                ansi_parser.state = ANSI_STATE_NORMAL;
            } else if (c == '8') {
                // DECRC - Restore cursor position
                tty_state.cursor_x = tty_state.saved_cursor_x < tty_state.cols ? tty_state.saved_cursor_x : (tty_state.cols - 1);
                tty_state.cursor_y = tty_state.saved_cursor_y < tty_state.rows ? tty_state.saved_cursor_y : (tty_state.rows - 1);
                tty_apply_cursor();
                ansi_parser.state = ANSI_STATE_NORMAL;
            } else if (c == 'c') {
                // RIS - Reset to Initial State
                tty_clear();
                tty_state.cursor_x = 0;
                tty_state.cursor_y = 0;
                tty_state.fg = TEXT_ATTR_LIGHT_GRAY;
                tty_state.bg = TEXT_ATTR_BLACK;
                tty_state.bold = false;
                tty_state.faint = false;
                tty_state.underline = false;
                tty_state.blink = false;
                tty_state.inverse = false;
                tty_state.use_true_colors = false;
                ansi_parser.state = ANSI_STATE_NORMAL;
            } else if (c == 'M') {
                // Reverse Index - move cursor up one line, scroll if needed
                if (tty_state.cursor_y > 0) {
                    tty_state.cursor_y--;
                } else {
                    // Would need to implement reverse scroll here
                }
                tty_apply_cursor();
                ansi_parser.state = ANSI_STATE_NORMAL;
            } else if (c == 'D') {
                // Index - move cursor down one line, scroll if needed
                tty_state.cursor_y++;
                tty_scroll_if_needed();
                tty_apply_cursor();
                ansi_parser.state = ANSI_STATE_NORMAL;
            } else if (c == 'E') {
                // Next Line - move to start of next line
                tty_state.cursor_x = 0;
                tty_state.cursor_y++;
                tty_scroll_if_needed();
                tty_apply_cursor();
                ansi_parser.state = ANSI_STATE_NORMAL;
            } else {
                // Unknown escape, treat literally
                ansi_parser.state = ANSI_STATE_NORMAL;
                tty_handle_control(c);
            }
            break;

        case ANSI_STATE_CSI:
            if (c >= '0' && c <= '9') {
                if (ansi_parser.param_count < (sizeof(ansi_parser.params) / sizeof(ansi_parser.params[0]))) {
                    ansi_parser.params[ansi_parser.param_count] = ansi_parser.params[ansi_parser.param_count] * 10 + (c - '0');
                    ansi_parser.param_in_progress = true;
                }
            } else if (c == '?') {
                ansi_parser.private_mode = true;
            } else if (c == '$') {
                ansi_parser.has_dollar = true;
            } else if (c == ';' || c == ':') {
                if (ansi_parser.param_in_progress) {
                    ansi_parser.param_count++;
                    ansi_parser.param_in_progress = false;
                } else {
                    if (ansi_parser.param_count < (sizeof(ansi_parser.params) / sizeof(ansi_parser.params[0]))) {
                        ansi_parser.params[ansi_parser.param_count++] = 0;
                    }
                }
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '`' || c == '~') {
                if (ansi_parser.param_in_progress || ansi_parser.param_count == 0) {
                    ansi_parser.param_count++;
                }
                ansi_parser.final_char = c;
                tty_handle_csi_command(c);
                tty_reset_ansi_parser();
            }
            break;

        case ANSI_STATE_OSC:
            if (c == '\x07' || c == '\x9C') {
                tty_handle_osc_command();
                tty_reset_ansi_parser();
            } else if (c == '\x1B') {
                ansi_parser.state = ANSI_STATE_STRING;
            } else if (!ansi_parser.osc_param_parsed) {
                if (c >= '0' && c <= '9') {
                    ansi_parser.osc_param = ansi_parser.osc_param * 10 + (c - '0');
                } else if (c == ';' || c == ':') {
                    ansi_parser.osc_param_parsed = true;
                } else {
                    ansi_parser.osc_param_parsed = true;
                    if (ansi_parser.string_length < sizeof(ansi_parser.string_buffer) - 1) {
                        ansi_parser.string_buffer[ansi_parser.string_length++] = c;
                    }
                }
            } else if (ansi_parser.string_length < sizeof(ansi_parser.string_buffer) - 1) {
                ansi_parser.string_buffer[ansi_parser.string_length++] = c;
            }
            break;

        case ANSI_STATE_DCS:
            if (c == '\x1B') {
                ansi_parser.state = ANSI_STATE_STRING;
            } else if (c == '\x9C') { // ST
                tty_handle_dcs_command();
                tty_reset_ansi_parser();
            } else if (ansi_parser.string_length < sizeof(ansi_parser.string_buffer) - 1) {
                ansi_parser.string_buffer[ansi_parser.string_length++] = c;
            }
            break;

        case ANSI_STATE_STRING:
            if (c == '\\') { // ESC \ (ST)
                if (ansi_parser.state == ANSI_STATE_STRING) {
                    tty_handle_osc_command();
                    tty_reset_ansi_parser();
                }
            } else {
                ansi_parser.state = ANSI_STATE_NORMAL;
                tty_handle_control(c);
            }
            break;
    }
}

bool tty_init(void) {
    if (!tty_runtime_mutation_allowed()) {
        return false;
    }
    if (!tty_state.initialized) {
        // Initialize with reasonable defaults
        tty_state.cols = 80;
        tty_state.rows = 25;
        tty_init_256_palette();  // Initialize the extended color palette
    }

    // Framebuffer-only TTY - require graphics to be initialized
    if (!graphics_is_initialized()) {
        /* In nofb / text-only mode the graphics subsystem is deliberately
         * disabled.  Forcing graphics_init() here would bring up the
         * framebuffer and defeat the purpose of the nofb boot flag. */
        if (kernel_framebuffer_disabled()) {
            debuglog(DEBUG_INFO, "TTY: nofb mode active, skipping framebuffer TTY init\n");
            return false;
        }
        debuglog(DEBUG_ERROR, "TTY: graphics subsystem required for framebuffer console\n");
        debuglog(DEBUG_INFO, "TTY: Checking V2 graphics status directly...\n");
        
        // Try to get more info about what's available
        extern bool gfx_is_initialized(void);
        extern uint32_t gfx_get_fb_width(void);
        extern uint32_t gfx_get_fb_height(void);
        extern uint32_t gfx_get_fb_bpp(void);
        extern void* gfx_get_fb_addr(void);
        
        if (gfx_is_initialized()) {
            debuglog(DEBUG_INFO, "TTY: V2 graphics IS initialized! Trying to use it directly...\n");
            debuglog(DEBUG_INFO, "TTY: V2 framebuffer: %ux%u %ubpp @ %p\n",
                    gfx_get_fb_width(), gfx_get_fb_height(), gfx_get_fb_bpp(), gfx_get_fb_addr());
            
            // Try to force graphics_init() since V2 is ready
            extern graphics_result_t graphics_init(void);
            graphics_result_t init_result = graphics_init();
            if (init_result == GRAPHICS_SUCCESS) {
                debuglog(DEBUG_INFO, "TTY: Manually initialized legacy graphics manager!\n");
            } else {
                debuglog(DEBUG_ERROR, "TTY: Manual graphics_init() failed\n");
                return false;
            }
        } else {
            debuglog(DEBUG_ERROR, "TTY: V2 graphics is NOT initialized\n");
            return false;
        }
    }

    // Initialize TTY font renderer
    if (tty_font_renderer_init() != TTY_FONT_SUCCESS) {
        debuglog(DEBUG_ERROR, "TTY: failed to initialize TTY font renderer\n");
        return false;
    }

    // Try to set a graphics mode suitable for text rendering
    debuglog(DEBUG_INFO, "TTY: Attempting to set graphics mode for framebuffer console...\n");

    // Test writing directly to framebuffer since graphics_set_mode is broken
    debuglog(DEBUG_INFO, "TTY: Testing direct framebuffer access\n");

    framebuffer_t* fb = NULL;
    graphics_result_t result = graphics_map_framebuffer(&fb);
    debuglog(DEBUG_INFO, "TTY: graphics_map_framebuffer returned %s\n", graphics_get_error_string(result));

    if (result == GRAPHICS_SUCCESS && fb && fb->virtual_addr) {
        debuglog(DEBUG_INFO, "TTY: Framebuffer mapped at 0x%x, size %u bytes, %ux%u\n",
                (uint32_t)(uintptr_t)fb->virtual_addr, (unsigned int)fb->size, fb->width, fb->height);

        // Set up basic framebuffer console with 8x8 font
        tty_state.backend = TTY_BACKEND_FRAMEBUFFER;
        tty_state.cols = fb->width / 8;
        // Reserve space for status bar (24 pixels)
        tty_state.rows = (fb->height - 24) / 8;  // Use 8x8 font height
        tty_state.char_width = 8;
        tty_state.char_height = 8;  // Match 8x8 font

        // Try to update with actual font metrics
        tty_update_dimensions_from_graphics();

        debuglog(DEBUG_INFO, "TTY: framebuffer console size %ux%u, char size %ux%u\n",
                tty_state.cols, tty_state.rows, tty_state.char_width, tty_state.char_height);

        // Unmap framebuffer
        graphics_unmap_framebuffer(fb);

        // Now complete the initialization - allocate cell buffer
        if (!tty_set_dimensions(tty_state.cols, tty_state.rows)) {
            debuglog(DEBUG_ERROR, "TTY: failed to allocate screen buffer\n");
            return false;
        }

        tty_reset_ansi_parser();
        tty_state.initialized = true;
        tty_state.scroll_top = 0;
        tty_state.scroll_bottom = tty_state.rows;

        // Exit boot mode immediately so print() routes to framebuffer from this point.
        // Previously boot_mode was kept true until tty_exit_boot_mode() (called at the
        // very end of kmain), which meant ALL boot messages went to VGA text (0xB8000)
        // and never appeared on the framebuffer display.
        tty_state.boot_mode = false;

        debuglog(DEBUG_INFO, "TTY: framebuffer console fully initialized\n");

        // Initialize virtual terminal buffers for VT 3-12
        tty_init_vt_buffers();

        // Clear screen after full initialization
        debuglog(DEBUG_INFO, "TTY: calling tty_clear...\n");
        tty_clear();
        debuglog(DEBUG_INFO, "TTY: tty_clear done\n");
        return true;
    } else {
        debuglog(DEBUG_ERROR, "TTY: Failed to map framebuffer\n");
    }

    // ----------------------------------------------------------------
    // Fallback: cross-architecture console (serial UART or arch-level
    // framebuffer).  Used when the legacy x86 graphics subsystem is
    // not available (e.g. ARM, AArch64, RISC-V, or nofb mode).
    // ----------------------------------------------------------------
    debuglog(DEBUG_INFO, "TTY: Falling back to cross-architecture console\n");

    if (arch_console_init() != 0) {
        debuglog(DEBUG_ERROR, "TTY: arch_console_init() failed\n");
        return false;
    }

    tty_state.backend = TTY_BACKEND_ARCH_CONSOLE;

    // Derive dimensions from the arch console
    {
        uint32_t ac_rows = 0, ac_cols = 0;
        arch_console_get_size(&ac_rows, &ac_cols);
        tty_state.cols = (ac_cols > 0) ? (uint16_t)ac_cols : 80;
        tty_state.rows = (ac_rows > 0) ? (uint16_t)ac_rows : 25;
    }
    tty_state.char_width = 8;
    tty_state.char_height = 16;

    if (!tty_set_dimensions(tty_state.cols, tty_state.rows)) {
        debuglog(DEBUG_ERROR, "TTY: failed to allocate screen buffer for arch console\n");
        return false;
    }

    tty_reset_ansi_parser();
    tty_state.initialized = true;
    tty_state.boot_mode = false;
    tty_state.scroll_top = 0;
    tty_state.scroll_bottom = tty_state.rows;

    debuglog(DEBUG_INFO, "TTY: arch-console backend ready (%ux%u)\n",
             tty_state.cols, tty_state.rows);

    tty_init_vt_buffers();
    tty_clear();
    return true;
}

void tty_clear(void) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Always reset cursor position first
    tty_state.cursor_x = 0;
    tty_state.cursor_y = 0;

    uint8_t attr = tty_current_attr();

    // Ensure cell buffer exists
    if (!tty_state.cells) {
        tty_set_dimensions(tty_state.cols, tty_state.rows);
    }

    // Clear cell buffer if it exists
    if (tty_state.cells) {
        for (size_t i = 0; i < tty_state.cell_count; i++) {
            tty_state.cells[i].ch = ' ';
            tty_state.cells[i].attr = attr;
            tty_state.cells[i].dirty = 1;  // Mark all as dirty for full redraw
        }
        tty_flush_screen_full();  // Use full redraw for clear operation
    } else {
        if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
            arch_console_clear();
        } else {
            // Fallback: use graphics subsystem for clearing
            graphics_color_t bg = tty_color_from_nibble((attr >> 4) & 0x0F);
            graphics_clear_screen(bg);
        }
    }

    // Always apply cursor position after clearing
    tty_apply_cursor();
}

void tty_force_redraw(void) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Force a full screen redraw without clearing content
    // Used when switching TTY sessions via Ctrl+Alt+Fn
    if (!tty_state.initialized) {
        return;
    }

    // Don't redraw TTY when a graphics app owns the display
    if (tty_state.graphics_app_active) {
        return;
    }

    // Mark all cells as dirty
    if (tty_state.cells) {
        for (size_t i = 0; i < tty_state.cell_count; i++) {
            tty_state.cells[i].dirty = 1;
        }
    }

    // Redraw status bar with current TTY session
    tty_draw_status_bar();

    // Flush entire screen
    tty_flush_screen_full();
}

void tty_putc(char c) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Skip TTY output when a graphics app owns the display
    if (tty_state.graphics_app_active) {
        return;
    }
    if (g_tty_ansi_processing_enabled && g_tty_advanced_mode) {
        tty_process_ansi(c);
    } else {
        tty_handle_control(c);
    }
}

void tty_write(const char* text) {
    if (!text) return;
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Skip TTY output when a graphics app owns the display
    if (tty_state.graphics_app_active) {
        return;
    }
    while (*text) {
        if (g_tty_ansi_processing_enabled && g_tty_advanced_mode) {
            tty_process_ansi(*text++);
        } else {
            tty_handle_control(*text++);
        }
    }
}

void tty_write_ansi(const char* text) {
    if (!text) return;
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    // Skip TTY output when a graphics app owns the display
    if (tty_state.graphics_app_active) {
        return;
    }
    while (*text) {
        if (g_tty_ansi_processing_enabled && g_tty_advanced_mode) {
            tty_process_ansi(*text++);
        } else {
            tty_handle_control(*text++);
        }
    }
}

void tty_puts(const char* text) {
    tty_write(text);
}

void tty_set_attr(uint8_t attr) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    tty_state.fg = attr & 0x0F;
    tty_state.bg = (attr >> 4) & 0x0F;
    tty_state.blink = (attr & TEXT_ATTR_BLINK) != 0;
    tty_state.bold = (attr & TEXT_ATTR_BRIGHT) != 0;
    tty_state.faint = false;
    tty_state.inverse = false;
    tty_state.underline = false;

    // Forward color to cross-architecture console when active
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        // Map 4-bit VGA index to 0x00RRGGBB for arch console
        static const uint32_t vga_to_rgb[16] = {
            0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
            0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
            0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
            0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF,
        };
        uint32_t fg_rgb = vga_to_rgb[tty_state.fg & 0x0F];
        uint32_t bg_rgb = vga_to_rgb[tty_state.bg & 0x07];
        arch_console_set_color(fg_rgb, bg_rgb);
    }
}

uint8_t tty_get_attr(void) {
    return tty_current_attr();
}

void tty_set_color(uint32_t fg, uint32_t bg) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }

    // Map 0x00RRGGBB to nearest 4-bit VGA index for internal state
    static const uint32_t vga_to_rgb[16] = {
        0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
        0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
        0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
        0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF,
    };

    // Find best-match VGA index for fg
    uint8_t best_fg = 7;  // default light gray
    uint32_t best_err = UINT32_MAX;
    for (int i = 0; i < 16; i++) {
        int32_t dr = (int32_t)((fg >> 16) & 0xFF) - (int32_t)((vga_to_rgb[i] >> 16) & 0xFF);
        int32_t dg = (int32_t)((fg >> 8) & 0xFF) - (int32_t)((vga_to_rgb[i] >> 8) & 0xFF);
        int32_t db = (int32_t)(fg & 0xFF) - (int32_t)(vga_to_rgb[i] & 0xFF);
        uint32_t err = (uint32_t)(dr * dr + dg * dg + db * db);
        if (err < best_err) {
            best_err = err;
            best_fg = (uint8_t)i;
        }
    }

    // Find best-match VGA index for bg (lower 8 colors only)
    uint8_t best_bg = 0;  // default black
    best_err = UINT32_MAX;
    for (int i = 0; i < 8; i++) {
        int32_t dr = (int32_t)((bg >> 16) & 0xFF) - (int32_t)((vga_to_rgb[i] >> 16) & 0xFF);
        int32_t dg = (int32_t)((bg >> 8) & 0xFF) - (int32_t)((vga_to_rgb[i] >> 8) & 0xFF);
        int32_t db = (int32_t)(bg & 0xFF) - (int32_t)(vga_to_rgb[i] & 0xFF);
        uint32_t err = (uint32_t)(dr * dr + dg * dg + db * db);
        if (err < best_err) {
            best_err = err;
            best_bg = (uint8_t)i;
        }
    }

    tty_state.fg = best_fg & 0x0F;
    tty_state.bg = best_bg & 0x07;
    tty_state.use_true_colors = false;

    // Forward to cross-architecture console when active
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        arch_console_set_color(fg, bg);
    }

    if (tty_is_ready()) {
        tty_force_redraw();
    }
}

bool tty_uses_graphics_backend(void) {
    /* Return true only when the framebuffer TTY is actually active.
     * In nofb / text-only mode the graphics subsystem is never initialized,
     * so callers that check this before writing via the TTY will correctly
     * fall back to the VGA text console at 0xB8000. */
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        return (arch_console_get_mode() == CONSOLE_MODE_FRAMEBUFFER);
    }
    return tty_state.initialized && graphics_is_initialized();
}

bool tty_try_enable_graphics_backend(void) {
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        return (arch_console_get_mode() == CONSOLE_MODE_FRAMEBUFFER);
    }
    // Graphics backend is always enabled in framebuffer-only TTY
    return graphics_is_initialized();
}

bool tty_is_ready(void) {
    // Boot mode no longer suppresses framebuffer TTY output.
    // tty_init() clears boot_mode as soon as the framebuffer console is ready,
    // so the check below is only meaningful before tty_init() succeeds.
    if (tty_state.boot_mode) {
        return false;
    }
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        return tty_state.initialized;
    }
    return tty_state.initialized && graphics_is_initialized();
}

// Exit boot mode and switch to framebuffer TTY for graphics
// Call this after early boot is complete (e.g., before starting desktop)
void tty_exit_boot_mode(void) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    if (!tty_state.boot_mode) {
        return;  // Already exited boot mode
    }

    /* In nofb / text-only mode the framebuffer TTY was never initialized.
     * Exiting boot mode here would make tty_is_ready() return true (because
     * boot_mode becomes false) but graphics_is_initialized() is still false,
     * causing print()/printch() to skip the VGA text fallback path and
     * produce no output at all.  Stay in boot mode so output keeps going
     * to the VGA text console at 0xB8000. */
    if (kernel_framebuffer_disabled()) {
        debuglog(DEBUG_INFO, "TTY: nofb mode, staying in boot mode (VGA text console)\n");
        return;
    }

    tty_state.boot_mode = false;
    debuglog(DEBUG_INFO, "TTY: Exited boot mode, framebuffer TTY now active\n");

    // Clear and redraw screen with framebuffer TTY (skip if splash is still visible)
    if (tty_state.initialized && !splash_is_running()) {
        tty_clear();
    }

    // Display CPU core dots if graphics is initialized
    if (graphics_is_initialized()) {
        tty_display_cpu_dots();
    }
}

// Check if still in boot mode
bool tty_in_boot_mode(void) {
    return tty_state.boot_mode;
}

// =============================================================================
// GRAPHICS APP MODE - Suppress TTY output when graphical apps own the display
// =============================================================================

// Clear framebuffer directly - used when switching between GUI and TTY
void tty_clear_framebuffer_raw(void) {
    framebuffer_t* fb = NULL;
    if (graphics_map_framebuffer(&fb) != GRAPHICS_SUCCESS || !fb) {
        return;
    }

    if (fb->virtual_addr && fb->size) {
        memset((uint8_t*)fb->virtual_addr, 0, fb->size);
        __asm__ volatile("mfence" ::: "memory");
    }
    graphics_unmap_framebuffer(fb);
}

// Track which process owns the framebuffer display
static uint32_t g_graphics_app_owner_pid = 0;

void tty_set_graphics_app_active(bool active) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    bool was_active = tty_state.graphics_app_active;
    tty_state.graphics_app_active = active;

    if (active && !was_active) {
        // Track the owning process
        g_graphics_app_owner_pid = (current_task) ? current_task->id : 0;
        tty_clear_status_bar();
        debuglog(DEBUG_INFO, "TTY: Graphics app mode enabled (owner PID %u)\n",
                 g_graphics_app_owner_pid);
    } else if (!active && was_active) {
        debuglog(DEBUG_INFO, "TTY: Graphics app mode disabled (was owner PID %u)\n",
                 g_graphics_app_owner_pid);
        g_graphics_app_owner_pid = 0;
        // Restore VGA text buffer visibility if we were in graphics mode
        // and are switching back to text mode
        tty_force_redraw();
    }
}

bool tty_is_graphics_app_active(void) {
    return tty_state.graphics_app_active;
}

uint32_t tty_get_graphics_app_owner(void) {
    return g_graphics_app_owner_pid;
}

void tty_release_graphics_ownership(uint32_t pid) {
    if (g_graphics_app_owner_pid == pid) {
        g_graphics_app_owner_pid = 0;
        tty_state.graphics_app_active = false;
        debuglog(DEBUG_INFO, "TTY: Released graphics ownership from PID %u\n", pid);
    }
}

bool tty_get_dimensions(uint16_t* cols, uint16_t* rows) {
    if (!tty_state.initialized || !tty_state.cells) {
        return false;
    }

    if (cols) {
        *cols = tty_state.cols;
    }
    if (rows) {
        *rows = tty_state.rows;
    }
    return true;
}

// Narrow accessor used by tty_render.c's tty_draw_scroll_indicator() so it
// can position the scroll thumb without reaching into tty.c's file-local
// tty_state directly. Mirrors the early-return this replaced verbatim
// (`!tty_state.cells || tty_state.rows == 0`).
bool tty_internal_get_scroll_state(uint16_t* rows, uint16_t* char_height, uint16_t* cursor_y) {
    if (!tty_state.cells || tty_state.rows == 0) {
        return false;
    }
    if (rows) {
        *rows = tty_state.rows;
    }
    if (char_height) {
        *char_height = tty_state.char_height;
    }
    if (cursor_y) {
        *cursor_y = tty_state.cursor_y;
    }
    return true;
}

// Public-header counterpart of tty_internal_get_scroll_state() above (see
// include/tty_render.h): same file-local tty_state fields, same early
// return when there is no active cell buffer to scroll.
bool tty_render_get_scroll_state(uint16_t* rows, uint16_t* char_height, uint16_t* cursor_y) {
    return tty_internal_get_scroll_state(rows, char_height, cursor_y);
}

bool tty_get_cell_metrics(uint16_t* char_width, uint16_t* char_height) {
    // Cross-architecture console uses fixed 8x16 font
    if (tty_state.backend == TTY_BACKEND_ARCH_CONSOLE) {
        if (char_width) *char_width = 8;
        if (char_height) *char_height = 16;
        return true;
    }

    if (!graphics_is_initialized()) {
        return false;
    }

    uint16_t cw = 8;
    uint16_t ch = 16;
    tty_font_t* tty_font = NULL;

    if (tty_font_load_builtin("tty-8x8", 8, &tty_font) == TTY_FONT_SUCCESS && tty_font) {
        cw = tty_font->width;
        ch = tty_font->height;
    }

    if (char_width) {
        *char_width = cw;
    }
    if (char_height) {
        *char_height = ch;
    }
    return true;
}

bool tty_get_cell(uint16_t x, uint16_t y, char* ch, uint8_t* attr) {
    if (!tty_state.initialized || !tty_state.cells) {
        return false;
    }
    if (x >= tty_state.cols || y >= tty_state.rows) {
        return false;
    }

    tty_cell_t cell = tty_state.cells[tty_cell_index(x, y)];
    if (ch) {
        *ch = cell.ch;
    }
    if (attr) {
        *attr = cell.attr;
    }
    return true;
}

void tty_redraw_region(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    if (!tty_runtime_mutation_allowed()) {
        return;
    }
    if (!tty_state.cells || width == 0 || height == 0) {
        return;
    }

    uint16_t max_x = x + width;
    uint16_t max_y = y + height;
    if (max_x > tty_state.cols) {
        max_x = tty_state.cols;
    }
    if (max_y > tty_state.rows) {
        max_y = tty_state.rows;
    }

    for (uint16_t row = y; row < max_y; row++) {
        for (uint16_t col = x; col < max_x; col++) {
            tty_cell_t cell = tty_state.cells[tty_cell_index(col, row)];
            tty_render_cell(col, row, cell.ch, cell.attr);
        }
    }

    tty_apply_cursor();
}

// =============================================================================
// VIRTUAL TERMINAL (VT) MANAGEMENT
// =============================================================================

bool tty_init_vt_buffers(void) {
    if (!tty_runtime_mutation_allowed()) {
        return false;
    }
    if (g_vt_buffers_initialized) {
        return true;
    }

    uint16_t cols = tty_state.cols;
    uint16_t rows = tty_state.rows;

    if (cols == 0 || rows == 0) {
        debuglog(DEBUG_ERROR, "TTY: Cannot init VT buffers - invalid dimensions %ux%u\n", cols, rows);
        return false;
    }

    size_t cell_count = (size_t)cols * rows;

    // Initialize VT buffers for TTY VTs (3-12)
    for (int i = TTY_FIRST_TTY_VT - 1; i < TTY_LAST_TTY_VT; i++) {
        vt_buffer_t* vt = &g_vt_buffers[i];
        
        vt->cells = (tty_cell_t*)kzalloc(cell_count * sizeof(tty_cell_t));
        if (!vt->cells) {
            debuglog(DEBUG_ERROR, "TTY: Failed to allocate buffer for VT %d\n", i + 1);
            // Clean up already allocated buffers
            for (int j = TTY_FIRST_TTY_VT - 1; j < i; j++) {
                if (g_vt_buffers[j].cells) {
                    kfree(g_vt_buffers[j].cells);
                    g_vt_buffers[j].cells = NULL;
                }
            }
            return false;
        }

        vt->cell_count = cell_count;
        vt->cols = cols;
        vt->rows = rows;
        vt->cursor_x = 0;
        vt->cursor_y = 0;
        vt->saved_cursor_x = 0;
        vt->saved_cursor_y = 0;
        vt->fg = TEXT_ATTR_LIGHT_GRAY;
        vt->bg = TEXT_ATTR_BLACK;
        vt->bold = false;
        vt->faint = false;
        vt->underline = false;
        vt->blink = false;
        vt->inverse = false;
        vt->conceal = false;
        vt->italic = false;
        vt->strike = false;
        vt->double_underline = false;
        vt->overlined = false;
        vt->framed = false;
        vt->encircled = false;
        vt->crossed_out = false;
        vt->true_fg = (graphics_color_t){170, 170, 170, 255};
        vt->true_bg = (graphics_color_t){0, 0, 0, 255};
        vt->use_true_colors = false;
        vt->initialized = true;

        // Initialize cells to spaces
        uint8_t attr = (vt->bg << 4) | (vt->fg & 0x0F);
        for (size_t j = 0; j < cell_count; j++) {
            vt->cells[j].ch = ' ';
            vt->cells[j].attr = attr;
            vt->cells[j].dirty = 1;
        }

        debuglog(DEBUG_INFO, "TTY: Initialized VT %d buffer (%u bytes)\n", i + 1, (unsigned int)(cell_count * sizeof(tty_cell_t)));
    }

    g_vt_buffers_initialized = true;
    g_current_vt = 1;  // Start at VT1 (graphical)
    debuglog(DEBUG_INFO, "TTY: Virtual terminal buffers initialized for VTs %d-%d\n", 
             TTY_FIRST_TTY_VT, TTY_LAST_TTY_VT);
    return true;
}

static void tty_save_current_vt_buffer(void) {
    if (!tty_state.cells || !g_vt_buffers_initialized) {
        return;
    }

    // Only save if current VT is a TTY VT (3-12)
    if (g_current_vt < TTY_FIRST_TTY_VT || g_current_vt > TTY_LAST_TTY_VT) {
        return;
    }

    vt_buffer_t* vt = &g_vt_buffers[g_current_vt - 1];
    if (!vt->cells || vt->cell_count != tty_state.cell_count) {
        return;
    }

    // Copy current TTY state to VT buffer
    memcpy(vt->cells, tty_state.cells, tty_state.cell_count * sizeof(tty_cell_t));
    vt->cursor_x = tty_state.cursor_x;
    vt->cursor_y = tty_state.cursor_y;
    vt->saved_cursor_x = tty_state.saved_cursor_x;
    vt->saved_cursor_y = tty_state.saved_cursor_y;
    vt->fg = tty_state.fg;
    vt->bg = tty_state.bg;
    vt->bold = tty_state.bold;
    vt->faint = tty_state.faint;
    vt->underline = tty_state.underline;
    vt->blink = tty_state.blink;
    vt->inverse = tty_state.inverse;
    vt->conceal = tty_state.conceal;
    vt->italic = tty_state.italic;
    vt->strike = tty_state.strike;
    vt->double_underline = tty_state.double_underline;
    vt->overlined = tty_state.overlined;
    vt->framed = tty_state.framed;
    vt->encircled = tty_state.encircled;
    vt->crossed_out = tty_state.crossed_out;
    vt->true_fg = tty_state.true_fg;
    vt->true_bg = tty_state.true_bg;
    vt->use_true_colors = tty_state.use_true_colors;
}

static void tty_restore_vt_buffer(uint8_t vt_num) {
    if (!g_vt_buffers_initialized) {
        return;
    }

    if (vt_num < TTY_FIRST_TTY_VT || vt_num > TTY_LAST_TTY_VT) {
        return;
    }

    vt_buffer_t* vt = &g_vt_buffers[vt_num - 1];
    if (!vt->cells || !tty_state.cells || vt->cell_count != tty_state.cell_count) {
        return;
    }

    // Restore VT buffer to current TTY state
    memcpy(tty_state.cells, vt->cells, tty_state.cell_count * sizeof(tty_cell_t));
    tty_state.cursor_x = vt->cursor_x;
    tty_state.cursor_y = vt->cursor_y;
    tty_state.saved_cursor_x = vt->saved_cursor_x;
    tty_state.saved_cursor_y = vt->saved_cursor_y;
    tty_state.fg = vt->fg;
    tty_state.bg = vt->bg;
    tty_state.bold = vt->bold;
    tty_state.faint = vt->faint;
    tty_state.underline = vt->underline;
    tty_state.blink = vt->blink;
    tty_state.inverse = vt->inverse;
    tty_state.conceal = vt->conceal;
    tty_state.italic = vt->italic;
    tty_state.strike = vt->strike;
    tty_state.double_underline = vt->double_underline;
    tty_state.overlined = vt->overlined;
    tty_state.framed = vt->framed;
    tty_state.encircled = vt->encircled;
    tty_state.crossed_out = vt->crossed_out;
    tty_state.true_fg = vt->true_fg;
    tty_state.true_bg = vt->true_bg;
    tty_state.use_true_colors = vt->use_true_colors;
}

bool tty_switch_vt(uint8_t vt_number) {
    if (!tty_runtime_mutation_allowed()) {
        return false;
    }
    if (vt_number < 1 || vt_number > TTY_VT_COUNT) {
        debuglog(DEBUG_ERROR, "TTY: Invalid VT number %u (valid: 1-%d)\n", vt_number, TTY_VT_COUNT);
        return false;
    }

    uint8_t old_vt = g_current_vt;
    bool old_is_graphics = (old_vt < TTY_FIRST_TTY_VT);
    bool new_is_graphics = (vt_number < TTY_FIRST_TTY_VT);

    // Show transition screen to prevent black flash
    if (old_is_graphics != new_is_graphics) {
        if (new_is_graphics) {
            // Text → Graphics: show transition before enabling graphics
            graphics_enable_double_buffering(true);
            tty_draw_transition_screen("Switching to desktop...");
        } else {
            // Graphics → Text: show transition then switch to text mode
            tty_draw_transition_screen("Switching to TTY...");
            graphics_enable_double_buffering(false);
        }
    }

    // Fade-out transition
    g_vt_transition_active = true;
    for (uint8_t i = 0; i < VT_TRANSITION_FRAMES; i++) {
        uint8_t opacity = (uint8_t)(((uint16_t)(i + 1) * 255) / VT_TRANSITION_FRAMES);
        tty_draw_fade_overlay(opacity);
        g_vt_transition_frame = i;
    }

    tty_save_current_vt_buffer();
    g_current_vt = vt_number;

    debuglog(DEBUG_INFO, "TTY: Switching from VT %u to VT %u (graphics %d->%d)\n",
             old_vt, vt_number, old_is_graphics, new_is_graphics);

    if (vt_number >= TTY_FIRST_TTY_VT && vt_number <= TTY_LAST_TTY_VT) {
        // Switching to text VT
        tty_set_graphics_app_active(false);
        tty_restore_vt_buffer(vt_number);
        // Ensure framebuffer is available for text rendering
        tty_force_redraw();
        g_vt_transition_active = false;
        return true;
    }

    // Switching to graphical VT (1-2)
    tty_restore_vt_buffer(vt_number);
    // The fade-out above always ends on solid black (last frame is opacity
    // 255, i.e. fully dimmed) - nothing else repaints the actual desktop
    // afterward otherwise, leaving the screen black until/unless the WM's
    // own background render loop task happens to run (it may not be running
    // at all, e.g. no GUI session started yet). Force one immediate
    // repaint so the real desktop content replaces the black transition
    // frame right away instead of leaving that to chance.
    wm_render_loop_tick();
    g_vt_transition_active = false;
    return true;
}

uint8_t tty_get_current_vt(void) {
    return g_current_vt;
}

bool tty_is_active(void) {
    // TTY is active when current VT is a TTY VT (not graphical)
    return (g_current_vt >= TTY_FIRST_TTY_VT && g_current_vt <= TTY_LAST_TTY_VT);
}

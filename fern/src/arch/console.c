/*
 * Fern - Cross-Architecture Console Implementation
 * console.c
 *
 * Provides a unified text console that works with either:
 *   1. Framebuffer + bitmap font rendering (primary)
 *   2. Serial UART output (fallback)
 *
 * The framebuffer path maintains an internal cell buffer and renders
 * characters using the 8x8 bitmap font (font8x8_basic). Scrolling
 * shifts the cell buffer up and redraws.
 *
 * The serial path simply forwards characters to uart_putc().
 */

#include "console.h"
#include "framebuffer.h"
#include "uart.h"
#include "arch.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 8x8 bitmap font (public domain, from graphics/font8x8.c)           */
/* ------------------------------------------------------------------ */
extern const char font8x8_basic[128][8];

/* ------------------------------------------------------------------ */
/* Console state                                                       */
/* ------------------------------------------------------------------ */

#define MAX_COLS 256
#define MAX_ROWS 256

static console_mode_t  g_mode     = CONSOLE_MODE_SERIAL;
static bool            g_initialized = false;

/* Text cell buffer */
static console_cell_t  g_cells[MAX_ROWS][MAX_COLS];
static uint32_t        g_rows     = CONSOLE_DEFAULT_ROWS;
static uint32_t        g_cols     = CONSOLE_DEFAULT_COLS;

/* Cursor */
static uint32_t        g_cur_row  = 0;
static uint32_t        g_cur_col  = 0;

/* Default colors (4-bit VGA indices) */
static uint8_t         g_fg       = 0x0F;  /* white */
static uint8_t         g_bg       = 0x00;  /* black */

/* Framebuffer geometry (pixels) */
static uint32_t        g_fb_width  = 0;
static uint32_t        g_fb_height = 0;
static uint32_t        g_fb_pitch  = 0;

/* Character cell size in pixels */
#define FONT_W  8
#define FONT_H  16

/* ------------------------------------------------------------------ */
/* VGA 16-color palette -> 0x00RRGGBB                                  */
/* ------------------------------------------------------------------ */

static const uint32_t vga_palette[16] = {
    0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
    0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
    0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
    0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

/* Clear the cell buffer */
static void cells_clear(void) {
    for (uint32_t r = 0; r < g_rows; r++) {
        for (uint32_t c = 0; c < g_cols; c++) {
            g_cells[r][c].ch = ' ';
            g_cells[r][c].fg  = g_fg;
            g_cells[r][c].bg  = g_bg;
        }
    }
}

/* Scroll the cell buffer up by one line */
static void cells_scroll_up(void) {
    /* Move rows up */
    memmove(&g_cells[0][0], &g_cells[1][0],
            (g_rows - 1) * g_cols * sizeof(console_cell_t));
    /* Clear bottom row */
    for (uint32_t c = 0; c < g_cols; c++) {
        g_cells[g_rows - 1][c].ch = ' ';
        g_cells[g_rows - 1][c].fg  = g_fg;
        g_cells[g_rows - 1][c].bg  = g_bg;
    }
}

/* ------------------------------------------------------------------ */
/* Framebuffer rendering                                               */
/* ------------------------------------------------------------------ */

/* Render a single cell at (row, col) to the framebuffer */
static void fb_render_cell(uint32_t row, uint32_t col) {
    const console_cell_t* cell = &g_cells[row][col];
    uint32_t px = col * FONT_W;
    uint32_t py = row * FONT_H;

    uint32_t fg_rgb = (cell->fg < 16) ? vga_palette[cell->fg & 0x0F] : 0x00FFFFFF;
    uint32_t bg_rgb = (cell->bg < 16) ? vga_palette[cell->bg & 0x0F] : 0x00000000;

    uint8_t fg_r = (fg_rgb >> 16) & 0xFF;
    uint8_t fg_g = (fg_rgb >>  8) & 0xFF;
    uint8_t fg_b = (fg_rgb >>  0) & 0xFF;
    uint8_t bg_r = (bg_rgb >> 16) & 0xFF;
    uint8_t bg_g = (bg_rgb >>  8) & 0xFF;
    uint8_t bg_b = (bg_rgb >>  0) & 0xFF;

    unsigned char ch = (unsigned char)cell->ch;
    if (ch >= 128) ch = '?';
    const char* glyph = font8x8_basic[ch];

    for (uint32_t y = 0; y < FONT_H; y++) {
        uint8_t row_bits;
        /* The font has 8 rows (8x8). For 16-high cells, duplicate each row. */
        if (y < 8)
            row_bits = (uint8_t)glyph[y];
        else
            row_bits = (uint8_t)glyph[y - 8];

        for (uint32_t x = 0; x < FONT_W; x++) {
            bool on = (row_bits >> x) & 1;
            uint32_t color;
            if (on)
                color = 0xFF000000u | ((uint32_t)fg_r << 16) |
                         ((uint32_t)fg_g << 8) | fg_b;
            else
                color = 0xFF000000u | ((uint32_t)bg_r << 16) |
                         ((uint32_t)bg_g << 8) | bg_b;
            framebuffer_put_pixel(px + x, py + y, color);
        }
    }
}

/* Render the visible portion of the cell buffer to the framebuffer */
static void fb_render_all(void) {
    for (uint32_t r = 0; r < g_rows && r * FONT_H < g_fb_height; r++) {
        for (uint32_t c = 0; c < g_cols && c * FONT_W < g_fb_width; c++) {
            fb_render_cell(r, c);
        }
    }
}

/* Scroll the framebuffer up by one character row (pixel-level copy) */
static void fb_scroll_pixels(void) {
    /* We need to move the framebuffer content up by FONT_H pixels.
     * Since we don't have direct access to the buffer pointer here
     * (it may not be mapped), we re-render from the cell buffer. */
    fb_render_all();
}

/* ------------------------------------------------------------------ */
/* Serial output (fallback)                                            */
/* ------------------------------------------------------------------ */

static void serial_putc(char c) {
    uart_putc(c);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int arch_console_init(void) {
    if (g_initialized)
        return 0;

    /* Reset state */
    g_cur_row = 0;
    g_cur_col = 0;
    g_fg = 0x0F;
    g_bg = 0x00;

    /* Try framebuffer first */
    if (framebuffer_init() == 0 && framebuffer_is_available()) {
        framebuffer_get_info(&g_fb_width, &g_fb_height, &g_fb_pitch, NULL);

        if (g_fb_width > 0 && g_fb_height > 0) {
            g_cols = g_fb_width / FONT_W;
            g_rows = g_fb_height / FONT_H;
            if (g_cols > MAX_COLS) g_cols = MAX_COLS;
            if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
            if (g_cols == 0) g_cols = CONSOLE_DEFAULT_COLS;
            if (g_rows == 0) g_rows = CONSOLE_DEFAULT_ROWS;

            g_mode = CONSOLE_MODE_FRAMEBUFFER;

            /* Map framebuffer into kernel virtual space if not already done.
             * The framebuffer_init() fills physical info; we need the
             * virtual mapping. For now, the arch-level code is expected to
             * have mapped it. If addr is NULL, framebuffer_put_pixel() is a
             * no-op, so we degrade gracefully. */

            cells_clear();
            framebuffer_clear(0x00000000);
            fb_render_all();
            g_initialized = true;
            return 0;
        }
    }

    /* Fallback: serial UART */
    g_mode = CONSOLE_MODE_SERIAL;
    g_cols = CONSOLE_DEFAULT_COLS;
    g_rows = CONSOLE_DEFAULT_ROWS;
    cells_clear();
    g_initialized = true;
    return 0;
}

void arch_console_putc(char c) {
    if (!g_initialized) return;

    /* Control characters */
    if (c == '\n') {
        g_cur_row++;
        g_cur_col = 0;
    } else if (c == '\r') {
        g_cur_col = 0;
    } else if (c == '\t') {
        g_cur_col = (g_cur_col + 8) & ~(uint32_t)7;
    } else if (c == '\b') {
        if (g_cur_col > 0) {
            g_cur_col--;
            g_cells[g_cur_row][g_cur_col].ch = ' ';
            g_cells[g_cur_row][g_cur_col].fg = g_fg;
            g_cells[g_cur_row][g_cur_col].bg = g_bg;
        }
    } else {
        if (g_cur_col < g_cols && g_cur_row < g_rows) {
            g_cells[g_cur_row][g_cur_col].ch = c;
            g_cells[g_cur_row][g_cur_col].fg = g_fg;
            g_cells[g_cur_row][g_cur_col].bg = g_bg;
        }
        g_cur_col++;
    }

    /* Line wrap */
    if (g_cur_col >= g_cols) {
        g_cur_col = 0;
        g_cur_row++;
    }

    /* Scroll if needed */
    if (g_cur_row >= g_rows) {
        cells_scroll_up();
        g_cur_row = g_rows - 1;

        if (g_mode == CONSOLE_MODE_FRAMEBUFFER) {
            fb_scroll_pixels();
        }
    }

    /* Render / output */
    if (g_mode == CONSOLE_MODE_FRAMEBUFFER) {
        fb_render_cell(g_cur_row, g_cur_col > 0 ? g_cur_col - 1 : 0);
        if (c == '\n' || c == '\r') {
            /* Re-render entire screen for line changes */
            fb_render_all();
        }
    } else {
        serial_putc(c);
    }
}

void arch_console_puts(const char* str) {
    if (!str) return;
    while (*str) {
        arch_console_putc(*str++);
    }
}

void arch_console_clear(void) {
    if (!g_initialized) return;

    g_cur_row = 0;
    g_cur_col = 0;
    cells_clear();

    if (g_mode == CONSOLE_MODE_FRAMEBUFFER) {
        framebuffer_clear(0x00000000);
        fb_render_all();
    }
}

void arch_console_set_color(uint32_t fg, uint32_t bg) {
    /* Map 0x00RRGGBB to nearest VGA index for internal storage */
    if (fg < 16)
        g_fg = (uint8_t)fg;
    else
        g_fg = 0x0F; /* white fallback */

    if (bg < 16)
        g_bg = (uint8_t)bg;
    else
        g_bg = 0x00; /* black fallback */
}

void arch_console_get_size(uint32_t* rows, uint32_t* cols) {
    if (rows) *rows = g_rows;
    if (cols) *cols = g_cols;
}

void arch_console_set_cursor(uint32_t row, uint32_t col) {
    if (row < g_rows) g_cur_row = row;
    if (col < g_cols) g_cur_col = col;
}

void arch_console_get_cursor(uint32_t* row, uint32_t* col) {
    if (row) *row = g_cur_row;
    if (col) *col = g_cur_col;
}

console_mode_t arch_console_get_mode(void) {
    return g_mode;
}

void arch_console_flush(void) {
    if (!g_initialized) return;
    if (g_mode == CONSOLE_MODE_FRAMEBUFFER) {
        fb_render_all();
    }
}

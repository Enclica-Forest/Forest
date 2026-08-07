/*
 * Fern - Cross-Architecture Console Interface
 * console.h
 *
 * Unified console API that works across all supported architectures:
 *   - With framebuffer: text-mode rendering using bitmap font (8x16)
 *   - Without framebuffer: serial UART output fallback
 *
 * Features:
 *   - Character and string output with control character handling
 *   - Scrolling when screen is full
 *   - Color support (foreground/background via 0x00RGB format)
 *   - Dimension query
 *
 * Note: This is the cross-architecture console, distinct from the
 * legacy x86 VGA text-mode console in screen.h/screen.c.
 */

#ifndef FOREST_ARCH_CONSOLE_H
#define FOREST_ARCH_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Default console dimensions (text cells) */
#define CONSOLE_DEFAULT_COLS  80
#define CONSOLE_DEFAULT_ROWS  25

/* Character cell: glyph + packed color */
typedef struct {
    char     ch;
    uint8_t  fg;        /* foreground index (0-15) or RGB888 low byte */
    uint8_t  bg;        /* background index (0-15) or RGB888 low byte */
} console_cell_t;

/* Console operating mode */
typedef enum {
    CONSOLE_MODE_SERIAL = 0,    /* UART serial output only */
    CONSOLE_MODE_FRAMEBUFFER,   /* Framebuffer text rendering */
} console_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * arch_console_init() - Initialize the console subsystem.
 *
 * Auto-detects available output (framebuffer > serial) and sets up
 * internal state. Must be called once during early boot before any
 * console output.
 *
 * Returns 0 on success, -1 if no output method is available.
 */
int arch_console_init(void);

/**
 * arch_console_putc() - Output a single character.
 *
 * Handles control characters: \n, \r, \t, \b.
 * If a framebuffer is active, the character is written to the
 * internal cell buffer and rendered.
 *
 * @c: Character to output.
 */
void arch_console_putc(char c);

/**
 * arch_console_puts() - Output a NUL-terminated string.
 *
 * @str: String to output.
 */
void arch_console_puts(const char* str);

/**
 * arch_console_clear() - Clear the entire screen.
 */
void arch_console_clear(void);

/**
 * arch_console_set_color() - Set default text color.
 *
 * @fg: Foreground color (0x00RRGGBB or 4-bit VGA index).
 * @bg: Background color (0x00RRGGBB or 4-bit VGA index).
 */
void arch_console_set_color(uint32_t fg, uint32_t bg);

/**
 * arch_console_get_size() - Get current console dimensions in text cells.
 *
 * @rows: Output pointer for row count (may be NULL).
 * @cols: Output pointer for column count (may be NULL).
 */
void arch_console_get_size(uint32_t* rows, uint32_t* cols);

/**
 * arch_console_set_cursor() - Set cursor position in text cells.
 *
 * @row: Row (0-indexed).
 * @col: Column (0-indexed).
 */
void arch_console_set_cursor(uint32_t row, uint32_t col);

/**
 * arch_console_get_cursor() - Get current cursor position.
 *
 * @row: Output pointer (may be NULL).
 * @col: Output pointer (may be NULL).
 */
void arch_console_get_cursor(uint32_t* row, uint32_t* col);

/**
 * arch_console_get_mode() - Query the active console output mode.
 */
console_mode_t arch_console_get_mode(void);

/**
 * arch_console_flush() - Flush pending framebuffer updates to display.
 *
 * Only meaningful in CONSOLE_MODE_FRAMEBUFFER. No-op in serial mode.
 */
void arch_console_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* FOREST_ARCH_CONSOLE_H */

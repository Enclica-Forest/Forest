#ifndef TEXT_CONSOLE_H
#define TEXT_CONSOLE_H

/*
 * text_console.h - Text-mode (VGA/TTY) console API summary.
 *
 * This is the no-framebuffer fallback display path. When HAS_FRAMEBUFFER
 * is 0 (or FB_FORCE_TEXT_MODE is set), all kernel output goes through
 * this API. The implementation lives in screen.c (VGA 0xb8000 direct
 * memory) with TTY rendering handled by tty.c / tty_render.c.
 *
 * The functions below are declared in screen.h and tty.h; this header
 * merely collects the text-mode-relevant subset so no-fb code has a
 * single inclusion point and never needs to reference graphics headers.
 */

#include <stdint.h>
#include <stdbool.h>
#include "screen.h"
#include "tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------*/
void console_init(void);           /* screen.h */
bool tty_init(void);               /* tty.h   */

/* -----------------------------------------------------------------------
 * Cursor
 * ---------------------------------------------------------------------*/
void updateCursor(void);           /* screen.h */

/* -----------------------------------------------------------------------
 * Character / string output
 * ---------------------------------------------------------------------*/
void printch(char c);              /* screen.h */
void print(const char* ch);        /* screen.h */
void printl(const char* ch);       /* screen.h */
void print_hex(uint32_t value);    /* screen.h */
void print_dec(uint32_t value);    /* screen.h */
void tty_putc(char c);             /* tty.h */
void tty_write(const char* text);  /* tty.h */

/* Put a single character at an absolute cell coordinate. */
void tui_set_char_at(int x, int y, char c, int fg_color, int bg_color); /* screen.h */

/* -----------------------------------------------------------------------
 * Color
 * ---------------------------------------------------------------------*/
void set_screen_color(int text_color, int bg_color);          /* screen.h */
void set_screen_color_from_color_code(int color_code);        /* screen.h */
void print_colored(const char* ch, int text_color, int bg_color); /* screen.h */
void tty_set_attr(uint8_t attr);                               /* tty.h */

/* -----------------------------------------------------------------------
 * Scroll / clear
 * ---------------------------------------------------------------------*/
void clearScreen(void);            /* screen.h */
void scrollUp(uint16_t lineNumber); /* screen.h */
void tty_clear(void);              /* tty.h */
void tty_force_redraw(void);       /* tty.h */

/* -----------------------------------------------------------------------
 * Dimensions
 * ---------------------------------------------------------------------*/
extern uint16 screen_width, screen_height;  /* screen.h */
bool tty_get_dimensions(uint16_t* cols, uint16_t* rows);  /* tty.h */

#ifdef __cplusplus
}
#endif

#endif /* TEXT_CONSOLE_H */

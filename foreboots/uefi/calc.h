/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/calc.h - fixed-point calculator + function grapher (TEMPLATE B window).
 * =============================================================================
 * A decimal calculator with graphing. SSE/x87 are disabled in this build, so
 * "floating point" is SOFTWARE Q32.32 signed fixed-point carried in an INT64
 * (~9 decimal digits, range +/-2.1e9); no float/double is used anywhere.
 *
 * Expressions support: + - * / % (remainder), parentheses, unary minus,
 * decimal literals ("3.14", ".5"), ^ (integer powers), the variable x, the
 * constants pi and e, and the functions sin cos tan sqrt abs. Precedence is
 * the usual ^ > * / % > + - with ^ right-associative. Errors shown: /0,
 * overflow, domain (sqrt of a negative, non-integer exponent), syntax.
 *
 * The GRA button (or 'g') opens a second window plotting y = f(x) for the
 * SAME expression, updated live as the expression is edited (typing works in
 * either window). The grapher has an adaptive 1/2/5 grid with axes and tick
 * labels; arrows pan, PgUp/PgDn or the mouse wheel zoom (wheel zooms around
 * the cursor), clicking re-centres, shift-R resets, Esc (or 'g') closes. A
 * trace readout shows x/y at the cursor.
 *
 * Input comes from an on-screen button grid (mouse clicks, hit-tested in the
 * event callback) AND the keyboard (digits, + - * / % ^ ( ) . and letters for
 * function names, Enter/= evaluates, Esc closes, Backspace deletes, shift-C
 * clears, 'g' opens the graph).
 *
 * Freestanding (no libc, no heap): the whole tool lives in fixed static state
 * reached from the callbacks via wm_user(). Runs entirely before
 * ExitBootServices.
 * ========================================================================== */
#ifndef FOREB_UEFI_CALC_H
#define FOREB_UEFI_CALC_H

/* Open the Calculator window (TEMPLATE B). Returns immediately; the bootx64.c
 * menu loop drives the window via its draw/event callbacks. Re-opening while it
 * is already open is a no-op. */
void tool_calc_open(void);

/* Open (or no-op if already open) the graph window for the calculator's
 * current expression. Also bound to the GRA button and the 'g' key. */
void tool_calc_graph_open(void);

#endif /* FOREB_UEFI_CALC_H */

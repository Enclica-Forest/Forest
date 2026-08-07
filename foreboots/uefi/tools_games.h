/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_games.h - "Games" tool category: playable mini-games.
 * =============================================================================
 * Ten self-contained template-B window games (see tools.h DESIGN CONTRACT).
 * Pure compute/draw: integer math only (no float - SSE/x87 disabled), fixed
 * buffers, no heap, no firmware services (NO init function). Each game gets its
 * per-frame tick from repeated draw-callback invocations by the menu loop.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_GAMES_H
#define FOREB_UEFI_TOOLS_GAMES_H

#include "tools.h"   /* struct forebo_tool */

/* Individual game open() callbacks (template B: open a wm window + return). */
void tool_games_snake_open(void);       /* Snake                              */
void tool_games_pong_open(void);        /* Pong vs simple AI                  */
void tool_games_ttt_open(void);         /* Tic-Tac-Toe vs AI (minimax)        */
void tool_games_2048_open(void);        /* 2048 sliding tiles                 */
void tool_games_mines_open(void);       /* Minesweeper (9x9)                  */
void tool_games_breakout_open(void);    /* Breakout                          */
void tool_games_life_open(void);        /* Conway's Game of Life              */
void tool_games_simon_open(void);       /* Simon memory game                  */
void tool_games_dice_open(void);        /* Animated dice roller               */
void tool_games_whack_open(void);       /* Whack / reaction timer             */

/* Category exports (consumed by uefi/tools_registry.c). */
extern const struct forebo_tool cat_games_tools[];
extern const int                cat_games_count;

#endif /* FOREB_UEFI_TOOLS_GAMES_H */

/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_rng.h - "Random & Security" tool category (KEY = rng).
 * =============================================================================
 * A group of self-contained, template-B wm.c windows: RNG, hashes + generators.
 * Entropy comes from RDRAND when CPUID advertises it, else a TSC-seeded /
 * TSC-mixed xorshift64 fallback. All integer / fixed math (no libc, no float,
 * no heap for the pure tools; the file-CRC path uses a fixed stack buffer).
 *
 * Freestanding, pre-ExitBootServices. See tools_cat.h for the category contract.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_RNG_H
#define FOREB_UEFI_TOOLS_RNG_H

#include "tools.h"   /* struct forebo_tool, efi/wm/ui/input */

/* Store gST/gBS/gRT for the tools that need firmware services (Stall / a
 * filesystem to CRC a file). Pure compute tools ignore the stored globals.
 * Call once (clock.c idiom) before any rng tool is opened. */
void cat_rng_init(EFI_SYSTEM_TABLE *st);

/* The individual tool open() functions (template B). */
void tool_rng_passwd_open(void);    /* Password generator (RDRAND/TSC entropy)  */
void tool_rng_coin_open(void);      /* Coin flip + running tally                 */
void tool_rng_magic8_open(void);    /* Magic 8-ball answer oracle                */
void tool_rng_crc32_open(void);     /* CRC32 of a typed string or a file         */
void tool_rng_fnv_open(void);       /* FNV-1a 32/64-bit hash of a string         */
void tool_rng_uuid_open(void);      /* GUID / UUIDv4 generator                   */
void tool_rng_dice_open(void);      /* RPG dice roller (NdM+K)                    */
void tool_rng_dicestat_open(void);  /* Dice statistics / histogram               */
void tool_rng_guess_open(void);     /* Number guesser game                       */

/* Category table exports (defined in tools_rng.c). */
extern const struct forebo_tool cat_rng_tools[];
extern const int                cat_rng_count;

#endif /* FOREB_UEFI_TOOLS_RNG_H */

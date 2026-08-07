/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_registry.c - Aggregate tool-CATEGORY registry.
 * =============================================================================
 * Lists every tool category for the 2-level Tools launcher (see tools_cat.h).
 * Row 0 wraps the original flat forebo_tools[] (core disk/boot/system tools);
 * the remaining rows come from the category modules (uefi/tools_<key>.c).
 *
 * All .count fields are patched at runtime by tools_categories_init(): the
 * per-category cat_<key>_count / forebo_tools_count are `const int` globals,
 * which are NOT constant expressions in C, so they cannot initialize a
 * file-scope aggregate. tools_categories_init() also runs each category's
 * cat_<key>_init(st) exactly once (categories that need firmware services).
 *
 * Freestanding, pre-ExitBootServices. NO libc, NO float (-mno-sse).
 * ========================================================================== */
#include "tools_cat.h"

#include "tools_convert.h"
#include "tools_datetime.h"
#include "tools_games.h"
#include "tools_gfx.h"
#include "tools_hw.h"
#include "tools_math.h"
#include "tools_rng.h"
#include "tools_text.h"
#include "tools_toys.h"

struct forebo_tool_category forebo_categories[] = {
    /* name                desc                                  icon       tools                count: patched in init */
    { "Core Tools",        "Disk, boot & system utilities",      "disk",     forebo_tools,        0 },
    { "Converters",        "Number, text & unit converters",     "gear",     cat_convert_tools,   0 },
    { "Games",             "Playable mini-games",                "terminal", cat_games_tools,     0 },
    { "Graphics Demos",    "Animated visual demos",              "os",       cat_gfx_tools,       0 },
    { "Math",              "Number theory & calculators",        "gear",     cat_math_tools,      0 },
    { "Time & Date",       "Clocks, timers, calendars",          "gear",     cat_datetime_tools,  0 },
    { "Text Tools",        "Editors & string utilities",         "text",     cat_text_tools,      0 },
    { "Hardware & Diag",   "CPU, PCI, ACPI, memory diagnostics", "disk",     cat_hw_tools,        0 },
    { "Random & Security", "RNG, hashes, generators",            "safe",     cat_rng_tools,       0 },
    { "Toys & Audio",      "Fun toys, paint, sound",             "os",       cat_toys_tools,      0 },
};

const int forebo_categories_count =
    (int)(sizeof(forebo_categories) / sizeof(forebo_categories[0]));

/* Count sources in the SAME order as forebo_categories[] rows. */
static const int *const cat_count_src[] = {
    &forebo_tools_count,
    &cat_convert_count,
    &cat_games_count,
    &cat_gfx_count,
    &cat_math_count,
    &cat_datetime_count,
    &cat_text_count,
    &cat_hw_count,
    &cat_rng_count,
    &cat_toys_count,
};

void tools_categories_init(EFI_SYSTEM_TABLE *st)
{
    static int done;
    int i, n;

    if (done) return;
    done = 1;

    /* Patch every row's count from its source (order must match the table). */
    n = (int)(sizeof(cat_count_src) / sizeof(cat_count_src[0]));
    if (n > forebo_categories_count) n = forebo_categories_count;
    for (i = 0; i < n; i++)
        forebo_categories[i].count = *cat_count_src[i];

    /* Categories needing firmware services (RTC, ESP writes, Stall, ...).
     * Their init fns are NULL-safe; convert/games/gfx/math need none. */
    cat_datetime_init(st);
    cat_text_init(st);
    cat_hw_init(st);
    cat_rng_init(st);
    cat_toys_init(st);
}

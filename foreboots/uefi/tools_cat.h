/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_cat.h - Tool CATEGORY registry (for the 2-level Tools launcher).
 * =============================================================================
 * The Tools launcher shows a list of CATEGORIES; selecting one drills into that
 * category's list of `struct forebo_tool` (see tools.h). Backspace/Esc goes back
 * up to the category list. This keeps a large tool set navigable.
 *
 * Each category lives in its own module (uefi/tools_<key>.c + .h). That module
 * defines its tools' template-B open() functions and exports its table:
 *
 *     const struct forebo_tool cat_<key>_tools[] = {
 *         { "Name", "one-line desc", "icon", tool_<key>_<x>_open }, ...
 *     };
 *     const int cat_<key>_count = (int)(sizeof(cat_<key>_tools)/sizeof(cat_<key>_tools[0]));
 *
 * The aggregate table forebo_categories[] (in uefi/tools_registry.c) lists every
 * category, including the original flat forebo_tools[] as the first category.
 * Freestanding, pre-ExitBootServices, fixed pools. NO libc, NO float (-mno-sse).
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_CAT_H
#define FOREB_UEFI_TOOLS_CAT_H

#include "tools.h"   /* struct forebo_tool */

/* One category = a named, icon'd group of tools. */
struct forebo_tool_category {
    const char               *name;   /* category label shown in the launcher   */
    const char               *desc;   /* one-line description                    */
    const char               *icon;   /* short icon name (tools_icon_path)       */
    const struct forebo_tool *tools;  /* this category's tool table              */
    int                       count;  /* number of tools in it                   */
};

/* The aggregate registry (defined in uefi/tools_registry.c).
 * Non-const: row 0 wraps the flat forebo_tools[] whose count is only known at
 * runtime, so tools_categories_init() patches forebo_categories[0].count once. */
extern struct forebo_tool_category       forebo_categories[];
extern const int                         forebo_categories_count;

/* One-time registry init: patch row-0 count + run each category's cat_<key>_init.
 * Called from tools_init() (tools.c). Safe to call more than once. */
void tools_categories_init(EFI_SYSTEM_TABLE *st);

#endif /* FOREB_UEFI_TOOLS_CAT_H */

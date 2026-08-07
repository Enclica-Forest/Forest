/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_text.h - "Text Tools" category: editors & string utilities.
 * =============================================================================
 * Eight self-contained template-B GUI tools. Only Notepad needs firmware
 * services (ESP EFI_FILE writes); the rest are pure integer compute/draw.
 * See the CATEGORY MODULE CONTRACT in tools_cat.h.
 * ========================================================================== */
#ifndef FOREB_UEFI_TOOLS_TEXT_H
#define FOREB_UEFI_TOOLS_TEXT_H

#include "tools.h"   /* struct forebo_tool, efi.h, wm.h, ui.h, input.h */

/* Store gST/gBS/gRT for the tools that touch firmware (Notepad). Call once,
 * clock.c-style, before any text tool is opened. NULL-safe. */
void cat_text_init(EFI_SYSTEM_TABLE *st);

/* The eight tools (template B: open one wm window, return immediately). */
void tool_text_notepad_open(void);    /* editable buffer, save to ESP notes    */
void tool_text_banner_open(void);     /* big block-letter ASCII banner          */
void tool_text_hex_open(void);        /* text <-> hex bytes converter           */
void tool_text_count_open(void);      /* char / word / line counter             */
void tool_text_transform_open(void);  /* reverse / UPPER / lower / Title        */
void tool_text_lorem_open(void);      /* canned-word + LCG filler generator     */
void tool_text_morse_open(void);      /* Morse encode / decode                  */
void tool_text_find_open(void);       /* find + highlight in a sample text      */

/* Category export (CATEGORY MODULE CONTRACT). */
extern const struct forebo_tool cat_text_tools[];
extern const int                cat_text_count;

#endif /* FOREB_UEFI_TOOLS_TEXT_H */

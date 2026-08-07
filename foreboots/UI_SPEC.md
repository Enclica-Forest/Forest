# ForeB Shared UI Specification

A single visual design implemented by **two** renderers so BIOS and UEFI look
pixel-identical:

- **BIOS**: 8bpp palette-indexed LFB, drawn from stage2 (`draw_*` routines).
- **UEFI**: 32bpp GOP framebuffer, drawn by direct writes to `fb_base`.

Both consume the same assets:

- `include/font8x8.h` - 8x8 font, 96 glyphs (ASCII 0x20..0x7F), **LSB-first**
  (bit 0 = leftmost pixel). See the header for the mandatory render loop.
- `include/forebo_theme.h` - forest colors (`0x00RRGGBB`) + layout constants
  (absolute BIOS-reference pixels **and** screen-fraction values for UEFI).

The two hard rules of this spec:

1. **Never scroll.** No per-line text-console output on the graphics path. All
   status is drawn in place (fixed rectangles repainted), never appended.
2. **Detailed logs go to serial (COM1) only.** The screen shows one status
   line + one progress bar + a percentage. Everything verbose is on serial.

---

## 1. Screen coordinate model

- Origin top-left, +x right, +y down, pixels.
- BIOS reference resolution is 800x600 (`FOREB_REF_W/H`). Absolute constants in
  `forebo_theme.h` (`FOREB_MENU_X`, etc.) target this.
- UEFI runs at whatever GOP mode is active. It uses the **fractional** constants
  (`FOREB_F_*`) multiplied by `fb_w`/`fb_h` and rounded, so the identical layout
  scales to any resolution. Text is drawn with the same 8x8 font; on large
  framebuffers glyphs may be integer-scaled (2x) for legibility - scale factor
  is a renderer choice, geometry stays fractional.

---

## 2. Screen regions (top to bottom)

```
+----------------------------------------------------------------+
|                    (background: forest gradient)               |
|  ------------------------------------------------------------  |  title rule
|                     ForeB - Forest Bootloader                  |  title
|                     Forest OS Boot Manager                     |  subtitle (dim)
|                             /\                                  |
|                            /##\      (tree logo)               |
|                           /####\                               |
|                          /######\                              |
|                            ||||         (trunk)                |
|                          ^^^^^^^^        (ground)              |
|         +------------------------------------------+           |
|         | [ Boot Menu ]                            |           |  panel label
|         | -----------------------------------------|           |  panel rule
|         | > Forest OS (default)                    |  <- sel   |  entries
|         |     Standard boot                        |           |
|         |   Forest OS (no framebuffer)             |           |
|         |     VGA text mode, no VBE LFB            |           |
|         |   Forest OS (safe mode)                  |           |
|         |   Reboot                                 |           |
|         |                                          |           |
|         |  [######################........]  62%   |  <- load  |  progress
|         |  Loading kernel...                       |           |  status line
|         +------------------------------------------+           |
|                                          Auto-boot in 3 sec    |  timer (amber)
|  ------------------------------------------------------------  |  footer rule
|  [Up/Down] Navigate  [Enter] Boot  [Esc] Reset                 |  footer hint
+----------------------------------------------------------------+
```

### 2.1 Background
Full-screen fill. Preferred: vertical gradient from `FOREB_BG_TOP` (top) to
`FOREB_BG_BOTTOM` (bottom); flat `FOREB_BG` is acceptable if a gradient is too
costly on the BIOS path.

### 2.2 Title bar
- 2px horizontal rule in `FOREB_BORDER` at `y = FOREB_TITLEBAR_Y`, spanning
  `x = margin .. w - margin`.
- Title `FOREB_TITLE_STR` in `FOREB_TITLE`, centered.
- Subtitle `FOREB_SUBTITLE_STR` in `FOREB_DIM`, centered under the title.

### 2.3 Tree logo
Centered near `FOREB_F_LOGO_CX/CY`. Either:
- ASCII-art rows using the font (`/\`, `/##\`, `/####\`, `/######\`, `||||`,
  `^^^^^^^^`) in `TREE2`/`TREE3` foliage, `TREE1` trunk, `FOREB_DIM` ground; or
- filled triangles (`TREE2`/`TREE3`) + a `TREE1` trunk rectangle for a crisper
  look. Either is spec-compliant; keep it inside the logo box.

### 2.4 Menu panel
- Filled rect `FOREB_PANEL` at the panel rect.
- 1px outline `FOREB_BORDER` offset -2px around it.
- Label `FOREB_PANEL_LABEL` in `FOREB_TITLE` near top-left inside the panel.
- 1px `FOREB_BORDER` rule under the label.

### 2.5 Boot entries
`FOREB_BOOT_ENTRY_COUNT` (4) rows, `entry_height` apart.

Entry i, row-top `y`:
- **Selected**: fill highlight bar `FOREB_SELECT` across the panel interior;
  draw a `FOREB_WHITE` `>` glyph, then the label (also `FOREB_WHITE`).
- **Unselected**: no bar; label in `FOREB_TEXT`, indented where the `>` would be.
- Description line under the label in `FOREB_DIM`.

Labels: `Forest OS (default)`, `Forest OS (no framebuffer)`,
`Forest OS (safe mode)`, `Reboot`.
Descriptions: `Standard boot`, `VGA text mode, no VBE LFB`,
`Minimal safe-mode boot`, `Restart the system`.

### 2.6 Countdown timer
Amber (`FOREB_TIMER`) `Auto-boot in N sec`. **Only its own small rectangle is
repainted each second** (erase to background, redraw) - the rest of the menu is
untouched. This partial-repaint is the model for the progress bar. Timer is
cancelled on any keypress.

### 2.7 Footer
- 1px `FOREB_BORDER` rule near the bottom.
- Hint `FOREB_FOOTER_HINT` in `FOREB_DIM`.

---

## 3. Load progress model (replaces scrolling status text)

**Problem being fixed:** on UEFI, every `ConOut` newline on a hi-res GOP text
console memmoves the whole screen -> visible slow scroll during kernel load.
The BIOS path never scrolled, but must follow the same in-place model.

**The model** - three fixed, in-place elements, no scrolling ever:

1. **One progress bar** at the progress rect (`FOREB_F_PROGRESS_*` / the BIOS
   `FOREB_PROGRESS_*`):
   - Draw the track once in `FOREB_PROGRESS_TRACK` with a 1px `FOREB_BORDER`
     outline.
   - As work advances, fill `0..W` px of the interior in `FOREB_PROGRESS_FILL`
     proportional to `done/total`. Only repaint the changed span; never clear
     and never move the bar.
2. **One status line** (single line, in `FOREB_TEXT`) just above the bar. To
   change the message, **erase its rectangle to background and redraw** - do
   not append a new line. Messages are short: `Loading kernel...`,
   `Staging segments...`, `Starting Forest OS...`.
3. **One percentage** (`NN%`) drawn at the right end of the bar in
   `FOREB_TITLE`, repainted in place when it changes.

**Serial-only detail:** all verbose logging (GOP geometry, ELF class/entry,
per-segment paddr/filesz, E820 count, ExitBootServices status, handoff summary)
stays on COM1 via `serial_puts`/`serial_puthex`. None of it touches the screen.

**Phases and where the bar advances:**

| Phase                    | Fraction of bar | Notes                                    |
|--------------------------|-----------------|------------------------------------------|
| File read (chunked)      | 0 -> 80%        | UEFI: split the single `kf->Read` into ~64 KiB chunks; tick per chunk using `fsize`. Pre-ExitBootServices, framebuffer writable directly. |
| PT_LOAD staging (copy)   | 80 -> 100%      | Per program-header. **Post-ExitBootServices on UEFI: direct framebuffer writes only** - no ConOut, no GOP Blt, no firmware calls. |
| Handoff                  | 100% + status   | Status line -> `Starting Forest OS...`, then jump to kernel. |

**BIOS note:** kernel load is already silent; add the same fixed progress rect
inside the panel and tick it during `load_kernel`/`stream_segment`. Keep the
`draw_timer_display` partial-repaint discipline.

**Timing note (UEFI):** the framebuffer stays valid across ExitBootServices via
the raw `fb_base` MMIO address, so the bar can be advanced both before and after
firmware is released. Do not call any `gBS`/`gST`/`gop` service after
ExitBootServices.

---

## 4. Renderer API contract

Both renderers implement the same five primitives against their own framebuffer.
C signatures below; the BIOS asm provides equivalents (`plot_px`, `fill_box`,
`draw_glyph`, `gprint`). All colors are `0x00RRGGBB` (BIOS maps to palette
index; UEFI stores packed per PixelFormat, applying `foreb_swap_rb` if RGBX).

```c
/* One pixel. Bounds-checked; out-of-range is a no-op. */
void put_pixel(int x, int y, uint32_t color);

/* Filled axis-aligned rectangle [x, x+w) x [y, y+h). */
void fill_rect(int x, int y, int w, int h, uint32_t color);

/* One 8x8 glyph at (x,y). Renders font8x8 LSB-first (bit0 = leftmost).
   Background is transparent (only lit pixels are drawn) unless bg != -1,
   in which case the 8x8 cell is filled with bg first. */
void draw_char(int x, int y, unsigned char c, uint32_t fg, int32_t bg);

/* NUL-terminated string, 8 px advance per char, no wrapping. Returns the
   x just past the last glyph. */
int  draw_string(int x, int y, const char *s, uint32_t fg, int32_t bg);

/* In-place progress bar. Draws/updates the track+outline and fills
   interior to (done/total). pct is drawn at the right if show_pct.
   Repaints only what changed; never scrolls. */
void draw_progress(int x, int y, int w, int h,
                   uint32_t done, uint32_t total,
                   uint32_t fill, uint32_t track, int show_pct);
```

Derived helpers a renderer may add (all built from the five above):
`draw_hline`/`draw_vline` (1px `fill_rect`), `rect_outline` (four rules),
`draw_string_centered` (compute `x = (screen_w - 8*len)/2`).

**Text scaling (UEFI):** an optional integer `scale` multiplies glyph size;
implement by drawing each lit font pixel as a `scale x scale` `fill_rect`.
Geometry stays fractional so the layout is unchanged.

---

## 5. Color / pixel-format notes

- BIOS 8bpp: program DAC registers 16..28 from the RGB triples (already done in
  `program_palette` when `screen_bpp == 8`); draw with palette indices
  `FOREB_BG..FOREB_TREE3`.
- UEFI 32bpp: no palette. Honor `mi->PixelFormat`:
  - `PixelBlueGreenRedReserved8BitPerColor` (x86 default, BGRX): store the
    `0x00RRGGBB` value as-is.
  - `PixelRedGreenBlueReserved8BitPerColor` (RGBX): store
    `foreb_swap_rb(color)`.
  - `PixelBitMask`: derive shifts from the mask (rare; fall back to BGRX).
- Progress fill = `FOREB_PROGRESS_FILL` (leaf green), track =
  `FOREB_PROGRESS_TRACK` (dark border green).

---

## 6. Input (UEFI menu)

- Poll `gST->ConIn->ReadKeyStroke` (requires the `EFI_INPUT_KEY` /
  `EFI_SIMPLE_TEXT_INPUT_PROTOCOL` types to be added to `uefi/efi.h`).
- Scan codes: Up = `0x01`, Down = `0x02`. Enter = `UnicodeChar == 0x0D`.
  Esc = `ScanCode 0x17`.
- `ReadKeyStroke` returns `EFI_NOT_READY` when idle; poll on a `gBS->Stall`
  interval and decrement the countdown. The whole menu + poll loop must finish
  **before** `GetMemoryMap`/`ExitBootServices` (map-key validity contract).
- BIOS scan codes (`text`/graphics menu): Up `0x48`, Down `0x50`, Enter `0x1C`,
  Esc `0x01`.

Selection sets the boot entry (and, for the no-framebuffer/safe entries, the
kernel cmdline / framebuffer flags in the boot info) before handoff.

# ForeB Visual Assets - Background & Icons

This document specifies the image formats ForeB's UEFI UI loads from the ESP
for the menu **background** and per-entry **icons**, plus recommended sizes.
All assets live under `\forebo\` on the EFI System Partition and are named by
`forebo.cfg` (see `background=` / `icon=` keys).

> BIOS path note: the graphical BIOS menu (stage2/stage3, 8 KiB cap) does
> **not** decode BMP/TGA - backgrounds and icons are a **UEFI-only** feature.
> The BIOS path keeps its procedural gradient + tree logo.

---

## Supported formats

The loader ships a tiny, dependency-free decoder for two uncompressed,
easy-to-parse formats. No PNG/JPEG (would need zlib/huffman - too heavy for a
freestanding loader).

### 1. BMP - Windows bitmap (backgrounds, opaque icons)
- **24-bit** (`BI_RGB`, no compression): B,G,R per pixel.
- **32-bit** (`BI_RGB`/`BI_BITFIELDS`): B,G,R,A per pixel. The alpha byte is
  used for icon transparency when present.
- Rows are bottom-up (positive `biHeight`) - the decoder flips them. A
  negative `biHeight` (top-down) is also accepted.
- Rows are padded to a 4-byte boundary (handled by the decoder).
- **Not supported:** RLE-compressed BMP, 8/16-bit palettized BMP.

### 2. TGA - Truevision Targa (icons with alpha, backgrounds)
- **Type 2**, uncompressed true-color: 24-bit (BGR) or 32-bit (BGRA).
- **Type 10** (RLE true-color) is optional/bonus; type 2 is the baseline.
- Honors the top-left vs bottom-left origin bit in the image descriptor.
- 32-bit TGA is the **recommended icon format** because its 8-bit alpha
  channel gives clean edges over any background.

Both decoders output a linear `BGRX`/`BGRA` pixel buffer that `ui.c`'s blit
primitives consume directly (matching the common x86 GOP pixel order; R/B are
swapped at blit time on RGBX framebuffers, same as `foreb_swap_rb`).

---

## Recommended sizes

| Asset            | Recommended         | Notes                                        |
|------------------|---------------------|----------------------------------------------|
| Background       | 1920x1080 (16:9)    | Scaled (nearest/integer) to fill the GOP fb. |
| Background (min) | 800x600             | Matches the BIOS reference resolution.       |
| Menu entry icon  | 32x32 or 48x48      | Square; drawn in the left gutter of the row. |
| Icon (hi-DPI)    | 64x64               | Downscaled for small framebuffers.           |

Keep backgrounds dark/low-contrast in the menu region so the mint/leaf text
(`forebo_theme.h`) stays readable. Use 32-bit TGA for icons so anti-aliased
edges blend over the background.

---

## Default assets & generator

Default assets are **generated**, not committed as binaries, by:

    tools/gen_assets.py        (written during implementation)

It produces, into `\forebo\` layout under the build tree:
- `bg.bmp`      - a 24-bit forest-gradient background matching `forebo_theme.h`
                  (`FOREB_BG_TOP` -> `FOREB_BG_BOTTOM`) with the tree logo.
- `icons/os.tga`, `icons/text.tga`, `icons/safe.tga`, `icons/reboot.tga`
                  - 32-bit TGA icons with alpha for each sample menuentry.

The generator uses only Python's stdlib (or PIL if available) so the build
has no binary-asset dependency and the theme stays the single source of truth
for colors. Run it from the Makefile's asset stage; the resulting files are
copied onto the ESP image (`esp.img`) alongside `kernel.elf` and `forebo.cfg`.

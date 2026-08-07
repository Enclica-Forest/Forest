# Theme Customization

> *A bootloader is the first thing your eyes see. Make it beautiful.*

ForeB's theme engine scales from 800x600 BIOS VGA to arbitrary-resolution UEFI GOP. Every color, shape, and effect is configurable through `forebo.cfg` -- no recompilation required.

---

## Table of Contents

1. [Theme System Overview](#1-theme-system-overview)
2. [Color Palette](#2-color-palette)
3. [Named Theme Presets](#3-named-theme-presets)
4. [Style Presets](#4-style-presets)
5. [Selection & Border & Corner Styles](#5-selection--border--corner-styles)
6. [Background Images & Icons](#6-background-images--icons)
7. [Font Rendering](#7-font-rendering)
8. [Widget Styling](#8-widget-styling)
9. [Visual Effects](#9-visual-effects)
10. [Audio Feedback](#10-audio-feedback)
11. [Configuration Reference](#11-configuration-reference)
12. [Theme Presets Table](#12-theme-presets-table)
13. [Creating Custom Themes](#13-creating-custom-themes)
14. [Asset Generation Tools](#14-asset-generation-tools)

---

## 1. Theme System Overview

Three layers of customization, resolved in order:

```
Level 3: Granular Overrides (forebo.cfg)    color_*, btn_*, win_*, fx_*, menu_*
Level 2: Style Preset (menu_style=)         classic, modern, neon, glass, ...
Level 1: Color Theme (theme=)               forest, midnight, nord, dracula, ...
```

The renderer (`uefi/ui.c`) writes 32bpp pixels directly to a GOP linear framebuffer, supporting BGRX and RGBX byte orders. Colors are `0x00RRGGBB` matching the BIOS DAC palette for pixel-identical rendering across BIOS and UEFI.

| Source File | Role |
|-------------|------|
| `include/forebo_theme.h` | 14 palette colors, layout constants |
| `include/forebo_cfg.h` | Style/widget/theme structs, enums |
| `uefi/ui.c` | Runtime palette, 30 styles, 12 selection renderers, effects |

---

## 2. Color Palette

14 named slots defined in `forebo_theme.h`:

### Core Palette (Forest Default)

| Slot | Hex | RGB | Role |
|------|-----|-----|------|
| `FOREB_BG` | `#182D18` | (24, 45, 24) | Dark forest background |
| `FOREB_PANEL` | `#1C351C` | (28, 53, 28) | Menu panel fill |
| `FOREB_BORDER` | `#285128` | (40, 81, 40) | Separators / outlines |
| `FOREB_SELECT` | `#146514` | (20, 101, 20) | Selected-entry highlight |
| `FOREB_TITLE` | `#51CA3D` | (81, 202, 61) | Title / menu label |
| `FOREB_TEXT` | `#B6DFB6` | (182, 223, 182) | Normal text (mint) |
| `FOREB_DIM` | `#658265` | (101, 130, 101) | Subtitles / hints |
| `FOREB_TIMER` | `#DFA214` | (223, 162, 20) | Countdown timer (amber) |
| `FOREB_WHITE` | `#FFFFFF` | (255, 255, 255) | Selected label + arrow |
| `FOREB_SHADOW` | `#040804` | (4, 8, 4) | Drop shadow / near-black |

### Decorative & Progress

| Slot | Hex | RGB | Role |
|------|-----|-----|------|
| `FOREB_TREE1` | `#3D1C08` | (61, 28, 8) | Tree trunk (brown) |
| `FOREB_TREE2` | `#1C791C` | (28, 121, 28) | Foliage mid green |
| `FOREB_TREE3` | `#3DB63D` | (61, 182, 61) | Foliage highlight |
| `PROGRESS_TRACK` | `#285128` | aliases `BORDER` | Empty load bar |
| `PROGRESS_FILL` | `#51CA3D` | aliases `TITLE` | Filled load bar |
| `BG_TOP` | `#102010` | (16, 32, 16) | Gradient top |
| `BG_BOTTOM` | `#1E3A1E` | (30, 58, 30) | Gradient bottom |

### Runtime Overrides

```ini
color_bg=0x0E1A12          # background
color_fg=0xDDE7DE          # text
color_accent=0x3FB56B      # highlights / focus
color_sel_bg=0x1F5E3A      # selected row bg
color_sel_fg=0xFFFFFF      # selected row text
color_titlebar=0x1F5E3A    # window title bar
color_window=0x16241B      # window client area
color_cursor=0xFFFFFF      # cursor sprite fill
```

---

## 3. Named Theme Presets

Set `theme=<name>` to reskin the entire UI in one line:

| Theme | Vibe |
|-------|------|
| `forest` | Deep greens on charcoal -- moonlit woodland (default) |
| `midnight` | Deep navy blues with warm amber highlights |
| `nord` | Arctic cool tones from the Nord palette |
| `dracula` | Purple and pink on dark grey |
| `gruvbox` | Warm retro earth tones with punchy yellow/orange |
| `solarized` | Precision-engineered dark palette |
| `amber` | Retro CRT phosphor amber on black |
| `matrix` | Green phosphor on void -- digital rain aesthetic |
| `rose` | Rose Pine-inspired soft pinks on dark plum |
| `ocean` | Deep teal blues with cyan highlights |
| `mono` | Neutral grayscale -- distraction-free |

---

## 4. Style Presets

30 layout presets via `menu_style=<name>`. Each detail can be individually overridden.

| Style | Key Traits |
|-------|-----------|
| `classic` | Centered panel, double-bar selection, thick border, gradient, shadow |
| `minimal` | No border/gradient/shadow, arrow selection, no icons |
| `terminal` | Left-aligned, bracket selection, thin border, dividers |
| `flat` | No gradient/shadow, thin border, bar selection |
| `modern` | Pill selection, round corners, no border, gradient+shadow |
| `card` | Round corners, thick border, shadow, box selection |
| `neon` | Glow selection+border, accent strip, gradient |
| `outline` | Thin border, outline-only selection, flat |
| `underline` | Accent underline on selection, dividers |
| `invert` | Selection inverts fg/bg, thin border |
| `brackets` | Centered text with `[ ]` brackets |
| `sidebar-left` | Panel docked left, icons left, no title |
| `sidebar-right` | Panel docked right, no title |
| `banner-top` | Panel across top |
| `dock-bottom` | Panel at bottom |
| `fullscreen` | Panel fills screen |
| `centered` | Centered panel+text, bar selection, no icons |
| `compact` | Reduced entry height + padding |
| `spacious` | Generous entry height + padding |
| `retro` | Double border, bracket selection, cut corners |
| `glass` | Gradient selection, accent strip |
| `hacker` | Left-aligned, no selection fill, dividers |
| `ribbon` | Accent strip, bar selection, gradient |
| `framed` | Double border, square corners, bar selection |
| `dashed` | Dashed border, outline selection |
| `spotlight` | Glow selection, no border, gradient |
| `pill` | Pill selection, round corners, thin border |
| `boxed` | Box selection, thick border |
| `ghost` | No border/gradient/shadow, outline selection |
| `elegant` | Underline selection, accent strip, round corners |

---

## 5. Selection, Border & Corner Styles

### Selection Styles (12)

| Style | Value | Look |
|-------|-------|------|
| Bar | `bar` | Gradient highlight bar |
| Double Bar | `doublebar` | Bar + left accent stripe |
| Box | `box` | 2px outlined box |
| Outline | `outline` | Thin accent outline |
| Underline | `underline` | 2px accent rule below |
| Arrow | `arrow` | `>` caret only, no fill |
| Bracket | `bracket` | `[ label ]` in accent |
| Invert | `invert` | Swap fg/bg |
| Pill | `pill` | Rounded-cap gradient pill |
| Gradient | `gradient` | Horizontal accent-to-panel fade |
| Glow | `glow` | Bar + soft edge lines |
| None | `none` | Text color change only |

### Border Styles (7)

| Style | Value | Description |
|-------|-------|-------------|
| None | `none` | No border |
| Thin | `thin` | 1px |
| Thick | `thick` | 2px (default) |
| Double | `double` | Two concentric 1px |
| Shadow | `shadow` | 1px tinted toward shadow |
| Glow | `glow` | Accent border + outer glow |
| Dashed | `dashed` | 6px dashes / 4px gaps |

### Corner Styles (3)

| Style | Value | Description |
|-------|-------|-------------|
| Square | `square` | Sharp 90deg (default) |
| Round | `round` | Faux-rounded via 4px notch |
| Cut | `cut` | 6px diagonal chamfer |

---

## 6. Background Images & Icons

### Backgrounds

BMP (24/32-bit) and TGA (uncompressed), scaled to fill the framebuffer:

```ini
background=/forebo/bg.bmp           # global default
img_background=/forebo/skin/bg.bmp  # explicit override
# Per-entry:
menuentry "Forest OS" { background=/forebo/custom-bg.tga }
```

Custom panel/window/titlebar/button faces:

```ini
img_panel=/forebo/skin/panel.tga
img_window=/forebo/skin/window.tga
img_titlebar=/forebo/skin/titlebar.tga
img_button=/forebo/skin/button.tga
```

Keep images <= 4096px (cursor <= 64px).

### Icons

18 shipped 32x32 TGA icons with alpha in `/forebo/icons/`:

`os`, `text`, `safe`, `gear`, `shield`, `reboot`, `ubuntu`, `debian`, `arch`, `fedora`, `mint`, `tux`, `windows`, `grub`, `usb`, `disk`, `terminal`, `settings`

```ini
icon=os                           # short name
icon=/forebo/icons/my-icon.tga    # full path
menu_icon_side=right              # left|right
menu_show_icons=0                 # disable
```

---

## 7. Font Rendering

Crisp **8x16 bitmap font** (`include/font8x16.h`). Auto-scales 2x on 1080p+ panels for legibility. No antialiasing -- pure bitmap precision.

```
FOREB_GLYPH_W = 8     FOREB_GLYPH_H = 16
```

---

## 8. Widget Styling

### Button Styles (6)

| Style | Value | Description |
|-------|-------|-------------|
| Flat | `flat` | Solid fill |
| Raised | `raised` | Gradient + highlight + shadow (default) |
| Pill | `pill` | Rounded caps + gradient |
| Outline | `outline` | Transparent + accent border |
| Ghost | `ghost` | Nearly invisible, subtle hover tint |
| Glass | `glass` | Frosted backdrop blur + translucent |

```ini
btn_style=raised  btn_corner=round  btn_border=1
btn_pad_x=12  btn_pad_y=5
btn_gradient=1  btn_shadow=1  btn_glow=0
btn_fill=0x1C351C  btn_fill_hover=0x246B24  btn_fill_active=0x3FB56B
btn_text=0xFFFFFF  btn_border_color=0x285128  btn_focus_color=0x51CA3D
```

### Window Chrome

```ini
window_skin=beveled              # flat|beveled|glass
win_title_h=-1  win_title_fill=0x1F5E3A  win_title_fg=0xFFFFFF
win_border_color=0x3FB56B  win_border_w=1  win_corner=square
win_shadow=1  win_close_color=0xB03030  win_button_style=raised
```

---

## 9. Visual Effects

```ini
fx_glass=1          # frosted-glass backdrops (window_skin=glass)
fx_blur=8           # blur radius 0..32 (>16 capped)
fx_opacity=72       # backdrop darken 0..255
fx_vignette=128     # screen-edge darken 0..255
fx_scanlines=64     # CRT scanline dim 0..255
animations=1        # particles / fades / spinner
```

The blur uses O(w*h) separable box blur via running sums. Vignette darkens radially from center. Scanlines dim every other row for CRT aesthetics.

---

## 10. Audio Feedback

PC speaker tones -- no pre-OS audio stack needed:

```ini
pcspeaker=1                # master on/off
audio_volume=80            # 0..100
audio_nav_freq=880         # navigation  audio_nav_ms=18
audio_select_freq=1320     # enter       audio_select_ms=40
audio_open_freq=660        # window open audio_open_ms=30
audio_error_freq=220       # rejected    audio_error_ms=90
audio_back_freq=494        # esc/back    audio_back_ms=22
```

---

## 11. Configuration Reference

### Complete Key Table

| Category | Key | Default | Values |
|----------|-----|---------|--------|
| Theme | `theme` | `forest` | `forest midnight nord dracula gruvbox solarized amber matrix rose ocean mono` |
| Colors | `color_bg/fg/accent/sel_bg/sel_fg/titlebar/window/cursor` | built-in | `0xRRGGBB` |
| Layout | `menu_style` | `classic` | 30 presets |
| | `menu_pos` | `center` | `center left right top bottom full custom` |
| | `menu_x/y/w/h` | auto | permille (0..1000) |
| | `menu_entry_h` | auto | permille of screen H |
| | `menu_pad` | `14` | px |
| | `menu_align` | `left` | `left center right` |
| Selection | `menu_selection` | `doublebar` | 12 styles |
| Chrome | `menu_border` | `thick` | 7 styles |
| | `menu_corner` | `square` | `square round cut` |
| | `menu_accent_strip` | `1` | `0/1` |
| | `menu_dividers` | `0` | `0/1` |
| | `menu_gradient` | `1` | `0/1` |
| | `menu_shadow` | `1` | `0/1` |
| | `menu_title_bar` | `1` | `0/1` |
| | `menu_show_title` | `1` | `0/1` |
| | `menu_show_footer` | `1` | `0/1` |
| | `menu_show_timer` | `1` | `0/1` |
| | `menu_show_icons` | `1` | `0/1` |
| | `menu_icon_side` | `right` | `left right` |
| | `menu_scrollbar` | `1` | `0/1` |
| | `menu_caret` | `1` | `0/1` |
| Buttons | `btn_style` | `raised` | `flat raised pill outline ghost glass` |
| | `btn_corner` | `round` | `square round cut` |
| Effects | `fx_glass` | `0` | `0/1` |
| | `fx_blur` | `8` | `0..32` |
| | `fx_opacity` | `72` | `0..255` |
| | `fx_vignette` | `0` | `0..255` |
| | `fx_scanlines` | `0` | `0..255` |
| Audio | `pcspeaker` | `0` | `0/1` |
| | `audio_volume` | `80` | `0..100` |
| Input | `mouse_enabled` | `1` | `0/1` |
| | `cursor_enabled` | `1` | `0/1` |
| | `animations` | `0` | `0/1` |
| | `double_buffer` | `1` | `0/1` |
| | `window_skin` | `beveled` | `flat beveled glass` |
| Images | `background` | none | ESP path |
| | `img_background/panel/window/titlebar/button/cursor` | none | ESP path |
| Window | `win_title_h/title_fill/title_fg/border_color/border_w` | auto | varies |
| | `win_corner/shadow/close_color/button_style` | auto | varies |

---

## 12. Theme Presets Table

Full hex values for all 11 color themes (all slots except tree decorative colors):

| Theme | BG | Panel | Border | Select | Title | Text | Dim | Timer | Accent |
|-------|-----|-------|--------|--------|-------|------|-----|-------|--------|
| **forest** | `182D18` | `1C351C` | `285128` | `146514` | `51CA3D` | `B6DFB6` | `658265` | `DFA214` | `51CA3D` |
| **midnight** | `0B1020` | `131B2E` | `2B3B5C` | `1E3A66` | `6AA9FF` | `C7D6EE` | `6A7C99` | `FFC24B` | `6AA9FF` |
| **nord** | `2E3440` | `343B49` | `4C566A` | `434C5E` | `88C0D0` | `ECEFF4` | `818C9C` | `EBCB8B` | `88C0D0` |
| **dracula** | `282A36` | `31333F` | `44475A` | `454863` | `BD93F9` | `F8F8F2` | `6272A4` | `FFB86C` | `FF79C6` |
| **gruvbox** | `282828` | `323028` | `504945` | `453C30` | `FABD2F` | `EBDBB2` | `A89984` | `FE8019` | `FABD2F` |
| **solarized** | `002B36` | `073642` | `586E75` | `094A56` | `268BD2` | `93A1A1` | `657B83` | `B58900` | `268BD2` |
| **amber** | `120A00` | `1A1200` | `4A3300` | `3B2600` | `FFB000` | `FFCC55` | `A87A20` | `FF7818` | `FFB000` |
| **matrix** | `001200` | `001A00` | `105010` | `073807` | `00FF41` | `90FFA0` | `309040` | `00FF41` | `00FF41` |
| **rose** | `191724` | `232135` | `403D52` | `2A2740` | `EBBCBA` | `E0DEF4` | `908CAA` | `F6C177` | `EB6F92` |
| **ocean** | `0A1E24` | `102A32` | `285561` | `13414C` | `33C5D8` | `CDECEF` | `5F8A92` | `FFC24B` | `33C5D8` |
| **mono** | `141414` | `1E1E1E` | `404040` | `343434` | `E0E0E0` | `C8C8C8` | `808080` | `E0E0E0` | `E0E0E0` |

---

## 13. Creating Custom Themes

### Quick Start

1. Pick a base theme
2. Override individual colors
3. Choose a layout preset
4. Fine-tune details
5. Add effects and widgets

### Example: Cyberpunk Neon

```ini
theme=midnight
menu_style=neon
menu_selection=glow
menu_border=glow
menu_corner=round
color_accent=0xFF00FF
window_skin=glass
fx_glass=1  fx_blur=16  fx_vignette=96
btn_style=glass  btn_corner=round
pcspeaker=1  audio_select_freq=1760  audio_select_ms=25
```

### Example: Retro CRT Terminal

```ini
theme=amber
menu_style=terminal
menu_selection=bracket
menu_border=thin
menu_corner=square
menu_gradient=0  menu_shadow=0  menu_accent_strip=0
menu_dividers=1  menu_show_icons=0
fx_scanlines=80  fx_vignette=128
btn_style=flat  btn_corner=square
pcspeaker=1  audio_nav_freq=440  audio_nav_ms=12
```

---

## 14. Asset Generation Tools

### forb-config (Python CLI)

Edit, validate, query, and convert `forebo.cfg` files:

```bash
forb-config info forebo.cfg              # summary
forb-config validate forebo.cfg --esp /boot
forb-config themes / forb-config icons   # list assets
forb-config set forebo.cfg theme dracula
forb-config convert grub.cfg --to foreb  # migrate from GRUB
```

### forb-customizer (C++ Qt GUI)

Visual theme editor with live preview, scrollable preset gallery, interactive color picker, diff dialog, and entry inspector. Build:

```bash
cd tools/forb-customizer && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

---

## Quick Reference

```
theme=     forest midnight nord dracula gruvbox solarized amber matrix rose ocean mono
style=     classic minimal terminal flat modern card neon outline underline invert brackets
            sidebar-left sidebar-right banner-top dock-bottom fullscreen centered compact
            spacious retro glass hacker ribbon framed dashed spotlight pill boxed ghost elegant
selection= bar doublebar box outline underline arrow bracket invert pill gradient glow none
border=    none thin thick double shadow glow dashed
corner=    square round cut
button=    flat raised pill outline ghost glass
```

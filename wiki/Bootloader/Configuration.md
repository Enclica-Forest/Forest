# ForeB Bootloader Configuration Reference

ForeB uses a GRUB-like configuration file (`forebo.cfg`) stored on the EFI
System Partition at `\forebo\forebo.cfg`. The parser lives in
`uefi/core/config.c` and populates a POD struct defined in
`include/forebo_cfg.h`.

## File Format Basics

- Lines starting with `#` are comments. Blank lines are ignored.
- Paths are ESP-absolute; both `/` and `\` separators work.
- Quotes around titles/values are stripped by the parser.
- Unknown keys are silently ignored (forward/backward compatibility).
- The parser is tolerant: malformed lines are skipped, never faulted.

```
# This is a comment
timeout=60
background=/forebo/bg.bmp
```

**Capacity limits:** 64 max entries, 8 modules per entry, 64-char titles,
256-char paths, 256-char command lines, 8-level submenu nesting.

---

## Global Settings

| Key             | Default | Description |
|-----------------|---------|-------------|
| `timeout`       | 10      | Seconds before auto-booting the default entry. `0` = boot immediately. |
| `default`       | 0       | Pre-selected entry. Numeric = 0-based top-level index. String = title path (e.g. `CachyOS/linux-cachyos`). Last `default=` wins. |
| `remember_last` | 0       | When `1`, persists last booted entry in UEFI variable `ForeBLastEntry` and pre-selects it next boot (overrides `default=`). Requires writable NVRAM. |
| `background`    | (none)  | Default menu background image for all entries (BMP/TGA). Per-entry `background=` overrides. |

The `default` path resolves case-sensitively level by level; if it lands on
a submenu it descends to its first child automatically. On any mismatch the
first top-level non-submenu row is used.

---

## Menu Entry Syntax

Each entry is a `menuentry "Title" { ... }` block. The `type=` key selects
the boot method (defaults to `forest`).

```
menuentry "Entry Title" {
    type=<kind>
    kernel=/forebo/kernel.elf
    icon=os
    cmdline=""
}
```

**Per-entry keys:** `type`, `icon`, `background`, `cmdline`, `kernel`,
`module` (repeatable, up to 8), `module2` (alias of `module`), `vmlinuz`,
`initrd`, `chain`.

---

## Boot Methods

| `type=` | Description |
|---------|-------------|
| `forest` (default) | Multiboot1 Forest kernel handoff (x86_64 only). |
| `linux` | EFI-stub vmlinuz + initrd via LoadImage/StartImage + LoadFile2. Works on x64/ARM/RISC-V. |
| `chainload` / `chain` | LoadImage/StartImage another EFI bootloader. Empty `chain=` = auto-scan all volumes. |
| `windows` / `win` | Chainload alias. Auto-sets `chain=/EFI/Microsoft/Boot/bootmgfw.efi`. |
| `shell` | Open the interactive ForeB shell window. |
| `recovery` | Open the Recovery / disk-tools window. |
| `tools` | Open the GUI Tools launcher (Disk Info, GPT Viewer, File Browser, Hex Viewer, Memory Map, EFI Variables, ...). |
| `settings` / `theme` | Open the live Theme/Settings editor. |
| `setup` / `firmware` | Reboot into UEFI firmware setup via OsIndications. |
| `uefi_settings` | Open the UEFI firmware settings panel (view/edit). |
| `reboot` | Firmware reset. Legacy alias: `kernel=reboot`. |

Setting `vmlinuz=` on a `forest` entry auto-promotes to `linux`. Setting
`chain=` on a `forest` entry auto-promotes to `chainload`.

### Forest OS

```
menuentry "Forest OS" {
    type=forest
    kernel=/forebo/kernel.elf
    module=/forebo/initrd.tar
    icon=os
    cmdline=""
}
```

### Linux (EFI-stub)

```
menuentry "Linux" {
    type=linux
    vmlinuz=/forebo/vmlinuz
    initrd=/forebo/initrd.img
    cmdline="root=/dev/sda2 ro quiet loglevel=3"
    icon=tux
}
```

### Chainload

```
menuentry "Chainload GRUB (USB)" {
    type=chainload
    chain=/EFI/BOOT/BOOTX64.EFI
    icon=grub
}

menuentry "Boot removable device" {
    type=chainload
    icon=usb
}
```

**Icon short names** (resolved to `/forebo/icons/<name>.tga`): `os`, `text`,
`safe`, `gear`, `shield`, `reboot`, `ubuntu`, `debian`, `arch`, `fedora`,
`mint`, `tux`, `windows`, `grub`, `usb`, `disk`, `terminal`.

---

## Theme Configuration

### Named Presets

```
theme=forest
```

Available: `forest` (default), `midnight`, `nord`, `dracula`, `gruvbox`,
`solarized`, `amber`, `matrix`, `rose`, `ocean`, `mono`. Any `color_*` key
overrides a single color on top of the preset.

### Colors (0xRRGGBB, also accepts #RRGGBB)

| Key | Default | Description |
|-----|---------|-------------|
| `color_bg` | `0x0E1A12` | Desktop / menu background |
| `color_fg` | `0xDDE7DE` | Default text |
| `color_accent` | `0x3FB56B` | Highlights, progress bar, focus ring |
| `color_sel_bg` | `0x1F5E3A` | Selected menu row background |
| `color_sel_fg` | `0xFFFFFF` | Selected menu row text |
| `color_titlebar` | `0x1F5E3A` | Window title bar |
| `color_window` | `0x16241B` | Window client area |
| `color_cursor` | `0xFFFFFF` | Cursor sprite fill |

### Cursor and Input

| Key | Default | Description |
|-----|---------|-------------|
| `mouse_enabled` | 1 | Poll pointer protocols |
| `cursor_enabled` | 1 | Draw mouse cursor sprite |
| `cursor` | (none) | Cursor sprite path (TGA w/ alpha) |
| `animations` | 1 | Fades / particles / spinner |
| `double_buffer` | 1 | Off-screen back buffer + ui_present |

### Window Skin

`window_skin=flat|beveled|glass` (default: `beveled`).

### Custom Images (ESP-absolute TGA/BMP)

| Key | Description |
|-----|-------------|
| `img_background` | Menu background (overrides `background=`) |
| `img_panel` | Menu panel face (blit + tint) |
| `img_window` | Window client face |
| `img_titlebar` | Window title-bar face |
| `img_button` | Button face |
| `img_cursor` | Cursor sprite (alias of `cursor=`) |

Keep panel/window/titlebar/button <= 4096px; cursor <= 64px.

### Visual Effects

Off by default (each forces a full-frame flip).

| Key | Default | Range | Description |
|-----|---------|-------|-------------|
| `fx_glass` | 0 | 0/1 | Frosted-glass blur behind windows |
| `fx_blur` | 8 | 0..32 | Backdrop blur radius (px) |
| `fx_opacity` | 72 | 0..255 | Backdrop darken level |
| `fx_vignette` | 0 | 0..255 | Screen-edge darken |
| `fx_scanlines` | 0 | 0..255 | CRT scanline dim |

### Audio (PC Speaker)

All off by default. Keys: `pcspeaker` (master on/off), `audio_volume`
(0..100), and per-event pairs `audio_{nav,select,open,error,back}_freq` /
`audio_{nav,select,open,error,back}_ms` (Hz and ms).

---

## Menu Style Configuration

`menu_style` picks one of 30 built-in layout presets:

```
classic minimal terminal flat modern card neon outline underline invert
brackets sidebar-left sidebar-right banner-top dock-bottom fullscreen
centered compact spacious retro glass hacker ribbon framed dashed
spotlight pill boxed ghost elegant
```

All fields below are optional; omit any to inherit from the preset. `-1`
means "inherit".

| Key | Values | Description |
|-----|--------|-------------|
| `menu_pos` | `center`, `left`, `right`, `top`, `bottom`, `full`, `custom` | Panel position |
| `menu_x/y/w/h` | 0..1000 | Permille of screen (custom pos/size) |
| `menu_entry_h` | int | Row height (permille of screen height) |
| `menu_pad` | int | Inner padding (px) |
| `menu_align` | `left`, `center`, `right` | Label alignment |
| `menu_selection` | `bar`, `doublebar`, `box`, `outline`, `underline`, `arrow`, `bracket`, `invert`, `pill`, `gradient`, `glow`, `none` | Selection indicator |
| `menu_border` | `none`, `thin`, `thick`, `double`, `shadow`, `glow`, `dashed` | Panel frame |
| `menu_corner` | `square`, `round`, `cut` | Panel corners |
| `menu_accent_strip` | 0/1 | Colored strip along panel top |
| `menu_dividers` | 0/1 | Thin rule between rows |
| `menu_gradient` | 0/1 | Vertical gradient panel body |
| `menu_shadow` | 0/1 | Drop shadow under panel |
| `menu_title_bar` | 0/1 | "[ Boot Menu ]" header row |
| `menu_show_title` | 0/1 | Big centered scene title |
| `menu_show_footer` | 0/1 | Key-hint footer line |
| `menu_show_timer` | 0/1 | Auto-boot countdown |
| `menu_show_icons` | 0/1 | Per-entry icons |
| `menu_icon_side` | `left`, `right` | Icon gutter position |
| `menu_scrollbar` | 0/1 | Scrollbar when list overflows |
| `menu_caret` | 0/1 | ">" caret on selected row |

---

## Submenu Syntax

Submenus group entries under a collapsible level. Enter/Right descends,
Esc/Left goes back. Nest up to 8 levels.

```
submenu "CachyOS" {
    icon=arch

    menuentry "linux-cachyos" {
        type=linux
        vmlinuz=/vmlinuz-linux-cachyos
        initrd=/initramfs-linux-cachyos.img
        cmdline="root=UUID=xxxx-xxxx rw"
        icon=arch
    }

    submenu "Snapshot 906" {
        menuentry "linux-cachyos (906)" {
            type=linux
            vmlinuz=/vmlinuz-linux-cachyos
            initrd=/initramfs-linux-cachyos.img
            cmdline="root=UUID=xxxx-xxxx rw rootflags=subvol=/@/.snapshots/906/snapshot"
            icon=safe
        }
    }
}
```

Only `icon=` is accepted inside a submenu block. A `}` closes the innermost
open block. With the above, `default=CachyOS/linux-cachyos` auto-boots the
child entry.

---

## Widget / Button Appearance

All prefixed `btn_` or `ui_`. Omit any to inherit the built-in look.

| Key | Description |
|-----|-------------|
| `btn_style` | `flat`, `raised`, `pill`, `outline`, `ghost`, `glass` |
| `btn_corner` | `square`, `round`, `cut` |
| `btn_border` | Outline width (px) |
| `btn_pad_x`, `btn_pad_y` | Label padding (px) |
| `btn_gradient`, `btn_shadow`, `btn_glow` | 0/1 toggles |
| `btn_fill` | Normal face color |
| `btn_fill_hover`, `btn_fill_active`, `btn_fill_disabled` | State colors |
| `btn_text` | Normal text color |
| `btn_text_hover`, `btn_text_active` | State text colors |
| `btn_border_color`, `btn_focus_color` | Border and focus ring colors |
| `ui_window_corner` | `square`, `round`, `cut` |
| `ui_window_border` | Window border width |
| `ui_panel_alpha` | 0..255 panel opacity |
| `ui_separator` | Separator color |
| `ui_scrollbar_w`, `ui_scrollbar_color` | Scrollbar appearance |
| `ui_focus_color`, `ui_focus_width` | Focus ring |
| `ui_font_scale` | 25..800 percent (100 = 1x) |

---

## Window Chrome Overrides

Override individual aspects of the window skin. Any key omitted inherits
the skin/theme.

| Key | Description |
|-----|-------------|
| `win_title_h` | Title-bar height (px), `-1` = auto |
| `win_title_fill` | Title bar background color |
| `win_title_fg` | Title text color |
| `win_border_color` | Window frame color |
| `win_border_w` | Frame thickness (px) |
| `win_corner` | `square`, `round`, `cut` |
| `win_shadow` | 0/1 drop shadow |
| `win_close_color` | Close-box fill color |
| `win_button_style` | `flat`, `raised`, `pill`, `outline`, `ghost`, `glass` |

---

## Validation and Error Handling

**Parser tolerance:**
- Malformed lines are skipped; parsing continues.
- Unknown keys are silently ignored.
- Missing closing quotes are tolerated (runs to EOF/newline).
- Empty/partial configs produce a bootable menu with defaults.
- If `forebo.cfg` is missing, a hardcoded 8-entry default config is used.

**Fallback rules:**

| Condition | Behavior |
|-----------|----------|
| `timeout` omitted | 10 seconds |
| `default` omitted or invalid | First top-level non-submenu entry |
| `type` omitted | `forest` |
| `kernel=reboot` (legacy) | Maps to `type=reboot` |
| `type=windows` / `win` | Auto-sets `chain=/EFI/Microsoft/Boot/bootmgfw.efi` |
| `vmlinuz=` on forest entry | Auto-promotes to `type=linux` |
| `chain=` on forest entry | Auto-promotes to `type=chainload` |
| Submenu nesting exceeds 8 | Block discarded, parsing continues |
| `remember_last` variable error | Falls back to config `default=` |

---

## Complete Example

```
# Global
timeout=30
default=0
remember_last=1
background=/forebo/bg.bmp
theme=forest
menu_style=classic
menu_selection=doublebar
menu_show_icons=1
pcspeaker=0

# Forest OS
menuentry "Forest OS" {
    type=forest
    kernel=/forebo/kernel.elf
    module=/forebo/initrd.tar
    icon=os
    cmdline=""
}

# Linux
menuentry "Linux" {
    type=linux
    vmlinuz=/forebo/vmlinuz
    initrd=/forebo/initrd.img
    cmdline="root=/dev/sda2 ro quiet"
    icon=tux
}

# Chainload
menuentry "Chainload GRUB" {
    type=chainload
    chain=/EFI/BOOT/BOOTX64.EFI
    icon=grub
}

# Submenu
submenu "CachyOS" {
    icon=arch
    menuentry "linux-cachyos" {
        type=linux
        vmlinuz=/vmlinuz-linux-cachyos
        initrd=/initramfs-linux-cachyos.img
        cmdline="root=UUID=xxxx-xxxx rw"
        icon=arch
    }
}

# Tools
menuentry "ForeB Shell"   { type=shell    icon=terminal }
menuentry "Recovery"      { type=recovery icon=gear }
menuentry "Tools"         { type=tools    icon=gear }
menuentry "Settings"      { type=settings icon=gear }
menuentry "Firmware"      { type=setup    icon=settings }
menuentry "Reboot"        { type=reboot   icon=reboot }
```

---

## ESP File Layout

```
/forebo/
  forebo.cfg              Configuration file
  kernel.elf              Forest OS kernel (Multiboot1)
  initrd.tar              Forest OS initrd
  bg.bmp                  Menu background image
  vmlinuz                 Linux kernel (optional)
  initrd.img              Linux initramfs (optional)
  icons/                  Per-entry icons (TGA)
```

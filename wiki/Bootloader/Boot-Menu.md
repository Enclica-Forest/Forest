# Boot Menu

The ForeB (Forest Bootloader) boot menu is a fully graphical, GOP-framebuffer-based interface that renders directly to the screen without relying on any text-mode firmware services. It provides a polished, themeable entry point for selecting and launching operating systems, chainloading other bootloaders, and accessing built-in recovery and diagnostic tools.

## Design Philosophy

The boot menu is designed to feel like a mini desktop environment before the OS even loads. It features a windowed compositor, a mouse cursor, draggable windows, smooth animations, and a deep theming system -- all implemented in freestanding C with no libc. Every pixel is written directly to the Graphics Output Protocol (GOP) linear framebuffer using 32bpp BGRA/RGBA pixel operations.

The menu supports both x86_64 UEFI and has stubs for AArch64 and RISC-V targets. On x86_64, it can take advantage of write-combining framebuffer mapping and VGA vertical-blank synchronization for tear-free presentation. The entire rendering pipeline -- from primitives to text to image blitting to window compositing -- shares a common clip-rect stack so that only visible pixels are ever touched.

## Forest Theme

The default theme is called "forest" and uses a palette inspired by deep woodland greens and earthy tones. The colors are defined in `forebo_theme.h` and are shared between the BIOS and UEFI paths for pixel-identical rendering:

| Color Name      | Hex         | Role                                    |
|-----------------|-------------|----------------------------------------|
| `FOREB_BG`      | `0x182D18`  | Dark forest background                  |
| `FOREB_PANEL`   | `0x1C351C`  | Menu panel fill                         |
| `FOREB_BORDER`  | `0x285128`  | Separators and outlines                 |
| `FOREB_SELECT`  | `0x146514`  | Selected-entry highlight                |
| `FOREB_TITLE`   | `0x51CA3D`  | Title text and accent (leaf green)      |
| `FOREB_TEXT`    | `0xB6DFB6`  | Normal entry text (soft mint)           |
| `FOREB_DIM`     | `0x658265`  | Subtitles and hints (grey-green)        |
| `FOREB_TIMER`   | `0xDFA214`  | Countdown timer (amber)                 |
| `FOREB_WHITE`   | `0xFFFFFF`  | Selected label and caret arrow          |
| `FOREB_SHADOW`  | `0x040804`  | Drop shadows and near-black             |
| `FOREB_TREE1`   | `0x3D1C08`  | Tree trunk (brown)                      |
| `FOREB_TREE2`   | `0x1C791C`  | Foliage mid-green                       |
| `FOREB_TREE3`   | `0x3DB63D`  | Foliage highlight                       |

The background uses a vertical gradient from `FOREB_BG_TOP` (`0x102010`) at the top to `FOREB_BG_BOTTOM` (`0x1E3A1E`) at the bottom, giving the menu a subtle sense of depth. A stylized tree logo is drawn procedurally using three overlapping isosceles triangles for the foliage tiers and a rectangular trunk. The gradient is rendered row-by-row with linear interpolation, writing each row's clipped span directly to the back buffer and issuing a single dirty mark for the entire band.

The layout uses fractional coordinates (permille of screen width/height) so it scales correctly to any GOP resolution -- from 800x600 BIOS-mode all the way to 4K. The panel is centered at 20% from the left and 36% from the top, occupying 60% width and 42% height of the screen. Absolute pixel constants from the BIOS 800x600 reference design are also available for BIOS-mode rendering.

## Double-Buffered Rendering

The menu uses double buffering to eliminate visible tearing and flicker. On initialization, `ui_init()` attempts to allocate an off-screen RAM back buffer (`g_back`) whose stride matches the screen width. All drawing primitives -- `put_pixel`, `fill_rect`, `draw_string`, etc. -- write to this RAM buffer rather than directly to VRAM. If the allocation fails (or BootServices are no longer available), the menu falls back to drawing directly into VRAM, which is correct but slower on real hardware.

When a frame is complete, `ui_present()` copies the back buffer to the GOP front buffer (VRAM). On x86_64, this copy uses the `rep movsq` instruction (Enhanced REP MOVSB on modern CPUs) for maximum memory bandwidth. Small spans (under 512 bytes) use a 32-bit word loop instead to avoid the microcode startup latency of `rep movsq`. A write-combining (`WC`) memory attribute is requested via the CPU Architectural Protocol so the sequential stores stream through WC buffers rather than stalling on the uncached bus. A write fence (`sfence` on x86, `dsb st` on AArch64) is issued after each flip to drain the WC buffers.

### Dirty-Rectangle Tracking

To avoid copying the entire framebuffer on every frame (which is expensive on real hardware where VRAM is uncached MMIO), the menu tracks per-scanline dirty spans. Every draw primitive updates `[min, max)` column extents for the rows it touches. On `ui_present()`, only the union of this frame's and the previous frame's dirty spans is copied to VRAM. This means the cursor, particles, and animations -- which touch a small fraction of the screen -- are nearly free compared to a full 8MB blit at 60fps.

The animation system also uses this for efficient background restoration. After a cursor or particle frame, `ui_restore_prev_spans()` copies back only the previously-damaged rows from a cached background snapshot, avoiding a full back-buffer memcpy every tick.

### Clip-Rectangle Stack

Every drawing primitive intersects its output with the current clip rectangle (default: the whole screen). The compositor pushes each window's visible region before painting it, so windows covered by opaque windows above them skip real pixel work instead of overdraw-blind repainting. The clip stack supports up to 8 nested levels via `ui_clip_push()` and `ui_clip_pop()`.

### VSync

On x86 hardware where the GPU keeps legacy VGA I/O alive, the menu probes the VGA input-status register (port `0x3DA`, bit 3) for a live vertical-retrace signal. If a toggling signal is detected, full-screen flips are gated on the start of vertical blanking to ensure tear-free presentation. Partial flips (cursor and particle updates) are never gated since their tearing is imperceptible and gating them would add input latency.

## Menu Entry Types

Each menu entry has a `type=` field that determines how it boots. The supported types are:

- **forest** -- Multiboot1 ELF handoff to a Forest OS kernel (x86_64 only). Loads the kernel ELF and optional modules (initrd, safe overlays). The default type when `type=` is omitted.
- **linux** -- EFI-stub Linux boot. Loads `vmlinuz` as a PE executable via `LoadImage/StartImage`, passes `cmdline` as `LoadOptions`, and delivers `initrd` through the Linux initrd media protocol (`LINUX_EFI_INITRD_MEDIA_GUID` + `LoadFile2`). Works on x64, ARM64, and RISC-V UEFI.
- **chainload** -- LoadImage/StartImage of another EFI bootloader. If `chain=` names a file, that EFI application is loaded directly. If `chain=` is empty, ForeB auto-scans all SimpleFileSystem/BlockIo volumes for a standard EFI loader (`\EFI\BOOT\BOOTX64.EFI`, `\EFI\*\grubx64.efi`).
- **shell** -- Opens the interactive ForeB shell window.
- **recovery** -- Opens the Recovery/disk-tools window with file undelete, disk cloning, and firmware setup utilities.
- **tools** -- Opens the windowed GUI Tools launcher (Disk Info, GPT Viewer, Partition Browser, Hex Viewer, Memory Map, EFI Variables, System Info, Key Tester, and more).
- **settings** -- Opens the live Theme/Settings editor with an interactive RGB color picker.
- **uefi_settings** -- Displays UEFI firmware settings (BootOrder, BootNext, Secure Boot state) without rebooting.
- **setup** -- Reboots into the firmware/UEFI setup screen by setting the `OsIndications` runtime variable.
- **reboot** -- Firmware cold reset via `RuntimeServices->ResetSystem`.

Each entry can specify an `icon=` (a short name or full ESP path to a TGA/BMP image) and a per-entry `background=` override.

## Submenu Support

ForeB supports Limine-style submenus for organizing entries hierarchically. Submenus are defined with `submenu "Title" { ... }` blocks that can nest up to 8 levels deep. The menu renders one level at a time; pressing Enter or Right descends into a submenu, while Escape or Left goes back up. The panel title shows a breadcrumb of the current path.

Each row stores a `parent` field pointing to the flat index of its enclosing submenu row (or `-1` for top-level rows). The default selection (`default=`) can reference entries by title path (e.g., `default=CachyOS/linux-cachyos`) where intermediate segments name submenu rows. A default that lands on a submenu row automatically descends to its first child. On any resolution failure, the fallback is the first top-level non-submenu row.

When `remember_last=1` is enabled, ForeB persists the last booted entry index in a UEFI NVRAM variable (`ForeBLastEntry`, vendor GUID `{46524542-4F4F-5442-8001-466F72654231}`, attributes NV|BS) and pre-selects it on the next boot. Any variable error falls back to the config default.

## Animation System

The animation engine (`anim.c`) provides several visual effects. It operates directly on the framebuffer, bypassing the `ui.c` primitives, so it calls `ui_mark_dirty()` explicitly to feed changes into the partial-present pass.

### Fade Transitions

- **Fade-in** -- Ramps all pixels from black to the captured scene over a configurable number of frames using a pre-computed 256-entry brightness lookup table. Used when the menu first appears.
- **Fade-out** -- Captures the current screen contents (including particles and cursor), then ramps them toward black. Used before boot handoff. The capture deliberately overwrites the static-background snapshot since fade-out is the last visual before a boot.

Both fades are per-channel integer operations with no floating point. The brightness LUT avoids a per-pixel divide by precomputing `(channel * step) / frames` for each frame. Each fade step forces a whole-screen flip via `ui_mark_all()`.

### Particle System

A configurable particle layer of up to 96 motes drifts across the background. Particles come in two palettes:
- **Leaves** (default) -- four shades of green from the forest theme.
- **Embers** -- four shades of amber/orange for a warm look.

Each particle is a 2-4px square with its own vertical speed (bigger = faster for parallax depth), horizontal drift, and alpha. The particle colors can be tinted to match any active theme via `anim_set_tint()`, which builds a 4-stop gradient palette between the accent and title colors. Particles alpha-blend over the background snapshot using a fast integer approximation of `/255` via the formula `(x + 0x80 + ((x + 0x80) >> 8)) >> 8`.

An exclusion rectangle mechanism prevents particles from being seeded into or drawn over the menu panel, keeping the text area clean. Particles that drift into the excluded zone are simply not rendered until they exit. The step function checks both the old and new positions against the exclusion rect to avoid dirtying panel pixels.

### Spinner

A loading spinner with 8 dots arranged on a unit circle (radius ~7px, scaled) is drawn next to the progress bar during kernel loads. Each dot's brightness fades based on its distance from the "head" dot (level `255 - rel * 30`), creating a rotating trail effect. The spinner uses its own snapshot buffer so old dots are erased before new ones are drawn, preventing accumulation as a static ring.

### Eased Progress Bar

The `anim_progress_to()` function smoothly animates the progress bar fill from its current value to the target in 4-percent steps, with the spinner advancing in sync. Each step is presented individually with a small delay (10ms per step via `Stall()`). The integer easing helper `anim_lerp()` provides quadratic ease-out interpolation for smooth menu highlight sliding.

## Background Images

The menu supports custom background images in BMP and TGA formats, loaded from the EFI System Partition.

### BMP Support

- 24-bit and 32-bit uncompressed BMP files.
- Both `BI_RGB` (no compression) and `BI_BITFIELDS` (explicit channel masks) are supported.
- Bottom-up (default) and top-down orientations.
- Row stride is padded to 4-byte boundaries per the BMP specification.
- 32-bit BMPs with an entirely zero alpha channel are auto-detected as "no alpha authored" and forced to opaque so the image is not invisible.

### TGA Support

- Type 2 (uncompressed true-color) and Type 10 (RLE-compressed true-color) TGA files.
- 24-bit and 32-bit color depth.
- Top-down and bottom-up row ordering (controlled by descriptor bit 5).
- RLE packets span rows freely; the decoder carries a running pixel position across packets.
- Color-mapped TGAs are rejected.

Both decoders emit a linear `0xAARRGGBB` pixel buffer. Images are loaded by `img_load_file()` which reads the file from an open root directory handle in 256KB chunks, then decodes and frees the raw data. The decoded image can be blitted to the framebuffer with `img_blit_scaled()` (nearest-neighbor scaling with fixed-point 16.16 stepping, two divisions total per blit) or `img_blit_alpha_scaled()` (with per-pixel alpha blending). Alpha blending uses the formula `(src * alpha + dst * (255 - alpha) + 127) * 0x8081 >> 23` for exact `/255` without a divide.

## Icon System

Each menu entry can have an associated icon displayed in the menu row. Icons are 32x32 TGA or BMP images with an alpha channel, loaded from `/forebo/icons/` on the ESP. ForeB ships with 18 pre-made icons:

`arch`, `debian`, `disk`, `fedora`, `gear`, `grub`, `mint`, `os`, `reboot`, `safe`, `settings`, `shield`, `terminal`, `text`, `tux`, `ubuntu`, `usb`, `windows`

Icons can be referenced by short name (e.g., `icon=os` resolves to `/forebo/icons/os.tga`) or by a full ESP path. The `menu_show_icons` and `menu_icon_side` style options control whether icons are shown and whether they appear on the left or right side of each row. The icon gutter width adapts to the entry height, and icons are composited with alpha blending over the panel background.

## Font Rendering

The menu uses an embedded 8x16 bitmap font (`font8x16.h`) for all text rendering. Each glyph is an 8-bit-wide, 16-row-tall bitmap where each bit represents a pixel (MSB-first). The font is rendered by `draw_char()` which:

1. Performs glyph-level clipping against the active clip rectangle (one compare instead of up to 128 clipped fill_rect calls).
2. Packs the foreground and background colors to native framebuffer order once.
3. Iterates over each row of the glyph bitmap.
4. Coalesces consecutive columns in the same on/off state into horizontal runs.
5. Writes each run as a single `ui_raw_fill()` call (foreground for set bits, background for unset bits when opaque mode is active).
6. Issues one dirty mark for the entire clipped cell.

On panels 1080p or taller, the font is automatically scaled 2x (`g_uiscale = 2`) so text remains legible at high DPI. Callers can request additional scaling, and the effective magnification is `caller_scale * g_uiscale`.

String rendering functions include `draw_string()`, `draw_string_center()`, and `draw_string_clip()` (which truncates with "..." ellipsis if the string exceeds the available width, reserving 2 cells for the dots).

## Mouse Cursor and Pointer Support

The input system (`input.c`) supports three types of pointer devices simultaneously:

- **Absolute Pointer** -- USB tablets and touchscreens via `EFI_ABSOLUTE_POINTER_PROTOCOL`. Provides pixel-accurate positioning. Multiple absolute devices are enumerated, and the first "live" device (one that reports non-origin coordinates) is selected as authoritative.
- **Simple Pointer** -- Relative mice via `EFI_SIMPLE_POINTER_PROTOCOL`. Deltas are accumulated from every queued report in a single frame (draining the whole queue, not just one report per frame) with a guard against absurd per-poll spikes.
- **PS/2 Mouse** -- A firmware-independent fallback using direct i8042 port I/O (`0x60` data, `0x64` status/command). The driver enables the auxiliary device, configures it at 100 samples/sec, and optionally detects IntelliMouse scroll-wheel support via the 200/100/80 magic knock. Only aux-tagged bytes (status bit `0x20`) are consumed, leaving keyboard bytes for the firmware.

The `input_rescan()` function calls `ConnectController` recursively on all handles to ensure firmware actually binds USB/PS2 pointer drivers (a common issue with OVMF's selective auto-connect). Button and wheel state is merged across all device types via logical OR so a USB mouse click registers alongside a PS/2 trackpad tap.

The cursor is a 12x19 arrow sprite defined as a compact ASCII art string. `'1'` marks body pixels, `'2'` marks outline pixels, and spaces are transparent. On hi-res panels, the sprite is scaled up by `ui_scale()`. A custom cursor can be loaded from a TGA file via the `cursor=` config key.

The cursor is drawn via `input_draw_cursor()` which composites it into the back buffer alongside everything else. It supports a configurable fill color (`color_cursor=`) and a dark outline (`CURSOR_OUTLINE = 0x101010`) for visibility on any background. The renderer coalesces horizontal runs of the same glyph type into single `fill_rect` calls for efficiency.

## Keyboard Navigation

The boot menu responds to the following keys:

- **Up/Down** -- Move the selection highlight between menu entries.
- **Enter** -- Boot the selected entry, or descend into a submenu.
- **Escape** -- Go back to the parent submenu, or reset the auto-boot timer.
- **Left** -- Go back to the parent submenu (same as Escape in submenu context).
- **Right** -- Descend into the selected submenu (same as Enter).
- **S** -- Open the Settings/Theme editor at any time.
- **U** -- Open the UEFI Firmware Settings panel at any time.

When the auto-boot timer is active, a countdown is displayed at the bottom of the menu panel with the format "Auto-boot in N sec". Pressing any key cancels the countdown. If `timeout=0`, the default entry boots immediately without showing the menu. The menu supports scrolling when the entry list overflows the visible area, with a scrollbar rendered on the right edge.

## Window Manager

ForeB includes a lightweight compositor (`wm.c`) that provides a windowed GUI for the Recovery tools, Settings editor, Shell, and other built-in utilities. The window manager supports:

### Window Lifecycle

Windows are opened with `wm_open()` which allocates from a fixed pool (`WM_MAX_WINDOWS`), centers the window on screen, and raises it to the top of the z-order. Windows are closed with `wm_close()` or by clicking the close box. A callback-based event system (`wm_event_cb`) delivers mouse events (`WM_EV_MOUSE_DOWN`, `WM_EV_MOUSE_UP`, `WM_EV_MOUSE_MOVE`, `WM_EV_MOUSE_WHEEL`), keyboard events (`WM_EV_KEY`), and lifecycle events (`WM_EV_OPEN`, `WM_EV_CLOSE`) to each window's handler. The callback can return `WM_CLOSE_REQUEST` to signal the compositor to close the window.

### Dragging and Z-Order

Windows can be dragged by their title bar. The compositor tracks which window is being dragged by ID (`g_dragid`) and updates its position each frame while the left mouse button is held. The window is constrained so at least 40px of title bar remains visible. Clicking on a window raises it to the top. The z-order is maintained in a simple array (`g_order[]`), with the last entry being the focused/topmost window.

### Occlusion Culling

A top-down pass computes the visible region of each window by subtracting the footprints of opaque windows above it. The algorithm maintains a set of up to 6 visible rectangles per window, splitting each existing piece when a coverage rect overlaps it (producing at most 4 sub-rectangles per split). Fully covered windows skip their draw callback entirely, and partially covered windows are clipped to their visible bounding box. Glass (frosted) windows never occlude since their backdrop blur depends on what is beneath them.

### Content Caching

Each window optionally caches a snapshot of its rendered content. On idle frames (no events, no drag, no animation), the cache is restored via a fast row-by-row memcpy (8-byte copies for aligned rows) instead of re-running the expensive draw callback. The cache is invalidated on any input event, window move, theme change, or animation frame. Glass windows never cache because their content depends on the changing backdrop.

### Window Skins

Three window skin presets are available:
- **flat** -- Solid title bar with a 1px border.
- **beveled** -- Raised bevel highlight/shadow edges on the title bar and buttons.
- **glass** -- Translucent alpha-blended title bar with a frosted-glass backdrop blur (requires `fx_glass=1`).

Each aspect of the window chrome (title bar height, fill color, text color, border color/thickness, corner style, close-box color, button style) can be individually overridden via `win_*` config keys. Custom TGA/BMP images can replace any chrome surface via `img_window`, `img_titlebar`, and `img_button`.

### Button Widget

A reusable button widget (`wm_button_draw()`) supports five states (normal, hover, active, focused, disabled) and multiple styles (flat, raised, pill, outline, ghost, glass). Buttons can be measured (`wm_button_measure()`), hit-tested (`wm_button_hit()`), and drawn within any window's client area. The widget handles clipping gracefully -- buttons partially outside the client area are not drawn.

## Theme Customization

ForeB offers an extensive theming system that controls every visual aspect of the boot menu.

### Named Palette Presets

Twelve built-in color palettes can be selected with the `theme=` key:

`forest`, `midnight`, `nord`, `dracula`, `gruvbox`, `solarized`, `amber`, `matrix`, `rose`, `ocean`, `mono`

Each preset defines 18 colors (background, panel, border, selection, title, text, dim, timer, white, shadow, tree tiers, progress bar, and accent). Individual colors can be overridden on top of any preset using `color_bg=`, `color_fg=`, `color_accent=`, `color_sel_bg=`, `color_sel_fg=`, `color_titlebar=`, `color_window=`, and `color_cursor=` keys. Any omitted key inherits from the active preset.

### Menu Style Presets

Thirty built-in layout presets can be selected with `menu_style=`:

`classic`, `minimal`, `terminal`, `flat`, `modern`, `card`, `neon`, `outline`, `underline`, `invert`, `brackets`, `sidebar-left`, `sidebar-right`, `banner-top`, `dock-bottom`, `fullscreen`, `centered`, `compact`, `spacious`, `retro`, `glass`, `hacker`, `ribbon`, `framed`, `dashed`, `spotlight`, `pill`, `boxed`, `ghost`, `elegant`

Each preset configures:
- **Panel position** -- center, left, right, top, bottom, fullscreen, or custom permille coordinates.
- **Selection style** -- bar, doublebar, box, outline, underline, arrow, bracket, invert, pill, gradient, glow, or none.
- **Border style** -- none, thin, thick, double, shadow, glow, or dashed.
- **Corner style** -- square, round, or cut.
- **Visibility toggles** -- accent strip, dividers, gradient, shadow, title bar, footer, timer, icons, scrollbar, caret.

Every field can be individually overridden with `menu_*` config keys (e.g., `menu_selection=pill`, `menu_border=glow`, `menu_corner=round`). Geometry fields accept permille values for custom panel positioning.

### Visual Effects

Opt-in visual effects can be enabled via `fx_*` keys:

- **fx_glass** -- Enables frosted-glass backdrop blur behind windows (requires `window_skin=glass`).
- **fx_blur** -- Backdrop blur radius in pixels (0..32, default 8). Implemented as a separable box blur with O(w*h) running sums.
- **fx_opacity** -- Backdrop darken amount (0..255, default 72). Multiplies each channel by `(255 - amount) / 255`.
- **fx_vignette** -- Screen-edge darken amount (0..255, 0=off). Uses distance-from-center squared for a radial falloff.
- **fx_scanlines** -- CRT-style scanline dim on alternating rows (0..255, 0=off).

Each effect forces a full-frame flip, so they are off by default for performance.

### Widget Styling

Buttons, checkboxes, sliders, and other UI elements can be restyled via `btn_*` and `ui_*` config keys. Every aspect of button appearance (face color for each state, text color, border, corner radius, padding, gradient, shadow, glow) is independently configurable. UI-wide settings include window corner style, border width, panel opacity, separator color, scrollbar width and color, focus ring color and width, and font scale percentage.

### Custom Image Skins

Any surface can be replaced with a custom TGA or BMP image loaded from the ESP:

- `img_background` -- Menu background image (overrides `background=`).
- `img_panel` -- Menu panel face (blitted opaquely, then tinted translucently under the text).
- `img_window` -- Window client face.
- `img_titlebar` -- Window title-bar face.
- `img_button` -- Button face.
- `img_cursor` -- Cursor sprite (alias of `cursor=`).

### Audio Feedback

PC speaker beeps can be enabled for navigation feedback:

- `pcspeaker=1` -- Enable audio.
- `audio_volume` -- Volume 0..100 (PWM gate duty).
- `audio_nav_freq` / `audio_nav_ms` -- Tone for menu Up/Down navigation.
- `audio_select_freq` / `audio_select_ms` -- Tone for Enter/activate.
- `audio_open_freq` / `audio_open_ms` -- Tone for window open.
- `audio_error_freq` / `audio_error_ms` -- Tone for rejected action.
- `audio_back_freq` / `audio_back_ms` -- Tone for Esc/back.

## Configuration File

The boot menu is configured via `forebo.cfg`, a GRUB-like text file placed on the EFI System Partition at `\forebo\forebo.cfg`. The parser (`uefi/config.c`) is lenient: unknown keys are silently ignored, so the file stays forward- and backward-compatible across ForeB versions. Blank lines and lines starting with `#` are comments.

### Global Settings

These appear outside any `menuentry` block:

- `timeout=<seconds>` -- Auto-boot countdown. `0` boots the default immediately with no menu wait. Omitting this uses the compiled default (10 seconds).
- `default=<N|path>` -- Pre-selected entry. Accepts a 0-based index (`default=2`) or a title path (`default=CachyOS/linux-cachyos`).
- `remember_last=0|1` -- Persist the last booted entry in NVRAM and pre-select it on next boot.
- `background=<path>` -- Default menu background image for all entries (BMP or TGA from the ESP).
- `theme=<name>` -- Named color palette preset.
- `menu_style=<name>` -- Named layout preset.
- `animations=0|1` -- Toggle fades, particles, and spinner.
- `double_buffer=0|1` -- Toggle the off-screen back buffer.
- `window_skin=<name>` -- Compositor window style (flat, beveled, or glass).
- `mouse_enabled=0|1` -- Toggle pointer protocol polling.
- `cursor_enabled=0|1` -- Toggle the mouse cursor sprite.

### Per-Entry Blocks

Each boot option is defined with a `menuentry "Title" { ... }` block. The `type=` key selects the boot method (defaults to `forest` when omitted). Additional keys depend on the type:

- `type=forest` -- `kernel=<path>`, `module=<path>` (repeatable), `cmdline="<string>"`.
- `type=linux` -- `vmlinuz=<path>`, `initrd=<path>`, `cmdline="<string>"`.
- `type=chainload` -- `chain=<path>` (empty = auto-scan volumes).
- `type=shell|recovery|tools|settings|setup|reboot` -- No additional keys needed.
- `icon=<name|path>` -- Per-entry icon (short name or full ESP path).
- `background=<path>` -- Per-entry background override.

### Submenu Blocks

Submenus group entries under a collapsible level:

```
submenu "CachyOS" {
    icon=arch
    menuentry "linux-cachyos" {
        type=linux
        vmlinuz=/vmlinuz-linux-cachyos
        initrd=/initramfs-linux-cachyos.img
        cmdline="root=UUID=<uuid> rw"
        icon=arch
    }
}
```

Enter/Right descends, Esc/Left goes back. Nesting is capped at 8 levels.

### Example Configuration

A minimal `forebo.cfg`:

```
timeout=30
default=0
theme=forest
menu_style=classic

menuentry "Forest OS" {
    type=forest
    kernel=/forebo/kernel.elf
    module=/forebo/initrd.tar
    icon=os
}

menuentry "Linux" {
    type=linux
    vmlinuz=/vmlinuz
    initrd=/initrd.img
    icon=tux
    cmdline="root=/dev/sda2 ro"
}

menuentry "Recovery" {
    type=recovery
    icon=gear
}
```

### Image Asset Paths

All image paths (`background=`, `icon=`, `img_*=`) are ESP-absolute and may use `/` or `\` as the separator. Both refer to the same file on the FAT ESP. The `tools/gen_assets.py` script generates the shipped icon assets from source images.

## Internal Architecture

The boot menu is built from several cooperating modules:

- **`ui.c`** -- The core rendering engine. Provides primitives (`put_pixel`, `fill_rect`, `draw_string`), double-buffering, dirty-rectangle tracking, clip-rect stack, theme palette management, and the menu layout engine. Also implements visual effects (blur, vignette, scanlines) and widget rendering (buttons, checkboxes, sliders).
- **`anim.c`** -- Animation helpers. Fade-in/fade-out, particle system, loading spinner, and eased progress bar. Operates directly on the framebuffer and calls `ui_mark_dirty()` for partial-present integration.
- **`wm.c`** -- The window manager / compositor. Window lifecycle, z-order, occlusion culling, content caching, title bar rendering, close box, dragging, and the button widget.
- **`image.c`** -- BMP and TGA image decoders plus scaled and alpha-blended blitters. Format-sniffing decoder that auto-detects BMP by magic and falls back to TGA.
- **`input.c`** -- Pointer device enumeration and polling (absolute, relative, PS/2). Cursor sprite rendering. Handles the firmware-independent i8042 driver and the `ConnectController` sweep for USB devices.
- **`config.c`** -- The `forebo.cfg` parser. Reads global settings, `menuentry` blocks, `submenu` blocks, and theme/style overrides into a flat `forebo_config` struct.
- **`statusbar.c`** -- Status bar overlay for displaying system information during boot.

All modules share the `forebo_theme.h` color constants and the `forebo_cfg.h` data model. The rendering stack is entirely self-contained (no libc, no dynamic linking) and is valid both before and after `ExitBootServices` -- the only dependency is the raw framebuffer address.

# ForeB Compositor / Window Manager Design

Foundation for goals 1-3 and 10: double buffering, mouse, movable windows, and
menu entries that open as windows. This documents the model the UI code targets;
the public `ui.c` draw API (put_pixel / fill_rect / draw_char / draw_string /
ui_menu / ui_progress / ...) is **unchanged** — only its *destination* moves from
VRAM to an off-screen back buffer, plus one new `ui_present()`.

---

## 1. Double buffering (the foundation)

### Today (before this upgrade)
Three independent modules each cache their own copy of the GOP framebuffer base
and write 32bpp words **straight to VRAM (MMIO)**:

- `ui.c` — every primitive funnels through **`put_pixel()`** and **`fill_rect()`**.
- `anim.c` — separate state (`a_fb`, `a_snap`); reads VRAM back via `a_get()` for
  particle-erase and snapshot (slow MMIO read-back).
- `image.c` — `img_init(fb_base,...)` blits BMP/TGA straight to VRAM.

There is **no present step**; draws are visible immediately, causing tearing and
making the animation loop pay for MMIO round-trips.

### After (back buffer + single flip)
```
        draw calls (unchanged API)
                 |
                 v
     +------------------------+        ui_present()          +-------------+
     |  BACK BUFFER (RAM)      |  --- single fast blit --->   |  VRAM (GOP) |
     |  w*h*4, native packed   |     row-by-row, honors       |  front fb   |
     |  words (BGRA/RGBA)      |     front pitch              +-------------+
     +------------------------+
                 ^
                 |  same pointer
        anim.c (a_fb)  +  image.c (img fb)  -> all draw into RAM
```

Key decisions:

1. **Redirect at the choke points only.** Point `ui.c`'s `g_fb` at a RAM buffer.
   `put_pixel`/`fill_rect` bodies are unchanged; all ~15 higher-level primitives
   follow for free. Zero public-API change.
2. **Store NATIVE packed words** in the back buffer (exactly what `ui_pack()` /
   `a_pack()` already produce). `ui_present()` is then a dumb `memcpy` per row —
   no pixel-format conversion.
3. **Share ONE back buffer** across `ui.c`, `anim.c`, and `image.c`. `anim.c`'s
   `a_get`/`a_blend`/`anim_capture` must read what `ui.c` wrote; if `ui.c` draws
   to RAM but `anim.c` reads VRAM the snapshot/particle logic breaks. Expose the
   buffer via `ui_backbuffer()` (getter) or pass it into `anim_init`/`img_init`.
4. **`ui_present(x,y,w,h)`** optionally takes a dirty rect for partial flips so
   the ~50 ms particle tick stays cheap; `ui_present_all()` flips the whole frame.

### API additions (only these)
```c
void  ui_init(EFI_BOOT_SERVICES *bs, void *fb_base, UINTN pitch,
              UINT32 w, UINT32 h, EFI_GRAPHICS_PIXEL_FORMAT fmt); /* gains bs */
void  ui_present(void);                    /* full-frame flip  */
void  ui_present_rect(int x,int y,int w,int h);   /* dirty-rect flip */
void *ui_backbuffer(void);                 /* shared RAM base for anim/image  */
```
`AllocatePool(EfiLoaderData, w*h*4)` for the back buffer. It stays valid after
`ExitBootServices` (EfiLoaderData persists, VRAM stays writable), so `ui_present`
remains a pure `memcpy` + MMIO on the post-EBS staging bar.

### Present call sites (bootx64.c)
Add one `ui_present()` after each composite/progress site: `paint_menu()` tail,
the 50 ms particle tick, the countdown repaint, the key-driven redraw, every
`ui_progress`/`ui_status`/`anim_load_spinner`/`anim_progress_to`, and each
iteration of the chunked kernel-read loop and the post-EBS staging loop.

---

## 2. Mouse

Poll both pointer protocols each frame (declared in `uefi/efi_ext.h`):

- **`EFI_SIMPLE_POINTER_PROTOCOL`** — relative deltas; accumulate into a virtual
  cursor position, clamp to `[0,w) x [0,h)`.
- **`EFI_ABSOLUTE_POINTER_PROTOCOL`** — absolute coords (QEMU `usb-tablet`,
  touchscreens). Preferred when present: map `CurrentX/Y` from
  `[AbsoluteMin,AbsoluteMax]` linearly onto GOP resolution.

`GetState()` returns `EFI_NOT_READY` when there's no new data — that's normal,
just keep the last position. Poll alongside `ConIn->ReadKeyStroke` inside the
existing 10 ms `Stall` frame clock in `run_menu_animated()`.

The cursor sprite is drawn **onto the back buffer last** (on top of everything)
right before `ui_present()`. With double buffering there is no manual
save/restore of what's under the cursor — the whole scene is recomposited each
frame. Cursor sprite: built-in arrow, or `theme.cursor_path` TGA.

Button state: `LeftButton` (simple) / `ActiveButtons & EFI_ABSP_TouchActive`
(absolute). Track edges (press vs held vs release) for click and drag.

---

## 3. Movable windows (the compositor)

A small WM (`uefi/wm.c` + `wm.h`) sits between the scene draw and `ui_present()`.

### Window object
```c
typedef void (*wm_draw_fn)(struct wm_window *w, void *ctx);   /* client paint */
typedef int  (*wm_event_fn)(struct wm_window *w, const wm_input *in, void *ctx);

struct wm_window {
    int   x, y, w, h;         /* frame rect on the desktop            */
    int   z;                  /* z-order; higher = nearer the front   */
    int   flags;              /* VISIBLE | FOCUSED | MOVABLE | CLOSABLE */
    char  title[64];
    wm_draw_fn   on_draw;     /* paints the client area               */
    wm_event_fn  on_event;    /* consumes mouse/key while focused      */
    void        *ctx;         /* client state (shell, recovery, ...)  */
};
```

### Per-frame compositor pass
```
draw_menu_background()          # desktop / wallpaper into back buffer
  -> (animations) particles
  -> for each window in ASCENDING z-order:
         draw title bar (skin: flat|beveled|glass, theme.color_titlebar)
         draw close box (top-right)
         draw client area (theme.color_window) via on_draw()
         draw focus ring on the focused window (theme.color_accent)
  -> draw cursor sprite (top-most)
  -> ui_present()
```

### Interaction
- **Focus / raise:** left-press inside a window's frame focuses it and moves it to
  the top of the z-order.
- **Drag:** left-press on the **title bar** starts a drag; while held, translate
  `x,y` by the pointer delta each frame. Release ends the drag.
- **Close:** left-click the close box → hide/destroy; return focus to the next
  window down.
- **Hit-testing** reuses the existing menu geometry constants. Note these are
  **duplicated** in `ui.c` (permille `UIP_PANEL_*`, `UIP_ENTRY_H`) and in
  `bootx64.c`'s `draw_icons()` — any WM row hit-test must use the same numbers or
  factor them into one shared place.

### Window skins (`theme.window_skin`)
- `flat` — solid title bar + 1px border.
- `beveled` — raised highlight (top/left) + shadow (bottom/right) edges.
- `glass` — alpha-blended translucent title bar over the desktop (uses `a_blend`).

---

## 4. Menu entries that open windows (goal 10)

`type=shell` and `type=recovery` entries (and the `c` key) open the shell /
recovery **as windows** on the desktop instead of a full-screen takeover:

- In `run_menu_animated()`'s Enter handler, an entry whose `type` is
  `FOREB_ENTRY_SHELL` / `FOREB_ENTRY_RECOVERY` creates a `wm_window` whose
  `on_draw`/`on_event` wrap `shell_run()` / the recovery tools, rather than
  returning a boot index.
- Mirror the existing **`reboot` pseudo-entry** pattern (literal-match reset in
  `bootx64.c`): the same dispatch point checks `ent->type` and routes
  shell/recovery/reboot/linux/chainload to their handlers, everything else to the
  Forest multiboot path.
- The shell window returns a verdict identical to today's `shell_run()` contract
  (`FOREB_SHELL_REBOOT` / boot-index `>=0` / `FOREB_SHELL_BACK`), so the outer
  menu loop is unchanged.

---

## 5. Cross-arch note

The compositor, double buffer, mouse, shell, and recovery are all pure UEFI +
GOP and compile for x86_64 / aarch64 / riscv64 (see `arch.h`). Only the Forest
multiboot handoff is `#if FOREB_MULTIBOOT_SUPPORTED` (x86_64). On ARM/RISC-V the
same windowed UI drives Linux-boot and chainload.

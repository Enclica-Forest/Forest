# ForeB UEFI — GUI Tools, Menu UX, Pointer & Firmware-Setup Spec

Authoritative design spec for the UEFI upgrade covering: (1) the ~11 windowed
GUI tools + launcher, (2) the menu viewport / scrollbar / keyboard scrolling,
(3) the selection slide animation, (4) the pointer (mouse) fix, (5) the
firmware-setup (OsIndications) flow, and (6) the extended icon set + `icon=`
short-name resolution.

All file references are absolute-in-repo (`uefi/…`, `include/…`, `tools/…`).
Everything runs pre-ExitBootServices, freestanding (no libc), fixed pools.

---

## 0. Where this plugs into the existing code

| Concern | Existing anchor |
|---|---|
| Menu draw (panel/rows/highlight/timer) | `uefi/ui.c` `ui_menu()` lines 366-426 |
| Menu loop (input, per-frame composite) | `uefi/bootx64.c` `run_menu_animated()` ~763-880 |
| Icon blits over rows | `uefi/bootx64.c` `draw_icons()` 516-539 |
| Mouse→row mapping | `uefi/bootx64.c` `menu_hit_test()` 556-575 |
| In-menu dispatch | `uefi/bootx64.c` `menu_activate()` 727-746 |
| Post-menu dispatch (timeout path) | `uefi/bootx64.c` efi_main switch 974-1008 |
| Window template (lightweight, non-modal) | `uefi/bootx64.c` `open_recovery_window()` 600-695 |
| Window template (rich, self-contained) | `uefi/recovery.c` `recovery_run()` 407-474 |
| Pointer layer | `uefi/input.c` + `uefi/input.h` |
| Compositor | `uefi/wm.c` + `uefi/wm.h` |
| icon= parsing | `uefi/config.c` `entry_set()` 414-415 |
| icon load | `uefi/bootx64.c` `preload_assets()` 456-461 |
| Entry types | `include/forebo_cfg.h` enum 88-99 (now +TOOLS,+FWSETUP) |
| type string→enum | `uefi/config.c` `entry_type_from_str()` 131-140 |
| Icon painters/registry | `tools/gen_assets.py` `ICONS` dict |

New files this spec introduces: `uefi/tools.h` (registry + launcher + firmware
setup API — written), `uefi/tools.c` (implementation — to build against tools.h).

---

## 1. Menu viewport + scrollbar + keyboard/wheel scrolling

**Problem (verified):** `ui_menu()` draws `for(i=0;i<count;i++)` with no clamp
against the panel bottom (`ui.c:391-403`). At 1080p `entries_top≈438`, panel
bottom `≈842`, `eh≈59` → only ~6-7 rows fit but 9+ are drawn, so trailing rows
spill over the timer strip (`ui.c:418`) and footer (`ui.c:424`).

**Fix — a scrolled viewport with three synchronized copies of the geometry.**

### 1.1 Scroll state
Add a file-static scroll offset in `ui.c` plus a setter (keeps `ui_menu()`'s
public signature stable):

```c
/* ui.c */
static int g_menu_first = 0;   /* first visible entry index */
void ui_menu_set_scroll(int first);           /* clamp + store */
int  ui_menu_visible_rows(void);              /* rows that fit the panel body */
```
Declared in `ui.h`. `ui_menu_visible_rows()` computes
`avail = (py+ph - timer_margin) - entries_top; visible = avail / eh` (min 1),
using the same per-mille geometry already in `ui_menu()`.

### 1.2 Draw loop (ui.c:391-403)
Iterate only `i in [first, first+visible)`; place row at
`rowtop = entries_top + (i - first) * eh`. The `>` marker + `FOREB_SELECT`
highlight fill stay keyed on `i==selected` but are simply skipped when `selected`
is outside the window (the slide animation, §3, owns the on-screen bar).

### 1.3 Scrollbar
Only when `count > visible`, draw on the panel's right inner edge
(`x = px + pw - 10`, width 6), spanning `entries_top … entries_top+visible*eh`:
- track: `fill_rect(..., FOREB_BORDER)` dimmed,
- thumb: height `= max(12, visible*trackH/count)`, top
  `= trackTop + first*trackH/count`, color `FOREB_ACCENT`.

### 1.4 Selection-follow (bootx64.c run loop)
After any `sel` change, clamp `first` so `sel` stays visible, then push it:
```c
if (sel < first)                 first = sel;
else if (sel >= first + visible) first = sel - visible + 1;
if (first < 0) first = 0;
if (first > count - visible)     first = (count>visible)? count-visible : 0;
ui_menu_set_scroll(first);
```
Do this at: keyboard Up/Down (`bootx64.c:825-828`), mouse hover/click
(`:849-859`), and wheel (§1.5).

### 1.5 Mouse wheel
`mouse_state.wheel` already carries a per-poll wheel delta (`input.h:34`,
populated from `RelativeMovementZ` / AbsolutePointer Z). In the run loop, when
`ms.wheel` is non-zero and no window is focused, `first -= sign(ms.wheel)` (one
row per notch), clamp as above. Wheel is best-effort — usb-tablet under OVMF may
report no Z; keyboard scroll is the guaranteed path.

### 1.6 Mirror the offset into the two geometry copies
`draw_icons()` (`bootx64.c:516-539`) and `menu_hit_test()` (`:556-575`)
recompute the identical geometry. BOTH must (a) read the same `first`
(via `ui_menu_visible_rows()` + a shared `ui_menu_get_scroll()` accessor, or by
threading `first` down), (b) skip `i < first || i >= first+visible`, (c) place at
`rowtop = entries_top + (i-first)*eh`. Otherwise icons and click targets desync
from the drawn rows. This is the single most bug-prone part — keep one helper
that returns `(entries_top, eh, first, visible)` and call it from all three
sites.

---

## 2. (folded into §1) panel overflow is fully resolved by the viewport.

The timer strip and footer are never overdrawn once rows are clamped to
`[entries_top, entries_top+visible*eh)`.

---

## 3. Selection slide animation

**Goal:** when `selected` changes, the green `FOREB_SELECT` bar SLIDES from the
old row's Y to the new row's Y over N frames (double-buffered, no flicker).

**Mechanism (all in `run_menu_animated`, no ui.c signature change):**
- Locals near `sel` (`bootx64.c:763`): `int prev_sel = sel; int slide = 0;`
  where `slide` counts down remaining animation frames.
- On a `sel` change set `prev_sel = <old>`, `slide = SLIDE_FRAMES` (6 frames ≈
  100 ms at the 16 ms `Stall` cadence — snappy, not sluggish).
- Split the highlight out of `ui_menu()`: draw rows WITHOUT the select fill
  (guarded by a `ui_menu_set_highlight(-1)` "no built-in bar" flag), then paint
  the moving bar in the caller at an interpolated Y:
  ```c
  int y_old = row_screen_y(prev_sel);   /* using the §1.6 helper, honours scroll */
  int y_new = row_screen_y(sel);
  int t = SLIDE_FRAMES - slide;         /* 0..SLIDE_FRAMES */
  int y = anim_lerp(y_old, y_new, t, SLIDE_FRAMES);   /* eased */
  fill_rect(px+6, y, pw-12, eh-2, FOREB_SELECT);
  if (slide) slide--;
  ```
- Add `anim_lerp(from,to,step,steps)` to `anim.c` (there is only
  `anim_progress_to`/eased-progress today, `anim.h:93`): quadratic ease-out so
  the bar decelerates into the target. When `slide==0` the bar sits exactly on
  `sel`.
- If the destination row is scrolled off-screen, clamp the bar Y to the viewport
  edge (or trigger the scroll first so both endpoints are visible). The redraw
  already happens every frame (`ui_present()` at `bootx64.c:873`), so no extra
  present is needed.

Keep the `>` caret and per-row text redraw as-is; only the fill rect moves.

---

## 4. Pointer (mouse) fix

Root cause (confirmed in the investigation): `input.c:41,57` use
`LocateProtocol`, which binds exactly ONE arbitrary handle per GUID. Under OVMF
the pointer GUID is installed on multiple handles (real USB device + ConSplitter
aggregate); the bound instance often returns `EFI_NOT_READY` forever, so the
cursor draws (`present>0`) but never moves.

**Fix in `uefi/input.c` (structs in `efi_ext.h` are already spec-correct):**

1. **Acquire ALL handles.** Replace both `LocateProtocol` calls with
   `bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &n, &handles)` for each of
   `EFI_SIMPLE_POINTER_PROTOCOL_GUID` and `EFI_ABSOLUTE_POINTER_PROTOCOL_GUID`.
   For each handle `HandleProtocol`/`OpenProtocol` the interface. Store arrays
   (cap e.g. 8 each) in `mouse_state` instead of the single `void *simple` /
   `void *absolute`. `present = total bound instances`.
2. **Reset every instance** (`->Reset(inst, FALSE)`) at init; cache each absolute
   instance's `Mode->AbsoluteMin/Max X/Y`.
3. **Poll every instance each frame and MERGE:**
   - Absolute: `GetState`; on success map `CurrentX/Y` linearly from
     `[amin,amax]` onto `[0,screen-1]` → set `m->x/m->y` (absolute wins, it is
     authoritative). Left = `ActiveButtons & EFI_ABSP_TouchActive`.
   - Simple: `GetState`; **accumulate `RelativeMovementX/Y` DIRECTLY** — do NOT
     divide by `Mode->ResolutionX/Y` (the current `input.c:104-109` division by
     "counts/mm" collapses motion to ~0; the `±1` floor is a crawl). Apply a
     small sensitivity scalar instead, e.g. `m->x += rdx * SENS / 8` with
     `SENS≈8` (≈1:1), tune to taste. Wheel from `RelativeMovementZ`.
   - OR-in buttons across all instances; first success with motion sets `moved`.
   - Skip `EFI_NOT_READY` instances silently (normal when idle).
4. Recompute `dx/dy/moved` + button rising/falling edges once after the merge
   (as today, `input.c:120-131`). Clamp `m->x/y` to screen.

**QEMU / honesty:**
- `Makefile` `qemu-uefi` (line 736) already adds `-device qemu-xhci -device
  usb-tablet` → exercises the ABSOLUTE path. Also add `-device usb-mouse` to
  exercise the SIMPLE/relative path during testing.
- **PS/2 reality:** OVMF (no CSM) does NOT surface a PS/2 pointer to UEFI apps.
  Only USB is exposed (usb-tablet→AbsolutePointer, usb-mouse→SimplePointer).
  Document this; do NOT claim PS/2 works. On real hardware most firmware exposes
  the USB HID pointer via these same protocols, so the multi-handle poll is the
  correct portable approach.

---

## 5. Firmware / BIOS setup entry (OsIndications)

New entry type `FOREB_ENTRY_FWSETUP` (value 7) + shell command `setup`/`firmware`.
API in `uefi/tools.h`: `tools_firmware_setup_supported()`,
`tools_enter_firmware_setup()`.

**Flow (RuntimeServices via `gST->RuntimeServices`):**
1. `GetVariable(L"OsIndicationsSupported", &EFI_GLOBAL_VARIABLE_GUID, NULL,
   &sz, &supported)`. Test `supported & EFI_OS_INDICATIONS_BOOT_TO_FW_UI` (0x1).
2. If the bit is CLEAR (or the variable is absent): return `<0`. Caller shows a
   graceful message ("Firmware setup not supported by this firmware") via
   `ui_status()` or a small wm window and stays in the menu. The menu entry can
   be annotated/greyed using `tools_firmware_setup_supported()`.
3. If SET: `GetVariable(L"OsIndications", …)` (default 0 if absent), OR-in
   `EFI_OS_INDICATIONS_BOOT_TO_FW_UI`, then `SetVariable(L"OsIndications", …,
   NV|BS|RT, sizeof(UINT64), &val)`.
4. `gST->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL)` — the
   `do_reset()` pattern at `bootx64.c:180-197` already shows this call. Firmware
   reboots straight into setup.

**Dispatch wiring:**
- `entry_type_from_str()` (`config.c:131-140`): map `"setup"`→FWSETUP,
  `"firmware"`→FWSETUP, `"tools"`→TOOLS.
- `menu_activate()` (`bootx64.c:732` switch): `FOREB_ENTRY_FWSETUP` →
  `tools_enter_firmware_setup()`; on `<0` show message + `MENU_HANDLED`.
  `FOREB_ENTRY_TOOLS` → `tools_launcher_open()` + `MENU_HANDLED`.
- Post-menu switch (`bootx64.c:974-1008`): FWSETUP triggers the same call at
  timeout; TOOLS is treated like shell/recovery (nothing to boot → stay/reset).

---

## 6. The GUI tools

Registry: `forebo_tools[]` in `tools.c`, launched via `tools_launcher_open()`
(a wm window listing name+icon+desc, Up/Down+Enter, mouse hover/click/wheel).
Each tool = one `wm_open(title, w, h, draw_cb, evcb, user)` following template B
(non-modal, driven by the existing menu loop). State lives in a per-window
user struct reached via `wm_user(w)`. Client origin arrives in SCREEN coords in
the draw callback; use `ui.c` primitives. Scrolling lists reuse the §1 viewport
+ scrollbar pattern inside the client rect, and the ring-buffer/scroll idiom
from `recovery.c:79-121`.

Reuse note: several tools are the shell's text commands refactored to draw into
a window instead of printing — extract the data-gathering half of `shell.c`'s
`cmd_gpt`/`cmd_parts`/`cmd_memmap`/`cmd_efivars`/`cmd_lsblk` into shared helpers
that both the shell and the tool draw callback call.

| # | Tool (title) | icon | Shows | Interactions | Reuses |
|---|---|---|---|---|---|
| 1 | Disk Info | `disk` | Every `EFI_BLOCK_IO` device: index, media id, removable?, LBA size, block count, total bytes, logical-partition flag | Up/Down select device; scroll | `LocateHandleBuffer(BlockIoProtocol)`; shell `lsblk` |
| 2 | GPT Viewer | `disk` | Selected disk's protective MBR + GPT header (signature, rev, LBA count, disk GUID) + partition array (index, type GUID→name, part GUID, start/end LBA, size) | pick disk; Up/Down scroll the partition list; scrollbar | raw LBA1 read via BlockIo; shell `gpt` |
| 3 | Partition Browser | `disk` | Each partition's detected FS (FAT/ext4/btrfs), label, total/free where derivable | select partition → open File Browser rooted there (deferred action) | `fs_ext.c`, `fs_btrfs.c`, shell `parts` |
| 4 | File Browser (ESP) | `text` | Directory listing of the ESP tree (name, dir?, size); breadcrumb path | Up/Down; Enter dir to descend; `..`/Backspace up; Enter file → open Hex Viewer on it | `esp_open_root()` + `EFI_FILE_PROTOCOL` (`config.c` helpers), shell `ls`/`cd` |
| 5 | Hex Viewer | `text` | Hexdump (16 bytes/row, offset + hex + ASCII) of a file or raw sector | PgUp/PgDn + Up/Down + wheel scroll; `g`/`G` top/bottom | shell `hexdump`; BlockIo/FileProtocol reads |
| 6 | Memory Map | `gear` | `GetMemoryMap` descriptors: type name (Conventional/Reserved/ACPI/MMIO/…), phys start, #pages, size, attributes; totals per type | scroll; toggle bytes/pages | `gBS->GetMemoryMap`; shell `memmap` |
| 7 | EFI Variables | `gear` | Enumerate all vars (`GetNextVariableName`): name, GUID, attributes, size; select → hex of value | Up/Down + scroll; Enter → value pane | `RT->GetNextVariableName`/`GetVariable`; shell `efivars` |
| 8 | Boot Manager | `grub` | Parse `BootOrder` + each `Boot####` (attrs, description, device path summary), `BootCurrent`, `Timeout` | Up/Down; (read-only v1; write BootNext optional/deferred) | `RT->GetVariable` global vars |
| 9 | System / Firmware Info | `gear` | `gST->FirmwareVendor`/`FirmwareRevision`, UEFI spec rev, GOP mode (res/pitch/format), total RAM (from memmap), CPU count if available, secure-boot state (`SecureBoot` var) | static; scroll if long | `gST`, GOP mode, memmap sum, `SecureBoot` var |
| 10 | Theme / Settings | `gear` | Live theme colors + toggles (cursor/mouse/animations/double_buffer/window_skin) with swatches | click swatch to cycle; toggles on click; applies immediately via `wm_init(theme)` re-skin | `struct forebo_theme`, `forebo_theme.h` |
| 11 | Key Tester | `terminal` | Last key's EFI scancode + unicode + a rolling log; mouse button/pos readout | any key logs; mouse events log | `EFI_INPUT_KEY`, `mouse_state` |

Deferred-action tools (3→open browser, 4→open hex) stash a `pending` in their
user struct and let the run loop open the target window outside `wm_run_frame`,
mirroring `recovery.c`'s `rc->pending` pattern (`recovery.c:382-402`).

Window budget: `WM_MAX_WINDOWS = 8` (`wm.h:29`) — launcher + up to 7 tools.
The launcher may close itself on selection to conserve slots, or stay open.

---

## 7. Icons + `icon=` short-name resolution

### 7.1 New icons (generated by `tools/gen_assets.py`, all 32x32 32-bit TGA + alpha)
Added painters + `ICONS` registry entries (verified to emit valid 4114-byte
TGAs): `ubuntu`, `debian`, `arch`, `fedora`, `mint`, `tux`, `windows`, `grub`,
`usb`, `disk`, `terminal` (plus existing `os`, `gear`, `shield`, `reboot`,
`text`, `safe`). Colors are documented inline in `gen_assets.py` (Ubuntu plum +
orange ring, Debian red swirl, Arch blue mountain, Fedora blue `f`, Mint green
leaf, Tux penguin, Windows blue four-pane, GRUB console, USB trident, HDD
platter, terminal screen). `make assets` / `python3 tools/gen_assets.py` writes
them to `assets/icons/<name>.tga`, deployed to `/forebo/icons/<name>.tga`.

### 7.2 Name→path resolution (`tools_icon_path()`, `uefi/tools.h`)
Today `icon=` is a RAW path (`config.c:414-415` `scopy` verbatim). Add short-name
resolution so `icon=arch` works:
- If the value contains `/` or `\` OR ends in `.tga`/`.bmp` → treat as a raw
  path (unchanged).
- Otherwise rewrite to `/forebo/icons/<value>.tga`.

Hook at ONE of (pick 7.2a):
- **(a) preferred** `config.c` `entry_set()` icon case (`config.c:414`): rewrite
  before `scopy`, so the stored `e->icon` is always a full path and
  `preload_assets()` (`bootx64.c:456-461`) is unchanged.
- (b) `preload_assets()` before `esp_ascii_to_char16` (`bootx64.c:458`).

The same `tools_icon_path()` resolves the tool registry `icon` field, so tools
and menu entries share one table.

### 7.3 Suggested default menu-entry icons (forebo.cfg)
`Linux(vmlinuz)`→`tux`, `Chainload GRUB(USB)`→`grub`, `Boot removable`→`usb`,
`ForeB Shell`→`terminal`, `Recovery / Disk Tools`→`gear`, `Tools`→`gear`,
`Forest OS`→`os`, `Reboot`→`reboot`, `Firmware Setup`→`gear`. Distro icons
(`ubuntu`/`debian`/`arch`/`fedora`/`mint`/`windows`) are available for user
`menuentry` blocks that chainload those systems.

---

## 8. Honest status of this deliverable

**Written / done in this pass:**
- `include/forebo_cfg.h` — `FOREB_ENTRY_TOOLS` (6) + `FOREB_ENTRY_FWSETUP` (7)
  appended (existing values unchanged); doc comment updated.
- `uefi/tools.h` — full registry struct, launcher + firmware-setup API, all 11
  `tool_*_open()` declarations, `tools_icon_path()`, OsIndications constant.
- `tools/gen_assets.py` — 11 new distro/tool icon painters + registry entries,
  documented palette; verified to generate valid TGAs.
- This spec (`GUI_TOOLS.md`).

**Left to implement (separate pass), against the anchors above:**
- `uefi/tools.c` — the 11 tool callbacks + launcher + firmware-setup impl.
- `uefi/input.c` — LocateHandleBuffer multi-handle poll + remove Resolution
  division (§4).
- `uefi/ui.c` + `uefi/bootx64.c` — viewport/scrollbar/scroll (§1), slide anim
  (§3), and the 3-site geometry helper.
- `uefi/config.c` — `entry_type_from_str` `tools`/`setup`/`firmware`; `icon=`
  short-name rewrite (§7.2).
- `uefi/bootx64.c` `menu_activate()` + post-menu switch — TOOLS/FWSETUP cases.
- `uefi/shell.c` — `setup`/`firmware` command.
- `Makefile` — add `uefi/tools.c` to the build; optional `-device usb-mouse`.

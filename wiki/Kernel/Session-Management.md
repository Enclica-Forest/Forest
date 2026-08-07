# Session Management

Forest OS supports a full multi-session environment with up to 22 concurrent TTY sessions, a graphical desktop on TTY 1, text consoles on TTYs 2-22, and a kernel-side display manager that handles smooth transitions between them. This page explains how it all fits together.

## Overview

The session system is built around three main layers:

1. **Session Manager** (`session.c`) -- Tracks login state, launches shells and desktop environments, handles crash recovery
2. **TTY Subsystem** (`tty.c`, `tty_devices.c`, `tty_render.c`) -- Renders text to the framebuffer, manages multiple virtual terminals, handles ANSI escape codes
3. **Display Manager** (`display_manager.c`) -- Controls which display client (GUI or TTY) owns the framebuffer, handles fade transitions

The kernel boots into TTY 1 as a graphical session. After login, the user can switch between sessions using Ctrl+Alt+F1-F12 (and chvt for TTYs 13-24).

## Session Architecture

### The tty_session_t Structure

Every session is represented by a `tty_session_t` struct defined in `include/session.h`:

```c
typedef struct {
    uint32_t session_id;         // 1-22
    session_type_t type;         // SESSION_TYPE_GUI or SESSION_TYPE_TEXT
    session_state_t state;       // LOGIN, ACTIVE, LOGOUT, SUSPENDED, RECOVERY
    bool logged_in;
    auth_user_info_t user_info;
    uint32_t shell_pid;          // PID of shell or desktop environment
    bool gui_suspended;
    uint32_t de_crash_count;     // Desktop environment crash counter
    void* fb_snapshot;           // Framebuffer snapshot for VT switching
    uint32_t fb_snapshot_size;
    bool fb_snapshot_valid;
} tty_session_t;
```

Sessions are stored in a static array `g_tty_sessions[MAX_TTY_SESSIONS]` (22 slots). TTY 1 is always initialized as `SESSION_TYPE_GUI`, while all other TTYs default to `SESSION_TYPE_TEXT`.

### Session States

Each session goes through a lifecycle:

| State | Meaning |
|-------|---------|
| `SESSION_STATE_LOGIN` | Waiting for user to authenticate |
| `SESSION_STATE_ACTIVE` | User logged in, shell/DE running |
| `SESSION_STATE_LOGOUT` | Session ended, cleaning up |
| `SESSION_STATE_SUSPENDED` | Paused (e.g., when switching away from a GUI session) |
| `SESSION_STATE_RECOVERY` | Desktop environment crashed, attempting restart |

## TTY Sessions

### Multiple Virtual Terminals

Forest OS supports up to 24 virtual terminals (VTs), though session IDs are limited to 22. The TTY subsystem maintains 24 VT buffers (`g_vt_buffers[TTY_VT_COUNT]`), each with its own cell grid, cursor position, colors, and scroll state.

VTs are organized as:

| VT | Purpose |
|----|---------|
| VT 1 | Graphical desktop (GUI login, display manager) |
| VT 2 | Alternate graphical mode |
| VT 3-24 | Text TTY consoles (session IDs 1-22) |

The current VT is tracked by `g_current_vt` in `tty.c` and `g_current_tty_session` in `hotkey.c`. When you switch VTs, the TTY subsystem saves the current cell buffer and restores the target one.

### TTY Cell Buffers

Each VT maintains its own cell buffer:

```c
typedef struct {
    char ch;
    uint8_t attr;
    uint8_t dirty;
} tty_cell_t;
```

The cell grid is dynamically allocated based on the framebuffer resolution and 8x8 font metrics. For example, a 1024x768 display with an 8x8 font gives 128 columns and 90 rows (minus the 24-pixel status bar).

### VT Switching Fade Effect

When switching between text VTs, Forest OS applies a brief fade transition using `tty_draw_fade_overlay()` from `tty_render.c`. The transition uses 6 frames (`VT_TRANSITION_FRAMES`) to smoothly darken the outgoing VT before restoring the incoming one.

## GUI vs Text Session Types

### GUI Sessions (SESSION_TYPE_GUI)

GUI sessions launch a desktop environment (DE) -- typically the shell binary configured via `sys.conf` or the user's `~/.session/.conf`. The process:

1. The kernel loads the DE ELF binary from VFS (or uses a preloaded copy)
2. Creates a task with `task_create_elf()` and sets it as the foreground graphics task
3. Starts the window manager render loop via `wm_start_render_loop_task()`
4. Waits for the DE to map the framebuffer (`framebuffer_has_userspace_mapping()`)
5. Tracks ownership via `g_fb_owner_session_active` and `g_fb_owner_session`

When a GUI session is active, TTY output is suppressed (`tty_state.graphics_app_active = true`) and the status bar is hidden.

### Text Sessions (SESSION_TYPE_TEXT)

Text sessions simply launch `/bin/shell` (or the embedded shell). The shell process gets a controlling TTY and standard job control. Output renders directly to the framebuffer via the TTY cell renderer.

### Fallback Behavior

If the graphics backend is unavailable (no framebuffer, display mode change failed, etc.), GUI sessions automatically fall back to text mode. The DE launch process has multiple retry paths and backup binary locations.

## The Login Process

### Authentication

The login prompt (`prompt_tty_login()` in `session.c`) handles:

1. Displaying a banner: `Fern - TTY N`
2. Prompting for username and password
3. Calling `auth_login()` from `auth.c` which verifies against `/etc/shadow`
4. Supporting `signup` at the username prompt to create new accounts
5. Handling Ctrl+Alt+F-key switches mid-login (aborts input cleanly)

The authentication system uses SHA-256 with 200,000 iterations of key stretching. Passwords are stored as salted hashes in `/etc/shadow` on the initrd. Legacy single-round hashes from existing shadow files are lazily upgraded on first successful login.

### Login Flow

```
session_run()
  -> session_init_all()         // Initialize 22 session slots
  -> auth_init()                // Load /etc/shadow, create root user
  -> while(1) loop:
       -> session_check_recovery()
       -> run_session_login()
            -> tty_suspend_wm() // Stop compositor for text mode
            -> prompt_tty_login()
            -> launch_user_session()
                 -> GUI path: load DE ELF, start WM, wait for FB mmap
                 -> TEXT path: load /bin/shell, set controlling TTY
            -> wait for shell/DE to exit
            -> session_cleanup_session()
            -> auth_logout()
```

### Auto-Login

When `autologin_root` is true (configurable at boot), the root user is automatically logged in without a password prompt. This is useful for development and headless setups.

## Session Switching

### Ctrl+Alt+F1-F12

The hotkey system (`hotkey.c`) registers default mappings at boot:

- **Ctrl+Alt+F1**: Switch to VT1 (graphical desktop)
- **Ctrl+Alt+F2**: Switch to VT2 (alternate GUI)
- **Ctrl+Alt+F3-F12**: Switch to TTY consoles (sessions 1-10)
- **Alt+SysRq**: Emergency switch to TTY console

The hotkey manager registers with the input multiplexer at EXCLUSIVE priority, so it intercepts key events before focused applications can consume them.

### Switching Flow

When you press Ctrl+Alt+F3, the hotkey handler (`hotkey_handler_switch_to_tty_vt()`) does:

1. Updates `g_current_tty_session` directly
2. Releases graphics app ownership (`tty_set_graphics_app_active(false)`)
3. Unmaps framebuffer from all userspace processes
4. Resumes the window manager render task
5. Calls `display_manager_switch_mode(DISPLAY_MODE_TTY_CONSOLE)`
6. Calls `tty_switch_vt()` to swap VT buffers

### chvt Command

Users can also switch TTYs from the command line using `chvt N` (where N is 1-24). This goes through the same VT-switching path as the hotkey.

## Framebuffer Snapshots

### Why Snapshots?

When switching between two GUI sessions (e.g., Ctrl+Alt+F1 to Ctrl+Alt+F2), the kernel needs to preserve each session's display state. Without snapshots, switching back would show a black screen or stale content.

### Snapshot Mechanism

Each `tty_session_t` has `fb_snapshot`, `fb_snapshot_size`, and `fb_snapshot_valid` fields. The snapshot functions:

- `session_save_framebuffer(session_num)` -- Copies the current framebuffer contents into the session's snapshot buffer (allocated with `kmalloc`)
- `session_restore_framebuffer(session_num)` -- Copies the snapshot back to the framebuffer, followed by an `mfence` instruction to ensure memory ordering

### Current Status

The framebuffer snapshot system is defined but not currently wired into the real VT switch path. The hotkey handler (`hotkey.c`) performs direct VT switches without going through the session pipeline. This is a known gap documented in the code -- the snapshot/cross-fade pipeline and the hotkey's direct switch differ in ways that aren't safe to reconcile without careful testing.

## Fade Transitions Between Sessions

### Session Cross-Fade

The session system includes a cross-fade transition (`session_perform_transition()`) for switching between two GUI sessions:

1. **Fade out**: Gradually darken the outgoing session's framebuffer over `SESSION_TRANSITION_HALF_TICKS` (4 ticks)
2. **Restore**: Copy the incoming session's snapshot to the framebuffer
3. **Fade in**: Gradually brighten the incoming session over 4 more ticks

Each step processes a portion of the screen per tick, using pixel-by-pixel alpha blending. The fade uses a progress value (0-256) to blend old and new pixel values.

### Display Manager Transitions

The display manager (`display_manager.c`) has its own transition system:

- `display_manager_start_transition()` -- Begins a fade between two display modes
- `display_manager_update_transition()` -- Performs alpha blending each frame
- Uses offscreen framebuffers to save/restore state during transitions
- Default duration: 300ms

### Text VT Transitions

For text-mode VT switches, `tty_draw_fade_overlay()` applies a simpler darkening effect over 6 frames. This is handled by the TTY renderer rather than the session manager.

## Console Output Management

### The TTY Renderer

The TTY renderer (`tty.c`) supports multiple backends:

- **Framebuffer backend**: Renders 8x8 bitmap font glyphs directly to the framebuffer. Used when graphics are available.
- **Arch console backend**: Falls back to serial output or architecture-specific console. Used on non-x86 platforms or when framebuffer is unavailable.

Cell rendering (`tty_render_cell()`) checks for dirty cells and only redraws changed content, which is critical for performance on emulators like VirtualBox.

### Status Bar

The status bar (`tty_render.c`) displays at the top of each text VT with:

- TTY number label
- Boot logo (loaded from `/usr/share/images/bootup/logo.bmp`)
- Login status or current username
- Keyboard modifier indicators (Ctrl, Alt, Shift)
- CPU core dots (colored circles for each core)
- Real-time clock
- Scroll position indicator

The status bar is hidden when a graphical app is active and shown again for text sessions.

### Output Suppression

When a GUI session owns the framebuffer, TTY output is suppressed via `tty_state.graphics_app_active`. This prevents text output from corrupting the desktop display. The `print()` function in `screen.c` checks this flag and returns immediately when GUI mode is active.

### Panic Lockdown

During a kernel panic, `tty_enter_panic_lockdown()` sets a one-way latch that blocks all normal TTY mutations. The crash screen bypasses the normal TTY system and writes directly to the framebuffer using the `crash_font` bitmap.

## Terminal Emulation

### ANSI Escape Code Processing

The TTY parser (`tty.c`) handles standard ANSI/VT100 escape sequences:

- **CSI sequences**: Cursor movement, erase, color changes, scroll regions
- **OSC sequences**: Window title setting
- **DCS sequences**: Device control strings

The parser maintains state across characters (NORMAL -> ESC -> CSI -> OSC -> DCS) and supports 256-color palette with a 6x6x6 color cube and 24 grayscale ramp.

### Termios Support

Virtual TTY devices (`tty_devices.c`) implement POSIX termios interface:

- **Canonical mode** (ICANON): Line buffering with edit cursor, backspace, arrow key navigation
- **Non-canonical mode**: Raw input with VMIN/VTIME timeouts
- **Signal handling**: Ctrl+C (SIGINT), Ctrl+Z (SIGTSTP), Ctrl+\ (SIGQUIT)
- **Flow control**: XON/XOFF (IXON/IXOFF)
- **Output processing**: NL->CRNL translation (ONLCR), carriage return handling

### Background Process Handling

The TTY device driver implements POSIX job control:

- Background reads from controlling terminal generate SIGTTIN
- Background writes (with TOSTOP) generate SIGTTOU
- Orphaned process groups get EIO instead of signals
- Foreground process group tracking via `fg_pgid`

## Session Configuration

### System Configuration

The desktop environment path is configured via:

1. **User config**: `/home/<username>/.session/.conf` -- Per-user DE preference
2. **System config**: `/usr/share/sysconf/sys.conf` -- System-wide default

The config file supports lines like:
```
desktop=shell
DE=shell
de=/usr/bin/mydesktop
```

Path resolution rules:
- Absolute paths (`/usr/bin/mydesktop`) are used as-is
- Paths starting with `/` but no prefix get the leading slash stripped
- Bare names (no `/`) resolve to `/usr/bin/<name>`
- `.elf` suffixes are stripped (modern binaries carry no extension)

### Session Header Constants

```c
#define MAX_TTY_SESSIONS 22
#define SESSION_RECOVERY_LIMIT 3
#define SESSION_INPUT_MAX 64
#define SESSION_DE_PATH_MAX 256
#define SESSION_TRANSITION_FADE_TICKS 8
#define SESSION_TRANSITION_HALF_TICKS 4
```

## Display Manager

### Architecture

The display manager (`display_manager.c`) provides a client-server model for display output:

- **Clients**: Registered display consumers (TTY console, desktop, fullscreen app)
- **Master framebuffer**: The physical framebuffer that clients render to
- **Offscreen buffers**: Each client gets a private framebuffer for save/restore
- **Overlays**: Z-ordered transparent layers composited on top of the active client

### Display Modes

```c
typedef enum {
    DISPLAY_MODE_TTY_CONSOLE,    // Text console
    DISPLAY_MODE_DESKTOP,        // Graphical desktop
    DISPLAY_MODE_FULLSCREEN_APP, // Fullscreen application
    DISPLAY_MODE_TRANSITION,     // Mid-transition state
} display_mode_t;
```

### Client Registration

Display clients register with callbacks:
- `frame_callback`: Called to render a frame
- `input_callback`: Called to process input events
- `suspend`/`resume`: Called during mode switches

The TTY console is the default client. The desktop environment registers itself when it starts.

### Transition System

When switching modes, the display manager:

1. Saves the current framebuffer to an offscreen buffer
2. Performs alpha blending between old and new content
3. Supports dirty-region tracking to only redraw changed areas
4. Uses a spinlock to prevent races during transitions

The transition duration defaults to 300ms but can be configured via `display_manager_set_transition_fade()`.

## Key Source Files

| File | Purpose |
|------|---------|
| `src/session.c` | Session lifecycle, login, DE/shell launch |
| `src/include/session.h` | Session type definitions |
| `src/tty.c` | TTY rendering, ANSI parser, VT buffers |
| `src/tty_devices.c` | Virtual TTY device drivers, termios |
| `src/tty_render.c` | Status bar, CPU dots, fade effects |
| `src/tty_internal.h` | Internal TTY interface |
| `src/display_manager.c` | Display mode switching, transitions |
| `src/hotkey.c` | Ctrl+Alt+F-key handling |
| `src/auth.c` | User authentication, password hashing |
| `src/screen.c` | Legacy VGA text console, TUI primitives |
| `src/kb.c` | Keyboard input, scancode translation |

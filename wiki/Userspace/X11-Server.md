# Forest OS X11 Server (forest-x11)

`forest-x11` is a userspace X11 display server for Forest OS. It implements
enough of the X11R6 protocol to run simple Xlib clients like `xterm` and
`xclock`, compositing all windows into a framebuffer and presenting the result
to the kernel's display hardware.

---

## Table of Contents

1. [What forest-x11 Is](#what-forest-x11-is)
2. [Userspace vs In-Kernel X11](#userspace-vs-in-kernel-x11)
3. [X11 Protocol Implementation](#x11-protocol-implementation)
4. [Client Connection Management](#client-connection-management)
5. [Window Management](#window-management)
6. [Input Handling](#input-handling)
7. [Display Rendering](#display-rendering)
8. [Kernel Communication](#kernel-communication)
9. [X11 Extensions](#x11-extensions)
10. [Configuration and Usage](#configuration-and-usage)
11. [Performance Characteristics](#performance-characteristics)
12. [Limitations](#limitations)
13. [Source Files Reference](#source-files-reference)

---

## What forest-x11 Is

A self-contained, single-process, single-threaded X11 display server that
runs as a standard Forest OS userspace process. It listens on the Unix domain
socket `/tmp/.X11-unix/X0`, accepts X11 client connections, parses the wire
protocol, manages windows, renders in software, composites into a back buffer,
and flips to the framebuffer via kernel syscalls.

- ~2000 lines of C across 15 source files
- Software-only rendering (no GPU, no SSE -- explicitly disabled)
- Custom 2 MB heap allocator (no libc malloc)
- Hardcoded limits: 16 clients, 256 windows, 64 GCs, 64 pixmaps
- Built with the `i686-forestos` cross-toolchain

---

## Userspace vs In-Kernel X11

Forest OS has two X11 server implementations:

| Feature | In-kernel (Fern) | Userspace (forest-x11) |
|---------|-----------------|----------------------|
| Runs in | Kernel context | Userspace process |
| IPC | In-kernel ring buffers | POSIX Unix sockets |
| Window limit | 64 | 256 |
| Rendering | graphics_manager API | Direct framebuffer mmap |
| Input | Input mux callback | Direct syscall polling |
| Crash impact | Kernel panic | Process dies |
| Build cycle | Full kernel rebuild | Standalone build |

The in-kernel server (`fern/src/x11_server.c`, ~2100 lines) uses custom IPC
ring buffers because Forest OS lacked a POSIX socket layer. It directly calls
kernel graphics/window manager APIs. The userspace server was written as a
cleaner alternative with process isolation, standard sockets, and easier
debugging.

---

## X11 Protocol Implementation

### Request Format

Each X11 request starts with a 4-byte header (opcode, pad, 16-bit length in
4-byte units), followed by the request body. The server dispatches through a
`switch` statement in `handle_request()` (`main.c:944`).

### Supported Core Requests

| Opcode | Request | Opcode | Request |
|--------|---------|--------|---------|
| 1 | CreateWindow | 53 | CreatePixmap |
| 2 | ChangeWindowAttributes | 54 | FreePixmap |
| 3 | GetWindowAttributes | 55 | CreateGC |
| 4 | DestroyWindow | 56 | ChangeGC |
| 8 | MapWindow | 60 | FreeGC |
| 9 | MapSubwindows | 61 | ClearArea |
| 10 | UnmapWindow | 62 | CopyArea |
| 12 | ConfigureWindow | 63 | CopyPlane |
| 14 | GetGeometry | 64 | PolyPoint |
| 15 | QueryTree | 65 | PolyLine |
| 16 | InternAtom | 66 | PolySegment |
| 17 | GetAtomName | 67 | PolyArc |
| 18 | ChangeProperty | 69 | FillPoly |
| 19 | DeleteProperty | 70 | PolyFillRectangle |
| 20 | GetProperty | 71 | PolyFillArc |
| 21 | ListProperties | 72 | PutImage |
| 42 | SetInputFocus | 73 | GetImage |
| 43 | GetInputFocus | 74 | PolyText8 |
| 45 | OpenFont | 76 | ImageText8 |
| 46 | CloseFont | 84 | AllocColor |
| 47 | QueryFont | 85 | AllocNamedColor |
| 48 | ListFonts | 89 | QueryColors |
| 49 | ListFontsWithInfo | 92-94 | Selection ops |
| | | 127 | NoOp |

Replies are 32-byte headers (byte 0 = `1`, byte 1 = status, bytes 2-3 =
sequence number) with optional trailing data.

---

## Client Connection Management

### Socket and Handshake

The server binds to `/tmp/.X11-unix/X0` with a listen backlog of 8. On
connection it immediately sends two 32-byte setup messages (protocol version
and pixel format). There is no full X11 connection setup reply with visual
types -- clients that require this may need patches.

### Client Slots

16 fixed client slots (`X11_MAX_CLIENTS`), each holding:

- File descriptor (non-blocking)
- Sequence number counter (incremented per request)
- 8 KB rx/tx buffers (currently unused -- direct read/write)

On `read()` returning 0, the client slot is freed. The server is single-threaded
and processes all clients in one loop iteration.

---

## Window Management

Each window is a 256-byte struct in a flat array with: id, parent, position,
size, event mask, background color, mapped flag, z-order, a pixel surface
(allocated from the 2 MB heap), dirty flag, title (64 bytes), and up to 32
properties (256 bytes each).

**Key operations:**

- **Create**: Allocates surface, fills with background color
- **Destroy**: Frees surface, clears slot
- **Map/Unmap**: Toggles visibility
- **Configure**: Moves/resizes; reallocates surface on size change
- **Hit testing**: Iterates mapped windows, returns highest z-order match

Z-order is a monotonically increasing counter -- new windows always end up on
top. Properties support `WM_NAME` and `_NET_WM_NAME` to set window titles.

---

## Input Handling

Polled once per main loop via `x11_input_poll()` (`sys.c:106`):

- **Mouse**: `SYS_POLL_INPUT` (481) checks availability, `SYS_READ_MOUSE`
  (480) reads events. Relative deltas accumulate into position (clamped to
  screen), button bits toggle on press/release.
- **Keyboard**: `SYS_READ_KBD` (479) reads events. 256-byte key state array
  plus shift/ctrl/alt/caps_lock flags.

**Focus routing** (`main.c:1047`): After polling, the server finds the window
under the cursor. If it changed, sends `FocusOut` to the old and `FocusIn` +
`EnterNotify` to the new window. Motion events go to the focused window.
All events are broadcast to every connected client (no event mask filtering).

---

## Display Rendering

### Compositing Pipeline

1. Fill back buffer with dark blue (`0xFF1A1A2E`)
2. Collect all mapped windows with surfaces
3. Sort by z-order (insertion sort)
4. For each window (back-to-front): blit surface pixels at window position
5. Convert ARGB8888 to framebuffer format (BGRA or RGBA)
6. Copy back buffer to framebuffer row-by-row (`memcpy`)
7. Call `SYS_FB_FLUSH` to present

### Drawing Primitives (`x11_draw.c`)

- Rectangles, lines (Bresenham), circles (midpoint), arcs (64-segment approximation)
- Scanline polygon fill
- 8x8 bitmap font for ASCII 32-126
- Image blit and area copy
- All operations support per-pixel alpha blending

### Graphics Contexts

64 GC slots store fg/bg colors, function (only GXcopy), plane mask, line
width, cap/join/fill styles. Drawing reads colors from the GC.

---

## Kernel Communication

| Syscall | Number | Purpose |
|---------|--------|---------|
| `SYS_GET_FB_INFO` | 473 | Query framebuffer dimensions/format |
| `SYS_MMAP_FB` | 471 | Map framebuffer into address space |
| `SYS_FB_FLUSH` | 478 | Present framebuffer to display |
| `SYS_FB_DIRTY_RECT` | 496 | Partial screen update hint (unused) |
| `SYS_POLL_INPUT` | 481 | Check for pending input |
| `SYS_READ_KBD` | 479 | Read keyboard event |
| `SYS_READ_MOUSE` | 480 | Read mouse event |

The framebuffer is memory-mapped for direct pixel access. All other
communication uses Forest OS custom syscalls via libc wrappers.

---

## X11 Extensions

**None.** The userspace server does not handle QueryExtension (opcode 98)
or ListExtensions (opcode 99). The in-kernel server (Fern) does, but
forest-x11 is limited to core X11 protocol only.

---

## Configuration and Usage

No configuration file. All behavior is hardcoded: display `:0`, background
color dark blue, fixed 8x8 font, 32-bit color depth (kernel-dependent format).

```bash
# Build
cd userspace/forest-x11 && make

# Run
forest-x11 &

# Connect clients
export DISPLAY=:0
xterm &
```

Startup sequence: init heap (2 MB) -> mmap framebuffer -> initialize atoms,
windows, GCs, pixmaps, events, fonts, colors, input -> create Unix socket
-> enter main loop.

---

## Performance Characteristics

**Strengths:** Low per-frame overhead, no dynamic allocation during rendering,
small footprint (~2 MB total).

**Weaknesses:** Full-screen copy every frame (no dirty tracking), O(n) window
compositing with pixel-by-pixel blitting, software alpha blending without SIMD,
per-request processing without batching, no event coalescing.

**Scaling:** 16 clients, 256 windows (but compositing all would be slow), 2 MB
heap limits total surface memory.

---

## Limitations

- No extension support
- No cursor rendering (kernel-managed)
- No window decorations or title bars
- No subwindow clipping optimization
- No backing store or save-unders
- No colormap support
- No GRAB/UNGRAVB operations
- Font is always the same 8x8 bitmap regardless of name
- Named color table is 20 hardcoded colors
- `QueryColors` always returns zeros
- No true window hierarchy enforcement
- No per-client event filtering
- No keyboard mapping or key repeat
- No authentication (any process can connect)
- Heap can fragment under heavy window creation

---

## Source Files Reference

| File | Lines | Purpose |
|------|-------|---------|
| `main.c` | 1200 | Event loop, request dispatch, compositing |
| `x11_protocol.{h,c}` | 146 | Wire format, request/reply/event helpers |
| `x11_socket.{h,c}` | 143 | Unix socket setup and client I/O |
| `x11_window.{h,c}` | 261 | Window create/destroy/map/configure |
| `x11_events.{h,c}` | 223 | Event construction and delivery |
| `x11_draw.{h,c}` | 310 | Software rasterizer |
| `x11_gc.{h,c}` | 108 | Graphics context management |
| `x11_atoms.{h,c}` | 94 | Atom interning and lookup |
| `x11_color.{h,c}` | 80 | Color allocation and named colors |
| `x11_font.{h,c}` | 73 | Font open/close/query |
| `x11_pixmap.{h,c}` | 66 | Pixmap create/free |
| `sys.{h,c}` | 203 | Kernel syscall wrappers |
| `mem.{h,c}` | 85 | Custom 2 MB heap allocator |

---

*This wiki page covers forest-x11 as of the current source tree.*

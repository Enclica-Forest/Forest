# Forest OS Graphics Subsystem

Forest OS has a surprisingly ambitious graphics stack for a monolithic kernel. What started as "let's show stuff on screen" grew into a multi-layered system with hardware drivers, a software compositor, font rendering, a window manager, and even in-kernel Wayland and X11 servers. This page walks through how it all fits together.

---

## 1. Framebuffer Architecture

At the heart of the graphics system is a framebuffer abstraction that supports **double-buffered rendering** -- the key technique for smooth, flicker-free display updates.

The framebuffer structure (`framebuffer_t` in `include/graphics/graphics_types.h`) carries everything the kernel needs to know about the display surface:

```c
struct framebuffer {
    uintptr_t virtual_addr;     // Mapped virtual address
    uintptr_t physical_addr;    // Physical framebuffer address
    size_t size;                // Total size in bytes
    uint32_t width, height;     // Visible resolution
    uint32_t pitch;             // Bytes per scanline
    pixel_format_t format;      // Pixel format enum
    uint32_t bpp;               // Bits per pixel

    // Double buffering support
    uintptr_t back_buffer;      // Off-screen rendering target
    bool double_buffered;       // Whether double-buffering is active
};
```

When double-buffering is enabled, all drawing operations write to the **back buffer** (an allocated region of kernel memory the same size as the visible framebuffer). When the frame is complete, `graphics_swap_buffers()` copies the back buffer to the front buffer in one bulk `memcpy`. This eliminates tearing during complex redraws like the splash animation or window manager compositing.

The `framebuffer_dbuf.c` module adds **dirty-rectangle tracking** on top of this. Instead of always doing a full-screen copy, the system tracks which regions of the screen changed and only copies those. If too many dirty regions accumulate (above `FB_DIRTY_FULL_THRESHOLD`), it collapses to a full-screen invalidate to avoid the overhead of tracking dozens of tiny rects.

### Render Layers

The kernel also has a **z-ordered layer compositor** (`render_layers.c`). Each layer (splash, TTY, GUI, overlays) renders into its own off-screen buffer, and the compositor blits them bottom-to-top into the master framebuffer. This is how the splash screen smoothly transitions into the TTY console, which in turn transitions into the graphical window manager.

---

## 2. Graphics Drivers

The V2 driver architecture (`graphics_manager_v2.c`) is a clean, modular system with a priority-based selection mechanism. Drivers register themselves, the system probes for hardware, and the best driver wins.

### Driver Priority Order

Drivers are assigned priorities (higher = preferred):

| Priority | Driver | Target |
|----------|--------|--------|
| 200 | VESA VBE | Multiboot/UEFI framebuffer |
| 180 | Bochs BGA | QEMU, Bochs, VirtualBox |
| 170 | VMware SVGA | VMware Workstation/Player/ESXi |
| 150 | Intel HD | Intel integrated graphics |
| 140 | AMD/ATI | AMD Radeon GPUs |
| 130 | NVIDIA | NVIDIA GeForce GPUs |
| 100 | VGA Text | Universal fallback |
| 50 | Software FB | Last resort |

### VESA VBE Driver

The VESA driver (`vesa_vbe_driver_v2.c`) uses the framebuffer pre-configured by the bootloader (GRUB multiboot or UEFI GOP). In protected mode you can't call BIOS functions, so this driver is essentially a "here's what the bootloader gave us" wrapper. It provides a linear framebuffer interface but cannot change video modes at runtime -- the mode is fixed at boot time.

### Bochs BGA Driver

The BGA driver (`bochs_bga_driver_v2.c`) is the workhorse for virtual machines. It supports Bochs, QEMU (with `-vga std`), and VirtualBox. Features include:

- Linear framebuffer support (LFB)
- Virtual display for hardware scrolling
- 8/15/16/24/32-bit color depths
- PCI BAR detection for framebuffer address
- Proper pitch alignment (critical for 24bpp modes)

The driver talks to the BGA hardware through I/O ports (`VBE_DISPI_IOPORT_INDEX` / `VBE_DISPI_IOPORT_DATA`) and auto-detects the BGA version ID to determine supported features.

### VMware SVGA-II Driver

The VMware driver (`vmware_svga_driver_v2.c`) is the most feature-rich VM driver. It uses the SVGA-II command interface with a **FIFO command buffer** for 2D acceleration. Key capabilities:

- SVGA-II FIFO for batch rendering commands
- Hardware cursor support
- Capability detection (resolution limits, VRAM size)
- Pixel format mask negotiation

The driver maintains its own FIFO memory (minimum 256 KB) and communicates with the VMware device through memory-mapped registers and I/O ports.

### Intel HD Graphics Driver

The Intel driver (`intel_hd_driver_v2.c`) supports generations from Ironlake (Gen 5) through Tiger Lake/Alder Lake (Gen 12). It primarily uses the mode configured by the BIOS/UEFI since full mode-setting requires extensive register programming. The driver:

- Detects the Intel generation via PCI device ID
- Maps MMIO registers for the graphics engine
- Reads EDID via GMBUS/I2C when available
- Uses the pre-configured GOP framebuffer

### NVIDIA Driver

The NVIDIA driver (`nvidia_driver_v2.c`) covers architectures from RIVA TNT (NV04) through Ada Lovelace (RTX 40 series). It's a "use what BIOS gave you" driver -- in protected mode without full driver support, it maps the BIOS-configured framebuffer and provides basic drawing. Full NVIDIA support would require the extensive register documentation partially available from the Nouveau project.

### AMD/ATI Driver

The AMD driver (`amd_ati_driver_v2.c`) supports families from the original Radeon (R100) through Navi (RX 5000/6000 series). Like the NVIDIA driver, it primarily uses the BIOS/UEFI-configured framebuffer. Full support would require Atombios command table parsing and complex register programming.

### VGA Text Driver

The VGA text driver (`vga_text_driver_v2.c`) is the universal fallback. It operates on the classic VGA text buffer at `0xB8000`, supporting 80x25 text mode with hardware cursor and 16-color attributes. Every other driver can fail and the system will still have a working display.

### Driver Blacklisting and Runtime Swap

The V2 system supports **runtime driver swapping** via `gfx_swap_driver()`. If the primary driver fails or encounters corruption, the system can hot-swap to a different driver. There's also a blacklist mechanism -- for example, VMware SVGA is automatically blacklisted in QEMU environments where it's known to cause issues.

---

## 3. VGA Text Mode

VGA text mode is the bedrock fallback. The text driver writes characters directly to VGA text memory -- each character cell is two bytes: the ASCII character and a color attribute byte.

The `graphics_manager.c` provides a high-level API for text rendering in framebuffer mode too, using the 8x8 bitmap font:

```c
graphics_result_t graphics_write_char(int32_t x, int32_t y, char c, uint8_t attr);
graphics_result_t graphics_write_string(int32_t x, int32_t y, const char* str, uint8_t attr);
graphics_result_t graphics_printf(int32_t x, int32_t y, uint8_t attr, const char* format, ...);
```

Text attributes follow the standard VGA convention: foreground color in bits 0-3, background color in bits 4-6 (with bit 7 reserved for blink). The standard 16-color palette is hardcoded:

```c
static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};
```

---

## 4. Font Rendering

Forest OS supports two font systems:

### 8x8 Bitmap Fonts

The built-in font (`font8x8.c`) is a public-domain 8x8 monospace bitmap font covering the full CP437 character set. Each character is an 8-byte array where each byte represents one row, and each bit represents one pixel. It's fast, deterministic, and zero-allocation -- perfect for kernel panic displays and early boot.

The font renderer (`font_renderer.c`) also includes an **8x16 VGA BIOS font** with the complete CP437 glyph set, including box-drawing characters and special symbols. The renderer initializes built-in glyph arrays for both 8x8 and 8x16 sizes.

### TrueType Support

Forest OS has a proper TrueType/OpenType font parser (`truetype.c`) and rasterizer (`truetype_raster.c`). The parser handles:

- Big-endian TrueType table parsing (head, cmap, glyf, loca, etc.)
- FUnit-to-pixel scaling
- Quadratic Bezier curve flattening for glyph outlines
- Scan-line conversion with 2x2 supersampled antialiasing

The rasterizer uses an edge pool allocator for efficient memory management during glyph rendering. This enables proportional fonts and proper typography in the window manager and applications.

---

## 5. Splash Screen System

The splash screen (`splash.c`) is an XP-style animated boot screen that runs at 30 FPS. It's one of the first things users see when Forest OS boots.

The splash uses a **dedicated animation thread** that continuously sweeps a marquee highlight across the progress bar. The main boot thread calls `splash_set_progress()` and `splash_update_status()` to update state, while the animation thread handles all rendering independently.

Key features:
- **Early-boot buffer**: Before the render layer system is available, the splash draws directly to a pre-allocated buffer. Once the compositor is ready, `splash_migrate_to_layer()` moves the splash into the layer system.
- **XP-style color palette**: Deep blue gradient background, white logo, green progress bar with animated marquee.
- **Fade-out transition**: When boot completes, the splash performs a smooth fade-out before being removed.
- **Thread-safe**: Double-buffered status text and atomic progress updates prevent race conditions between the boot thread and animation thread.

---

## 6. Panic UI

When the kernel panics, the **PanicUI** (`panicui.c` and friends) takes over the display. It's a multi-page crash screen rendered directly to the framebuffer, bypassing the TTY system to avoid buffer conflicts.

The panic screen has five navigable pages:

1. **Overview**: The panic message, file/line info, error code, fault address
2. **Registers**: CPU register dump at the time of the crash
3. **Memory**: Memory map and allocation state
4. **Stack**: Stack trace and call history
5. **System**: System state (uptime, task info, etc.)

The UI uses the 8x8 bitmap font for text rendering and supports keyboard navigation between pages. The color scheme is a blue background with white/yellow/cyan text -- visually distinct from the normal desktop to make it clear something went wrong.

The panic subsystem is split across multiple files:
- `panicui.c`: Main UI logic and page rendering
- `panicui_colors.c`: Color palette definitions
- `panicui_effects.c`: Visual effects (if any)
- `panicui_gfx.c`: Low-level graphics helpers
- `panicui_input.c`: Keyboard input handling
- `panicui_wm.c`: Minimal window management for the panic display

---

## 7. Display Management

The display manager (`display_manager.c`) handles runtime display configuration:

- **Resolution changes**: `graphics_set_mode()` delegates to the active driver to switch resolutions
- **Color depth**: Supports 8-bit indexed, 16-bit RGB565, 24-bit RGB888, and 32-bit BGRA/RGBA
- **Mode enumeration**: `graphics_enumerate_modes()` queries the driver for available modes. The Bochs BGA driver provides a real mode list; VESA reports only the current (fixed) mode.
- **Client management**: The display manager tracks multiple "clients" (TTY, GUI, panic) and handles transitions between them with alpha-blended fade effects
- **Off-screen framebuffers**: Creates backing buffers for inactive display clients
- **Dirty-rect compositing**: Only repaints changed regions for efficiency

The `hardware_detect.c` module maintains a database of known GPU PCI device IDs (Intel, NVIDIA, AMD) for automatic driver selection.

---

## 8. The LeafGFX Library

LeafGFX (`libs/leafgfx/`) is the **userspace graphics library**. It provides everything applications need to draw to the framebuffer without touching kernel APIs directly.

### Core Features

- **Framebuffer access**: Maps the kernel framebuffer into userspace via syscalls
- **Drawing primitives**: Pixel, line, rectangle, circle, filled variants, anti-aliased shapes
- **Clipping**: Clip stack with push/pop semantics for nested drawing regions
- **Dirty tracking**: Tracks modified regions for efficient partial updates
- **Back buffering**: Optional double-buffering in userspace for tear-free rendering
- **Color utilities**: ARGB color constants, alpha blending, format conversion

### Image Loading

LeafGFX includes BMP image loading (`leafgfx_bmp.c`) for loading bitmap images from the filesystem.

### Font Rendering

The `leafgfx_font.c` and `leafgfx_ttf.c` modules provide both bitmap and TrueType font rendering in userspace, with `leafgfx_ttf_raster.c` handling glyph rasterization.

### Animation Support

`leafgfx_anim.c` provides animation utilities for smooth UI transitions and effects.

### Modern Effects

`leafgfx_modern.c` and `leafgfx_modern.h` offer GNOME/KDE/macOS-inspired visual effects:

- **Box blur** and **fast Gaussian blur** for frosted glass effects
- **Shadow rendering** with configurable offset, blur radius, spread, and color
- **Gradient fills**: Linear, radial, and angular gradients
- **Rounded rectangles** with anti-aliased corners
- Predefined shadow styles (subtle, small, medium, large, elevated)

The library also defines a comprehensive color system with semantic colors (success, warning, error), surface colors for dark/light themes, and text colors with different opacity levels.

---

## 9. The LeafUI Framework

LeafUI (`libs/leafui/`) is the **widget toolkit** that sits on top of LeafGFX. It provides a modern, glass-themed UI framework for building applications.

### Widget Types

- **Button**: Clickable elements with hover/press states
- **Label**: Text display with font size control
- **Input**: Text input fields with focus management
- **Panel**: Container widgets with background styling
- **Progress**: Progress bars and indicators

### Widget System

Each widget has:
- Position and size (leafui_rect_t)
- Text content
- Visibility and enable states
- Hover and press tracking (for interactive widgets)
- Background, text, and border colors
- Border width and corner radius
- Callback functions for events
- Parent/child hierarchy for layout

### Glass Theme

LeafUI ships with a dark glass theme inspired by modern desktop environments. The color palette includes:
- Dark backgrounds with translucent panels
- White text with secondary/hint variants
- Input fields with focus highlighting
- Primary/hover/pressed button states
- Semantic colors for success/error states

---

## 10. Wayland Compositor Support

Forest OS includes an **in-kernel Wayland compositor** (`wayland_compositor.c`, `wayland_server.c`, `wayland_protocol.c`, etc.). This is a significant undertaking -- a display server running inside the kernel.

### Architecture

The Wayland server uses an in-kernel IPC model:

```
userspace client
    |
    v
x11_client_write() ──► server-side recv ring ──► request parser
x11_client_read()   ◄── server-side send ring ◄── reply builder
```

Each client owns two IPC rings (rx for requests, rx for replies). The kernel task processes pending data by calling `wayland_server_pump()` periodically.

### Protocol Support

The server implements several Wayland protocol objects:
- **wl_compositor**: Surface creation
- **wl_shell**: Shell surface management
- **xdg_shell**: XDG window management (xdg_surface, xdg_toplevel)
- **wl_seat**: Input device (pointer, keyboard, touch)
- **zwp_linux_dmabuf**: DMA-BUF buffer sharing for zero-copy rendering

### Global Registry

Clients discover available services through the Wayland global registry:
- `WAYLAND_GLOBAL_NAME_COMPOSITOR` (1)
- `WAYLAND_GLOBAL_NAME_SHELL` (2)
- `WAYLAND_GLOBAL_NAME_XDG_WM_BASE` (3)
- `WAYLAND_GLOBAL_NAME_SEAT` (4)
- `WAYLAND_GLOBAL_NAME_DMABUF` (5)

### Rendering

Drawing requests (PolyFillRectangle, PutImage, ImageText8) operate on the window's surface via the graphics_manager and window_manager APIs. After any draw, the window is marked dirty and `compositor_update()` is called.

---

## 11. X11 Server

Forest OS also includes a minimal **X11R6-compatible server** (`x11_server.c`). This is an in-kernel implementation that lets Xlib clients like `xterm` and `xclock` connect and render.

### Capabilities

- Up to 16 simultaneous clients
- Up to 64 windows, 64 GCs, 32 pixmaps, 64 atoms
- X11 wire-format request parsing and reply building
- Basic drawing: PolyFillRectangle, PutImage, ImageText8
- Keyboard and pointer input events converted to X11 wire format

### Input Handling

`x11_input_event_callback()` is registered with the input multiplexer, so keyboard and pointer events from device drivers flow into per-client event queues and are converted to X11 wire protocol.

### XDG Integration

The `xdg.c` file provides XDG desktop integration, connecting the X11 server with the window manager for proper window decoration and management.

---

## 12. Window Manager and Display Manager

### Window Manager

The kernel includes a **software window manager** (`window_manager.c`) that provides:

- **Window management**: Create, destroy, focus, move, resize windows
- **Z-ordering**: Up to `WM_MAX_WINDOWS` windows with proper stacking
- **Compositing**: A composition buffer that composites all visible windows
- **Desktop surface**: Background wallpaper support
- **Mouse interaction**: Drag to move, edge-resize, snap previews
- **Dirty-rect tracking**: Only recomposites changed regions
- **Render state machine**: Normal, deferred, fallback, and recovery states

The window manager runs as a kernel task (`wm-render`) at maximum priority. When userspace rendering is available, the kernel render loop is disabled and userspace handles composition.

### Display Manager

The display manager (`display_manager.c`) sits above the window manager and handles:

- **Mode transitions**: Smooth fade effects when switching between display clients (TTY, GUI, etc.)
- **Client lifecycle**: Suspend/resume display clients
- **Overlay processing**: Alpha-blended overlay compositing
- **Dirty region merging**: Efficient partial updates

---

## 13. Clipboard and Drag-and-Drop

### Clipboard

The clipboard (`clipboard.c`) provides a kernel-level clipboard service accessible via syscalls:

```c
long sys_clipboard_set(clipboard_type_t type, const void* user_data, uint32 size);
long sys_clipboard_get(clipboard_type_t type, void* user_data, uint32* user_size);
long sys_clipboard_clear(void);
long sys_clipboard_has(clipboard_type_t type);
```

Supported clipboard types:
- `CLIPBOARD_TYPE_TEXT`: Plain text
- `CLIPBOARD_TYPE_IMAGE`: Image data
- `CLIPBOARD_TYPE_FILE`: File references
- `CLIPBOARD_TYPE_CUSTOM`: Application-defined data

The clipboard stores up to 1 MB of data (`CLIPBOARD_MAX_SIZE`) and tracks the owning process. Access is spinlock-protected for thread safety.

### Drag-and-Drop

The drag-and-drop system (`dragdrop.c`) provides a full DnD framework:

**States**: idle -> dragging -> over_target -> dropped/cancelled

**Actions**: copy, move, link

**Features**:
- Target registration: Windows can register as drop targets with specific data type acceptances
- Hover detection: 8-pixel threshold before a drag is recognized
- Callback system: on_drag_start, on_drag_enter, on_drag_leave, on_drop, on_drag_cancel
- Event handler: Optional real-time event notifications
- IPC interface: `dragdrop_handle_ipc()` for command-line or inter-process DnD control
- Up to 32 simultaneous drop targets

---

## Build Configuration

The entire graphics subsystem is gated by build-time feature flags in `build/features/graphics.mk` and `build/features/opengl.mk`. Key flags:

| Flag | Controls |
|------|----------|
| `ENABLE_GRAPHICS` | Master graphics toggle |
| `ENABLE_VESA` | VESA VBE driver |
| `ENABLE_BOCHS_BGA` | Bochs BGA driver |
| `ENABLE_VMWARE_SVGA` | VMware SVGA driver |
| `ENABLE_INTEL_HD` | Intel HD driver |
| `ENABLE_NVIDIA_GPU` | NVIDIA driver |
| `ENABLE_AMD_GPU` | AMD driver |
| `ENABLE_VGA_TEXT` | VGA text/graphics modes |
| `ENABLE_DOUBLE_BUFFERING` | Framebuffer double-buffering |
| `ENABLE_FONT_RENDERER` | 8x8/8x16 bitmap fonts |
| `ENABLE_TRUETYPE` | TrueType font parsing |
| `ENABLE_SPLASH_SCREEN` | Boot splash screen |
| `ENABLE_PANICUI` | Graphical panic display |
| `ENABLE_DISPLAY_MANAGER` | Display mode management |
| `ENABLE_WAYLAND_SERVER` | Wayland compositor |
| `ENABLE_X11_SERVER` | X11 compatibility server |
| `ENABLE_CLIPBOARD` | Clipboard support |
| `ENABLE_DRAG_DROP` | Drag-and-drop support |
| `ENABLE_GPU_ACCEL` | GPU acceleration |
| `ENABLE_OPENGL` | Software OpenGL renderer |

When `ENABLE_GRAPHICS=no`, all graphics source files are excluded and every entry point compiles to a stub that returns `GRAPHICS_ERROR_NOT_SUPPORTED`. This keeps the kernel small for headless or embedded builds.

---

## Software OpenGL Renderer

Forest OS includes a **software OpenGL 1.1 implementation** (`src/gl/`). The renderer is a complete pipeline:

- **Vertex processing** (`vertex.c`, `api_vertex.c`): Transform vertices through the modelview/projection matrix stack
- **Rasterization** (`rasterizer.c`): Scan-line triangle rasterization with edge equations
- **Fragment processing** (`fragment.c`): Per-pixel shading with configurable fragment shader
- **Texturing** (`texture.c`, `api_texture.c`): 2D texture mapping with min/mag filters
- **Framebuffer** (`framebuffer.c`): Color, depth, and stencil buffer management
- **Lighting** (`lighting.c`): Fixed-function per-vertex lighting
- **Display lists** (`displaylist.c`): Compiled geometry caching
- **State management** (`state.c`): OpenGL state machine (enable/disable capabilities)
- **Matrix math** (`math.c`): 4x4 matrix operations, gluPerspective, gluLookAt

The renderer reports itself as `"Software OpenGL 1.1"` via `glGetString()`. It's gated by `ENABLE_OPENGL` and excluded from builds when not needed.

---

## Summary

Forest OS's graphics subsystem is layered from bottom to top:

1. **Hardware drivers** (VESA, BGA, SVGA, Intel, NVIDIA, AMD, VGA) provide raw framebuffer access
2. **Graphics manager** offers a unified API over the V2 driver system
3. **Double-buffering and dirty-rect tracking** enable efficient updates
4. **Render layers** composite splash, TTY, and GUI into the final display
5. **Font rendering** (8x8 bitmap + TrueType) makes text possible
6. **Window manager** provides windowing and compositing
7. **Wayland/X11 servers** enable userspace display clients
8. **LeafGFX/LeafUI** give userspace apps drawing and widget capabilities
9. **Clipboard and DnD** round out the desktop experience

The whole thing is designed to degrade gracefully: if the fancy GPU driver fails, fall back to VESA; if VESA fails, fall back to VGA text; if all graphics fail, the kernel still boots with serial console output. That defensive approach is what makes Forest OS bootable on everything from QEMU to bare-metal hardware with NVIDIA RTX 4090s.

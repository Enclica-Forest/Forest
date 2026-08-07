# Forest OS Libraries Overview

Welcome to the Forest OS library ecosystem! This page covers everything you
need to know about the reusable libraries that ship with Forest OS — from the
C standard library all the way up to the modern UI toolkit.

## The Big Picture

Forest OS libraries live under `libs/` and are organized by purpose. Some are
written from scratch for Forest OS, others are third-party projects pulled in
as subtrees. Together they form a layered stack:

```
┌─────────────────────────────────────────────────────┐
│                  Userspace Apps                      │
│            (canopydm, canopyde, etc.)                │
├──────────────┬──────────────────────────────────────┤
│   LeafUI     │   QR Code Gen    │   Other Userspace  │
│   (widgets)  │   (qrcodegen)    │   Libraries        │
├──────────────┴──────────────────┴───────────────────┤
│                   LeafGFX                            │
│     (framebuffer, fonts, input, animations)          │
├─────────────────────────────────────────────────────┤
│                   ForestCore                         │
│   (types, MMIO, audio, string, system helpers)       │
├─────────────────────────────────────────────────────┤
│                    Forest libc                        │
│  (POSIX API, syscalls, errno, stdio, pthread stubs)  │
├─────────────────────────────────────────────────────┤
│                     Kernel                           │
│              (syscall interface)                      │
└─────────────────────────────────────────────────────┘
```

## Library Directory Layout

Each library follows a consistent structure to keep the build system simple:

```
libs/
├── README.md              # Top-level overview
├── libc/                  # C standard library
│   ├── include/libc/      # Public headers (stdio.h, stdlib.h, etc.)
│   ├── src/               # Implementation (syscalls.c, string.c, etc.)
│   ├── Makefile.inc       # Build integration
│   └── README.md
├── forestcore/            # Kernel/runtime helpers
│   ├── include/           # Forest-specific types and helpers
│   ├── src/               # Freestanding implementations
│   └── README.md
├── leafgfx/               # Userspace graphics library
│   ├── leafgfx.h          # Main header (framebuffer, drawing, colors)
│   ├── leafgfx_modern.h   # Modern effects (blur, glass, gradients)
│   ├── leafgfx_anim.h     # Animation system (springs, tweens)
│   ├── leafgfx_input.h    # Keyboard and mouse input
│   ├── leafgfx_bmp.h      # BMP image loading
│   ├── leafgfx_font.h     # Font rendering
│   ├── leafgfx_ttf.h      # TrueType font support
│   ├── leafgfx.c          # Core implementation
│   ├── leafgfx_*.c        # Module implementations
│   └── Makefile.inc
├── leafui/                # UI widget framework
│   └── leafui.h           # Widgets, layouts, events, animations
├── uacpi/                 # ACPI implementation (third-party)
│   ├── include/uacpi/     # Public API headers
│   ├── source/            # AML interpreter, table management
│   ├── tests/             # Test suite
│   ├── LICENSE            # MIT license
│   └── README.md
└── qrcodegen/             # QR code generator (third-party)
    ├── qrcodegen.h        # Public API
    ├── qrcodegen.c        # Implementation
    └── (no README — see header comment)
```

## Library Details

### Forest libc — The POSIX Foundation

Forest libc is the consolidated C standard library for the entire Forest OS
ecosystem. It sits between userspace applications and the kernel, handling:

- **Architecture abstraction** — different syscalls per CPU architecture
  (`int 0x80` on x86, `syscall` on x86_64)
- **Error translation** — kernel returns negative errors; libc sets `errno`
- **Data type conversion** — userspace types vs kernel types
- **Buffered I/O** — `printf()` buffers output to reduce syscall overhead

It uses **Linux-compatible system call numbers** for maximum portability. If
you know Linux programming, you already know Forest libc.

**Supported subsystems:** File I/O, memory management (`mmap`, `brk`), process
control (`fork`, `execve`), networking (`socket`, `bind`), signals, time, and
Forest OS extensions (`mmap_fb`, `poweroff`, `read_kbd_event`).

**Build target:** Static library `libforest.a`.

### ForestCore — Low-Level Runtime Helpers

ForestCore packages the kernel runtime pieces that don't belong in the public
libc. Think of it as the "internal utilities" layer:

- **`types.h`** — Forest-specific type definitions
- **`system.h`** — System-level helpers
- **`net.h`** — Network utilities
- **MMIO/I/O port access** — direct hardware interaction
- **Audio** — low-level audio support
- **String utilities** — freestanding string functions

Run `make refresh-libc` to regenerate the exported ForestCore snapshot from the
authoritative sources under `src/`.

### LeafGFX — The Graphics Engine

LeafGFX is the workhorse of Forest OS userspace graphics. It provides:

- **Framebuffer access** — map the framebuffer into userspace, query screen
  dimensions and pixel format
- **Drawing primitives** — pixels, lines, rectangles, circles, rounded
  rectangles, all with optional anti-aliasing
- **Advanced shapes** — capsules, squircles (iOS app icon shape), stadiums,
  rings
- **Color system** — ARGB colors with a full palette of named constants
  (semantic, surface, text, border colors for both dark and light themes)
- **Pixel format conversion** — RGB565, RGB555, BGRA, and more
- **Alpha blending** — src-over compositing with fast integer math
- **Gradients** — linear, radial, angular, mesh, and multi-stop
- **Shadows and glass** — soft shadows, frosted glass panels, glow effects
- **Double buffering** — allocate a back buffer and flip regions
- **Dirty region tracking** — optimize redraws by tracking changed areas
- **Offscreen buffers** — for composited application windows
- **Mouse cursor** — standard and custom cursor rendering
- **Animation helpers** — fixed-point math, easing functions (quadratic,
  cubic, bounce, sine, spring), timing utilities

LeafGFX also has sub-modules:

| Module | Header | Purpose |
|--------|--------|---------|
| Modern | `leafgfx_modern.h` | Blur, glass panels, advanced shadows, gradient types |
| Animation | `leafgfx_anim.h` | Spring physics, tween engine, easing library |
| Input | `leafgfx_input.h` | Keyboard/mouse polling via `/dev/kbd` and `/dev/mouse` |
| BMP | `leafgfx_bmp.h` | BMP image loading |
| Font | `leafgfx_font.h` | Bitmap font rendering |
| TTF | `leafgfx_ttf.h` | TrueType font rasterization |

### LeafUI — The Widget Toolkit

LeafUI builds on LeafGFX to provide a structured UI framework:

- **Widget system** — buttons, labels, input fields, panels, progress bars
- **Layout engine** — vertical, horizontal, and grid layouts with spacing
  and padding
- **Input handling** — mouse and keyboard event dispatch to focused widgets
- **Animation support** — smooth transitions for UI state changes
- **Glass theme** — built-in dark theme with semi-transparent panels and
  blur-ready colors
- **Main loop** — frame control, event processing, and quit detection

LeafUI uses its own type system (`leafui_color_t`, `leafui_rect_t`, etc.)
that is compatible with LeafGFX types.

### uACPI — Hardware Discovery

uACPI is a third-party ACPI implementation pulled in as a subtree. It's a
fast, portable, NT-compatible AML interpreter that's about **3.5x faster
than ACPICA** in synthetic benchmarks. Forest OS uses it for:

- Hardware device enumeration
- Power management (sleep states)
- Interrupt routing
- PCI configuration

It supports both 32-bit and 64-bit platforms and is fully thread-safe.

### QR Code Generator

A lightweight QR code generation library (version 1.8.0) based on the Nayuki
implementation. Used for displaying QR codes in the UI (e.g., network
configuration, pairing).

## How Libraries Are Built

### Kernel vs Userspace

Libraries are compiled differently depending on where they run:

| Context | Libraries | Compiler Flags |
|---------|-----------|----------------|
| Kernel | libc, forestcore, uacpi | `-ffreestanding -nostdinc -nostdlib` |
| Userspace | libc, leafgfx, leafui, qrcodegen | `-ffreestanding -nostdinc -DUSERSPACE_BUILD` |

The kernel links `libforest.a` (libc + forestcore) directly. Userspace apps
link against their needed libraries.

### Build System Integration

Each library provides a `Makefile.inc` that the main Makefile includes.
Here's how it works:

```makefile
# In the main Makefile:
include libs/libc/Makefile.inc
include libs/leafgfx/Makefile.inc

# Use exported variables:
CFLAGS += $(LIBC_INCLUDES) $(LEAFGFX_INCLUDE)

my_app: my_app.o $(LIBC_LIBRARY)
    $(LD) -o $@ $^
```

The libc builds into `libforest.a` as a static archive. LeafGFX objects
are compiled into the userspace object directory and linked directly into
apps that need them.

**Current LeafGFX consumers:** `canopydm`, `canopyde`

### Standalone Builds

You can build libraries independently:

```bash
# Build just the libc
make forestlibc

# Build just the LeafGFX objects
make leafgfx
```

## Library Versions and Licensing

| Library | Version | License | Origin |
|---------|---------|---------|--------|
| Forest libc | — | Forest OS License | First-party |
| ForestCore | — | Forest OS License | First-party |
| LeafGFX | — | Forest OS License | First-party |
| LeafUI | — | Forest OS License | First-party |
| uACPI | — | MIT | [UltraOS/uACPI](https://github.com/UltraOS/uACPI) |
| QR Code Gen | 1.8.0 | MIT | [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) |

## Header Organization

Public headers follow a consistent pattern:

```
libs/<library>/
├── <library>.h              # Main public API
├── <library>_<module>.h     # Sub-modules (modern, anim, input, etc.)
└── include/                 # Third-party libs use this convention
```

When including headers in your app:

```c
// First-party libraries — include directly
#include <leafgfx.h>         // Main graphics
#include <leafgfx_modern.h>  // Modern effects
#include <leafgfx_anim.h>    // Animations
#include <leafgfx_input.h>   // Input handling
#include <leafui.h>           // Widget toolkit
#include <qrcodegen.h>       // QR codes

// libc — standard POSIX headers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// uACPI — namespaced under uacpi/
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
```

## Using Libraries in Userspace Applications

Here's a minimal example that uses LeafGFX + LeafUI:

```c
#include <leafgfx.h>
#include <leafui.h>

int main(void) {
    // Initialize graphics
    gfx_init();

    // Initialize UI toolkit
    leafui_fb_t fb = {
        .addr  = gfx_get_framebuffer()->addr,
        .width = gfx_screen_width(),
        .height = gfx_screen_height(),
    };
    leafui_init(&fb);

    // Create a button
    leafui_widget_t* btn = leafui_widget_create(
        LEAFUI_WIDGET_BUTTON,
        (leafui_rect_t){100, 100, 200, 40}
    );
    leafui_widget_set_text(btn, "Click Me");

    // Main loop
    while (!leafui_should_quit()) {
        leafui_begin_frame();
        leafui_process_events();

        gfx_clear(GFX_COLOR_SURFACE_0);
        leafui_widget_draw(btn);
        leafui_draw_cursor(gfx_get_mouse()->x, gfx_get_mouse()->y,
                          GFX_CURSOR_ARROW);

        leafui_end_frame();
        leafui_present();
    }

    leafui_widget_destroy(btn);
    gfx_shutdown();
    return 0;
}
```

## Common Patterns Across Libraries

**1. Init/Cleanup lifecycle.** Every library follows `*_init()` / `*_cleanup()`
(or `*_shutdown()`). Always call cleanup before exit.

**2. ARGB color format.** Colors are 32-bit ARGB values (`0xAARRGGBB`).
LeafGFX provides extensive named constants for common colors.

**3. 16.16 fixed-point math.** Animation and gradient APIs use fixed-point
arithmetic for efficiency (no floating point in freestanding code).
`GFX_FP_ONE` = 65536 = 1.0.

**4. Anti-aliased drawing.** Advanced shapes have both aliased (`gfx_fill_circle`)
and anti-aliased (`gfx_fill_circle_sdf`) variants. The `_aa` suffix is the
convention.

**5. Callback-based events.** LeafUI widgets use function pointers for event
handling. LeafGFX input uses polling (check state each frame).

**6. Static library linking.** All userspace libraries are linked statically.
No dynamic loading is supported yet.

## Adding New Libraries

When adding a new library to Forest OS:

1. Create `libs/<name>/` with the standard layout (`<name>.h`, `<name>.c`,
   `Makefile.inc`)
2. Follow the naming convention: `<library>_<module>.h` for sub-modules
3. Provide a `Makefile.inc` that exports `<NAME>_INCLUDE` and `<NAME>_OBJECTS`
4. Use `gfx_*` prefix for LeafGFX-compatible libraries
5. Use `-ffreestanding -nostdinc` for freestanding code
6. Document with a `README.md` in the library directory
7. Keep the header self-contained (include `<stdint.h>`, `<stdbool.h>`, etc.)

## Future Directions

Planned or potential library additions:

- **Networking library** — higher-level TCP/UDP abstractions beyond raw sockets
- **Audio library** — built on ForestCore's audio helpers
- **Font management** — TrueType loading, glyph caching, font fallback
- **Image formats** — PNG, JPEG support beyond BMP
- **Compression** — zlib-compatible decompression for archives
- **JSON/Config parser** — for reading system configuration files
- **XML parser** — for compatibility with desktop standards
- **Math library** — optimized trig, logarithm, and matrix operations
- **Crypto library** — hash functions and basic encryption

As the ecosystem grows, the `libs/` directory will remain the central place
for all reusable code. New libraries should follow the same patterns and
build integration described here.

---

*This page covers the Forest OS library ecosystem as of the current source tree.
See individual library READMEs for API-specific details.*

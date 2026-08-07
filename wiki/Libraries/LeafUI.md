# LeafUI - Forest OS Widget Toolkit

LeafUI is the modern UI framework for Forest OS. It sits on top of LeafGFX
and provides a widget-based toolkit for building graphical interfaces — from
boot-time panic screens to full userspace desktop applications. If LeafGFX is
the canvas, LeafUI is the paintbrush that makes building UIs actually fun.

## What Is LeafUI?

At its core, LeafUI is a **header-defined widget library** that gives you
buttons, text inputs, labels, panels, and progress bars out of the box. It
handles the tedious parts — hit testing, hover states, event routing, layout
calculation — so you can focus on what your app actually does.

LeafUI uses a **glass-themed design language** with translucent panels, soft
borders, and smooth anti-aliased rendering. It's not just functional; it looks
good doing it.

### Where It Sits

```
┌──────────────────────────────────┐
│         Your Application         │
├──────────────────────────────────┤
│           LeafUI                 │  ← Widgets, layouts, events
├──────────────────────────────────┤
│           LeafGFX                │  ← Framebuffer, fonts, rendering
├──────────────────────────────────┤
│         ForestCore / libc        │  ← Types, syscalls, helpers
└──────────────────────────────────┘
```

LeafUI calls LeafGFX for all low-level drawing. It never touches the
framebuffer directly — that keeps things clean and portable.

## Widget Types

LeafUI ships with five widget types. Each one is a `leafui_widget_t` with
type-specific behavior handled internally.

| Widget | Purpose | Interactive? |
|--------|---------|:------------:|
| `LEAFUI_WIDGET_BUTTON` | Clickable button with hover/press states | Yes |
| `LEAFUI_WIDGET_LABEL` | Static text display | No |
| `LEAFUI_WIDGET_INPUT` | Text input field with focus handling | Yes |
| `LEAFUI_WIDGET_PANEL` | Container with background styling | No |
| `LEAFUI_WIDGET_PROGRESS` | Progress bar / indicator | No |

Every widget shares a common structure:

```c
struct leafui_widget {
    leafui_widget_type_t type;
    leafui_rect_t rect;            // position + size
    char* text;                    // label or input content
    bool visible, enabled;         // state flags
    bool hovered, pressed;         // interaction state (auto-managed)
    leafui_color_t bg_color;       // background
    leafui_color_t text_color;     // text color
    leafui_color_t border_color;   // border
    uint32_t border_width;
    int32_t border_radius;         // rounded corners
    leafui_widget_callback_t callback;  // event handler
    void* user_data;               // your context pointer
    struct leafui_widget* parent;
    struct leafui_widget* first_child;
    struct leafui_widget* next_sibling;
};
```

The parent/child/sibling pointers form a tree — LeafUI traverses this tree
for layout and drawing.

## Layout System

LeafUI includes a layout engine that automatically positions widgets. Create
a layout, add widgets to it, and LeafUI handles the rest.

### Layout Types

```c
typedef enum {
    LEAFUI_LAYOUT_VERTICAL,    // Stack widgets top-to-bottom
    LEAFUI_LAYOUT_HORIZONTAL,  // Stack widgets left-to-right
    LEAFUI_LAYOUT_GRID         // Grid arrangement
} leafui_layout_type_t;
```

### Creating a Layout

```c
leafui_layout_t* layout = leafui_layout_create(
    LEAFUI_LAYOUT_VERTICAL,
    (leafui_rect_t){50, 50, 400, 300}
);

leafui_layout_add_widget(layout, label_widget);
leafui_layout_add_widget(layout, input_widget);
leafui_layout_add_widget(layout, button_widget);

// Let the layout engine calculate positions
leafui_layout_update(layout);
```

The layout respects `spacing` (gap between widgets) and `padding` (inner
margin). After calling `leafui_layout_update()`, each widget's `rect` is
updated with its computed position.

To draw everything in one shot:

```c
leafui_layout_draw(layout);
```

This traverses the widget tree and draws each visible widget.

## Event Handling

LeafUI uses a **callback-based** event system. You attach a function to a
widget, and LeafUI calls it when the widget is interacted with.

### Event Types

```c
typedef enum {
    LEAFUI_EVENT_MOUSE_MOVE,
    LEAFUI_EVENT_MOUSE_DOWN,
    LEAFUI_EVENT_MOUSE_UP,
    LEAFUI_EVENT_KEY_DOWN,
    LEAFUI_EVENT_KEY_UP,
    LEAFUI_EVENT_CLICK
} leafui_event_type_t;
```

Mouse events carry coordinates and button state. Key events carry key codes
and modifier flags (shift, ctrl, alt).

### Setting Up Callbacks

```c
void on_button_click(leafui_widget_t* widget, void* user_data) {
    int* counter = (int*)user_data;
    (*counter)++;
    printf("Button clicked! Count: %d\n", *counter);
}

int counter = 0;
leafui_widget_set_callback(btn, on_button_click, &counter);
```

The callback receives both the widget pointer and your `user_data`, so you
can access any context you need.

### Processing Events

In your main loop, feed events from LeafGFX into LeafUI:

```c
while (!leafui_should_quit()) {
    leafui_begin_frame();
    leafui_process_events();   // dispatches queued events to widgets
    // ... draw your UI ...
    leafui_end_frame();
    leafui_present();
}
```

LeafUI also tracks focus. Call `leafui_set_focused_widget()` to direct
keyboard input to a specific widget (useful for text inputs):

```c
leafui_set_focused_widget(input_field);
```

## Styling and Theming

LeafUI ships with a **glass theme** — a dark color palette with translucent
surfaces inspired by modern desktop environments. All theme colors are
defined as constants:

```c
#define LEAFUI_COLOR_BG             0xFF1A1A2E   // Dark navy background
#define LEAFUI_COLOR_BG_TOP         0xFF16213E   // Gradient top
#define LEAFUI_COLOR_BG_BOTTOM      0xFF0F3460   // Gradient bottom
#define LEAFUI_COLOR_PANEL          0x40000000   // Translucent panel (25% opacity)
#define LEAFUI_COLOR_PANEL_BORDER   0x60FFFFFF   // Soft white border
#define LEAFUI_COLOR_TEXT           0xFFFFFFFF   // Primary white text
#define LEAFUI_COLOR_TEXT_SECONDARY 0xDDFFFFFF   // Slightly transparent
#define LEAFUI_COLOR_TEXT_HINT      0x99FFFFFF   // Placeholder/hint text
#define LEAFUI_COLOR_INPUT_BG       0x50FFFFFF   // Input field background
#define LEAFUI_COLOR_INPUT_FOCUS    0x80FFFFFF   // Focused input highlight
#define LEAFUI_COLOR_BUTTON_PRIMARY 0xFFFFFFFF   // Button background
#define LEAFUI_COLOR_BUTTON_HOVER   0xFFE0E0E0   // Hover state
#define LEAFUI_COLOR_BUTTON_PRESSED 0xFFC0C0C0   // Pressed state
#define LEAFUI_COLOR_SUCCESS        0xFF62D09B   // Green for success
#define LEAFUI_COLOR_ERROR          0xFFFF6B6B   // Red for errors
```

Colors are 32-bit ARGB values (`0xAARRGGBB`). The alpha channel controls
transparency — notice how panels and borders use partial alpha for that glass
effect.

### Customizing Widget Appearance

Override colors per-widget:

```c
leafui_widget_t* btn = leafui_widget_create(
    LEAFUI_WIDGET_BUTTON,
    (leafui_rect_t){100, 100, 200, 50}
);
btn->bg_color = LEAFUI_COLOR_SUCCESS;    // green button
btn->text_color = LEAFUI_COLOR_BLACK;
btn->border_radius = 12;                  // rounded corners
btn->border_width = 2;
```

### Color Utilities

LeafUI provides helper functions for creating colors:

```c
// From RGBA components
leafui_color_t c = leafui_color(255, 128, 0, 200);

// From RGB (alpha defaults to 255)
leafui_color_t c = leafui_color_rgb(255, 128, 0);

// Convert to raw pixel value
uint32_t pixel = leafui_color_to_pixel(c);
```

## Drawing Primitives

LeafUI provides a full set of drawing primitives for custom rendering:

```c
// Basic shapes
leafui_clear(color);                          // fill entire screen
leafui_pixel(x, y, color);                   // single pixel
leafui_rect(rect, color);                     // outline
leafui_rect_filled(rect, color);              // filled
leafui_circle(cx, cy, radius, color);         // outline
leafui_circle_filled(cx, cy, radius, color);  // filled

// Anti-aliased shapes (smooth edges)
leafui_circle_aa(cx, cy, radius, color);
leafui_rect_rounded_aa(rect, radius, color);
leafui_rect_rounded_filled_aa(rect, radius, color);

// Text
leafui_set_font_size(24);
leafui_text(x, y, "Hello, Forest!", color);
leafui_text_centered(rect, "Centered!", color);
```

The `_aa` variants use anti-aliasing for smooth rendering. The rounded rect
functions take a corner radius parameter — perfect for the glass theme.

## Animation System

LeafUI includes a built-in animation system for smooth transitions:

```c
// Create an animation that fades a value from 0.0 to 1.0 over 0.5 seconds
leafui_animation_t* anim = leafui_animation_create(
    0.5f,    // duration in seconds
    0.0f,    // start value
    1.0f,    // end value
    fade_callback,
    user_data
);

// In your frame loop:
if (leafui_animation_update(anim, current_time)) {
    // Animation completed
    leafui_animation_destroy(anim);
}
```

The `update_callback` is called each frame with the interpolated value,
letting you drive any visual property (opacity, position, size, color).

Call `leafui_draw_all_animations()` to render all active animations at once.

## Integration with LeafGFX

LeafUI is designed to work hand-in-hand with LeafGFX. Here's the typical
integration pattern:

```c
#include <leafgfx.h>
#include <leafui.h>

int main(void) {
    // 1. Initialize LeafGFX (framebuffer, fonts, input)
    gfx_init();

    // 2. Create a LeafUI framebuffer from LeafGFX's buffer
    leafui_fb_t fb = {
        .addr  = gfx_get_framebuffer()->addr,
        .width = gfx_screen_width(),
        .height = gfx_screen_height(),
        .pitch = gfx_get_framebuffer()->pitch,
        .bpp   = gfx_get_framebuffer()->bpp
    };
    leafui_init(&fb);

    // 3. Build your UI with LeafUI widgets
    leafui_widget_t* label = leafui_widget_create(
        LEAFUI_WIDGET_LABEL,
        (leafui_rect_t){100, 50, 300, 30}
    );
    leafui_widget_set_text(label, "Welcome to Forest OS!");

    // 4. Main loop
    while (!leafui_should_quit()) {
        leafui_begin_frame();
        leafui_process_events();

        gfx_clear(GFX_COLOR_SURFACE_0);
        leafui_widget_draw(label);
        leafui_draw_cursor(gfx_get_mouse()->x, gfx_get_mouse()->y,
                          GFX_CURSOR_ARROW);

        leafui_end_frame();
        leafui_present();
    }

    // 5. Cleanup
    leafui_widget_destroy(label);
    gfx_shutdown();
    return 0;
}
```

LeafGFX handles the framebuffer, mouse cursor, and font loading. LeafUI
handles widgets and layout. They share the same buffer — LeafUI draws into
the LeafGFX framebuffer.

## Bootloader Usage

The Forest OS bootloader uses LeafUI for its boot-time UI. Since LeafUI is
header-only and needs no dynamic allocation, it works perfectly in the
early-boot environment where only a framebuffer is available.

Typical bootloader use cases:

- **Boot menu** — selectable options with keyboard navigation
- **Progress display** — loading indicators during OS startup
- **Panic screen** — error display when something goes wrong

The bootloader initializes LeafUI with whatever framebuffer the UEFI/BIOS
provides, creates a few widgets, and enters a simple event loop. No
filesystem, no dynamic libraries — just the header and a framebuffer.

## Userspace Applications

For userspace apps, LeafUI works with LeafGFX's framebuffer abstraction.
The pattern is identical to the bootloader case — init LeafGFX, wrap its
buffer in a `leafui_fb_t`, and you're good to go.

### Building an App

LeafUI is a static library. Link it alongside LeafGFX and libc:

```makefile
# In your app's Makefile
APP_OBJECTS = main.o
APP_LIBS = leafui leafgfx forestcore libc

include ../../libs/leafui/Makefile.inc
include ../../libs/leafgfx/Makefile.inc
```

Include the header:

```c
#include <leafui.h>
```

That's it. No runtime dependencies, no shared libraries, no package manager.
Just compile and run.

## API Quick Reference

### Initialization

| Function | Description |
|----------|-------------|
| `leafui_init(fb)` | Initialize with a framebuffer |
| `leafui_should_quit()` | Check if the app should exit |
| `leafui_begin_frame()` | Start a new frame |
| `leafui_end_frame()` | End the current frame |
| `leafui_present()` | Present the frame to screen |

### Widgets

| Function | Description |
|----------|-------------|
| `leafui_widget_create(type, rect)` | Create a new widget |
| `leafui_widget_destroy(widget)` | Free a widget |
| `leafui_widget_add_child(parent, child)` | Add child to parent |
| `leafui_widget_set_text(widget, text)` | Set widget text |
| `leafui_widget_set_callback(widget, cb, data)` | Attach event handler |
| `leafui_widget_draw(widget)` | Draw a widget |

### Layout

| Function | Description |
|----------|-------------|
| `leafui_layout_create(type, rect)` | Create a layout |
| `leafui_layout_destroy(layout)` | Free a layout |
| `leafui_layout_add_widget(layout, widget)` | Add widget to layout |
| `leafui_layout_update(layout)` | Recalculate positions |
| `leafui_layout_draw(layout)` | Draw all widgets in layout |

### Input

| Function | Description |
|----------|-------------|
| `leafui_handle_mouse_event(event)` | Process a mouse event |
| `leafui_handle_key_event(event)` | Process a keyboard event |
| `leafui_get_focused_widget()` | Get the focused widget |
| `leafui_set_focused_widget(widget)` | Set keyboard focus |

### Drawing

| Function | Description |
|----------|-------------|
| `leafui_clear(color)` | Fill screen with color |
| `leafui_rect(rect, color)` | Draw rectangle outline |
| `leafui_rect_filled(rect, color)` | Draw filled rectangle |
| `leafui_circle_filled(cx, cy, r, color)` | Draw filled circle |
| `leafui_circle_aa(cx, cy, r, color)` | Anti-aliased circle |
| `leafui_rect_rounded_filled_aa(rect, r, color)` | Rounded filled rect |
| `leafui_text(x, y, text, color)` | Draw text at position |
| `leafui_text_centered(rect, text, color)` | Draw centered text |

### Animation

| Function | Description |
|----------|-------------|
| `leafui_animation_create(...)` | Create an animation |
| `leafui_animation_destroy(anim)` | Free an animation |
| `leafui_animation_update(anim, time)` | Update animation state |
| `leafui_draw_all_animations()` | Draw all active animations |

## Tips and Best Practices

1. **Always call `leafui_begin_frame()` and `leafui_end_frame()`** around
   your draw calls. LeafUI batches state changes between these markers.

2. **Use `leafui_layout_*` for positioning** instead of manually setting
   widget rects. It's less error-prone and handles resizing gracefully.

3. **Keep callbacks short.** They run on the UI thread — long operations
   will freeze the interface.

4. **Free widgets when done.** LeafUI allocates memory for widget structs
   and text strings. Call `leafui_widget_destroy()` to clean up.

5. **Use the glass theme colors** as defaults and override per-widget only
   when you need a different look. Consistency is key.

6. **Anti-aliased primitives** (`_aa` suffix) look better but cost more.
   Use them for user-facing UI; skip them for debug overlays.

---

*LeafUI is part of the Forest OS library ecosystem. See
[Libraries Overview](Overview.md) for how it fits into the bigger picture.*

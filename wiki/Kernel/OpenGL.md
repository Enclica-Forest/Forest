# OpenGL Implementation

Forest OS includes a complete **software OpenGL 1.1** renderer built directly into the kernel. No GPU, graphics card, or hardware acceleration needed — everything runs on the CPU in pure C.

> **Source:** `fern/src/gl/` — 69 files implementing the full GL pipeline.

---

## What Is It?

A software rasterizer implementing the OpenGL 1.1 fixed-function pipeline plus extensions. When you call `glBegin()`, `glVertex3f()`, or `glDrawArrays()`, the kernel processes every vertex, rasterizes every triangle, and writes every pixel — all on the CPU.

```c
glGetString(GL_RENDERER);  // "Software OpenGL 1.1"
glGetString(GL_VERSION);   // "1.1 Forest Software"
```

Enable with `ENABLE_OPENGL=yes` in your build config (`fern/build/features/opengl.mk`).

---

## Why Software Rendering?

Forest OS is a from-scratch operating system without GPU drivers. Software rendering provides:

- **Universal compatibility** — works on any hardware with a CPU and framebuffer
- **No driver dependencies** — no GPU-specific code or proprietary blobs
- **Deterministic output** — same code produces the same pixels everywhere
- **Self-contained** — the entire renderer is ~7,000 lines of C

The tradeoff is performance, but for UI compositing, simple 3D, or basic games, it's sufficient.

---

## The Rendering Pipeline

```
Application Code
       v
  [Vertex Transform]  — MVP matrix multiply, perspective divide, viewport
       v
  [Triangle Setup]    — face culling, winding order check
       v
  [Rasterization]     — 4x4 block scanline rasterizer
       v
  [Fragment Shader]   — texture sampling, lighting, fog, alpha test
       v
  [Per-Pixel Tests]   — depth, stencil, scissor, blending
       v
  [Framebuffer Write] — color/depth/stencil buffer output
       v
  [Present to Screen] — format conversion → display
```

---

## Vertex Transform

Vertices transform from object space to screen space via: Model-View Matrix → Projection Matrix → perspective divide → viewport mapping. The math library (`math.h`) provides all matrix operations: multiply, translate, rotate, scale, ortho, perspective, lookAt, and invert.

---

## Vertex Array Support

### Immediate Mode

```c
glBegin(GL_TRIANGLES);
glColor3f(1.0f, 0.0f, 0.0f); glVertex3f(-1.0f, -1.0f, 0.0f);
glColor3f(0.0f, 1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, 0.0f);
glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 0.0f,  1.0f, 0.0f);
glEnd();
```

Vertices buffer up to 4096 per batch, flushed on `glEnd()`.

### Vertex Arrays & VBOs

```c
glEnableClientState(GL_VERTEX_ARRAY);
glVertexPointer(3, GL_FLOAT, 0, myVertices);
glDrawArrays(GL_TRIANGLES, 0, count);

// Or with VBOs:
glGenBuffers(1, &vbo);
glBindBuffer(GL_ARRAY_BUFFER, vbo);
glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW);
```

Supported arrays: `GL_VERTEX_ARRAY`, `GL_COLOR_ARRAY`, `GL_NORMAL_ARRAY`, `GL_TEXTURE_COORD_ARRAY`. Data types: float, byte, short, int (signed/unsigned). Up to 256 buffers and 16 vertex attrib pointers.

### Display Lists

Record and replay command sequences: `glGenLists`, `glNewList`, `glEndList`, `glCallList`. Records vertex, color, normal, texcoord, begin/end, enable/disable, bind texture, and matrix operations.

### Supported Primitives

`GL_POINTS`, `GL_LINES`, `GL_LINE_STRIP`, `GL_LINE_LOOP`, `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`, `GL_TRIANGLE_FAN`

---

## Texture Mapping

Up to **256 textures** and **8 texture units** (multi-texturing).

- **Formats:** RGBA8, RGB8 (auto-converted to RGBA), Luminance8, Alpha
- **Max size:** 4096x4096
- **Filtering:** `GL_NEAREST`, `GL_LINEAR`
- **Wrapping:** `GL_REPEAT`, `GL_CLAMP_TO_EDGE`
- **Mipmaps:** Auto-generated via `glGenerateMipmap()` (2x2 box filter down to 1x1)
- **Combine modes:** `GL_MODULATE`, `GL_REPLACE`, `GL_ADD`, `GL_ADD_SIGNED`, `GL_INTERPOLATE`, `GL_SUBTRACT`, `GL_DOT3_RGB`, `GL_DOT3_RGBA`

---

## The Lighting Model

OpenGL 1.1 fixed-function lighting with up to **8 lights**, computed per-pixel in the fragment shader.

Each light has: **ambient** (constant), **diffuse** (Lambertian N dot L), **specular** (Phong reflection R dot V raised to shininess), and **position** (directional if w=0, positional if w=1).

Material properties: ambient, diffuse, specular, and shininess. The normal matrix (inverse-transpose of the upper 3x3 model-view) is computed on-demand for correct handling of non-uniform scaling.

Per-pixel formula:
```
color = global_ambient * material_ambient
      + sum(light_ambient * material_ambient
          + light_diffuse * material_diffuse * max(N.L, 0)
          + light_specular * material_specular * max(R.V, 0)^shininess)
```

---

## Shader Support

The shader API exists for compatibility but is a **stub**:

```c
GLuint vs = glCreateShader(GL_VERTEX_SHADER);
glShaderSource(vs, 1, &source, NULL);
glCompileShader(vs);  // always reports success
// ... attach, link, use — falls back to fixed-function pipeline
```

Shader source is stored but never compiled or executed. Applications using the shader interface won't crash, but always get the default fixed-function behavior. Pool: 64 shaders, 32 programs, 4KB max source.

---

## The Rasterizer

The performance-critical heart of the renderer, with several optimizations:

1. **Fixed-point arithmetic** — 12.4 format avoids float division in the inner loop
2. **Incremental edge functions** — precompute step values, add per pixel/row
3. **4x4 block rasterization** — cache-line-aligned blocks improve L1 utilization
4. **Early Z testing** — depth test before fragment shader, skipping expensive work on depth-fail
5. **Perspective-correct interpolation** — attributes corrected by clip-space w
6. **Branchless depth test** — ternary chain compiles to `cmov` on x86

Also supports line rasterization (Bresenham) and point rasterization.

---

## The Fragment Shader

The default shader (`fragment.c`) runs per visible pixel:

1. **Texture sampling** — all 8 units, combined via texture environment mode
2. **Lighting** — per-pixel Phong illumination if enabled
3. **Alpha test** — discard fragments based on comparison function
4. **Fog** — blend toward fog color based on distance (linear, exp, or exp2)

The shader is a function pointer (`g_gl_fragment_shader`) that can be replaced.

---

## Per-Pixel Tests and Blending

| Test | Description |
|------|-------------|
| Scissor | Clips to rectangular region |
| Alpha | Discards based on alpha comparison |
| Depth | LESS, LEQUAL, GREATER, GEQUAL, EQUAL, NOTEQUAL, ALWAYS, NEVER |
| Stencil | Compare + write operations (keep, zero, replace, incr, decr, invert, wrap) |
| Blending | 10 factors: zero, one, src/dst color/alpha, one-minus variants |
| Logic ops | 16 bitwise operations: AND, OR, XOR, NAND, NOR, INVERT, etc. |

---

## Buffer Layout

Three buffers in RAM:

| Buffer | Type | Per Pixel | 1920x1080 |
|--------|------|-----------|-----------|
| Color | `unsigned int[]` | 4 bytes (RGBA8888) | ~8 MB |
| Depth | `float[]` | 4 bytes | ~8 MB |
| Stencil | `unsigned char[]` | 1 byte | ~2 MB |

---

## Integration with the Framebuffer

`gl_present()` copies the GL framebuffer to the screen:

1. Gets kernel framebuffer info (width, height, pitch, bpp)
2. Converts RGBA to screen format (32-bit BGGA swap, 24-bit RGB888, or 16-bit RGB565)
3. Nearest-neighbor scaling if resolutions differ
4. Double-buffer present with dirty rect invalidation

`gl_present_region()` handles partial updates. `glBlitFramebuffer()` copies between FBOs.

---

## Framebuffer Objects

Offscreen rendering via FBOs: 64 FBOs, 64 renderbuffers. Color and depth attachments supported. Functions: `glGenFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D`, `glFramebufferRenderbuffer`, `glCheckFramebufferStatus`, `glBlitFramebuffer`.

---

## Performance Characteristics

**What affects speed:** triangle count (O(area) per triangle), resolution (linear scaling), texture filtering, per-pixel lighting, overdraw.

**What's fast:** early Z, 4x4 blocks, branchless depth test, incremental edge functions, fixed-point math.

**Rough numbers** (x86 at 3 GHz):
- 500K–1M triangles/sec
- 10–30 FPS at 1024x768 for modest 3D scenes
- 30+ FPS for UI/2D graphics

Built-in stats tracking (`stats.h`): triangle count, pixel count, depth pass/fail, block skip rate.

---

## Limitations vs Hardware OpenGL

| Feature | Hardware GL | Forest GL |
|---------|-------------|-----------|
| Programmable shaders | Full | Stub only |
| Geometry/tessellation shaders | Yes | No |
| Compute shaders | Yes | No |
| Texture max size | 16K+ | 4096 |
| Texture formats | Dozens | RGBA8, RGB8, Luminance8 |
| Anisotropic filtering | Yes | No |
| MSAA | Yes | No |
| Instanced rendering | Yes | No |
| Transform feedback | Yes | No |
| Multiple render targets | Yes | Single |
| Performance | Millions/tri frame | Thousands–low millions/sec |

---

## Quick Start

A minimal example that draws a colored triangle:

```c
#include "gl.h"

void render_frame(void) {
    gl_init_with_framebuffer();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 800, 600, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 0.0f, 0.0f); glVertex2f(100, 100);
        glColor3f(0.0f, 1.0f, 0.0f); glVertex2f(400, 500);
        glColor3f(0.0f, 0.0f, 1.0f); glVertex2f(700, 100);
    glEnd();

    gl_present();
}
```

Call `gl_init_with_framebuffer()` once at startup. It queries the kernel framebuffer dimensions, allocates the color/depth/stencil buffers, and sets up an orthographic projection matching the screen. Then draw your scene and call `gl_present()` to blit to the display.

---

## Source Files

| File | Purpose |
|------|---------|
| `gl.c` / `gl.h` | Top-level API, glGetString |
| `init.c` | Initialization, framebuffer setup |
| `rasterizer.c/.h` | Triangle/line/point rasterization |
| `vertex.c/.h` | Vertex transform, fetching, interpolation |
| `fragment.c/.h` | Fragment shader, texture sampling, lighting |
| `texture.c/.h` | Texture objects, mipmapping |
| `lighting.c/.h` | Normal matrix, Phong lighting |
| `math.c/.h` | Matrix/vector math |
| `state.c/.h` | GL state machine, type definitions |
| `framebuffer.c/.h` | FBO/RBO management |
| `present.c/.h` | GL → screen framebuffer conversion |
| `buffer.c/.h` | VBO management, vertex attrib pointers |
| `displaylist.c/.h` | Display list recording/playback |
| `stats.c/.h` | Performance counters |
| `api_*.c/.h` | OpenGL function implementations |

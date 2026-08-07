/**
 * Fern Graphics Driver Interface V2
 * 
 * This is a complete rewrite of the graphics driver interface based on
 * official hardware documentation from:
 * - Bochs VBE Extensions specification
 * - VMware SVGA-II device specification  
 * - Intel Graphics Programming Reference Manuals
 * - AMD Atombios documentation
 * - NVIDIA open-source documentation
 * - VESA VBE 3.0 specification
 * - VGA Hardware documentation
 * 
 * This interface provides a unified abstraction for all graphics hardware
 * while allowing driver-specific extensions via capabilities and ioctls.
 */

#ifndef GRAPHICS_DRIVER_V2_H
#define GRAPHICS_DRIVER_V2_H

#include "../stdint.h"
#include <stddef.h>
#include <stdbool.h>

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

typedef struct gfx_device gfx_device_t;
typedef struct gfx_driver gfx_driver_t;
typedef struct gfx_framebuffer gfx_framebuffer_t;
typedef struct gfx_mode gfx_mode_t;
typedef struct gfx_edid gfx_edid_t;

/* ============================================================================
 * Result Codes
 * ============================================================================ */

typedef enum {
    GFX_OK = 0,
    GFX_ERR_INVALID_PARAM,
    GFX_ERR_NO_MEMORY,
    GFX_ERR_NOT_SUPPORTED,
    GFX_ERR_HARDWARE,
    GFX_ERR_MODE_NOT_FOUND,
    GFX_ERR_DEVICE_BUSY,
    GFX_ERR_TIMEOUT,
    GFX_ERR_NO_DRIVER,
    GFX_ERR_INIT_FAILED,
    GFX_ERR_MAPPING_FAILED,
} gfx_result_t;

/* ============================================================================
 * Device Types and Capabilities
 * ============================================================================ */

typedef enum {
    GFX_DEVICE_UNKNOWN = 0,
    GFX_DEVICE_VGA,              /* Standard VGA */
    GFX_DEVICE_VESA,             /* VESA BIOS Extensions */
    GFX_DEVICE_BOCHS_BGA,        /* Bochs/QEMU BGA */
    GFX_DEVICE_VMWARE_SVGA,      /* VMware SVGA-II */
    GFX_DEVICE_VIRTUALBOX,       /* VirtualBox Graphics */
    GFX_DEVICE_INTEL_HD,         /* Intel integrated graphics */
    GFX_DEVICE_AMD_ATI,          /* AMD/ATI graphics */
    GFX_DEVICE_NVIDIA,           /* NVIDIA graphics */
    GFX_DEVICE_SOFTWARE_FB,      /* Software framebuffer fallback */
} gfx_device_type_t;

/* Capability flags */
#define GFX_CAP_LINEAR_FB       (1 << 0)   /* Linear framebuffer support */
#define GFX_CAP_BANKED_FB       (1 << 1)   /* Banked framebuffer support */
#define GFX_CAP_HW_CURSOR       (1 << 2)   /* Hardware cursor support */
#define GFX_CAP_HW_FILL         (1 << 3)   /* Hardware rectangle fill */
#define GFX_CAP_HW_COPY         (1 << 4)   /* Hardware rectangle copy/blit */
#define GFX_CAP_VSYNC           (1 << 5)   /* VSync support */
#define GFX_CAP_PAGE_FLIP       (1 << 6)   /* Hardware page flipping */
#define GFX_CAP_EDID            (1 << 7)   /* EDID reading support */
#define GFX_CAP_DPMS            (1 << 8)   /* Display power management */
#define GFX_CAP_MULTI_HEAD      (1 << 9)   /* Multiple display heads */
#define GFX_CAP_3D              (1 << 10)  /* 3D acceleration */
#define GFX_CAP_SHADERS         (1 << 11)  /* Shader support */
#define GFX_CAP_TEXTURE_2D      (1 << 12)  /* 2D texture support */
#define GFX_CAP_TEXTURE_3D      (1 << 13)  /* 3D texture support */
#define GFX_CAP_VERTEX_BUFFERS  (1 << 14)  /* Vertex buffer objects */
#define GFX_CAP_INDEX_BUFFERS   (1 << 15)  /* Index buffer objects */
#define GFX_CAP_BLENDING        (1 << 16)  /* Blending support */
#define GFX_CAP_DEPTH_TEST      (1 << 17)  /* Depth testing */
#define GFX_CAP_STENCIL_TEST    (1 << 18)  /* Stencil testing */
#define GFX_CAP_SCISSOR_TEST    (1 << 19)  /* Scissor testing */
#define GFX_CAP_ALPHA_TEST      (1 << 20)  /* Alpha testing */
#define GFX_CAP_FOG             (1 << 21)  /* Fog support */
#define GFX_CAP_LIGHTING        (1 << 22)  /* Lighting support */
#define GFX_CAP_TEXTURE_ENV     (1 << 23)  /* Texture environment */
#define GFX_CAP_MULTI_TEXTURE   (1 << 24)  /* Multiple textures */
#define GFX_CAP_ANTIALIAS       (1 << 25)  /* Anti-aliasing */
#define GFX_CAP_STENCIL_BUFFER  (1 << 26)  /* Stencil buffer support */
#define GFX_CAP_ACCUM_BUFFER    (1 << 27)  /* Accumulation buffer */
#define GFX_CAP_STEREO          (1 << 28)  /* Stereo rendering */
#define GFX_CAP_MULTISAMPLE     (1 << 29)  /* Multisample anti-aliasing */
#define GFX_CAP_DITHER          (1 << 30)  /* Dithering */

/* ============================================================================
 * Pixel Formats
 * ============================================================================ */

typedef enum {
    GFX_FORMAT_UNKNOWN = 0,
    GFX_FORMAT_TEXT,            /* VGA text mode (char + attr) */
    GFX_FORMAT_INDEXED_4,       /* 4-bit indexed (16 colors) */
    GFX_FORMAT_INDEXED_8,       /* 8-bit indexed (256 colors) */
    GFX_FORMAT_RGB555,          /* 15-bit RGB (5-5-5) */
    GFX_FORMAT_RGB565,          /* 16-bit RGB (5-6-5) */
    GFX_FORMAT_RGB888,          /* 24-bit RGB (8-8-8) */
    GFX_FORMAT_RGBX8888,        /* 32-bit RGB (8-8-8-x) */
    GFX_FORMAT_RGBA8888,        /* 32-bit RGBA (8-8-8-8) */
    GFX_FORMAT_BGR555,          /* 15-bit BGR */
    GFX_FORMAT_BGR565,          /* 16-bit BGR */
    GFX_FORMAT_BGR888,          /* 24-bit BGR */
    GFX_FORMAT_BGRX8888,        /* 32-bit BGR (x-8-8-8) */
    GFX_FORMAT_BGRA8888,        /* 32-bit BGRA */
} gfx_pixel_format_t;

/* Get bytes per pixel for a format */
static inline uint32_t gfx_format_bpp(gfx_pixel_format_t fmt) {
    switch (fmt) {
        case GFX_FORMAT_TEXT:
        case GFX_FORMAT_INDEXED_4:
            return 1;  /* 4-bit uses byte granularity in planar mode */
        case GFX_FORMAT_INDEXED_8:
            return 1;
        case GFX_FORMAT_RGB555:
        case GFX_FORMAT_BGR555:
        case GFX_FORMAT_RGB565:
        case GFX_FORMAT_BGR565:
            return 2;
        case GFX_FORMAT_RGB888:
        case GFX_FORMAT_BGR888:
            return 3;
        case GFX_FORMAT_RGBX8888:
        case GFX_FORMAT_RGBA8888:
        case GFX_FORMAT_BGRX8888:
        case GFX_FORMAT_BGRA8888:
            return 4;
        default:
            return 0;
    }
}

/* ============================================================================
 * Video Mode Structure
 * ============================================================================ */

struct gfx_mode {
    uint32_t mode_id;           /* Hardware-specific mode number */
    uint32_t width;             /* Horizontal resolution */
    uint32_t height;            /* Vertical resolution */
    uint32_t bpp;               /* Bits per pixel */
    uint32_t pitch;             /* Bytes per scanline (may be > width * bpp/8) */
    gfx_pixel_format_t format;  /* Pixel format */
    uint32_t refresh_hz;        /* Refresh rate in Hz */
    
    /* Timing information (for mode setting) */
    uint32_t htotal;            /* Total horizontal pixels including blanking */
    uint32_t vtotal;            /* Total vertical lines including blanking */
    uint32_t hblank_start;      /* Horizontal blank start */
    uint32_t hblank_end;        /* Horizontal blank end */
    uint32_t hsync_start;       /* Horizontal sync start */
    uint32_t hsync_end;         /* Horizontal sync end */
    uint32_t vblank_start;      /* Vertical blank start */
    uint32_t vblank_end;        /* Vertical blank end */
    uint32_t vsync_start;       /* Vertical sync start */
    uint32_t vsync_end;         /* Vertical sync end */
    uint32_t pixel_clock;       /* Pixel clock in kHz */
    
    /* Flags */
    bool is_text_mode;          /* True for text modes */
    bool interlaced;            /* Interlaced mode */
    bool doublescan;            /* Double-scan mode */
    bool hsync_positive;        /* Horizontal sync polarity */
    bool vsync_positive;        /* Vertical sync polarity */
};

/* ============================================================================
 * Framebuffer Structure
 * ============================================================================ */

struct gfx_framebuffer {
    uintptr_t phys_addr;        /* Physical address of framebuffer */
    void* virt_addr;            /* Virtual address (after mapping) */
    size_t size;                /* Total size in bytes */
    
    uint32_t width;             /* Visible width in pixels */
    uint32_t height;            /* Visible height in pixels */
    uint32_t pitch;             /* Bytes per scanline */
    uint32_t bpp;               /* Bits per pixel */
    gfx_pixel_format_t format;  /* Pixel format */
    
    /* Virtual framebuffer support (for hardware scrolling) */
    uint32_t virtual_width;     /* Virtual width (may be > width) */
    uint32_t virtual_height;    /* Virtual height (may be > height) */
    int32_t x_offset;           /* Display X offset in virtual fb */
    int32_t y_offset;           /* Display Y offset in virtual fb */
    
    /* Double buffering */
    void* back_buffer;          /* Software back buffer (if enabled) */
    bool double_buffered;       /* Double buffering active */
    
    /* Hardware-specific */
    void* driver_data;          /* Driver-private data */
};

/* ============================================================================
 * EDID Structure (simplified)
 * ============================================================================ */

struct gfx_edid {
    uint8_t raw[128];           /* Raw EDID block */
    bool valid;                 /* EDID is valid */
    
    /* Parsed fields */
    char manufacturer[4];       /* 3-letter manufacturer code */
    uint16_t product_code;      /* Product code */
    uint32_t serial;            /* Serial number */
    uint8_t week;               /* Week of manufacture */
    uint8_t year;               /* Year of manufacture (+ 1990) */
    
    /* Display capabilities */
    uint32_t max_width;         /* Maximum horizontal resolution */
    uint32_t max_height;        /* Maximum vertical resolution */
    uint32_t preferred_width;   /* Preferred width */
    uint32_t preferred_height;  /* Preferred height */
    
    /* Monitor descriptors */
    char monitor_name[14];      /* Monitor name string */
};

/* ============================================================================
 * Color and Drawing Types
 * ============================================================================ */

typedef struct {
    uint8_t r, g, b, a;
} gfx_color_t;

typedef struct {
    int32_t x, y;
    uint32_t width, height;
} gfx_rect_t;

typedef struct {
    int32_t x, y;
} gfx_point_t;

/* Common colors */
#define GFX_COLOR_BLACK     ((gfx_color_t){0, 0, 0, 255})
#define GFX_COLOR_WHITE     ((gfx_color_t){255, 255, 255, 255})
#define GFX_COLOR_RED       ((gfx_color_t){255, 0, 0, 255})
#define GFX_COLOR_GREEN     ((gfx_color_t){0, 255, 0, 255})
#define GFX_COLOR_BLUE      ((gfx_color_t){0, 0, 255, 255})

/* Convert gfx_color_t to raw pixel value for a given format */
static inline uint32_t gfx_color_to_pixel(gfx_color_t c, gfx_pixel_format_t fmt) {
    switch (fmt) {
        case GFX_FORMAT_RGB555:
            return ((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3);
        case GFX_FORMAT_BGR555:
            return ((c.b >> 3) << 10) | ((c.g >> 3) << 5) | (c.r >> 3);
        case GFX_FORMAT_RGB565:
            return ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3);
        case GFX_FORMAT_BGR565:
            return ((c.b >> 3) << 11) | ((c.g >> 2) << 5) | (c.r >> 3);
        case GFX_FORMAT_RGB888:
        case GFX_FORMAT_RGBX8888:
            return (c.r << 16) | (c.g << 8) | c.b;
        case GFX_FORMAT_BGR888:
        case GFX_FORMAT_BGRX8888:
            return (c.b << 16) | (c.g << 8) | c.r;
        case GFX_FORMAT_RGBA8888:
            return (c.a << 24) | (c.r << 16) | (c.g << 8) | c.b;
        case GFX_FORMAT_BGRA8888:
            return (c.a << 24) | (c.b << 16) | (c.g << 8) | c.r;
        case GFX_FORMAT_INDEXED_8:
            /* Simple grayscale approximation */
            return (c.r * 77 + c.g * 150 + c.b * 29) >> 8;
        default:
            return 0;
    }
}

/* ============================================================================
 * Hardware Cursor Structure
 * ============================================================================ */

typedef struct {
    uint32_t width;             /* Cursor width (typically 32 or 64) */
    uint32_t height;            /* Cursor height */
    int32_t hotspot_x;          /* Hotspot X offset */
    int32_t hotspot_y;          /* Hotspot Y offset */
    uint32_t* pixels;           /* ARGB pixel data */
} gfx_cursor_t;

/* ============================================================================
 * Device Structure
 * ============================================================================ */

struct gfx_device {
    /* Device identification */
    gfx_device_type_t type;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    char name[64];
    
    /* PCI location */
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    
    /* Hardware resources */
    uintptr_t io_base;          /* I/O port base (if applicable) */
    uintptr_t mmio_base;        /* MMIO register base */
    size_t mmio_size;           /* MMIO region size */
    uintptr_t fb_base;          /* Framebuffer base address */
    size_t fb_size;             /* Framebuffer size */
    size_t vram_size;           /* Total video RAM */
    
    /* Capabilities */
    uint32_t caps;              /* GFX_CAP_* flags */
    uint32_t max_width;         /* Maximum supported width */
    uint32_t max_height;        /* Maximum supported height */
    uint32_t max_bpp;           /* Maximum bits per pixel */
    
    /* Current state */
    gfx_mode_t current_mode;    /* Currently active mode */
    gfx_framebuffer_t* fb;      /* Current framebuffer */
    bool active;                /* Device is active */
    
    /* Driver */
    gfx_driver_t* driver;       /* Associated driver */
    void* driver_data;          /* Driver-private data */
};

/* ============================================================================
 * Driver Operations Structure
 * ============================================================================ */

typedef struct gfx_driver_ops {
    /* Driver identification */
    const char* name;
    uint32_t version;
    
    /* Probe and lifecycle */
    gfx_result_t (*probe)(gfx_device_t* dev);
    gfx_result_t (*init)(gfx_device_t* dev);
    gfx_result_t (*shutdown)(gfx_device_t* dev);
    gfx_result_t (*reset)(gfx_device_t* dev);
    
    /* Mode management */
    gfx_result_t (*get_modes)(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
    gfx_result_t (*set_mode)(gfx_device_t* dev, const gfx_mode_t* mode);
    gfx_result_t (*get_mode)(gfx_device_t* dev, gfx_mode_t* mode);
    
    /* Framebuffer */
    gfx_result_t (*map_fb)(gfx_device_t* dev, gfx_framebuffer_t** fb);
    gfx_result_t (*unmap_fb)(gfx_device_t* dev, gfx_framebuffer_t* fb);
    gfx_result_t (*set_fb_offset)(gfx_device_t* dev, int32_t x, int32_t y);
    
    /* Drawing (software fallbacks if hardware not available) */
    gfx_result_t (*clear)(gfx_device_t* dev, gfx_color_t color);
    gfx_result_t (*draw_pixel)(gfx_device_t* dev, int32_t x, int32_t y, gfx_color_t c);
    gfx_result_t (*draw_rect)(gfx_device_t* dev, const gfx_rect_t* r, gfx_color_t c, bool filled);
    gfx_result_t (*blit)(gfx_device_t* dev, const gfx_rect_t* src, int32_t dx, int32_t dy);
    
    /* Hardware cursor */
    gfx_result_t (*set_cursor)(gfx_device_t* dev, const gfx_cursor_t* cursor);
    gfx_result_t (*move_cursor)(gfx_device_t* dev, int32_t x, int32_t y);
    gfx_result_t (*show_cursor)(gfx_device_t* dev, bool show);
    
    /* Text mode (for VGA-compatible drivers) */
    gfx_result_t (*write_char)(gfx_device_t* dev, int32_t x, int32_t y, char c, uint8_t attr);
    gfx_result_t (*set_text_cursor)(gfx_device_t* dev, int32_t x, int32_t y);
    gfx_result_t (*scroll)(gfx_device_t* dev, int32_t lines);
    
    /* Synchronization */
    gfx_result_t (*wait_vsync)(gfx_device_t* dev);
    gfx_result_t (*flip)(gfx_device_t* dev);
    gfx_result_t (*flush)(gfx_device_t* dev);
    
    /* Display detection */
    gfx_result_t (*read_edid)(gfx_device_t* dev, gfx_edid_t* edid);
    gfx_result_t (*detect_displays)(gfx_device_t* dev, uint32_t* count);
    
    /* Power management */
    gfx_result_t (*set_dpms)(gfx_device_t* dev, uint32_t state);
    
    /* 3D Acceleration */
    gfx_result_t (*create_context)(gfx_device_t* dev, void** context);
    gfx_result_t (*destroy_context)(gfx_device_t* dev, void* context);
    gfx_result_t (*make_current)(gfx_device_t* dev, void* context);
    gfx_result_t (*swap_buffers)(gfx_device_t* dev);
    
    /* Shader management */
    gfx_result_t (*create_shader)(gfx_device_t* dev, uint32_t type, const char* source, void** shader);
    gfx_result_t (*destroy_shader)(gfx_device_t* dev, void* shader);
    gfx_result_t (*create_program)(gfx_device_t* dev, void** program);
    gfx_result_t (*attach_shader)(gfx_device_t* dev, void* program, void* shader);
    gfx_result_t (*link_program)(gfx_device_t* dev, void* program);
    gfx_result_t (*use_program)(gfx_device_t* dev, void* program);
    
    /* Vertex buffer management */
    gfx_result_t (*create_buffer)(gfx_device_t* dev, uint32_t target, size_t size, const void* data, uint32_t usage, void** buffer);
    gfx_result_t (*bind_buffer)(gfx_device_t* dev, uint32_t target, void* buffer);
    gfx_result_t (*buffer_data)(gfx_device_t* dev, uint32_t target, size_t size, const void* data);
    gfx_result_t (*buffer_sub_data)(gfx_device_t* dev, uint32_t target, size_t offset, size_t size, const void* data);
    gfx_result_t (*destroy_buffer)(gfx_device_t* dev, void* buffer);
    
    /* Vertex attributes */
    gfx_result_t (*enable_vertex_attrib_array)(gfx_device_t* dev, uint32_t index);
    gfx_result_t (*disable_vertex_attrib_array)(gfx_device_t* dev, uint32_t index);
    gfx_result_t (*vertex_attrib_pointer)(gfx_device_t* dev, uint32_t index, int32_t size, uint32_t type, bool normalized, int32_t stride, const void* pointer);
    
    /* Uniforms */
    gfx_result_t (*get_uniform_location)(gfx_device_t* dev, void* program, const char* name, int32_t* location);
    gfx_result_t (*uniform1f)(gfx_device_t* dev, int32_t location, double value);
    gfx_result_t (*uniform1i)(gfx_device_t* dev, int32_t location, int32_t value);
    gfx_result_t (*uniform2f)(gfx_device_t* dev, int32_t location, double x, double y);
    gfx_result_t (*uniform3f)(gfx_device_t* dev, int32_t location, double x, double y, double z);
    gfx_result_t (*uniform4f)(gfx_device_t* dev, int32_t location, double x, double y, double z, double w);
    gfx_result_t (*uniform_matrix4fv)(gfx_device_t* dev, int32_t location, bool transpose, const double* value);
    
    /* Drawing */
    gfx_result_t (*draw_arrays)(gfx_device_t* dev, uint32_t mode, int32_t first, int32_t count);
    gfx_result_t (*draw_elements)(gfx_device_t* dev, uint32_t mode, int32_t count, uint32_t type, const void* indices);
    
    /* Driver-specific extensions */
    gfx_result_t (*ioctl)(gfx_device_t* dev, uint32_t cmd, void* arg);
} gfx_driver_ops_t;

/* ============================================================================
 * Driver Structure
 * ============================================================================ */

struct gfx_driver {
    const gfx_driver_ops_t* ops;
    gfx_device_type_t supported_types;   /* Bitmask of supported device types */
    uint32_t flags;                       /* Driver flags */
    uint32_t priority;                    /* Higher = preferred (0-255) */
    void* private_data;                   /* Driver global data */
    gfx_driver_t* next;                  /* Linked list of drivers */
};

/* Driver flags */
#define GFX_DRV_FLAG_BUILTIN     (1 << 0)  /* Built-in driver */
#define GFX_DRV_FLAG_PREFERRED   (1 << 1)  /* Preferred for matching devices */

/* ============================================================================
 * Driver Registration API
 * ============================================================================ */

gfx_result_t gfx_register_driver(gfx_driver_t* drv);
gfx_result_t gfx_unregister_driver(gfx_driver_t* drv);
gfx_driver_t* gfx_find_driver(gfx_device_t* dev);

/* ============================================================================
 * Device Management API
 * ============================================================================ */

gfx_result_t gfx_probe_devices(void);
gfx_result_t gfx_get_device(uint32_t index, gfx_device_t** dev);
gfx_result_t gfx_get_primary_device(gfx_device_t** dev);
uint32_t gfx_get_device_count(void);

/* ============================================================================
 * High-Level Graphics API
 * ============================================================================ */

gfx_result_t gfx_init(void);
gfx_result_t gfx_shutdown(void);

gfx_result_t gfx_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
gfx_result_t gfx_get_framebuffer(gfx_framebuffer_t** fb);

gfx_result_t gfx_clear_screen(gfx_color_t color);
gfx_result_t gfx_draw_pixel(int32_t x, int32_t y, gfx_color_t color);
gfx_result_t gfx_draw_rect(const gfx_rect_t* rect, gfx_color_t color, bool filled);
gfx_result_t gfx_swap_buffers(void);
gfx_result_t gfx_flush_framebuffer(void);

/* Driver management */
gfx_result_t gfx_swap_driver(gfx_device_type_t type);
bool gfx_is_driver_blacklisted(gfx_device_type_t type);
void gfx_blacklist_driver(gfx_device_type_t type);
void gfx_unblacklist_driver(gfx_device_type_t type);

/* 3D Acceleration API */
gfx_result_t gfx_create_context(void** context);
gfx_result_t gfx_destroy_context(void* context);
gfx_result_t gfx_make_current(void* context);
gfx_result_t gfx_swap_3d_buffers(void);

gfx_result_t gfx_create_shader(uint32_t type, const char* source, void** shader);
gfx_result_t gfx_destroy_shader(void* shader);
gfx_result_t gfx_create_program(void** program);
gfx_result_t gfx_attach_shader(void* program, void* shader);
gfx_result_t gfx_link_program(void* program);
gfx_result_t gfx_use_program(void* program);

gfx_result_t gfx_create_buffer(uint32_t target, size_t size, const void* data, uint32_t usage, void** buffer);
gfx_result_t gfx_bind_buffer(uint32_t target, void* buffer);
gfx_result_t gfx_buffer_data(uint32_t target, size_t size, const void* data);
gfx_result_t gfx_buffer_sub_data(uint32_t target, size_t offset, size_t size, const void* data);
gfx_result_t gfx_destroy_buffer(void* buffer);

gfx_result_t gfx_enable_vertex_attrib_array(uint32_t index);
gfx_result_t gfx_disable_vertex_attrib_array(uint32_t index);
gfx_result_t gfx_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type, bool normalized, int32_t stride, const void* pointer);

gfx_result_t gfx_get_uniform_location(void* program, const char* name, int32_t* location);
gfx_result_t gfx_uniform1f(int32_t location, double value);
gfx_result_t gfx_uniform1i(int32_t location, int32_t value);
gfx_result_t gfx_uniform2f(int32_t location, double x, double y);
gfx_result_t gfx_uniform3f(int32_t location, double x, double y, double z);
gfx_result_t gfx_uniform4f(int32_t location, double x, double y, double z, double w);
gfx_result_t gfx_uniform_matrix4fv(int32_t location, bool transpose, const double* value);

gfx_result_t gfx_draw_arrays(uint32_t mode, int32_t first, int32_t count);
gfx_result_t gfx_draw_elements(uint32_t mode, int32_t count, uint32_t type, const void* indices);

/* Emergency panic display - writes error directly to framebuffer */
void gfx_panic_display(const char* message);

/* ============================================================================
 * Helper Macros for Driver Implementation
 * ============================================================================ */

#define DECLARE_GFX_DRIVER(name, ops_ptr, types) \
    gfx_driver_t name##_gfx_driver = { \
        .ops = ops_ptr, \
        .supported_types = types, \
        .flags = GFX_DRV_FLAG_BUILTIN, \
        .priority = 128, \
        .private_data = NULL, \
        .next = NULL \
    }

#define GFX_DRIVER_INIT(name) \
    gfx_result_t name##_gfx_init(void)

#define GFX_DRIVER_EXIT(name) \
    void name##_gfx_exit(void)

#endif /* GRAPHICS_DRIVER_V2_H */

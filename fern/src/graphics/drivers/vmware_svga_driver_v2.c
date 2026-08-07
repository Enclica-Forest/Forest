/**
 * Fern - VMware SVGA-II Driver V2
 * 
 * Complete rewrite based on VMware SVGA Device Developer Kit documentation.
 * 
 * This driver supports:
 * - VMware Workstation/Player/Fusion
 * - VMware ESXi
 * - QEMU with -vga vmware
 * 
 * Features:
 * - SVGA-II command interface
 * - FIFO command buffer for 2D acceleration
 * - Hardware cursor support
 * - Multiple display support (if available)
 * - Capabilities detection
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/pci.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/kheap_enhanced.h"

/* FIFO Memory Layout */
#define SVGA_FIFO_MIN_SIZE      (256 * 1024)    /* 256 KB minimum */
#define SVGA_FIFO_CMD_OFFSET    (SVGA_FIFO_NUM_REGS * 4)  /* Start of command area */

/* Driver-private data structure */
typedef struct {
    /* Hardware resources */
    uint16_t io_base;           /* I/O port base */
    void* fb_virt;              /* Mapped framebuffer */
    volatile uint32_t* fifo;    /* Mapped FIFO memory */
    size_t fifo_size;           /* FIFO memory size */
    
    /* Device capabilities */
    uint32_t capabilities;      /* SVGA_CAP_* flags */
    uint32_t vram_size;         /* Total VRAM */
    uint32_t fb_size;           /* Framebuffer size */
    uint32_t max_width;         /* Maximum width */
    uint32_t max_height;        /* Maximum height */
    
    /* Current state */
    uint32_t svga_id;           /* Negotiated SVGA ID */
    uint32_t width;             /* Current width */
    uint32_t height;            /* Current height */
    uint32_t bpp;               /* Current BPP */
    uint32_t pitch;             /* Current pitch */
    uint32_t depth;             /* Color depth */
    
    /* Masks for pixel format */
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    
    /* Cursor state */
    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;
    uint32_t cursor_id;
    
    /* FIFO state */
    bool fifo_enabled;
    uint32_t fifo_next_fence;
    
    /* Framebuffer tracking */
    gfx_framebuffer_t framebuffer;
} svga_private_t;

/* Forward declarations */
static gfx_result_t svga_probe(gfx_device_t* dev);
static gfx_result_t svga_init(gfx_device_t* dev);
static gfx_result_t svga_shutdown(gfx_device_t* dev);
static gfx_result_t svga_reset(gfx_device_t* dev);
static gfx_result_t svga_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t svga_set_mode(gfx_device_t* dev, const gfx_mode_t* mode);
static gfx_result_t svga_get_mode(gfx_device_t* dev, gfx_mode_t* mode);
static gfx_result_t svga_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t svga_unmap_fb(gfx_device_t* dev, gfx_framebuffer_t* fb);
static gfx_result_t svga_clear(gfx_device_t* dev, gfx_color_t color);
static gfx_result_t svga_draw_rect(gfx_device_t* dev, const gfx_rect_t* r, gfx_color_t c, bool filled);
static gfx_result_t svga_blit(gfx_device_t* dev, const gfx_rect_t* src, int32_t dx, int32_t dy);
static gfx_result_t svga_set_cursor(gfx_device_t* dev, const gfx_cursor_t* cursor);
static gfx_result_t svga_move_cursor(gfx_device_t* dev, int32_t x, int32_t y);
static gfx_result_t svga_show_cursor(gfx_device_t* dev, bool show);
static gfx_result_t svga_flush(gfx_device_t* dev);

/* 3D acceleration operations */
static gfx_result_t svga_create_context(gfx_device_t* dev, void** context);
static gfx_result_t svga_destroy_context(gfx_device_t* dev, void* context);
static gfx_result_t svga_make_current(gfx_device_t* dev, void* context);
static gfx_result_t svga_swap_buffers(gfx_device_t* dev);
static gfx_result_t svga_create_shader(gfx_device_t* dev, uint32_t type, const char* source, void** shader);
static gfx_result_t svga_destroy_shader(gfx_device_t* dev, void* shader);
static gfx_result_t svga_create_program(gfx_device_t* dev, void** program);
static gfx_result_t svga_attach_shader(gfx_device_t* dev, void* program, void* shader);
static gfx_result_t svga_link_program(gfx_device_t* dev, void* program);
static gfx_result_t svga_use_program(gfx_device_t* dev, void* program);
static gfx_result_t svga_create_buffer(gfx_device_t* dev, uint32_t target, size_t size, const void* data, uint32_t usage, void** buffer);
static gfx_result_t svga_bind_buffer(gfx_device_t* dev, uint32_t target, void* buffer);
static gfx_result_t svga_buffer_data(gfx_device_t* dev, uint32_t target, size_t size, const void* data);
static gfx_result_t svga_buffer_sub_data(gfx_device_t* dev, uint32_t target, size_t offset, size_t size, const void* data);
static gfx_result_t svga_destroy_buffer(gfx_device_t* dev, void* buffer);
static gfx_result_t svga_enable_vertex_attrib_array(gfx_device_t* dev, uint32_t index);
static gfx_result_t svga_disable_vertex_attrib_array(gfx_device_t* dev, uint32_t index);
static gfx_result_t svga_vertex_attrib_pointer(gfx_device_t* dev, uint32_t index, int32_t size, uint32_t type, bool normalized, int32_t stride, const void* pointer);
static gfx_result_t svga_get_uniform_location(gfx_device_t* dev, void* program, const char* name, int32_t* location);
static gfx_result_t svga_uniform1f(gfx_device_t* dev, int32_t location, double value);
static gfx_result_t svga_uniform1i(gfx_device_t* dev, int32_t location, int32_t value);
static gfx_result_t svga_uniform2f(gfx_device_t* dev, int32_t location, double x, double y);
static gfx_result_t svga_uniform3f(gfx_device_t* dev, int32_t location, double x, double y, double z);
static gfx_result_t svga_uniform4f(gfx_device_t* dev, int32_t location, double x, double y, double z, double w);
static gfx_result_t svga_uniform_matrix4fv(gfx_device_t* dev, int32_t location, bool transpose, const double* value);
static gfx_result_t svga_draw_arrays(gfx_device_t* dev, uint32_t mode, int32_t first, int32_t count);
static gfx_result_t svga_draw_elements(gfx_device_t* dev, uint32_t mode, int32_t count, uint32_t type, const void* indices);

/* Driver operations table */
static const gfx_driver_ops_t svga_driver_ops = {
    .name = "vmware-svga",
    .version = 0x00020001,  /* 2.0.1 */
    
    .probe = svga_probe,
    .init = svga_init,
    .shutdown = svga_shutdown,
    .reset = svga_reset,
    
    .get_modes = svga_get_modes,
    .set_mode = svga_set_mode,
    .get_mode = svga_get_mode,
    
    .map_fb = svga_map_fb,
    .unmap_fb = svga_unmap_fb,
    .set_fb_offset = NULL,
    
    .clear = svga_clear,
    .draw_pixel = NULL,
    .draw_rect = svga_draw_rect,
    .blit = svga_blit,
    
    .set_cursor = svga_set_cursor,
    .move_cursor = svga_move_cursor,
    .show_cursor = svga_show_cursor,
    
    .write_char = NULL,
    .set_text_cursor = NULL,
    .scroll = NULL,
    
    .wait_vsync = NULL,
    .flip = NULL,
    .flush = svga_flush,
    
    .read_edid = NULL,
    .detect_displays = NULL,
    
    .set_dpms = NULL,
    
    /* 3D acceleration operations */
    .create_context = svga_create_context,
    .destroy_context = svga_destroy_context,
    .make_current = svga_make_current,
    .swap_buffers = svga_swap_buffers,
    .create_shader = svga_create_shader,
    .destroy_shader = svga_destroy_shader,
    .create_program = svga_create_program,
    .attach_shader = svga_attach_shader,
    .link_program = svga_link_program,
    .use_program = svga_use_program,
    .create_buffer = svga_create_buffer,
    .bind_buffer = svga_bind_buffer,
    .buffer_data = svga_buffer_data,
    .buffer_sub_data = svga_buffer_sub_data,
    .destroy_buffer = svga_destroy_buffer,
    .enable_vertex_attrib_array = svga_enable_vertex_attrib_array,
    .disable_vertex_attrib_array = svga_disable_vertex_attrib_array,
    .vertex_attrib_pointer = svga_vertex_attrib_pointer,
    .get_uniform_location = svga_get_uniform_location,
    .uniform1f = svga_uniform1f,
    .uniform1i = svga_uniform1i,
    .uniform2f = svga_uniform2f,
    .uniform3f = svga_uniform3f,
    .uniform4f = svga_uniform4f,
    .uniform_matrix4fv = svga_uniform_matrix4fv,
    .draw_arrays = svga_draw_arrays,
    .draw_elements = svga_draw_elements,
    
    .ioctl = NULL,
};

/* Driver instance */
DECLARE_GFX_DRIVER(svga, &svga_driver_ops, GFX_DEVICE_VMWARE_SVGA);

/* ============================================================================
 * SVGA Register Access
 * ============================================================================ */

static void svga_write_reg(svga_private_t* priv, uint32_t index, uint32_t value) {
    gfx_outl(priv->io_base + SVGA_INDEX_PORT, index);
    gfx_outl(priv->io_base + SVGA_VALUE_PORT, value);
}

static uint32_t svga_read_reg(svga_private_t* priv, uint32_t index) {
    gfx_outl(priv->io_base + SVGA_INDEX_PORT, index);
    return gfx_inl(priv->io_base + SVGA_VALUE_PORT);
}

/* ============================================================================
 * SVGA FIFO Operations
 * ============================================================================ */

/**
 * Initialize the FIFO command buffer
 */
static bool svga_fifo_init(svga_private_t* priv) {
    if (!priv->fifo || priv->fifo_size < SVGA_FIFO_MIN_SIZE) {
        return false;
    }
    
    /* Initialize FIFO registers */
    priv->fifo[SVGA_FIFO_MIN] = SVGA_FIFO_NUM_REGS * sizeof(uint32_t);
    priv->fifo[SVGA_FIFO_MAX] = priv->fifo_size;
    priv->fifo[SVGA_FIFO_NEXT_CMD] = priv->fifo[SVGA_FIFO_MIN];
    priv->fifo[SVGA_FIFO_STOP] = priv->fifo[SVGA_FIFO_MIN];
    
    /* Memory barrier before enabling */
    __asm__ volatile("mfence" ::: "memory");
    
    /* Enable FIFO */
    svga_write_reg(priv, SVGA_REG_CONFIG_DONE, 1);
    
    priv->fifo_enabled = true;
    priv->fifo_next_fence = 1;
    
    debug_print("[SVGA] FIFO initialized: min=%u max=%u\n",
                priv->fifo[SVGA_FIFO_MIN], priv->fifo[SVGA_FIFO_MAX]);
    
    return true;
}

/**
 * Reserve space in the FIFO for a command
 */
static uint32_t* svga_fifo_reserve(svga_private_t* priv, uint32_t bytes) {
    if (!priv->fifo_enabled) {
        return NULL;
    }
    
    uint32_t min = priv->fifo[SVGA_FIFO_MIN];
    uint32_t max = priv->fifo[SVGA_FIFO_MAX];
    uint32_t next_cmd = priv->fifo[SVGA_FIFO_NEXT_CMD];
    
    /* Wait for enough space */
    uint32_t stop = priv->fifo[SVGA_FIFO_STOP];
    
    /* Calculate available space */
    uint32_t available;
    if (next_cmd >= stop) {
        available = (max - next_cmd) + (stop - min);
    } else {
        available = stop - next_cmd;
    }
    
    /* Wait if not enough space */
    if (available < bytes) {
        /* Sync to empty FIFO */
        svga_write_reg(priv, SVGA_REG_SYNC, 1);
        while (svga_read_reg(priv, SVGA_REG_BUSY));
        
        /* Re-check available space */
        stop = priv->fifo[SVGA_FIFO_STOP];
        if (next_cmd >= stop) {
            available = (max - next_cmd) + (stop - min);
        } else {
            available = stop - next_cmd;
        }
        
        if (available < bytes) {
            return NULL;  /* Still not enough */
        }
    }
    
    return (uint32_t*)((uint8_t*)priv->fifo + next_cmd);
}

/**
 * Commit FIFO command
 */
static void svga_fifo_commit(svga_private_t* priv, uint32_t bytes) {
    uint32_t min = priv->fifo[SVGA_FIFO_MIN];
    uint32_t max = priv->fifo[SVGA_FIFO_MAX];
    uint32_t next_cmd = priv->fifo[SVGA_FIFO_NEXT_CMD] + bytes;
    
    /* Wrap if necessary */
    if (next_cmd >= max) {
        next_cmd = min + (next_cmd - max);
    }
    
    __asm__ volatile("mfence" ::: "memory");
    priv->fifo[SVGA_FIFO_NEXT_CMD] = next_cmd;
}

/**
 * Simple FIFO command: Update a rectangular region
 */
static void svga_update_rect(svga_private_t* priv, int32_t x, int32_t y, 
                             uint32_t width, uint32_t height) {
    if (!priv->fifo_enabled) {
        /* Fallback: update entire screen */
        svga_write_reg(priv, SVGA_REG_SYNC, 1);
        while (svga_read_reg(priv, SVGA_REG_BUSY));
        return;
    }
    
    uint32_t* cmd = svga_fifo_reserve(priv, 5 * sizeof(uint32_t));
    if (!cmd) return;
    
    cmd[0] = SVGA_CMD_UPDATE;
    cmd[1] = x;
    cmd[2] = y;
    cmd[3] = width;
    cmd[4] = height;
    
    svga_fifo_commit(priv, 5 * sizeof(uint32_t));
}

/**
 * FIFO command: Rectangle fill (hardware accelerated)
 */
static void svga_fifo_rect_fill(svga_private_t* priv, int32_t x, int32_t y,
                                uint32_t width, uint32_t height, uint32_t color) {
    if (!priv->fifo_enabled) return;
    
    uint32_t* cmd = svga_fifo_reserve(priv, 6 * sizeof(uint32_t));
    if (!cmd) return;
    
    cmd[0] = SVGA_CMD_FRONT_ROP_FILL;
    cmd[1] = color;
    cmd[2] = x;
    cmd[3] = y;
    cmd[4] = width;
    cmd[5] = height;
    
    svga_fifo_commit(priv, 6 * sizeof(uint32_t));
}

/**
 * FIFO command: Rectangle copy (blit)
 */
static void svga_fifo_rect_copy(svga_private_t* priv, 
                                int32_t src_x, int32_t src_y,
                                int32_t dst_x, int32_t dst_y,
                                uint32_t width, uint32_t height) {
    if (!priv->fifo_enabled || !(priv->capabilities & SVGA_CAP_RECT_COPY)) return;
    
    uint32_t* cmd = svga_fifo_reserve(priv, 7 * sizeof(uint32_t));
    if (!cmd) return;
    
    cmd[0] = SVGA_CMD_RECT_COPY;
    cmd[1] = src_x;
    cmd[2] = src_y;
    cmd[3] = dst_x;
    cmd[4] = dst_y;
    cmd[5] = width;
    cmd[6] = height;
    
    svga_fifo_commit(priv, 7 * sizeof(uint32_t));
}

/**
 * FIFO command: Define cursor
 */
__attribute__((unused)) static void svga_fifo_define_cursor(svga_private_t* priv, uint32_t id,
                                    uint32_t width, uint32_t height,
                                    int32_t hotspot_x, int32_t hotspot_y,
                                    const uint32_t* and_mask, const uint32_t* xor_mask) {
    if (!priv->fifo_enabled || !(priv->capabilities & SVGA_CAP_CURSOR)) return;
    
    uint32_t num_pixels = width * height;
    uint32_t cmd_size = 6 + (num_pixels * 2);  /* header + AND mask + XOR mask */
    
    uint32_t* cmd = svga_fifo_reserve(priv, cmd_size * sizeof(uint32_t));
    if (!cmd) return;
    
    cmd[0] = SVGA_CMD_DEFINE_CURSOR;
    cmd[1] = id;
    cmd[2] = hotspot_x;
    cmd[3] = hotspot_y;
    cmd[4] = width;
    cmd[5] = height;
    
    /* Copy AND mask */
    for (uint32_t i = 0; i < num_pixels; i++) {
        cmd[6 + i] = and_mask ? and_mask[i] : 0x00000000;
    }
    
    /* Copy XOR mask */
    for (uint32_t i = 0; i < num_pixels; i++) {
        cmd[6 + num_pixels + i] = xor_mask ? xor_mask[i] : 0x00000000;
    }
    
    svga_fifo_commit(priv, cmd_size * sizeof(uint32_t));
}

/**
 * FIFO command: Define alpha cursor (ARGB)
 */
static void svga_fifo_define_alpha_cursor(svga_private_t* priv, uint32_t id,
                                          uint32_t width, uint32_t height,
                                          int32_t hotspot_x, int32_t hotspot_y,
                                          const uint32_t* pixels) {
    if (!priv->fifo_enabled || !(priv->capabilities & SVGA_CAP_ALPHA_CURSOR)) return;
    
    uint32_t num_pixels = width * height;
    uint32_t cmd_size = 6 + num_pixels;
    
    uint32_t* cmd = svga_fifo_reserve(priv, cmd_size * sizeof(uint32_t));
    if (!cmd) return;
    
    cmd[0] = SVGA_CMD_DEFINE_ALPHA_CURSOR;
    cmd[1] = id;
    cmd[2] = hotspot_x;
    cmd[3] = hotspot_y;
    cmd[4] = width;
    cmd[5] = height;
    
    /* Copy pixel data */
    for (uint32_t i = 0; i < num_pixels; i++) {
        cmd[6 + i] = pixels ? pixels[i] : 0;
    }
    
    svga_fifo_commit(priv, cmd_size * sizeof(uint32_t));
}

/* ============================================================================
 * PCI Detection
 * ============================================================================ */

static bool svga_find_pci_device(gfx_device_t* dev, svga_private_t* priv) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                
                if (vendor_device == 0xFFFFFFFF) continue;
                
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                if (vendor == VMWARE_PCI_VENDOR && 
                    (device == VMWARE_SVGA_PCI_DEVICE || device == VMWARE_SVGA_PCI_DEVICE_OLD)) {
                    
                    dev->pci_bus = bus;
                    dev->pci_slot = slot;
                    dev->pci_func = func;
                    dev->vendor_id = vendor;
                    dev->device_id = device;
                    
                    /* BAR 0: I/O ports */
                    uint32_t bar0 = pci_read_config(bus, slot, func, 0x10);
                    if (bar0 & 0x1) {
                        priv->io_base = bar0 & 0xFFFC;
                    }
                    
                    /* BAR 1: Framebuffer */
                    uint32_t bar1 = pci_read_config(bus, slot, func, 0x14);
                    if ((bar1 & 0x1) == 0) {
                        dev->fb_base = bar1 & 0xFFFFFFF0;
                    }
                    
                    /* BAR 2: FIFO memory */
                    uint32_t bar2 = pci_read_config(bus, slot, func, 0x18);
                    if ((bar2 & 0x1) == 0) {
                        dev->mmio_base = bar2 & 0xFFFFFFF0;
                    }
                    
                    /* Enable bus mastering, I/O, and memory */
                    uint32_t cmd = pci_read_config(bus, slot, func, 0x04);
                    cmd |= 0x07;
                    pci_write_config(bus, slot, func, 0x04, cmd);
                    
                    debug_print("[SVGA] PCI: %04x:%04x at %02x:%02x.%x\n",
                                vendor, device, bus, slot, func);
                    debug_print("[SVGA] I/O: 0x%04x, FB: 0x%08x, FIFO: 0x%08x\n",
                                priv->io_base, (uint32_t)dev->fb_base, (uint32_t)dev->mmio_base);
                    
                    return true;
                }
            }
        }
    }
    
    return false;
}

/* ============================================================================
 * Driver Implementation
 * ============================================================================ */

static gfx_result_t svga_probe(gfx_device_t* dev) {
    /* Allocate private data */
    svga_private_t* priv = (svga_private_t*)kheap_alloc(sizeof(svga_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(svga_private_t));
    
    /* Find PCI device */
    if (!svga_find_pci_device(dev, priv)) {
    kheap_free(priv);
    return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Negotiate SVGA ID version */
    svga_write_reg(priv, SVGA_REG_ID, SVGA_ID_2);
    priv->svga_id = svga_read_reg(priv, SVGA_REG_ID);
    
    if (priv->svga_id == SVGA_ID_INVALID || priv->svga_id < SVGA_ID_0) {
        debug_print("[SVGA] Invalid SVGA ID: 0x%08x\n", priv->svga_id);
    kheap_free(priv);
    return GFX_ERR_NOT_SUPPORTED;
    }
    
    debug_print("[SVGA] Negotiated SVGA ID: 0x%08x\n", priv->svga_id);
    
    /* Read capabilities */
    priv->capabilities = svga_read_reg(priv, SVGA_REG_CAPABILITIES);
    priv->vram_size = svga_read_reg(priv, SVGA_REG_VRAM_SIZE);
    priv->fb_size = svga_read_reg(priv, SVGA_REG_FB_SIZE);
    priv->max_width = svga_read_reg(priv, SVGA_REG_MAX_WIDTH);
    priv->max_height = svga_read_reg(priv, SVGA_REG_MAX_HEIGHT);
    
    debug_print("[SVGA] Capabilities: 0x%08x\n", priv->capabilities);
    debug_print("[SVGA] VRAM: %u MB, Max: %ux%u\n",
                priv->vram_size / (1024*1024), priv->max_width, priv->max_height);
    
    /* Get FIFO memory size */
    priv->fifo_size = svga_read_reg(priv, SVGA_REG_MEM_SIZE);
    if (priv->fifo_size < SVGA_FIFO_MIN_SIZE) {
        priv->fifo_size = SVGA_FIFO_MIN_SIZE;
    }
    
    dev->fb_size = priv->fb_size;
    dev->vram_size = priv->vram_size;
    dev->mmio_size = priv->fifo_size;
    
    /* Fill in device info */
    dev->type = GFX_DEVICE_VMWARE_SVGA;
    dev->max_width = priv->max_width;
    dev->max_height = priv->max_height;
    dev->max_bpp = 32;
    dev->driver_data = priv;
    
    /* Set capabilities */
    dev->caps = GFX_CAP_LINEAR_FB;
    if (priv->capabilities & SVGA_CAP_RECT_COPY) {
        dev->caps |= GFX_CAP_HW_COPY;
    }
    if (priv->capabilities & SVGA_CAP_CURSOR) {
        dev->caps |= GFX_CAP_HW_CURSOR;
    }
    /* GFX_CAP_ALPHA_CURSOR is not defined in current version */
    if (priv->capabilities & SVGA_CAP_ALPHA_CURSOR) {
        /* Just use GFX_CAP_HW_CURSOR for alpha cursor support */
        dev->caps |= GFX_CAP_HW_CURSOR;
    }
    if (priv->capabilities & SVGA_CAP_3D) {
        dev->caps |= GFX_CAP_3D | GFX_CAP_SHADERS | GFX_CAP_TEXTURE_2D | 
                      GFX_CAP_VERTEX_BUFFERS | GFX_CAP_INDEX_BUFFERS | 
                      GFX_CAP_BLENDING | GFX_CAP_DEPTH_TEST;
    }
    if (priv->capabilities & SVGA_CAP_MULTIMON) {
        dev->caps |= GFX_CAP_MULTI_HEAD;
    }
    
    strncpy(dev->name, "VMware SVGA-II", sizeof(dev->name) - 1);
    
    debug_print("[SVGA] Probe successful\n");
    return GFX_OK;
}

static gfx_result_t svga_init(gfx_device_t* dev) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Map FIFO memory */
    if (dev->mmio_base && priv->fifo_size > 0) {
        priv->fifo = (volatile uint32_t*)map_physical_memory(dev->mmio_base, priv->fifo_size);
        if (priv->fifo) {
            svga_fifo_init(priv);
        } else {
            debug_print("[SVGA] Warning: Failed to map FIFO memory\n");
        }
    }
    
    debug_print("[SVGA] Initialized\n");
    return GFX_OK;
}

static gfx_result_t svga_shutdown(gfx_device_t* dev) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (priv) {
        /* Disable SVGA */
        svga_write_reg(priv, SVGA_REG_ENABLE, 0);
        
        kheap_free(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t svga_reset(gfx_device_t* dev) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (priv) {
        svga_write_reg(priv, SVGA_REG_ENABLE, 0);
        priv->fifo_enabled = false;
    }
    
    return GFX_OK;
}

static gfx_result_t svga_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    /* Standard resolutions */
    static const struct { uint32_t w, h; } resolutions[] = {
        { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 },
        { 1280, 800 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 },
        { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
        { 2560, 1440 }, { 2560, 1600 }, { 3840, 2160 },
    };
    
    /* Count valid modes */
    uint32_t num_modes = 0;
    for (size_t i = 0; i < sizeof(resolutions)/sizeof(resolutions[0]); i++) {
        if (resolutions[i].w <= priv->max_width && resolutions[i].h <= priv->max_height) {
            num_modes++;  /* 32-bit only for SVGA */
        }
    }
    
    if (num_modes == 0) {
        return GFX_ERR_MODE_NOT_FOUND;
    }
    
    gfx_mode_t* mode_list = (gfx_mode_t*)kheap_alloc(num_modes * sizeof(gfx_mode_t));
    if (!mode_list) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(mode_list, 0, num_modes * sizeof(gfx_mode_t));
    
    uint32_t idx = 0;
    for (size_t i = 0; i < sizeof(resolutions)/sizeof(resolutions[0]); i++) {
        if (resolutions[i].w <= priv->max_width && resolutions[i].h <= priv->max_height) {
            gfx_mode_t* m = &mode_list[idx++];
            m->mode_id = idx;
            m->width = resolutions[i].w;
            m->height = resolutions[i].h;
            m->bpp = 32;
            m->pitch = m->width * 4;
            m->format = GFX_FORMAT_BGRX8888;
            m->refresh_hz = 60;
        }
    }
    
    *modes = mode_list;
    *count = num_modes;
    return GFX_OK;
}

static gfx_result_t svga_set_mode(gfx_device_t* dev, const gfx_mode_t* mode) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!mode || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    debug_print("[SVGA] Setting mode: %ux%ux%u\n", mode->width, mode->height, mode->bpp);
    
    /* Disable display first */
    svga_write_reg(priv, SVGA_REG_ENABLE, 0);
    
    /* Set dimensions */
    svga_write_reg(priv, SVGA_REG_WIDTH, mode->width);
    svga_write_reg(priv, SVGA_REG_HEIGHT, mode->height);
    svga_write_reg(priv, SVGA_REG_BITS_PER_PIXEL, mode->bpp);
    
    /* Enable display */
    svga_write_reg(priv, SVGA_REG_ENABLE, 1);
    
    /* Read back actual values */
    priv->width = svga_read_reg(priv, SVGA_REG_WIDTH);
    priv->height = svga_read_reg(priv, SVGA_REG_HEIGHT);
    priv->bpp = svga_read_reg(priv, SVGA_REG_BITS_PER_PIXEL);
    priv->pitch = svga_read_reg(priv, SVGA_REG_BYTES_PER_LINE);
    priv->depth = svga_read_reg(priv, SVGA_REG_DEPTH);
    
    /* Read color masks */
    priv->red_mask = svga_read_reg(priv, SVGA_REG_RED_MASK);
    priv->green_mask = svga_read_reg(priv, SVGA_REG_GREEN_MASK);
    priv->blue_mask = svga_read_reg(priv, SVGA_REG_BLUE_MASK);
    
    /* Read framebuffer offset (may change between mode sets) */
    uint32_t fb_offset = svga_read_reg(priv, SVGA_REG_FB_OFFSET);
    priv->framebuffer.phys_addr = dev->fb_base + fb_offset;
    
    debug_print("[SVGA] Mode set: %ux%ux%u pitch=%u\n",
                priv->width, priv->height, priv->bpp, priv->pitch);
    debug_print("[SVGA] Masks: R=0x%08x G=0x%08x B=0x%08x\n",
                priv->red_mask, priv->green_mask, priv->blue_mask);
    
    /* Update device state */
    dev->current_mode.width = priv->width;
    dev->current_mode.height = priv->height;
    dev->current_mode.bpp = priv->bpp;
    dev->current_mode.pitch = priv->pitch;
    dev->current_mode.format = GFX_FORMAT_BGRX8888;
    
    /* Update framebuffer info */
    priv->framebuffer.width = priv->width;
    priv->framebuffer.height = priv->height;
    priv->framebuffer.bpp = priv->bpp;
    priv->framebuffer.pitch = priv->pitch;
    priv->framebuffer.size = priv->pitch * priv->height;
    priv->framebuffer.format = GFX_FORMAT_BGRX8888;
    
    /* Re-initialize FIFO if available */
    if (priv->fifo && !priv->fifo_enabled) {
        svga_fifo_init(priv);
    }
    
    dev->active = true;
    return GFX_OK;
}

static gfx_result_t svga_get_mode(gfx_device_t* dev, gfx_mode_t* mode) {
    if (!mode) return GFX_ERR_INVALID_PARAM;
    *mode = dev->current_mode;
    return GFX_OK;
}

static gfx_result_t svga_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!fb || !priv) return GFX_ERR_INVALID_PARAM;
    
    if (!priv->framebuffer.virt_addr) {
        (void)map_physical_memory(dev->fb_base, (priv->fb_size + 4095) & ~4095);
        
        void* vaddr = map_physical_memory(dev->fb_base, map_size);
        if (!vaddr) {
            debug_print("[SVGA] Failed to map framebuffer\n");
            return GFX_ERR_MAPPING_FAILED;
        }
        
        priv->framebuffer.virt_addr = vaddr;
        priv->fb_virt = vaddr;
        
        debug_print("[SVGA] Mapped framebuffer: 0x%08x -> %p\n",
                    (uint32_t)dev->fb_base, vaddr);
    }
    
    *fb = &priv->framebuffer;
    return GFX_OK;
}

static gfx_result_t svga_unmap_fb(gfx_device_t* dev, gfx_framebuffer_t* fb) {
    (void)fb;
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (priv) {
        priv->framebuffer.virt_addr = NULL;
        priv->fb_virt = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t svga_clear(gfx_device_t* dev, gfx_color_t color) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv || !priv->fb_virt) return GFX_ERR_INVALID_PARAM;
    
    uint32_t pixel = gfx_color_to_pixel(color, priv->framebuffer.format);
    
    /* Use hardware fill if available */
    if (priv->fifo_enabled) {
        svga_fifo_rect_fill(priv, 0, 0, priv->width, priv->height, pixel);
        svga_update_rect(priv, 0, 0, priv->width, priv->height);
        return GFX_OK;
    }
    
    /* Software fallback */
    uint32_t* fb = (uint32_t*)priv->fb_virt;
    for (uint32_t y = 0; y < priv->height; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)fb + y * priv->pitch);
        for (uint32_t x = 0; x < priv->width; x++) {
            row[x] = pixel;
        }
    }
    
    return GFX_OK;
}

static gfx_result_t svga_draw_rect(gfx_device_t* dev, const gfx_rect_t* r, 
                                   gfx_color_t c, bool filled) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv || !r) return GFX_ERR_INVALID_PARAM;
    
    uint32_t pixel = gfx_color_to_pixel(c, priv->framebuffer.format);
    
    if (filled && priv->fifo_enabled) {
        svga_fifo_rect_fill(priv, r->x, r->y, r->width, r->height, pixel);
        svga_update_rect(priv, r->x, r->y, r->width, r->height);
        return GFX_OK;
    }
    
    /* Software fallback for non-filled or when FIFO unavailable */
    return GFX_ERR_NOT_SUPPORTED;
}

static gfx_result_t svga_blit(gfx_device_t* dev, const gfx_rect_t* src, 
                              int32_t dx, int32_t dy) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv || !src) return GFX_ERR_INVALID_PARAM;
    
    if (priv->fifo_enabled && (priv->capabilities & SVGA_CAP_RECT_COPY)) {
        svga_fifo_rect_copy(priv, src->x, src->y, dx, dy, src->width, src->height);
        svga_update_rect(priv, dx, dy, src->width, src->height);
        return GFX_OK;
    }
    
    return GFX_ERR_NOT_SUPPORTED;
}

static gfx_result_t svga_set_cursor(gfx_device_t* dev, const gfx_cursor_t* cursor) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv || !cursor) return GFX_ERR_INVALID_PARAM;
    
    if (priv->capabilities & SVGA_CAP_ALPHA_CURSOR) {
        priv->cursor_id++;
        svga_fifo_define_alpha_cursor(priv, priv->cursor_id,
                                      cursor->width, cursor->height,
                                      cursor->hotspot_x, cursor->hotspot_y,
                                      cursor->pixels);
        svga_write_reg(priv, SVGA_REG_CURSOR_ID, priv->cursor_id);
        return GFX_OK;
    }
    
    return GFX_ERR_NOT_SUPPORTED;
}

static gfx_result_t svga_move_cursor(gfx_device_t* dev, int32_t x, int32_t y) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv) return GFX_ERR_INVALID_PARAM;
    
    priv->cursor_x = x;
    priv->cursor_y = y;
    
    if (priv->capabilities & SVGA_CAP_CURSOR) {
        svga_write_reg(priv, SVGA_REG_CURSOR_X, x);
        svga_write_reg(priv, SVGA_REG_CURSOR_Y, y);
    }
    
    return GFX_OK;
}

static gfx_result_t svga_show_cursor(gfx_device_t* dev, bool show) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    
    if (!priv) return GFX_ERR_INVALID_PARAM;
    
    priv->cursor_visible = show;
    
    if (priv->capabilities & SVGA_CAP_CURSOR) {
        svga_write_reg(priv, SVGA_REG_CURSOR_ON, show ? 1 : 0);
    }
    
    return GFX_OK;
}

static gfx_result_t svga_flush(gfx_device_t* dev) {
    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_update_rect(priv, 0, 0, priv->width, priv->height);
    return GFX_OK;
}

/* ============================================================================
 * 3D Acceleration Implementation
 * ============================================================================ */

static gfx_result_t svga_create_context(gfx_device_t* dev, void** context) {
    if (!dev || !context) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    /* Allocate context memory */
    void* ctx = kheap_alloc(4096);  /* Small context for now */
    if (!ctx) {
        return GFX_ERR_NO_MEMORY;
    }

    memset(ctx, 0, 4096);
    *context = ctx;

    debug_print("[SVGA] 3D context created\n");
    return GFX_OK;
}

static gfx_result_t svga_destroy_context(gfx_device_t* dev, void* context) {
    if (!dev || !context) {
        return GFX_ERR_INVALID_PARAM;
    }

    kheap_free(context);
    debug_print("[SVGA] 3D context destroyed\n");
    return GFX_OK;
}

static gfx_result_t svga_make_current(gfx_device_t* dev, void* context) {
    if (!dev) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    debug_print("[SVGA] 3D context %p made current\n", context);
    return GFX_OK;
}

static gfx_result_t svga_swap_buffers(gfx_device_t* dev) {
    if (!dev) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    /* For now, just flush the framebuffer */
    svga_flush(dev);
    return GFX_OK;
}

static gfx_result_t svga_create_shader(gfx_device_t* dev, uint32_t type, const char* source, void** shader) {
    if (!dev || !source || !shader) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    void* shad = kheap_alloc(256);
    if (!shad) {
        return GFX_ERR_NO_MEMORY;
    }

    memset(shad, 0, 256);
    *shader = shad;

    debug_print("[SVGA] Shader created type=%u\n", type);
    return GFX_OK;
}

static gfx_result_t svga_destroy_shader(gfx_device_t* dev, void* shader) {
    if (!dev || !shader) {
        return GFX_ERR_INVALID_PARAM;
    }

    kheap_free(shader);
    debug_print("[SVGA] Shader destroyed\n");
    return GFX_OK;
}

static gfx_result_t svga_create_program(gfx_device_t* dev, void** program) {
    if (!dev || !program) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    void* prog = kheap_alloc(512);
    if (!prog) {
        return GFX_ERR_NO_MEMORY;
    }

    memset(prog, 0, 512);
    *program = prog;

    debug_print("[SVGA] Program created\n");
    return GFX_OK;
}

static gfx_result_t svga_attach_shader(gfx_device_t* dev, void* program, void* shader) {
    if (!dev || !program || !shader) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_link_program(gfx_device_t* dev, void* program) {
    if (!dev || !program) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    debug_print("[SVGA] Program linked\n");
    return GFX_OK;
}

static gfx_result_t svga_use_program(gfx_device_t* dev, void* program) {
    if (!dev) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    debug_print("[SVGA] Program %p in use\n", program);
    return GFX_OK;
}

static gfx_result_t svga_create_buffer(gfx_device_t* dev, uint32_t target, size_t size, const void* data, uint32_t usage, void** buffer) {
    (void)usage;
    if (!dev || !buffer) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    void* buf = kheap_alloc(size);
    if (!buf) {
        return GFX_ERR_NO_MEMORY;
    }

    if (data) {
        memcpy(buf, data, size);
    }

    *buffer = buf;
    debug_print("[SVGA] Buffer created target=%u size=%u\n", target, size);
    return GFX_OK;
}

static gfx_result_t svga_bind_buffer(gfx_device_t* dev, uint32_t target, void* buffer) {
    (void)target;
    (void)buffer;
    if (!dev) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_buffer_data(gfx_device_t* dev, uint32_t target, size_t size, const void* data) {
    (void)target;
    (void)size;
    if (!dev || !data) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_buffer_sub_data(gfx_device_t* dev, uint32_t target, size_t offset, size_t size, const void* data) {
    (void)target;
    (void)offset;
    (void)size;
    if (!dev || !data) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_destroy_buffer(gfx_device_t* dev, void* buffer) {
    if (!dev || !buffer) {
        return GFX_ERR_INVALID_PARAM;
    }

    kheap_free(buffer);
    debug_print("[SVGA] Buffer destroyed\n");
    return GFX_OK;
}

static gfx_result_t svga_enable_vertex_attrib_array(gfx_device_t* dev, uint32_t index) {
    if (!dev || index >= 16) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_disable_vertex_attrib_array(gfx_device_t* dev, uint32_t index) {
    if (!dev || index >= 16) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_vertex_attrib_pointer(gfx_device_t* dev, uint32_t index, int32_t size, uint32_t type, bool normalized, int32_t stride, const void* pointer) {
    (void)size; (void)type; (void)normalized; (void)stride; (void)pointer;
    if (!dev || index >= 16) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_get_uniform_location(gfx_device_t* dev, void* program, const char* name, int32_t* location) {
    (void)program; (void)name;
    if (!dev || !program || !name || !location) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    *location = 0;
    return GFX_OK;
}

static gfx_result_t svga_uniform1f(gfx_device_t* dev, int32_t location, double value) {
    (void)value;
    if (!dev || location < 0) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_uniform1i(gfx_device_t* dev, int32_t location, int32_t value) {
    (void)value;
    if (!dev || location < 0) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_uniform2f(gfx_device_t* dev, int32_t location, double x, double y) {
    (void)x; (void)y;
    if (!dev || location < 0) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_uniform3f(gfx_device_t* dev, int32_t location, double x, double y, double z) {
    (void)x; (void)y; (void)z;
    if (!dev || location < 0) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_uniform4f(gfx_device_t* dev, int32_t location, double x, double y, double z, double w) {
    (void)x; (void)y; (void)z; (void)w;
    if (!dev || location < 0) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_uniform_matrix4fv(gfx_device_t* dev, int32_t location, bool transpose, const double* value) {
    (void)transpose;
    if (!dev || !value || location < 0) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return GFX_OK;
}

static gfx_result_t svga_draw_arrays(gfx_device_t* dev, uint32_t mode, int32_t first, int32_t count) {
    if (!dev) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    debug_print("[SVGA] Draw arrays: mode=%u, first=%d, count=%d\n", mode, first, count);
    return GFX_OK;
}

static gfx_result_t svga_draw_elements(gfx_device_t* dev, uint32_t mode, int32_t count, uint32_t type, const void* indices) {
    if (!dev || !indices) {
        return GFX_ERR_INVALID_PARAM;
    }

    svga_private_t* priv = (svga_private_t*)dev->driver_data;
    if (!priv || !(priv->capabilities & SVGA_CAP_3D)) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    debug_print("[SVGA] Draw elements: mode=%u, count=%d, type=%u\n", mode, count, type);
    return GFX_OK;
}

/* ============================================================================
 * Module Init/Exit
 * ============================================================================ */

gfx_result_t svga_driver_init(void) {
    debug_print("[SVGA] Registering VMware SVGA driver\n");
    return gfx_register_driver(&svga_gfx_driver);
}

void svga_driver_exit(void) {
    gfx_unregister_driver(&svga_gfx_driver);
}

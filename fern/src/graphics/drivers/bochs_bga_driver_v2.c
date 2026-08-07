/**
 * Fern - Bochs VBE Extensions (BGA) Driver V2
 * 
 * Complete rewrite based on official Bochs VBE Extensions specification.
 * 
 * This driver supports:
 * - Bochs emulator with VBE Extensions
 * - QEMU with -vga std or -vga qxl
 * - VirtualBox (VBE compatible mode)
 * 
 * Features:
 * - Linear framebuffer support (LFB)
 * - Virtual display for hardware scrolling
 * - 8/15/16/24/32 bit color depths
 * - PCI BAR detection for framebuffer address
 * - Fallback to standard BGA addresses
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/pci.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/string.h"

/* Driver-private data structure */
typedef struct {
    uint32_t fb_base;               /* Framebuffer physical address */
    uint32_t mmio_base;             /* MMIO base address (if any) */
    uint16_t bga_version;           /* BGA version ID */
    
    /* Capabilities */
    uint32_t max_width;             /* Maximum resolution */
    uint32_t max_height;
    uint32_t max_bpp;               /* Maximum BPP (usually 32) */
    uint32_t vram_size;             /* Total VRAM in bytes */
    
    /* Current state */
    bool lfb_enabled;               /* Linear framebuffer enabled */
    bool has_pci;                   /* PCI device found */
    
    /* Framebuffer info */
    gfx_framebuffer_t framebuffer;
} bga_private_t;

/* 
 * Calculate proper pitch with alignment
 * CRITICAL FIX: The hardware requires pitch to be aligned (typically to 4 bytes)
 * This is essential for 24bpp modes where width*3 may not be aligned
 */
static inline uint32_t calculate_pitch(uint32_t width, uint32_t bpp) {
    uint32_t bytes_per_pixel = (bpp + 7) / 8;
    uint32_t pitch = width * bytes_per_pixel;
    
    /* Align pitch to 4 bytes for proper memory access */
    pitch = (pitch + 3) & ~3;
    
    return pitch;
}

/* Forward declarations */
static gfx_result_t bga_probe(gfx_device_t* dev);
static gfx_result_t bga_init(gfx_device_t* dev);
static gfx_result_t bga_shutdown(gfx_device_t* dev);
static gfx_result_t bga_reset(gfx_device_t* dev);
static gfx_result_t bga_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t bga_set_mode(gfx_device_t* dev, const gfx_mode_t* mode);
static gfx_result_t bga_get_mode(gfx_device_t* dev, gfx_mode_t* mode);
static gfx_result_t bga_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t bga_unmap_fb(gfx_device_t* dev, gfx_framebuffer_t* fb);
static gfx_result_t bga_set_fb_offset(gfx_device_t* dev, int32_t x, int32_t y);
static gfx_result_t bga_clear(gfx_device_t* dev, gfx_color_t color);
static gfx_result_t bga_wait_vsync(gfx_device_t* dev);
static gfx_result_t bga_flush(gfx_device_t* dev);

/* Driver operations table */
static const gfx_driver_ops_t bga_driver_ops = {
    .name = "bochs-bga",
    .version = 0x00020000,  /* 2.0.0 */
    
    .probe = bga_probe,
    .init = bga_init,
    .shutdown = bga_shutdown,
    .reset = bga_reset,
    
    .get_modes = bga_get_modes,
    .set_mode = bga_set_mode,
    .get_mode = bga_get_mode,
    
    .map_fb = bga_map_fb,
    .unmap_fb = bga_unmap_fb,
    .set_fb_offset = bga_set_fb_offset,
    
    .clear = bga_clear,
    .flush = bga_flush,
    .draw_pixel = NULL,     /* Use software fallback */
    .draw_rect = NULL,      /* Use software fallback */
    .blit = NULL,           /* Use software fallback */
    
    .set_cursor = NULL,     /* BGA doesn't have hardware cursor */
    .move_cursor = NULL,
    .show_cursor = NULL,
    
    .write_char = NULL,     /* Not a text mode driver */
    .set_text_cursor = NULL,
    .scroll = NULL,
    
    .wait_vsync = bga_wait_vsync,
    .flip = NULL,
    
    .read_edid = NULL,      /* BGA doesn't support EDID */
    .detect_displays = NULL,
    
    .set_dpms = NULL,
    .ioctl = NULL,
};

/* Driver instance */
DECLARE_GFX_DRIVER(bga, &bga_driver_ops, GFX_DEVICE_BOCHS_BGA);

/* ============================================================================
 * BGA Register Access
 * ============================================================================ */

/**
 * Read a BGA VBE Dispi register
 */
static uint16_t bga_read_reg(uint16_t index) {
    gfx_outw(VBE_DISPI_IOPORT_INDEX, index);
    return gfx_inw(VBE_DISPI_IOPORT_DATA);
}

/**
 * Write a BGA VBE Dispi register
 */
static void bga_write_reg(uint16_t index, uint16_t value) {
    gfx_outw(VBE_DISPI_IOPORT_INDEX, index);
    gfx_outw(VBE_DISPI_IOPORT_DATA, value);
}

/**
 * Check if BGA hardware is present by probing the ID register
 */
static bool bga_hw_present(void) {
    uint16_t id = bga_read_reg(VBE_DISPI_INDEX_ID);
    
    /* Check for valid BGA ID (B0C0-B0C5) */
    return (id >= VBE_DISPI_ID0 && id <= VBE_DISPI_ID5);
}

/**
 * Get the BGA version from the ID register
 */
static uint16_t bga_get_version(void) {
    /* Write the highest version we support */
    bga_write_reg(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    
    /* Read back what the hardware supports */
    return bga_read_reg(VBE_DISPI_INDEX_ID);
}

/**
 * Query maximum resolution using GETCAPS mode
 */
static void bga_get_capabilities(bga_private_t* priv) {
    /* Save current enable state */
    uint16_t saved_enable = bga_read_reg(VBE_DISPI_INDEX_ENABLE);
    
    /* Enter GETCAPS mode */
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_GETCAPS);
    
    /* Read maximum values */
    priv->max_width = bga_read_reg(VBE_DISPI_INDEX_XRES);
    priv->max_height = bga_read_reg(VBE_DISPI_INDEX_YRES);
    priv->max_bpp = bga_read_reg(VBE_DISPI_INDEX_BPP);
    
    /* Read VRAM size (in 64KB units) */
    if (priv->bga_version >= VBE_DISPI_ID3) {
        uint16_t vram_64k = bga_read_reg(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
        priv->vram_size = (uint32_t)vram_64k * 65536;
    } else {
        /* Default to 16MB for older versions */
        priv->vram_size = 16 * 1024 * 1024;
    }
    
    /* Restore enable state */
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, saved_enable);
    
    /* Sanity check limits */
    if (priv->max_width == 0 || priv->max_width > BGA_MAX_XRES) {
        priv->max_width = 1920;  /* Safe default */
    }
    if (priv->max_height == 0 || priv->max_height > BGA_MAX_YRES) {
        priv->max_height = 1080;  /* Safe default */
    }
    if (priv->max_bpp == 0 || priv->max_bpp > 32) {
        priv->max_bpp = 32;
    }
    if (priv->vram_size == 0) {
        priv->vram_size = 16 * 1024 * 1024;  /* 16MB default */
    }
    
    debug_print("[BGA] Capabilities: max %ux%ux%u, VRAM: %u MB\n",
                priv->max_width, priv->max_height, priv->max_bpp,
                priv->vram_size / (1024 * 1024));
}

/* ============================================================================
 * PCI Detection
 * ============================================================================ */

/**
 * Find the BGA PCI device and get the framebuffer address from BAR 0
 */
static bool bga_find_pci_device(gfx_device_t* dev) {
    /* Scan PCI bus for BGA device */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                
                if (vendor_device == 0xFFFFFFFF) continue;
                
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                /* Check for Bochs BGA or QEMU QXL */
                if ((vendor == BGA_PCI_VENDOR_BOCHS && device == BGA_PCI_DEVICE_BGA) ||
                    (vendor == BGA_PCI_VENDOR_QEMU && device == BGA_PCI_DEVICE_QXL) ||
                    (vendor == VBOX_PCI_VENDOR && device == VBOX_PCI_DEVICE_VESA)) {
                    
                    dev->pci_bus = bus;
                    dev->pci_slot = slot;
                    dev->pci_func = func;
                    dev->vendor_id = vendor;
                    dev->device_id = device;
                    
                    /* Read BAR 0 for framebuffer address */
                    uint32_t bar0 = pci_read_config(bus, slot, func, 0x10);
                    if ((bar0 & 0x1) == 0) {  /* Memory BAR */
                        dev->fb_base = bar0 & 0xFFFFFFF0;
                        
                        /* Try to determine BAR size */
                        pci_write_config(bus, slot, func, 0x10, 0xFFFFFFFF);
                        uint32_t size_mask = pci_read_config(bus, slot, func, 0x10);
                        pci_write_config(bus, slot, func, 0x10, bar0);
                        
                        if (size_mask != 0 && size_mask != 0xFFFFFFFF) {
                            size_mask &= 0xFFFFFFF0;
                            dev->fb_size = (~size_mask) + 1;
                        } else {
                            dev->fb_size = 16 * 1024 * 1024;  /* Default 16MB */
                        }
                    }
                    
                    /* Read BAR 2 for MMIO (if present) */
                    uint32_t bar2 = pci_read_config(bus, slot, func, 0x18);
                    if ((bar2 & 0x1) == 0 && bar2 != 0) {
                        dev->mmio_base = bar2 & 0xFFFFFFF0;
                        dev->mmio_size = 4096;  /* Typical MMIO size */
                    }
                    
                    /* Enable memory and I/O access */
                    uint32_t cmd = pci_read_config(bus, slot, func, 0x04);
                    cmd |= 0x03;  /* I/O + Memory enable */
                    pci_write_config(bus, slot, func, 0x04, cmd);
                    
                    debug_print("[BGA] PCI: %04x:%04x at %02x:%02x.%x, FB @ 0x%08x (%u MB)\n",
                                vendor, device, bus, slot, func,
                                (uint32_t)dev->fb_base, (uint32_t)(dev->fb_size / (1024*1024)));
                    
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

/**
 * Probe for BGA hardware
 */
static gfx_result_t bga_probe(gfx_device_t* dev) {
    /* First check if BGA hardware is present via I/O ports */
    if (!bga_hw_present()) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Allocate private data */
    bga_private_t* priv = (bga_private_t*)kmalloc(sizeof(bga_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(bga_private_t));
    
    /* Get BGA version */
    priv->bga_version = bga_get_version();
    debug_print("[BGA] Detected BGA version: 0x%04x\n", priv->bga_version);
    
    /* Get capabilities */
    bga_get_capabilities(priv);
    
    /* Try to find PCI device for framebuffer address */
    priv->has_pci = bga_find_pci_device(dev);
    
    if (!priv->has_pci) {
        /* Use default framebuffer address */
        dev->fb_base = BGA_LFB_PHYSICAL_ADDRESS;
        dev->fb_size = priv->vram_size;
        debug_print("[BGA] No PCI device found, using default FB @ 0x%08x\n",
                    (uint32_t)dev->fb_base);
    }
    
    /* Fill in device info */
    dev->type = GFX_DEVICE_BOCHS_BGA;
    dev->vram_size = priv->vram_size;
    dev->max_width = priv->max_width;
    dev->max_height = priv->max_height;
    dev->max_bpp = priv->max_bpp;
    dev->driver_data = priv;
    
    /* Set capabilities */
    dev->caps = GFX_CAP_LINEAR_FB;
    /* GFX_CAP_VIRTUAL_FB and GFX_CAP_8BIT_DAC are not defined in current version */
    
    /* Set name */
    const char* name = "Bochs VBE Extensions";
    if (dev->vendor_id == BGA_PCI_VENDOR_QEMU) {
        name = "QEMU QXL Graphics";
    } else if (dev->vendor_id == VBOX_PCI_VENDOR) {
        name = "VirtualBox Graphics";
    }
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    
    debug_print("[BGA] Probe successful: %s\n", dev->name);
    return GFX_OK;
}

/**
 * Initialize the BGA driver
 */
static gfx_result_t bga_init(gfx_device_t* dev) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Disable display during mode set */
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    
    debug_print("[BGA] Initialized successfully\n");
    return GFX_OK;
}

/**
 * Shutdown the BGA driver
 */
static gfx_result_t bga_shutdown(gfx_device_t* dev) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    if (priv) {
        /* Disable VBE mode */
        bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        
        kfree(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

/**
 * Reset the BGA to default state
 */
static gfx_result_t bga_reset(gfx_device_t* dev) {
    (void)dev;
    /* Disable VBE mode */
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    
    /* Reset to default VGA mode would happen here */
    return GFX_OK;
}

/**
 * Get list of supported modes
 */
static gfx_result_t bga_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    /* Standard resolutions to offer */
    static const struct { uint32_t w, h; } resolutions[] = {
        { 640, 480 },
        { 800, 600 },
        { 1024, 768 },
        { 1152, 864 },
        { 1280, 720 },
        { 1280, 800 },
        { 1280, 1024 },
        { 1366, 768 },
        { 1440, 900 },
        { 1600, 900 },
        { 1680, 1050 },
        { 1920, 1080 },
        { 1920, 1200 },
        { 2560, 1440 },
    };
    
    static const uint32_t bpp_values[] = { 32, 24, 16, 15, 8 };
    
    /* Count valid modes */
    uint32_t num_modes = 0;
    for (size_t i = 0; i < sizeof(resolutions)/sizeof(resolutions[0]); i++) {
        if (resolutions[i].w <= priv->max_width && 
            resolutions[i].h <= priv->max_height) {
            for (size_t j = 0; j < sizeof(bpp_values)/sizeof(bpp_values[0]); j++) {
                if (bpp_values[j] <= priv->max_bpp) {
                    /* Check if mode fits in VRAM */
                    uint32_t fb_size = resolutions[i].w * resolutions[i].h * (bpp_values[j] / 8);
                    if (fb_size <= priv->vram_size) {
                        num_modes++;
                    }
                }
            }
        }
    }
    
    if (num_modes == 0) {
        return GFX_ERR_MODE_NOT_FOUND;
    }
    
    /* Allocate mode array */
    gfx_mode_t* mode_list = (gfx_mode_t*)kmalloc(num_modes * sizeof(gfx_mode_t));
    if (!mode_list) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(mode_list, 0, num_modes * sizeof(gfx_mode_t));
    
    /* Fill in modes */
    uint32_t mode_idx = 0;
    for (size_t i = 0; i < sizeof(resolutions)/sizeof(resolutions[0]); i++) {
        if (resolutions[i].w <= priv->max_width && 
            resolutions[i].h <= priv->max_height) {
            for (size_t j = 0; j < sizeof(bpp_values)/sizeof(bpp_values[0]); j++) {
                if (bpp_values[j] <= priv->max_bpp) {
                    uint32_t fb_size = resolutions[i].w * resolutions[i].h * (bpp_values[j] / 8);
                    if (fb_size <= priv->vram_size) {
                        gfx_mode_t* m = &mode_list[mode_idx];
                        m->mode_id = mode_idx;
                        m->width = resolutions[i].w;
                        m->height = resolutions[i].h;
                        m->bpp = bpp_values[j];
                        /* CRITICAL FIX: Use proper pitch calculation with alignment */
                        m->pitch = calculate_pitch(m->width, m->bpp);
                        m->refresh_hz = 60;
                        m->is_text_mode = false;
                        
                        /* Determine pixel format */
                        switch (m->bpp) {
                            case 8:  m->format = GFX_FORMAT_INDEXED_8; break;
                            case 15: m->format = GFX_FORMAT_BGR555; break;
                            case 16: m->format = GFX_FORMAT_BGR565; break;
                            case 24: m->format = GFX_FORMAT_BGR888; break;
                            case 32: m->format = GFX_FORMAT_BGRX8888; break;
                            default: m->format = GFX_FORMAT_UNKNOWN; break;
                        }
                        
                        mode_idx++;
                    }
                }
            }
        }
    }
    
    *modes = mode_list;
    *count = num_modes;
    return GFX_OK;
}

/**
 * Set display mode
 */
static gfx_result_t bga_set_mode(gfx_device_t* dev, const gfx_mode_t* mode) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    if (!mode || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Validate mode parameters */
    if (mode->width == 0 || mode->height == 0) {
        return GFX_ERR_INVALID_PARAM;
    }
    if (mode->width > priv->max_width || mode->height > priv->max_height) {
        debug_print("[BGA] Mode %ux%u exceeds max %ux%u\n",
                    mode->width, mode->height, priv->max_width, priv->max_height);
        return GFX_ERR_INVALID_PARAM;
    }

    if (mode->bpp != 8 && mode->bpp != 15 && mode->bpp != 16 &&
        mode->bpp != 24 && mode->bpp != 32) {
        debug_print("[BGA] Unsupported BPP: %u\n", mode->bpp);
        return GFX_ERR_INVALID_PARAM;
    }

    /* CRITICAL FIX: Calculate pitch and size properly.
     *
     * Defense in depth: do the required-VRAM check in 64-bit arithmetic.
     * calculate_pitch()/`* mode->height` below are only safe from overflow
     * because upstream callers (gfx_set_mode() in graphics_manager_v2.c,
     * sys_set_fb_mode() in syscall.c) now cap width/height at 4096. Compute
     * the check itself without relying on that -- a 32-bit
     * `pitch * height` can still wrap to a small number for sufficiently
     * large inputs, which would silently pass the "fits in VRAM" gate
     * below and then let the hardware get programmed with a resolution far
     * bigger than the mapped framebuffer, causing an out-of-bounds write
     * (page fault) the moment anything draws to it. */
    uint32_t proper_pitch = calculate_pitch(mode->width, mode->bpp);
    uint64_t required_vram_64 = (uint64_t)proper_pitch * (uint64_t)mode->height;
    if (required_vram_64 > (uint64_t)priv->vram_size) {
        debug_print("[BGA] Mode requires more bytes (pitch=%u) than %u available\n",
                    proper_pitch, priv->vram_size);
        return GFX_ERR_NO_MEMORY;
    }
    uint32_t required_vram = (uint32_t)required_vram_64;
    
    debug_print("[BGA] Setting mode: %ux%ux%u\n", mode->width, mode->height, mode->bpp);
    
    /* 
     * Mode setting sequence (from Bochs VBE specification):
     * 1. Disable VBE
     * 2. Set resolution and BPP
     * 3. Enable VBE with LFB
     */
    
    /* Step 1: Disable VBE */
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    
    /* Step 2: Set resolution */
    bga_write_reg(VBE_DISPI_INDEX_XRES, mode->width);
    bga_write_reg(VBE_DISPI_INDEX_YRES, mode->height);
    bga_write_reg(VBE_DISPI_INDEX_BPP, mode->bpp);
    
    /* Set virtual resolution (can be larger for scrolling) */
    bga_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, mode->width);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, mode->height);
    
    /* Reset offsets */
    bga_write_reg(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_Y_OFFSET, 0);
    
    /* Reset bank for banked modes */
    bga_write_reg(VBE_DISPI_INDEX_BANK, 0);
    
    /* Step 3: Enable VBE with LFB */
    uint16_t enable_flags = VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED;
    if (priv->bga_version >= VBE_DISPI_ID4) {
        enable_flags |= VBE_DISPI_8BIT_DAC;  /* Use 8-bit DAC for better colors */
    }
    /* Note: VBE_DISPI_NOCLEARMEM can be added to preserve framebuffer contents */
    
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, enable_flags);
    
    /* Verify mode was set correctly */
    uint16_t actual_enable = bga_read_reg(VBE_DISPI_INDEX_ENABLE);
    if (!(actual_enable & VBE_DISPI_ENABLED)) {
        debug_print("[BGA] Failed to enable VBE mode\n");
        return GFX_ERR_HARDWARE;
    }
    
    /* Update device state with PROPER pitch */
    dev->current_mode = *mode;
    dev->current_mode.pitch = proper_pitch;  /* Use calculated pitch */
    
    /* Update framebuffer info with correct size */
    priv->framebuffer.phys_addr = dev->fb_base;
    priv->framebuffer.width = mode->width;
    priv->framebuffer.height = mode->height;
    priv->framebuffer.bpp = mode->bpp;
    priv->framebuffer.pitch = proper_pitch;  /* Use proper pitch */
    priv->framebuffer.size = required_vram;  /* Use pitch * height */
    priv->framebuffer.format = mode->format;
    priv->framebuffer.virtual_width = mode->width;
    priv->framebuffer.virtual_height = mode->height;
    priv->lfb_enabled = true;
    
    dev->active = true;
    debug_print("[BGA] Mode set successfully, pitch=%u\n", priv->framebuffer.pitch);
    
    return GFX_OK;
}

/**
 * Get current mode
 */
static gfx_result_t bga_get_mode(gfx_device_t* dev, gfx_mode_t* mode) {
    if (!mode) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    *mode = dev->current_mode;
    return GFX_OK;
}

/* Multiboot framebuffer globals set by the kernel before drivers start */
extern void*     g_multiboot_framebuffer;  /* virtual address of bootloader FB */
extern uintptr_t g_multiboot_fb_addr;      /* physical address */
extern uint32_t  g_multiboot_fb_width;
extern uint32_t  g_multiboot_fb_height;
extern uint32_t  g_multiboot_fb_pitch;
extern uint32_t  g_multiboot_fb_bpp;

/**
 * Populate the private framebuffer struct from the multiboot globals.
 * Used as a fallback when the BGA-native mapping path fails.
 */
static void bga_apply_multiboot_fb(bga_private_t* priv) {
    priv->framebuffer.phys_addr      = g_multiboot_fb_addr;
    priv->framebuffer.virt_addr      = g_multiboot_framebuffer;
    priv->framebuffer.width          = g_multiboot_fb_width  ? g_multiboot_fb_width  : 1024;
    priv->framebuffer.height         = g_multiboot_fb_height ? g_multiboot_fb_height : 768;
    priv->framebuffer.bpp            = g_multiboot_fb_bpp    ? g_multiboot_fb_bpp    : 32;
    priv->framebuffer.pitch          = g_multiboot_fb_pitch  ?
                                           g_multiboot_fb_pitch :
                                           calculate_pitch(priv->framebuffer.width,
                                                           priv->framebuffer.bpp);
    priv->framebuffer.size           = (size_t)priv->framebuffer.pitch *
                                           priv->framebuffer.height;
    priv->framebuffer.virtual_width  = priv->framebuffer.width;
    priv->framebuffer.virtual_height = priv->framebuffer.height;

    /* Derive pixel format from bpp */
    switch (priv->framebuffer.bpp) {
        case 8:  priv->framebuffer.format = GFX_FORMAT_INDEXED_8; break;
        case 15: priv->framebuffer.format = GFX_FORMAT_BGR555;    break;
        case 16: priv->framebuffer.format = GFX_FORMAT_BGR565;    break;
        case 24: priv->framebuffer.format = GFX_FORMAT_BGR888;    break;
        default: priv->framebuffer.format = GFX_FORMAT_BGRX8888;  break;
    }

    priv->lfb_enabled = true;
}

/**
 * Map the framebuffer into virtual memory.
 *
 * Primary path: identity-map the BGA VRAM via vmm_identity_map_range /
 *               map_physical_memory.
 *
 * Fallback path: the bootloader already mapped the framebuffer and stored
 *                the virtual pointer in g_multiboot_framebuffer.  Use that
 *                directly so that graphics init can proceed even when the
 *                BGA PCI BAR address is unavailable or already mapped.
 */
static gfx_result_t bga_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;

    if (!fb || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }

    /* Already mapped — nothing to do. */
    if (priv->framebuffer.virt_addr) {
        *fb = &priv->framebuffer;
        return GFX_OK;
    }

    /* ------------------------------------------------------------------ */
    /* Primary path: map the BGA VRAM via the VMM                         */
    /* ------------------------------------------------------------------ */
    bool primary_ok = false;

    if (priv->framebuffer.phys_addr != 0 && priv->framebuffer.size != 0) {
        size_t map_size = (priv->framebuffer.size + 4095) & ~4095u;
        uint32_t map_start = ((uint32_t)priv->framebuffer.phys_addr) & ~0xFFFu;
        uint32_t map_end   = map_start + (uint32_t)map_size;

        if (map_end > map_start) {
            page_directory_t* dir = vmm_get_current_page_directory();
            if (dir) {
                memory_result_t mr = vmm_identity_map_range(
                    dir, map_start, map_end,
                    PAGE_WRITABLE | PAGE_CACHE_DISABLE);
                if (mr == MEMORY_OK) {
                    void* vaddr = map_physical_memory(
                        priv->framebuffer.phys_addr, map_size);
                    if (vaddr) {
                        priv->framebuffer.virt_addr = vaddr;
                        debug_print("[BGA] Mapped FB: phys=0x%08x virt=0x%p size=%u\n",
                                    (uint32_t)priv->framebuffer.phys_addr,
                                    vaddr, (uint32_t)map_size);
                        primary_ok = true;
                    } else {
                        debug_print("[BGA] map_physical_memory failed for 0x%08x\n",
                                    (uint32_t)priv->framebuffer.phys_addr);
                    }
                } else {
                    debug_print("[BGA] vmm_identity_map_range failed "
                                "(0x%08x-0x%08x, err=%d) — trying fallback\n",
                                map_start, map_end, (int)mr);
                }
            } else {
                debug_print("[BGA] No active page directory — trying fallback\n");
            }
        } else {
            debug_print("[BGA] Invalid FB range (start=0x%08x size=%u) — trying fallback\n",
                        map_start, (uint32_t)map_size);
        }
    } else {
        debug_print("[BGA] phys_addr or size not set (phys=0x%08x size=%u) — trying fallback\n",
                    (uint32_t)priv->framebuffer.phys_addr,
                    (uint32_t)priv->framebuffer.size);
    }

    /* ------------------------------------------------------------------ */
    /* Fallback path: reuse the bootloader-mapped multiboot framebuffer   */
    /* ------------------------------------------------------------------ */
    if (!primary_ok) {
        if (g_multiboot_framebuffer != NULL && g_multiboot_fb_addr != 0) {
            debug_print("[BGA] Using multiboot FB fallback: "
                        "virt=0x%p phys=0x%08x %ux%u@%ubpp\n",
                        g_multiboot_framebuffer,
                        (uint32_t)g_multiboot_fb_addr,
                        g_multiboot_fb_width, g_multiboot_fb_height,
                        g_multiboot_fb_bpp);
            bga_apply_multiboot_fb(priv);
        } else {
            debug_print("[BGA] map_fb failed: no BGA mapping and no multiboot FB\n");
            return GFX_ERR_MAPPING_FAILED;
        }
    }

    *fb = &priv->framebuffer;
    return GFX_OK;
}

/**
 * Unmap the framebuffer
 */
static gfx_result_t bga_unmap_fb(gfx_device_t* dev, gfx_framebuffer_t* fb) {
    (void)fb;
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    if (priv && priv->framebuffer.virt_addr) {
        /* Note: In a real kernel we'd unmap here */
        /* For now, just mark as unmapped */
        priv->framebuffer.virt_addr = NULL;
    }
    
    return GFX_OK;
}

/**
 * Set framebuffer display offset (for hardware scrolling)
 */
static gfx_result_t bga_set_fb_offset(gfx_device_t* dev, int32_t x, int32_t y) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    if (!priv || !priv->lfb_enabled) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Clamp offsets */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x > priv->framebuffer.virtual_width - priv->framebuffer.width) {
        x = priv->framebuffer.virtual_width - priv->framebuffer.width;
    }
    if ((uint32_t)y > priv->framebuffer.virtual_height - priv->framebuffer.height) {
        y = priv->framebuffer.virtual_height - priv->framebuffer.height;
    }
    
    bga_write_reg(VBE_DISPI_INDEX_X_OFFSET, (uint16_t)x);
    bga_write_reg(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)y);
    
    priv->framebuffer.x_offset = x;
    priv->framebuffer.y_offset = y;
    
    return GFX_OK;
}

/**
 * Clear the screen to a solid color
 */
static gfx_result_t bga_clear(gfx_device_t* dev, gfx_color_t color) {
    bga_private_t* priv = (bga_private_t*)dev->driver_data;
    
    if (!priv || !priv->framebuffer.virt_addr) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    uint32_t pixel = gfx_color_to_pixel(color, priv->framebuffer.format);
    uint8_t* fb = (uint8_t*)priv->framebuffer.virt_addr;
    uint32_t bytes_per_pixel = priv->framebuffer.bpp / 8;
    
    for (uint32_t y = 0; y < priv->framebuffer.height; y++) {
        uint8_t* row = fb + y * priv->framebuffer.pitch;
        
        switch (bytes_per_pixel) {
            case 1:
                memset(row, pixel & 0xFF, priv->framebuffer.width);
                break;
            case 2:
                for (uint32_t x = 0; x < priv->framebuffer.width; x++) {
                    *(uint16_t*)(row + x * 2) = pixel & 0xFFFF;
                }
                break;
            case 3:
                for (uint32_t x = 0; x < priv->framebuffer.width; x++) {
                    row[x * 3 + 0] = (pixel >> 0) & 0xFF;
                    row[x * 3 + 1] = (pixel >> 8) & 0xFF;
                    row[x * 3 + 2] = (pixel >> 16) & 0xFF;
                }
                break;
            case 4:
                for (uint32_t x = 0; x < priv->framebuffer.width; x++) {
                    *(uint32_t*)(row + x * 4) = pixel;
                }
                break;
        }
    }
    
    return GFX_OK;
}

/**
 * Wait for vertical sync
 * BGA doesn't have true vsync, so we use VGA port polling
 */
static gfx_result_t bga_wait_vsync(gfx_device_t* dev) {
    (void)dev;
    /* Wait for end of current vsync */
    while (gfx_inb(VGA_INPUT_STATUS1_COLOR) & 0x08);
    
    /* Wait for start of vsync */
    while (!(gfx_inb(VGA_INPUT_STATUS1_COLOR) & 0x08));
    
    return GFX_OK;
}

static gfx_result_t bga_flush(gfx_device_t* dev) {
    (void)dev;
    __asm__ volatile("mfence" ::: "memory");
    return GFX_OK;
}

/* ============================================================================
 * Module Init/Exit
 * ============================================================================ */

/**
 * Initialize and register the BGA driver
 */
gfx_result_t bga_driver_init(void) {
    debug_print("[BGA] Registering Bochs VBE Extensions driver\n");
    return gfx_register_driver(&bga_gfx_driver);
}

/**
 * Unregister the BGA driver
 */
void bga_driver_exit(void) {
    gfx_unregister_driver(&bga_gfx_driver);
}

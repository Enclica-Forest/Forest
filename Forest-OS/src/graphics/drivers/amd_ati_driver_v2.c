/**
 * Fern - AMD/ATI Graphics Driver V2
 * 
 * Basic AMD/ATI graphics driver. In protected mode without Atombios
 * support, this driver primarily uses the framebuffer pre-configured
 * by the BIOS or UEFI.
 * 
 * Full AMD GPU support would require:
 * - Atombios command table parsing
 * - Complex register programming
 * - Memory controller setup
 * - Display controller programming
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/pci.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/string.h"
#include <stdio.h>

/* AMD device families */
typedef enum {
    AMD_FAMILY_UNKNOWN = 0,
    AMD_FAMILY_R100,        /* Original Radeon */
    AMD_FAMILY_R200,        /* Radeon 8500-9200 */
    AMD_FAMILY_R300,        /* Radeon 9500-9800, X300-X600 */
    AMD_FAMILY_R420,        /* Radeon X700-X850 */
    AMD_FAMILY_R500,        /* Radeon X1xxx */
    AMD_FAMILY_R600,        /* Radeon HD 2xxx-4xxx */
    AMD_FAMILY_EVERGREEN,   /* Radeon HD 5xxx */
    AMD_FAMILY_NI,          /* Radeon HD 6xxx */
    AMD_FAMILY_SI,          /* Radeon HD 7xxx, R9 2xx */
    AMD_FAMILY_CI,          /* Radeon R7/R9 3xx */
    AMD_FAMILY_VI,          /* Radeon R9 Fury */
    AMD_FAMILY_POLARIS,     /* RX 4xx/5xx */
    AMD_FAMILY_VEGA,        /* Vega series */
    AMD_FAMILY_NAVI,        /* RX 5xxx/6xxx */
} amd_family_t;

/* Driver-private data structure */
typedef struct {
    volatile uint32_t* mmio;
    size_t mmio_size;
    void* fb_virt;
    
    amd_family_t family;
    uint32_t vram_size;
    
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    
    gfx_framebuffer_t framebuffer;
} amd_private_t;

/* Forward declarations */
static gfx_result_t amd_probe(gfx_device_t* dev);
static gfx_result_t amd_init(gfx_device_t* dev);
static gfx_result_t amd_shutdown(gfx_device_t* dev);
static gfx_result_t amd_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t amd_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t amd_clear(gfx_device_t* dev, gfx_color_t color);

/* Driver operations table */
static const gfx_driver_ops_t amd_driver_ops = {
    .name = "amd-ati",
    .version = 0x00020000,
    
    .probe = amd_probe,
    .init = amd_init,
    .shutdown = amd_shutdown,
    .reset = NULL,
    
    .get_modes = amd_get_modes,
    .set_mode = NULL,
    .get_mode = NULL,
    
    .map_fb = amd_map_fb,
    .unmap_fb = NULL,
    .set_fb_offset = NULL,
    
    .clear = amd_clear,
    .draw_pixel = NULL,
    .draw_rect = NULL,
    .blit = NULL,
    
    .set_cursor = NULL,
    .move_cursor = NULL,
    .show_cursor = NULL,
    
    .write_char = NULL,
    .set_text_cursor = NULL,
    .scroll = NULL,
    
    .wait_vsync = NULL,
    .flip = NULL,
    
    .read_edid = NULL,
    .detect_displays = NULL,
    
    .set_dpms = NULL,
    .ioctl = NULL,
};

DECLARE_GFX_DRIVER(amd, &amd_driver_ops, GFX_DEVICE_AMD_ATI);

/* ============================================================================
 * AMD Device Detection
 * ============================================================================ */

/**
 * Identify AMD family from device ID (simplified)
 */
static amd_family_t amd_identify_family(uint16_t device_id) {
    /* Very simplified family detection based on device ID ranges */
    
    /* RX 5xxx/6xxx Navi */
    if ((device_id >= 0x7310 && device_id <= 0x73FF) ||
        (device_id >= 0x7340 && device_id <= 0x74FF)) {
        return AMD_FAMILY_NAVI;
    }
    
    /* Vega */
    if (device_id >= 0x6860 && device_id <= 0x687F) {
        return AMD_FAMILY_VEGA;
    }
    
    /* Polaris RX 4xx/5xx */
    if ((device_id >= 0x67DF && device_id <= 0x67FF) ||
        (device_id >= 0x6980 && device_id <= 0x699F)) {
        return AMD_FAMILY_POLARIS;
    }
    
    /* Volcanic Islands */
    if (device_id >= 0x6900 && device_id <= 0x695F) {
        return AMD_FAMILY_VI;
    }
    
    /* Sea Islands */
    if (device_id >= 0x6600 && device_id <= 0x67FF) {
        return AMD_FAMILY_CI;
    }
    
    /* Southern Islands */
    if (device_id >= 0x6780 && device_id <= 0x6850) {
        return AMD_FAMILY_SI;
    }
    
    /* Northern Islands */
    if (device_id >= 0x6700 && device_id <= 0x677F) {
        return AMD_FAMILY_NI;
    }
    
    /* Evergreen */
    if (device_id >= 0x6880 && device_id <= 0x68FF) {
        return AMD_FAMILY_EVERGREEN;
    }
    
    /* R600-R700 */
    if (device_id >= 0x9400 && device_id <= 0x9FFF) {
        return AMD_FAMILY_R600;
    }
    
    /* Older families */
    if (device_id >= 0x7100 && device_id <= 0x72FF) {
        return AMD_FAMILY_R500;
    }
    
    return AMD_FAMILY_UNKNOWN;
}

static const char* amd_family_name(amd_family_t family) {
    switch (family) {
        case AMD_FAMILY_R100: return "Radeon R100";
        case AMD_FAMILY_R200: return "Radeon R200";
        case AMD_FAMILY_R300: return "Radeon R300";
        case AMD_FAMILY_R420: return "Radeon R420";
        case AMD_FAMILY_R500: return "Radeon R500";
        case AMD_FAMILY_R600: return "Radeon HD 2xxx-4xxx";
        case AMD_FAMILY_EVERGREEN: return "Radeon HD 5xxx";
        case AMD_FAMILY_NI: return "Radeon HD 6xxx";
        case AMD_FAMILY_SI: return "Radeon HD 7xxx";
        case AMD_FAMILY_CI: return "Radeon R7/R9";
        case AMD_FAMILY_VI: return "Radeon R9 Fury";
        case AMD_FAMILY_POLARIS: return "Radeon RX 4xx/5xx";
        case AMD_FAMILY_VEGA: return "Radeon Vega";
        case AMD_FAMILY_NAVI: return "Radeon RX 5xxx/6xxx";
        default: return "Unknown AMD GPU";
    }
}

/* ============================================================================
 * Driver Implementation
 * ============================================================================ */

static gfx_result_t amd_probe(gfx_device_t* dev) {
    bool found = false;
    amd_family_t family = AMD_FAMILY_UNKNOWN;
    
    for (uint32_t bus = 0; bus < 256 && !found; bus++) {
        for (uint32_t slot = 0; slot < 32 && !found; slot++) {
            for (uint32_t func = 0; func < 8 && !found; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                
                if (vendor_device == 0xFFFFFFFF) continue;
                
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                if (vendor == AMD_PCI_VENDOR || vendor == ATI_PCI_VENDOR) {
                    uint32_t class_code = pci_read_config(bus, slot, func, 0x08);
                    uint8_t base_class = (class_code >> 24) & 0xFF;
                    uint8_t sub_class = (class_code >> 16) & 0xFF;
                    
                    if (base_class == 0x03 && (sub_class == 0x00 || sub_class == 0x80)) {
                        dev->pci_bus = bus;
                        dev->pci_slot = slot;
                        dev->pci_func = func;
                        dev->vendor_id = vendor;
                        dev->device_id = device;
                        family = amd_identify_family(device);
                        found = true;
                    }
                }
            }
        }
    }
    
    if (!found) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    amd_private_t* priv = (amd_private_t*)kmalloc(sizeof(amd_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(amd_private_t));
    
    priv->family = family;
    
    debug_print("[AMD] Found %s at %02x:%02x.%x\n",
                amd_family_name(family), dev->pci_bus, dev->pci_slot, dev->pci_func);
    
    /* Read BARs */
    uint32_t bar0 = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10);
    uint32_t bar2 = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x18);
    
    /* BAR 0: VRAM (framebuffer) */
    if ((bar0 & 0x1) == 0) {
        dev->fb_base = bar0 & 0xFFFFFFF0;
        
        pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10, 0xFFFFFFFF);
        uint32_t size_mask = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10);
        pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10, bar0);
        
        size_mask &= 0xFFFFFFF0;
        dev->fb_size = (~size_mask) + 1;
    }
    
    /* BAR 2: MMIO registers */
    if ((bar2 & 0x1) == 0) {
        dev->mmio_base = bar2 & 0xFFFFFFF0;
        dev->mmio_size = 256 * 1024;  /* Typical MMIO size */
    }
    
    /* Enable memory access */
    uint32_t cmd = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x04);
    cmd |= 0x06;
    pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x04, cmd);
    
    debug_print("[AMD] FB @ 0x%08x (%u MB), MMIO @ 0x%08x\n",
                (uint32_t)dev->fb_base, (uint32_t)(dev->fb_size / (1024*1024)),
                (uint32_t)dev->mmio_base);
    
    dev->type = GFX_DEVICE_AMD_ATI;
    dev->vram_size = dev->fb_size;
    dev->max_width = 4096;
    dev->max_height = 4096;
    dev->max_bpp = 32;
    dev->driver_data = priv;
    dev->caps = GFX_CAP_LINEAR_FB;
    
    snprintf(dev->name, sizeof(dev->name), "AMD %s", amd_family_name(family));
    
    debug_print("[AMD] Probe successful: %s\n", dev->name);
    return GFX_OK;
}

static gfx_result_t amd_init(gfx_device_t* dev) {
    amd_private_t* priv = (amd_private_t*)dev->driver_data;
    
    if (!priv) return GFX_ERR_INVALID_PARAM;
    
    /* Map MMIO */
    if (dev->mmio_base) {
        priv->mmio = (volatile uint32_t*)map_physical_memory(dev->mmio_base, dev->mmio_size);
    }
    
    /* Default mode (we rely on BIOS/UEFI setup) */
    priv->width = 1024;
    priv->height = 768;
    priv->bpp = 32;
    /* CRITICAL FIX: Calculate pitch with proper alignment */
    priv->pitch = (priv->width * 4 + 3) & ~3;  /* Align to 4 bytes */
    
    /* Try to read CRTC registers if mapped */
    if (priv->mmio) {
        uint32_t h_total = priv->mmio[RADEON_CRTC_H_TOTAL_DISP / 4];
        uint32_t v_total = priv->mmio[RADEON_CRTC_V_TOTAL_DISP / 4];
        
        uint32_t h_disp = ((h_total >> 16) & 0xFFF) + 1;
        uint32_t v_disp = ((v_total >> 16) & 0xFFF) + 1;
        
        if (h_disp > 0 && h_disp <= 4096 && v_disp > 0 && v_disp <= 4096) {
            priv->width = h_disp;
            priv->height = v_disp;
            
            uint32_t pitch_reg = priv->mmio[RADEON_CRTC_PITCH / 4];
            priv->pitch = (pitch_reg & 0x7FF) * 64;  /* In 64-byte units */
        }
        
        debug_print("[AMD] CRTC reports: %ux%u\n", priv->width, priv->height);
    }
    
    dev->current_mode.width = priv->width;
    dev->current_mode.height = priv->height;
    dev->current_mode.bpp = priv->bpp;
    dev->current_mode.pitch = priv->pitch;
    dev->current_mode.format = GFX_FORMAT_BGRX8888;
    
    priv->framebuffer.phys_addr = dev->fb_base;
    priv->framebuffer.width = priv->width;
    priv->framebuffer.height = priv->height;
    priv->framebuffer.bpp = priv->bpp;
    priv->framebuffer.pitch = priv->pitch;
    priv->framebuffer.size = priv->pitch * priv->height;
    priv->framebuffer.format = GFX_FORMAT_BGRX8888;
    
    dev->fb = &priv->framebuffer;
    dev->active = true;
    
    debug_print("[AMD] Initialized: %ux%ux%u\n", priv->width, priv->height, priv->bpp);
    return GFX_OK;
}

static gfx_result_t amd_shutdown(gfx_device_t* dev) {
    amd_private_t* priv = (amd_private_t*)dev->driver_data;
    
    if (priv) {
        kfree(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t amd_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    gfx_mode_t* mode_list = (gfx_mode_t*)kmalloc(sizeof(gfx_mode_t));
    if (!mode_list) return GFX_ERR_NO_MEMORY;
    
    mode_list[0] = dev->current_mode;
    mode_list[0].mode_id = 0;
    
    *modes = mode_list;
    *count = 1;
    return GFX_OK;
}

static gfx_result_t amd_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    amd_private_t* priv = (amd_private_t*)dev->driver_data;
    
    if (!fb || !priv) return GFX_ERR_INVALID_PARAM;
    
    if (!priv->framebuffer.virt_addr) {
        size_t map_size = priv->framebuffer.size;
        if (map_size < 16 * 1024 * 1024) map_size = 16 * 1024 * 1024;
        
        void* vaddr = map_physical_memory(priv->framebuffer.phys_addr, map_size);
        if (!vaddr) return GFX_ERR_MAPPING_FAILED;
        
        priv->framebuffer.virt_addr = vaddr;
        priv->fb_virt = vaddr;
    }
    
    *fb = &priv->framebuffer;
    return GFX_OK;
}

static gfx_result_t amd_clear(gfx_device_t* dev, gfx_color_t color) {
    amd_private_t* priv = (amd_private_t*)dev->driver_data;
    
    if (!priv || !priv->fb_virt) return GFX_ERR_INVALID_PARAM;
    
    uint32_t pixel = gfx_color_to_pixel(color, priv->framebuffer.format);
    uint32_t* fb = (uint32_t*)priv->fb_virt;
    
    for (uint32_t y = 0; y < priv->height; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)fb + y * priv->pitch);
        for (uint32_t x = 0; x < priv->width; x++) {
            row[x] = pixel;
        }
    }
    
    return GFX_OK;
}

/* Module init/exit */
gfx_result_t amd_driver_init(void) {
    debug_print("[AMD] Registering AMD/ATI driver\n");
    return gfx_register_driver(&amd_gfx_driver);
}

void amd_driver_exit(void) {
    gfx_unregister_driver(&amd_gfx_driver);
}

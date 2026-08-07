/**
 * Fern - NVIDIA Graphics Driver V2
 * 
 * Basic NVIDIA graphics driver based on Nouveau project documentation.
 * 
 * In protected mode without full driver support, this driver primarily
 * uses the framebuffer pre-configured by the BIOS or UEFI.
 * 
 * Full NVIDIA support would require extensive register documentation
 * which is partially available from the Nouveau project.
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/pci.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/libc/stdio.h"

/* NVIDIA architectures */
typedef enum {
    NV_ARCH_UNKNOWN = 0,
    NV_ARCH_NV04,       /* RIVA TNT, TNT2 */
    NV_ARCH_NV10,       /* GeForce 256, GeForce2 */
    NV_ARCH_NV20,       /* GeForce3, GeForce4 Ti */
    NV_ARCH_NV30,       /* GeForce FX */
    NV_ARCH_NV40,       /* GeForce 6/7 */
    NV_ARCH_NV50,       /* GeForce 8/9/100-300 */
    NV_ARCH_FERMI,      /* GeForce 400/500 */
    NV_ARCH_KEPLER,     /* GeForce 600/700 */
    NV_ARCH_MAXWELL,    /* GeForce 900 */
    NV_ARCH_PASCAL,     /* GeForce 10 */
    NV_ARCH_TURING,     /* GeForce 16/20 */
    NV_ARCH_AMPERE,     /* GeForce 30 */
    NV_ARCH_ADA,        /* GeForce 40 */
} nv_arch_t;

/* Driver-private data structure */
typedef struct {
    volatile uint32_t* mmio;
    size_t mmio_size;
    void* fb_virt;
    
    nv_arch_t architecture;
    uint32_t vram_size;
    
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    
    gfx_framebuffer_t framebuffer;
} nv_private_t;

/* Forward declarations */
static gfx_result_t nv_probe(gfx_device_t* dev);
static gfx_result_t nv_init(gfx_device_t* dev);
static gfx_result_t nv_shutdown(gfx_device_t* dev);
static gfx_result_t nv_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t nv_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t nv_clear(gfx_device_t* dev, gfx_color_t color);

static const gfx_driver_ops_t nv_driver_ops = {
    .name = "nvidia",
    .version = 0x00020000,
    
    .probe = nv_probe,
    .init = nv_init,
    .shutdown = nv_shutdown,
    .reset = NULL,
    
    .get_modes = nv_get_modes,
    .set_mode = NULL,
    .get_mode = NULL,
    
    .map_fb = nv_map_fb,
    .unmap_fb = NULL,
    .set_fb_offset = NULL,
    
    .clear = nv_clear,
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

DECLARE_GFX_DRIVER(nv, &nv_driver_ops, GFX_DEVICE_NVIDIA);

/* ============================================================================
 * NVIDIA Device Detection
 * ============================================================================ */

static nv_arch_t nv_identify_arch(uint16_t device_id) {
    /* Device ID format for modern NVIDIA: 0x1xxx-0x2xxx */
    (void)device_id;
    
    /* Ada Lovelace (RTX 40) */
    if (device_id >= 0x2680 && device_id <= 0x28FF) {
        return NV_ARCH_ADA;
    }
    
    /* Ampere (RTX 30) */
    if (device_id >= 0x2200 && device_id <= 0x267F) {
        return NV_ARCH_AMPERE;
    }
    
    /* Turing (RTX 20, GTX 16) */
    if ((device_id >= 0x1E00 && device_id <= 0x1FFF) ||
        (device_id >= 0x2180 && device_id <= 0x21FF)) {
        return NV_ARCH_TURING;
    }
    
    /* Pascal (GTX 10) */
    if (device_id >= 0x1B00 && device_id <= 0x1DFF) {
        return NV_ARCH_PASCAL;
    }
    
    /* Maxwell (GTX 9xx, GTX 750) */
    if ((device_id >= 0x1340 && device_id <= 0x13FF) ||
        (device_id >= 0x1700 && device_id <= 0x17FF)) {
        return NV_ARCH_MAXWELL;
    }
    
    /* Kepler (GTX 6xx/7xx) */
    if ((device_id >= 0x0FC0 && device_id <= 0x0FFF) ||
        (device_id >= 0x1180 && device_id <= 0x11FF) ||
        (device_id >= 0x1280 && device_id <= 0x12FF)) {
        return NV_ARCH_KEPLER;
    }
    
    /* Fermi (GTX 4xx/5xx) */
    if ((device_id >= 0x06C0 && device_id <= 0x06FF) ||
        (device_id >= 0x0DC0 && device_id <= 0x0DFF) ||
        (device_id >= 0x1080 && device_id <= 0x10FF)) {
        return NV_ARCH_FERMI;
    }
    
    /* Tesla (GeForce 8/9/GT 100-300) - NV50 */
    if ((device_id >= 0x0400 && device_id <= 0x04FF) ||
        (device_id >= 0x0600 && device_id <= 0x06BF) ||
        (device_id >= 0x0A00 && device_id <= 0x0AFF)) {
        return NV_ARCH_NV50;
    }
    
    /* GeForce 6/7 - NV40 */
    if ((device_id >= 0x0040 && device_id <= 0x00FF) ||
        (device_id >= 0x0140 && device_id <= 0x01FF) ||
        (device_id >= 0x0220 && device_id <= 0x02FF) ||
        (device_id >= 0x0390 && device_id <= 0x03FF)) {
        return NV_ARCH_NV40;
    }
    
    /* Older architectures */
    if (device_id >= 0x0300 && device_id <= 0x033F) return NV_ARCH_NV30;
    if (device_id >= 0x0200 && device_id <= 0x02FF) return NV_ARCH_NV20;
    if (device_id >= 0x0100 && device_id <= 0x01FF) return NV_ARCH_NV10;
    if (device_id >= 0x0020 && device_id <= 0x003F) return NV_ARCH_NV04;
    
    return NV_ARCH_UNKNOWN;
}

static const char* nv_arch_name(nv_arch_t arch) {
    switch (arch) {
        case NV_ARCH_NV04: return "RIVA TNT/TNT2";
        case NV_ARCH_NV10: return "GeForce 256/2";
        case NV_ARCH_NV20: return "GeForce3/4 Ti";
        case NV_ARCH_NV30: return "GeForce FX";
        case NV_ARCH_NV40: return "GeForce 6/7";
        case NV_ARCH_NV50: return "GeForce 8/9/GT";
        case NV_ARCH_FERMI: return "GeForce 400/500";
        case NV_ARCH_KEPLER: return "GeForce 600/700";
        case NV_ARCH_MAXWELL: return "GeForce 900";
        case NV_ARCH_PASCAL: return "GeForce 10";
        case NV_ARCH_TURING: return "GeForce 16/20";
        case NV_ARCH_AMPERE: return "GeForce 30";
        case NV_ARCH_ADA: return "GeForce 40";
        default: return "Unknown NVIDIA GPU";
    }
}

/* ============================================================================
 * Driver Implementation
 * ============================================================================ */

static gfx_result_t nv_probe(gfx_device_t* dev) {
    bool found = false;
    nv_arch_t arch = NV_ARCH_UNKNOWN;
    
    for (uint32_t bus = 0; bus < 256 && !found; bus++) {
        for (uint32_t slot = 0; slot < 32 && !found; slot++) {
            for (uint32_t func = 0; func < 8 && !found; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                
                if (vendor_device == 0xFFFFFFFF) continue;
                
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                if (vendor == NVIDIA_PCI_VENDOR) {
                    uint32_t class_code = pci_read_config(bus, slot, func, 0x08);
                    uint8_t base_class = (class_code >> 24) & 0xFF;
                    uint8_t sub_class = (class_code >> 16) & 0xFF;
                    
                    if (base_class == 0x03 && (sub_class == 0x00 || sub_class == 0x02)) {
                        dev->pci_bus = bus;
                        dev->pci_slot = slot;
                        dev->pci_func = func;
                        dev->vendor_id = vendor;
                        dev->device_id = device;
                        arch = nv_identify_arch(device);
                        found = true;
                    }
                }
            }
        }
    }
    
    if (!found) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    nv_private_t* priv = (nv_private_t*)kmalloc(sizeof(nv_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(nv_private_t));
    
    priv->architecture = arch;
    
    debug_print("[NVIDIA] Found %s at %02x:%02x.%x (0x%04x)\n",
                nv_arch_name(arch), dev->pci_bus, dev->pci_slot, dev->pci_func,
                dev->device_id);
    
    /* Read BARs */
    uint32_t bar0 = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10);
    uint32_t bar1 = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x14);
    
    /* BAR 0: MMIO registers */
    if ((bar0 & 0x1) == 0) {
        dev->mmio_base = bar0 & 0xFFFFFFF0;
        dev->mmio_size = 16 * 1024 * 1024;  /* 16 MB typical */
    }
    
    /* BAR 1: VRAM (framebuffer) */
    if ((bar1 & 0x1) == 0) {
        dev->fb_base = bar1 & 0xFFFFFFF0;
        
        pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x14, 0xFFFFFFFF);
        uint32_t size_mask = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x14);
        pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x14, bar1);
        
        size_mask &= 0xFFFFFFF0;
        dev->fb_size = (~size_mask) + 1;
    }
    
    /* Enable memory access and bus mastering */
    uint32_t cmd = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x04);
    cmd |= 0x06;
    pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x04, cmd);
    
    debug_print("[NVIDIA] MMIO @ 0x%08x, FB @ 0x%08x (%u MB)\n",
                (uint32_t)dev->mmio_base, (uint32_t)dev->fb_base,
                (uint32_t)(dev->fb_size / (1024*1024)));
    
    dev->type = GFX_DEVICE_NVIDIA;
    dev->vram_size = dev->fb_size;
    dev->max_width = 4096;
    dev->max_height = 4096;
    dev->max_bpp = 32;
    dev->driver_data = priv;
    dev->caps = GFX_CAP_LINEAR_FB;
    
    snprintf(dev->name, sizeof(dev->name), "NVIDIA %s", nv_arch_name(arch));
    
    debug_print("[NVIDIA] Probe successful: %s\n", dev->name);
    return GFX_OK;
}

static gfx_result_t nv_init(gfx_device_t* dev) {
    nv_private_t* priv = (nv_private_t*)dev->driver_data;
    
    if (!priv) return GFX_ERR_INVALID_PARAM;
    
    /* Map MMIO */
    if (dev->mmio_base) {
        priv->mmio = (volatile uint32_t*)map_physical_memory(dev->mmio_base, dev->mmio_size);
    }
    
    /* Default mode */
    priv->width = 1024;
    priv->height = 768;
    priv->bpp = 32;
    priv->pitch = priv->width * 4;
    
    /* Try to read CRTC registers for actual mode */
    if (priv->mmio) {
        /* Read PCRTC start address to verify FB location */
        uint32_t crtc_start = NV_RD32((uintptr_t)priv->mmio, NV_PCRTC_START);
        
        /* Try to get framebuffer config */
        uint32_t fb_cfg = NV_RD32((uintptr_t)priv->mmio, NV_PFB_CFG0);
        
        debug_print("[NVIDIA] CRTC start: 0x%08x, FB cfg: 0x%08x\n", crtc_start, fb_cfg);
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
    
    debug_print("[NVIDIA] Initialized: %ux%ux%u\n", priv->width, priv->height, priv->bpp);
    return GFX_OK;
}

static gfx_result_t nv_shutdown(gfx_device_t* dev) {
    nv_private_t* priv = (nv_private_t*)dev->driver_data;
    
    if (priv) {
        kfree(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t nv_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    gfx_mode_t* mode_list = (gfx_mode_t*)kmalloc(sizeof(gfx_mode_t));
    if (!mode_list) return GFX_ERR_NO_MEMORY;
    
    mode_list[0] = dev->current_mode;
    mode_list[0].mode_id = 0;
    
    *modes = mode_list;
    *count = 1;
    return GFX_OK;
}

static gfx_result_t nv_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    nv_private_t* priv = (nv_private_t*)dev->driver_data;
    
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

static gfx_result_t nv_clear(gfx_device_t* dev, gfx_color_t color) {
    nv_private_t* priv = (nv_private_t*)dev->driver_data;
    
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
gfx_result_t nv_driver_init(void) {
    debug_print("[NVIDIA] Registering NVIDIA driver\n");
    return gfx_register_driver(&nv_gfx_driver);
}

void nv_driver_exit(void) {
    gfx_unregister_driver(&nv_gfx_driver);
}

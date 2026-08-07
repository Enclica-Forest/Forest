/**
 * Fern - Intel HD Graphics Driver V2
 * 
 * Basic Intel integrated graphics driver based on Intel Open Source
 * Graphics drivers documentation.
 * 
 * This driver provides:
 * - PCI device detection for Intel HD Graphics
 * - GMBUS/I2C for EDID reading
 * - Basic framebuffer mode using pre-configured GOP/VBIOS mode
 * - Display pipe configuration
 * 
 * Note: Full mode setting requires extensive hardware knowledge and is
 * complex. This driver primarily uses the mode configured by the BIOS/UEFI.
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/pci.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/string.h"
#include "../include/timer.h"
#include <stdio.h>

/* Intel generation detection */
typedef enum {
    INTEL_GEN_UNKNOWN = 0,
    INTEL_GEN_2,        /* i830, i855 */
    INTEL_GEN_3,        /* i915, i945 */
    INTEL_GEN_4,        /* i965, G35, G45 */
    INTEL_GEN_5,        /* Ironlake */
    INTEL_GEN_6,        /* Sandy Bridge */
    INTEL_GEN_7,        /* Ivy Bridge, Haswell */
    INTEL_GEN_8,        /* Broadwell */
    INTEL_GEN_9,        /* Skylake, Kaby Lake, Coffee Lake */
    INTEL_GEN_11,       /* Ice Lake */
    INTEL_GEN_12,       /* Tiger Lake, Alder Lake */
} intel_gen_t;

/* Driver-private data structure */
typedef struct {
    /* Hardware resources */
    volatile uint32_t* mmio;        /* MMIO registers */
    size_t mmio_size;
    uintptr_t gtt_base;             /* Graphics Translation Table */
    size_t gtt_size;
    void* fb_virt;                  /* Mapped framebuffer */
    
    /* Device info */
    intel_gen_t generation;
    bool is_mobile;
    uint32_t vram_size;
    
    /* Current mode */
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uintptr_t fb_phys;
    
    /* EDID */
    gfx_edid_t edid;
    bool has_edid;
    
    /* Framebuffer tracking */
    gfx_framebuffer_t framebuffer;
} intel_private_t;

/* Forward declarations */
static gfx_result_t intel_probe(gfx_device_t* dev);
static gfx_result_t intel_init(gfx_device_t* dev);
static gfx_result_t intel_shutdown(gfx_device_t* dev);
static gfx_result_t intel_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t intel_set_mode(gfx_device_t* dev, const gfx_mode_t* mode);
static gfx_result_t intel_get_mode(gfx_device_t* dev, gfx_mode_t* mode);
static gfx_result_t intel_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t intel_read_edid(gfx_device_t* dev, gfx_edid_t* edid);
static gfx_result_t intel_clear(gfx_device_t* dev, gfx_color_t color);

/* Driver operations table */
static const gfx_driver_ops_t intel_driver_ops = {
    .name = "intel-hd",
    .version = 0x00020000,
    
    .probe = intel_probe,
    .init = intel_init,
    .shutdown = intel_shutdown,
    .reset = NULL,
    
    .get_modes = intel_get_modes,
    .set_mode = intel_set_mode,
    .get_mode = intel_get_mode,
    
    .map_fb = intel_map_fb,
    .unmap_fb = NULL,
    .set_fb_offset = NULL,
    
    .clear = intel_clear,
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
    
    .read_edid = intel_read_edid,
    .detect_displays = NULL,
    
    .set_dpms = NULL,
    .ioctl = NULL,
};

/* Driver instance */
DECLARE_GFX_DRIVER(intel, &intel_driver_ops, GFX_DEVICE_INTEL_HD);

/* ============================================================================
 * Intel Device Detection
 * ============================================================================ */

/* Intel graphics device IDs (partial list - there are hundreds) */
static const struct {
    uint16_t device_id;
    intel_gen_t gen;
    const char* name;
} intel_devices[] = {
    /* Gen 3 */
    { 0x2582, INTEL_GEN_3, "915G" },
    { 0x2592, INTEL_GEN_3, "915GM" },
    { 0x2772, INTEL_GEN_3, "945G" },
    { 0x27A2, INTEL_GEN_3, "945GM" },
    { 0x27AE, INTEL_GEN_3, "945GME" },
    
    /* Gen 4 */
    { 0x2972, INTEL_GEN_4, "946GZ" },
    { 0x2982, INTEL_GEN_4, "G35" },
    { 0x2992, INTEL_GEN_4, "Q965" },
    { 0x29A2, INTEL_GEN_4, "G965" },
    { 0x2A02, INTEL_GEN_4, "GM965" },
    { 0x2A12, INTEL_GEN_4, "GME965" },
    { 0x2E02, INTEL_GEN_4, "4 Series" },
    { 0x2E12, INTEL_GEN_4, "Q45" },
    { 0x2E22, INTEL_GEN_4, "G45" },
    { 0x2E32, INTEL_GEN_4, "G41" },
    { 0x2E42, INTEL_GEN_4, "B43" },
    
    /* Gen 5 (Ironlake) */
    { 0x0042, INTEL_GEN_5, "HD Graphics" },
    { 0x0046, INTEL_GEN_5, "HD Graphics (Mobile)" },
    
    /* Gen 6 (Sandy Bridge) */
    { 0x0102, INTEL_GEN_6, "HD Graphics 2000" },
    { 0x0112, INTEL_GEN_6, "HD Graphics 3000" },
    { 0x0122, INTEL_GEN_6, "HD Graphics 3000" },
    { 0x0106, INTEL_GEN_6, "HD Graphics 2000 (Mobile)" },
    { 0x0116, INTEL_GEN_6, "HD Graphics 3000 (Mobile)" },
    { 0x0126, INTEL_GEN_6, "HD Graphics 3000 (Mobile)" },
    
    /* Gen 7 (Ivy Bridge) */
    { 0x0152, INTEL_GEN_7, "HD Graphics 2500" },
    { 0x0162, INTEL_GEN_7, "HD Graphics 4000" },
    { 0x0156, INTEL_GEN_7, "HD Graphics 2500 (Mobile)" },
    { 0x0166, INTEL_GEN_7, "HD Graphics 4000 (Mobile)" },
    
    /* Gen 7.5 (Haswell) */
    { 0x0402, INTEL_GEN_7, "HD Graphics (Haswell)" },
    { 0x0412, INTEL_GEN_7, "HD Graphics 4600" },
    { 0x0422, INTEL_GEN_7, "HD Graphics 5000" },
    { 0x0406, INTEL_GEN_7, "HD Graphics (Haswell Mobile)" },
    { 0x0416, INTEL_GEN_7, "HD Graphics 4600 (Mobile)" },
    { 0x0426, INTEL_GEN_7, "HD Graphics 5000 (Mobile)" },
    
    /* Gen 8 (Broadwell) */
    { 0x1602, INTEL_GEN_8, "HD Graphics (Broadwell)" },
    { 0x1612, INTEL_GEN_8, "HD Graphics 5600" },
    { 0x1622, INTEL_GEN_8, "Iris Pro 6200" },
    { 0x1606, INTEL_GEN_8, "HD Graphics (Broadwell Mobile)" },
    { 0x1616, INTEL_GEN_8, "HD Graphics 5500" },
    { 0x1626, INTEL_GEN_8, "HD Graphics 6000" },
    
    /* Gen 9 (Skylake) */
    { 0x1902, INTEL_GEN_9, "HD Graphics 510" },
    { 0x1912, INTEL_GEN_9, "HD Graphics 530" },
    { 0x1916, INTEL_GEN_9, "HD Graphics 520" },
    { 0x191B, INTEL_GEN_9, "HD Graphics 530" },
    { 0x191D, INTEL_GEN_9, "HD Graphics P530" },
    { 0x191E, INTEL_GEN_9, "HD Graphics 515" },
    { 0x1926, INTEL_GEN_9, "Iris Graphics 540" },
    { 0x1927, INTEL_GEN_9, "Iris Graphics 550" },
    { 0x192B, INTEL_GEN_9, "Iris Graphics 555" },
    
    /* Gen 9.5 (Kaby Lake, Coffee Lake) */
    { 0x5902, INTEL_GEN_9, "HD Graphics 610" },
    { 0x5912, INTEL_GEN_9, "HD Graphics 630" },
    { 0x5916, INTEL_GEN_9, "HD Graphics 620" },
    { 0x591B, INTEL_GEN_9, "HD Graphics 630" },
    { 0x591E, INTEL_GEN_9, "HD Graphics 615" },
    { 0x5926, INTEL_GEN_9, "Iris Plus 640" },
    { 0x5927, INTEL_GEN_9, "Iris Plus 650" },
    { 0x3E92, INTEL_GEN_9, "UHD Graphics 630" },
    { 0x3E91, INTEL_GEN_9, "UHD Graphics 630" },
    
    { 0, INTEL_GEN_UNKNOWN, NULL }
};

/**
 * Identify Intel device generation and name
 */
static bool intel_identify_device(uint16_t device_id, intel_gen_t* gen, const char** name) {
    for (int i = 0; intel_devices[i].name != NULL; i++) {
        if (intel_devices[i].device_id == device_id) {
            *gen = intel_devices[i].gen;
            *name = intel_devices[i].name;
            return true;
        }
    }
    
    /* Unknown device - try to guess generation from device ID patterns */
    if ((device_id & 0xFF00) == 0x0100) {
        *gen = INTEL_GEN_6;  /* Sandy Bridge era */
        *name = "Unknown HD Graphics";
        return true;
    } else if ((device_id & 0xFF00) == 0x0400 || (device_id & 0xFF00) == 0x0A00) {
        *gen = INTEL_GEN_7;  /* Haswell era */
        *name = "Unknown HD Graphics";
        return true;
    } else if ((device_id & 0xFF00) == 0x1600) {
        *gen = INTEL_GEN_8;  /* Broadwell */
        *name = "Unknown HD Graphics";
        return true;
    } else if ((device_id & 0xFF00) == 0x1900 || (device_id & 0xFF00) == 0x5900 ||
               (device_id & 0xFF00) == 0x3E00) {
        *gen = INTEL_GEN_9;  /* Skylake/Kaby Lake/Coffee Lake */
        *name = "Unknown HD Graphics";
        return true;
    }
    
    return false;
}

/* ============================================================================
 * MMIO Register Access
 * ============================================================================ */

static inline uint32_t intel_read32(intel_private_t* priv, uint32_t reg) {
    return priv->mmio[reg / 4];
}

static inline void intel_write32(intel_private_t* priv, uint32_t reg, uint32_t val) {
    priv->mmio[reg / 4] = val;
    (void)priv->mmio[reg / 4];  /* Posting read */
}

/* ============================================================================
 * GMBUS (I2C) Interface for EDID
 * ============================================================================ */

/**
 * Wait for GMBUS to be ready
 */
static bool intel_gmbus_wait(intel_private_t* priv, uint32_t flag, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 10; i++) {
        uint32_t status = intel_read32(priv, INTEL_GMBUS2);
        
        if (status & INTEL_GMBUS_NAK) {
            /* Clear NAK and return failure */
            intel_write32(priv, INTEL_GMBUS1, INTEL_GMBUS_SW_CLR_INT);
            intel_write32(priv, INTEL_GMBUS1, 0);
            return false;
        }
        
        if (status & flag) {
            return true;
        }
        
        /* Small delay */
        for (volatile int j = 0; j < 1000; j++);
    }
    
    return false;
}

/**
 * Read EDID using GMBUS I2C
 */
static bool intel_read_edid_gmbus(intel_private_t* priv, uint8_t port, gfx_edid_t* edid) {
    /* Initialize GMBUS */
    intel_write32(priv, INTEL_GMBUS0, port);
    
    /* Set up read from DDC address 0x50, offset 0 */
    intel_write32(priv, INTEL_GMBUS1, 
                  INTEL_GMBUS_SW_RDY | 
                  INTEL_GMBUS_CYCLE_INDEX |
                  INTEL_GMBUS_BYTE_COUNT(1) |
                  INTEL_GMBUS_SLAVE_ADDR(0x50) |
                  INTEL_GMBUS_WRITE);
    
    /* Write offset byte (0) */
    intel_write32(priv, INTEL_GMBUS3, 0);
    
    /* Wait for hardware ready */
    if (!intel_gmbus_wait(priv, INTEL_GMBUS_HW_RDY, 50)) {
        debug_print("[Intel] GMBUS: Write timeout\n");
        return false;
    }
    
    /* Set up read of 128 bytes */
    intel_write32(priv, INTEL_GMBUS1,
                  INTEL_GMBUS_SW_RDY |
                  INTEL_GMBUS_CYCLE_STOP |
                  INTEL_GMBUS_BYTE_COUNT(128) |
                  INTEL_GMBUS_SLAVE_ADDR(0x50) |
                  INTEL_GMBUS_READ);
    
    /* Read EDID data */
    for (int i = 0; i < 128; i += 4) {
        if (!intel_gmbus_wait(priv, INTEL_GMBUS_HW_RDY, 50)) {
            debug_print("[Intel] GMBUS: Read timeout at byte %d\n", i);
            return false;
        }
        
        uint32_t data = intel_read32(priv, INTEL_GMBUS3);
        edid->raw[i + 0] = (data >> 0) & 0xFF;
        edid->raw[i + 1] = (data >> 8) & 0xFF;
        edid->raw[i + 2] = (data >> 16) & 0xFF;
        edid->raw[i + 3] = (data >> 24) & 0xFF;
    }
    
    /* Wait for transfer complete */
    intel_gmbus_wait(priv, INTEL_GMBUS_HW_WAIT, 50);
    
    /* Disable GMBUS */
    intel_write32(priv, INTEL_GMBUS0, 0);
    
    /* Validate EDID header */
    static const uint8_t edid_header[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (memcmp(edid->raw, edid_header, 8) != 0) {
        return false;
    }
    
    /* Calculate checksum */
    uint8_t checksum = 0;
    for (int i = 0; i < 128; i++) {
        checksum += edid->raw[i];
    }
    if (checksum != 0) {
        debug_print("[Intel] EDID checksum failed\n");
        return false;
    }
    
    edid->valid = true;
    return true;
}

/**
 * Parse EDID data
 */
static void intel_parse_edid(gfx_edid_t* edid) {
    if (!edid->valid) return;
    
    /* Manufacturer ID (3 5-bit codes packed in 2 bytes) */
    uint16_t mfg = (edid->raw[8] << 8) | edid->raw[9];
    edid->manufacturer[0] = ((mfg >> 10) & 0x1F) + 'A' - 1;
    edid->manufacturer[1] = ((mfg >> 5) & 0x1F) + 'A' - 1;
    edid->manufacturer[2] = (mfg & 0x1F) + 'A' - 1;
    edid->manufacturer[3] = '\0';
    
    /* Product code */
    edid->product_code = edid->raw[10] | (edid->raw[11] << 8);
    
    /* Serial number */
    edid->serial = edid->raw[12] | (edid->raw[13] << 8) | 
                   (edid->raw[14] << 16) | (edid->raw[15] << 24);
    
    /* Week/year of manufacture */
    edid->week = edid->raw[16];
    edid->year = edid->raw[17];
    
    /* Look for preferred timing in detailed timing descriptors */
    for (int i = 0; i < 4; i++) {
        uint8_t* desc = &edid->raw[54 + i * 18];
        
        /* Check if it's a timing descriptor (not a display descriptor) */
        if (desc[0] != 0 || desc[1] != 0) {
            /* Pixel clock in 10 kHz units */
            uint32_t pixel_clock = desc[0] | (desc[1] << 8);
            if (pixel_clock > 0) {
                uint32_t h_active = desc[2] | ((desc[4] & 0xF0) << 4);
                uint32_t v_active = desc[5] | ((desc[7] & 0xF0) << 4);
                
                if (i == 0) {
                    edid->preferred_width = h_active;
                    edid->preferred_height = v_active;
                }
                
                if (h_active > edid->max_width) {
                    edid->max_width = h_active;
                }
                if (v_active > edid->max_height) {
                    edid->max_height = v_active;
                }
            }
        } else if (desc[3] == 0xFC) {
            /* Monitor name descriptor */
            for (int j = 0; j < 13; j++) {
                char c = desc[5 + j];
                if (c == '\n' || c == '\0') break;
                edid->monitor_name[j] = c;
            }
        }
    }
    
    debug_print("[Intel] EDID: %s %04X, Preferred: %ux%u\n",
                edid->manufacturer, edid->product_code,
                edid->preferred_width, edid->preferred_height);
}

/* ============================================================================
 * Display Pipe Reading (get current mode)
 * ============================================================================ */

/**
 * Read current display configuration from Pipe A
 */
static void intel_read_current_mode(intel_private_t* priv) {
    /* Read pipe configuration */
    uint32_t pipe_conf = intel_read32(priv, INTEL_PIPEACONF);
    
    if (!(pipe_conf & INTEL_PIPE_ENABLE)) {
        /* Pipe A not enabled, try Pipe B */
        pipe_conf = intel_read32(priv, INTEL_PIPEBCONF);
        if (!(pipe_conf & INTEL_PIPE_ENABLE)) {
            debug_print("[Intel] No active display pipe found\n");
            return;
        }
        debug_print("[Intel] Using Pipe B\n");
    }
    
    /* Read timing registers (Pipe A) */
    uint32_t htotal = intel_read32(priv, INTEL_HTOTAL_A);
    uint32_t vtotal = intel_read32(priv, INTEL_VTOTAL_A);
    uint32_t pipesrc = intel_read32(priv, INTEL_PIPEASRC);
    
    /* Extract active resolution */
    priv->width = (pipesrc >> 16) + 1;
    priv->height = (pipesrc & 0xFFFF) + 1;
    
    /* Read display plane configuration */
    uint32_t dspcntr = intel_read32(priv, INTEL_DSPACNTR);
    
    if (dspcntr & INTEL_DSPCNTR_ENABLE) {
        /* Get pixel format */
        uint32_t format = dspcntr & INTEL_DSPCNTR_FORMAT_MASK;
        
        if (format == INTEL_DSPCNTR_FORMAT_BGRX8888 ||
            format == INTEL_DSPCNTR_FORMAT_RGBX8888) {
            priv->bpp = 32;
        } else {
            priv->bpp = 32;  /* Default assumption */
        }
        
        /* Read stride/pitch */
        priv->pitch = intel_read32(priv, INTEL_DSPASTRIDE);
        
        /* Read surface base address */
        priv->fb_phys = intel_read32(priv, INTEL_DSPASURF);
    }
    
    debug_print("[Intel] Current mode: %ux%ux%u pitch=%u\n",
                priv->width, priv->height, priv->bpp, priv->pitch);
}

/* ============================================================================
 * Driver Implementation
 * ============================================================================ */

static gfx_result_t intel_probe(gfx_device_t* dev) {
    /* Scan for Intel graphics device */
    bool found = false;
    intel_gen_t gen = INTEL_GEN_UNKNOWN;
    const char* name = NULL;
    
    for (uint32_t bus = 0; bus < 256 && !found; bus++) {
        for (uint32_t slot = 0; slot < 32 && !found; slot++) {
            for (uint32_t func = 0; func < 8 && !found; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                
                if (vendor_device == 0xFFFFFFFF) continue;
                
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                if (vendor == INTEL_PCI_VENDOR) {
                    /* Check device class (VGA compatible controller) */
                    uint32_t class_code = pci_read_config(bus, slot, func, 0x08);
                    uint8_t base_class = (class_code >> 24) & 0xFF;
                    uint8_t sub_class = (class_code >> 16) & 0xFF;
                    
                    if (base_class == 0x03 && (sub_class == 0x00 || sub_class == 0x80)) {
                        if (intel_identify_device(device, &gen, &name)) {
                            dev->pci_bus = bus;
                            dev->pci_slot = slot;
                            dev->pci_func = func;
                            dev->vendor_id = vendor;
                            dev->device_id = device;
                            found = true;
                        }
                    }
                }
            }
        }
    }
    
    if (!found) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Allocate private data */
    intel_private_t* priv = (intel_private_t*)kmalloc(sizeof(intel_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(intel_private_t));
    
    priv->generation = gen;
    
    debug_print("[Intel] Found %s (Gen %d) at %02x:%02x.%x\n",
                name, gen, dev->pci_bus, dev->pci_slot, dev->pci_func);
    
    /* Read BARs */
    uint32_t bar0 = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x10);
    uint32_t bar2 = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x18);
    
    /* BAR 0: MMIO registers */
    if ((bar0 & 0x1) == 0) {
        dev->mmio_base = bar0 & 0xFFFFFFF0;
        dev->mmio_size = INTEL_MMIO_SIZE;
    }
    
    /* BAR 2: Aperture (GMADR) - framebuffer access */
    if ((bar2 & 0x1) == 0) {
        dev->fb_base = bar2 & 0xFFFFFFF0;
        
        /* Determine aperture size from BAR */
        pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x18, 0xFFFFFFFF);
        uint32_t size_mask = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x18);
        pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x18, bar2);
        
        size_mask &= 0xFFFFFFF0;
        dev->fb_size = (~size_mask) + 1;
    }
    
    /* Enable bus mastering and memory */
    uint32_t cmd = pci_read_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x04);
    cmd |= 0x06;  /* Memory + Bus Master */
    pci_write_config(dev->pci_bus, dev->pci_slot, dev->pci_func, 0x04, cmd);
    
    debug_print("[Intel] MMIO @ 0x%08x, Aperture @ 0x%08x (%u MB)\n",
                (uint32_t)dev->mmio_base, (uint32_t)dev->fb_base,
                (uint32_t)(dev->fb_size / (1024*1024)));
    
    /* Fill in device info */
    dev->type = GFX_DEVICE_INTEL_HD;
    dev->vram_size = dev->fb_size;
    dev->max_width = 4096;
    dev->max_height = 4096;
    dev->max_bpp = 32;
    dev->driver_data = priv;
    
    /* Set capabilities */
    dev->caps = GFX_CAP_LINEAR_FB | GFX_CAP_EDID;
    
    snprintf(dev->name, sizeof(dev->name), "Intel %s", name);
    
    debug_print("[Intel] Probe successful: %s\n", dev->name);
    return GFX_OK;
}

static gfx_result_t intel_init(gfx_device_t* dev) {
    intel_private_t* priv = (intel_private_t*)dev->driver_data;
    
    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* Map MMIO registers */
    priv->mmio = (volatile uint32_t*)map_physical_memory(dev->mmio_base, dev->mmio_size);
    if (!priv->mmio) {
        debug_print("[Intel] Failed to map MMIO\n");
        return GFX_ERR_MAPPING_FAILED;
    }
    
    /* Try to read EDID */
    static const uint8_t gmbus_ports[] = {
        INTEL_GMBUS_PORT_VGADDC,
        INTEL_GMBUS_PORT_PANEL,
        INTEL_GMBUS_PORT_DPCTRL,
    };
    
    for (size_t i = 0; i < sizeof(gmbus_ports); i++) {
        if (intel_read_edid_gmbus(priv, gmbus_ports[i], &priv->edid)) {
            priv->has_edid = true;
            intel_parse_edid(&priv->edid);
            break;
        }
    }
    
    /* Read current display mode */
    intel_read_current_mode(priv);
    
    /* If no mode detected, use EDID preferred or defaults */
    if (priv->width == 0 || priv->height == 0) {
        if (priv->has_edid && priv->edid.preferred_width > 0) {
            priv->width = priv->edid.preferred_width;
            priv->height = priv->edid.preferred_height;
        } else {
            priv->width = 1024;
            priv->height = 768;
        }
        priv->bpp = 32;
        /* CRITICAL FIX: Calculate pitch with proper alignment */
        priv->pitch = (priv->width * 4 + 3) & ~3;  /* Align to 4 bytes */
    }
    
    /* Set up device mode */
    dev->current_mode.width = priv->width;
    dev->current_mode.height = priv->height;
    dev->current_mode.bpp = priv->bpp;
    dev->current_mode.pitch = priv->pitch;
    dev->current_mode.format = GFX_FORMAT_BGRX8888;
    
    /* Set up framebuffer info */
    priv->framebuffer.phys_addr = priv->fb_phys ? priv->fb_phys : dev->fb_base;
    priv->framebuffer.width = priv->width;
    priv->framebuffer.height = priv->height;
    priv->framebuffer.bpp = priv->bpp;
    priv->framebuffer.pitch = priv->pitch;
    priv->framebuffer.size = priv->pitch * priv->height;
    priv->framebuffer.format = GFX_FORMAT_BGRX8888;
    
    dev->fb = &priv->framebuffer;
    dev->active = true;
    
    debug_print("[Intel] Initialized: %ux%ux%u\n", priv->width, priv->height, priv->bpp);
    
    return GFX_OK;
}

static gfx_result_t intel_shutdown(gfx_device_t* dev) {
    intel_private_t* priv = (intel_private_t*)dev->driver_data;
    
    if (priv) {
        kfree(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t intel_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    intel_private_t* priv = (intel_private_t*)dev->driver_data;
    
    /* Return current mode (we can't easily change modes) */
    gfx_mode_t* mode_list = (gfx_mode_t*)kmalloc(sizeof(gfx_mode_t));
    if (!mode_list) {
        return GFX_ERR_NO_MEMORY;
    }
    
    mode_list[0] = dev->current_mode;
    mode_list[0].mode_id = 0;
    
    *modes = mode_list;
    *count = 1;
    
    return GFX_OK;
}

static gfx_result_t intel_set_mode(gfx_device_t* dev, const gfx_mode_t* mode) {
    /* In protected mode without full driver support, we can't change modes */
    /* Just verify the requested mode matches current */
    if (mode->width != dev->current_mode.width ||
        mode->height != dev->current_mode.height) {
        debug_print("[Intel] Mode change not supported in basic driver\n");
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    return GFX_OK;
}

static gfx_result_t intel_get_mode(gfx_device_t* dev, gfx_mode_t* mode) {
    if (!mode) return GFX_ERR_INVALID_PARAM;
    *mode = dev->current_mode;
    return GFX_OK;
}

static gfx_result_t intel_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    intel_private_t* priv = (intel_private_t*)dev->driver_data;
    
    if (!fb || !priv) return GFX_ERR_INVALID_PARAM;
    
    if (!priv->framebuffer.virt_addr) {
        size_t map_size = (priv->framebuffer.size + 4095) & ~4095;
        if (map_size < 16 * 1024 * 1024) {
            map_size = 16 * 1024 * 1024;  /* Map at least 16MB */
        }
        
        void* vaddr = map_physical_memory(priv->framebuffer.phys_addr, map_size);
        if (!vaddr) {
            debug_print("[Intel] Failed to map framebuffer\n");
            return GFX_ERR_MAPPING_FAILED;
        }
        
        priv->framebuffer.virt_addr = vaddr;
        priv->fb_virt = vaddr;
        
        debug_print("[Intel] Mapped framebuffer: 0x%08x -> %p\n",
                    (uint32_t)priv->framebuffer.phys_addr, vaddr);
    }
    
    *fb = &priv->framebuffer;
    return GFX_OK;
}

static gfx_result_t intel_read_edid(gfx_device_t* dev, gfx_edid_t* edid) {
    intel_private_t* priv = (intel_private_t*)dev->driver_data;
    
    if (!edid || !priv) return GFX_ERR_INVALID_PARAM;
    
    if (priv->has_edid) {
        *edid = priv->edid;
        return GFX_OK;
    }
    
    return GFX_ERR_NOT_SUPPORTED;
}

static gfx_result_t intel_clear(gfx_device_t* dev, gfx_color_t color) {
    intel_private_t* priv = (intel_private_t*)dev->driver_data;
    
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

/* ============================================================================
 * Module Init/Exit
 * ============================================================================ */

gfx_result_t intel_driver_init(void) {
    debug_print("[Intel] Registering Intel HD Graphics driver\n");
    return gfx_register_driver(&intel_gfx_driver);
}

void intel_driver_exit(void) {
    gfx_unregister_driver(&intel_gfx_driver);
}

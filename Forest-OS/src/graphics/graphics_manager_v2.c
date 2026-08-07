/**
 * Fern - Graphics Manager V2
 * 
 * Complete rewrite of the graphics subsystem manager.
 * 
 * This module handles:
 * - Driver registration and management
 * - Device detection and probing
 * - Driver selection (best match)
 * - Mode setting and framebuffer management
 * - Fallback chain for compatibility
 * 
 * Driver priority (highest to lowest):
 * 1. VMware SVGA (if running in VMware)
 * 2. Bochs BGA (if running in QEMU/Bochs/VirtualBox)
 * 3. VESA VBE (multiboot framebuffer)
 * 4. Intel HD (real hardware)
 * 5. AMD/ATI (real hardware)
 * 6. NVIDIA (real hardware)
 * 7. VGA Text (fallback)
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/debuglog.h"
#include "../include/string.h"
#include "../include/pci.h"
#include "../include/multiboot.h"
#include "../include/spinlock.h"
#include "../include/hardware.h"
#include "../include/libc/stdio.h"
#include "../include/framebuffer.h"

/* Use debuglog for visible output to serial console */
#define gfx_log(fmt, ...) debuglog(DEBUG_INFO, "[GFX] " fmt, ##__VA_ARGS__)
#define gfx_err(fmt, ...) debuglog(DEBUG_ERROR, "[GFX] " fmt, ##__VA_ARGS__)

/* ============================================================================
 * Global State
 * ============================================================================ */

/* Maximum number of devices and drivers */
#define GFX_MAX_DEVICES     8
#define GFX_MAX_DRIVERS     16

/* Error correction and validation */
#define GFX_FB_MAGIC        0x46423031  /* "FB01" */
#define GFX_MAX_RETRIES     3
#define GFX_CORRUPTION_THRESHOLD 5

/* Global graphics state */
static struct {
    gfx_driver_t* drivers[GFX_MAX_DRIVERS];
    uint32_t driver_count;
    
    gfx_device_t devices[GFX_MAX_DEVICES];
    uint32_t device_count;
    
    gfx_device_t* primary_device;
    gfx_framebuffer_t* primary_fb;
    
    bool initialized;
    
    /* Error tracking */
    uint32_t corruption_count;
    uint32_t last_valid_magic;
    bool recovery_in_progress;
    
    /* Framebuffer validation state */
    uintptr_t expected_phys_addr;
    uint32_t expected_size;

    /* Mapping synchronization — serializes all framebuffer page table
     * modifications to prevent concurrent remap races during task switching. */
    spinlock_t fb_mapping_lock;
    uint32_t fb_mapping_seq;
    uint32_t fb_mapping_ops;
    page_directory_t* fb_owner_pd;

    /* Driver blacklist */
    bool driver_blacklist[GFX_MAX_DRIVERS];
    uint32_t blacklist_count;

    /* Environment detection */
    bool is_vm;
    bool env_detected;
} g_gfx = {0};

/* Last page directory we synced for framebuffer visibility. */
static page_directory_t* g_fb_last_synced_pd = NULL;

/* Framebuffer header for validation */
typedef struct fb_validation_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t format;
    uint32_t checksum;
} fb_validation_header_t;

/* Forward declarations for error correction */
static bool gfx_validate_framebuffer(void);
static gfx_result_t gfx_recover_framebuffer(void);
static void gfx_handle_corruption(void);
static void gfx_write_validation_header(void);
static uint32_t gfx_calculate_checksum(const void* data, size_t size);
gfx_result_t gfx_get_framebuffer(gfx_framebuffer_t** fb);

/* Environment and blacklist helpers */
static bool gfx_detect_environment(void);
static bool gfx_is_device_blacklisted(gfx_device_type_t type);

/* ============================================================================
 * Environment Detection
 * ============================================================================ */

static bool gfx_detect_environment(void) {
    if (g_gfx.env_detected) {
        return g_gfx.is_vm;
    }

    const cpuid_info_t* cpuid = hardware_get_cpuid_info();
    g_gfx.is_vm = cpuid && cpuid->hypervisor_present;
    g_gfx.env_detected = true;

    if (g_gfx.is_vm) {
        gfx_log("Running inside a virtual machine\n");
    } else {
        gfx_log("Running on bare metal\n");
    }

    return g_gfx.is_vm;
}

/* ============================================================================
 * Driver Blacklist
 * ============================================================================ */

static bool gfx_is_device_blacklisted(gfx_device_type_t type) {
    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        if (g_gfx.driver_blacklist[i] && g_gfx.drivers[i]) {
            /* Check if this driver handles the blacklisted type */
            if (g_gfx.drivers[i]->supported_types & (1 << type)) {
                return true;
            }
        }
    }
    return false;
}

bool gfx_is_driver_blacklisted(gfx_device_type_t type) {
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        if (g_gfx.driver_blacklist[i]) {
            if (g_gfx.drivers[i]->supported_types & (1 << type)) {
                return true;
            }
        }
    }
    return false;
}

void gfx_blacklist_driver(gfx_device_type_t type) {
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        if (g_gfx.drivers[i]->supported_types & (1 << type)) {
            g_gfx.driver_blacklist[i] = true;
            g_gfx.blacklist_count++;
            gfx_log("Blacklisted driver: %s (type %d)\n",
                    g_gfx.drivers[i]->ops->name, type);
        }
    }
}

void gfx_unblacklist_driver(gfx_device_type_t type) {
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        if (g_gfx.driver_blacklist[i]) {
            if (g_gfx.drivers[i]->supported_types & (1 << type)) {
                g_gfx.driver_blacklist[i] = false;
                if (g_gfx.blacklist_count > 0) {
                    g_gfx.blacklist_count--;
                }
                gfx_log("Unblacklisted driver: %s (type %d)\n",
                        g_gfx.drivers[i]->ops->name, type);
            }
        }
    }
}

/* External driver init functions */
extern gfx_result_t bga_driver_init(void);
extern gfx_result_t svga_driver_init(void);
extern gfx_result_t vesa_driver_init(void);
extern gfx_result_t vga_text_driver_init(void);
extern gfx_result_t vga_graphics_driver_init(void);
extern gfx_result_t intel_driver_init(void);
extern gfx_result_t amd_driver_init(void);
extern gfx_result_t nv_driver_init(void);
extern gfx_result_t software_3d_driver_init(void);

/* External multiboot info */
extern multiboot_info_t* g_multiboot_info;
extern void* g_multiboot_framebuffer;
extern uint32_t g_multiboot_fb_width;
extern uint32_t g_multiboot_fb_height;
extern uint32_t g_multiboot_fb_pitch;
extern uint32_t g_multiboot_fb_bpp;
extern uintptr_t g_multiboot_fb_addr;

/* ============================================================================
 * Driver Registration
 * ============================================================================ */

gfx_result_t gfx_register_driver(gfx_driver_t* drv) {
    if (!drv || !drv->ops) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    if (g_gfx.driver_count >= GFX_MAX_DRIVERS) {
        return GFX_ERR_NO_MEMORY;
    }
    
    /* Check for duplicate */
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        if (g_gfx.drivers[i] == drv) {
            return GFX_OK;  /* Already registered */
        }
    }
    
    g_gfx.drivers[g_gfx.driver_count++] = drv;
    
    return GFX_OK;
}

gfx_result_t gfx_unregister_driver(gfx_driver_t* drv) {
    if (!drv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        if (g_gfx.drivers[i] == drv) {
            /* Shift remaining drivers */
            for (uint32_t j = i; j < g_gfx.driver_count - 1; j++) {
                g_gfx.drivers[j] = g_gfx.drivers[j + 1];
            }
            g_gfx.driver_count--;
            return GFX_OK;
        }
    }
    
    return GFX_ERR_NOT_SUPPORTED;
}

gfx_driver_t* gfx_find_driver(gfx_device_t* dev) {
    if (!dev) return NULL;
    
    /* Find best matching driver */
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        gfx_driver_t* drv = g_gfx.drivers[i];
        
        if (drv->supported_types & (1 << dev->type)) {
            return drv;
        }
    }
    
    return NULL;
}

/* ============================================================================
 * Device Detection and Probing
 * ============================================================================ */

/**
 * Try to probe a device with all registered drivers
 * Only tries drivers that support the device type
 */
static gfx_result_t gfx_probe_device(gfx_device_t* dev) {
    gfx_result_t result = GFX_ERR_NO_DRIVER;
    
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        gfx_driver_t* drv = g_gfx.drivers[i];
        
        /* Only try drivers that support this device type */
        if (drv->supported_types != dev->type) {
            continue;
        }

        /* Skip blacklisted drivers */
        if (g_gfx.driver_blacklist[i]) {
            gfx_log("Skipping blacklisted driver: %s for type %d\n",
                    drv->ops->name, dev->type);
            continue;
        }
        
        if (drv->ops->probe) {
            gfx_log("Probing device type %d with driver '%s' (priority=%u)...\n",
                    dev->type, drv->ops->name, drv->priority);
            result = drv->ops->probe(dev);
            
            if (result == GFX_OK) {
                dev->driver = drv;
                gfx_log("Driver '%s' probe SUCCESS for type %d\n",
                        drv->ops->name, dev->type);
                return GFX_OK;
            } else {
                gfx_log("Driver '%s' probe FAILED for type %d (err=%d)\n",
                        drv->ops->name, dev->type, result);
            }
        } else {
            gfx_log("Driver '%s' has no probe function, skipping\n", drv->ops->name);
        }
    }
    
    gfx_log("No working driver found for device type %d\n", dev->type);
    return result;
}

/**
 * Detect virtual machine graphics
 */
static bool gfx_detect_vm_graphics(void) {
    gfx_detect_environment();

    /* Check for Bochs BGA */
    gfx_outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    uint16_t bga_id = gfx_inw(VBE_DISPI_IOPORT_DATA);
    
    if (bga_id >= VBE_DISPI_ID0 && bga_id <= VBE_DISPI_ID5) {
        debug_print("[GFX] Detected Bochs BGA (ID: 0x%04x)\n", bga_id);
        gfx_log("Bochs BGA detected (ID: 0x%04x)\n", bga_id);
        
        if (g_gfx.device_count < GFX_MAX_DEVICES) {
            gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
            memset(dev, 0, sizeof(gfx_device_t));
            dev->type = GFX_DEVICE_BOCHS_BGA;
            
            if (gfx_probe_device(dev) == GFX_OK) {
                g_gfx.device_count++;
                return true;
            }
        }
    }
    
    /* Check for VMware SVGA via PCI */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t vendor_device = pci_read_config(bus, slot, 0, 0x00);
            
            if (vendor_device == 0xFFFFFFFF) continue;
            
            uint16_t vendor = vendor_device & 0xFFFF;
            uint16_t device = (vendor_device >> 16) & 0xFFFF;
            
            if (vendor == VMWARE_PCI_VENDOR && 
                (device == VMWARE_SVGA_PCI_DEVICE || device == VMWARE_SVGA_PCI_DEVICE_OLD)) {
                
                debug_print("[GFX] Detected VMware SVGA at %02x:%02x.0\n", bus, slot);
                gfx_log("VMware SVGA detected at %02x:%02x.0\n", bus, slot);

                /* Blacklist VMware SVGA in QEMU - it's known to cause issues */
                if (g_gfx.is_vm && !gfx_is_driver_blacklisted(GFX_DEVICE_VMWARE_SVGA)) {
                    bool is_qemu = false;
                    for (uint32_t b = 0; b < 256 && !is_qemu; b++) {
                        for (uint32_t s = 0; s < 32 && !is_qemu; s++) {
                            uint32_t vd = pci_read_config(b, s, 0, 0x00);
                            if (vd == 0xFFFFFFFF) continue;
                            uint16_t v = vd & 0xFFFF;
                            if (v == BGA_PCI_VENDOR_BOCHS || v == BGA_PCI_VENDOR_QEMU) {
                                is_qemu = true;
                            }
                        }
                    }
                    if (is_qemu) {
                        gfx_blacklist_driver(GFX_DEVICE_VMWARE_SVGA);
                        gfx_log("Blacklisting VMware SVGA in QEMU environment\n");
                    }
                }
                
                if (g_gfx.device_count < GFX_MAX_DEVICES) {
                    gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
                    memset(dev, 0, sizeof(gfx_device_t));
                    dev->type = GFX_DEVICE_VMWARE_SVGA;
                    
                    if (gfx_probe_device(dev) == GFX_OK) {
                        g_gfx.device_count++;
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

/**
 * Detect real hardware graphics
 */
static bool gfx_detect_real_hardware(void) {
    bool found = false;
    
    /* Scan PCI for graphics devices */
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            for (uint32_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, slot, func, 0x00);
                
                if (vendor_device == 0xFFFFFFFF) continue;
                
                uint32_t class_code = pci_read_config(bus, slot, func, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                uint8_t sub_class = (class_code >> 16) & 0xFF;
                
                /* VGA compatible controller (class 03.00) or 3D controller (03.02) */
                if (base_class != 0x03) continue;
                if (sub_class != 0x00 && sub_class != 0x80 && sub_class != 0x02) continue;
                
                uint16_t vendor = vendor_device & 0xFFFF;
                uint16_t device = (vendor_device >> 16) & 0xFFFF;
                
                /* Skip virtual device vendors we already handle */
                if (vendor == BGA_PCI_VENDOR_BOCHS || vendor == VMWARE_PCI_VENDOR ||
                    vendor == VBOX_PCI_VENDOR) {
                    continue;
                }
                
                debug_print("[GFX] Found PCI graphics: %04x:%04x at %02x:%02x.%x\n",
                            vendor, device, bus, slot, func);
                
                if (g_gfx.device_count < GFX_MAX_DEVICES) {
                    gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
                    memset(dev, 0, sizeof(gfx_device_t));
                    
                    /* Set device type based on vendor */
                    if (vendor == INTEL_PCI_VENDOR) {
                        dev->type = GFX_DEVICE_INTEL_HD;
                    } else if (vendor == AMD_PCI_VENDOR) {
                        dev->type = GFX_DEVICE_AMD_ATI;
                    } else if (vendor == NVIDIA_PCI_VENDOR) {
                        dev->type = GFX_DEVICE_NVIDIA;
                    } else {
                        dev->type = GFX_DEVICE_UNKNOWN;
                    }
                    
                    if (gfx_probe_device(dev) == GFX_OK) {
                        g_gfx.device_count++;
                        found = true;
                    }
                }
            }
        }
    }
    
    return found;
}

/**
 * Set up VESA/multiboot framebuffer
 */
static bool gfx_setup_vesa_framebuffer(void) {
    /* Check if multiboot framebuffer is available */
    if (g_multiboot_fb_addr == 0 && g_multiboot_info == NULL) {
        gfx_log("No multiboot framebuffer available, skipping VESA\n");
        return false;
    }
    
    gfx_log("Attempting VESA/multiboot framebuffer setup\n");
    
    if (g_gfx.device_count < GFX_MAX_DEVICES) {
        gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
        memset(dev, 0, sizeof(gfx_device_t));
        dev->type = GFX_DEVICE_VESA;
        
        if (gfx_probe_device(dev) == GFX_OK) {
            g_gfx.device_count++;
            gfx_log("VESA framebuffer setup SUCCESS\n");
            return true;
        }
        gfx_log("VESA framebuffer setup FAILED\n");
    }
    
    return false;
}

/**
 * Set up VGA graphics mode (with double-buffering support)
 */
static bool gfx_setup_vga_graphics(void) {
    gfx_log("Attempting VGA graphics mode setup\n");
    
    if (g_gfx.device_count < GFX_MAX_DEVICES) {
        gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
        memset(dev, 0, sizeof(gfx_device_t));
        dev->type = GFX_DEVICE_VGA;
        
        if (gfx_probe_device(dev) == GFX_OK) {
            g_gfx.device_count++;
            gfx_log("VGA graphics mode setup SUCCESS\n");
            return true;
        }
        gfx_log("VGA graphics mode setup FAILED\n");
    }
    
    return false;
}

/**
 * Set up VGA text mode fallback
 */
static bool gfx_setup_vga_text(void) {
    gfx_log("Attempting VGA text mode setup (mandatory fallback)\n");
    
    if (g_gfx.device_count < GFX_MAX_DEVICES) {
        gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
        memset(dev, 0, sizeof(gfx_device_t));
        dev->type = GFX_DEVICE_VGA;
        
        if (gfx_probe_device(dev) == GFX_OK) {
            g_gfx.device_count++;
            gfx_log("VGA text mode setup SUCCESS\n");
            return true;
        }
        gfx_log("VGA text mode setup FAILED\n");
    }
    
    return false;
}

/**
 * Probe all devices
 */
gfx_result_t gfx_probe_devices(void) {
    gfx_log("Starting device probing (env: %s)\n",
            g_gfx.is_vm ? "virtual" : "bare metal");
    
    /* Try virtual machine graphics first (fastest) */
    gfx_log("Phase 1: Probing VM graphics...\n");
    gfx_detect_vm_graphics();
    
    /* Try VESA/multiboot framebuffer */
    gfx_log("Phase 2: Probing VESA/multiboot...\n");
    gfx_setup_vesa_framebuffer();
    
    /* Try real hardware */
    gfx_log("Phase 3: Probing PCI graphics hardware...\n");
    gfx_detect_real_hardware();
    
    /* Try VGA graphics mode with double-buffering */
    gfx_log("Phase 4: Probing VGA graphics...\n");
    gfx_setup_vga_graphics();
    
    /* VGA text mode as ultimate fallback */
    gfx_log("Phase 5: Probing VGA text mode...\n");
    gfx_setup_vga_text();
    
    debug_print("[GFX] Found %u device(s)\n", g_gfx.device_count);
    gfx_log("Device probing complete: %u device(s) found\n", g_gfx.device_count);
    
    /* MANDATORY FALLBACK: if nothing works, create a software framebuffer device */
    if (g_gfx.device_count == 0) {
        gfx_log("No hardware drivers found, creating software framebuffer fallback\n");
        
        if (g_gfx.device_count < GFX_MAX_DEVICES) {
            gfx_device_t* dev = &g_gfx.devices[g_gfx.device_count];
            memset(dev, 0, sizeof(gfx_device_t));
            dev->type = GFX_DEVICE_SOFTWARE_FB;
            snprintf(dev->name, sizeof(dev->name), "Software Framebuffer");
            
            if (gfx_probe_device(dev) == GFX_OK) {
                g_gfx.device_count++;
                gfx_log("Software framebuffer fallback created SUCCESS\n");
            } else {
                gfx_err("Software framebuffer fallback FAILED - no display output available\n");
            }
        }
    }
    
    return (g_gfx.device_count > 0) ? GFX_OK : GFX_ERR_NOT_SUPPORTED;
}

/* ============================================================================
 * Device Management
 * ============================================================================ */

gfx_result_t gfx_get_device(uint32_t index, gfx_device_t** dev) {
    if (!dev || index >= g_gfx.device_count) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    *dev = &g_gfx.devices[index];
    return GFX_OK;
}

gfx_result_t gfx_get_primary_device(gfx_device_t** dev) {
    if (!dev) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    *dev = g_gfx.primary_device;
    return g_gfx.primary_device ? GFX_OK : GFX_ERR_NOT_SUPPORTED;
}

uint32_t gfx_get_device_count(void) {
    return g_gfx.device_count;
}

/* ============================================================================
 * Device Selection
 * ============================================================================ */

/**
 * Select the best device to use as primary
 */
static gfx_device_t* gfx_select_primary_device(void) {
    if (g_gfx.device_count == 0) {
        return NULL;
    }

    gfx_log("Selecting primary device from %u candidate(s)\n", g_gfx.device_count);

    /* Select the device whose driver has the highest priority value.
     * Driver priorities are set in gfx_init() (higher number = preferred). */
    gfx_device_t* best = NULL;
    uint32_t best_priority = 0;

    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        gfx_device_t* dev = &g_gfx.devices[i];
        uint32_t prio = (dev->driver) ? dev->driver->priority : 0;
        gfx_log("  device[%u] '%s' type=%d driver_priority=%u\n",
                i, dev->name, dev->type, prio);
        if (prio > best_priority) {
            best_priority = prio;
            best = dev;
        }
    }

    if (best) {
        gfx_log("Selected device: '%s' (type=%d, priority=%u)\n",
                best->name, best->type, best_priority);
        return best;
    }

    /* Fallback: return first device if no driver priorities were set */
    gfx_log("No driver priorities found, using first device (index 0, type %d)\n",
            g_gfx.devices[0].type);
    return &g_gfx.devices[0];
}

/* ============================================================================
 * High-Level API
 * ============================================================================ */

gfx_result_t gfx_init(void) {
    if (g_gfx.initialized) {
        return GFX_OK;
    }
    
    debug_print("[GFX] Initializing graphics subsystem V2...\n");
    
    /* Clear state */
    memset(&g_gfx, 0, sizeof(g_gfx));
    
    /* Register all builtin drivers with priorities */
    bga_driver_init();
    svga_driver_init();
    vesa_driver_init();
    intel_driver_init();
    amd_driver_init();
    nv_driver_init();
    vga_graphics_driver_init();
    vga_text_driver_init();
    software_3d_driver_init();
    
    /* Set driver priorities (higher = preferred) */
    for (uint32_t i = 0; i < g_gfx.driver_count; i++) {
        gfx_driver_t* drv = g_gfx.drivers[i];
        switch (drv->supported_types) {
            case GFX_DEVICE_VESA:         drv->priority = 200; break;
            case GFX_DEVICE_BOCHS_BGA:    drv->priority = 180; break;
            case GFX_DEVICE_VMWARE_SVGA:  drv->priority = 170; break;
            case GFX_DEVICE_INTEL_HD:     drv->priority = 150; break;
            case GFX_DEVICE_AMD_ATI:      drv->priority = 140; break;
            case GFX_DEVICE_NVIDIA:       drv->priority = 130; break;
            case GFX_DEVICE_VGA:          drv->priority = 100; break;
            case GFX_DEVICE_SOFTWARE_FB:  drv->priority = 50;  break;
            default:                      drv->priority = 128; break;
        }
        gfx_log("Driver '%s' registered (type=%d, priority=%u)\n",
                drv->ops->name, drv->supported_types, drv->priority);
    }
    
    debug_print("[GFX] %u drivers registered\n", g_gfx.driver_count);
    
    /* Probe for devices */
    gfx_result_t result = gfx_probe_devices();
    if (result != GFX_OK) {
        debug_print("[GFX] No graphics devices found!\n");
        return result;
    }
    
    /* Select primary device */
    g_gfx.primary_device = gfx_select_primary_device();
    if (!g_gfx.primary_device) {
        debug_print("[GFX] Failed to select primary device\n");
        return GFX_ERR_NO_DRIVER;
    }
    
    debug_print("[GFX] Primary device: %s (%ux%ux%u)\n", 
                g_gfx.primary_device->name,
                g_gfx.primary_device->max_width,
                g_gfx.primary_device->max_height,
                g_gfx.primary_device->max_bpp);
    
    /* Initialize primary device with fallback chain */
    if (g_gfx.primary_device->driver && g_gfx.primary_device->driver->ops->init) {
        result = g_gfx.primary_device->driver->ops->init(g_gfx.primary_device);
        if (result != GFX_OK) {
            gfx_log("Primary device '%s' init FAILED (err=%d), trying next device...\n",
                    g_gfx.primary_device->name, result);
            
            /* Try the next available device */
            bool found_fallback = false;
            for (uint32_t i = 0; i < g_gfx.device_count; i++) {
                gfx_device_t* dev = &g_gfx.devices[i];
                if (dev == g_gfx.primary_device) continue;
                if (!dev->driver || !dev->driver->ops->init) continue;
                
                gfx_log("Attempting fallback init: device '%s' (type %d)\n",
                        dev->name, dev->type);
                result = dev->driver->ops->init(dev);
                if (result == GFX_OK) {
                    g_gfx.primary_device = dev;
                    found_fallback = true;
                    gfx_log("Fallback to device '%s' SUCCESS\n", dev->name);
                    break;
                }
                gfx_log("Fallback init FAILED for '%s' (err=%d)\n", dev->name, result);
            }
            
            if (!found_fallback) {
                debug_print("[GFX] All device init attempts failed\n");
                return GFX_ERR_INIT_FAILED;
            }
        }
    }
    
    /* Set primary_fb from device's fb pointer (set during init).
     * Only accept dev->fb if it already has a valid virtual address; otherwise
     * fall through to map_fb so the driver can resolve it properly. */
    if (g_gfx.primary_device->fb && g_gfx.primary_device->fb->virt_addr) {
        g_gfx.primary_fb = g_gfx.primary_device->fb;
        gfx_log("primary_fb from dev->fb: virt=%p\n",
                g_gfx.primary_fb->virt_addr);
    } else {
        /* dev->fb is NULL or virt_addr is not yet resolved — call map_fb. */
        if (g_gfx.primary_device->driver && g_gfx.primary_device->driver->ops->map_fb) {
            result = g_gfx.primary_device->driver->ops->map_fb(
                g_gfx.primary_device, &g_gfx.primary_fb);
            if (result == GFX_OK && g_gfx.primary_fb) {
                gfx_log("primary_fb from map_fb: virt=%p\n",
                        g_gfx.primary_fb->virt_addr);
            } else {
                gfx_err("map_fb failed or returned NULL fb (result=%d), trying fallback devices...\n", result);

                /* Try map_fb on other probed devices before giving up */
                bool map_fallback_found = false;
                for (uint32_t i = 0; i < g_gfx.device_count; i++) {
                    gfx_device_t* dev = &g_gfx.devices[i];
                    if (dev == g_gfx.primary_device) continue;
                    if (!dev->driver || !dev->driver->ops->map_fb) continue;

                    gfx_log("Attempting map_fb fallback on device '%s' (type %d)\n",
                            dev->name, dev->type);
                    result = dev->driver->ops->map_fb(dev, &g_gfx.primary_fb);
                    if (result == GFX_OK && g_gfx.primary_fb) {
                        g_gfx.primary_device = dev;
                        map_fallback_found = true;
                        gfx_log("map_fb fallback SUCCESS on device '%s': virt=%p\n",
                                dev->name, g_gfx.primary_fb->virt_addr);
                        break;
                    }
                    gfx_log("map_fb fallback FAILED for '%s' (err=%d)\n", dev->name, result);
                }

                if (!map_fallback_found) {
                    gfx_err("map_fb failed on all devices; aborting init\n");
                    return GFX_ERR_MAPPING_FAILED;
                }
            }
        }
    }

    /* Sanity-check: if map_fb returned a framebuffer but left virt_addr NULL,
     * apply the MMIO identity-map fallback for Bochs VBE / QEMU where the
     * aperture (e.g. 0xF0000000) is wired by the hardware memory controller
     * and does not need a software page-table entry.  Only do this when the
     * physical address sits in the high MMIO range (>= 0xC0000000). */
    if (g_gfx.primary_fb && !g_gfx.primary_fb->virt_addr) {
        uintptr_t phys = g_gfx.primary_fb->phys_addr;
        if (phys >= 0xC0000000U) {
            gfx_log("virt_addr NULL after map_fb; applying identity MMIO fallback "
                    "phys=0x%08x -> virt=0x%08x\n", (uint32_t)phys, (uint32_t)phys);
            g_gfx.primary_fb->virt_addr = (void*)(uintptr_t)phys;
        } else {
            gfx_err("virt_addr NULL after map_fb and phys=0x%08x is not MMIO; "
                    "aborting init\n", (uint32_t)phys);
            return GFX_ERR_MAPPING_FAILED;
        }
    } else if (!g_gfx.primary_fb) {
        gfx_err("primary_fb is NULL after all mapping attempts; trying fallback devices\n");

        /* Primary device's map_fb failed — walk all other registered devices and
         * try each one in order, preferring VESA (type GFX_DEVICE_VESA) first,
         * then anything with a working map_fb.
         * Two passes: pass 0 = VESA only, pass 1 = all remaining. */
        bool found_fb_fallback = false;
        for (int pass = 0; pass < 2 && !found_fb_fallback; pass++) {
            for (uint32_t i = 0; i < g_gfx.device_count && !found_fb_fallback; i++) {
                gfx_device_t* dev = &g_gfx.devices[i];

                /* Skip the already-failed primary device */
                if (dev == g_gfx.primary_device) {
                    continue;
                }

                /* Pass 0: only VESA devices; pass 1: everything else */
                if (pass == 0 && dev->type != GFX_DEVICE_VESA) {
                    continue;
                }

                if (!dev->driver || !dev->driver->ops || !dev->driver->ops->map_fb) {
                    continue;
                }

                gfx_framebuffer_t* fb_candidate = NULL;
                gfx_result_t fb_result = dev->driver->ops->map_fb(dev, &fb_candidate);

                if (fb_result == GFX_OK && fb_candidate) {
                    /* Accept immediately if virt_addr is already set */
                    if (fb_candidate->virt_addr) {
                        g_gfx.primary_device = dev;
                        g_gfx.primary_fb     = fb_candidate;
                        found_fb_fallback = true;
                        gfx_log("Fallback map_fb SUCCESS via device '%s' (type=%d) virt=%p\n",
                                dev->name, (int)dev->type, fb_candidate->virt_addr);
                        break;
                    }

                    /* virt_addr is still NULL — try the MMIO identity fallback */
                    uintptr_t phys_fb = fb_candidate->phys_addr;
                    if (phys_fb >= 0xC0000000U) {
                        fb_candidate->virt_addr = (void*)(uintptr_t)phys_fb;
                        g_gfx.primary_device = dev;
                        g_gfx.primary_fb     = fb_candidate;
                        found_fb_fallback = true;
                        gfx_log("Fallback map_fb SUCCESS (MMIO identity) via device '%s' "
                                "(type=%d) phys=0x%08x\n",
                                dev->name, (int)dev->type, (uint32_t)phys_fb);
                        break;
                    }

                    gfx_log("Fallback device '%s' map_fb gave virt_addr=NULL and "
                            "phys=0x%08x is not MMIO; skipping\n",
                            dev->name, (uint32_t)phys_fb);
                } else {
                    gfx_log("Fallback map_fb FAILED for device '%s' (result=%d)\n",
                            dev->name, (int)fb_result);
                }
            }
        }

        if (!found_fb_fallback) {
            gfx_err("primary_fb is NULL after all fallback attempts; aborting init\n");
            return GFX_ERR_MAPPING_FAILED;
        }
    }

    g_gfx.initialized = true;
    g_gfx.corruption_count = 0;
    g_gfx.recovery_in_progress = false;
    spinlock_init(&g_gfx.fb_mapping_lock, "gfx_fb_map");
    g_gfx.fb_mapping_ops = 0;
    g_gfx.fb_owner_pd = vmm_get_current_page_directory();
    g_gfx.fb_mapping_seq = (g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) ? 1 : 0;
    
    /* Write validation header for error detection */
    gfx_write_validation_header();
    
    debug_print("[GFX] Graphics subsystem initialized\n");
    gfx_log("Graphics init complete: using '%s'\n",
            g_gfx.primary_device->name);
    
    return GFX_OK;
}

gfx_result_t gfx_shutdown(void) {
    if (!g_gfx.initialized) {
        return GFX_OK;
    }
    
    /* Shutdown all devices */
    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        gfx_device_t* dev = &g_gfx.devices[i];
        
        if (dev->driver && dev->driver->ops->shutdown) {
            dev->driver->ops->shutdown(dev);
        }
    }
    
    g_gfx.initialized = false;
    g_gfx.primary_device = NULL;
    g_gfx.primary_fb = NULL;
    g_gfx.device_count = 0;
    g_gfx.corruption_count = 0;
    g_gfx.recovery_in_progress = false;
    g_gfx.fb_owner_pd = NULL;
    g_fb_last_synced_pd = NULL;
    
    return GFX_OK;
}

/* ============================================================================
 * Runtime Driver Swap
 * ============================================================================ */

gfx_result_t gfx_swap_driver(gfx_device_type_t type) {
    if (!g_gfx.initialized) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    gfx_log("Attempting runtime driver swap to type %d\n", type);
    
    /* Find a device of the requested type */
    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        gfx_device_t* dev = &g_gfx.devices[i];
        if (dev->type != type) continue;
        
        /* Skip if this is already the primary */
        if (dev == g_gfx.primary_device) {
            gfx_log("Device '%s' is already primary\n", dev->name);
            return GFX_OK;
        }
        
        /* Initialize the new device if not active */
        if (!dev->active && dev->driver && dev->driver->ops->init) {
            gfx_log("Initializing swap target '%s'\n", dev->name);
            gfx_result_t result = dev->driver->ops->init(dev);
            if (result != GFX_OK) {
                gfx_log("Swap target init FAILED (err=%d)\n", result);
                continue;
            }
        }
        
        /* Unmap old framebuffer */
        if (g_gfx.primary_fb && g_gfx.primary_device &&
            g_gfx.primary_device->driver && g_gfx.primary_device->driver->ops->unmap_fb) {
            g_gfx.primary_device->driver->ops->unmap_fb(
                g_gfx.primary_device, g_gfx.primary_fb);
        }
        
        /* Swap */
        gfx_device_t* old_dev = g_gfx.primary_device;
        g_gfx.primary_device = dev;
        g_gfx.primary_fb = NULL;
        
        /* Map new framebuffer */
        if (dev->driver && dev->driver->ops->map_fb) {
            gfx_result_t map_result = dev->driver->ops->map_fb(dev, &g_gfx.primary_fb);
            if (map_result != GFX_OK || !g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
                gfx_err("Swap FB mapping FAILED (err=%d), reverting\n", map_result);
                g_gfx.primary_device = old_dev;
                g_gfx.primary_fb = NULL;
                return map_result;
            }
        } else if (dev->fb) {
            g_gfx.primary_fb = dev->fb;
        }
        
        g_gfx.fb_owner_pd = vmm_get_current_page_directory();
        g_gfx.fb_mapping_seq++;
        g_gfx.fb_mapping_ops++;
        g_gfx.corruption_count = 0;
        
        gfx_write_validation_header();
        gfx_log("Driver swap to '%s' SUCCESS\n", dev->name);
        return GFX_OK;
    }
    
    gfx_log("No device of type %d available for swap\n", type);
    return GFX_ERR_NOT_SUPPORTED;
}

/* ============================================================================
 * Error Correction and Validation Functions
 * ============================================================================ */

static inline uint32_t gfx_fb_bytes_per_pixel(const gfx_framebuffer_t* fb) {
    if (!fb) {
        return 0;
    }

    uint32_t bytes_pp = (fb->bpp + 7) / 8;
    if (fb->width != 0 && fb->pitch >= fb->width && (fb->pitch % fb->width) == 0) {
        uint32_t stride_bpp = fb->pitch / fb->width;
        if (stride_bpp >= 1 && stride_bpp <= 4) {
            bytes_pp = stride_bpp;
        }
    }

    return bytes_pp;
}

static bool gfx_is_fb_mapping_valid(const gfx_framebuffer_t* fb) {
    if (!fb || !fb->virt_addr || fb->pitch == 0 || fb->height == 0) {
        return false;
    }

    /* Some text backends may not provide a physical address. */
    if (!fb->phys_addr) {
        return true;
    }

    page_directory_t* pd = vmm_get_current_page_directory();
    if (!pd) {
        return false;
    }

    uint32_t virt_page = ((uint32_t)(uintptr_t)fb->virt_addr) & ~MEMORY_PAGE_MASK;
    uint32_t phys_page = ((uint32_t)(uintptr_t)fb->phys_addr) & ~MEMORY_PAGE_MASK;
    uint32_t phys_offset = ((uint32_t)(uintptr_t)fb->phys_addr) & MEMORY_PAGE_MASK;
    uint64_t visible_size64 = (uint64_t)fb->pitch * (uint64_t)fb->height;
    uint64_t map_size64 = visible_size64 + (uint64_t)phys_offset;
    if (map_size64 == 0 || map_size64 > 0xFFFFFFFFULL) {
        return false;
    }
    uint32_t page_count = ((uint32_t)map_size64 + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
    if (page_count == 0) {
        return false;
    }

    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t vaddr = virt_page + (i * MEMORY_PAGE_SIZE);
        uint32_t paddr = phys_page + (i * MEMORY_PAGE_SIZE);
        uint32_t mapped = vmm_get_physical_addr(pd, vaddr);
        if (mapped != paddr) {
            /* vmm_get_physical_addr() only knows about pages our VMM
             * explicitly tracked.  High MMIO windows (e.g. Bochs VBE
             * aperture at 0xF0000000) may be mapped by hardware/BIOS and
             * never appear in our software page tables with the right
             * physical address.  If the query returned 0 (page not present
             * in our tables) AND the virtual address is in the high MMIO
             * range (>= 0xC0000000), treat it as "probably firmware-mapped"
             * and skip the failure rather than declaring the whole mapping
             * invalid. */
            if (mapped == 0 && vaddr >= 0xC0000000U) {
                continue;
            }
            return false;
        }
    }
    return true;
}

static bool gfx_try_repair_fb_mapping(gfx_framebuffer_t* fb) {
    if (!fb || !fb->virt_addr || !fb->phys_addr || fb->pitch == 0 || fb->height == 0) {
        return false;
    }

    page_directory_t* pd = vmm_get_current_page_directory();
    if (!pd) {
        return false;
    }

    uint32_t virt_page = ((uint32_t)(uintptr_t)fb->virt_addr) & ~MEMORY_PAGE_MASK;
    uint32_t phys_page = ((uint32_t)(uintptr_t)fb->phys_addr) & ~MEMORY_PAGE_MASK;
    uint32_t phys_offset = ((uint32_t)(uintptr_t)fb->phys_addr) & MEMORY_PAGE_MASK;
    if (gfx_is_fb_mapping_valid(fb)) {
        return true;
    }

    unsigned long irq_flags = spinlock_irq_save();
    spinlock_acquire(&g_gfx.fb_mapping_lock);

    if (gfx_is_fb_mapping_valid(fb)) {
        spinlock_release(&g_gfx.fb_mapping_lock);
        spinlock_irq_restore(irq_flags);
        return true;
    }

    uint64_t visible_size_64 = (uint64_t)fb->pitch * (uint64_t)fb->height;
    if (visible_size_64 == 0 || visible_size_64 > 0xFFFFFFFFULL) {
        spinlock_release(&g_gfx.fb_mapping_lock);
        spinlock_irq_restore(irq_flags);
        return false;
    }
    uint32_t visible_size = (uint32_t)visible_size_64;
    uint32_t map_payload = fb->size > visible_size ? fb->size : visible_size;
    uint64_t map_size_64 = (uint64_t)map_payload + (uint64_t)phys_offset;
    if (map_size_64 == 0 || map_size_64 > 0xFFFFFFFFULL) {
        spinlock_release(&g_gfx.fb_mapping_lock);
        spinlock_irq_restore(irq_flags);
        return false;
    }
    uint32_t map_size = (uint32_t)map_size_64;
    uint32_t page_count = (map_size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;

    uint32_t repaired = 0;
    bool failed = false;
    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t vaddr = virt_page + (i * MEMORY_PAGE_SIZE);
        uint32_t paddr = phys_page + (i * MEMORY_PAGE_SIZE);
        uint32_t mapped = vmm_get_physical_addr(pd, vaddr);
        if (mapped != 0 && mapped != paddr) {
            vmm_unmap_page(pd, vaddr);
        }
        memory_result_t res = vmm_map_page(pd, vaddr, paddr,
                                           PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        if (res == MEMORY_OK || res == MEMORY_ERROR_ALREADY_MAPPED) {
            repaired++;
        } else {
            debuglog(DEBUG_WARN, "[GFX] FB remap failed: v=0x%08x p=0x%08x res=%d\n",
                     vaddr, paddr, res);
            failed = true;
        }
        __asm__ __volatile__("invlpg (%0)" :: "r"(vaddr) : "memory");
    }

    bool valid = !failed && gfx_is_fb_mapping_valid(fb);
    if (valid) {
        g_gfx.fb_owner_pd = pd;
        g_gfx.fb_mapping_seq++;
        g_gfx.fb_mapping_ops++;
    }

    spinlock_release(&g_gfx.fb_mapping_lock);
    spinlock_irq_restore(irq_flags);

    if (!valid) {
        debuglog(DEBUG_WARN,
                 "[GFX] FB remap verify failed: virt=0x%08x phys=0x%08x pages=%u\n",
                 virt_page, phys_page, page_count);
        return false;
    }

    debuglog(DEBUG_WARN, "[GFX] Repaired framebuffer mapping: virt=0x%08x phys=0x%08x pages=%u\n",
             virt_page, phys_page, repaired);
    return true;
}

/**
 * Calculate a simple checksum for validation
 */
static uint32_t gfx_calculate_checksum(const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t checksum = 0;
    for (size_t i = 0; i < size; i++) {
        checksum = (checksum << 1) | (checksum >> 31);
        checksum ^= bytes[i];
    }
    return checksum;
}

/**
 * Write validation header to framebuffer
 */
static void gfx_write_validation_header(void) {
    if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
        return;
    }

    size_t visible_size = (size_t)g_gfx.primary_fb->pitch * (size_t)g_gfx.primary_fb->height;
    if (g_gfx.primary_fb->size <= visible_size ||
        g_gfx.primary_fb->size <= sizeof(fb_validation_header_t)) {
        return; /* No hidden region available; avoid scribbling visible pixels. */
    }
    
    /* Write header at end of framebuffer (won't be visible) */
    fb_validation_header_t* header = (fb_validation_header_t*)
        ((uint8_t*)g_gfx.primary_fb->virt_addr + g_gfx.primary_fb->size - sizeof(fb_validation_header_t));
    
    header->magic = GFX_FB_MAGIC;
    header->version = 1;
    header->width = g_gfx.primary_fb->width;
    header->height = g_gfx.primary_fb->height;
    header->pitch = g_gfx.primary_fb->pitch;
    header->format = g_gfx.primary_fb->format;
    header->checksum = gfx_calculate_checksum(g_gfx.primary_fb->virt_addr, 
                                               g_gfx.primary_fb->size - sizeof(fb_validation_header_t));
    
    g_gfx.last_valid_magic = GFX_FB_MAGIC;
}

/**
 * Validate framebuffer integrity
 * Returns true if framebuffer is valid, false if corrupted
 */
static bool gfx_validate_framebuffer(void) {
    if (!g_gfx.primary_fb) {
        gfx_err("validate: primary_fb is NULL\n");
        return false;
    }

    gfx_framebuffer_t* fb = g_gfx.primary_fb;

    if (!fb->virt_addr) {
        gfx_err("validate: virt_addr is NULL\n");
        return false;
    }

    if (fb->width == 0 || fb->height == 0 || fb->pitch == 0) {
        gfx_err("validate: invalid dimensions %ux%u pitch=%u\n",
                fb->width, fb->height, fb->pitch);
        return false;
    }

    if (fb->pitch < fb->width) {
        gfx_err("validate: pitch %u < width %u\n", fb->pitch, fb->width);
        return false;
    }

    uint64_t visible_size = (uint64_t)fb->pitch * (uint64_t)fb->height;
    if (visible_size > 256 * 1024 * 1024) {
        gfx_err("validate: framebuffer too large (%llu bytes)\n", visible_size);
        return false;
    }

    if (fb->size != 0 && fb->size < (uint32_t)visible_size) {
        gfx_err("validate: size %u < visible %llu\n", fb->size, visible_size);
        return false;
    }

    if (!gfx_is_fb_mapping_valid(fb)) {
        gfx_log("validate: mapping invalid, attempting repair\n");

        if (g_gfx.primary_device && g_gfx.primary_device->driver &&
            g_gfx.primary_device->driver->ops->map_fb) {
            g_gfx.primary_fb = NULL;
            if (g_gfx.primary_device->driver->ops->map_fb(
                    g_gfx.primary_device, &g_gfx.primary_fb) == GFX_OK &&
                g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) {
                if (gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
                    gfx_log("validate: re-mapped successfully\n");
                    gfx_write_validation_header();
                    return true;
                }
            }
        }

        page_directory_t* pd = vmm_get_current_page_directory();
        if (pd && pd == vmm_get_kernel_page_directory() &&
            gfx_try_repair_fb_mapping(fb)) {
            gfx_write_validation_header();
            return true;
        }

        gfx_err("validate: mapping repair failed\n");
        return false;
    }

    if (fb->size != 0 &&
        fb->size > visible_size &&
        fb->size >= sizeof(fb_validation_header_t) + sizeof(uint32_t)) {
        fb_validation_header_t* header = (fb_validation_header_t*)
            ((uint8_t*)fb->virt_addr + fb->size - sizeof(fb_validation_header_t));

        if (header->magic == GFX_FB_MAGIC) {
            uint32_t expected = gfx_calculate_checksum(
                fb->virt_addr, fb->size - sizeof(fb_validation_header_t));
            if (expected != header->checksum) {
                g_gfx.corruption_count++;
                gfx_err("validate: checksum mismatch (corruption #%u)\n",
                        g_gfx.corruption_count);
                if (g_gfx.corruption_count >= GFX_CORRUPTION_THRESHOLD) {
                    gfx_handle_corruption();
                }
                return false;
            }
            if (header->width != fb->width || header->height != fb->height ||
                header->pitch != fb->pitch) {
                gfx_err("validate: header dimensions mismatch\n");
                return false;
            }
        }
    }

    return true;
}

/**
 * Attempt to recover framebuffer after corruption
 */
static gfx_result_t gfx_recover_framebuffer(void) {
    if (g_gfx.recovery_in_progress) {
        return GFX_ERR_DEVICE_BUSY;
    }
    
    g_gfx.recovery_in_progress = true;
    debug_print("[GFX_RECOVERY] Attempting framebuffer recovery...\n");

    unsigned long irq_flags = spinlock_irq_save();
    spinlock_acquire(&g_gfx.fb_mapping_lock);
    
    /* Attempt 1: Re-map the framebuffer */
    if (g_gfx.primary_device && g_gfx.primary_device->driver && 
        g_gfx.primary_device->driver->ops->map_fb) {
        gfx_framebuffer_t* new_fb = NULL;
        gfx_result_t result = g_gfx.primary_device->driver->ops->map_fb(
            g_gfx.primary_device, &new_fb);
        
        if (result == GFX_OK && new_fb && new_fb->virt_addr) {
            g_gfx.primary_fb = new_fb;
            g_gfx.fb_owner_pd = vmm_get_current_page_directory();
            g_gfx.fb_mapping_seq++;
            debug_print("[GFX_RECOVERY] Successfully re-mapped framebuffer at %p\n", 
                       new_fb->virt_addr);
            g_gfx.corruption_count = 0;
            spinlock_release(&g_gfx.fb_mapping_lock);
            spinlock_irq_restore(irq_flags);
            g_gfx.recovery_in_progress = false;
            gfx_write_validation_header();
            return GFX_OK;
        }
    }
    
    spinlock_release(&g_gfx.fb_mapping_lock);
    spinlock_irq_restore(irq_flags);
    
    /* Attempt 2: Re-initialize the device */
    if (g_gfx.primary_device && g_gfx.primary_device->driver &&
        g_gfx.primary_device->driver->ops->init) {
        debug_print("[GFX_RECOVERY] Re-initializing device...\n");
        gfx_result_t result = g_gfx.primary_device->driver->ops->init(g_gfx.primary_device);
        
        if (result == GFX_OK) {
            if (g_gfx.primary_device->fb) {
                g_gfx.primary_fb = g_gfx.primary_device->fb;
            } else if (g_gfx.primary_device->driver->ops->map_fb) {
                g_gfx.primary_device->driver->ops->map_fb(
                    g_gfx.primary_device, &g_gfx.primary_fb);
            }
            
            if (g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) {
                debug_print("[GFX_RECOVERY] Device re-initialized successfully\n");
                g_gfx.corruption_count = 0;
                g_gfx.recovery_in_progress = false;
                g_gfx.fb_owner_pd = vmm_get_current_page_directory();
                g_gfx.fb_mapping_seq++;
                gfx_write_validation_header();
                return GFX_OK;
            }
        }
    }
    
    /* Attempt 3: Try VESA fallback via driver swap */
    debug_print("[GFX_RECOVERY] Trying VESA fallback...\n");
    if (gfx_swap_driver(GFX_DEVICE_VESA) == GFX_OK) {
        debug_print("[GFX_RECOVERY] Fallback to VESA successful\n");
        g_gfx.corruption_count = 0;
        g_gfx.recovery_in_progress = false;
        return GFX_OK;
    }
    
    /* Attempt 4: Try other available devices */
    debug_print("[GFX_RECOVERY] Trying other available devices...\n");
    static const gfx_device_type_t fallback_types[] = {
        GFX_DEVICE_BOCHS_BGA,
        GFX_DEVICE_VMWARE_SVGA,
        GFX_DEVICE_INTEL_HD,
        GFX_DEVICE_AMD_ATI,
        GFX_DEVICE_NVIDIA,
    };
    for (size_t f = 0; f < sizeof(fallback_types)/sizeof(fallback_types[0]); f++) {
        if (gfx_swap_driver(fallback_types[f]) == GFX_OK) {
            debug_print("[GFX_RECOVERY] Fallback to type %d successful\n", fallback_types[f]);
            g_gfx.corruption_count = 0;
            g_gfx.recovery_in_progress = false;
            return GFX_OK;
        }
    }
    
    /* Attempt 5: VGA text mode as last resort */
    debug_print("[GFX_RECOVERY] Trying VGA text mode fallback...\n");
    if (gfx_swap_driver(GFX_DEVICE_VGA) == GFX_OK) {
        debug_print("[GFX_RECOVERY] Fallback to VGA text mode successful\n");
        g_gfx.corruption_count = 0;
        g_gfx.recovery_in_progress = false;
        return GFX_OK;
    }
    
    /* Attempt 6: Software framebuffer as absolute last resort */
    debug_print("[GFX_RECOVERY] Trying software framebuffer fallback...\n");
    if (gfx_swap_driver(GFX_DEVICE_SOFTWARE_FB) == GFX_OK) {
        debug_print("[GFX_RECOVERY] Fallback to software framebuffer successful\n");
        g_gfx.corruption_count = 0;
        g_gfx.recovery_in_progress = false;
        return GFX_OK;
    }
    
    g_gfx.recovery_in_progress = false;
    debug_print("[GFX_RECOVERY] All recovery attempts failed\n");
    return GFX_ERR_HARDWARE;
}

/**
 * Handle detected corruption with automatic recovery
 */
static void gfx_handle_corruption(void) {
    g_gfx.corruption_count++;
    
    debug_print("[GFX_CORRUPTION] Detected corruption #%u\n", g_gfx.corruption_count);
    
    if (g_gfx.corruption_count >= GFX_CORRUPTION_THRESHOLD) {
        debug_print("[GFX_CORRUPTION] Threshold reached, initiating recovery\n");
        gfx_recover_framebuffer();
    }
}

/**
 * Safe framebuffer access with validation
 */
static bool gfx_ensure_framebuffer_valid(void) {
    if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
        if (gfx_get_framebuffer(&g_gfx.primary_fb) != GFX_OK ||
            !g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
            return false;
        }
    }

    if (gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
        return true;
    }

    page_directory_t* current_pd = vmm_get_current_page_directory();
    page_directory_t* kernel_pd = vmm_get_kernel_page_directory();

    if (current_pd && current_pd != g_fb_last_synced_pd) {
        vmm_sync_kernel_pdes(current_pd);
        g_fb_last_synced_pd = current_pd;
        if (gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
            return true;
        }
    }

    if (gfx_try_repair_fb_mapping(g_gfx.primary_fb)) {
        return true;
    }

    if (g_gfx.primary_device && g_gfx.primary_device->driver &&
        g_gfx.primary_device->driver->ops->map_fb) {
        unsigned long irq_flags = spinlock_irq_save();
        spinlock_acquire(&g_gfx.fb_mapping_lock);

        g_gfx.primary_fb = NULL;
        gfx_result_t map_result = g_gfx.primary_device->driver->ops->map_fb(
            g_gfx.primary_device, &g_gfx.primary_fb);

        if (map_result == GFX_OK && g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) {
            if (gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
                g_gfx.fb_owner_pd = vmm_get_current_page_directory();
                g_gfx.fb_mapping_seq++;
                g_gfx.fb_mapping_ops++;
                spinlock_release(&g_gfx.fb_mapping_lock);
                spinlock_irq_restore(irq_flags);
                return true;
            }
        }

        spinlock_release(&g_gfx.fb_mapping_lock);
        spinlock_irq_restore(irq_flags);
    }

    if (current_pd && current_pd != kernel_pd) {
        if (gfx_try_repair_fb_mapping(g_gfx.primary_fb)) {
            return true;
        }
    }

    if (!g_gfx.recovery_in_progress) {
        gfx_recover_framebuffer();
        if (g_gfx.primary_fb && g_gfx.primary_fb->virt_addr &&
            gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
            return true;
        }
    }

    return false;
}

/* Defense-in-depth bound, mirroring sys_set_fb_mode()'s cap in syscall.c:
 * gfx_set_mode() is a public kernel entry point (also reachable via the
 * legacy graphics_set_mode() compat shim), and the pitch/size math just
 * below is done in plain 32-bit arithmetic. Without a cap here too, any
 * future or indirect caller that skips the syscall-layer check could still
 * drive an integer overflow in `width * bytes_per_pixel` / `pitch *
 * height` downstream, wrapping a huge true framebuffer size down to
 * something that looks small enough to fit in VRAM. */
#define GFX_SET_MODE_MAX_DIMENSION 4096u

gfx_result_t gfx_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    if (width == 0 || height == 0 ||
        width > GFX_SET_MODE_MAX_DIMENSION || height > GFX_SET_MODE_MAX_DIMENSION) {
        return GFX_ERR_INVALID_PARAM;
    }

    gfx_mode_t mode = {0};
    mode.width = width;
    mode.height = height;
    mode.bpp = bpp;
    mode.pitch = width * ((bpp + 7) / 8);

    /* Determine pixel format */
    switch (bpp) {
        case 8:  mode.format = GFX_FORMAT_INDEXED_8; break;
        case 15: mode.format = GFX_FORMAT_BGR555; break;
        case 16: mode.format = GFX_FORMAT_BGR565; break;
        case 24: mode.format = GFX_FORMAT_BGR888; break;
        case 32: mode.format = GFX_FORMAT_BGRX8888; break;
        default: return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    gfx_result_t first_result = GFX_ERR_NOT_SUPPORTED;

    /* First, try current primary device. */
    if (g_gfx.primary_device->driver->ops &&
        g_gfx.primary_device->driver->ops->set_mode) {
        first_result = g_gfx.primary_device->driver->ops->set_mode(
            g_gfx.primary_device, &mode);

        if (first_result == GFX_OK) {
            unsigned long irq_flags = spinlock_irq_save();
            spinlock_acquire(&g_gfx.fb_mapping_lock);

            gfx_result_t map_result = GFX_OK;

            if (g_gfx.primary_fb && g_gfx.primary_device->driver->ops->unmap_fb) {
                g_gfx.primary_device->driver->ops->unmap_fb(g_gfx.primary_device, g_gfx.primary_fb);
            }
            g_gfx.primary_fb = NULL;

            if (g_gfx.primary_device->driver->ops->map_fb) {
                map_result = g_gfx.primary_device->driver->ops->map_fb(
                    g_gfx.primary_device, &g_gfx.primary_fb);
            } else {
                g_gfx.primary_fb = g_gfx.primary_device->fb;
            }

            if (map_result == GFX_OK && g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) {
                g_gfx.fb_owner_pd = vmm_get_current_page_directory();
                g_gfx.fb_mapping_seq++;
                g_gfx.fb_mapping_ops++;
            }

            spinlock_release(&g_gfx.fb_mapping_lock);
            spinlock_irq_restore(irq_flags);

            if (map_result != GFX_OK || !g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
                debug_print("[GFX] Primary device set_mode succeeded but FB mapping failed (map=%d)\n",
                            map_result);
                return (map_result != GFX_OK) ? map_result : GFX_ERR_MAPPING_FAILED;
            }

            return GFX_OK;
        }
    }

    /* Fallback: if primary cannot set mode (e.g. VESA), try other devices. */
    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        gfx_device_t* dev = &g_gfx.devices[i];
        if (dev == g_gfx.primary_device || !dev->driver || !dev->driver->ops) {
            continue;
        }
        if (!dev->driver->ops->set_mode) {
            continue;
        }

        if (!dev->active && dev->driver->ops->init) {
            gfx_result_t init_result = dev->driver->ops->init(dev);
            if (init_result != GFX_OK) {
                continue;
            }
        }

        gfx_result_t result = dev->driver->ops->set_mode(dev, &mode);
        if (result == GFX_OK) {
            unsigned long irq_flags = spinlock_irq_save();
            spinlock_acquire(&g_gfx.fb_mapping_lock);

            gfx_device_t* old_dev = g_gfx.primary_device;
            if (g_gfx.primary_fb && old_dev && old_dev->driver &&
                old_dev->driver->ops && old_dev->driver->ops->unmap_fb) {
                old_dev->driver->ops->unmap_fb(old_dev, g_gfx.primary_fb);
            }
            g_gfx.primary_device = dev;
            g_gfx.primary_fb = NULL;
            gfx_result_t map_result = GFX_OK;
            if (dev->driver->ops->map_fb) {
                map_result = dev->driver->ops->map_fb(dev, &g_gfx.primary_fb);
            } else {
                g_gfx.primary_fb = dev->fb;
            }

            if (map_result == GFX_OK && g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) {
                g_gfx.fb_owner_pd = vmm_get_current_page_directory();
                g_gfx.fb_mapping_seq++;
                g_gfx.fb_mapping_ops++;
            }

            spinlock_release(&g_gfx.fb_mapping_lock);
            spinlock_irq_restore(irq_flags);

            if (map_result != GFX_OK || !g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
                debug_print("[GFX] Device '%s' mode set but FB mapping failed (map=%d)\n",
                            dev->name, map_result);
                continue;
            }

            debug_print("[GFX] Switched primary device to '%s' for mode %ux%ux%u\n",
                        dev->name, width, height, bpp);
            return GFX_OK;
        }
    }

    return first_result;
}

/* Enumerate the modes a runtime gfx_set_mode() call could actually apply.
 * Tries the primary device first (matching gfx_set_mode()'s own priority),
 * then falls back to any other probed device that implements get_modes --
 * mirroring gfx_set_mode()'s own device fallback so this reports what would
 * really succeed, e.g. the BGA device's list even when VESA (which has no
 * get_modes / always fails post-boot mode-setting) is still primary. */
gfx_result_t gfx_get_modes(gfx_mode_t** modes, uint32_t* count) {
    if (!modes || !count) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (g_gfx.primary_device && g_gfx.primary_device->driver &&
        g_gfx.primary_device->driver->ops && g_gfx.primary_device->driver->ops->get_modes) {
        gfx_result_t result = g_gfx.primary_device->driver->ops->get_modes(
            g_gfx.primary_device, modes, count);
        if (result == GFX_OK) {
            return GFX_OK;
        }
    }

    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        gfx_device_t* dev = &g_gfx.devices[i];
        if (dev == g_gfx.primary_device || !dev->driver || !dev->driver->ops ||
            !dev->driver->ops->get_modes) {
            continue;
        }
        if (dev->driver->ops->get_modes(dev, modes, count) == GFX_OK) {
            return GFX_OK;
        }
    }

    return GFX_ERR_NOT_SUPPORTED;
}

gfx_result_t gfx_get_framebuffer(gfx_framebuffer_t** fb) {
    if (!fb) {
        return GFX_ERR_INVALID_PARAM;
    }

    /* Fast path: cached framebuffer is ready and mapping is still valid. */
    if (g_gfx.primary_fb && g_gfx.primary_fb->virt_addr) {
        if (gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
            *fb = g_gfx.primary_fb;
            return GFX_OK;
        }
        g_gfx.primary_fb = NULL;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    /* If we have a cached fb but its virt_addr was never filled in, clear it
     * so the map_fb call below can set it properly. */
    if (g_gfx.primary_fb && !g_gfx.primary_fb->virt_addr) {
        gfx_log("primary_fb has NULL virt_addr — re-resolving via map_fb\n");
        g_gfx.primary_fb = NULL;
    }

    if (!g_gfx.primary_device->driver->ops->map_fb) {
        /* Driver has no map_fb; try reading from dev->fb directly. */
        if (g_gfx.primary_device->fb && g_gfx.primary_device->fb->virt_addr) {
            g_gfx.primary_fb = g_gfx.primary_device->fb;
            *fb = g_gfx.primary_fb;
            return GFX_OK;
        }
        return GFX_ERR_NOT_SUPPORTED;
    }

    gfx_result_t result = g_gfx.primary_device->driver->ops->map_fb(
        g_gfx.primary_device, &g_gfx.primary_fb);

    if (result == GFX_OK && g_gfx.primary_fb) {
        /* Apply MMIO identity-map fallback if driver left virt_addr NULL.
         * Bochs VBE at 0xF0000000 is wired by the hardware memory controller
         * so the physical address is directly usable as a virtual address. */
        if (!g_gfx.primary_fb->virt_addr) {
            uintptr_t phys = g_gfx.primary_fb->phys_addr;
            if (phys >= 0xC0000000U) {
                gfx_log("get_framebuffer: virt_addr NULL, applying MMIO identity "
                        "fallback phys=0x%08x\n", (uint32_t)phys);
                g_gfx.primary_fb->virt_addr = (void*)(uintptr_t)phys;
            } else {
                gfx_err("get_framebuffer: map_fb returned OK but virt_addr NULL "
                        "and phys=0x%08x is not MMIO\n", (uint32_t)phys);
                g_gfx.primary_fb = NULL;
                return GFX_ERR_MAPPING_FAILED;
            }
        }
        if (!gfx_is_fb_mapping_valid(g_gfx.primary_fb)) {
            gfx_err("map_fb returned framebuffer with invalid mapping\n");
            g_gfx.primary_fb = NULL;
            return GFX_ERR_MAPPING_FAILED;
        }
        *fb = g_gfx.primary_fb;
        return GFX_OK;
    }

    /* map_fb returned OK but fb pointer itself is NULL — treat as failure. */
    if (result == GFX_OK) {
        gfx_err("map_fb returned OK but framebuffer pointer is NULL\n");
        result = GFX_ERR_MAPPING_FAILED;
    }

    return result;
}

gfx_result_t gfx_clear_screen(gfx_color_t color) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!gfx_ensure_framebuffer_valid()) {
        return GFX_ERR_MAPPING_FAILED;
    }

    if (g_gfx.primary_device->driver->ops->clear) {
        gfx_result_t result = g_gfx.primary_device->driver->ops->clear(g_gfx.primary_device, color);
        if (result == GFX_OK) {
            gfx_write_validation_header();
        }
        return result;
    }
    
    /* Software fallback */
    if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
        gfx_get_framebuffer(&g_gfx.primary_fb);
        if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
            return GFX_ERR_NOT_SUPPORTED;
        }
    }
    
    uint32_t pixel = gfx_color_to_pixel(color, g_gfx.primary_fb->format);
    uint8_t* fb = (uint8_t*)g_gfx.primary_fb->virt_addr;
    uint32_t bpp = gfx_fb_bytes_per_pixel(g_gfx.primary_fb);
    if (bpp == 0) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    for (uint32_t y = 0; y < g_gfx.primary_fb->height; y++) {
        uint8_t* row = fb + y * g_gfx.primary_fb->pitch;
        
        for (uint32_t x = 0; x < g_gfx.primary_fb->width; x++) {
            switch (bpp) {
                case 4:
                    *(uint32_t*)(row + x * 4) = pixel;
                    break;
                case 3:
                    row[x * 3 + 0] = (pixel >> 0) & 0xFF;
                    row[x * 3 + 1] = (pixel >> 8) & 0xFF;
                    row[x * 3 + 2] = (pixel >> 16) & 0xFF;
                    break;
                case 2:
                    *(uint16_t*)(row + x * 2) = pixel & 0xFFFF;
                    break;
                case 1:
                    row[x] = pixel & 0xFF;
                    break;
            }
        }
    }
    
    return GFX_OK;
}

gfx_result_t gfx_draw_pixel(int32_t x, int32_t y, gfx_color_t color) {
    /* Validate framebuffer before use */
    if (!gfx_ensure_framebuffer_valid()) {
        return GFX_ERR_MAPPING_FAILED;
    }
    
    if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
        gfx_get_framebuffer(&g_gfx.primary_fb);
        if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
            return GFX_ERR_NOT_SUPPORTED;
        }
    }
    
    if (x < 0 || y < 0 || (uint32_t)x >= g_gfx.primary_fb->width || 
        (uint32_t)y >= g_gfx.primary_fb->height) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    uint32_t pixel = gfx_color_to_pixel(color, g_gfx.primary_fb->format);
    uint8_t* fb = (uint8_t*)g_gfx.primary_fb->virt_addr;
    uint32_t bpp = gfx_fb_bytes_per_pixel(g_gfx.primary_fb);
    if (bpp == 0) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    uint8_t* addr = fb + y * g_gfx.primary_fb->pitch + x * bpp;
    
    switch (bpp) {
        case 4:
            *(uint32_t*)addr = pixel;
            break;
        case 3:
            addr[0] = (pixel >> 0) & 0xFF;
            addr[1] = (pixel >> 8) & 0xFF;
            addr[2] = (pixel >> 16) & 0xFF;
            break;
        case 2:
            *(uint16_t*)addr = pixel & 0xFFFF;
            break;
        case 1:
            *addr = pixel & 0xFF;
            break;
    }
    
    return GFX_OK;
}

gfx_result_t gfx_draw_rect(const gfx_rect_t* rect, gfx_color_t color, bool filled) {
    if (!rect) {
        return GFX_ERR_INVALID_PARAM;
    }

    /* Validate framebuffer before use */
    if (!gfx_ensure_framebuffer_valid()) {
        debuglog(DEBUG_WARN, "[GFX] draw_rect: framebuffer validation failed\n");
        return GFX_ERR_MAPPING_FAILED;
    }

    /* Try hardware acceleration first */
    if (g_gfx.primary_device && g_gfx.primary_device->driver &&
        g_gfx.primary_device->driver->ops &&
        g_gfx.primary_device->driver->ops->draw_rect) {
        gfx_result_t result = g_gfx.primary_device->driver->ops->draw_rect(
            g_gfx.primary_device, rect, color, filled);
        if (result == GFX_OK) {
            return GFX_OK;
        }
    }

    /* Software fallback */
    if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
        gfx_get_framebuffer(&g_gfx.primary_fb);
        if (!g_gfx.primary_fb || !g_gfx.primary_fb->virt_addr) {
            debuglog(DEBUG_WARN, "[GFX] draw_rect: no valid framebuffer\n");
            return GFX_ERR_NOT_SUPPORTED;
        }
    }
    
    int32_t x1 = rect->x;
    int32_t y1 = rect->y;
    int32_t x2 = rect->x + rect->width;
    int32_t y2 = rect->y + rect->height;
    
    /* Clamp to screen bounds */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)g_gfx.primary_fb->width) x2 = g_gfx.primary_fb->width;
    if (y2 > (int32_t)g_gfx.primary_fb->height) y2 = g_gfx.primary_fb->height;
    
    if (x1 >= x2 || y1 >= y2) {
        return GFX_OK;  /* Nothing to draw */
    }

    uint32_t pixel = gfx_color_to_pixel(color, g_gfx.primary_fb->format);
    uint8_t* fb = (uint8_t*)g_gfx.primary_fb->virt_addr;
    uint32_t bpp = gfx_fb_bytes_per_pixel(g_gfx.primary_fb);
    if (bpp == 0) {
        debuglog(DEBUG_WARN, "[GFX] draw_rect: invalid bytes-per-pixel\n");
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (filled) {
        for (int32_t y = y1; y < y2; y++) {
            uint8_t* row = fb + y * g_gfx.primary_fb->pitch;
            for (int32_t x = x1; x < x2; x++) {
                switch (bpp) {
                    case 4: *(uint32_t*)(row + x * 4) = pixel; break;
                    case 3:
                        row[x * 3 + 0] = (pixel >> 0) & 0xFF;
                        row[x * 3 + 1] = (pixel >> 8) & 0xFF;
                        row[x * 3 + 2] = (pixel >> 16) & 0xFF;
                        break;
                    case 2: *(uint16_t*)(row + x * 2) = pixel & 0xFFFF; break;
                    case 1: row[x] = pixel & 0xFF; break;
                }
            }
        }
    } else {
        /* Draw outline */
        for (int32_t x = x1; x < x2; x++) {
            gfx_draw_pixel(x, y1, color);
            gfx_draw_pixel(x, y2 - 1, color);
        }
        for (int32_t y = y1; y < y2; y++) {
            gfx_draw_pixel(x1, y, color);
            gfx_draw_pixel(x2 - 1, y, color);
        }
    }
    
    return GFX_OK;
}

gfx_result_t gfx_swap_buffers(void) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    if (g_gfx.primary_device->driver->ops->flip) {
        return g_gfx.primary_device->driver->ops->flip(g_gfx.primary_device);
    }
    
    /* No hardware flip available - synchronize with vsync if possible */
    if (g_gfx.primary_device->driver->ops->wait_vsync) {
        return g_gfx.primary_device->driver->ops->wait_vsync(g_gfx.primary_device);
    }
    
    return GFX_OK;
}

gfx_result_t gfx_flush_framebuffer(void) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (g_gfx.primary_device->driver->ops->flush) {
        return g_gfx.primary_device->driver->ops->flush(g_gfx.primary_device);
    }

    return GFX_OK;
}

/* ============================================================================
 * 3D Acceleration API Implementation
 * ============================================================================ */

gfx_result_t gfx_create_context(void** context) {
    if (!context) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->create_context) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->create_context(g_gfx.primary_device, context);
}

gfx_result_t gfx_destroy_context(void* context) {
    if (!context) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->destroy_context) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->destroy_context(g_gfx.primary_device, context);
}

void gfx_panic_display(const char* message) {
    gfx_framebuffer_t* fb = g_gfx.primary_fb;
    if (!fb || !fb->virt_addr || fb->width == 0 || fb->height == 0) {
        return;
    }

    uint32_t bpp = (fb->bpp + 7) / 8;
    if (bpp == 0) bpp = 4;

    uint32_t red = 0x00AA0000;
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t* row = (uint8_t*)fb->virt_addr + y * fb->pitch;
        for (uint32_t x = 0; x < fb->width; x++) {
            uint8_t* addr = row + x * bpp;
            switch (bpp) {
                case 4: *(uint32_t*)addr = red; break;
                case 3: addr[0] = 0x00; addr[1] = 0x00; addr[2] = 0xAA; break;
                case 2: *(uint16_t*)addr = (uint16_t)(red & 0xFFFF); break;
                case 1: *addr = 0x04; break;
            }
        }
    }

    if (!message || !message[0]) return;

    uint32_t char_w = 8;
    uint32_t char_h = 16;
    uint32_t margin = 20;
    uint32_t max_chars = (fb->width - 2 * margin) / char_w;
    if (max_chars == 0) max_chars = 1;

    uint32_t white = 0x00FFFFFF;
    uint32_t y_pos = margin;

    const char* p = message;
    while (*p && y_pos + char_h + margin <= fb->height) {
        const char* line_end = p;
        uint32_t line_len = 0;
        while (*line_end && *line_end != '\n' && line_len < max_chars) {
            line_end++;
            line_len++;
        }

        for (uint32_t ci = 0; ci < line_len; ci++) {
            char ch = p[ci];
            uint32_t cx = margin + ci * char_w;
            for (uint32_t py = 0; py < char_h; py++) {
                for (uint32_t px = 0; px < char_w; px++) {
                    bool draw = false;
                    if (ch >= 'A' && ch <= 'Z') draw = true;
                    else if (ch >= 'a' && ch <= 'z') draw = true;
                    else if (ch >= '0' && ch <= '9') draw = true;
                    else if (ch == ' ' || ch == '.' || ch == ',' || ch == ':' ||
                             ch == '!' || ch == '-' || ch == '_' || ch == '/') draw = false;
                    else draw = true;

                    if (draw && py >= 2 && py < char_h - 2 && px >= 1 && px < char_w - 1) {
                        uint32_t sx = cx + px;
                        uint32_t sy = y_pos + py;
                        if (sx < fb->width && sy < fb->height) {
                            uint8_t* addr = (uint8_t*)fb->virt_addr + sy * fb->pitch + sx * bpp;
                            switch (bpp) {
                                case 4: *(uint32_t*)addr = white; break;
                                case 3: addr[0] = 0xFF; addr[1] = 0xFF; addr[2] = 0xFF; break;
                                case 2: *(uint16_t*)addr = (uint16_t)(white & 0xFFFF); break;
                                case 1: *addr = 0x0F; break;
                            }
                        }
                    }
                }
            }
        }

        y_pos += char_h + 4;
        p = line_end;
        if (*p == '\n') p++;
    }
}

gfx_result_t gfx_make_current(void* context) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->make_current) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->make_current(g_gfx.primary_device, context);
}

gfx_result_t gfx_swap_3d_buffers(void) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->swap_buffers) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->swap_buffers(g_gfx.primary_device);
}

gfx_result_t gfx_create_shader(uint32_t type, const char* source, void** shader) {
    if (!source || !shader) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->create_shader) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->create_shader(g_gfx.primary_device, type, source, shader);
}

gfx_result_t gfx_destroy_shader(void* shader) {
    if (!shader) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->destroy_shader) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->destroy_shader(g_gfx.primary_device, shader);
}

gfx_result_t gfx_create_program(void** program) {
    if (!program) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->create_program) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->create_program(g_gfx.primary_device, program);
}

gfx_result_t gfx_attach_shader(void* program, void* shader) {
    if (!program || !shader) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->attach_shader) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->attach_shader(g_gfx.primary_device, program, shader);
}

gfx_result_t gfx_link_program(void* program) {
    if (!program) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->link_program) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->link_program(g_gfx.primary_device, program);
}

gfx_result_t gfx_use_program(void* program) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->use_program) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->use_program(g_gfx.primary_device, program);
}

gfx_result_t gfx_create_buffer(uint32_t target, size_t size, const void* data, uint32_t usage, void** buffer) {
    if (!buffer) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->create_buffer) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->create_buffer(g_gfx.primary_device, target, size, data, usage, buffer);
}

gfx_result_t gfx_bind_buffer(uint32_t target, void* buffer) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->bind_buffer) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->bind_buffer(g_gfx.primary_device, target, buffer);
}

gfx_result_t gfx_buffer_data(uint32_t target, size_t size, const void* data) {
    if (!data) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->buffer_data) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->buffer_data(g_gfx.primary_device, target, size, data);
}

gfx_result_t gfx_buffer_sub_data(uint32_t target, size_t offset, size_t size, const void* data) {
    if (!data) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->buffer_sub_data) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->buffer_sub_data(g_gfx.primary_device, target, offset, size, data);
}

gfx_result_t gfx_destroy_buffer(void* buffer) {
    if (!buffer) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->destroy_buffer) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->destroy_buffer(g_gfx.primary_device, buffer);
}

gfx_result_t gfx_enable_vertex_attrib_array(uint32_t index) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->enable_vertex_attrib_array) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->enable_vertex_attrib_array(g_gfx.primary_device, index);
}

gfx_result_t gfx_disable_vertex_attrib_array(uint32_t index) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->disable_vertex_attrib_array) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->disable_vertex_attrib_array(g_gfx.primary_device, index);
}

gfx_result_t gfx_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type, bool normalized, int32_t stride, const void* pointer) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->vertex_attrib_pointer) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->vertex_attrib_pointer(g_gfx.primary_device, index, size, type, normalized, stride, pointer);
}

gfx_result_t gfx_get_uniform_location(void* program, const char* name, int32_t* location) {
    if (!program || !name || !location) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->get_uniform_location) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->get_uniform_location(g_gfx.primary_device, program, name, location);
}

gfx_result_t gfx_uniform1f(int32_t location, float value) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->uniform1f) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->uniform1f(g_gfx.primary_device, location, (float)value);
}

gfx_result_t gfx_uniform1i(int32_t location, int32_t value) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->uniform1i) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->uniform1i(g_gfx.primary_device, location, value);
}

gfx_result_t gfx_uniform2f(int32_t location, float x, float y) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->uniform2f) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->uniform2f(g_gfx.primary_device, location, (float)x, (float)y);
}

gfx_result_t gfx_uniform3f(int32_t location, float x, float y, float z) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->uniform3f) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->uniform3f(g_gfx.primary_device, location, (float)x, (float)y, (float)z);
}

gfx_result_t gfx_uniform4f(int32_t location, float x, float y, float z, float w) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->uniform4f) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->uniform4f(g_gfx.primary_device, location, (float)x, (float)y, (float)z, (float)w);
}

gfx_result_t gfx_uniform_matrix4fv(int32_t location, bool transpose, const float* value) {
    if (!value) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->uniform_matrix4fv) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->uniform_matrix4fv(g_gfx.primary_device, location, transpose, (const float*)value);
}

gfx_result_t gfx_draw_arrays(uint32_t mode, int32_t first, int32_t count) {
    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->draw_arrays) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->draw_arrays(g_gfx.primary_device, mode, first, count);
}

gfx_result_t gfx_draw_elements(uint32_t mode, int32_t count, uint32_t type, const void* indices) {
    if (!indices) {
        return GFX_ERR_INVALID_PARAM;
    }

    if (!g_gfx.primary_device || !g_gfx.primary_device->driver) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    if (!g_gfx.primary_device->driver->ops->draw_elements) {
        return GFX_ERR_NOT_SUPPORTED;
    }

    return g_gfx.primary_device->driver->ops->draw_elements(g_gfx.primary_device, mode, count, type, indices);
}

/* ============================================================================
 * Legacy Compatibility Functions
 * ============================================================================ */

/**
 * Get framebuffer info for legacy code
 */
void* gfx_get_fb_addr(void) {
    if (g_gfx.primary_fb) {
        return g_gfx.primary_fb->virt_addr;
    }
    
    gfx_framebuffer_t* fb = NULL;
    if (gfx_get_framebuffer(&fb) == GFX_OK && fb) {
        return fb->virt_addr;
    }
    
    return NULL;
}

uint32_t gfx_get_fb_width(void) {
    if (g_gfx.primary_fb) {
        return g_gfx.primary_fb->width;
    }
    return 0;
}

uint32_t gfx_get_fb_height(void) {
    if (g_gfx.primary_fb) {
        return g_gfx.primary_fb->height;
    }
    return 0;
}

uint32_t gfx_get_fb_pitch(void) {
    if (g_gfx.primary_fb) {
        return g_gfx.primary_fb->pitch;
    }
    return 0;
}

uint32_t gfx_get_fb_bpp(void) {
    if (g_gfx.primary_fb) {
        return g_gfx.primary_fb->bpp;
    }
    return 0;
}

bool gfx_is_initialized(void) {
    return g_gfx.initialized;
}

/**
 * Print graphics system status
 */
void gfx_print_status(void) {
    debug_print("=== Graphics System Status ===\n");
    debug_print("Initialized: %s\n", g_gfx.initialized ? "yes" : "no");
    debug_print("Environment: %s\n", g_gfx.is_vm ? "virtual machine" : "bare metal");
    debug_print("Registered drivers: %u\n", g_gfx.driver_count);
    debug_print("Detected devices: %u\n", g_gfx.device_count);
    debug_print("Blacklisted drivers: %u\n", g_gfx.blacklist_count);
    
    for (uint32_t i = 0; i < g_gfx.device_count; i++) {
        gfx_device_t* dev = &g_gfx.devices[i];
        debug_print("  Device %u: %s\n", i, dev->name);
        debug_print("    Type: %u, Active: %s\n", dev->type, dev->active ? "yes" : "no");
        if (dev->driver) {
            debug_print("    Driver: %s (priority=%u)\n", dev->driver->ops->name, dev->driver->priority);
        }
    }
    
    if (g_gfx.primary_device) {
        debug_print("Primary device: %s\n", g_gfx.primary_device->name);
    }
    
    if (g_gfx.primary_fb) {
        debug_print("Framebuffer: %ux%ux%u @ %p (pitch=%u)\n",
                    g_gfx.primary_fb->width, g_gfx.primary_fb->height,
                    g_gfx.primary_fb->bpp, g_gfx.primary_fb->virt_addr,
                    g_gfx.primary_fb->pitch);
    }
    
    debug_print("==============================\n");
}

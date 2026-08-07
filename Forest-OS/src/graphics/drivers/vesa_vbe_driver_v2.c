/**
 * Fern - VESA VBE Driver V2
 * 
 * This driver handles VESA BIOS Extensions framebuffers, primarily using
 * the framebuffer information provided by the bootloader (GRUB multiboot).
 * 
 * In protected mode, we cannot call BIOS functions, so this driver:
 * 1. Uses the framebuffer pre-configured by GRUB via multiboot info
 * 2. Provides a simple linear framebuffer interface
 * 3. Cannot change video modes at runtime (mode is set at boot)
 * 
 * For UEFI boot, the GOP framebuffer is used instead, but this driver
 * can still operate on that framebuffer.
 */

#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/graphics_hw_regs.h"
#include "../include/memory.h"
#include "../include/debug.h"
#include "../include/debuglog.h"
#include "../include/string.h"
#include "../include/multiboot.h"

/* Debug logging macros */
#define vesa_log(fmt, ...) debuglog(DEBUG_INFO, "[VESA] " fmt, ##__VA_ARGS__)
#define vesa_warn(fmt, ...) debuglog(DEBUG_WARN, "[VESA] " fmt, ##__VA_ARGS__)
#define vesa_err(fmt, ...) debuglog(DEBUG_ERROR, "[VESA] " fmt, ##__VA_ARGS__)

/* VESA VBE Information Block (from BIOS, for reference) */
typedef struct __attribute__((packed)) {
    char signature[4];          /* "VESA" */
    uint16_t version;           /* VBE version (e.g., 0x0300) */
    uint32_t oem_string;        /* Far pointer to OEM string */
    uint32_t capabilities;      /* Capabilities */
    uint32_t video_modes;       /* Far pointer to mode list */
    uint16_t total_memory;      /* 64KB blocks of video memory */
    uint16_t software_rev;      /* VBE software revision */
    uint32_t vendor;            /* Far pointer to vendor name */
    uint32_t product_name;      /* Far pointer to product name */
    uint32_t product_rev;       /* Far pointer to product revision */
    uint8_t reserved[222];
    uint8_t oem_data[256];
} vbe_info_block_t;

/* VESA Mode Information Block */
typedef struct __attribute__((packed)) {
    uint16_t mode_attributes;
    uint8_t win_a_attributes;
    uint8_t win_b_attributes;
    uint16_t win_granularity;
    uint16_t win_size;
    uint16_t win_a_segment;
    uint16_t win_b_segment;
    uint32_t win_func_ptr;
    uint16_t bytes_per_scanline;
    
    /* VBE 1.2+ */
    uint16_t x_resolution;
    uint16_t y_resolution;
    uint8_t x_char_size;
    uint8_t y_char_size;
    uint8_t number_of_planes;
    uint8_t bits_per_pixel;
    uint8_t number_of_banks;
    uint8_t memory_model;
    uint8_t bank_size;
    uint8_t number_of_image_pages;
    uint8_t reserved1;
    
    /* Direct color fields */
    uint8_t red_mask_size;
    uint8_t red_field_position;
    uint8_t green_mask_size;
    uint8_t green_field_position;
    uint8_t blue_mask_size;
    uint8_t blue_field_position;
    uint8_t rsvd_mask_size;
    uint8_t rsvd_field_position;
    uint8_t direct_color_mode_info;
    
    /* VBE 2.0+ */
    uint32_t phys_base_ptr;
    uint32_t reserved2;
    uint16_t reserved3;
    
    /* VBE 3.0+ */
    uint16_t lin_bytes_per_scanline;
    uint8_t bank_number_of_image_pages;
    uint8_t lin_number_of_image_pages;
    uint8_t lin_red_mask_size;
    uint8_t lin_red_field_position;
    uint8_t lin_green_mask_size;
    uint8_t lin_green_field_position;
    uint8_t lin_blue_mask_size;
    uint8_t lin_blue_field_position;
    uint8_t lin_rsvd_mask_size;
    uint8_t lin_rsvd_field_position;
    uint32_t max_pixel_clock;
    
    uint8_t reserved4[189];
} vbe_mode_info_t;

/* Driver-private data structure */
typedef struct {
    /* Framebuffer from bootloader */
    uintptr_t fb_phys;          /* Physical address */
    void* fb_virt;              /* Virtual address (mapped) */
    uint32_t fb_size;           /* Total size */
    
    /* Mode info */
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    
    /* Color format */
    uint8_t red_mask_size;
    uint8_t red_position;
    uint8_t green_mask_size;
    uint8_t green_position;
    uint8_t blue_mask_size;
    uint8_t blue_position;
    
    /* Mode type */
    bool is_text_mode;
    bool is_lfb;                /* Linear framebuffer */
    
    /* Framebuffer tracking */
    gfx_framebuffer_t framebuffer;
} vesa_private_t;

/* External multiboot info (set by kernel) */
extern multiboot_info_t* g_multiboot_info;
extern void* g_multiboot_framebuffer;
extern uint32_t g_multiboot_fb_width;
extern uint32_t g_multiboot_fb_height;
extern uint32_t g_multiboot_fb_pitch;
extern uint32_t g_multiboot_fb_bpp;
extern uintptr_t g_multiboot_fb_addr;
extern uint32_t g_multiboot_magic;
extern uint32_t g_multiboot_info_addr;

/* Forward declarations */
static gfx_result_t vesa_probe(gfx_device_t* dev);
static gfx_result_t vesa_init(gfx_device_t* dev);
static gfx_result_t vesa_shutdown(gfx_device_t* dev);
static gfx_result_t vesa_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count);
static gfx_result_t vesa_set_mode(gfx_device_t* dev, const gfx_mode_t* mode);
static gfx_result_t vesa_get_mode(gfx_device_t* dev, gfx_mode_t* mode);
static gfx_result_t vesa_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb);
static gfx_result_t vesa_unmap_fb(gfx_device_t* dev, gfx_framebuffer_t* fb);
static gfx_result_t vesa_clear(gfx_device_t* dev, gfx_color_t color);
static gfx_result_t vesa_draw_pixel(gfx_device_t* dev, int32_t x, int32_t y, gfx_color_t c);
static gfx_result_t vesa_wait_vsync(gfx_device_t* dev);
static gfx_result_t vesa_flush(gfx_device_t* dev);

/* Driver operations table */
static const gfx_driver_ops_t vesa_driver_ops = {
    .name = "vesa-vbe",
    .version = 0x00020000,
    
    .probe = vesa_probe,
    .init = vesa_init,
    .shutdown = vesa_shutdown,
    .reset = NULL,
    
    .get_modes = vesa_get_modes,
    .set_mode = vesa_set_mode,
    .get_mode = vesa_get_mode,
    
    .map_fb = vesa_map_fb,
    .unmap_fb = vesa_unmap_fb,
    .set_fb_offset = NULL,
    
    .clear = vesa_clear,
    .draw_pixel = vesa_draw_pixel,
    .flush = vesa_flush,
    .draw_rect = NULL,
    .blit = NULL,
    
    .set_cursor = NULL,
    .move_cursor = NULL,
    .show_cursor = NULL,
    
    .write_char = NULL,
    .set_text_cursor = NULL,
    .scroll = NULL,
    
    .wait_vsync = vesa_wait_vsync,
    .flip = NULL,
    
    .read_edid = NULL,
    .detect_displays = NULL,
    
    .set_dpms = NULL,
    .ioctl = NULL,
};

/* Driver instance */
DECLARE_GFX_DRIVER(vesa, &vesa_driver_ops, GFX_DEVICE_VESA);

/* ============================================================================
 * Multiboot Framebuffer Parsing
 * ============================================================================ */

/**
 * Parse multiboot framebuffer information
 * Uses global variables set by the kernel during early boot
 */
static bool vesa_apply_boot_fb(vesa_private_t* priv,
                               uintptr_t fb_addr,
                               uint32_t width,
                               uint32_t height,
                               uint32_t pitch,
                               uint32_t bpp,
                               uint8_t fb_type,
                               const char* source) {
    if (!priv || fb_addr == 0 || width == 0 || height == 0 || bpp == 0) {
        return false;
    }

    /* Multiboot type 2 is EGA text. Let VGA text driver handle it. */
    if (fb_type == 2) {
        return false;
    }

    priv->fb_phys = fb_addr;
    priv->width = width;
    priv->height = height;
    priv->bpp = bpp;
    priv->pitch = pitch ? pitch : (width * ((bpp + 7) / 8));
    priv->fb_size = priv->pitch * priv->height;

    /* Use kernel-provided virtual mapping when it matches this framebuffer. */
    if (g_multiboot_framebuffer != NULL && g_multiboot_fb_addr == fb_addr) {
        priv->fb_virt = g_multiboot_framebuffer;
    } else if (g_multiboot_framebuffer != NULL && g_multiboot_fb_addr == 0) {
        /* Partial init: g_multiboot_framebuffer set but addr not matched yet */
        priv->fb_virt = g_multiboot_framebuffer;
    } else if (fb_addr < MEMORY_USER_START) {
        /* Physical address below 1GB = within identity-mapped range, safe to use directly */
        priv->fb_virt = (void*)fb_addr;
    } else {
        /* Physical address above identity-map limit: must be mapped.
         * kernel_finalize_framebuffer_mapping() should have done this already.
         * If g_multiboot_framebuffer is set, use it; otherwise map now at 0xF0000000. */
        if (g_multiboot_framebuffer != NULL) {
            priv->fb_virt = g_multiboot_framebuffer;
        } else {
            /* Last-resort: identity-map attempt failed upstream.
             * Try the fixed virtual window 0xF0000000 — it may have been
             * partially mapped by kernel_finalize_framebuffer_mapping(). */
            priv->fb_virt = (void*)0xF0000000;
        }
    }

    priv->is_text_mode = false;
    priv->is_lfb = true;

    /* Set default color positions based on BPP */
    if (priv->bpp == 32 || priv->bpp == 24) {
        priv->blue_position = 0;
        priv->blue_mask_size = 8;
        priv->green_position = 8;
        priv->green_mask_size = 8;
        priv->red_position = 16;
        priv->red_mask_size = 8;
    } else if (priv->bpp == 16) {
        priv->blue_position = 0;
        priv->blue_mask_size = 5;
        priv->green_position = 5;
        priv->green_mask_size = 6;
        priv->red_position = 11;
        priv->red_mask_size = 5;
    } else if (priv->bpp == 15) {
        priv->blue_position = 0;
        priv->blue_mask_size = 5;
        priv->green_position = 5;
        priv->green_mask_size = 5;
        priv->red_position = 10;
        priv->red_mask_size = 5;
    }

    debug_print("[VESA] Using %s framebuffer: %ux%ux%u @ 0x%08x\n",
                source, priv->width, priv->height, priv->bpp, (uint32_t)priv->fb_phys);
    return true;
}

static bool vesa_parse_multiboot(vesa_private_t* priv, gfx_device_t* dev) {
    (void)dev;  /* Unused parameter */
    
    /* Check global variables (set during early boot from multiboot info) */
    if (g_multiboot_fb_addr != 0 && g_multiboot_fb_width > 0) {
        if (vesa_apply_boot_fb(priv,
                               g_multiboot_fb_addr,
                               g_multiboot_fb_width,
                               g_multiboot_fb_height,
                               g_multiboot_fb_pitch,
                               g_multiboot_fb_bpp,
                               1,
                               "kernel-global")) {
            return true;
        }
    }

    /* Fallback for 32-bit multiboot1 path: parse framebuffer directly. */
    if (g_multiboot_info &&
        (g_multiboot_info->flags & MULTIBOOT_FLAG_FRAMEBUFFER)) {
        if (vesa_apply_boot_fb(priv,
                               (uintptr_t)g_multiboot_info->framebuffer_addr,
                               g_multiboot_info->framebuffer_width,
                               g_multiboot_info->framebuffer_height,
                               g_multiboot_info->framebuffer_pitch,
                               g_multiboot_info->framebuffer_bpp,
                               g_multiboot_info->framebuffer_type,
                               "multiboot1")) {
            return true;
        }
    }

    /* Fallback for multiboot2 path: parse framebuffer tag directly. */
    if (g_multiboot_magic == MULTIBOOT2_BOOTLOADER_MAGIC &&
        g_multiboot_info_addr != 0) {
        multiboot2_info_t* hdr = (multiboot2_info_t*)g_multiboot_info_addr;
        uint8_t* cursor = (uint8_t*)g_multiboot_info_addr + sizeof(multiboot2_info_t);
        uint8_t* end = (uint8_t*)g_multiboot_info_addr + hdr->total_size;

        while (cursor < end) {
            multiboot2_tag_t* tag = (multiboot2_tag_t*)cursor;
            if (tag->size < sizeof(multiboot2_tag_t)) {
                break;
            }
            if (tag->type == MULTIBOOT2_TAG_END) {
                break;
            }
            if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
                multiboot2_tag_framebuffer_t* fb_tag = (multiboot2_tag_framebuffer_t*)tag;
                if (vesa_apply_boot_fb(priv,
                                       (uintptr_t)fb_tag->framebuffer_addr,
                                       fb_tag->framebuffer_width,
                                       fb_tag->framebuffer_height,
                                       fb_tag->framebuffer_pitch,
                                       fb_tag->framebuffer_bpp,
                                       fb_tag->framebuffer_type,
                                       "multiboot2")) {
                    return true;
                }
            }
            uint32_t advance = (tag->size + 7) & ~7;
            if (advance == 0 || cursor + advance > end) {
                break;
            }
            cursor += advance;
        }
    }
    
    debug_print("[VESA] No framebuffer info available from bootloader\n");
    return false;
}

/**
 * Determine pixel format from color masks
 */
static gfx_pixel_format_t vesa_determine_format(vesa_private_t* priv) {
    if (priv->is_text_mode) {
        return GFX_FORMAT_TEXT;
    }
    
    switch (priv->bpp) {
        case 8:
            return GFX_FORMAT_INDEXED_8;
            
        case 15:
        case 16:
            /* Check if it's 555 or 565 
             * red_position > blue_position means Blue at low address (BGR memory order) */
            if (priv->green_mask_size == 6) {
                return (priv->red_position > priv->blue_position) ? 
                       GFX_FORMAT_BGR565 : GFX_FORMAT_RGB565;
            } else {
                return (priv->red_position > priv->blue_position) ? 
                       GFX_FORMAT_BGR555 : GFX_FORMAT_RGB555;
            }
            
        case 24:
            /* red_position > blue_position means Blue at low address (BGR memory order) */
            return (priv->red_position > priv->blue_position) ? 
                   GFX_FORMAT_BGR888 : GFX_FORMAT_RGB888;
            
        case 32:
            /* Check red/blue position to determine RGB vs BGR
             * red_position > blue_position means Blue at low address (BGR memory order) */
            if (priv->red_position == 0 || priv->red_position == 16) {
                return (priv->red_position > priv->blue_position) ? 
                       GFX_FORMAT_BGRX8888 : GFX_FORMAT_RGBX8888;
            }
            return GFX_FORMAT_BGRX8888;  /* Default */
            
        default:
            return GFX_FORMAT_UNKNOWN;
    }
}

/* ============================================================================
 * Driver Implementation
 * ============================================================================ */

static gfx_result_t vesa_probe(gfx_device_t* dev) {
    /* Allocate private data */
    vesa_private_t* priv = (vesa_private_t*)kmalloc(sizeof(vesa_private_t));
    if (!priv) {
        return GFX_ERR_NO_MEMORY;
    }
    memset(priv, 0, sizeof(vesa_private_t));
    
    /* Parse multiboot framebuffer info */
    if (!vesa_parse_multiboot(priv, dev)) {
        kfree(priv);
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Skip if it's a text mode framebuffer (let VGA driver handle it) */
    if (priv->is_text_mode) {
        kfree(priv);
        debug_print("[VESA] Text mode detected, deferring to VGA driver\n");
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    /* Set defaults if color info wasn't provided */
    if (priv->red_mask_size == 0 && priv->bpp >= 15) {
        switch (priv->bpp) {
            case 15:
                priv->red_mask_size = priv->green_mask_size = priv->blue_mask_size = 5;
                priv->red_position = 10;
                priv->green_position = 5;
                priv->blue_position = 0;
                break;
            case 16:
                priv->red_mask_size = 5;
                priv->green_mask_size = 6;
                priv->blue_mask_size = 5;
                priv->red_position = 11;
                priv->green_position = 5;
                priv->blue_position = 0;
                break;
            case 24:
            case 32:
                priv->red_mask_size = priv->green_mask_size = priv->blue_mask_size = 8;
                priv->blue_position = 0;
                priv->green_position = 8;
                priv->red_position = 16;
                break;
        }
    }
    
    /* Fill in device info */
    dev->type = GFX_DEVICE_VESA;
    dev->fb_base = priv->fb_phys;
    dev->fb_size = priv->fb_size;
    dev->vram_size = priv->fb_size;
    dev->max_width = priv->width;
    dev->max_height = priv->height;
    dev->max_bpp = priv->bpp;
    dev->driver_data = priv;
    
    /* Set capabilities */
    dev->caps = GFX_CAP_LINEAR_FB;
    
    strncpy(dev->name, "VESA VBE Linear Framebuffer", sizeof(dev->name) - 1);
    
    /* Set up framebuffer structure */
    priv->framebuffer.phys_addr = priv->fb_phys;
    priv->framebuffer.width = priv->width;
    priv->framebuffer.height = priv->height;
    priv->framebuffer.pitch = priv->pitch;
    priv->framebuffer.bpp = priv->bpp;
    priv->framebuffer.size = priv->fb_size;
    priv->framebuffer.format = vesa_determine_format(priv);
    priv->framebuffer.virt_addr = priv->fb_virt;
    
    debug_print("[VESA] Probe successful: %s (format: %d)\n", 
                dev->name, priv->framebuffer.format);
    
    return GFX_OK;
}

/**
 * Resolve the virtual address for the VESA framebuffer.
 *
 * Priority:
 *  1. Already resolved in priv->fb_virt — reuse it.
 *  2. g_multiboot_framebuffer matches the physical address — use it.
 *  3. g_multiboot_framebuffer is set (addr mismatch or partial init) — use it.
 *  4. Physical address is in the identity-mapped low range (< MEMORY_USER_START) — cast directly.
 *  5. High physical address (e.g. 0xFD000000) — use the fixed kernel window 0xF0000000
 *     that kernel_finalize_framebuffer_mapping() established.
 *
 * Never calls map_physical_memory() with a high physical address, because that
 * macro is just a cast and would create a dangling pointer into unmapped space.
 */
static void* vesa_resolve_virt(vesa_private_t* priv) {
    /* 1. Already done. */
    if (priv->fb_virt) {
        return priv->fb_virt;
    }

    /* 2 & 3. Kernel has already mapped the framebuffer — trust its pointer. */
    if (g_multiboot_framebuffer != NULL) {
        return g_multiboot_framebuffer;
    }

    /* 4. Low physical address is covered by identity mapping. */
    if (priv->fb_phys < MEMORY_USER_START) {
        return (void*)(uintptr_t)priv->fb_phys;
    }

    /* 5. High address: kernel_finalize_framebuffer_mapping() maps the physical
     *    framebuffer to the fixed window 0xF0000000.  Use that virtual address
     *    rather than the physical address (which would be an invalid pointer). */
    return (void*)0xF0000000;
}

static bool vesa_mapping_matches(vesa_private_t* priv, void* virt_addr) {
    if (!priv || !virt_addr || !priv->fb_phys || priv->pitch == 0 || priv->height == 0) {
        return false;
    }
    page_directory_t* pd = vmm_get_current_page_directory();
    if (!pd) {
        return false;
    }

    uint32_t virt_page = ((uint32_t)(uintptr_t)virt_addr) & ~MEMORY_PAGE_MASK;
    uint32_t phys_page = ((uint32_t)priv->fb_phys) & ~MEMORY_PAGE_MASK;
    uint32_t phys_offset = ((uint32_t)priv->fb_phys) & MEMORY_PAGE_MASK;
    uint64_t visible_size64 = priv->fb_size ? (uint64_t)priv->fb_size :
                              ((uint64_t)priv->pitch * (uint64_t)priv->height);
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
            /* vmm_get_physical_addr() only knows about pages our VMM explicitly
             * tracked.  BIOS/firmware MMIO windows (e.g. the Bochs VBE aperture
             * at 0xF0000000) are mapped by the hardware memory controller and
             * will never appear in our software page tables with the right
             * physical address.  If the query returned 0 (not present in our
             * tables) AND the virtual address is in the high MMIO range
             * (>= 0xC0000000), treat it as "probably mapped by firmware" and
             * skip the mismatch rather than failing the whole check. */
            if (mapped == 0 && vaddr >= 0xC0000000U) {
                continue;
            }
            return false;
        }
    }
    return true;
}

static bool vesa_repair_mapping(vesa_private_t* priv, void* virt_addr) {
    if (!priv || !virt_addr || !priv->fb_phys || priv->pitch == 0 || priv->height == 0) {
        return false;
    }

    if (vesa_mapping_matches(priv, virt_addr)) {
        return true;
    }

    page_directory_t* pd = vmm_get_current_page_directory();
    if (!pd) {
        return false;
    }

    uint32_t virt_page = ((uint32_t)(uintptr_t)virt_addr) & ~MEMORY_PAGE_MASK;
    uint32_t phys_page = ((uint32_t)priv->fb_phys) & ~MEMORY_PAGE_MASK;
    uint32_t phys_offset = ((uint32_t)priv->fb_phys) & MEMORY_PAGE_MASK;
    uint64_t visible_size64 = priv->fb_size ? (uint64_t)priv->fb_size :
                              ((uint64_t)priv->pitch * (uint64_t)priv->height);
    uint64_t map_size64 = visible_size64 + (uint64_t)phys_offset;
    if (map_size64 == 0 || map_size64 > 0xFFFFFFFFULL) {
        vesa_err("mapping repair size overflow: visible=%u offset=%u\n",
                 priv->fb_size ? priv->fb_size : (priv->pitch * priv->height), phys_offset);
        return false;
    }
    uint32_t map_size = (uint32_t)map_size64;
    uint32_t page_count = (map_size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
    if (page_count == 0) {
        return false;
    }

    bool failed = false;
    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t vaddr = virt_page + (i * MEMORY_PAGE_SIZE);
        uint32_t paddr = phys_page + (i * MEMORY_PAGE_SIZE);
        uint32_t mapped = vmm_get_physical_addr(pd, vaddr);
        if (mapped != paddr && mapped != 0) {
            vmm_unmap_page(pd, vaddr);
        }
        memory_result_t res = vmm_map_page(pd, vaddr, paddr, PAGE_PRESENT | PAGE_WRITABLE);
        if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
            vesa_err("mapping repair failed: v=0x%08x p=0x%08x res=%d\n", vaddr, paddr, res);
            failed = true;
        }
        __asm__ __volatile__("invlpg (%0)" :: "r"(vaddr) : "memory");
    }

    if (failed) {
        return false;
    }

    return vesa_mapping_matches(priv, virt_addr);
}

static gfx_result_t vesa_init(gfx_device_t* dev) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;

    if (!priv) {
        return GFX_ERR_INVALID_PARAM;
    }

    /* Resolve virtual address for the framebuffer if not already set. */
    if (!priv->fb_virt) {
        priv->fb_virt = vesa_resolve_virt(priv);
        if (!priv->fb_virt) {
            vesa_err("Failed to resolve framebuffer virtual address (phys=0x%08x)\n",
                     (uint32_t)priv->fb_phys);
            return GFX_ERR_MAPPING_FAILED;
        }
    }
    /* Keep framebuffer struct in sync. */
    priv->framebuffer.virt_addr = priv->fb_virt;

    if (!vesa_mapping_matches(priv, priv->fb_virt) &&
        !vesa_repair_mapping(priv, priv->fb_virt)) {
        /* Both the page-table check and the VMM-based repair failed.  Before
         * returning an error, probe the virtual address directly: BIOS/QEMU
         * may have wired this MMIO aperture in hardware even though our VMM
         * has no record of it.  A successful volatile round-trip confirms the
         * mapping is live and we can proceed safely. */
        volatile uint32_t* probe = (volatile uint32_t*)priv->fb_virt;
        uint32_t saved  = *probe;        /* read current pixel word */
        uint32_t canary = 0xDEADF00DU;
        *probe = canary;                 /* write test value */
        uint32_t readback = *probe;      /* read it back */
        *probe = saved;                  /* restore original content */
        if (readback == canary) {
            vesa_log("Mapping probe succeeded (virt=%p phys=0x%08x) - "
                     "MMIO live despite missing VMM entry, proceeding\n",
                     priv->fb_virt, (uint32_t)priv->fb_phys);
        } else {
            vesa_err("Failed to validate/repair framebuffer mapping "
                     "virt=%p phys=0x%08x (probe readback=0x%08x)\n",
                     priv->fb_virt, (uint32_t)priv->fb_phys, readback);
            return GFX_ERR_MAPPING_FAILED;
        }
    }

    /* Set up current mode */
    dev->current_mode.width = priv->width;
    dev->current_mode.height = priv->height;
    dev->current_mode.bpp = priv->bpp;
    dev->current_mode.pitch = priv->pitch;
    dev->current_mode.format = priv->framebuffer.format;
    dev->current_mode.refresh_hz = 60;

    dev->fb = &priv->framebuffer;
    dev->active = true;

    vesa_log("Initialized: %ux%ux%u @ %p (phys=0x%08x)\n",
             priv->width, priv->height, priv->bpp,
             priv->fb_virt, (uint32_t)priv->fb_phys);

    return GFX_OK;
}

static gfx_result_t vesa_shutdown(gfx_device_t* dev) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;
    
    if (priv) {
        kfree(priv);
        dev->driver_data = NULL;
    }
    
    return GFX_OK;
}

static gfx_result_t vesa_get_modes(gfx_device_t* dev, gfx_mode_t** modes, uint32_t* count) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;
    
    /* In protected mode, we can only report the current mode */
    gfx_mode_t* mode_list = (gfx_mode_t*)kmalloc(sizeof(gfx_mode_t));
    if (!mode_list) {
        return GFX_ERR_NO_MEMORY;
    }
    
    mode_list[0].mode_id = 0;
    mode_list[0].width = priv->width;
    mode_list[0].height = priv->height;
    mode_list[0].bpp = priv->bpp;
    mode_list[0].pitch = priv->pitch;
    mode_list[0].format = priv->framebuffer.format;
    mode_list[0].refresh_hz = 60;
    mode_list[0].is_text_mode = false;
    
    *modes = mode_list;
    *count = 1;
    
    return GFX_OK;
}

static gfx_result_t vesa_set_mode(gfx_device_t* dev, const gfx_mode_t* mode) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;
    
    if (!mode || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    /* In protected mode, we can't change modes */
    /* Just verify the requested mode matches current */
    if (mode->width != priv->width || mode->height != priv->height || 
        mode->bpp != priv->bpp) {
        debug_print("[VESA] Cannot change modes in protected mode\n");
        debug_print("[VESA] Requested: %ux%ux%u, Current: %ux%ux%u\n",
                    mode->width, mode->height, mode->bpp,
                    priv->width, priv->height, priv->bpp);
        return GFX_ERR_NOT_SUPPORTED;
    }
    
    return GFX_OK;
}

static gfx_result_t vesa_get_mode(gfx_device_t* dev, gfx_mode_t* mode) {
    if (!mode) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    *mode = dev->current_mode;
    return GFX_OK;
}

static gfx_result_t vesa_map_fb(gfx_device_t* dev, gfx_framebuffer_t** fb) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;

    if (!fb || !priv) {
        return GFX_ERR_INVALID_PARAM;
    }

    /* Ensure virtual address is resolved.  Use vesa_resolve_virt() so we
     * never incorrectly cast a high physical address as a virtual pointer.
     * This function is idempotent: if already resolved it returns the
     * cached value immediately. */
    priv->fb_virt = vesa_resolve_virt(priv);
    if (!priv->fb_virt) {
        vesa_err("map_fb: failed to resolve virt addr (phys=0x%08x)\n",
                 (uint32_t)priv->fb_phys);
        return GFX_ERR_MAPPING_FAILED;
    }
    priv->framebuffer.virt_addr = priv->fb_virt;

    if (!vesa_mapping_matches(priv, priv->fb_virt) &&
        !vesa_repair_mapping(priv, priv->fb_virt)) {
        /* If the virtual address falls in MMIO range (>= 0xC0000000) the
         * hardware may still be directly accessible via identity/MMIO mapping
         * even though our page-table check failed.  Proceed with a warning
         * rather than aborting so that QEMU and similar environments keep
         * working; only hard-fail for low virtual addresses that are
         * definitely wrong. */
        if ((uint32_t)priv->fb_virt >= 0xC0000000U) {
            vesa_warn("map_fb: mapping check failed but virt=%p is in MMIO range, proceeding anyway\n",
                      priv->fb_virt);
        } else {
            vesa_err("map_fb: unresolved mapping virt=%p phys=0x%08x\n",
                     priv->fb_virt, (uint32_t)priv->fb_phys);
            return GFX_ERR_MAPPING_FAILED;
        }
    }

    *fb = &priv->framebuffer;
    return GFX_OK;
}

static gfx_result_t vesa_unmap_fb(gfx_device_t* dev, gfx_framebuffer_t* fb) {
    /* We don't actually unmap the framebuffer */
    return GFX_OK;
}

static gfx_result_t vesa_clear(gfx_device_t* dev, gfx_color_t color) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;
    
    if (!priv || !priv->fb_virt) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    uint32_t pixel = gfx_color_to_pixel(color, priv->framebuffer.format);
    uint8_t* fb = (uint8_t*)priv->fb_virt;
    uint32_t bytes_per_pixel = priv->bpp / 8;
    
    for (uint32_t y = 0; y < priv->height; y++) {
        uint8_t* row = fb + y * priv->pitch;
        
        switch (bytes_per_pixel) {
            case 1:
                memset(row, pixel & 0xFF, priv->width);
                break;
            case 2:
                for (uint32_t x = 0; x < priv->width; x++) {
                    *(uint16_t*)(row + x * 2) = pixel & 0xFFFF;
                }
                break;
            case 3:
                for (uint32_t x = 0; x < priv->width; x++) {
                    row[x * 3 + 0] = (pixel >> 0) & 0xFF;
                    row[x * 3 + 1] = (pixel >> 8) & 0xFF;
                    row[x * 3 + 2] = (pixel >> 16) & 0xFF;
                }
                break;
            case 4:
                for (uint32_t x = 0; x < priv->width; x++) {
                    *(uint32_t*)(row + x * 4) = pixel;
                }
                break;
        }
    }
    
    return GFX_OK;
}

static gfx_result_t vesa_draw_pixel(gfx_device_t* dev, int32_t x, int32_t y, gfx_color_t c) {
    vesa_private_t* priv = (vesa_private_t*)dev->driver_data;
    
    if (!priv || !priv->fb_virt) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    if (x < 0 || y < 0 || (uint32_t)x >= priv->width || (uint32_t)y >= priv->height) {
        return GFX_ERR_INVALID_PARAM;
    }
    
    uint32_t pixel = gfx_color_to_pixel(c, priv->framebuffer.format);
    uint8_t* fb = (uint8_t*)priv->fb_virt;
    uint32_t bytes_per_pixel = priv->bpp / 8;
    uint8_t* addr = fb + y * priv->pitch + x * bytes_per_pixel;
    
    switch (bytes_per_pixel) {
        case 1:
            *addr = pixel & 0xFF;
            break;
        case 2:
            *(uint16_t*)addr = pixel & 0xFFFF;
            break;
        case 3:
            addr[0] = (pixel >> 0) & 0xFF;
            addr[1] = (pixel >> 8) & 0xFF;
            addr[2] = (pixel >> 16) & 0xFF;
            break;
        case 4:
            *(uint32_t*)addr = pixel;
            break;
    }
    
    return GFX_OK;
}

static gfx_result_t vesa_wait_vsync(gfx_device_t* dev) {
    /* Use VGA port for vsync detection */
    while (gfx_inb(VGA_INPUT_STATUS1_COLOR) & 0x08);
    while (!(gfx_inb(VGA_INPUT_STATUS1_COLOR) & 0x08));
    return GFX_OK;
}

static gfx_result_t vesa_flush(gfx_device_t* dev) {
    (void)dev;
    __asm__ volatile("mfence" ::: "memory");
    return GFX_OK;
}

/* ============================================================================
 * Module Init/Exit
 * ============================================================================ */

gfx_result_t vesa_driver_init(void) {
    return gfx_register_driver(&vesa_gfx_driver);
}

void vesa_driver_exit(void) {
    gfx_unregister_driver(&vesa_gfx_driver);
}

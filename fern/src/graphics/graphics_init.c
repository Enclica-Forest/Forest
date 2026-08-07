/**
 * Fern - Graphics Subsystem Initialization (V2)
 * 
 * This file initializes the V2 graphics driver system and provides
 * the interface between the old API and the new driver architecture.
 */

#include "../include/graphics/graphics_manager.h"
#include "../include/graphics/graphics_driver_v2.h"
#include "../include/graphics/display_driver.h"
#include "../include/graphics/hardware_detect.h"
#include "../include/graphics/window_manager.h"
#include "../include/graphics/font_renderer.h"
#include "../include/graphics/app_graphics.h"
#include "../include/string.h"
#include "../include/libc/stdio.h"
#include "../include/debuglog.h"
#include "../include/memory.h"
#include "../include/task.h"
#include "../include/framebuffer.h"
#include "../include/render_layers.h"
#include "../include/gfx_config.h"

#if HAS_GRAPHICS

extern uintptr_t g_multiboot_fb_addr;
extern uint32_t g_multiboot_fb_width;
extern uint32_t g_multiboot_fb_height;
extern uint32_t g_multiboot_fb_pitch;
extern uint32_t g_multiboot_fb_bpp;

extern gfx_result_t bga_driver_init(void);
extern gfx_result_t svga_driver_init(void);
extern gfx_result_t vesa_driver_init(void);
extern gfx_result_t vga_text_driver_init(void);
extern gfx_result_t intel_driver_init(void);
extern gfx_result_t amd_driver_init(void);
extern gfx_result_t nv_driver_init(void);

extern gfx_result_t gfx_init(void);
extern gfx_result_t gfx_shutdown(void);
extern gfx_result_t gfx_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
extern gfx_result_t gfx_get_framebuffer(gfx_framebuffer_t** fb);
extern gfx_result_t gfx_clear_screen(gfx_color_t color);
extern bool gfx_is_initialized(void);
extern void* gfx_get_fb_addr(void);
extern uint32_t gfx_get_fb_width(void);
extern uint32_t gfx_get_fb_height(void);
extern uint32_t gfx_get_fb_pitch(void);
extern uint32_t gfx_get_fb_bpp(void);
extern void gfx_print_status(void);

typedef enum {
    GFX_STEP_UNINITIALIZED = 0,
    GFX_STEP_V2_DRIVER,
    GFX_STEP_COMPAT_LAYER,
    GFX_STEP_LEGACY_BRIDGE,
    GFX_STEP_FONT_RENDERER,
    GFX_STEP_WINDOW_MANAGER,
    GFX_STEP_APP_GRAPHICS,
    GFX_STEP_COMPLETE
} gfx_init_step_t;

static const char* gfx_step_name(gfx_init_step_t step) {
    switch (step) {
        case GFX_STEP_UNINITIALIZED: return "uninitialized";
        case GFX_STEP_V2_DRIVER: return "V2 driver";
        case GFX_STEP_COMPAT_LAYER: return "compat layer";
        case GFX_STEP_LEGACY_BRIDGE: return "legacy bridge";
        case GFX_STEP_FONT_RENDERER: return "font renderer";
        case GFX_STEP_WINDOW_MANAGER: return "window manager";
        case GFX_STEP_APP_GRAPHICS: return "app graphics";
        case GFX_STEP_COMPLETE: return "complete";
    }
    return "unknown";
}

static bool g_graphics_v2_initialized = false;
static bool g_display_ready = false;
static bool g_init_in_progress = false;
static framebuffer_t g_compat_framebuffer = {0};
static graphics_device_t g_compat_device = {0};

static struct {
    bool v2_driver;
    bool compat_layer;
    bool legacy_bridge;
    bool font_renderer;
    bool window_manager;
    bool app_graphics;
    gfx_init_step_t last_completed_step;
} g_gfx_state = {0};

static void wm_render_loop_entry(void) {
    debuglog(DEBUG_INFO, "[WM] Render loop task started (PID %u)\n",
             current_task ? current_task->id : 0);

    while (1) {
        if (window_manager_is_initialized()) {
            wm_render_loop_tick();
        }
        sleep_interruptible(16);
    }
}

static graphics_result_t convert_v2_result(gfx_result_t result) {
    switch (result) {
        case GFX_OK: return GRAPHICS_SUCCESS;
        case GFX_ERR_INVALID_PARAM: return GRAPHICS_ERROR_INVALID_PARAMETER;
        case GFX_ERR_NO_MEMORY: return GRAPHICS_ERROR_OUT_OF_MEMORY;
        case GFX_ERR_NOT_SUPPORTED: return GRAPHICS_ERROR_NOT_SUPPORTED;
        case GFX_ERR_HARDWARE: return GRAPHICS_ERROR_HARDWARE_FAULT;
        case GFX_ERR_MODE_NOT_FOUND: return GRAPHICS_ERROR_INVALID_MODE;
        case GFX_ERR_DEVICE_BUSY: return GRAPHICS_ERROR_DEVICE_BUSY;
        case GFX_ERR_NO_DRIVER: return GRAPHICS_ERROR_GENERIC;
        case GFX_ERR_INIT_FAILED: return GRAPHICS_ERROR_GENERIC;
        case GFX_ERR_MAPPING_FAILED: return GRAPHICS_ERROR_OUT_OF_MEMORY;
        default: return GRAPHICS_ERROR_GENERIC;
    }
}

static void update_compat_framebuffer(void) {
    gfx_framebuffer_t* v2_fb = NULL;

    if (gfx_get_framebuffer(&v2_fb) == GFX_OK && v2_fb) {
        uintptr_t prev_phys = g_compat_framebuffer.physical_addr;
        uintptr_t phys = v2_fb->phys_addr ? v2_fb->phys_addr : prev_phys;
        uintptr_t virt = (uintptr_t)v2_fb->virt_addr;

        /*
         * Identity-map fallback: QEMU/Bochs maps high MMIO (e.g. 0xF0000000)
         * at the same physical and virtual address.  If the V2 driver filled in
         * phys_addr but left virt_addr == NULL (mapping step failed or was
         * skipped), use phys_addr directly so downstream consumers get a usable
         * pointer rather than writing to address 0.
         */
        if (virt == 0 && phys != 0) {
            debuglog(DEBUG_WARN,
                     "[GFXINIT] virt_addr=0 but phys_addr=0x%08x — using identity-map fallback\n",
                     (uint32_t)phys);
            virt = phys;
        }

        g_compat_framebuffer.virtual_addr = virt;
        g_compat_framebuffer.physical_addr = phys;
        g_compat_framebuffer.width = v2_fb->width;
        g_compat_framebuffer.height = v2_fb->height;
        g_compat_framebuffer.pitch = v2_fb->pitch;
        g_compat_framebuffer.bpp = v2_fb->bpp;
        g_compat_framebuffer.size = v2_fb->size;
        g_compat_framebuffer.back_buffer = (uintptr_t)v2_fb->back_buffer;
        g_compat_framebuffer.double_buffered = v2_fb->double_buffered;
        
        switch (v2_fb->format) {
            case GFX_FORMAT_BGRX8888:
            case GFX_FORMAT_BGRA8888:
                g_compat_framebuffer.format = PIXEL_FORMAT_BGRA_8888;
                break;
            case GFX_FORMAT_RGBX8888:
            case GFX_FORMAT_RGBA8888:
                g_compat_framebuffer.format = PIXEL_FORMAT_RGBA_8888;
                break;
            case GFX_FORMAT_BGR888:
                g_compat_framebuffer.format = PIXEL_FORMAT_BGR_888;
                break;
            case GFX_FORMAT_RGB888:
                g_compat_framebuffer.format = PIXEL_FORMAT_RGB_888;
                break;
            case GFX_FORMAT_BGR565:
            case GFX_FORMAT_RGB565:
                g_compat_framebuffer.format = PIXEL_FORMAT_RGB_565;
                break;
            case GFX_FORMAT_BGR555:
            case GFX_FORMAT_RGB555:
                g_compat_framebuffer.format = PIXEL_FORMAT_RGB_555;
                break;
            default:
                if (v2_fb->bpp == 24) {
                    g_compat_framebuffer.format = PIXEL_FORMAT_BGR_888;
                } else {
                    g_compat_framebuffer.format = PIXEL_FORMAT_BGRA_8888;
                }
                break;
        }
        
        debuglog(DEBUG_INFO, "[GFXINIT] Framebuffer updated: %ux%ux%u @ 0x%08x\n",
                 g_compat_framebuffer.width, g_compat_framebuffer.height,
                 g_compat_framebuffer.bpp, (uint32_t)g_compat_framebuffer.virtual_addr);
    }
}

static void gfx_restore_kernel_fb_window_if_needed(void) {
    if (g_multiboot_fb_addr == 0 || g_multiboot_fb_pitch == 0 || g_multiboot_fb_height == 0) {
        return;
    }
    /* Suppress repeated WARN spam: only log at WARN on the first call; all
     * subsequent calls (e.g. from update_compat_framebuffer) use DEBUG. */
    static bool s_first_call = true;
    int log_level = s_first_call ? DEBUG_WARN : DEBUG_INFO;

    uint64_t fb_size64 = (uint64_t)g_multiboot_fb_pitch * (uint64_t)g_multiboot_fb_height;
    if (fb_size64 == 0 || fb_size64 > 0xFFFFFFFFULL) {
        return;
    }

    uint32_t fb_phys = (uint32_t)g_multiboot_fb_addr;
    uint32_t phys_page = fb_phys & ~MEMORY_PAGE_MASK;
    uint32_t phys_off = fb_phys & MEMORY_PAGE_MASK;
    uint32_t map_size = (uint32_t)fb_size64 + phys_off;
    uint32_t pages = (map_size + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;

    page_directory_t* pds[2];
    pds[0] = vmm_get_current_page_directory();
    pds[1] = vmm_get_kernel_page_directory();

    for (int pd_idx = 0; pd_idx < 2; pd_idx++) {
        page_directory_t* pd = pds[pd_idx];
        if (!pd) {
            continue;
        }

        uint32_t repaired = 0;
        bool all_ok = true;
        for (uint32_t i = 0; i < pages; i++) {
            uint32_t vaddr = 0xF0000000 + (i * MEMORY_PAGE_SIZE);
            uint32_t paddr = phys_page + (i * MEMORY_PAGE_SIZE);
            uint32_t mapped = vmm_get_physical_addr(pd, vaddr);
            if (mapped == paddr) {
                continue;
            }

            memory_result_t res = vmm_map_page(pd, vaddr, paddr,
                                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
            if (res == MEMORY_OK || res == MEMORY_ERROR_ALREADY_MAPPED) {
                repaired++;
                continue;
            }

            all_ok = false;
        }

        if (all_ok) {
            debuglog(log_level,
                     "[GFXINIT] FB window verified/repaired in pd=%p: phys=0x%08x pages=%u repaired=%u\n",
                     pd, phys_page, pages, repaired);
        } else {
            debuglog(log_level,
                     "[GFXINIT] FB window repair incomplete in pd=%p: phys=0x%08x pages=%u repaired=%u\n",
                     pd, phys_page, pages, repaired);
        }
    }
    s_first_call = false;
}

static bool init_v2_driver(void) {
    if (g_gfx_state.v2_driver) {
        return true;
    }

#define FB_PHYS_CHECK() debuglog(DEBUG_INFO, "[GFXDBG] fb 0xF0000000 phys=0x%08x\n", \
        vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000))

    FB_PHYS_CHECK();
    gfx_result_t v2_result = gfx_init();
    FB_PHYS_CHECK();
    if (v2_result != GFX_OK) {
        debuglog(DEBUG_ERROR, "[GFXINIT] V2 graphics init failed: %d\n", v2_result);

        /*
         * Degraded-mode fallback: if multiboot gave us a physical framebuffer
         * address, populate the compat framebuffer directly so the rest of the
         * init chain (legacy bridge, font renderer, WM) can still run.  The
         * identity-map fallback in update_compat_framebuffer() will supply a
         * usable virtual address when virt_addr is absent.
         */
        if (g_multiboot_fb_addr != 0 && g_multiboot_fb_width != 0 && g_multiboot_fb_height != 0) {
            debuglog(DEBUG_WARN,
                     "[GFXINIT] V2 driver failed — falling back to multiboot FB "
                     "phys=0x%08x %ux%u %ubpp\n",
                     (uint32_t)g_multiboot_fb_addr,
                     g_multiboot_fb_width, g_multiboot_fb_height, g_multiboot_fb_bpp);
            g_compat_framebuffer.physical_addr = (uintptr_t)g_multiboot_fb_addr;
            /* Identity-map: QEMU/Bochs MMIO virt == phys for high addresses */
            g_compat_framebuffer.virtual_addr  = (uintptr_t)g_multiboot_fb_addr;
            g_compat_framebuffer.width         = g_multiboot_fb_width;
            g_compat_framebuffer.height        = g_multiboot_fb_height;
            g_compat_framebuffer.pitch         = g_multiboot_fb_pitch;
            g_compat_framebuffer.bpp           = g_multiboot_fb_bpp;
            g_compat_framebuffer.size          = g_multiboot_fb_pitch * g_multiboot_fb_height;
            g_compat_framebuffer.format        = (g_multiboot_fb_bpp == 32)
                                                     ? PIXEL_FORMAT_BGRA_8888
                                                 : (g_multiboot_fb_bpp == 24)
                                                     ? PIXEL_FORMAT_BGR_888
                                                     : PIXEL_FORMAT_RGB_565;
            /* Mark as partially initialised so compat/legacy steps proceed */
            g_graphics_v2_initialized = false;   /* V2 driver itself is not up */
            g_gfx_state.v2_driver = false;
            /*
             * Return true so initialize_graphics_subsystem() continues with the
             * compat layer, legacy bridge, and font renderer — giving best-effort
             * display even without a working V2 driver.
             */
            return true;
        }

        return false;
    }

    g_graphics_v2_initialized = true;
    g_gfx_state.v2_driver = true;
    g_gfx_state.last_completed_step = GFX_STEP_V2_DRIVER;
    debuglog(DEBUG_INFO, "[GFXINIT] V2 driver initialized\n");
    return true;
}

static bool init_compat_layer(void) {
    if (g_gfx_state.compat_layer) {
        return true;
    }
    /*
     * Allow the compat layer to proceed when:
     *   a) the V2 driver initialised normally, OR
     *   b) init_v2_driver() already populated g_compat_framebuffer via the
     *      multiboot fallback (virtual_addr != 0 means we have something usable).
     */
    if (!g_gfx_state.v2_driver && g_compat_framebuffer.virtual_addr == 0) {
        return false;
    }

    update_compat_framebuffer();
    FB_PHYS_CHECK();
    
    debuglog(DEBUG_INFO, "[GFXINIT] Compat framebuffer after V2 init: %ux%u %ubpp virt=0x%08x\n",
            g_compat_framebuffer.width, g_compat_framebuffer.height,
            g_compat_framebuffer.bpp, (uint32_t)g_compat_framebuffer.virtual_addr);
    
    g_compat_device.type = GRAPHICS_DEVICE_VESA;
    strncpy(g_compat_device.name, "V2 Graphics Device", sizeof(g_compat_device.name) - 1);
    g_compat_device.is_active = true;
    
    gfx_print_status();

    g_gfx_state.compat_layer = true;
    g_gfx_state.last_completed_step = GFX_STEP_COMPAT_LAYER;
    debuglog(DEBUG_INFO, "[GFXINIT] Compat layer initialized\n");
    return true;
}

static bool init_legacy_bridge(void) {
    if (g_gfx_state.legacy_bridge) {
        return true;
    }

    debuglog(DEBUG_INFO, "[GFXINIT] Initializing legacy graphics manager bridge...\n");
    graphics_result_t legacy_result = graphics_init();
    FB_PHYS_CHECK();
    if (legacy_result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_WARN, "[GFXINIT] Legacy graphics manager init failed: %s\n",
                graphics_get_error_string(legacy_result));
        g_gfx_state.last_completed_step = GFX_STEP_COMPAT_LAYER;
        return false;
    }

    g_gfx_state.legacy_bridge = true;
    g_gfx_state.last_completed_step = GFX_STEP_LEGACY_BRIDGE;
    debuglog(DEBUG_INFO, "[GFXINIT] Legacy graphics manager initialized successfully\n");
    return true;
}

static bool init_font_renderer(void) {
    if (g_gfx_state.font_renderer) {
        return true;
    }

    debuglog(DEBUG_INFO, "[GFXINIT] Initializing font renderer...\n");
    graphics_result_t result = font_renderer_init();
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_WARN, "[GFXINIT] Font renderer initialization failed: %s\n",
                graphics_get_error_string(result));
        g_gfx_state.last_completed_step = GFX_STEP_LEGACY_BRIDGE;
        return false;
    }

    g_gfx_state.font_renderer = true;
    g_gfx_state.last_completed_step = GFX_STEP_FONT_RENDERER;
    debuglog(DEBUG_INFO, "[GFXINIT] Font renderer initialized\n");
    return true;
}

static bool init_window_manager(void) {
    if (g_gfx_state.window_manager) {
        return true;
    }

    debuglog(DEBUG_INFO, "[GFXINIT] Initializing window manager...\n");
    FB_PHYS_CHECK();
    graphics_result_t result = window_manager_init();
    gfx_restore_kernel_fb_window_if_needed();
    FB_PHYS_CHECK();
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_WARN, "[GFXINIT] Window manager initialization failed: %s — continuing\n",
                graphics_get_error_string(result));
        g_gfx_state.last_completed_step = GFX_STEP_FONT_RENDERER;
        return false;
    }

    g_gfx_state.window_manager = true;
    g_gfx_state.last_completed_step = GFX_STEP_WINDOW_MANAGER;
    debuglog(DEBUG_INFO, "[GFXINIT] Window manager initialized successfully\n");
    return true;
}

static bool init_app_graphics(void) {
    if (g_gfx_state.app_graphics) {
        return true;
    }

    debuglog(DEBUG_INFO, "[GFXINIT] Initializing application graphics API...\n");
    graphics_result_t result = app_graphics_init();
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_WARN, "[GFXINIT] Application graphics API initialization failed: %s\n",
                graphics_get_error_string(result));
        g_gfx_state.last_completed_step = GFX_STEP_WINDOW_MANAGER;
        return false;
    }

    g_gfx_state.app_graphics = true;
    g_gfx_state.last_completed_step = GFX_STEP_APP_GRAPHICS;
    debuglog(DEBUG_INFO, "[GFXINIT] Application graphics API initialized\n");
    return true;
}

graphics_result_t initialize_graphics_subsystem(void) {
    if (g_init_in_progress) {
        debuglog(DEBUG_WARN, "[GFXINIT] Init already in progress, skipping\n");
        return GRAPHICS_SUCCESS;
    }
    if (g_display_ready) {
        debuglog(DEBUG_INFO, "[GFXINIT] Graphics already initialized and display ready\n");
        return GRAPHICS_SUCCESS;
    }

    g_init_in_progress = true;
    debuglog(DEBUG_INFO, "[GFXINIT] Initializing Forest-OS graphics subsystem (V2)...\n");

    if (!init_v2_driver()) {
        debuglog(DEBUG_ERROR, "[GFXINIT] V2 driver init failed — cannot continue\n");
        g_init_in_progress = false;
        return GRAPHICS_ERROR_GENERIC;
    }

    if (!init_compat_layer()) {
        debuglog(DEBUG_ERROR, "[GFXINIT] Compat layer init failed\n");
    }

    /* Initialize the render layer compositor now that we know the framebuffer dimensions */
    if (g_compat_framebuffer.width > 0 && g_compat_framebuffer.height > 0) {
        if (rl_init(g_compat_framebuffer.width, g_compat_framebuffer.height,
                    g_compat_framebuffer.pitch, g_compat_framebuffer.bpp,
                    g_compat_framebuffer.format)) {
            debuglog(DEBUG_INFO, "[GFXINIT] Render layer compositor initialized\n");
            /* Migrate the splash screen from its early buffer to the layer system */
            extern void splash_migrate_to_layer(void);
            splash_migrate_to_layer();
        } else {
            debuglog(DEBUG_WARN, "[GFXINIT] Render layer init failed — splash uses direct fb\n");
        }
    }

    /* Re-verify framebuffer mapping after rl_init allocations (kmalloc may
     * have triggered heap expansion which rebuilds page tables) */
    gfx_restore_kernel_fb_window_if_needed();

    if (!init_legacy_bridge()) {
        debuglog(DEBUG_WARN, "[GFXINIT] Legacy bridge failed — degraded mode (no graphics_get_current_mode)\n");
    }

    if (!init_font_renderer()) {
        debuglog(DEBUG_WARN, "[GFXINIT] Font renderer failed — WM will have no text rendering\n");
    }

    if (!init_window_manager()) {
        debuglog(DEBUG_WARN, "[GFXINIT] WM init failed — running without compositor\n");
    } else {
        if (current_task != NULL) {
            debuglog(DEBUG_INFO, "[GFXINIT] Task system is up — WM render loop disabled (userspace handles rendering)\n");
            wm_enable_cursor(true);
        } else {
            debuglog(DEBUG_WARN, "[GFXINIT] Task system not yet initialised — "
                     "WM render loop task deferred; call wm_start_render_loop_task() "
                     "after tasks_init()\n");
        }
    }

    if (!init_app_graphics()) {
        debuglog(DEBUG_WARN, "[GFXINIT] App graphics API failed — apps cannot use drawing API\n");
    }

    gfx_restore_kernel_fb_window_if_needed();
    
    debuglog(DEBUG_INFO, "[GFXINIT] Framebuffer: %ux%u %ubpp, pitch=%u\n",
            g_compat_framebuffer.width, g_compat_framebuffer.height,
            g_compat_framebuffer.bpp, g_compat_framebuffer.pitch);

    debuglog(DEBUG_INFO, "[GFXINIT] Graphics subsystem init complete (step: %s)\n",
            gfx_step_name(g_gfx_state.last_completed_step));

    g_display_ready = true;
    g_init_in_progress = false;

    debuglog(DEBUG_INFO, "[GFXINIT] Display ready notification: all init steps completed\n");

    return GRAPHICS_SUCCESS;
}

bool graphics_health_check(void) {
    bool healthy = true;

    if (g_gfx_state.v2_driver && !gfx_is_initialized()) {
        debuglog(DEBUG_WARN, "[GFXINIT] Health check: V2 driver lost\n");
        healthy = false;
    }

    if (g_gfx_state.window_manager && !window_manager_is_initialized()) {
        debuglog(DEBUG_WARN, "[GFXINIT] Health check: WM lost\n");
        healthy = false;
    }

    if (g_gfx_state.font_renderer && !font_renderer_is_initialized()) {
        debuglog(DEBUG_WARN, "[GFXINIT] Health check: font renderer lost\n");
        healthy = false;
    }

    if (g_gfx_state.app_graphics && !app_graphics_is_initialized()) {
        debuglog(DEBUG_WARN, "[GFXINIT] Health check: app graphics lost\n");
        healthy = false;
    }

    if (g_gfx_state.v2_driver) {
        gfx_framebuffer_t* fb = NULL;
        gfx_result_t fb_res = gfx_get_framebuffer(&fb);
        if (fb_res != GFX_OK || !fb) {
            debuglog(DEBUG_WARN, "[GFXINIT] Health check: framebuffer invalid\n");
            healthy = false;
        } else if (!fb->virt_addr && !fb->phys_addr) {
            /* virt_addr may be NULL when using the identity-map fallback;
             * only flag as unhealthy if both virt and phys are absent. */
            debuglog(DEBUG_WARN, "[GFXINIT] Health check: framebuffer has no address\n");
            healthy = false;
        }
    }

    return healthy;
}

graphics_result_t graphics_recover_subsystem(void) {
    debuglog(DEBUG_INFO, "[GFXINIT] Attempting graphics subsystem recovery...\n");
    bool recovered_any = false;

    if (g_gfx_state.v2_driver && !gfx_is_initialized()) {
        debuglog(DEBUG_INFO, "[GFXINIT] Recovery: reinitializing V2 driver\n");
        if (init_v2_driver()) {
            recovered_any = true;
        }
    }

    if (g_gfx_state.v2_driver && !g_gfx_state.compat_layer) {
        if (init_compat_layer()) {
            recovered_any = true;
        }
    }

    if (!g_gfx_state.legacy_bridge) {
        if (init_legacy_bridge()) {
            recovered_any = true;
        }
    }

    if (!g_gfx_state.font_renderer) {
        if (init_font_renderer()) {
            recovered_any = true;
        }
    }

    if (g_gfx_state.window_manager && !window_manager_is_initialized()) {
        debuglog(DEBUG_INFO, "[GFXINIT] Recovery: reinitializing window manager\n");
        if (init_window_manager()) {
            recovered_any = true;
        }
    } else if (!g_gfx_state.window_manager && g_gfx_state.font_renderer) {
        if (init_window_manager()) {
            recovered_any = true;
        }
    }

    if (!g_gfx_state.app_graphics && g_gfx_state.window_manager) {
        if (init_app_graphics()) {
            recovered_any = true;
        }
    }

    if (recovered_any) {
        debuglog(DEBUG_INFO, "[GFXINIT] Recovery completed — some subsystems restored\n");
    } else {
        debuglog(DEBUG_WARN, "[GFXINIT] Recovery failed — no subsystems could be restored\n");
    }

    return recovered_any ? GRAPHICS_SUCCESS : GRAPHICS_ERROR_GENERIC;
}

void wm_start_render_loop_task(void) {
    if (!window_manager_is_initialized()) {
        debuglog(DEBUG_WARN, "[GFXINIT] wm_start_render_loop_task: WM not init, skipping\n");
        return;
    }

    if (ready_queue_head) {
        task_t* t = ready_queue_head;
        do {
            if (t->name[0] == 'w' && t->name[1] == 'm' && t->name[2] == '-') {
                debuglog(DEBUG_INFO, "[GFXINIT] wm-render task already exists (ID %u), skipping\n",
                         t->id);
                return;
            }
            t = t->next;
        } while (t && t != ready_queue_head);
    }

    if (current_task == NULL) {
        debuglog(DEBUG_WARN, "[GFXINIT] Task system not up, deferring render loop creation\n");
        return;
    }

    task_t* render_task = task_create_kernel(wm_render_loop_entry, "wm-render", 0);
    if (!render_task) {
        debuglog(DEBUG_ERROR, "[GFXINIT] Failed to create WM render loop task\n");
        return;
    }

    task_set_priority(render_task->id, TASK_PRIORITY_MAX);
    task_set_protected(render_task->id, true);

    debuglog(DEBUG_INFO, "[GFXINIT] WM render loop task created (PID %u, priority %u)\n",
             render_task->id, TASK_PRIORITY_MAX);

    wm_enable_cursor(true);

    {
        uint32_t w = 0, h = 0;
        desktop_get_size(&w, &h);
        if (w > 0 && h > 0) {
            extern void ps2_mouse_set_bounds(int32_t width, int32_t height);
            ps2_mouse_set_bounds((int32_t)w, (int32_t)h);
            wm_update_cursor((int32_t)(w / 2), (int32_t)(h / 2));
            debuglog(DEBUG_INFO,
                     "[GFXINIT] Mouse cursor seeded to centre (%u,%u) of %ux%u desktop\n",
                     w / 2, h / 2, w, h);
        }
    }

    wm_enable_cursor(true);
}

graphics_result_t shutdown_graphics_subsystem(void) {
    debuglog(DEBUG_INFO, "[GFXINIT] Shutting down graphics subsystem...\n");
    
    if (g_gfx_state.app_graphics) {
        graphics_result_t app_result = app_graphics_shutdown();
        if (app_result != GRAPHICS_SUCCESS) {
            debuglog(DEBUG_ERROR, "[GFXINIT] Application graphics API shutdown failed: %s\n",
                    graphics_get_error_string(app_result));
        }
        g_gfx_state.app_graphics = false;
    }
    
    if (g_gfx_state.font_renderer) {
        graphics_result_t font_result = font_renderer_shutdown();
        if (font_result != GRAPHICS_SUCCESS) {
            debuglog(DEBUG_ERROR, "[GFXINIT] Font renderer shutdown failed: %s\n",
                    graphics_get_error_string(font_result));
        }
        g_gfx_state.font_renderer = false;
    }
    
    if (g_gfx_state.window_manager) {
        graphics_result_t wm_result = window_manager_shutdown();
        if (wm_result != GRAPHICS_SUCCESS) {
            debuglog(DEBUG_ERROR, "[GFXINIT] Window manager shutdown failed: %s\n",
                    graphics_get_error_string(wm_result));
        }
        g_gfx_state.window_manager = false;
    }
    
    if (g_graphics_v2_initialized) {
        gfx_shutdown();
        g_graphics_v2_initialized = false;
    }
    g_gfx_state.v2_driver = false;
    g_gfx_state.compat_layer = false;
    g_gfx_state.legacy_bridge = false;
    g_gfx_state.last_completed_step = GFX_STEP_UNINITIALIZED;
    
    g_display_ready = false;

    debuglog(DEBUG_INFO, "[GFXINIT] Graphics subsystem shutdown complete\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t test_graphics_functionality(void) {
    debuglog(DEBUG_INFO, "[GFXINIT] Running graphics functionality test...\n");
    
    if (!g_graphics_v2_initialized) {
        debuglog(DEBUG_ERROR, "[GFXINIT] Graphics not initialized for test\n");
        return GRAPHICS_ERROR_GENERIC;
    }
    
    gfx_color_t colors[] = {
        {255, 0, 0, 255},
        {0, 255, 0, 255},
        {0, 0, 255, 255},
        {0, 0, 0, 255},
    };
    
    for (int i = 0; i < 4; i++) {
        gfx_result_t result = gfx_clear_screen(colors[i]);
        if (result != GFX_OK) {
            debuglog(DEBUG_ERROR, "[GFXINIT] Clear screen test %d failed\n", i);
            return GRAPHICS_ERROR_GENERIC;
        }
        for (volatile int j = 0; j < 1000000; j++);
    }
    
    debuglog(DEBUG_INFO, "[GFXINIT] Graphics functionality test completed\n");
    return GRAPHICS_SUCCESS;
}

graphics_result_t test_window_manager(void) {
    debuglog(DEBUG_INFO, "[GFXINIT] Running window manager test...\n");
    
    if (!window_manager_is_initialized()) {
        debuglog(DEBUG_ERROR, "[GFXINIT] Window manager not initialized for test\n");
        return GRAPHICS_ERROR_GENERIC;
    }
    
    window_handle_t test_window = window_create(100, 100, 400, 300, "Test Window", WINDOW_FLAGS_DEFAULT);
    if (test_window == INVALID_WINDOW_HANDLE) {
        debuglog(DEBUG_ERROR, "[GFXINIT] Failed to create test window\n");
        return GRAPHICS_ERROR_GENERIC;
    }
    
    window_set_title(test_window, "Updated Test Window");
    window_focus(test_window);
    compositor_update();
    
    window_destroy(test_window);
    
    debuglog(DEBUG_INFO, "[GFXINIT] Window manager test completed\n");
    return GRAPHICS_SUCCESS;
}

bool graphics_is_initialized_v2_compat(void) {
    return g_graphics_v2_initialized;
}

framebuffer_t* graphics_get_framebuffer_v2_compat(void) {
    if (!g_graphics_v2_initialized) {
        return NULL;
    }
    
    update_compat_framebuffer();
    return &g_compat_framebuffer;
}

graphics_device_t* graphics_get_primary_device_v2_compat(void) {
    if (!g_graphics_v2_initialized) {
        return NULL;
    }
    return &g_compat_device;
}

graphics_result_t graphics_set_mode_v2_compat(uint32_t width, uint32_t height,
                                             uint32_t bpp, uint32_t refresh_rate) {
    (void)refresh_rate;
    
    gfx_result_t result = gfx_set_mode(width, height, bpp);
    if (result == GFX_OK) {
        update_compat_framebuffer();
    }
    return convert_v2_result(result);
}

graphics_result_t graphics_clear_screen_v2_compat(graphics_color_t color) {
    gfx_color_t v2_color = {color.r, color.g, color.b, color.a};
    return convert_v2_result(gfx_clear_screen(v2_color));
}

bool graphics_is_display_ready(void) {
    return g_display_ready;
}

#else /* !HAS_GRAPHICS */

/* No-framebuffer graphics init stubs. The graphics subsystem is never
 * initialised; kernel_framebuffer_disabled() (defined in kernel.c) is the
 * canonical runtime check used by callers. */

graphics_result_t initialize_graphics_subsystem(void)            { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t shutdown_graphics_subsystem(void)              { return GRAPHICS_SUCCESS; }
void wm_start_render_loop_task(void)                              { }
graphics_result_t test_graphics_functionality(void)              { return GRAPHICS_ERROR_NOT_SUPPORTED; }
graphics_result_t test_window_manager(void)                      { return GRAPHICS_ERROR_NOT_SUPPORTED; }
bool graphics_is_initialized_v2_compat(void)                     { return false; }
framebuffer_t* graphics_get_framebuffer_v2_compat(void)          { return NULL; }
graphics_device_t* graphics_get_primary_device_v2_compat(void)   { return NULL; }
graphics_result_t graphics_set_mode_v2_compat(uint32_t w, uint32_t h, uint32_t b, uint32_t r) {
    (void)w; (void)h; (void)b; (void)r; return GRAPHICS_ERROR_NOT_SUPPORTED;
}
graphics_result_t graphics_clear_screen_v2_compat(graphics_color_t c) { (void)c; return GRAPHICS_ERROR_NOT_SUPPORTED; }
bool graphics_is_display_ready(void)                              { return false; }
bool graphics_health_check(void)                                  { return false; }
graphics_result_t graphics_recover_subsystem(void)                { return GRAPHICS_ERROR_NOT_SUPPORTED; }

#endif /* HAS_GRAPHICS */

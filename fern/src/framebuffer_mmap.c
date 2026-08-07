#include "include/framebuffer.h"
#include "include/graphics/graphics_driver_v2.h"
#include "include/tlb_manager.h"
#include "include/graphics/graphics_manager.h"
#include "include/graphics/graphics_types.h"
#include "include/mm.h"
#include "include/memory.h"
#include "include/memory_safe.h"
#include "include/smep_smap.h"
#include "include/syscall.h"
#include "include/task.h"
#include "include/screen.h"
#include "include/string.h"
#include "include/util.h"
#include "include/debuglog.h"
#include "include/timer.h"
#include "include/pci.h"

#if HAS_FRAMEBUFFER

#define FB_PAGE_FLAGS           (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE | PAGE_CACHE_DISABLE)
#define FB_PAGE_FLAGS_CACHED   (PAGE_PRESENT | PAGE_USER | PAGE_WRITABLE)

#define FB_MAX_REGIONS         8
#define FB_DIRTY_MAX_RECTS     64
#define FB_FLOCK_UNOWNED       0

typedef struct fb_dirty_rect {
    uint32_t x, y, w, h;
} fb_dirty_rect_t;

typedef struct fb_dirty_tracker {
    uint32_t pid;
    fb_dirty_rect_t rects[FB_DIRTY_MAX_RECTS];
    uint32_t count;
    bool full_screen;
    bool in_use;
} fb_dirty_tracker_t;

typedef struct fb_region {
    uintptr_t phys_addr;
    void* virt_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t size;
    uint32_t format;
    bool active;
    uint32_t owner_pid;
} fb_region_t;

typedef struct fb_flock {
    uint32_t owner_pid;
    bool held;
} fb_flock_t;

typedef struct fb_mapping {
    void* virtual_addr;
    uintptr_t physical_addr;
    size_t size;
    uint32_t flags;
    task_t* task;
    uint32_t task_pid;
    uint32_t region_id;
    struct fb_mapping* next;
} fb_mapping_t;

static struct {
    bool initialized;
    fb_mapping_t* mappings;
    spinlock_t lock;

    bool auto_refresh_enabled;
    uint32_t last_flush_time;
    uint32_t flush_interval_ms;

    gfx_framebuffer_t* current_fb;
    uintptr_t cached_phys_addr;
    uint32_t cached_width;
    uint32_t cached_height;
    uint32_t cached_pitch;
    uint32_t cached_bpp;
    uint32_t cached_size;
    uint32_t cached_format;
    bool cached_double_buffered;
    bool userspace_forced_single_buffer;
    bool restore_double_buffer_on_last_unmap;
    uint32_t double_buffer_owner_pid;

    void* last_page_directory;
    uint32_t validation_failures;
    bool recovery_in_progress;

    fb_region_t regions[FB_MAX_REGIONS];
    uint32_t region_count;

    fb_dirty_tracker_t dirty_trackers[16];
    uint32_t dirty_tracker_count;

    fb_flock_t flock;

    volatile int region_table_version;

    bool preserve_last_frame;

    volatile uint32_t mode_generation;
} fb_state;

static void flush_framebuffer_update(void);
static void* map_framebuffer_to_user(uintptr_t phys_addr, void* virt_start, size_t size, uint32_t flags);
static fb_mapping_t* find_mapping(void* virt_addr);
static fb_mapping_t* add_mapping(void* virt_addr, uintptr_t phys_addr, size_t size, uint32_t flags);
static int remove_mapping(void* virt_addr);
static void cleanup_task_mappings(task_t* task);
static int get_framebuffer_info(fb_info_t* info);
static int unmap_framebuffer_from_user(void* virt_addr, size_t size);
static int unmap_framebuffer_from_task(task_t* task, void* virt_addr, size_t size);
static int refresh_cached_framebuffer_info(void);
static void prune_stale_mappings_locked(void);

static bool validate_framebuffer_mapping(fb_mapping_t* mapping, task_t* owner_task);
static bool recover_framebuffer_mapping(fb_mapping_t* mapping, task_t* owner_task);
static bool check_page_directory_changed(void);
static void handle_mapping_corruption(fb_mapping_t* mapping);
static bool framebuffer_mmap_ensure_initialized(void);

static fb_dirty_tracker_t* get_dirty_tracker(uint32_t pid);
static void dirty_track_rect(uint32_t pid, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static void dirty_track_full(uint32_t pid);
static void dirty_clear(uint32_t pid);
static bool validate_fb_page(uintptr_t fault_addr);
static bool find_fb_region_for_addr(uintptr_t fault_addr, fb_region_t** out_region);
static bool fb_acquire_flock(uint32_t pid, bool blocking);
static bool fb_release_flock(uint32_t pid);
static uint32_t fb_get_region_count(void);
static int fb_get_region_info(uint32_t region_id, fb_region_t* info);
static bool validate_all_fb_pages(void);

extern bool kernel_get_multiboot_framebuffer(uintptr_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);

static uint32_t v2_format_to_fb_format(uint32_t v2_format, uint32_t bpp) {
    switch (v2_format) {
        case GFX_FORMAT_TEXT:
            return FB_FORMAT_TEXT_MODE;
        case GFX_FORMAT_INDEXED_8:
            return FB_FORMAT_INDEXED_8;
        case GFX_FORMAT_RGB555:
        case GFX_FORMAT_BGR555:
            return FB_FORMAT_RGB_555;
        case GFX_FORMAT_RGB565:
        case GFX_FORMAT_BGR565:
            return FB_FORMAT_RGB_565;
        case GFX_FORMAT_RGB888:
            return FB_FORMAT_BGR_888;
        case GFX_FORMAT_BGR888:
            return FB_FORMAT_BGR_888;
        case GFX_FORMAT_RGBX8888:
        case GFX_FORMAT_RGBA8888:
            return FB_FORMAT_RGBA_8888;
        case GFX_FORMAT_BGRX8888:
        case GFX_FORMAT_BGRA8888:
            return FB_FORMAT_BGRA_8888;
        default:
            break;
    }

    switch ((bpp + 7) / 8) {
        case 1:  return FB_FORMAT_INDEXED_8;
        case 2:  return FB_FORMAT_RGB_565;
        case 3:  return FB_FORMAT_BGR_888;
        default: return FB_FORMAT_BGRA_8888;
    }
}

static bool framebuffer_mmap_ensure_initialized(void) {
    if (fb_state.initialized) {
        return true;
    }
    return framebuffer_mmap_init() == 0;
}

static fb_dirty_tracker_t* get_dirty_tracker(uint32_t pid) {
    for (uint32_t i = 0; i < fb_state.dirty_tracker_count; i++) {
        if (fb_state.dirty_trackers[i].in_use && fb_state.dirty_trackers[i].pid == pid) {
            return &fb_state.dirty_trackers[i];
        }
    }
    for (uint32_t i = 0; i < fb_state.dirty_tracker_count; i++) {
        if (!fb_state.dirty_trackers[i].in_use) {
            fb_state.dirty_trackers[i].pid = pid;
            fb_state.dirty_trackers[i].count = 0;
            fb_state.dirty_trackers[i].full_screen = false;
            fb_state.dirty_trackers[i].in_use = true;
            return &fb_state.dirty_trackers[i];
        }
    }
    if (fb_state.dirty_tracker_count < 16) {
        fb_dirty_tracker_t* tracker = &fb_state.dirty_trackers[fb_state.dirty_tracker_count];
        tracker->pid = pid;
        tracker->count = 0;
        tracker->full_screen = false;
        tracker->in_use = true;
        fb_state.dirty_tracker_count++;
        return tracker;
    }
    return NULL;
}

static void dirty_track_rect(uint32_t pid, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    fb_dirty_tracker_t* tracker = get_dirty_tracker(pid);
    if (!tracker) return;

    if (tracker->full_screen) return;

    for (uint32_t i = 0; i < tracker->count; i++) {
        fb_dirty_rect_t* existing = &tracker->rects[i];
        if (x >= existing->x && y >= existing->y &&
            x + w <= existing->x + existing->w &&
            y + h <= existing->y + existing->h) {
            return;
        }
    }

    if (tracker->count < FB_DIRTY_MAX_RECTS) {
        tracker->rects[tracker->count].x = x;
        tracker->rects[tracker->count].y = y;
        tracker->rects[tracker->count].w = w;
        tracker->rects[tracker->count].h = h;
        tracker->count++;
    } else {
        tracker->full_screen = true;
        tracker->count = 0;
    }
}

static void dirty_track_full(uint32_t pid) {
    fb_dirty_tracker_t* tracker = get_dirty_tracker(pid);
    if (!tracker) return;
    tracker->full_screen = true;
    tracker->count = 0;
}

static void dirty_clear(uint32_t pid) {
    fb_dirty_tracker_t* tracker = get_dirty_tracker(pid);
    if (!tracker) return;
    tracker->count = 0;
    tracker->full_screen = false;
}

static bool find_fb_region_for_addr(uintptr_t fault_addr, fb_region_t** out_region) {
    int version = fb_state.region_table_version;
    __asm__ volatile("" ::: "memory");

    for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
        fb_region_t* r = &fb_state.regions[i];
        if (!r->active) continue;
        uintptr_t start = (uintptr_t)r->virt_addr;
        if (start != 0 && fault_addr >= start && fault_addr < start + r->size) {
            if (out_region) *out_region = r;
            return true;
        }
    }

    __asm__ volatile("" ::: "memory");
    if (fb_state.region_table_version != version) {
        for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
            fb_region_t* r = &fb_state.regions[i];
            if (!r->active) continue;
            uintptr_t start = (uintptr_t)r->virt_addr;
            if (start != 0 && fault_addr >= start && fault_addr < start + r->size) {
                if (out_region) *out_region = r;
                return true;
            }
        }
    }

    return false;
}

static bool validate_fb_page(uintptr_t fault_addr) {
    fb_region_t* region = NULL;
    if (!find_fb_region_for_addr(fault_addr, &region)) {
        return false;
    }

    uintptr_t page_addr = fault_addr & ~(PAGE_SIZE - 1);
    uintptr_t offset = page_addr - (uintptr_t)region->virt_addr;
    uintptr_t phys = region->phys_addr + offset;

    task_t* task = current_task;
    if (!task || !task->page_directory) return false;

    vmm_unmap_page(task->page_directory, page_addr);

    memory_result_t result = vmm_map_page(task->page_directory, page_addr, phys, FB_PAGE_FLAGS);
    if (result != MEMORY_OK) return false;

    __asm__ volatile("invlpg (%0)" : : "r"(page_addr) : "memory");
    return true;
}

static bool fb_acquire_flock(uint32_t pid, bool blocking) {
    if (pid == FB_FLOCK_UNOWNED) return false;

    if (fb_state.flock.held && fb_state.flock.owner_pid == pid) {
        return true;
    }

    if (fb_state.flock.held) {
        if (!blocking) return false;
        while (fb_state.flock.held) {
            if (fb_state.flock.owner_pid == pid) return true;
        }
    }

    fb_state.flock.owner_pid = pid;
    fb_state.flock.held = true;
    return true;
}

static bool fb_release_flock(uint32_t pid) {
    if (!fb_state.flock.held || fb_state.flock.owner_pid != pid) {
        return false;
    }
    fb_state.flock.owner_pid = FB_FLOCK_UNOWNED;
    fb_state.flock.held = false;
    return true;
}

static uint32_t fb_get_region_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
        if (fb_state.regions[i].active) count++;
    }
    return count;
}

static int fb_get_region_info(uint32_t region_id, fb_region_t* info) {
    if (!info || region_id >= FB_MAX_REGIONS) return FB_ERROR_INVALID_PARAM;
    fb_region_t* r = &fb_state.regions[region_id];
    if (!r->active) return FB_ERROR_NOT_FOUND;
    *info = *r;
    return FB_SUCCESS;
}

static bool validate_all_fb_pages(void) {
    task_t* task = current_task;
    if (!task || !task->page_directory) return false;

    bool all_valid = true;

    spinlock_acquire(&fb_state.lock);
    fb_mapping_t* m = fb_state.mappings;
    while (m) {
        if (m->task_pid == task->id) {
            uintptr_t aligned_virt = ((uintptr_t)m->virtual_addr) & ~(PAGE_SIZE - 1);
            uintptr_t aligned_phys = m->physical_addr & ~(PAGE_SIZE - 1);
            size_t aligned_size = (m->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

            for (size_t i = 0; i < aligned_size; i += PAGE_SIZE) {
                uintptr_t current_phys = vmm_get_physical_addr(task->page_directory, aligned_virt + i);
                if (current_phys != (aligned_phys + i)) {
                    all_valid = false;
                }
            }
        }
        m = m->next;
    }
    spinlock_release(&fb_state.lock);

    return all_valid;
}

static int refresh_cached_framebuffer_info(void) {
    gfx_framebuffer_t* v2_fb = NULL;
    static bool s_logged_cache_valid = false;
    static uintptr_t s_logged_phys = 0;
    static uint32_t s_logged_w = 0;
    static uint32_t s_logged_h = 0;
    static uint32_t s_logged_pitch = 0;
    static uint32_t s_logged_bpp = 0;
    static uint32_t s_logged_fmt = 0;

    if (gfx_get_framebuffer(&v2_fb) == GFX_OK && v2_fb && v2_fb->virt_addr) {
        fb_state.current_fb = v2_fb;
        fb_state.cached_phys_addr = v2_fb->phys_addr;
        fb_state.cached_width = v2_fb->width;
        fb_state.cached_height = v2_fb->height;
        fb_state.cached_pitch = v2_fb->pitch;
        fb_state.cached_bpp = v2_fb->bpp;
        fb_state.cached_size = v2_fb->size;
        fb_state.cached_format = v2_format_to_fb_format(v2_fb->format, v2_fb->bpp);
        fb_state.cached_double_buffered = v2_fb->double_buffered;

        if (fb_state.cached_width != 0 &&
            fb_state.cached_pitch >= fb_state.cached_width &&
            (fb_state.cached_pitch % fb_state.cached_width) == 0) {
            uint32_t stride_bpp = fb_state.cached_pitch / fb_state.cached_width;
            if (stride_bpp >= 1 && stride_bpp <= 4) {
                uint32_t declared_bpp = (fb_state.cached_bpp + 7) / 8;
                if (declared_bpp != stride_bpp) {
                    debuglog(DEBUG_WARN,
                             "[FB_CACHE] bpp mismatch: reported=%u (%u Bpp), pitch/width=%u Bpp; normalizing\n",
                             fb_state.cached_bpp, declared_bpp, stride_bpp);
                    fb_state.cached_bpp = stride_bpp * 8;
                    fb_state.cached_format = v2_format_to_fb_format(v2_fb->format, fb_state.cached_bpp);
                }
            }
        }

        uint32_t min_pitch = fb_state.cached_width * ((fb_state.cached_bpp + 7) / 8);
        if (fb_state.cached_pitch < min_pitch) {
            debuglog(DEBUG_ERROR, "[FB_CACHE] WARNING: pitch %u < minimum %u, fixing\n",
                     fb_state.cached_pitch, min_pitch);
            fb_state.cached_pitch = (min_pitch + 3) & ~3;
            fb_state.cached_size = fb_state.cached_pitch * fb_state.cached_height;
        }

        uint32_t visible_size = fb_state.cached_pitch * fb_state.cached_height;
        if (fb_state.cached_size < visible_size) {
            debuglog(DEBUG_WARN, "[FB_CACHE] size %u < visible %u, expanding\n",
                     fb_state.cached_size, visible_size);
            fb_state.cached_size = visible_size;
        }

        bool changed = !s_logged_cache_valid ||
                       s_logged_phys != fb_state.cached_phys_addr ||
                       s_logged_w != fb_state.cached_width ||
                       s_logged_h != fb_state.cached_height ||
                       s_logged_pitch != fb_state.cached_pitch ||
                       s_logged_bpp != fb_state.cached_bpp ||
                       s_logged_fmt != fb_state.cached_format;
        if (changed) {
            debuglog(DEBUG_INFO,
                     "[FB_CACHE] V2: %ux%u pitch=%u bpp=%u fmt=%u phys=0x%x\n",
                     fb_state.cached_width, fb_state.cached_height,
                     fb_state.cached_pitch, fb_state.cached_bpp, fb_state.cached_format,
                     (uint32_t)fb_state.cached_phys_addr);
            s_logged_cache_valid = true;
            s_logged_phys = fb_state.cached_phys_addr;
            s_logged_w = fb_state.cached_width;
            s_logged_h = fb_state.cached_height;
            s_logged_pitch = fb_state.cached_pitch;
            s_logged_bpp = fb_state.cached_bpp;
            s_logged_fmt = fb_state.cached_format;
        }
        return 0;
    }

    framebuffer_t* legacy_fb = graphics_get_framebuffer();
    if (legacy_fb && legacy_fb->virtual_addr) {
        fb_state.current_fb = NULL;
        fb_state.cached_phys_addr = legacy_fb->physical_addr;
        fb_state.cached_width = legacy_fb->width;
        fb_state.cached_height = legacy_fb->height;
        fb_state.cached_pitch = legacy_fb->pitch;
        fb_state.cached_bpp = legacy_fb->bpp;
        fb_state.cached_size = legacy_fb->size;
        fb_state.cached_format = legacy_fb->format;
        fb_state.cached_double_buffered = legacy_fb->double_buffered;

        uint32_t visible_size = fb_state.cached_pitch * fb_state.cached_height;
        if (fb_state.cached_size < visible_size) {
            fb_state.cached_size = visible_size;
        }

        bool changed = !s_logged_cache_valid ||
                       s_logged_phys != fb_state.cached_phys_addr ||
                       s_logged_w != fb_state.cached_width ||
                       s_logged_h != fb_state.cached_height ||
                       s_logged_pitch != fb_state.cached_pitch ||
                       s_logged_bpp != fb_state.cached_bpp ||
                       s_logged_fmt != fb_state.cached_format;
        if (changed) {
            debuglog(DEBUG_INFO,
                     "[FB_CACHE] LEGACY: %ux%u pitch=%u bpp=%u fmt=%u phys=0x%x\n",
                     fb_state.cached_width, fb_state.cached_height,
                     fb_state.cached_pitch, fb_state.cached_bpp, fb_state.cached_format,
                     (uint32_t)fb_state.cached_phys_addr);
            s_logged_cache_valid = true;
            s_logged_phys = fb_state.cached_phys_addr;
            s_logged_w = fb_state.cached_width;
            s_logged_h = fb_state.cached_height;
            s_logged_pitch = fb_state.cached_pitch;
            s_logged_bpp = fb_state.cached_bpp;
            s_logged_fmt = fb_state.cached_format;
        }
        return 0;
    }

    uintptr_t mb_addr = 0;
    uint32_t mb_width = 0, mb_height = 0, mb_bpp = 0, mb_pitch = 0;

    if (kernel_get_multiboot_framebuffer(&mb_addr, &mb_width, &mb_height, &mb_bpp, &mb_pitch) &&
        mb_addr != 0 && mb_width != 0 && mb_height != 0) {
        fb_state.current_fb = NULL;
        fb_state.cached_phys_addr = mb_addr;
        fb_state.cached_width = mb_width;
        fb_state.cached_height = mb_height;
        fb_state.cached_pitch = mb_pitch;
        fb_state.cached_bpp = mb_bpp;
        fb_state.cached_size = mb_pitch * mb_height;
        fb_state.cached_format = (mb_bpp == 32) ? FB_FORMAT_BGRA_8888 :
                                   (mb_bpp == 24) ? FB_FORMAT_BGR_888 :
                                   (mb_bpp == 16) ? FB_FORMAT_RGB_565 : FB_FORMAT_RGB_888;
        fb_state.cached_double_buffered = false;

        bool changed = !s_logged_cache_valid ||
                       s_logged_phys != fb_state.cached_phys_addr ||
                       s_logged_w != fb_state.cached_width ||
                       s_logged_h != fb_state.cached_height ||
                       s_logged_pitch != fb_state.cached_pitch ||
                       s_logged_bpp != fb_state.cached_bpp ||
                       s_logged_fmt != fb_state.cached_format;
        if (changed) {
            debuglog(DEBUG_INFO,
                     "[FB_CACHE] MB: %ux%u pitch=%u bpp=%u fmt=%u phys=0x%x\n",
                     fb_state.cached_width, fb_state.cached_height,
                     fb_state.cached_pitch, fb_state.cached_bpp, fb_state.cached_format,
                     (uint32_t)fb_state.cached_phys_addr);
            s_logged_cache_valid = true;
            s_logged_phys = fb_state.cached_phys_addr;
            s_logged_w = fb_state.cached_width;
            s_logged_h = fb_state.cached_height;
            s_logged_pitch = fb_state.cached_pitch;
            s_logged_bpp = fb_state.cached_bpp;
            s_logged_fmt = fb_state.cached_format;
        }
        return 0;
    }

    debuglog(DEBUG_ERROR, "[FB_CACHE] No framebuffer available from any source\n");
    return -1;
}

static void flush_framebuffer_update(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static fb_mapping_t* find_mapping(void* virt_addr) {
    fb_mapping_t* mapping = fb_state.mappings;
    while (mapping) {
        if (mapping->virtual_addr == virt_addr) {
            return mapping;
        }
        mapping = mapping->next;
    }
    return NULL;
}

static fb_mapping_t* add_mapping(void* virt_addr, uintptr_t phys_addr, size_t size, uint32_t flags) {
    fb_mapping_t* mapping = (fb_mapping_t*)kmalloc(sizeof(fb_mapping_t));
    if (!mapping) {
        return NULL;
    }

    mapping->virtual_addr = virt_addr;
    mapping->physical_addr = phys_addr;
    mapping->size = size;
    mapping->flags = flags;
    mapping->task = current_task;
    mapping->task_pid = current_task ? current_task->id : 0;
    mapping->region_id = 0;
    mapping->next = fb_state.mappings;
    fb_state.mappings = mapping;

    return mapping;
}

static int remove_mapping(void* virt_addr) {
    fb_mapping_t** current = &fb_state.mappings;
    while (*current) {
        if ((*current)->virtual_addr == virt_addr) {
            fb_mapping_t* to_remove = *current;
            *current = (*current)->next;
            kfree(to_remove);
            return FB_SUCCESS;
        }
        current = &(*current)->next;
    }
    return FB_ERROR_NOT_MAPPED;
}

static void cleanup_task_mappings(task_t* task) {
    if (!task) {
        return;
    }

    fb_mapping_t** current = &fb_state.mappings;
    while (*current) {
        if ((*current)->task == task || ((*current)->task_pid != 0 && (*current)->task_pid == task->id)) {
            fb_mapping_t* to_remove = *current;
            *current = (*current)->next;

            (void)unmap_framebuffer_from_task(task, to_remove->virtual_addr, to_remove->size);
            kfree(to_remove);
        } else {
            current = &(*current)->next;
        }
    }
}

static void prune_stale_mappings_locked(void) {
    fb_mapping_t** current = &fb_state.mappings;
    while (*current) {
        fb_mapping_t* entry = *current;
        if (entry->task_pid == 0 || !task_exists(entry->task_pid)) {
            *current = entry->next;
            kfree(entry);
            continue;
        }
        current = &entry->next;
    }
}

static void* map_framebuffer_to_user(uintptr_t phys_addr, void* virt_start, size_t size, uint32_t flags) {
    (void)flags;
    if (!phys_addr || !size) {
        return NULL;
    }

    task_t* current = current_task;
    if (!current || !current->page_directory) {
        return NULL;
    }

    uintptr_t aligned_phys = phys_addr & ~(PAGE_SIZE - 1);
    uintptr_t offset = phys_addr - aligned_phys;
    size_t aligned_size = (size + offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    uintptr_t virt_addr;
    if (virt_start) {
        virt_addr = (uintptr_t)virt_start;
    } else {
        const uintptr_t search_start = 0x70000000u;
        const uintptr_t search_end = (uintptr_t)USER_STACK_TOP - MEMORY_PAGE_SIZE;
        bool found = false;

        if (aligned_size == 0 || aligned_size > (search_end - search_start)) {
            return NULL;
        }

        for (virt_addr = search_start;
             virt_addr + aligned_size <= search_end;
             virt_addr += MEMORY_PAGE_SIZE) {
            bool collision = false;
            for (uintptr_t check = virt_addr; check < virt_addr + aligned_size; check += PAGE_SIZE) {
                uintptr_t existing = vmm_get_physical_addr(current->page_directory, check);
                if (existing != 0) {
                    collision = true;
                    break;
                }
            }
            if (!collision) {
                found = true;
                break;
            }
        }

        if (!found) {
            return NULL;
        }
    }

    uint32_t page_flags = FB_PAGE_FLAGS;

    for (size_t i = 0; i < aligned_size; i += PAGE_SIZE) {
        uintptr_t current_virt = virt_addr + i;
        uintptr_t current_phys = aligned_phys + i;

        memory_result_t result = vmm_map_page(current->page_directory,
                                             current_virt,
                                             current_phys,
                                             page_flags);

        if (result != MEMORY_OK) {
            for (size_t j = 0; j < i; j += PAGE_SIZE) {
                vmm_unmap_page(current->page_directory, virt_addr + j);
            }
            tlb_invalidate_range((uint32_t)virt_addr, (uint32_t)(virt_addr + i));
            return NULL;
        }
    }

    tlb_invalidate_range((uint32_t)virt_addr, (uint32_t)(virt_addr + aligned_size));
    __asm__ volatile("mfence" ::: "memory");

    return (void*)(virt_addr + offset);
}

static int unmap_framebuffer_from_task(task_t* task, void* virt_addr, size_t size) {
    if (!task || !task->page_directory || !virt_addr || !size) {
        return FB_ERROR_INVALID_PARAM;
    }

    uintptr_t aligned_virt = (uintptr_t)virt_addr & ~(PAGE_SIZE - 1);
    uintptr_t offset = (uintptr_t)virt_addr - aligned_virt;
    size_t aligned_size = (size + offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (size_t i = 0; i < aligned_size; i += PAGE_SIZE) {
        vmm_unmap_page(task->page_directory, aligned_virt + i);
    }

    tlb_invalidate_range((uint32_t)aligned_virt, (uint32_t)(aligned_virt + aligned_size));
    __asm__ volatile("mfence" ::: "memory");

    return FB_SUCCESS;
}

static int unmap_framebuffer_from_user(void* virt_addr, size_t size) {
    if (!current_task || !current_task->page_directory) {
        return FB_ERROR_PERMISSION;
    }
    return unmap_framebuffer_from_task(current_task, virt_addr, size);
}

static int get_framebuffer_info(fb_info_t* info) {
    if (!info) {
        return FB_ERROR_INVALID_PARAM;
    }

    if (refresh_cached_framebuffer_info() != 0) {
        debuglog(DEBUG_ERROR, "[FB_INFO] Failed to get framebuffer info\n");
        return FB_ERROR_NOT_FOUND;
    }

    info->phys_addr = fb_state.cached_phys_addr;
    info->width = fb_state.cached_width;
    info->height = fb_state.cached_height;
    info->pitch = fb_state.cached_pitch;
    info->bpp = fb_state.cached_bpp;
    info->size = fb_state.cached_size;
    info->addr = NULL;
    info->format = fb_state.cached_format;

    info->flags = FB_FLAG_USER_ACCESSIBLE;
    if (fb_state.cached_double_buffered) {
        info->flags |= FB_FLAG_DOUBLE_BUFFERED;
    }

    debuglog(DEBUG_INFO, "[FB_INFO] Returning: %ux%u %ubpp pitch=%u, phys=0x%x, format=%u\n",
             info->width, info->height, info->bpp, info->pitch,
             (uint32_t)info->phys_addr, info->format);

    return FB_SUCCESS;
}

long sys_get_fb_info(fb_info_t* user_info) {
    if (!user_info) {
        return FB_ERROR_INVALID_PARAM;
    }
    if (!framebuffer_mmap_ensure_initialized()) {
        return FB_ERROR_NOT_FOUND;
    }

    if (memory_probe_user_buffer(user_info, sizeof(fb_info_t)) < sizeof(fb_info_t)) {
        return FB_ERROR_PERMISSION;
    }

    fb_info_t info;
    int result = get_framebuffer_info(&info);
    if (result != FB_SUCCESS) {
        return result;
    }

    debuglog(DEBUG_INFO, "[FB_INFO] sys_get_fb_info: phys=0x%08x, size=%u, %ux%ux%u, format=%u\n",
             (uint32_t)info.phys_addr, info.size, info.width, info.height, info.bpp, info.format);

    USER_ACCESS_BEGIN();
    memory_copy((const char*)&info, (char*)user_info, sizeof(fb_info_t));
    USER_ACCESS_END();

    return FB_SUCCESS;
}

long sys_mmap_fb(void) {
    fb_info_t info;
    if (!framebuffer_mmap_ensure_initialized()) {
        return FB_ERROR_NOT_FOUND;
    }
    int result = get_framebuffer_info(&info);
    if (result != FB_SUCCESS) {
        debuglog(DEBUG_ERROR, "[FB_MMAP] get_framebuffer_info failed: %d\n", result);
        return result;
    }

    if (!current_task) {
        return FB_ERROR_PERMISSION;
    }

    debuglog(DEBUG_INFO, "[FB_MMAP] Task %d: Mapping FB phys=0x%08x, size=%u\n",
             current_task->id, (uint32_t)info.phys_addr, info.size);

    bool force_single_buffer = false;
    bool restore_double_buffer = false;
    spinlock_acquire(&fb_state.lock);
    prune_stale_mappings_locked();
    if (fb_state.mappings == NULL) {
        if (refresh_cached_framebuffer_info() == 0 && fb_state.cached_double_buffered) {
            force_single_buffer = true;
            restore_double_buffer = true;
        }
    }
    spinlock_release(&fb_state.lock);

    if (force_single_buffer) {
        if (graphics_enable_double_buffering(false) == GRAPHICS_SUCCESS) {
            framebuffer_mmap_refresh();
        } else {
            debuglog(DEBUG_WARN, "[FB_MMAP] Failed to force single-buffer mode for userspace mapping\n");
        }
    }

    spinlock_acquire(&fb_state.lock);
    prune_stale_mappings_locked();

    fb_mapping_t* m = fb_state.mappings;
    while (m) {
        if (m->task_pid == current_task->id && m->virtual_addr != NULL) {
            uintptr_t new_phys_aligned = ((uintptr_t)info.phys_addr) & ~(PAGE_SIZE - 1);
            uintptr_t old_phys_aligned = m->physical_addr & ~(PAGE_SIZE - 1);
            if (new_phys_aligned != old_phys_aligned || m->size != info.size) {
                debuglog(DEBUG_INFO,
                         "[FB_MMAP] Rebinding stale mapping pid=%u old(phys=0x%x,size=%u) new(phys=0x%x,size=%u)\n",
                         m->task_pid, (uint32_t)m->physical_addr, (uint32_t)m->size,
                         (uint32_t)info.phys_addr, info.size);
                (void)unmap_framebuffer_from_task(current_task, m->virtual_addr, m->size);
                remove_mapping(m->virtual_addr);
                break;
            }

            if (validate_framebuffer_mapping(m, current_task)) {
                m->task = current_task;
                debuglog(DEBUG_INFO, "[FB_MMAP] Returning existing valid mapping at %p\n", m->virtual_addr);
                spinlock_release(&fb_state.lock);
                return (long)m->virtual_addr;
            } else {
                debuglog(DEBUG_WARN, "[FB_MMAP] Existing mapping invalid, attempting recovery\n");
                if (recover_framebuffer_mapping(m, current_task)) {
                    m->task = current_task;
                    debuglog(DEBUG_INFO, "[FB_MMAP] Recovery successful, returning %p\n", m->virtual_addr);
                    spinlock_release(&fb_state.lock);
                    return (long)m->virtual_addr;
                }
                (void)unmap_framebuffer_from_task(current_task, m->virtual_addr, m->size);
                remove_mapping(m->virtual_addr);
                break;
            }
        }
        m = m->next;
    }

    if ((uintptr_t)info.phys_addr == 0 || info.size == 0) {
        debuglog(DEBUG_ERROR, "[FB_MMAP] Invalid framebuffer: phys=0x%x, size=%u\n",
                (uint32_t)info.phys_addr, info.size);
        spinlock_release(&fb_state.lock);
        return FB_ERROR_NOT_FOUND;
    }

    debuglog(DEBUG_INFO, "[FB_MMAP] About to map: phys=0x%x, size=%u\n", (uint32_t)info.phys_addr, info.size);
    void* virt_addr = map_framebuffer_to_user((uintptr_t)info.phys_addr, NULL, info.size,
                                               info.flags | FB_PAGE_FLAGS);
    if (!virt_addr) {
        debuglog(DEBUG_ERROR, "[FB_MMAP] map_framebuffer_to_user failed\n");
        spinlock_release(&fb_state.lock);
        return FB_ERROR_NO_MEMORY;
    }

    debuglog(DEBUG_INFO, "[FB_MMAP] Mapped framebuffer to virtual address %p\n", virt_addr);

    fb_mapping_t* mapping = add_mapping(virt_addr, (uintptr_t)info.phys_addr, info.size, info.flags);
    if (!mapping) {
        unmap_framebuffer_from_user(virt_addr, info.size);
        spinlock_release(&fb_state.lock);
        return FB_ERROR_NO_MEMORY;
    }

    if (mapping && mapping->next == NULL) {
        fb_state.userspace_forced_single_buffer = force_single_buffer;
        fb_state.restore_double_buffer_on_last_unmap = restore_double_buffer;
    }

    fb_state.last_page_directory = vmm_get_current_page_directory();
    fb_state.validation_failures = 0;

    for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
        if (!fb_state.regions[i].active) {
            fb_state.regions[i].phys_addr = (uintptr_t)info.phys_addr;
            fb_state.regions[i].virt_addr = virt_addr;
            fb_state.regions[i].width = info.width;
            fb_state.regions[i].height = info.height;
            fb_state.regions[i].pitch = info.pitch;
            fb_state.regions[i].bpp = info.bpp;
            fb_state.regions[i].size = info.size;
            fb_state.regions[i].format = info.format;
            fb_state.regions[i].active = true;
            fb_state.regions[i].owner_pid = current_task->id;
            mapping->region_id = i;
            __asm__ volatile("" ::: "memory");
            fb_state.region_table_version++;
            break;
        }
    }

    current_task->has_framebuffer_mapping = true;

    spinlock_release(&fb_state.lock);

    __asm__ volatile("mfence" ::: "memory");

    return (long)virt_addr;
}

long sys_munmap_fb(void* addr) {
    if (!addr) {
        return FB_ERROR_INVALID_PARAM;
    }
    if (!framebuffer_mmap_ensure_initialized()) {
        return FB_ERROR_NOT_MAPPED;
    }

    spinlock_acquire(&fb_state.lock);
    prune_stale_mappings_locked();

    fb_mapping_t* mapping = find_mapping(addr);
    if (!mapping || !current_task || mapping->task_pid != current_task->id) {
        spinlock_release(&fb_state.lock);
        return FB_ERROR_NOT_MAPPED;
    }

    if (fb_state.flock.held && fb_state.flock.owner_pid == current_task->id) {
        fb_release_flock(current_task->id);
    }

    int result = unmap_framebuffer_from_user(addr, mapping->size);
    if (result == FB_SUCCESS) {
        uint32_t region_id = mapping->region_id;
        bool is_last_for_region = true;
        fb_mapping_t* check = fb_state.mappings;
        while (check) {
            if (check != mapping && check->region_id == region_id) {
                is_last_for_region = false;
                break;
            }
            check = check->next;
        }

        if (is_last_for_region && region_id < FB_MAX_REGIONS && fb_state.regions[region_id].active) {
            fb_state.regions[region_id].active = false;
            __asm__ volatile("" ::: "memory");
            fb_state.region_table_version++;
        }

        remove_mapping(addr);
    }

    bool restore_double_buffer = false;
    if (result == FB_SUCCESS && fb_state.mappings == NULL) {
        restore_double_buffer = fb_state.restore_double_buffer_on_last_unmap;
        fb_state.userspace_forced_single_buffer = false;
        fb_state.restore_double_buffer_on_last_unmap = false;
    }

    if (fb_state.mappings == NULL) {
        current_task->has_framebuffer_mapping = false;
    }

    spinlock_release(&fb_state.lock);

    if (restore_double_buffer) {
        if (graphics_enable_double_buffering(true) == GRAPHICS_SUCCESS) {
            framebuffer_mmap_refresh();
        } else {
            debuglog(DEBUG_WARN, "[FB_MMAP] Failed to restore double-buffer mode after last userspace unmap\n");
        }
    }

    __asm__ volatile("mfence" ::: "memory");

    return result;
}

long sys_fb_flush(void) {
    if (!current_task) {
        return FB_ERROR_PERMISSION;
    }
    if (!framebuffer_mmap_ensure_initialized()) {
        return FB_ERROR_NOT_FOUND;
    }

    bool has_mapping = current_task->has_framebuffer_mapping;
    if (!has_mapping) {
        spinlock_acquire(&fb_state.lock);
        fb_mapping_t* m = fb_state.mappings;
        while (m) {
            if (m->task_pid == current_task->id) {
                has_mapping = true;
                break;
            }
            m = m->next;
        }
        spinlock_release(&fb_state.lock);
    }

    if (!has_mapping) {
        return FB_ERROR_NOT_MAPPED;
    }

    spinlock_acquire(&fb_state.lock);
    fb_state.last_flush_time = timer_get_ticks();
    spinlock_release(&fb_state.lock);

    flush_framebuffer_update();
    dirty_clear(current_task->id);

    if (!framebuffer_has_userspace_mapping()) {
        gfx_flush_framebuffer();
    }

    return FB_SUCCESS;
}

long sys_start_fb_watcher(void) {
    if (!framebuffer_mmap_ensure_initialized()) {
        return FB_ERROR_NOT_FOUND;
    }

    fb_state.auto_refresh_enabled = false;

    return FB_SUCCESS;
}

long sys_stop_fb_watcher(void) {
    if (!fb_state.initialized) {
        return FB_SUCCESS;
    }
    fb_state.auto_refresh_enabled = false;
    return FB_SUCCESS;
}

long sys_fb_lock(void) {
    if (!current_task) return FB_ERROR_PERMISSION;
    if (!framebuffer_mmap_ensure_initialized()) return FB_ERROR_NOT_FOUND;

    spinlock_acquire(&fb_state.lock);
    bool acquired = fb_acquire_flock(current_task->id, false);
    spinlock_release(&fb_state.lock);

    if (!acquired) {
        debuglog(DEBUG_WARN, "[FB_FLOCK] Task %d failed to acquire lock (owner=%d)\n",
                 current_task->id, fb_state.flock.owner_pid);
        return FB_ERROR_PERMISSION;
    }

    debuglog(DEBUG_INFO, "[FB_FLOCK] Task %d acquired lock\n", current_task->id);
    return FB_SUCCESS;
}

long sys_fb_unlock(void) {
    if (!current_task) return FB_ERROR_PERMISSION;
    if (!framebuffer_mmap_ensure_initialized()) return FB_ERROR_NOT_FOUND;

    spinlock_acquire(&fb_state.lock);
    bool released = fb_release_flock(current_task->id);
    spinlock_release(&fb_state.lock);

    if (!released) {
        return FB_ERROR_PERMISSION;
    }

    debuglog(DEBUG_INFO, "[FB_FLOCK] Task %d released lock\n", current_task->id);
    return FB_SUCCESS;
}

long sys_fb_dirty_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!current_task) return FB_ERROR_PERMISSION;
    if (w == 0 || h == 0) return FB_ERROR_INVALID_PARAM;

    dirty_track_rect(current_task->id, x, y, w, h);
    return FB_SUCCESS;
}

long sys_fb_get_regions(uint32_t* user_count, void* user_buf, uint32_t max_regions) {
    if (!user_count) return FB_ERROR_INVALID_PARAM;
    if (!framebuffer_mmap_ensure_initialized()) return FB_ERROR_NOT_FOUND;

    spinlock_acquire(&fb_state.lock);
    uint32_t count = fb_get_region_count();

    USER_ACCESS_BEGIN();
    memory_copy((const char*)&count, (char*)user_count, sizeof(uint32_t));

    if (user_buf && max_regions > 0) {
        uint32_t to_copy = count < max_regions ? count : max_regions;
        for (uint32_t i = 0; i < to_copy; i++) {
            if (fb_state.regions[i].active) {
                fb_region_t info;
                fb_get_region_info(i, &info);
                memory_copy((const char*)&info, (char*)user_buf + i * sizeof(fb_region_t), sizeof(fb_region_t));
            }
        }
    }
    USER_ACCESS_END();

    spinlock_release(&fb_state.lock);
    return FB_SUCCESS;
}

int framebuffer_mmap_init(void) {
    if (fb_state.initialized) {
        return 0;
    }

    fb_state.mappings = NULL;
    fb_state.auto_refresh_enabled = false;
    fb_state.last_flush_time = 0;
    fb_state.flush_interval_ms = 16;
    fb_state.current_fb = NULL;
    fb_state.last_page_directory = NULL;
    fb_state.validation_failures = 0;
    fb_state.recovery_in_progress = false;
    fb_state.userspace_forced_single_buffer = false;
    fb_state.restore_double_buffer_on_last_unmap = false;
    fb_state.double_buffer_owner_pid = 0;
    fb_state.preserve_last_frame = false;

    fb_state.region_count = 0;
    fb_state.dirty_tracker_count = 0;
    fb_state.flock.owner_pid = FB_FLOCK_UNOWNED;
    fb_state.flock.held = false;
    fb_state.region_table_version = 0;
    fb_state.mode_generation = 1;

    for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
        fb_state.regions[i].active = false;
    }
    for (uint32_t i = 0; i < 16; i++) {
        fb_state.dirty_trackers[i].count = 0;
        fb_state.dirty_trackers[i].full_screen = false;
        fb_state.dirty_trackers[i].in_use = false;
    }

    spinlock_init(&fb_state.lock, "fb_mmap");
    fb_state.initialized = true;

    refresh_cached_framebuffer_info();

    debuglog(DEBUG_INFO, "[FB_INIT] Framebuffer mmap subsystem initialized\n");
    return 0;
}

bool framebuffer_mmap_handle_page_fault(uint32_t fault_addr) {
    if (!fb_state.initialized) return false;

    if (validate_fb_page(fault_addr)) {
        debuglog(DEBUG_INFO, "[FB_PF] Re-mapped framebuffer page at 0x%x\n", fault_addr);
        return true;
    }

    return false;
}

static bool check_page_directory_changed(void) {
    page_directory_t* current_pd = vmm_get_current_page_directory();

    if (fb_state.last_page_directory == NULL) {
        fb_state.last_page_directory = current_pd;
        return false;
    }

    if (fb_state.last_page_directory != current_pd) {
        debuglog(DEBUG_INFO, "[FB_VALIDATION] Page directory changed: %p -> %p\n",
                 fb_state.last_page_directory, current_pd);
        fb_state.last_page_directory = current_pd;
        return true;
    }

    return false;
}

static bool validate_framebuffer_mapping(fb_mapping_t* mapping, task_t* owner_task) {
    if (!mapping || !mapping->virtual_addr || !owner_task) {
        return false;
    }

    if (!owner_task->page_directory) {
        debuglog(DEBUG_WARN, "[FB_VALIDATION] Task has no page directory\n");
        return false;
    }

    uintptr_t aligned_virt = ((uintptr_t)mapping->virtual_addr) & ~(PAGE_SIZE - 1);
    uintptr_t virt_offset = (uintptr_t)mapping->virtual_addr - aligned_virt;
    uintptr_t aligned_phys = mapping->physical_addr & ~(PAGE_SIZE - 1);
    size_t aligned_size = (mapping->size + virt_offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (size_t i = 0; i < aligned_size; i += PAGE_SIZE) {
        uintptr_t current_phys = vmm_get_physical_addr(owner_task->page_directory, aligned_virt + i);
        if (current_phys == 0) {
            debuglog(DEBUG_WARN, "[FB_VALIDATION] Virtual page 0x%x unmapped\n",
                     (uint32_t)(aligned_virt + i));
            return false;
        }
        if (current_phys != (aligned_phys + i)) {
            debuglog(DEBUG_WARN,
                     "[FB_VALIDATION] Physical mismatch at +0x%x: expected 0x%x got 0x%x\n",
                     (uint32_t)i, (uint32_t)(aligned_phys + i), (uint32_t)current_phys);
            return false;
        }
    }

    return true;
}

static bool recover_framebuffer_mapping(fb_mapping_t* mapping, task_t* owner_task) {
    if (!mapping || !owner_task || fb_state.recovery_in_progress) {
        return false;
    }

    fb_state.recovery_in_progress = true;
    debuglog(DEBUG_INFO, "[FB_RECOVERY] Attempting to recover mapping at %p\n",
             mapping->virtual_addr);

    (void)unmap_framebuffer_from_task(owner_task, mapping->virtual_addr, mapping->size);

    void* new_virt = map_framebuffer_to_user(
        mapping->physical_addr,
        mapping->virtual_addr,
        mapping->size,
        mapping->flags
    );

    if (new_virt) {
        debuglog(DEBUG_INFO, "[FB_RECOVERY] Successfully remapped framebuffer at %p\n", new_virt);
        mapping->virtual_addr = new_virt;
        fb_state.validation_failures = 0;
        fb_state.recovery_in_progress = false;
        return true;
    }

    new_virt = map_framebuffer_to_user(
        mapping->physical_addr,
        NULL,
        mapping->size,
        mapping->flags
    );

    if (new_virt) {
        debuglog(DEBUG_INFO, "[FB_RECOVERY] Remapped at new address %p\n", new_virt);
        mapping->virtual_addr = new_virt;
        fb_state.validation_failures = 0;
        fb_state.recovery_in_progress = false;
        return true;
    }

    debuglog(DEBUG_ERROR, "[FB_RECOVERY] Failed to recover mapping\n");
    fb_state.recovery_in_progress = false;
    return false;
}

static void rebind_mapping_to_current_fb(fb_mapping_t* mapping) {
    if (!mapping || !current_task || !current_task->page_directory) {
        return;
    }

    fb_info_t info;
    if (get_framebuffer_info(&info) != FB_SUCCESS || info.phys_addr == 0 || info.size == 0) {
        return;
    }

    if (mapping->physical_addr == (uintptr_t)info.phys_addr && mapping->size == info.size) {
        return;
    }

    (void)unmap_framebuffer_from_task(current_task, mapping->virtual_addr, mapping->size);

    void* new_virt = map_framebuffer_to_user((uintptr_t)info.phys_addr,
                                             mapping->virtual_addr,
                                             info.size,
                                             info.flags | FB_PAGE_FLAGS);
    if (new_virt) {
        mapping->virtual_addr = new_virt;
        mapping->physical_addr = (uintptr_t)info.phys_addr;
        mapping->size = info.size;
        mapping->flags = info.flags;
    }
}

static void handle_mapping_corruption(fb_mapping_t* mapping) {
    fb_state.validation_failures++;

    debuglog(DEBUG_WARN, "[FB_CORRUPTION] Mapping validation failed (#%u)\n",
             fb_state.validation_failures);

    if (fb_state.validation_failures < 5) {
        if (recover_framebuffer_mapping(mapping, current_task)) {
            debuglog(DEBUG_INFO, "[FB_CORRUPTION] Recovery successful\n");
        } else {
            debuglog(DEBUG_ERROR, "[FB_CORRUPTION] Recovery failed\n");
        }
    } else {
        debuglog(DEBUG_ERROR, "[FB_CORRUPTION] Too many failures, giving up\n");
    }
}

void framebuffer_validate_all_mappings(void) {
    if (!fb_state.initialized) {
        return;
    }
    if (!fb_state.mappings) {
        return;
    }

    spinlock_acquire(&fb_state.lock);
    prune_stale_mappings_locked();

    fb_mapping_t* mapping = fb_state.mappings;
    while (mapping) {
        if (current_task && mapping->task_pid == current_task->id) {
            mapping->task = current_task;
            if (!validate_framebuffer_mapping(mapping, current_task)) {
                handle_mapping_corruption(mapping);
            }
        }
        mapping = mapping->next;
    }

    spinlock_release(&fb_state.lock);
}

void framebuffer_update_periodic(void) {
}

void framebuffer_mmap_set_double_buffer_owner(uint32_t pid) {
    spinlock_acquire(&fb_state.lock);
    fb_state.double_buffer_owner_pid = pid;
    spinlock_release(&fb_state.lock);
}

void framebuffer_set_preserve_last_frame(bool preserve) {
    spinlock_acquire(&fb_state.lock);
    fb_state.preserve_last_frame = preserve;
    spinlock_release(&fb_state.lock);
    debuglog(DEBUG_INFO, "[FB_MMAP] preserve_last_frame=%d\n", preserve);
}

void framebuffer_mmap_task_exit(task_t* task) {
    if (!fb_state.initialized || !task) {
        return;
    }

    spinlock_acquire(&fb_state.lock);

    if (fb_state.flock.held && fb_state.flock.owner_pid == task->id) {
        fb_release_flock(task->id);
        debuglog(DEBUG_INFO, "[FB_MMAP] Released flock for exiting task %d\n", task->id);
    }

    for (uint32_t i = 0; i < 16; i++) {
        if (fb_state.dirty_trackers[i].in_use && fb_state.dirty_trackers[i].pid == task->id) {
            fb_state.dirty_trackers[i].in_use = false;
            fb_state.dirty_trackers[i].count = 0;
            fb_state.dirty_trackers[i].full_screen = false;
        }
    }

    bool restore_double_buffer = false;
    bool disable_double_buffer = false;
    prune_stale_mappings_locked();
    cleanup_task_mappings(task);

    if (fb_state.double_buffer_owner_pid == task->id) {
        disable_double_buffer = true;
        fb_state.double_buffer_owner_pid = 0;
        debuglog(DEBUG_INFO, "[FB_MMAP] Task %d (double buffer owner) exiting, disabling double buffering\n", task->id);
    }

    for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
        if (fb_state.regions[i].active && fb_state.regions[i].owner_pid == task->id) {
            fb_state.regions[i].active = false;
            __asm__ volatile("" ::: "memory");
            fb_state.region_table_version++;
        }
    }

    if (fb_state.mappings == NULL) {
        restore_double_buffer = fb_state.restore_double_buffer_on_last_unmap;
        fb_state.userspace_forced_single_buffer = false;
        fb_state.restore_double_buffer_on_last_unmap = false;
    }

    spinlock_release(&fb_state.lock);

    task->has_framebuffer_mapping = false;

    if (disable_double_buffer) {
        if (!fb_state.preserve_last_frame) {
            if (graphics_enable_double_buffering(false) == GRAPHICS_SUCCESS) {
                framebuffer_mmap_refresh();
            } else {
                debuglog(DEBUG_WARN, "[FB_MMAP] Failed to disable double-buffer mode after owner task exit\n");
            }
        } else {
            debuglog(DEBUG_INFO, "[FB_MMAP] Skipping double-buffer disable during frame handoff\n");
        }
    } else if (restore_double_buffer) {
        if (!fb_state.preserve_last_frame) {
            if (graphics_enable_double_buffering(true) == GRAPHICS_SUCCESS) {
                framebuffer_mmap_refresh();
            } else {
                debuglog(DEBUG_WARN, "[FB_MMAP] Failed to restore double-buffer mode after task exit cleanup\n");
            }
        } else {
            debuglog(DEBUG_INFO, "[FB_MMAP] Skipping double-buffer restore during frame handoff\n");
        }
    }
}

void framebuffer_mmap_refresh(void) {
    if (!fb_state.initialized) {
        return;
    }

    spinlock_acquire(&fb_state.lock);
    prune_stale_mappings_locked();
    refresh_cached_framebuffer_info();
    for (fb_mapping_t* m = fb_state.mappings; m; m = m->next) {
        if (current_task && m->task_pid == current_task->id) {
            m->task = current_task;
            rebind_mapping_to_current_fb(m);
        }
    }
    spinlock_release(&fb_state.lock);
}

// Unmap framebuffer from all tasks - called when switching display modes
// to ensure clean state and prevent multiple processes from writing simultaneously
void framebuffer_mmap_unmap_all(void) {
    if (!fb_state.initialized) {
        return;
    }

    spinlock_acquire(&fb_state.lock);

    fb_mapping_t* m = fb_state.mappings;
    while (m) {
        fb_mapping_t* next = m->next;
        task_t* task = m->task;
        if (task && task->page_directory) {
            (void)unmap_framebuffer_from_task(task, m->virtual_addr, m->size);
            task->has_framebuffer_mapping = false;
        }
        kfree(m);
        m = next;
    }
    fb_state.mappings = NULL;

    // Clear all regions
    for (uint32_t i = 0; i < FB_MAX_REGIONS; i++) {
        if (fb_state.regions[i].active) {
            fb_state.regions[i].active = false;
        }
    }
    __asm__ volatile("" ::: "memory");
    fb_state.region_table_version++;

    // Release framebuffer lock if held
    if (fb_state.flock.held) {
        fb_state.flock.held = false;
        fb_state.flock.owner_pid = FB_FLOCK_UNOWNED;
    }

    // Bump the generation counter so polling clients (see
    // framebuffer_mmap_get_mode_generation()) notice their mapping was
    // dropped and re-map via SYS_MMAP_FB before touching it again.
    fb_state.mode_generation++;

    spinlock_release(&fb_state.lock);

    debuglog(DEBUG_INFO, "[FB_MMAP] Unmapped framebuffer from all tasks\n");
}

uint32_t framebuffer_mmap_get_mode_generation(void) {
    return fb_state.mode_generation;
}

bool framebuffer_has_userspace_mapping(void) {
    if (!fb_state.mappings) {
        return false;
    }
    spinlock_acquire(&fb_state.lock);
    prune_stale_mappings_locked();
    bool found = (fb_state.mappings != NULL);
    spinlock_release(&fb_state.lock);
    return found;
}

uint32_t framebuffer_get_userspace_last_flush(void) {
    if (!fb_state.initialized) return 0;
    spinlock_acquire(&fb_state.lock);
    uint32_t t = fb_state.last_flush_time;
    spinlock_release(&fb_state.lock);
    return t;
}

uint32_t framebuffer_get_userspace_mapping_duration_ms(void) {
    if (!fb_state.initialized) return 0;
    spinlock_acquire(&fb_state.lock);
    bool mapped = (fb_state.mappings != NULL);
    uint32_t last = fb_state.last_flush_time;
    spinlock_release(&fb_state.lock);
    if (!mapped || last == 0) return 0;
    uint32_t now = timer_get_ticks();
    return (now - last) * fb_state.flush_interval_ms;
}

#else /* !HAS_FRAMEBUFFER */

/* No-framebuffer stubs for the framebuffer mmap path. Keeps the syscall
 * dispatch and kernel boot path linking when ENABLE_FRAMEBUFFER=0. */

long sys_get_fb_info(fb_info_t* user_info)              { (void)user_info; return FB_ERROR_NOT_FOUND; }
long sys_mmap_fb(void)                                  { return FB_ERROR_NOT_FOUND; }
long sys_munmap_fb(void* addr)                          { (void)addr; return FB_ERROR_NOT_MAPPED; }
long sys_fb_flush(void)                                 { return FB_ERROR_NOT_MAPPED; }
long sys_start_fb_watcher(void)                         { return FB_ERROR_NOT_FOUND; }
long sys_stop_fb_watcher(void)                          { return FB_SUCCESS; }
long sys_fb_lock(void)                                  { return FB_ERROR_NOT_FOUND; }
long sys_fb_unlock(void)                                { return FB_ERROR_NOT_FOUND; }
long sys_fb_dirty_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)x; (void)y; (void)w; (void)h; return FB_ERROR_NOT_FOUND;
}
long sys_fb_get_regions(uint32_t* user_count, void* user_buf, uint32_t max_regions) {
    (void)user_count; (void)user_buf; (void)max_regions; return FB_ERROR_NOT_FOUND;
}

int      framebuffer_mmap_init(void)                    { return 0; }
bool     framebuffer_mmap_handle_page_fault(uint32_t fault_addr) { (void)fault_addr; return false; }
void     framebuffer_validate_all_mappings(void)       { }
void     framebuffer_update_periodic(void)             { }
void     framebuffer_mmap_set_double_buffer_owner(uint32_t pid) { (void)pid; }
void     framebuffer_set_preserve_last_frame(bool preserve) { (void)preserve; }
void     framebuffer_mmap_task_exit(task_t* task)      { (void)task; }
void     framebuffer_mmap_refresh(void)                 { }
void     framebuffer_mmap_unmap_all(void)               { }
uint32_t framebuffer_mmap_get_mode_generation(void)    { return 0; }
bool     framebuffer_has_userspace_mapping(void)       { return false; }
uint32_t framebuffer_get_userspace_last_flush(void)    { return 0; }
uint32_t framebuffer_get_userspace_mapping_duration_ms(void) { return 0; }

#endif /* HAS_FRAMEBUFFER */

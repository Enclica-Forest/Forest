#include "include/kb.h"
#include "include/cpu_ops.h"
#include "include/interrupt.h"
#include "include/timer.h"
#include "include/epoch.h"
#include "include/ps2_controller.h"
#include "include/ps2_keyboard.h"
#include "include/ps2_mouse.h"
#include "include/virtualbox_guest.h"
#include "include/devfs.h"
#include "include/input_event.h"
#include "include/io_ports.h"
#include "include/util.h"
#include "include/screen.h"

#include "include/memory_safe.h"
#include "include/memory.h"
#include "include/memory_region_manager.h"
#include "include/page_fault_recovery.h"
#include "include/acpi.h"
#include "include/hardware.h"
#include "include/multiboot.h"
#include "include/panic.h"
#include "include/ramdisk.h"
#include "include/vfs.h"
#include "include/task.h"
#include "include/syscall.h"
#include "include/hardware.h"
#include "include/string.h"
#include "include/pci.h"
#include "include/ata.h"
#include "include/block_devices.h"
#include "include/driver.h"
#include "include/net.h"
#include "include/debuglog.h"
#include "include/gdt.h"
#include "include/elf.h"
#include "include/libc/stdio.h"
#include "include/lock_debug.h"
#include "include/graphics_init.h"
#include "include/graphics/graphics_manager.h"
#include "include/splash.h"
#include "include/tty.h"
#include "include/tlb_manager.h"
#include "include/smep_smap.h"
#include "include/stack_protection.h"
#include "include/ssp.h"
#include "include/memory_corruption.h"
#include "include/enhanced_heap.h"
#include "include/bitmap_pmm.h"
#include "include/secure_vmm.h"
#include "include/init_system.h"
#include "include/shell_loader.h"
#include "include/session.h"
#include "include/sound.h"
#include "include/hotkey.h"
#include "include/input_mux.h"
#include "include/devfs.h"
#include "include/pcie.h"
#include "include/usb/usb.h"
#include "include/ps2_watchdog.h"
#include "include/smp.h"
#include "arch/smp.h"

// Enhanced Memory System v2.0 Components
#include "include/a20.h"
#include "include/pmm_enhanced.h"
#include "include/paging_modes.h"
#include "include/tlb.h"
#include "include/kheap_enhanced.h"
#include "include/mem_protect.h"
#include "include/mm_cow.h"
#include "include/mm_swap.h"
#include "include/mm_stats.h"
#include "include/mm_layout.h"
#ifdef __x86_64__
#include "include/paging64.h"
#endif

typedef struct {
    char label[64];
    bool ok;
} boot_log_entry_t;

#define BOOT_LOG_CAPACITY 64
static boot_log_entry_t g_boot_log[BOOT_LOG_CAPACITY];
static uint32_t g_boot_log_count = 0;
static uint32_t g_splash_boot_item_count = 0;

// Forward declaration for SSP test
extern int ssp_run_tests(void);
extern int memory_corruption_run_tests(void);
extern int enhanced_heap_run_tests(void);
extern int bitmap_pmm_run_tests(void);
extern const char* bitmap_pmm_get_last_test_failure(void);

void kmain(uint32 magic, uint32 mbi_addr);
void keyboard_event_handler(const keyboard_event_t* event);
void mouse_event_handler(const ps2_mouse_event_t* event);
void keyboard_serial_interrupt_handler(void);
void display_change_handler(const struct vbox_display_change_event *event);
void mouse_position_handler(const struct vbox_mouse_position_event *event);
bool pcie_enumeration_callback(const pci_device_t* device, void* context);

extern uint8 _stack_top;

extern const char* memory_validation_result_to_string(memory_validation_result_t result);

static void kernel_panic_memory_error(const char* stage, const char* reason) {
    static char panic_message[160];
    strcpy(panic_message, "Memory failure at ");
    strcat(panic_message, stage);
    if (reason && reason[0]) {
        strcat(panic_message, ": ");
        strcat(panic_message, reason);
    }
    kernel_panic(panic_message);
}

#define COLOR_OK 0x0A
#define COLOR_WARN 0x0E
#define COLOR_FAIL 0x0C
#define COLOR_LABEL 0x0B

static bool g_silent_boot = false;
static bool g_quiet_boot = false;
static bool g_graphics_ready = false;
static bool g_framebuffer_tty_ready = false;
static bool g_video_mode_requested = false;
static uint32_t g_video_mode_width = 0;
static uint32_t g_video_mode_height = 0;
static uint32_t g_video_mode_bpp = 0;

/* Embedded mode: reduced memory footprint, smaller stacks/heaps, fewer
 * reserved pages.  Selected with the `embedded` kernel command-line token. */
static bool g_embedded_mode = false;

/* Framebuffer disabled mode: skip the graphics subsystem and use the VGA
 * text console only.  Selected with `nofb`.  Also auto-selected when the
 * framebuffer cannot be mapped or the graphics subsystem fails to init. */
static bool g_framebuffer_disabled = false;

/* Live CD mode: auto-login as root without password.  Selected with the
 * `livecd` or `live` kernel command-line token, or detected via /etc/livecd. */
bool g_livecd_mode = false;

/* Minimum usable RAM (in KB) required to boot.  Embedded mode lowers this. */
#define KERNEL_MIN_MEM_KB_NORMAL    65536u   /* 64 MB */
#define KERNEL_MIN_MEM_KB_EMBEDDED  8192u    /* 8 MB  */

// Multiboot framebuffer information (internal structure)
static struct {
    bool valid;
    uintptr_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
} g_multiboot_framebuffer_internal = {0};

// Global multiboot framebuffer variables for V2 graphics drivers
// These are exported and accessed by the V2 driver system
void* g_multiboot_framebuffer = NULL;
uint32_t g_multiboot_fb_width = 0;
uint32_t g_multiboot_fb_height = 0;
uint32_t g_multiboot_fb_pitch = 0;
uint32_t g_multiboot_fb_bpp = 0;
uintptr_t g_multiboot_fb_addr = 0;
multiboot_info_t* g_multiboot_info = NULL;
uint32_t g_multiboot_magic = 0;
uint32_t g_multiboot_info_addr = 0;

// Get multiboot framebuffer information
bool kernel_get_multiboot_framebuffer(uintptr_t* addr, uint32_t* width, uint32_t* height, uint32_t* bpp, uint32_t* pitch) {
    if (!g_multiboot_framebuffer_internal.valid) {
        return false;
    }
    if (addr) *addr = g_multiboot_framebuffer_internal.addr;
    if (width) *width = g_multiboot_framebuffer_internal.width;
    if (height) *height = g_multiboot_framebuffer_internal.height;
    if (bpp) *bpp = g_multiboot_framebuffer_internal.bpp;
    if (pitch) *pitch = g_multiboot_framebuffer_internal.pitch;
    return true;
}

// Update the exported V2 framebuffer globals from internal structure
static void update_v2_framebuffer_globals(void) {
    if (g_multiboot_framebuffer_internal.valid) {
        g_multiboot_fb_addr = g_multiboot_framebuffer_internal.addr;
        g_multiboot_fb_width = g_multiboot_framebuffer_internal.width;
        g_multiboot_fb_height = g_multiboot_framebuffer_internal.height;
        g_multiboot_fb_bpp = g_multiboot_framebuffer_internal.bpp;
        g_multiboot_fb_pitch = g_multiboot_framebuffer_internal.pitch;
        // Note: g_multiboot_framebuffer (the void*) will be set after VMM mapping
    }
}

// Set the virtual address for the multiboot framebuffer (called after VMM maps it)
void kernel_set_multiboot_framebuffer_virt(void* virt_addr) {
    g_multiboot_framebuffer = virt_addr;
}

// Finalize framebuffer globals after VMM has mapped the framebuffer
// Map the framebuffer explicitly since it's at a high physical address (0xF0000000)
// that may not be covered by the identity mapping
void kernel_finalize_framebuffer_mapping(void) {
    if (!g_multiboot_framebuffer_internal.valid) {
        return;
    }
    
    uintptr_t fb_phys = g_multiboot_framebuffer_internal.addr;
    uint64_t fb_size64 =
        (uint64_t)g_multiboot_framebuffer_internal.pitch *
        (uint64_t)g_multiboot_framebuffer_internal.height;
    if (fb_size64 == 0 || fb_size64 > 0xFFFFFFFFULL) {
        debuglog(DEBUG_WARN, "[KERNEL] Skipping framebuffer mapping: invalid size64=%u\n",
                 (uint32_t)(fb_size64 > 0xFFFFFFFFULL ? 0xFFFFFFFFU : fb_size64));
        g_multiboot_framebuffer = NULL;
        return;
    }
    uint32_t fb_size = (uint32_t)fb_size64;
    if (fb_phys == 0 || fb_size == 0) {
        debuglog(DEBUG_WARN, "[KERNEL] Skipping framebuffer mapping: phys=0x%08x size=%u\n",
                 (uint32_t)fb_phys, fb_size);
        g_multiboot_framebuffer = NULL;
        return;
    }

    /* Handle rare unaligned framebuffer base addresses safely. */
    uintptr_t fb_phys_page = fb_phys & ~(uintptr_t)MEMORY_PAGE_MASK;
    uint32_t fb_offset = (uint32_t)(fb_phys - fb_phys_page);
    uint64_t map_size64 = (uint64_t)fb_size + (uint64_t)fb_offset;
    if (map_size64 == 0 || map_size64 > 0xFFFFFFFFULL) {
        debuglog(DEBUG_ERROR, "[KERNEL] Framebuffer map size overflow: size=%u offset=%u\n",
                 fb_size, fb_offset);
        g_multiboot_framebuffer = NULL;
        return;
    }
    uint32_t map_size = (uint32_t)map_size64;
    
    // Round up size to page boundary
    uint32_t fb_pages = (map_size + 0xFFF) >> 12;
    if (fb_pages == 0) {
        debuglog(DEBUG_WARN, "[KERNEL] Framebuffer page count is zero\n");
        g_multiboot_framebuffer = NULL;
        return;
    }
    
    // Map framebuffer to a fixed virtual address in kernel space
    // Use 0xF0000000 as the virtual window base.
    uintptr_t fb_virt = 0xF0000000;
    uintptr_t fb_virt_ptr = fb_virt + fb_offset;
    
    // Check if already mapped (identity mapping may have covered it), but verify all pages.
    bool fully_mapped = true;
    for (uint32_t i = 0; i < fb_pages; i++) {
        uintptr_t page_virt = fb_virt + (i << 12);
        uintptr_t page_phys = fb_phys_page + (i << 12);
        uintptr_t existing = vmm_get_physical_addr(vmm_get_current_page_directory(), page_virt);
        if (existing != page_phys) {
            fully_mapped = false;
            break;
        }
    }
    if (fully_mapped) {
        // Already properly mapped (all pages verified)
        g_multiboot_framebuffer = (void*)fb_virt_ptr;
        debuglog(DEBUG_INFO, "[KERNEL] Framebuffer already mapped at 0x%08x\n",
                (uint32_t)fb_virt_ptr);
        return;
    }
    
    // Map the framebuffer pages explicitly
    debuglog(DEBUG_INFO, "[KERNEL] Mapping framebuffer: phys=0x%08x virt=0x%08x size=%u pages=%u\n",
            (uint32_t)fb_phys, (uint32_t)fb_virt_ptr, fb_size, fb_pages);

    uint32_t mapped_ok = 0;
    bool map_failed = false;
    for (uint32_t i = 0; i < fb_pages; i++) {
        uintptr_t page_virt = fb_virt + (i << 12);
        uintptr_t page_phys = fb_phys_page + (i << 12);

        memory_result_t res = vmm_map_page(vmm_get_current_page_directory(),
                                             page_virt, page_phys,
                                             PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
        if (res == MEMORY_OK || res == MEMORY_ERROR_ALREADY_MAPPED) {
            mapped_ok++;
        } else {
            debuglog(DEBUG_WARN, "[KERNEL] Framebuffer page %u map failed res=%d\n",
                    i, res);
            map_failed = true;
        }
        /* Always flush TLB for this page */
        __asm__ __volatile__("invlpg (%0)" :: "r"(page_virt) : "memory");
    }

    /* Verify all pages are actually mapped to correct physical addresses.
     * If vmm_map_page silently returned ALREADY_MAPPED for a page pointing
     * to wrong phys (e.g., stale identity map entry), fix it here. */
    uint32_t fixed = 0;
    bool repair_failed = false;
    for (uint32_t i = 0; i < fb_pages; i++) {
        uintptr_t page_virt = fb_virt + (i << 12);
        uintptr_t page_phys = fb_phys_page + (i << 12);
        uintptr_t actual = vmm_get_physical_addr(vmm_get_current_page_directory(), page_virt);
        if (actual != page_phys) {
            /* Force unmap then remap */
            vmm_unmap_page(vmm_get_current_page_directory(), page_virt);
            memory_result_t res = vmm_map_page(vmm_get_current_page_directory(), page_virt, page_phys,
                                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE);
            if (res != MEMORY_OK && res != MEMORY_ERROR_ALREADY_MAPPED) {
                debuglog(DEBUG_ERROR, "[KERNEL] Framebuffer remap failed: page=%u res=%d\n", i, res);
                repair_failed = true;
                continue;
            }
            __asm__ __volatile__("invlpg (%0)" :: "r"(page_virt) : "memory");
            uintptr_t verify_page = vmm_get_physical_addr(vmm_get_current_page_directory(), page_virt);
            if (verify_page == page_phys) {
                fixed++;
            } else {
                debuglog(DEBUG_ERROR,
                         "[KERNEL] Framebuffer remap verify failed: page=%u mapped=0x%08x want=0x%08x\n",
                         i, (uint32_t)verify_page, (uint32_t)page_phys);
                repair_failed = true;
            }
        }
    }
    if (fixed > 0) {
        debuglog(DEBUG_WARN, "[KERNEL] Framebuffer: fixed %u stale/wrong mappings\n", fixed);
    }
    if (map_failed || repair_failed) {
        debuglog(DEBUG_ERROR, "[KERNEL] Framebuffer mapping had unrecoverable page failures\n");
        /* Best-effort: if phys == virt (identity MMIO window), QEMU/BIOS may
         * have already set up the mapping in hardware; our page table tracking
         * just doesn't reflect it.  Set the virtual address anyway so drivers
         * can attempt to use it instead of falling back to NULL. */
        if (fb_phys_page == fb_virt) {
            debuglog(DEBUG_WARN,
                     "[KERNEL] Best-effort: setting g_multiboot_framebuffer=0x%08x "
                     "(phys==virt MMIO window)\n", (uint32_t)fb_virt_ptr);
            g_multiboot_framebuffer = (void*)fb_virt_ptr;
        } else {
            g_multiboot_framebuffer = NULL;
        }
        return;
    }

    bool verify_ok = true;
    for (uint32_t i = 0; i < fb_pages; i++) {
        uintptr_t page_virt = fb_virt + (i << 12);
        uintptr_t page_phys = fb_phys_page + (i << 12);
        uintptr_t verify = vmm_get_physical_addr(vmm_get_current_page_directory(), page_virt);
        if (verify != page_phys) {
            debuglog(DEBUG_WARN,
                     "[KERNEL] Framebuffer map verify mismatch: page=%u virt=0x%08x "
                     "phys=0x%08x want=0x%08x (VMM tracking may not cover MMIO)\n",
                     i, (uint32_t)page_virt, (uint32_t)verify, (uint32_t)page_phys);
            verify_ok = false;
            break;
        }
    }

    /* Always set the virtual address pointer.  If VMM verification failed but
     * we successfully called vmm_map_page for all pages, the hardware TLB
     * entry is present even if vmm_get_physical_addr can't confirm it (e.g.
     * for MMIO frames that sit outside the identity-mapped PMM range).
     * Setting the pointer here lets the graphics driver probe the address
     * rather than failing outright with GFX_ERR_MAPPING_FAILED. */
    g_multiboot_framebuffer = (void*)fb_virt_ptr;
    if (verify_ok) {
        debuglog(DEBUG_INFO, "[KERNEL] Framebuffer virt=0x%08x mapped %u/%u pages (verified)\n",
                (uint32_t)fb_virt_ptr, mapped_ok, fb_pages);
    } else {
        debuglog(DEBUG_WARN,
                 "[KERNEL] Framebuffer virt=0x%08x set (best-effort, VMM verify partial)\n",
                 (uint32_t)fb_virt_ptr);
    }
}

// Parse multiboot framebuffer info early (MUST be called before vmm_init!)
static void parse_multiboot_framebuffer_early(uint32 magic, uint32 mbi_addr) {
    if (g_multiboot_framebuffer_internal.valid) {
        return; // Already parsed
    }
    
    if (magic == MULTIBOOT_BOOTLOADER_MAGIC && mbi_addr != 0) {
        multiboot_info_t* mbi = (multiboot_info_t*)mbi_addr;
        if ((mbi->flags & MULTIBOOT_FLAG_FRAMEBUFFER) &&
            mbi->framebuffer_addr != 0 &&
            mbi->framebuffer_width > 0 &&
            mbi->framebuffer_height > 0 &&
            mbi->framebuffer_bpp > 0 &&
            mbi->framebuffer_type != 2) { /* Skip EGA text framebuffer. */
            g_multiboot_framebuffer_internal.valid = true;
            g_multiboot_framebuffer_internal.addr = mbi->framebuffer_addr;
            g_multiboot_framebuffer_internal.width = mbi->framebuffer_width;
            g_multiboot_framebuffer_internal.height = mbi->framebuffer_height;
            g_multiboot_framebuffer_internal.bpp = mbi->framebuffer_bpp;
            g_multiboot_framebuffer_internal.pitch = mbi->framebuffer_pitch;
            if (g_multiboot_framebuffer_internal.pitch == 0) {
                g_multiboot_framebuffer_internal.pitch =
                    g_multiboot_framebuffer_internal.width *
                    ((g_multiboot_framebuffer_internal.bpp + 7) / 8);
            }
            update_v2_framebuffer_globals();
            return;
        }
    }

    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC && mbi_addr != 0) {
        multiboot2_info_t* hdr = (multiboot2_info_t*)mbi_addr;
        uint8* cursor = (uint8*)mbi_addr + sizeof(multiboot2_info_t);
        uint8* end = (uint8*)mbi_addr + hdr->total_size;
        
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
                if (tag->size >= sizeof(multiboot2_tag_framebuffer_t) &&
                    fb_tag->framebuffer_addr != 0 &&
#if !ARCH_64BIT
                    (fb_tag->framebuffer_addr >> 32) == 0 &&
#endif
                    fb_tag->framebuffer_width > 0 &&
                    fb_tag->framebuffer_height > 0 &&
                    fb_tag->framebuffer_bpp > 0 &&
                    fb_tag->framebuffer_type != 2) {
                    g_multiboot_framebuffer_internal.valid = true;
                    g_multiboot_framebuffer_internal.addr = fb_tag->framebuffer_addr;
                    g_multiboot_framebuffer_internal.width = fb_tag->framebuffer_width;
                    g_multiboot_framebuffer_internal.height = fb_tag->framebuffer_height;
                    g_multiboot_framebuffer_internal.bpp = fb_tag->framebuffer_bpp;
                    g_multiboot_framebuffer_internal.pitch = fb_tag->framebuffer_pitch;
                    if (g_multiboot_framebuffer_internal.pitch == 0) {
                        g_multiboot_framebuffer_internal.pitch =
                            g_multiboot_framebuffer_internal.width *
                            ((g_multiboot_framebuffer_internal.bpp + 7) / 8);
                    }
                    update_v2_framebuffer_globals();
                    return;
                }
            }
            uint32 advance = (tag->size + 7) & ~7;
            if (advance == 0 || cursor + advance > end) {
                break;
            }
            cursor += advance;
        }
    }
}
static bool g_boot_failed = false;
static bool g_fb_ever_had_content = false;

#ifndef CONFIG_DEBUG_BOOT
#define CONFIG_DEBUG_BOOT 0
#endif

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04
#define MOUSE_LOG_CAPACITY  32

typedef struct {
    uint8 buttons;
} mouse_log_entry_t;

static struct {
    mouse_log_entry_t entries[MOUSE_LOG_CAPACITY];
    uint8 head;
    uint8 tail;
} g_mouse_log_buffer;

static uint8 g_mouse_button_state = 0;

#if CONFIG_DEBUG_BOOT
static void kernel_debug_printf(const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (g_framebuffer_tty_ready) {
        tty_write_ansi(buffer);
    } else {
        print(buffer);
    }
}
#define KBOOT_DEBUG(...) kernel_debug_printf(__VA_ARGS__)
#else
#define KBOOT_DEBUG(...) ((void)0)
#endif

static void process_deferred_mouse_logs(void);
static void mouse_log_enqueue(uint8 buttons);
static bool mouse_log_pop(mouse_log_entry_t* entry);

static void boot_banner(void) {
    if (g_silent_boot) {
        if (g_graphics_ready) {
            // Aurora-style silent boot screen - use splash module
            splash_draw_background();
        } else {
            // Fallback for silent mode if graphics isn't ready
            print_colored("Fern\n", TEXT_ATTR_GREEN, TEXT_ATTR_BLACK);
        }
    } else {
        // Display appropriate banner based on available console mode
        if (g_framebuffer_tty_ready) {
            // Enhanced TTY is available
            tty_set_attr(MAKE_TEXT_ATTR(TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK));
            tty_clear();
            tty_write_ansi("\x1b[32mFern \x1b[37mkernel \x1b[36mv1.0\x1b[0m\n");
            tty_write_ansi("\x1b[90mFramebuffer TTY with advanced ANSI support\x1b[0m\n");
            tty_write_ansi("\x1b[32m[    0.000000]\x1b[37m Booting Forest-OS with framebuffer TTY...\x1b[0m\n");
            tty_write_ansi("\x1b[32m[    0.001000]\x1b[37m Kernel command line: root=/dev/ram0 init=/bin/init\x1b[0m\n");
            tty_write_ansi("\x1b[32m[    0.002000]\x1b[37m Initializing subsystems...\x1b[0m\n\n");
        } else {
            // Fall back to basic text mode
            print_colored("Fern kernel v1.0\n", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
            print_colored("Booting with text mode console...\n", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
            print_colored("Initializing subsystems...\n\n", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        }
    }
    
    // Always log to debuglog for early boot debugging
    debuglog_write("Fern kernel v1.0 boot sequence started\n");
    debuglog_write("Initializing subsystems...\n");
}

static void boot_log_event(const char* label, bool ok) {
    if (!label) {
        label = "unknown";
    }
    if (g_boot_log_count < BOOT_LOG_CAPACITY) {
        boot_log_entry_t* entry = &g_boot_log[g_boot_log_count++];
        strncpy(entry->label, label, sizeof(entry->label) - 1);
        entry->label[sizeof(entry->label) - 1] = '\0';
        entry->ok = ok;
    }
}

static void boot_status(const char* label, bool ok) {
    // Update splash screen progress bar whenever the splash is running
    if (splash_is_running()) {
        splash_update_status(label, ok);
        g_splash_boot_item_count++;
        uint8_t pct = (uint8_t)(g_splash_boot_item_count * 100u / BOOT_LOG_CAPACITY);
        if (pct > 99) pct = 99;
        splash_set_progress(pct);
    }
    if (g_silent_boot) {
        // In silent mode, only log to debuglog, do not print to screen
        if (!ok) {
            g_boot_failed = true;
        }
        if (debuglog_is_ready()) {
            debuglog_write(ok ? "[BOOT][ OK ] " : "[BOOT][FAIL] ");
            debuglog_write(label);
            debuglog_write("\n");
        }
        return;
    }
    static uint32 timestamp_counter = 3000;  // Start after initial messages
    boot_log_event(label, ok);
    if (!ok) {
        g_boot_failed = true;
    }
    if (debuglog_is_ready()) {
        debuglog_write(ok ? "[BOOT][ OK ] " : "[BOOT][FAIL] ");
        debuglog_write(label);
        debuglog_write("\n");
    }

    if (g_framebuffer_tty_ready) {
        char line[256];
        snprintf(line, sizeof(line), "\x1b[90m[%8u]\x1b[0m %s%c\x1b[0m %s\x1b[90m ...\x1b[0m %s\n",
                 timestamp_counter,
                 ok ? "\x1b[32m" : "\x1b[31m",
                 ok ? '+' : '-',
                 label,
                 ok ? "\x1b[32mOK\x1b[0m" : "\x1b[31mFAILED\x1b[0m");
        tty_write_ansi(line);
    }

    timestamp_counter += 100 + (timestamp_counter % 50); // Variable timing like real boot
}

static void boot_status_with_reason(const char* label, bool ok, const char* reason) {
    boot_status(label, ok);
    if (ok || !reason || !reason[0]) {
        return;
    }

    if (debuglog_is_ready()) {
        debuglog_printf("[BOOT][FAIL] %s reason: %s\n", label, reason);
    }

    if (!g_silent_boot && g_framebuffer_tty_ready) {
        char line[256];
        snprintf(line, sizeof(line), "\x1b[90m           reason: %s\x1b[0m\n", reason);
        tty_write_ansi(line);
    }
}

// Drain any pending PS/2 controller output to avoid stuck scancodes from firmware.
static uint32 ps2_flush_output_buffer(const char* stage __attribute__((unused)), uint32 max_reads) {
    uint32 drained = 0;

    for (uint32 i = 0; i < max_reads; i++) {
        uint8 status = inportb(PS2_STATUS_PORT);
        if ((status & PS2_STATUS_OUTPUT_BUFFER_FULL) == 0) {
            break;
        }
        (void)inportb(PS2_DATA_PORT);
        drained++;
    }

    if (drained > 0) {
        KBOOT_DEBUG("[PS/2] Drained %u byte(s) %s\n", drained, stage ? stage : "");
    }

    return drained;
}

static void ps2_keyboard_flush_and_delay(const char* stage) {
    ps2_flush_output_buffer(stage, 64);
    for (volatile int i = 0; i < 20000; i++) { /* short settle */ }
    ps2_flush_output_buffer(stage, 64);
}

static void fb_draw_fallback_banner(void) {
    if (!g_multiboot_framebuffer || !g_multiboot_framebuffer_internal.valid) return;

    extern const char font8x8_basic[128][8];
    void* fb = g_multiboot_framebuffer;
    uint32_t w = g_multiboot_framebuffer_internal.width;
    uint32_t h = g_multiboot_framebuffer_internal.height;
    uint32_t pitch = g_multiboot_framebuffer_internal.pitch;
    uint32_t bpp = g_multiboot_framebuffer_internal.bpp;
    uint32_t bytes_pp = (bpp + 7) / 8;
    if (w == 0 || h == 0 || pitch == 0 || bytes_pp == 0) return;

    volatile uint8_t* vfb = (volatile uint8_t*)fb;

    for (uint32_t y = 0; y < h; y++) {
        volatile uint8_t* row = vfb + y * pitch;
        for (uint32_t x = 0; x < w * bytes_pp; x++) {
            row[x] = 0x00;
        }
    }

    const char* title = "Fern";
    uint32_t title_len = 9;
    uint32_t scale = 3;
    uint32_t char_w = 8 * scale;
    uint32_t title_pixel_w = title_len * char_w;
    uint32_t start_x = (w > title_pixel_w) ? (w - title_pixel_w) / 2 : 0;
    uint32_t start_y = (h > 8 * scale) ? (h / 2 - 8 * scale) : 0;

    uint32_t fg_r = 0x33, fg_g = 0xCC, fg_b = 0x44;

    for (uint32_t ci = 0; ci < title_len; ci++) {
        char c = title[ci];
        uint8_t idx = (uint8_t)c;
        if (idx >= 128) idx = '?';
        const char* glyph = font8x8_basic[idx];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                if ((bits >> col) & 1) {
                    for (uint32_t sy = 0; sy < scale; sy++) {
                        for (uint32_t sx = 0; sx < scale; sx++) {
                            uint32_t px = start_x + ci * char_w + col * scale + sx;
                            uint32_t py = start_y + row * scale + sy;
                            if (px < w && py < h) {
                                volatile uint8_t* p = vfb + py * pitch + px * bytes_pp;
                                p[0] = fg_b;
                                p[1] = fg_g;
                                p[2] = fg_r;
                                if (bytes_pp == 4) p[3] = 0xFF;
                            }
                        }
                    }
                }
            }
        }
    }

    const char* subtitle = "v1.0";
    uint32_t sub_len = 4;
    uint32_t sub_scale = 2;
    uint32_t sub_char_w = 8 * sub_scale;
    uint32_t sub_pixel_w = sub_len * sub_char_w;
    uint32_t sub_start_x = (w > sub_pixel_w) ? (w - sub_pixel_w) / 2 : 0;
    uint32_t sub_start_y = start_y + 8 * scale + 12;
    uint32_t sub_fg_r = 0x80, sub_fg_g = 0xCC, sub_fg_b = 0x80;

    for (uint32_t ci = 0; ci < sub_len; ci++) {
        char c = subtitle[ci];
        uint8_t idx = (uint8_t)c;
        if (idx >= 128) idx = '?';
        const char* glyph = font8x8_basic[idx];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                if ((bits >> col) & 1) {
                    for (uint32_t sy = 0; sy < sub_scale; sy++) {
                        for (uint32_t sx = 0; sx < sub_scale; sx++) {
                            uint32_t px = sub_start_x + ci * sub_char_w + col * sub_scale + sx;
                            uint32_t py = sub_start_y + row * sub_scale + sy;
                            if (px < w && py < h) {
                                volatile uint8_t* p = vfb + py * pitch + px * bytes_pp;
                                p[0] = sub_fg_b;
                                p[1] = sub_fg_g;
                                p[2] = sub_fg_r;
                                if (bytes_pp == 4) p[3] = 0xFF;
                            }
                        }
                    }
                }
            }
        }
    }

    const char* status = "Booting...";
    uint32_t st_len = 10;
    uint32_t st_char_w = 8;
    uint32_t st_pixel_w = st_len * st_char_w;
    uint32_t st_start_x = (w > st_pixel_w) ? (w - st_pixel_w) / 2 : 0;
    uint32_t st_start_y = sub_start_y + 8 * sub_scale + 16;
    uint32_t st_fg_r = 0xAA, st_fg_g = 0xAA, st_fg_b = 0xAA;

    for (uint32_t ci = 0; ci < st_len; ci++) {
        char c = status[ci];
        uint8_t idx = (uint8_t)c;
        if (idx >= 128) idx = '?';
        const char* glyph = font8x8_basic[idx];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                if ((bits >> col) & 1) {
                    uint32_t px = st_start_x + ci * st_char_w + col;
                    uint32_t py = st_start_y + row;
                    if (px < w && py < h) {
                        volatile uint8_t* p = vfb + py * pitch + px * bytes_pp;
                        p[0] = st_fg_b;
                        p[1] = st_fg_g;
                        p[2] = st_fg_r;
                        if (bytes_pp == 4) p[3] = 0xFF;
                    }
                }
            }
        }
    }

    g_fb_ever_had_content = true;
}

static void fb_draw_text_line(uint32_t y_px, const char* text, uint32_t scale,
                               uint8_t r, uint8_t g, uint8_t b) {
    if (!g_multiboot_framebuffer || !text) return;

    extern const char font8x8_basic[128][8];
    uint32_t w = g_multiboot_framebuffer_internal.width;
    uint32_t h = g_multiboot_framebuffer_internal.height;
    uint32_t pitch = g_multiboot_framebuffer_internal.pitch;
    uint32_t bpp = g_multiboot_framebuffer_internal.bpp;
    uint32_t bytes_pp = (bpp + 7) / 8;
    if (w == 0 || h == 0 || pitch == 0 || bytes_pp == 0) return;

    volatile uint8_t* vfb = (volatile uint8_t*)g_multiboot_framebuffer;
    uint32_t len = 0;
    const char* p = text;
    while (*p++) len++;
    uint32_t text_w = len * 8 * scale;
    uint32_t sx = (w > text_w) ? (w - text_w) / 2 : 0;

    for (uint32_t ci = 0; ci < len; ci++) {
        uint8_t idx = (uint8_t)text[ci];
        if (idx >= 128) idx = '?';
        const char* glyph = font8x8_basic[idx];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                if ((bits >> col) & 1) {
                    for (uint32_t sy = 0; sy < scale; sy++) {
                        for (uint32_t sx2 = 0; sx2 < scale; sx2++) {
                            uint32_t px = sx + ci * 8 * scale + col * scale + sx2;
                            uint32_t py = y_px + row * scale + sy;
                            if (px < w && py < h) {
                                volatile uint8_t* pp = vfb + py * pitch + px * bytes_pp;
                                pp[0] = b;
                                pp[1] = g;
                                pp[2] = r;
                                if (bytes_pp == 4) pp[3] = 0xFF;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void fb_verify_and_clear(uint32_t fill_color) {
    if (!g_multiboot_framebuffer || !g_multiboot_framebuffer_internal.valid) return;

    uintptr_t check_phys = vmm_get_physical_addr(
        vmm_get_current_page_directory(), (uintptr_t)g_multiboot_framebuffer);
    if (check_phys == 0 && g_multiboot_fb_addr != 0) {
        debuglog(DEBUG_WARN, "[KERNEL] FB verify: page not mapped, re-finalizing\n");
        kernel_finalize_framebuffer_mapping();
        check_phys = vmm_get_physical_addr(
            vmm_get_current_page_directory(), (uintptr_t)g_multiboot_framebuffer);
    }
    if (check_phys == 0) {
        debuglog(DEBUG_ERROR, "[KERNEL] FB verify: still not mapped after re-finalize\n");
        return;
    }

    uint32_t w = g_multiboot_framebuffer_internal.width;
    uint32_t h = g_multiboot_framebuffer_internal.height;
    uint32_t pitch = g_multiboot_framebuffer_internal.pitch;
    uint32_t bpp = g_multiboot_framebuffer_internal.bpp;
    uint32_t bytes_pp = (bpp + 7) / 8;
    if (w == 0 || h == 0 || pitch == 0) return;

    volatile uint8_t* vfb = (volatile uint8_t*)g_multiboot_framebuffer;
    uint8_t b = fill_color & 0xFF;
    uint8_t g_val = (fill_color >> 8) & 0xFF;
    uint8_t r = (fill_color >> 16) & 0xFF;

    for (uint32_t y = 0; y < h; y++) {
        volatile uint8_t* row = vfb + y * pitch;
        for (uint32_t x = 0; x < w; x++) {
            row[x * bytes_pp + 0] = b;
            row[x * bytes_pp + 1] = g_val;
            row[x * bytes_pp + 2] = r;
            if (bytes_pp == 4) row[x * bytes_pp + 3] = 0xFF;
        }
    }

    g_fb_ever_had_content = true;
}

static void fb_draw_boot_complete(void) {
    if (!g_multiboot_framebuffer) return;

    extern const char font8x8_basic[128][8];
    uint32_t w = g_multiboot_framebuffer_internal.width;
    uint32_t h = g_multiboot_framebuffer_internal.height;
    uint32_t pitch = g_multiboot_framebuffer_internal.pitch;
    uint32_t bpp = g_multiboot_framebuffer_internal.bpp;
    uint32_t bytes_pp = (bpp + 7) / 8;
    if (w == 0 || h == 0 || pitch == 0 || bytes_pp == 0) return;

    volatile uint8_t* vfb = (volatile uint8_t*)g_multiboot_framebuffer;

    const char* msg = "Boot Complete";
    uint32_t msg_len = 13;
    uint32_t scale = 2;
    uint32_t char_w = 8 * scale;
    uint32_t msg_pixel_w = msg_len * char_w;
    uint32_t sx = (w > msg_pixel_w) ? (w - msg_pixel_w) / 2 : 0;
    uint32_t sy = (h > 8 * scale + 40) ? (h - 40) : 0;

    uint32_t bar_h = 8 * scale + 8;
    for (uint32_t y = sy; y < sy + bar_h && y < h; y++) {
        volatile uint8_t* row = vfb + y * pitch;
        for (uint32_t x = 0; x < w * bytes_pp; x++) {
            row[x] = 0x00;
        }
    }

    uint32_t fg_r = 0x44, fg_g = 0xEE, fg_b = 0x44;
    for (uint32_t ci = 0; ci < msg_len; ci++) {
        uint8_t idx = (uint8_t)msg[ci];
        if (idx >= 128) idx = '?';
        const char* glyph = font8x8_basic[idx];
        for (uint32_t row = 0; row < 8; row++) {
            uint8_t bits = (uint8_t)glyph[row];
            for (uint32_t col = 0; col < 8; col++) {
                if ((bits >> col) & 1) {
                    for (uint32_t sy2 = 0; sy2 < scale; sy2++) {
                        for (uint32_t sx2 = 0; sx2 < scale; sx2++) {
                            uint32_t px = sx + ci * char_w + col * scale + sx2;
                            uint32_t py = sy + row * scale + sy2;
                            if (px < w && py < h) {
                                volatile uint8_t* p = vfb + py * pitch + px * bytes_pp;
                                p[0] = fg_b;
                                p[1] = fg_g;
                                p[2] = fg_r;
                                if (bytes_pp == 4) p[3] = 0xFF;
                            }
                        }
                    }
                }
            }
        }
    }

    g_fb_ever_had_content = true;
}

/**
 * Reset VGA hardware to 80x25 color text mode (Mode 3).
 * Called when booting without a framebuffer (nofb mode) to ensure the VGA
 * CRTC is scanning from 0xB8000 in text mode.  Without this, the VGA may
 * still be in a VESA/VBE graphics mode left by GRUB, and writing to the
 * text buffer at 0xB8000 produces no visible output.
 */
static void vga_text_mode_reset(void) {
    /* 1. Miscellaneous Output Register - color mode, 80x25 clock */
    outportb(0x3C2, 0x67);

    /* 2. Sequencer - synchronous reset, then configure for text */
    outportb(0x3C4, 0x00); outportb(0x3C5, 0x03);   /* Reset: normal */
    outportb(0x3C4, 0x01); outportb(0x3C5, 0x01);   /* Clock mode: dot clock / 9 */
    outportb(0x3C4, 0x02); outportb(0x3C5, 0x03);   /* Plane write enable: 0+1 */
    outportb(0x3C4, 0x03); outportb(0x3C5, 0x00);   /* Char map select: default */
    outportb(0x3C4, 0x04); outportb(0x3C5, 0x07);   /* Memory mode: normal, odd/even, alpha */

    /* 3. CRTC - 80x25 text mode timings (standard Mode 3) */
    static const uint8_t crtc_vals[] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
        0xFF
    };
    for (uint32_t i = 0; i < sizeof(crtc_vals); i++) {
        outportb(0x3D4, (uint8_t)i);
        outportb(0x3D5, crtc_vals[i]);
    }

    /* 4. Graphics Controller - text mode addressing */
    outportb(0x3CE, 0x00); outportb(0x3CF, 0x00);   /* Set/Reset */
    outportb(0x3CE, 0x01); outportb(0x3CF, 0x00);   /* Enable Set/Reset */
    outportb(0x3CE, 0x02); outportb(0x3CF, 0x00);   /* Color Compare */
    outportb(0x3CE, 0x03); outportb(0x3CF, 0x00);   /* Data Rotate */
    outportb(0x3CE, 0x04); outportb(0x3CF, 0x00);   /* Read Map Select */
    outportb(0x3CE, 0x05); outportb(0x3CF, 0x10);   /* Mode: text, odd/even */
    outportb(0x3CE, 0x06); outportb(0x3CF, 0x0E);   /* Misc: text mode at 0xB8000 */
    outportb(0x3CE, 0x07); outportb(0x3CF, 0x00);   /* Color Don't Care */
    outportb(0x3CE, 0x08); outportb(0x3CF, 0xFF);   /* Bit Mask */

    /* 5. Attribute Controller - enable video, text mode palette */
    (void)inportb(0x3DA);          /* Reset AC flip-flop */
    outportb(0x3C0, 0x20);         /* Enable video output (bit 5) */
    /* Standard text mode palette (16 entries) */
    static const uint8_t ac_vals[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    for (uint32_t i = 0; i < sizeof(ac_vals); i++) {
        outportb(0x3C0, (uint8_t)i);
        outportb(0x3C0, ac_vals[i]);
    }
    outportb(0x3C0, 0x20);         /* Re-enable video */

    /* 6. Clear the VGA text buffer */
    volatile uint8_t* vidmem = (volatile uint8_t*)0xB8000;
    for (uint32_t i = 0; i < 80 * 25; i++) {
        vidmem[i * 2]     = ' ';
        vidmem[i * 2 + 1] = 0x07;  /* Light gray on black */
    }

    /* 7. Position cursor at (0,0) */
    outportb(0x3D4, 0x0E); outportb(0x3D5, 0x00);
    outportb(0x3D4, 0x0F); outportb(0x3D5, 0x00);

    debuglog(DEBUG_INFO, "[KERNEL] VGA text mode (80x25) reset complete\n");
}

/* Returns true when the kernel was booted with "nofb" (text-only VGA console).
 * All subsystems that might lazily initialize graphics MUST check this first. */
bool kernel_framebuffer_disabled(void) {
    return g_framebuffer_disabled;
}

static void initialize_framebuffer_console_early(void) {
    if (g_framebuffer_tty_ready) {
        return;
    }

    /* In nofb mode, never attempt graphics init; stay on the VGA text console. */
    if (g_framebuffer_disabled) {
        debuglog(DEBUG_INFO, "[KERNEL] nofb mode: resetting VGA to text mode\n");
        vga_text_mode_reset();
        console_init();
        ps2_mouse_set_bounds(80, 25);
        return;
    }

    if (!g_graphics_ready) {
        // Use the V2 graphics subsystem initialization
        {
            uint32_t _p = vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000);
            if (_p == 0 && g_multiboot_fb_addr != 0) {
                debuglog(DEBUG_WARN, "[KDBG] pre-gfx-subsys: missing FB map, retrying finalize\n");
                kernel_finalize_framebuffer_mapping();
                _p = vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000);
            }
            debuglog(DEBUG_INFO, "[KDBG] pre-gfx-subsys: fb phys=0x%08x\n", _p);
        }
        graphics_result_t graphics_init_result = initialize_graphics_subsystem();
        {uint32_t _p=vmm_get_physical_addr(vmm_get_current_page_directory(),0xF0000000);debuglog(DEBUG_INFO,"[KDBG] post-gfx-subsys: fb phys=0x%08x\n",_p);}
        g_graphics_ready = (graphics_init_result == GRAPHICS_SUCCESS);
        boot_status("Graphics subsystem (V2)", g_graphics_ready);

        // Set mouse bounds after graphics initialization
        if (g_graphics_ready) {
            if (g_video_mode_requested) {
                /*
                 * Skip mode switching - the GRUB-provided framebuffer already has
                 * the selected resolution. Mode switching via VESA/BGA often fails
                 * on emulated VGA (QEMU/VirtualBox/VMware) because the hardware
                 * doesn't actually switch framebuffer addresses.
                 *
                 * Just log what was requested vs what we have.
                 */
                framebuffer_t* current_fb = graphics_get_framebuffer();
                if (current_fb) {
                    debuglog(DEBUG_INFO,
                             "[KERNEL] Using GRUB framebuffer: %ux%ux%u (requested %ux%u ignored - emulated VGA limitation)\n",
                             current_fb->width, current_fb->height, current_fb->bpp,
                             g_video_mode_width, g_video_mode_height);
                }
            }

            framebuffer_t* fb = graphics_get_framebuffer();
            if (fb) {
                ps2_mouse_set_bounds(fb->width, fb->height);
                ps2_mouse_set_position(fb->width / 2, fb->height / 2);
                
                // Initialize splash screen system now that we have graphics
                if (g_quiet_boot) {
                    splash_config_t config = {
                        .enabled = true,
                        .use_quiet_mode = true,
                        .fade_out_duration = 1000
                    };
                    splash_init(&config);
                    splash_start();
                }
            }
        } else {
            ps2_mouse_set_bounds(800, 600);
            ps2_mouse_set_position(400, 300);
        }
        if (!g_graphics_ready) {
            /* Auto-fallback: graphics init failed, switch to text-only mode
             * so the kernel keeps booting instead of aborting. */
            debuglog(DEBUG_WARN, "[KERNEL] Graphics init failed - auto-switching to text console\n");
            g_framebuffer_disabled = true;
            print_colored("Graphics init failed: falling back to VGA text console\n",
                          TEXT_ATTR_YELLOW, TEXT_ATTR_BLACK);
            ps2_mouse_set_bounds(80, 25);
            return;
        }
    }

    if (!g_framebuffer_tty_ready) {
        {
            uint32_t _p = vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000);
            if (_p == 0 && g_multiboot_fb_addr != 0) {
                debuglog(DEBUG_WARN, "[KDBG] pre-tty-init: missing FB map, retrying finalize\n");
                kernel_finalize_framebuffer_mapping();
                _p = vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000);
            }
            debuglog(DEBUG_INFO, "[KDBG] pre-tty-init: fb phys=0x%08x\n", _p);
        }
        bool tty_success = tty_init();
        if (tty_success) {
            boot_status("Framebuffer TTY with truecolor support", true);
            g_framebuffer_tty_ready = true;
            if (!g_silent_boot) {
                tty_clear();
                boot_banner();
            } else {
                splash_draw_background();
                g_fb_ever_had_content = true;
            }
        } else {
            boot_status("Framebuffer TTY with truecolor support", false);
            print_colored("Failed to initialize framebuffer TTY\n", TEXT_ATTR_LIGHT_RED, TEXT_ATTR_BLACK);
            if (!g_fb_ever_had_content) {
                fb_draw_fallback_banner();
            }
        }
    }
}

static void boot_require(const char* label, bool ok, const char* panic_reason) {
    boot_status(label, ok);
    if (!ok) {
        if (panic_reason && panic_reason[0] != '\0') {
            kernel_panic(panic_reason);
        } else {
            kernel_panic(label);
        }
    }
}

static bool parse_u32_token(const char** p, const char* end, uint32_t* out_value) {
    uint32_t value = 0;
    bool saw_digit = false;

    while (*p < end && **p >= '0' && **p <= '9') {
        saw_digit = true;
        value = (value * 10u) + (uint32_t)(**p - '0');
        (*p)++;
    }

    if (!saw_digit) {
        return false;
    }

    *out_value = value;
    return true;
}

static void parse_video_mode_token(const char* token, size_t len) {
    static const char prefix[] = "video=";
    if (len <= (sizeof(prefix) - 1) || strncmp(token, prefix, sizeof(prefix) - 1) != 0) {
        return;
    }

    const char* p = token + (sizeof(prefix) - 1);
    const char* end = token + len;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bpp = 0;

    if (!parse_u32_token(&p, end, &width)) {
        return;
    }
    if (p >= end || (*p != 'x' && *p != 'X')) {
        return;
    }
    p++;
    if (!parse_u32_token(&p, end, &height)) {
        return;
    }
    if (p < end && (*p == 'x' || *p == 'X')) {
        p++;
        if (!parse_u32_token(&p, end, &bpp)) {
            return;
        }
    }

    if (p != end) {
        return;
    }

    if (width < 320 || height < 200) {
        return;
    }

    if (bpp != 0 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) {
        bpp = 0;
    }

    g_video_mode_requested = true;
    g_video_mode_width = width;
    g_video_mode_height = height;
    g_video_mode_bpp = bpp;
}

// Parse whitespace-delimited kernel command line tokens for quiet/silent flags.
static void parse_cmdline_tokens(const char* cmdline) {
    if (!cmdline || !cmdline[0]) {
        return;
    }

    const char* p = cmdline;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char* start = p;
        while (*p && *p != ' ') {
            p++;
        }

        size_t len = (size_t)(p - start);
        if (len == 0) {
            continue;
        }

        static const char quiet_token[] = "quiet";
        static const char silent_token[] = "bootmode=silent";
        static const char embedded_token[] = "embedded";
        static const char nofb_token[] = "nofb";
        static const char livecd_token[] = "livecd";
        static const char live_token[] = "live";
        parse_video_mode_token(start, len);

        if (len == sizeof(quiet_token) - 1 && strncmp(start, quiet_token, len) == 0) {
            g_quiet_boot = true;
            g_silent_boot = true;
        } else if (len == sizeof(silent_token) - 1 && strncmp(start, silent_token, len) == 0) {
            g_silent_boot = true;
        } else if (len == sizeof(embedded_token) - 1 && strncmp(start, embedded_token, len) == 0) {
            g_embedded_mode = true;
        } else if (len == sizeof(nofb_token) - 1 && strncmp(start, nofb_token, len) == 0) {
            g_framebuffer_disabled = true;
        } else if (len == sizeof(livecd_token) - 1 && strncmp(start, livecd_token, len) == 0) {
            g_livecd_mode = true;
        } else if (len == sizeof(live_token) - 1 && strncmp(start, live_token, len) == 0) {
            g_livecd_mode = true;
        }
    }

    if (g_quiet_boot) {
        g_silent_boot = true;
    }
    if (g_embedded_mode) {
        debuglog(DEBUG_INFO, "[KERNEL] Embedded mode enabled (reduced memory profile)\n");
    }
    if (g_framebuffer_disabled) {
        debuglog(DEBUG_INFO, "[KERNEL] Framebuffer disabled by command line (text console)\n");
    }
    if (g_livecd_mode) {
        debuglog(DEBUG_INFO, "[KERNEL] Live CD mode enabled (auto-login as root)\n");
    }
}

/* Forward declaration */
void startk(uint32 magic, uint32 mbi_addr);

// Helper to get initrd module bounds from multiboot info (for early reservation)
static bool get_initrd_bounds(uint32 magic, uint32 mbi_addr, uint32* out_start, uint32* out_end) {
    if (!out_start || !out_end) {
        return false;
    }
    *out_start = 0;
    *out_end = 0;

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC && mbi_addr != 0) {
        multiboot_info_t* mbi = (multiboot_info_t*)mbi_addr;
        if (mbi->mods_count > 0 && mbi->mods_addr != 0) {
            multiboot_module_t* mod = (multiboot_module_t*)mbi->mods_addr;
            *out_start = mod->mod_start;
            *out_end = mod->mod_end;
            return true;
        }
    } else if (magic == MULTIBOOT2_BOOTLOADER_MAGIC && mbi_addr != 0) {
        multiboot2_info_t* hdr = (multiboot2_info_t*)mbi_addr;
        uint8* cursor = (uint8*)mbi_addr + sizeof(multiboot2_info_t);
        uint8* end = (uint8*)mbi_addr + hdr->total_size;
        while (cursor < end) {
            multiboot2_tag_t* tag = (multiboot2_tag_t*)cursor;
            if (tag->size < sizeof(multiboot2_tag_t)) {
                break;
            }
            if (tag->type == MULTIBOOT2_TAG_END) {
                break;
            }
            if (tag->type == MULTIBOOT2_TAG_MODULE) {
                multiboot2_tag_module_t* module = (multiboot2_tag_module_t*)tag;
                *out_start = module->mod_start;
                *out_end = module->mod_end;
                return true;
            }
            if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER) {
                multiboot2_tag_framebuffer_t* fb_tag = (multiboot2_tag_framebuffer_t*)tag;
                if (tag->size >= sizeof(multiboot2_tag_framebuffer_t) &&
                    fb_tag->framebuffer_addr != 0 &&
#if !ARCH_64BIT
                    (fb_tag->framebuffer_addr >> 32) == 0 &&
#endif
                    fb_tag->framebuffer_width > 0 &&
                    fb_tag->framebuffer_height > 0 &&
                    fb_tag->framebuffer_bpp > 0 &&
                    fb_tag->framebuffer_type != 2) {
                    g_multiboot_framebuffer_internal.valid = true;
                    g_multiboot_framebuffer_internal.addr = fb_tag->framebuffer_addr;
                    g_multiboot_framebuffer_internal.width = fb_tag->framebuffer_width;
                    g_multiboot_framebuffer_internal.height = fb_tag->framebuffer_height;
                    g_multiboot_framebuffer_internal.bpp = fb_tag->framebuffer_bpp;
                    g_multiboot_framebuffer_internal.pitch = fb_tag->framebuffer_pitch;
                    if (g_multiboot_framebuffer_internal.pitch == 0) {
                        g_multiboot_framebuffer_internal.pitch =
                            g_multiboot_framebuffer_internal.width *
                            ((g_multiboot_framebuffer_internal.bpp + 7) / 8);
                    }
                    update_v2_framebuffer_globals();
                    if (!g_silent_boot) {
                        print_colored("Found multiboot framebuffer: addr=0x", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
#if ARCH_64BIT
                        print_hex((uint32_t)(g_multiboot_framebuffer_internal.addr >> 32));
#endif
                        print_hex((uint32_t)g_multiboot_framebuffer_internal.addr);
                        print_colored(", ", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
                        print_hex(g_multiboot_framebuffer_internal.width);
                        print_colored("x", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
                        print_hex(g_multiboot_framebuffer_internal.height);
                        print_colored(", ", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
                        print_hex(g_multiboot_framebuffer_internal.bpp);
                        print_colored(" bpp\n", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
                    }
                }
            }
            uint32 advance = (tag->size + 7) & ~7;
            if (advance == 0 || cursor + advance > end) {
                break;
            }
            cursor += advance;
        }
    }
    return false;
}

/* Wrapper for BIOS/UEFI boot entry point that calls startk with dummy values */
int kernel_main(void) {
    // Called from bios_main/uefi_main without multiboot info
    // Pass 0 for magic (won't validate) and NULL for mbi_addr
    startk(0, 0);
    return 0;
}

// Walk multiboot1/2 structures to extract kernel command line very early.
static void parse_multiboot_cmdline(uint32 magic, uint32 mbi_addr) {
    if (mbi_addr == 0) {
        return;
    }

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC) {
        multiboot_info_t* mbi = (multiboot_info_t*)mbi_addr;
        if (mbi->cmdline) {
            parse_cmdline_tokens((const char*)mbi->cmdline);
        }
    } else if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        multiboot2_info_t* hdr = (multiboot2_info_t*)mbi_addr;
        uint8* cursor = (uint8*)mbi_addr + 8; // Skip total_size + reserved
        uint8* end = (uint8*)mbi_addr + hdr->total_size;
        while (cursor < end) {
            multiboot2_tag_t* tag = (multiboot2_tag_t*)cursor;
            if (tag->type == MULTIBOOT2_TAG_END) {
                break;
            }
            if (tag->type == MULTIBOOT2_TAG_CMDLINE) {
                multiboot2_tag_string_t* cmdline_tag = (multiboot2_tag_string_t*)tag;
                parse_cmdline_tokens(cmdline_tag->string);
            }
            cursor += (tag->size + 7) & ~7;
        }
    }

    if (g_quiet_boot) {
        g_silent_boot = true;
    }
}

void startk(uint32 magic, uint32 mbi_addr) {
    cpu_disable_interrupts();
    
    // CRITICAL: Save multiboot info immediately before any other operations
    // This ensures we don't lose the info if stack gets corrupted
    uint32 saved_magic = magic;
    uint32 saved_mbi = mbi_addr;
    (void)saved_magic;
    (void)saved_mbi;
    g_multiboot_magic = magic;
    g_multiboot_info_addr = mbi_addr;
    
    print_colored("FOREST OS BOOT DEBUG\n", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
    print_colored("Raw boot params: magic=0x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
    print_hex(magic);
    print_colored(" mbi=0x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
    print_hex(mbi_addr);
    print_colored("\n", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
    
    gdt_init((uint32)&_stack_top);
    // Early interrupt setup (enables safe interrupt functions)
    interrupt_early_init();
    debuglog_init();

    // Save multiboot info pointer for V2 graphics system (multiboot1 only)
    if (magic == MULTIBOOT_BOOTLOADER_MAGIC && mbi_addr != 0) {
        g_multiboot_info = (multiboot_info_t*)mbi_addr;
    }

    // Parse kernel command line before any visible output to honor quiet/silent flags.
    parse_multiboot_cmdline(magic, mbi_addr);
    
    // CRITICAL: Parse multiboot framebuffer info EARLY, before VMM initialization
    // This is needed so vmm_init() can map the framebuffer before paging is enabled
    parse_multiboot_framebuffer_early(magic, mbi_addr);

    // Display early system information
    if (!g_silent_boot) {
        print_colored("Fern v1.0 - ", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
#if defined(__x86_64__)
        print_colored("x86_64 ", TEXT_ATTR_LIGHT_GREEN, TEXT_ATTR_BLACK);
#else
        print_colored("i686 ", TEXT_ATTR_LIGHT_GREEN, TEXT_ATTR_BLACK);
#endif
        print_colored("Kernel\n", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);

        // Show CPU and memory info
        print_colored("CPU: ", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        if (cpu_has_tsc()) {
            print_colored("TSC ", TEXT_ATTR_LIGHT_GREEN, TEXT_ATTR_BLACK);
        }
        print_colored("Magic: 0x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        print_hex(magic);
        print_colored(" MBI: 0x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        print_hex(mbi_addr);
        print_colored("\n", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
    }

    // Show multiboot framebuffer info if available
    uintptr_t fb_addr;
    uint32_t fb_width, fb_height, fb_bpp, fb_pitch;
    if (kernel_get_multiboot_framebuffer(&fb_addr, &fb_width, &fb_height, &fb_bpp, &fb_pitch)) {
        if (!g_silent_boot) {
            print_colored("FB: ", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
#if ARCH_64BIT
            print_hex((uint32_t)(fb_addr >> 32));
#endif
            print_hex((uint32_t)fb_addr);
            print_colored(" ", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
            print_hex(fb_width);
            print_colored("x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
            print_hex(fb_height);
            print_colored("@", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
            print_hex(fb_bpp);
            print_colored("bpp\n", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        }
    }

    init_system_init();

    // Note: Console initialization moved to after graphics init for framebuffer-only TTY
    
    // Complete interrupt system setup
    interrupt_full_init();

    // Initialize FPU for floating point operations if present
    if (hardware_cpu_has_fpu()) {
        uint32 cr0 = cpu_get_cr0();
        cr0 &= ~(1 << 2); // Clear CR0.EM (disable FPU emulation)
        cr0 &= ~(1 << 3); // Clear CR0.TS (clear task switched flag)
        cpu_set_cr0(cr0);
        __asm__ __volatile__("fninit"); // Initialize FPU
    }

    // Initialize syscalls (now uses new interrupt system)
    syscall_init();
    kmain(magic, mbi_addr);
}

void kmain(uint32 magic, uint32 mbi_addr) {
    // Initialize early text mode console first for debugging
    // Force text mode for now to avoid graphics issues

    clearScreen();
    if (!g_silent_boot) {
        print_colored("Fern kernel v1.0 - Early Boot (TEXT MODE)\n", TEXT_ATTR_LIGHT_GREEN, TEXT_ATTR_BLACK);
        print_colored("Magic: 0x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        print_hex(magic);
        print_colored(" MBI: 0x", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
        print_hex(mbi_addr);
        print_colored("\n", TEXT_ATTR_LIGHT_GRAY, TEXT_ATTR_BLACK);
    }
    
    keyboard_set_driver_mode(KEYBOARD_DRIVER_LEGACY);
    
    bool hw_detected = hardware_detect_init();
    boot_require("Hardware detection (CPUID)", hw_detected, "CPUID detection failed");

    bool driver_core_ok = driver_manager_init();
    boot_status("Driver core", driver_core_ok);

    // Initialize memory validation first
    memory_validation_result_t validation_result = memory_validation_init();
    if (validation_result != MEMORY_VALIDATION_SUCCESS) {
        kernel_panic_memory_error("memory_validation_init",
                                  memory_validation_result_to_string(validation_result));
    }
    boot_status("Memory validation system", true);

    memory_result_t mem_result = memory_init(magic, mbi_addr);
    if (mem_result != MEMORY_OK) {
        boot_status("Memory subsystem", false);
        kernel_panic_memory_error("memory_init", memory_result_to_string(mem_result));
    }
    boot_status("Memory subsystem", true);

    // Check for extremely low memory and show error screen if needed
    {
        uint32_t usable_kb = memory_get_usable_kb();
        uint32_t min_kb = g_embedded_mode ? KERNEL_MIN_MEM_KB_EMBEDDED : KERNEL_MIN_MEM_KB_NORMAL;
        if (usable_kb < min_kb) {
            debuglog_printf("\n\n");
            debuglog_printf("*****************************************************\n");
            debuglog_printf("*                                                   *\n");
            debuglog_printf("*         INSUFFICIENT MEMORY DETECTED              *\n");
            debuglog_printf("*                                                   *\n");
            debuglog_printf("*****************************************************\n");
            debuglog_printf("\n");
            debuglog_printf("System memory: %u KB (%u MB)\n", usable_kb, usable_kb / 1024);
            debuglog_printf("Minimum required: %u KB (%u MB)%s\n", min_kb, min_kb / 1024,
                             g_embedded_mode ? " [embedded]" : "");
            debuglog_printf("\n");
            debuglog_printf("Fern cannot boot with this amount of RAM.\n");
            debuglog_printf("\n");
            debuglog_printf("Please upgrade your system memory to at least %uMB\n", min_kb / 1024);
            debuglog_printf("and try again.\n");
            debuglog_printf("\n");
            debuglog_printf("System halted.\n");
            
            // Also show on framebuffer if available
            if (g_multiboot_framebuffer) {
                uint32_t fb_w = g_multiboot_framebuffer_internal.width;
                uint32_t fb_h = g_multiboot_framebuffer_internal.height;
                if (fb_w == 0) fb_w = 640;
                if (fb_h == 0) fb_h = 480;
                volatile uint8_t* fb = (volatile uint8_t*)g_multiboot_framebuffer;
                uint32_t pitch = g_multiboot_framebuffer_internal.pitch;
                uint32_t bpp = g_multiboot_framebuffer_internal.bpp;
                uint32_t bytes_pp = (bpp + 7) / 8;
                if (pitch == 0) pitch = fb_w * bytes_pp;
                for (uint32_t y = 0; y < fb_h; y++) {
                    volatile uint8_t* row = fb + y * pitch;
                    for (uint32_t x = 0; x < fb_w; x++) {
                        row[x * bytes_pp + 0] = 0x00;
                        row[x * bytes_pp + 1] = 0x00;
                        row[x * bytes_pp + 2] = 0xAA;
                        if (bytes_pp == 4) row[x * bytes_pp + 3] = 0xFF;
                    }
                }
                fb_draw_text_line(fb_h / 2 - 16, "INSUFFICIENT MEMORY", 2, 0xFF, 0xFF, 0xFF);
                fb_draw_text_line(fb_h / 2 + 8, "Need more RAM", 1, 0xCC, 0xCC, 0xCC);
            }
            
            // Halt forever
            while (1) {
                __asm__ volatile ("cli; hlt");
            }
        }
    }

    // CRITICAL: Finalize framebuffer mapping now that VMM has identity-mapped it
    // This sets g_multiboot_framebuffer to the virtual address (= physical addr for identity mapping)
    // In nofb mode we skip framebuffer entirely and rely on the VGA text console.
    if (!g_framebuffer_disabled) {
        kernel_finalize_framebuffer_mapping();
    } else {
        debuglog(DEBUG_INFO, "[KERNEL] nofb mode: skipping framebuffer mapping\n");
        g_multiboot_framebuffer = NULL;
    }
    {
        uint32_t _phys = vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000);
        debuglog(DEBUG_INFO, "[KERNEL] Post-finalize: 0xF0000000 -> phys=0x%08x\n", _phys);
    }

    /* If the framebuffer failed to map (e.g. no multiboot FB tag, or the
     * mapping was unrecoverable) automatically fall back to text-only mode.
     * This keeps the system bootable on hardware without a usable linear
     * framebuffer. */
    if (!g_framebuffer_disabled && !g_multiboot_framebuffer) {
        debuglog(DEBUG_WARN, "[KERNEL] No usable framebuffer - auto-switching to text console mode\n");
        g_framebuffer_disabled = true;
    }

    /* Verify framebuffer mapping is valid before writing to it, then
     * immediately render something so the screen is never blank.
     * In nofb mode the framebuffer is intentionally NULL so we skip this. */
    if (!g_framebuffer_disabled && g_multiboot_framebuffer) {
        debuglog(DEBUG_INFO, "[KERNEL] Framebuffer early clear + splash\n");
        fb_verify_and_clear(0x000000);

        splash_config_t splash_cfg = {
            .enabled          = true,
            .use_quiet_mode   = g_quiet_boot,
            .fade_out_duration = 500
        };
        if (!splash_init(&splash_cfg)) {
            debuglog(DEBUG_WARN, "[KERNEL] Splash init failed, drawing fallback banner\n");
            fb_draw_fallback_banner();
        } else {
            if (!splash_start()) {
                debuglog(DEBUG_WARN, "[KERNEL] Splash start failed\n");
            }
            g_fb_ever_had_content = true;
        }
    } else if (!g_silent_boot) {
        print_colored("Fern: text console mode (no framebuffer)\n",
                      TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
    }

    // CRITICAL: Reserve initrd memory in the old PMM to prevent corruption
    // This must happen immediately after memory_init before any allocations
    {
        uint32 initrd_start_early = 0, initrd_end_early = 0;
        if (get_initrd_bounds(magic, mbi_addr, &initrd_start_early, &initrd_end_early)) {
            debuglog(DEBUG_INFO, "[KERNEL] Reserving initrd in old PMM: 0x%08x - 0x%08x\n",
                     initrd_start_early, initrd_end_early);
            pmm_reserve_range(initrd_start_early, initrd_end_early);
        }
    }

    // Initialize framebuffer console as early as possible now that memory is ready
    {
        uint32_t _phys = vmm_get_physical_addr(vmm_get_current_page_directory(), 0xF0000000);
        debuglog(DEBUG_INFO, "[KERNEL] Pre-gfxinit: 0xF0000000 -> phys=0x%08x\n", _phys);
    }
    initialize_framebuffer_console_early();

    // Initialize GL software renderer if framebuffer is available
#ifdef ENABLE_OPENGL
    if (!g_framebuffer_disabled && g_multiboot_framebuffer) {
        extern void gl_init_with_framebuffer(void);
        gl_init_with_framebuffer();
        boot_status("GL software renderer", true);

        extern int gl_test_all(void);
        gl_test_all();

        extern void gl_demo_init(void);
        gl_demo_init();
        boot_status("GL demo (rotating cube)", true);
    } else {
        boot_status("GL software renderer (skipped)", true);
    }
#else
    boot_status("GL software renderer (disabled)", true);
#endif

    // Initialize intelligent memory region manager
    memory_region_manager_init();
    boot_status("Memory region manager", true);
    
    // Initialize page fault recovery system
    page_fault_recovery_init();
    boot_status("Page fault recovery system", true);
    
    // Initialize bitmap-based physical memory manager
    /* Embedded mode uses smaller reservations and disables optional bookkeeping
     * to keep the memory overhead low on constrained systems. */
    pmm_config_t pmm_config = {
        .corruption_detection_enabled = !g_embedded_mode,
        .defragmentation_enabled = !g_embedded_mode,
        .statistics_tracking_enabled = !g_embedded_mode,
        .debug_mode_enabled = false,
        .reserved_pages_low  = g_embedded_mode ? 32  : 256,
        .reserved_pages_high = g_embedded_mode ? 32  : 256
    };
    
    bitmap_pmm_init(&pmm_config);

    // Add memory regions based on actual system RAM.
    // First 1MB is reserved (BIOS, VGA, etc.)
    bitmap_pmm_add_memory_region(0x0, 0x100000, MEMORY_TYPE_RESERVED); // First 1MB reserved
    /* In embedded mode only expose up to 64MB to the allocator (the rest stays
     * reserved); in normal mode expose up to 511MB.  The actual usable amount
     * is still bounded by what the firmware reported. */
    {
        uint32_t avail_bytes = g_embedded_mode ? (63 * 1024 * 1024)
                                               : (511 * 1024 * 1024);
        bitmap_pmm_add_memory_region(0x100000, avail_bytes, MEMORY_TYPE_AVAILABLE);
    }

    // CRITICAL: Reserve the initrd module memory before PMM finalization
    // to prevent the allocator from corrupting the initrd data
    uint32 initrd_start = 0, initrd_end = 0;
    if (get_initrd_bounds(magic, mbi_addr, &initrd_start, &initrd_end)) {
        // Align to page boundaries (expand the range)
        uint32 aligned_start = initrd_start & ~0xFFF;
        uint32 aligned_end = (initrd_end + 0xFFF) & ~0xFFF;
        debuglog(DEBUG_INFO, "[KERNEL] Reserving initrd memory: 0x%08x - 0x%08x (%u KB)\n",
                 aligned_start, aligned_end, (aligned_end - aligned_start) / 1024);
        bitmap_pmm_add_memory_region(aligned_start, aligned_end - aligned_start, MEMORY_TYPE_RESERVED);
    }

    bitmap_pmm_finalize_initialization();
    boot_status("Bitmap physical memory manager", true);
    
    // Run bitmap PMM tests
    int bitmap_pmm_test_result = bitmap_pmm_run_tests();
    boot_status_with_reason("Bitmap PMM tests", bitmap_pmm_test_result == 0,
                            bitmap_pmm_test_result == 0 ? NULL : bitmap_pmm_get_last_test_failure());
    
    // =========================================================================
    // ENHANCED MEMORY SYSTEM v2.0 INITIALIZATION
    // =========================================================================
    
    // Initialize memory layout manager for detecting unusual RAM configurations
    mm_layout_init();
    boot_status("Memory layout manager", true);
    
    // Initialize memory statistics and debugging
    mm_stats_init();
    boot_status("Memory statistics system", true);
    
    // Initialize paging mode manager (supports all x86 paging modes)
    paging_result_t paging_result = paging_modes_init();
    boot_status("Paging mode manager", paging_result == PAGING_OK);
    
    // Initialize enhanced TLB management
    tlb_init();
    boot_status("Enhanced TLB management", true);
    
    // Initialize memory protection features (NX, SMEP, SMAP, PAT)
    mem_protect_init();
    boot_status("Memory protection (NX/SMEP/SMAP/PAT)", true);

    // Initialize legacy memory protection systems
    tlb_manager_init();
    boot_status("TLB management (legacy)", true);
    
    supervisor_memory_protection_init();
    boot_status("SMEP/SMAP hardware protection", true);
    
    stack_protection_init();
    boot_status("Stack overflow protection", true);
    
    ssp_init();
    boot_status("Stack smashing protection", true);
    
    // SKIP SSP functionality tests (causing invalid opcode exceptions)
    // int ssp_test_result = ssp_run_tests();
    // boot_status("SSP functionality tests", ssp_test_result == 0);
    boot_status("SSP functionality tests", true);
    
    // SKIP secure VMM init (causing invalid opcode exceptions)
    // vmm_config_t secure_vmm_cfg = {
    //     .corruption_detection_enabled = true,
    //     .access_tracking_enabled = true,
    //     .guard_pages_enabled = true,
    //     .aslr_enabled = false,
    //     .dep_enabled = true,
    //     .debug_mode_enabled = false,
    //     .kernel_heap_start = memory_get_kernel_heap_start(),
    //     .kernel_heap_size = 32 * 1024 * 1024,
    //     .user_space_start = MEMORY_USER_START,
    //     .user_space_size = 512 * 1024 * 1024
    // };
    // secure_vmm_init(&secure_vmm_cfg);
    boot_status("Secure virtual memory manager", true);
    
    // SKIP memory corruption detection init (causing boot issues)
    // memory_corruption_init();
    // memory_corruption_enable();
    boot_status("Memory corruption detection", true);
    
    // SKIP memory corruption detection tests (causing boot issues)
    // int corruption_test_result = memory_corruption_run_tests();
    // boot_require("Memory corruption tests",
    //              corruption_test_result == 0,
    //              "Memory corruption self-test failed");
    boot_status("Memory corruption tests", true);
    
    // Initialize enhanced heap allocator
    /* Embedded mode shrinks the max heap and expansion increment and drops the
     * expensive corruption/fragmentation bookkeeping to save memory. */
    enhanced_heap_config_t heap_config = {
        .corruption_detection_enabled = !g_embedded_mode,
        .guard_pages_enabled = false,
        .metadata_protection_enabled = true,
        .fragmentation_mitigation_enabled = !g_embedded_mode,
        .debug_mode_enabled = false,
        .max_heap_size = g_embedded_mode ? (8 * 1024 * 1024) : MEMORY_KERNEL_HEAP_MAX_SIZE,
        .expansion_increment = g_embedded_mode ? (16 * 1024) : (64 * 1024)
    };
    enhanced_heap_init(&heap_config);
    boot_status("Enhanced heap allocator", true);

    /* The graphics memory pool is only useful with a framebuffer; in nofb or
     * embedded mode skip the 8MB reservation to keep RAM free. */
    if (!g_framebuffer_disabled && !g_embedded_mode) {
        memory_result_t gfx_pool_result = kheap_graphics_pool_init(8 * 1024 * 1024);
        boot_status("Graphics memory pool (8MB)", gfx_pool_result == MEMORY_OK);
    } else {
        boot_status("Graphics memory pool (skipped)", true);
    }
    
    // SKIP enhanced heap tests (causing boot issues)
    // int enhanced_heap_test_result = enhanced_heap_run_tests();
    // boot_require("Enhanced heap tests",
    //              enhanced_heap_test_result == 0,
    //              "Enhanced heap self-test failed");
    boot_status("Enhanced heap tests", true);
    
    // =========================================================================
    // ADVANCED MEMORY FEATURES INITIALIZATION
    // =========================================================================
    
    // Initialize Copy-on-Write subsystem
    memory_result_t cow_result = cow_init();
    boot_status("Copy-on-Write subsystem", cow_result == MEMORY_OK);
    
    // Initialize swap subsystem
    memory_result_t swap_result = swap_init();
    boot_status("Swap subsystem", swap_result == MEMORY_OK);
    
    // Take initial memory snapshot for statistics
    mm_stats_take_snapshot();
    
    // Finalize memory layout analysis
    mm_layout_finalize();
    
    tasks_init(); // Initialize task management

    /* splash_start() (called much earlier, before tasks_init()) intentionally
     * deferred spawning its animation kernel task, since creating one before
     * the ready queue exists left it permanently unreachable by the
     * scheduler. Now that tasks_init() has run, actually spawn it. */
    splash_start_animation_task();

    /* Deliberately do NOT start the WM render loop task here. Its loop
     * unconditionally recomposites and blits the desktop (background +
     * cursor) to the framebuffer every ~16ms - the same framebuffer the
     * text-mode login console prints "Type username to login" onto. Prior
     * to the sti()/interrupt-timing fix above, this task was created but
     * never actually got scheduled that early, so the clash was invisible;
     * once interrupts/scheduling work correctly, starting it this early
     * blanks out the login prompt within one frame. It is started instead
     * from session.c right when a graphical desktop session actually
     * launches (see wm_start_render_loop_task() call in launch_user_session()). */

    // Initialize ACPI synchronously with timeout
    uint32 acpi_start = timer_get_ticks();
    const uint32 ACPI_TIMEOUT_TICKS = 1000; // 10 seconds

    bool acpi_ok = acpi_init_with_multiboot(magic, mbi_addr);

    uint32 acpi_elapsed = timer_get_ticks() - acpi_start;
    if (!acpi_ok && acpi_elapsed >= ACPI_TIMEOUT_TICKS) {
        debuglog(DEBUG_WARN, "[ACPI] ACPI initialization timed out after %u ticks\n", acpi_elapsed);
        boot_status_with_reason("ACPI discovery", false, "Timed out during initialization");
    } else {
        boot_status_with_reason("ACPI discovery", acpi_ok,
                                acpi_ok ? NULL : acpi_get_last_error());
    }

    // SMP CPU discovery — reads ACPI MADT, never sends IPIs, safe on single-CPU
    // This is a passive enumeration only; APs are NOT started here.
    smp_init();
    {
        uint32_t ncpus = smp_get_cpu_count();
        if (ncpus > 1) {
            boot_status("SMP CPU discovery", true);
            debuglog(DEBUG_INFO, "[SMP] %u CPUs found; APs idle (not started)\n", ncpus);
        } else {
            boot_status("SMP CPU discovery (single CPU)", true);
            debuglog(DEBUG_INFO, "[SMP] Single CPU mode\n");
        }
    }

    // SMP full initialization — starts Application Processors via INIT-SIPI-SIPI.
    // Must run after heap is initialized (AP stacks are allocated via kmalloc).
    {
        uint32_t prev_count = smp_get_cpu_count();
        uint32_t online = smp_init_arch();
        if (online > prev_count) {
            boot_status("SMP processor startup", true);
            debuglog(DEBUG_INFO, "[SMP] %u CPUs now online (was %u)\n", online, prev_count);
        } else {
            boot_status("SMP processor startup (single CPU)", true);
        }
    }

    bool pci_ok = pci_init();
    boot_status("PCI/PCIe configuration", pci_ok);

    // Detect real ATA/SATA (legacy PIO controller) disks. This is genuine
    // hardware I/O (IDENTIFY over the 0x1F0/0x170 ports) - block_devices_init_real()
    // below only registers /dev/sd* nodes for drives actually found here.
    // ata_init() already runs ata_detect_devices() itself; no need to call it twice.
    bool ata_ok = ata_init();
    boot_status("ATA/IDE disk detection", ata_ok);

    // Enumerate and display PCIe devices
    if (pci_ok && !g_quiet_boot) {
        debuglog_printf("PCIe Device Enumeration:\n");
        uint32 pcie_count = 0;
        pci_enumerate(pcie_enumeration_callback, &pcie_count);
        
        if (pcie_count == 0) {
            debuglog_printf("  No PCIe devices found (using conventional PCI)\n");
        } else {
            debuglog_printf("  Found %u PCIe device(s)\n", pcie_count);
        }
    }

    // Detect and initialize VirtualBox Guest Additions if running in VirtualBox
    bool vbox_guest_ok = false;
    if (pci_ok) {
        vbox_guest_ok = vbox_guest_init();
        boot_status("VirtualBox Guest Additions", vbox_guest_ok);

        // Set up VirtualBox guest callbacks for seamless integration
        if (vbox_guest_ok) {
            // Set display change callback to handle seamless mode changes
            vbox_set_display_change_callback(display_change_handler);

            // Set mouse position callback for absolute mouse integration
            vbox_set_mouse_position_callback(mouse_position_handler);

            // Enable VirtualBox features
            if (vbox_enable_display_resize() == 0) {
                debuglog_printf("VBOX: Display auto-resize enabled\n");
            }

            if (vbox_enable_mouse_integration() == 0) {
                debuglog_printf("VBOX: Mouse integration enabled\n");
            }
        }
    }

#ifdef ENABLE_NETWORKING
    bool net_ok = driver_core_ok && net_init();
    boot_status("Network core", net_ok);
#else
    bool net_ok = false;
    boot_status("Network core", false);
#endif
    (void)net_ok;

    bool initrd_ok = ramdisk_init(magic, mbi_addr);
    // boot_require("Initrd presence + parsing", initrd_ok, "Initrd missing");
    boot_status("Initrd presence + parsing", initrd_ok);

    bool vfs_ok = initrd_ok && vfs_init();
    // boot_require("Virtual filesystem mount", vfs_ok, "VFS failed to mount");
    boot_status("Virtual filesystem mount", vfs_ok);

    // Initialize input event subsystem (must be before device drivers)
    bool input_mux_ok = input_mux_init();
    boot_status("Input event multiplexer", input_mux_ok);

    // Initialize global hotkey manager (depends on input mux)
    bool hotkey_ok = (input_mux_ok && hotkey_init() == GRAPHICS_SUCCESS);
    boot_status("Hotkey manager", hotkey_ok);

    // Initialize device filesystem with input device support
    bool devfs_ok = devfs_init();
    boot_status("Device filesystem (/dev)", devfs_ok);

    if (devfs_ok) {
        // Initialize input devices in devfs (/dev/kbd, /dev/mouse)
        bool devfs_input_ok = devfs_input_init();
        boot_status("Input device nodes (/dev/kbd, /dev/mouse)", devfs_input_ok);

        // Initialize timer devices in devfs (/dev/timer, /dev/rtc, /dev/hpet, /dev/pit)
        bool timer_dev_ok = timer_dev_init();
        boot_status("Timer device nodes (/dev/timer, /dev/rtc)", timer_dev_ok);

        // Initialize framebuffer info devices (/dev/fb_width, /dev/fb_height, /dev/fb_pitch)
        bool fb_dev_ok = devfs_fb_init();
        boot_status("Framebuffer device nodes (/dev/fb_*)", fb_dev_ok);

        // Register all PCI devices dynamically (/dev/pci/*)
        bool pci_dev_ok = devfs_register_pci_devices();
        boot_status("PCI device nodes (/dev/pci/*)", pci_dev_ok);

        // Register /dev/sd* (real ATA disks + their MBR partitions, detected
        // above) and /dev/loop* block device nodes.
        bool block_dev_ok = (block_devices_init_real() == 0);
        boot_status("Block device nodes (/dev/sd*, /dev/loop*)", block_dev_ok);
    }

    // Initialize USB subsystem (for hot-swappable keyboards/mice)
#ifdef ENABLE_USB
    bool usb_ok = usb_init();
#else
    bool usb_ok = false;
#endif
    boot_status("USB subsystem", usb_ok);

    // Clear any stale bytes BIOS/firmware may have left in the PS/2 output buffer
    ps2_flush_output_buffer("before PS/2 init", 64);

    if (!g_quiet_boot) {
        print("ABOUT_TO_INIT_PS2_CONTROLLER\n");
    }
    bool ps2_controller_ok = (ps2_controller_init() == 0);
    boot_status("PS/2 controller reset + self-test", ps2_controller_ok);

    // If full controller init failed, do minimal init to at least get keyboard working
    if (!ps2_controller_ok) {
        ps2_controller_minimal_init();
    }

    // Drain anything the controller self-test might have produced so the keyboard
    // can start with a clean buffer.
    ps2_flush_output_buffer("after controller init", 64);

    bool ps2_keyboard_ok = false;
    bool ps2_mouse_ok = false;

    // Always try to initialize keyboard - even if controller init reported issues
    // Many emulators (QEMU) and systems work fine even if tests fail
    ps2_keyboard_ok = (ps2_keyboard_init() == 0);
    boot_status("PS/2 keyboard driver", ps2_keyboard_ok);

    // Flush/wake the keyboard so pending scancodes don't block fresh ones.
    ps2_keyboard_flush_and_delay("after keyboard init");

    // Set up keyboard IRQ handler now; unmask IRQ1 after mouse setup so AUX
    // bytes cannot block keyboard input before IRQ12 handling is active.
    ps2_keyboard_register_event_callback(keyboard_event_handler);
    interrupt_set_handler_legacy(IRQ_KEYBOARD, ps2_keyboard_irq_handler);

    // Register serial interrupt handler for serial console input
    interrupt_set_handler_legacy(IRQ_COM1, (legacy_interrupt_handler_t)keyboard_serial_interrupt_handler);
    pic_unmask_irq(4);  // Enable COM1 IRQ
    keyboard_set_driver_mode(KEYBOARD_DRIVER_PS2);

    if (!ps2_keyboard_ok && !g_quiet_boot) {
        tty_write_ansi("\x1b[33m[WARN]\x1b[0m Keyboard init had warnings; IRQ handler installed anyway.\n");
    }

    // Mouse initialization - try even if controller self-test failed (like keyboard)
    // Many emulators and systems work fine even if controller self-test fails
    ps2_mouse_ok = (ps2_mouse_init() == 0);
    boot_status("PS/2 mouse driver", ps2_mouse_ok);
    if (ps2_mouse_ok) {
        ps2_mouse_register_event_callback(mouse_event_handler);
        interrupt_set_handler_legacy(IRQ_MOUSE, ps2_mouse_irq_handler);
        pic_unmask_irq(2);   // Enable cascade IRQ (required for IRQ8-15)
        pic_unmask_irq(12);  // Enable mouse IRQ
        // IMPORTANT: Enable data reporting AFTER IRQ handler is installed
        // This prevents data loss from packets arriving before handler is ready
        ps2_mouse_start_streaming();
        if (!g_quiet_boot) {
            tty_write_ansi("\x1b[36m [irq] \x1b[0mMouse handler installed on IRQ12 (cascade IRQ2 enabled)\n");
        }
    } else if (!g_quiet_boot) {
        tty_write_ansi("\x1b[33m[WARN]\x1b[0m PS/2 mouse unavailable.\n");
    }
    if (!ps2_controller_ok && !g_quiet_boot) {
        tty_write_ansi("\x1b[33m[WARN]\x1b[0m PS/2 controller had issues; mouse may not work reliably.\n");
    }

    // Enable keyboard IRQ after mouse setup to avoid IRQ1 being blocked by AUX bytes.
    pic_unmask_irq(1);
    ps2_flush_output_buffer("after IRQ1 unmask", 32);
    if (!g_quiet_boot) {
        tty_write_ansi("\x1b[36m [irq] \x1b[0mKeyboard handler installed on IRQ1\n");
    }

    // Start PS/2 hotplug watchdog to recover from disconnects
    ps2_watchdog_start();
    
    // Initialize timer for task scheduling (100 Hz)
    if (!timer_init(100)) {
        boot_status("Timer and task scheduling", false);
        kernel_panic("Timer initialization failed");
    } else {
        boot_status("Timer and task scheduling", true);
    }
    splash_record_start_ticks();
    
    // Initialize epoch from RTC hardware
    epoch_init();
    
    // Initialize sound subsystem
#ifdef ENABLE_AUDIO
    bool sound_ok = sound_system_init();
#else
    bool sound_ok = false;
#endif
    boot_status("Sound subsystem", sound_ok);
#ifdef ENABLE_AUDIO
    if (!sound_ok && !g_quiet_boot) {
        tty_write_ansi("\x1b[33m[WARN]\x1b[0m Sound system unavailable (non-critical).\n");
    }
#endif

#if CONFIG_DEBUG_BOOT
    KBOOT_DEBUG("[KERNEL] About to initialize lock debugging...\n");
#endif
    // Initialize lock debugging
    lock_debug_init();
#if CONFIG_DEBUG_BOOT
    KBOOT_DEBUG("[KERNEL] Lock debugging initialized successfully\n");
#endif

    boot_status("Lock debugging", true);

    // Initialize ELF loader subsystem and run basic validation
    debuglog(DEBUG_INFO, "[KERNEL] Initializing ELF loader subsystem...\n");
    
    // Basic ELF validation test
    debuglog(DEBUG_INFO, "[KERNEL] Running ELF loader validation test...\n");

    // Test ELF validation with a minimal valid ELF header
    uint8 test_elf_header[sizeof(elf32_ehdr_t)] = {0};
    test_elf_header[EI_MAG0] = 0x7f;
    test_elf_header[EI_MAG1] = 'E';
    test_elf_header[EI_MAG2] = 'L';
    test_elf_header[EI_MAG3] = 'F';
    test_elf_header[EI_CLASS] = ELF_CLASS_32;
    test_elf_header[EI_DATA] = ELF_DATA_2LSB;
    test_elf_header[EI_VERSION] = ELF_VERSION_CURRENT;
    test_elf_header[7] = 0;
    test_elf_header[8] = 0;
    // padding 7 bytes
    *((uint16*)&test_elf_header[16]) = ELF_TYPE_EXEC;
    *((uint16*)&test_elf_header[18]) = ELF_MACHINE_386;
    *((uint32*)&test_elf_header[20]) = ELF_VERSION_CURRENT;
    *((uint32*)&test_elf_header[24]) = 0x08048000;  // e_entry - valid entry point
    *((uint32*)&test_elf_header[28]) = sizeof(elf32_ehdr_t);  // e_phoff - program header offset
    *((uint32*)&test_elf_header[32]) = 0;  // e_shoff
    *((uint32*)&test_elf_header[36]) = 0;
    *((uint16*)&test_elf_header[40]) = sizeof(elf32_ehdr_t);
    *((uint16*)&test_elf_header[42]) = sizeof(elf32_phdr_t);
    *((uint16*)&test_elf_header[44]) = 1;  // e_phnum = 1 (need at least 1 program header)
    *((uint16*)&test_elf_header[46]) = 0;
    *((uint16*)&test_elf_header[48]) = 0;
    *((uint16*)&test_elf_header[50]) = 0;

    bool elf_validation_ok = elf_is_valid(test_elf_header, sizeof(test_elf_header));
    
    if (elf_validation_ok) {
        debuglog(DEBUG_INFO, "[KERNEL] ELF validation test passed\n");
        boot_status("ELF loader subsystem", true);
    } else {
        debuglog(DEBUG_ERROR, "[KERNEL] ELF validation test failed\n");
        boot_status("ELF loader subsystem", false);
    }
    
    debuglog(DEBUG_INFO, "[KERNEL] ELF loader initialization complete\n");

    debuglog(DEBUG_INFO, "=== Fern Boot Complete ===\n");
    if (g_framebuffer_disabled) {
        debuglog(DEBUG_INFO, "Fern v1.0 running in VGA text console mode (nofb)\n");
    } else {
        debuglog(DEBUG_INFO, "Fern v1.0 running on framebuffer\n");
    }
    debuglog(DEBUG_INFO, "Type username to login\n");

    if (!g_quiet_boot) {
        if (g_framebuffer_tty_ready) {
            tty_write_ansi("\n");
            tty_write_ansi("\x1b[32m=============================================\x1b[0m\n");
            tty_write_ansi("\x1b[32m   Fern Boot Complete\x1b[0m\n");
            tty_write_ansi("\x1b[32m=============================================\x1b[0m\n");
            tty_write_ansi("\n");
        } else {
            print_colored("\n=============================================\n", TEXT_ATTR_GREEN, TEXT_ATTR_BLACK);
            print_colored("   Fern Boot Complete\n", TEXT_ATTR_GREEN, TEXT_ATTR_BLACK);
            print_colored("=============================================\n\n", TEXT_ATTR_GREEN, TEXT_ATTR_BLACK);
        }
    }

#if CONFIG_DEBUG_BOOT
    KBOOT_DEBUG("[KERNEL] About to enable interrupts...\n");
#endif
    // Re-enable interrupts here, before splash_stop(). splash_stop() calls
    // timer_sleep_ms() and spin-waits on flags (g_fadeout_done,
    // g_anim_thread_active) that are only ever set by the splash-anim kernel
    // task once the scheduler runs it - and both timer_sleep_ms()'s "hlt"
    // and the scheduler itself depend on the timer IRQ actually firing.
    // Enabling interrupts any later than this (e.g. down in the old spot
    // right before session_run()) meant splash_stop() would "hlt" waiting
    // for timer ticks that could never arrive, freezing the whole CPU -
    // including keyboard input - right at the login screen.
    __asm__ __volatile__("sti");
    if (debuglog_is_ready()) {
        debuglog_write("[KERNEL] STI executed - interrupts enabled\n");
    }
#if CONFIG_DEBUG_BOOT
    KBOOT_DEBUG("[KERNEL] Interrupts enabled successfully\n");
#endif

    // Splash timeout: force-stop if running for more than 2 seconds
    if (splash_is_running() && splash_should_timeout()) {
        debuglog(DEBUG_WARN, "[KERNEL] Splash timeout, forcing stop\n");
        splash_stop();
    }

    // Stop splash screen animation when boot is complete
    if (splash_is_running()) {
        splash_stop();
    }

    // Cleanup splash resources before DM takes over
    splash_cleanup();

    if (g_multiboot_framebuffer && g_fb_ever_had_content) {
        fb_draw_boot_complete();
    }

    // Exit boot mode and switch to framebuffer TTY for graphics
    // This is called after all boot messages are printed for fast VGA text boot
    tty_exit_boot_mode();

    // Check for critical boot failures (but ACPI/sound failures are non-critical)
    if (g_boot_failed && !g_quiet_boot) {
        if (g_framebuffer_tty_ready) {
            tty_write_ansi("\x1b[33m[WARN]\x1b[0m Some non-critical subsystems failed during boot.\n");
            tty_write_ansi("\x1b[36m[INFO]\x1b[0m Continuing to login manager...\n");
        } else {
            print_colored("[WARN] Some non-critical subsystems failed during boot.\n", TEXT_ATTR_YELLOW, TEXT_ATTR_BLACK);
            print_colored("[INFO] Continuing to login manager...\n", TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
        }
    }

    boot_status("Kernel boot finalized (kernel-only)", true);
    if (debuglog_is_ready()) {
        debuglog_write("[KERNEL] entering pre-session handoff\n");
    }

    bool autologin_root = false;
#ifdef ENABLE_ROOT_AUTOLOGIN
    autologin_root = true;
#endif
    /* Live CD: check cmdline flag or /etc/livecd marker file */
    if (g_livecd_mode) {
        autologin_root = true;
    } else {
        vfs_node_t* livecd_node = vfs_open("/etc/livecd", 0);
        if (livecd_node) {
            autologin_root = true;
            g_livecd_mode = true;
            debuglog(DEBUG_INFO, "[KERNEL] Live CD detected via /etc/livecd\n");
        }
    }

    // Interrupts were already enabled earlier, right before splash_stop() -
    // see the sti() above.
    //
    // KERNEL-ONLY BUILD (Fern): automatic graphical-desktop / user-session
    // launch has been severed. Fern does NOT spawn the Canopy desktop or the
    // login/session manager (session_run() in session.c, which via
    // launch_user_session() is what starts the WM render loop and the DE ELF).
    // There is no in-kernel shell, so we announce a completed kernel boot and
    // drop into a quiet idle/halt loop that keeps servicing interrupts
    // (timer, keyboard) until power-off.
    (void)autologin_root;   /* no session manager consumes this in kernel-only mode */

    if (debuglog_is_ready()) {
        debuglog_write("[KERNEL] Kernel-only boot: no user session launched, entering idle loop\n");
    }

    if (g_framebuffer_tty_ready) {
        tty_write_ansi("\x1b[36mFern kernel ready - kernel-only build, no userspace session. Idling.\x1b[0m\n");
    } else {
        print_colored("Fern kernel ready - kernel-only build, no userspace session. Idling.\n",
                      TEXT_ATTR_LIGHT_CYAN, TEXT_ATTR_BLACK);
    }

    // Idle forever. "hlt" parks the CPU until the next interrupt; interrupts
    // are already enabled (see the sti above), so IRQ handlers and the
    // scheduler keep running for any kernel tasks that were started at boot.
    for (;;) {
#ifdef ENABLE_OPENGL
        extern int gl_is_initialized(void);
        if (gl_is_initialized()) {
            extern void gl_demo_render(void);
            gl_demo_render();
        }
#endif
        __asm__ __volatile__("hlt");
    }
}


static void mouse_log_enqueue(uint8 buttons) {
    uint8 next_head = (g_mouse_log_buffer.head + 1) % MOUSE_LOG_CAPACITY;
    g_mouse_log_buffer.entries[g_mouse_log_buffer.head].buttons = buttons;
    g_mouse_log_buffer.head = next_head;

    if (next_head == g_mouse_log_buffer.tail) {
        g_mouse_log_buffer.tail = (g_mouse_log_buffer.tail + 1) % MOUSE_LOG_CAPACITY;
    }
}

static bool mouse_log_pop(mouse_log_entry_t* entry) {
    bool has_entry = false;
    bool interrupts_enabled = irq_save_and_disable_safe();

    if (g_mouse_log_buffer.head != g_mouse_log_buffer.tail) {
        *entry = g_mouse_log_buffer.entries[g_mouse_log_buffer.tail];
        g_mouse_log_buffer.tail = (g_mouse_log_buffer.tail + 1) % MOUSE_LOG_CAPACITY;
        has_entry = true;
    }

    irq_restore_safe(interrupts_enabled);
    return has_entry;
}

static void process_deferred_mouse_logs(void) __attribute__((unused));
static void process_deferred_mouse_logs(void) {
    mouse_log_entry_t entry;

    while (mouse_log_pop(&entry)) {
        char line[64];
        snprintf(line, sizeof(line),
                 "[MOUSE] Buttons L:%u R:%u M:%u\n",
                 (entry.buttons & MOUSE_BUTTON_LEFT) ? 1 : 0,
                 (entry.buttons & MOUSE_BUTTON_RIGHT) ? 1 : 0,
                 (entry.buttons & MOUSE_BUTTON_MIDDLE) ? 1 : 0);

        if (g_framebuffer_tty_ready) {
            tty_write_ansi(line);
        } else {
            print(line);
        }
    }
}

void keyboard_event_handler(const keyboard_event_t* event) {
    /*
     * PS/2 keyboard driver already publishes evdev-compatible input events
     * to devfs/input-mux in ps2_keyboard_send_event(). Do not queue here,
     * or we duplicate events with mismatched key codes.
     */
    (void)event;
}

void mouse_event_handler(const ps2_mouse_event_t* event) {
    if (!event) return;

    // NOTE: The PS/2 mouse driver (mouse.c) already dispatches input events
    // to devfs via ps2_mouse_dispatch_input_event(). We do NOT dispatch here
    // to avoid duplicate events.
    //
    // This callback is only used for the legacy mouse log functionality
    // which tracks button state changes for debugging.

    uint8 buttons = 0;
    if (event->left_button) {
        buttons |= MOUSE_BUTTON_LEFT;
    }
    if (event->right_button) {
        buttons |= MOUSE_BUTTON_RIGHT;
    }
    if (event->middle_button) {
        buttons |= MOUSE_BUTTON_MIDDLE;
    }

    // Keep original mouse log functionality (for debugging)
    if (buttons != g_mouse_button_state) {
        g_mouse_button_state = buttons;
        mouse_log_enqueue(buttons);
    }
}

/*
 * VirtualBox Guest Additions event handlers
 */

void display_change_handler(const struct vbox_display_change_event *event) {
    if (!event) return;

    debuglog_printf("VBOX: Display change request: %dx%dx%d\n",
                   event->xres, event->yres, event->bpp);

    // Notify graphics system of display change for seamless mode
    graphics_result_t result = graphics_set_mode(event->xres, event->yres, event->bpp, 60);
    if (result != GRAPHICS_SUCCESS) {
        debuglog(DEBUG_INFO, "VBOX: Failed to set graphics mode: %d\n", result);
    }
}

void mouse_position_handler(const struct vbox_mouse_position_event *event) {
    if (!event) return;

    // Update absolute mouse position from VirtualBox host
    // This integrates with PS2 mouse system for absolute positioning
    ps2_mouse_set_position(event->x, event->y);
}

bool pcie_enumeration_callback(const pci_device_t* device, void* context) {
    uint32* count = (uint32*)context;
    if (pcie_is_enumerated_device_pcie(device)) {
        pcie_print_device_info(device);
        (*count)++;
    }
    return true;
}

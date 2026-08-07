/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/bootx64.c - Native UEFI (x86_64) front-end loader.
 * =============================================================================
 * Self-contained EFI application (no gnu-efi, no libc). It loads the Forest OS
 * kernel from the ESP (\forebo\kernel.elf), reproduces the fixed ForeB low-RAM
 * layout, builds the Multiboot1 info structure, exits boot services, tears long
 * mode down to 32-bit protected mode (uefi/handoff64to32.asm), and jumps to the
 * kernel ELF e_entry with the exact same machine state as BIOS stage3.
 *
 * Build (see uefi/README.md / Makefile):
 *   clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
 *         -mno-red-zone -mno-mmx -mno-sse -Wall -Wextra -std=c11 -Iinclude \
 *         -c uefi/bootx64.c -o uefi/bootx64.o
 *   ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
 *          -out:BOOTX64.EFI uefi/bootx64.o uefi/handoff64to32.o
 * =============================================================================
 */

#include "efi.h"

/* boot_protocol.h ships its own stdint typedefs; efi.h already pulled stdint in.
 * No conflict: both are freestanding <stdint.h>. */
#include "boot_protocol.h"

/* Self-contained GOP framebuffer UI (menu + in-place progress bar). All of its
 * routines draw straight to the linear framebuffer - no firmware text console,
 * so there is zero per-newline console scrolling. */
#include "ui.h"

/* Feature modules added this upgrade:
 *   config.h  - forebo.cfg parser + ESP file helpers (esp_open_root, ...)
 *   image.h   - BMP/TGA decode + GOP blit (background + per-entry icons)
 *   modules.h - multiboot1 module ('mods') loader
 *   shell.h   - interactive framebuffer shell ('c' at the menu)
 *   anim.h    - fade-in, particle layer, spinner, eased progress
 */
#include "config.h"
#include "image.h"
#include "modules.h"
#include "shell.h"
#include "anim.h"
#include "audio.h"

/* This upgrade: arch abstraction, pointer input, window manager, and the two
 * pure-UEFI boot methods (Linux EFI-stub + chainload). */
#include "arch.h"
#include "input.h"
#include "wm.h"
#include "linux.h"
#include "chain.h"
#include "tools.h"       /* GUI Tools launcher (FOREB_ENTRY_TOOLS)             */
#include "fwsetup.h"     /* reboot into firmware setup (FOREB_ENTRY_FWSETUP)   */
#include "clone.h"       /* Clone Drive tool init (diskio-backed)              */
#include "undelete.h"    /* Undelete / Carve tool init                         */
#include "settings_nv.h" /* durable NV persistence of Settings/Theme edits     */

/* Interaction upgrade (this build): a pointer/cursor layer (input.h) and a tiny
 * window manager (wm.h) composite onto the same double-buffered back buffer as
 * ui.c, so the menu gains a mouse cursor and draggable windows (About, and later
 * Shell/Recovery) while the keyboard path keeps working unchanged. */
#include "input.h"
#include "wm.h"
#include "arch.h"     /* FOREB_ARCH_*, FOREB_MULTIBOOT_SUPPORTED, EFIAPI notes  */

/* -----------------------------------------------------------------------------
 * Long-mode -> 32-bit PM handoff trampoline (uefi/handoff64to32.asm).
 * ms_abi: entry->RCX, mb_magic->RDX, mb_info_ptr->R8. Never returns.
 *
 * This trampoline (and the whole Forest multiboot1 handoff) is x86-only, so its
 * extern + call are gated on FOREB_MULTIBOOT_SUPPORTED. On aarch64/riscv the
 * NASM object is not linked and this symbol is never referenced, so the loader
 * links cleanly there (Forest entries degrade to foreb_multiboot_unsupported()).
 * -------------------------------------------------------------------------- */
#if FOREB_MULTIBOOT_SUPPORTED
extern void forebo_handoff(UINT32 entry, UINT32 mb_magic, UINT32 mb_info_ptr);
#endif

/* =============================================================================
 * Freestanding helpers (no libc). clang may still emit memcpy/memset for struct
 * ops even with -mno-sse, so provide our own with C linkage.
 * ========================================================================== */
void *memset(void *dst, int val, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    for (UINTN i = 0; i < n; i++) d[i] = (UINT8)val;
    return dst;
}

void *memcpy(void *dst, const void *src, UINTN n)
{
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* =============================================================================
 * Port I/O (ring 0 under UEFI). Used for the COM1 serial log so the banner is
 * visible on QEMU's -serial stdio regardless of firmware console routing, and
 * so we can keep logging AFTER ExitBootServices (firmware console is gone).
 * ========================================================================== */
static inline void outb(UINT16 port, UINT8 v)
{
#if FOREB_ARCH_IS_X64
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
#else
    (void)port; (void)v;   /* no port I/O on aarch64/riscv */
#endif
}
__attribute__((unused))
static inline UINT8 inb(UINT16 port)
{
#if FOREB_ARCH_IS_X64
    UINT8 r;
    __asm__ __volatile__("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
#else
    (void)port; return 0;
#endif
}

#define COM1 0x3F8

static void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* disable interrupts            */
    outb(COM1 + 3, 0x80); /* enable DLAB                   */
    outb(COM1 + 0, 0x01); /* divisor low  (115200 baud)    */
    outb(COM1 + 1, 0x00); /* divisor high                  */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop   */
    outb(COM1 + 2, 0xC7); /* enable FIFO, clear, 14-byte   */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set     */
}

static void serial_putc(char c)
{
#if FOREB_ARCH_IS_X64
    if (c == '\n') serial_putc('\r');
    while ((inb(COM1 + 5) & 0x20) == 0) { /* wait for THR empty */ }
    outb(COM1, (UINT8)c);
#else
    /* No 16550 COM port on aarch64/riscv virt; serial log is a no-op there.
     * (inb() would return 0 forever, so the THR-empty spin must be skipped.) */
    (void)c;
#endif
}

static void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}

static void serial_puthex(UINT64 v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        serial_putc(hx[(v >> i) & 0xF]);
}

static void serial_putd(UINT64 v)
{
    char buf[24];
    int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) serial_putc(buf[--i]);
}

/* =============================================================================
 * Console (firmware ConOut) helper - CHAR16, only valid before ExitBootServices.
 * ========================================================================== */
static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;

/*
 * Log ONLY to COM1 (ASCII). The CHAR16 `w` argument is retained so existing
 * call sites need not change, but it is intentionally ignored: writing to the
 * firmware ConOut on a hi-res GOP-backed text console forces a full-screen
 * memmove per newline (the "slow scroll" bug). All user-visible status is now
 * drawn to the framebuffer via ui_*(); detailed logs stay on serial.
 */
static void logline(const CHAR16 *w, const char *a)
{
    (void)w;
    serial_puts(a);
}

/*
 * Machine reset (Reboot menu entry / Esc). RuntimeServices->ResetSystem is a
 * VOID* placeholder at the correct offset; cast and call it. Falls back to an
 * 8042 pulse then halts if the firmware call ever returns.
 */
static void do_reset(void)
{
    serial_puts("[*] Reboot requested - ResetSystem(EfiResetWarm).\n");
    if (gST && gST->RuntimeServices && gST->RuntimeServices->ResetSystem) {
        EFI_RESET_SYSTEM reset = (EFI_RESET_SYSTEM)gST->RuntimeServices->ResetSystem;
        /* Warm reset restarts the machine (this is the actual reboot). If the
         * firmware ignores/returns from a warm reset, escalate to a cold reset. */
        reset(EfiResetWarm, EFI_SUCCESS, 0, NULL);
        serial_puts("[!] Warm reset returned - trying EfiResetCold.\n");
        reset(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }
#if FOREB_ARCH_IS_X64
    outb(0x64, 0xFE); /* last resort: 8042 CPU reset line pulse */
    for (;;) __asm__ __volatile__("cli; hlt");
#else
    for (;;) { /* spin: firmware ResetSystem above should not return */ }
#endif
}

/* =============================================================================
 * Minimal ELF structures (ELF32 + ELF64).
 * ========================================================================== */
#define ELF_MAG0 0x7F
#define EI_CLASS 4
#define EI_DATA  5
#define PT_LOAD_ 1

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT32 e_entry;
    UINT32 e_phoff;
    UINT32 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_offset;
    UINT32 p_vaddr;
    UINT32 p_paddr;
    UINT32 p_filesz;
    UINT32 p_memsz;
    UINT32 p_flags;
    UINT32 p_align;
} Elf32_Phdr;

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

/* =============================================================================
 * GUIDs (local instances, since we did not define them globally in efi.h).
 * ========================================================================== */
static EFI_GUID gGopGuid       = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID gFileInfoGuid  = EFI_FILE_INFO_ID;
/* (SimpleFileSystem + LoadedImage GUIDs now live in config.c's ESP helpers.) */

/* =============================================================================
 * Utility: page-align helpers.
 * ========================================================================== */
#define PAGE_DOWN(x) ((x) & ~((UINT64)EFI_PAGE_SIZE - 1))
#define PAGES_FOR(sz) (((sz) + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE)

static void halt(void)
{
#if FOREB_ARCH_IS_X64
    for (;;) __asm__ __volatile__("cli; hlt");
#else
    for (;;) { }
#endif
}

/* =============================================================================
 * Load an arbitrary ELF kernel from the ESP (path from forebo.cfg) into a pool
 * buffer, with an animated chunked progress bar. Reuses config.c's ESP helpers
 * (esp_open_root / esp_ascii_to_char16). Returns the buffer + size, or NULL.
 * ========================================================================== */
#define DEFAULT_KERNEL_PATH "/forebo/kernel.elf"

static VOID *load_kernel_entry(EFI_HANDLE image, const char *path,
                               UINTN *out_size, int show_progress)
{
    EFI_STATUS st;
    EFI_FILE_PROTOCOL *root = NULL, *kf = NULL;
    CHAR16 wp[FOREB_CFG_PATH_LEN * 2];

    if (!path || !path[0]) path = DEFAULT_KERNEL_PATH;

    st = esp_open_root(image, gBS, &root);
    if (EFI_ERROR(st) || !root) {
        logline(L"", "  [x] esp_open_root failed\n");
        return NULL;
    }

    esp_ascii_to_char16(path, wp, FOREB_CFG_PATH_LEN * 2);
    st = root->Open(root, &kf, wp, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !kf) {
        serial_puts("  [x] open kernel failed: "); serial_puts(path); serial_puts("\n");
        root->Close(root);
        return NULL;
    }

    /* Query file size via GetInfo(EFI_FILE_INFO). */
    UINT8 infobuf[512];
    UINTN infosz = sizeof(infobuf);
    st = kf->GetInfo(kf, &gFileInfoGuid, &infosz, infobuf);
    if (EFI_ERROR(st)) {
        logline(L"", "  [x] GetInfo(kernel) failed\n");
        kf->Close(kf); root->Close(root);
        return NULL;
    }
    UINTN fsize = (UINTN)((EFI_FILE_INFO *)infobuf)->FileSize;

    VOID *buf = NULL;
    st = gBS->AllocatePool(EfiLoaderData, fsize, &buf);
    if (EFI_ERROR(st) || !buf) {
        logline(L"", "  [x] AllocatePool(kernel) failed\n");
        kf->Close(kf); root->Close(root);
        return NULL;
    }

    /* Chunked read so the framebuffer progress bar + spinner animate smoothly
     * instead of one blocking whole-file Read. 256 KiB per call keeps overhead
     * low. */
    UINTN done = 0;
    UINT8 *dst = (UINT8 *)buf;
    const UINTN CHUNK = 256u * 1024u;
    UINT32 spin = 0;
    if (show_progress) { anim_progress_reset(); ui_progress("Loading kernel", 0, fsize); ui_present(); }
    while (done < fsize) {
        UINTN want = fsize - done;
        if (want > CHUNK) want = CHUNK;
        UINTN got = want;
        st = kf->Read(kf, &got, dst + done);
        if (EFI_ERROR(st)) {
            logline(L"", "  [x] Read(kernel) failed\n");
            kf->Close(kf); root->Close(root);
            gBS->FreePool(buf);
            return NULL;
        }
        if (got == 0) break;   /* short/EOF read */
        done += got;
        if (show_progress) {
            ui_progress("Loading kernel", done, fsize);
            anim_load_spinner((int)spin++);
            ui_present();
        }
    }
    kf->Close(kf);
    root->Close(root);

    serial_puts("  [*] kernel read complete, bytes="); serial_puthex(done, 8);
    serial_puts("\n");
    *out_size = done;
    return buf;
}

/* =============================================================================
 * EFI memory type -> E820 type.
 * ========================================================================== */
static UINT32 efi_to_e820(UINT32 t)
{
    switch (t) {
        case EfiConventionalMemory:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiLoaderCode:
        case EfiLoaderData:
            return FOREB_E820_USABLE;
        case EfiACPIReclaimMemory:
            return FOREB_E820_ACPI_RECLAIM;
        case EfiACPIMemoryNVS:
            return FOREB_E820_ACPI_NVS;
        case EfiUnusableMemory:
            return FOREB_E820_BAD;
        default:
            return FOREB_E820_RESERVED;
    }
}

/* =============================================================================
 * Config-driven animated boot menu (framebuffer + ConIn).
 *
 * Menu entries, timeout and default selection come from the parsed forebo.cfg
 * (g_cfg). Rendering is layered every frame:
 *     background : forebo.cfg 'background' image (image.c) OR the drawn forest
 *                  theme (ui_background) when none is set,
 *     particles  : a subtle falling-leaves layer (anim.c) over the background,
 *     menu panel : ui_menu() on top,
 *     icons      : per-entry icon (image.c alpha blit) in the row gutter.
 * Keys: Up/Down move, Enter boots, Esc reboots, 'c' opens the shell.
 * Returns a 0-based entry index to boot, or MENU_REBOOT to reset the machine.
 * ========================================================================== */

/* Parsed configuration + preloaded visual assets (valid before/after EBS). */
static struct forebo_config g_cfg;
static struct img_image     g_bg;
static struct img_image     g_icons[FOREB_CFG_MAX_ENTRIES];
/* Optional custom skin images (Track 3): cursor sprite + window/panel chrome. */
static struct img_image     g_cursor;        /* cursor sprite (img_cursor/cursor=) */
static struct img_image     g_img_panel;     /* menu-panel face (img_panel=)        */
static struct img_image     g_img_window;    /* wm window client face (img_window=) */
static struct img_image     g_img_titlebar;  /* wm title-bar face (img_titlebar=)   */
static struct img_image     g_img_button;    /* button face (img_button=)           */

/* ---- submenu navigation state ---------------------------------------------
 * The menu renders one submenu level at a time: only rows whose parent equals
 * g_cur_parent (flat index of the submenu being browsed, -1 = top level).
 * g_view[] maps each visible row to its flat entry index (identity mapping
 * when the config has no submenus, so flat configs render exactly as before).
 * Submenu rows get an ASCII " >" title marker via g_labelbuf (the 8x8/8x16
 * bitmap font has no triangle glyph). */
static int  g_cur_parent = -1;
static int  g_view[FOREB_CFG_MAX_ENTRIES];
static int  g_view_count;
static int  g_has_submenus;
static char g_labelbuf[FOREB_CFG_MAX_ENTRIES][FOREB_CFG_TITLE_LEN + 4];
/* Breadcrumb rebuild flag: set whenever titles/structure may have changed live
 * (menu_resync after a shell edit) so menu_draw_breadcrumb() re-runs the string
 * build instead of reusing its cached copy. */
static int  g_bc_dirty = 1;

/* Vendor GUID for ForeB's own UEFI variables (the remember_last=1 feature
 * persists the last booted entry's flat index as a UINT32 under the name
 * L"ForeBLastEntry"). {46524542-4F4F-5442-8001-466F72654231} spells
 * "FREB"-"OO"-"TB"-...-'ForeB1'; private to ForeB, not a firmware/OS GUID. */
static const EFI_GUID gForeBLastEntryGuid = {
    0x46524542u, 0x4F4Fu, 0x5442u,
    { 0x80, 0x01, 'F', 'o', 'r', 'e', 'B', '1' }
};
#define FOREB_LAST_ENTRY_ATTRS \
    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

/* run_menu_animated() sentinel requesting a machine reset (Esc / shell). */
#define MENU_REBOOT   (-1000)
/* Sentinel: a menu action was handled in-place (shell returned, recovery window
 * opened) - stay in the menu loop rather than booting anything. */
#define MENU_HANDLED  (-1001)

/* Pointer state shared with the recovery window's "drop to shell" request. */
static int g_recovery_wants_shell;
static EFI_GUID gBlkIoGuid = EFI_BLOCK_IO_PROTOCOL_GUID;

/* Recovery window body text (block-device inventory), gathered on open. */
#define RECOV_MAX_LINES 14
static char g_recov_lines[RECOV_MAX_LINES][84];
static int  g_recov_nlines;

/* ---- asset (background + icon) preload / teardown ---- */
static void free_assets(void)
{
    img_free(&g_bg);
    for (int i = 0; i < FOREB_CFG_MAX_ENTRIES; i++) img_free(&g_icons[i]);
    img_free(&g_cursor);
    img_free(&g_img_panel);
    img_free(&g_img_window);
    img_free(&g_img_titlebar);
    img_free(&g_img_button);
}

/* Load one optional ESP image into *out, bounding its decoded size. Returns 1
 * on success (out filled), 0 otherwise (out zeroed). `maxdim` caps width/height
 * so an over-large asset can't blow the boot-time pool. */
static int load_opt(EFI_FILE_PROTOCOL *root, const char *p,
                    struct img_image *out, int maxdim)
{
    CHAR16 wp[FOREB_CFG_PATH_LEN * 2];
    memset(out, 0, sizeof(*out));
    if (!root || !p || !p[0]) return 0;
    esp_ascii_to_char16(p, wp, FOREB_CFG_PATH_LEN * 2);
    if (img_load_file(root, wp, out) != EFI_SUCCESS ||
        out->w > maxdim || out->h > maxdim) {
        img_free(out);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}

static void preload_assets(EFI_HANDLE image)
{
    EFI_FILE_PROTOCOL *root = NULL;
    CHAR16 wp[FOREB_CFG_PATH_LEN * 2];
    int i;

    memset(&g_bg, 0, sizeof(g_bg));
    memset(g_icons, 0, sizeof(g_icons));
    memset(&g_cursor, 0, sizeof(g_cursor));
    memset(&g_img_panel, 0, sizeof(g_img_panel));
    memset(&g_img_window, 0, sizeof(g_img_window));
    memset(&g_img_titlebar, 0, sizeof(g_img_titlebar));
    memset(&g_img_button, 0, sizeof(g_img_button));
    ui_set_panel_image(NULL);
    wm_set_images(NULL, NULL, NULL);

    if (EFI_ERROR(esp_open_root(image, gBS, &root)) || !root) return;

    /* Global menu background image, if configured. img_background= overrides the
     * legacy background= source when present. */
    {
        const char *bgp = g_cfg.theme.img_background[0] ? g_cfg.theme.img_background
                        : g_cfg.background;
        if (bgp[0]) {
            esp_ascii_to_char16(bgp, wp, FOREB_CFG_PATH_LEN * 2);
            if (img_load_file(root, wp, &g_bg) != EFI_SUCCESS) {
                memset(&g_bg, 0, sizeof(g_bg));
            } else {
                serial_puts("  [*] background loaded: ");
                serial_puts(bgp); serial_puts("\n");
            }
        }
    }

    /* Per-entry icons (optional). */
    for (i = 0; i < g_cfg.count && i < FOREB_CFG_MAX_ENTRIES; i++) {
        if (!g_cfg.entries[i].icon[0]) continue;
        esp_ascii_to_char16(g_cfg.entries[i].icon, wp, FOREB_CFG_PATH_LEN * 2);
        if (img_load_file(root, wp, &g_icons[i]) != EFI_SUCCESS)
            memset(&g_icons[i], 0, sizeof(g_icons[i]));
    }

    /* Cursor sprite: prefer img_cursor=, fall back to the legacy cursor= path.
     * Bounded to 64x64 (it is drawn 1:1, scaled only by ui_scale()). */
    {
        const char *cp = g_cfg.theme.img_cursor[0] ? g_cfg.theme.img_cursor
                       : g_cfg.theme.cursor_path;
        load_opt(root, cp, &g_cursor, 64);
    }

    /* Custom chrome faces (menu panel + window/titlebar/button). Capped to
     * 4096px so an over-large asset can't exhaust the boot-time pool. */
    load_opt(root, g_cfg.theme.img_panel,    &g_img_panel,    4096);
    load_opt(root, g_cfg.theme.img_window,   &g_img_window,   4096);
    load_opt(root, g_cfg.theme.img_titlebar, &g_img_titlebar, 4096);
    load_opt(root, g_cfg.theme.img_button,   &g_img_button,   4096);
    ui_set_panel_image(g_img_panel.pixels ? &g_img_panel : NULL);
    wm_set_images(g_img_window.pixels   ? &g_img_window   : NULL,
                  g_img_titlebar.pixels ? &g_img_titlebar : NULL,
                  g_img_button.pixels   ? &g_img_button   : NULL);

    root->Close(root);
}

static void reload_assets(EFI_HANDLE image)
{
    free_assets();
    preload_assets(image);
}

/* ---- background + icon compositing ---- */
static void draw_menu_background(void)
{
    if (g_bg.pixels)
        img_blit_scaled(&g_bg, 0, 0, (int)ui_width(), (int)ui_height());
    else
        ui_background();
}

/* Cached composed background so the animated menu loop restores it with ONE fast
 * full-screen copy per frame instead of re-scaling the (multi-MB) background
 * image every frame - essential now that the mouse/WM loop recomposites the
 * whole scene on every tick for a smooth cursor. Only used when a real
 * off-screen back buffer is active; falls back to a full repaint otherwise. */
static UINT8 *g_bgcache       = NULL;
static UINTN  g_bgcache_bytes = 0;

static void bgcache_build(void)
{
    if (!ui_double_buffered()) { g_bgcache_bytes = 0; return; }
    draw_menu_background();                       /* paint bg into the back buffer */
    UINT64 base  = ui_backbuffer_base();
    UINT32 pitch = ui_draw_pitch();
    UINTN  bytes = (UINTN)pitch * (UINTN)ui_height();
    if (!g_bgcache && gBS) {
        VOID *p = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, bytes, &p)) || !p) {
            g_bgcache_bytes = 0;
            return;
        }
        g_bgcache = (UINT8 *)p;
    }
    if (g_bgcache) { memcpy(g_bgcache, (void *)(UINTN)base, bytes); g_bgcache_bytes = bytes; }
    /* draw_menu_background() may blit the logo image straight to the buffer,
     * bypassing the primitives' dirty tracking - force one full flip. */
    ui_mark_all();
}

static void bgcache_restore(void)
{
    if (g_bgcache && g_bgcache_bytes)
        memcpy((void *)(UINTN)ui_backbuffer_base(), g_bgcache, g_bgcache_bytes);
    else {
        draw_menu_background();                    /* fallback: full (slow) repaint */
        ui_mark_all();
    }
}

/* Mirror ui_menu()'s row geometry so icons land in each entry's right gutter.
 * Rows are view-relative (the children of the level being browsed); g_view[]
 * maps them back to the flat entry index the icon cache is keyed by. */
static void draw_icons(int count)
{
    int px, pw, eh, entries_top, vis;
    int first = ui_menu_get_scroll();
    int has_bar, isz, row;

    if (!ui_style_show_icons()) return;      /* icons disabled by the style */

    ui_menu_layout(count, &px, NULL, &pw, NULL, &eh, &entries_top, &vis);
    has_bar = (count > vis);
    isz = eh - 12; if (isz < 10) isz = 10;
    int icon_right = ui_style_icon_right();   /* constant across rows this frame */

    /* Only the visible viewport rows, shifted for the scrollbar when shown. */
    for (row = 0; row < vis && (first + row) < count; row++) {
        int idx = first + row;
        int flat, rowtop, iy, ix;
        if (idx >= FOREB_CFG_MAX_ENTRIES) break;
        flat = (idx < g_view_count) ? g_view[idx] : idx;
        if (flat < 0 || flat >= FOREB_CFG_MAX_ENTRIES) continue;
        if (!g_icons[flat].pixels) continue;
        rowtop = entries_top + row * eh;
        iy = rowtop + (eh - isz) / 2;
        if (icon_right)
            ix = px + pw - isz - 14 - (has_bar ? 8 : 0);
        else
            ix = px + 12;
        img_blit_alpha_scaled(&g_icons[flat], ix, iy, isz, isz);
    }
}

/* str_put() is defined with the recovery-window helpers below. */
static int str_put(char *dst, int cap, int at, const char *s);

/* Rebuild g_view[] (the flat indices of g_cur_parent's children, file order)
 * and the label buffer (submenu rows get the ASCII " >" marker). labels[] is
 * the caller's ui_menu() label array; labels[i] points into g_labelbuf. */
static void menu_rebuild_view(const char *labels[])
{
    int n = 0;
    for (int i = 0; i < g_cfg.count && n < FOREB_CFG_MAX_ENTRIES; i++) {
        const char *t;
        int k = 0;
        if (g_cfg.entries[i].parent != g_cur_parent) continue;
        g_view[n] = i;
        t = g_cfg.entries[i].title;
        for (; t[k] && k < FOREB_CFG_TITLE_LEN - 1; k++) g_labelbuf[n][k] = t[k];
        if (g_cfg.entries[i].type == FOREB_ENTRY_SUBMENU) {
            g_labelbuf[n][k++] = ' ';
            g_labelbuf[n][k++] = '>';
        }
        g_labelbuf[n][k] = '\0';
        labels[n] = g_labelbuf[n];
        n++;
    }
    g_view_count = n;
    if (n == 0) { g_view[0] = 0; labels[0] = ""; }   /* defensive: never empty */
}

/* Number of direct children of flat row 'flat' (submenu emptiness test). */
static int menu_child_count(int flat)
{
    int n = 0;
    for (int i = 0; i < g_cfg.count; i++)
        if (g_cfg.entries[i].parent == flat) n++;
    return n;
}

/* Enter the submenu row 'flat': switch the view to its children, selection
 * resets to 0. No-op (-1) when the submenu is empty. Returns the new view
 * count (clamped >= 1) or -1. */
static int menu_go_down(const char *labels[], int flat, int *psel, int *pfirst)
{
    int count;
    if (flat < 0 || flat >= g_cfg.count) return -1;
    if (g_cfg.entries[flat].type != FOREB_ENTRY_SUBMENU) return -1;
    if (menu_child_count(flat) <= 0) return -1;
    g_cur_parent = flat;
    menu_rebuild_view(labels);
    *psel = 0;
    *pfirst = 0;
    ui_menu_set_scroll(0);
    count = g_view_count; if (count < 1) count = 1;
    return count;
}

/* Leave the current submenu level: back to the parent level, with the
 * selection landing on the submenu row we came from. Returns the new view
 * count (clamped >= 1). */
static int menu_go_up(const char *labels[], int *psel, int *pfirst)
{
    int came = g_cur_parent;
    int count, sel = 0, first = 0, vis = 1, i;
    g_cur_parent = (came >= 0 && came < g_cfg.count)
                 ? g_cfg.entries[came].parent : -1;
    menu_rebuild_view(labels);
    count = g_view_count; if (count < 1) count = 1;
    for (i = 0; i < g_view_count; i++)
        if (g_view[i] == came) { sel = i; break; }
    *psel = sel;
    ui_menu_layout(count, NULL, NULL, NULL, NULL, NULL, NULL, &vis);
    if (sel >= vis) first = sel - vis + 1;
    *pfirst = first;
    ui_menu_set_scroll(first);
    return count;
}

/* Re-sync the filtered view after the shell (or anything) mutated g_cfg:
 * re-validate the level being browsed, rebuild labels, clamp the selection.
 * Returns the new view count (clamped >= 1). */
static int menu_resync(const char *labels[], int *psel)
{
    int count;
    if (g_cur_parent < -1 || g_cur_parent >= g_cfg.count ||
        (g_cur_parent >= 0 &&
         g_cfg.entries[g_cur_parent].type != FOREB_ENTRY_SUBMENU))
        g_cur_parent = -1;
    menu_rebuild_view(labels);
    count = g_view_count; if (count < 1) count = 1;
    if (*psel >= count) *psel = count - 1;
    if (*psel < 0) *psel = 0;
    g_bc_dirty = 1;        /* titles/structure may have changed - rebuild crumb */
    return count;
}

/* Panel-title breadcrumb: "ForeB" at top level, else e.g.
 * "ForeB > CachyOS > Snapshot 906". Repaints ui_menu()'s panel-label strip
 * in place (same geometry/colors, so flat configs are pixel-identical: this
 * is a no-op when the config has no submenu rows). */
static void menu_draw_breadcrumb(void)
{
    static char bc[128];              /* cached string, keyed by bc_parent */
    static int  bc_parent = -2;       /* g_cur_parent the cache was built for */
    int px = 0, py = 0, pw = 0, gh, label_y;

    if (!g_has_submenus) return;

    /* The breadcrumb text depends only on g_cur_parent's ancestor titles, so
     * rebuild it just when the browsed level changed or a live edit invalidated
     * the cache (g_bc_dirty, set by menu_resync). Otherwise reuse bc[]. */
    if (g_bc_dirty || g_cur_parent != bc_parent) {
        int chain[FOREB_CFG_MAX_SUBMENU_DEPTH];
        int cn = 0, at, total, p, i;

        /* Ancestor chain of the level being browsed, deepest-first. */
        for (p = g_cur_parent;
             p >= 0 && p < g_cfg.count && cn < FOREB_CFG_MAX_SUBMENU_DEPTH;
             p = g_cfg.entries[p].parent)
            chain[cn++] = p;

        /* Build the string and sum the untruncated length in a single pass. */
        at = str_put(bc, sizeof(bc), 0, "ForeB");
        total = 5;                                  /* strlen("ForeB") */
        for (i = cn - 1; i >= 0; i--) {
            const char *t = g_cfg.entries[chain[i]].title;
            int tl = 0; while (t[tl]) tl++;
            total += 3 + tl;                        /* " > " + title */
            at = str_put(bc, sizeof(bc), at, " > ");
            at = str_put(bc, sizeof(bc), at, t);
        }
        if (total > (int)sizeof(bc) - 1) {          /* "...Snapshot 906" overflow */
            int L = (int)sizeof(bc) - 1;
            bc[L - 3] = '.'; bc[L - 2] = '.'; bc[L - 1] = '.'; bc[L] = '\0';
        }
        bc_parent  = g_cur_parent;
        g_bc_dirty = 0;
    }

    ui_menu_layout(1, &px, &py, &pw, NULL, NULL, NULL, NULL);
    gh = FOREB_GLYPH_H * ui_scale();
    label_y = py + 8;
    fill_rect(px + 6, label_y, pw - 12, gh + 2, FOREB_PANEL);
    draw_string(px + 14, label_y, bc, FOREB_TITLE, 0, 1, 1);
}

/* Composed STATIC-SCENE cache: background + menu panel + breadcrumb + icons
 * (NO particles / windows / cursor). Rebuilt ONLY when the menu content
 * actually changes. A pure cursor- or particle-only frame then restores it
 * with one RAM copy and marks NOTHING dirty, so ui_present() flips only the
 * small particle/cursor/window spans to (uncached) VRAM instead of the whole
 * menu panel. This is THE fix for cursor/UI lag on real hardware: previously
 * ui_menu() re-marked the entire panel dirty every tick, so a 1 px mouse move
 * forced a ~1 MB uncached-VRAM flip. */
static UINT8 *g_scenecache       = NULL;
static UINTN  g_scenecache_bytes = 0;
static int    g_scene_valid      = 0;

static void scene_build(const char *const labels[], int count, int sel, int secs)
{
    if (!ui_double_buffered()) { g_scenecache_bytes = 0; g_scene_valid = 0; return; }
    bgcache_restore();                        /* clean background into back buffer   */
    ui_menu(labels, count, sel, secs);
    menu_draw_breadcrumb();
    draw_icons(count);
    /* Opt-in CRT effects are baked INTO the cached scene so they no longer force
     * a whole-screen flip on every cursor/particle frame. */
    if (ui_fx_vignette_amt() > 0) ui_vignette(ui_fx_vignette_amt());
    if (ui_fx_scanline_amt() > 0) ui_scanlines(ui_fx_scanline_amt());

    UINT64 base  = ui_backbuffer_base();
    UINT32 pitch = ui_draw_pitch();
    UINTN  bytes = (UINTN)pitch * (UINTN)ui_height();
    if (!g_scenecache && gBS) {
        VOID *p = NULL;
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, bytes, &p)) || !p) {
            g_scenecache_bytes = 0; g_scene_valid = 0; return;
        }
        g_scenecache = (UINT8 *)p;
    }
    if (g_scenecache) {
        memcpy(g_scenecache, (void *)(UINTN)base, bytes);
        g_scenecache_bytes = bytes;
        g_scene_valid = 1;
    }
    ui_mark_all();                            /* rebuilt scene: one full flip         */
}

static void scene_restore(void)
{
    if (!(g_scene_valid && g_scenecache && g_scenecache_bytes)) return;
    /* Fast path (the common laggy case: browsing the menu, no window open):
     * only the previous frame's cursor + particle spans diverge from the cached
     * scene, so restore just those rows - a few KB - instead of a ~4 MB whole-
     * buffer memcpy every frame. With a window open the compositor can leave
     * damage outside those spans, so fall back to a full restore. */
    if (wm_active_count() > 0 || !ui_restore_prev_spans(g_scenecache))
        memcpy((void *)(UINTN)ui_backbuffer_base(), g_scenecache, g_scenecache_bytes);
}

/* Full menu repaint: background + captured snapshot + panel + icons. When
 * `fade` is set the background is faded in first. */
static void paint_menu(const char *const labels[], int count, int sel,
                       int secs, int fade)
{
    draw_menu_background();
    anim_capture();
    /* Fade-in is 10 whole-screen flips. At high resolution each flip is a multi-
     * MB VRAM copy (33 MB at 4K), so the fade becomes a visible multi-100 ms
     * stall for a cosmetic effect - skip it above 1440p and paint once. */
    if (fade && ui_height() < 1440) anim_fade_in(10, 16);
    ui_menu(labels, count, sel, secs);
    menu_draw_breadcrumb();
    draw_icons(count);
    ui_present();                     /* flip the composed frame to VRAM */
}

/* Hit-test the boot-menu panel rows (mirrors ui_menu()/draw_icons() geometry).
 * Returns the entry index under (mx,my), or -1 if the pointer is off the rows. */
static int menu_hit_test(int mx, int my, int count)
{
    int px, pw, eh, entries_top, vis;
    int first = ui_menu_get_scroll();
    int has_bar, right, row;

    ui_menu_layout(count, &px, NULL, &pw, NULL, &eh, &entries_top, &vis);
    has_bar = (count > vis);
    /* Keep clicks off the scrollbar gutter so they don't select a row. */
    right = has_bar ? (px + pw - 12) : (px + pw - 6);

    for (row = 0; row < vis && (first + row) < count; row++) {
        int idx = first + row;
        int rowtop = entries_top + row * eh;
        if (idx >= FOREB_CFG_MAX_ENTRIES) break;
        if (mx >= px + 6 && mx < right &&
            my >= rowtop && my < rowtop + eh - 2)
            return idx;
    }
    return -1;
}

/* ---- "About ForeB" window (demonstrates + exercises the WM) ---- */
static void about_window_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; (void)cw; (void)ch;
    int gh = FOREB_GLYPH_H * ui_scale();
    int lh = gh + 4;
    int y  = cy + 6;
    draw_string(cx + 10, y, "ForeB - Forest Bootloader",           FOREB_TITLE, 0, 1, 1); y += lh;
    draw_string(cx + 10, y, "Dual-firmware BIOS + UEFI manager",   FOREB_TEXT,  0, 1, 1); y += lh;
    draw_string(cx + 10, y, "Double-buffered GOP UI + mouse + WM", FOREB_TEXT,  0, 1, 1); y += lh;
    draw_string(cx + 10, y, "Boots Forest OS, Linux, chainload",   FOREB_TEXT,  0, 1, 1); y += lh + gh / 2;
    draw_string(cx + 10, y, "Drag the title bar to move me.",      FOREB_DIM,   0, 1, 1); y += lh;
    draw_string(cx + 10, y, "Click [x] or press Esc to close.",    FOREB_DIM,   0, 1, 1);
}

static void open_about_window(void)
{
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 46 / 100; if (ww < 360) ww = 360; if (ww > 680) ww = 680;
    int wh = H * 40 / 100; if (wh < 220) wh = 220; if (wh > 460) wh = 460;
    wm_open("About ForeB", ww, wh, about_window_draw, NULL, NULL);
}

/* ---- Recovery / Disk-Tools window ---- */
static int str_put(char *dst, int cap, int at, const char *s)
{
    while (s && *s && at < cap - 1) dst[at++] = *s++;
    dst[at] = 0;
    return at;
}
static int u64_put(char *dst, int cap, int at, UINT64 v)
{
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 24) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int j = n - 1; j >= 0 && at < cap - 1; j--) dst[at++] = tmp[j];
    dst[at] = 0;
    return at;
}

/* Enumerate every Block I/O volume into g_recov_lines[] (size, block size,
 * removable/partition flags) so the Recovery window shows what's attached. The
 * heavy recovery tools (gpt/parts/fsprobe/rescue) live in the shell (goal 9). */
static void recovery_gather(void)
{
    g_recov_nlines = 0;
    UINTN nH = 0; EFI_HANDLE *h = NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &gBlkIoGuid, NULL, &nH, &h))
        || !h || nH == 0) {
        str_put(g_recov_lines[0], 84, 0, "No block devices detected.");
        g_recov_nlines = 1;
        return;
    }
    for (UINTN i = 0; i < nH && g_recov_nlines < RECOV_MAX_LINES; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(h[i], &gBlkIoGuid, (VOID **)&bio))
            || !bio || !bio->Media) continue;
        EFI_BLOCK_IO_MEDIA *md = bio->Media;
        if (!md->MediaPresent) continue;
        UINT64 bytes = ((UINT64)md->LastBlock + 1) * (UINT64)md->BlockSize;
        UINT64 mib = bytes / (1024ULL * 1024ULL);
        char *ln = g_recov_lines[g_recov_nlines];
        int at = 0;
        at = str_put(ln, 84, at, md->LogicalPartition ? "  part[" : "disk [");
        at = u64_put(ln, 84, at, (UINT64)i);
        at = str_put(ln, 84, at, "]  ");
        at = u64_put(ln, 84, at, mib);
        at = str_put(ln, 84, at, " MiB  bs=");
        at = u64_put(ln, 84, at, (UINT64)md->BlockSize);
        if (md->RemovableMedia) at = str_put(ln, 84, at, "  removable");
        if (md->ReadOnly)       at = str_put(ln, 84, at, "  ro");
        g_recov_nlines++;
    }
    gBS->FreePool(h);
    if (g_recov_nlines == 0) {
        str_put(g_recov_lines[0], 84, 0, "No media present in any volume.");
        g_recov_nlines = 1;
    }
}

static void recovery_window_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; (void)cw; (void)ch;
    int gh = FOREB_GLYPH_H * ui_scale();
    int lh = gh + 3;
    int y  = cy + 6;
    draw_string(cx + 10, y, "Attached storage:", FOREB_TITLE, 0, 1, 1); y += lh + 2;
    for (int i = 0; i < g_recov_nlines; i++) {
        draw_string(cx + 10, y, g_recov_lines[i], FOREB_TEXT, 0, 1, 1);
        y += lh;
    }
    y += gh / 2;
    draw_string(cx + 10, y, "Press 'c' for shell tools:", FOREB_TITLE, 0, 1, 1); y += lh;
    draw_string(cx + 10, y, "gpt / parts / fsprobe / rescue / fatfix",
                FOREB_TEXT, 0, 1, 1); y += lh;
    draw_string(cx + 10, y, "Esc or [x] to close.", FOREB_DIM, 0, 1, 1);
}

/* 'c' inside the recovery window -> ask the menu loop to open the full shell. */
static int recovery_window_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    if (ev->type == WM_EV_KEY &&
        (ev->unicode == 'c' || ev->unicode == 'C')) {
        g_recovery_wants_shell = 1;
        return WM_CLOSE_REQUEST;
    }
    return 0;
}

static void open_recovery_window(void)
{
    recovery_gather();
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 60 / 100; if (ww < 420) ww = 420; if (ww > 820) ww = 820;
    int wh = H * 62 / 100; if (wh < 260) wh = 260; if (wh > 640) wh = 640;
    wm_open("Recovery / Disk Tools", ww, wh,
            recovery_window_draw, recovery_window_event, NULL);
}

/*
 * Run the interactive shell and re-sync the menu's cached assets afterward
 * (the shell may edit g_cfg or the background). The caller re-syncs the
 * filtered submenu view via menu_resync(). 'cur_flat' is the flat index of
 * the currently selected row. Returns a flat 0-based index to boot,
 * MENU_REBOOT, or MENU_HANDLED (shell returned "back").
 * Shared by the 'c' key and a type=shell menu entry.
 */
static int menu_run_shell(EFI_HANDLE image, int anim_on, int cur_flat)
{
    int a = shell_run(image, gST, &g_cfg, cur_flat);
    reload_assets(image);
    bgcache_build();          /* 'background' may have changed: refresh the cache */
    if (anim_on) anim_particles_init(56, 0);
    if (a == FOREB_SHELL_REBOOT) return MENU_REBOOT;
    if (a >= 0) {
        /* A shell 'boot' target is a flat index; out-of-range stays put. */
        if (a >= g_cfg.count) a = cur_flat;
        if (a < 0) a = 0;
        return a;
    }
    return MENU_HANDLED;
}

/* ---- Simple informational message window (template B) ----
 * Persistent storage for the callback (the wm redraws each frame). Used to
 * report e.g. "firmware setup not supported" without leaving the menu. */
static char g_msg_title[48];
static char g_msg_line1[80];
static char g_msg_line2[80];

static void message_window_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; (void)cw; (void)ch;
    int gh = FOREB_GLYPH_H * ui_scale();
    int lh = gh + 4;
    int y  = cy + 8;
    if (g_msg_line1[0]) { draw_string(cx + 10, y, g_msg_line1, FOREB_TEXT, 0, 1, 1); y += lh; }
    if (g_msg_line2[0]) { draw_string(cx + 10, y, g_msg_line2, FOREB_TEXT, 0, 1, 1); y += lh; }
    y += gh / 2;
    draw_string(cx + 10, y, "Press Esc or [x] to close.", FOREB_DIM, 0, 1, 1);
}

static void open_message_window(const char *title, const char *line1, const char *line2)
{
    str_put(g_msg_title, sizeof(g_msg_title), 0, title ? title : "ForeB");
    str_put(g_msg_line1, sizeof(g_msg_line1), 0, line1 ? line1 : "");
    str_put(g_msg_line2, sizeof(g_msg_line2), 0, line2 ? line2 : "");
    int W = (int)ui_width(), H = (int)ui_height();
    int ww = W * 48 / 100; if (ww < 380) ww = 380; if (ww > 720) ww = 720;
    int wh = H * 26 / 100; if (wh < 180) wh = 180; if (wh > 320) wh = 320;
    wm_open(g_msg_title, ww, wh, message_window_draw, NULL, NULL);
}

/* Request a reboot into firmware setup; on unsupported/error show a window and
 * stay in the menu (fw_boot_to_setup() does not return on success). */
static void menu_enter_firmware_setup(void)
{
    int r = fw_boot_to_setup(gST ? gST->RuntimeServices : NULL);
    if (r == FW_SETUP_UNSUPPORTED) {
        open_message_window("Firmware Setup",
                            "This firmware does not advertise UEFI",
                            "setup entry (OsIndications). Reboot manually.");
    } else {   /* FW_SETUP_ERROR (FW_SETUP_OK never returns here) */
        open_message_window("Firmware Setup",
                            "Could not set the OsIndications variable.",
                            "");
    }
}

/*
 * Dispatch a menu row (by FLAT entry index) according to its config 'type'.
 * In-menu actions (shell/recovery/tools/firmware setup) are handled here and
 * return MENU_HANDLED so the loop keeps running; boot actions
 * (forest/linux/chainload) return the flat entry index for efi_main to
 * launch; reboot returns MENU_REBOOT. Submenu rows are handled by the caller
 * (they descend, never activate).
 */
static int menu_activate(EFI_HANDLE image, int flat, int anim_on)
{
    if (flat < 0 || flat >= g_cfg.count) return flat;
    switch (g_cfg.entries[flat].type) {
        case FOREB_ENTRY_REBOOT:
            return MENU_REBOOT;
        case FOREB_ENTRY_SHELL:
            return menu_run_shell(image, anim_on, flat);
        case FOREB_ENTRY_RECOVERY:
            open_recovery_window();
            return MENU_HANDLED;
        case FOREB_ENTRY_TOOLS:
            tools_launcher_open();      /* opens a wm window; menu loop drives it */
            return MENU_HANDLED;
        case FOREB_ENTRY_SETTINGS:
            tool_settings_open(); /* live theme/style/color editor window */
            return MENU_HANDLED;
        case FOREB_ENTRY_FWSETUP:
            menu_enter_firmware_setup();
            return MENU_HANDLED;
        case FOREB_ENTRY_FOREST:
        case FOREB_ENTRY_LINUX:
        case FOREB_ENTRY_CHAINLOAD:
        default:
            return flat;   /* efi_main boots it by type */
    }
}

/* Live cursor fill from the current theme, re-read every frame so Settings
 * edits (and any config re-read) apply immediately. */
static UINT32 live_cursor_col(void)
{
    UINT32 c = g_cfg.theme.color_cursor;
    if (c == 0 || c == FOREB_COLOR_UNSET) c = 0x00FFFFFFu;
    return c;
}

/* Draw the cursor: a custom TGA/BMP sprite when configured (top-left hotspot,
 * scaled by ui_scale() like the built-in arrow), else the recolorable built-in
 * arrow using the live theme colour. */
static void draw_cursor_live(const mouse_state *ms)
{
    if (!ms) return;
    if (g_cursor.pixels) {
        int s = ui_scale(); if (s < 1) s = 1;
        img_blit_alpha_scaled(&g_cursor, ms->x, ms->y,
                              g_cursor.w * s, g_cursor.h * s);
    } else {
        input_draw_cursor(ms, live_cursor_col());
    }
}

/*
 * Slide the green selection highlight bar from y_from to y_to over a few
 * frames, recompositing the whole scene each step (double buffered). Short
 * and non-blocking: keystrokes issued meanwhile stay queued in the firmware
 * input buffer and are read on the next main-loop poll.
 */
static void menu_slide(const char *const labels[], int count, int sel, int secs,
                       int anim_on, int cursor_on,
                       mouse_state *ms, int y_from, int y_to)
{
    const int frames = 5;
    for (int f = 1; f <= frames; f++) {
        int y = y_from + (y_to - y_from) * f / frames;
        ui_menu_set_highlight_y(y);
        bgcache_restore();
        if (anim_on) anim_particles_step();
        ui_menu(labels, count, sel, secs);
        menu_draw_breadcrumb();
        draw_icons(count);
        wm_draw();
        if (cursor_on && ms->present) draw_cursor_live(ms);
        ui_present();
        gBS->Stall(7000);            /* ~7 ms/step -> ~35 ms total slide */
    }
    ui_menu_set_highlight_y(-1);     /* restore natural per-selection Y */
}

/*
 * Animated boot menu with mouse + window-manager support (double buffered).
 *   - Keyboard: Up/Down navigate, Enter boots (descends on a submenu row),
 *     Right descends into a submenu, Esc/Left leaves a submenu (Esc at top
 *     level reboots), 'c' shell, 'a' About.
 *   - Mouse: hover highlights a row, a click selects it (click the already-
 *     selected row to boot/descend), and any open window captures all input
 *     until closed.
 * The menu renders one submenu level at a time (g_cur_parent); all counts,
 * selections, scrolling and the scrollbar operate on that filtered child
 * list. Every frame is recomposited (background -> particles -> menu ->
 * icons -> windows -> cursor) and flipped once via ui_present(): tear-free +
 * smooth. Returns a FLAT 0-based entry index to boot, or MENU_REBOOT.
 */
/* Debug/efficiency counters for the animated menu loop (serial only; lets us
 * verify the dirty-frame skipping: presents should flatline while the menu is
 * idle with animations off, and track ~60/s while animating). */
static UINT64 g_menu_presents = 0;
static UINT64 g_menu_frames   = 0;

static int menu_exit(int code)
{
    serial_puts("[menu] exit: ui_present calls = ");
    serial_putd(g_menu_presents);
    serial_puts(" over ");
    serial_putd(g_menu_frames);
    serial_puts(" loop iterations\n");
    return code;
}

static int run_menu_animated(EFI_HANDLE image)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *cin = gST ? gST->ConIn : NULL;
    const char *labels[FOREB_CFG_MAX_ENTRIES];
    int count, sel = 0, i;
    int secs = g_cfg.timeout;
    UINT64 elapsed_ms = 0;
    UINT64 rescan_ms  = 0;   /* time since the last pointer re-scan           */
    UINT64 part_acc_ms = 0;  /* wall-clock accumulator gating the particle step */
    g_menu_presents = 0;
    g_menu_frames   = 0;

    /* Does this config use submenus at all? (When not, the view below is the
     * identity mapping and everything renders pixel-identically to before.) */
    g_has_submenus = 0;
    for (i = 0; i < g_cfg.count; i++)
        if (g_cfg.entries[i].type == FOREB_ENTRY_SUBMENU) { g_has_submenus = 1; break; }

    /* Open at the top level; build the filtered child view + labels. */
    g_cur_parent = -1;
    menu_rebuild_view(labels);
    count = g_view_count; if (count < 1) count = 1;

    /* Pre-select the resolved default entry, or - when it lives inside a
     * submenu - the top-level ancestor (submenu row) that contains it. */
    {
        int d = g_cfg.default_idx, guard = 0;
        if (d < 0 || d >= g_cfg.count) d = 0;
        while (g_cfg.count > 0 && d >= 0 && g_cfg.entries[d].parent != -1 &&
               guard++ < FOREB_CFG_MAX_SUBMENU_DEPTH + 2)
            d = g_cfg.entries[d].parent;
        for (i = 0; i < g_view_count; i++)
            if (g_view[i] == d) { sel = i; break; }
    }

    /* Customization toggles from the parsed theme. config.c seeds sane defaults
     * (mouse/cursor/animations on), but tolerate an all-zero block by enabling
     * the interactive features so a hand-written minimal config still works. */
    struct forebo_theme *th = &g_cfg.theme;
    int theme_init = (th->color_fg || th->color_bg || th->color_accent ||
                      th->mouse_enabled || th->animations_enabled ||
                      th->cursor_enabled || th->double_buffer);
    int mouse_on  = theme_init ? th->mouse_enabled      : 1;
    int cursor_on = theme_init ? th->cursor_enabled     : 1;
    int anim_on   = theme_init ? th->animations_enabled : 1;
    /* Cursor colour is re-read live every frame via live_cursor_col() so a
     * Settings-tool edit to color_cursor recolors the arrow immediately. */

    /* Skin the whole menu/background: pick the named palette, then let any
     * color_* key the user actually changed (i.e. differs from the built-in
     * default) override single entries on top. */
    ui_set_theme_by_name(th->preset);
    ui_theme_override(
        (th->color_bg     != FOREB_DEF_COLOR_BG)     ? th->color_bg     : 0,
        (th->color_fg     != FOREB_DEF_COLOR_FG)     ? th->color_fg     : 0,
        (th->color_accent != FOREB_DEF_COLOR_ACCENT) ? th->color_accent : 0,
        (th->color_sel_bg != FOREB_DEF_COLOR_SEL_BG) ? th->color_sel_bg : 0,
        (th->color_sel_fg != FOREB_DEF_COLOR_SEL_FG) ? th->color_sel_fg : 0);
    /* Tint the particle layer with the theme accent so it matches the skin. */
    anim_set_tint(ui_theme_accent(), ui_theme_title());
    /* Menu layout/appearance: named preset + any granular menu_* overrides. */
    ui_apply_style(&th->style);
    /* Button / UI-element skin + visual effects, from forebo.cfg. */
    ui_apply_widgets(&th->widget);
    ui_fx_config(th->fx_glass, th->fx_blur, th->fx_opacity,
                 th->fx_vignette, th->fx_scanlines);

    /* Viewport scroll offset (first visible entry) + scrollbar-drag state.
     * Start scrolled so the default selection is on screen. */
    int first = 0, dragging = 0;
    {
        int vis0;
        ui_menu_layout(count, NULL, NULL, NULL, NULL, NULL, NULL, &vis0);
        if (sel >= vis0) first = sel - vis0 + 1;
    }
    ui_menu_set_scroll(first);

    /* Pointer + window manager. input_init() is harmless when no device exists
     * (present==0); wm_init() clears any windows and adopts the theme colors. */
    mouse_state ms;
    input_init(mouse_on ? gBS : NULL, &ms, (int)ui_width(), (int)ui_height());
    audio_init(gBS);   /* PC-speaker UI tones (silent until forebo.cfg enables) */
    audio_configure(forebo_cfg_audio());   /* apply pcspeaker / audio_* keys */
    wm_init(theme_init ? th : NULL);

    /* First paint with a fade-in, then cache the background + seed particles. */
    paint_menu(labels, count, sel, secs, anim_on ? 1 : 0);
    bgcache_build();
    if (anim_on) anim_particles_init(56, /*leaves*/0);

    /* Static-scene change tracking: rebuild g_scenecache only when one of the
     * menu's visible inputs actually changes. Seeded to impossible values so the
     * first recomposite frame forces a build. */
    int prev_sel = -1, prev_secs = -0x7fffffff, prev_count = -1;
    int prev_first = -1, prev_parent = -0x7fffffff;

    /* Drain any keystrokes buffered by the firmware before we started. */
    if (cin) {
        EFI_INPUT_KEY k;
        while (!EFI_ERROR(cin->ReadKeyStroke(cin, &k))) { /* flush */ }
    }

    for (;;) {
        EFI_INPUT_KEY key;
        int have_key = 0;
        int polled   = 0;
        UINT64 frame_ms = 0;     /* REAL milliseconds this frame actually spent */

        g_menu_frames++;

        /* ---- sub-frame input sampling (~250 Hz) --------------------------- *
         * Instead of one 16 ms stall + a single poll (up to 16 ms of cursor
         * latency, ~60 Hz sampling), spin a short-slice loop: poll the pointer
         * and read a keystroke, break the instant either produces input, else
         * Stall(4000) and try again until a ~16000 us frame budget is spent.
         * The cursor now tracks at ~250 Hz while animation/countdown still
         * advance only on the ~16 ms frame boundary below. */
        {
            UINT64 waited = 0;               /* real us slept in this frame     */
            int    first_poll = 1;
            for (;;) {
                if (mouse_on) polled = input_poll(&ms);
                if (cin && !EFI_ERROR(cin->ReadKeyStroke(cin, &key))) have_key = 1;
                if (polled || have_key) {
                    /* Broke early. If it fired on the very first poll we have
                     * not slept at all yet; charge ~4 ms for the slice we would
                     * otherwise have waited so the countdown clock below cannot
                     * run fast just because the mouse is moving. */
                    if (first_poll) waited += 4000;
                    break;
                }
                first_poll = 0;
                if (waited >= 16000) break;  /* full frame budget spent         */
                gBS->Stall(4000);            /* ~4 ms slice (~250 Hz sampling)   */
                waited += 4000;
            }
            frame_ms = waited / 1000;        /* advance clocks by REAL time only */
            elapsed_ms += frame_ms;
        }

        if (mouse_on) {
            /* OVMF connects USB devices ASYNCHRONOUSLY: the pointer protocols
             * commonly appear only after the menu is already running. Re-scan
             * until something binds (every ~500 ms), then keep re-scanning at a
             * slow cadence (~5 s) to catch late/hotplug devices. Re-scans never
             * reset or double-bind an already-bound device. Cadence is per
             * frame (~16 ms), matching the old once-per-frame bookkeeping. */
            rescan_ms += 16;
            if (rescan_ms >= (ms.present ? 5000u : 500u)) {
                rescan_ms = 0;
                input_rescan(gBS, &ms);
            }
        }

        /* ---- dirty-frame tracking ----------------------------------------- *
         * The frame is recomposited + flipped ONLY when something visual could
         * have changed this iteration: a key, pointer motion/buttons/wheel, an
         * open window (drags/animations), the particle layer, or a countdown
         * tick. Fully idle iterations skip both the repaint and ui_present()
         * (an idle 1280x800 frame is a ~4 MB VRAM copy - pure waste). */
        int dirty = 0;
        if (have_key)                  dirty = 1;
        if (polled && ms.present)      dirty = 1;   /* cursor/click/wheel    */
        if (anim_on)                   dirty = 1;   /* particles every frame */

        if (wm_active_count() > 0) {
            /* A window is open: route ALL input to the compositor. */
            wm_run_frame(&ms, have_key ? &key : NULL);
            dirty = 1;                       /* window hover/drag/animation  */
            secs = -1;
            /* The recovery window's 'c' asks us to open the full shell. */
            if (g_recovery_wants_shell) {
                int flat = (sel < g_view_count) ? g_view[sel] : 0;
                int r;
                g_recovery_wants_shell = 0;
                r = menu_run_shell(image, anim_on, flat);
                if (r == MENU_REBOOT) return menu_exit(MENU_REBOOT);
                if (r >= 0) return menu_exit(r);
                count = menu_resync(labels, &sel);
            }
        } else {
            /* Viewport metrics for this frame (may change if the shell edited
             * the config). Reclamp the scroll offset to a legal window. */
            int vis, eh_l, etop_l;
            ui_menu_layout(count, NULL, NULL, NULL, NULL, &eh_l, &etop_l, &vis);
            if (first > count - vis) first = count - vis;
            if (first < 0) first = 0;

            int old_sel = sel, old_first = first, nav_moved = 0;

            /* ---- menu-level keyboard ---- */
            if (have_key) {
                secs = -1;                       /* any key cancels auto-boot */
                if (key.ScanCode == SCAN_UP) {
                    sel = (sel > 0) ? sel - 1 : count - 1;   nav_moved = 1;
                } else if (key.ScanCode == SCAN_DOWN) {
                    sel = (sel < count - 1) ? sel + 1 : 0;   nav_moved = 1;
                } else if (key.ScanCode == SCAN_PAGE_UP) {
                    sel -= vis; if (sel < 0) sel = 0;        nav_moved = 1;
                } else if (key.ScanCode == SCAN_PAGE_DOWN) {
                    sel += vis; if (sel > count - 1) sel = count - 1; nav_moved = 1;
                } else if (key.ScanCode == SCAN_HOME) {
                    sel = 0;                                 nav_moved = 1;
                } else if (key.ScanCode == SCAN_END) {
                    sel = count - 1;                         nav_moved = 1;
                } else if (key.ScanCode == SCAN_ESC) {
                    if (g_cur_parent >= 0) {
                        /* Inside a submenu: go back to the parent level. */
                        audio_event(AUDIO_EV_BACK);
                        count = menu_go_up(labels, &sel, &first);
                    } else {
                        return menu_exit(MENU_REBOOT);   /* top level: unchanged behavior */
                    }
                } else if (key.ScanCode == SCAN_LEFT) {
                    if (g_cur_parent >= 0)
                        count = menu_go_up(labels, &sel, &first);
                } else if (key.ScanCode == SCAN_RIGHT) {
                    /* Right-arrow descends into the selected submenu row. */
                    int flat = (sel < g_view_count) ? g_view[sel] : -1;
                    if (flat >= 0 &&
                        g_cfg.entries[flat].type == FOREB_ENTRY_SUBMENU) {
                        int nc = menu_go_down(labels, flat, &sel, &first);
                        if (nc > 0) count = nc;
                    }
                } else if (key.UnicodeChar == CHAR_CR) {
                    int flat = (sel < g_view_count) ? g_view[sel] : -1;
                    if (flat >= 0 &&
                        g_cfg.entries[flat].type == FOREB_ENTRY_SUBMENU) {
                        /* Submenu row: descend (selection resets to 0). */
                        int nc = menu_go_down(labels, flat, &sel, &first);
                        if (nc > 0) count = nc;
                    } else {
                        /* Dispatch by entry type (forest/linux/chainload boot;
                         * shell/recovery open in-place; reboot resets). */
                        audio_event(AUDIO_EV_SELECT);
                        int r = menu_activate(image, flat, anim_on);
                        if (r == MENU_REBOOT) return menu_exit(MENU_REBOOT);
                        if (r >= 0) return menu_exit(r);
                        /* MENU_HANDLED: stay in the menu; the shell may have
                         * mutated g_cfg, so re-sync the filtered view. */
                        count = menu_resync(labels, &sel);
                    }
                } else if (key.UnicodeChar == 'a' || key.UnicodeChar == 'A') {
                    open_about_window();
                } else if (key.UnicodeChar == 's' || key.UnicodeChar == 'S') {
                    audio_event(AUDIO_EV_OPEN);
                    tool_settings_open();   /* live customization editor */
                } else if (key.UnicodeChar == 'c' || key.UnicodeChar == 'C') {
                    /* Enter the interactive shell; it may mutate g_cfg. */
                    int flat = (sel < g_view_count) ? g_view[sel] : 0;
                    int r = menu_run_shell(image, anim_on, flat);
                    if (r == MENU_REBOOT) return menu_exit(MENU_REBOOT);
                    if (r >= 0) return menu_exit(r);
                    count = menu_resync(labels, &sel);
                }
                if (nav_moved) audio_event(AUDIO_EV_NAV);
            }
            /* ---- menu-level mouse ---- */
            if (mouse_on && ms.present) {
                /* row is only consumed on motion or a press; skip the full
                 * hit-test on idle-mouse frames (the common countdown case). */
                int row = (ms.moved || ms.left_pressed)
                        ? menu_hit_test(ms.x, ms.y, count) : -1;
                if (ms.moved && row >= 0) sel = row;      /* hover to highlight */
                if (ms.left_pressed) {
                    secs = -1;
                    if (row >= 0) {
                        if (row == sel) {             /* click selected = activate */
                            int flat = (sel < g_view_count) ? g_view[sel] : -1;
                            if (flat >= 0 &&
                                g_cfg.entries[flat].type == FOREB_ENTRY_SUBMENU) {
                                int nc = menu_go_down(labels, flat, &sel, &first);
                                if (nc > 0) count = nc;
                            } else {
                                int r = menu_activate(image, flat, anim_on);
                                if (r == MENU_REBOOT) return menu_exit(MENU_REBOOT);
                                if (r >= 0) return menu_exit(r);
                                count = menu_resync(labels, &sel);
                            }
                        } else {
                            sel = row;
                        }
                    }
                }

                /* ---- mouse wheel: free viewport scroll (selection may leave
                 * the viewport) ---- */
                if (ms.wheel) {
                    first -= ms.wheel;            /* wheel up scrolls to the top */
                    if (first > count - vis) first = count - vis;
                    if (first < 0) first = 0;
                    secs = -1;
                }

                /* ---- scrollbar thumb click / drag ---- *
                 * The thumb geometry is only needed to start or continue a
                 * drag, so skip the whole computation on idle frames. A drag in
                 * progress keeps ms.left (or dragging) set, so the release frame
                 * still enters here and clears dragging below. */
                if (ms.left || dragging) {
                    int tx, ty, tw, th, thy, thh;
                    if (ui_menu_scrollbar(count, &tx, &ty, &tw, &th, &thy, &thh)) {
                        if (ms.left_pressed &&
                            ms.x >= tx - 3 && ms.x < tx + tw + 3 &&
                            ms.y >= ty && ms.y < ty + th)
                            dragging = 1;
                        if (dragging && ms.left) {
                            int span = th - thh;
                            int rel  = ms.y - ty - thh / 2;
                            if (span > 0) {
                                first = rel * (count - vis) / span;
                                if (first > count - vis) first = count - vis;
                                if (first < 0) first = 0;
                            }
                            secs = -1;
                        }
                    }
                    if (!ms.left) dragging = 0;
                }
            }

            /* Keyboard navigation scrolls the viewport to keep the selection
             * visible. (Wheel/drag scroll the viewport WITHOUT following.) */
            if (nav_moved) {
                if (sel < first)              first = sel;
                else if (sel >= first + vis)  first = sel - vis + 1;
                if (first > count - vis) first = count - vis;
                if (first < 0) first = 0;
            }

            /* Smooth slide of the highlight bar when the selection moved via
             * the keyboard AND the viewport did not scroll (both rows on
             * screen). Scroll jumps skip the slide (they reframe the list). */
            if (nav_moved && sel != old_sel && first == old_first &&
                old_sel >= first && old_sel < first + vis &&
                sel >= first && sel < first + vis) {
                int y_from = etop_l + (old_sel - first) * eh_l;
                int y_to   = etop_l + (sel     - first) * eh_l;
                ui_menu_set_scroll(first);
                menu_slide(labels, count, sel, secs, anim_on,
                           cursor_on, &ms, y_from, y_to);
            }

            ui_menu_set_scroll(first);
        }

        /* Countdown tick (1 Hz). A ticked second is a visual change, so it
         * marks the frame dirty and the full recomposite below runs - cheap
         * enough at 1 Hz, and simpler than a partial countdown-only repaint. */
        if (secs >= 0 && elapsed_ms >= 1000) {
            elapsed_ms = 0;
            secs--;
            dirty = 1;
            /* Timeout: boot the RESOLVED default (flat) directly, no matter
             * which submenu level is being browsed. */
            if (secs < 0) {
                int d = g_cfg.default_idx;
                if (d < 0 || d >= g_cfg.count) d = 0;
                return menu_exit(d);
            }
        }

        /* ---- recomposite + flip ONLY when something visual changed -------- *
         * Idle iterations (no key, no pointer, no window, no particles, no
         * countdown tick) skip both the repaint and the ~4 MB ui_present(). */
        if (!dirty) continue;

        /* Rebuild the cached static scene ONLY when the menu itself changed;
         * otherwise restore it (one RAM copy, marks nothing) so a cursor- or
         * particle-only frame flips just those small spans - not the whole
         * panel - to uncached VRAM. g_bc_dirty (structural edits via
         * menu_resync) is cleared inside scene_build()->menu_draw_breadcrumb(). */
        int first = ui_menu_get_scroll();
        int menu_changed = !g_scene_valid || g_bc_dirty ||
                           sel   != prev_sel   || secs  != prev_secs  ||
                           count != prev_count || first != prev_first ||
                           g_cur_parent != prev_parent;
        if (menu_changed) {
            scene_build(labels, count, sel, secs);        /* bg+menu+crumb+icons+fx */
            prev_sel = sel; prev_secs = secs; prev_count = count;
            prev_first = ui_menu_get_scroll(); prev_parent = g_cur_parent;
        } else {
            scene_restore();                              /* cached scene, no re-raster */
        }
        /* Advance particles on a WALL-CLOCK cadence, not once per recomposite.
         * During pointer motion frames arrive faster (sub-frame early breaks),
         * so gate the step to a ~16 ms real-time accumulator - otherwise the
         * particle layer would visibly speed up whenever the mouse moves. */
        if (anim_on) {
            part_acc_ms += frame_ms;
            if (part_acc_ms >= 16) {
                part_acc_ms -= 16;
                anim_particles_step();
            }
        }
        wm_draw();                                        /* windows over the menu */
        if (cursor_on && ms.present) draw_cursor_live(&ms);
        ui_present();                                     /* single flip to VRAM   */
        g_menu_presents++;
    }
}

/* =============================================================================
 *  remember_last=1 support (UEFI variable "ForeBLastEntry")
 * ========================================================================== */

/* DESCEND RULE, mirroring config.c's resolution: while idx is a submenu row,
 * descend to its first child. Returns -1 on an empty submenu/corrupt links. */
static int foreb_descend_default(int idx)
{
    int guard = 0;
    while (idx >= 0 && idx < g_cfg.count &&
           g_cfg.entries[idx].type == FOREB_ENTRY_SUBMENU) {
        int child = -1;
        for (int i = 0; i < g_cfg.count; i++)
            if (g_cfg.entries[i].parent == idx) { child = i; break; }
        if (child < 0 || ++guard > FOREB_CFG_MAX_SUBMENU_DEPTH + 2) return -1;
        idx = child;
    }
    return idx;
}

/* After config load: when remember_last=1, read the ForeBLastEntry UEFI
 * variable (UINT32 flat index, NV|BS, vendor GUID gForeBLastEntryGuid) and,
 * if valid, use it as the default (with the DESCEND RULE). Any error keeps
 * the config default. */
static void remember_last_read(void)
{
    EFI_RUNTIME_SERVICES *rt;
    UINT32 val = 0, attr = 0;
    UINTN sz = sizeof(val);
    int idx;

    if (!g_cfg.remember_last || !gST || !gST->RuntimeServices) return;
    rt = gST->RuntimeServices;
    if (!rt->GetVariable) return;
    if (EFI_ERROR(rt->GetVariable(L"ForeBLastEntry",
                                  (EFI_GUID *)&gForeBLastEntryGuid,
                                  &attr, &sz, &val)))
        return;                                   /* not set / any error */
    if (sz != sizeof(val) || val >= (UINT32)g_cfg.count) return;
    idx = foreb_descend_default((int)val);
    if (idx < 0) return;                          /* empty submenu: keep cfg */
    g_cfg.default_idx = idx;
    serial_puts("[*] remember_last: default from ForeBLastEntry var, idx=");
    serial_puthex((UINT64)(UINT32)idx, 2); serial_puts("\n");
}

/* Immediately before dispatching a boot: when remember_last=1, persist the
 * chosen row's FLAT index for the next boot. Only real boot actions
 * (forest/linux/chainload) are remembered; shell/tools/recovery/setup/reboot
 * are not. Covers both manual Enter and countdown auto-boot. Non-fatal. */
static void remember_last_write(int flat)
{
    EFI_RUNTIME_SERVICES *rt;
    UINT32 val;
    int t;

    if (!g_cfg.remember_last || !gST || !gST->RuntimeServices) return;
    if (flat < 0 || flat >= g_cfg.count) return;
    t = g_cfg.entries[flat].type;
    if (t != FOREB_ENTRY_FOREST && t != FOREB_ENTRY_LINUX &&
        t != FOREB_ENTRY_CHAINLOAD) return;
    rt = gST->RuntimeServices;
    if (!rt->SetVariable) return;
    val = (UINT32)flat;
    rt->SetVariable(L"ForeBLastEntry", (EFI_GUID *)&gForeBLastEntryGuid,
                    FOREB_LAST_ENTRY_ATTRS, sizeof(val), &val);
}

/* =============================================================================
 * efi_main
 * ========================================================================== */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    settings_nv_init(SystemTable);   /* cache RT for durable Settings persistence */

    serial_init();
    /* No ConOut Reset/SetAttribute: we do not use the firmware text console at
     * all (its per-newline scroll on a hi-res GOP console is the slow-scroll
     * bug). Once GOP is up we clear the screen with a framebuffer fill instead. */

    logline(L"\r\n=== ForeB UEFI loader v2.0 ===\r\n",
            "\n=== ForeB UEFI loader v2.0 ===\n");
    logline(L"[*] Boot services online.\r\n", "[*] Boot services online.\n");

    /* ---- 1. Graphics Output Protocol (framebuffer) ---- */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    UINT64 fb_base = 0;
    UINT32 fb_w = 0, fb_h = 0, fb_pitch = 0, fb_bpp = 0, fb_type = FOREB_FB_RGB;
    UINT32 fb_pixfmt = PixelBlueGreenRedReserved8BitPerColor; /* x86 default BGRX */
    UINT8  have_fb = 0;

    EFI_STATUS st = gBS->LocateProtocol(&gGopGuid, NULL, (VOID **)&gop);
    if (!EFI_ERROR(st) && gop && gop->Mode && gop->Mode->Info) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
        fb_base  = (UINT64)gop->Mode->FrameBufferBase;
        fb_w     = mi->HorizontalResolution;
        fb_h     = mi->VerticalResolution;
        fb_bpp   = 32;
        fb_pitch = mi->PixelsPerScanLine * 4;
        fb_type  = FOREB_FB_RGB;
        fb_pixfmt = (UINT32)mi->PixelFormat;   /* honor RGBX vs BGRX byte order */
        have_fb  = 1;
        logline(L"[*] GOP framebuffer acquired.\r\n", "[*] GOP framebuffer: ");
        serial_puthex(fb_base, 16); serial_puts(" ");
        serial_puthex(fb_w, 4); serial_puts("x"); serial_puthex(fb_h, 4);
        serial_puts(" pitch="); serial_puthex(fb_pitch, 8);
        serial_puts(" pixfmt="); serial_puthex(fb_pixfmt, 2); serial_puts("\n");
    } else {
        logline(L"[!] No GOP; proceeding text-mode.\r\n", "[!] No GOP; proceeding text-mode.\n");
    }

    /* ---- 1b. Parse forebo.cfg + bring up the graphical animated menu ----
     * Config parsing, asset loads and the menu all run in the pre-GetMemoryMap
     * window while BootServices file IO is live. The menu draws only to the
     * linear framebuffer (no ConOut), so there is no console scrolling, and its
     * ConIn polling + Stall (neither allocates) cannot invalidate the map key. */
    forebo_config_load(ImageHandle, gBS, FOREB_CFG_ESP_PATH, &g_cfg);
    settings_nv_load(&g_cfg);   /* saved Settings/Theme override parsed config   */
    remember_last_read();       /* remember_last=1: ForeBLastEntry var override */
    serial_puts("[*] Config entries="); serial_puthex((UINT64)(UINT32)g_cfg.count, 2);
    serial_puts(" default="); serial_puthex((UINT64)(UINT32)g_cfg.default_idx, 2);
    serial_puts(" timeout="); serial_puthex((UINT64)(UINT32)g_cfg.timeout, 2);
    serial_puts("\n");

    int sel_entry = g_cfg.default_idx;
    if (sel_entry < 0 || sel_entry >= g_cfg.count) sel_entry = 0;

    if (have_fb) {
        ui_init(gBS, fb_base, fb_pitch, fb_w, fb_h, fb_pixfmt);
        /* Route the sibling draw modules into the SAME buffer ui.c draws to (the
         * off-screen back buffer when one was allocated, else VRAM). All three
         * now composite off-screen and a single ui_present() per frame flips to
         * the real VRAM front buffer (fb_base) - which is what we still hand the
         * kernel via the multiboot framebuffer fields below. */
        UINT64 draw_base  = ui_backbuffer_base();
        UINT32 draw_pitch = ui_draw_pitch();
        img_init(gBS, draw_base, draw_pitch, fb_w, fb_h, fb_pixfmt);
        anim_init(draw_base, draw_pitch, fb_w, fb_h, fb_pixfmt, gBS);
        serial_puts("[*] Double buffer: ");
        serial_puts(ui_double_buffered() ? "ON\n" : "OFF (direct VRAM)\n");
        preload_assets(ImageHandle);
        /* GUI Tools layer (Disk Info, GPT Viewer, ... via the Tools launcher).
         * NULL-safe; reuses the active config/theme + block IO / variable svcs. */
        tools_init(ImageHandle, gST, &g_cfg);
        tool_clone_init(ImageHandle, gST);      /* Clone Drive (diskio_init idempotent) */
        tool_undelete_init(ImageHandle, gST);   /* Undelete / Carve (diskio-backed)     */

        sel_entry = run_menu_animated(ImageHandle);
        serial_puts("[*] Menu selection="); serial_puthex((UINT64)(UINT32)sel_entry, 8);
        serial_puts("\n");
        if (sel_entry == MENU_REBOOT) {
            do_reset();          /* never returns */
        }
        if (sel_entry < 0 || sel_entry >= g_cfg.count) sel_entry = 0;

        /* Clean "loading" screen: background (image or forest) + status line. */
        draw_menu_background();
        ui_status("Loading...");
        ui_present();
    }

    /* ---- 2. Dispatch the selected entry by its config 'type' ----
     * The menu loop already handles shell/recovery in-place; here we handle the
     * launch types that leave the menu: reboot, Linux (EFI stub), chainload, and
     * the Forest multiboot handoff (which falls through to steps 3-11 below). */
    struct forebo_menuentry *ent = &g_cfg.entries[sel_entry];

    /* remember_last=1: persist the entry about to boot (manual selection AND
     * countdown auto-boot both pass through here). Non-fatal on failure. */
    remember_last_write(sel_entry);

    if (ent->type == FOREB_ENTRY_REBOOT) {
        serial_puts("[*] reboot entry selected -> resetting.\n");
        do_reset();                       /* never returns */
    }

    if (ent->type == FOREB_ENTRY_LINUX) {
        serial_puts("[*] Booting Linux (EFI stub): ");
        serial_puts(ent->vmlinuz[0] ? ent->vmlinuz : ent->kernel); serial_puts("\n");
        if (have_fb) {
            draw_menu_background(); ui_status("Booting Linux..."); ui_present();
            /* Fade to black before boot_linux does its own ExitBootServices. */
            anim_fade_out(12, 16);
        }
        boot_linux(ImageHandle, gBS, gST, ent);   /* returns only on failure */
        serial_puts("[x] Linux boot failed.\n");
        if (have_fb) { ui_status("Linux boot failed - halting"); ui_present(); }
        halt();
        return EFI_LOAD_ERROR;
    }

    if (ent->type == FOREB_ENTRY_CHAINLOAD) {
        serial_puts("[*] Chainloading EFI app: ");
        serial_puts(ent->chain[0] ? ent->chain : "(auto-scan volumes)"); serial_puts("\n");
        if (have_fb) {
            /* Every OS shows a staging bar + spinner before the fade, so the
             * chainload path (Windows/GRUB/other EFI apps) matches forest/linux.
             * StartImage does not return, so this is a cosmetic staging sweep. */
            char lbl[64];
            static const char pfx[] = "Starting ";
            int n = 0;
            while (pfx[n] && n < 63) { lbl[n] = pfx[n]; n++; }
            const char *t = ent->title;
            for (int j = 0; t[j] && n < 63; j++) lbl[n++] = t[j];
            lbl[n] = 0;
            draw_menu_background();
            anim_progress_reset();
            for (UINT64 p = 0; p <= 100; p += 10) anim_progress_to(lbl, p, 100, 1);
            ui_status(lbl);
            ui_present();
            /* Fade to black before chain.c's StartImage() hands off control. */
            anim_fade_out(12, 16);
        }
        chainload(ImageHandle, gBS, gST, ent);    /* returns only on failure */
        serial_puts("[x] Chainload failed.\n");
        if (have_fb) { ui_status("Chainload failed - halting"); ui_present(); }
        halt();
        return EFI_LOAD_ERROR;
    }

    if (ent->type == FOREB_ENTRY_FWSETUP) {
        /* Default-at-timeout firmware-setup entry: try the OsIndications flow,
         * then reset regardless (if unsupported, a plain reboot is the best we
         * can do without a live menu to report into). */
        serial_puts("[*] firmware-setup default at timeout -> OsIndications reboot.\n");
        fw_boot_to_setup(gST ? gST->RuntimeServices : NULL);  /* no return on success */
        do_reset();                       /* never returns (unsupported/error path) */
    }

    if (ent->type == FOREB_ENTRY_SHELL || ent->type == FOREB_ENTRY_RECOVERY ||
        ent->type == FOREB_ENTRY_TOOLS || ent->type == FOREB_ENTRY_SUBMENU ||
        ent->type == FOREB_ENTRY_SETTINGS) {
        /* Normally handled inside the menu; only reachable if such an entry is
         * the default and the countdown expires - nothing to boot, so reset.
         * (SUBMENU is unreachable: resolution descends submenu rows, but keep
         * the guard so a corrupt/edited config can never "boot" a submenu.) */
        serial_puts("[*] shell/recovery/tools/submenu default at timeout -> resetting.\n");
        do_reset();                       /* never returns */
    }

    /* ---- FOREST: Multiboot1 Forest-kernel handoff (x86_64 only) ---- */
#if !FOREB_MULTIBOOT_SUPPORTED
    foreb_multiboot_unsupported(gST);
    serial_puts("[x] Forest multiboot handoff is unsupported on this arch;\n"
                "    use a type=linux or type=chainload entry instead. Halting.\n");
    if (have_fb) { ui_status("Forest handoff unsupported on this arch"); ui_present(); }
    halt();
    return EFI_UNSUPPORTED;
#else
    if (have_fb) { ui_status("Loading Forest OS..."); ui_present(); }
    serial_puts("[*] Loading kernel: ");
    serial_puts(ent->kernel[0] ? ent->kernel : DEFAULT_KERNEL_PATH);
    serial_puts("\n");
    UINTN kfsize = 0;
    VOID *kbuf = load_kernel_entry(ImageHandle, ent->kernel, &kfsize, (int)have_fb);
    if (!kbuf) {
        logline(L"", "[x] Kernel not available; halting (bootloader-only image).\n");
        halt();
        return EFI_LOAD_ERROR;
    }
    serial_puts("[*] Kernel file loaded, size="); serial_puthex(kfsize, 8); serial_puts("\n");

    /* ---- 3. Parse ELF, discover entry + PT_LOAD extents ---- */
    UINT8 *e = (UINT8 *)kbuf;
    if (!(e[0] == ELF_MAG0 && e[1] == 'E' && e[2] == 'L' && e[3] == 'F')) {
        logline(L"[x] Bad ELF magic.\r\n", "[x] Bad ELF magic.\n");
        halt();
    }
    UINT8 eclass = e[EI_CLASS];
    UINT32 kentry = 0;
    UINT32 kis64  = (eclass == FOREB_ELFCLASS64) ? 1 : 0;
    UINT64 kmin = ~0ULL, kmax = 0;

    /* First pass: compute [kmin,kmax] of PT_LOAD paddr regions + entry. */
    UINT16 phnum, phentsize;
    UINT64 phoff;
    if (eclass == FOREB_ELFCLASS64) {
        Elf64_Ehdr *eh = (Elf64_Ehdr *)e;
        kentry = (UINT32)eh->e_entry;
        phnum = eh->e_phnum; phentsize = eh->e_phentsize; phoff = eh->e_phoff;
    } else {
        Elf32_Ehdr *eh = (Elf32_Ehdr *)e;
        kentry = eh->e_entry;
        phnum = eh->e_phnum; phentsize = eh->e_phentsize; phoff = eh->e_phoff;
    }

    for (UINT16 i = 0; i < phnum; i++) {
        UINT8 *ph = e + phoff + (UINT64)i * phentsize;
        UINT32 ptype; UINT64 ppaddr, pmemsz;
        if (eclass == FOREB_ELFCLASS64) {
            Elf64_Phdr *p = (Elf64_Phdr *)ph;
            ptype = p->p_type; ppaddr = p->p_paddr; pmemsz = p->p_memsz;
        } else {
            Elf32_Phdr *p = (Elf32_Phdr *)ph;
            ptype = p->p_type; ppaddr = p->p_paddr; pmemsz = p->p_memsz;
        }
        if (ptype != PT_LOAD_ || pmemsz == 0) continue;
        if (ppaddr < kmin) kmin = ppaddr;
        if (ppaddr + pmemsz > kmax) kmax = ppaddr + pmemsz;
    }
    if (kmin == ~0ULL) { logline(L"[x] No PT_LOAD.\r\n", "[x] No PT_LOAD.\n"); halt(); }

    serial_puts("[*] ELF class="); serial_puthex(eclass, 2);
    serial_puts(" entry="); serial_puthex(kentry, 8);
    serial_puts(" load=["); serial_puthex(kmin, 8); serial_puts(",");
    serial_puthex(kmax, 8); serial_puts(")\n");

    /* ---- 4. Reserve physical memory that must survive ExitBootServices ---- */
    /* (a) Kernel PT_LOAD destination range. */
    EFI_PHYSICAL_ADDRESS kdst = PAGE_DOWN(kmin);
    UINTN kpages = PAGES_FOR(kmax - kdst);
    st = gBS->AllocatePages(AllocateAddress, EfiLoaderData, kpages, &kdst);
    if (EFI_ERROR(st)) {
        /* Non-fatal: OVMF usually leaves >=1MiB conventional; we still own it
         * post-ExitBootServices. Log and continue. */
        serial_puts("[!] AllocateAddress(kernel dst) rc="); serial_puthex(st, 16);
        serial_puts(" (continuing)\n");
    }
    /* (b) Fixed ForeB low-RAM region 0x1000..0x7FFF (structs + strings). */
    EFI_PHYSICAL_ADDRESS lowbase = 0x1000;
    st = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 7, &lowbase);
    if (EFI_ERROR(st)) {
        serial_puts("[!] AllocateAddress(low structs) rc="); serial_puthex(st, 16);
        serial_puts(" (continuing)\n");
    }

    /* ---- 5. cmdline/loader strings + multiboot structs (base) + modules ----
     * The multiboot_info (0x1800) and foreboots_boot_info (0x1000) BASE fields
     * are built HERE, before ExitBootServices, so modules_load() (which needs
     * live BootServices to read module files off the ESP) can register the
     * mb_module[] array into them. The memory-map-dependent fields (mem_upper,
     * mmap_addr/length) are filled in AFTER ExitBootServices - WITHOUT a second
     * memset that would wipe the module wiring. */
    char *cmdline = (char *)0x2000;
    char *ldrname = (char *)0x2040;
    {
        const char *cl = ent->cmdline;   /* per-entry kernel command line */
        UINTN i = 0; for (; cl[i] && i < 0x30; i++) cmdline[i] = cl[i]; cmdline[i] = '\0';

        const char *n = "ForeB";
        i = 0; for (; n[i]; i++) ldrname[i] = n[i]; ldrname[i] = '\0';
    }

    /* multiboot_info base (the struct the kernel reads via EBX). */
    struct multiboot_info *mbi =
        (struct multiboot_info *)(UINTN)FOREB_MULTIBOOT_INFO_ADDR;
    memset(mbi, 0, sizeof(*mbi));
    mbi->flags = MB_FLAG_MEM | MB_FLAG_CMDLINE | MB_FLAG_BOOTLOADER;
    mbi->mem_lower = 640;
    mbi->boot_device = 0x80;
    mbi->cmdline = (UINT32)(UINTN)cmdline;
    mbi->boot_loader_name = (UINT32)(UINTN)ldrname;
    if (have_fb) {
        mbi->flags |= MB_FLAG_FRAMEBUFFER;
        mbi->framebuffer_addr = fb_base;
        mbi->framebuffer_pitch = fb_pitch;
        mbi->framebuffer_width = fb_w;
        mbi->framebuffer_height = fb_h;
        mbi->framebuffer_bpp = (UINT8)fb_bpp;
        mbi->framebuffer_type = (UINT8)fb_type;
    }

    /* foreboots_boot_info base (forward-looking rich info). */
    struct foreboots_boot_info *bi =
        (struct foreboots_boot_info *)(UINTN)FOREB_BOOT_INFO_ADDRESS;
    memset(bi, 0, sizeof(*bi));
    bi->magic = FOREB_BOOT_INFO_MAGIC;
    bi->version = FOREB_BOOT_INFO_VER;
    bi->flags = FOREB_BIF_CMDLINE | FOREB_BIF_LONG_MODE | FOREB_BIF_CPUID |
                FOREB_BIF_PAE | FOREB_BIF_KERNEL_PRELOADED;
    bi->boot_disk = 0x80;
    bi->cmdline = (UINT32)(UINTN)cmdline;
    bi->boot_loader_name = (UINT32)(UINTN)ldrname;
    bi->mem_lower = 640;
    if (have_fb) {
        bi->flags |= FOREB_BIF_FRAMEBUFFER;
        bi->framebuffer_addr = fb_base;
        bi->framebuffer_pitch = fb_pitch;
        bi->framebuffer_width = fb_w;
        bi->framebuffer_height = fb_h;
        bi->framebuffer_bpp = fb_bpp;
        bi->framebuffer_type = fb_type;
    }
    bi->cpuid_available = 1;
    bi->long_mode_available = 1;
    bi->pae_available = 1;
    bi->kernel_load_addr = (UINT32)(UINTN)kbuf;
    bi->kernel_size = (UINT32)kfsize;
    bi->kernel_entry = kentry;
    bi->kernel_is64bit = kis64;
    bi->boot_entry = (UINT32)sel_entry;

    /* Load configured modules (initrd/...) into memory that survives EBS and
     * wire them into mbi (MB_FLAG_MODS/mods_*) + bi (initrd_*). No-op when the
     * entry lists no modules. Must run while BootServices are still live. */
    if (ent->module_count > 0) {
        EFI_STATUS mst = modules_load(ImageHandle, gBS, ent, mbi, bi);
        serial_puts("[*] modules_load rc="); serial_puthex((UINT64)mst, 16);
        serial_puts(" mods_count="); serial_puthex((UINT64)mbi->mods_count, 2);
        serial_puts("\n");
    }

    /* ---- 6. Final GetMemoryMap + ExitBootServices ----
     * CRITICAL: firmware calls that allocate memory (including console output
     * via ConOut) between GetMemoryMap and ExitBootServices invalidate the map
     * key and make ExitBootServices return EFI_INVALID_PARAMETER. So we do ALL
     * chatty logging BEFORE the loop, over-provision the map buffer once, then
     * spin GetMemoryMap -> ExitBootServices with NOTHING but port-I/O serial
     * (which never touches firmware memory) in between. */
    logline(L"[*] Retrieving EFI memory map + ExitBootServices ...\r\n",
            "[*] Retrieving EFI memory map + ExitBootServices ...\n");

    /* Final on-screen status (pure framebuffer writes -- no firmware alloc, so
     * this does NOT jeopardize the upcoming GetMemoryMap key). */
    if (have_fb) {
        anim_progress_to("Loading kernel", 1, 1, 1);   /* eased snap to 100% (pre-EBS); self-presents */
        ui_status("Starting Forest OS...");
        ui_present();
        /* Smooth fade to black -- the last thing the user sees before the kernel
         * takes over. Paced with BootServices Stall (valid pre-ExitBootServices);
         * runs before the GetMemoryMap loop so nothing allocates afterward. */
        anim_fade_out(12, 16);
    }

    UINTN mapsz = 0, mapkey = 0, descsz = 0, mapcap = 0;
    UINT32 descver = 0;
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;

    /* Size the buffer, then allocate with generous headroom (the AllocatePool
     * itself, plus any fragmentation during the loop, can add descriptors). */
    st = gBS->GetMemoryMap(&mapsz, mmap, &mapkey, &descsz, &descver);
    mapcap = mapsz + 16 * (descsz ? descsz : sizeof(EFI_MEMORY_DESCRIPTOR));
    st = gBS->AllocatePool(EfiLoaderData, mapcap, (VOID **)&mmap);
    if (EFI_ERROR(st) || !mmap) {
        logline(L"[x] AllocatePool(memmap) failed.\r\n", "[x] AllocatePool(memmap) failed.\n");
        halt();
    }

    st = EFI_INVALID_PARAMETER;
    for (int attempt = 0; attempt < 16; attempt++) {
        mapsz = mapcap;  /* reset to full capacity each iteration */
        st = gBS->GetMemoryMap(&mapsz, mmap, &mapkey, &descsz, &descver);
        if (EFI_ERROR(st)) continue;              /* buffer too small etc. */
        st = gBS->ExitBootServices(ImageHandle, mapkey);
        if (!EFI_ERROR(st)) break;                /* success -> firmware gone */
        /* else: map key went stale; loop and retry with a fresh map. */
    }
    if (EFI_ERROR(st)) {
        serial_puts("[x] ExitBootServices failed rc="); serial_puthex(st, 16); serial_puts("\n");
        halt();
    }

    /* ================= FIRMWARE IS GONE - serial port only ================= */
    serial_puts("[*] ExitBootServices OK - firmware released.\n");

    /* ---- 7. Copy PT_LOAD segments to their p_paddr, zero BSS tails ----
     * Firmware (gBS/gop service calls, ConOut) is gone, but the linear
     * framebuffer memory is still writable, so ui_progress()/ui_status() -- which
     * are pure MMIO stores -- remain valid for the staging bar. */
    if (have_fb) { ui_status("Staging kernel segments..."); ui_present(); }
    for (UINT16 i = 0; i < phnum; i++) {
        UINT8 *ph = e + phoff + (UINT64)i * phentsize;
        UINT32 ptype; UINT64 poff, ppaddr, pfilesz, pmemsz;
        if (eclass == FOREB_ELFCLASS64) {
            Elf64_Phdr *p = (Elf64_Phdr *)ph;
            ptype = p->p_type; poff = p->p_offset; ppaddr = p->p_paddr;
            pfilesz = p->p_filesz; pmemsz = p->p_memsz;
        } else {
            Elf32_Phdr *p = (Elf32_Phdr *)ph;
            ptype = p->p_type; poff = p->p_offset; ppaddr = p->p_paddr;
            pfilesz = p->p_filesz; pmemsz = p->p_memsz;
        }
        if (ptype != PT_LOAD_ || pmemsz == 0) continue;
        memcpy((void *)(UINTN)ppaddr, e + poff, (UINTN)pfilesz);
        if (pmemsz > pfilesz)
            memset((void *)(UINTN)(ppaddr + pfilesz), 0, (UINTN)(pmemsz - pfilesz));
        if (have_fb) {
            ui_progress("Staging kernel", (UINT64)(i + 1), (UINT64)phnum);
            anim_load_spinner((int)i);   /* pure MMIO spinner, post-EBS safe */
            ui_present();                /* post-EBS memcpy flip (no BootServices) */
        }
    }
    if (have_fb) { ui_status("Starting Forest OS..."); ui_present(); }
    serial_puts("[*] Kernel PT_LOAD segments staged.\n");

    /* ---- 8. Build E820 arrays: foreboots_mmap (0x1100) + mb_mmap (0x1400) --- */
    struct foreboots_mmap_entry *fmap = (struct foreboots_mmap_entry *)(UINTN)FOREB_MMAP_ADDRESS;
    struct mb_mmap_entry        *mbm  = (struct mb_mmap_entry *)(UINTN)FOREB_MB_MMAP_ADDRESS;

    UINTN ndesc = mapsz / descsz;
    UINT32 count = 0;
    UINT64 mem_upper_kib = 0;
    for (UINTN i = 0; i < ndesc && count < FOREB_MMAP_MAX; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + i * descsz);
        UINT64 base = d->PhysicalStart;
        UINT64 len  = d->NumberOfPages * (UINT64)EFI_PAGE_SIZE;
        UINT32 type = efi_to_e820(d->Type);

        fmap[count].base = base;
        fmap[count].length = len;
        fmap[count].type = type;
        fmap[count].acpi = 1;

        mbm[count].size     = 20;
        mbm[count].addr_low = (UINT32)base;
        mbm[count].addr_high = (UINT32)(base >> 32);
        mbm[count].len_low  = (UINT32)len;
        mbm[count].len_high = (UINT32)(len >> 32);
        mbm[count].type     = type;

        if (type == FOREB_E820_USABLE && base >= 0x100000)
            mem_upper_kib += len / 1024;

        count++;
    }
    serial_puts("[*] E820 entries="); serial_puthex(count, 4);
    serial_puts(" mem_upper_KiB="); serial_puthex(mem_upper_kib, 8); serial_puts("\n");

    /* ---- 9. Fill the memory-map-dependent multiboot_info fields (the base
     * fields + module wiring were already built before ExitBootServices). ---- */
    mbi->flags |= MB_FLAG_MMAP;
    mbi->mem_upper = (UINT32)mem_upper_kib;
    mbi->mmap_length = count * (UINT32)sizeof(struct mb_mmap_entry);
    mbi->mmap_addr = FOREB_MB_MMAP_ADDRESS;

    /* ---- 10. Fill the memory-map-dependent foreboots_boot_info fields (the
     * base fields + module initrd wiring were built before ExitBootServices). */
    bi->flags |= FOREB_BIF_MMAP;
    bi->mem_upper = (UINT32)mem_upper_kib;
    bi->mmap_count = count;
    bi->mmap_addr = FOREB_MMAP_ADDRESS;

    serial_puts("[*] Structures built. Handoff -> entry=");
    serial_puthex(kentry, 8);
    serial_puts(" EBX="); serial_puthex(FOREB_MULTIBOOT_INFO_ADDR, 8);
    serial_puts(" EAX="); serial_puthex(MULTIBOOT1_MAGIC, 8); serial_puts("\n");
    serial_puts("[*] Tearing down long mode -> 32-bit PM. Jumping to kernel.\n");

    /* ---- 11. Long mode -> 32-bit PM -> jump to kernel (never returns) ---- */
#if FOREB_MULTIBOOT_SUPPORTED
    forebo_handoff(kentry, MULTIBOOT1_MAGIC, FOREB_MULTIBOOT_INFO_ADDR);
#else
    /* Non-x86 UEFI: the Forest multiboot1 handoff does not exist. This code is
     * unreachable at runtime (Forest entries are not offered when
     * FOREB_MULTIBOOT_SUPPORTED==0), but must still link. */
    foreb_multiboot_unsupported(gST);
    return EFI_UNSUPPORTED;
#endif

    /* Unreachable. */
    serial_puts("[x] handoff returned?!\n");
    halt();
    return EFI_LOAD_ERROR;
#endif /* FOREST path (FOREB_MULTIBOOT_SUPPORTED) */
}

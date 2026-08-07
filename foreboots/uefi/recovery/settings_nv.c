/*
 * settings_nv.c - Durable UEFI-NV persistence for the Settings/Theme tool.
 *
 * Mirrors the bootx64.c remember_last_read/remember_last_write pattern
 * (GetVariable/SetVariable on a private ForeB vendor GUID, NV attributes) but
 * carries a small VERSIONED, packed blob of the user-tunable UI settings the
 * Settings tool mutates instead of a single UINT32. Freestanding, no libc,
 * no allocation; every pointer is guarded.
 */
#include "efi.h"
#include "efi_ext.h"
#include "forebo_cfg.h"
#include "settings_nv.h"

/* --------------------------------------------------------------------------
 * Optional serial debug (local putc, same idiom as chainload.c). Silent by
 * default; flip SNV_DEBUG to 1 to trace saves/loads over COM1.
 * ------------------------------------------------------------------------ */
#define SNV_DEBUG 0
#if SNV_DEBUG
static void snv_outb(UINT16 port, UINT8 val)
{ __asm__ __volatile__("outb %0,%1" : : "a"(val), "Nd"(port)); }
static void snv_puts(const char *s)
{ if(!s) return; while(*s){ if(*s=='\n') snv_outb(0x3F8,'\r'); snv_outb(0x3F8,(UINT8)*s++); } }
#else
static void snv_puts(const char *s) { (void)s; }
#endif

/* --------------------------------------------------------------------------
 * Variable identity. Dedicated vendor GUID, DISTINCT from gForeBLastEntryGuid
 * {46524542-4F4F-5442-8001-466F72654231}. Here node 4 differs ('S'ettings):
 *   {46524542-4F4F-5442-8002-466F72655354}  ("FREB-OO-TB-..-'ForeST'")
 * ------------------------------------------------------------------------ */
static const EFI_GUID gForeBSettingsGuid = {
    0x46524542u, 0x4F4Fu, 0x5442u,
    { 0x80, 0x02, 'F', 'o', 'r', 'e', 'S', 'T' }
};

#define SNV_VAR_NAME  L"ForeBSettings"
#define SNV_ATTRS     (EFI_VARIABLE_NON_VOLATILE |    \
                       EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                       EFI_VARIABLE_RUNTIME_ACCESS)

#define SNV_MAGIC    0x53424546u   /* 'F','E','B','S' little-endian */
#define SNV_VERSION  1u

/*
 * The persisted, packed blob. Only the fields the Settings tool actually lets
 * the user change are stored (theme colours + boolean/enum toggles), plus the
 * UI font scale. Fixed-width members keep the on-media layout stable; `size`
 * is checked on load so any future layout change is rejected as stale rather
 * than misread. Bump SNV_VERSION whenever this layout changes.
 */
#pragma pack(push, 1)
typedef struct {
    UINT32 magic;      /* SNV_MAGIC                                        */
    UINT32 version;    /* SNV_VERSION                                      */
    UINT32 size;       /* sizeof(snv_blob) guard                          */
    /* theme colours (0x00RRGGBB, or FOREB_COLOR_UNSET) */
    UINT32 color_bg;
    UINT32 color_fg;
    UINT32 color_accent;
    UINT32 color_sel_bg;
    UINT32 color_sel_fg;
    UINT32 color_titlebar;
    UINT32 color_window;
    UINT32 color_cursor;
    /* toggles / enums the Settings tool flips */
    INT32  cursor_enabled;
    INT32  mouse_enabled;
    INT32  animations_enabled;
    INT32  double_buffer;
    INT32  window_skin;        /* enum forebo_window_skin */
    INT32  ui_font_scale;      /* theme.widget.font_scale (percent, 100=1x) */
} snv_blob;
#pragma pack(pop)

/* --------------------------------------------------------------------------
 * Cached system table. This codebase has no ambient gST global; every module
 * (diskio_init, img_init, ...) caches the table from the caller once. We do the
 * same: the integrator calls settings_nv_init(SystemTable) early in efi_main.
 * If init was never called both entry points no-op safely.
 * ------------------------------------------------------------------------ */
static EFI_SYSTEM_TABLE *gST;

void settings_nv_init(void *st)
{
    gST = (EFI_SYSTEM_TABLE *)st;
}

/* Resolve RuntimeServices, or NULL when unavailable. */
static EFI_RUNTIME_SERVICES *snv_rt(void)
{
    if(!gST || !gST->RuntimeServices) return 0;
    return gST->RuntimeServices;
}

void settings_nv_save(const struct forebo_config *cfg)
{
    EFI_RUNTIME_SERVICES *rt;
    const struct forebo_theme *t;
    snv_blob b;

    if(!cfg) return;
    rt = snv_rt();
    if(!rt || !rt->SetVariable) { snv_puts("[settings_nv] save: no RT\n"); return; }
    t = &cfg->theme;

    b.magic   = SNV_MAGIC;
    b.version = SNV_VERSION;
    b.size    = (UINT32)sizeof(b);
    b.color_bg       = (UINT32)t->color_bg;
    b.color_fg       = (UINT32)t->color_fg;
    b.color_accent   = (UINT32)t->color_accent;
    b.color_sel_bg   = (UINT32)t->color_sel_bg;
    b.color_sel_fg   = (UINT32)t->color_sel_fg;
    b.color_titlebar = (UINT32)t->color_titlebar;
    b.color_window   = (UINT32)t->color_window;
    b.color_cursor   = (UINT32)t->color_cursor;
    b.cursor_enabled     = (INT32)t->cursor_enabled;
    b.mouse_enabled      = (INT32)t->mouse_enabled;
    b.animations_enabled = (INT32)t->animations_enabled;
    b.double_buffer      = (INT32)t->double_buffer;
    b.window_skin        = (INT32)t->window_skin;
    b.ui_font_scale      = (INT32)t->widget.font_scale;

    rt->SetVariable(SNV_VAR_NAME, (EFI_GUID *)&gForeBSettingsGuid,
                    SNV_ATTRS, sizeof(b), &b);
    snv_puts("[settings_nv] saved\n");
}

void settings_nv_load(struct forebo_config *cfg)
{
    EFI_RUNTIME_SERVICES *rt;
    struct forebo_theme *t;
    snv_blob b;
    UINTN sz = sizeof(b);
    UINT32 attr = 0;

    if(!cfg) return;
    rt = snv_rt();
    if(!rt || !rt->GetVariable) { snv_puts("[settings_nv] load: no RT\n"); return; }

    if(EFI_ERROR(rt->GetVariable(SNV_VAR_NAME, (EFI_GUID *)&gForeBSettingsGuid,
                                 &attr, &sz, &b)))
        return;                                   /* not set / any error */

    /* Version + size guards: ignore any stale/foreign blob. */
    if(sz != sizeof(b) || b.magic != SNV_MAGIC ||
       b.version != SNV_VERSION || b.size != (UINT32)sizeof(b)) {
        snv_puts("[settings_nv] load: stale blob ignored\n");
        return;
    }

    t = &cfg->theme;
    t->color_bg       = (unsigned int)b.color_bg;
    t->color_fg       = (unsigned int)b.color_fg;
    t->color_accent   = (unsigned int)b.color_accent;
    t->color_sel_bg   = (unsigned int)b.color_sel_bg;
    t->color_sel_fg   = (unsigned int)b.color_sel_fg;
    t->color_titlebar = (unsigned int)b.color_titlebar;
    t->color_window   = (unsigned int)b.color_window;
    t->color_cursor   = (unsigned int)b.color_cursor;
    t->cursor_enabled     = (int)b.cursor_enabled;
    t->mouse_enabled      = (int)b.mouse_enabled;
    t->animations_enabled = (int)b.animations_enabled;
    t->double_buffer      = (int)b.double_buffer;
    t->window_skin        = (int)b.window_skin;
    t->widget.font_scale  = (int)b.ui_font_scale;
    snv_puts("[settings_nv] applied saved settings\n");
}

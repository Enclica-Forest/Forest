/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools.c - Windowed GUI tool registry + the ~11 tools + launcher.
 * =============================================================================
 * Implements the contract in tools.h. Every tool is a wm.c window ("template B":
 * open() calls wm_open() and returns immediately; the existing menu loop in
 * bootx64.c drives input + compositing + present). State lives in per-tool static
 * structs reached from the callbacks via wm_user(). Data is gathered ONCE on open
 * (and on user interaction) into a scrollable line buffer; the per-frame draw
 * callback only renders - it performs no file / variable / block IO.
 *
 * Shared list model
 * -----------------
 * Most tools are a scrollable list of coloured text lines (the disk table, GPT
 * table, memory map, variables, boot entries, sysinfo, key log). They share:
 *   - a line composer (LN* -> tl_begin/tl_end) that appends into a caller's
 *     text[][]/col[]/n arrays (identical layout in tstate + fbstate),
 *   - render_list() (viewport + selection highlight + scrollbar),
 *   - list geometry helpers (L_rows / L_yoff) so the draw callback and the
 *     mouse hit-test agree on row positions.
 * The File Browser, Hex Viewer and Theme/Settings tools carry extra structure and
 * add their own interaction on top of the shared renderer.
 *
 * Freestanding (no libc), pre-ExitBootServices. Fixed pools; heap only via
 * BootServices AllocatePool (hex/memmap/variable value buffers), freed on close.
 * ========================================================================== */

#include "tools.h"
#include "tools_cat.h"
#include "../efi.h"
#include "../efi_ext.h"
#include "../core/wm.h"
#include "../ui.h"
#include "../core/input.h"
#include "../core/image.h"
#include "../standalone/imgview.h"
#include "../recovery/clone.h"
#include "../recovery/undelete.h"
#include "../standalone/calc.h"
#include "../standalone/clock.h"
#include "../standalone/sysmon.h"
#include "../recovery/settings_nv.h"
#include "../core/config.h"
#include "../../include/forebo_cfg.h"
#include "../../include/forebo_theme.h"

/* ==========================================================================
 * Module state (captured at tools_init).
 * ========================================================================== */
static EFI_HANDLE            gImage;
static EFI_SYSTEM_TABLE     *gST;
static EFI_BOOT_SERVICES    *gBS;
static EFI_RUNTIME_SERVICES *gRT;
static struct forebo_config *gCfg;

/* Resolved theme colours (recomputed by resolve_theme() incl. after settings). */
static UINT32 c_win, c_fg, c_dim, c_accent, c_sel_bg, c_sel_fg, c_border, c_warn;

static EFI_GUID gBlkGuid  = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID gSfsGuid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gGopGuid  = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID gFinfoGuid = EFI_FILE_INFO_ID;
static EFI_GUID gGlobalVar = EFI_GLOBAL_VARIABLE;

static UINT32 pick(unsigned int v, UINT32 def)
{ return (v == FOREB_COLOR_UNSET) ? def : (UINT32)v; }

static void resolve_theme(void)
{
    const struct forebo_theme *t = gCfg ? &gCfg->theme : 0;
    c_win    = t ? pick(t->color_window, FOREB_PANEL)  : FOREB_PANEL;
    c_fg     = t ? pick(t->color_fg,     FOREB_TEXT)   : FOREB_TEXT;
    c_accent = t ? pick(t->color_accent, FOREB_TITLE)  : FOREB_TITLE;
    c_sel_bg = t ? pick(t->color_sel_bg, FOREB_SELECT) : FOREB_SELECT;
    c_sel_fg = t ? pick(t->color_sel_fg, FOREB_WHITE)  : FOREB_WHITE;
    c_dim    = FOREB_DIM;
    c_border = FOREB_BORDER;
    c_warn   = FOREB_TIMER;
}

void tools_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, struct forebo_config *cfg)
{
    gImage = image;
    gST    = st;
    gBS    = st ? st->BootServices : 0;
    gRT    = st ? st->RuntimeServices : 0;
    gCfg   = cfg;
    tool_imgview_init(image, st);   /* image viewer needs image handle + ESP */
    clock_init(st);                 /* Clock needs RuntimeServices->GetTime   */
    tool_sysmon_init(st);           /* System Monitor caches gST/gBS/gRT+diskio*/
    tools_categories_init(st);      /* patch forebo_categories[0] + cat inits  */
    resolve_theme();
}

/* ==========================================================================
 * Tiny freestanding helpers.
 * ========================================================================== */
static int  slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }
static void scopy(char *d, const char *s, int cap)
{ int i=0; if(cap<=0)return; for(;s&&s[i]&&i+1<cap;i++)d[i]=s[i]; d[i]=0; }
static int  mem_eq(const UINT8 *a, const UINT8 *b, int n)
{ for(int i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static int  ci_eq(const char *a, const char *b)  /* case-insensitive, both NUL */
{ for(;*a&&*b;a++,b++){ char x=*a,y=*b; if(x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32; if(x!=y)return 0; } return *a==*b; }
static void u2a(const CHAR16 *u, char *a, int cap)
{ int i=0; for(;u&&u[i]&&i+1<cap;i++){ CHAR16 c=u[i]; a[i]=(c>=0x20&&c<0x7f)?(char)c:'?'; } a[i]=0; }

static UINT32 rd_u32(const UINT8 *p)
{ return (UINT32)p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24); }
static UINT64 rd_u64(const UINT8 *p)
{ return (UINT64)rd_u32(p)|((UINT64)rd_u32(p+4)<<32); }
static UINT16 rd_u16(const UINT8 *p){ return (UINT16)(p[0]|((UINT16)p[1]<<8)); }

static int gsc(void){ int s=ui_scale(); return s<1?1:s; }

/* ==========================================================================
 * Shared list model + line composer.
 * ========================================================================== */
#define TL_MAXLINES 200
#define TL_COLS     100

/* Composer target (set by tl_begin before LN* calls). */
static char  (*g_txt)[TL_COLS];
static UINT32 *g_colr;
static int    *g_np;
static int     g_cap;

/* Current line accumulator. */
static char   g_cl[TL_COLS];
static int    g_cll;
static UINT32 g_clcol;

static void tl_begin(char (*txt)[TL_COLS], UINT32 *colr, int *np, int cap)
{ g_txt=txt; g_colr=colr; g_np=np; g_cap=cap; *np=0; }
static void LN0(UINT32 c){ g_cll=0; g_cl[0]=0; g_clcol=c; }
static void LNc(char c){ if(g_cll<TL_COLS-1){ g_cl[g_cll++]=c; g_cl[g_cll]=0; } }
static void LNs(const char *s){ while(s&&*s) LNc(*s++); }
static void LNsp(int n){ while(n-->0) LNc(' '); }
static void LNu(UINT64 v)
{ char t[24]; int i=0; if(!v){LNc('0');return;} while(v){t[i++]=(char)('0'+(int)(v%10));v/=10;} while(i)LNc(t[--i]); }
static void LNx(UINT64 v, int width)  /* zero-padded hex, `width` nibbles (0=min) */
{ static const char h[]="0123456789ABCDEF"; char t[16]; int i=0;
  if(!v){ t[i++]='0'; } while(v){ t[i++]=h[v&0xF]; v>>=4; }
  while(i<width) t[i++]='0'; while(i) LNc(t[--i]); }
static void LNhex2(UINT8 b){ static const char h[]="0123456789ABCDEF"; LNc(h[b>>4]); LNc(h[b&0xF]); }
static void LNend(void)
{ if(*g_np>=g_cap) return; int i=0; for(;g_cl[i]&&i<TL_COLS-1;i++) g_txt[*g_np][i]=g_cl[i];
  g_txt[*g_np][i]=0; g_colr[*g_np]=g_clcol; (*g_np)++; }

/* ---- list geometry (shared by render + hit-test) ---- */
static int L_lineH(void){ return 16*gsc(); }
static int L_yoff(int header){ int sc=gsc(); return 4*sc + (header ? (16*sc+3*sc) : 0); }
static int L_rows(int ch, int header)
{ int sc=gsc(); int avail=ch - L_yoff(header) - 4*sc; int r=avail/L_lineH(); return r<1?1:r; }
static int L_cols(int cw){ int c=(cw-14*gsc())/(8*gsc()); if(c<1)c=1; if(c>TL_COLS-1)c=TL_COLS-1; return c; }

/* Row index under client-relative my, or -1. */
static int L_row_at(int my, int scroll, int n, int rows, int header)
{ int y=L_yoff(header); if(my<y) return -1; int r=(my-y)/L_lineH();
  if(r<0||r>=rows) return -1; int idx=scroll+r; return (idx<n)?idx:-1; }

/*
 * Render a scrollable coloured-line list into a window client rect. Clamps
 * *pscroll. When selectable, keeps `sel` in view and paints the highlight bar.
 * Draws a right-edge scrollbar when the content overflows.
 */
static void render_list(int cx, int cy, int cw, int ch, const char *header,
                        char (*txt)[TL_COLS], UINT32 *colr, int n,
                        int *pscroll, int sel, int selectable)
{
    int sc=gsc(), lineH=L_lineH();
    int x=cx+6*sc, y=cy+4*sc;

    if (header) { draw_string(x, y, header, c_accent, c_win, 1, sc); y += lineH+3*sc; }

    int rows=L_rows(ch, header?1:0);
    int cols=L_cols(cw);
    int scroll=*pscroll;

    /* Keep the selection visible (selectable lists follow sel). */
    if (selectable && sel>=0) {
        if (sel<scroll)          scroll=sel;
        else if (sel>=scroll+rows) scroll=sel-rows+1;
    }
    if (scroll>n-rows) scroll=n-rows;
    if (scroll<0) scroll=0;
    *pscroll=scroll;

    int barw = (n>rows) ? 6*sc : 0;
    for (int r=0; r<rows; r++) {
        int idx=scroll+r; if(idx>=n) break;
        int ry=y+r*lineH;
        UINT32 fg=colr[idx];
        if (selectable && idx==sel) {
            fill_rect(x-3*sc, ry-1, cw-10*sc-barw, lineH, c_sel_bg);
            fg=c_sel_fg;
        }
        char line[TL_COLS]; int j=0;
        for(; txt[idx][j] && j<cols; j++) line[j]=txt[idx][j];
        line[j]=0;
        draw_string(x, ry, line, fg, c_win, 1, sc);
    }

    /* Scrollbar. */
    if (barw) {
        int trackX=cx+cw-barw-2*sc, trackY=y, trackH=rows*lineH;
        fill_rect(trackX, trackY, barw, trackH, c_border);
        int thumbH=rows*trackH/n; if(thumbH<8*sc)thumbH=8*sc;
        int thumbY=trackY + (n>rows ? scroll*(trackH-thumbH)/(n-rows) : 0);
        fill_rect(trackX, thumbY, barw, thumbH, c_accent);
    }
}

/* Generic keyboard scroll/nav shared by list tools. Returns 1 if it consumed the
 * key. `selectable` moves sel; otherwise moves scroll. rows = visible rows. */
static int L_key_nav(UINT16 scan, int *pscroll, int *psel, int n, int rows,
                     int selectable)
{
    int page=rows-1; if(page<1)page=1;
    if (selectable) {
        switch (scan) {
            case SCAN_UP:        if(*psel>0)(*psel)--; return 1;
            case SCAN_DOWN:      if(*psel<n-1)(*psel)++; return 1;
            case SCAN_PAGE_UP:   *psel-=page; if(*psel<0)*psel=0; return 1;
            case SCAN_PAGE_DOWN: *psel+=page; if(*psel>n-1)*psel=n-1; return 1;
            case SCAN_HOME:      *psel=0; return 1;
            case SCAN_END:       *psel=n-1; if(*psel<0)*psel=0; return 1;
            default: return 0;
        }
    }
    switch (scan) {
        case SCAN_UP:        if(*pscroll>0)(*pscroll)--; return 1;
        case SCAN_DOWN:      if(*pscroll<n-rows)(*pscroll)++; return 1;
        case SCAN_PAGE_UP:   *pscroll-=page; return 1;
        case SCAN_PAGE_DOWN: *pscroll+=page; return 1;
        case SCAN_HOME:      *pscroll=0; return 1;
        case SCAN_END:       *pscroll=n; return 1;
        default: return 0;
    }
}

/* ==========================================================================
 * Shared bottom button bar (wm_button widget). Geometry lives in these
 * helpers ONLY, so a tool's draw callback and its event hit-test can never
 * disagree: content is rendered with bar_content_h(ch), buttons sit below a
 * separator in the reserved strip. When the client area is too short for the
 * strip (bar_fits()==0) the tool draws content full-height and no buttons.
 * ========================================================================== */
/* Client height available ABOVE the bar strip (4*sc gap + button + 4*sc pad
 * + 2*sc breathing room are reserved at the bottom). */
static int bar_content_h(int ch){ return ch - (wm_button_h() + 10*gsc()); }
static int bar_fits(int ch){ return bar_content_h(ch) >= 40*gsc(); }

static void btn_place(wm_button *b, int id, const char *label, int x, int y, int w, int h)
{ b->x=x; b->y=y; b->w=w; b->h=h; b->id=id; b->enabled=1;
  scopy(b->label,label,(int)sizeof(b->label)); }
static void btn_set(wm_button *b, int id, const char *label, int x, int y)
{ btn_place(b,id,label,x,y,wm_button_measure(label),wm_button_h()); }

/*
 * Build the standard bar into `out` (caller-sized, nl+1 entries max): the
 * `nl` left-aligned buttons described by ids[]/labels[], then a right-aligned
 * [Close] when close_id > 0. Returns the count (0 when the bar does not fit).
 */
static int bar_build(int cw, int ch, const int *ids, const char *const *labels,
                     int nl, int close_id, wm_button *out)
{
    if(!bar_fits(ch)) return 0;
    int sc=gsc(), y=ch-4*sc-wm_button_h(), x=6*sc, n=0;
    for(int i=0;i<nl;i++){ btn_set(&out[n],ids[i],labels[i],x,y); x+=out[n].w+6*sc; n++; }
    if(close_id>0){ btn_set(&out[n],close_id,"Close",0,y); out[n].x=cw-6*sc-out[n].w; n++; }
    return n;
}

/* Separator + the buttons themselves (client coords; call from a draw cb). */
static void bar_draw(int cx, int cy, int cw, int ch, const wm_button *b, int n,
                     int hover_id, int press_id)
{
    int sc=gsc();
    draw_hline(cx+4*sc, cy+ch-4*sc-wm_button_h()-4*sc, cw-8*sc, c_border);
    for(int i=0;i<n;i++) wm_button_draw(&b[i], b[i].id==hover_id, b[i].id==press_id);
}

/* First enabled button under (mx,my) -> its id, else 0. */
static int bar_hit(const wm_button *b, int n, int mx, int my)
{ for(int i=0;i<n;i++) if(b[i].enabled && wm_button_hit(&b[i],mx,my)) return b[i].id; return 0; }

/* ==========================================================================
 * Generic list-tool state (disk/gpt/partbrowse/memmap/efivars/bootmgr/sysinfo/
 * keytest all use this).
 * ========================================================================== */
typedef struct {
    int        kind;
    wm_window *win;                 /* NULL when closed (idempotent open)      */
    char       text[TL_MAXLINES][TL_COLS];
    UINT32     col[TL_MAXLINES];
    int        n, scroll, sel, selectable;
    int        aux[TL_MAXLINES];    /* per-row payload (device idx, etc.)      */
    int        cur_dev;             /* selected block device (gpt/hex source)  */
    int        dev_count;           /* block-device count cached by build_gpt  */
    int        b_hover, b_press;    /* button-bar hover / pressed button id    */
    wm_button  bar_b[2];            /* cached bottom button-bar geometry ...    */
    int        bar_nb;              /* ... its count ...                        */
    int        bar_cw, bar_ch, bar_sc, bar_valid; /* ... keyed on dims/scale.   */
} tstate;

static void tstate_zero(tstate *t)
{ if(gBS) gBS->SetMem(t, sizeof(*t), 0); else { char*p=(char*)t; for(unsigned i=0;i<sizeof(*t);i++)p[i]=0; } }

/* ==========================================================================
 * Block-device access.
 * ========================================================================== */
static int blk_count(void)
{ UINTN n=0; EFI_HANDLE *h=NULL; if(!gBS) return 0;
  if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gBlkGuid,NULL,&n,&h))||!h) return 0;
  gBS->FreePool(h); return (int)n; }

static EFI_BLOCK_IO_PROTOCOL *blk_idx(int want, int *count, EFI_HANDLE *out_h)
{
    UINTN n=0; EFI_HANDLE *h=NULL;
    if(out_h) *out_h=NULL;
    if(!gBS || EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gBlkGuid,NULL,&n,&h))||!h)
    { if(count)*count=0; return NULL; }
    if(count) *count=(int)n;
    EFI_BLOCK_IO_PROTOCOL *r=NULL;
    if(want>=0 && want<(int)n) {
        EFI_BLOCK_IO_PROTOCOL *b=NULL;
        if(!EFI_ERROR(gBS->HandleProtocol(h[want],&gBlkGuid,(VOID**)&b))) { r=b; if(out_h)*out_h=h[want]; }
    }
    gBS->FreePool(h);
    return r;
}

/* On-disk FS magic probe (shared by Partition Browser). Returns a name or NULL. */
static const char *fs_probe(EFI_BLOCK_IO_PROTOCOL *b)
{
    if(!b||!b->Media||!gBS) return NULL;
    EFI_BLOCK_IO_MEDIA *m=b->Media; UINT32 bs=m->BlockSize?m->BlockSize:512;
    UINTN want=66560; UINT64 devB=((UINT64)m->LastBlock+1)*bs; if((UINT64)want>devB)want=(UINTN)devB;
    UINTN secs=(want+bs-1)/bs, allocB=secs*bs; if(allocB==0) return NULL;
    UINT8 *d=NULL;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,allocB,(VOID**)&d))||!d) return NULL;
    const char *fs=NULL;
    if(!EFI_ERROR(b->ReadBlocks(b,m->MediaId,0,allocB,d))) {
        #define AT(off,str,len) (allocB>=(UINTN)((off)+(len)) && mem_eq(d+(off),(const UINT8*)(str),(len)))
        if      (allocB>511 && d[510]==0x55 && d[511]==0xAA && AT(3,"NTFS    ",8)) fs="NTFS";
        else if (allocB>511 && d[510]==0x55 && d[511]==0xAA && AT(3,"EXFAT   ",8)) fs="exFAT";
        else if (AT(82,"FAT32   ",8))                                             fs="FAT32";
        else if (AT(54,"FAT12   ",8)||AT(54,"FAT16   ",8)||AT(54,"FAT     ",8))   fs="FAT";
        else if (allocB>1082 && d[1080]==0x53 && d[1081]==0xEF)                   fs="ext2/3/4";
        else if (AT(0x10040,"_BHRfS_M",8))                                        fs="btrfs";
        else if (AT(0,"XFSB",4))                                                  fs="XFS";
        else if (AT(0,"LUKS\xba\xbe",6))                                          fs="LUKS";
        else if (AT(32769,"CD001",5))                                             fs="ISO9660";
        else if (AT(4086,"SWAPSPACE2",10)||AT(4086,"SWAP-SPACE",10))             fs="swap";
        else if (allocB>511 && d[510]==0x55 && d[511]==0xAA)                      fs="MBR/boot";
        #undef AT
    }
    gBS->FreePool(d);
    return fs;
}

/* ==========================================================================
 * Known GPT partition-type GUIDs (on-disk byte order) -> names.
 * ========================================================================== */
static const struct { UINT8 g[16]; const char *name; } gpt_types[] = {
    {{0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}, "EFI System"},
    {{0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4}, "Linux fs"},
    {{0x6d,0xfd,0x57,0x06,0xab,0xa4,0xc4,0x43,0x84,0xe5,0x09,0x33,0xc8,0x4b,0x4f,0x4f}, "Linux swap"},
    {{0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}, "MS basic data"},
    {{0x16,0xe3,0xc9,0xe3,0x5c,0x0b,0xb8,0x4d,0x81,0x7d,0xf9,0x2d,0xf0,0x02,0x15,0xae}, "MS reserved"},
    {{0x79,0xd3,0xd6,0xe6,0x07,0xf5,0xc2,0x44,0xa2,0x3c,0x23,0x8f,0x2a,0x3d,0xf9,0x28}, "Linux LVM"},
    {{0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49}, "BIOS boot"},
};
static const char *gpt_name(const UINT8 *g)
{ for(unsigned i=0;i<sizeof(gpt_types)/sizeof(gpt_types[0]);i++) if(mem_eq(gpt_types[i].g,g,16)) return gpt_types[i].name; return 0; }

/* GUID pretty-printer (registry/mixed byte order per UEFI text form). */
static void LN_guid(const UINT8 *g)
{
    LNx(rd_u32(g),8); LNc('-'); LNx(rd_u16(g+4),4); LNc('-'); LNx(rd_u16(g+6),4); LNc('-');
    LNhex2(g[8]); LNhex2(g[9]); LNc('-');
    for(int i=10;i<16;i++) LNhex2(g[i]);
}

/* ==========================================================================
 * Forward decls for cross-tool opens + the shared list draw/event.
 * ========================================================================== */
static void list_draw(wm_window *w, int cx, int cy, int cw, int ch);
static int  list_event(wm_window *w, const wm_event *ev);
static const char *tool_header(int kind);
static void tool_rebuild(tstate *t);
static void tool_activate_row(tstate *t);   /* Enter / click on a selectable row */

/* ==========================================================================
 * Hex Viewer (own state; renders live from a byte blob, handles large files).
 * ========================================================================== */
enum { HEXSRC_NONE=0, HEXSRC_SECTOR, HEXSRC_FILE, HEXSRC_MEM };
static struct {          /* one-shot request set by a caller before open()      */
    int   src;
    int   dev; UINT64 lba;                 /* HEXSRC_SECTOR                      */
    EFI_FILE_PROTOCOL *root; char path[300];/* HEXSRC_FILE                        */
    UINT8 *mem; UINTN memsize;             /* HEXSRC_MEM (ownership transferred) */
    char  title[WM_TITLE_LEN];
} g_hexreq;

typedef struct {
    wm_window *win;
    UINT8    *blob; UINTN size; UINT64 base;
    int       scroll;                      /* first visible 16-byte row          */
    char      title[WM_TITLE_LEN];
    int       b_hover, b_press;            /* button-bar hover / pressed id      */
} hexstate;
static hexstate g_hex;

#define HEX_CAP  (64*1024)

/* Visible 16-byte rows, reserving the button-bar strip when it fits. Shared
 * by hex_draw + hex_event so render and hit-test agree. */
static int hex_rows(int ch)
{
    int sc=gsc();
    int bot=bar_fits(ch)?bar_content_h(ch):ch-4*sc;
    int r=(bot-L_yoff(1))/L_lineH(); return r<1?1:r;
}

enum { HX_PREV=1, HX_NEXT=2, HX_CLOSE=3 };
static int hex_btns(int cw, int ch, wm_button *out)
{
    int ids[2]={HX_PREV,HX_NEXT}; const char *lb[2]={"Prev","Next"};
    int n=bar_build(cw,ch,ids,lb,2,HX_CLOSE,out);
    if(n){
        int total=(int)((g_hex.size+15)/16), rows=hex_rows(ch);
        out[0].enabled=(g_hex.scroll>0);
        out[1].enabled=(g_hex.scroll<total-rows);
    }
    return n;
}

/* One page of scroll (dir = -1 prev / +1 next), same step as PgUp/PgDn. */
static void hex_page(hexstate *h, int dir, int rows)
{
    int total=(int)((h->size+15)/16);
    h->scroll+=dir*(rows-1);
    if(h->scroll>total-rows)h->scroll=total-rows;
    if(h->scroll<0)h->scroll=0;
}

static void hex_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    hexstate *h=&g_hex;
    int sc=gsc(), lineH=L_lineH();
    int x=cx+6*sc, y=cy+4*sc;
    /* header */
    { char hb[TL_COLS]; tl_begin(g_txt?g_txt:0,0,0,0); (void)0;
      /* build header directly */
      char t[TL_COLS]; int p=0;
      const char *ttl=h->title[0]?h->title:"Hex"; while(*ttl&&p<TL_COLS-1)t[p++]=*ttl++;
      const char *sz="  bytes="; for(const char*s=sz;*s&&p<TL_COLS-1;)t[p++]=*s++;
      /* size as decimal */
      { UINT64 v=h->size; char tmp[24]; int i=0; if(!v)tmp[i++]='0'; while(v){tmp[i++]=(char)('0'+(int)(v%10));v/=10;} while(i&&p<TL_COLS-1)t[p++]=tmp[--i]; }
      t[p]=0; (void)hb;
      draw_string(x,y,t,c_accent,c_win,1,sc); y+=lineH+3*sc; }

    int rows=hex_rows(ch);
    int totalRows=(int)((h->size+15)/16); if(totalRows<0)totalRows=0;
    if(h->scroll>totalRows-rows) h->scroll=totalRows-rows;
    if(h->scroll<0) h->scroll=0;

    static const char hx[]="0123456789ABCDEF";
    for(int r=0;r<rows;r++){
        int row=h->scroll+r; if(row>=totalRows) break;
        UINTN off=(UINTN)row*16;
        char ln[TL_COLS]; int p=0;
        /* offset (8 hex) */
        UINT64 a=h->base+off; for(int k=28;k>=0;k-=4) ln[p++]=hx[(a>>k)&0xF];
        ln[p++]=' '; ln[p++]=' ';
        /* 16 hex bytes */
        for(int i=0;i<16;i++){
            if(off+i<h->size){ UINT8 b=h->blob[off+i]; ln[p++]=hx[b>>4]; ln[p++]=hx[b&0xF]; }
            else { ln[p++]=' '; ln[p++]=' '; }
            ln[p++]=' ';
            if(i==7) ln[p++]=' ';
        }
        ln[p++]=' '; ln[p++]='|';
        /* ascii */
        for(int i=0;i<16;i++){
            if(off+i<h->size){ UINT8 b=h->blob[off+i]; ln[p++]=(b>=0x20&&b<0x7f)?(char)b:'.'; }
            else ln[p++]=' ';
        }
        ln[p++]='|'; ln[p]=0;
        int cols=L_cols(cw); if(cols<TL_COLS-1 && p>cols) ln[cols]=0;
        draw_string(x,y+r*lineH,ln,c_fg,c_win,1,sc);
    }
    /* scrollbar */
    if(totalRows>rows){
        int barw=6*sc, trackX=cx+cw-barw-2*sc, trackH=rows*lineH;
        fill_rect(trackX,y,barw,trackH,c_border);
        int thumbH=rows*trackH/totalRows; if(thumbH<8*sc)thumbH=8*sc;
        int thumbY=y+h->scroll*(trackH-thumbH)/(totalRows-rows);
        fill_rect(trackX,thumbY,barw,thumbH,c_accent);
    }
    /* button bar: [Prev] [Next] ... [Close] */
    { wm_button b[3]; int nb=hex_btns(cw,ch,b);
      if(nb) bar_draw(cx,cy,cw,ch,b,nb,h->b_hover,h->b_press); }
}

static void hex_free(void)
{ if(g_hex.blob && gBS){ gBS->FreePool(g_hex.blob); } g_hex.blob=NULL; g_hex.size=0; }

static int hex_event(wm_window *w, const wm_event *ev)
{
    hexstate *h=&g_hex;
    int ch=wm_client_h(w), cw=wm_client_w(w);
    int rows=hex_rows(ch);
    int total=(int)((h->size+15)/16);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_UP && h->scroll>0) h->scroll--;
            else if(ev->scancode==SCAN_DOWN && h->scroll<total-rows) h->scroll++;
            else if(ev->scancode==SCAN_PAGE_UP) hex_page(h,-1,rows);
            else if(ev->scancode==SCAN_PAGE_DOWN) hex_page(h,+1,rows);
            else if(ev->scancode==SCAN_HOME || ev->unicode=='g') h->scroll=0;
            else if(ev->scancode==SCAN_END  || ev->unicode=='G'){ h->scroll=total-rows; if(h->scroll<0)h->scroll=0; }
            return 0;
        case WM_EV_MOUSE_WHEEL:
            h->scroll-=ev->wheel;
            if(h->scroll>total-rows)h->scroll=total-rows;
            if(h->scroll<0)h->scroll=0;
            return 0;
        case WM_EV_MOUSE_MOVE: {
            wm_button b[3]; int nb=hex_btns(cw,ch,b);
            h->b_hover=bar_hit(b,nb,ev->mx,ev->my);
            return 0; }
        case WM_EV_MOUSE_DOWN: {
            wm_button b[3]; int nb=hex_btns(cw,ch,b);
            int id=bar_hit(b,nb,ev->mx,ev->my);
            if(id) h->b_press=id;
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!h->b_press) return 0;
            wm_button b[3]; int nb=hex_btns(cw,ch,b);
            int id=bar_hit(b,nb,ev->mx,ev->my), p=h->b_press;
            h->b_press=0;
            if(id==p){
                if(p==HX_CLOSE) return WM_CLOSE_REQUEST;
                hex_page(h, p==HX_PREV?-1:+1, rows);
            }
            return 0; }
        case WM_EV_CLOSE:
            g_hex.win=NULL; hex_free(); return 0;
        default: return 0;
    }
}

/* Read a file (via any already-open root) into a fresh capped blob. */
static EFI_STATUS read_file_root(EFI_FILE_PROTOCOL *root, const char *ascii_path,
                                 UINT8 **out, UINTN *outsz)
{
    *out=NULL; *outsz=0;
    if(!root||!gBS) return EFI_INVALID_PARAMETER;
    CHAR16 wp[300]; esp_ascii_to_char16(ascii_path, wp, 300);
    EFI_FILE_PROTOCOL *fh=NULL;
    if(EFI_ERROR(root->Open(root,&fh,wp,EFI_FILE_MODE_READ,0))||!fh) return EFI_NOT_FOUND;
    UINT8 info[512]; UINTN isz=sizeof(info); UINT64 fsize=0;
    if(!EFI_ERROR(fh->GetInfo(fh,&gFinfoGuid,&isz,info))) fsize=((EFI_FILE_INFO*)info)->FileSize;
    UINTN want=(UINTN)fsize; if(want==0) want=HEX_CAP; if(want>HEX_CAP) want=HEX_CAP;
    UINT8 *buf=NULL;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,want?want:1,(VOID**)&buf))||!buf){ fh->Close(fh); return EFI_OUT_OF_RESOURCES; }
    UINTN rd=want; if(EFI_ERROR(fh->Read(fh,&rd,buf))){ gBS->FreePool(buf); fh->Close(fh); return EFI_DEVICE_ERROR; }
    fh->Close(fh);
    *out=buf; *outsz=rd; return EFI_SUCCESS;
}

void tool_hexview_open(void)
{
    if(g_hex.win){                      /* already open: drop any pending request */
        if(g_hexreq.src==HEXSRC_MEM && g_hexreq.mem && gBS) gBS->FreePool(g_hexreq.mem);
        if(gBS) gBS->SetMem(&g_hexreq,sizeof(g_hexreq),0);
        return;
    }
    hex_free();
    g_hex.scroll=0; g_hex.base=0; g_hex.title[0]=0;

    int src=g_hexreq.src ? g_hexreq.src : HEXSRC_SECTOR;
    g_hex.b_hover=0; g_hex.b_press=0;
    if(g_hexreq.title[0]) scopy(g_hex.title, g_hexreq.title, WM_TITLE_LEN);

    if(src==HEXSRC_MEM && g_hexreq.mem){
        g_hex.blob=g_hexreq.mem; g_hex.size=g_hexreq.memsize; /* take ownership */
        if(!g_hex.title[0]) scopy(g_hex.title,"Variable value",WM_TITLE_LEN);
    } else if(src==HEXSRC_FILE && g_hexreq.root && g_hexreq.path[0]){
        if(EFI_ERROR(read_file_root(g_hexreq.root,g_hexreq.path,&g_hex.blob,&g_hex.size))){
            g_hex.blob=NULL; g_hex.size=0;
        }
        if(!g_hex.title[0]) scopy(g_hex.title,g_hexreq.path,WM_TITLE_LEN);
    } else {
        /* raw sector of a block device */
        int dev=g_hexreq.dev; EFI_BLOCK_IO_PROTOCOL *b=blk_idx(dev,NULL,NULL);
        if(b&&b->Media&&gBS){
            EFI_BLOCK_IO_MEDIA *m=b->Media; UINT32 bs=m->BlockSize?m->BlockSize:512;
            UINTN want=4096; UINTN secs=(want+bs-1)/bs, allocB=secs*bs;
            UINT8 *buf=NULL;
            if(!EFI_ERROR(gBS->AllocatePool(EfiLoaderData,allocB,(VOID**)&buf))&&buf){
                if(!EFI_ERROR(b->ReadBlocks(b,m->MediaId,g_hexreq.lba,allocB,buf))){
                    g_hex.blob=buf; g_hex.size=allocB; g_hex.base=g_hexreq.lba*bs;
                } else gBS->FreePool(buf);
            }
        }
        if(!g_hex.title[0]) scopy(g_hex.title,"Disk sector",WM_TITLE_LEN);
    }
    /* consume the request */
    if(gBS) gBS->SetMem(&g_hexreq,sizeof(g_hexreq),0);

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*66/100; if(ww<560)ww=560; if(ww>900)ww=900; if(ww>W-40)ww=W-40;
    int wh=H*60/100; if(wh<300)wh=300; if(wh>640)wh=640; if(wh>H-40)wh=H-40;
    g_hex.win=wm_open(g_hex.title[0]?g_hex.title:"Hex Viewer", ww, wh, hex_draw, hex_event, &g_hex);
    if(!g_hex.win) hex_free();
}

/* ==========================================================================
 * File Browser (ESP / a chosen volume) - own state (needs an entry name table).
 * ========================================================================== */
typedef struct {
    wm_window *win;
    EFI_FILE_PROTOCOL *root;              /* volume root (closed on window close) */
    char       dir[256];                  /* current dir, '\'-relative, ""=root   */
    char       name[TL_MAXLINES][96];     /* entry names (index 0 may be "..")    */
    UINT8      isdir[TL_MAXLINES];
    UINT64     fsize[TL_MAXLINES];
    /* mirror render arrays */
    char       text[TL_MAXLINES][TL_COLS];
    UINT32     col[TL_MAXLINES];
    int        n, scroll, sel;
    int        b_hover, b_press;          /* button-bar hover / pressed id     */
} fbstate;
static fbstate g_fb;

enum { FB_UP=1, FB_OPEN=2, FB_CLOSE=3 };
static int fb_btns(int cw, int ch, wm_button *out)
{
    int ids[2]={FB_UP,FB_OPEN}; const char *lb[2]={"Up","Open"};
    int n=bar_build(cw,ch,ids,lb,2,FB_CLOSE,out);
    if(n){ out[0].enabled=(g_fb.dir[0]!=0); out[1].enabled=(g_fb.n>0); }
    return n;
}

static struct { int active; EFI_FILE_PROTOCOL *root; } g_fbreq;

/* List the current directory into name[]/isdir[]/fsize[] + render text[]. */
static void fb_list(fbstate *f)
{
    /* reset arrays via composer target */
    int cnt=0;
    EFI_FILE_PROTOCOL *dir=f->root;
    EFI_FILE_PROTOCOL *opened=NULL;
    if(f->dir[0] && f->root){
        CHAR16 wp[256]; esp_ascii_to_char16(f->dir, wp, 256);
        if(!EFI_ERROR(f->root->Open(f->root,&opened,wp,EFI_FILE_MODE_READ,0))&&opened) dir=opened;
    }
    /* ".." entry when not at root */
    if(f->dir[0]){ scopy(f->name[cnt],"..",96); f->isdir[cnt]=1; f->fsize[cnt]=0; cnt++; }

    if(dir){
        for(;;){
            UINT8 ib[1024]; UINTN isz=sizeof(ib);
            EFI_STATUS st=dir->Read(dir,&isz,ib);
            if(EFI_ERROR(st) || isz==0) break;
            EFI_FILE_INFO *fi=(EFI_FILE_INFO*)ib;
            char nm[96]; u2a(fi->FileName,nm,96);
            if(nm[0]=='.' && nm[1]==0) continue;            /* skip "." */
            if(nm[0]=='.' && nm[1]=='.' && nm[2]==0) continue;/* firmware ".." handled above */
            if(cnt>=TL_MAXLINES) break;
            scopy(f->name[cnt],nm,96);
            f->isdir[cnt]=(fi->Attribute&EFI_FILE_DIRECTORY)?1:0;
            f->fsize[cnt]=fi->FileSize;
            cnt++;
        }
    }
    if(opened) opened->Close(opened);

    /* render text[] */
    tl_begin(f->text, f->col, &f->n, TL_MAXLINES);
    for(int i=0;i<cnt;i++){
        LN0(f->isdir[i]?c_accent:c_fg);
        if(f->isdir[i]){ LNs("[DIR] "); LNs(f->name[i]); }
        else { LNs("      "); LNs(f->name[i]); LNsp(2); LNs("("); LNu(f->fsize[i]); LNs(" B)"); }
        LNend();
    }
    f->n=cnt;               /* LNend already advanced n to cnt */
    if(f->sel>=cnt) f->sel=cnt-1; if(f->sel<0)f->sel=0;
    f->scroll=0;
}

static void fb_header(fbstate *f, char *out)
{ int p=0; out[p++]='E'; out[p++]='S'; out[p++]='P'; out[p++]=':';
  if(!f->dir[0]) out[p++]='\\'; else { for(const char*s=f->dir;*s&&p<TL_COLS-1;)out[p++]=*s++; } out[p]=0; }

static void fb_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; char hdr[TL_COLS]; fb_header(&g_fb,hdr);
    int cch=bar_fits(ch)?bar_content_h(ch):ch;
    render_list(cx,cy,cw,cch,hdr,g_fb.text,g_fb.col,g_fb.n,&g_fb.scroll,g_fb.sel,1);
    wm_button b[3]; int nb=fb_btns(cw,ch,b);
    if(nb) bar_draw(cx,cy,cw,ch,b,nb,g_fb.b_hover,g_fb.b_press);
}

static void fb_up(fbstate *f)
{ int i=slen(f->dir); if(i==0) return; if(f->dir[i-1]=='\\')i--;
  while(i>0 && f->dir[i-1]!='\\') i--; if(i>0)i--; f->dir[i]=0; }

static void fb_enter(fbstate *f)
{
    if(f->sel<0||f->sel>=f->n) return;
    if(f->name[f->sel][0]=='.'&&f->name[f->sel][1]=='.'&&f->name[f->sel][2]==0){ fb_up(f); f->sel=0; fb_list(f); return; }
    if(f->isdir[f->sel]){
        int p=slen(f->dir); if(p+2+slen(f->name[f->sel])<256){
            f->dir[p++]='\\'; scopy(f->dir+p, f->name[f->sel], 256-p);
        }
        f->sel=0; fb_list(f); return;
    }
    /* file -> open Hex Viewer on it (deferred: set request, open inline) */
    if(gBS) gBS->SetMem(&g_hexreq,sizeof(g_hexreq),0);
    g_hexreq.src=HEXSRC_FILE; g_hexreq.root=f->root;
    { int p=0; for(const char*s=f->dir;*s&&p<299;)g_hexreq.path[p++]=*s++;
      g_hexreq.path[p++]='\\'; scopy(g_hexreq.path+p, f->name[f->sel], 300-p); }
    scopy(g_hexreq.title, f->name[f->sel], WM_TITLE_LEN);
    tool_hexview_open();
}

static int fb_event(wm_window *w, const wm_event *ev)
{
    fbstate *f=&g_fb;
    int ch=wm_client_h(w), cw=wm_client_w(w);
    int cch=bar_fits(ch)?bar_content_h(ch):ch;
    int rows=L_rows(cch,1);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_CR){ fb_enter(f); return 0; }
            if(ev->unicode==CHAR_BACKSPACE || ev->scancode==SCAN_LEFT){ fb_up(f); f->sel=0; fb_list(f); return 0; }
            L_key_nav(ev->scancode,&f->scroll,&f->sel,f->n,rows,1);
            return 0;
        case WM_EV_MOUSE_WHEEL:
            f->sel-=ev->wheel;
            if(f->sel<0)f->sel=0; if(f->sel>f->n-1)f->sel=f->n-1;
            return 0;
        case WM_EV_MOUSE_MOVE: {
            wm_button b[3]; int nb=fb_btns(cw,ch,b);
            f->b_hover=bar_hit(b,nb,ev->mx,ev->my);
            return 0; }
        case WM_EV_MOUSE_DOWN: {
            wm_button b[3]; int nb=fb_btns(cw,ch,b);
            int id=bar_hit(b,nb,ev->mx,ev->my);
            if(id){ f->b_press=id; return 0; }
            int r=L_row_at(ev->my,f->scroll,f->n,rows,1);
            if(r>=0){ if(r==f->sel) fb_enter(f); else f->sel=r; }
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!f->b_press) return 0;
            wm_button b[3]; int nb=fb_btns(cw,ch,b);
            int id=bar_hit(b,nb,ev->mx,ev->my), p=f->b_press;
            f->b_press=0;
            if(id==p){
                if(p==FB_CLOSE) return WM_CLOSE_REQUEST;
                if(p==FB_UP){ fb_up(f); f->sel=0; fb_list(f); }
                else if(p==FB_OPEN) fb_enter(f);
            }
            return 0; }
        case WM_EV_CLOSE:
            f->win=NULL; if(f->root){ f->root->Close(f->root); f->root=NULL; } return 0;
        default: return 0;
    }
}

void tool_filebrowse_open(void)
{
    if(g_fb.win) return;
    if(gBS) gBS->SetMem(&g_fb,sizeof(g_fb),0);
    g_fb.sel=0;
    /* volume root: a requested one (partition browser) or the ESP */
    if(g_fbreq.active && g_fbreq.root){ g_fb.root=g_fbreq.root; }
    else { EFI_FILE_PROTOCOL *root=NULL; if(!EFI_ERROR(esp_open_root(gImage,gBS,&root))) g_fb.root=root; }
    g_fbreq.active=0; g_fbreq.root=NULL;
    if(!g_fb.root){
        tl_begin(g_fb.text,g_fb.col,&g_fb.n,TL_MAXLINES);
        LN0(c_warn); LNs("Could not open the boot volume (ESP)."); LNend();
    } else fb_list(&g_fb);

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*56/100; if(ww<440)ww=440; if(ww>820)ww=820; if(ww>W-40)ww=W-40;
    int wh=H*62/100; if(wh<300)wh=300; if(wh>640)wh=640; if(wh>H-40)wh=H-40;
    g_fb.win=wm_open("File Browser", ww, wh, fb_draw, fb_event, &g_fb);
    if(!g_fb.win && g_fb.root){ g_fb.root->Close(g_fb.root); g_fb.root=NULL; }
}

/* ==========================================================================
 * Theme / Settings tool (live edits to gCfg->theme + wm re-skin).
 * ========================================================================== */
typedef struct { wm_window *win; int sel; int b_hover, b_press; } setstate;
static setstate g_set;

void tool_colorpicker_open(unsigned int *target, void (*apply)(void)); /* fwd */

/* A palette users can cycle colours through. */
static const UINT32 g_palette[] = {
    0x00182D18u,0x001C351Cu,0x00285128u,0x00146514u,0x0051CA3Du,0x00B6DFB6u,
    0x00FFFFFFu,0x003FB56Bu,0x001F5E3Au,0x00DFA214u,0x00202830u,0x000E1A12u
};
#define PAL_N ((int)(sizeof(g_palette)/sizeof(g_palette[0])))

#define SET_ROWS 14
static const char *set_labels[SET_ROWS] = {
    "Cursor visible", "Poll mouse", "Animations", "Double buffer", "Window skin",
    "-- colours --",
    "Background", "Text", "Accent", "Select bg", "Select fg", "Title bar",
    "Window", "Cursor colour"
};

static unsigned int *set_color_ptr(int row)
{
    if(!gCfg) return 0;
    struct forebo_theme *t=&gCfg->theme;
    switch(row){ case 6:return &t->color_bg; case 7:return &t->color_fg;
        case 8:return &t->color_accent; case 9:return &t->color_sel_bg;
        case 10:return &t->color_sel_fg; case 11:return &t->color_titlebar;
        case 12:return &t->color_window; case 13:return &t->color_cursor; default:return 0; }
}

/* Button ids: bottom-bar [Close] plus the per-color-row [<] / [>] pair. */
#define SET_CLOSE_ID  1
#define SET_LT(row)   (16+(row))
#define SET_GT(row)   (32+(row))

/*
 * Small [<] [>] adjust buttons for one colour row, client coords, matching
 * set_draw's row geometry. Returns 1 (+ fills out[2]) when they fit beside
 * the value text, else 0 (window too narrow -> just don't draw them).
 */
static int set_adj_btns(int cw, int row, wm_button out[2])
{
    int sc=gsc(), lineH=L_lineH()+2*sc;
    int ry=6*sc+lineH+2*sc + row*lineH;         /* row top (matches set_draw) */
    int bw=wm_button_measure("<"), bh=lineH-2*sc;
    int bxR=cw-8*sc-bw, bxL=bxR-2*sc-bw;
    if(bxL < 8*sc+22*8*sc+92*sc) return 0;      /* keep clear of swatch + hex */
    btn_place(&out[0],SET_LT(row),"<",bxL,ry+sc,bw,bh);
    btn_place(&out[1],SET_GT(row),">",bxR,ry+sc,bw,bh);
    return 1;
}

/* Any settings button under (mx,my) -> its id, else 0 (shared hover/hit). */
static int set_hit(int cw, int ch, int mx, int my)
{
    wm_button cb;
    if(bar_build(cw,ch,0,0,0,SET_CLOSE_ID,&cb) && wm_button_hit(&cb,mx,my))
        return SET_CLOSE_ID;
    int sc=gsc(), lineH=L_lineH()+2*sc;
    int r=(my-(6*sc+lineH+2*sc))/lineH;
    if(r>=6 && r<SET_ROWS){
        wm_button ab[2];
        if(set_adj_btns(cw,r,ab)){
            if(wm_button_hit(&ab[0],mx,my)) return SET_LT(r);
            if(wm_button_hit(&ab[1],mx,my)) return SET_GT(r);
        }
    }
    return 0;
}

static void set_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    int sc=gsc(), lineH=L_lineH()+2*sc;
    int x=cx+8*sc, y=cy+6*sc;
    struct forebo_theme *t=gCfg?&gCfg->theme:0;
    draw_string(x,y,"Theme / Settings  (Enter/click toggles, Esc closes)",c_accent,c_win,1,sc);
    y+=lineH+2*sc;
    for(int i=0;i<SET_ROWS;i++){
        int ry=y+i*lineH; int selr=(g_set.sel==i);
        if(selr) fill_rect(x-4*sc,ry-1,cw-16*sc,lineH,c_sel_bg);
        UINT32 fg=selr?c_sel_fg:c_fg;
        if(i==5){ draw_string(x,ry,set_labels[i],c_dim,c_win,1,sc); continue; }
        draw_string(x,ry,set_labels[i],fg,c_win,1,sc);
        int vx=x+22*8*sc;
        if(!t){ draw_string(vx,ry,"(no config)",c_dim,c_win,1,sc); continue; }
        if(i<4){ int v = i==0?t->cursor_enabled : i==1?t->mouse_enabled : i==2?t->animations_enabled : t->double_buffer;
                 draw_string(vx,ry,v?"ON":"off",v?c_accent:c_dim,c_win,1,sc); }
        else if(i==4){ const char*s=t->window_skin==FOREB_SKIN_BEVELED?"beveled":t->window_skin==FOREB_SKIN_GLASS?"glass":"flat";
                       draw_string(vx,ry,s,c_fg,c_win,1,sc); }
        else { unsigned int *cp=set_color_ptr(i); if(cp){ UINT32 cc=pick(*cp, c_fg);
                 fill_rect(vx,ry,3*8*sc,lineH-3*sc,cc); draw_rect_outline(vx,ry,3*8*sc,lineH-3*sc,1,c_border);
                 char hb[10]; static const char h[]="0123456789ABCDEF"; UINT32 v=cc&0xFFFFFF;
                 hb[0]='#'; for(int k=0;k<6;k++)hb[1+k]=h[(v>>((5-k)*4))&0xF]; hb[7]=0;
                 draw_string(vx+3*8*sc+6*sc,ry,hb,c_dim,c_win,1,sc);
                 wm_button ab[2];
                 if(set_adj_btns(cw,i,ab)){
                     wm_button_draw(&ab[0], g_set.b_hover==SET_LT(i), g_set.b_press==SET_LT(i));
                     wm_button_draw(&ab[1], g_set.b_hover==SET_GT(i), g_set.b_press==SET_GT(i));
                 } } }
    }
    /* bottom bar: right-aligned [Close] */
    { wm_button cb; int nb=bar_build(cw,ch,0,0,0,SET_CLOSE_ID,&cb);
      if(nb) bar_draw(cx,cy,cw,ch,&cb,nb,g_set.b_hover,g_set.b_press); }
}

static void set_apply(void)
{ resolve_theme(); if(gCfg) wm_set_theme(&gCfg->theme); }

/* Cycle a colour row through the palette, dir = +1 / -1 (Enter/[>] / [<]). */
static void set_color_cycle(int row, int dir)
{
    unsigned int *cp=set_color_ptr(row); if(!cp) return;
    UINT32 cur=(*cp)&0xFFFFFF; int idx=0;
    for(int k=0;k<PAL_N;k++) if((g_palette[k]&0xFFFFFF)==cur){idx=k;break;}
    idx=(idx+dir)%PAL_N; if(idx<0)idx+=PAL_N;
    *cp=g_palette[idx];
    set_apply();
}

static void set_activate(int row)
{
    if(!gCfg) return; struct forebo_theme *t=&gCfg->theme;
    if(row==0) t->cursor_enabled=!t->cursor_enabled;
    else if(row==1) t->mouse_enabled=!t->mouse_enabled;
    else if(row==2) t->animations_enabled=!t->animations_enabled;
    else if(row==3) t->double_buffer=!t->double_buffer;
    else if(row==4) t->window_skin=(t->window_skin+1)%3;
    else tool_colorpicker_open(set_color_ptr(row), set_apply);  /* full RGB pick */
    if(row<=4) set_apply();
}

static int set_event(wm_window *w, const wm_event *ev)
{
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_UP){ do{ g_set.sel=(g_set.sel>0)?g_set.sel-1:SET_ROWS-1; }while(g_set.sel==5); }
            else if(ev->scancode==SCAN_DOWN){ do{ g_set.sel=(g_set.sel<SET_ROWS-1)?g_set.sel+1:0; }while(g_set.sel==5); }
            else if(ev->scancode==SCAN_LEFT){ if(g_set.sel>=6) set_color_cycle(g_set.sel,-1); }
            else if(ev->scancode==SCAN_RIGHT){ if(g_set.sel>=6) set_color_cycle(g_set.sel,+1); }
            else if(ev->unicode==CHAR_CR) set_activate(g_set.sel);
            return 0;
        case WM_EV_MOUSE_MOVE:
            g_set.b_hover=set_hit(cw,ch,ev->mx,ev->my);
            return 0;
        case WM_EV_MOUSE_DOWN: {
            int id=set_hit(cw,ch,ev->mx,ev->my);
            if(id){ g_set.b_press=id; return 0; }
            int sc=gsc(), lineH=L_lineH()+2*sc;
            int y0=6*sc + lineH+2*sc;   /* header offset (matches set_draw) */
            int r=(ev->my - y0)/lineH;
            if(r>=0 && r<SET_ROWS && r!=5){ g_set.sel=r; set_activate(r); }
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!g_set.b_press) return 0;
            int id=set_hit(cw,ch,ev->mx,ev->my), p=g_set.b_press;
            g_set.b_press=0;
            if(id==p){
                if(p==SET_CLOSE_ID) return WM_CLOSE_REQUEST;
                if(p>=SET_LT(6) && p<SET_LT(SET_ROWS)) set_color_cycle(p-16,-1);
                else if(p>=SET_GT(6) && p<SET_GT(SET_ROWS)) set_color_cycle(p-32,+1);
            }
            return 0; }
        case WM_EV_CLOSE: settings_nv_save(gCfg); g_set.win=NULL; return 0;
        default: return 0;
    }
}

/* ==========================================================================
 * Interactive RGB colour picker (opened from a Settings colour row).
 * Three gradient channel bars (R/G/B) with draggable knobs, a live swatch +
 * hex readout, and [OK]/[Cancel]. Writes the chosen 0x00RRGGBB back to the
 * target pointer and calls the apply callback live while dragging.
 * ========================================================================== */
typedef struct {
    wm_window   *win;
    unsigned int *target;      /* where the result is written                 */
    unsigned int  orig;        /* value on open (restored on Cancel)          */
    unsigned int  val;         /* current 0x00RRGGBB                          */
    void        (*apply)(void);/* live-preview callback (may be NULL)         */
    int          drag;         /* channel being dragged 0/1/2, else -1        */
    int          b_hover, b_press;
    unsigned int bars_val;     /* colour the gradient bars were last drawn for */
    int          bars_cw;      /* client width they were drawn for (-1=stale)  */
} cpickstate;
static cpickstate g_cp;

#define CP_OK_ID    1
#define CP_CANCEL_ID 2
static const int         CP_IDS[2] = { CP_OK_ID, CP_CANCEL_ID };
static const char *const CP_LB[2]  = { "OK", "Cancel" };

/* Bar rect for channel `ch` (0=R,1=G,2=B) in client coords. */
static void cp_bar(int cx, int cy, int cw, int ch_i, int *bx, int *by, int *bw, int *bh)
{
    int sc=gsc(), lineH=L_lineH()+2*sc;
    *bx = cx + 8*sc + 3*8*sc;                 /* room for the "R:" label      */
    *bw = cw - (*bx - cx) - 8*sc - 5*8*sc;    /* room for the numeric value   */
    if(*bw < 40) *bw = 40;
    *by = cy + 6*sc + lineH*2 + ch_i*(lineH+4*sc);
    *bh = lineH;
}

static void cp_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    int sc=gsc(), lineH=L_lineH()+2*sc;
    int x=cx+8*sc, y=cy+6*sc;
    draw_string(x,y,"Colour picker  (drag bars, Enter=OK, Esc=Cancel)",c_accent,c_win,1,sc);
    UINT32 v=g_cp.val;
    int chan[3]={ (int)((v>>16)&0xFF), (int)((v>>8)&0xFF), (int)(v&0xFF) };
    const char *nm[3]={"R:","G:","B:"};
    /* The three gradient bars only change when the colour (or the bar width on
     * a resize) changes; on plain hover frames the client already holds them,
     * so skip the ~900 per-pixel fill_rect calls to VRAM and just repaint the
     * cheap knob/label/value on top. tool_colorpicker_open() sets bars_cw=-1 so
     * a freshly opened window always draws the bars once. */
    int cp_regen=(v!=g_cp.bars_val || cw!=g_cp.bars_cw);
    for(int i=0;i<3;i++){
        int bx,by,bw,bh; cp_bar(cx,cy,cw,i,&bx,&by,&bw,&bh);
        draw_string(cx+8*sc,by,nm[i],c_fg,c_win,1,sc);
        /* gradient bar: this channel swept 0..255 over the other two. */
        if(cp_regen) for(int px=0;px<bw;px++){
            int cv=px*255/(bw>1?bw-1:1);
            UINT32 col = i==0 ? ((UINT32)cv<<16)|((UINT32)chan[1]<<8)|chan[2]
                       : i==1 ? ((UINT32)chan[0]<<16)|((UINT32)cv<<8)|chan[2]
                              : ((UINT32)chan[0]<<16)|((UINT32)chan[1]<<8)|cv;
            fill_rect(bx+px,by,1,bh,col);
        }
        draw_rect_outline(bx,by,bw,bh,1,c_border);
        int kx=bx+chan[i]*(bw-1)/255;
        fill_rect(kx-2,by-2,5,bh+4,c_sel_fg); draw_rect_outline(kx-2,by-2,5,bh+4,1,c_border);
        char nb[5]; int n=chan[i],d=0; char tmp[4];
        do{ tmp[d++]=(char)('0'+n%10); n/=10; }while(n&&d<3);
        for(int k=0;k<d;k++) nb[k]=tmp[d-1-k]; nb[d]=0;
        draw_string(bx+bw+6*sc,by,nb,c_fg,c_win,1,sc);
    }
    g_cp.bars_val=v; g_cp.bars_cw=cw;
    /* Swatch + hex. */
    int swy=cy+6*sc+lineH*2+3*(lineH+4*sc)+6*sc;
    fill_rect(x,swy,10*8*sc,lineH+4*sc,v&0xFFFFFF);
    draw_rect_outline(x,swy,10*8*sc,lineH+4*sc,1,c_border);
    char hb[10]; static const char h[]="0123456789ABCDEF"; UINT32 hv=v&0xFFFFFF;
    hb[0]='#'; for(int k=0;k<6;k++) hb[1+k]=h[(hv>>((5-k)*4))&0xF]; hb[7]=0;
    draw_string(x+10*8*sc+8*sc,swy+2*sc,hb,c_accent,c_win,1,sc);
    /* [OK] [Cancel] bar. */
    wm_button bb[2]; int nb2=bar_build(cw,ch,CP_IDS,CP_LB,2,0,bb);
    if(nb2) bar_draw(cx,cy,cw,ch,bb,nb2,g_cp.b_hover,g_cp.b_press);
}

static void cp_set_from_x(int ch_i, int cx, int cy, int cw, int mx)
{
    int bx,by,bw,bh; cp_bar(cx,cy,cw,ch_i,&bx,&by,&bw,&bh);
    int cv=(mx-bx)*255/(bw>1?bw-1:1); if(cv<0)cv=0; if(cv>255)cv=255;
    UINT32 v=g_cp.val;
    if(ch_i==0) v=(v&0x00FFFF)|((UINT32)cv<<16);
    else if(ch_i==1) v=(v&0xFF00FF)|((UINT32)cv<<8);
    else v=(v&0xFFFF00)|(UINT32)cv;
    g_cp.val=v;
    if(g_cp.target){ *g_cp.target=v&0xFFFFFF; if(g_cp.apply) g_cp.apply(); }
}

static int cp_bar_hit(int cx, int cy, int cw, int mx, int my)
{
    for(int i=0;i<3;i++){ int bx,by,bw,bh; cp_bar(cx,cy,cw,i,&bx,&by,&bw,&bh);
        if(mx>=bx-4 && mx<=bx+bw+4 && my>=by-3 && my<=by+bh+3) return i; }
    return -1;
}

static int cp_event(wm_window *w, const wm_event *ev)
{
    int cw=wm_client_w(w), ch=wm_client_h(w);
    int cx=0, cy=0;                            /* client-relative in ev coords */
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC){ if(g_cp.target){*g_cp.target=g_cp.orig&0xFFFFFF; if(g_cp.apply)g_cp.apply();} return WM_CLOSE_REQUEST; }
            if(ev->unicode==CHAR_CR) return WM_CLOSE_REQUEST;
            return 0;
        case WM_EV_MOUSE_DOWN: {
            wm_button bb[2]; int nb=bar_build(cw,ch,CP_IDS,CP_LB,2,0,bb);
            for(int i=0;i<nb;i++) if(wm_button_hit(&bb[i],ev->mx,ev->my)){ g_cp.b_press=bb[i].id; return 0; }
            int bi=cp_bar_hit(cx,cy,cw,ev->mx,ev->my);
            if(bi>=0){ g_cp.drag=bi; cp_set_from_x(bi,cx,cy,cw,ev->mx); }
            return 0; }
        case WM_EV_MOUSE_MOVE:
            if(g_cp.drag>=0){ cp_set_from_x(g_cp.drag,cx,cy,cw,ev->mx); return 0; }
            { wm_button bb[2]; int nb=bar_build(cw,ch,CP_IDS,CP_LB,2,0,bb); g_cp.b_hover=0;
              for(int i=0;i<nb;i++) if(wm_button_hit(&bb[i],ev->mx,ev->my)) g_cp.b_hover=bb[i].id; }
            return 0;
        case WM_EV_MOUSE_UP: {
            g_cp.drag=-1;
            if(g_cp.b_press){ wm_button bb[2]; int nb=bar_build(cw,ch,CP_IDS,CP_LB,2,0,bb); int p=g_cp.b_press; g_cp.b_press=0;
                for(int i=0;i<nb;i++) if(bb[i].id==p && wm_button_hit(&bb[i],ev->mx,ev->my)){
                    if(p==CP_CANCEL_ID && g_cp.target){*g_cp.target=g_cp.orig&0xFFFFFF; if(g_cp.apply)g_cp.apply();}
                    return WM_CLOSE_REQUEST; } }
            return 0; }
        case WM_EV_CLOSE: g_cp.win=NULL; return 0;
        default: return 0;
    }
}

void tool_colorpicker_open(unsigned int *target, void (*apply)(void))
{
    if(g_cp.win || !target) return;
    g_cp.target=target; g_cp.orig=*target; g_cp.val=(*target)&0xFFFFFF;
    g_cp.apply=apply; g_cp.drag=-1; g_cp.b_hover=g_cp.b_press=0;
    g_cp.bars_cw=-1;                 /* force the gradient bars to draw once */
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*40/100; if(ww<420)ww=420; if(ww>560)ww=560; if(ww>W-40)ww=W-40;
    int wh=H*36/100; if(wh<240)wh=240; if(wh>340)wh=340; if(wh>H-40)wh=H-40;
    g_cp.win=wm_open("Colour Picker", ww, wh, cp_draw, cp_event, &g_cp);
}

void tool_settings_open(void)
{
    if(g_set.win) return; g_set.sel=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*52/100; if(ww<520)ww=520; if(ww>760)ww=760; if(ww>W-40)ww=W-40;
    int wh=H*58/100; if(wh<360)wh=360; if(wh>600)wh=600; if(wh>H-40)wh=H-40;
    g_set.win=wm_open("Theme / Settings", ww, wh, set_draw, set_event, &g_set);
}

/* ==========================================================================
 * The generic list tools - one tstate each.
 * ========================================================================== */
enum { K_DISK=0, K_GPT, K_PART, K_MEMMAP, K_EFIVARS, K_BOOTMGR, K_SYSINFO, K_KEYTEST };
static tstate g_disk, g_gpt, g_part, g_mem, g_vars, g_boot, g_sys, g_key;

static tstate *kind_state(int k)
{ switch(k){ case K_DISK:return &g_disk; case K_GPT:return &g_gpt; case K_PART:return &g_part;
    case K_MEMMAP:return &g_mem; case K_EFIVARS:return &g_vars; case K_BOOTMGR:return &g_boot;
    case K_SYSINFO:return &g_sys; case K_KEYTEST:return &g_key; default:return 0; } }

static const char *tool_header(int kind)
{
    switch(kind){
        case K_DISK:   return "Block devices  (Enter: GPT viewer)";
        case K_GPT:    return "GPT Viewer  ( [ / ] : change disk )";
        case K_PART:   return "Partitions  (Enter: browse if mountable)";
        case K_MEMMAP: return "UEFI Memory Map";
        case K_EFIVARS:return "EFI Variables  (Enter: hex value)";
        case K_BOOTMGR:return "Boot Manager  (BootOrder / Boot####)";
        case K_SYSINFO:return "System / Firmware Info";
        case K_KEYTEST:return "Key Tester  (press keys / click)";
        default:       return "";
    }
}

/* ---- Disk Info ---- */
static void build_disk(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    UINTN n=0; EFI_HANDLE *h=NULL;
    if(!gBS || EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gBlkGuid,NULL,&n,&h))||!h){
        LN0(c_warn); LNs("No block devices."); LNend(); t->selectable=0; return; }
    int row=0;
    for(UINTN i=0;i<n && row<TL_MAXLINES;i++){
        EFI_BLOCK_IO_PROTOCOL *b=NULL;
        if(EFI_ERROR(gBS->HandleProtocol(h[i],&gBlkGuid,(VOID**)&b))||!b||!b->Media) continue;
        EFI_BLOCK_IO_MEDIA *m=b->Media;
        UINT64 bytes=((UINT64)m->LastBlock+1)*(UINT64)m->BlockSize;
        LN0(c_fg);
        LNc('['); LNu(i); LNs("] "); LNs(m->LogicalPartition?"part ":"disk ");
        LNu(bytes>>20); LNs(" MiB  bs="); LNu(m->BlockSize);
        LNs("  lba="); LNu((UINT64)m->LastBlock+1);
        LNs("  id="); LNu(m->MediaId);
        if(m->RemovableMedia) LNs("  rm");
        if(!m->MediaPresent)  LNs("  no-media");
        if(m->ReadOnly)       LNs("  ro");
        t->aux[t->n]=(int)i;                 /* device index for Enter -> GPT */
        LNend(); row++;
    }
    gBS->FreePool(h);
    t->selectable=1;
    if(t->n==0){ LN0(c_warn); LNs("No usable media."); LNend(); t->selectable=0; }
}

/* ---- GPT Viewer ---- */
static void build_gpt(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    int cnt=0; EFI_BLOCK_IO_PROTOCOL *b=blk_idx(t->cur_dev,&cnt,NULL);
    t->dev_count=cnt;               /* cache for the [ / ] bracket-key handler */
    LN0(c_dim); LNs("disk "); LNu(t->cur_dev); LNs(" of "); LNu(cnt); LNs("  ([ / ] to change)"); LNend();
    if(!b||!b->Media){ LN0(c_warn); LNs("no such device"); LNend(); t->selectable=0; return; }
    EFI_BLOCK_IO_MEDIA *m=b->Media; UINT32 bs=m->BlockSize?m->BlockSize:512;
    if((UINT64)m->LastBlock<2){ LN0(c_warn); LNs("device too small for GPT"); LNend(); t->selectable=0; return; }
    UINT8 *hdr=NULL;
    if(!gBS||EFI_ERROR(gBS->AllocatePool(EfiLoaderData,bs,(VOID**)&hdr))||!hdr){ t->selectable=0; return; }
    if(EFI_ERROR(b->ReadBlocks(b,m->MediaId,1,bs,hdr))){ LN0(c_warn); LNs("LBA1 read failed"); LNend(); gBS->FreePool(hdr); t->selectable=0; return; }
    static const UINT8 sig[8]={'E','F','I',' ','P','A','R','T'};
    if(!mem_eq(hdr,sig,8)){ LN0(c_warn); LNs("no GPT signature (MBR or raw disk)"); LNend(); gBS->FreePool(hdr); t->selectable=0; return; }
    UINT32 rev=rd_u32(hdr+8);
    UINT64 myLba=rd_u64(hdr+24), altLba=rd_u64(hdr+32);
    UINT64 firstU=rd_u64(hdr+40), lastU=rd_u64(hdr+48);
    UINT64 entLba=rd_u64(hdr+72); UINT32 num=rd_u32(hdr+80), esz=rd_u32(hdr+84);
    LN0(c_fg); LNs("rev "); LNx(rev>>16,1); LNc('.'); LNx(rev&0xFFFF,1);
    LNs("  myLBA="); LNu(myLba); LNs(" altLBA="); LNu(altLba); LNend();
    LN0(c_fg); LNs("usable LBA "); LNu(firstU); LNs("..."); LNu(lastU); LNend();
    LN0(c_fg); LNs("disk GUID "); LN_guid(hdr+56); LNend();
    LN0(c_dim); LNs("entries="); LNu(num); LNs(" size="); LNu(esz); LNend();
    if(esz<128||esz>1024||num==0||num>512){ LN0(c_warn); LNs("bad GPT geometry"); LNend(); gBS->FreePool(hdr); t->selectable=0; return; }
    UINTN arrB=(UINTN)num*esz, secs=(arrB+bs-1)/bs, allocB=secs*bs;
    UINT8 *arr=NULL;
    if(!EFI_ERROR(gBS->AllocatePool(EfiLoaderData,allocB,(VOID**)&arr))&&arr){
        if(!EFI_ERROR(b->ReadBlocks(b,m->MediaId,entLba,allocB,arr))){
            int used=0;
            for(UINT32 i=0;i<num && t->n<TL_MAXLINES;i++){
                const UINT8 *e=arr+(UINTN)i*esz; int z=1; for(int j=0;j<16;j++) if(e[j]){z=0;break;} if(z) continue;
                used++; UINT64 s=rd_u64(e+32), en=rd_u64(e+40);
                LN0(c_fg); LNs("  ["); LNu(i); LNs("] ");
                const char *nm=gpt_name(e); LNs(nm?nm:"(type)");
                LNsp(1); LNu(((en-s+1)*(UINT64)bs)>>20); LNs("MiB LBA "); LNu(s); LNc('-'); LNu(en);
                char pn[40]; u2a((const CHAR16*)(e+56),pn,sizeof pn);
                if(pn[0]){ LNs(" \""); LNs(pn); LNs("\""); }
                LNend();
            }
            LN0(c_dim); LNu(used); LNs(" partition(s)"); LNend();
        }
        gBS->FreePool(arr);
    }
    gBS->FreePool(hdr);
    t->selectable=0;
}

/* ---- Partition Browser ---- */
static void build_part(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    UINTN n=0; EFI_HANDLE *h=NULL;
    if(!gBS || EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gBlkGuid,NULL,&n,&h))||!h){
        LN0(c_warn); LNs("No block devices."); LNend(); t->selectable=0; return; }
    int row=0;
    for(UINTN i=0;i<n && row<TL_MAXLINES;i++){
        EFI_BLOCK_IO_PROTOCOL *b=NULL;
        if(EFI_ERROR(gBS->HandleProtocol(h[i],&gBlkGuid,(VOID**)&b))||!b||!b->Media) continue;
        EFI_BLOCK_IO_MEDIA *m=b->Media; if(!m->MediaPresent) continue;
        if(!m->LogicalPartition) continue;          /* partitions only */
        UINT64 bytes=((UINT64)m->LastBlock+1)*(UINT64)m->BlockSize;
        const char *fs=fs_probe(b);
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs=NULL;
        int mountable=!EFI_ERROR(gBS->HandleProtocol(h[i],&gSfsGuid,(VOID**)&sfs)) && sfs;
        LN0(c_fg);
        LNc('['); LNu(i); LNs("] "); LNu(bytes>>20); LNs(" MiB  ");
        LNs(fs?fs:"unknown");
        if(mountable) LNs("  [mountable]");
        t->aux[t->n]=(int)i;
        LNend(); row++;
    }
    gBS->FreePool(h);
    t->selectable=1;
    if(t->n==0){ LN0(c_dim); LNs("No logical partitions found."); LNend(); t->selectable=0; }
}

/* ---- Memory Map ---- */
static const char *mem_type_name(UINT32 tp)
{
    switch(tp){
        case EfiReservedMemoryType:return "Reserved"; case EfiLoaderCode:return "LoaderCode";
        case EfiLoaderData:return "LoaderData"; case EfiBootServicesCode:return "BS-Code";
        case EfiBootServicesData:return "BS-Data"; case EfiRuntimeServicesCode:return "RT-Code";
        case EfiRuntimeServicesData:return "RT-Data"; case EfiConventionalMemory:return "Conventional";
        case EfiUnusableMemory:return "Unusable"; case EfiACPIReclaimMemory:return "ACPI-Reclaim";
        case EfiACPIMemoryNVS:return "ACPI-NVS"; case EfiMemoryMappedIO:return "MMIO";
        case EfiMemoryMappedIOPortSpace:return "MMIO-Port"; case EfiPalCode:return "PalCode";
        case EfiPersistentMemory:return "Persistent"; default:return "Other";
    }
}
static void build_memmap(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    t->selectable=0;
    if(!gBS){ LN0(c_warn); LNs("BootServices N/A"); LNend(); return; }
    UINTN sz=0,key=0,dsz=0; UINT32 dver=0;
    EFI_STATUS st=gBS->GetMemoryMap(&sz,NULL,&key,&dsz,&dver);
    if(st!=EFI_BUFFER_TOO_SMALL || sz==0){ LN0(c_warn); LNs("GetMemoryMap sizing failed"); LNend(); return; }
    sz+=dsz*8; UINT8 *map=NULL;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,sz,(VOID**)&map))||!map){ LN0(c_warn); LNs("alloc failed"); LNend(); return; }
    if(EFI_ERROR(gBS->GetMemoryMap(&sz,(EFI_MEMORY_DESCRIPTOR*)map,&key,&dsz,&dver))){ LN0(c_warn); LNs("GetMemoryMap failed"); LNend(); gBS->FreePool(map); return; }
    UINTN entries=sz/dsz; UINT64 totalPages=0, convPages=0;
    for(UINTN i=0;i<entries && t->n<TL_MAXLINES-3;i++){
        EFI_MEMORY_DESCRIPTOR *d=(EFI_MEMORY_DESCRIPTOR*)(map+i*dsz);
        totalPages+=d->NumberOfPages; if(d->Type==EfiConventionalMemory) convPages+=d->NumberOfPages;
        LN0(c_fg);
        LNs("0x"); LNx(d->PhysicalStart,12); LNs("  "); LNs(mem_type_name(d->Type));
        LNsp(2); LNu(d->NumberOfPages); LNs("pg "); LNu((d->NumberOfPages*4096)>>10); LNs("K");
        if(d->Attribute&EFI_MEMORY_RUNTIME) LNs(" RT");
        if(d->Attribute&EFI_MEMORY_WB) LNs(" WB"); else if(d->Attribute&EFI_MEMORY_UC) LNs(" UC");
        LNend();
    }
    LN0(c_accent); LNs("total RAM "); LNu((totalPages*4096)>>20); LNs(" MiB, free ");
    LNu((convPages*4096)>>20); LNs(" MiB, "); LNu(entries); LNs(" descriptors"); LNend();
    gBS->FreePool(map);
}

/* ---- EFI Variables ---- */
static void build_efivars(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    t->selectable=0;
    if(!gRT){ LN0(c_warn); LNs("RuntimeServices N/A"); LNend(); return; }
    static CHAR16 name[512]; EFI_GUID guid; name[0]=0;
    if(gBS) gBS->SetMem(&guid,sizeof(guid),0);
    int count=0;
    for(;;){
        UINTN nsz=sizeof(name);
        EFI_STATUS st=gRT->GetNextVariableName(&nsz,name,&guid);
        if(st==EFI_NOT_FOUND) break;
        if(EFI_ERROR(st)) break;
        if(t->n>=TL_MAXLINES) break;
        UINTN vsz=0; UINT32 attr=0;
        gRT->GetVariable(name,&guid,&attr,&vsz,NULL);   /* -> BUFFER_TOO_SMALL, sets vsz */
        char an[80]; u2a(name,an,80);
        LN0(c_fg); LNs(an); LNsp(1); LNs("  "); LNu(vsz); LNs("B ");
        if(attr&EFI_VARIABLE_NON_VOLATILE) LNc('N');
        if(attr&EFI_VARIABLE_BOOTSERVICE_ACCESS) LNc('B');
        if(attr&EFI_VARIABLE_RUNTIME_ACCESS) LNc('R');
        LNs("  {"); LNx(guid.Data1,8); LNc('}');
        t->aux[t->n]=count;
        LNend(); count++;
    }
    t->selectable=(t->n>0);
    if(t->n==0){ LN0(c_dim); LNs("No variables enumerated."); LNend(); }
}

/* Fetch the idx-th variable name/guid (re-enumerate). Returns 1 on success. */
static int efivar_nth(int idx, CHAR16 *outname, UINTN outcap, EFI_GUID *outguid)
{
    if(!gRT) return 0;
    static CHAR16 name[512]; EFI_GUID guid; name[0]=0;
    if(gBS) gBS->SetMem(&guid,sizeof(guid),0);
    int i=0;
    for(;;){
        UINTN nsz=sizeof(name);
        EFI_STATUS st=gRT->GetNextVariableName(&nsz,name,&guid);
        if(EFI_ERROR(st)) return 0;
        if(i==idx){ UINTN k=0; for(;name[k]&&k+1<outcap/2;k++) outname[k]=name[k]; outname[k]=0; *outguid=guid; return 1; }
        i++;
    }
}

/* ---- Boot Manager ---- */
static int get_global_var(const CHAR16 *nm, void *buf, UINTN cap, UINTN *outsz, UINT32 *attr)
{
    if(!gRT) return 0; UINTN sz=cap;
    EFI_STATUS st=gRT->GetVariable((CHAR16*)nm,&gGlobalVar,attr,&sz,buf);
    if(EFI_ERROR(st)) return 0; if(outsz)*outsz=sz; return 1;
}
static void build_bootmgr(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    t->selectable=0;
    if(!gRT){ LN0(c_warn); LNs("RuntimeServices N/A"); LNend(); return; }
    UINT16 cur=0xFFFF, tmo=0; UINTN sz=0;
    if(get_global_var(L"BootCurrent",&cur,sizeof(cur),&sz,NULL)){ LN0(c_accent); LNs("BootCurrent = Boot"); LNx(cur,4); LNend(); }
    if(get_global_var(L"Timeout",&tmo,sizeof(tmo),&sz,NULL)){ LN0(c_dim); LNs("Timeout = "); LNu(tmo); LNs(" s"); LNend(); }
    static UINT16 order[256]; UINTN osz=sizeof(order);
    if(!get_global_var(L"BootOrder",order,sizeof(order),&osz,NULL) || osz<2){
        LN0(c_warn); LNs("No BootOrder variable."); LNend(); return; }
    int no=(int)(osz/2);
    LN0(c_dim); LNs("BootOrder: "); LNu(no); LNs(" entries"); LNend();
    static UINT8 lo[1024];
    for(int i=0;i<no && t->n<TL_MAXLINES;i++){
        CHAR16 vn[9]; static const char hx[]="0123456789ABCDEF";
        vn[0]='B';vn[1]='o';vn[2]='o';vn[3]='t';
        vn[4]=hx[(order[i]>>12)&0xF];vn[5]=hx[(order[i]>>8)&0xF];vn[6]=hx[(order[i]>>4)&0xF];vn[7]=hx[order[i]&0xF];vn[8]=0;
        UINTN lsz=sizeof(lo);
        if(!get_global_var(vn,lo,sizeof(lo),&lsz,NULL) || lsz<6){
            LN0(c_dim); LNs("  Boot"); LNx(order[i],4); LNs("  (unreadable)"); LNend(); continue; }
        UINT32 attrs=rd_u32(lo);
        char desc[80]; u2a((const CHAR16*)(lo+6),desc,80);
        LN0(c_fg); LNs("  Boot"); LNx(order[i],4); LNs(order[i]==cur?" * ":"   ");
        LNs((attrs&1)?"[on] ":"[off] "); LNs(desc);
        LNend();
    }
}

/* ---- System / Firmware Info ---- */
#if defined(__x86_64__) || defined(_M_X64)
static void cpuid(UINT32 leaf, UINT32 sub, UINT32 r[4])
{
    UINT32 a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(sub));
    r[0]=a; r[1]=b; r[2]=c; r[3]=d;
}
#else
static void cpuid(UINT32 leaf, UINT32 sub, UINT32 r[4])
{
    (void)leaf; (void)sub;
    r[0]=r[1]=r[2]=r[3]=0;
}
#endif
static void build_sysinfo(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES);
    t->selectable=0;
    /* firmware vendor / revision + UEFI spec rev */
    if(gST){
        char fv[80]; u2a(gST->FirmwareVendor,fv,80);
        LN0(c_accent); LNs("Firmware: "); LNs(fv[0]?fv:"(none)"); LNend();
        LN0(c_fg); LNs("FW revision 0x"); LNx(gST->FirmwareRevision,8); LNend();
        LN0(c_fg); LNs("UEFI spec "); LNu((gST->Hdr.Revision>>16)&0xFFFF); LNc('.'); LNu(gST->Hdr.Revision&0xFFFF); LNend();
    }
    LN0(c_fg); LNs("Arch: x86_64 (Microsoft x64 ABI)"); LNend();

    /* CPU vendor + brand via cpuid */
    UINT32 r[4]; cpuid(0,0,r);
    char vend[13]; ((UINT32*)vend)[0]=r[1]; ((UINT32*)vend)[1]=r[3]; ((UINT32*)vend)[2]=r[2]; vend[12]=0;
    LN0(c_fg); LNs("CPU vendor: "); LNs(vend); LNend();
    cpuid(0x80000000u,0,r);
    if(r[0]>=0x80000004u){
        char brand[49]; UINT32 *bp=(UINT32*)brand;
        for(int i=0;i<3;i++){ UINT32 rr[4]; cpuid(0x80000002u+i,0,rr); bp[i*4+0]=rr[0];bp[i*4+1]=rr[1];bp[i*4+2]=rr[2];bp[i*4+3]=rr[3]; }
        brand[48]=0; char *p=brand; while(*p==' ')p++;
        LN0(c_fg); LNs("CPU: "); LNs(p); LNend();
    }

    /* GOP mode */
    if(gBS){
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=NULL;
        if(!EFI_ERROR(gBS->LocateProtocol(&gGopGuid,NULL,(VOID**)&gop)) && gop && gop->Mode && gop->Mode->Info){
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=gop->Mode->Info;
            LN0(c_fg); LNs("GOP: "); LNu(mi->HorizontalResolution); LNc('x'); LNu(mi->VerticalResolution);
            LNs(" pitch="); LNu(mi->PixelsPerScanLine); LNs(" fmt=");
            LNs(mi->PixelFormat==PixelBlueGreenRedReserved8BitPerColor?"BGRX":
                mi->PixelFormat==PixelRedGreenBlueReserved8BitPerColor?"RGBX":"other"); LNend();
            LN0(c_dim); LNs("  fb 0x"); LNx(gop->Mode->FrameBufferBase,1); LNs(" size "); LNu((UINT64)gop->Mode->FrameBufferSize>>20); LNs(" MiB"); LNend();
        }
    }

    /* total RAM from the memory map */
    if(gBS){
        UINTN sz=0,key=0,dsz=0; UINT32 dver=0;
        if(gBS->GetMemoryMap(&sz,NULL,&key,&dsz,&dver)==EFI_BUFFER_TOO_SMALL && sz){
            sz+=dsz*8; UINT8 *map=NULL;
            if(!EFI_ERROR(gBS->AllocatePool(EfiLoaderData,sz,(VOID**)&map))&&map){
                if(!EFI_ERROR(gBS->GetMemoryMap(&sz,(EFI_MEMORY_DESCRIPTOR*)map,&key,&dsz,&dver))){
                    UINT64 pages=0; UINTN entries=sz/dsz;
                    for(UINTN i=0;i<entries;i++){ EFI_MEMORY_DESCRIPTOR *d=(EFI_MEMORY_DESCRIPTOR*)(map+i*dsz); pages+=d->NumberOfPages; }
                    LN0(c_fg); LNs("Total RAM: "); LNu((pages*4096)>>20); LNs(" MiB"); LNend();
                }
                gBS->FreePool(map);
            }
        }
    }

    /* Secure Boot / Setup Mode */
    if(gRT){
        UINT8 sb=0,sm=0; UINTN vs=0;
        if(get_global_var(L"SecureBoot",&sb,1,&vs,NULL)){ LN0(c_fg); LNs("SecureBoot: "); LNs(sb?"ENABLED":"disabled"); LNend(); }
        if(get_global_var(L"SetupMode",&sm,1,&vs,NULL)){ LN0(c_dim); LNs("SetupMode: "); LNs(sm?"setup":"user"); LNend(); }
    }

    /* firmware setup support */
    LN0(c_dim); LNs("Firmware Setup: "); LNs(tools_firmware_setup_supported()?"supported":"not advertised"); LNend();
    LN0(c_dim); LNs("Block devices: "); LNu(blk_count()); LNend();
}

/* ---- Key Tester (rolling log) ---- */
static void keytest_seed(tstate *t)
{
    tl_begin(t->text,t->col,&t->n,TL_MAXLINES); t->selectable=0;
    LN0(c_dim); LNs("Press any key, or click/drag inside this window."); LNend();
    LN0(c_dim); LNs("Esc closes."); LNend();
}
static void keytest_log_key(tstate *t, UINT16 scan, CHAR16 uni)
{
    if(t->n>=TL_MAXLINES){ /* drop oldest: shift up by 1 */
        for(int i=1;i<t->n;i++){ scopy(t->text[i-1],t->text[i],TL_COLS); t->col[i-1]=t->col[i]; }
        t->n--;
    }
    int save=t->n; tl_begin(t->text,t->col,&t->n,TL_MAXLINES); t->n=save;   /* keep existing */
    LN0(c_fg); LNs("key scan=0x"); LNx(scan,4); LNs(" uni=0x"); LNx(uni,4);
    if(uni>=0x20&&uni<0x7f){ LNs(" '"); LNc((char)uni); LNc('\''); }
    LNend();
    t->scroll=t->n;   /* follow tail */
}
static void keytest_log_mouse(tstate *t, const char *what, int mx, int my)
{
    if(t->n>=TL_MAXLINES){ for(int i=1;i<t->n;i++){ scopy(t->text[i-1],t->text[i],TL_COLS); t->col[i-1]=t->col[i]; } t->n--; }
    int save=t->n; tl_begin(t->text,t->col,&t->n,TL_MAXLINES); t->n=save;
    LN0(c_accent); LNs(what); LNs(" @ "); LNu(mx); LNc(','); LNu(my); LNend();
    t->scroll=t->n;
}

/* ==========================================================================
 * Rebuild + activate dispatch for the generic list tools.
 * ========================================================================== */
static void tool_rebuild(tstate *t)
{
    switch(t->kind){
        case K_DISK:    build_disk(t);    break;
        case K_GPT:     build_gpt(t);     break;
        case K_PART:    build_part(t);    break;
        case K_MEMMAP:  build_memmap(t);  break;
        case K_EFIVARS: build_efivars(t); break;
        case K_BOOTMGR: build_bootmgr(t); break;
        case K_SYSINFO: build_sysinfo(t); break;
        case K_KEYTEST: keytest_seed(t);  break;
        default: break;
    }
    if(t->sel>=t->n) t->sel=t->n-1; if(t->sel<0)t->sel=0;
}

static void tool_activate_row(tstate *t)
{
    if(!t->selectable || t->sel<0 || t->sel>=t->n) return;
    if(t->kind==K_DISK){
        if(gBS) gBS->SetMem(&g_hexreq,sizeof(g_hexreq),0);
        g_gpt.cur_dev=t->aux[t->sel];
        tool_gptview_open();
    } else if(t->kind==K_PART){
        /* open File Browser on the partition if it exposes a filesystem */
        int devidx=t->aux[t->sel];
        EFI_HANDLE bh=NULL; blk_idx(devidx,NULL,&bh);
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs=NULL; EFI_FILE_PROTOCOL *root=NULL;
        if(bh && gBS && !EFI_ERROR(gBS->HandleProtocol(bh,&gSfsGuid,(VOID**)&sfs)) && sfs &&
           !EFI_ERROR(sfs->OpenVolume(sfs,&root)) && root){
            g_fbreq.active=1; g_fbreq.root=root; tool_filebrowse_open();
        }
    } else if(t->kind==K_EFIVARS){
        CHAR16 nm[512]; EFI_GUID guid;
        if(efivar_nth(t->aux[t->sel],nm,sizeof(nm),&guid) && gRT && gBS){
            UINTN vsz=0; UINT32 attr=0;
            gRT->GetVariable(nm,&guid,&attr,&vsz,NULL);
            if(vsz>0 && vsz<=HEX_CAP){
                UINT8 *buf=NULL;
                if(!EFI_ERROR(gBS->AllocatePool(EfiLoaderData,vsz,(VOID**)&buf))&&buf){
                    if(!EFI_ERROR(gRT->GetVariable(nm,&guid,&attr,&vsz,buf))){
                        gBS->SetMem(&g_hexreq,sizeof(g_hexreq),0);
                        g_hexreq.src=HEXSRC_MEM; g_hexreq.mem=buf; g_hexreq.memsize=vsz;
                        { char an[40]; u2a(nm,an,40); scopy(g_hexreq.title,an,WM_TITLE_LEN); }
                        tool_hexview_open();
                    } else gBS->FreePool(buf);
                }
            }
        }
    }
}

/* ==========================================================================
 * Shared list draw + event callbacks.
 * ========================================================================== */
/* Bottom-bar ids: LS_ACT = [Refresh] (re-enumerate) or [Clear] (Key Tester). */
enum { LS_ACT=1, LS_CLOSE=2 };
static int list_btns(int cw, int ch, int kind, wm_button *out)
{
    int ids[1]={LS_ACT};
    const char *lb[1]={ kind==K_KEYTEST ? "Clear" : "Refresh" };
    return bar_build(cw,ch,ids,lb,1,LS_CLOSE,out);
}

/* Cached bar geometry (launch_btns_cached idiom): list_btns only depends on
 * cw/ch/scale + the fixed kind, none of which change on a plain pointer move,
 * and its buttons are always enabled -- so rebuild lazily on resize and reuse
 * the cached wm_button[] for every hover/click hit-test and the draw. */
static int list_btns_cached(tstate *t, int cw, int ch, wm_button **out)
{
    int sc=gsc();
    if(!t->bar_valid || t->bar_cw!=cw || t->bar_ch!=ch || t->bar_sc!=sc){
        t->bar_nb=list_btns(cw,ch,t->kind,t->bar_b);
        t->bar_cw=cw; t->bar_ch=ch; t->bar_sc=sc; t->bar_valid=1;
    }
    *out=t->bar_b;
    return t->bar_nb;
}

static void list_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    tstate *t=(tstate*)wm_user(w); if(!t) return;
    int cch=bar_fits(ch)?bar_content_h(ch):ch;
    render_list(cx,cy,cw,cch,tool_header(t->kind),t->text,t->col,t->n,&t->scroll,t->sel,t->selectable);
    wm_button *b; int nb=list_btns_cached(t,cw,ch,&b);
    if(nb) bar_draw(cx,cy,cw,ch,b,nb,t->b_hover,t->b_press);
}

static int list_event(wm_window *w, const wm_event *ev)
{
    tstate *t=(tstate*)wm_user(w); if(!t) return 0;
    int ch=wm_client_h(w), cw=wm_client_w(w);
    int cch=bar_fits(ch)?bar_content_h(ch):ch;
    int rows=L_rows(cch,1);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(t->kind==K_KEYTEST){ keytest_log_key(t,ev->scancode,ev->unicode); return 0; }
            if(t->kind==K_GPT){
                int cnt=t->dev_count;   /* cached by the last build_gpt() */
                if((ev->unicode=='[' || ev->scancode==SCAN_LEFT) && cnt>0){ t->cur_dev=(t->cur_dev+cnt-1)%cnt; tool_rebuild(t); return 0; }
                if((ev->unicode==']' || ev->scancode==SCAN_RIGHT) && cnt>0){ t->cur_dev=(t->cur_dev+1)%cnt; tool_rebuild(t); return 0; }
            }
            if(ev->unicode==CHAR_CR){ tool_activate_row(t); return 0; }
            if(ev->unicode=='r'||ev->unicode=='R'){ tool_rebuild(t); return 0; }
            L_key_nav(ev->scancode,&t->scroll,&t->sel,t->n,rows,t->selectable);
            return 0;
        case WM_EV_MOUSE_WHEEL:
            if(t->selectable){
                t->sel-=ev->wheel;
                if(t->sel<0)t->sel=0; if(t->sel>t->n-1)t->sel=t->n-1;
            } else {
                t->scroll-=ev->wheel;
                if(t->scroll>t->n-rows)t->scroll=t->n-rows;
                if(t->scroll<0)t->scroll=0;
            }
            return 0;
        case WM_EV_MOUSE_MOVE: {
            wm_button *b; int nb=list_btns_cached(t,cw,ch,&b);
            t->b_hover=bar_hit(b,nb,ev->mx,ev->my);
            return 0; }
        case WM_EV_MOUSE_DOWN: {
            wm_button *b; int nb=list_btns_cached(t,cw,ch,&b);
            int id=bar_hit(b,nb,ev->mx,ev->my);
            if(id){ t->b_press=id; return 0; }
            if(t->kind==K_KEYTEST){ keytest_log_mouse(t,ev->button?"R-click":"click",ev->mx,ev->my); return 0; }
            if(t->selectable){
                int r=L_row_at(ev->my,t->scroll,t->n,rows,1);
                if(r>=0){ if(r==t->sel) tool_activate_row(t); else t->sel=r; }
            }
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!t->b_press) return 0;
            wm_button *b; int nb=list_btns_cached(t,cw,ch,&b);
            int id=bar_hit(b,nb,ev->mx,ev->my), p=t->b_press;
            t->b_press=0;
            if(id==p){
                if(p==LS_CLOSE) return WM_CLOSE_REQUEST;
                if(t->kind==K_KEYTEST) keytest_seed(t); else tool_rebuild(t);
            }
            return 0; }
        case WM_EV_CLOSE:
            t->win=NULL; return 0;
        default: return 0;
    }
}

/* Generic opener for the list tools. */
static void list_open(int kind, const char *title, int wpct, int hpct)
{
    tstate *t=kind_state(kind); if(!t) return;
    if(t->win) return;                    /* idempotent single instance */
    int cur=t->cur_dev;                   /* preserve GPT disk selection */
    tstate_zero(t);
    t->kind=kind; t->cur_dev=cur;
    tool_rebuild(t);
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*wpct/100; if(ww<420)ww=420; if(ww>900)ww=900; if(ww>W-40)ww=W-40;
    int wh=H*hpct/100; if(wh<280)wh=280; if(wh>660)wh=660; if(wh>H-40)wh=H-40;
    t->win=wm_open(title, ww, wh, list_draw, list_event, t);
}

void tool_diskinfo_open(void){ list_open(K_DISK,   "Disk Info",     58, 55); }
void tool_gptview_open(void) { list_open(K_GPT,    "GPT Viewer",    64, 60); }
void tool_partbrowse_open(void){ list_open(K_PART, "Partition Browser", 58, 55); }
void tool_memmap_open(void)  { list_open(K_MEMMAP, "Memory Map",    70, 62); }
void tool_efivars_open(void) { list_open(K_EFIVARS,"EFI Variables", 62, 62); }
void tool_bootmgr_open(void) { list_open(K_BOOTMGR,"Boot Manager",  62, 58); }
void tool_sysinfo_open(void) { list_open(K_SYSINFO,"System Info",   60, 60); }
void tool_keytest_open(void) { list_open(K_KEYTEST,"Key Tester",    50, 52); }

/* ==========================================================================
 * Icon-name resolution.
 * ========================================================================== */
char *tools_icon_path(const char *name, char *out, unsigned long out_len)
{
    if(!out || out_len==0) return out;
    if(!name){ out[0]=0; return out; }
    int hassep=0, n=0; for(const char*p=name;*p;p++){ n++; if(*p=='/'||*p=='\\') hassep=1; }
    int isimg=0;
    if(n>=4){ const char*e=name+n-4; if(ci_eq(e,".tga")||ci_eq(e,".bmp")) isimg=1; }
    if(hassep||isimg){ scopy(out,name,(int)out_len); return out; }
    /* rewrite short name -> /forebo/icons/<name>.tga */
    const char *pre="/forebo/icons/", *suf=".tga";
    unsigned long p=0;
    for(const char*s=pre;*s&&p+1<out_len;) out[p++]=*s++;
    for(const char*s=name;*s&&p+1<out_len;) out[p++]=*s++;
    for(const char*s=suf;*s&&p+1<out_len;) out[p++]=*s++;
    out[p]=0;
    return out;
}

/* ==========================================================================
 * Firmware / BIOS setup (OsIndications).
 * ========================================================================== */
int tools_firmware_setup_supported(void)
{
    if(!gRT) return 0;
    UINT64 sup=0; UINTN sz=sizeof(sup);
    EFI_STATUS st=gRT->GetVariable(L"OsIndicationsSupported",&gGlobalVar,NULL,&sz,&sup);
    if(EFI_ERROR(st)) return 0;
    return (sup & EFI_OS_INDICATIONS_BOOT_TO_FW_UI) ? 1 : 0;
}

int tools_enter_firmware_setup(void)
{
    if(!gRT) return 1;
    if(!tools_firmware_setup_supported()) return -1;
    UINT64 ind=0; UINTN sz=sizeof(ind); UINT32 attr=0;
    /* default 0 if absent */
    if(EFI_ERROR(gRT->GetVariable(L"OsIndications",&gGlobalVar,&attr,&sz,&ind))){ ind=0; }
    ind |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
    EFI_STATUS st=gRT->SetVariable(L"OsIndications",&gGlobalVar,
        EFI_VARIABLE_NON_VOLATILE|EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS,
        sizeof(ind),&ind);
    if(EFI_ERROR(st)) return (int)(st & 0xFF ? (st&0xFF) : 2);
    /* Reboot into firmware setup. Does not return on success. */
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    return 0;
}

/* ==========================================================================
 * The registry.
 * ========================================================================== */
const struct forebo_tool forebo_tools[] = {
    { "Disk Info",         "Block-IO devices: size, block size, flags",        "disk",     tool_diskinfo_open  },
    { "GPT Viewer",        "GPT header + partition table of a disk",            "disk",     tool_gptview_open   },
    { "Partition Browser", "Detected filesystem + size per partition",          "disk",     tool_partbrowse_open},
    { "File Browser",      "Navigate the ESP tree; open files in the hex view", "text",     tool_filebrowse_open},
    { "Hex Viewer",        "Hexdump a file or a raw disk sector",               "text",     tool_hexview_open   },
    { "Memory Map",        "UEFI GetMemoryMap descriptors + totals",            "gear",     tool_memmap_open    },
    { "EFI Variables",     "Enumerate NVRAM variables; hex their values",       "gear",     tool_efivars_open   },
    { "Boot Manager",      "BootOrder / Boot#### / BootCurrent",                "grub",     tool_bootmgr_open   },
    { "System Info",       "Firmware, UEFI rev, GOP, CPU, RAM, SecureBoot",     "gear",     tool_sysinfo_open   },
    { "Theme / Settings",  "Live theme colours + cursor/anim toggles",          "gear",     tool_settings_open  },
    { "Key Tester",        "Show scancode + unicode of pressed keys",           "terminal", tool_keytest_open   },
    { "Image Viewer",      "Browse a drive; preview BMP/TGA pictures",          "text",     tool_imgview_open   },
    { "Clone Drive",       "Clone a disk to another disk or an ESP image file", "disk",     tool_clone_open     },
    { "Undelete / Carve",  "Recover deleted files by signature carving (JPEG/PNG/PDF/ZIP/GIF)", "disk", tool_undelete_open },
    { "Calculator",        "Decimal calculator + f(x) grapher (sin cos sqrt ^ %)", "gear",  tool_calc_open      },
    { "Clock",             "Firmware RTC date + time",                          "gear",     tool_clock_open     },
    { "System Monitor",    "Live RAM / GOP / firmware / uptime",                "gear",     tool_sysmon_open    },
};
const int forebo_tools_count = (int)(sizeof(forebo_tools)/sizeof(forebo_tools[0]));

/* ==========================================================================
 * The Tools launcher window: a 2-level navigator over forebo_categories[]
 * (see tools_cat.h). Level 0 lists the categories; Enter/click drills into
 * the selected category's tools (level 1); Backspace/Esc goes back up a
 * level (Esc at level 0 closes the launcher). All list geometry, hit-testing
 * and scrolling are level-independent via the launch_cur_*() accessors.
 * ========================================================================== */
typedef struct {
    wm_window       *win;
    int              cur_cat;       /* -1 = category list, >=0 = its tools    */
    int              sel;
    int              scroll;        /* first visible row (viewport)           */
    int              hover;         /* row under the pointer, -1 = none       */
    int              b_hover, b_press; /* button-bar hover / pressed id       */
    int              icons_tried;
    int              icons_cat;     /* cur_cat the cached icons belong to     */
    struct img_image icon[32];
    EFI_FILE_PROTOCOL *root;        /* cached ESP root, opened once per window */
    char             breadcrumb[96];/* cached header text, rebuilt on launch_goto */
    wm_button        bar_b[2];      /* cached button-bar geometry ... */
    int              bar_nb;        /* ... and its button count       */
    int              bar_cw, bar_ch, bar_sc; /* dims/scale it was built for   */
    int              bar_valid;
} launchstate;
static launchstate g_launch = { .cur_cat=-1, .icons_cat=-2 };

/* Launcher geometry: two text lines per row (title + description), so the
 * row stride is 2*L_lineH()+6*sc and ALL of draw / hit-test / window-height /
 * scrollbar math derive from these helpers. The header reserves TWO lines
 * (breadcrumb + hint), so LA_rowsY() = gap + 2*lineH + gap. */
static int LA_stride(void){ return 2*L_lineH()+6*gsc(); }
static int LA_rowsY(void){ int sc=gsc(); return 6*sc+2*L_lineH()+6*sc; } /* first row top (client y) */

/* Current-level accessors: cur_cat<0 -> the category list; cur_cat>=0 -> that
 * category's tools. Every list consumer (draw/hit-test/scroll/icons) goes
 * through these so the two levels share one code path. Out-of-range cur_cat
 * yields an empty list instead of indexing past forebo_categories[]. */
static int launch_cur_count(void)
{
    if(g_launch.cur_cat<0) return forebo_categories_count;
    if(g_launch.cur_cat>=forebo_categories_count) return 0;
    return forebo_categories[g_launch.cur_cat].count;
}
static const struct forebo_tool *launch_cur_tools(void)
{
    if(g_launch.cur_cat<0 || g_launch.cur_cat>=forebo_categories_count) return NULL;
    return forebo_categories[g_launch.cur_cat].tools;
}
static const char *launch_cur_name(int i)
{
    if(g_launch.cur_cat<0 || g_launch.cur_cat>=forebo_categories_count)
        return forebo_categories[i].name;
    return forebo_categories[g_launch.cur_cat].tools[i].name;
}
static const char *launch_cur_desc(int i)
{
    if(g_launch.cur_cat<0 || g_launch.cur_cat>=forebo_categories_count)
        return forebo_categories[i].desc;
    return forebo_categories[g_launch.cur_cat].tools[i].desc;
}
static const char *launch_cur_icon(int i)
{
    if(g_launch.cur_cat<0 || g_launch.cur_cat>=forebo_categories_count)
        return forebo_categories[i].icon;
    return forebo_categories[g_launch.cur_cat].tools[i].icon;
}

/* Visible rows in the viewport (reserving the button bar when it fits). */
static int launch_vrows(int ch)
{
    int bot=bar_fits(ch)?bar_content_h(ch):ch-4*gsc();
    int v=(bot-LA_rowsY())/LA_stride(); return v<1?1:v;
}

/* Keep sel valid + visible; clamp scroll (selection-follow, render_list style). */
static void launch_clamp(int vrows)
{
    int n=launch_cur_count();
    if(g_launch.sel<0) g_launch.sel=0;
    if(g_launch.sel>n-1) g_launch.sel=n-1;
    if(g_launch.sel<g_launch.scroll) g_launch.scroll=g_launch.sel;
    else if(g_launch.sel>=g_launch.scroll+vrows) g_launch.scroll=g_launch.sel-vrows+1;
    if(g_launch.scroll>n-vrows) g_launch.scroll=n-vrows;
    if(g_launch.scroll<0) g_launch.scroll=0;
}

/* Button-bar ids + builder: [Open] (activates sel) ... [Close]. */
enum { LA_OPEN=1, LA_CLOSE=2 };
static int launch_btns(int cw, int ch, wm_button *out)
{
    int ids[1]={LA_OPEN}; const char *lb[1]={"Open"};
    return bar_build(cw,ch,ids,lb,1,LA_CLOSE,out);
}

/* Cached button-bar geometry: bar_build() only depends on cw/ch/scale, which
 * change on resize, not on every pointer move -- so rebuild it lazily and
 * reuse the cached wm_button[] for hit-testing (WM_EV_MOUSE_MOVE et al). */
static int launch_btns_cached(int cw, int ch, wm_button **out)
{
    int sc=gsc();
    if(!g_launch.bar_valid || g_launch.bar_cw!=cw || g_launch.bar_ch!=ch || g_launch.bar_sc!=sc){
        g_launch.bar_nb=launch_btns(cw,ch,g_launch.bar_b);
        g_launch.bar_cw=cw; g_launch.bar_ch=ch; g_launch.bar_sc=sc;
        g_launch.bar_valid=1;
    }
    *out=g_launch.bar_b;
    return g_launch.bar_nb;
}

static void launch_load_icons(void)
{
    /* Reload whenever the level changed (icons_tried reset by launch_goto, or
     * icons_cat mismatch after a launcher reopen on a different level). */
    if(g_launch.icons_tried && g_launch.icons_cat==g_launch.cur_cat) return;
    g_launch.icons_tried=1; g_launch.icons_cat=g_launch.cur_cat;
    for(int i=0;i<32;i++) img_free(&g_launch.icon[i]);  /* drop previous level */
    /* Root is opened once per launcher window (tools_launcher_open) and
     * closed on WM_EV_CLOSE; it stays valid for every category navigation. */
    if(!g_launch.root) return;
    int n=launch_cur_count();
    if(n>32) n=32;        /* icon cache cap: every category list is < 32 entries */
    for(int i=0;i<n;i++){
        char ap[FOREB_CFG_PATH_LEN]; tools_icon_path(launch_cur_icon(i), ap, sizeof(ap));
        CHAR16 wp[FOREB_CFG_PATH_LEN*2]; esp_ascii_to_char16(ap, wp, FOREB_CFG_PATH_LEN*2);
        if(img_load_file(g_launch.root, wp, &g_launch.icon[i])!=EFI_SUCCESS){
            if(gBS) gBS->SetMem(&g_launch.icon[i],sizeof(g_launch.icon[i]),0);
        }
    }
}

/* Rebuild the breadcrumb header text for the current level into the cache
 * ("ForeB Tools" / "ForeB Tools > <cat>"); called only when cur_cat changes. */
static void launch_build_breadcrumb(void)
{
    char *bc=g_launch.breadcrumb;
    scopy(bc,"ForeB Tools",(int)sizeof(g_launch.breadcrumb));
    if(g_launch.cur_cat>=0 && g_launch.cur_cat<forebo_categories_count){
        int l=slen(bc); scopy(bc+l," > ",(int)sizeof(g_launch.breadcrumb)-l); l=slen(bc);
        scopy(bc+l,forebo_categories[g_launch.cur_cat].name,(int)sizeof(g_launch.breadcrumb)-l);
    }
}

/* Drill into category `cat` (-1 = back up to the category list): reset the
 * view and reload the icon cache for the new level. */
static void launch_goto(int cat)
{
    g_launch.cur_cat=cat; g_launch.sel=0; g_launch.scroll=0; g_launch.hover=-1;
    g_launch.icons_tried=0; launch_load_icons();
    launch_build_breadcrumb();
}

/* Enter/click/[Open] on the selection: drill into the category at level 0,
 * launch the tool's open() at level 1. */
static void launch_activate(void)
{
    if(g_launch.sel<0 || g_launch.sel>=launch_cur_count()) return;
    if(g_launch.cur_cat<0){ launch_goto(g_launch.sel); return; }
    const struct forebo_tool *t=launch_cur_tools();
    if(t) t[g_launch.sel].open();
}

static void launch_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    int sc=gsc(), lineH=L_lineH(), stride=LA_stride();
    int x=cx+10*sc;
    int hdrw=(cx+cw)-x-8*sc; if(hdrw<0) hdrw=0;
    /* Breadcrumb: "ForeB Tools" at the category level, "ForeB Tools > <cat>"
     * inside a category; below it the hint line (with [Back] at level 1). */
    draw_string_clip(x,cy+6*sc,hdrw,g_launch.breadcrumb,c_dim,c_win,1,sc);
    draw_string_clip(x,cy+6*sc+lineH,hdrw,
        (g_launch.cur_cat<0)?"ForeB GUI Tools  (Up/Down + Enter, or click)"
                            :"[Back] Backspace/Esc    Up/Down + Enter, or click",
        c_accent,c_win,1,sc);
    int rowsY=LA_rowsY();
    int n=launch_cur_count();
    int vrows=launch_vrows(ch);
    launch_clamp(vrows);
    int scroll=g_launch.scroll;
    int isz=2*lineH;                        /* icon spans both text lines      */
    UINT32 hovc=wm_blend(c_win,c_fg,28);    /* subtle hover, distinct from sel */
    for(int r=0;r<vrows;r++){
        int i=scroll+r; if(i>=n) break;
        int ry=cy+rowsY+r*stride;
        int selr=(g_launch.sel==i);
        if(selr) fill_rect(cx+4*sc,ry-2*sc,cw-8*sc,stride,c_sel_bg);
        else if(g_launch.hover==i) fill_rect(cx+4*sc,ry-2*sc,cw-8*sc,stride,hovc);
        int ix=x;
        if(i<32 && g_launch.icon[i].pixels){ img_blit_alpha_scaled(&g_launch.icon[i], ix, ry, isz, isz); }
        else { fill_rect(ix,ry,isz,isz,c_border); }
        int tx=ix+isz+8*sc;
        UINT32 fg=selr?c_sel_fg:c_fg;
        /* Clip title/desc so long strings ellipsize instead of running off the
         * window. Reserve space for the scrollbar (when shown) + right margin. */
        int rmargin=8*sc + ((n>vrows)?(6*sc+2*sc):0);
        int maxw=(cx+cw) - tx - rmargin; if(maxw<0) maxw=0;
        draw_string_clip(tx,ry,maxw,launch_cur_name(i),fg,c_win,1,sc);
        draw_string_clip(tx,ry+lineH,maxw,launch_cur_desc(i),selr?c_sel_fg:c_dim,c_win,1,sc);
    }
    /* Right-edge scrollbar (render_list math) when the list overflows. */
    if(n>vrows){
        int barw=6*sc, trackX=cx+cw-barw-2*sc, trackY=cy+rowsY, trackH=vrows*stride;
        fill_rect(trackX,trackY,barw,trackH,c_border);
        int thumbH=vrows*trackH/n; if(thumbH<8*sc)thumbH=8*sc;
        int thumbY=trackY+scroll*(trackH-thumbH)/(n-vrows);
        fill_rect(trackX,thumbY,barw,thumbH,c_accent);
    }
    /* Button bar: [Open] ... [Close]. */
    wm_button *b; int nb=launch_btns_cached(cw,ch,&b);
    if(nb) bar_draw(cx,cy,cw,ch,b,nb,g_launch.b_hover,g_launch.b_press);
}

/* Row index under client-relative my (uses the SAME stride + scroll as draw). */
static int launch_row_at(int my, int ch)
{
    int rowsY=LA_rowsY();
    if(my<rowsY) return -1;
    int r=(my-rowsY)/LA_stride();
    if(r<0 || r>=launch_vrows(ch)) return -1;
    int idx=g_launch.scroll+r;
    return (idx<launch_cur_count())?idx:-1;
}

static int launch_event(wm_window *w, const wm_event *ev)
{
    int ch=wm_client_h(w), cw=wm_client_w(w);
    int n=launch_cur_count();
    switch(ev->type){
        case WM_EV_OPEN:
            g_launch.scroll=0; g_launch.hover=-1;
            g_launch.b_hover=0; g_launch.b_press=0;
            return 0;
        case WM_EV_KEY: {
            int v=launch_vrows(ch);
            /* Esc: up a level inside a category; close at the category list. */
            if(ev->scancode==SCAN_ESC){
                if(g_launch.cur_cat>=0){ launch_goto(-1); return 0; }
                return WM_CLOSE_REQUEST;
            }
            if(ev->unicode==CHAR_BACKSPACE && g_launch.cur_cat>=0){ launch_goto(-1); return 0; }
            if(ev->scancode==SCAN_UP)   g_launch.sel=(g_launch.sel>0)?g_launch.sel-1:n-1;
            else if(ev->scancode==SCAN_DOWN) g_launch.sel=(g_launch.sel<n-1)?g_launch.sel+1:0;
            else if(ev->scancode==SCAN_PAGE_UP){ g_launch.sel-=v; if(g_launch.sel<0)g_launch.sel=0; }
            else if(ev->scancode==SCAN_PAGE_DOWN){ g_launch.sel+=v; if(g_launch.sel>n-1)g_launch.sel=n-1; }
            else if(ev->scancode==SCAN_HOME) g_launch.sel=0;
            else if(ev->scancode==SCAN_END){ g_launch.sel=n-1; if(g_launch.sel<0)g_launch.sel=0; }
            else if(ev->unicode==CHAR_CR){ launch_activate(); return 0; }
            launch_clamp(v);
            return 0; }
        case WM_EV_MOUSE_WHEEL: {
            g_launch.sel-=ev->wheel;
            if(g_launch.sel<0)g_launch.sel=0; if(g_launch.sel>n-1)g_launch.sel=n-1;
            launch_clamp(launch_vrows(ch));
            return 0; }
        case WM_EV_MOUSE_MOVE: {
            wm_button *b; int nb=launch_btns_cached(cw,ch,&b);
            g_launch.b_hover=bar_hit(b,nb,ev->mx,ev->my);
            g_launch.hover=launch_row_at(ev->my,ch);
            return 0; }
        case WM_EV_MOUSE_DOWN: {
            wm_button *b; int nb=launch_btns_cached(cw,ch,&b);
            int id=bar_hit(b,nb,ev->mx,ev->my);
            if(id){ g_launch.b_press=id; return 0; }
            int v=launch_vrows(ch);
            /* Scrollbar track click pages (thumb rect computed as in draw). */
            if(n>v){
                int sc=gsc(), barw=6*sc, trackXc=cw-barw-2*sc;
                if(ev->mx>=trackXc){
                    int trackH=v*LA_stride();
                    int thumbH=v*trackH/n; if(thumbH<8*sc)thumbH=8*sc;
                    int thumbY=LA_rowsY()+g_launch.scroll*(trackH-thumbH)/(n-v);
                    if(ev->my<thumbY) g_launch.sel-=v;
                    else if(ev->my>thumbY+thumbH) g_launch.sel+=v;
                    if(g_launch.sel<0)g_launch.sel=0; if(g_launch.sel>n-1)g_launch.sel=n-1;
                    launch_clamp(v);
                    return 0;
                }
            }
            int r=launch_row_at(ev->my,ch);
            if(r>=0){ g_launch.sel=r; launch_clamp(v); launch_activate(); }
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!g_launch.b_press) return 0;
            wm_button *b; int nb=launch_btns_cached(cw,ch,&b);
            int id=bar_hit(b,nb,ev->mx,ev->my), p=g_launch.b_press;
            g_launch.b_press=0;
            if(id==p){
                if(p==LA_CLOSE) return WM_CLOSE_REQUEST;
                launch_activate();   /* LA_OPEN */
            }
            return 0; }
        case WM_EV_CLOSE:
            g_launch.win=NULL;
            if(g_launch.root){ g_launch.root->Close(g_launch.root); g_launch.root=NULL; }
            return 0;
        default: return 0;
    }
}

/* Open the 2-level Tools launcher: always starts at the category list
 * (forebo_categories[], level 0); Enter/click drills into a category's tools
 * (level 1), Backspace/Esc goes back up. Idempotent; the menu loop drives it. */
void tools_launcher_open(void)
{
    if(g_launch.win) return;
    g_launch.cur_cat=-1;                /* always open at the category level */
    g_launch.sel=0; g_launch.scroll=0; g_launch.hover=-1;
    g_launch.b_hover=0; g_launch.b_press=0;
    /* Open the ESP root once for the whole window lifetime (mirrors g_fb);
     * reused by launch_load_icons() across every category navigation. */
    { EFI_FILE_PROTOCOL *root=NULL; if(!EFI_ERROR(esp_open_root(gImage,gBS,&root))) g_launch.root=root; else g_launch.root=NULL; }
    launch_load_icons();
    launch_build_breadcrumb();
    int sc=gsc();
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*40/100; if(ww<420)ww=420; if(ww>620)ww=620; if(ww>W-40)ww=W-40;
    /* rows*stride + header + button-bar strip + window chrome; cap at H-40. */
    int rowsH=launch_cur_count()*LA_stride() + LA_rowsY() + wm_button_h()+10*sc
              + wm_chrome_h();
    int wh=rowsH; if(wh>H-40)wh=H-40; if(wh<260)wh=260;
    g_launch.win=wm_open("Tools", ww, wh, launch_draw, launch_event, &g_launch);
    if(!g_launch.win && g_launch.root){ g_launch.root->Close(g_launch.root); g_launch.root=NULL; }
}

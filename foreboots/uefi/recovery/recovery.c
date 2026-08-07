/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/recovery.c - Windowed Recovery / disk-tools panel.
 * =============================================================================
 * A mouse-driven Recovery window composited over the ui.c double buffer using
 * the tiny window manager (wm.c) + pointer layer (input.c). It offers a column
 * of tool buttons and a scrolling output log:
 *
 *     List Disks     - enumerate every EFI_BLOCK_IO device (size / flags)
 *     Next Disk      - cycle the "target" device the tools act on
 *     GPT View       - parse + print the target's GPT header + partitions
 *     FS Probe       - identify the target's filesystem by on-disk magic
 *     Chainload USB  - scan all volumes for GRUB / \EFI\BOOT and boot one
 *     Open Shell     - drop into the full text shell (rescue, fatfix, ext-*, ...)
 *     Close          - back to the boot menu
 *
 * DESTRUCTIVE operations (rescue / fatfix / raw writes) live only in the text
 * shell, which gates each behind a typed 'yes'. This window is read-only apart
 * from the shell hand-off and the (user-initiated) chainload.
 *
 * Everything runs BEFORE ExitBootServices. See recovery.h for the bootx64
 * integration contract (FOREB_ENTRY_RECOVERY / FOREB_ENTRY_SHELL menu types).
 * ========================================================================== */

#include "recovery.h"
#include "../ui.h"          /* draw primitives + ui_present + ui_width/height/scale */
#include "../core/wm.h"          /* wm_init/open/run_frame/draw + wm_event               */
#include "../core/input.h"       /* mouse_state + input_poll + input_draw_cursor         */
#include "../tools/shell.h"       /* shell_run + FOREB_SHELL_*                             */
#include "../boot/chainload.h"   /* chain_list + chain_boot_first                         */

/* =============================================================================
 * Module-wide state (single modal instance at a time, mirroring shell.c).
 * ==========================================================================*/
static EFI_SYSTEM_TABLE               *sST;
static EFI_BOOT_SERVICES              *sBS;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *sIn;
static EFI_HANDLE                      sImage;
static struct forebo_config           *sCfg;
static int                             sSel;

static EFI_GUID gBlk = EFI_BLOCK_IO_PROTOCOL_GUID;

/* =============================================================================
 * Tiny freestanding helpers.
 * ==========================================================================*/
static UINT32 rd_u32(const UINT8 *p)
{ return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT64 rd_u64(const UINT8 *p)
{ return (UINT64)rd_u32(p) | ((UINT64)rd_u32(p + 4) << 32); }
static int  mem_eq(const UINT8 *a, const UINT8 *b, int n)
{ for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static void u2a(const CHAR16 *u, char *a, int cap)
{ int i = 0; for (; u && u[i] && i + 1 < cap; i++) { CHAR16 c = u[i]; a[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?'; } a[i] = 0; }

static UINT32 pick(unsigned int v, UINT32 def)
{ return (v == FOREB_COLOR_UNSET) ? def : (UINT32)v; }

/* Known GPT partition-type GUIDs (on-disk byte order) -> friendly names. */
static const struct { UINT8 g[16]; const char *name; } gpt_types[] = {
    {{0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b}, "EFI System"},
    {{0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4}, "Linux filesystem"},
    {{0x6d,0xfd,0x57,0x06,0xab,0xa4,0xc4,0x43,0x84,0xe5,0x09,0x33,0xc8,0x4b,0x4f,0x4f}, "Linux swap"},
    {{0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}, "MS basic data"},
    {{0x16,0xe3,0xc9,0xe3,0x5c,0x0b,0xb8,0x4d,0x81,0x7d,0xf9,0x2d,0xf0,0x02,0x15,0xae}, "MS reserved"},
    {{0x79,0xd3,0xd6,0xe6,0x07,0xf5,0xc2,0x44,0xa2,0x3c,0x23,0x8f,0x2a,0x3d,0xf9,0x28}, "Linux LVM"},
    {{0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49}, "BIOS boot"},
};
static const char *gpt_name(const UINT8 *g)
{
    for (unsigned i = 0; i < sizeof(gpt_types) / sizeof(gpt_types[0]); i++)
        if (mem_eq(gpt_types[i].g, g, 16)) return gpt_types[i].name;
    return 0;
}

/* =============================================================================
 * Recovery window state (the wm_window's `user`).
 * ==========================================================================*/
#define RLOG_MAX   160
#define RLOG_COLS  100

typedef struct {
    char   log[RLOG_MAX][RLOG_COLS];
    UINT32 logcol[RLOG_MAX];
    int    loghead, logn;

    int    sel;          /* keyboard-highlighted button                  */
    int    hover;        /* mouse-hovered button (-1 none)               */
    int    cur_dev;      /* target BlockIo index for GPT/FS Probe        */
    int    devcount;

    int    pending;      /* queued action id (executed by the loop)      */
    int    want_close;
    int    result;       /* recovery_run() return code                   */

    /* resolved theme colors */
    UINT32 c_bg, c_fg, c_dim, c_accent, c_sel_bg, c_sel_fg, c_win, c_border, c_warn, c_cursor;
} rec_state;

static rec_state *g_rc;

/* -------- log line composer (operates on g_rc) --------------------------- */
static char   g_cl[RLOG_COLS];
static int    g_cll;
static UINT32 g_clcol;
static void L0(UINT32 c) { g_cll = 0; g_cl[0] = 0; g_clcol = c; }
static void Lc(char c)   { if (g_cll < RLOG_COLS - 1) { g_cl[g_cll++] = c; g_cl[g_cll] = 0; } }
static void Ls(const char *s) { while (s && *s) Lc(*s++); }
static void Lu(UINT64 v)
{ char t[24]; int i = 0; if (!v) { Lc('0'); return; } while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; } while (i) Lc(t[--i]); }
static void Lhex2(UINT8 b)
{ static const char h[] = "0123456789ABCDEF"; Lc(h[b >> 4]); Lc(h[b & 0xF]); }
static void Lend(void)
{
    rec_state *rc = g_rc;
    int pos = (rc->loghead + rc->logn) % RLOG_MAX;
    int i = 0; for (; g_cl[i] && i < RLOG_COLS - 1; i++) rc->log[pos][i] = g_cl[i];
    rc->log[pos][i] = 0; rc->logcol[pos] = g_clcol;
    if (rc->logn < RLOG_MAX) rc->logn++;
    else rc->loghead = (rc->loghead + 1) % RLOG_MAX;
}

/* =============================================================================
 * Block-device access.
 * ==========================================================================*/
static EFI_BLOCK_IO_PROTOCOL *blk_idx(int want, int *count)
{
    UINTN n = 0; EFI_HANDLE *h = NULL;
    if (EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gBlk, NULL, &n, &h)) || !h) {
        if (count) *count = 0; return NULL;
    }
    if (count) *count = (int)n;
    EFI_BLOCK_IO_PROTOCOL *r = NULL;
    if (want >= 0 && want < (int)n) {
        EFI_BLOCK_IO_PROTOCOL *b = NULL;
        if (!EFI_ERROR(sBS->HandleProtocol(h[want], &gBlk, (VOID **)&b))) r = b;
    }
    sBS->FreePool(h);
    return r;
}

/* =============================================================================
 * Tool actions (append lines to the window log).
 * ==========================================================================*/
static void act_list(void)
{
    rec_state *rc = g_rc;
    UINTN n = 0; EFI_HANDLE *h = NULL;
    L0(rc->c_accent); Ls("== block devices =="); Lend();
    if (EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gBlk, NULL, &n, &h)) || !h) {
        L0(rc->c_warn); Ls("no block devices"); Lend(); rc->devcount = 0; return;
    }
    rc->devcount = (int)n;
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *b = NULL;
        if (EFI_ERROR(sBS->HandleProtocol(h[i], &gBlk, (VOID **)&b)) || !b || !b->Media) continue;
        EFI_BLOCK_IO_MEDIA *m = b->Media;
        UINT64 mib = ((m->LastBlock + 1) * (UINT64)m->BlockSize) >> 20;
        L0(i == (UINTN)rc->cur_dev ? rc->c_sel_fg : rc->c_fg);
        Lc('['); Lu(i); Ls("] "); Ls(m->LogicalPartition ? "part " : "disk ");
        Lu(mib); Ls("MiB bs="); Lu(m->BlockSize);
        if (m->RemovableMedia) Ls(" rm");
        if (!m->MediaPresent)  Ls(" no-media");
        if (m->ReadOnly)       Ls(" ro");
        Lend();
    }
    sBS->FreePool(h);
    L0(rc->c_dim); Ls("target dev = "); Lu(rc->cur_dev); Ls("  (Next Disk to change)"); Lend();
}

static void act_next(void)
{
    rec_state *rc = g_rc;
    int cnt = 0; (void)blk_idx(-1, &cnt);
    rc->devcount = cnt;
    if (cnt <= 0) { L0(rc->c_warn); Ls("no devices"); Lend(); return; }
    rc->cur_dev = (rc->cur_dev + 1) % cnt;
    L0(rc->c_dim); Ls("target dev = "); Lu(rc->cur_dev); Lend();
}

static void act_gpt(void)
{
    rec_state *rc = g_rc; int cnt = 0;
    EFI_BLOCK_IO_PROTOCOL *b = blk_idx(rc->cur_dev, &cnt);
    L0(rc->c_accent); Ls("== GPT dev "); Lu(rc->cur_dev); Ls(" =="); Lend();
    if (!b || !b->Media) { L0(rc->c_warn); Ls("no such device"); Lend(); return; }
    EFI_BLOCK_IO_MEDIA *m = b->Media; UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    if ((UINT64)m->LastBlock < 2) { L0(rc->c_warn); Ls("device too small"); Lend(); return; }
    UINT8 *hdr = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bs, (VOID **)&hdr)) || !hdr) return;
    if (EFI_ERROR(b->ReadBlocks(b, m->MediaId, 1, bs, hdr))) { L0(rc->c_warn); Ls("read failed"); Lend(); sBS->FreePool(hdr); return; }
    static const UINT8 sig[8] = { 'E','F','I',' ','P','A','R','T' };
    if (!mem_eq(hdr, sig, 8)) { L0(rc->c_warn); Ls("no valid GPT (MBR or raw disk)"); Lend(); sBS->FreePool(hdr); return; }
    UINT64 entLba = rd_u64(hdr + 72); UINT32 num = rd_u32(hdr + 80), esz = rd_u32(hdr + 84);
    if (esz < 128 || esz > 1024 || num == 0 || num > 512) { L0(rc->c_warn); Ls("bad GPT geometry"); Lend(); sBS->FreePool(hdr); return; }
    UINTN arrB = (UINTN)num * esz, secs = (arrB + bs - 1) / bs, allocB = secs * bs;
    UINT8 *arr = NULL;
    if (!EFI_ERROR(sBS->AllocatePool(EfiLoaderData, allocB, (VOID **)&arr)) && arr) {
        if (!EFI_ERROR(b->ReadBlocks(b, m->MediaId, entLba, allocB, arr))) {
            int used = 0;
            for (UINT32 i = 0; i < num; i++) {
                const UINT8 *e = arr + (UINTN)i * esz;
                int z = 1; for (int j = 0; j < 16; j++) if (e[j]) { z = 0; break; }
                if (z) continue; used++;
                UINT64 s = rd_u64(e + 32), en = rd_u64(e + 40);
                L0(rc->c_fg); Ls("  ["); Lu(i); Ls("] ");
                const char *nm = gpt_name(e); Ls(nm ? nm : "(type GUID)");
                Ls("  "); Lu(((en - s + 1) * (UINT64)bs) >> 20); Ls("MiB");
                char pn[40]; u2a((const CHAR16 *)(e + 56), pn, sizeof pn);
                if (pn[0]) { Ls("  \""); Ls(pn); Ls("\""); }
                Lend();
            }
            L0(rc->c_dim); Lu(used); Ls(" partition(s)"); Lend();
        }
        sBS->FreePool(arr);
    }
    sBS->FreePool(hdr);
}

static void act_fsprobe(void)
{
    rec_state *rc = g_rc; int cnt = 0;
    EFI_BLOCK_IO_PROTOCOL *b = blk_idx(rc->cur_dev, &cnt);
    L0(rc->c_accent); Ls("== FS probe dev "); Lu(rc->cur_dev); Ls(" =="); Lend();
    if (!b || !b->Media) { L0(rc->c_warn); Ls("no such device"); Lend(); return; }
    EFI_BLOCK_IO_MEDIA *m = b->Media; UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    UINT64 devBytes = (m->LastBlock + 1) * (UINT64)bs;
    UINTN want = 66560; if ((UINT64)want > devBytes) want = (UINTN)devBytes;
    UINTN secs = (want + bs - 1) / bs, allocB = secs * bs;
    UINT8 *d = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, allocB, (VOID **)&d)) || !d) return;
    if (EFI_ERROR(b->ReadBlocks(b, m->MediaId, 0, allocB, d))) { L0(rc->c_warn); Ls("read failed"); Lend(); sBS->FreePool(d); return; }

    const char *fs = 0, *extra = 0;
    #define AT(off,str,len) (allocB >= (UINTN)((off)+(len)) && mem_eq(d+(off),(const UINT8*)(str),(len)))
    if      (allocB > 511 && d[510] == 0x55 && d[511] == 0xAA && AT(3, "NTFS    ", 8)) fs = "NTFS";
    else if (allocB > 511 && d[510] == 0x55 && d[511] == 0xAA && AT(3, "EXFAT   ", 8)) fs = "exFAT";
    else if (AT(82, "FAT32   ", 8))                                                    fs = "FAT32";
    else if (AT(54, "FAT12   ", 8) || AT(54, "FAT16   ", 8) || AT(54, "FAT     ", 8))  fs = "FAT12/16";
    else if (allocB > 1082 && d[1080] == 0x53 && d[1081] == 0xEF) {
        UINT32 compat = rd_u32(d + 1024 + 92), incompat = rd_u32(d + 1024 + 96);
        if      (incompat & 0x0240) { fs = "ext4"; extra = (incompat & 0x40) ? "extents" : "64bit"; }
        else if (compat & 0x0004)   { fs = "ext3"; extra = "has_journal"; }
        else                          fs = "ext2";
    }
    else if (AT(0x10040, "_BHRfS_M", 8))            fs = "btrfs";
    else if (AT(0, "XFSB", 4))                      fs = "XFS";
    else if (AT(0, "LUKS\xba\xbe", 6))              fs = "LUKS (encrypted)";
    else if (AT(32769, "CD001", 5))                 fs = "ISO9660";
    else if (AT(4086, "SWAPSPACE2", 10) || AT(4086, "SWAP-SPACE", 10)) fs = "Linux swap";
    else if (allocB > 511 && d[510] == 0x55 && d[511] == 0xAA)         fs = "FAT/MBR (bootable)";
    #undef AT

    L0(fs ? rc->c_fg : rc->c_warn);
    Ls("filesystem: "); Ls(fs ? fs : "unknown / raw");
    if (extra) { Ls("  ("); Ls(extra); Ls(")"); }
    Lend();
    sBS->FreePool(d);
}

/* Chainload: scan every volume for a secondary EFI loader and boot the first. */
static void act_chain(void)
{
    rec_state *rc = g_rc;
    L0(rc->c_accent); Ls("== chainload scan =="); Lend();
    static struct foreb_chain_list cl;
    int n = chain_list(sImage, sST, &cl);
    if (n <= 0) { L0(rc->c_warn); Ls("no bootable EFI loaders found on any volume"); Lend(); return; }
    for (int i = 0; i < n && i < FOREB_CHAIN_MAX_RESULTS; i++) {
        L0(rc->c_fg); Ls("  "); Ls(cl.items[i].label); Lend();
    }
    L0(rc->c_dim); Ls("booting first candidate..."); Lend();
    /* Present the log once before we hand off (chain_boot_first does not
     * return on success). */
    ui_present();
    EFI_STATUS st = chain_boot_first(sImage, sST);
    L0(rc->c_warn); Ls("chainload failed (status 0x"); Lhex2((UINT8)((UINT64)st >> 8)); Lhex2((UINT8)(UINT64)st); Ls(")"); Lend();
}

/* =============================================================================
 * Button table.
 * ==========================================================================*/
enum { A_NONE = 0, A_LIST, A_NEXT, A_GPT, A_FSPROBE, A_CHAIN, A_SHELL, A_CLOSE };
static const struct { const char *label; int action; } RBTN[] = {
    { "List Disks",    A_LIST    },
    { "Next Disk",     A_NEXT    },
    { "GPT View",      A_GPT     },
    { "FS Probe",      A_FSPROBE },
    { "Chainload USB", A_CHAIN   },
    { "Open Shell",    A_SHELL   },
    { "Close",         A_CLOSE   },
};
#define RBTN_N ((int)(sizeof(RBTN) / sizeof(RBTN[0])))

/* Client-relative button rect (matches both draw + hit-test). */
static void btn_rect(int cw, int i, int *x, int *y, int *w, int *h)
{
    int sc = ui_scale(); if (sc < 1) sc = 1;
    int pad = 8 * sc, bh = 24 * sc, gap = 6 * sc;
    int bw = cw * 32 / 100; if (bw < 96 * sc) bw = 96 * sc; if (bw > cw - 40) bw = cw - 40;
    *x = pad; *y = pad + i * (bh + gap); *w = bw; *h = bh;
}
static int btn_hit(int cw, int mx, int my)
{
    for (int i = 0; i < RBTN_N; i++) {
        int bx, by, bw, bh; btn_rect(cw, i, &bx, &by, &bw, &bh);
        if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) return i;
    }
    return -1;
}

/* =============================================================================
 * WM callbacks.
 * ==========================================================================*/
static void rec_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    rec_state *rc = g_rc; if (!rc) return;
    int sc = ui_scale(); if (sc < 1) sc = 1;
    int pad = 8 * sc, glyphW = 8 * sc, lineH = 16 * sc;
    (void)w;

    /* Buttons. */
    for (int i = 0; i < RBTN_N; i++) {
        int bx, by, bw, bh; btn_rect(cw, i, &bx, &by, &bw, &bh);
        int selb = (rc->sel == i), hot = (rc->hover == i);
        UINT32 fillc = selb ? rc->c_accent : (hot ? rc->c_sel_bg : rc->c_win);
        UINT32 txtc  = selb ? rc->c_sel_fg : rc->c_fg;
        fill_rect(cx + bx, cy + by, bw, bh, fillc);
        draw_rect_outline(cx + bx, cy + by, bw, bh, 1, rc->c_border);
        draw_string(cx + bx + 6 * sc, cy + by + (bh - lineH) / 2, RBTN[i].label, txtc, fillc, 1, sc);
    }

    /* Output log panel to the right of the button column. */
    int b0x, b0y, bw0, bh0; btn_rect(cw, 0, &b0x, &b0y, &bw0, &bh0);
    int lx = 2 * pad + bw0, ly = pad, lw = cw - lx - pad, lh = ch - 2 * pad;
    if (lw < glyphW * 8) lw = glyphW * 8;
    fill_rect(cx + lx, cy + ly, lw, lh, rc->c_win);
    draw_rect_outline(cx + lx, cy + ly, lw, lh, 1, rc->c_border);

    int rows = (lh - 4 * sc) / lineH; if (rows < 1) rows = 1;
    int cols = (lw - 8 * sc) / glyphW; if (cols < 1) cols = 1; if (cols > RLOG_COLS - 1) cols = RLOG_COLS - 1;
    int first = (rc->logn > rows) ? rc->logn - rows : 0;
    int shown = rc->logn - first;
    for (int r = 0; r < shown; r++) {
        int idx = (rc->loghead + first + r) % RLOG_MAX;
        char line[RLOG_COLS];
        int j = 0; for (; rc->log[idx][j] && j < cols; j++) line[j] = rc->log[idx][j];
        line[j] = 0;
        draw_string(cx + lx + 4 * sc, cy + ly + 2 * sc + r * lineH, line, rc->logcol[idx], rc->c_win, 1, sc);
    }
}

static int rec_event(wm_window *w, const wm_event *e)
{
    rec_state *rc = g_rc; if (!rc) return 0;
    int cw = wm_client_w(w);
    switch (e->type) {
        case WM_EV_KEY:
            if      (e->scancode == SCAN_UP)   rc->sel = (rc->sel > 0) ? rc->sel - 1 : RBTN_N - 1;
            else if (e->scancode == SCAN_DOWN) rc->sel = (rc->sel < RBTN_N - 1) ? rc->sel + 1 : 0;
            else if (e->scancode == SCAN_ESC)  return WM_CLOSE_REQUEST;
            else if (e->unicode == CHAR_CR)    rc->pending = RBTN[rc->sel].action;
            break;
        case WM_EV_MOUSE_MOVE:
            rc->hover = btn_hit(cw, e->mx, e->my);
            break;
        case WM_EV_MOUSE_DOWN:
            if (e->button == 0) {
                int i = btn_hit(cw, e->mx, e->my);
                if (i >= 0) { rc->sel = i; rc->pending = RBTN[i].action; }
            }
            break;
        case WM_EV_CLOSE:
            rc->want_close = 1;
            break;
        default: break;
    }
    return 0;
}

/* Execute a queued action outside the WM frame (some launch nested loops). */
static void run_action(rec_state *rc, int a)
{
    switch (a) {
        case A_LIST:    act_list();    break;
        case A_NEXT:    act_next();    break;
        case A_GPT:     act_gpt();     break;
        case A_FSPROBE: act_fsprobe(); break;
        case A_CHAIN:   act_chain();   break;
        case A_CLOSE:   rc->want_close = 1; break;
        case A_SHELL: {
            int r = shell_run(sImage, sST, sCfg, sSel);
            if (r == FOREB_SHELL_REBOOT) { rc->result = FOREB_RECOVERY_REBOOT; rc->want_close = 1; }
            else if (r >= 0)             { rc->result = r;                     rc->want_close = 1; }
            /* FOREB_SHELL_BACK: stay in the recovery window; the loop repaints. */
            /* Drain any keys the shell left buffered. */
            if (sIn) { EFI_INPUT_KEY k; while (!EFI_ERROR(sIn->ReadKeyStroke(sIn, &k))) { } }
            break;
        }
        default: break;
    }
}

/* =============================================================================
 * Public entry point.
 * ==========================================================================*/
int recovery_run(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                 struct forebo_config *cfg, int cur_sel)
{
    if (!image || !st || !st->BootServices) return FOREB_RECOVERY_BACK;
    sImage = image; sST = st; sBS = st->BootServices; sIn = st->ConIn;
    sCfg = cfg; sSel = cur_sel;

    static rec_state rc;              /* zero-initialised, single instance */
    sBS->SetMem(&rc, sizeof(rc), 0);
    rc.result = FOREB_RECOVERY_BACK;
    rc.hover = -1;
    g_rc = &rc;

    /* Resolve theme colors (built-in Forest palette when unset / no config). */
    const struct forebo_theme *th = cfg ? &cfg->theme : 0;
    rc.c_bg     = th ? pick(th->color_bg,       FOREB_BG)     : FOREB_BG;
    rc.c_fg     = th ? pick(th->color_fg,       FOREB_TEXT)   : FOREB_TEXT;
    rc.c_dim    = FOREB_DIM;
    rc.c_accent = th ? pick(th->color_accent,   FOREB_TITLE)  : FOREB_TITLE;
    rc.c_sel_bg = th ? pick(th->color_sel_bg,   FOREB_SELECT) : FOREB_SELECT;
    rc.c_sel_fg = th ? pick(th->color_sel_fg,   FOREB_WHITE)  : FOREB_WHITE;
    rc.c_win    = th ? pick(th->color_window,   FOREB_PANEL)  : FOREB_PANEL;
    rc.c_border = FOREB_BORDER;
    rc.c_warn   = FOREB_TIMER;
    rc.c_cursor = th ? pick(th->color_cursor,   FOREB_WHITE)  : FOREB_WHITE;

    /* Seed the log + device count. */
    L0(rc.c_accent); Ls("ForeB Recovery Tools"); Lend();
    L0(rc.c_dim); Ls("Click a tool, or Up/Down + Enter. Esc / Close returns to the menu."); Lend();
    act_list();

    /* Pointer + window manager. */
    mouse_state ms;
    input_init(sBS, &ms, (int)ui_width(), (int)ui_height());

    wm_init(th);
    wm_init_cache(sBS);
    int W = (int)ui_width(), H = (int)ui_height();
    int cw = W * 3 / 4; if (cw > 980) cw = 980; if (cw < 480) cw = 480; if (cw > W - 40) cw = W - 40;
    int ch = H * 3 / 4; if (ch > 640) ch = 640; if (ch < 320) ch = 320; if (ch > H - 40) ch = H - 40;
    wm_window *win = wm_open("Recovery Tools", cw, ch, rec_draw, rec_event, &rc);
    if (!win) { g_rc = 0; return FOREB_RECOVERY_BACK; }

    /* Drain buffered keys before the loop. */
    if (sIn) { EFI_INPUT_KEY k; while (!EFI_ERROR(sIn->ReadKeyStroke(sIn, &k))) { } }

    int dirty = 1;   /* force the first composite; then paint only on change */
    for (;;) {
        EFI_INPUT_KEY key;

        /* Snapshot pointer state so we can detect real motion/button changes. */
        int pmx = ms.x, pmy = ms.y, pml = ms.left, pmr = ms.right;
        input_poll(&ms);
        if (ms.x != pmx || ms.y != pmy || ms.left != pml || ms.right != pmr)
            dirty = 1;

        /* Drain the whole key buffer this frame, dispatching each to the WM. */
        while (sIn && !EFI_ERROR(sIn->ReadKeyStroke(sIn, &key))) {
            wm_run_frame(&ms, &key);
            dirty = 1;
        }
        /* One more frame for pointer motion / drags / focus. */
        wm_run_frame(&ms, NULL);
        if (wm_wants_repaint()) dirty = 1;

        if (rc.pending) { int a = rc.pending; rc.pending = 0; run_action(&rc, a); dirty = 1; }

        if (rc.want_close || wm_active_count() == 0) break;

        /* Recomposite only when something changed: background + window +
         * cursor, then a single flip. Avoids ~100 idle full-screen blits/sec
         * to uncached VRAM. */
        if (dirty) {
            ui_fill(rc.c_bg);
            wm_draw();
            if (ms.present) input_draw_cursor(&ms, rc.c_cursor);
            ui_present();
            dirty = 0;
        }

        sBS->Stall(10000);   /* ~10 ms frame clock */
    }

    wm_close_all();
    g_rc = 0;
    return rc.result;
}

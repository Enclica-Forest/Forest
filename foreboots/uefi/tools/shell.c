/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/shell.c - Interactive shell rendered on the GOP framebuffer.
 * =============================================================================
 * Freestanding (no libc, no gnu-efi). All output is drawn with the ui.c
 * primitives (draw_string / fill_rect) into a scrolling in-place text console;
 * the firmware text console (ConOut) is never used. Keyboard input comes from
 * ConIn->ReadKeyStroke on a ~10ms Stall poll cadence, exactly like the boot
 * menu. Everything runs BEFORE ExitBootServices, so Boot Services, the Simple
 * File System, Block I/O, and Runtime variable services are all live.
 *
 * Entry point: shell_run(image, cfg, cur_sel) - see shell.h / SHELL.md.
 *
 * The EFI system table is obtained from the LoadedImage protocol on `image`
 * (li->SystemTable), so the shell is self-contained and needs no globals from
 * bootx64.c. The ESP is reached through li->DeviceHandle's SimpleFileSystem.
 * ==========================================================================*/

#include "shell.h"
#include "../ui.h"          /* draw_string / fill_rect / ui_width/height/scale + theme */
#include "../efi_ext.h"     /* EFI_DISK_IO (optional byte reads) + device-path bits    */
#include "../recovery/fwsetup.h"     /* fw_boot_to_setup / fw_setup_supported ('setup' command) */
#include "tools.h"       /* tools_launcher_open ('tools' command)                   */

/* -----------------------------------------------------------------------------
 * Optional read-only filesystem back-ends (ext2/3/4, btrfs). These are provided
 * by sibling modules fs_ext.c / fs_btrfs.c. If their headers are not present in
 * this build the ext-ls/ext-cat/btrfs-snaps commands degrade to a clear "not
 * built" message, so the shell always compiles and links standalone.
 *
 * DOCUMENTED CONTRACT (fs_ext.h / fs_btrfs.h must match these signatures):
 *
 *   // fs_ext.h
 *   int fs_ext_probe(EFI_BLOCK_IO_PROTOCOL *bio);          // 1 if ext2/3/4
 *   int fs_ext_ls(EFI_BLOCK_IO_PROTOCOL *bio, const char *path,
 *                 void (*cb)(const char *name, UINT64 size, int is_dir,
 *                            void *ctx), void *ctx);        // <0 on error
 *   int fs_ext_cat(EFI_BLOCK_IO_PROTOCOL *bio, const char *path,
 *                  void (*cb)(const unsigned char *data, UINTN n, void *ctx),
 *                  void *ctx);                              // <0 on error
 *
 *   // fs_btrfs.h
 *   int fs_btrfs_probe(EFI_BLOCK_IO_PROTOCOL *bio);         // 1 if btrfs
 *   int fs_btrfs_list_snapshots(EFI_BLOCK_IO_PROTOCOL *bio,
 *                 void (*cb)(const char *name, UINT64 id, void *ctx),
 *                 void *ctx);                               // <0 on error
 * --------------------------------------------------------------------------- */
#if defined(__has_include)
# if __has_include("fs_ext.h")
#  include "fs_ext.h"
#  define FOREB_HAVE_FS_EXT 1
# endif
# if __has_include("fs_btrfs.h")
#  include "fs_btrfs.h"
#  define FOREB_HAVE_FS_BTRFS 1
# endif
#endif

/* =============================================================================
 * Local GUIDs (efi.h provides the macros; instantiate the ones we use).
 * ==========================================================================*/
static EFI_GUID gLoadedImgGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID gSfsGuid       = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gFileInfoGuid  = EFI_FILE_INFO_ID;
static EFI_GUID gBlockIoGuid   = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID gGlobalVarGuid = EFI_GLOBAL_VARIABLE;
/* Device inventory / input-test ('devices', 'inputtest'). */
static EFI_GUID gDevPathGuid   = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID gTextInGuid    = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
static EFI_GUID gPointerGuid   = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
static EFI_GUID gAbsPtrGuid    = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;

/* Messaging device-path SubTypes used to classify a block device's transport.
 * (efi_ext.h defines the node Type + MEDIA SubTypes; these are the bus ones.) */
#define MSG_ATAPI_DP   0x01
#define MSG_SCSI_DP    0x02
#define MSG_USB_DP     0x05
#define MSG_SATA_DP    0x12
#define MSG_NVME_DP    0x17
#define MSG_UFS_DP     0x19
#define MSG_SD_DP      0x1A
#define MSG_EMMC_DP    0x1D

/* EFI_FILE_SYSTEM_INFO (not in efi.h) - used by `drives` for the volume label. */
#define FOREB_FILE_SYSTEM_INFO_ID \
    { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
static EFI_GUID gFsInfoGuid = FOREB_FILE_SYSTEM_INFO_ID;
typedef struct {
    UINT64  Size;
    BOOLEAN ReadOnly;
    UINT64  VolumeSize;
    UINT64  FreeSpace;
    UINT32  BlockSize;
    CHAR16  VolumeLabel[1];   /* NUL-terminated tail */
} FOREB_EFI_FILE_SYSTEM_INFO;

/* forebo_cfg.c parser (used by the `config` reload command). */
void forebo_cfg_init(struct forebo_config *cfg);
int  forebo_cfg_parse(struct forebo_config *cfg, const char *text, unsigned long len);

/* =============================================================================
 * Shell-wide state (file-static; one shell instance at a time).
 * ==========================================================================*/
static EFI_SYSTEM_TABLE               *sST;
static EFI_BOOT_SERVICES              *sBS;
static EFI_RUNTIME_SERVICES           *sRT;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *sIn;
static EFI_HANDLE                      sImage;
static EFI_HANDLE                      sDev;     /* ESP device handle          */
static struct forebo_config           *sCfg;
static int                             sSel;     /* current/default entry idx  */

/* =============================================================================
 * Tiny freestanding string helpers.
 * ==========================================================================*/
static UINTN s_strlen(const char *s)      { UINTN n = 0; while (s && s[n]) n++; return n; }

static void s_strcpy(char *dst, const char *src, UINTN cap)
{
    UINTN i = 0;
    if (!cap) return;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int s_ci_eq(const char *a, const char *b)   /* case-insensitive equal */
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int s_eq(const char *a, const char *b)      /* exact equal */
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int s_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse decimal, or hex when prefixed 0x/0X. Returns 1 on success. */
static int s_parse_u64(const char *s, UINT64 *out)
{
    if (!s || !*s) return 0;
    UINT64 v = 0; int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; if (!*s) return 0; }
    for (; *s; s++) {
        int d;
        if (base == 16) { d = s_hexval(*s); if (d < 0) return 0; }
        else { if (*s < '0' || *s > '9') return 0; d = *s - '0'; }
        v = v * (UINT64)base + (UINT64)d;
    }
    *out = v;
    return 1;
}

/* ASCII -> CHAR16 (UCS-2). */
static void s_a2u(const char *a, CHAR16 *u, int cap)
{
    int i = 0;
    for (; a && a[i] && i + 1 < cap; i++) u[i] = (CHAR16)(UINT8)a[i];
    u[i] = 0;
}
/* CHAR16 -> ASCII ('?' for anything outside 0x20..0x7e). */
static void s_u2a(const CHAR16 *u, char *a, int cap)
{
    int i = 0;
    for (; u && u[i] && i + 1 < cap; i++) {
        CHAR16 c = u[i];
        a[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    a[i] = 0;
}

static int s_guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *pa = (const UINT8 *)a, *pb = (const UINT8 *)b;
    for (int i = 0; i < (int)sizeof(EFI_GUID); i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

/* Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into an EFI_GUID (big-endian per
 * field, standard string form). Hyphens are optional. Returns 1 on success. */
static int s_parse_guid(const char *s, EFI_GUID *g)
{
    UINT8 b[16];
    int nib = 0;
    for (int i = 0; i < 16; i++) {
        while (*s == '-') s++;
        int hi = s_hexval(*s++); if (hi < 0) return 0;
        while (*s == '-') s++;
        int lo = s_hexval(*s++); if (lo < 0) return 0;
        b[i] = (UINT8)((hi << 4) | lo);
        nib++;
    }
    if (nib != 16) return 0;
    g->Data1 = ((UINT32)b[0] << 24) | ((UINT32)b[1] << 16) | ((UINT32)b[2] << 8) | b[3];
    g->Data2 = (UINT16)(((UINT16)b[4] << 8) | b[5]);
    g->Data3 = (UINT16)(((UINT16)b[6] << 8) | b[7]);
    for (int i = 0; i < 8; i++) g->Data4[i] = b[8 + i];
    return 1;
}

/* =============================================================================
 * In-place scrolling text console (framebuffer, no ConOut).
 *
 * Scrollback is a ring of finalized lines. con_putc/con_puts build the current
 * line; a '\n' (con_flush) pushes it into the ring. con_render_all() clears the
 * screen and redraws the last visible lines plus the active line.
 * ==========================================================================*/
#define SB_MAX  256      /* scrollback line capacity (ring)                    */
#define COLW    256      /* max chars per stored line (incl NUL)               */

static char   g_sb[SB_MAX][COLW];
static UINT32 g_sb_col[SB_MAX];
static int    g_sb_head, g_sb_count;

static char   g_cur[COLW];
static int    g_curlen;
static UINT32 g_curcol;

static int    g_cols, g_rows;     /* console geometry in cells                */
static int    g_glyphW, g_lineH;  /* effective cell size in pixels            */
static int    g_margin;
static int    g_active_row;       /* screen row of the active (input) line    */
static int    g_caret_painted = -1; /* last painted caret column, -1 = none/stale */

static void con_init(void)
{
    int sc = ui_scale(); if (sc < 1) sc = 1;
    g_margin = 8;
    g_glyphW = FOREB_GLYPH_W * sc;
    g_lineH  = FOREB_GLYPH_H * sc;
    int w = (int)ui_width(), h = (int)ui_height();
    g_cols = (w - 2 * g_margin) / (g_glyphW ? g_glyphW : 8);
    if (g_cols > COLW - 1) g_cols = COLW - 1;
    if (g_cols < 8) g_cols = 8;
    g_rows = (h - 2 * g_margin) / (g_lineH ? g_lineH : 16);
    if (g_rows < 3) g_rows = 3;
    g_sb_head = g_sb_count = 0;
    g_curlen = 0; g_cur[0] = 0; g_curcol = FOREB_TEXT;
    g_active_row = 0;
    g_caret_painted = -1;
}

static void con_setcol(UINT32 c) { g_curcol = c; }

static void con_flush(void)       /* push active line into scrollback */
{
    int idx = (g_sb_head + g_sb_count) & (SB_MAX - 1);
    s_strcpy(g_sb[idx], g_cur, COLW);
    g_sb_col[idx] = g_curcol;
    if (g_sb_count < SB_MAX) g_sb_count++;
    else g_sb_head = (g_sb_head + 1) & (SB_MAX - 1);
    g_curlen = 0; g_cur[0] = 0; g_curcol = FOREB_TEXT;
}

static void con_putc(char c)
{
    if (c == '\n') { con_flush(); return; }
    if (c == '\r') return;
    if (c == '\t') { for (int i = 0; i < 4; i++) con_putc(' '); return; }
    if (c < 0x20 || c > 0x7e) c = '.';
    if (g_curlen >= g_cols || g_curlen >= COLW - 1) con_flush();  /* wrap */
    g_cur[g_curlen++] = c; g_cur[g_curlen] = 0;
}

static void con_puts(const char *s) { while (s && *s) con_putc(*s++); }

static void con_putu(UINT64 v)
{
    char t[24]; int i = 0;
    if (!v) { con_putc('0'); return; }
    while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i) con_putc(t[--i]);
}

static void con_puthex(UINT64 v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    con_puts("0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) con_putc(hx[(v >> i) & 0xF]);
}

static void con_puti(int v)
{
    if (v < 0) { con_putc('-'); con_putu((UINT64)(-v)); }
    else con_putu((UINT64)v);
}

static void con_putguid(const EFI_GUID *g)
{
    con_puthex(g->Data1, 8); con_putc('-');
    con_puthex(g->Data2, 4); con_putc('-');
    con_puthex(g->Data3, 4); con_putc('-');
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        con_putc(hx[g->Data4[i] >> 4]); con_putc(hx[g->Data4[i] & 0xF]);
        if (i == 1) con_putc('-');
    }
}

/* Draw only the active (input) line in place - used during line editing.
 * `dirty` is nonzero when g_cur's text changed since the previous call; when
 * it is zero (a pure caret move) only the caret bar needs to move: the old
 * caret cell is restored from g_cur (unchanged) and the new cell is painted,
 * skipping the fill_rect/draw_string of the whole line. Final pixel state is
 * identical to always doing the full repaint. */
static void con_redraw_active(int caret_col, int dirty)
{
    int y = g_margin + g_active_row * g_lineH;
    if (dirty || g_caret_painted < 0) {
        /* Redraw only the changed tail: everything left of the earlier of the old
         * and new caret columns is unchanged on screen (and the old caret bar, at
         * g_caret_painted, falls inside this span so it is cleared). A stale caret
         * (g_caret_painted < 0) forces a full-line repaint from column 0. */
        int col = 0;
        if (g_caret_painted >= 0) {
            col = g_caret_painted;
            if (caret_col >= 0 && caret_col < col) col = caret_col;
        }
        int cx0 = g_margin + col * g_glyphW;
        fill_rect(cx0, y, (g_cols - col + 1) * g_glyphW, g_lineH, FOREB_BG);
        draw_string(cx0, y, g_cur + col, FOREB_TEXT, FOREB_BG, 0, 1);
    } else if (caret_col != g_caret_painted) {
        char oc = (g_caret_painted < g_curlen) ? g_cur[g_caret_painted] : ' ';
        int  ox = g_margin + g_caret_painted * g_glyphW;
        draw_char(ox, y, oc, FOREB_TEXT, FOREB_BG, 0, 1);
    }
    if (caret_col >= 0) {
        int cx = g_margin + caret_col * g_glyphW;
        fill_rect(cx, y + g_lineH - 3, g_glyphW, 2, FOREB_WHITE);
    }
    g_caret_painted = caret_col;
    ui_present();   /* flip the edited line to VRAM (no-op if not double-buffered) */
}

static void con_render_all(void)
{
    fill_rect(0, 0, (int)ui_width(), (int)ui_height(), FOREB_BG);
    int total = g_sb_count + 1;                 /* +1 for the active line */
    int start = (total > g_rows) ? total - g_rows : 0;
    int row = 0;
    for (int i = start; i < g_sb_count; i++) {
        int idx = (g_sb_head + i) & (SB_MAX - 1);
        draw_string(g_margin, g_margin + row * g_lineH, g_sb[idx],
                    g_sb_col[idx], FOREB_BG, 0, 1);
        row++;
    }
    g_active_row = row;
    draw_string(g_margin, g_margin + g_active_row * g_lineH, g_cur,
                g_curcol, FOREB_BG, 0, 1);
    ui_present();   /* flip the whole console to VRAM */
}

/* =============================================================================
 * Line editor. Returns typed length (>=0), or -1 if Esc was pressed.
 * The finalized "prompt+input" line is pushed to the scrollback on Enter.
 * ==========================================================================*/
static int read_line(const char *prompt, char *out, int outcap)
{
    char in[COLW];
    int  len = 0, cur = 0, pl = (int)s_strlen(prompt);
    in[0] = 0;

    /* Compose g_cur = prompt (input empty) and paint the whole console. */
    s_strcpy(g_cur, prompt, COLW);
    g_curlen = pl; g_curcol = FOREB_TEXT;
    con_render_all();
    con_redraw_active(pl + cur, 1);

    for (;;) {
        EFI_INPUT_KEY key;
        UINTN ei;
        if (!sIn) continue;                          /* no input protocol available */
        sBS->WaitForEvent(1, &sIn->WaitForKey, &ei); /* block until a key is ready   */

        int dirty = 0;              /* any text change in this burst forces a repaint */
        int did   = 0;              /* did we accept at least one editing key?         */
        int lo    = 0x7fffffff;     /* leftmost g_cur column whose glyph changed        */

        /* Drain every buffered keystroke, then repaint once - coalesces bursts
         * (key-repeat / fast typing) into a single VRAM update. */
        while (!EFI_ERROR(sIn->ReadKeyStroke(sIn, &key))) {
            UINT16 sc = key.ScanCode;
            CHAR16 ch = key.UnicodeChar;

            if (ch == CHAR_CR || ch == 0x000A) {     /* Enter -> commit */
                /* g_cur[0..pl) already holds prompt from the one-time copy above */
                s_strcpy(g_cur + pl, in, COLW - pl);
                g_curlen = pl + len; g_curcol = FOREB_TEXT;
                con_flush();
                s_strcpy(out, in, outcap);
                return len;
            }
            if (sc == SCAN_ESC) return -1;

            int cur0 = cur;         /* caret before this key (bounds the changed span) */
            int edit = 1;           /* 1 = text changed, 0 = caret-only move            */

            if (ch == 0x0008) {                      /* Backspace */
                if (cur > 0) {
                    for (int i = cur - 1; i < len - 1; i++) in[i] = in[i + 1];
                    len--; cur--; in[len] = 0;
                }
            } else if (sc == SCAN_DELETE) {
                if (cur < len) {
                    for (int i = cur; i < len - 1; i++) in[i] = in[i + 1];
                    len--; in[len] = 0;
                }
            } else if (sc == SCAN_LEFT)  { if (cur > 0)   cur--; edit = 0; }
            else if (sc == SCAN_RIGHT)   { if (cur < len) cur++; edit = 0; }
            else if (sc == SCAN_HOME)    { cur = 0; edit = 0; }
            else if (sc == SCAN_END)     { cur = len; edit = 0; }
            else if (ch >= 0x20 && ch < 0x7f) {      /* insert printable */
                if (len < outcap - 1 && len < COLW - 2 && (pl + len) < g_cols - 1) {
                    for (int i = len; i > cur; i--) in[i] = in[i - 1];
                    in[cur] = (char)ch; len++; cur++; in[len] = 0;
                }
            } else {
                continue;                            /* ignore other keys */
            }

            did = 1;
            if (edit) {             /* record the earliest changed column in the burst */
                int col = pl + (cur < cur0 ? cur : cur0);
                if (col < lo) lo = col;
                dirty = 1;
            }
        }

        if (!did) continue;         /* only caret-less noise arrived: nothing to paint */

        /* g_cur[0..pl) already holds prompt from the one-time copy above */
        s_strcpy(g_cur + pl, in, COLW - pl);
        g_curlen = pl + len; g_curcol = FOREB_TEXT;
        /* If the burst edited text left of the last-painted caret, widen the tail
         * repaint left to that column so con_redraw_active covers every change. */
        if (dirty && g_caret_painted >= 0 && lo < g_caret_painted)
            g_caret_painted = lo;
        con_redraw_active(pl + cur, dirty);
    }
}

/* =============================================================================
 * ESP file access.
 * ==========================================================================*/
static EFI_FILE_PROTOCOL *esp_root(void)
{
    static EFI_FILE_PROTOCOL *root;   /* cached across calls */
    if (root) return root;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    if (EFI_ERROR(sBS->HandleProtocol(sDev, &gSfsGuid, (VOID **)&fs)) || !fs) return NULL;
    if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) { root = NULL; return NULL; }
    return root;
}

/* Current working directory on the ESP, backslash-normalized, no trailing
 * separator except at the root ("\"). Updated by `cd`, used to resolve every
 * relative shell path (ls/cat/hexdump/background/modules/...). */
static char s_cwd[FOREB_CFG_PATH_LEN] = "\\";

/* Resolve an input path against s_cwd into an absolute, backslash-normalized
 * ESP path (e.g. "\forebo\icons"). Handles ".", "..", absolute (leading '/'
 * or '\') and relative inputs. Root collapses to "\". Freestanding. */
static void path_resolve(const char *in, char *out, int cap)
{
    char stk[FOREB_CFG_PATH_LEN * 2];
    int len = 0;

    if (in && (in[0] == '/' || in[0] == '\\')) {
        stk[len++] = '\\';                       /* absolute: start at root   */
    } else {
        for (const char *p = s_cwd; *p && len < (int)sizeof(stk); p++)
            stk[len++] = *p;                      /* relative: seed with cwd   */
    }

    const char *p = in ? in : "";
    while (*p) {
        while (*p == '/' || *p == '\\') p++;      /* skip separators           */
        if (!*p) break;
        char comp[FOREB_CFG_PATH_LEN]; int ci = 0;
        while (*p && *p != '/' && *p != '\\' && ci < (int)sizeof(comp) - 1)
            comp[ci++] = *p++;
        comp[ci] = 0;
        if (comp[0] == '.' && comp[1] == 0) continue;              /* "."      */
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {    /* ".."     */
            while (len > 0 && stk[len - 1] != '\\') len--;         /* drop name */
            if (len > 1) len--;                                    /* drop sep  */
            continue;
        }
        if (len == 0 || stk[len - 1] != '\\') {
            if (len < (int)sizeof(stk)) stk[len++] = '\\';
        }
        for (int k = 0; comp[k] && len < (int)sizeof(stk); k++) stk[len++] = comp[k];
    }

    while (len > 1 && stk[len - 1] == '\\') len--;                 /* trailing */
    if (len == 0) stk[len++] = '\\';                              /* root      */
    int o = 0;
    for (int k = 0; k < len && o < cap - 1; k++) out[o++] = stk[k];
    out[o] = 0;
    if (o == 0 && cap >= 2) { out[0] = '\\'; out[1] = 0; }
}

/* Convert an ASCII ESP path to a CHAR16 absolute path, resolved against the
 * current working directory (see path_resolve). */
static void path_to_efi(const char *ascii, CHAR16 *out, int cap)
{
    char abs[FOREB_CFG_PATH_LEN * 2];
    path_resolve(ascii, abs, (int)sizeof(abs));
    int i = 0;
    for (const char *p = abs; *p && i + 1 < cap; p++)
        out[i++] = (CHAR16)(UINT8)*p;
    out[i] = 0;
}

static EFI_STATUS esp_open(const char *ascii_path, UINT64 mode, EFI_FILE_PROTOCOL **out)
{
    EFI_FILE_PROTOCOL *root = esp_root();
    if (!root) return EFI_NOT_FOUND;
    CHAR16 wp[FOREB_CFG_PATH_LEN + 8];
    path_to_efi(ascii_path, wp, (int)(sizeof(wp) / sizeof(wp[0])));
    return root->Open(root, out, wp, mode, 0);
}

/* Read a whole ESP file into a pool buffer (clamped to `cap`). Caller frees. */
static EFI_STATUS esp_load(const char *path, UINTN cap, VOID **out_buf, UINTN *out_size)
{
    *out_buf = NULL; *out_size = 0;
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = esp_open(path, EFI_FILE_MODE_READ, &f);
    if (EFI_ERROR(st) || !f) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    UINT8 info[512]; UINTN isz = sizeof(info);
    st = f->GetInfo(f, &gFileInfoGuid, &isz, info);
    if (EFI_ERROR(st)) { f->Close(f); return st; }
    UINTN fsize = (UINTN)((EFI_FILE_INFO *)info)->FileSize;
    UINTN want = (fsize > cap) ? cap : fsize;

    VOID *buf = NULL;
    st = sBS->AllocatePool(EfiLoaderData, want ? want : 1, &buf);
    if (EFI_ERROR(st) || !buf) { f->Close(f); return EFI_OUT_OF_RESOURCES; }

    UINTN done = 0;
    while (done < want) {
        UINTN chunk = want - done;
        st = f->Read(f, &chunk, (UINT8 *)buf + done);
        if (EFI_ERROR(st)) { sBS->FreePool(buf); f->Close(f); return st; }
        if (chunk == 0) break;
        done += chunk;
    }
    f->Close(f);
    *out_buf = buf; *out_size = done;
    return EFI_SUCCESS;
}

/* =============================================================================
 * Block I/O enumeration helper.
 * ==========================================================================*/
static EFI_BLOCK_IO_PROTOCOL *blockio_by_index(int want, int *out_count)
{
    EFI_HANDLE *h = NULL; UINTN n = 0;
    EFI_STATUS st = sBS->LocateHandleBuffer(ByProtocol, &gBlockIoGuid, NULL, &n, &h);
    if (EFI_ERROR(st) || !h) { if (out_count) *out_count = 0; return NULL; }
    if (out_count) *out_count = (int)n;
    EFI_BLOCK_IO_PROTOCOL *res = NULL;
    if (want >= 0 && want < (int)n) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        if (!EFI_ERROR(sBS->HandleProtocol(h[want], &gBlockIoGuid, (VOID **)&bio)))
            res = bio;
    }
    sBS->FreePool(h);
    return res;
}

/* =============================================================================
 * hexdump helper.
 * ==========================================================================*/
static void hexdump_bytes(const UINT8 *d, UINTN n, UINT64 base)
{
    static const char hx[] = "0123456789ABCDEF";
    for (UINTN off = 0; off < n; off += 16) {
        con_puthex(base + off, 8); con_putc(' '); con_putc(' ');
        for (int i = 0; i < 16; i++) {
            if (off + (UINTN)i < n) {
                UINT8 b = d[off + i];
                con_putc(hx[b >> 4]); con_putc(hx[b & 0xF]);
            } else { con_putc(' '); con_putc(' '); }
            con_putc(' ');
            if (i == 7) con_putc(' ');
        }
        con_putc('|');
        for (int i = 0; i < 16 && off + (UINTN)i < n; i++) {
            UINT8 b = d[off + i];
            con_putc((b >= 0x20 && b < 0x7f) ? (char)b : '.');
        }
        con_putc('|');
        con_putc('\n');
    }
}

/* =============================================================================
 * Recovery / disk-fix helpers.
 * ==========================================================================*/

/* Little-endian scalar reads from a byte buffer (packed on-disk structures). */
static UINT16 rd_u16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 rd_u32(const UINT8 *p)
{ return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT64 rd_u64(const UINT8 *p)
{ return (UINT64)rd_u32(p) | ((UINT64)rd_u32(p + 4) << 32); }

/* Fixed-length byte compare (used by fsprobe's magic checks). */
static int s_mem_eq(const UINT8 *a, const UINT8 *b, int n)
{ for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

/* Whole-sector read wrapper. bytes MUST be a multiple of the media block size. */
static EFI_STATUS blk_read(EFI_BLOCK_IO_PROTOCOL *bio, UINT64 lba, UINTN bytes, VOID *buf)
{
    if (!bio || !bio->Media) return EFI_INVALID_PARAMETER;
    return bio->ReadBlocks(bio, bio->Media->MediaId, lba, bytes, buf);
}

/* Poll ConIn once; return 1 if Esc was pressed (used to abort long loops). */
static int esc_pressed(void)
{
    if (!sIn) return 0;
    EFI_INPUT_KEY k;
    if (!EFI_ERROR(sIn->ReadKeyStroke(sIn, &k)) && k.ScanCode == SCAN_ESC) return 1;
    return 0;
}

/* Ask the user to type 'yes' to proceed with a destructive action. */
static int confirm_yes(void)
{
    con_setcol(FOREB_TIMER);
    con_puts("This may DESTROY data and cannot be undone. Type 'yes' to proceed:\n");
    con_setcol(FOREB_TEXT);
    char resp[16];
    int r = read_line("confirm> ", resp, sizeof(resp));
    if (r < 0 || !s_eq(resp, "yes")) {
        con_setcol(FOREB_TITLE); con_puts("aborted (no changes made)\n"); con_setcol(FOREB_TEXT);
        return 0;
    }
    return 1;
}

/* -------- GPT ------------------------------------------------------------- */
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
static const char *gpt_type_name(const UINT8 *g)
{
    for (unsigned i = 0; i < sizeof(gpt_types) / sizeof(gpt_types[0]); i++) {
        int eq = 1;
        for (int j = 0; j < 16; j++) if (gpt_types[i].g[j] != g[j]) { eq = 0; break; }
        if (eq) return gpt_types[i].name;
    }
    return 0;
}
static int guid_is_zero(const UINT8 *g)
{ for (int i = 0; i < 16; i++) if (g[i]) return 0; return 1; }

/* Parse + print the GPT of one whole-disk BlockIo device. Returns #partitions,
 * or -1 if no valid GPT. When quiet != 0 only counts (used by `parts`). */
static int gpt_dump(EFI_BLOCK_IO_PROTOCOL *bio, int quiet)
{
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    if ((UINT64)m->LastBlock < 2) return -1;

    UINT8 *hdr = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bs, (VOID **)&hdr)) || !hdr) return -1;
    if (EFI_ERROR(blk_read(bio, 1, bs, hdr))) { sBS->FreePool(hdr); return -1; }
    static const UINT8 sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (hdr[i] != sig[i]) { sBS->FreePool(hdr); return -1; }

    UINT64 entLba  = rd_u64(hdr + 72);
    UINT32 numEnt  = rd_u32(hdr + 80);
    UINT32 entSz   = rd_u32(hdr + 84);
    UINT64 firstU  = rd_u64(hdr + 40);
    UINT64 lastU   = rd_u64(hdr + 48);
    if (entSz < 128 || entSz > 1024 || numEnt == 0 || numEnt > 512) {
        sBS->FreePool(hdr); return -1;
    }

    if (!quiet) {
        con_setcol(FOREB_TITLE); con_puts("GPT header valid"); con_setcol(FOREB_TEXT);
        con_puts("  disk GUID {"); { EFI_GUID g; sBS->CopyMem(&g, hdr + 56, 16); con_putguid(&g); }
        con_puts("}\n");
        con_puts("  usable LBA "); con_putu(firstU); con_puts(" .. "); con_putu(lastU);
        con_puts("   entries "); con_putu(numEnt); con_puts(" x "); con_putu(entSz); con_puts("B\n");
    }

    UINTN arrBytes = (UINTN)numEnt * entSz;
    UINTN arrSecs  = (arrBytes + bs - 1) / bs;
    UINTN allocB   = arrSecs * bs;
    UINT8 *arr = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, allocB, (VOID **)&arr)) || !arr) {
        sBS->FreePool(hdr); return -1;
    }
    int used = 0;
    if (!EFI_ERROR(blk_read(bio, entLba, allocB, arr))) {
        for (UINT32 i = 0; i < numEnt; i++) {
            const UINT8 *e = arr + (UINTN)i * entSz;
            if (guid_is_zero(e)) continue;             /* unused slot */
            used++;
            if (quiet) continue;
            UINT64 st = rd_u64(e + 32), en = rd_u64(e + 40);
            con_setcol(FOREB_TITLE); con_puts("  ["); con_putu(i); con_puts("] ");
            const char *nm = gpt_type_name(e);
            if (nm) con_puts(nm);
            else    { con_puts("{"); EFI_GUID g; sBS->CopyMem(&g, (VOID *)e, 16); con_putguid(&g); con_puts("}"); }
            con_setcol(FOREB_TEXT);
            con_puts("  LBA "); con_putu(st); con_puts("-"); con_putu(en);
            UINT64 mib = ((en - st + 1) * (UINT64)bs) >> 20;
            con_puts("  ("); con_putu(mib); con_puts(" MiB)");
            /* partition name: 36 CHAR16 at offset 56 */
            char pn[40]; s_u2a((const CHAR16 *)(e + 56), pn, sizeof(pn));
            if (pn[0]) { con_puts("  \""); con_puts(pn); con_puts("\""); }
            con_putc('\n');
        }
    }
    sBS->FreePool(arr);
    sBS->FreePool(hdr);
    return used;
}

static void cmd_gpt(int argc, char **argv)
{
    UINT64 dev;
    if (argc < 2 || !s_parse_u64(argv[1], &dev)) {
        con_setcol(FOREB_TIMER); con_puts("usage: gpt <dev>   (dev index from lsblk)\n"); con_setcol(FOREB_TEXT); return;
    }
    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio || !bio->Media) {
        con_setcol(FOREB_TIMER); con_puts("gpt: no such device (0.."); con_puti(total - 1); con_puts(")\n");
        con_setcol(FOREB_TEXT); return;
    }
    if (bio->Media->LogicalPartition) {
        con_setcol(FOREB_DIM); con_puts("(note: this is a partition device; GPT lives on its parent disk)\n"); con_setcol(FOREB_TEXT);
    }
    int n = gpt_dump(bio, 0);
    if (n < 0) { con_setcol(FOREB_TIMER); con_puts("gpt: no valid GPT on this device\n"); con_setcol(FOREB_TEXT); return; }
    con_setcol(FOREB_DIM); con_puti(n); con_puts(" partition(s)\n"); con_setcol(FOREB_TEXT);
}

/* -------- parts (all BlockIo, with a partition summary) ------------------- */
static void cmd_parts(void)
{
    EFI_HANDLE *h = NULL; UINTN n = 0;
    if (EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gBlockIoGuid, NULL, &n, &h)) || !h) {
        con_setcol(FOREB_TIMER); con_puts("parts: no block devices\n"); con_setcol(FOREB_TEXT); return;
    }
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        if (EFI_ERROR(sBS->HandleProtocol(h[i], &gBlockIoGuid, (VOID **)&bio)) || !bio || !bio->Media) continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        UINT64 mib = ((m->LastBlock + 1) * (UINT64)m->BlockSize) >> 20;
        con_setcol(FOREB_TITLE); con_putc('['); con_putu(i); con_puts("]"); con_setcol(FOREB_TEXT);
        con_puts(m->LogicalPartition ? " partition " : " disk ");
        con_putu(mib); con_puts("MiB bs="); con_putu(m->BlockSize);
        if (m->RemovableMedia) con_puts(" removable");
        if (!m->MediaPresent)  con_puts(" no-media");
        con_putc('\n');
        if (!m->LogicalPartition && m->MediaPresent) {
            int g = gpt_dump(bio, 0);
            if (g < 0) {
                /* No GPT: peek for an MBR signature. */
                UINT32 bs = m->BlockSize ? m->BlockSize : 512;
                UINT8 *sec = NULL;
                if (!EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bs, (VOID **)&sec)) && sec) {
                    if (!EFI_ERROR(blk_read(bio, 0, bs, sec)) && sec[510] == 0x55 && sec[511] == 0xAA) {
                        con_setcol(FOREB_DIM); con_puts("   MBR partition table:\n"); con_setcol(FOREB_TEXT);
                        for (int p = 0; p < 4; p++) {
                            const UINT8 *pe = sec + 446 + p * 16;
                            if (pe[4] == 0) continue;    /* type 0 = empty */
                            UINT32 lba = rd_u32(pe + 8), cnt = rd_u32(pe + 12);
                            con_puts("     part "); con_puti(p); con_puts(" type=0x");
                            con_putc("0123456789ABCDEF"[pe[4] >> 4]); con_putc("0123456789ABCDEF"[pe[4] & 0xF]);
                            con_puts(" start="); con_putu(lba); con_puts(" sectors="); con_putu(cnt); con_putc('\n');
                        }
                    } else {
                        con_setcol(FOREB_DIM); con_puts("   (no GPT / MBR partition table)\n"); con_setcol(FOREB_TEXT);
                    }
                    sBS->FreePool(sec);
                }
            }
        }
    }
    sBS->FreePool(h);
}

/* -------- fsprobe (identify a filesystem by magic) ------------------------ */
static void cmd_fsprobe(int argc, char **argv)
{
    UINT64 dev;
    if (argc < 2 || !s_parse_u64(argv[1], &dev)) {
        con_setcol(FOREB_TIMER); con_puts("usage: fsprobe <dev>\n"); con_setcol(FOREB_TEXT); return;
    }
    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio || !bio->Media) {
        con_setcol(FOREB_TIMER); con_puts("fsprobe: no such device\n"); con_setcol(FOREB_TEXT); return;
    }
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    UINT64 devBytes = (m->LastBlock + 1) * (UINT64)bs;
    UINTN want = 66560;                                   /* covers btrfs @ 0x10040 */
    if ((UINT64)want > devBytes) want = (UINTN)devBytes;
    UINTN secs = (want + bs - 1) / bs; UINTN allocB = secs * bs;
    UINT8 *d = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, allocB, (VOID **)&d)) || !d) {
        con_setcol(FOREB_TIMER); con_puts("fsprobe: out of memory\n"); con_setcol(FOREB_TEXT); return;
    }
    if (EFI_ERROR(blk_read(bio, 0, allocB, d))) {
        con_setcol(FOREB_TIMER); con_puts("fsprobe: read failed\n"); con_setcol(FOREB_TEXT); sBS->FreePool(d); return;
    }

    const char *fs = 0; const char *extra = 0;
    #define AT(off,str,len) (allocB >= (UINTN)((off)+(len)) && s_mem_eq(d+(off),(const UINT8*)(str),(len)))
    if      (allocB > 511 && d[510] == 0x55 && d[511] == 0xAA && AT(3, "NTFS    ", 8)) fs = "NTFS";
    else if (allocB > 511 && d[510] == 0x55 && d[511] == 0xAA && AT(3, "EXFAT   ", 8)) fs = "exFAT";
    else if (allocB > 90 && AT(82, "FAT32   ", 8))                                     fs = "FAT32";
    else if (allocB > 62 && (AT(54, "FAT12   ", 8) || AT(54, "FAT16   ", 8) || AT(54, "FAT     ", 8))) fs = "FAT12/16";
    else if (allocB > 1082 && d[1080] == 0x53 && d[1081] == 0xEF) {
        UINT32 compat   = rd_u32(d + 1024 + 92);
        UINT32 incompat = rd_u32(d + 1024 + 96);
        if      (incompat & 0x0240) { fs = "ext4"; extra = (incompat & 0x40) ? "extents" : "64bit"; }
        else if (compat & 0x0004)   { fs = "ext3"; extra = "has_journal"; }
        else                          fs = "ext2";
    }
    else if (allocB >= 0x10048 && AT(0x10040, "_BHRfS_M", 8))                          fs = "btrfs";
    else if (allocB >= 4 && AT(0, "XFSB", 4))                                          fs = "XFS";
    else if (allocB >= 6 && AT(0, "LUKS\xba\xbe", 6))                                  fs = "LUKS (encrypted)";
    else if (allocB > 32774 && AT(32769, "CD001", 5))                                  fs = "ISO9660";
    else if (allocB >= 4096 && (AT(4086, "SWAPSPACE2", 10) || AT(4086, "SWAP-SPACE", 10))) fs = "Linux swap";
    else if (allocB > 511 && d[510] == 0x55 && d[511] == 0xAA)                         fs = "FAT/MBR (bootable)";
    #undef AT

    con_setcol(FOREB_TITLE); con_puts("fsprobe dev "); con_putu(dev); con_puts(": ");
    if (fs) { con_puts(fs); if (extra) { con_setcol(FOREB_DIM); con_puts("  ("); con_puts(extra); con_puts(")"); } }
    else    { con_setcol(FOREB_TIMER); con_puts("unknown / raw"); }
    con_putc('\n'); con_setcol(FOREB_TEXT);
    sBS->FreePool(d);
}

/* -------- rescue (sector copy with bad-block skip) ------------------------ */
static void cmd_rescue(int argc, char **argv)
{
    if (argc < 3) {
        con_setcol(FOREB_TIMER); con_puts("usage: rescue <srcdev> <dstfile|dstdev> [skip-bad]\n");
        con_setcol(FOREB_DIM); con_puts("  dst is a device index (numeric) or an ESP file path.\n"); con_setcol(FOREB_TEXT); return;
    }
    UINT64 srcIdx;
    if (!s_parse_u64(argv[1], &srcIdx)) { con_setcol(FOREB_TIMER); con_puts("rescue: bad srcdev\n"); con_setcol(FOREB_TEXT); return; }
    int skipbad = (argc >= 4 && (s_ci_eq(argv[3], "skip-bad") || s_ci_eq(argv[3], "skipbad") || s_eq(argv[3], "1")));

    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *src = blockio_by_index((int)srcIdx, &total);
    if (!src || !src->Media || !src->Media->MediaPresent) {
        con_setcol(FOREB_TIMER); con_puts("rescue: source not present\n"); con_setcol(FOREB_TEXT); return;
    }
    EFI_BLOCK_IO_MEDIA *sm = src->Media;
    UINT32 bs = sm->BlockSize ? sm->BlockSize : 512;
    UINT64 nsec = sm->LastBlock + 1;
    UINT64 tbytes = nsec * (UINT64)bs;

    /* Resolve destination: numeric -> device, else -> ESP file. */
    UINT64 dstIdx = 0; int dstIsDev = s_parse_u64(argv[2], &dstIdx);
    EFI_BLOCK_IO_PROTOCOL *dstDev = NULL; EFI_FILE_PROTOCOL *dstFile = NULL;
    if (dstIsDev) {
        dstDev = blockio_by_index((int)dstIdx, &total);
        if (!dstDev || !dstDev->Media) { con_setcol(FOREB_TIMER); con_puts("rescue: bad dstdev\n"); con_setcol(FOREB_TEXT); return; }
        if (dstDev->Media->ReadOnly) { con_setcol(FOREB_TIMER); con_puts("rescue: dst is read-only\n"); con_setcol(FOREB_TEXT); return; }
        if (dstDev->Media->BlockSize != bs) { con_setcol(FOREB_TIMER); con_puts("rescue: src/dst block sizes differ\n"); con_setcol(FOREB_TEXT); return; }
        if ((UINT64)dstDev->Media->LastBlock + 1 < nsec) { con_setcol(FOREB_TIMER); con_puts("rescue: dst too small\n"); con_setcol(FOREB_TEXT); return; }
    }

    con_setcol(FOREB_TITLE); con_puts("*** RESCUE COPY ***\n"); con_setcol(FOREB_TEXT);
    con_puts("  source : dev "); con_putu(srcIdx); con_puts("  "); con_putu(tbytes >> 20); con_puts(" MiB, "); con_putu(nsec); con_puts(" sectors\n");
    con_puts("  dest   : "); if (dstIsDev) { con_puts("dev "); con_putu(dstIdx); } else con_puts(argv[2]);
    con_putc('\n');
    con_puts("  bad-blocks: "); con_puts(skipbad ? "SKIP (zero-fill, continue)\n" : "ABORT on first error\n");
    if (!confirm_yes()) return;

    if (!dstIsDev) {
        if (EFI_ERROR(esp_open(argv[2], EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_READ, &dstFile)) || !dstFile) {
            con_setcol(FOREB_TIMER); con_puts("rescue: cannot create dst file\n"); con_setcol(FOREB_TEXT); return;
        }
    }

    UINTN chunkSecs = 256;                                /* 128 KiB @ 512B     */
    UINTN chunkB = chunkSecs * bs;
    UINT8 *buf = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, chunkB, (VOID **)&buf)) || !buf) {
        con_setcol(FOREB_TIMER); con_puts("rescue: out of memory\n"); con_setcol(FOREB_TEXT);
        if (dstFile) dstFile->Close(dstFile); return;
    }

    UINT64 done = 0, bad = 0; int aborted = 0; int lastpct = -1;
    for (UINT64 lba = 0; lba < nsec; lba += chunkSecs) {
        UINTN thisSecs = (UINTN)((nsec - lba < chunkSecs) ? (nsec - lba) : chunkSecs);
        UINTN thisB = thisSecs * bs;
        EFI_STATUS rst = blk_read(src, lba, thisB, buf);
        if (EFI_ERROR(rst)) {
            if (!skipbad) {
                con_setcol(FOREB_TIMER); con_puts("\nrescue: read error at LBA "); con_putu(lba);
                con_puts(" (use skip-bad to continue)\n"); con_setcol(FOREB_TEXT); aborted = 1; break;
            }
            /* Recover good sectors one at a time; zero-fill the unreadable ones. */
            for (UINTN s = 0; s < thisSecs; s++) {
                if (EFI_ERROR(blk_read(src, lba + s, bs, buf + s * bs))) {
                    sBS->SetMem(buf + s * bs, bs, 0); bad++;
                }
            }
        }
        EFI_STATUS wst = EFI_SUCCESS;
        if (dstIsDev) wst = dstDev->WriteBlocks(dstDev, dstDev->Media->MediaId, lba, thisB, buf);
        else { UINTN wsz = thisB; wst = dstFile->Write(dstFile, &wsz, buf); }
        if (EFI_ERROR(wst)) {
            con_setcol(FOREB_TIMER); con_puts("\nrescue: write error at LBA "); con_putu(lba); con_putc('\n');
            con_setcol(FOREB_TEXT); aborted = 1; break;
        }
        done += thisSecs;
        int pct = (int)((done * 100) / nsec);
        if (pct != lastpct && (pct % 5 == 0)) {
            lastpct = pct;
            con_setcol(FOREB_DIM); con_puts("  "); con_puti(pct); con_puts("%  ("); con_putu(done);
            con_puts("/"); con_putu(nsec); con_puts(" sectors, bad="); con_putu(bad); con_puts(")\n");
            con_setcol(FOREB_TEXT);
        }
        if (esc_pressed()) { con_setcol(FOREB_TIMER); con_puts("\nrescue: aborted by user\n"); con_setcol(FOREB_TEXT); aborted = 1; break; }
    }
    if (dstIsDev) dstDev->FlushBlocks(dstDev);
    else if (dstFile) { dstFile->Flush(dstFile); dstFile->Close(dstFile); }
    sBS->FreePool(buf);
    if (!aborted) {
        con_setcol(FOREB_TITLE); con_puts("rescue complete: "); con_putu(done); con_puts(" sectors copied, ");
        con_putu(bad); con_puts(" bad sector(s) zero-filled\n"); con_setcol(FOREB_TEXT);
    }
}

/* -------- fatfix (FAT boot-sector / FSInfo / backup check + basic repair) - */
static void cmd_fatfix(int argc, char **argv)
{
    UINT64 dev;
    if (argc < 2 || !s_parse_u64(argv[1], &dev)) {
        con_setcol(FOREB_TIMER); con_puts("usage: fatfix <dev>\n"); con_setcol(FOREB_TEXT); return;
    }
    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio || !bio->Media) { con_setcol(FOREB_TIMER); con_puts("fatfix: no such device\n"); con_setcol(FOREB_TEXT); return; }
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    UINT8 *b0 = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bs, (VOID **)&b0)) || !b0) {
        con_setcol(FOREB_TIMER); con_puts("fatfix: out of memory\n"); con_setcol(FOREB_TEXT); return;
    }
    if (EFI_ERROR(blk_read(bio, 0, bs, b0))) { con_setcol(FOREB_TIMER); con_puts("fatfix: read failed\n"); con_setcol(FOREB_TEXT); sBS->FreePool(b0); return; }

    int sig_ok = (b0[510] == 0x55 && b0[511] == 0xAA);
    UINT16 bps = rd_u16(b0 + 11);
    UINT8  spc = b0[13];
    UINT16 rootEnt = rd_u16(b0 + 17);
    UINT16 fatsz16 = rd_u16(b0 + 22);
    int is_fat32 = (rootEnt == 0 && fatsz16 == 0);
    con_setcol(FOREB_TITLE); con_puts("fatfix dev "); con_putu(dev); con_putc('\n'); con_setcol(FOREB_TEXT);
    con_puts("  boot signature 0x55AA : "); con_puts(sig_ok ? "OK\n" : "MISSING\n");
    con_puts("  bytes/sector="); con_putu(bps); con_puts(" sectors/cluster="); con_putu(spc);
    con_puts(is_fat32 ? "  type=FAT32\n" : "  type=FAT12/16\n");
    int sane = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && (spc && (spc & (spc - 1)) == 0);
    if (!sane) { con_setcol(FOREB_TIMER); con_puts("  BPB looks INVALID (bps/spc not power-of-two)\n"); con_setcol(FOREB_TEXT); }

    int need_fix = 0;
    UINT16 bkboot = 0, fsinfo = 0;
    if (is_fat32) {
        fsinfo = rd_u16(b0 + 48);
        bkboot = rd_u16(b0 + 50);
        con_puts("  FSInfo sector="); con_putu(fsinfo); con_puts("  backup boot sector="); con_putu(bkboot); con_putc('\n');
        /* Compare backup boot sector to primary. */
        if (bkboot && (UINT64)bkboot <= m->LastBlock) {
            UINT8 *bk = NULL;
            if (!EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bs, (VOID **)&bk)) && bk) {
                if (!EFI_ERROR(blk_read(bio, bkboot, bs, bk))) {
                    int same = 1; for (UINT32 i = 0; i < bs; i++) if (bk[i] != b0[i]) { same = 0; break; }
                    con_puts("  backup boot sector : "); con_puts(same ? "matches primary\n" : "DIFFERS from primary\n");
                    if (!same && sig_ok && sane) need_fix = 1;
                } else con_puts("  backup boot sector : unreadable\n");
                sBS->FreePool(bk);
            }
        }
        /* FSInfo signature check. */
        if (fsinfo && (UINT64)fsinfo <= m->LastBlock) {
            UINT8 *fi = NULL;
            if (!EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bs, (VOID **)&fi)) && fi) {
                if (!EFI_ERROR(blk_read(bio, fsinfo, bs, fi))) {
                    int fsig = (rd_u32(fi + 0) == 0x41615252u && rd_u32(fi + 484) == 0x61417272u
                                && fi[510] == 0x55 && fi[511] == 0xAA);
                    con_puts("  FSInfo signatures  : "); con_puts(fsig ? "OK\n" : "BAD\n");
                }
                sBS->FreePool(fi);
            }
        }
    }
    if (!sig_ok && sane) need_fix = 1;

    if (!need_fix) {
        con_setcol(FOREB_TITLE); con_puts("fatfix: nothing to repair\n"); con_setcol(FOREB_TEXT);
        sBS->FreePool(b0); return;
    }
    if (m->ReadOnly) { con_setcol(FOREB_TIMER); con_puts("fatfix: device read-only, cannot repair\n"); con_setcol(FOREB_TEXT); sBS->FreePool(b0); return; }
    con_setcol(FOREB_TIMER); con_puts("fatfix: repair boot signature + refresh backup boot sector from primary?\n"); con_setcol(FOREB_TEXT);
    if (!confirm_yes()) { sBS->FreePool(b0); return; }

    b0[510] = 0x55; b0[511] = 0xAA;                        /* restore signature  */
    EFI_STATUS w = bio->WriteBlocks(bio, m->MediaId, 0, bs, b0);
    if (!EFI_ERROR(w) && is_fat32 && bkboot && (UINT64)bkboot <= m->LastBlock)
        w = bio->WriteBlocks(bio, m->MediaId, bkboot, bs, b0);
    if (EFI_ERROR(w)) { con_setcol(FOREB_TIMER); con_puts("fatfix: write failed\n"); }
    else { bio->FlushBlocks(bio); con_setcol(FOREB_TITLE); con_puts("fatfix: repaired\n"); }
    con_setcol(FOREB_TEXT);
    sBS->FreePool(b0);
}

/* -------- scan (best-effort magic carve summary) ------------------------- */
static void cmd_scan(int argc, char **argv)
{
    UINT64 dev;
    if (argc < 2 || !s_parse_u64(argv[1], &dev)) {
        con_setcol(FOREB_TIMER); con_puts("usage: scan <dev>   (carve known file magics, read-only)\n"); con_setcol(FOREB_TEXT); return;
    }
    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio || !bio->Media || !bio->Media->MediaPresent) { con_setcol(FOREB_TIMER); con_puts("scan: device not present\n"); con_setcol(FOREB_TEXT); return; }
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    UINT64 nsec = m->LastBlock + 1;
    UINT64 limitBytes = 128ull * 1024 * 1024;             /* cap the scan region */
    UINT64 scanSecs = (nsec * bs > limitBytes) ? (limitBytes / bs) : nsec;

    struct { const UINT8 *sig; int len; const char *name; UINT64 count; UINT64 first; } sigs[] = {
        {(const UINT8*)"\xFF\xD8\xFF", 3, "JPEG", 0, 0},
        {(const UINT8*)"\x89PNG",       4, "PNG",  0, 0},
        {(const UINT8*)"GIF8",          4, "GIF",  0, 0},
        {(const UINT8*)"%PDF",          4, "PDF",  0, 0},
        {(const UINT8*)"PK\x03\x04",   4, "ZIP/docx", 0, 0},
        {(const UINT8*)"\x1F\x8B",      2, "GZIP", 0, 0},
        {(const UINT8*)"\x7F""ELF",     4, "ELF",  0, 0},
        {(const UINT8*)"BM",            2, "BMP",  0, 0},
        {(const UINT8*)"RIFF",          4, "RIFF(wav/avi)", 0, 0},
        {(const UINT8*)"\x1F\x8B\x08", 3, "GZIP", 0, 0},
    };
    int nsig = (int)(sizeof(sigs) / sizeof(sigs[0]));

    UINTN chunkSecs = 512; UINTN chunkB = chunkSecs * bs;
    UINT8 *buf = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, chunkB, (VOID **)&buf)) || !buf) {
        con_setcol(FOREB_TIMER); con_puts("scan: out of memory\n"); con_setcol(FOREB_TEXT); return;
    }
    con_setcol(FOREB_DIM); con_puts("scanning "); con_putu((scanSecs * bs) >> 20); con_puts(" MiB (Esc aborts)...\n"); con_setcol(FOREB_TEXT);
    int lastpct = -1; UINT64 pos = 0;
    for (UINT64 lba = 0; lba < scanSecs; lba += chunkSecs) {
        UINTN thisSecs = (UINTN)((scanSecs - lba < chunkSecs) ? (scanSecs - lba) : chunkSecs);
        UINTN thisB = thisSecs * bs;
        if (EFI_ERROR(blk_read(bio, lba, thisB, buf))) { pos += thisB; continue; }  /* skip bad chunk */
        for (UINTN i = 0; i + 4 <= thisB; i++) {
            for (int s = 0; s < nsig; s++) {
                if (i + (UINTN)sigs[s].len > thisB) continue;
                int ok = 1;
                for (int k = 0; k < sigs[s].len; k++) if (buf[i + k] != sigs[s].sig[k]) { ok = 0; break; }
                if (ok) { if (!sigs[s].count) sigs[s].first = pos + i; sigs[s].count++; }
            }
        }
        pos += thisB;
        int pct = (int)((lba * 100) / scanSecs);
        if (pct != lastpct && pct % 10 == 0) { lastpct = pct; con_setcol(FOREB_DIM); con_puts("  "); con_puti(pct); con_puts("%\n"); con_setcol(FOREB_TEXT); }
        if (esc_pressed()) { con_setcol(FOREB_TIMER); con_puts("scan: aborted\n"); con_setcol(FOREB_TEXT); break; }
    }
    sBS->FreePool(buf);
    con_setcol(FOREB_TITLE); con_puts("carve summary (best-effort):\n"); con_setcol(FOREB_TEXT);
    int any = 0;
    for (int s = 0; s < nsig; s++) {
        if (!sigs[s].count) continue; any = 1;
        con_puts("  "); con_puts(sigs[s].name); con_puts(" : "); con_putu(sigs[s].count);
        con_puts(" hit(s), first @ "); con_puthex(sigs[s].first, 16); con_putc('\n');
    }
    if (!any) { con_setcol(FOREB_DIM); con_puts("  (no known magics found)\n"); con_setcol(FOREB_TEXT); }
    con_setcol(FOREB_DIM); con_puts("  Note: heuristic signature scan, not a full undelete.\n"); con_setcol(FOREB_TEXT);
}

/* -------- ext-ls / ext-cat / btrfs-snaps (wire the optional fs back-ends) - *
 * These call the real fs_ext.c / fs_btrfs.c drivers when their headers are
 * present (FOREB_HAVE_FS_EXT / FOREB_HAVE_FS_BTRFS), else degrade gracefully.  */
#ifdef FOREB_HAVE_FS_EXT
static void sh_ext_dirent(const char *name, uint32_t inode, uint8_t ft, void *user)
{
    (void)inode; (void)user;
    int isdir = (ft == EXT_FT_DIR);
    con_setcol(isdir ? FOREB_TITLE : (ft == EXT_FT_SYMLINK ? FOREB_DIM : FOREB_TEXT));
    con_puts(isdir ? "  <DIR>  " : (ft == EXT_FT_SYMLINK ? "  <LNK>  " : "         "));
    con_puts(name); con_putc('\n'); con_setcol(FOREB_TEXT);
}
#endif
#ifdef FOREB_HAVE_FS_BTRFS
static void sh_btrfs_snap(const char *name, uint64_t id, uint64_t parent, void *user)
{
    (void)parent; (void)user;
    con_puts("  subvol id "); con_putu(id); con_puts("  "); con_puts(name); con_putc('\n');
}
#endif

static void cmd_extls(int argc, char **argv)
{
#ifdef FOREB_HAVE_FS_EXT
    UINT64 dev; const char *path = (argc >= 3) ? argv[2] : "/";
    if (argc < 2 || !s_parse_u64(argv[1], &dev)) { con_setcol(FOREB_TIMER); con_puts("usage: ext-ls <dev> [path]\n"); con_setcol(FOREB_TEXT); return; }
    int total = 0; EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio) { con_setcol(FOREB_TIMER); con_puts("ext-ls: no such device\n"); con_setcol(FOREB_TEXT); return; }
    if (!ext_probe(sBS, bio)) { con_setcol(FOREB_TIMER); con_puts("ext-ls: not an ext2/3/4 volume\n"); con_setcol(FOREB_TEXT); return; }
    ext_ctx *ctx = ext_mount(sBS, bio, NULL);
    if (!ctx) { con_setcol(FOREB_TIMER); con_puts("ext-ls: mount failed\n"); con_setcol(FOREB_TEXT); return; }
    con_setcol(FOREB_TITLE); con_puts("ext "); con_puts(path); con_putc('\n'); con_setcol(FOREB_TEXT);
    if (ext_ls(ctx, path, sh_ext_dirent, 0) < 0) { con_setcol(FOREB_TIMER); con_puts("ext-ls: path not found / read error\n"); con_setcol(FOREB_TEXT); }
    ext_unmount(ctx);
#else
    (void)argc; (void)argv;
    con_setcol(FOREB_DIM); con_puts("ext-ls: ext2/3/4 back-end (fs_ext.c) not built in this image\n"); con_setcol(FOREB_TEXT);
#endif
}
static void cmd_extcat(int argc, char **argv)
{
#ifdef FOREB_HAVE_FS_EXT
    UINT64 dev;
    if (argc < 3 || !s_parse_u64(argv[1], &dev)) { con_setcol(FOREB_TIMER); con_puts("usage: ext-cat <dev> <path>\n"); con_setcol(FOREB_TEXT); return; }
    int total = 0; EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio) { con_setcol(FOREB_TIMER); con_puts("ext-cat: no such device\n"); con_setcol(FOREB_TEXT); return; }
    if (!ext_probe(sBS, bio)) { con_setcol(FOREB_TIMER); con_puts("ext-cat: not an ext2/3/4 volume\n"); con_setcol(FOREB_TEXT); return; }
    ext_ctx *ctx = ext_mount(sBS, bio, NULL);
    if (!ctx) { con_setcol(FOREB_TIMER); con_puts("ext-cat: mount failed\n"); con_setcol(FOREB_TEXT); return; }
    int64_t sz = ext_file_size(ctx, argv[2]);
    if (sz < 0) { con_setcol(FOREB_TIMER); con_puts("ext-cat: not a readable file\n"); con_setcol(FOREB_TEXT); ext_unmount(ctx); return; }
    UINT64 cap = (sz > 1024 * 1024) ? 1024 * 1024 : (UINT64)sz;   /* clamp to 1 MiB */
    VOID *buf = NULL;
    if (cap && (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, (UINTN)cap, &buf)) || !buf)) {
        con_setcol(FOREB_TIMER); con_puts("ext-cat: out of memory\n"); con_setcol(FOREB_TEXT); ext_unmount(ctx); return;
    }
    int64_t n = cap ? ext_read(ctx, argv[2], buf, cap) : 0;
    if (n < 0) { con_setcol(FOREB_TIMER); con_puts("ext-cat: read error\n"); con_setcol(FOREB_TEXT); }
    else {
        const UINT8 *p = (const UINT8 *)buf;
        for (int64_t i = 0; i < n; i++) con_putc((char)p[i]);
        if (n && p[n - 1] != '\n') con_putc('\n');
        if ((UINT64)sz > cap) { con_setcol(FOREB_DIM); con_puts("...(truncated to 1 MiB)\n"); con_setcol(FOREB_TEXT); }
    }
    if (buf) sBS->FreePool(buf);
    ext_unmount(ctx);
#else
    (void)argc; (void)argv;
    con_setcol(FOREB_DIM); con_puts("ext-cat: ext2/3/4 back-end (fs_ext.c) not built in this image\n"); con_setcol(FOREB_TEXT);
#endif
}
static void cmd_btrfssnaps(int argc, char **argv)
{
#ifdef FOREB_HAVE_FS_BTRFS
    UINT64 dev;
    if (argc < 2 || !s_parse_u64(argv[1], &dev)) { con_setcol(FOREB_TIMER); con_puts("usage: btrfs-snaps <dev>\n"); con_setcol(FOREB_TEXT); return; }
    int total = 0; EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev, &total);
    if (!bio) { con_setcol(FOREB_TIMER); con_puts("btrfs-snaps: no such device\n"); con_setcol(FOREB_TEXT); return; }
    if (!btrfs_probe(sBS, bio)) { con_setcol(FOREB_TIMER); con_puts("btrfs-snaps: not a btrfs volume\n"); con_setcol(FOREB_TEXT); return; }
    con_setcol(FOREB_TITLE); con_puts("btrfs subvolumes / snapshots:\n"); con_setcol(FOREB_TEXT);
    int r = btrfs_list_snapshots(sBS, bio, NULL, sh_btrfs_snap, 0);
    if (r < 0) { con_setcol(FOREB_TIMER); con_puts("btrfs-snaps: read error\n"); con_setcol(FOREB_TEXT); }
    else { con_setcol(FOREB_DIM); con_puti(r); con_puts(" subvolume(s)/snapshot(s)\n"); con_setcol(FOREB_TEXT); }
#else
    (void)argc; (void)argv;
    con_setcol(FOREB_DIM); con_puts("btrfs-snaps: btrfs back-end (fs_btrfs.c) not built in this image\n"); con_setcol(FOREB_TEXT);
#endif
}

/* =============================================================================
 * Commands.
 * ==========================================================================*/
static void cmd_help(int argc, char **argv)
{
    if (argc >= 2) {
        const char *c = argv[1];
        con_setcol(FOREB_TITLE);
        if      (s_ci_eq(c, "ls"))       con_puts("ls [path]        - list an ESP directory (default \\)");
        else if (s_ci_eq(c, "cat"))      con_puts("cat <file>       - print a text file from the ESP");
        else if (s_ci_eq(c, "hexdump"))  con_puts("hexdump <f> [n]  - hex+ASCII dump (default 256 bytes)");
        else if (s_ci_eq(c, "lsblk"))    con_puts("lsblk            - list EFI_BLOCK_IO devices");
        else if (s_ci_eq(c, "read"))     con_puts("read <dev> <lba> [n] - dump n sectors (read-only)");
        else if (s_ci_eq(c, "write"))    con_puts("write <dev> <lba> <file> - DESTRUCTIVE; asks 'yes'");
        else if (s_ci_eq(c, "drives"))   con_puts("drives           - list SimpleFileSystem volumes");
        else if (s_ci_eq(c, "devices"))  con_puts("devices          - inventory input+storage devices with type");
        else if (s_ci_eq(c, "inputtest")) con_puts("inputtest        - live key/pointer echo; press c to cancel");
        else if (s_ci_eq(c, "modules"))  con_puts("modules [add <p>]- list/append the entry's modules");
        else if (s_ci_eq(c, "efivars"))  con_puts("efivars          - enumerate all UEFI variables");
        else if (s_ci_eq(c, "bootvars")) con_puts("bootvars         - Boot####/BootOrder global vars");
        else if (s_ci_eq(c, "getvar"))   con_puts("getvar <name> [guid] - print one variable");
        else if (s_ci_eq(c, "setvar"))   con_puts("setvar <name> <guid> <hex> - set a variable");
        else if (s_ci_eq(c, "background")) con_puts("background <file>- set the menu background image");
        else if (s_ci_eq(c, "memmap"))   con_puts("memmap           - EFI memory map summary");
        else if (s_ci_eq(c, "config"))   con_puts("config           - reload forebo.cfg from the ESP");
        else if (s_ci_eq(c, "boot"))     con_puts("boot [idx|title] - boot an entry now");
        else if (s_ci_eq(c, "gpt"))      con_puts("gpt <dev>        - parse+print the GPT header + partitions");
        else if (s_ci_eq(c, "parts"))    con_puts("parts            - all block devices + partition summary");
        else if (s_ci_eq(c, "fsprobe"))  con_puts("fsprobe <dev>    - identify FS by magic (FAT/ext/btrfs/NTFS...)");
        else if (s_ci_eq(c, "rescue"))   con_puts("rescue <src> <dst> [skip-bad] - sector copy; asks 'yes'");
        else if (s_ci_eq(c, "fatfix"))   con_puts("fatfix <dev>     - check/repair FAT boot sector+backup; asks 'yes'");
        else if (s_ci_eq(c, "scan"))     con_puts("scan <dev>       - best-effort file-magic carve summary");
        else if (s_ci_eq(c, "ext-ls"))   con_puts("ext-ls <dev> [p] - list an ext2/3/4 directory (read-only)");
        else if (s_ci_eq(c, "ext-cat"))  con_puts("ext-cat <dev> <p>- print an ext2/3/4 file (read-only)");
        else if (s_ci_eq(c, "btrfs-snaps")) con_puts("btrfs-snaps <dev>- list btrfs subvolumes/snapshots");
        else                             { con_setcol(FOREB_TIMER); con_puts("no help for '"); con_puts(c); con_puts("'"); }
        con_putc('\n');
        con_setcol(FOREB_TEXT);
        return;
    }
    con_setcol(FOREB_TITLE);  con_puts("ForeB shell commands:"); con_putc('\n');
    con_setcol(FOREB_TEXT);
    con_puts("  help [cmd]  clear        cd [dir]      pwd"); con_putc('\n');
    con_puts("  ls [path]   cat <file>   hexdump <file> [len]"); con_putc('\n');
    con_puts("  lsblk       drives       devices       inputtest"); con_putc('\n');
    con_puts("  read <dev> <lba> [n]     write <dev> <lba> <file>  (DESTRUCTIVE)"); con_putc('\n');
    con_puts("  modules [add <path>]     efivars       bootvars"); con_putc('\n');
    con_puts("  getvar <name> [guid]     setvar <name> <guid> <hex>"); con_putc('\n');
    con_puts("  background <file>        memmap        config"); con_putc('\n');
    con_puts("  boot [idx|title]         reboot        exit"); con_putc('\n');
    con_puts("  setup / firmware  (enter UEFI setup)   tools  (GUI tools)"); con_putc('\n');
    con_setcol(FOREB_TITLE); con_puts("Recovery / disk-fix:"); con_putc('\n'); con_setcol(FOREB_TEXT);
    con_puts("  gpt <dev>   parts   fsprobe <dev>   scan <dev>"); con_putc('\n');
    con_puts("  rescue <srcdev> <dstfile|dstdev> [skip-bad]   (DESTRUCTIVE)"); con_putc('\n');
    con_puts("  fatfix <dev>  (DESTRUCTIVE)   ext-ls <dev> [path]"); con_putc('\n');
    con_puts("  ext-cat <dev> <path>         btrfs-snaps <dev>"); con_putc('\n');
    con_setcol(FOREB_DIM);
    con_puts("  (Esc also leaves the shell; destructive tools ask 'yes')"); con_putc('\n');
    con_setcol(FOREB_TEXT);
}

/* pwd - print the current working directory. */
static void cmd_pwd(void)
{
    con_setcol(FOREB_TEXT); con_puts(s_cwd); con_putc('\n');
}

/* cd [dir] - change the working directory on the ESP. No arg (or "/") -> root.
 * Verifies the target exists and is a directory before committing. */
static void cmd_cd(int argc, char **argv)
{
    if (argc < 2 || s_eq(argv[1], "/") || s_eq(argv[1], "\\")) {
        s_cwd[0] = '\\'; s_cwd[1] = 0; return;
    }

    char abs[FOREB_CFG_PATH_LEN * 2];
    path_resolve(argv[1], abs, (int)sizeof(abs));

    EFI_FILE_PROTOCOL *d = NULL;
    if (EFI_ERROR(esp_open(abs, EFI_FILE_MODE_READ, &d)) || !d) {
        con_setcol(FOREB_TIMER); con_puts("cd: no such directory: ");
        con_puts(argv[1]); con_putc('\n'); con_setcol(FOREB_TEXT); return;
    }
    UINT8 info[512]; UINTN isz = sizeof(info);
    EFI_STATUS st = d->GetInfo(d, &gFileInfoGuid, &isz, info);
    d->Close(d);
    if (EFI_ERROR(st) || !(((EFI_FILE_INFO *)info)->Attribute & EFI_FILE_DIRECTORY)) {
        con_setcol(FOREB_TIMER); con_puts("cd: not a directory: ");
        con_puts(argv[1]); con_putc('\n'); con_setcol(FOREB_TEXT); return;
    }
    int i = 0;
    for (const char *p = abs; *p && i < (int)sizeof(s_cwd) - 1; p++) s_cwd[i++] = *p;
    s_cwd[i] = 0;
}

static void cmd_ls(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : ".";   /* bare `ls` -> cwd */
    EFI_FILE_PROTOCOL *dir = NULL;
    if (EFI_ERROR(esp_open(path, EFI_FILE_MODE_READ, &dir)) || !dir) {
        con_setcol(FOREB_TIMER); con_puts("ls: cannot open '"); con_puts(path); con_puts("'\n");
        con_setcol(FOREB_TEXT); return;
    }
    UINT8 buf[1024];
    int count = 0;
    for (;;) {
        UINTN sz = sizeof(buf);
        if (EFI_ERROR(dir->Read(dir, &sz, buf)) || sz == 0) break;
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
        char name[FOREB_CFG_PATH_LEN];
        s_u2a(fi->FileName, name, sizeof(name));
        if (s_eq(name, ".") || s_eq(name, "..")) continue;
        int isdir = (fi->Attribute & EFI_FILE_DIRECTORY) != 0;
        con_setcol(isdir ? FOREB_TITLE : FOREB_TEXT);
        con_puts(isdir ? "  <DIR>  " : "         ");
        if (!isdir) { con_putu(fi->FileSize); con_puts("  "); }
        con_puts(name); con_putc('\n');
        count++;
    }
    dir->Close(dir);
    con_setcol(FOREB_DIM); con_puti(count); con_puts(" item(s)\n");
    con_setcol(FOREB_TEXT);
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) { con_setcol(FOREB_TIMER); con_puts("usage: cat <file>\n"); con_setcol(FOREB_TEXT); return; }
    VOID *buf = NULL; UINTN sz = 0;
    EFI_STATUS st = esp_load(argv[1], 128u * 1024u, &buf, &sz);
    if (EFI_ERROR(st) || !buf) {
        con_setcol(FOREB_TIMER); con_puts("cat: cannot read '"); con_puts(argv[1]); con_puts("'\n");
        con_setcol(FOREB_TEXT); return;
    }
    const UINT8 *p = (const UINT8 *)buf;
    for (UINTN i = 0; i < sz; i++) con_putc((char)p[i]);
    if (sz && p[sz - 1] != '\n') con_putc('\n');
    sBS->FreePool(buf);
}

static void cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) { con_setcol(FOREB_TIMER); con_puts("usage: hexdump <file> [len]\n"); con_setcol(FOREB_TEXT); return; }
    UINT64 len = 256;
    if (argc >= 3 && !s_parse_u64(argv[2], &len)) len = 256;
    if (len > 64u * 1024u) len = 64u * 1024u;
    VOID *buf = NULL; UINTN sz = 0;
    if (EFI_ERROR(esp_load(argv[1], (UINTN)len, &buf, &sz)) || !buf) {
        con_setcol(FOREB_TIMER); con_puts("hexdump: cannot read '"); con_puts(argv[1]); con_puts("'\n");
        con_setcol(FOREB_TEXT); return;
    }
    hexdump_bytes((const UINT8 *)buf, sz, 0);
    con_setcol(FOREB_DIM); con_putu(sz); con_puts(" byte(s)\n"); con_setcol(FOREB_TEXT);
    sBS->FreePool(buf);
}

static void cmd_lsblk(void)
{
    EFI_HANDLE *h = NULL; UINTN n = 0;
    if (EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gBlockIoGuid, NULL, &n, &h)) || !h) {
        con_setcol(FOREB_TIMER); con_puts("lsblk: no block devices\n"); con_setcol(FOREB_TEXT); return;
    }
    con_setcol(FOREB_TITLE);
    con_puts("dev  blocksz    lastLBA      size      flags\n");
    con_setcol(FOREB_TEXT);
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        if (EFI_ERROR(sBS->HandleProtocol(h[i], &gBlockIoGuid, (VOID **)&bio)) || !bio || !bio->Media)
            continue;
        EFI_BLOCK_IO_MEDIA *m = bio->Media;
        con_putc('['); con_putu(i); con_puts("]  ");
        con_putu(m->BlockSize); con_puts("  ");
        con_putu((UINT64)m->LastBlock); con_puts("  ");
        /* size in MiB */
        UINT64 mib = ((m->LastBlock + 1) * (UINT64)m->BlockSize) >> 20;
        con_putu(mib); con_puts("MiB  ");
        if (m->RemovableMedia) con_puts("removable ");
        if (!m->MediaPresent)  con_puts("no-media ");
        if (m->ReadOnly)       con_puts("ro ");
        if (m->LogicalPartition) con_puts("part ");
        con_putc('\n');
    }
    sBS->FreePool(h);
}

static void cmd_read(int argc, char **argv)
{
    UINT64 dev64, lba, cnt = 1;
    if (argc < 3 || !s_parse_u64(argv[1], &dev64) || !s_parse_u64(argv[2], &lba)) {
        con_setcol(FOREB_TIMER); con_puts("usage: read <dev> <lba> [count]\n"); con_setcol(FOREB_TEXT); return;
    }
    if (argc >= 4 && !s_parse_u64(argv[3], &cnt)) cnt = 1;
    if (cnt < 1) cnt = 1;
    if (cnt > 64) { cnt = 64; con_setcol(FOREB_DIM); con_puts("(count clamped to 64 sectors)\n"); con_setcol(FOREB_TEXT); }

    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev64, &total);
    if (!bio || !bio->Media) {
        con_setcol(FOREB_TIMER); con_puts("read: no such device (0.."); con_puti(total - 1); con_puts(")\n");
        con_setcol(FOREB_TEXT); return;
    }
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    if (lba + cnt - 1 > (UINT64)m->LastBlock) {
        con_setcol(FOREB_TIMER); con_puts("read: LBA range past LastBlock ("); con_putu((UINT64)m->LastBlock); con_puts(")\n");
        con_setcol(FOREB_TEXT); return;
    }
    UINTN bytes = (UINTN)(cnt * (UINT64)m->BlockSize);
    VOID *buf = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, bytes, &buf)) || !buf) {
        con_setcol(FOREB_TIMER); con_puts("read: out of memory\n"); con_setcol(FOREB_TEXT); return;
    }
    EFI_STATUS st = bio->ReadBlocks(bio, m->MediaId, lba, bytes, buf);
    if (EFI_ERROR(st)) {
        con_setcol(FOREB_TIMER); con_puts("read: ReadBlocks failed ("); con_puthex(st, 16); con_puts(")\n");
        con_setcol(FOREB_TEXT); sBS->FreePool(buf); return;
    }
    hexdump_bytes((const UINT8 *)buf, bytes, lba * (UINT64)m->BlockSize);
    sBS->FreePool(buf);
}

static void cmd_write(int argc, char **argv)
{
    UINT64 dev64, lba;
    if (argc < 4 || !s_parse_u64(argv[1], &dev64) || !s_parse_u64(argv[2], &lba)) {
        con_setcol(FOREB_TIMER); con_puts("usage: write <dev> <lba> <file>\n"); con_setcol(FOREB_TEXT); return;
    }
    int total = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = blockio_by_index((int)dev64, &total);
    if (!bio || !bio->Media) {
        con_setcol(FOREB_TIMER); con_puts("write: no such device\n"); con_setcol(FOREB_TEXT); return;
    }
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    if (m->ReadOnly) {
        con_setcol(FOREB_TIMER); con_puts("write: device is read-only - refused\n"); con_setcol(FOREB_TEXT); return;
    }
    VOID *fbuf = NULL; UINTN fsz = 0;
    if (EFI_ERROR(esp_load(argv[3], 16u * 1024u * 1024u, &fbuf, &fsz)) || !fbuf || fsz == 0) {
        con_setcol(FOREB_TIMER); con_puts("write: cannot read source '"); con_puts(argv[3]); con_puts("'\n");
        con_setcol(FOREB_TEXT); if (fbuf) sBS->FreePool(fbuf); return;
    }
    UINT32 bs = m->BlockSize ? m->BlockSize : 512;
    UINT64 sectors = (fsz + bs - 1) / bs;
    if (lba + sectors - 1 > (UINT64)m->LastBlock) {
        con_setcol(FOREB_TIMER);
        con_puts("write: range past LastBlock ("); con_putu((UINT64)m->LastBlock); con_puts(") - refused\n");
        con_setcol(FOREB_TEXT); sBS->FreePool(fbuf); return;
    }

    /* Summary + hard confirmation gate. */
    con_setcol(FOREB_TIMER);
    con_puts("*** DESTRUCTIVE WRITE ***\n");
    con_setcol(FOREB_TEXT);
    con_puts("  device   : dev "); con_putu(dev64);
    con_puts("  blocksz "); con_putu(bs);
    con_puts("  lastLBA "); con_putu((UINT64)m->LastBlock); con_putc('\n');
    con_puts("  source   : "); con_puts(argv[3]); con_puts(" ("); con_putu(fsz); con_puts(" bytes)\n");
    con_puts("  target   : LBA "); con_putu(lba); con_puts(" .. "); con_putu(lba + sectors - 1);
    con_puts("  ("); con_putu(sectors); con_puts(" sector(s))\n");
    con_setcol(FOREB_TIMER);
    con_puts("This CANNOT be undone. Type 'yes' to proceed:\n");
    con_setcol(FOREB_TEXT);

    char resp[16];
    int r = read_line("confirm> ", resp, sizeof(resp));
    if (r < 0 || !s_eq(resp, "yes")) {
        con_setcol(FOREB_TITLE); con_puts("write aborted (no changes made)\n"); con_setcol(FOREB_TEXT);
        sBS->FreePool(fbuf); return;
    }

    /* Whole-sector buffer, zero-padded tail. */
    UINTN wbytes = (UINTN)(sectors * bs);
    VOID *wbuf = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, wbytes, &wbuf)) || !wbuf) {
        con_setcol(FOREB_TIMER); con_puts("write: out of memory\n"); con_setcol(FOREB_TEXT);
        sBS->FreePool(fbuf); return;
    }
    sBS->SetMem(wbuf, wbytes, 0);
    sBS->CopyMem(wbuf, fbuf, fsz);
    EFI_STATUS st = bio->WriteBlocks(bio, m->MediaId, lba, wbytes, wbuf);
    if (EFI_ERROR(st)) {
        con_setcol(FOREB_TIMER); con_puts("write: WriteBlocks failed ("); con_puthex(st, 16); con_puts(")\n");
    } else {
        bio->FlushBlocks(bio);
        con_setcol(FOREB_TITLE);
        con_puts("write ok: "); con_putu(sectors); con_puts(" sector(s) written");
        if (fsz % bs) con_puts(" (final sector zero-padded)");
        con_putc('\n');
    }
    con_setcol(FOREB_TEXT);
    sBS->FreePool(wbuf); sBS->FreePool(fbuf);
}

static void cmd_drives(void)
{
    EFI_HANDLE *h = NULL; UINTN n = 0;
    if (EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gSfsGuid, NULL, &n, &h)) || !h) {
        con_setcol(FOREB_TIMER); con_puts("drives: no filesystem volumes\n"); con_setcol(FOREB_TEXT); return;
    }
    for (UINTN i = 0; i < n; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        if (EFI_ERROR(sBS->HandleProtocol(h[i], &gSfsGuid, (VOID **)&fs)) || !fs) continue;
        EFI_FILE_PROTOCOL *root = NULL;
        con_putc('['); con_putu(i); con_puts("] ");
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) { con_puts("(mount failed)\n"); continue; }
        UINT8 info[512]; UINTN isz = sizeof(info);
        if (!EFI_ERROR(root->GetInfo(root, &gFsInfoGuid, &isz, info))) {
            FOREB_EFI_FILE_SYSTEM_INFO *si = (FOREB_EFI_FILE_SYSTEM_INFO *)info;
            char label[64]; s_u2a(si->VolumeLabel, label, sizeof(label));
            con_setcol(FOREB_TITLE);
            con_puts(label[0] ? label : "(no label)");
            con_setcol(FOREB_TEXT);
            con_puts("  "); con_putu(si->VolumeSize >> 20); con_puts("MiB total, ");
            con_putu(si->FreeSpace >> 20); con_puts("MiB free");
            if (si->ReadOnly) con_puts(" [ro]");
        } else {
            con_puts("(volume info unavailable)");
        }
        con_putc('\n');
        root->Close(root);
    }
    sBS->FreePool(h);
}

/* Classify a block device by walking its device path for the transport bus.
 * Returns a static human string; sets *optical when a CD/DVD media node is seen.
 * Firmware cannot report platter-vs-flash for SATA/ATA, so those stay honest. */
static const char *storage_kind(EFI_DEVICE_PATH_PROTOCOL *dp, int removable, int *optical)
{
    const char *kind = NULL;
    *optical = 0;
    if (dp) {
        for (EFI_DEVICE_PATH_PROTOCOL *node = dp; !EFI_DP_IS_END(node); node = EFI_DP_NEXT(node)) {
            UINT16 len = EFI_DP_NODE_LEN(node);
            if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL)) break;   /* malformed - stop */
            if (node->Type == MESSAGING_DEVICE_PATH) {
                switch (node->SubType) {
                    case MSG_NVME_DP:  kind = "NVMe SSD";            break;
                    case MSG_SATA_DP:  kind = "SATA disk (SSD/HDD)"; break;
                    case MSG_ATAPI_DP: kind = "ATA/IDE disk";        break;
                    case MSG_SCSI_DP:  kind = "SCSI disk";           break;
                    case MSG_USB_DP:   kind = "USB storage";         break;
                    case MSG_UFS_DP:   kind = "UFS SSD";             break;
                    case MSG_SD_DP:    kind = "SD card";             break;
                    case MSG_EMMC_DP:  kind = "eMMC flash";          break;
                    default: break;
                }
            } else if (node->Type == MEDIA_DEVICE_PATH && node->SubType == MEDIA_CDROM_DP) {
                *optical = 1;
            }
        }
    }
    if (*optical) return "optical (CD/DVD)";
    if (kind)     return kind;
    return removable ? "removable disk" : "disk";
}

/* devices - inventory of input + storage hardware with a type for each. */
static void cmd_devices(void)
{
    EFI_HANDLE *h = NULL; UINTN n = 0;

    /* ---- keyboards / text input ---- */
    con_setcol(FOREB_TITLE); con_puts("Input - keyboards:\n"); con_setcol(FOREB_TEXT);
    n = 0;
    if (!EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gTextInGuid, NULL, &n, &h)) && h) {
        for (UINTN i = 0; i < n; i++) { con_puts("  [kbd "); con_putu(i); con_puts("] keyboard / text-input\n"); }
        sBS->FreePool(h); h = NULL;
    } else con_puts("  (none)\n");

    /* ---- mice / pointers ---- */
    con_setcol(FOREB_TITLE); con_puts("Input - pointers:\n"); con_setcol(FOREB_TEXT);
    int pcount = 0;
    n = 0;
    if (!EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gPointerGuid, NULL, &n, &h)) && h) {
        for (UINTN i = 0; i < n; i++) {
            EFI_SIMPLE_POINTER_PROTOCOL *p = NULL;
            if (EFI_ERROR(sBS->HandleProtocol(h[i], &gPointerGuid, (VOID **)&p)) || !p) continue;
            con_puts("  [mouse "); con_putu(i); con_puts("] relative pointer (mouse/trackpad)");
            if (p->Mode) {
                con_puts("  buttons:");
                if (p->Mode->LeftButton)  con_puts(" L");
                if (p->Mode->RightButton) con_puts(" R");
            }
            con_putc('\n'); pcount++;
        }
        sBS->FreePool(h); h = NULL;
    }
    n = 0;
    if (!EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gAbsPtrGuid, NULL, &n, &h)) && h) {
        for (UINTN i = 0; i < n; i++) { con_puts("  [touch "); con_putu(i); con_puts("] absolute pointer (touch/tablet)\n"); pcount++; }
        sBS->FreePool(h); h = NULL;
    }
    if (!pcount) con_puts("  (none)\n");

    /* ---- storage ---- */
    con_setcol(FOREB_TITLE); con_puts("Storage:\n"); con_setcol(FOREB_TEXT);
    n = 0;
    if (!EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gBlockIoGuid, NULL, &n, &h)) && h) {
        for (UINTN i = 0; i < n; i++) {
            EFI_BLOCK_IO_PROTOCOL *bio = NULL;
            if (EFI_ERROR(sBS->HandleProtocol(h[i], &gBlockIoGuid, (VOID **)&bio)) || !bio || !bio->Media) continue;
            EFI_BLOCK_IO_MEDIA *m = bio->Media;
            EFI_DEVICE_PATH_PROTOCOL *dp = NULL;
            sBS->HandleProtocol(h[i], &gDevPathGuid, (VOID **)&dp);
            con_puts("  [dev "); con_putu(i); con_puts("] ");
            if (m->LogicalPartition) {
                con_setcol(FOREB_DIM); con_puts("partition"); con_setcol(FOREB_TEXT);
            } else {
                int optical = 0;
                const char *kind = storage_kind(dp, (int)m->RemovableMedia, &optical);
                con_setcol(FOREB_TITLE); con_puts(kind); con_setcol(FOREB_TEXT);
            }
            UINT64 mib = ((m->LastBlock + 1) * (UINT64)m->BlockSize) >> 20;
            con_puts("  "); con_putu(mib); con_puts("MiB");
            if (m->RemovableMedia) con_puts("  removable");
            if (!m->MediaPresent)  con_puts("  no-media");
            if (m->ReadOnly)       con_puts("  ro");
            con_putc('\n');
        }
        sBS->FreePool(h); h = NULL;
    } else con_puts("  (none)\n");

    /* ---- audio ---- */
    con_setcol(FOREB_TITLE); con_puts("Audio:\n"); con_setcol(FOREB_TEXT);
    con_setcol(FOREB_DIM);
    con_puts("  (UEFI exposes no standard audio protocol pre-boot)\n");
    con_puts("Tip: 'inputtest' tests keys + pointer live (press c to cancel).\n");
    con_setcol(FOREB_TEXT);
}

/* inputtest - live keyboard + pointer echo. Press 'c' to cancel. */
static void cmd_inputtest(void)
{
    EFI_HANDLE *h = NULL; UINTN n = 0;
    EFI_SIMPLE_POINTER_PROTOCOL *ptr = NULL;
    if (!EFI_ERROR(sBS->LocateHandleBuffer(ByProtocol, &gPointerGuid, NULL, &n, &h)) && h) {
        for (UINTN i = 0; i < n && !ptr; i++)
            sBS->HandleProtocol(h[i], &gPointerGuid, (VOID **)&ptr);
        sBS->FreePool(h);
    }
    if (ptr) ptr->Reset(ptr, FALSE);

    con_setcol(FOREB_TITLE); con_puts("Input test - press keys or move the pointer.\n");
    con_setcol(FOREB_DIM);
    con_puts("Press 'c' to cancel.");
    if (!ptr) con_puts("  (no pointer device present)");
    con_putc('\n');
    con_setcol(FOREB_TEXT);

    for (;;) {
        EFI_INPUT_KEY key;
        sBS->Stall(5000);                         /* 5 ms poll (interleaves keys + pointer) */

        while (sIn && !EFI_ERROR(sIn->ReadKeyStroke(sIn, &key))) {  /* drain the buffer */
            CHAR16 ch = key.UnicodeChar;
            UINT16 sc = key.ScanCode;
            if (ch == 'c' || ch == 'C' || sc == SCAN_ESC) {
                con_setcol(FOREB_TITLE); con_puts("input test cancelled.\n"); con_setcol(FOREB_TEXT);
                return;
            }
            con_puts("key: ");
            if (ch >= 0x20 && ch < 0x7f) { con_puts("char '"); con_putc((char)ch); con_puts("'"); }
            else                         { con_puts("char=0x"); con_puthex(ch, 4); }
            con_puts("  scan=0x"); con_puthex(sc, 4);
            con_putc('\n');
        }

        if (ptr) {
            EFI_SIMPLE_POINTER_STATE st;
            if (!EFI_ERROR(ptr->GetState(ptr, &st)) &&
                (st.RelativeMovementX || st.RelativeMovementY ||
                 st.RelativeMovementZ || st.LeftButton || st.RightButton)) {
                con_puts("ptr: dx="); con_puti((int)st.RelativeMovementX);
                con_puts(" dy=");     con_puti((int)st.RelativeMovementY);
                if (st.RelativeMovementZ) { con_puts(" wheel="); con_puti((int)st.RelativeMovementZ); }
                if (st.LeftButton)  con_puts(" [L]");
                if (st.RightButton) con_puts(" [R]");
                con_putc('\n');
            }
        }
    }
}

static void cmd_modules(int argc, char **argv)
{
    if (!sCfg || sCfg->count <= 0) {
        con_setcol(FOREB_TIMER); con_puts("modules: no config loaded\n"); con_setcol(FOREB_TEXT); return;
    }
    int e = sSel;
    if (e < 0 || e >= sCfg->count) e = sCfg->default_idx;
    if (e < 0 || e >= sCfg->count) e = 0;
    struct forebo_menuentry *me = &sCfg->entries[e];

    if (argc >= 3 && s_ci_eq(argv[1], "add")) {
        if (me->module_count >= FOREB_CFG_MAX_MODULES) {
            con_setcol(FOREB_TIMER); con_puts("modules: entry module list is full\n"); con_setcol(FOREB_TEXT); return;
        }
        /* Validate the file exists before registering it. */
        EFI_FILE_PROTOCOL *f = NULL;
        if (EFI_ERROR(esp_open(argv[2], EFI_FILE_MODE_READ, &f)) || !f) {
            con_setcol(FOREB_TIMER); con_puts("modules: cannot open '"); con_puts(argv[2]); con_puts("'\n");
            con_setcol(FOREB_TEXT); return;
        }
        f->Close(f);
        s_strcpy(me->modules[me->module_count], argv[2], FOREB_CFG_PATH_LEN);
        me->module_count++;
        con_setcol(FOREB_TITLE); con_puts("added module: "); con_puts(argv[2]); con_putc('\n');
        con_setcol(FOREB_TEXT);
        return;
    }

    con_setcol(FOREB_TITLE);
    con_puts("Modules for entry ["); con_puti(e); con_puts("] "); con_puts(me->title); con_putc('\n');
    con_setcol(FOREB_TEXT);
    if (me->module_count == 0) { con_setcol(FOREB_DIM); con_puts("  (none)\n"); con_setcol(FOREB_TEXT); return; }
    for (int i = 0; i < me->module_count; i++) {
        con_puts("  "); con_puti(i); con_puts(": "); con_puts(me->modules[i]); con_putc('\n');
    }
}

static int is_boot_option_name(const char *n)  /* "Boot" + 4 hex digits */
{
    if (s_strlen(n) != 8) return 0;
    if (n[0] != 'B' || n[1] != 'o' || n[2] != 'o' || n[3] != 't') return 0;
    for (int i = 4; i < 8; i++) if (s_hexval(n[i]) < 0) return 0;
    return 1;
}

static void cmd_efivars(void)
{
    CHAR16 name[512]; EFI_GUID g;
    name[0] = 0;
    int count = 0;
    for (;;) {
        UINTN nsz = sizeof(name);
        EFI_STATUS st = sRT->GetNextVariableName(&nsz, name, &g);
        if (st == EFI_NOT_FOUND) break;
        if (EFI_ERROR(st)) { con_setcol(FOREB_TIMER); con_puts("(enumeration ended)\n"); con_setcol(FOREB_TEXT); break; }
        char a[256]; s_u2a(name, a, sizeof(a));
        con_puts(a);
        con_setcol(FOREB_DIM); con_puts("  {"); con_putguid(&g); con_puts("}"); con_setcol(FOREB_TEXT);
        con_putc('\n');
        if (++count >= 400) { con_setcol(FOREB_DIM); con_puts("...(truncated)\n"); con_setcol(FOREB_TEXT); break; }
    }
    con_setcol(FOREB_DIM); con_puti(count); con_puts(" variable(s)\n"); con_setcol(FOREB_TEXT);
}

static void print_boot_desc(const char *name)   /* GetVariable Boot#### -> description */
{
    CHAR16 wn[16]; s_a2u(name, wn, 16);
    UINT32 attr = 0; UINTN dsz = 0;
    EFI_STATUS st = sRT->GetVariable(wn, &gGlobalVarGuid, &attr, &dsz, NULL);
    if (st != EFI_BUFFER_TOO_SMALL || dsz < 6) return;
    VOID *d = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, dsz, &d)) || !d) return;
    if (!EFI_ERROR(sRT->GetVariable(wn, &gGlobalVarGuid, &attr, &dsz, d))) {
        /* EFI_LOAD_OPTION: u32 Attributes, u16 FilePathListLength, CHAR16 Description[] */
        const CHAR16 *desc = (const CHAR16 *)((const UINT8 *)d + 6);
        char a[128]; s_u2a(desc, a, sizeof(a));
        con_puts("  "); con_setcol(FOREB_TEXT); con_puts(a);
    }
    sBS->FreePool(d);
}

static void cmd_bootvars(void)
{
    /* BootCurrent / BootNext / BootOrder first. */
    struct { const char *nm; } fixed[] = { {"BootCurrent"}, {"BootNext"}, {"Timeout"} };
    for (int k = 0; k < 3; k++) {
        CHAR16 wn[16]; s_a2u(fixed[k].nm, wn, 16);
        UINT16 val = 0; UINT32 attr = 0; UINTN dsz = sizeof(val);
        if (!EFI_ERROR(sRT->GetVariable(wn, &gGlobalVarGuid, &attr, &dsz, &val)) && dsz >= 2) {
            con_setcol(FOREB_TITLE); con_puts(fixed[k].nm); con_setcol(FOREB_TEXT);
            con_puts(" = "); con_puthex(val, 4); con_putc('\n');
        }
    }
    /* BootOrder (array of u16). */
    {
        CHAR16 wn[16]; s_a2u("BootOrder", wn, 16);
        UINT32 attr = 0; UINTN dsz = 0;
        if (sRT->GetVariable(wn, &gGlobalVarGuid, &attr, &dsz, NULL) == EFI_BUFFER_TOO_SMALL && dsz >= 2) {
            VOID *d = NULL;
            if (!EFI_ERROR(sBS->AllocatePool(EfiLoaderData, dsz, &d)) && d) {
                /* Free on BOTH the success and GetVariable-failure paths - the
                 * old compound-if short-circuited FreePool on read failure. */
                if (!EFI_ERROR(sRT->GetVariable(wn, &gGlobalVarGuid, &attr, &dsz, d))) {
                    con_setcol(FOREB_TITLE); con_puts("BootOrder"); con_setcol(FOREB_TEXT); con_puts(" = ");
                    const UINT16 *o = (const UINT16 *)d;
                    for (UINTN i = 0; i < dsz / 2; i++) { con_puthex(o[i], 4); con_putc(' '); }
                    con_putc('\n');
                }
                sBS->FreePool(d);
            }
        }
    }
    /* Enumerate every global Boot#### option with its description. */
    CHAR16 name[512]; EFI_GUID g; name[0] = 0;
    int n = 0;
    for (;;) {
        UINTN nsz = sizeof(name);
        EFI_STATUS st = sRT->GetNextVariableName(&nsz, name, &g);
        if (st == EFI_NOT_FOUND || EFI_ERROR(st)) break;
        if (!s_guid_eq(&g, &gGlobalVarGuid)) continue;
        char a[64]; s_u2a(name, a, sizeof(a));
        if (!is_boot_option_name(a)) continue;
        con_setcol(FOREB_TITLE); con_puts(a); con_setcol(FOREB_TEXT);
        print_boot_desc(a);
        con_putc('\n');
        if (++n >= 128) break;
    }
    if (n == 0) { con_setcol(FOREB_DIM); con_puts("(no Boot#### options)\n"); con_setcol(FOREB_TEXT); }
}

static void cmd_getvar(int argc, char **argv)
{
    if (argc < 2) { con_setcol(FOREB_TIMER); con_puts("usage: getvar <name> [guid]\n"); con_setcol(FOREB_TEXT); return; }
    EFI_GUID g = gGlobalVarGuid;
    if (argc >= 3 && !s_parse_guid(argv[2], &g)) {
        con_setcol(FOREB_TIMER); con_puts("getvar: bad guid\n"); con_setcol(FOREB_TEXT); return;
    }
    CHAR16 wn[128]; s_a2u(argv[1], wn, 128);
    UINT32 attr = 0; UINTN dsz = 0;
    EFI_STATUS st = sRT->GetVariable(wn, &g, &attr, &dsz, NULL);
    if (st != EFI_BUFFER_TOO_SMALL) {
        con_setcol(FOREB_TIMER); con_puts("getvar: not found ("); con_puthex(st, 16); con_puts(")\n");
        con_setcol(FOREB_TEXT); return;
    }
    VOID *d = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, dsz ? dsz : 1, &d)) || !d) return;
    if (EFI_ERROR(sRT->GetVariable(wn, &g, &attr, &dsz, d))) { sBS->FreePool(d); return; }
    con_setcol(FOREB_TITLE); con_puts(argv[1]); con_setcol(FOREB_TEXT);
    con_puts("  attr="); con_puthex(attr, 8); con_puts("  size="); con_putu(dsz); con_putc('\n');
    hexdump_bytes((const UINT8 *)d, dsz > 256 ? 256 : dsz, 0);
    sBS->FreePool(d);
}

static void cmd_setvar(int argc, char **argv)
{
    if (argc < 4) { con_setcol(FOREB_TIMER); con_puts("usage: setvar <name> <guid> <hex>\n"); con_setcol(FOREB_TEXT); return; }
    EFI_GUID g;
    if (!s_parse_guid(argv[2], &g)) { con_setcol(FOREB_TIMER); con_puts("setvar: bad guid\n"); con_setcol(FOREB_TEXT); return; }
    /* Parse hex nibbles into bytes (ignore separators). */
    UINT8 data[256]; UINTN dlen = 0;
    const char *h = argv[3]; int hi = -1;
    for (; *h; h++) {
        if (*h == '-' || *h == ':' || *h == ' ') continue;
        int v = s_hexval(*h);
        if (v < 0) { con_setcol(FOREB_TIMER); con_puts("setvar: bad hex\n"); con_setcol(FOREB_TEXT); return; }
        if (hi < 0) hi = v;
        else { if (dlen >= sizeof(data)) break; data[dlen++] = (UINT8)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) { con_setcol(FOREB_TIMER); con_puts("setvar: odd hex digit count\n"); con_setcol(FOREB_TEXT); return; }

    con_setcol(FOREB_TITLE); con_puts("setvar "); con_puts(argv[1]); con_setcol(FOREB_TEXT);
    con_puts("  {"); con_putguid(&g); con_puts("}  "); con_putu(dlen); con_puts(" byte(s)\n");

    CHAR16 wn[128]; s_a2u(argv[1], wn, 128);
    UINT32 attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    EFI_STATUS st = sRT->SetVariable(wn, &g, attr, dlen, data);
    if (EFI_ERROR(st)) { con_setcol(FOREB_TIMER); con_puts("setvar failed ("); con_puthex(st, 16); con_puts(")\n"); }
    else               { con_setcol(FOREB_TITLE); con_puts("setvar ok\n"); }
    con_setcol(FOREB_TEXT);
}

static void cmd_background(int argc, char **argv)
{
    if (argc < 2) { con_setcol(FOREB_TIMER); con_puts("usage: background <file>\n"); con_setcol(FOREB_TEXT); return; }
    if (!sCfg) { con_setcol(FOREB_TIMER); con_puts("background: no config loaded\n"); con_setcol(FOREB_TEXT); return; }
    /* Validate the file is present/readable, then record it in the live config.
     * The menu picks it up on the next repaint / boot. */
    EFI_FILE_PROTOCOL *f = NULL;
    if (EFI_ERROR(esp_open(argv[1], EFI_FILE_MODE_READ, &f)) || !f) {
        con_setcol(FOREB_TIMER); con_puts("background: cannot open '"); con_puts(argv[1]); con_puts("'\n");
        con_setcol(FOREB_TEXT); return;
    }
    f->Close(f);
    s_strcpy(sCfg->background, argv[1], FOREB_CFG_PATH_LEN);
    if (sSel >= 0 && sSel < sCfg->count)
        s_strcpy(sCfg->entries[sSel].background, argv[1], FOREB_CFG_PATH_LEN);
    con_setcol(FOREB_TITLE); con_puts("background set: "); con_puts(argv[1]);
    con_setcol(FOREB_DIM); con_puts("  (applies on next menu/boot)\n"); con_setcol(FOREB_TEXT);
}

static void cmd_memmap(void)
{
    UINTN mapsz = 0, mapkey = 0, dsz = 0; UINT32 dver = 0;
    EFI_STATUS st = sBS->GetMemoryMap(&mapsz, NULL, &mapkey, &dsz, &dver);
    if (st != EFI_BUFFER_TOO_SMALL || dsz == 0) {
        con_setcol(FOREB_TIMER); con_puts("memmap: GetMemoryMap failed\n"); con_setcol(FOREB_TEXT); return;
    }
    mapsz += 8 * dsz;   /* headroom for the AllocatePool below */
    VOID *map = NULL;
    if (EFI_ERROR(sBS->AllocatePool(EfiLoaderData, mapsz, &map)) || !map) {
        con_setcol(FOREB_TIMER); con_puts("memmap: out of memory\n"); con_setcol(FOREB_TEXT); return;
    }
    st = sBS->GetMemoryMap(&mapsz, (EFI_MEMORY_DESCRIPTOR *)map, &mapkey, &dsz, &dver);
    if (EFI_ERROR(st)) {
        con_setcol(FOREB_TIMER); con_puts("memmap: GetMemoryMap failed\n"); con_setcol(FOREB_TEXT);
        sBS->FreePool(map); return;
    }
    UINTN count = mapsz / dsz;
    UINT64 total_pages = 0, usable_pages = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * dsz);
        total_pages += d->NumberOfPages;
        switch (d->Type) {
            case EfiConventionalMemory:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiLoaderCode:
            case EfiLoaderData:
                usable_pages += d->NumberOfPages;
                break;
            default: break;
        }
    }
    con_setcol(FOREB_TITLE); con_puts("EFI memory map"); con_setcol(FOREB_TEXT); con_putc('\n');
    con_puts("  descriptors : "); con_putu(count); con_putc('\n');
    con_puts("  total RAM   : "); con_putu((total_pages * 4096) >> 20); con_puts(" MiB\n");
    con_puts("  usable      : "); con_putu((usable_pages * 4096) >> 20); con_puts(" MiB\n");
    sBS->FreePool(map);
}

static void cmd_config(void)
{
    if (!sCfg) { con_setcol(FOREB_TIMER); con_puts("config: no config buffer\n"); con_setcol(FOREB_TEXT); return; }
    VOID *buf = NULL; UINTN sz = 0;
    EFI_STATUS st = esp_load(FOREB_CFG_ESP_PATH, 64u * 1024u, &buf, &sz);
    if (EFI_ERROR(st) || !buf) {
        con_setcol(FOREB_TIMER); con_puts("config: cannot read " FOREB_CFG_ESP_PATH "\n"); con_setcol(FOREB_TEXT);
        if (buf) sBS->FreePool(buf);
        return;
    }
    forebo_cfg_init(sCfg);
    int n = forebo_cfg_parse(sCfg, (const char *)buf, (unsigned long)sz);
    sBS->FreePool(buf);
    con_setcol(FOREB_TITLE); con_puts("config reloaded: "); con_puti(n); con_puts(" entr(y/ies), ");
    con_puts("default="); con_puti(sCfg->default_idx); con_puts(", timeout="); con_puti(sCfg->timeout);
    con_putc('\n'); con_setcol(FOREB_TEXT);
    if (sSel >= sCfg->count) sSel = sCfg->default_idx;
}

/* Resolve a `boot` argument (index or title match) to an entry index, or -1. */
static int resolve_boot_arg(int argc, char **argv)
{
    if (!sCfg || sCfg->count <= 0) return -1;
    if (argc < 2) {
        int e = sSel;
        if (e < 0 || e >= sCfg->count) e = sCfg->default_idx;
        if (e < 0 || e >= sCfg->count) e = 0;
        return e;
    }
    UINT64 idx;
    if (s_parse_u64(argv[1], &idx) && (int)idx < sCfg->count) return (int)idx;
    /* title match (case-insensitive exact) */
    for (int i = 0; i < sCfg->count; i++)
        if (s_ci_eq(sCfg->entries[i].title, argv[1])) return i;
    return -1;
}

/* =============================================================================
 * Command dispatch. Sets *ret (a shell_run return code) and returns 1 if the
 * shell should exit; returns 0 to keep looping.
 * ==========================================================================*/
/* -------- setup / firmware (reboot into the UEFI/BIOS setup screen) -------- */
static void cmd_setup(void)
{
    if (!fw_setup_supported(sRT)) {
        con_setcol(FOREB_TIMER);
        con_puts("firmware setup not supported: this firmware does not advertise\n");
        con_puts("the OsIndications BOOT_TO_FW_UI bit. Reboot and enter setup manually.\n");
        con_setcol(FOREB_TEXT);
        return;
    }
    con_setcol(FOREB_TITLE);
    con_puts("Rebooting into firmware setup...\n");
    con_setcol(FOREB_TEXT);
    ui_present();
    fw_boot_to_setup(sRT);            /* does not return on success */
    con_setcol(FOREB_TIMER);
    con_puts("firmware setup request failed (SetVariable/ResetSystem error)\n");
    con_setcol(FOREB_TEXT);
}

/* -------- tools (open the windowed GUI Tools launcher) --------------------- */
static void cmd_tools(void)
{
    tools_launcher_open();
    con_setcol(FOREB_DIM);
    con_puts("GUI Tools launcher opened. Type 'exit' (or Esc) to return to the\n");
    con_puts("boot menu, where the compositor draws and drives the tool windows.\n");
    con_setcol(FOREB_TEXT);
}

static int dispatch(int argc, char **argv, int *ret)
{
    const char *c = argv[0];

    if (s_ci_eq(c, "help") || s_eq(c, "?"))         cmd_help(argc, argv);
    else if (s_ci_eq(c, "clear") || s_ci_eq(c, "cls")) { con_init(); }
    else if (s_ci_eq(c, "cd") || s_ci_eq(c, "chdir")) cmd_cd(argc, argv);
    else if (s_ci_eq(c, "pwd"))                     cmd_pwd();
    else if (s_ci_eq(c, "ls") || s_ci_eq(c, "dir")) cmd_ls(argc, argv);
    else if (s_ci_eq(c, "cat") || s_ci_eq(c, "type")) cmd_cat(argc, argv);
    else if (s_ci_eq(c, "hexdump") || s_ci_eq(c, "xxd")) cmd_hexdump(argc, argv);
    else if (s_ci_eq(c, "lsblk"))                   cmd_lsblk();
    else if (s_ci_eq(c, "read"))                    cmd_read(argc, argv);
    else if (s_ci_eq(c, "write"))                   cmd_write(argc, argv);
    else if (s_ci_eq(c, "drives"))                  cmd_drives();
    else if (s_ci_eq(c, "devices") || s_ci_eq(c, "lsdev") || s_ci_eq(c, "hw")) cmd_devices();
    else if (s_ci_eq(c, "inputtest") || s_ci_eq(c, "testinput")) cmd_inputtest();
    else if (s_ci_eq(c, "modules"))                 cmd_modules(argc, argv);
    else if (s_ci_eq(c, "efivars"))                 cmd_efivars();
    else if (s_ci_eq(c, "bootvars"))                cmd_bootvars();
    else if (s_ci_eq(c, "getvar"))                  cmd_getvar(argc, argv);
    else if (s_ci_eq(c, "setvar"))                  cmd_setvar(argc, argv);
    else if (s_ci_eq(c, "background") || s_ci_eq(c, "bg")) cmd_background(argc, argv);
    else if (s_ci_eq(c, "memmap"))                  cmd_memmap();
    else if (s_ci_eq(c, "config") || s_ci_eq(c, "reload")) cmd_config();
    else if (s_ci_eq(c, "gpt"))                     cmd_gpt(argc, argv);
    else if (s_ci_eq(c, "parts") || s_ci_eq(c, "partitions")) cmd_parts();
    else if (s_ci_eq(c, "fsprobe"))                 cmd_fsprobe(argc, argv);
    else if (s_ci_eq(c, "rescue"))                  cmd_rescue(argc, argv);
    else if (s_ci_eq(c, "fatfix"))                  cmd_fatfix(argc, argv);
    else if (s_ci_eq(c, "scan") || s_ci_eq(c, "carve")) cmd_scan(argc, argv);
    else if (s_ci_eq(c, "ext-ls")  || s_ci_eq(c, "extls"))  cmd_extls(argc, argv);
    else if (s_ci_eq(c, "ext-cat") || s_ci_eq(c, "extcat")) cmd_extcat(argc, argv);
    else if (s_ci_eq(c, "btrfs-snaps") || s_ci_eq(c, "btrfs")) cmd_btrfssnaps(argc, argv);
    else if (s_ci_eq(c, "boot")) {
        int e = resolve_boot_arg(argc, argv);
        if (e < 0) { con_setcol(FOREB_TIMER); con_puts("boot: no such entry\n"); con_setcol(FOREB_TEXT); }
        else { *ret = e; return 1; }
    }
    else if (s_ci_eq(c, "setup") || s_ci_eq(c, "firmware")) cmd_setup();
    else if (s_ci_eq(c, "tools"))                   cmd_tools();
    else if (s_ci_eq(c, "reboot") || s_ci_eq(c, "reset")) { *ret = FOREB_SHELL_REBOOT; return 1; }
    else if (s_ci_eq(c, "exit") || s_ci_eq(c, "quit") || s_ci_eq(c, "menu")) { *ret = FOREB_SHELL_BACK; return 1; }
    else {
        con_setcol(FOREB_TIMER); con_puts("unknown command: "); con_puts(c);
        con_setcol(FOREB_DIM); con_puts("  (try 'help')\n"); con_setcol(FOREB_TEXT);
    }
    return 0;
}

/* Split a mutable line into argv on whitespace. Returns argc. */
static int tokenize(char *line, char **argv, int maxv)
{
    int argc = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        if (argc < maxv) argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return argc;
}

/* =============================================================================
 * Public entry point.
 * ==========================================================================*/
int shell_run(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
              struct forebo_config *cfg, int cur_sel)
{
    if (!image || !st || !st->BootServices || !st->RuntimeServices)
        return FOREB_SHELL_BACK;

    sImage = image; sCfg = cfg; sSel = cur_sel;
    sST = st;
    sBS = st->BootServices;
    sRT = st->RuntimeServices;
    sIn = st->ConIn;

    /* The ESP is reached through the loader image's DeviceHandle. */
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    if (!EFI_ERROR(sBS->HandleProtocol(image, &gLoadedImgGuid, (VOID **)&li)) && li)
        sDev = li->DeviceHandle;

    /* Drain buffered keystrokes. */
    if (sIn) { EFI_INPUT_KEY k; while (!EFI_ERROR(sIn->ReadKeyStroke(sIn, &k))) { } }

    con_init();
    con_setcol(FOREB_TITLE);
    con_puts("ForeB interactive shell"); con_putc('\n');
    con_setcol(FOREB_DIM);
    con_puts("type 'help' for commands, 'exit' (or Esc) to return to the menu"); con_putc('\n');
    con_setcol(FOREB_TEXT);
    con_putc('\n');

    char line[COLW];
    char *argv[16];
    for (;;) {
        char prompt[FOREB_CFG_PATH_LEN + 16];
        int pi = 0;
        for (const char *q = "forb:"; *q; q++) prompt[pi++] = *q;
        for (const char *q = s_cwd; *q && pi < (int)sizeof(prompt) - 3; q++) prompt[pi++] = *q;
        prompt[pi++] = '>'; prompt[pi++] = ' '; prompt[pi] = 0;
        int r = read_line(prompt, line, sizeof(line));
        if (r < 0) return FOREB_SHELL_BACK;           /* Esc */
        int argc = tokenize(line, argv, 16);
        if (argc == 0) continue;                       /* empty line */
        int ret = FOREB_SHELL_BACK;
        if (dispatch(argc, argv, &ret)) return ret;
    }
}

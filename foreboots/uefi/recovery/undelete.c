/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/undelete.c - advanced GUI file-recovery tool (implements undelete.h).
 * =============================================================================
 * Template B window (see tools.h): tool_undelete_open() calls wm_open() and
 * returns; the menu loop drives draw/event. State lives in a single static
 * udstate reached from the callbacks via wm_user().
 *
 * Layout: a SPLIT client rect - LEFT navigation pane, vertical divider, RIGHT
 * preview pane, a full-width breadcrumb header on top and a button bar +status
 * line on the bottom.
 *
 * BROWSE mode walks a real filesystem's directory tree of EXISTING files:
 *   ext2/3/4 via fs_ext (ext_mount/ext_ls/ext_read), FAT/ESP via the firmware
 *   EFI_SIMPLE_FILE_SYSTEM + EFI_FILE, btrfs via fs_btrfs (flat subvolume list).
 *   Unknown filesystems are reported ("unrecognized FS") and skipped.
 *
 * CARVE mode signature-scans the selected raw device in fixed SCAN_WIN windows
 *   through diskio_read_bytes() (ddrescue-style: bad sectors zero-filled, scan
 *   continues), matching JPEG/PNG/PDF/ZIP/GIF/BMP/TGA and recording bounded
 *   extents. Scanning advances a few windows per draw frame so the UI stays live.
 *
 * PREVIEW decodes a selected BMP/TGA with image.c into a fit-to-pane thumbnail,
 *   else shows a bounded hex+ASCII dump of the first bytes. All preview reads
 *   are capped at UD_PREVBUF so a whole disk/file never lives in RAM.
 *
 * RECOVER copies the selected real file OR carved extent to
 *   \forebo\recovered\NNNN.<ext> on the ESP. READ-ONLY on the source device.
 * ========================================================================== */

#include "undelete.h"
#include "efi.h"
#include "efi_ext.h"
#include "wm.h"
#include "ui.h"
#include "input.h"
#include "diskio.h"
#include "config.h"
#include "image.h"
#include "fs_ext.h"
#include "fs_btrfs.h"
#include "../include/forebo_cfg.h"
#include "../include/forebo_theme.h"

/* ==========================================================================
 * Cached services (this codebase has no ambient gST; each module caches it).
 * ========================================================================== */
static EFI_HANDLE         gImage;
static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;

/* Local protocol GUID (efi.h leaves the named globals behind a define). */
static EFI_GUID gSfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

void tool_undelete_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    gImage = image;
    gST    = st;
    gBS    = st ? st->BootServices : 0;
    diskio_init(st);
}

/* ==========================================================================
 * Tunables (all scan/preview buffers fixed-size).
 * ========================================================================== */
#define UD_MAX_DEV     16                 /* enumerated devices shown          */
#define UD_MAX_ITEMS   256                /* carved items retained             */
#define UD_MAX_ENT     512                /* directory entries listed per dir  */
#define UD_NAME        96                 /* max stored entry name             */
#define UD_PATH        320                /* max stored path                   */
#define UD_COLS        128                /* chars per rendered line           */
#define SCAN_WIN       (128u*1024u)       /* raw bytes read per scan window    */
#define UD_CARRY       16                 /* boundary-straddle carry (>= maxhdr)*/
#define UD_CAP         (32u*1024u*1024u)  /* per-file carve extent cap (32 MiB)*/
#define UD_SCAN_BUDGET (2ULL*1024*1024*1024) /* max bytes scanned per device   */
#define UD_RECBUF      (64u*1024u)        /* recovery copy chunk               */
#define UD_WINS_TICK   3                  /* scan windows processed per frame  */
#define UD_PREVBUF     (2u*1024u*1024u)   /* preview read cap (2 MiB)          */
#define UD_FSREC_CAP   (32u*1024u*1024u)  /* max real-file recovery size       */
#define UD_HEXSHOW     256                /* bytes kept for the hex preview    */

/* Scratch: one scan window (+carry), one recovery chunk, one preview window. */
static UINT8 g_scanbuf[UD_CARRY + SCAN_WIN];
static UINT8 g_recbuf[UD_RECBUF];
static UINT8 g_prevbuf[UD_PREVBUF];

/* ==========================================================================
 * Signature table. hdr = magic bytes; ftr = terminator (NULL => header-only,
 * extent capped at UD_CAP). fmin = minimum plausible size before a footer.
 * ========================================================================== */
enum { T_JPEG=0, T_PNG, T_PDF, T_ZIP, T_GIF, T_BMP, T_TGA, T_N };

static const UINT8 SIG_JPEG_H[] = {0xFF,0xD8,0xFF};
static const UINT8 SIG_JPEG_F[] = {0xFF,0xD9};
static const UINT8 SIG_PNG_H[]  = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
static const UINT8 SIG_PNG_F[]  = {0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82}; /* IEND+CRC */
static const UINT8 SIG_PDF_H[]  = {0x25,0x50,0x44,0x46};                     /* %PDF */
static const UINT8 SIG_PDF_F[]  = {0x25,0x25,0x45,0x4F,0x46};                /* %%EOF */
static const UINT8 SIG_ZIP_H[]  = {0x50,0x4B,0x03,0x04};                     /* PK.. */
static const UINT8 SIG_GIF_H[]  = {0x47,0x49,0x46,0x38};                     /* GIF8 */
static const UINT8 SIG_BMP_H[]  = {0x42,0x4D};                               /* BM   */
static const UINT8 SIG_TGA_H[]  = {0x00,0x00,0x02,0x00};                     /* TGA type2 hdr slice */

static const struct udsig {
    const UINT8 *hdr; int hlen;
    const UINT8 *ftr; int flen;
    unsigned     fmin;                 /* min size for footer search           */
    const char  *ext;
    const char  *name;
    int          decodable;            /* 1 if image.c can decode it           */
} g_sig[T_N] = {
    { SIG_JPEG_H, 3, SIG_JPEG_F, 2,  4,  "jpg", "JPEG", 0 },
    { SIG_PNG_H,  8, SIG_PNG_F,  8, 16,  "png", "PNG",  0 },
    { SIG_PDF_H,  4, SIG_PDF_F,  5,  8,  "pdf", "PDF",  0 },
    { SIG_ZIP_H,  4, 0,          0,  0,  "zip", "ZIP",  0 },
    { SIG_GIF_H,  4, 0,          0,  0,  "gif", "GIF",  0 },
    { SIG_BMP_H,  2, 0,          0,  0,  "bmp", "BMP",  1 },
    { SIG_TGA_H,  4, 0,          0,  0,  "tga", "TGA",  1 },
};

/* ==========================================================================
 * State.
 * ========================================================================== */
enum { NAV_DEVICES=0, NAV_FS, NAV_CARVE };  /* what the left pane shows        */
enum { MODE_BROWSE=0, MODE_CARVE };          /* Tab toggles this               */
enum { FS_NONE=0, FS_EXT, FS_FAT, FS_BTRFS };

/* Preview classification. */
enum { PK_NONE=0, PK_DEVICE, PK_DIR, PK_SUBVOL, PK_FILE, PK_CARVE };

typedef struct {
    char   name[UD_NAME];
    UINT8  isdir;
    UINT8  subvol;       /* btrfs subvolume/snapshot (flat, non-descendable)   */
    UINT8  dotdot;       /* the synthetic ".." row                             */
    UINT64 size;
} ud_ent;

typedef struct {
    wm_window *win;                 /* NULL when closed (idempotent open)      */
    int        avail;               /* 1 if services present                   */
    int        mode;                /* MODE_BROWSE / MODE_CARVE                 */
    int        level;              /* NAV_DEVICES / NAV_FS / NAV_CARVE         */

    struct diskio_dev dev[UD_MAX_DEV];
    int        ndev;
    int        cur_dev;             /* device we descended into                */

    /* mounted filesystem (BROWSE) */
    int                fstype;      /* FS_*                                    */
    ext_ctx           *ext;         /* ext mount ctx (NULL unless FS_EXT)      */
    EFI_FILE_PROTOCOL *fatroot;     /* FAT volume root (NULL unless FS_FAT)    */
    char               path[UD_PATH]; /* current dir, canonical '/'-separated  */

    /* current directory listing */
    ud_ent     ent[UD_MAX_ENT];
    int        nent;

    /* carve scan */
    struct { UINT64 off; UINT64 size; int type; int saved; } item[UD_MAX_ITEMS];
    int        nitems;
    UINT64     scan_off, scan_end;
    int        carrylen, scanning;
    UINT64     bad_blocks;

    int        sel, scroll;         /* selection in the LEFT pane              */

    /* preview cache (rebuilt when the selection changes) */
    int              prev_valid;
    int              prev_pending;  /* sel seen last frame (debounce heavy build) */
    int              prev_sel, prev_level, prev_mode;
    int              prev_kind;     /* PK_*                                    */
    int              prev_is_img;
    struct img_image prev_img;
    UINT64           prev_size;
    int              prev_hexlen;
    int              prev_whole;    /* whole file/extent fit in UD_PREVBUF     */
    UINT8            prev_hex[UD_HEXSHOW];
    char             prev_name[UD_NAME];
    char             prev_note[64];

    int        b_hover, b_press;
    wm_button  btn_cache[8];        /* laid-out button bar, rebuilt each ud_draw */
    int        nbtn_cache;          /* valid entries in btn_cache                */
    unsigned   saved_seq;
    char       msg[96];
} udstate;

static udstate g_ud;

/* Resolved theme colours (from the WM's adopted theme). */
static UINT32 c_win, c_fg, c_dim, c_accent, c_sel_bg, c_sel_fg, c_border, c_warn;

static void resolve_theme(void)
{
    c_win    = wm_theme_color(WM_COL_WINDOW);
    c_fg     = wm_theme_color(WM_COL_FG);
    c_accent = wm_theme_color(WM_COL_ACCENT);
    c_sel_bg = wm_theme_color(WM_COL_SEL_BG);
    c_sel_fg = wm_theme_color(WM_COL_SEL_FG);
    c_dim    = wm_blend(c_win, c_fg, 128);
    c_border = wm_blend(c_win, c_fg, 64);
    c_warn   = FOREB_TIMER;
}

/* ==========================================================================
 * Tiny freestanding helpers.
 * ========================================================================== */
static int  gsc(void){ int s=ui_scale(); return s<1?1:s; }
static int  ud_slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }
static void ud_scopy(char *d, const char *s, int cap)
{ int i=0; if(cap<=0)return; for(;s&&s[i]&&i+1<cap;i++)d[i]=s[i]; d[i]=0; }
static int  ud_meq(const UINT8 *a, const UINT8 *b, int n)
{ for(int i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static void u2a(const CHAR16 *u, char *a, int cap)
{ int i=0; if(cap<=0)return; for(;u&&u[i]&&i+1<cap;i++) a[i]=(char)(u[i]&0x7F); a[i]=0; }

/* Append helpers into a fixed line buffer (p is the running length). */
static void ap_c(char *o, int *p, char c){ if(*p<UD_COLS-1) o[(*p)++]=c; }
static void ap_s(char *o, int *p, const char *s){ while(s&&*s) ap_c(o,p,*s++); }
static void ap_u(char *o, int *p, UINT64 v)
{ char t[24]; int i=0; if(!v){ap_c(o,p,'0');return;} while(v){t[i++]=(char)('0'+(int)(v%10));v/=10;} while(i)ap_c(o,p,t[--i]); }
static void ap_x(char *o, int *p, UINT64 v)
{ static const char h[]="0123456789ABCDEF"; char t[16]; int i=0;
  if(!v){t[i++]='0';} while(v){t[i++]=h[v&0xF];v>>=4;} while(i)ap_c(o,p,t[--i]); }
static void ap_u4(char *o, int *p, unsigned v)   /* zero-padded 4 digits */
{ char t[8]; int i=0; for(int k=0;k<4;k++){ t[i++]=(char)('0'+(int)(v%10)); v/=10; } while(i)ap_c(o,p,t[--i]); }
static void ap_sz(char *o, int *p, UINT64 b)     /* human-ish size */
{ if(b>>30){ ap_u(o,p,b>>30); ap_s(o,p," GB"); }
  else if(b>>20){ ap_u(o,p,b>>20); ap_s(o,p," MB"); }
  else if(b>>10){ ap_u(o,p,b>>10); ap_s(o,p," KB"); }
  else { ap_u(o,p,b); ap_s(o,p," B"); } }

/* ==========================================================================
 * Path helpers (canonical '/'-separated; root == "/").
 * ========================================================================== */
static int  path_is_root(const char *p){ return p[0]=='/' && p[1]==0; }
static void path_join(const char *base, const char *name, char *out, int cap)
{
    int p=0;
    for(const char *s=base; *s && p<cap-1; s++) out[p++]=*s;
    if(!(p==1 && out[0]=='/')){ if(p<cap-1) out[p++]='/'; }
    for(const char *s=name; *s && p<cap-1; s++) out[p++]=*s;
    out[p]=0;
}
static void path_up(char *path)
{
    int i=ud_slen(path);
    if(i<=1){ path[0]='/'; path[1]=0; return; }
    if(path[i-1]=='/') i--;
    while(i>0 && path[i-1]!='/') i--;
    if(i<=1){ path[0]='/'; path[1]=0; return; }
    path[i-1]=0;
}
/* file extension (without dot) from an ASCII name, or "bin". */
static const char *name_ext(const char *name)
{
    int n=ud_slen(name), dot=-1;
    for(int i=0;i<n;i++) if(name[i]=='.') dot=i;
    if(dot<0 || dot+1>=n) return "bin";
    return name+dot+1;
}

/* ==========================================================================
 * Directory listing (BROWSE).
 * ========================================================================== */
static void ud_add_ent(udstate *u, const char *name, int isdir, int subvol, int dotdot, UINT64 size)
{
    if(u->nent >= UD_MAX_ENT) return;
    ud_ent *e = &u->ent[u->nent++];
    ud_scopy(e->name, name, UD_NAME);
    e->isdir=(UINT8)(isdir?1:0);
    e->subvol=(UINT8)(subvol?1:0);
    e->dotdot=(UINT8)(dotdot?1:0);
    e->size=size;
}

static void ext_ls_cb(const char *name, uint32_t inode, uint8_t file_type, void *user)
{
    (void)inode;
    udstate *u=(udstate*)user;
    if(!name || !name[0]) return;
    if(name[0]=='.' && name[1]==0) return;
    if(name[0]=='.' && name[1]=='.' && name[2]==0) return;
    ud_add_ent(u, name, file_type==EXT_FT_DIR, 0, 0, 0);
}

static void ud_btrfs_cb(const char *name, uint64_t subvol_id, uint64_t parent_id, void *user)
{
    (void)subvol_id; (void)parent_id;
    udstate *u=(udstate*)user;
    if(!name || !name[0]) return;
    ud_add_ent(u, name, 0, 1, 0, 0);
}

/* Rebuild u->ent[] for the current fstype + path. */
static void ud_fs_list(udstate *u)
{
    u->nent=0;
    int isroot = path_is_root(u->path);
    if(!isroot && u->fstype!=FS_BTRFS)
        ud_add_ent(u, "..", 1, 0, 1, 0);

    if(u->fstype==FS_EXT && u->ext){
        ext_ls(u->ext, u->path, ext_ls_cb, u);
    } else if(u->fstype==FS_FAT && u->fatroot){
        EFI_FILE_PROTOCOL *dir=u->fatroot, *opened=0;
        if(!isroot){
            CHAR16 wp[UD_PATH]; esp_ascii_to_char16(u->path, wp, UD_PATH);
            if(!EFI_ERROR(u->fatroot->Open(u->fatroot,&opened,wp,EFI_FILE_MODE_READ,0)) && opened)
                dir=opened;
        }
        if(dir){
            for(;;){
                UINT8 ib[1024]; UINTN isz=sizeof(ib);
                EFI_STATUS st=dir->Read(dir,&isz,ib);
                if(EFI_ERROR(st) || isz==0) break;
                EFI_FILE_INFO *fi=(EFI_FILE_INFO*)ib;
                char nm[UD_NAME]; u2a(fi->FileName,nm,UD_NAME);
                if(nm[0]=='.' && nm[1]==0) continue;
                if(nm[0]=='.' && nm[1]=='.' && nm[2]==0) continue;
                ud_add_ent(u, nm, (fi->Attribute&EFI_FILE_DIRECTORY)?1:0, 0, 0, fi->FileSize);
                if(u->nent>=UD_MAX_ENT) break;
            }
        }
        if(opened) opened->Close(opened);
    } else if(u->fstype==FS_BTRFS){
        struct diskio_dev *d=&u->dev[u->cur_dev];
        btrfs_list_snapshots(gBS, d->bio, d->dio, ud_btrfs_cb, u);
    }

    if(u->sel>=u->nent) u->sel=u->nent-1;
    if(u->sel<0) u->sel=0;
    u->scroll=0;
}

/* ==========================================================================
 * Mount / unmount a device for BROWSE.
 * ========================================================================== */
static void ud_unmount(udstate *u)
{
    if(u->ext){ ext_unmount(u->ext); u->ext=0; }
    if(u->fatroot){ u->fatroot->Close(u->fatroot); u->fatroot=0; }
    u->fstype=FS_NONE;
}

/* Detect + mount the fs on dev[idx]. Returns 1 on success (fstype set). */
static int ud_mount(udstate *u, int idx)
{
    ud_unmount(u);
    if(idx<0 || idx>=u->ndev || !gBS) return 0;
    struct diskio_dev *d=&u->dev[idx];
    if(!d->bio) return 0;

    if(ext_probe(gBS, d->bio)){
        u->ext = ext_mount(gBS, d->bio, d->dio);
        if(u->ext){ u->fstype=FS_EXT; return 1; }
    }
    if(btrfs_probe(gBS, d->bio)){
        u->fstype=FS_BTRFS; return 1;
    }
    /* FAT / firmware-mountable volume */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs=0; EFI_FILE_PROTOCOL *root=0;
    if(d->handle &&
       !EFI_ERROR(gBS->HandleProtocol(d->handle,&gSfsGuid,(VOID**)&sfs)) && sfs &&
       !EFI_ERROR(sfs->OpenVolume(sfs,&root)) && root){
        u->fatroot=root; u->fstype=FS_FAT; return 1;
    }
    u->fstype=FS_NONE;
    return 0;
}

/* ==========================================================================
 * Carving core (windowed, bounded RAM).
 * ========================================================================== */
static int find_pat(const UINT8 *buf, int from, int lim, const UINT8 *pat, int patlen)
{
    if(patlen<=0) return -1;
    for(int i=from; i+patlen<=lim; i++)
        if(buf[i]==pat[0] && ud_meq(buf+i, pat, patlen)) return i;
    return -1;
}

static UINT64 carve_size(const udstate *u, int type, int p, int validlen, UINT64 base_abs)
{
    const struct udsig *s=&g_sig[type];
    UINT64 A=base_abs + (UINT64)p;
    UINT64 avail=(u->scan_end>A)?(u->scan_end-A):0;
    UINT64 cap=UD_CAP; if(cap>avail && avail) cap=avail; if(cap==0) cap=UD_CAP;

    if(s->ftr){
        int from=p + s->hlen + (int)s->fmin;
        int lim =validlen;
        int fp=find_pat(g_scanbuf, from, lim, s->ftr, s->flen);
        if(fp>=0){
            UINT64 end=base_abs + (UINT64)(fp + s->flen);
            UINT64 sz=end - A;
            if(sz>UD_CAP) sz=UD_CAP;
            if(sz<(UINT64)s->hlen) sz=s->hlen;
            return sz;
        }
    }
    return cap;
}

/* First-byte -> signature-type dispatch table (every g_sig[].hdr[0] is
 * distinct, so at most one type can ever match a given byte). Lets
 * carve_window() test each scanned byte once instead of re-checking all
 * T_N signatures' first byte per position - this loop runs over every byte
 * of every scan window (up to UD_SCAN_BUDGET total), so it is the hottest
 * code path in the carve tool. */
static signed char g_sig_by_byte[256];
static int         g_sig_by_byte_ready;
static void ensure_sig_by_byte(void)
{
    if(g_sig_by_byte_ready) return;
    for(int i=0;i<256;i++) g_sig_by_byte[i]=-1;
    for(int t=0;t<T_N;t++) g_sig_by_byte[g_sig[t].hdr[0]]=(signed char)t;
    g_sig_by_byte_ready=1;
}

static void carve_window(udstate *u, int validlen, UINT64 base_abs, int carry)
{
    ensure_sig_by_byte();
    for(int p=0; p<validlen; p++){
        int t=g_sig_by_byte[g_scanbuf[p]];
        if(t<0) continue;
        const struct udsig *s=&g_sig[t];
        if(p + s->hlen > validlen) continue;
        if(p + s->hlen <= carry) continue;               /* dedup carry */
        if(!ud_meq(g_scanbuf+p, s->hdr, s->hlen)) continue;
        if(u->nitems<UD_MAX_ITEMS){
            UINT64 sz=carve_size(u, t, p, validlen, base_abs);
            u->item[u->nitems].off  = base_abs + (UINT64)p;
            u->item[u->nitems].size = sz;
            u->item[u->nitems].type = t;
            u->item[u->nitems].saved= 0;
            u->nitems++;
        }
        p += s->hlen - 1;
    }
}

static int scan_one_window(udstate *u)
{
    if(u->cur_dev<0 || u->cur_dev>=u->ndev) return 0;
    struct diskio_dev *d=&u->dev[u->cur_dev];
    if(u->scan_off>=u->scan_end) return 0;

    int carry=u->carrylen;
    UINT64 want=u->scan_end - u->scan_off;
    if(want>SCAN_WIN) want=SCAN_WIN;
    if(want==0) return 0;

    struct diskio_read_stat st;
    int rc=diskio_read_bytes(d, u->scan_off, g_scanbuf+carry, (UINTN)want, &st);
    if(rc<0) return 0;
    u->bad_blocks += st.blocks_bad;

    int validlen=carry + (int)want;
    UINT64 base_abs=u->scan_off - (UINT64)carry;
    carve_window(u, validlen, base_abs, carry);

    u->scan_off += want;
    int ncarry=(validlen<UD_CARRY)?validlen:UD_CARRY;
    for(int i=0;i<ncarry;i++) g_scanbuf[i]=g_scanbuf[validlen-ncarry+i];
    u->carrylen=ncarry;

    return (u->scan_off<u->scan_end && u->nitems<UD_MAX_ITEMS);
}

static void scan_tick(udstate *u)
{
    if(u->level!=NAV_CARVE || !u->scanning) return;
    for(int i=0;i<UD_WINS_TICK;i++){
        if(!scan_one_window(u)){
            u->scanning=0;
            int p=0; u->msg[0]=0;
            ap_s(u->msg,&p,"Carve done: "); ap_u(u->msg,&p,(UINT64)u->nitems);
            ap_s(u->msg,&p," item(s), "); ap_u(u->msg,&p,u->bad_blocks);
            ap_s(u->msg,&p," bad block(s)"); u->msg[p]=0;
            if(u->sel>=u->nitems) u->sel=u->nitems-1;
            if(u->sel<0) u->sel=0;
            break;
        }
    }
}

static void start_carve(udstate *u)
{
    if(u->cur_dev<0 || u->cur_dev>=u->ndev) return;
    struct diskio_dev *d=&u->dev[u->cur_dev];
    u->nitems=0; u->sel=0; u->scroll=0;
    u->scan_off=0; u->carrylen=0; u->bad_blocks=0;
    u->scan_end=d->total_bytes;
    if(u->scan_end>UD_SCAN_BUDGET) u->scan_end=UD_SCAN_BUDGET;
    u->scanning=1;
    u->level=NAV_CARVE;
    u->msg[0]=0;
}

/* ==========================================================================
 * Reading source bytes for preview / recovery.
 * ========================================================================== */
/* Read up to `want` bytes of a BROWSE file at absolute canonical `full` into
 * buf. Returns bytes read (>=0) or -1 on error. */
static int64_t ud_read_fs(udstate *u, const char *full, UINT64 want, UINT8 *buf)
{
    if(u->fstype==FS_EXT && u->ext)
        return ext_read(u->ext, full, buf, want);
    if(u->fstype==FS_FAT && u->fatroot){
        CHAR16 wp[UD_PATH]; esp_ascii_to_char16(full, wp, UD_PATH);
        EFI_FILE_PROTOCOL *fh=0;
        if(EFI_ERROR(u->fatroot->Open(u->fatroot,&fh,wp,EFI_FILE_MODE_READ,0)) || !fh)
            return -1;
        UINTN sz=(UINTN)want;
        EFI_STATUS st=fh->Read(fh,&sz,buf);
        fh->Close(fh);
        if(EFI_ERROR(st)) return -1;
        return (int64_t)sz;
    }
    return -1;
}

/* ==========================================================================
 * Preview build (rebuilt when the left-pane selection changes).
 * ========================================================================== */
static void ud_invalidate(udstate *u){ u->prev_valid=0; }

static void ud_preview_reset(udstate *u)
{
    img_free(&u->prev_img);
    u->prev_is_img=0; u->prev_hexlen=0; u->prev_size=0; u->prev_whole=0;
    u->prev_kind=PK_NONE; u->prev_name[0]=0; u->prev_note[0]=0;
}

/* Try to decode the first `got` bytes in g_prevbuf as an image; fill hex. */
static void ud_preview_from_buf(udstate *u, int64_t got, int whole)
{
    if(got<0) got=0;
    u->prev_whole=whole;
    u->prev_hexlen = (got<UD_HEXSHOW)?(int)got:UD_HEXSHOW;
    for(int i=0;i<u->prev_hexlen;i++) u->prev_hex[i]=g_prevbuf[i];
    if(whole && got>0){
        if(img_decode(g_prevbuf, (UINTN)got, &u->prev_img)==IMG_OK && u->prev_img.pixels){
            u->prev_is_img=1;
        }
    }
    if(!u->prev_is_img){
        if(!whole) ud_scopy(u->prev_note, "large file - hex preview only", sizeof(u->prev_note));
        else       ud_scopy(u->prev_note, "not BMP/TGA - hex preview", sizeof(u->prev_note));
    }
}

static void ud_build_preview(udstate *u)
{
    ud_preview_reset(u);
    u->prev_valid=1; u->prev_sel=u->sel; u->prev_level=u->level; u->prev_mode=u->mode;

    if(u->level==NAV_DEVICES){
        u->prev_kind=PK_DEVICE;
        if(u->sel>=0 && u->sel<u->ndev){
            struct diskio_dev *d=&u->dev[u->sel];
            ud_scopy(u->prev_name, d->label[0]?d->label:"device", sizeof(u->prev_name));
            /* Probe the FS once here (each call is a live block read); the
             * per-frame render just draws the cached string. */
            const char *fsn="?";
            if(gBS && d->bio){
                if(ext_probe(gBS,d->bio)) fsn="ext2/3/4";
                else if(btrfs_probe(gBS,d->bio)) fsn="btrfs";
                else { EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *s=0;
                       if(d->handle && !EFI_ERROR(gBS->HandleProtocol(d->handle,&gSfsGuid,(VOID**)&s)) && s) fsn="FAT/ESP";
                       else fsn="unrecognized"; }
            }
            ud_scopy(u->prev_note, fsn, sizeof(u->prev_note));
        }
        return;
    }

    if(u->level==NAV_CARVE){
        if(u->sel<0 || u->sel>=u->nitems){ u->prev_kind=PK_NONE; return; }
        u->prev_kind=PK_CARVE;
        int t=u->item[u->sel].type;
        UINT64 size=u->item[u->sel].size;
        u->prev_size=size;
        { int p=0; u->prev_name[0]=0;
          ap_s(u->prev_name,&p,g_sig[t].name); ap_s(u->prev_name,&p," extent"); u->prev_name[p]=0; }
        struct diskio_dev *d=&u->dev[u->cur_dev];
        UINT64 want=size; if(want>UD_PREVBUF) want=UD_PREVBUF;
        int whole=(size<=UD_PREVBUF);
        struct diskio_read_stat st;
        int64_t got=-1;
        if(want>0 && diskio_read_bytes(d, u->item[u->sel].off, g_prevbuf, (UINTN)want, &st)>=0)
            got=(int64_t)want;
        ud_preview_from_buf(u, got, whole);  /* img_decode rejects non-BMP/TGA */
        return;
    }

    /* NAV_FS */
    if(u->sel<0 || u->sel>=u->nent){ u->prev_kind=PK_NONE; return; }
    ud_ent *e=&u->ent[u->sel];
    ud_scopy(u->prev_name, e->name, sizeof(u->prev_name));
    if(e->dotdot || e->isdir){ u->prev_kind=PK_DIR; return; }
    if(e->subvol){ u->prev_kind=PK_SUBVOL;
        ud_scopy(u->prev_note, "btrfs subvolume (no file read)", sizeof(u->prev_note)); return; }

    u->prev_kind=PK_FILE;
    u->prev_size=e->size;
    char full[UD_PATH]; path_join(u->path, e->name, full, UD_PATH);
    UINT64 want=e->size; if(want>UD_PREVBUF) want=UD_PREVBUF;
    int whole=(e->size<=UD_PREVBUF);
    int64_t got = (want>0) ? ud_read_fs(u, full, want, g_prevbuf) : 0;
    /* ext files may report size 0 via listing (unknown); attempt a bounded read */
    if(e->size==0){
        got = ud_read_fs(u, full, UD_PREVBUF, g_prevbuf);
        whole = (got>=0 && got<(int64_t)UD_PREVBUF);
        if(got>0) u->prev_size=(UINT64)got;
    }
    ud_preview_from_buf(u, got, whole);
}

/* ==========================================================================
 * Recovery: copy selected real file OR carved extent to the ESP.
 * ========================================================================== */
static void ensure_dir(EFI_FILE_PROTOCOL *root, const CHAR16 *path)
{
    EFI_FILE_PROTOCOL *d=0;
    if(!EFI_ERROR(root->Open(root, &d, (CHAR16*)path,
                             EFI_FILE_MODE_CREATE|EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,
                             EFI_FILE_DIRECTORY)) && d)
        d->Close(d);
}

/* Open \forebo\recovered\NNNN.<ext> for writing. Fills wp_ascii for the msg. */
static EFI_FILE_PROTOCOL *ud_open_dest(udstate *u, EFI_FILE_PROTOCOL *root,
                                       const char *ext, char *ascii_out, int cap)
{
    ensure_dir(root, L"\\forebo");
    ensure_dir(root, L"\\forebo\\recovered");
    int p=0; ascii_out[0]=0;
    ap_s(ascii_out,&p,"/forebo/recovered/"); ap_u4(ascii_out,&p,u->saved_seq);
    ap_c(ascii_out,&p,'.'); ap_s(ascii_out,&p,ext);
    if(p>=cap) p=cap-1; ascii_out[p]=0;
    CHAR16 wp[UD_PATH]; esp_ascii_to_char16(ascii_out, wp, UD_PATH);
    EFI_FILE_PROTOCOL *fh=0;
    if(EFI_ERROR(root->Open(root,&fh,wp,
                 EFI_FILE_MODE_CREATE|EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0)) || !fh)
        return 0;
    return fh;
}

/* Recover a carved extent by streaming from the raw device. */
static void ud_recover_carve(udstate *u, int idx)
{
    struct diskio_dev *d=&u->dev[u->cur_dev];
    int t=u->item[idx].type;
    UINT64 off=u->item[idx].off, size=u->item[idx].size;

    EFI_FILE_PROTOCOL *root=0;
    if(EFI_ERROR(esp_open_root(gImage, gBS, &root)) || !root){
        ud_scopy(u->msg,"Recover: cannot open ESP",sizeof(u->msg)); return; }
    char ap[UD_PATH];
    EFI_FILE_PROTOCOL *fh=ud_open_dest(u, root, g_sig[t].ext, ap, sizeof(ap));
    if(!fh){ root->Close(root); ud_scopy(u->msg,"Recover: cannot create file",sizeof(u->msg)); return; }

    UINT64 done=0, bad=0;
    while(done<size){
        UINTN chunk=UD_RECBUF;
        if((UINT64)chunk>size-done) chunk=(UINTN)(size-done);
        struct diskio_read_stat st;
        if(diskio_read_bytes(d, off+done, g_recbuf, chunk, &st)<0) break;
        bad += st.blocks_bad;
        UINTN wsz=chunk;
        if(EFI_ERROR(fh->Write(fh,&wsz,g_recbuf))) break;
        done += chunk;
    }
    fh->Flush(fh); fh->Close(fh); root->Close(root);
    u->item[idx].saved=1; u->saved_seq++;

    int q=0; u->msg[0]=0;
    ap_s(u->msg,&q,"Saved "); ap_s(u->msg,&q,ap); ap_s(u->msg,&q,"  (");
    ap_sz(u->msg,&q,done);
    if(bad){ ap_s(u->msg,&q,", "); ap_u(u->msg,&q,bad); ap_s(u->msg,&q," bad"); }
    ap_c(u->msg,&q,')'); u->msg[q]=0;
}

/* Recover a real FAT file by streaming through EFI_FILE Read. */
static void ud_recover_fat(udstate *u, const char *full, const char *name)
{
    CHAR16 wp[UD_PATH]; esp_ascii_to_char16(full, wp, UD_PATH);
    EFI_FILE_PROTOCOL *src=0;
    if(EFI_ERROR(u->fatroot->Open(u->fatroot,&src,wp,EFI_FILE_MODE_READ,0)) || !src){
        ud_scopy(u->msg,"Recover: cannot open source",sizeof(u->msg)); return; }
    EFI_FILE_PROTOCOL *root=0;
    if(EFI_ERROR(esp_open_root(gImage, gBS, &root)) || !root){
        src->Close(src); ud_scopy(u->msg,"Recover: cannot open ESP",sizeof(u->msg)); return; }
    char ap[UD_PATH];
    EFI_FILE_PROTOCOL *fh=ud_open_dest(u, root, name_ext(name), ap, sizeof(ap));
    if(!fh){ src->Close(src); root->Close(root); ud_scopy(u->msg,"Recover: cannot create file",sizeof(u->msg)); return; }

    UINT64 done=0;
    for(;;){
        UINTN rsz=UD_RECBUF;
        if(EFI_ERROR(src->Read(src,&rsz,g_recbuf)) || rsz==0) break;
        UINTN wsz=rsz;
        if(EFI_ERROR(fh->Write(fh,&wsz,g_recbuf))) break;
        done += rsz;
    }
    fh->Flush(fh); fh->Close(fh); root->Close(root); src->Close(src);
    u->saved_seq++;

    int q=0; u->msg[0]=0;
    ap_s(u->msg,&q,"Saved "); ap_s(u->msg,&q,ap); ap_s(u->msg,&q,"  (");
    ap_sz(u->msg,&q,done); ap_c(u->msg,&q,')'); u->msg[q]=0;
}

/* Recover a real ext file via one bounded AllocatePool read (ext_read has no
 * offset, so we pull up to UD_FSREC_CAP bytes at once). */
static void ud_recover_ext(udstate *u, const char *full, const char *name)
{
    int64_t fsz=ext_file_size(u->ext, full);
    if(fsz<0){ ud_scopy(u->msg,"Recover: not a regular file",sizeof(u->msg)); return; }
    UINT64 want=(UINT64)fsz; int truncated=0;
    if(want>UD_FSREC_CAP){ want=UD_FSREC_CAP; truncated=1; }

    void *buf=0;
    if(want>0){
        if(!gBS || EFI_ERROR(gBS->AllocatePool(EfiLoaderData,(UINTN)want,&buf)) || !buf){
            ud_scopy(u->msg,"Recover: out of memory",sizeof(u->msg)); return; }
    }
    int64_t got = (want>0) ? ext_read(u->ext, full, buf, want) : 0;
    if(got<0){ if(buf) gBS->FreePool(buf); ud_scopy(u->msg,"Recover: read failed",sizeof(u->msg)); return; }

    EFI_FILE_PROTOCOL *root=0;
    if(EFI_ERROR(esp_open_root(gImage, gBS, &root)) || !root){
        if(buf) gBS->FreePool(buf); ud_scopy(u->msg,"Recover: cannot open ESP",sizeof(u->msg)); return; }
    char ap[UD_PATH];
    EFI_FILE_PROTOCOL *fh=ud_open_dest(u, root, name_ext(name), ap, sizeof(ap));
    if(!fh){ if(buf) gBS->FreePool(buf); root->Close(root);
             ud_scopy(u->msg,"Recover: cannot create file",sizeof(u->msg)); return; }

    UINT64 done=0;
    while((int64_t)done<got){
        UINTN wsz=UD_RECBUF;
        if((int64_t)(done+wsz)>got) wsz=(UINTN)(got-done);
        if(EFI_ERROR(fh->Write(fh,&wsz,(UINT8*)buf+done))) break;
        done += wsz;
    }
    fh->Flush(fh); fh->Close(fh); root->Close(root);
    if(buf) gBS->FreePool(buf);
    u->saved_seq++;

    int q=0; u->msg[0]=0;
    ap_s(u->msg,&q,"Saved "); ap_s(u->msg,&q,ap); ap_s(u->msg,&q,"  (");
    ap_sz(u->msg,&q,done);
    if(truncated) ap_s(u->msg,&q,", truncated");
    ap_c(u->msg,&q,')'); u->msg[q]=0;
}

static void ud_recover(udstate *u)
{
    if(!gBS || !gImage){ ud_scopy(u->msg,"Recover: unavailable",sizeof(u->msg)); return; }

    if(u->level==NAV_CARVE){
        if(u->sel<0 || u->sel>=u->nitems){ ud_scopy(u->msg,"Recover: nothing selected",sizeof(u->msg)); return; }
        ud_recover_carve(u, u->sel);
        return;
    }
    if(u->level==NAV_FS){
        if(u->sel<0 || u->sel>=u->nent){ ud_scopy(u->msg,"Recover: nothing selected",sizeof(u->msg)); return; }
        ud_ent *e=&u->ent[u->sel];
        if(e->dotdot || e->isdir || e->subvol){ ud_scopy(u->msg,"Recover: select a file",sizeof(u->msg)); return; }
        char full[UD_PATH]; path_join(u->path, e->name, full, UD_PATH);
        if(u->fstype==FS_EXT)      ud_recover_ext(u, full, e->name);
        else if(u->fstype==FS_FAT) ud_recover_fat(u, full, e->name);
        else                       ud_scopy(u->msg,"Recover: unsupported source",sizeof(u->msg));
        return;
    }
    ud_scopy(u->msg,"Recover: select a file or carved item",sizeof(u->msg));
}

/* ==========================================================================
 * Navigation actions.
 * ========================================================================== */
static void ud_set_mode(udstate *u, int mode)
{
    ud_unmount(u);
    u->scanning=0;
    u->mode=mode;
    u->level=NAV_DEVICES;
    u->nitems=0;
    u->sel=(u->cur_dev>=0 && u->cur_dev<u->ndev)?u->cur_dev:0;
    u->scroll=0;
    u->msg[0]=0;
    ud_invalidate(u);
}

static void ud_dev_open(udstate *u)
{
    if(u->sel<0 || u->sel>=u->ndev) return;
    u->cur_dev=u->sel;
    if(u->mode==MODE_CARVE){ start_carve(u); ud_invalidate(u); return; }

    if(!ud_mount(u, u->cur_dev)){
        int p=0; u->msg[0]=0;
        ap_s(u->msg,&p,"unrecognized FS on ");
        ap_s(u->msg,&p, u->dev[u->cur_dev].label[0]?u->dev[u->cur_dev].label:"device");
        u->msg[p]=0;
        return;
    }
    u->path[0]='/'; u->path[1]=0;
    u->level=NAV_FS; u->sel=0; u->scroll=0; u->msg[0]=0;
    ud_fs_list(u);
    ud_invalidate(u);
}

static void ud_fs_enter(udstate *u)
{
    if(u->sel<0 || u->sel>=u->nent) return;
    ud_ent *e=&u->ent[u->sel];
    if(e->dotdot){ path_up(u->path); u->sel=0; ud_fs_list(u); ud_invalidate(u); return; }
    if(e->subvol) return;                       /* btrfs subvols are flat leaves */
    if(e->isdir){
        char np[UD_PATH]; path_join(u->path, e->name, np, UD_PATH);
        ud_scopy(u->path, np, UD_PATH);
        u->sel=0; ud_fs_list(u); ud_invalidate(u); return;
    }
    /* file: selection already drives the preview; Enter is descend-only */
}

static void ud_nav_up(udstate *u)
{
    if(u->level==NAV_FS){
        if(path_is_root(u->path)){
            ud_unmount(u);
            u->level=NAV_DEVICES; u->sel=u->cur_dev; u->scroll=0;
        } else {
            path_up(u->path); u->sel=0; ud_fs_list(u);
        }
    } else if(u->level==NAV_CARVE){
        u->scanning=0;
        u->level=NAV_DEVICES; u->sel=u->cur_dev; u->scroll=0;
    }
    ud_invalidate(u);
}

/* ==========================================================================
 * Layout geometry.
 * ========================================================================== */
static int L_lineH(void){ return 16*gsc(); }
static int ud_headerH(void){ int s=gsc(); return 4*s + L_lineH() + 4*s; }
static int ud_btnbar_y(int cy, int ch){ return cy + ch - 4*gsc() - wm_button_h(); }
static int ud_status_y(int cy, int ch){ return ud_btnbar_y(cy,ch) - L_lineH() - 2*gsc(); }
static int ud_leftw(int cw){ int w=cw*44/100; if(w<150)w=150; if(w>cw-160)w=cw-160; if(w<60)w=cw/2; return w; }
static int ud_list_y(int cy){ return cy + ud_headerH(); }
static int ud_panes_bot(int cy, int ch){ return ud_status_y(cy,ch) - 2*gsc(); }
static int ud_rows(int cy, int ch)
{ int r=(ud_panes_bot(cy,ch)-ud_list_y(cy))/L_lineH(); return r<1?1:r; }

static int ud_left_count(udstate *u)
{ return (u->level==NAV_DEVICES)?u->ndev : (u->level==NAV_CARVE)?u->nitems : u->nent; }

/* Left-pane row under client-relative my, or -1. */
static int ud_row_at(udstate *u, int ch, int mx, int my, int cw)
{
    if(mx >= ud_leftw(cw)) return -1;
    int top=ud_headerH();
    if(my<top) return -1;
    int rows=ud_rows(0,ch);
    int r=(my-top)/L_lineH();
    if(r<0 || r>=rows) return -1;
    int idx=u->scroll+r;
    int n=ud_left_count(u);
    return (idx<n)?idx:-1;
}

/* ==========================================================================
 * Rendering.
 * ========================================================================== */
enum { UB_OPEN=1, UB_UP, UB_RECOVER, UB_MODE, UB_STOP, UB_CLOSE };

/* Build one left-pane row's text. */
static void row_text(udstate *u, int idx, char *out, UINT32 *fg)
{
    int p=0; out[0]=0; *fg=c_fg;
    if(u->level==NAV_DEVICES){
        struct diskio_dev *d=&u->dev[idx];
        ap_s(out,&p, d->logical_partition ? "[part] " : "[disk] ");
        ap_s(out,&p, d->label[0]?d->label:"device");
        ap_s(out,&p, "  "); ap_sz(out,&p, d->total_bytes);
        if(d->removable) ap_s(out,&p,"  (rm)");
        *fg=c_accent;
    } else if(u->level==NAV_CARVE){
        int t=u->item[idx].type;
        ap_u4(out,&p,(unsigned)idx); ap_s(out,&p,"  ");
        ap_s(out,&p, g_sig[t].name);
        int pad=5-ud_slen(g_sig[t].name); while(pad-->0) ap_c(out,&p,' ');
        ap_s(out,&p,"  0x"); ap_x(out,&p, u->item[idx].off);
        ap_s(out,&p,"  "); ap_sz(out,&p, u->item[idx].size);
        if(u->item[idx].saved) ap_s(out,&p,"  [saved]");
    } else { /* NAV_FS */
        ud_ent *e=&u->ent[idx];
        if(e->dotdot){ ap_s(out,&p,"[..]"); *fg=c_accent; }
        else if(e->isdir){ ap_s(out,&p,"[dir] "); ap_s(out,&p,e->name); *fg=c_accent; }
        else if(e->subvol){ ap_s(out,&p,"{sub} "); ap_s(out,&p,e->name); *fg=c_accent; }
        else { ap_s(out,&p,"      "); ap_s(out,&p,e->name); }
    }
    out[p]=0;
}

static void ud_render_left(udstate *u, int cx, int cy, int cw, int ch)
{
    int sc=gsc(), lineH=L_lineH();
    int leftw=ud_leftw(cw);
    int x=cx+6*sc, y=ud_list_y(cy);
    int rows=ud_rows(cy,ch);
    int n=ud_left_count(u);
    int maxw=leftw-12*sc;

    if(u->sel<0) u->sel=0;
    if(u->sel>n-1) u->sel=n-1;
    if(u->sel>=0){
        if(u->sel<u->scroll)              u->scroll=u->sel;
        else if(u->sel>=u->scroll+rows)   u->scroll=u->sel-rows+1;
    }
    if(u->scroll>n-rows) u->scroll=n-rows;
    if(u->scroll<0) u->scroll=0;

    int barw=(n>rows)?6*sc:0;
    for(int r=0;r<rows;r++){
        int idx=u->scroll+r; if(idx>=n) break;
        int ry=y+r*lineH;
        UINT32 fg;
        char ln[UD_COLS]; row_text(u, idx, ln, &fg);
        if(idx==u->sel){ fill_rect(x-3*sc, ry-1, leftw-8*sc-barw, lineH, c_sel_bg); fg=c_sel_fg; }
        draw_string_clip(x, ry, maxw-barw, ln, fg, c_win, 1, sc);
    }
    if(barw){
        int trackX=cx+leftw-barw-2*sc, trackH=rows*lineH;
        fill_rect(trackX, y, barw, trackH, c_border);
        int thumbH=rows*trackH/n; if(thumbH<8*sc)thumbH=8*sc;
        int thumbY=y + ((n>rows)?u->scroll*(trackH-thumbH)/(n-rows):0);
        fill_rect(trackX, thumbY, barw, thumbH, c_accent);
    }
    if(n==0){
        const char *m=(u->level==NAV_DEVICES)?"No block devices."
                    : (u->level==NAV_CARVE)  ?"No items carved."
                                             :"(empty directory)";
        draw_string_clip(x, y, maxw, m, c_dim, c_win, 1, sc);
    }
}

/* Hex + ASCII block for the preview pane. */
static void ud_render_hex(udstate *u, int px, int py, int pw, int ph)
{
    int sc=gsc(), lineH=L_lineH();
    int per=16, rows=u->prev_hexlen/per + ((u->prev_hexlen%per)?1:0);
    int maxrows=ph/lineH; if(maxrows<1)maxrows=1;
    if(rows>maxrows) rows=maxrows;
    for(int r=0;r<rows;r++){
        char ln[UD_COLS]; int p=0;
        int base=r*per;
        ap_x(ln,&p,(UINT64)base); ap_s(ln,&p,": ");
        for(int i=0;i<per;i++){
            int off=base+i;
            if(off<u->prev_hexlen){
                static const char h[]="0123456789ABCDEF";
                ap_c(ln,&p,h[(u->prev_hex[off]>>4)&0xF]);
                ap_c(ln,&p,h[u->prev_hex[off]&0xF]);
            } else { ap_c(ln,&p,' '); ap_c(ln,&p,' '); }
            ap_c(ln,&p,' ');
        }
        ap_c(ln,&p,' ');
        for(int i=0;i<per;i++){
            int off=base+i;
            if(off<u->prev_hexlen){
                UINT8 b=u->prev_hex[off];
                ap_c(ln,&p,(b>=32 && b<127)?(char)b:'.');
            }
        }
        ln[p]=0;
        draw_string_clip(px, py+r*lineH, pw, ln, c_fg, c_win, 0, sc);
    }
}

static void ud_render_preview(udstate *u, int cx, int cy, int cw, int ch)
{
    int sc=gsc(), lineH=L_lineH();
    int leftw=ud_leftw(cw);
    int divx=cx+leftw;
    int px=divx+8*sc, py=ud_list_y(cy);
    int pw=cx+cw - px - 4*sc;
    int pbot=ud_panes_bot(cy,ch);
    int ph=pbot-py;
    if(pw<20 || ph<lineH) return;

    /* header: name + size/type */
    char h[UD_COLS]; int p=0;
    ap_s(h,&p,"Preview"); h[p]=0;
    draw_string_clip(px, py, pw, h, c_accent, c_win, 1, sc);
    int y=py+lineH+2*sc;

    if(u->prev_kind==PK_NONE){
        draw_string_clip(px, y, pw, "(nothing selected)", c_dim, c_win, 1, sc);
        return;
    }
    if(u->prev_kind==PK_DEVICE){
        if(u->sel>=0 && u->sel<u->ndev){
            struct diskio_dev *d=&u->dev[u->sel];
            char l[UD_COLS]; int q;
            q=0; ap_s(l,&q,"Name: "); ap_s(l,&q,d->label[0]?d->label:"device"); l[q]=0;
            draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
            q=0; ap_s(l,&q,d->logical_partition?"Type: partition":"Type: whole disk"); l[q]=0;
            draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
            q=0; ap_s(l,&q,"Size: "); ap_sz(l,&q,d->total_bytes); l[q]=0;
            draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
            q=0; ap_s(l,&q,"Block: "); ap_u(l,&q,d->block_size); ap_s(l,&q," B"); l[q]=0;
            draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
            /* FS type resolved once in ud_build_preview -> u->prev_note. */
            q=0; ap_s(l,&q,"FS: "); ap_s(l,&q,u->prev_note[0]?u->prev_note:"?"); l[q]=0;
            draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
            draw_string_clip(px,y,pw,
                u->mode==MODE_CARVE?"Enter: carve this device":"Enter: browse this device",
                c_dim,c_win,1,sc);
        }
        return;
    }
    if(u->prev_kind==PK_DIR){
        char l[UD_COLS]; int q=0; ap_s(l,&q,"Directory: "); ap_s(l,&q,u->prev_name); l[q]=0;
        draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
        draw_string_clip(px,y,pw,"Enter to descend.",c_dim,c_win,1,sc);
        return;
    }
    if(u->prev_kind==PK_SUBVOL){
        char l[UD_COLS]; int q=0; ap_s(l,&q,"Subvolume: "); ap_s(l,&q,u->prev_name); l[q]=0;
        draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH;
        draw_string_clip(px,y,pw,u->prev_note,c_dim,c_win,1,sc);
        return;
    }

    /* PK_FILE / PK_CARVE */
    { char l[UD_COLS]; int q=0; ap_s(l,&q,"Name: "); ap_s(l,&q,u->prev_name); l[q]=0;
      draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH; }
    { char l[UD_COLS]; int q=0; ap_s(l,&q,"Size: "); ap_sz(l,&q,u->prev_size); l[q]=0;
      draw_string_clip(px,y,pw,l,c_fg,c_win,1,sc); y+=lineH; }

    if(u->prev_is_img && u->prev_img.pixels && u->prev_img.w>0 && u->prev_img.h>0){
        char l[UD_COLS]; int q=0;
        ap_s(l,&q,"Image "); ap_u(l,&q,(UINT64)u->prev_img.w);
        ap_c(l,&q,'x'); ap_u(l,&q,(UINT64)u->prev_img.h); l[q]=0;
        draw_string_clip(px,y,pw,l,c_dim,c_win,1,sc); y+=lineH+2*sc;

        int availh=pbot-y; if(availh<8) availh=8;
        int iw=u->prev_img.w, ih=u->prev_img.h;
        int dstw=pw, dsth=(int)((INT64)ih*pw/iw);
        if(dsth>availh){ dsth=availh; dstw=(int)((INT64)iw*availh/ih); }
        if(dstw<1)dstw=1; if(dsth<1)dsth=1;
        int bx=px+(pw-dstw)/2, by=y+(availh-dsth)/2;
        img_blit_scaled(&u->prev_img, bx, by, dstw, dsth);
    } else {
        if(u->prev_note[0]){ draw_string_clip(px,y,pw,u->prev_note,c_dim,c_win,1,sc); y+=lineH+2*sc; }
        ud_render_hex(u, px, y, pw, pbot-y);
    }
}

static void ud_header(udstate *u, int cx, int cy, int cw)
{
    int sc=gsc(), x=cx+6*sc, y=cy+4*sc;
    char h[UD_COLS]; int p=0;
    if(u->level==NAV_DEVICES){
        ap_s(h,&p, u->mode==MODE_CARVE ? "Carve mode - pick a device (Tab: Browse)"
                                       : "Browse mode - pick a device (Tab: Carve)");
        h[p]=0; draw_string_clip(x,y,cw-12*sc,h,c_accent,c_win,1,sc);
    } else if(u->level==NAV_FS){
        const char *fsn=(u->fstype==FS_EXT)?"ext":(u->fstype==FS_FAT)?"fat":(u->fstype==FS_BTRFS)?"btrfs":"?";
        ap_s(h,&p,fsn); ap_s(h,&p,":");
        ap_s(h,&p, u->dev[u->cur_dev].label[0]?u->dev[u->cur_dev].label:"dev");
        ap_s(h,&p,"  "); ap_s(h,&p,u->path);
        h[p]=0; draw_string_clip(x,y,cw-12*sc,h,c_accent,c_win,1,sc);
    } else { /* NAV_CARVE */
        UINT64 pct=u->scan_end?(u->scan_off*100/u->scan_end):100;
        if(u->scanning){ ap_s(h,&p,"Carving "); ap_u(h,&p,pct); ap_s(h,&p,"%  found "); ap_u(h,&p,(UINT64)u->nitems); }
        else { ap_s(h,&p, u->msg[0]?u->msg:"Carve done"); }
        h[p]=0; draw_string_clip(x,y,cw-12*sc,h,c_accent,c_win,1,sc);
        if(u->scanning){
            int barY=y+L_lineH()-3*sc, barW=cw-12*sc, barH=3*sc;
            fill_rect(x,barY,barW,barH,c_border);
            int fillW=(int)((UINT64)barW*pct/100);
            if(fillW>0) fill_rect(x,barY,fillW,barH,c_accent);
        }
    }
}

/* Bottom button bar; fills out[], returns count. */
static int ud_btns(udstate *u, int cw, int ch, wm_button *out)
{
    int sc=gsc(), y=ud_btnbar_y(0,ch), x=6*sc, n=0;
    int n_left=ud_left_count(u);

    if(u->level==NAV_DEVICES){
        out[n].id=UB_OPEN; ud_scopy(out[n].label, u->mode==MODE_CARVE?"Carve":"Open", sizeof(out[n].label));
        out[n].enabled=(n_left>0); n++;
    } else if(u->level==NAV_FS){
        out[n].id=UB_OPEN; ud_scopy(out[n].label,"Enter",sizeof(out[n].label)); out[n].enabled=(n_left>0); n++;
        out[n].id=UB_UP;   ud_scopy(out[n].label,"Up",sizeof(out[n].label));    out[n].enabled=1; n++;
        out[n].id=UB_RECOVER; ud_scopy(out[n].label,"Recover",sizeof(out[n].label)); out[n].enabled=(n_left>0); n++;
    } else { /* NAV_CARVE */
        if(u->scanning){ out[n].id=UB_STOP; ud_scopy(out[n].label,"Stop",sizeof(out[n].label)); out[n].enabled=1; n++; }
        else { out[n].id=UB_RECOVER; ud_scopy(out[n].label,"Recover",sizeof(out[n].label)); out[n].enabled=(u->nitems>0); n++; }
        out[n].id=UB_UP; ud_scopy(out[n].label,"Up",sizeof(out[n].label)); out[n].enabled=1; n++;
    }
    out[n].id=UB_MODE; ud_scopy(out[n].label, u->mode==MODE_CARVE?"Browse":"Carve", sizeof(out[n].label));
    out[n].enabled=1; n++;

    /* lay left-aligned buttons out */
    for(int i=0;i<n;i++){ out[i].x=x; out[i].y=y; out[i].h=wm_button_h();
                          out[i].w=wm_button_measure(out[i].label); x+=out[i].w+6*sc; }
    /* right-aligned Close */
    out[n].id=UB_CLOSE; ud_scopy(out[n].label,"Close",sizeof(out[n].label));
    out[n].enabled=1; out[n].h=wm_button_h(); out[n].w=wm_button_measure("Close");
    out[n].x=cw-6*sc-out[n].w; out[n].y=y; n++;
    return n;
}

static int ud_btn_hit(const wm_button *b, int n, int mx, int my)
{ for(int i=0;i<n;i++) if(b[i].enabled && wm_button_hit(&b[i],mx,my)) return b[i].id; return 0; }

static void ud_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    udstate *u=(udstate*)wm_user(w);
    if(!u) return;
    resolve_theme();

    scan_tick(u);

    if(!u->avail){
        draw_string_clip(cx+6*gsc(), cy+6*gsc(), cw-12*gsc(),
            "Undelete unavailable (services not initialised).", c_warn, c_win, 1, gsc());
        return;
    }

    /* refresh preview when the selection / view changed, but debounce the
     * multi-MiB read + img_decode: only build once the selection has been held
     * for a frame (sel unchanged since last draw). Rapid scrolling skips the
     * heavy read until the cursor settles. Safe because ud_render_preview does
     * not clear the client interior, so a one-frame-stale preview is fine. */
    if((!u->prev_valid || u->prev_sel!=u->sel || u->prev_level!=u->level || u->prev_mode!=u->mode)
       && u->sel==u->prev_pending)
        ud_build_preview(u);
    u->prev_pending=u->sel;

    int sc=gsc();
    ud_header(u, cx, cy, cw);

    /* vertical divider between panes */
    int leftw=ud_leftw(cw);
    draw_vline(cx+leftw, ud_list_y(cy), ud_panes_bot(cy,ch)-ud_list_y(cy), c_border);

    ud_render_left(u, cx, cy, cw, ch);
    ud_render_preview(u, cx, cy, cw, ch);

    /* status line (full width) */
    if(u->msg[0])
        draw_string_clip(cx+6*sc, ud_status_y(cy,ch), cw-12*sc, u->msg, c_dim, c_win, 1, sc);

    draw_hline(cx+4*sc, ud_btnbar_y(cy,ch)-4*sc, cw-8*sc, c_border);
    /* Lay out the button bar once per frame and cache it; the mouse handlers
     * hit-test this cache instead of rebuilding the bar per event. */
    u->nbtn_cache=ud_btns(u, cw, ch, u->btn_cache);
    for(int i=0;i<u->nbtn_cache;i++)
        wm_button_draw(&u->btn_cache[i], u->btn_cache[i].id==u->b_hover, u->btn_cache[i].id==u->b_press);
}

/* ==========================================================================
 * Events.
 * ========================================================================== */
static void ud_activate(udstate *u, int id)
{
    switch(id){
        case UB_OPEN:
            if(u->level==NAV_DEVICES) ud_dev_open(u);
            else if(u->level==NAV_FS) ud_fs_enter(u);
            break;
        case UB_UP:      ud_nav_up(u); break;
        case UB_RECOVER: ud_recover(u); break;
        case UB_MODE:    ud_set_mode(u, u->mode==MODE_CARVE?MODE_BROWSE:MODE_CARVE); break;
        case UB_STOP:    u->scanning=0; ud_scopy(u->msg,"Carve stopped.",sizeof(u->msg)); break;
        default: break;
    }
}

static int ud_event(wm_window *w, const wm_event *ev)
{
    udstate *u=(udstate*)wm_user(w);
    if(!u) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    int n=ud_left_count(u);
    int rows=ud_rows(0,ch);

    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_TAB){ ud_set_mode(u, u->mode==MODE_CARVE?MODE_BROWSE:MODE_CARVE); return 0; }
            if(ev->scancode==SCAN_UP)             { if(u->sel>0)u->sel--; }
            else if(ev->scancode==SCAN_DOWN)      { if(u->sel<n-1)u->sel++; }
            else if(ev->scancode==SCAN_PAGE_UP)   { u->sel-=rows; if(u->sel<0)u->sel=0; }
            else if(ev->scancode==SCAN_PAGE_DOWN) { u->sel+=rows; if(u->sel>n-1)u->sel=n-1; }
            else if(ev->scancode==SCAN_HOME)      { u->sel=0; }
            else if(ev->scancode==SCAN_END)       { u->sel=n-1; if(u->sel<0)u->sel=0; }
            else if(ev->scancode==SCAN_LEFT || ev->unicode==CHAR_BACKSPACE) { ud_nav_up(u); }
            else if(ev->unicode==CHAR_CR || ev->scancode==SCAN_RIGHT){
                if(u->level==NAV_DEVICES) ud_dev_open(u);
                else if(u->level==NAV_FS) ud_fs_enter(u);
            }
            else if(ev->unicode=='r' || ev->unicode=='R') ud_recover(u);
            return 0;

        case WM_EV_MOUSE_WHEEL:
            u->sel -= ev->wheel;
            if(u->sel<0)u->sel=0; if(u->sel>n-1)u->sel=n-1;
            return 0;

        case WM_EV_MOUSE_MOVE: {
            u->b_hover=ud_btn_hit(u->btn_cache,u->nbtn_cache,ev->mx,ev->my);
            return 0; }

        case WM_EV_MOUSE_DOWN: {
            int id=ud_btn_hit(u->btn_cache,u->nbtn_cache,ev->mx,ev->my);
            if(id){ u->b_press=id; return 0; }
            int r=ud_row_at(u,ch,ev->mx,ev->my,cw);
            if(r>=0){
                if(r==u->sel){
                    if(u->level==NAV_DEVICES) ud_dev_open(u);
                    else if(u->level==NAV_FS) ud_fs_enter(u);
                } else u->sel=r;
            }
            return 0; }

        case WM_EV_MOUSE_UP: {
            if(!u->b_press) return 0;
            int id=ud_btn_hit(u->btn_cache,u->nbtn_cache,ev->mx,ev->my), pr=u->b_press;
            u->b_press=0;
            if(id==pr){
                if(pr==UB_CLOSE) return WM_CLOSE_REQUEST;
                ud_activate(u, pr);
            }
            return 0; }

        case WM_EV_CLOSE:
            ud_unmount(u);
            img_free(&u->prev_img);
            u->win=NULL; u->scanning=0;
            return 0;

        default: return 0;
    }
}

/* ==========================================================================
 * Open.
 * ========================================================================== */
void tool_undelete_open(void)
{
    if(g_ud.win) return;                    /* already open */

    if(gBS) gBS->SetMem(&g_ud, sizeof(g_ud), 0);
    else { char *pz=(char*)&g_ud; for(unsigned i=0;i<sizeof(g_ud);i++) pz[i]=0; }

    g_ud.avail=(gBS && gST)?1:0;
    g_ud.mode=MODE_BROWSE;
    g_ud.level=NAV_DEVICES;
    g_ud.cur_dev=0;
    g_ud.fstype=FS_NONE;
    g_ud.path[0]='/'; g_ud.path[1]=0;

    if(g_ud.avail)
        g_ud.ndev=diskio_enumerate(g_ud.dev, UD_MAX_DEV);

    resolve_theme();

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*70/100; if(ww<640)ww=640; if(ww>1100)ww=1100; if(ww>W-40)ww=W-40;
    int wh=H*66/100; if(wh<360)wh=360; if(wh>720)wh=720; if(wh>H-40)wh=H-40;
    g_ud.win=wm_open("Undelete / Carve", ww, wh, ud_draw, ud_event, &g_ud);
}

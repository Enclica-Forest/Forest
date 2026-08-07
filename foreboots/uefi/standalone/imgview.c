/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/imgview.c - Windowed GUI image viewer (browse + preview BMP/TGA).
 * =============================================================================
 * Implements the contract in imgview.h. A self-contained wm.c "template B" tool:
 *
 *   BROWSE mode - a scrollable directory listing rooted at the ESP. Directories
 *     descend on Enter (with a ".." entry + breadcrumb + Up button); .bmp/.tga
 *     files open into PREVIEW; every other file is greyed and annotated
 *     "(no preview)" and simply does nothing when opened.
 *
 *   PREVIEW mode - the decoded image scaled-to-fit (aspect-preserving letterbox)
 *     inside the client rect, with the filename + WxH on a header line.
 *     Left/Right or PgUp/PgDn step to the prev/next image in the SAME directory;
 *     [Fit]/[1:1] toggles fit-to-window vs actual size; Esc (or [List]) returns
 *     to the browser. A decode failure shows "cannot decode <name>: <reason>"
 *     rather than crashing.
 *
 * Only image.c's public API is used to decode/blit (img_decode / img_free); the
 * preview is composited with a clipped nearest-neighbour sampler so an actual-
 * size image larger than the window never spills outside the client rect.
 *
 * Freestanding (no libc), pre-ExitBootServices. Fixed static pools; heap only
 * via BootServices AllocatePool for the transient file read buffer (freed
 * immediately after img_decode()).
 * ========================================================================== */

#include "imgview.h"
#include "../efi.h"
#include "../efi_ext.h"
#include "../core/wm.h"
#include "../ui.h"
#include "../core/input.h"
#include "../core/image.h"
#include "../core/config.h"
#include "../../include/forebo_cfg.h"

/* ==========================================================================
 * Module state (captured at tool_imgview_init).
 * ========================================================================== */
static EFI_HANDLE         gImage;
static EFI_BOOT_SERVICES *gBS;
static EFI_GUID           gFinfoGuid = EFI_FILE_INFO_ID;

void tool_imgview_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    gImage = image;
    gBS    = st ? st->BootServices : 0;
}

/* ==========================================================================
 * Tiny freestanding helpers.
 * ========================================================================== */
static int  iv_slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }
static void iv_scopy(char *d, const char *s, int cap)
{ int i=0; if(cap<=0)return; for(;s&&s[i]&&i+1<cap;i++)d[i]=s[i]; d[i]=0; }
static void iv_u2a(const CHAR16 *u, char *a, int cap)
{ int i=0; for(;u&&u[i]&&i+1<cap;i++){ CHAR16 c=u[i]; a[i]=(c>=0x20&&c<0x7f)?(char)c:'?'; } a[i]=0; }
/* Append decimal `v` to `b` at *p (bounded by cap); advances *p. */
static void iv_appu(char *b, int *p, int cap, UINT64 v)
{ char t[24]; int i=0; if(!v){ if(*p<cap-1)b[(*p)++]='0'; return; }
  while(v){ t[i++]=(char)('0'+(int)(v%10)); v/=10; }
  while(i && *p<cap-1) b[(*p)++]=t[--i]; }
static void iv_apps(char *b, int *p, int cap, const char *s)
{ while(s&&*s && *p<cap-1) b[(*p)++]=*s++; }

static int gsc(void){ int s=ui_scale(); return s<1?1:s; }

/* Case-insensitive test that `name` ends with `.bmp` or `.tga`. */
static int iv_ext_is(const char *name, const char *ext4)
{
    int n=iv_slen(name); if(n<4) return 0;
    const char *s=name+n-4;
    for(int i=0;i<4;i++){ char a=s[i], b=ext4[i];
        if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b) return 0; }
    return 1;
}
static int iv_is_image(const char *name)
{ return iv_ext_is(name,".bmp") || iv_ext_is(name,".tga"); }

/* ==========================================================================
 * Viewer state.
 * ========================================================================== */
#define IV_MAXENT   256
#define IV_NAMELEN  96
#define IV_MAXFILE  (16*1024*1024)   /* file-read cap (fits a 1080p 32bpp BMP) */

typedef struct {
    wm_window        *win;
    EFI_FILE_PROTOCOL *root;            /* volume root (closed on window close) */
    char   dir[256];                    /* current dir, '\'-relative, ""=root   */

    /* directory listing */
    char   name[IV_MAXENT][IV_NAMELEN];
    UINT8  isdir[IV_MAXENT];
    UINT8  isimg[IV_MAXENT];            /* decodable .bmp/.tga                  */
    UINT64 fsize[IV_MAXENT];
    int    n, scroll, sel;

    int    b_hover, b_press;            /* button-bar hover / pressed id        */

    /* Cached button-bar builds: MOUSE_MOVE fires far more often than redraws,
     * and rebuilding the bar re-measures every label's pixel width. Cache the
     * last build + the (cw,ch,state) it was built for; reuse it whenever those
     * haven't changed instead of rebuilding on every hit-test. */
    wm_button c_bbtns[3]; int c_bnb, c_bvalid, c_bcw, c_bch, c_bdir, c_bn;
    wm_button c_vbtns[5]; int c_vnb, c_vvalid, c_vcw, c_vch, c_vfit;

    /* preview */
    int    viewing;                     /* 0 = browse list, 1 = image preview   */
    int    fit;                         /* 1 = fit-to-window, 0 = actual size   */
    struct img_image img;               /* decoded image (when have_img)        */
    int    have_img;
    int    vidx;                        /* entry index being previewed          */
    char   vname[IV_NAMELEN];
    int    vw, vh;                      /* image dimensions                     */
    char   err[160];                    /* decode error, or "" when ok          */

    /* Change-gate for the preview image area. The scaled blit into uncached
     * VRAM is by far the costliest part of a preview frame; draw_one() never
     * clears the client interior, so when the previewed image, fit mode and
     * target rect are all unchanged the pixels on screen are still correct and
     * both the letterbox fill and the blit can be skipped. pv_valid==0 forces
     * one genuine repaint (set on show/step/fit-toggle). */
    int    pv_valid, pv_idx, pv_fit, pv_ax, pv_ay, pv_aw, pv_ah;
} ivstate;
static ivstate g_iv;

/* Resolved theme colours (recomputed each draw so live re-skin is honoured). */
static UINT32 c_win, c_fg, c_dim, c_accent, c_sel_bg, c_sel_fg, c_border, c_warn;
static void iv_colors(void)
{
    c_win    = wm_theme_color(WM_COL_WINDOW);
    c_fg     = wm_theme_color(WM_COL_FG);
    c_accent = wm_theme_color(WM_COL_ACCENT);
    c_sel_bg = wm_theme_color(WM_COL_SEL_BG);
    c_sel_fg = wm_theme_color(WM_COL_SEL_FG);
    c_dim    = wm_blend(c_fg, c_win, 150);
    c_border = wm_blend(c_fg, c_win, 200);
    c_warn   = 0x00E0B020u;
}

/* ==========================================================================
 * Layout helpers (shared by draw + hit-test so they can never disagree).
 * ========================================================================== */
/*
 * Each helper has an `_sc`-suffixed variant taking the scale factor explicitly,
 * plus a no-arg wrapper (used by call sites that don't already have `sc` in
 * hand) that just forwards to it via gsc(). Draw/event code that has already
 * computed `sc` locally should call the `_sc` variants directly so a single
 * ui_scale() lookup is reused instead of every helper re-deriving it.
 */
static int iv_lineH_sc(int sc){ return 16*sc; }
static int iv_lineH(void){ return iv_lineH_sc(gsc()); }
static int iv_bar_h_sc(int sc){ return wm_button_h() + 10*sc; }
static int iv_bar_h(void){ return iv_bar_h_sc(gsc()); }
static int iv_bar_fits_sc(int ch, int sc){ return (ch - iv_bar_h_sc(sc)) >= 40*sc; }
static int iv_bar_fits(int ch){ return iv_bar_fits_sc(ch,gsc()); }
static int iv_content_h_sc(int ch, int sc){ return iv_bar_fits_sc(ch,sc) ? ch - iv_bar_h_sc(sc) : ch; }
static int iv_content_h(int ch){ return iv_content_h_sc(ch,gsc()); }
/* Client-relative Y where the list / preview body starts (below the header). */
static int iv_body_y0_sc(int sc){ return 4*sc + iv_lineH_sc(sc) + 3*sc; }
static int iv_body_y0(void){ return iv_body_y0_sc(gsc()); }
static int iv_list_rows_sc(int ch, int sc)
{ int avail=iv_content_h_sc(ch,sc) - iv_body_y0_sc(sc) - 4*sc;
  int r=avail/iv_lineH_sc(sc); return r<1?1:r; }
static int iv_list_rows(int ch){ return iv_list_rows_sc(ch,gsc()); }

/* Build a standard bottom bar: `nl` left buttons then a right [Close]. */
static int iv_bar(int cw, int ch, const int *ids, const char *const *lbl,
                  int nl, int close_id, wm_button *out)
{
    int sc=gsc();
    if(!iv_bar_fits_sc(ch,sc)) return 0;
    int y=ch-4*sc-wm_button_h(), x=6*sc, n=0;
    for(int i=0;i<nl;i++){
        int w=wm_button_measure(lbl[i]);
        out[n].x=x; out[n].y=y; out[n].w=w; out[n].h=wm_button_h();
        out[n].id=ids[i]; out[n].enabled=1;
        iv_scopy(out[n].label, lbl[i], (int)sizeof out[n].label);
        x+=w+6*sc; n++;
    }
    if(close_id>0){
        int w=wm_button_measure("Close");
        out[n].x=cw-6*sc-w; out[n].y=y; out[n].w=w; out[n].h=wm_button_h();
        out[n].id=close_id; out[n].enabled=1;
        iv_scopy(out[n].label,"Close",(int)sizeof out[n].label);
        n++;
    }
    return n;
}
static void iv_bar_draw(int cx, int cy, int cw, int ch, const wm_button *b, int n,
                        int hover_id, int press_id)
{
    int sc=gsc();
    draw_hline(cx+4*sc, cy+ch-4*sc-wm_button_h()-4*sc, cw-8*sc, c_border);
    for(int i=0;i<n;i++) wm_button_draw(&b[i], b[i].id==hover_id, b[i].id==press_id);
}
static int iv_bar_hit(const wm_button *b, int n, int mx, int my)
{ for(int i=0;i<n;i++) if(b[i].enabled && wm_button_hit(&b[i],mx,my)) return b[i].id; return 0; }

/* Button ids. */
enum { IVB_UP=1, IVB_OPEN=2,                    /* browse-mode left buttons */
       IVV_LIST=3, IVV_PREV=4, IVV_NEXT=5, IVV_FIT=6, /* preview-mode left  */
       IV_CLOSE=9 };

static int iv_browse_btns(int cw, int ch, wm_button *out)
{
    const int ids[2]={IVB_UP,IVB_OPEN}; const char *lb[2]={"Up","Open"};
    int n=iv_bar(cw,ch,ids,lb,2,IV_CLOSE,out);
    if(n){ out[0].enabled=(g_iv.dir[0]!=0); out[1].enabled=(g_iv.n>0); }
    return n;
}
static int iv_view_btns(int cw, int ch, wm_button *out)
{
    const int ids[4]={IVV_LIST,IVV_PREV,IVV_NEXT,IVV_FIT};
    const char *lb[4]={"List","Prev","Next", g_iv.fit?"1:1":"Fit"};
    return iv_bar(cw,ch,ids,lb,4,IV_CLOSE,out);
}

/* Cached variants: rebuild only when (cw,ch) or the inputs that affect the
 * bar's labels/enabled flags have changed since the last build; otherwise
 * hand back the cached rects. Used on the high-frequency MOUSE_MOVE/DOWN/UP
 * hit-test path (and by the draw callbacks, which are already redraw-rate). */
static int iv_browse_btns_cached(int cw, int ch, wm_button **out)
{
    ivstate *f=&g_iv;
    int dirflag=(f->dir[0]!=0), nflag=(f->n>0);
    if(!f->c_bvalid || f->c_bcw!=cw || f->c_bch!=ch || f->c_bdir!=dirflag || f->c_bn!=nflag){
        f->c_bnb=iv_browse_btns(cw,ch,f->c_bbtns);
        f->c_bcw=cw; f->c_bch=ch; f->c_bdir=dirflag; f->c_bn=nflag; f->c_bvalid=1;
    }
    *out=f->c_bbtns; return f->c_bnb;
}
static int iv_view_btns_cached(int cw, int ch, wm_button **out)
{
    ivstate *f=&g_iv;
    if(!f->c_vvalid || f->c_vcw!=cw || f->c_vch!=ch || f->c_vfit!=f->fit){
        f->c_vnb=iv_view_btns(cw,ch,f->c_vbtns);
        f->c_vcw=cw; f->c_vch=ch; f->c_vfit=f->fit; f->c_vvalid=1;
    }
    *out=f->c_vbtns; return f->c_vnb;
}

/* ==========================================================================
 * File I/O.
 * ========================================================================== */
/* Build the ESP path "<dir>\<name>" (CHAR16) for img read / dir open. */
static void iv_path16(const ivstate *f, const char *name, CHAR16 *out, UINTN cap)
{
    char ap[400]; int p=0;
    for(const char *s=f->dir; *s && p<398; ) ap[p++]=*s++;
    ap[p++]='\\';
    for(const char *s=name; *s && p<399; ) ap[p++]=*s++;
    ap[p]=0;
    esp_ascii_to_char16(ap, out, cap);
}

/* Read a whole file (via root) into a fresh pool buffer (capped). 1 on ok. */
static int iv_read_file(EFI_FILE_PROTOCOL *root, const CHAR16 *path,
                        UINT8 **out, UINTN *outsz)
{
    *out=0; *outsz=0;
    if(!root || !gBS) return 0;
    EFI_FILE_PROTOCOL *fh=NULL;
    if(EFI_ERROR(root->Open(root,&fh,(CHAR16*)path,EFI_FILE_MODE_READ,0)) || !fh) return 0;
    UINT8 info[512]; UINTN isz=sizeof info; UINT64 fsize=0;
    if(!EFI_ERROR(fh->GetInfo(fh,&gFinfoGuid,&isz,info))) fsize=((EFI_FILE_INFO*)info)->FileSize;
    UINTN want=(UINTN)fsize;
    if(want==0){ fh->Close(fh); return 0; }
    if(want>IV_MAXFILE) want=IV_MAXFILE;
    UINT8 *buf=NULL;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,want,(VOID**)&buf)) || !buf){ fh->Close(fh); return 0; }
    UINTN rd=want;
    if(EFI_ERROR(fh->Read(fh,&rd,buf))){ gBS->FreePool(buf); fh->Close(fh); return 0; }
    fh->Close(fh);
    *out=buf; *outsz=rd; return 1;
}

/* List the current directory into name[]/isdir[]/isimg[]/fsize[]. */
static void iv_list(ivstate *f)
{
    int cnt=0;
    EFI_FILE_PROTOCOL *dir=f->root, *opened=NULL;
    if(f->dir[0] && f->root){
        CHAR16 wp[256]; esp_ascii_to_char16(f->dir, wp, 256);
        if(!EFI_ERROR(f->root->Open(f->root,&opened,wp,EFI_FILE_MODE_READ,0)) && opened) dir=opened;
    }
    /* ".." when not at root. */
    if(f->dir[0]){ iv_scopy(f->name[cnt],"..",IV_NAMELEN); f->isdir[cnt]=1; f->isimg[cnt]=0; f->fsize[cnt]=0; cnt++; }

    if(dir){
        for(;;){
            UINT8 ib[1024]; UINTN isz=sizeof(ib);
            EFI_STATUS st=dir->Read(dir,&isz,ib);
            if(EFI_ERROR(st) || isz==0) break;
            EFI_FILE_INFO *fi=(EFI_FILE_INFO*)ib;
            char nm[IV_NAMELEN]; iv_u2a(fi->FileName,nm,IV_NAMELEN);
            if(nm[0]=='.' && nm[1]==0) continue;                 /* "."  */
            if(nm[0]=='.' && nm[1]=='.' && nm[2]==0) continue;   /* ".." handled above */
            if(cnt>=IV_MAXENT) break;
            iv_scopy(f->name[cnt],nm,IV_NAMELEN);
            f->isdir[cnt]=(fi->Attribute&EFI_FILE_DIRECTORY)?1:0;
            f->isimg[cnt]=(!f->isdir[cnt] && iv_is_image(nm))?1:0;
            f->fsize[cnt]=fi->FileSize;
            cnt++;
        }
    }
    if(opened) opened->Close(opened);

    f->n=cnt;
    if(f->sel>=cnt) f->sel=cnt-1; if(f->sel<0) f->sel=0;
    f->scroll=0;
}

static void iv_up(ivstate *f)
{ int i=iv_slen(f->dir); if(i==0) return; if(f->dir[i-1]=='\\')i--;
  while(i>0 && f->dir[i-1]!='\\') i--; if(i>0)i--; f->dir[i]=0; }

/* ==========================================================================
 * Preview.
 * ========================================================================== */
static const char *iv_imgerr(int rc)
{
    switch(rc){
        case IMG_ERR_FORMAT:      return "not a BMP/TGA";
        case IMG_ERR_UNSUPPORTED: return "unsupported (RLE/palette)";
        case IMG_ERR_TRUNCATED:   return "truncated pixel data";
        case IMG_ERR_NOMEM:       return "out of memory";
        case IMG_ERR_ARG:         return "bad / zero-size";
        default:                  return "decode error";
    }
}
static void iv_free_img(ivstate *f){ if(f->have_img){ img_free(&f->img); f->have_img=0; } }

/* Decode entry `idx` into the preview (idx must be an image entry). */
static void iv_show(ivstate *f, int idx)
{
    iv_free_img(f);
    f->err[0]=0; f->vw=f->vh=0;
    f->vidx=idx; f->viewing=1;
    f->pv_valid=0;   /* force a genuine image-area repaint for the new image */
    iv_scopy(f->vname, f->name[idx], IV_NAMELEN);

    CHAR16 wp[400]; iv_path16(f, f->name[idx], wp, 400);
    UINT8 *buf=NULL; UINTN sz=0;
    if(!iv_read_file(f->root, wp, &buf, &sz)){
        int p=0; iv_apps(f->err,&p,(int)sizeof f->err,"cannot read ");
        iv_apps(f->err,&p,(int)sizeof f->err,f->vname); f->err[p]=0;
        return;
    }
    int rc=img_decode(buf, sz, &f->img);
    if(gBS) gBS->FreePool(buf);
    if(rc!=IMG_OK){
        int p=0; iv_apps(f->err,&p,(int)sizeof f->err,"cannot decode ");
        iv_apps(f->err,&p,(int)sizeof f->err,f->vname);
        iv_apps(f->err,&p,(int)sizeof f->err,": ");
        iv_apps(f->err,&p,(int)sizeof f->err,iv_imgerr(rc)); f->err[p]=0;
        return;
    }
    f->have_img=1; f->vw=f->img.w; f->vh=f->img.h;
}

/* Step to the prev/next image entry in the same directory (no wrap). */
static void iv_step(ivstate *f, int dir)
{
    int i=f->vidx;
    for(;;){ i+=dir; if(i<0||i>=f->n) return; if(f->isimg[i]){ f->sel=i; iv_show(f,i); return; } }
}

/* ==========================================================================
 * Draw callbacks.
 * ========================================================================== */
static void iv_draw_browse(int cx, int cy, int cw, int ch)
{
    int sc=gsc(), lineH=iv_lineH_sc(sc);
    int x=cx+6*sc, y=cy+4*sc;

    /* breadcrumb header */
    { char hdr[280]; int p=0;
      iv_apps(hdr,&p,(int)sizeof hdr,"ESP:");
      if(!g_iv.dir[0]) iv_apps(hdr,&p,(int)sizeof hdr,"\\");
      else iv_apps(hdr,&p,(int)sizeof hdr,g_iv.dir);
      hdr[p]=0;
      draw_string(x,y,hdr,c_accent,c_win,1,sc); }
    y+=lineH+3*sc;

    int rows=iv_list_rows_sc(ch,sc), n=g_iv.n, scroll=g_iv.scroll, sel=g_iv.sel;
    if(sel<scroll) scroll=sel; else if(sel>=scroll+rows) scroll=sel-rows+1;
    if(scroll>n-rows) scroll=n-rows; if(scroll<0) scroll=0;
    g_iv.scroll=scroll;

    int barw=(n>rows)?6*sc:0;
    int cols=(cw-14*sc)/(8*sc); if(cols<1)cols=1;   /* loop-invariant: cw,sc fixed for the whole draw */
    for(int r=0;r<rows;r++){
        int idx=scroll+r; if(idx>=n) break;
        int ry=y+r*lineH, selr=(idx==sel);
        UINT32 fg = g_iv.isdir[idx] ? c_accent : (g_iv.isimg[idx] ? c_fg : c_dim);
        if(selr){ fill_rect(x-3*sc,ry-1,cw-10*sc-barw,lineH,c_sel_bg); fg=c_sel_fg; }
        /* compose the row text */
        char ln[128]; int p=0;
        if(g_iv.isdir[idx]){ iv_apps(ln,&p,128,"[DIR] "); iv_apps(ln,&p,128,g_iv.name[idx]); }
        else {
            iv_apps(ln,&p,128, g_iv.isimg[idx] ? "[IMG] " : "      ");
            iv_apps(ln,&p,128,g_iv.name[idx]);
            iv_apps(ln,&p,128,"  (");
            iv_appu(ln,&p,128,g_iv.fsize[idx]); iv_apps(ln,&p,128," B)");
            if(!g_iv.isimg[idx]) iv_apps(ln,&p,128,"  (no preview)");
        }
        ln[p]=0;
        if(cols<p) ln[cols]=0;
        draw_string(x,ry,ln,fg,c_win,1,sc);
    }
    /* scrollbar */
    if(barw){
        int trackX=cx+cw-barw-2*sc, trackH=rows*lineH;
        fill_rect(trackX,y,barw,trackH,c_border);
        int thumbH=rows*trackH/n; if(thumbH<8*sc)thumbH=8*sc;
        int thumbY=y+(n>rows?scroll*(trackH-thumbH)/(n-rows):0);
        fill_rect(trackX,thumbY,barw,thumbH,c_accent);
    }
    /* bottom bar */
    { wm_button *b; int nb=iv_browse_btns_cached(cw,ch,&b);
      if(nb) iv_bar_draw(cx,cy,cw,ch,b,nb,g_iv.b_hover,g_iv.b_press); }
}

static void iv_draw_view(int cx, int cy, int cw, int ch)
{
    int sc=gsc(), lineH=iv_lineH_sc(sc);
    int x=cx+6*sc, y=cy+4*sc;

    /* header: name  WxH  [mode] */
    { char hdr[160]; int p=0;
      iv_apps(hdr,&p,160,g_iv.vname);
      if(g_iv.have_img){
          iv_apps(hdr,&p,160,"  "); iv_appu(hdr,&p,160,(UINT64)g_iv.vw);
          iv_apps(hdr,&p,160,"x");  iv_appu(hdr,&p,160,(UINT64)g_iv.vh);
          iv_apps(hdr,&p,160, g_iv.fit ? "  [fit]" : "  [1:1]");
      }
      hdr[p]=0;
      draw_string(x,y,hdr, g_iv.have_img?c_accent:c_warn, c_win,1,sc); }
    y+=lineH+3*sc;

    /* image area = body, above the bottom bar */
    int ax=cx+4*sc, ay=y;
    int aw=cw-8*sc, ah=iv_content_h_sc(ch,sc)-(ay-cy)-2*sc;
    if(aw<1)aw=1; if(ah<1)ah=1;

    if(g_iv.have_img && g_iv.img.pixels){
        int dw, dh;
        if(g_iv.fit){
            /* aspect-preserving fit within (aw,ah) */
            long a=(long)aw*g_iv.vh, b=(long)ah*g_iv.vw;
            if(a<=b){ dw=aw; dh=(int)((long)aw*g_iv.vh/(g_iv.vw>0?g_iv.vw:1)); }
            else    { dh=ah; dw=(int)((long)ah*g_iv.vw/(g_iv.vh>0?g_iv.vh:1)); }
            if(dw<1)dw=1; if(dh<1)dh=1;
        } else { dw=g_iv.vw; dh=g_iv.vh; }
        int dx=ax+(aw-dw)/2, dy=ay+(ah-dh)/2;
        /* Change-gate: the scaled blit into uncached VRAM dominates a preview
         * frame, and draw_one() never clears the client interior, so when the
         * previewed image, fit mode and target rect all match the last genuine
         * paint the pixels on screen are still correct - skip both the letterbox
         * fill and the blit entirely. pv_valid==0 (set on show/step/fit-toggle)
         * forces one real repaint. */
        int repaint = !g_iv.pv_valid
                    || g_iv.pv_idx!=g_iv.vidx || g_iv.pv_fit!=g_iv.fit
                    || g_iv.pv_ax!=ax || g_iv.pv_ay!=ay
                    || g_iv.pv_aw!=aw || g_iv.pv_ah!=ah;
        if(repaint){
            /* Fill only the four letterbox margin bands around the (clamped)
             * visible image rect instead of the whole aw*ah area - the blit
             * overwrites the interior anyway, so filling it first is pure wasted
             * VRAM writes. The bands collapse to zero size (fill_rect no-ops)
             * when the image covers the area, e.g. an over-size 1:1 image. */
            int vx0=dx<ax?ax:dx, vy0=dy<ay?ay:dy;
            int vx1=(dx+dw>ax+aw)?ax+aw:dx+dw, vy1=(dy+dh>ay+ah)?ay+ah:dy+dh;
            fill_rect(ax, ay, aw, vy0-ay, 0x00101418u);              /* top    */
            fill_rect(ax, vy1, aw, ay+ah-vy1, 0x00101418u);          /* bottom */
            fill_rect(ax, vy0, vx0-ax, vy1-vy0, 0x00101418u);        /* left   */
            fill_rect(vx1, vy0, ax+aw-vx1, vy1-vy0, 0x00101418u);    /* right  */
            /* Clip so an over-size 1:1 image never spills past the area, then
             * hand off to the fast 16.16 clipped scaler (per-row g_swap hoist,
             * word writes, a single ui_mark_dirty) instead of the old per-pixel
             * put_pixel + per-pixel dirty-mark loop. */
            ui_clip_push(ax,ay,aw,ah);
            img_blit_scaled(&g_iv.img, dx,dy,dw,dh);
            ui_clip_pop();
            /* Remember what we just painted so unchanged frames can be skipped. */
            g_iv.pv_idx=g_iv.vidx; g_iv.pv_fit=g_iv.fit;
            g_iv.pv_ax=ax; g_iv.pv_ay=ay; g_iv.pv_aw=aw; g_iv.pv_ah=ah;
            g_iv.pv_valid=1;
        }
    } else {
        fill_rect(ax,ay,aw,ah,0x00101418u);           /* letterbox background */
        const char *msg = g_iv.err[0] ? g_iv.err : "no image";
        draw_string_center(ax+aw/2, ay+ah/2-8*sc, msg, c_warn, c_win, 1, sc);
    }

    /* bottom bar */
    { wm_button *b; int nb=iv_view_btns_cached(cw,ch,&b);
      if(nb) iv_bar_draw(cx,cy,cw,ch,b,nb,g_iv.b_hover,g_iv.b_press); }
}

static void iv_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w; iv_colors();
    if(g_iv.viewing) iv_draw_view(cx,cy,cw,ch);
    else             iv_draw_browse(cx,cy,cw,ch);
}

/* ==========================================================================
 * Interaction.
 * ========================================================================== */
/* Row index under client-relative my (browse list), or -1. */
static int iv_row_at_sc(int my, int rows, int sc)
{
    int y0=iv_body_y0_sc(sc); if(my<y0) return -1;
    int r=(my-y0)/iv_lineH_sc(sc); if(r<0||r>=rows) return -1;
    int idx=g_iv.scroll+r; return (idx<g_iv.n)?idx:-1;
}
static int iv_row_at(int my, int rows){ return iv_row_at_sc(my,rows,gsc()); }

/* Enter / open on the selected browse row. */
static void iv_enter(ivstate *f)
{
    if(f->sel<0 || f->sel>=f->n) return;
    const char *nm=f->name[f->sel];
    if(nm[0]=='.'&&nm[1]=='.'&&nm[2]==0){ iv_up(f); f->sel=0; iv_list(f); return; }
    if(f->isdir[f->sel]){
        int p=iv_slen(f->dir);
        if(p+2+iv_slen(nm)<256){ f->dir[p++]='\\'; iv_scopy(f->dir+p,nm,256-p); }
        f->sel=0; iv_list(f); return;
    }
    if(f->isimg[f->sel]) iv_show(f,f->sel);   /* non-image: silently ignore */
}

static void iv_nav(ivstate *f, UINT16 scan, int rows)
{
    switch(scan){
        case SCAN_UP:        if(f->sel>0)f->sel--; break;
        case SCAN_DOWN:      if(f->sel<f->n-1)f->sel++; break;
        case SCAN_PAGE_UP:   f->sel-=rows-1; if(f->sel<0)f->sel=0; break;
        case SCAN_PAGE_DOWN: f->sel+=rows-1; if(f->sel>f->n-1)f->sel=f->n-1; break;
        case SCAN_HOME:      f->sel=0; break;
        case SCAN_END:       f->sel=f->n-1; if(f->sel<0)f->sel=0; break;
        default: break;
    }
}

static int iv_event_browse(wm_window *w, const wm_event *ev)
{
    ivstate *f=&g_iv;
    int ch=wm_client_h(w), cw=wm_client_w(w);
    int rows=iv_list_rows(ch);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_CR){ iv_enter(f); return 0; }
            if(ev->unicode==CHAR_BACKSPACE || ev->scancode==SCAN_LEFT){ iv_up(f); f->sel=0; iv_list(f); return 0; }
            if(ev->scancode==SCAN_RIGHT){ iv_enter(f); return 0; }
            iv_nav(f,ev->scancode,rows);
            return 0;
        case WM_EV_MOUSE_WHEEL:
            f->sel-=ev->wheel;
            if(f->sel<0)f->sel=0; if(f->sel>f->n-1)f->sel=f->n-1;
            return 0;
        case WM_EV_MOUSE_MOVE: {
            wm_button *b; int nb=iv_browse_btns_cached(cw,ch,&b);
            f->b_hover=iv_bar_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN: {
            wm_button *b; int nb=iv_browse_btns_cached(cw,ch,&b);
            int id=iv_bar_hit(b,nb,ev->mx,ev->my);
            if(id){ f->b_press=id; return 0; }
            int r=iv_row_at(ev->my,rows);
            if(r>=0){ if(r==f->sel) iv_enter(f); else f->sel=r; }
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!f->b_press) return 0;
            wm_button *b; int nb=iv_browse_btns_cached(cw,ch,&b);
            int id=iv_bar_hit(b,nb,ev->mx,ev->my), p=f->b_press;
            f->b_press=0;
            if(id==p){
                if(p==IV_CLOSE) return WM_CLOSE_REQUEST;
                if(p==IVB_UP){ iv_up(f); f->sel=0; iv_list(f); }
                else if(p==IVB_OPEN) iv_enter(f);
            }
            return 0; }
        default: return 0;
    }
}

static int iv_event_view(wm_window *w, const wm_event *ev)
{
    ivstate *f=&g_iv;
    int ch=wm_client_h(w), cw=wm_client_w(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC){ iv_free_img(f); f->viewing=0; return 0; }
            if(ev->scancode==SCAN_LEFT || ev->scancode==SCAN_PAGE_UP){ iv_step(f,-1); return 0; }
            if(ev->scancode==SCAN_RIGHT|| ev->scancode==SCAN_PAGE_DOWN){ iv_step(f,+1); return 0; }
            if(ev->unicode=='f' || ev->unicode=='F' || ev->unicode==' ' || ev->unicode==CHAR_CR){ f->fit=!f->fit; f->pv_valid=0; return 0; }
            return 0;
        case WM_EV_MOUSE_WHEEL:
            iv_step(f, ev->wheel>0 ? -1 : +1);
            return 0;
        case WM_EV_MOUSE_MOVE: {
            wm_button *b; int nb=iv_view_btns_cached(cw,ch,&b);
            f->b_hover=iv_bar_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN: {
            wm_button *b; int nb=iv_view_btns_cached(cw,ch,&b);
            int id=iv_bar_hit(b,nb,ev->mx,ev->my);
            if(id) f->b_press=id;
            return 0; }
        case WM_EV_MOUSE_UP: {
            if(!f->b_press) return 0;
            wm_button *b; int nb=iv_view_btns_cached(cw,ch,&b);
            int id=iv_bar_hit(b,nb,ev->mx,ev->my), p=f->b_press;
            f->b_press=0;
            if(id==p){
                if(p==IV_CLOSE) return WM_CLOSE_REQUEST;
                if(p==IVV_LIST){ iv_free_img(f); f->viewing=0; }
                else if(p==IVV_PREV) iv_step(f,-1);
                else if(p==IVV_NEXT) iv_step(f,+1);
                else if(p==IVV_FIT){ f->fit=!f->fit; f->pv_valid=0; }
            }
            return 0; }
        default: return 0;
    }
}

static int iv_event(wm_window *w, const wm_event *ev)
{
    if(ev->type==WM_EV_CLOSE){
        iv_free_img(&g_iv);
        g_iv.win=NULL;
        if(g_iv.root){ g_iv.root->Close(g_iv.root); g_iv.root=NULL; }
        return 0;
    }
    return g_iv.viewing ? iv_event_view(w,ev) : iv_event_browse(w,ev);
}

/* ==========================================================================
 * Open.
 * ========================================================================== */
void tool_imgview_open(void)
{
    if(g_iv.win) return;
    if(gBS) gBS->SetMem(&g_iv,sizeof(g_iv),0);
    else { char *p=(char*)&g_iv; for(unsigned i=0;i<sizeof(g_iv);i++) p[i]=0; }
    g_iv.fit=1; g_iv.sel=0;

    EFI_FILE_PROTOCOL *root=NULL;
    if(!EFI_ERROR(esp_open_root(gImage,gBS,&root))) g_iv.root=root;
    if(g_iv.root) iv_list(&g_iv);
    /* (no root -> n==0, browse view shows an empty list; still openable) */

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*60/100; if(ww<480)ww=480; if(ww>960)ww=960; if(ww>W-40)ww=W-40;
    int wh=H*66/100; if(wh<340)wh=340; if(wh>720)wh=720; if(wh>H-40)wh=H-40;
    g_iv.win=wm_open("Image Viewer", ww, wh, iv_draw, iv_event, &g_iv);
    if(!g_iv.win && g_iv.root){ g_iv.root->Close(g_iv.root); g_iv.root=NULL; }
}

/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/clone.c - drive CLONE tool + its GUI window.
 * =============================================================================
 * See clone.h for the contract. A "template B" wm window that:
 *   - enumerates block devices with diskio_enumerate(),
 *   - lets the user pick a SOURCE then a DESTINATION (Up/Down + Enter), or flip
 *     to "clone to FILE" mode (writes \forebo\clone.img on the ESP),
 *   - guards dest != source and dest capacity >= source (warns, never truncates),
 *   - after an explicit on-screen CONFIRM, copies the source with diskio_read()
 *     (corruption-tolerant) in 256-block chunks, writing to the destination
 *     EFI_BLOCK_IO_PROTOCOL->WriteBlocks (or the image file), showing a live
 *     progress bar with % + MB copied + bad-sector count,
 *   - prints a final summary (blocks ok / bad tolerated / MB).
 *
 * The copy runs SYNCHRONOUSLY from the event callback once confirmed (it is the
 * only long-running action; pre-ExitBootServices this is fine). It pumps
 * ui_progress()/ui_status()/ui_present() each chunk so the bar animates, and
 * polls ConIn so ESC aborts mid-copy. Everything else is pure per-frame draw.
 *
 * Freestanding (no libc). Heap only via one BootServices AllocatePool chunk
 * buffer with a matching FreePool. Guards every pointer.
 * ========================================================================== */

#include "clone.h"
#include "../efi.h"
#include "../efi_ext.h"
#include "../core/wm.h"
#include "../ui.h"
#include "../core/input.h"
#include "../core/config.h"          /* esp_open_root / esp_ascii_to_char16 */
#include "../core/diskio.h"
#include "../../include/forebo_theme.h"

/* ---- optional serial debug (0x3F8), off by default ----------------------- */
#ifndef CLONE_DEBUG
#define CLONE_DEBUG 0
#endif
#if CLONE_DEBUG
static inline void cl_outb(unsigned short port, unsigned char v)
{ __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(port)); }
static void cl_dlog(const char *s){ if(!s)return; while(*s) cl_outb(0x3F8,(unsigned char)*s++); }
#else
static void cl_dlog(const char *s){ (void)s; }
#endif

/* ==========================================================================
 * Module state (captured at tool_clone_init).
 * ========================================================================== */
static EFI_HANDLE         gImage;
static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;

void tool_clone_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    gImage = image;
    gST    = st;
    gBS    = st ? st->BootServices : 0;
    if(gST) diskio_init(gST);          /* idempotent; safe to call again */
}

/* ==========================================================================
 * Tiny freestanding helpers.
 * ========================================================================== */
static void cl_scopy(char *d, const char *s, int cap)
{ int i=0; if(cap<=0)return; for(;s&&s[i]&&i+1<cap;i++)d[i]=s[i]; d[i]=0; }
/* Append unsigned decimal to d (NUL-terminated), returns new length. */
static int cl_au(char *d, int len, int cap, UINT64 v)
{
    char t[24]; int i=0;
    if(!v){ if(len+1<cap)d[len++]='0'; d[len]=0; return len; }
    while(v){ t[i++]=(char)('0'+(int)(v%10)); v/=10; }
    while(i>0 && len+1<cap) d[len++]=t[--i];
    d[len]=0; return len;
}
static int cl_as(char *d, int len, int cap, const char *s)
{ while(s&&*s && len+1<cap) d[len++]=*s++; d[len]=0; return len; }

static int cl_sc(void){ int s=ui_scale(); return s<1?1:s; }

/* ==========================================================================
 * Tool state.
 * ========================================================================== */
#define CL_MAXDEV        16
#define CL_CHUNK_BLOCKS  256          /* logical blocks read/written per chunk */

enum { CL_PICK_SRC=0, CL_PICK_DST, CL_CONFIRM, CL_DONE };

typedef struct {
    wm_window        *win;
    struct diskio_dev dev[CL_MAXDEV];
    int               ndev;
    int               phase;
    int               sel;            /* highlighted device row              */
    int               scroll;         /* first visible device row            */
    int               src;            /* chosen source idx, -1 = none        */
    int               dst;            /* chosen dest idx, -1 = none/file     */
    int               to_file;        /* 1 = clone SOURCE -> \forebo\clone.img */
    char              warn[96];       /* transient status / warning line     */
    /* run summary */
    int               ran, aborted, failed;
    UINT64            sum_ok, sum_bad, sum_bytes;
    /* button-bar hover / pressed id */
    int               b_hover, b_press;
} clonestate;

static clonestate g_clone;

/* ==========================================================================
 * Bottom button bar (wm_button widget). Geometry lives ONLY here so draw +
 * hit-test agree. Button set depends on the current phase.
 * ========================================================================== */
enum {
    CL_B_TOFILE = 1,   /* toggle device/file destination (pick-src phase) */
    CL_B_SELECT,       /* choose the highlighted device                   */
    CL_B_BACK,         /* pick-dst / confirm -> previous phase            */
    CL_B_CONFIRM,      /* start the destructive copy                      */
    CL_B_AGAIN,        /* done -> start over                              */
    CL_B_CLOSE         /* close the window                                */
};

/* Fill out[] with the buttons for the current phase; returns the count.
 * Left-aligned action buttons then a right-aligned [Close].
 * with_labels: 1 = copy each label string (draw path), 0 = geometry/id only
 * (mouse hit-test paths never read .label, so skip the copies). */
static int cl_bar(const clonestate *c, int cw, int ch, wm_button *out, int with_labels)
{
    int sc=cl_sc(), bh=wm_button_h();
    int y=ch-4*sc-bh, x=8*sc, n=0;
    struct { int id; const char *lb; } act[3]; int na=0;

    switch(c->phase){
        case CL_PICK_SRC:
            act[na].id=CL_B_TOFILE; act[na].lb=c->to_file?"Dest: FILE":"Dest: DEVICE"; na++;
            act[na].id=CL_B_SELECT; act[na].lb="Select"; na++;
            break;
        case CL_PICK_DST:
            act[na].id=CL_B_BACK;   act[na].lb="Back";   na++;
            act[na].id=CL_B_SELECT; act[na].lb="Select"; na++;
            break;
        case CL_CONFIRM:
            act[na].id=CL_B_CONFIRM; act[na].lb="CONFIRM"; na++;
            act[na].id=CL_B_BACK;    act[na].lb="Cancel";  na++;
            break;
        case CL_DONE:
            act[na].id=CL_B_AGAIN;  act[na].lb="Again";  na++;
            break;
        default: break;
    }
    for(int i=0;i<na;i++){
        int bw=wm_button_measure(act[i].lb);
        out[n].x=x; out[n].y=y; out[n].w=bw; out[n].h=bh; out[n].id=act[i].id; out[n].enabled=1;
        if(with_labels) cl_scopy(out[n].label, act[i].lb, (int)sizeof(out[n].label));
        x+=bw+6*sc; n++;
    }
    /* right-aligned Close */
    { int bw=wm_button_measure("Close");
      out[n].x=cw-8*sc-bw; out[n].y=y; out[n].w=bw; out[n].h=bh;
      out[n].id=CL_B_CLOSE; out[n].enabled=1;
      if(with_labels) cl_scopy(out[n].label,"Close",(int)sizeof(out[n].label)); n++; }
    return n;
}

/* First enabled button under (mx,my) -> id, else 0. */
static int cl_bar_hit(const wm_button *b, int n, int mx, int my)
{ for(int i=0;i<n;i++) if(b[i].enabled && wm_button_hit(&b[i],mx,my)) return b[i].id; return 0; }

/* ==========================================================================
 * Device enumeration.
 * ========================================================================== */
static void cl_reload(clonestate *c)
{
    c->ndev = diskio_enumerate(c->dev, CL_MAXDEV);
    if(c->ndev<0) c->ndev=0;
    if(c->sel>=c->ndev) c->sel=c->ndev>0?c->ndev-1:0;
}

/* ==========================================================================
 * Draw.
 * ========================================================================== */
static int cl_list_rows(int ch)
{
    int sc=cl_sc(), lineH=16*sc;
    int top=6*sc + (16*sc+8*sc);            /* header + hint */
    int bot=ch - (wm_button_h()+10*sc);     /* above button bar */
    int r=(bot-top)/lineH; return r<1?1:r;
}

static void cl_dev_line(const struct diskio_dev *d, int idx, char *out, int cap)
{
    int p=0;
    out[0]=0;
    p=cl_as(out,p,cap,"  [");
    p=cl_au(out,p,cap,(UINT64)idx);
    p=cl_as(out,p,cap,"] ");
    p=cl_as(out,p,cap,d->label[0]?d->label:"(device)");
    /* clarify whole-disk vs partition (label already hints part/disk) */
    if(d->logical_partition) p=cl_as(out,p,cap,"  (partition)");
    (void)p;
}

static void cl_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    clonestate *c=&g_clone;
    UINT32 win   = wm_theme_color(WM_COL_WINDOW);
    UINT32 fg    = wm_theme_color(WM_COL_FG);
    UINT32 accent= wm_theme_color(WM_COL_ACCENT);
    UINT32 selbg = wm_theme_color(WM_COL_SEL_BG);
    UINT32 selfg = wm_theme_color(WM_COL_SEL_FG);
    UINT32 dim   = FOREB_DIM;
    UINT32 warn  = FOREB_TIMER;
    UINT32 border= FOREB_BORDER;
    int sc=cl_sc(), lineH=16*sc;
    int x=cx+8*sc, y=cy+6*sc;

    /* header + phase hint */
    const char *hdr =
        c->phase==CL_PICK_SRC ? "Clone: choose SOURCE (read-only)" :
        c->phase==CL_PICK_DST ? "Clone: choose DESTINATION (will be OVERWRITTEN)" :
        c->phase==CL_CONFIRM  ? "Clone: CONFIRM - this ERASES the destination" :
                                "Clone: finished";
    draw_string(x,y,hdr,accent,win,1,sc);
    y+=lineH;
    const char *hint =
        c->phase==CL_PICK_SRC ? "Up/Down + Enter select, 'f' toggles file/device dest, Esc closes" :
        c->phase==CL_PICK_DST ? "Up/Down + Enter select destination, Back returns" :
        c->phase==CL_CONFIRM  ? "Press CONFIRM (or 'y') to start, Cancel/'n' to go back" :
                                "Enter/Again to clone another, Esc closes";
    draw_string(x,y,hint,dim,win,1,sc);
    y+=lineH+2*sc;

    if(c->phase==CL_CONFIRM){
        /* Summary of what is about to happen. */
        char b[120]; int p;
        struct diskio_dev *s=(c->src>=0&&c->src<c->ndev)?&c->dev[c->src]:0;
        p=0; p=cl_as(b,p,(int)sizeof(b),"SOURCE : "); p=cl_as(b,p,(int)sizeof(b), s?(s->label[0]?s->label:"(device)"):"?");
        draw_string(x,y,b,fg,win,1,sc); y+=lineH;
        p=0;
        if(c->to_file){ p=cl_as(b,p,(int)sizeof(b),"DEST   : file  \\forebo\\clone.img (ESP)"); }
        else{
            struct diskio_dev *d=(c->dst>=0&&c->dst<c->ndev)?&c->dev[c->dst]:0;
            p=cl_as(b,p,(int)sizeof(b),"DEST   : "); p=cl_as(b,p,(int)sizeof(b), d?(d->label[0]?d->label:"(device)"):"?");
        }
        draw_string(x,y,b,warn,win,1,sc); y+=lineH;
        p=0; p=cl_as(b,p,(int)sizeof(b),"Copy   : "); p=cl_au(b,p,(int)sizeof(b), s?(s->total_bytes>>20):0);
        p=cl_as(b,p,(int)sizeof(b)," MiB, corruption-tolerant (bad sectors zero-filled)");
        draw_string(x,y,b,dim,win,1,sc); y+=lineH;
    } else if(c->phase==CL_DONE){
        char b[120]; int p;
        p=0; p=cl_as(b,p,(int)sizeof(b), c->failed?"Result : FAILED" : c->aborted?"Result : ABORTED":"Result : OK");
        draw_string(x,y, b, c->failed?warn:(c->aborted?warn:accent), win,1,sc); y+=lineH;
        p=0; p=cl_as(b,p,(int)sizeof(b),"Copied : "); p=cl_au(b,p,(int)sizeof(b),c->sum_bytes>>20); p=cl_as(b,p,(int)sizeof(b)," MiB");
        draw_string(x,y,b,fg,win,1,sc); y+=lineH;
        p=0; p=cl_as(b,p,(int)sizeof(b),"Blocks : "); p=cl_au(b,p,(int)sizeof(b),c->sum_ok);
        p=cl_as(b,p,(int)sizeof(b)," ok, "); p=cl_au(b,p,(int)sizeof(b),c->sum_bad); p=cl_as(b,p,(int)sizeof(b)," bad tolerated");
        draw_string(x,y,b, c->sum_bad?warn:fg, win,1,sc); y+=lineH;
    } else {
        /* device list (pick src / pick dst) */
        int rows=cl_list_rows(ch);
        if(c->sel<c->scroll) c->scroll=c->sel;
        else if(c->sel>=c->scroll+rows) c->scroll=c->sel-rows+1;
        if(c->scroll>c->ndev-rows) c->scroll=c->ndev-rows;
        if(c->scroll<0) c->scroll=0;

        if(c->ndev==0){
            draw_string(x,y,"No block devices found.",warn,win,1,sc); y+=lineH;
        }
        for(int r=0;r<rows;r++){
            int idx=c->scroll+r; if(idx>=c->ndev) break;
            int ry=y+r*lineH;
            int chosen_src=(idx==c->src);
            int is_sel=(idx==c->sel);
            UINT32 tf=fg;
            if(is_sel){ fill_rect(x-4*sc,ry-1,cw-16*sc,lineH,selbg); tf=selfg; }
            else if(chosen_src) tf=accent;
            char b[120]; cl_dev_line(&c->dev[idx],idx,b,(int)sizeof(b));
            draw_string(x,ry,b,tf,win,1,sc);
            if(c->phase==CL_PICK_DST && chosen_src)
                draw_string(cx+cw-14*sc-8*8*sc,ry,"[SOURCE]",warn,win,1,sc);
        }
        /* scrollbar */
        if(c->ndev>rows){
            int barw=6*sc, trackX=cx+cw-barw-3*sc, trackH=rows*lineH;
            fill_rect(trackX,y,barw,trackH,border);
            int thumbH=rows*trackH/c->ndev; if(thumbH<8*sc)thumbH=8*sc;
            int thumbY=y+c->scroll*(trackH-thumbH)/(c->ndev-rows);
            fill_rect(trackX,thumbY,barw,thumbH,accent);
        }
    }

    /* transient warning / status line just above the bar */
    if(c->warn[0]){
        int wy=cy+ch-(wm_button_h()+10*sc)-lineH;
        draw_string(x,wy,c->warn,warn,win,1,sc);
    }

    /* button bar */
    { wm_button b[4]; int nb=cl_bar(c,cw,ch,b,1);
      draw_hline(cx+4*sc, cy+ch-4*sc-wm_button_h()-4*sc, cw-8*sc, border);
      for(int i=0;i<nb;i++) wm_button_draw(&b[i], b[i].id==c->b_hover, b[i].id==c->b_press); }
}

/* ==========================================================================
 * Progress overlay (drawn during the synchronous copy). Uses the fixed-position
 * ui_progress()/ui_status() region + ui_present(), so it needs no window coords.
 * ========================================================================== */
static void cl_progress(UINT64 copied, UINT64 total, UINT64 bad)
{
    char lab[80]; int p=0;
    p=cl_as(lab,p,(int)sizeof(lab),"Cloning  ");
    p=cl_au(lab,p,(int)sizeof(lab),copied>>20);
    p=cl_as(lab,p,(int)sizeof(lab)," / ");
    p=cl_au(lab,p,(int)sizeof(lab),total>>20);
    p=cl_as(lab,p,(int)sizeof(lab)," MiB   (Esc aborts)");
    ui_progress(lab, copied, total);

    char st[64]; p=0;
    p=cl_as(st,p,(int)sizeof(st),"bad sectors tolerated: ");
    p=cl_au(st,p,(int)sizeof(st),bad);
    ui_status(st);
    ui_present();
}

/* ==========================================================================
 * The actual clone. Synchronous; runs after CONFIRM. Read-only on the source.
 * ========================================================================== */
static int cl_esc_pressed(void)
{
    if(!gST || !gST->ConIn) return 0;
    EFI_INPUT_KEY k;
    if(EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn,&k))) return 0;
    return k.ScanCode==SCAN_ESC;
}

static void cl_run(clonestate *c)
{
    c->warn[0]=0; c->ran=1; c->aborted=0; c->failed=0;
    c->sum_ok=0; c->sum_bad=0; c->sum_bytes=0;

    if(!gBS){ cl_scopy(c->warn,"Not initialised (no BootServices)",96); c->failed=1; c->phase=CL_DONE; return; }
    if(c->src<0 || c->src>=c->ndev){ cl_scopy(c->warn,"No source selected",96); c->failed=1; c->phase=CL_DONE; return; }

    struct diskio_dev *s=&c->dev[c->src];
    if(!s->bio){ cl_scopy(c->warn,"Source has no Block IO",96); c->failed=1; c->phase=CL_DONE; return; }
    UINT32 sbs = s->block_size?s->block_size:512;
    UINT64 total_blocks = s->last_lba+1;
    UINT64 total_bytes  = s->total_bytes ? s->total_bytes : total_blocks*(UINT64)sbs;
    UINTN  chunk_bytes  = (UINTN)CL_CHUNK_BLOCKS*sbs;

    /* ---- set up destination ------------------------------------------- */
    struct diskio_dev *d=0; UINT32 dbs=0;
    EFI_FILE_PROTOCOL *root=0, *fh=0;

    if(c->to_file){
        if(EFI_ERROR(esp_open_root(gImage,gBS,&root))||!root){
            cl_scopy(c->warn,"Cannot open ESP for clone.img",96); c->failed=1; c->phase=CL_DONE; return; }
        /* best-effort create the \forebo directory, then the file */
        CHAR16 wdir[16]; esp_ascii_to_char16("\\forebo", wdir, 16);
        EFI_FILE_PROTOCOL *dir=0;
        if(!EFI_ERROR(root->Open(root,&dir,wdir,
                EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,
                EFI_FILE_DIRECTORY)) && dir) dir->Close(dir);
        CHAR16 wp[32]; esp_ascii_to_char16("\\forebo\\clone.img", wp, 32);
        if(EFI_ERROR(root->Open(root,&fh,wp,
                EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0)) || !fh){
            root->Close(root);
            cl_scopy(c->warn,"Cannot create \\forebo\\clone.img",96); c->failed=1; c->phase=CL_DONE; return; }
    } else {
        if(c->dst<0 || c->dst>=c->ndev){ cl_scopy(c->warn,"No destination selected",96); c->failed=1; c->phase=CL_DONE; return; }
        d=&c->dev[c->dst];
        if(!d->bio){ cl_scopy(c->warn,"Destination has no Block IO",96); c->failed=1; c->phase=CL_DONE; return; }
        dbs = d->block_size?d->block_size:512;
        if(d->bio->Media && d->bio->Media->ReadOnly){
            cl_scopy(c->warn,"Destination is read-only",96); c->failed=1; c->phase=CL_DONE; return; }
        if(d->total_bytes < total_bytes){
            cl_scopy(c->warn,"Destination smaller than source",96); c->failed=1; c->phase=CL_DONE; return; }
        /* keep every chunk write aligned to whole destination blocks */
        if(chunk_bytes % dbs != 0){
            cl_scopy(c->warn,"Incompatible block sizes",96); c->failed=1; c->phase=CL_DONE; return; }
    }

    UINT8 *buf=0;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,chunk_bytes,(VOID**)&buf))||!buf){
        if(fh) fh->Close(fh); if(root) root->Close(root);
        cl_scopy(c->warn,"Out of memory for copy buffer",96); c->failed=1; c->phase=CL_DONE; return;
    }

    cl_dlog("clone: start\r\n");
    UINT64 copied=0, ok=0, bad=0, lba=0;
    int aborted=0, failed=0;

    while(lba<total_blocks){
        UINT32 n=CL_CHUNK_BLOCKS;
        if((UINT64)n > total_blocks-lba) n=(UINT32)(total_blocks-lba);

        struct diskio_read_stat rst;
        int r=diskio_read(s, lba, n, buf, &rst);
        if(r<0){ cl_scopy(c->warn,"Fatal read error on source",96); failed=1; break; }
        ok += rst.blocks_ok; bad += rst.blocks_bad;

        UINTN bytes_this=(UINTN)n*sbs;

        if(c->to_file){
            UINTN wn=bytes_this;
            if(EFI_ERROR(fh->Write(fh,&wn,buf)) || wn!=bytes_this){
                cl_scopy(c->warn,"Write error on clone.img",96); failed=1; break; }
        } else {
            /* pad the (possibly ragged) final chunk up to a whole dest block */
            UINTN wbytes=bytes_this;
            if(wbytes % dbs){
                UINTN pad=dbs-(wbytes%dbs);
                for(UINTN i=0;i<pad && wbytes+i<chunk_bytes;i++) buf[wbytes+i]=0;
                wbytes+=pad;
            }
            UINT64 dlba=copied/dbs;                 /* copied is dbs-aligned each iter */
            if(EFI_ERROR(d->bio->WriteBlocks(d->bio,d->media_id,dlba,wbytes,buf))){
                cl_scopy(c->warn,"Write error on destination",96); failed=1; break; }
        }

        copied += bytes_this;
        lba    += n;

        cl_progress(copied,total_bytes,bad);
        if(cl_esc_pressed()){ aborted=1; break; }
    }

    /* flush + release */
    if(c->to_file){ if(fh){ fh->Flush(fh); fh->Close(fh);} if(root) root->Close(root); }
    else if(d && d->bio->FlushBlocks) d->bio->FlushBlocks(d->bio);
    gBS->FreePool(buf);

    c->sum_ok=ok; c->sum_bad=bad; c->sum_bytes=copied;
    c->aborted=aborted; c->failed=failed;
    if(!failed && !aborted && !c->warn[0]) cl_scopy(c->warn,"Clone complete",96);
    c->phase=CL_DONE;
    cl_dlog("clone: done\r\n");
}

/* ==========================================================================
 * Phase transitions from the picker.
 * ========================================================================== */
static void cl_select(clonestate *c)
{
    c->warn[0]=0;
    if(c->sel<0 || c->sel>=c->ndev){ cl_scopy(c->warn,"No device",96); return; }
    if(c->phase==CL_PICK_SRC){
        c->src=c->sel;
        if(c->to_file){ c->dst=-1; c->phase=CL_CONFIRM; }
        else { c->dst=-1; c->phase=CL_PICK_DST; c->sel=0; c->scroll=0; }
        return;
    }
    if(c->phase==CL_PICK_DST){
        if(c->sel==c->src){ cl_scopy(c->warn,"Destination must differ from source",96); return; }
        if(c->dev[c->sel].total_bytes < c->dev[c->src].total_bytes){
            cl_scopy(c->warn,"Destination too small - refusing to truncate",96); return; }
        c->dst=c->sel; c->phase=CL_CONFIRM;
        return;
    }
}

static void cl_reset(clonestate *c)
{
    c->phase=CL_PICK_SRC; c->src=-1; c->dst=-1; c->sel=0; c->scroll=0;
    c->ran=0; c->aborted=0; c->failed=0; c->warn[0]=0;
    cl_reload(c);
}

static void cl_do_button(clonestate *c, int id)
{
    switch(id){
        case CL_B_TOFILE:  if(c->phase==CL_PICK_SRC){ c->to_file=!c->to_file; c->warn[0]=0; } break;
        case CL_B_SELECT:  cl_select(c); break;
        case CL_B_BACK:
            c->warn[0]=0;
            if(c->phase==CL_PICK_DST){ c->phase=CL_PICK_SRC; c->sel=(c->src>=0?c->src:0); }
            else if(c->phase==CL_CONFIRM){
                if(c->to_file){ c->phase=CL_PICK_SRC; c->sel=(c->src>=0?c->src:0); }
                else { c->phase=CL_PICK_DST; c->sel=(c->dst>=0?c->dst:0); }
            }
            break;
        case CL_B_CONFIRM: if(c->phase==CL_CONFIRM) cl_run(c); break;
        case CL_B_AGAIN:   cl_reset(c); break;
        default: break;
    }
}

/* ==========================================================================
 * Event callback.
 * ========================================================================== */
static int cl_event(wm_window *w, const wm_event *ev)
{
    clonestate *c=&g_clone;
    int cw=wm_client_w(w), ch=wm_client_h(w);

    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(c->phase==CL_PICK_SRC || c->phase==CL_PICK_DST){
                if(ev->scancode==SCAN_UP){ if(c->sel>0)c->sel--; }
                else if(ev->scancode==SCAN_DOWN){ if(c->sel<c->ndev-1)c->sel++; }
                else if(ev->scancode==SCAN_HOME) c->sel=0;
                else if(ev->scancode==SCAN_END)  c->sel=c->ndev>0?c->ndev-1:0;
                else if(ev->unicode==CHAR_CR) cl_select(c);
                else if((ev->unicode=='f'||ev->unicode=='F') && c->phase==CL_PICK_SRC)
                    { c->to_file=!c->to_file; c->warn[0]=0; }
                else if((ev->unicode=='b'||ev->unicode=='B') && c->phase==CL_PICK_DST)
                    cl_do_button(c,CL_B_BACK);
            } else if(c->phase==CL_CONFIRM){
                if(ev->unicode=='y'||ev->unicode=='Y'||ev->unicode==CHAR_CR) cl_run(c);
                else if(ev->unicode=='n'||ev->unicode=='N') cl_do_button(c,CL_B_BACK);
            } else if(c->phase==CL_DONE){
                if(ev->unicode==CHAR_CR) cl_reset(c);
            }
            return 0;

        case WM_EV_MOUSE_WHEEL:
            if(c->phase==CL_PICK_SRC || c->phase==CL_PICK_DST){
                c->sel-=ev->wheel;
                if(c->sel<0)c->sel=0; if(c->sel>c->ndev-1)c->sel=c->ndev>0?c->ndev-1:0;
            }
            return 0;

        case WM_EV_MOUSE_MOVE: {
            wm_button b[4]; int nb=cl_bar(c,cw,ch,b,0);
            c->b_hover=cl_bar_hit(b,nb,ev->mx,ev->my);
            return 0; }

        case WM_EV_MOUSE_DOWN: {
            wm_button b[4]; int nb=cl_bar(c,cw,ch,b,0);
            int id=cl_bar_hit(b,nb,ev->mx,ev->my);
            if(id){ c->b_press=id; return 0; }
            /* click a device row (picker phases) */
            if(c->phase==CL_PICK_SRC || c->phase==CL_PICK_DST){
                int sc=cl_sc(), lineH=16*sc;
                int top=6*sc+(16*sc+8*sc)+2*sc;
                if(ev->my>=top){
                    int r=(ev->my-top)/lineH, idx=c->scroll+r;
                    if(r>=0 && idx<c->ndev){
                        if(idx==c->sel) cl_select(c); else c->sel=idx;
                    }
                }
            }
            return 0; }

        case WM_EV_MOUSE_UP: {
            if(!c->b_press) return 0;
            wm_button b[4]; int nb=cl_bar(c,cw,ch,b,0);
            int id=cl_bar_hit(b,nb,ev->mx,ev->my), p=c->b_press;
            c->b_press=0;
            if(id==p){
                if(p==CL_B_CLOSE) return WM_CLOSE_REQUEST;
                cl_do_button(c,p);
            }
            return 0; }

        case WM_EV_CLOSE:
            c->win=NULL;
            return 0;

        default: return 0;
    }
}

/* ==========================================================================
 * Open (template B).
 * ========================================================================== */
void tool_clone_open(void)
{
    clonestate *c=&g_clone;
    if(c->win) return;                 /* already open */

    if(gST) diskio_init(gST);          /* in case init order differs; idempotent */

    if(gBS) gBS->SetMem(c,sizeof(*c),0);
    else { char *p=(char*)c; for(unsigned i=0;i<sizeof(*c);i++) p[i]=0; }
    c->phase=CL_PICK_SRC; c->src=-1; c->dst=-1; c->sel=0;
    cl_reload(c);
    if(!gBS) cl_scopy(c->warn,"Not initialised - call tool_clone_init()",96);

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*60/100; if(ww<520)ww=520; if(ww>880)ww=880; if(ww>W-40)ww=W-40;
    int wh=H*56/100; if(wh<320)wh=320; if(wh>600)wh=600; if(wh>H-40)wh=H-40;
    c->win=wm_open("Clone Drive", ww, wh, cl_draw, cl_event, c);
}

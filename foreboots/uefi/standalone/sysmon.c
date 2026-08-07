/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/sysmon.c - System Monitor tool (live RAM / GOP / firmware / uptime).
 * =============================================================================
 * Implements sysmon.h. "Template B" window: tool_sysmon_open() opens a wm.c
 * window and returns; the bootx64.c menu loop drives it. The draw callback
 * re-polls a cheap snapshot every SM_REFRESH frames (RAM breakdown via
 * GetMemoryMap, GOP mode, firmware/SecureBoot, block-device count) into the
 * per-window static state, then paints gauges (fill_rect) + clipped labels
 * (draw_string_clip). Every source is guarded; a missing one shows "N/A".
 *
 * Freestanding C11, no libc, integer math only (build is -mno-sse -mno-mmx, so
 * NO float/double). Heap only via BootServices AllocatePool for a scratch
 * memory-map buffer, freed immediately after each poll.
 * ========================================================================== */

#include "sysmon.h"
#include "../efi.h"
#include "../core/wm.h"
#include "../ui.h"
#include "../core/diskio.h"
#include "../../include/forebo_theme.h"

/* ==========================================================================
 * Module state (captured at tool_sysmon_init).
 * ========================================================================== */
static EFI_SYSTEM_TABLE     *gST;
static EFI_BOOT_SERVICES    *gBS;
static EFI_RUNTIME_SERVICES *gRT;

static EFI_GUID gGopGuid    = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID gGlobalVar  = EFI_GLOBAL_VARIABLE;

/* GetTime is a VOID* placeholder in EFI_RUNTIME_SERVICES; cast to call it. */
typedef EFI_STATUS (EFIAPI *sm_get_time_fn)(EFI_TIME *Time, VOID *Caps);

/* Re-gather the snapshot every this many composited frames. */
#define SM_REFRESH   30
/* RAM breakdown (GetMemoryMap + AllocatePool) is far heavier and barely
 * changes at runtime, so re-poll it on a much coarser cadence than the rest. */
#define SM_RAM_REFRESH (4*SM_REFRESH)
/* Rough compositor cadence used only for the uptime fallback label. */
#define SM_EST_FPS   30

/* ==========================================================================
 * Per-window live snapshot (reached from callbacks via wm_user()).
 * ========================================================================== */
typedef struct {
    wm_window *win;
    UINT64     frames;          /* monotonic composited-frame counter        */
    UINT64     last_poll;       /* frame index of the last re-gather         */
    UINT64     last_ram_poll;   /* frame index of the last RAM re-gather     */

    /* RAM (MiB), derived from GetMemoryMap page sums.                        */
    int  ram_ok;
    UINT64 ram_total_mib;       /* physical RAM (all types except MMIO)      */
    UINT64 ram_conv_mib;        /* EfiConventionalMemory (free)              */
    UINT64 ram_reserved_mib;
    UINT64 ram_acpi_mib;
    UINT64 ram_mmio_mib;
    UINT64 ram_used_mib;        /* ram_total - conventional                  */
    UINT64 mem_descriptors;

    /* GOP.                                                                   */
    int  gop_ok;
    UINT32 gop_w, gop_h, gop_pitch;
    const char *gop_fmt;
    UINT64 gop_fb_mib;

    /* Firmware.                                                              */
    int  fw_ok;
    char fw_vendor[64];
    UINT32 fw_rev;
    UINT16 uefi_major, uefi_minor;
    int  secure_boot;           /* -1 N/A, 0 off, 1 on                       */

    /* Pre-formatted label lines, rebuilt only in sm_gather_all() (once per
     * SM_REFRESH frames) since the underlying values don't change between
     * polls; sm_draw() just draws these cached strings every frame.        */
    char ram_line1[64], ram_line2[64], ram_line3[80];
    char gop_line[96];
    char fw_line1[96], fw_line2[64], sb_line[40];

    /* Uptime.                                                               */
    int  rtc_ok;               /* 1 when RTC start time was captured         */
    UINT32 start_daysec;        /* Hour*3600+Min*60+Sec at open              */
    UINT32 start_day;           /* Day-of-month at open (coarse wrap fix)    */
    UINT64 uptime_sec;
    int    rtc_sampled;         /* 1 once the first throttled RTC sample lands */
    UINT64 rtc_sample_frame;    /* s->frames at the last actual RTC read      */
    UINT64 rtc_cached_uptime_sec; /* uptime_sec as of that last RTC read      */

    /* Block devices.                                                        */
    int  blkdev_count;
} sm_state;

static sm_state g_sm;

/* Small scratch array for the block-device count (count only; contents unused).*/
static struct diskio_dev g_sm_devs[24];

/* ==========================================================================
 * Tiny freestanding helpers.
 * ========================================================================== */
static int  sm_slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }

/* Unsigned decimal -> ascii; returns the written length (excl. NUL). */
static int sm_u2a(UINT64 v, char *out, int cap)
{
    char tmp[24]; int i=0;
    if(cap<=0){ return 0; }
    if(!v) tmp[i++]='0';
    while(v && i<24){ tmp[i++]=(char)('0'+(int)(v%10)); v/=10; }
    int p=0;
    while(i>0 && p+1<cap) out[p++]=tmp[--i];
    out[p]=0;
    return p;
}

/* Append src to dst[] at *pos (bounded by cap incl. NUL). */
static void sm_append(char *dst, int cap, int *pos, const char *src)
{
    int p=*pos;
    for(int i=0; src && src[i] && p+1<cap; i++) dst[p++]=src[i];
    dst[p]=0; *pos=p;
}
static void sm_append_u(char *dst, int cap, int *pos, UINT64 v)
{
    char t[24]; sm_u2a(v,t,24); sm_append(dst,cap,pos,t);
}

/* CHAR16 -> ascii (printable ASCII only). */
static void sm_u16toa(const CHAR16 *u, char *a, int cap)
{
    int i=0;
    for(; u && u[i] && i+1<cap; i++){ CHAR16 c=u[i]; a[i]=(c>=0x20&&c<0x7f)?(char)c:'?'; }
    a[i]=0;
}

static int sm_sc(void){ int s=ui_scale(); return s<1?1:s; }

/* Read the RTC as seconds-into-the-day (+ day-of-month). Returns 1 on success. */
static int sm_read_rtc(UINT32 *daysec, UINT32 *day)
{
    if(!gRT || !gRT->GetTime) return 0;
    sm_get_time_fn gt = (sm_get_time_fn)gRT->GetTime;
    EFI_TIME t;
    if(gBS) gBS->SetMem(&t, sizeof(t), 0);
    if(EFI_ERROR(gt(&t, NULL))) return 0;
    if(daysec) *daysec = (UINT32)t.Hour*3600u + (UINT32)t.Minute*60u + (UINT32)t.Second;
    if(day)    *day    = t.Day;
    return 1;
}

/* ==========================================================================
 * Data gathering (called on open + every SM_REFRESH frames).
 * ========================================================================== */
static void sm_gather_ram(sm_state *s)
{
    s->ram_ok=0;
    if(!gBS) return;
    UINTN sz=0,key=0,dsz=0; UINT32 dver=0;
    if(gBS->GetMemoryMap(&sz,NULL,&key,&dsz,&dver)!=EFI_BUFFER_TOO_SMALL || !sz || !dsz) return;
    sz += dsz*8;                                    /* headroom for allocation churn */
    UINT8 *map=NULL;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,sz,(VOID**)&map)) || !map) return;
    if(EFI_ERROR(gBS->GetMemoryMap(&sz,(EFI_MEMORY_DESCRIPTOR*)map,&key,&dsz,&dver))){
        gBS->FreePool(map); return;
    }
    UINT64 total=0, conv=0, reserved=0, acpi=0, mmio=0;
    UINTN entries = sz/dsz;
    for(UINTN i=0;i<entries;i++){
        EFI_MEMORY_DESCRIPTOR *d=(EFI_MEMORY_DESCRIPTOR*)(map+i*dsz);
        UINT64 pg=d->NumberOfPages;
        switch(d->Type){
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
                mmio+=pg; break;                    /* not physical RAM */
            case EfiReservedMemoryType:
            case EfiUnusableMemory:
            case EfiPalCode:
                reserved+=pg; total+=pg; break;
            case EfiACPIReclaimMemory:
            case EfiACPIMemoryNVS:
                acpi+=pg; total+=pg; break;
            case EfiConventionalMemory:
                conv+=pg; total+=pg; break;
            default:                                /* loader/BS/RT code+data etc. */
                total+=pg; break;
        }
    }
    gBS->FreePool(map);

    /* pages * 4096 >> 20  ==  pages >> 8  (MiB). */
    s->ram_total_mib    = total>>8;
    s->ram_conv_mib     = conv>>8;
    s->ram_reserved_mib = reserved>>8;
    s->ram_acpi_mib     = acpi>>8;
    s->ram_mmio_mib     = mmio>>8;
    s->ram_used_mib     = (total>conv ? (total-conv) : 0)>>8;
    s->mem_descriptors  = entries;
    s->ram_ok=1;

    /* Pre-format the label lines once here; sm_draw() reuses them every
     * frame until the next poll (byte-identical output, no per-frame work). */
    int p;
    p=0; s->ram_line1[0]=0;
    sm_append(s->ram_line1,sizeof(s->ram_line1),&p,"Used ");
    sm_append_u(s->ram_line1,sizeof(s->ram_line1),&p,s->ram_used_mib);
    sm_append(s->ram_line1,sizeof(s->ram_line1),&p," / ");
    sm_append_u(s->ram_line1,sizeof(s->ram_line1),&p,s->ram_total_mib);
    sm_append(s->ram_line1,sizeof(s->ram_line1),&p," MiB");

    p=0; s->ram_line2[0]=0;
    sm_append(s->ram_line2,sizeof(s->ram_line2),&p,"Free ");
    sm_append_u(s->ram_line2,sizeof(s->ram_line2),&p,s->ram_conv_mib);
    sm_append(s->ram_line2,sizeof(s->ram_line2),&p,"  Reserved ");
    sm_append_u(s->ram_line2,sizeof(s->ram_line2),&p,s->ram_reserved_mib);
    sm_append(s->ram_line2,sizeof(s->ram_line2),&p," MiB");

    p=0; s->ram_line3[0]=0;
    sm_append(s->ram_line3,sizeof(s->ram_line3),&p,"ACPI ");
    sm_append_u(s->ram_line3,sizeof(s->ram_line3),&p,s->ram_acpi_mib);
    sm_append(s->ram_line3,sizeof(s->ram_line3),&p,"  MMIO ");
    sm_append_u(s->ram_line3,sizeof(s->ram_line3),&p,s->ram_mmio_mib);
    sm_append(s->ram_line3,sizeof(s->ram_line3),&p," MiB  (");
    sm_append_u(s->ram_line3,sizeof(s->ram_line3),&p,s->mem_descriptors);
    sm_append(s->ram_line3,sizeof(s->ram_line3),&p," descriptors)");
}

/* Cached GOP protocol pointer: located once and reused, since the boot
 * device's GOP instance never changes after firmware handoff. Re-located
 * only if it comes back NULL (e.g. protocol not yet installed on first try). */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *g_sm_gop = NULL;

static void sm_gather_gop(sm_state *s)
{
    s->gop_ok=0;
    if(!gBS) return;
    if(!g_sm_gop){
        if(EFI_ERROR(gBS->LocateProtocol(&gGopGuid,NULL,(VOID**)&g_sm_gop)) || !g_sm_gop){
            g_sm_gop=NULL; return;
        }
    }
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=g_sm_gop;
    if(!gop->Mode || !gop->Mode->Info) return;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=gop->Mode->Info;
    s->gop_w=mi->HorizontalResolution;
    s->gop_h=mi->VerticalResolution;
    s->gop_pitch=mi->PixelsPerScanLine;
    s->gop_fmt = (mi->PixelFormat==PixelBlueGreenRedReserved8BitPerColor)?"BGRX":
                 (mi->PixelFormat==PixelRedGreenBlueReserved8BitPerColor)?"RGBX":
                 (mi->PixelFormat==PixelBitMask)?"BitMask":"BltOnly";
    s->gop_fb_mib=(UINT64)gop->Mode->FrameBufferSize>>20;
    s->gop_ok=1;

    int p=0; s->gop_line[0]=0;
    sm_append_u(s->gop_line,sizeof(s->gop_line),&p,s->gop_w);
    sm_append(s->gop_line,sizeof(s->gop_line),&p,"x");
    sm_append_u(s->gop_line,sizeof(s->gop_line),&p,s->gop_h);
    sm_append(s->gop_line,sizeof(s->gop_line),&p,"  ");
    sm_append(s->gop_line,sizeof(s->gop_line),&p,s->gop_fmt);
    sm_append(s->gop_line,sizeof(s->gop_line),&p,"  pitch ");
    sm_append_u(s->gop_line,sizeof(s->gop_line),&p,s->gop_pitch);
    sm_append(s->gop_line,sizeof(s->gop_line),&p,"px  fb ");
    sm_append_u(s->gop_line,sizeof(s->gop_line),&p,s->gop_fb_mib);
    sm_append(s->gop_line,sizeof(s->gop_line),&p," MiB");
}

static void sm_gather_fw(sm_state *s)
{
    s->fw_ok=0;
    s->secure_boot=-1;
    if(gST){
        sm_u16toa(gST->FirmwareVendor, s->fw_vendor, (int)sizeof(s->fw_vendor));
        s->fw_rev     = gST->FirmwareRevision;
        s->uefi_major = (UINT16)((gST->Hdr.Revision>>16)&0xFFFF);
        s->uefi_minor = (UINT16)(gST->Hdr.Revision&0xFFFF);
        s->fw_ok=1;

        int p=0; s->fw_line1[0]=0;
        sm_append(s->fw_line1,sizeof(s->fw_line1),&p,"Vendor: ");
        sm_append(s->fw_line1,sizeof(s->fw_line1),&p,s->fw_vendor[0]?s->fw_vendor:"(none)");

        p=0; s->fw_line2[0]=0;
        sm_append(s->fw_line2,sizeof(s->fw_line2),&p,"UEFI ");
        sm_append_u(s->fw_line2,sizeof(s->fw_line2),&p,s->uefi_major);
        sm_append(s->fw_line2,sizeof(s->fw_line2),&p,".");
        sm_append_u(s->fw_line2,sizeof(s->fw_line2),&p,s->uefi_minor);
        sm_append(s->fw_line2,sizeof(s->fw_line2),&p,"  FW rev ");
        sm_append_u(s->fw_line2,sizeof(s->fw_line2),&p,s->fw_rev);
    }
    if(gRT && gRT->GetVariable){
        UINT8 sb=0; UINTN vs=1;
        EFI_STATUS st=gRT->GetVariable(L"SecureBoot",&gGlobalVar,NULL,&vs,&sb);
        if(!EFI_ERROR(st)) s->secure_boot = sb?1:0;
    }
    {
        const char *sb = (s->secure_boot<0)?"N/A":(s->secure_boot?"ENABLED":"disabled");
        int p=0; s->sb_line[0]=0;
        sm_append(s->sb_line,sizeof(s->sb_line),&p,"SecureBoot: ");
        sm_append(s->sb_line,sizeof(s->sb_line),&p,sb);
    }
}

static void sm_gather_uptime(sm_state *s)
{
    if(s->rtc_ok){
        /* The displayed uptime only has whole-second granularity, so throttle
         * the firmware GetTime() call to ~once per second (SM_EST_FPS frames)
         * instead of every composited frame; interpolate the seconds in
         * between from the frame counter. The displayed h/m/s string is
         * unchanged vs. per-frame sampling in the overwhelming majority of
         * frames. */
        if(!s->rtc_sampled || (s->frames - s->rtc_sample_frame) >= SM_EST_FPS){
            UINT32 nowsec=0, nowday=0;
            if(sm_read_rtc(&nowsec,&nowday)){
                INT64 d=(INT64)nowsec-(INT64)s->start_daysec;
                d += (INT64)((INT32)nowday-(INT32)s->start_day)*86400;   /* coarse day wrap */
                if(d<0) d+=86400;                                        /* month rollover */
                if(d<0) d=0;
                s->rtc_cached_uptime_sec=(UINT64)d;
                s->rtc_sample_frame=s->frames;
                s->rtc_sampled=1;
            }
        }
        if(s->rtc_sampled){
            s->uptime_sec = s->rtc_cached_uptime_sec + (s->frames - s->rtc_sample_frame)/SM_EST_FPS;
        } else {
            /* First read hasn't succeeded yet: fall back to frame estimate. */
            s->uptime_sec = s->frames / SM_EST_FPS;
        }
    } else {
        /* No RTC: estimate from the monotonic frame counter. */
        s->uptime_sec = s->frames / SM_EST_FPS;
    }
}

static void sm_gather_blk(sm_state *s)
{
    int n=diskio_enumerate(g_sm_devs, (int)(sizeof(g_sm_devs)/sizeof(g_sm_devs[0])));
    s->blkdev_count = (n<0)?0:n;
}

static void sm_gather_all(sm_state *s)
{
    /* RAM is the heavy poll (GetMemoryMap + AllocatePool/FreePool churn) and
     * its values are near-static, so refresh it on a coarser cadence than the
     * cheap GOP/firmware/uptime snapshots. */
    if(!s->last_poll || (s->frames - s->last_ram_poll) >= SM_RAM_REFRESH){
        sm_gather_ram(s);
        s->last_ram_poll = s->frames;
    }
    sm_gather_gop(s);
    sm_gather_fw(s);
    /* Block-device count is enumerated once at open (static for this window). */
    sm_gather_uptime(s);
    s->last_poll = s->frames;
}

/* ==========================================================================
 * Drawing helpers (all bounds are already clipped by ui.c primitives; we
 * additionally clip labels to the client width with draw_string_clip).
 * ========================================================================== */

/* A labeled horizontal gauge: track + proportional fill (num/den). */
static void sm_bar(int x, int y, int w, int h, UINT64 num, UINT64 den,
                   UINT32 fill, UINT32 track)
{
    if(w<2 || h<1) return;
    fill_rect(x,y,w,h,track);
    if(den==0) return;
    if(num>den) num=den;
    /* integer scale, guard against overflow on huge MiB values */
    UINT64 fw = (UINT64)w * num / den;
    int fwi=(int)fw; if(fwi>w) fwi=w; if(fwi<0) fwi=0;
    if(fwi>0) fill_rect(x,y,fwi,h,fill);
}

/* ==========================================================================
 * Window draw callback.
 * ========================================================================== */
static void sm_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    sm_state *s=&g_sm;
    (void)w;

    /* Advance the monotonic frame clock and re-poll on cadence. */
    s->frames++;
    if(s->frames - s->last_poll >= SM_REFRESH) sm_gather_all(s);
    else sm_gather_uptime(s);   /* uptime label ticks every frame */

    UINT32 c_win    = wm_theme_color(WM_COL_WINDOW);
    UINT32 c_fg     = wm_theme_color(WM_COL_FG);
    UINT32 c_accent = wm_theme_color(WM_COL_ACCENT);
    UINT32 c_dim    = FOREB_DIM;
    UINT32 c_track  = FOREB_BORDER;

    int sc=sm_sc();
    int gh=16*sc;                 /* glyph cell height */
    int row=gh+4*sc;              /* line pitch        */
    int padx=8*sc;
    int x=cx+padx;
    int y=cy+6*sc;
    int right=cx+cw-padx;         /* clip boundary (exclusive-ish)     */
    int maxw=right-x; if(maxw<8) maxw=8;
    int barh=10*sc;

    char line[160];

    /* --- Header -----------------------------------------------------------
     * SM_REFRESH is a compile-time constant, so this text never changes;
     * hand-written as a literal instead of rebuilt via sm_append/sm_append_u
     * every frame. Byte-identical to the old "System Monitor  (live, ~" +
     * "30" + "-frame refresh)" output -- keep this in sync with SM_REFRESH
     * if that macro's value is ever changed.                              */
    draw_string_clip(x,y,maxw,"System Monitor  (live, ~30-frame refresh)",c_accent,c_win,1,sc);
    y+=row+2*sc;

    /* --- RAM ------------------------------------------------------------ */
    draw_string_clip(x,y,maxw,"RAM",c_accent,c_win,1,sc); y+=row;
    if(s->ram_ok){
        draw_string_clip(x,y,maxw,s->ram_line1,c_fg,c_win,1,sc); y+=row;

        int bw=right-x; if(bw<8)bw=8;
        sm_bar(x,y,bw,barh,s->ram_used_mib,s->ram_total_mib,c_accent,c_track);
        y+=barh+6*sc;

        draw_string_clip(x,y,maxw,s->ram_line2,c_dim,c_win,1,sc); y+=row;
        draw_string_clip(x,y,maxw,s->ram_line3,c_dim,c_win,1,sc); y+=row;
    } else {
        draw_string_clip(x,y,maxw,"GetMemoryMap: N/A",c_dim,c_win,1,sc); y+=row;
    }
    y+=4*sc;

    /* --- GOP ------------------------------------------------------------ */
    draw_string_clip(x,y,maxw,"Graphics (GOP)",c_accent,c_win,1,sc); y+=row;
    if(s->gop_ok){
        draw_string_clip(x,y,maxw,s->gop_line,c_fg,c_win,1,sc); y+=row;
    } else {
        draw_string_clip(x,y,maxw,"GOP: N/A",c_dim,c_win,1,sc); y+=row;
    }
    y+=4*sc;

    /* --- Firmware ------------------------------------------------------- */
    draw_string_clip(x,y,maxw,"Firmware",c_accent,c_win,1,sc); y+=row;
    if(s->fw_ok){
        draw_string_clip(x,y,maxw,s->fw_line1,c_fg,c_win,1,sc); y+=row;
        draw_string_clip(x,y,maxw,s->fw_line2,c_dim,c_win,1,sc); y+=row;
    } else {
        draw_string_clip(x,y,maxw,"Firmware: N/A",c_dim,c_win,1,sc); y+=row;
    }
    {
        UINT32 col = (s->secure_boot==1)?FOREB_TITLE:(s->secure_boot==0)?FOREB_TIMER:c_dim;
        draw_string_clip(x,y,maxw,s->sb_line,col,c_win,1,sc); y+=row;
    }
    y+=4*sc;

    /* --- Uptime + devices ---------------------------------------------- */
    {
        UINT64 up=s->uptime_sec;
        UINT64 hh=up/3600, mm=(up/60)%60, ss=up%60;
        int p=0; line[0]=0;
        sm_append(line,sizeof(line),&p,s->rtc_ok?"Uptime: ":"Uptime (approx): ");
        sm_append_u(line,sizeof(line),&p,hh);
        sm_append(line,sizeof(line),&p,"h ");
        sm_append_u(line,sizeof(line),&p,mm);
        sm_append(line,sizeof(line),&p,"m ");
        sm_append_u(line,sizeof(line),&p,ss);
        sm_append(line,sizeof(line),&p,"s");
        draw_string_clip(x,y,maxw,line,c_fg,c_win,1,sc); y+=row;

        /* No per-frame "Frames:" counter here: it would mutate the window's
         * text every composited frame and defeat repaint elision. The
         * block-device count is static for the window lifetime, so this line
         * is byte-identical frame-to-frame. */
        p=0; line[0]=0;
        sm_append(line,sizeof(line),&p,"Block devices: ");
        sm_append_u(line,sizeof(line),&p,(UINT64)s->blkdev_count);
        draw_string_clip(x,y,maxw,line,c_dim,c_win,1,sc); y+=row;
    }

    draw_string_clip(cx+padx, cy+ch-gh-2*sc, cw-2*padx,
                     "Esc to close", c_dim, c_win, 1, sc);

    /* silence unused warnings when sc==0 edge never hits (kept explicit) */
    (void)sm_slen;
}

/* ==========================================================================
 * Window event callback.
 * ========================================================================== */
static int sm_event(wm_window *w, const wm_event *ev)
{
    (void)w;
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC || ev->unicode=='q' || ev->unicode=='Q')
                return WM_CLOSE_REQUEST;
            return 0;
        case WM_EV_CLOSE:
            g_sm.win=NULL;
            return 0;
        default:
            return 0;
    }
}

/* ==========================================================================
 * Public entry points.
 * ========================================================================== */
void tool_sysmon_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices : 0;
    gRT = st ? st->RuntimeServices : 0;
    diskio_init(st);                 /* idempotent; safe if already inited */
}

void tool_sysmon_open(void)
{
    if(g_sm.win) return;             /* already open */

    /* Fresh state each open. */
    if(gBS) gBS->SetMem(&g_sm, sizeof(g_sm), 0);
    else {
        /* No BootServices: zero the fields we rely on by hand. */
        g_sm.win=0; g_sm.frames=0; g_sm.last_poll=0;
        g_sm.ram_ok=0; g_sm.gop_ok=0; g_sm.fw_ok=0;
        g_sm.rtc_ok=0; g_sm.blkdev_count=0; g_sm.secure_boot=-1;
    }
    g_sm.secure_boot=-1;

    /* Capture an RTC baseline for real uptime; fall back to frame counter. */
    g_sm.rtc_ok = sm_read_rtc(&g_sm.start_daysec, &g_sm.start_day);

    sm_gather_all(&g_sm);
    sm_gather_blk(&g_sm);              /* block-device count is static for the
                                        * window lifetime: enumerate once here
                                        * instead of every SM_REFRESH frames. */
    g_sm.frames=0; g_sm.last_poll=0; g_sm.last_ram_poll=0;  /* fresh after seed */

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*46/100; if(ww<420)ww=420; if(ww>620)ww=620; if(ww>W-40)ww=W-40;
    int wh=H*62/100; if(wh<360)wh=360; if(wh>560)wh=560; if(wh>H-40)wh=H-40;

    g_sm.win = wm_open("System Monitor", ww, wh, sm_draw, sm_event, &g_sm);
}

/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_hw.c - "Hardware & Diag" tool category (KEY = hw).
 * =============================================================================
 * Nine self-contained template-B windows:
 *   CPUID viewer, MSR reader, PCI lister, ACPI table lister, SMBIOS/DMI info,
 *   Memory pattern tester, GOP mode lister, TSC frequency estimate, PIT test.
 *
 * All windows share one scrolling text-list widget (hw_render) fed from a fixed
 * per-tool line buffer; interactive tools rebuild that buffer on a keypress /
 * click. No libc, no heap (except a bounded AllocatePool in the memory tester),
 * integer math only, all pre-ExitBootServices.
 * ========================================================================== */
#include "tools_hw.h"
#include "../ui.h"
#include "../core/wm.h"
#include "../core/input.h"
#include "../efi.h"
#include "../../include/forebo_theme.h"

/* ==========================================================================
 * Firmware services (clock.c idiom).
 * ========================================================================== */
static EFI_SYSTEM_TABLE     *gST;
static EFI_BOOT_SERVICES    *gBS;
static EFI_RUNTIME_SERVICES *gRT;

void cat_hw_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices : 0;
    gRT = st ? st->RuntimeServices : 0;
}

/* ==========================================================================
 * Low-level x86 helpers (inline asm; CPL0 in the loader).
 * ========================================================================== */
#if defined(__x86_64__) || defined(_M_X64)
static inline void io_outb(UINT16 p, UINT8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline UINT8 io_inb(UINT16 p){ UINT8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void io_outl(UINT16 p, UINT32 v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline UINT32 io_inl(UINT16 p){ UINT32 r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p)); return r; }

static inline UINT64 hw_rdtsc(void)
{
    UINT32 lo, hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return ((UINT64)hi<<32)|lo;
}
static inline void hw_cpuid(UINT32 leaf, UINT32 sub, UINT32 r[4])
{
    UINT32 a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(leaf),"c"(sub));
    r[0]=a; r[1]=b; r[2]=c; r[3]=d;
}
/* Only ever called on the curated whitelist (architectural MSRs) -> no #GP. */
static inline UINT64 hw_rdmsr(UINT32 idx)
{
    UINT32 lo, hi;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(idx));
    return ((UINT64)hi<<32)|lo;
}
#else
static inline void io_outb(UINT16 p, UINT8 v){ (void)p; (void)v; }
static inline UINT8 io_inb(UINT16 p){ (void)p; return 0; }
static inline void io_outl(UINT16 p, UINT32 v){ (void)p; (void)v; }
static inline UINT32 io_inl(UINT16 p){ (void)p; return 0; }
static inline UINT64 hw_rdtsc(void){ static UINT64 c=0x9E3779B97F4A7C15ull; c+=0x9E3779B97F4A7C15ull; return c; }
static inline void hw_cpuid(UINT32 leaf, UINT32 sub, UINT32 r[4]){ (void)leaf; (void)sub; r[0]=r[1]=r[2]=r[3]=0; }
static inline UINT64 hw_rdmsr(UINT32 idx){ (void)idx; return 0; }
#endif

static void hw_stall(UINTN us){ if(gBS && gBS->Stall) gBS->Stall(us); }

/* Unaligned little-endian reads from firmware tables. */
static UINT16 rd16(const UINT8 *p){ return (UINT16)(p[0]|(p[1]<<8)); }
static UINT32 rd32(const UINT8 *p){ return (UINT32)(p[0]|(p[1]<<8)|(p[2]<<16)|((UINT32)p[3]<<24)); }
static UINT64 rd64(const UINT8 *p){ return (UINT64)rd32(p)|((UINT64)rd32(p+4)<<32); }

/* ==========================================================================
 * Shared scrolling text-list widget.
 * ========================================================================== */
#define HW_MAXLINES 300
#define HW_COLS     84

enum {
    HW_CPUID = 0, HW_MSR, HW_PCI, HW_ACPI, HW_SMBIOS,
    HW_MEMTEST, HW_GOP, HW_TSC, HW_PIT
};

typedef struct hwlist {
    wm_window  *win;
    int         kind;
    const char *title;
    void      (*rebuild)(struct hwlist *);

    char   text[HW_MAXLINES][HW_COLS];
    UINT32 col [HW_MAXLINES];
    int    n;
    int    scroll;

    /* MSR typed-index entry + last result. */
    char   input[9];
    int    inlen;
    UINT32 res_idx;
    UINT64 res_val;
    int    res_state;        /* 0 none, 1 ok, 2 refused (not whitelisted)   */

    /* Memory tester. */
    int    sizesel;          /* 0..3 -> 1/4/8/16 MiB                         */
    int    ran;              /* 1 once a test has been executed             */
} hwlist;

static int hw_slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }

/* Begin a fresh line with color c. */
static void L_line(hwlist *L, UINT32 c)
{
    if(L->n>=HW_MAXLINES) return;
    L->col[L->n]=c; L->text[L->n][0]=0; L->n++;
}
static void L_str(hwlist *L, const char *s)
{
    if(L->n<=0 || !s) return;
    char *d=L->text[L->n-1]; int len=hw_slen(d);
    while(*s && len<HW_COLS-1) d[len++]=*s++;
    d[len]=0;
}
static void L_ch(hwlist *L, char ch)
{
    if(L->n<=0) return;
    char *d=L->text[L->n-1]; int len=hw_slen(d);
    if(len<HW_COLS-1){ d[len++]=ch; d[len]=0; }
}
static void L_u(hwlist *L, UINT64 v)
{
    char t[24]; int i=0;
    if(!v) t[i++]='0';
    while(v && i<24){ t[i++]=(char)('0'+(int)(v%10)); v/=10; }
    char o[24]; int p=0; while(i>0) o[p++]=t[--i]; o[p]=0;
    L_str(L,o);
}
/* Hex, zero-padded to at least `dig` digits (dig<=0 -> minimal). */
static void L_x(hwlist *L, UINT64 v, int dig)
{
    static const char hx[]="0123456789ABCDEF";
    char t[16]; int i=0;
    if(!v) t[i++]='0';
    while(v && i<16){ t[i++]=hx[v&0xF]; v>>=4; }
    while(i<dig && i<16) t[i++]='0';
    char o[17]; int p=0; while(i>0) o[p++]=t[--i]; o[p]=0;
    L_str(L,o);
}

/* Rows the list body can show for a given client height (draw + event agree). */
static int hw_rows(int ch)
{
    int sc=ui_scale(); if(sc<1)sc=1;
    int gh=16*sc, row=gh+2*sc, pad=8*sc;
    int listtop = pad + row + 4*sc;          /* title row + gap             */
    int avail = ch - listtop - (row + pad);  /* leave a footer row          */
    int rows = avail/row;
    return rows<1?1:rows;
}

static void hw_clamp(hwlist *L, int rows)
{
    if(L->scroll > L->n-rows) L->scroll = L->n-rows;
    if(L->scroll < 0) L->scroll = 0;
}

static const char *hw_footer(const hwlist *L)
{
    switch(L->kind){
        case HW_MSR:     return "hex digits + Enter = read | Bksp | R rescan | Esc";
        case HW_MEMTEST: return "Left/Right size  Enter/click = run (large sizes pause UI)  R reset  Esc";
        case HW_TSC:     return "Enter / click = re-measure   R refresh   Esc close";
        case HW_PIT:     return "Enter/click = re-measure   Space = beep   Esc close";
        default:         return "Up/Down/PgUp/PgDn scroll   R refresh   Esc close";
    }
}

static void hw_render(wm_window *w, int cx, int cy, int cw, int ch)
{
    hwlist *L=(hwlist*)wm_user(w);
    if(!L) return;

    UINT32 c_win    = wm_theme_color(WM_COL_WINDOW);
    UINT32 c_accent = wm_theme_color(WM_COL_ACCENT);
    UINT32 c_dim    = FOREB_DIM;

    fill_rect(cx,cy,cw,ch,c_win);

    int sc=ui_scale(); if(sc<1)sc=1;
    int gh=16*sc, row=gh+2*sc, pad=8*sc;
    int x=cx+pad, y=cy+pad;
    int maxw=cw-2*pad; if(maxw<8) maxw=8;

    /* Title. */
    draw_string_clip(x,y,maxw,L->title,c_accent,c_win,1,sc);
    y += row + 4*sc;

    int rows=hw_rows(ch);
    hw_clamp(L,rows);

    int listtop=y;
    int barw = (L->n>rows) ? (6*sc) : 0;
    int textw = maxw - barw; if(textw<8) textw=8;

    for(int i=0;i<rows;i++){
        int idx=L->scroll+i;
        if(idx>=L->n) break;
        draw_string_clip(x, listtop+i*row, textw, L->text[idx], L->col[idx], c_win, 1, sc);
    }

    /* Scrollbar. */
    if(L->n>rows){
        int trackx = cx+cw-pad-barw+2*sc;
        int trackh = rows*row;
        fill_rect(trackx, listtop, barw-2*sc, trackh, FOREB_BORDER);
        int thumbh = trackh*rows/L->n; if(thumbh<6) thumbh=6;
        int span = trackh-thumbh; if(span<0) span=0;
        int maxscroll = L->n-rows; if(maxscroll<1) maxscroll=1;
        int thumby = listtop + span*L->scroll/maxscroll;
        fill_rect(trackx, thumby, barw-2*sc, thumbh, c_accent);
    }

    /* Footer hint (+ MSR live index). */
    int fy = cy+ch-gh-2*sc;
    if(L->kind==HW_MSR){
        char f[64]; int p=0;
        const char *pre="idx=0x";
        while(*pre && p<63) f[p++]=*pre++;
        if(L->inlen==0){ if(p<63) f[p++]='_'; }
        else for(int i=0;i<L->inlen && p<63;i++) f[p++]=L->input[i];
        f[p]=0;
        draw_string_clip(x, fy, maxw/2, f, FOREB_WHITE, c_win, 1, sc);
        draw_string_clip(x+maxw/2, fy, maxw/2, hw_footer(L), c_dim, c_win, 1, sc);
    } else {
        draw_string_clip(x, fy, maxw, hw_footer(L), c_dim, c_win, 1, sc);
    }
}

/* ==========================================================================
 * 1. CPUID viewer.
 * ========================================================================== */
static const char *const g_f1edx[32] = {
    "FPU","VME","DE","PSE","TSC","MSR","PAE","MCE","CX8","APIC",0,"SEP",
    "MTRR","PGE","MCA","CMOV","PAT","PSE36","PSN","CLFSH",0,"DS","ACPI","MMX",
    "FXSR","SSE","SSE2","SS","HTT","TM","IA64","PBE"
};
static const char *const g_f1ecx[32] = {
    "SSE3","PCLMUL","DTES64","MONITOR","DS-CPL","VMX","SMX","EIST","TM2","SSSE3",
    "CNXT-ID","SDBG","FMA","CX16","XTPR","PDCM",0,"PCID","DCA","SSE4.1","SSE4.2",
    "x2APIC","MOVBE","POPCNT","TSC-DL","AESNI","XSAVE","OSXSAVE","AVX","F16C",
    "RDRAND","HYPERV"
};
static const char *const g_f7ebx[32] = {
    "FSGSBASE",0,"SGX","BMI1","HLE","AVX2",0,"SMEP","BMI2","ERMS","INVPCID","RTM",
    "PQM",0,0,"PQE","AVX512F","AVX512DQ","RDSEED","ADX","SMAP","AVX512IFMA",0,
    "CLFLUSHOPT","CLWB","PT","AVX512PF","AVX512ER","AVX512CD","SHA","AVX512BW",
    "AVX512VL"
};

/* Append set feature names from `bits`, wrapping onto fresh indented lines. */
static void feat_dump(hwlist *L, UINT32 bits, const char *const *names, UINT32 c)
{
    L_line(L,c); L_str(L,"  ");
    int any=0;
    for(int i=0;i<32;i++){
        if(((bits>>i)&1u) && names[i]){
            int need=hw_slen(names[i])+1;
            if(hw_slen(L->text[L->n-1])+need > HW_COLS-2){ L_line(L,c); L_str(L,"  "); }
            L_str(L,names[i]); L_ch(L,' ');
            any=1;
        }
    }
    if(!any) L_str(L,"(none)");
}

static void build_cpuid(hwlist *L)
{
    L->n=0;
    UINT32 r[4];

    /* Vendor. */
    hw_cpuid(0,0,r);
    UINT32 maxleaf=r[0];
    char vend[13];
    ((UINT32*)vend)[0]=r[1]; ((UINT32*)vend)[1]=r[3]; ((UINT32*)vend)[2]=r[2];
    vend[12]=0;
    L_line(L,FOREB_TITLE); L_str(L,"Vendor: "); L_str(L,vend);
    L_str(L,"   max leaf 0x"); L_x(L,maxleaf,0);

    /* Brand string (0x80000002..4). */
    hw_cpuid(0x80000000u,0,r);
    UINT32 maxext=r[0];
    if(maxext>=0x80000004u){
        char brand[49]; UINT32 *bp=(UINT32*)brand;
        for(int i=0;i<3;i++){ UINT32 rr[4]; hw_cpuid(0x80000002u+(UINT32)i,0,rr);
            bp[i*4+0]=rr[0]; bp[i*4+1]=rr[1]; bp[i*4+2]=rr[2]; bp[i*4+3]=rr[3]; }
        brand[48]=0; char *p=brand; while(*p==' ')p++;
        L_line(L,FOREB_WHITE); L_str(L,"Brand:  "); L_str(L,p);
    }

    /* Family / model / stepping (leaf 1 EAX). */
    if(maxleaf>=1){
        hw_cpuid(1,0,r);
        UINT32 eax=r[0];
        UINT32 base_family=(eax>>8)&0xF, ext_family=(eax>>20)&0xFF;
        UINT32 base_model =(eax>>4)&0xF, ext_model =(eax>>16)&0xF;
        UINT32 family=base_family + ((base_family==0xF)?ext_family:0);
        UINT32 model =base_model  + ((base_family==0xF||base_family==0x6)?(ext_model<<4):0);
        UINT32 stepping=eax&0xF;
        L_line(L,FOREB_TEXT);
        L_str(L,"Family "); L_u(L,family);
        L_str(L,"  Model "); L_u(L,model);
        L_str(L,"  Stepping "); L_u(L,stepping);
        L_str(L,"  (0x"); L_x(L,eax,0); L_str(L,")");

        UINT32 ebx=r[1];
        L_line(L,FOREB_DIM);
        L_str(L,"CLFLUSH line "); L_u(L,(UINT64)((ebx>>8)&0xFF)*8);
        L_str(L," B   Init APIC id "); L_u(L,(ebx>>24)&0xFF);

        L_line(L,FOREB_TITLE); /* reused for the "Features" header below */
    }

    /* Feature flags. (r[] still holds leaf 1 from above; untouched since.) */
    if(maxleaf>=1){
        L->text[L->n-1][0]=0; L->col[L->n-1]=FOREB_TITLE; L_str(L,"Features (leaf 1 EDX):");
        feat_dump(L,r[3],g_f1edx,FOREB_TEXT);
        L_line(L,FOREB_TITLE); L_str(L,"Features (leaf 1 ECX):");
        feat_dump(L,r[2],g_f1ecx,FOREB_TEXT);
    }
    if(maxleaf>=7){
        hw_cpuid(7,0,r);
        L_line(L,FOREB_TITLE); L_str(L,"Ext features (leaf 7 EBX):");
        feat_dump(L,r[1],g_f7ebx,FOREB_TEXT);
    }
    /* Long mode / NX from ext leaf. */
    if(maxext>=0x80000001u){
        hw_cpuid(0x80000001u,0,r);
        L_line(L,FOREB_TITLE); L_str(L,"Ext (0x80000001 EDX):");
        L_line(L,FOREB_TEXT); L_str(L,"  ");
        if(r[3]&(1u<<11)) L_str(L,"SYSCALL ");
        if(r[3]&(1u<<20)) L_str(L,"NX ");
        if(r[3]&(1u<<26)) L_str(L,"1GB-PAGE ");
        if(r[3]&(1u<<27)) L_str(L,"RDTSCP ");
        if(r[3]&(1u<<29)) L_str(L,"LM(64-bit) ");
        if(r[2]&(1u<<0))  L_str(L,"LAHF ");
        if(r[2]&(1u<<5))  L_str(L,"LZCNT ");
    }
}

/* ==========================================================================
 * 2. MSR reader (curated architectural whitelist + typed index, guarded).
 * ========================================================================== */
static const struct { UINT32 idx; const char *name; } g_msr_safe[] = {
    { 0x10,       "IA32_TIME_STAMP_COUNTER" },
    { 0x17,       "IA32_PLATFORM_ID"        },
    { 0x1B,       "IA32_APIC_BASE"          },
    { 0x3A,       "IA32_FEATURE_CONTROL"    },
    { 0xFE,       "IA32_MTRRCAP"            },
    { 0x174,      "IA32_SYSENTER_CS"        },
    { 0x175,      "IA32_SYSENTER_ESP"       },
    { 0x176,      "IA32_SYSENTER_EIP"       },
    { 0x1A0,      "IA32_MISC_ENABLE"        },
    { 0x277,      "IA32_PAT"                },
    { 0xC0000080, "IA32_EFER"               },
    { 0xC0000081, "IA32_STAR"               },
    { 0xC0000082, "IA32_LSTAR"              },
    { 0xC0000100, "IA32_FS_BASE"            },
    { 0xC0000101, "IA32_GS_BASE"            },
    { 0xC0000102, "IA32_KERNEL_GS_BASE"     },
};
static const int g_msr_safe_n = (int)(sizeof(g_msr_safe)/sizeof(g_msr_safe[0]));

static int msr_whitelisted(UINT32 idx)
{
    for(int i=0;i<g_msr_safe_n;i++) if(g_msr_safe[i].idx==idx) return 1;
    return 0;
}

static int cpu_has_msr(void)
{
    static int cached=-1;      /* CPU feature bits are invariant for the boot session. */
    if(cached>=0) return cached;
    UINT32 r[4]; hw_cpuid(0,0,r);
    if(r[0]<1){ cached=0; return cached; }
    hw_cpuid(1,0,r);
    cached=(int)((r[3]>>5)&1u);      /* EDX bit 5 = MSR support */
    return cached;
}

static void build_msr(hwlist *L)
{
    L->n=0;
    L_line(L,FOREB_DIM);
    L_str(L,"RDMSR reads only the architectural MSRs below (a bad index #GPs).");
    if(!cpu_has_msr()){
        L_line(L,FOREB_TIMER); L_str(L,"CPU does not report MSR support (CPUID.1:EDX.5=0).");
        return;
    }
    L_line(L,FOREB_TITLE); L_str(L,"idx        value              name");
    for(int i=0;i<g_msr_safe_n;i++){
        UINT64 v=hw_rdmsr(g_msr_safe[i].idx);
        L_line(L,FOREB_TEXT);
        L_str(L,"0x"); L_x(L,g_msr_safe[i].idx,8);
        L_str(L," 0x"); L_x(L,v,16);
        L_str(L," "); L_str(L,g_msr_safe[i].name);
    }

    /* Typed-index result. */
    L_line(L,FOREB_BORDER); L_str(L,"----");
    if(L->res_state==1){
        L_line(L,FOREB_WHITE);
        L_str(L,"read 0x"); L_x(L,L->res_idx,0);
        L_str(L," = 0x"); L_x(L,L->res_val,16);
    } else if(L->res_state==2){
        L_line(L,FOREB_TIMER);
        L_str(L,"refused 0x"); L_x(L,L->res_idx,0);
        L_str(L," (not in safe whitelist)");
    } else {
        L_line(L,FOREB_DIM);
        L_str(L,"Type a hex MSR index and press Enter (whitelisted only).");
    }
}

static void msr_read_typed(hwlist *L)
{
    if(L->inlen==0) return;
    UINT32 idx=0;
    for(int i=0;i<L->inlen;i++){
        char c=L->input[i]; int hv;
        if(c>='0'&&c<='9') hv=c-'0';
        else if(c>='A'&&c<='F') hv=c-'A'+10;
        else if(c>='a'&&c<='f') hv=c-'a'+10;
        else hv=0;
        idx=(idx<<4)|(UINT32)hv;
    }
    L->res_idx=idx;
    if(cpu_has_msr() && msr_whitelisted(idx)){
        L->res_val=hw_rdmsr(idx);
        L->res_state=1;
    } else {
        L->res_state=2;
    }
    L->inlen=0; L->input[0]=0;
    build_msr(L);
}

/* ==========================================================================
 * 3. PCI lister (0xCF8/0xCFC config space scan).
 * ========================================================================== */
static UINT32 pci_read32(int bus, int dev, int fn, int off)
{
    UINT32 addr=(UINT32)((1u<<31)|((UINT32)bus<<16)|((UINT32)dev<<11)|
                         ((UINT32)fn<<8)|((UINT32)off&0xFC));
    io_outl(0xCF8,addr);
    return io_inl(0xCFC);
}

static const char *pci_class_name(UINT8 base, UINT8 sub)
{
    switch(base){
        case 0x00: return "Unclassified";
        case 0x01:
            switch(sub){ case 0x01: return "IDE controller"; case 0x06: return "SATA/AHCI";
                         case 0x08: return "NVMe"; default: return "Mass storage"; }
        case 0x02: return "Network controller";
        case 0x03: return "Display / VGA";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory controller";
        case 0x06:
            switch(sub){ case 0x00: return "Host bridge"; case 0x01: return "ISA bridge";
                         case 0x04: return "PCI-PCI bridge"; default: return "Bridge"; }
        case 0x07: return "Comm controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input device";
        case 0x0A: return "Docking station";
        case 0x0B: return "Processor";
        case 0x0C:
            switch(sub){ case 0x03: return "USB controller"; case 0x05: return "SMBus";
                         default: return "Serial bus"; }
        case 0x0D: return "Wireless controller";
        case 0x10: return "Encryption";
        case 0x11: return "Signal processing";
        default:   return "Device";
    }
}

static void build_pci(hwlist *L)
{
    L->n=0;
    L_line(L,FOREB_TITLE); L_str(L,"B:D.F  vendor:device  class          name");
    int found=0;

    /* Bus worklist: start at bus 0, descend only into buses discovered behind
     * PCI-PCI bridges (avoids brute-scanning all 256*32 config slots, which
     * stalls the WM event path on real hardware). */
    UINT8 buslist[64]; int busn=0, busi=0;
    UINT8 seen[256]; for(int i=0;i<256;i++) seen[i]=0;
    buslist[busn++]=0; seen[0]=1;

    while(busi<busn && L->n<HW_MAXLINES-2){
        int bus=buslist[busi++];
        for(int dev=0; dev<32 && L->n<HW_MAXLINES-2; dev++){
            UINT32 v0=pci_read32(bus,dev,0,0x00);
            if((v0&0xFFFF)==0xFFFF) continue;         /* no function 0 */
            UINT32 hdr=pci_read32(bus,dev,0,0x0C);
            int multi=(hdr>>16)&0x80;
            int maxfn=multi?8:1;
            for(int fn=0; fn<maxfn && L->n<HW_MAXLINES-2; fn++){
                UINT32 idv=pci_read32(bus,dev,fn,0x00);
                UINT16 vend=(UINT16)(idv&0xFFFF);
                if(vend==0xFFFF) continue;
                UINT16 devid=(UINT16)(idv>>16);
                UINT32 cl=pci_read32(bus,dev,fn,0x08);
                UINT8 baseclass=(UINT8)(cl>>24), subclass=(UINT8)(cl>>16);
                found++;
                L_line(L,FOREB_TEXT);
                L_x(L,(UINT64)bus,2); L_ch(L,':'); L_x(L,(UINT64)dev,2);
                L_ch(L,'.'); L_u(L,(UINT64)fn); L_str(L,"  ");
                L_x(L,vend,4); L_ch(L,':'); L_x(L,devid,4); L_str(L,"  ");
                L_x(L,baseclass,2); L_ch(L,'.'); L_x(L,subclass,2); L_str(L,"  ");
                L_str(L,pci_class_name(baseclass,subclass));

                /* PCI-PCI bridge (class 0x06/sub 0x04): queue its secondary
                 * bus (config byte 0x19) for a later pass. */
                if(baseclass==0x06 && subclass==0x04){
                    UINT8 sec=(UINT8)((pci_read32(bus,dev,fn,0x18)>>8)&0xFF);
                    if(!seen[sec] && busn<(int)sizeof(buslist)){
                        seen[sec]=1; buslist[busn++]=sec;
                    }
                }
            }
        }
    }
    L_line(L,FOREB_DIM);
    L->text[L->n-1][0]=0; L->col[L->n-1]=FOREB_DIM;
    L_str(L,"Total functions: "); L_u(L,(UINT64)found);
    if(!found){ L_line(L,FOREB_TIMER); L_str(L,"No PCI devices (config mechanism #1 empty)."); }
}

/* ==========================================================================
 * 4. ACPI table lister (RSDP -> RSDT/XSDT).
 * ========================================================================== */
static EFI_GUID g_guid_acpi20 =
    { 0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81} };
static EFI_GUID g_guid_acpi10 =
    { 0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d} };
static EFI_GUID g_guid_smbios =
    { 0xeb9d2d31,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d} };
static EFI_GUID g_guid_smbios3 =
    { 0xf2fd1544,0x9794,0x4a2c,{0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94} };

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const UINT8 *x=(const UINT8*)a,*y=(const UINT8*)b;
    for(int i=0;i<16;i++) if(x[i]!=y[i]) return 0;
    return 1;
}
static void *find_cfg_table(const EFI_GUID *g)
{
    if(!gST || !gST->ConfigurationTable) return 0;
    for(UINTN i=0;i<gST->NumberOfTableEntries;i++)
        if(guid_eq(&gST->ConfigurationTable[i].VendorGuid,g))
            return gST->ConfigurationTable[i].VendorTable;
    return 0;
}

/* Emit one SDT header row: signature + address + length + OEM id. */
static void acpi_row(hwlist *L, const UINT8 *hdr, UINT64 phys)
{
    if(!hdr){ return; }
    L_line(L,FOREB_TEXT);
    L_str(L,"  ");
    for(int i=0;i<4;i++){ char c=(char)hdr[i]; L_ch(L,(c>=0x20&&c<0x7f)?c:'?'); }
    L_str(L," @0x"); L_x(L,phys,8);
    L_str(L," len "); L_u(L,rd32(hdr+4));
    L_str(L,"  OEM ");
    for(int i=10;i<16;i++){ char c=(char)hdr[i]; if(c>=0x20&&c<0x7f) L_ch(L,c); }
}

static void build_acpi(hwlist *L)
{
    L->n=0;
    const UINT8 *rsdp=(const UINT8*)find_cfg_table(&g_guid_acpi20);
    int is20=1;
    if(!rsdp){ rsdp=(const UINT8*)find_cfg_table(&g_guid_acpi10); is20=0; }
    if(!rsdp){
        L_line(L,FOREB_TIMER); L_str(L,"No ACPI RSDP in the EFI configuration table.");
        return;
    }
    L_line(L,FOREB_TITLE); L_str(L,"RSDP @0x"); L_x(L,(UINT64)(UINTN)rsdp,8);
    L_str(L," rev "); L_u(L,rsdp[15]);
    L_str(L,"  OEM ");
    for(int i=9;i<15;i++){ char c=(char)rsdp[i]; if(c>=0x20&&c<0x7f) L_ch(L,c); }

    UINT8 rev=rsdp[15];
    const UINT8 *root=0; int entsz=4; UINT64 rootphys=0;
    if(is20 && rev>=2){
        rootphys=rd64(rsdp+24);           /* XSDT */
        root=(const UINT8*)(UINTN)rootphys; entsz=8;
        L_line(L,FOREB_DIM); L_str(L,"Using XSDT @0x"); L_x(L,rootphys,8);
    } else {
        rootphys=rd32(rsdp+16);           /* RSDT */
        root=(const UINT8*)(UINTN)rootphys; entsz=4;
        L_line(L,FOREB_DIM); L_str(L,"Using RSDT @0x"); L_x(L,rootphys,8);
    }
    if(!root){ L_line(L,FOREB_TIMER); L_str(L,"Root table pointer is null."); return; }

    UINT32 rootlen=rd32(root+4);
    if(rootlen<36 || rootlen>0x10000){ L_line(L,FOREB_TIMER); L_str(L,"Bad root table length."); return; }
    UINT32 count=(rootlen-36)/(UINT32)entsz;

    L_line(L,FOREB_TITLE); L_str(L,"Tables ("); L_u(L,count); L_str(L,"):");
    for(UINT32 i=0;i<count && L->n<HW_MAXLINES-1;i++){
        const UINT8 *ep=root+36+i*(UINT32)entsz;
        UINT64 phys = (entsz==8)? rd64(ep) : (UINT64)rd32(ep);
        if(!phys) continue;
        acpi_row(L,(const UINT8*)(UINTN)phys,phys);
    }
}

/* ==========================================================================
 * 5. SMBIOS / DMI info.
 * ========================================================================== */
static const char *smbios_type_name(UINT8 t)
{
    switch(t){
        case 0:  return "BIOS Information";
        case 1:  return "System Information";
        case 2:  return "Baseboard";
        case 3:  return "Chassis";
        case 4:  return "Processor";
        case 7:  return "Cache";
        case 9:  return "System Slot";
        case 16: return "Physical Memory Array";
        case 17: return "Memory Device";
        case 19: return "Memory Array Mapped Addr";
        case 32: return "System Boot Info";
        case 127:return "End of Table";
        default: return "Type";
    }
}

/* Return the string with 1-based index `idx` from a structure's string set
 * (which starts at struct base + formatted-length). Bounded by `end`. */
static const char *smbios_string(const UINT8 *stru, const UINT8 *end, UINT8 idx)
{
    if(idx==0) return "";
    const UINT8 *s=stru+stru[1];            /* start of string set */
    UINT8 cur=1;
    while(s<end && *s){
        if(cur==idx) return (const char*)s;
        while(s<end && *s) s++;             /* skip to NUL */
        s++;                                /* past NUL    */
        cur++;
    }
    return "";
}

static void smbios_add_field(hwlist *L, const char *label, const char *val)
{
    if(!val || !val[0]) return;
    L_line(L,FOREB_DIM); L_str(L,"    "); L_str(L,label); L_str(L,": "); L_str(L,val);
}

static void build_smbios(hwlist *L)
{
    L->n=0;
    const UINT8 *ep=(const UINT8*)find_cfg_table(&g_guid_smbios3);
    const UINT8 *tbl=0; UINT64 tbllen=0; UINT16 count=0; int v3=0;

    if(ep && ep[0]=='_'&&ep[1]=='S'&&ep[2]=='M'&&ep[3]=='3'&&ep[4]=='_'){
        v3=1;
        tbllen=rd32(ep+0x0C);
        tbl=(const UINT8*)(UINTN)rd64(ep+0x10);
        L_line(L,FOREB_TITLE);
        L_str(L,"SMBIOS "); L_u(L,ep[7]); L_ch(L,'.'); L_u(L,ep[8]);
        L_str(L," (64-bit entry) max "); L_u(L,tbllen); L_str(L," B");
    } else {
        ep=(const UINT8*)find_cfg_table(&g_guid_smbios);
        if(ep && ep[0]=='_'&&ep[1]=='S'&&ep[2]=='M'&&ep[3]=='_'){
            tbllen=rd16(ep+0x16);
            tbl=(const UINT8*)(UINTN)(UINT64)rd32(ep+0x18);
            count=rd16(ep+0x1C);
            L_line(L,FOREB_TITLE);
            L_str(L,"SMBIOS "); L_u(L,ep[6]); L_ch(L,'.'); L_u(L,ep[7]);
            L_str(L," structures "); L_u(L,count);
        }
    }
    if(!tbl){
        L_line(L,FOREB_TIMER); L_str(L,"No SMBIOS entry point in the EFI configuration table.");
        return;
    }

    const UINT8 *p=tbl;
    const UINT8 *end=tbl+(tbllen? tbllen : 0x20000);
    int shown=0;
    for(int guard=0; guard<2048 && p+4<=end && L->n<HW_MAXLINES-4; guard++){
        UINT8 type=p[0], flen=p[1];
        if(flen<4) break;
        UINT16 handle=rd16(p+2);
        const UINT8 *strs=p+flen;
        /* find end of unformatted (string) area: double NUL */
        const UINT8 *q=strs;
        while(q+1<end && !(q[0]==0 && q[1]==0)) q++;
        const UINT8 *next=q+2;

        L_line(L,FOREB_TEXT);
        L_str(L,"Type "); L_u(L,type);
        L_str(L," ["); L_x(L,handle,4); L_str(L,"] ");
        L_str(L,smbios_type_name(type));
        shown++;

        if(type==0){
            smbios_add_field(L,"Vendor", smbios_string(p,end,p[4]));
            smbios_add_field(L,"Version",smbios_string(p,end,p[5]));
            smbios_add_field(L,"Date",   smbios_string(p,end,p[8]));
        } else if(type==1){
            smbios_add_field(L,"Manufacturer",smbios_string(p,end,p[4]));
            smbios_add_field(L,"Product",     smbios_string(p,end,p[5]));
            smbios_add_field(L,"Version",     smbios_string(p,end,p[6]));
            smbios_add_field(L,"Serial",      smbios_string(p,end,p[7]));
        } else if(type==4){
            smbios_add_field(L,"Socket",  smbios_string(p,end,p[4]));
            smbios_add_field(L,"Mfr",     smbios_string(p,end,p[7]));
            smbios_add_field(L,"Version", smbios_string(p,end,p[0x10]));
        } else if(type==17){
            if(flen>0x0D){
                UINT16 mb=rd16(p+0x0C);
                if(mb && mb!=0xFFFF){ L_line(L,FOREB_DIM); L_str(L,"    Size: "); L_u(L,mb); L_str(L," MB"); }
            }
            smbios_add_field(L,"Locator",smbios_string(p,end,flen>0x10?p[0x10]:0));
        }

        if(type==127) break;                /* end-of-table */
        if(next<=p) break;                  /* malformed    */
        p=next;
        if(count && shown>=count) break;
    }
    L_line(L,FOREB_DIM); L_str(L,"Structures shown: "); L_u(L,(UINT64)shown);
    (void)v3;
}

/* ==========================================================================
 * 6. Memory pattern tester (bounded LoaderData buffer).
 * ========================================================================== */
static const UINT64 g_mt_pat[] = {
    0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
    0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
    0x0123456789ABCDEFULL
};
static const int g_mt_pat_n = (int)(sizeof(g_mt_pat)/sizeof(g_mt_pat[0]));
static const int g_mt_size_mb[4] = { 1, 4, 8, 16 };

static void mt_size_line(hwlist *L)
{
    L_line(L,FOREB_TITLE);
    L_str(L,"Buffer size: "); L_u(L,(UINT64)g_mt_size_mb[L->sizesel]); L_str(L," MiB");
    L_str(L,"   (Left/Right to change)");
}

static void build_memtest(hwlist *L)
{
    L->n=0;
    L_line(L,FOREB_DIM);
    L_str(L,"Allocates EfiLoaderData, writes+verifies patterns, then frees.");
    mt_size_line(L);

    if(!L->ran){
        L_line(L,FOREB_TEXT); L_str(L,"Press Enter (or click) to run the test.");
        return;
    }
    if(!gBS){ L_line(L,FOREB_TIMER); L_str(L,"BootServices N/A - cannot allocate."); return; }

    UINTN bytes=(UINTN)g_mt_size_mb[L->sizesel]*1024u*1024u;
    UINT8 *buf=0;
    if(EFI_ERROR(gBS->AllocatePool(EfiLoaderData,bytes,(VOID**)&buf)) || !buf){
        L_line(L,FOREB_TIMER); L_str(L,"AllocatePool failed for "); L_u(L,(UINT64)(bytes>>20)); L_str(L," MiB.");
        return;
    }
    UINT64 *w=(UINT64*)buf;
    UINTN nq=bytes/sizeof(UINT64);
    UINT64 total_err=0;

    for(int pi=0; pi<g_mt_pat_n; pi++){
        UINT64 pat=g_mt_pat[pi];
        for(UINTN i=0;i<nq;i++) w[i]=pat;
        UINT64 err=0;
        for(UINTN i=0;i<nq;i++) if(w[i]!=pat) err++;
        total_err+=err;
        L_line(L, err? FOREB_TIMER : FOREB_TITLE);
        L_str(L,"  pattern 0x"); L_x(L,pat,16);
        L_str(L,err? "  FAIL errors=" : "  PASS errors=");
        L_u(L,err);
    }

    /* Address-in-address walk (each cell = its own index). */
    for(UINTN i=0;i<nq;i++) w[i]=(UINT64)i;
    {
        UINT64 err=0;
        for(UINTN i=0;i<nq;i++) if(w[i]!=(UINT64)i) err++;
        total_err+=err;
        L_line(L, err? FOREB_TIMER : FOREB_TITLE);
        L_str(L,"  addr-in-addr        ");
        L_str(L,err? "  FAIL errors=" : "  PASS errors=");
        L_u(L,err);
    }

    gBS->FreePool(buf);

    L_line(L, total_err? FOREB_TIMER : FOREB_WHITE);
    L_str(L, total_err? "RESULT: FAILED - total errors " : "RESULT: PASSED - total errors ");
    L_u(L,total_err);
    L_line(L,FOREB_DIM);
    L_str(L,"Tested "); L_u(L,(UINT64)nq); L_str(L," 64-bit cells over ");
    L_u(L,(UINT64)(g_mt_pat_n+1)); L_str(L," patterns.");
}

/* ==========================================================================
 * 7. GOP mode lister.
 * ========================================================================== */
static EFI_GUID g_guid_gop = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

static const char *gop_fmt_name(UINT32 f)
{
    return (f==PixelBlueGreenRedReserved8BitPerColor)?"BGRX":
           (f==PixelRedGreenBlueReserved8BitPerColor)?"RGBX":
           (f==PixelBitMask)?"BitMask":
           (f==PixelBltOnly)?"BltOnly":"?";
}

static void build_gop(hwlist *L)
{
    L->n=0;
    if(!gBS){ L_line(L,FOREB_TIMER); L_str(L,"BootServices N/A."); return; }
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop=0;
    if(EFI_ERROR(gBS->LocateProtocol(&g_guid_gop,NULL,(VOID**)&gop)) || !gop || !gop->Mode){
        L_line(L,FOREB_TIMER); L_str(L,"No Graphics Output Protocol located."); return;
    }
    UINT32 cur=gop->Mode->Mode, max=gop->Mode->MaxMode;
    L_line(L,FOREB_TITLE);
    L_str(L,"GOP: "); L_u(L,max); L_str(L," modes, current #"); L_u(L,cur);
    L_str(L,"  fb 0x"); L_x(L,gop->Mode->FrameBufferBase,1);
    L_str(L," "); L_u(L,(UINT64)gop->Mode->FrameBufferSize>>20); L_str(L," MiB");
    L_line(L,FOREB_DIM); L_str(L," #    resolution    fmt      pitch(px)");

    for(UINT32 m=0;m<max && L->n<HW_MAXLINES-1;m++){
        UINTN infosz=0; EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi=0;
        if(EFI_ERROR(gop->QueryMode(gop,m,&infosz,&mi)) || !mi) continue;
        L_line(L, m==cur? FOREB_WHITE : FOREB_TEXT);
        L_str(L, m==cur? "*" : " ");
        L_u(L,m); L_str(L,"   ");
        L_u(L,mi->HorizontalResolution); L_ch(L,'x'); L_u(L,mi->VerticalResolution);
        L_str(L,"   "); L_str(L,gop_fmt_name(mi->PixelFormat));
        L_str(L,"   "); L_u(L,mi->PixelsPerScanLine);
        /* QueryMode's Info buffer is caller-owned per the GOP spec; free it each
         * iteration or every mode leaks a pool block on every open/refresh. */
        if(gBS && mi) gBS->FreePool(mi);
    }
    L_line(L,FOREB_DIM); L_str(L,"(read-only; modes are not switched)");
}

/* ==========================================================================
 * 8. TSC frequency estimate (rdtsc + Stall).
 * ========================================================================== */
static void build_tsc(hwlist *L)
{
    L->n=0;
    L_line(L,FOREB_DIM); L_str(L,"Estimates TSC rate: rdtsc, Stall(N ms), rdtsc.");
    if(!gBS || !gBS->Stall){
        L_line(L,FOREB_TIMER); L_str(L,"BootServices Stall N/A - cannot time."); return;
    }
    static int has_tsc=-1;      /* CPU feature bits are invariant for the boot session. */
    if(has_tsc<0){
        UINT32 r[4]; hw_cpuid(0,0,r);
        has_tsc=(r[0]<1)?0:(int)((hw_cpuid(1,0,r),r[3])&(1u<<4));
    }
    if(!has_tsc){
        L_line(L,FOREB_TIMER); L_str(L,"CPU reports no TSC (CPUID.1:EDX.4=0)."); return;
    }

    /* Single 50 ms window: keeps the cursor freeze to ~50 ms on uncached VRAM
     * (was {20,50,100} ms = ~170 ms of blocking Stall). */
    static const UINT32 win_ms[1]={50};
    UINT64 best_hz=0;
    for(int i=0;i<1;i++){
        UINT64 t0=hw_rdtsc();
        hw_stall((UINTN)win_ms[i]*1000u);
        UINT64 t1=hw_rdtsc();
        UINT64 dt=t1-t0;
        UINT64 hz=dt*1000u/win_ms[i];
        best_hz=hz;
        L_line(L,FOREB_TEXT);
        L_str(L,"  "); L_u(L,win_ms[i]); L_str(L," ms window: dticks ");
        L_u(L,dt); L_str(L,"  ~"); L_u(L,hz/1000000u); L_str(L," MHz");
    }
    L_line(L,FOREB_WHITE);
    L_str(L,"Estimated TSC: "); L_u(L,best_hz/1000000u);
    L_str(L,"."); {
        UINT64 frac=(best_hz/1000u)%1000u;
        if(frac<100) L_ch(L,'0'); if(frac<10) L_ch(L,'0'); L_u(L,frac);
    }
    L_str(L," MHz");
    L_line(L,FOREB_DIM); L_str(L,"Stall precision is firmware-dependent; treat as approximate.");
}

/* ==========================================================================
 * 9. Timer / PIT test (8254 channel 0 + PC-speaker channel 2).
 * ========================================================================== */
#define PIT_HZ 1193182u

/* Latch + read the current 16-bit count of PIT channel 0. */
static UINT16 pit_read_ch0(void)
{
    io_outb(0x43, 0x00);                     /* latch counter 0 */
    UINT8 lo=io_inb(0x40);
    UINT8 hi=io_inb(0x40);
    return (UINT16)(lo|(hi<<8));
}

static void pit_beep(UINT32 freq, UINT32 ms)
{
    if(!gBS || !gBS->Stall || freq==0){ return; }
    UINT32 div=PIT_HZ/freq; if(div==0) div=1;
    io_outb(0x43, 0xB6);                     /* ch2, lobyte/hibyte, square wave */
    io_outb(0x42, (UINT8)(div&0xFF));
    io_outb(0x42, (UINT8)((div>>8)&0xFF));
    UINT8 g=io_inb(0x61);
    io_outb(0x61, (UINT8)(g|0x03));          /* enable speaker + gate 2 */
    hw_stall((UINTN)ms*1000u);
    io_outb(0x61, (UINT8)(g&~0x03));         /* restore */
}

static void build_pit(hwlist *L)
{
    L->n=0;
    L_line(L,FOREB_TITLE); L_str(L,"Intel 8254 PIT (legacy timer @ 1.193182 MHz)");

    /* Sample channel-0 counter twice to prove it is ticking. */
    UINT16 c0=pit_read_ch0();
    hw_stall(2000);                          /* 2 ms */
    UINT16 c1=pit_read_ch0();
    L_line(L,FOREB_TEXT);
    L_str(L,"ch0 count: "); L_u(L,c0); L_str(L," -> "); L_u(L,c1);

    if(gBS && gBS->Stall){
        /* Measure decrements over ~10 ms to recover the input frequency. */
        UINT16 a=pit_read_ch0();
        hw_stall(10000);                     /* 10 ms */
        UINT16 b=pit_read_ch0();
        UINT32 dec = (a>=b)? (UINT32)(a-b) : (UINT32)(a + (0x10000u - b));
        UINT32 hz = dec*100u;                /* dec per 10 ms -> per second */
        L_line(L,FOREB_TEXT);
        L_str(L,"decrements/10ms: "); L_u(L,dec);
        L_str(L,"  => ~"); L_u(L,hz/1000u); L_str(L," kHz");
        int ok = (c0!=c1) && dec>0;
        L_line(L, ok? FOREB_WHITE : FOREB_TIMER);
        L_str(L, ok? "RESULT: PIT is counting (PASS)" :
                     "RESULT: counter not advancing (firmware may own it)");
    } else {
        L_line(L,FOREB_DIM); L_str(L,"Stall N/A: cannot measure frequency.");
    }

    L_line(L,FOREB_DIM); L_str(L,"Space plays a 1 kHz beep on the PC speaker (PIT ch2).");
    L_line(L,FOREB_DIM); L_str(L,"Enter / click re-samples the counter.");
}

/* ==========================================================================
 * Shared event callback.
 * ========================================================================== */
static int hexdigit(CHAR16 c)
{
    return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
}
static char upcase(CHAR16 c){ return (c>='a'&&c<='f')?(char)(c-'a'+'A'):(char)c; }

/* Primary "do it" action for the interactive tools. */
static void hw_action(hwlist *L)
{
    switch(L->kind){
        case HW_MEMTEST: L->ran=1; build_memtest(L); L->scroll=0; break;
        case HW_TSC:     build_tsc(L);  L->scroll=0; break;
        case HW_PIT:     build_pit(L);  L->scroll=0; break;
        case HW_MSR:     msr_read_typed(L); break;
        default:         if(L->rebuild) L->rebuild(L); break;
    }
}

static int hw_event(wm_window *w, const wm_event *ev)
{
    hwlist *L=(hwlist*)wm_user(w);
    if(!L || !ev) return 0;
    int rows=hw_rows(wm_client_h(w));

    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;

            if(L->kind==HW_MSR){
                if(hexdigit(ev->unicode) && L->inlen<8){
                    L->input[L->inlen++]=upcase(ev->unicode); L->input[L->inlen]=0; return 0;
                }
                if(ev->unicode==CHAR_BACKSPACE && L->inlen>0){
                    L->inlen--; L->input[L->inlen]=0; return 0;
                }
                if(ev->unicode==CHAR_CR){ msr_read_typed(L); return 0; }
                if(ev->unicode=='r'||ev->unicode=='R'){ build_msr(L); return 0; }
            } else if(L->kind==HW_MEMTEST){
                if(ev->scancode==SCAN_LEFT){ if(L->sizesel>0)L->sizesel--; L->ran=0; build_memtest(L); return 0; }
                if(ev->scancode==SCAN_RIGHT){ if(L->sizesel<3)L->sizesel++; L->ran=0; build_memtest(L); return 0; }
                if(ev->unicode==CHAR_CR){ hw_action(L); return 0; }
                if(ev->unicode=='r'||ev->unicode=='R'){ L->ran=0; build_memtest(L); return 0; }
            } else if(L->kind==HW_PIT){
                if(ev->unicode==' '){ pit_beep(1000,90); return 0; }
                if(ev->unicode==CHAR_CR){ hw_action(L); return 0; }
                if(ev->unicode=='r'||ev->unicode=='R'){ build_pit(L); return 0; }
            } else if(L->kind==HW_TSC){
                if(ev->unicode==CHAR_CR){ hw_action(L); return 0; }
                if(ev->unicode=='r'||ev->unicode=='R'){ build_tsc(L); return 0; }
            } else {
                if((ev->unicode=='r'||ev->unicode=='R'||ev->unicode==CHAR_CR) && L->rebuild){
                    L->rebuild(L); L->scroll=0; return 0;
                }
            }

            /* Common scroll keys. */
            if(ev->scancode==SCAN_UP)        L->scroll--;
            else if(ev->scancode==SCAN_DOWN) L->scroll++;
            else if(ev->scancode==SCAN_PAGE_UP)   L->scroll-=rows;
            else if(ev->scancode==SCAN_PAGE_DOWN) L->scroll+=rows;
            else if(ev->scancode==SCAN_HOME) L->scroll=0;
            else if(ev->scancode==SCAN_END)  L->scroll=L->n;
            hw_clamp(L,rows);
            return 0;

        case WM_EV_MOUSE_WHEEL:
            L->scroll -= ev->wheel*2;
            hw_clamp(L,rows);
            return 0;

        case WM_EV_MOUSE_DOWN:
            if(L->kind==HW_MEMTEST || L->kind==HW_TSC || L->kind==HW_PIT){
                hw_action(L);
            }
            return 0;

        case WM_EV_CLOSE:
            L->win=NULL;
            return 0;

        default:
            return 0;
    }
}

/* ==========================================================================
 * Openers (template B).
 * ========================================================================== */
static hwlist g_cpuid, g_msr, g_pci, g_acpi, g_smbios, g_memtest, g_gop, g_tsc, g_pit;

static void hw_zero(hwlist *L)
{
    /* Zero without libc/BootServices dependency. */
    UINT8 *p=(UINT8*)L;
    for(UINTN i=0;i<sizeof(*L);i++) p[i]=0;
}

static void hw_open(hwlist *L, int kind, const char *title,
                    void (*rebuild)(hwlist*), int wpct, int hpct)
{
    if(L->win) return;                       /* idempotent single instance */
    hw_zero(L);
    L->kind=kind; L->title=title; L->rebuild=rebuild;
    if(rebuild) rebuild(L);

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*wpct/100; if(ww<460)ww=460; if(ww>900)ww=900; if(ww>W-40)ww=W-40;
    int wh=H*hpct/100; if(wh<300)wh=300; if(wh>680)wh=680; if(wh>H-40)wh=H-40;
    L->win=wm_open(title, ww, wh, hw_render, hw_event, L);
}

void tool_hw_cpuid_open(void)  { hw_open(&g_cpuid, HW_CPUID, "CPUID Viewer",   build_cpuid,  62, 66); }
void tool_hw_msr_open(void)    { hw_open(&g_msr,   HW_MSR,   "MSR Reader",      build_msr,    64, 62); }
void tool_hw_pci_open(void)    { hw_open(&g_pci,   HW_PCI,   "PCI Devices",     build_pci,    70, 64); }
void tool_hw_acpi_open(void)   { hw_open(&g_acpi,  HW_ACPI,  "ACPI Tables",     build_acpi,   66, 60); }
void tool_hw_smbios_open(void) { hw_open(&g_smbios,HW_SMBIOS,"SMBIOS / DMI",    build_smbios, 68, 64); }
void tool_hw_memtest_open(void){ hw_open(&g_memtest,HW_MEMTEST,"Memory Tester", build_memtest,60, 58); }
void tool_hw_gop_open(void)    { hw_open(&g_gop,   HW_GOP,   "GOP Modes",       build_gop,    62, 62); }
void tool_hw_tsc_open(void)    { hw_open(&g_tsc,   HW_TSC,   "TSC Frequency",   build_tsc,    58, 52); }
void tool_hw_pit_open(void)    { hw_open(&g_pit,   HW_PIT,   "Timer / PIT Test",build_pit,    60, 54); }

/* ==========================================================================
 * Category table.
 * ========================================================================== */
const struct forebo_tool cat_hw_tools[] = {
    { "CPUID Viewer",  "CPU vendor, family/model, feature flags",   "gear",     tool_hw_cpuid_open  },
    { "MSR Reader",    "Read curated architectural MSRs (guarded)", "gear",     tool_hw_msr_open    },
    { "PCI Devices",   "Config-space scan: vendor/device/class",    "disk",     tool_hw_pci_open    },
    { "ACPI Tables",   "Walk RSDP -> RSDT/XSDT table signatures",   "text",     tool_hw_acpi_open   },
    { "SMBIOS / DMI",  "Firmware/board/CPU/memory inventory",       "text",     tool_hw_smbios_open },
    { "Memory Tester", "Pattern-test a bounded RAM buffer",         "safe",     tool_hw_memtest_open},
    { "GOP Modes",     "List every graphics QueryMode mode",        "gear",     tool_hw_gop_open    },
    { "TSC Frequency", "Estimate TSC rate via rdtsc + Stall",       "terminal", tool_hw_tsc_open    },
    { "Timer / PIT",   "8254 PIT counter + PC-speaker beep test",   "terminal", tool_hw_pit_open    },
};
const int cat_hw_count = (int)(sizeof(cat_hw_tools)/sizeof(cat_hw_tools[0]));

/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_convert.c - "Converters" tool category (KEY = convert).
 * =============================================================================
 * Nine self-contained template-B GUI tools. All math is 64-bit INTEGER /
 * fixed-point (no float: -mno-sse/-mno-mmx). Every window fills its client
 * area, clips all text with draw_string_clip, and responds to keyboard + mouse.
 * NO firmware services -> NO cat_convert_init().
 * ========================================================================== */
#include "tools_convert.h"
#include "../efi.h"
#include "../ui.h"
#include "../core/wm.h"
#include "../core/input.h"

/* ==========================================================================
 * Small freestanding helpers (file-local; no libc).
 * ========================================================================== */
#define CV_PAD 10

static int cv_len(const char *s){ int n=0; if(!s) return 0; while(s[n]) n++; return n; }

static void cv_copy(char *d, const char *s, int max){
    int i=0; if(max<=0){ return; }
    if(!s){ if(max>0) d[0]=0; return; }
    for(; s[i] && i<max-1; i++) d[i]=s[i];
    d[i]=0;
}

static char cv_upper(char c){ return (c>='a'&&c<='z') ? (char)(c-'a'+'A') : c; }

/* Compare two NUL-terminated strings for equality (no libc strcmp). */
static int cv_eq(const char *a, const char *b){
    int i=0; for(; a[i] && b[i]; i++) if(a[i]!=b[i]) return 0;
    return a[i]==b[i];
}

/* Unsigned 64-bit -> decimal. Returns length. out >= 21 bytes. */
static int cv_u64toa(UINT64 v, char *out){
    char t[24]; int i=0;
    if(!v) t[i++]='0';
    while(v){ t[i++]=(char)('0'+(v%10ULL)); v/=10ULL; }
    int o=0; while(i>0) out[o++]=t[--i]; out[o]=0; return o;
}

/* Unsigned -> arbitrary base (2..16), UPPERCASE, min width padded with '0'. */
static void cv_u64tobase(UINT64 v, int base, char *out, int minw){
    static const char D[]="0123456789ABCDEF";
    char t[70]; int i=0;
    if(base<2||base>16){ out[0]=0; return; }
    if(!v) t[i++]='0';
    while(v){ t[i++]=D[v%(UINT64)base]; v/=(UINT64)base; }
    while(i<minw && i<69) t[i++]='0';
    int o=0; while(i>0) out[o++]=t[--i]; out[o]=0;
}

/* Parse a string in `base` (2..16), spaces ignored. Returns 1 + *out on
 * success, 0 on any invalid digit / empty. */
static int cv_parse_base(const char *s, int base, UINT64 *out){
    UINT64 v=0; int any=0;
    for(; *s; s++){
        char c=*s; int d;
        if(c==' ') continue;
        if(c>='0'&&c<='9') d=c-'0';
        else if(c>='a'&&c<='f') d=c-'a'+10;
        else if(c>='A'&&c<='F') d=c-'A'+10;
        else return 0;
        if(d>=base) return 0;
        if(v > (~0ULL - (UINT64)d)/(UINT64)base) { *out=~0ULL; return 1; } /* saturate */
        v=v*(UINT64)base+(UINT64)d; any=1;
    }
    if(!any) return 0;
    *out=v; return 1;
}

/* Parse a signed decimal integer. Returns 1 + *out on success. */
static int cv_parse_int(const char *s, INT64 *out){
    INT64 v=0; int neg=0, any=0; const char *p=s;
    while(*p==' ') p++;
    if(*p=='-'){ neg=1; p++; } else if(*p=='+'){ p++; }
    for(; *p; p++){
        if(*p==' ') continue;
        if(*p<'0'||*p>'9') return 0;
        if(v > (INT64)922337203685477580LL ||
           (v == (INT64)922337203685477580LL && *p > '7'))
            { v=(INT64)9223372036854775807LL; break; }   /* saturate, no overflow */
        v=v*10+(*p-'0'); any=1;
    }
    if(!any) return 0;
    *out = neg ? -v : v; return 1;
}

/* Format a scaled integer with `dec` decimal places (e.g. 1550,2 -> "15.50"). */
static void cv_fixfmt(char *out, INT64 scaled, int dec){
    int o=0; UINT64 u;
    if(scaled<0){ out[o++]='-'; u=(UINT64)(-(scaled+1))+1ULL; } else u=(UINT64)scaled;
    UINT64 p=1; for(int i=0;i<dec;i++) p*=10ULL;
    UINT64 ip=u/p, fp=u%p;
    o+=cv_u64toa(ip, out+o);
    if(dec>0){
        out[o++]='.';
        char fb[24]; int fl=cv_u64toa(fp, fb);
        for(int i=0;i<dec-fl;i++) out[o++]='0';
        for(int i=0;i<fl;i++) out[o++]=fb[i];
    }
    out[o]=0;
}

/* ---- one-line text editor over a fixed buffer -------------------------- */
typedef struct { char buf[80]; int len; } cv_edit;
static void cv_edit_clear(cv_edit *e){ e->len=0; e->buf[0]=0; }
static void cv_edit_putc(cv_edit *e, char c){
    if(e->len < (int)sizeof(e->buf)-1){ e->buf[e->len++]=c; e->buf[e->len]=0; }
}
static void cv_edit_bksp(cv_edit *e){ if(e->len>0){ e->buf[--e->len]=0; } }

/* ---- theme snapshot used by all draw callbacks ------------------------- */
typedef struct { UINT32 bg,fg,accent,selbg,selfg,panel,dim,border; } cv_pal;
static cv_pal cv_theme(void){
    static int have_cache=0;
    static cv_pal cache;
    UINT32 bg     = wm_theme_color(WM_COL_WINDOW);
    UINT32 fg     = wm_theme_color(WM_COL_FG);
    UINT32 accent = wm_theme_color(WM_COL_ACCENT);
    UINT32 selbg  = wm_theme_color(WM_COL_SEL_BG);
    UINT32 selfg  = wm_theme_color(WM_COL_SEL_FG);
    if(have_cache && cache.bg==bg && cache.fg==fg && cache.accent==accent &&
       cache.selbg==selbg && cache.selfg==selfg)
        return cache;
    cv_pal p;
    p.bg=bg; p.fg=fg; p.accent=accent; p.selbg=selbg; p.selfg=selfg;
    p.panel  = wm_blend(p.bg, 0x00000000u, 90);
    p.dim    = wm_blend(p.fg, p.bg, 120);
    p.border = wm_blend(p.bg, p.fg, 60);
    cache = p; have_cache = 1;
    return p;
}

/* Draw a titled input field. Returns the y just below it (screen coords). */
static int cv_input_box(int cx, int y, int cw, const cv_pal *p,
                        const char *label, const char *text){
    int S=ui_scale(); int chh=16*S;
    int bx=cx+CV_PAD, bw=cw-2*CV_PAD;
    draw_string_clip(bx, y, bw, label, p->dim, p->bg, 1, 1);
    y += chh + 2*S;
    int bh=chh+8*S;
    fill_rect(bx, y, bw, bh, p->panel);
    draw_rect_outline(bx, y, bw, bh, 1, p->border);
    const char *shown = (text && text[0]) ? text : "";
    draw_string_clip(bx+6*S, y+4*S, bw-12*S, shown, p->fg, p->panel, 1, 1);
    /* caret */
    int tw = cv_len(shown)*8*S;
    int caretx = bx+6*S+tw;
    if(caretx < bx+bw-2*S) fill_rect(caretx, y+4*S, 2*S, chh, p->accent);
    return y + bh + 6*S;
}

/* A labelled output row "label: value". */
static void cv_row(int cx, int y, int cw, const cv_pal *p,
                   const char *label, const char *value, int scale){
    int S=ui_scale();
    int bx=cx+CV_PAD, bw=cw-2*CV_PAD;
    int lw = cv_len(label)*8*S*scale;
    draw_string_clip(bx, y, bw, label, p->dim, p->bg, 1, scale);
    draw_string_clip(bx+lw+6*S, y, bw-lw-6*S, value, p->fg, p->bg, 1, scale);
}

/* ---- reusable button row (client coords) ------------------------------- */
static void cv_btn_set(wm_button *b, int id, const char *lb, int x, int y){
    b->x=x; b->y=y; b->w=wm_button_measure(lb); b->h=wm_button_h();
    b->id=id; b->enabled=1; cv_copy(b->label, lb, 28);
}
static int cv_btn_hit(const wm_button *b, int n, int mx, int my){
    for(int i=0;i<n;i++) if(b[i].enabled && wm_button_hit(&b[i],mx,my)) return b[i].id;
    return 0;
}
static void cv_btn_draw_row(const wm_button *b, int n, int hover, int press){
    for(int i=0;i<n;i++) wm_button_draw(&b[i], b[i].id==hover, b[i].id==press);
}

/* ==========================================================================
 * (1) BASE CONVERTER - dec / hex / bin / oct live.
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int base_idx; int b_hover, b_press;
                 char cache_in[80]; int cache_base; char cache_dec[24], cache_hx[24], cache_oc[24], cache_bn[70];
                 int cache_ok; int cache_valid; } base_state;
static base_state g_base;

static const int   BASE_VAL[4] = { 10, 16, 2, 8 };
static const char *BASE_LBL[4] = { "DEC", "HEX", "BIN", "OCT" };

/* buttons: 4 base tabs (id 1..4) + Clear (id 5). client coords. */
static int base_btns(int cw, int ch, wm_button *out){
    int S=ui_scale(), x=CV_PAD, y=CV_PAD; (void)ch;
    for(int i=0;i<4;i++){ cv_btn_set(&out[i], i+1, BASE_LBL[i], x, y); x+=out[i].w+6*S; }
    int clx = cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[4], 5, "Clear", clx<x?x:clx, y);
    return 5;
}

static void base_draw(wm_window *w, int cx, int cy, int cw, int ch){
    base_state *s=(base_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[5]; int nb=base_btns(cw,ch,b);
    /* draw: active tab uses the pressed style */
    for(int i=0;i<nb;i++){
        int active = (i<4 && i==s->base_idx);
        wm_button_draw(&b[i], b[i].id==s->b_hover, active || b[i].id==s->b_press);
    }
    int S=ui_scale();
    int y = cy + CV_PAD + wm_button_h() + 8*S;
    char lbl[32]; cv_copy(lbl,"Input (",32); { int l=cv_len(lbl); cv_copy(lbl+l,BASE_LBL[s->base_idx],32-l);} { int l=cv_len(lbl); cv_copy(lbl+l,")",32-l);}
    y = cv_input_box(cx, y, cw, &p, lbl, s->in.buf);
    y += 4*S;

    if(!s->cache_valid || s->cache_base!=s->base_idx || !cv_eq(s->cache_in, s->in.buf)){
        UINT64 v=0; int ok = cv_parse_base(s->in.buf, BASE_VAL[s->base_idx], &v);
        if(ok){
            cv_u64toa(v,s->cache_dec);
            cv_u64tobase(v,16,s->cache_hx,0);
            cv_u64tobase(v,8,s->cache_oc,0);
            cv_u64tobase(v,2,s->cache_bn,0);
        } else { cv_copy(s->cache_dec,"-",24); cv_copy(s->cache_hx,"-",24); cv_copy(s->cache_oc,"-",24); cv_copy(s->cache_bn,"-",70); }
        s->cache_ok = ok;
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_base = s->base_idx;
        s->cache_valid = 1;
    }
    int lh=16*S+6*S;
    char hbuf[26]; hbuf[0]='0'; hbuf[1]='x'; cv_copy(hbuf+2,s->cache_hx,24);
    cv_row(cx,y,cw,&p,"DEC:",s->cache_dec,1); y+=lh;
    cv_row(cx,y,cw,&p,"HEX:",hbuf,1); y+=lh;
    cv_row(cx,y,cw,&p,"OCT:",s->cache_oc,1); y+=lh;
    cv_row(cx,y,cw,&p,"BIN:",s->cache_bn,1); y+=lh;
    if(!s->cache_ok && s->in.len)
        draw_string_clip(cx+CV_PAD, y+4*S, cw-2*CV_PAD, "invalid digits for base",
                         0x00FF5A5Au, p.bg, 1, 1);
}

static int base_event(wm_window *w, const wm_event *ev){
    base_state *s=(base_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_LEFT){ s->base_idx=(s->base_idx+3)&3; return 0; }
            if(ev->scancode==SCAN_RIGHT || ev->unicode==CHAR_TAB){ s->base_idx=(s->base_idx+1)&3; return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode;
              if((u>='0'&&u<='9')||(u>='a'&&u<='f')||(u>='A'&&u<='F'))
                  cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[5]; int nb=base_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[5]; int nb=base_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id) s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press) return 0; wm_button b[5]; int nb=base_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id>=1&&id<=4) s->base_idx=id-1; else if(id==5) cv_edit_clear(&s->in); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_base_open(void){
    if(g_base.win) return;
    cv_edit_clear(&g_base.in); g_base.base_idx=0; g_base.b_hover=0; g_base.b_press=0; g_base.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*46/100; if(ww<380)ww=380; if(ww>560)ww=560; if(ww>W-40)ww=W-40;
    int wh=H*46/100; if(wh<300)wh=300; if(wh>420)wh=420; if(wh>H-40)wh=H-40;
    g_base.win=wm_open("Base Converter", ww, wh, base_draw, base_event, &g_base);
}

/* ==========================================================================
 * (2) ASCII TABLE - printable glyph + dec/hex code, scroll 0..255.
 * ========================================================================== */
typedef struct { wm_window *win; int scroll; } ascii_state;
static ascii_state g_ascii;

static const char *ASCII_CTRL[33] = {
    "NUL","SOH","STX","ETX","EOT","ENQ","ACK","BEL","BS","TAB","LF","VT","FF",
    "CR","SO","SI","DLE","DC1","DC2","DC3","DC4","NAK","SYN","ETB","CAN","EM",
    "SUB","ESC","FS","GS","RS","US","SPC"
};

static void ascii_draw(wm_window *w, int cx, int cy, int cw, int ch){
    ascii_state *s=(ascii_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    int S=ui_scale(); int lh=16*S+2*S;
    int y0=cy+CV_PAD;
    /* header */
    draw_string_clip(cx+CV_PAD, y0, cw-2*CV_PAD, "DEC  HEX  CHR  NAME", p.accent, p.bg, 1, 1);
    y0 += lh + 2*S;
    int rows=(cy+ch-y0)/lh; if(rows<1)rows=1;
    int total=256;
    if(s->scroll>total-rows) s->scroll=total-rows;
    if(s->scroll<0) s->scroll=0;
    for(int r=0;r<rows;r++){
        int code=s->scroll+r; if(code>=total) break;
        int ry=y0+r*lh;
        char line[48]; int o=0;
        char t[8]; int tl=cv_u64toa((UINT64)code,t);
        for(int i=0;i<3-tl;i++) line[o++]=' ';
        for(int i=0;i<tl;i++) line[o++]=t[i];
        line[o++]=' '; line[o++]=' ';
        char hx[8]; cv_u64tobase((UINT64)code,16,hx,2); line[o++]='0';line[o++]='x';
        for(int i=0;hx[i];i++) line[o++]=hx[i];
        line[o++]=' '; line[o]=0;   /* glyph column = char 10 (under "CHR") */
        draw_string_clip(cx+CV_PAD, ry, cw-2*CV_PAD, line, p.fg, p.bg, 1, 1);
        int gx = cx+CV_PAD + o*8*S;
        /* glyph */
        if(code>=32 && code<127) draw_char(gx, ry, (char)code, p.selfg, p.bg, 1, 1);
        else fill_rect(gx, ry+6*S, 6*S, 4*S, p.dim);
        /* name for control chars / DEL, at char 15 (under "NAME") */
        const char *nm=0;
        if(code<=32) nm=ASCII_CTRL[code];
        else if(code==127) nm="DEL";
        if(nm) draw_string_clip(gx+5*8*S, ry, cw-2*CV_PAD-(gx-cx)-5*8*S, nm, p.dim, p.bg, 1, 1);
    }
    /* scrollbar */
    if(total>rows){
        int barw=6*S, tx=cx+cw-barw-2*S, th=rows*lh;
        fill_rect(tx,y0,barw,th,p.border);
        int thh=rows*th/total; if(thh<8*S)thh=8*S;
        int ty=y0 + s->scroll*(th-thh)/(total-rows);
        fill_rect(tx,ty,barw,thh,p.accent);
    }
}

static int ascii_event(wm_window *w, const wm_event *ev){
    ascii_state *s=(ascii_state*)wm_user(w); if(!s) return 0;
    int ch=wm_client_h(w); int S=ui_scale(); int lh=16*S+2*S;
    int y0=CV_PAD+lh+2*S;          /* same header offset as ascii_draw() */
    int rows=(ch-y0)/lh; if(rows<1)rows=1;
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_UP) s->scroll--;
            else if(ev->scancode==SCAN_DOWN) s->scroll++;
            else if(ev->scancode==SCAN_PAGE_UP) s->scroll-=rows;
            else if(ev->scancode==SCAN_PAGE_DOWN) s->scroll+=rows;
            else if(ev->scancode==SCAN_HOME) s->scroll=0;
            else if(ev->scancode==SCAN_END) s->scroll=256;
            if(s->scroll<0) s->scroll=0;
            if(s->scroll>256) s->scroll=256;
            return 0;
        case WM_EV_MOUSE_WHEEL:
            s->scroll-=ev->wheel;
            if(s->scroll<0) s->scroll=0;
            if(s->scroll>256) s->scroll=256;
            return 0;
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_ascii_open(void){
    if(g_ascii.win) return;
    g_ascii.scroll=32;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*40/100; if(ww<340)ww=340; if(ww>480)ww=480; if(ww>W-40)ww=W-40;
    int wh=H*60/100; if(wh<320)wh=320; if(wh>620)wh=620; if(wh>H-40)wh=H-40;
    g_ascii.win=wm_open("ASCII Table", ww, wh, ascii_draw, ascii_event, &g_ascii);
}

/* ==========================================================================
 * (3) BASE64 - encode / decode a typed string.
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int decode; int b_hover, b_press;
                 char cache_in[80]; int cache_decode; char cache_out[512]; int cache_valid; } b64_state;
static b64_state g_b64;

static const char B64_ENC[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_dec_val(char c){
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='a'&&c<='z') return c-'a'+26;
    if(c>='0'&&c<='9') return c-'0'+52;
    if(c=='+') return 62;
    if(c=='/') return 63;
    return -1;
}

/* encode src[len] -> out (NUL-terminated, capped). */
static void b64_encode(const char *src, int len, char *out, int outcap){
    int o=0;
    for(int i=0;i<len;i+=3){
        unsigned b0=(unsigned char)src[i];
        unsigned b1=(i+1<len)?(unsigned char)src[i+1]:0;
        unsigned b2=(i+2<len)?(unsigned char)src[i+2]:0;
        unsigned trip=(b0<<16)|(b1<<8)|b2;
        if(o+4>=outcap) break;
        out[o++]=B64_ENC[(trip>>18)&63];
        out[o++]=B64_ENC[(trip>>12)&63];
        out[o++]=(i+1<len)?B64_ENC[(trip>>6)&63]:'=';
        out[o++]=(i+2<len)?B64_ENC[trip&63]:'=';
    }
    out[o]=0;
}

/* decode src -> out (printable, non-printable shown as '.'). */
static void b64_decode(const char *src, char *out, int outcap){
    int o=0; unsigned acc=0; int bits=0;
    for(const char *p=src; *p; p++){
        if(*p=='=') break;                       /* padding = end of stream */
        if(*p==' '||*p=='\n'||*p=='\r') continue;
        int v=b64_dec_val(*p); if(v<0) continue;
        acc=(acc<<6)|(unsigned)v; bits+=6;
        if(bits>=8){ bits-=8; unsigned byte=(acc>>bits)&0xFFu;
            acc &= (bits>0) ? ((1u<<bits)-1u) : 0u;   /* drop consumed bits */
            if(o+1>=outcap) break;
            out[o++]=(byte>=0x20u&&byte<0x7fu)?(char)byte:'.';
        }
    }
    out[o]=0;
}

static int b64_btns(int cw, int ch, wm_button *out){
    int S=ui_scale(), x=CV_PAD, y=CV_PAD; (void)ch;
    cv_btn_set(&out[0], 1, "Encode", x, y); x+=out[0].w+6*S;
    cv_btn_set(&out[1], 2, "Decode", x, y); x+=out[1].w+6*S;
    int clx=cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[2], 3, "Clear", clx<x?x:clx, y);
    return 3;
}

/* Draw a possibly long string wrapped across lines within [bx,bx+bw]. */
static int cv_wrap(int bx, int y, int bw, const char *s, cv_pal *p){
    int S=ui_scale(); int per=(bw)/(8*S); if(per<1)per=1;
    int lh=16*S+2*S; int n=cv_len(s);
    char line[128];
    for(int i=0;i<n;i+=per){
        int c=0; for(; c<per && i+c<n && c<127; c++) line[c]=s[i+c]; line[c]=0;
        draw_string_clip(bx,y,bw,line,p->fg,p->bg,1,1); y+=lh;
    }
    if(n==0) y+=lh;
    return y;
}

static void b64_draw(wm_window *w, int cx, int cy, int cw, int ch){
    b64_state *s=(b64_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[3]; int nb=b64_btns(cw,ch,b);
    for(int i=0;i<nb;i++){
        int active=(i==0&&!s->decode)||(i==1&&s->decode);
        wm_button_draw(&b[i], b[i].id==s->b_hover, active||b[i].id==s->b_press);
    }
    int S=ui_scale();
    int y=cy+CV_PAD+wm_button_h()+8*S;
    y=cv_input_box(cx,y,cw,&p, s->decode?"Base64 to decode:":"Text to encode:", s->in.buf);
    y+=4*S;
    if(!s->cache_valid || s->cache_decode!=s->decode || !cv_eq(s->cache_in, s->in.buf)){
        if(s->decode) b64_decode(s->in.buf, s->cache_out, sizeof(s->cache_out));
        else          b64_encode(s->in.buf, s->in.len, s->cache_out, sizeof(s->cache_out));
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_decode = s->decode;
        s->cache_valid = 1;
    }
    draw_string_clip(cx+CV_PAD,y,cw-2*CV_PAD, s->decode?"Decoded:":"Base64:", p.dim,p.bg,1,1);
    y+=16*S+2*S;
    cv_wrap(cx+CV_PAD, y, cw-2*CV_PAD, s->cache_out, &p);
}

static int b64_event(wm_window *w, const wm_event *ev){
    b64_state *s=(b64_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_TAB){ s->decode=!s->decode; return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode; if(u>=0x20 && u<0x7f) cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[3]; int nb=b64_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[3]; int nb=b64_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[3]; int nb=b64_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id==1)s->decode=0; else if(id==2)s->decode=1; else if(id==3)cv_edit_clear(&s->in); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_base64_open(void){
    if(g_b64.win) return;
    cv_edit_clear(&g_b64.in); g_b64.decode=0; g_b64.b_hover=0; g_b64.b_press=0; g_b64.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*48/100; if(ww<400)ww=400; if(ww>600)ww=600; if(ww>W-40)ww=W-40;
    int wh=H*48/100; if(wh<300)wh=300; if(wh>460)wh=460; if(wh>H-40)wh=H-40;
    g_b64.win=wm_open("Base64", ww, wh, b64_draw, b64_event, &g_b64);
}

/* ==========================================================================
 * (4) CAESAR / ROT13 cipher.
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int shift; int b_hover, b_press;
                 char cache_in[80]; int cache_shift; char cache_out[80]; int cache_valid; } caesar_state;
static caesar_state g_caesar;

static char caesar_shift_ch(char c, int sh){
    if(c>='a'&&c<='z') return (char)('a'+((c-'a'+sh)%26));
    if(c>='A'&&c<='Z') return (char)('A'+((c-'A'+sh)%26));
    return c;
}

static int caesar_btns(int cw, int ch, wm_button *out){
    int S=ui_scale(), x=CV_PAD, y=CV_PAD; (void)ch;
    cv_btn_set(&out[0], 1, "-", x, y); x+=out[0].w+6*S;
    cv_btn_set(&out[1], 2, "+", x, y); x+=out[1].w+6*S;
    cv_btn_set(&out[2], 3, "ROT13", x, y); x+=out[2].w+6*S;
    int clx=cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[3], 4, "Clear", clx<x?x:clx, y);
    return 4;
}

static void caesar_draw(wm_window *w, int cx, int cy, int cw, int ch){
    caesar_state *s=(caesar_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[4]; int nb=caesar_btns(cw,ch,b);
    cv_btn_draw_row(b,nb,s->b_hover,s->b_press);
    int S=ui_scale();
    int y=cy+CV_PAD+wm_button_h()+8*S;
    char sh[40]; cv_copy(sh,"Shift = ",40); { int l=cv_len(sh); char t[8]; cv_u64toa((UINT64)s->shift,t); cv_copy(sh+l,t,40-l);}
    { int l=cv_len(sh); cv_copy(sh+l," (Left/Right arrows)",40-l); }
    draw_string_clip(cx+CV_PAD,y,cw-2*CV_PAD,sh,p.accent,p.bg,1,1);
    y+=16*S+6*S;
    y=cv_input_box(cx,y,cw,&p,"Plain text:",s->in.buf);
    y+=4*S;
    if(!s->cache_valid || s->cache_shift!=s->shift || !cv_eq(s->cache_in, s->in.buf)){
        int i=0; for(; s->in.buf[i] && i<79; i++) s->cache_out[i]=caesar_shift_ch(s->in.buf[i], s->shift); s->cache_out[i]=0;
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_shift = s->shift;
        s->cache_valid = 1;
    }
    draw_string_clip(cx+CV_PAD,y,cw-2*CV_PAD,"Cipher:",p.dim,p.bg,1,1);
    y+=16*S+2*S;
    cv_wrap(cx+CV_PAD,y,cw-2*CV_PAD,s->cache_out,&p);
}

static int caesar_event(wm_window *w, const wm_event *ev){
    caesar_state *s=(caesar_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_LEFT){ s->shift=(s->shift+25)%26; return 0; }
            if(ev->scancode==SCAN_RIGHT){ s->shift=(s->shift+1)%26; return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode; if(u>=0x20 && u<0x7f) cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[4]; int nb=caesar_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[4]; int nb=caesar_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[4]; int nb=caesar_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id==1)s->shift=(s->shift+25)%26; else if(id==2)s->shift=(s->shift+1)%26; else if(id==3)s->shift=13; else if(id==4)cv_edit_clear(&s->in); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_caesar_open(void){
    if(g_caesar.win) return;
    cv_edit_clear(&g_caesar.in); g_caesar.shift=3; g_caesar.b_hover=0; g_caesar.b_press=0; g_caesar.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*48/100; if(ww<400)ww=400; if(ww>600)ww=600; if(ww>W-40)ww=W-40;
    int wh=H*46/100; if(wh<300)wh=300; if(wh>440)wh=440; if(wh>H-40)wh=H-40;
    g_caesar.win=wm_open("Caesar / ROT13", ww, wh, caesar_draw, caesar_event, &g_caesar);
}

/* ==========================================================================
 * (5) RGB <-> HEX with a live swatch.
 * ========================================================================== */
typedef struct { wm_window *win; int r,g,b; int sel; int b_hover, b_press; } rgb_state;
static rgb_state g_rgb;

/* per-channel [-] and [+] buttons + a Reset. ids: ch*2+1 minus, ch*2+2 plus (ch 0..2); reset=99 */
static int rgb_btns(int cw, int ch, wm_button *out){
    (void)ch;
    int S=ui_scale(); int lh=16*S+12*S;
    int y0=CV_PAD + 16*S + 6*S;   /* below title line */
    int bx=cw-CV_PAD-wm_button_measure("+")-6*S-wm_button_measure("-");
    int n=0;
    for(int c=0;c<3;c++){
        int ry=y0+c*lh;
        cv_btn_set(&out[n], c*2+1, "-", bx, ry); n++;
        cv_btn_set(&out[n], c*2+2, "+", bx+wm_button_measure("-")+6*S, ry); n++;
    }
    cv_btn_set(&out[n], 99, "Reset", CV_PAD, y0+3*lh+4*S); n++;
    return n;
}

static void rgb_draw(wm_window *w, int cx, int cy, int cw, int ch){
    rgb_state *s=(rgb_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    int S=ui_scale(); int lh=16*S+12*S;
    UINT32 col=((UINT32)s->r<<16)|((UINT32)s->g<<8)|(UINT32)s->b;
    /* title with hex */
    char hx[10]; hx[0]='#'; { char t[8]; cv_u64tobase(col,16,t,6); cv_copy(hx+1,t,9); }
    draw_string_clip(cx+CV_PAD, cy+CV_PAD, cw-2*CV_PAD, hx, p.accent, p.bg, 1, 2);
    int y0=cy+CV_PAD+16*S+6*S;
    const char *nm[3]={"R","G","B"}; int val[3]={s->r,s->g,s->b};
    for(int c=0;c<3;c++){
        int ry=y0+c*lh;
        int selrow=(c==s->sel);
        if(selrow) fill_rect(cx+CV_PAD-4*S, ry-2*S, cw-2*CV_PAD+8*S, 16*S+4*S, p.selbg);
        char row[24]; int o=0; row[o++]=nm[c][0]; row[o++]=':'; row[o++]=' ';
        char t[8]; int tl=cv_u64toa((UINT64)val[c],t); for(int i=0;i<3-tl;i++)row[o++]=' '; for(int i=0;i<tl;i++)row[o++]=t[i];
        row[o++]=' '; row[o]=0;
        UINT32 chc = (c==0)?0x00FF4040u:(c==1)?0x0040FF40u:0x004060FFu;
        draw_string_clip(cx+CV_PAD, ry, 120*S, row, selrow?p.selfg:chc, selrow?p.selbg:p.bg, 1, 1);
        /* channel bar */
        int barx=cx+CV_PAD+70*S, barw=cw-CV_PAD-70*S-CV_PAD-2*(wm_button_measure("-")+6*S);
        if(barw>10*S){ fill_rect(barx,ry+3*S,barw,10*S,p.panel);
            fill_rect(barx,ry+3*S,barw*val[c]/255,10*S,chc); }
    }
    /* buttons */
    wm_button b[7]; int nb=rgb_btns(cw,ch,b);
    cv_btn_draw_row(b,nb,s->b_hover,s->b_press);
    /* swatch */
    int sy=y0+3*lh+4*S;
    int swx=cx+cw/2, sww=cw/2-CV_PAD, swh=cy+ch-sy-CV_PAD;
    if(sww>20*S && swh>16*S){
        fill_rect(swx,sy,sww,swh,col);
        draw_rect_outline(swx,sy,sww,swh,1,p.border);
    }
    draw_string_clip(cx+CV_PAD, cy+ch-16*S-CV_PAD, cw/2-CV_PAD, "Up/Down pick, Left/Right +-1", p.dim, p.bg, 1, 1);
}

static void rgb_adj(rgb_state *s, int ch, int d){
    int *pp = ch==0?&s->r : ch==1?&s->g : &s->b;
    int v=*pp+d; if(v<0)v=0; if(v>255)v=255; *pp=v;
}

static int rgb_event(wm_window *w, const wm_event *ev){
    rgb_state *s=(rgb_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_UP){ s->sel=(s->sel+2)%3; return 0; }
            if(ev->scancode==SCAN_DOWN){ s->sel=(s->sel+1)%3; return 0; }
            if(ev->scancode==SCAN_LEFT){ rgb_adj(s,s->sel,-1); return 0; }
            if(ev->scancode==SCAN_RIGHT){ rgb_adj(s,s->sel,+1); return 0; }
            if(ev->scancode==SCAN_PAGE_DOWN){ rgb_adj(s,s->sel,-16); return 0; }
            if(ev->scancode==SCAN_PAGE_UP){ rgb_adj(s,s->sel,+16); return 0; }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[7]; int nb=rgb_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[7]; int nb=rgb_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id){s->b_press=id; return 0;}
              /* click a channel row to select */
              int S=ui_scale(); int lh=16*S+12*S; int y0=CV_PAD+16*S+6*S;
              for(int c=0;c<3;c++){ if(ev->my>=y0+c*lh-2*S && ev->my<y0+c*lh+16*S){ s->sel=c; break; } }
              return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[7]; int nb=rgb_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id==99){s->r=s->g=s->b=0;} else { int c=(id-1)/2; int minus=((id-1)&1)==0; rgb_adj(s,c,minus?-1:+1); s->sel=c; } } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_rgb_open(void){
    if(g_rgb.win) return;
    g_rgb.r=61; g_rgb.g=182; g_rgb.b=61; g_rgb.sel=0; g_rgb.b_hover=0; g_rgb.b_press=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*48/100; if(ww<420)ww=420; if(ww>580)ww=580; if(ww>W-40)ww=W-40;
    int wh=H*44/100; if(wh<280)wh=280; if(wh>400)wh=400; if(wh>H-40)wh=H-40;
    g_rgb.win=wm_open("RGB <-> Hex", ww, wh, rgb_draw, rgb_event, &g_rgb);
}

/* ==========================================================================
 * (6) TEMPERATURE C / F / K (tenths precision, one decimal shown).
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int unit; int b_hover, b_press;
                 char cache_in[80]; int cache_unit; char cB[24], fB[24], kB[24]; int cache_valid; } temp_state;
static temp_state g_temp;
static const char *TEMP_LBL[3]={"C","F","K"};

static int temp_btns(int cw, int ch, wm_button *out){
    int S=ui_scale(), x=CV_PAD, y=CV_PAD; (void)ch;
    for(int i=0;i<3;i++){ cv_btn_set(&out[i], i+1, TEMP_LBL[i], x, y); x+=out[i].w+6*S; }
    int clx=cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[3], 4, "Clear", clx<x?x:clx, y);
    return 4;
}

static void temp_draw(wm_window *w, int cx, int cy, int cw, int ch){
    temp_state *s=(temp_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[4]; int nb=temp_btns(cw,ch,b);
    for(int i=0;i<nb;i++){ int active=(i<3&&i==s->unit); wm_button_draw(&b[i], b[i].id==s->b_hover, active||b[i].id==s->b_press); }
    int S=ui_scale();
    int y=cy+CV_PAD+wm_button_h()+8*S;
    char lbl[40]; cv_copy(lbl,"Value in ",40); { int l=cv_len(lbl); cv_copy(lbl+l,TEMP_LBL[s->unit],40-l);} { int l=cv_len(lbl); cv_copy(lbl+l," (integer, '-' ok):",40-l);}
    y=cv_input_box(cx,y,cw,&p,lbl,s->in.buf);
    y+=6*S;
    if(!s->cache_valid || s->cache_unit!=s->unit || !cv_eq(s->cache_in, s->in.buf)){
        INT64 val; int ok=cv_parse_int(s->in.buf,&val);
        cv_copy(s->cB,"-",24); cv_copy(s->fB,"-",24); cv_copy(s->kB,"-",24);
        if(ok){
            if(val>1000000) val=1000000;          /* keep val*10 far from INT64 overflow */
            if(val<-1000000) val=-1000000;
            INT64 c10;
            if(s->unit==0) c10=val*10;
            else if(s->unit==1) c10=(val*10-320)*5/9;
            else c10=val*10-2732;
            INT64 f10=c10*9/5+320, k10=c10+2732;
            cv_fixfmt(s->cB,c10,1); cv_fixfmt(s->fB,f10,1); cv_fixfmt(s->kB,k10,1);
        }
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_unit = s->unit;
        s->cache_valid = 1;
    }
    int lh=16*S+8*S;
    char t[32];
    cv_copy(t,s->cB,32);{int l=cv_len(t);cv_copy(t+l," C",32-l);} cv_row(cx,y,cw,&p,"Celsius:",t,1); y+=lh;
    cv_copy(t,s->fB,32);{int l=cv_len(t);cv_copy(t+l," F",32-l);} cv_row(cx,y,cw,&p,"Fahrenheit:",t,1); y+=lh;
    cv_copy(t,s->kB,32);{int l=cv_len(t);cv_copy(t+l," K",32-l);} cv_row(cx,y,cw,&p,"Kelvin:",t,1); y+=lh;
}

static int temp_event(wm_window *w, const wm_event *ev){
    temp_state *s=(temp_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_TAB){ s->unit=(s->unit+1)%3; return 0; }
            if(ev->scancode==SCAN_LEFT){ s->unit=(s->unit+2)%3; return 0; }
            if(ev->scancode==SCAN_RIGHT){ s->unit=(s->unit+1)%3; return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode; if((u>='0'&&u<='9')||u=='-') cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[4]; int nb=temp_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[4]; int nb=temp_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[4]; int nb=temp_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id>=1&&id<=3)s->unit=id-1; else if(id==4)cv_edit_clear(&s->in); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_temp_open(void){
    if(g_temp.win) return;
    cv_edit_clear(&g_temp.in); g_temp.unit=0; g_temp.b_hover=0; g_temp.b_press=0; g_temp.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*44/100; if(ww<380)ww=380; if(ww>520)ww=520; if(ww>W-40)ww=W-40;
    int wh=H*42/100; if(wh<280)wh=280; if(wh>380)wh=380; if(wh>H-40)wh=H-40;
    g_temp.win=wm_open("Temperature", ww, wh, temp_draw, temp_event, &g_temp);
}

/* ==========================================================================
 * (7) DATA SIZE B / KB / MB / GB / TB (all rows, 2-decimal fixed point).
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int unit; int b_hover, b_press;
                 char cache_in[80]; int cache_unit; char cache_full[5][40]; int cache_over; int cache_valid; } size_state;
static size_state g_size;
static const char *SIZE_LBL[5]={"B","KB","MB","GB","TB"};
static const UINT64 SIZE_MUL[5]={1ULL,1024ULL,1048576ULL,1073741824ULL,1099511627776ULL};

static int size_btns(int cw, int ch, wm_button *out){
    int S=ui_scale(), x=CV_PAD, y=CV_PAD; (void)ch;
    for(int i=0;i<5;i++){ cv_btn_set(&out[i], i+1, SIZE_LBL[i], x, y); x+=out[i].w+6*S; }
    int clx=cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[5], 6, "Clear", clx<x?x:clx, y);
    return 6;
}

static void size_draw(wm_window *w, int cx, int cy, int cw, int ch){
    size_state *s=(size_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[6]; int nb=size_btns(cw,ch,b);
    for(int i=0;i<nb;i++){ int active=(i<5&&i==s->unit); wm_button_draw(&b[i], b[i].id==s->b_hover, active||b[i].id==s->b_press); }
    int S=ui_scale();
    int y=cy+CV_PAD+wm_button_h()+8*S;
    char lbl[40]; cv_copy(lbl,"Amount in ",40);{int l=cv_len(lbl);cv_copy(lbl+l,SIZE_LBL[s->unit],40-l);}{int l=cv_len(lbl);cv_copy(lbl+l,":",40-l);}
    y=cv_input_box(cx,y,cw,&p,lbl,s->in.buf);
    y+=6*S;
    if(!s->cache_valid || s->cache_unit!=s->unit || !cv_eq(s->cache_in, s->in.buf)){
        INT64 val; int ok=cv_parse_int(s->in.buf,&val);
        UINT64 bytes=0; int over=0;
        if(ok && val>=0){
            if(__builtin_mul_overflow((UINT64)val, SIZE_MUL[s->unit], &bytes)) over=1;
        } else if(ok && val<0) ok=0;
        for(int i=0;i<5;i++){
            char vb[32];
            if(!ok||over) cv_copy(vb,"-",32);
            else if(i==0){ cv_u64toa(bytes,vb); }
            else {
                /* bytes*100/divisor -> 2 decimals, guard overflow via 128-bit-ish split */
                UINT64 div=SIZE_MUL[i];
                UINT64 whole=bytes/div, rem=bytes%div;
                UINT64 frac=(rem*100ULL)/div;
                char wb[24]; int wl=cv_u64toa(whole,wb); int o=0;
                for(int k=0;k<wl;k++) vb[o++]=wb[k];
                vb[o++]='.'; vb[o++]=(char)('0'+(frac/10)); vb[o++]=(char)('0'+(frac%10)); vb[o]=0;
            }
            char full[40]; cv_copy(full,vb,40);{int l=cv_len(full);full[l]=' ';cv_copy(full+l+1,SIZE_LBL[i],40-l-1);}
            cv_copy(s->cache_full[i], full, 40);
        }
        s->cache_over = over;
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_unit = s->unit;
        s->cache_valid = 1;
    }
    int lh=16*S+8*S;
    for(int i=0;i<5;i++){
        char lab[8]; int o=0; for(const char*q=SIZE_LBL[i];*q;)lab[o++]=*q++; lab[o++]=':'; lab[o]=0;
        cv_row(cx,y,cw,&p,lab,s->cache_full[i],1); y+=lh;
    }
    if(s->cache_over) draw_string_clip(cx+CV_PAD,y,cw-2*CV_PAD,"value too large",0x00FF5A5Au,p.bg,1,1);
}

static int size_event(wm_window *w, const wm_event *ev){
    size_state *s=(size_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_TAB || ev->scancode==SCAN_RIGHT){ s->unit=(s->unit+1)%5; return 0; }
            if(ev->scancode==SCAN_LEFT){ s->unit=(s->unit+4)%5; return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode; if(u>='0'&&u<='9') cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[6]; int nb=size_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[6]; int nb=size_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[6]; int nb=size_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id>=1&&id<=5)s->unit=id-1; else if(id==6)cv_edit_clear(&s->in); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_datasize_open(void){
    if(g_size.win) return;
    cv_edit_clear(&g_size.in); g_size.unit=2; g_size.b_hover=0; g_size.b_press=0; g_size.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*46/100; if(ww<420)ww=420; if(ww>560)ww=560; if(ww>W-40)ww=W-40;
    int wh=H*48/100; if(wh<320)wh=320; if(wh>440)wh=440; if(wh>H-40)wh=H-40;
    g_size.win=wm_open("Data Size", ww, wh, size_draw, size_event, &g_size);
}

/* ==========================================================================
 * (8) ROMAN NUMERALS <-> integer (auto-detect direction).
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int b_hover, b_press;
                 char cache_in[80]; char cache_lbl[16]; char cache_res[64]; int cache_valid; } roman_state;
static roman_state g_roman;

static void roman_encode(int n, char *out){
    static const int   V[13]={1000,900,500,400,100,90,50,40,10,9,5,4,1};
    static const char *R[13]={"M","CM","D","CD","C","XC","L","XL","X","IX","V","IV","I"};
    int o=0;
    for(int i=0;i<13;i++){ while(n>=V[i]){ for(const char*q=R[i];*q;)out[o++]=*q++; n-=V[i]; } }
    out[o]=0;
}
static int roman_val(char c){
    switch(cv_upper(c)){ case 'I':return 1; case 'V':return 5; case 'X':return 10;
        case 'L':return 50; case 'C':return 100; case 'D':return 500; case 'M':return 1000;
        default:return 0; }
}
static int roman_decode(const char *s, int *out){
    int total=0, prev=0, any=0;
    /* right to left */
    int n=cv_len(s);
    for(int i=n-1;i>=0;i--){
        if(s[i]==' ') continue;
        int v=roman_val(s[i]); if(!v) return 0;
        any=1;
        if(v<prev) total-=v; else { total+=v; prev=v; }
    }
    if(!any || total<1 || total>3999) return 0;
    *out=total; return 1;
}

static int roman_btns(int cw, int ch, wm_button *out){
    (void)ch; int clx=cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[0], 1, "Clear", clx<CV_PAD?CV_PAD:clx, CV_PAD);
    return 1;
}

static void roman_draw(wm_window *w, int cx, int cy, int cw, int ch){
    roman_state *s=(roman_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[1]; int nb=roman_btns(cw,ch,b);
    cv_btn_draw_row(b,nb,s->b_hover,s->b_press);
    int S=ui_scale();
    draw_string_clip(cx+CV_PAD,cy+CV_PAD,cw-2*CV_PAD-wm_button_measure("Clear")-6*S,
                     "Type an integer (1-3999) or Roman numerals.", p.dim, p.bg,1,1);
    int y=cy+CV_PAD+16*S+8*S;
    y=cv_input_box(cx,y,cw,&p,"Input:",s->in.buf);
    y+=6*S;
    if(!s->cache_valid || !cv_eq(s->cache_in, s->in.buf)){
        /* detect direction: first non-space char */
        const char *q=s->in.buf; while(*q==' ') q++;
        char resLbl[16]; char res[64]="-";
        if(*q){
            if(*q>='0'&&*q<='9'){
                INT64 v; cv_copy(resLbl,"Roman:",16);
                if(cv_parse_int(s->in.buf,&v) && v>=1 && v<=3999) roman_encode((int)v,res);
                else cv_copy(res,"(1..3999)",64);
            } else {
                int v; cv_copy(resLbl,"Integer:",16);
                if(roman_decode(s->in.buf,&v)){ char t[16]; cv_u64toa((UINT64)v,t); cv_copy(res,t,64); }
                else cv_copy(res,"(invalid roman)",64);
            }
        } else cv_copy(resLbl,"Result:",16);
        cv_copy(s->cache_lbl, resLbl, 16);
        cv_copy(s->cache_res, res, 64);
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_valid = 1;
    }
    cv_row(cx,y,cw,&p,s->cache_lbl,s->cache_res,2);
}

static int roman_event(wm_window *w, const wm_event *ev){
    roman_state *s=(roman_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode;
              if((u>='0'&&u<='9')||u=='I'||u=='V'||u=='X'||u=='L'||u=='C'||u=='D'||u=='M'
                 ||u=='i'||u=='v'||u=='x'||u=='l'||u=='c'||u=='d'||u=='m')
                  cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[1]; int nb=roman_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[1]; int nb=roman_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[1]; int nb=roman_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr && id==1) cv_edit_clear(&s->in);
              return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_roman_open(void){
    if(g_roman.win) return;
    cv_edit_clear(&g_roman.in); g_roman.b_hover=0; g_roman.b_press=0; g_roman.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*46/100; if(ww<400)ww=400; if(ww>560)ww=560; if(ww>W-40)ww=W-40;
    int wh=H*36/100; if(wh<240)wh=240; if(wh>320)wh=320; if(wh>H-40)wh=H-40;
    g_roman.win=wm_open("Roman Numerals", ww, wh, roman_draw, roman_event, &g_roman);
}

/* ==========================================================================
 * (9) ANGLE deg / rad(milli) / grad - integer scaled.
 * ========================================================================== */
typedef struct { wm_window *win; cv_edit in; int unit; int b_hover, b_press;
                 char cache_in[80]; int cache_unit; char cache_d[32], cache_r[32], cache_g[32]; int cache_valid; } angle_state;
static angle_state g_angle;
static const char *ANG_LBL[3]={"deg","mrad","grad"};

static int angle_btns(int cw, int ch, wm_button *out){
    int S=ui_scale(), x=CV_PAD, y=CV_PAD; (void)ch;
    for(int i=0;i<3;i++){ cv_btn_set(&out[i], i+1, ANG_LBL[i], x, y); x+=out[i].w+6*S; }
    int clx=cw-CV_PAD-wm_button_measure("Clear");
    cv_btn_set(&out[3], 4, "Clear", clx<x?x:clx, y);
    return 4;
}

static void angle_draw(wm_window *w, int cx, int cy, int cw, int ch){
    angle_state *s=(angle_state*)wm_user(w); if(!s) return;
    cv_pal p=cv_theme();
    fill_rect(cx,cy,cw,ch,p.bg);
    wm_button b[4]; int nb=angle_btns(cw,ch,b);
    for(int i=0;i<nb;i++){ int active=(i<3&&i==s->unit); wm_button_draw(&b[i], b[i].id==s->b_hover, active||b[i].id==s->b_press); }
    int S=ui_scale();
    int y=cy+CV_PAD+wm_button_h()+8*S;
    char lbl[40]; cv_copy(lbl,"Value in ",40);{int l=cv_len(lbl);cv_copy(lbl+l,ANG_LBL[s->unit],40-l);}{int l=cv_len(lbl);cv_copy(lbl+l," (integer):",40-l);}
    y=cv_input_box(cx,y,cw,&p,lbl,s->in.buf);
    y+=6*S;
    if(!s->cache_valid || s->cache_unit!=s->unit || !cv_eq(s->cache_in, s->in.buf)){
        INT64 val; int ok=cv_parse_int(s->in.buf,&val);
        cv_copy(s->cache_d,"-",32); cv_copy(s->cache_r,"-",32); cv_copy(s->cache_g,"-",32);
        if(ok){
            if(val>100000000) val=100000000;      /* keep the *1000 / *174533 chain safe */
            if(val<-100000000) val=-100000000;
            /* internal: milli-degrees */
            INT64 mdeg;
            if(s->unit==0) mdeg=val*1000;
            else if(s->unit==1) mdeg=val*5729578/100000;    /* 1 mrad = 57.29578 mdeg */
            else mdeg=val*900;                              /* 1 grad = 0.9 deg       */
            INT64 mgrad=mdeg*10/9;                          /* deg*10/9               */
            INT64 mmrad=mdeg*17453293/1000000;              /* deg*17.453293, x1000   */
            cv_fixfmt(s->cache_d,mdeg,3); cv_fixfmt(s->cache_g,mgrad,3); cv_fixfmt(s->cache_r,mmrad,3);
        }
        cv_copy(s->cache_in, s->in.buf, sizeof(s->cache_in));
        s->cache_unit = s->unit;
        s->cache_valid = 1;
    }
    int lh=16*S+8*S; char t[40];
    cv_copy(t,s->cache_d,40);{int l=cv_len(t);cv_copy(t+l," deg",40-l);} cv_row(cx,y,cw,&p,"Degrees:",t,1); y+=lh;
    cv_copy(t,s->cache_r,40);{int l=cv_len(t);cv_copy(t+l," mrad",40-l);} cv_row(cx,y,cw,&p,"Milli-rad:",t,1); y+=lh;
    cv_copy(t,s->cache_g,40);{int l=cv_len(t);cv_copy(t+l," grad",40-l);} cv_row(cx,y,cw,&p,"Gradians:",t,1); y+=lh;
}

static int angle_event(wm_window *w, const wm_event *ev){
    angle_state *s=(angle_state*)wm_user(w); if(!s) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_TAB || ev->scancode==SCAN_RIGHT){ s->unit=(s->unit+1)%3; return 0; }
            if(ev->scancode==SCAN_LEFT){ s->unit=(s->unit+2)%3; return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ cv_edit_bksp(&s->in); return 0; }
            { CHAR16 u=ev->unicode; if((u>='0'&&u<='9')||u=='-') cv_edit_putc(&s->in,(char)u); }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button b[4]; int nb=angle_btns(cw,ch,b); s->b_hover=cv_btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button b[4]; int nb=angle_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button b[4]; int nb=angle_btns(cw,ch,b); int id=cv_btn_hit(b,nb,ev->mx,ev->my),pr=s->b_press; s->b_press=0;
              if(id==pr){ if(id>=1&&id<=3)s->unit=id-1; else if(id==4)cv_edit_clear(&s->in); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}

void tool_convert_angle_open(void){
    if(g_angle.win) return;
    cv_edit_clear(&g_angle.in); g_angle.unit=0; g_angle.b_hover=0; g_angle.b_press=0; g_angle.cache_valid=0;
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*44/100; if(ww<400)ww=400; if(ww>540)ww=540; if(ww>W-40)ww=W-40;
    int wh=H*42/100; if(wh<280)wh=280; if(wh>380)wh=380; if(wh>H-40)wh=H-40;
    g_angle.win=wm_open("Angle", ww, wh, angle_draw, angle_event, &g_angle);
}

/* ==========================================================================
 * Category registry.
 * ========================================================================== */
const struct forebo_tool cat_convert_tools[] = {
    { "Base Converter", "Live dec / hex / bin / oct conversion",        "gear", tool_convert_base_open     },
    { "ASCII Table",    "Printable glyphs + dec/hex codes (scroll)",     "text", tool_convert_ascii_open    },
    { "Base64",         "Encode or decode a typed string",              "text", tool_convert_base64_open   },
    { "Caesar / ROT13", "Shift cipher with adjustable key",             "text", tool_convert_caesar_open   },
    { "RGB <-> Hex",    "Colour channels with a live swatch",           "gear", tool_convert_rgb_open      },
    { "Temperature",    "Celsius / Fahrenheit / Kelvin",                "gear", tool_convert_temp_open     },
    { "Data Size",      "Bytes / KB / MB / GB / TB (all rows)",         "gear", tool_convert_datasize_open },
    { "Roman Numerals", "Integer <-> Roman (auto direction)",           "text", tool_convert_roman_open    },
    { "Angle",          "Degrees / milli-radians / gradians",           "gear", tool_convert_angle_open    },
};
const int cat_convert_count = (int)(sizeof(cat_convert_tools)/sizeof(cat_convert_tools[0]));

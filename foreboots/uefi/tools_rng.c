/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_rng.c - "Random & Security" tools (KEY = rng).
 * =============================================================================
 * Nine template-B wm windows: password generator, coin flip, magic 8-ball,
 * CRC32 (string/file), FNV-1a, UUIDv4 generator, RPG dice, dice statistics and
 * a number-guesser game. Entropy: RDRAND when CPUID.01H:ECX[30]=1, else a
 * TSC-seeded, per-call-TSC-mixed xorshift64. Integer / fixed only. No libc.
 *
 * Each tool keeps its own static state (reached via wm_user) with fixed buffers.
 * Draw callbacks only render + clip; actions run from the event callback.
 * ========================================================================== */
#include "tools_rng.h"
#include "efi.h"
#include "wm.h"
#include "ui.h"
#include "input.h"
#include "config.h"                    /* esp_ascii_to_char16 (file CRC path)   */
#include "../include/forebo_theme.h"

/* ==========================================================================
 * Captured firmware services (cat_rng_init).
 * ========================================================================== */
static EFI_SYSTEM_TABLE     *gST;
static EFI_BOOT_SERVICES    *gBS;
static EFI_RUNTIME_SERVICES *gRT;

static EFI_GUID gSfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID gFinfoGuid = EFI_FILE_INFO_ID;

void cat_rng_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices : 0;
    gRT = st ? st->RuntimeServices : 0;
    (void)gRT; (void)gFinfoGuid;
}

/* ==========================================================================
 * Freestanding helpers (no libc).
 * ========================================================================== */
static int slen(const char *s){ int n=0; while(s&&s[n])n++; return n; }
static void scopy(char *d, const char *s, int cap)
{ int i=0; if(cap<=0)return; for(;s&&s[i]&&i+1<cap;i++)d[i]=s[i]; d[i]=0; }

/* unsigned decimal -> ascii, returns length. */
static int u2dec(UINT64 v, char *o)
{
    char t[24]; int n=0;
    if(v==0){ o[0]='0'; o[1]=0; return 1; }
    while(v){ t[n++]=(char)('0'+(int)(v%10)); v/=10; }
    for(int i=0;i<n;i++) o[i]=t[n-1-i];
    o[n]=0; return n;
}
/* signed decimal -> ascii. */
static int i2dec(long long v, char *o)
{
    if(v<0){ o[0]='-'; return 1+u2dec((UINT64)(-v), o+1); }
    return u2dec((UINT64)v, o);
}
/* lower-hex, fixed number of digits. */
static void hexn(UINT64 v, int digits, char *o)
{
    static const char hx[]="0123456789abcdef";
    for(int i=0;i<digits;i++) o[i]=hx[(v>>((digits-1-i)*4))&0xF];
    o[digits]=0;
}
static int is_digit(CHAR16 u){ return u>='0'&&u<='9'; }

/* ==========================================================================
 * Entropy: RDRAND if available, else TSC-seeded xorshift64 mixed with TSC.
 * ========================================================================== */
static UINT64 rdtsc64(void)
{
    UINT32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi<<32) | lo;
}
static int cpuid_has_rdrand(void)
{
    UINT32 a, b, c, d;
    __asm__ __volatile__("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(1U), "c"(0U));
    (void)a; (void)b; (void)d;
    return (int)((c>>30)&1U);
}
static int rdrand64(UINT64 *out)
{
    unsigned char ok = 0; UINT64 v = 0;
    for(int i=0;i<12;i++){
        __asm__ __volatile__("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if(ok){ *out=v; return 1; }
    }
    return 0;
}

static int    g_have_rdrand = -1;   /* lazy CPUID cache */
static UINT64 g_xstate = 0;

/* 1 if hardware RNG is in use, 0 if the software fallback is active. */
static int rng_hw(void)
{
    if(g_have_rdrand<0) g_have_rdrand = cpuid_has_rdrand();
    return g_have_rdrand;
}
static UINT64 rng_u64(void)
{
    UINT64 v;
    if(rng_hw() && rdrand64(&v)) return v;
    if(g_xstate==0) g_xstate = rdtsc64() ^ 0x9E3779B97F4A7C15ULL;
    UINT64 x = g_xstate;
    x ^= x<<13; x ^= x>>7; x ^= x<<17;
    g_xstate = x;
    return x ^ rdtsc64();            /* fold in fresh timing jitter per call */
}
/* Uniform-ish in [0,n): modulo bias is irrelevant for a boot-menu toy. */
static int rng_range(int n)
{
    if(n<=1) return 0;
    return (int)(rng_u64() % (UINT64)n);
}

/* ==========================================================================
 * Small shared widgets.
 * ========================================================================== */
typedef struct { int x, y, w, h; } rrect;
static int rhit(rrect r, int mx, int my)
{ return mx>=r.x && mx<r.x+r.w && my>=r.y && my<r.y+r.h; }

/* One text-input edit step. Returns 1 when Enter was pressed. */
static int tin_edit(char *buf, int *len, int cap, const wm_event *ev, int digits_only)
{
    if(ev->unicode==CHAR_CR) return 1;
    if(ev->unicode==CHAR_BACKSPACE){ if(*len>0){ (*len)--; buf[*len]=0; } return 0; }
    CHAR16 u = ev->unicode;
    if(digits_only){ if(!is_digit(u)) return 0; }
    else if(u<0x20 || u>=0x7f) return 0;
    if(*len < cap-1){ buf[*len]=(char)u; (*len)++; buf[*len]=0; }
    return 0;
}

/* Draw a text box (screen coords) with a blinking-free trailing cursor. */
static void inbox(int sx, int sy, int w, const char *buf, int focus)
{
    int uis = ui_scale(); if(uis<1) uis=1;
    int h = 16*uis + 10;
    fill_rect(sx, sy, w, h, FOREB_BG);
    draw_rect_outline(sx, sy, w, h, 1, focus?FOREB_TITLE:FOREB_BORDER);
    char tmp[192]; int i=0;
    for(; buf[i] && i<190; i++) tmp[i]=buf[i];
    if(focus && i<190) tmp[i++]='_';
    tmp[i]=0;
    draw_string_clip(sx+6, sy+5, w-12, tmp, FOREB_WHITE, FOREB_BG, 1, 1);
}
/* Natural input-box height for the current scale. */
static int inbox_h(void){ int uis=ui_scale(); if(uis<1)uis=1; return 16*uis+10; }

/* Draw a labelled checkbox (screen coords). */
static void cbox(int sx, int sy, int checked, const char *label)
{
    int uis = ui_scale(); if(uis<1) uis=1;
    int box = 12*uis;
    fill_rect(sx, sy, box, box, checked?FOREB_TITLE:FOREB_BORDER);
    draw_rect_outline(sx, sy, box, box, 1, FOREB_DIM);
    if(checked) draw_string(sx+2*uis, sy-2*uis, "x", FOREB_BG, FOREB_TITLE, 1, uis);
    draw_string(sx+box+8, sy+(box-16)/2, label, FOREB_TEXT, FOREB_PANEL, 1, 1);
}

static void heading(int cx, int cy, int cw, const char *s)
{ draw_string_clip(cx+14, cy+12, cw-28, s, FOREB_TITLE, FOREB_PANEL, 1, 2); }
static int heading_h(void){ int uis=ui_scale(); if(uis<1)uis=1; return 12 + 16*2*uis + 12; }

static void footer(int cx, int cy, int cw, int ch, const char *s)
{ draw_string_clip(cx+14, cy+ch-20, cw-28, s, FOREB_DIM, FOREB_PANEL, 1, 1); }

static void mkbtn(wm_button *b, int id, int x, int y, const char *label)
{
    b->x=x; b->y=y; b->w=wm_button_measure(label); b->h=wm_button_h();
    b->id=id; b->enabled=1; scopy(b->label, label, (int)sizeof(b->label));
}
/* Which button id (if any) is under (mx,my). */
static int btn_hit(const wm_button *b, int n, int mx, int my)
{ for(int i=0;i<n;i++) if(wm_button_hit(&b[i],mx,my)) return b[i].id; return 0; }
static void btns_draw(const wm_button *b, int n, int hover, int press)
{ for(int i=0;i<n;i++) wm_button_draw(&b[i], hover==b[i].id, press==b[i].id); }

/* ---- Button-bar cache -------------------------------------------------------
 * Every tool used to rebuild its whole button bar (a wm_button_measure() text
 * measure per label) on every MOUSE_MOVE and every draw. The layout depends only
 * on client size and ui_scale(), so cache it per window and rebuild solely when
 * one of those changes - keeps the hot mouse/draw path from re-measuring text.
 * Max bar is 4 buttons (dice statistics). */
typedef int (*btn_build_fn)(int cw, int ch, wm_button *b);
typedef struct { wm_button b[4]; int n; int cw, ch, scale; int valid; } btncache;

static int btns_get(btncache *bc, btn_build_fn build, int cw, int ch, wm_button **out)
{
    int sc = ui_scale();
    if(!bc->valid || bc->cw!=cw || bc->ch!=ch || bc->scale!=sc){
        bc->n = build(cw, ch, bc->b);
        bc->cw=cw; bc->ch=ch; bc->scale=sc; bc->valid=1;
    }
    *out = bc->b;
    return bc->n;
}

/* Standard centered open helper. */
static wm_window *rng_open(const char *title, int wpct, int hpct, int minw, int minh,
                           wm_draw_cb d, wm_event_cb e, void *u)
{
    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*wpct/100; if(ww<minw)ww=minw; if(ww>W-40)ww=W-40;
    int wh=H*hpct/100; if(wh<minh)wh=minh; if(wh>H-40)wh=H-40;
    return wm_open(title, ww, wh, d, e, u);
}

/* ==========================================================================
 * 1) PASSWORD GENERATOR
 * ========================================================================== */
static const char PW_LOWER[] = "abcdefghijklmnopqrstuvwxyz";
static const char PW_UPPER[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char PW_DIGIT[] = "0123456789";
static const char PW_SYM[]   = "!@#$%^&*()-_=+[]{};:,.?/";

typedef struct {
    wm_window *win;
    int  len;
    int  use[4];              /* lower, upper, digit, symbol */
    char out[80];
    int  have;
    int  b_hover, b_press;
    btncache bc;
} pwstate;
static pwstate g_pw;

/* Buttons: [-] [+] [Generate]. Filled in client coords. */
static int pw_buttons(int cw, int ch, wm_button *b)
{
    (void)cw;
    int y = ch - wm_button_h() - 12;
    mkbtn(&b[0], 1, 14, y, " - ");
    mkbtn(&b[1], 2, 14+b[0].w+8, y, " + ");
    mkbtn(&b[2], 3, 14+b[0].w+8+b[1].w+16, y, "Generate");
    return 3;
}
/* Checkbox row rects (client coords) for the 4 toggles. */
static void pw_toggle_rects(int cx0, int y0, rrect *r)
{
    for(int i=0;i<4;i++){ r[i].x=cx0; r[i].y=y0+i*24; r[i].w=200; r[i].h=20; }
}
static void pw_generate(pwstate *s)
{
    char pool[80]; int n=0;
    const char *sets[4]={PW_LOWER,PW_UPPER,PW_DIGIT,PW_SYM};
    for(int k=0;k<4;k++) if(s->use[k])
        for(const char *p=sets[k]; *p && n<(int)sizeof(pool); p++) pool[n++]=*p;
    if(n==0){ s->out[0]=0; s->have=0; return; }
    int L=s->len; if(L<4)L=4; if(L>(int)sizeof(s->out)-1)L=(int)sizeof(s->out)-1;
    for(int i=0;i<L;i++) s->out[i]=pool[rng_range(n)];
    s->out[L]=0; s->have=1;
}
static void pw_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "Password Generator");
    int y = cy + heading_h();

    /* generated password */
    fill_rect(cx+12, y, cw-24, inbox_h(), FOREB_BG);
    draw_rect_outline(cx+12, y, cw-24, inbox_h(), 1, FOREB_BORDER);
    draw_string_clip(cx+18, y+5, cw-36,
        g_pw.have ? g_pw.out : "(press Generate)",
        g_pw.have?FOREB_WHITE:FOREB_DIM, FOREB_BG, 1, 1);
    y += inbox_h() + 10;

    char ln[64]; int p=0;
    const char *lab="Length: "; while(*lab)ln[p++]=*lab++;
    p += u2dec((UINT64)g_pw.len, ln+p);
    draw_string_clip(cx+14, y, cw-28, ln, FOREB_TEXT, FOREB_PANEL, 1, 1);
    y += 16*(ui_scale()<1?1:ui_scale()) + 8;

    rrect r[4]; pw_toggle_rects(cx+14, y, r);
    static const char *tl[4]={"Lowercase  a-z","Uppercase  A-Z",
                              "Digits     0-9","Symbols  !@#$%"};
    for(int i=0;i<4;i++) cbox(r[i].x, r[i].y, g_pw.use[i], tl[i]);

    const char *src = rng_hw() ? "Entropy: RDRAND (hardware)"
                               : "Entropy: TSC xorshift (software)";
    draw_string_clip(cx+14, y+4*24+6, cw-28, src, FOREB_DIM, FOREB_PANEL, 1, 1);

    wm_button *b; int nb=btns_get(&g_pw.bc, pw_buttons, cw, ch, &b);
    btns_draw(b, nb, g_pw.b_hover, g_pw.b_press);
    footer(cx, cy, cw, ch, "l/u/d/s toggle  Left/Right len  Enter generate  Esc close");
}
static void pw_do(pwstate *s, int id)
{
    if(id==1){ if(s->len>4)s->len--; }
    else if(id==2){ if(s->len<64)s->len++; }
    else if(id==3){ pw_generate(s); }
}
static int pw_event(wm_window *w, const wm_event *ev)
{
    pwstate *s=&g_pw;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_LEFT) pw_do(s,1);
            else if(ev->scancode==SCAN_RIGHT) pw_do(s,2);
            else if(ev->unicode==CHAR_CR || ev->unicode==' ' ||
                    ev->unicode=='g' || ev->unicode=='G') pw_generate(s);
            else if(ev->unicode=='l'||ev->unicode=='L') s->use[0]^=1;
            else if(ev->unicode=='u'||ev->unicode=='U') s->use[1]^=1;
            else if(ev->unicode=='d'||ev->unicode=='D') s->use[2]^=1;
            else if(ev->unicode=='s'||ev->unicode=='S') s->use[3]^=1;
            return 0;
        case WM_EV_MOUSE_MOVE:{
            wm_button *b; int nb=btns_get(&s->bc,pw_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{
            wm_button *b; int nb=btns_get(&s->bc,pw_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my);
            if(id){ s->b_press=id; return 0; }
            rrect r[4]; pw_toggle_rects(14, heading_h()+inbox_h()+10+16*(ui_scale()<1?1:ui_scale())+8, r);
            for(int i=0;i<4;i++) if(rhit(r[i],ev->mx,ev->my)){ s->use[i]^=1; return 0; }
            return 0; }
        case WM_EV_MOUSE_UP:{
            if(!s->b_press) return 0;
            wm_button *b; int nb=btns_get(&s->bc,pw_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press;
            s->b_press=0; if(id==pr) pw_do(s,pr);
            return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_passwd_open(void)
{
    if(g_pw.win) return;
    for(unsigned i=0;i<sizeof(g_pw);i++) ((UINT8*)&g_pw)[i]=0;
    g_pw.len=16; g_pw.use[0]=g_pw.use[1]=g_pw.use[2]=1; g_pw.use[3]=0;
    pw_generate(&g_pw);
    g_pw.win=rng_open("Password Generator", 46, 60, 380, 360, pw_draw, pw_event, &g_pw);
}

/* ==========================================================================
 * 2) COIN FLIP
 * ========================================================================== */
typedef struct { wm_window *win; int last; int heads, tails; int b_hover,b_press; btncache bc; } coinstate;
static coinstate g_coin;

static int coin_buttons(int cw, int ch, wm_button *b)
{
    (void)cw;
    int y = ch - wm_button_h() - 12;
    mkbtn(&b[0], 1, 14, y, "Flip");
    mkbtn(&b[1], 2, 14+b[0].w+10, y, "Reset");
    return 2;
}
static void coin_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "Coin Flip");
    int mid = cx + cw/2;
    const char *face = (g_coin.last<0) ? "?" : (g_coin.last ? "HEADS" : "TAILS");
    UINT32 fc = (g_coin.last<0)?FOREB_DIM:(g_coin.last?FOREB_TIMER:FOREB_TITLE);
    /* coin disc */
    int cyc = cy + heading_h() + 60;
    fill_rect(mid-46, cyc-46, 92, 92, FOREB_BORDER);
    draw_rect_outline(mid-46, cyc-46, 92, 92, 2, fc);
    draw_string_center(mid, cyc-8, (g_coin.last<0)?"?":(g_coin.last?"H":"T"), fc, FOREB_BORDER, 1, 3);
    draw_string_center(mid, cyc+58, face, fc, FOREB_PANEL, 1, 2);

    char ln[64]; int p=0;
    const char *a="Heads: "; while(*a)ln[p++]=*a++; p+=u2dec((UINT64)g_coin.heads,ln+p);
    const char *b=" Tails: "; while(*b)ln[p++]=*b++; p+=u2dec((UINT64)g_coin.tails,ln+p);
    ln[p]=0;
    draw_string_center(mid, cyc+94, ln, FOREB_TEXT, FOREB_PANEL, 1, 1);
    int total=g_coin.heads+g_coin.tails;
    char t2[64]; int q=0; const char *tp="Flips: "; while(*tp)t2[q++]=*tp++;
    q+=u2dec((UINT64)total,t2+q); t2[q]=0;
    draw_string_center(mid, cyc+114, t2, FOREB_DIM, FOREB_PANEL, 1, 1);

    wm_button *bt; int nb=btns_get(&g_coin.bc, coin_buttons, cw, ch, &bt);
    btns_draw(bt, nb, g_coin.b_hover, g_coin.b_press);
    footer(cx, cy, cw, ch, "Space/Enter flip  r reset  Esc close");
}
static void coin_flip(coinstate *s){ s->last=rng_range(2); if(s->last)s->heads++; else s->tails++; }
static int coin_event(wm_window *w, const wm_event *ev)
{
    coinstate *s=&g_coin; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==' '||ev->unicode==CHAR_CR||ev->unicode=='f'||ev->unicode=='F') coin_flip(s);
            else if(ev->unicode=='r'||ev->unicode=='R'){ s->heads=s->tails=0; s->last=-1; }
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,coin_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,coin_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; else coin_flip(s); return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,coin_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr){ if(pr==1)coin_flip(s); else { s->heads=s->tails=0; s->last=-1; } } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_coin_open(void)
{
    if(g_coin.win) return;
    g_coin.last=-1; g_coin.heads=g_coin.tails=0; g_coin.b_hover=g_coin.b_press=0;
    g_coin.win=rng_open("Coin Flip", 38, 52, 320, 340, coin_draw, coin_event, &g_coin);
}

/* ==========================================================================
 * 3) MAGIC 8-BALL
 * ========================================================================== */
static const char *M8[] = {
    "It is certain.","Without a doubt.","Yes, definitely.","You may rely on it.",
    "As I see it, yes.","Most likely.","Outlook good.","Signs point to yes.",
    "Reply hazy, try again.","Ask again later.","Cannot predict now.",
    "Concentrate and ask again.","Don't count on it.","My reply is no.",
    "My sources say no.","Outlook not so good.","Very doubtful."
};
#define M8_N ((int)(sizeof(M8)/sizeof(M8[0])))

typedef struct { wm_window *win; char q[64]; int qlen; int ans; int b_hover,b_press; btncache bc; } m8state;
static m8state g_m8;

static int m8_buttons(int cw, int ch, wm_button *b)
{ (void)cw; int y=ch-wm_button_h()-12; mkbtn(&b[0],1,14,y,"Shake"); return 1; }
static void m8_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "Magic 8-Ball");
    int y=cy+heading_h();
    draw_string_clip(cx+14, y, cw-28, "Ask a yes/no question:", FOREB_TEXT, FOREB_PANEL, 1, 1);
    y += inbox_h();
    inbox(cx+14, y, cw-28, g_m8.q, 1);
    y += inbox_h() + 24;

    int mid=cx+cw/2;
    fill_rect(mid-52, y, 104, 104, FOREB_SHADOW);
    draw_rect_outline(mid-52, y, 104, 104, 2, FOREB_BORDER);
    draw_string_center(mid, y+38, "8", FOREB_WHITE, FOREB_SHADOW, 1, 4);
    y += 116;

    if(g_m8.ans>=0 && g_m8.ans<M8_N)
        draw_string_clip(cx+14, y, cw-28, M8[g_m8.ans], FOREB_TIMER, FOREB_PANEL, 1, 1);
    else
        draw_string_clip(cx+14, y, cw-28, "(shake to reveal)", FOREB_DIM, FOREB_PANEL, 1, 1);

    wm_button *b; int nb=btns_get(&g_m8.bc, m8_buttons, cw, ch, &b);
    btns_draw(b, nb, g_m8.b_hover, g_m8.b_press);
    footer(cx, cy, cw, ch, "Type a question  Enter shake  Esc close");
}
static void m8_shake(m8state *s){ s->ans=rng_range(M8_N); }
static int m8_event(wm_window *w, const wm_event *ev)
{
    m8state *s=&g_m8; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(tin_edit(s->q,&s->qlen,(int)sizeof(s->q),ev,0)) m8_shake(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,m8_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,m8_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,m8_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr) m8_shake(s); return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_magic8_open(void)
{
    if(g_m8.win) return;
    g_m8.q[0]=0; g_m8.qlen=0; g_m8.ans=-1; g_m8.b_hover=g_m8.b_press=0;
    g_m8.win=rng_open("Magic 8-Ball", 42, 60, 340, 380, m8_draw, m8_event, &g_m8);
}

/* ==========================================================================
 * 4) CRC32 (string or file)
 * ========================================================================== */
/* Lazily-built 256-entry CRC32 table (static storage, no heap). */
static UINT32 g_crc32_tbl[256];
static int    g_crc32_tbl_ready=0;
static void crc32_build_tbl(void)
{
    for(UINT32 i=0;i<256;i++){
        UINT32 c=i;
        for(int k=0;k<8;k++) c = (c>>1) ^ (0xEDB88320u & (0u - (c&1u)));
        g_crc32_tbl[i]=c;
    }
    g_crc32_tbl_ready=1;
}
static UINT32 crc32_upd(UINT32 c, const UINT8 *d, UINTN n)
{
    if(!g_crc32_tbl_ready) crc32_build_tbl();
    for(UINTN i=0;i<n;i++) c = g_crc32_tbl[(c ^ d[i]) & 0xFFu] ^ (c >> 8);
    return c;
}
static UINT32 crc32_buf(const UINT8 *d, UINTN n)
{ return crc32_upd(0xFFFFFFFFu, d, n) ^ 0xFFFFFFFFu; }

typedef struct {
    wm_window *win;
    int  mode;                 /* 0 = string, 1 = file */
    char in[128];
    int  len;
    int  done;
    UINT32 crc;
    UINT64 size;
    const char *err;
    int  b_hover, b_press;
    btncache bc;
} crcstate;
static crcstate g_crc;

/* CRC32 a file from the first Simple File System (typically the ESP). */
static int crc_file(const char *path, UINT32 *out_crc, UINT64 *out_size, const char **err)
{
    *err=0;
    if(!gBS){ *err="no BootServices"; return 0; }
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs=0;
    if(EFI_ERROR(gBS->LocateProtocol(&gSfsGuid,0,(void**)&sfs)) || !sfs){ *err="no filesystem"; return 0; }
    EFI_FILE_PROTOCOL *root=0;
    if(EFI_ERROR(sfs->OpenVolume(sfs,&root)) || !root){ *err="open volume failed"; return 0; }
    CHAR16 wp[260]; esp_ascii_to_char16(path, wp, 260);
    EFI_FILE_PROTOCOL *fh=0;
    EFI_STATUS st=root->Open(root,&fh,wp,EFI_FILE_MODE_READ,0);
    root->Close(root);
    if(EFI_ERROR(st) || !fh){ *err="file not found"; return 0; }
    /* 64 KiB read block: ~16x fewer firmware Read round-trips than 4 KiB, which
     * shortens the (uncancellable) hashing loop on large files. Kept static
     * rather than on-stack - a 64 KiB automatic array risks a UEFI stack
     * overflow, and crc_file runs single-threaded from the UI event path. */
    static UINT8 buf[65536];
    UINT32 c=0xFFFFFFFFu; UINT64 total=0;
    for(;;){
        UINTN rd=sizeof(buf);
        if(EFI_ERROR(fh->Read(fh,&rd,buf))){ *err="read error"; fh->Close(fh); return 0; }
        if(rd==0) break;
        c=crc32_upd(c,buf,rd); total+=rd;
    }
    fh->Close(fh);
    *out_crc=c^0xFFFFFFFFu; *out_size=total; return 1;
}
static int crc_buttons(int cw, int ch, wm_button *b)
{
    (void)cw; int y=ch-wm_button_h()-12;
    mkbtn(&b[0],1,14,y,"String");
    mkbtn(&b[1],2,14+b[0].w+8,y,"File");
    mkbtn(&b[2],3,14+b[0].w+8+b[1].w+16,y,"Compute");
    return 3;
}
static void crc_compute(crcstate *s)
{
    s->done=0; s->err=0; s->crc=0; s->size=0;
    if(s->mode==0){
        s->crc=crc32_buf((const UINT8*)s->in,(UINTN)s->len);
        s->size=(UINT64)s->len; s->done=1;
    } else {
        if(crc_file(s->in,&s->crc,&s->size,&s->err)) s->done=1;
    }
}
static void crc_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "CRC32");
    int y=cy+heading_h();
    draw_string_clip(cx+14, y, cw-28, g_crc.mode?"Mode: File (path on ESP)":"Mode: String",
                     FOREB_TITLE, FOREB_PANEL, 1, 1);
    y += inbox_h();
    draw_string_clip(cx+14, y, cw-28, g_crc.mode?"File path:":"Text:", FOREB_TEXT, FOREB_PANEL, 1, 1);
    y += inbox_h()-2;
    inbox(cx+14, y, cw-28, g_crc.in, 1);
    y += inbox_h() + 16;

    if(g_crc.err){
        char e[96]; int p=0; const char *pre="Error: "; while(*pre)e[p++]=*pre++;
        scopy(e+p,g_crc.err,(int)sizeof(e)-p);
        draw_string_clip(cx+14, y, cw-28, e, FOREB_TIMER, FOREB_PANEL, 1, 1);
    } else if(g_crc.done){
        char r[64]; int p=0; const char *pre="CRC32: 0x"; while(*pre)r[p++]=*pre++;
        char h[16]; hexn(g_crc.crc,8,h); for(int i=0;h[i];i++)r[p++]=h[i]; r[p]=0;
        draw_string_clip(cx+14, y, cw-28, r, FOREB_WHITE, FOREB_PANEL, 1, 2);
        y += 16*2*(ui_scale()<1?1:ui_scale()) + 8;
        char sz[48]; int q=0; const char *sp="Bytes: "; while(*sp)sz[q++]=*sp++;
        q+=u2dec(g_crc.size,sz+q); sz[q]=0;
        draw_string_clip(cx+14, y, cw-28, sz, FOREB_DIM, FOREB_PANEL, 1, 1);
    } else {
        draw_string_clip(cx+14, y, cw-28, "(enter input, then Compute)", FOREB_DIM, FOREB_PANEL, 1, 1);
    }

    wm_button *b; int nb=btns_get(&g_crc.bc, crc_buttons, cw, ch, &b);
    btns_draw(b, nb, g_crc.b_hover, g_crc.b_press);
    footer(cx, cy, cw, ch, "Tab switch mode  Enter compute  Esc close");
}
static int crc_event(wm_window *w, const wm_event *ev)
{
    crcstate *s=&g_crc; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==CHAR_TAB){ s->mode^=1; s->done=0; s->err=0; return 0; }
            if(tin_edit(s->in,&s->len,(int)sizeof(s->in),ev,0)) crc_compute(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,crc_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,crc_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,crc_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr){ if(pr==1){s->mode=0;s->done=0;s->err=0;} else if(pr==2){s->mode=1;s->done=0;s->err=0;} else crc_compute(s); }
            return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_crc32_open(void)
{
    if(g_crc.win) return;
    for(unsigned i=0;i<sizeof(g_crc);i++) ((UINT8*)&g_crc)[i]=0;
    g_crc.win=rng_open("CRC32", 48, 56, 400, 320, crc_draw, crc_event, &g_crc);
}

/* ==========================================================================
 * 5) FNV-1a hash (32 + 64 bit)
 * ========================================================================== */
typedef struct {
    wm_window *win; char in[128]; int len; int done;
    UINT32 h32; UINT64 h64; int b_hover,b_press;
    btncache bc;
} fnvstate;
static fnvstate g_fnv;

static void fnv_compute(fnvstate *s)
{
    UINT32 h32=2166136261u;
    UINT64 h64=14695981039346656037ULL;
    for(int i=0;i<s->len;i++){
        UINT8 c=(UINT8)s->in[i];
        h32 = (h32 ^ c) * 16777619u;
        h64 = (h64 ^ c) * 1099511628211ULL;
    }
    s->h32=h32; s->h64=h64; s->done=1;
}
static int fnv_buttons(int cw, int ch, wm_button *b)
{ (void)cw; int y=ch-wm_button_h()-12; mkbtn(&b[0],1,14,y,"Hash"); return 1; }
static void fnv_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "FNV-1a Hash");
    int y=cy+heading_h();
    draw_string_clip(cx+14, y, cw-28, "Text:", FOREB_TEXT, FOREB_PANEL, 1, 1);
    y += inbox_h()-2;
    inbox(cx+14, y, cw-28, g_fnv.in, 1);
    y += inbox_h() + 16;

    if(g_fnv.done){
        char r[80]; int p=0; const char *a="FNV-1a 32: 0x"; while(*a)r[p++]=*a++;
        char h[20]; hexn(g_fnv.h32,8,h); for(int i=0;h[i];i++)r[p++]=h[i]; r[p]=0;
        draw_string_clip(cx+14, y, cw-28, r, FOREB_WHITE, FOREB_PANEL, 1, 1);
        y += 16*(ui_scale()<1?1:ui_scale()) + 10;
        p=0; const char *b="FNV-1a 64: 0x"; while(*b)r[p++]=*b++;
        hexn(g_fnv.h64,16,h); for(int i=0;h[i];i++)r[p++]=h[i]; r[p]=0;
        draw_string_clip(cx+14, y, cw-28, r, FOREB_WHITE, FOREB_PANEL, 1, 1);
    } else {
        draw_string_clip(cx+14, y, cw-28, "(type text, press Hash)", FOREB_DIM, FOREB_PANEL, 1, 1);
    }

    wm_button *b; int nb=btns_get(&g_fnv.bc, fnv_buttons, cw, ch, &b);
    btns_draw(b, nb, g_fnv.b_hover, g_fnv.b_press);
    footer(cx, cy, cw, ch, "Type text  Enter hash  Esc close");
}
static int fnv_event(wm_window *w, const wm_event *ev)
{
    fnvstate *s=&g_fnv; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(tin_edit(s->in,&s->len,(int)sizeof(s->in),ev,0)) fnv_compute(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,fnv_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,fnv_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,fnv_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr) fnv_compute(s); return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_fnv_open(void)
{
    if(g_fnv.win) return;
    for(unsigned i=0;i<sizeof(g_fnv);i++) ((UINT8*)&g_fnv)[i]=0;
    g_fnv.win=rng_open("FNV-1a Hash", 46, 48, 400, 280, fnv_draw, fnv_event, &g_fnv);
}

/* ==========================================================================
 * 6) UUIDv4 generator
 * ========================================================================== */
typedef struct { wm_window *win; char cur[40]; char hist[6][40]; int nh; int b_hover,b_press; btncache bc; } uuidstate;
static uuidstate g_uuid;

static void uuid_fmt(UINT8 *b16, char *out)
{
    /* version 4 + RFC4122 variant */
    b16[6] = (UINT8)((b16[6] & 0x0F) | 0x40);
    b16[8] = (UINT8)((b16[8] & 0x3F) | 0x80);
    static const char hx[]="0123456789abcdef";
    int p=0;
    for(int i=0;i<16;i++){
        if(i==4||i==6||i==8||i==10) out[p++]='-';
        out[p++]=hx[b16[i]>>4]; out[p++]=hx[b16[i]&0xF];
    }
    out[p]=0;
}
static void uuid_gen(uuidstate *s)
{
    UINT8 b[16];
    UINT64 a=rng_u64(), c=rng_u64();
    for(int i=0;i<8;i++) b[i]=(UINT8)(a>>(i*8));
    for(int i=0;i<8;i++) b[8+i]=(UINT8)(c>>(i*8));
    if(s->cur[0]){
        for(int i=(s->nh<6?s->nh:5); i>0; i--) scopy(s->hist[i], s->hist[i-1], 40);
        scopy(s->hist[0], s->cur, 40);
        if(s->nh<6) s->nh++;
    }
    uuid_fmt(b, s->cur);
}
static int uuid_buttons(int cw, int ch, wm_button *b)
{ (void)cw; int y=ch-wm_button_h()-12; mkbtn(&b[0],1,14,y,"Generate"); return 1; }
static void uuid_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "UUIDv4 Generator");
    int y=cy+heading_h();
    fill_rect(cx+12, y, cw-24, inbox_h(), FOREB_BG);
    draw_rect_outline(cx+12, y, cw-24, inbox_h(), 1, FOREB_TITLE);
    draw_string_clip(cx+18, y+5, cw-36, g_uuid.cur[0]?g_uuid.cur:"(none)",
                     FOREB_WHITE, FOREB_BG, 1, 1);
    y += inbox_h() + 14;
    draw_string_clip(cx+14, y, cw-28, "Recent:", FOREB_TITLE, FOREB_PANEL, 1, 1);
    y += 16*(ui_scale()<1?1:ui_scale()) + 4;
    for(int i=0;i<g_uuid.nh;i++){
        draw_string_clip(cx+14, y, cw-28, g_uuid.hist[i], FOREB_DIM, FOREB_PANEL, 1, 1);
        y += 16*(ui_scale()<1?1:ui_scale()) + 2;
    }
    wm_button *b; int nb=btns_get(&g_uuid.bc, uuid_buttons, cw, ch, &b);
    btns_draw(b, nb, g_uuid.b_hover, g_uuid.b_press);
    footer(cx, cy, cw, ch, "g/Enter/Space generate  Esc close");
}
static int uuid_event(wm_window *w, const wm_event *ev)
{
    uuidstate *s=&g_uuid; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode=='g'||ev->unicode=='G'||ev->unicode==CHAR_CR||ev->unicode==' ') uuid_gen(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,uuid_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,uuid_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; else uuid_gen(s); return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,uuid_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr) uuid_gen(s); return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_uuid_open(void)
{
    if(g_uuid.win) return;
    for(unsigned i=0;i<sizeof(g_uuid);i++) ((UINT8*)&g_uuid)[i]=0;
    uuid_gen(&g_uuid);
    g_uuid.win=rng_open("UUIDv4 Generator", 48, 56, 420, 340, uuid_draw, uuid_event, &g_uuid);
}

/* ==========================================================================
 * 7) RPG DICE - NdM+K
 * ========================================================================== */
#define DICE_MAXROLLS 40
typedef struct {
    wm_window *win; char expr[24]; int len;
    int rolls[DICE_MAXROLLS]; int nrolls; long long total;
    int ok; const char *err; int b_hover,b_press;
    btncache bc;
} dicestate;
static dicestate g_dice;

/* Parse "NdM+K" / "NdM-K" / "dM". Returns 1 on success. */
static int dice_parse(const char *s, int *n, int *m, int *k)
{
    int i=0; long long N=0, M=0, K=0; int sign=1; int haveN=0, haveM=0;
    while(s[i]==' ') i++;
    while(s[i]>='0'&&s[i]<='9'){ N=N*10+(s[i]-'0'); haveN=1; i++; if(N>1000)return 0; }
    if(s[i]!='d'&&s[i]!='D') return 0;
    i++;
    while(s[i]>='0'&&s[i]<='9'){ M=M*10+(s[i]-'0'); haveM=1; i++; if(M>100000)return 0; }
    if(!haveM || M<1) return 0;
    if(!haveN) N=1;
    if(s[i]=='+'||s[i]=='-'){ sign=(s[i]=='-')?-1:1; i++;
        int haveK=0; while(s[i]>='0'&&s[i]<='9'){ K=K*10+(s[i]-'0'); haveK=1; i++; if(K>1000000)return 0; }
        if(!haveK) return 0; }
    while(s[i]==' ') i++;
    if(s[i]!=0) return 0;
    if(N<1||N>DICE_MAXROLLS) return 0;
    *n=(int)N; *m=(int)M; *k=(int)(sign*K); return 1;
}
static void dice_roll(dicestate *s)
{
    int n,m,k;
    s->ok=0; s->err=0; s->nrolls=0; s->total=0;
    if(!dice_parse(s->expr,&n,&m,&k)){ s->err="format: NdM+K (e.g. 3d6+2)"; return; }
    long long sum=0;
    for(int i=0;i<n;i++){ int r=rng_range(m)+1; s->rolls[i]=r; sum+=r; }
    s->nrolls=n; s->total=sum+k; s->ok=1;
}
static int dice_buttons(int cw, int ch, wm_button *b)
{ (void)cw; int y=ch-wm_button_h()-12; mkbtn(&b[0],1,14,y,"Roll"); return 1; }
static void dice_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "RPG Dice");
    int y=cy+heading_h();
    draw_string_clip(cx+14, y, cw-28, "Dice expression (NdM+K):", FOREB_TEXT, FOREB_PANEL, 1, 1);
    y += inbox_h()-2;
    inbox(cx+14, y, cw-28, g_dice.expr, 1);
    y += inbox_h() + 14;

    if(g_dice.err){
        draw_string_clip(cx+14, y, cw-28, g_dice.err, FOREB_TIMER, FOREB_PANEL, 1, 1);
    } else if(g_dice.ok){
        /* individual rolls, wrapped */
        char ln[112]; int p=0; ln[0]=0;   /* wrap guard p>96 + max 6-digit roll + ' ' + NUL */
        const char *pre="Rolls: "; while(*pre)ln[p++]=*pre++;
        for(int i=0;i<g_dice.nrolls;i++){
            if(p>96){ ln[p]=0; draw_string_clip(cx+14,y,cw-28,ln,FOREB_TEXT,FOREB_PANEL,1,1);
                      y+=16*(ui_scale()<1?1:ui_scale())+2; p=0; }
            p+=u2dec((UINT64)g_dice.rolls[i], ln+p);
            if(i<g_dice.nrolls-1){ ln[p++]=' '; }
        }
        ln[p]=0; draw_string_clip(cx+14,y,cw-28,ln,FOREB_TEXT,FOREB_PANEL,1,1);
        y += 16*(ui_scale()<1?1:ui_scale()) + 10;
        char tot[48]; int q=0; const char *tp="Total: "; while(*tp)tot[q++]=*tp++;
        q+=i2dec(g_dice.total,tot+q); tot[q]=0;
        draw_string_clip(cx+14, y, cw-28, tot, FOREB_WHITE, FOREB_PANEL, 1, 2);
    } else {
        draw_string_clip(cx+14, y, cw-28, "e.g. 3d6+2, 1d20, 2d10-1", FOREB_DIM, FOREB_PANEL, 1, 1);
    }

    wm_button *b; int nb=btns_get(&g_dice.bc, dice_buttons, cw, ch, &b);
    btns_draw(b, nb, g_dice.b_hover, g_dice.b_press);
    footer(cx, cy, cw, ch, "Type expr  Enter roll  Esc close");
}
static int dice_event(wm_window *w, const wm_event *ev)
{
    dicestate *s=&g_dice; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(tin_edit(s->expr,&s->len,(int)sizeof(s->expr),ev,0)) dice_roll(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,dice_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,dice_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,dice_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr) dice_roll(s); return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_dice_open(void)
{
    if(g_dice.win) return;
    for(unsigned i=0;i<sizeof(g_dice);i++) ((UINT8*)&g_dice)[i]=0;
    scopy(g_dice.expr,"3d6",(int)sizeof(g_dice.expr)); g_dice.len=slen(g_dice.expr);
    g_dice.win=rng_open("RPG Dice", 46, 52, 400, 320, dice_draw, dice_event, &g_dice);
}

/* ==========================================================================
 * 8) DICE STATISTICS - roll many, histogram the faces.
 * ========================================================================== */
static const int DS_SIDES[] = { 4, 6, 8, 10, 12, 20 };
#define DS_NSIDES ((int)(sizeof(DS_SIDES)/sizeof(DS_SIDES[0])))
#define DS_MAXFACE 20
#define DS_BATCH 1000

typedef struct {
    wm_window *win; int sidx; UINT32 counts[DS_MAXFACE]; UINT64 trials;
    int b_hover, b_press;
    btncache bc;
} dstatstate;
static dstatstate g_ds;

static int ds_buttons(int cw, int ch, wm_button *b)
{
    (void)cw; int y=ch-wm_button_h()-12;
    mkbtn(&b[0],1,14,y,"< die");
    mkbtn(&b[1],2,14+b[0].w+8,y,"die >");
    mkbtn(&b[2],3,14+b[0].w+8+b[1].w+16,y,"Roll x1000");
    mkbtn(&b[3],4,14+b[0].w+8+b[1].w+16+b[2].w+8,y,"Reset");
    return 4;
}
static void ds_reset(dstatstate *s){ for(int i=0;i<DS_MAXFACE;i++)s->counts[i]=0; s->trials=0; }
static void ds_roll(dstatstate *s)
{
    int m=DS_SIDES[s->sidx];
    for(int i=0;i<DS_BATCH;i++) s->counts[rng_range(m)]++;
    s->trials+=DS_BATCH;
}
static void ds_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "Dice Statistics");
    int y=cy+heading_h();
    int m=DS_SIDES[g_ds.sidx];

    char hd[64]; int p=0; hd[p++]='d'; p+=u2dec((UINT64)m,hd+p);
    const char *tp="   trials: "; while(*tp)hd[p++]=*tp++; p+=u2dec(g_ds.trials,hd+p); hd[p]=0;
    draw_string_clip(cx+14, y, cw-28, hd, FOREB_TITLE, FOREB_PANEL, 1, 1);
    y += 16*(ui_scale()<1?1:ui_scale()) + 8;

    UINT32 mx=1; for(int i=0;i<m;i++) if(g_ds.counts[i]>mx) mx=g_ds.counts[i];
    int barmax = cw - 130; if(barmax<40) barmax=40;
    int rowh = 14*(ui_scale()<1?1:ui_scale());
    int bottom = ch - wm_button_h() - 28;
    for(int i=0;i<m;i++){
        if(cy+ (y-cy) + rowh > cy+bottom) break;
        char lab[16]; int q=0; q+=u2dec((UINT64)(i+1),lab+q); lab[q]=0;
        draw_string_clip(cx+14, y, 34, lab, FOREB_TEXT, FOREB_PANEL, 1, 1);
        int bw=(int)((UINT64)g_ds.counts[i]*(UINT64)barmax/mx);
        fill_rect(cx+50, y+2, bw, rowh-6, FOREB_TITLE);
        char cnt[16]; u2dec((UINT64)g_ds.counts[i], cnt);
        draw_string_clip(cx+54+bw, y, cw-(54+bw)-14, cnt, FOREB_DIM, FOREB_PANEL, 1, 1);
        y += rowh;
    }
    if(g_ds.trials){
        char ex[48]; int e=0; const char *ep="Expected/face: "; while(*ep)ex[e++]=*ep++;
        e+=u2dec(g_ds.trials/(UINT64)m, ex+e); ex[e]=0;
        int fy=cy+ch-wm_button_h()-30;
        draw_string_clip(cx+14, fy, cw-28, ex, FOREB_DIM, FOREB_PANEL, 1, 1);
    }

    wm_button *b; int nb=btns_get(&g_ds.bc, ds_buttons, cw, ch, &b);
    btns_draw(b, nb, g_ds.b_hover, g_ds.b_press);
    footer(cx, cy, cw, ch, "Left/Right die  Space roll  r reset  Esc close");
}
static void ds_setside(dstatstate *s, int d)
{ s->sidx=(s->sidx+DS_NSIDES+d)%DS_NSIDES; ds_reset(s); }
static void ds_do(dstatstate *s, int id)
{
    if(id==1) ds_setside(s,-1);
    else if(id==2) ds_setside(s,+1);
    else if(id==3) ds_roll(s);
    else if(id==4) ds_reset(s);
}
static int ds_event(wm_window *w, const wm_event *ev)
{
    dstatstate *s=&g_ds; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_LEFT) ds_setside(s,-1);
            else if(ev->scancode==SCAN_RIGHT) ds_setside(s,+1);
            else if(ev->unicode==' '||ev->unicode==CHAR_CR) ds_roll(s);
            else if(ev->unicode=='r'||ev->unicode=='R') ds_reset(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,ds_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,ds_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,ds_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr) ds_do(s,pr); return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_dicestat_open(void)
{
    if(g_ds.win) return;
    for(unsigned i=0;i<sizeof(g_ds);i++) ((UINT8*)&g_ds)[i]=0;
    g_ds.sidx=1;                 /* d6 */
    g_ds.win=rng_open("Dice Statistics", 50, 66, 420, 400, ds_draw, ds_event, &g_ds);
}

/* ==========================================================================
 * 9) NUMBER GUESSER - computer picks 1..100.
 * ========================================================================== */
typedef struct {
    wm_window *win; int secret; int lo, hi; int tries; int won;
    char in[8]; int len; char msg[40]; int b_hover,b_press;
    btncache bc;
} guessstate;
static guessstate g_guess;

static void guess_new(guessstate *s)
{
    s->secret=rng_range(100)+1; s->lo=1; s->hi=100; s->tries=0; s->won=0;
    s->in[0]=0; s->len=0; scopy(s->msg,"Guess 1..100",(int)sizeof(s->msg));
}
static void guess_submit(guessstate *s)
{
    if(s->won){ guess_new(s); return; }
    if(s->len==0) return;
    int g=0; for(int i=0;i<s->len;i++) g=g*10+(s->in[i]-'0');
    s->in[0]=0; s->len=0;
    if(g<1||g>100){ scopy(s->msg,"Enter 1..100",(int)sizeof(s->msg)); return; }
    s->tries++;
    if(g==s->secret){
        s->won=1;
        char m[40]; int p=0; const char *a="Correct in "; while(*a)m[p++]=*a++;
        p+=u2dec((UINT64)s->tries,m+p); const char *b=" tries!"; while(*b)m[p++]=*b++; m[p]=0;
        scopy(s->msg,m,(int)sizeof(s->msg));
    } else if(g<s->secret){
        if(g>=s->lo) s->lo=g+1; scopy(s->msg,"Higher!",(int)sizeof(s->msg));
    } else {
        if(g<=s->hi) s->hi=g-1; scopy(s->msg,"Lower!",(int)sizeof(s->msg));
    }
}
static int guess_buttons(int cw, int ch, wm_button *b)
{
    (void)cw; int y=ch-wm_button_h()-12;
    mkbtn(&b[0],1,14,y,"Guess");
    mkbtn(&b[1],2,14+b[0].w+10,y,"New Game");
    return 2;
}
static void guess_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    (void)w;
    fill_rect(cx, cy, cw, ch, FOREB_PANEL);
    heading(cx, cy, cw, "Number Guesser");
    int y=cy+heading_h();
    draw_string_clip(cx+14, y, cw-28, "I'm thinking of a number 1..100", FOREB_TEXT, FOREB_PANEL, 1, 1);
    y += 16*(ui_scale()<1?1:ui_scale()) + 6;

    char rg[48]; int p=0; const char *rp="Range: "; while(*rp)rg[p++]=*rp++;
    p+=u2dec((UINT64)g_guess.lo,rg+p); rg[p++]='-'; p+=u2dec((UINT64)g_guess.hi,rg+p);
    const char *tp="   Tries: "; while(*tp)rg[p++]=*tp++; p+=u2dec((UINT64)g_guess.tries,rg+p); rg[p]=0;
    draw_string_clip(cx+14, y, cw-28, rg, FOREB_DIM, FOREB_PANEL, 1, 1);
    y += 16*(ui_scale()<1?1:ui_scale()) + 10;

    inbox(cx+14, y, cw-28, g_guess.in, !g_guess.won);
    y += inbox_h() + 16;

    UINT32 mc = g_guess.won?FOREB_TITLE:FOREB_TIMER;
    draw_string_clip(cx+14, y, cw-28, g_guess.msg, mc, FOREB_PANEL, 1, 2);

    wm_button *b; int nb=btns_get(&g_guess.bc, guess_buttons, cw, ch, &b);
    btns_draw(b, nb, g_guess.b_hover, g_guess.b_press);
    footer(cx, cy, cw, ch, "Type number  Enter guess  n new game  Esc close");
}
static int guess_event(wm_window *w, const wm_event *ev)
{
    guessstate *s=&g_guess; int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if((ev->unicode=='n'||ev->unicode=='N') && s->len==0){ guess_new(s); return 0; }
            if(tin_edit(s->in,&s->len,(int)sizeof(s->in),ev,1)) guess_submit(s);
            return 0;
        case WM_EV_MOUSE_MOVE:{ wm_button *b; int nb=btns_get(&s->bc,guess_buttons,cw,ch,&b);
            s->b_hover=btn_hit(b,nb,ev->mx,ev->my); return 0; }
        case WM_EV_MOUSE_DOWN:{ wm_button *b; int nb=btns_get(&s->bc,guess_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my); if(id)s->b_press=id; return 0; }
        case WM_EV_MOUSE_UP:{ if(!s->b_press)return 0; wm_button *b; int nb=btns_get(&s->bc,guess_buttons,cw,ch,&b);
            int id=btn_hit(b,nb,ev->mx,ev->my), pr=s->b_press; s->b_press=0;
            if(id==pr){ if(pr==1)guess_submit(s); else guess_new(s); } return 0; }
        case WM_EV_CLOSE: s->win=NULL; return 0;
        default: return 0;
    }
}
void tool_rng_guess_open(void)
{
    if(g_guess.win) return;
    for(unsigned i=0;i<sizeof(g_guess);i++) ((UINT8*)&g_guess)[i]=0;
    guess_new(&g_guess);
    g_guess.win=rng_open("Number Guesser", 42, 50, 360, 300, guess_draw, guess_event, &g_guess);
}

/* ==========================================================================
 * Category table.
 * ========================================================================== */
const struct forebo_tool cat_rng_tools[] = {
    { "Password Generator", "Random passwords (RDRAND/TSC, charset toggles)", "safe",     tool_rng_passwd_open   },
    { "Coin Flip",          "Flip a coin with a running heads/tails tally",   "safe",     tool_rng_coin_open     },
    { "Magic 8-Ball",       "Ask a yes/no question, shake for an answer",     "safe",     tool_rng_magic8_open   },
    { "CRC32",              "CRC32 of a typed string or an ESP file",         "text",     tool_rng_crc32_open    },
    { "FNV-1a Hash",        "FNV-1a 32/64-bit hash of a string",              "text",     tool_rng_fnv_open      },
    { "UUIDv4 Generator",   "Generate random RFC-4122 UUIDs",                 "gear",     tool_rng_uuid_open     },
    { "RPG Dice",           "Roll dice with NdM+K notation",                  "gear",     tool_rng_dice_open     },
    { "Dice Statistics",    "Roll thousands and histogram the faces",         "gear",     tool_rng_dicestat_open },
    { "Number Guesser",     "Guess the computer's secret 1..100",             "terminal", tool_rng_guess_open    },
};
const int cat_rng_count = (int)(sizeof(cat_rng_tools)/sizeof(cat_rng_tools[0]));

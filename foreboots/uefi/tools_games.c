/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_games.c - "Games" tool category (10 playable mini-games).
 * =============================================================================
 * Template-B windows (see tools.h). Pure compute/draw, integer math only (no
 * float: -mno-sse/-mno-mmx), fixed static buffers, no heap, no firmware
 * services. Per-frame ticks come from the menu loop's repeated draw calls; the
 * event callbacks handle keyboard + mouse. All drawing is clipped to the
 * window client rectangle via gfill()/draw_string_clip().
 *
 * Entropy: RDTSC feeds a xorshift64 PRNG (no RDRAND / RTC dependency).
 * ========================================================================== */
#include "efi.h"
#include "ui.h"
#include "wm.h"
#include "input.h"
#include "tools_games.h"

/* ================================================================== */
/*  Shared freestanding helpers                                        */
/* ================================================================== */
static void u32toa(UINT32 v, char *o){
    char t[12]; int i=0;
    if(!v) t[i++]='0';
    while(v){ t[i++]=(char)('0'+(v%10u)); v/=10u; }
    int j=0; while(i>0) o[j++]=t[--i]; o[j]=0;
}
static void i32toa(int v, char *o){
    if(v<0){ *o++='-'; v=-v; }
    u32toa((UINT32)v, o);
}
/* tiny string builder into a fixed buffer */
static void sb_puts(char *d, int cap, int *pos, const char *s){
    int p=*pos; if(s) for(int i=0; s[i] && p<cap-1; i++) d[p++]=s[i];
    d[p]=0; *pos=p;
}
static void sb_putn(char *d, int cap, int *pos, int v){
    char t[12]; i32toa(v,t); sb_puts(d,cap,pos,t);
}

/* xorshift64 PRNG seeded/stirred from the CPU timestamp counter. */
static inline UINT64 rdtsc_now(void){
    UINT32 lo,hi;
    __asm__ __volatile__("rdtsc":"=a"(lo),"=d"(hi));
    return ((UINT64)hi<<32)|lo;
}
static UINT64 g_rng_state;
static void rng_stir(void){ g_rng_state ^= rdtsc_now(); if(!g_rng_state) g_rng_state=0x9E3779B97F4A7C15ULL; }
static UINT32 rng_next(void){
    if(!g_rng_state) rng_stir();
    UINT64 x=g_rng_state; x^=x<<13; x^=x>>7; x^=x<<17; g_rng_state=x;
    return (UINT32)(x>>32);
}
static int rng_range(int n){ if(n<=0) return 0; return (int)(rng_next()%(UINT32)n); }

/* Clipped fill: rect in SCREEN coords, intersected with client [cx,cx+cw). */
static void gfill(int cx,int cy,int cw,int ch,int x,int y,int w,int h,UINT32 col){
    int x0=x,y0=y,x1=x+w,y1=y+h;
    if(x0<cx)x0=cx; if(y0<cy)y0=cy;
    if(x1>cx+cw)x1=cx+cw; if(y1>cy+ch)y1=cy+ch;
    if(x1>x0 && y1>y0) fill_rect(x0,y0,x1-x0,y1-y0,col);
}
/* Clipped 1px outline (screen coords), only draws edges inside the client. */
static void goutline(int cx,int cy,int cw,int ch,int x,int y,int w,int h,UINT32 col){
    gfill(cx,cy,cw,ch, x,        y,        w, 1, col);
    gfill(cx,cy,cw,ch, x,        y+h-1,    w, 1, col);
    gfill(cx,cy,cw,ch, x,        y,        1, h, col);
    gfill(cx,cy,cw,ch, x+w-1,    y,        1, h, col);
}

#define GMARGIN 8

/* ================================================================== */
/*  1. SNAKE                                                           */
/* ================================================================== */
#define SNK_W 24
#define SNK_H 18
#define SNK_MAX (SNK_W*SNK_H)

typedef struct {
    wm_window *win;
    short sx[SNK_MAX], sy[SNK_MAX];
    int   len;
    int   dx, dy;         /* current heading                 */
    int   ndx, ndy;       /* queued heading                  */
    int   fx, fy;         /* food                            */
    int   dead, running;
    int   score, best;
    unsigned tick;
    int   speed;          /* frames per step (lower = faster)*/
} snake_state;
static snake_state g_snk;

static void snake_place_food(void){
    for(int tries=0; tries<4096; tries++){
        int x=rng_range(SNK_W), y=rng_range(SNK_H); int on=0;
        for(int i=0;i<g_snk.len;i++) if(g_snk.sx[i]==x && g_snk.sy[i]==y){on=1;break;}
        if(!on){ g_snk.fx=x; g_snk.fy=y; return; }
    }
    g_snk.fx=0; g_snk.fy=0;
}
static void snake_reset(void){
    rng_stir();
    g_snk.len=4;
    for(int i=0;i<g_snk.len;i++){ g_snk.sx[i]=(short)(SNK_W/2-i); g_snk.sy[i]=SNK_H/2; }
    g_snk.dx=1; g_snk.dy=0; g_snk.ndx=1; g_snk.ndy=0;
    g_snk.dead=0; g_snk.running=0; g_snk.score=0; g_snk.tick=0; g_snk.speed=7;
    snake_place_food();
}
static void snake_step(void){
    g_snk.dx=g_snk.ndx; g_snk.dy=g_snk.ndy;
    int hx=g_snk.sx[0]+g_snk.dx, hy=g_snk.sy[0]+g_snk.dy;
    if(hx<0||hx>=SNK_W||hy<0||hy>=SNK_H){ g_snk.dead=1; g_snk.running=0; return; }
    int grow = (hx==g_snk.fx && hy==g_snk.fy);
    int chk = grow ? g_snk.len : g_snk.len-1;   /* tail vacates when not growing */
    for(int i=0;i<chk;i++) if(g_snk.sx[i]==hx && g_snk.sy[i]==hy){ g_snk.dead=1; g_snk.running=0; return; }
    if(grow && g_snk.len<SNK_MAX) g_snk.len++;
    for(int i=g_snk.len-1;i>0;i--){ g_snk.sx[i]=g_snk.sx[i-1]; g_snk.sy[i]=g_snk.sy[i-1]; }
    g_snk.sx[0]=(short)hx; g_snk.sy[0]=(short)hy;
    if(grow){
        g_snk.score+=10; if(g_snk.score>g_snk.best) g_snk.best=g_snk.score;
        if(g_snk.speed>3 && (g_snk.score%50)==0) g_snk.speed--;
        snake_place_food();
    }
}
static void snake_draw(wm_window *w, int cx, int cy, int cw, int ch){
    snake_state *s=&g_snk; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    UINT32 acc=wm_theme_color(WM_COL_ACCENT);
    fill_rect(cx,cy,cw,ch,bg);
    int top=26;
    /* per-frame tick */
    s->tick++;
    if(s->running && !s->dead && (s->tick % (unsigned)s->speed)==0) snake_step();
    /* board geometry */
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int cell=availw/SNK_W; int c2=availh/SNK_H; if(c2<cell)cell=c2; if(cell<3)cell=3;
    int bw=cell*SNK_W, bh=cell*SNK_H;
    int bx=cx+(cw-bw)/2, by=cy+top+(availh-bh)/2;
    /* header */
    char hb[48]; int p=0; sb_puts(hb,sizeof hb,&p,"Score "); sb_putn(hb,sizeof hb,&p,s->score);
    sb_puts(hb,sizeof hb,&p,"  Best "); sb_putn(hb,sizeof hb,&p,s->best);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    /* playfield */
    gfill(cx,cy,cw,ch,bx,by,bw,bh,wm_blend(bg,0x00000000u,80));
    goutline(cx,cy,cw,ch,bx-1,by-1,bw+2,bh+2,wm_blend(bg,fg,60));
    /* food */
    gfill(cx,cy,cw,ch,bx+s->fx*cell+1,by+s->fy*cell+1,cell-2,cell-2,0x00E24A4Au);
    /* snake */
    UINT32 body=wm_blend(acc,0x0020C060u,120);
    for(int i=0;i<s->len;i++){
        UINT32 col = (i==0)?acc:body;
        gfill(cx,cy,cw,ch,bx+s->sx[i]*cell,by+s->sy[i]*cell,cell-1,cell-1,col);
    }
    if(!s->running && !s->dead)
        draw_string_center(cx+cw/2,by+bh/2-8,"Arrows/WASD to start",fg,bg,1,1);
    if(s->dead){
        gfill(cx,cy,cw,ch,bx,by+bh/2-16,bw,32,wm_blend(bg,0x00000000u,150));
        draw_string_center(cx+cw/2,by+bh/2-12,"GAME OVER",0x00FF6060u,bg,1,2);
        draw_string_center(cx+cw/2,by+bh/2+12,"Press R to restart",fg,bg,1,1);
    }
}
static int snake_event(wm_window *w, const wm_event *ev){
    snake_state *s=&g_snk; (void)w;
    if(ev->type==WM_EV_CLOSE){ s->win=NULL; return 0; }
    if(ev->type!=WM_EV_KEY) return 0;
    UINT16 sc=ev->scancode; CHAR16 u=ev->unicode;
    if(sc==SCAN_ESC) return WM_CLOSE_REQUEST;
    if(u=='r'||u=='R'){ snake_reset(); return 0; }
    if(u==' '){ if(!s->dead) s->running=!s->running; return 0; }
    int nx=s->ndx, ny=s->ndy;
    if(sc==SCAN_UP||u=='w'||u=='W'){ nx=0; ny=-1; }
    else if(sc==SCAN_DOWN||u=='s'||u=='S'){ nx=0; ny=1; }
    else if(sc==SCAN_LEFT||u=='a'||u=='A'){ nx=-1; ny=0; }
    else if(sc==SCAN_RIGHT||u=='d'||u=='D'){ nx=1; ny=0; }
    else return 0;
    if(s->dead) return 0;
    /* no 180-degree reversal */
    if(nx==-s->dx && ny==-s->dy) return 0;
    s->ndx=nx; s->ndy=ny; s->running=1;
    return 0;
}
void tool_games_snake_open(void){
    if(g_snk.win) return;
    /* snake_reset() keeps g_snk.best, so the hi-score survives sessions */
    snake_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*45/100; if(ww<420)ww=420; if(ww>560)ww=560; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<380)wh=380; if(wh>520)wh=520; if(wh>H-40)wh=H-40;
    g_snk.win=wm_open("Snake",ww,wh,snake_draw,snake_event,&g_snk);
}

/* ================================================================== */
/*  2. PONG (vs simple AI)                                             */
/* ================================================================== */
typedef struct {
    wm_window *win;
    int ply, ry;          /* paddle centers (client y)      */
    int bx, by, vx, vy;   /* ball                           */
    int ps, as;           /* scores                         */
    int running, over;
    int key_up, key_dn;
    int mouse_y, use_mouse;
    unsigned tick;
} pong_state;
static pong_state g_pong;

#define PONG_PADH 60
#define PONG_PADW 8
#define PONG_TOP  26
#define PONG_WIN  7

static void pong_serve(int to_player){
    /* leave bx/by as the sentinel set by the caller; geom centers it in draw */
    g_pong.vx = to_player ? -5 : 5;
    g_pong.vy = (rng_range(2)?1:-1) * (2+rng_range(3));
    g_pong.tick=0;
}
static void pong_reset(void){
    rng_stir();
    g_pong.ply=0; g_pong.ry=0; g_pong.ps=0; g_pong.as=0;
    g_pong.running=0; g_pong.over=0; g_pong.use_mouse=0;
    g_pong.bx=-99999;                        /* sentinel: center on first draw */
    pong_serve(rng_range(2));
}
static void pong_draw(wm_window *w, int cx, int cy, int cw, int ch){
    pong_state *s=&g_pong; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    UINT32 acc=wm_theme_color(WM_COL_ACCENT);
    fill_rect(cx,cy,cw,ch,bg);
    int fx=GMARGIN, fy=PONG_TOP, fw=cw-2*GMARGIN, fh=ch-PONG_TOP-GMARGIN;
    if(fw<40)fw=40; if(fh<40)fh=40;
    /* init / clamp positions to field */
    if(s->bx<=-9999){ s->bx=fx+fw/2; s->by=fy+fh/2; s->ply=fy+fh/2; s->ry=fy+fh/2; }
    int half=PONG_PADH/2;
    /* ---- simulate one frame ---- */
    s->tick++;
    if(s->running && !s->over){
        /* player paddle */
        if(s->use_mouse){ s->ply = s->mouse_y; }
        else { if(s->key_up) s->ply-=7; if(s->key_dn) s->ply+=7; }
        if(s->ply<fy+half) s->ply=fy+half; if(s->ply>fy+fh-half) s->ply=fy+fh-half;
        /* AI paddle: chase with capped speed + deadzone */
        int diff = s->by - s->ry;
        int aspd = 5;
        if(diff> 6) s->ry += (diff<aspd?diff:aspd);
        else if(diff<-6) s->ry -= (-diff<aspd?-diff:aspd);
        if(s->ry<fy+half) s->ry=fy+half; if(s->ry>fy+fh-half) s->ry=fy+fh-half;
        /* ball */
        s->bx += s->vx; s->by += s->vy;
        if(s->by<fy+3){ s->by=fy+3; s->vy=-s->vy; }
        if(s->by>fy+fh-3){ s->by=fy+fh-3; s->vy=-s->vy; }
        /* left paddle collision (player) */
        int px=fx+2+PONG_PADW;
        if(s->vx<0 && s->bx<=px && s->bx>=fx){
            if(s->by>=s->ply-half-3 && s->by<=s->ply+half+3){
                s->bx=px; s->vx=-s->vx; s->vy += (s->by-s->ply)/12;
            }
        }
        /* right paddle collision (AI) */
        int rx=fx+fw-2-PONG_PADW;
        if(s->vx>0 && s->bx>=rx && s->bx<=fx+fw){
            if(s->by>=s->ry-half-3 && s->by<=s->ry+half+3){
                s->bx=rx; s->vx=-s->vx; s->vy += (s->by-s->ry)/12;
            }
        }
        if(s->vy>7)s->vy=7; if(s->vy<-7)s->vy=-7;
        /* score */
        if(s->bx<fx){ s->as++; s->bx=-9999; pong_serve(0); s->running=1;
                      if(s->as>=PONG_WIN){s->over=1;s->running=0;} }
        else if(s->bx>fx+fw){ s->ps++; s->bx=-9999; pong_serve(1); s->running=1;
                      if(s->ps>=PONG_WIN){s->over=1;s->running=0;} }
        if(s->bx<=-9999){ s->bx=fx+fw/2; s->by=fy+fh/2; }
    }
    /* ---- draw ---- */
    gfill(cx,cy,cw,ch,cx+fx,cy+fy,fw,fh,wm_blend(bg,0x00000000u,70));
    goutline(cx,cy,cw,ch,cx+fx,cy+fy,fw,fh,wm_blend(bg,fg,60));
    /* center dashed line */
    for(int y=fy;y<fy+fh;y+=14) gfill(cx,cy,cw,ch,cx+fx+fw/2-1,cy+y,2,7,wm_blend(bg,fg,50));
    /* paddles */
    gfill(cx,cy,cw,ch,cx+fx+2,cy+s->ply-half,PONG_PADW,PONG_PADH,acc);
    gfill(cx,cy,cw,ch,cx+fx+fw-2-PONG_PADW,cy+s->ry-half,PONG_PADW,PONG_PADH,0x00E0A040u);
    /* ball */
    gfill(cx,cy,cw,ch,cx+s->bx-4,cy+s->by-4,8,8,fg);
    /* scores */
    char sb[16]; i32toa(s->ps,sb); draw_string_center(cx+fx+fw/4,cy+fy+6,sb,acc,bg,1,2);
    i32toa(s->as,sb); draw_string_center(cx+fx+3*fw/4,cy+fy+6,sb,0x00E0A040u,bg,1,2);
    draw_string_clip(cx+6,cy+5,cw/2,"You  vs  AI",fg,bg,1,1);
    if(!s->running && !s->over)
        draw_string_center(cx+cw/2,cy+fy+fh/2,"Space to serve - Up/Down or mouse",fg,bg,1,1);
    if(s->over){
        const char *msg = (s->ps>s->as)?"YOU WIN!":"AI WINS";
        draw_string_center(cx+cw/2,cy+fy+fh/2-8,msg,(s->ps>s->as)?0x0060FF60u:0x00FF6060u,bg,1,2);
        draw_string_center(cx+cw/2,cy+fy+fh/2+16,"Press R to restart",fg,bg,1,1);
    }
}
static int pong_event(wm_window *w, const wm_event *ev){
    pong_state *s=&g_pong; (void)w;
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY: {
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode=='r'||ev->unicode=='R'){ pong_reset(); return 0; }
            if(ev->unicode==' '){ if(!s->over){ s->running=1; } return 0; }
            if(ev->scancode==SCAN_UP||ev->unicode=='w'||ev->unicode=='W'){ s->use_mouse=0; s->ply-=18; s->running|=!s->over; }
            if(ev->scancode==SCAN_DOWN||ev->unicode=='s'||ev->unicode=='S'){ s->use_mouse=0; s->ply+=18; s->running|=!s->over; }
            return 0;
        }
        case WM_EV_MOUSE_MOVE:
            s->use_mouse=1; s->mouse_y=ev->my; if(!s->over) s->running=1; return 0;
        case WM_EV_MOUSE_DOWN:
            if(!s->over) s->running=1; return 0;
        default: return 0;
    }
}
void tool_games_pong_open(void){
    if(g_pong.win) return;
    pong_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*55/100; if(ww<480)ww=480; if(ww>680)ww=680; if(ww>W-40)ww=W-40;
    int wh=H*50/100; if(wh<320)wh=320; if(wh>460)wh=460; if(wh>H-40)wh=H-40;
    g_pong.win=wm_open("Pong",ww,wh,pong_draw,pong_event,&g_pong);
}

/* ================================================================== */
/*  3. TIC-TAC-TOE (vs minimax AI)                                     */
/* ================================================================== */
typedef struct {
    wm_window *win;
    int b[9];             /* 0 empty, 1 X (player), 2 O (AI)  */
    int over, winner;     /* winner 0 none/draw,1,2           */
    int hover;
    int pw, aw, draws;
} ttt_state;
static ttt_state g_ttt;

static int ttt_winner(const int *b){
    static const int L[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
    for(int i=0;i<8;i++){ int a=b[L[i][0]]; if(a && a==b[L[i][1]] && a==b[L[i][2]]) return a; }
    return 0;
}
static int ttt_full(const int *b){ for(int i=0;i<9;i++) if(!b[i]) return 0; return 1; }
/* minimax: returns score for O (AI). +10-depth win, -(10-depth) loss. */
static int ttt_minimax(int *b, int aiturn, int depth, int alpha, int beta){
    int wn=ttt_winner(b);
    if(wn==2) return 10-depth;
    if(wn==1) return depth-10;
    if(ttt_full(b)) return 0;
    int best = aiturn ? -1000 : 1000;
    for(int i=0;i<9;i++){
        if(b[i]) continue;
        b[i]= aiturn?2:1;
        int sc=ttt_minimax(b,!aiturn,depth+1,alpha,beta);
        b[i]=0;
        if(aiturn){ if(sc>best) best=sc; if(best>alpha) alpha=best; }
        else      { if(sc<best) best=sc; if(best<beta)  beta=best;  }
        if(beta<=alpha) break;          /* alpha-beta prune: rest can't matter */
    }
    return best;
}
static void ttt_ai_move(void){
    int best=-1000, mv=-1;
    for(int i=0;i<9;i++){
        if(g_ttt.b[i]) continue;
        g_ttt.b[i]=2; int sc=ttt_minimax(g_ttt.b,0,1,-1000,1000); g_ttt.b[i]=0;
        if(sc>best){ best=sc; mv=i; }
    }
    if(mv>=0) g_ttt.b[mv]=2;
}
static void ttt_check_end(void){
    int wn=ttt_winner(g_ttt.b);
    if(wn){ g_ttt.over=1; g_ttt.winner=wn; if(wn==1)g_ttt.pw++; else g_ttt.aw++; }
    else if(ttt_full(g_ttt.b)){ g_ttt.over=1; g_ttt.winner=0; g_ttt.draws++; }
}
static void ttt_reset(void){
    for(int i=0;i<9;i++) g_ttt.b[i]=0;
    g_ttt.over=0; g_ttt.winner=0; g_ttt.hover=-1;
}
static void ttt_geom(int cw,int ch,int *bx,int *by,int *cell){
    int top=28;
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int c=availw/3; int c2=availh/3; if(c2<c)c=c2; if(c<12)c=12;
    *cell=c; *bx=(cw-c*3)/2; *by=top+(availh-c*3)/2;
}
static void ttt_draw(wm_window *w, int cx, int cy, int cw, int ch){
    ttt_state *s=&g_ttt; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    UINT32 acc=wm_theme_color(WM_COL_ACCENT);
    fill_rect(cx,cy,cw,ch,bg);
    char hb[56]; int p=0;
    sb_puts(hb,sizeof hb,&p,"You "); sb_putn(hb,sizeof hb,&p,s->pw);
    sb_puts(hb,sizeof hb,&p,"  AI "); sb_putn(hb,sizeof hb,&p,s->aw);
    sb_puts(hb,sizeof hb,&p,"  Draw "); sb_putn(hb,sizeof hb,&p,s->draws);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    int bx,by,cell; ttt_geom(cw,ch,&bx,&by,&cell);
    int sbx=cx+bx, sby=cy+by, bs=cell*3;
    gfill(cx,cy,cw,ch,sbx,sby,bs,bs,wm_blend(bg,0x00000000u,60));
    for(int i=1;i<3;i++){
        gfill(cx,cy,cw,ch,sbx+i*cell-1,sby,2,bs,wm_blend(bg,fg,80));
        gfill(cx,cy,cw,ch,sbx,sby+i*cell-1,bs,2,wm_blend(bg,fg,80));
    }
    for(int i=0;i<9;i++){
        int col=i%3, row=i/3;
        int gx=sbx+col*cell, gy=sby+row*cell;
        if(!s->b[i] && s->hover==i && !s->over)
            gfill(cx,cy,cw,ch,gx+2,gy+2,cell-4,cell-4,wm_blend(bg,acc,40));
        if(s->b[i]==1){ /* X: 5x2 stamps at a wider step => half the fills */
            int m=cell/4, cxx=gx+cell/2, cyy=gy+cell/2;
            for(int d=-m;d<=m;d+=2){
                gfill(cx,cy,cw,ch,cxx+d-2,cyy+d,5,2,acc);
                gfill(cx,cy,cw,ch,cxx+d-2,cyy-d,5,2,acc);
            }
        } else if(s->b[i]==2){ /* O ring: scan a squared-distance annulus */
            int r=cell/3, cxx=gx+cell/2, cyy=gy+cell/2;
            int ro2=(r+1)*(r+1), ri2=(r-2)*(r-2);
            /* Batch contiguous in-annulus pixels per scanline into one gfill.
             * Each row has up to two runs (left/right arc); flush on exit/end. */
            for(int yy=-r-2;yy<=r+2;yy++){
                int xs=-1000;                       /* run start; -1000 = idle */
                for(int xx=-r-2;xx<=r+2;xx++){
                    int d2=xx*xx+yy*yy;
                    if(d2<=ro2 && d2>=ri2){ if(xs==-1000) xs=xx; }
                    else if(xs!=-1000){
                        gfill(cx,cy,cw,ch,cxx+xs,cyy+yy,xx-xs,1,0x00E0A040u);
                        xs=-1000;
                    }
                }
                if(xs!=-1000)                       /* run reaching row end */
                    gfill(cx,cy,cw,ch,cxx+xs,cyy+yy,(r+3)-xs,1,0x00E0A040u);
            }
        }
    }
    if(s->over){
        const char *m = s->winner==1?"YOU WIN!": s->winner==2?"AI WINS":"DRAW";
        UINT32 mc = s->winner==1?0x0060FF60u: s->winner==2?0x00FF6060u:fg;
        draw_string_center(cx+cw/2,sby+bs+6,m,mc,bg,1,2);
    } else {
        draw_string_center(cx+cw/2,sby+bs+8,"Your move (X) - click a cell",fg,bg,1,1);
    }
}
static int ttt_event(wm_window *w, const wm_event *ev){
    ttt_state *s=&g_ttt; (void)w;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    int bx,by,cell; ttt_geom(cw,ch,&bx,&by,&cell);
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode=='r'||ev->unicode=='R') ttt_reset();
            return 0;
        case WM_EV_MOUSE_MOVE: {
            s->hover=-1;
            int mx=ev->mx-bx, my=ev->my-by;
            if(mx>=0&&my>=0&&mx<cell*3&&my<cell*3) s->hover=(my/cell)*3+(mx/cell);
            return 0;
        }
        case WM_EV_MOUSE_DOWN: {
            if(s->over){ ttt_reset(); return 0; }
            int mx=ev->mx-bx, my=ev->my-by;
            if(mx<0||my<0||mx>=cell*3||my>=cell*3) return 0;
            int i=(my/cell)*3+(mx/cell);
            if(s->b[i]) return 0;
            g_ttt.b[i]=1; ttt_check_end();
            if(!s->over){ ttt_ai_move(); ttt_check_end(); }
            return 0;
        }
        default: return 0;
    }
}
void tool_games_ttt_open(void){
    if(g_ttt.win) return;
    ttt_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*35/100; if(ww<340)ww=340; if(ww>440)ww=440; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<380)wh=380; if(wh>500)wh=500; if(wh>H-40)wh=H-40;
    g_ttt.win=wm_open("Tic-Tac-Toe",ww,wh,ttt_draw,ttt_event,&g_ttt);
}

/* ================================================================== */
/*  4. 2048                                                            */
/* ================================================================== */
typedef struct {
    wm_window *win;
    int g[4][4];
    int score, best, over, won;
} g2048_state;
static g2048_state g_2048;

static void g2048_spawn(void){
    int empty[16], n=0;
    for(int y=0;y<4;y++)for(int x=0;x<4;x++) if(!g_2048.g[y][x]) empty[n++]=y*4+x;
    if(!n) return;
    int c=empty[rng_range(n)];
    g_2048.g[c/4][c%4] = (rng_range(10)==0)?4:2;
}
static void g2048_reset(void){
    rng_stir();
    for(int y=0;y<4;y++)for(int x=0;x<4;x++) g_2048.g[y][x]=0;
    g_2048.score=0; g_2048.over=0; g_2048.won=0;
    g2048_spawn(); g2048_spawn();
}
/* slide+merge one row toward index 0. returns 1 if changed. */
static int g2048_slide(int *r){
    int tmp[4], n=0, changed=0;
    for(int i=0;i<4;i++) if(r[i]) tmp[n++]=r[i];
    int out[4]={0,0,0,0}, o=0;
    for(int i=0;i<n;i++){
        if(i+1<n && tmp[i]==tmp[i+1]){
            int v=tmp[i]*2; out[o++]=v; g_2048.score+=v;
            if(v==2048) g_2048.won=1; i++;
        } else out[o++]=tmp[i];
    }
    for(int i=0;i<4;i++){ if(r[i]!=out[i]) changed=1; r[i]=out[i]; }
    return changed;
}
static int g2048_move(int dir){ /* 0 left 1 right 2 up 3 down */
    int changed=0;
    if(dir==0||dir==1){
        for(int y=0;y<4;y++){
            int row[4];
            for(int x=0;x<4;x++) row[x]= (dir==0)? g_2048.g[y][x] : g_2048.g[y][3-x];
            if(g2048_slide(row)) changed=1;
            for(int x=0;x<4;x++){ if(dir==0) g_2048.g[y][x]=row[x]; else g_2048.g[y][3-x]=row[x]; }
        }
    } else {
        for(int x=0;x<4;x++){
            int col[4];
            for(int y=0;y<4;y++) col[y]= (dir==2)? g_2048.g[y][x] : g_2048.g[3-y][x];
            if(g2048_slide(col)) changed=1;
            for(int y=0;y<4;y++){ if(dir==2) g_2048.g[y][x]=col[y]; else g_2048.g[3-y][x]=col[y]; }
        }
    }
    return changed;
}
static int g2048_can_move(void){
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){
        if(!g_2048.g[y][x]) return 1;
        if(x<3 && g_2048.g[y][x]==g_2048.g[y][x+1]) return 1;
        if(y<3 && g_2048.g[y][x]==g_2048.g[y+1][x]) return 1;
    }
    return 0;
}
static UINT32 g2048_tilecol(int v){
    switch(v){
        case 2:return 0x00EEE4DA; case 4:return 0x00EDE0C8; case 8:return 0x00F2B179;
        case 16:return 0x00F59563; case 32:return 0x00F67C5F; case 64:return 0x00F65E3B;
        case 128:return 0x00EDCF72; case 256:return 0x00EDCC61; case 512:return 0x00EDC850;
        case 1024:return 0x00EDC53F; case 2048:return 0x00EDC22E; default:return 0x003C3A32;
    }
}
static void g2048_draw(wm_window *w, int cx, int cy, int cw, int ch){
    g2048_state *s=&g_2048; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    fill_rect(cx,cy,cw,ch,bg);
    if(s->score>s->best) s->best=s->score;
    char hb[48]; int p=0; sb_puts(hb,sizeof hb,&p,"Score "); sb_putn(hb,sizeof hb,&p,s->score);
    sb_puts(hb,sizeof hb,&p,"  Best "); sb_putn(hb,sizeof hb,&p,s->best);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    int top=28;
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int bs=availw<availh?availw:availh; int cell=bs/4; bs=cell*4;
    int bx=cx+(cw-bs)/2, by=cy+top+(availh-bs)/2;
    gfill(cx,cy,cw,ch,bx-4,by-4,bs+8,bs+8,0x00A89078);
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){
        int gx=bx+x*cell+3, gy=by+y*cell+3, cs=cell-6;
        int v=s->g[y][x];
        UINT32 tcol=v?g2048_tilecol(v):0x00BDA99B;
        gfill(cx,cy,cw,ch,gx,gy,cs,cs,tcol);
        if(v){
            char nb[8]; i32toa(v,nb);
            int sc = v>=1024?1:2;
            UINT32 tc = v<=4?0x00776E65u:0x00F9F6F2u;
            draw_string_center(gx+cs/2,gy+cs/2-8*sc,nb,tc,tcol,1,sc);
        }
    }
    if(s->over){
        gfill(cx,cy,cw,ch,bx,by+bs/2-18,bs,36,wm_blend(bg,0x00000000u,150));
        draw_string_center(cx+cw/2,by+bs/2-12,s->won?"YOU WIN!":"GAME OVER",
                           s->won?0x0060FF60u:0x00FF8060u,bg,1,2);
        draw_string_center(cx+cw/2,by+bs/2+12,"R to restart",fg,bg,1,1);
    } else {
        draw_string_center(cx+cw/2,by+bs+6,"Arrows / WASD",fg,bg,1,1);
    }
}
static int g2048_event(wm_window *w, const wm_event *ev){
    g2048_state *s=&g_2048; (void)w;
    if(ev->type==WM_EV_CLOSE){ s->win=NULL; return 0; }
    if(ev->type!=WM_EV_KEY) return 0;
    UINT16 sc=ev->scancode; CHAR16 u=ev->unicode;
    if(sc==SCAN_ESC) return WM_CLOSE_REQUEST;
    if(u=='r'||u=='R'){ g2048_reset(); return 0; }
    if(s->over) return 0;
    int dir=-1;
    if(sc==SCAN_LEFT||u=='a'||u=='A') dir=0;
    else if(sc==SCAN_RIGHT||u=='d'||u=='D') dir=1;
    else if(sc==SCAN_UP||u=='w'||u=='W') dir=2;
    else if(sc==SCAN_DOWN||u=='s'||u=='S') dir=3;
    if(dir<0) return 0;
    if(g2048_move(dir)){ g2048_spawn(); if(!g2048_can_move()) s->over=1; }
    return 0;
}
void tool_games_2048_open(void){
    if(g_2048.win) return;
    g2048_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*35/100; if(ww<360)ww=360; if(ww>460)ww=460; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<400)wh=400; if(wh>520)wh=520; if(wh>H-40)wh=H-40;
    g_2048.win=wm_open("2048",ww,wh,g2048_draw,g2048_event,&g_2048);
}

/* ================================================================== */
/*  5. MINESWEEPER (9x9, 10 mines)                                     */
/* ================================================================== */
#define MS_W 9
#define MS_H 9
#define MS_N (MS_W*MS_H)
#define MS_MINES 10
typedef struct {
    wm_window *win;
    unsigned char mine[MS_N], open[MS_N], flag[MS_N];
    signed char adj[MS_N];
    int started, lost, won, hover;
} mines_state;
static mines_state g_mines;

static void mines_reset(void){
    for(int i=0;i<MS_N;i++){ g_mines.mine[i]=g_mines.open[i]=g_mines.flag[i]=0; g_mines.adj[i]=0; }
    g_mines.started=0; g_mines.lost=0; g_mines.won=0; g_mines.hover=-1;
}
static void mines_plant(int safe){
    rng_stir();
    int placed=0;
    while(placed<MS_MINES){
        int c=rng_range(MS_N);
        if(c==safe || g_mines.mine[c]) continue;
        g_mines.mine[c]=1; placed++;
    }
    for(int y=0;y<MS_H;y++)for(int x=0;x<MS_W;x++){
        int c=y*MS_W+x; if(g_mines.mine[c]){ g_mines.adj[c]=-1; continue; }
        int n=0;
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
            int nx=x+dx, ny=y+dy;
            if(nx>=0&&nx<MS_W&&ny>=0&&ny<MS_H && g_mines.mine[ny*MS_W+nx]) n++;
        }
        g_mines.adj[c]=(signed char)n;
    }
    g_mines.started=1;
}
static void mines_flood(int c){
    if(g_mines.open[c]||g_mines.flag[c]) return;
    g_mines.open[c]=1;
    if(g_mines.adj[c]!=0) return;
    int x=c%MS_W, y=c/MS_W;
    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
        int nx=x+dx, ny=y+dy;
        if(nx>=0&&nx<MS_W&&ny>=0&&ny<MS_H) mines_flood(ny*MS_W+nx);
    }
}
static void mines_check_win(void){
    int closed=0;
    for(int i=0;i<MS_N;i++) if(!g_mines.open[i] && !g_mines.mine[i]) closed++;
    if(closed==0){ g_mines.won=1; }
}
static void mines_reveal(int c){
    if(g_mines.lost||g_mines.won||g_mines.flag[c]||g_mines.open[c]) return;
    if(!g_mines.started) mines_plant(c);
    if(g_mines.mine[c]){
        g_mines.lost=1;
        for(int i=0;i<MS_N;i++) if(g_mines.mine[i]) g_mines.open[i]=1;
        return;
    }
    mines_flood(c);
    mines_check_win();
}
static void mines_geom(int cw,int ch,int *bx,int *by,int *cell){
    int top=28;
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int c=availw/MS_W; int c2=availh/MS_H; if(c2<c)c=c2; if(c<8)c=8;
    *cell=c; *bx=(cw-c*MS_W)/2; *by=top+(availh-c*MS_H)/2;
}
static void mines_draw(wm_window *w, int cx, int cy, int cw, int ch){
    mines_state *s=&g_mines; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    fill_rect(cx,cy,cw,ch,bg);
    static const UINT32 numcol[9]={0,0x005B9BFFu,0x0060C060u,0x00FF6060u,0x00C070FFu,
                                   0x00E0A040u,0x0040D0D0u,0x00E0E0E0u,0x00A0A0A0u};
    int flags=0; for(int i=0;i<MS_N;i++) if(s->flag[i]) flags++;
    char hb[40]; int p=0; sb_puts(hb,sizeof hb,&p,"Mines "); sb_putn(hb,sizeof hb,&p,MS_MINES-flags);
    sb_puts(hb,sizeof hb,&p,s->lost?"  BOOM":s->won?"  CLEARED!":"");
    draw_string_clip(cx+6,cy+5,cw-12,hb,s->lost?0x00FF6060u:s->won?0x0060FF60u:fg,bg,1,1);
    int bx,by,cell; mines_geom(cw,ch,&bx,&by,&cell);
    int sbx=cx+bx, sby=cy+by;
    UINT32 open_face=wm_blend(bg,0x00FFFFFFu,30);
    UINT32 hover_face=wm_blend(bg,fg,70), closed_face=wm_blend(bg,fg,40);
    UINT32 grid_col=wm_blend(bg,0x00000000u,60);
    for(int i=0;i<MS_N;i++){
        int gx=sbx+(i%MS_W)*cell, gy=sby+(i/MS_W)*cell;
        UINT32 face;
        if(s->open[i]){
            if(s->mine[i]) face=0x00C03030u;
            else face=open_face;
        } else {
            face = (s->hover==i && !s->lost && !s->won) ? hover_face : closed_face;
        }
        gfill(cx,cy,cw,ch,gx+1,gy+1,cell-2,cell-2,face);
        goutline(cx,cy,cw,ch,gx,gy,cell,cell,grid_col);
        if(s->open[i]){
            if(s->mine[i]) gfill(cx,cy,cw,ch,gx+cell/2-3,gy+cell/2-3,6,6,0x00101010u);
            else if(s->adj[i]>0){ char n[2]={(char)('0'+s->adj[i]),0};
                draw_string_center(gx+cell/2,gy+cell/2-8,n,numcol[(int)s->adj[i]],face,1,1); }
        } else if(s->flag[i]){
            gfill(cx,cy,cw,ch,gx+cell/2-1,gy+3,2,cell-6,0x00A0A0A0u);
            gfill(cx,cy,cw,ch,gx+cell/2,gy+3,cell/3,cell/4,0x00E04040u);
        }
    }
    draw_string_center(cx+cw/2,sby+cell*MS_H+6,
        (s->lost||s->won)?"R restart":"L=open  R=flag",fg,bg,1,1);
}
static int mines_event(wm_window *w, const wm_event *ev){
    mines_state *s=&g_mines; (void)w;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    int bx,by,cell; mines_geom(cw,ch,&bx,&by,&cell);
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode=='r'||ev->unicode=='R') mines_reset();
            return 0;
        case WM_EV_MOUSE_MOVE: {
            s->hover=-1; int mx=ev->mx-bx, my=ev->my-by;
            if(mx>=0&&my>=0&&mx<cell*MS_W&&my<cell*MS_H) s->hover=(my/cell)*MS_W+(mx/cell);
            return 0;
        }
        case WM_EV_MOUSE_DOWN: {
            if(s->lost||s->won){ mines_reset(); return 0; }
            int mx=ev->mx-bx, my=ev->my-by;
            if(mx<0||my<0||mx>=cell*MS_W||my>=cell*MS_H) return 0;
            int c=(my/cell)*MS_W+(mx/cell);
            if(ev->button==1){ if(!s->open[c]) s->flag[c]=!s->flag[c]; }
            else mines_reveal(c);
            return 0;
        }
        default: return 0;
    }
}
void tool_games_mines_open(void){
    if(g_mines.win) return;
    mines_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*38/100; if(ww<360)ww=360; if(ww>460)ww=460; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<400)wh=400; if(wh>500)wh=500; if(wh>H-40)wh=H-40;
    g_mines.win=wm_open("Minesweeper",ww,wh,mines_draw,mines_event,&g_mines);
}

/* ================================================================== */
/*  6. BREAKOUT                                                        */
/* ================================================================== */
#define BO_COLS 10
#define BO_ROWS 6
typedef struct {
    wm_window *win;
    unsigned char brick[BO_ROWS*BO_COLS];
    int padx, padw;
    int bx, by, vx, vy;
    int launched, lives, score, over, won;
    int mouse_x, use_mouse, key_l, key_r;
    int bricks_left;
} bo_state;
static bo_state g_bo;

static void bo_reset(void){
    rng_stir();
    for(int i=0;i<BO_ROWS*BO_COLS;i++) g_bo.brick[i]=1;
    g_bo.bricks_left=BO_ROWS*BO_COLS;
    g_bo.padw=70; g_bo.padx=-1; g_bo.launched=0; g_bo.lives=3;
    g_bo.score=0; g_bo.over=0; g_bo.won=0; g_bo.use_mouse=0;
    g_bo.bx=-1;
}
static void bo_draw(wm_window *w, int cx, int cy, int cw, int ch){
    bo_state *s=&g_bo; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    UINT32 acc=wm_theme_color(WM_COL_ACCENT);
    fill_rect(cx,cy,cw,ch,bg);
    int top=26;
    int fx=GMARGIN, fy=top, fw=cw-2*GMARGIN, fh=ch-top-GMARGIN;
    if(fw<60)fw=60; if(fh<80)fh=80;
    if(s->padx<0) s->padx=fx+fw/2;
    int pady=fy+fh-14, padh=8;
    /* brick layout */
    int bgap=3;
    int brw=(fw-(BO_COLS+1)*bgap)/BO_COLS; if(brw<4)brw=4;
    int brh=14;
    int bregion_top=fy+8;
    if(s->bx<0){ s->bx=s->padx; s->by=pady-6; s->vx=0; s->vy=0; }
    /* simulate */
    if(!s->over){
        if(s->use_mouse) s->padx=s->mouse_x;
        else { if(s->key_l) s->padx-=8; if(s->key_r) s->padx+=8; }
        if(s->padx<fx+s->padw/2) s->padx=fx+s->padw/2;
        if(s->padx>fx+fw-s->padw/2) s->padx=fx+fw-s->padw/2;
        if(!s->launched){ s->bx=s->padx; s->by=pady-6; }
        else {
            s->bx+=s->vx; s->by+=s->vy;
            if(s->bx<fx+4){ s->bx=fx+4; s->vx=-s->vx; }
            if(s->bx>fx+fw-4){ s->bx=fx+fw-4; s->vx=-s->vx; }
            if(s->by<fy+4){ s->by=fy+4; s->vy=-s->vy; }
            /* paddle */
            if(s->vy>0 && s->by>=pady-4 && s->by<=pady+padh &&
               s->bx>=s->padx-s->padw/2-3 && s->bx<=s->padx+s->padw/2+3){
                s->by=pady-4; s->vy=-s->vy;
                s->vx += (s->bx - s->padx)/8;
                if(s->vx>6)s->vx=6; if(s->vx<-6)s->vx=-6;
                if(s->vx==0) s->vx=(rng_range(2)?1:-1);
            }
            /* bricks */
            for(int r=0;r<BO_ROWS;r++)for(int c=0;c<BO_COLS;c++){
                int idx=r*BO_COLS+c; if(!s->brick[idx]) continue;
                int rx=fx+bgap+c*(brw+bgap), ry=bregion_top+r*(brh+bgap);
                if(s->bx>=rx-3 && s->bx<=rx+brw+3 && s->by>=ry-3 && s->by<=ry+brh+3){
                    s->brick[idx]=0; s->bricks_left--; s->score+=10; s->vy=-s->vy;
                    r=BO_ROWS; break;
                }
            }
            /* lost ball */
            if(s->by>fy+fh){ s->lives--; s->launched=0; s->vx=0; s->vy=0;
                             if(s->lives<=0){ s->over=1; } }
            /* win */
            if(s->bricks_left==0){ s->won=1; s->over=1; }
        }
    }
    /* draw header */
    char hb[48]; int p=0; sb_puts(hb,sizeof hb,&p,"Score "); sb_putn(hb,sizeof hb,&p,s->score);
    sb_puts(hb,sizeof hb,&p,"  Lives "); sb_putn(hb,sizeof hb,&p,s->lives>0?s->lives:0);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    goutline(cx,cy,cw,ch,cx+fx,cy+fy,fw,fh,wm_blend(bg,fg,50));
    /* bricks */
    static const UINT32 rowcol[BO_ROWS]={0x00E24A4Au,0x00E0894Au,0x00E0C84Au,
                                         0x0060C060u,0x005B9BFFu,0x00B060E0u};
    for(int r=0;r<BO_ROWS;r++)for(int c=0;c<BO_COLS;c++){
        if(!s->brick[r*BO_COLS+c]) continue;
        int rx=cx+fx+bgap+c*(brw+bgap), ry=cy+bregion_top+r*(brh+bgap);
        gfill(cx,cy,cw,ch,rx,ry,brw,brh,rowcol[r]);
    }
    /* paddle + ball */
    gfill(cx,cy,cw,ch,cx+s->padx-s->padw/2,cy+pady,s->padw,padh,acc);
    gfill(cx,cy,cw,ch,cx+s->bx-4,cy+s->by-4,8,8,fg);
    if(!s->launched && !s->over)
        draw_string_center(cx+cw/2,cy+fy+fh/2,"Space/click to launch",fg,bg,1,1);
    if(s->over){
        draw_string_center(cx+cw/2,cy+fy+fh/2-8,s->won?"YOU WIN!":"GAME OVER",
                           s->won?0x0060FF60u:0x00FF6060u,bg,1,2);
        draw_string_center(cx+cw/2,cy+fy+fh/2+16,"R to restart",fg,bg,1,1);
    }
}
static void bo_launch(void){
    if(g_bo.over||g_bo.launched) return;
    g_bo.launched=1; g_bo.vy=-5; g_bo.vx=(rng_range(2)?2:-2);
}
static int bo_event(wm_window *w, const wm_event *ev){
    bo_state *s=&g_bo; (void)w;
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode=='r'||ev->unicode=='R'){ bo_reset(); return 0; }
            if(ev->unicode==' ') bo_launch();
            if(ev->scancode==SCAN_LEFT||ev->unicode=='a'||ev->unicode=='A'){ s->use_mouse=0; s->padx-=22; }
            if(ev->scancode==SCAN_RIGHT||ev->unicode=='d'||ev->unicode=='D'){ s->use_mouse=0; s->padx+=22; }
            return 0;
        case WM_EV_MOUSE_MOVE: s->use_mouse=1; s->mouse_x=ev->mx; return 0;
        case WM_EV_MOUSE_DOWN:
            if(s->over) bo_reset(); else bo_launch();
            return 0;
        default: return 0;
    }
}
void tool_games_breakout_open(void){
    if(g_bo.win) return;
    bo_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*45/100; if(ww<440)ww=440; if(ww>620)ww=620; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<400)wh=400; if(wh>560)wh=560; if(wh>H-40)wh=H-40;
    g_bo.win=wm_open("Breakout",ww,wh,bo_draw,bo_event,&g_bo);
}

/* ================================================================== */
/*  7. CONWAY'S GAME OF LIFE                                           */
/* ================================================================== */
#define LF_W 44
#define LF_H 30
typedef struct {
    wm_window *win;
    unsigned char cell[LF_W*LF_H];
    int running, gen;
    unsigned tick;
    int speed;
    int paint_val, painting;
    int pop;           /* running live-cell count (kept in sync w/ cell[]) */
} life_state;
static life_state g_life;

static void life_clear(void){
    for(int i=0;i<LF_W*LF_H;i++) g_life.cell[i]=0;
    g_life.gen=0; g_life.running=0; g_life.pop=0;
}
static void life_random(void){
    rng_stir();
    int pop=0;
    for(int i=0;i<LF_W*LF_H;i++){ int v=(rng_range(100)<28)?1:0; g_life.cell[i]=(unsigned char)v; pop+=v; }
    g_life.gen=0; g_life.pop=pop;
}
static void life_reset(void){
    life_clear();
    g_life.speed=4; g_life.tick=0; g_life.painting=0; g_life.paint_val=1;
    /* seed a glider for a nice default */
    int cxg=5, cyg=5;
    int gl[5][2]={{1,0},{2,1},{0,2},{1,2},{2,2}};
    for(int i=0;i<5;i++) g_life.cell[(cyg+gl[i][1])*LF_W+(cxg+gl[i][0])]=1;
    g_life.pop=5;   /* glider has 5 live cells, board just cleared */
}
static void life_step(void){
    static unsigned char nxt[LF_W*LF_H];
    for(int y=0;y<LF_H;y++)for(int x=0;x<LF_W;x++){
        int n=0;
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
            if(!dx&&!dy) continue;
            int nx=x+dx, ny=y+dy;
            if(nx>=0&&nx<LF_W&&ny>=0&&ny<LF_H && g_life.cell[ny*LF_W+nx]) n++;
        }
        int a=g_life.cell[y*LF_W+x];
        nxt[y*LF_W+x] = (a && (n==2||n==3)) || (!a && n==3);
    }
    int pop=0;
    for(int i=0;i<LF_W*LF_H;i++){ g_life.cell[i]=nxt[i]; pop+=nxt[i]; }
    g_life.pop=pop;
    g_life.gen++;
}
static void life_geom(int cw,int ch,int *bx,int *by,int *cell){
    int top=28;
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int c=availw/LF_W; int c2=availh/LF_H; if(c2<c)c=c2; if(c<2)c=2;
    *cell=c; *bx=(cw-c*LF_W)/2; *by=top+(availh-c*LF_H)/2;
}
static void life_draw(wm_window *w, int cx, int cy, int cw, int ch){
    life_state *s=&g_life; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    UINT32 acc=wm_theme_color(WM_COL_ACCENT);
    fill_rect(cx,cy,cw,ch,bg);
    s->tick++;
    if(s->running && (s->tick%(unsigned)s->speed)==0) life_step();
    int pop=s->pop;
    char hb[64]; int p=0;
    sb_puts(hb,sizeof hb,&p,s->running?"RUN  ":"PAUSE ");
    sb_puts(hb,sizeof hb,&p,"Gen "); sb_putn(hb,sizeof hb,&p,s->gen);
    sb_puts(hb,sizeof hb,&p,"  Pop "); sb_putn(hb,sizeof hb,&p,pop);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    int bx,by,cell; life_geom(cw,ch,&bx,&by,&cell);
    int sbx=cx+bx, sby=cy+by, bw=cell*LF_W, bh=cell*LF_H;
    gfill(cx,cy,cw,ch,sbx,sby,bw,bh,wm_blend(bg,0x00000000u,90));
    int cd = cell>1?cell-1:1;
    for(int y=0;y<LF_H;y++)for(int x=0;x<LF_W;x++){
        if(s->cell[y*LF_W+x])
            gfill(cx,cy,cw,ch,sbx+x*cell,sby+y*cell,cd,cd,acc);
    }
    goutline(cx,cy,cw,ch,sbx-1,sby-1,bw+2,bh+2,wm_blend(bg,fg,50));
    draw_string_center(cx+cw/2,sby+bh+5,"click=draw  Space=run  N=step  R=rand  C=clear",fg,bg,1,1);
}
static void life_toggle_at(int mx,int my,int cw,int ch,int set){
    int bx,by,cell; life_geom(cw,ch,&bx,&by,&cell);
    int gx=mx-bx, gy=my-by;
    if(gx<0||gy<0||gx>=cell*LF_W||gy>=cell*LF_H) return;
    int i=(gy/cell)*LF_W+(gx/cell);
    int old=g_life.cell[i];
    int nv = set<0 ? !old : (set?1:0);
    g_life.cell[i]=(unsigned char)nv; g_life.pop += nv-old;
}
static int life_event(wm_window *w, const wm_event *ev){
    life_state *s=&g_life; (void)w;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==' ') s->running=!s->running;
            else if(ev->unicode=='n'||ev->unicode=='N'){ s->running=0; life_step(); }
            else if(ev->unicode=='r'||ev->unicode=='R') life_random();
            else if(ev->unicode=='c'||ev->unicode=='C') life_clear();
            else if(ev->unicode=='+'){ if(s->speed>1) s->speed--; }
            else if(ev->unicode=='-'){ if(s->speed<30) s->speed++; }
            return 0;
        case WM_EV_MOUSE_DOWN: {
            /* toggle target cell; remember its NEW value so a drag paints it */
            int bx,by,cell; life_geom(cw,ch,&bx,&by,&cell);
            int gx=ev->mx-bx, gy=ev->my-by;
            if(gx>=0&&gy>=0&&gx<cell*LF_W&&gy<cell*LF_H){
                int i=(gy/cell)*LF_W+(gx/cell);
                int old=s->cell[i];
                s->cell[i]=!old; s->pop += s->cell[i]-old;
                s->paint_val=s->cell[i]; s->painting=1;
            }
            return 0;
        }
        case WM_EV_MOUSE_MOVE:
            if(s->painting) life_toggle_at(ev->mx,ev->my,cw,ch,s->paint_val);
            return 0;
        case WM_EV_MOUSE_UP: s->painting=0; return 0;
        default: return 0;
    }
}
void tool_games_life_open(void){
    if(g_life.win) return;
    life_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*50/100; if(ww<480)ww=480; if(ww>680)ww=680; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<400)wh=400; if(wh>560)wh=560; if(wh>H-40)wh=H-40;
    g_life.win=wm_open("Game of Life",ww,wh,life_draw,life_event,&g_life);
}

/* ================================================================== */
/*  8. SIMON (memory game)                                             */
/* ================================================================== */
#define SIM_MAX 64
enum { SIM_IDLE=0, SIM_SHOW, SIM_INPUT, SIM_OVER };
typedef struct {
    wm_window *win;
    unsigned char seq[SIM_MAX];
    int len;              /* sequence length this round      */
    int state;
    int show_i;           /* index being shown               */
    int in_i;             /* player input index              */
    int lit;              /* pad currently lit (-1 none)     */
    unsigned tick, phase_start;
    int best;
    int flash_pad, flash_left;   /* transient player press flash */
} simon_state;
static simon_state g_simon;

static const UINT32 SIM_COL[4]   ={0x00E24A4Au,0x0060C060u,0x005B9BFFu,0x00E0C84Au};
static const UINT32 SIM_COLLIT[4]={0x00FF9090u,0x00A0FFA0u,0x00A0D0FFu,0x00FFF0A0u};

static void simon_new(void){
    rng_stir();
    g_simon.len=0; g_simon.state=SIM_IDLE; g_simon.lit=-1;
    g_simon.show_i=0; g_simon.in_i=0; g_simon.tick=0; g_simon.flash_left=0;
}
static void simon_next_round(void){
    if(g_simon.len<SIM_MAX) g_simon.seq[g_simon.len++]=(unsigned char)rng_range(4);
    g_simon.state=SIM_SHOW; g_simon.show_i=0; g_simon.lit=-1;
    g_simon.phase_start=g_simon.tick;
}
static void simon_pad_rect(int cw,int ch,int pad,int *x,int *y,int *s){
    int top=30;
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int side=availw<availh?availw:availh; int cell=side/2;
    int ox=(cw-cell*2)/2, oy=top+(availh-cell*2)/2;
    *x=ox+(pad%2)*cell; *y=oy+(pad/2)*cell; *s=cell;
}
static void simon_draw(wm_window *w, int cx, int cy, int cw, int ch){
    simon_state *s=&g_simon; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    fill_rect(cx,cy,cw,ch,bg);
    s->tick++;
    /* animate the SHOW phase */
    if(s->state==SIM_SHOW){
        unsigned el=s->tick - s->phase_start;
        unsigned onT=22, offT=12, per=onT+offT;
        unsigned idx=el/per, ph=el%per;
        if((int)idx>=s->len){ s->state=SIM_INPUT; s->in_i=0; s->lit=-1; }
        else { s->lit = (ph<onT) ? s->seq[idx] : -1; }
    }
    if(s->flash_left>0){ s->flash_left--; if(s->flash_left==0) s->flash_pad=-1; }
    char hb[48]; int p=0;
    sb_puts(hb,sizeof hb,&p,"Round "); sb_putn(hb,sizeof hb,&p,s->len);
    sb_puts(hb,sizeof hb,&p,"  Best "); sb_putn(hb,sizeof hb,&p,s->best);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    for(int pad=0;pad<4;pad++){
        int x,y,cs; simon_pad_rect(cw,ch,pad,&x,&y,&cs);
        int on = (s->lit==pad) || (s->flash_pad==pad && s->flash_left>0);
        gfill(cx,cy,cw,ch,cx+x+4,cy+y+4,cs-8,cs-8,on?SIM_COLLIT[pad]:SIM_COL[pad]);
        goutline(cx,cy,cw,ch,cx+x+4,cy+y+4,cs-8,cs-8,wm_blend(bg,0x00000000u,80));
    }
    const char *msg = s->state==SIM_SHOW?"Watch...":
                      s->state==SIM_INPUT?"Repeat it!":
                      s->state==SIM_OVER?"Wrong! Space=retry":"Space to start";
    draw_string_center(cx+cw/2,cy+ch-18,msg,s->state==SIM_OVER?0x00FF6060u:fg,bg,1,1);
}
static void simon_press(int pad){
    simon_state *s=&g_simon;
    if(s->state!=SIM_INPUT) return;
    s->flash_pad=pad; s->flash_left=8;
    if(pad==s->seq[s->in_i]){
        s->in_i++;
        if(s->in_i>=s->len){
            if(s->len>s->best) s->best=s->len;
            s->state=SIM_SHOW; /* brief pause then next */
            /* extend after showing: schedule next round */
            simon_next_round();
        }
    } else {
        s->state=SIM_OVER;
    }
}
static int simon_event(wm_window *w, const wm_event *ev){
    simon_state *s=&g_simon; (void)w;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==' '){
                if(s->state==SIM_IDLE || s->state==SIM_OVER){ simon_new(); simon_next_round(); }
            }
            /* keys 1-4 also play pads */
            if(ev->unicode>='1'&&ev->unicode<='4') simon_press((int)(ev->unicode-'1'));
            return 0;
        case WM_EV_MOUSE_DOWN: {
            if(s->state==SIM_IDLE||s->state==SIM_OVER){ simon_new(); simon_next_round(); return 0; }
            for(int pad=0;pad<4;pad++){
                int x,y,cs; simon_pad_rect(cw,ch,pad,&x,&y,&cs);
                if(ev->mx>=x+4&&ev->mx<x+cs-4&&ev->my>=y+4&&ev->my<y+cs-4){ simon_press(pad); break; }
            }
            return 0;
        }
        default: return 0;
    }
}
void tool_games_simon_open(void){
    if(g_simon.win) return;
    simon_new(); g_simon.flash_pad=-1;
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*35/100; if(ww<340)ww=340; if(ww>440)ww=440; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<380)wh=380; if(wh>500)wh=500; if(wh>H-40)wh=H-40;
    g_simon.win=wm_open("Simon",ww,wh,simon_draw,simon_event,&g_simon);
}

/* ================================================================== */
/*  9. DICE ROLLER (animated)                                          */
/* ================================================================== */
typedef struct {
    wm_window *win;
    int ndice;            /* 1..5                            */
    int val[5];
    int rolling;
    unsigned roll_end, tick;
} dice_state;
static dice_state g_dice;

static void dice_reset(void){
    rng_stir();
    g_dice.ndice=2; for(int i=0;i<5;i++) g_dice.val[i]=1+rng_range(6);
    g_dice.rolling=0; g_dice.tick=0;
}
static void dice_roll(void){
    rng_stir();
    g_dice.rolling=1; g_dice.roll_end=g_dice.tick+30;
}
/* draw a die face with pips into rect (screen coords, clipped) */
static void dice_face(int cx,int cy,int cw,int ch,int x,int y,int sz,int v){
    gfill(cx,cy,cw,ch,x,y,sz,sz,0x00F0F0F0u);
    goutline(cx,cy,cw,ch,x,y,sz,sz,0x00303030u);
    int r=sz/10; if(r<2)r=2;
    int a=x+sz/4, b=x+sz/2, c=x+3*sz/4;
    int d=y+sz/4, e=y+sz/2, f=y+3*sz/4;
    UINT32 pc=0x00202020u;
    #define PIP(px,py) gfill(cx,cy,cw,ch,(px)-r,(py)-r,2*r,2*r,pc)
    if(v==1||v==3||v==5) PIP(b,e);
    if(v>=2){ PIP(a,d); PIP(c,f); }
    if(v>=4){ PIP(c,d); PIP(a,f); }
    if(v==6){ PIP(a,e); PIP(c,e); }
    #undef PIP
}
static void dice_draw(wm_window *w, int cx, int cy, int cw, int ch){
    dice_state *s=&g_dice; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    fill_rect(cx,cy,cw,ch,bg);
    s->tick++;
    if(s->rolling){
        for(int i=0;i<s->ndice;i++) s->val[i]=1+rng_range(6);
        if(s->tick>=s->roll_end) s->rolling=0;
    }
    int total=0; for(int i=0;i<s->ndice;i++) total+=s->val[i];
    char hb[48]; int p=0; sb_puts(hb,sizeof hb,&p,"Dice "); sb_putn(hb,sizeof hb,&p,s->ndice);
    sb_puts(hb,sizeof hb,&p,"  Total "); sb_putn(hb,sizeof hb,&p,total);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    /* layout dice in a row, wrap if needed */
    int top=30, availw=cw-2*GMARGIN, availh=ch-top-40;
    int per = s->ndice<=3?s->ndice:3;
    int rows=(s->ndice+per-1)/per;
    int sz = availw/per - 10; int sz2 = availh/rows - 10; if(sz2<sz)sz=sz2;
    if(sz<24)sz=24; if(sz>110)sz=110;
    int gap=12;
    for(int i=0;i<s->ndice;i++){
        int row=i/per, col=i%per;
        int thisper = (row==rows-1)? (s->ndice-row*per):per;
        int trw=thisper*sz+(thisper-1)*gap;
        int x0=cx+(cw-trw)/2;
        int x=x0+col*(sz+gap);
        int y=cy+top+(availh-rows*sz-(rows-1)*gap)/2 + row*(sz+gap);
        dice_face(cx,cy,cw,ch,x,y,sz,s->val[i]);
    }
    draw_string_center(cx+cw/2,cy+ch-20,
        s->rolling?"rolling...":"Space/click=roll  1-5=count",fg,bg,1,1);
}
static int dice_event(wm_window *w, const wm_event *ev){
    dice_state *s=&g_dice; (void)w;
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==' '||ev->unicode=='r'||ev->unicode=='R'){ dice_roll(); return 0; }
            if(ev->unicode>='1'&&ev->unicode<='5'){ s->ndice=(int)(ev->unicode-'0'); dice_roll(); }
            return 0;
        case WM_EV_MOUSE_DOWN: dice_roll(); return 0;
        default: return 0;
    }
}
void tool_games_dice_open(void){
    if(g_dice.win) return;
    dice_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*40/100; if(ww<380)ww=380; if(ww>520)ww=520; if(ww>W-40)ww=W-40;
    int wh=H*45/100; if(wh<300)wh=300; if(wh>420)wh=420; if(wh>H-40)wh=H-40;
    g_dice.win=wm_open("Dice Roller",ww,wh,dice_draw,dice_event,&g_dice);
}

/* ================================================================== */
/*  10. WHACK / REACTION TIMER                                         */
/* ================================================================== */
#define WK_W 3
#define WK_H 3
#define WK_N (WK_W*WK_H)
enum { WK_IDLE=0, WK_WAIT, WK_UP, WK_DONE };
typedef struct {
    wm_window *win;
    int state;
    int mole;             /* active hole (-1 none)           */
    unsigned tick, next_at, up_since;
    int score, misses, hits;
    int last_react, best_react;   /* frames                  */
    int rounds, max_rounds;
    int hover;
} whack_state;
static whack_state g_whack;

static void whack_reset(void){
    rng_stir();
    g_whack.state=WK_IDLE; g_whack.mole=-1; g_whack.tick=0;
    g_whack.score=0; g_whack.misses=0; g_whack.hits=0;
    g_whack.last_react=0; g_whack.best_react=0;
    g_whack.rounds=0; g_whack.max_rounds=20; g_whack.hover=-1;
}
static void whack_arm(void){
    /* schedule next mole after a random delay */
    g_whack.state=WK_WAIT;
    g_whack.mole=-1;
    g_whack.next_at=g_whack.tick + 30 + (unsigned)rng_range(90);
}
static void whack_start(void){
    whack_reset();
    whack_arm();
}
static void whack_geom(int cw,int ch,int *bx,int *by,int *cell){
    int top=44;
    int availw=cw-2*GMARGIN, availh=ch-top-GMARGIN;
    int c=availw/WK_W; int c2=availh/WK_H; if(c2<c)c=c2; if(c<16)c=16;
    *cell=c; *bx=(cw-c*WK_W)/2; *by=top+(availh-c*WK_H)/2;
}
static void whack_draw(wm_window *w, int cx, int cy, int cw, int ch){
    whack_state *s=&g_whack; (void)w;
    UINT32 bg=wm_theme_color(WM_COL_WINDOW), fg=wm_theme_color(WM_COL_FG);
    fill_rect(cx,cy,cw,ch,bg);
    s->tick++;
    /* state machine */
    if(s->state==WK_WAIT && s->tick>=s->next_at){
        s->mole=rng_range(WK_N); s->state=WK_UP; s->up_since=s->tick;
    } else if(s->state==WK_UP){
        if(s->tick - s->up_since > 55){  /* missed: mole hid */
            s->misses++; s->rounds++;
            if(s->rounds>=s->max_rounds) s->state=WK_DONE; else whack_arm();
        }
    }
    /* header */
    char hb[56]; int p=0;
    sb_puts(hb,sizeof hb,&p,"Hits "); sb_putn(hb,sizeof hb,&p,s->hits);
    sb_puts(hb,sizeof hb,&p,"  Miss "); sb_putn(hb,sizeof hb,&p,s->misses);
    sb_puts(hb,sizeof hb,&p,"  Rd "); sb_putn(hb,sizeof hb,&p,s->rounds);
    sb_puts(hb,sizeof hb,&p,"/"); sb_putn(hb,sizeof hb,&p,s->max_rounds);
    draw_string_clip(cx+6,cy+5,cw-12,hb,fg,bg,1,1);
    char rb[48]; p=0;
    sb_puts(rb,sizeof rb,&p,"React "); sb_putn(rb,sizeof rb,&p,s->last_react);
    sb_puts(rb,sizeof rb,&p,"f  Best "); sb_putn(rb,sizeof rb,&p,s->best_react);
    sb_puts(rb,sizeof rb,&p,"f");
    draw_string_clip(cx+6,cy+22,cw-12,rb,wm_blend(fg,bg,60),bg,1,1);
    int bx,by,cell; whack_geom(cw,ch,&bx,&by,&cell);
    UINT32 hole = wm_blend(bg,0x00000000u,110);
    UINT32 hole_outline = wm_blend(bg,fg,50);
    for(int i=0;i<WK_N;i++){
        int gx=cx+bx+(i%WK_W)*cell, gy=cy+by+(i/WK_W)*cell;
        int m=6, hs=cell-2*m;
        int isup = (s->state==WK_UP && s->mole==i);
        gfill(cx,cy,cw,ch,gx+m,gy+m,hs,hs,hole);
        goutline(cx,cy,cw,ch,gx+m,gy+m,hs,hs,hole_outline);
        if(isup){
            int mm=m+6, ms=cell-2*mm;
            gfill(cx,cy,cw,ch,gx+mm,gy+mm,ms,ms,0x0060C060u);
            /* eyes */
            gfill(cx,cy,cw,ch,gx+mm+ms/4,gy+mm+ms/3,3,3,0x00101010u);
            gfill(cx,cy,cw,ch,gx+mm+ms-ms/4-3,gy+mm+ms/3,3,3,0x00101010u);
        } else if(s->hover==i && s->state!=WK_DONE){
            goutline(cx,cy,cw,ch,gx+m,gy+m,hs,hs,wm_blend(bg,fg,120));
        }
    }
    if(s->state==WK_IDLE)
        draw_string_center(cx+cw/2,cy+ch-18,"Space/click to start",fg,bg,1,1);
    else if(s->state==WK_DONE){
        gfill(cx,cy,cw,ch,cx+bx,cy+by+cell*WK_H/2-16,cell*WK_W,32,wm_blend(bg,0x00000000u,150));
        char db[40]; p=0; sb_puts(db,sizeof db,&p,"DONE  Hits "); sb_putn(db,sizeof db,&p,s->hits);
        draw_string_center(cx+cw/2,cy+by+cell*WK_H/2-12,db,0x0060FF60u,bg,1,2);
        draw_string_center(cx+cw/2,cy+ch-18,"Space/R to play again",fg,bg,1,1);
    } else {
        draw_string_center(cx+cw/2,cy+ch-18,
            s->state==WK_WAIT?"get ready...":"WHACK IT!",fg,bg,1,1);
    }
}
static void whack_hit(int cell_i){
    whack_state *s=&g_whack;
    if(s->state==WK_UP && cell_i==s->mole){
        int r=(int)(s->tick - s->up_since);
        s->last_react=r; if(s->best_react==0||r<s->best_react) s->best_react=r;
        s->hits++; s->score+=10; s->rounds++;
        if(s->rounds>=s->max_rounds) s->state=WK_DONE; else whack_arm();
    } else if(s->state==WK_UP || s->state==WK_WAIT){
        /* wrong / early click: small penalty (miss) */
        s->misses++;
    }
}
static int whack_event(wm_window *w, const wm_event *ev){
    whack_state *s=&g_whack; (void)w;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    int bx,by,cell; whack_geom(cw,ch,&bx,&by,&cell);
    switch(ev->type){
        case WM_EV_CLOSE: s->win=NULL; return 0;
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->unicode==' '||ev->unicode=='r'||ev->unicode=='R'){
                if(s->state==WK_IDLE||s->state==WK_DONE) whack_start();
            }
            return 0;
        case WM_EV_MOUSE_MOVE: {
            s->hover=-1; int mx=ev->mx-bx, my=ev->my-by;
            if(mx>=0&&my>=0&&mx<cell*WK_W&&my<cell*WK_H) s->hover=(my/cell)*WK_W+(mx/cell);
            return 0;
        }
        case WM_EV_MOUSE_DOWN: {
            if(s->state==WK_IDLE||s->state==WK_DONE){ whack_start(); return 0; }
            int mx=ev->mx-bx, my=ev->my-by;
            if(mx<0||my<0||mx>=cell*WK_W||my>=cell*WK_H) return 0;
            whack_hit((my/cell)*WK_W+(mx/cell));
            return 0;
        }
        default: return 0;
    }
}
void tool_games_whack_open(void){
    if(g_whack.win) return;
    whack_reset();
    int W=(int)ui_width(),H=(int)ui_height();
    int ww=W*38/100; if(ww<360)ww=360; if(ww>460)ww=460; if(ww>W-40)ww=W-40;
    int wh=H*55/100; if(wh<400)wh=400; if(wh>520)wh=520; if(wh>H-40)wh=H-40;
    g_whack.win=wm_open("Whack-a-Mole",ww,wh,whack_draw,whack_event,&g_whack);
}

/* ================================================================== */
/*  Category registry                                                  */
/* ================================================================== */
const struct forebo_tool cat_games_tools[] = {
    { "Snake",        "Classic grow-the-snake (arrows/WASD)",     "terminal", tool_games_snake_open    },
    { "Pong",         "Paddle vs a simple AI",                    "terminal", tool_games_pong_open     },
    { "Tic-Tac-Toe",  "Beat the unbeatable minimax AI",           "terminal", tool_games_ttt_open      },
    { "2048",         "Slide + merge tiles to 2048",              "terminal", tool_games_2048_open     },
    { "Minesweeper",  "9x9 mines: L=open R=flag",                 "terminal", tool_games_mines_open    },
    { "Breakout",     "Bounce the ball, clear the bricks",        "terminal", tool_games_breakout_open },
    { "Game of Life", "Conway cells: draw, step, run",            "terminal", tool_games_life_open     },
    { "Simon",        "Repeat the growing color sequence",        "terminal", tool_games_simon_open    },
    { "Dice Roller",  "Animated 1-5 dice with pips",              "terminal", tool_games_dice_open     },
    { "Whack-a-Mole", "Reaction timer: whack the moles",          "terminal", tool_games_whack_open    },
};
const int cat_games_count = (int)(sizeof(cat_games_tools)/sizeof(cat_games_tools[0]));

/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools_math.c - "Math" tool category: number theory & integer calculators.
 * =============================================================================
 * Nine self-contained windowed calculators, all integer / fixed-point math
 * (freestanding C11, no libc, no heap, no float - built with -mno-sse). Each
 * shares one editable-field UI framework in this file (fixed static state per
 * tool kind): type digits into the focused field, Tab / arrows to move between
 * fields, [-1]/[+1] buttons or the mouse to nudge values, wheel / PageUp/Down to
 * scroll long results. Results recompute instantly on every edit.
 *
 * Tools: Prime checker (+next prime), Factorial, Fibonacci, GCD/LCM, integer
 * Quadratic solver, Prime factorizer, Modular power a^b mod m, Integer sqrt
 * (Newton) + perfect-square test, Pascal's triangle.
 * ========================================================================== */
#include "tools_math.h"
#include "ui.h"
#include "wm.h"
#include "input.h"
#include "efi.h"

#include <stdarg.h>

typedef UINT64 u64;
typedef INT64  i64;

/* ------------------------------------------------------------------ */
/*  Tiny freestanding string / format helpers (no libc)               */
/* ------------------------------------------------------------------ */
static int m_strlen(const char *s){ int n=0; while(s&&s[n]) n++; return n; }

static void m_strcpy(char *d, const char *s, int cap)
{
    int i=0; if(cap<=0) return;
    for(; s && s[i] && i<cap-1; i++) d[i]=s[i];
    d[i]=0;
}

/* Append unsigned decimal to buf[pos..cap). */
static void put_u64(char *buf, int *pos, int cap, u64 v)
{
    char tmp[24]; int n=0;
    if(v==0){ tmp[n++]='0'; }
    while(v){ tmp[n++]=(char)('0'+(int)(v%10)); v/=10; }
    while(n>0 && *pos<cap-1) buf[(*pos)++]=tmp[--n];
    buf[*pos]=0;
}

/* Append one char to buf[pos..cap), bounded. */
static void put_ch(char *buf, int *pos, int cap, char c)
{
    if(*pos<cap-1){ buf[*pos]=c; buf[*pos+1]=0; (*pos)++; }
}

/* Minimal formatter. Specifiers: %s %c %d(int) %l(i64) %u(u64) %%. */
static void m_vfmt(char *buf, int cap, const char *f, va_list ap)
{
    int pos=0;
    if(cap<=0) return;
    for(; *f && pos<cap-1; f++){
        if(*f!='%'){ buf[pos++]=*f; continue; }
        f++;
        switch(*f){
            case 's': { const char *s=va_arg(ap,const char*);
                        while(s && *s && pos<cap-1) buf[pos++]=*s++; } break;
            case 'c': { int c=va_arg(ap,int); if(pos<cap-1) buf[pos++]=(char)c; } break;
            case 'd': { int v=va_arg(ap,int);
                        if(v<0){ if(pos<cap-1) buf[pos++]='-'; put_u64(buf,&pos,cap,(u64)(-(i64)v)); }
                        else put_u64(buf,&pos,cap,(u64)v); } break;
            case 'l': { i64 v=va_arg(ap,i64);
                        if(v<0){ if(pos<cap-1) buf[pos++]='-'; put_u64(buf,&pos,cap,(u64)(-v)); }
                        else put_u64(buf,&pos,cap,(u64)v); } break;
            case 'u': { u64 v=va_arg(ap,u64); put_u64(buf,&pos,cap,v); } break;
            case '%': buf[pos++]='%'; break;
            default:  if(pos<cap-1) buf[pos++]='%'; if(*f && pos<cap-1) buf[pos++]=*f; break;
        }
        if(!*f) break;
    }
    buf[pos]=0;
}

/* ------------------------------------------------------------------ */
/*  Integer math primitives                                           */
/* ------------------------------------------------------------------ */
static u64 isqrt64(u64 n)
{
    if(n<2) return n;
    u64 x=n, y=n/2+1;      /* (n+1)/2 without overflowing at n=UINT64_MAX */
    if(y<x) x=y;
    while(1){ y=(x + n/x)/2; if(y>=x) return x; x=y; }
}

static u64 gcd_u64(u64 a, u64 b){ while(b){ u64 t=a%b; a=b; b=t; } return a; }

/* (x+y) mod m for 0<=x,y<m, with no 64-bit overflow (full-range m). */
static u64 addmod(u64 x, u64 y, u64 m)
{
    return (x >= m - y) ? x - (m - y) : x + y;
}

/* (a*b) mod m without 128-bit types (Russian-peasant). Requires m>=1.
 * Correct for the FULL 64-bit range of m (addmod never overflows). */
static u64 mulmod(u64 a, u64 b, u64 m)
{
    if(m<=1) return 0;
    a%=m; u64 r=0;
    while(b){
        if(b&1) r=addmod(r,a,m);
        b>>=1;
        if(b) a=addmod(a,a,m);
    }
    return r;
}

static u64 modpow(u64 a, u64 e, u64 m)
{
    if(m<=1) return 0;
    u64 r=1%m; a%=m;
    while(e){ if(e&1) r=mulmod(r,a,m); a=mulmod(a,a,m); e>>=1; }
    return r;
}

/* Deterministic Miller-Rabin for the full 64-bit range. */
static int is_prime_u64(u64 n)
{
    static const u64 bases[12] = {2,3,5,7,11,13,17,19,23,29,31,37};
    if(n<2) return 0;
    for(int i=0;i<12;i++){ u64 p=bases[i]; if(n%p==0) return n==p; }
    u64 d=n-1; int s=0;
    while(!(d&1)){ d>>=1; s++; }
    for(int i=0;i<12;i++){
        u64 a=bases[i]%n; if(a==0) continue;
        u64 x=modpow(a,d,n);
        if(x==1 || x==n-1) continue;
        int ok=0;
        for(int r=1;r<s;r++){ x=mulmod(x,x,n); if(x==n-1){ ok=1; break; } }
        if(!ok) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Framework: editable-field calculator windows                      */
/* ------------------------------------------------------------------ */
enum { K_PRIME=0, K_FACT, K_FIB, K_GCD, K_QUAD, K_PFACT, K_MODPOW, K_ISQRT, K_PASCAL, NKIND };

#define MAXF     3
#define OUTMAX   112
#define LINEW    100
#define FLD_MAX  18     /* max digits (fits i64) */
#define QUAD_LIM 1000000000LL  /* |coef| cap: keeps b*b-4*a*c inside i64  */

enum { BTN_DEC=1, BTN_INC=2, BTN_CLOSE=3 };

/* Window layout (client-relative coords), computed by mk_layout() further
 * down.  Declared here so calc_state can cache one instance per window. */
typedef struct {
    int sc, lh;
    int fy0;                 /* first field row y                     */
    int outy, outx, outw, outh, rows;
    wm_button btn[3];        /* DEC, INC, CLOSE                        */
    int nbtn;
} layout;

typedef struct {
    int        kind;
    int        nf;                 /* active field count                */
    char       flabel[MAXF][14];
    char       fbuf[MAXF][FLD_MAX+2];
    int        allow_neg;
    int        focus;              /* 0..nf-1                           */
    char       out[OUTMAX][LINEW];
    UINT32     outcol[OUTMAX];
    int        nout;
    int        scroll;
    int        b_hover, b_press;
    wm_window *win;
} calc_state;

static calc_state g_calc[NKIND];

static int gsc(void){ int s=ui_scale(); return s<1?1:s; }

/* On-theme colors. */
static UINT32 c_win(void){ return wm_theme_color(WM_COL_WINDOW); }
static UINT32 c_fg(void){ return wm_theme_color(WM_COL_FG); }
static UINT32 c_accent(void){ return wm_theme_color(WM_COL_ACCENT); }
static UINT32 c_selbg(void){ return wm_theme_color(WM_COL_SEL_BG); }
static UINT32 c_selfg(void){ return wm_theme_color(WM_COL_SEL_FG); }
static UINT32 c_dim(void){ return wm_blend(c_fg(), c_win(), 120); }

/* Parse a decimal field buffer (optional leading '-') into i64. */
static i64 field_i64(const char *s)
{
    int neg=0; const char *p=s;
    if(*p=='-'){ neg=1; p++; }
    u64 v=0;
    while(*p>='0'&&*p<='9'){ v=v*10 + (u64)(*p-'0'); p++; }
    i64 r=(i64)v;
    return neg? -r : r;
}
static u64 field_u64(const char *s)
{
    const char *p=s; if(*p=='-') p++;
    u64 v=0; while(*p>='0'&&*p<='9'){ v=v*10 + (u64)(*p-'0'); p++; }
    return v;
}

/* Write an i64 into a field buffer. */
static void set_field_i64(char *buf, i64 v)
{
    int pos=0;
    if(v<0){ buf[pos++]='-'; put_u64(buf,&pos,FLD_MAX+2,(u64)(-v)); }
    else    { put_u64(buf,&pos,FLD_MAX+2,(u64)v); }
}

/* Append an output line with color; ignores overflow past OUTMAX. */
static void outln(calc_state *cs, UINT32 col, const char *f, ...)
{
    if(cs->nout>=OUTMAX) return;
    va_list ap; va_start(ap,f);
    m_vfmt(cs->out[cs->nout], LINEW, f, ap);
    va_end(ap);
    cs->outcol[cs->nout]=col;
    cs->nout++;
}

/* -------- per-kind result computation ------------------------------ */
static void compute_prime(calc_state *cs)
{
    u64 n = field_u64(cs->fbuf[0]);
    if(n<2){
        outln(cs, c_dim(), "%u is not prime (need n >= 2).", n);
    } else if(is_prime_u64(n)){
        outln(cs, c_accent(), "%u is PRIME.", n);
    } else {
        outln(cs, c_fg(), "%u is composite.", n);
        u64 f=0;
        if((n&1)==0) f=2;
        else {
            /* Trial division, frame-budgeted (Miller-Rabin already proved
             * compositeness, so a factor <= sqrt(n) provably exists). */
            u64 lim=isqrt64(n);
            /* Miller-Rabin at line 224 already proved compositeness, so we only
             * need a small smallest-factor probe here; cap it tight to stay
             * responsive on every keystroke ('trial capped' path handles miss). */
            u64 bound=lim; if(bound>50000ull) bound=50000ull;
            for(u64 d=3; d<=bound; d+=2){ if(n%d==0){ f=d; break; } }
            if(!f && bound<lim)
                outln(cs, c_dim(), "  smallest factor > %u (trial capped)", bound);
        }
        if(f) outln(cs, c_dim(), "  smallest factor = %u", f);
    }
    /* previous prime (track success in-loop; avoid a second is_prime_u64) */
    if(n>2){
        u64 p=n-1; int guard=0, found=0;
        while(p>=2 && guard<200000){
            if(is_prime_u64(p)){ found=1; break; }
            p--; guard++;
        }
        if(found) outln(cs,c_dim(),"prev prime <= n-1 : %u", p);
    }
    /* next prime strictly greater than n (track success in-loop) */
    {
        u64 q = n+1; int guard=0, found=0;
        while(guard<200000){
            if(is_prime_u64(q)){ found=1; break; }
            q++; guard++;
        }
        if(found) outln(cs, c_fg(), "next prime > n    : %u", q);
    }
    outln(cs, c_dim(), "");
    outln(cs, c_dim(), "Miller-Rabin, exact for all 64-bit n.");
}

static void compute_factorial(calc_state *cs)
{
    i64 ns = field_i64(cs->fbuf[0]);
    if(ns<0){ outln(cs,c_dim(),"n must be >= 0."); return; }
    u64 n=(u64)ns;
    if(n>20){
        outln(cs, c_dim(), "%u! overflows 64-bit (max is 20!).", n);
        outln(cs, c_dim(), "Showing 0! .. 20! :");
        n=20;
    }
    u64 f=1;
    u64 nsel=field_u64(cs->fbuf[0]);
    for(u64 k=0;k<=n;k++){
        if(k>0) f*=k;
        outln(cs, k==nsel?c_accent():c_fg(), "%u! = %u", k, f);
    }
}

static void compute_fib(calc_state *cs)
{
    i64 cnt = field_i64(cs->fbuf[0]);
    if(cnt<1){ outln(cs,c_dim(),"count must be >= 1."); return; }
    u64 a=0,b=1, sum=0; int printed=0, sumovf=0;
    for(i64 i=0; i<cnt; i++){
        if(i>93){ outln(cs,c_dim(),"(F94+ overflow 64-bit; stopped at F93)"); break; }
        outln(cs, (i==cnt-1)?c_accent():c_fg(), "F%l = %u", i, a);
        if(sum+a < sum) sumovf=1; else sum+=a;   /* overflow-checked sum */
        printed++;
        u64 nx=a+b; a=b; b=nx;
    }
    outln(cs, c_dim(), "");
    if(sumovf) outln(cs, c_dim(), "sum of %d shown terms overflows 64-bit.", printed);
    else       outln(cs, c_dim(), "sum of %d shown terms = %u", printed, sum);
}

static void compute_gcdlcm(calc_state *cs)
{
    i64 A=field_i64(cs->fbuf[0]), B=field_i64(cs->fbuf[1]);
    u64 a=(u64)(A<0?-A:A), b=(u64)(B<0?-B:B);
    u64 g=gcd_u64(a,b);
    outln(cs, c_fg(), "a = %l   b = %l", A, B);
    outln(cs, c_accent(), "gcd(a,b) = %u", g);
    if(g==0){ outln(cs, c_dim(), "lcm(a,b) = 0"); return; }
    u64 l = (a/g);
    /* overflow-checked lcm = a/g*b */
    if(b!=0 && l > (~(u64)0)/b){
        outln(cs, c_dim(), "lcm(a,b) overflows 64-bit.");
    } else {
        outln(cs, c_accent(), "lcm(a,b) = %u", l*b);
    }
    /* coprime note */
    outln(cs, c_dim(), "");
    outln(cs, c_dim(), g==1 ? "a and b are coprime." : "a and b share common factors.");
}

static void compute_quad(calc_state *cs)
{
    i64 a=field_i64(cs->fbuf[0]), b=field_i64(cs->fbuf[1]), c=field_i64(cs->fbuf[2]);
    outln(cs, c_fg(), "%l x^2 + %l x + %l = 0", a, b, c);
    /* |a|,|b|,|c| <= 1e9  =>  |b*b - 4*a*c| <= 5e18 < 2^63 (no i64 overflow). */
    if(a>QUAD_LIM||a<-QUAD_LIM||b>QUAD_LIM||b<-QUAD_LIM||c>QUAD_LIM||c<-QUAD_LIM){
        outln(cs, c_dim(), "|a|,|b|,|c| must be <= %l to keep the", (i64)QUAD_LIM);
        outln(cs, c_dim(), "discriminant inside signed 64-bit.");
        return;
    }
    if(a==0){
        outln(cs, c_dim(), "a = 0 -> linear equation.");
        if(b==0){ outln(cs, c_dim(), c==0?"any x is a solution.":"no solution."); return; }
        /* x = -c / b as reduced fraction */
        i64 num=-c, den=b;
        if(den<0){ num=-num; den=-den; }
        u64 g=gcd_u64((u64)(num<0?-num:num),(u64)den); if(g==0)g=1;
        num/=(i64)g; den/=(i64)g;
        if(den==1) outln(cs, c_accent(), "x = %l", num);
        else       outln(cs, c_accent(), "x = %l/%l", num, den);
        return;
    }
    i64 disc = b*b - 4*a*c;
    outln(cs, c_fg(), "discriminant D = %l", disc);
    if(disc<0){
        outln(cs, c_dim(), "D < 0 : no real roots (complex conjugate pair).");
        return;
    }
    u64 s = isqrt64((u64)disc);
    if(s*s==(u64)disc){
        outln(cs, c_dim(), "D is a perfect square (sqrt D = %u).", s);
        i64 den=2*a;
        for(int sign=0; sign<2; sign++){
            i64 num = -b + (sign? -(i64)s : (i64)s);
            i64 dn=den;
            if(dn<0){ num=-num; dn=-dn; }
            u64 g=gcd_u64((u64)(num<0?-num:num),(u64)dn); if(g==0)g=1;
            i64 rn=num/(i64)g, rd=dn/(i64)g;
            if(rd==1) outln(cs, c_accent(), "x%d = %l", sign+1, rn);
            else      outln(cs, c_accent(), "x%d = %l/%l", sign+1, rn, rd);
            if(s==0) break; /* double root */
        }
        if(s==0) outln(cs, c_dim(), "(double root)");
    } else {
        outln(cs, c_dim(), "D not a perfect square -> irrational roots.");
        outln(cs, c_accent(), "x = (%l +/- sqrt %l) / %l", -b, disc, 2*a);
        outln(cs, c_dim(), "floor(sqrt D) = %u", s);
    }
}

static void compute_factor(calc_state *cs)
{
    u64 n = field_u64(cs->fbuf[0]);
    if(n<2){ outln(cs,c_dim(),"%u has no prime factorization (need n >= 2).", n); return; }
    if(is_prime_u64(n)){ outln(cs, c_accent(), "%u is prime.", n); return; }

    char pretty[LINEW]; int pp=0; pretty[0]=0;
    u64 m=n; int nterms=0;
    outln(cs, c_fg(), "%u =", n);
    /* factor out 2 first */
    {
        int e=0; while(m%2==0){ m/=2; e++; }
        if(e){ outln(cs, c_accent(), "  2^%d = %u", e, (u64)1<<e);
               put_u64(pretty,&pp,LINEW,2);
               if(e>1){ put_ch(pretty,&pp,LINEW,'^'); put_u64(pretty,&pp,LINEW,(u64)e); }
               nterms++; }
    }
    /* odd trial division, bounded so we never stall the frame */
    u64 limit = isqrt64(m);
    /* keep editing responsive; anything past this falls to the 'beyond trial
     * bound' branch below, which reports the remaining cofactor as-is. */
    u64 bound = limit; if(bound > 100000ull) bound = 100000ull;
    for(u64 d=3; d<=bound; d+=2){
        if(m%d==0){
            int e=0; u64 pw=1; while(m%d==0){ m/=d; e++; pw*=d; }
            outln(cs, c_accent(), "  %u^%d = %u", d, e, pw);
            if(pp){ put_ch(pretty,&pp,LINEW,' '); put_ch(pretty,&pp,LINEW,'x'); put_ch(pretty,&pp,LINEW,' '); }
            put_u64(pretty,&pp,LINEW,d);
            if(e>1){ put_ch(pretty,&pp,LINEW,'^'); put_u64(pretty,&pp,LINEW,(u64)e); }
            nterms++;
            limit=isqrt64(m); if(limit<bound) bound=limit;
        }
    }
    if(m>1){
        if(is_prime_u64(m)){
            outln(cs, c_accent(), "  %u^1 = %u", m, m);
            if(pp){ put_ch(pretty,&pp,LINEW,' '); put_ch(pretty,&pp,LINEW,'x'); put_ch(pretty,&pp,LINEW,' '); }
            put_u64(pretty,&pp,LINEW,m);
            nterms++;
        } else {
            outln(cs, c_dim(), "  %u = large composite (beyond trial bound)", m);
            if(pp){ put_ch(pretty,&pp,LINEW,' '); put_ch(pretty,&pp,LINEW,'x'); put_ch(pretty,&pp,LINEW,' '); }
            put_u64(pretty,&pp,LINEW,m);
        }
    }
    pretty[pp]=0;
    outln(cs, c_dim(), "");
    outln(cs, c_fg(), "%u = %s", n, pretty);
    outln(cs, c_dim(), "%d distinct prime factors", nterms);
}

static void compute_modpow(calc_state *cs)
{
    i64 as=field_i64(cs->fbuf[0]);
    i64 es=field_i64(cs->fbuf[1]);
    u64 m =field_u64(cs->fbuf[2]);
    outln(cs, c_fg(), "compute a^b mod m");
    if(m==0){ outln(cs, c_dim(), "m = 0 : modulus must be >= 1."); return; }
    if(es<0){ outln(cs, c_dim(), "b < 0 : exponent must be >= 0."); return; }
    if(m==1){ outln(cs, c_accent(), "result = 0   (everything is 0 mod 1)"); return; }
    /* normalize possibly-negative base into [0,m) */
    u64 a;
    if(as<0){ u64 t=(u64)(-as)%m; a = t? m-t : 0; }
    else      a=(u64)as % m;
    u64 r = modpow(a,(u64)es,m);
    outln(cs, c_dim(), "a mod m = %u", a);
    outln(cs, c_accent(), "%l ^ %l mod %u = %u", as, es, m, r);
}

static void compute_isqrt(calc_state *cs)
{
    i64 ns=field_i64(cs->fbuf[0]);
    if(ns<0){ outln(cs,c_dim(),"n must be >= 0."); return; }
    u64 n=(u64)ns;
    u64 r=isqrt64(n);
    outln(cs, c_accent(), "floor(sqrt(%u)) = %u", n, r);
    outln(cs, c_fg(), "%u^2 = %u", r, r*r);
    if((r+1) <= 4294967295ull) outln(cs, c_dim(), "(%u+1)^2 = %u", r, (r+1)*(r+1));
    if(r*r==n) outln(cs, c_accent(), "%u IS a perfect square.", n);
    else       outln(cs, c_dim(), "%u is not a perfect square (remainder %u).", n, n-r*r);
    outln(cs, c_dim(), "");
    outln(cs, c_dim(), "Newton's method, purely integer.");
}

static void compute_pascal(calc_state *cs)
{
    i64 rs=field_i64(cs->fbuf[0]);
    if(rs<1){ outln(cs,c_dim(),"rows must be >= 1."); return; }
    int rows=(int)rs;
    if(rows>62){ rows=62; outln(cs,c_dim(),"capped at 62 rows (values fit 64-bit)."); }
    for(int r=0; r<rows; r++){
        char line[LINEW]; int pos=0; line[0]=0;
        u64 val=1; int overflow=0;
        for(int k=0;k<=r;k++){
            if(pos>0 && pos<LINEW-1) line[pos++]=' ';
            put_u64(line,&pos,LINEW,val);
            /* next = val*(r-k)/(k+1) with overflow guard */
            if(k<r){
                u64 mul=(u64)(r-k);
                if(val!=0 && mul!=0 && val > (~(u64)0)/mul){ overflow=1; break; }
                val = val*mul/(u64)(k+1);
            }
        }
        line[pos]=0;
        outln(cs, (r==rows-1)?c_accent():c_fg(), "%s%s", line, overflow?" ...(overflow)":"");
    }
}

static void recompute(calc_state *cs)
{
    cs->nout=0; cs->scroll=0;
    switch(cs->kind){
        case K_PRIME:  compute_prime(cs);     break;
        case K_FACT:   compute_factorial(cs); break;
        case K_FIB:    compute_fib(cs);       break;
        case K_GCD:    compute_gcdlcm(cs);    break;
        case K_QUAD:   compute_quad(cs);      break;
        case K_PFACT:  compute_factor(cs);    break;
        case K_MODPOW: compute_modpow(cs);    break;
        case K_ISQRT:  compute_isqrt(cs);     break;
        case K_PASCAL: compute_pascal(cs);    break;
        default: break;
    }
    if(cs->nout==0) outln(cs, c_dim(), "(enter a value)");
}

/* ------------------------------------------------------------------ */
/*  Layout (client-relative coords) - struct `layout` defined near top */
/* ------------------------------------------------------------------ */
static void mk_layout(calc_state *cs, int cw, int ch, layout *L)
{
    int sc=gsc(); int lh=16*sc+2*sc;
    L->sc=sc; L->lh=lh;
    int y=5*sc;
    y += lh + 2*sc;                 /* title line + gap               */
    L->fy0=y;
    y += cs->nf*lh;
    y += 4*sc;                      /* gap before separator/output    */
    L->outx = 6*sc;
    L->outw = cw - 12*sc;
    L->outy = y;

    int bh = wm_button_h();
    int barY = ch - bh - 4*sc;
    L->outh = barY - 4*sc - L->outy;
    if(L->outh < lh) L->outh = lh;
    L->rows = L->outh / lh;
    if(L->rows < 1) L->rows = 1;

    int dw = wm_button_measure("-1");
    int iw = wm_button_measure("+1");
    int clw= wm_button_measure("Close");
    wm_button dec = { 6*sc, barY, dw, bh, BTN_DEC, 1, "" };
    wm_button inc = { 6*sc+dw+4*sc, barY, iw, bh, BTN_INC, 1, "" };
    wm_button cls = { cw-clw-6*sc, barY, clw, bh, BTN_CLOSE, 1, "" };
    m_strcpy(dec.label, "-1", sizeof dec.label);
    m_strcpy(inc.label, "+1", sizeof inc.label);
    m_strcpy(cls.label, "Close", sizeof cls.label);
    L->btn[0]=dec; L->btn[1]=inc; L->btn[2]=cls; L->nbtn=3;
}

/* ------------------------------------------------------------------ */
/*  Draw                                                               */
/* ------------------------------------------------------------------ */
static void calc_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    calc_state *cs=(calc_state*)wm_user(w); if(!cs) return;
    layout L; mk_layout(cs, cw, ch, &L);
    int sc=L.sc, lh=L.lh;
    UINT32 win=c_win();

    fill_rect(cx, cy, cw, ch, win);

    /* title */
    draw_string_clip(cx+6*sc, cy+5*sc, cw-12*sc,
                     cs->kind==K_PRIME?"Prime checker + next prime":
                     cs->kind==K_FACT ?"Factorial n! (0..20)":
                     cs->kind==K_FIB  ?"Fibonacci sequence F0..Fn":
                     cs->kind==K_GCD  ?"GCD / LCM of two integers":
                     cs->kind==K_QUAD ?"Integer quadratic solver":
                     cs->kind==K_PFACT?"Prime factorizer":
                     cs->kind==K_MODPOW?"Modular power  a^b mod m":
                     cs->kind==K_ISQRT?"Integer sqrt + perfect square":
                                       "Pascal's triangle",
                     c_accent(), win, 1, sc);

    /* fields */
    for(int i=0;i<cs->nf;i++){
        int ry = cy + L.fy0 + i*lh;
        int focused = (cs->focus==i);
        if(focused) fill_rect(cx+3*sc, ry-1, cw-6*sc, lh, c_selbg());
        char line[64];
        /* build "label = value_" */
        int pos=0; line[0]=0;
        m_strcpy(line, cs->flabel[i], sizeof line);
        pos=m_strlen(line);
        if(pos<(int)sizeof line-3){ line[pos++]=' '; line[pos++]='='; line[pos++]=' '; line[pos]=0; }
        {
            int j=0;
            while(cs->fbuf[i][j] && pos<(int)sizeof line-2){ line[pos++]=cs->fbuf[i][j++]; }
            if(focused && pos<(int)sizeof line-2) line[pos++]='_';
            line[pos]=0;
        }
        draw_string_clip(cx+6*sc, ry, cw-12*sc, line,
                         focused?c_selfg():c_fg(), focused?c_selbg():win, 1, sc);
    }

    /* separator */
    draw_hline(cx+6*sc, cy+L.outy-3*sc, cw-12*sc, wm_blend(c_fg(), win, 60));

    /* output (scrollable) */
    int barw = (cs->nout > L.rows) ? 6*sc : 0;
    if(cs->scroll > cs->nout - L.rows) cs->scroll = cs->nout - L.rows;
    if(cs->scroll < 0) cs->scroll = 0;
    for(int r=0;r<L.rows;r++){
        int idx=cs->scroll+r; if(idx>=cs->nout) break;
        int ry=cy+L.outy + r*lh;
        draw_string_clip(cx+L.outx, ry, L.outw-barw, cs->out[idx], cs->outcol[idx], win, 1, sc);
    }
    if(barw){
        int trackX=cx+cw-barw-2*sc, trackY=cy+L.outy, trackH=L.rows*lh;
        fill_rect(trackX, trackY, barw, trackH, wm_blend(c_fg(), win, 40));
        int thumbH=L.rows*trackH/cs->nout; if(thumbH<8*sc)thumbH=8*sc;
        int denom=cs->nout-L.rows;
        int thumbY=trackY + (denom>0 ? cs->scroll*(trackH-thumbH)/denom : 0);
        fill_rect(trackX, thumbY, barw, thumbH, c_accent());
    }

    /* buttons */
    for(int i=0;i<L.nbtn;i++)
        wm_button_draw(&L.btn[i], cs->b_hover==L.btn[i].id, cs->b_press==L.btn[i].id);
}

/* ------------------------------------------------------------------ */
/*  Events                                                             */
/* ------------------------------------------------------------------ */
static void field_edit_digit(calc_state *cs, char ch)
{
    char *b=cs->fbuf[cs->focus];
    int len=m_strlen(b);
    if(len<FLD_MAX){ b[len]=ch; b[len+1]=0; recompute(cs); }
}
static void field_backspace(calc_state *cs)
{
    char *b=cs->fbuf[cs->focus];
    int len=m_strlen(b);
    if(len>0){ b[len-1]=0; recompute(cs); }
}
static void field_toggle_sign(calc_state *cs)
{
    if(!cs->allow_neg) return;
    char *b=cs->fbuf[cs->focus];
    if(b[0]=='-'){
        /* drop the minus */
        int i=0; while(b[i+1]){ b[i]=b[i+1]; i++; } b[i]=0;
    } else {
        int len=m_strlen(b);
        if(len<FLD_MAX+1){ for(int i=len;i>=0;i--) b[i+1]=b[i]; b[0]='-'; }
    }
    recompute(cs);
}
static void field_nudge(calc_state *cs, i64 delta)
{
    i64 v=field_i64(cs->fbuf[cs->focus]) + delta;
    if(!cs->allow_neg && v<0) v=0;
    set_field_i64(cs->fbuf[cs->focus], v);
    recompute(cs);
}

static int calc_event(wm_window *w, const wm_event *ev)
{
    calc_state *cs=(calc_state*)wm_user(w); if(!cs) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    layout L; mk_layout(cs, cw, ch, &L);

    switch(ev->type){
        case WM_EV_KEY:
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            if(ev->scancode==SCAN_UP || ev->scancode==SCAN_LEFT){
                cs->focus=(cs->focus + cs->nf - 1)%cs->nf; return 0; }
            if(ev->scancode==SCAN_DOWN || ev->scancode==SCAN_RIGHT || ev->unicode=='\t'){
                cs->focus=(cs->focus + 1)%cs->nf; return 0; }
            if(ev->scancode==SCAN_PAGE_UP){   cs->scroll-=L.rows; if(cs->scroll<0)cs->scroll=0; return 0; }
            if(ev->scancode==SCAN_PAGE_DOWN){ cs->scroll+=L.rows; return 0; }
            if(ev->scancode==SCAN_HOME){ cs->scroll=0; return 0; }
            if(ev->scancode==SCAN_END){  cs->scroll=cs->nout; return 0; }
            if(ev->unicode>='0' && ev->unicode<='9'){ field_edit_digit(cs,(char)ev->unicode); return 0; }
            if(ev->unicode=='-'){ field_toggle_sign(cs); return 0; }
            if(ev->unicode=='+' ){ field_nudge(cs, 1); return 0; }
            if(ev->unicode==CHAR_BACKSPACE){ field_backspace(cs); return 0; }
            return 0;

        case WM_EV_MOUSE_WHEEL:
            cs->scroll -= ev->wheel;
            if(cs->scroll<0) cs->scroll=0;
            return 0;

        case WM_EV_MOUSE_MOVE: {
            cs->b_hover=0;
            for(int i=0;i<L.nbtn;i++)
                if(wm_button_hit(&L.btn[i], ev->mx, ev->my)){ cs->b_hover=L.btn[i].id; break; }
            return 0; }

        case WM_EV_MOUSE_DOWN: {
            for(int i=0;i<L.nbtn;i++)
                if(wm_button_hit(&L.btn[i], ev->mx, ev->my)){ cs->b_press=L.btn[i].id; return 0; }
            /* field row click -> focus */
            for(int i=0;i<cs->nf;i++){
                int ry=L.fy0 + i*L.lh;
                if(ev->my>=ry-1 && ev->my<ry-1+L.lh){ cs->focus=i; return 0; }
            }
            return 0; }

        case WM_EV_MOUSE_UP: {
            if(!cs->b_press) return 0;
            int p=cs->b_press; cs->b_press=0;
            int hit=0;
            for(int i=0;i<L.nbtn;i++)
                if(wm_button_hit(&L.btn[i], ev->mx, ev->my)){ hit=L.btn[i].id; break; }
            if(hit==p){
                if(p==BTN_CLOSE) return WM_CLOSE_REQUEST;
                if(p==BTN_DEC)   field_nudge(cs, -1);
                if(p==BTN_INC)   field_nudge(cs, +1);
            }
            return 0; }

        case WM_EV_CLOSE:
            cs->win=NULL;
            return 0;

        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Openers                                                            */
/* ------------------------------------------------------------------ */
static void set_labels(calc_state *cs, const char *l0, const char *l1, const char *l2)
{
    if(l0) m_strcpy(cs->flabel[0], l0, sizeof cs->flabel[0]);
    if(l1) m_strcpy(cs->flabel[1], l1, sizeof cs->flabel[1]);
    if(l2) m_strcpy(cs->flabel[2], l2, sizeof cs->flabel[2]);
}

static void calc_open(int kind, int nf, int allow_neg,
                      const char *l0, const char *l1, const char *l2,
                      i64 d0, i64 d1, i64 d2,
                      const char *title, int wpct, int hpct)
{
    calc_state *cs=&g_calc[kind];
    if(cs->win) return;                       /* single instance      */

    /* zero the whole state */
    for(int i=0;i<MAXF;i++){ cs->fbuf[i][0]=0; cs->flabel[i][0]=0; }
    cs->kind=kind; cs->nf=nf; cs->allow_neg=allow_neg;
    cs->focus=0; cs->scroll=0; cs->nout=0; cs->b_hover=0; cs->b_press=0;
    set_labels(cs, l0, l1, l2);
    if(nf>0) set_field_i64(cs->fbuf[0], d0);
    if(nf>1) set_field_i64(cs->fbuf[1], d1);
    if(nf>2) set_field_i64(cs->fbuf[2], d2);
    recompute(cs);

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*wpct/100; if(ww<440)ww=440; if(ww>820)ww=820; if(ww>W-40)ww=W-40;
    int wh=H*hpct/100; if(wh<300)wh=300; if(wh>680)wh=680; if(wh>H-40)wh=H-40;
    cs->win=wm_open(title, ww, wh, calc_draw, calc_event, cs);
}

void tool_math_prime_open(void)
{ calc_open(K_PRIME, 1, 0, "n", 0,0, 1000003,0,0, "Prime Checker", 52, 55); }

void tool_math_factorial_open(void)
{ calc_open(K_FACT, 1, 0, "n", 0,0, 12,0,0, "Factorial", 50, 62); }

void tool_math_fib_open(void)
{ calc_open(K_FIB, 1, 0, "count", 0,0, 20,0,0, "Fibonacci", 50, 66); }

void tool_math_gcdlcm_open(void)
{ calc_open(K_GCD, 2, 1, "a", "b", 0, 462,1071,0, "GCD / LCM", 52, 50); }

void tool_math_quad_open(void)
{ calc_open(K_QUAD, 3, 1, "a", "b", "c", 1,-5,6, "Quadratic Solver", 56, 56); }

void tool_math_factor_open(void)
{ calc_open(K_PFACT, 1, 0, "n", 0,0, 360360,0,0, "Prime Factorizer", 54, 60); }

void tool_math_modpow_open(void)
{ calc_open(K_MODPOW, 3, 1, "a (base)", "b (exp)", "m (mod)", 7, 256, 13, "Modular Power", 56, 50); }

void tool_math_isqrt_open(void)
{ calc_open(K_ISQRT, 1, 0, "n", 0,0, 1000000,0,0, "Integer Sqrt", 52, 52); }

void tool_math_pascal_open(void)
{ calc_open(K_PASCAL, 1, 0, "rows", 0,0, 12,0,0, "Pascal's Triangle", 60, 66); }

/* ------------------------------------------------------------------ */
/*  Category registry                                                  */
/* ------------------------------------------------------------------ */
const struct forebo_tool cat_math_tools[] = {
    { "Prime Checker",    "Test primality + previous/next prime",        "gear", tool_math_prime_open     },
    { "Factorial",        "n! for 0..20 (64-bit, capped)",               "gear", tool_math_factorial_open },
    { "Fibonacci",        "List Fibonacci terms F0..Fn",                 "gear", tool_math_fib_open       },
    { "GCD / LCM",        "Greatest common divisor & least multiple",    "gear", tool_math_gcdlcm_open    },
    { "Quadratic Solver", "Integer ax^2+bx+c via discriminant",          "gear", tool_math_quad_open      },
    { "Prime Factorizer", "Break n into its prime factors",              "gear", tool_math_factor_open    },
    { "Modular Power",    "Fast a^b mod m (modular exponentiation)",     "gear", tool_math_modpow_open    },
    { "Integer Sqrt",     "Newton floor-sqrt + perfect-square test",     "gear", tool_math_isqrt_open     },
    { "Pascal's Triangle","Binomial coefficient triangle (scroll)",      "gear", tool_math_pascal_open    },
};
const int cat_math_count = (int)(sizeof(cat_math_tools)/sizeof(cat_math_tools[0]));

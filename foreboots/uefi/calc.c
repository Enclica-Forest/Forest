/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/calc.c - fixed-point calculator + function grapher (TEMPLATE B window).
 * =============================================================================
 * See calc.h. SSE/x87 are disabled by the build flags, so "floating point"
 * here is SOFTWARE Q32.32 signed fixed-point carried in an INT64 (~9 decimal
 * digits, range +/-2.1e9). All transcendentals are integer-only: sin/cos use
 * argument reduction mod 2pi + a Taylor polynomial, sqrt uses Newton, ^ is
 * integer powers by squaring. 128-bit products use __int128 (inline mul only -
 * NO 128-bit division anywhere, so no libgcc __divti3/__udivti3 is pulled in);
 * fixed-point division is a 64/64 divide for the integer part plus a 32-step
 * long division for the fraction.
 *
 * The GRA button (or 'g') opens a second window plotting y = f(x) for the
 * SAME expression, with adaptive grid, axes, tick labels, pan/zoom and a
 * cursor trace. Typing while the graph is focused edits the shared expression
 * live.
 * ========================================================================== */
#include "efi.h"
#include "ui.h"
#include "wm.h"
#include "calc.h"

/* ------------------------------------------------------------------ */
/*  Small freestanding string helpers (no libc)                        */
/* ------------------------------------------------------------------ */
static int cslen(const char *s){ int n=0; if(!s) return 0; while(s[n]) n++; return n; }
static int is_digit(char c){ return c>='0' && c<='9'; }
static int is_alpha(char c){ return (c>='a'&&c<='z') || (c>='A'&&c<='Z'); }
static char to_lower(char c){ return (c>='A'&&c<='Z') ? (char)(c+32) : c; }

#define I64_MIN (-9223372036854775807LL - 1)

/* ------------------------------------------------------------------ */
/*  Q32.32 signed fixed-point core                                     */
/* ------------------------------------------------------------------ */
#define FX_ONE      ((INT64)1 << 32)
#define FX_FRAC     0xFFFFFFFFLL
#define FX_INT(n)   ((INT64)(n) * FX_ONE)

#define FX_PI       13493037705LL   /* pi   * 2^32 */
#define FX_TWO_PI   26986075409LL   /* 2pi  * 2^32 */
#define FX_HALF_PI  6746518852LL    /* pi/2 * 2^32 */
#define FX_E        11674931555LL   /* e    * 2^32 */

/* err codes: 0 ok, 1 syntax, 2 divide-by-zero, 3 overflow, 4 empty, 5 domain */

/* a*b in fixed point. __int128 mul is inline on x86-64/AArch64 (no libcall). */
static INT64 fx_mul(INT64 a, INT64 b, int *err)
{
    __int128 p = ((__int128)a * (__int128)b) >> 32;
    if(p > (__int128)9223372036854775807LL || p < (__int128)I64_MIN){ *err=3; return 0; }
    return (INT64)p;
}

/* a/b in fixed point: 64/64 divide + 32-step long division (no 128-bit div). */
static INT64 fx_div(INT64 a, INT64 b, int *err)
{
    if(b==0){ *err=2; return 0; }
    UINT64 ua = (a<0) ? (UINT64)(-(a+1)) + 1ULL : (UINT64)a;
    UINT64 ub = (b<0) ? (UINT64)(-(b+1)) + 1ULL : (UINT64)b;
    UINT64 qi  = ua / ub;
    UINT64 rem = ua % ub;
    if(qi >= (1ULL<<32)){ *err=3; return 0; }
    UINT64 frac = 0;
    for(int i=0;i<32;i++){
        rem <<= 1;              /* rem < ub <= 2^63, so this fits UINT64 */
        frac <<= 1;
        if(rem >= ub){ rem -= ub; frac |= 1ULL; }
    }
    UINT64 mag = (qi << 32) | frac;
    int neg = (a<0) != (b<0);
    if(!neg && mag > 0x7FFFFFFFFFFFFFFFULL){ *err=3; return 0; }
    if( neg && mag > 0x8000000000000000ULL){ *err=3; return 0; }
    if(neg) return (mag==0x8000000000000000ULL) ? I64_MIN : -(INT64)mag;
    return (INT64)mag;
}

/* Truncate toward zero, keeping fixed-point representation. */
static INT64 fx_trunc(INT64 v)
{
    if(v>=0) return v & ~FX_FRAC;
    if(v==I64_MIN) return v;            /* already integral */
    return -((-v) & ~FX_FRAC);
}

/* Remainder: a - b*trunc(a/b). */
static INT64 fx_mod(INT64 a, INT64 b, int *err)
{
    if(b==0){ *err=2; return 0; }
    INT64 q = fx_div(a,b,err); if(*err) return 0;
    INT64 m = fx_mul(b, fx_trunc(q), err); if(*err) return 0;
    return a - m;
}

/* base^n for INTEGER n (|n|<=1024). Repeated squaring; negative n = 1/base. */
static INT64 fx_powi(INT64 base, INT64 n, int *err)
{
    if(n<0){
        INT64 p = fx_powi(base, -n, err); if(*err) return 0;
        if(p==0){ *err=2; return 0; }
        return fx_div(FX_ONE, p, err);
    }
    INT64 r = FX_ONE;
    while(n>0){
        if(n&1){ r = fx_mul(r, base, err); if(*err) return 0; }
        n >>= 1;
        if(n){ base = fx_mul(base, base, err); if(*err) return 0; }
    }
    return r;
}

/* sin core: `r` must already be reduced to |r| <= pi (mod 2pi). Folds to
 * [-pi/2,pi/2] and evaluates the Taylor polynomial to the x^15 term
 * (|err| < 5e-9). Terms are computed as r*u^k/k! with exact fixed-point
 * divisions; 13! and 15! exceed the Q32.32 integer range, so those are
 * split into two divides (13! = 62270208*100, 15! = 1307674368*1000).
 * Factored out of fx_sin so fx_cos/fx_tan can share one mod-2pi reduction
 * instead of each performing their own expensive fx_div range reduction. */
static INT64 fx_sin_core(INT64 r, int *err)
{
    if(r >  FX_HALF_PI) r = FX_PI - r;
    else if(r < -FX_HALF_PI) r = -FX_PI - r;
    INT64 u  = fx_mul(r, r, err);        /* r^2 (<= 2.47, no overflow) */
    INT64 up = u;
    INT64 s  = r;
    INT64 t;
    t = fx_mul(r, up, err); up = fx_mul(up, u, err);
    s -= fx_div(t, FX_INT(6), err);
    t = fx_mul(r, up, err); up = fx_mul(up, u, err);
    s += fx_div(t, FX_INT(120), err);
    t = fx_mul(r, up, err); up = fx_mul(up, u, err);
    s -= fx_div(t, FX_INT(5040), err);
    t = fx_mul(r, up, err); up = fx_mul(up, u, err);
    s += fx_div(t, FX_INT(362880), err);
    t = fx_mul(r, up, err); up = fx_mul(up, u, err);
    s -= fx_div(t, FX_INT(39916800), err);
    t = fx_mul(r, up, err); up = fx_mul(up, u, err);
    s += fx_div(fx_div(t, FX_INT(62270208), err), FX_INT(100), err);
    t = fx_mul(r, up, err);
    s -= fx_div(fx_div(t, FX_INT(1307674368), err), FX_INT(1000), err);
    return s;
}

/* Reduce x to r with |r| <= pi (mod 2pi); the costly part (fx_div 32-step
 * long division). Shared by fx_sin/fx_cos/fx_tan so a given x is only
 * reduced once no matter how many of the three are evaluated on it. */
static INT64 fx_reduce2pi(INT64 x, int *err)
{
    INT64 k = fx_div(x, FX_TWO_PI, err); if(*err) return 0;
    /* nearest integer to k (fixed) */
    INT64 n = (k + (FX_ONE>>1)) >> 32;
    /* r = x - n*2pi, |r| <= pi (128-bit product: n can be ~3e8) */
    return (INT64)((__int128)x - (__int128)n * FX_TWO_PI);
}

static INT64 fx_sin(INT64 x, int *err)
{
    INT64 r = fx_reduce2pi(x, err); if(*err) return 0;
    return fx_sin_core(r, err);
}

static INT64 fx_cos(INT64 x, int *err)
{
    /* reduce mod 2pi first so the +pi/2 shift can never overflow */
    INT64 r = fx_reduce2pi(x, err); if(*err) return 0;
    /* sin(r+pi/2)=cos(r) identity holds for the whole reduced range, so the
     * core's existing +-pi/2 fold is sufficient -- no second fx_div needed */
    return fx_sin_core(r + FX_HALF_PI, err);
}

static INT64 fx_tan(INT64 x, int *err)
{
    INT64 r = fx_reduce2pi(x, err); if(*err) return 0;
    INT64 s = fx_sin_core(r, err); if(*err) return 0;
    INT64 c = fx_sin_core(r + FX_HALF_PI, err); if(*err) return 0;
    return fx_div(s, c, err);
}

/* Newton sqrt; converges from above in <= 64 steps. */
static INT64 fx_sqrt(INT64 v, int *err)
{
    if(v<0){ *err=5; return 0; }
    if(v==0) return 0;
    INT64 y = (v > FX_ONE) ? v : FX_ONE;
    for(int i=0;i<64;i++){
        INT64 q = fx_div(v, y, err); if(*err) return 0;
        INT64 ny = (INT64)(((__int128)y + q) >> 1);
        if(ny >= y) break;
        y = ny;
    }
    return y;
}

/* Format fixed-point: up to 9 fractional digits, rounded, zeros trimmed. */
static void fx_toa(INT64 v, char *out)
{
    UINT64 mag = (v<0) ? (UINT64)(-(v+1)) + 1ULL : (UINT64)v;
    mag += 2;                                   /* round at the 9th digit  */
    UINT64 ip = mag >> 32;
    UINT64 fr = mag & 0xFFFFFFFFULL;
    char tmp[24]; int i=0;
    if(ip==0) tmp[i++]='0';
    while(ip){ tmp[i++]=(char)('0'+(ip%10ULL)); ip/=10ULL; }
    int o=0;
    if(v<0) out[o++]='-';
    while(i>0) out[o++]=tmp[--i];
    if(fr){
        char fd[9]; int nd=9;
        for(int k=0;k<9;k++){ fr*=10ULL; fd[k]=(char)('0'+(fr>>32)); fr&=0xFFFFFFFFULL; }
        while(nd>0 && fd[nd-1]=='0') nd--;
        if(nd>0){
            out[o++]='.';
            for(int k=0;k<nd;k++) out[o++]=fd[k];
        }
    }
    out[o]=0;
}

/* ------------------------------------------------------------------ */
/*  Recursive-descent fixed-point expression evaluator                 */
/* ------------------------------------------------------------------ */
typedef struct { const char *p; int err; INT64 xval; int uses_x; } eparse;

static INT64 p_expr(eparse *s);   /* fwd */

static void skip_ws(eparse *s){ while(*s->p==' ') s->p++; }

static INT64 p_number(eparse *s)
{
    UINT64 ip=0; int ovf=0;
    while(is_digit(*s->p)){
        if(ip > 300000000ULL) ovf=1; else ip = ip*10ULL + (UINT64)(*s->p-'0');
        s->p++;
    }
    UINT64 fr=0, fs=1;
    if(*s->p=='.'){
        s->p++;
        while(is_digit(*s->p)){
            if(fs <= 100000000ULL){ fr = fr*10ULL + (UINT64)(*s->p-'0'); fs *= 10ULL; }
            s->p++;
        }
    }
    if(ovf || ip >= 2147483648ULL){ s->err=3; return 0; }
    INT64 v = (INT64)(ip << 32);
    if(fs>1) v |= (INT64)((fr << 32) / fs);
    return v;
}

static int id_eq(const char *id, const char *w)
{
    int i=0; while(w[i]){ if(id[i]!=w[i]) return 0; i++; }
    return id[i]==0;
}

static INT64 p_ident(eparse *s)
{
    char id[6]; int n=0;
    while(is_alpha(*s->p)){
        if(n<5) id[n]=to_lower(*s->p);
        n++; s->p++;
    }
    if(n==0 || n>5){ s->err=1; return 0; }
    id[n]=0;
    if(id_eq(id,"x")){ s->uses_x=1; return s->xval; }
    if(id_eq(id,"pi")) return FX_PI;
    if(id_eq(id,"e"))  return FX_E;
    /* otherwise a function call: name '(' expr ')' */
    skip_ws(s);
    if(*s->p!='('){ s->err=1; return 0; }
    s->p++;
    INT64 arg = p_expr(s);
    skip_ws(s);
    if(*s->p==')') s->p++;
    else if(!s->err) s->err=1;
    if(s->err) return 0;
    if(id_eq(id,"sin"))  return fx_sin(arg, &s->err);
    if(id_eq(id,"cos"))  return fx_cos(arg, &s->err);
    if(id_eq(id,"tan"))  return fx_tan(arg, &s->err);
    if(id_eq(id,"sqrt")) return fx_sqrt(arg, &s->err);
    if(id_eq(id,"abs"))  return (arg<0) ? ((arg==I64_MIN)?(s->err=3,0):-arg) : arg;
    s->err=1; return 0;
}

static INT64 p_primary(eparse *s)
{
    skip_ws(s);
    if(*s->p=='('){
        s->p++;
        INT64 v = p_expr(s);
        skip_ws(s);
        if(*s->p==')') s->p++;
        else if(!s->err) s->err=1;
        return v;
    }
    if(is_digit(*s->p) || *s->p=='.') return p_number(s);
    if(is_alpha(*s->p)) return p_ident(s);
    s->err=1; return 0;
}

static INT64 p_unary(eparse *s);   /* fwd */

/* power: primary ('^' unary)? -- right associative, binds tighter than unary */
static INT64 p_pow(eparse *s)
{
    INT64 b = p_primary(s);
    if(s->err) return 0;
    skip_ws(s);
    if(*s->p!='^') return b;
    s->p++;
    INT64 e = p_unary(s);
    if(s->err) return 0;
    if(e & FX_FRAC){ s->err=5; return 0; }          /* non-integer exponent */
    INT64 n = e >> 32;
    if(n>1024 || n<-1024){ s->err=3; return 0; }
    return fx_powi(b, n, &s->err);
}

static INT64 p_unary(eparse *s)
{
    skip_ws(s);
    if(*s->p=='-'){
        s->p++;
        INT64 v = p_unary(s);
        if(s->err) return 0;
        if(v==I64_MIN){ s->err=3; return 0; }
        return -v;
    }
    if(*s->p=='+'){ s->p++; return p_unary(s); }
    return p_pow(s);
}

static INT64 p_term(eparse *s)
{
    INT64 v = p_unary(s);
    if(s->err) return 0;
    for(;;){
        skip_ws(s);
        char op = *s->p;
        if(op!='*' && op!='/' && op!='%') break;
        s->p++;
        INT64 r = p_unary(s);
        if(s->err) return 0;
        if(op=='*') v = fx_mul(v, r, &s->err);
        else if(op=='/') v = fx_div(v, r, &s->err);
        else v = fx_mod(v, r, &s->err);
        if(s->err) return 0;
    }
    return v;
}

static INT64 p_expr(eparse *s)
{
    INT64 v = p_term(s);
    if(s->err) return 0;
    for(;;){
        skip_ws(s);
        char op = *s->p;
        if(op!='+' && op!='-') break;
        s->p++;
        INT64 r = p_term(s);
        if(s->err) return 0;
        INT64 o;
        if(op=='+'){ if(__builtin_add_overflow(v, r, &o)){ s->err=3; return 0; } }
        else        { if(__builtin_sub_overflow(v, r, &o)){ s->err=3; return 0; } }
        v=o;
    }
    return v;
}

/* Evaluate `expr` at x=xval. Returns err code (0 ok); *uses_x may be NULL. */
static int calc_eval(const char *expr, INT64 xval, INT64 *out, int *uses_x)
{
    eparse s; s.p=expr; s.err=0; s.xval=xval; s.uses_x=0;
    skip_ws(&s);
    if(*s.p==0){ *out=0; if(uses_x)*uses_x=0; return 4; }       /* empty */
    INT64 v = p_expr(&s);
    skip_ws(&s);
    if(!s.err && *s.p!=0) s.err=1;                              /* trailing junk */
    *out = s.err ? 0 : v;
    if(uses_x) *uses_x = s.uses_x;
    return s.err;
}

static const char *err_text(int e)
{
    if(e==2) return "ERR: /0";
    if(e==3) return "ERR: overflow";
    if(e==5) return "ERR: domain";
    return "ERR: syntax";
}

/* ------------------------------------------------------------------ */
/*  Tool state                                                         */
/* ------------------------------------------------------------------ */
#define CALC_EXPR_MAX 128

typedef struct {
    wm_window *win;
    char expr[CALC_EXPR_MAX];   /* editable expression string                */
    int  just_eval;             /* expr currently holds a committed result   */
    char status[32];            /* error message (persist until next input)  */
    int  b_hover, b_press;      /* hovered / pressed button id (1..COUNT)    */
} calcstate;

static calcstate g_calc;

/* Button labels, laid out 5 columns x 6 rows. id = index+1. */
static const char *const CALC_LBL[30] = {
    "C","(",")","%","/",
    "7","8","9","*","sqrt",
    "4","5","6","-","sin",
    "1","2","3","+","cos",
    "+/-","0",".","x","tan",
    "pi","e","^","=","GRA"
};
#define CALC_COLS 5
#define CALC_ROWS 6
#define CALC_NBTN 30

/* ------------------------------------------------------------------ */
/*  Input handling                                                     */
/* ------------------------------------------------------------------ */
void tool_calc_graph_open(void);   /* fwd (graph window, below) */

static void calc_append(char c)
{
    g_calc.status[0]=0;
    if(g_calc.just_eval){
        /* After '=': a digit, '.' or '(' starts a fresh expression; an
         * operator continues from the previous result. */
        if(is_digit(c) || c=='(' || c=='.'){ g_calc.expr[0]=0; }
        g_calc.just_eval=0;
    }
    int n=cslen(g_calc.expr);
    if(n < CALC_EXPR_MAX-1){ g_calc.expr[n]=c; g_calc.expr[n+1]=0; }
}

static void calc_append_str(const char *s){ while(*s) calc_append(*s++); }

static void calc_clear(void)
{
    g_calc.expr[0]=0; g_calc.status[0]=0; g_calc.just_eval=0;
}

static void calc_backspace(void)
{
    g_calc.status[0]=0; g_calc.just_eval=0;
    int n=cslen(g_calc.expr);
    if(n>0) g_calc.expr[n-1]=0;
}

/* Negate the last numeric literal (or the whole result after '='). */
static void calc_negate(void)
{
    g_calc.status[0]=0;
    int n=cslen(g_calc.expr);
    if(n==0){ /* nothing typed: start a negative */ calc_append('-'); return; }
    /* find start of the trailing run of digits/dots */
    int end=n;
    if(!is_digit(g_calc.expr[end-1]) && g_calc.expr[end-1]!='.'){ calc_append('-'); return; }
    int start=end;
    while(start>0 && (is_digit(g_calc.expr[start-1]) || g_calc.expr[start-1]=='.')) start--;
    /* is it already negated by a unary '-' ? */
    int has_minus = (start>0 && g_calc.expr[start-1]=='-' &&
                     (start-1==0 || !is_digit(g_calc.expr[start-2])) &&
                     (start-1==0 || g_calc.expr[start-2]!=')'));
    if(has_minus){
        /* remove the '-' at start-1 */
        for(int i=start-1;i<n;i++) g_calc.expr[i]=g_calc.expr[i+1];
    } else {
        if(n>=CALC_EXPR_MAX-1) return;
        /* insert '-' before start */
        for(int i=n;i>=start;i--) g_calc.expr[i+1]=g_calc.expr[i];
        g_calc.expr[start]='-';
    }
    g_calc.just_eval=0;
}

/* tiny local: copy an error string into g_calc.status */
static void scopyd(const char *s)
{
    int i=0; while(s[i] && i<(int)sizeof(g_calc.status)-1){ g_calc.status[i]=s[i]; i++; }
    g_calc.status[i]=0;
}

static void calc_equals(void)
{
    INT64 v; int ux;
    int e=calc_eval(g_calc.expr, 0, &v, &ux);
    if(e==4){ return; }                             /* empty: ignore */
    if(e){ scopyd(err_text(e)); return; }
    /* success: commit the result into expr as the new operand */
    fx_toa(v, g_calc.expr);
    g_calc.just_eval=1;
    g_calc.status[0]=0;
}

/* Apply a button id (1..CALC_NBTN). */
static void calc_do_button(int id)
{
    if(id<1 || id>CALC_NBTN) return;
    const char *lb = CALC_LBL[id-1];
    if(lb[0]=='C' && lb[1]==0){ calc_clear(); return; }
    if(lb[0]=='=' && lb[1]==0){ calc_equals(); return; }
    if(lb[0]=='+' && lb[1]=='/'){ calc_negate(); return; }   /* "+/-" */
    if(lb[0]=='G'){ tool_calc_graph_open(); return; }        /* "GRA" */
    if(lb[0]=='s' || lb[0]=='c' || lb[0]=='t'){              /* sin cos sqrt tan */
        calc_append_str(lb); calc_append('('); return;
    }
    calc_append_str(lb);   /* digits, ops, parens, '.', '%', '^', x, pi, e */
}

/* ------------------------------------------------------------------ */
/*  Geometry (shared by draw + hit-test so they never disagree)        */
/* ------------------------------------------------------------------ */
#define CALC_PAD   8
#define CALC_GAP   6
#define CALC_DISPH 60          /* display panel height (client px) */

/* Grid geometry shared by all 30 buttons for a given (cw,ch); computed once
 * per hit-test / redraw instead of redoing the same divisions per button. */
typedef struct { int gx, gy, cellw, cellh; } calc_grid;

static calc_grid calc_grid_geo(int cw, int ch)
{
    calc_grid r;
    r.gx = CALC_PAD;
    r.gy = CALC_PAD + CALC_DISPH + CALC_PAD;
    int gw = cw - 2*CALC_PAD;
    int gh = ch - r.gy - CALC_PAD;
    if(gw<CALC_COLS) gw=CALC_COLS;
    if(gh<CALC_ROWS) gh=CALC_ROWS;
    r.cellw = gw / CALC_COLS;
    r.cellh = gh / CALC_ROWS;
    return r;
}

/* Fill the client rect of button `idx` (0..29) in CLIENT coords, given the
 * grid geometry already computed by calc_grid_geo(). */
static void calc_btn_rect(const calc_grid *g, int idx, int *rx, int *ry, int *rw, int *rh)
{
    int col = idx % CALC_COLS;
    int row = idx / CALC_COLS;
    *rx = g->gx + col*g->cellw;
    *ry = g->gy + row*g->cellh;
    *rw = g->cellw - CALC_GAP;
    *rh = g->cellh - CALC_GAP;
    if(*rw<1) *rw=1; if(*rh<1) *rh=1;
}

/* Return the button id (1..30) at client point (mx,my), or 0. */
static int calc_hit(int cw, int ch, int mx, int my)
{
    calc_grid g = calc_grid_geo(cw, ch);
    for(int i=0;i<CALC_NBTN;i++){
        int x,y,w,h; calc_btn_rect(&g,i,&x,&y,&w,&h);
        if(mx>=x && mx<x+w && my>=y && my<y+h) return i+1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Compiled-expression cache engine (defined further down). Forward-  */
/*  declared here so calc_draw's live preview can reuse the parsed tree */
/*  instead of re-tokenizing on every mouse-move redraw.               */
/* ------------------------------------------------------------------ */
static void  ecompile(const char *expr);
static INT64 eeval(int idx, INT64 xval, int *err);
static int   g_eroot;           /* root node index, -1 if none   (tentative) */
static int   g_ecompile_err;    /* 0 ok, else 1(syntax)/4(empty) (tentative) */
static int   g_ecache_usesx;    /* set by ecompile: expr references x         */

/* ------------------------------------------------------------------ */
/*  Draw                                                               */
/* ------------------------------------------------------------------ */
static void calc_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    calcstate *c=(calcstate*)wm_user(w);
    if(!c) return;

    UINT32 bg     = wm_theme_color(WM_COL_WINDOW);
    UINT32 fg     = wm_theme_color(WM_COL_FG);
    UINT32 accent = wm_theme_color(WM_COL_ACCENT);
    UINT32 selbg  = wm_theme_color(WM_COL_SEL_BG);
    UINT32 selfg  = wm_theme_color(WM_COL_SEL_FG);
    UINT32 panel  = wm_blend(bg, 0x00000000u, 96);   /* darker display face */
    UINT32 btnbg  = wm_blend(bg, fg, 26);
    UINT32 dim    = wm_blend(fg, bg, 128);

    /* client background */
    fill_rect(cx, cy, cw, ch, bg);

    /* ---- display panel ---- */
    int dx=cx+CALC_PAD, dy=cy+CALC_PAD, dw=cw-2*CALC_PAD, dh=CALC_DISPH;
    fill_rect(dx, dy, dw, dh, panel);
    draw_rect_outline(dx, dy, dw, dh, 1, wm_blend(bg, fg, 60));

    /* expression (large), clipped so it never runs off the window */
    const char *shown = (cslen(c->expr)>0) ? c->expr : "0";
    draw_string_clip(dx+8, dy+8, dw-16, shown, fg, panel, 1, 2);

    /* second line: live preview "= N", a hint when f(x), or the error */
    if(c->status[0]){
        draw_string_clip(dx+8, dy+8+18*2, dw-16, c->status, 0x00FF5A5Au, panel, 1, 1);
    } else if(!c->just_eval && cslen(c->expr)>0){
        /* Reuse the shared compiled tree (a no-op string compare when the
         * expression text is unchanged) so hovering the keypad doesn't
         * re-tokenize the whole expression on every mouse-move redraw. */
        ecompile(c->expr);
        int e=g_ecompile_err; INT64 v = e ? 0 : eeval(g_eroot, 0, &e);
        if(e==0 && g_ecache_usesx){
            draw_string_clip(dx+8, dy+8+18*2, dw-16, "f(x): GRA plots y=f(x)", dim, panel, 1, 1);
        } else if(e==0){
            char pv[32]; pv[0]='='; pv[1]=' ';
            fx_toa(v, pv+2);
            draw_string_clip(dx+8, dy+8+18*2, dw-16, pv, dim, panel, 1, 1);
        }
    }

    /* ---- button grid ---- */
    calc_grid grid = calc_grid_geo(cw, ch);
    for(int i=0;i<CALC_NBTN;i++){
        int x,y,bw,bh; calc_btn_rect(&grid,i,&x,&y,&bw,&bh);
        int sx=cx+x, sy=cy+y;
        int id=i+1;
        int hover = (c->b_hover==id);
        int press = (c->b_press==id);
        const char *lb = CALC_LBL[i];

        UINT32 face = btnbg;
        UINT32 txt  = fg;
        /* operators + special keys tinted with the accent */
        int is_op = (lb[1]==0 && (lb[0]=='+'||lb[0]=='-'||lb[0]=='*'||
                                  lb[0]=='/'||lb[0]=='%'||lb[0]=='^'||
                                  lb[0]=='('||lb[0]==')'));
        int is_fn = (lb[0]=='s'||lb[0]=='c'||lb[0]=='t'||lb[0]=='p'||
                     (lb[0]=='x'&&lb[1]==0)||(lb[0]=='e'&&lb[1]==0));
        int is_eq = (lb[0]=='='&&lb[1]==0);
        int is_c  = (lb[0]=='C'&&lb[1]==0);
        int is_g  = (lb[0]=='G');
        if(is_eq){ face=accent; txt=selfg; }
        else if(is_c){ face=wm_blend(btnbg, 0x00C83232u, 90); }
        else if(is_g){ face=wm_blend(btnbg, selbg, 110); }
        else if(is_op){ face=wm_blend(btnbg, accent, 70); }
        else if(is_fn){ face=wm_blend(btnbg, accent, 40); }

        if(press){ face=wm_blend(face, 0x00000000u, 80); }
        else if(hover){ face=wm_blend(face, selbg, 90); }

        fill_rect(sx, sy, bw, bh, face);
        if(hover||press) draw_rect_outline(sx, sy, bw, bh, 1, accent);

        /* centered label, clipped to the button width */
        int tscale = 2;
        int tw = cslen(lb)*8*tscale;
        if(tw > bw-4){ tscale=1; tw=cslen(lb)*8*tscale; }
        int tx = sx + (bw - tw)/2;   if(tx<sx) tx=sx;
        int tyy = sy + (bh - 16*tscale)/2; if(tyy<sy) tyy=sy;
        draw_string_clip(tx, tyy, bw, lb, txt, face, 1, tscale);
    }

    (void)selfg;
}

/* ------------------------------------------------------------------ */
/*  Events                                                             */
/* ------------------------------------------------------------------ */
static int calc_event(wm_window *w, const wm_event *ev)
{
    calcstate *c=(calcstate*)wm_user(w);
    if(!c) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);

    switch(ev->type){
        case WM_EV_KEY: {
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            CHAR16 u = ev->unicode;
            if(u==CHAR_CR || u==CHAR_LINEFEED || u=='='){ calc_equals(); return 0; }
            if(u==CHAR_BACKSPACE){ calc_backspace(); return 0; }
            if(u=='C'){ calc_clear(); return 0; }              /* shift-C clears */
            if(u=='g' || u=='G'){ tool_calc_graph_open(); return 0; }
            if((u>='0'&&u<='9')||u=='+'||u=='-'||u=='*'||u=='/'||
               u=='%'||u=='('||u==')'||u=='.'||u=='^'){ calc_append((char)u); return 0; }
            if(u<128 && is_alpha((char)u)){ calc_append(to_lower((char)u)); return 0; }
            return 0;
        }
        case WM_EV_MOUSE_MOVE:
            c->b_hover = calc_hit(cw,ch,ev->mx,ev->my);
            return 0;
        case WM_EV_MOUSE_DOWN: {
            int id=calc_hit(cw,ch,ev->mx,ev->my);
            if(id) c->b_press=id;
            return 0;
        }
        case WM_EV_MOUSE_UP: {
            if(!c->b_press) return 0;
            int id=calc_hit(cw,ch,ev->mx,ev->my), p=c->b_press;
            c->b_press=0;
            if(id==p) calc_do_button(p);
            return 0;
        }
        case WM_EV_CLOSE:
            c->win=NULL;
            return 0;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Open (TEMPLATE B)                                                  */
/* ------------------------------------------------------------------ */
void tool_calc_open(void)
{
    if(g_calc.win) return;                 /* already open */
    /* keep any expression (shared with the graph); reset transient state */
    g_calc.status[0]=0;
    g_calc.just_eval=0; g_calc.b_hover=0; g_calc.b_press=0;

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*44/100; if(ww<340)ww=340; if(ww>460)ww=460; if(ww>W-40)ww=W-40;
    int wh=H*64/100; if(wh<400)wh=400; if(wh>560)wh=560; if(wh>H-40)wh=H-40;
    g_calc.win=wm_open("Calculator", ww, wh, calc_draw, calc_event, &g_calc);
}

/* =============================================================================
 *  Function grapher - plots y = f(x) for the calculator's shared expression
 * ============================================================================= */
#define GRAPH_HUD   34          /* top band height (2 text lines)        */
#define GRAPH_MINPX 42          /* min grid spacing in pixels            */

typedef struct {
    wm_window *win;
    INT64 vx, vy;               /* view centre (fixed-point coords)      */
    INT64 scale;                /* fixed units per pixel; 0 = auto-init  */
    int   trace;                /* show cursor coordinates in the HUD    */
    int   tmx, tmy;             /* last cursor position (client coords)  */
} graphstate;

static graphstate g_graph;

/* ---- client-clipped plot primitives (y clipped below the HUD band) ---- */
static void gpx(int cx, int cy, int cw, int ch, int x, int y, UINT32 c)
{
    if((unsigned)x < (unsigned)cw && y>=GRAPH_HUD && y<ch)
        put_pixel(cx+x, cy+y, c);
}
static void gline(int cx, int cy, int cw, int ch, int x0, int y0, int x1, int y1, UINT32 c)
{
    int dx = x1-x0, dy = y1-y0;
    int ax = dx<0?-dx:dx, ay = dy<0?-dy:dy;
    int sx = dx<0?-1:1,     sy = dy<0?-1:1;
    int err = (ax>ay?ax:-ay)/2, e2;
    int guard = ax + ay + 4;
    for(;;){
        gpx(cx,cy,cw,ch,x0,y0,c);
        if(x0==x1 && y0==y1) break;
        if(--guard<0) break;
        e2=err;
        if(e2>-ax){ err-=ay; x0+=sx; }
        if(e2< ay){ err+=ax; y0+=sy; }
    }
}

/* smallest m*10^e (m in 1,2,5) whose on-screen spacing is >= GRAPH_MINPX */
static INT64 graph_step(INT64 scale)
{
    INT64 base = FX_ONE;
    for(int i=0;i<9;i++) base /= 10;    /* base = 10^-9 in fixed point (e=-9) */
    for(int e=-9; e<=9; e++){
        if(base>0){
            static const int m[3] = {1,2,5};
            for(int k=0;k<3;k++){
                INT64 st = base*m[k];
                if(st/scale >= GRAPH_MINPX) return st;
            }
        }
        base *= 10;                     /* carry 10^e forward to 10^(e+1) */
    }
    return FX_INT(1000000000LL);
}

/* screen x of graph x (column of vertical line at x); centre-safe. */
static int graph_px(const graphstate *g, INT64 x, int cw)
{
    INT64 q = (x - g->vx) / g->scale + cw/2;
    if(q < -1073741824LL) q = -1073741824LL;
    if(q >  1073741824LL) q =  1073741824LL;
    return (int)q;
}
/* graph x at screen column px (128-bit intermediate, bounded result). */
static INT64 graph_x_at(const graphstate *g, int px, int cw)
{
    return g->vx + (INT64)((__int128)(px - cw/2) * g->scale);
}
/* screen y (plot-relative) of graph y, clamped so huge values stay sane. */
static int graph_sy(const graphstate *g, INT64 y, int ph)
{
    __int128 d = (__int128)y - g->vy;
    int neg = (d<0);
    if(neg) d = -d;
    __int128 lim = (__int128)g->scale * (2*ph + 8);
    INT64 q = (d > lim) ? (INT64)(2*ph + 9) : (INT64)d / g->scale;
    return neg ? (ph/2 + (int)q) : (ph/2 - (int)q);
}

static void graph_clamp_view(graphstate *g)
{
    const INT64 smax = FX_INT(1000000);
    const INT64 clim = FX_INT(1000000000LL);
    if(g->scale > smax) g->scale = smax;
    if(g->scale < 1)    g->scale = 1;
    if(g->vx >  clim) g->vx =  clim;
    if(g->vx < -clim) g->vx = -clim;
    if(g->vy >  clim) g->vy =  clim;
    if(g->vy < -clim) g->vy = -clim;
}

/* ------------------------------------------------------------------ */
/*  Pre-parsed expression AST -- used only by the curve loop below.    */
/*  The curve loop samples calc_eval() up to `cw` (~hundreds) times per */
/*  redraw with the SAME expression text and only `x` changing. Rather */
/*  than re-tokenizing/re-parsing the source string from scratch for   */
/*  every sample, ecompile() parses it once into a small fixed-size    */
/*  node pool (static storage, no heap) mirroring the grammar above,   */
/*  and eeval() walks that tree per-sample. Structure/parse errors are */
/*  purely a function of the text (never of x), so building the tree   */
/*  once is behavior-identical to re-parsing every time; value errors  */
/*  (/0, overflow, domain) still depend on x and are re-checked by     */
/*  eeval() on every call, in the same order the original recursive-   */
/*  descent evaluator would have produced them.                        */
/* ------------------------------------------------------------------ */
enum {
    EN_CONST, EN_X, EN_ADD, EN_SUB, EN_MUL, EN_DIV, EN_MOD, EN_NEG, EN_POW,
    EN_SIN, EN_COS, EN_TAN, EN_SQRT, EN_ABS
};

typedef struct { INT64 val; int a, b; unsigned char type; } enode;

#define ENODE_MAX (CALC_EXPR_MAX*4)
static enode g_epool[ENODE_MAX];
static int   g_epool_n;
static char  g_ecache_expr[CALC_EXPR_MAX];  /* text the pool was built from */
static int   g_eroot;                       /* root node index, -1 if none  */
static int   g_ecompile_err;                /* 0 ok, else 1(syntax)/4(empty)*/

typedef struct { const char *p; int err; int uses_x; } ebuild;

static void eb_skip_ws(ebuild *s){ while(*s->p==' ') s->p++; }

static int eb_expr(ebuild *s);   /* fwd */
static int eb_unary(ebuild *s);  /* fwd */

static int enode_new(unsigned char type, INT64 val, int a, int b)
{
    if(g_epool_n >= ENODE_MAX) return -1;
    int idx = g_epool_n++;
    g_epool[idx].type=type; g_epool[idx].val=val; g_epool[idx].a=a; g_epool[idx].b=b;
    return idx;
}

static int eb_number(ebuild *s)
{
    UINT64 ip=0; int ovf=0;
    while(is_digit(*s->p)){
        if(ip > 300000000ULL) ovf=1; else ip = ip*10ULL + (UINT64)(*s->p-'0');
        s->p++;
    }
    UINT64 fr=0, fs=1;
    if(*s->p=='.'){
        s->p++;
        while(is_digit(*s->p)){
            if(fs <= 100000000ULL){ fr = fr*10ULL + (UINT64)(*s->p-'0'); fs *= 10ULL; }
            s->p++;
        }
    }
    if(ovf || ip >= 2147483648ULL){ s->err=3; return -1; }
    INT64 v = (INT64)(ip << 32);
    if(fs>1) v |= (INT64)((fr << 32) / fs);
    int idx = enode_new(EN_CONST, v, -1, -1);
    if(idx<0){ s->err=3; return -1; }
    return idx;
}

static int eb_ident(ebuild *s)
{
    char id[6]; int n=0;
    while(is_alpha(*s->p)){
        if(n<5) id[n]=to_lower(*s->p);
        n++; s->p++;
    }
    if(n==0 || n>5){ s->err=1; return -1; }
    id[n]=0;
    if(id_eq(id,"x")){ s->uses_x=1; int idx=enode_new(EN_X,0,-1,-1); if(idx<0){s->err=3; return -1;} return idx; }
    if(id_eq(id,"pi")){ int idx=enode_new(EN_CONST,FX_PI,-1,-1); if(idx<0){s->err=3; return -1;} return idx; }
    if(id_eq(id,"e")) { int idx=enode_new(EN_CONST,FX_E,-1,-1);  if(idx<0){s->err=3; return -1;} return idx; }
    /* otherwise a function call: name '(' expr ')' */
    eb_skip_ws(s);
    if(*s->p!='('){ s->err=1; return -1; }
    s->p++;
    int arg = eb_expr(s);
    eb_skip_ws(s);
    if(*s->p==')') s->p++;
    else if(!s->err) s->err=1;
    if(s->err) return -1;
    unsigned char t;
    if(id_eq(id,"sin"))       t=EN_SIN;
    else if(id_eq(id,"cos"))  t=EN_COS;
    else if(id_eq(id,"tan"))  t=EN_TAN;
    else if(id_eq(id,"sqrt")) t=EN_SQRT;
    else if(id_eq(id,"abs"))  t=EN_ABS;
    else { s->err=1; return -1; }
    int idx = enode_new(t, 0, arg, -1);
    if(idx<0){ s->err=3; return -1; }
    return idx;
}

static int eb_primary(ebuild *s)
{
    eb_skip_ws(s);
    if(*s->p=='('){
        s->p++;
        int v = eb_expr(s);
        eb_skip_ws(s);
        if(*s->p==')') s->p++;
        else if(!s->err) s->err=1;
        return v;
    }
    if(is_digit(*s->p) || *s->p=='.') return eb_number(s);
    if(is_alpha(*s->p)) return eb_ident(s);
    s->err=1; return -1;
}

/* power: primary ('^' unary)? -- right associative, binds tighter than unary.
 * The exponent may itself reference x (e.g. "2^x"), so unlike the original
 * eager evaluator the integer/range check on it can't happen here at parse
 * time -- it is deferred to EN_POW in eeval(), which runs per-x. */
static int eb_pow(ebuild *s)
{
    int b = eb_primary(s);
    if(s->err) return -1;
    eb_skip_ws(s);
    if(*s->p!='^') return b;
    s->p++;
    int e = eb_unary(s);
    if(s->err) return -1;
    int idx = enode_new(EN_POW, 0, b, e);
    if(idx<0){ s->err=3; return -1; }
    return idx;
}

static int eb_unary(ebuild *s)
{
    eb_skip_ws(s);
    if(*s->p=='-'){
        s->p++;
        int v = eb_unary(s);
        if(s->err) return -1;
        int idx = enode_new(EN_NEG, 0, v, -1);
        if(idx<0){ s->err=3; return -1; }
        return idx;
    }
    if(*s->p=='+'){ s->p++; return eb_unary(s); }
    return eb_pow(s);
}

static int eb_term(ebuild *s)
{
    int v = eb_unary(s);
    if(s->err) return -1;
    for(;;){
        eb_skip_ws(s);
        char op = *s->p;
        if(op!='*' && op!='/' && op!='%') break;
        s->p++;
        int r = eb_unary(s);
        if(s->err) return -1;
        unsigned char t = (op=='*') ? EN_MUL : (op=='/') ? EN_DIV : EN_MOD;
        int idx = enode_new(t, 0, v, r);
        if(idx<0){ s->err=3; return -1; }
        v = idx;
    }
    return v;
}

static int eb_expr(ebuild *s)
{
    int v = eb_term(s);
    if(s->err) return -1;
    for(;;){
        eb_skip_ws(s);
        char op = *s->p;
        if(op!='+' && op!='-') break;
        s->p++;
        int r = eb_term(s);
        if(s->err) return -1;
        int idx = enode_new(op=='+' ? EN_ADD : EN_SUB, 0, v, r);
        if(idx<0){ s->err=3; return -1; }
        v = idx;
    }
    return v;
}

static int estreq(const char *a, const char *b)
{
    int i=0; for(;;i++){ if(a[i]!=b[i]) return 0; if(a[i]==0) return 1; }
}

/* Parse `expr` into the static node pool if it isn't already cached there. */
static void ecompile(const char *expr)
{
    if(estreq(expr, g_ecache_expr)) return;   /* already compiled */
    g_epool_n = 0;
    ebuild s; s.p=expr; s.err=0; s.uses_x=0;
    eb_skip_ws(&s);
    if(*s.p==0){
        g_eroot=-1; g_ecompile_err=4; g_ecache_usesx=0;
    } else {
        int root = eb_expr(&s);
        eb_skip_ws(&s);
        if(!s.err && *s.p!=0) s.err=1;         /* trailing junk */
        g_eroot = s.err ? -1 : root;
        g_ecompile_err = s.err;
        g_ecache_usesx = s.err ? 0 : s.uses_x;
    }
    int i=0; while(expr[i] && i<CALC_EXPR_MAX-1){ g_ecache_expr[i]=expr[i]; i++; }
    g_ecache_expr[i]=0;
}

/* Evaluate the pre-parsed tree at x=xval; mirrors calc_eval()'s per-op error
 * codes and short-circuit order exactly. */
static INT64 eeval(int idx, INT64 xval, int *err)
{
    if(idx<0){ *err=1; return 0; }
    enode *nd = &g_epool[idx];
    switch(nd->type){
        case EN_CONST: return nd->val;
        case EN_X:     return xval;
        case EN_ADD: {
            INT64 a=eeval(nd->a,xval,err); if(*err) return 0;
            INT64 b=eeval(nd->b,xval,err); if(*err) return 0;
            INT64 o; if(__builtin_add_overflow(a,b,&o)){ *err=3; return 0; }
            return o;
        }
        case EN_SUB: {
            INT64 a=eeval(nd->a,xval,err); if(*err) return 0;
            INT64 b=eeval(nd->b,xval,err); if(*err) return 0;
            INT64 o; if(__builtin_sub_overflow(a,b,&o)){ *err=3; return 0; }
            return o;
        }
        case EN_MUL: {
            INT64 a=eeval(nd->a,xval,err); if(*err) return 0;
            INT64 b=eeval(nd->b,xval,err); if(*err) return 0;
            return fx_mul(a,b,err);
        }
        case EN_DIV: {
            INT64 a=eeval(nd->a,xval,err); if(*err) return 0;
            INT64 b=eeval(nd->b,xval,err); if(*err) return 0;
            return fx_div(a,b,err);
        }
        case EN_MOD: {
            INT64 a=eeval(nd->a,xval,err); if(*err) return 0;
            INT64 b=eeval(nd->b,xval,err); if(*err) return 0;
            return fx_mod(a,b,err);
        }
        case EN_NEG: {
            INT64 v=eeval(nd->a,xval,err); if(*err) return 0;
            if(v==I64_MIN){ *err=3; return 0; }
            return -v;
        }
        case EN_POW: {
            INT64 b=eeval(nd->a,xval,err); if(*err) return 0;
            INT64 e=eeval(nd->b,xval,err); if(*err) return 0;
            if(e & FX_FRAC){ *err=5; return 0; }         /* non-integer exponent */
            INT64 n = e >> 32;
            if(n>1024 || n<-1024){ *err=3; return 0; }
            return fx_powi(b, n, err);
        }
        case EN_SIN:  { INT64 a=eeval(nd->a,xval,err); if(*err) return 0; return fx_sin(a,err); }
        case EN_COS:  { INT64 a=eeval(nd->a,xval,err); if(*err) return 0; return fx_cos(a,err); }
        case EN_TAN:  { INT64 a=eeval(nd->a,xval,err); if(*err) return 0; return fx_tan(a,err); }
        case EN_SQRT: { INT64 a=eeval(nd->a,xval,err); if(*err) return 0; return fx_sqrt(a,err); }
        case EN_ABS: {
            INT64 a=eeval(nd->a,xval,err); if(*err) return 0;
            if(a<0){ if(a==I64_MIN){ *err=3; return 0; } return -a; }
            return a;
        }
        default: *err=1; return 0;
    }
}

static void graph_draw(wm_window *w, int cx, int cy, int cw, int ch)
{
    graphstate *g=(graphstate*)wm_user(w);
    if(!g) return;
    UINT32 fg     = wm_theme_color(WM_COL_FG);
    UINT32 accent = wm_theme_color(WM_COL_ACCENT);
    UINT32 dim    = wm_blend(fg, 0x00000000u, 150);
    UINT32 gridc  = wm_blend(0x00000000u, fg, 36);
    UINT32 axisc  = wm_blend(0x00000000u, fg, 96);

    fill_rect(cx, cy, cw, ch, 0x00000000u);
    if(g->scale<=0){                       /* first draw: fit x in [-10,10] */
        g->scale = FX_INT(20) / (cw>0?cw:1);
        if(g->scale<=0) g->scale=1;
    }
    int ph = ch - GRAPH_HUD;
    if(cw<16 || ph<16) return;

    /* ---- grid + axes + tick labels ---- */
    INT64 st = graph_step(g->scale);
    INT64 x0 = graph_x_at(g, 0, cw), x1 = graph_x_at(g, cw, cw);
    INT64 y0 = g->vy - (INT64)((__int128)(ph - ph/2) * g->scale);  /* bottom */
    INT64 y1 = g->vy + (INT64)((__int128)(ph/2) * g->scale);       /* top    */

    /* axis position (clamped so labels stay on screen) */
    INT64 axy64 = GRAPH_HUD + ph/2 - g->vy / g->scale;
    if(axy64 < GRAPH_HUD+2) axy64 = GRAPH_HUD+2;
    if(axy64 > ch-10) axy64 = ch-10;
    int axy = (int)axy64;
    int axx = graph_px(g, 0, cw);
    if(axx < 2) axx = 2;
    if(axx > cw-42) axx = cw-42;

    INT64 n0 = x0/st; if(x0<0 && x0%st) n0--;
    INT64 n1 = x1/st; if(x1>0 && x1%st) n1++;
    if(n1-n0 > 4096) n1 = n0 + 4096;      /* paranoid bound */
    for(INT64 n=n0; n<=n1; n++){
        INT64 xv = n*st;
        int px = graph_px(g, xv, cw);
        if(px<0 || px>=cw) continue;
        UINT32 cc = (n==0) ? axisc : gridc;
        for(int yy=GRAPH_HUD; yy<ch; yy++) put_pixel(cx+px, cy+yy, cc);
        if(n!=0){
            char lb[26]; fx_toa(xv, lb);
            draw_string_clip(cx+px+2, cy+axy+2, 40, lb, dim, 0, 1, 1);
        }
    }
    n0 = y0/st; if(y0<0 && y0%st) n0--;
    n1 = y1/st; if(y1>0 && y1%st) n1++;
    if(n1-n0 > 4096) n1 = n0 + 4096;
    for(INT64 n=n0; n<=n1; n++){
        INT64 yv = n*st;
        int py = GRAPH_HUD + ph/2 - (int)((yv - g->vy) / g->scale);
        if(py<GRAPH_HUD || py>=ch) continue;
        UINT32 cc = (n==0) ? axisc : gridc;
        draw_hline(cx, cy+py, cw, cc);
        if(n!=0){
            char lb[26]; fx_toa(yv, lb);
            draw_string_clip(cx+axx+2, cy+py-8, 40, lb, dim, 0, 1, 1);
        }
    }

    /* ---- curve y = f(x) ---- */
    int ux=0; INT64 dummy;
    int expr_err = calc_eval(g_calc.expr, 0, &dummy, &ux);
    if(g_calc.expr[0] && expr_err!=1){
        /* Parse the expression once (cached across redraws while the text is
         * unchanged) instead of re-tokenizing it from scratch for every one
         * of the up to `cw` x-samples below. */
        ecompile(g_calc.expr);
        int have=0, prevx=0, prevy=0;
        for(int px=0; px<cw && !g_ecompile_err; px++){
            INT64 xf = graph_x_at(g, px, cw);
            INT64 y; int yerr=0;
            y = eeval(g_eroot, xf, &yerr);
            if(yerr){ have=0; continue; }
            int sy = GRAPH_HUD + graph_sy(g, y, ph);
            if(have && (sy-prevy > 4*ph || prevy-sy > 4*ph)) have=0; /* asymptote */
            if(have) gline(cx, cy, cw, ch, prevx, prevy, px, sy, accent);
            prevx=px; prevy=sy; have=1;
        }
    }

    /* ---- HUD band ---- */
    fill_rect(cx, cy, cw, GRAPH_HUD, 0x00101018u);
    draw_hline(cx, cy+GRAPH_HUD-1, cw, wm_blend(0x00101018u, fg, 60));
    char hb[CALC_EXPR_MAX+8];
    int p=0; const char *pre = "y = ";
    while(pre[p]){ hb[p]=pre[p]; p++; }
    const char *ex = g_calc.expr[0] ? g_calc.expr : "(type an expression, use x)";
    int q=0; while(ex[q] && p<(int)sizeof(hb)-1) hb[p++]=ex[q++];
    hb[p]=0;
    draw_string_clip(cx+4, cy+3, cw-8, hb, fg, 0x00101018u, 1, 1);

    if(g->trace && g->tmy>=GRAPH_HUD){
        INT64 xf = graph_x_at(g, g->tmx, cw);
        INT64 yv; int e = calc_eval(g_calc.expr, xf, &yv, 0);
        char tb[72]; int t=0;
        const char *xs="x="; while(*xs) tb[t++]=*xs++;
        fx_toa(xf, tb+t); t+=cslen(tb+t);
        tb[t++]=' '; tb[t++]='y'; tb[t++]='=';
        if(e){ const char *et="ERR"; while(*et) tb[t++]=*et++; }
        else { fx_toa(yv, tb+t); t+=cslen(tb+t); }
        tb[t]=0;
        draw_string_clip(cx+4, cy+19, cw-8, tb, accent, 0x00101018u, 1, 1);
    } else {
        const char *h = expr_err==1 ? "ERR: syntax"
                      : "arrows pan  PgUp/PgDn/wheel zoom  click centre  R reset  Esc close";
        draw_string_clip(cx+4, cy+19, cw-8, h,
                         expr_err==1 ? 0x00FF5A5Au : dim, 0x00101018u, 1, 1);
    }
}

static void graph_zoom(graphstate *g, int mx, int my, int cw, int ph, int in)
{
    INT64 so = g->scale;
    INT64 sn = in ? (so*4/5) : (so*5/4);
    if(sn<1) sn=1;
    if(sn>FX_INT(1000000)) sn=FX_INT(1000000);
    if(sn==so) return;
    g->vx += (INT64)((__int128)(mx - cw/2) * (so - sn));
    g->vy -= (INT64)((__int128)(my - GRAPH_HUD - ph/2) * (so - sn));
    g->scale = sn;
    graph_clamp_view(g);
}

static int graph_event(wm_window *w, const wm_event *ev)
{
    graphstate *g=(graphstate*)wm_user(w);
    if(!g) return 0;
    int cw=wm_client_w(w), ch=wm_client_h(w);
    int ph=ch-GRAPH_HUD;
    INT64 panx = (INT64)(cw/8>0?cw/8:1) * (g->scale>0?g->scale:1);
    INT64 pany = (INT64)(ph/8>0?ph/8:1) * (g->scale>0?g->scale:1);

    switch(ev->type){
        case WM_EV_CLOSE:
            g->win=NULL; return 0;
        case WM_EV_KEY: {
            if(ev->scancode==SCAN_ESC) return WM_CLOSE_REQUEST;
            CHAR16 u = ev->unicode;
            switch(ev->scancode){
                case SCAN_LEFT:      g->vx -= panx; graph_clamp_view(g); return 0;
                case SCAN_RIGHT:     g->vx += panx; graph_clamp_view(g); return 0;
                case SCAN_UP:        g->vy += pany; graph_clamp_view(g); return 0;
                case SCAN_DOWN:      g->vy -= pany; graph_clamp_view(g); return 0;
                case SCAN_PAGE_UP:   graph_zoom(g, cw/2, GRAPH_HUD+ph/2, cw, ph, 1); return 0;
                case SCAN_PAGE_DOWN: graph_zoom(g, cw/2, GRAPH_HUD+ph/2, cw, ph, 0); return 0;
                default: break;
            }
            /* shared expression editing (plot updates live) */
            if(u=='R'){ g->vx=0; g->vy=0; g->scale=0; return 0; }  /* shift-R */
            if(u=='g' || u=='G'){ return WM_CLOSE_REQUEST; }   /* toggle */
            if(u=='C'){ calc_clear(); return 0; }
            if(u==CHAR_BACKSPACE){ calc_backspace(); return 0; }
            if((u>='0'&&u<='9')||u=='+'||u=='-'||u=='*'||u=='/'||
               u=='%'||u=='('||u==')'||u=='.'||u=='^'){ calc_append((char)u); return 0; }
            if(u<128 && is_alpha((char)u)){ calc_append(to_lower((char)u)); return 0; }
            return 0;
        }
        case WM_EV_MOUSE_DOWN:
            if(ev->my>=GRAPH_HUD && g->scale>0){       /* re-centre on click */
                g->vx = graph_x_at(g, ev->mx, cw);
                g->vy -= (INT64)((__int128)(ev->my - GRAPH_HUD - ph/2) * g->scale);
                graph_clamp_view(g);
            }
            return 0;
        case WM_EV_MOUSE_MOVE:
            g->trace=1; g->tmx=ev->mx; g->tmy=ev->my;
            return 0;
        case WM_EV_MOUSE_WHEEL: {
            int k = ev->wheel<0 ? -ev->wheel : ev->wheel;
            if(k>8) k=8;
            for(int i=0;i<k;i++)
                graph_zoom(g, ev->mx, ev->my, cw, ph, ev->wheel>0);
            return 0;
        }
        default: return 0;
    }
}

/* Open (or focus) the graph window. Shares g_calc.expr with the calculator. */
void tool_calc_graph_open(void)
{
    if(g_graph.win) return;                /* already open */
    g_graph.vx=0; g_graph.vy=0; g_graph.scale=0;
    g_graph.trace=0; g_graph.tmx=0; g_graph.tmy=0;

    int W=(int)ui_width(), H=(int)ui_height();
    int ww=W*56/100; if(ww<360)ww=360; if(ww>720)ww=720; if(ww>W-40)ww=W-40;
    int wh=H*60/100; if(wh<320)wh=320; if(wh>560)wh=560; if(wh>H-40)wh=H-40;
    g_graph.win=wm_open("Calculator - Graph", ww, wh, graph_draw, graph_event, &g_graph);
}

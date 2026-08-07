/*
 * FORCE -- Forest C Encapsulated
 *
 * A tree-walking interpreter for a small dynamically-typed C/HolyC-like
 * language, running in ring0. See src/include/force.h for the public API and
 * the design docs for rationale. This file is intentionally single-unit.
 *
 * Safety model (ring0 guest): never panic, never hang, never leak on any
 * input. Every failure returns a clean error code + human message. All work
 * is bounded by the caps in section 1. The interpreter runs on a dedicated
 * kmalloc'd stack (section 3b) so deep recursion cannot smash the 8 KiB task
 * kernel stack; a live stack-budget probe turns would-be overflow into a
 * clean FORCE_E_LIMIT. All heap goes through one tracked allocator so a full
 * teardown frees everything, even leaked reference cycles.
 */

#include "include/force.h"
#include "include/types.h"
#include "include/memory.h"
#include "include/string.h"
#include "include/screen.h"
#include "include/timer.h"

/* ==================================================================== */
/* 1. Caps / constants                                                   */
/* ==================================================================== */

#define FORCE_MAX_SRC        (64u * 1024u)
#define FORCE_MAX_TOKENS     100000u
#define FORCE_MAX_NODES      200000u
#define FORCE_MAX_STEPS      5000000u
#define FORCE_MAX_DEPTH      256          /* logical call depth */
#define FORCE_MAX_HEAP       (4u * 1024u * 1024u)
#define FORCE_MAX_STR        (1u * 1024u * 1024u)
#define FORCE_MAX_ARR        1000000u
#define FORCE_MAX_SLEEP_MS   5000u
#define FORCE_MAX_ARGS       64

#define FORCE_STACK_BYTES    (128u * 1024u)   /* dedicated interpreter stack */
#define FORCE_STACK_BUDGET   (FORCE_STACK_BYTES - 16384u) /* leave 16K slack */
#define FORCE_OUTBUF_CAP     8192u            /* captured output buffer */
#define FORCE_ERR_MAX        256
#define FORCE_TOSTR_DEPTH    16               /* array->string recursion cap */

/* ==================================================================== */
/* 2. Forward types                                                      */
/* ==================================================================== */

typedef struct force_value force_value;
typedef struct force_node  force_node;
typedef struct force_env   force_env;
typedef struct force_str   force_str;
typedef struct force_arr   force_arr;
typedef struct force_fn    force_fn;

typedef enum {
    FV_NULL = 0, FV_INT, FV_FLOAT, FV_BOOL, FV_STR, FV_ARRAY, FV_FN
} force_type;

struct force_value {
    uint8 type;
    union {
        int64      i;   /* FV_INT, FV_BOOL(0/1) */
        double     f;   /* FV_FLOAT */
        force_str *s;   /* FV_STR */
        force_arr *a;   /* FV_ARRAY */
        force_fn  *fn;  /* FV_FN */
    } u;
};

struct force_str { uint32 rc; uint32 len; char data[1]; }; /* len+1 alloc */
struct force_arr { uint32 rc; uint32 len; uint32 cap; force_value *items; };

struct force_fn {
    int          is_native;
    int          native_id;
    const char  *name;
    force_node  *def;   /* user fn: N_FNDEF node */
};

/* Tracked allocation header (doubly linked) -> total teardown, no leaks. */
typedef struct force_alloc_hdr {
    struct force_alloc_hdr *next, *prev;
    uint32 size;
} force_alloc_hdr;

struct force_env {
    force_env *parent;
    struct { char *name; force_value val; } *b;
    uint32 count, cap;
};

struct force_ctx {
    volatile int busy;

    /* heap accounting */
    uint32 heap_used;
    force_alloc_hdr *alloc_head;

    /* per-eval counters */
    uint32 steps;
    int    depth;
    uint32 nodes;
    uint32 tokens;

    /* stack guard */
    uintptr_t stack_base;

    /* error channel */
    int  has_err;
    int  err_code;
    int  err_line;
    char err[FORCE_ERR_MAX];

    /* return-value channel */
    force_value ret_value;

    /* output capture */
    char   outbuf[FORCE_OUTBUF_CAP];
    uint32 out_len;
    int    out_trunc;
    void (*sink)(const char *, uint32);

    force_env *global;
};

/* eval flow */
typedef enum { FR_OK = 0, FR_ERROR, FR_RETURN, FR_BREAK, FR_CONTINUE } force_flow;

/* ==================================================================== */
/* AST node kinds                                                        */
/* ==================================================================== */

enum {
    N_INT, N_FLOAT, N_STR, N_BOOL, N_NULL, N_IDENT, N_ARRAY,
    N_UNARY, N_BINARY, N_LOGICAL, N_ASSIGN, N_INDEX, N_CALL,
    N_VARDECL, N_BLOCK, N_IF, N_WHILE, N_FOR,
    N_BREAK, N_CONTINUE, N_RETURN, N_EXPRSTMT, N_FNDEF
};

struct force_node {
    uint8  kind;
    int    op;            /* token op for unary/binary/logical/assign */
    int    line;
    int64  ival;          /* N_INT; also string length for N_STR */
    double fval;          /* N_FLOAT */
    char  *sval;          /* ident / string data / fn name / var name */
    force_node *a, *b, *c, *d;
    force_node **list;    /* args / stmts / elems / params */
    uint32 nlist;
};

/* ==================================================================== */
/* Token kinds                                                           */
/* ==================================================================== */

enum {
    TT_EOF, TT_ERROR, TT_INT, TT_FLOAT, TT_STRING, TT_IDENT,
    TT_TRUE, TT_FALSE, TT_NULL,
    TT_IF, TT_ELSE, TT_WHILE, TT_FOR, TT_BREAK, TT_CONTINUE, TT_RETURN, TT_FN,
    TT_LPAREN, TT_RPAREN, TT_LBRACE, TT_RBRACE, TT_LBRACK, TT_RBRACK,
    TT_COMMA, TT_SEMI, TT_COLON,
    TT_PLUS, TT_MINUS, TT_STAR, TT_SLASH, TT_PERCENT,
    TT_EQ, TT_NE, TT_LT, TT_LE, TT_GT, TT_GE,
    TT_AND, TT_OR, TT_NOT,
    TT_BAND, TT_BOR, TT_BXOR, TT_BNOT, TT_SHL, TT_SHR,
    TT_ASSIGN, TT_PLUSEQ, TT_MINUSEQ, TT_STAREQ, TT_SLASHEQ, TT_PERCENTEQ,
    TT_DOT, TT_INC, TT_DEC
};

typedef struct {
    int    type;
    int64  ival;
    double fval;
    char  *sval;   /* ident / string (tracked alloc) */
    uint32 slen;
    int    line;
} token;

typedef struct {
    force_ctx  *ctx;
    const char *src;
    uint32      len;
    uint32      pos;
    int         line;
    token       cur, nxt;
} parser;

/* ==================================================================== */
/* Builtins                                                              */
/* ==================================================================== */

enum {
    BI_PRINT, BI_PRINTLN, BI_PRINTF, BI_STR, BI_INT, BI_FLOAT, BI_BOOL,
    BI_LEN, BI_PUSH, BI_POP, BI_TYPE, BI_SLEEP_MS, BI_MEM_STATS, BI_TICKS,
    BI__COUNT
};

static const char *const g_builtin_names[BI__COUNT] = {
    "print", "println", "printf", "str", "int", "float", "bool",
    "len", "push", "pop", "type", "sleep_ms", "mem_stats", "ticks"
};

static force_fn g_builtin_fns[BI__COUNT];
static int      g_force_ready = 0;

/* dedicated interpreter stack */
static uint8 *g_force_stack     = 0;
static void  *g_force_stack_top = 0;

/* ==================================================================== */
/* 2b. Small string builder (fixed buffer, never overflows)              */
/* ==================================================================== */

typedef struct { char *buf; uint32 cap; uint32 len; } sbuf;

static void sb_init(sbuf *s, char *buf, uint32 cap) { s->buf = buf; s->cap = cap; s->len = 0; if (cap) buf[0] = 0; }
static void sb_c(sbuf *s, char c) { if (s->len + 1 < s->cap) { s->buf[s->len++] = c; s->buf[s->len] = 0; } }
static void sb_s(sbuf *s, const char *p) { while (*p) sb_c(s, *p++); }
static void sb_sn(sbuf *s, const char *p, uint32 n) { while (n--) sb_c(s, *p++); }

static void sb_u(sbuf *s, uint64 v) {
    char t[24]; int n = 0;
    if (v == 0) { sb_c(s, '0'); return; }
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) sb_c(s, t[--n]);
}
static void sb_i(sbuf *s, int64 v) {
    if (v < 0) { sb_c(s, '-'); sb_u(s, (uint64)(-(v + 1)) + 1ull); }
    else sb_u(s, (uint64)v);
}
static void sb_hex(sbuf *s, uint64 v) {
    char t[16]; int n = 0; const char *d = "0123456789abcdef";
    if (v == 0) { sb_c(s, '0'); return; }
    while (v) { t[n++] = d[v & 0xF]; v >>= 4; }
    while (n) sb_c(s, t[--n]);
}
static void sb_f(sbuf *s, double d) {
    /* NaN / inf guards (no libc) */
    if (d != d) { sb_s(s, "nan"); return; }
    if (d > 1.7e308) { sb_s(s, "inf"); return; }
    if (d < -1.7e308) { sb_s(s, "-inf"); return; }
    if (d < 0) { sb_c(s, '-'); d = -d; }
    uint64 ip;
    if (d >= 1.8e19) { sb_s(s, "1.8e19+"); return; }   /* out of u64 range */
    ip = (uint64)d;
    double frac = d - (double)ip;
    sb_u(s, ip);
    /* fractional: up to 6 digits, trimmed */
    char fr[8]; int fn = 0;
    for (int k = 0; k < 6; k++) {
        frac *= 10.0;
        int dig = (int)frac;
        if (dig < 0) dig = 0;
        if (dig > 9) dig = 9;
        fr[fn++] = (char)('0' + dig);
        frac -= dig;
    }
    while (fn > 0 && fr[fn - 1] == '0') fn--;   /* trim trailing zeros */
    if (fn > 0) { sb_c(s, '.'); for (int k = 0; k < fn; k++) sb_c(s, fr[k]); }
}

/* ==================================================================== */
/* 3. Tracked allocator + heap accounting                                */
/* ==================================================================== */

static void *force_alloc(force_ctx *c, uint32 n) {
    uint32 total = n + (uint32)sizeof(force_alloc_hdr);
    if (c->heap_used + total > FORCE_MAX_HEAP) return 0;
    force_alloc_hdr *h = (force_alloc_hdr *)kmalloc(total);
    if (!h) return 0;
    h->size = total;
    h->prev = 0;
    h->next = c->alloc_head;
    if (c->alloc_head) c->alloc_head->prev = h;
    c->alloc_head = h;
    c->heap_used += total;
    memset((void *)(h + 1), 0, n);
    return (void *)(h + 1);
}

static void force_free(force_ctx *c, void *p) {
    if (!p) return;
    force_alloc_hdr *h = ((force_alloc_hdr *)p) - 1;
    if (h->prev) h->prev->next = h->next; else c->alloc_head = h->next;
    if (h->next) h->next->prev = h->prev;
    c->heap_used -= h->size;
    kfree(h);
}

static void force_free_all(force_ctx *c) {
    force_alloc_hdr *h = c->alloc_head;
    while (h) { force_alloc_hdr *nx = h->next; kfree(h); h = nx; }
    c->alloc_head = 0;
    c->heap_used = 0;
}

/* ==================================================================== */
/* 3b. Dedicated-stack execution (protects the 8 KiB task kernel stack)  */
/* ==================================================================== */

static int __attribute__((noinline))
force_call_on_stack(void *newsp, int (*fn)(void *), void *arg) {
    int ret;
    __asm__ __volatile__(
        "movl %%esp, %%edi\n\t"      /* save old esp */
        "movl %[nsp], %%esp\n\t"     /* switch to interpreter stack */
        "pushl %%edi\n\t"            /* stash old esp on new stack */
        "pushl %[arg]\n\t"
        "call *%[fn]\n\t"
        "addl $4, %%esp\n\t"         /* drop arg */
        "popl %%esp\n\t"             /* restore old esp */
        : "=a"(ret)
        : [nsp] "r"(newsp), [fn] "r"(fn), [arg] "r"(arg)
        : "ecx", "edx", "edi", "cc", "memory");
    return ret;
}

/* ==================================================================== */
/* 4. Error helpers                                                      */
/* ==================================================================== */

static void err_set(force_ctx *c, int code, int line, const char *msg) {
    if (c->has_err) return;   /* first error wins */
    c->has_err = 1;
    c->err_code = code;
    c->err_line = line;
    sbuf s; sb_init(&s, c->err, FORCE_ERR_MAX);
    sb_s(&s, msg);
}
static void err_set2(force_ctx *c, int code, int line, const char *a, const char *b) {
    if (c->has_err) return;
    c->has_err = 1; c->err_code = code; c->err_line = line;
    sbuf s; sb_init(&s, c->err, FORCE_ERR_MAX);
    sb_s(&s, a); sb_s(&s, b);
}

/* ==================================================================== */
/* 5. Value / heap-object helpers                                        */
/* ==================================================================== */

static force_value V_NULL(void) { force_value v; v.type = FV_NULL; v.u.i = 0; return v; }
static force_value V_INT(int64 x) { force_value v; v.type = FV_INT; v.u.i = x; return v; }
static force_value V_FLOAT(double x) { force_value v; v.type = FV_FLOAT; v.u.f = x; return v; }
static force_value V_BOOL(int x) { force_value v; v.type = FV_BOOL; v.u.i = x ? 1 : 0; return v; }

static void str_free(force_ctx *c, force_str *s) { force_free(c, s); }
static void arr_free(force_ctx *c, force_arr *a);

static void fv_incref(force_value v) {
    if (v.type == FV_STR && v.u.s) v.u.s->rc++;
    else if (v.type == FV_ARRAY && v.u.a) v.u.a->rc++;
}
static void fv_decref(force_ctx *c, force_value v) {
    if (v.type == FV_STR && v.u.s) { if (--v.u.s->rc == 0) str_free(c, v.u.s); }
    else if (v.type == FV_ARRAY && v.u.a) { if (--v.u.a->rc == 0) arr_free(c, v.u.a); }
}

static void arr_free(force_ctx *c, force_arr *a) {
    if (a->items) {
        for (uint32 i = 0; i < a->len; i++) fv_decref(c, a->items[i]);
        force_free(c, a->items);
    }
    force_free(c, a);
}

static force_value make_str(force_ctx *c, const char *data, uint32 len) {
    force_str *s = (force_str *)force_alloc(c, (uint32)sizeof(force_str) + len);
    if (!s) { err_set(c, FORCE_E_NOMEM, 0, "out of memory (string)"); return V_NULL(); }
    s->rc = 1; s->len = len;
    if (len) memcpy(s->data, data, len);
    s->data[len] = 0;
    force_value v; v.type = FV_STR; v.u.s = s; return v;
}

static force_value make_arr(force_ctx *c) {
    force_arr *a = (force_arr *)force_alloc(c, (uint32)sizeof(force_arr));
    if (!a) { err_set(c, FORCE_E_NOMEM, 0, "out of memory (array)"); return V_NULL(); }
    a->rc = 1; a->len = 0; a->cap = 0; a->items = 0;
    force_value v; v.type = FV_ARRAY; v.u.a = a; return v;
}

/* store an (incref'd) copy of val at end of array */
static int arr_push(force_ctx *c, force_arr *a, force_value val) {
    if (a->len >= FORCE_MAX_ARR) { err_set(c, FORCE_E_LIMIT, 0, "array length cap exceeded"); return 0; }
    if (a->len == a->cap) {
        uint32 ncap = a->cap ? a->cap * 2 : 8;
        force_value *ni = (force_value *)force_alloc(c, ncap * (uint32)sizeof(force_value));
        if (!ni) { err_set(c, FORCE_E_NOMEM, 0, "out of memory (array grow)"); return 0; }
        if (a->items) { memcpy(ni, a->items, a->len * sizeof(force_value)); force_free(c, a->items); }
        a->items = ni; a->cap = ncap;
    }
    fv_incref(val);
    a->items[a->len++] = val;
    return 1;
}

static int fv_truthy(force_value v) {
    switch (v.type) {
        case FV_NULL:  return 0;
        case FV_INT:
        case FV_BOOL:  return v.u.i != 0;
        case FV_FLOAT: return v.u.f != 0.0;
        case FV_STR:   return v.u.s && v.u.s->len != 0;
        case FV_ARRAY: return v.u.a && v.u.a->len != 0;
        case FV_FN:    return 1;
    }
    return 0;
}

static const char *fv_typename(force_value v) {
    switch (v.type) {
        case FV_NULL:  return "null";
        case FV_INT:   return "int";
        case FV_FLOAT: return "float";
        case FV_BOOL:  return "bool";
        case FV_STR:   return "str";
        case FV_ARRAY: return "array";
        case FV_FN:    return "fn";
    }
    return "?";
}

static void value_to_sb(force_ctx *c, sbuf *s, force_value v, int depth) {
    if (c->has_err) return;
    switch (v.type) {
        case FV_NULL:  sb_s(s, "null"); break;
        case FV_INT:   sb_i(s, v.u.i); break;
        case FV_FLOAT: sb_f(s, v.u.f); break;
        case FV_BOOL:  sb_s(s, v.u.i ? "true" : "false"); break;
        case FV_STR:   if (v.u.s) sb_sn(s, v.u.s->data, v.u.s->len); break;
        case FV_FN:    sb_s(s, "<fn "); sb_s(s, v.u.fn && v.u.fn->name ? v.u.fn->name : "?"); sb_c(s, '>'); break;
        case FV_ARRAY:
            if (depth >= FORCE_TOSTR_DEPTH) { sb_s(s, "[...]"); break; }
            sb_c(s, '[');
            if (v.u.a) {
                for (uint32 i = 0; i < v.u.a->len; i++) {
                    if (i) sb_s(s, ", ");
                    force_value it = v.u.a->items[i];
                    if (it.type == FV_STR) { sb_c(s, '"'); value_to_sb(c, s, it, depth + 1); sb_c(s, '"'); }
                    else value_to_sb(c, s, it, depth + 1);
                }
            }
            sb_c(s, ']');
            break;
    }
}

static int fv_equal(force_value a, force_value b) {
    if ((a.type == FV_INT || a.type == FV_FLOAT || a.type == FV_BOOL) &&
        (b.type == FV_INT || b.type == FV_FLOAT || b.type == FV_BOOL)) {
        if (a.type == FV_FLOAT || b.type == FV_FLOAT) {
            double x = (a.type == FV_FLOAT) ? a.u.f : (double)a.u.i;
            double y = (b.type == FV_FLOAT) ? b.u.f : (double)b.u.i;
            return x == y;
        }
        return a.u.i == b.u.i;
    }
    if (a.type != b.type) return 0;
    switch (a.type) {
        case FV_NULL:  return 1;
        case FV_STR:   return a.u.s->len == b.u.s->len &&
                              memcmp(a.u.s->data, b.u.s->data, a.u.s->len) == 0;
        case FV_ARRAY: return a.u.a == b.u.a;
        case FV_FN:    return a.u.fn == b.u.fn;
    }
    return 0;
}

/* ==================================================================== */
/* 6. Environment / scope                                                */
/* ==================================================================== */

static force_env *env_new(force_ctx *c, force_env *parent) {
    force_env *e = (force_env *)force_alloc(c, (uint32)sizeof(force_env));
    if (!e) { err_set(c, FORCE_E_NOMEM, 0, "out of memory (env)"); return 0; }
    e->parent = parent; e->b = 0; e->count = 0; e->cap = 0;
    return e;
}

static void env_free(force_ctx *c, force_env *e) {
    if (!e) return;
    if (e->b) {
        for (uint32 i = 0; i < e->count; i++) fv_decref(c, e->b[i].val);
        force_free(c, e->b);
    }
    force_free(c, e);
}

/* define in THIS scope (stores an incref'd copy) */
static int env_define(force_ctx *c, force_env *e, char *name, force_value val) {
    for (uint32 i = 0; i < e->count; i++) {
        if (strcmp(e->b[i].name, name) == 0) {
            fv_decref(c, e->b[i].val);
            fv_incref(val);
            e->b[i].val = val;
            return 1;
        }
    }
    if (e->count == e->cap) {
        uint32 ncap = e->cap ? e->cap * 2 : 8;
        void *nb = force_alloc(c, ncap * (uint32)sizeof(*e->b));
        if (!nb) { err_set(c, FORCE_E_NOMEM, 0, "out of memory (scope)"); return 0; }
        if (e->b) { memcpy(nb, e->b, e->count * sizeof(*e->b)); force_free(c, e->b); }
        e->b = nb; e->cap = ncap;
    }
    fv_incref(val);
    e->b[e->count].name = name;
    e->b[e->count].val = val;
    e->count++;
    return 1;
}

/* assign to an existing binding somewhere in the chain */
static int env_assign(force_ctx *c, force_env *e, const char *name, force_value val) {
    for (force_env *s = e; s; s = s->parent) {
        for (uint32 i = 0; i < s->count; i++) {
            if (strcmp(s->b[i].name, name) == 0) {
                fv_decref(c, s->b[i].val);
                fv_incref(val);
                s->b[i].val = val;
                return 1;
            }
        }
    }
    return 0;
}

/* look up; returns an incref'd copy in *out */
static int env_get(force_ctx *c, force_env *e, const char *name, force_value *out) {
    (void)c;
    for (force_env *s = e; s; s = s->parent) {
        for (uint32 i = 0; i < s->count; i++) {
            if (strcmp(s->b[i].name, name) == 0) {
                *out = s->b[i].val;
                fv_incref(*out);
                return 1;
            }
        }
    }
    return 0;
}

/* ==================================================================== */
/* 7. Lexer                                                              */
/* ==================================================================== */

static int is_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int is_digit(char ch) { return ch >= '0' && ch <= '9'; }
static int is_alnum(char ch) { return is_alpha(ch) || is_digit(ch); }
static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static char lx_peek(parser *p) { return p->pos < p->len ? p->src[p->pos] : 0; }
static char lx_peek2(parser *p) { return (p->pos + 1) < p->len ? p->src[p->pos + 1] : 0; }
static char lx_adv(parser *p) {
    char ch = lx_peek(p);
    if (ch) { p->pos++; if (ch == '\n') p->line++; }
    return ch;
}

static int kw_match(const char *s, uint32 n) {
    struct { const char *w; int t; } kws[] = {
        {"if", TT_IF}, {"else", TT_ELSE}, {"while", TT_WHILE}, {"for", TT_FOR},
        {"break", TT_BREAK}, {"continue", TT_CONTINUE}, {"return", TT_RETURN},
        {"fn", TT_FN}, {"true", TT_TRUE}, {"false", TT_FALSE}, {"null", TT_NULL}
    };
    for (unsigned i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        const char *w = kws[i].w; uint32 j = 0;
        while (j < n && w[j] && w[j] == s[j]) j++;
        if (j == n && w[j] == 0) return kws[i].t;
    }
    return 0;
}

static token lex_next(parser *p) {
    token t; t.type = TT_EOF; t.ival = 0; t.fval = 0; t.sval = 0; t.slen = 0; t.line = p->line;
    force_ctx *c = p->ctx;

    for (;;) {
        char ch = lx_peek(p);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { lx_adv(p); continue; }
        if (ch == '/' && lx_peek2(p) == '/') { while (lx_peek(p) && lx_peek(p) != '\n') lx_adv(p); continue; }
        if (ch == '/' && lx_peek2(p) == '*') {
            lx_adv(p); lx_adv(p);
            int closed = 0;
            while (lx_peek(p)) { if (lx_peek(p) == '*' && lx_peek2(p) == '/') { lx_adv(p); lx_adv(p); closed = 1; break; } lx_adv(p); }
            if (!closed) { err_set(c, FORCE_E_PARSE, p->line, "unterminated block comment"); t.type = TT_ERROR; return t; }
            continue;
        }
        break;
    }

    t.line = p->line;
    char ch = lx_peek(p);
    if (ch == 0) { t.type = TT_EOF; return t; }

    /* numbers */
    if (is_digit(ch) || (ch == '.' && is_digit(lx_peek2(p)))) {
        int is_float = 0;
        uint64 iv = 0;
        /* radix prefixes */
        if (ch == '0' && (lx_peek2(p) == 'x' || lx_peek2(p) == 'X')) {
            lx_adv(p); lx_adv(p); int any = 0;
            while (1) { char d = lx_peek(p); if (d == '_') { lx_adv(p); continue; } int hv = hexval(d); if (hv < 0) break; iv = iv * 16 + hv; any = 1; lx_adv(p); }
            if (!any) { err_set(c, FORCE_E_PARSE, p->line, "malformed hex literal"); t.type = TT_ERROR; return t; }
            t.type = TT_INT; t.ival = (int64)iv; return t;
        }
        if (ch == '0' && (lx_peek2(p) == 'b' || lx_peek2(p) == 'B')) {
            lx_adv(p); lx_adv(p); int any = 0;
            while (1) { char d = lx_peek(p); if (d == '_') { lx_adv(p); continue; } if (d != '0' && d != '1') break; iv = iv * 2 + (d - '0'); any = 1; lx_adv(p); }
            if (!any) { err_set(c, FORCE_E_PARSE, p->line, "malformed binary literal"); t.type = TT_ERROR; return t; }
            t.type = TT_INT; t.ival = (int64)iv; return t;
        }
        if (ch == '0' && (lx_peek2(p) == 'o' || lx_peek2(p) == 'O')) {
            lx_adv(p); lx_adv(p); int any = 0;
            while (1) { char d = lx_peek(p); if (d == '_') { lx_adv(p); continue; } if (d < '0' || d > '7') break; iv = iv * 8 + (d - '0'); any = 1; lx_adv(p); }
            if (!any) { err_set(c, FORCE_E_PARSE, p->line, "malformed octal literal"); t.type = TT_ERROR; return t; }
            t.type = TT_INT; t.ival = (int64)iv; return t;
        }
        /* decimal / float */
        double dv = 0; int seen_digit = 0;
        while (1) { char d = lx_peek(p); if (d == '_') { lx_adv(p); continue; } if (!is_digit(d)) break; iv = iv * 10 + (uint64)(d - '0'); dv = dv * 10.0 + (double)(d - '0'); seen_digit = 1; lx_adv(p); }
        if (lx_peek(p) == '.') { is_float = 1; lx_adv(p); double frac = 0.1; while (1) { char d = lx_peek(p); if (d == '_') { lx_adv(p); continue; } if (!is_digit(d)) break; dv += (double)(d - '0') * frac; frac *= 0.1; seen_digit = 1; lx_adv(p); } }
        if (lx_peek(p) == 'e' || lx_peek(p) == 'E') {
            is_float = 1; lx_adv(p); int neg = 0; if (lx_peek(p) == '+' ) lx_adv(p); else if (lx_peek(p) == '-') { neg = 1; lx_adv(p); }
            int e = 0; int eany = 0; while (is_digit(lx_peek(p))) { e = e * 10 + (lx_peek(p) - '0'); eany = 1; lx_adv(p); }
            if (!eany) { err_set(c, FORCE_E_PARSE, p->line, "malformed exponent"); t.type = TT_ERROR; return t; }
            double m = 1.0; for (int k = 0; k < e && k < 308; k++) m *= 10.0;
            if (neg) dv /= m; else dv *= m;
        }
        if (!seen_digit) { err_set(c, FORCE_E_PARSE, p->line, "malformed number"); t.type = TT_ERROR; return t; }
        if (is_float) { t.type = TT_FLOAT; t.fval = dv; } else { t.type = TT_INT; t.ival = (int64)iv; }
        return t;
    }

    /* identifiers / keywords */
    if (is_alpha(ch)) {
        uint32 start = p->pos;
        while (is_alnum(lx_peek(p))) lx_adv(p);
        uint32 n = p->pos - start;
        int kw = kw_match(p->src + start, n);
        if (kw) { t.type = kw; return t; }
        char *nm = (char *)force_alloc(c, n + 1);
        if (!nm) { err_set(c, FORCE_E_NOMEM, p->line, "out of memory (ident)"); t.type = TT_ERROR; return t; }
        memcpy(nm, p->src + start, n); nm[n] = 0;
        t.type = TT_IDENT; t.sval = nm; t.slen = n; return t;
    }

    /* strings */
    if (ch == '"') {
        lx_adv(p);
        char tmp[1024]; uint32 tn = 0;   /* small chunk; larger strings use dynamic path below */
        /* We assemble into a growing tracked buffer to allow long strings. */
        uint32 cap = 64; char *buf = (char *)force_alloc(c, cap);
        if (!buf) { err_set(c, FORCE_E_NOMEM, p->line, "out of memory (string)"); t.type = TT_ERROR; return t; }
        uint32 blen = 0;
        (void)tmp; (void)tn;
        for (;;) {
            char d = lx_peek(p);
            if (d == 0 || d == '\n') { err_set(c, FORCE_E_PARSE, p->line, "unterminated string literal"); t.type = TT_ERROR; return t; }
            if (d == '"') { lx_adv(p); break; }
            char out;
            if (d == '\\') {
                lx_adv(p); char e = lx_adv(p);
                switch (e) {
                    case 'n': out = '\n'; break;
                    case 't': out = '\t'; break;
                    case 'r': out = '\r'; break;
                    case '0': out = '\0'; break;
                    case '\\': out = '\\'; break;
                    case '"': out = '"'; break;
                    case '\'': out = '\''; break;
                    case 'x': {
                        int h1 = hexval(lx_peek(p)); if (h1 < 0) { err_set(c, FORCE_E_PARSE, p->line, "bad \\x escape"); t.type = TT_ERROR; return t; } lx_adv(p);
                        int h2 = hexval(lx_peek(p)); if (h2 < 0) { err_set(c, FORCE_E_PARSE, p->line, "bad \\x escape"); t.type = TT_ERROR; return t; } lx_adv(p);
                        out = (char)((h1 << 4) | h2); break;
                    }
                    default: err_set(c, FORCE_E_PARSE, p->line, "unknown string escape"); t.type = TT_ERROR; return t;
                }
            } else { out = lx_adv(p); }
            if (blen >= FORCE_MAX_STR) { err_set(c, FORCE_E_LIMIT, p->line, "string literal too long"); t.type = TT_ERROR; return t; }
            if (blen + 1 >= cap) {
                uint32 ncap = cap * 2;
                char *nb = (char *)force_alloc(c, ncap);
                if (!nb) { err_set(c, FORCE_E_NOMEM, p->line, "out of memory (string)"); t.type = TT_ERROR; return t; }
                memcpy(nb, buf, blen); force_free(c, buf); buf = nb; cap = ncap;
            }
            buf[blen++] = out;
        }
        t.type = TT_STRING; t.sval = buf; t.slen = blen; return t;
    }

    /* punctuation / operators */
    lx_adv(p);
    char n2 = lx_peek(p);
    switch (ch) {
        case '(': t.type = TT_LPAREN; return t;
        case ')': t.type = TT_RPAREN; return t;
        case '{': t.type = TT_LBRACE; return t;
        case '}': t.type = TT_RBRACE; return t;
        case '[': t.type = TT_LBRACK; return t;
        case ']': t.type = TT_RBRACK; return t;
        case ',': t.type = TT_COMMA; return t;
        case ';': t.type = TT_SEMI; return t;
        case ':': t.type = TT_COLON; return t;
        case '.': t.type = TT_DOT; return t;
        case '~': t.type = TT_BNOT; return t;
        case '+': if (n2 == '=') { lx_adv(p); t.type = TT_PLUSEQ; } else if (n2 == '+') { lx_adv(p); t.type = TT_INC; } else t.type = TT_PLUS; return t;
        case '-': if (n2 == '=') { lx_adv(p); t.type = TT_MINUSEQ; } else if (n2 == '-') { lx_adv(p); t.type = TT_DEC; } else t.type = TT_MINUS; return t;
        case '*': if (n2 == '=') { lx_adv(p); t.type = TT_STAREQ; } else t.type = TT_STAR; return t;
        case '/': if (n2 == '=') { lx_adv(p); t.type = TT_SLASHEQ; } else t.type = TT_SLASH; return t;
        case '%': if (n2 == '=') { lx_adv(p); t.type = TT_PERCENTEQ; } else t.type = TT_PERCENT; return t;
        case '=': if (n2 == '=') { lx_adv(p); t.type = TT_EQ; } else t.type = TT_ASSIGN; return t;
        case '!': if (n2 == '=') { lx_adv(p); t.type = TT_NE; } else t.type = TT_NOT; return t;
        case '<': if (n2 == '=') { lx_adv(p); t.type = TT_LE; } else if (n2 == '<') { lx_adv(p); t.type = TT_SHL; } else t.type = TT_LT; return t;
        case '>': if (n2 == '=') { lx_adv(p); t.type = TT_GE; } else if (n2 == '>') { lx_adv(p); t.type = TT_SHR; } else t.type = TT_GT; return t;
        case '&': if (n2 == '&') { lx_adv(p); t.type = TT_AND; } else t.type = TT_BAND; return t;
        case '|': if (n2 == '|') { lx_adv(p); t.type = TT_OR; } else t.type = TT_BOR; return t;
        case '^': t.type = TT_BXOR; return t;
    }
    err_set(c, FORCE_E_PARSE, p->line, "unexpected character");
    t.type = TT_ERROR;
    return t;
}

/* ==================================================================== */
/* 8. Parser (recursive descent)                                         */
/* ==================================================================== */

static void p_advance(parser *p) {
    p->cur = p->nxt;
    if (p->ctx->tokens++ > FORCE_MAX_TOKENS) { err_set(p->ctx, FORCE_E_LIMIT, p->line, "token count cap exceeded"); p->nxt.type = TT_EOF; return; }
    p->nxt = lex_next(p);
}

static int parse_stack_ok(parser *p) {
    char probe;
    if (p->ctx->stack_base - (uintptr_t)&probe > FORCE_STACK_BUDGET) {
        err_set(p->ctx, FORCE_E_LIMIT, p->line, "expression nesting too deep");
        return 0;
    }
    return 1;
}

static force_node *new_node(parser *p, int kind) {
    force_ctx *c = p->ctx;
    if (c->nodes++ > FORCE_MAX_NODES) { err_set(c, FORCE_E_LIMIT, p->cur.line, "AST node cap exceeded"); return 0; }
    force_node *n = (force_node *)force_alloc(c, (uint32)sizeof(force_node));
    if (!n) { err_set(c, FORCE_E_NOMEM, p->cur.line, "out of memory (node)"); return 0; }
    n->kind = (uint8)kind; n->line = p->cur.line;
    return n;
}

static int p_expect(parser *p, int type, const char *what) {
    if (p->cur.type != type) { err_set2(p->ctx, FORCE_E_PARSE, p->cur.line, "expected ", what); return 0; }
    p_advance(p);
    return 1;
}

static force_node *parse_expr(parser *p);
static force_node *parse_statement(parser *p);

/* node-list builder */
typedef struct { force_node **list; uint32 count, cap; } nodelist;
static int nl_push(parser *p, nodelist *nl, force_node *n) {
    force_ctx *c = p->ctx;
    if (nl->count == nl->cap) {
        uint32 ncap = nl->cap ? nl->cap * 2 : 8;
        force_node **nn = (force_node **)force_alloc(c, ncap * (uint32)sizeof(force_node *));
        if (!nn) { err_set(c, FORCE_E_NOMEM, p->cur.line, "out of memory (list)"); return 0; }
        if (nl->list) { memcpy(nn, nl->list, nl->count * sizeof(force_node *)); force_free(c, nl->list); }
        nl->list = nn; nl->cap = ncap;
    }
    nl->list[nl->count++] = n;
    return 1;
}

static force_node *parse_primary(parser *p) {
    if (!parse_stack_ok(p)) return 0;
    force_ctx *c = p->ctx;
    token t = p->cur;
    switch (t.type) {
        case TT_INT:   { force_node *n = new_node(p, N_INT); if (!n) return 0; n->ival = t.ival; p_advance(p); return n; }
        case TT_FLOAT: { force_node *n = new_node(p, N_FLOAT); if (!n) return 0; n->fval = t.fval; p_advance(p); return n; }
        case TT_STRING:{ force_node *n = new_node(p, N_STR); if (!n) return 0; n->sval = t.sval; n->ival = (int64)t.slen; p_advance(p); return n; }
        case TT_TRUE:  { force_node *n = new_node(p, N_BOOL); if (!n) return 0; n->ival = 1; p_advance(p); return n; }
        case TT_FALSE: { force_node *n = new_node(p, N_BOOL); if (!n) return 0; n->ival = 0; p_advance(p); return n; }
        case TT_NULL:  { force_node *n = new_node(p, N_NULL); if (!n) return 0; p_advance(p); return n; }
        case TT_IDENT: { force_node *n = new_node(p, N_IDENT); if (!n) return 0; n->sval = t.sval; p_advance(p); return n; }
        case TT_LPAREN:{ p_advance(p); force_node *e = parse_expr(p); if (!e) return 0; if (!p_expect(p, TT_RPAREN, ")")) return 0; return e; }
        case TT_LBRACK:{
            force_node *n = new_node(p, N_ARRAY); if (!n) return 0;
            p_advance(p);
            nodelist nl = {0, 0, 0};
            if (p->cur.type != TT_RBRACK) {
                for (;;) {
                    force_node *e = parse_expr(p); if (!e) return 0;
                    if (nl.count >= FORCE_MAX_ARGS) { err_set(c, FORCE_E_LIMIT, p->cur.line, "array literal too large"); return 0; }
                    if (!nl_push(p, &nl, e)) return 0;
                    if (p->cur.type == TT_COMMA) { p_advance(p); continue; }
                    break;
                }
            }
            if (!p_expect(p, TT_RBRACK, "]")) return 0;
            n->list = nl.list; n->nlist = nl.count;
            return n;
        }
        case TT_INC: case TT_DEC:
            err_set(c, FORCE_E_PARSE, t.line, "++/-- not supported (use += 1)"); return 0;
        case TT_DOT:
            err_set(c, FORCE_E_PARSE, t.line, "member access not supported (MVP)"); return 0;
        default:
            err_set(c, FORCE_E_PARSE, t.line, "unexpected token in expression"); return 0;
    }
}

static force_node *parse_postfix(parser *p) {
    force_node *e = parse_primary(p);
    if (!e) return 0;
    for (;;) {
        if (p->cur.type == TT_LPAREN) {
            force_node *call = new_node(p, N_CALL); if (!call) return 0;
            call->a = e;
            p_advance(p);
            nodelist nl = {0, 0, 0};
            if (p->cur.type != TT_RPAREN) {
                for (;;) {
                    force_node *arg = parse_expr(p); if (!arg) return 0;
                    if (nl.count >= FORCE_MAX_ARGS) { err_set(p->ctx, FORCE_E_LIMIT, p->cur.line, "too many arguments"); return 0; }
                    if (!nl_push(p, &nl, arg)) return 0;
                    if (p->cur.type == TT_COMMA) { p_advance(p); continue; }
                    break;
                }
            }
            if (!p_expect(p, TT_RPAREN, ")")) return 0;
            call->list = nl.list; call->nlist = nl.count;
            e = call;
        } else if (p->cur.type == TT_LBRACK) {
            force_node *idx = new_node(p, N_INDEX); if (!idx) return 0;
            idx->a = e;
            p_advance(p);
            idx->b = parse_expr(p); if (!idx->b) return 0;
            if (!p_expect(p, TT_RBRACK, "]")) return 0;
            e = idx;
        } else break;
    }
    return e;
}

static force_node *parse_unary(parser *p) {
    if (!parse_stack_ok(p)) return 0;
    int tt = p->cur.type;
    if (tt == TT_NOT || tt == TT_MINUS || tt == TT_BNOT) {
        force_node *n = new_node(p, N_UNARY); if (!n) return 0;
        n->op = tt; p_advance(p);
        n->a = parse_unary(p); if (!n->a) return 0;
        return n;
    }
    if (tt == TT_INC || tt == TT_DEC) { err_set(p->ctx, FORCE_E_PARSE, p->cur.line, "++/-- not supported (use += 1)"); return 0; }
    return parse_postfix(p);
}

/* binary level helper via explicit precedence-climbing chain */
#define BINLEVEL(fname, next, cond) \
static force_node *fname(parser *p) { \
    if (!parse_stack_ok(p)) return 0; \
    force_node *left = next(p); if (!left) return 0; \
    while (cond) { int op = p->cur.type; force_node *n = new_node(p, N_BINARY); if (!n) return 0; \
        n->op = op; n->a = left; p_advance(p); n->b = next(p); if (!n->b) return 0; left = n; } \
    return left; }

BINLEVEL(parse_factor, parse_unary, (p->cur.type == TT_STAR || p->cur.type == TT_SLASH || p->cur.type == TT_PERCENT))
BINLEVEL(parse_term,   parse_factor, (p->cur.type == TT_PLUS || p->cur.type == TT_MINUS))
BINLEVEL(parse_shift,  parse_term,   (p->cur.type == TT_SHL || p->cur.type == TT_SHR))
BINLEVEL(parse_cmp,    parse_shift,  (p->cur.type == TT_LT || p->cur.type == TT_LE || p->cur.type == TT_GT || p->cur.type == TT_GE))
BINLEVEL(parse_eq,     parse_cmp,    (p->cur.type == TT_EQ || p->cur.type == TT_NE))
BINLEVEL(parse_band,   parse_eq,     (p->cur.type == TT_BAND))
BINLEVEL(parse_bxor,   parse_band,   (p->cur.type == TT_BXOR))
BINLEVEL(parse_bor,    parse_bxor,   (p->cur.type == TT_BOR))

static force_node *parse_logic_and(parser *p) {
    if (!parse_stack_ok(p)) return 0;
    force_node *left = parse_bor(p); if (!left) return 0;
    while (p->cur.type == TT_AND) {
        force_node *n = new_node(p, N_LOGICAL); if (!n) return 0;
        n->op = TT_AND; n->a = left; p_advance(p); n->b = parse_bor(p); if (!n->b) return 0; left = n;
    }
    return left;
}
static force_node *parse_logic_or(parser *p) {
    if (!parse_stack_ok(p)) return 0;
    force_node *left = parse_logic_and(p); if (!left) return 0;
    while (p->cur.type == TT_OR) {
        force_node *n = new_node(p, N_LOGICAL); if (!n) return 0;
        n->op = TT_OR; n->a = left; p_advance(p); n->b = parse_logic_and(p); if (!n->b) return 0; left = n;
    }
    return left;
}

static int is_assign_tt(int tt) {
    return tt == TT_ASSIGN || tt == TT_PLUSEQ || tt == TT_MINUSEQ ||
           tt == TT_STAREQ || tt == TT_SLASHEQ || tt == TT_PERCENTEQ;
}

static force_node *parse_assignment(parser *p) {
    if (!parse_stack_ok(p)) return 0;
    force_node *left = parse_logic_or(p); if (!left) return 0;
    if (is_assign_tt(p->cur.type)) {
        if (left->kind != N_IDENT && left->kind != N_INDEX) { err_set(p->ctx, FORCE_E_PARSE, p->cur.line, "invalid assignment target"); return 0; }
        int op = p->cur.type;
        force_node *n = new_node(p, N_ASSIGN); if (!n) return 0;
        n->op = op; n->a = left; p_advance(p);
        n->b = parse_assignment(p); if (!n->b) return 0;
        return n;
    }
    return left;
}

static force_node *parse_expr(parser *p) { return parse_assignment(p); }

static int is_type_kw(const char *s) {
    return strcmp(s, "int") == 0 || strcmp(s, "float") == 0 || strcmp(s, "str") == 0 ||
           strcmp(s, "bool") == 0 || strcmp(s, "auto") == 0;
}

static force_node *parse_block(parser *p) {
    force_node *n = new_node(p, N_BLOCK); if (!n) return 0;
    if (!p_expect(p, TT_LBRACE, "{")) return 0;
    nodelist nl = {0, 0, 0};
    while (p->cur.type != TT_RBRACE && p->cur.type != TT_EOF && !p->ctx->has_err) {
        force_node *st = parse_statement(p); if (!st) return 0;
        if (!nl_push(p, &nl, st)) return 0;
    }
    if (!p_expect(p, TT_RBRACE, "}")) return 0;
    n->list = nl.list; n->nlist = nl.count;
    return n;
}

/* var_decl: TYPEKW IDENT [ = expr ] ; ; caller verified the two-ident shape */
static force_node *parse_vardecl(parser *p) {
    p_advance(p);                 /* consume type kw ident */
    force_node *n = new_node(p, N_VARDECL); if (!n) return 0;
    n->sval = p->cur.sval;        /* variable name */
    p_advance(p);                 /* consume name */
    if (p->cur.type == TT_ASSIGN) { p_advance(p); n->a = parse_expr(p); if (!n->a) return 0; }
    if (!p_expect(p, TT_SEMI, ";")) return 0;
    return n;
}

static force_node *parse_simple_vardecl_noterm(parser *p) {
    /* for-init var decl without trailing ';' consumption */
    p_advance(p);
    force_node *n = new_node(p, N_VARDECL); if (!n) return 0;
    n->sval = p->cur.sval;
    p_advance(p);
    if (p->cur.type == TT_ASSIGN) { p_advance(p); n->a = parse_expr(p); if (!n->a) return 0; }
    return n;
}

static force_node *parse_fndef(parser *p) {
    force_node *n = new_node(p, N_FNDEF); if (!n) return 0;
    p_advance(p);   /* fn */
    if (p->cur.type != TT_IDENT) { err_set(p->ctx, FORCE_E_PARSE, p->cur.line, "expected function name"); return 0; }
    n->sval = p->cur.sval; p_advance(p);
    if (!p_expect(p, TT_LPAREN, "(")) return 0;
    nodelist params = {0, 0, 0};
    if (p->cur.type != TT_RPAREN) {
        for (;;) {
            if (p->cur.type != TT_IDENT) { err_set(p->ctx, FORCE_E_PARSE, p->cur.line, "expected parameter name"); return 0; }
            if (params.count >= FORCE_MAX_ARGS) { err_set(p->ctx, FORCE_E_LIMIT, p->cur.line, "too many parameters"); return 0; }
            force_node *pn = new_node(p, N_IDENT); if (!pn) return 0;
            pn->sval = p->cur.sval; p_advance(p);
            if (!nl_push(p, &params, pn)) return 0;
            if (p->cur.type == TT_COMMA) { p_advance(p); continue; }
            break;
        }
    }
    if (!p_expect(p, TT_RPAREN, ")")) return 0;
    n->list = params.list; n->nlist = params.count;
    n->a = parse_block(p); if (!n->a) return 0;
    return n;
}

static force_node *parse_statement(parser *p) {
    if (!parse_stack_ok(p)) return 0;
    force_ctx *c = p->ctx;
    switch (p->cur.type) {
        case TT_LBRACE: return parse_block(p);
        case TT_FN:     return parse_fndef(p);
        case TT_IF: {
            force_node *n = new_node(p, N_IF); if (!n) return 0;
            p_advance(p);
            if (!p_expect(p, TT_LPAREN, "(")) return 0;
            n->a = parse_expr(p); if (!n->a) return 0;
            if (!p_expect(p, TT_RPAREN, ")")) return 0;
            n->b = parse_statement(p); if (!n->b) return 0;
            if (p->cur.type == TT_ELSE) { p_advance(p); n->c = parse_statement(p); if (!n->c) return 0; }
            return n;
        }
        case TT_WHILE: {
            force_node *n = new_node(p, N_WHILE); if (!n) return 0;
            p_advance(p);
            if (!p_expect(p, TT_LPAREN, "(")) return 0;
            n->a = parse_expr(p); if (!n->a) return 0;
            if (!p_expect(p, TT_RPAREN, ")")) return 0;
            n->b = parse_statement(p); if (!n->b) return 0;
            return n;
        }
        case TT_FOR: {
            force_node *n = new_node(p, N_FOR); if (!n) return 0;
            p_advance(p);
            if (!p_expect(p, TT_LPAREN, "(")) return 0;
            /* init */
            if (p->cur.type == TT_SEMI) { p_advance(p); }
            else if (p->cur.type == TT_IDENT && is_type_kw(p->cur.sval) && p->nxt.type == TT_IDENT) {
                n->a = parse_simple_vardecl_noterm(p); if (!n->a) return 0;
                if (!p_expect(p, TT_SEMI, ";")) return 0;
            } else {
                force_node *es = new_node(p, N_EXPRSTMT); if (!es) return 0;
                es->a = parse_expr(p); if (!es->a) return 0;
                n->a = es;
                if (!p_expect(p, TT_SEMI, ";")) return 0;
            }
            /* cond */
            if (p->cur.type != TT_SEMI) { n->b = parse_expr(p); if (!n->b) return 0; }
            if (!p_expect(p, TT_SEMI, ";")) return 0;
            /* post */
            if (p->cur.type != TT_RPAREN) { n->c = parse_expr(p); if (!n->c) return 0; }
            if (!p_expect(p, TT_RPAREN, ")")) return 0;
            n->d = parse_statement(p); if (!n->d) return 0;
            return n;
        }
        case TT_BREAK:   { force_node *n = new_node(p, N_BREAK); if (!n) return 0; p_advance(p); if (!p_expect(p, TT_SEMI, ";")) return 0; return n; }
        case TT_CONTINUE:{ force_node *n = new_node(p, N_CONTINUE); if (!n) return 0; p_advance(p); if (!p_expect(p, TT_SEMI, ";")) return 0; return n; }
        case TT_RETURN:  {
            force_node *n = new_node(p, N_RETURN); if (!n) return 0; p_advance(p);
            if (p->cur.type != TT_SEMI) { n->a = parse_expr(p); if (!n->a) return 0; }
            if (!p_expect(p, TT_SEMI, ";")) return 0;
            return n;
        }
        case TT_SEMI:    { force_node *n = new_node(p, N_EXPRSTMT); if (!n) return 0; p_advance(p); return n; }
        case TT_IDENT:
            if (is_type_kw(p->cur.sval) && p->nxt.type == TT_IDENT) return parse_vardecl(p);
            /* fallthrough */
        default: {
            force_node *n = new_node(p, N_EXPRSTMT); if (!n) return 0;
            n->a = parse_expr(p); if (!n->a) return 0;
            if (!p_expect(p, TT_SEMI, ";")) return 0;
            (void)c;
            return n;
        }
    }
}

/* parse whole program into an N_BLOCK of top items */
static force_node *parse_program(force_ctx *c, const char *src, uint32 len) {
    parser p;
    p.ctx = c; p.src = src; p.len = len; p.pos = 0; p.line = 1;
    p.cur.type = TT_EOF; p.nxt.type = TT_EOF;
    p.nxt = lex_next(&p);
    p_advance(&p);   /* load cur */
    if (c->has_err) return 0;

    force_node *prog = new_node(&p, N_BLOCK); if (!prog) return 0;
    nodelist nl = {0, 0, 0};
    while (p.cur.type != TT_EOF && !c->has_err) {
        force_node *st = parse_statement(&p); if (!st) return 0;
        if (!nl_push(&p, &nl, st)) return 0;
    }
    if (c->has_err) return 0;
    prog->list = nl.list; prog->nlist = nl.count;
    return prog;
}

/* ==================================================================== */
/* 9. Evaluator                                                          */
/* ==================================================================== */

static int eval_guard(force_ctx *c) {
    char probe;
    if (c->has_err) return 0;
    if (c->steps++ > FORCE_MAX_STEPS) { err_set(c, FORCE_E_LIMIT, 0, "step budget exceeded"); return 0; }
    if (c->stack_base - (uintptr_t)&probe > FORCE_STACK_BUDGET) { err_set(c, FORCE_E_LIMIT, 0, "stack budget exceeded"); return 0; }
    return 1;
}

static force_value eval_expr(force_ctx *c, force_env *env, force_node *n);
static force_flow  exec_stmt(force_ctx *c, force_env *env, force_node *n);
static force_flow  exec_stmts(force_ctx *c, force_env *env, force_node **list, uint32 nlist);
static force_value call_native(force_ctx *c, int id, force_value *args, uint32 nargs);

/* output routing */
static void out_write(force_ctx *c, const char *s, uint32 n) {
    if (c->sink) { c->sink(s, n); return; }
    for (uint32 i = 0; i < n; i++) {
        if (c->out_len + 1 < FORCE_OUTBUF_CAP) c->outbuf[c->out_len++] = s[i];
        else { c->out_trunc = 1; break; }
    }
    if (c->out_len < FORCE_OUTBUF_CAP) c->outbuf[c->out_len] = 0;
}

static __attribute__((unused)) int64 to_int_or_err(force_ctx *c, force_value v, const char *ctxmsg) {
    if (v.type == FV_INT || v.type == FV_BOOL) return v.u.i;
    err_set2(c, FORCE_E_RUNTIME, 0, "expected int for ", ctxmsg);
    return 0;
}

static force_value eval_binary(force_ctx *c, int op, force_value L, force_value R) {
    /* string concat */
    if (op == TT_PLUS && L.type == FV_STR && R.type == FV_STR) {
        uint32 ln = L.u.s->len, rn = R.u.s->len;
        if ((uint64)ln + rn > FORCE_MAX_STR) { err_set(c, FORCE_E_LIMIT, 0, "string length cap exceeded"); return V_NULL(); }
        force_str *s = (force_str *)force_alloc(c, (uint32)sizeof(force_str) + ln + rn);
        if (!s) { err_set(c, FORCE_E_NOMEM, 0, "out of memory (concat)"); return V_NULL(); }
        s->rc = 1; s->len = ln + rn;
        memcpy(s->data, L.u.s->data, ln);
        memcpy(s->data + ln, R.u.s->data, rn);
        s->data[ln + rn] = 0;
        force_value v; v.type = FV_STR; v.u.s = s; return v;
    }

    /* equality works across types */
    if (op == TT_EQ) return V_BOOL(fv_equal(L, R));
    if (op == TT_NE) return V_BOOL(!fv_equal(L, R));

    int lnum = (L.type == FV_INT || L.type == FV_FLOAT || L.type == FV_BOOL);
    int rnum = (R.type == FV_INT || R.type == FV_FLOAT || R.type == FV_BOOL);

    /* bitwise / shift: ints only */
    if (op == TT_BAND || op == TT_BOR || op == TT_BXOR || op == TT_SHL || op == TT_SHR) {
        if (L.type != FV_INT || R.type != FV_INT) { err_set(c, FORCE_E_RUNTIME, 0, "bitwise op requires int operands"); return V_NULL(); }
        int64 a = L.u.i, b = R.u.i;
        switch (op) {
            case TT_BAND: return V_INT(a & b);
            case TT_BOR:  return V_INT(a | b);
            case TT_BXOR: return V_INT(a ^ b);
            case TT_SHL:  return V_INT((int64)((uint64)a << (b & 63)));
            case TT_SHR:  return V_INT(a >> (b & 63));
        }
    }

    if (!lnum || !rnum) {
        if (op == TT_PLUS && (L.type == FV_STR || R.type == FV_STR))
            err_set(c, FORCE_E_RUNTIME, 0, "cannot + string and non-string (use str())");
        else
            err_set(c, FORCE_E_RUNTIME, 0, "operator requires numeric operands");
        return V_NULL();
    }

    int is_float = (L.type == FV_FLOAT || R.type == FV_FLOAT);
    if (is_float) {
        double a = (L.type == FV_FLOAT) ? L.u.f : (double)L.u.i;
        double b = (R.type == FV_FLOAT) ? R.u.f : (double)R.u.i;
        switch (op) {
            case TT_PLUS:  return V_FLOAT(a + b);
            case TT_MINUS: return V_FLOAT(a - b);
            case TT_STAR:  return V_FLOAT(a * b);
            case TT_SLASH: if (b == 0.0) { err_set(c, FORCE_E_RUNTIME, 0, "division by zero"); return V_NULL(); } return V_FLOAT(a / b);
            case TT_PERCENT: err_set(c, FORCE_E_RUNTIME, 0, "%% requires int operands"); return V_NULL();
            case TT_LT: return V_BOOL(a < b);
            case TT_LE: return V_BOOL(a <= b);
            case TT_GT: return V_BOOL(a > b);
            case TT_GE: return V_BOOL(a >= b);
        }
    } else {
        int64 a = L.u.i, b = R.u.i;
        switch (op) {
            case TT_PLUS:  return V_INT(a + b);
            case TT_MINUS: return V_INT(a - b);
            case TT_STAR:  return V_INT(a * b);
            case TT_SLASH: if (b == 0) { err_set(c, FORCE_E_RUNTIME, 0, "division by zero"); return V_NULL(); } return V_INT(a / b);
            case TT_PERCENT: if (b == 0) { err_set(c, FORCE_E_RUNTIME, 0, "modulo by zero"); return V_NULL(); } return V_INT(a % b);
            case TT_LT: return V_BOOL(a < b);
            case TT_LE: return V_BOOL(a <= b);
            case TT_GT: return V_BOOL(a > b);
            case TT_GE: return V_BOOL(a >= b);
        }
    }
    err_set(c, FORCE_E_RUNTIME, 0, "bad binary operator");
    return V_NULL();
}

/* call a user or native function */
static force_value eval_call(force_ctx *c, force_env *env, force_node *n) {
    force_value callee = eval_expr(c, env, n->a);
    if (c->has_err) { fv_decref(c, callee); return V_NULL(); }
    if (callee.type != FV_FN || !callee.u.fn) {
        fv_decref(c, callee);
        err_set(c, FORCE_E_RUNTIME, n->line, "attempt to call a non-function");
        return V_NULL();
    }
    force_fn *fn = callee.u.fn;

    /* evaluate args */
    force_value args[FORCE_MAX_ARGS];
    uint32 na = n->nlist;
    if (na > FORCE_MAX_ARGS) na = FORCE_MAX_ARGS;
    uint32 i;
    for (i = 0; i < na; i++) {
        args[i] = eval_expr(c, env, n->list[i]);
        if (c->has_err) { for (uint32 j = 0; j < i; j++) fv_decref(c, args[j]); return V_NULL(); }
    }

    force_value result = V_NULL();

    if (fn->is_native) {
        result = call_native(c, fn->native_id, args, na);
    } else {
        force_node *def = fn->def;
        if (n->nlist != def->nlist) {
            err_set(c, FORCE_E_RUNTIME, n->line, "wrong number of arguments");
        } else if (c->depth + 1 > FORCE_MAX_DEPTH) {
            err_set(c, FORCE_E_LIMIT, n->line, "recursion depth cap exceeded");
        } else {
            c->depth++;
            force_env *fenv = env_new(c, c->global);   /* lexical: parent is global */
            if (fenv) {
                for (i = 0; i < def->nlist && !c->has_err; i++)
                    env_define(c, fenv, def->list[i]->sval, args[i]);
                if (!c->has_err) {
                    force_flow fl = exec_stmts(c, fenv, def->a->list, def->a->nlist);
                    if (fl == FR_RETURN) { result = c->ret_value; c->ret_value = V_NULL(); }
                }
                env_free(c, fenv);
            }
            c->depth--;
        }
    }

    for (i = 0; i < na; i++) fv_decref(c, args[i]);
    return result;
}

static force_value eval_expr(force_ctx *c, force_env *env, force_node *n) {
    if (!eval_guard(c)) return V_NULL();
    switch (n->kind) {
        case N_INT:   return V_INT(n->ival);
        case N_FLOAT: return V_FLOAT(n->fval);
        case N_BOOL:  return V_BOOL((int)n->ival);
        case N_NULL:  return V_NULL();
        case N_STR:   return make_str(c, n->sval, (uint32)n->ival);
        case N_IDENT: {
            force_value v;
            if (env_get(c, env, n->sval, &v)) return v;
            err_set2(c, FORCE_E_RUNTIME, n->line, "undefined variable ", n->sval);
            return V_NULL();
        }
        case N_ARRAY: {
            force_value av = make_arr(c);
            if (c->has_err) return V_NULL();
            for (uint32 i = 0; i < n->nlist; i++) {
                force_value e = eval_expr(c, env, n->list[i]);
                if (c->has_err) { fv_decref(c, e); fv_decref(c, av); return V_NULL(); }
                arr_push(c, av.u.a, e);
                fv_decref(c, e);
                if (c->has_err) { fv_decref(c, av); return V_NULL(); }
            }
            return av;
        }
        case N_UNARY: {
            force_value v = eval_expr(c, env, n->a);
            if (c->has_err) { fv_decref(c, v); return V_NULL(); }
            force_value r = V_NULL();
            if (n->op == TT_NOT) r = V_BOOL(!fv_truthy(v));
            else if (n->op == TT_MINUS) {
                if (v.type == FV_INT) r = V_INT(-v.u.i);
                else if (v.type == FV_FLOAT) r = V_FLOAT(-v.u.f);
                else err_set(c, FORCE_E_RUNTIME, n->line, "unary - requires number");
            } else if (n->op == TT_BNOT) {
                if (v.type == FV_INT) r = V_INT(~v.u.i);
                else err_set(c, FORCE_E_RUNTIME, n->line, "unary ~ requires int");
            }
            fv_decref(c, v);
            return r;
        }
        case N_LOGICAL: {
            force_value l = eval_expr(c, env, n->a);
            if (c->has_err) { fv_decref(c, l); return V_NULL(); }
            int lt = fv_truthy(l);
            if (n->op == TT_AND) {
                if (!lt) return l;              /* short-circuit, return left */
                fv_decref(c, l);
                return eval_expr(c, env, n->b);
            } else { /* OR */
                if (lt) return l;
                fv_decref(c, l);
                return eval_expr(c, env, n->b);
            }
        }
        case N_BINARY: {
            force_value l = eval_expr(c, env, n->a);
            if (c->has_err) { fv_decref(c, l); return V_NULL(); }
            force_value r = eval_expr(c, env, n->b);
            if (c->has_err) { fv_decref(c, l); fv_decref(c, r); return V_NULL(); }
            force_value res = eval_binary(c, n->op, l, r);
            fv_decref(c, l); fv_decref(c, r);
            return res;
        }
        case N_INDEX: {
            force_value base = eval_expr(c, env, n->a);
            if (c->has_err) { fv_decref(c, base); return V_NULL(); }
            force_value iv = eval_expr(c, env, n->b);
            if (c->has_err) { fv_decref(c, base); fv_decref(c, iv); return V_NULL(); }
            force_value res = V_NULL();
            if (iv.type != FV_INT) err_set(c, FORCE_E_RUNTIME, n->line, "index must be int");
            else if (base.type == FV_ARRAY) {
                int64 idx = iv.u.i;
                if (idx < 0 || (uint64)idx >= base.u.a->len) err_set(c, FORCE_E_RUNTIME, n->line, "array index out of range");
                else { res = base.u.a->items[idx]; fv_incref(res); }
            } else if (base.type == FV_STR) {
                int64 idx = iv.u.i;
                if (idx < 0 || (uint64)idx >= base.u.s->len) err_set(c, FORCE_E_RUNTIME, n->line, "string index out of range");
                else res = V_INT((int64)(uint8)base.u.s->data[idx]);
            } else err_set(c, FORCE_E_RUNTIME, n->line, "cannot index this value");
            fv_decref(c, base); fv_decref(c, iv);
            return res;
        }
        case N_ASSIGN: {
            force_value rhs;
            /* compound: read current, combine */
            if (n->op != TT_ASSIGN) {
                force_value cur = eval_expr(c, env, n->a);
                if (c->has_err) { fv_decref(c, cur); return V_NULL(); }
                force_value delta = eval_expr(c, env, n->b);
                if (c->has_err) { fv_decref(c, cur); fv_decref(c, delta); return V_NULL(); }
                int binop = TT_PLUS;
                if (n->op == TT_MINUSEQ) binop = TT_MINUS;
                else if (n->op == TT_STAREQ) binop = TT_STAR;
                else if (n->op == TT_SLASHEQ) binop = TT_SLASH;
                else if (n->op == TT_PERCENTEQ) binop = TT_PERCENT;
                rhs = eval_binary(c, binop, cur, delta);
                fv_decref(c, cur); fv_decref(c, delta);
                if (c->has_err) { fv_decref(c, rhs); return V_NULL(); }
            } else {
                rhs = eval_expr(c, env, n->b);
                if (c->has_err) { fv_decref(c, rhs); return V_NULL(); }
            }
            /* store */
            if (n->a->kind == N_IDENT) {
                if (!env_assign(c, env, n->a->sval, rhs)) {
                    err_set2(c, FORCE_E_RUNTIME, n->line, "assignment to undefined variable ", n->a->sval);
                    fv_decref(c, rhs); return V_NULL();
                }
            } else { /* N_INDEX */
                force_node *ix = n->a;
                force_value base = eval_expr(c, env, ix->a);
                if (c->has_err) { fv_decref(c, base); fv_decref(c, rhs); return V_NULL(); }
                force_value iv = eval_expr(c, env, ix->b);
                if (c->has_err) { fv_decref(c, base); fv_decref(c, iv); fv_decref(c, rhs); return V_NULL(); }
                if (iv.type != FV_INT) err_set(c, FORCE_E_RUNTIME, n->line, "index must be int");
                else if (base.type != FV_ARRAY) err_set(c, FORCE_E_RUNTIME, n->line, "cannot index-assign this value");
                else {
                    int64 idx = iv.u.i;
                    if (idx < 0 || (uint64)idx >= base.u.a->len) err_set(c, FORCE_E_RUNTIME, n->line, "array index out of range");
                    else { fv_decref(c, base.u.a->items[idx]); fv_incref(rhs); base.u.a->items[idx] = rhs; }
                }
                fv_decref(c, base); fv_decref(c, iv);
                if (c->has_err) { fv_decref(c, rhs); return V_NULL(); }
            }
            return rhs;   /* assignment is an expression */
        }
        case N_CALL: return eval_call(c, env, n);
    }
    err_set(c, FORCE_E_RUNTIME, n->line, "cannot evaluate node");
    return V_NULL();
}

static force_flow exec_stmts(force_ctx *c, force_env *env, force_node **list, uint32 nlist) {
    for (uint32 i = 0; i < nlist; i++) {
        force_flow fl = exec_stmt(c, env, list[i]);
        if (fl != FR_OK) return fl;
        if (c->has_err) return FR_ERROR;
    }
    return FR_OK;
}

static force_flow exec_stmt(force_ctx *c, force_env *env, force_node *n) {
    if (!eval_guard(c)) return FR_ERROR;
    switch (n->kind) {
        case N_FNDEF: {
            force_fn *fn = (force_fn *)force_alloc(c, (uint32)sizeof(force_fn));
            if (!fn) { err_set(c, FORCE_E_NOMEM, n->line, "out of memory (fn)"); return FR_ERROR; }
            fn->is_native = 0; fn->native_id = -1; fn->name = n->sval; fn->def = n;
            force_value v; v.type = FV_FN; v.u.fn = fn;
            env_define(c, c->global, n->sval, v);
            return c->has_err ? FR_ERROR : FR_OK;
        }
        case N_VARDECL: {
            force_value v = V_NULL();
            if (n->a) { v = eval_expr(c, env, n->a); if (c->has_err) { fv_decref(c, v); return FR_ERROR; } }
            env_define(c, env, n->sval, v);
            fv_decref(c, v);
            return c->has_err ? FR_ERROR : FR_OK;
        }
        case N_EXPRSTMT: {
            if (n->a) { force_value v = eval_expr(c, env, n->a); fv_decref(c, v); }
            return c->has_err ? FR_ERROR : FR_OK;
        }
        case N_BLOCK: {
            force_env *be = env_new(c, env);
            if (!be) return FR_ERROR;
            force_flow fl = exec_stmts(c, be, n->list, n->nlist);
            env_free(c, be);
            return fl;
        }
        case N_IF: {
            force_value cnd = eval_expr(c, env, n->a);
            if (c->has_err) { fv_decref(c, cnd); return FR_ERROR; }
            int t = fv_truthy(cnd); fv_decref(c, cnd);
            if (t) return exec_stmt(c, env, n->b);
            else if (n->c) return exec_stmt(c, env, n->c);
            return FR_OK;
        }
        case N_WHILE: {
            for (;;) {
                if (!eval_guard(c)) return FR_ERROR;
                force_value cnd = eval_expr(c, env, n->a);
                if (c->has_err) { fv_decref(c, cnd); return FR_ERROR; }
                int t = fv_truthy(cnd); fv_decref(c, cnd);
                if (!t) break;
                force_flow fl = exec_stmt(c, env, n->b);
                if (fl == FR_BREAK) break;
                if (fl == FR_RETURN || fl == FR_ERROR) return fl;
                /* FR_CONTINUE / FR_OK -> loop */
            }
            return FR_OK;
        }
        case N_FOR: {
            force_env *fe = env_new(c, env);
            if (!fe) return FR_ERROR;
            force_flow ret = FR_OK;
            if (n->a) { force_flow fl = exec_stmt(c, fe, n->a); if (fl != FR_OK || c->has_err) { env_free(c, fe); return c->has_err ? FR_ERROR : fl; } }
            for (;;) {
                if (!eval_guard(c)) { ret = FR_ERROR; break; }
                if (n->b) {
                    force_value cnd = eval_expr(c, fe, n->b);
                    if (c->has_err) { fv_decref(c, cnd); ret = FR_ERROR; break; }
                    int t = fv_truthy(cnd); fv_decref(c, cnd);
                    if (!t) break;
                }
                force_flow fl = exec_stmt(c, fe, n->d);
                if (fl == FR_BREAK) break;
                if (fl == FR_RETURN || fl == FR_ERROR) { ret = fl; break; }
                if (n->c) { force_value pv = eval_expr(c, fe, n->c); fv_decref(c, pv); if (c->has_err) { ret = FR_ERROR; break; } }
            }
            env_free(c, fe);
            return ret;
        }
        case N_BREAK:    return FR_BREAK;
        case N_CONTINUE: return FR_CONTINUE;
        case N_RETURN: {
            force_value v = V_NULL();
            if (n->a) { v = eval_expr(c, env, n->a); if (c->has_err) { fv_decref(c, v); return FR_ERROR; } }
            c->ret_value = v;   /* transfer ownership to ret channel */
            return FR_RETURN;
        }
        default: {
            /* expression used as statement */
            force_value v = eval_expr(c, env, n);
            fv_decref(c, v);
            return c->has_err ? FR_ERROR : FR_OK;
        }
    }
}

/* ==================================================================== */
/* 10. Native builtins                                                   */
/* ==================================================================== */

static int parse_str_to_int(const char *s, uint32 len, int64 *out) {
    uint32 i = 0; int neg = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); i++; }
    uint64 v = 0; int any = 0; int base = 10;
    if (i + 1 < len && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) { base = 16; i += 2; }
    else if (i + 1 < len && s[i] == '0' && (s[i+1] == 'b' || s[i+1] == 'B')) { base = 2; i += 2; }
    for (; i < len; i++) {
        char ch = s[i]; int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (uint64)base + (uint64)d; any = 1;
    }
    if (!any) return 0;
    *out = neg ? -(int64)v : (int64)v;
    return 1;
}

static int parse_str_to_float(const char *s, uint32 len, double *out) {
    uint32 i = 0; int neg = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); i++; }
    double v = 0; int any = 0;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) { v = v * 10.0 + (s[i] - '0'); any = 1; }
    if (i < len && s[i] == '.') { i++; double f = 0.1; for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) { v += (s[i]-'0')*f; f *= 0.1; any = 1; } }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++; int en = 0; if (i < len && (s[i]=='+'||s[i]=='-')) { en = (s[i]=='-'); i++; }
        int e = 0; int ea = 0; for (; i < len && s[i]>='0'&&s[i]<='9'; i++) { e = e*10+(s[i]-'0'); ea=1; }
        if (ea) { double m = 1.0; for (int k=0;k<e&&k<308;k++) m*=10.0; if (en) v/=m; else v*=m; }
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* printf-ish */
static force_value bi_printf(force_ctx *c, force_value *args, uint32 nargs) {
    if (nargs < 1 || args[0].type != FV_STR) { err_set(c, FORCE_E_RUNTIME, 0, "printf: first arg must be a format string"); return V_NULL(); }
    const char *f = args[0].u.s->data; uint32 flen = args[0].u.s->len;
    uint32 ai = 1;
    char line[FORCE_OUTBUF_CAP]; sbuf s; sb_init(&s, line, sizeof(line));
    for (uint32 i = 0; i < flen; i++) {
        char ch = f[i];
        if (ch != '%') { sb_c(&s, ch); continue; }
        i++;
        if (i >= flen) { err_set(c, FORCE_E_RUNTIME, 0, "printf: trailing %"); return V_NULL(); }
        char cv = f[i];
        if (cv == '%') { sb_c(&s, '%'); continue; }
        if (ai >= nargs) { err_set(c, FORCE_E_RUNTIME, 0, "printf: not enough arguments"); return V_NULL(); }
        force_value a = args[ai++];
        switch (cv) {
            case 'd': case 'i':
                if (a.type == FV_INT || a.type == FV_BOOL) sb_i(&s, a.u.i);
                else if (a.type == FV_FLOAT) sb_i(&s, (int64)a.u.f);
                else { err_set(c, FORCE_E_RUNTIME, 0, "printf: %d needs a number"); return V_NULL(); }
                break;
            case 'f':
                if (a.type == FV_FLOAT) sb_f(&s, a.u.f);
                else if (a.type == FV_INT || a.type == FV_BOOL) sb_f(&s, (double)a.u.i);
                else { err_set(c, FORCE_E_RUNTIME, 0, "printf: %f needs a number"); return V_NULL(); }
                break;
            case 's':
                if (a.type == FV_STR) sb_sn(&s, a.u.s->data, a.u.s->len);
                else value_to_sb(c, &s, a, 0);
                break;
            case 'b':
                sb_s(&s, fv_truthy(a) ? "true" : "false"); break;
            case 'x':
                if (a.type == FV_INT || a.type == FV_BOOL) sb_hex(&s, (uint64)a.u.i);
                else { err_set(c, FORCE_E_RUNTIME, 0, "printf: %x needs an int"); return V_NULL(); }
                break;
            default:
                err_set(c, FORCE_E_RUNTIME, 0, "printf: unknown format specifier"); return V_NULL();
        }
    }
    out_write(c, line, s.len);
    return V_INT((int64)s.len);
}

static force_value call_native(force_ctx *c, int id, force_value *args, uint32 nargs) {
    switch (id) {
        case BI_PRINT:
        case BI_PRINTLN: {
            char line[FORCE_OUTBUF_CAP]; sbuf s; sb_init(&s, line, sizeof(line));
            for (uint32 i = 0; i < nargs; i++) { if (i) sb_c(&s, ' '); value_to_sb(c, &s, args[i], 0); }
            if (id == BI_PRINTLN) sb_c(&s, '\n');
            out_write(c, line, s.len);
            return V_NULL();
        }
        case BI_PRINTF: return bi_printf(c, args, nargs);
        case BI_STR: {
            if (nargs != 1) { err_set(c, FORCE_E_RUNTIME, 0, "str() takes 1 argument"); return V_NULL(); }
            char tmp[2048]; sbuf s; sb_init(&s, tmp, sizeof(tmp));
            value_to_sb(c, &s, args[0], 0);
            return make_str(c, tmp, s.len);
        }
        case BI_INT: {
            if (nargs != 1) { err_set(c, FORCE_E_RUNTIME, 0, "int() takes 1 argument"); return V_NULL(); }
            force_value a = args[0];
            if (a.type == FV_INT || a.type == FV_BOOL) return V_INT(a.u.i);
            if (a.type == FV_FLOAT) return V_INT((int64)a.u.f);
            if (a.type == FV_STR) { int64 v; if (parse_str_to_int(a.u.s->data, a.u.s->len, &v)) return V_INT(v); err_set(c, FORCE_E_RUNTIME, 0, "int(): cannot parse string"); return V_NULL(); }
            err_set(c, FORCE_E_RUNTIME, 0, "int(): unsupported type"); return V_NULL();
        }
        case BI_FLOAT: {
            if (nargs != 1) { err_set(c, FORCE_E_RUNTIME, 0, "float() takes 1 argument"); return V_NULL(); }
            force_value a = args[0];
            if (a.type == FV_FLOAT) return a;
            if (a.type == FV_INT || a.type == FV_BOOL) return V_FLOAT((double)a.u.i);
            if (a.type == FV_STR) { double v; if (parse_str_to_float(a.u.s->data, a.u.s->len, &v)) return V_FLOAT(v); err_set(c, FORCE_E_RUNTIME, 0, "float(): cannot parse string"); return V_NULL(); }
            err_set(c, FORCE_E_RUNTIME, 0, "float(): unsupported type"); return V_NULL();
        }
        case BI_BOOL:
            if (nargs != 1) { err_set(c, FORCE_E_RUNTIME, 0, "bool() takes 1 argument"); return V_NULL(); }
            return V_BOOL(fv_truthy(args[0]));
        case BI_LEN: {
            if (nargs != 1) { err_set(c, FORCE_E_RUNTIME, 0, "len() takes 1 argument"); return V_NULL(); }
            force_value a = args[0];
            if (a.type == FV_STR) return V_INT((int64)a.u.s->len);
            if (a.type == FV_ARRAY) return V_INT((int64)a.u.a->len);
            err_set(c, FORCE_E_RUNTIME, 0, "len(): needs string or array"); return V_NULL();
        }
        case BI_PUSH: {
            if (nargs != 2 || args[0].type != FV_ARRAY) { err_set(c, FORCE_E_RUNTIME, 0, "push(array, value)"); return V_NULL(); }
            if (!arr_push(c, args[0].u.a, args[1])) return V_NULL();
            return V_INT((int64)args[0].u.a->len);
        }
        case BI_POP: {
            if (nargs != 1 || args[0].type != FV_ARRAY) { err_set(c, FORCE_E_RUNTIME, 0, "pop(array)"); return V_NULL(); }
            force_arr *a = args[0].u.a;
            if (a->len == 0) { err_set(c, FORCE_E_RUNTIME, 0, "pop(): empty array"); return V_NULL(); }
            force_value v = a->items[--a->len];   /* transfer ref */
            return v;
        }
        case BI_TYPE:
            if (nargs != 1) { err_set(c, FORCE_E_RUNTIME, 0, "type() takes 1 argument"); return V_NULL(); }
            return make_str(c, fv_typename(args[0]), (uint32)strlen(fv_typename(args[0])));
        case BI_SLEEP_MS: {
            if (nargs != 1 || (args[0].type != FV_INT && args[0].type != FV_FLOAT)) { err_set(c, FORCE_E_RUNTIME, 0, "sleep_ms(n)"); return V_NULL(); }
            int64 ms = (args[0].type == FV_INT) ? args[0].u.i : (int64)args[0].u.f;
            if (ms < 0) ms = 0;
            if (ms > (int64)FORCE_MAX_SLEEP_MS) ms = FORCE_MAX_SLEEP_MS;
            timer_sleep_ms((uint32)ms);
            return V_NULL();
        }
        case BI_MEM_STATS: {
            memory_stats_t st = memory_get_stats();
            force_value av = make_arr(c);
            if (c->has_err) return V_NULL();
            arr_push(c, av.u.a, V_INT((int64)st.heap_used_kb * 1024));
            arr_push(c, av.u.a, V_INT((int64)st.heap_free_kb * 1024));
            return av;
        }
        case BI_TICKS:
            return V_INT((int64)get_system_timer_ticks());
    }
    err_set(c, FORCE_E_RUNTIME, 0, "unknown builtin");
    return V_NULL();
}

/* ==================================================================== */
/* 11. Program run + context lifecycle + public API                      */
/* ==================================================================== */

static void register_builtins(force_ctx *c) {
    for (int i = 0; i < BI__COUNT; i++) {
        force_value v; v.type = FV_FN; v.u.fn = &g_builtin_fns[i];
        /* name is a static const char*, cast away const for env storage */
        env_define(c, c->global, (char *)g_builtin_names[i], v);
    }
}

static void run_program(force_ctx *c, force_node *prog) {
    /* pass 1: hoist top-level fn defs */
    for (uint32 i = 0; i < prog->nlist && !c->has_err; i++) {
        if (prog->list[i]->kind == N_FNDEF) exec_stmt(c, c->global, prog->list[i]);
    }
    /* pass 2: execute non-fn top items in order */
    for (uint32 i = 0; i < prog->nlist && !c->has_err; i++) {
        if (prog->list[i]->kind == N_FNDEF) continue;
        force_flow fl = exec_stmt(c, c->global, prog->list[i]);
        if (fl == FR_ERROR) break;
        if (fl == FR_BREAK || fl == FR_CONTINUE) { err_set(c, FORCE_E_RUNTIME, prog->list[i]->line, "break/continue outside loop"); break; }
        if (fl == FR_RETURN) { fv_decref(c, c->ret_value); c->ret_value = V_NULL(); break; }
    }
}

typedef struct { force_ctx *ctx; const char *src; uint32 len; } exec_args;

static int force_exec_entry(void *pv) {
    volatile char anchor;
    exec_args *a = (exec_args *)pv;
    force_ctx *c = a->ctx;
    c->stack_base = (uintptr_t)&anchor;

    force_node *prog = parse_program(c, a->src, a->len);
    if (c->has_err || !prog) return c->err_code ? c->err_code : FORCE_E_PARSE;
    run_program(c, prog);
    if (c->has_err) return c->err_code ? c->err_code : FORCE_E_RUNTIME;
    return FORCE_OK;
}

int force_init(void) {
    if (g_force_ready) return 1;
    for (int i = 0; i < BI__COUNT; i++) {
        g_builtin_fns[i].is_native = 1;
        g_builtin_fns[i].native_id = i;
        g_builtin_fns[i].name = g_builtin_names[i];
        g_builtin_fns[i].def = 0;
    }
    /* dedicated interpreter stack so deep recursion cannot smash the task
     * kernel stack; a NULL result simply falls back to the caller stack. */
    g_force_stack = (uint8 *)kmalloc(FORCE_STACK_BYTES);
    if (g_force_stack) {
        uintptr_t top = (uintptr_t)(g_force_stack + FORCE_STACK_BYTES);
        top &= ~(uintptr_t)0xF;   /* 16-byte align */
        g_force_stack_top = (void *)top;
    } else {
        g_force_stack_top = 0;
    }
    g_force_ready = 1;
    return 1;
}

force_ctx *force_ctx_create(void) {
    force_ctx *c = (force_ctx *)kmalloc(sizeof(force_ctx));
    if (!c) return 0;
    memset(c, 0, sizeof(force_ctx));
    if (!g_force_ready) force_init();
    c->global = env_new(c, 0);
    if (!c->global) { force_free_all(c); kfree(c); return 0; }
    register_builtins(c);
    if (c->has_err) { force_free_all(c); kfree(c); return 0; }
    return c;
}

void force_ctx_destroy(force_ctx *c) {
    if (!c) return;
    force_free_all(c);   /* frees EVERY tracked block: env, heap, AST, cycles */
    kfree(c);
}

void force_ctx_reset(force_ctx *c) {
    if (!c) return;
    force_free_all(c);
    c->steps = 0; c->depth = 0; c->nodes = 0; c->tokens = 0;
    c->has_err = 0; c->err_code = 0; c->err_line = 0; c->err[0] = 0;
    c->out_len = 0; c->out_trunc = 0; c->ret_value = V_NULL();
    c->global = env_new(c, 0);
    if (c->global) register_builtins(c);
}

void force_ctx_set_output(force_ctx *c, void (*sink)(const char *, uint32)) {
    if (c) c->sink = sink;
}

static uint32 force_strnlen(const char *s, uint32 max) {
    uint32 i = 0; while (i < max && s[i]) i++; return i;
}

int force_eval_ctx(force_ctx *c, const char *src, char *out_msg, uint32 out_sz) {
    if (!c || !src || !out_msg || out_sz == 0) {
        if (out_msg && out_sz) out_msg[0] = 0;
        return FORCE_E_ARG;
    }
    if (c->busy) {
        sbuf s; sb_init(&s, out_msg, out_sz); sb_s(&s, "force: context busy");
        return FORCE_E_RUNTIME;
    }
    c->busy = 1;

    /* per-eval reset (heap/global persist for REPL semantics) */
    c->steps = 0; c->depth = 0; c->nodes = 0; c->tokens = 0;
    c->has_err = 0; c->err_code = 0; c->err_line = 0; c->err[0] = 0;
    c->out_len = 0; c->out_trunc = 0; c->ret_value = V_NULL();

    uint32 slen = force_strnlen(src, FORCE_MAX_SRC + 1);
    int rc;
    if (slen > FORCE_MAX_SRC) {
        err_set(c, FORCE_E_LIMIT, 0, "source too long");
        rc = FORCE_E_LIMIT;
    } else {
        exec_args a; a.ctx = c; a.src = src; a.len = slen;
        if (g_force_stack_top) rc = force_call_on_stack(g_force_stack_top, force_exec_entry, &a);
        else rc = force_exec_entry(&a);
    }

    /* build out_msg */
    sbuf s; sb_init(&s, out_msg, out_sz);
    if (c->has_err) {
        sb_s(&s, "force: ");
        sb_s(&s, c->err);
        if (c->err_line > 0) { sb_s(&s, " (line "); sb_i(&s, c->err_line); sb_c(&s, ')'); }
    } else {
        /* captured program output */
        sb_sn(&s, c->outbuf, c->out_len);
        if (c->out_trunc) sb_s(&s, "\n[output truncated]");
    }

    c->busy = 0;
    return rc;
}

int force_eval_string(const char *src, char *out_msg, uint32 out_sz) {
    if (!src || !out_msg || out_sz == 0) return FORCE_E_ARG;
    force_ctx *c = force_ctx_create();
    if (!c) { sbuf s; sb_init(&s, out_msg, out_sz); sb_s(&s, "force: out of memory"); return (int)force_strnlen(out_msg, out_sz); }
    force_eval_ctx(c, src, out_msg, out_sz);
    force_ctx_destroy(c);
    return (int)force_strnlen(out_msg, out_sz);   /* bytes in out_msg (incl. error text) */
}

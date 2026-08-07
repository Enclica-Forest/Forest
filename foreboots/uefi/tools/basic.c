/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/tools/basic.c - Minimal BASIC interpreter for the ForeB shell.
 * =============================================================================
 * Freestanding (no libc). Uses con_puts/con_putc/read_line from shell.c for
 * I/O. Supports numbered program lines, PRINT, LET, INPUT, IF/THEN, GOTO,
 * GOSUB/RETURN, FOR/NEXT, REM, END, LIST, RUN, NEW, and basic arithmetic
 * expressions with comparison operators and ABS/INT/RND functions.
 * ==========================================================================*/

#include "basic.h"
#include "shell.h"

/* ---------------------------------------------------------------------------
 * External I/O primitives from shell.c (made non-static for this module).
 * ---------------------------------------------------------------------------*/
extern void con_puts(const char *s);
extern void con_putc(char c);
extern void con_putu(UINT64 v);
extern void con_puti(int v);
extern void con_puthex(UINT64 v, int digits);
extern void con_flush(void);
extern int  read_line(const char *prompt, char *out, int outcap);

/* ---------------------------------------------------------------------------
 * Tiny freestanding string helpers (local copies - no libc).
 * ---------------------------------------------------------------------------*/
static int b_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int b_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int b_ci_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static void b_strcpy(char *dst, const char *src, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static char b_toupper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* ---------------------------------------------------------------------------
 * Program storage.
 * ---------------------------------------------------------------------------*/
#define BASIC_MAX_LINES  256
#define BASIC_LINE_LEN   128

static char basic_program[BASIC_MAX_LINES][BASIC_LINE_LEN];
static int  basic_line_nums[BASIC_MAX_LINES];
static int  basic_line_count = 0;

/* ---------------------------------------------------------------------------
 * Variables: A-Z (26 single-letter numeric variables, INT32).
 * ---------------------------------------------------------------------------*/
static int basic_vars[26];

static int *b_var_ref(char c)
{
    if (c >= 'A' && c <= 'Z') return &basic_vars[c - 'A'];
    if (c >= 'a' && c <= 'z') return &basic_vars[c - 'a'];
    return NULL;
}

/* ---------------------------------------------------------------------------
 * GOSUB / RETURN stack (max depth 16).
 * ---------------------------------------------------------------------------*/
#define BASIC_GOSUB_MAX  16
static int basic_gosub_stack[BASIC_GOSUB_MAX];
static int basic_gosub_sp = 0;

/* ---------------------------------------------------------------------------
 * FOR / NEXT stack (max depth 8).
 * ---------------------------------------------------------------------------*/
#define BASIC_FOR_MAX  8
struct basic_for_frame {
    char var;           /* loop variable (A-Z) */
    int  limit;         /* TO value */
    int  step;          /* STEP value */
    int  line_idx;      /* index into basic_program[] to loop back to */
};
static struct basic_for_frame basic_for_stack[BASIC_FOR_MAX];
static int basic_for_sp = 0;

/* ---------------------------------------------------------------------------
 * Execution state.
 * ---------------------------------------------------------------------------*/
static int basic_running = 0;   /* set to 0 to stop execution */
static int basic_pc = 0;        /* current index into basic_program[] */

/* ---------------------------------------------------------------------------
 * Simple pseudo-random number generator (xorshift32, no libc).
 * ---------------------------------------------------------------------------*/
static UINT32 basic_rng_state = 0xDEADBEEF;

static int b_random_0_99(void)
{
    UINT32 x = basic_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    basic_rng_state = x;
    return (int)(x % 100);
}

/* ---------------------------------------------------------------------------
 * Expression parser (recursive descent).
 *
 * Grammar:
 *   expr      = cmp (('+' | '-') cmp)*
 *   cmp       = mul (('='<|'<>'|'<'|'>'|'<='|'>=') mul)*
 *   mul       = unary (('*'|'/'|'MOD') unary)*
 *   unary     = ('-' unary) | atom
 *   atom      = NUMBER | VARIABLE | '(' expr ')' | FUNC '(' expr ')'
 *
 * Comparison operators return 1 (true) or 0 (false).
 * ---------------------------------------------------------------------------*/
static const char *ep;   /* expression parse pointer */
static int ep_error;     /* set on parse error */

static void skip_ws(void) { while (*ep == ' ' || *ep == '\t') ep++; }

static int parse_number(void)
{
    skip_ws();
    int neg = 0;
    if (*ep == '-') { neg = 1; ep++; }
    else if (*ep == '+') { ep++; }
    int val = 0;
    if (*ep < '0' || *ep > '9') { ep_error = 1; return 0; }
    while (*ep >= '0' && *ep <= '9') { val = val * 10 + (*ep - '0'); ep++; }
    return neg ? -val : val;
}

/* Forward declarations. */
static int eval_expr(void);
static int eval_cmp(void);
static int eval_mul(void);
static int eval_unary(void);
static int eval_atom(void);

static int eval_expr(void)
{
    int v = eval_cmp();
    while (*ep == '+' || *ep == '-') {
        char op = *ep; ep++;
        int r = eval_cmp();
        v = (op == '+') ? v + r : v - r;
    }
    return v;
}

static int eval_cmp(void)
{
    int v = eval_mul();
    skip_ws();
    if (*ep == '=' && ep[1] != '=' && ep[1] != '>' && ep[1] != '<') {
        ep++; int r = eval_mul(); return (v == r) ? 1 : 0;
    }
    if (*ep == '<' && ep[1] == '>') { ep += 2; int r = eval_mul(); return (v != r) ? 1 : 0; }
    if (*ep == '<' && ep[1] == '=') { ep += 2; int r = eval_mul(); return (v <= r) ? 1 : 0; }
    if (*ep == '>' && ep[1] == '=') { ep += 2; int r = eval_mul(); return (v >= r) ? 1 : 0; }
    if (*ep == '<') { ep++; int r = eval_mul(); return (v < r) ? 1 : 0; }
    if (*ep == '>') { ep++; int r = eval_mul(); return (v > r) ? 1 : 0; }
    return v;
}

static int eval_mul(void)
{
    int v = eval_unary();
    for (;;) {
        skip_ws();
        if (*ep == '*') { ep++; v *= eval_unary(); }
        else if (*ep == '/') { ep++; int d = eval_unary(); v = d ? v / d : 0; }
        else if (b_ci_eq(ep, "MOD")) { ep += 3; int d = eval_unary(); v = d ? v % d : 0; }
        else break;
    }
    return v;
}

static int eval_unary(void)
{
    skip_ws();
    if (*ep == '-') { ep++; return -eval_unary(); }
    if (*ep == '+') { ep++; return eval_unary(); }
    return eval_atom();
}

static int eval_atom(void)
{
    skip_ws();

    /* Parenthesized expression. */
    if (*ep == '(') { ep++; int v = eval_expr(); skip_ws(); if (*ep == ')') ep++; return v; }

    /* Number literal. */
    if (*ep >= '0' && *ep <= '9') return parse_number();

    /* Function calls: ABS, INT, RND. */
    if ((b_ci_eq(ep, "ABS") || b_ci_eq(ep, "INT")) &&
        ep[b_strlen(ep) > 3 ? 3 : 3] == '(') {  /* peek for '(' */
        int is_abs = b_ci_eq(ep, "ABS");
        ep += 3; skip_ws();
        if (*ep == '(') ep++;
        int v = eval_expr();
        skip_ws(); if (*ep == ')') ep++;
        return is_abs ? (v < 0 ? -v : v) : v;   /* INT truncates toward zero (already int) */
    }
    if (b_ci_eq(ep, "RND")) {
        ep += 3;
        return b_random_0_99();
    }

    /* Variable reference. */
    if ((*ep >= 'A' && *ep <= 'Z') || (*ep >= 'a' && *ep <= 'z')) {
        int *ref = b_var_ref(*ep);
        ep++;
        return ref ? *ref : 0;
    }

    ep_error = 1;
    return 0;
}

/* Convenience: evaluate an expression from a string, returning result. */
static int b_eval(const char *s)
{
    ep = s;
    ep_error = 0;
    int v = eval_expr();
    return ep_error ? 0 : v;
}

/* ---------------------------------------------------------------------------
 * String literal parser: extracts a quoted string (without quotes) into dst.
 * Returns pointer past the closing quote, or NULL on error.
 * ---------------------------------------------------------------------------*/
static const char *parse_string_literal(const char *src, char *dst, int cap)
{
    if (*src != '"') return NULL;
    src++;
    int i = 0;
    while (*src && *src != '"' && i < cap - 1) { dst[i++] = *src++; }
    dst[i] = 0;
    if (*src == '"') src++;
    return src;
}

/* ---------------------------------------------------------------------------
 * Find a program line by line number. Returns index or -1.
 * ---------------------------------------------------------------------------*/
static int b_find_line(int num)
{
    for (int i = 0; i < basic_line_count; i++)
        if (basic_line_nums[i] == num) return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Insert a numbered line into the program (sorted by line number).
 * Replaces an existing line with the same number.
 * ---------------------------------------------------------------------------*/
static void b_store_line(int num, const char *text)
{
    int idx = b_find_line(num);
    if (idx >= 0) {
        /* Replace existing line. */
        b_strcpy(basic_program[idx], text, BASIC_LINE_LEN);
        return;
    }
    /* Find insertion point (sorted). */
    int ins = basic_line_count;
    for (int i = 0; i < basic_line_count; i++) {
        if (basic_line_nums[i] > num) { ins = i; break; }
    }
    if (basic_line_count >= BASIC_MAX_LINES) {
        con_puts("?OUT OF PROGRAM SPACE\n");
        return;
    }
    /* Shift lines down. */
    for (int i = basic_line_count; i > ins; i--) {
        basic_line_nums[i] = basic_line_nums[i - 1];
        b_strcpy(basic_program[i], basic_program[i - 1], BASIC_LINE_LEN);
    }
    basic_line_nums[ins] = num;
    b_strcpy(basic_program[ins], text, BASIC_LINE_LEN);
    basic_line_count++;
}

/* ---------------------------------------------------------------------------
 * Execute a single BASIC statement. `line` is the text of the statement.
 * Returns: 0 = ok, 1 = goto performed (basic_pc set), -1 = error/stop.
 * ---------------------------------------------------------------------------*/
static int b_exec_stmt(const char *line);

static int b_exec_line(int idx)
{
    if (idx < 0 || idx >= basic_line_count) return -1;
    const char *line = basic_program[idx];
    int result = 0;
    /* A line can contain multiple statements separated by ':' */
    while (*line && result == 0) {
        /* Skip leading whitespace. */
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        result = b_exec_stmt(line);
        if (*line == ':') line++;   /* advance past colon separator */
        /* Skip to next colon (but don't skip inside strings). */
        if (result == 0) {
            while (*line && *line != ':') {
                if (*line == '"') { line++; while (*line && *line != '"') line++; }
                if (*line) line++;
            }
        }
    }
    return result;
}

static int b_exec_stmt(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    /* REM - comment, ignore rest of line. */
    if (b_ci_eq(p, "REM")) return 0;

    /* END / STOP - halt execution. */
    if (b_ci_eq(p, "END") || b_ci_eq(p, "STOP")) { basic_running = 0; return -1; }

    /* PRINT ... */
    if (b_ci_eq(p, "PRINT")) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;

        /* PRINT with no args: just newline. */
        if (!*p || *p == ':') { con_putc('\n'); return 0; }

        /* PRINT "string literal" */
        if (*p == '"') {
            char buf[BASIC_LINE_LEN];
            const char *end = parse_string_literal(p, buf, sizeof(buf));
            if (end) {
                con_puts(buf);
                p = end;
                while (*p == ' ' || *p == '\t') p++;
                /* Check for ; or , (no newline / tab stop). */
                if (*p == ';') { p++; }
                else if (*p == ',') { con_putc('\t'); p++; }
                else { con_putc('\n'); }
                /* Print rest as expressions if any. */
                while (*p && *p != ':') {
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == ';') { p++; continue; }
                    if (*p == ',') { con_putc('\t'); p++; continue; }
                    break;
                }
                return 0;
            }
        }

        /* PRINT expr[;|,expr...] */
        ep = p;
        ep_error = 0;
        int val = eval_cmp();
        p = ep;
        if (!ep_error) { con_puti(val); }

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';') { p++; }
        else if (*p == ',') { con_putc('\t'); p++; }
        else { con_putc('\n'); }
        return 0;
    }

    /* LET var = expr (LET is optional) */
    if (b_ci_eq(p, "LET")) { p += 3; while (*p == ' ' || *p == '\t') p++; }

    /* Check if it's an assignment: single letter followed by '=' */
    if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) &&
        p[1] == '=') {
        int *ref = b_var_ref(*p);
        if (!ref) { con_puts("?BAD VARIABLE\n"); return -1; }
        p += 2;
        *ref = b_eval(p);
        return 0;
    }

    /* INPUT var */
    if (b_ci_eq(p, "INPUT")) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        int *ref = b_var_ref(*p);
        if (!ref) { con_puts("?BAD VARIABLE\n"); return -1; }
        p++;
        /* Read a line from the user. */
        char buf[BASIC_LINE_LEN];
        int r = read_line("? ", buf, sizeof(buf));
        if (r >= 0) {
            *ref = b_eval(buf);
        }
        return 0;
    }

    /* IF expr THEN linenumber */
    if (b_ci_eq(p, "IF")) {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        int cond = b_eval(p);
        /* Advance past the expression we just parsed. */
        /* Actually b_eval used a global ep, so... */
        /* We need to find THEN ourselves. Let's parse differently. */
        /* Find "THEN" in the rest of the line. */
        const char *rest = p;
        while (*rest && !b_ci_eq(rest, "THEN")) rest++;
        if (!*rest) { con_puts("?MISSING THEN\n"); return -1; }
        rest += 4; /* skip THEN */
        while (*rest == ' ' || *rest == '\t') rest++;

        if (cond) {
            /* THEN <linenumber> -> goto */
            if (*rest >= '0' && *rest <= '9') {
                int target = 0;
                while (*rest >= '0' && *rest <= '9') { target = target * 10 + (*rest - '0'); rest++; }
                int idx = b_find_line(target);
                if (idx < 0) { con_puts("?UNDEF'D STATEMENT\n"); return -1; }
                basic_pc = idx;
                return 1; /* goto performed */
            }
            /* THEN <statement> -> execute inline */
            return b_exec_stmt(rest);
        }
        return 0;
    }

    /* GOTO linenumber */
    if (b_ci_eq(p, "GOTO")) {
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        int target = 0;
        while (*p >= '0' && *p <= '9') { target = target * 10 + (*p - '0'); p++; }
        int idx = b_find_line(target);
        if (idx < 0) { con_puts("?UNDEF'D STATEMENT\n"); return -1; }
        basic_pc = idx;
        return 1;
    }

    /* GOSUB linenumber */
    if (b_ci_eq(p, "GOSUB")) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        int target = 0;
        while (*p >= '0' && *p <= '9') { target = target * 10 + (*p - '0'); p++; }
        int idx = b_find_line(target);
        if (idx < 0) { con_puts("?UNDEF'D STATEMENT\n"); return -1; }
        if (basic_gosub_sp >= BASIC_GOSUB_MAX) { con_puts("?GOSUB OVERFLOW\n"); return -1; }
        basic_gosub_stack[basic_gosub_sp++] = basic_pc + 1; /* return to next line */
        basic_pc = idx;
        return 1;
    }

    /* RETURN (from GOSUB) */
    if (b_ci_eq(p, "RETURN")) {
        if (basic_gosub_sp <= 0) { con_puts("?RETURN WITHOUT GOSUB\n"); return -1; }
        basic_pc = basic_gosub_stack[--basic_gosub_sp];
        return 1;
    }

    /* FOR var = expr TO expr [STEP expr] */
    if (b_ci_eq(p, "FOR")) {
        p += 3;
        while (*p == ' ' || *p == '\t') p++;
        char var = b_toupper(*p);
        if (var < 'A' || var > 'Z') { con_puts("?SYNTAX ERROR\n"); return -1; }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') { con_puts("?SYNTAX ERROR\n"); return -1; }
        p++;
        while (*p == ' ' || *p == '\t') p++;

        /* Find TO keyword. */
        const char *to_p = p;
        while (*to_p && !b_ci_eq(to_p, "TO")) to_p++;
        if (!*to_p) { con_puts("?MISSING TO\n"); return -1; }

        /* Evaluate start value. */
        char start_str[BASIC_LINE_LEN];
        int len = (int)(to_p - p);
        if (len >= BASIC_LINE_LEN) len = BASIC_LINE_LEN - 1;
        b_strcpy(start_str, p, len + 1);
        start_str[len] = 0;

        int start_val = b_eval(start_str);
        to_p += 2; /* skip TO */

        /* Find STEP or end. */
        const char *step_p = to_p;
        while (*step_p && !b_ci_eq(step_p, "STEP")) step_p++;
        int step_val = 1;
        char to_str[BASIC_LINE_LEN];
        if (b_ci_eq(step_p, "STEP")) {
            len = (int)(step_p - to_p);
            if (len >= BASIC_LINE_LEN) len = BASIC_LINE_LEN - 1;
            b_strcpy(to_str, to_p, len + 1);
            to_str[len] = 0;
            step_p += 4;
            while (*step_p == ' ' || *step_p == '\t') step_p++;
            step_val = b_eval(step_p);
            if (step_val == 0) step_val = 1;
        } else {
            len = b_strlen(to_p);
            if (len >= BASIC_LINE_LEN) len = BASIC_LINE_LEN - 1;
            b_strcpy(to_str, to_p, len + 1);
            to_str[len] = 0;
        }

        int limit = b_eval(to_str);
        int *ref = b_var_ref(var);
        if (ref) *ref = start_val;

        if (basic_for_sp >= BASIC_FOR_MAX) { con_puts("?FOR STACK OVERFLOW\n"); return -1; }
        basic_for_stack[basic_for_sp].var = var;
        basic_for_stack[basic_for_sp].limit = limit;
        basic_for_stack[basic_for_sp].step = step_val;
        basic_for_stack[basic_for_sp].line_idx = basic_pc;
        basic_for_sp++;
        return 0;
    }

    /* NEXT var */
    if (b_ci_eq(p, "NEXT")) {
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        char var = b_toupper(*p);
        if (var < 'A' || var > 'Z') { con_puts("?SYNTAX ERROR\n"); return -1; }

        /* Find matching FOR frame. */
        int fi = -1;
        for (int i = basic_for_sp - 1; i >= 0; i--) {
            if (basic_for_stack[i].var == var) { fi = i; break; }
        }
        if (fi < 0) { con_puts("?NEXT WITHOUT FOR\n"); return -1; }

        struct basic_for_frame *f = &basic_for_stack[fi];
        int *ref = b_var_ref(f->var);
        if (ref) *ref += f->step;

        int done = 0;
        if (f->step > 0) done = (*ref > f->limit);
        else done = (*ref < f->limit);

        if (done) {
            /* Remove this frame and any above it. */
            for (int i = fi; i < basic_for_sp - 1; i++)
                basic_for_stack[i] = basic_for_stack[i + 1];
            basic_for_sp--;
            return 0;
        }
        /* Loop back. */
        basic_pc = f->line_idx + 1;  /* will be incremented by main loop */
        return 0;
    }

    /* LIST - list program. */
    if (b_ci_eq(p, "LIST")) {
        for (int i = 0; i < basic_line_count; i++) {
            con_puti(basic_line_nums[i]);
            con_putc(' ');
            con_puts(basic_program[i]);
            con_putc('\n');
        }
        return 0;
    }

    /* RUN - execute program. */
    if (b_ci_eq(p, "RUN")) {
        /* This is handled at a higher level. */
        return 0;
    }

    /* NEW - clear program. */
    if (b_ci_eq(p, "NEW")) {
        basic_line_count = 0;
        for (int i = 0; i < 26; i++) basic_vars[i] = 0;
        basic_gosub_sp = 0;
        basic_for_sp = 0;
        con_puts("OK\n");
        return 0;
    }

    /* Unknown statement. */
    con_puts("?SYNTAX ERROR\n");
    return -1;
}

/* ---------------------------------------------------------------------------
 * RUN: execute the stored program from the beginning.
 * ---------------------------------------------------------------------------*/
static void b_run(void)
{
    /* Reset state. */
    for (int i = 0; i < 26; i++) basic_vars[i] = 0;
    basic_gosub_sp = 0;
    basic_for_sp = 0;
    basic_pc = 0;
    basic_running = 1;

    while (basic_running && basic_pc >= 0 && basic_pc < basic_line_count) {
        int prev_pc = basic_pc;
        int result = b_exec_line(basic_pc);
        if (result == 1) {
            /* GOTO/GOSUB/RETURN: basic_pc was set. */
            if (basic_pc == prev_pc) { basic_pc++; }  /* avoid infinite loop on same line */
        } else if (result == 0) {
            basic_pc++;
        } else {
            /* Error or END. */
            break;
        }
        /* Safety: if we haven't moved and no goto, advance. */
        if (basic_pc == prev_pc && result == 0) basic_pc++;
    }
    basic_running = 0;
}

/* ---------------------------------------------------------------------------
 * BASIC REPL (read-eval-print loop).
 * ---------------------------------------------------------------------------*/
int basic_repl(void)
{
    /* Reset variables on entry. */
    for (int i = 0; i < 26; i++) basic_vars[i] = 0;
    basic_gosub_sp = 0;
    basic_for_sp = 0;

    con_puts("ForeB BASIC - type HELP for commands\n");

    char line[BASIC_LINE_LEN];
    for (;;) {
        int r = read_line("READY> ", line, sizeof(line));
        if (r < 0) break;   /* Esc pressed */

        /* Skip leading whitespace. */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;  /* empty line */

        /* Check if line starts with a number -> store program line. */
        if (*p >= '0' && *p <= '9') {
            int num = 0;
            while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) {
                /* Just a number -> delete line. */
                int idx = b_find_line(num);
                if (idx >= 0) {
                    for (int i = idx; i < basic_line_count - 1; i++) {
                        basic_line_nums[i] = basic_line_nums[i + 1];
                        b_strcpy(basic_program[i], basic_program[i + 1], BASIC_LINE_LEN);
                    }
                    basic_line_count--;
                }
            } else {
                b_store_line(num, p);
            }
            continue;
        }

        /* Direct command. */
        if (b_ci_eq(p, "RUN")) { b_run(); continue; }
        if (b_ci_eq(p, "NEW")) {
            basic_line_count = 0;
            for (int i = 0; i < 26; i++) basic_vars[i] = 0;
            basic_gosub_sp = 0;
            basic_for_sp = 0;
            con_puts("OK\n");
            continue;
        }
        if (b_ci_eq(p, "LIST")) {
            for (int i = 0; i < basic_line_count; i++) {
                con_puti(basic_line_nums[i]);
                con_putc(' ');
                con_puts(basic_program[i]);
                con_putc('\n');
            }
            continue;
        }
        if (b_ci_eq(p, "HELP")) {
            con_puts("BASIC commands:\n");
            con_puts("  RUN      - run program\n");
            con_puts("  LIST     - list program\n");
            con_puts("  NEW      - clear program\n");
            con_puts("  HELP     - this help\n");
            con_puts("  (Esc)    - exit BASIC\n");
            con_puts("\nStatements (in program lines):\n");
            con_puts("  PRINT expr[;|,]   LET var=expr   INPUT var\n");
            con_puts("  IF expr THEN line  GOTO line      GOSUB line\n");
            con_puts("  RETURN            FOR var=TO TO [STEP] / NEXT var\n");
            con_puts("  REM comment       END\n");
            con_puts("Expressions: + - * / MOD, ABS(x), INT(x), RND, comparisons\n");
            continue;
        }

        /* Try to execute as a direct statement. */
        basic_pc = 0;
        ep = p;
        ep_error = 0;
        b_exec_stmt(p);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * basic_run: execute a BASIC program from a text buffer.
 * Each line in the buffer should be "NUM statement" format.
 * ---------------------------------------------------------------------------*/
int basic_run(const char *program)
{
    if (!program) return -1;

    /* Reset state. */
    basic_line_count = 0;
    for (int i = 0; i < 26; i++) basic_vars[i] = 0;
    basic_gosub_sp = 0;
    basic_for_sp = 0;

    /* Parse the program text into numbered lines. */
    const char *p = program;
    while (*p) {
        /* Skip blank lines. */
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        /* Read a line up to newline. */
        char tmp[BASIC_LINE_LEN];
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < BASIC_LINE_LEN - 1) {
            tmp[i++] = *p++;
        }
        tmp[i] = 0;
        while (*p == '\n' || *p == '\r') p++;

        /* Parse line number. */
        const char *lp = tmp;
        while (*lp == ' ' || *lp == '\t') lp++;
        if (*lp < '0' || *lp > '9') continue;
        int num = 0;
        while (*lp >= '0' && *lp <= '9') { num = num * 10 + (*lp - '0'); lp++; }
        while (*lp == ' ' || *lp == '\t') lp++;
        if (!*lp) continue;

        b_store_line(num, lp);
    }

    /* Run the program. */
    b_run();
    return 0;
}

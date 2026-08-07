#ifndef FORCE_H
#define FORCE_H

/*
 * FORCE -- Forest C Encapsulated
 *
 * A small C/HolyC-like dynamically-typed language with a tree-walking
 * interpreter that runs INSIDE the kernel (ring0). It is a sandboxed guest:
 * it never panics, never hangs, and never leaks kernel memory on any input,
 * hostile or otherwise. Every failure path returns a clean error code plus a
 * human-readable message. All work is bounded by compile-time caps (steps,
 * recursion depth, heap bytes, source/token/string/array lengths).
 *
 * Public surface only. All internal types (values, AST, environments) live in
 * src/force.c and are deliberately NOT exposed here.
 */

#include "types.h"

/* Opaque persistent REPL context. Owns a global scope + all heap accounting.
 * One in-flight eval per context (not reentrant, not thread-safe). */
typedef struct force_ctx force_ctx;

/* Error codes (negative). FORCE_OK == 0. */
enum {
    FORCE_OK        =  0,
    FORCE_E_PARSE   = -1,   /* lex/parse error */
    FORCE_E_RUNTIME = -2,   /* runtime error (bad op, undefined var, ...) */
    FORCE_E_LIMIT   = -3,   /* a guardrail cap was hit */
    FORCE_E_NOMEM   = -4,   /* kmalloc failed / heap cap */
    FORCE_E_IO      = -5,   /* VFS read failed */
    FORCE_E_ARG     = -6    /* bad API argument */
};

/* One-time global init. Idempotent. Returns 1 on success, 0 on failure.
 * (Returns a truthy int so the boot sequence's `vfs_ok && force_init()`
 * expression reads correctly.) */
int force_init(void);

/* Persistent REPL context lifecycle. */
force_ctx *force_ctx_create(void);
void       force_ctx_destroy(force_ctx *ctx);   /* frees ALL env + heap */
void       force_ctx_reset(force_ctx *ctx);      /* wipe user state, keep builtins */

/* Redirect print output. sink==NULL restores the default (internal buffer
 * returned via out_msg). */
void force_ctx_set_output(force_ctx *ctx, void (*sink)(const char *s, uint32 n));

/*
 * Evaluate `src` in ctx's global scope. Writes a NUL-terminated,
 * length-bounded result-or-error line into out_msg. Returns FORCE_OK on
 * success or a negative FORCE_E_* code on failure; out_msg is populated in
 * BOTH cases (on error it carries the human-readable error text).
 */
int force_eval_ctx(force_ctx *ctx, const char *src, char *out_msg, uint32 out_sz);

/*
 * Convenience one-shot eval in a throwaway context. Returns the number of
 * bytes written to out_msg (>= 0), or a negative FORCE_E_* code only for bad
 * API arguments. The message (program output, or error text) is always in
 * out_msg so callers can print it verbatim.
 */
int force_eval_string(const char *src, char *out_msg, uint32 out_sz);

#endif /* FORCE_H */

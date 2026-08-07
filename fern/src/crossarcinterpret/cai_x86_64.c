/*
 * cai_x86_64.c - x86-64 (AMD64) instruction emulator
 *
 * Implements a working subset of the x86-64 ISA.  The key additions over the
 * x86-32 emulator are:
 *   - 64-bit register file (rax-r15, rsp, rbp, rip, rflags)
 *   - REX prefix decoding (REX.W forces 64-bit operand, REX.R/X/B extend regs)
 *   - SYSCALL instruction → Fern syscall bridge
 *   - 64-bit addressing (mod=0, rm=5 → RIP-relative; SIB with full 64-bit base)
 *
 * Suffix conventions
 * ------------------
 *   Without REX.W  → 32-bit operands (result zero-extends to 64-bit in dst reg)
 *   With REX.W     → 64-bit operands
 *   The 0x66 prefix is tracked but not fully emulated (16-bit moves not critical
 *   for syscall-only programs).
 *
 * Supported instructions
 * ----------------------
 *  MOV, PUSH, POP, LEA, XCHG, MOVSX, MOVZX
 *  ADD, SUB, AND, OR, XOR, CMP, TEST, INC, DEC, NEG, NOT, IMUL, DIV, IDIV
 *  SHL, SHR, SAR, ROL, ROR
 *  JMP (rel8/rel32/r/m64), Jcc rel8/rel32, CALL (rel32/r/m64), RET
 *  SYSCALL (0F 05)
 *  LEAVE, CDQ/CQO, NOP, REP MOVS/STOS/SCAS
 */

#include "crossarcinterpret.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

/* =========================================================================
 * RFLAGS bits
 * ========================================================================= */

#define FL_CF  (1ULL <<  0)
#define FL_PF  (1ULL <<  2)
#define FL_AF  (1ULL <<  4)
#define FL_ZF  (1ULL <<  6)
#define FL_SF  (1ULL <<  7)
#define FL_DF  (1ULL << 10)
#define FL_OF  (1ULL << 11)

/* =========================================================================
 * Register accessors
 * ========================================================================= */

#define R(ctx)      ((ctx)->cpu.x86_64)
#define RIP(ctx)    (R(ctx).rip)
#define RSP(ctx)    (R(ctx).rsp)
#define RFLAGS(ctx) (R(ctx).rflags)

/* Index 0-15 maps to rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8-r15 */
static uint64_t *reg64(cai_context_t *ctx, int idx)
{
    switch (idx & 15) {
    case  0: return &R(ctx).rax;
    case  1: return &R(ctx).rcx;
    case  2: return &R(ctx).rdx;
    case  3: return &R(ctx).rbx;
    case  4: return &R(ctx).rsp;
    case  5: return &R(ctx).rbp;
    case  6: return &R(ctx).rsi;
    case  7: return &R(ctx).rdi;
    case  8: return &R(ctx).r8;
    case  9: return &R(ctx).r9;
    case 10: return &R(ctx).r10;
    case 11: return &R(ctx).r11;
    case 12: return &R(ctx).r12;
    case 13: return &R(ctx).r13;
    case 14: return &R(ctx).r14;
    case 15: return &R(ctx).r15;
    }
    return &R(ctx).rax; /* unreachable */
}

static uint8_t read_reg8_64(cai_context_t *ctx, int idx, bool rex_present)
{
    /* Without REX: 0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH */
    /* With REX:    0=AL,1=CL,...7=DIL (no high regs),8-15=r8b-r15b */
    if (rex_present || idx >= 8) {
        return (uint8_t)(*reg64(ctx, idx) & 0xFF);
    }
    if (idx < 4)
        return (uint8_t)(*reg64(ctx, idx) & 0xFF);
    else
        return (uint8_t)((*reg64(ctx, idx - 4) >> 8) & 0xFF);
}

static void write_reg8_64(cai_context_t *ctx, int idx, uint8_t val,
                           bool rex_present)
{
    if (rex_present || idx >= 8) {
        uint64_t *r = reg64(ctx, idx);
        *r = (*r & ~0xFFULL) | val;
        return;
    }
    if (idx < 4) {
        uint64_t *r = reg64(ctx, idx);
        *r = (*r & ~0xFFULL) | val;
    } else {
        uint64_t *r = reg64(ctx, idx - 4);
        *r = (*r & ~0xFF00ULL) | ((uint64_t)val << 8);
    }
}

/* =========================================================================
 * Flag updates
 * ========================================================================= */

static void update_flags_logical64(cai_context_t *ctx, uint64_t result,
                                    int width_bits)
{
    uint64_t mask = (width_bits == 8)  ? 0xFFULL :
                    (width_bits == 16) ? 0xFFFFULL :
                    (width_bits == 32) ? 0xFFFFFFFFULL :
                                         0xFFFFFFFFFFFFFFFFULL;
    uint64_t r = result & mask;

    RFLAGS(ctx) &= ~(FL_CF | FL_OF | FL_SF | FL_ZF | FL_PF);
    if (r == 0)                RFLAGS(ctx) |= FL_ZF;
    if (r >> (width_bits - 1)) RFLAGS(ctx) |= FL_SF;
    uint8_t p = (uint8_t)r;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) RFLAGS(ctx) |= FL_PF;
}

static void update_flags_add64(cai_context_t *ctx, uint64_t a, uint64_t b,
                                 uint64_t result, int w)
{
    uint64_t mask = (w == 8) ? 0xFFULL : (w == 16) ? 0xFFFFULL :
                    (w == 32) ? 0xFFFFFFFFULL : 0xFFFFFFFFFFFFFFFFULL;
    uint64_t sign_bit = 1ULL << (w - 1);
    uint64_t r = result & mask;

    RFLAGS(ctx) &= ~(FL_CF | FL_OF | FL_SF | FL_ZF | FL_PF | FL_AF);

    /* CF – unsigned overflow (tricky for 64-bit: use intermediate) */
    if (w == 64) {
        if (result < a) RFLAGS(ctx) |= FL_CF; /* unsigned wraparound */
    } else {
        if (result > mask) RFLAGS(ctx) |= FL_CF;
    }

    if (!((a ^ b) & sign_bit) && ((a ^ r) & sign_bit)) RFLAGS(ctx) |= FL_OF;
    if (r & sign_bit) RFLAGS(ctx) |= FL_SF;
    if (r == 0) RFLAGS(ctx) |= FL_ZF;
    if (((a & 0xF) + (b & 0xF)) > 0xF) RFLAGS(ctx) |= FL_AF;
    uint8_t p = (uint8_t)r;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) RFLAGS(ctx) |= FL_PF;
}

static void update_flags_sub64(cai_context_t *ctx, uint64_t a, uint64_t b,
                                 uint64_t result, int w)
{
    uint64_t mask = (w == 8) ? 0xFFULL : (w == 16) ? 0xFFFFULL :
                    (w == 32) ? 0xFFFFFFFFULL : 0xFFFFFFFFFFFFFFFFULL;
    uint64_t sign_bit = 1ULL << (w - 1);
    uint64_t r = result & mask;

    RFLAGS(ctx) &= ~(FL_CF | FL_OF | FL_SF | FL_ZF | FL_PF | FL_AF);

    if ((a & mask) < (b & mask)) RFLAGS(ctx) |= FL_CF;
    if (((a ^ b) & sign_bit) && ((a ^ r) & sign_bit)) RFLAGS(ctx) |= FL_OF;
    if (r & sign_bit) RFLAGS(ctx) |= FL_SF;
    if (r == 0) RFLAGS(ctx) |= FL_ZF;
    if ((a & 0xF) < (b & 0xF)) RFLAGS(ctx) |= FL_AF;
    uint8_t p = (uint8_t)r;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) RFLAGS(ctx) |= FL_PF;
}

static bool eval_cond64(cai_context_t *ctx, int cc)
{
    bool cf = !!(RFLAGS(ctx) & FL_CF);
    bool pf = !!(RFLAGS(ctx) & FL_PF);
    bool zf = !!(RFLAGS(ctx) & FL_ZF);
    bool sf = !!(RFLAGS(ctx) & FL_SF);
    bool of = !!(RFLAGS(ctx) & FL_OF);
    switch (cc & 0xF) {
    case 0x0: return  of;
    case 0x1: return !of;
    case 0x2: return  cf;
    case 0x3: return !cf;
    case 0x4: return  zf;
    case 0x5: return !zf;
    case 0x6: return  cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return  sf;
    case 0x9: return !sf;
    case 0xA: return  pf;
    case 0xB: return !pf;
    case 0xC: return  sf != of;
    case 0xD: return  sf == of;
    case 0xE: return  zf || (sf != of);
    case 0xF: return !zf && (sf == of);
    }
    return false;
}

/* =========================================================================
 * Fetch helpers
 * ========================================================================= */

static int fetch8_64(cai_context_t *ctx, uint8_t *out)
{
    int rc = cai_mem_read8(ctx, RIP(ctx), out);
    if (rc == CAI_OK) RIP(ctx)++;
    return rc;
}

static int fetch32_64(cai_context_t *ctx, uint32_t *out)
{
    int rc = cai_mem_read32(ctx, RIP(ctx), out);
    if (rc == CAI_OK) RIP(ctx) += 4;
    return rc;
}

static int fetch64_64(cai_context_t *ctx, uint64_t *out)
{
    int rc = cai_mem_read64(ctx, RIP(ctx), out);
    if (rc == CAI_OK) RIP(ctx) += 8;
    return rc;
}

/* =========================================================================
 * REX prefix state
 * ========================================================================= */

typedef struct {
    bool W;  /* 64-bit operand size */
    bool R;  /* reg field extension */
    bool X;  /* SIB index extension */
    bool B;  /* r/m or base extension */
    bool present;
} rex_t;

/* Apply REX extensions to a 3-bit register index */
static inline int apply_rex_r(rex_t r, int idx) { return r.R ? (idx | 8) : idx; }
static inline int apply_rex_b(rex_t r, int idx) { return r.B ? (idx | 8) : idx; }
static inline int apply_rex_x(rex_t r, int idx) { return r.X ? (idx | 8) : idx; }

/* =========================================================================
 * ModRM decoder (64-bit)
 * ========================================================================= */

typedef struct {
    int      mod;
    int      reg;   /* Already extended with REX.R */
    int      rm;    /* Already extended with REX.B */
    uint64_t ea;    /* Effective address (mod != 3) */
    bool     is_reg;
} modrm64_t;

static int decode_modrm64(cai_context_t *ctx, rex_t rex, modrm64_t *m)
{
    uint8_t byte;
    int rc = fetch8_64(ctx, &byte);
    if (rc) return rc;

    int mod  = (byte >> 6) & 3;
    int reg  = apply_rex_r(rex, (byte >> 3) & 7);
    int rm   = apply_rex_b(rex, byte & 7);

    m->mod    = mod;
    m->reg    = reg;
    m->rm     = rm;
    m->is_reg = (mod == 3);

    if (mod == 3) { m->ea = 0; return CAI_OK; }

    uint64_t base = 0, index = 0, disp = 0;
    bool has_base = true;

    int raw_rm = byte & 7; /* Un-extended RM for SIB/disp32 detection */

    if (raw_rm == 4) {
        /* SIB byte follows */
        uint8_t sib;
        rc = fetch8_64(ctx, &sib); if (rc) return rc;
        int ss  = (sib >> 6) & 3;
        int idx = apply_rex_x(rex, (sib >> 3) & 7);
        int bas = apply_rex_b(rex, sib & 7);

        uint32_t scale = 1u << ss;
        if ((sib >> 3 & 7) != 4) /* index != RSP */
            index = *reg64(ctx, idx) * scale;

        if ((sib & 7) == 5 && mod == 0) {
            has_base = false;
        } else {
            base = *reg64(ctx, bas);
        }
    } else if (raw_rm == 5 && mod == 0) {
        /* RIP-relative addressing */
        uint32_t d32; rc = fetch32_64(ctx, &d32); if (rc) return rc;
        m->ea = RIP(ctx) + (int64_t)(int32_t)d32;
        return CAI_OK;
    } else {
        base = *reg64(ctx, rm);
    }

    if (mod == 1) {
        uint8_t d8; rc = fetch8_64(ctx, &d8); if (rc) return rc;
        disp = (uint64_t)(int64_t)(int8_t)d8;
    } else if (mod == 2) {
        uint32_t d32; rc = fetch32_64(ctx, &d32); if (rc) return rc;
        disp = (uint64_t)(int64_t)(int32_t)d32;
    } else if (!has_base) {
        /* SIB with disp32 (mod=0, base=rbp) */
        uint32_t d32; rc = fetch32_64(ctx, &d32); if (rc) return rc;
        disp = (uint64_t)(int64_t)(int32_t)d32;
    }

    m->ea = (has_base ? base : 0) + index + disp;
    return CAI_OK;
}

/* Read/write via ModRM – operand width determined by rex.W */
static int modrm_read_op(cai_context_t *ctx, const modrm64_t *m, rex_t rex,
                          uint64_t *out)
{
    if (m->is_reg) {
        *out = rex.W ? *reg64(ctx, m->rm) :
               (*reg64(ctx, m->rm) & 0xFFFFFFFFULL);
        return CAI_OK;
    }
    if (rex.W) return cai_mem_read64(ctx, m->ea, out);
    uint32_t v32; int rc = cai_mem_read32(ctx, m->ea, &v32);
    *out = v32; return rc;
}

static int modrm_write_op(cai_context_t *ctx, const modrm64_t *m, rex_t rex,
                           uint64_t val)
{
    if (m->is_reg) {
        if (rex.W)
            *reg64(ctx, m->rm) = val;
        else
            *reg64(ctx, m->rm) = val & 0xFFFFFFFFULL; /* 32-bit zero-extends */
        return CAI_OK;
    }
    if (rex.W) return cai_mem_write64(ctx, m->ea, val);
    return cai_mem_write32(ctx, m->ea, (uint32_t)val);
}

static int modrm_read8_64(cai_context_t *ctx, const modrm64_t *m, rex_t rex,
                           uint8_t *out)
{
    if (m->is_reg) { *out = read_reg8_64(ctx, m->rm, rex.present); return CAI_OK; }
    return cai_mem_read8(ctx, m->ea, out);
}

static int modrm_write8_64(cai_context_t *ctx, const modrm64_t *m, rex_t rex,
                            uint8_t val)
{
    if (m->is_reg) { write_reg8_64(ctx, m->rm, val, rex.present); return CAI_OK; }
    return cai_mem_write8(ctx, m->ea, val);
}

/* =========================================================================
 * ALU helpers
 * ========================================================================= */

static int alu64(cai_context_t *ctx, int op, uint64_t dst, uint64_t src,
                  int w, uint64_t *result_out)
{
    uint64_t r;
    switch (op) {
    case 0: r = dst + src; update_flags_add64(ctx, dst, src, r, w); break; /* ADD */
    case 1: r = dst | src; update_flags_logical64(ctx, r, w); break;        /* OR  */
    case 2: { uint64_t c = !!(RFLAGS(ctx)&FL_CF);                          /* ADC */
              r = dst + src + c;
              update_flags_add64(ctx, dst, src + c, r, w); break; }
    case 3: { uint64_t c = !!(RFLAGS(ctx)&FL_CF);                          /* SBB */
              r = dst - src - c;
              update_flags_sub64(ctx, dst, src + c, r, w); break; }
    case 4: r = dst & src; update_flags_logical64(ctx, r, w); break;        /* AND */
    case 5: r = dst - src; update_flags_sub64(ctx, dst, src, r, w); break;  /* SUB */
    case 6: r = dst ^ src; update_flags_logical64(ctx, r, w); break;        /* XOR */
    case 7: r = dst - src; update_flags_sub64(ctx, dst, src, r, w);        /* CMP */
            *result_out = dst; return CAI_OK; /* CMP: no write */
    default: return CAI_EILL;
    }
    *result_out = r;
    return CAI_OK;
}

/* =========================================================================
 * Push/pop (64-bit mode always uses 64-bit stack operands)
 * ========================================================================= */

static int push64(cai_context_t *ctx, uint64_t val)
{
    RSP(ctx) -= 8;
    return cai_mem_write64(ctx, RSP(ctx), val);
}

static int pop64(cai_context_t *ctx, uint64_t *out)
{
    int rc = cai_mem_read64(ctx, RSP(ctx), out);
    if (rc == CAI_OK) RSP(ctx) += 8;
    return rc;
}

/* =========================================================================
 * Single-step execution (x86-64)
 * ========================================================================= */

int cai_x86_64_step(cai_context_t *ctx)
{
    uint8_t opcode;
    uint64_t saved_rip = RIP(ctx);
    int rc;
    rex_t rex = {0};
    bool rep_prefix = false;

    /* Collect prefix bytes */
next_prefix:
    rc = fetch8_64(ctx, &opcode);
    if (rc) return rc;

    if (opcode >= 0x40 && opcode <= 0x4F) {
        /* REX prefix */
        rex.present = true;
        rex.W = !!(opcode & 0x08);
        rex.R = !!(opcode & 0x04);
        rex.X = !!(opcode & 0x02);
        rex.B = !!(opcode & 0x01);
        goto next_prefix;
    }
    switch (opcode) {
    case 0xF3: case 0xF2: rep_prefix = true; goto next_prefix;
    case 0x66: goto next_prefix;
    case 0x67: goto next_prefix;
    case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0x64: case 0x65: goto next_prefix;
    case 0xF0: goto next_prefix; /* LOCK */
    default: break;
    }

    /* Width for this instruction */
    int w = rex.W ? 64 : 32;

    switch (opcode) {

    /* ------------------------------------------------------------------ */
    case 0x90: /* NOP (or XCHG RAX,RAX if no REX) */
        break;

    /* ------------------------------------------------------------------ */
    /* MOV r/m, r  (89)                                                    */
    case 0x89: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t val = rex.W ? *reg64(ctx, m.reg) : (*reg64(ctx, m.reg) & 0xFFFFFFFF);
        rc = modrm_write_op(ctx, &m, rex, val);
        break;
    }
    /* MOV r, r/m  (8B)                                                    */
    case 0x8B: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t val; rc = modrm_read_op(ctx, &m, rex, &val); if (rc) return rc;
        if (rex.W) *reg64(ctx, m.reg) = val;
        else *reg64(ctx, m.reg) = val & 0xFFFFFFFF;
        break;
    }
    /* MOV r/m8, r8  (88)                                                  */
    case 0x88: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint8_t val = read_reg8_64(ctx, m.reg, rex.present);
        rc = modrm_write8_64(ctx, &m, rex, val);
        break;
    }
    /* MOV r8, r/m8  (8A)                                                  */
    case 0x8A: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint8_t val; rc = modrm_read8_64(ctx, &m, rex, &val); if (rc) return rc;
        write_reg8_64(ctx, m.reg, val, rex.present);
        break;
    }
    /* MOV r, imm  (B8+r)  – note: B8-BF without REX.W → 32-bit; with → 64 */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int ridx = apply_rex_b(rex, opcode - 0xB8);
        if (rex.W) {
            uint64_t imm; rc = fetch64_64(ctx, &imm); if (rc) return rc;
            *reg64(ctx, ridx) = imm;
        } else {
            uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc;
            *reg64(ctx, ridx) = (uint64_t)imm;
        }
        break;
    }
    /* MOV r8, imm8  (B0+r)                                                */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        uint8_t imm; rc = fetch8_64(ctx, &imm); if (rc) return rc;
        int ridx = apply_rex_b(rex, opcode - 0xB0);
        write_reg8_64(ctx, ridx, imm, rex.present);
        break;
    }
    /* MOV r/m, imm32 (sign-extend to 64)  (C7)                           */
    case 0xC7: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc;
        uint64_t val = rex.W ? (uint64_t)(int64_t)(int32_t)imm : (uint64_t)imm;
        rc = modrm_write_op(ctx, &m, rex, val);
        break;
    }
    /* MOV r/m8, imm8  (C6)                                                */
    case 0xC6: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint8_t imm; rc = fetch8_64(ctx, &imm); if (rc) return rc;
        rc = modrm_write8_64(ctx, &m, rex, imm);
        break;
    }

    /* ------------------------------------------------------------------ */
    /* LEA  (8D)                                                            */
    case 0x8D: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        if (m.is_reg) return CAI_EILL;
        if (rex.W) *reg64(ctx, m.reg) = m.ea;
        else *reg64(ctx, m.reg) = (uint64_t)(uint32_t)m.ea;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* PUSH r64  (50+r)                                                     */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        rc = push64(ctx, *reg64(ctx, apply_rex_b(rex, opcode - 0x50)));
        break;

    /* POP r64  (58+r)                                                     */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint64_t val; rc = pop64(ctx, &val); if (rc) return rc;
        *reg64(ctx, apply_rex_b(rex, opcode - 0x58)) = val;
        break;
    }

    /* PUSH imm32 sign-extended  (68)                                      */
    case 0x68: {
        uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc;
        rc = push64(ctx, (uint64_t)(int64_t)(int32_t)imm);
        break;
    }
    /* PUSH imm8 sign-extended  (6A)                                       */
    case 0x6A: {
        uint8_t imm; rc = fetch8_64(ctx, &imm); if (rc) return rc;
        rc = push64(ctx, (uint64_t)(int64_t)(int8_t)imm);
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Group 1  (80/81/83)                                                 */
    case 0x81: { /* r/m, imm32 */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc;
        uint64_t src = rex.W ? (uint64_t)(int64_t)(int32_t)imm : (uint64_t)imm;
        uint64_t dst; rc = modrm_read_op(ctx, &m, rex, &dst); if (rc) return rc;
        uint64_t res; rc = alu64(ctx, m.reg, dst, src, w, &res); if (rc) return rc;
        if (m.reg != 7) rc = modrm_write_op(ctx, &m, rex, res);
        break;
    }
    case 0x83: { /* r/m, imm8 sign-extended */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint8_t imm8; rc = fetch8_64(ctx, &imm8); if (rc) return rc;
        uint64_t src = (uint64_t)(int64_t)(int8_t)imm8;
        uint64_t dst; rc = modrm_read_op(ctx, &m, rex, &dst); if (rc) return rc;
        uint64_t res; rc = alu64(ctx, m.reg, dst, src, w, &res); if (rc) return rc;
        if (m.reg != 7) rc = modrm_write_op(ctx, &m, rex, res);
        break;
    }
    case 0x80: { /* r/m8, imm8 */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint8_t imm8; rc = fetch8_64(ctx, &imm8); if (rc) return rc;
        uint8_t dst; rc = modrm_read8_64(ctx, &m, rex, &dst); if (rc) return rc;
        uint64_t res; rc = alu64(ctx, m.reg, dst, imm8, 8, &res); if (rc) return rc;
        if (m.reg != 7) rc = modrm_write8_64(ctx, &m, rex, (uint8_t)res);
        break;
    }

    /* ADD/SUB/AND/OR/XOR/CMP two-operand forms                           */
    case 0x01: { /* r/m, r */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg) : (*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res; alu64(ctx,0,dst,src,w,&res);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }
    case 0x03: { /* r, r/m */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
        uint64_t dst = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,0,dst,src,w,&res);
        if(rex.W)*reg64(ctx,m.reg)=res; else *reg64(ctx,m.reg)=res&0xFFFFFFFF;
        break;
    }
    case 0x29: { /* r/m, r  SUB */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res; alu64(ctx,5,dst,src,w,&res);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }
    case 0x2B: { /* r, r/m  SUB */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
        uint64_t dst = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,5,dst,src,w,&res);
        if(rex.W)*reg64(ctx,m.reg)=res; else *reg64(ctx,m.reg)=res&0xFFFFFFFF;
        break;
    }
    case 0x09: { /* r/m, r  OR */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res; alu64(ctx,1,dst,src,w,&res);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }
    case 0x0B: { /* r, r/m  OR */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
        uint64_t dst = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,1,dst,src,w,&res);
        if(rex.W)*reg64(ctx,m.reg)=res; else *reg64(ctx,m.reg)=res&0xFFFFFFFF;
        break;
    }
    case 0x21: { /* r/m, r  AND */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res; alu64(ctx,4,dst,src,w,&res);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }
    case 0x23: { /* r, r/m  AND */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
        uint64_t dst = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,4,dst,src,w,&res);
        if(rex.W)*reg64(ctx,m.reg)=res; else *reg64(ctx,m.reg)=res&0xFFFFFFFF;
        break;
    }
    case 0x31: { /* r/m, r  XOR */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res; alu64(ctx,6,dst,src,w,&res);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }
    case 0x33: { /* r, r/m  XOR */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
        uint64_t dst = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,6,dst,src,w,&res);
        if(rex.W)*reg64(ctx,m.reg)=res; else *reg64(ctx,m.reg)=res&0xFFFFFFFF;
        break;
    }
    case 0x39: { /* r/m, r  CMP */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res; alu64(ctx,7,dst,src,w,&res); break;
    }
    case 0x3B: { /* r, r/m  CMP */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
        uint64_t dst = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,7,dst,src,w,&res); break;
    }
    case 0x3D: { /* CMP rAX, imm32 */
        uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc;
        uint64_t src = rex.W ? (uint64_t)(int64_t)(int32_t)imm : (uint64_t)imm;
        uint64_t dst = rex.W ? *reg64(ctx,0) : (*reg64(ctx,0)&0xFFFFFFFF);
        uint64_t res; alu64(ctx,7,dst,src,w,&res); break;
    }
    case 0x85: { /* TEST r/m, r */
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t src = rex.W ? *reg64(ctx,m.reg):(*reg64(ctx,m.reg)&0xFFFFFFFF);
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        update_flags_logical64(ctx, dst & src, w); break;
    }
    case 0xA9: { /* TEST rAX, imm32 */
        uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc;
        uint64_t src = rex.W ? (uint64_t)(int64_t)(int32_t)imm : (uint64_t)imm;
        uint64_t dst = rex.W ? *reg64(ctx,0) : (*reg64(ctx,0)&0xFFFFFFFF);
        update_flags_logical64(ctx, dst & src, w); break;
    }

    /* INC/DEC r64  (48+r now REX prefix, so must be via FE/FF)           */
    /* INC/DEC r32  (40+r — NB: these ARE valid in 64-bit mode since REX  */
    /* prefixes 0x40-0x4F were processed above already)                    */

    /* Group 5  (FF)                                                        */
    case 0xFF: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        switch (m.reg) {
        case 0: { /* INC r/m */
            uint64_t val; rc = modrm_read_op(ctx,&m,rex,&val); if(rc) return rc;
            uint64_t cf = RFLAGS(ctx) & FL_CF;
            uint64_t res = val + 1;
            update_flags_add64(ctx, val, 1, res, w);
            RFLAGS(ctx) = (RFLAGS(ctx) & ~FL_CF) | cf;
            rc = modrm_write_op(ctx,&m,rex,res); break;
        }
        case 1: { /* DEC r/m */
            uint64_t val; rc = modrm_read_op(ctx,&m,rex,&val); if(rc) return rc;
            uint64_t cf = RFLAGS(ctx) & FL_CF;
            uint64_t res = val - 1;
            update_flags_sub64(ctx, val, 1, res, w);
            RFLAGS(ctx) = (RFLAGS(ctx) & ~FL_CF) | cf;
            rc = modrm_write_op(ctx,&m,rex,res); break;
        }
        case 2: { /* CALL r/m64 */
            uint64_t target; rc = modrm_read_op(ctx,&m,rex,&target); if(rc) return rc;
            rc = push64(ctx, RIP(ctx)); if(rc) return rc;
            RIP(ctx) = target; break;
        }
        case 4: { /* JMP r/m64 */
            uint64_t target; rc = modrm_read_op(ctx,&m,rex,&target); if(rc) return rc;
            RIP(ctx) = target; break;
        }
        case 6: { /* PUSH r/m64 */
            uint64_t val; rc = modrm_read_op(ctx,&m,rex,&val); if(rc) return rc;
            rc = push64(ctx, val); break;
        }
        default: return CAI_EILL;
        }
        break;
    }

    /* Group 3  (F7)                                                        */
    case 0xF7: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t val; rc = modrm_read_op(ctx,&m,rex,&val); if(rc) return rc;
        switch (m.reg) {
        case 0: { /* TEST imm32 */
            uint32_t imm; rc = fetch32_64(ctx,&imm); if(rc) return rc;
            uint64_t src = rex.W ? (uint64_t)(int64_t)(int32_t)imm : (uint64_t)imm;
            update_flags_logical64(ctx, val & src, w); break;
        }
        case 2: rc = modrm_write_op(ctx,&m,rex,~val); break; /* NOT */
        case 3: { /* NEG */
            uint64_t res = (uint64_t)(-(int64_t)val);
            update_flags_sub64(ctx,0,val,res,w);
            rc = modrm_write_op(ctx,&m,rex,res); break;
        }
        case 4: { /* MUL */
            if (rex.W) {
                /* __uint128_t not available – use 64-bit only */
                uint64_t prod = *reg64(ctx,0) * val;
                *reg64(ctx,0) = prod; *reg64(ctx,2) = 0; /* simplified */
            } else {
                uint64_t prod = (uint64_t)(*reg64(ctx,0) & 0xFFFFFFFF) * (uint64_t)(uint32_t)val;
                *reg64(ctx,0) = prod & 0xFFFFFFFF;
                *reg64(ctx,2) = prod >> 32;
            }
            break;
        }
        case 5: { /* IMUL */
            if (rex.W) {
                int64_t prod = (int64_t)*reg64(ctx,0) * (int64_t)val;
                *reg64(ctx,0) = (uint64_t)prod; *reg64(ctx,2) = 0;
            } else {
                int64_t prod = (int64_t)(int32_t)(*reg64(ctx,0)&0xFFFFFFFF) * (int64_t)(int32_t)val;
                *reg64(ctx,0) = (uint64_t)(uint32_t)prod;
                *reg64(ctx,2) = (uint64_t)(uint32_t)(prod >> 32);
            }
            break;
        }
        case 6: { /* DIV */
            if (!val) return CAI_EILL;
            if (rex.W) {
                *reg64(ctx,0) = *reg64(ctx,0) / val;
                *reg64(ctx,2) = *reg64(ctx,0) % val;
            } else {
                uint64_t num = ((*reg64(ctx,2)&0xFFFFFFFF)<<32)|(*reg64(ctx,0)&0xFFFFFFFF);
                *reg64(ctx,0) = (num / (uint32_t)val) & 0xFFFFFFFF;
                *reg64(ctx,2) = (num % (uint32_t)val) & 0xFFFFFFFF;
            }
            break;
        }
        default: return CAI_EILL;
        }
        break;
    }

    /* Shift Group 2  (C1, D3, D1)                                         */
    case 0xC1: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint8_t cnt; rc = fetch8_64(ctx, &cnt); if (rc) return rc;
        cnt &= (w == 64) ? 63 : 31;
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res = dst;
        switch (m.reg) {
        case 4: res = dst << cnt; break; /* SHL */
        case 5: res = (rex.W ? dst : (dst & 0xFFFFFFFF)) >> cnt; break; /* SHR */
        case 7: res = rex.W ? (uint64_t)((int64_t)dst >> cnt)
                             : (uint64_t)((int32_t)(dst & 0xFFFFFFFF) >> cnt); break; /* SAR */
        default: return CAI_EILL;
        }
        if (cnt) update_flags_logical64(ctx, res, w);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }
    case 0xD3: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint32_t cnt = (uint32_t)(*reg64(ctx,1) & ((w==64)?63:31)); /* CL */
        uint64_t dst; rc = modrm_read_op(ctx,&m,rex,&dst); if(rc) return rc;
        uint64_t res = dst;
        switch (m.reg) {
        case 4: res = dst << cnt; break;
        case 5: res = (rex.W ? dst : (dst & 0xFFFFFFFF)) >> cnt; break;
        case 7: res = rex.W ? (uint64_t)((int64_t)dst >> cnt)
                             : (uint64_t)((int32_t)(dst & 0xFFFFFFFF) >> cnt); break;
        default: return CAI_EILL;
        }
        if (cnt) update_flags_logical64(ctx, res, w);
        rc = modrm_write_op(ctx,&m,rex,res); break;
    }

    /* ------------------------------------------------------------------ */
    /* CDQ / CQO  (99)                                                      */
    case 0x99:
        if (rex.W)
            *reg64(ctx,2) = (*reg64(ctx,0) & (1ULL<<63)) ? ~0ULL : 0ULL; /* CQO */
        else
            *reg64(ctx,2) = (*reg64(ctx,0) & 0x80000000ULL) ? 0xFFFFFFFFULL : 0ULL; /* CDQ */
        break;

    /* ------------------------------------------------------------------ */
    /* CALL rel32  (E8)                                                     */
    case 0xE8: {
        uint32_t rel; rc = fetch32_64(ctx, &rel); if (rc) return rc;
        rc = push64(ctx, RIP(ctx)); if (rc) return rc;
        RIP(ctx) = RIP(ctx) + (int64_t)(int32_t)rel;
        break;
    }

    /* RET near  (C3)                                                       */
    case 0xC3: {
        uint64_t addr; rc = pop64(ctx, &addr); if (rc) return rc;
        RIP(ctx) = addr; break;
    }

    /* RET imm16  (C2)                                                      */
    case 0xC2: {
        uint32_t imm; rc = fetch32_64(ctx, &imm); if (rc) return rc; /* actually 16 */
        uint64_t addr; rc = pop64(ctx, &addr); if (rc) return rc;
        RIP(ctx) = addr;
        RSP(ctx) += (uint16_t)imm;
        break;
    }

    /* JMP rel8  (EB)                                                       */
    case 0xEB: {
        uint8_t rel; rc = fetch8_64(ctx, &rel); if (rc) return rc;
        RIP(ctx) = RIP(ctx) + (int64_t)(int8_t)rel; break;
    }

    /* JMP rel32  (E9)                                                      */
    case 0xE9: {
        uint32_t rel; rc = fetch32_64(ctx, &rel); if (rc) return rc;
        RIP(ctx) = RIP(ctx) + (int64_t)(int32_t)rel; break;
    }

    /* LEAVE  (C9)                                                          */
    case 0xC9: {
        RSP(ctx) = R(ctx).rbp;
        uint64_t val; rc = pop64(ctx, &val); if (rc) return rc;
        R(ctx).rbp = val; break;
    }

    /* Jcc rel8  (70..7F)                                                  */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint8_t rel; rc = fetch8_64(ctx, &rel); if (rc) return rc;
        if (eval_cond64(ctx, opcode - 0x70))
            RIP(ctx) = RIP(ctx) + (int64_t)(int8_t)rel;
        break;
    }

    /* CLD / STD                                                            */
    case 0xFC: RFLAGS(ctx) &= ~FL_DF; break;
    case 0xFD: RFLAGS(ctx) |=  FL_DF; break;
    case 0xF8: RFLAGS(ctx) &= ~FL_CF; break;
    case 0xF9: RFLAGS(ctx) |=  FL_CF; break;

    /* PUSHFQ / POPFQ  (9C/9D)                                            */
    case 0x9C: rc = push64(ctx, RFLAGS(ctx)); break;
    case 0x9D: { uint64_t v; rc = pop64(ctx, &v); if(!rc) RFLAGS(ctx) = v; break; }

    /* POP r/m64  (8F)                                                      */
    case 0x8F: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        uint64_t val; rc = pop64(ctx, &val); if (rc) return rc;
        rc = modrm_write_op(ctx, &m, rex, val); break;
    }

    /* ------------------------------------------------------------------ */
    /* String ops                                                           */
    case 0xA5: { /* MOVSD / MOVSQ */
        uint64_t count = rep_prefix ? *reg64(ctx,1) : 1; /* RCX */
        int dir = (RFLAGS(ctx) & FL_DF) ? -1 : 1;
        if (rex.W) {
            while (count--) {
                uint64_t v; rc = cai_mem_read64(ctx, *reg64(ctx,6), &v); if(rc) return rc;
                rc = cai_mem_write64(ctx, *reg64(ctx,7), v); if(rc) return rc;
                *reg64(ctx,6) += (uint64_t)(int64_t)(dir * 8);
                *reg64(ctx,7) += (uint64_t)(int64_t)(dir * 8);
            }
        } else {
            while (count--) {
                uint32_t v; rc = cai_mem_read32(ctx, *reg64(ctx,6), &v); if(rc) return rc;
                rc = cai_mem_write32(ctx, *reg64(ctx,7), v); if(rc) return rc;
                *reg64(ctx,6) += (uint64_t)(int64_t)(dir * 4);
                *reg64(ctx,7) += (uint64_t)(int64_t)(dir * 4);
            }
        }
        if (rep_prefix) *reg64(ctx,1) = 0;
        break;
    }
    case 0xAB: { /* STOSD / STOSQ */
        uint64_t count = rep_prefix ? *reg64(ctx,1) : 1;
        int dir = (RFLAGS(ctx) & FL_DF) ? -1 : 1;
        uint64_t rdi = *reg64(ctx,7);
        if (rex.W) {
            while (count--) { rc = cai_mem_write64(ctx, rdi, *reg64(ctx,0)); if(rc) return rc; rdi += (uint64_t)(int64_t)(dir*8); }
        } else {
            while (count--) { rc = cai_mem_write32(ctx, rdi, (uint32_t)*reg64(ctx,0)); if(rc) return rc; rdi += (uint64_t)(int64_t)(dir*4); }
        }
        *reg64(ctx,7) = rdi;
        if (rep_prefix) *reg64(ctx,1) = 0;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Two-byte escape (0F xx)                                             */
    case 0x0F: {
        uint8_t op2; rc = fetch8_64(ctx, &op2); if (rc) return rc;
        switch (op2) {

        /* SYSCALL  (0F 05)                                                */
        case 0x05: {
            int64_t result = cai_syscall_dispatch(ctx, *reg64(ctx,0));
            *reg64(ctx,0) = (uint64_t)result;
            if (!ctx->running) return CAI_EXITED;
            break;
        }

        /* Jcc rel32  (0F 80..8F)                                          */
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
        case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
            uint32_t rel; rc = fetch32_64(ctx, &rel); if (rc) return rc;
            if (eval_cond64(ctx, op2 - 0x80))
                RIP(ctx) = RIP(ctx) + (int64_t)(int32_t)rel;
            break;
        }

        /* IMUL r, r/m  (0F AF)                                            */
        case 0xAF: {
            modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
            uint64_t src; rc = modrm_read_op(ctx,&m,rex,&src); if(rc) return rc;
            if (rex.W) {
                *reg64(ctx,m.reg) = (uint64_t)((int64_t)*reg64(ctx,m.reg) * (int64_t)src);
            } else {
                uint32_t r = (uint32_t)((int32_t)(*reg64(ctx,m.reg)&0xFFFFFFFF) * (int32_t)(src&0xFFFFFFFF));
                *reg64(ctx,m.reg) = (uint64_t)r;
            }
            break;
        }

        /* MOVZX r, r/m8  (0F B6)                                          */
        case 0xB6: {
            modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
            uint8_t val; rc = modrm_read8_64(ctx,&m,rex,&val); if(rc) return rc;
            *reg64(ctx,m.reg) = (uint64_t)val;
            break;
        }
        /* MOVZX r, r/m16  (0F B7)                                         */
        case 0xB7: {
            modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
            uint16_t val;
            if (m.is_reg) val = (uint16_t)(*reg64(ctx,m.rm) & 0xFFFF);
            else { rc = cai_mem_read16(ctx, m.ea, &val); if(rc) return rc; }
            *reg64(ctx,m.reg) = (uint64_t)val;
            break;
        }
        /* MOVSX r, r/m8  (0F BE)                                          */
        case 0xBE: {
            modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
            uint8_t val; rc = modrm_read8_64(ctx,&m,rex,&val); if(rc) return rc;
            *reg64(ctx,m.reg) = rex.W ? (uint64_t)(int64_t)(int8_t)val
                                       : (uint64_t)(uint32_t)(int32_t)(int8_t)val;
            break;
        }
        /* MOVSX r, r/m32  (0F 63 - actually separate opcode group)        */
        /* MOVSXD r64, r/m32  (63 with REX.W)                             */

        /* SETcc r/m8  (0F 90..9F)                                         */
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
        case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
            rc = modrm_write8_64(ctx, &m, rex, eval_cond64(ctx, op2-0x90) ? 1 : 0);
            break;
        }

        default:
            debuglog(DEBUG_WARN, "cai/x86_64: unimplemented 0F %02X at rip=%llX\n",
                     op2, (unsigned long long)saved_rip);
            return CAI_EILL;
        }
        break;
    }

    /* MOVSXD r64, r/m32  (63 with REX.W=1) / ARPL without REX           */
    case 0x63: {
        modrm64_t m; rc = decode_modrm64(ctx, rex, &m); if (rc) return rc;
        if (rex.W) {
            uint32_t val; rc = modrm_read_op(ctx,&m,rex,&val); /* force 32-bit read */
            /* Actually need a 32-bit read regardless of REX.W here */
            if (m.is_reg) val = (uint32_t)(*reg64(ctx,m.rm) & 0xFFFFFFFF);
            else { rc = cai_mem_read32(ctx, m.ea, &val); if(rc) return rc; }
            *reg64(ctx, m.reg) = (uint64_t)(int64_t)(int32_t)val;
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    default:
        debuglog(DEBUG_WARN, "cai/x86_64: unimplemented opcode 0x%02X at rip=%llX\n",
                 opcode, (unsigned long long)saved_rip);
        RIP(ctx) = saved_rip;
        return CAI_EILL;
    }

    if (rc) return rc;
    ctx->pc = RIP(ctx);
    return CAI_OK;
}

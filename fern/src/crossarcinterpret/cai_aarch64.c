/*
 * cai_aarch64.c - AArch64 (ARMv8-A 64-bit) instruction interpreter
 *
 * Implements cai_aarch64_step() for the Fern crossarcinterpret framework.
 *
 * All AArch64 instructions are 32-bit fixed-width, little-endian.
 * The decoder follows the ARMv8-A Architecture Reference Manual encoding
 * hierarchy (op0 bits [31:29], then sub-group bits).
 *
 * Supported instruction groups
 * ----------------------------
 *  Data-processing (register)
 *    ADD/ADDS, SUB/SUBS, AND/ANDS, ORR, EOR, BIC, ORN, EON
 *    LSL/LSR/ASR/ROR (aliases of variable-shift data-processing)
 *    CMP/CMN (aliases of SUBS/ADDS to XZR)
 *    TST     (alias  of ANDS  to XZR)
 *    NEG/NEGS (aliases of SUB/SUBS from XZR)
 *
 *  Data-processing (immediate)
 *    ADD/ADDS/SUB/SUBS Rd, Rn, #imm12 {, LSL #0|12}
 *    MOVZ  Xd, #imm16, LSL #shift     (zero remaining halfwords)
 *    MOVK  Xd, #imm16, LSL #shift     (keep remaining halfwords)
 *    MOVN  Xd, #imm16, LSL #shift     (NOT of shifted immediate)
 *    Logical immediates: AND/ORR/EOR/ANDS Xd, Xn, #bitmask
 *
 *  Load / Store
 *    LDR/STR   Xt, [Xn, #imm12]        (unsigned offset, 64-bit)
 *    LDR/STR   Wt, [Xn, #imm12]        (unsigned offset, 32-bit)
 *    LDRB/STRB Wt, [Xn, #imm12]        (unsigned offset, byte)
 *    LDRH/STRH Wt, [Xn, #imm12]        (unsigned offset, halfword)
 *    LDRSB/LDRSH/LDRSW                  (sign-extended loads)
 *    LDR/STR   Xt, [Xn, #simm9]!       (pre-index)
 *    LDR/STR   Xt, [Xn], #simm9        (post-index)
 *    LDP/STP   Xt1, Xt2, [Xn, #imm7]  (load/store pair, signed offset)
 *    LDP/STP   pre/post index variants
 *    LDR Xt, #imm19                     (PC-relative literal)
 *
 *  Branch
 *    B   #imm26          (unconditional, PC-relative)
 *    BL  #imm26          (branch-with-link, sets x30)
 *    BR  Xn              (branch to register)
 *    BLR Xn              (branch-with-link to register)
 *    RET {Xn}            (return via x30 by default)
 *    B.cond #imm19       (conditional branch, all 16 conditions)
 *    CBZ/CBNZ Xt, #imm19 (compare-and-branch zero/non-zero)
 *    TBZ/TBNZ Xt, #b, #imm14 (test-bit-and-branch)
 *
 *  System
 *    SVC  #imm16         (Linux uses SVC #0; x8=nr, x0-x5=args, ret→x0)
 *    NOP  (0xD503201F)
 *    MSR/MRS NZCV        (access condition flags)
 *
 * Unimplemented instructions return CAI_EILL so the caller can report them.
 *
 * Integration
 * -----------
 * The "main" entry point for normal Fern use is cai_aarch64_step()
 * declared in crossarcinterpret.h, which takes a cai_context_t*.  The
 * stand-alone variants (cai_aarch64_ctx_t) are thin wrappers for unit tests.
 *
 * XZR semantics
 * -------------
 * Register index 31 is the zero register (XZR) in most encodings:
 *   reads  → 0
 *   writes → discarded (treated as /dev/null)
 * SP is selected instead of XZR only in a small number of specific
 * encodings (stack-pointer-qualified instructions); those cases are
 * handled explicitly.
 */

#include "crossarcinterpret.h"
#include "cai_aarch64.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

/* =========================================================================
 * Compile-time / portability helpers
 * ========================================================================= */

/* Sign-extend a value that occupies @bits bits */
static inline int64_t sign_extend64(uint64_t val, unsigned bits)
{
    uint64_t mask = (uint64_t)1 << (bits - 1);
    return (int64_t)((val ^ mask) - mask);
}

static inline int32_t sign_extend32(uint32_t val, unsigned bits)
{
    uint32_t mask = (uint32_t)1 << (bits - 1);
    return (int32_t)((val ^ mask) - mask);
}

/* Rotate-right 64-bit */
static inline uint64_t ror64(uint64_t val, unsigned shift)
{
    shift &= 63;
    if (shift == 0) return val;
    return (val >> shift) | (val << (64 - shift));
}

/* Rotate-right 32-bit */
static inline uint32_t ror32(uint32_t val, unsigned shift)
{
    shift &= 31;
    if (shift == 0) return val;
    return (val >> shift) | (val << (32 - shift));
}

/* Count leading zeros (64-bit) – avoid compiler built-ins for portability */
static inline unsigned clz64(uint64_t val)
{
    if (val == 0) return 64;
    unsigned n = 0;
    if (!(val >> 32)) { n += 32; val <<= 32; }
    if (!(val >> 48)) { n += 16; val <<= 16; }
    if (!(val >> 56)) { n +=  8; val <<=  8; }
    if (!(val >> 60)) { n +=  4; val <<=  4; }
    if (!(val >> 62)) { n +=  2; val <<=  2; }
    if (!(val >> 63)) { n +=  1; }
    return n;
}

/* =========================================================================
 * AArch64 bitmask immediate decoding (DecodeBitMasks)
 *
 * The logical-immediate encoding stores N:immr:imms in 13 bits.
 * This produces the canonical "element replicated" bitmask described in
 * the ARM ARM section C4.2.1.
 * ========================================================================= */

static bool decode_bitmask(uint32_t N, uint32_t immr, uint32_t imms,
                            bool is64, uint64_t *out_wmask, uint64_t *out_tmask)
{
    unsigned len;

    /* Determine element size */
    if (N) {
        len = 6; /* 64-bit elements */
        if (!is64) return false; /* N=1 only valid in 64-bit ops */
    } else {
        /* Find highest set bit in ~(imms | (imms<<1)) limited to 6 bits */
        uint32_t inv = (~imms) & 0x3F;
        if (inv == 0) return false;
        /* len = highest bit position in inv */
        len = 0;
        if (inv & 0x20) len = 5;
        else if (inv & 0x10) len = 4;
        else if (inv & 0x08) len = 3;
        else if (inv & 0x04) len = 2;
        else if (inv & 0x02) len = 1;
        else                 len = 0;
    }

    unsigned esize = 1u << len;         /* element size: 2,4,8,16,32,64 bits */
    unsigned S = imms & (esize - 1);    /* number of set bits minus 1        */
    unsigned R = immr & (esize - 1);    /* right-rotation amount             */

    /* Build the "S+1 ones" base element */
    uint64_t welem = (S == esize - 1) ? ~(uint64_t)0
                                      : ((uint64_t)1 << (S + 1)) - 1;

    /* Rotate right by R within the element */
    if (R) {
        welem = ((welem >> R) | (welem << (esize - R))) & (((uint64_t)1 << esize) - 1);
    }

    /* Replicate across 64 bits */
    uint64_t wmask = welem;
    if (esize < 64) {
        unsigned reps = 64 / esize;
        for (unsigned i = 1; i < reps; i++)
            wmask |= wmask << (i * esize);
    }

    if (!is64) wmask &= 0xFFFFFFFFULL;

    if (out_wmask) *out_wmask = wmask;
    if (out_tmask) {
        /* tmask is the field covered by imms ones shifted to element-aligned */
        uint64_t t = ((uint64_t)1 << (S + 1)) - 1;
        /* Replicate */
        uint64_t tmask = t;
        if (esize < 64) {
            unsigned reps = 64 / esize;
            for (unsigned i = 1; i < reps; i++)
                tmask |= t << (i * esize);
        }
        if (!is64) tmask &= 0xFFFFFFFFULL;
        *out_tmask = tmask;
    }
    return true;
}

/* =========================================================================
 * Condition code evaluation
 * ========================================================================= */

#define NZCV_GET_N(flags)  (!!((flags) & NZCV_N))
#define NZCV_GET_Z(flags)  (!!((flags) & NZCV_Z))
#define NZCV_GET_C(flags)  (!!((flags) & NZCV_C))
#define NZCV_GET_V(flags)  (!!((flags) & NZCV_V))

/*
 * eval_cond - evaluate a 4-bit AArch64 condition code against nzcv flags.
 * Returns true when the condition is met.
 */
static bool eval_cond(uint64_t nzcv, uint32_t cond)
{
    bool N = NZCV_GET_N(nzcv);
    bool Z = NZCV_GET_Z(nzcv);
    bool C = NZCV_GET_C(nzcv);
    bool V = NZCV_GET_V(nzcv);
    bool result;

    switch (cond >> 1) {
    case 0: result = Z;               break; /* EQ / NE */
    case 1: result = C;               break; /* CS / CC */
    case 2: result = N;               break; /* MI / PL */
    case 3: result = V;               break; /* VS / VC */
    case 4: result = C && !Z;         break; /* HI / LS */
    case 5: result = (N == V);        break; /* GE / LT */
    case 6: result = (N == V) && !Z;  break; /* GT / LE */
    case 7: result = true;            break; /* AL / NV */
    default: result = false;          break;
    }

    /* Odd condition codes invert (except 0xF which stays true) */
    if ((cond & 1) && cond != 0xF)
        result = !result;
    return result;
}

/* =========================================================================
 * NZCV update helpers
 * ========================================================================= */

/*
 * update_nzcv_add - set N,Z,C,V after a 64-bit addition: result = a + b + cin
 */
static uint64_t update_nzcv_add(uint64_t a, uint64_t b, uint64_t cin)
{
    uint64_t result = a + b + cin;
    uint64_t flags = 0;

    if (result >> 63)            flags |= NZCV_N;
    if (result == 0)             flags |= NZCV_Z;
    /* Carry: unsigned overflow */
    if (result < a || (cin && result == a))
        flags |= NZCV_C;
    /* Overflow: signed overflow */
    /* Same sign inputs, different sign result */
    if (!((a ^ b) >> 63) && ((a ^ result) >> 63))
        flags |= NZCV_V;
    return flags;
}

/*
 * update_nzcv_add32 - same for 32-bit addition
 */
static uint64_t update_nzcv_add32(uint32_t a, uint32_t b, uint32_t cin)
{
    uint64_t r64 = (uint64_t)a + b + cin;
    uint32_t result = (uint32_t)r64;
    uint64_t flags = 0;

    if (result >> 31)            flags |= NZCV_N;
    if (result == 0)             flags |= NZCV_Z;
    if (r64 >> 32)               flags |= NZCV_C;
    if (!((a ^ b) >> 31) && ((a ^ result) >> 31))
        flags |= NZCV_V;
    return flags;
}

/*
 * update_nzcv_logic - set N,Z after a logical op; clears C and V.
 */
static uint64_t update_nzcv_logic64(uint64_t result)
{
    uint64_t flags = 0;
    if (result >> 63)  flags |= NZCV_N;
    if (result == 0)   flags |= NZCV_Z;
    return flags; /* C=0, V=0 */
}

static uint64_t update_nzcv_logic32(uint32_t result)
{
    uint64_t flags = 0;
    if (result >> 31)  flags |= NZCV_N;
    if (result == 0)   flags |= NZCV_Z;
    return flags;
}

/* =========================================================================
 * Register access through cai_context_t
 * ========================================================================= */

/*
 * Read general-purpose register Xn (64-bit).
 * Index 31 → XZR (returns 0).
 */
static inline uint64_t xreg_rd(cai_context_t *ctx, unsigned n)
{
    if (n == 31) return 0;
    return ctx->cpu.aarch64.x[n];
}

/*
 * Write general-purpose register Xd (64-bit).
 * Index 31 → XZR (discarded).
 */
static inline void xreg_wr(cai_context_t *ctx, unsigned d, uint64_t val)
{
    if (d != 31) ctx->cpu.aarch64.x[d] = val;
}

/*
 * Read Wn (32-bit view of Xn).  Returns zero-extended lower 32 bits.
 */
static inline uint32_t wreg_rd(cai_context_t *ctx, unsigned n)
{
    return (uint32_t)xreg_rd(ctx, n);
}

/*
 * Write Wd (32-bit).  Zero-extends to 64 bits (upper 32 cleared).
 */
static inline void wreg_wr(cai_context_t *ctx, unsigned d, uint32_t val)
{
    if (d != 31) ctx->cpu.aarch64.x[d] = (uint64_t)val;
}

/* SP accessors (SP always uses the actual sp field, not x[]) */
static inline uint64_t sp_rd(cai_context_t *ctx)
{
    return ctx->cpu.aarch64.sp;
}
static inline void sp_wr(cai_context_t *ctx, uint64_t val)
{
    ctx->cpu.aarch64.sp = val;
}

/*
 * Read Xn, but register 31 means SP (used by ADD/SUB sp-qualified forms).
 */
static inline uint64_t xreg_sp_rd(cai_context_t *ctx, unsigned n)
{
    if (n == 31) return sp_rd(ctx);
    return ctx->cpu.aarch64.x[n];
}
static inline void xreg_sp_wr(cai_context_t *ctx, unsigned d, uint64_t val)
{
    if (d == 31) { sp_wr(ctx, val); return; }
    ctx->cpu.aarch64.x[d] = val;
}

/* Shorthand for PC */
#define PC  (ctx->cpu.aarch64.pc)
#define NZCV (ctx->cpu.aarch64.nzcv)

/* =========================================================================
 * Guest memory access through cai_context_t
 * ========================================================================= */

static inline uint8_t mem_rd8(cai_context_t *ctx, uint64_t addr)
{
    uint8_t v = 0;
    cai_mem_read8(ctx, addr, &v);
    return v;
}
static inline uint16_t mem_rd16(cai_context_t *ctx, uint64_t addr)
{
    uint16_t v = 0;
    cai_mem_read16(ctx, addr, &v);
    return v;
}
static inline uint32_t mem_rd32(cai_context_t *ctx, uint64_t addr)
{
    uint32_t v = 0;
    cai_mem_read32(ctx, addr, &v);
    return v;
}
static inline uint64_t mem_rd64(cai_context_t *ctx, uint64_t addr)
{
    uint64_t v = 0;
    cai_mem_read64(ctx, addr, &v);
    return v;
}
static inline int mem_wr8 (cai_context_t *ctx, uint64_t a, uint8_t  v)
{ return cai_mem_write8 (ctx, a, v); }
static inline int mem_wr16(cai_context_t *ctx, uint64_t a, uint16_t v)
{ return cai_mem_write16(ctx, a, v); }
static inline int mem_wr32(cai_context_t *ctx, uint64_t a, uint32_t v)
{ return cai_mem_write32(ctx, a, v); }
static inline int mem_wr64(cai_context_t *ctx, uint64_t a, uint64_t v)
{ return cai_mem_write64(ctx, a, v); }

/* =========================================================================
 * Shift / extend helpers
 * ========================================================================= */

typedef enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3 } sh_type_t;

static uint64_t apply_shift64(uint64_t val, sh_type_t sh, unsigned amount)
{
    amount &= 63;
    switch (sh) {
    case SH_LSL: return amount < 64 ? val << amount : 0;
    case SH_LSR: return amount < 64 ? val >> amount : 0;
    case SH_ASR: return (uint64_t)((int64_t)val >> (amount < 64 ? amount : 63));
    case SH_ROR: return ror64(val, amount);
    }
    return val;
}

static uint32_t apply_shift32(uint32_t val, sh_type_t sh, unsigned amount)
{
    amount &= 31;
    switch (sh) {
    case SH_LSL: return amount < 32 ? val << amount : 0;
    case SH_LSR: return amount < 32 ? val >> amount : 0;
    case SH_ASR: return (uint32_t)((int32_t)val >> (amount < 32 ? amount : 31));
    case SH_ROR: return ror32(val, amount);
    }
    return val;
}

/* =========================================================================
 * Decode helpers for common field positions
 * ========================================================================= */

#define BITS(insn, hi, lo) (((insn) >> (lo)) & ((1u << ((hi)-(lo)+1)) - 1))
#define BIT(insn, pos)     (((insn) >> (pos)) & 1u)

/* =========================================================================
 * Instruction group decoders (called from cai_aarch64_step)
 * ========================================================================= */

/* ------------------------------------------------------------------
 * Data-processing (register) – op0 = 0b?01_0101 style
 * Covers: logical shifted-reg, add/sub shifted-reg, add/sub ext-reg,
 *         conditional select, data-processing 1/2 source, etc.
 * ------------------------------------------------------------------ */

static int decode_dp_register(cai_context_t *ctx, uint32_t insn)
{
    /* sf: bit 31 (1=64-bit, 0=32-bit) */
    unsigned sf  = BIT(insn, 31);
    unsigned op  = BITS(insn, 30, 29); /* sub-group opcode */
    unsigned S   = BIT(insn, 29);      /* set flags */
    unsigned g   = BITS(insn, 28, 24); /* instruction group identifier */

    /* ---- Add/Sub shifted register  [sf|op|S|01011|shift|0|Rm|imm6|Rn|Rd] ---- */
    if ((g & 0x1F) == 0x0B) {
        /* op:  0=ADD, 1=SUB, Bit29=S (flags) */
        unsigned shift = BITS(insn, 23, 22);
        /* Bit 21 must be 0 for shifted-reg variant */
        if (BIT(insn, 21) == 0) {
            unsigned Rm    = BITS(insn, 20, 16);
            unsigned imm6  = BITS(insn, 15, 10);
            unsigned Rn    = BITS(insn,  9,  5);
            unsigned Rd    = BITS(insn,  4,  0);

            if (sf) {
                /* 64-bit */
                uint64_t operand1 = xreg_rd(ctx, Rn);
                uint64_t operand2 = apply_shift64(xreg_rd(ctx, Rm),
                                                  (sh_type_t)shift, imm6);
                uint64_t result;
                if (!op) { /* ADD */
                    result = operand1 + operand2;
                    if (S) NZCV = update_nzcv_add(operand1, operand2, 0);
                } else {   /* SUB */
                    result = operand1 - operand2;
                    if (S) NZCV = update_nzcv_add(operand1, ~operand2, 1);
                }
                xreg_wr(ctx, Rd, result);
            } else {
                /* 32-bit */
                uint32_t operand1 = wreg_rd(ctx, Rn);
                uint32_t operand2 = apply_shift32(wreg_rd(ctx, Rm),
                                                  (sh_type_t)shift, imm6);
                uint32_t result;
                if (!op) {
                    result = operand1 + operand2;
                    if (S) NZCV = update_nzcv_add32(operand1, operand2, 0);
                } else {
                    result = operand1 - operand2;
                    if (S) NZCV = update_nzcv_add32(operand1, ~operand2, 1);
                }
                wreg_wr(ctx, Rd, result);
            }
            return CAI_OK;
        }
        /* Bit21=1 → add/sub extended register */
        if (BIT(insn, 21) == 1) {
            unsigned opt   = BITS(insn, 23, 22); /* must be 00 */
            unsigned Rm    = BITS(insn, 20, 16);
            unsigned ext   = BITS(insn, 15, 13); /* extend type */
            unsigned sh    = BITS(insn, 12, 10); /* left shift 0-4 */
            unsigned Rn    = BITS(insn,  9,  5);
            unsigned Rd    = BITS(insn,  4,  0);
            (void)opt;

            uint64_t operand1 = xreg_sp_rd(ctx, Rn);
            uint64_t rm_val   = xreg_rd(ctx, Rm);
            /* Apply extend */
            uint64_t operand2;
            switch (ext) {
            case 0: operand2 = (uint64_t)(uint8_t) rm_val; break; /* UXTB */
            case 1: operand2 = (uint64_t)(uint16_t)rm_val; break; /* UXTH */
            case 2: operand2 = (uint64_t)(uint32_t)rm_val; break; /* UXTW */
            case 3: operand2 = rm_val;                      break; /* UXTX */
            case 4: operand2 = (uint64_t)(int64_t)(int8_t) rm_val; break; /* SXTB */
            case 5: operand2 = (uint64_t)(int64_t)(int16_t)rm_val; break; /* SXTH */
            case 6: operand2 = (uint64_t)(int64_t)(int32_t)rm_val; break; /* SXTW */
            case 7: operand2 = rm_val;                      break; /* SXTX */
            default: operand2 = rm_val; break;
            }
            operand2 <<= sh;

            uint64_t result;
            if (!op) {
                result = operand1 + operand2;
                if (S) NZCV = update_nzcv_add(operand1, operand2, 0);
            } else {
                result = operand1 - operand2;
                if (S) NZCV = update_nzcv_add(operand1, ~operand2, 1);
            }
            if (!sf) result &= 0xFFFFFFFFULL;
            xreg_sp_wr(ctx, Rd, result);
            return CAI_OK;
        }
    }

    /* ---- Logical (shifted register)  [sf|opc|01010|shift|N|Rm|imm6|Rn|Rd] ---- */
    if ((g & 0x1F) == 0x0A) {
        unsigned opc   = BITS(insn, 30, 29);
        unsigned shift = BITS(insn, 23, 22);
        unsigned N     = BIT(insn, 21);   /* invert Rm? */
        unsigned Rm    = BITS(insn, 20, 16);
        unsigned imm6  = BITS(insn, 15, 10);
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rd    = BITS(insn,  4,  0);

        if (sf) {
            uint64_t operand1 = xreg_rd(ctx, Rn);
            uint64_t operand2 = apply_shift64(xreg_rd(ctx, Rm),
                                              (sh_type_t)shift, imm6);
            if (N) operand2 = ~operand2;
            uint64_t result;
            switch (opc) {
            case 0: result = operand1 & operand2; break; /* AND / BIC */
            case 1: result = operand1 | operand2; break; /* ORR / ORN */
            case 2: result = operand1 ^ operand2; break; /* EOR / EON */
            case 3: result = operand1 & operand2;        /* ANDS / BICS */
                    NZCV = update_nzcv_logic64(result);
                    xreg_wr(ctx, Rd, result);
                    return CAI_OK;
            default: return CAI_EILL;
            }
            xreg_wr(ctx, Rd, result);
        } else {
            uint32_t operand1 = wreg_rd(ctx, Rn);
            uint32_t operand2 = apply_shift32(wreg_rd(ctx, Rm),
                                              (sh_type_t)shift, imm6);
            if (N) operand2 = ~operand2;
            uint32_t result;
            switch (opc) {
            case 0: result = operand1 & operand2; break;
            case 1: result = operand1 | operand2; break;
            case 2: result = operand1 ^ operand2; break;
            case 3: result = operand1 & operand2;
                    NZCV = update_nzcv_logic32(result);
                    wreg_wr(ctx, Rd, result);
                    return CAI_OK;
            default: return CAI_EILL;
            }
            wreg_wr(ctx, Rd, result);
        }
        return CAI_OK;
    }

    /* ---- Data-processing 1-source  [sf|1|S|11010110|opcode2|opcode|Rn|Rd] ---- */
    /* Covers RBIT, REV16, REV32, REV, CLZ, CLS */
    if (BITS(insn, 30, 21) == 0x2D6 || BITS(insn, 30, 21) == 0x2D7) {
        unsigned Rn   = BITS(insn, 9, 5);
        unsigned Rd   = BITS(insn, 4, 0);
        unsigned opc  = BITS(insn, 15, 10);
        if (sf) {
            uint64_t val = xreg_rd(ctx, Rn);
            uint64_t res = 0;
            switch (opc) {
            case 0: /* RBIT */
                for (int b = 0; b < 64; b++) res |= ((val >> b) & 1ULL) << (63 - b);
                break;
            case 1: /* REV16 */
                for (int w = 0; w < 4; w++) {
                    uint16_t hw = (val >> (w * 16)) & 0xFFFF;
                    hw = (uint16_t)((hw >> 8) | (hw << 8));
                    res |= (uint64_t)hw << (w * 16);
                }
                break;
            case 2: /* REV32 */
                for (int w = 0; w < 2; w++) {
                    uint32_t dw = (val >> (w * 32)) & 0xFFFFFFFF;
                    dw = ((dw >> 24) | ((dw >> 8) & 0xFF00) |
                          ((dw << 8) & 0xFF0000) | (dw << 24));
                    res |= (uint64_t)dw << (w * 32);
                }
                break;
            case 3: /* REV */
                for (int b = 0; b < 8; b++)
                    res |= ((val >> (b * 8)) & 0xFF) << ((7 - b) * 8);
                break;
            case 4: /* CLZ */
                res = (uint64_t)clz64(val);
                break;
            default: return CAI_EILL;
            }
            xreg_wr(ctx, Rd, res);
        } else {
            uint32_t val = wreg_rd(ctx, Rn);
            uint32_t res = 0;
            switch (opc) {
            case 0: /* RBIT */
                for (int b = 0; b < 32; b++) res |= ((val >> b) & 1u) << (31 - b);
                break;
            case 1: /* REV16 */
                res = (uint32_t)(((val & 0x00FF00FF) << 8) | ((val >> 8) & 0x00FF00FF));
                break;
            case 2: /* REV */
                res = ((val >> 24) | ((val >> 8) & 0xFF00) |
                       ((val << 8) & 0xFF0000) | (val << 24));
                break;
            case 4: /* CLZ */
                res = (uint32_t)clz64((uint64_t)val) - 32;
                break;
            default: return CAI_EILL;
            }
            wreg_wr(ctx, Rd, res);
        }
        return CAI_OK;
    }

    /* ---- Data-processing 2-source (variable shifts, UDIV, SDIV, ...) ---- */
    /* [sf|0|S|11010110|Rm|opcode|Rn|Rd]  (bits 28:21 = 11010110) */
    if (BITS(insn, 28, 21) == 0xD6) {
        unsigned Rm   = BITS(insn, 20, 16);
        unsigned opc  = BITS(insn, 15, 10);
        unsigned Rn   = BITS(insn,  9,  5);
        unsigned Rd   = BITS(insn,  4,  0);

        if (sf) {
            uint64_t n = xreg_rd(ctx, Rn);
            uint64_t m = xreg_rd(ctx, Rm);
            uint64_t res = 0;
            switch (opc) {
            case 0x02: res = m ? (uint64_t)((int64_t)n / (int64_t)m) : 0; break; /* SDIV */
            case 0x03: res = m ? n / m : 0; break;                                /* UDIV */
            case 0x08: res = m & 63 ? (n << (m & 63)) : n; break;                /* LSLV */
            case 0x09: res = m & 63 ? (n >> (m & 63)) : n; break;                /* LSRV */
            case 0x0A: res = (uint64_t)((int64_t)n >> (m & 63)); break;           /* ASRV */
            case 0x0B: res = ror64(n, (unsigned)(m & 63)); break;                 /* RORV */
            case 0x00: /* SUBP / not implemented as data-proc-2src here */ return CAI_EILL;
            default:   return CAI_EILL;
            }
            xreg_wr(ctx, Rd, res);
        } else {
            uint32_t n = wreg_rd(ctx, Rn);
            uint32_t m = wreg_rd(ctx, Rm);
            uint32_t res = 0;
            switch (opc) {
            case 0x02: res = m ? (uint32_t)((int32_t)n / (int32_t)m) : 0; break;
            case 0x03: res = m ? n / m : 0; break;
            case 0x08: res = m & 31 ? (n << (m & 31)) : n; break;
            case 0x09: res = m & 31 ? (n >> (m & 31)) : n; break;
            case 0x0A: res = (uint32_t)((int32_t)n >> (m & 31)); break;
            case 0x0B: res = ror32(n, (unsigned)(m & 31)); break;
            default:   return CAI_EILL;
            }
            wreg_wr(ctx, Rd, res);
        }
        return CAI_OK;
    }

    /* ---- Conditional select  [sf|op|S|11010100|Rm|cond|op2|Rn|Rd] ---- */
    if (BITS(insn, 28, 21) == 0xD4) {
        unsigned op2  = BITS(insn, 11, 10);
        unsigned cond4= BITS(insn, 15, 12);
        unsigned Rm   = BITS(insn, 20, 16);
        unsigned Rn   = BITS(insn,  9,  5);
        unsigned Rd   = BITS(insn,  4,  0);
        bool taken    = eval_cond(NZCV, cond4);

        if (sf) {
            uint64_t tval = xreg_rd(ctx, Rn);
            uint64_t fval = xreg_rd(ctx, Rm);
            uint64_t res;
            switch (op2) {
            case 0: res = taken ? tval : fval;          break; /* CSEL   */
            case 1: res = taken ? tval + 1 : fval + 1;  break; /* CSINC  */
            case 2: res = taken ? tval : ~fval;          break; /* CSINV  */
            case 3: res = taken ? tval : ~fval + 1;      break; /* CSNEG  */
            default: return CAI_EILL;
            }
            /* For CSINC/CSINV/CSNEG false branch uses Rm */
            /* Correct: true=Rn, false=Rm with transform */
            (void)res;
            /* Redo properly */
            uint64_t true_v  = xreg_rd(ctx, Rn);
            uint64_t false_v = xreg_rd(ctx, Rm);
            switch (op2) {
            case 0: res = taken ? true_v : false_v;        break;
            case 1: res = taken ? true_v : false_v + 1;    break;
            case 2: res = taken ? true_v : ~false_v;        break;
            case 3: res = taken ? true_v : (~false_v + 1);  break;
            default: return CAI_EILL;
            }
            xreg_wr(ctx, Rd, res);
        } else {
            uint32_t true_v  = wreg_rd(ctx, Rn);
            uint32_t false_v = wreg_rd(ctx, Rm);
            uint32_t res;
            switch (op2) {
            case 0: res = taken ? true_v : false_v;        break;
            case 1: res = taken ? true_v : false_v + 1;    break;
            case 2: res = taken ? true_v : ~false_v;        break;
            case 3: res = taken ? true_v : (~false_v + 1);  break;
            default: return CAI_EILL;
            }
            wreg_wr(ctx, Rd, res);
        }
        return CAI_OK;
    }

    /* ---- 3-source (MADD / MSUB / SMADDL / SMSUBL / UMADDL / UMSUBL) ---- */
    /* op0=0001_101x => bits 28:24 = 11011 */
    if (BITS(insn, 28, 24) == 0x1B) {
        unsigned op31 = BITS(insn, 23, 21);
        unsigned Rm   = BITS(insn, 20, 16);
        unsigned o0   = BIT(insn, 15);
        unsigned Ra   = BITS(insn, 14, 10);
        unsigned Rn   = BITS(insn,  9,  5);
        unsigned Rd   = BITS(insn,  4,  0);

        if (!sf && op31 == 0) {
            /* MADD / MSUB  (32-bit) */
            uint32_t n = wreg_rd(ctx, Rn);
            uint32_t m = wreg_rd(ctx, Rm);
            uint32_t a = wreg_rd(ctx, Ra);
            uint32_t res = o0 ? a - n * m : a + n * m;
            wreg_wr(ctx, Rd, res);
            return CAI_OK;
        }
        if (sf && op31 == 0) {
            /* MADD / MSUB  (64-bit) */
            uint64_t n = xreg_rd(ctx, Rn);
            uint64_t m = xreg_rd(ctx, Rm);
            uint64_t a = xreg_rd(ctx, Ra);
            uint64_t res = o0 ? a - n * m : a + n * m;
            xreg_wr(ctx, Rd, res);
            return CAI_OK;
        }
        if (sf && op31 == 1) {
            /* SMADDL / SMSUBL */
            int64_t n = (int64_t)(int32_t)wreg_rd(ctx, Rn);
            int64_t m = (int64_t)(int32_t)wreg_rd(ctx, Rm);
            uint64_t a = xreg_rd(ctx, Ra);
            uint64_t res = o0 ? a - (uint64_t)(n * m) : a + (uint64_t)(n * m);
            xreg_wr(ctx, Rd, res);
            return CAI_OK;
        }
        if (sf && op31 == 5) {
            /* UMADDL / UMSUBL */
            uint64_t n = (uint64_t)wreg_rd(ctx, Rn);
            uint64_t m = (uint64_t)wreg_rd(ctx, Rm);
            uint64_t a = xreg_rd(ctx, Ra);
            uint64_t res = o0 ? a - n * m : a + n * m;
            xreg_wr(ctx, Rd, res);
            return CAI_OK;
        }
        /* SMULH / UMULH */
        if (sf && (op31 == 2 || op31 == 6)) {
            /* 128-bit mul, return high 64 bits */
            uint64_t n = xreg_rd(ctx, Rn);
            uint64_t m = xreg_rd(ctx, Rm);
            /* Compute hi64 of 128-bit product via split */
            uint64_t lo_n = n & 0xFFFFFFFF, hi_n = n >> 32;
            uint64_t lo_m = m & 0xFFFFFFFF, hi_m = m >> 32;
            uint64_t t = lo_n * lo_m;
            uint64_t mid1 = lo_n * hi_m;
            uint64_t mid2 = hi_n * lo_m;
            uint64_t carry = ((t >> 32) + (mid1 & 0xFFFFFFFF) + (mid2 & 0xFFFFFFFF)) >> 32;
            uint64_t hi = hi_n * hi_m + (mid1 >> 32) + (mid2 >> 32) + carry;
            if (op31 == 2) {
                /* SMULH: adjust for signed */
                if ((int64_t)n < 0) hi -= m;
                if ((int64_t)m < 0) hi -= n;
            }
            xreg_wr(ctx, Rd, hi);
            return CAI_OK;
        }
        return CAI_EILL;
    }

    return CAI_EILL;
}

/* ------------------------------------------------------------------
 * Data-processing (immediate)
 * Bit pattern: op0 in [100xxxx] for PC-rel, [100010x] for add/sub imm,
 *              [100110x] for logical imm, [101001x] for MOV imm, etc.
 * ------------------------------------------------------------------ */

static int decode_dp_imm(cai_context_t *ctx, uint32_t insn)
{
    unsigned sf  = BIT(insn, 31);
    unsigned op  = BITS(insn, 30, 29);
    unsigned g   = BITS(insn, 28, 23); /* 6-bit sub-group */

    /* ---- PC-relative (ADR / ADRP)  bits[28:24] = 10000 ---- */
    if ((g & 0x1F) == 0x10) {
        unsigned Rd    = BITS(insn, 4, 0);
        uint64_t immlo = BITS(insn, 30, 29);
        uint64_t immhi = BITS(insn, 23, 5);
        int64_t  imm   = (int64_t)sign_extend64((immhi << 2) | immlo, 21);
        if (BIT(insn, 31) == 0) {
            /* ADR */
            xreg_wr(ctx, Rd, (uint64_t)((int64_t)PC + imm));
        } else {
            /* ADRP - page aligned */
            uint64_t base = PC & ~(uint64_t)0xFFF;
            xreg_wr(ctx, Rd, (uint64_t)((int64_t)base + (imm << 12)));
        }
        return CAI_OK;
    }

    /* ---- Add/Sub immediate  bits[28:23] = 10001x ---- */
    if ((g >> 1) == 0x10) {
        unsigned sh    = BIT(insn, 22);   /* shift: 0=LSL#0, 1=LSL#12 */
        unsigned imm12 = BITS(insn, 21, 10);
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rd    = BITS(insn,  4,  0);
        unsigned S     = BIT(insn, 29);
        unsigned sub   = BIT(insn, 30);

        uint64_t imm = (uint64_t)imm12 << (sh ? 12 : 0);
        if (sf) {
            uint64_t operand1 = xreg_sp_rd(ctx, Rn);
            uint64_t result;
            if (!sub) {
                result = operand1 + imm;
                if (S) NZCV = update_nzcv_add(operand1, imm, 0);
            } else {
                result = operand1 - imm;
                if (S) NZCV = update_nzcv_add(operand1, ~imm, 1);
            }
            xreg_sp_wr(ctx, Rd, result);
        } else {
            uint32_t op1 = (uint32_t)xreg_sp_rd(ctx, Rn);
            uint32_t imm32 = (uint32_t)imm;
            uint32_t result;
            if (!sub) {
                result = op1 + imm32;
                if (S) NZCV = update_nzcv_add32(op1, imm32, 0);
            } else {
                result = op1 - imm32;
                if (S) NZCV = update_nzcv_add32(op1, ~imm32, 1);
            }
            xreg_sp_wr(ctx, Rd, (uint64_t)result);
        }
        return CAI_OK;
    }

    /* ---- Logical immediate  bits[28:23] = 10011x ---- */
    if ((g >> 1) == 0x13) {
        unsigned N     = BIT(insn, 22);
        unsigned immr  = BITS(insn, 21, 16);
        unsigned imms  = BITS(insn, 15, 10);
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rd    = BITS(insn,  4,  0);
        unsigned opc   = op; /* 00=AND, 01=ORR, 10=EOR, 11=ANDS */

        uint64_t wmask;
        if (!decode_bitmask(N, immr, imms, sf, &wmask, NULL))
            return CAI_EILL;

        if (sf) {
            uint64_t operand = xreg_rd(ctx, Rn);
            uint64_t result;
            switch (opc) {
            case 0: result = operand & wmask; break; /* AND  */
            case 1: result = operand | wmask; break; /* ORR  */
            case 2: result = operand ^ wmask; break; /* EOR  */
            case 3: result = operand & wmask;        /* ANDS */
                    NZCV = update_nzcv_logic64(result);
                    xreg_wr(ctx, Rd, result);
                    return CAI_OK;
            default: return CAI_EILL;
            }
            /* AND/ORR/EOR can target SP */
            xreg_sp_wr(ctx, Rd, result);
        } else {
            uint32_t operand = wreg_rd(ctx, Rn);
            uint32_t mask32  = (uint32_t)wmask;
            uint32_t result;
            switch (opc) {
            case 0: result = operand & mask32; break;
            case 1: result = operand | mask32; break;
            case 2: result = operand ^ mask32; break;
            case 3: result = operand & mask32;
                    NZCV = update_nzcv_logic32(result);
                    wreg_wr(ctx, Rd, result);
                    return CAI_OK;
            default: return CAI_EILL;
            }
            xreg_sp_wr(ctx, Rd, (uint64_t)result);
        }
        return CAI_OK;
    }

    /* ---- Move wide immediate (MOVN/MOVZ/MOVK)  bits[28:23] = 10010x ---- */
    if ((g >> 1) == 0x12) {
        /* opc: 00=MOVN, 10=MOVZ, 11=MOVK */
        unsigned hw    = BITS(insn, 22, 21);  /* halfword select: 0-3 */
        unsigned imm16 = BITS(insn, 20,  5);
        unsigned Rd    = BITS(insn,  4,  0);

        if (sf == 0 && hw > 1)
            return CAI_EILL; /* 32-bit: only HW=0,1 valid */

        uint64_t shifted = (uint64_t)imm16 << (hw * 16);

        switch (op) {
        case 0: /* MOVN */
            xreg_wr(ctx, Rd, sf ? ~shifted : (~shifted & 0xFFFFFFFF));
            break;
        case 2: /* MOVZ */
            xreg_wr(ctx, Rd, sf ? shifted : (shifted & 0xFFFFFFFF));
            break;
        case 3: /* MOVK */
            if (Rd != 31) {
                uint64_t prev = ctx->cpu.aarch64.x[Rd];
                uint64_t mask = ~((uint64_t)0xFFFF << (hw * 16));
                ctx->cpu.aarch64.x[Rd] = (prev & mask) | shifted;
                if (!sf) ctx->cpu.aarch64.x[Rd] &= 0xFFFFFFFF;
            }
            break;
        default:
            return CAI_EILL;
        }
        return CAI_OK;
    }

    /* ---- Bitfield operations (SBFM / BFM / UBFM)  bits[28:23] = 10011x ---- */
    /* Already handled above for logical imm; the overlap is resolved by
     * checking opc values: logical=00/01/10/11 and bitfield=00/01/10 but
     * are encoded differently because bit 22 (N) separates them. */

    /* ---- Bitfield  bits[28:23] = 10011x  (same range but different opc) ---- */
    /* Encoding: sf | opc | 100110 | N | immr | imms | Rn | Rd
     * opc: 00=SBFM, 01=BFM, 10=UBFM */
    if ((g >> 1) == 0x13 || (g >> 1) == 0x12) {
        /* Already handled MOVZ/MOVK/MOVN and logical imm above */
        /* Reach here only for bitfield (opc encoded in bits 30:29) */
        if (op <= 2) {
            unsigned N     = BIT(insn, 22);
            unsigned immr  = BITS(insn, 21, 16);
            unsigned imms  = BITS(insn, 15, 10);
            unsigned Rn    = BITS(insn,  9,  5);
            unsigned Rd    = BITS(insn,  4,  0);
            (void)N;

            if (sf) {
                uint64_t src = xreg_rd(ctx, Rn);
                uint64_t dst = xreg_rd(ctx, Rd);
                uint64_t wmask, tmask;
                if (!decode_bitmask(1, immr, imms, true, &wmask, &tmask))
                    return CAI_EILL;
                uint64_t bot = ror64(src, immr);
                uint64_t res;
                switch (op) {
                case 0: /* SBFM: replicate sign bit */
                    res = (bot & tmask);
                    if (imms < immr)
                        res |= ~tmask & (((int64_t)src < 0) ? ~(uint64_t)0 : 0);
                    else
                        res = (uint64_t)(int64_t)(bot << (63 - imms)) >> (63 - imms);
                    /* Simplified: sign-extend field */
                    {
                        unsigned width = imms - immr + 1;
                        uint64_t field = (src >> immr) & (((uint64_t)1 << width) - 1);
                        res = (uint64_t)sign_extend64(field, width);
                    }
                    break;
                case 1: /* BFM: merge */
                    res = (dst & ~wmask) | (bot & wmask);
                    break;
                case 2: /* UBFM: zero-extend */
                    {
                        unsigned width = imms - immr + 1;
                        if (imms >= immr) {
                            uint64_t field = (src >> immr) & (((uint64_t)1 << width) - 1);
                            res = field;
                        } else {
                            res = bot & tmask;
                        }
                    }
                    break;
                default: return CAI_EILL;
                }
                xreg_wr(ctx, Rd, res);
            } else {
                uint32_t src = wreg_rd(ctx, Rn);
                uint32_t dst = wreg_rd(ctx, Rd);
                uint64_t wmask, tmask;
                if (!decode_bitmask(0, immr, imms, false, &wmask, &tmask))
                    return CAI_EILL;
                uint32_t bot = ror32(src, immr);
                uint32_t res;
                switch (op) {
                case 0: /* SBFM */
                    {
                        unsigned width = imms - immr + 1;
                        uint32_t field = (src >> immr) & (((uint32_t)1 << width) - 1);
                        res = (uint32_t)sign_extend32(field, width);
                    }
                    break;
                case 1: /* BFM */
                    res = (dst & ~(uint32_t)wmask) | (bot & (uint32_t)wmask);
                    break;
                case 2: /* UBFM */
                    {
                        if (imms >= immr) {
                            unsigned width = imms - immr + 1;
                            res = (src >> immr) & (((uint32_t)1 << width) - 1);
                        } else {
                            res = bot & (uint32_t)tmask;
                        }
                    }
                    break;
                default: return CAI_EILL;
                }
                wreg_wr(ctx, Rd, res);
            }
            return CAI_OK;
        }
    }

    return CAI_EILL;
}

/* ------------------------------------------------------------------
 * Branches, exception generating, system instructions
 * ------------------------------------------------------------------ */

static int decode_branch(cai_context_t *ctx, uint32_t insn)
{
    unsigned op0 = BITS(insn, 31, 29);

    /* ---- Unconditional branch (immediate)  op0 = 000 or 100 ---- */
    if ((op0 & 0x6) == 0x0 && BITS(insn, 28, 26) == 0x5) {
        int64_t imm = sign_extend64(BITS(insn, 25, 0), 26) * 4;
        if (BIT(insn, 31)) { /* BL */
            ctx->cpu.aarch64.x[30] = PC + 4; /* link register */
        }
        PC = (uint64_t)((int64_t)PC + imm);
        return CAI_OK;
    }

    /* ---- Compare and branch (immediate) CBZ / CBNZ ---- */
    if (BITS(insn, 30, 25) == 0x1A || BITS(insn, 30, 25) == 0x1B) {
        unsigned sf  = BIT(insn, 31);
        unsigned op  = BIT(insn, 24);   /* 0=CBZ, 1=CBNZ */
        int64_t  imm = sign_extend64(BITS(insn, 23, 5), 19) * 4;
        unsigned Rt  = BITS(insn, 4, 0);
        uint64_t val = sf ? xreg_rd(ctx, Rt) : wreg_rd(ctx, Rt);
        bool cond    = op ? (val != 0) : (val == 0);
        if (cond) PC = (uint64_t)((int64_t)PC + imm);
        else      PC += 4;
        return CAI_OK;
    }

    /* ---- Test bit and branch TBZ / TBNZ ---- */
    if (BITS(insn, 30, 25) == 0x1C || BITS(insn, 30, 25) == 0x1D) {
        unsigned op    = BIT(insn, 24);
        unsigned b5    = BIT(insn, 31);          /* bit 5 of bit position */
        unsigned b40   = BITS(insn, 23, 19);     /* bits 4:0 of bit pos   */
        unsigned bit   = (b5 << 5) | b40;
        int64_t  imm   = sign_extend64(BITS(insn, 18, 5), 14) * 4;
        unsigned Rt    = BITS(insn, 4, 0);
        uint64_t val   = xreg_rd(ctx, Rt);
        bool     tst   = (val >> bit) & 1;
        bool     cond  = op ? tst : !tst;         /* TBNZ / TBZ */
        if (cond) PC = (uint64_t)((int64_t)PC + imm);
        else      PC += 4;
        return CAI_OK;
    }

    /* ---- Conditional branch (immediate)  B.cond ---- */
    if (BITS(insn, 31, 25) == 0x2A && BIT(insn, 4) == 0) {
        uint32_t cond4 = BITS(insn, 3, 0);
        int64_t  imm   = sign_extend64(BITS(insn, 23, 5), 19) * 4;
        if (eval_cond(NZCV, cond4))
            PC = (uint64_t)((int64_t)PC + imm);
        else
            PC += 4;
        return CAI_OK;
    }

    /* ---- Unconditional branch (register)  BR / BLR / RET / ERET ---- */
    if (BITS(insn, 31, 25) == 0x6B && BITS(insn, 15, 10) == 0 &&
        BITS(insn,  4,  0) == 0) {
        unsigned opc = BITS(insn, 24, 21);
        unsigned op2 = BITS(insn, 20, 16);
        unsigned Rn  = BITS(insn,  9,  5);
        (void)op2;

        switch (opc) {
        case 0: /* BR */
            PC = xreg_rd(ctx, Rn);
            return CAI_OK;
        case 1: /* BLR */
            ctx->cpu.aarch64.x[30] = PC + 4;
            PC = xreg_rd(ctx, Rn);
            return CAI_OK;
        case 2: /* RET */
            PC = xreg_rd(ctx, Rn); /* Rn defaults to x30 in asm, encoder sets it */
            return CAI_OK;
        case 4: /* ERET – skip (kernel use) */
            return CAI_ENOTSUP;
        default:
            return CAI_EILL;
        }
    }

    /* ---- Exception generating (SVC / HVC / SMC / BRK / HLT) ---- */
    if (BITS(insn, 31, 24) == 0xD4) {
        unsigned opc   = BITS(insn, 23, 21);
        unsigned imm16 = BITS(insn, 20,  5);
        unsigned ll    = BITS(insn,  2,  0);
        (void)imm16;

        if (opc == 0 && ll == 1) {
            /* SVC - syscall */
            int64_t result = cai_syscall_dispatch(ctx, ctx->cpu.aarch64.x[8]);
            ctx->cpu.aarch64.x[0] = (uint64_t)result;
            if (!ctx->running) return CAI_EXITED;
            PC += 4;
            return CAI_OK;
        }
        if (opc == 1 && ll == 1) {
            /* HVC - hypervisor call, not supported */
            return CAI_ENOTSUP;
        }
        if (opc == 0 && ll == 0) {
            /* BRK #imm16 – software breakpoint */
            debuglog(DEBUG_WARN, "cai_aarch64: BRK #%u at PC=%llx\n",
                     imm16, (unsigned long long)PC);
            return CAI_EILL;
        }
        if (opc == 2 && ll == 0) {
            /* HLT – halt */
            ctx->running = false;
            ctx->exit_code = 0;
            return CAI_EXITED;
        }
        return CAI_EILL;
    }

    /* ---- System instructions (NOP / MSR / MRS / HINT) ---- */
    if ((insn & 0xFFFFF01F) == 0xD503201F) {
        /* NOP and other hints */
        PC += 4;
        return CAI_OK;
    }
    /* MSR / MRS pstate (NZCV, DAIF, etc.) */
    if ((insn & 0xFFF8F01F) == 0xD5180000) {
        /* MSR <pstatefield>, Xt */
        unsigned op1 = BITS(insn, 18, 16);
        unsigned CRm = BITS(insn, 11,  8);
        unsigned op2 = BITS(insn,  7,  5);
        unsigned Rt  = BITS(insn,  4,  0);
        if (op1 == 0 && op2 == 4) { /* MSR NZCV */
            NZCV = xreg_rd(ctx, Rt) & (NZCV_N | NZCV_Z | NZCV_C | NZCV_V);
        }
        (void)CRm;
        PC += 4;
        return CAI_OK;
    }
    if ((insn & 0xFFF80000) == 0xD5300000) {
        /* MRS Xt, <sysreg> - simplified: just return NZCV when relevant */
        unsigned Rt = BITS(insn, 4, 0);
        xreg_wr(ctx, Rt, NZCV);
        PC += 4;
        return CAI_OK;
    }

    return CAI_EILL;
}

/* ------------------------------------------------------------------
 * Load / Store (all variants)
 * ------------------------------------------------------------------ */

/*
 * Helper to read a (possibly sign-extended) value of @size bytes from memory.
 * @sign: true → sign-extend to 64 bits.
 * @to64: true → store 64-bit result in Xd (not zero-extend to Wd only).
 */
static int load_helper(cai_context_t *ctx, uint64_t addr, unsigned size,
                       bool sign_ext, bool to64, unsigned Rt)
{
    uint64_t val = 0;
    int rc = CAI_OK;
    switch (size) {
    case 1:
        { uint8_t v = 0; rc = cai_mem_read8(ctx, addr, &v); val = v; } break;
    case 2:
        { uint16_t v = 0; rc = cai_mem_read16(ctx, addr, &v); val = v; } break;
    case 4:
        { uint32_t v = 0; rc = cai_mem_read32(ctx, addr, &v); val = v; } break;
    case 8:
        rc = cai_mem_read64(ctx, addr, &val); break;
    default: return CAI_EILL;
    }
    if (rc != CAI_OK) return rc;

    if (sign_ext && size < 8) {
        unsigned bits = size * 8;
        val = (uint64_t)sign_extend64(val, bits);
        if (!to64) val &= 0xFFFFFFFF; /* SXTW result in W register */
    }

    if (size == 8 || (sign_ext && to64))
        xreg_wr(ctx, Rt, val);
    else
        wreg_wr(ctx, Rt, (uint32_t)val);

    return CAI_OK;
}

static int store_helper(cai_context_t *ctx, uint64_t addr, unsigned size,
                        bool is64, unsigned Rt)
{
    switch (size) {
    case 1: return cai_mem_write8 (ctx, addr, (uint8_t) xreg_rd(ctx, Rt));
    case 2: return cai_mem_write16(ctx, addr, (uint16_t)xreg_rd(ctx, Rt));
    case 4: return cai_mem_write32(ctx, addr, (uint32_t)xreg_rd(ctx, Rt));
    case 8: return cai_mem_write64(ctx, addr, xreg_rd(ctx, Rt));
    default: (void)is64; return CAI_EILL;
    }
}

/*
 * Decode load/store instructions.
 *
 * The AArch64 load/store encoding space is large.  We handle the most common
 * sub-groups by checking bits [31:28] and [27:24].
 */
static int decode_ldst(cai_context_t *ctx, uint32_t insn)
{
    unsigned top4 = BITS(insn, 31, 28);
    unsigned g    = BITS(insn, 27, 24);

    /* ---- Load/Store register (unsigned offset)  [size|11100[01]x] ---- */
    /* Encoding: size[31:30] | V[26] | 111001 | opc[23:22] | imm12[21:10] | Rn[9:5] | Rt[4:0] */
    if ((g & 0xF) == 0x9 && BIT(insn, 24) == 0) {
        unsigned size  = BITS(insn, 31, 30); /* 0=B,1=H,2=W,3=X */
        unsigned V     = BIT(insn, 26);       /* SIMD/FP? (ignored) */
        unsigned opc   = BITS(insn, 23, 22);
        unsigned imm12 = BITS(insn, 21, 10);
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rt    = BITS(insn,  4,  0);
        (void)V;

        unsigned bytes = 1u << size; /* 1,2,4,8 */
        uint64_t base  = xreg_sp_rd(ctx, Rn);
        uint64_t addr  = base + (uint64_t)imm12 * bytes;

        /* opc: 00=STR, 01=LDR, 10=STR(32), 11=LDRSW etc. */
        /* Simplify: opc bit1=0 → STR, bit1=0 with opc=01 → LDR,
         *           signed loads encoded in opc bits */
        bool is_load  = (opc & 1) || (opc == 2 && size < 2);
        bool sign_ext = (opc >= 2) && !is_load ? false : (opc >= 2);
        /* Proper decode: */
        switch (opc) {
        case 0: /* STR */
            return store_helper(ctx, addr, bytes, (size == 3), Rt);
        case 1: /* LDR */
            return load_helper(ctx, addr, bytes, false, (size == 3), Rt);
        case 2: /* LDRSW (size=2) / STR (size=3→PRFM, ignore) */
            if (size == 2)
                return load_helper(ctx, addr, 4, true, true, Rt);
            else
                return store_helper(ctx, addr, bytes, true, Rt); /* PRFM / STR */
        case 3: /* LDRSH/LDRSB – sign extend to W or X */
            /* size: 0=LDRSB→X, 1=LDRSH→X, etc.  Bit[22] toggles W vs X target */
            return load_helper(ctx, addr, bytes, true,
                               /* to64= */ !(opc & 1) || size < 2, Rt);
        default: (void)sign_ext; (void)is_load; return CAI_EILL;
        }
    }

    /* ---- Load/Store register (register offset)  [size|111000011] ---- */
    if (BITS(insn, 24, 21) == 0x3 && BIT(insn, 11) == 1) {
        unsigned size  = BITS(insn, 31, 30);
        unsigned opc   = BITS(insn, 23, 22);
        unsigned Rm    = BITS(insn, 20, 16);
        unsigned opt   = BITS(insn, 15, 13);
        unsigned S     = BIT(insn, 12);
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rt    = BITS(insn,  4,  0);

        unsigned bytes = 1u << size;
        uint64_t base  = xreg_sp_rd(ctx, Rn);
        uint64_t offset = xreg_rd(ctx, Rm);

        /* Apply extend option (opt 010=UXTW, 011=LSL, 110=SXTW, 111=SXTX) */
        if (opt == 6) offset = (uint64_t)(int64_t)(int32_t)offset; /* SXTW */
        if (S)        offset <<= size; /* optional left shift by log2(bytes) */

        uint64_t addr = base + offset;
        switch (opc) {
        case 0: return store_helper(ctx, addr, bytes, (size == 3), Rt);
        case 1: return load_helper(ctx, addr, bytes, false, (size == 3), Rt);
        case 2: if (size == 2)
                    return load_helper(ctx, addr, 4, true, true, Rt);
                return store_helper(ctx, addr, bytes, true, Rt);
        case 3: return load_helper(ctx, addr, bytes, true, true, Rt);
        default: return CAI_EILL;
        }
    }

    /* ---- Load/Store register (pre-index / post-index / unscaled offset)
     *      [size|111000_0xx] where bits[10:9] distinguish variants ---- */
    if (BITS(insn, 27, 24) == 0x8 && BIT(insn, 25) == 0) {
        unsigned size  = BITS(insn, 31, 30);
        unsigned opc   = BITS(insn, 23, 22);
        int32_t  simm9 = (int32_t)sign_extend32(BITS(insn, 20, 12), 9);
        unsigned idx   = BITS(insn, 11, 10); /* 00=unscaled, 01=post, 11=pre */
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rt    = BITS(insn,  4,  0);

        unsigned bytes = 1u << size;
        uint64_t base  = xreg_sp_rd(ctx, Rn);
        uint64_t addr;

        switch (idx) {
        case 0: /* STUR / LDUR (unscaled offset) */
        case 2: /* Unscaled - same handling */
            addr = (uint64_t)((int64_t)base + simm9);
            break;
        case 1: /* Post-index: address = Rn, then Rn += simm9 */
            addr = base;
            xreg_sp_wr(ctx, Rn, (uint64_t)((int64_t)base + simm9));
            break;
        case 3: /* Pre-index: Rn += simm9, address = new Rn */
            base = (uint64_t)((int64_t)base + simm9);
            xreg_sp_wr(ctx, Rn, base);
            addr = base;
            break;
        default: return CAI_EILL;
        }

        switch (opc) {
        case 0: return store_helper(ctx, addr, bytes, (size == 3), Rt);
        case 1: return load_helper(ctx, addr, bytes, false, (size == 3), Rt);
        case 2: if (size == 2)
                    return load_helper(ctx, addr, 4, true, true, Rt);
                return store_helper(ctx, addr, bytes, true, Rt);
        case 3: return load_helper(ctx, addr, bytes, true, true, Rt);
        default: return CAI_EILL;
        }
    }

    /* ---- Load/Store pair  [opc|101_0[01][01]] ---- */
    /* Encoding bits [31:30]=opc, [29:27]=101, [26]=V(FP), [25:23]=index_mode */
    if (BITS(insn, 29, 27) == 0x5 && BIT(insn, 26) == 0) {
        unsigned opc   = BITS(insn, 31, 30); /* 00=32bit, 01=LDPSW, 10=64bit */
        unsigned L     = BIT(insn, 22);       /* 1=load, 0=store */
        unsigned idx   = BITS(insn, 24, 23);  /* 01=post, 10=offset, 11=pre */
        int32_t  imm7  = (int32_t)sign_extend32(BITS(insn, 21, 15), 7);
        unsigned Rt2   = BITS(insn, 14, 10);
        unsigned Rn    = BITS(insn,  9,  5);
        unsigned Rt    = BITS(insn,  4,  0);

        unsigned bytes = (opc == 2) ? 8 : 4;
        int64_t  offset = (int64_t)imm7 * (int64_t)bytes;
        uint64_t base   = xreg_sp_rd(ctx, Rn);
        uint64_t addr;

        switch (idx) {
        case 1: /* Post-index */
            addr = base;
            xreg_sp_wr(ctx, Rn, (uint64_t)((int64_t)base + offset));
            break;
        case 2: /* Signed offset */
            addr = (uint64_t)((int64_t)base + offset);
            break;
        case 3: /* Pre-index */
            base = (uint64_t)((int64_t)base + offset);
            xreg_sp_wr(ctx, Rn, base);
            addr = base;
            break;
        default: return CAI_EILL;
        }

        bool ldpsw = (opc == 1 && L);

        if (L) { /* Load pair */
            int rc;
            if (bytes == 8) {
                rc = load_helper(ctx, addr,          8, false, true, Rt);
                if (rc) return rc;
                rc = load_helper(ctx, addr + 8,      8, false, true, Rt2);
            } else if (ldpsw) {
                rc = load_helper(ctx, addr,          4, true, true, Rt);
                if (rc) return rc;
                rc = load_helper(ctx, addr + 4,      4, true, true, Rt2);
            } else {
                rc = load_helper(ctx, addr,          4, false, false, Rt);
                if (rc) return rc;
                rc = load_helper(ctx, addr + 4,      4, false, false, Rt2);
            }
            return rc;
        } else { /* Store pair */
            int rc;
            if (bytes == 8) {
                rc = store_helper(ctx, addr,     8, true, Rt);
                if (rc) return rc;
                rc = store_helper(ctx, addr + 8, 8, true, Rt2);
            } else {
                rc = store_helper(ctx, addr,     4, false, Rt);
                if (rc) return rc;
                rc = store_helper(ctx, addr + 4, 4, false, Rt2);
            }
            return rc;
        }
    }

    /* ---- LDR (literal) – PC-relative  [opc|011_000] ---- */
    if (BITS(insn, 27, 24) == 0x8 && BIT(insn, 25) == 1 && BIT(insn, 26) == 0) {
        unsigned opc   = BITS(insn, 31, 30);
        int64_t  imm19 = sign_extend64(BITS(insn, 23, 5), 19) * 4;
        unsigned Rt    = BITS(insn, 4, 0);
        uint64_t addr  = (uint64_t)((int64_t)PC + imm19);

        switch (opc) {
        case 0: return load_helper(ctx, addr, 4, false, false, Rt); /* LDR Wt */
        case 1: return load_helper(ctx, addr, 8, false, true,  Rt); /* LDR Xt */
        case 2: return load_helper(ctx, addr, 4, true,  true,  Rt); /* LDRSW  */
        case 3: /* PRFM – prefetch hint, ignore */ return CAI_OK;
        default: return CAI_EILL;
        }
    }

    (void)top4;
    return CAI_EILL;
}

/* =========================================================================
 * cai_aarch64_step - main entry point (cai_context_t* variant)
 *
 * Called by cai_step() in the framework dispatcher.
 * ========================================================================= */

int cai_aarch64_step(cai_context_t *ctx)
{
    /* Fetch instruction (little-endian 32-bit word) */
    uint32_t insn = 0;
    int rc = cai_mem_read32(ctx, PC, &insn);
    if (rc != CAI_OK) {
        debuglog(DEBUG_WARN,
                 "cai_aarch64: fetch fault at PC=%llx\n",
                 (unsigned long long)PC);
        return CAI_EFAULT;
    }

    /* Instruction group decode via top-level encoding tree.
     *
     * AArch64 instructions: bits[28:25] select the major group.
     *   0000 : unallocated
     *   0001 : unallocated
     *   0010 : SVE
     *   0011 : unallocated
     *   100x : data-processing (immediate)
     *   101x : branches / system
     *   x1x0 : loads / stores
     *   x101 : data-processing (register)
     *   x111 : data-processing (scalar FP / SIMD)
     */

    uint64_t saved_pc = PC;
    unsigned op1 = BITS(insn, 28, 25); /* 4-bit major group */
    int ret;

    switch (op1) {
    case 0x8:  /* 1000 – DP immediate */
    case 0x9:  /* 1001 – DP immediate */
        ret = decode_dp_imm(ctx, insn);
        break;

    case 0xA:  /* 1010 – branches / exception / system */
    case 0xB:  /* 1011 – branches / exception / system */
        /* decode_branch handles its own PC update (branch targets) */
        if (decode_branch(ctx, insn) == CAI_OK)
            return CAI_OK; /* PC already updated by branch decoder */
        ret = decode_branch(ctx, insn);
        if (ret == CAI_OK) return CAI_OK;
        break;

    case 0x4:  /* 0100 – load/store */
    case 0x6:  /* 0110 – load/store */
    case 0xC:  /* 1100 – load/store */
    case 0xE:  /* 1110 – load/store */
        ret = decode_ldst(ctx, insn);
        break;

    case 0x5:  /* 0101 – DP register */
    case 0xD:  /* 1101 – DP register */
        ret = decode_dp_register(ctx, insn);
        break;

    case 0x7:  /* 0111 – FP/SIMD (not fully implemented) */
    case 0xF:  /* 1111 – FP/SIMD */
        /* NOP on FP instructions to allow programs to run without FP */
        debuglog(DEBUG_DETAIL,
                 "cai_aarch64: FP/SIMD insn %08x at PC=%llx (NOP)\n",
                 insn, (unsigned long long)PC);
        ret = CAI_OK;
        break;

    default:
        ret = CAI_EILL;
        break;
    }

    if (ret == CAI_OK) {
        /* Advance PC only for non-branch instructions.
         * Branch decoders set PC directly; we detect that by checking
         * whether PC was already changed. */
        if (PC == saved_pc)
            PC += 4;
    } else if (ret == CAI_EILL) {
        debuglog(DEBUG_WARN,
                 "cai_aarch64: illegal insn %08x at PC=%llx op1=%u\n",
                 insn, (unsigned long long)saved_pc, op1);
    }

    return ret;
}

/* =========================================================================
 * Stand-alone context API (thin wrappers over the flat mem[] pool)
 *
 * These exist so the AArch64 interpreter can be used / unit-tested
 * independently of the full cai_context_t infrastructure.
 * ========================================================================= */

cai_aarch64_ctx_t *cai_aarch64_create(size_t mem_size)
{
    cai_aarch64_ctx_t *ctx = (cai_aarch64_ctx_t *)kmalloc(sizeof(*ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    ctx->mem = (uint8_t *)kmalloc(mem_size);
    if (!ctx->mem) {
        kfree(ctx);
        return NULL;
    }
    memset(ctx->mem, 0, mem_size);
    ctx->mem_size = mem_size;
    ctx->mem_base = 0;
    ctx->running  = true;
    return ctx;
}

void cai_aarch64_destroy(cai_aarch64_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->mem) kfree(ctx->mem);
    kfree(ctx);
}

/* Stand-alone memory helpers */
uint8_t cai_aa64_read8(cai_aarch64_ctx_t *ctx, uint64_t addr)
{
    uint64_t off = addr - ctx->mem_base;
    if (off >= ctx->mem_size) return 0;
    return ctx->mem[off];
}
uint16_t cai_aa64_read16(cai_aarch64_ctx_t *ctx, uint64_t addr)
{
    uint64_t off = addr - ctx->mem_base;
    if (off + 1 >= ctx->mem_size) return 0;
    return (uint16_t)ctx->mem[off] | ((uint16_t)ctx->mem[off + 1] << 8);
}
uint32_t cai_aa64_read32(cai_aarch64_ctx_t *ctx, uint64_t addr)
{
    uint64_t off = addr - ctx->mem_base;
    if (off + 3 >= ctx->mem_size) return 0;
    return (uint32_t)ctx->mem[off]         |
           ((uint32_t)ctx->mem[off + 1] << 8)  |
           ((uint32_t)ctx->mem[off + 2] << 16) |
           ((uint32_t)ctx->mem[off + 3] << 24);
}
uint64_t cai_aa64_read64(cai_aarch64_ctx_t *ctx, uint64_t addr)
{
    uint64_t off = addr - ctx->mem_base;
    if (off + 7 >= ctx->mem_size) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)ctx->mem[off + i] << (i * 8);
    return v;
}
void cai_aa64_write8(cai_aarch64_ctx_t *ctx, uint64_t addr, uint8_t v)
{
    uint64_t off = addr - ctx->mem_base;
    if (off < ctx->mem_size) ctx->mem[off] = v;
}
void cai_aa64_write16(cai_aarch64_ctx_t *ctx, uint64_t addr, uint16_t v)
{
    uint64_t off = addr - ctx->mem_base;
    if (off + 1 < ctx->mem_size) {
        ctx->mem[off]     = (uint8_t)v;
        ctx->mem[off + 1] = (uint8_t)(v >> 8);
    }
}
void cai_aa64_write32(cai_aarch64_ctx_t *ctx, uint64_t addr, uint32_t v)
{
    uint64_t off = addr - ctx->mem_base;
    if (off + 3 < ctx->mem_size) {
        ctx->mem[off]     = (uint8_t)v;
        ctx->mem[off + 1] = (uint8_t)(v >> 8);
        ctx->mem[off + 2] = (uint8_t)(v >> 16);
        ctx->mem[off + 3] = (uint8_t)(v >> 24);
    }
}
void cai_aa64_write64(cai_aarch64_ctx_t *ctx, uint64_t addr, uint64_t v)
{
    uint64_t off = addr - ctx->mem_base;
    if (off + 7 < ctx->mem_size) {
        for (int i = 0; i < 8; i++)
            ctx->mem[off + i] = (uint8_t)(v >> (i * 8));
    }
}

/* =========================================================================
 * Stand-alone step / run  (used when not going through cai_context_t)
 * =========================================================================
 *
 * We build a temporary cai_context_t on the stack that points into the
 * stand-alone context's flat memory pool.  This lets decode_dp_register,
 * decode_dp_imm, decode_ldst, and decode_branch all use the shared helpers
 * without duplication.
 */

static void sa_ctx_init(cai_context_t *full, cai_aarch64_ctx_t *sa)
{
    memset(full, 0, sizeof(*full));
    full->target_arch = CAI_ARCH_AARCH64;
    full->host_arch   = cai_host_arch();
    full->mem_base    = sa->mem;
    full->mem_size    = sa->mem_size;
    full->running     = sa->running;

    /* Copy registers */
    for (int i = 0; i < 31; i++)
        full->cpu.aarch64.x[i] = sa->regs.x[i];
    full->cpu.aarch64.sp   = sa->regs.sp;
    full->cpu.aarch64.pc   = sa->regs.pc;
    full->cpu.aarch64.nzcv = (uint32_t)sa->regs.nzcv;

    /* Register a single read/write region covering the whole pool */
    full->n_regions = 1;
    full->regions[0].gva_base  = sa->mem_base;
    full->regions[0].host_ptr  = sa->mem;
    full->regions[0].size      = sa->mem_size;
    full->regions[0].flags     = CAI_MEM_READ | CAI_MEM_WRITE | CAI_MEM_EXEC;
}

static void sa_ctx_sync(cai_aarch64_ctx_t *sa, cai_context_t *full)
{
    for (int i = 0; i < 31; i++)
        sa->regs.x[i] = full->cpu.aarch64.x[i];
    sa->regs.sp   = full->cpu.aarch64.sp;
    sa->regs.pc   = full->cpu.aarch64.pc;
    sa->regs.nzcv = full->cpu.aarch64.nzcv;
    sa->running   = full->running;
    sa->exit_code = full->exit_code;
}

/*
 * cai_aarch64_step_sa - standalone context wrapper
 * Builds a minimal cai_context_t, calls the main cai_aarch64_step(), then
 * copies mutated state back into the standalone context.
 */
int cai_aarch64_step_sa(cai_aarch64_ctx_t *sa)
{
    if (!sa || !sa->running) return CAI_EXITED;

    cai_context_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));

    ctx.target_arch = CAI_ARCH_AARCH64;
    ctx.host_arch   = cai_host_arch();
    ctx.running     = sa->running;
    ctx.exit_code   = sa->exit_code;

    /* Copy registers */
    for (int i = 0; i < 31; i++)
        ctx.cpu.aarch64.x[i] = sa->regs.x[i];
    ctx.cpu.aarch64.sp   = sa->regs.sp;
    ctx.cpu.aarch64.pc   = sa->regs.pc;
    ctx.cpu.aarch64.nzcv = (uint32_t)sa->regs.nzcv;
    ctx.pc = sa->regs.pc;

    /* Wire flat memory as a single region */
    ctx.mem_base  = sa->mem;
    ctx.mem_size  = sa->mem_size;
    ctx.n_regions = 1;
    ctx.regions[0].gva_base = sa->mem_base;
    ctx.regions[0].host_ptr = sa->mem;
    ctx.regions[0].size     = sa->mem_size;
    ctx.regions[0].flags    = CAI_MEM_READ | CAI_MEM_WRITE | CAI_MEM_EXEC;

    int rc = cai_aarch64_step(&ctx);

    /* Copy back */
    for (int i = 0; i < 31; i++)
        sa->regs.x[i] = ctx.cpu.aarch64.x[i];
    sa->regs.sp   = ctx.cpu.aarch64.sp;
    sa->regs.pc   = ctx.cpu.aarch64.pc;
    sa->regs.nzcv = ctx.cpu.aarch64.nzcv;
    sa->running   = ctx.running;
    sa->exit_code = ctx.exit_code;

    return rc;
}

int cai_aarch64_run(cai_aarch64_ctx_t *sa, int max_steps)
{
    int ret = CAI_OK;
    int steps = 0;
    while (sa->running) {
        ret = cai_aarch64_step_sa(sa);
        if (ret != CAI_OK) break;
        steps++;
        if (max_steps > 0 && steps >= max_steps) {
            ret = CAI_EAGAIN;
            break;
        }
    }
    return ret;
}

/* =========================================================================
 * Minimal ELF loader for the stand-alone context
 *
 * Parses an AArch64 ELF64 binary and copies PT_LOAD segments into the
 * flat memory pool, then sets the entry point.
 * ========================================================================= */

/* Minimal ELF64 header / program header structures (self-contained) */
#define CAI_ELFMAG0  0x7F
#define CAI_ELFMAG1  'E'
#define CAI_ELFMAG2  'L'
#define CAI_ELFMAG3  'F'
#define CAI_ET_EXEC  2
#define CAI_ET_DYN   3
#define CAI_PT_LOAD  1
#define CAI_EM_AA64  183

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} cai_elf64_hdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} cai_elf64_phdr_t;

int cai_aarch64_load_elf(cai_aarch64_ctx_t *ctx, const uint8_t *elf, size_t size)
{
    if (!ctx || !elf || size < sizeof(cai_elf64_hdr_t)) return CAI_EINVAL;

    const cai_elf64_hdr_t *hdr = (const cai_elf64_hdr_t *)elf;

    /* Validate magic */
    if (hdr->e_ident[0] != CAI_ELFMAG0 || hdr->e_ident[1] != CAI_ELFMAG1 ||
        hdr->e_ident[2] != CAI_ELFMAG2 || hdr->e_ident[3] != CAI_ELFMAG3)
        return CAI_EINVAL;

    /* Validate AArch64 */
    if (hdr->e_machine != CAI_EM_AA64)
        return CAI_EINVAL;

    /* Map PT_LOAD segments */
    uint64_t phoff = hdr->e_phoff;
    uint16_t phnum = hdr->e_phnum;
    uint16_t phentsz = hdr->e_phentsize;

    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t phdr_off = phoff + (uint64_t)i * phentsz;
        if (phdr_off + sizeof(cai_elf64_phdr_t) > size)
            return CAI_EINVAL;

        const cai_elf64_phdr_t *ph = (const cai_elf64_phdr_t *)(elf + phdr_off);
        if (ph->p_type != CAI_PT_LOAD) continue;

        /* Determine destination offset into flat pool */
        uint64_t dest_off = ph->p_vaddr - ctx->mem_base;
        if (dest_off >= ctx->mem_size || dest_off + ph->p_memsz > ctx->mem_size)
            return CAI_ENOMEM;

        /* Zero the memsz region (handles .bss) */
        memset(ctx->mem + dest_off, 0, (size_t)ph->p_memsz);

        /* Copy the file image */
        if (ph->p_filesz > 0) {
            if (ph->p_offset + ph->p_filesz > size) return CAI_EINVAL;
            memcpy(ctx->mem + dest_off, elf + ph->p_offset, (size_t)ph->p_filesz);
        }
    }

    /* Set entry point and start state */
    ctx->regs.pc = hdr->e_entry;
    ctx->running = true;
    return CAI_OK;
}

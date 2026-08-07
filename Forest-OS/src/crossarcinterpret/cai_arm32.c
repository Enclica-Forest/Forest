/*
 * cai_arm32.c - ARM32 (ARMv7-A) instruction interpreter
 *
 * Implements cai_arm32_step() which decodes and executes one ARM32 or Thumb
 * instruction per call, operating on the shared cai_context_t defined in
 * crossarcinterpret.h.
 *
 * Instruction set reference:
 *   ARM Architecture Reference Manual ARMv7-A and ARMv7-R edition
 *   ARM DDI 0406C.d  (publicly available from ARM)
 *
 * Encoding summary used here:
 *   ARM state   : 32-bit fixed-width instructions, little-endian
 *   Thumb state : 16-bit (T1/T2) and 32-bit (T3/T4) instructions
 *
 * Calling convention (Linux EABI, ARM IHI 0042F):
 *   syscall nr  : r7
 *   arguments   : r0-r6
 *   return      : r0
 *   trap insn   : SWI #0  (ARM) / SVC #0 (Thumb)
 *
 * Fern integration:
 *   Syscalls are forwarded to cai_syscall_dispatch(); the result is placed
 *   into r0 and execution resumes after the trap instruction.
 */

#include "crossarcinterpret.h"
#include "cai_arm32.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"
#include "../include/elf.h"

/* =========================================================================
 * Convenience aliases into cai_context_t
 * ========================================================================= */

/* r[n] where n is 0-15 */
#define R(ctx, n)   ((ctx)->cpu.arm32.r[(n)])
/* SP, LR, PC */
#define SP(ctx)     R(ctx, ARM_SP)
#define LR(ctx)     R(ctx, ARM_LR)
#define PC(ctx)     R(ctx, ARM_PC)
/* CPSR */
#define CPSR(ctx)   ((ctx)->cpu.arm32.cpsr)

/* Flag helpers */
#define GET_N(ctx)  (((CPSR(ctx)) >> 31) & 1u)
#define GET_Z(ctx)  (((CPSR(ctx)) >> 30) & 1u)
#define GET_C(ctx)  (((CPSR(ctx)) >> 29) & 1u)
#define GET_V(ctx)  (((CPSR(ctx)) >> 28) & 1u)

#define SET_N(ctx, v) do { if (v) CPSR(ctx) |= ARM_CPSR_N; else CPSR(ctx) &= ~ARM_CPSR_N; } while(0)
#define SET_Z(ctx, v) do { if (v) CPSR(ctx) |= ARM_CPSR_Z; else CPSR(ctx) &= ~ARM_CPSR_Z; } while(0)
#define SET_C(ctx, v) do { if (v) CPSR(ctx) |= ARM_CPSR_C; else CPSR(ctx) &= ~ARM_CPSR_C; } while(0)
#define SET_V(ctx, v) do { if (v) CPSR(ctx) |= ARM_CPSR_V; else CPSR(ctx) &= ~ARM_CPSR_V; } while(0)

/* =========================================================================
 * Condition code evaluation  (ARM DDI 0406C §A8.3)
 *
 * The top 4 bits of every ARM instruction [31:28] carry the condition field.
 * AL (0b1110) is always-execute; 0b1111 is used for unconditional
 * instructions in ARMv7.
 * ========================================================================= */

static bool arm_check_cond(cai_context_t *ctx, uint32_t cond)
{
    unsigned n = GET_N(ctx);
    unsigned z = GET_Z(ctx);
    unsigned c = GET_C(ctx);
    unsigned v = GET_V(ctx);

    switch (cond & 0xFu) {
    case  0: return  z;               /* EQ – equal / zero                  */
    case  1: return !z;               /* NE – not equal                     */
    case  2: return  c;               /* CS/HS – carry set / unsigned >=    */
    case  3: return !c;               /* CC/LO – carry clear / unsigned <   */
    case  4: return  n;               /* MI – minus / negative              */
    case  5: return !n;               /* PL – plus / positive or zero       */
    case  6: return  v;               /* VS – overflow                      */
    case  7: return !v;               /* VC – no overflow                   */
    case  8: return  c && !z;         /* HI – unsigned higher               */
    case  9: return !c ||  z;         /* LS – unsigned lower or same        */
    case 10: return (n == v);         /* GE – signed >=                     */
    case 11: return (n != v);         /* LT – signed <                      */
    case 12: return !z && (n == v);   /* GT – signed >                      */
    case 13: return  z || (n != v);   /* LE – signed <=                     */
    case 14: return true;             /* AL – always                        */
    case 15: return true;             /* (unconditional in ARMv7)           */
    default: return false;
    }
}

/* =========================================================================
 * Barrel shifter  (ARM DDI 0406C §A8.4.3)
 *
 * Computes  val SHIFT_TYPE amount  and updates *carry_out.
 * For register shifts the amount is masked to 8 bits; only the bottom 5
 * bits matter for LSL/LSR/ASR, and 6 bits for ROR (mod 32 effectively).
 * ========================================================================= */

typedef enum {
    SHIFT_LSL = 0,  /* Logical Shift Left         */
    SHIFT_LSR = 1,  /* Logical Shift Right        */
    SHIFT_ASR = 2,  /* Arithmetic Shift Right     */
    SHIFT_ROR = 3,  /* Rotate Right / RRX         */
} arm_shift_t;

static uint32_t arm_shift(uint32_t val, arm_shift_t type, uint32_t amount,
                          unsigned c_in, unsigned *carry_out)
{
    if (amount == 0) {
        *carry_out = c_in; /* Shift by 0: carry unchanged, value unchanged */
        return val;
    }

    switch (type) {
    case SHIFT_LSL:
        if (amount >= 32) {
            *carry_out = (amount == 32) ? (val & 1u) : 0u;
            return 0u;
        }
        *carry_out = (val >> (32u - amount)) & 1u;
        return val << amount;

    case SHIFT_LSR:
        if (amount >= 32) {
            *carry_out = (amount == 32) ? ((val >> 31) & 1u) : 0u;
            return 0u;
        }
        *carry_out = (val >> (amount - 1u)) & 1u;
        return val >> amount;

    case SHIFT_ASR: {
        int32_t sv = (int32_t)val;
        if (amount >= 32) {
            *carry_out = (sv < 0) ? 1u : 0u;
            return (sv < 0) ? 0xFFFFFFFFu : 0u;
        }
        *carry_out = (uint32_t)((sv >> (int)(amount - 1u)) & 1);
        return (uint32_t)(sv >> (int)amount);
    }

    case SHIFT_ROR:
        /* RRX (Rotate Right Extended) when amount==0 is handled by caller
         * passing amount==0, which is caught above.  When amount is set
         * but shift type is ROR, normalise to mod-32. */
        amount &= 31u;
        if (amount == 0) {
            /* RRX: shift right by 1 with carry-in */
            *carry_out = val & 1u;
            return (val >> 1u) | (c_in << 31u);
        }
        *carry_out = (val >> (amount - 1u)) & 1u;
        return (val >> amount) | (val << (32u - amount));
    }
    /* unreachable */
    *carry_out = 0;
    return val;
}

/* =========================================================================
 * Immediate operand with rotation  (ARM DDI 0406C §A5.2.4)
 *
 * Bits [11:8] = rotate_imm (×2), bits [7:0] = imm8.
 * The resulting immediate is ROR(ZeroExtend(imm8, 32), rotate_imm*2).
 * ========================================================================= */

static uint32_t arm_expand_imm(uint32_t imm12, unsigned c_in,
                                unsigned *carry_out)
{
    uint32_t imm8   = imm12 & 0xFFu;
    uint32_t rotate = ((imm12 >> 8) & 0xFu) * 2u;
    if (rotate == 0) {
        *carry_out = c_in;
        return imm8;
    }
    uint32_t result = (imm8 >> rotate) | (imm8 << (32u - rotate));
    *carry_out = (result >> 31) & 1u;
    return result;
}

/* =========================================================================
 * Flag update helpers for arithmetic operations
 * ========================================================================= */

/* Update N and Z flags from a result word */
static void update_nz(cai_context_t *ctx, uint32_t result)
{
    SET_N(ctx, (result >> 31) & 1u);
    SET_Z(ctx, result == 0u);
}

/* ADD carry/overflow logic.  Returns carry and sets *ov. */
static unsigned add_carry_overflow(uint32_t a, uint32_t b, uint32_t cin,
                                   unsigned *ov)
{
    uint64_t ua = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
    unsigned carry = (ua >> 32) & 1u;
    uint32_t result = (uint32_t)ua;
    /* Signed overflow: both operands same sign but result differs */
    *ov = (((~(a ^ b)) & (a ^ result)) >> 31) & 1u;
    return carry;
}

/* SUB  (a - b - borrow).  borrow = !carry_in under ARM convention. */
static unsigned sub_carry_overflow(uint32_t a, uint32_t b, uint32_t borrow_in,
                                   unsigned *ov)
{
    /* ARM: C flag for SUB represents NOT borrow */
    uint64_t ua = (uint64_t)a - (uint64_t)b - (uint64_t)borrow_in;
    /* Carry out: if no unsigned borrow occurred */
    unsigned carry = (a >= (uint64_t)b + borrow_in) ? 1u : 0u;
    uint32_t result = (uint32_t)ua;
    /* Overflow: operands differ in sign, result sign differs from minuend */
    *ov = (((a ^ b) & (a ^ result)) >> 31) & 1u;
    (void)carry; /* silence unused - caller uses separate carry calc */
    return ((uint64_t)a >= (uint64_t)b + (uint64_t)borrow_in) ? 1u : 0u;
}

/* =========================================================================
 * Data-processing operand 2 decode  (ARM DDI 0406C §A5.2)
 *
 * bit[25] distinguishes immediate (1) from register operand (0).
 * ========================================================================= */

/* Decode register-shifted operand from bits [11:0] of the instruction.
 * @insn_bits : the 12-bit operand field (bits 11:0 of the instruction word).
 * Returns the shifted value; updates *carry_out. */
static uint32_t decode_reg_operand(cai_context_t *ctx, uint32_t op2_bits,
                                   unsigned *carry_out)
{
    uint32_t rm          = op2_bits & 0xFu;
    uint32_t shift_type  = (op2_bits >> 5) & 0x3u;
    unsigned reg_shift   = (op2_bits >> 4) & 1u;  /* 1 = shift by reg     */
    uint32_t rm_val      = R(ctx, rm);
    unsigned c_in        = GET_C(ctx);

    uint32_t amount;
    if (reg_shift) {
        /* Shift amount in Rs[7:0]; Rs is bits [11:8] */
        uint32_t rs = (op2_bits >> 8) & 0xFu;
        amount = R(ctx, rs) & 0xFFu;
        /* When shift amount is 0: value and carry unchanged */
        if (amount == 0) {
            *carry_out = c_in;
            return rm_val;
        }
    } else {
        /* Immediate shift amount in bits [11:7] */
        amount = (op2_bits >> 7) & 0x1Fu;
        /* LSL #0 / LSR #0 / ASR #0 / ROR #0 have special semantics: */
        if (amount == 0) {
            if (shift_type == SHIFT_LSR || shift_type == SHIFT_ASR) {
                /* LSR #0 → LSR #32; ASR #0 → ASR #32 */
                amount = 32u;
            } else if (shift_type == SHIFT_ROR) {
                /* ROR #0 → RRX */
                /* RRX: shift right 1 through carry */
                *carry_out = rm_val & 1u;
                return (rm_val >> 1u) | (c_in << 31u);
            }
            /* LSL #0: value and carry unchanged */
            if (shift_type == SHIFT_LSL) {
                *carry_out = c_in;
                return rm_val;
            }
        }
    }

    return arm_shift(rm_val, (arm_shift_t)shift_type, amount, c_in,
                     carry_out);
}

/* =========================================================================
 * Data Processing instruction execution  (ARM DDI 0406C §A5.2, §A8)
 * ========================================================================= */

static int exec_data_proc(cai_context_t *ctx, uint32_t insn)
{
    unsigned s       = (insn >> 20) & 1u;  /* S bit: update condition codes */
    unsigned opcode  = (insn >> 21) & 0xFu;
    unsigned rn      = (insn >> 16) & 0xFu;
    unsigned rd      = (insn >> 12) & 0xFu;
    unsigned imm_op  = (insn >> 25) & 1u;  /* bit25: immediate operand       */

    uint32_t op1     = R(ctx, rn);
    uint32_t op2;
    unsigned carry_out = GET_C(ctx);  /* carry into flags update            */

    if (imm_op) {
        /* Immediate: bits[11:0] = rotate_imm:imm8 */
        op2 = arm_expand_imm(insn & 0xFFFu, GET_C(ctx), &carry_out);
    } else {
        /* Register / register-shifted register */
        op2 = decode_reg_operand(ctx, insn & 0xFFFu, &carry_out);
    }

    uint32_t result = 0;
    unsigned new_c  = carry_out;
    unsigned new_v  = GET_V(ctx);
    bool     write_rd = true;

    switch (opcode) {
    case  0: /* AND */
        result = op1 & op2;
        new_c  = carry_out;
        break;
    case  1: /* EOR (XOR) */
        result = op1 ^ op2;
        new_c  = carry_out;
        break;
    case  2: /* SUB  Rd = Rn - Op2 */
        result = op1 - op2;
        new_c  = sub_carry_overflow(op1, op2, 0u, &new_v);
        break;
    case  3: /* RSB  Rd = Op2 - Rn */
        result = op2 - op1;
        new_c  = sub_carry_overflow(op2, op1, 0u, &new_v);
        break;
    case  4: /* ADD  Rd = Rn + Op2 */
        result = op1 + op2;
        new_c  = add_carry_overflow(op1, op2, 0u, &new_v);
        break;
    case  5: /* ADC  Rd = Rn + Op2 + C */
        result = op1 + op2 + GET_C(ctx);
        new_c  = add_carry_overflow(op1, op2, GET_C(ctx), &new_v);
        break;
    case  6: /* SBC  Rd = Rn - Op2 - !C */
        result = op1 - op2 - (1u - GET_C(ctx));
        new_c  = sub_carry_overflow(op1, op2, 1u - GET_C(ctx), &new_v);
        break;
    case  7: /* RSC  Rd = Op2 - Rn - !C */
        result = op2 - op1 - (1u - GET_C(ctx));
        new_c  = sub_carry_overflow(op2, op1, 1u - GET_C(ctx), &new_v);
        break;
    case  8: /* TST  sets flags, no dest */
        result   = op1 & op2;
        new_c    = carry_out;
        write_rd = false;
        break;
    case  9: /* TEQ  sets flags, no dest */
        result   = op1 ^ op2;
        new_c    = carry_out;
        write_rd = false;
        break;
    case 10: /* CMP  sets flags, no dest */
        result   = op1 - op2;
        new_c    = sub_carry_overflow(op1, op2, 0u, &new_v);
        write_rd = false;
        break;
    case 11: /* CMN  sets flags, no dest */
        result   = op1 + op2;
        new_c    = add_carry_overflow(op1, op2, 0u, &new_v);
        write_rd = false;
        break;
    case 12: /* ORR */
        result = op1 | op2;
        new_c  = carry_out;
        break;
    case 13: /* MOV */
        result = op2;
        new_c  = carry_out;
        break;
    case 14: /* BIC  Rd = Rn & ~Op2 */
        result = op1 & ~op2;
        new_c  = carry_out;
        break;
    case 15: /* MVN  Rd = ~Op2 */
        result = ~op2;
        new_c  = carry_out;
        break;
    }

    /* Write destination register (if applicable) */
    if (write_rd) {
        if (rd == ARM_PC) {
            /* Writing to PC: branch.  If S bit set, restore CPSR from SPSR. */
            if (s) {
                /* MOVS/SUBS/etc PC: restore CPSR from SPSR (mode return) */
                CPSR(ctx) = ctx->cpu.arm32.spsr;
                ctx->cpu.arm32.thumb = !!(CPSR(ctx) & ARM_CPSR_T);
            }
            PC(ctx) = result & ~1u;  /* bit0 ignored for ARM PC writes      */
            ctx->pc  = PC(ctx);
            /* PC written: don't advance past instruction */
            return CAI_OK;
        }
        R(ctx, rd) = result;
    }

    /* Update condition flags */
    if (s) {
        if (rd == ARM_PC) {
            /* Already handled above (CPSR restore) */
        } else {
            update_nz(ctx, result);
            SET_C(ctx, new_c);
            SET_V(ctx, new_v);
        }
    }

    return CAI_OK;
}

/* =========================================================================
 * Multiply instructions  (ARM DDI 0406C §A5.2.5)
 *
 * Bits [27:24] = 0000  (MUL/MLA/UMULL/SMULL/UMLAL/SMLAL)
 * Bit  [7:4]   = 1001
 * ========================================================================= */

static int exec_multiply(cai_context_t *ctx, uint32_t insn)
{
    /* MUL / MLA / UMULL / UMLAL / SMULL / SMLAL */
    unsigned s     = (insn >> 20) & 1u;
    unsigned op    = (insn >> 21) & 0x7u; /* bits 23:21 */
    unsigned rd    = (insn >> 16) & 0xFu; /* accumulate dest / hi dest      */
    unsigned rn    = (insn >> 12) & 0xFu; /* addend / lo dest               */
    unsigned rs    = (insn >>  8) & 0xFu;
    unsigned rm    = (insn >>  0) & 0xFu;

    uint32_t rm_v  = R(ctx, rm);
    uint32_t rs_v  = R(ctx, rs);
    uint32_t rn_v  = R(ctx, rn);
    uint32_t rd_v  = R(ctx, rd);

    switch (op) {
    case 0: { /* MUL  Rd = Rm * Rs */
        uint32_t result = rm_v * rs_v;
        R(ctx, rd) = result;
        if (s) { update_nz(ctx, result); }
        break;
    }
    case 1: { /* MLA  Rd = Rm * Rs + Rn */
        uint32_t result = rm_v * rs_v + rn_v;
        R(ctx, rd) = result;
        if (s) { update_nz(ctx, result); }
        break;
    }
    case 4: { /* UMULL  RdHi:RdLo = Rm * Rs (unsigned) */
        uint64_t result = (uint64_t)rm_v * (uint64_t)rs_v;
        R(ctx, rn) = (uint32_t)(result & 0xFFFFFFFFu);  /* RdLo = Rn */
        R(ctx, rd) = (uint32_t)(result >> 32);           /* RdHi = Rd */
        if (s) { update_nz(ctx, (uint32_t)(result >> 32)); }
        break;
    }
    case 5: { /* UMLAL  RdHi:RdLo += Rm * Rs (unsigned) */
        uint64_t acc    = ((uint64_t)rd_v << 32) | (uint64_t)rn_v;
        uint64_t result = acc + (uint64_t)rm_v * (uint64_t)rs_v;
        R(ctx, rn) = (uint32_t)(result & 0xFFFFFFFFu);
        R(ctx, rd) = (uint32_t)(result >> 32);
        if (s) { update_nz(ctx, (uint32_t)(result >> 32)); }
        break;
    }
    case 6: { /* SMULL  RdHi:RdLo = Rm * Rs (signed) */
        int64_t  result = (int64_t)(int32_t)rm_v * (int64_t)(int32_t)rs_v;
        R(ctx, rn) = (uint32_t)((uint64_t)result & 0xFFFFFFFFu);
        R(ctx, rd) = (uint32_t)((uint64_t)result >> 32);
        if (s) { update_nz(ctx, (uint32_t)((uint64_t)result >> 32)); }
        break;
    }
    case 7: { /* SMLAL  RdHi:RdLo += Rm * Rs (signed) */
        int64_t  acc    = (int64_t)(((uint64_t)rd_v << 32) | (uint64_t)rn_v);
        int64_t  result = acc + (int64_t)(int32_t)rm_v * (int64_t)(int32_t)rs_v;
        R(ctx, rn) = (uint32_t)((uint64_t)result & 0xFFFFFFFFu);
        R(ctx, rd) = (uint32_t)((uint64_t)result >> 32);
        if (s) { update_nz(ctx, (uint32_t)((uint64_t)result >> 32)); }
        break;
    }
    default:
        debuglog(DEBUG_WARN, "cai_arm32: unimplemented multiply op=%u\n", op);
        return CAI_EILL;
    }
    return CAI_OK;
}

/* =========================================================================
 * Load / Store address mode computation  (ARM DDI 0406C §A5.3, §A5.4)
 * ========================================================================= */

/*
 * Compute the effective address for an LDR/STR instruction and optionally
 * perform the writeback update to Rn.
 *
 * Bit 24 = P (pre-index), bit 23 = U (add offset), bit 21 = W (writeback),
 * bit 25 = register offset.
 */
static uint32_t ldr_str_addr(cai_context_t *ctx, uint32_t insn,
                              uint32_t *wback_val, unsigned *do_wback,
                              unsigned rn)
{
    unsigned P    = (insn >> 24) & 1u;
    unsigned U    = (insn >> 23) & 1u;
    unsigned W    = (insn >> 21) & 1u;
    unsigned reg  = (insn >> 25) & 1u;  /* register offset if set          */

    uint32_t base = R(ctx, rn);
    uint32_t offset;
    unsigned dummy_carry = GET_C(ctx);

    if (reg) {
        /* Register offset, possibly shifted (bits[11:0]) */
        offset = decode_reg_operand(ctx, insn & 0xFFFu, &dummy_carry);
    } else {
        /* 12-bit immediate offset */
        offset = insn & 0xFFFu;
    }

    uint32_t addr_pre  = U ? (base + offset) : (base - offset);
    uint32_t addr      = P ? addr_pre : base;  /* pre or post index         */

    /* Writeback: either post-index (always) or pre-index with W=1 */
    *do_wback  = (!P) || W;
    *wback_val = addr_pre;  /* updated base value                            */

    return addr;
}

/* =========================================================================
 * Load / Store Word / Byte  (ARM DDI 0406C §A5.3)
 * ========================================================================= */

static int exec_ldr_str(cai_context_t *ctx, uint32_t insn)
{
    unsigned load = (insn >> 20) & 1u;  /* 1=LDR, 0=STR                    */
    unsigned byte = (insn >> 22) & 1u;  /* 1=byte, 0=word                  */
    unsigned rn   = (insn >> 16) & 0xFu;
    unsigned rd   = (insn >> 12) & 0xFu;

    uint32_t wback_val;
    unsigned do_wback;
    uint32_t addr = ldr_str_addr(ctx, insn, &wback_val, &do_wback, rn);

    int rc;
    if (load) {
        if (byte) {
            uint8_t val;
            rc = cai_mem_read8(ctx, (uint64_t)addr, &val);
            if (rc != CAI_OK) return rc;
            R(ctx, rd) = (uint32_t)val;
        } else {
            uint32_t val;
            rc = cai_mem_read32(ctx, (uint64_t)addr, &val);
            if (rc != CAI_OK) return rc;
            if (rd == ARM_PC) {
                /* LDR PC, [...] – branch to loaded value */
                PC(ctx) = val & ~1u;
                ctx->pc  = PC(ctx);
                if (do_wback && rn != rd)
                    R(ctx, rn) = wback_val;
                return CAI_OK;
            }
            R(ctx, rd) = val;
        }
    } else {
        /* STR / STRB */
        uint32_t val = R(ctx, rd);
        if (rd == ARM_PC) val = PC(ctx) + 4u;  /* STR PC stores PC+12 in ARM */
        if (byte) {
            rc = cai_mem_write8(ctx, (uint64_t)addr, (uint8_t)(val & 0xFFu));
        } else {
            rc = cai_mem_write32(ctx, (uint64_t)addr, val);
        }
        if (rc != CAI_OK) return rc;
    }

    /* Writeback */
    if (do_wback)
        R(ctx, rn) = wback_val;

    return CAI_OK;
}

/* =========================================================================
 * Load/Store Halfword and Signed Byte  (ARM DDI 0406C §A5.4)
 *
 * Encoding: bits[27:25]=000, bit[7]=1, bit[4]=1  (extra load/store)
 * ========================================================================= */

static int exec_ldr_str_halfword(cai_context_t *ctx, uint32_t insn)
{
    unsigned load = (insn >> 20) & 1u;
    unsigned rn   = (insn >> 16) & 0xFu;
    unsigned rd   = (insn >> 12) & 0xFu;
    unsigned sh   = (insn >>  5) & 0x3u;  /* 01=UH, 10=SB, 11=SH           */
    unsigned imm  = (insn >> 22) & 1u;    /* 1=immediate offset             */
    unsigned P    = (insn >> 24) & 1u;
    unsigned U    = (insn >> 23) & 1u;
    unsigned W    = (insn >> 21) & 1u;

    uint32_t base   = R(ctx, rn);
    uint32_t offset;
    if (imm) {
        /* Immediate: imm8 = bits[11:8] | bits[3:0] */
        offset = ((insn >> 4) & 0xF0u) | (insn & 0xFu);
    } else {
        /* Register offset: bits[3:0] */
        unsigned rm = insn & 0xFu;
        offset = R(ctx, rm);
    }

    uint32_t addr_pre = U ? (base + offset) : (base - offset);
    uint32_t addr     = P ? addr_pre : base;
    unsigned do_wback = (!P) || W;

    int rc = CAI_OK;
    if (load) {
        switch (sh) {
        case 1: { /* LDRH – unsigned halfword */
            uint16_t v;
            rc = cai_mem_read16(ctx, (uint64_t)addr, &v);
            if (rc == CAI_OK) R(ctx, rd) = (uint32_t)v;
            break;
        }
        case 2: { /* LDRSB – signed byte */
            uint8_t v;
            rc = cai_mem_read8(ctx, (uint64_t)addr, &v);
            if (rc == CAI_OK) R(ctx, rd) = (uint32_t)(int32_t)(int8_t)v;
            break;
        }
        case 3: { /* LDRSH – signed halfword */
            uint16_t v;
            rc = cai_mem_read16(ctx, (uint64_t)addr, &v);
            if (rc == CAI_OK) R(ctx, rd) = (uint32_t)(int32_t)(int16_t)v;
            break;
        }
        default:
            return CAI_EILL;
        }
    } else {
        /* STRH – store unsigned halfword */
        rc = cai_mem_write16(ctx, (uint64_t)addr, (uint16_t)(R(ctx, rd) & 0xFFFFu));
    }

    if (rc != CAI_OK) return rc;
    if (do_wback) R(ctx, rn) = addr_pre;
    return CAI_OK;
}

/* =========================================================================
 * Block Load / Store  (LDM/STM)  (ARM DDI 0406C §A5.7)
 *
 * Modes:
 *   bits [24:23]  P U
 *     0 0  DA  – decrement after  (LDMDA / STMDA)
 *     0 1  IA  – increment after  (LDMIA / STMIA)  ← LDMFD / STMEA
 *     1 0  DB  – decrement before (LDMDB / STMDB)  ← LDMEA / STMFD (push)
 *     1 1  IB  – increment before (LDMIB / STMIB)
 *
 * Bit 21 = W (writeback), bit 22 = S (user regs / PSR restore, not used in
 * user-mode emulation), bit 20 = L (1=load, 0=store).
 * ========================================================================= */

static int exec_ldm_stm(cai_context_t *ctx, uint32_t insn)
{
    unsigned P      = (insn >> 24) & 1u;
    unsigned U      = (insn >> 23) & 1u;
    unsigned W      = (insn >> 21) & 1u;
    unsigned load   = (insn >> 20) & 1u;
    unsigned rn     = (insn >> 16) & 0xFu;
    uint16_t reglist = (uint16_t)(insn & 0xFFFFu);

    /* Count number of set bits to compute total transfer size */
    unsigned count = 0;
    for (unsigned i = 0; i < 16; i++)
        if (reglist & (1u << i)) count++;

    uint32_t base = R(ctx, rn);
    uint32_t start_addr;

    /* Compute starting address per addressing mode */
    if (!P && U) {
        start_addr = base;                    /* IA: start = base            */
    } else if (P && U) {
        start_addr = base + 4u;               /* IB: start = base + 4       */
    } else if (!P && !U) {
        start_addr = base - (count * 4u);     /* DA: start = base - n*4     */
    } else {
        start_addr = base - (count * 4u);     /* DB: start = base - n*4     */
    }

    uint32_t addr = start_addr;
    bool     pc_loaded = false;

    for (unsigned i = 0; i < 16; i++) {
        if (!(reglist & (1u << i))) continue;

        int rc;
        if (load) {
            uint32_t val;
            rc = cai_mem_read32(ctx, (uint64_t)addr, &val);
            if (rc != CAI_OK) return rc;
            if (i == ARM_PC) {
                PC(ctx) = val & ~1u;
                ctx->pc  = PC(ctx);
                pc_loaded = true;
            } else {
                R(ctx, i) = val;
            }
        } else {
            uint32_t val = R(ctx, i);
            if (i == ARM_PC) val = PC(ctx) + 4u;  /* STM stores PC+12       */
            rc = cai_mem_write32(ctx, (uint64_t)addr, val);
            if (rc != CAI_OK) return rc;
        }
        addr += 4u;
    }

    /* Writeback: update Rn with new base */
    if (W) {
        if (U) {
            R(ctx, rn) = base + (count * 4u);
        } else {
            R(ctx, rn) = base - (count * 4u);
        }
    }

    /* If LDM loaded PC, suppress normal PC advance */
    if (pc_loaded) return CAI_OK;
    return CAI_OK;
}

/* =========================================================================
 * Branch instructions  (ARM DDI 0406C §A8.8)
 * ========================================================================= */

static int exec_branch(cai_context_t *ctx, uint32_t insn, uint32_t pc_fetch)
{
    unsigned link = (insn >> 24) & 1u;  /* BL if set */

    /* 24-bit signed offset, shifted left by 2, relative to PC+8 */
    int32_t  offset24 = (int32_t)(insn & 0x00FFFFFFu);
    if (offset24 & 0x00800000u)  /* sign extend from bit 23 */
        offset24 |= (int32_t)0xFF000000u;
    int32_t  offset = offset24 << 2;

    /* ARM: fetch address of branch is PC; pipeline adds 8 */
    uint32_t target = (uint32_t)((int32_t)(pc_fetch + 8u) + offset);

    if (link) {
        LR(ctx) = pc_fetch + 4u;  /* Return address = instruction after branch */
    }

    PC(ctx)  = target & ~1u;
    ctx->pc  = PC(ctx);
    return CAI_OK;
}

/* =========================================================================
 * Branch and Exchange  (BX)  (ARM DDI 0406C §A8.8.15)
 * BX / BLX (register)
 * ========================================================================= */

static int exec_bx(cai_context_t *ctx, uint32_t insn, uint32_t pc_fetch)
{
    unsigned rm   = insn & 0xFu;
    unsigned link = (insn >> 5) & 1u;  /* BLX has bit 5 set in the encoding */

    if (link) {
        LR(ctx) = (pc_fetch + 4u) | 1u;  /* Return addr with Thumb bit hint  */
    }

    uint32_t target = R(ctx, rm);
    if (target & 1u) {
        /* Switch to Thumb state */
        CPSR(ctx) |= ARM_CPSR_T;
        ctx->cpu.arm32.thumb = true;
    } else {
        CPSR(ctx) &= ~ARM_CPSR_T;
        ctx->cpu.arm32.thumb = false;
    }
    PC(ctx)  = target & ~1u;
    ctx->pc  = PC(ctx);
    return CAI_OK;
}

/* =========================================================================
 * Miscellaneous instructions (MRS/MSR, CLZ, BKPT, etc.)
 * ========================================================================= */

static int exec_misc(cai_context_t *ctx, uint32_t insn, uint32_t pc_fetch)
{
    (void)pc_fetch;
    unsigned op2 = (insn >> 4) & 0xFu;

    /* MRS – Move to Register from Status register */
    if ((insn & 0x0FBF0FFFu) == 0x010F0000u) {
        unsigned rd = (insn >> 12) & 0xFu;
        unsigned R  = (insn >> 22) & 1u;  /* 0=CPSR, 1=SPSR */
        R(ctx, rd)  = R ? ctx->cpu.arm32.spsr : CPSR(ctx);
        return CAI_OK;
    }

    /* MSR – Move to Status register from Register/Immediate */
    if ((insn & 0x0FB00000u) == 0x03200000u || /* immediate form */
        (insn & 0x0FB0F000u) == 0x0120F000u)   /* register form  */ {
        /* For user-mode emulation we only support writing condition flags. */
        unsigned use_spsr = (insn >> 22) & 1u;
        unsigned imm_flag = (insn >> 25) & 1u;
        uint32_t mask_field = (insn >> 16) & 0xFu;
        uint32_t val;

        if (imm_flag) {
            unsigned carry_dummy = GET_C(ctx);
            val = arm_expand_imm(insn & 0xFFFu, GET_C(ctx), &carry_dummy);
        } else {
            unsigned rm = insn & 0xFu;
            val = R(ctx, rm);
        }

        uint32_t byte_mask = 0u;
        if (mask_field & 1u) byte_mask |= 0x000000FFu;
        if (mask_field & 2u) byte_mask |= 0x0000FF00u;
        if (mask_field & 4u) byte_mask |= 0x00FF0000u;
        if (mask_field & 8u) byte_mask |= 0xFF000000u;

        if (use_spsr) {
            ctx->cpu.arm32.spsr = (ctx->cpu.arm32.spsr & ~byte_mask) | (val & byte_mask);
        } else {
            CPSR(ctx) = (CPSR(ctx) & ~byte_mask) | (val & byte_mask);
            ctx->cpu.arm32.thumb = !!(CPSR(ctx) & ARM_CPSR_T);
        }
        return CAI_OK;
    }

    /* CLZ – Count Leading Zeros */
    if ((insn & 0x0FFF0FF0u) == 0x016F0F10u) {
        unsigned rd = (insn >> 12) & 0xFu;
        unsigned rm = insn & 0xFu;
        uint32_t v  = R(ctx, rm);
        uint32_t n  = 0u;
        if (v == 0) { R(ctx, rd) = 32u; return CAI_OK; }
        if (!(v & 0xFFFF0000u)) { n += 16u; v <<= 16; }
        if (!(v & 0xFF000000u)) { n +=  8u; v <<= 8;  }
        if (!(v & 0xF0000000u)) { n +=  4u; v <<= 4;  }
        if (!(v & 0xC0000000u)) { n +=  2u; v <<= 2;  }
        if (!(v & 0x80000000u)) { n +=  1u; }
        R(ctx, rd) = n;
        return CAI_OK;
    }

    /* BX/BLX Rm – handled by op2 check */
    if (op2 == 1u && (insn & 0x0FFFFFF0u) == 0x012FFF10u) {
        return exec_bx(ctx, insn, pc_fetch);
    }
    if (op2 == 3u && (insn & 0x0FFFFFF0u) == 0x012FFF30u) {
        /* BLX Rm */
        return exec_bx(ctx, insn | (1u << 5), pc_fetch);
    }

    debuglog(DEBUG_WARN, "cai_arm32: unimplemented misc insn %08x\n", insn);
    return CAI_EILL;
}

/* =========================================================================
 * Software Interrupt / SWI  (ARM DDI 0406C §A8.8.221)
 * Transfers control to the Fern syscall bridge.
 * ========================================================================= */

static int exec_swi(cai_context_t *ctx, uint32_t insn)
{
    (void)insn;  /* imm24 is conventionally 0 for Linux; nr is in r7 */

    int64_t result = cai_syscall_dispatch(ctx, ctx->cpu.arm32.r[7]);

    /* Write result into r0 */
    ctx->cpu.arm32.r[0] = (uint32_t)(int32_t)result;

    /* Check if exit was requested */
    if (!ctx->running)
        return CAI_EXITED;

    return CAI_OK;
}

/* =========================================================================
 * Saturating add/subtract  (QADD, QSUB, etc.)  (ARMv5E / DSP extension)
 * ========================================================================= */

static int exec_sat_addsubq(cai_context_t *ctx, uint32_t insn)
{
    unsigned op  = (insn >> 21) & 0x3u;
    unsigned rn  = (insn >> 16) & 0xFu;
    unsigned rd  = (insn >> 12) & 0xFu;
    unsigned rm  = insn & 0xFu;

    int32_t rm_v = (int32_t)R(ctx, rm);
    int32_t rn_v = (int32_t)R(ctx, rn);
    int32_t result;

    /* Saturating limits */
    const int64_t MAX32 =  (int64_t)0x7FFFFFFF;
    const int64_t MIN32 = -(int64_t)0x80000000;

    auto int64_t saturate(int64_t v);
    /* Helper: clamp v to signed 32-bit range and set Q on saturation */
#define SAT32(v)  ({ int64_t _v = (v); \
    if (_v > MAX32) { _v = MAX32; CPSR(ctx) |= ARM_CPSR_Q; } \
    else if (_v < MIN32) { _v = MIN32; CPSR(ctx) |= ARM_CPSR_Q; } \
    (int32_t)_v; })

    switch (op) {
    case 0: result = SAT32((int64_t)rm_v + (int64_t)rn_v); break;  /* QADD  */
    case 1: result = SAT32((int64_t)rm_v - (int64_t)rn_v); break;  /* QSUB  */
    case 2: /* QDADD: Rd = Saturate(Rm + Saturate(2*Rn)) */
        { int32_t t = SAT32((int64_t)rn_v * 2); result = SAT32((int64_t)rm_v + t); break; }
    case 3: /* QDSUB: Rd = Saturate(Rm - Saturate(2*Rn)) */
        { int32_t t = SAT32((int64_t)rn_v * 2); result = SAT32((int64_t)rm_v - t); break; }
    default: return CAI_EILL;
    }
#undef SAT32
    R(ctx, rd) = (uint32_t)result;
    return CAI_OK;
}

/* =========================================================================
 * Move Immediate 16-bit  (MOVW / MOVT)  (ARMv7, §A8.8.102/103)
 *
 * Encoding: cond 0011 0xx0 imm4 Rd imm12
 *   MOVW: bits[27:20] = 0011 0000 (0x30), sets lower 16 bits zero-extended
 *   MOVT: bits[27:20] = 0011 0100 (0x34), sets upper 16 bits, lower preserved
 * ========================================================================= */

static int exec_movw_movt(cai_context_t *ctx, uint32_t insn)
{
    unsigned rd    = (insn >> 12) & 0xFu;
    /* imm16 = bits[19:16] | bits[11:0] */
    uint32_t imm16 = ((insn >> 4) & 0xF000u) | (insn & 0xFFFu);
    unsigned top   = (insn >> 22) & 1u;  /* 0=MOVW, 1=MOVT */

    if (top) {
        R(ctx, rd) = (R(ctx, rd) & 0x0000FFFFu) | (imm16 << 16);
    } else {
        R(ctx, rd) = imm16;              /* MOVW zero-extends              */
    }
    return CAI_OK;
}

/* =========================================================================
 * Coprocessor instructions (stubs – we only care about VFP/NEON traps)
 * ========================================================================= */

static int exec_coprocessor(cai_context_t *ctx, uint32_t insn)
{
    (void)ctx; (void)insn;
    /* In user-mode emulation, coprocessor instructions other than SWI are
     * undefined.  Return CAI_EILL so the caller can signal SIGILL. */
    debuglog(DEBUG_WARN, "cai_arm32: coprocessor insn %08x ignored\n", insn);
    return CAI_EILL;
}

/* =========================================================================
 * THUMB-16 instruction decoder  (ARMv7-M §A6, Thumb T1/T2 encoding)
 *
 * The PC for Thumb instructions points to the current 16-bit instruction.
 * The pipeline-visible PC = instruction_address + 4 (two 16-bit stages).
 * ========================================================================= */

/* Helper: sign extend N-bit value */
static inline int32_t sign_ext(uint32_t v, unsigned bits)
{
    uint32_t mask = 1u << (bits - 1u);
    return (int32_t)((v ^ mask) - mask);
}

/* Update PC from Thumb step (advance by 2) */
#define THUMB_ADVANCE(ctx, pc_before) do { \
    PC(ctx)  = (pc_before) + 2u; \
    ctx->pc  = PC(ctx); \
} while (0)

static int exec_thumb16(cai_context_t *ctx, uint16_t insn, uint32_t pc_insn)
{
    uint32_t pc_vis = pc_insn + 4u;  /* Pipeline-visible PC for branches    */

    /* ------------------------------------------------------------------ */
    /* A6.2 – 16-bit Thumb instruction encoding                           */
    /* Decode based on bits[15:10]                                        */
    /* ------------------------------------------------------------------ */

    uint32_t op = (insn >> 10) & 0x3Fu;

    /* ---- Shift / Add / Sub / Move / Compare (bits[15:14]=00) ---- */
    if (op < 0x10u) {
        unsigned inner = (insn >> 11) & 0x7u;
        switch (inner) {
        case 0: case 1: case 2: case 3: {
            /* LSL / LSR / ASR immediate */
            arm_shift_t stype = (arm_shift_t)(inner);
            unsigned rd    = insn & 0x7u;
            unsigned rm    = (insn >> 3) & 0x7u;
            unsigned imm5  = (insn >> 6) & 0x1Fu;
            unsigned c_out = GET_C(ctx);
            uint32_t val   = arm_shift(R(ctx, rm), stype, imm5 ? imm5 : 0u, c_out, &c_out);
            /* LSL/LSR/ASR #0 → amount 0 (handled: shift by 0 returns original) */
            if (imm5 == 0 && stype != SHIFT_LSL) {
                /* LSR #0 / ASR #0 encode a 32-bit shift */
                val = arm_shift(R(ctx, rm), stype, 32u, c_out, &c_out);
            }
            R(ctx, rd) = val;
            update_nz(ctx, val);
            SET_C(ctx, c_out);
            break;
        }
        case 4: { /* ADD register Rd = Rn + Rm */
            unsigned rd = insn & 0x7u;
            unsigned rn = (insn >> 3) & 0x7u;
            unsigned rm = (insn >> 6) & 0x7u;
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rn) + R(ctx, rm);
            nc = add_carry_overflow(R(ctx, rn), R(ctx, rm), 0u, &ov);
            R(ctx, rd) = res;
            update_nz(ctx, res);
            SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        case 5: { /* SUB register Rd = Rn - Rm */
            unsigned rd = insn & 0x7u;
            unsigned rn = (insn >> 3) & 0x7u;
            unsigned rm = (insn >> 6) & 0x7u;
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rn) - R(ctx, rm);
            nc = sub_carry_overflow(R(ctx, rn), R(ctx, rm), 0u, &ov);
            R(ctx, rd) = res;
            update_nz(ctx, res);
            SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        case 6: { /* ADD immediate 3-bit */
            unsigned rd  = insn & 0x7u;
            unsigned rn  = (insn >> 3) & 0x7u;
            unsigned imm = (insn >> 6) & 0x7u;
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rn) + imm;
            nc = add_carry_overflow(R(ctx, rn), imm, 0u, &ov);
            R(ctx, rd) = res;
            update_nz(ctx, res);
            SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        case 7: { /* SUB immediate 3-bit */
            unsigned rd  = insn & 0x7u;
            unsigned rn  = (insn >> 3) & 0x7u;
            unsigned imm = (insn >> 6) & 0x7u;
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rn) - imm;
            nc = sub_carry_overflow(R(ctx, rn), imm, 0u, &ov);
            R(ctx, rd) = res;
            update_nz(ctx, res);
            SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        }
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- MOV / CMP / ADD / SUB immediate 8-bit (bits[15:13]=001) ---- */
    if ((op >> 2) == 4u || (op >> 2) == 5u || (op >> 2) == 6u || (op >> 2) == 7u) {
        unsigned inner2 = (insn >> 11) & 0x3u;
        unsigned rdn    = (insn >> 8) & 0x7u;
        uint32_t imm8   = insn & 0xFFu;
        switch (inner2) {
        case 0: { /* MOV imm8 */
            R(ctx, rdn) = imm8;
            update_nz(ctx, imm8);
            break;
        }
        case 1: { /* CMP imm8 */
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rdn) - imm8;
            nc = sub_carry_overflow(R(ctx, rdn), imm8, 0u, &ov);
            update_nz(ctx, res); SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        case 2: { /* ADD imm8 */
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rdn) + imm8;
            nc = add_carry_overflow(R(ctx, rdn), imm8, 0u, &ov);
            R(ctx, rdn) = res;
            update_nz(ctx, res); SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        case 3: { /* SUB imm8 */
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rdn) - imm8;
            nc = sub_carry_overflow(R(ctx, rdn), imm8, 0u, &ov);
            R(ctx, rdn) = res;
            update_nz(ctx, res); SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        }
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- Data processing / Special (bits[15:10]=010000-010001) ---- */
    if (op == 0x10u) {
        /* ALU operations: bits[9:6] select the operation */
        unsigned aluop = (insn >> 6) & 0xFu;
        unsigned rn    = (insn >> 3) & 0x7u;
        unsigned rd    = insn & 0x7u;
        uint32_t a = R(ctx, rd);
        uint32_t b = R(ctx, rn);
        uint32_t res;
        unsigned nc = GET_C(ctx), ov = GET_V(ctx);

        switch (aluop) {
        case  0: res = a & b; nc = GET_C(ctx); SET_Z(ctx, !res); SET_N(ctx, res>>31); SET_C(ctx,nc); break; /* AND */
        case  1: res = a ^ b; nc = GET_C(ctx); update_nz(ctx, res); SET_C(ctx,nc); break; /* EOR */
        case  2: /* LSL reg */
            { unsigned c_o = GET_C(ctx); res = arm_shift(a, SHIFT_LSL, b & 0xFFu, c_o, &c_o);
              R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,c_o); THUMB_ADVANCE(ctx, pc_insn); return CAI_OK; }
        case  3: /* LSR reg */
            { unsigned c_o = GET_C(ctx); res = arm_shift(a, SHIFT_LSR, b & 0xFFu, c_o, &c_o);
              R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,c_o); THUMB_ADVANCE(ctx, pc_insn); return CAI_OK; }
        case  4: /* ASR reg */
            { unsigned c_o = GET_C(ctx); res = arm_shift(a, SHIFT_ASR, b & 0xFFu, c_o, &c_o);
              R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,c_o); THUMB_ADVANCE(ctx, pc_insn); return CAI_OK; }
        case  5: /* ADC */
            res = a + b + GET_C(ctx); nc = add_carry_overflow(a, b, GET_C(ctx), &ov);
            R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,nc); SET_V(ctx,ov); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case  6: /* SBC */
            res = a - b - (1u-GET_C(ctx)); nc = sub_carry_overflow(a, b, 1u-GET_C(ctx), &ov);
            R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,nc); SET_V(ctx,ov); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case  7: /* ROR reg */
            { unsigned c_o = GET_C(ctx); res = arm_shift(a, SHIFT_ROR, b & 0xFFu, c_o, &c_o);
              R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,c_o); THUMB_ADVANCE(ctx, pc_insn); return CAI_OK; }
        case  8: /* TST */
            res = a & b; update_nz(ctx, res); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case  9: /* NEG / RSB #0 */
            res = 0u - b; nc = sub_carry_overflow(0u, b, 0u, &ov);
            R(ctx,rd) = res; update_nz(ctx,res); SET_C(ctx,nc); SET_V(ctx,ov); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case 10: /* CMP */
            res = a - b; nc = sub_carry_overflow(a, b, 0u, &ov);
            update_nz(ctx,res); SET_C(ctx,nc); SET_V(ctx,ov); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case 11: /* CMN */
            res = a + b; nc = add_carry_overflow(a, b, 0u, &ov);
            update_nz(ctx,res); SET_C(ctx,nc); SET_V(ctx,ov); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case 12: res = a | b;  update_nz(ctx, res); break; /* ORR */
        case 13: /* MUL */
            res = a * b; update_nz(ctx, res); R(ctx,rd) = res; THUMB_ADVANCE(ctx,pc_insn); return CAI_OK;
        case 14: res = a & ~b; update_nz(ctx, res); break; /* BIC */
        case 15: res = ~b;     update_nz(ctx, res); break; /* MVN */
        default: res = 0;
        }
        R(ctx, rd) = res;
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- Special data / BX (bits[15:10]=010001) ---- */
    if (op == 0x11u) {
        unsigned inner3 = (insn >> 8) & 0x3u;
        switch (inner3) {
        case 0: { /* ADD high registers */
            unsigned dn  = ((insn >> 7) & 1u) << 3;
            unsigned rd  = dn | (insn & 0x7u);
            unsigned rm  = (insn >> 3) & 0xFu;
            R(ctx, rd) += R(ctx, rm);
            if (rd == ARM_PC) { ctx->pc = PC(ctx); THUMB_ADVANCE(ctx, pc_insn); return CAI_OK; }
            break;
        }
        case 1: { /* CMP high registers */
            unsigned dn = ((insn >> 7) & 1u) << 3;
            unsigned rn = dn | (insn & 0x7u);
            unsigned rm = (insn >> 3) & 0xFu;
            unsigned ov; unsigned nc;
            uint32_t res = R(ctx, rn) - R(ctx, rm);
            nc = sub_carry_overflow(R(ctx, rn), R(ctx, rm), 0u, &ov);
            update_nz(ctx, res); SET_C(ctx, nc); SET_V(ctx, ov);
            break;
        }
        case 2: { /* MOV high registers */
            unsigned dn = ((insn >> 7) & 1u) << 3;
            unsigned rd = dn | (insn & 0x7u);
            unsigned rm = (insn >> 3) & 0xFu;
            R(ctx, rd) = R(ctx, rm);
            if (rd == ARM_PC) { PC(ctx) = R(ctx, rm) & ~1u; ctx->pc = PC(ctx); THUMB_ADVANCE(ctx,pc_insn); return CAI_OK; }
            break;
        }
        case 3: { /* BX / BLX */
            unsigned rm   = (insn >> 3) & 0xFu;
            unsigned link = (insn >> 7) & 1u;  /* bit 7: BLX */
            if (link) LR(ctx) = (pc_insn + 2u) | 1u;
            uint32_t target = R(ctx, rm);
            if (target & 1u) {
                CPSR(ctx) |= ARM_CPSR_T;
                ctx->cpu.arm32.thumb = true;
            } else {
                CPSR(ctx) &= ~ARM_CPSR_T;
                ctx->cpu.arm32.thumb = false;
            }
            PC(ctx) = target & ~1u;
            ctx->pc = PC(ctx);
            /* No advance – PC set explicitly */
            return CAI_OK;
        }
        }
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- LDR literal pool (bits[15:11]=01001) ---- */
    if ((insn >> 11) == 0x9u) {
        unsigned rd    = (insn >> 8) & 0x7u;
        uint32_t imm8  = (insn & 0xFFu) << 2;
        /* PC for load = word-aligned PC_vis */
        uint32_t addr  = (pc_vis & ~3u) + imm8;
        uint32_t val;
        int rc = cai_mem_read32(ctx, (uint64_t)addr, &val);
        if (rc != CAI_OK) return rc;
        R(ctx, rd) = val;
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- Load/Store register (bits[15:12]=0101) ---- */
    if ((insn >> 12) == 0x5u) {
        unsigned opB   = (insn >> 9) & 0x7u;
        unsigned rm    = (insn >> 6) & 0x7u;
        unsigned rn    = (insn >> 3) & 0x7u;
        unsigned rd    = insn & 0x7u;
        uint32_t addr  = R(ctx, rn) + R(ctx, rm);
        int rc;
        switch (opB) {
        case 0: rc = cai_mem_write32(ctx, addr, R(ctx, rd)); break; /* STR  */
        case 1: rc = cai_mem_write16(ctx, addr, (uint16_t)R(ctx, rd)); break; /* STRH */
        case 2: rc = cai_mem_write8 (ctx, addr, (uint8_t)R(ctx, rd)); break; /* STRB */
        case 3: { /* LDRSB */
            uint8_t v; rc = cai_mem_read8(ctx, addr, &v);
            if (rc == CAI_OK) R(ctx,rd) = (uint32_t)(int32_t)(int8_t)v; break; }
        case 4: { /* LDR */
            uint32_t v; rc = cai_mem_read32(ctx, addr, &v);
            if (rc == CAI_OK) R(ctx,rd) = v; break; }
        case 5: { /* LDRH */
            uint16_t v; rc = cai_mem_read16(ctx, addr, &v);
            if (rc == CAI_OK) R(ctx,rd) = v; break; }
        case 6: { /* LDRB */
            uint8_t v; rc = cai_mem_read8(ctx, addr, &v);
            if (rc == CAI_OK) R(ctx,rd) = v; break; }
        case 7: { /* LDRSH */
            uint16_t v; rc = cai_mem_read16(ctx, addr, &v);
            if (rc == CAI_OK) R(ctx,rd) = (uint32_t)(int32_t)(int16_t)v; break; }
        default: rc = CAI_EILL;
        }
        if (rc != CAI_OK) return rc;
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- Load/Store immediate (bits[15:13]=011 or 100) ---- */
    if ((insn >> 13) == 0x3u || (insn >> 13) == 0x4u) {
        unsigned opC  = (insn >> 11) & 0x3u;  /* for 011 family */
        unsigned isH  = (insn >> 13) == 0x4u;  /* halfword family? */
        unsigned rd   = insn & 0x7u;
        unsigned rn   = (insn >> 3) & 0x7u;
        int rc;

        if (!isH) {
            unsigned imm5 = (insn >> 6) & 0x1Fu;
            switch (opC) {
            case 0: { /* STR imm5 */
                uint32_t addr = R(ctx, rn) + (imm5 << 2);
                rc = cai_mem_write32(ctx, addr, R(ctx, rd)); break; }
            case 1: { /* LDR imm5 */
                uint32_t addr = R(ctx, rn) + (imm5 << 2);
                uint32_t v; rc = cai_mem_read32(ctx, addr, &v);
                if (rc == CAI_OK) R(ctx,rd) = v; break; }
            case 2: { /* STRB imm5 */
                uint32_t addr = R(ctx, rn) + imm5;
                rc = cai_mem_write8(ctx, addr, (uint8_t)R(ctx, rd)); break; }
            case 3: { /* LDRB imm5 */
                uint32_t addr = R(ctx, rn) + imm5;
                uint8_t v; rc = cai_mem_read8(ctx, addr, &v);
                if (rc == CAI_OK) R(ctx,rd) = v; break; }
            default: rc = CAI_EILL;
            }
        } else {
            /* STRH / LDRH imm5 (bits[15:13]=100) */
            unsigned load2 = (insn >> 11) & 1u;
            unsigned imm5h = (insn >> 6) & 0x1Fu;
            uint32_t addr  = R(ctx, rn) + (imm5h << 1);
            if (load2) {
                uint16_t v; rc = cai_mem_read16(ctx, addr, &v);
                if (rc == CAI_OK) R(ctx,rd) = v;
            } else {
                rc = cai_mem_write16(ctx, addr, (uint16_t)R(ctx, rd));
            }
        }
        if (rc != CAI_OK) return rc;
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- SP-relative load/store (bits[15:12]=1001) ---- */
    if ((insn >> 12) == 0x9u) {
        unsigned load2 = (insn >> 11) & 1u;
        unsigned rd    = (insn >> 8) & 0x7u;
        uint32_t imm8  = (insn & 0xFFu) << 2;
        uint32_t addr  = SP(ctx) + imm8;
        int rc;
        if (load2) {
            uint32_t v; rc = cai_mem_read32(ctx, addr, &v);
            if (rc == CAI_OK) R(ctx, rd) = v;
        } else {
            rc = cai_mem_write32(ctx, addr, R(ctx, rd));
        }
        if (rc != CAI_OK) return rc;
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- ADD SP or PC relative (bits[15:12]=1010) ---- */
    if ((insn >> 12) == 0xAu) {
        unsigned sp_flag = (insn >> 11) & 1u;
        unsigned rd      = (insn >> 8) & 0x7u;
        uint32_t imm8    = (insn & 0xFFu) << 2;
        uint32_t base    = sp_flag ? SP(ctx) : (pc_vis & ~3u);
        R(ctx, rd) = base + imm8;
        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- Misc 16-bit (bits[15:12]=1011) ---- */
    if ((insn >> 12) == 0xBu) {
        unsigned inner4 = (insn >> 8) & 0xFu;

        /* PUSH */
        if ((insn & 0xFE00u) == 0xB400u) {
            unsigned reglist  = insn & 0xFFu;
            unsigned lr_flag  = (insn >> 8) & 1u;
            unsigned count    = 0;
            for (unsigned i = 0; i < 8; i++) if (reglist & (1u<<i)) count++;
            if (lr_flag) count++;
            SP(ctx) -= count * 4u;
            uint32_t addr = SP(ctx);
            for (unsigned i = 0; i < 8; i++) {
                if (reglist & (1u << i)) {
                    cai_mem_write32(ctx, addr, R(ctx, i));
                    addr += 4u;
                }
            }
            if (lr_flag) {
                cai_mem_write32(ctx, addr, LR(ctx));
            }
            THUMB_ADVANCE(ctx, pc_insn);
            return CAI_OK;
        }

        /* POP */
        if ((insn & 0xFE00u) == 0xBC00u) {
            unsigned reglist  = insn & 0xFFu;
            unsigned pc_flag  = (insn >> 8) & 1u;
            uint32_t addr     = SP(ctx);
            unsigned count    = 0;
            for (unsigned i = 0; i < 8; i++) if (reglist & (1u<<i)) count++;
            if (pc_flag) count++;
            for (unsigned i = 0; i < 8; i++) {
                if (reglist & (1u << i)) {
                    uint32_t v; cai_mem_read32(ctx, addr, &v);
                    R(ctx, i) = v; addr += 4u;
                }
            }
            if (pc_flag) {
                uint32_t target; cai_mem_read32(ctx, addr, &target);
                addr += 4u;
                SP(ctx) = addr;
                if (target & 1u) {
                    CPSR(ctx) |= ARM_CPSR_T; ctx->cpu.arm32.thumb = true;
                } else {
                    CPSR(ctx) &= ~ARM_CPSR_T; ctx->cpu.arm32.thumb = false;
                }
                PC(ctx) = target & ~1u;
                ctx->pc = PC(ctx);
                return CAI_OK;
            }
            SP(ctx) = addr;
            THUMB_ADVANCE(ctx, pc_insn);
            return CAI_OK;
        }

        /* ADD/SUB SP imm7 */
        if ((insn & 0xFF00u) == 0xB000u) {
            unsigned sub = (insn >> 7) & 1u;
            uint32_t imm = (insn & 0x7Fu) << 2;
            SP(ctx) = sub ? SP(ctx) - imm : SP(ctx) + imm;
            THUMB_ADVANCE(ctx, pc_insn);
            return CAI_OK;
        }

        /* BKPT */
        if ((insn & 0xFF00u) == 0xBE00u) {
            debuglog(DEBUG_WARN, "cai_arm32: BKPT %u\n", insn & 0xFFu);
            THUMB_ADVANCE(ctx, pc_insn);
            return CAI_OK;
        }

        (void)inner4;
        debuglog(DEBUG_WARN, "cai_arm32: unimplemented Thumb misc %04x\n", insn);
        return CAI_EILL;
    }

    /* ---- LDM / STM (bits[15:12]=1100) ---- */
    if ((insn >> 12) == 0xCu) {
        unsigned load2   = (insn >> 11) & 1u;
        unsigned rn      = (insn >> 8) & 0x7u;
        unsigned reglist = insn & 0xFFu;
        uint32_t addr    = R(ctx, rn);
        unsigned count   = 0;
        for (unsigned i = 0; i < 8; i++) if (reglist & (1u<<i)) count++;

        for (unsigned i = 0; i < 8; i++) {
            if (!(reglist & (1u << i))) continue;
            if (load2) {
                uint32_t v; cai_mem_read32(ctx, addr, &v);
                R(ctx, i) = v;
            } else {
                cai_mem_write32(ctx, addr, R(ctx, i));
            }
            addr += 4u;
        }
        /* Writeback: for LDM, writeback if Rn not in reglist; STM always */
        if (!load2 || !(reglist & (1u << rn)))
            R(ctx, rn) = addr;

        THUMB_ADVANCE(ctx, pc_insn);
        return CAI_OK;
    }

    /* ---- Conditional branch / SVC (bits[15:12]=1101) ---- */
    if ((insn >> 12) == 0xDu) {
        unsigned cond2 = (insn >> 8) & 0xFu;
        if (cond2 == 0xEu) {
            /* UDF – permanently undefined */
            return CAI_EILL;
        }
        if (cond2 == 0xFu) {
            /* SVC #imm8 */
            int64_t result = cai_syscall_dispatch(ctx, ctx->cpu.arm32.r[7]);
            ctx->cpu.arm32.r[0] = (uint32_t)(int32_t)result;
            if (!ctx->running) return CAI_EXITED;
            THUMB_ADVANCE(ctx, pc_insn);
            return CAI_OK;
        }
        /* Conditional branch: imm8 signed, PC += 4 + SignExt(imm8)<<1 */
        if (arm_check_cond(ctx, cond2)) {
            int32_t off = sign_ext(insn & 0xFFu, 8) << 1;
            PC(ctx)  = (uint32_t)((int32_t)pc_vis + off);
            ctx->pc  = PC(ctx);
        } else {
            THUMB_ADVANCE(ctx, pc_insn);
        }
        return CAI_OK;
    }

    /* ---- Unconditional branch (bits[15:11]=11100) ---- */
    if ((insn >> 11) == 0x1Cu) {
        int32_t off = sign_ext(insn & 0x7FFu, 11) << 1;
        PC(ctx)  = (uint32_t)((int32_t)pc_vis + off);
        ctx->pc  = PC(ctx);
        return CAI_OK;
    }

    /* ---- BL / BLX prefix (Thumb 32-bit – upper half, bits[15:11]=11111) ---- */
    /* This case is handled by the caller when it detects a 32-bit Thumb word  */

    debuglog(DEBUG_WARN, "cai_arm32: unimplemented Thumb16 insn %04x\n",
             (unsigned)insn);
    return CAI_EILL;
}

/* =========================================================================
 * Thumb-32 (T3/T4) instruction decoder
 *
 * Two consecutive 16-bit halfwords; first halfword (hw1) has bits[15:11]
 * in {11101, 11110, 11111}.
 * ========================================================================= */

static int exec_thumb32(cai_context_t *ctx, uint16_t hw1, uint16_t hw2,
                        uint32_t pc_insn)
{
    uint32_t insn32 = ((uint32_t)hw1 << 16) | (uint32_t)hw2;
    uint32_t pc_vis = pc_insn + 4u;  /* pipeline-visible PC                 */

    unsigned op1 = (hw1 >> 11) & 0x3u;  /* bits [12:11] of hw1 -> op1     */
    unsigned op2 = (hw1 >>  4) & 0x7Fu; /* bits [10:4]  of hw1 -> op2     */

    /* Branch and Miscellaneous (op1 == 10, bit28 set) */
    if ((hw1 & 0xF800u) == 0xF000u && (hw2 & 0x8000u)) {
        /* BL / BLX immediate */
        unsigned blt = (hw2 >> 12) & 0x5u;  /* bits [14,12] of hw2 */
        if ((hw2 & 0xD000u) == 0xD000u) {
            /* BL  (T1): 0b11110 S imm10  0b11 J1 1 J2 imm11 */
            unsigned S   = (hw1 >> 10) & 1u;
            unsigned J1  = (hw2 >> 13) & 1u;
            unsigned J2  = (hw2 >> 11) & 1u;
            unsigned I1  = !(J1 ^ S);
            unsigned I2  = !(J2 ^ S);
            uint32_t imm10 = hw1 & 0x3FFu;
            uint32_t imm11 = hw2 & 0x7FFu;
            int32_t off = (int32_t)(
                (S   << 24) |
                (I1  << 23) |
                (I2  << 22) |
                (imm10 << 12) |
                (imm11 << 1));
            if (S) off |= (int32_t)0xFE000000u;  /* sign extend from bit 24 */
            LR(ctx) = (pc_insn + 4u) | 1u;       /* Return address with T bit */
            PC(ctx) = (uint32_t)((int32_t)pc_vis + off) & ~1u;
            ctx->pc  = PC(ctx);
            return CAI_OK;
        }
        if ((hw2 & 0xD000u) == 0xC000u) {
            /* BLX immediate (T2): switches to ARM */
            unsigned S   = (hw1 >> 10) & 1u;
            unsigned J1  = (hw2 >> 13) & 1u;
            unsigned J2  = (hw2 >> 11) & 1u;
            unsigned I1  = !(J1 ^ S);
            unsigned I2  = !(J2 ^ S);
            uint32_t imm10H = hw1 & 0x3FFu;
            uint32_t imm10L = (hw2 >> 1) & 0x3FFu;
            int32_t off = (int32_t)(
                (S    << 24) |
                (I1   << 23) |
                (I2   << 22) |
                (imm10H << 12) |
                (imm10L << 2));
            if (S) off |= (int32_t)0xFE000000u;
            LR(ctx) = (pc_insn + 4u) | 1u;
            /* Switch to ARM mode */
            CPSR(ctx) &= ~ARM_CPSR_T;
            ctx->cpu.arm32.thumb = false;
            PC(ctx) = (uint32_t)((int32_t)(pc_vis & ~3u) + off);
            ctx->pc  = PC(ctx);
            return CAI_OK;
        }
        (void)blt; (void)op2;
        debuglog(DEBUG_WARN, "cai_arm32: unimplemented Thumb32 branch %08x\n",
                 insn32);
        return CAI_EILL;
    }

    /* Thumb-32 LDR literal (bits[27:20] = 1111 1000 / 1111 1001) */
    if ((insn32 & 0xFF7F0000u) == 0xF85F0000u) {
        unsigned rd  = (insn32 >> 12) & 0xFu;
        unsigned U   = (insn32 >> 23) & 1u;
        uint32_t imm = insn32 & 0xFFFu;
        uint32_t addr = (pc_vis & ~3u);
        addr = U ? (addr + imm) : (addr - imm);
        uint32_t v; int rc = cai_mem_read32(ctx, addr, &v);
        if (rc != CAI_OK) return rc;
        R(ctx, rd) = v;
        PC(ctx) = pc_insn + 4u; ctx->pc = PC(ctx);
        return CAI_OK;
    }

    /* Thumb-32 MOV immediate (MOVW T3) bits[31:20] = 1111 0x10 0100 */
    if ((insn32 & 0xFBF08000u) == 0xF2400000u) {
        unsigned rd   = (insn32 >> 8) & 0xFu;
        uint32_t imm  = ((insn32 >> 4) & 0xF000u) |  /* imm4 */
                        ((insn32 >> 15) & 0x0800u) |  /* i */
                        ((insn32 >> 4) & 0x0700u) |   /* imm3 */
                        (insn32 & 0xFFu);              /* imm8 */
        unsigned top  = (insn32 >> 23) & 1u;          /* MOVT: bit23=1       */
        if (top) {
            R(ctx, rd) = (R(ctx, rd) & 0x0000FFFFu) | (imm << 16);
        } else {
            R(ctx, rd) = imm;
        }
        PC(ctx) = pc_insn + 4u; ctx->pc = PC(ctx);
        return CAI_OK;
    }

    /* Thumb-32 data processing – we forward unsupported as EILL */
    debuglog(DEBUG_WARN, "cai_arm32: unimplemented Thumb32 insn %08x\n",
             insn32);
    return CAI_EILL;
}

/* =========================================================================
 * Main ARM32 step function  (cai_context_t version)
 *
 * Called by cai_step() in the main interpreter loop.
 * ========================================================================= */

int cai_arm32_step(cai_context_t *ctx)
{
    if (!ctx->running)
        return CAI_EXITED;

    /* ------------------------------------------------------------------ */
    /* Check Thumb vs ARM state                                            */
    /* ------------------------------------------------------------------ */

    if (ctx->cpu.arm32.thumb || (ctx->cpu.arm32.cpsr & ARM_CPSR_T)) {
        /* ---- Thumb mode ---- */
        uint32_t pc_insn = PC(ctx);
        uint16_t hw1;
        int rc = cai_mem_read16(ctx, (uint64_t)pc_insn, &hw1);
        if (rc != CAI_OK) return rc;

        /* Detect 32-bit Thumb instruction: hw1 bits[15:11] in {11101,11110,11111} */
        if ((hw1 & 0xE000u) == 0xE000u && (hw1 & 0x1800u)) {
            uint16_t hw2;
            rc = cai_mem_read16(ctx, (uint64_t)(pc_insn + 2u), &hw2);
            if (rc != CAI_OK) return rc;
            PC(ctx)  = pc_insn + 4u;
            ctx->pc  = PC(ctx);
            return exec_thumb32(ctx, hw1, hw2, pc_insn);
        }

        /* 16-bit Thumb instruction */
        return exec_thumb16(ctx, hw1, pc_insn);

    } else {
        /* ---- ARM (32-bit) mode ---- */
        uint32_t pc_insn = PC(ctx);
        uint32_t insn;
        int rc = cai_mem_read32(ctx, (uint64_t)pc_insn, &insn);
        if (rc != CAI_OK) return rc;

        /* Advance PC before execution (ARM prefetch model) */
        PC(ctx)  = pc_insn + 4u;
        ctx->pc  = PC(ctx);

        /* Check condition field [31:28] */
        unsigned cond = (insn >> 28) & 0xFu;
        if (!arm_check_cond(ctx, cond)) {
            /* Condition false: skip instruction, PC already advanced */
            return CAI_OK;
        }

        /* Decode bits[27:25] for the primary instruction class */
        unsigned b27_25 = (insn >> 25) & 0x7u;
        unsigned b27_26 = (insn >> 26) & 0x3u;
        unsigned b24_20 = (insn >> 20) & 0x1Fu;

        /* ---- Unconditional / ARMv7 new encodings (cond=0b1111) ---- */
        if (cond == 0xFu) {
            /* BLX immediate (T5 encoding for ARM): bits[27:25] = 101 */
            if ((insn & 0xFE000000u) == 0xFA000000u) {
                /* BLX imm: similar to BL but H bit shifts target by 2 */
                unsigned H     = (insn >> 24) & 1u;
                int32_t  off24 = (int32_t)(insn & 0x00FFFFFFu);
                if (off24 & 0x00800000u) off24 |= (int32_t)0xFF000000u;
                int32_t  off   = (off24 << 2) | (H << 1);
                LR(ctx) = pc_insn + 4u;
                /* Switch to Thumb */
                CPSR(ctx) |= ARM_CPSR_T;
                ctx->cpu.arm32.thumb = true;
                PC(ctx) = (uint32_t)((int32_t)(pc_insn + 8u) + off);
                ctx->pc = PC(ctx);
                return CAI_OK;
            }
            /* Other unconditional instructions – treat as NOP for now */
            return CAI_OK;
        }

        /* ---- SWI / SVC  (bits[27:24] = 1111) ---- */
        if ((insn & 0x0F000000u) == 0x0F000000u) {
            return exec_swi(ctx, insn);
        }

        /* ---- Branch  (bits[27:25] = 101) ---- */
        if (b27_25 == 0x5u) {
            return exec_branch(ctx, insn, pc_insn);
        }

        /* ---- Coprocessor  (bits[27:26] = 11) ---- */
        if (b27_26 == 0x3u) {
            return exec_coprocessor(ctx, insn);
        }

        /* ---- Load/Store multiple  (bits[27:25] = 100) ---- */
        if (b27_25 == 0x4u) {
            return exec_ldm_stm(ctx, insn);
        }

        /* ---- Load/Store word/byte  (bits[27:26] = 01) ---- */
        if (b27_26 == 0x1u) {
            return exec_ldr_str(ctx, insn);
        }

        /* ---- bits[27:26] = 00 ---- */
        if (b27_26 == 0x0u) {
            unsigned bit25 = (insn >> 25) & 1u;
            unsigned bit7  = (insn >>  7) & 1u;
            unsigned bit4  = (insn >>  4) & 1u;

            /* Multiply / multiply-accumulate:
             *   bits[27:24]=0000, bits[7:4]=1001 */
            if ((insn & 0x0FC000F0u) == 0x00000090u) {
                return exec_multiply(ctx, insn);
            }
            /* Long multiply:
             *   bits[27:24]=0000, bits[23:22]!=00, bits[7:4]=1001 */
            if ((insn & 0x0F0000F0u) == 0x00800090u) {
                return exec_multiply(ctx, insn);
            }

            /* Extra load/store (halfword/signed byte):
             *   bit25=0, bit7=1, bit4=1 */
            if (!bit25 && bit7 && bit4 &&
                (insn & 0x00000060u) != 0x00000000u) {
                /* Check bits[6:5] != 00 (those are swap/semaphore) */
                return exec_ldr_str_halfword(ctx, insn);
            }

            /* Swap / SWP:  bits[27:23]=00010, bits[11:4]=00001001 */
            if ((insn & 0x0FB00FF0u) == 0x01000090u) {
                /* SWP / SWPB – single memory swap */
                unsigned byte   = (insn >> 22) & 1u;
                unsigned rn     = (insn >> 16) & 0xFu;
                unsigned rd     = (insn >> 12) & 0xFu;
                unsigned rm_swp = insn & 0xFu;
                uint32_t addr   = R(ctx, rn);
                if (byte) {
                    uint8_t old;
                    rc = cai_mem_read8(ctx, addr, &old);
                    if (rc != CAI_OK) return rc;
                    rc = cai_mem_write8(ctx, addr, (uint8_t)R(ctx, rm_swp));
                    if (rc != CAI_OK) return rc;
                    R(ctx, rd) = old;
                } else {
                    uint32_t old;
                    rc = cai_mem_read32(ctx, addr, &old);
                    if (rc != CAI_OK) return rc;
                    rc = cai_mem_write32(ctx, addr, R(ctx, rm_swp));
                    if (rc != CAI_OK) return rc;
                    R(ctx, rd) = old;
                }
                return CAI_OK;
            }

            /* MOVW / MOVT (bits[27:23] = 00110, 00110) */
            if ((insn & 0x0FB00000u) == 0x03000000u) {
                return exec_movw_movt(ctx, insn);
            }

            /* Saturating instructions (QADD/QSUB etc):
             *   bits[27:24]=0001, bits[7:4]=0101 */
            if ((insn & 0x0F900090u) == 0x01000050u) {
                return exec_sat_addsubq(ctx, insn);
            }

            /* Miscellaneous (MRS/MSR/BX/CLZ/BKPT):
             *   bit25=0, bit24=1, S=0 and bit[4]=0 or 1 depending on sub-op */
            if (!bit25 && (b24_20 & 0x19u) == 0x10u) {
                return exec_misc(ctx, insn, pc_insn);
            }

            /* Data processing (catch-all for 00 class) */
            return exec_data_proc(ctx, insn);
        }

        /* Unknown / unimplemented */
        debuglog(DEBUG_WARN,
                 "cai_arm32: unimplemented insn %08x at PC=%08x\n",
                 insn, pc_insn);
        return CAI_EILL;
    }
}

/* =========================================================================
 * Standalone cai_arm32_ctx_t API
 *
 * A thin shim that wraps the standalone arm32_ctx into a temporary
 * cai_context_t so the main step function can be reused.
 * ========================================================================= */

cai_arm32_ctx_t *cai_arm32_create(size_t mem_size)
{
    cai_arm32_ctx_t *ctx = (cai_arm32_ctx_t *)kzalloc(sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->mem = (uint8_t *)kzalloc(mem_size);
    if (!ctx->mem) {
        kfree(ctx);
        return NULL;
    }
    ctx->mem_size = mem_size;
    ctx->mem_base = 0u;
    ctx->running  = false;
    ctx->thumb_mode = false;
    ctx->exit_code  = 0;
    return ctx;
}

void cai_arm32_destroy(cai_arm32_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->mem) kfree(ctx->mem);
    kfree(ctx);
}

/*
 * Assemble a temporary cai_context_t from a standalone cai_arm32_ctx_t so
 * cai_arm32_step() can operate on it.  After the step, copy the mutated
 * state back out.
 */
int cai_arm32_step_sa(cai_arm32_ctx_t *sctx)
{
    /* Build a minimal cai_context_t on the stack */
    cai_context_t ctx;
    __builtin_memset(&ctx, 0, sizeof(ctx));

    ctx.target_arch = CAI_ARCH_ARM32;
    ctx.host_arch   = cai_host_arch();
    ctx.running     = sctx->running;
    ctx.exit_code   = sctx->exit_code;

    /* Copy registers */
    for (int i = 0; i < 16; i++)
        ctx.cpu.arm32.r[i] = sctx->regs.r[i];
    ctx.cpu.arm32.cpsr  = sctx->regs.cpsr;
    ctx.cpu.arm32.thumb = sctx->thumb_mode;

    /* Wire up a single read+write+exec region covering the flat memory */
    ctx.mem_base   = sctx->mem;
    ctx.mem_size   = sctx->mem_size;
    ctx.n_regions  = 1;
    ctx.regions[0].gva_base  = (uint64_t)sctx->mem_base;
    ctx.regions[0].host_ptr  = sctx->mem;
    ctx.regions[0].size      = sctx->mem_size;
    ctx.regions[0].flags     = CAI_MEM_READ | CAI_MEM_WRITE | CAI_MEM_EXEC;
    ctx.pc = ctx.cpu.arm32.r[ARM_PC];

    int rc = cai_arm32_step(&ctx);

    /* Copy state back */
    for (int i = 0; i < 16; i++)
        sctx->regs.r[i] = ctx.cpu.arm32.r[i];
    sctx->regs.cpsr   = ctx.cpu.arm32.cpsr;
    sctx->thumb_mode  = ctx.cpu.arm32.thumb;
    sctx->running     = ctx.running;
    sctx->exit_code   = ctx.exit_code;

    return rc;
}

int cai_arm32_run(cai_arm32_ctx_t *ctx, int max_steps)
{
    if (!ctx || !ctx->running) return CAI_EXITED;

    for (int i = 0; max_steps <= 0 || i < max_steps; i++) {
        int rc = cai_arm32_step_sa(ctx);
        if (rc == CAI_EXITED || !ctx->running) return CAI_EXITED;
        if (rc != CAI_OK) return rc;
    }
    return CAI_OK;
}

/* =========================================================================
 * Minimal ELF loader for the standalone context
 * ========================================================================= */

int cai_arm32_load_elf(cai_arm32_ctx_t *sctx, const uint8_t *elf, size_t size)
{
    if (!sctx || !elf || size < sizeof(elf32_ehdr_t))
        return CAI_EINVAL;

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)elf;

    /* Validate ELF magic and class */
    if (ehdr->e_ident[EI_MAG0] != ELF_MAGIC_0 ||
        ehdr->e_ident[EI_MAG1] != ELF_MAGIC_1 ||
        ehdr->e_ident[EI_MAG2] != ELF_MAGIC_2 ||
        ehdr->e_ident[EI_MAG3] != ELF_MAGIC_3) {
        debuglog(DEBUG_ERROR, "cai_arm32_load_elf: bad ELF magic\n");
        return CAI_EINVAL;
    }
    if (ehdr->e_ident[EI_CLASS] != ELF_CLASS_32) {
        debuglog(DEBUG_ERROR, "cai_arm32_load_elf: not a 32-bit ELF\n");
        return CAI_EINVAL;
    }
    if (ehdr->e_machine != CAI_EM_ARM) {
        debuglog(DEBUG_ERROR, "cai_arm32_load_elf: not an ARM ELF (e_machine=%u)\n",
                 ehdr->e_machine);
        return CAI_EINVAL;
    }

    /* Map PT_LOAD segments */
    const elf32_phdr_t *phdr =
        (const elf32_phdr_t *)(elf + ehdr->e_phoff);

    for (unsigned i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *ph = &phdr[i];
        if (ph->p_type != PT_LOAD) continue;

        uint32_t vaddr  = ph->p_vaddr;
        uint32_t filesz = ph->p_filesz;
        uint32_t memsz  = ph->p_memsz;

        /* Bounds check: segment must fit inside flat memory */
        if ((uint64_t)vaddr + memsz > sctx->mem_size ||
            (uint64_t)ph->p_offset + filesz > size) {
            debuglog(DEBUG_ERROR,
                     "cai_arm32_load_elf: segment %u out of bounds\n", i);
            return CAI_EINVAL;
        }

        /* Copy file data */
        uint8_t *dst = sctx->mem + (vaddr - sctx->mem_base);
        __builtin_memcpy(dst, elf + ph->p_offset, filesz);

        /* Zero BSS region (memsz > filesz) */
        if (memsz > filesz)
            __builtin_memset(dst + filesz, 0, memsz - filesz);
    }

    /* Set entry point */
    uint32_t entry = ehdr->e_entry;
    sctx->regs.r[ARM_PC] = entry & ~1u;

    /* Check EF_ARM_THUMB_FUNC flag or entry point LSB for Thumb */
    bool thumb_entry = (entry & 1u) || (ehdr->e_flags & 0x08u /* EF_ARM_INTERWORK */);
    /* More reliable: check entry LSB */
    thumb_entry = !!(entry & 1u);
    sctx->thumb_mode = thumb_entry;
    if (thumb_entry) {
        sctx->regs.cpsr |= ARM_CPSR_T;
    } else {
        sctx->regs.cpsr &= ~ARM_CPSR_T;
    }

    /* Set up a default stack at the top of memory, aligned to 8 bytes */
    uint32_t stack_top = (uint32_t)(sctx->mem_size & ~7u);
    sctx->regs.r[ARM_SP] = sctx->mem_base + stack_top;

    /* Set processor mode to User */
    sctx->regs.cpsr = (sctx->regs.cpsr & ~ARM_CPSR_M) | ARM_MODE_USR;

    sctx->running = true;
    return CAI_OK;
}

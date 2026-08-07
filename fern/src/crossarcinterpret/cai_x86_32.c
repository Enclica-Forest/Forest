/*
 * cai_x86_32.c - IA-32 (x86 32-bit) instruction emulator
 *
 * Implements a correct subset of the IA-32 ISA sufficient to run statically
 * linked ELF executables that use INT 0x80 for Linux syscalls.
 *
 * Supported instructions
 * ----------------------
 *  Data movement   : MOV r/m8/16/32, PUSH r/m/imm, POP r/m
 *  Arithmetic      : ADD, SUB, CMP, INC, DEC, NEG, IMUL (2-op)
 *  Logic           : AND, OR, XOR, NOT, SHL, SHR, SAR
 *  Control flow    : JMP (rel8/rel32/r/m32), Jcc rel8/rel32, CALL, RET
 *  String          : MOVS, STOS, SCAS, REP prefix
 *  Misc            : NOP, XCHG, LEA, INT, LEAVE, CDQ
 *
 * Flags
 * -----
 *  CF (bit 0), PF (bit 2), AF (bit 4), ZF (bit 6), SF (bit 7), OF (bit 11)
 *
 * Segment model
 * -------------
 *  Flat 32-bit with no effective segmentation (all segment bases = 0).
 *  Segment registers are tracked but have no effect on address translation.
 *
 * Not implemented (return CAI_EILL)
 * -----------------------------------
 *  Floating point, SSE/AVX, ring transitions, protected-mode segment
 *  descriptors, paging (guest runs in its own flat address space managed by
 *  the interpreter), most 2-byte opcode (0F xx) instructions.
 */

#include "crossarcinterpret.h"
#include "cai_x86_32.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/debuglog.h"

/* =========================================================================
 * EFLAGS bit masks
 * ========================================================================= */

#define FL_CF  (1u <<  0)   /* Carry flag         */
#define FL_PF  (1u <<  2)   /* Parity flag        */
#define FL_AF  (1u <<  4)   /* Auxiliary carry    */
#define FL_ZF  (1u <<  6)   /* Zero flag          */
#define FL_SF  (1u <<  7)   /* Sign flag          */
#define FL_TF  (1u <<  8)   /* Trap flag          */
#define FL_IF  (1u <<  9)   /* Interrupt enable   */
#define FL_DF  (1u << 10)   /* Direction flag     */
#define FL_OF  (1u << 11)   /* Overflow flag      */

/* =========================================================================
 * Convenience accessors for the x86-32 register file
 * ========================================================================= */

#define REG(ctx)    ((ctx)->cpu.x86_32)
#define EIP(ctx)    (REG(ctx).eip)
#define ESP(ctx)    (REG(ctx).esp)
#define EAX(ctx)    (REG(ctx).eax)
#define EBX(ctx)    (REG(ctx).ebx)
#define ECX(ctx)    (REG(ctx).ecx)
#define EDX(ctx)    (REG(ctx).edx)
#define ESI(ctx)    (REG(ctx).esi)
#define EDI(ctx)    (REG(ctx).edi)
#define EBP(ctx)    (REG(ctx).ebp)
#define EFLAGS(ctx) (REG(ctx).eflags)

/* =========================================================================
 * Register index → register pointer (for ModRM encoding)
 * ========================================================================= */

static uint32_t *reg32(cai_context_t *ctx, int idx)
{
    switch (idx & 7) {
    case 0: return &REG(ctx).eax;
    case 1: return &REG(ctx).ecx;
    case 2: return &REG(ctx).edx;
    case 3: return &REG(ctx).ebx;
    case 4: return &REG(ctx).esp;
    case 5: return &REG(ctx).ebp;
    case 6: return &REG(ctx).esi;
    case 7: return &REG(ctx).edi;
    }
    return &REG(ctx).eax; /* unreachable */
}

/* Low byte of a 32-bit register */
static uint8_t read_reg8(cai_context_t *ctx, int idx)
{
    /* idx 0-3 = AL/CL/DL/BL (low byte), 4-7 = AH/CH/DH/BH (high byte) */
    if (idx < 4)
        return (uint8_t)(*reg32(ctx, idx) & 0xFF);
    else
        return (uint8_t)((*reg32(ctx, idx - 4) >> 8) & 0xFF);
}

static void write_reg8(cai_context_t *ctx, int idx, uint8_t val)
{
    if (idx < 4) {
        uint32_t *r = reg32(ctx, idx);
        *r = (*r & 0xFFFFFF00u) | val;
    } else {
        uint32_t *r = reg32(ctx, idx - 4);
        *r = (*r & 0xFFFF00FFu) | ((uint32_t)val << 8);
    }
}

/* =========================================================================
 * Flag computation helpers
 * ========================================================================= */

static void update_flags_logical(cai_context_t *ctx, uint32_t result,
                                 int width_bits)
{
    uint32_t mask = (width_bits == 8) ? 0xFFu :
                    (width_bits == 16) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t r = result & mask;

    EFLAGS(ctx) &= ~(FL_CF | FL_OF | FL_SF | FL_ZF | FL_PF);
    if (r == 0)               EFLAGS(ctx) |= FL_ZF;
    if (r >> (width_bits - 1)) EFLAGS(ctx) |= FL_SF;
    /* Parity of low byte */
    uint8_t p = (uint8_t)r;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) EFLAGS(ctx) |= FL_PF;
}

static void update_flags_add(cai_context_t *ctx, uint32_t a, uint32_t b,
                              uint32_t result, int width_bits)
{
    uint32_t mask = (width_bits == 8) ? 0xFFu :
                    (width_bits == 16) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t sign_bit = 1u << (width_bits - 1);
    uint32_t r = result & mask;

    EFLAGS(ctx) &= ~(FL_CF | FL_OF | FL_SF | FL_ZF | FL_PF | FL_AF);

    /* CF: unsigned overflow */
    if (result > mask) EFLAGS(ctx) |= FL_CF;
    /* OF: signed overflow */
    if (!((a ^ b) & sign_bit) && ((a ^ r) & sign_bit))
        EFLAGS(ctx) |= FL_OF;
    /* SF */
    if (r & sign_bit) EFLAGS(ctx) |= FL_SF;
    /* ZF */
    if (r == 0) EFLAGS(ctx) |= FL_ZF;
    /* AF */
    if (((a & 0xF) + (b & 0xF)) > 0xF) EFLAGS(ctx) |= FL_AF;
    /* PF */
    uint8_t p = (uint8_t)r;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) EFLAGS(ctx) |= FL_PF;
}

static void update_flags_sub(cai_context_t *ctx, uint32_t a, uint32_t b,
                              uint32_t result, int width_bits)
{
    uint32_t mask = (width_bits == 8) ? 0xFFu :
                    (width_bits == 16) ? 0xFFFFu : 0xFFFFFFFFu;
    uint32_t sign_bit = 1u << (width_bits - 1);
    uint32_t r = result & mask;

    EFLAGS(ctx) &= ~(FL_CF | FL_OF | FL_SF | FL_ZF | FL_PF | FL_AF);

    /* CF: unsigned borrow */
    if ((a & mask) < (b & mask)) EFLAGS(ctx) |= FL_CF;
    /* OF: signed overflow */
    if (((a ^ b) & sign_bit) && ((a ^ r) & sign_bit))
        EFLAGS(ctx) |= FL_OF;
    /* SF */
    if (r & sign_bit) EFLAGS(ctx) |= FL_SF;
    /* ZF */
    if (r == 0) EFLAGS(ctx) |= FL_ZF;
    /* AF */
    if ((a & 0xF) < (b & 0xF)) EFLAGS(ctx) |= FL_AF;
    /* PF */
    uint8_t p = (uint8_t)r;
    p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) EFLAGS(ctx) |= FL_PF;
}

/* =========================================================================
 * Condition code evaluation (Jcc / CMOVcc)
 * ========================================================================= */

static bool eval_cond(cai_context_t *ctx, int cc)
{
    bool cf = !!(EFLAGS(ctx) & FL_CF);
    bool pf = !!(EFLAGS(ctx) & FL_PF);
    bool zf = !!(EFLAGS(ctx) & FL_ZF);
    bool sf = !!(EFLAGS(ctx) & FL_SF);
    bool of = !!(EFLAGS(ctx) & FL_OF);

    switch (cc & 0xF) {
    case 0x0: return  of;             /* JO  */
    case 0x1: return !of;             /* JNO */
    case 0x2: return  cf;             /* JB/JC/JNAE */
    case 0x3: return !cf;             /* JAE/JNB/JNC */
    case 0x4: return  zf;             /* JE/JZ */
    case 0x5: return !zf;             /* JNE/JNZ */
    case 0x6: return  cf || zf;       /* JBE/JNA */
    case 0x7: return !cf && !zf;      /* JA/JNBE */
    case 0x8: return  sf;             /* JS */
    case 0x9: return !sf;             /* JNS */
    case 0xA: return  pf;             /* JP/JPE */
    case 0xB: return !pf;             /* JNP/JPO */
    case 0xC: return  sf != of;       /* JL/JNGE */
    case 0xD: return  sf == of;       /* JGE/JNL */
    case 0xE: return  zf || (sf != of); /* JLE/JNG */
    case 0xF: return !zf && (sf == of); /* JG/JNLE */
    }
    return false;
}

/* =========================================================================
 * Instruction byte fetch helpers
 * ========================================================================= */

static int fetch8(cai_context_t *ctx, uint8_t *out)
{
    int rc = cai_mem_read8(ctx, (uint64_t)EIP(ctx), out);
    if (rc == CAI_OK) EIP(ctx)++;
    return rc;
}

static int fetch16(cai_context_t *ctx, uint16_t *out)
{
    int rc = cai_mem_read16(ctx, (uint64_t)EIP(ctx), out);
    if (rc == CAI_OK) EIP(ctx) += 2;
    return rc;
}

static int fetch32(cai_context_t *ctx, uint32_t *out)
{
    int rc = cai_mem_read32(ctx, (uint64_t)EIP(ctx), out);
    if (rc == CAI_OK) EIP(ctx) += 4;
    return rc;
}

/* =========================================================================
 * ModRM decoder
 *
 * Decodes a ModRM byte + optional SIB + displacement and returns either:
 *   - A pointer to a host-side 32-bit register (*reg_out != NULL)
 *   - A guest virtual address (*mem_gva set, reg_out→NULL)
 *
 * Returns CAI_OK or negative error.
 * ========================================================================= */

typedef struct {
    int      mod;       /* 0/1/2/3 */
    int      reg;       /* reg/opcode field */
    int      rm;        /* r/m field */
    uint32_t ea;        /* Effective address (if mod != 3) */
    bool     is_reg;    /* true if mod==3 */
} modrm_t;

static int decode_modrm(cai_context_t *ctx, modrm_t *m)
{
    uint8_t byte;
    int rc = fetch8(ctx, &byte);
    if (rc) return rc;

    m->mod = (byte >> 6) & 3;
    m->reg = (byte >> 3) & 7;
    m->rm  = byte & 7;
    m->is_reg = (m->mod == 3);

    if (m->mod == 3) {
        m->ea = 0; /* not used */
        return CAI_OK;
    }

    uint32_t base = 0, index = 0, scale = 1, disp = 0;
    bool has_base = true;

    /* SIB byte */
    if (m->rm == 4) {
        uint8_t sib;
        rc = fetch8(ctx, &sib);
        if (rc) return rc;

        int ss  = (sib >> 6) & 3;
        int idx = (sib >> 3) & 7;
        int bas = sib & 7;

        scale = 1u << ss;

        /* Index */
        if (idx != 4)
            index = *reg32(ctx, idx) * scale;

        /* Base */
        if (bas == 5 && m->mod == 0) {
            /* No base; displacement32 follows */
            has_base = false;
        } else {
            base = *reg32(ctx, bas);
        }
    } else {
        base = *reg32(ctx, m->rm);
    }

    /* Displacement */
    if (m->mod == 1) {
        uint8_t d8; rc = fetch8(ctx, &d8); if (rc) return rc;
        disp = (uint32_t)(int32_t)(int8_t)d8;
    } else if (m->mod == 2 || (!has_base && m->rm == 4)) {
        uint32_t d32; rc = fetch32(ctx, &d32); if (rc) return rc;
        disp = d32;
    } else if (m->mod == 0 && m->rm == 5) {
        /* Direct 32-bit address (no SIB) */
        uint32_t d32; rc = fetch32(ctx, &d32); if (rc) return rc;
        m->ea = d32;
        return CAI_OK;
    }

    m->ea = (has_base ? base : 0) + index + disp;
    return CAI_OK;
}

/* Read/write 32-bit value via ModRM */
static int modrm_read32(cai_context_t *ctx, const modrm_t *m, uint32_t *out)
{
    if (m->is_reg)
        return (*out = *reg32(ctx, m->rm)), CAI_OK;
    return cai_mem_read32(ctx, (uint64_t)m->ea, out);
}

static int modrm_write32(cai_context_t *ctx, const modrm_t *m, uint32_t val)
{
    if (m->is_reg)
        return (*reg32(ctx, m->rm) = val), CAI_OK;
    return cai_mem_write32(ctx, (uint64_t)m->ea, val);
}

static int modrm_read8(cai_context_t *ctx, const modrm_t *m, uint8_t *out)
{
    if (m->is_reg)
        return (*out = read_reg8(ctx, m->rm)), CAI_OK;
    return cai_mem_read8(ctx, (uint64_t)m->ea, out);
}

static int modrm_write8(cai_context_t *ctx, const modrm_t *m, uint8_t val)
{
    if (m->is_reg)
        return write_reg8(ctx, m->rm, val), CAI_OK;
    return cai_mem_write8(ctx, (uint64_t)m->ea, val);
}

/* =========================================================================
 * ALU dispatch for Group1 opcodes (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP)
 * ========================================================================= */

static int alu32(cai_context_t *ctx, int op, uint32_t dst, uint32_t src,
                 uint32_t *result_out)
{
    uint32_t r;
    switch (op) {
    case 0: r = dst + src; update_flags_add(ctx, dst, src, dst + src, 32); break; /* ADD */
    case 1: r = dst | src; update_flags_logical(ctx, r, 32); break;               /* OR  */
    case 2: {  /* ADC */
        uint32_t c = !!(EFLAGS(ctx) & FL_CF);
        r = dst + src + c;
        update_flags_add(ctx, dst, src + c, r, 32);
        break;
    }
    case 3: {  /* SBB */
        uint32_t c = !!(EFLAGS(ctx) & FL_CF);
        r = dst - src - c;
        update_flags_sub(ctx, dst, src + c, r, 32);
        break;
    }
    case 4: r = dst & src; update_flags_logical(ctx, r, 32); break;               /* AND */
    case 5: r = dst - src; update_flags_sub(ctx, dst, src, r, 32); break;         /* SUB */
    case 6: r = dst ^ src; update_flags_logical(ctx, r, 32); break;               /* XOR */
    case 7: r = dst - src; update_flags_sub(ctx, dst, src, r, 32); /* CMP */
            *result_out = dst; return CAI_OK; /* CMP: result not written */
    default: return CAI_EILL;
    }
    *result_out = r;
    return CAI_OK;
}

static int alu8(cai_context_t *ctx, int op, uint8_t dst, uint8_t src,
                uint8_t *result_out)
{
    uint32_t d = dst, s = src, r;
    switch (op) {
    case 0: r = d + s; update_flags_add(ctx, d, s, r, 8); break;
    case 1: r = d | s; update_flags_logical(ctx, r, 8); break;
    case 2: { uint32_t c = !!(EFLAGS(ctx) & FL_CF); r = d + s + c;
              update_flags_add(ctx, d, s + c, r, 8); break; }
    case 3: { uint32_t c = !!(EFLAGS(ctx) & FL_CF); r = d - s - c;
              update_flags_sub(ctx, d, s + c, r, 8); break; }
    case 4: r = d & s; update_flags_logical(ctx, r, 8); break;
    case 5: r = d - s; update_flags_sub(ctx, d, s, r, 8); break;
    case 6: r = d ^ s; update_flags_logical(ctx, r, 8); break;
    case 7: r = d - s; update_flags_sub(ctx, d, s, r, 8);
            *result_out = dst; return CAI_OK;
    default: return CAI_EILL;
    }
    *result_out = (uint8_t)r;
    return CAI_OK;
}

/* =========================================================================
 * Single-step execution
 * ========================================================================= */

int cai_x86_32_step(cai_context_t *ctx)
{
    uint8_t opcode;
    uint32_t saved_eip = EIP(ctx);
    int rc;
    bool rep_prefix = false;
    bool operand_prefix = false; /* 0x66 prefix (16-bit operand override) */

    /* Collect prefix bytes */
next_prefix:
    rc = fetch8(ctx, &opcode);
    if (rc) return rc;

    switch (opcode) {
    case 0xF3: rep_prefix = true; goto next_prefix;
    case 0xF2: rep_prefix = true; goto next_prefix; /* REPNE – treat as REP here */
    case 0x66: operand_prefix = true; goto next_prefix;
    case 0x67: goto next_prefix;  /* address-size prefix – 32-bit addressing assumed */
    case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0x64: case 0x65: goto next_prefix; /* segment overrides – ignored */
    case 0xF0: goto next_prefix;  /* LOCK – ignored */
    default: break;
    }

    switch (opcode) {

    /* ------------------------------------------------------------------ */
    /* NOP (also XCHG EAX,EAX)                                            */
    case 0x90:
        break;

    /* ------------------------------------------------------------------ */
    /* MOV r/m32, r32  (89)                                               */
    case 0x89: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t val = *reg32(ctx, m.reg);
        rc = modrm_write32(ctx, &m, val);
        break;
    }

    /* MOV r32, r/m32  (8B)                                               */
    case 0x8B: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t val; rc = modrm_read32(ctx, &m, &val); if (rc) return rc;
        *reg32(ctx, m.reg) = val;
        break;
    }

    /* MOV r/m8, r8  (88)                                                 */
    case 0x88: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint8_t val = read_reg8(ctx, m.reg);
        rc = modrm_write8(ctx, &m, val);
        break;
    }

    /* MOV r8, r/m8  (8A)                                                 */
    case 0x8A: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint8_t val; rc = modrm_read8(ctx, &m, &val); if (rc) return rc;
        write_reg8(ctx, m.reg, val);
        break;
    }

    /* MOV r32, imm32  (B8+r)                                             */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
        *reg32(ctx, opcode - 0xB8) = imm;
        break;
    }

    /* MOV r8, imm8  (B0+r)                                               */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        uint8_t imm; rc = fetch8(ctx, &imm); if (rc) return rc;
        write_reg8(ctx, opcode - 0xB0, imm);
        break;
    }

    /* MOV r/m32, imm32  (C7 /0)                                          */
    case 0xC7: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
        rc = modrm_write32(ctx, &m, imm);
        break;
    }

    /* MOV r/m8, imm8  (C6 /0)                                            */
    case 0xC6: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint8_t imm; rc = fetch8(ctx, &imm); if (rc) return rc;
        rc = modrm_write8(ctx, &m, imm);
        break;
    }

    /* MOVZX r32, r/m8  (0F B6)  – see 0x0F handler                      */

    /* ------------------------------------------------------------------ */
    /* LEA r32, m  (8D)                                                    */
    case 0x8D: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        if (m.is_reg) return CAI_EILL; /* LEA with register source is invalid */
        *reg32(ctx, m.reg) = m.ea;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* PUSH r32 (50+r)                                                     */
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57: {
        uint32_t val = *reg32(ctx, opcode - 0x50);
        rc = cai_stack_push32(ctx, val);
        break;
    }

    /* POP r32  (58+r)                                                     */
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint32_t val;
        rc = cai_stack_pop32(ctx, &val); if (rc) return rc;
        *reg32(ctx, opcode - 0x58) = val;
        break;
    }

    /* PUSH imm32  (68)                                                    */
    case 0x68: {
        uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
        rc = cai_stack_push32(ctx, imm);
        break;
    }

    /* PUSH imm8 (sign-extended) (6A)                                     */
    case 0x6A: {
        uint8_t imm8; rc = fetch8(ctx, &imm8); if (rc) return rc;
        rc = cai_stack_push32(ctx, (uint32_t)(int32_t)(int8_t)imm8);
        break;
    }

    /* PUSH r/m32  (FF /6)                                                 */
    /* POP r/m32  (8F /0) – see Group 5 / Group 1A below                  */

    /* ------------------------------------------------------------------ */
    /* XCHG r32, EAX  (91+r)                                              */
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: {
        uint32_t *r = reg32(ctx, opcode - 0x90);
        uint32_t tmp = EAX(ctx);
        EAX(ctx) = *r;
        *r = tmp;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Group 1 arithmetic  (80/81/83) – r/m, imm                          */
    case 0x80: {  /* r/m8, imm8 */
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint8_t src8; rc = fetch8(ctx, &src8); if (rc) return rc;
        uint8_t dst8; rc = modrm_read8(ctx, &m, &dst8); if (rc) return rc;
        uint8_t res8;
        rc = alu8(ctx, m.reg, dst8, src8, &res8); if (rc) return rc;
        if (m.reg != 7) rc = modrm_write8(ctx, &m, res8);
        break;
    }
    case 0x81: {  /* r/m32, imm32 */
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res;
        rc = alu32(ctx, m.reg, dst, imm, &res); if (rc) return rc;
        if (m.reg != 7) rc = modrm_write32(ctx, &m, res);
        break;
    }
    case 0x83: {  /* r/m32, imm8 sign-extended */
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint8_t imm8; rc = fetch8(ctx, &imm8); if (rc) return rc;
        uint32_t imm = (uint32_t)(int32_t)(int8_t)imm8;
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res;
        rc = alu32(ctx, m.reg, dst, imm, &res); if (rc) return rc;
        if (m.reg != 7) rc = modrm_write32(ctx, &m, res);
        break;
    }

    /* ADD r/m32, r32  (01)                                                */
    case 0x01: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res; alu32(ctx, 0, dst, src, &res);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    /* ADD r32, r/m32  (03)                                                */
    case 0x03: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
        uint32_t dst = *reg32(ctx, m.reg);
        uint32_t res; alu32(ctx, 0, dst, src, &res);
        *reg32(ctx, m.reg) = res;
        break;
    }

    /* SUB r/m32, r32  (29)                                                */
    case 0x29: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res; alu32(ctx, 5, dst, src, &res);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    /* SUB r32, r/m32  (2B)                                                */
    case 0x2B: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
        uint32_t dst = *reg32(ctx, m.reg);
        uint32_t res; alu32(ctx, 5, dst, src, &res);
        *reg32(ctx, m.reg) = res;
        break;
    }

    /* AND r/m32, r32  (21)                                                */
    case 0x21: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res; alu32(ctx, 4, dst, src, &res);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    /* AND r32, r/m32  (23)                                                */
    case 0x23: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
        uint32_t dst = *reg32(ctx, m.reg);
        uint32_t res; alu32(ctx, 4, dst, src, &res);
        *reg32(ctx, m.reg) = res;
        break;
    }

    /* OR r/m32, r32  (09)                                                 */
    case 0x09: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res; alu32(ctx, 1, dst, src, &res);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    /* OR r32, r/m32  (0B)                                                 */
    case 0x0B: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
        uint32_t dst = *reg32(ctx, m.reg);
        uint32_t res; alu32(ctx, 1, dst, src, &res);
        *reg32(ctx, m.reg) = res;
        break;
    }

    /* XOR r/m32, r32  (31)                                                */
    case 0x31: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res; alu32(ctx, 6, dst, src, &res);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    /* XOR r32, r/m32  (33)                                                */
    case 0x33: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
        uint32_t dst = *reg32(ctx, m.reg);
        uint32_t res; alu32(ctx, 6, dst, src, &res);
        *reg32(ctx, m.reg) = res;
        break;
    }

    /* CMP r/m32, r32  (39)                                                */
    case 0x39: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res; alu32(ctx, 7, dst, src, &res);
        break;
    }
    /* CMP r32, r/m32  (3B)                                                */
    case 0x3B: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
        uint32_t dst = *reg32(ctx, m.reg);
        uint32_t res; alu32(ctx, 7, dst, src, &res);
        break;
    }
    /* CMP AL, imm8  (3C)                                                  */
    case 0x3C: {
        uint8_t imm; rc = fetch8(ctx, &imm); if (rc) return rc;
        uint8_t dst = (uint8_t)EAX(ctx);
        uint8_t res; alu8(ctx, 7, dst, imm, &res);
        break;
    }
    /* CMP EAX, imm32  (3D)                                                */
    case 0x3D: {
        uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
        uint32_t res; alu32(ctx, 7, EAX(ctx), imm, &res);
        break;
    }

    /* TEST r/m32, r32  (85)                                               */
    case 0x85: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t src = *reg32(ctx, m.reg);
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res = dst & src;
        update_flags_logical(ctx, res, 32);
        break;
    }
    /* TEST EAX, imm32  (A9)                                               */
    case 0xA9: {
        uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
        uint32_t res = EAX(ctx) & imm;
        update_flags_logical(ctx, res, 32);
        break;
    }
    /* TEST AL, imm8  (A8)                                                 */
    case 0xA8: {
        uint8_t imm; rc = fetch8(ctx, &imm); if (rc) return rc;
        uint8_t res = (uint8_t)EAX(ctx) & imm;
        update_flags_logical(ctx, res, 8);
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Group 2: Shift/rotate  (C0/C1/D0/D1/D2/D3)                        */
    case 0xC1: {  /* r/m32, imm8 */
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint8_t cnt; rc = fetch8(ctx, &cnt); if (rc) return rc;
        cnt &= 31;
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res = dst;
        switch (m.reg) {
        case 4: res = dst << cnt; EFLAGS(ctx) &= ~FL_CF;
                if (cnt) EFLAGS(ctx) |= (dst >> (32 - cnt)) & 1 ? FL_CF : 0;
                break; /* SHL */
        case 5: res = dst >> cnt; EFLAGS(ctx) &= ~FL_CF;
                if (cnt) EFLAGS(ctx) |= (dst >> (cnt - 1)) & 1 ? FL_CF : 0;
                break; /* SHR */
        case 7: res = (uint32_t)((int32_t)dst >> cnt); break; /* SAR */
        case 0: /* ROL */
                if (cnt) res = (dst << cnt) | (dst >> (32 - cnt));
                break;
        case 1: /* ROR */
                if (cnt) res = (dst >> cnt) | (dst << (32 - cnt));
                break;
        default: return CAI_EILL;
        }
        if (cnt) update_flags_logical(ctx, res, 32);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    case 0xD3: {  /* r/m32, CL */
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t cnt = ECX(ctx) & 31;
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res = dst;
        switch (m.reg) {
        case 4: res = dst << cnt; break;  /* SHL */
        case 5: res = dst >> cnt; break;  /* SHR */
        case 7: res = (uint32_t)((int32_t)dst >> cnt); break; /* SAR */
        default: return CAI_EILL;
        }
        if (cnt) update_flags_logical(ctx, res, 32);
        rc = modrm_write32(ctx, &m, res);
        break;
    }
    case 0xD1: {  /* r/m32, 1 */
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t dst; rc = modrm_read32(ctx, &m, &dst); if (rc) return rc;
        uint32_t res = dst;
        switch (m.reg) {
        case 4: EFLAGS(ctx) = (EFLAGS(ctx) & ~FL_CF) | ((dst >> 31) & 1 ? FL_CF : 0);
                res = dst << 1; break;  /* SHL */
        case 5: EFLAGS(ctx) = (EFLAGS(ctx) & ~FL_CF) | (dst & 1 ? FL_CF : 0);
                res = dst >> 1; break;  /* SHR */
        case 7: res = (uint32_t)((int32_t)dst >> 1); break; /* SAR */
        default: return CAI_EILL;
        }
        update_flags_logical(ctx, res, 32);
        rc = modrm_write32(ctx, &m, res);
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Group 3: NOT / NEG / MUL / IMUL / DIV / IDIV  (F7)               */
    case 0xF7: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t val; rc = modrm_read32(ctx, &m, &val); if (rc) return rc;
        switch (m.reg) {
        case 2: /* NOT */
            rc = modrm_write32(ctx, &m, ~val);
            break;
        case 3: { /* NEG */
            uint32_t res = (uint32_t)(-(int32_t)val);
            update_flags_sub(ctx, 0, val, res, 32);
            rc = modrm_write32(ctx, &m, res);
            break;
        }
        case 4: { /* MUL – EDX:EAX = EAX * r/m32 */
            uint64_t prod = (uint64_t)EAX(ctx) * (uint64_t)val;
            EAX(ctx) = (uint32_t)(prod);
            EDX(ctx) = (uint32_t)(prod >> 32);
            break;
        }
        case 5: { /* IMUL – EDX:EAX = EAX * r/m32 */
            int64_t prod = (int64_t)(int32_t)EAX(ctx) * (int64_t)(int32_t)val;
            EAX(ctx) = (uint32_t)(prod);
            EDX(ctx) = (uint32_t)((uint64_t)prod >> 32);
            break;
        }
        case 6: { /* DIV EAX / r/m32 */
            uint64_t num = ((uint64_t)EDX(ctx) << 32) | EAX(ctx);
            if (!val) return CAI_EILL; /* #DE */
            EAX(ctx) = (uint32_t)(num / val);
            EDX(ctx) = (uint32_t)(num % val);
            break;
        }
        case 7: { /* IDIV */
            int64_t num = (int64_t)(((uint64_t)EDX(ctx) << 32) | EAX(ctx));
            if (!val) return CAI_EILL;
            EAX(ctx) = (uint32_t)((uint32_t)((int64_t)num / (int32_t)val));
            EDX(ctx) = (uint32_t)((uint32_t)((int64_t)num % (int32_t)val));
            break;
        }
        case 0: { /* TEST r/m32, imm32 */
            uint32_t imm; rc = fetch32(ctx, &imm); if (rc) return rc;
            update_flags_logical(ctx, val & imm, 32);
            break;
        }
        default: return CAI_EILL;
        }
        break;
    }

    /* IMUL r32, r/m32  (0F AF)  – see two-byte escape                    */

    /* ------------------------------------------------------------------ */
    /* INC r32  (40+r)                                                     */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: {
        uint32_t *r = reg32(ctx, opcode - 0x40);
        uint32_t old = *r;
        (*r)++;
        uint32_t res; alu32(ctx, 0, old, 1, &res);
        /* Preserve CF (INC does not modify CF) */
        uint32_t cf = EFLAGS(ctx) & FL_CF;
        update_flags_add(ctx, old, 1, old + 1, 32);
        EFLAGS(ctx) = (EFLAGS(ctx) & ~FL_CF) | cf;
        break;
    }

    /* DEC r32  (48+r)                                                     */
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        uint32_t *r = reg32(ctx, opcode - 0x48);
        uint32_t old = *r;
        uint32_t cf = EFLAGS(ctx) & FL_CF;
        (*r)--;
        update_flags_sub(ctx, old, 1, old - 1, 32);
        EFLAGS(ctx) = (EFLAGS(ctx) & ~FL_CF) | cf;
        break;
    }

    /* Group 5: INC/DEC/CALL/JMP/PUSH r/m32  (FF)                         */
    case 0xFF: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        switch (m.reg) {
        case 0: { /* INC r/m32 */
            uint32_t val; rc = modrm_read32(ctx, &m, &val); if (rc) return rc;
            uint32_t cf = EFLAGS(ctx) & FL_CF;
            update_flags_add(ctx, val, 1, val + 1, 32);
            EFLAGS(ctx) = (EFLAGS(ctx) & ~FL_CF) | cf;
            rc = modrm_write32(ctx, &m, val + 1);
            break;
        }
        case 1: { /* DEC r/m32 */
            uint32_t val; rc = modrm_read32(ctx, &m, &val); if (rc) return rc;
            uint32_t cf = EFLAGS(ctx) & FL_CF;
            update_flags_sub(ctx, val, 1, val - 1, 32);
            EFLAGS(ctx) = (EFLAGS(ctx) & ~FL_CF) | cf;
            rc = modrm_write32(ctx, &m, val - 1);
            break;
        }
        case 2: { /* CALL r/m32 */
            uint32_t target; rc = modrm_read32(ctx, &m, &target); if (rc) return rc;
            rc = cai_stack_push32(ctx, EIP(ctx)); if (rc) return rc;
            EIP(ctx) = target;
            break;
        }
        case 4: { /* JMP r/m32 */
            uint32_t target; rc = modrm_read32(ctx, &m, &target); if (rc) return rc;
            EIP(ctx) = target;
            break;
        }
        case 6: { /* PUSH r/m32 */
            uint32_t val; rc = modrm_read32(ctx, &m, &val); if (rc) return rc;
            rc = cai_stack_push32(ctx, val);
            break;
        }
        default: return CAI_EILL;
        }
        break;
    }

    /* Group 1A: POP r/m32  (8F /0)                                        */
    case 0x8F: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t val; rc = cai_stack_pop32(ctx, &val); if (rc) return rc;
        rc = modrm_write32(ctx, &m, val);
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Control flow: unconditional                                          */

    /* CALL rel32  (E8)                                                    */
    case 0xE8: {
        uint32_t rel; rc = fetch32(ctx, &rel); if (rc) return rc;
        rc = cai_stack_push32(ctx, EIP(ctx)); if (rc) return rc;
        EIP(ctx) = EIP(ctx) + (int32_t)rel;
        break;
    }

    /* RET (near)  (C3)                                                    */
    case 0xC3: {
        uint32_t addr; rc = cai_stack_pop32(ctx, &addr); if (rc) return rc;
        EIP(ctx) = addr;
        break;
    }

    /* RET imm16  (C2)                                                     */
    case 0xC2: {
        uint16_t imm; rc = fetch16(ctx, &imm); if (rc) return rc;
        uint32_t addr; rc = cai_stack_pop32(ctx, &addr); if (rc) return rc;
        EIP(ctx) = addr;
        ESP(ctx) += imm;
        break;
    }

    /* JMP rel8  (EB)                                                      */
    case 0xEB: {
        uint8_t rel; rc = fetch8(ctx, &rel); if (rc) return rc;
        EIP(ctx) = EIP(ctx) + (int32_t)(int8_t)rel;
        break;
    }

    /* JMP rel32  (E9)                                                     */
    case 0xE9: {
        uint32_t rel; rc = fetch32(ctx, &rel); if (rc) return rc;
        EIP(ctx) = EIP(ctx) + (int32_t)rel;
        break;
    }

    /* LEAVE  (C9)                                                         */
    case 0xC9: {
        ESP(ctx) = EBP(ctx);
        uint32_t val; rc = cai_stack_pop32(ctx, &val); if (rc) return rc;
        EBP(ctx) = val;
        break;
    }

    /* ENTER imm16, 0  (C8)  – simplified: only level 0                   */
    case 0xC8: {
        uint16_t frame_size; rc = fetch16(ctx, &frame_size); if (rc) return rc;
        uint8_t level; rc = fetch8(ctx, &level); if (rc) return rc;
        rc = cai_stack_push32(ctx, EBP(ctx)); if (rc) return rc;
        EBP(ctx) = ESP(ctx);
        ESP(ctx) -= frame_size;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Jcc rel8  (70..7F)                                                  */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint8_t rel; rc = fetch8(ctx, &rel); if (rc) return rc;
        if (eval_cond(ctx, opcode - 0x70))
            EIP(ctx) = EIP(ctx) + (int32_t)(int8_t)rel;
        break;
    }

    /* JCXZ/JECXZ rel8  (E3)                                              */
    case 0xE3: {
        uint8_t rel; rc = fetch8(ctx, &rel); if (rc) return rc;
        if (ECX(ctx) == 0)
            EIP(ctx) = EIP(ctx) + (int32_t)(int8_t)rel;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* LOOP/LOOPZ/LOOPNZ rel8                                             */
    case 0xE2: { /* LOOP */
        uint8_t rel; rc = fetch8(ctx, &rel); if (rc) return rc;
        ECX(ctx)--;
        if (ECX(ctx) != 0) EIP(ctx) = EIP(ctx) + (int32_t)(int8_t)rel;
        break;
    }
    case 0xE1: { /* LOOPZ */
        uint8_t rel; rc = fetch8(ctx, &rel); if (rc) return rc;
        ECX(ctx)--;
        if (ECX(ctx) != 0 && (EFLAGS(ctx) & FL_ZF))
            EIP(ctx) = EIP(ctx) + (int32_t)(int8_t)rel;
        break;
    }
    case 0xE0: { /* LOOPNZ */
        uint8_t rel; rc = fetch8(ctx, &rel); if (rc) return rc;
        ECX(ctx)--;
        if (ECX(ctx) != 0 && !(EFLAGS(ctx) & FL_ZF))
            EIP(ctx) = EIP(ctx) + (int32_t)(int8_t)rel;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* String operations                                                    */

    /* MOVSD  (A5) – with optional REP                                     */
    case 0xA5: {
        uint32_t count = rep_prefix ? ECX(ctx) : 1;
        int dir = (EFLAGS(ctx) & FL_DF) ? -1 : 1;
        while (count--) {
            uint32_t val; rc = cai_mem_read32(ctx, ESI(ctx), &val); if (rc) return rc;
            rc = cai_mem_write32(ctx, EDI(ctx), val); if (rc) return rc;
            ESI(ctx) += (uint32_t)(dir * 4);
            EDI(ctx) += (uint32_t)(dir * 4);
        }
        if (rep_prefix) ECX(ctx) = 0;
        break;
    }

    /* MOVSB  (A4)                                                         */
    case 0xA4: {
        uint32_t count = rep_prefix ? ECX(ctx) : 1;
        int dir = (EFLAGS(ctx) & FL_DF) ? -1 : 1;
        while (count--) {
            uint8_t val; rc = cai_mem_read8(ctx, ESI(ctx), &val); if (rc) return rc;
            rc = cai_mem_write8(ctx, EDI(ctx), val); if (rc) return rc;
            ESI(ctx) += (uint32_t)dir;
            EDI(ctx) += (uint32_t)dir;
        }
        if (rep_prefix) ECX(ctx) = 0;
        break;
    }

    /* STOSD  (AB)                                                          */
    case 0xAB: {
        uint32_t count = rep_prefix ? ECX(ctx) : 1;
        int dir = (EFLAGS(ctx) & FL_DF) ? -1 : 1;
        while (count--) {
            rc = cai_mem_write32(ctx, EDI(ctx), EAX(ctx)); if (rc) return rc;
            EDI(ctx) += (uint32_t)(dir * 4);
        }
        if (rep_prefix) ECX(ctx) = 0;
        break;
    }

    /* STOSB  (AA)                                                          */
    case 0xAA: {
        uint32_t count = rep_prefix ? ECX(ctx) : 1;
        int dir = (EFLAGS(ctx) & FL_DF) ? -1 : 1;
        while (count--) {
            rc = cai_mem_write8(ctx, EDI(ctx), (uint8_t)EAX(ctx)); if (rc) return rc;
            EDI(ctx) += (uint32_t)dir;
        }
        if (rep_prefix) ECX(ctx) = 0;
        break;
    }

    /* SCASD (AE/AF)                                                        */
    case 0xAF: {
        uint32_t count = rep_prefix ? ECX(ctx) : 1;
        int dir = (EFLAGS(ctx) & FL_DF) ? -1 : 1;
        while (count--) {
            uint32_t val; rc = cai_mem_read32(ctx, EDI(ctx), &val); if (rc) return rc;
            uint32_t res; alu32(ctx, 7, EAX(ctx), val, &res);
            EDI(ctx) += (uint32_t)(dir * 4);
            if (rep_prefix) {
                ECX(ctx)--;
                if (EFLAGS(ctx) & FL_ZF) { count = 0; }
            }
        }
        if (rep_prefix) ECX(ctx) = 0;
        break;
    }

    /* LODSD  (AD)                                                          */
    case 0xAD: {
        rc = cai_mem_read32(ctx, ESI(ctx), &EAX(ctx)); if (rc) return rc;
        ESI(ctx) += (EFLAGS(ctx) & FL_DF) ? (uint32_t)-4 : 4;
        break;
    }

    /* LODSB  (AC)                                                          */
    case 0xAC: {
        uint8_t val; rc = cai_mem_read8(ctx, ESI(ctx), &val); if (rc) return rc;
        EAX(ctx) = (EAX(ctx) & 0xFFFFFF00u) | val;
        ESI(ctx) += (EFLAGS(ctx) & FL_DF) ? (uint32_t)-1 : 1;
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Misc                                                                 */

    /* CDQ  (99)                                                            */
    case 0x99:
        EDX(ctx) = (EAX(ctx) & 0x80000000u) ? 0xFFFFFFFFu : 0;
        break;

    /* CLC/STC/CLD/STD                                                     */
    case 0xF8: EFLAGS(ctx) &= ~FL_CF; break;  /* CLC */
    case 0xF9: EFLAGS(ctx) |=  FL_CF; break;  /* STC */
    case 0xFC: EFLAGS(ctx) &= ~FL_DF; break;  /* CLD */
    case 0xFD: EFLAGS(ctx) |=  FL_DF; break;  /* STD */

    /* PUSHFD  (9C)                                                         */
    case 0x9C:
        rc = cai_stack_push32(ctx, EFLAGS(ctx));
        break;

    /* POPFD  (9D)                                                          */
    case 0x9D:
        rc = cai_stack_pop32(ctx, &EFLAGS(ctx));
        break;

    /* PUSHA  (60)                                                          */
    case 0x60: {
        uint32_t old_esp = ESP(ctx);
        rc = cai_stack_push32(ctx, EAX(ctx)); if (rc) return rc;
        rc = cai_stack_push32(ctx, ECX(ctx)); if (rc) return rc;
        rc = cai_stack_push32(ctx, EDX(ctx)); if (rc) return rc;
        rc = cai_stack_push32(ctx, EBX(ctx)); if (rc) return rc;
        rc = cai_stack_push32(ctx, old_esp);  if (rc) return rc;
        rc = cai_stack_push32(ctx, EBP(ctx)); if (rc) return rc;
        rc = cai_stack_push32(ctx, ESI(ctx)); if (rc) return rc;
        rc = cai_stack_push32(ctx, EDI(ctx)); if (rc) return rc;
        break;
    }

    /* POPA  (61)                                                           */
    case 0x61: {
        rc = cai_stack_pop32(ctx, &EDI(ctx)); if (rc) return rc;
        rc = cai_stack_pop32(ctx, &ESI(ctx)); if (rc) return rc;
        rc = cai_stack_pop32(ctx, &EBP(ctx)); if (rc) return rc;
        uint32_t dummy; rc = cai_stack_pop32(ctx, &dummy); if (rc) return rc;
        rc = cai_stack_pop32(ctx, &EBX(ctx)); if (rc) return rc;
        rc = cai_stack_pop32(ctx, &EDX(ctx)); if (rc) return rc;
        rc = cai_stack_pop32(ctx, &ECX(ctx)); if (rc) return rc;
        rc = cai_stack_pop32(ctx, &EAX(ctx)); if (rc) return rc;
        break;
    }

    /* MOV to/from segment registers (8C/8E) – accept but ignore          */
    case 0x8C: case 0x8E: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        if (opcode == 0x8C && m.is_reg) {
            /* MOV r/m16, Sreg – write 0 */
            *reg32(ctx, m.rm) = 0;
        }
        break;
    }

    /* XCHG r/m32, r32  (87)                                               */
    case 0x87: {
        modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
        uint32_t rm_val, reg_val;
        rc = modrm_read32(ctx, &m, &rm_val); if (rc) return rc;
        reg_val = *reg32(ctx, m.reg);
        rc = modrm_write32(ctx, &m, reg_val); if (rc) return rc;
        *reg32(ctx, m.reg) = rm_val;
        break;
    }

    /* MOVSX r32, r/m8  (BE from two-byte escape handled below)           */
    /* MOVZX r32, r/m8                                                     */

    /* ------------------------------------------------------------------ */
    /* INT  (CD)                                                            */
    case 0xCD: {
        uint8_t vector; rc = fetch8(ctx, &vector); if (rc) return rc;
        if (vector == 0x80) {
            /* Linux i386 syscall */
            int64_t result = cai_syscall_dispatch(ctx, EAX(ctx));
            EAX(ctx) = (uint32_t)(int32_t)result;
            if (!ctx->running)
                return CAI_EXITED;
        } else {
            debuglog(DEBUG_WARN, "cai/x86_32: unhandled INT 0x%02X\n", vector);
            return CAI_EILL;
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Two-byte escape (0F xx)                                             */
    case 0x0F: {
        uint8_t op2; rc = fetch8(ctx, &op2); if (rc) return rc;
        switch (op2) {

        /* Jcc rel32  (0F 80..8F)                                          */
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
        case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
            uint32_t rel; rc = fetch32(ctx, &rel); if (rc) return rc;
            if (eval_cond(ctx, op2 - 0x80))
                EIP(ctx) = EIP(ctx) + (int32_t)rel;
            break;
        }

        /* IMUL r32, r/m32  (0F AF)                                        */
        case 0xAF: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
            int64_t prod = (int64_t)(int32_t)*reg32(ctx, m.reg) *
                           (int64_t)(int32_t)src;
            *reg32(ctx, m.reg) = (uint32_t)prod;
            break;
        }

        /* MOVZX r32, r/m8  (0F B6)                                        */
        case 0xB6: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint8_t val; rc = modrm_read8(ctx, &m, &val); if (rc) return rc;
            *reg32(ctx, m.reg) = (uint32_t)val;
            break;
        }

        /* MOVZX r32, r/m16  (0F B7)                                       */
        case 0xB7: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint16_t val;
            if (m.is_reg) val = (uint16_t)*reg32(ctx, m.rm);
            else { rc = cai_mem_read16(ctx, m.ea, &val); if (rc) return rc; }
            *reg32(ctx, m.reg) = (uint32_t)val;
            break;
        }

        /* MOVSX r32, r/m8  (0F BE)                                        */
        case 0xBE: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint8_t val; rc = modrm_read8(ctx, &m, &val); if (rc) return rc;
            *reg32(ctx, m.reg) = (uint32_t)(int32_t)(int8_t)val;
            break;
        }

        /* MOVSX r32, r/m16  (0F BF)                                       */
        case 0xBF: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint16_t val;
            if (m.is_reg) val = (uint16_t)*reg32(ctx, m.rm);
            else { rc = cai_mem_read16(ctx, m.ea, &val); if (rc) return rc; }
            *reg32(ctx, m.reg) = (uint32_t)(int32_t)(int16_t)val;
            break;
        }

        /* SETcc r/m8  (0F 90..9F)                                         */
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
        case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint8_t v = eval_cond(ctx, op2 - 0x90) ? 1 : 0;
            rc = modrm_write8(ctx, &m, v);
            break;
        }

        /* BSF r32, r/m32  (0F BC)                                         */
        case 0xBC: {
            modrm_t m; rc = decode_modrm(ctx, &m); if (rc) return rc;
            uint32_t src; rc = modrm_read32(ctx, &m, &src); if (rc) return rc;
            if (src == 0) {
                EFLAGS(ctx) |= FL_ZF;
            } else {
                EFLAGS(ctx) &= ~FL_ZF;
                int bit = 0;
                while (!(src & (1u << bit))) bit++;
                *reg32(ctx, m.reg) = (uint32_t)bit;
            }
            break;
        }

        default:
            debuglog(DEBUG_WARN,
                     "cai/x86_32: unimplemented 0F %02X at eip=%08X\n",
                     op2, saved_eip);
            return CAI_EILL;
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    /* Unimplemented opcode                                                 */
    default:
        debuglog(DEBUG_WARN,
                 "cai/x86_32: unimplemented opcode 0x%02X at eip=%08X\n",
                 opcode, saved_eip);
        EIP(ctx) = saved_eip; /* Restore so caller can diagnose */
        return CAI_EILL;
    }

    if (rc) return rc;

    /* Sync the unified PC field */
    ctx->pc = (uint64_t)EIP(ctx);
    return CAI_OK;
}

/* =========================================================================
 * Standalone context API  (cai_x86_32_ctx_t from cai_x86_32.h)
 *
 * These functions wrap the framework-based step function by projecting the
 * standalone register layout onto a cai_context_t on each call.
 *
 * The standalone x86_32_regs_t uses Intel encoding order
 *   (eax, ecx, edx, ebx, esp, ebp, esi, edi)
 * while cai_x86_32_regs_t in crossarcinterpret.h uses
 *   (eax, ebx, ecx, edx, esi, edi, esp, ebp).
 * The copy helpers below translate between the two layouts explicitly.
 * ========================================================================= */

/* Copy standalone registers → framework registers */
static void sa_to_fctx(cai_context_t *fctx, const cai_x86_32_ctx_t *sa)
{
    fctx->cpu.x86_32.eax    = sa->regs.eax;
    fctx->cpu.x86_32.ecx    = sa->regs.ecx;
    fctx->cpu.x86_32.edx    = sa->regs.edx;
    fctx->cpu.x86_32.ebx    = sa->regs.ebx;
    fctx->cpu.x86_32.esp    = sa->regs.esp;
    fctx->cpu.x86_32.ebp    = sa->regs.ebp;
    fctx->cpu.x86_32.esi    = sa->regs.esi;
    fctx->cpu.x86_32.edi    = sa->regs.edi;
    fctx->cpu.x86_32.eip    = sa->regs.eip;
    fctx->cpu.x86_32.eflags = sa->regs.eflags;
    fctx->cpu.x86_32.cs     = sa->regs.cs;
    fctx->cpu.x86_32.ds     = sa->regs.ds;
    fctx->cpu.x86_32.es     = sa->regs.es;
    fctx->cpu.x86_32.fs     = sa->regs.fs;
    fctx->cpu.x86_32.gs     = sa->regs.gs;
    fctx->cpu.x86_32.ss     = sa->regs.ss;
    fctx->pc                = (uint64_t)sa->regs.eip;
}

/* Copy framework registers → standalone registers */
static void fctx_to_sa(cai_x86_32_ctx_t *sa, const cai_context_t *fctx)
{
    sa->regs.eax    = fctx->cpu.x86_32.eax;
    sa->regs.ecx    = fctx->cpu.x86_32.ecx;
    sa->regs.edx    = fctx->cpu.x86_32.edx;
    sa->regs.ebx    = fctx->cpu.x86_32.ebx;
    sa->regs.esp    = fctx->cpu.x86_32.esp;
    sa->regs.ebp    = fctx->cpu.x86_32.ebp;
    sa->regs.esi    = fctx->cpu.x86_32.esi;
    sa->regs.edi    = fctx->cpu.x86_32.edi;
    sa->regs.eip    = fctx->cpu.x86_32.eip;
    sa->regs.eflags = fctx->cpu.x86_32.eflags;
    sa->regs.cs     = fctx->cpu.x86_32.cs;
    sa->regs.ds     = fctx->cpu.x86_32.ds;
    sa->regs.es     = fctx->cpu.x86_32.es;
    sa->regs.fs     = fctx->cpu.x86_32.fs;
    sa->regs.gs     = fctx->cpu.x86_32.gs;
    sa->regs.ss     = fctx->cpu.x86_32.ss;
    sa->exit_code   = fctx->exit_code;
    sa->running     = fctx->running;
}

/*
 * Build a minimal cai_context_t that shares the standalone context's flat
 * memory pool.  The entire pool is registered as a single all-permissions
 * region so that cai_mem_* helpers work correctly.
 */
static void build_fctx(cai_context_t *fctx, const cai_x86_32_ctx_t *sa)
{
    memset(fctx, 0, sizeof(*fctx));

    fctx->target_arch = CAI_ARCH_X86_32;
    fctx->host_arch   = cai_host_arch();
    fctx->mem_base    = sa->mem;
    fctx->mem_size    = sa->mem_size;
    fctx->running     = sa->running;
    fctx->exit_code   = sa->exit_code;

    /* One flat region covering all of guest memory */
    fctx->regions[0].gva_base = (uint64_t)sa->mem_base;
    fctx->regions[0].host_ptr = sa->mem;
    fctx->regions[0].size     = sa->mem_size;
    fctx->regions[0].flags    = CAI_MEM_READ | CAI_MEM_WRITE | CAI_MEM_EXEC;
    fctx->n_regions           = 1;

    sa_to_fctx(fctx, sa);
}

/* -------------------------------------------------------------------------
 * cai_x86_32_create / cai_x86_32_destroy
 * ------------------------------------------------------------------------- */

cai_x86_32_ctx_t *cai_x86_32_create(size_t mem_size)
{
    cai_x86_32_ctx_t *ctx = (cai_x86_32_ctx_t *)kmalloc(sizeof(*ctx));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));

    ctx->mem = (uint8_t *)kmalloc(mem_size);
    if (!ctx->mem) {
        kfree(ctx);
        return NULL;
    }
    memset(ctx->mem, 0, mem_size);

    ctx->mem_size  = mem_size;
    ctx->mem_base  = 0;     /* Guest virtual base = 0 */
    ctx->running   = true;
    ctx->exit_code = 0;

    return ctx;
}

void cai_x86_32_destroy(cai_x86_32_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->mem) kfree(ctx->mem);
    kfree(ctx);
}

/* -------------------------------------------------------------------------
 * cai_x86_32_step_sa  (standalone context wrapper)
 * ------------------------------------------------------------------------- */

int cai_x86_32_step_sa(cai_x86_32_ctx_t *sa)
{
    if (!sa || !sa->running) return CAI_EXITED;

    cai_context_t fctx;
    build_fctx(&fctx, sa);

    int rc = cai_x86_32_step(&fctx);

    fctx_to_sa(sa, &fctx);
    return rc;
}

/* -------------------------------------------------------------------------
 * cai_x86_32_run  (standalone)
 * ------------------------------------------------------------------------- */

int cai_x86_32_run(cai_x86_32_ctx_t *sa, int max_steps)
{
    if (!sa) return CAI_EINVAL;

    cai_context_t fctx;
    build_fctx(&fctx, sa);

    int rc  = CAI_OK;
    int cnt = 0;

    while (fctx.running) {
        if (max_steps > 0 && cnt >= max_steps) {
            rc = CAI_EAGAIN;
            break;
        }
        rc = cai_x86_32_step(&fctx);
        if (rc == CAI_EXITED || rc < 0)
            break;
        cnt++;
    }

    fctx_to_sa(sa, &fctx);
    return rc;
}

/* -------------------------------------------------------------------------
 * cai_x86_32_load_elf  (standalone)
 *
 * Parses a statically-linked 32-bit ELF binary and maps its PT_LOAD
 * segments into the standalone context's flat memory pool.
 * Sets regs.eip to the ELF entry point and initialises esp to a simple
 * initial stack pointer at the top of guest memory.
 * ------------------------------------------------------------------------- */

int cai_x86_32_load_elf(cai_x86_32_ctx_t *sa,
                         const uint8_t *elf, size_t size)
{
    if (!sa || !elf || size < 52)   /* minimum size for a 32-bit ELF header */
        return CAI_EINVAL;

    /* Validate ELF magic */
    if (elf[0] != 0x7Fu || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
        return CAI_EINVAL;

    /* Must be 32-bit (ELFCLASS32 = 1) */
    if (elf[4] != 1)
        return CAI_EINVAL;

    /* Must be little-endian (ELFDATA2LSB = 1) */
    if (elf[5] != 1)
        return CAI_EINVAL;

/* Safe little-endian field readers */
#define ELF_LE16(p) ((uint16_t)((p)[0] | ((uint16_t)(p)[1] << 8)))
#define ELF_LE32(p) ((uint32_t)((p)[0] | ((uint32_t)(p)[1] << 8) | \
                      ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24)))

    uint16_t e_type      = ELF_LE16(elf + 16);
    uint16_t e_machine   = ELF_LE16(elf + 18);
    uint32_t e_entry     = ELF_LE32(elf + 24);
    uint32_t e_phoff     = ELF_LE32(elf + 28);
    uint16_t e_phentsize = ELF_LE16(elf + 42);
    uint16_t e_phnum     = ELF_LE16(elf + 44);

    /* ET_EXEC (2) or ET_DYN (3), EM_386 (3) */
    if ((e_type != 2 && e_type != 3) || e_machine != 3)
        return CAI_EINVAL;

    if (e_phoff == 0 || e_phnum == 0 || e_phentsize < 32)
        return CAI_EINVAL;

    /* Walk PT_LOAD program headers */
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (uint32_t)i * e_phentsize;

        /* Bounds check */
        if ((size_t)(ph - elf) + e_phentsize > size)
            return CAI_EINVAL;

        uint32_t p_type   = ELF_LE32(ph +  0);
        uint32_t p_offset = ELF_LE32(ph +  4);
        uint32_t p_vaddr  = ELF_LE32(ph +  8);
        uint32_t p_filesz = ELF_LE32(ph + 16);
        uint32_t p_memsz  = ELF_LE32(ph + 20);

        if (p_type != 1 /* PT_LOAD */)
            continue;

        /* Map vaddr relative to mem_base into the flat buffer */
        if ((uint64_t)p_vaddr < (uint64_t)sa->mem_base)
            return CAI_EFAULT;

        uint64_t offset_in_pool = (uint64_t)p_vaddr - (uint64_t)sa->mem_base;

        if (offset_in_pool + p_memsz > sa->mem_size)
            return CAI_ENOMEM;

        /* Zero the full in-memory extent (handles .bss) */
        memset(sa->mem + offset_in_pool, 0, p_memsz);

        /* Copy file image */
        if (p_filesz > 0) {
            if ((uint64_t)p_offset + p_filesz > size)
                return CAI_EINVAL;
            memcpy(sa->mem + offset_in_pool, elf + p_offset, p_filesz);
        }
    }

#undef ELF_LE16
#undef ELF_LE32

    /* Set entry point */
    sa->regs.eip = e_entry;

    /* Place initial stack at the top of guest memory, aligned to 16 bytes */
    uint32_t stack_top = sa->mem_base + (uint32_t)sa->mem_size - 16u;
    stack_top &= ~15u;
    sa->regs.esp = stack_top;

    sa->running   = true;
    sa->exit_code = 0;

    return CAI_OK;
}

/* -------------------------------------------------------------------------
 * Standalone direct memory accessors
 * ------------------------------------------------------------------------- */

static inline uint32_t sa_off(const cai_x86_32_ctx_t *sa, uint32_t addr)
{
    return addr - sa->mem_base;
}

uint8_t cai_x86_32_read8(cai_x86_32_ctx_t *sa, uint32_t addr)
{
    uint32_t off = sa_off(sa, addr);
    if ((uint64_t)off >= sa->mem_size) return 0;
    return sa->mem[off];
}

uint16_t cai_x86_32_read16(cai_x86_32_ctx_t *sa, uint32_t addr)
{
    uint32_t off = sa_off(sa, addr);
    if ((uint64_t)off + 1 >= sa->mem_size) return 0;
    return (uint16_t)sa->mem[off] | ((uint16_t)sa->mem[off + 1] << 8);
}

uint32_t cai_x86_32_read32(cai_x86_32_ctx_t *sa, uint32_t addr)
{
    uint32_t off = sa_off(sa, addr);
    if ((uint64_t)off + 3 >= sa->mem_size) return 0;
    return (uint32_t)sa->mem[off]
         | ((uint32_t)sa->mem[off + 1] <<  8)
         | ((uint32_t)sa->mem[off + 2] << 16)
         | ((uint32_t)sa->mem[off + 3] << 24);
}

void cai_x86_32_write8(cai_x86_32_ctx_t *sa, uint32_t addr, uint8_t val)
{
    uint32_t off = sa_off(sa, addr);
    if ((uint64_t)off >= sa->mem_size) return;
    sa->mem[off] = val;
}

void cai_x86_32_write16(cai_x86_32_ctx_t *sa, uint32_t addr, uint16_t val)
{
    uint32_t off = sa_off(sa, addr);
    if ((uint64_t)off + 1 >= sa->mem_size) return;
    sa->mem[off]     = (uint8_t)val;
    sa->mem[off + 1] = (uint8_t)(val >> 8);
}

void cai_x86_32_write32(cai_x86_32_ctx_t *sa, uint32_t addr, uint32_t val)
{
    uint32_t off = sa_off(sa, addr);
    if ((uint64_t)off + 3 >= sa->mem_size) return;
    sa->mem[off]     = (uint8_t)val;
    sa->mem[off + 1] = (uint8_t)(val >>  8);
    sa->mem[off + 2] = (uint8_t)(val >> 16);
    sa->mem[off + 3] = (uint8_t)(val >> 24);
}

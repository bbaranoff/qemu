/*
 * calypso_c54x.c — TMS320C54x DSP emulator for Calypso
 *
 * Minimal C54x core: enough to run the Calypso DSP ROM for GSM
 * signal processing (Viterbi, deinterleaving, burst decode).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "calypso_c54x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C54_LOG(fmt, ...) \
    fprintf(stderr, "[c54x] " fmt "\n", ##__VA_ARGS__)

/* ================================================================
 * Helpers
 * ================================================================ */

/* Sign-extend 40-bit accumulator */
static inline int64_t sext40(int64_t v)
{
    if (v & ((int64_t)1 << 39))
        v |= ~(((int64_t)1 << 40) - 1);
    else
        v &= ((int64_t)1 << 40) - 1;
    return v;
}

/* Saturate 40-bit to 32-bit (OVM mode) */
static inline int64_t sat32(int64_t v)
{
    if (v > 0x7FFFFFFF) return 0x7FFFFFFF;
    if (v < (int64_t)(int32_t)0x80000000) return (int64_t)(int32_t)0x80000000;
    return v;
}

/* Get ARP from ST0 */
static inline int arp(C54xState *s)
{
    return (s->st0 >> ST0_ARP_SHIFT) & 7;
}

/* Get DP from ST0 */
static inline uint16_t dp(C54xState *s)
{
    return s->st0 & ST0_DP_MASK;
}

/* Get ASM from ST1 (5-bit signed) */
static inline int asm_shift(C54xState *s)
{
    int v = s->st1 & ST1_ASM_MASK;
    if (v & 0x10) v |= ~0x1F;  /* sign extend */
    return v;
}

/* ================================================================
 * Memory access
 * ================================================================ */

static uint16_t data_read(C54xState *s, uint16_t addr)
{
    /* MMR region */
    if (addr < 0x20) {
        switch (addr) {
        case MMR_IMR:  return s->imr;
        case MMR_IFR:  return s->ifr;
        case MMR_ST0:  return s->st0;
        case MMR_ST1:  return s->st1;
        case MMR_AL:   return (uint16_t)(s->a & 0xFFFF);
        case MMR_AH:   return (uint16_t)((s->a >> 16) & 0xFFFF);
        case MMR_AG:   return (uint16_t)((s->a >> 32) & 0xFF);
        case MMR_BL:   return (uint16_t)(s->b & 0xFFFF);
        case MMR_BH:   return (uint16_t)((s->b >> 16) & 0xFFFF);
        case MMR_BG:   return (uint16_t)((s->b >> 32) & 0xFF);
        case MMR_T:    return s->t;
        case MMR_TRN:  return s->trn;
        case MMR_AR0: case MMR_AR1: case MMR_AR2: case MMR_AR3:
        case MMR_AR4: case MMR_AR5: case MMR_AR6: case MMR_AR7:
            return s->ar[addr - MMR_AR0];
        case MMR_SP:   return s->sp;
        case MMR_BK:   return s->bk;
        case MMR_BRC:  return s->brc;
        case MMR_RSA:  return s->rsa;
        case MMR_REA:  return s->rea;
        case MMR_PMST: return s->pmst;
        case MMR_XPC:  return s->xpc;
        default: return 0;
        }
    }

    /* API RAM (shared with ARM) */
    if (addr >= C54X_API_BASE && addr < C54X_API_BASE + C54X_API_SIZE) {
        if (s->api_ram)
            return s->api_ram[addr - C54X_API_BASE];
    }

    return s->data[addr];
}

static void data_write(C54xState *s, uint16_t addr, uint16_t val)
{
    /* MMR region */
    if (addr < 0x20) {
        switch (addr) {
        case MMR_IMR:  s->imr = val; return;
        case MMR_IFR:  s->ifr &= ~val; return;  /* write 1 to clear */
        case MMR_ST0:  s->st0 = val; return;
        case MMR_ST1:  s->st1 = val; return;
        case MMR_AL:   s->a = (s->a & ~0xFFFF) | val; return;
        case MMR_AH:   s->a = (s->a & ~((int64_t)0xFFFF << 16)) | ((int64_t)val << 16); return;
        case MMR_AG:   s->a = (s->a & 0xFFFFFFFF) | ((int64_t)(val & 0xFF) << 32); return;
        case MMR_BL:   s->b = (s->b & ~0xFFFF) | val; return;
        case MMR_BH:   s->b = (s->b & ~((int64_t)0xFFFF << 16)) | ((int64_t)val << 16); return;
        case MMR_BG:   s->b = (s->b & 0xFFFFFFFF) | ((int64_t)(val & 0xFF) << 32); return;
        case MMR_T:    s->t = val; return;
        case MMR_TRN:  s->trn = val; return;
        case MMR_AR0: case MMR_AR1: case MMR_AR2: case MMR_AR3:
        case MMR_AR4: case MMR_AR5: case MMR_AR6: case MMR_AR7:
            s->ar[addr - MMR_AR0] = val; return;
        case MMR_SP:   s->sp = val; return;
        case MMR_BK:   s->bk = val; return;
        case MMR_BRC:  s->brc = val; return;
        case MMR_RSA:  s->rsa = val; return;
        case MMR_REA:  s->rea = val; return;
        case MMR_PMST: s->pmst = val; return;
        case MMR_XPC:  s->xpc = val; return;
        default: return;
        }
    }

    /* API RAM (shared with ARM) */
    if (addr >= C54X_API_BASE && addr < C54X_API_BASE + C54X_API_SIZE) {
        if (s->api_ram)
            s->api_ram[addr - C54X_API_BASE] = val;
    }

    s->data[addr] = val;
}

static uint16_t prog_read(C54xState *s, uint32_t addr)
{
    addr &= (C54X_PROG_SIZE - 1);
    /* OVLY: DARAM also visible in program space */
    if ((s->pmst & PMST_OVLY) && addr < 0x8000 && addr >= 0x80)
        return s->data[addr];
    return s->prog[addr];
}

static void __attribute__((unused)) prog_write(C54xState *s, uint32_t addr, uint16_t val)
{
    addr &= (C54X_PROG_SIZE - 1);
    if ((s->pmst & PMST_OVLY) && addr < 0x8000 && addr >= 0x80)
        s->data[addr] = val;
    s->prog[addr] = val;
}

/* ================================================================
 * Addressing mode helpers
 * ================================================================ */

/* Resolve Smem operand: direct or indirect addressing.
 * Returns the data memory address. */
static uint16_t resolve_smem(C54xState *s, uint16_t opcode, bool *indirect)
{
    if (opcode & 0x80) {
        /* Indirect addressing */
        *indirect = true;
        int mod = (opcode >> 3) & 0x0F;
        int nar = opcode & 0x07;
        int cur_arp = arp(s);
        uint16_t addr = s->ar[cur_arp];

        /* Post-modify */
        switch (mod) {
        case 0x0: /* *ARn */
            break;
        case 0x1: /* *ARn- */
            s->ar[cur_arp]--;
            break;
        case 0x2: /* *ARn+ */
            s->ar[cur_arp]++;
            break;
        case 0x3: /* *+ARn */
            addr = ++s->ar[cur_arp];
            break;
        case 0x4: /* *ARn-0 */
            s->ar[cur_arp] -= s->ar[0];
            break;
        case 0x5: /* *ARn+0 */
            s->ar[cur_arp] += s->ar[0];
            break;
        case 0x6: /* *ARn-0B (bit-reversed) */
            /* Simplified: just subtract */
            s->ar[cur_arp] -= s->ar[0];
            break;
        case 0x7: /* *ARn+0B (bit-reversed) */
            s->ar[cur_arp] += s->ar[0];
            break;
        case 0x8: /* *ARn-% (circular) */
            if (s->bk == 0) s->ar[cur_arp]--;
            else {
                uint16_t base = s->ar[cur_arp] - (s->ar[cur_arp] % s->bk);
                s->ar[cur_arp]--;
                if (s->ar[cur_arp] < base) s->ar[cur_arp] = base + s->bk - 1;
            }
            break;
        case 0x9: /* *ARn+% (circular) */
            if (s->bk == 0) s->ar[cur_arp]++;
            else {
                uint16_t base = s->ar[cur_arp] - (s->ar[cur_arp] % s->bk);
                s->ar[cur_arp]++;
                if (s->ar[cur_arp] >= base + s->bk) s->ar[cur_arp] = base;
            }
            break;
        case 0xA: /* *ARn-0% */
            s->ar[cur_arp] -= s->ar[0];
            break;
        case 0xB: /* *ARn+0% */
            s->ar[cur_arp] += s->ar[0];
            break;
        case 0xC: /* *(lk) — next word is address */
            /* handled by caller */
            break;
        case 0xD: /* *+ARn(lk) */
            /* handled by caller */
            break;
        case 0xE: /* *ARn(lk) */
            /* handled by caller */
            break;
        case 0xF: /* *+ARn(lk)% */
            /* handled by caller */
            break;
        }

        /* Update ARP */
        s->st0 = (s->st0 & ~ST0_ARP_MASK) | (nar << ST0_ARP_SHIFT);

        return addr;
    } else {
        /* Direct addressing: DP:offset */
        *indirect = false;
        uint16_t offset = opcode & 0x7F;
        return (dp(s) << 7) | offset;
    }
}

/* ================================================================
 * Instruction execution
 * ================================================================ */

/* Execute one instruction. Returns number of words consumed (1 or 2). */
static int c54x_exec_one(C54xState *s)
{
    uint16_t op = prog_read(s, s->pc);
    uint16_t op2;
    bool ind;
    uint16_t addr;
    int consumed = 1;

    uint8_t hi4 = (op >> 12) & 0xF;
    uint8_t hi8 = (op >> 8) & 0xFF;

    switch (hi4) {
    case 0xF:
        /* 0xF --- large group: branches, misc, short immediates */
        if (hi8 == 0xF4) {
            /* F4xx: unconditional branch/call */
            uint8_t sub = (op >> 4) & 0xF;
            op2 = prog_read(s, s->pc + 1);
            consumed = 2;
            switch (sub) {
            case 0x0: /* B pmad */
                s->pc = op2;
                return 0; /* don't increment PC */
            case 0x6: /* CALL pmad */
                s->sp--;
                data_write(s, s->sp, (uint16_t)(s->pc + 2));
                s->pc = op2;
                return 0;
            case 0xE: /* BD pmad (delayed branch) */
                /* Execute 2 more instructions then branch */
                /* Simplified: just branch */
                s->pc = op2;
                return 0;
            case 0x8: /* CALLD pmad */
                s->sp--;
                data_write(s, s->sp, (uint16_t)(s->pc + 2));
                s->pc = op2;
                return 0;
            default:
                goto unimpl;
            }
        }
        if (hi8 == 0xF0) {
            /* F07x: RET variants */
            uint8_t sub = op & 0xFF;
            switch (sub) {
            case 0x73: /* NOP (actually F073 = RET in some encodings) */
                /* Check: F073 could be FRET or other */
                {
                    uint16_t ret_addr = data_read(s, s->sp);
                    s->sp++;
                    s->pc = ret_addr;
                    return 0;
                }
            default:
                break;
            }
        }
        if (op == 0xF495) {
            /* NOP */
            return consumed;
        }
        if (op == 0xF4E4) {
            /* IDLE */
            s->idle = true;
            return consumed;
        }
        /* FXXX short immediates and misc */
        if (hi8 == 0xF0 || hi8 == 0xF1) {
            /* F0xx/F1xx: various - SSBX, RSBX, etc. */
            if ((op & 0xFFF0) == 0xF070) {
                /* F07x: misc control */
                uint8_t sub = op & 0x0F;
                if (sub == 0x3) {
                    /* RET */
                    uint16_t ret_addr = data_read(s, s->sp);
                    s->sp++;
                    s->pc = ret_addr;
                    return 0;
                }
                if (sub == 0x4) {
                    /* RETE (return from interrupt) */
                    uint16_t ret_addr = data_read(s, s->sp);
                    s->sp++;
                    s->st1 &= ~ST1_INTM;  /* re-enable interrupts */
                    s->pc = ret_addr;
                    return 0;
                }
                goto unimpl;
            }
            /* F0Bx / F1Bx: RSBX/SSBX */
            if ((op & 0xFE00) == 0xF000 && (op & 0x00F0) == 0x00B0) {
                /* RSBX/SSBX bit in ST0/ST1 */
                int bit = op & 0x0F;
                int set = (op >> 8) & 1;
                int st = (op >> 5) & 1;  /* 0=ST0, 1=ST1 */
                if (st == 0) {
                    if (set) s->st0 |= (1 << bit);
                    else     s->st0 &= ~(1 << bit);
                } else {
                    if (set) s->st1 |= (1 << bit);
                    else     s->st1 &= ~(1 << bit);
                }
                return consumed;
            }
        }
        /* F8xx: RPT */
        if (hi8 == 0xF8) {
            uint8_t sub = (op >> 4) & 0xF;
            if (sub == 0x2) {
                /* F82x: RPTB pmad */
                op2 = prog_read(s, s->pc + 1);
                consumed = 2;
                s->rea = op2;
                s->rsa = (uint16_t)(s->pc + 2);
                s->rptb_active = true;
                s->st1 |= ST1_BRAF;
                return consumed;
            }
            if (sub == 0x3) {
                /* F83x: RPT #k (short) */
                /* Or other F83x variants */
                op2 = prog_read(s, s->pc + 1);
                consumed = 2;
                s->rpt_count = op2;
                s->rpt_pc = (uint16_t)(s->pc + 2);
                s->rpt_active = true;
                return consumed;
            }
            /* F8xx Smem: RPT Smem */
            addr = resolve_smem(s, op, &ind);
            s->rpt_count = data_read(s, addr);
            s->rpt_pc = (uint16_t)(s->pc + consumed);
            s->rpt_active = true;
            return consumed;
        }
        /* FB/FC: short LD #k */
        if (hi8 >= 0xF8) {
            /* Various short forms */
            goto unimpl;
        }
        goto unimpl;

    case 0xE:
        /* Exxxx: single-word ALU, status, misc */
        if (hi8 == 0xEA) {
            /* BANZ pmad, *ARn- */
            op2 = prog_read(s, s->pc + 1);
            consumed = 2;
            int n = arp(s);
            if (s->ar[n] != 0) {
                s->ar[n]--;
                s->pc = op2;
                return 0;
            }
            s->ar[n]--;
            return consumed;
        }
        if (hi8 == 0xEC) {
            /* EC: BC pmad, cond (conditional branch, 2 words) */
            op2 = prog_read(s, s->pc + 1);
            consumed = 2;
            /* Simplified: evaluate common conditions */
            uint8_t cond = op & 0xFF;
            bool take = false;
            /* Condition evaluation (simplified) */
            if (cond == 0x03) take = (s->a == 0);        /* AEQ */
            else if (cond == 0x0B) take = (s->b == 0);   /* BEQ */
            else if (cond == 0x02) take = (s->a != 0);   /* ANEQ */
            else if (cond == 0x0A) take = (s->b != 0);   /* BNEQ */
            else if (cond == 0x00) take = (s->a < 0);    /* ALT */
            else if (cond == 0x08) take = (s->b < 0);    /* BLT */
            else if (cond == 0x04) take = (s->a > 0);    /* AGT */
            else if (cond == 0x0C) take = (s->b > 0);    /* BGT */
            else if (cond == 0x01) take = (s->a >= 0);   /* AGEQ */
            else if (cond == 0x09) take = (s->b >= 0);   /* BGEQ */
            else if (cond == 0x05) take = (s->a <= 0);   /* ALEQ */
            else if (cond == 0x0D) take = (s->b <= 0);   /* BLEQ */
            else if (cond == 0x40) take = (s->st0 & ST0_TC);   /* TC */
            else if (cond == 0x41) take = !(s->st0 & ST0_TC);  /* NTC */
            else if (cond == 0x20) take = (s->st0 & ST0_C);    /* C */
            else if (cond == 0x21) take = !(s->st0 & ST0_C);   /* NC */
            else take = true;  /* unknown cond: take (safer than skip) */

            if (take) { s->pc = op2; return 0; }
            return consumed;
        }
        if (hi8 == 0xE5) {
            /* E5xx: MVMM ARx, ARy or similar */
            int src = (op >> 4) & 0xF;
            int dst = op & 0xF;
            /* Map src/dst to register: 0-7=AR0-AR7, 8=SP */
            uint16_t val;
            if (src >= 0x10 && src <= 0x17) val = s->ar[src - 0x10];
            else if (src == 0x18) val = s->sp;
            else val = 0;
            if (dst >= 0x10 && dst <= 0x17) s->ar[dst - 0x10] = val;
            else if (dst == 0x18) s->sp = val;
            return consumed;
        }
        goto unimpl;

    case 0x6: case 0x7:
        /* LD / ST operations */
        if ((op & 0xF800) == 0x7000) {
            /* 70xx: STL src, Smem */
            int src_acc = (op >> 9) & 1;
            addr = resolve_smem(s, op, &ind);
            int64_t acc = src_acc ? s->b : s->a;
            data_write(s, addr, (uint16_t)(acc & 0xFFFF));
            return consumed;
        }
        if ((op & 0xF800) == 0x7800) {
            /* 78xx: STH src, Smem */
            int src_acc = (op >> 9) & 1;
            addr = resolve_smem(s, op, &ind);
            int64_t acc = src_acc ? s->b : s->a;
            data_write(s, addr, (uint16_t)((acc >> 16) & 0xFFFF));
            return consumed;
        }
        if ((op & 0xF800) == 0x6000) {
            /* 60xx: LD Smem, dst */
            int dst_acc = (op >> 9) & 1;
            int shift = (op >> 8) & 1;
            addr = resolve_smem(s, op, &ind);
            uint16_t val = data_read(s, addr);
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)val : val;
            if (shift) v <<= 16;  /* LD Smem, 16, dst */
            if (dst_acc) s->b = sext40(v); else s->a = sext40(v);
            return consumed;
        }
        if ((op & 0xF800) == 0x6800) {
            /* 68xx: LD Smem, T */
            addr = resolve_smem(s, op, &ind);
            s->t = data_read(s, addr);
            return consumed;
        }
        goto unimpl;

    case 0x1:
        /* 1xxx: SUB variants */
        addr = resolve_smem(s, op, &ind);
        {
            int dst = (op >> 8) & 1;
            uint16_t val = data_read(s, addr);
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)val : val;
            v <<= 16;
            if (dst) s->b = sext40(s->b - v);
            else     s->a = sext40(s->a - v);
        }
        return consumed;

    case 0x0:
        /* 0xxx: ADD variants */
        addr = resolve_smem(s, op, &ind);
        {
            int dst = (op >> 8) & 1;
            uint16_t val = data_read(s, addr);
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)val : val;
            v <<= 16;
            if (dst) s->b = sext40(s->b + v);
            else     s->a = sext40(s->a + v);
        }
        return consumed;

    case 0x3:
        /* 3xxx: MAC / MAS */
        addr = resolve_smem(s, op, &ind);
        {
            int dst = (op >> 8) & 1;
            uint16_t val = data_read(s, addr);
            int64_t product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
            if (s->st1 & ST1_FRCT) product <<= 1;
            if (dst) s->b = sext40(s->b + product);
            else     s->a = sext40(s->a + product);
        }
        return consumed;

    case 0x2:
        /* 2xxx: MPY, SQUR, etc. */
        if ((op & 0xFC00) == 0x2000) {
            /* MPY Smem, dst */
            addr = resolve_smem(s, op, &ind);
            uint16_t val = data_read(s, addr);
            int64_t product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
            if (s->st1 & ST1_FRCT) product <<= 1;
            int dst = (op >> 8) & 1;
            if (dst) s->b = sext40(product);
            else     s->a = sext40(product);
            return consumed;
        }
        goto unimpl;

    case 0x4:
        /* 4xxx: AND, OR, XOR */
        addr = resolve_smem(s, op, &ind);
        {
            int sub = (op >> 8) & 0xF;
            uint16_t val = data_read(s, addr);
            switch (sub & 0x3) {
            case 0: /* AND */
                if (sub & 4) s->b = (s->b & 0xFFFF0000) | (((uint16_t)s->b) & val);
                else         s->a = (s->a & 0xFFFF0000) | (((uint16_t)s->a) & val);
                break;
            case 1: /* OR */
                if (sub & 4) s->b |= val;
                else         s->a |= val;
                break;
            case 2: /* XOR */
                if (sub & 4) s->b ^= val;
                else         s->a ^= val;
                break;
            }
        }
        return consumed;

    case 0x5:
        /* 5xxx: shifts */
        if ((op & 0xFC00) == 0x5000) {
            /* SFTA src, shift */
            int dst = (op >> 8) & 1;
            int shift = asm_shift(s);
            int64_t *acc = dst ? &s->b : &s->a;
            if (shift >= 0) *acc = sext40(*acc << shift);
            else            *acc = sext40(*acc >> (-shift));
            return consumed;
        }
        goto unimpl;

    case 0x8: case 0x9:
        /* Memory moves: MVDK, MVKD, MVDD, etc. */
        if (hi8 == 0x8A) {
            /* MVDK Smem, dmad */
            addr = resolve_smem(s, op, &ind);
            op2 = prog_read(s, s->pc + 1);
            consumed = 2;
            data_write(s, op2, data_read(s, addr));
            return consumed;
        }
        if (hi8 == 0x9A) {
            /* MVKD dmad, Smem */
            addr = resolve_smem(s, op, &ind);
            op2 = prog_read(s, s->pc + 1);
            consumed = 2;
            data_write(s, addr, data_read(s, op2));
            return consumed;
        }
        goto unimpl;

    case 0xA: case 0xB:
        /* Axx: STLM, LDMM, PSHM, POPM, etc. */
        if (hi8 == 0xAA) {
            /* STLM src, MMR */
            int src_acc = (op >> 4) & 1;
            uint16_t mmr = op & 0x1F;
            int64_t acc = src_acc ? s->b : s->a;
            data_write(s, mmr, (uint16_t)(acc & 0xFFFF));
            return consumed;
        }
        if (hi8 == 0xBA) {
            /* LDMM MMR, dst */
            uint16_t mmr = op & 0x1F;
            int dst = (op >> 4) & 1;
            int64_t v = (int64_t)(int16_t)data_read(s, mmr);
            if (dst) s->b = sext40(v << 16);
            else     s->a = sext40(v << 16);
            return consumed;
        }
        goto unimpl;

    case 0xC: case 0xD:
        /* C/Dxxx: PSHM, POPM, RPT, FRAME, etc. */
        if (hi8 == 0xC5) {
            /* PSHM MMR */
            uint16_t mmr = op & 0x1F;
            s->sp--;
            data_write(s, s->sp, data_read(s, mmr));
            return consumed;
        }
        if (hi8 == 0xCD) {
            /* POPM MMR */
            uint16_t mmr = op & 0x1F;
            data_write(s, mmr, data_read(s, s->sp));
            s->sp++;
            return consumed;
        }
        if (hi8 == 0xCE) {
            /* FRAME #k (signed 8-bit) */
            int8_t k = (int8_t)(op & 0xFF);
            s->sp += k;
            return consumed;
        }
        goto unimpl;

    default:
        break;
    }

unimpl:
    s->unimpl_count++;
    if (s->unimpl_count <= 20 || op != s->last_unimpl) {
        C54_LOG("UNIMPL @0x%04x: 0x%04x (hi8=0x%02x) [#%u]",
                s->pc, op, hi8, s->unimpl_count);
        s->last_unimpl = op;
    }
    return consumed;
}

/* ================================================================
 * Main execution loop
 * ================================================================ */

int c54x_run(C54xState *s, int n_insns)
{
    int executed = 0;

    while (executed < n_insns && s->running && !s->idle) {
        /* Check RPTB (block repeat) */
        if (s->rptb_active && s->pc == s->rea + 1) {
            if (s->brc > 0) {
                s->brc--;
                s->pc = s->rsa;
            } else {
                s->rptb_active = false;
                s->st1 &= ~ST1_BRAF;
            }
        }

        /* Execute instruction */
        int consumed;
        if (s->rpt_active) {
            /* RPT: repeat single instruction */
            consumed = c54x_exec_one(s);
            if (s->rpt_count > 0) {
                s->rpt_count--;
                /* Don't advance PC — re-execute same instruction */
                s->cycles++;
                executed++;
                continue;
            } else {
                s->rpt_active = false;
            }
        } else {
            consumed = c54x_exec_one(s);
        }

        if (consumed > 0)
            s->pc += consumed;
        /* consumed == 0 means PC was set by branch */

        s->cycles++;
        executed++;
    }

    s->insn_count += executed;
    return executed;
}

/* ================================================================
 * ROM loader
 * ================================================================ */

int c54x_load_rom(C54xState *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        C54_LOG("Cannot open ROM dump: %s", path);
        return -1;
    }

    char line[1024];
    int section = -1; /* 0=regs, 1=DROM, 2=PDROM, 3-6=PROM0-3 */
    
    int total_words = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Section headers */
        if (strstr(line, "DSP dump: Registers"))  { section = 0; continue; }
        if (strstr(line, "DSP dump: DROM"))        { section = 1; continue; }
        if (strstr(line, "DSP dump: PDROM"))       { section = 2; continue; }
        if (strstr(line, "DSP dump: PROM0"))       { section = 3; continue; }
        if (strstr(line, "DSP dump: PROM1"))       { section = 4; continue; }
        if (strstr(line, "DSP dump: PROM2"))       { section = 5; continue; }
        if (strstr(line, "DSP dump: PROM3"))       { section = 6; continue; }
        if (section < 0) continue;

        /* Parse data lines: "ADDR : XXXX XXXX XXXX ..." */
        uint32_t addr;
        if (sscanf(line, "%x :", &addr) != 1) continue;

        char *p = strchr(line, ':');
        if (!p) continue;
        p++;

        uint16_t word;
        while (sscanf(p, " %hx%n", &word, (int[]){0}) == 1) {
            int n;
            sscanf(p, " %hx%n", &word, &n);
            p += n;

            if (section == 0) {
                /* Registers: store in data memory */
                if (addr < 0x60) s->data[addr] = word;
            } else if (section == 1 || section == 2) {
                /* DROM/PDROM: data memory */
                if (addr < C54X_DATA_SIZE) s->data[addr] = word;
            } else {
                /* PROM: program memory */
                if (addr < C54X_PROG_SIZE) s->prog[addr] = word;
            }
            addr++;
            total_words++;
        }
    }

    fclose(f);
    C54_LOG("Loaded ROM: %d words from %s", total_words, path);
    return 0;
}

/* ================================================================
 * Init / Reset / Interrupts
 * ================================================================ */

C54xState *c54x_init(void)
{
    C54xState *s = calloc(1, sizeof(C54xState));
    if (!s) return NULL;
    return s;
}

void c54x_set_api_ram(C54xState *s, uint16_t *api_ram)
{
    s->api_ram = api_ram;
}

void c54x_reset(C54xState *s)
{
    s->a = 0; s->b = 0;
    memset(s->ar, 0, sizeof(s->ar));
    s->t = 0; s->trn = 0;
    s->sp = 0; s->bk = 0;
    s->brc = 0; s->rsa = 0; s->rea = 0;
    s->st0 = 0;
    s->st1 = ST1_INTM;  /* interrupts disabled at reset */
    s->pmst = 0xFFE0;   /* IPTR = 0x1FF (reset vector at 0xFF80) */
    s->imr = 0;
    s->ifr = 0;
    s->xpc = 0;
    s->rpt_active = false;
    s->rptb_active = false;
    s->idle = false;
    s->running = true;
    s->cycles = 0;
    s->insn_count = 0;
    s->unimpl_count = 0;

    /* Reset vector: IPTR * 0x80 */
    uint16_t iptr = (s->pmst >> PMST_IPTR_SHIFT) & 0x1FF;
    s->pc = iptr * 0x80;  /* 0xFF80 for default PMST */

    C54_LOG("Reset: PC=0x%04x PMST=0x%04x SP=0x%04x", s->pc, s->pmst, s->sp);
}

void c54x_interrupt(C54xState *s, int irq)
{
    if (irq < 0 || irq >= C54X_NUM_INTS) return;
    s->ifr |= (1 << irq);

    /* If not masked and interrupts enabled, take it */
    if (!(s->st1 & ST1_INTM) && (s->imr & (1 << irq))) {
        s->ifr &= ~(1 << irq);

        /* Push PC, set INTM */
        s->sp--;
        data_write(s, s->sp, (uint16_t)s->pc);
        s->st1 |= ST1_INTM;

        /* Jump to vector */
        uint16_t iptr = (s->pmst >> PMST_IPTR_SHIFT) & 0x1FF;
        s->pc = (iptr * 0x80) + irq * 2;

        /* Wake from IDLE */
        s->idle = false;
    }
}

void c54x_wake(C54xState *s)
{
    s->idle = false;
}

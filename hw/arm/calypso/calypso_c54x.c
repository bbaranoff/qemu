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

static inline int64_t sext40(int64_t v)
{
    if (v & ((int64_t)1 << 39))
        v |= ~(((int64_t)1 << 40) - 1);
    else
        v &= ((int64_t)1 << 40) - 1;
    return v;
}

static inline int64_t sat32(int64_t v)
{
    if (v > 0x7FFFFFFF) return 0x7FFFFFFF;
    if (v < (int64_t)(int32_t)0x80000000) return (int64_t)(int32_t)0x80000000;
    return v;
}

static inline int arp(C54xState *s)
{
    return (s->st0 >> ST0_ARP_SHIFT) & 7;
}

static inline uint16_t dp(C54xState *s)
{
    return s->st0 & ST0_DP_MASK;
}

static inline int asm_shift(C54xState *s)
{
    int v = s->st1 & ST1_ASM_MASK;
    if (v & 0x10) v |= ~0x1F;
    return v;
}

/* ================================================================
 * Memory access
 * ================================================================ */

static uint16_t prog_read(C54xState *s, uint32_t addr);

static uint16_t data_read(C54xState *s, uint16_t addr)
{
    if (addr == 0x08D4) {
        static int dsp_page_log = 0;
        if (dsp_page_log < 50) {
            C54_LOG("d_dsp_page RD = 0x%04x PC=0x%04x insn=%u SP=0x%04x",
                    s->api_ram ? s->api_ram[addr - 0x0800] : s->data[addr],
                    s->pc, s->insn_count, s->sp);
            dsp_page_log++;
        }
    }
    if (addr == TIM_ADDR) return s->data[TIM_ADDR];
    if (addr == PRD_ADDR) return s->data[PRD_ADDR];
    if (addr == TCR_ADDR) {
        uint16_t tcr = s->data[TCR_ADDR] & ~TCR_PSC_MASK;
        tcr |= (s->timer_psc & 0xF) << TCR_PSC_SHIFT;
        return tcr;
    }

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

    if (addr >= C54X_API_BASE && addr < C54X_API_BASE + C54X_API_SIZE) {
        if (s->api_ram) {
            return s->api_ram[addr - C54X_API_BASE];
        }
    }

    return s->data[addr];
}

static void data_write(C54xState *s, uint16_t addr, uint16_t val)
{
    if (addr == TCR_ADDR) {
        if (val & TCR_TRB) {
            s->data[TIM_ADDR] = s->data[PRD_ADDR];
            s->timer_psc = val & TCR_TDDR_MASK;
        }
        s->data[TCR_ADDR] = val & ~TCR_TRB;
        return;
    }
    if (addr == TIM_ADDR) { s->data[TIM_ADDR] = val; return; }
    if (addr == PRD_ADDR) { s->data[PRD_ADDR] = val; return; }

    if (addr < 0x20) {
        switch (addr) {
        case MMR_IMR:
            if (val != s->imr)
                C54_LOG("IMR change 0x%04x -> 0x%04x PC=0x%04x", s->imr, val, s->pc);
            s->imr = val; return;
        case MMR_IFR:  s->ifr &= ~val; return;
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
        case MMR_PMST:
            if (val != s->pmst)
                C54_LOG("PMST change 0x%04x -> 0x%04x (IPTR=0x%03x OVLY=%d) PC=0x%04x",
                        s->pmst, val, (val >> PMST_IPTR_SHIFT) & 0x1FF, !!(val & PMST_OVLY), s->pc);
            s->pmst = val; return;
        case MMR_XPC:
            if (val > 3) val &= 3;
            s->xpc = val; return;
        default: return;
        }
    }

    if (addr >= C54X_API_BASE && addr < C54X_API_BASE + C54X_API_SIZE) {
        if (s->api_ram)
            s->api_ram[addr - C54X_API_BASE] = val;
    }

    s->data[addr] = val;
}

static uint16_t prog_read(C54xState *s, uint32_t addr)
{
    uint16_t addr16 = addr & 0xFFFF;
    if ((s->pmst & PMST_OVLY) && addr16 < 0x8000 && addr16 >= 0x80)
        return s->data[addr16];
    if (addr16 >= 0x8000) {
        uint32_t ext = ((uint32_t)s->xpc << 16) | addr16;
        ext &= (C54X_PROG_SIZE - 1);
        return s->prog[ext];
    }
    return s->prog[addr16];
}

static void prog_write(C54xState *s, uint32_t addr, uint16_t val)
{
    uint16_t addr16 = addr & 0xFFFF;
    if ((s->pmst & PMST_OVLY) && addr16 < 0x8000 && addr16 >= 0x80)
        s->data[addr16] = val;
    if (addr16 >= 0x8000) {
        uint32_t ext = ((uint32_t)s->xpc << 16) | addr16;
        ext &= (C54X_PROG_SIZE - 1);
        s->prog[ext] = val;
    }
    s->prog[addr16] = val;
}

/* ================================================================
 * Addressing mode helpers — FIX P0: handle mod 0xC-0xF properly
 *
 * extra_words: output, set to 1 if a long-offset (lk) word follows
 *              the opcode. Caller must add this to consumed.
 * pc:          current PC, used to fetch lk word from prog space.
 * ================================================================ */

static uint16_t resolve_smem(C54xState *s, uint16_t opcode, bool *indirect,
                             int *extra_words, uint16_t pc)
{
    *extra_words = 0;

    if (opcode & 0x80) {
        /* Indirect addressing */
        *indirect = true;
        int mod = (opcode >> 3) & 0x0F;
        int nar = opcode & 0x07;
        int cur_arp = arp(s);
        uint16_t addr = s->ar[cur_arp];

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
        case 0xC: /* *(lk) — next word is absolute address */
        {
            uint16_t lk = prog_read(s, pc + 1);
            *extra_words = 1;
            addr = lk;
            break;
        }
        case 0xD: /* *+ARn(lk) — pre-increment by lk */
        {
            uint16_t lk = prog_read(s, pc + 1);
            *extra_words = 1;
            addr = s->ar[cur_arp] + lk;
            break;
        }
        case 0xE: /* *ARn(lk) — index, no modify */
        {
            uint16_t lk = prog_read(s, pc + 1);
            *extra_words = 1;
            addr = s->ar[cur_arp] + lk;
            break;
        }
        case 0xF: /* *+ARn(lk)% — circular with long offset */
        {
            uint16_t lk = prog_read(s, pc + 1);
            *extra_words = 1;
            addr = s->ar[cur_arp] + lk;
            if (s->bk > 0) {
                uint16_t base = s->ar[cur_arp] - (s->ar[cur_arp] % s->bk);
                addr = base + ((addr - base) % s->bk);
            }
            break;
        }
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
 * Instruction size helper — needed for XC skip (P0 fix)
 *
 * Returns the number of program words for the instruction at addr.
 * Only needs to handle the common cases; rare ones default to 1.
 * ================================================================ */

static int c54x_insn_words(C54xState *s, uint16_t addr)
{
    uint16_t op = prog_read(s, addr);
    uint8_t hi8 = (op >> 8) & 0xFF;
    uint8_t hi4 = (op >> 12) & 0xF;

    /* Check for Smem indirect mod 0xC-0xF (adds +1 word) in
     * single-operand instructions. We detect this via bit pattern. */
    bool has_smem_lk = false;
    if (op & 0x80) {
        int mod = (op >> 3) & 0x0F;
        if (mod >= 0xC) has_smem_lk = true;
    }

    switch (hi4) {
    case 0xF:
        if (op == 0xF495 || op == 0xF4E2 || op == 0xF4E3 || op == 0xF4E4)
            return 1; /* NOP, RSBX INTM, SSBX INTM, IDLE */
        if (hi8 == 0xFD || hi8 == 0xFF) return 1; /* XC */
        if (hi8 == 0xF4) return 2; /* B/CALL pmad */
        if (hi8 == 0xF8) {
            uint8_t sub = (op >> 4) & 0xF;
            if (sub == 0x2 || sub == 0x3) return 2; /* RPTB/RPT #lk */
            return 1 + (has_smem_lk ? 1 : 0); /* RPT Smem */
        }
        if (hi8 == 0xF3) return 1; /* LD #k9, DP */
        if (hi8 == 0xF5) return 1; /* RPT #k8 */
        if (hi8 == 0xF6) return 1; /* LD acc, acc */
        if (hi8 == 0xF7) return 1; /* LD #k8, reg */
        if (hi8 == 0xF9) return 2; /* RPT #lk16 */
        if (hi8 == 0xFA) return 2; /* BC delayed */
        if (hi8 == 0xFB || hi8 == 0xFC || hi8 == 0xFD || hi8 == 0xFE)
            return 1; /* LD #k, 16, A/B */
        if ((hi8 & 0xFE) == 0xF0) {
            if ((op & 0xFFF0) == 0xF070) return 1; /* RET/RETE */
            if ((op & 0x00F0) == 0x00B0) return 1; /* RSBX/SSBX */
            return 1; /* other F0/F1 single-word */
        }
        return 1;

    case 0xE:
        if (hi8 == 0xEA || hi8 == 0xEC || hi8 == 0xE4 || hi8 == 0xE7 ||
            hi8 == 0xE9 || hi8 == 0xEE || hi8 == 0xED)
            return 2; /* 2-word branch/call/bitf/st */
        if (hi8 == 0xE5 || hi8 == 0xE8) return 1;
        if (hi8 == 0xE1 || hi8 == 0xE6) return 1;
        return 1;

    case 0x6: case 0x7:
        return 1 + (has_smem_lk ? 1 : 0);
    case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5:
        return 1 + (has_smem_lk ? 1 : 0);

    case 0x8: case 0x9:
        /* Most 8x/9x are 2-word (MVDK, MVKD, MVPD, PORTR, PORTW, etc.) */
        if (hi8 == 0x81 || hi8 == 0x82 || hi8 == 0x83 || hi8 == 0x84)
            return 1 + (has_smem_lk ? 1 : 0);
        return 2; /* default for 8x/9x group */

    case 0xA: case 0xB:
        if (hi8 == 0xAA || hi8 == 0xBA || hi8 == 0xBD || hi8 == 0xA0 || hi8 == 0xA5)
            return 1;
        if (hi8 == 0xA8 || hi8 == 0xA9 || hi8 == 0xA2 || hi8 == 0xA3 || hi8 == 0xB3)
            return 2;
        return 1;

    case 0xC: case 0xD:
        if (hi8 == 0xC5 || hi8 == 0xCD || hi8 == 0xCE) return 1;
        if (hi8 == 0xC4 || hi8 == 0xCC || hi8 == 0xDA || hi8 == 0xDE) return 2;
        if (hi8 == 0xC0 || hi8 == 0xC1 || hi8 == 0xDD) return 1 + (has_smem_lk ? 1 : 0);
        return 1;

    default:
        return 1;
    }
}

/* ================================================================
 * Condition evaluation helper (shared by XC, BC, CC, BCD, etc.)
 * ================================================================ */

static bool eval_condition(C54xState *s, uint8_t cc)
{
    if (cc == 0x00) return true; /* UNC */
    if (cc == 0x0C) return (s->st0 & ST0_C) != 0;
    if (cc == 0x08) return !(s->st0 & ST0_C);
    if (cc == 0x30) return (s->st0 & ST0_TC) != 0;
    if (cc == 0x20) return !(s->st0 & ST0_TC);
    if (cc == 0x45) return (sext40(s->a) == 0);
    if (cc == 0x44) return (sext40(s->a) != 0);
    if (cc == 0x46) return (sext40(s->a) > 0);
    if (cc == 0x42) return (sext40(s->a) >= 0);
    if (cc == 0x43) return (sext40(s->a) < 0);
    if (cc == 0x47) return (sext40(s->a) <= 0);
    if (cc == 0x4D) return (sext40(s->b) == 0);
    if (cc == 0x4C) return (sext40(s->b) != 0);
    if (cc == 0x4E) return (sext40(s->b) > 0);
    if (cc == 0x4A) return (sext40(s->b) >= 0);
    if (cc == 0x4B) return (sext40(s->b) < 0);
    if (cc == 0x4F) return (sext40(s->b) <= 0);
    if (cc == 0x70) return (s->st0 & ST0_OVA) != 0;
    if (cc == 0x60) return !(s->st0 & ST0_OVA);
    if (cc == 0x78) return (s->st0 & ST0_OVB) != 0;
    if (cc == 0x68) return !(s->st0 & ST0_OVB);

    /* Combined conditions */
    bool cond = false;
    if (cc & 0x0C) cond |= ((cc & 0x04) ? (s->st0 & ST0_C) != 0 : !(s->st0 & ST0_C));
    if (cc & 0x30) cond |= ((cc & 0x10) ? (s->st0 & ST0_TC) != 0 : !(s->st0 & ST0_TC));
    if (cc & 0x40) {
        int64_t acc = (cc & 0x08) ? s->b : s->a;
        int c3 = cc & 0x07;
        switch (c3) {
        case 0x5: cond |= (sext40(acc) == 0); break;
        case 0x4: cond |= (sext40(acc) != 0); break;
        case 0x6: cond |= (sext40(acc) > 0); break;
        case 0x2: cond |= (sext40(acc) >= 0); break;
        case 0x3: cond |= (sext40(acc) < 0); break;
        case 0x7: cond |= (sext40(acc) <= 0); break;
        default: cond = true; break;
        }
    }
    if ((cc & 0x70) && !(cc & 0x40)) {
        if (cc & 0x08) cond |= (s->st0 & ST0_OVB) != 0;
        else           cond |= (s->st0 & ST0_OVA) != 0;
    }
    return cond;
}

/* Simplified condition for EC/E9/EE/ED-style branches (different encoding) */
static bool eval_branch_cond(C54xState *s, uint8_t cond)
{
    switch (cond) {
    case 0x00: return (s->a < 0);
    case 0x01: return (s->a >= 0);
    case 0x02: return (s->a != 0);
    case 0x03: return (s->a == 0);
    case 0x04: return (s->a > 0);
    case 0x05: return (s->a <= 0);
    case 0x08: return (s->b < 0);
    case 0x09: return (s->b >= 0);
    case 0x0A: return (s->b != 0);
    case 0x0B: return (s->b == 0);
    case 0x0C: return (s->b > 0);
    case 0x0D: return (s->b <= 0);
    case 0x20: return (s->st0 & ST0_C) != 0;
    case 0x21: return !(s->st0 & ST0_C);
    case 0x40: return (s->st0 & ST0_TC) != 0;
    case 0x41: return !(s->st0 & ST0_TC);
    default: return true;
    }
}

/* ================================================================
 * Instruction execution
 * ================================================================ */

static uint16_t pc_ring[256];
static int pc_ring_idx = 0;

static int c54x_exec_one(C54xState *s)
{
    uint16_t op = prog_read(s, s->pc);
    uint16_t op2;
    bool ind;
    int smem_extra = 0;
    uint16_t addr;
    int consumed = 1;

    uint8_t hi4 = (op >> 12) & 0xF;
    uint8_t hi8 = (op >> 8) & 0xFF;

    switch (hi4) {
    case 0xF:
        /* NOP */
        if (op == 0xF495) return consumed;

        /* RSBX INTM / SSBX INTM */
        if (op == 0xF4E2) { s->st1 &= ~ST1_INTM; return consumed; }
        if (op == 0xF4E3) { s->st1 |= ST1_INTM; return consumed; }

        /* IDLE */
        if (op == 0xF4E4) {
            if (s->pc >= 0x8000 && s->pc < 0x8020) {
                return consumed; /* TDMA slot table: skip */
            }
            s->idle = true;
            return 0;
        }

        /* XC n, cond — FIX P0: properly skip multi-word instructions */
        if (hi8 == 0xFD || hi8 == 0xFF) {
            int n_insns = (hi8 == 0xFF) ? 2 : 1;
            uint8_t cc = op & 0xFF;
            bool cond = eval_condition(s, cc);
            if (!cond) {
                /* Skip n instructions by decoding their sizes */
                uint16_t skip_pc = s->pc + 1;
                for (int i = 0; i < n_insns; i++) {
                    skip_pc += c54x_insn_words(s, skip_pc);
                }
                s->pc = skip_pc;
                return 0; /* PC already set */
            }
            return consumed; /* condition true: execute next normally */
        }

        /* F4xx: unconditional branch/call (2-word) */
        if (hi8 == 0xF4) {
            uint8_t sub = (op >> 4) & 0xF;
            op2 = prog_read(s, s->pc + 1);
            consumed = 2;
            switch (sub) {
            case 0x0: case 0xE: /* B / BD pmad */
                s->pc = op2; return 0;
            case 0x2: case 0x3: /* BACC / BACCD */
                s->pc = (uint16_t)(s->a & 0xFFFF); return 0;
            case 0x6: case 0x8: /* CALL / CALLD pmad */
                s->sp--;
                data_write(s, s->sp, (uint16_t)(s->pc + 2));
                s->pc = op2; return 0;
            case 0x7: /* CALA src */
                s->sp--;
                data_write(s, s->sp, (uint16_t)(s->pc + 1));
                s->pc = (uint16_t)(s->a & 0xFFFF); return 0;
            case 0x9: /* BD pmad (delayed) */
                s->pc = op2; return 0;
            case 0xD: /* FB extpmad — far branch */
                s->pc = op2; return 0;
            case 0xF: /* FCALL extpmad — far call (push PC+2, push XPC) */
                s->sp--;
                data_write(s, s->sp, (uint16_t)(s->pc + 2));
                s->sp--;
                data_write(s, s->sp, s->xpc);
                s->pc = op2; return 0;
            case 0xA: case 0xB: case 0xC:
                s->pc = op2; return 0;
            default:
                goto unimpl;
            }
        }

        /* F0xx/F1xx group (single-word) */
        if ((hi8 & 0xFE) == 0xF0) {
            /* F073: RET */
            if (op == 0xF073) {
                uint16_t ra = data_read(s, s->sp); s->sp++;
                s->pc = ra; return 0;
            }
            /* F074: RETE */
            if (op == 0xF074) {
                uint16_t ra = data_read(s, s->sp); s->sp++;
                s->st1 &= ~ST1_INTM;
                s->pc = ra; return 0;
            }
            /* F072: FRET — pop XPC then PC (2 pops) */
            if (op == 0xF072) {
                s->xpc = data_read(s, s->sp); s->sp++;
                uint16_t ra = data_read(s, s->sp); s->sp++;
                s->pc = ra; return 0;
            }
            /* F07x: other RET variants */
            if ((op & 0xFFF0) == 0xF070) {
                uint16_t ra = data_read(s, s->sp); s->sp++;
                s->pc = ra; return 0;
            }
            /* F0Bx/F1Bx: RSBX/SSBX bit in ST0/ST1 */
            if ((op & 0x00F0) == 0x00B0) {
                int bit = op & 0x0F;
                int set = (op >> 8) & 1;
                int st = (op >> 5) & 1;
                if (st == 0) { if (set) s->st0 |= (1<<bit); else s->st0 &= ~(1<<bit); }
                else         { if (set) s->st1 |= (1<<bit); else s->st1 &= ~(1<<bit); }
                return consumed;
            }
            /* F010: NOP */
            if (op == 0xF010) return consumed;
            goto unimpl;
        }

        /* F2xx */
        if (hi8 == 0xF2) {
            uint8_t sub = (op >> 4) & 0xF;
            if (sub == 0x7) {
                op2 = prog_read(s, s->pc + 1);
                consumed = 2;
                uint16_t ra = data_read(s, s->sp); s->sp++;
                s->pc = ra; return 0;
            }
            goto unimpl;
        }
        /* F3xx: LD #k9, DP */
        if (hi8 == 0xF3) {
            uint16_t k9 = op & 0x1FF;
            s->st0 = (s->st0 & ~ST0_DP_MASK) | k9;
            return consumed;
        }
        /* F5xx: RPT #k8 */
        if (hi8 == 0xF5) {
            s->rpt_count = op & 0xFF;
            s->rpt_pc = (uint16_t)(s->pc + 1);
            s->rpt_active = true;
            return consumed;
        }
        /* F6xx: LD acc, acc */
        if (hi8 == 0xF6) {
            uint8_t sub = (op >> 4) & 0xF;
            if (sub == 0x2 || sub == 0x6) {
                int dst = op & 1;
                if (dst) s->b = s->a; else s->a = s->b;
            }
            return consumed;
        }
        /* F7xx: LD #k8, reg */
        if (hi8 == 0xF7) {
            uint8_t sub = (op >> 4) & 0xF;
            uint16_t k = op & 0xFF;
            switch (sub) {
            case 0x0: s->st1 = (s->st1 & ~ST1_ASM_MASK) | (k & ST1_ASM_MASK); break;
            case 0x1: s->ar[0] = k; break;
            case 0x2: s->ar[1] = k; break;
            case 0x3: s->ar[2] = k; break;
            case 0x4: s->ar[3] = k; break;
            case 0x5: s->ar[4] = k; break;
            case 0x6: s->ar[5] = k; break;
            case 0x7: s->ar[6] = k; break;
            case 0x8: s->t = (s->st1 & ST1_SXM) ? (uint16_t)(int8_t)k : k; break;
            case 0x9: s->st0 = (s->st0 & ~ST0_DP_MASK) | (k & ST0_DP_MASK); break;
            case 0xA: s->st0 = (s->st0 & ~ST0_ARP_MASK) | ((k & 7) << ST0_ARP_SHIFT); break;
            case 0xB: s->ar[7] = k; break;
            case 0xC: s->bk = k; break;
            case 0xD: s->sp = k; break;
            case 0xE: s->brc = k; break;
            default: break;
            }
            return consumed;
        }
        /* F8xx: RPT/RPTB group */
        if (hi8 == 0xF8) {
            uint8_t sub = (op >> 4) & 0xF;
            if (sub == 0x2) { /* RPTB pmad */
                op2 = prog_read(s, s->pc + 1); consumed = 2;
                s->rea = op2;
                s->rsa = (uint16_t)(s->pc + 2);
                s->rptb_active = true;
                s->st1 |= ST1_BRAF;
                return consumed;
            }
            if (sub == 0x3) { /* RPT #lk */
                op2 = prog_read(s, s->pc + 1); consumed = 2;
                s->rpt_count = op2;
                s->rpt_pc = (uint16_t)(s->pc + 2);
                s->rpt_active = true;
                return consumed;
            }
            /* RPT Smem */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            s->rpt_count = data_read(s, addr);
            s->rpt_pc = (uint16_t)(s->pc + consumed);
            s->rpt_active = true;
            return consumed;
        }
        /* F9xx: RPT #lk16 */
        if (hi8 == 0xF9) {
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            s->rpt_count = op2;
            s->rpt_pc = (uint16_t)(s->pc + 2);
            s->rpt_active = true;
            return consumed;
        }
        /* FAxx: BC delayed */
        if (hi8 == 0xFA) {
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            s->pc = op2; return 0;
        }
        /* FBxx: LD #k, 16, A */
        if (hi8 == 0xFB) {
            int8_t k = (int8_t)(op & 0xFF);
            s->a = sext40((int64_t)k << 16);
            return consumed;
        }
        /* FCxx: LD #k, 16, B */
        if (hi8 == 0xFC) {
            int8_t k = (int8_t)(op & 0xFF);
            s->b = sext40((int64_t)k << 16);
            return consumed;
        }
        /* FDxx: LD #k, A */
        if (hi8 == 0xFD) {
            int8_t k = (int8_t)(op & 0xFF);
            s->a = sext40((int64_t)k);
            return consumed;
        }
        /* FExx: LD #k, B */
        if (hi8 == 0xFE) {
            int8_t k = (int8_t)(op & 0xFF);
            s->b = sext40((int64_t)k);
            return consumed;
        }
        /* FFxx: ADD #k short */
        if (hi8 == 0xFF) {
            int8_t k = (int8_t)(op & 0x7F);
            int dst = (op >> 7) & 1;
            if (dst) s->b = sext40(s->b + ((int64_t)k << 16));
            else     s->a = sext40(s->a + ((int64_t)k << 16));
            return consumed;
        }
        goto unimpl;

    case 0xE:
        if (hi8 == 0xEA) { /* BANZ */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            int n = arp(s);
            if (s->ar[n] != 0) { s->ar[n]--; s->pc = op2; return 0; }
            s->ar[n]--;
            return consumed;
        }
        if (hi8 == 0xEC || hi8 == 0xEE || hi8 == 0xED) { /* BC/BCD */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            if (eval_branch_cond(s, op & 0xFF)) { s->pc = op2; return 0; }
            return consumed;
        }
        if (hi8 == 0xE5) { /* MVMM */
            int src = (op >> 4) & 0xF;
            int dst = op & 0xF;
            uint16_t val;
            if (src <= 7) val = s->ar[src];
            else if (src == 8) val = s->sp;
            else val = data_read(s, src + 0x10);
            if (dst <= 7) s->ar[dst] = val;
            else if (dst == 8) s->sp = val;
            else data_write(s, dst + 0x10, val);
            return consumed;
        }
        if (hi8 == 0xE4) { /* BITF Smem, #lk */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed);
            consumed++;
            uint16_t val = data_read(s, addr);
            s->st0 = (val & op2) ? (s->st0 | ST0_TC) : (s->st0 & ~ST0_TC);
            return consumed;
        }
        if (hi8 == 0xE7) { /* ST #lk, Smem */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed);
            consumed++;
            data_write(s, addr, op2);
            return consumed;
        }
        if (hi8 == 0xE9) { /* CC pmad, cond */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            if (eval_branch_cond(s, op & 0xFF)) {
                s->sp--;
                data_write(s, s->sp, (uint16_t)(s->pc + 2));
                s->pc = op2; return 0;
            }
            return consumed;
        }
        if (hi8 == 0xE1) { /* NEG/ABS/CMPL/SAT/ROR/ROL */
            uint8_t sub = op & 0xFF;
            switch (sub) {
            case 0xE0: s->a = sext40(~s->a); break;
            case 0xE1: s->b = sext40(~s->b); break;
            case 0xE2: s->a = sext40(-s->a); break;
            case 0xE3: s->b = sext40(-s->b); break;
            case 0xE4: if (s->st0 & ST0_OVA) s->a = (s->a < 0) ? (int64_t)0xFF80000000LL : 0x7FFFFFFFLL; break;
            case 0xE5: if (s->st0 & ST0_OVB) s->b = (s->b < 0) ? (int64_t)0xFF80000000LL : 0x7FFFFFFFLL; break;
            case 0xE8: s->a = sext40((s->a < 0) ? -s->a : s->a); break;
            case 0xE9: s->b = sext40((s->b < 0) ? -s->b : s->b); break;
            case 0xEA: { uint16_t c = s->st0 & ST0_C ? 1 : 0; if (s->a & 1) s->st0 |= ST0_C; else s->st0 &= ~ST0_C; s->a = sext40((s->a >> 1) | ((int64_t)c << 39)); } break;
            case 0xEB: { uint16_t c = s->st0 & ST0_C ? 1 : 0; if (s->a & ((int64_t)1<<39)) s->st0 |= ST0_C; else s->st0 &= ~ST0_C; s->a = sext40((s->a << 1) | c); } break;
            default: break;
            }
            return consumed;
        }
        if (hi8 == 0xE6) { /* SFTA/SFTL */
            int shift = op & 0x1F;
            if (shift & 0x10) shift |= ~0x1F;
            int dst = (op >> 5) & 1;
            int logical = (op >> 6) & 1;
            int64_t *acc = dst ? &s->b : &s->a;
            if (logical) {
                uint64_t u = (uint64_t)(*acc) & 0xFFFFFFFFFFULL;
                if (shift >= 0) *acc = sext40((int64_t)(u << shift));
                else            *acc = sext40((int64_t)(u >> (-shift)));
            } else {
                if (shift >= 0) *acc = sext40(*acc << shift);
                else            *acc = sext40(*acc >> (-shift));
            }
            return consumed;
        }
        if (hi8 == 0xE8) { /* CMPR */
            int cmp_cond = (op >> 4) & 3;
            int n = arp(s);
            bool result = false;
            switch (cmp_cond) {
            case 0: result = (s->ar[n] == s->ar[0]); break;
            case 1: result = (s->ar[n] < s->ar[0]); break;
            case 2: result = (s->ar[n] > s->ar[0]); break;
            case 3: result = (s->ar[n] != s->ar[0]); break;
            }
            if (result) s->st0 |= ST0_TC; else s->st0 &= ~ST0_TC;
            return consumed;
        }
        goto unimpl;

    case 0x6: case 0x7:
        if ((op & 0xF800) == 0x7000) { /* STL */
            int src_acc = (op >> 9) & 1;
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            int64_t acc = src_acc ? s->b : s->a;
            data_write(s, addr, (uint16_t)(acc & 0xFFFF));
            return consumed;
        }
        if ((op & 0xF800) == 0x7800) { /* STH */
            int src_acc = (op >> 9) & 1;
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            int64_t acc = src_acc ? s->b : s->a;
            data_write(s, addr, (uint16_t)((acc >> 16) & 0xFFFF));
            return consumed;
        }
        if ((op & 0xF800) == 0x6000) { /* LD Smem, dst */
            int dst_acc = (op >> 9) & 1;
            int shift = (op >> 8) & 1;
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            uint16_t val = data_read(s, addr);
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)val : val;
            if (shift) v <<= 16;
            if (dst_acc) s->b = sext40(v); else s->a = sext40(v);
            return consumed;
        }
        if ((op & 0xF800) == 0x6800) { /* LD Smem, T */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            s->t = data_read(s, addr);
            return consumed;
        }
        goto unimpl;

    case 0x1: /* SUB */
        addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
        consumed += smem_extra;
        { int dst = (op >> 8) & 1;
          uint16_t val = data_read(s, addr);
          int64_t v = (s->st1 & ST1_SXM) ? (int16_t)val : val;
          v <<= 16;
          if (dst) s->b = sext40(s->b - v);
          else     s->a = sext40(s->a - v);
        }
        return consumed;

    case 0x0: /* ADD */
        addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
        consumed += smem_extra;
        { int dst = (op >> 8) & 1;
          uint16_t val = data_read(s, addr);
          int64_t v = (s->st1 & ST1_SXM) ? (int16_t)val : val;
          v <<= 16;
          if (dst) s->b = sext40(s->b + v);
          else     s->a = sext40(s->a + v);
        }
        return consumed;

    case 0x3: /* MAC */
        addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
        consumed += smem_extra;
        { int dst = (op >> 8) & 1;
          uint16_t val = data_read(s, addr);
          int64_t product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
          if (s->st1 & ST1_FRCT) product <<= 1;
          if (dst) s->b = sext40(s->b + product);
          else     s->a = sext40(s->a + product);
        }
        return consumed;

    case 0x2: /* MPY group */
        { int sub = (op >> 8) & 0xF;
          addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
          consumed += smem_extra;
          uint16_t val = data_read(s, addr);
          int64_t product;
          int dst;
          switch (sub) {
          case 0x0: case 0x1:
              product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
              if (s->st1 & ST1_FRCT) product <<= 1;
              if (sub & 1) s->b = sext40(product); else s->a = sext40(product);
              return consumed;
          case 0x4: case 0x5:
              product = (int64_t)(int16_t)val * (int64_t)(int16_t)val;
              if (s->st1 & ST1_FRCT) product <<= 1;
              s->t = val;
              if (sub & 1) s->b = sext40(product); else s->a = sext40(product);
              return consumed;
          case 0x8: case 0x9:
              product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
              if (s->st1 & ST1_FRCT) product <<= 1;
              if (sub & 1) { s->a += s->b; s->b = sext40(product); }
              else         { s->b += s->a; s->a = sext40(product); }
              return consumed;
          case 0xA: case 0xB:
              dst = sub & 1;
              product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
              if (s->st1 & ST1_FRCT) product <<= 1;
              if (dst) { s->a = sext40(s->a + s->b); s->b = sext40(product); }
              else     { s->b = sext40(s->b + s->a); s->a = sext40(product); }
              s->t = val;
              return consumed;
          default:
              product = (int64_t)(int16_t)s->t * (int64_t)(int16_t)val;
              if (s->st1 & ST1_FRCT) product <<= 1;
              dst = sub & 1;
              if (dst) s->b = sext40(s->b - product);
              else     s->a = sext40(s->a - product);
              return consumed;
          }
        }

    case 0x4: /* AND/OR/XOR */
        addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
        consumed += smem_extra;
        { int sub = (op >> 8) & 0xF;
          uint16_t val = data_read(s, addr);
          switch (sub & 0x3) {
          case 0:
              if (sub & 4) { s->b = (s->b & 0xFFFF0000) | (((uint16_t)s->b) & val); }
              else         { s->a = (s->a & 0xFFFF0000) | (((uint16_t)s->a) & val); }
              break;
          case 1:
              if (sub & 4) { s->b |= val; } else { s->a |= val; }
              break;
          case 2:
              if (sub & 4) { s->b ^= val; } else { s->a ^= val; }
              break;
          }
        }
        return consumed;

    case 0x5: /* Shifts */
        { int dst = (op >> 8) & 1;
          int64_t *acc = dst ? &s->b : &s->a;
          int sub = (op >> 9) & 0x7;
          if (sub <= 1) {
              int shift = asm_shift(s);
              if (shift >= 0) *acc = sext40(*acc << shift);
              else            *acc = sext40(*acc >> (-shift));
          } else if (sub == 2 || sub == 3) {
              addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
              consumed += smem_extra;
              int shift = (int16_t)data_read(s, addr);
              if (shift >= 0) *acc = sext40(*acc << shift);
              else            *acc = sext40(*acc >> (-shift));
          } else if (sub == 4 || sub == 5) {
              int shift = asm_shift(s);
              uint64_t u = (uint64_t)(*acc) & 0xFFFFFFFFFFULL;
              if (shift >= 0) *acc = sext40((int64_t)(u << shift));
              else            *acc = sext40((int64_t)(u >> (-shift)));
          } else {
              addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
              consumed += smem_extra;
              int shift = (int16_t)data_read(s, addr);
              uint64_t u = (uint64_t)(*acc) & 0xFFFFFFFFFFULL;
              if (shift >= 0) *acc = sext40((int64_t)(u << shift));
              else            *acc = sext40((int64_t)(u >> (-shift)));
          }
        }
        return consumed;

    case 0x8: case 0x9: /* Memory moves, PORTR/PORTW */
        if (hi8 == 0x8A) { /* MVDK */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            data_write(s, op2, data_read(s, addr));
            return consumed;
        }
        if (hi8 == 0x9A) { /* MVKD */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            data_write(s, addr, data_read(s, op2));
            return consumed;
        }
        if (hi8 == 0x88 || hi8 == 0x80) { /* MVDD */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            data_write(s, op2, data_read(s, addr));
            return consumed;
        }
        if (hi8 == 0x8C) { /* MVPD */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            data_write(s, addr, prog_read(s, op2));
            return consumed;
        }
        if (hi8 == 0x8E) { /* MVDP */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            prog_write(s, op2, data_read(s, addr));
            return consumed;
        }
        if (hi8 == 0x8F) { /* PORTR */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            if (op2 == 0xF430 && s->bsp_pos < s->bsp_len)
                data_write(s, addr, s->bsp_buf[s->bsp_pos++]);
            else
                data_write(s, addr, 0);
            return consumed;
        }
        if (hi8 == 0x9F) { /* PORTW */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            return consumed;
        }
        if (hi8 == 0x85) { /* MVPD alt */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            data_write(s, addr, prog_read(s, op2));
            return consumed;
        }
        if (hi8 == 0x86) { /* MVDM */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            uint16_t mmr = op & 0x1F;
            data_write(s, mmr, data_read(s, op2));
            return consumed;
        }
        if (hi8 == 0x87) { /* MVMD */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            uint16_t mmr = op & 0x1F;
            data_write(s, op2, data_read(s, mmr));
            return consumed;
        }
        if (hi8 == 0x81) { /* STL ASM */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            int shift = asm_shift(s);
            int64_t v = s->a;
            if (shift >= 0) v <<= shift; else v >>= (-shift);
            data_write(s, addr, (uint16_t)(v & 0xFFFF));
            return consumed;
        }
        if (hi8 == 0x82) { /* STH ASM */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            int shift = asm_shift(s);
            int64_t v = s->a;
            if (shift >= 0) v <<= shift; else v >>= (-shift);
            data_write(s, addr, (uint16_t)((v >> 16) & 0xFFFF));
            return consumed;
        }
        if (hi8 == 0x83) { /* WRITA */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            prog_write(s, (uint16_t)(s->a & 0xFFFF), data_read(s, addr));
            return consumed;
        }
        if (hi8 == 0x84) { /* READA */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            data_write(s, addr, prog_read(s, (uint16_t)(s->a & 0xFFFF)));
            return consumed;
        }
        /* Remaining 2-word 8x/9x variants */
        if (hi8 == 0x89 || hi8 == 0x8B || hi8 == 0x8D || hi8 == 0x91 || hi8 == 0x95 || hi8 == 0x96 || hi8 == 0x97) {
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            op2 = prog_read(s, s->pc + consumed); consumed++;
            if (hi8 == 0x91) data_write(s, addr, data_read(s, op2));
            else if (hi8 == 0x95 || hi8 == 0x96 || hi8 == 0x97) data_write(s, addr, op2);
            else data_write(s, op2, data_read(s, addr));
            return consumed;
        }
        goto unimpl;

    case 0xA: case 0xB:
        if (hi8 == 0xAA) { /* STLM */
            int src_acc = (op >> 4) & 1;
            uint16_t mmr = op & 0x1F;
            int64_t acc = src_acc ? s->b : s->a;
            data_write(s, mmr, (uint16_t)(acc & 0xFFFF));
            return consumed;
        }
        if (hi8 == 0xBD) { /* POPM */
            uint16_t mmr = op & 0x1F;
            data_write(s, mmr, data_read(s, s->sp)); s->sp++;
            return consumed;
        }
        if (hi8 == 0xBA) { /* LDMM */
            uint16_t mmr = op & 0x1F;
            int dst = (op >> 4) & 1;
            int64_t v = (int64_t)(int16_t)data_read(s, mmr);
            if (dst) s->b = sext40(v << 16); else s->a = sext40(v << 16);
            return consumed;
        }
        if (hi8 == 0xA8 || hi8 == 0xA9) { /* AND #lk */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            int dst = op & 1;
            int64_t *acc = dst ? &s->b : &s->a;
            *acc = sext40(*acc & ((int64_t)op2 << 16));
            return consumed;
        }
        if (hi8 == 0xA0) { /* LD acc, acc / NEG / ABS */
            uint8_t sub = op & 0xFF;
            if (sub == 0x00) s->a = s->b;
            else if (sub == 0x01) s->b = s->a;
            else if (sub == 0x08) s->a = sext40(-s->a);
            else if (sub == 0x09) s->b = sext40(-s->b);
            else if (sub == 0x0A) s->a = sext40((s->a < 0) ? -s->a : s->a);
            else if (sub == 0x0B) s->b = sext40((s->b < 0) ? -s->b : s->b);
            return consumed;
        }
        if (hi8 == 0xA5) { /* CMPS (Viterbi) */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            uint16_t val = data_read(s, addr);
            int src = (op >> 4) & 1;
            int64_t acc = src ? s->b : s->a;
            int64_t cmp = (int64_t)(int16_t)val << 16;
            s->trn <<= 1;
            if (acc >= cmp) { s->st0 |= ST0_TC; s->trn |= 1; }
            else { s->st0 &= ~ST0_TC; if (src) s->b = cmp; else s->a = cmp; }
            return consumed;
        }
        if (hi8 == 0xB3) { /* LD #lk, dst */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            int dst = op & 1;
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)op2 : op2;
            if (dst) s->b = sext40(v << 16); else s->a = sext40(v << 16);
            return consumed;
        }
        if (hi8 == 0xA2) { /* ADD #lk */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            int dst = op & 1;
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)op2 : op2;
            if (dst) s->b = sext40(s->b + (v << 16)); else s->a = sext40(s->a + (v << 16));
            return consumed;
        }
        if (hi8 == 0xA3) { /* SUB #lk */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            int dst = op & 1;
            int64_t v = (s->st1 & ST1_SXM) ? (int16_t)op2 : op2;
            if (dst) s->b = sext40(s->b - (v << 16)); else s->a = sext40(s->a - (v << 16));
            return consumed;
        }
        goto unimpl;

    case 0xC: case 0xD:
        if (hi8 == 0xC5) { /* PSHM */
            uint16_t mmr = op & 0x1F;
            s->sp--; data_write(s, s->sp, data_read(s, mmr));
            return consumed;
        }
        if (hi8 == 0xCD) { /* POPM */
            uint16_t mmr = op & 0x1F;
            data_write(s, mmr, data_read(s, s->sp)); s->sp++;
            return consumed;
        }
        if (hi8 == 0xCE) { /* FRAME */
            int8_t k = (int8_t)(op & 0xFF);
            s->sp += k;
            return consumed;
        }
        if (hi8 == 0xC4 || hi8 == 0xCC) { /* PSHD dmad (2-word) */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            s->sp--; data_write(s, s->sp, data_read(s, op2));
            return consumed;
        }
        if (hi8 == 0xC0) { /* PSHD Smem */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            s->sp--; data_write(s, s->sp, data_read(s, addr));
            return consumed;
        }
        if (hi8 == 0xC1) { /* RPT Smem */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            s->rpt_count = data_read(s, addr);
            s->rpt_pc = (uint16_t)(s->pc + consumed);
            s->rpt_active = true;
            return consumed;
        }
        if (hi8 == 0xDA) { /* RPTBD */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            s->rea = op2;
            s->rsa = (uint16_t)(s->pc + 4);
            s->rptb_active = true;
            s->st1 |= ST1_BRAF;
            return consumed;
        }
        if (hi8 == 0xDD) { /* POPD Smem */
            addr = resolve_smem(s, op, &ind, &smem_extra, s->pc);
            consumed += smem_extra;
            data_write(s, addr, data_read(s, s->sp)); s->sp++;
            return consumed;
        }
        if (hi8 == 0xDE) { /* POPD dmad */
            op2 = prog_read(s, s->pc + 1); consumed = 2;
            data_write(s, op2, data_read(s, s->sp)); s->sp++;
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
        pc_ring[pc_ring_idx & 255] = s->pc;
        pc_ring_idx++;

        /* Check RPTB */
        if (s->rptb_active && s->pc == s->rea + 1) {
            if (s->brc > 0) { s->brc--; s->pc = s->rsa; }
            else { s->rptb_active = false; s->st1 &= ~ST1_BRAF; }
        }

        int consumed;
        if (s->rpt_active) {
            consumed = c54x_exec_one(s);
            if (s->rpt_count > 0) {
                s->rpt_count--;
                s->cycles++; executed++; continue;
            } else {
                s->rpt_active = false;
            }
        } else {
            consumed = c54x_exec_one(s);
        }

        if (consumed > 0) s->pc += consumed;
        s->pc &= 0xFFFF;

        /* Timer0 tick */
        if (!(s->data[TCR_ADDR] & TCR_TSS)) {
            if (s->timer_psc > 0) {
                s->timer_psc--;
            } else {
                s->timer_psc = s->data[TCR_ADDR] & TCR_TDDR_MASK;
                if (s->data[TIM_ADDR] > 0) s->data[TIM_ADDR]--;
                if (s->data[TIM_ADDR] == 0 && s->data[PRD_ADDR] > 0) {
                    s->data[TIM_ADDR] = s->data[PRD_ADDR];
                    s->ifr |= (1 << 4);
                    uint16_t fn_lo = s->data[0x0585];
                    uint16_t fn_hi = s->data[0x0584] & 0x00FF;
                    uint32_t fn = ((uint32_t)fn_hi << 16) | fn_lo;
                    fn = (fn + 1) % 2715648;
                    s->data[0x0585] = fn & 0xFFFF;
                    s->data[0x0584] = (s->data[0x0584] & 0xFF00) | ((fn >> 16) & 0xFF);
                }
            }
        }

        s->cycles++;
        s->insn_count++;
        executed++;
    }
    return executed;
}

/* ================================================================
 * ROM loader
 * ================================================================ */

int c54x_load_rom(C54xState *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { C54_LOG("Cannot open ROM dump: %s", path); return -1; }

    char line[1024];
    int section = -1;
    int total_words = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "DSP dump: Registers"))  { section = 0; continue; }
        if (strstr(line, "DSP dump: DROM"))        { section = 1; continue; }
        if (strstr(line, "DSP dump: PDROM"))       { section = 2; continue; }
        if (strstr(line, "DSP dump: PROM0"))       { section = 3; continue; }
        if (strstr(line, "DSP dump: PROM1"))       { section = 4; continue; }
        if (strstr(line, "DSP dump: PROM2"))       { section = 5; continue; }
        if (strstr(line, "DSP dump: PROM3"))       { section = 6; continue; }
        if (section < 0) continue;

        uint32_t addr;
        if (sscanf(line, "%x :", &addr) != 1) continue;
        char *p = strchr(line, ':');
        if (!p) continue;
        p++;

        uint16_t word;
        int n;
        while (sscanf(p, " %hx%n", &word, &n) == 1) {
            p += n;
            if (section == 0) { if (addr < 0x60) s->data[addr] = word; }
            else if (section == 1 || section == 2) { if (addr < C54X_DATA_SIZE) s->data[addr] = word; }
            else {
                if (addr < C54X_PROG_SIZE) s->prog[addr] = word;
                if (section == 4) {
                    uint16_t addr16 = addr & 0xFFFF;
                    if (addr16 >= 0x8000) s->prog[addr16] = word;
                }
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
    s->st1 = ST1_INTM;
    s->pmst = 0xFFE0;
    s->imr = 0; s->ifr = 0;
    s->xpc = 0;
    s->timer_psc = 0;
    s->data[TCR_ADDR] = TCR_TSS;
    s->data[TIM_ADDR] = 0xFFFF;
    s->data[PRD_ADDR] = 0xFFFF;
    s->rpt_active = false;
    s->rptb_active = false;
    s->idle = false;
    s->running = true;
    s->cycles = 0;
    s->insn_count = 0;
    s->unimpl_count = 0;

    uint16_t iptr = (s->pmst >> PMST_IPTR_SHIFT) & 0x1FF;
    s->pc = iptr * 0x80;

    C54_LOG("Reset: PC=0x%04x PMST=0x%04x SP=0x%04x", s->pc, s->pmst, s->sp);
}

void c54x_interrupt_ex(C54xState *s, int vec, int imr_bit)
{
    if (vec < 0 || vec >= 32) return;
    if (imr_bit < 0 || imr_bit >= 16) return;
    s->ifr |= (1 << imr_bit);

    if (!(s->st1 & ST1_INTM) && (s->imr & (1 << imr_bit))) {
        s->ifr &= ~(1 << imr_bit);
        uint16_t ret_addr = s->idle ? (uint16_t)(s->pc + 1) : (uint16_t)s->pc;
        s->sp--;
        data_write(s, s->sp, ret_addr);
        s->st1 |= ST1_INTM;
        uint16_t iptr = (s->pmst >> PMST_IPTR_SHIFT) & 0x1FF;
        s->pc = (iptr * 0x80) + vec * 4;
        s->idle = false;
    } else if (s->idle && (s->imr & (1 << imr_bit))) {
        s->idle = false;
        s->ifr &= ~(1 << imr_bit);
        s->sp--;
        data_write(s, s->sp, (uint16_t)(s->pc + 1));
        s->st1 |= ST1_INTM;
        uint16_t iptr = (s->pmst >> PMST_IPTR_SHIFT) & 0x1FF;
        s->pc = (iptr * 0x80) + vec * 4;
    }
}

void c54x_wake(C54xState *s) { s->idle = false; }

void c54x_bsp_load(C54xState *s, const uint16_t *samples, int n)
{
    if (n > 160) n = 160;
    memcpy(s->bsp_buf, samples, n * sizeof(uint16_t));
    s->bsp_len = n;
    s->bsp_pos = 0;
}

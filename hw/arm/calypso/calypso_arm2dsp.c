/*
 * calypso_arm2dsp — ARM->DSP orchestration bridge (see calypso_arm2dsp.h).
 *
 * The ARM orchestrates the DSP: when it commands the FB task (d_dsp_page bit1 =
 * B_GSM_TASK), we drive the stalled DSP into its go-live setter path with a
 * coherent context, so it arms its own IMR and the frame ISR / FCCH correlator
 * finally run. No poking of IMR / d_fb_det / vectors: we only redirect the DSP
 * control flow into ROM code it was meant to run once the ARM said "go".
 */
#include "qemu/osdep.h"
#include "calypso_arm2dsp.h"

#include <stdlib.h>
#include <stdio.h>

/* ARM byte offset of d_dsp_page (DSP word 0x08D4). B_GSM_TASK = bit1. */
#define A2D_DSP_PAGE_OFF   0x01A8
#define A2D_B_GSM_TASK     0x0002

static int      a2d_on = -1;      /* -1 = unresolved, 0/1 = disabled/enabled   */
static uint16_t a2d_tgt;          /* redirect target PC                        */
static uint16_t a2d_at;           /* DSP PC at which to apply the redirect      */
static int      a2d_sp_set;       /* whether to force SP                        */
static uint16_t a2d_sp;           /* forced SP value                           */
static unsigned a2d_max;          /* how many drives allowed                    */
static int      a2d_435b_set;     /* whether to set data[0x435b] at drive       */
static uint16_t a2d_435b;         /* value to write into data[0x435b]           */
static int      a2d_imr_set;      /* whether to OR bits into IMR at drive        */
static uint16_t a2d_imr;          /* IMR bits to arm (e.g. 0x08 = INT3 frame)    */
static int      a2d_enai;         /* whether to clear INTM (enable interrupts)   */
static int      a2d_noredir;      /* if set, arm only; do not redirect PC        */

static volatile int a2d_pending;  /* ARM requested orchestration, awaiting DSP   */
static unsigned     a2d_done;     /* drives applied so far                       */

static uint16_t a2d_env_u16(const char *name, uint16_t def)
{
    const char *e = getenv(name);
    if (!e || !*e) {
        return def;
    }
    return (uint16_t)strtoul(e, NULL, 0);
}

static void a2d_resolve(void)
{
    if (a2d_on >= 0) {
        return;
    }
    const char *e = getenv("CALYPSO_ARM2DSP");
    a2d_on = (e && atoi(e) > 0) ? 1 : 0;
    a2d_tgt = a2d_env_u16("CALYPSO_ARM2DSP_TGT", 0xb3ec);
    a2d_at  = a2d_env_u16("CALYPSO_ARM2DSP_AT", 0xa4d4);
    a2d_max = a2d_env_u16("CALYPSO_ARM2DSP_N", 1);
    const char *sp = getenv("CALYPSO_ARM2DSP_SP");
    a2d_sp_set = (sp && *sp) ? 1 : 0;
    a2d_sp = a2d_sp_set ? (uint16_t)strtoul(sp, NULL, 0) : 0;
    const char *r = getenv("CALYPSO_ARM2DSP_435B");
    a2d_435b_set = (r && *r) ? 1 : 0;
    a2d_435b = a2d_435b_set ? (uint16_t)strtoul(r, NULL, 0) : 0;
    const char *im = getenv("CALYPSO_ARM2DSP_IMR");
    a2d_imr_set = (im && *im) ? 1 : 0;
    a2d_imr = a2d_imr_set ? (uint16_t)strtoul(im, NULL, 0) : 0;
    a2d_enai = getenv("CALYPSO_ARM2DSP_ENAI") ? 1 : 0;
    a2d_noredir = getenv("CALYPSO_ARM2DSP_NOREDIR") ? 1 : 0;
    if (a2d_on) {
        fprintf(stderr,
                "[arm2dsp] enabled: trigger=d_dsp_page(0x01A8) bit1 ; "
                "redirect @0x%04x -> 0x%04x ; SP%s ; N=%u\n",
                a2d_at, a2d_tgt,
                a2d_sp_set ? "=(forced)" : "=(kept)", a2d_max);
    }
}

void calypso_arm2dsp_on_arm_write(uint16_t offset, uint16_t value)
{
    a2d_resolve();
    if (!a2d_on) {
        return;
    }
    /* The ARM orchestrates the FB task by writing d_dsp_page bit1 (B_GSM_TASK). */
    if (offset == A2D_DSP_PAGE_OFF && (value & A2D_B_GSM_TASK)) {
        if (a2d_done < a2d_max && !a2d_pending) {
            a2d_pending = 1;
            fprintf(stderr,
                    "[arm2dsp] ARM orchestrates FB task (d_dsp_page=0x%04x) "
                    "-> go-live drive pending\n", value);
        }
    }
}

void calypso_arm2dsp_on_dsp_step(C54xState *s, uint16_t exec_pc)
{
    a2d_resolve();
    if (!a2d_on) {
        return;
    }
    /* Sustained interrupt window: once armed, keep INTM cleared each pass
     * through the wait-loop PC so the armed frame IT (INT3) can actually be
     * taken. Done only at a2d_at (in the bring-up loop, never inside an ISR)
     * to avoid nesting; the ISR sets INTM=1 on entry and RETE restores it. */
    if (a2d_enai && a2d_done > 0 && exec_pc == a2d_at) {
        s->st1 &= ~ST1_INTM;
    }
    if (!a2d_pending || exec_pc != a2d_at) {
        return;
    }
    /* Apply the drive: the DSP is in its go-live bring-up loop; redirect it into
     * the setter path so it runs the real go-live follow-through. Optionally wire
     * a coherent operational stack pointer first. */
    a2d_pending = 0;
    a2d_done++;
    uint16_t old_sp = s->sp;
    uint16_t old_pc = s->pc;
    if (a2d_imr_set) {
        s->imr |= a2d_imr;          /* arm frame IT (INT3/bit3) so BSP IT vectorizes */
    }
    if (a2d_enai) {
        s->st1 &= ~ST1_INTM;        /* RSBX INTM: enable maskable interrupts */
    }
    if (!a2d_noredir) {
        if (a2d_sp_set) {
            s->sp = a2d_sp;
        }
        if (a2d_435b_set) {
            s->data[0x435b] = a2d_435b;
        }
        s->pc = a2d_tgt;
    }
    fprintf(stderr,
            "[arm2dsp] DRIVE #%u @0x%04x: PC 0x%04x->0x%04x  SP 0x%04x->0x%04x  "
            "IMR=0x%04x insn=%u\n",
            a2d_done, exec_pc, old_pc, s->pc, old_sp, s->sp,
            s->imr, s->insn_count);
}

/*
 * calypso_layer1.c — HLE L1 model for the Calypso DSP (CALYPSO_L1=c).
 *
 * ÉTAPE 0 (read-only, zéro write firmware) : calcule et logge le ratio de
 * cohérence différentielle FCCH
 *      R = |Σ d[n]| / Σ |d[n]| ,  d[n] = z[n]·conj(z[n-1]),  z = I + jQ
 * sur le burst I/Q livré en DARAM 0x2a00. Un ton pur (FCCH, +fc/4) donne des
 * d[n] colinéaires → R≈1 ; du bruit / GMSK modulé → R≈0.
 *
 * CRITÈRE (ce qu'on veut voir avant d'écrire le détecteur à l'ÉTAPE 1) : R doit
 * PIQUER à FN mod 51 ∈ {0,10,20,30,40} (positions FCCH sur C0 TS0) et s'écrouler
 * ailleurs. Si R ne pique jamais à ces positions, le pipeline I/Q ne délivre pas
 * de FCCH planifié → l'ÉTAPE 1 échouerait pour une raison SIGNAL, pas ALGO.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/arm/calypso/calypso_layer1.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- offsets en data[] (DSP word) / dsp_ram (ARM word) --- */
#define L1_DARAM_IQ      0x2a00      /* 148 paires int16 I/Q entrelacées */
#define L1_IQ_PAIRS      148
/* write-page courante (lue côté miroir ARM dsp_ram) */
#define WP0_TASK_MD      (0x0008/2)  /* d_task_md page 0, ARM byte 0x0008 */
#define WP1_TASK_MD      (0x0030/2)  /* d_task_md page 1, ARM byte 0x0030 */
#define WP0_TASK_D       (0x0000/2)  /* d_task_d  page 0 */
#define WP1_TASK_D       (0x0028/2)  /* d_task_d  page 1 */
#define D_DSP_PAGE_WORD  (0x01A8/2)  /* d_dsp_page (ARM byte 0x01A8) */

int calypso_l1_c_active(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("CALYPSO_L1");
        v = (e && e[0] == 'c') ? 1 : 0;
    }
    return v;
}

void calypso_layer1_tick(C54xState *dsp, uint16_t *dsp_ram, uint32_t fn)
{
    if (!dsp || !dsp_ram) {
        return;
    }

    uint16_t page = dsp_ram[D_DSP_PAGE_WORD] & 1;
    uint16_t md   = page ? dsp_ram[WP1_TASK_MD] : dsp_ram[WP0_TASK_MD];
    uint16_t td   = page ? dsp_ram[WP1_TASK_D]  : dsp_ram[WP0_TASK_D];

    /* === ÉTAPE 0 augmentée : cohérence différentielle (READ-ONLY) === */
    double sre = 0.0, sim = 0.0, smag = 0.0, energy = 0.0;
    int16_t pi = (int16_t)dsp->data[L1_DARAM_IQ + 0];
    int16_t pq = (int16_t)dsp->data[L1_DARAM_IQ + 1];
    energy += (double)pi * pi + (double)pq * pq;
    for (int n = 1; n < L1_IQ_PAIRS; n++) {
        int16_t ii = (int16_t)dsp->data[L1_DARAM_IQ + 2 * n];
        int16_t qq = (int16_t)dsp->data[L1_DARAM_IQ + 2 * n + 1];
        /* d = z · conj(z_prev) = (ii + j qq)(pi - j pq) */
        double dre = (double)ii * pi + (double)qq * pq;
        double dim = (double)qq * pi - (double)ii * pq;
        sre  += dre;
        sim  += dim;
        smag += sqrt(dre * dre + dim * dim);
        energy += (double)ii * ii + (double)qq * qq;
        pi = ii;
        pq = qq;
    }
    double sabs = sqrt(sre * sre + sim * sim);
    double R = (smag > 0.0) ? (sabs / smag) : 0.0;
    double ang_deg = (sre != 0.0 || sim != 0.0)
                     ? atan2(sim, sre) * 180.0 / M_PI : 0.0;

    unsigned m51 = fn % 51;
    int fcch_pos = (m51 == 0 || m51 == 10 || m51 == 20 || m51 == 30 || m51 == 40);

    static unsigned logn = 0;
    if (logn++ < 6000) {
        fprintf(stderr,
                "[L1c] fn=%u m51=%u%s md=%u td=%u page=%u R=%.3f |S|=%.0f "
                "ang=%.0f E=%.0f\n",
                fn, m51, fcch_pos ? " FCCH" : "", md, td, page,
                R, sabs, ang_deg, energy);
    }
}

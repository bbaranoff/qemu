/*
 * calypso_rhea_dma.c — contrôleur DMA RHEA du Calypso, côté MCU (FFFF:FC00).
 *
 * POURQUOI CE FICHIER EXISTE (2026-08-03).
 * ----------------------------------------
 * Mesure : le firmware DSP pilote correctement le RIF (séquence RRST à deux
 * écritures du §12.6, relecture de SPCR, réaction sur FIFO non-vide) mais laisse
 * `RINT_MASK` ET `RDMA_MASK` à 1 sur tout le run, et ne lit JAMAIS DRR. Il
 * constate donc que des données sont là et ne les prend pas par le port série.
 * Elles doivent arriver en MÉMOIRE — §3.7.1 : « API interface for radio data in
 * DMA mode (buffered mode with data block transfer) ».
 *
 * Or la configuration DMA se fait « from the MCU only » (§3.5.15) sur cette
 * fenêtre, et le modèle n'y avait qu'un `add_stub` MUET : lectures à 0, écritures
 * jetées. Toute la programmation DMA de l'ARM disparaissait sans un mot — d'où
 * l'impossibilité de savoir où le burst est censé atterrir.
 *
 * CE QUE CE MODULE FAIT, ET NE FAIT PAS.
 * Il enregistre et restitue les registres du §11, avec leurs valeurs de reset, et
 * journalise chaque écriture DÉCODÉE. Il n'exécute AUCUN transfert : c'est un
 * instrument de lecture, pas une implémentation du DMA. La question qu'il répond
 * est « où l'ARM demande-t-il que le burst soit déposé », et `DMA1_AAD` la donne
 * en clair (canal 1 = `RIF_DMA_REQ_R`, Table 2 du §6).
 *
 * CARTE (§11.1, Table 18) — fenêtre 0x100 à partir de FFFF:FC00 :
 *   +0x00 CONTROLLER_CONFIG   6b R/W   DMA_BURST(4:2)=1, PRIORITY_ENABLE(5)=1
 *   +0x02 ALLOC_CONFIG        4b R/W   reset 1111 = les 4 canaux pilotés par l'ARM
 *   +0x10 DMA1_RAD           16b R/W   RHEA_START(10:0) + RHEA_CS(15:11)
 *   +0x12 DMA1_RDPTH         11b R/W   profondeur du tampon Rhea, en OCTETS
 *   +0x14 DMA1_AAD           12b R/W   début du tampon de réception dans l'API,
 *                                      « The address is always expressed in Bytes »
 *   +0x16 DMA1_ALGTH         12b R/W   longueur de page API, en octets
 *   +0x18 DMA1_CTRL          13b       reset 0x04A2 (cf. ci-dessous)
 *   +0x1A DMA1_CUR_OFFSET_API 12b R    offset courant
 *   … canaux 2/3/4 aux +0x20 / +0x30 / +0x40.
 *
 * DMAn_CTRL (§11.3.5) — la valeur de reset annoncée « 0 0100 1?10 0010 » a été
 * revérifiée champ par champ et tombe sur 0x04A2, ce qui valide la lecture :
 *   0 ENABLE=0 · 1 IDLE=1 (R) · 2 ONE_SHOT=0 · 3 FIFO_MODE=0 · 4 CURRENT_PAGE=0
 *   5 MAS=1 (transferts 16 bits) · 6 DMA_START (W, relu toujours 0)
 *   7 IRQ_MODE=1 · 8 IRQ_STATE=0 (R) · 9 RHEA_ERROR=0 (R)
 *   10 DIRECTION=1 → « Transactions are done on Rhea -> API » · 12:11 PRIORITY=00
 * Le « ? » du doc est DMA_START, qui se relit toujours à zéro. Et DIRECTION=1 au
 * reset confirme le sens attendu pour la réception radio : périphérique → API.
 *
 * NB une incohérence du doc, signalée plutôt que masquée : la Table 18 donne
 * CONTROLLER_CONFIG en reset « 11 111? » alors que la liste des champs du §11.2.1
 * donne DMA_BURST=0x1 et PRIORITY_ENABLE=1, soit 0b100100 = 0x24. On suit la
 * liste des champs, plus précise que la chaîne de la table.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/arm/calypso/calypso_debug.h"
#include "hw/arm/calypso/calypso_rhea_dma.h"

#include <stdio.h>
#include <stdlib.h>

#define RD_CTRL_CFG   0x00
#define RD_ALLOC_CFG  0x02
#define RD_CH_BASE(n) (0x10 + 0x10 * (n))   /* n = 0..3 -> 0x10 0x20 0x30 0x40 */
#define RD_RAD        0x0
#define RD_RDPTH      0x2
#define RD_AAD        0x4
#define RD_ALGTH      0x6
#define RD_CTRL       0x8
#define RD_CUR_OFF    0xA

#define CTRL_RESET    0x04A2
#define CTRLCFG_RESET 0x0024
#define ALLOC_RESET   0x000F

/* champs de DMAn_CTRL */
#define CTRL_ENABLE       (1u << 0)
#define CTRL_IDLE         (1u << 1)
#define CTRL_ONE_SHOT     (1u << 2)
#define CTRL_FIFO_MODE    (1u << 3)
#define CTRL_CURRENT_PAGE (1u << 4)
#define CTRL_MAS          (1u << 5)
#define CTRL_DMA_START    (1u << 6)
#define CTRL_IRQ_MODE     (1u << 7)
#define CTRL_IRQ_STATE    (1u << 8)
#define CTRL_RHEA_ERROR   (1u << 9)
#define CTRL_DIRECTION    (1u << 10)

/* bits en lecture seule : conservés par le modèle, pas écrasés par l'ARM */
#define CTRL_RO_MASK  (CTRL_IDLE | CTRL_IRQ_STATE | CTRL_RHEA_ERROR)

static struct {
    bool     init;
    uint16_t ctrl_cfg, alloc_cfg;
    struct { uint16_t rad, rdpth, aad, algth, ctrl, cur_off; } ch[4];
    unsigned n_wr, n_rd, n_start;
} rd;

static bool rhea_dma_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = calypso_gate("CALYPSO_RHEA_DMA", 1);
        if (!on)
            fprintf(stderr, "[rhea-dma] CALYPSO_RHEA_DMA=0 : retour au stub muet "
                    "(lectures a 0, ecritures jetees) — comportement d'avant le 03/08\n");
    }
    return on != 0;
}

static void rhea_dma_init(void)
{
    if (rd.init)
        return;
    rd.init = true;
    rd.ctrl_cfg  = CTRLCFG_RESET;
    rd.alloc_cfg = ALLOC_RESET;
    for (int i = 0; i < 4; i++)
        rd.ch[i].ctrl = CTRL_RESET;
    fprintf(stderr, "[rhea-dma] controleur DMA RHEA arme (CAL207 §11) @0xFFFFFC00 : "
            "4 canaux, reset CTRL=0x%04x (DIRECTION=1 Rhea->API, MAS=1 16 bits, "
            "IRQ_MODE=1, ENABLE=0). Canal 1 = RIF_DMA_REQ_R (§6 Table 2). "
            "ENREGISTREMENT SEUL : aucun transfert n'est execute.\n", CTRL_RESET);
}

/* Traduit l'adresse API (en OCTETS, §11.3.3) dans les deux repères utiles. */
static void log_aad(int n, uint16_t aad)
{
    uint32_t byte = aad & 0x0FFF;
    fprintf(stderr, "[rhea-dma] *** DMA%d_AAD = 0x%03x octets  ->  mot DSP 0x%04x  "
            "(= ARM 0x%08x). C'est la DESTINATION du tampon de reception dans la "
            "memoire API.\n",
            n + 1, byte, (unsigned)(0x0800 + byte / 2), 0xFFD00000u + byte);
}

static void log_ctrl(int n, uint16_t v)
{
    fprintf(stderr, "[rhea-dma] DMA%d_CTRL <- 0x%04x : ENABLE=%d ONE_SHOT=%d "
            "FIFO_MODE=%d PAGE=%d MAS=%d(%s) DMA_START=%d IRQ_MODE=%d DIRECTION=%d(%s) "
            "PRIORITY=%d\n",
            n + 1, v,
            !!(v & CTRL_ENABLE), !!(v & CTRL_ONE_SHOT), !!(v & CTRL_FIFO_MODE),
            !!(v & CTRL_CURRENT_PAGE), !!(v & CTRL_MAS),
            (v & CTRL_MAS) ? "16b" : "8b",
            !!(v & CTRL_DMA_START), !!(v & CTRL_IRQ_MODE),
            !!(v & CTRL_DIRECTION),
            (v & CTRL_DIRECTION) ? "Rhea->API" : "API->Rhea",
            (v >> 11) & 3);
}

uint64_t calypso_rhea_dma_read(void *opaque, hwaddr off, unsigned size)
{
    (void)opaque; (void)size;
    if (!rhea_dma_on())
        return 0;
    rhea_dma_init();

    uint16_t v = 0;
    if (off == RD_CTRL_CFG)       v = rd.ctrl_cfg;
    else if (off == RD_ALLOC_CFG) v = rd.alloc_cfg;
    else {
        for (int n = 0; n < 4; n++) {
            hwaddr b = RD_CH_BASE(n);
            if (off < b || off > b + RD_CUR_OFF)
                continue;
            switch (off - b) {
            case RD_RAD:     v = rd.ch[n].rad;     break;
            case RD_RDPTH:   v = rd.ch[n].rdpth;   break;
            case RD_AAD:     v = rd.ch[n].aad;     break;
            case RD_ALGTH:   v = rd.ch[n].algth;   break;
            /* DMA_START « Reading of this bit is always equal to zero » (§11.3.5) */
            case RD_CTRL:    v = rd.ch[n].ctrl & (uint16_t)~CTRL_DMA_START; break;
            case RD_CUR_OFF: v = rd.ch[n].cur_off; break;
            default: break;
            }
            break;
        }
    }
    if (rd.n_rd++ < 40)
        fprintf(stderr, "[rhea-dma] RD  +0x%02x = 0x%04x\n", (unsigned)off, v);
    return v;
}

void calypso_rhea_dma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    (void)opaque; (void)size;
    if (!rhea_dma_on())
        return;
    rhea_dma_init();

    uint16_t v = (uint16_t)val;
    rd.n_wr++;

    if (off == RD_CTRL_CFG) {
        rd.ctrl_cfg = v & 0x003F;
        fprintf(stderr, "[rhea-dma] CONTROLLER_CONFIG <- 0x%04x (DMA_BURST=%d "
                "PRIORITY_ENABLE=%d)\n", v, (v >> 2) & 7, !!(v & 0x20));
        return;
    }
    if (off == RD_ALLOC_CFG) {
        rd.alloc_cfg = v & 0x000F;
        fprintf(stderr, "[rhea-dma] ALLOC_CONFIG <- 0x%04x : canal1=%s canal2=%s "
                "canal3=%s canal4=%s\n", v,
                (v & 1) ? "ARM" : "DSP", (v & 2) ? "ARM" : "DSP",
                (v & 4) ? "ARM" : "DSP", (v & 8) ? "ARM" : "DSP");
        return;
    }

    for (int n = 0; n < 4; n++) {
        hwaddr b = RD_CH_BASE(n);
        if (off < b || off > b + RD_CUR_OFF)
            continue;
        switch (off - b) {
        case RD_RAD:
            rd.ch[n].rad = v;
            fprintf(stderr, "[rhea-dma] DMA%d_RAD   <- 0x%04x (RHEA_START=0x%03x "
                    "RHEA_CS=%d)\n", n + 1, v, v & 0x7FF, (v >> 11) & 0x1F);
            break;
        case RD_RDPTH:
            rd.ch[n].rdpth = v & 0x07FF;
            fprintf(stderr, "[rhea-dma] DMA%d_RDPTH <- %u octets\n", n + 1, v & 0x7FF);
            break;
        case RD_AAD:
            rd.ch[n].aad = v & 0x0FFF;
            log_aad(n, v);
            break;
        case RD_ALGTH:
            rd.ch[n].algth = v & 0x0FFF;
            fprintf(stderr, "[rhea-dma] DMA%d_ALGTH <- %u octets (= %u mots de page API)\n",
                    n + 1, v & 0xFFF, (v & 0xFFF) / 2);
            break;
        case RD_CTRL: {
            uint16_t keep = rd.ch[n].ctrl & CTRL_RO_MASK;
            rd.ch[n].ctrl = (uint16_t)((v & ~CTRL_RO_MASK & 0x1FFF) | keep);
            log_ctrl(n, v);
            if (v & CTRL_DMA_START) {
                rd.n_start++;
                fprintf(stderr, "[rhea-dma] *** DMA%d DMA_START #%u — l'ARM DEMANDE un "
                        "transfert. Ce module NE L'EXECUTE PAS (instrument de lecture). "
                        "Parametres courants : RAD=0x%04x RDPTH=%u AAD=0x%03x "
                        "(mot DSP 0x%04x) ALGTH=%u\n",
                        n + 1, rd.n_start, rd.ch[n].rad, rd.ch[n].rdpth,
                        rd.ch[n].aad, (unsigned)(0x0800 + rd.ch[n].aad / 2),
                        rd.ch[n].algth);
            }
            break;
        }
        case RD_CUR_OFF:
            /* R seulement (§11.3.6) — on ignore l'ecriture. */
            break;
        default:
            break;
        }
        return;
    }

    fprintf(stderr, "[rhea-dma] WR  +0x%02x = 0x%04x (hors carte §11)\n",
            (unsigned)off, v);
}

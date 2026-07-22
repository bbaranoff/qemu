/*
 * calypso_dsp_internal.h — internal state/primitives shared between
 * calypso_dsp_shunt.c (the shunt GLUE) and calypso_dsp_helper.c (the
 * mode-neutral NDB-write PRIMITIVES).
 *
 * Pure mechanical split : all #define/enum/struct here were moved VERBATIM
 * from the top of calypso_dsp_shunt.c so BOTH translation units see them.
 * No logic change.
 */

#ifndef CALYPSO_DSP_INTERNAL_H
#define CALYPSO_DSP_INTERNAL_H

#include "qemu/osdep.h"
#include "exec/memory.h"
#include "sysemu/dma.h"
#include "qemu/error-report.h"
#include "hw/arm/calypso/calypso_c54x.h"   /* C54xState */

/* ---- Memory map (ARM-side addresses, from osmocom-bb dsp_api.h:18-23) ---- */
#define BASE_API_W_PAGE_0   0xFFD00000UL  /* 20 words MCU→DSP page 0 */
#define BASE_API_W_PAGE_1   0xFFD00028UL  /* 20 words MCU→DSP page 1 */
#define BASE_API_R_PAGE_0   0xFFD00050UL  /* 20 words DSP→MCU page 0 */
#define BASE_API_R_PAGE_1   0xFFD00078UL  /* 20 words DSP→MCU page 1 */
#define BASE_API_NDB        0xFFD001A8UL  /* 268 words persistent NDB */

/* ---- Write page (T_DB_MCU_TO_DSP) field offsets (DWARF-validated) ---- */
#define WP_D_TASK_D         0x00
#define WP_D_BURST_D        0x02
#define WP_D_TASK_U         0x04
#define WP_D_BURST_U        0x06
#define WP_D_TASK_MD        0x08
#define WP_D_TASK_RA        0x0E
#define WP_D_FN             0x10
#define WP_D_CTRL_SYSTEM    0x20

/* ---- Read page (T_DB_DSP_TO_MCU) field offsets ---- */
#define RP_D_TASK_D         0x00
#define RP_D_BURST_D        0x02
#define RP_D_TASK_MD        0x08
#define RP_A_SERV_DEMOD     0x10   /* [4] words: {D_TOA,D_PM,D_ANGLE,D_SNR} */
#define RP_A_PM             0x18   /* [3] words */
#define RP_A_SCH            0x1E   /* [5] words: SB header+info */

/* ---- NDB (T_NDB_MCU_DSP) field offsets ---- */
#define NDB_D_DSP_PAGE      0x00
#define NDB_D_ERROR_STATUS  0x02
#define NDB_D_FB_DET        0x48
#define NDB_D_FB_MODE       0x4A
#define NDB_A_SYNC_DEMOD    0x4C   /* [4] words */
#define NDB_A_CD            0x1FC  /* a_cd[15] : CCCH demod result.
                                       FIX 2026-06-02 : 0x1DC → 0x1FC (retour à la
                                       valeur DWARF autoritaire). Le DWARF de
                                       layer1.highram.elf donne offsetof(T_NDB_MCU_DSP,
                                       a_cd)=0x1FC (vérifié gdb : d_fb_det=0x48→ARM
                                       0x1F0 ✓ cohérent FORCE_TOA, a_sync_demod=0x4C ✓).
                                       Avec 0x1DC le firmware lisait num_biterr=0xff +
                                       CRC fail (a_cd écrit À CÔTÉ → SI3 canned jamais
                                       atteint). Le "FIX 2026-05-28 0x1FC→0x1DC" était
                                       faux (validé sur un autre build, pas cet ELF). */
#define NDB_A_SCH26         0x54   /* [5] words */

/* ---- l1_environment.h constants ---- */
#define B_GSM_PAGE          (1 << 0)
#define B_GSM_TASK          (1 << 1)
#define B_SCH_CRC           8

#define PM_DSP_TASK         1     /* power measurement (l1s pm_cmd) — lit a_pm */
#define FB_DSP_TASK         5
#define SB_DSP_TASK         6
#define ALLC_DSP_TASK       24

/* ---- TCH/F voix (a′). JALON 1 = DL seulement ; UL (a_du_1) = JALON 3, gated. ----
 * Le shunt fait UNIQUEMENT le relais NDB ; le codage canal (gsm0503_tch_fr) est cote
 * qemu_wrap/gr-gsm. task ids : l1_environment.h:50-76. dsp_task_iq_swap (dsp.h:46) peut
 * OR 0x8000 -> comparer avec (& 0x7FFF). */
#define DUL_DSP_TASK        12     /* SDCCH/SACCH UL (sideband calypso_sdcch_ul existant) */
#define TCHT_DSP_TASK       13     /* TCH traffic : RX(d_task_d) ET TX(d_task_u) — le vrai trafic */
#define TCHA_DSP_TASK       14     /* TCH SACCH */
#define TCHD_DSP_TASK       28     /* TCH dummy (RX-only, PAS de data UL) — ne PAS relayer en UL */
/* Offsets NDB (BASE_API_NDB), confirmes DWARF layer1.highram.elf (T_NDB sizeof=0x18d4). */
#define NDB_A_DD_0          0x238  /* DL traffic FR sub0 : [0]@+0 hdr, [2]@+4 biterr, [3]@+6 (33o) */
#define NDB_A_DU_1          0x134  /* PIEGE #1 : UL sub0 = a_du_1 (PAS a_du_0=0x2A0) cf prim_tch.c:485. JALON 3. */
#define NDB_D_TCH_MODE      0x006
/* a_dd_0[0] header bits (l1_environment.h:267-270) */
#define B_FIRE0             5
#define B_FIRE1             6
#define B_BLUD              15     /* data block present (1<<15 = 0x8000) */

#define D_TOA               0
#define D_PM                1
#define D_ANGLE             2
#define D_SNR               3

/* ---- pending-task state ---- */
#ifndef SHM_IQ_LEN
#define SHM_IQ_LEN    320          /* int16 par slot (>= 296 = 148 complexes cs16) */
#endif

struct dsp_shunt_state {
    bool       active;                /* CALYPSO_DSP_SHUNT=1 */
    AddressSpace *as;                 /* ARM AS to peek/poke API RAM */
    /* latched task awaiting dispatch on next FRAME IRQ tick */
    bool       pending;
    uint8_t    page_idx;              /* 0 or 1 (B_GSM_PAGE) */
    uint16_t   d_task_md;             /* FB=5, SB=6, ... */
    uint16_t   d_task_d;              /* NB DL tasks */
    uint16_t   d_task_u;              /* NB UL */
    uint16_t   d_task_ra;             /* RACH */
    uint16_t   d_burst_d;
    uint16_t   d_fn;
    uint32_t   tick_cnt;              /* FRAME IRQ ticks since shunt enabled */
    /* TCH/F DL (JALON 1) : derniere trame FR 33o lue du sideband /dev/shm/calypso_tch_dl */
    uint8_t    tch_dl_fr[33];
    bool       tch_dl_valid;
    uint32_t   tch_dl_seq;
    /* SI réel injecté (gr-gsm ou démod C native) via calypso_dsp_shunt_feed_si.
     * Si si_valid, shunt_dispatch_allc écrit si_buf dans a_cd au lieu du canned. */
    uint8_t    si_buf[23];
    bool       si_valid;
    /* (A) Set SI COMPLET : un buffer par type (SI1/2/3/4/2bis/2ter). Sinon le
     * mobile ne reçoit qu'UN type → "No sysinfo yet" → sync timeout. On tourne
     * au début de chaque bloc (burst 0) pour tenir un type STABLE sur les 4
     * bursts (a_cd mono-frame ; sinon frame incohérente → CRC fail). */
    uint8_t    si_set[6][23];
    bool       si_set_have[6];
    uint8_t    sacch_buf[23];   /* SI5/SI6 (B4) SACCH dediee SS0 : REEL via feed_sacch
                                 * (fallback = fabrique depuis SI3 tant que !sacch_real) */
    bool       sacch_have;
    bool       sacch_real;      /* true des qu'un SI5/SI6 REEL grgsm est arrive ->
                                 * la fabrication SI3->SI6 cesse de clobber sacch_buf */
    int        si_rr;                 /* index round-robin du dernier type servi */
    /* Resultat de sync REEL poste par gr-gsm (= le DSP) via UDP SCH (4731,
     * shunt_sch_read). Remplace SHUNT_CANNED_BSIC dans shunt_dispatch_sb. */
    uint8_t    sb_bsic;               /* BSIC reel = ncc<<3|bcc (decode_sch gr-gsm) */
    uint32_t   sb_fn;                 /* FN reelle du SCH */
    int16_t    sb_toa;                /* TOA reel mesure du SCH (base 23 = on-time) */
    bool       sb_valid;              /* gr-gsm a poste au moins un SCH reel */
    /* AGCH (#11) : IMM ASSIGN (si_bridge GSMTAP AGCH 0x04). Stocke a part des SI
     * (pas de clobber) ; presente dans a_cd sur un bloc CCCH -> firmware chan_nr=0x90. */
    uint8_t    agch_buf[23];
    bool       agch_valid;
    uint32_t   agch_tick;            /* tick_cnt a l'arrivee (TTL anti-stale) */
    /* SDCCH/4 SS0 DL (#2) : UA/AUTH forwardes par si_bridge (GSMTAP 0x07).
     * Distinct des SI/AGCH ; presente dans a_cd sur le bloc SDCCH/4 SS0
     * (fn%51 in {22-25}) -> firmware tague chan_nr=0x20 -> LAPDm dediee. */
    uint8_t    sdcch_buf[23];
    bool       sdcch_valid;
    uint32_t   sdcch_tick;           /* tick_cnt a l'arrivee (TTL anti-stale) */
    /* PM REEL (no-hardcode) : magnitude moyenne du dernier burst DL (feed_iq).
     * Remplace le canned 0x7000 / le 0=-110 : le firmware en derive le vrai rxlev. */
    uint16_t   last_pm;
    /* CALYPSO_DSP=c54x : handle du VRAI DSP (relie via calypso_dsp_shunt_set_c54x()
     * depuis calypso_mb.c). NULL => route c54x inactive (fallback mock). */
    C54xState *c54x;
    /* Dernier burst I/Q DL stashe par feed_iq pour pilotage au frame tick
     * (cs16 entrelace I,Q). Rejoue via c54x_bsp_load dans shunt_route_to_c54x(). */
    int16_t    last_iq[SHM_IQ_LEN];
    int        last_iq_n;
    uint32_t   last_iq_fn;
    bool       last_iq_valid;
    /* [2026-07-22] Detection FCCH REELLE host-side (CALYPSO_SHUNT_REAL_FB) :
     * correle last_iq (vraie RX) -> d_fb_det/AFC/SNR/TOA reels (bypass go-live). */
    int        rx_fb_det;
    uint16_t   rx_snr;
    int16_t    rx_afc;
    uint16_t   rx_toa;
};

/* ---- Canned tuning ---- */
#define SHUNT_CANNED_TOA     23     /* raw → "on time" after -23 */
#define SHUNT_CANNED_PM      0x7000
#define SHUNT_CANNED_SNR     0x7000
#define SHUNT_CANNED_ANGLE   0
#define SHUNT_CANNED_BSIC    63

/* ---- CALYPSO_CANNED bitmask ---- */
enum {
    CAN_FBDET = 1u << 0,   /* d_fb_det = 1 ("FB found") forcé          */
    CAN_TOA   = 1u << 1,   /* a_sync/serv_demod[TOA] = 23 ("on time")  */
    CAN_PM    = 1u << 2,   /* [PM] = 0x7000 (rxlev fort)               */
    CAN_SNR   = 1u << 3,   /* [SNR] = 0x7000 (passe AFC_SNR_THRESHOLD) */
    CAN_ANGLE = 1u << 4,   /* [ANGLE] = 0 (AFC convergé, pas de chasse)*/
    CAN_CRC   = 1u << 5,   /* a_sch[0]/a_cd status = 0 (CRC forcé pass)*/
};
#define CAN_ALL (CAN_FBDET|CAN_TOA|CAN_PM|CAN_SNR|CAN_ANGLE|CAN_CRC)
#define CAN_DEFAULT (0u)

/* ---- Shared state (DEFINED in calypso_dsp_shunt.c) ---- */
extern struct dsp_shunt_state g_shunt;
extern unsigned g_canned;

/* FN TDMA reelle (calypso_trx.c) — utilisee par les primitives + la glue. */
extern uint32_t calypso_trx_get_fn(void);

/* ---- Log tag helper + macros (both TUs use them) ---- */
const char *shunt_tag(void);
#define SHUNT_LOG(fmt, ...) fprintf(stderr, "%s " fmt, shunt_tag(), ##__VA_ARGS__)
#define SHUNT_ERR(fmt, ...) error_report("%s " fmt, shunt_tag(), ##__VA_ARGS__)

/* ---- Mode-neutral NDB-write primitives (calypso_dsp_helper.c) ---- */
bool shunt_route_c54x(void);
uint16_t shunt_read_w(uint32_t addr);
void shunt_write_w(uint32_t addr, uint16_t v);
uint32_t shunt_l1s_fn(void);
uint32_t shunt_last_rach_fn(void);
uint32_t wp_base(uint8_t page_idx);
uint32_t rp_base(uint8_t page_idx);
bool shunt_is_canned(unsigned bit);
int shunt_toa_val(void);
uint32_t shunt_encode_sb(uint8_t bsic, uint16_t t1, uint8_t t2, uint8_t t3);
void shunt_dispatch_fb(uint8_t page_idx);
void shunt_dispatch_sb(uint8_t page_idx);
void shunt_dispatch_allc(uint8_t page_idx);
void shunt_dispatch_pm(uint8_t page_idx);

#endif /* CALYPSO_DSP_INTERNAL_H */

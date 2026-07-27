# TODO - Calypso QEMU (par mode, 2026-07-27)

Le STATUT ET LES TODO DEPENDENT DU MODE. Le socle = `calypso.env` + un
`calypso_X.env`. Voir la matrice statut x mode dans `ETAT_ACTUEL.md` §1.

Modes :
- **SHUNT** = SHUNT_LEGIT=1 / SHUNT_NO_LEGIT=1 / SHUNT_LEGIT=DSP,NO_CANNED.
  FBSB host-side (real_fb + gr-gsm -> API RAM). Camp + LU + SMS = DONE.
- **NATIF** = CALYPSO_NATIVE=1 / CALYPSO_NATIVE_HELPED=1. DSP c54x fait le FBSB.

Priorites : P1 = debloque la feature courante du mode ; P2 = suivant ; P3 = dette.
Reference d'adresses : `SHUNT_LEGIT_ADDRESS_MAP.md`, `DSP_ADDRESS_MAP.md`,
`DSP_ARM_LINKAGE.md`.

DEJA FAIT (ne pas re-lister) : go-live / handshake BGEN (Fix A, ARM->DSP
0x098a/0x098c), shadow IMR 0x435b (=0x52ed arme), boucle Location Update en
SHUNT (LU ACCEPT + TMSI REALLOC + On Network) et le RACH UL qui la fermait.

---

## SHUNT (mode fiable : camp + LU + SMS = DONE)

- **[P1] Voix TCH/F - lever l'ASSIGNMENT FAILURE**
  - Etat : ASSIGNMENT COMMAND est atteint puis ASSIGNMENT FAILURE ; le shunt ne
    presente pas le TCH DL. Cote reseau OK (call fake_trx = ACTIVE + audio).
  - Quoi : (a) presenter le TCH DL host-side vers le mobile ; (b) capturer le
    FACCH UL a_fu @0x282 pour boucler l'assignation.
  - Ou : chemin DL/UL shunt ; [`VOIX_PLAN.md`](VOIX_PLAN.md) ; a_fu @0x282.

- **[P2] Fiabiliser SMS en SHUNT_LEGIT=DSP,NO_CANNED (flaky)**
  - Etat : SMS MO/MT = DONE en SHUNT_LEGIT/NO_LEGIT ; WIP flaky en DSP,NO_CANNED
    (anti-stall deja ajoute, encore intermittent).
  - Quoi : stabiliser MO/MT quand le DSP c54x tourne en // sans cannes.

- **[P3] Refacto `sdcch_ring` (CODE - a faire en FOREGROUND, pas ici)**
  - Quoi : `fn` stocke-non-utilise ; eviction silencieuse ; ajouter TTL + drop
    explicite. Dette de structure, pas un blocage fonctionnel.
  - Note : refacto code a mener en session foreground dediee, NE PAS coder depuis
    la reorg doc.

---

## NATIF (DSP c54x fait le FBSB - WIP)

> Reecrit 2026-07-28. Le natif **n'avait jamais tourne** avant cette date (SIGSEGV au
> boot + DSP jamais sorti de son bootloader) : toute mesure anterieure est
> ininterpretable. Detail : `ETAT_ACTUEL.md` §3, `../../../RAPPORT_DFBDET.md` §8-9.

- **[P0] ✅ FAIT — rendre le natif observable** (4 defauts « le natif depend du shunt »)
  - SIGSEGV `g_shunt.as = NULL` ; early-boot gate sur le routage shunt ; `get_task_md()`
    shunt-only ; split `active()` / `substitutes()`.
  - Effet mesure : DSP a la cadence trame (`dsp_n_exec_2/5 = 32768`) et
    **`DSP_ERR_STACK_OV` eteint**.

- **[P1] Pointeur d'entree du demod jamais initialise** ← *le verrou courant*
  - Mesure (`CALYPSO_DEMODRD`, `XPC=0`) : le demod lit ses echantillons a
    `data[0x0000]`, `[0x0005]`, `[0x000a]` (pas de 5) — que des zeros ; `AR6 = 0000` sur
    toutes les iterations, alors que `AR4` (ecriture) parcourt bien `0x2a00+`.
  - Consequence : **toute l'IQ injectee depuis des semaines n'a jamais ete lue**
    (`0x2a00`, `0x9213/0x9215`, `0x9260/0x9261`).
  - Hypothese a tester EN PREMIER : on entre en `0x9500` alors que l'entree ROM est
    `0x94f5` -> 11 mots de mise en place sautes. `CALYPSO_FB_CORR_ENTRY=0x94f5`
    (**verifier la valeur dans `/proc/<pid>/environ`** : un run l'a recue tronquee).
  - Si insuffisant : identifier qui pose `AR6` en amont, et le reproduire.

- **[P2] Slot de dispatch FB = stub `RET`**
  - `0xb01c: 10f8 43d8` (adressage **absolu**, ni `0x4387` ni `0x43c0`).
    `data[0x43d8] = 0xab38`, dont le 1er mot est `fc00` = `RET`. Unique ecrivain sur les
    4 banks : `0xbb00`. Aucun writer cache (watchpoint dans `data_write_locked`).
  - ⚠️ **NE PAS refaire** le correctif TPU de `DOC_PATH_BOOT_TO_CORRELATOR_2026-07-25.md` :
    `0x8341` a **0 reference** sur 4 banks, et `0x7234 -> 0x013b` est un **CALL ROM
    inconditionnel** qui retourne (mesures 28/07).
  - Bequille de validation dispo : `CALYPSO_BSP_DISPATCH_FB=1` (+ `_TGT`, `_NOIMR`).

- **[P3] `d_fb_det` : personne ne l'ecrit en natif**
  - Verifie des 2 cotes du miroir api_ram (`CALYPSO_FBDET_API`) : seul l'ARM touche la
    cellule, toujours a 0. Le `FB0_SEARCH -> SB_SEARCH` observe est le **renoncement**
    d'osmocom, pas une detection (`BSIC=0`, `snr=0`).
  - Depend de P1 puis P2.

- **[P4] Remplacer gr-gsm par le DSP DANS `SHUNT_LEGIT`** (plan 2026-07-28)
  - Le FB ne depend **deja plus** de gr-gsm (correlateur hote `REAL_FB`, 280/300
    detections). Restent SCH (`sb_bsic/sb_fn/sb_toa`) et SI (`si_buf`).
  - Couper gr-gsm : `CALYPSO_SHUNT_NO_GRGSM=1`.
  - Piloter le correlateur DSP depuis le shunt : `CALYPSO_SHUNT_DSP_FB=1`
    (excursion **bornee** `_MAX`, **pile dediee** `_SP` — sans elle on corrompt la pile
    du DSP et `STACK_OV` revient).
  - Ordre : FB (fait) -> SCH -> SI. Oracle a chaque etape = le producteur actuel.

## Dette transverse (tous modes, P3)

- Retirer les env `BURST_*` mortes (BURST_FN / BURST_OFS / BURST_ECHO) une fois
  `d_burst_d` WP-mirror valide au run.
- `a_pm` mot 8 vs mot 12 : section A de `on_frame_tick` ecrit sous label "a_pm"
  les idx 0x30/0x44 (= a_serv_demod) ; vraie cellule a_pm = 0x834/0x848.
- Code mort a supprimer : `bsp.fb_valid` jamais mis a 1 ;
  `calypso_dsp_shunt_route_c54x_active()` sans appelant ; API
  `calypso_orch_init/publish` inexistante ; `calypso_tint0_start()` jamais
  reference ; `fw_console.c` sans appelant.
- Decodeur c54x : cluster MAC/LD/BITT 0x30-0x37 mort sous case 0xF ; BC/FB 0xF8
  par nibble ; catch-all FIRS/LMS 0xE000-E3FF ; 0xF6/0xF7 fabriques.
  NE PAS re-fixer 0x72/0x73 MVDM/MVMD sans le side-effect (REVERT_MVMD).
- Threading (roadmap) : un thread par bloc (ARM/DSP/TPU/BSP) pour eliminer les
  ~300 artefacts de serialisation TCG (THREADING_TODO).
- Doc : `DSP_ROM_MAP.md` canonique dit "PROM1 mirroree 0x8000-0xFFFF" ; corriger
  en "chargee 0x18000+ sans mirror" (fix 2026-05-29). Marquer "historique" les
  REPORT_CLAUDE_WEB_* / BOOT_TO_FBSB_SEQUENCE / FBSB_SEQUENCE_TRACE.

---

Note taxonomie : `api_write_cb` (calypso_c54x.h) = callback mort, fausse piste
(cf `ETAT_ACTUEL.md` §6 "ne pas theoriser dessus") -> retire des TODO.

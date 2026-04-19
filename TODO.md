# Calypso QEMU — TODO / état des lieux

## Session 2026-04-07 (suite) — vraie addr DARAM trouvée + opcodes manquants identifiés

### Découverte majeure : `CALYPSO_BSP_DARAM_ADDR=0x3fc0`

Après réactivation de `l1ctl_sock_init` dans `calypso_soc.c` (QEMU
propriétaire de `/tmp/osmocom_l2_1`, bridge.py démuni de son rôle L1CTL),
le DSP est sorti de sa boucle initiale et a commencé à exécuter le
FB-det handler. L'histogramme `BSP-ENV-CHECK` s'est rempli :

```
[DBG_BSP] BSP-ENV-CHECK reads=201509618
  top: [0x3fc0..0x3fff]=201013734  [0x1280..0x12bf]=13323  [0x28c0..0x28ff]=2275
  cfg=[0x3fc0..0x4040]  ENV=OK (top1 in window)
```

99.7% des reads DSP dans `[0x3fc0..0x3fff]`. **Hardcodé** :
- `calypso_bsp.c:55-58` : `daram_addr=0x3fc0`, `daram_len=64` par défaut.
- `run.sh` et `run_all_debug.sh` : `CALYPSO_BSP_DARAM_ADDR=0x3fc0`.

### Réactivation L1CTL côté QEMU

- `calypso_soc.c:227-237` : `l1ctl_sock_init(&s->uart_modem, ...)` décommenté.
- `bridge.py` : `srv = None` (plus de bind sur la socket Unix), poll
  register conditionnel. Bridge garde TRXD/TRXC/CLK UDP relay et la
  lecture PTY pour les autres DLCIs.
- Le hook `l1ctl_sock_uart_tx_byte` était déjà câblé dans
  `hw/char/calypso_uart.c:505` pour le label `"modem"`.

Doc complète : `docs/L1CTL_SOCK_FLOW.md` (init, accept, format wire
len-prefix big-endian, sercomm wrap/unwrap, burst gating, master/slave
table, ordre de démarrage, sync avec osmocom L23).

### Opcodes manquants identifiés (chemin critique FB-det handler)

Trace après réactivation L1CTL :
- **`UNIMPL @0x79dd: 0x75e8 (hi8=0x75)`** — dans le **FB-det handler**
  PROM0 [0x77xx..0x79xx]. Hi8=0x75 = MVPD pmad,Smem (move from program
  memory to data memory) selon SPRU172C. **Priorité 1** car directement
  sur le chemin critique : après cet opcode, le DSP "tombe" dans des
  NOPs, walk linéairement, et finit en `NOP-SLIDE PC=0xff3b`.
- **`UNIMPL @0xc001: 0xf210 (hi8=0xf2)`** — opcode F2xx PROM1 non géré.
- **`UNIMPL @0xfffc: 0x7981 (hi8=0x79)`** — autre wedge zone.
- **`F5xx unhandled: 0xf58e PC=0x79a8`** — F5xx hors dispatch.
- **`BC unknown cond=0x42/0x44/0x45/0x4d`** aux PCs 0x79a6, 0x79c7,
  0xc003, 0xc010 — group 1 conditions retirées en début de session,
  retombent en fall-through systématique → branchements ratés.
- **`SP-WRITE-BAD: SP <= 0x0000 PC=0x79d9`** — un MMR write force SP=0
  juste avant le wedge. Cause secondaire à investiguer.
- **`NDB WR [0x08fb]=0xff3a PC=0x79de` puis `NOP-SLIDE PC=0xff3b`** —
  bizarre : le DSP écrit `0xff3a` dans NDB puis le PC saute à
  `0xff3a/0xff3b`. Probablement un store+load+branch tordu post-MVPD
  manquant.

### Wedges DSP observés (différents runs, même cause racine)

Le DSP wedge se déplace entre runs :
- Session 1 : `0xb3fc/0xb3fe` (2-insn loop, op=0x0000 NOP).
- Session 2 : `0x6f70..0x6f8f` (linear walk, op=0x0000 NOP).

Cause commune : **CALL/RET balance massivement déséquilibré** :
```
[DBG_CALL] CALL/RET balance: calls=64 rets=28461 delta=-28397 SP=0x00d4
```
28461 RETs spurieux pour 64 CALLs. Chaque RET pop garbage de la stack,
PC saute n'importe où, finit dans une zone NOP de PROM. Le coupable
est probablement un opcode misdecodé dans le dispatch (mask error)
qui s'exécute comme RET.

Trace blocks ajoutés dans `calypso_c54x.c` :
- `0xB3F0..0xB400` (B3FC WEDGE) — détecté ce run.
- `0x6F70..0x6F9F` (6Fxx WEDGE) — détecté précédent run.
- Bloc existant `0xB5E0..0xB5FF` (B5xx).

Tous dumpent op + op+1..op+3 + IMR/IFR/INTM/SP/XPC + ring buffer
des 8 PCs précédents.

### Plan d'attaque immédiat

1. **Implémenter MVPD `0x75xx`** dans le dispatch — ça devrait
   débloquer le FB-det handler à PC=0x79dd.
2. **Implémenter F2xx `0xf210`** (sub-decoder à creuser).
3. **Réimplémenter les BC group 1 conds** (0x42/0x44/0x45/0x4d) avec
   les bonnes polarités tirées de tic54x-opc.c.
4. Tracer la source des spurious RETs pour identifier l'opcode mal
   dispatché.

---

## Session 2026-04-07 — fbsb branché et FB phase OK

Wins :
- Module `calypso_fbsb.c/h` créé, branché dans `calypso_trx.c` (init + hook
  `d_task_md` + hook frame tick).
- Hook `on_dsp_task_change` publie immédiatement `d_fb_det=1` +
  `a_sync_demod[TOA/PM/ANGLE/SNR]` via `publish_fb_found(0, 80, 0, 100)`
  dès que l'ARM écrit `d_task_md=FB_DSP_TASK`. Pas d'attente, pas de
  race avec le 12-poll cycle de `prim_fbsb`.
- Doublon `dsp_ram[0xF8] = 1` dans `calypso_trx.c` (lines 573-582)
  supprimé. fbsb seul propriétaire des writes NDB FB.
- Offsets NDB validés par comptage de la struct DSP=36 dans
  `osmocom-bb/.../dsp_api.h` :
  `d_fb_det = NDB+36 words = dsp_ram[0xF8] = DSP word 0x08F8`.
- Hack `tint0.fn = 0` dans `calypso_tint0_start` retiré (la BTS ne
  reset jamais fn).
- Forwarding TRXD BTS→gate dans `bridge.py` retiré.
- `hack_gdb.py` débranché dans `run_all_debug.sh`.
- Doc complète : `docs/FBSB_FLOW.md` (entrée `l1s_fbsb_req` →
  `fb_sched_set` 14 frames → `l1s_fbdet_cmd` → 12 polls
  `l1s_fbdet_resp` → 3 retries × 30 AFC retries → `l1a_fb_compl` →
  `l1ctl_fbsb_resp(255 ou 0)`).

Effet observé :
- Wedge DSP a bougé de `0xb3fc/0xb3fe` (idle 2-insn) vers `0x6f7f..0x6f90`
  (zone NB/BCCH, code beaucoup plus loin dans le flow).
- L'ARM écrit maintenant `d_task_md=0x0005` (DDL_DSP_TASK, downlink
  dummy) après le FB → ce qui veut dire que le firmware a passé FB0/FB1
  et tente la réception SB/BCCH.

Reste à faire (next coup) :
- **Phase SB** : `l1s_sbdet_resp` lit `dsp_api.db_r->a_sch[0..4]` et
  vérifie le bit `B_SCH_CRC=8`. fbsb ne publie rien pour SB → 2 attempts
  → `attempt=13` → `result=255`. Hook `DSP_TASK_SB` ajouté +
  `publish_sb_found(bsic=0)` → écrit `a_sch[0..4]=0` (CRC OK) dans
  les 2 pages db_r (`dsp_ram[0x28..]` et `dsp_ram[0x3C..]`). MAIS
  observation: l'ARM n'écrit jamais `d_task_md=2` (SB) dans le tmux,
  donc le hook ne se déclenche pas → la chaîne firmware ne progresse
  pas jusqu'au SB. À investiguer.

- **Wedge DSP `0x6F70..0x6F9F`** : trace block ajouté dans
  `calypso_c54x.c` (sur le pattern existant `B5xx`). Au prochain run,
  cherche `6Fxx WEDGE PC=0x6Fxx op=0xXXXX op+1=...` → ça donne
  l'opcode mal émulé qui forme la boucle de 16 PCs. Une fois identifié,
  fix dans le dispatcher → DSP avance → exécute `[0x7730..0x7990]` →
  histogramme `BSP-ENV-CHECK` se remplit → vraie addr DARAM trouvée.

- **`run.sh`** : env vars + venv ajoutées (CALYPSO_BSP_BYPASS_BDLENA=1,
  DEBUG=ALL, CALYPSO_DBG=all, DARAM_ADDR=0, DARAM_LEN=128, source
  /root/.env si présent).

- **Histogramme BSP-ENV-CHECK indécouvrable circulaire** : tant que le
  DSP est wedgé hors de la zone FB-det `[0x7730..0x7990]`, il ne fait
  pas les reads DARAM nécessaires (`obs_count > 10000`) pour que le
  dump périodique se déclenche. Donc la vraie addr DARAM ne peut être
  trouvée qu'**après** avoir débloqué le wedge `0x6Fxx`.

Bugs résiduels code review fbsb :
- `names[]` array sans bounds check dans `calypso_fbsb_dump`.
- Compteurs `uint8_t` wrappent à 256.
- Branches `FB0_FOUND/FB1_FOUND` dans `on_frame_tick` désormais inutiles
  (publish synchrone) — code mort, à nettoyer.

---

État au 2026-04-07. Snapshot après les fixes :
- CALLD/RETD/BD délayés (delay slots)
- ROL/ROR/ROLTC implémentés
- F273 BD (était RETD wrongly)
- F5xx fallback NOP au lieu de RPT #k
- AVIS/APTS interrupt push fix (Calypso n'a pas APTS)
- INT3 vec 19 pour FRAME IRQ DSP
- RPTB wrap range (REA+1 ou REA+2)
- Dédup F272/F273/F274 dans dispatch F2xx
- SP-OOR tracer init fix

## Statut courant

| Composant | État |
|---|---|
| Tic-tac frame ARM↔DSP | ✅ INT3 |
| Pages NDB toggle | ✅ 0x02/0x03 |
| ARM L1S scheduler | ✅ tourne |
| DSP boot + IMR setup | ✅ IMR=0xFF88 / 0xFFF5 |
| DSP FB correlator (0x8208-0x8218) | ✅ tourne sur vrais samples |
| SNR calculé | ✅ 30-58 (variance) |
| FBSB result=0 | ❌ SNR < seuil firmware |
| DATA_IND | ❌ pas atteint |
| SP stable | ⚠️ stable dans ~75% des runs, wrap dans 25% |

---

## Session 2026-04-07 nuit — wedge PC=0/1 reset loop (BLOQUANT)

**Symptôme persistant** observé sur tmux/runs après les fixes BC F8xx :
- PC HIST top: `0000:98040 0001:98040 c600..c621:19608` chacun
- DSP loope sur reset vector (PC=0/1) → exécute c600..c621 → revient à PC=0
- ARM côté l1ctl reçoit `FBSB_CONF result=255 snr=0` puis `RESET_CONF` en boucle
- Mobile log : `MM_EVENT_NO_CELL_FOUND` répétitif
- Cycle ~3s entre chaque reset → c'est l'ARM qui timeout sur FBSB et fait RESET_REQ

**État du code à ce moment** :
- F8xx complètement réécrit en `BC pmad, COND` (cf binutils tic54x-opc.c)
- Plus de hallucination F82x=RPTB / F83x=RPT / F86x=BANZ / F88x=B / F8Cx=CALL
- Conds implémentées: 0x00 (true), 0x20 NTC, 0x30 TC. Reste fall-through par défaut.
- Run 78/79 (avant ce dernier batch d'ajouts conds groupe 1) avait `snr=37` et 766k lignes — c'était notre meilleur état.
- Tentative d'ajouter conds 0x42-0x4F (AGEQ/AGT/etc), 0x60/0x70 (AOV/ANOV), 0x08/0x0C (NC/C) → REGRESSION immédiate, retour au wedge PC=0/1.

**Hypothèses pour le wedge PC=0/1** :
1. Une de mes nouvelles conds groupe 1 a la mauvaise polarité → BC prend une mauvaise direction → PC corrompu
2. PROM[0x7000] = `f880 b360` = `BC #b360, cond=0x80`. cond=0x80 inconnue. Avec default fall-through (run 78/79), tombe sur PROM[0x7002] = fc00 = RC. RC return-conditional, condition=0 → toujours retour. SP au boot a une return address valide (poussée par le bootloader ?). Ça marche ? Pas clair.
3. PC=0/1 atteint via un RET avec SP corrompu → return address = 0 (data zero).

**Action immédiate** : revert à `case 0x00 + 0x20 + 0x30 only` (matching run 78/79 minimum). Si SNR remonte à 37, on a confirmé que c'est l'extension qui casse. Ensuite réintroduire les conds une par une avec validation.

**À DOCUMENTER ENSUITE** :
- la table cond byte exacte avec polarités validées
- où le firmware sort de ce wedge dans la version "qui marche" (run 78/79)

---

## Session 2026-04-07 soir — batch 0x80-0x9F (REGRESSION)

**Action**: réécrit handlers 0x80-0x9F en STL/STH/STLM/ST T/ST TRN/CMPS/STH-SHFT (binutils tic54x-opc.c) pour remplacer les wrong MVDD/MVPD/MVDP/MVKD qui squattaient cette plage. Ajouté MV* à 0x70/71/7C/7D.

**Résultat runs 26 / 41 / 42**:
- Pas de wedge — DSP tourne, FBSB_CONF émis
- **snr=58 → snr=0** régression nette
- AR2 figé à 0 dans FBLOOP-RD (avant: variable)
- Pas de UNIMPL nouveau côté 0x80-0x9F

**Diagnostic possible**:
1. CMPS en double : mon nouveau à 0x8E (calypso_c54x.c:2985) overshadow l'ancien à 0xA5 (calypso_c54x.c:3337). L'ancien marchait pour le Viterbi du correlator, le mien probablement pas (encoding différent).
2. STH SHFT à 0x9A : encoding très approximatif (`shift = (op >> 8) & 0xF` est faux, c'est un bitfield différent).
3. STLM à 0x88 : peut overshadow un ancien handler à 3311+ qui gérait correctement.
4. Les anciens "wrong" handlers MVDD/MVPD/MVDP/MVKD étaient peut-être *fonctionnellement compatibles* avec ce que le firmware attendait (genre coïncidence sémantique sur les paths utilisés).

**Action piste**:
- Désactiver mes 0x88/0x8E/0x9A pour rendre la main aux anciens (3311/3337/etc)
- Garder uniquement STL 0x80, STH 0x82, ST T 0x8C, ST TRN 0x8D
- Re-tester. Si snr remonte → mes CMPS/STLM/STH-SHFT sont coupables, faut les corriger un par un.

**MV* 0x70-0x7D toujours UNIMPL en run 31**:
Les opcodes 0x7045/0x712b/etc tombent en UNIMPL malgré mes handlers `hi8==0x70/0x71` ligne 3015+. Diagnostic: il y a probablement un `return` antérieur dans le dispatch qui short-circuit la plage 0x70-0x7F (les anciens handlers à 2393/2408 hi8==0x76/0x77 sont là, mais aussi peut-être un fallthrough plus haut). À investiguer: tracer le dispatch path pour op=0x7045.

**Nouveaux UNIMPL vus run 31** (pas couverts du tout) :
- `0x72xx` (mvdm dmad,MMR ou mvmd MMR,dmad)
- `0x73xx` (port?)
- `0x75f8`
- `0x79e8`

---

## Bugs critiques découverts 2026-04-07 soir

### MISENCODAGES MASSIFS — handlers à la mauvaise adresse opcode

Tous ces opcodes sont mal encodés dans `calypso_c54x.c`. Comparaison avec
`/home/nirvana/gnuarm/src/binutils-2.21.1/opcodes/tic54x-opc.c` :

| Mnemonic | Vrai opcode | Notre handler | Statut | Impact |
|---|---|---|---|---|
| **Bloc parallel ST||X 0xC0-0xDF** | 0xC000-0xDFFF mask FC00 (8 paires) | divers C0/C1/C2/C5/CC/CD/CE/DA/DD/DE | ✅ FIX appliqué (élargi mask) | SP corruption majeure |
| PSHM MMR | 0x4A00 mask FF00 | absent (0xC5 = wrong) | ⏳ TODO | stack |
| POPM MMR | 0x8A00 mask FF00 | 0x8A = MVDK (wrong) | ⏳ TODO | stack |
| PSHD Smem | 0x4B00 mask FF00 | 0xC0 = wrong | ⏳ TODO | stack |
| POPD Smem | 0x8B00 mask FF00 | 0x8B (peut-être MVDP wrong) | ⏳ TODO | stack |
| FRAME #k | 0xEE00 mask FF00 | 0xCE = wrong + 0xEE = ? | ⏳ TODO | SP arith |
| DELAY Smem | 0x4D00 mask FF00 | 0xDF = wrong | ⏳ TODO | FIR delay |
| RPT Smem | 0x4700 mask FF00 | 0xC1 = wrong | ⏳ TODO | repeat count |
| SACCD | 0x9E00 mask FE00 | 0xCC = wrong | ⏳ TODO | conditional store |
| MVDK Smem,dmad | 0x7100 mask FF00 | 0x8A (wrong, en collision avec POPM) | ⏳ TODO | data move |
| MVKD dmad,Smem | 0x7000 mask FF00 | 0x9A (wrong) | ⏳ TODO | data move |
| MVDD Xmem,Ymem | 0xE500 mask FF00 | 0x88/0x80 (wrong) | ⏳ TODO | data move |
| MVPD pmad,Smem | 0x7C00 mask FF00 | ? | ⏳ TODO | prog→data |
| MVDP Smem,pmad | 0x7D00 mask FF00 | ? | ⏳ TODO | data→prog |

**Méthode pour traiter chacun :**
1. Vérifier qu'aucun autre handler n'occupe déjà la VRAIE adresse
2. Si oui : déplacer/supprimer le mauvais
3. Implémenter à la bonne adresse selon le format binutils
4. Build + tester si SNR/SP s'améliorent
5. Cocher la case et passer au suivant

---

## Ordre de priorité

### P0 — bugs bloquants restants

#### P0.1 Variance SP wrap (3/13 runs)
- **Symptôme** : SP grimpe à 0x81fb / 0xffb7 / 0x6dea sur certains runs au lieu de rester ~0x5AC8
- **Hypothèse** : push/pop asymétrique restant ailleurs (POPM/PSHM, F4xx return variants, ou interrupt nesting)
- **Action** : tracer les delta SP > N par instruction sur run wrappé, identifier l'opcode coupable
- **Fichier** : `calypso_c54x.c` autour de l'interrupt + RETE/RETF/FRET

#### P0.2 IDLE wake race
- **Symptôme** : variance run-à-run du chemin pris par le DSP en early init
- **Hypothèse** : IRQ qui arrive avant que IMR soit configuré → DSP réveillé dans un état partiel
- **Action** : ajouter délai/check `dsp_init_done` avant le 1er FRAME IRQ
- **Fichier** : `calypso_trx.c:c54x_interrupt_ex` et le startup TINT0

### P1 — opcodes manquants à fort impact

#### P1.1 F2xx C548 enhanced (range 0xF2C0..0xF2DF)
- **Vu en runtime** : 22 opcodes distincts utilisés dans le RPTB block 0x8d5a..0x8da1
- **Pas dans tic54x-opc.c standard** (binutils 2.21 n'a que F272/F273/F274)
- **Hypothèses** : FIR symétrique unrollé / POLY / MAC #k4
- **Action** :
  - Option A — Tracer tous les hits avec snapshot acc/AR/T/status pré/post → reverse par observation
  - Option B — Trouver SPRU172C C548 enhancement supplément (TI doc ou autre source)
  - Option C — Implémenter comme MAC #imm avec coef=low4bits et tester
- **Risque** : moyen (peut casser autre chose)
- **Gain attendu** : +20-50 points SNR, possible FBSB result=0

#### P1.2 F2xx individuels (hors range Cx/Dx)
Liste vue : `F210, F211, F261, F280, F2A0, F2A1, F2A8, F2AF, F2B4, F2B8`
- **Action** : grep tic54x-opc.c étendu / SPRU172C, implémenter au cas par cas
- **Gain** : variable

#### P1.3 F5xx individuels restants
Vu : `f58e, f520, f500, f585`
- **Action** : décoder via tic54x-opc.c (rotates, shifts, accumulator ops)
- **Gain** : faible-moyen

#### P1.4 F6xx unhandled
Vu : `f6e2, f6a0`
- F6E2 = `baccd src` (BACC delayed) — déjà partiellement traité ?
- F6A0 = ?
- **Action** : implémenter BACC/CALAD/FBACC famille délayée

#### P1.5 6F00 ext (LD/ST/ADD/SUB Smem,SHIFT)
- **Vu** : `UNIMPL 6F op=0x6f40 op2=0xcc80`
- **Status** : handler existe à `calypso_c54x.c:2540` mais ne couvre pas tous les sub-opcodes (op2=0xcc80 = STLM ?)
- **Action** : compléter le switch sur op2

### P2 — qualité

#### P2.1 PC HIST nettoyage
- Le pc_hist est verbose. Garder mais filtrer les hot paths attendus (correlator inner loop) pour mieux voir les anomalies.

#### P2.2 CALL/RET counter symétrique
- **Bug counter** : compte F074/F274 côté CALL mais FC00/F4EB côté RET. Ajouter les interrupts (push) pour symétrie ; sinon delta n'a aucun sens.

#### P2.3 Dump DSP ROM rangée code vs data
- `calypso_dsp.txt` a 2 rangées par adresse pour certaines plages → ambiguïté. Documenter laquelle est PROM, laquelle DROM, ou splitter en 2 fichiers.

### P3 — features

#### P3.1 FB seuil firmware
- Lire `prim_fbsb.c` pour `FB0_SNR_THRESH` exact, comparer avec SNR observé.
- Si seuil = 100 et SNR max observé = 58 : besoin d'environ 2× plus de précision côté correlator.

#### P3.2 cfile contenant une vraie FCCH burst
- Vérifier que `/root/out_arfcn_100.cfile` contient bien une frequency burst au bon offset.
- Si non : générer un cfile avec FCCH explicite (sinusoïde 67.7kHz × 148 symboles).

#### P3.3 DATA_IND path
- Une fois FBSB result=0 atteint, vérifier que ARM enchaîne sur SB → BCCH → DATA_IND.
- Hooks BPs disponibles dans `calypso_hack_gdb.py`.

---

## Bugs identifiés mais pas encore reproduits

- **DBG_CORRUPT** sporadique : NDB d_dsp_page écrit avec une return address (0xb908). Vu sur runs antérieurs, pas reproduit récemment.
- **F074 SP runaway PROM0 0xb906** : cascade hypothèse #2 (CALL inside RPTB, wrap loop), à investiguer si SP wrap revient.

---

## Outils dispo

- `run_all_debug.sh` : 10 runs auto, logs dans `/root/logs/qemu_NNN.log`
- `calypso_hack_gdb.py` : forçage ARM-side FBSB success (hack pédagogique)
- `calypso_gdb.py` : observation pure GDB (no patch)
- `c_patches.md` : patches C confirmés vs spéculatifs
- `calypso_dsp.txt` : dump PROM+DROM C54x

---

## Sessions précédentes (référence)

Voir `MEMORY.md` index dans `/root/.claude/projects/-home-nirvana/memory/`.

# CLAUDE_CODE_TASK — go-live c54x, ordonné par dépendance

> Source : brief advisor (2026-07-14), calé sur les logs nu confirmés cette session.
> Épistémique : **instrumenter avant d'éditer, arbitrer la fourche des docs avant de
> toucher au control-flow**, Rule #1 partout.
>
> **Ancres vérifiées dans l'arbre (2026-07-14)** :
> - `calypso_dsp_shunt_active()` : ✓ `calypso_bsp.c:404/462/956`
> - `api_write_cb` : ✓ `calypso_c54x.c:3403-3404`
> - T5 OOB : ✓ bug réel — `calypso_arm2dsp.c:115-116` n'a que le check `>= A2D_API_BASE`,
>   pas de borne haute `< C54X_API_SIZE`.
> - T1 tests : fichiers présents (`tests/test_run_observability.py`, `tests/test_calypso_milestones.py`).
> - ⚠️ commit `c279ba6` introuvable dans `git log` — le bug T5 reste réel, la réf commit est à re-sourcer.

---

## CONTRAINTES (T0 — valables sur toutes les tâches)
- **Rule #1** : réparer **le câblage de l'émulateur uniquement**. Aucun hack, aucun poke
  d'état DSP interne, aucune valeur cannée. Ne pas réimplémenter le DSP.
- **Shunt-safety** : toute modif du cœur c54x doit être dead-code quand
  `CALYPSO_DSP_SHUNT=1` (vérifier via `calypso_dsp_shunt_active()`). Ne pas casser le shunt.
- **Keepers à ne pas revert** : fixes MVKD `0x70`, READA `rpt_fresh`, STL/STH, `resolve_xmem`.
- Toute sonde ajoutée : `fprintf(stderr,…)` gaté ou compteur monitor, jamais une modif de flux.

## T1 — Exposer les compteurs ISR via monitor (no-regret, en premier)
Cible : `hw/arm/calypso/calypso_c54x.c` (+ `.h`) + enregistrement commande `info` monitor.
Compteurs (incrémentés au bon site, exposés en lecture) :
- `interrupt_ex_called` — dans `c54x_interrupt_ex()`.
- `isr_entered` — entrée d'ISR (post-vectoring, prologue).
- `rete_executed` — exécution de `RETE`.
- `pending_irq_gated` — IRQ latchée en IFR mais masquée par IMR/INTM.
Exposer via `hmp`/`info calypso-irq` (ou équivalent existant).
**Acceptance** : `test_interrupt_ex_called_counter_exposed`, `test_isr_entered_matches_rete`,
`test_no_pending_irq_gating`, `test_c54x_interrupt_ex_called_nonzero`,
`test_isr_entered_implies_rete` passent. Ces compteurs sont l'instrument de T4 — livrés d'abord.

## T2 — Arbitrer la fourche go-live (READ-ONLY, aucune édition de flux)
Contradiction docs : `STATUS_2026-07-01` → `api_write_cb` non-assigné → ARM n'écrit que
`0x0000` en `0x0314/0x0318` → wait-loop `0xa4d4`. `calypso_audit.md` (07-03) → `d[0x3f70]`
= **red herring**, vrai mur = **Break 1** = slot dispatch `data[0x4387]` (lu par `BACC @0xb40f`)
qui se re-résout au stub `0xab38` au lieu de `0xa4c7`. Trancher par sondes read-only :
- Tracer chaque write ARM en `0x0314/0x0318` (DSP words `0x098a/0x098c`) : valeur + PC ARM.
  Confirmer qu'elle est toujours `0x0000`.
- Tracer la résolution de `data[0x4387]` à chaque `BACC @0xb40f` : valeur lue, cible calculée,
  hits sur `0xa4c7` (attendu 0).
- Tracer si `api_write_cb` serait appelé si assigné (`calypso_c54x.c:3403-3404` atteint ? `woff` ?).
**Acceptance** : rapport `doc/GOLIVE_FORK_ARBITRATION.md` disant lequel des deux chemins est la
vraie voie morte, preuves de log. **Ne pas éditer de control-flow avant ce verdict.**

## T3 — Fix go-live (conditionnel au verdict T2)
- Si voie `api_write_cb` : assigner le notifieur DSP→ARM (`api_write_cb`/`api_write_cb_opaque`)
  côté glue ARM (`calypso_trx.c`/`calypso_arm2dsp.c`) pour propager l'enable — **sans poker**
  `data[0x3f70]` ni forcer l'IMR.
- Si Break 1 : corriger la liveness du dispatch `data[0x4387]` → résout vers `0xa4c7` par le
  mécanisme silicon correct (câblage émulateur), pas par remap forcé.
**Acceptance** : run c54x (`SHUNT=0`, `MODE=full`), `IMR` passe `0x0000 → 0x3000` **nativement**
(grep `IMR=0x3000` > 0), sans aucun levier `CALYPSO_*FORCE*`/`POKE`/`SEED` actif.

## T4 — Audit Break 3 : fidélité push PC/XPC dans `c54x_interrupt_ex` (READ-ONLY puis fix)
Avec les compteurs T1 : dès que T3 fait entrer une ISR, vérifier `isr_entered == rete_executed`.
Le derail `CALL 0x013b → PC=0x0000` s'enracine dans le contexte poussé par `c54x_interrupt_ex`.
Comparer (désassemblage ROM) ce que le prologue ISR (`CALL 0x013b`, `RETE`) **dépile** vs ce que
`c54x_interrupt_ex` **empile** (ordre, XPC présent, nombre de mots).
> Note session : push confirmé 2-mots (PC+XPC, c54x.c:13982-87) ; RETE dépile 2 (5118-27) ;
> mais le ROM sort l'ISR via **POPM ST1 + RCD (1-mot)**, pas RETE (comment 5142-44) → XPC orphelin.
**Acceptance** : `isr_entered == rete_executed` sur un run, et exécution atteint `0xa4e4`
(dispatch → corrélateur) ≥ 1 fois (grep dispatch FB > 0).

## T5 — Régression : restaurer la borne OOB dans `calypso_arm2dsp.c`
`calypso_arm2dsp.c:115-116` : l'OR-write `s->api_ram[a2d_word - A2D_API_BASE] |= a2d_bit`
n'a que le check bas `>= A2D_API_BASE`, **pas** de borne haute `< C54X_API_SIZE`. Si
`CALYPSO_ARM2DSP_TASKWORD` (env, jusqu'à `0xFFFF`) n'est pas borné en amont de façon prouvable,
c'est une écriture hors-buffer. Restaurer `(unsigned)(a2d_word - A2D_API_BASE) < C54X_API_SIZE`
**ou** `assert()` + clamp.
**Acceptance** : impossible d'OR-écrire au-delà de `api_ram[C54X_API_SIZE]` ; test d'invariant si possible.

## T6 — (repo firmware `osmo_egprs/firmware`, séparé) Museler le print `LOST`
`printf("LOST %u!")` du L1 crache 143k lignes quand la sync échoue. Rate-limit ou flag debug
OFF par défaut. **Pas un bug** — bruit aval du go-live qui noie osmocon + charge l'UART partagé.
S'auto-résout quand T3+T4 passent ; à museler d'ici là pour des logs propres.

## NE PAS RETENTER (dead-ends confirmés)
- `CALYPSO_DSP_FRAME_VEC28` force-vectorise → épilogue `0x013b` déraille (c'est T4, pas un fix).
- `CALYPSO_SEED5AC8` / go-live forcé au boot → SP corrompu (`0x5ac8` vs `0x1100`).
- Poke `d[0x3f70]` bit1 seul → insuffisant (red herring, gate `d_background_enable` zeroé par le firmware).

## CRITÈRE GLOBAL DE SUCCÈS
`d_fb_det(0x01F0) ≠ 0` produit par le **vrai** corrélateur DSP (pas `CALYPSO_ORCH` synthétique),
et bascule de la colonne milestone xfail (`fb0_att` → … → `location_updating_accept`).

## ORDRE D'ATTAQUE
**T1 → T5** (rapides, sans risque) → **T2** (arbitrage read-only) → **T3** → **T4**.
T1 et T4 sont couplés : les compteurs T1 rendent T4 mesurable au lieu de narratif.

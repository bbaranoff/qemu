# TODO — chemin FBSB QEMU Calypso

État au 2026-04-26 fin de soirée.

## État courant

Le bootloader DSP termine enfin son handshake avec l'ARM. Le DSP exécute le user
code à PROM0 0x7000 puis dans une grande zone DARAM uploadée (0x10ab → 0x148d
→ 0x2f13 → ...). Plus de stack-overflow, plus de wait loop infini. La BTS ne
crash plus pour clock skew. **FB1/FB2 jamais imprimé** : le DSP n'écrit pas
encore d_fb_det avec une vraie détection — INTM probablement encore à 1.

## Pipeline utilisé

```
docker exec -it trying bash -c "killall -9 qemu-system-arm 2>/dev/null; \
  rm -f /tmp/osmocom_l2 /tmp/qemu-calypso-mon.sock /tmp/qemu_l1ctl_disabled; \
  bash /opt/GSM/qemu-src/run_new.sh"
```

Pour injecter des FB bursts directement (diagnostic) :
```
docker exec trying python3 /opt/GSM/qemu-src/scripts/inject_fb.py
```

## Fixes appliqués cette session (cumul)

| Fichier | Fix | Effet mesuré |
|---|---|---|
| `bridge.py` | `fn = bts_fn` (anchor double supprimé) | delta 797 → 24 |
| `bridge.py` | CLK IND wall-clock (471 ms) | BTS ne shutdown plus pour PC clock skew |
| `calypso_bsp.c` | default daram_addr = 0x3fb0 | DSP lit le bon buffer BSP |
| `calypso_bsp.c` | BSP_FN_MATCH_WINDOW = 64 | Stale ratio /10 |
| `calypso_timer.c` | byte access + sémantique CNTL silicon (bit 5 CLOCK_ENABLE, prescaler 4:2) | "LOST 0!" disparaît |
| `calypso_timer.c` | mode lazy (count interpolé depuis virtual time) | LOST diff converge vers ~1885 (au lieu de chaotique) |
| `calypso_c54x.c` | opcode SFTC `0xF494/F594` | Fin du F4xx unhandled spam |
| `calypso_c54x.c` | F4E4 = vrai FRET (workaround retiré) | DSP retourne de 0x770c à 0xb41a |
| `calypso_c54x.c` | F6E2/F6E3/F6E4/F6E5/F6E6/F6E7/F6EB/F69B (delayed B/CALA/RET/RETED) | Returns from interrupt enabled |
| `calypso_c54x.c` | STLM 1-word pour 0x88xx/0x89xx | PC sync correct, plus de skip d'instruction |
| `calypso_c54x.c` | case 0x1 = LD/LDU/LDR (était SUB faux) | A correctement chargé |
| `calypso_c54x.c` | case 0x0 = ADD/ADDS/SUB/SUBS proper | Plus de << 16 implicite |
| `calypso_c54x.c` | resolve_smem MOD 12-15 réordonnés (15 = `*(lk)` absolute) | Bootloader lit BL_ADDR_LO=0x7000 |
| `calypso_c54x.c` | CMPM 0x60xx + BITF 0x61xx (sets TC) | Wait loop bootloader sort enfin |

## Tracers ajoutés

- `[c54x] DYN-CALL` : tous les BACC/CALA (F4E2/F5E2/F4E3/F5E3) avec data[]/prog[] dump
- `[c54x] FRET #N` : exec FRET (PC retour, SP, XPC)
- `[c54x] RETED #N` : exec RETED
- `[c54x] WATCH-READ #N data[0x0ffc..0x0fff]` : data vs api_ram (mailbox bootloader)
- `[c54x] WATCH-READ d_fb_det[0x08F8]` : real d_fb_det (était 0x01F0 erroné)
- `[c54x] WAIT-3DD0` : DSP polls data[0x3dd0]
- `[c54x] DISP-PTR data[0x3f65]` : dispatcher pointer
- `[calypso-trx] BL ARM WR` : ARM-side writes à BL_ADDR_HI/SIZE/LO/CMD_STATUS

## Indicateurs au dernier run

| Métrique | Valeur |
|---|---|
| FRET count | 1 |
| RETED count | 0 |
| RETE count | 0 |
| DSP WR d_fb_det count | 0 |
| DYN-CALL count | 1 (BACC à 0x7000 ✓) |
| IDLE count | 0 (DSP run vraiment) |
| IRQ INT3 servies | 0 (INTM=1 toujours bloque) |
| FB1/FB2 print firmware | 0 |

## Bug racine restant

**INTM=1 forever** : aucun RSBX INTM (`0xf6bb`) jamais exécuté. Le DSP visite
maintenant énormément de zones DARAM différentes (0x10ab → 0x148d → 0x2f13 →
...) mais on ne voit jamais visite à PROM0 0xa4d0/0xa510/0xa6c0/0xc660/...
(addresses des RSBX INTM par CLAUDE.md).

Sans clear INTM, l'INT3 frame interrupt fire mais est rejetée, le DSP ne
process pas task_md=5, n'écrit pas d_fb_det.

## TODO ordre priorité

### Priorité A — Atteindre RSBX INTM

**#1 Tracer où le DSP devrait normalement faire RSBX INTM**
- Toutes les RSBX INTM sont dans les ISR (INT3, BRINT0, etc.) selon CLAUDE.md
- Pour entrer dans une ISR, il faut INTM=0. Catch-22.
- Au boot, INTM=1 par défaut (reset value de ST1).
- **Hypothèse** : le code uploadé en DARAM par le DSP user contient un RSBX
  INTM dans son init. Il faut le trouver. Tracer 0xf6bb à chaque exec.
- **Action** : ajouter dans calypso_c54x.c F6Bx handler un log "RSBX-INTM
  exec PC=…" si bit==11.

**#2 Vérifier CMPM/BITF / autres opcodes manquants**
- L'audit a révélé que beaucoup d'opcodes 0x60xx-0x67xx sont mal couverts
  (seul 0x60/0x61 sont fixés). Voir tic54x-opc.c lignes pour 0x6200-0x67FF :
  MPY Smem,lk; MAC Smem,lk; etc. À faire selon besoin si DSP spam unimpl.

**#3 Continuer audit case 0x4-0x7 du switch**
- case 0x2, 0x3 : MAC/MAS/MPY variants (vérifier le shift et les flags)
- case 0x4, 0x5 : ADD/SUB Smem,SHIFT,SRC1
- case 0x9 : LD Xmem,SHFT,DST etc
- Beaucoup d'opcodes PROM peuvent être mal décodés silencieusement.

### Priorité B — Si INT3 servies

**#4 BSP delivery efficiency**
- STALE ratio toujours élevé (~50:1) malgré window=64
- Si le DSP atteint enfin son inner correlator, peut-être augmenter window à 128

**#5 Nettoyer les workarounds latents**
- Le f4e4=IDLE workaround a été retiré. Peut-être d'autres workarounds
  similaires dans le code (chercher comments "Until X is fixed").

## Run config courante

`run_new.sh` passe `CALYPSO_SIM_CFG="$MOBILE_CFG"` à QEMU pour le module SIM
(session 04-26 matin) et désactive l'ancien injection hack.

## Session précédente (matin/après-midi 2026-04-26)

Voir mémoires `project_session_20260426*.md`. Résumé des découvertes :
- Module SIM ISO 7816 émulé natif (calypso_sim.c)
- Hacks fw_patch supprimés (no INJECT, no fw_patch_apply)
- BSP daram fix initial 0x3fb0
- Bridge anchor fix
- Multiple opcode fixes (SFTC, F6Ex, etc.)

## Session de soirée (cette session, 2026-04-26)

L'audit CLAUDE.md a révélé une cascade de **7 bugs d'opcode majeurs** :
- F4E4 workaround → vrai FRET
- F6Ex tous décodés en MVDD
- 0x88/0x89 STLM décodé en MVDM 2-word
- case 0x1 = SUB au lieu de LD/LDU
- case 0x0 = ADD avec << 16 toujours
- Indirect MOD table 12-15 décalée d'1
- 0x60xx/0x61xx CMPM/BITF jamais implémentés

Cette série a permis au bootloader de terminer (BACC à 0x7000) et au DSP
d'entrer dans le user code. Le bug INTM=1 reste.

## Issues annexes

### tmpfs /tmp 16G

`qemu.log` peut atteindre 12G+ avec tous les tracers. Surveiller `df -h /tmp`.

### Link `-lm` cassé (workaround manuel)

```bash
cd /opt/GSM/qemu-src/build
ninja -t commands qemu-system-arm | tail -1 > /tmp/link.sh
sed -i 's|$| -lm|' /tmp/link.sh && bash /tmp/link.sh
```

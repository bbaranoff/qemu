# TODO — chemin FBSB QEMU Calypso

État au 2026-04-26 fin de session (suite session matin).

## État courant

**Run end-to-end NO-HACK** : QEMU + osmocon natif + bridge + BTS + mobile.
- Firmware ARM tourne propre (battery_init, DSP API check, PM scan complet 514→823, boucle FBSB)
- SIM ISO 7816 / GSM 11.11 émulée natif (IMSI/Ki chargés du `mobile.cfg` partagé avec le mobile L23)
- 0 hack firmware restant, 0 inject/cpu_physical_memory_write, fw_console poller actif

**Bug racine restant** : DSP n'entre **JAMAIS** dans le code FB-det (PORTR PA=0xF430 jamais exécuté).

## Architecture nouvelle (à connaître pour next session)

### Module SIM (`calypso_sim.c`, ~480 lignes)
- Header `include/hw/arm/calypso/calypso_sim.h`
- Wirage dans `calypso_trx.c`: `s->sim = calypso_sim_new(s->irqs[CALYPSO_IRQ_SIM])`
- Registres `0xFFFE0000-0xFFFE000E` : CMD/STAT/CONF1/CONF2/IT/DRX/DTX/MASKIT
- IT_RX **level-sensitive** (recompute depuis FIFO), IRQ raise/lower (pas pulse)
- IT_WT timer (2ms après FIFO drain) — débloque `calypso_sim_powerup` polling rxDoneFlag
- File system : MF, DF_GSM, DF_TELECOM + EFs IMSI/ICCID/PLMNsel/SST/ACC/AD/LOCI/BCCH/SPN
- APDU dispatch : SELECT (A4), READ_BINARY (B0), GET_RESPONSE (C0), STATUS (F2),
  VERIFY_CHV (20), RUN_GSM_ALGORITHM (88)
- IMSI/Ki chargés au boot depuis `getenv("CALYPSO_SIM_CFG")` (= `$MOBILE_CFG` via run_new.sh)

### Logs diagnostic en place
| Tag | Source | Quoi |
|-----|--------|------|
| `[sim]` | `calypso_sim.c` | init, ATR, IRQ raise/lower, WT, APDU |
| `[fw-console]` | `fw_console.c` | printf_buffer firmware (poller 10ms) |
| `BSP-ENTRY` | `calypso_c54x.c` | DSP visite 0x9a78/0x9aaf/0x9ad3/0x9b4c/0x8811 |
| `DISP-WRITE` | `calypso_c54x.c` | Write data[0x4359] ou data[0x3fab] |
| `DISP-TRACE` | `calypso_c54x.c` | Dispatcher hot loop 0xb968-0xb9a4 |
| `PORTR PA=` | `calypso_c54x.c` | Tous accès I/O port DSP |
| `MVPD#N` | `calypso_c54x.c` | Move Program→Data au boot |
| `VEC-TRACE` | `calypso_c54x.c` | IRQ vec slots 0xFFCC-0xFFE0 |

### Fix opcodes appliqués
- **F820 = BC if A != 0**, **F830 = BC if A == 0** (surgical override, le reste F8xx garde l'ancien décodage)
- **PORTR PA=0xF430 = BSP RX** (en plus de 0x0034 pour compat)
- (CALLD/RETD delay slots restent à faire — voir #4)

## TODO ordre priorité

### Priorité A — Diagnostic FB-det final

**#1 Tracer BACC/CALA dynamiquement**
- Aucun chemin STATIQUE depuis dispatcher 0xb990, IRQ vecs 0xFF80-FFB4, ou boot 0xB410 vers les FB functions (0xa0c9, 0xa0ce, 0xa140, 0xa0ed, 0x8817, 0x8811)
- Hypothèse : le call chain passe par BACC (Branch on Accumulator) ou CALA (Call Accumulator), invisibles en analyse statique
- Action : ajouter dans `calypso_c54x.c` un log `DYN-CALL src=PC tgt=A_low` à chaque exec BACC/CALA. Si on voit cible dans `0xa0XX`-`0xa1XX` ou `0x88XX`-`0x99XX`, le chemin existe et il faut comprendre quand il fire.

**#2 Vérifier upload firmware DSP par osmocom-bb**
- 0 MVPD jamais exécuté dans tous nos runs (confirmé)
- Question : est-ce que osmocom-bb upload un firmware DSP en DARAM via une autre méthode (ARM writes dans api_ram → DSP exécute) ?
- Grep `/opt/GSM/osmocom-bb/src/target/firmware/` pour : `dsp_load`, `dsp_upload`, `dsp_patch`, `dsp_dump`
- Si oui, la séquence n'est probablement pas déclenchée — tracer ARM writes à api_ram

**#3 Si vraiment FB code dead dans ce ROM**
- Soit chercher un AUTRE dump DSP firmware Calypso (peut-être dans `osmocom-bb` patches ou scripts/dsp_dump_*)
- Soit accepter que FBSB ne marchera pas et chercher path alternatif (BTS-side mock du résultat ?) — **dernière option, gros hack**

### Priorité B — Cleanup (à faire après FB-det validé)

**#4 Étendre delay slots à CALLD/RETD**
- RCD utilise déjà delayed_pc/delay_slots (fix session 04-25)
- CALLD (F274) et RETD (F273) sautent leurs 2 delay slots — peut-être source d'anomalies subtiles

**#5 Cleanup logs diagnostic**
- DISP-TRACE, BSP-ENTRY, DISP-WRITE, MVPD-SUMMARY, VEC-TRACE — beaucoup de tracers en place
- Une fois FB-det validé, factoriser ou guarder par flag env

## Run config courante

```bash
# run_new.sh passe maintenant CALYPSO_SIM_CFG="$MOBILE_CFG" à QEMU
docker exec -it trying bash -c "killall -9 qemu-system-arm 2>/dev/null; \
  rm -f /tmp/osmocom_l2 /tmp/qemu-calypso-mon.sock /tmp/qemu_l1ctl_disabled; \
  bash /opt/GSM/qemu-src/run_new.sh"
```

Pour mobile cfg avec FBSB forcé (sans attendre PM detect) :
```
ms 1
  ...
  test-arfcn-stick 514   # ou: stick 514
```

## État branche

Pas de commit récent — toutes les modifs en working tree sur les 3 emplacements (`/home/nirvana/qemu-src/`, `/home/nirvana/qemu/`, container `/opt/GSM/qemu-src/`).

**Fichiers modifiés/nouveaux session 04-26 (suite)** :
- `hw/arm/calypso/calypso_sim.c` (nouveau, ~480 lignes)
- `include/hw/arm/calypso/calypso_sim.h` (nouveau)
- `hw/arm/calypso/calypso_trx.c` (suppression hacks fw_patch + wirage SIM)
- `hw/arm/calypso/calypso_c54x.c` (fix F820/F830, PORTR 0xF430, traces BSP-ENTRY/DISP-WRITE)
- `hw/char/calypso_uart.c` (suppression appel fw_patch_apply)
- `include/hw/arm/calypso/calypso_trx.h` (suppression proto)
- `hw/arm/calypso/meson.build` (ajout calypso_sim.c)
- `hw/arm/calypso/fw_console.c` (poller printf_buffer — déjà session matin)
- `run_new.sh` (ajout CALYPSO_SIM_CFG=$MOBILE_CFG)

## Historique sessions précédentes

(Archive complet dans memory `/root/.claude/projects/-home-nirvana/memory/`)

### Session 2026-04-26 matin
VEC-TRACE révèle cycle IRQ tortueux via boot stub. Fix FRET expose 3 bugs latents (MVPD/ISR/mem[0x0ffe]).
TODO original (#4 MVPD trace, #5 DARAM 0xa04c, #6 mem[0x0ffe], #7 PMST stack) maintenant **OBSOLÈTE** :
- #4 résolu : 0 MVPD exécuté (boot DSP via C-code copy au reset, pas par MVPD opcode)
- #5 résolu : PROM0 0xa000+ contient code dense, pas vec table — l'hypothèse IPTR=0x140 était fausse
- #6 résolu : mem[0x0ffe]=0x10 hors-path (FRET reverté en IDLE)
- #7 résolu : PMST stable à 0xffa8 (IPTR=0x1FF), corruption unique à 0xdbc7

Le vrai bug est le shift de focus identifié 04-26 matin : dispatcher 0xb990 ne dispatch pas. Maintenant on sait pourquoi : **aucun chemin statique vers le FB code**.

### Session 2026-04-25 (RCD fix)
- RCD delay slot mécanisme introduit
- 0x7700 loop cassée, FBSB_CONF (type=0x02) envoyé au mobile

### Sessions 2026-04-04 → 2026-04-17
Voir mémoires `project_session_2026040X.md` à `project_session_20260417.md`.
Highlights : bridge server, INTH refactor, opcode fixes (50+), DSP boot stabilisation,
FB sample buffer localisé à DARAM[0x021f] (12K mots), inner loop FB-det à PROM0 0xa10d.

## Issues annexes (inchangées)

### Link `-lm` cassé (workaround manuel)
```bash
cd /opt/GSM/qemu-src/build
ninja -t commands qemu-system-arm | tail -1 > /tmp/link.sh
sed -i 's|$| -lm|' /tmp/link.sh && bash /tmp/link.sh
```

### `/tmp` tmpfs 16G
`qemu.log` peut atteindre 12G+ avec tous les tracers. Surveiller `df -h /tmp`.

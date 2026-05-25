# Status session 2026-05-25 (night) — DSP correlator lock chase

## TL;DR

- ✅ Pipeline E2E **fonctionnel** (bridge→BSP→DSP, ARM L1S scheduler, L1CTL mobile↔fw)
- ✅ **INTM=1 forever isolé comme partie du blocker** (= BRINT0 jamais dispatched naturally)
- ✅ Bypass osmo-bts validé : `BRIDGE_BURST_SOURCE=internal` génère FB bursts qfn-gated
- ✅ Bug **0xf210 SUB #lk** F2xx fix + F3xx INTR mis-décode fix
- ❌ Correlator FB-det **n'écrit pas snr/toa réels** (= 0xFFFF garbage post-force, mais state corrupted by force = inconcluant)
- 🎯 **Front unique = F1 : pourquoi ISR INT3 ne RETE pas naturellement**

## Architecture verrouillée

Bridge → BSP UDP `:6702` → DARAM[0x3FB0] → DSP correlator

- BSP_DARAM_ADDR=0x3fb0 confirmé OK (DSP read 0x3FD3 = +35 in buffer, 14k reads)
- Pas de confondre avec 0x2C00/0x3D94 = buffers DSP-internal (SARAM scratch)
- BDLENA rising edge OK, BRINT0 fire 1× puis bloqué par INTM=1

## Decoder fixes appliqués (visibles dans BUILD-IDENT)

```
F1xx-FIRS-catch=REMOVED
L3609-src-dst=FIXED
F-AUDIT-v5=max-min-cmpl-rnd-roltc-fixed
F2xx-ALU-block=ADDED-2026-05-25-night
F3xx-INTR-mis-REMOVED-ADD-SUB-LD-ADDED
```

## Sondes ajoutées (env-gated, no fix, just observe)

| Env var | Quoi |
|---|---|
| `CALYPSO_CORRELATOR_TRACE=1` | CORR-ENTRY range étendu 0x8d00-0x9000, log entry+context |
| `CALYPSO_FBDB_PROBE=1` | B@fbd9 + A@fbdb + A@fbf3 + r/w 0x3DC0 |
| `CALYPSO_STUCK_PROBE=1` | PC+XPC histogramme quand INTM=1 + BRINT0 pending |
| `CALYPSO_FORCE_INTM_ONESHOT=1` | clear INTM 1× one-shot (= sonde diagnostic, JAMAIS fix) |
| `CALYPSO_FORCE_INTM_AT_PC=0xXXXX` | restreindre force au PC (= safe-PC test, c web caveat) |
| `CALYPSO_INT3_CYCLE_TRACE=1` | **NOUVEAU** : par cycle INT3, log toutes branches conditionnelles. Diff offline good vs orphan = 1ère branche divergente = trigger |

## Bridge sondes (env-gated)

| Env var | Quoi |
|---|---|
| `BRIDGE_BURST_SOURCE=internal` | Bypass osmo-bts, bridge génère FB bursts via `_handle_dl()` |
| `BRIDGE_BURST_PATTERN={fcch,empty}` | fcch=148 zéros (tone pur), empty=0xFF (sanity check) |
| `BRIDGE_CLK_MODE={wall,hybrid,qfn-pure,wall-qfn}` | 4 modes CLK_IND |
| _clk_thread dédié | CLK_IND wall-paced indépendant TRXD select |

## Partition c web validée

Force-INTM=0 one-shot → IRQ dispatched, ISR runs, CORR-ENTRY +1, a_sync writes from runtime PCs (0xfb72/0xfb88) **but values = 0xFFFF garbage**.

**Per c web caveat** : forcer INTM=0 n'importe où mid-ISR autorise nesting BRINT0 que firmware ne sanctionne PAS → state clobberé. **Pas de "PC sûr" pour forcer mid-ISR**. Donc 0xFFFF = peut venir du state corrupted, **pas preuve d'aval cassé**.

→ **1 seul front, pas 2** : chasser F1 (RETE manquant). Si post-fix correlator écrit encore 0xFFFF, ALORS F2 existe. Pas avant.

## INT3 cycle observations (à exploiter avec INT3_CYCLE_TRACE)

Boot timeline INTM-TRANS observé :
- `#1 0→1 @insn=255970 PC=0x9DCC` (= INT3 vec @ IPTR=0x13B, ISR entry #1)
- `#2 1→0 @insn=2627748 PC=0x7707` (= **RETE depuis ISR #1, GOOD**)
- `#3 0→1 @insn=2630352 PC=0x7707` (= ISR entry #2)
- Puis NO MORE INTM-TRANS → ISR #2 stuck, never RETE.

Entre insn=2.6M et stuck observation = **~5M insns dans ISR sans RETE**. C'est LA fenêtre à analyser.

Avec INT3_CYCLE_TRACE : on aura signature complète des branches de ISR #1 (GOOD) vs ISR #2 (ORPHAN). Diff = 1ère branche divergente.

## Pour relance décisive

```bash
CALYPSO_INT3_CYCLE_TRACE=1        # NEW : log branches par cycle ISR
CALYPSO_STUCK_PROBE=1
CALYPSO_CORRELATOR_TRACE=1
BRIDGE_BURST_SOURCE=internal
BRIDGE_BURST_PATTERN=fcch
BRIDGE_CLK_MODE=wall
CALYPSO_BSP_DARAM_ADDR=0x3fb0
CALYPSO_ICOUNT=auto
CALYPSO_MTTCG=0
CALYPSO_PCB_TICK_THREADS=0
# NE PAS activer CALYPSO_FORCE_INTM_ONESHOT (= sonde retirée, corrompt state)
```

Run quelques secondes, dump `[c54x] INT3-CYCLE` lines, diff cycle #1 (RETE-GOOD) vs cycle #2 (ORPHAN-NEXT-INT3). Première branche où `next_pc` diffère = trigger du bug.

## Fichiers modifiés cette session

```
hw/arm/calypso/calypso_c54x.c   (+ probes : FBDB, STUCK, FORCE_INTM, INT3_CYCLE)
hw/arm/calypso/calypso_bsp.c    (DARAM lock helpers, déjà committed)
hw/arm/calypso/calypso_trx.c    (DARAM lock helpers, déjà committed)
hw/arm/calypso/calypso_fbsb.c   (DARAM lock helpers, déjà committed)
hw/arm/calypso/calypso_full_pcb.c (DARAM helpers + virtual-clock tick threads)
bridge.py                        (CLK_MODE wall/hybrid/qfn-pure/wall-qfn,
                                 BURST_SOURCE internal, _clk_thread dédié)
run.sh                           (BUILD-IDENT detection, menu profiles)
```

Sync : host `/home/nirvana/qemu-src/` ↔ host `/home/nirvana/qemu-calypso/` ↔ container `trying:/opt/GSM/qemu-src/`

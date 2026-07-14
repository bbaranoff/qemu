# GOLIVE_FORK_ARBITRATION — T2 (2026-07-14)

Arbitrage READ-ONLY de la fourche go-live, sur run nu vivant (PID courant,
`SHUNT=0 MODE=full`, cheats OFF sauf SEED pour franchir le storm ; `FORCE-IMR=0`,
`VEC28-FORCE=0`, `ARM2DSP=0`). Instrument : compteurs T1 + sondes existantes.

## La fourche
- **STATUS_2026-07-01** : `api_write_cb` non-assigné → l'ARM n'écrit que `0x0000` en
  `0x0314/0x0318` (DSP `0x098a/0x098c`) → wait-loop `0xa4d4`, IMR jamais armé.
- **calypso_audit.md (07-03)** : `d[0x3f70]` = red herring ; le vrai mur = **BP1**,
  le slot dispatch `data[0x4387]` (lu par `BACC@0xb40f`) qui se re-résout au stub
  no-op `0xab38` au lieu de `0xa4c7` (`ORM #0x3000,IMR`), boucle auto-référentielle.

## Les 3 symptômes — CONFIRMÉS (preuves log)
| # | Fait | Preuve |
|---|------|--------|
| a | ARM écrit `0x0314/0x0318` **toujours `0x0000`**, 4× au **boot (fn=0)**, jamais après | `[trx] HS-ARM-GATE #1..4 off=0x0314/0x0318 val=0x0000 fn=0` ; writes non-zero 098a/098c = **0** |
| b | `data[0x4387] = 0xab38` (stub, seule valeur) ; `0xa4c7` **jamais exécuté** | `SLOT4387-WR data[0x4387] <- 0xab38 PC=0xb4bd` (×2) ; 0 saut `tgt=0xa4c7` (les "2 hits" = les `DISPPTR-WR data[0x43c0]<-0xa4c7`, pas des dispatch) |
| c | `api_write_cb` **jamais assigné** (branche `c54x.c:3403 if(s->api_write_cb)` morte) ; le DSP écrit pourtant 14 cellules API (le site EST sollicité) | grep `api_write_cb *=` → ∅ ; `DSP-API-WR` = 14 @PC=0xb446 |

## Le piège méthodologique (pourquoi 3 confirmations n'arbitrent PAS)
Les trois sondes confirment **leur symptôme** — mais la fourche n'est pas « quel
symptôme existe », c'est **lequel CAUSE l'autre** :
- **BP1 aval de api_write_cb** : le slot résout au stub *faute d'entrée* — le boot-done
  DSP n'est jamais signalé à l'ARM (cb=NULL), donc l'ARM ne pose jamais l'enable que le
  dispatcher attend. Prédiction falsifiable : `data[0x4387]` **varierait** si une écriture
  ARM non-nulle arrivait en `0x0314/0x0318`.
- **BP1 autonome** : le slot boucle sur lui-même *quoi que fasse l'ARM* ; l'enable ARM
  n'y change rien. Prédiction : même avec `0x0314/0x0318` non-nul, `data[0x4387]` resterait
  au stub. api_write_cb serait alors un **second** mur, pas la cause de BP1.

**Le seul discriminateur RO** = « `data[0x4387]` varie-t-il avec `0x0314/0x0318` ? ».
Il est **INOBSERVABLE** : `0x0314/0x0318` est constant à `0x0000` (boot-only) sur tout le
run naturel — il n'existe **aucune** occurrence où l'ARM écrit non-nul pour voir si le slot
bouge. Forcer un write serait un **edit de flux** = dead-end #3 documenté (`FORCE_GOLIVE`,
faux positif garanti). **Non fait.**

## Lean RO (indice, PAS preuve)
Finding antérieur (audit osmocom-bb, cette session) : le L1 osmocom-bb **n'écrit JAMAIS**
`0x098a/0x098c` non-nul — ces cellules ne sont dans aucun `ndb->X =` du firmware. Les 4
`HS-ARM-GATE val=0x0000` sont le **zéroing d'init** de l'API RAM, pas un write d'enable
ciblé. → Câbler `api_write_cb` ne ferait pas *apparaître* un enable ARM non-nul, puisque le
firmware n'a pas ce write. Ça **penche BP1-autonome** (l'enable est DSP-auto, le dispatcher
doit résoudre `data[0x4387]→0xa4c7` par son mécanisme interne). Mais c'est un lean, pas le
discriminateur — le firmware pourrait piloter le go-live autrement que par un write direct.

## VERDICT
**Non-tranchable en READ-ONLY pur.** Les deux hypothèses ne divergent que quand l'ARM écrit
non-nul en `0x0314/0x0318`, ce qui n'arrive jamais dans le run naturel. Confirmer les trois
symptômes ne donne pas le lien de causalité.

**Le discriminateur est le PREMIER edit de T3** : câbler `api_write_cb` (édit minimal,
fidèle — assigner le notifieur DSP→ARM côté glue, sans poke IMR/3f70) et **observer** :
- si `data[0x4387]` se met à résoudre vers `0xa4c7` (+ IMR `0x0000→0x3000`, + `isr_entered>0`
  au compteur T1) → **api_write_cb était la racine, BP1 son symptôme.** T3 = ce câblage.
- si `data[0x4387]` reste au stub `0xab38` malgré cb câblé → **BP1 est autonome**, api_write_cb
  est un mur séparé, et T3 doit viser la **liveness du dispatcher** directement (l'île morte
  `0xa9ea-0xc800`).

Autrement dit : **T2 ne se ferme pas en RO ; il se ferme au premier test de T3.** Le premier
edit de T3 *est* l'expérience qui tranche la fourche. Le lean RO parie BP1-autonome, à
confirmer/réfuter par ce test.

## Prochaine action
T3, sous-cas « câbler api_write_cb » **en premier** (c'est le test le moins invasif et il
arbitre T2). Compteurs T1 = l'instrument de lecture : `isr_entered` passe >0 ⇔ le go-live a
franchi le mur.

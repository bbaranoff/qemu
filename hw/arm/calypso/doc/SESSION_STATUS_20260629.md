# SESSION STATUS — Calypso EXE DSP go-live — 2026-06-29

## TL;DR
État **stable et propre**. Un vrai bug mort (RSBX/SFTL, vérifié). Trois faux-coupables
éliminés proprement. Le mur central = **cercle d'amorce**, localisé à l'instruction
(cellule `d[3fde]`, mécanisme = l'interrupt bootstrap n'est jamais armé).
**Prochain coup unique, à tête reposée : décoder le bootloader 0x7000.**

---

## ✅ FIXÉ & VÉRIFIÉ — RSBX/SFTL (le vrai gain de la session)
- **Bug** : handler SFTL `if ((op & 0xFCE0) == 0xF4A0)` (calypso_c54x.c ~L5530 + ~L5794).
  Le mask `0xFCE0` masque bit4 ; RSBX/SSBX = nibble `0x_Bx`(1011) vs SFTL `0x_Ax`(1010)
  ne diffèrent QUE par bit4 → `f6bb`(RSBX INTM)/`f7bb`(SSBX INTM)/`f4bb`/`f5bb` avalés et
  exécutés comme shifts d'accu → **INTM gelé à sa valeur d'init (1)**.
- **Fix** : ajouter `&& (op & 0xF0) != 0xB0` aux 2 handlers SFTL.
- **Vérifié runtime (2 runs)** : `RSBX-FIRE @0xa6cf ST1 0x2900->0x2100` fire ;
  `INTM-CLEAR #1 @0xa6d0 / #2 @0xdde7` fire (~130 insn). **INTM respire.**

## ❌ RÉFUTÉS — NE PAS re-chasser (chacun tué par la donnée)
1. **Gap miroir mailbox** (ARM écrit data[]/dsp_ram[] mais pas api_ram[] ; DSP lit api_ram[]
   pour [0x0800,0x2800)). RÉFUTÉ par sonde GOLIVE-MIRROR : `data[0x0c36]=api_ram=0x7002`
   COHÉRENT. Le fix trx api_ram[] **n'a PAS été appliqué** (confirm-before-fix a évité le faux fix).
2. **Staging ARM manquant / zéros**. RÉFUTÉ : l'ARM stage avec de vraies commandes —
   `TASKPTR=0x7002` livré ET lu par le DSP, handshake bootloader OK (`data[0x0fff]=1`).
3. **Entrée control-flow** (« le scheduler devrait viser 0xa4c7 mais vise 0xa4ca »). RÉFUTÉ :
   `0xa4c6=RET`, `0xa4c7` référencé QUE par la table d'events → c'est un handler d'event,
   pas un fall-through légitime.
4. **« past go-live / FB0_SEARCH = progrès »**. FAUX : `IMR=0x0000` toujours ; `[fbsb]
   FB0_SEARCH (real DSP path) fb0_att->217 fb0_ret=0` = le HOST L1 qui réessaie, pas le DSP.

## 🧱 LE MUR (confirmé) — cercle d'amorce [[calypso-dsp-scheduler-vec28]]
- Porte IDLE go-live = **`d[3fde]`**. Branche : `0xa6ae: BC 0xa6b8 (IDLE-2) si d[3fde]≠0`.
- `d[3fde]=0x0001` posé par l'init DSP lui-même `@0xa4c0` (+0xa563). **JAMAIS 0** (200 samples).
- `d[3fde]<-0` SEULEMENT à `0xaa71/0xaa98/0xcb78/0xdbc7` — TOUS dans du code
  event-handler/post-dispatch. `0xaa6c` appelé par **25+ sites `0xab3b-0xac00`** (dispatch
  events). **Aucun cold-reachable.**
- → `d[3fde]` cleared seulement après traitement d'un event ring ⟸ ring nourri ⟸ ISR
  vectorise ⟸ **IMR armé** ⟸ ... ⟸ event ring. **CIRCULAIRE.**
- **IMR jamais armé** : seul write = clear `0x3000->0 @0xb37e insn 1047`. L'arm `0xa4c7`
  (`ORM #0x3000,IMR`, bit12=BRINT0) est lui-même gaté event-12/13.
- **Vecteurs vec19(frame)/vec21(BRINT0) = stubs RETE @0xb4d6 à froid** → même s'ils
  vectorisaient, ils ne font RIEN. Les vrais handlers (qui nourrissent le ring) ne sont
  installés qu'après go-live.
- DSP gelé ~167k insn en 121s (quasi-IDLE ; réveillé par INT3 masqué chaque trame, RSBX
  clear INTM, reboucle).

## 🎯 PROCHAIN COUP (forcé par la donnée, FIDÈLE, borné) — décoder le bootloader 0x7000
**Smell** : un firmware correct ne se sabote pas à l'init. Le nôtre, dès le cold-init :
met `IMR=0` (0xb37e) ET pose lui-même le flag qui le fait idler (`d[3fde]=1` @0xa4c0).
Donc soit ce n'est pas l'init cold qui tourne, soit **il manque une étape entre le re-init
et la boucle go-live** : celle qui arme l'interrupt bootstrap + installe les VRAIS vecteurs.
C'est le job du **bootloader 0x7000** (celui que l'ARM lance via BACC, vu marcher plus tôt).

**Question précise à trancher** : le bootloader 0x7000 installe-t-il les VRAIS vecteurs ISR
(ring-feeders, pas les stubs RETE) ET arme-t-il `IMR.bit12` (vec28/BRINT0) AVANT de passer
la main à L1 ? **Hypothèse** : le replay-from-warm-dump traverse le bootloader en mode
dégradé (hérite d'un état warm — le dump portait IMR=0x3000 — qui lui fait sauter
l'install/arm), puis L1 clear IMR et plonge dans la boucle go-live sans bootstrap armé.

**Comment** :
- Désassembler la région 0x7000 (bootloader). Chercher les writes IMR (STM/ORM vers MMR00)
  et les installs de vecteurs (writes vers 0x0080-0x00ff) DANS le chemin bootloader.
- Comparer ce que le bootloader DOIT faire vs ce que notre replay exécute (le départ
  warm-dump saute peut-être l'étape).

**= candidat #1 = la conclusion que la donnée impose, fidèle.**
**REJETÉ : #2 « forcer la 1ère vectorisation » = un poke déguisé. NE PAS le faire.**

## Sondes en place
- **GOLIVE-MIRROR** (c54x data_read ~L1900, cap 80) : logge `data[]` vs `api_ram[]` pour
  0x0c36/0x0c37/0x08de.
- **A6GATE** (`CALYPSO_DEBUG=A6GATE`) : op+ST1+d[3fde/3fdb/3fdd]+T+A+INTM+IMR aux PC go-live.
- RSBX-FIRE, INTM-CLEAR (ungated), IMR-ARM, AAD5 (dequeue), INT3-RATE, GOLIVE-WATCH.

## Adresses clés
- `0xa4ca` boucle go-live (CMPM d[0x3f70]==2 ; RC TC). `0xa4c7` ORM #0x3000,IMR (handler
  event-12/13, arme BRINT0). `0xa4c0` ST #1,*(0x3fde).
- `0xa671` service routine (PORTR PA=0x0003) ; `0xa6ae` BC IDLE si d[3fde]≠0 ; `0xa6b8`
  IDLE-2 ; `0xa6cf` RSBX INTM (fixé) ; `0xa6d0` RET.
- `0xaac3` enqueue ring (0x434e/0x434f) ; `0xaad5` dequeue ; `0xaa6c` clear d[3fde]/enqueue
  ring 0x433e/0x433f.
- `0xb37e` STM #0,IMR (l'unique write IMR) ; `0xb4d6` install vecteurs (stubs RETE) ;
  **`0x7000` bootloader (PROCHAIN).**
- Dump `calypso_dsp.Registers.bin` : IMR=0x3000, IFR=0x0008, ST1=0x2900 (snapshot warm
  mid-op = le mauvais oracle pour l'amorce cold).

## Méthode qui a marché (à garder)
- **Confirm-before-fix** : la sonde GOLIVE-MIRROR a tué le fix mailbox AVANT application.
- Laisser le grep trancher ; ne pas conclure sur le 1er signal ; distinguer propriété
  statique ROM vs fait dynamique du run.
- Le mur a reculé toute la session car chaque recul était un artefact de NOTRE couche —
  SAUF le dernier, qui est architectural (le cercle), et pointe vers une étape boot sautée.
- ⚠️ Note d'état : 3 sur-lectures en un message = signe de fatigue, pas de rigueur.
  Reprendre le 0x7000 à tête reposée, une chose à la fois.

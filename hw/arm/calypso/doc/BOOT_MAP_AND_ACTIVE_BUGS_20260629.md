# Carte du boot DSP Calypso (EXE) + bugs actifs de la couche émulateur

**Date** : 2026-06-29. **Cible** : chemin EXE (CALYPSO_ORCH unset = vrai DSP TMS320C54x).

## ⚠️ RECADRAGE (2026-06-29, post grep-sur-run) — ce n'est PAS une impossibilité
La version initiale concluait à une « impossibilité méthodologique du replay-from-mid-dump ». **Le run
l'a réfutée.** Le firmware fait ce qu'il faut (il exécute son RSBX INTM, il cherche son FB). Les murs sont
dans **la couche host/émulateur qu'on a écrite**, pas dans le silicium ni le firmware. Donc : réparables.

### BUGS ACTIFS (le 1er a une adresse)
1. **RSBX INTM droppé à `0xa6cf`** *(bug d'exécution, fix probable = 1 ligne)*. `op=0xf6bb` (RSBX INTM)
   exécuté à 0xa6cf, mais `ST1=0x2900` AVANT **et** APRÈS (A6GATE PC=0xa6cf/0xa6d0) → `st1 &= ~(1<<11)`
   sans effet sur ce chemin. INTM ne clear jamais → aucune IT ne vectorise → tout le cold-boot stalle.
   3 suspects : (a) court-circuit `pc==0xa6cf` plus tôt ; (b) longueur fausse de l'instr précédente
   (PORTW 0xa6c3) décalant le fetch ; (c) ST1 réécrit après le clear. Sonde `RSBX-FIRE @0xa6cf` (ajoutée,
   CALYPSO_DEBUG=A6GATE) + watch ST1 0xa6cf→0xa6d0 tranchent. **PRIORITÉ ABSOLUE — c'est le fix actif.**
2. **Poke a_sync host-side** (`calypso_bsp.c:1227`, d_fb_det/TOA=23/PM=SNR=0x7000) — INJECTION non-silicium
   qui ment « FB trouvé » AVANT tout search → polluait toute mesure FB en EXE (d'où « FB RES avant FB
   SEARCH »). **ORCH-GATÉ le 2026-06-29** (réservé à l'ORCH). À refaire : toute mesure FB *après* ce gate.

La « chaîne en 7 fronts » ci-dessous reste la **carte fidèle du boot** (les états/cellules/handshakes
traversés), mais sa lecture change : les fronts 4-7 (IMR=0, INTM jamais clear, IDLE 0xa6b8) sont des
**conséquences du bug #1** (RSBX droppé → INTM gelé → IMR jamais armé par le firmware), pas une
impossibilité. Reste à confirmer si #1 seul débloque la cascade.

## Carte : la trajectoire de boot (front par front)
Un bootstrap est une **trajectoire**, pas un état. Le dump 3606 capture la **destination** (registres
d'un DSP déjà vivant) mais : (1) **ne contient pas le PC** → le replay repart toujours du reset vector
`0xff80→FB 0xb410` ; (2) **INTM=1** au capture (ST1=0x2900) ; (3) le re-init firmware efface l'état de
contrôle volatil. Le firmware re-dérive donc le *départ* depuis son *point d'arrivée* — impossible par
construction. Chaque « mur » ci-dessous est un **front / handshake / périphérique** que le snapshot
escamote parce qu'il a l'état post-transition, jamais la transition.

## La chaîne causale (7 fronts, instrumentés)
1. **Go-live état==2 jamais atteint.** Wait-loop `0xa4d4 CMPM data[0x3f70],#2 ; RC TC`. État oscille
   0↔1 (writer `0xdddb ST#1`, reset `0xde8b ST#0`), jamais 2. Writers état→2 (`0x710c`, `0xde9c`,
   `0xa5be`) jamais atteints.
2. **Le gate 1→2 = `data[0x098a]`** (`0xddeb LD ; 0xdded BC 0xde8a si ==0`). 0x098a n'est JAMAIS posé
   non-nul dans la ROM (seulement zéroïsé/copié) — il serait posé par l'ISR vec28.
3. **Ring « C » @0x4340 vide.** `0xaad5` = dequeue (MVDM AR1←tail, AR0←head, tous deux 0x4340 par
   l'init `0xa4ba/0xa4bd`) → `e800 LD #0,A` → A=0 = sentinelle vide. Décode FIDÈLE (CALYPSO_FIX_MVDM=1
   vérifié, +2 PC). Le producteur (enqueue) n'est atteint qu'après go-live.
4. **IMR bit12 (=vec28=BRINT0=BSP0, offset 0x70) jamais armé.** Sonde IMR-ARM (ungated, tout opcode) :
   **une seule** écriture du run = `0x3000→0x0000 @0xb37e` (clear d'init canonique). Le dump portait
   bit12 (0x3000) ; le re-init l'efface ; rien ne le ré-arme. Le `0x52fd` go-live n'est jamais atteint.
5. **INTM ne passe JAMAIS à 0.** Sonde INTM-CLEAR (ungated, par-instruction) : 0 transition 1→0 sur tout
   le run. Décode résolu : `f6bb=RSBX INTM`, `f7bb=SSBX INTM` (canonique, handler émulateur correct
   `st1 &= ~(1<<bit)`) ; `f4e2=BACC A` (pas RSBX). Les sites RSBX/RETED vivent tous dans l'ISR vec28 /
   la machine go-live gatée / les épilogues d'IT — jamais atteints à froid.
6. **L'IDLE cold-start réel = `0xa6b8` (f5e1, IDLE 2), atteint avec IMR=0x0000.** Le BRANCH-TRACE montre
   le boot complet : `0xff80→0xb410→poll bootloader→BACC 0x7000(=FB 0xb360)→re-init→L1 init` (0xbb00,
   tables 0x86xx/0x88xx/0x90xx) `→0xa4df→0xa671→0xa6b8 IDLE`. L'IDLE est réveillé (frame INT3, déjà levé
   par l'émulateur `calypso_bsp.c:1002`) mais **IMR=0 → jamais SERVI** → retombe dans l'oscillation
   `0xa4ca`. (Le bootloader, lui, MARCHE — fix CMD-CORUN existant ; ce n'était pas le mur.)
7. **Handshake périphérique `PA=0x0003 / 0xf900` NON modélisé** gate l'enable. Segment `0xa671→0xa6cf` :
   `PORTR PA=0x0003→T` ; `ANDM/ORM` ; `PORTW PA=0x0003/0xf900` ; `BC` sur `data[0x3fde/3fdb/3fdd]` →
   décide entre les 3 IDLE morts (0xa6a0/0xa6a7/0xa6b8) et la sortie productive `0xa6cf RSBX INTM`
   (l'enable). PA=0x0003/0xf900 (TPU/TSP) renvoient garbage → branche vers l'IDLE → IMR reste 0.
   [Maillon clos par la sonde A6GATE : warm-residue OU peripheral-garbage — les deux confirment
   « le snapshot ne porte pas le contexte cold ».]

## Ce qui EST injecté (et pourquoi ce n'est pas le bug)
Le DSP n'est pas vierge au reset : `dsp-drom → data[0x9000..0xDFFF]` (LUT scheduler 0x9187, coeffs
corrélateur) et `dsp-pdrom → data[0xE000..0xFFFF]` (table de vecteurs 0xFF80+, code ISR) sont chargés dès
l'instant 0. C'est **le vrai data-ROM du DSP — injection FIDÈLE** (présent sur silicium). Les cellules de
contrôle du cold-boot (data[0x3fde], 0x098a, 0x3f70, 0x4340) sont toutes **< 0x9000 = DARAM RAM**, donc
PAS injectées par la DROM (runtime : 0x3f70 oscille 0/1, 0x098a=0 → RAM firmware-écrites). L'injection
DROM/PDROM est donc *nécessaire* à un vrai cold-boot (A2) et ne doit PAS être zéroïsée. Elle ne masque ni
ne cause les 7 fronts — ceux-ci vivent dans la RAM de contrôle + les registres du dump (PC-absent, INTM=1,
IMR effacé par re-init). [Sonde A6GATE = data[0x3fde] au gate 0xa671 : RAM cold-zéro vs PORTR-garbage.]

## Correction par grep-sur-run (sonde A6GATE, 2026-06-29) — honnêteté
La trace runtime du segment `0xa671→0xa6cf` corrige deux formulations ci-dessus :
- **Le segment est ATTEINT et BOUCLÉ** (insn 4435/4565/4695…), PAS un dead-end. La formulation
  « RSBX jamais atteint » (front 5) était imprécise : c'est le *site* qui est bouclé.
- **Fait dur vérifié** : `INTM-CLEAR` (ungated) ne fire **jamais**, `INTM=0` **0 occurrence**, `IMR=0x0000`
  370× — donc **INTM ne clear jamais ET IMR=0 reste gelé** *malgré* la boucle. L'« enable » n'a jamais
  lieu : soit l'opcode exécuté à 0xa6cf n'est pas RSBX INTM (banking — décode dump = autre page), soit le
  chemin le saute. [Run suivant : A6GATE logge op+ST1 réels à 0xa6cf pour trancher.]
- **Valeurs runtime du gate** : `data[0x3fde]=0x0001` (RAM, ni cold-zéro ni DROM-warm 0xc074),
  `PORTR PA=0x0003 → T=data[0x000e]=0x0000` (port non modélisé renvoie 0), `d[3fdb]=d[3fdd]=0`. Donc le
  handshake PA=0x0003 renvoie 0 et le gate sur data[0x3fde]=1 route vers l'IDLE/boucle, INTM/IMR figés.
- **Net** : les fronts 5-7 se résument au fait vérifié **INTM=1 ∧ IMR=0 gelés pendant que le firmware
  boucle le segment handshake** — l'enable est gaté par le périph PA=0x0003 non modélisé (+ banking à
  confirmer). Le cœur de la preuve (replay cold incapable, front 1-4 + registres-dump) tient inchangé.

## La signature
À chaque tour le mur recule d'un cran et la cause est toujours **un front absent / un périphérique non
backé**, jamais une cellule isolée. Ce n'est pas une coïncidence : c'est la **signature structurelle du
replay-from-mid-dump**. Le snapshot a capturé l'état d'arrivée (IT en route, file semée, IMR armé,
handshakes complétés) ; le replay arrive froid (PC reset, INTM=1, IMR effacé, file vidée par l'init) et
ré-exécute des chemins qui *supposent* ce contexte chaud déjà établi.

## Cahier des charges pour un vrai cold-boot (le travail suivant, A)
Deux voies, toutes deux maintenant **bornées** par cette chaîne :
- **(A1) Cold-capture** : refaire la capture silicium **au reset**, PC + ST1(INTM) + état de contrôle
  inclus, OU un warm-resume fidèle (reprendre du PC capturé, sans re-init).
- **(A2) Modèle de séquencement périphérique depuis reset** : modéliser les périphs que la chaîne a
  nommés — **BSP0/BRINT0 (vec28/bit12)**, **INT3/frame (vec19/bit3, déjà levé)**, **PA=0x0003/0xf900
  (TPU/TSP)** — pour que le 1er interrupt soit *servi* et que le firmware arme IMR + RSBX INTM lui-même.

Les sept tours ne sont pas perdus : **ils SONT le cahier des charges.**

# Rapport — cause racine de `d_fb_det = 0` (mode natif)

> **Provenance.** Enquête multi-agents du 2026-07-27 : 5 hypothèses instruites en
> parallèle sur le code live et le log du run `19:28:40 → 19:57:26`, chacune soumise à
> un agent **sceptique** chargé de la réfuter, puis synthèse. 11 agents, 368 appels
> d'outils. **Les 5 hypothèses initiales ont été réfutées** — le rapport ci-dessous est
> ce qui a survécu au tri, plus deux corrections qui invalident une partie du dossier
> antérieur (dont une mesure que j'avais moi-même présentée comme un fait établi).
>
> Diagnostic **en lecture seule** : aucun correctif appliqué. Le test décisif du §4 est
> à lancer avant toute modification.
>
> Voir aussi `run_results.md` (mesures chiffrées, règles de décision, reproduction).

## RAPPORT DE SYNTHÈSE — cause racine de `d_fb_det` (data[0x08f8]) = 0

---

### 0. DEUX CORRECTIONS PRÉALABLES QUI CHANGENT LA LECTURE DE TOUT LE DOSSIER

**(0.a) Le binaire vivant N'EST PAS celui du disque — mais les sondes critiques SONT dedans.**
Deux agents ont invoqué « citations périmées » pour invalider en bloc les preuves. La moitié est vraie, la conclusion est fausse. Mesure directe :

```
/proc/3827436/exe -> /opt/GSM/qemu-src/build/qemu-system-arm (deleted)
running-inode=43363984   ondisk-inode=39105158 (mtime 19:55:28, rebuild en cours)
process start = Mon Jul 27 19:28:39 2026
```

Le processus tourne bien sur un inode supprimé (rebuild écrasé pendant le run). MAIS `grep` sur l'image vivante `/proc/3827436/exe` :

| chaîne | image VIVANTE | disque |
|---|---|---|
| `B4-DFBDET-WR` | **présent** | présent |
| `FBDET-WR` | **présent** | présent |
| `ANGLE-WR` | **présent** | présent |
| `DETECTOR-RUN`, `B4B-FLOW`, `SCAN-08F8`, `FLOWTRACE` | **présents** | présents |
| `depots_depuis` / `BUFFER FIGE` | **absents** | présents |

→ Seuls le compteur `depots_depuis`/`BUFFER FIGE` et le format d'en-tête du `.cfile` sont post-run. **Tous les « 0 hit » de B4/FBDET-WR/ANGLE-WR sont donc des mesures valides**, pas des artefacts de compilation. Les réfutations « vue-memoire », « reroutes » et « qui-ecrit-2a00 » ont sur-généralisé sur ce point.

**(0.b) Le « fait établi #3 » du brief est un artefact de fenêtre — le piège explicitement listé.**
Le dump DARAM couvre `DETECTOR-RUN #0..#199`, soit `+5.609s` → `+6.073s` (`/root/qemu.log:26417` et `:28222`). Or :

```
:28240  +6.083s DETECTOR-RUN #200 ... d_fb_mode[08f9]=0x0000
:28846  +6.541s DETECTOR-RUN #400 ... d_fb_mode[08f9]=0x0001   <-- premier passage en mode FB
```

**Les 200 records ont TOUS été pris pendant que `d_fb_mode = 0`**, c'est-à-dire quand le DSP n'était pas en recherche FCCH. « 0 FCCH sur 200 records » ne prouve donc rien sur le chemin FB. La sonde n'a jamais été rouverte (fermeture définitive à `+6.073s`, `_ddn >= _ddmax`, `calypso_c54x.c:14708-14711`) alors que le run dure `+1620s`. **Toute l'enquête « qui écrit 0x2a00 / timing / vue mémoire » a été menée sur une fenêtre hors sujet.**

---

### 1. CAUSE RACINE LA PLUS PROBABLE

> **Le DSP n'exécute jamais sa routine de résultat FB/SB en banque commune `0x7700–0x79F0`, seule productrice de `d_fb_det` ET de `a_sync_demod[]`. Le mode `NATIVE_HELPED` pousse le flux dans un noyau d'énergie bancarisé (`0x9500→0xa033→0xa076`) qui ne contient structurellement aucun écrivain de `0x08f8`.**

**Chaîne causale, maillon par maillon :**

**(1) Aucune instruction DSP n'écrit jamais `0x08f8`, sur 1,08 milliard d'instructions / 1620 s.**
La sonde `FBDET-WR` est **inconditionnelle et sans cap** :
```c
calypso_c54x.c:2880   if (addr == 0x08F8) {
calypso_c54x.c:2881       fprintf(stderr, "[c54x] FBDET-WR data[0x08F8] 0x%04x -> 0x%04x PC=0x%04x insn=%u\n",
```
Elle est dans `data_write_locked` (déf. `:2719`), dont la queue est `s->data[addr] = val;` (`calypso_c54x.c:4186`) — **tous** les stores d'instruction DSP y transitent. Résultat : `grep -c FBDET-WR` = **0**. Idem `B4-DFBDET-WR` = 0 (gate `CALYPSO_B4=1`, manifeste `qemu.log:33`).

**(2) Le voisinage `a_sync_demod` n'est jamais écrit non plus.** Sonde `ANGLE-WR`, elle aussi **inconditionnelle** (cap 40, jamais atteint) :
```c
calypso_c54x.c:2677   if (addr >= 0x08fa && addr <= 0x08fd) {
```
`grep -c ANGLE-WR` = **0**. C'est le maillon qui verrouille le diagnostic : la routine contient **quatre stores distincts** vers ces cellules, dont un non conditionnel — aucun ne s'exécute.

**(3) Ces stores localisent la routine, et elle est unique.** Désassemblage PROM0 (base `0x7000`, mémoire confirmée) :
```
0x798a: 82f8 08fc          ST  A, *(0x08fc)   ; ANGLE
0x798c: 76f8 08fd 4000     ST  #0x4000, *(0x08fd) ; SNR  <-- inconditionnel
0x79d4: 82f8 08fd          ST  A, *(0x08fd)
0x79de: 81f8 08fb          STH A, *(0x08fb)   ; PM
0x79e3: fc44               XC/cond
0x79e4: 69f8 08f8 0001     ORM #0x0001, *(0x08f8)  <-- LE PUBLISHER de d_fb_det
0x778a: 68f8 08f8 fffe     ANDM #0xfffe, *(0x08f8) <-- le clear apparié
```
Un scan mot-à-mot de PROM0 (28672 mots) donne 26 occurrences du mot `0x08f8` ; en écartant celles où `0x08f8` est un **opcode** `ADD *(lk),A` (vérifié en contexte : `0x772b` `10f8 3fb4 | 08f8 3fb3` ; `0xa335` `f820 a33b | 08f8 0c6d` ; `0xa3cb`, `0xa3fb`, `0xa406`, `0xa423`, `0x91e3`, `0x9204` — tous du même moule), il ne reste comme **adresses** que `0x79e5` (ORM, set), `0x778b` (ANDM, clear) et `0xb2cd` (`76f8 08f8 0000`, ST #0 = reset NDB). **Le seul chemin qui pose le bit est `0x79e4`, et il est dans `0x7700–0x79F0`.** (Ceci corrige le brief : le « RMW @0xe5af » en PDROM est `ADD *(0x09f1),A`, et le « cluster 0xa335/0xa33b/0xa3cb » est de l'opcode — ce ne sont pas des writers.)

**(4) Cette routine vit en banque COMMUNE, le noyau où le flux est envoyé vit en banque OVERLAY.**
`c54x_prog_xlate` (`calypso_c54x.c` ≈4200) : `if (addr16 >= 0x8000 && addr16 < 0xE000) → XPC bank`. Donc `0x7700` est XPC-indépendant (toujours atteignable), tandis que `0x9500 / 0xa076 / 0x8d00` sont bancarisés. Le graphe de transferts de contrôle PROM0 confirme la séparation : la routine `0x77xx` n'a **aucun appelant hors d'elle-même**, sauf l'entrée `@0x76fb f272 7700` (`BD 0x7700`), et ses seules cibles internes sont `0x770d, 0x7794, 0x795f, 0x7773, 0x78e6, 0x79e8, 0x79d6…`. Symétriquement, tout le chemin rerouté reste bancarisé : `@0x9517/0x959a/0x95f8/0x9618 f274 → 0xa033`, `@0xa054 f273 → 0xa076`. **Les deux mondes ne se rejoignent jamais.**

**(5) Et c'est précisément ce monde-là que `NATIVE_HELPED` alimente.**
```c
calypso_c54x.c:5736   if (is_call && src_pc == 0xb01e) {
calypso_c54x.c:5743       if (_fbe && s->data[0x058a] == 5) {   /* d_task_md == 5 (commande FB) */
calypso_c54x.c:5750           tgt = _fbentry;                   /* = CALYPSO_FB_CORR_ENTRY = 0x9500 */
```
Manifeste : `CALYPSO_FB_ENERGY=1` (`qemu.log:52`), `CALYPSO_FB_CORR_ENTRY=0x9500` (`qemu.log:4`). Au log :
```
:26109 +5.601s FB-ENERGY-REROUTE CALA@0xb01e tgt 0xab38 -> 0x9500
:28363 +6.170s FB-ENERGY-REROUTE CALA@0xb01e tgt 0x8d00 -> 0x9500
```
Après boot, la CALA native résout vers **`0x8d00`** — et on la détourne quand même. Le détecteur `0x9ac0` tourne alors **97 400+ fois** (`:1712392 +1596.891s DETECTOR-RUN #97400`), `d_fb_mode` alterne 0↔1 (195 vs 328 échantillons loggés) — **l'ARM commande bien le FB, le DSP fait bien tourner un corrélateur, et ce corrélateur n'a pas de sortie**.

**Résumé causal :** ARM commande FB (`d_task_md=5`, `d_fb_mode=1`) → CALA `0xb01e` → **reroute forcé vers `0x9500`** (noyau énergie bancarisé, sans publisher) → boucle `0x9ac0/0xa0xx` → jamais de retour vers la tâche banque-0 `0x7700` → `ORM #1,*(0x08f8)` @`0x79e4` jamais exécuté → `d_fb_det = 0` → l'ARM ne voit jamais de FCCH.

---

### 2. CAUSES SECONDAIRES PLAUSIBLES (classées)

**S1 — La tâche `0x7700` n'est pas dispatchée du tout, indépendamment du reroute.** *(probabilité haute, non départageable du #1 sans le test §4)*
Le seul point d'entrée est `@0x76fb BD 0x7700`, atteint depuis l'ordonnanceur de tâches. Si l'ordonnanceur ne sélectionne jamais ce slot, désactiver le reroute ne suffira pas. Corrobore : les slots de dispatch bougent (`DISPATCH-CELL-RESEED` = 30 hits sur `0x43d8/0x3fd4/0x4368`) mais rien ne prouve qu'ils pointent vers `0x76fb`.

**S2 — Garde d'état `dma(0x7e)` non satisfaite à l'entrée.** *(probabilité moyenne)*
```
0x7720: 107e            LD  dma(0x7e), A
0x7721: f010 0004       SUB #4, A
0x7723: f844 7729       BC  0x7729 if A != 0     <-- saute l'appel
0x7725: f074 795f       CALL 0x795f              <-- corrélation + publish
```
et à l'intérieur, `0x795d: 767e 0004` (ST #4, dma(0x7e)), `0x795f-0x7962` re-teste ==4, `0x79e0-0x79e4` re-teste avant l'ORM. **Trois gardes sur la même cellule d'état.** ⚠️ `dma(0x7e)` dépend de DP au runtime : si `DP=0x11`, c'est `data[0x08fe]` (NDB, voisin de `d_fb_det`) ; **DP n'est pas mesuré**, c'est une hypothèse non vérifiée. Si S2 est la vraie garde, la correction est une écriture ARM→NDB, pas un changement de dispatch.

**S3 — L'entrée `0x9500` imposée par l'env n'est pas l'entrée codée en ROM (`0x94f5`).** *(faible, mais réel)*
Le défaut source est `_fbentry = 0x94f5` (`calypso_c54x.c:5737`), et `0x94f5` est bien référencé en ROM (`@0x87e7 f930 94f5`) ; `0x9500` ne l'est **nulle part** (0 hit sur 28672 mots). On saute donc 11 mots de mise en place (dont potentiellement ST1/DP/ARP). Effet secondaire possible sur la garde S2.

**S4 — `CALYPSO_DEMOD_NOCLOBBER=1` gèle 8 mots de `0x2a00` sur un prétexte faux.** *(faible impact ici, mais dette réelle)*
Le message dit « feed_iq autoritaire » alors que `CALYPSO_FB_IQ_DARAM=0` et `CALYPSO_FB_IQ_OWNS=0` (manifeste `:28`, `:15`). En pratique c'est `rx_burst` qui alimente (`calypso_bsp.c:1225`), donc la config n'est pas incohérente — mais le commentaire est trompeur et n'a que 8 hits cappés (`grep -c DEMOD-NOCLOBBER` = 8), donc **il n'apporte aucune information** sur l'état du buffer.

**S5 — Asymétrie de verrou producteur/sonde.** *(non manifesté dans ce run)*
Producteur sous `qemu_mutex_lock` (`calypso_bsp.c:1216/1295`), sonde DARAM-DUMP lit `s->data[]` sans verrou (`calypso_c54x.c:14666`). Lecture déchirée possible en principe ; réfutée en pratique ici (64 records byte-identiques ⇒ pas de course).

---

### 3. DÉFINITIVEMENT ÉCARTÉ

| Hypothèse | Preuve d'écartement |
|---|---|
| **Deux vues mémoire / shadow DARAM** | `calypso_c54x.h:198 uint16_t data[C54X_DATA_SIZE];` = tableau plat ; un seul `c54x_init` vivant (`calypso_trx.c:1921`) ; `calypso_bsp.c:809 bsp.dsp = dsp;` ; même pointeur `0x7b5cd4b5e010` côté WATCH-2A00 et côté feed-daram. |
| **« Le burst suivant écrase la FCCH avant lecture »** | Consommateur (`0x9ac0`, 97 400 passages) largement plus fréquent que le producteur (216,7 dépôts/s, `DRAIN-CB delivered`). Non pertinent de toute façon : cf. ligne suivante. |
| **« Le buffer 0x2a00 ne contient jamais de FCCH » (fait #3 du brief)** | **Artefact de fenêtre** : les 200 records couvrent `+5.609s→+6.073s`, période où `d_fb_mode=0x0000` (`:28240` à `+6.083s`). Mesure hors sujet. |
| **« Les 200 records sont identiques »** | 5 corps distincts (md5 : blocs 16/64/16/16/32/16/16/16/8) ; 288 des 296 mots varient. |
| **Seuil d'énergie / SNR trop haut dans le corrélateur** | Impossible : `ANGLE-WR` = 0 ⇒ même le `ST #0x4000, *(0x08fd)` **inconditionnel** de `0x798c` n'a jamais tourné. Le code de décision n'est pas atteint, il n'est pas « en échec ». |
| **`0x08f8` non écrit parce que la sonde est aveugle** | `FBDET-WR` inconditionnel dans `data_write_locked`, dont la queue `s->data[addr]=val` (`:4186`) capte tous les stores DSP ; chaîne présente dans l'image vivante (§0.a). |
| **« XPC ne passe jamais à 0xec07 »** | Confusion PC/XPC. `0xec07` est un **PC** cible (`@0x8e8a`, `@0x9ff5`), pas une valeur de XPC. |
| **Toutes les citations `chemin:ligne` sont périmées** | Faux (§0.a) : seuls `depots_depuis`/`BUFFER FIGE` et l'en-tête `.cfile` le sont. |

---

### 4. LE TEST DÉCISIF

Il départage **#1 (reroute coupable)** de **S1/S2 (dispatch/garde d'état coupables)**. Il ne demande **aucun rebuild** : la sonde nécessaire (`FBDET-WR`, inconditionnelle) est déjà dans l'image.

**Relancer la pile avec une seule variable modifiée : `CALYPSO_FB_ENERGY=0`** (tout le reste identique — `NATIVE_HELPED=1`, `DSP_RUN_C54X=1`, `ARM2DSP_BGEN=1`, `FRAME_IT_NATIVE=1`). Laisser tourner ≥ 120 s, puis **une seule commande de verdict** :

```bash
docker exec osmo-operator-1 bash -lc 'echo "FBDET-WR=$(grep -c "FBDET-WR data\[0x08F8\]" /root/qemu.log)  ANGLE-WR=$(grep -c ANGLE-WR /root/qemu.log)  REROUTE=$(grep -c FB-ENERGY-REROUTE /root/qemu.log)"; grep -m5 "FBDET-WR data\[0x08F8\]\|ANGLE-WR" /root/qemu.log; grep "DETECTOR-RUN" /root/qemu.log | tail -1'
```

**Règle de décision, posée d'avance :**

- **`FBDET-WR > 0`** (peu importe la valeur écrite, même `0x0000`) → **cause #1 CONFIRMÉE** : le reroute `FB_ENERGY` détournait le flux hors de l'unique publisher. Le correctif est §5-A.
- **`FBDET-WR = 0` mais `ANGLE-WR > 0`** → la tâche `0x7700` tourne mais s'arrête avant `0x79e4` → **c'est S2** (garde `dma(0x7e)`/`XC` à `0x79e3`). Le `PC=` de la première ligne `ANGLE-WR` dit exactement où on s'arrête.
- **`FBDET-WR = 0` ET `ANGLE-WR = 0`** → la tâche `0x7700` n'est jamais entrée → **c'est S1** (dispatch). Le reroute est innocent ; l'enquête bascule sur qui atteint `@0x76fb`, et il faudra une sonde `exec_pc == 0x76fb` (à ajouter, ~3 lignes).
- Cas dégénéré à surveiller : si `DETECTOR-RUN` **cesse** d'apparaître avec `FB_ENERGY=0`, c'est que la CALA retombe sur le stub `0xab38` — alors le test est non concluant et il faut faire le B/ suivant : `CALYPSO_FB_ENERGY=1 CALYPSO_FB_CORR_ENTRY=0x94f5` (entrée ROM légitime) avant de conclure.

---

### 5. CORRECTIF PROPOSÉ

**A — si le test confirme #1 (le plus probable) : neutraliser le reroute, chirurgicalement.**
- Fichier : `/opt/GSM/qemu-src/hw/arm/calypso/calypso_c54x.c`
- Bloc : **`:5736`–`:5751`** (`if (is_call && src_pc == 0xb01e)`, gate `:5743 s->data[0x058a] == 5`, override `:5750 tgt = _fbentry;`)
- Action minimale : ne rien toucher au code — passer `CALYPSO_FB_ENERGY=0` dans `/opt/GSM/qemu-src/calypso_native_helped.env` (idiome `:=` pour rester surchargeable en CLI, cf. mémoire *env-propagation*).
- Action propre : conditionner le reroute au cas où la cible native est le **stub** seulement :
  `:5743` → `if (_fbe && s->data[0x058a] == 5 && tgt == 0xab38)`.
  Justification : au log, la cible native devient `0x8d00` dès `+6.170s` (`:28363`) ; le reroute n'avait de sens que contre `0xab38` (`:26109`).

**Ce qui peut mal tourner (A) :** `0x8d00` est le « corrélateur symbole », pas le corrélateur FB — le commentaire `:5730-5731` affirme qu'il « ne touche jamais le buffer IQ `0x2a00` ni le noyau `0xa076` ». On peut donc perdre `DETECTOR-RUN` sans rien gagner (cas dégénéré du §4). Risque de régression sur le camp SHUNT : nul ici (`CALYPSO_DSP_SHUNT=0`, `SHUNT_LEGIT=0`), mais ne pas propager ce défaut aux profils shunt.

**B — si le test pointe S2 : instrumenter la garde avant de corriger.**
Ajouter dans `calypso_c54x.c` (à côté de la sonde `0x9ac0`, ≈`:14717`) un bloc `exec_pc == 0x7720` loggant `DP`, `dma(0x7e)` résolu et `A`, et un bloc `exec_pc == 0x76fb`. **Ne pas forcer la cellule tant que son adresse effective n'est pas mesurée** : si `DP=0x11`, l'écrire aveuglément corrompt `data[0x08fe]` dans le bloc NDB, à un mot de `d_fb_det` — exactement le genre d'empilement de correctif sur symptôme listé dans les pièges.

**C — dette d'hygiène, à faire dans tous les cas (sinon la prochaine session repart sur du faux) :**
1. **Rebuild-pendant-run.** Le binaire vivant est un inode supprimé (`/proc/3827436/exe → (deleted)`), rebuildé deux fois pendant la mesure. Interdire le build tant qu'un `qemu-system-arm` tourne, ou logger `md5sum` du binaire en tête de `qemu.log` à côté du manifeste.
2. **Fenêtre de la sonde DARAM.** `calypso_c54x.c:14654` déclenche sur `exec_pc == _ddpc` sans condition d'état → le dump se remplit au boot et se ferme (`:14708`) avant que `d_fb_mode` passe à 1. Ajouter `&& s->data[0x08f9] != 0` : la sonde ne filmera plus que les passages en mode FB. **C'est ce défaut qui a produit le « fait établi #3 » et fait dérailler trois des cinq dimensions d'enquête.**
3. Corriger le message `DEMOD-NOCLOBBER` (« feed_iq autoritaire ») qui est faux avec `FB_IQ_OWNS=0` — c'est `rx_burst` (`calypso_bsp.c:1225`) l'autoritaire.

---

### 6. CE QUI RESTE INCERTAIN — à ne pas surinterpréter

- **Le lien de causalité entre le reroute et la non-exécution de `0x7700` n'est PAS démontré**, seulement rendu plausible par la séparation banque-commune/overlay. Le reroute ne modifie qu'**une** CALA (`0xb01e`) ; il est parfaitement possible que `0x7700` soit dispatché par un chemin totalement indépendant qui, lui, est cassé pour une autre raison (S1). **C'est exactement ce que le test §4 tranche — ne pas appliquer le correctif A avant.**
- **`dma(0x7e)` n'est pas résolu.** L'équivalence `dma(0x7e) ≡ data[0x08fe]` suppose `DP=0x11`, non mesuré. Hypothèse non vérifiée.
- **Le décodage de `0xfc44` @`0x79e3` comme `XC n, ANEQ`** est une inférence par analogie avec `f844`/`f842`/`f846` (même octet bas = même condition). Non confirmé dans la table d'opcodes de `calypso_c54x.c`.
- **Le contenu de `0x2a00` reste non caractérisé pendant les phases `d_fb_mode=1`** — soit 99,6 % du run. Tout ce qu'on sait de ce buffer vient d'une fenêtre de 0,46 s où le DSP ne cherchait pas de FCCH. Les analyses « signal écrêté / ton miroir / 35 valeurs complexes distinctes » décrivent un état hors-FB et **ne doivent pas être reportées** sur la phase FB.
- **`d_fb_mode` alterne 0↔1 jusqu'à la fin du run** (dernier échantillon `+1620.263s` à `0x0000`) : le firmware ARM boucle sur sa recherche, ce qui est cohérent avec l'absence de détection, mais ne dit rien de plus.
- Aucun des correctifs proposés n'a été appliqué ni compilé. Diagnostic en lecture seule, conformément à la préférence de restart de la pile.

---

## 7. RECOUPEMENT AVEC OSMOCOM-BB — comment le vrai firmware obtient `d_fb_det`

Source : `/opt/GSM/osmocom-bb/src/target/firmware`. Ce recoupement n'était pas dans le
périmètre du workflow ; il **corrobore** la §1 et **ouvre une piste** que le rapport n'a
pas explorée.

**Déclaration.** `include/calypso/dsp_api.h:202` :
```c
API d_fb_det;      // FB detection result. (1 for FOUND).
```
Cellule **NDB** (non double-bufferisée) : écrite par le DSP, lue par l'ARM sans page-flip.

**Commande** — `layer1/prim_fbsb.c:364-386`, `l1s_fbdet_cmd()` fait **trois** choses :
```c
rffe_compute_gain(rxlev2dbm(fbs.req.rxlev_exp), CAL_DSP_TGT_BB_LVL);  /* AGC */
dsp_api.db_w->d_task_md = FB_DSP_TASK;      /* = 5  (l1_environment.h:73) */
dsp_api.ndb->d_fb_mode  = fb_mode;
l1s_rx_win_ctrl(fbs.req.band_arfcn, L1_RXWIN_FB, 0);   /* programme le TPU */
```

**Lecture** — `prim_fbsb.c:404`, `l1s_fbdet_resp()` : `if (!dsp_api.ndb->d_fb_det)`,
jusqu'à 12 tentatives par set puis `FB0_RETRY_COUNT` re-planifications. Sur détection,
`read_fb_result()` (`:305`) lit `a_sync_demod[D_TOA/D_PM/D_ANGLE/D_SNR]` **puis remet
`d_fb_det = 0`**.

### Ce que ça apporte au diagnostic

| # | Constat osmocom | Conséquence |
|---|---|---|
| a | `FB_DSP_TASK = 5` (`l1_environment.h:73`) | confirme **indépendamment** le `d_task_md == 5` du reroute (§1.5) |
| b | `d_fb_det` **et** `a_sync_demod[]` sont lus dans la même routine, et produits par le même étage DSP | corrobore §1.2 : `FBDET-WR = 0` et `ANGLE-WR = 0` ne sont pas deux symptômes mais **un seul producteur absent** |
| c | **`l1s_rx_win_ctrl(..., L1_RXWIN_FB, ...)` programme le TPU en même temps que le DSP** | la détection FB = DSP **+ fenêtre TPU**. Piste non explorée par le workflow |
| d | l'AGC est réglée *avant*, en visant `CAL_DSP_TGT_BB_LVL` (**niveau bande de base cible**) | fonde la piste amplitude : notre chaîne livre 99,3 % de la pleine échelle **sans AGC** |

**Le point (c) est le plus important.** Il désigne un candidat sérieux pour **S1** (« la
tâche `0x7700` n'est jamais dispatchée ») : dans le vrai firmware, la tâche FB n'est pas
seulement commandée par `d_task_md`, elle est **cadencée par une fenêtre RX programmée au
TPU**. Or c'est précisément le câblage identifié comme manquant dans la dette du projet
(`l1s_rx_win_ctrl → tpu_enq_dsp_irq` = 0 hit ; tâche **RANK2 — Fenêtre RX BDLENA**, encore
ouverte). Si le DSP n'est jamais réveillé au bon instant par le TPU, désactiver le reroute
`FB_ENERGY` ne suffira pas — ce que le test du §4 tranchera par le cas « `FBDET-WR = 0` ET
`ANGLE-WR = 0` ».

**Le point (d)** donne un fondement au levier `CALYPSO_BSP_IQ_SHIFT=n` (ajouté le 27/07,
défaut 0) : osmocom présuppose un niveau bande de base **calibré par l'AGC**, pas un signal
à pleine échelle.

### Reproduire ce recoupement
```bash
docker exec osmo-operator-1 bash -lc 'cd /opt/GSM/osmocom-bb/src/target/firmware && \
  grep -n "d_fb_det" include/calypso/dsp_api.h layer1/prim_fbsb.c calypso/dsp.c && \
  sed -n 364,386p layer1/prim_fbsb.c && grep -n "FB_DSP_TASK" include/calypso/l1_environment.h'
```

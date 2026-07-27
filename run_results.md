# Run results — mesures chiffrées, règles de décision, reproduction

Résultats **mesurés** (pas d'affirmation sans chiffre), chacun confronté à une règle de
décision explicite. Le statut dépend du **mode** : chaque section nomme le sien via le
manifeste de run (`[calypso-manifest]` en tête de log = config `CALYPSO_*` **effective**
après le parseur value-list, donc reproductible).

> Ce fichier est régénéré à la main après chaque campagne de mesure. Les commandes
> d'extraction sont données pour que **n'importe qui rejoue les chiffres** sur ses logs.

---

## Run A — `SHUNT_LEGIT` (mode fiable, DSP off)

**Manifeste :** `CALYPSO_SHUNT_LEGIT=1  CALYPSO_SHUNT_NO_CANNED=1  CALYPSO_DSP_RUN_C54X=0`
· `CALYPSO_NATIVE=0` · `CALYPSO_FRAME_IT_NATIVE=1`. Run 123.7 s, une LU complète.

| # | Mesure | Valeur | Règle de décision | Verdict |
|---|---|---|---|---|
| A1 | éviction ring (3 politiques) | **overflow=0, ttl=0, reps=0** (`EVICT-STATS`) **en SHUNT_LEGIT** | ≥2 des 3 à zéro ⇒ candidat retrait — MAIS voir ⚠️ | ⚠️ **mode-dépendant** (voir ci-dessous) |
| A2 | profondeur ring | **max 1** (bucket 0-1 uniquement, 2 runs) | max ≤ 2-3 ⇒ buffer 1-slot déguisé | ✅ ring = 1-slot |
| A3 | `delta = fn_bloc − fn_L1` au DISPATCH | **−553 (±1), n=106, aucune dérive** | petit/stable ⇒ sélection FN inutile ; dérive ⇒ à faire | ✅ **stable, sélection FN inutile** |
| A4 | ENQUEUE vs DISPATCH | **11** vs **44** (0.09/s vs 0.36/s, ratio 4:1 = 4 bursts/bloc) | comptes proches ⇒ déséquilibre résorbé | ✅ pas de déséquilibre |
| A5 | RACH → LU ACCEPT | **2.70 s** (2 runs), **0 retry T3211** | un chiffre vaut mieux que « quasi systématique » | ✅ LU 2.7 s, 1er coup |

**Lecture (⚠️ rectifiée).** La saturation `depth=32` existait en **`DSP,NO_CANNED`** (jitter
c54x), **pas** en `SHUNT_LEGIT`. Donc les 3 politiques sont mortes **en `SHUNT_LEGIT`
uniquement**, pas mortes tout court. **NE RIEN SUPPRIMER** avant d'avoir mesuré A1 en
`DSP,NO_CANNED` : si les compteurs y montent, c'est de la **politique mode-dépendante** (à
documenter comme telle), pas du code mort — sinon on referait à l'envers l'erreur de mode
qu'on vient de corriger dans la doc. La profondeur max=1 (A2) montre par ailleurs que la
sélection par FN est sans objet **par construction** (un seul bloc en file, rien à
sélectionner) — argument plus fort que le delta.

**A3 en détail — la pépite.** `delta = −553 ± 1` sur 106 présentations, **sans dérive**. Cette
stabilité (±1 sur 106) ne dit pas « constante magique » : elle dit que les **deux horloges
sont verrouillées en fréquence** (L1 firmware ↔ gr-gsm/réseau) et ne diffèrent que par la
**phase** (−553 = décalage de phase constant). ⚠️ Ça **réfute** le « firmware 73 FN/s vs
gr-gsm 217 FN/s » qui traînait dans mes notes mentales (jamais dans les docs committés) : des
horloges à fréquences différentes **dériveraient** ; ±1 stable = **même fréquence**. A4 (ratio
4:1) le confirme. **→ 73/217 RETIRÉ.** Pour le papier : le −553 est **empirique** (mesuré, pas
dérivé) ; TODO = vérifier qu'il survit à un redémarrage et à un décalage de lancement BTS.

### Reproduire (Run A)

```bash
# A1 overflow (+ split via EVICT-STATS sur binaire instrumenté)
grep -c "RING OVERFLOW" /root/qemu.log
grep "EVICT-STATS" /root/qemu.log | tail -1
# A2 histogramme profondeur
grep -oE "depth=[0-9]+" /root/qemu.log | grep -oE "[0-9]+" | \
  awk '{if($1>m)m=$1;b[($1<=1)?"0-1":($1<=3)?"2-3":($1<=7)?"4-7":"8+"]++}END{print "max",m;for(k in b)print k,b[k]}'
# A3 delta (binaire instrumenté)
grep -oE "delta=-?[0-9]+" /root/qemu.log | grep -oE "\-?[0-9]+" | \
  sort -n | awk '{a[NR]=$1}END{print "min",a[1],"med",a[int(NR/2)],"max",a[NR]}'
# A4 débits
echo "ENQUEUE=$(grep -c 'feed_sdcch: ENQUEUE' /root/qemu.log) DISPATCH=$(grep -c 'DISPATCH SDCCH' /root/qemu.log)"
# A5 temps LU + retries
grep -E "CHANNEL REQUEST: 00|LOCATION UPDATING ACCEPT" /root/mobile.log | head
grep -c "T3211" /root/mobile.log
```

---

## Run B — `NATIVE_HELPED` : diagnostic `d_fb_det = 0` (CLOS, chiffré)

**Manifeste :** `CALYPSO_NATIVE_HELPED=1` (⇒ `CALYPSO_FB_IQ_DARAM=1 CALYPSO_FB_IQ_BASE=0x9210`
→ feed réel de l'entrée démod `0x9213`(I)/`0x9215`(Q)). rxlev réel **−47 dBm** (trf6151/DECAN).

**Adressage (⚠️ CORRIGÉ — ma note précédente était INVERSÉE)** : `0x2a00` **EST l'entrée réelle
du corrélateur** = là où le BSP dépose la sortie ADC du TWL (prouvé E2E : `calypso_bsp.c`
`daram_addr=0x2a00`, le DSP lit `0x2a00` depuis `PC=0x93a5` en AR3 post-inc, scan statique = 50
sites `STM #imm,ARx`). `0x9213/0x9215` ne sont **pas** une adresse matérielle : c'est un
**read-intercept** (`c54x.c:1646`, PC 0x9f00–0x9fb8) = mon **point d'injection** FB-STREAM (et
ce run-ci il n'a même pas firé → non consommé). **Donc B2 mesurait le BON tampon** ; le spike
DC est la **vraie entrée du corrélateur**. La chaîne : `TRF6151` (transpose RF→IQ) → `TWL3025`
(ADC) → BSP → `0x2a00` → corrélateur DSP → `d_fb_det`. Le DSP **décide**, mais la chaîne
**RF/ABB (AFC via DAC TWL→VCXO, gain TRF) décide de ce qu'il regarde**.

| # | Mesure | Résultat | Verdict |
|---|---|---|---|
| B1 | table réf `0x2c00` au kernel `0xa076` | **peuplée** (écrite par PC `0x9fd5`, démod), pas vide ; se stabilise à `001f…` (plat) | ✅ pas « corrèle contre du vide » |
| B2 | accu A/B + max fenêtre 296 sur `0x2a00` (**vraie entrée**) | `\|A\|=294908 \|B\|=36863` ; `max=21229@0` (spike **index 0 = DC**, pas de ton) | ✅ le MAC calcule ; **l'entrée réelle est DC/sans FCCH** → à confirmer par B2SEQ (pattern) |
| B4B | flux instruction par instruction après `0x9ac0` | `STL A` → boucle de **normalisation** (A≫1 jusqu'à 0) → `952c`→`9511`→`a033` (setup pointeurs) → re-boucle. **XPC reste 0, n'atteint JAMAIS `0xec07`** | ✅ mur de flux : boucle sans sortir vers la décision |
| B4 | watchpoint écritures `data[0x08f8]` | **`count = 0`** — jamais écrit | 🔑 **d_fb_det jamais écrit** = chemin pas atteint (≠ « écrit 0 ») |
| SCAN | refs `0x08f8` dans la PROM (bank 0) | **30+** occurrences, dont des **writers** (`STL A` @0xd2c0/0xd30e, cluster `0xa335/0xa33b/0xa3cb`, RMW `0xff20` @0xe5af) | ✅ les writers **existent**… |

### Conclusion (prouvée instruction par instruction)
Le firmware DSP **contient** le code qui écrit `d_fb_det` (SCAN : 30+ refs, plusieurs writers),
le corrélateur **calcule** bien (B2 : A/B non-nuls), **mais** son flux **boucle dans le bank 0**
(`0x8d00`→`0xa07x`) sans jamais atteindre l'étage publish/décision (B4B) → **aucun writer ne
s'exécute** (B4 : `data[0x08f8]` jamais écrit). C'est le **mur de contrôle de flux « RANK3 »,
désormais chiffré**, pas une conjecture.

### ATLAS NATIF — graphe de données reconstruit par TRACE (méthode, pas conjecture)

Après ~15 sondes ponctuelles qui **oscillaient** (0x2a00 entrée→sortie→entrée ; démod lisant
0x9213 puis 0x9260 ; g_fbs vide puis pollué), on a arrêté d échantillonner : **une seule trace
brute** de tous les accès R/W dans `data[0x2800..0x3000)` armée au kernel `0xa076`
(`CALYPSO_FLOWTRACE=N` → `/tmp/calypso_flow.txt`), puis reconstruction **hors ligne** du graphe.
200 000 accès analysés. C est ce qui a tranché — et corrigé trois de mes conclusions.

**Graphe mesuré :**

| PC | Rôle | Région | Volume |
|---|---|---|---|
| `0x9fe0` | lit le buffer | `0x2a00..0x2b27` | 4 720 |
| **`0x9fb8` (I) / `0x9fe2` (Q)** | **écrivent** le buffer | `0x2a00..0x2b27` | 9 472 |
| `0xa07x–0xa08x` | lisent **et** écrivent (workspace de calcul) | `0x2c00..` | ~4 210 R / 2 498 W |
| `0xa0e6/0xa0e7` | lisent/écrivent | `0x2b28..0x2c00` | 7 424 |

**Trois corrections que la trace impose :**
1. **`0x52ED` n est PAS le shadow IMR** (coïncidence de valeur) : c est **le démod lui-même
   (`PC=0x9fe2`) qui l écrit** dans le buffer.
2. **`0x2c00` n est pas une table plate** : le kernel MAC y écrit ses **résultats intermédiaires**
   (valeurs variées `455c, 6ab8, 255c…`). Mon « réf plate 31/-31 » était un instantané.
3. Les verdicts « AR5 jamais dans le buffer » / « entrée DC » venaient de sondes lues **au mauvais
   instant** (store `0x9ac0`, `head` des logs). Ne jamais conclure d un échantillon non daté.

### 🔑 MAILLON CASSÉ — l étage démod produit du DC

`0x9fb8` écrit `0000` **4 319×** et `0x9fe2` écrit `52ed` **4 319×** — même compte : ce sont les
paires **(I,Q)**. Le démod remplit donc `0x2a00..0x2b27` de **`(0000, 52ed)` constants (91 % des
écritures, 7 valeurs distinctes seulement)**, en boucle séquentielle, sur toute la fenêtre
(insn 3.81M→4.31M) — **alors que son entrée IQ est variée et réelle** (`ff6e, c307, 910d…`,
injectée en `0x9260/0x9261`, cf. maillon 2 ci-dessous).

**Donc : ce n est ni l antenne, ni l entrée, ni le corrélateur, ni les pointeurs — c est la
transformation démod (`0x9f00`→`0x9fe2`) qui dégénère en constante.**

| # | Maillon | État | Preuve |
|---|---|---|---|
| 1 | FCCH à l antenne | ✅ | `corr_iq.py` : +67 708 Hz, coh 0.998, dphi +1.00×π/2 |
| 2 | IQ → cellules démod `0x9260/61` | ✅ **(fixé)** | `WATCH_9F00_RD` a montré que le démod lit `0x9260/61` (pas `0x9213`) ; cellules rendues configurables + skip des frames all-zero dans `g_fbs` → FB-STREAM sert enfin de l IQ variée |
| 3 | **démod → buffer `0x2a00`** | ❌ **CASSÉ** | fill `(0000,52ed)` 91 % (trace de flux) |
| 4 | kernel MAC `0xa07x` ↔ `0x2c00` | ✅ tourne | workspace actif, valeurs variées |
| 5 | `d_fb_det` `0x08f8` | ❌ jamais écrit | watchpoint B4 = 0 écriture ; **conséquence** de (3) |

### Fix appliqué : `CALYPSO_DEMOD_NOCLOBBER=1`
L étage démod émulé étant dégénéré, on le **court-circuite** : ses écritures vers
`0x2a00..0x2b27` (PC `0x9fb8`/`0x9fe2`) sont ignorées, et `feed_iq` (`CALYPSO_FB_IQ_DARAM=1`)
reste **autoritaire** sur le buffer — le kernel MAC lit alors la vraie FCCH décimée au lieu du DC.
Gate opt-in, aucun effet quand absent.

```bash
CALYPSO_NATIVE_HELPED=1 CALYPSO_FB_IQ_DARAM=1 CALYPSO_DEMOD_NOCLOBBER=1 ./start-direct.sh
grep -E "DEMOD-NOCLOBBER|DETECTOR-RUN" /root/qemu.log | head
```
Critère : `d_fb_det[08f8]` devient ≠ 0 ⇒ la chaîne native complète la FBSB.

**Résultat mesuré** : le skip fire (`DEMOD-NOCLOBBER skip PC=0x9fb8/0x9fe2`) mais `d_fb_det`
reste 0 (0/55 sur tout le run). Deux faits de plus en sont sortis :

| Fait | Mesure | Fix |
|---|---|---|
| feed_iq ne remplissait que **27 %** du buffer | `wrote=80` sur 296 mots | `CALYPSO_BSP_IQ_DECIM=1` → **`wrote=296`** (buffer plein) ✅ |
| sur-décimage : IQ **déjà à 1 SPS** re-décimée ×4 | `corr_iq.py` : bursts fed `0x2a00` **@1SPS**, N=148 | idem ci-dessus |

Avec buffer plein **et** clobber supprimé, `d_fb_det` reste 0 (0/41). **Point de reprise (méthode,
pas conjecture)** : dumper `data[0x2a00..0x2b28]` en **binaire** pendant le run et le passer dans
`corr_iq.py` — « le buffer contient-il une vraie FCCH ? » devient alors une **mesure** (coh, dphi)
avec l outil déjà validé, au lieu d un jugement à l œil sur 16 paires. Si coh>0.85 ⇒ le problème
est en aval du buffer ; sinon le remplissage est encore fautif.

### Reproduire (Run B)
```bash
CALYPSO_NATIVE_HELPED=1 CALYPSO_B2=1 CALYPSO_B2SEQ=1 CALYPSO_B4=1 CALYPSO_B4B=1 CALYPSO_SCAN_08F8=1 ./start-direct.sh
grep -E "B2 @0x9ac0|B2SEQ|B4-DFBDET-WR|B4B-FLOW|SCAN-08F8" /root/qemu.log | head -60
```
Test décisif du signal (dump entrée `0x2a00`) : `grep "B2SEQ" /root/qemu.log | head`.

---

## Acquis contextuels (autres modes, pour situer)

- `SHUNT_LEGIT` : registration (LU ACCEPT + TMSI), SMS MO/MT bidirectionnel, service tenu,
  Ctrl-C recover — **DONE** (cf `hw/arm/calypso/doc/ETAT_ACTUEL.md`, matrice statut × mode).
- Voix TCH/F : call atteint l'ASSIGNMENT COMMAND → ASSIGNMENT FAILURE (shunt ne présente pas
  le TCH DL) ; call fake_trx = ACTIVE+audio ⇒ réseau OK (cf `hw/arm/calypso/doc/VOIX_PLAN.md`).

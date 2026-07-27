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

**Réinterprétation (⚠️ UNE SEULE cause, en AMONT).** Puisque `0x2a00` est la **vraie entrée** et
qu'elle est **DC/sans FCCH** (B2), la boucle de normalisation qui ne sort jamais (B4B) est
probablement le **comportement CORRECT d'un chercheur de FCCH sur entrée vide** — **pas** un mur
de flux. `d_fb_det` jamais écrit (B4) = **conséquence** d'une entrée sans ton, pas un bug de
contrôle de flux. Les deux causes que je chassais **s'effondrent en une seule, en amont : la
chaîne RF/ABB émulée ne dépose pas de FCCH dans `0x2a00`.** Cause probable : modèle **AFC**
(DAC TWL3025→VCXO) décalé → le ton FCCH n'atterrit pas dans le bin où la ROM le cherche ; et/ou
gain **TRF6151** → l'offset DC. **B2SEQ CONFIRME** : `0x2a00` = **toutes `(21229,21229)` = DC plat, ZÉRO FCCH** (pas de rotation Fs/4). Entrée morte prouvée au byte.

**Le shunt host-side reste la voie qui marche** : gr-gsm trouve la FCCH sur le **même downlink**
→ le signal EXISTE à l'antenne → la rupture est dans **RF-modèle → BSP → `0x2a00`**, pas dans le
DSP. Voir la matrice statut × mode dans `hw/arm/calypso/doc/ETAT_ACTUEL.md`.

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

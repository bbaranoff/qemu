# Calypso QEMU — État de session 2026-04-06

## TL;DR
Pipeline osmocom complet câblé et fonctionnel jusqu'au BSP du DSP. Le DSP atteint
IDLE proprement. **Blocage final** : boucle MVDD à PROM0 0x76FE corrompt l'IMR,
empêche `BRINT0` d'être servi, donc `FB_DET` n'est jamais produit et FBSB échoue.

## Architecture validée

```
osmo-bts-trx                    bridge.py                          QEMU + sercomm_gate
─────────────                   ─────────                          ───────────────────
local 5800/5801/5802 ──────►  bind 5700 (CLK out)
                              bind 5701 (TRXC, ack local)
                              bind 5702 (TRXD)  ──── GMSK ────► bind 6702 (TRXD)
                                                                       │
                                                                       ▼
                                                            calypso_trx_rx_burst()
                                                                       │
                                                                       ▼
                                                                c54x_bsp_load()
                                                                       │
                                                                       ▼
                                                            [PORTR 0xF430 → DSP]

mobile  ─── unix /tmp/osmocom_l2_1 ─── bridge.py ─── PTY ─── QEMU UART modem ─── firmware
                                       (sercomm DLCI 5)         (sercomm_gate parser)
```

### Conventions de port (osmocom)
- BTS local **5800** série, remote **5700** série
- Mobile/L1 (gate) **6700** série, peer (bridge) **6800** série
- L1 binde base+0/+1/+2 (CLK/TRXC/TRXD), peer binde base+100/+101/+102

### Composants modifiés cette session
| Fichier | Modif |
|---|---|
| `/opt/GSM/qemu-src/bridge.py` | NOUVEAU — unifie L1CTL/PTY + UDP TRX relay + GMSK + génération CLK |
| `/opt/GSM/qemu-src/hw/arm/calypso/sercomm_gate.c` | Réécrit — PTY HDLC parser + UDP CLK/TRXC/TRXD bind, callback `trxd_cb` → `calypso_trx_rx_burst` |
| `/opt/GSM/qemu-src/include/hw/arm/calypso/sercomm_gate.h` | Ajout `sercomm_gate_init(int base_port)` |
| `/opt/GSM/qemu-src/hw/arm/calypso/calypso_mb.c` | Appel `sercomm_gate_init(6700)` à la fin de `calypso_machine_init` |
| `/opt/GSM/qemu-src/hw/arm/calypso/calypso_soc.c` | `l1ctl_sock_init(...)` commenté (le bridge possède maintenant le socket L1CTL) |
| `/opt/GSM/qemu-src/hw/char/calypso_uart.c` | `l1ctl_sock_uart_tx_byte(ch)` commenté (évitait double-parsing TX) |
| `/opt/GSM/qemu-src/run.sh` | Réécrit — kill propre, build sain, ordre QEMU→bridge→bts→mobile |
| `/opt/GSM/qemu-src/hw/arm/calypso/calypso_c54x.c` | Bug 19 (F4EB=RETE), Bug 21 (F4xx exact-match), Bug 22 (0x6Dxx=MAR), E8 CMPR |

### Composants explicitement non touchés
- `calypso_trx.c` — version main HEAD (74b2464), restaurée tel quel
- `l1ctl_sock.c` — code intact, juste désinitialisé
- `calypso_c54x.h` — version main GitHub

## État DSP / C54x

### Baseline
`calypso_c54x.c` = version GitHub `bbaranoff/qemu` `74b2464e` (115 152 octets).
**Pas la version 138K du dossier `qemU/`** — celle-ci a 17 fix opcodes supplémentaires
(session 2026-04-05 night4) mais introduit une régression : DSP boucle à PC=0x81af
sans jamais atteindre IDLE.

### Bugs déjà fixés dans 115K (vérifié)
- Bug 3 partiel (PROM1 mirror >= 0xE000)
- Bug 7 (lk_used pour modes 0xC-0xF)
- Bug 8 (BANZ teste AR après resolve_smem)
- Bug 9 (F6xx MVDD pour sub >= 8)
- Bug 10 (RPTB skip pendant rpt_active)
- Bug 11 (RPT F5xx return 0)
- Bug 12 partiel (NORM bits 39/38, 1 site sur 2)
- Bug 16 (boot ROM stubs prog[0x0000-0x007F] = 0xF073 RET)
- Bug 24 (prog_write reject >= 0xE000)

### Bugs ajoutés cette session
- **Bug 19** (F4EB = RETE) — nouveau handler après F4E3
- **Bug 21** (F4xx arithmétique) — bloc complet 211 lignes injecté avant le nibble switch (208 handlers exact-match : SAT/NEG/ABS/MPYA/SQUR/EXP/NORM/MAX/MIN/SUBC/ROR/ROL/MACA/CMPL/RND/ADD/SUB/LD/SFTA/SFTL + RSBX)
- **Bug 22** (0x6Dxx = MAR) — handler en tête de `case 0x6/0x7`
- **E8 = CMPR cond,ARn** — handler avant E1xx (n'était pas dans la liste BUGS_AND_FIXES.md mais nécessaire)

### Bug 20 (RPTBD pc+4) — APPLIQUÉ PUIS REVERTÉ
La doc dit `RSA = PC+4` (delay slots avant la boucle). Quand on l'applique aux
deux handlers F272 (L702 et L941), **régression majeure** :
- Avant : DSP atteint IDLE proprement, IMR corrompue uniquement depuis PC=0x76fe (~2900 changes)
- Après : DSP ne fait plus IDLE, PC erre dans tout PROM0/PROM1, IMR corrompue depuis ~10 PCs différents (~72 000 changes)

**Hypothèse** : le 115K a été écrit en s'appuyant sur le bug `pc + 2`, donc d'autres
parties du code (offsets de loop body, BRC compensé manuellement) sont calibrées
pour ce comportement. Le fix isolé casse les dépendances.

## Le blocage final

### Symptôme observé
```
TINT0: fn=29 page=0x7900 ran=16361 PC=0x16ac idle=1 IMR=0x0000
TINT0: fn=30 page=0x7900 ran=16361 PC=0x16ac idle=1 IMR=0x0000
[c54x] IRQ vec=21 bit=5: ... IMR=0x0000  ← BRINT0 firé mais masqué
mobile: FBSB RESP: result=255            ← échec FBSB (no cell)
```

### Cause confirmée
Boucle d'init DSP autour de PROM0 0x76FB :
```
0x76fb = f272  RPTBD pmad
0x76fc = 7700  REA target = 0x7700
0x76fd = e800  CMPR cond, ARn (delay slot 1)
0x76fe = f6b9  MVDD ar3→ar1 (delay slot 2 OU loop body selon Bug 20)
0x76ff = 3892  loop body
0x7700 = 3892  loop body end
```
- `BRC = 63` (chargé via `STM #0x003F, BRC` à 0x76f9)
- `MVDD f6b9` copie `data[ar3]` → `data[ar1]`
- `ar1` finit par valoir 0 (= adresse MMR `IMR`) → corruption
- Résultat : IMR = 0x0000 → tous les interrupts DSP masqués → BRINT0 ignoré → pas de FB_DET → FBSB échoue

### Pourquoi Bug 20 ne le fixe pas
- Avec `RSA = PC+4 = 0x76ff`, la boucle exclut bien le MVDD
- Mais le DSP commence alors à diverger ailleurs (probablement parce que d'autres
  boucles RPTBD dans le firmware ont été contournées avec des trucs qui supposent
  `pc+2`)
- Résultat : régression globale

## Comment lancer

```bash
docker exec osmo-operator-1 /opt/GSM/qemu-src/run.sh
```

`run.sh` orchestre : kill propre → QEMU (PTY) → bridge.py (PTY+L1CTL+UDP) →
osmo-bts-trx → mobile, dans une session tmux `calypso`.

Logs :
- `/tmp/qemu.log` (énorme — filtrer avec `grep -vE "RPTB EXIT|^\[c54x\] SP|TINT0:|^\[INTH\]|d_dsp_page WR"`)
- `/tmp/bridge.log`
- `/tmp/bts.log`
- `/tmp/mobile.log`

## Commandes de diagnostic utiles

```bash
# Compter les corruptions IMR
grep -c "IMR change" /tmp/qemu.log

# Top PCs source des corruptions
grep "IMR change" /tmp/qemu.log | awk '{print $NF}' | sort | uniq -c | sort -rn | head

# Dernier état DSP (idle? IMR? PC?)
grep "TINT0:" /tmp/qemu.log | tail -3

# Stats bridge (CLK, TRXC, bursts DL/UL forwardés)
tail /tmp/bridge.log

# Forward des bursts arrive bien au gate ?
grep "TRXD remote" /tmp/qemu.log

# IRQ DSP (vec 21 = BRINT0)
grep -E "c54x.*IRQ vec" /tmp/qemu.log | tail
```

## Pour poursuivre

### Piste 1 — Désassembler 0x76FB-0x7700 (recommandé)
Décoder à la main les opcodes `f272 7700 e800 f6b9 3892 3892` selon SPRU172C
et déterminer si la boucle correcte est `pc+2` ou `pc+4`. Vérifier avec
`gdb-binutils` (objdump-tic54x) ou ti-tooling.
- Si la sémantique TI dit `pc+4` mais ça casse → un AUTRE bug plus profond
  compense actuellement.
- Si la sémantique dit `pc+2` → la doc Bug 20 est fausse, le `pc+4` du qemU
  138K est en fait la vraie régression.

### Piste 2 — Bisect entre commits GitHub
Entre `0771c20` (initial baseline UDP) et `74b2464` (HEAD actuel), il y a
~30 commits. Trouver lequel a introduit le bug d'init DSP au 0x76FE.
```bash
cd /home/nirvana/qemu
for c in $(git log --oneline 0771c20..74b2464 -- hw/arm/calypso/calypso_c54x.c | awk '{print $1}'); do
    echo "=== $c ==="
    git checkout $c -- hw/arm/calypso/calypso_c54x.c
    # rebuild + test FBSB
done
```

### Piste 3 — Forcer IMR = 0xFFFF dans c54x_reset() (workaround)
Hack rapide pour vérifier si c'est BIEN l'IMR qui bloque :
```c
// dans c54x_reset() de calypso_c54x.c
s->imr = 0xFFFF;  // tous interrupts unmasked en permanence
```
+ ignorer toute écriture à `MMR_IMR` :
```c
case MMR_IMR:
    return;  // ignore writes, keep IMR locked open
```
Si avec ce hack le DSP produit FB_DET → confirme que la corruption IMR est
bien la cause unique. Sinon il y a un autre blocage.

### Piste 4 — Tracer ar[1] avant chaque MVDD à 0x76fe
Ajouter dans le handler MVDD :
```c
if (s->pc == 0x76fe) {
    fprintf(stderr, "MVDD@76fe ar[%d]=0x%04x → ar[%d]=0x%04x\n",
            xar, s->ar[xar], yar, s->ar[yar]);
}
```
Pour comprendre pourquoi ar1 vaut 0.

### Piste 5 — Comparer encodages F6xx avec spec TI
Le `f6b9` actuel décode comme `MVDD xar=3 yar=1 xmod=incr ymod=incr`.
Vérifier que c'est la bonne interprétation. Le bit layout dans 138K vs 115K
peut différer.

### Piste 6 — Vérifier les autres handlers RSA = pc+2 manquants
Sur 5 sites RSA :
```
702 F272      ← RPTBD, doit être pc+4 mais casse
851 F82x      ← RPTB non-delayed, pc+2 OK
941 F272      ← RPTBD (doublon)
1320 EBxx     ← RPTB[D] selon bit, à vérifier
2234 C2/C3/C6/C7 ← RPTB (C2/C6) ou RPTBD (C3/C7), à différencier
```
La piste 1320 et 2234 peuvent encoder des RPTBD mal traités.

## Snapshots à connaître

Sur `/home/nirvana/all_qemu_stuff/ALL-QEMUs/` il y a 44+ snapshots horodatés.
Particulièrement intéressants :
- `qemu-calypso-20260403-123539-correctif18-UDP6802_bursts-PTY_L1CTL_only`
  (vieille tentative de séparation UDP/PTY — équivalent de ce qu'on a refait)
- `qemu-calypso-20260403-134846-GSMTAP_WORKING`
- `qemu-calypso-20260403-222046-FIRS+GMSK-no_more_unimpl`

Sur `/home/nirvana/stuff/qemU/` il y a la version 138K tardive avec tous les
fix opcodes (mais régression DSP idle). Utile comme référence pour extraire
des handlers manquants (E6/EB/EE/EF/E8 → fait, mais à compléter avec
prudence : on a déjà constaté que certains injects cassent l'init).

## Rappels importants
- **Toujours `make` après chaque `sed`** dans le container pour qu'il prenne effet
- **`touch ../hw/arm/calypso/<file>.c`** avant `make` parce que ninja ne détecte
  pas toujours les changements via docker cp / sed direct
- **Le PTY est pour L1CTL only** — bursts via UDP et BSP, jamais via DLCI 4
- **L'IMR à 0x0000** est le symptôme principal du blocage actuel
- **`l1ctl_sock_init` doit rester désactivé** — sinon double-bind du socket

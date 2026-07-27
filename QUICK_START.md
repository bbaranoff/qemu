# QUICK START — QEMU-Calypso

Émulation QEMU du baseband GSM TI Calypso (ARM946 + DSP TMS320C54x mask-ROM)
faisant tourner le firmware osmocom-bb, face à un cœur réseau Osmocom complet.

> **Vérité courante** : [`hw/arm/calypso/doc/ETAT_ACTUEL.md`](hw/arm/calypso/doc/ETAT_ACTUEL.md).

---

## 1. Installation

Tout tourne dans un **conteneur** (`osmo-operator-1`), image bâtie depuis
`/opt/GSM/osmo_egprs/Dockerfile`. Il embarque :

| Composant | Rôle |
|---|---|
| Cœur Osmocom (STP/HLR/MSC/MGW/BSC + `osmo-bts`) | Réseau GSM |
| `fake_trx` / `osmo-trx` | Radio (downlink GSM réel I/Q) |
| `gr-gsm` | Décodage SB/SI du downlink |
| **`qemu-src`** (ce dépôt) | Le baseband Calypso émulé |
| `osmocom-bb-transceiver` | Firmware L1 (`layer1.highram.elf`) + blobs DSP-ROM |

### Reconstruire (depuis le conteneur)
```bash
cd /opt/GSM/qemu-src/build && ninja qemu-system-arm
```
Le binaire : `/opt/GSM/qemu-src/build/qemu-system-arm`. Firmware + ROM DSP
(`calypso_dsp.PROM0..3/DROM/PDROM`) dans `/opt/GSM`.

### Reconstruire l'image complète (hôte)
```bash
cd /opt/GSM/osmo_egprs && ./build.sh          # ou docker build -f Dockerfile
```

---

## 2. Lancer

Le launcher `osmo_egprs/start-direct.sh` monte cœur + radio + Calypso.

### Modes de plateforme (`MODE=`)
| Mode | Ce qu'il monte |
|---|---|
| `qemu` *(défaut)* | Cœur (no-process) + `qemu-src/start-clean.sh` (Calypso QEMU) |
| `faketrx` | Cœur + osmo-bts + fake_trx + trxcon + mobile (soft) |
| `virtphy` | Cœur + osmo-bts-virtual + virtphy + mobile |
| `faketrx-qemu` | Cœur + fake_trx vivant, Calypso QEMU attaché |

```bash
cd /opt/GSM/osmo_egprs
./start-direct.sh                 # mode qemu (défaut)
MODE=faketrx ./start-direct.sh
```

### Modes Calypso (socle `calypso.env` + `calypso_X.env`)
`start-clean.sh` source `calypso.env` (socle commun) qui, selon un flag, source
le fichier du mode. **Toutes les vars sont overridables en CLI** (idiome `:=`).

| Mode | Flag | Fichier | Chaîne FBSB |
|---|---|---|---|
| **Base (shunt_legit)** | *(défaut)* | `calypso.env` | Host : real_fb + gr-gsm → API RAM |
| **No-legit** | `CALYPSO_SHUNT_NO_LEGIT=1` | `calypso_shunt_no_legit.env` | Injections explicites, DSP shunté |
| **Natif pur** | `CALYPSO_NATIVE=1` | `calypso_native.env` | DSP : FB-STREAM → `data[0x9213/0x9215]` |
| **Natif aidé** | `CALYPSO_NATIVE_HELPED=1` | `calypso_native_helped.env` | DSP + `feed_iq` DARAM (`0x9210`) |

```bash
./start-direct.sh                              # base shunt_legit
CALYPSO_SHUNT_NO_LEGIT=1 ./start-direct.sh
CALYPSO_NATIVE=1 ./start-direct.sh
CALYPSO_NATIVE_HELPED=1 ./start-direct.sh
```

Logs : `/root/qemu.log` (QEMU/DSP), `/root/mobile.log` (mobile Calypso).

---

## 3. État — ce qui est atteint

### Fonctionnel (chaîne host — base / no-legit)
| Objectif | État | Preuve |
|---|---|---|
| FB/SB (sync) | ✅ | `DISPATCH SB BSIC=7 (gr-gsm REEL)` |
| RXLEV serving | ✅ | `RLA_C -53 dBm`, C1/C2 > 0 |
| Camp (C3) | ✅ | `normal service` |
| **Location Update** | ✅ | `LOCATION UPDATING ACCEPT (lai=001-01-1)` |
| **MO SMS** | ✅ | `RX SMS RP-ACK` |
| MT SMS (échange) | ✅ | transaction SDCCH complète, `RP-ACK` reçu |
| **MT SMS stable** | ⚠️ | échange OK mais re-sync instable après le dédié |

### En cours / non atteint
| Objectif | État | Verrou identifié |
|---|---|---|
| SMS stables (répétés) | ❌ | Go-live pas rejoué sur `L1CTL_RESET_REQ FULL` → re-sync timeout (BGEN one-shot) |
| Camp maintenu post-dédié | ⚠️ | `no cell info` corrigé (expiry agch) mais re-acquisition FBSB échoue après FULL reset |
| `d_fb_det` natif | ❌ | Corrélateur DSP = vrai corrélateur ; entrée `0x9213/0x9215` **prouvée inscriptible** (FB-STREAM) ; reste completion FBSB + dispatch par-frame |

### Acquis techniques (buildés)
| Élément | État |
|---|---|
| Entrée FB native `0x9213/0x9215` inscriptible | ✅ prouvé (rampe relue par le démod) |
| FB-STREAM (feed IQ FCCH → démod) | ✅ |
| Résolution ELF dynamique (`l1s`/`last_rach`) | ✅ |
| Fix req-ref SMS (`last_rach`, défaut ON) | ✅ |
| Expiry agch (fix `no cell info`) | ✅ |
| Env socle + modes (`:=` overridables CLI) | ✅ |

---

## 4. Pièges

- **NE PAS feed `0x2a00`** (workzone de SORTIE du démod, régénéré par le DSP) — feed la source `0x9213/0x9215`.
- `CALYPSO_ARM2DSP_CTRLSYS=1` **casse** le go-live natif (re-force B_TASK_ABORT) — laisser à `0`.
- Caps de log (`KEEP-IMR <8`, `SHADOW-DADST <40||%2000`) : un `grep` qui « s'arrête » ≠ perte réelle.
- `calypso_fbsb.c` : compteurs désormais réels (avant `0 0 0 0` = red herring).

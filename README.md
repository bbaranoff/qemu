# QEMU Calypso – GSM Layer1 Emulation (WIP)

Ce dépôt contient un port expérimental du SoC **TI Calypso (GSM 2G)** dans QEMU, avec objectif de faire tourner :

- le bootloader Compal
- Layer1 Osmocom
- un TRX virtuel
- osmocon
- puis une pile GSM minimale

On est actuellement dans une phase de **hardware bring-up logiciel** : le firmware s’exécute réellement, mais attend encore certains registres matériels non implémentés.

Ce n’est pas un “emulateur prêt à l’emploi” — c’est du reverse + silicon bring-up.

---

## État actuel

### ✅ Fonctionnel

- Machine QEMU `-M calypso`
- CPU ARM946E-S
- Mapping RAM / Flash
- Chargement ELF (`loader.highram.elf`, `trx.highram.elf`)
- Exécution du vrai code Layer1
- Initialisation GSM :

On observe dans les traces :

- `do_global_ctors`
- `prim_rach_init`
- `prim_tx_nb_init`
- `l1s_prim_fbsb_init`
- `prim_tch_init`

Donc :

👉 le firmware GSM est bien vivant.

- TRX virtuel actif :

```

TRX bridge ready
DSP API
TPU
ULPD
TDMA IRQ

```

IRQ + timers fonctionnent.

---

### ❌ Bloquant actuellement

Le firmware boucle sur :

```

ULPD @ 0xfffe2800

````

Typiquement :

```c
while (!(ULPD_STATUS & READY));
````

Dans QEMU, ce registre retourne toujours 0 → attente infinie.

➡ Il faut encore faker le bit READY du domaine ULPD.

C’est la première vraie barrière “hardware”.

Une fois patchée :

* UART devrait parler
* osmocon devrait handshaker
* Layer1 passera au stade suivant

---

## Dépendances

### Build QEMU

Testé avec QEMU 9.x.

Paquets :

```bash
sudo apt install build-essential ninja meson \
    libglib2.0-dev libpixman-1-dev \
    libslirp-dev libgtk-3-dev \
    python3
```

Build :

```bash
git clone https://github.com/bbaranoff/qemu
cd qemu
git checkout stable-9.2
mkdir build
cd build
../configure --target-list=arm-softmmu
ninja
```

Le binaire est :

```
build/qemu-system-arm
```

---

### Osmocom

Requis pour osmocon :

```bash
git clone https://gitea.osmocom.org/phone-side/osmocom-bb
cd osmocom-bb
make
```

Chemin attendu :

```
osmocom-bb/src/host/osmocon/osmocon
```

---

### Firmwares Compal

Placer dans :

```
compal_e88/
```

Minimum :

* loader.highram.elf
* layer1.highram.bin
* trx.highram.elf

---

## Lancement simple

Boot sans firmware :

```bash
./qemu-system-arm -M calypso -cpu arm946 -nographic -monitor none
```

Boot avec loader :

```bash
./qemu-system-arm \
  -M calypso \
  -cpu arm946 \
  -kernel compal_e88/loader.highram.elf \
  -nographic \
  -monitor none
```

---

## Script complet (QEMU + UART + osmocon + TRX)

Voir `run.sh` :

* démarre QEMU
* expose UART via PTY
* bridge avec socat
* charge Layer1 via osmocon
* lance TRX loopback

Principe :

```
QEMU → PTY → socat → osmocon
                 → trx_test.py
```

---

## Où on en est techniquement

Pipeline actuel :

```
ARM reset
→ ELF loaded @ 0x00820000
→ Layer1 init
→ GSM primitives init
→ attente ULPD READY   ← blocage ici
```

C’est exactement la phase de **power / oscillator bringup**.

Ce n’est plus du QEMU “classique”, mais du hardware modeling.

---

## Prochaines étapes

1. Forcer READY dans ULPD
2. Laisser passer init power
3. Vérifier UART TX
4. Handshake osmocon
5. Continuer bring-up périphériques

Approche : incrémentale, registre par registre.

---

## Disclaimer

Ce projet est :

* expérimental
* instable
* non sécurisé
* non conforme 3GPP

Il est destiné à :

* reverse engineering
* recherche
* compréhension GSM bas niveau
* émulation firmware legacy

Pas production.

---

## Auteur

Travail exploratoire / reverse / bring-up par Bastien.

---

## Licence

Libre / recherche. À préciser selon intégration upstream éventuelle.


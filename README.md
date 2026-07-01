# qemu-src — le fork Calypso QEMU (arbre de build & run)

Arbre de travail **autoritaire** du fork QEMU pour l'émulation du baseband
**TI Calypso** (ARM7 + DSP TMS320C54x). C'est ici qu'on **build** et qu'on
**lance** le pipeline.

```
QEMU (genuine 9.2.4)  +  sous-système Calypso  =  ce fork (qemu-src)
```

> L'**overlay** qui isole le delta vs QEMU upstream (et sait le ré-appliquer sur
> un genuine via `make-fork.sh`) vit dans `../qemu-calypso`. `qemu-src` est le
> résultat matérialisé, directement compilable — pas un overlay.

## Build

Le répertoire `build/` est déjà configuré (produit `qemu-system-arm`) :

```sh
cd build && ninja qemu-system-arm
```

Reconfiguration depuis zéro si besoin :

```sh
mkdir -p build && cd build && ../configure --target-list=arm-softmmu && ninja qemu-system-arm
```

## Lancer le pipeline

```sh
./start-clean.sh          # alias racine -> bash_scripts/start-clean.sh
                          # source calypso.env puis exec bash_scripts/run.sh
```

Chemin DSP réel (défauts dans `run.sh`) : `CALYPSO_DSP=c54x`,
`CALYPSO_DSP_REG_MODE=c54x`, `CALYPSO_DSP_SHUNT=0`. Log runtime : `/root/qemu.log`.
Les scripts sont relocatables (`ROOT` détecté depuis `bash_scripts/`).

## Structure

**Arbre QEMU + sous-système Calypso** :

| Chemin | Rôle |
|---|---|
| `hw/arm/calypso/` | Le sous-système : DSP c54x, TPU/TDMA, BSP, API-RAM, machine. |
| `include/hw/arm/calypso/` | Headers publics du sous-système. |
| `hw/arm/{meson.build,Kconfig}` · `hw/{char,intc,ssi,timer}/meson.build` | Enregistrement machine `calypso` + périphériques. |
| `configs/devices/arm-softmmu/default.mak` | Active la machine dans le build arm-softmmu. |
| `build/` | Répertoire de build (ninja) — produit `qemu-system-arm`. |
| (le reste) | QEMU 9.2.4 upstream, inchangé. |

**Glue projet (runtime / dev)** :

| Chemin | Rôle |
|---|---|
| `calypso.env` | Env du pipeline (sourcé par `start-clean.sh`). |
| `start-clean.sh` · `bash_scripts/` | Lancement (`run.sh`, `start-clean.sh`, runners GSM). |
| `python_scripts/`, `gdb_scripts/`, `opt-gsm-scripts/`, `diag/` | Décodage / GDB / sniff / diag. |
| `scripts/` | Outils build DSP (`make_dsp_bin_L1.py`, `populate-si.sh`, …) + scripts QEMU. |
| `dsp_blobs/`, `calypso_dsp.txt` | ROM DSP (dump réel) + blobs chargés par la machine. |
| `cfgs/`, `patches/` | Configs (osmo-trx-ipc, mobile) + patch externe (osmo-bts skew). |

## Documentation

Toute la doc est centralisée dans **[`doc/`](doc/)** :

- **Index maître** : [`doc/doc_master.md`](doc/doc_master.md) — README sous-système,
  schematics, corrélateur, décodeur, rapports, sessions archivées.
- **Projet** (flux e2e, threading, statut) : [`doc/project/`](doc/project/)
  (`CLAUDE.md`, `ARCHITECTURE.md`, `*_FLOW.md`, …).

## État courant

Objectif : réveiller le vrai DSP c54x pour qu'il écrive lui-même `d_fb_det != 0`
(détection FB/FCCH), **sans hack** (règle #1 : on répare le câblage de l'émulateur,
jamais l'état interne du DSP).

**Blocker terminal** : les interruptions DSP restent masquées (`IMR=0x0000` tout le
run, jamais ré-armé) → le DSP déraille (`POST-BOOTSTUB-RET`) au lieu de tourner le
corrélateur → `d_fb_det=0`, `FB0_SEARCH fb0_ret=0`, `NO_CELL_FOUND`. Cause en amont
= le **handshake go-live ARM→DSP** n'aboutit pas (l'ARM n'assert jamais l'enable ;
`api_write_cb` non câblé). Détail + leviers : [`doc/doc_master.md`](doc/doc_master.md).

> Note infra : la machine tourne headless (`-serial pty`) et **doit** être lancée
> avec `-display none` (sinon échec fatal `could not read keymap file: 'en-us'`) —
> déjà posé dans `bash_scripts/run.sh`.

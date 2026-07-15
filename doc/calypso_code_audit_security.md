# Audit de code — émulation Calypso (sécurité / robustesse mémoire)

Date : 2026-07-15
Périmètre : code C spécifique au fork (émulation baseband Calypso), soit
~11 000 lignes hors table d'opcodes générée `calypso_c54x.c`. Fichiers audités :
`hw/arm/calypso/*.c`, `hw/char/calypso_uart.c`, `hw/intc/calypso_inth.c`,
`hw/ssi/calypso_{spi,i2c}.c`, `hw/timer/calypso_timer.c`,
`tools/calypso-ipc-device/*.c`.

Méthode : revue ciblée sur les surfaces de parsing d'entrées externes
(sockets L1CTL/sercomm, IPC unix, mémoire partagée, TRXD) et sur les handlers
MMIO (offset contrôlé par le guest). Chaque constat listé ci-dessous a été
vérifié sur les sources.

## Synthèse

Le bug classique de QEMU — offset MMIO contrôlé par le guest utilisé comme
index de tableau sans borne — **n'est pas présent** : les deux handlers qui
indexent par offset (`inth.ilr[32]`, `spi.abb_regs[256]`) sont correctement
bornés. Les chemins de parsing réseau/guest (L1CTL, sercomm, TRXD, APDU SIM)
sont défensifs.

Le point le plus grave est côté **outil hôte `calypso-ipc-device`** (constat
#1, écriture hors-bornes déclenchable par un client du socket IPC maître).

| # | Sévérité | Fichier | Nature |
|---|----------|---------|--------|
| 1 | Critique | tools/calypso-ipc-device/calypso_ipc_device.c:255 | Écriture OOB (`num_chans` non borné) |
| 2 | Haute | hw/arm/calypso/calypso_dsp_shunt.c:1117 | Récursion infinie (MMIO auto-référent) → crash |
| 3 | Moyenne | tools/calypso-ipc-device/ipc_shm.c:184 | Lecture OOB (`data_len` shm non validé) |
| 4 | Moyenne | hw/arm/calypso/calypso_dsp_shunt.c:1102 | Write overlay non commité → handshake cassé |
| 5 | Moyenne | hw/intc/calypso_inth.c:75 | Index périmé → interruption perdue |
| 6 | Faible-moy | hw/arm/calypso/calypso_mb.c:147 | Largeur de bus flash 8 bits au lieu de 16 |
| 7 | Faible | hw/arm/calypso/calypso_arm2dsp.c:116 | Écriture OOB (index via env, off par défaut) |
| 8 | Faible | hw/arm/calypso/calypso_bsp.c:1073,1362 | Lecture OOB en log debug (env) |
| 9 | Faible | hw/arm/calypso/calypso_sim.c:431 | READ_BINARY borné sur `size`, pas sur le buffer |
| 10 | Faible | hw/arm/calypso/calypso_dsp_shunt.c:1636 | Bloc `cfile2` dupliqué (artefact debug) |
| 11 | Faible | tools/calypso-ipc-device/ipc_shm.c:43,84 | `malloc` non vérifié + `condattr` non initialisé |
| 12 | Faible | hw/arm/calypso/calypso_full_pcb.c:106 | Init paresseuse non atomique (course) |

---

## Détails

### 1. CRITIQUE — `num_chans` non borné → écriture hors-bornes de pointeurs
`tools/calypso-ipc-device/calypso_ipc_device.c:255-259`

`open_req->num_chans` est un `uint32_t` reçu sur le socket unix maître
(`OPEN_REQ`), et n'est **jamais validé** malgré le commentaire ligne 245
(« Here we verify num_chans… » — aucune vérification n'existe). La boucle
d'init producteur écrit `ios_tx_to_device[i]` / `ios_rx_from_device[i]`, tableaux
globaux fixes de **8 entrées** (lignes 72-73).

Scénario : un client envoie `OPEN_REQ` avec `num_chans = 100` → la boucle
déborde les deux tableaux de 8 et corrompt les globales adjacentes (`shm`,
`global_dev`, `decoded_region`, `global_ctrl_socks`) avec des pointeurs
`ipc_shm_io*`, déréférencés plus tard. Même cause dans `ipc_tx_open_cnf`
(lignes 206-213) qui dépasse `open_cnf.chan_info[MAX_NUM_CHANS=30]`.

Correctif : borner tôt dans `ipc_rx_open_req` :
`if (open_req->num_chans == 0 || open_req->num_chans > 8) { rejeter; }`
(et ne jamais dépasser `MAX_NUM_CHANS`).

### 2. HAUTE — lecture MMIO auto-référente → récursion infinie → crash
`hw/arm/calypso/calypso_dsp_shunt.c:1112-1118` (auto-lecture aussi en :1027)

Le shunt enregistre un overlay IO de 2 octets à `0xFFD001A8` avec priorité 10
sur `system_memory` (lignes 1690-1696). Son handler de lecture
`shunt_d_dsp_page_read()` appelle `shunt_read_w(BASE_API_NDB + NDB_D_DSP_PAGE)`,
c.-à-d. sa **propre** adresse `0xFFD001A8` via `dma_memory_read` (ligne 230-233),
qui redispatch vers la région de plus haute priorité = l'overlay lui-même →
récursion non bornée → épuisement de pile → SIGSEGV.

Le garde de réentrance MMIO de QEMU ne s'applique pas : la région est créée
avec `memory_region_init_io(trigger, NULL, …)` (owner = NULL, ligne 1691),
donc `mr->dev == NULL` et le garde est ignoré. Atteignable si le firmware
ARM lit `d_dsp_page` (2 octets à `0xFFD001A8`) pendant que l'overlay est actif,
ou via `shunt_route_to_c54x()` (ligne 1027) → DoS.

Correctif : ne pas ré-entrer sur l'adresse overlayée ; conserver une copie
shadow de la dernière valeur écrite de `d_dsp_page` et la renvoyer, ou lire un
miroir non-overlayé.

### 3. MOYENNE — `data_len` de mémoire partagée non validé → lecture OOB
`tools/calypso-ipc-device/ipc_shm.c:175-189`

`buf->data_len` est lu depuis la mémoire partagée (écrite par le pair
osmo-trx-ipc) sans contrôle contre `buffer_size`, et
`r->partial_read_begin_ptr` s'accumule (`+= num_samples`, ligne 186) sans
borne haute. Si le pair positionne `data_len > buffer_size`, les appels
successifs prennent la branche `else` (ligne 184) et `partial_read_begin_ptr`
croît jusqu'à ce que
`memcpy(out_buf, &buf->samples[partial_read_begin_ptr*2], …)` (ligne 185)
lise au-delà de l'allocation `samples` → lecture OOB (crash / fuite d'info).

Correctif : valider `buf->data_len <= stream->buffer_size` et
`partial_read_begin_ptr + n <= buffer_size` avant chaque `memcpy` ; réinitialiser
le buffer en cas de violation.

### 4. MOYENNE — write overlay non commité (pass-through cassé)
`hw/arm/calypso/calypso_dsp_shunt.c:1102-1110`

`shunt_d_dsp_page_write()` n'appelle que `shunt_latch_task(value)` et n'écrit
pas `value` dans la RAM API sous-jacente. Le commentaire prétend des
« pass-through semantics », mais une région IO de plus haute priorité **remplace**
l'accès à la RAM (QEMU n'écrit pas aussi la région basse). L'écriture de
`d_dsp_page` par l'ARM est donc silencieusement perdue de `dsp_ram`, et toute
lecture ultérieure (une fois #2 corrigé) renvoie une valeur périmée → handshake
DSP cassé.

Correctif : écrire explicitement la valeur dans le backing store après le
latch, ou faire de l'overlay une région RAM-backed avec hook d'écriture.

### 5. MOYENNE — index périmé `ith_v`/`fiq_v` → interruption perdue
`hw/intc/calypso_inth.c:69,74-76` puis edge-clear lignes 128-133 / 161-164

`fiq_v` n'est assigné (ligne 75) que si `best_fiq >= 0` ; sinon il garde sa
valeur précédente. `ith_v` n'est remis à 0 que si les deux canaux sont inactifs
(ligne 69). Une lecture ultérieure de FIQ_NUM (0x12/0x82) renvoie ce numéro
périmé et, s'il vaut 4, 5 ou 15, exécute `s->levels &= ~(1u<<num)` — effaçant
le bit de niveau d'une source qui n'est pas celle acquittée.

Scénario : une source FIQ (ligne 6/SIM) devient inactive pendant qu'une source
IRQ est active ; `fiq_v` garde p.ex. 15 (API) ; la lecture de FIQ_NUM efface le
bit 15 de `levels`, abandonnant une interruption API de niveau réellement
pendante → interruption manquée / busy-loop firmware (exactement la classe de
blocage décrite par les commentaires du fichier). Pas de corruption mémoire :
`num ∈ {4,5,15}`.

Correctif : dans `calypso_inth_update`, toujours affecter `ith_v`/`fiq_v`
(= 0 quand `best_* < 0`) ; n'effectuer l'edge-clear dans les handlers de lecture
que si une source est réellement acquittée (`best_* >= 0`).

### 6. FAIBLE-MOYENNE — largeur de bus flash 8 bits au lieu de 16
`hw/arm/calypso/calypso_mb.c:147`

`pflash_cfi01_register(...)` reçoit `width = 1` (8 bits), alors que le
commentaire (124-125), le log (`width=2 (16-bit)`, ligne 137) et le vrai
matériel (Intel 28F320, 16 bits sur Calypso CS0) disent 16 bits. Les requêtes
CFI et les accès flash 16 bits sont modélisés avec une géométrie 8 bits.
Correctif : passer `2` (et corriger le commentaire).

### 7. FAIBLE — index non borné dans `api_ram` (écriture OOB, via env)
`hw/arm/calypso/calypso_arm2dsp.c:115-116`

`s->api_ram[a2d_word - A2D_API_BASE] |= a2d_bit;` ne borne pas par le haut.
`a2d_word` vient de `CALYPSO_ARM2DSP_TASKWORD` (`strtoul`, jusqu'à 0xFFFF) ;
`api_ram` fait `C54X_API_SIZE = 0x2000` mots. Tout `a2d_word >= 0x8800` écrit
au-delà. Fonctionnalité désactivée par défaut (`CALYPSO_ARM2DSP`), défaut
`0x0fff` in-bounds. Correctif : garder
`(a2d_word - A2D_API_BASE) < C54X_API_SIZE`.

### 8. FAIBLE — lecture OOB en log debug (`dsp->data[]`)
`hw/arm/calypso/calypso_bsp.c:1073-1080, 1362-1369`

Les logs lisent `bsp.dsp->data[bsp.daram_addr + N]` (N jusqu'à 7) sans cast
`uint16_t`. `daram_addr` vient de `CALYPSO_BSP_DARAM_ADDR` (défaut 0x2a00) ;
`data[]` fait exactement 0x10000 mots. Avec `daram_addr=0xFFFE`, lecture
jusqu'à 6 mots au-delà. Correctif : `data[(uint16_t)(bsp.daram_addr + N)]`.

### 9. FAIBLE — READ_BINARY borné sur `size` et non sur le buffer
`hw/arm/calypso/calypso_sim.c:431-435`

`cmd_read_binary` valide `offset + n > f->size` puis lit `f->data + offset`
(`SimFile.data` = `uint8_t[64]`). Tous les EF actuels ont `size <= 24`, donc
pas de débordement aujourd'hui ; mais la borne est `f->size`, pas
`sizeof(f->data)`. Un EF futur avec `size > 64` (fréquent sur vraies SIM)
provoquerait une lecture OOB. Correctif : clamp additionnel sur
`sizeof(f->data)`.

### 10. FAIBLE — bloc `cfile2` dupliqué (artefact debug)
`hw/arm/calypso/calypso_dsp_shunt.c:1620-1635` et `1636-1655`

Le bloc `if (g_iq_cfile2) { … }` est copié-collé deux fois, chacun avec ses
propres `static` (`spf/base_fn/pos/have_base`) : chaque burst est écrit deux
fois dans le même fichier avec deux reconstructions de position divergentes →
sortie doublée et désynchronisée. Correctif : supprimer le second bloc.

### 11. FAIBLE — `malloc` non vérifié + `condattr` non initialisé
`tools/calypso-ipc-device/ipc_shm.c:43-50, 84-99`

`ipc_shm_init_consumer` déréférence deux `malloc` sans test NULL.
`ipc_shm_init_producer` appelle `pthread_condattr_setpshared` sans
`pthread_condattr_init` préalable (UB). Chemins de démarrage, non pilotés par
l'attaquant. Correctif : vérifier les allocations, initialiser les condattr.

### 12. FAIBLE — init paresseuse non atomique (course)
`hw/arm/calypso/calypso_full_pcb.c:95-116`

`calypso_async_log()` fait `if (!async_log_inited) async_log_init();` sans
verrou : deux threads peuvent réinitialiser mutex/cond et lancer deux threads
de drain (UB). Latent car `calypso_pcb_init()` init d'abord dans le flux normal.
Correctif : init unique via `pthread_once` ou depuis le seul chemin
mono-thread.

---

## Vérifié et jugé sûr

- **L1CTL socket** (`l1ctl_sock.c`) : parser à préfixe de longueur avec reset
  sur débordement, `sercomm_wrap` borne chaque écriture, `klen` clampé à 16.
- **Sercomm gate** (`sercomm_gate.c`) : `sc_len < GATE_BUF_SIZE`, `snprintf`
  bornés, `frame[1024]` non débordable pour `plen <= 512`.
- **Indexation MMIO** : `inth.ilr[32]` (offset 0x20..0x60, idx 0..31),
  `spi.abb_regs[256]` (addr clampé <256). Bornés.
- **TRXD/BSP réseau** : `tn = buf[0] & 0x07`, `nbits`/`copy_count` clampés,
  recv dans buffers fixes avec garde de taille minimale.
- **Chemins TRX** (`nbdi_poll_and_present`, `sbi_poll_and_present`,
  `calypso_trx_rx_burst`, `calypso_rach_publish`) : bornés.
- **APDU SIM** : `apdu[261]`, dispatch à `apdu_pos == apdu_expected`, réponses
  clampées à `sizeof(resp_buf)`.
- **Timer** : pas de division par zéro (`tick_ns >= 1` garanti avant usage).
- **IOTA/TWL3025** : indices ring `% IOTA_PENDING_MAX`, DAC clampé.
- **calypso-ipc-device** (`ipc_sock.c`, `qemu_wrap.c`) : retours `recv`/`recvfrom`
  vérifiés, tailles gardées.

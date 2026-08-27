# =============================================================================
#  19-asterisk — le PBX SIP qui porte les appels
# =============================================================================
#  RÔLE      terminaison des appels : osmo-sip-connector traduit le MNCC du MSC
#            en SIP et le pousse vers Asterisk. Sans PBX, un appel MS→MS n'est
#            jamais raccordé. Optionnel : ni le rattachement ni le SMS n'en
#            dépendent.
#  PRÉREQUIS binaire asterisk ; /etc/asterisk/asterisk.conf.
#  SUCCÈS    la console de contrôle RÉPOND (« core show uptime ») — c'est le
#            seul critère qui prouve qu'Asterisk a fini de charger ses modules,
#            là où « actif » ne prouve que le fork. Plus : aucun redémarrage.
#  JOURNAL   journalctl -u asterisk ; /var/log/asterisk/
#
#  NOTE l'ancien lancement supprimait /var/lib/asterisk/astdb.sqlite3 à chaque
#  démarrage (scripts/run.sh:455). Ce module ne le fait PAS : détruire une base
#  n'est pas une étape de démarrage.
# -----------------------------------------------------------------------------
: "${MODDIR:=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
. "$MODDIR/_lib/core.sh"

MOD_REGISTER asterisk "Cœur — Asterisk (PBX SIP)"
MOD_REQUIRED[asterisk]=0
MOD_PROFILES[asterisk]="calypso faketrx hybrid core"
MOD_JOURNAL[asterisk]="asterisk"
MOD_TIMEOUT[asterisk]=45
MOD_ENABLED_IF[asterisk]='[ "${NO_OSMO_START:-0}" != 1 ] && [ "${CORE_VOICE:-1}" = 1 ]'

: "${ASTERISK_UNIT:=asterisk}"
: "${ASTERISK_CFG:=/etc/asterisk/asterisk.conf}"

_ast_cli() { asterisk -rx "$1" 2>/dev/null; }
# PAS DE PIPELINE ICI. [2026-08-27] Cette sonde etait ecrite
#     _ast_cli "core show uptime" | grep -qi 'uptime'
# et run.sh tourne sous `set -o pipefail`. grep -q sort des la premiere
# correspondance et ferme le tuyau ; asterisk -rx prend un SIGPIPE et sort en
# 141 ; pipefail retient 141 pour tout le pipeline. Resultat : la sonde
# repondait FAUX alors meme que la console repondait. Mesure sur la VM, console
# ouverte et Asterisk pret :
#     sans pipefail                -> rc=0
#     avec pipefail (comme run.sh) -> rc=141, cinq fois sur cinq
# La barriere ne pouvait donc jamais aboutir : 45 s d'attente puis
#     console Asterisk : toujours pas pret apres 45s
# — un message qui accusait Asterisk, le service, le lanceur, tout sauf la
# sonde. On capture la sortie et on la teste, sans tuyau.
_ast_ready() {
    local out
    out="$(_ast_cli "core show uptime")" || true
    case "$out" in *[Uu]ptime*) return 0 ;; esac
    return 1
}

# =============================================================================
#  LANCEMENT DIRECT, PROPRIÉTAIRE UNIQUE  [2026-08-27]
# =============================================================================
#  Asterisk est lancé ICI, en exécutable. Il n'est PAS un service.
#
#  CE QUI CASSAIT. Le paquet installe asterisk.service `enabled` : systemd en
#  démarrait un au boot pendant que ce module lançait le sien. Deux Asterisk
#  pour un seul /etc/asterisk et une seule socket /var/run/asterisk/asterisk.ctl.
#  Relevé sur la VM, au même instant :
#      systemctl is-active asterisk    -> inactive
#      asterisk -rx "core show uptime" -> répond
#  Les deux sondes désignaient deux processus différents, et le journal du module
#  empilait les « lancement direct » run après run. La barrière finissait sur
#      console Asterisk : toujours pas prêt après 45s
#  — un message qui accuse le temps de chargement alors que la faute est à la
#  propriété du processus.
#
#  Mesure de référence, systemd écarté et aucun rescapé en vie : console prête
#  en UNE SECONDE. La lenteur n'a jamais été le problème.
# -----------------------------------------------------------------------------

ASTERISK_RUNDIR="/var/run/asterisk"

mod_asterisk_check() {
    command -v asterisk >/dev/null 2>&1 || {
        mod_hint "installez asterisk, ou désactivez la voix : CORE_VOICE=0"
        mod_fail "binaire asterisk introuvable"; return $MOD_RC_FAIL; }
    [ -r "$ASTERISK_CFG" ] || {
        mod_fail "configuration illisible : $ASTERISK_CFG"; return $MOD_RC_FAIL; }
    mod_ok
}

mod_asterisk_status() { _ast_ready; }

mod_asterisk_start() {
    # 1. systemd dégage. `disable --now`, pas seulement `stop` : sans le disable,
    #    l'unité revient au prochain boot et la collision avec elle.
    if core_unit_exists "$ASTERISK_UNIT"; then
        core_unit_active "$ASTERISK_UNIT" && \
            mod_say "systemd tenait Asterisk — on le lui retire"
        systemctl disable --now "$ASTERISK_UNIT" >/dev/null 2>&1 || true
    fi

    # 2. Plus aucun Asterisk en vie : tant qu'il en reste un, il garde
    #    asterisk.ctl et le nôtre ne pourra pas l'ouvrir.
    if pgrep -x asterisk >/dev/null 2>&1; then
        mod_say "Asterisk déjà en vie — on repart d'une machine propre"
        pkill -x asterisk 2>/dev/null
        local i
        for i in 1 2 3 4 5; do pgrep -x asterisk >/dev/null 2>&1 || break; sleep 1; done
        pkill -9 -x asterisk 2>/dev/null
        sleep 1
    fi

    # 3. Le répertoire d'exécution. /var/run est un tmpfs : après un boot où
    #    l'unité n'a jamais tourné, il N'EXISTE PAS. Asterisk descend vers
    #    l'utilisateur asterisk (-U) AVANT d'ouvrir sa socket : sans répertoire
    #    accessible, il tourne et reste muet pour toujours. C'est le mode
    #    d'échec que le passage au lancement direct a introduit sans le voir —
    #    au début l'unité tournait encore au boot et créait le répertoire.
    install -d -o asterisk -g asterisk -m 0755 "$ASTERISK_RUNDIR" 2>/dev/null \
        || mkdir -p "$ASTERISK_RUNDIR" 2>/dev/null || true
    # Socket orpheline laissée par un kill -9 : `asterisk -rx` s'y connecte et
    # échoue sans jamais dire qu'elle ne mène plus nulle part.
    rm -f "$ASTERISK_RUNDIR/asterisk.ctl" 2>/dev/null || true

    local bin log pf
    bin="$(command -v asterisk)"
    pf="$(core_pidfile "$ASTERISK_UNIT")"
    log="${LOG_DIR}/${ASTERISK_UNIT}.log"
    mkdir -p "$RUN_DIR" "$LOG_DIR" 2>/dev/null || true

    # -f : premier plan, pas de double fork. -U : descend vers l'utilisateur
    # asterisk. Ni -p (temps réel, refusé sans les capacités) ni -g (dump core).
    mod_say "lancement direct : $bin -f -U asterisk"
    setsid "$bin" -f -U asterisk >>"$log" 2>&1 </dev/null &

    # LE PID : celui qu'Asterisk écrit lui-même, PAS $!. setsid se dédouble et
    # rend la main aussitôt, donc $! désigne un processus déjà mort — le module
    # se croyait devant un Asterisk défunt une seconde après l'avoir lancé.
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -s "$ASTERISK_RUNDIR/asterisk.pid" ] && break
        sleep 1
    done
    if [ -s "$ASTERISK_RUNDIR/asterisk.pid" ]; then
        cp "$ASTERISK_RUNDIR/asterisk.pid" "$pf"
    else
        pgrep -x asterisk | head -1 > "$pf"
    fi

    if ! kill -0 "$(cat "$pf" 2>/dev/null)" 2>/dev/null; then
        mod_hint "tail -30 $log"
        mod_fail "asterisk n'a pas démarré"
        return $MOD_RC_FAIL
    fi
    mod_ok
}

# BARRIÈRE — la socket de contrôle existe avant qu'Asterisk ne réponde : on
# interroge la CLI, pas le système de fichiers.
mod_asterisk_wait() {
    local pf; pf="$(core_pidfile "$ASTERISK_UNIT")"
    if ! wait_until "${MOD_TIMEOUT[asterisk]}" "console Asterisk" _ast_ready; then
        if kill -0 "$(cat "$pf" 2>/dev/null)" 2>/dev/null; then
            mod_hint "Asterisk tourne (PID $(cat "$pf")) mais n'ouvre pas sa console : vérifiez $ASTERISK_RUNDIR (propriétaire asterisk), puis tail -50 ${LOG_DIR}/${ASTERISK_UNIT}.log"
        else
            mod_hint "tail -50 ${LOG_DIR}/${ASTERISK_UNIT}.log"
        fi
        return $MOD_RC_FAIL
    fi
    # Personne ne relance Asterisk : une mort est définitive et se voit au PID.
    # NRestarts, la sonde systemd, n'a plus de sens ici.
    if ! kill -0 "$(cat "$pf" 2>/dev/null)" 2>/dev/null; then
        mod_hint "tail -50 ${LOG_DIR}/${ASTERISK_UNIT}.log"
        mod_fail "le PID lancé n'existe plus : Asterisk est mort ou a été remplacé"
        return $MOD_RC_FAIL
    fi
    mod_ok
}

mod_asterisk_stop() {
    _ast_cli "core stop now" >/dev/null 2>&1
    core_svc_stop "$ASTERISK_UNIT" ""
}

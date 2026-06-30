# harvard.sh -- shell sh sur le DSP TMS320C54x VIVANT (Calypso/QEMU).
#   data + registres = LUS EN LIVE via gdb -p sur le process qemu.
#   programme (dis/pp) = ROM statique (immuable).
# Lancer:  bash --rcfile /tmp/harvard.sh -i     (ou:  source /tmp/harvard.sh)
B=/opt/GSM

_qpid(){ pgrep -f "qemu-src/build/qemu-system-arm" | head -1; }
_gdb(){ local p; p=$(_qpid); [ -z "$p" ] && { echo "(DSP eteint - qemu introuvable)"; return 1; }
        gdb -p "$p" -batch -ex "set pagination off" "$@" -ex detach -ex quit 2>/dev/null; }

# --- identite / etat live ---
uname(){ echo "C54x-DSP calypso-lead ROM-3606 Harvard tms320c54x 16le  (LIVE pid=$(_qpid))"; }

id(){ _gdb -ex 'printf "uid=0(l1) gid=0(dsp) groups=1(fb),2(sb),3(afc),12(sched-vec28)\n"' \
           -ex 'printf "  PC=0x%04x  INTM=%d  IMR=0x%04x  IFR=0x%04x  SP=0x%04x  insn=%llu\n", bsp.dsp->pc,(bsp.dsp->st1>>11)&1,bsp.dsp->imr,bsp.dsp->ifr,bsp.dsp->sp,bsp.dsp->insn_count' \
           -ex 'printf "  state: %s\n", bsp.dsp->pc==0xa6b8 ? "IDLE(0xa6b8) -- attend commande #2" : "running"' \
        | grep -E "uid=|PC=|state:"; }

reg(){ _gdb -ex 'printf "PC=0x%04x IMR=0x%04x IFR=0x%04x ST0=0x%04x ST1=0x%04x T=0x%04x SP=0x%04x PMST=0x%04x insn=%llu\n", bsp.dsp->pc,bsp.dsp->imr,bsp.dsp->ifr,bsp.dsp->st0,bsp.dsp->st1,bsp.dsp->t,bsp.dsp->sp,bsp.dsp->pmst,bsp.dsp->insn_count' \
           -ex 'printf "AR = %04x %04x %04x %04x %04x %04x %04x %04x\n", bsp.dsp->ar[0],bsp.dsp->ar[1],bsp.dsp->ar[2],bsp.dsp->ar[3],bsp.dsp->ar[4],bsp.dsp->ar[5],bsp.dsp->ar[6],bsp.dsp->ar[7]' \
        | grep -E "PC=|AR ="; }

# ps / top : l'etat du "process" dsp.exe
ps(){ _gdb -ex 'printf "  PID  PC      STATE                  INSN\n  dsp  0x%04x  %-20s   %llu\n", bsp.dsp->pc, bsp.dsp->pc==0xa6b8?"IDLE go-live":"R running", bsp.dsp->insn_count' | grep -E "PID|dsp "; }
top(){ ps; }

# --- espace DONNEES (live) ---  x <addr> [n] : hexdump live
x(){ local a=$1 n=${2:-8}; _gdb -ex "printf \"D:%04x  \", $a" -ex "p/x bsp.dsp->data[$a]@$n" \
       | grep -E "D:|=" | tr '\n' ' ' | sed -E 's/.*D:/D:/; s/\$[0-9]+ = \{//; s/\}.*//; s/,//g; s/0x//g'; echo "  (LIVE)"; }
d(){ x "$@"; }
api(){ x "$@"; }

# une cellule nommee precise
cell(){ _gdb -ex "printf \"$1 = 0x%04x  (LIVE)\\n\", bsp.dsp->data[$2]" | grep " = 0x"; }

# vecteurs live @0x80
vec(){ for v in $(seq 16 31); do cell "vec$v" $((0x80 + v*4)); done; }

# --- espace PROGRAMME (ROM statique, immuable) ---
dis(){ python3 /tmp/hsh.py u "$@"; }
u(){ python3 /tmp/hsh.py u "$@"; }
pp(){ python3 /tmp/hsh.py p "$@"; }

# --- ls : carte memoire facon FS ---
ls(){ cat <<'MAP'
prog/   PROGRAMME (ROM, bus prog)   PROM0 7000-DFFF | PROM1 8000-FFFF
data/   DONNEES   (RAM, bus data)   DARAM 0-3FFF | api_ram 0800-27FF
  d_idle_gate   0x3fde      ring_C  0x434e/f
  golive_state  0x3f70      mbox    0x0fff
  vectors       0x0080-00FF api     0x08de/0c36/0c37
mmr/    registres (IMR IFR ST1 SP PMST ...)
-- cmds: uname id reg ps x<a>[n] cell<nom><a> vec u<a>[n] pp<a>[n] dsp-help
MAP
}

dsp-help(){ cat <<'H'
harvard-sh (sh) -- DSP C54x VIVANT
  uname           identite cible
  id              contexte live (INTM/IMR/IFR/state)
  reg             tous les registres MMR (live)
  ps / top        etat du process dsp.exe (PC/insn)
  x <a> [n]       hexdump DONNEES live      ex: x 0x3fde 4
  d / api         alias de x
  cell <nom> <a>  une cellule nommee        ex: cell ring 0x434e
  vec             table des vecteurs (live)
  u / dis <a> [n] desassembler PROGRAMME    ex: u 0xa6b8 5
  pp <a> [n]      lire PROGRAMME (hex)
  ls              carte memoire
LIVE = lu par gdb sur le process qemu (pause ~0.2s, detache propre).
H
}

# --- TRADUCTION : mobile -> L1(ARM) -> task-cells DSP (la commande traduite) ---
task(){ _gdb -ex 'printf "d_dsp_page = 0x%04x   B_GSM_TASK=%d\n", bsp.dsp->data[0x08d4], (bsp.dsp->data[0x08d4]>>1)&1' \
             -ex 'printf "d_task_d   = 0x%04x   d_task_u = 0x%04x   d_task_md = 0x%04x\n", bsp.dsp->data[0x0586], bsp.dsp->data[0x0588], bsp.dsp->data[0x058a]' \
             -ex 'printf "mbox_cmd   = 0x%04x   PC=0x%04x %s\n", bsp.dsp->data[0x0fff], bsp.dsp->pc, bsp.dsp->pc==0xa6b8?"(IDLE: ignore la tache)":"(run)"' \
          | grep -E "^d_|^mbox"; }
xlate(){ task; }

# --- CAMP : la procedure d'accroche cellule de bout en bout (env -> dsp.exe -> mobile) ---
camp(){
  local pc fb toa sch page iq insn
  read pc fb toa sch page iq insn < <(_gdb -ex 'printf "%x %x %x %x %x %x%x%x%x %llu\n", bsp.dsp->pc, bsp.dsp->data[0x08f8], bsp.dsp->data[0x08fa], bsp.dsp->data[0x006e], bsp.dsp->data[0x08d4], bsp.dsp->data[0x2a00], bsp.dsp->data[0x2a01], bsp.dsp->data[0x2a02], bsp.dsp->data[0x2a03], bsp.dsp->insn_count' | grep -E "^[0-9a-f]+ ")
  echo "== CAMP : env -> dsp.exe -> mobile (LIVE) =="
  echo "[1 env]     BTS/trx    : $(pgrep -c osmo-trx-ipc 2>/dev/null) proc trx actifs"
  echo "[2 radio]   burst->DSP : data[0x2a00]=$iq   $([ "$iq" = 0000 ] && echo '** VIDE : aucun burst ne descend **' || echo 'I/Q present')"
  echo "[3 dsp.exe] FB detect  : d_fb_det=0x$fb (TOA=0x$toa)   $([ "$fb" = 0 ] && echo '** pas de FCCH **')"
  echo "[4 dsp.exe] SB sync    : a_sch_crc=0x$sch   $([ "$sch" = 0 ] && echo '** pas de BSIC **')"
  echo "[5 dsp.exe] etat       : PC=0x$pc page=0x$page insn=$insn   $([ "$pc" = a6b8 ] && echo '<- IDLE : la tache postee est IGNOREE')"
  echo "[6 mobile]  camp       : pas de SI decode -> MS ne campe pas"
  echo "== STOP a [3] : dsp.exe coince a l'IDLE -> jamais de FB -> chaine camp cassee (= la racine de la nuit) =="
}

# --- STACK : la pile MS entiere, du haut (mobile) au bas (radio BTS) ---
stack(){
  echo "== MS stack (haut -> bas) =="
  echo "[L3]   mobile     : VTY 127.0.0.1:4247   ($(pgrep -c mobile 2>/dev/null) proc actifs)"
  echo "[xlate] L1->DSP   : commande traduite (LIVE) :"
  task | sed 's/^/           /'
  echo "[L1]   dsp.exe    :"
  id | grep -E "PC=|state:" | sed 's/^/           /'
  echo "[RF]   radio BTS  : burst -> correlateur DSP data[0x2a00] (LIVE) :"
  x 0x2a00 4 | sed 's/^/           /'
}

PS1='c54x(live):\w$ '
echo "== harvard-sh :: TMS320C54x VIVANT (data via gdb) =="
echo "   cmds: id reg ps x<a> u<a> task xlate stack  | 'dsp-help' pour tout."

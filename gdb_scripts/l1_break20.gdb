# ============================================================================
# l1_break20.gdb -- trace le DISPATCH DSP cote ARM L1 (Frontiere A, root bootstrap).
# But : voir OU la chaine de dispatch FB casse, et si l'ARM atteint jamais
#       dsp_end_scenario (le write d_dsp_page = B_GSM_TASK = le "go" au DSP).
#
# Usage (gdb-multiarch attache a :1234 avec layer1.highram.elf) :
#   (gdb) source /opt/GSM/qemu-src/gdb_scripts/l1_break20.gdb
#   (gdb) continue          # laisse tourner
#   ... attends ~20s puis Ctrl-C ...
#   (gdb) dispatch_report
# ============================================================================
set pagination off
set confirm off

# commandes diag L1 (l1state/dsp_api/fn...) si dispo
source /tmp/abc.gdb

# compteurs de la chaine de dispatch
set $n_fbsb=0
set $n_fbdet=0
set $n_loadrx=0
set $n_endscn=0
set $n_fbresp=0

# --- tracepoints silencieux : chaque etape auto-continue ---
break l1s_fbsb_req
commands
  silent
  set $n_fbsb=$n_fbsb+1
  printf ">>  l1s_fbsb_req   #%d  (ARM demande FB/SB search)\n", $n_fbsb
  continue
end

break l1s_fbdet_cmd
commands
  silent
  set $n_fbdet=$n_fbdet+1
  printf ">>  l1s_fbdet_cmd  #%d  (charge la tache FB pour le DSP)\n", $n_fbdet
  continue
end

break dsp_load_rx_task
commands
  silent
  set $n_loadrx=$n_loadrx+1
  continue
end

break dsp_end_scenario
commands
  silent
  set $n_endscn=$n_endscn+1
  printf ">>> dsp_end_scenario #%d  == ECRIT d_dsp_page (B_GSM_TASK) = LE GO AU DSP !\n", $n_endscn
  continue
end

break l1s_fbdet_resp
commands
  silent
  set $n_fbresp=$n_fbresp+1
  printf ">>  l1s_fbdet_resp #%d  (lit le resultat du DSP)\n", $n_fbresp
  continue
end

define dispatch_report
  printf "\n===== DISPATCH DSP cote ARM L1 =====\n"
  printf "  l1s_fbsb_req     : %d   (ARM demande FB search)\n", $n_fbsb
  printf "  l1s_fbdet_cmd    : %d   (dispatch tache FB au DSP)\n", $n_fbdet
  printf "  dsp_load_rx_task : %d\n", $n_loadrx
  printf "  dsp_end_scenario : %d   <-- SI 0 : l'ARM ne poste JAMAIS la tache = ROOT bootstrap\n", $n_endscn
  printf "  l1s_fbdet_resp   : %d   (lit le resultat DSP)\n", $n_fbresp
  printf "Interpretation : la 1ere etape a 0 = la ou la chaine casse.\n"
end
document dispatch_report
Compteurs de la chaine de dispatch DSP cote ARM (localise ou ca casse).
end

printf "[l1_break20] tracepoints armes sur la chaine dispatch DSP.\n"
printf "  -> tape:  continue   (laisse ~20s)   puis Ctrl-C   puis  dispatch_report\n"

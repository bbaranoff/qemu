# Diagnostic : dsp_end_scenario (ecrit d_dsp_page) est-il appele ?
# Adresses ARM (layer1.highram.elf) : dsp_end_scenario=0x82c0dc,
# l1s_fbdet_cmd=0x8262cc, tdma_sched_flag_scan=0x828eac
# Usage : arm-none-eabi-gdb (ou gdb-multiarch) puis: source ce fichier
set pagination off
set confirm off
target remote :1234

# compteurs
set $n_scenario = 0
set $n_fbdet = 0
set $n_flagscan = 0

break *0x0082c0dc
commands
  set $n_scenario = $n_scenario + 1
  printf "[GDB] dsp_end_scenario #%d appele ! (r0=0x%x)\n", $n_scenario, $r0
  continue
end

break *0x008262cc
commands
  set $n_fbdet = $n_fbdet + 1
  if $n_fbdet <= 5
    printf "[GDB] l1s_fbdet_cmd #%d (FB task tourne)\n", $n_fbdet
  end
  continue
end

# rapport apres N secondes : Ctrl-C puis: print-report
define print-report
  printf "=== RAPPORT ===\n"
  printf "dsp_end_scenario appele : %d fois\n", $n_scenario
  printf "l1s_fbdet_cmd (FB task) : %d fois\n", $n_fbdet
end
document print-report
Affiche les compteurs (dsp_end_scenario vs FB task)
end

printf "[GDB] breakpoints poses. continue... (Ctrl-C puis print-report pour le bilan)\n"
continue

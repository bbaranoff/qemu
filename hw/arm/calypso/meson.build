# Calypso SoC and machine files
#
# This directory contains the Calypso machine and SoC implementation
# for QEMU ARM emulation.
#
# Organization:
#   - calypso_mb.c    : Machine (board) definition
#   - calypso_soc.c   : SoC device with integrated peripherals
#   - calypso_trx.c   : DSP/TPU/TRX bridge for GSM simulation
#
# Peripheral devices (UART, Timer, SPI, INTH) are in their respective
# directories (hw/char/, hw/timer/, hw/ssi/, hw/intc/).

arm_ss.add(when: 'CONFIG_CALYPSO', if_true: files(
  'calypso_mb.c',
  'calypso_soc.c',
  'calypso_trx.c',
))

# Optional: include old monolithic machine for migration period
# Uncomment to build both old and new machines side-by-side
# arm_softmmu_ss.add(when: 'CONFIG_CALYPSO', if_true: files(
#   'calypso_high.c',
# ))

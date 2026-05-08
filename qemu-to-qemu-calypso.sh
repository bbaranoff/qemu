mkdir -p ~/qemu-calypso


# ── Calypso hardware (le coeur) ──
cp -r hw/arm/calypso/              ~/qemu-calypso/hw/arm/calypso/
cp -r include/hw/arm/calypso/      ~/qemu-calypso/include/hw/arm/calypso/
cp hw/char/calypso_uart.c          ~/qemu-calypso/hw/char/
cp hw/intc/calypso_inth.c          ~/qemu-calypso/hw/intc/
cp hw/ssi/calypso_i2c.c            ~/qemu-calypso/hw/ssi/
cp hw/ssi/calypso_spi.c            ~/qemu-calypso/hw/ssi/
cp hw/timer/calypso_timer.c        ~/qemu-calypso/hw/timer/

# ── Build system (diffs vs stock) ──
cp configs/devices/arm-softmmu/default.mak  ~/qemu-calypso/configs/devices/arm-softmmu/
cp hw/arm/Kconfig                  ~/qemu-calypso/hw/arm/
cp hw/arm/meson.build              ~/qemu-calypso/hw/arm/
cp hw/char/meson.build             ~/qemu-calypso/hw/char/
cp hw/intc/meson.build             ~/qemu-calypso/hw/intc/
cp hw/ssi/meson.build              ~/qemu-calypso/hw/ssi/
cp hw/timer/meson.build            ~/qemu-calypso/hw/timer/

# ── Bridge + scripts ──
cp l1ctl_bridge.py                 ~/qemu-calypso/
cp sercomm_relay.py                ~/qemu-calypso/
cp l1ctl_passthrough.py            ~/qemu-calypso/
cp run_calypso.sh                  ~/qemu-calypso/

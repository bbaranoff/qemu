#!/bin/bash
# cleanup_duplicates.sh — Remove obsolete calypso/calypso/ subdirectory
# and duplicate headers in include/hw/arm/calypso/calypso/
#
# The active code is in:
#   hw/arm/calypso/*.c          (built by hw/arm/calypso/meson.build)
#   include/hw/arm/calypso/*.h  (included by active code)
#
# The duplicates are in:
#   hw/arm/calypso/calypso/     (old copies, different content, never built)
#   include/hw/arm/calypso/calypso/  (duplicate headers)

set -e

QEMU_SRC="${1:-/opt/GSM/qemu-src}"

echo "=== Removing duplicate hw/arm/calypso/calypso/ ==="
DUPS=(
    "$QEMU_SRC/hw/arm/calypso/calypso/calypso_mb.c"
    "$QEMU_SRC/hw/arm/calypso/calypso/calypso_soc.c"
    "$QEMU_SRC/hw/arm/calypso/calypso/calypso_trx.c"
    "$QEMU_SRC/hw/arm/calypso/calypso/l1ctl_sock.c"
    "$QEMU_SRC/hw/arm/calypso/calypso/meson.build"
    "$QEMU_SRC/hw/arm/calypso/calypso/Kconfig"
)

for f in "${DUPS[@]}"; do
    if [ -f "$f" ]; then
        echo "  rm $f"
        rm -f "$f"
    fi
done

# Remove the empty directory
if [ -d "$QEMU_SRC/hw/arm/calypso/calypso" ]; then
    rmdir "$QEMU_SRC/hw/arm/calypso/calypso" 2>/dev/null || \
        echo "  WARNING: calypso/calypso/ not empty after cleanup"
fi

echo "=== Removing duplicate include/hw/arm/calypso/calypso/ ==="
HDR_DUPS=(
    "$QEMU_SRC/include/hw/arm/calypso/calypso/calypso_inth.h"
    "$QEMU_SRC/include/hw/arm/calypso/calypso/calypso_soc.h"
    "$QEMU_SRC/include/hw/arm/calypso/calypso/calypso_spi.h"
    "$QEMU_SRC/include/hw/arm/calypso/calypso/calypso_timer.h"
    "$QEMU_SRC/include/hw/arm/calypso/calypso/calypso_trx.h"
    "$QEMU_SRC/include/hw/arm/calypso/calypso/calypso_uart.h"
)

for f in "${HDR_DUPS[@]}"; do
    if [ -f "$f" ]; then
        echo "  rm $f"
        rm -f "$f"
    fi
done

if [ -d "$QEMU_SRC/include/hw/arm/calypso/calypso" ]; then
    rmdir "$QEMU_SRC/include/hw/arm/calypso/calypso" 2>/dev/null || \
        echo "  WARNING: include calypso/calypso/ not empty after cleanup"
fi

echo "=== Also removing duplicate Kconfig in hw/arm/calypso/ ==="
# The real Kconfig is hw/arm/Kconfig which already has CONFIG_CALYPSO
# hw/arm/calypso/Kconfig redefines it (harmless but confusing)
if [ -f "$QEMU_SRC/hw/arm/calypso/Kconfig" ]; then
    echo "  rm $QEMU_SRC/hw/arm/calypso/Kconfig"
    rm -f "$QEMU_SRC/hw/arm/calypso/Kconfig"
fi

echo "=== Done ==="
echo ""
echo "Active files:"
echo "  hw/arm/calypso/calypso_mb.c"
echo "  hw/arm/calypso/calypso_soc.c"
echo "  hw/arm/calypso/calypso_trx.c"
echo "  hw/arm/calypso/calypso_c54x.c"
echo "  hw/arm/calypso/l1ctl_sock.c"
echo "  hw/arm/calypso/meson.build"
echo ""
echo "Active headers:"
echo "  include/hw/arm/calypso/calypso_inth.h"
echo "  include/hw/arm/calypso/calypso_soc.h"
echo "  include/hw/arm/calypso/calypso_spi.h"
echo "  include/hw/arm/calypso/calypso_timer.h"
echo "  include/hw/arm/calypso/calypso_trx.h"
echo "  include/hw/arm/calypso/calypso_uart.h"

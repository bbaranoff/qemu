#!/bin/bash
# Sync qemu-src between host and osmo-operator-1 container
# Usage: ./sync.sh [push|pull|check]
#   pull  = container → host (default)
#   push  = host → container
#   check = compare only

CONTAINER="trying"
HOST_DIR="/home/nirvana/qemu-src"
CONT_DIR="/opt/GSM/qemu-src"

# C/H files live in qemu-src
FILES=(
  hw/arm/calypso/calypso_trx.c
  hw/arm/calypso/calypso_c54x.c
  hw/arm/calypso/calypso_c54x.h
  hw/arm/calypso/calypso_tdma_hw.c
  hw/arm/calypso/calypso_tdma_hw.h
  hw/arm/calypso/calypso_soc.c
  hw/arm/calypso/calypso_mb.c
  hw/arm/calypso/l1ctl_sock.c
  hw/arm/calypso/meson.build
  hw/char/calypso_uart.c
  hw/intc/calypso_inth.c
  include/hw/arm/calypso/calypso_trx.h
  include/hw/arm/calypso/calypso_soc.h
  include/hw/arm/calypso/calypso_uart.h
)

# Scripts live in /opt/GSM (not qemu-src)
GSM_DIR="/opt/GSM"
GSM_FILES=(
  bridge.py
  run.sh
)

MODE="${1:-pull}"
DIRTY=0

sync_file() {
  local f="$1" host_path="$2" cont_path="$3"
  local h=$(md5sum "$host_path" 2>/dev/null | cut -d' ' -f1)
  local c=$(docker exec "$CONTAINER" md5sum "$cont_path" 2>/dev/null | cut -d' ' -f1)

  if [ -z "$h" ] && [ -z "$c" ]; then
    return
  elif [ "$h" = "$c" ]; then
    echo "✅ $f"
  else
    DIRTY=1
    if [ "$MODE" = "check" ]; then
      echo "❌ $f  host=${h:-MISSING} cont=${c:-MISSING}"
    elif [ "$MODE" = "pull" ]; then
      docker cp "$CONTAINER:$cont_path" "$host_path"
      echo "⬇️  $f  (pulled)"
    elif [ "$MODE" = "push" ]; then
      docker cp "$host_path" "$CONTAINER:$cont_path"
      echo "⬆️  $f  (pushed)"
    fi
  fi
}

# C/H sources in qemu-src
for f in "${FILES[@]}"; do
  sync_file "$f" "$HOST_DIR/$f" "$CONT_DIR/$f"
done

# Scripts in /opt/GSM
for f in "${GSM_FILES[@]}"; do
  sync_file "$f" "$HOST_DIR/$f" "$GSM_DIR/$f"
done

if [ "$DIRTY" = "0" ]; then
  echo "All files in sync."
elif [ "$MODE" = "check" ]; then
  echo "Files out of sync. Use: ./sync.sh pull|push"
fi

# Snapshot on push
if [ "$MODE" = "push" ] && [ "$DIRTY" = "1" ]; then
  STAMP=$(date +%Y%m%d-%H%M%S)
  EVENT="${2:-}"
  if [ -n "$EVENT" ]; then
    SNAP="/home/nirvana/ALL-QEMUs/qemu-calypso-${STAMP}-${EVENT}"
  else
    SNAP="/home/nirvana/ALL-QEMUs/qemu-calypso-${STAMP}"
  fi
  mkdir -p "$SNAP"
  for f in "${FILES[@]}" "${GSM_FILES[@]}"; do
    mkdir -p "$SNAP/$(dirname $f)"
    cp "$HOST_DIR/$f" "$SNAP/$f" 2>/dev/null
  done
  echo "$STAMP ${EVENT:-push}" >> "/home/nirvana/ALL-QEMUs/HISTORY.log"
  echo "📸 Snapshot: $SNAP"
fi

# Check binary age vs source files
BINARY="$CONT_DIR/build/qemu-system-arm"
BIN_TS=$(docker exec "$CONTAINER" stat -c %Y "$BINARY" 2>/dev/null)
if [ -n "$BIN_TS" ]; then
  BIN_DATE=$(docker exec "$CONTAINER" date -d "@$BIN_TS" "+%H:%M:%S" 2>/dev/null)
  STALE=0
  for f in "${FILES[@]}"; do
    [[ "$f" == *.py || "$f" == *.sh ]] && continue
    SRC_TS=$(docker exec "$CONTAINER" stat -c %Y "$CONT_DIR/$f" 2>/dev/null)
    if [ -n "$SRC_TS" ] && [ "$SRC_TS" -gt "$BIN_TS" ]; then
      SRC_DATE=$(docker exec "$CONTAINER" date -d "@$SRC_TS" "+%H:%M:%S" 2>/dev/null)
      echo "⚠️  STALE: $f ($SRC_DATE) newer than binary ($BIN_DATE)"
      STALE=1
    fi
  done
  if [ "$STALE" = "0" ]; then
    echo "🔨 Binary up to date ($BIN_DATE)"
  else
    echo "🔨 Binary OUTDATED ($BIN_DATE) — run: ninja -C build"
  fi
else
  echo "⚠️  Binary not found at $BINARY"
fi

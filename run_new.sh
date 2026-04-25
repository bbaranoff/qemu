#!/bin/bash
# run_new.sh — Calypso QEMU pipeline with osmocon as the mobile↔UART bridge.
#
# Replaces the old INJECT hack (l1ctl_sock.c:calypso_uart_receive direct call)
# with the real osmocom-bb path:
#
#   mobile  ──► /tmp/osmocom_l2 (Unix sock)  ──► osmocon ──► /dev/pts/N (PTY)
#                                                                │
#                                                                ▼
#                                                       QEMU UART modem (chardev)
#                                                                │
#                                                                ▼
#                                                       firmware Calypso (sercomm)
#
# IMPORTANT — KNOWN GAP (TODO ROMLOAD STUB):
#   On real HW osmocon first downloads the firmware over the Calypso bootloader
#   (romload protocol: <i / >i, <w / >w, <c / >c, <b / >b). QEMU jumps straight
#   to the firmware via -kernel, so the bootloader is never running and the
#   handshake fails. Two ways out:
#     (1) implement a romloader stub inside QEMU's calypso_uart.c that ACKs the
#         handshake and silently switches to "running" (preferred, fidèle au HW)
#     (2) patch osmocon to skip the download when its -p points at a QEMU PTY
#   Until either is done, osmocon will loop on "Waiting for handshake".
#   Mobile commands won't reach the firmware. The DSP / BSP / bridge / BTS half
#   of the pipeline still runs (FB-det, IOTA pulses, BSP DMA, TDMA tick).

set -euo pipefail

SESSION="calypso"
FW_ELF="/opt/GSM/firmware/board/compal_e88/layer1.highram.elf"
FW_BIN="/opt/GSM/firmware/board/compal_e88/layer1.highram.bin"
QEMU="/opt/GSM/qemu-src/build/qemu-system-arm"
BRIDGE="/opt/GSM/qemu-src/bridge.py"
OSMOCON="/opt/GSM/osmocom-bb/src/host/osmocon/osmocon"
BTS_CFG="/etc/osmocom/osmo-bts-trx.cfg"
MOBILE_CFG="/root/.osmocom/bb/mobile_group1.cfg"

QEMU_LOG="/root/qemu.log"
BRIDGE_LOG="/tmp/bridge.log"
OSMOCON_LOG="/tmp/osmocon.log"
MON_SOCK="/tmp/qemu-calypso-mon.sock"
L1CTL_SOCK="/tmp/osmocom_l2"          # owned by osmocon now
QEMU_DUMMY_SOCK="/tmp/qemu_l1ctl_disabled"  # parking spot for QEMU's old listener

# ---------- cleanup ----------
tmux kill-session -t $SESSION 2>/dev/null || true
killall -9 qemu-system-arm osmo-bts-trx mobile osmocon 2>/dev/null || true
pkill -9 -f bridge.py 2>/dev/null || true
rm -f "$L1CTL_SOCK" "$MON_SOCK" "$QEMU_DUMMY_SOCK" /tmp/osmocom_l2_*
sleep 1

/etc/osmocom/status.sh stop 2>/dev/null || true
/etc/osmocom/osmo-start.sh 2>/dev/null || true

tmux new-session -d -s $SESSION -n qemu

# ---------- 1. QEMU ----------
# L1CTL_SOCK env var moves QEMU's vestigial l1ctl_sock listener off
# /tmp/osmocom_l2 so osmocon can create it.
L1CTL_SOCK="$QEMU_DUMMY_SOCK" \
CALYPSO_DSP_ROM=/opt/GSM/calypso_dsp.txt \
"$QEMU" -M calypso -cpu arm946 \
    -serial pty -serial pty \
    -monitor "unix:${MON_SOCK},server,nowait" \
    -kernel "$FW_ELF" \
    > "$QEMU_LOG" 2>&1 &
QEMU_PID=$!
tmux send-keys -t $SESSION:qemu "tail -f $QEMU_LOG" C-m

echo -n "Waiting for QEMU PTY allocation..."
PTY_MODEM=""
for i in $(seq 1 30); do
    if grep -q 'redirected to /dev/pts/.* (label serial0)' "$QEMU_LOG" 2>/dev/null; then
        PTY_MODEM=$(grep 'redirected to /dev/pts/.* (label serial0)' "$QEMU_LOG" \
                    | sed -E 's/.*redirected to (\/dev\/pts\/[0-9]+).*/\1/' | head -1)
        break
    fi
    sleep 1; echo -n "."
done
if [ -z "$PTY_MODEM" ]; then
    echo " TIMEOUT — no PTY in $QEMU_LOG"
    exit 1
fi
echo " OK ($PTY_MODEM, QEMU_PID=$QEMU_PID)"

# ---------- 2. osmocon (mobile ↔ UART bridge) ----------
# -m romload : skip Compal e88 bootloader handshake (PROMPT1/PROMPT2). Goes
#              straight to the Calypso romload protocol, which our QEMU
#              stub in calypso_uart.c (`romload_stub_eat`) ACKs.
# -p PTY     : serial dev = QEMU's UART modem chardev
# -s SOCK    : Unix socket served to mobile (default path it expects)
# -i 100     : send <i ident every 100ms (default 5s is too slow for boot)
tmux new-window -t $SESSION -n osmocon
tmux send-keys -t $SESSION:osmocon \
    "$OSMOCON -m romload -i 100 -p $PTY_MODEM -s $L1CTL_SOCK $FW_BIN 2>&1 | tee $OSMOCON_LOG" C-m

echo -n "Waiting for osmocon to expose $L1CTL_SOCK..."
for i in $(seq 1 30); do
    [ -S "$L1CTL_SOCK" ] && break
    sleep 1; echo -n "."
done
if [ -S "$L1CTL_SOCK" ]; then
    echo " OK"
else
    echo " WARN — socket missing (osmocon may still be in romload handshake)"
fi

# ---------- 3. bridge.py ----------
tmux new-window -t $SESSION -n bridge
tmux send-keys -t $SESSION:bridge \
    "python3 $BRIDGE 2>&1 | tee $BRIDGE_LOG" C-m

echo -n "Waiting for bridge to receive QEMU ticks..."
for i in $(seq 1 30); do
    grep -q "QEMU tick" "$BRIDGE_LOG" 2>/dev/null && break
    sleep 1; echo -n "."
done
if grep -q "QEMU tick" "$BRIDGE_LOG" 2>/dev/null; then
    echo " OK"
else
    echo " TIMEOUT"
fi

# ---------- 4. osmo-bts-trx ----------
tmux new-window -t $SESSION -n bts
tmux send-keys -t $SESSION:bts "osmo-bts-trx -c $BTS_CFG" C-m
sleep 2

# ---------- 5. mobile ----------
tmux new-window -t $SESSION -n mobile
tmux send-keys -t $SESSION:mobile \
    "sleep 3 && mobile -c $MOBILE_CFG" C-m

# ---------- shell + attach ----------
tmux new-window -t $SESSION -n shell
tmux select-window -t $SESSION:qemu
exec tmux attach -t $SESSION

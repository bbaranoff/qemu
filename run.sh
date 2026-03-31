#!/bin/bash
SESSION="calypso"
TRX_FW="/opt/GSM/firmware/board/compal_e88/trx.highram.elf"
L1_FW="/opt/GSM/firmware/board/compal_e88/layer1.highram.elf"
BTS_CFG="/root/osmo-bts-trx.cfg"

# ---- Cleanup ----
tmux kill-session -t $SESSION 2>/dev/null
killall -9 qemu-system-arm transceiver osmo-bts-trx mobile ccch_scan 2>/dev/null
pkill -9 -f l1ctl_bridge 2>/dev/null
pkill -9 -f l1ctl_passthrough 2>/dev/null
pkill -9 -f grgsm_trxd 2>/dev/null
pkill -9 -f osmo-bsc 2>/dev/null
systemctl stop osmo-bsc 2>/dev/null || true
systemctl disable osmo-bsc 2>/dev/null || true
rm -f /tmp/qemu-calypso-mon.sock /tmp/qemu-calypso-mon2.sock /tmp/osmocom_l2 /tmp/osmocom_l2.2
sleep 2

cd /opt/GSM/qemu

# ---- QEMU 1: TRX (BTS radio) — port 6700 ----
CALYPSO_AIR_LOCAL=4800 CALYPSO_AIR_PEER=4801 ./build/qemu-system-arm -M calypso -cpu arm946 \
  -serial pty -serial pty \
  -monitor "unix:/tmp/qemu-calypso-mon.sock,server,nowait" \
  -kernel $TRX_FW > /root/qemu-trx.log 2>&1 &
sleep 4
printf "cont\n" | socat - UNIX-CONNECT:/tmp/qemu-calypso-mon.sock 2>/dev/null
TRX_PTY=$(grep -o "/dev/pts/[0-9]*" /root/qemu-trx.log | head -1)

# ---- QEMU 2: Layer1 (MS) — TRX disabled ----
CALYPSO_TRX_PORT=0 CALYPSO_AIR_LOCAL=4801 CALYPSO_AIR_PEER=4800 \
./build/qemu-system-arm -M calypso -cpu arm946 \
  -serial pty -serial pty \
  -monitor "unix:/tmp/qemu-calypso-mon2.sock,server,nowait" \
  -kernel $L1_FW > /root/qemu-mobile.log 2>&1 &
sleep 4
printf "cont\n" | socat - UNIX-CONNECT:/tmp/qemu-calypso-mon2.sock 2>/dev/null
MS_PTY=$(grep -o "/dev/pts/[0-9]*" /root/qemu-mobile.log | head -1)

echo "TRX_PTY=$TRX_PTY  MS_PTY=$MS_PTY"
[ -z "$TRX_PTY" ] || [ -z "$MS_PTY" ] && echo "FATAL: PTY detection failed" && exit 1

# ---- tmux ----
tmux new-session -d -s $SESSION -n trx

# Window 0: TRX bridge (creates /tmp/osmocom_l2)
tmux send-keys -t $SESSION:0 "cd /opt/GSM/qemu && BRIDGE_AIR_PORT=0 python3 l1ctl_bridge.py $TRX_PTY" C-m
sleep 2

# Window 0 pane 1: MS bridge (creates /tmp/osmocom_l2.2 for transceiver -2)
tmux split-window -t $SESSION:0 -h
tmux send-keys -t $SESSION:0.1 "cd /opt/GSM/qemu && BRIDGE_AIR_PORT=4801 python3 l1ctl_bridge.py $MS_PTY /tmp/osmocom_l2.2" C-m
sleep 2

# Window 1: transceiver with -2 (second phone on /tmp/osmocom_l2.2)
tmux new-window -t $SESSION -n transceiver
tmux send-keys -t $SESSION:1 "/opt/GSM/osmocom-bb-transceiver/src/host/layer23/src/transceiver/transceiver -a 100 -2" C-m
sleep 3

# Window 2: bts
tmux new-window -t $SESSION -n bts
tmux send-keys -t $SESSION:2 "osmo-bts-trx -c $BTS_CFG" C-m

# Window 3: mobile/ccch_scan (connects to /tmp/osmocom_l2.2)
tmux new-window -t $SESSION -n mobile
tmux send-keys -t $SESSION:3 "sleep 3 && mobile -c /root/.osmocom/bb/mobile-qemu.cfg" C-m

# Window 4: bsc
tmux new-window -t $SESSION -n bsc
tmux send-keys -t $SESSION:4 "osmo-bsc -c /etc/osmocom/osmo-bsc.cfg" C-m

# Window 5: shell
tmux new-window -t $SESSION -n shell

tmux select-window -t $SESSION:4
exec tmux attach -t $SESSION

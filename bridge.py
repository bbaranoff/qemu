#!/usr/bin/env python3
"""
bridge.py — BTS TRX UDP bridge for QEMU Calypso

BTS side:  osmo-bts-trx (CLK/TRXC/TRXD on 5700-5702)
QEMU side: BSP receives DL bursts on UDP 6702
           QEMU sends TDMA ticks on UDP 6700 (QEMU is clock master)

The bridge tracks QEMU's FN but sends CLK IND to the BTS at wall-clock
rate (~470ms = 102 frames). The BTS scheduler needs real-time CLK IND
to avoid drift, but the FN must come from QEMU for coherence.

Usage: bridge.py
"""
import errno, os, select, signal, socket, struct, sys, time

GSM_HYPERFRAME = 2715648
CLK_IND_PERIOD = 102

def udp_bind(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.setblocking(False)
    return s

class Bridge:
    def __init__(self, bts_base=5700):
        self.clk_sock  = udp_bind(bts_base)
        self.trxc_sock = udp_bind(bts_base + 1)
        self.trxd_sock = udp_bind(bts_base + 2)
        self.bts_clk_addr = ("127.0.0.1", bts_base + 100)
        self.trxc_remote = None
        self.trxd_remote = None
        self.powered = False
        self.fn = 0
        self._stop = False
        self.stats = {"clk": 0, "trxc": 0, "dl": 0, "ul": 0, "tick": 0}

        # QEMU CLK socket: we BIND (receive qemu acks/heartbeats on 6700)
        # and we SEND fn updates to qemu on 6701 so qemu's TINT0 syncs
        # to our real-time clock.
        self.qemu_clk_sock = udp_bind(6700)
        self.qemu_sync_addr = ("127.0.0.1", 6701)

        # Wall-clock timer for CLK IND to BTS
        self.last_clk_time = 0.0
        self.clk_interval = CLK_IND_PERIOD * 4.615e-3  # ~470ms
        self.last_clk_fn  = -1  # monotonic guard
        self.clk_start_time = 0.0
        self.clk_start_fn   = 0

        print(f"bridge: CLK={bts_base} TRXC={bts_base+1} TRXD={bts_base+2} → BSP@6702", flush=True)
        print(f"bridge: QEMU CLK ticks on UDP 6700 (clock-slave mode)", flush=True)

    def handle_qemu_tick(self):
        """Receive TDMA tick from QEMU — 4 bytes big-endian FN."""
        try:
            data, addr = self.qemu_clk_sock.recvfrom(64)
        except OSError:
            return
        if len(data) < 4:
            return

        fn = int.from_bytes(data[0:4], 'big')
        self.fn = fn % GSM_HYPERFRAME
        self.stats["tick"] += 1

        if self.stats["tick"] <= 3 or self.stats["tick"] % 10000 == 0:
            print(f"bridge: QEMU tick #{self.stats['tick']} FN={self.fn}", flush=True)

    def maybe_send_clk(self):
        """Send CLK IND to BTS at the REAL GSM clock rate (wall-clock),
        regardless of QEMU's emulation speed. BTS advances its internal
        scheduler at 4.615ms per frame using its own OS timer; CLK INDs
        must match this cadence or BTS reports 'GSM clock skew' and fills
        bursts with dummy content.

        The bridge acts as a real-time clock master: each CLK IND fn is
        computed from elapsed wall-clock time since power-on, not from
        QEMU's tick count. QEMU may lag behind but burst scheduling on
        the BTS stays coherent with the real GSM 217 frames/sec."""
        if not self.powered:
            return
        now = time.monotonic()
        if self.clk_start_time == 0.0:
            self.clk_start_time = now
            self.clk_start_fn   = 0  # BTS adopts first CLK IND as its clock
        # Compute wall-clock-expected fn for the current moment.
        elapsed = now - self.clk_start_time
        expected_fn = self.clk_start_fn + int(elapsed * 1000.0 / 4.615)
        next_clk_fn = self.last_clk_fn + CLK_IND_PERIOD
        if expected_fn < next_clk_fn:
            return  # not enough real time elapsed yet
        # Quantize to multiples of CLK_IND_PERIOD (BTS expects step = 102).
        clk_fn = (expected_fn // CLK_IND_PERIOD) * CLK_IND_PERIOD
        if clk_fn <= self.last_clk_fn:
            clk_fn = self.last_clk_fn + CLK_IND_PERIOD
        self.last_clk_fn = clk_fn
        try:
            self.clk_sock.sendto(
                f"IND CLOCK {clk_fn}\0".encode(), self.bts_clk_addr)
            self.stats["clk"] += 1
        except OSError:
            pass
        # Also push fn to QEMU (port 6701) so its TINT0/fn stays in sync
        # with the bridge's real-time master clock.
        try:
            pkt = clk_fn.to_bytes(4, "big")
            self.qemu_clk_sock.sendto(pkt, self.qemu_sync_addr)
        except OSError:
            pass

    def handle_trxc(self):
        try: data, addr = self.trxc_sock.recvfrom(256)
        except OSError: return
        if not data: return
        self.trxc_remote = addr
        raw = data.strip(b'\x00').decode(errors='replace')
        if not raw.startswith("CMD "): return
        parts = raw[4:].split(); verb = parts[0]; args = parts[1:]
        self.stats["trxc"] += 1

        if verb == "POWERON":
            self.powered = True; print(f"BTS: POWERON", flush=True)
            rsp = "RSP POWERON 0"
        elif verb == "POWEROFF":
            self.powered = False; rsp = "RSP POWEROFF 0"
        elif verb == "SETFORMAT":
            # Accept any TRXD PDU version requested (we now support v2).
            req_ver = int(args[0]) if args else 0
            self.trxd_ver = req_ver
            rsp = f"RSP SETFORMAT 0 {req_ver}"
            print(f"bridge: SETFORMAT → use TRXDv{req_ver}", flush=True)
        elif verb == "NOMTXPOWER":
            rsp = "RSP NOMTXPOWER 0 50"
        elif verb == "MEASURE":
            freq = args[0] if args else "0"
            rsp = f"RSP MEASURE 0 {freq} -60"
        else:
            rsp = f"RSP {verb} 0 {' '.join(args)}".rstrip()

        self.trxc_sock.sendto((rsp + "\0").encode(), addr)

    def handle_trxd(self):
        """Bidirectional TRXD :
           - DL : packet from BTS  → forward to qemu BSP  (port 6702)
           - UL : packet from BSP  → forward to BTS       (remembered addr)
           Source distinguishes direction. """
        try: data, addr = self.trxd_sock.recvfrom(512)
        except OSError: return
        if len(data) < 6: return

        # Detect source: BSP runs on 127.0.0.1:6702 (we forward DL there).
        # Anything coming FROM that port is UL → relay to BTS.
        from_bsp = (addr[0] in ("127.0.0.1", "::1") and addr[1] == 6702)

        if from_bsp:
            # UL from qemu BSP → BTS
            self.stats["ul"] = self.stats.get("ul", 0) + 1
            tn = data[0] & 0x07
            fn = int.from_bytes(data[1:5], 'big') if len(data) >= 5 else 0
            if self.stats["ul"] <= 10 or self.stats["ul"] % 1000 == 0:
                print(f"bridge: UL #{self.stats['ul']} TN={tn} FN={fn} "
                      f"len={len(data)} hdr={data[:8].hex()}", flush=True)
            if self.trxd_remote:
                try: self.trxd_sock.sendto(data, self.trxd_remote)
                except OSError: pass
            return

        # DL from BTS → qemu BSP
        self.trxd_remote = addr
        self.stats["dl"] += 1

        tn = data[0] & 0x07
        fn = int.from_bytes(data[1:5], 'big')
        # TRXDv0 DL header = 6 bytes per osmo-bts trx_if.c:1153-1158:
        #   [0] = (ver<<4) | tn ; [1..4] = fn BE ; [5] = att ; [6+] = 148 hard-bits
        hdr_bytes = data[:6] if len(data) >= 6 else data
        payload = data[6:] if len(data) > 6 else b''
        is_fb = all(b == 0 for b in payload) if payload else False
        if self.stats["dl"] <= 10 or self.stats["dl"] % 5000 == 0 or is_fb:
            print(f"bridge: DL #{self.stats['dl']} TN={tn} FN={fn} len={len(data)} "
                  f"hdr={hdr_bytes.hex()} "
                  f"bits[0:8]={list(payload[:8])} "
                  f"{'*** FB ***' if is_fb else ''}", flush=True)

        try: self.trxd_sock.sendto(data, ("127.0.0.1", 6702))
        except OSError: pass

    def run(self):
        running = True
        def shutdown(s, f):
            nonlocal running; running = False
        signal.signal(signal.SIGINT, shutdown)
        signal.signal(signal.SIGTERM, shutdown)

        while running:
            fds = [self.trxc_sock, self.trxd_sock, self.qemu_clk_sock]
            try:
                readable, _, _ = select.select(fds, [], [], 0.1)
            except (OSError, ValueError) as e:
                print(f"bridge: select error: {e}", flush=True)
                break

            if self.qemu_clk_sock in readable: self.handle_qemu_tick()
            if self.trxc_sock in readable: self.handle_trxc()
            if self.trxd_sock in readable: self.handle_trxd()

            # Send CLK IND at wall-clock rate
            self.maybe_send_clk()

            if self.stats["dl"] > 0 and self.stats["dl"] % 5000 == 0:
                print(f"bridge: tick={self.stats['tick']} clk={self.stats['clk']} dl={self.stats['dl']} ul={self.stats['ul']}", flush=True)

        self._stop = True
        print(f"bridge: tick={self.stats['tick']} clk={self.stats['clk']} trxc={self.stats['trxc']} dl={self.stats['dl']} ul={self.stats['ul']}", flush=True)

if __name__ == "__main__":
    Bridge().run()

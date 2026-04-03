#!/usr/bin/env python3
"""
gsm_clock.py — Master GSM TDMA clock for the Calypso QEMU stack.

Single source of truth for frame number (FN). Sends air bursts to all
targets at GSM cadence so they stay synchronized.

FIX P2: Real GSM 05.03 CRC for SCH (CRC-10) and L2 (fire code CRC-40).

Usage:
  python3 gsm_clock.py [-r RATE] [--bsic BSIC] [--arfcn ARFCN]
"""

import argparse
import socket
import struct
import time
import signal
import sys

GSM_HYPERFRAME = 2715648
GSM_FRAME_US = 4615

FCCH_FN51 = {0, 10, 20, 30, 40}
SCH_FN51  = {1, 11, 21, 31, 41}

FCCH_BURST = bytes(148)

SCH_TRAIN = [
    1,0,1,1,1,0,0,1,0,1,1,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,1,1,1,1,
    0,0,1,0,1,1,0,1,0,1,0,0,0,1,0,1,0,1,1,1,0,1,1,0,0,0,0,1,1,0,1,1,
]

IDLE_L2 = bytes([0x03, 0x03, 0x01, 0x2B] + [0x2B] * 19)  # 23 bytes


def gsm_crc10(bits):
    """GSM 05.03 SCH CRC-10: g(D) = D^10+D^8+D^6+D^5+D^4+D^2+D+1 = 0x175 << shifted"""
    # Polynomial: x^10 + x^8 + x^6 + x^5 + x^4 + x^2 + x + 1
    poly = 0x537  # 10000101110111 — bit-reversed representation not needed, use shift register
    reg = 0
    for b in bits:
        feedback = ((reg >> 9) ^ b) & 1
        reg = ((reg << 1) & 0x3FF)
        if feedback:
            reg ^= 0x175  # generator polynomial without leading x^10
    # Return 10 CRC bits (MSB first)
    return [(reg >> (9 - i)) & 1 for i in range(10)]


def gsm_fire_code_40(bits):
    """GSM 05.03 fire code CRC-40: g(D) = D^40+D^26+D^23+D^17+D^3+1"""
    # 40-bit shift register
    poly = (1 << 26) | (1 << 23) | (1 << 17) | (1 << 3) | 1  # without D^40
    reg = 0
    for b in bits:
        feedback = ((reg >> 39) ^ b) & 1
        reg = ((reg << 1) & ((1 << 40) - 1))
        if feedback:
            reg ^= poly
    return [(reg >> (39 - i)) & 1 for i in range(40)]


def make_sch_burst(bsic, fn):
    """Build a 148-bit SCH burst with proper CRC-10 (GSM 05.03)."""
    t1 = fn // (26 * 51)
    t2 = fn % 26
    t3 = fn % 51
    t3p = (t3 - 1) // 10 if t3 > 0 else 0

    # 25 info bits: spare(2) + BSIC(6) + T1(11) + T2(5) + T3'(3)
    info = [0, 0]
    for i in range(5, -1, -1):
        info.append((bsic >> i) & 1)
    for i in range(10, -1, -1):
        info.append((t1 >> i) & 1)
    for i in range(4, -1, -1):
        info.append((t2 >> i) & 1)
    for i in range(2, -1, -1):
        info.append((t3p >> i) & 1)

    # CRC-10 (real GSM 05.03)
    crc = gsm_crc10(info)

    # 25 info + 10 CRC + 4 tail = 39 bits
    full = info + crc + [0] * 4

    # Convolutional encode (rate 1/2, K=5)
    reg = 0
    coded = []
    for b in full:
        reg = ((reg << 1) | b) & 0x1F
        g0 = ((reg >> 0) ^ (reg >> 3) ^ (reg >> 4)) & 1
        g1 = ((reg >> 0) ^ (reg >> 1) ^ (reg >> 3) ^ (reg >> 4)) & 1
        coded.extend([g0, g1])

    # SCH burst: 3 tail + 39 coded + 64 train + 39 coded + 3 tail + guard
    burst = [0]*3 + coded[:39] + SCH_TRAIN + coded[39:78] + [0]*3
    burst = (burst + [0]*148)[:148]
    return bytes(burst)


def make_normal_burst(data_bits_114, tsc=0):
    """Build a 148-bit normal burst from 114 coded data bits."""
    TSC_BITS = [
        [0,0,1,0,0,1,0,1,1,1,0,0,0,0,1,0,0,0,1,0,0,1,0,1,1,1],
        [0,0,1,0,1,1,0,1,1,1,0,1,1,1,1,0,0,0,1,0,1,1,0,1,1,1],
        [0,1,0,0,0,0,1,1,1,0,1,1,1,0,1,0,0,1,0,0,0,0,1,1,1,0],
        [0,1,0,0,0,1,1,1,1,0,1,1,0,1,0,0,0,1,0,0,0,1,1,1,1,0],
        [0,0,0,1,1,0,1,0,1,1,1,0,0,1,0,0,0,0,0,1,1,0,1,0,1,1],
        [0,1,0,0,1,1,1,0,1,0,1,1,0,0,0,0,0,1,0,0,1,1,1,0,1,0],
        [1,0,1,0,0,1,1,1,1,1,0,1,1,0,0,0,1,0,1,0,0,1,1,1,1,1],
        [1,1,1,0,1,1,1,1,0,0,0,1,0,0,1,0,1,1,1,0,1,1,1,1,0,0],
    ]
    d = list(data_bits_114)
    if len(d) < 114:
        d.extend([0] * (114 - len(d)))
    mid = TSC_BITS[tsc % 8]
    burst = [0]*3 + d[:57] + [0] + mid + [0] + d[57:114] + [0]*3 + [0]*8
    return bytes((burst + [0]*148)[:148])


def encode_l2_to_coded(l2_bytes):
    """Encode 23 L2 bytes -> 456 coded bits with real fire code CRC-40."""
    # 184 info bits
    info = []
    for byte in l2_bytes[:23]:
        for i in range(7, -1, -1):
            info.append((byte >> i) & 1)
    info = (info + [0]*184)[:184]

    # Fire code CRC-40 (real GSM 05.03)
    parity = gsm_fire_code_40(info)

    # 184 info + 40 parity + 4 tail = 228 bits
    full = info + parity + [0]*4

    # Convolutional encode (rate 1/2, K=5)
    reg = 0
    coded = []
    for b in full:
        reg = ((reg << 1) | b) & 0x1F
        g0 = ((reg >> 0) ^ (reg >> 3) ^ (reg >> 4)) & 1
        g1 = ((reg >> 0) ^ (reg >> 1) ^ (reg >> 3) ^ (reg >> 4)) & 1
        coded.extend([g0, g1])

    # Interleave into 4 bursts of 114 bits
    bursts = [[0]*114 for _ in range(4)]
    for k in range(456):
        bursts[k % 4][k // 4] = coded[k] if k < len(coded) else 0
    return bursts


class GSMClock:
    def __init__(self, rate, bsic, arfcn, targets):
        self.rate = rate
        self.bsic = bsic
        self.arfcn = arfcn
        self.fn = 0
        self.targets = targets
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.running = True
        self.idle_bursts_114 = encode_l2_to_coded(IDLE_L2)

    def send_burst(self, tn, fn, burst_bytes):
        pkt = struct.pack(">BI", tn, fn) + burst_bytes[:148]
        for target in self.targets:
            try:
                self.sock.sendto(pkt, target)
            except OSError:
                pass

    def run(self):
        frame_ns = GSM_FRAME_US * 1000
        if self.rate != 1.0:
            frame_ns = int(frame_ns / self.rate)

        print(f"gsm-clock: BSIC={self.bsic} ARFCN={self.arfcn}", flush=True)
        print(f"gsm-clock: rate={self.rate}x -> {frame_ns/1e6:.3f}ms/frame", flush=True)
        print(f"gsm-clock: targets={self.targets}", flush=True)
        print(f"gsm-clock: starting TDMA...", flush=True)

        t_start = time.monotonic_ns()
        frames_sent = 0

        while self.running:
            fn = self.fn
            fn51 = fn % 51

            if fn51 in FCCH_FN51:
                self.send_burst(0, fn, FCCH_BURST)
            elif fn51 in SCH_FN51:
                sch = make_sch_burst(self.bsic, fn)
                self.send_burst(0, fn, sch)
            else:
                block_starts = [2, 6, 12, 16, 22, 26, 32, 36, 42, 46]
                burst_idx = None
                for start in block_starts:
                    if start <= fn51 < start + 4:
                        burst_idx = fn51 - start
                        break
                if burst_idx is not None:
                    coded_114 = self.idle_bursts_114[burst_idx]
                    nb = make_normal_burst(coded_114, tsc=self.bsic & 0x7)
                    self.send_burst(0, fn, nb)

            self.fn = (self.fn + 1) % GSM_HYPERFRAME
            frames_sent += 1

            if frames_sent <= 5 or frames_sent % 5000 == 0:
                elapsed_s = (time.monotonic_ns() - t_start) / 1e9
                fps = frames_sent / elapsed_s if elapsed_s > 0 else 0
                print(f"gsm-clock: FN={fn} frames={frames_sent} "
                      f"elapsed={elapsed_s:.1f}s fps={fps:.1f}", flush=True)

            target_ns = t_start + frames_sent * frame_ns
            now_ns = time.monotonic_ns()
            sleep_ns = target_ns - now_ns
            if sleep_ns > 0:
                time.sleep(sleep_ns / 1e9)


def main():
    ap = argparse.ArgumentParser(description="Master GSM TDMA clock")
    ap.add_argument("-r", "--rate", type=float, default=0.1)
    ap.add_argument("--bsic", type=int, default=63)
    ap.add_argument("--arfcn", type=int, default=100)
    ap.add_argument("--ip", default="127.0.0.1")
    args = ap.parse_args()

    targets = [
        (args.ip, 6700),
        (args.ip, 6800),
    ]

    clock = GSMClock(args.rate, args.bsic, args.arfcn, targets)

    def stop(_s, _f):
        clock.running = False
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    clock.run()
    print(f"gsm-clock: stopped at FN={clock.fn}", flush=True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
hack.py — direct QEMU GDB-stub client for the Calypso emulator.

Connects to the QEMU gdbstub on tcp::1234, sets two hardware breakpoints
inside the OsmocomBB layer1 firmware, and forces the FB-detection path
to always succeed by patching ARM registers at the right moments.

Targets (from disas of layer1.highram.elf, build of 2026-04-07):

  l1s_fbdet_resp+0x10 = 0x00826434
      ldrh r8, [r3, #72]   ; r8 = dsp_api.ndb->d_fb_det
      lsl  r2, r2, #16
      cmp  r8, #0          ; if d_fb_det == 0 → fail path
      bne  +90 <fb_found>  ; else → fb_found
      → at 0x826434, r8 has just been loaded; we set r8 = 1.

  l1a_fb_compl+0x8    = 0x00826754
      ldr  r3, [r3, #4]    ; r3 = last_fb->attempt
      cmp  r3, #12         ; if attempt > 12 → result=255
      bgt  +20             ; else → fbinfo2cellinfo + l1ctl_fbsb_resp(0)
      → at 0x826754, r3 has just been loaded; we set r3 = 0.

If both patches fire, the firmware sends FBSB_CONF result=0 to mobile.

Usage:
    python3 hack.py [host[:port]]    # default 127.0.0.1:1234

This is a debug-only contraption, kept here so the rest of the codebase
stays free of hacks. Run it in parallel with run_debug.sh (with QEMU
started under -S -gdb tcp::1234).
"""

import os
import socket
import struct
import subprocess
import sys
import time

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 1234

# ARM register indices in the GDB Remote Serial Protocol stream.
ARM_R0  = 0
ARM_R1  = 1
ARM_R2  = 2
ARM_R3  = 3
ARM_R8  = 8
ARM_PC  = 15
ARM_CPSR = 25  # GDB ARM target XML usually exposes CPSR at index 25

# ────────────────── BP address resolution (default vs discovery) ──────────────────
# Defaults: hardcoded, valid for layer1.highram.elf build of 2026-04-07
DEFAULT_BP_FBDET_RESP = 0x00826434  # l1s_fbdet_resp + 0x10  (after ldrh r8,[r3,#72])
DEFAULT_BP_FBSB_COMPL = 0x00826754  # l1a_fb_compl   + 0x08  (after attempt load)
# New BPs from disas of l1s_fbdet_resp:
DEFAULT_BP_FREQ_BGE   = 0x008265bc  # bge 0x8265d8 — skip to fall-through (FB0)
DEFAULT_BP_SNR_CMP    = 0x008265c4  # cmp r3, #0 — patch r3=1 so bne taken (FB0)
# Schedule-redirect: 0x826704 is `bl tdma_schedule_set` reached by ALL success
# paths (FB0→FB1, FB1→SB, FB1 retry). Patch r0=L1_COMPL_FB(=0) and PC=0x82670c
# (bl l1s_compl_sched) to force the FBSB completion callback.
DEFAULT_BP_SCHED_REDIRECT = 0x00826704
COMPL_SCHED_PC = 0x0082670c  # bl l1s_compl_sched in l1s_fbdet_resp

DEFAULT_FW = "/opt/GSM/firmware/board/compal_e88/layer1.highram.elf"

def discover_bps(fw_path: str):
    """Discover BP addresses from ELF symbol table via objdump.
    Returns (bp_fbdet, bp_fbcompl). Falls back to defaults on error.
    """
    syms = {}
    try:
        out = subprocess.check_output(
            ["objdump", "-t", fw_path], stderr=subprocess.DEVNULL, text=True
        )
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 6:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            name = parts[-1]
            syms[name] = addr
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        print(f"[hack] discovery failed ({e}), using defaults", file=sys.stderr)
        return DEFAULT_BP_FBDET_RESP, DEFAULT_BP_FBSB_COMPL

    fbdet = syms.get("l1s_fbdet_resp")
    fbcompl = syms.get("l1a_fb_compl")
    if fbdet is None or fbcompl is None:
        print("[hack] symbols not found, using defaults", file=sys.stderr)
        return DEFAULT_BP_FBDET_RESP, DEFAULT_BP_FBSB_COMPL
    return fbdet + 0x10, fbcompl + 0x08

def _env_addr(name, default):
    v = os.environ.get(name)
    if not v:
        return default
    return int(v, 0)

# Resolution order:
#   1. Explicit env override:  HACK_BP_FBDET / HACK_BP_FBCOMPL
#   2. HACK_DISCOVER=1: read ELF symbol table at HACK_FW (default firmware path)
#   3. Hardcoded defaults
if os.environ.get("HACK_DISCOVER") == "1":
    fw = os.environ.get("HACK_FW", DEFAULT_FW)
    BP_FBDET_RESP, BP_FBSB_COMPL = discover_bps(fw)
    BP_SOURCE = f"discovered from {fw}"
else:
    BP_FBDET_RESP = DEFAULT_BP_FBDET_RESP
    BP_FBSB_COMPL = DEFAULT_BP_FBSB_COMPL
    BP_SOURCE = "hardcoded defaults"

# These two are always hardcoded (no symbol in objdump for the cmp/bge offsets)
BP_FREQ_BGE       = DEFAULT_BP_FREQ_BGE
BP_SNR_CMP        = DEFAULT_BP_SNR_CMP
BP_SCHED_REDIRECT = DEFAULT_BP_SCHED_REDIRECT

BP_FBDET_RESP = _env_addr("HACK_BP_FBDET",   BP_FBDET_RESP)
BP_FBSB_COMPL = _env_addr("HACK_BP_FBCOMPL", BP_FBSB_COMPL)
BP_FREQ_BGE   = _env_addr("HACK_BP_FREQ",    BP_FREQ_BGE)
BP_SNR_CMP    = _env_addr("HACK_BP_SNR",     BP_SNR_CMP)
if "HACK_BP_FBDET" in os.environ or "HACK_BP_FBCOMPL" in os.environ:
    BP_SOURCE = "env override"

# NDB shared memory (ARM side). dsp_api.ndb base = 0xFFD001A8
# d_fb_det at +0x48 (word 36) = 0xFFD001F0
# d_fb_mode at +0x4A         = 0xFFD001F2
NDB_D_FB_DET  = 0xFFD001F0
NDB_D_FB_MODE = 0xFFD001F2

# ────────────────────────── RSP protocol ──────────────────────────

def _checksum(payload: bytes) -> bytes:
    s = sum(payload) & 0xff
    return f"{s:02x}".encode()

def rsp_pack(payload: str) -> bytes:
    p = payload.encode()
    return b"$" + p + b"#" + _checksum(p)

class GdbStub:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def _read(self, n: int = 4096) -> bytes:
        data = self.sock.recv(n)
        if not data:
            raise EOFError("gdbstub closed")
        return data

    def _read_packet(self) -> str:
        """Read one full $...#XX packet, ignoring acks."""
        while True:
            while b"$" not in self.buf:
                self.buf += self._read()
            i = self.buf.index(b"$")
            # drop everything before $ (acks, leftovers)
            self.buf = self.buf[i:]
            i = 0
            while b"#" not in self.buf[i+1:]:
                self.buf += self._read()
            try:
                j = self.buf.index(b"#", i + 1)
            except ValueError:
                self.buf += self._read()
                continue
            if len(self.buf) < j + 3:
                self.buf += self._read()
                continue
            payload = self.buf[i + 1 : j]
            self.buf = self.buf[j + 3 :]
            self.sock.sendall(b"+")
            return payload.decode(errors="replace")

    def send(self, payload: str) -> None:
        self.sock.sendall(rsp_pack(payload))

    def cmd(self, payload: str) -> str:
        self.send(payload)
        return self._read_packet()

    # ── helpers ──

    def set_hw_bp(self, addr: int, kind: int = 4) -> None:
        # Use SW BPs (Z0) — unlimited, more reliable than HW (Z1) on QEMU gdbstub.
        r = self.cmd(f"Z0,{addr:x},{kind}")
        if r != "OK":
            print(f"[hack] Z0@{addr:#x} → {r!r}", file=sys.stderr)

    def cont(self) -> str:
        return self.cmd("c")

    def read_reg(self, idx: int) -> int:
        r = self.cmd(f"p{idx:x}")
        # ARM regs are 4 bytes little-endian hex
        return int.from_bytes(bytes.fromhex(r[:8]), "little")

    def write_reg(self, idx: int, value: int) -> None:
        v = value.to_bytes(4, "little").hex()
        r = self.cmd(f"P{idx:x}={v}")
        if r != "OK":
            print(f"[hack] P{idx}={v} → {r!r}", file=sys.stderr)

    def read_pc(self) -> int:
        return self.read_reg(ARM_PC)

    def write_mem(self, addr: int, data: bytes) -> bool:
        r = self.cmd(f"M{addr:x},{len(data):x}:{data.hex()}")
        if r != "OK":
            print(f"[hack] M@{addr:#x} → {r!r} (probably MMIO, ignored)", file=sys.stderr)
            return False
        return True

    def write_u16(self, addr: int, value: int) -> bool:
        return self.write_mem(addr, value.to_bytes(2, "little"))

    def read_mem(self, addr: int, n: int) -> bytes:
        r = self.cmd(f"m{addr:x},{n:x}")
        try:
            return bytes.fromhex(r)
        except ValueError:
            return b""

# ────────────────────────── main loop ──────────────────────────

def main() -> int:
    host, port = DEFAULT_HOST, DEFAULT_PORT
    if len(sys.argv) > 1:
        s = sys.argv[1]
        if ":" in s:
            host, p = s.split(":", 1)
            port = int(p)
        else:
            host = s

    banner = r"""
   ┌────────────────────────────────────────────────────────────┐
   │   hack.py — Calypso FBSB forcer (GDB-stub direct RSP)      │
   │   "le hack, mais uniquement ici. PAS AILLEURS !!!"         │
   └────────────────────────────────────────────────────────────┘
    """
    print(banner)
    print(f"[hack] ░ connecting to gdbstub @ {host}:{port} ...")
    t0 = time.time()
    g = GdbStub(host, port)
    print(f"[hack] ✓ connected in {(time.time()-t0)*1000:.1f} ms")

    # Initial query
    s = g.cmd("?")
    print(f"[hack] ░ initial stop reply: {s}")
    pc0 = g.read_pc()
    print(f"[hack] ░ ARM PC = {pc0:#010x}")

    print(f"[hack] ░ arming hardware breakpoints ({BP_SOURCE}) ...")
    g.set_hw_bp(BP_FBDET_RESP)
    print(f"[hack]   ✓ HW#1  {BP_FBDET_RESP:#010x}  l1s_fbdet_resp+0x10  →  r8 := 1   (force d_fb_det == 1)")
    g.set_hw_bp(BP_FREQ_BGE)
    print(f"[hack]   ✓ HW#2  {BP_FREQ_BGE:#010x}  bge after cmp |freq_diff|,thresh1  →  PC := PC+4   (skip bge, force PASS)")
    g.set_hw_bp(BP_SNR_CMP)
    print(f"[hack]   ✓ HW#3  {BP_SNR_CMP:#010x}  cmp r3,#0 (snr)  →  r3 := 1   (force snr > 0, take success bne)")
    g.set_hw_bp(BP_SCHED_REDIRECT)
    print(f"[hack]   ✓ HW#4  {BP_SCHED_REDIRECT:#010x}  bl tdma_schedule_set (success exits)  →  r0:=0, PC:={COMPL_SCHED_PC:#x}  (jump to bl l1s_compl_sched)")
    g.set_hw_bp(BP_FBSB_COMPL)
    print(f"[hack]   ✓ HW#5  {BP_FBSB_COMPL:#010x}  l1a_fb_compl+0x08    →  r3 := 0   (force attempt < 13  ⇒  FBSB result=0)")
    print(f"[hack] ░ state engaged: [FB-FORCE] [FREQ-SKIP] [SNR-FORCE] [SCHED-REDIRECT] [COMPL-OVERRIDE]")
    print(f"[hack] ░ resuming target ... 5")
    for i in (4, 3, 2, 1):
        time.sleep(0.15)
        print(f"[hack]                          {i}")
    time.sleep(0.15)
    print(f"[hack] ░ GO ─────────────────────────────────────────────")

    n_fb_force = 0
    n_freq_skip = 0
    n_snr_force = 0
    n_sched_redir = 0
    n_compl_force = 0
    t_start = time.time()

    while True:
        # Continue
        stop = g.cont()
        # Stop reasons: T05 thread:01;hwbreak:; or S05 etc.
        if not stop.startswith(("T", "S")):
            print(f"[hack] unexpected stop: {stop!r}")
            break
        pc = g.read_pc()

        if pc == BP_FBDET_RESP:
            r1 = g.read_reg(ARM_R1)
            r2 = g.read_reg(ARM_R2)
            attempt = r1 & 0xff
            fb_mode = r2 & 0xffff
            old_r8 = g.read_reg(ARM_R8)
            g.write_reg(ARM_R8, 1)
            n_fb_force += 1
            elapsed = time.time() - t_start
            print(
                f"[hack] ▲ [{n_fb_force:04d}] FB-FORCE   "
                f"pc={pc:#010x}  r8 {old_r8}→1  attempt={attempt} fb_mode={fb_mode}  "
                f"t+{elapsed:6.2f}s"
            )

        elif pc == BP_FREQ_BGE:
            # Skip the bge instruction by jumping past it (PC += 4)
            g.write_reg(ARM_PC, pc + 4)
            n_freq_skip += 1
            elapsed = time.time() - t_start
            print(
                f"[hack] ⇒ [{n_freq_skip:04d}] FREQ-SKIP  "
                f"pc={pc:#010x} → {pc+4:#010x}  (skip bge, force |fd|<thresh)  "
                f"t+{elapsed:6.2f}s"
            )

        elif pc == BP_SNR_CMP:
            old_r3 = g.read_reg(ARM_R3)
            g.write_reg(ARM_R3, 1)
            n_snr_force += 1
            elapsed = time.time() - t_start
            print(
                f"[hack] ◆ [{n_snr_force:04d}] SNR-FORCE  "
                f"pc={pc:#010x}  r3 {old_r3}→1  (force snr>0)  "
                f"t+{elapsed:6.2f}s"
            )

        elif pc == BP_SCHED_REDIRECT:
            g.write_reg(ARM_R0, 0)  # r0 = L1_COMPL_FB
            g.write_reg(ARM_PC, COMPL_SCHED_PC)
            n_sched_redir += 1
            elapsed = time.time() - t_start
            print(
                f"[hack] ✦ [{n_sched_redir:04d}] SCHED-REDIRECT "
                f"pc={pc:#010x} → {COMPL_SCHED_PC:#010x}  r0:=0 (L1_COMPL_FB)  "
                f"t+{elapsed:6.2f}s"
            )

        elif pc == BP_FBSB_COMPL:
            old_r3 = g.read_reg(ARM_R3)
            g.write_reg(ARM_R3, 0)
            n_compl_force += 1
            elapsed = time.time() - t_start
            print(
                f"[hack] ★ [{n_compl_force:04d}] COMPL-FORCE "
                f"pc={pc:#010x}  r3 {old_r3}→0  "
                f"⇒ FBSB_CONF result=0  t+{elapsed:6.2f}s"
            )
            print(
                f"[hack]   └─ dashboard: FB={n_fb_force} FREQ={n_freq_skip} "
                f"SNR={n_snr_force} SCHED={n_sched_redir} COMPL={n_compl_force} "
                f"rate={n_fb_force/max(elapsed,0.01):.1f}/s"
            )

        else:
            print(f"[hack] stop at unexpected pc={pc:#010x}")
            # remove ourselves and continue
            break

    return 0


# ═════════════════════════════════════════════════════════════════════
# HELPERS — L1CTL wire format
# ─────────────────────────────────────────────────────────────────────
# Length-prefixed BE16 framing over the layer23 unix socket. Message
# types and packers/parsers used by the provoke mode below. Kept here
# so other ad-hoc scripts can import them from hack.py if needed.
# ═════════════════════════════════════════════════════════════════════
import socket as _sock

L1CTL_RESET_IND  = 7
L1CTL_RESET_REQ  = 8
L1CTL_RESET_CONF = 9
L1CTL_PM_REQ     = 11
L1CTL_PM_CONF    = 12

TARGET_ARFCN     = 100      # synth PM_CONF: which ARFCN gets rxlev
TARGET_RXLEV     = 0        # 0 = no signal → "no cell found"

def l1ctl_pack(mt, payload=b""):
    """Wrap (msg_type, payload) into a length-prefixed L1CTL frame."""
    msg = struct.pack("BBxx", mt, 0) + payload
    return struct.pack(">H", len(msg)) + msg

def l1ctl_send(s, mt, payload=b""):
    s.sendall(l1ctl_pack(mt, payload))

def l1ctl_recv(s):
    """Read one length-prefixed L1CTL message. None on EOF."""
    hdr = b""
    while len(hdr) < 2:
        c = s.recv(2 - len(hdr))
        if not c: return None
        hdr += c
    mlen = struct.unpack(">H", hdr)[0]
    body = b""
    while len(body) < mlen:
        c = s.recv(mlen - len(body))
        if not c: return None
        body += c
    return body

def l1ctl_pm_conf_batches(arfcn_from, arfcn_to,
                          target=TARGET_ARFCN, rxlev=TARGET_RXLEV):
    """Yield PM_CONF payloads (≤10 entries each) over [arfcn_from..arfcn_to]."""
    batch = b""; n = 0
    for a in range(arfcn_from, arfcn_to + 1):
        rx = rxlev if a == target else 0
        batch += struct.pack(">HBB", a, rx, rx); n += 1
        if n >= 10:
            yield batch; batch = b""; n = 0
    if batch:
        yield batch

# ═════════════════════════════════════════════════════════════════════
# L1CTL provoke mode — connects to the layer23 unix socket as a fake L1
# and injects synth RESET_IND/RESET_CONF/PM_CONF (the old "nocell"
# trick from l1ctl_passthrough.py). Use to poke the mobile while the
# real firmware path is debugged.
#
# Usage:  python3 hack.py l1ctl [/tmp/osmocom_l2_1]
# ═════════════════════════════════════════════════════════════════════
L1CTL_FBSB_REQ   = 1
L1CTL_FBSB_CONF  = 2
L1CTL_DATA_IND   = 3
L1CTL_RACH_REQ   = 4
L1CTL_RACH_CONF  = 5
L1CTL_DATA_REQ   = 6
L1CTL_DATA_CONF  = 10
L1CTL_CCCH_MODE_REQ  = 18
L1CTL_CCCH_MODE_CONF = 19
L1CTL_DM_EST_REQ     = 20

L1CTL_NAMES = {
    1:"FBSB_REQ", 2:"FBSB_CONF", 3:"DATA_IND", 4:"RACH_REQ", 5:"RACH_CONF",
    6:"DATA_REQ", 7:"RESET_IND", 8:"RESET_REQ", 9:"RESET_CONF", 10:"DATA_CONF",
    11:"PM_REQ", 12:"PM_CONF", 13:"ECHO_REQ", 14:"ECHO_CONF",
    18:"CCCH_MODE_REQ", 19:"CCCH_MODE_CONF",
    20:"DM_EST_REQ", 21:"DM_FREQ_REQ", 22:"DM_REL_REQ",
}

def l1ctl_fbsb_conf(arfcn=TARGET_ARFCN, bsic=0, result=0):
    """Synth FBSB_CONF payload — result=0 means cell found.
       Layout (osmocom): u32 initial_freq_err, u8 result, u8 bsic, pad×2."""
    return struct.pack(">iBBxx", 0, result, bsic)

def l1ctl_rach_conf(fn=0):
    """Synth RACH_CONF — u32 fn BE."""
    return struct.pack(">I", fn)

def l1ctl_data_conf(chan_nr=0x08, link_id=0, fn=0):
    return struct.pack(">BBxxI", chan_nr, link_id, fn)

def l1ctl_provoke(sock_path="/tmp/osmocom_l2_1"):
    print(f"[hack] L1CTL provoke → connecting to {sock_path}")
    s = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
    s.connect(sock_path)
    print("[hack]   connected — sending synth RESET_IND(BOOT)")
    l1ctl_send(s, L1CTL_RESET_IND, struct.pack("Bxxx", 0))
    while True:
        msg = l1ctl_recv(s)
        if msg is None:
            print("[hack]   peer closed"); return 0
        if not msg: continue
        mt = msg[0]
        name = L1CTL_NAMES.get(mt, f"0x{mt:02x}")
        print(f"[hack]   ← {name} len={len(msg)}")

        if mt == L1CTL_RESET_REQ:
            rt = msg[4] if len(msg) > 4 else 1
            l1ctl_send(s, L1CTL_RESET_CONF, struct.pack("Bxxx", rt))
            print(f"[hack]   → RESET_CONF type={rt}")

        elif mt == L1CTL_PM_REQ and len(msg) >= 12:
            af = (msg[8] << 8) | msg[9]
            at = (msg[10] << 8) | msg[11]
            for batch in l1ctl_pm_conf_batches(af, at):
                l1ctl_send(s, L1CTL_PM_CONF, batch)
            print(f"[hack]   → PM_CONF {af}-{at} (target ARFCN={TARGET_ARFCN})")

        elif mt == L1CTL_FBSB_REQ:
            # Synth FBSB_CONF result=0 (cell found, BSIC=0) at TARGET_ARFCN
            l1ctl_send(s, L1CTL_FBSB_CONF, l1ctl_fbsb_conf())
            print(f"[hack]   → FBSB_CONF result=0 arfcn={TARGET_ARFCN}")

        elif mt == L1CTL_RACH_REQ:
            l1ctl_send(s, L1CTL_RACH_CONF, l1ctl_rach_conf(fn=0))
            print("[hack]   → RACH_CONF fn=0")

        elif mt == L1CTL_DATA_REQ:
            # Echo a DATA_CONF to keep the L2 happy.
            l1ctl_send(s, L1CTL_DATA_CONF, l1ctl_data_conf())
            print("[hack]   → DATA_CONF")

        elif mt == L1CTL_CCCH_MODE_REQ:
            # CCCH_MODE_CONF: u8 ccch_mode, pad×3
            mode = msg[4] if len(msg) > 4 else 0
            l1ctl_send(s, L1CTL_CCCH_MODE_CONF, struct.pack("Bxxx", mode))
            print(f"[hack]   → CCCH_MODE_CONF mode={mode}")

# ═════════════════════════════════════════════════════════════════════
# FULL ATTACH — drives the layer23 through the whole L1 handshake by
# fabricating every CONF/IND, plus pushing fake BCCH DATA_IND with
# minimal System Information so cell selection completes and the MS
# starts RACH. This is the maximal "nocell" extension: from RESET to
# IMMEDIATE ASSIGNMENT, no firmware involved at all.
#
# Usage:  python3 hack.py full [/tmp/osmocom_l2_1]
# ═════════════════════════════════════════════════════════════════════
# L1CTL info_dl header (used by all DATA_IND): chan_nr u8, link_id u8,
# band_arfcn u16 BE, frame_nr u32 BE, rx_level u8, snr u8, num_biterr u8,
# fire_crc u8 → 12 bytes, then 23-byte L2 frame.
def l1ctl_data_ind(chan_nr, l2, arfcn=TARGET_ARFCN, fn=0,
                   rxlev=TARGET_RXLEV, link_id=0):
    if len(l2) < 23:
        l2 = l2 + b"\x2b" * (23 - len(l2))   # GSM L2 fill = 0x2b
    l2 = l2[:23]
    hdr = struct.pack(">BBHIBBBB",
                      chan_nr, link_id, arfcn, fn,
                      rxlev, 0, 0, 0)
    return hdr + l2

# Minimal BCCH SI stubs — RR header + msg type, rest = fill. Real SIs
# carry MCC/MNC/LAC/CI/neighbour list; the L23 will likely log decode
# errors but the goal here is to push the state machine forward.
SI1 = bytes([0x55, 0x06, 0x19]) + b"\x2b" * 20   # RR SI1 (msg type 0x19)
SI2 = bytes([0x55, 0x06, 0x1a]) + b"\x2b" * 20   # RR SI2 (msg type 0x1a)
SI3 = bytes([0x55, 0x06, 0x1b]) + b"\x2b" * 20   # RR SI3 (msg type 0x1b)
SI4 = bytes([0x55, 0x06, 0x1c]) + b"\x2b" * 20   # RR SI4 (msg type 0x1c)

# Channel numbers (TS 04.08 §10.5.2.5)
CHAN_BCCH = 0x80
CHAN_CCCH = 0x88   # PCH/AGCH
CHAN_SDCCH4_0 = 0x20

def push_bcch_cycle(s, fn_base=0):
    """Push one BCCH cycle: SI1..SI4 as DATA_IND on BCCH."""
    for i, si in enumerate((SI1, SI2, SI3, SI4)):
        l1ctl_send(s, L1CTL_DATA_IND,
                   l1ctl_data_ind(CHAN_BCCH, si, fn=fn_base + i * 51))
    print(f"[hack]   → DATA_IND BCCH ×4 (SI1..SI4) fn={fn_base}+")

def l1ctl_full_attach(sock_path="/tmp/osmocom_l2_1"):
    """Drive an L23 through a synthetic full attach. No firmware in loop."""
    print(f"[hack] FULL ATTACH → connecting to {sock_path}")
    s = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
    s.connect(sock_path)
    print("[hack]   connected — RESET_IND(BOOT)")
    l1ctl_send(s, L1CTL_RESET_IND, struct.pack("Bxxx", 0))

    fn = 0
    fbsb_done = False
    while True:
        msg = l1ctl_recv(s)
        if msg is None:
            print("[hack]   peer closed"); return 0
        if not msg: continue
        mt = msg[0]
        name = L1CTL_NAMES.get(mt, f"0x{mt:02x}")
        print(f"[hack]   ← {name} len={len(msg)}")

        if mt == L1CTL_RESET_REQ:
            rt = msg[4] if len(msg) > 4 else 1
            l1ctl_send(s, L1CTL_RESET_CONF, struct.pack("Bxxx", rt))
            print(f"[hack]   → RESET_CONF type={rt}")

        elif mt == L1CTL_PM_REQ and len(msg) >= 12:
            af = (msg[8] << 8) | msg[9]
            at = (msg[10] << 8) | msg[11]
            for batch in l1ctl_pm_conf_batches(af, at):
                l1ctl_send(s, L1CTL_PM_CONF, batch)
            print(f"[hack]   → PM_CONF {af}-{at} (target={TARGET_ARFCN})")

        elif mt == L1CTL_FBSB_REQ:
            l1ctl_send(s, L1CTL_FBSB_CONF, l1ctl_fbsb_conf())
            print(f"[hack]   → FBSB_CONF result=0 arfcn={TARGET_ARFCN}")
            fbsb_done = True
            push_bcch_cycle(s, fn_base=fn); fn += 200

        elif mt == L1CTL_CCCH_MODE_REQ:
            mode = msg[4] if len(msg) > 4 else 0
            l1ctl_send(s, L1CTL_CCCH_MODE_CONF, struct.pack("Bxxx", mode))
            print(f"[hack]   → CCCH_MODE_CONF mode={mode}")
            push_bcch_cycle(s, fn_base=fn); fn += 200

        elif mt == L1CTL_RACH_REQ:
            l1ctl_send(s, L1CTL_RACH_CONF, l1ctl_rach_conf(fn=fn))
            print(f"[hack]   → RACH_CONF fn={fn}")
            # Fake IMMEDIATE ASSIGNMENT on AGCH/CCCH
            ia = bytes([0x2d, 0x06, 0x3f]) + b"\x2b" * 20  # RR IA (0x3f)
            l1ctl_send(s, L1CTL_DATA_IND,
                       l1ctl_data_ind(CHAN_CCCH, ia, fn=fn + 4))
            print("[hack]   → DATA_IND CCCH (IMM ASS)")
            fn += 100

        elif mt == L1CTL_DATA_REQ:
            l1ctl_send(s, L1CTL_DATA_CONF, l1ctl_data_conf(fn=fn))
            print("[hack]   → DATA_CONF")

        elif mt == L1CTL_DM_EST_REQ:
            # Establishing dedicated mode — ack with a CCCH_MODE_CONF-ish
            # success and start emitting empty SDCCH frames so L2 keeps
            # the link alive. Real impl would wait for SABM here.
            l1ctl_send(s, L1CTL_DATA_IND,
                       l1ctl_data_ind(CHAN_SDCCH4_0, b"\x01\x03\x01\x2b",
                                      fn=fn))
            print("[hack]   → DATA_IND SDCCH (DM established)")
            fn += 100

        else:
            # Unknown — log and ignore
            pass

        if fbsb_done and fn % 1000 == 0:
            push_bcch_cycle(s, fn_base=fn); fn += 200

# ═════════════════════════════════════════════════════════════════════
# CHECKERS — format validator + shift detector
# ─────────────────────────────────────────────────────────────────────
# format: walks a buffer of length-prefixed L1CTL frames, checks BE16
#         length, msg_type known, payload size sane. Reports per-frame
#         status. Use to sanity-check captures from the unix socket.
#
# shift:  given an expected bit/byte pattern and a received pattern,
#         scans bit-shifts (0..7) and byte offsets (0..N) and reports
#         which (byte_off, bit_shift, msb_first?) reproduces it. Used
#         for the open TRXDv0 soft-byte shift issue.
# ═════════════════════════════════════════════════════════════════════
def check_format(buf):
    """Validate a stream of length-prefixed L1CTL frames. Returns
       (n_ok, n_bad, [report_lines])."""
    rep = []
    n_ok = n_bad = 0
    i = 0
    idx = 0
    while i + 2 <= len(buf):
        mlen = struct.unpack(">H", buf[i:i+2])[0]
        if mlen == 0 or mlen > 1024:
            rep.append(f"  #{idx} @{i}: BAD len={mlen}")
            n_bad += 1; break
        if i + 2 + mlen > len(buf):
            rep.append(f"  #{idx} @{i}: TRUNC need={mlen} have={len(buf)-i-2}")
            n_bad += 1; break
        msg = buf[i+2:i+2+mlen]
        mt = msg[0]
        name = L1CTL_NAMES.get(mt)
        if name is None:
            rep.append(f"  #{idx} @{i}: UNKNOWN mt=0x{mt:02x} len={mlen}")
            n_bad += 1
        else:
            # crude per-type size check
            min_sz = {
                L1CTL_RESET_IND: 4, L1CTL_RESET_CONF: 4, L1CTL_RESET_REQ: 4,
                L1CTL_PM_CONF: 8, L1CTL_FBSB_CONF: 8,
                L1CTL_DATA_IND: 12 + 23, L1CTL_DATA_CONF: 8,
            }.get(mt, 4)
            ok = mlen >= min_sz
            rep.append(f"  #{idx} @{i}: {'OK ' if ok else 'BAD'} "
                       f"{name} len={mlen} (min={min_sz})")
            if ok: n_ok += 1
            else:  n_bad += 1
        i += 2 + mlen
        idx += 1
    if i != len(buf):
        rep.append(f"  trailing {len(buf)-i} bytes unparsed")
    return n_ok, n_bad, rep

def check_shift(expected_bits, received_bits, max_byte_off=4):
    """Find (byte_off, bit_shift, msb_first) such that applying it to
       `received_bits` reproduces `expected_bits`. Both are bytes objects
       containing only 0/1 values (one bit per byte, the TRXDv0 ubit
       convention). Reports all matches found."""
    results = []
    elen = len(expected_bits)
    for byte_off in range(-max_byte_off, max_byte_off + 1):
        for bit_shift in range(8):
            for msb_first in (True, False):
                # Repack received as bytes via shift, then back to bits
                packed = bytearray()
                acc = 0; nbit = 0
                for b in received_bits:
                    if msb_first:
                        acc = (acc << 1) | (b & 1)
                    else:
                        acc = acc | ((b & 1) << nbit)
                    nbit += 1
                    if nbit == 8:
                        packed.append(acc & 0xff); acc = 0; nbit = 0
                # apply bit_shift
                shifted = bytearray()
                for j, p in enumerate(packed):
                    shifted.append(((p << bit_shift) | (
                        packed[j+1] >> (8 - bit_shift)
                        if bit_shift and j+1 < len(packed) else 0)) & 0xff)
                # back to bits, msb-first
                rebits = bytearray()
                for p in shifted:
                    for k in range(7, -1, -1):
                        rebits.append((p >> k) & 1)
                # apply byte_off (in bits)
                off = byte_off * 8
                if off >= 0:
                    cand = bytes(rebits[off:off+elen])
                else:
                    cand = bytes(rebits[:elen+off])
                if len(cand) == elen and cand == expected_bits:
                    results.append((byte_off, bit_shift, msb_first))
    return results

def cli_check_format(path):
    with open(path, "rb") as f:
        buf = f.read()
    n_ok, n_bad, rep = check_format(buf)
    for ln in rep: print(ln)
    print(f"format: ok={n_ok} bad={n_bad}")
    return 0 if n_bad == 0 else 1

def cli_check_shift(exp_path, got_path):
    with open(exp_path, "rb") as f: e = f.read()
    with open(got_path, "rb") as f: g = f.read()
    # accept either raw bytes (interpret each bit) or 0/1 byte streams
    def to_bits(x):
        if all(b in (0, 1) for b in x): return x
        out = bytearray()
        for b in x:
            for k in range(7, -1, -1): out.append((b >> k) & 1)
        return bytes(out)
    e = to_bits(e); g = to_bits(g)
    print(f"expected={len(e)} bits  received={len(g)} bits")
    res = check_shift(e, g)
    if not res:
        print("no shift found"); return 1
    for byte_off, bit_shift, msb in res:
        print(f"  match: byte_off={byte_off:+d} bit_shift={bit_shift} "
              f"msb_first={msb}")
    return 0

# ═════════════════════════════════════════════════════════════════════
# TEST RUNNER — exercises every helper without touching a real socket.
# Run with `hack.py test` or menu option `t`. Each test prints PASS/FAIL
# and the runner returns non-zero if anything failed.
# ═════════════════════════════════════════════════════════════════════
def _t(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print(f"  [{tag}] {name}" + (f"  — {detail}" if detail else ""))
    return 1 if cond else 0

def run_all_tests():
    print("── hack.py self-tests ──")
    n = ok = 0

    # 1. l1ctl_pack/recv roundtrip via in-memory pipe
    import socket as _s
    a, b = _s.socketpair()
    payload = struct.pack(">iBBxx", 0, 0, 7)
    a.sendall(l1ctl_pack(L1CTL_FBSB_CONF, payload))
    got = l1ctl_recv(b)
    n += 1; ok += _t("pack/recv FBSB_CONF roundtrip",
                     got is not None and got[0] == L1CTL_FBSB_CONF
                     and got[4:] == payload,
                     f"got={got!r}")
    a.close(); b.close()

    # 2. l1ctl_send writes correct length prefix
    a, b = _s.socketpair()
    l1ctl_send(a, L1CTL_RESET_IND, struct.pack("Bxxx", 0))
    raw = b.recv(64)
    mlen = struct.unpack(">H", raw[:2])[0]
    n += 1; ok += _t("RESET_IND length prefix BE16",
                     mlen == len(raw) - 2 and raw[2] == L1CTL_RESET_IND,
                     f"mlen={mlen} raw_len={len(raw)}")
    a.close(); b.close()

    # 3. PM_CONF batches: count entries, sizes, target rxlev
    batches = list(l1ctl_pm_conf_batches(1, 25, target=10, rxlev=48))
    total = sum(len(b) for b in batches) // 4
    has_target = any(b"\x00\x0a\x30\x30" in x for x in batches)
    n += 1; ok += _t("PM_CONF batches enumerate range",
                     total == 25 and len(batches) == 3 and has_target,
                     f"batches={len(batches)} total={total}")

    # 4. l1ctl_data_ind size = 12 hdr + 23 L2
    di = l1ctl_data_ind(CHAN_BCCH, SI3, fn=42)
    n += 1; ok += _t("DATA_IND size 12+23",
                     len(di) == 35 and di[3] == TARGET_ARFCN, f"len={len(di)}")

    # 5. fbsb_conf result encoding
    n += 1; ok += _t("FBSB_CONF result=0",  l1ctl_fbsb_conf()[4] == 0)
    n += 1; ok += _t("FBSB_CONF result=255", l1ctl_fbsb_conf(result=255)[4] == 255)

    # 6. check_format on a synthetic stream
    stream = (l1ctl_pack(L1CTL_RESET_IND, struct.pack("Bxxx", 0)) +
              l1ctl_pack(L1CTL_PM_CONF, struct.pack(">HBB", 100, 48, 48)) +
              l1ctl_pack(L1CTL_FBSB_CONF, l1ctl_fbsb_conf()))
    nok, nbad, _rep = check_format(stream)
    n += 1; ok += _t("check_format clean stream",
                     nok == 3 and nbad == 0, f"ok={nok} bad={nbad}")

    # 7. check_format detects truncation
    bad = stream[:-3]
    nok2, nbad2, _ = check_format(bad)
    n += 1; ok += _t("check_format detects TRUNC", nbad2 >= 1)

    # 8. check_format detects unknown msg type
    bogus = struct.pack(">H", 4) + bytes([0xff, 0, 0, 0])
    nok3, nbad3, _ = check_format(bogus)
    n += 1; ok += _t("check_format detects unknown mt", nbad3 == 1)

    # 9. check_shift trivial: identity bit-stream → byte_off=0,bit_shift=0
    bits = bytes([1,0,1,1,0,0,1,0]*4)   # 32 bits
    res = check_shift(bits, bits, max_byte_off=2)
    n += 1; ok += _t("check_shift identity match",
                     any(r == (0, 0, True) for r in res),
                     f"results={res[:3]}")

    # 10. check_shift detects byte_off=+1
    shifted = bytes([0]*8) + bits
    res2 = check_shift(bits, shifted, max_byte_off=3)
    n += 1; ok += _t("check_shift byte_off=+1 detect",
                     any(r[0] == 1 for r in res2),
                     f"results={res2[:3]}")

    # 11. CHAN_* constants sanity
    n += 1; ok += _t("CHAN_BCCH=0x80 CHAN_CCCH=0x88",
                     CHAN_BCCH == 0x80 and CHAN_CCCH == 0x88)

    # 12. SI stubs are 23 bytes
    n += 1; ok += _t("SI1..SI4 are 23 bytes each",
                     all(len(x) == 23 for x in (SI1, SI2, SI3, SI4)))

    print(f"── {ok}/{n} passed ──")
    return 0 if ok == n else 1

# ═════════════════════════════════════════════════════════════════════
# CLI menu
# ═════════════════════════════════════════════════════════════════════
MENU = """\
hack.py — Calypso emulator provocation toolbox
═══════════════════════════════════════════════
  1) gdb       — GDB-stub BP patching (force FB-DET path)
  2) l1ctl     — L1CTL provoke (reactive synth RESET/PM/FBSB/RACH/DATA)
  3) full      — Full attach (reactive + push BCCH SI1..4 + IMM ASS)
  4) reset     — Send only RESET_IND(BOOT) and exit
  5) pm        — Send only PM_CONF for ARFCN range
  6) fbsb      — Send only FBSB_CONF result=0
  i) inject    — Interactive REQ/RESP injector menu
  t) test      — Run all helper self-tests
  q) quit
"""

# ═════════════════════════════════════════════════════════════════════
# INJECTOR MENU — pick any L1CTL REQ or RESP to send manually,
# one shot at a time, on a persistent connection. Lets you walk
# the L23 state machine step by step instead of running an automated
# scenario. Useful when full attach desyncs and you want to nudge it.
# ═════════════════════════════════════════════════════════════════════
INJECT_MENU = """\
─── INJECT (REQ from MS side / RESP from L1 side) ───
   Responses (L1→MS):                  Requests (MS→L1) [echo-only]:
    a) RESET_IND(BOOT)                   A) RESET_REQ type=1
    b) RESET_CONF                        B) FBSB_REQ
    c) PM_CONF (one ARFCN)               C) PM_REQ range
    d) PM_CONF range                     D) RACH_REQ
    e) FBSB_CONF result=0                E) DATA_REQ
    f) FBSB_CONF result=255 (fail)       F) CCCH_MODE_REQ
    g) RACH_CONF                         G) DM_EST_REQ
    h) DATA_CONF
    i) DATA_IND BCCH SI1
    j) DATA_IND BCCH SI2
    k) DATA_IND BCCH SI3
    l) DATA_IND BCCH SI4
    m) DATA_IND CCCH IMM ASS
    n) CCCH_MODE_CONF mode=0
    p) push full BCCH cycle (SI1..4)
   ----
    r) recv one msg (non-blocking 1s)
    s) connect / reconnect socket
    x) close socket
    q) back to main menu
"""

def inject_menu(sock_path):
    """Persistent connection; send any frame on demand. r=read, q=quit."""
    s = None
    def ensure():
        nonlocal s
        if s is None:
            s = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
            s.connect(sock_path)
            print(f"  connected → {sock_path}")
        return s
    print(INJECT_MENU)
    while True:
        try:
            c = input("inject> ").strip()
        except (EOFError, KeyboardInterrupt):
            print(); break
        if not c: print(INJECT_MENU); continue
        try:
            if c == "q": break
            if c == "?": print(INJECT_MENU); continue
            if c == "s":
                if s: s.close()
                s = None; ensure(); continue
            if c == "x":
                if s: s.close(); s = None; print("  closed"); continue
            if c == "r":
                if not s: print("  not connected"); continue
                import select as _sel
                r, _, _ = _sel.select([s], [], [], 1.0)
                if not r: print("  (no msg)"); continue
                m = l1ctl_recv(s)
                if m is None: print("  EOF"); s.close(); s = None; continue
                mt = m[0]
                hexd = " ".join(f"{b:02x}" for b in m[:24])
                print(f"  ← {L1CTL_NAMES.get(mt, hex(mt))} "
                      f"len={len(m)} [{hexd}]")
                continue

            ensure()
            # ── RESPONSES (L1 → MS) ──
            if c == "a":
                l1ctl_send(s, L1CTL_RESET_IND, struct.pack("Bxxx", 0))
            elif c == "b":
                l1ctl_send(s, L1CTL_RESET_CONF, struct.pack("Bxxx", 1))
            elif c == "c":
                a = int(input("    ARFCN: "))
                rx = int(input("    rxlev [48]: ") or "48")
                l1ctl_send(s, L1CTL_PM_CONF, struct.pack(">HBB", a, rx, rx))
            elif c == "d":
                af = int(input("    from [1]: ") or "1")
                at = int(input("    to   [124]: ") or "124")
                for b in l1ctl_pm_conf_batches(af, at):
                    l1ctl_send(s, L1CTL_PM_CONF, b)
            elif c == "e":
                l1ctl_send(s, L1CTL_FBSB_CONF, l1ctl_fbsb_conf(result=0))
            elif c == "f":
                l1ctl_send(s, L1CTL_FBSB_CONF, l1ctl_fbsb_conf(result=255))
            elif c == "g":
                l1ctl_send(s, L1CTL_RACH_CONF, l1ctl_rach_conf(fn=0))
            elif c == "h":
                l1ctl_send(s, L1CTL_DATA_CONF, l1ctl_data_conf())
            elif c == "i":
                l1ctl_send(s, L1CTL_DATA_IND, l1ctl_data_ind(CHAN_BCCH, SI1))
            elif c == "j":
                l1ctl_send(s, L1CTL_DATA_IND, l1ctl_data_ind(CHAN_BCCH, SI2))
            elif c == "k":
                l1ctl_send(s, L1CTL_DATA_IND, l1ctl_data_ind(CHAN_BCCH, SI3))
            elif c == "l":
                l1ctl_send(s, L1CTL_DATA_IND, l1ctl_data_ind(CHAN_BCCH, SI4))
            elif c == "m":
                ia = bytes([0x2d, 0x06, 0x3f]) + b"\x2b" * 20
                l1ctl_send(s, L1CTL_DATA_IND, l1ctl_data_ind(CHAN_CCCH, ia))
            elif c == "n":
                l1ctl_send(s, L1CTL_CCCH_MODE_CONF, struct.pack("Bxxx", 0))
            elif c == "p":
                push_bcch_cycle(s)
            # ── REQUESTS (MS → L1) — for testing only ──
            elif c == "A":
                l1ctl_send(s, L1CTL_RESET_REQ, struct.pack("Bxxx", 1))
            elif c == "B":
                # FBSB_REQ payload: u16 arfcn, u8 flags, u8 sync_info_idx,
                # u8 ccch_mode, u8 rxlev_exp → 6 bytes
                l1ctl_send(s, L1CTL_FBSB_REQ,
                           struct.pack(">HBBBB", TARGET_ARFCN, 0x07, 0, 0, 0))
            elif c == "C":
                af = int(input("    from [1]: ") or "1")
                at = int(input("    to   [124]: ") or "124")
                l1ctl_send(s, L1CTL_PM_REQ,
                           struct.pack(">BxxxxxxxHH", 1, af, at))
            elif c == "D":
                l1ctl_send(s, L1CTL_RACH_REQ, struct.pack("Bxxx", 0))
            elif c == "E":
                l1ctl_send(s, L1CTL_DATA_REQ,
                           struct.pack("BBxx", 0x08, 0) + b"\x2b" * 23)
            elif c == "F":
                l1ctl_send(s, L1CTL_CCCH_MODE_REQ, struct.pack("Bxxx", 0))
            elif c == "G":
                l1ctl_send(s, L1CTL_DM_EST_REQ, b"\x00" * 8)
            else:
                print("  ?")
                continue
            print(f"  → sent ({c})")
        except Exception as e:
            print(f"  error: {e}")
    if s:
        try: s.close()
        except Exception: pass
    return 0

def _opt(argv, name, default=None, cast=str):
    """Parse --name=value or --name value from argv (mutates argv)."""
    for i, a in enumerate(argv):
        if a == f"--{name}" and i + 1 < len(argv):
            v = argv.pop(i + 1); argv.pop(i); return cast(v)
        if a.startswith(f"--{name}="):
            v = a.split("=", 1)[1]; argv.pop(i); return cast(v)
    return default

def _one_shot(sock_path, fn):
    s = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
    s.connect(sock_path); fn(s); s.close()

def cli_menu():
    print(MENU)
    while True:
        try:
            choice = input("hack> ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print(); return 0
        if choice in ("q", "quit", "exit"): return 0
        sp = input("  socket path [/tmp/osmocom_l2_1]: ").strip() \
             or "/tmp/osmocom_l2_1"
        try:
            if choice in ("1", "gdb"):
                return main()
            elif choice in ("2", "l1ctl"):
                l1ctl_provoke(sp)
            elif choice in ("3", "full"):
                l1ctl_full_attach(sp)
            elif choice in ("4", "reset"):
                _one_shot(sp, lambda s: l1ctl_send(
                    s, L1CTL_RESET_IND, struct.pack("Bxxx", 0)))
                print("  sent RESET_IND(BOOT)")
            elif choice in ("5", "pm"):
                af = int(input("    arfcn_from [1]: ") or "1")
                at = int(input("    arfcn_to [124]: ") or "124")
                def _pm(s):
                    for b in l1ctl_pm_conf_batches(af, at):
                        l1ctl_send(s, L1CTL_PM_CONF, b)
                _one_shot(sp, _pm)
                print(f"  sent PM_CONF {af}-{at}")
            elif choice in ("6", "fbsb"):
                _one_shot(sp, lambda s: l1ctl_send(
                    s, L1CTL_FBSB_CONF, l1ctl_fbsb_conf()))
                print(f"  sent FBSB_CONF result=0 arfcn={TARGET_ARFCN}")
            elif choice in ("i", "inject"):
                inject_menu(sp)
            elif choice in ("t", "test"):
                run_all_tests()
            else:
                print("  ?")
                print(MENU)
        except Exception as e:
            print(f"  error: {e}")

USAGE = """\
Usage:
  hack.py                              # interactive menu
  hack.py menu                         # interactive menu
  hack.py gdb [host[:port]]            # GDB BP patcher (default 127.0.0.1:1234)
  hack.py l1ctl [--sock PATH]          # reactive L1CTL provoke
  hack.py full  [--sock PATH] [--arfcn N] [--rxlev N]
                                       # full synth attach
  hack.py reset [--sock PATH]          # one-shot RESET_IND
  hack.py pm    [--sock PATH] [--from N] [--to N]
  hack.py fbsb  [--sock PATH] [--arfcn N]
"""

if __name__ == "__main__":
    argv = sys.argv[1:]
    if not argv or argv[0] in ("menu", "-i", "--menu"):
        try: sys.exit(cli_menu())
        except KeyboardInterrupt: print(); sys.exit(0)

    cmd = argv.pop(0)
    sock_path = _opt(argv, "sock", "/tmp/osmocom_l2_1")
    arfcn  = _opt(argv, "arfcn", TARGET_ARFCN, int)
    rxlev  = _opt(argv, "rxlev", TARGET_RXLEV, int)
    af     = _opt(argv, "from", 1, int)
    at     = _opt(argv, "to",   124, int)
    TARGET_ARFCN = arfcn
    TARGET_RXLEV = rxlev

    try:
        if cmd in ("gdb", "1"):
            # legacy positional host[:port] for gdb mode
            sys.exit(main())
        elif cmd in ("l1ctl", "2"):
            sys.exit(l1ctl_provoke(sock_path))
        elif cmd in ("full", "3"):
            sys.exit(l1ctl_full_attach(sock_path))
        elif cmd in ("reset", "4"):
            _one_shot(sock_path, lambda s: l1ctl_send(
                s, L1CTL_RESET_IND, struct.pack("Bxxx", 0)))
            print("sent RESET_IND(BOOT)"); sys.exit(0)
        elif cmd in ("pm", "5"):
            def _pm(s):
                for b in l1ctl_pm_conf_batches(af, at):
                    l1ctl_send(s, L1CTL_PM_CONF, b)
            _one_shot(sock_path, _pm)
            print(f"sent PM_CONF {af}-{at}"); sys.exit(0)
        elif cmd in ("fbsb", "6"):
            _one_shot(sock_path, lambda s: l1ctl_send(
                s, L1CTL_FBSB_CONF, l1ctl_fbsb_conf()))
            print(f"sent FBSB_CONF result=0 arfcn={TARGET_ARFCN}"); sys.exit(0)
        elif cmd in ("serve", "nocell", "7"):
            # Server mode: create L1CTL socket, wait for mobile to connect,
            # respond with pm=0 on all ARFCNs → "no cell found"
            import os
            try: os.unlink(sock_path)
            except: pass
            srv = _sock.socket(_sock.AF_UNIX, _sock.SOCK_STREAM)
            srv.bind(sock_path)
            srv.listen(1)
            print(f"[hack] L1CTL server listening on {sock_path}")
            print(f"[hack]   waiting for mobile to connect...")
            conn, _ = srv.accept()
            print(f"[hack]   mobile connected — sending RESET_IND")
            l1ctl_send(conn, L1CTL_RESET_IND, struct.pack("Bxxx", 0))
            while True:
                msg = l1ctl_recv(conn)
                if msg is None:
                    print("[hack]   mobile disconnected"); break
                if not msg: continue
                mt = msg[0]
                name = L1CTL_NAMES.get(mt, f"0x{mt:02x}")
                print(f"[hack]   <- {name} len={len(msg)}")
                if mt == L1CTL_RESET_REQ:
                    rt = msg[4] if len(msg) > 4 else 1
                    l1ctl_send(conn, L1CTL_RESET_CONF, struct.pack("Bxxx", rt))
                    print(f"[hack]   -> RESET_CONF type={rt}")
                elif mt == L1CTL_PM_REQ and len(msg) >= 12:
                    af_ = (msg[8] << 8) | msg[9]
                    at_ = (msg[10] << 8) | msg[11]
                    for batch in l1ctl_pm_conf_batches(af_, at_, rxlev=0):
                        l1ctl_send(conn, L1CTL_PM_CONF, batch)
                    print(f"[hack]   -> PM_CONF {af_}-{at_} (all pm=0)")
                elif mt == L1CTL_FBSB_REQ:
                    # Should not arrive since pm=0 everywhere, but just in case
                    l1ctl_send(conn, L1CTL_FBSB_CONF,
                               l1ctl_fbsb_conf(result=255))
                    print(f"[hack]   -> FBSB_CONF result=255 (no cell)")
                else:
                    print(f"[hack]   (ignored)")
            conn.close(); srv.close()
            sys.exit(0)
        elif cmd in ("test", "t", "selftest", "all"):
            sys.exit(run_all_tests())
        elif cmd in ("-h", "--help", "help"):
            print(USAGE); sys.exit(0)
        else:
            print(f"unknown command: {cmd}"); print(USAGE); sys.exit(2)
    except KeyboardInterrupt:
        print("\n[hack] ░ interrupted by user — au revoir"); sys.exit(0)
    except Exception as e:
        print(f"[hack] error: {e}"); sys.exit(1)

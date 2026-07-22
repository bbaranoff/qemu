#!/usr/bin/env python3
"""corr_iq.py -- diagnostic complet de l'I/Q du correlateur DSP (Calypso QEMU).

Metrique cle = COHERENCE du tone = |Sum iq[k+1].conj(iq[k])| / Sum|iq[k+1]||iq[k]|
  1.0 = tone pur (FCCH) ; ~0 = bruit/data GMSK.
dphi (rotation moyenne/sample), exprime en unites de pi/2 -> infere le SPS :
  +1.00 x pi/2  = FCCH @1SPS (ce que le correlateur veut)
  +0.25 x pi/2  = FCCH @4SPS non-decime (il faut decimer ÷4)
  negatif       = miroir spectral (I/Q swap -> CALYPSO_DL_IQ_CONJ=1)

Sources :
  shunt  : /dev/shm/dsp_iq.cfile (fc32) -- I/Q d'entree du shunt (reference propre)
  rxdump : /tmp/iq_rx_*.bin (CALYPSO_IQDUMP) -- bursts FCCH ecrits en DARAM 0x2a00
  bursts : /dev/shm/bursts.cfile (BSP_DUMP_RX_FILE, IQ16) -- idem, avec fn/tn
  daram  : 0x2a00 live via monitor qemu (best-effort, racy)

Usage : corr_iq.py [--src auto|shunt|rxdump|bursts|daram] [--fs Hz] [--all]
"""
import argparse, os, glob, socket, struct, sys, time
import numpy as np

FS_SYM  = 270833.0
FCCH_HZ = FS_SYM / 4.0
PI2     = np.pi / 2.0


# ---------- metriques ----------
def tone_metrics(iq, fs):
    n = len(iq)
    if n < 8:
        return dict(n=n, rms=0, peak=0, dc=0j, coh=0.0, dphi=0.0, fpk=0.0, conc=0.0, zero=0.0)
    mag = np.abs(iq)
    rms = float(np.sqrt(np.mean(mag ** 2)))
    dc  = complex(np.mean(iq))
    prod = iq[1:] * np.conj(iq[:-1])
    den  = float(np.sum(np.abs(iq[1:]) * np.abs(iq[:-1]))) + 1e-12
    acc  = np.sum(prod)
    coh  = float(np.abs(acc) / den)
    dphi = float(np.angle(acc))
    w = iq - dc
    win = np.hanning(n)
    sp = np.fft.fftshift(np.abs(np.fft.fft(w * win)))
    fr = np.fft.fftshift(np.fft.fftfreq(n, 1.0 / fs))
    k = int(np.argmax(sp))
    return dict(n=n, rms=rms, peak=float(mag.max()), dc=dc, coh=coh, dphi=dphi,
                fpk=float(fr[k]), conc=float(sp[k] / (np.mean(sp) + 1e-12)),
                zero=float(np.mean(mag < 1e-6)))


def verdict(m):
    if m["rms"] < 1e-9 or m["zero"] > 0.98:
        return "VIDE -- rien fed au correlateur"
    r = m["dphi"] / PI2
    near = abs(abs(m["fpk"]) - FCCH_HZ) < 0.15 * FCCH_HZ   # pic FFT a +/-67708 Hz
    # (1) burst unique coherent -> dphi fiable -> SPS + orientation
    if m["coh"] > 0.9:
        if abs(abs(r) - 1.0) < 0.20:
            return ("FCCH @1SPS PROPRE (dphi=%+.2fx pi/2) -- feed CORRECT" % r if r > 0
                    else "FCCH @1SPS MIROIR (dphi=%+.2fx pi/2) -> CALYPSO_DL_IQ_CONJ=1" % r)
        if abs(abs(r) - 0.25) < 0.10:
            return "FCCH @4SPS NON-DECIME (dphi=%+.2fx pi/2) -> decimer ÷4 (CALYPSO_BSP_IQ_DECIM=4)" % r
    # (2) flux continu / complement : pic FFT sur FCCH
    if m["conc"] > 20 and near:
        return "FCCH PRESENTE (FFT %+.0f Hz, conc=%.0f) -- signal OK (flux)" % (m["fpk"], m["conc"])
    if m["coh"] < 0.4 and m["conc"] < 10:
        return "BRUIT/DATA (coh=%.2f) -- pas un tone FCCH" % m["coh"]
    return "coherent hors FCCH std (dphi=%+.2fx pi/2, FFT %+.0f Hz)" % (r, m["fpk"])


def report(iq, fs, label):
    m = tone_metrics(iq, fs)
    print("\n=== %s ===  N=%d  fs=%.0f Hz" % (label, m["n"], fs))
    if m["n"] < 8:
        print("  (trop court)"); return m
    print("  rms=%.3g peak=%.3g |DC|=%.3g zeros=%.0f%%" % (m["rms"], m["peak"], abs(m["dc"]), 100 * m["zero"]))
    print("  coherence=%.3f  dphi=%+.3f rad/samp (%+.2fx pi/2)  FFTpic=%+.0f Hz conc=%.1fx  [FCCH@1SPS=+1.571]"
          % (m["coh"], m["dphi"], m["dphi"] / PI2, m["fpk"], m["conc"]))
    print("  VERDICT: %s" % verdict(m))
    return m


# ---------- loaders ----------
def load_fc32(path, n, off):
    with open(path, "rb") as f:
        f.seek(off * 8, os.SEEK_SET)
        raw = f.read(n * 8)
    return np.frombuffer(raw, dtype=np.complex64).astype(np.complex128)


def load_raw_bins(rxdir):
    out = []
    for f in sorted(glob.glob(os.path.join(rxdir, "iq_rx_*.bin"))):
        a = np.frombuffer(open(f, "rb").read(), dtype="<i2").astype(np.float32)
        a = a[:(len(a) // 2) * 2]
        out.append((os.path.basename(f), a[0::2] + 1j * a[1::2]))
    return out


def load_iq16(path, maxrec=400):
    out = []
    with open(path, "rb") as f:
        while len(out) < maxrec:
            hdr = f.read(12)
            if len(hdr) < 12:
                break
            magic, fn, tn, nint16, _pad = struct.unpack("<4sIBHB", hdr)
            if magic != b"IQ16":
                f.seek(-11, os.SEEK_CUR); continue
            raw = f.read(nint16 * 2)
            if len(raw) < nint16 * 2:
                break
            a = np.frombuffer(raw, dtype="<i2").astype(np.float32)
            out.append(("fn=%u/tn=%u" % (fn, tn), a[0::2] + 1j * a[1::2]))
    return out


def read_daram(mon, addr, words, tries=6):
    """Best-effort : lit `words` mots 16b a addr via HMP xp, garde la lecture la
    plus coherente (le DSP ecrit en concurrence -> racy)."""
    best = None
    for _ in range(tries):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(2.5)
            s.connect(mon); s.recv(4096)
            s.sendall(("xp/%dhx 0x%x\n" % (words, addr)).encode())
            buf = b""; t0 = time.time()
            while time.time() - t0 < 2.5:
                d = s.recv(65536)
                if not d: break
                buf += d
                if b"(qemu)" in buf: break
            s.close()
        except Exception as e:
            continue
        vals = []
        for line in buf.decode(errors="replace").splitlines():
            if ":" not in line: continue
            for tok in line.split(":", 1)[1].split():
                if tok.startswith("0x"):
                    try: vals.append(int(tok, 16))
                    except ValueError: pass
        a = np.array(vals, dtype=np.uint16).astype(np.int16).astype(np.float32)
        a = a[:(len(a) // 2) * 2]
        iq = a[0::2] + 1j * a[1::2]
        if len(iq) < 8: continue
        m = tone_metrics(iq, FS_SYM)
        if best is None or m["coh"] > best[0]:
            best = (m["coh"], iq)
    return best[1] if best else np.array([], dtype=complex)


# ---------- scan per-burst (rxdump / bursts) ----------
def scan_bursts(recs, fs, label):
    scored = []
    for name, iq in recs:
        if len(iq) < 8: continue
        m = tone_metrics(iq, fs)
        if m["rms"] < 1000: continue
        scored.append((m["coh"], m["dphi"], m["rms"], name, iq))
    if not scored:
        print("  (aucun burst non-nul)"); return
    scored.sort(reverse=True)
    nf = sum(1 for c, *_ in scored if c > 0.85)
    print("%s : %d bursts non-nuls, %d coherents (FCCH, coh>0.85)" % (label, len(scored), nf))
    for c, d, r, name, _ in scored[:8]:
        print("  %-14s coh=%.3f dphi=%+.3f (%+.2fx pi/2) rms=%.0f %s"
              % (name, c, d, d / PI2, r, "<- FCCH" if c > 0.85 else ""))
    report(scored[0][4], fs, "burst le + coherent = %s" % scored[0][3])


# ---------- main ----------
def main():
    ap = argparse.ArgumentParser(description="Diag I/Q correlateur DSP")
    ap.add_argument("--src", choices=["auto", "shunt", "rxdump", "bursts", "daram", "all"], default="auto")
    ap.add_argument("--shunt", default="/dev/shm/dsp_iq.cfile")
    ap.add_argument("--rxdir", default="/tmp")
    ap.add_argument("--bursts", default="/dev/shm/bursts.cfile")
    ap.add_argument("--mon", default="/tmp/qemu-calypso-mon.sock")
    ap.add_argument("--addr", default="0xFFD04400")
    ap.add_argument("--words", type=int, default=296)   # daram_len = 296 int16 = 148 cplx
    ap.add_argument("--n", type=int, default=40000)
    ap.add_argument("--off", type=int, default=-1)
    ap.add_argument("--fs", type=float, default=None)
    a = ap.parse_args()

    def do_shunt():
        if not os.path.exists(a.shunt): print("shunt: absent"); return
        sz = os.path.getsize(a.shunt); tot = sz // 8
        off = a.off if a.off >= 0 else max(0, tot - a.n)
        report(load_fc32(a.shunt, a.n, off), a.fs or 1083333.0, "shunt dsp_iq.cfile @%d (fc32, entree 4SPS)" % off)

    def do_rxdump():
        recs = load_raw_bins(a.rxdir)
        if not recs: print("rxdump: aucun iq_rx_*.bin (CALYPSO_IQDUMP=1 + relance)"); return
        scan_bursts(recs, a.fs or 270833.0, "rxdump (/tmp/iq_rx, fed 0x2a00 @1SPS)")

    def do_bursts():
        if not os.path.exists(a.bursts) or os.path.getsize(a.bursts) < 13:
            print("bursts: %s absent/vide (BSP_DUMP_RX_FILE=1 + relance, mode direct ok)" % a.bursts); return
        scan_bursts(load_iq16(a.bursts), a.fs or 270833.0, "bursts.cfile (IQ16, fed 0x2a00 @1SPS)")

    def do_daram():
        if not os.path.exists(a.mon): print("daram: monitor %s absent (qemu down ?)" % a.mon); return
        iq = read_daram(a.mon, int(a.addr, 16), a.words)
        if len(iq) < 8:
            print("daram: lecture vide/echec @%s (0x2a00 hors fenetre API ARM ? racy) -- fie-toi a rxdump/FCCH-PROBE" % a.addr); return
        report(iq, a.fs or 270833.0, "DARAM %s (%d mots, correlateur live, best-of-6)" % (a.addr, a.words))

    src = a.src
    if src == "auto":
        if os.path.exists(a.bursts) and os.path.getsize(a.bursts) > 13: src = "bursts"
        elif glob.glob(os.path.join(a.rxdir, "iq_rx_*.bin")): src = "rxdump"
        else: src = "shunt"
        print("[auto] source = %s" % src)

    if src == "all":
        for fn in (do_shunt, do_bursts, do_rxdump, do_daram):
            try: fn()
            except Exception as e: print("  ERR:", e)
    else:
        {"shunt": do_shunt, "rxdump": do_rxdump, "bursts": do_bursts, "daram": do_daram}[src]()


if __name__ == "__main__":
    main()

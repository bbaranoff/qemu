"""GMSK burst modulator for the QEMU Calypso bridge.

Takes a 148-bit GSM normal/SCH/FCCH burst (ubit_t, bytes 0x00/0x01),
runs it through a gr GMSK modulator at 4 samples/symbol, and returns
complex baseband I/Q samples packed as interleaved int16 little-endian.

Format produced (per burst):
    148 symbols * 4 sps = 592 complex samples
    = 592 * 2 * 2 bytes  = 2368 bytes  (I,Q,I,Q,... int16 LE)

Used by bridge.py to convert osmo-bts DL TRXDv0 hard-bit bursts into
an RTL-SDR-style I/Q stream that the QEMU Calypso BSP can consume.
"""

import struct
import numpy as np
from gnuradio import gr, blocks, digital

SPS = 4
SYMS_PER_BURST = 148
SAMPLES_PER_BURST = SYMS_PER_BURST * SPS
BYTES_PER_BURST = SAMPLES_PER_BURST * 2 * 2  # I+Q int16

# GSM GMSK params (3GPP TS 45.004): BT = 0.3, modulation index h = 0.5
# digital.gmsk_mod produces complex baseband samples at SPS samples/symbol.
_mod = None

def _ensure_mod():
    global _mod
    if _mod is not None:
        return
    # Build a one-shot flowgraph: vector_source -> gmsk_mod -> vector_sink.
    # We'll feed bursts by setting the source data each call and re-running.
    class _GMSKBatch(gr.top_block):
        def __init__(self):
            gr.top_block.__init__(self, "gmsk_burst_batch")
            self.src = blocks.vector_source_b([], False)
            self.mod = digital.gmsk_mod(samples_per_symbol=SPS, bt=0.3)
            self.sink = blocks.vector_sink_c()
            self.connect(self.src, self.mod, self.sink)
    _mod = _GMSKBatch()

def modulate_burst(ubits: bytes) -> bytes:
    """Modulate one 148-bit burst.

    Args:
        ubits: 148 bytes, each 0x00 or 0x01 (osmocom ubit_t).

    Returns:
        2368 bytes: 592 complex samples as int16 LE I,Q interleaved,
        scaled to fill Q15 range.
    """
    if len(ubits) != SYMS_PER_BURST:
        # Pad/truncate defensively
        if len(ubits) < SYMS_PER_BURST:
            ubits = ubits + b'\x00' * (SYMS_PER_BURST - len(ubits))
        else:
            ubits = ubits[:SYMS_PER_BURST]

    # Pad with zero-bit guard symbols on both sides so the gaussian
    # filter group-delay and transient land in the padding, not in the
    # 148-symbol burst payload.
    PAD_SYM = 8
    padded = b'\x00' * PAD_SYM + ubits + b'\x00' * PAD_SYM

    _ensure_mod()
    _mod.src.set_data(list(padded))
    _mod.sink.reset()
    _mod.run()
    cf = np.array(_mod.sink.data(), dtype=np.complex64)

    # Center-trim to exactly SAMPLES_PER_BURST. With PAD_SYM zero
    # guard symbols on each side, the true symbol-0 sample sits at
    # offset ~ PAD_SYM*SPS + filter_group_delay ≈ center of the output.
    if cf.size >= SAMPLES_PER_BURST:
        offset = (cf.size - SAMPLES_PER_BURST) // 2
        cf = cf[offset : offset + SAMPLES_PER_BURST]
    else:
        cf = np.concatenate([cf, np.zeros(SAMPLES_PER_BURST - cf.size,
                                          dtype=np.complex64)])

    # Scale to Q15 (signed int16). gmsk_mod output magnitude is ~1.0.
    iq = np.empty(SAMPLES_PER_BURST * 2, dtype=np.int16)
    iq[0::2] = np.clip(cf.real * 32767.0, -32768, 32767).astype(np.int16)
    iq[1::2] = np.clip(cf.imag * 32767.0, -32768, 32767).astype(np.int16)
    return iq.tobytes()


if __name__ == "__main__":
    # Quick self-test: modulate an all-zero burst (FCCH-like) and print stats.
    out = modulate_burst(b'\x00' * 148)
    print(f"FCCH burst: {len(out)} bytes")
    samp = np.frombuffer(out, dtype=np.int16).reshape(-1, 2)
    print(f"  samples: {samp.shape[0]} complex (I,Q)")
    print(f"  I range: {samp[:,0].min()}..{samp[:,0].max()}")
    print(f"  Q range: {samp[:,1].min()}..{samp[:,1].max()}")
    print(f"  |z| mean: {np.mean(np.hypot(samp[:,0], samp[:,1])):.0f}")

    # Random data burst
    import os
    rb = bytes(b & 1 for b in os.urandom(148))
    out2 = modulate_burst(rb)
    samp2 = np.frombuffer(out2, dtype=np.int16).reshape(-1, 2)
    print(f"random burst: {samp2.shape[0]} samples, |z| mean "
          f"{np.mean(np.hypot(samp2[:,0], samp2[:,1])):.0f}")

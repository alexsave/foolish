#!/usr/bin/env python3
"""Test for vocalset_extract.py.

Synthesises a deterministic 9 s mono 44.1 kHz file: three sustained harmonic tones with 5.5 Hz vibrato
(+-25 cents) at pitches 20 to 30 cents off equal temperament, 0.5 s gaps, and a 0.9 s blip that is below
the 1.2 s minimum length. Runs the extractor on it and asserts:
  1. exactly three segments (the blip is rejected),
  2. the right MIDI notes,
  3. start and end within 60 ms of the truth,
  4. the vibrato is found near 5.5 Hz,
  5. each written wav is 48 kHz 24-bit mono and its measured f0 is within 5 cents of equal temperament,
  6. each written wav has RMS -20 dBFS (forte) and a peak at or below -1 dBFS,
  7. a spiky segment is gain-reduced to a -1 dBFS peak and flagged.
Run: python3 -I test_vocalset_extract.py
The extractor path can be overridden with VOCALSET_EXTRACT_PATH (used for the mutation checks).
"""
import importlib.util
import os
import struct
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
path = os.environ.get("VOCALSET_EXTRACT_PATH", os.path.join(HERE, "vocalset_extract.py"))
spec = importlib.util.spec_from_file_location("vocalset_extract", path)
ve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ve)

SR = 44100
# (start s, end s, midi, cents off equal temperament)
TONES = [(0.3, 2.3, 45, +25.0), (2.8, 4.8, 52, -22.0), (5.3, 7.3, 57, +28.0)]
BLIP = (7.8, 8.7, 50, +10.0)
VIB_HZ, VIB_CENTS = 5.5, 25.0


def synth():
    x = np.zeros(9 * SR)
    for (s, e, m, c) in TONES + [BLIP]:
        n = int(round((e - s) * SR))
        t = np.arange(n) / SR
        f = 440.0 * 2.0 ** ((m - 69 + c / 100.0) / 12.0) * 2.0 ** (VIB_CENTS * np.sin(2 * np.pi * VIB_HZ * t) / 1200.0)
        ph = 2 * np.pi * np.cumsum(f) / SR
        y = sum(np.sin(k * ph) / k for k in range(1, 7))
        env = np.minimum(1.0, np.minimum(t / 0.02, (t[-1] - t) / 0.02))
        x[int(round(s * SR)):int(round(s * SR)) + n] = 0.3 * y * env
    return x


def write16(p, x):
    v = np.round(x * 32767).astype("<i2").tobytes()
    with open(p, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(v)) + b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16)
                + b"data" + struct.pack("<I", len(v)) + v)


def measure_f0(y, sr, m_expected):
    """Independent estimate: maximise summed harmonic magnitude of a long FFT over a +-100 cent grid."""
    y = y[int(0.1 * sr):int(-0.1 * sr)]
    n = 1 << 18
    X = np.abs(np.fft.rfft(y * np.hanning(len(y)), n))
    fr = np.fft.rfftfreq(n, 1.0 / sr)
    f_exp = 440.0 * 2.0 ** ((m_expected - 69) / 12.0)
    best, bc = -1, 0
    for c in np.arange(-100, 100.01, 0.5):
        f = f_exp * 2 ** (c / 1200.0)
        s = sum(np.interp(f * k, fr, X) for k in (1, 2, 3))
        if s > best:
            best, bc = s, c
    return bc


def main():
    d = tempfile.mkdtemp()
    src = os.path.join(d, "t.wav")
    write16(src, synth())
    tsv = os.path.join(d, "rows.tsv")
    rc = ve.main(["--file", src, "--singer", "test", "--exercise", "long_tones", "--technique", "forte",
                  "--vowel", "a", "--out", d, "--tsv", tsv])
    assert rc == 0, "extractor failed"
    lines = open(tsv).read().strip().split("\n")
    cols = lines[0].split("\t")
    rows = [dict(zip(cols, l.split("\t"))) for l in lines[1:]]
    assert len(rows) == 3, "expected 3 segments, got %d" % len(rows)
    for r, (s, e, m, c) in zip(rows, TONES):
        assert int(r["midi"]) == m, "midi: expected %d got %s" % (m, r["midi"])
        assert abs(float(r["start"]) - s) <= 0.06, "start: expected %.3f got %s" % (s, r["start"])
        assert abs(float(r["end"]) - e) <= 0.06, "end: expected %.3f got %s" % (e, r["end"])
        assert abs(float(r["vib_rate_hz"]) - VIB_HZ) <= 0.5, "vibrato rate got %s" % r["vib_rate_hz"]
        assert abs(float(r["correction_cents"]) + c) <= 5.0, "correction: expected %.1f got %s" % (-c, r["correction_cents"])
        y, sr = ve.read_wav(os.path.join(d, "test", r["wav"]))
        assert sr == 48000, "sample rate %d" % sr
        got = measure_f0(y, sr, m)
        assert abs(got) <= 5.0, "written wav f0 is %.1f cents from equal temperament (midi %d)" % (got, m)
        rms = 20 * np.log10(np.sqrt(np.mean(y[int(0.03 * sr):] ** 2)))
        assert abs(rms - -20.0) <= 1.0, "rms %.2f dBFS" % rms
        assert 20 * np.log10(np.max(np.abs(y))) <= -1.0 + 1e-3, "peak over -1 dBFS"
    # peak limiter: a spiky segment must be gain-reduced to a -1 dBFS peak and flagged
    t = np.arange(SR) / SR
    sp = 0.01 * np.sin(2 * np.pi * 220 * t)
    sp[20000::4000] = 0.9
    seg = ve.classify(dict(start=0.1, end=0.9, prev_end=0.0, f0=220.0, sd=0.0, vib_rate=0.0, vib_depth=0.0, rms_db=0.0))
    z, info = ve.render(sp, SR, seg, -20.0)
    assert info["limited"], "spiky segment was not flagged as peak limited"
    assert 20 * np.log10(np.max(np.abs(z))) <= -1.0 + 1e-3, "limiter let the peak pass -1 dBFS"
    print("PASS: 3 segments, notes %s, f0 within 5 cents, rms -20 dBFS" % [int(r["midi"]) for r in rows])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Test for lyria_notes_extract.py.

Synthesises a deterministic stereo 44.1 kHz clip (one numpy default_rng(SEED), used only for a -80 dB noise bed) with
  five clean harmonic notes: midi 57; midi 60 played 30 cents sharp; midi 64 with 6 Hz vibrato of 20 cents depth; midi 67; midi 52,
  (b) a two-note chord, a fifth (midi 62 + 69),
  (c) a note (midi 65) with a second attack 0.7 s in, same pitch,
  (d) a note (midi 69) plus an inharmonic second instrument 20 dB down, an octave and a tritone below,
  (e) a glide of 400 cents over 1.2 s (f0 stability),
  (f) a 0.4 s blip (length),
  (g) a harmonic note with band noise at purity 0.93 (accepted at 0.90, rejected for purity at a 0.95 limit).
Asserts:
  1. segment count and every boundary within 20 ms of the truth,
  2. the measured notes,
  3. the sharp note's offset within 3 cents of +30, the vibrato rate near 6 Hz and depth near 20 cents,
  4. the five clean notes are accepted with purity over 0.95, the noisy one is accepted with purity between 0.90 and 0.95; (b) for pitch only, a noisy note for purity only, (c) for second_onset only,
     (d) for pitch only, (e) for stability, (f) for length,
  5. the sharp note's cut sample, re-measured by an independent harmonic-comb estimator, is within 3 cents of equal temperament,
  6. the cut starts 8 ms before the onset (the window starts there, the first 8 ms are noise only) and holds no part of the next note,
  7. the manifest roots are exactly the accepted notes, one layer 0 rr 0 each, with the /SAMPLES/soundtrack/samples/lyria path,
  8. two samples of one root get rr 0 and 1.
Run: python3 -I test_lyria_notes_extract.py
The extractor path can be overridden with LYRIA_EXTRACT_PATH (used for the mutation checks).
"""
import importlib.util
import os
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
path = os.environ.get("LYRIA_EXTRACT_PATH", os.path.join(HERE, "lyria_notes_extract.py"))
spec = importlib.util.spec_from_file_location("lyria_notes_extract", path)
le = importlib.util.module_from_spec(spec)
spec.loader.exec_module(le)

SEED = 20260515
SR = 44100
NOISE_DB = -80.0
# kind, start s, end s, midi, cents
NOTES = [("clean", 0.5, 1.7, 57, 0.0), ("clean", 2.7, 3.9, 60, 30.0), ("vib", 4.9, 6.4, 64, 0.0),
         ("clean", 7.4, 8.4, 67, 0.0), ("clean", 9.4, 10.4, 52, 0.0),
         ("chord", 11.4, 12.6, 62, 0.0), ("reattack", 13.6, 15.0, 65, 0.0), ("inharm", 16.0, 17.2, 69, 0.0),
         ("glide", 18.2, 19.4, 60, 0.0), ("blip", 20.4, 20.8, 55, 0.0), ("noisy", 21.8, 23.0, 62, 0.0)]
TOTAL = 24.5
EXPECT_REASONS = {"clean": [], "vib": [], "chord": {"pitch"}, "noisy": [], "reattack": {"second_onset"}, "inharm": {"pitch"},
                  "glide": {"stability"}, "blip": {"length"}}
NHARM = 8


def hz(midi, cents=0.0):
    return 440.0 * 2.0 ** ((midi - 69 + cents / 100.0) / 12.0)


def tone(f, n, nh=NHARM):
    ph = 2 * np.pi * np.cumsum(f * np.ones(n)) / SR
    return sum(np.sin(k * ph) / k for k in range(1, nh + 1))


def synth():
    rng = np.random.default_rng(SEED)
    x = np.zeros(int(TOTAL * SR))
    truth = []
    for kind, s, e, m, c in NOTES:
        n = int(round((e - s) * SR))
        t = np.arange(n) / SR
        if kind == "vib":
            f = hz(m, c) * 2.0 ** (20.0 * np.sin(2 * np.pi * 6.0 * t) / 1200.0)
        elif kind == "glide":
            f = hz(m, c) * 2.0 ** (400.0 * t / t[-1] / 1200.0)
        else:
            f = hz(m, c) * np.ones(n)
        y = tone(f, n)
        if kind == "chord":
            y = y + tone(hz(69) * np.ones(n), n)
        if kind == "inharm":
            f2 = hz(m) / 2.0 / 2.0 ** 0.5
            y = y + 0.1 * np.sin(2 * np.pi * f2 * t) + 0.05 * np.sin(2 * np.pi * 2.13 * f2 * t) + 0.025 * np.sin(2 * np.pi * 3.31 * f2 * t)
        if kind == "noisy":
            nz = np.fft.rfft(rng.standard_normal(n))
            fr = np.fft.rfftfreq(n, 1.0 / SR)
            nz = np.fft.irfft(nz * ((fr >= 100.0) & (fr <= 7500.0)), n)
            y = y / np.std(y) + 0.37 * nz / np.std(nz)
        env = np.minimum(1.0, np.minimum(t / 0.012, (t[-1] - t) / 0.02))
        if kind == "reattack":
            t0 = 0.7
            dip = np.where((t >= t0) & (t < t0 + 0.02), 1.0 - 0.75 * (t - t0) / 0.02, 1.0)
            rise = np.where((t >= t0 + 0.02) & (t < t0 + 0.025), 0.25 + 0.75 * (t - t0 - 0.02) / 0.005, 1.0)
            env = env * np.where(t < t0 + 0.02, dip, rise)
        x[int(round(s * SR)):int(round(s * SR)) + n] += 0.12 * y * env
        truth.append((s, e, m, c, kind))
    x += rng.standard_normal(len(x)) * 10.0 ** (NOISE_DB / 20.0)
    st = np.stack([x, 0.8 * x], axis=1)
    return st, truth


def comb_f0(y, sr, midi):
    """Independent f0: maximise the summed harmonic magnitude of a long FFT over a +-100 cent grid. Returns cents off equal temperament."""
    y = y[int(0.1 * sr):int(len(y) - 0.2 * sr)]
    nfft = 1 << 18
    X = np.abs(np.fft.rfft(y * np.hanning(len(y)), nfft))
    base = hz(midi)
    best, bc = -1, 0
    for c in np.arange(-100, 100.5, 0.5):
        f0 = base * 2 ** (c / 1200.0)
        s = sum(X[int(round(k * f0 * nfft / sr))] / k for k in range(1, 6))
        if s > best:
            best, bc = s, c
    return bc


def check(cond, msg):
    if not cond:
        print("FAIL: " + msg)
        sys.exit(1)


def main():
    st, truth = synth()
    with tempfile.TemporaryDirectory() as d:
        clip = os.path.join(d, "clip.wav")
        wavfile.write(clip, SR, (st * 32767).astype("<i2"))
        outdir = os.path.join(d, "out")
        man = os.path.join(d, "m", "test.txt")
        log = os.path.join(d, "log.tsv")
        rep = os.path.join(d, "rep.md")
        rows = le.run(clip, "test", False, outdir, man, log, rep, [], None)

        # 1 segments
        check(len(rows) == len(truth), "segment count: expected %d got %d" % (len(truth), len(rows)))
        for r, (s, e, m, c, kind) in zip(rows, truth):
            check(abs(r["start"] - s) <= 0.02, "start %s: expected %.3f got %.3f" % (kind, s, r["start"]))
            check(abs(r["end"] - e) <= 0.02, "end %s: expected %.3f got %.3f" % (kind, e, r["end"]))
        # 2 notes (glide and blip excluded from the exact check of a moving pitch, the blip is tested too)
        for r, (s, e, m, c, kind) in zip(rows, truth):
            if kind in ("clean", "vib", "reattack", "inharm", "blip"):
                check(r["midi"] == m, "midi %s: expected %d got %d" % (kind, m, r["midi"]))
        # 3 offset and vibrato
        sharp = rows[1]
        check(abs(sharp["cents"] - 30.0) <= 3.0, "sharp note offset: expected 30 +-3 got %.2f" % sharp["cents"])
        v = rows[2]
        check(abs(v["vib_rate"] - 6.0) <= 0.8, "vibrato rate: expected 6 got %.2f" % v["vib_rate"])
        check(abs(v["vib_depth"] - 20.0) <= 6.0, "vibrato depth: expected 20 got %.2f" % v["vib_depth"])
        # 4 verdicts
        for i, (r, (s, e, m, c, kind)) in enumerate(zip(rows, truth)):
            want = EXPECT_REASONS[kind]
            got = set(r["reasons"])
            if kind in ("clean", "vib", "noisy"):
                check(not got, "note %d (%s) rejected for %s" % (i, kind, sorted(got)))
                if kind != "noisy":
                    check(r["purity"] > 0.95, "clean note %d purity %.3f" % (i, r["purity"]))
            elif kind in ("reattack", "inharm", "chord"):
                check(got == want, "%s: expected reasons %s got %s" % (kind, sorted(want), sorted(got)))
            else:
                check(want <= got, "%s: expected reasons %s in %s" % (kind, sorted(want), sorted(got)))
        check(rows[5]["pitches"] == 2, "chord pitches: expected 2 got %d" % rows[5]["pitches"])
        check(rows[7]["pitches"] == 2, "inharmonic second instrument pitches: expected 2 got %d" % rows[7]["pitches"])
        check(0.90 < rows[10]["purity"] < 0.95, "noisy note purity %.3f, expected between 0.90 and 0.95" % rows[10]["purity"])
        strict, _, _, _ = le.analyse(st, SR, 0.6, min_purity=0.95)
        check(set(strict[10]["reasons"]) == {"purity"}, "noisy note at purity limit 0.95: reasons %s, expected only purity" % strict[10]["reasons"])
        check(not strict[0]["reasons"], "clean note rejected at purity limit 0.95: %s" % strict[0]["reasons"])
        # 5 cut sample pitch
        files = sorted(os.listdir(outdir))
        want_files = ["test_%s_%d.wav" % (le.note_name(m), m) for (_, _, m, _, k) in truth if k in ("clean", "vib", "noisy")]
        check(files == sorted(want_files), "written files: expected %s got %s" % (sorted(want_files), files))
        import struct
        def read24(p):
            b = open(p, "rb").read()
            ch, rate = struct.unpack("<HI", b[22:24] + b[24:28])
            raw = np.frombuffer(b[44:], dtype=np.uint8)
            raw = raw[:len(raw) // 3 * 3].reshape(-1, 3).astype(np.int32)
            v = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
            v = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
            return v.reshape(-1, ch), rate
        y, rate = read24(os.path.join(outdir, "test_C4_60.wav"))
        check(rate == SR and y.shape[1] == 2, "sample format: rate %d channels %d" % (rate, y.shape[1]))
        err = comb_f0(y.mean(axis=1), rate, 60)
        check(abs(err) <= 3.0, "sharp note re-measured: %.1f cents from equal temperament (before: 30)" % err)
        # 6 cut window
        info = le.cut(st, SR, rows[1], *fl_args(le, st))[1]
        check(abs(info["a0"] - int(round((truth[1][0] - 0.008) * SR))) <= int(0.01 * SR) + 1,
              "cut start: expected 8 ms before %.3f, got %.4f s" % (truth[1][0], info["a0"] / SR))
        check(info["a1"] <= int(truth[2][0] * SR), "cut end %.3f s runs into the next note at %.3f s" % (info["a1"] / SR, truth[2][0]))
        pre = st[info["a0"]:info["a0"] + int(0.006 * SR)].mean(axis=1)
        check(np.sqrt(np.mean(pre ** 2)) < 0.002, "the first 6 ms of the cut are not silent: %.4f" % np.sqrt(np.mean(pre ** 2)))
        onset = np.argmax(np.abs(st[info["a0"]:, 0]) > 0.02) / SR
        check(0.004 <= onset <= 0.030, "onset inside the cut at %.1f ms, expected a few ms after the start" % (onset * 1000))
        # 7 manifest
        lines = open(man).read().split("\n")[:-1]
        roots = [int(l.split()[0]) for l in lines]
        want_roots = sorted(m for (_, _, m, _, k) in truth if k in ("clean", "vib", "noisy"))
        check(roots == want_roots, "manifest roots: expected %s got %s" % (want_roots, roots))
        for l, m in zip(lines, want_roots):
            p = l.split()
            check(p[1] == "0" and p[2] == "0", "manifest layer/rr: %s" % l)
            check(p[3] == "/SAMPLES/soundtrack/samples/lyria/test/test_%s_%d.wav" % (le.note_name(m), m), "manifest path: %s" % l)
        check(len(open(log).read().split("\n")[:-1]) == 6, "log rows: expected 6")
        check(os.path.getsize(rep) > 0, "report empty")
        # 8 round robin: the same clip twice with the same root gives rr 0 and 1
        st2 = np.concatenate([st[:int(2.2 * SR)], np.zeros((int(0.5 * SR), 2)), st[:int(2.2 * SR)]])
        clip2 = os.path.join(d, "clip2.wav")
        wavfile.write(clip2, SR, (st2 * 32767).astype("<i2"))
        le.run(clip2, "rr", False, os.path.join(d, "o2"), os.path.join(d, "m2.txt"), os.path.join(d, "l2.tsv"), os.path.join(d, "r2.md"), [], None)
        l2 = [l.split()[:3] for l in open(os.path.join(d, "m2.txt")).read().split("\n")[:-1]]
        check(l2 == [["57", "0", "0"], ["57", "0", "1"]], "round robin: %s" % l2)
    print("PASS: %d segments, 6 accepted, all assertions hold" % len(rows))


def fl_args(le, st):
    """db and floor of the test clip, the way the extractor derives them."""
    m = st.mean(axis=1)
    _, floor, _, db, _ = le.segment(m, SR)
    return db, floor


if __name__ == "__main__":
    main()

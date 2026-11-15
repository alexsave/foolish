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


    X = np.abs(np.fft.rfft(y * np.hanning(len(y)), nfft))
    base = hz(midi)
    best, bc = -1, 0
    for c in np.arange(-100, 100.5, 0.5):
        f0 = base * 2 ** (c / 1200.0)
        s = sum(X[int(round(k * f0 * nfft / sr))] / k for k in range(1, 6))
        if s > best:
            best, bc = s, c
    return bc


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
    test_modes()
    print("PASS: %d segments, 6 accepted, runs and onsets paths hold, all assertions hold" % len(rows))


# ---------------------------------------------------------------- runs and onsets modes
# legato clip: (kind, start s, end s, midi, cents). A note that follows another with no gap glides into it for 80 ms.
RUNS = [("lega", 1.0, 2.0, 57, 0.0, "natural"), ("lega", 2.0, 3.0, 60, 30.0, "legato"), ("vib", 3.0, 4.0, 64, 0.0, "legato"),
        ("dying", 5.0, 6.2, 67, 0.0, "natural"), ("chord", 7.2, 8.4, 62, 0.0, "natural"), ("reattack", 9.4, 10.6, 65, 0.0, "natural"),
        ("lega", 11.6, 12.3, 55, 0.0, "natural"), ("lega", 12.3, 13.0, 56, 0.0, "legato"), ("blip", 14.0, 14.4, 70, 0.0, "natural")]
# plucked clip: (kind, onset s, midi)
PLUCKS = [("pluck", 1.0, 52), ("pluck", 2.0, 59), ("chord", 3.5, 52), ("enter", 5.0, 57), ("short", 6.5, 64), ("pluck", 8.0, 55)]


def synth_runs():
    rng = np.random.default_rng(SEED + 1)
    x = np.zeros(int(16 * SR))
    prev_end, prev_f = None, None
    for kind, s0, e0, m, c, att in RUNS:
        n = int(round((e0 - s0) * SR))
        t = np.arange(n) / SR
        f = hz(m, c) * np.ones(n)
        if kind == "vib":
            f = f * 2.0 ** (15.0 * np.sin(2 * np.pi * 6.0 * t) / 1200.0)
        if att == "legato":   # glide from the previous note over the first 80 ms
            g = np.clip(t / 0.08, 0, 1)
            f = prev_f * (f[0] / prev_f) ** g
        prev_f = hz(m, c)
        y = tone(f, n)
        if kind == "chord":
            y = y + tone(hz(69) * np.ones(n), n)
        env = np.ones(n)
        if att == "natural":
            env = np.minimum(env, t / 0.012)
        if kind == "dying":
            env = env * 10.0 ** (-18.0 * t / t[-1] / 20.0)
        if kind == "reattack":
            t0 = 0.6
            dip = np.where((t >= t0) & (t < t0 + 0.02), 1.0 - 0.75 * (t - t0) / 0.02, 1.0)
            rise = np.where((t >= t0 + 0.02) & (t < t0 + 0.025), 0.25 + 0.75 * (t - t0 - 0.02) / 0.005, 1.0)
            env = env * np.where(t < t0 + 0.02, dip, rise)
        if e0 not in [r[1] for r in RUNS if r[5] == "legato"]:
            env = env * np.minimum(1.0, (t[-1] - t) / 0.02)
        x[int(round(s0 * SR)):int(round(s0 * SR)) + n] += 0.12 * y * env
    x += rng.standard_normal(len(x)) * 10.0 ** (NOISE_DB / 20.0)
    return np.stack([x, 0.8 * x], axis=1)


def synth_plucks():
    rng = np.random.default_rng(SEED + 2)
    x = np.zeros(int(10 * SR))
    for kind, o, m in PLUCKS:
        n = int(1.4 * SR)
        t = np.arange(n) / SR
        y = tone(hz(m) * np.ones(n), n)
        if kind == "chord":
            y = y + tone(hz(m + 7) * np.ones(n), n)
        tau = {"short": 0.03, "pluck": 0.25}.get(kind, 0.5)
        env = np.exp(-t / tau) * np.minimum(1.0, t / 0.003)
        if kind == "enter":   # a second tone fades in slowly from 60 ms, louder than the decayed pluck by 250 ms
            y = y * env + 3.0 * np.minimum(1.0, np.maximum(0.0, (t - 0.06) / 0.2)) * np.sin(2 * np.pi * hz(m + 6.3) * t) * np.exp(-t / 1.0)
        else:
            y = y * env
        x[int(o * SR):int(o * SR) + n] += 0.12 * y
    x += rng.standard_normal(len(x)) * 10.0 ** (NOISE_DB / 20.0)
    return np.stack([x, 0.8 * x], axis=1)


def test_modes():
    d_ = tempfile.mkdtemp()
    clip = os.path.join(d_, "legato.wav")
    wavfile.write(clip, SR, (synth_runs() * 32767).astype("<i2"))
    out, man, log, rep = os.path.join(d_, "o"), os.path.join(d_, "m.txt"), os.path.join(d_, "l.tsv"), os.path.join(d_, "r.md")
    allc, keep, on = le.run_modes([clip], "leg", "runs", False, out, man, log, rep, {}, [0], {})
    # 9 notes: the two joined first notes and the vibrato note, plus six separate ones; the legato 56 follows the 55
    check(len(allc) == 9, "runs: expected 9 candidates got %d" % len(allc))
    want = [([], "natural"), ([], "legato"), ([], "legato"), (["level"], "natural"), (["pitch"], "natural"),
            (["second_onset"], "natural"), ([], "natural"), ([], "legato"), (["length"], "natural")]
    for i, (c, (r, att)) in enumerate(zip(allc, want)):
        check(c["reasons"] == r, "run %d: expected reasons %s got %s" % (i, r, c["reasons"]))
        check((c["attack"] == "natural") == (att == "natural"), "run %d: expected attack %s got %s" % (i, att, c["attack"]))
    for i, (kind, s0, e0, m, cc, att) in enumerate(RUNS):
        c = allc[i]
        check(c["midi"] == m or kind == "chord", "run %d: midi expected %d got %d" % (i, m, c["midi"]))
        check(c["a"] >= s0 + (0.08 if att == "legato" else 0.0) - 0.005, "run %d: region starts %.3f, inside the glide that ends %.3f" % (i, c["a"], s0 + 0.08))
        check(c["b"] <= e0 + 0.005, "run %d: region ends %.3f after the note ends %.3f" % (i, c["b"], e0))
    check(abs(allc[1]["cents"] - 30.0) <= 3.0, "run 1 offset: expected 30 got %.1f" % allc[1]["cents"])
    # the semitone step: 55 and 56 are two runs, not one
    check(allc[6]["midi"] == 55 and allc[7]["midi"] == 56, "semitone step merged into one run")
    # cut windows and fades
    check(abs(allc[0]["cut_a"] - (1.0 - 0.008)) <= 0.02, "natural cut start %.3f, expected about 0.992" % allc[0]["cut_a"])
    check(abs(allc[1]["cut_a"] - allc[1]["a"]) < 1e-9, "legato cut starts at the region start")
    check(abs(allc[1]["fade_in"] - 0.012) < 1e-9 and abs(allc[0]["fade_in"] - 0.003) < 1e-9, "fade lengths")
    check(len(keep) == 5, "kept: expected 5 got %d" % len(keep))
    check(not on, "one layer expected")
    import struct
    def read24(p):
        b = open(p, "rb").read()
        ch = struct.unpack("<H", b[22:24])[0]
        raw = np.frombuffer(b[44:], dtype=np.uint8)
        raw = raw[:len(raw) // 3 * 3].reshape(-1, 3).astype(np.int32)
        v = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        return (np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)).reshape(-1, ch)
    files = sorted(os.listdir(out))
    check(files == sorted("leg_%s_%d.wav" % (le.note_name(m), m) for m in (55, 56, 57, 60, 64)), "files: %s" % files)
    y = read24(os.path.join(out, "leg_C4_60.wav")).mean(axis=1)
    err = comb_f0(y, SR, 60)
    check(abs(err) <= 3.0, "legato sharp note re-measured: %.1f cents (before 30)" % err)
    r0, r1 = np.sqrt(np.mean(y[:int(0.003 * SR)] ** 2)), np.sqrt(np.mean(y[int(0.012 * SR):int(0.024 * SR)] ** 2))
    check(r0 < 0.3 * r1, "legato sample has no 12 ms fade in: rms %.4f vs %.4f" % (r0, r1))
    log_rows = [l.split("\t") for l in open(log).read().split("\n")[:-1]]
    check(len(log_rows) == 5 and all(len(r) == 9 for r in log_rows), "log rows: 5 rows of 9 columns")
    check(sorted(r[7] for r in log_rows).count("natural") == 2, "log attack column")
    log2 = os.path.join(d_, "l2.tsv")
    le.run_modes([clip], "off", "runs", False, os.path.join(d_, "o2"), os.path.join(d_, "m2.txt"), log2, os.path.join(d_, "r2.md"), {}, [0],
                 {clip: "Demucs other stem of xx (second half only)"}, {clip: 10.0})
    r2 = [l.split("\t") for l in open(log2).read().split("\n")[:-1]]
    w0 = sorted(float(r[2].split("-")[0]) for r in r2)[0]
    check(abs(w0 - (10.0 + 0.992)) <= 0.02, "logged window start %.3f, expected the cut start plus the 10 s offset" % w0)
    check(all(r[1] == "Demucs other stem of xx (second half only)" for r in r2), "log source label")
    # two layers from the same range (7 semitones, under an octave): falls back to one layer with the sentence
    allc2, keep2, on2 = le.run_modes([clip, clip], "leg2", "runs", True, out, man, log, rep, {}, [0, 1], {})
    check(not on2 and "one layer" in open(rep).read(), "layer fallback not applied")
    check(all(c["layer"] == 1 for c in keep2), "fallback keeps layer 1 sources only")
    # select_best: natural first, then purity, then length, at most two per root
    cs = [dict(reasons=[], layer=0, midi=60, attack="none (legato)", purity=0.99, length=1.0, n=1),
          dict(reasons=[], layer=0, midi=60, attack="natural", purity=0.95, length=0.5, n=2),
          dict(reasons=[], layer=0, midi=60, attack="none (legato)", purity=0.995, length=2.0, n=3),
          dict(reasons=[], layer=0, midi=60, attack="none (legato)", purity=0.97, length=3.0, n=4)]
    got = [c["n"] for c in le.select_best(cs, False)]
    check(got == [2, 3], "select_best: expected [2, 3] got %s" % got)
    check(le.layer_problem([dict(layer=L, midi=m) for L in (0, 1) for m in (55, 59, 63, 67)]) == "", "layer_problem false positive")
    check("gap of 8" in le.layer_problem([dict(layer=0, midi=m) for m in (55, 60, 68, 72)] + [dict(layer=1, midi=60)]), "layer_problem gap")
    good = dict(pitches=1, purity=0.99, onset2=0.0, qspread=2.0, sd=10.0, length=1.0, f0=200.0)
    check(le.run_reasons(good) == [], "run_reasons: a good run is rejected")
    check(le.run_reasons(dict(good, sd=36.0)) == ["stability"], "run_reasons: sd 36 over the whole run must give stability")
    # plucks
    clip2 = os.path.join(d_, "pl.wav")
    wavfile.write(clip2, SR, (synth_plucks() * 32767).astype("<i2"))
    allp, keepp, _ = le.run_modes([clip2], "pl", "onsets", False, os.path.join(d_, "po"), os.path.join(d_, "pm.txt"), log, os.path.join(d_, "pr.md"), {}, [0], {})
    check(len(allp) == 6, "onsets: expected 6 candidates got %d" % len(allp))
    for c, (kind, o, m) in zip(allp, PLUCKS):
        check(abs(c["onset"] - o) <= 0.006, "onset %s: expected %.2f got %.3f" % (kind, o, c["onset"]))
        check(abs(c["cut_a"] - (o - 0.008)) <= 0.012, "pluck cut start %.3f vs onset %.2f" % (c["cut_a"], o))
    wantp = [[], [], ["pitch"], ["entered"], ["length"], []]
    for i, (c, w) in enumerate(zip(allp, wantp)):
        check(set(w) <= set(c["reasons"]) and (w or not c["reasons"]), "pluck %d: expected %s got %s" % (i, w, c["reasons"]))
    check(abs(allp[0]["cut_b"] - 1.98) <= 0.012, "pluck cut end %.3f, expected the next onset minus 20 ms" % allp[0]["cut_b"])
    check([c["midi"] for c in keepp] == [52, 55, 59], "plucks kept: %s" % [c["midi"] for c in keepp])


def fl_args(le, st):
    """db and floor of the test clip, the way the extractor derives them."""
    m = st.mean(axis=1)
    _, floor, _, db, _ = le.segment(m, SR)
    return db, floor


if __name__ == "__main__":
    main()

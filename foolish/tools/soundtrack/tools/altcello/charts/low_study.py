"""One-factor study of the low strings (ITERATION_LOG.md, lowstrings iteration 1): C2 open and G2 stopped on the C string.
For one setting of the knobs (knobs.h, ALTCELLO_KNOBS) it measures
  band     sweep_tool ours at beta 0.1 (re the sounding length), v_B 0.1, 32 forces 0.1-8 N, 6 s: the contiguous Helmholtz band (both classifiers)
  guettler onset_tool grid (the grid of charts/onset: 12 forces over the baked playability band x2 each side, accelerations 0.25 0.5 1 2 4 8):
           cells at steady Helmholtz motion with the audio time to periodic at most 0.1 s
  note     the expert player (scores/low_c_string.txt, 3 seeds): audio time to periodic and HNR (median), and the 1-3 kHz share against Philharmonia
           D2 (MIDI 38, for C2) and G2 (MIDI 43)

    python -I charts/low_study.py NAME "KNOBS" [parts: band guettler note]       -> out/low_study/NAME.txt
"""
import os, re, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out", "low_study")
STR = [("C2", 0, 36, 38), ("C2", 7, 43, 43)]  # string, semitones, our MIDI, Philharmonia MIDI
LABEL = {0: "C2 open", 7: "G2 on C"}
BANDS = [(100, 300), (300, 1000), (1000, 3000), (3000, 8000)]


def run(cmd, knobs):
    env = dict(os.environ); env["ALTCELLO_KNOBS"] = knobs
    return subprocess.run(["nice"] + cmd, capture_output=True, text=True, check=True, env=env, cwd=ROOT).stdout


def longest(F, ok):
    best, i = (0, -1), 0
    while i < len(F):
        if ok[i]:
            j = i
            while j + 1 < len(F) and ok[j + 1]: j += 1
            if j - i + 1 > best[0]: best = (j - i + 1, i)
            i = j + 1
        else: i += 1
    if best[0] == 0: return dict(n=0, lo=0.0, hi=0.0, ratio=0.0)
    lo, hi = F[best[1]], F[best[1] + best[0] - 1]
    return dict(n=best[0], lo=lo, hi=hi, ratio=hi / lo)


def band(knobs, semis, tag, fmin=0.1, fmax=8.0, nf=32, secs=6.0):
    path = os.path.join(OUT, "band_%s.txt" % tag)
    cmd = [os.path.join(ROOT, "sweep_tool"), "ours", "C2", "0.1", str(fmin), str(fmax), str(nf), str(secs), path, "--vb", "0.1", "--body"]
    if semis: cmd += ["--semis", str(semis)]
    run(cmd, knobs)
    rows = [l.split() for l in open(path) if not l.startswith("#") and l.strip()]
    r = longest([float(x[0]) for x in rows], [int(x[1]) == 1 and int(x[2]) == 1 for x in rows])
    r["npts"] = sum(int(x[1]) == 1 and int(x[2]) == 1 for x in rows)
    return r


def guettler(knobs, semis, tag):
    path = os.path.join(OUT, "grid_%s.txt" % tag)
    run([os.path.join(ROOT, "onset_tool"), "grid", "C2", str(semis), "ours", path], knobs)
    rows = [l.split() for l in open(path) if not l.startswith("#") and l.strip()]
    ta = [float(r[2]) for r in rows]; st = [int(r[6]) for r in rows]
    fast = [i for i in range(len(rows)) if st[i] == 1 and 0 <= ta[i] <= 0.1]
    steady = sum(st)
    return dict(cells=len(rows), fast=len(fast), steady=steady, best=min([ta[i] for i in range(len(rows)) if st[i] == 1 and ta[i] >= 0], default=-1.0))


def band_shares(y, sr, t0, t1):
    seg = y[int(t0 * sr):int(t1 * sr)]
    n = 1 << 15
    P = np.zeros(n // 2 + 1)
    w = np.hanning(n)
    for a in range(0, len(seg) - n, n // 2):
        P += np.abs(np.fft.rfft(seg[a:a + n] * w)) ** 2
    f = np.fft.rfftfreq(n, 1 / sr)
    tot = P[(f >= 50) & (f < 16000)].sum()
    return [10 * np.log10(P[(f >= lo) & (f < hi)].sum() / tot) for lo, hi in BANDS]


def note(knobs, tag, seeds=(0, 1, 2)):
    import onset_lib as L
    import player_gates as G
    M = L.M
    os.environ["ALTCELLO_KNOBS"] = knobs
    ys = {}
    for sd in seeds:
        w = os.path.join(L.TMP, "ls_%s_%d.wav" % (tag, sd))
        L.render("low_c_string.txt", w, extra=["--seed", str(sd)])
        ys[sd] = M.load_audio(w)
        os.remove(w)
    res = {}
    for k, (s, semis, midi, pm) in enumerate(STR):
        t0 = 0.5 if k == 0 else 3.5
        tper, hnr, sh = [], [], []
        for sd in seeds:
            seg, ts = L.note_segment(ys[sd], t0, pre=0.2, post=1.7)
            on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=ts, tag="%s%d%d" % (tag, k, sd))
            g = G.gates_of(seg, midi, "ls_%s_%d_%d" % (tag, k, sd))
            tper.append(tp); hnr.append(g["hnr"])
            sh.append(band_shares(seg, L.SR, on + 0.5, on + 1.4))
        ref = M.load_audio(G.PHIL_FULL % pm)
        env = M.rms_env(ref); ron = int(np.argmax(env >= 0.02 * env.max())) * 0.01
        rs = band_shares(ref, L.SR, ron + 0.5, ron + 1.4)
        shm = np.median(np.array(sh), axis=0)
        res[LABEL[semis]] = dict(t_per=[float(x) for x in tper], t_per_med=float(np.median([x if x >= 0 else 9.9 for x in tper])), hnr=[float(x) for x in hnr], hnr_med=float(np.median(hnr)),
                                 bands=[float(x) for x in shm], ref_bands=[float(x) for x in rs], diff=[float(a - b) for a, b in zip(shm, rs)])
    return res


def main():
    name, knobs = sys.argv[1], sys.argv[2]
    parts = sys.argv[3:] or ["band", "guettler", "note"]
    os.makedirs(OUT, exist_ok=True)
    f = open(os.path.join(OUT, name + ".txt"), "a")
    def emit(k, v):
        line = "RESULT %s %s %s" % (name, k, repr(v)); print(line, flush=True); f.write(line + "\n"); f.flush()
    emit("knobs", knobs)
    for semis in (0, 7):
        if "band" in parts: emit("band_" + LABEL[semis].replace(" ", "_"), band(knobs, semis, "%s_%d" % (name, semis)))
    for semis in (0, 7):
        if "guettler" in parts: emit("guettler_" + LABEL[semis].replace(" ", "_"), guettler(knobs, semis, "%s_%d" % (name, semis)))
    if "note" in parts: emit("note", note(knobs, name))


if __name__ == "__main__":
    main()

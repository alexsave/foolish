"""Legato transitions of a scale render: for each note after the first of the vibrato-off scale, the time at which the pitch is within 10 cents of
the note's settled pitch (median over 0.30-0.60 s after the written start) and stays so for 5 periods, relative to the written start (ms; negative =
early), and the pitch dip: the lowest pitch below both the previous and the new pitch in the window from 0.1 s before to 0.5 s after the written start.
The pitch track is the C one (onset_tool pitch, analysis.h pitch_track: window 2 periods, hop 0.5 ms).
    python -I charts/transitions.py SCALE.wav [SCALE2.wav ...]   (notes of scores/c_major_scale.txt)"""
import os, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
M = L.M
SR = L.SR


def notes_of(score):
    out = []
    for l in open(score):
        l = l.split("#")[0].split()
        if len(l) >= 4: out.append((float(l[0]), float(l[1]), int(l[2]), int(l[5]) if len(l) > 5 and l[5] != "-" else 1))
    return out


def track(y, flo, fhi, win, t0, t1, tag):
    p = os.path.join(L.TMP, "pt_%s.f32" % tag)
    np.asarray(y, dtype=np.float32).tofile(p)
    r = subprocess.run(["nice", L.TOOL, "pitch", p, str(SR), "%.2f" % flo, "%.2f" % fhi, "%.5f" % win, "%.4f" % t0, "%.4f" % t1, "0.0005"], capture_output=True, text=True, check=True).stdout
    os.remove(p)
    a = np.array([[float(x) for x in l.split()] for l in r.strip().split("\n")])
    return a[:, 0], a[:, 1]


def transitions(wav, score=None, vib=0):
    y = M.load_audio(wav)
    nts = [n for n in notes_of(score or os.path.join(L.SC, "c_major_scale.txt")) if n[3] == vib]
    rows = []
    for k in range(1, len(nts)):
        ts, dur, midi, _ = nts[k]
        fprev, fnew = M.midi_hz(nts[k - 1][2]), M.midi_hz(midi)
        flo, fhi = 0.85 * min(fprev, fnew), 1.15 * max(fprev, fnew)
        t, f = track(y, flo, fhi, 2.0 / fnew, ts - 0.15, ts + 0.65, "tr%d" % k)
        sel = (t > ts + 0.30) & (t < ts + 0.60) & (f > 0)
        fset = float(np.median(f[sel]))
        fp = float(np.median(f[(t > ts - 0.15) & (t < ts - 0.05) & (f > 0)])) if ((t > ts - 0.15) & (t < ts - 0.05)).any() else fprev
        c = np.where(f > 0, 1200 * np.log2(np.maximum(f, 1e-9) / fset), 1e9)
        need = int(np.ceil(5.0 / fset / 0.0005))
        run, tt = 0, np.nan
        for i in range(len(t)):
            if t[i] < ts - 0.05: continue
            run = run + 1 if abs(c[i]) < 10 else 0
            if run >= need: tt = t[i - need + 1] - ts; break
        lo = min(fp, fset)
        w = (t > ts - 0.1) & (t < ts + 0.5) & (f > 0)
        dip = float(min(0.0, np.min(1200 * np.log2(f[w] / lo)))) if w.any() else np.nan
        rows.append((midi, 1000 * tt, dip, 1200 * np.log2(fset / fnew)))
    return rows


if __name__ == "__main__":
    for w in sys.argv[1:]:
        rows = transitions(w)
        print(w)
        for r in rows: print("  midi %d: new pitch within 10 c (held 5 periods) %+.0f ms from the written start, dip %.0f c, settled %+.0f c from nominal" % r)
        tt = np.array([r[1] for r in rows])
        print("  median %.0f ms, max %.0f ms, notes within 30 ms: %d of %d; median dip %.0f c" % (np.nanmedian(tt), np.nanmax(tt), int(np.sum(np.abs(tt) <= 30)), len(tt), np.median([r[2] for r in rows])))

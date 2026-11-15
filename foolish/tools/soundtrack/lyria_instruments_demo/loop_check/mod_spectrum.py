#!/usr/bin/env python3
"""mod_spectrum.py - modulation of a sustained stretch: the 1 to 12 Hz spectrum of the f0 curve (cents) and of the 10 ms RMS level (dB).
Usage as a module: modspec(mono, sr, t0, t1) -> dict; or  mod_spectrum.py render RUN_DIR LABEL  (a render_one.sh output: the note is
analysed from 0.6 to 4.8 s after its start, with the join phases from the trace), or  mod_spectrum.py wav FILE T0 T1 LABEL.
Peak size is the peak-to-peak equivalent of the sinusoid at the peak (2 x amplitude). The three largest local maxima of the spectrum are
listed as freq Hz:size. sd is the standard deviation of the curve over the stretch. Join phases: the phase of the band-passed
(dominant rate +-1.5 Hz) f0 curve just before and just after each join, minus the phase advance the dominant rate predicts."""
import sys, re
import numpy as np
from scipy.signal import hilbert
sys.path.insert(0, "/Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/tools/samples")
import lyria_notes_extract as le
H = 0.01


def spec(v, fs=100.0, lo=1.0, hi=12.0):
    v = v - np.mean(v)
    n = len(v)
    w = np.hanning(n)
    N = 1 << 14
    X = np.abs(np.fft.rfft(v * w, N)) * 2.0 / w.sum()
    fr = np.fft.rfftfreq(N, 1.0 / fs)
    sel = (fr >= lo) & (fr <= hi)
    f, a = fr[sel], X[sel]
    pk = [i for i in range(1, len(a) - 1) if a[i] >= a[i - 1] and a[i] >= a[i + 1]]
    pk = sorted(pk, key=lambda i: -a[i])[:3]
    return [(float(f[i]), float(2 * a[i])) for i in pk]


def curves(m, sr, t0, t1):
    seg = m[int(t0 * sr):int(t1 * sr)]
    f0, cen = le.yin_f0(seg, sr)
    ok = f0 > 0
    t = cen / sr + t0
    if ok.sum() < 10:
        return None
    c = 1200 * np.log2(np.where(ok, f0, np.median(f0[ok])) / np.median(f0[ok]))
    tt = np.arange(len(c))
    c = np.interp(tt, tt[ok], c[ok])
    db, _ = le.level_db(m, sr)
    lv = db[int(t0 / H):int(t1 / H)]
    return c, t, lv, float(ok.mean())


def modspec(m, sr, t0, t1):
    r = curves(m, sr, t0, t1)
    if r is None:
        return None
    c, t, lv, vs = r
    return dict(pitch_sd=float(np.std(c)), pitch_peaks=spec(c), level_sd=float(np.std(lv)), level_peaks=spec(lv), voiced=vs)


def fmt(pk, unit):
    return " ".join("%.1fHz:%.1f%s" % (f, s, unit) for f, s in pk)


def join_phases(m, sr, t0, t1, joins):
    c, t, lv, vs = curves(m, sr, t0, t1)
    pk = spec(c)
    fv = pk[0][0]
    n = len(c)
    X = np.fft.rfft(c - c.mean())
    fr = np.fft.rfftfreq(n, H)
    X[(fr < fv - 1.5) | (fr > fv + 1.5)] = 0
    band = np.fft.irfft(X, n)
    ph = np.unwrap(np.angle(hilbert(band)))
    out = []
    for jt, xf in joins:
        ta, tb = jt - xf / 2 - 0.05, jt + xf / 2 + 0.05
        if ta < t[0] + 0.3 or tb > t[-1] - 0.3:
            continue
        ia, ib = int(round((ta - t[0]) / H)), int(round((tb - t[0]) / H))
        d = ph[ib] - ph[ia] - 2 * np.pi * fv * (tb - ta)
        d = (d + np.pi) % (2 * np.pi) - np.pi
        out.append((jt, np.degrees(d)))
    return fv, out


def main():
    if sys.argv[1] == "render":
        d, label = sys.argv[2:4]
        tr = open(d + "/trace.txt").read().split("\n")
        kv = dict(re.findall(r"(\w+)=(\S+)", [l for l in tr if l.startswith("TRACE note")][0]))
        t_on = float(kv["t"])
        joins = []
        for l in tr:
            m_ = re.match(r"TRACE join t=(\S+) xf_s=(\S+)", l)
            if m_:
                joins.append((float(m_.group(1)) - float(m_.group(2)) / 2, float(m_.group(2))))
        x, sr = le.decode(d + "/full.wav")
        m = x.mean(axis=1)
        a, b = t_on + 0.6, t_on + 4.8
        r = modspec(m, sr, a, b)
        fv, jp = join_phases(m, sr, a, b, joins)
        jj = [j for j in joins if a <= j[0] <= b]
        print("%s\tpitch sd %.1f c, peaks %s\tlevel sd %.2f dB, peaks %s\tjoins %d, phase jump at joins (deg, rms %.0f): %s" % (
            label, r["pitch_sd"], fmt(r["pitch_peaks"], "c"), r["level_sd"], fmt(r["level_peaks"], "dB"), len(jj),
            float(np.sqrt(np.mean([p[1] ** 2 for p in jp]))) if jp else 0, " ".join("%.0f" % p[1] for p in jp)))
    else:
        f, t0, t1, label = sys.argv[2], float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
        x, sr = le.decode(f)
        r = modspec(x.mean(axis=1), sr, t0, t1)
        print("%s\tpitch sd %.1f c, peaks %s\tlevel sd %.2f dB, peaks %s" % (label, r["pitch_sd"], fmt(r["pitch_peaks"], "c"), r["level_sd"], fmt(r["level_peaks"], "dB")))


if __name__ == "__main__":
    main()

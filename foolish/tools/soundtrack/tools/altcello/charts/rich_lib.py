"""Spectral richness measures (iteration 1 of the richness work): the same pitches as the Philharmonia notes, rendered by the expert player.

Notes (all 1.5 s, forte vel 0.85, vibrato on, dry, the candidate 4 settings = the shipped defaults unless ALTCELLO_KNOBS says otherwise):
  D2   = C2 + 2 semitones on the C string  (Philharmonia ph_cello_38)
  G2c  = G2 stopped on the C string       (ph_cello_43)
  D3   = D3 on the G string               (ph_cello_50)
  A3   = A3 on the D string               (ph_cello_57)
Metrics of the sustained part (0.5 - 1.4 s after the onset, both sides):
  (a) third-octave LTAS, each curve minus its own peak band, mean difference (ours - ref) in 200-500, 500-1600, 1600-5000, 5000-10000 Hz;
  (b) between-harmonic floor, dB re the mean harmonic peak of the same octave, per octave 500-16000 Hz (the note is time warped to a constant pitch first,
      so the vibrato does not smear the partials);
  (c) highest harmonic within 40 dB of the strongest partial (fmax40), and the highest one that also stands 6 dB over the floor next to it (fmax6);
  (d) HNR (analysis/measure.py through player_gates.gates_of) and the audio time to periodic (onset_tool).
"""
import os, sys, subprocess
import numpy as np
import scipy.signal as ss
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
SR = 48000
ROOT = L.ROOT
OUT = os.path.join(ROOT, "out", "rich")
NOTES = {"D2": (38, "C", "d2"), "G2c": (43, "C", "g2c"), "D3": (50, "G", "d3"), "A3": (57, "D", "a3")}
SEEDS = (0, 1, 2)
SUS = (0.5, 1.4)
RANGES = [(200, 500), (500, 1600), (1600, 5000), (5000, 10000.1)]
RLAB = ["200-500", "500-1600", "1600-5000", "5000-10000"]
OCT = [(500, 1000), (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000)]
THIRD = [10 ** (0.1 * k) for k in range(23, 41)]  # 200 .. 10000 nominal (10^(k/10)), k = 23 .. 40
BANDS = [10 ** (0.1 * k) for k in range(17, 41)]  # 50 Hz .. 10 kHz for the peak


def hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


def render_note(key, seed, tag, knobs=""):
    """render one note with the altcello CLI (ALTCELLO_KNOBS = knobs); returns the 48 kHz mono signal"""
    midi, st, nm = NOTES[key]
    wav = os.path.join(OUT, "%s_%s_s%d.wav" % (tag, nm, seed))
    if not os.path.exists(wav):
        env = dict(os.environ)
        env["ALTCELLO_KNOBS"] = knobs
        cmd = ["nice", L.EXE, "render", os.path.join(OUT, nm + ".txt"), wav, "--style", "expert", "--room", "off", "--seed", str(seed)]
        subprocess.run(cmd, capture_output=True, text=True, check=True, env=env)
    return M.load_audio(wav)


def ref_note(key):
    return M.load_audio(G.PHIL_FULL % NOTES[key][0])


def onset(y):
    env = M.rms_env(y)
    return int(np.argmax(env >= 0.02 * env.max())) * 0.01


def sustain(y):
    on = onset(y)
    return y[int((on + SUS[0]) * SR):int((on + SUS[1]) * SR)], on


def ltas_third(seg):
    f, p = ss.welch(seg, SR, nperseg=16384, noverlap=8192, window="hann")
    out = []
    for fc in BANDS:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        m = (f >= lo) & (f < hi)
        out.append(10 * np.log10(np.sum(p[m]) + 1e-30))
    out = np.array(out)
    return out - out.max()


def ltas_ranges(l_ours, l_ref):
    d = l_ours - l_ref
    res = []
    for lo, hi in RANGES:
        m = np.array([(fc >= lo * 0.999 and fc < hi) for fc in BANDS])
        res.append(float(np.mean(d[m])))
    return res


def f0_track(seg, f_exp):
    """frame wise f0 by a harmonic sum over a grid around f_exp (window 80 ms, hop 5 ms); returns times, f0"""
    n = int(0.08 * SR)
    hop = int(0.005 * SR)
    w = np.hanning(n)
    nfft = n * 8
    grid = f_exp * 2.0 ** (np.linspace(-0.04, 0.04, 321) / 1.0)
    fr = np.arange(nfft // 2 + 1) * SR / nfft
    ts, fs_ = [], []
    for a in range(0, len(seg) - n, hop):
        X = np.abs(np.fft.rfft(seg[a:a + n] * w, nfft))
        lx = np.log(X + 1e-9)
        sc = np.zeros(len(grid))
        for h in range(1, 9):
            sc += np.interp(grid * h, fr, lx)
        ts.append((a + n / 2) / SR)
        fs_.append(grid[np.argmax(sc)])
    f = ss.medfilt(np.array(fs_), 5)
    return np.array(ts), f


def warp(seg, f_exp):
    """resample seg so that its pitch is constant (the mean f0); 4x oversampled cubic; returns signal at rate 4 SR, f0 mean"""
    ts, f = f0_track(seg, f_exp)
    t = np.arange(len(seg)) / SR
    ph = np.concatenate([[0.0], np.cumsum(0.5 * (f[1:] + f[:-1]) * np.diff(ts))])  # cycles at ts
    ph_t = np.interp(t, ts, ph, left=np.nan, right=np.nan)
    ok = ~np.isnan(ph_t)
    f0m = (ph[-1] - ph[0]) / (ts[-1] - ts[0])
    up = ss.resample_poly(seg, 4, 1)
    tu = np.arange(len(up)) / (4 * SR)
    # new sample j at phase p_j = ph[0] + j f0m / (4 SR); find its time
    nj = int((ph[-1] - ph[0]) / f0m * 4 * SR)
    pj = ph[0] + np.arange(nj) * f0m / (4 * SR)
    tj = np.interp(pj, ph, ts)
    from scipy.interpolate import CubicSpline
    y = CubicSpline(tu, up)(tj)
    return y, f0m


def harmonics(seg, f_exp, fmax=16000.0):
    """returns dict: f0, h (array of harmonic numbers), peak (dB), floor (dB) per harmonic h of the time warped sustained note"""
    y, f0 = warp(seg, f_exp)
    fs = 4 * SR
    w = np.hanning(len(y))
    nfft = 1 << int(np.ceil(np.log2(len(y) * 2)))
    X = np.abs(np.fft.rfft(y * w, nfft)) ** 2
    fr = np.arange(len(X)) * fs / nfft
    H = int(fmax / f0)
    hs = np.arange(1, H + 1)
    pk = np.zeros(H)
    fl = np.zeros(H)
    for i, h in enumerate(hs):
        m = (fr > (h - 0.12) * f0) & (fr < (h + 0.12) * f0)
        pk[i] = 10 * np.log10(X[m].max() + 1e-30)
        m2 = (fr > (h + 0.3) * f0) & (fr < (h + 0.7) * f0)
        fl[i] = 10 * np.log10(np.median(X[m2]) + 1e-30)
    return dict(f0=f0, h=hs, peak=pk, floor=fl)


def floor_oct(hd):
    """between-harmonic floor re the mean harmonic peak of each octave (dB), nan where an octave holds fewer than 2 harmonics"""
    out = []
    for lo, hi in OCT:
        m = (hd["h"] * hd["f0"] >= lo) & (hd["h"] * hd["f0"] < hi)
        out.append(float(np.mean(hd["floor"][m]) - np.mean(hd["peak"][m])) if m.sum() >= 2 else np.nan)
    return out


def fmax_reach(hd):
    pk, fl = hd["peak"], hd["floor"]
    top = pk.max()
    i40 = np.where(pk >= top - 40.0)[0]
    f40 = float(hd["h"][i40[-1]] * hd["f0"]) if len(i40) else np.nan
    i6 = np.where((pk >= top - 40.0) & (pk - fl >= 6.0))[0]
    f6 = float(hd["h"][i6[-1]] * hd["f0"]) if len(i6) else 0.0
    return f40, f6


def measure(y, key, tag):
    """all metrics of one note signal (the 3 s render or the reference file)"""
    midi = NOTES[key][0]
    seg, on = sustain(y)
    hd = harmonics(seg, hz(midi))
    f40, f6 = fmax_reach(hd)
    g = G.gates_of(y, midi, "rich_" + tag)
    _, tp = L.t_periodic(y, hz(midi), t_from=0.0, tag="rich_" + tag)
    return dict(ltas=ltas_third(seg), floor=floor_oct(hd), f40=f40, f6=f6, hnr=g["hnr"], tper=tp, hd=hd, on=on)


def measure_set(tag, knobs="", seeds=SEEDS, notes=None):
    """metrics of every note and seed of one configuration, with the reference; returns {key: {ref: m, seeds: [m]}}"""
    os.makedirs(OUT, exist_ok=True)
    res = {}
    for key in notes or NOTES:
        r = ref_note(key)
        res[key] = {"ref": measure(r, key, "ref" + key), "ours": [measure(render_note(key, s, tag, knobs), key, "%s%s%d" % (tag, key, s)) for s in seeds]}
    return res


def summarize(res, notes=None):
    """a flat table: per-note and the mean over notes and seeds of the four range differences, floors, fmax, hnr, tper"""
    keys = list(notes or res)
    rows = {}
    for k in keys:
        o = res[k]["ours"]
        rd = np.array([ltas_ranges(m["ltas"], res[k]["ref"]["ltas"]) for m in o])
        rows[k] = dict(rng=rd.mean(0), floor=np.nanmean([m["floor"] for m in o], axis=0), f40=np.mean([m["f40"] for m in o]), f6=np.mean([m["f6"] for m in o]),
                       hnr=np.mean([m["hnr"] for m in o]), tper=np.median([m["tper"] for m in o]),
                       rfloor=np.array(res[k]["ref"]["floor"]), rf40=res[k]["ref"]["f40"], rf6=res[k]["ref"]["f6"], rhnr=res[k]["ref"]["hnr"], rtper=res[k]["ref"]["tper"])
    return rows


def fmt(tag, rows):
    out = ["== %s" % tag]
    out.append("  %-5s %-34s %-44s %-34s %6s %6s %6s %5s" % ("note", "LTAS ours-ref dB " + "/".join(RLAB), "floor re peaks dB 0.5/1/2/4/8k-16k (ours | ref)", "fmax40/fmax6 kHz (ours | ref)", "HNR", "refHNR", "tper", "rtper"))
    for k, r in rows.items():
        out.append("  %-5s %s  %s | %s  %5.1f %5.1f | %5.1f %5.1f  %6.1f %6.1f %6.2f %5.2f" % (
            k, " ".join("%+6.1f" % v for v in r["rng"]), " ".join("%5.0f" % v for v in r["floor"]), " ".join("%5.0f" % v for v in r["rfloor"]),
            r["f40"] / 1e3, r["f6"] / 1e3, r["rf40"] / 1e3, r["rf6"] / 1e3, r["hnr"], r["rhnr"], r["tper"], r["rtper"]))
    m = np.mean([r["rng"] for r in rows.values()], axis=0)
    out.append("  MEAN  %s   (mean |gap| %.2f)" % (" ".join("%+6.1f" % v for v in m), float(np.mean(np.abs(m)))))
    return "\n".join(out)

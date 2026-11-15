"""ALT-CELLO single note diagnosis: measurements of one note (ALT-CELLO or Philharmonia) and of the simulator's slip pattern.

Library used by diagnose_run.py and diagnose_chart.py (it imports analysis/measure.py and never modifies it).
Times are seconds after the note onset (first 10 ms RMS frame at 2 percent of the maximum, minus 20 ms).

Definitions
  jitter_pct   cycle-synchronous: every cycle (tracked phase of the f0 curve) is resampled on 512 points per cycle and aligned to the
               mean cycle by cross-correlation (parabolic peak); period_j = distance of the landmarks of cycles j and j+1.
               jitter = sd(diff(period)) / sqrt 2 / mean(period), percent (slow vibrato drift cancels in the difference).
  shimmer_db   the same for the cycle peak amplitude (half the peak to peak of the aligned cycle) in dB: sd(diff(dB)) / sqrt 2.
  h_sd, h_mod  for h1..h12: sd over the window of the 50 ms heterodyne level (dB) and the peak of its modulation spectrum (0.5-10 Hz).
  nf_tot[b]    residual (signal minus the f0-tracking harmonic resynthesis) power in the band re total power, dB (bands 0-1, 1-2.5, 2.5-5,
               5-10 kHz); nf_har[b]: re the harmonic power of the same band.
  hnr_ac       M.autocorr_hnr; hnr_res: harmonic power over residual power.
  centroid     spectral centroid 50 Hz - 16 kHz of the Welch spectrum; cent_f0 = centroid / f0.
  hbal         h1, h2, h3 in dB re total power per 100 ms frame (mean over the window and sd).
  narrow       peaks (prominence 10 dB) of the ALT long-term spectrum above 800 Hz within 45 dB of its maximum; the level of a peak
               is the power in +-min(3 percent f, 0.4 f0) of it, dB re the 50-800 Hz power of the same note; unmatched = more than 6 dB
               above the reference's power in the same band.
  attack       t3db: time from the first 2 percent frame to the first 10 ms RMS frame within 3 dB of the steady median;
               tr_db: 2-8 kHz power of the first 0.4 s re the same band's power in the steady window.
  ring         decay after the bow lifts: 0.5 s Hann frames (hop 0.25 s) from 2.55 s, peaks of the first frame, dB level of each peak
               over the frames, T60 = 60 / (slope in dB/s); "string" if within 0.3 percent of k f_str sqrt(1 + B k^2) of an open string.
"""
import os, sys, math
import numpy as np
import scipy.signal as ss
from scipy.interpolate import CubicSpline

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "analysis"))
import measure as M  # noqa: E402

SR = M.SR
PHIL = "/Users/alex/Dev/soundtrack-samples/philharmonia/wav/ph_cello_%d.wav"
NOTE = {"G2": (43, 98.00), "D3": (50, 146.83), "A3": (57, 220.00)}
FORCE = {"G2": 0.67082, "D3": 0.67082, "A3": 0.49717}
BANDS_NF = [(0, 1000), (1000, 2500), (2500, 5000), (5000, 10000)]
STRINGS = {"C2": (65.41, 8.0e-5), "G2": (98.00, 4.8e-5), "D3": (146.83, 3.5e-5), "A3": (220.00, 2.5e-5)}
STEADY = (0.5, 2.0)
EARLY = (0.0, 0.4)


class Note:
    def __init__(self, path, fnom, label=""):
        y = M.load_audio(path)
        env = M.rms_env(y)
        on = int(np.argmax(env >= 0.02 * env.max())) * 0.01
        t0 = max(0.0, on - 0.02)
        self.x = y[int(round(t0 * SR)):]
        self.label, self.path, self.fnom = label, path, fnom
        self.tf, self.f0 = M.track_f0(self.x, SR, fnom)
        self.hd = M.harmonic_analysis(self.x, SR, self.tf, self.f0)
        self.res = self.x - self.hd["harm"]
        self.dur = len(self.x) / SR

    def win(self, w):
        a, b = w
        b = min(b, self.dur - 0.05)
        return int(round(a * SR)), int(round(b * SR))

    def spec(self, w, sig=None, nper=32768):
        a, b = self.win(w)
        s = self.x if sig is None else sig
        return M.welch_pow(s[a:b], SR, nper)


def cycle_stats(nt, w):
    """jitter percent, shimmer dB, number of cycles"""
    x, tf, f0 = nt.x, nt.tf, nt.f0
    a, b = nt.win(w)
    t = np.arange(len(x)) / SR
    ph = np.cumsum(np.interp(t, tf, f0)) / SR
    cs = CubicSpline(t, x)
    j0 = int(math.ceil(np.interp(a / SR, t, ph))) + 1
    j1 = int(math.floor(np.interp(b / SR, t, ph))) - 2
    if j1 - j0 < 8: return float("nan"), float("nan"), 0, float("nan")
    Mv, mg = 512, 179
    v = np.arange(-mg, Mv + mg) / Mv
    S = np.array([cs(np.interp(j + v, ph, t)) for j in range(j0, j1)])
    n = len(S)
    idx = np.full(n, float(mg))
    for it in range(2):
        W = np.array([S[j, int(round(idx[j])):int(round(idx[j])) + Mv] for j in range(n)])
        tpl = W.mean(axis=0); tpl = tpl - tpl.mean()
        for j in range(n):
            win = np.lib.stride_tricks.sliding_window_view(S[j], Mv)[:2 * mg + 1]
            c = (win - win.mean(axis=1, keepdims=True)) @ tpl
            k = int(np.argmax(c))
            if 0 < k < len(c) - 1:
                den = c[k - 1] - 2 * c[k] + c[k + 1]
                d = 0.5 * (c[k - 1] - c[k + 1]) / den if den != 0 else 0.0
            else: d = 0.0
            idx[j] = k + d
    land = np.interp(np.arange(j0, j1) + (idx - mg) / Mv, ph, t)
    per = np.diff(land)
    jit = 100.0 * np.std(np.diff(per)) / math.sqrt(2) / np.mean(per)
    W = np.array([S[j, int(round(idx[j])):int(round(idx[j])) + Mv] for j in range(n)])
    pk = 0.5 * (W.max(axis=1) - W.min(axis=1))
    sh = np.std(np.diff(20 * np.log10(pk + 1e-12))) / math.sqrt(2)
    ppq = float(np.mean([abs(per[i] - np.mean(per[i - 2:i + 3])) for i in range(2, len(per) - 2)]) / np.mean(per) * 100)
    return jit, sh, n, ppq


def harm_mod(nt, w, K=12):
    """sd (dB) and modulation spectrum peak (Hz) of h1..h12 over the window"""
    lev, tfr = nt.hd["lev"], nt.hd["tfr"]
    sel = (tfr >= w[0]) & (tfr <= min(w[1], nt.dur))
    sds, pks = [], []
    for k in range(min(K, 16)):
        d = 10 * np.log10(lev[k, sel] + 1e-30)
        sds.append(float(np.std(d)))
        if len(d) < 8: pks.append(float("nan")); continue
        z = d - np.polyval(np.polyfit(np.arange(len(d)), d, 1), np.arange(len(d)))
        sp = np.abs(np.fft.rfft(z * np.hanning(len(z)), 512))
        fr = np.fft.rfftfreq(512, 0.05)
        m = (fr >= 0.5) & (fr <= 10)
        pks.append(float(fr[m][np.argmax(sp[m])]))
    return np.array(sds), np.array(pks)


def band_db(f, p, lo, hi, ref):
    return 10 * np.log10(M.band_power(f, p, lo, hi) / ref + 1e-30)


def window_metrics(nt, w):
    a, b = nt.win(w)
    m = {}
    m["jit_pct"], m["shim_db"], m["ncyc"], m["ppq5"] = cycle_stats(nt, w)
    sds, pks = harm_mod(nt, w)
    m["h_sd"], m["h_mod"] = sds, pks
    m["wander"] = float(np.median(sds))
    f, p = nt.spec(w)
    tot = p.sum()
    fr, pr = nt.spec(w, nt.res)
    fh, ph_ = nt.spec(w, nt.hd["harm"])
    m["nf_tot"] = [band_db(fr, pr, lo, hi, tot) for lo, hi in BANDS_NF]
    m["nf_har"] = [10 * np.log10(M.band_power(fr, pr, lo, hi) / (M.band_power(fh, ph_, lo, hi) + 1e-30) + 1e-30) for lo, hi in BANDS_NF]
    fmed = float(np.median(nt.f0))
    m["f0"] = fmed
    m["hnr_ac"] = float(M.autocorr_hnr(nt.x[a:b], SR, fmed))
    m["hnr_res"] = 10 * np.log10(np.sum(nt.hd["harm"][a:b] ** 2) / (np.sum(nt.res[a:b] ** 2) + 1e-30))
    sel = (f >= 50) & (f <= 16000)
    m["centroid"] = float(np.sum(f[sel] * p[sel]) / np.sum(p[sel]))
    m["cent_f0"] = m["centroid"] / fmed
    # h1..h3 over time, 100 ms frames, dB re total
    A = nt.hd["A"]
    fr100 = int(0.1 * SR)
    hb = []
    for s in range(a, b - fr100 + 1, fr100):
        tp = np.mean(nt.x[s:s + fr100] ** 2) + 1e-30
        hb.append([10 * np.log10(np.mean(A[k, s:s + fr100] ** 2) / 2 / tp + 1e-30) for k in range(3)])
    hb = np.array(hb) if hb else np.full((1, 3), np.nan)
    m["hbal_mean"], m["hbal_sd"] = hb.mean(axis=0), hb.std(axis=0)
    return m


def attack_metrics(nt):
    env = M.rms_env(nt.x)
    st = np.median(env[int(STEADY[0] * 100):int(min(STEADY[1], nt.dur - 0.05) * 100)])
    i0 = int(np.argmax(env >= 0.02 * st))
    i1 = int(np.argmax(env >= st * 10 ** (-3 / 20)))
    sos = ss.butter(4, [2000, 8000], btype="band", fs=SR, output="sos")
    xb = ss.sosfiltfilt(sos, nt.x)
    a, b = nt.win(EARLY)
    c, d = nt.win(STEADY)
    tr = 10 * np.log10(np.mean(xb[a:b] ** 2) / (np.mean(xb[c:d] ** 2) + 1e-30) + 1e-30)
    return dict(t3db=(i1 - i0) * 0.01, tr_db=float(tr))


def narrow_bands(alt, ref, w=STEADY, beta=0.1, top=5):
    """unmatched narrow bands of ALT against the reference"""
    fa, pa = alt.spec(w)
    fb, pb = ref.spec(w)
    na = M.band_power(fa, pa, 50, 800)
    nb = M.band_power(fb, pb, 50, 800)
    f0 = float(np.median(alt.f0))
    db = 10 * np.log10(pa / na + 1e-30)
    sel = fa > 800
    pk, _ = ss.find_peaks(np.where(sel, db, -300), prominence=10, distance=4)
    pk = [i for i in pk if db[i] > db[sel].max() - 45]
    rows = []
    for i in pk:
        f = fa[i]
        hw = min(0.03 * f, 0.4 * f0)
        la = 10 * np.log10(M.band_power(fa, pa, f - hw, f + hw) / na + 1e-30)
        lr = 10 * np.log10(M.band_power(fb, pb, f - hw, f + hw) / nb + 1e-30)
        k = f / f0
        mb = f / (f0 / beta)
        rows.append(dict(f=float(f), lev=float(la), ref=float(lr), exc=float(la - lr), h=float(k), hdev=float(100 * (k / round(k) - 1)),
                         mb=float(round(mb)), mbdev=float(100 * (mb / max(round(mb), 1) - 1)) if round(mb) >= 1 else float("nan")))
    um = sorted([r for r in rows if r["exc"] > 6.0], key=lambda r: -r["lev"])
    return um[:top], len(um), len(rows)


def ring_analysis(wav, tstop=2.55, tend=4.1, topn=40):
    """long-lived spectral lines after the bow lifts: list of dict(f, lev0, t60, owner)"""
    y = M.load_audio(wav)
    nw, hop = int(0.5 * SR), int(0.25 * SR)
    frames = []
    s = int(tstop * SR)
    while s + nw <= min(len(y), int(tend * SR)):
        frames.append(np.abs(np.fft.rfft(y[s:s + nw] * np.hanning(nw), 2 * nw)) / nw)
        s += hop
    F = np.array(frames)
    fr = np.fft.rfftfreq(2 * nw, 1 / SR)
    db = 20 * np.log10(F + 1e-12)
    pk, _ = ss.find_peaks(db[0], prominence=10, distance=3)
    pk = [i for i in pk if 60 < fr[i] < 8000]
    pk = sorted(pk, key=lambda i: -db[0][i])[:topn]
    out = []
    for i in pk:
        lo, hi = max(i - 2, 0), i + 3
        tr = db[:, lo:hi].max(axis=1)
        tt = np.arange(len(tr)) * 0.25
        good = tr > db[0][i] - 55
        if good.sum() < 3: continue
        sl = np.polyfit(tt[good], tr[good], 1)[0]
        t60 = 60.0 / -sl if sl < -0.01 else float("inf")
        # parabolic frequency
        a, b, c = db[0][i - 1], db[0][i], db[0][i + 1]
        d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0
        f = fr[i] + d * (fr[1] - fr[0])
        out.append(dict(f=float(f), lev0=float(db[0][i]), t60=float(t60), owner=owner_of(f)))
    return out


def owner_of(f, tol=0.003):
    best = None
    for name, (f0, B) in STRINGS.items():
        for k in range(1, 160):
            fk = k * f0 * math.sqrt(1 + B * k * k)
            if abs(f / fk - 1) < tol and (best is None or abs(f / fk - 1) < best[0]):
                best = (abs(f / fk - 1), "%s k=%d" % (name, k))
    return best[1] if best else "body"


# ------------------------------------------------------------------ simulator slip pattern
def slip_stats(prefix, f0, fs=96000, ign=0.02, late_from=2.0):
    """from the probe's channel file (vrel = channel 3; sticking is vrel == 0): time (s) to 10 consecutive one-slip periods
    (strict: every slip episode counts; lenient: episodes under ign periods are ignored), late fraction of one-slip periods"""
    d = np.fromfile(prefix + ".f32", dtype=np.float32).reshape(-1, 8)
    slip = d[:, 3] != 0.0
    T = fs / f0
    out = {}
    for name, minlen in (("strict", 0), ("lenient", ign * T)):
        on = np.flatnonzero(slip[1:] & ~slip[:-1]) + 1
        off = np.flatnonzero(~slip[1:] & slip[:-1]) + 1
        eps = []
        for o in on:
            e = off[off > o]
            ln = (e[0] - o) if len(e) else len(slip) - o
            if ln > minlen: eps.append(o)
        eps = np.array(eps, dtype=float)
        if len(eps) < 11:
            out[name] = (float("nan"), 0.0); continue
        dd = np.diff(eps)
        good = np.abs(dd / T - 1.0) < 0.15
        run = np.convolve(good.astype(int), np.ones(10, int), "valid") == 10
        tt = eps[np.argmax(run)] / fs if run.any() else float("nan")
        late = (eps[:-1] / fs > late_from)
        frac = float(good[late].mean()) if late.any() else 0.0
        out[name] = (float(tt), frac)
    return out


def fmt(v, p=1):
    if isinstance(v, (list, tuple, np.ndarray)): return " ".join(fmt(x, p) for x in v)
    return "n/a" if v is None or (isinstance(v, float) and not np.isfinite(v)) else ("%.*f" % (p, v))


def gates(nt, w=STEADY):
    """the lead's gate set, with my own definitions (see DIAGNOSIS.md)"""
    g = {}
    a, b = nt.win(w)
    f, p = nt.spec(w)
    tot = p.sum()
    bd = lambda lo, hi, pp=p, ff=f, t=tot: 10 * np.log10(M.band_power(ff, pp, lo, hi) / t + 1e-30)
    g["b1_3"], g["b3_8"] = bd(1000, 3000), bd(3000, 8000)
    fmed = float(np.median(nt.f0))
    snr = 10 ** (M.autocorr_hnr(nt.x[a:b], SR, fmed) / 10)
    g["sust_ac"] = snr / (1 + snr)
    g["hnr"] = float(M.autocorr_hnr(nt.x[a:b], SR, fmed))
    fe, pe = nt.spec(EARLY, nper=16384)
    sh = lambda ff, pp, lo, hi: 10 * np.log10(M.band_power(ff, pp, lo, hi) / pp.sum() + 1e-30)
    g["att_tilt"] = sh(fe, pe, 2000, 6000) - sh(f, p, 2000, 6000)
    env = 20 * np.log10(M.rms_env(nt.x)[int(w[0] * 100):int(min(w[1], nt.dur - 0.05) * 100)] + 1e-12)
    g["lev_sd"] = float(np.std(env))
    z = env - np.polyval(np.polyfit(np.arange(len(env)), env, 1), np.arange(len(env)))
    sp = np.abs(np.fft.rfft(z * np.hanning(len(z)), 1024)) ** 2
    fr = np.fft.rfftfreq(1024, 0.01)
    t_all = sp[(fr >= 0.5) & (fr <= 50)].sum()
    g["mod4_7"] = sp[(fr >= 4) & (fr <= 7)].sum() / t_all
    g["mod10"] = sp[(fr >= 10) & (fr <= 50)].sum() / t_all
    jit, shm, n, ppq = cycle_stats(nt, w)
    g["ppq5"], g["shim"] = ppq, shm
    # peakiest harmonic over the local median (harmonic power in +-0.3 f0, k = 1..30)
    L = []
    for k in range(1, 31):
        if k * fmed > 0.45 * SR: break
        L.append(10 * np.log10(M.band_power(f, p, (k - 0.3) * fmed, (k + 0.3) * fmed) + 1e-30))
    L = np.array(L)
    ex = [L[k] - np.median(np.delete(L[max(0, k - 3):k + 4], min(k, 3))) for k in range(len(L))]
    g["peaky"] = float(max(ex))
    # time to periodic (audio): 10 ms frames, normalised autocorrelation at the period >= 0.98 for 5 frames in a row
    T = int(round(SR / fmed))
    r = []
    for i in range(0, int((nt.dur - 0.1) * 100)):
        s0 = i * 480
        x = nt.x[s0:s0 + 960 + T]
        if len(x) < 960 + T: break
        u, v = x[:960], x[T:T + 960]
        r.append(np.dot(u, v) / (np.sqrt(np.dot(u, u) * np.dot(v, v)) + 1e-30))
    r = np.array(r)
    ok = np.convolve((r >= 0.98).astype(int), np.ones(5, int), "valid") == 5
    g["t_per"] = float(np.argmax(ok) * 0.01) if ok.any() else float("nan")
    return g

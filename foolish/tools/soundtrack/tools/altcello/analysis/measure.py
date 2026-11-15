#!/usr/bin/env python3
"""measure.py: ALT-CELLO measurement suite (plain TSV and Markdown output, no JSON, deterministic).

Run with: /Users/alex/Dev/soundtrack-tools/venv/bin/python -I measure.py --wav X.wav --notes NOTES.txt --label NAME --out PREFIX
  NOTES.txt: lines "start_s dur_s midi" (extra columns ignored, # comments).  Without --notes, steady-pitch runs of 0.8 s or longer
  found by pyin are used (as measure_cello.py does for the real Lyria clip).  Writes PREFIX.tsv (metric, value, unit, source),
  PREFIX.md, PREFIX_notes.tsv (one row per note) and PREFIX_hseries.tsv (harmonic levels per 50 ms frame, per note).

Reuse: the metric definitions and several functions (rms_env, pyin f0 tracking, steady-run finder, autocorrelation HNR, rolloff85,
centroid/f0, slow level and pitch sd, vibrato rate and sd-based depth, release T20/T40) are copied from
/Users/alex/Music/foolish_cities_v5/replicas/instruments/vsco_vs_lyria/measure_cello.py (same author and repo family), so numbers line up
with its RESULTS.md.  New here: heterodyne harmonic tracking with a refined f0, harmonic resynthesis and residual, 1/6-octave LTAS with
peaks, 1/3-octave noise shape, vibrato onset and regularity, anti-synthy metrics.

Definitions (all dB are re the stated reference):
  attack_ms        reference definition: first 10 ms RMS frame within 40 dB (or 2x the file floor) of the note's first-0.5 s peak, to the
                   first frame at 90 percent of that peak.  attack_10_90_ms: 10 to 90 percent of the peak (linear amplitude).  Only for
                   notes that follow silence (the level before the onset is under the threshold), else NaN.
  rel_t20_s/t40_s  seconds after the note end until the 10 ms RMS has fallen 20 / 40 dB under its mean over the last 150 ms of the note;
                   NaN when the next note starts first or the file floor is reached first.
  h2..h16          level of harmonic k re h1, dB: heterodyne (demodulate at k*f0(t), moving average of 4 f0 periods, at least 25 ms),
                   power-averaged per 50 ms frame, median over the body frames.
  hnr_db           reference definition: median over 40 ms frames of 10 log10(r/(1-r)), r the normalised autocorrelation peak at T0.
  hnr_res_db       harmonic power over residual power after subtracting the heterodyne resynthesis (the filter keeps 1/periods of the
                   noise in the harmonic part, corrected for).  noise_db@F: residual power in the 1/3-octave band at F re total, dB.
  bow_noise_db     residual power in 2 to 8 kHz re total power, dB.
  pitch_micro_c    sd of f0 in cents after removing a 0.5 s moving median (and the 3.5 to 9 Hz band when vibrato is present).
  level_micro_db   sd of the 10 ms RMS level in dB after removing a 0.5 s moving median (vibrato amplitude modulation is kept).
  vib_*            rate (peak of the 3.5 to 9 Hz f0-curve spectrum), depth (peak cents, median of the Hilbert envelope of the band-passed
                   curve where it exceeds half its maximum), onset (s from note start until the smoothed envelope reaches half the
                   final depth), regularity (sd/mean of the period between upward zero crossings).  Notes with a body under 0.9 s: NaN.
  wander_db        median over harmonics 1..12 (those within 35 dB of h1) of the sd of each harmonic's 50 ms dB level after removing a
                   0.5 s moving median.
  valley_db        power in the 0.3 to 0.7 zone between harmonics (f0 ratio 1.5 up to 30 or 8 kHz) re power over that whole range, dB
                   (a flat floor gives -4 dB); dev1..dev10 partial frequency deviation from k*(partial 1), cents, long-window peak
                   picking with a stiff-string (1+B k^2) prediction.
  h_sd_notes_db    sd across notes of each harmonic's level (h2..h12), median over harmonics; attack_sd_ms: sd of attack across notes.
"""
import os, sys, subprocess, math, argparse
import numpy as np
import scipy.signal as ss
import scipy.ndimage as ndi
import librosa

SR = 48000
HOP = 480                      # 10 ms f0 frame
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
THIRD_FC = [1000.0 * 2 ** (i / 3) for i in range(-10, 13)]            # 99 Hz ... 16 kHz
LTAS_F = [40.0 * 2 ** (i / 12) for i in range(0, 104) if 40.0 * 2 ** (i / 12) <= 16000]
SOURCE_UNITS = {}


def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12.0)


def load_audio(path, sr=SR):
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype=np.float32).astype(np.float64)


def rms_env(y, hop=480, win=960):
    """10 ms hop, 20 ms window RMS; frame i is centred at (i*hop + win/2)/SR (copied from measure_cello.py)."""
    n = (len(y) - win) // hop + 1
    if n < 1:
        return np.zeros(1) + 1e-10
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    return np.sqrt(np.mean(y[idx] ** 2, axis=1) + 1e-20)


def boxc(z, L):
    """centred moving average of length L (real or complex), edge-normalised."""
    n = len(z); c = np.concatenate(([0], np.cumsum(z)))
    i = np.arange(n); h = L // 2
    lo = np.clip(i - h, 0, n); hi = np.clip(i - h + L, 0, n)
    return (c[hi] - c[lo]) / np.maximum(hi - lo, 1)


def fill_nan(f, ok):
    xs = np.arange(len(f))
    return np.interp(xs, xs[ok], f[ok])


# ---------------------------------------------------------------- f0
def pyin_track(x, sr, fmin, fmax):
    f0, vf, vp = librosa.pyin(x, fmin=fmin, fmax=fmax, sr=sr, frame_length=4096, hop_length=HOP, resolution=0.05)
    return f0, vf & ~np.isnan(f0)


def smooth_f0(f0, fc=15.0):
    b, a = ss.butter(2, fc / (50.0)); return ss.filtfilt(b, a, f0, padlen=min(len(f0) - 1, 12))


def refine_f0(x, sr, tf, f0, iters=3, ks=(1, 2, 3)):
    """sub-cent f0 from the phase derivative of the demodulated harmonics 1..3 (weights k^2 |z|^2)."""
    n = len(x); t = np.arange(n) / sr
    fmed = float(np.median(f0)); L = max(3, int(round(4 * sr / fmed))); h = max(2, int(round(sr / fmed)) // 2 * 2)   # lag of one period
    for _ in range(iters):
        fs = np.interp(t, tf, f0); ph = 2 * np.pi * np.cumsum(fs) / sr
        num = np.zeros(len(tf)); den = np.zeros(len(tf))
        for k in ks:
            if k * fmed > 0.4 * sr: break
            zs = boxc(x * np.exp(-1j * k * ph), L)
            d = np.zeros(n); d[h // 2:n - h // 2] = np.angle(zs[h:] * np.conj(zs[:-h])) * sr / (2 * np.pi * h)   # beats at f0 cancel over one period
            d = boxc(d, HOP)
            w = np.interp(tf, t, np.abs(zs)) ** 2 * k * k
            num += w * np.interp(tf, t, d) / k; den += w
        delta = num / (den + 1e-30)
        delta = np.where(den > 1e-3 * den.max(), delta, 0.0)
        delta = ndi.median_filter(delta, size=3, mode="nearest")
        f0 = smooth_f0(f0 + np.clip(delta, -0.03 * f0, 0.03 * f0))
    return f0


def track_f0(x, sr, fnom, f0_curve=None, t_offset=0.0):
    """returns tf (frame times in the segment), f0 (Hz, refined).  f0_curve=(t, f0) from a clip-level pyin run, else pyin on x."""
    n = len(x); F = 1 + n // HOP; tf = np.arange(F) * HOP / sr
    if f0_curve is not None:
        f0 = np.interp(tf + t_offset, f0_curve[0], f0_curve[1])
    else:
        f, ok = pyin_track(x, sr, fnom / 1.5, fnom * 1.5)
        f = f[:F] if len(f) >= F else np.concatenate([f, np.full(F - len(f), np.nan)])
        ok = ok[:F] if len(ok) >= F else np.concatenate([ok, np.zeros(F - len(ok), bool)])
        f0 = fill_nan(f, ok) if ok.sum() >= 5 else np.full(F, fnom)
    f0 = smooth_f0(ndi.median_filter(f0, size=5, mode="nearest"))
    return tf, refine_f0(x, sr, tf, f0)


# ---------------------------------------------------------------- harmonics
def harmonic_analysis(x, sr, tf, f0, kmax=160, P=3):
    """heterodyne at k*f0(t).  returns dict: harm (resynthesis), frames (50 ms centre times), lev (16 x frames power, linear), K, L, periods."""
    n = len(x); t = np.arange(n) / sr
    fmed = float(np.median(f0)); L = max(int(round(P * sr / fmed)), int(0.02 * sr))
    # two boxes of L samples (triangle): sidelobes fall as sinc^2, so the sum of up to 160 resynthesised harmonics does not add up to a
    # pulse train of leakage (one box leaves 2e-4 per harmonic, which adds coherently to -21 dB); nulls sit at multiples of f0
    tri = np.convolve(np.ones(L), np.ones(L)); enbw = sr * np.sum(tri ** 2) / np.sum(tri) ** 2
    K = int(min(kmax, 0.45 * sr / f0.max()))
    fs = np.interp(t, tf, f0); ph = 2 * np.pi * np.cumsum(fs) / sr
    harm = np.zeros(n); A = np.zeros((16, n))
    for k in range(1, K + 1):
        e = np.exp(-1j * k * ph); zs = boxc(boxc(x * e, L), L)
        harm += 2 * np.real(zs * np.conj(e))
        if k <= 16: A[k - 1] = 2 * np.abs(zs)
    fh = int(0.05 * sr); nf = n // fh
    lev = (A[:, :nf * fh] ** 2 / 2).reshape(16, nf, fh).mean(axis=2) if nf > 0 else np.zeros((16, 0))
    return dict(harm=harm, lev=lev, tfr=(np.arange(nf) + 0.5) * 0.05, K=K, L=L, periods=fmed / enbw, A=A)


def welch_pow(x, sr, nper=8192):
    nper = int(min(nper, 2 ** int(math.log2(max(len(x), 256)))))
    f, p = ss.welch(x, fs=sr, window="hann", nperseg=nper, noverlap=nper // 2, scaling="spectrum")
    return f, p


def band_power(f, p, lo, hi):
    return float(p[(f >= lo) & (f < hi)].sum())


def autocorr_hnr(body, sr, fmed):
    """measure_cello.py definition."""
    T = sr / fmed; W = int(0.040 * sr); vals = []
    for s in range(0, len(body) - W - int(1.2 * T), int(0.020 * sr)):
        x = body[s:s + W + int(1.2 * T)]; x = x - x.mean(); a = x[:W]
        lag = np.arange(int(T * 0.97), int(T * 1.03) + 1)
        rr = [np.dot(a, x[l:l + W]) / (np.sqrt(np.dot(a, a) * np.dot(x[l:l + W], x[l:l + W])) + 1e-20) for l in lag]
        q = min(max(max(rr), 1e-4), 0.9999); vals.append(10 * np.log10(q / (1 - q)))
    return float(np.median(vals)) if vals else float("nan")


def partial_dev(x, sr, tf, f0, b0, b1, kmax=10):
    """per-frame long-window peak picking; returns dev cents (k=1..kmax, 0 for k=1), valley_db."""
    fmed = float(np.median(f0)); W = int(max(8 * sr / fmed, 0.06 * sr)); win = np.hanning(W)
    nfft = 2 ** int(math.ceil(math.log2(W * 8))); fr = np.fft.rfftfreq(nfft, 1 / sr)
    rows = []; valley = []
    for s in range(b0, b1 - W + 1, int(0.05 * sr)):
        mag = np.abs(np.fft.rfft(x[s:s + W] * win, nfft)); lm = np.log(mag + 1e-20)
        fc = float(np.interp((s + W / 2) / sr, tf, f0)); df = fr[1]
        def peak(pred, half):
            i0 = int(max(1, (pred - half) / df)); i1 = int((pred + half) / df) + 1
            if i1 >= len(mag) - 1 or i1 - i0 < 3: return np.nan
            j = i0 + int(np.argmax(mag[i0:i1]))
            if j == i0 or j == i1 - 1: return np.nan
            a, b, c = lm[j - 1], lm[j], lm[j + 1]; d = a - 2 * b + c
            return (j + (0.5 * (a - c) / d if d < 0 else 0.0)) * df
        f1 = peak(fc, 0.25 * fc)
        if np.isnan(f1): continue
        found = {1: f1}; B = 0.0; row = [0.0]
        for k in range(2, kmax + 1):
            if len(found) >= 3:
                ks = np.array([j for j in found if j >= 2], float); u = (np.array([found[int(j)] for j in ks]) / (ks * f1)) ** 2
                den = np.sum((ks ** 2 - u) ** 2)
                if den > 0: B = float(np.clip(np.sum((u - 1) * (ks ** 2 - u)) / den, 0.0, 0.02))
            pred = k * f1 * math.sqrt((1 + B * k * k) / (1 + B))
            if pred > 0.45 * sr: row.append(np.nan); continue
            fk = peak(pred, 0.3 * f1)
            if not np.isnan(fk): found[k] = fk
            row.append(1200 * math.log2(fk / (k * f1)) if not np.isnan(fk) else np.nan)
        rows.append(row)
        q = fr / fc; Q = min(30.0, 8000.0 / fc); sel = (q >= 1.5) & (q <= Q); fr_part = q[sel] % 1.0
        pw = mag[sel] ** 2
        if pw.sum() > 0: valley.append(10 * math.log10(max(pw[(fr_part >= 0.3) & (fr_part <= 0.7)].sum() / pw.sum(), 1e-12)))
    dev = np.full(kmax, np.nan)
    if len(rows) >= 3:
        R = np.array(rows)
        for k in range(kmax):
            v = R[:, k][~np.isnan(R[:, k])]
            if len(v) >= 3: dev[k] = float(np.median(v))
    return dev, (float(np.median(valley)) if valley else np.nan)


# ---------------------------------------------------------------- vibrato and micro variation
def vib_analysis(c, fps):
    """c: cents curve of the whole note at fps.  returns dict rate, depth, onset, regularity, present, curve band (3.5-9 Hz)."""
    r = dict(vib_present=0.0, vib_rate_hz=np.nan, vib_depth_c=np.nan, vib_onset_s=np.nan, vib_regularity=np.nan, vib_c_sd=np.nan)
    if len(c) < 0.9 * fps: return r, None
    L = int(fps * 0.4)
    cd = c - np.convolve(np.pad(c, (L, L), mode="edge"), np.ones(L) / L, mode="same")[L:-L]
    cdt = cd[int(fps * 0.2):-int(fps * 0.2)] if len(cd) > 0.8 * fps else cd
    sp = np.abs(np.fft.rfft(cdt * np.hanning(len(cdt)), n=4096)); fq = np.fft.rfftfreq(4096, 1 / fps)
    band = (fq >= 3.5) & (fq <= 9); wide = (fq >= 1) & (fq <= 15)
    j = int(np.argmax(np.where(band, sp, 0))); rate = float(fq[j]); ratio = float(sp[j] / (np.median(sp[wide]) + 1e-12))
    bb, aa = ss.butter(2, [3.5 / (fps / 2), 9 / (fps / 2)], btype="band")
    cb = ss.filtfilt(bb, aa, c, padlen=min(len(c) - 1, 3 * max(len(aa), len(bb))))
    env = np.abs(ss.hilbert(cb)); trim = int(fps * 0.15)
    core = env[trim:-trim] if len(env) > 2 * trim + 5 else env
    depth = float(np.median(core[core > 0.5 * core.max()]))
    r["vib_c_sd"] = float(np.sqrt(2) * np.std(cb[trim:-trim] if len(cb) > 2 * trim + 5 else cb))
    r["vib_rate_hz"] = rate; r["vib_depth_c"] = depth
    r["vib_present"] = float(depth >= 3.0 and ratio >= 3.0)
    if r["vib_present"]:
        es = ndi.median_filter(env, size=max(3, int(fps * 0.2)) | 1, mode="nearest")
        full = float(np.percentile(es[trim:-trim] if len(es) > 2 * trim + 5 else es, 90))
        idx = np.where(es >= 0.5 * full)[0]
        r["vib_onset_s"] = float(idx[0] / fps) if len(idx) else np.nan
        up = np.where((cb[:-1] < 0) & (cb[1:] >= 0))[0]
        if len(up) >= 4:
            tz = up + (-cb[up]) / (cb[up + 1] - cb[up]); per = np.diff(tz) / fps
            per = per[(per > 0.5 / 9.0) & (per < 1.0 / 3.5)]
            if len(per) >= 3: r["vib_regularity"] = float(np.std(per) / np.mean(per))
    return r, cb


def lowpass_slow(c, fps, fc=3.0):
    b, a = ss.butter(2, fc / (fps / 2)); return ss.filtfilt(b, a, c, padlen=min(len(c) - 1, 3 * max(len(a), len(b))))


def detrend_lin(c):
    x = np.arange(len(c)); p = np.polyfit(x, c, 1); return c - np.polyval(p, x)


def movmed_resid(v, n):
    return v - ndi.median_filter(v, size=int(n) | 1, mode="nearest")


# ---------------------------------------------------------------- one note
def analyze_note(y, sr, t0, dur, midi, floor, f0_curve=None, edge_silent=True, next_t0=None):
    fnom = midi_hz(midi); r = dict(midi=midi, t0=t0, dur=dur)
    n_y = len(y)
    s0 = max(0, int((t0 - 0.2) * sr)); s1 = min(n_y, int((t0 + dur + 0.2) * sr)); x = y[s0:s1]
    off = s0 / sr
    # attack (reference definition and 10-90)
    env = rms_env(y); it0 = int(round(t0 * 100)); thr_floor = max(2.0 * floor, 1e-9)
    seg = env[it0: it0 + int(min(dur, 0.5) * 100) + 1]
    r["attack_ms"] = r["attack_10_90_ms"] = np.nan
    if len(seg) >= 3:
        ip = it0 + int(np.argmax(seg)); pk = env[ip]; thr = max(0.01 * pk, thr_floor)
        lo = max(0, ip - 60); below = np.where(env[lo:ip] < thr)[0]
        i_on = None
        if len(below): i_on = lo + below[-1] + 1
        elif edge_silent and t0 < 0.06: i_on = 0
        if i_on is not None and thr < 0.5 * pk:
            i90 = i_on + int(np.argmax(env[i_on:ip + 1] >= 0.9 * pk)); i10 = i_on + int(np.argmax(env[i_on:ip + 1] >= 0.1 * pk))
            r["attack_ms"] = (i90 - i_on) * 10.0; r["attack_10_90_ms"] = (i90 - i10) * 10.0
    # release
    r["rel_t20_s"] = r["rel_t40_s"] = np.nan
    e0 = int(round((t0 + dur - 0.15) * 100)); e1 = int(round((t0 + dur) * 100))
    if e0 >= 0 and e1 <= len(env) and e1 > e0:
        db = 20 * np.log10(env + 1e-12); ref = float(np.mean(db[e0:e1])); tail = db[e1:e1 + 400]
        lim = len(tail) if next_t0 is None else max(0, int(round((next_t0 - t0 - dur) * 100)))
        for tag, d in (("rel_t20_s", 20), ("rel_t40_s", 40)):
            lv = ref - d; j = np.where(tail[:lim] <= lv)[0]
            if len(j) and lv > 20 * np.log10(max(floor, 1e-9) * 1.5): r[tag] = float(j[0] * 0.01)
    # f0, harmonics
    tf, f0 = track_f0(x, sr, fnom, f0_curve, t_offset=off)
    fmed_all = float(np.median(f0))
    bs = 0.25 if dur >= 0.7 else 0.1
    b0 = max(0, int((t0 + bs - off) * sr)); b1 = min(len(x), int((t0 + dur - 0.05 - off) * sr))
    if b1 - b0 < int(0.12 * sr):
        r["f0_hz"] = fmed_all; return r, None
    body = x[b0:b1]
    fi0, fi1 = int(b0 / HOP), int(b1 / HOP)
    fmed = float(np.median(f0[fi0:fi1])); r["f0_hz"] = fmed; r["cents_err"] = 1200 * math.log2(fmed / fnom)
    ha = harmonic_analysis(x, sr, tf, f0)
    harm = ha["harm"][b0:b1]; res = body - harm
    # spectra
    f, p = welch_pow(body - body.mean(), sr); cs = np.cumsum(p)
    r["rolloff85_hz"] = float(f[np.searchsorted(cs, 0.85 * cs[-1])]); r["centroid_hz"] = float((p * f).sum() / p.sum())
    r["centroid_over_f0"] = r["centroid_hz"] / fmed
    # harmonic levels per frame
    fr0, fr1 = int(math.ceil(b0 / (0.05 * sr))), int(b1 / (0.05 * sr))
    lev = ha["lev"][:, fr0:fr1]; tfr = ha["tfr"][fr0:fr1] + off - t0
    if lev.shape[1] >= 1:
        ldb = 10 * np.log10(lev + 1e-20); rel = ldb - ldb[0:1]
        for k in range(2, 17):
            r["h%d" % k] = float(np.median(rel[k - 1])) if k * fmed < 0.45 * sr else np.nan
        r["h1_abs_db"] = float(np.median(ldb[0]))
    else:
        ldb = rel = None
    # noise: harmonic subtraction with the 1/periods leakage correction
    c = 1.0 / ha["periods"]; fcut = (ha["K"] + 0.5) * fmed
    fr_, pr_ = welch_pow(res, sr); fx_, px_ = welch_pow(body - body.mean(), sr)
    tot = float(px_.sum()); lo_m = (fr_ >= 0.5 * fmed) & (fr_ < fcut)
    corr = np.where(lo_m, 1.0 / (1 - c), 1.0); pn = pr_ * corr
    N = float(pn.sum()); H = max(tot - N, 1e-20)
    r["hnr_res_db"] = 10 * math.log10(H / N)
    r["hnr_db"] = autocorr_hnr(body, sr, fmed)
    for fc in THIRD_FC:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        r["noise_db@%d" % round(fc)] = 10 * math.log10(max(band_power(fr_, pn, lo, hi) / tot, 1e-14))
    r["bow_noise_db"] = 10 * math.log10(max(band_power(fr_, pn, 2000, 8000) / tot, 1e-14))
    # micro variation
    fps = 100.0; cc = 1200 * np.log2(f0 / fmed)
    i_n0 = int((t0 - off) * 100); i_n1 = int((t0 + dur - off) * 100)
    vr, cb = vib_analysis(cc[max(i_n0, 0):i_n1], fps); r.update(vr)
    cbody = cc[fi0:fi1]
    if len(cbody) >= 30:
        m = cbody
        if vr["vib_present"]:
            bb, aa = ss.butter(2, [3.5 / 50, 9 / 50], btype="bandstop"); m = ss.filtfilt(bb, aa, m, padlen=min(len(m) - 1, 9))
        m = movmed_resid(m, 51)
        r["pitch_micro_c"] = float(np.std(m[10:-10] if len(m) > 40 else m))
        eb = 20 * np.log10(rms_env(body) + 1e-12)
        if len(eb) >= 30:
            r["level_micro_db"] = float(np.std(movmed_resid(eb, 51)))
            if len(eb) > 40:
                r["lvl_sd_slow_db"] = float(np.std(detrend_lin(lowpass_slow(eb, 100))))
                r["pit_sd_slow_c"] = float(np.std(detrend_lin(lowpass_slow(cbody, fps))[10:-10]))
    # wander
    if rel is not None and rel.shape[1] >= 8:
        sds = []
        for k in range(1, 13):
            if k * fmed < 0.45 * sr and np.median(rel[k - 1]) >= -35:
                sds.append(np.std(movmed_resid(ldb[k - 1], 10)))
        r["wander_db"] = float(np.median(sds)) if sds else np.nan
    # inharmonicity
    dev, valley = partial_dev(x, sr, tf, f0, b0, b1)
    for k in range(10): r["dev%d_c" % (k + 1)] = float(dev[k])
    r["valley_db"] = valley
    extra = dict(body=body, ldb=ldb, tfr=tfr)
    return r, extra


# ---------------------------------------------------------------- signal level
def find_runs(y, sr, min_len=0.8):
    """steady-pitch runs (within 60 cents of the run median), (start, dur, midi) plus the clip-level f0 curve (copied logic from measure_cello.py)."""
    f0, ok = pyin_track(y, sr, 60, 800); runs = []; s = None
    for i in range(len(f0)):
        if ok[i] and s is None: s = i
        if s is not None and (not ok[i] or (i - s >= 5 and abs(1200 * np.log2(f0[i] / np.median(f0[s:i]))) > 60)):
            if i - s >= int(min_len * 100): runs.append((s, i))
            s = i if ok[i] else None
    if s is not None and len(f0) - s >= int(min_len * 100): runs.append((s, len(f0)))
    notes = [(a * 0.01, (b - a) * 0.01, int(round(69 + 12 * np.log2(np.median(f0[a:b]) / 440)))) for a, b in runs]
    tt = np.arange(len(f0)) * HOP / sr
    return notes, (tt, fill_nan(f0, ok) if ok.sum() > 5 else np.full(len(f0), 150.0))


def read_notes(path):
    out = []
    for line in open(path):
        line = line.split("#")[0].split()
        if len(line) >= 3: out.append((float(line[0]), float(line[1]), int(round(float(line[2])))))
    return out


def ltas_curve(psds):
    """psds: list of (f, p).  each normalised to unit power, averaged, 1/6-octave smoothed on a 1/12-octave grid, dB re max."""
    f = psds[0][0]; acc = np.zeros(len(f))
    for fi, p in psds:
        pi = np.interp(f, fi, p); acc += pi / pi.sum()
    acc /= len(psds); cum = np.concatenate(([0], np.cumsum(acc))); fe = np.concatenate((f - (f[1] - f[0]) / 2, [f[-1] + (f[1] - f[0]) / 2]))
    out = []
    for fc in LTAS_F:
        lo, hi = fc / 2 ** (1 / 12), fc * 2 ** (1 / 12)
        a, b = np.interp([lo, hi], fe, cum); out.append((b - a) / (hi - lo))
    out = 10 * np.log10(np.array(out) + 1e-20)
    return out - out.max()


def analyze_signal(y, sr, notes, label, f0_curve=None, edge_silent=True, isolated=True):
    env = rms_env(y); floor = float(np.percentile(env, 10))
    rows, extras = [], []
    order = sorted(range(len(notes)), key=lambda i: notes[i][0])
    for n, i in enumerate(order):
        t0, dur, midi = notes[i]
        nxt = notes[order[n + 1]][0] if n + 1 < len(order) else None
        r, ex = analyze_note(y, sr, t0, dur, midi, floor, f0_curve, edge_silent, nxt)
        r["note"] = n; rows.append(r); extras.append(ex)
    return build_result(label, rows, extras, sr)


def nanmed(v):
    v = np.array([x for x in v if x is not None and not np.isnan(x)], float)
    return float(np.median(v)) if len(v) else float("nan")


def build_result(label, rows, extras, sr):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys: keys.append(k)
    S = {}
    for k in keys:
        if k in ("note", "t0", "dur", "midi", "file"): continue
        S[k] = nanmed([r.get(k, np.nan) for r in rows])
    S["vib_present_frac"] = nanmed([r["vib_present"] for r in rows if "vib_present" in r and not np.isnan(r.get("vib_depth_c", np.nan))]) if any("vib_present" in r for r in rows) else np.nan
    S["vib_present_frac"] = float(np.mean([r["vib_present"] for r in rows if "vib_present" in r and not np.isnan(r.get("vib_depth_c", np.nan))] or [np.nan]))
    for k in ("vib_rate_hz", "vib_depth_c", "vib_onset_s", "vib_regularity"):   # only notes that have vibrato
        S[k] = nanmed([r.get(k, np.nan) for r in rows if r.get("vib_present", 0) == 1.0])
    S["n_notes"] = float(len(rows))
    # anti-synthy across notes
    sds = []
    for k in range(2, 13):
        v = [r["h%d" % k] for r in rows if "h%d" % k in r and not np.isnan(r["h%d" % k])]
        if len(v) >= 3: sds.append(np.std(v))
    S["h_sd_notes_db"] = float(np.median(sds)) if sds else np.nan
    at = [r["attack_ms"] for r in rows if not np.isnan(r.get("attack_ms", np.nan))]
    S["attack_sd_ms"] = float(np.std(at)) if len(at) >= 3 else np.nan
    good = [e for e in extras if e is not None]
    ltas = formants = None
    if good:
        ps = []
        for e in good:
            b = e["body"]; ps.append(welch_pow(b - b.mean(), sr, 16384))
        ltas = ltas_curve(ps)
        pk, pr = ss.find_peaks(ltas, prominence=1.0)
        top = sorted(pk, key=lambda i: -ltas[i])[:5]
        formants = [(LTAS_F[i], ltas[i]) for i in top]
        for i, (fz, db) in enumerate(formants): S["formant%d_hz" % (i + 1)] = float(fz); S["formant%d_db" % (i + 1)] = float(db)
        # peaks under about 300 Hz follow the notes' own fundamentals; the body and bridge-hill peaks are the ones from 300 Hz up
        hi = sorted([i for i in pk if LTAS_F[i] >= 300], key=lambda i: -ltas[i])[:5]
        for i, j in enumerate(hi): S["bodypeak%d_hz" % (i + 1)] = float(LTAS_F[j]); S["bodypeak%d_db" % (i + 1)] = float(ltas[j])
    return dict(label=label, rows=rows, S=S, ltas=ltas, formants=formants, extras=extras)


# ---------------------------------------------------------------- output
def unit_of(k):
    if k.endswith("_ms"): return "ms"
    if k.endswith("_s"): return "s"
    if k.endswith("_hz") or k.endswith("hz"): return "Hz"
    if k.endswith("_c") or k.endswith("_c_sd"): return "cents"
    if k.endswith("_db") or k.startswith("noise_db") or k.startswith("ltas_db") or (k[0] == "h" and k[1:].isdigit()): return "dB"
    if k.startswith("formant") and k.endswith("_db"): return "dB"
    if k.endswith("_frac") or k in ("vib_present",): return "fraction"
    return "ratio" if k.startswith("centroid_over") or k == "vib_regularity" else "count" if k == "n_notes" else ""


def summary_metrics(res):
    """ordered (metric, value, unit) list including curves."""
    S = res["S"]; out = [(k, v, unit_of(k)) for k, v in S.items()]
    if res["ltas"] is not None:
        out += [("ltas_db@%d" % round(f), float(v), "dB") for f, v in zip(LTAS_F, res["ltas"])]
    return out


def write_outputs(res, prefix):
    os.makedirs(os.path.dirname(os.path.abspath(prefix)), exist_ok=True)
    lab = res["label"]
    with open(prefix + ".tsv", "w") as f:
        f.write("metric\tvalue\tunit\tsource\n")
        for k, v, u in summary_metrics(res): f.write("%s\t%.6g\t%s\t%s\n" % (k, v, u, lab))
    rows = res["rows"]; keys = []
    for r in rows:
        for k in r:
            if k not in keys: keys.append(k)
    with open(prefix + "_notes.tsv", "w") as f:
        f.write("\t".join(keys) + "\n")
        for r in rows: f.write("\t".join(("%.6g" % r[k]) if k in r and not isinstance(r[k], str) else "nan" for k in keys) + "\n")
    with open(prefix + "_hseries.tsv", "w") as f:
        f.write("note\ttime_s\th1_abs_db\t" + "\t".join("h%d_db" % k for k in range(2, 17)) + "\n")
        for r, e in zip(rows, res["extras"]):
            if e is None or e["ldb"] is None: continue
            ldb = e["ldb"]
            for j in range(ldb.shape[1]):
                f.write("%d\t%.3f\t%.2f\t%s\n" % (r["note"], e["tfr"][j], ldb[0, j], "\t".join("%.2f" % (ldb[k, j] - ldb[0, j]) for k in range(1, 16))))
    S = res["S"]
    def g(k, fmt="%.2f"): return fmt % S[k] if k in S and not np.isnan(S[k]) else "-"
    md = ["# ALT-CELLO measurement: %s" % lab, "", "Notes measured: %d" % len(rows), "",
          "| metric | value |", "|---|---|"]
    for k, lbl in (("attack_ms", "attack ms (1 to 90 percent, reference definition)"), ("attack_10_90_ms", "attack ms (10 to 90 percent)"),
                   ("rel_t20_s", "release T20 s"), ("rel_t40_s", "release T40 s"), ("rolloff85_hz", "rolloff85 Hz"),
                   ("centroid_over_f0", "centroid / f0"), ("hnr_db", "HNR dB (autocorrelation)"), ("hnr_res_db", "HNR dB (residual)"),
                   ("bow_noise_db", "bow noise 2-8 kHz re total dB"), ("pitch_micro_c", "pitch micro-variation cents"),
                   ("level_micro_db", "level micro-variation dB"), ("vib_present_frac", "fraction of notes with vibrato"),
                   ("vib_rate_hz", "vibrato rate Hz"), ("vib_depth_c", "vibrato depth cents peak"), ("vib_onset_s", "vibrato onset s"),
                   ("vib_regularity", "vibrato regularity sd/mean"), ("wander_db", "harmonic wandering dB"), ("valley_db", "between-harmonics energy dB"),
                   ("h_sd_notes_db", "sd across notes of harmonic level dB"), ("attack_sd_ms", "sd across notes of attack ms")):
        md.append("| %s | %s |" % (lbl, g(k)))
    md += ["", "Harmonic levels re h1 (dB), median: " + ", ".join("h%d %s" % (k, g("h%d" % k, "%.1f")) for k in range(2, 17)), "",
           "Partial deviation from k*(partial 1), cents: " + ", ".join("%d: %s" % (k, g("dev%d_c" % k, "%.1f")) for k in range(2, 11)), ""]
    if res["formants"]:
        md.append("Five strongest peaks of the 1/6-octave LTAS envelope (Hz, dB re max): " + ", ".join("%.0f (%.1f)" % fd for fd in res["formants"]))
    md += ["", "Noise residual 1/3-octave shape (dB re total): " + ", ".join("%d Hz %s" % (round(fc), g("noise_db@%d" % round(fc), "%.1f")) for fc in THIRD_FC), ""]
    open(prefix + ".md", "w").write("\n".join(md) + "\n")


def read_tsv(path, source=None):
    d = {}
    for i, line in enumerate(open(path)):
        if i == 0: continue
        p = line.rstrip("\n").split("\t")
        if len(p) < 4 or (source is not None and p[3] != source): continue
        lo = float(p[4]) if len(p) > 5 and p[4] != "" else None; hi = float(p[5]) if len(p) > 5 and p[5] != "" else None
        d[p[0]] = (float(p[1]), p[2], lo, hi)
    return d


HEADLINE = [("attack_ms", "attack ms"), ("hnr_db", "HNR dB"), ("hnr_res_db", "HNR resid dB"), ("bow_noise_db", "bow noise dB"),
            ("vib_rate_hz", "vib rate Hz"), ("vib_depth_c", "vib depth c"), ("vib_onset_s", "vib onset s"),
            ("pitch_micro_c", "pitch micro c"), ("level_micro_db", "level micro dB"), ("wander_db", "wander dB"),
            ("valley_db", "between-harm dB"), ("h_sd_notes_db", "sd notes h dB"), ("rolloff85_hz", "rolloff85 Hz"), ("centroid_over_f0", "centroid/f0")]


def compare_figure(alt_prefix, target_tsv, title, out_png):
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    A = read_tsv(alt_prefix + ".tsv"); T = read_tsv(target_tsv, "TARGET")
    fig = plt.figure(figsize=(16, 11)); fig.suptitle(title, fontsize=18, fontweight="bold")
    gs = fig.add_gridspec(2, 2, hspace=0.32, wspace=0.18, top=0.9, bottom=0.07, left=0.06, right=0.97)
    ax = fig.add_subplot(gs[0, 0]); fz = [f for f in LTAS_F if "ltas_db@%d" % round(f) in A and "ltas_db@%d" % round(f) in T]
    for D, lb, col in ((T, "TARGET", "tab:blue"), (A, "ALT-CELLO", "tab:red")):
        ax.plot(fz, [D["ltas_db@%d" % round(f)][0] for f in fz], color=col, lw=2, label=lb)
    ax.set_xscale("log"); ax.set_xlabel("Hz"); ax.set_ylabel("dB re peak"); ax.set_title("long-term spectral envelope (1/6 octave)"); ax.legend(); ax.grid(alpha=0.3)
    ax.set_ylim(-70, 3)
    ax = fig.add_subplot(gs[0, 1]); ks = list(range(2, 17))
    for D, lb, col in ((T, "TARGET", "tab:blue"), (A, "ALT-CELLO", "tab:red")):
        ax.plot(ks, [D.get("h%d" % k, (np.nan,))[0] for k in ks], "o-", color=col, lw=2, label=lb)
    ax.set_xlabel("harmonic k"); ax.set_ylabel("dB re h1"); ax.set_title("median harmonic levels"); ax.legend(); ax.grid(alpha=0.3)
    ax = fig.add_subplot(gs[1, 0]); fc = [f for f in THIRD_FC if "noise_db@%d" % round(f) in A and "noise_db@%d" % round(f) in T]
    for D, lb, col in ((T, "TARGET", "tab:blue"), (A, "ALT-CELLO", "tab:red")):
        ax.plot(fc, [D["noise_db@%d" % round(f)][0] for f in fc], "o-", color=col, lw=2, label=lb)
    ax.set_xscale("log"); ax.set_xlabel("Hz"); ax.set_ylabel("dB re total"); ax.set_title("noise residual, 1/3-octave bands"); ax.legend(); ax.grid(alpha=0.3)
    ax = fig.add_subplot(gs[1, 1]); ax.axis("off")
    lines = ["%-18s %10s %18s %10s" % ("metric", "TARGET", "(min..max)", "ALT-CELLO")]
    for k, lb in HEADLINE:
        t = T.get(k); a = A.get(k)
        tv = "%.2f" % t[0] if t and not np.isnan(t[0]) else "-"
        sp = "(%.2f..%.2f)" % (t[2], t[3]) if t and t[2] is not None and not np.isnan(t[2]) else ""
        av = "%.2f" % a[0] if a and not np.isnan(a[0]) else "-"
        lines.append("%-18s %10s %18s %10s" % (lb, tv, sp, av))
    for k in ("formant1_hz", "formant2_hz", "formant3_hz"):
        t = T.get(k); a = A.get(k)
        lines.append("%-18s %10s %18s %10s" % (k, "%.0f" % t[0] if t else "-", "", "%.0f" % a[0] if a else "-"))
    ax.text(0.0, 1.0, "\n".join(lines), family="monospace", fontsize=12, va="top"); ax.set_title("metrics (TARGET median of trusted sources)")
    fig.savefig(out_png, dpi=110); plt.close(fig)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--wav", required=True); ap.add_argument("--notes"); ap.add_argument("--label", default="ALT-CELLO")
    ap.add_argument("--out", required=True); a = ap.parse_args()
    y = load_audio(a.wav)
    if a.notes: res = analyze_signal(y, SR, read_notes(a.notes), a.label)
    else:
        notes, fc = find_runs(y, SR); res = analyze_signal(y, SR, notes, a.label, f0_curve=fc)
    write_outputs(res, a.out); print("wrote", a.out + ".tsv", "and .md;", len(res["rows"]), "notes")


if __name__ == "__main__":
    main()

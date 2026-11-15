#!/usr/bin/env python3
"""describe.py - measure one voice (a set of transcribed notes) on its stem audio; measurements only, no verdicts.

What it does: for a pitched voice it measures, at the transcribed note times, on the stem audio:
  pitch range (lowest, highest, median note), note count, note length distribution, polyphony (median simultaneous
  voice notes at each onset); envelope per note by envelopes.py (attack 10-90 percent, peak to sustain, sustain level re
  peak, release); spectral shape from the median over the middle half of each note (levels of harmonics 1-12 in dB
  re harmonic 1, odd-to-even ratio of harmonics 3,5,7,9,11 over 2,4,6,8,10,12 in dB, centroid in Hz and as a multiple
  of f0, rolloff 85 percent, inharmonicity = mean absolute deviation in cents of partials 2-8 from integer multiples of
  the measured fundamental, noise share = energy not within 40 cents of a harmonic); brightness over time (centroid at
  0-50, 50-150, 150-400 ms and the last third of the note; trend falls/rises/holds with a 15 percent margin, and the
  correlation of centroid with level); pitch behaviour (vibrato rate and depth as the amplitude in cents of the
  detrended pitch track, onset delay; glide into the note in cents and ms; portamento share between consecutive notes
  less than 120 ms apart; fundamental-peak width in cents above the window's own resolution; beating rate from the
  amplitude modulation of the fundamental band, under 12 Hz); stereo side/mid ratio of the stem during the voice's notes;
  entrance (first note, whether the 2 s before it are silent in that stem, fade-in, filter opening); formant-like
  broad peaks of the harmonic envelope and whether their frequency stays put as f0 changes.
  For a drum voice: decay time to -20 dB, centroid, peak frequency, pitch drop (peak frequency of the first 10-30 ms
  minus that of 40-60 ms), noise share, and the 16-step pattern per bar.
How a note is measured: STFT (window max(0.04, min(0.2, 8/f0)) s, hop 10 ms) of the stem mono mix; the fundamental is
  tracked by harmonic summation (harmonics 1-4) starting from the transcribed pitch with a cap of 60 cents per frame,
  so the transcription's rounding and glides are followed; harmonic levels are the local maxima within 40 cents of each
  multiple; a note whose fundamental region is not 12 dB over the spectrum median is dropped as unconfirmed.
  Notes that sound alone in their stem (no other stem note overlapping by more than 30 ms) are preferred: if at least
  5 such notes of length 0.12 s or more exist only they are measured; otherwise up to --max-notes of all notes are
  measured and the result says so ("overlapped"); the harmonic-masked centroid is then used.
Inputs: Python API describe_voice(...) used by inventory.py; CLI measures one voice from a stem wav and a note CSV:
  describe.py STEM.wav NOTES.csv --start S --end E [--bpm B --first-beat T]
Output: a TSV-like list "name<TAB>value" on stdout.
Example: describe.py stems/htdemucs/se_12/vocals.wav transcribe/3_seoul/se_12_vocals_basic_pitch.csv --start 64 --end 78
Failure modes: basic-pitch notes can be wrong in pitch or octave (unconfirmed notes are counted and dropped); a stem leak
  puts other instruments in the voice; windows of 40-200 ms limit the vibrato rate range to about 3-9 Hz and notes
  shorter than 0.6 s give no vibrato measure; the glide and portamento numbers need a 10 ms frame grid so are good to
  about 15 ms; the formant-like peak uses a Gaussian smoothing of max(350 Hz, f0) over the harmonic levels, so above f0 of about 500 Hz the harmonic spacing is wider than a formant and the peak position is not resolvable; inharmonicity of low notes (under 100 Hz) is not resolvable and is reported as not measurable; every
  statistic prints its count and a quantity with under 5 notes is "not measurable". Deterministic; no randomness.
"""
import argparse
import os
import sys

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "transcribe"))
sys.path.insert(0, HERE)
import envelopes as env  # noqa: E402

NH = 12
HOP = 0.010
TOL_CENTS = 40.0
MIN_STAT = 5
GRID_T = np.arange(0, 1.5, 0.01)


def midi_hz(m):
    return 440.0 * 2 ** ((np.asarray(m, float) - 69) / 12.0)


def cents(a, b):
    return 1200.0 * np.log2(a / b)


def stat(vals, nmin=MIN_STAT):
    """dict(median, p10, p90, n) of finite values, or None when fewer than nmin."""
    v = np.asarray([x for x in vals if x is not None and np.isfinite(x)], float)
    if len(v) < nmin:
        return None if len(v) == 0 else dict(median=None, p10=None, p90=None, n=len(v))
    return dict(median=float(np.median(v)), p10=float(np.percentile(v, 10)), p90=float(np.percentile(v, 90)), n=len(v))


def alone_mask(notes, stem_notes, tol=0.030):
    """True for voice notes with no other note of the same stem overlapping by more than tol seconds."""
    res = np.zeros(len(notes), bool)
    for i, n in enumerate(notes):
        ov = (np.minimum(n[1], stem_notes[:, 1]) - np.maximum(n[0], stem_notes[:, 0])) > tol
        same = (np.abs(stem_notes[:, 0] - n[0]) < 1e-6) & (np.abs(stem_notes[:, 2] - n[2]) < 1e-6)
        res[i] = not (ov & ~same).any()
    return res


def _stft(seg, sr, win, hop=HOP):
    n = int(round(win * sr))
    h = int(round(hop * sr))
    if len(seg) < n:
        seg = np.pad(seg, (0, n - len(seg)))
    nf = 1 + (len(seg) - n) // h
    idx = np.arange(n)[None, :] + h * np.arange(nf)[:, None]
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    mag = np.abs(np.fft.rfft(seg[idx] * np.hanning(n), nfft)) * (2.0 / n)
    fr = np.fft.rfftfreq(nfft, 1.0 / sr)
    tc = (np.arange(nf) * h + n / 2) / sr
    return mag, fr, tc


def _track(mag, fr, f_nom, k0, floor_frac=0.04):
    """Fundamental track by harmonic summation, started at frame k0 and followed outward. Returns f[nframes] (nan where off)."""
    nf = mag.shape[0]
    wts = np.array([1.0, 0.8, 0.6, 0.4])
    off = np.arange(-60, 61, 3.0)

    def score(k, centers_hz):
        s = np.zeros(len(centers_hz))
        for h in range(4):
            if (h + 1) * centers_hz.max() < fr[-1]:
                s += wts[h] * np.sqrt(np.interp((h + 1) * centers_hz, fr, mag[k]))
        return s

    f = np.full(nf, np.nan)
    c0 = f_nom * 2 ** (np.arange(-100, 101, 4.0) / 1200)
    s0 = score(k0, c0)
    j = int(np.argmax(s0))
    f[k0] = c0[j]
    peak_score = s0[j]
    for direction in (1, -1):
        prev = f[k0]
        k = k0 + direction
        while 0 <= k < nf:
            c = prev * 2 ** (off / 1200)
            s = score(k, c)
            j = int(np.argmax(s))
            if s[j] < floor_frac * peak_score:
                break
            if 0 < j < len(s) - 1:  # parabolic refinement on the 3-cent grid
                d = s[j - 1] - 2 * s[j] + s[j + 1]
                fr_j = j + (0.5 * (s[j - 1] - s[j + 1]) / d if d < 0 else 0.0)
                prev = prev * 2 ** (np.interp(fr_j, np.arange(len(off)), off) / 1200)
            else:
                prev = c[j]
            f[k] = prev
            k += direction
    return f


def _harm_peaks(mag, fr, f, nh=NH):
    """Per frame, for h=1..nh: (peak amplitude, peak frequency) within a search band around h*f."""
    nf = len(f)
    A = np.zeros((nf, nh))
    F = np.full((nf, nh), np.nan)
    df = fr[1] - fr[0]
    for k in range(nf):
        if not np.isfinite(f[k]):
            continue
        for h in range(1, nh + 1):
            c = h * f[k]
            if c > fr[-1] - 3 * df:
                break
            spacing = 1200 * np.log2((h + 1.0) / h)
            half = min(70.0, 0.45 * spacing) if h > 1 else 70.0
            lo, hi = c * 2 ** (-half / 1200), c * 2 ** (half / 1200)
            i0, i1 = int(lo / df), int(hi / df) + 2
            seg = mag[k, i0:i1]
            if len(seg) < 3:
                continue
            j = int(np.argmax(seg))
            A[k, h - 1] = seg[j]
            if 0 < j < len(seg) - 1:
                d = seg[j - 1] - 2 * seg[j] + seg[j + 1]
                dj = 0.5 * (seg[j - 1] - seg[j + 1]) / d if d < 0 else 0.0
                F[k, h - 1] = (i0 + j + dj) * df
            else:
                F[k, h - 1] = (i0 + j) * df
    return A, F


def _hmask(fr, f0, win, nh_max, tol_c=TOL_CENTS):
    """Boolean over bins: within tol cents (or 1.5 bins of the window) of a multiple of f0."""
    m = np.zeros(len(fr), bool)
    tolhz_min = 1.5 / win
    for h in range(1, nh_max + 1):
        c = h * f0
        if c > fr[-1]:
            break
        tol = max(c * (2 ** (tol_c / 1200) - 1), tolhz_min)
        m |= np.abs(fr - c) <= tol
    return m


def _measure_note(x, sr, on, off, midi, prev_off, alone):
    """One note's raw measurements, or (None, reason)."""
    f_nom = float(midi_hz(midi))
    dur = off - on
    win = float(np.clip(8.0 / f_nom, 0.04, 0.2))
    t_lo = max(0.0, on - 0.15)
    t_hi = off + 0.05
    a, b = int((t_lo - win / 2) * sr), int((t_hi + win / 2) * sr)
    a = max(a, 0)
    seg = x[a:b].astype(np.float64)
    if len(seg) < int(0.08 * sr):
        return None, "too short"
    mag, fr, tc = _stft(seg, sr, win)
    tc = tc + a / sr
    mid = np.where((tc >= on + 0.25 * dur) & (tc <= on + 0.75 * dur))[0]
    if len(mid) == 0:
        mid = np.array([int(np.argmin(np.abs(tc - (on + off) / 2)))])
    k0 = int(mid[len(mid) // 2])
    f = _track(mag, fr, f_nom, k0)
    ok = np.isfinite(f)
    if ok.sum() < 3:
        return None, "pitch lost"
    # confirmation: the fundamental region stands out of the spectrum median
    df = fr[1] - fr[0]
    c = f[k0]
    band = mag[k0, int(c * 0.97 / df):int(c * 1.03 / df) + 2]
    med = np.median(mag[k0, int(20 / df):int(min(fr[-1], 8 * c) / df)]) + 1e-12
    if band.max() < 4.0 * med:
        return None, "unconfirmed"
    A, F = _harm_peaks(mag, fr, f)
    lvl = np.sqrt((A ** 2).sum(axis=1))
    lvl_db = 20 * np.log10(lvl + 1e-12)
    pk = lvl.max()
    # sustain frames
    sus = [k for k in mid if ok[k] and lvl[k] > 0.1 * pk]
    if len(sus) == 0:
        sus = [k0]
    f0s = float(np.median(f[sus]))
    # harmonic levels (dB re harmonic 1), median over sustain frames
    Hs = np.median(A[sus], axis=0)
    hl = 20 * np.log10((Hs + 1e-12) / (Hs[0] + 1e-12))
    odd = (Hs[[2, 4, 6, 8, 10]] ** 2).sum()
    even = (Hs[[1, 3, 5, 7, 9, 11]] ** 2).sum()
    oe = 10 * np.log10((odd + 1e-18) / (even + 1e-18))
    # inharmonicity: partials 2..8 above -35 dB of the strongest, relative to measured partial 1
    inh = np.nan
    if f0s >= 100:
        devs = []
        for h in range(2, 9):
            if Hs[h - 1] > 0.0178 * Hs.max():
                fh = np.nanmedian(F[sus, h - 1])
                f1 = np.nanmedian(F[sus, 0]) if Hs[0] > 0.0178 * Hs.max() else f0s
                if np.isfinite(fh) and np.isfinite(f1):
                    devs.append(abs(cents(fh, h * f1)))
        if len(devs) >= 2:
            inh = float(np.mean(devs))
    # whole-spectrum measures over the sustain frames
    nhm = max(1, int(min(40, 0.45 * sr / f0s)))
    hm = _hmask(fr, f0s, win, nhm)
    band_all = (fr >= 0.5 * f0s) & (fr <= min(10000.0, sr * 0.45))
    P = (mag[sus] ** 2).mean(axis=0)
    tot = P[band_all].sum() + 1e-30
    noise = float(P[band_all & ~hm].sum() / tot)
    if f0s < 4.0 / win * 1.0:
        noise = np.nan
    cen_full = float((fr[band_all] * np.sqrt(P[band_all])).sum() / (np.sqrt(P[band_all]).sum() + 1e-30))
    Ph = P * hm
    cen_h = float((fr[band_all] * np.sqrt(Ph[band_all])).sum() / (np.sqrt(Ph[band_all]).sum() + 1e-30))
    cen = cen_full if alone else cen_h
    cum = np.cumsum(P[band_all])
    roll = float(fr[band_all][np.searchsorted(cum, 0.85 * cum[-1])])
    b25 = (fr >= 2000) & (fr <= 5000)
    share_2_5k = float(P[b25].sum() / (P[(fr >= 20)].sum() + 1e-30))
    # per-frame centroid for brightness over time (same alone/masked choice)
    cen_t = np.full(len(tc), np.nan)
    for k in range(len(tc)):
        if not ok[k] or lvl[k] < 0.03 * pk:
            continue
        mk = band_all if alone else (band_all & _hmask(fr, f[k], win, nhm))
        m = mag[k][mk]
        cen_t[k] = float((fr[mk] * m).sum() / (m.sum() + 1e-30))
    rel_t = tc - on
    def cwin(t0, t1):
        sel = (rel_t >= t0) & (rel_t < t1) & np.isfinite(cen_t)
        return float(np.mean(cen_t[sel])) if sel.sum() else np.nan
    last0 = dur * (2.0 / 3.0)
    bright = [cwin(0, 0.05), cwin(0.05, 0.15), cwin(0.15, 0.4), cwin(last0, dur)] if dur >= 0.45 else [np.nan] * 4
    bl = np.nan
    sel = np.isfinite(cen_t) & (lvl_db > lvl_db.max() - 25) & (tc >= on) & (tc <= off)
    if sel.sum() >= 6 and np.std(cen_t[sel]) > 0 and np.std(lvl_db[sel]) > 0:
        bl = float(np.corrcoef(cen_t[sel], lvl_db[sel])[0, 1])
    # pitch track in cents re the sustain pitch
    ct = cents(f, f0s)
    # pitch behaviour: glide into note. Region: from the transcribed onset (minus 30 ms) to 0.5 s after it.
    # A glide is a gradual approach: the deviation at the region start is 40 cents or more and at least 3 frames
    # (30 ms) lie between 25 cents and 85 percent of that deviation; a step leaves no such frames.
    glide_c, glide_ms, glide_flag = np.nan, np.nan, False
    pre = np.where(ok & (tc < on + 0.5) & (tc >= on - 0.03))[0]
    if len(pre):
        first = pre[0]
        d0 = float(np.nanmedian(ct[first:first + 2]))
        glide_c, glide_ms = d0, 0.0
        if abs(d0) >= 40.0:
            dev = np.abs(ct[pre])
            ramp = int(((dev > 25.0) & (dev < 0.85 * abs(d0))).sum())
            settle = len(pre) - 1
            for j in range(len(pre) - 2):
                if (dev[j:j + 3] <= 25.0).all():
                    settle = j
                    break
            if ramp >= 3:
                glide_flag = True
                glide_ms = float((tc[pre[settle]] - tc[first]) * 1000.0)
    # vibrato
    vib = dict(rate=np.nan, depth=np.nan, delay=np.nan)
    inn = np.where(ok & (tc >= on + 0.05) & (tc <= off))[0]
    if dur >= 0.6 and len(inn) >= 50 and ok[inn[0]:inn[-1] + 1].all():
        y = ct[inn[0]:inn[-1] + 1].copy()
        sos = signal.butter(2, 2.5, btype="low", fs=100, output="sos")
        trend = signal.sosfiltfilt(sos, y)
        d = y - trend
        spec = np.abs(np.fft.rfft(d * np.hanning(len(d)), 1024))
        fq = np.fft.rfftfreq(1024, HOP)
        band = (fq >= 3.5) & (fq <= 9.5)
        kpk = int(np.argmax(np.where(band, spec, 0)))
        pw_band = (spec[band] ** 2).sum()
        pw_all = (spec[(fq >= 0.5) & (fq <= 12)] ** 2).sum() + 1e-30
        amp = 2.0 * spec[kpk] / np.hanning(len(d)).sum()
        if amp >= 6.0 and pw_band / pw_all > 0.5 and spec[kpk] > 3 * np.median(spec[(fq >= 0.5) & (fq <= 12)]):
            sosb = signal.butter(2, [3.5, 9.5], btype="band", fs=100, output="sos")
            ev = np.abs(signal.hilbert(signal.sosfiltfilt(sosb, d)))
            thr = 0.5 * np.median(ev[len(ev) // 2:])
            up = np.where(ev >= thr)[0]
            vib = dict(rate=float(fq[kpk]), depth=float(amp),
                       delay=float(tc[inn[0] + up[0]] - on) if len(up) else np.nan)
        else:
            vib = dict(rate=0.0, depth=float(amp), delay=np.nan)
    # fundamental peak width (cents above window resolution) and beating, on notes of 0.5 s or more
    width = np.nan
    beat = np.nan
    if dur >= 0.5 and f0s >= 60:
        a2, b2 = int((on + 0.1) * sr), int(min(off, on + 0.1 + 0.4) * sr)
        sg = x[a2:b2].astype(np.float64)
        if len(sg) >= int(0.3 * sr):
            n2 = 1 << 17
            S = np.abs(np.fft.rfft(sg * np.hanning(len(sg)), n2))
            f2 = np.fft.rfftfreq(n2, 1.0 / sr)
            m2 = (f2 > f0s * 2 ** (-100 / 1200)) & (f2 < f0s * 2 ** (100 / 1200))
            if m2.any():
                s2 = S[m2]
                fm = f2[m2]
                pkv = s2.max()
                above = np.where(s2 >= pkv / np.sqrt(2))[0]
                w_hz = fm[above[-1]] - fm[above[0]]
                w_c = 1200 * np.log2(1 + w_hz / f0s)
                res_c = 1200 * np.log2(1 + 1.44 / (len(sg) / sr) / f0s)
                width = float(np.sqrt(max(w_c ** 2 - res_c ** 2, 0.0)))
        if dur >= 0.8:
            sos = signal.butter(4, [f0s * 2 ** (-150 / 1200) / (sr / 2), f0s * 2 ** (150 / 1200) / (sr / 2)], btype="band", output="sos")
            a3, b3 = int((on + 0.1) * sr), int(off * sr)
            yb = signal.sosfiltfilt(sos, x[max(0, a3 - 2000):b3 + 2000].astype(np.float64))[2000:2000 + (b3 - a3)] if a3 >= 2000 else None
            if yb is not None and len(yb) > int(0.6 * sr):
                e = np.abs(signal.hilbert(yb))
                e = signal.decimate(e, max(1, int(sr // 500)), zero_phase=True)
                e = e / (e.mean() + 1e-12)
                e = e - e.mean()
                sp = np.abs(np.fft.rfft(e * np.hanning(len(e)), 8192))
                fq2 = np.fft.rfftfreq(8192, max(1, int(sr // 500)) / sr)
                bd = (fq2 >= 0.8) & (fq2 <= 12)
                kp = int(np.argmax(np.where(bd, sp, 0)))
                am = 2.0 * sp[kp] / np.hanning(len(e)).sum()
                if am >= 0.10 and sp[kp] > 4 * np.median(sp[bd]):
                    beat = float(fq2[kp])
    # formant-like envelope: smooth the harmonic amplitude envelope over Hz (Gaussian, 350 Hz)
    fpk = np.nan
    if alone and f0s < 1000:
        # extend to more harmonics for a useful envelope
        nh2 = min(30, int(0.45 * sr / f0s))
        Ah = np.zeros(nh2)
        for h in range(1, nh2 + 1):
            c = h * f0s
            i0, i1 = int(c * 0.985 / df), int(c * 1.015 / df) + 2
            Ah[h - 1] = np.median(mag[sus][:, i0:i1].max(axis=1)) if i1 > i0 else 0
        fh = np.arange(1, nh2 + 1) * f0s
        grid = np.arange(150, min(5000, fh[-1]), 25.0)
        sig = max(350.0, f0s)
        if len(grid) > 5 and fh[-1] > 1500:
            la = 20 * np.log10(Ah + 1e-9)
            sm = np.array([np.sum(np.exp(-0.5 * ((fh - g) / sig) ** 2) * la) / np.sum(np.exp(-0.5 * ((fh - g) / sig) ** 2)) for g in grid])
            pks, pr = signal.find_peaks(sm, prominence=2.0)
            if len(pks):
                fpk = float(grid[pks[np.argmax(pr["prominences"])]])
    # envelope curve on a common grid for plotting
    envc = np.interp(GRID_T, rel_t[ok], lvl_db[ok] - lvl_db[ok].max(), left=np.nan, right=np.nan)
    cenc = np.interp(GRID_T, rel_t[np.isfinite(cen_t)], cen_t[np.isfinite(cen_t)], left=np.nan, right=np.nan) \
        if np.isfinite(cen_t).sum() > 2 else np.full(len(GRID_T), np.nan)
    return dict(on=on, off=off, dur=dur, midi=midi, f0=f0s, hl=hl, oe=float(oe), inh=inh, noise=noise, cen=cen,
                cen_mult=cen / f0s, roll=roll, share_2_5k=share_2_5k, bright=bright, bright_level_corr=bl,
                glide_c=glide_c, glide_ms=glide_ms, glide=glide_flag, vib=vib, width=width, beat=beat, fpk=fpk,
                env_curve=envc, cen_curve=cenc, track_t=tc - on, track_c=ct, lvl_db=lvl_db, alone=alone,
                first_dev=glide_c), None


def _trend(b):
    """falls / rises / holds from the four centroid windows (0-50, 50-150, 150-400 ms, last third)."""
    if not np.isfinite(b[1]) or not np.isfinite(b[3]):
        return None
    r = b[3] / b[1]
    ref = b[1]
    if r < 0.85:
        return "falls"
    if r > 1.0 / 0.85:
        return "rises"
    return "holds"


def _ratio_fall(b):
    return b[3] / b[1] if np.isfinite(b[1]) and np.isfinite(b[3]) and b[1] > 0 else np.nan


def describe_voice(x_stereo, sr, notes, stem_notes, bpm=None, first_beat=None, max_notes=48, clip_end=None,
                   kind="pitched", min_dur=0.12):
    """Measurements of one pitched voice. x_stereo: stem array [n,2]; notes: voice notes (on, off, midi, vel);
    stem_notes: all notes of that stem (for the alone test). Returns a dict (see keys below) of stats with counts."""
    x = x_stereo.mean(axis=1)
    notes = notes[np.argsort(notes[:, 0], kind="stable")]
    R = dict(kind="pitched", n_notes=len(notes))
    midis = notes[:, 2]
    R["pitch"] = dict(lo=int(midis.min()), hi=int(midis.max()), median=int(round(np.median(midis))),
                      p10=int(round(np.percentile(midis, 10))), p90=int(round(np.percentile(midis, 90))))
    dur = notes[:, 1] - notes[:, 0]
    R["length_s"] = stat(dur, 1)
    # polyphony: voice notes sounding at each voice onset
    poly = [int(((notes[:, 0] <= n[0] + 1e-9) & (notes[:, 1] > n[0] + 0.02)).sum()) for n in notes]
    R["polyphony_median"] = float(np.median(poly))
    # stem polyphony at those onsets
    sp = [int(((stem_notes[:, 0] <= n[0] + 1e-9) & (stem_notes[:, 1] > n[0] + 0.02)).sum()) for n in notes]
    R["stem_polyphony_median"] = float(np.median(sp))
    alone = alone_mask(notes, stem_notes)
    R["alone_notes"] = int(alone.sum())
    cand = np.where(dur >= min_dur)[0]
    ca = [i for i in cand if alone[i]]
    if len(ca) >= MIN_STAT:
        chosen, R["measured_on"] = ca, "alone"
    else:
        chosen, R["measured_on"] = list(cand), "overlapped"
    if len(chosen) > max_notes:
        chosen = [chosen[int(round(i))] for i in np.linspace(0, len(chosen) - 1, max_notes)]
    per = []
    drop = {}
    for i in chosen:
        prev = notes[i - 1, 1] if i > 0 else None
        if prev is not None and notes[i, 0] - prev > 0.15:
            prev = None
        r, why = _measure_note(x, sr, notes[i, 0], notes[i, 1], notes[i, 2], prev if prev is not None else None, bool(alone[i]))
        if r is None:
            drop[why] = drop.get(why, 0) + 1
        else:
            per.append(r)
    R["measured_n"] = len(per)
    R["dropped"] = drop
    R["per_note"] = per
    # envelopes.py measurements
    envs = []
    for r in per:
        later = notes[notes[:, 0] > r["on"] + 0.03, 0]
        later_s = stem_notes[stem_notes[:, 0] > r["on"] + 0.03, 0]
        nxt = float(min(later.min() if len(later) else 1e9, later_s.min() if len(later_s) else 1e9))
        m = env.measure_note(x, sr, r["on"], r["off"], r["midi"], None if nxt > 1e8 else nxt)
        if m is not None:
            envs.append(m)
    R["attack_s"] = stat([m[0] for m in envs])
    R["peak_to_sus_s"] = stat([m[1] for m in envs])
    R["sustain_db"] = stat([m[2] for m in envs])
    R["release_s"] = stat([m[3] for m in envs])
    R["env_n"] = len(envs)
    # spectral shape
    if per:
        hl = np.array([r["hl"] for r in per])
        R["harm_db"] = [float(np.median(hl[:, h])) for h in range(NH)]
        R["harm_db_p10"] = [float(np.percentile(hl[:, h], 10)) for h in range(NH)]
        R["harm_db_p90"] = [float(np.percentile(hl[:, h], 90)) for h in range(NH)]
        R["odd_even_db"] = stat([r["oe"] for r in per])
        R["centroid_hz"] = stat([r["cen"] for r in per])
        R["centroid_mult"] = stat([r["cen_mult"] for r in per])
        R["rolloff_hz"] = stat([r["roll"] for r in per])
        R["inharm_cents"] = stat([r["inh"] for r in per])
        ih = [(r["f0"], r["inh"]) for r in per if np.isfinite(r["inh"])]
        R["inharm_pitch_corr"] = None
        if len(ih) >= 8:
            a_, b_ = np.log2([i[0] for i in ih]), np.array([i[1] for i in ih])
            if np.std(a_) > 0 and np.std(b_) > 0:
                R["inharm_pitch_corr"] = float(np.corrcoef(a_, b_)[0, 1])
        R["noise_share"] = stat([r["noise"] for r in per])
        R["share_2_5k"] = stat([r["share_2_5k"] for r in per])
        # harmonic slope: least squares of dB against log2(h) over harmonics within -45 dB of harmonic 1 (h1..12)
        sl = []
        for r in per:
            h = np.arange(1, NH + 1)
            ok = r["hl"] > -45
            ok[0] = True
            if ok.sum() >= 4:
                sl.append(float(np.polyfit(np.log2(h[ok]), r["hl"][ok], 1)[0]))
        R["slope_db_oct"] = stat(sl)
        R["n_harm_30db"] = stat([int((r["hl"] > -30).sum()) for r in per])
        # brightness
        bc = []
        for k in range(4):
            bc.append(stat([r["bright"][k] for r in per]))
        R["bright_windows_hz"] = bc
        ratios = [_ratio_fall(r["bright"]) for r in per]
        R["bright_last_over_50_150"] = stat(ratios)
        trends = [_trend(r["bright"]) for r in per]
        tt = [t for t in trends if t]
        R["bright_trend_counts"] = {k: tt.count(k) for k in ("falls", "rises", "holds")}
        if len(tt) >= MIN_STAT:
            c = R["bright_trend_counts"]
            top = max(c, key=c.get)
            R["bright_trend"] = top if c[top] >= 0.5 * len(tt) else "mixed"
        else:
            R["bright_trend"] = None
        R["bright_level_corr"] = stat([r["bright_level_corr"] for r in per])
        # pitch behaviour
        vr = [r["vib"]["rate"] for r in per if np.isfinite(r["vib"]["rate"])]
        R["vib_notes_measured"] = len(vr)
        vib = [r["vib"] for r in per if r["vib"]["rate"] and np.isfinite(r["vib"]["rate"]) and r["vib"]["rate"] > 0]
        R["vib_share"] = (len(vib) / len(vr)) if len(vr) >= MIN_STAT else None
        R["vib_rate_hz"] = stat([v["rate"] for v in vib], 3)
        R["vib_depth_cents"] = stat([v["depth"] for v in vib], 3)
        R["vib_delay_s"] = stat([v["delay"] for v in vib], 3)
        gl = [r for r in per if np.isfinite(r["glide_c"])]
        R["glide_n"] = len(gl)
        R["glide_share"] = (sum(1 for r in gl if r["glide"]) / len(gl)) if len(gl) >= MIN_STAT else None
        R["glide_cents"] = stat([abs(r["glide_c"]) for r in gl if r["glide"]], 3)
        R["glide_ms"] = stat([r["glide_ms"] for r in gl if r["glide"]], 3)
        # portamento: consecutive voice notes less than 120 ms apart with a start deviation toward the previous pitch
        port = []
        by_on = {round(r["on"], 4): r for r in per}
        for i in range(1, len(notes)):
            r = by_on.get(round(notes[i, 0], 4))
            gap = notes[i, 0] - notes[i - 1, 1]
            if r is None or gap > 0.12 or not np.isfinite(r["glide_c"]):
                continue
            step = notes[i - 1, 2] - notes[i, 2]
            if abs(step) < 1:
                continue
            port.append(1 if (r["glide"] and np.sign(r["glide_c"]) == np.sign(step)) else 0)
        R["portamento_n"] = len(port)
        R["portamento_share"] = (sum(port) / len(port)) if len(port) >= MIN_STAT else None
        R["fund_width_cents"] = stat([r["width"] for r in per])
        R["beat_hz"] = stat([r["beat"] for r in per if np.isfinite(r["beat"])], 3)
        R["beat_notes_measured"] = int(sum(1 for r in per if r["dur"] >= 0.8))
        R["beat_share"] = (sum(1 for r in per if np.isfinite(r["beat"])) / max(1, R["beat_notes_measured"])) if R["beat_notes_measured"] >= MIN_STAT else None
        # formant-like peaks
        fp = [(r["f0"], r["fpk"]) for r in per if np.isfinite(r["fpk"])]
        R["formant"] = None
        if len(fp) >= 6:
            f0v = np.array([a for a, b in fp])
            pv = np.array([b for a, b in fp])
            spread = 12 * np.log2(f0v.max() / f0v.min())
            corr = float(np.corrcoef(np.log2(f0v), np.log2(pv))[0, 1]) if np.std(f0v) > 0 and np.std(pv) > 0 else np.nan
            R["formant"] = dict(n=len(fp), peak_hz_median=float(np.median(pv)), peak_hz_p10=float(np.percentile(pv, 10)),
                                peak_hz_p90=float(np.percentile(pv, 90)), cv=float(np.std(pv) / np.mean(pv)),
                                corr_with_f0=corr, f0_spread_semitones=float(spread),
                                moves_with_f0=(None if spread < 4 or not np.isfinite(corr) else
                                               True if corr > 0.6 else False if np.std(pv) / np.mean(pv) < 0.2 else None),
                                f0_median=float(np.median(f0v)))
        # curves
        for key, name in (("env_curve", "median_env_db"), ("cen_curve", "median_centroid_hz")):
            M = np.array([r[key] for r in per])
            with np.errstate(all="ignore"):
                cnt = np.isfinite(M).sum(axis=0)
                med = np.nanmedian(np.where(cnt[None, :] >= max(3, len(per) // 3), M, np.nan), axis=0)
            R[name] = med
        longest = sorted([r for r in per if r["alone"] and r["dur"] >= 0.3], key=lambda r: -r["dur"])[:3]
        if len(longest) < 3:
            longest = sorted(per, key=lambda r: -r["dur"])[:3]
        R["pitch_tracks"] = [(r["track_t"], r["track_c"], r["midi"], r["dur"]) for r in longest]
    # stereo side/mid during notes
    mask = np.zeros(len(x), bool)
    for n in notes:
        mask[int(n[0] * sr):int(n[1] * sr)] = True
    if mask.sum() > int(0.2 * sr):
        L, Rr = x_stereo[:, 0][mask].astype(np.float64), x_stereo[:, 1][mask].astype(np.float64)
        mid, side = (L + Rr) / 2, (L - Rr) / 2
        R["side_mid_db"] = float(20 * np.log10((np.sqrt(np.mean(side ** 2)) + 1e-12) / (np.sqrt(np.mean(mid ** 2)) + 1e-12)))
    else:
        R["side_mid_db"] = None
    R["entrance"] = entrance(x, sr, notes[0, 0], notes)
    return R


def entrance(x, sr, t_first, notes):
    """First voice note, silence of the 2 s before it in this stem, fade-in and filter-opening checks."""
    E = dict(t=float(t_first))
    pre0 = max(0.0, t_first - 2.0)
    seg = x[int(pre0 * sr):int(t_first * sr)]
    if t_first < 0.1:
        E["pre_silent"] = None
        E["pre_note"] = "note starts at the clip start"
    else:
        r = 20 * np.log10(np.sqrt(np.mean(seg.astype(np.float64) ** 2)) + 1e-12)
        act = x[int(t_first * sr):int(min(len(x) / sr, t_first + 4) * sr)]
        ra = 20 * np.log10(np.sqrt(np.mean(act.astype(np.float64) ** 2)) + 1e-12)
        E["pre_db"] = float(r)
        E["after_db"] = float(ra)
        E["pre_len_s"] = float(t_first - pre0)
        E["pre_silent"] = bool(r < -60.0 or r < ra - 40.0)
    seg2 = x[int(t_first * sr):int(min(len(x) / sr, t_first + 2.0) * sr)].astype(np.float64)
    hop = int(0.25 * sr)
    if len(seg2) >= 4 * hop:
        lv = np.array([20 * np.log10(np.sqrt(np.mean(seg2[i:i + hop] ** 2)) + 1e-12) for i in range(0, len(seg2) - hop + 1, hop)])
        t = np.arange(len(lv))
        c = np.corrcoef(t, lv)[0, 1] if np.std(lv) > 0 else 0.0
        E["fade_in"] = bool(lv[-1] - lv[0] >= 6.0 and c > 0.6)
        E["level_rise_db"] = float(lv[-1] - lv[0])
        cen = []
        for i in range(0, len(seg2) - hop + 1, hop):
            m = np.abs(np.fft.rfft(seg2[i:i + hop] * np.hanning(hop)))
            f = np.fft.rfftfreq(hop, 1.0 / sr)
            cen.append((f * m).sum() / (m.sum() + 1e-12))
        cen = np.array(cen)
        c2 = np.corrcoef(t, cen)[0, 1] if np.std(cen) > 0 else 0.0
        E["filter_opening"] = bool(cen[-1] > 1.25 * cen[0] and c2 > 0.6)
        E["centroid_rise"] = float(cen[-1] / (cen[0] + 1e-9))
    else:
        E["fade_in"] = None
        E["filter_opening"] = None
    return E


# ---------------------------------------------------------------- drums

def describe_drum(x_stereo, sr, onsets, name, grid=None):
    """Drum voice measurements from onset times (s). grid: list of bar start times (sorted) with bar length, or None."""
    x = x_stereo.mean(axis=1).astype(np.float64)
    R = dict(kind="drum", n_notes=len(onsets), name=name)
    dec, cen, pkf, drop, noi = [], [], [], [], []
    envs, specs = [], []
    lg = np.geomspace(30, 16000, 60)
    for t in onsets:
        a = int(t * sr)
        seg = x[a:a + int(0.5 * sr)]
        if len(seg) < int(0.1 * sr):
            continue
        e = np.sqrt(np.convolve(seg ** 2, np.ones(int(0.004 * sr)) / int(0.004 * sr), mode="same"))
        pk = int(np.argmax(e[:int(0.08 * sr)]))
        pv = e[pk]
        if pv < 1e-5:
            continue
        below = np.where(e[pk:] <= pv * 0.1)[0]
        dec.append(below[0] / sr if len(below) else np.nan)
        w = seg[:int(0.1 * sr)] * np.hanning(int(0.1 * sr))
        m = np.abs(np.fft.rfft(w, 16384))
        f = np.fft.rfftfreq(16384, 1.0 / sr)
        k = f >= 20
        cen.append(float((f[k] * m[k]).sum() / (m[k].sum() + 1e-20)))
        pkf.append(float(f[k][np.argmax(m[k])]))
        def pf(t0, t1):
            s = x[int((t + t0) * sr):int((t + t1) * sr)]
            if len(s) < 64:
                return np.nan
            mm = np.abs(np.fft.rfft(s * np.hanning(len(s)), 16384))
            kk = (f >= 25) & (f <= 1500)
            return float(f[kk][np.argmax(mm[kk])])
        drop.append(pf(0.003, 0.030) - pf(0.040, 0.070))
        tg = GRID_T[GRID_T < 0.49]
        envs.append(np.concatenate([20 * np.log10(e[np.minimum((tg * sr).astype(int), len(e) - 1)] / pv + 1e-9), np.full(len(GRID_T) - len(tg), np.nan)]))
        sp = 20 * np.log10(np.interp(lg, f, m) + 1e-9)
        specs.append(sp - sp.max())
        mp = m[k] ** 2
        top = np.sort(mp)[::-1][:12].sum()
        noi.append(float(1 - top / (mp.sum() + 1e-30)))
    if envs:
        with np.errstate(all="ignore"):
            R["median_env_db"] = np.nanmedian(np.array(envs), axis=0)
        R["median_spec_db"] = np.median(np.array(specs), axis=0)
        R["spec_freqs"] = lg
    R["decay_20db_s"] = stat(dec)
    R["centroid_hz"] = stat(cen)
    R["peak_hz"] = stat(pkf)
    R["pitch_drop_hz"] = stat(drop)
    R["noise_share"] = stat(noi)
    R["pattern"] = None
    if grid:
        starts, blen, bpb = grid
        steps = 16 if bpb == 4 else 12
        cnt = np.zeros(steps)
        nb = 0
        for s in starts:
            inb = [t for t in onsets if s <= t < s + blen]
            nb += 1
            for t in inb:
                cnt[min(steps - 1, int(round((t - s) / blen * steps)) % steps)] += 1
        if nb:
            share = cnt / nb
            R["pattern"] = dict(steps=steps, bars=nb, share=[float(v) for v in share],
                                line="".join("x" if v >= 0.5 else ("o" if v >= 0.25 else ".") for v in share))
    return R


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stem")
    ap.add_argument("notes")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", type=float)
    a = ap.parse_args()
    from scipy.io import wavfile
    sys.path.insert(0, HERE)
    import voices
    x, sr = voices.load_stem(a.stem)
    allm = env.read_notes(a.notes)
    we = a.end if a.end else len(x) / sr
    nn = voices.window_notes(allm, a.start, we)
    R = describe_voice(x, sr, nn, allm)
    for k, v in R.items():
        if k in ("per_note",) or isinstance(v, np.ndarray):
            continue
        print("%s\t%s" % (k, v))


if __name__ == "__main__":
    main()

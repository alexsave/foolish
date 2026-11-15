#!/usr/bin/env python3
"""extend_v7.py - turns a flat v6 sample into a LONG v7 sample (about 12 s) that a note never has to loop.

The v6 sample is split into two parts:
  h  the steady harmonic part: for every harmonic k of the refined fundamental f the complex amplitude c_k (per channel) measured over the
     sustain (Hann weighted projection), h(t) = sum_k 2 Re(c_k exp(j 2 pi k f t)). The v6 phase lock makes the sustain exactly that.
  r  everything else, r = x - h: bow and breath noise, the attack and the release (their harmonic deviations from the steady tone).
The output is h' + r' where
  h' is the same harmonic sum continued for the whole length, with a tiny slow drift per note (pitch below DRIFT_HZ, a few tenths of a
     cent, level about 0.05 to 0.1 dB, each harmonic a little on its own), fading to exactly zero over the splice zones;
  r' is the sample's own r for the attack (the first ta seconds) and for the release tail (the last tr seconds, moved to the end),
     and FRESH noise in between: a Gaussian noise whose per band power spectrum, left to right cross spectrum and slow band level
     modulation (an AR(1) process in dB per band, measured sigma and correlation time) are those measured on r over the sustain.
     The fresh noise is never a copy of measured noise, so no pattern repeats.
Splices: the tail moves by D samples, D = m periods of the fundamental EXACTLY (f is nudged by under 1e-5 relative so that D is an integer
number of samples), so h at the tail is the same sequence of values as in the source: the harmonic part needs no crossfade at all, and the
noise is crossfaded equal power (the two noises are uncorrelated, so equal power is the matched law). Outside the two fade zones the
output equals the v6 sample sample for sample (test_extend_v7.py null test).
Random: only through v7_rng (one seed constant, one stream per sample name). Usage: python3 -I extend_v7.py IN.wav OUT.wav --root MIDI [--keep-attack-ms N]"""
import argparse, os, sys
import numpy as np
from scipy.interpolate import CubicSpline
from scipy.signal import butter, sosfiltfilt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod
import v7_rng

LENGTH_S = 12.0        # target length: a 4 beat cello note at 50 BPM is 4.8 s, up to a 1.5 times faster read still never reaches the loop zone
ATTACK_S = 0.45        # kept natural attack (at least; keep-attack-ms + 0.2 s if larger)
TAIL_S = 0.25          # kept natural release tail
FADE_S = 0.12          # noise crossfade at each splice
KMAX = 40
F_MAX = 10000.0
REL_FLOOR_DB = -45.0
NFFT_LOW = 150.0       # below this fundamental the noise is analysed and made with 4096 point frames (10.8 Hz bins) so that the narrow skirts of the harmonics keep their width
BANDS = [0, 300, 800, 2000, 4500, 10000, 1e9]   # band edges (Hz) of the slow noise level modulation
LOW_CUT_HZ = 30.0      # the fresh noise has nothing under this (inaudible, and a slow wiggle that is coherent for 100 ms reads as an echo)
DRIFT_HZ = 0.8
DRIFT_PITCH_C = 0.10   # std in cents of the slow pitch drift (peak about 3 sigma = 0.3 c)
DRIFT_LEVEL_DB = 0.03  # std in dB of the common slow level drift (peak about 0.1 dB)
DRIFT_HARM_DB = 0.02   # std in dB of each harmonic's own slow level drift
DRIFT_RAMP_S = 1.0


def zones(N0, sr, keep_s):
    d = N0 / sr
    ta = min(max(ATTACK_S, keep_s + 0.2), 0.45 * d)
    tr = min(TAIL_S, 0.25 * d)
    xf = min(FADE_S, 0.1 * d)
    return int(round(ta * sr)), int(round(tr * sr)), int(round(xf * sr))


def estimate(x, sr, f_guess, i0, i1):
    """Refined fundamental from the phase slope of harmonics 1 to 8 over [i0, i1), then c[ch][k] at that frequency."""
    mono = x.mean(axis=1)[i0:i1]
    f = vibmod.refine_f0(mono, sr, f_guess)
    for _ in range(3):
        sl, wt = [], []
        for k in range(1, 9):
            if k * f > 0.45 * sr:
                break
            y = vibmod._het(mono, sr, k * f)
            e = int(0.1 * vibmod.DEC)
            ph = np.unwrap(np.angle(y))[e:-e] if len(y) > 2 * e + 8 else np.unwrap(np.angle(y))
            a = np.abs(y)[e:-e] if len(y) > 2 * e + 8 else np.abs(y)
            t = np.arange(len(ph)) / vibmod.DEC
            s = np.polyfit(t, ph, 1)[0]
            sl.append(s / (2 * np.pi * k)); wt.append(np.median(a))
        f += float(np.average(sl, weights=wt))
    return f


def project(x, sr, f, i0, i1, ks):
    """c[ch, k] with x ~ sum_k 2 Re(c_k exp(j 2 pi k f t)), t counted from the sample start, Hann weighted over [i0, i1)."""
    n = np.arange(i0, i1)
    w = np.hanning(len(n) + 2)[1:-1]
    c = np.zeros((x.shape[1], len(ks)), complex)
    for j, k in enumerate(ks):
        e = np.exp(-2j * np.pi * k * f * n / sr) * w
        for ch in range(x.shape[1]):
            c[ch, j] = np.sum(x[i0:i1, ch] * e) / w.sum()
    return c


def harm_sum(c, ks, f, sr, n, theta=None, gain_db=None):
    """h[n, ch]. theta (cycles of the fundamental, length n) and gain_db (n, or [len(ks), n]) are the drifts (None = steady)."""
    t = np.arange(n) / sr
    out = np.zeros((n, c.shape[0]))
    for j, k in enumerate(ks):
        ph = 2 * np.pi * k * (f * t + (theta if theta is not None else 0.0))
        e = np.exp(1j * ph)
        g = 1.0
        if gain_db is not None:
            g = 10 ** (gain_db[0] / 20.0 + (gain_db[1][j] / 20.0 if gain_db[1] is not None else 0.0))
        for ch in range(c.shape[0]):
            out[:, ch] += 2 * (c[ch, j] * e).real * g
    return out


def slow_noise(rng, n_out, sr, hz=DRIFT_HZ, fs=50.0):
    """Gaussian noise low passed (4th order Butterworth) below hz, made at fs and read at sr with a cubic spline; unit std."""
    m = int(n_out / sr * fs) + 400
    v = sosfiltfilt(butter(4, hz, fs=fs, output="sos"), rng.standard_normal(m))[200:-200]
    v = v / v.std()
    return CubicSpline(np.arange(len(v)) / fs, v)(np.arange(n_out) / sr)


def ramp_window(n, a, b, ramp):
    """0 before a, raised cosine up over ramp samples, 1, raised cosine down over ramp samples ending at b, 0 after."""
    w = np.zeros(n)
    r = max(1, min(ramp, (b - a) // 2))
    s = 0.5 - 0.5 * np.cos(np.pi * np.arange(r) / r)
    w[a:b] = 1.0
    w[a:a + r] = s
    w[b - r:b] = s[::-1]
    return w


def frame_size(f):
    n = 4096 if f < NFFT_LOW else 2048
    return n, n // 4


def het_residual(x, sr, f, kmax=KMAX):
    """x minus its harmonics 1 to kmax tracked with a heterodyne of bandwidth min(40 Hz, 0.35 f) (residual_check.residual, per channel).
    What is left is the noise OUTSIDE the narrow bands around the harmonics. The bandwidth is the project's definition of 'harmonic'
    (flatten_v6 removes every modulation inside it), so the slow wobble of a harmonic is not re-made as noise."""
    t = np.arange(len(x)) / sr
    sos = butter(4, min(40.0, 0.35 * f), fs=sr, output="sos")
    x = sosfiltfilt(butter(4, LOW_CUT_HZ, btype="high", fs=sr, output="sos"), x, axis=0)   # rumble under 30 Hz is not modelled
    out = x.copy()
    for k in range(1, kmax + 1):
        if k * f > 0.45 * sr:
            break
        e = np.exp(-2j * np.pi * k * f * t)
        for ch in range(x.shape[1]):
            z = x[:, ch] * e
            z = sosfiltfilt(sos, z.real) + 1j * sosfiltfilt(sos, z.imag)
            out[:, ch] -= 2 * (z * np.conj(e)).real
    return out


def fill_notches(P, fq, f, bw):
    """P is the power matrix of the het residual, which went through the measurement filter: the noise within +-bw of a harmonic is gone and
    the noise beyond it is attenuated by (1 - G)^2 with G = 1 / (1 + (d / bw)^8) the zero phase response of the 4th order filter at the distance
    d from the nearest harmonic. Undo that (at most 6 dB, d >= bw) and fill the bins nearer than bw by interpolation (log power; the complex
    coherence) between the corrected bins, so that the fresh noise, measured by the same filter, reads as the source's residual does."""
    ks = np.arange(1, KMAX + 1)
    d = np.abs(fq[:, None] - ks[None, :] * f).min(axis=1)
    G = 1.0 / (1.0 + (d / bw) ** 8)
    corr = np.maximum((1.0 - G) ** 2, 0.25)
    mask = d < bw
    mask[:2] = False
    ok = ~mask
    if mask.sum() == 0 or ok.sum() < 8:
        return P
    g = P[2] / np.sqrt(np.maximum(P[0] * P[1], 1e-30))
    out = P.copy()
    for i in (0, 1):
        pc = P[i] / corr
        out[i] = np.exp(np.interp(fq, fq[ok], np.log(np.maximum(pc[ok], 1e-30))))
        out[i][ok] = pc[ok]
    gr = np.interp(fq, fq[ok], g.real[ok]); gi = np.interp(fq, fq[ok], g.imag[ok])
    gg = np.where(mask, gr + 1j * gi, g)
    out[2] = gg * np.sqrt(out[0] * out[1])
    return out


def noise_model(r, sr, e0, e1, NFFT, HOP, f=None):
    """Per bin power matrices and the slow band modulation measured on r[e0:e1] (two channels)."""
    win = np.sqrt(np.hanning(NFFT + 1)[:-1])
    fr = [np.fft.rfft(r[a:a + NFFT].T * win, axis=1) for a in range(e0, e1 - NFFT, HOP)]
    fr = np.array(fr)           # [frames, 2, bins]
    nb = fr.shape[2]
    L, R = fr[:, 0], fr[:, 1]
    P = np.array([np.mean(np.abs(L) ** 2, 0), np.mean(np.abs(R) ** 2, 0), np.mean(L * np.conj(R), 0)])
    ker = np.array([1, 2, 3, 2, 1], float) / 9.0 if len(fr) < 24 else np.array([1, 2, 1], float) / 4.0
    P = np.array([np.convolve(np.pad(p, (len(ker) // 2,) * 2, mode="edge"), ker, mode="valid") for p in P])
    if f is not None:
        P = fill_notches(P, np.fft.rfftfreq(NFFT, 1.0 / sr), f, min(40.0, 0.35 * f))
    P[2] = np.where(np.abs(P[2]) > 0.98 * np.sqrt(P[0] * P[1]), P[2] * 0.98 * np.sqrt(P[0] * P[1]) / np.maximum(np.abs(P[2]), 1e-30), P[2])
    fq = np.fft.rfftfreq(NFFT, 1.0 / sr)
    sig, rho = [], []
    fps = sr / HOP
    for lo, hi in zip(BANDS[:-1], BANDS[1:]):
        m = (fq >= lo) & (fq < hi)
        g = 10 * np.log10(np.maximum((np.abs(L[:, m]) ** 2 + np.abs(R[:, m]) ** 2).sum(1), 1e-30))
        g = g - np.polyval(np.polyfit(np.arange(len(g)), g, 1), np.arange(len(g))) if len(g) > 8 else g - g.mean()
        s = float(np.clip(g.std(), 0.2, 2.5))
        lag = 4
        r4 = float(np.corrcoef(g[:-lag], g[lag:])[0, 1]) if len(g) > 12 and g.std() > 0 else 0.5
        r4 = float(np.clip(r4 if np.isfinite(r4) else 0.5, 0.05, 0.98))
        tau = float(np.clip(-lag / (fps * np.log(r4)), 0.05, 1.0))
        sig.append(s); rho.append(tau)
    return P, sig, rho


def synth_noise(rng, P, sig, tau, sr, n, NFFT, HOP):
    """Fresh stereo noise [n, 2] with the power matrices P and the slow band modulation (sig dB, tau s)."""
    win = np.sqrt(np.hanning(NFFT + 1)[:-1])
    ew = float((win ** 2).sum())
    pad = NFFT
    nt = n + 2 * pad
    z = rng.standard_normal((2, nt))
    nfr = (nt - NFFT) // HOP + 1
    fq = np.fft.rfftfreq(NFFT, 1.0 / sr)
    nb = len(fq)
    fps = sr / HOP
    # band gains over time (dB), AR(1), mean power compensated, then interpolated over log frequency
    cen = np.array([np.sqrt(max(lo, 100.0) * min(hi, 20000.0)) for lo, hi in zip(BANDS[:-1], BANDS[1:])])
    gb = np.zeros((nfr, len(sig)))
    for b in range(len(sig)):
        rho = np.exp(-1.0 / (fps * tau[b]))
        e = rng.standard_normal(nfr)
        g = np.zeros(nfr)
        g[0] = sig[b] * e[0]
        for t in range(1, nfr):
            g[t] = rho * g[t - 1] + np.sqrt(1 - rho ** 2) * sig[b] * e[t]
        gb[:, b] = g - 0.5 * (sig[b] * np.log(10) / 10.0) ** 2 * 10.0 / np.log(10)   # E[10^(g/10)] = 1
    lf = np.log(np.maximum(fq, 50.0))
    gdb = np.array([np.interp(lf, np.log(cen), gb[t]) for t in range(nfr)])     # [frames, bins]
    A = np.sqrt(np.maximum(P[0], 1e-30) / ew)
    B = np.conj(P[2]) / (A * ew)
    C = np.sqrt(np.maximum(P[1] - np.abs(B) ** 2 * ew, 1e-30) / ew)
    out = np.zeros((2, nt))
    nrm = np.zeros(nt)
    for t in range(nfr):
        a = t * HOP
        Z1 = np.fft.rfft(z[0, a:a + NFFT] * win)
        Z2 = np.fft.rfft(z[1, a:a + NFFT] * win)
        g = 10 ** (gdb[t] / 20.0)
        XL = A * Z1 * g
        XR = (B * Z1 + C * Z2) * g
        XL[fq < LOW_CUT_HZ] = 0
        XR[fq < LOW_CUT_HZ] = 0
        out[0, a:a + NFFT] += np.fft.irfft(XL, NFFT) * win
        out[1, a:a + NFFT] += np.fft.irfft(XR, NFFT) * win
        nrm[a:a + NFFT] += win ** 2
    # sqrt Hann analysis and synthesis at 75 percent overlap: the window power sums to 2 (a constant), normalise it exactly
    nrm = np.maximum(nrm, 1e-9)
    out /= nrm
    return out[:, pad:pad + n].T


def extend(x, sr, root, name, keep_ms=0.0):
    """x: v6 sample [n, 2]. Returns (y, info) the long sample and its numbers."""
    N0 = len(x)
    ta, tr, xf = zones(N0, sr, keep_ms / 1000.0)
    i0 = max(int(0.25 * N0), ta)
    i1 = min(int(0.90 * N0), N0 - tr)
    if i1 - i0 < int(0.2 * sr):
        i0, i1 = max(int(0.2 * N0), int(0.1 * sr)), N0 - int(0.05 * sr)
    f = estimate(x, sr, vibmod.hz_of(root), i0, i1)
    m = int(round((LENGTH_S - N0 / sr) * f))
    D = int(round(m * sr / f))
    f = m * sr / D                   # D is exactly m periods
    ks = [k for k in range(1, KMAX + 1) if k * f < min(F_MAX, 0.45 * sr)]
    c = project(x, sr, f, i0, i1, ks)
    mag = np.abs(c).max(axis=0)
    keep = mag >= mag.max() * 10 ** (REL_FLOOR_DB / 20.0)
    ks = [k for k, kp in zip(ks, keep) if kp]
    c = c[:, keep]
    h0 = harm_sum(c, ks, f, sr, N0)
    r0 = x - h0
    Nout = N0 + D
    rng = v7_rng.stream(name)
    # fresh noise from the sustain noise, measured on r0 over the span after the attack zone
    e0 = max(ta, int(0.15 * sr)) if N0 - tr - ta > int(0.3 * sr) else max(int(0.15 * sr), N0 - tr - int(0.3 * sr))
    e1 = N0 - tr
    nfft, hop = frame_size(f)
    rh = het_residual(x, sr, f)
    P, sig, tau = noise_model(rh, sr, e0, e1, nfft, hop, f)
    rs = synth_noise(rng, P, sig, tau, sr, Nout, nfft, hop)
    # noise: own attack, fresh sustain, own tail moved by D
    a0, a1 = ta, ta + xf
    t0 = N0 - tr + D            # start of the tail in the output
    b0, b1 = t0 - xf, t0
    n = np.arange(Nout)
    rn = rs.copy()
    rn[:a0] = r0[:a0]
    s = np.clip((n[a0:a1] - a0) / xf, 0, 1)[:, None]
    rn[a0:a1] = np.cos(np.pi / 2 * s) * r0[a0:a1] + np.sin(np.pi / 2 * s) * rs[a0:a1]
    s = np.clip((n[b0:b1] - b0) / xf, 0, 1)[:, None]
    rn[b0:b1] = np.cos(np.pi / 2 * s) * rs[b0:b1] + np.sin(np.pi / 2 * s) * r0[b0 - D:b1 - D]
    rn[t0:] = r0[t0 - D:]
    # harmonics with the drift, zero over the splice zones
    env = ramp_window(Nout, a1, b0, int(DRIFT_RAMP_S * sr))
    th = slow_noise(rng, Nout, sr) * env
    d = np.gradient(th) * sr
    cents = 1200.0 / np.log(2) * d / (f * 1.0)
    th *= DRIFT_PITCH_C / max(cents[a1 + int(DRIFT_RAMP_S * sr):b0 - int(DRIFT_RAMP_S * sr)].std() if b0 - a1 > 2 * DRIFT_RAMP_S * sr else cents.std(), 1e-12)
    # theta is in cycles of the fundamental: th was drawn with unit std; the scaling above gives DRIFT_PITCH_C cents std. th has the unit of cycles
    lv = slow_noise(rng, Nout, sr) * env * DRIFT_LEVEL_DB
    lk = np.array([slow_noise(rng, Nout, sr) * env * DRIFT_HARM_DB for _ in ks])
    h = harm_sum(c, ks, f, sr, Nout, theta=th, gain_db=(lv, lk))
    y = h + rn
    y[:ta] = x[:ta]            # the kept attack and the moved tail are the v6 samples themselves, not h + (x - h) rounded
    y[t0:] = x[t0 - D:]
    info = dict(rn=rn, r0=r0, h=h, f0=f, ks=len(ks), N0=N0, Nout=Nout, ta=ta, tr=tr, xf=xf, D=D, m=m, a1=a1, b0=b0, cents_std=float((1200 / np.log(2) * np.gradient(th) * sr / f)[a1:b0].std()))
    return y, info


def write_wav24_exact(path, x, sr):
    """24 bit PCM with the scale 2^23 that ffmpeg's decoder undoes (le.write_wav24 uses 2^23 - 1, which moves a full scale sample by one
    step), so that a kept stretch of a v6 sample comes back bit for bit."""
    v = np.clip(np.round(x * 2 ** 23), -(2 ** 23), 2 ** 23 - 1).astype(np.int64)
    n, ch = v.shape
    u = np.where(v < 0, v + (1 << 24), v).astype(np.uint32).reshape(-1)
    b = np.stack([u & 255, (u >> 8) & 255, (u >> 16) & 255], axis=1).astype(np.uint8).tobytes()
    import struct
    hdr = b"RIFF" + struct.pack("<I", 36 + len(b)) + b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, sr, sr * 3 * ch, 3 * ch, 24) + b"data" + struct.pack("<I", len(b))
    with open(path, "wb") as f:
        f.write(hdr + b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp"); ap.add_argument("out")
    ap.add_argument("--root", type=int, required=True)
    ap.add_argument("--keep-attack-ms", type=float, default=0.0)
    a = ap.parse_args()
    x, sr = le.decode(a.inp)
    y, info = extend(x, sr, a.root, os.path.basename(a.out), a.keep_attack_ms)
    pk = np.abs(y).max()
    if pk > 0.999:
        print("PEAK %.3f" % pk, file=sys.stderr)
    write_wav24_exact(a.out, y, sr)
    print("length %.3f -> %.3f s, f0 %.4f Hz, %d harmonics, drift %.3f c std, zones ta %.2f tr %.2f xf %.2f" % (info["N0"] / sr, info["Nout"] / sr, info["f0"], info["ks"], info["cents_std"], info["ta"] / sr, info["tr"] / sr, info["xf"] / sr))


if __name__ == "__main__":
    main()

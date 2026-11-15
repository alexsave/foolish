#!/usr/bin/env python3
"""vibmod.py - sub-cent pitch, level and per-harmonic modulation of a held note, and its 3 to 8 Hz modulation spectrum.

Method: harmonic k of a note with mean fundamental f0 is heterodyned with exp(-j 2 pi k f0 t), low-passed (Butterworth, zero phase,
40 Hz) and decimated to 200 Hz. The unwrapped phase gives the instantaneous frequency (resolution far below a cent, the limit is
noise), the magnitude the harmonic's level. Pitch in cents is the median over harmonics 1 to 6 that are not inside a magnitude null (under 0.3 of their own median;
a phase there is meaningless), a ratio, so the same cents whichever harmonic carries it. Level is the dB of the power sum of harmonics 1 to 8. Each series (pitch, level, harmonic
1 to 4 levels) is windowed (Hann) after removing mean and linear trend; the modulation spectrum is the amplitude of its FFT.
Peak = largest amplitude at 3 to 8 Hz; floor = median amplitude at 20 to 35 Hz; ratio = peak / floor; depth = 2 * peak (peak to peak).
A series whose peak/floor is under RATIO_AUDIBLE (3) has no peak above the floor.
Assumed perceptual thresholds (from memory of the literature, NOT verified here: Shower and Biddulph 1931; Zwicker and Fastl,
Psychoacoustics): frequency modulation at 4 to 5 Hz is heard from about 5 to 9 cents peak to peak, amplitude modulation from about
1 dB peak to peak. These are context only; the pass criterion used is no peak above the floor.
Deterministic, no randomness."""
import numpy as np
from scipy.signal import butter, sosfiltfilt

DEC = 200.0
RATIO_AUDIBLE = 3.0
FLOOR_MIN = 0.02   # cents or dB: below this a floor is numerical noise, so the ratio is taken against this instead
NFFT = 4096


def _het(x, sr, f, bw=40.0):
    t = np.arange(len(x)) / sr
    y = x * np.exp(-2j * np.pi * f * t)
    sos = butter(4, bw, fs=sr, output="sos")
    y = sosfiltfilt(sos, y.real) + 1j * sosfiltfilt(sos, y.imag)
    step = int(round(sr / DEC))
    return y[::step]


def hz_of(midi):
    return 440.0 * 2 ** ((midi - 69) / 12.0)


def refine_f0(x, sr, f_guess, span_cents=60.0):
    best, bf = -1.0, f_guess
    t = np.arange(len(x)) / sr
    for c in np.arange(-span_cents, span_cents + 0.01, 1.0):
        f = f_guess * 2 ** (c / 1200.0)
        e = sum(abs(np.sum(x * np.exp(-2j * np.pi * k * f * t))) ** 2 for k in (1, 2, 3))
        if e > best:
            best, bf = e, f
    # parabolic-free second pass at 0.1 cent
    for c in np.arange(-1.0, 1.01, 0.1):
        f = bf * 2 ** (c / 1200.0)
        e = sum(abs(np.sum(x * np.exp(-2j * np.pi * k * f * t))) ** 2 for k in (1, 2, 3))
        if e > best:
            best, bf = e, f
    return bf


def analyse(x, sr, f_guess, nh=8):
    """x mono float array of the sustain stretch. Returns dict of series at DEC Hz: pitch_c, level_db, harm_db[4], f0."""
    f0 = refine_f0(x, sr, f_guess)
    ys, cents, amps = [], [], []
    for k in range(1, nh + 1):
        y = _het(x, sr, k * f0)
        ph = np.unwrap(np.angle(y))
        dev = np.gradient(ph) * DEC / (2 * np.pi)
        cents.append(1200 * np.log2(np.maximum(1 + dev / (k * f0), 1e-6)))
        amps.append(np.abs(y))
    amps = np.array(amps)
    cents = np.array(cents)
    # a harmonic's phase is meaningless while its magnitude passes through a null (interference with a neighbouring partial), so a
    # harmonic under 0.3 of its own median magnitude does not vote; the pitch is the median of the voting harmonics 1 to 6
    med = np.median(amps, axis=1, keepdims=True)
    vote = amps >= 0.3 * med
    c6 = np.where(vote[:6], cents[:6], np.nan)
    pitch = np.nanmedian(np.where(np.isnan(c6).all(0), cents[:6], c6), axis=0)
    level = 10 * np.log10(np.maximum((amps ** 2).sum(0), 1e-20))
    harm = 20 * np.log10(np.maximum(amps[:4], 1e-10))
    return dict(f0=f0, pitch_c=pitch, level_db=level, harm_db=harm, harm_amp=amps)


def modspec(v, fs=DEC, trim=0.1):
    """Trim the filter edges, remove mean and trend, window. Returns (freqs, amplitude)."""
    n = len(v)
    a = int(trim * fs)
    v = np.asarray(v[a:n - a], float)
    t = np.arange(len(v))
    v = v - np.polyval(np.polyfit(t, v, 1), t)
    w = np.hanning(len(v))
    X = np.abs(np.fft.rfft(v * w, NFFT)) * 2.0 / w.sum()
    return np.fft.rfftfreq(NFFT, 1.0 / fs), X


def peak_floor(v, fs=DEC):
    """Returns (peak amplitude 3 to 8 Hz, its frequency, floor, ratio, depth peak to peak)."""
    fr, X = modspec(v, fs)
    s = (fr >= 3) & (fr <= 8)
    i = np.argmax(np.where(s, X, 0))
    fl = float(np.median(X[(fr >= 20) & (fr <= 35)]))
    return float(X[i]), float(fr[i]), fl, float(X[i] / max(fl, FLOOR_MIN)), float(2 * X[i])


def peak_band2(v, fs=DEC):
    """Peak amplitude at 8 to 16 Hz (the second harmonic of a vibrato rate: a level that follows |pitch| rectifies to twice the rate) and its ratio to the floor."""
    fr, X = modspec(v, fs)
    s = (fr > 8) & (fr <= 16)
    fl = float(np.median(X[(fr >= 20) & (fr <= 35)]))
    return float(X[s].max()), float(X[s].max() / max(fl, FLOOR_MIN))


def measure_note(x, sr, f_guess):
    """x mono sustain stretch. Returns dict with pitch, level and harmonic 1 to 4 (peak, freq, floor, ratio, depth) tuples plus the series."""
    a = analyse(x, sr, f_guess)
    out = dict(a)
    out["pitch"] = peak_floor(a["pitch_c"])
    out["level"] = peak_floor(a["level_db"])
    out["harm"] = [peak_floor(a["harm_db"][k]) for k in range(4)]
    out["level2"] = peak_band2(a["level_db"])
    out["harm2"] = [peak_band2(a["harm_db"][k]) for k in range(4)]
    return out

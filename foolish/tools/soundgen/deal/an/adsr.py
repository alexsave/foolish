"""ADSR of each slide from its 1-15 kHz envelope (5 ms RMS, 1 ms hop):
A = 10% -> 90% of peak amplitude; peak time; S = median level over the middle (from 90%-crossing to the last
time above -10 dB), in dB re peak, and its length; D = peak -> first return to within 1 dB of S;
R = from S's end to the room floor + 3 dB (or -35 dB if the floor is lower), and the shape of R (dB per 10 ms)."""
import sys, wave, glob, numpy as np
def load(p):
    with wave.open(p) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768, w.getframerate()
def bandpass(x, sr, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / sr); X[(f < lo) | (f > hi)] = 0; return np.fft.irfft(X, len(x))
room, _ = load("room.wav"); floor_db = 20 * np.log10(np.sqrt(np.mean(bandpass(room, 48000, 1000, 15000) ** 2)) + 1e-12)
def adsr(p, use_floor):
    x, sr = load(p); y = bandpass(x, sr, 1000, 15000); w, h = int(0.005 * sr), int(0.001 * sr)
    a = np.sqrt(np.convolve(y ** 2, np.ones(w) / w, "same"))[::h]; d = 20 * np.log10(a + 1e-12); pk = int(np.argmax(a)); P = a[pk]; Pd = d[pk]
    t10 = next((k for k in range(pk, -1, -1) if a[k] < 0.1 * P), 0); t90 = next(k for k in range(t10, pk + 1) if a[k] >= 0.9 * P)
    last10 = max(k for k in range(len(d)) if d[k] > Pd - 10)
    S = np.median(d[t90:last10 + 1]) - Pd
    dec = next((k - pk for k in range(pk, last10 + 1) if d[k] <= Pd + S + 1), 0)
    stop = max(floor_db + 3 if use_floor else -1e9, Pd - 35)
    rel_end = next((k for k in range(last10, len(d)) if d[k] < stop), len(d) - 1)
    rel = rel_end - last10; slope = (d[rel_end] - d[last10]) / max(1, rel) * 10
    return t90 - t10, pk - t10, dec, S, last10 - t90, rel, slope, stop - Pd
cols = ("attack 10-90%", "peak at", "decay", "sustain dB", "sustain len", "release", "rel dB/10ms", "rel to dB")
print(f"{'':18s}" + "".join(f"{c:>14s}" for c in cols))
for g, pat, uf in (("real", "parts/real_thumb_*.wav", True), ("synth", "synth/thumb7_0[1-8].wav", True)):
    R = []
    for p in sorted(glob.glob(pat)):
        r = adsr(p, uf); R.append(r); print(f"{p.split('/')[-1]:18s}" + "".join(f"{v:14.1f}" for v in r))
    R = np.array(R); print(f"{g+' median':18s}" + "".join(f"{v:14.1f}" for v in np.median(R, 0)) + "\n")

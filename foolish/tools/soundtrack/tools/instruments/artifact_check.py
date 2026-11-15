#!/usr/bin/env python3
"""artifact_check.py SCALES.wav [...] : artifact measures of a C major scale render (lyria_instruments_demo/score_scale.txt, tempo 60,
three sections of 20 s: cello, haegeum, trumpet; 15 notes of 1.0 s every 1.3 s, vibrato off). Per instrument, over the notes:
  ac     the largest autocorrelation of the note's level envelope (5 ms RMS in dB, 0.10 to 0.95 s of the note, 0.3 s moving average
         removed) at lags 40 to 300 ms, times the envelope's std in dB: a repeated envelope pattern (an echo or a loop period) as dB;
  dip    the deepest dip of harmonic 1 to 4 below its own median level (dB) inside the note: a loop join or a null shows as a big dip;
  pre    the level (dBFS RMS) in the 50 ms before the note starts (the end of the gap): ghost energy before an onset;
  steps  the largest 10 ms to 10 ms level step (dB) inside the note.
Prints the median over notes and the worst note. Deterministic. Module use: check(wav) -> {inst: dict}."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod
import vib_demo_measure as dm

SCALE = {"cello": ["C3", "D3", "E3", "F3", "G3", "A3", "B3", "C4"], "haegeum": ["C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5"],
         "trumpet": ["C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5"]}


def env_db(x, sr, hop=0.005):
    w = int(hop * sr)
    n = len(x) // w
    r = np.sqrt((x[:n * w].reshape(n, w) ** 2).mean(1))
    return 20 * np.log10(np.maximum(r, 1e-9))


def check(wav):
    x, sr = le.decode(wav)
    m = x.mean(axis=1)
    out = {}
    for j, (inst, up) in enumerate(SCALE.items()):
        seq = up + up[-2::-1]
        rows = []
        for i, nm in enumerate(seq):
            t0 = j * 20 + i * 1.3
            seg = m[int((t0 + 0.10) * sr):int((t0 + 0.95) * sr)]
            e = env_db(seg, sr)
            k = 60   # 0.3 s moving average removed: only repeats faster than that count
            e = e - np.convolve(np.pad(e, (k // 2, k // 2), mode="edge"), np.ones(k) / k, mode="valid")[:len(e)]
            e = e[k // 2:-k // 2]
            ac = np.correlate(e, e, "full")[len(e) - 1:] / max(np.dot(e, e), 1e-12)
            ac_max = float(ac[8:61].max() * e.std())     # lags 40 to 300 ms: autocorrelation peak times the ripple's std in dB
            r = vibmod.measure_note(seg, sr, dm.hz(dm.midi(nm)))
            dip = float(max(-(h - np.median(h)).min() for h in r["harm_db"][:4]) if False else
                        max(np.median(h[10:-10]) - h[10:-10].min() for h in r["harm_db"][:4]))
            pre = float(20 * np.log10(max(np.sqrt(np.mean(m[int((t0 - 0.05) * sr):int(t0 * sr)] ** 2)), 1e-9))) if t0 > 0.05 else -99.0
            e10 = env_db(m[int((t0 + 0.10) * sr):int((t0 + 0.95) * sr)], sr, 0.010)
            steps = float(np.abs(np.diff(e10)).max())
            rows.append((ac_max, dip, pre, steps))
        out[inst] = np.array(rows)
    return out


def main():
    for wav in sys.argv[1:]:
        print(wav)
        for inst, r in check(wav).items():
            print("  %-8s repeat-ripple dB median %.2f worst %.2f | harmonic dip median %.1f worst %.1f dB | pre-onset level median %.1f worst %.1f dBFS | step median %.1f worst %.1f dB" % (
                inst, np.median(r[:, 0]), r[:, 0].max(), np.median(r[:, 1]), r[:, 1].max(), np.median(r[:, 2]), r[:, 2].max(), np.median(r[:, 3]), r[:, 3].max()))


if __name__ == "__main__":
    main()

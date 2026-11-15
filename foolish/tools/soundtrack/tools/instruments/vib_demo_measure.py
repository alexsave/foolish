#!/usr/bin/env python3
"""vib_demo_measure.py WAV SECTIONS.txt [--json-free] : measures every held note of the on-then-off demo (score_vib.txt: per
instrument a section 'vibrato on' then 'vibrato off', five held notes of 4 beats each at 90 BPM, the first four in bars 1 to 4 and
the fifth in bar 5) with vibmod. The stretch measured is 0.35 s after the note start to 0.30 s before its end. Prints, per
instrument and half: the median over notes of pitch depth (cents peak to peak), level depth (dB pp), harmonic 1 to 4 depth, and
the 3 to 8 Hz peak/floor ratio of each; also the number of notes whose pitch or level or harmonic ratio is >= RATIO_AUDIBLE.
Module use: notes(wav, sections) yields (inst, half, name, series dict)."""
import os, re, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "samples"))
import lyria_notes_extract as le
import vibmod

BPM = 90.0
GATE = 0.92
LONGS = {"cello": ["D3", "F3", "G3", "A3", "G3"], "haegeum": ["C4", "E4", "G4", "C5", "C5"], "trumpet": ["D4", "F4", "A#4", "D5", "D5"]}
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(s):
    m = re.match(r"([A-G])(#?)(\d)", s)
    return NAMES[m.group(1)] + (1 if m.group(2) else 0) + 12 * (int(m.group(3)) + 1)


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12.0)


def notes(wav, sections):
    x, sr = le.decode(wav)
    mono = x.mean(axis=1)
    for line in open(sections):
        p = line.split()
        if len(p) < 5:
            continue
        t0, inst, half = float(p[0]), p[3], p[5]
        beat = 0.0
        for i, nm in enumerate(LONGS[inst]):
            b = i * 4.0
            a = t0 + b * 60 / BPM + 0.35
            e = t0 + (b + 4.0) * 60 / BPM * GATE - 0.30 - (0 if True else 0)
            seg = mono[int(a * sr):int(e * sr)]
            yield inst, half, nm, vibmod.measure_note(seg, sr, hz(midi(nm))), (a, e)


def summary(wav, sections):
    res = {}
    for inst, half, nm, m, _ in notes(wav, sections):
        res.setdefault((inst, half), []).append(m)
    out = {}
    for k, ms in res.items():
        d = lambda f: float(np.median([f(m) for m in ms]))
        out[k] = dict(
            pitch_pp=d(lambda m: m["pitch"][4]), pitch_ratio=d(lambda m: m["pitch"][3]),
            level_pp=d(lambda m: m["level"][4]), level_ratio=d(lambda m: m["level"][3]),
            harm_pp=d(lambda m: max(h[4] for h in m["harm"])), harm_ratio=d(lambda m: max(h[3] for h in m["harm"])),
            pitch_max=max(m["pitch"][4] for m in ms), level_max=max(m["level"][4] for m in ms),
            bad=sum(1 for m in ms if max(m["pitch"][3], m["level"][3], max(h[3] for h in m["harm"])) >= vibmod.RATIO_AUDIBLE),
            n=len(ms), peak_hz=d(lambda m: m["pitch"][1]))
    return out


def main():
    s = summary(sys.argv[1], sys.argv[2])
    print("%-8s %-4s | pitch pp c (ratio) | level pp dB (ratio) | harm1-4 pp dB (ratio) | notes with a peak >= %.0f x floor" % ("inst", "half", vibmod.RATIO_AUDIBLE))
    for (inst, half), v in s.items():
        print("%-8s %-4s | %6.2f (%6.1f) | %5.2f (%6.1f) | %5.2f (%6.1f) | %d of %d" % (
            inst, half, v["pitch_pp"], v["pitch_ratio"], v["level_pp"], v["level_ratio"], v["harm_pp"], v["harm_ratio"], v["bad"], v["n"]))


if __name__ == "__main__":
    main()

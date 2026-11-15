#!/usr/bin/env python3
"""measure.py - measures the Lyria instruments demo render: for every played note the f0 against the intended equal-tempered
pitch (YIN median over 20 to 80 percent of the sounding length), and clicks (a sample-to-sample jump more than 12 dB above
the local 5 ms RMS, where that RMS is above -60 dBFS), attributed to the note whose sounding span (or the 150 ms after its end)
holds them, with the time since the note's start.
Usage: python3 -I measure.py out/full.wav score.txt out/sections.txt TABLE.md
Notes are read back from score.txt (tempo 90, bars of 4 beats, token pitch:beats, default gate 0.92) and the section start
times from sections.txt. Deterministic."""
import sys, re
import numpy as np
sys.path.insert(0, "/Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/tools/samples")
import lyria_notes_extract as le

BPM, GATE = 90.0, 0.92
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi_of(s):
    m = re.match(r"([A-G])(#?)(\d)", s)
    return NAMES[m.group(1)] + (1 if m.group(2) else 0) + 12 * (int(m.group(3)) + 1)


def main():
    wav, score, secs, out = sys.argv[1:5]
    x, sr = le.decode(wav)
    L, R = x[:, 0], x[:, 1]
    mono = x.mean(axis=1)
    starts = [float(l.split()[0]) for l in open(secs)]
    notes, sec = [], -1
    for line in open(score):
        if line.startswith("SECTION"):
            sec += 1
            name = line.split()[1]
        m = re.match(r"([A-Z]+): (.*)", line)
        if m and m.group(1) != "DYN":
            beat = 0.0
            for bar in m.group(2).split("|"):
                for tok in bar.split():
                    p, ln = tok.split(":")
                    ln = float(ln)
                    if p != "r":
                        notes.append(dict(inst=name, t=starts[sec] + beat * 60.0 / BPM, len=ln * 60.0 / BPM * GATE, midi=midi_of(p), name=p,
                                          beats=ln))
                    beat += ln
    # clicks
    w = int(0.005 * sr)
    clicks = []
    jumps = np.full(len(L), -99.0)
    for ch in (L, R):
        e = np.convolve(ch ** 2, np.ones(w) / w, mode="same")
        rms = np.sqrt(e)
        d = np.abs(np.diff(ch, prepend=ch[0]))
        with np.errstate(divide="ignore", invalid="ignore"):
            jump = 20 * np.log10(np.maximum(d, 1e-12) / np.maximum(rms, 1e-12))
        jumps = np.maximum(jumps, np.where(rms > 1e-3, jump, -99.0))
        bad = np.nonzero((jump > 12.0) & (rms > 1e-3))[0]
        clicks += [i / sr for i in bad]
    clicks = sorted(set(round(c, 5) for c in clicks))
    rows = []
    for n in notes:
        a, b = n["t"] + 0.2 * n["len"], n["t"] + 0.8 * n["len"]
        seg = mono[int(a * sr):int(b * sr)]
        f0, _ = le.yin_f0(seg, sr)
        v = f0[f0 > 0]
        if len(v) < 3:
            n["cents"], n["f0"] = None, 0.0
        else:
            f = float(np.median(v))
            n["f0"] = f
            n["cents"] = 1200 * np.log2(f / (440.0 * 2 ** ((n["midi"] - 69) / 12.0)))
        inside = [c for c in clicks if n["t"] <= c <= n["t"] + n["len"] + 0.15]
        n["clicks"] = [round(c - n["t"], 3) for c in inside]
        n["maxjump"] = float(jumps[int(n["t"] * sr):int((n["t"] + n["len"] + 0.15) * sr)].max())
        rows.append(n)
    L_ = ["# Lyria instruments demo, measured", "",
          "f0: YIN median over 20 to 80 percent of the sounding length, against equal temperament (A4 = 440 Hz).",
          "Click: a sample-to-sample jump more than 12 dB above the local 5 ms RMS (RMS above -60 dBFS), either channel; listed as seconds since the note's start.",
          "", "| instrument | note | beats | f0 Hz | cents error | largest jump dB re local RMS | clicks (s after note start) |", "|---|---|---|---|---|---|---|"]
    for n in rows:
        L_.append("| %s | %s | %g | %s | %s | %.1f | %s |" % (n["inst"], n["name"], n["beats"], "%.1f" % n["f0"] if n["cents"] is not None else "none",
                                                      "%+.1f" % n["cents"] if n["cents"] is not None else "-", n["maxjump"], " ".join(map(str, n["clicks"])) or "none"))
    L_.append("")
    for inst in sorted({n["inst"] for n in rows}):
        r = [n for n in rows if n["inst"] == inst]
        ce = [abs(n["cents"]) for n in r if n["cents"] is not None]
        L_.append("%s: %d notes, %d with an f0, mean abs error %.1f cents, max %.1f cents, %d notes over 10 cents, %d notes with a click, %d clicks in all, largest jump %.1f dB re local RMS." % (
            inst, len(r), len(ce), np.mean(ce), max(ce), sum(1 for c in ce if c > 10), sum(1 for n in r if n["clicks"]), sum(len(n["clicks"]) for n in r), max(n["maxjump"] for n in r)))
        L_.append("")
    open(out, "w").write("\n".join(L_) + "\n")
    print("\n".join(l for l in L_ if "notes," in l))


main()

#!/usr/bin/env python3
"""make_score.py - writes score.txt for the Lyria instruments demo from the sampler manifests.
Usage: python3 -I make_score.py MANIFEST_DIR > score.txt
Per instrument: every sampled root as a 2-beat note, a slow one-octave scale in semitone steps from the lowest root
(1 beat per note), then three 4-beat notes on the lowest, middle and highest root. Deterministic, no randomness."""
import sys
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
INST = [("cello", "CELLO"), ("haegeum", "HAEGEUM"), ("trumpet", "TRUMPET")]


def nm(m):
    return "%s%d" % (NAMES[m % 12], m // 12 - 1)


def bars(tokens, per_bar=4.0):
    """tokens: list of (text, beats). Returns the bar strings, rests padding the last bar."""
    out, cur, used = [], [], 0.0
    for text, b in tokens:
        cur.append("%s:%g" % (text, b))
        used += b
        if used >= per_bar - 1e-9:
            out.append(" ".join(cur))
            cur, used = [], 0.0
    if cur:
        cur.append("r:%g" % (per_bar - used))
        out.append(" ".join(cur))
    return out


def main():
    man = sys.argv[1]
    print("TEMPO 90\n")
    for inst, part in INST:
        roots = sorted({int(l.split()[0]) for l in open("%s/lyria_%s.txt" % (man, inst)) if l.strip()})
        toks = [(nm(r), 2) for r in roots]
        toks += [("r", 0)] * 0
        a = bars(toks)
        scale = bars([(nm(roots[0] + i), 1) for i in range(13)])
        mid = roots[len(roots) // 2]
        three = bars([(nm(roots[0]), 4), (nm(mid), 4), (nm(roots[-1]), 4)])
        allb = a + scale + three
        print("SECTION %s roots, scale, long notes BARS %d KEY C" % (inst, len(allb)))
        print("%s: %s" % (part, " | ".join(allb)))
        print("DYN: mf\n")


main()

#!/usr/bin/env python3
"""release_measure.py - T20 and T40 of note ends.
Usage: release_measure.py sources LABEL=FILE[,LABEL=FILE...]   (natural note ends in clips: a note followed by at least 300 ms
                                                               40 dB under the clip peak)
       release_measure.py render WAV SCORE SECTIONS OUT.md      (the separated notes of the demo render)
Definitions: the 10 ms RMS in dB; S, the note's sustain level, is the median level over 25 to 60 percent of the note
(sources) or from 30 percent of the note to 50 ms before note-off (render). The decay starts at j, the last frame within 1 dB
of S before the final fall (the first frame after which every frame stays more than 3 dB under S up to the note's end).
T20 and T40 are the times from j until the level first reaches S-20 and S-40 dB. Slope 1 is -20 / T20 and slope 2 is
-20 / (T40 - T20) in dB per second. The decay is two-stage when slope 2 is under half of slope 1."""
import sys, re
import numpy as np
sys.path.insert(0, "/Users/alex/Dev/foolish-wt-city-songs-v5/foolish/tools/soundtrack/tools/samples")
import lyria_notes_extract as le
H = 0.01


def decay(db, S, mid, end_idx):
    """db level array, S sustain level, mid the first frame to search from, end_idx the last frame of the note. Returns dict or None."""
    k3 = None
    for i in range(mid, end_idx + 1):
        if db[i:end_idx + 1].max() < S - 3.0:
            k3 = i
            break
    if k3 is None:
        return None
    j = k3 - 1
    while j > mid and db[j] < S - 1.0:
        j -= 1
    def first(th):
        for k in range(j + 1, len(db)):
            if db[k] <= th:
                return (k - j) * H
        return None
    t20, t40 = first(S - 20.0), first(S - 40.0)
    out = dict(S=S, t20=t20, t40=t40, j=j * H)
    if t20:
        out["s1"] = -20.0 / t20
        if t40 and t40 > t20:
            out["s2"] = -20.0 / (t40 - t20)
    return out


def source_ends(path):
    x, sr = le.decode(path)
    m = x.mean(axis=1)
    db, _ = le.level_db(m, sr)
    peak = db.max()
    f0, cen = le.yin_f0(m, sr)
    low = db < peak - 40.0
    res = []
    i = 0
    while i < len(low):
        if low[i]:
            j = i
            while j < len(low) and low[j]:
                j += 1
            if j - i >= 30 and i > 20:
                # the note: back to the previous low frame (or 4 s)
                a = i - 1
                while a > 0 and not low[a] and (i - a) * H < 4.0:
                    a -= 1
                L = i - a
                if L * H >= 0.3:
                    a0, a1 = a + int(0.25 * L), a + int(0.60 * L)
                    S = float(np.median(db[a0:a1]))
                    vo = np.mean(f0[(cen >= (i - 20) * H * sr) & (cen < i * H * sr)] > 0) if len(f0) else 0
                    d = decay(db, S, a + int(0.5 * L), i)
                    if d and vo >= 0.5:
                        d["at"] = i * H
                        res.append(d)
            i = j
        else:
            i += 1
    return res


def stats(v):
    v = [x for x in v if x is not None]
    if not v:
        return "none"
    q = np.percentile(v, [25, 50, 75])
    return "%.3f / %.3f / %.3f" % tuple(q)


def main():
    if sys.argv[1] == "sources":
        for grp in sys.argv[2:]:
            name, files = grp.split("=", 1)
            allr = []
            for f in files.split(","):
                r = source_ends(f)
                for d in r:
                    d["src"] = f.split("/")[-1]
                allr += r
            print("== %s: %d natural note ends" % (name, len(allr)))
            for d in allr:
                print("   %s at %.2f S %.1f dB T20 %s T40 %s slope1 %s slope2 %s" % (d["src"][:28], d["at"], d["S"], d["t20"], d["t40"],
                      "%.0f" % d["s1"] if "s1" in d else "-", "%.0f" % d["s2"] if "s2" in d else "-"))
            print("   T20 q1/median/q3: %s s;  T40: %s s;  slope1: %s;  slope2: %s" % (stats([d["t20"] for d in allr]), stats([d["t40"] for d in allr]),
                  stats([d.get("s1") for d in allr]), stats([d.get("s2") for d in allr])))
    else:
        wav, score, secs, out = sys.argv[2:6]
        x, sr = le.decode(wav)
        m = x.mean(axis=1)
        db, _ = le.level_db(m, sr)
        starts = [float(l.split()[0]) for l in open(secs)]
        sec = -1
        notes = []
        for line in open(score):
            if line.startswith("SECTION"):
                sec += 1
                name = line.split()[1]
            mm = re.match(r"([A-Z]+): (.*)", line)
            if mm and mm.group(1) != "DYN":
                beat = 0.0
                for bar in mm.group(2).split("|"):
                    for tok in bar.split():
                        p, ln = tok.split(":")
                        ln = float(ln)
                        if p != "r":
                            notes.append(dict(inst=name, p=p, beats=ln, t=starts[sec] + beat * 60 / 90, off=starts[sec] + (beat + ln * 0.92) * 60 / 90))
                        beat += ln
        rows = []
        for k, n in enumerate(notes):
            nxt = notes[k + 1]["t"] if k + 1 < len(notes) and notes[k + 1]["inst"] == n["inst"] else None
            if nxt is not None and nxt - n["off"] < 2.0:
                continue   # joined to the next note: no measurable end
            L = n["off"] - n["t"]
            S = float(np.median(db[int((n["t"] + 0.3 * L) / H):int((n["off"] - 0.05) / H)]))
            end = int(min(n["off"] + 3.0, nxt - 0.05 if nxt else 1e9, len(db) * H - 0.01) / H)
            d = decay(db, S, int((n["t"] + 0.5 * L) / H), end)
            n["d"] = d
            rows.append(n)
        L_ = ["| instrument | note | beats | S dB | T20 s | T40 s | slope1 dB/s | slope2 dB/s |", "|---|---|---|---|---|---|---|---|"]
        for n in rows:
            d = n["d"] or {}
            L_.append("| %s | %s | %g | %s | %s | %s | %s | %s |" % (n["inst"], n["p"], n["beats"], "%.1f" % d["S"] if d else "-", d.get("t20"), d.get("t40"),
                      "%.0f" % d["s1"] if "s1" in d else "-", "%.0f" % d["s2"] if "s2" in d else "-"))
        L_.append("")
        for inst in sorted({n["inst"] for n in rows}):
            r = [n["d"] for n in rows if n["inst"] == inst and n["d"]]
            L_.append("%s: %d note ends, T20 q1/median/q3 %s s, T40 %s s, slope1 %s, slope2 %s" % (inst, len(r), stats([d["t20"] for d in r]), stats([d["t40"] for d in r]),
                      stats([d.get("s1") for d in r]), stats([d.get("s2") for d in r])))
        open(out, "w").write("\n".join(L_) + "\n")
        print("\n".join(l for l in L_ if "note ends" in l))


main()

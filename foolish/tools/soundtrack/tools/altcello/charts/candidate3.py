"""Candidate 3 for the lead: the candidate 2 file set rendered with the THERMAL bow model (the new default of the instrument): single notes (G2 D3 A3),
low notes (C2, G2 against Philharmonia D2 and G2), the C major scale, the held low line; dry and room_0p6 (no room_1p0). The player seed of each single
note is the best of three takes by the audio time to periodic (out/thermal/gates_NAME.txt of charts/thermal_gates.py). Loudness -20 LUFS.

    python -I charts/candidate3.py GATES_NAME [OUTDIR]
"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import candidate2 as C  # noqa: E402
import onset_lib as L  # noqa: E402

OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate3"


def best_seeds(name):
    rows = {}
    for l in open(os.path.join(L.ROOT, "out", "thermal", "gates_%s.txt" % name)):
        q = l.split()
        if q and q[0] == "ROW" and q[1] == name:
            rows.setdefault(q[2], {})[int(q[3])] = float(q[4])
    seeds = {}
    for lab, d in rows.items():
        seeds[lab] = min(d, key=lambda s: (d[s] if d[s] >= 0 else 99.0, s))
    return seeds


def main():
    name = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else OUT
    C.OUT = out
    C.TMP = os.path.join(L.TMP, "cand3")
    C.ROOMS = {k: v for k, v in C.ROOMS.items() if k != "room_1p0"}
    os.makedirs(out, exist_ok=True)
    os.makedirs(C.TMP, exist_ok=True)
    sd = best_seeds(name)
    print("seeds", sd)
    res = {}
    C.singles("notes", "single_notes.txt", [(43, 0.5), (50, 3.5), (57, 6.5)], [43, 50, 57], res, [sd["43"], sd["50"], sd["57"]])
    C.singles("low_notes", "low_notes.txt", [(36, 0.5), (43, 3.5)], [38, 43], res, [sd["36"], sd["43c"]])
    C.whole("scale", "c_major_scale.txt", res, C.ROOMS)
    C.whole("held_line", "held_line_low.txt", res, ["dry", "room_0p6"])
    for k, v in sorted(res.items()):
        print("%-34s integrated %.2f LUFS, peak %.3f" % (k, v[0], v[1]))
    return sd, res


if __name__ == "__main__":
    main()

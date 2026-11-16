"""Candidate 5 (richness iteration 1): the candidate 4 file set with the new defaults (bow noise corner 16 kHz and level 0.02, body eta exponent 0.15 and mass exponent 0.3,
soft 15 ms force landing). The low notes are D2 (C2 + 2 semitones on the C string) and G2 on C, so they pair exactly with Philharmonia ph_cello_38 and _43. Player seeds: the best of
three by the audio time to periodic (out/rich/res_comb.pkl). -20 LUFS. Writes KEY.txt.      python -I charts/candidate5.py [OUTDIR]"""
import os, sys, pickle
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import candidate2 as C  # noqa: E402
import onset_lib as L  # noqa: E402
import rich_lib as R  # noqa: E402
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate5"
KEY = """ALT-CELLO candidate 5 (for the lead to check, not for Alex). All files 48 kHz mono float32, integrated loudness matched to -20 LUFS (ffmpeg ebur128).
Made by charts/candidate5.py in the alt-cello worktree. Same file sets as candidate 4, except the low notes: D2 in place of C2.

What changed since candidate 4 (richness iteration 1, ITERATION_LOG.md)
  - bow noise (the fluctuation of the sliding friction): low-pass corner 16 kHz (was 3 kHz), 64000 draws per second of logical time (was 4000), level 0.02 as a density
  - body: the loss factor of the statistical modes eta = 0.012 (f/500)^0.15 (exponent was 0.3), their mass law m ~ f^0.3 (was f^0.5)
  - the expert's bow force lands over 15 ms (was 5 ms): the 5 ms step rang the string 24-27 dB under the sustain before the bow moved (the blob at the open pitch before a stopped note)
  - single notes (notes_*, low_notes_*): every note is its own simulation (fresh instrument), as the Philharmonia notes are separate recordings; before, the G2 note's sympathetic ring of the open G string was still sounding 1.3 s later under the D3 note
  - vibrato: default depth 8 c (was 11; real notes ph_cello_43/50/57 median 7.4 c, ours measured 10.3) and rate 4.9-5.3 Hz (was 5.2-5.6; real median 5.4, ours measured 5.9)
  - room_0p6: measured, not changed (4-10 kHz re 0.1-4 kHz in the first 0.3 s of each note, room minus dry: +1.1, -2.4, +3.1, +2.2, +1.1 dB)
  - NOT changed: the string loss (the relaxation loss with a flat Q is implemented, tested and left OFF: it closed nothing in the bowed spectrum), the damping touch before a shift (no gain)
Files: notes_*, low_notes_* (D2 on C, G2 on C; reference ph_cello_38, ph_cello_43), scale_*, held_line_* ; dry and room_0p6 as in candidate 4. Player seeds: notes %s, low notes %s.
Measured against Philharmonia (dry, 3 seeds, sustained part): 5-10 kHz LTAS gap -19.9 -> -11.0 dB, 1.6-5 kHz -3.3 -> +0.4, 0.5-1.6 kHz +4.7 -> +4.3, 0.2-0.5 kHz -2.8 -> -2.9.
Known faults: the 5-10 kHz level is still 11 dB under the real notes; HNR of G2 on C (12.7 dB, Philharmonia 25.4) and D3 (21.6 against 25.6) is lower than candidate 4's; the room is as in candidate 4 (not yet checked for darkness);
the vibrato, the onset shape and the release blob at the end of G2 are untouched (lead addendum F6-F8, not done).
"""


def singles_sep(name, notes, refs, res, seeds):
    """every note is its own simulation (fresh instrument: no sympathetic ring of the previous note, like the separate Philharmonia recordings), placed at the same times as the one render
    of candidate 4. notes: [(midi, string letter, start s)]"""
    total = int((notes[-1][2] + C.PRE + C.POST + 0.3) * C.SR)
    outs = {v: np.zeros(total) for v in C.ROOMS}
    outs["ref"] = np.zeros(total)
    for (midi, st, t), rm, sd in zip(notes, refs, seeds):
        sc = os.path.join(C.TMP, "%s_%d.txt" % (name, midi))
        open(sc, "w").write("# one note\n0.5000 1.5 %d 0.85 %s 1 - - 0\n" % (midi, st))
        ys = {v: C.render(sc, "%s_%d_s%d" % (name, midi, sd), v, extra=["--seed", str(sd)], trace=None) for v in C.ROOMS}
        on = C.onset(ys["dry"])
        a0 = int(round((t - C.PRE) * C.SR))
        for v in C.ROOMS:
            w = C.window(ys[v], on)
            outs[v][a0:a0 + len(w)] += w
        ref = C.M.load_audio(C.G.PHIL_FULL % rm)
        w = C.window(ref, C.onset(ref))
        outs["ref"][a0:a0 + len(w)] += w
    for v, y in outs.items():
        fn = "%s_%s.wav" % (name, "ref" if v == "ref" else "alt_" + v)
        res[fn] = C.match(y, os.path.join(C.OUT, fn))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    C.OUT = out
    C.TMP = os.path.join(L.TMP, "cand5")
    C.ROOMS = {k: v for k, v in C.ROOMS.items() if k != "room_1p0"}
    os.makedirs(out, exist_ok=True)
    os.makedirs(C.TMP, exist_ok=True)
    res = pickle.load(open(os.path.join(R.OUT, "res_comb.pkl"), "rb"))
    best = {k: int(np.argmin([m["tper"] if m["tper"] >= 0 else 99 for m in v["ours"]])) for k, v in res.items()}
    print("seeds", best)
    r = {}
    singles_sep("notes", [(43, "C", 0.5), (50, "G", 3.5), (57, "D", 6.5)], [43, 50, 57], r, [best["G2c"], best["D3"], best["A3"]])
    singles_sep("low_notes", [(38, "C", 0.5), (43, "C", 3.5)], [38, 43], r, [best["D2"], best["G2c"]])
    C.whole("scale", "c_major_scale.txt", r, C.ROOMS)
    C.whole("held_line", "held_line_low.txt", r, ["dry", "room_0p6"])
    open(os.path.join(out, "KEY.txt"), "w").write(KEY % (" ".join(str(best[k]) for k in ("G2c", "D3", "A3")), " ".join(str(best[k]) for k in ("D2", "G2c"))))
    for k, v in sorted(r.items()):
        print("%-34s integrated %.2f LUFS, peak %.3f" % (k, v[0], v[1]))


main()

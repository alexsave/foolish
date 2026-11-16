"""Candidate 6 (roughness iteration 1): the candidate 5 file set with the distributed bow contact (4 points across the hair). The low notes are D2 (C2 + 2 semitones on the C string) and G2 on C, so they pair exactly with Philharmonia ph_cello_38 and _43. Player seeds: the best of
three by the audio time to periodic (out/rough/res_c_n4.pkl). -20 LUFS. Writes KEY.txt.      python -I charts/candidate6.py [OUTDIR]"""
import os, sys, pickle
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import candidate2 as C  # noqa: E402
import onset_lib as L  # noqa: E402
import rich_lib as R  # noqa: E402
import rough_lib as RL  # noqa: E402
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/candidate6"
KEY = """ALT-CELLO candidate 6 (for the lead to check, not for Alex). All files 48 kHz mono float32, integrated loudness matched to -20 LUFS (ffmpeg ebur128).
Made by charts/candidate6.py in the alt-cello worktree. The same file sets as candidate 5 (D2 and G2 on the C string as the low notes).

What changed since candidate 5 (roughness iteration 1, ITERATION_LOG.md)
  - the bow contact is distributed across the hair width: 4 contact points, each with its own bristle state and temperature (van Walstijn et al. 2026 use N = 4), in place of one lumped contact.
    On the low strings the lumped contact let the string switch between one slip per period and two (D2 and G2 on C: 55 and 65 percent of the periods had an extra slip, D3 and A3 none),
    which is the "rough" texture; with 4 points 13 and 11 percent of the periods have one (D3 and A3 still none).
  - the analysis counts a slip only when it carries 5 percent of the displacement of the window's largest slip (the 4 points release one after the other, which leaves 0.1-0.5 percent precursors on the mean velocity)
  - NOT changed: the body, the room, the string loss, the player, the vibrato, the bow noise level (all one-factor tests in ITERATION_LOG.md: bow noise on the C string alone, hair width, torsion damping and impedance, bow force / position / speed, vibrato depth)
Files: notes_*, low_notes_* (D2 on C, G2 on C; reference ph_cello_38, ph_cello_43), scale_*, held_line_* ; dry and room_0p6 as in candidate 5. Player seeds: notes %s, low notes %s (the best of three by the audio time to periodic).
Known faults: G2 on C has HNR 18.8 dB against 25.4 for the Philharmonia G2 (an open G string of ours gets 24.2: the real note may be the open string); the 5-10 kHz level is still 11 dB under the real notes; the top edge near C8;
the room is as in candidate 5.
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
    C.TMP = os.path.join(L.TMP, "cand6")
    C.ROOMS = {k: v for k, v in C.ROOMS.items() if k != "room_1p0"}
    os.makedirs(out, exist_ok=True)
    os.makedirs(C.TMP, exist_ok=True)
    res = pickle.load(open(os.path.join(RL.OUT, "res_c_n4.pkl"), "rb"))  # the 3 seeds of the shipped defaults (rough_run.py c_n4)
    best = {k: int(np.argmin([m["tper"] if m["tper"] >= 0 else 99 for m in v])) for k, v in res.items()}
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

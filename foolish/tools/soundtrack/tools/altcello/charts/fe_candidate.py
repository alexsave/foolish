"""Listening set of the FE body test (for the lead to check, not for Alex): the candidate 5 single notes (one simulation per note, dry, placed as in candidate 5) with the statistical body,
FE-A and FE-B, -20 LUFS, equal lengths. low_notes: D2 (C2 + 2 on C), G2 on C; notes: G2 on C, D3 on G, A3 on D; open_c2: the open C string. Writes KEY.txt.
    /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/fe_candidate.py [OUTDIR]"""
import os, sys, pickle
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import candidate2 as C  # noqa: E402
import onset_lib as L  # noqa: E402
import rich_lib as R  # noqa: E402
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/fe_body"
BODIES = [("stat", ""), ("fea", "fe=1"), ("feb", "fe=2")]
KEY = """ALT-CELLO solved FE body, one-factor test (for the lead to check, not for Alex). All files 48 kHz mono float32, dry, integrated loudness matched to -20 LUFS (ffmpeg ebur128),
equal lengths within a set. Made by charts/fe_candidate.py in the alt-cello worktree. Each note is its own simulation, placed as in candidate 5; the player seeds are those of candidate 5
(notes %s, low notes %s), the same for the three bodies. Everything except the body is the candidate 5 default.

Files (low_notes: D2 = C2 + 2 semitones on the C string, then G2 on C; notes: G2 on C, D3 on G, A3 on D)
  low_notes_stat.wav, notes_stat.wav    the statistical body (the shipped default)
  low_notes_fea.wav,  notes_fea.wav     FE-A: the 380 FE modes below 2.5 kHz radiated as the monopole of r_net (p = rho V'' / 4 pi d); statistical modes above 2.5 kHz as in the default;
                                        level matched to the default at 2.0-2.5 kHz by one scale (white bridge force)
  low_notes_feb.wav,  notes_feb.wav     FE-B: the same FE modes through our radiation filters (A0-like and < 250 Hz modes through the ka filter, the others through the plate efficiency
                                        filter with the sign of r_net); the same crossover match
  low_notes_ref.wav,  notes_ref.wav     the Philharmonia notes ph_cello_38/43 and ph_cello_43/50/57
  open_c2_stat.wav, open_c2_fea.wav, open_c2_feb.wav   the open C string (C2), 1.5 s, no Philharmonia counterpart in the set
Measured (charts/fe_body.py, 3 seeds, sustained part, ours - real): 200-500 / 500-1600 / 1600-5000 / 5000-10000 Hz  STAT -3.2 / +3.5 / +0.0 / -11.1;  FE-A -0.8 / -2.1 / -20.5 / -29.9;  FE-B +0.4 / +6.0 / +1.0 / -7.8 dB.
Known faults: FE-A is far too dark above 1.6 kHz (the monopole alone, as body_fem INTERFACE.md section 4 warns); both FE bodies hum at A0 (87 Hz) under the whole note (the file's Q is an upper bound, the
air damping is missing); FE-B's D2 and G2 on C take 0.4-0.8 s to become periodic in the audio (STAT 0.06-0.13 s).
"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    C.OUT = out
    C.TMP = os.path.join(L.TMP, "fe_body")
    C.ROOMS = {"dry": ("off", [])}
    os.makedirs(out, exist_ok=True)
    os.makedirs(C.TMP, exist_ok=True)
    res = pickle.load(open(os.path.join(R.OUT, "res_comb.pkl"), "rb"))
    best = {k: int(np.argmin([m["tper"] if m["tper"] >= 0 else 99 for m in v["ours"]])) for k, v in res.items()}
    sets = {"notes": ([(43, "C", 0.5), (50, "G", 3.5), (57, "D", 6.5)], [43, 50, 57], [best["G2c"], best["D3"], best["A3"]]),
            "low_notes": ([(38, "C", 0.5), (43, "C", 3.5)], [38, 43], [best["D2"], best["G2c"]])}
    r = {}
    for name, (notes, refs, seeds) in sets.items():
        total = int((notes[-1][2] + C.PRE + C.POST + 0.3) * C.SR)
        outs = {t: np.zeros(total) for t, _ in BODIES}
        outs["ref"] = np.zeros(total)
        for (midi, st, t0), rm, sd in zip(notes, refs, seeds):
            sc = os.path.join(C.TMP, "%s_%d.txt" % (name, midi))
            open(sc, "w").write("# one note\n0.5000 1.5 %d 0.85 %s 1 - - 0\n" % (midi, st))
            a0 = int(round((t0 - C.PRE) * C.SR))
            for tag, knobs in BODIES:
                os.environ["ALTCELLO_KNOBS"] = knobs
                y = C.render(sc, "%s_%d_%s_s%d" % (name, midi, tag, sd), "dry", extra=["--seed", str(sd)], trace=None)
                w = C.window(y, C.onset(y))
                outs[tag][a0:a0 + len(w)] += w
            ref = C.M.load_audio(C.G.PHIL_FULL % rm)
            w = C.window(ref, C.onset(ref))
            outs["ref"][a0:a0 + len(w)] += w
        for tag, y in outs.items():
            fn = "%s_%s.wav" % (name, tag)
            r[fn] = C.match(y, os.path.join(out, fn))
    sc = os.path.join(C.TMP, "open_c2.txt")
    open(sc, "w").write("# one note\n0.5000 1.5 36 0.85 C 1 - - 0\n")
    for tag, knobs in BODIES:
        os.environ["ALTCELLO_KNOBS"] = knobs
        y = C.render(sc, "open_c2_%s" % tag, "dry", extra=["--seed", "0"], trace=None)
        w = C.window(y, C.onset(y))
        fn = "open_c2_%s.wav" % tag
        r[fn] = C.match(w, os.path.join(out, fn))
    os.environ["ALTCELLO_KNOBS"] = ""
    open(os.path.join(out, "KEY.txt"), "w").write(KEY % (" ".join(str(best[k]) for k in ("G2c", "D3", "A3")), " ".join(str(best[k]) for k in ("D2", "G2c"))))
    for k, v in sorted(r.items()):
        print("%-24s integrated %.2f LUFS, peak %.3f" % (k, v[0], v[1]))


main()

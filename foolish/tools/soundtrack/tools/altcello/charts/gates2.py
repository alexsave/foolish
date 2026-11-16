"""Gates of candidate 2 against candidate 1 (the same player with --att off) and the Philharmonia notes, dry, seeds 0-2, per note.

    python -I charts/gates2.py OUT.txt
Writes a plain text table (also read by charts/onset.py): rows 'KEY name midi seed value...'. Time to periodic: C measure (analysis.h), the onset searched from the
written start for ours and from 0 for the recordings; the other keys are the player gates of charts/player_gates.py."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
KEYS = ["tper", "att10_90", "att_tilt", "hnr", "vib_rate", "vib_depth", "lev_sd", "mod4_7"]


def gates(y, midi, t0_from, tag):
    seg, t0 = L.note_segment(y, t0_from) if t0_from is not None else (y, 0.0)
    on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0 if t0_from is not None else -1.0, tag=tag)
    g = G.gates_of(seg, midi, tag)
    return dict(tper=tp, att10_90=g["att10_90"], att_tilt=g["att_tilt"], hnr=g["hnr"], vib_rate=g["vib_rate"], vib_depth=g["vib_depth"], lev_sd=g["lev_sd"], mod4_7=g["mod4_7"])


def main():
    out = open(sys.argv[1], "w")
    out.write("# KEY style midi seed " + " ".join(KEYS) + "\n")
    for midi, rm in ((43, 43), (50, 50), (57, 57), (38, 38)):
        r = gates(M.load_audio(G.PHIL_FULL % rm), midi, None, "ph%d" % rm)
        out.write("ROW phil %d 0 " % midi + " ".join("%.4f" % r[k] for k in KEYS) + "\n")
    for style, extra in (("c1", ["--att", "off"]), ("c2", ["--att", "on"])):
        for seed in (0, 1, 2):
            for score, notes in (("single_notes.txt", [(43, 0.5), (50, 3.5), (57, 6.5)]), ("low_notes.txt", [(36, 0.5), (43, 3.5)])):
                wav = os.path.join(L.TMP, "g2_%s_%d.wav" % (style, seed))
                L.render(score, wav, extra + ["--seed", str(seed)])
                y = M.load_audio(wav)
                for midi, t in notes:
                    r = gates(y, midi, t, "g2%s%d%d" % (style, seed, midi))
                    out.write("ROW %s %d %d " % (style, midi, seed) + " ".join("%.4f" % r[k] for k in KEYS) + "\n")
                    out.flush()
                os.remove(wav)
    out.close()


main()

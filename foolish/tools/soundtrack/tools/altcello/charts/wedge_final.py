"""Step 3 measurement: the single notes G2 (on C) D3 A3 (single_notes.txt) and the low notes C2 G2 (low_notes.txt), expert, dry, seeds 0 1 2: audio time to periodic, level
re sustain at +20/+50/+100 ms after the written start, 2-6 kHz attack minus sustain, HNR. Run for any build of the renderer (candidate 3 = the commit f7e9e30c0).
    python -I charts/wedge_final.py LABEL [EXE] [SEEDS] -> out/wedge/final_LABEL.txt   (ROW label note seed tper lev20 lev50 lev100 tilt hnr)
    python -I charts/wedge_final.py phil -> the Philharmonia notes"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
import attack_scan2_lib as A  # noqa: E402
G, M = L.G, L.M
NOTES = [("G2", "single_notes.txt", 43, 0.5), ("D3", "single_notes.txt", 50, 3.5), ("A3", "single_notes.txt", 57, 6.5), ("C2", "low_notes.txt", 36, 0.5), ("G2c", "low_notes.txt", 43, 3.5)]
PHIL = {"G2": 43, "D3": 50, "A3": 57, "C2": 38, "G2c": 43}


def measure(seg, t0, midi, tag):
    on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0, tag=tag)
    g = G.gates_of(seg, midi, tag)
    return tp, A.lev(seg, t0), g["att_tilt"], g["hnr"]


def main():
    label = sys.argv[1]
    out = open(os.path.join(L.ROOT, "out", "wedge", "final_%s.txt" % label), "w")
    if label == "phil":
        for nm, _, _, _ in NOTES:
            y = M.load_audio(G.PHIL_FULL % PHIL[nm]); on = G.onset_s(y)
            a = max(int((on - 0.2) * 48000), 0); seg = y[a:a + int(1.4 * 48000)]
            tp, lv, tilt, hnr = measure(seg, on - a / 48000.0, PHIL[nm], "wf_ph")
            out.write("ROW phil %s 0 tper %.3f lev %s tilt %.1f hnr %.1f\n" % (nm, tp, " ".join("%.1f" % x for x in lv), tilt, hnr))
        return
    if len(sys.argv) > 2 and sys.argv[2] != "-": L.EXE = sys.argv[2]
    seeds = [int(s) for s in (sys.argv[3] if len(sys.argv) > 3 else "0,1,2").split(",")]
    for seed in seeds:
        for score in ("single_notes.txt", "low_notes.txt"):
            wav = os.path.join(L.TMP, "wf_%s_%s_%d.wav" % (label, score[:-4], seed))
            L.render(score, wav, ["--seed", seed] + os.environ.get("EXTRA", "").split())
            y = M.load_audio(wav)
            for nm, sc, midi, t in NOTES:
                if sc != score: continue
                seg, t0 = L.note_segment(y, t, post=1.4)
                tp, lv, tilt, hnr = measure(seg, t0, midi, "wf_%s" % label)
                out.write("ROW %s %s %d tper %.3f lev %s tilt %.1f hnr %.1f\n" % (label, nm, seed, tp, " ".join("%.1f" % x for x in lv), tilt, hnr)); out.flush()
            os.remove(wav)
    out.close()


main()

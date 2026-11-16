"""Wedge scan on the real player: single_notes_1s.txt (G2 on C, D3 on G, A3 on D, expert, thermal) for wedge points (a, F): level re sustain at +20/+50/+100 ms
after the written start, audio time to periodic, 2-6 kHz attack-minus-sustain, HNR; 3 seeds. Philharmonia levels are the PHIL rows of out/thermal/attack_scan2.txt.
    python -I charts/wedge_scan.py OUTNAME "a1:F1 a2:F2 ..." [SEEDS] [extra renderer options]  -> out/wedge/scan_OUTNAME.txt  (ROW name midi seed tper lev20 lev50 lev100 tilt hnr)"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
import attack_scan2_lib as A  # noqa: E402
G, M = L.G, L.M
SCORE = os.environ.get("SCORE", "single_notes_1s.txt")  # single_notes.txt: the 1.5 s notes of the candidates (the sustain reference depends on the note length)
NOTES = [(43, 0.5), (50, 3.5), (57, 6.5)] if SCORE == "single_notes.txt" else [(43, 0.5), (50, 2.5), (57, 4.5)]


def main():
    name = sys.argv[1]
    cfgs = [tuple(float(x) for x in c.split(":")) for c in sys.argv[2].split()]
    seeds = [int(s) for s in (sys.argv[3] if len(sys.argv) > 3 else "0,1,2").split(",")]
    extra = sys.argv[4].split() if len(sys.argv) > 4 else []
    out = open(os.path.join(L.ROOT, "out", "wedge", "scan_%s.txt" % name), "w")
    for a, F in cfgs:
        cname = "a%g_F%g" % (a, F)
        for seed in seeds:
            wav = os.path.join(L.TMP, "ws_%s_%s_%d.wav" % (name, cname, seed))
            L.render(SCORE, wav, ["--atta", a, "--attk", F / a, "--seed", seed] + extra)
            y = M.load_audio(wav)
            for midi, t in NOTES:
                seg, t0 = L.note_segment(y, t)
                on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0, tag="ws")
                g = G.gates_of(seg, midi, "ws_%d" % midi)
                out.write("ROW %s %d %d tper %.3f lev %s tilt %.1f hnr %.1f\n" % (cname, midi, seed, tp, " ".join("%.1f" % x for x in A.lev(seg, t0)), g["att_tilt"], g["hnr"])); out.flush()
            os.remove(wav)
    out.close()


main()

"""Scan of the expert attack parameters on the three single notes (G2 D3 A3, forte, scores/single_notes.txt), dry, several player seeds.

    python -I charts/onset_scan.py TAG "extra args" ["extra args"...]    -> prints one line per (config, seed, note)
Reports per note: time to periodic (C measure, onset searched from the written start), attack 10-90 ms, attack minus sustain 2-6 kHz share."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
NOTES = [(43, 0.5), (50, 2.5), (57, 4.5)]


def measure(wav, tag):
    y = M.load_audio(wav)
    res = []
    for midi, t in NOTES:
        seg, t0 = L.note_segment(y, t)
        on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0, tag=tag)
        g = G.gates_of(seg, midi, "%s_%d" % (tag, midi))
        res.append(dict(midi=midi, on=on - t0, tp=tp, att=g["att10_90"], tilt=g["att_tilt"], hnr=g["hnr"]))
    return res


def main():
    tag = sys.argv[1]
    for k, extra in enumerate(sys.argv[2:]):
        for seed in (0, 1, 2):
            wav = os.path.join(L.TMP, "scan_%s_%d_%d.wav" % (tag, k, seed))
            L.render("single_notes_1s.txt", wav, extra.split() + ["--seed", str(seed)])
            for r in measure(wav, "%s%d%d" % (tag, k, seed)):
                print("%-40s seed %d midi %d  t_per %.3f  onset+%.3f  att10_90 %.0f ms  tilt %.1f dB  hnr %.1f" % (extra, seed, r["midi"], r["tp"], r["on"], r["att"], r["tilt"], r["hnr"]), flush=True)
            os.remove(wav)


main()

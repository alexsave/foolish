"""Expert single notes under one bow model (ALTCELLO_KNOBS from the environment), dry, seeds 0-2 (charts/gates2.py notes and measures) plus the
1-3 kHz share against Philharmonia.

    ALTCELLO_KNOBS=bow=1 python -I charts/thermal_gates.py NAME      -> out/thermal/gates_NAME.txt, rows 'ROW name label seed tper att10_90 att_tilt hnr share13 phil_share13'
Time to periodic: C measure (analysis.h); att_tilt: the 2-6 kHz share of the attack minus the sustain; share13: the 1-3 kHz share of the 50-16000 Hz power from 0.5 s to
1.4 s after the onset (dB) (charts/low_study.py band_shares); the reference rows use the Philharmonia notes (D2 for C2)."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
import low_study as LS  # noqa: E402
G, M = L.G, L.M
KEYS = ["tper", "att10_90", "att_tilt", "hnr", "share13"]
PHIL = {36: 38, 43: 43, 50: 50, 57: 57}


def gates(y, midi, t0_from, tag):
    seg, t0 = L.note_segment(y, t0_from) if t0_from is not None else (y, 0.0)
    on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0 if t0_from is not None else -1.0, tag=tag)
    g = G.gates_of(seg, midi, tag)
    if t0_from is None:
        env = M.rms_env(seg); on = int(np.argmax(env >= 0.02 * env.max())) * 0.01
    sh = LS.band_shares(seg, L.SR, on + 0.5, on + 1.4)[2]
    return dict(tper=tp, att10_90=g["att10_90"], att_tilt=g["att_tilt"], hnr=g["hnr"], share13=sh)


def main():
    name = sys.argv[1]
    out = open(os.path.join(L.ROOT, "out", "thermal", "gates_%s.txt" % name), "w")
    out.write("# ROW name label seed " + " ".join(KEYS) + "\n")
    if name == "curve":
        for lab, midi, rm in (("36", 36, 38), ("43", 43, 43), ("43c", 43, 43), ("50", 50, 50), ("57", 57, 57)):
            r = gates(M.load_audio(G.PHIL_FULL % rm), midi, None, "thph%d" % rm)
            out.write("ROW phil %s 0 " % lab + " ".join("%.4f" % r[k] for k in KEYS) + "\n")
    for seed in (0, 1, 2):
        for score, notes in (("single_notes.txt", [("43", 43, 0.5), ("50", 50, 3.5), ("57", 57, 6.5)]), ("low_notes.txt", [("36", 36, 0.5), ("43c", 43, 3.5)])):
            wav = os.path.join(L.TMP, "tg_%s_%d.wav" % (name, seed))
            L.render(score, wav, ["--att", "on", "--seed", str(seed)])
            y = M.load_audio(wav)
            for lab, midi, t in notes:
                r = gates(y, midi, t, "tg%s%d%s" % (name, seed, lab))
                out.write("ROW %s %s %d " % (name, lab, seed) + " ".join("%.4f" % r[k] for k in KEYS) + "\n")
                out.flush()
            os.remove(wav)
    out.close()


main()

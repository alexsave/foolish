"""Player gates of the notes of two candidates against the Philharmonia notes of the same file set (charts/player_gates.py gates_of on each note window
of notes_alt_dry.wav, low_notes_alt_dry.wav, and notes_ref.wav, low_notes_ref.wav), plus the audio time to periodic of the C measure.

    python -I charts/cand_gates.py CAND_A_DIR CAND_B_DIR     -> out/thermal/cand_gates.txt and the table on stdout, low (C, G) and high (D, A) strings apart
Notes: single_notes at 0.4 3.4 6.4 s (G2 D3 A3), low_notes at 0.4 3.4 s (C2 vs D2, G2 on C); windows start 0.0 / 0.1 s before the onset (candidate2.py)."""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
SR = L.SR
KEYS = ["tper", "t_per", "att10_90", "att_tilt", "mod4_7", "mod10", "lev_sd", "vib_rate", "vib_depth", "hnr"]  # the release gates are left out: the candidate windows end 0.1 s after the lift
LABEL = {"tper": "audio time to periodic s (C measure)", "t_per": "time to periodic s (gate)", "att10_90": "attack 10-90 ms", "att_tilt": "attack - sustain 2-6 kHz dB",
         "mod4_7": "4-7 Hz modulation share", "mod10": "above-10 Hz share", "lev_sd": "level sd dB", "vib_rate": "vibrato rate Hz", "vib_depth": "vibrato depth c",
         "hnr": "HNR dB"}
NOTES = [("single", "notes", [("G2 (G string)", 43, 0.4, "G"), ("D3 (D string)", 50, 3.4, "D"), ("A3 (A string)", 57, 6.4, "A")]),
         ("low", "low_notes", [("C2 (C string; ref D2)", 36, 0.4, "C"), ("G2 on C (C string)", 43, 3.4, "C")])]


def gates(y, midi, t0, tag):
    seg = y[int(t0 * SR):int((t0 + 2.6) * SR)]
    on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=0.0, tag=tag)
    g = G.gates_of(seg, midi, tag)
    r = {k: g.get(k, np.nan) for k in KEYS}
    r["tper"] = tp
    return r


def main():
    dirs = sys.argv[1:3]
    res = {}
    for kind, base, notes in NOTES:
        for lab, midi, t, s in notes:
            res[(lab, "phil")] = gates(M.load_audio(os.path.join(dirs[0], "%s_ref.wav" % base)), 38 if midi == 36 else midi, t, "cgref%d" % midi + s + kind)
            for d in dirs:
                res[(lab, os.path.basename(d))] = gates(M.load_audio(os.path.join(d, "%s_alt_dry.wav" % base)), midi, t, "cg%s%d%s" % (os.path.basename(d), midi, kind))
    out = []
    names = ["phil"] + [os.path.basename(d) for d in dirs]
    for grp, labs in (("LOW STRINGS (C, G)", ["C2 (C string; ref D2)", "G2 on C (C string)", "G2 (G string)"]), ("HIGH STRINGS (D, A)", ["D3 (D string)", "A3 (A string)"])):
        out.append("== " + grp)
        for lab in labs:
            out.append(lab)
            out.append("  %-34s " % "gate" + " ".join("%10s" % n for n in names))
            for k in KEYS:
                out.append("  %-34s " % LABEL[k] + " ".join("%10.2f" % res[(lab, n)][k] for n in names))
    txt = "\n".join(out)
    open(os.path.join(L.ROOT, "out", "thermal", "cand_gates.txt"), "w").write(txt + "\n")
    print(txt)


main()

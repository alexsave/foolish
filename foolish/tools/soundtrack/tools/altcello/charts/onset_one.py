"""One note, many settings: python -I charts/onset_one.py MIDI "args" ["args"...]  -> t_per (3 seeds), attack, hnr"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import onset_lib as L  # noqa: E402
G, M = L.G, L.M
midi = int(sys.argv[1])
sc = os.path.join(L.TMP, "one_%d.txt" % midi)
os.makedirs(L.TMP, exist_ok=True)
open(sc, "w").write("0.5 1.0 %d 0.85 - 1 - - 0\n" % midi)
for k, extra in enumerate(sys.argv[2:]):
    out = []
    for seed in (0, 1, 2):
        wav = os.path.join(L.TMP, "one_%d_%d.wav" % (midi, seed))
        L.render(sc, wav, extra.split() + ["--seed", str(seed)])
        y = M.load_audio(wav)
        seg, t0 = L.note_segment(y, 0.5)
        on, tp = L.t_periodic(seg, M.midi_hz(midi), t_from=t0, tag="one%d" % seed)
        g = G.gates_of(seg, midi, "one%d" % seed)
        out.append((tp, g["att10_90"], g["hnr"], g["att_tilt"]))
        os.remove(wav)
    print("%-36s t_per %s  att %s  hnr %s" % (extra, "/".join("x" if o[0] < 0 else "%.2f" % o[0] for o in out), "/".join("%.0f" % o[1] for o in out), "/".join("%.0f" % o[2] for o in out)), flush=True)

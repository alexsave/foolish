"""One stopped note (vibrato on) over bow position, speed and band position: HNR. python -I charts/player_scan3.py MIDI"""
import os, sys, subprocess, itertools
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_gates as G  # noqa: E402
import numpy as np
M = G.M
EXE = os.path.join(G.ROOT, "altcello")
midi = int(sys.argv[1]) if len(sys.argv) > 1 else 43
notes = os.path.join(G.TMP, "scan3_notes.txt")
open(notes, "w").write("\n".join("%.2f 1.5 %d 0.85 - 1 - - 0" % (0.5 + 3.0 * i, midi) for i in range(3)) + "\n")
for b, vb, fr in itertools.product((0.105, 0.13), (0.06, 0.12), (0.2, 0.35, 0.5)):
    wav = os.path.join(G.TMP, "scan3.wav")
    subprocess.run(["nice", EXE, "render", notes, wav, "--room", "off", "--beta0", str(b), "--vb", str(vb), "1e-6", "--frac", str(fr), "1e-6"], capture_output=True, check=True)
    y = M.load_audio(wav)
    h = [G.alt_note_gates(y, 0.5 + 3.0 * i, midi, "scan3")["hnr"] for i in range(3)]
    print("beta %.3f vb %.2f frac %.2f hnr %s" % (b, vb, fr, " ".join("%.1f" % v for v in h)), flush=True)

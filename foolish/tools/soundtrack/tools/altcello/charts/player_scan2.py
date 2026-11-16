"""Scan one stopped note (string, semitones) over band position and speed and bow position: HNR and time to periodic."""
import os, sys, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import player_gates as G  # noqa: E402
import numpy as np
M = G.M
EXE = os.path.join(G.ROOT, "altcello")
string, midi = sys.argv[1], int(sys.argv[2])
vbs = [float(x) for x in sys.argv[3].split(",")]
betas = [float(x) for x in sys.argv[4].split(",")]
vels = [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
notes = os.path.join(G.TMP, "scan2_notes.txt")
open(notes, "w").write("\n".join("%.2f 1.5 %d %.2f %s 0 - - 0" % (0.5 + 2.2 * i, midi, v, string) for i, v in enumerate(vels)) + "\n")
for vb in vbs:
    for b in betas:
        wav = os.path.join(G.TMP, "scan2.wav")
        subprocess.run(["nice", EXE, "render", notes, wav, "--room", "off", "--vb", str(vb), "1e-6", "--frac", "0.2", "0.6", "--beta0", str(b)], capture_output=True, check=True)
        y = M.load_audio(wav)
        out = []
        for i, v in enumerate(vels):
            r = G.alt_note_gates(y, 0.5 + 2.2 * i, midi, "scan2", post=0.3)
            out.append("%.2f:%4.1f/%s" % (0.2 + 0.6 * v, r["hnr"], ("%.2f" % r["t_per"]) if np.isfinite(r["t_per"]) else "n/a"))
        print("vb %.2f beta %.3f  frac:hnr/tper  " % (vb, b) + "  ".join(out), flush=True)

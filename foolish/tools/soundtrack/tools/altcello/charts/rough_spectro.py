"""Note-axis spectrograms (CQT C1-C9, 65 dB below each file's own peak) of the low-note roughness work: Philharmonia | candidate 5 | candidate 6, one row per note (seed 0).
python -I charts/rough_spectro.py [C5TAG C6TAG OUT.png]      defaults: base c_n4 /Users/alex/Music/foolish_cities_v5/alt_cello/charts/rough_iter1_spectro.png"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, "/Users/alex/Music/foolish_cities_v5/shared_tools/spectro")
import rough_lib as RL, rich_lib as R  # noqa: E402
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import librosa  # noqa: E402
from notespec import cqt_db  # noqa: E402

t5 = sys.argv[1] if len(sys.argv) > 1 else "base"
t6 = sys.argv[2] if len(sys.argv) > 2 else "c_n4"
out = sys.argv[3] if len(sys.argv) > 3 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/rough_iter1_spectro.png"
keys = ["D2", "G2c", "D3", "A3"]
lab = {"D2": "D2 (C2+2 on C)", "G2c": "G2 on C", "D3": "D3 on G", "A3": "A3 on D"}
fmin = librosa.note_to_hz("C1")
fig, axs = plt.subplots(4, 3, figsize=(15, 16), dpi=90, sharey=True)
fig.suptitle("ALT-CELLO - low-note roughness - note-axis spectrograms C1-C9, 65 dB range below each file's own peak (seed 0)", fontsize=15, fontweight="bold", y=0.995)
for r, k in enumerate(keys):
    nm = R.NOTES[k][2]
    for c, (t, title) in enumerate([("ref", "Philharmonia"), (t5, "candidate 5 (lumped contact)"), (t6, "candidate 6 (4 contact points)")]):
        y = R.ref_note(k) if t == "ref" else R.M.load_audio(os.path.join(RL.OUT, "%s_%s_s0.wav" % (t, nm)))
        on = R.onset(y)
        a, b = int(max(on - 0.15, 0) * R.SR), int((on + 1.6) * R.SR)
        db, tt, f = cqt_db(y[a:b], R.SR, hop=512, fmin=fmin, octaves=8)
        ax = axs[r, c]
        ax.imshow(db, origin="lower", aspect="auto", extent=[0, tt[-1], 0, db.shape[0]], vmin=db.max() - 65, vmax=db.max(), cmap="magma")
        ticks = [(s * 36 / 12.0, "C%d" % (1 + s // 12)) for s in range(0, 8 * 12 + 1, 12)]
        ax.set_yticks([p for p, _ in ticks])
        ax.set_yticklabels([l for _, l in ticks], fontsize=9)
        for p, _ in ticks:
            ax.axhline(p, color="white", lw=0.5, alpha=0.45)
        if r == 0: ax.set_title(title, fontsize=13)
        if c == 0: ax.set_ylabel(lab[k] + "\nnote", fontsize=11)
        if r == 3: ax.set_xlabel("s from 0.15 s before the onset")
fig.tight_layout(rect=(0, 0, 1, 0.985))
fig.savefig(out, dpi=90)
print(out)

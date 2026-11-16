"""Contact pickup set (pickup1): level to -20 LUFS, KEY.txt, third-octave LTAS table against the room render, note-axis spectrograms (C1-C9, 80 dB).
   python -I charts/pickup_post.py level|ltas|chart"""
import os, sys, subprocess, re
import numpy as np
import scipy.io.wavfile as sw
import scipy.signal as ss
sys.path.insert(0, "/Users/alex/Music/foolish_cities_v5/shared_tools/spectro")
RAW = "/private/tmp/claude-501/pk"
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/pickup1"
CH = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts"
SR = 48000
FILES = [  # raw file, label, description
    ("stat.wav", "ref_room0p6", "REFERENCE: the normal room_0p6 render of the same take (cardioid, 1.5 m, radiated pressure), statistical body"),
    ("stat_bridge_u_acc_fc16.wav", "P1_piezo_bridge_u_fc16", "P1 piezo on the bridge, in-plane (bowing direction u) bridge acceleration, high-pass 16 Hz (1 nF into 10 MOhm), statistical body. THE MAIN ONE"),
    ("stat_bridge_w_acc_fc16.wav", "P1_piezo_bridge_w_fc16", "P1 piezo, bridge motion normal to the top (w), acceleration, high-pass 16 Hz, statistical body"),
    ("stat_bridge_u_vel.wav", "P2_velocity_bridge_u", "P2 velocity pickup (electrodynamic) on the bridge, u, statistical body"),
    ("stat_bridge_w_vel.wav", "P2_velocity_bridge_w", "P2 velocity pickup on the bridge, w, statistical body"),
    ("stat_bridge_u_acc_fc160.wav", "P1_piezo_bridge_u_fc160", "P1 piezo, u, high-pass 160 Hz (1 nF into 1 MOhm), statistical body"),
    ("fe_top_acc_fc16.wav", "P3_FE_top_centre_acc", "P3 solved FE body (fe=2): top plate centre, normal acceleration, high-pass 16 Hz; the modes above 5 kHz are not in the FE point shape"),
    ("fe_top_vel.wav", "P3_FE_top_centre_vel", "P3 FE body: top plate centre, normal velocity"),
    ("fe_back_acc_fc16.wav", "P3_FE_back_centre_acc", "P3 FE body: back plate centre, normal acceleration, high-pass 16 Hz"),
    ("fe_back_vel.wav", "P3_FE_back_centre_vel", "P3 FE body: back plate centre, normal velocity"),
    ("fe_bridge_u_acc_fc16.wav", "P1_FE_bridge_u_fc16", "P1 piezo on the bridge (u, 16 Hz) with the solved FE body for comparison with file 02"),
]
NAMES = ["%02d_%s" % (i + 1, f[1]) for i, f in enumerate(FILES)]
BANDS = 1000.0 * 2.0 ** (np.arange(-13, 14) / 3.0)  # 63 Hz ... 16 kHz third-octave centres


def lufs(path):
    r = subprocess.run(["ffmpeg", "-nostats", "-i", path, "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True)
    return float(re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)[-1])


def load(path):
    sr, y = sw.read(path)
    assert sr == SR
    return y.astype(np.float64)


def level():
    key = ["ALT-CELLO pickup1 (contact pickups, Alex: 'what if you put the mic ON the cello'). 48 kHz mono float32, integrated loudness -20 LUFS (ffmpeg ebur128, one gain per file).",
           "Bach BWV 1007 Prelude bars 1-4 (scores/bach_bwv1007_bars1_4.txt), expert, seed 0, current default instrument (4-point contact, statistical body) except files 07-11 (solved FE-B body, fe=2).",
           "A contact pickup senses the surface motion of one point, not the radiated pressure: no room, no distance. Files 02-06 and the reference 01 are the SAME take (one simulation).",
           "Transducer constants: piezo C 1 nF; 10 MOhm input gives f_c 15.9 Hz (16 Hz files), 1 MOhm gives 159 Hz (160 Hz file). Output = acceleration (derivative of the velocity) through a one-pole high-pass. No other EQ.", ""]
    for (raw, lab, desc), nm in zip(FILES, NAMES):
        y = load(os.path.join(RAW, raw))
        p = os.path.join(OUT, nm + ".wav")
        y = y / np.max(np.abs(y)) * 0.5
        sw.write(p, SR, y.astype(np.float32))
        g = 10 ** ((-20.0 - lufs(p)) / 20)
        sw.write(p, SR, (y * g).astype(np.float32))
        key.append("%s.wav  %s  (%.2f LUFS, peak %.3f)" % (nm, desc, lufs(p), np.max(np.abs(y * g))))
    open(os.path.join(OUT, "KEY.txt"), "w").write("\n".join(key) + "\n")
    print("\n".join(key))


def third(y):
    f, P = ss.welch(y, SR, nperseg=16384)
    out = []
    for c in BANDS:
        m = (f >= c / 2 ** (1 / 6)) & (f < c * 2 ** (1 / 6))
        out.append(10 * np.log10(np.sum(P[m]) + 1e-30))
    return np.array(out)


def ltas():
    ys = [load(os.path.join(OUT, n + ".wav")) for n in NAMES]
    L = [third(y) for y in ys]
    ref = L[0]
    sel = (BANDS >= 62) & (BANDS <= 16100)
    print("third-octave LTAS, dB re the room render (file 01), all files -20 LUFS")
    print("%6s " % "Hz" + " ".join("%5s" % n[:2] for n in NAMES))
    for i in np.where(sel)[0]:
        print("%6.0f " % BANDS[i] + " ".join("%+5.0f" % (l[i] - ref[i]) for l in L))
    print("\nabsolute level of file 01 per band (dB):", " ".join("%.0f" % v for v in ref[sel]))
    for n, y, l in zip(NAMES, ys, L):
        P = 10 ** (l / 10)
        cen = np.sum(BANDS * P) / np.sum(P)
        lo = 10 * np.log10(np.sum(P[(BANDS < 300)]) / np.sum(P)); mid = 10 * np.log10(np.sum(P[(BANDS >= 500) & (BANDS < 2000)]) / np.sum(P))
        hi = 10 * np.log10(np.sum(P[(BANDS >= 4000)]) / np.sum(P))
        pk = BANDS[np.argmax(np.where(sel, l, -999))]
        print("%-32s centroid %5.0f Hz  share <300 %+5.1f dB  500-2k %+5.1f  >4k %+5.1f  peak band %5.0f Hz" % (n, cen, lo, mid, hi, pk))


def chart():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import librosa
    import notespec as N
    fmin = librosa.note_to_hz("C1")
    N.note_ticks = lambda fmin=fmin, octaves=8, bpo=N.BPO: [(i * bpo / 12.0, N.NOTE_NAMES[i % 12] + str(1 + i // 12), i % 12 == 0) for i in range(octaves * 12 + 1)]
    for (raw, lab, desc), nm in zip(FILES, NAMES):
        y = load(os.path.join(OUT, nm + ".wav"))
        fig, ax = plt.subplots(figsize=(18, 7))
        N.show_note_spectrogram(ax, y, SR, title="%s: %s" % (nm, desc[:95]), vrange=80, fmin=fmin, octaves=8)
        ax.set_xlabel("s")
        fig.tight_layout()
        p = os.path.join(CH, "pickup1_%s.png" % nm)
        fig.savefig(p, dpi=70)
        plt.close(fig)
        print(p, flush=True)


if __name__ == "__main__":
    {"level": level, "ltas": ltas, "chart": chart}[sys.argv[1]]()

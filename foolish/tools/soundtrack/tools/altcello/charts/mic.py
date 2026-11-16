"""Microphone position study, iteration 1 (ROOM stage only; the source is a point).
Inputs: raw room renders in RAW (made by /private/tmp/claude-501/mic1/render_mics.sh and ir_runs.sh with `altcello room ... --rt60 0.6 --abs-level`).
  python -I charts/mic.py post      # Bach renders at -20 LUFS into audio/mic1/ (numbered 01-06 = M1-M6) + KEY.txt part 1
  python -I charts/mic.py measure   # DRR, proximity boost, self-noise re signal, LTAS gaps against Philharmonia -> out/mic/mic_measure.npz and a text table
  python -I charts/mic.py notes M   # single notes and low notes at position M (m1..m6), with references, into audio/mic1/ (07-10), KEY.txt part 2
  python -I charts/mic.py spectro   # note-axis spectrograms (C1-C9, 80 dB) of M1, M3, M4
  python -I charts/mic.py chart     # charts/mic_iter1.png"""
import os, sys, re, subprocess
import numpy as np
import scipy.io.wavfile as sw
import scipy.signal as ss
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, "/Users/alex/Music/foolish_cities_v5/shared_tools/spectro")
SR = 48000
RAW = "/private/tmp/claude-501/mic1/raw"
ROOT = "/Users/alex/Music/foolish_cities_v5/alt_cello"
OUT = ROOT + "/audio/mic1"
C5 = ROOT + "/audio/candidate5"
CH = ROOT + "/charts"
RES = os.path.join(HERE, "..", "out", "mic")
# name, label, mic position (room m), pattern, short description
POS = [
    ("m1", "M1", (2.7, 3.6952, 0.72), "cardioid", "cardioid 0.3 m, 30 deg above the front axis (upper-bout side)"),
    ("m2", "M2", (2.7, 3.3783, 0.7354), "cardioid", "cardioid 0.6 m, in front (16 deg up, like M4)"),
    ("m3", "M3", (2.7, 2.9937, 0.8456), "cardioid", "cardioid 1.0 m, in front (16 deg up)"),
    ("m4", "M4", (2.7, 2.4550, 1.0), "cardioid", "cardioid 1.56 m (the current default: 1.5 m out, 1.0 m high)"),
    ("m5", "M5", (2.7, 3.805, 1.05), "cardioid", "cardioid 0.5 m above the bridge/fingerboard end, aimed down"),
    ("m6", "M6", (2.7, 3.6952, 0.72), "omni", "omni 0.3 m, same place as M1"),
]
SRC = (2.7, 3.955, 0.57)
NOTES3 = [("G2c", 0.5), ("D3", 3.5), ("A3", 6.5)]
NOTES2 = [("D2", 0.5), ("G2c", 3.5)]
RANGES = [(200, 500), (500, 1600), (1600, 5000), (5000, 10000.1)]
RLAB = ["200-500", "500-1600", "1600-5000", "5000-10000"]
BANDS = [10 ** (0.1 * k) for k in range(17, 41)]  # 50 Hz .. 10 kHz third octaves, as rich_lib


def load(p):
    sr, y = sw.read(p)
    assert sr == SR
    return y.astype(np.float64)


def lufs(path):
    r = subprocess.run(["ffmpeg", "-nostats", "-i", path, "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True)
    return float(re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)[-1])


def write_lufs(y, path, target=-20.0):
    y = y * (0.1 / np.max(np.abs(y)))  # the raw close-mic render is far above full scale: measure at a sane level (ffmpeg clips at 1.0)
    sw.write(path, SR, y.astype(np.float32))
    g = 10 ** ((target - lufs(path)) / 20)
    for _ in range(3):  # the gated loudness is not exactly linear in the gain on a file with a noise floor: iterate
        sw.write(path, SR, (y * g).astype(np.float32))
        L = lufs(path)
        if abs(L - target) < 0.005:
            break
        g *= 10 ** ((target - L) / 20)
    return L, float(np.max(np.abs(y * g)))


def rms_env(y, hop=480, win=960):
    n = (len(y) - win) // hop + 1
    idx = np.arange(win)[None, :] + hop * np.arange(n)[:, None]
    return np.sqrt(np.mean(y[idx] ** 2, axis=1) + 1e-20)


def onset(y):
    env = rms_env(y)
    return int(np.argmax(env >= 0.02 * env.max())) * 0.01


def ltas_third(seg):
    f, p = ss.welch(seg, SR, nperseg=16384, noverlap=8192, window="hann")
    out = []
    for fc in BANDS:
        m = (f >= fc / 2 ** (1 / 6)) & (f < fc * 2 ** (1 / 6))
        out.append(10 * np.log10(np.sum(p[m]) + 1e-30))
    out = np.array(out)
    return out - out.max()


def gaps(l_ours, l_ref):
    d = l_ours - l_ref
    return [float(np.mean(d[np.array([(fc >= lo * 0.999 and fc < hi) for fc in BANDS])])) for lo, hi in RANGES]


def note_ltas(y, t0):
    """third-octave LTAS (re own peak) of the sustained part (0.5-1.4 s after the onset) of the note placed at t0"""
    a = int((t0 - 0.3) * SR)
    seg = y[a:a + int(2.2 * SR)]
    on = onset(seg)
    s = seg[int((on + 0.5) * SR):int((on + 1.4) * SR)]
    return ltas_third(s)


def active_rms(y):
    fl = int(0.02 * SR)
    nf = len(y) // fl
    p = np.array([np.mean(y[i * fl:(i + 1) * fl] ** 2) for i in range(nf)])
    return np.sqrt(np.mean(p[p >= 0.0625 * p.max()]))


def aweight(f):
    f = np.maximum(f, 1e-3)
    f2 = f * f
    c1 = 12194.0 ** 2
    ra = c1 * f2 * f2 / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + c1))
    return ra * 10 ** (2.0 / 20)


def a_rms(y):
    Y = np.fft.rfft(y)
    f = np.fft.rfftfreq(len(y), 1.0 / SR)
    Y *= aweight(f) * (f >= 20)
    return np.sqrt(np.sum(np.abs(Y) ** 2) * 2 / len(y) ** 2)


def band_db(y, lo, hi):
    f, p = ss.welch(y, SR, nperseg=16384, noverlap=8192, window="hann")
    m = (f >= lo) & (f < hi)
    return 10 * np.log10(np.sum(p[m]))


def ir_drr(name, half=0.0006):
    ir = np.fromfile(os.path.join(RAW, "irnp_%s.wav_ir.f32" % name), dtype=np.float32).astype(np.float64)
    mic = [p for p in POS if p[0] == name][0][2]
    r = np.sqrt(sum((a - b) ** 2 for a, b in zip(SRC, mic)))
    t = r / (331.3 * np.sqrt(1 + 20 / 273.15))  # the room's speed of sound at 20 C
    a, b = int((t - half) * SR), int(np.ceil((t + half) * SR))
    e = ir ** 2
    return 10 * np.log10(e[a:b + 1].sum() / (e.sum() - e[a:b + 1].sum())), r


DESC = {p[0]: p[4] for p in POS}


def post():
    os.makedirs(OUT, exist_ok=True)
    key = ["ALT-CELLO microphone position study, iteration 1 (for the lead; the dispatcher blinds the order). 48 kHz mono float32, one gain per file to -20 LUFS (ffmpeg ebur128).",
           "Same dry render for every file: 01_bach_s0_dry.wav of audio/music1 (Bach BWV 1007 prelude bars 1-4, seed 0, expert player, candidate-5 instrument), through the room, RT60 0.6 s, source a point at the f-hole/bridge region.",
           "Room: altcello room --rt60 0.6 --abs-level (the level of every position is set by the 1.5 m default placement, so the self-noise follows the position), cardioid proximity effect on (room.c).",
           "Room coordinates: source (2.70, 3.955, 0.57) m, the cello faces -Y, Z up. Point-source limit: at 0.3-0.5 m the real microphone is in the body's near field; the f-hole against bridge against top-plate differences are NOT modelled.", ""]
    for i, (n, lab, pos, pat, d) in enumerate(POS):
        y = load(os.path.join(RAW, "bach_%s.wav" % n))
        fn = "%02d_bach_%s.wav" % (i + 1, n)
        L, pk = write_lufs(y, os.path.join(OUT, fn))
        key.append("%s  %s  mic (%.2f, %.2f, %.2f)  (%.2f LUFS, peak %.3f)" % (fn, d, pos[0], pos[1], pos[2], L, pk))
    open(os.path.join(OUT, "KEY.txt"), "w").write("\n".join(key) + "\n")
    print("\n".join(key))


def measure():
    os.makedirs(RES, exist_ok=True)
    dry = load(ROOT + "/audio/music1/01_bach_s0_dry.wav")
    res = {}
    lines = []
    ref3 = load(C5 + "/notes_ref.wav")
    ref2 = load(C5 + "/low_notes_ref.wav")
    lt_ref3 = [note_ltas(ref3, t) for _, t in NOTES3]
    lt_ref2 = [note_ltas(ref2, t) for _, t in NOTES2]
    drys = {"3": load(C5 + "/notes_alt_dry.wav"), "2": load(C5 + "/low_notes_alt_dry.wav")}
    lt_dry3 = [note_ltas(drys["3"], t) for _, t in NOTES3]
    gd3 = np.array([gaps(a, b) for a, b in zip(lt_dry3, lt_ref3)])
    res["dry_gap3"] = gd3
    res["ref_ltas3"] = np.array(lt_ref3)
    res["ref_ltas2"] = np.array(lt_ref2)
    prox = {}
    for n, lab, pos, pat, d in POS:
        y = load(os.path.join(RAW, "bach_%s.wav" % n))
        lo = band_db(y, 30, 200) - band_db(y, 200, 2000)
        prox[n] = lo
    iso = {}
    for n, lab, pos, pat, d in POS:
        f = os.path.join(RAW, "bach_%snp.wav" % n)
        iso[n] = prox[n] - (band_db(load(f), 30, 200) - band_db(load(f), 200, 2000)) if os.path.exists(f) else 0.0  # the omni capsule has no proximity effect
    for n, lab, pos, pat, d in POS:
        drr, r = ir_drr(n)
        sig = active_rms(load(os.path.join(RAW, "bach_%s.wav" % n)))
        noi = load(os.path.join(RAW, "noise_%s.wav" % n))[:SR * 20]
        yb = load(os.path.join(RAW, "bach_%s.wav" % n))
        snr = 20 * np.log10(sig / np.sqrt(np.mean(noi ** 2)))
        # A-weighted: signal over its active part (whole file, weighted, active frames)
        ya = np.fft.irfft(np.fft.rfft(yb) * aweight(np.fft.rfftfreq(len(yb), 1.0 / SR)) * (np.fft.rfftfreq(len(yb), 1.0 / SR) >= 20), len(yb))
        snr_a = 20 * np.log10(active_rms(ya) / a_rms(noi))
        g3, l3, g2 = [], [], []
        yn = load(os.path.join(RAW, "notes_%s.wav" % n))
        for (nm, t), lr in zip(NOTES3, lt_ref3):
            l = note_ltas(yn, t)
            l3.append(l)
            g3.append(gaps(l, lr))
        yl = load(os.path.join(RAW, "low_%s.wav" % n))
        for (nm, t), lr in zip(NOTES2, lt_ref2):
            g2.append(gaps(note_ltas(yl, t), lr))
        g3, g2 = np.array(g3), np.array(g2)
        res[n + "_drr"], res[n + "_r"], res[n + "_prox"], res[n + "_snr"], res[n + "_snra"] = drr, r, prox[n] - prox["m4"], snr, snr_a
        res[n + "_iso"] = iso[n]
        res[n + "_gap3"], res[n + "_gap2"], res[n + "_ltas3"] = g3, g2, np.array(l3)
        m3 = g3.mean(axis=0)
        lines.append("%s %-8s r %.2f m  DRR %6.2f dB  <200 Hz re 200-2000 Hz vs M4 %+6.2f dB (proximity alone %+5.2f)  noise re signal %6.1f dB (A-weighted %6.1f)  gaps(3 notes mean) %s  mean|gap| %.2f  (low notes mean|gap| %.2f)" % (
            lab, pat, r, drr, prox[n] - prox["m4"], iso[n], snr, snr_a, " ".join("%+6.2f" % v for v in m3), np.mean(np.abs(m3)), np.mean(np.abs(g2.mean(axis=0)))))
        for (nm, t), row in zip(NOTES3, g3):
            lines.append("      %-4s gaps %s  mean|gap| %.2f" % (nm, " ".join("%+6.2f" % v for v in row), np.mean(np.abs(row))))
    lines.append("dry (no room, no mic): gaps(3 notes mean) %s  mean|gap| %.2f" % (" ".join("%+6.2f" % v for v in gd3.mean(axis=0)), np.mean(np.abs(gd3.mean(axis=0)))))
    lines.append("dry source <200 Hz re 200-2000 Hz: %.2f dB; M4 render: %.2f dB" % (band_db(dry, 30, 200) - band_db(dry, 200, 2000), prox["m4"]))
    np.savez(os.path.join(RES, "mic_measure.npz"), **res)
    txt = "\n".join(lines)
    open(os.path.join(RES, "mic_measure.txt"), "w").write(txt + "\n")
    print(txt)


def notes(best):
    key = open(os.path.join(OUT, "KEY.txt")).read().rstrip("\n").split("\n")
    key = [l for l in key if not l.startswith("07_") and not l.startswith("08_") and not l.startswith("09_") and not l.startswith("10_")]
    items = [("07_notes_%s.wav" % best, "notes_%s.wav" % best, "candidate 5 single notes G2 on C, D3, A3 (separate simulations, seeds 1 1 2) at %s: %s" % (best.upper(), DESC[best])),
             ("08_notes_ref.wav", None, "Philharmonia reference of 07 (ph_cello_43, 50, 57), same placement in time, -20 LUFS"),
             ("09_low_notes_%s.wav" % best, "low_%s.wav" % best, "candidate 5 low notes D2 on C, G2 on C (seeds 0 1) at %s: %s" % (best.upper(), DESC[best])),
             ("10_low_notes_ref.wav", None, "Philharmonia reference of 09 (ph_cello_38, 43), -20 LUFS")]
    for fn, src, d in items:
        if src is None:
            y = load(C5 + ("/notes_ref.wav" if "low" not in fn else "/low_notes_ref.wav"))
            L, pk = write_lufs(y, os.path.join(OUT, fn))
        else:
            y = load(os.path.join(RAW, src))
            n = int((8.65 if "low" not in fn else 5.65) * SR)  # the candidate 5 length
            L, pk = write_lufs(y[:n], os.path.join(OUT, fn))
        key.append("%s  %s  (%.2f LUFS, peak %.3f)" % (fn, d, L, pk))
    open(os.path.join(OUT, "KEY.txt"), "w").write("\n".join(key) + "\n")
    print("\n".join(key[-4:]))


def spectro():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import librosa
    import notespec as N
    fmin = librosa.note_to_hz("C1")
    N.note_ticks = lambda fmin=fmin, octaves=8, bpo=N.BPO: [(i * bpo / 12.0, N.NOTE_NAMES[i % 12] + str(1 + i // 12), i % 12 == 0) for i in range(octaves * 12 + 1)]
    for i, (n, lab, pos, pat, d) in enumerate(POS):
        if n not in ("m1", "m3", "m4"):
            continue
        y = load(os.path.join(OUT, "%02d_bach_%s.wav" % (i + 1, n)))
        fig, ax = plt.subplots(figsize=(18, 7))
        N.show_note_spectrogram(ax, y, SR, title="%s: %s" % (lab, d), vrange=80, fmin=fmin, octaves=8)
        ax.set_xlabel("s")
        fig.tight_layout()
        p = os.path.join(CH, "mic_notespec_%s.png" % n)
        fig.savefig(p, dpi=70)
        plt.close(fig)
        print(p)


def chart():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.gridspec import GridSpec
    R = np.load(os.path.join(RES, "mic_measure.npz"))
    cols = {"m1": "#c0392b", "m2": "#e67e22", "m3": "#27ae60", "m4": "#2c3e50", "m5": "#8e44ad", "m6": "#16a085"}
    labs = [p[1] for p in POS]
    fig = plt.figure(figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - microphone distance and pattern: proximity effect, direct-to-reverberant ratio, spectrum against Philharmonia - iteration 1", fontsize=13, fontweight="bold", y=0.985)
    gs = GridSpec(3, 12, figure=fig, left=0.055, right=0.985, top=0.935, bottom=0.04, hspace=0.42, wspace=2.2, height_ratios=[1.0, 1.05, 0.62])
    # plan and side view
    ap = fig.add_subplot(gs[0, 0:4])
    asd = fig.add_subplot(gs[0, 4:8])
    for ax, (ix, iy, xl, yl) in ((ap, (0, 1, "room X (m)", "room Y (m)")), (asd, (1, 2, "room Y (m)", "room Z (m)"))):
        ax.set_xlabel(xl)
        ax.set_ylabel(yl)
        ax.plot(SRC[ix], SRC[iy], "k*", ms=14, zorder=5)
        for n, lab, pos, pat, d in POS:
            ax.plot(pos[ix], pos[iy], "o" if pat == "cardioid" else "s", color=cols[n], ms=8, zorder=4)
            dx, dy = SRC[ix] - pos[ix], SRC[iy] - pos[iy]
            if abs(dx) + abs(dy) > 1e-6:
                ax.annotate("", xy=(pos[ix] + 0.35 * dx, pos[iy] + 0.35 * dy), xytext=(pos[ix], pos[iy]),
                            arrowprops=dict(arrowstyle="->", color=cols[n], lw=1.2))
    ap.set_xlim(1.9, 3.5); ap.set_ylim(2.2, 4.4); ap.set_aspect("equal")
    ap.add_patch(plt.Rectangle((SRC[0] - 0.22, SRC[1] - 0.02), 0.44, 0.12, fc="0.8", ec="0.4", zorder=1))  # the cello body seen from above, schematic (room walls: 6 x 5 m, off this zoom)
    ap.set_title("plan view, zoom (room 6 x 5 m; cello faces -Y; star = source)", fontsize=10)
    asd.set_xlim(2.2, 4.4); asd.set_ylim(0, 1.6); asd.set_aspect("equal")
    asd.set_title("side view, zoom (Z up; floor at 0)", fontsize=10)
    asd.axhline(0, color="0.4")
    asd.add_patch(plt.Rectangle((SRC[1] - 0.02, 0.0), 0.14, 1.29, fc="0.8", ec="0.4", zorder=1))  # the cello standing on its endpin: body, neck, schematic
    asd.text(SRC[1] + 0.14, 1.33, "cello", fontsize=8, color="0.4")
    # label M positions
    for n, lab, pos, pat, d in POS:
        off = {"m1": (7, 2), "m6": (7, -10), "m5": (-26, -4), "m2": (7, -3), "m3": (7, -3), "m4": (7, -3)}[n]
        ap.annotate(lab, (pos[0], pos[1]), textcoords="offset points", xytext=off, fontsize=8, color=cols[n])
        asd.annotate(lab, (pos[1], pos[2]), textcoords="offset points", xytext=(4, 4 if n != "m6" else -11), fontsize=8, color=cols[n])
    # DRR and proximity: two stacked panels
    sub = gs[0, 8:12].subgridspec(2, 1, hspace=0.55, height_ratios=[1.25, 1])
    ab = fig.add_subplot(sub[0])
    ac = fig.add_subplot(sub[1])
    x = np.arange(6)
    drr = [float(R[p[0] + "_drr"]) for p in POS]
    pr = [float(R[p[0] + "_prox"]) for p in POS]
    iso = [float(R[p[0] + "_iso"]) for p in POS]
    xt = ["%s\n%.2f m" % (p[1], float(R[p[0] + "_r"])) for p in POS]
    ab.bar(x, drr, 0.6, color=[cols[p[0]] for p in POS])
    for i in range(6):
        ab.text(i, drr[i] + 0.3, "%.1f" % drr[i], ha="center", fontsize=8)
    ab.set_xticks(x); ab.set_xticklabels(xt, fontsize=8)
    ab.set_ylabel("DRR, dB", fontsize=9); ab.set_ylim(0, 16)
    ab.set_title("direct-to-reverberant ratio (from the IR, proximity off)", fontsize=10)
    ac.bar(x - 0.2, pr, 0.4, color="0.75", edgecolor="0.3", hatch="///", label="vs M4 (as the brief)")
    ac.bar(x + 0.2, iso, 0.4, color=[cols[p[0]] for p in POS], label="proximity effect alone (with minus without)")
    for i in range(6):
        ac.text(i - 0.2, pr[i] + (0.15 if pr[i] >= 0 else -0.55), "%+.1f" % pr[i], ha="center", fontsize=7)
        ac.text(i + 0.2, iso[i] + 0.15, "%+.1f" % iso[i], ha="center", fontsize=7)
    ac.axhline(0, color="k", lw=0.8)
    ac.set_xticks(x); ac.set_xticklabels(xt, fontsize=8)
    ac.set_ylabel("dB", fontsize=9); ac.set_ylim(-4.5, 6.5)
    ac.set_title("energy < 200 Hz re 200-2000 Hz, Bach render", fontsize=10)
    ac.legend(fontsize=6.5, loc="upper right", ncol=1)
    # LTAS
    al = fig.add_subplot(gs[1, 0:7])
    fc = np.array(BANDS)
    tgt = R["ref_ltas3"].mean(axis=0)
    al.plot(fc, tgt, "k", lw=3.2, label="TARGET: Philharmonia (mean of ph_cello_43, 50, 57)", zorder=6)
    for n, lab, pos, pat, d in POS:
        l = R[n + "_ltas3"].mean(axis=0)
        al.plot(fc, l, color=cols[n], lw=2.2 if n in ("m1", "m3", "m4") else 1.2, ls="-" if n in ("m1", "m3", "m4") else "--", label=lab + " " + pat)
    al.set_xscale("log"); al.set_xlim(50, 10500); al.set_ylim(-75, 24)
    al.set_xticks([63, 125, 250, 500, 1000, 2000, 4000, 8000]); al.set_xticklabels(["63", "125", "250", "500", "1k", "2k", "4k", "8k"])
    al.set_xlabel("third-octave centre frequency, Hz"); al.set_ylabel("dB re own peak band")
    al.set_title("sustained LTAS, mean of G2 (on C), D3, A3: candidate 5 through each position", fontsize=10)
    al.grid(alpha=0.3, which="both"); al.legend(fontsize=8, ncol=3, loc="upper center")
    for lo, hi in RANGES:
        al.axvspan(lo, min(hi, 10000), color="0.5", alpha=0.05)
    # gap bars
    ag = fig.add_subplot(gs[1, 7:12])
    w = 0.13
    for i, (n, lab, pos, pat, d) in enumerate(POS):
        g = R[n + "_gap3"].mean(axis=0)
        ag.bar(np.arange(4) + (i - 2.5) * w, g, w, color=cols[n], label=lab)
    gd = R["dry_gap3"].mean(axis=0)
    ag.plot(np.arange(4), gd, "k_", ms=26, mew=2, label="dry (no mic)")
    ag.axhline(0, color="k", lw=0.8)
    ag.set_xticks(range(4)); ag.set_xticklabels([l + " Hz" for l in RLAB], fontsize=9)
    ag.set_ylabel("gap, ours - Philharmonia, dB (mean of 3 notes)")
    ag.set_title("gap per range (0 = same shape as the real notes)", fontsize=10)
    ag.set_ylim(-14, 12.5); ag.legend(fontsize=7, ncol=4, loc="upper left"); ag.grid(alpha=0.3, axis="y")
    # metrics table
    at = fig.add_subplot(gs[2, :])
    at.axis("off")
    hdr = "%-4s %-9s %6s %8s %12s %9s %12s %14s %29s %9s %9s" % ("pos", "pattern", "r m", "DRR dB", "<200Hz vs M4", "prox only", "noise/sig dB", "noise/sig dB(A)", "gaps 200-500/500-1.6k/1.6-5k/5-10k", "mean|gap|", "low notes")
    rows = [hdr]
    for n, lab, pos, pat, d in POS:
        g = R[n + "_gap3"].mean(axis=0)
        rows.append("%-4s %-9s %6.2f %8.2f %+11.2f %+9.2f %12.1f %14.1f %29s %9.2f %9.2f" % (lab, pat, float(R[n + "_r"]), float(R[n + "_drr"]), float(R[n + "_prox"]), float(R[n + "_iso"]), float(R[n + "_snr"]), float(R[n + "_snra"]),
                                                                                " ".join("%+6.1f" % v for v in g), np.mean(np.abs(g)), np.mean(np.abs(R[n + "_gap2"].mean(axis=0)))))
    rows.append("dry %-9s %6s %8s %12s %9s %12s %14s %29s %9.2f" % ("(none)", "-", "-", "-", "-", "-", "-", " ".join("%+6.1f" % v for v in gd), np.mean(np.abs(gd))))
    rows.append("")
    rows.append("Point-source limit: the radiation is ONE pressure signal at the f-hole/bridge point. A cello is 0.75 m long, so a real mic at 0.3-0.5 m is in the near field of the body: the variation over the instrument")
    rows.append("(f-hole vs bridge vs top plate) is NOT modelled and needs the planned FDTD or a multi-point source. Modelled: 1/r spreading, DRR, cardioid proximity (gradient term), arrival angle. 0.3 m is an approximation.")
    at.text(0.0, 1.0, "\n".join(rows), family="monospace", fontsize=8.6, va="top")
    p = os.path.join(CH, "mic_iter1.png")
    fig.savefig(p, dpi=100)
    print(p)


if __name__ == "__main__":
    {"post": post, "measure": measure, "spectro": spectro, "chart": chart}.get(sys.argv[1], lambda: notes(sys.argv[2]))()

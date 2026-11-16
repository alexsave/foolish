"""Post-processing of the music1 renders: level match to -20 LUFS into audio/music1/, note-axis spectrograms (C1-C9, 80 dB), per-note pitch error,
transition arrival, hum of the previous note, level dip and noise burst at the bow changes, top-edge measure.
   python -I charts/music_post.py level      # copy the raw renders to audio/music1/NN_name.wav at -20 LUFS
   python -I charts/music_post.py chart      # spectrograms
   python -I charts/music_post.py measure    # tables to stdout and audio/music1/../charts/music1_measure.txt"""
import os, sys, subprocess, re
import numpy as np
import scipy.io.wavfile as sw
import scipy.signal as ss
sys.path.insert(0, "/Users/alex/Music/foolish_cities_v5/shared_tools/spectro")
RAW = "/private/tmp/claude-501/m1/raw"
OUT = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/music1"
CH = "/Users/alex/Music/foolish_cities_v5/alt_cello/charts"
SC = "/Users/alex/Music/foolish_cities_v5/alt_cello/scores/"
TOOL = "/private/tmp/claude-501/m1/onset_tool"
SR = 48000
# number, raw name, score, kind
FILES = [
    ("bach_s0_off", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 0, dry"),
    ("bach_s0_0.6", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 0, room_0p6"),
    ("bach_s1_off", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 1, dry"),
    ("bach_s1_0.6", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 1, room_0p6"),
    ("bach_s2_off", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 2, dry"),
    ("bach_s2_0.6", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 2, room_0p6"),
    ("air_s0_off", "bach_air_g_major.txt", "Bach Air (from memory, G major, low-medium confidence), seed 0, dry"),
    ("air_s0_0.6", "bach_air_g_major.txt", "Bach Air (from memory, G major, low-medium confidence), seed 0, room_0p6"),
    ("bachfe_s0_off", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, SOLVED FE-B body (hf_body.bodymodes.txt, 900 modes to 5 kHz, knob fe=2), seed 0, dry"),
    ("bachfe_s0_0.6", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, SOLVED FE-B body (hf_body.bodymodes.txt), seed 0, room_0p6"),
    ("bachslur_s0_off", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 0, dry, --slur 1 (bow never reverses at the legato changes), diagnostic"),
    ("bachslur_s0_0.6", "bach_bwv1007_bars1_4.txt", "Bach prelude bars 1-4, statistical body, seed 0, room_0p6, --slur 1, diagnostic"),
]
NAMES = {n: "%02d_%s" % (i + 1, n.replace("_off", "_dry").replace("_0.6", "_room0p6")) for i, (n, _, _) in enumerate(FILES)}


def lufs(path):
    r = subprocess.run(["ffmpeg", "-nostats", "-i", path, "-af", "ebur128", "-f", "null", "-"], capture_output=True, text=True)
    return float(re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)[-1])


def load(path):
    sr, y = sw.read(path)
    assert sr == SR
    return y.astype(np.float64)


def level():
    os.makedirs(OUT, exist_ok=True)
    key = ["ALT-CELLO music1 (for the lead; the dispatcher blinds the order). 48 kHz mono float32, integrated loudness matched to -20 LUFS (ffmpeg ebur128, one gain per file).",
           "EVERY render is the full cello model: four strings with thermal bow friction, the expert player (style expert, seeds as named), the body, and for room_0p6 the image-source room.",
           "The default body is the statistical modal body; the solved FE-B body (files 09, 10) is only an optional swap of the body part (knob fe=2).",
           "Scores: scores/bach_bwv1007_bars1_4.txt, scores/bach_air_g_major.txt (Air written from memory, low to medium confidence in the notes). Binary: current default of the alt-cello worktree, copied before the run.", ""]
    for n, sc, desc in FILES:
        y = load(os.path.join(RAW, n + ".wav"))
        p = os.path.join(OUT, NAMES[n] + ".wav")
        sw.write(p, SR, y.astype(np.float32))
        g = 10 ** ((-20.0 - lufs(p)) / 20)
        sw.write(p, SR, (y * g).astype(np.float32))
        key.append("%s.wav  %s  (%.2f LUFS, peak %.3f)" % (NAMES[n], desc, lufs(p), np.max(np.abs(y * g))))
    open(os.path.join(OUT, "KEY.txt"), "w").write("\n".join(key) + "\n")
    print("\n".join(key))


def chart():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import librosa
    import notespec as N
    fmin = librosa.note_to_hz("C1")
    def note_ticks(fmin=fmin, octaves=8, bpo=N.BPO):  # notespec labels assume C2 at the bottom; here the bottom is C1
        return [(i * bpo / 12.0, N.NOTE_NAMES[i % 12] + str(1 + i // 12), i % 12 == 0) for i in range(octaves * 12 + 1)]
    N.note_ticks = note_ticks
    for n, sc, desc in FILES:
        y = load(os.path.join(OUT, NAMES[n] + ".wav"))
        fig, ax = plt.subplots(figsize=(18, 7))
        N.show_note_spectrogram(ax, y, SR, title="%s: %s" % (NAMES[n], desc[:90]), vrange=80, fmin=fmin, octaves=8)
        ax.set_xlabel("s")
        fig.tight_layout()
        p = os.path.join(CH, "music1_%s.png" % NAMES[n])
        fig.savefig(p, dpi=70)
        plt.close(fig)
        print(p, flush=True)


def hz(m): return 440.0 * 2 ** ((m - 69) / 12.0)


def score_notes(sc):
    out = []
    for l in open(SC + sc):
        l = l.split("#")[0].split()
        if len(l) >= 4: out.append((float(l[0]), float(l[1]), int(l[2])))
    return out


def ptrack(y, flo, fhi, win, t0, t1):
    p = "/private/tmp/claude-501/m1/pt.f32"
    y.astype(np.float32).tofile(p)
    r = subprocess.run(["nice", "-n", "15", TOOL, "pitch", p, str(SR), "%.2f" % flo, "%.2f" % fhi, "%.5f" % win, "%.4f" % max(t0, 0.0), "%.4f" % t1, "0.0005"], capture_output=True, text=True, check=True).stdout
    a = np.array([[float(x) for x in l.split()[:2]] for l in r.strip().split("\n")])
    return a[:, 0], a[:, 1]


def measure_one(name, sc, vib_ok=False):
    y = load(os.path.join(RAW, name + ".wav"))
    notes = score_notes(sc)
    # dry render only for the physics checks; the same y at the raw level (raw is peak normalised; ratios are level independent)
    e_t = np.arange(0, len(y) - 480, 240) / SR
    hf = ss.sosfilt(ss.butter(4, 5000, "hp", fs=SR, output="sos"), y)
    rms = lambda x: np.sqrt(np.convolve(x * x, np.ones(480) / 480, "same"))[240::240][: len(e_t)]
    r_all, r_hf = rms(y), rms(hf)
    db = lambda v: 20 * np.log10(np.maximum(v, 1e-12))
    rows = []
    for k, (ts, dur, m) in enumerate(notes):
        f0 = hz(m)
        t0, t1 = ts + 0.2 * dur, ts + 0.8 * dur  # middle 60 percent
        tt, ff = ptrack(y, 0.8 * f0, 1.25 * f0, 2.0 / f0 if f0 > 90 else 2.5 / f0, ts - 0.12, min(ts + dur + 0.05, len(y) / SR - 0.05))
        sel = (tt >= t0) & (tt <= t1) & (ff > 0)
        cents = 1200 * np.log2(np.where(ff > 0, ff, 1) / f0)
        err = float(np.median(cents[sel])) if sel.any() else np.nan
        # arrival: first time from ts - 0.05 within 10 cents of nominal for 5 periods
        need = int(np.ceil(5.0 / f0 / 0.0005))
        run, arr = 0, np.nan
        for i in range(len(tt)):
            if tt[i] < ts - 0.05: continue
            run = run + 1 if (ff[i] > 0 and abs(cents[i]) < 10) else 0
            if run >= need: arr = 1000 * (tt[i - need + 1] - ts); break
        # hum of the previous note: partials of the previous pitch not coinciding with this note's partials, 60 ms Hann window 0.1 s in
        hum = np.nan
        if k > 0:
            fp = hz(notes[k - 1][2])
            a = int((ts + 0.07) * SR); n = int(0.06 * SR) * 2
            w = y[a:a + n] * np.hanning(n)
            S = np.abs(np.fft.rfft(w, 8 * n)) ** 2
            fr = np.fft.rfftfreq(8 * n, 1 / SR)
            def pw(freqs):
                return sum(S[(fr > f * 0.985) & (fr < f * 1.015)].sum() for f in freqs)
            cur = [f0 * h for h in range(1, 11)]
            prev = [fp * h for h in range(1, 11) if min(abs(fp * h / c - 1) for c in cur) > 0.04]
            if prev and fp != f0: hum = 10 * np.log10(pw(prev) / max(pw(cur), 1e-30))
        # bow change dip and noise burst at the written start
        dip = burst = np.nan
        if k > 0:
            def seg(x, a, b): return x[int(a * 100):int(b * 100) + 1]
            lvl = lambda x, a, b: 10 * np.log10(np.mean(seg(x, a, b) ** 2) + 1e-20)
            ref = min(lvl(r_all, ts - 0.09, ts - 0.04), lvl(r_all, ts + 0.06, ts + 0.12))
            dip = 10 * np.log10(np.min(seg(r_all, ts - 0.03, ts + 0.05) ** 2) + 1e-20) - ref
            burst = 10 * np.log10(np.max(seg(r_hf, ts - 0.03, ts + 0.05) ** 2) + 1e-20) - 10 * np.log10(np.median(r_hf[(e_t > ts) & (e_t < ts + dur)] ** 2) + 1e-20)
        rows.append((k, m, ts, err, arr, hum, dip, burst))
    # long-term spectrum for the top
    f, P = ss.welch(y, SR, nperseg=8192)
    Pd = 10 * np.log10(P + 1e-30)
    def band(lo, hi): return 10 * np.log10(np.mean(P[(f >= lo) & (f < hi)]) + 1e-30)
    edges = [(1000, 2000), (2000, 3000), (3000, 4186), (4186, 5000), (5000, 6000), (6000, 8000), (8000, 10000), (10000, 13000), (13000, 16000), (16000, 20000)]
    top = [(lo, hi, band(lo, hi)) for lo, hi in edges]
    # steepest third-octave step above 3 kHz
    cs = np.array([3000 * 2 ** (i / 3) for i in range(0, 9)])
    lv = [band(c / 2 ** (1 / 6), c * 2 ** (1 / 6)) for c in cs]
    steps = [(cs[i], lv[i + 1] - lv[i]) for i in range(len(lv) - 1)]
    return rows, top, steps


NN = lambda m: ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][m % 12] + str(m // 12 - 1)


def measure():
    out = []
    P = lambda s="": (out.append(s), print(s, flush=True))
    allrows = {}
    for n, sc, desc in FILES:
        if n.endswith("0.6"): continue
        rows, top, steps = measure_one(n, sc)
        allrows[n] = rows
        P("=== %s : %s" % (NAMES[n], desc))
        P("%3s %-4s %7s %8s %9s %8s %8s %8s" % ("k", "note", "start", "err_c", "arrive_ms", "hum_dB", "dip_dB", "burst_dB"))
        for k, m, ts, err, arr, hum, dip, burst in rows:
            P("%3d %-4s %7.3f %8.1f %9.1f %8.1f %8.1f %8.1f" % (k, NN(m), ts, err, arr, hum, dip, burst))
        P("LTAS dB per Hz band: " + "  ".join("%d-%d: %.1f" % (lo, hi, v) for lo, hi, v in top))
        P("third-octave steps above 3 kHz: " + "  ".join("%.0f Hz: %+.1f" % (c, s) for c, s in steps))
        P()
    P("=== SUMMARY by pitch (median over notes of the same pitch; err = median cents; arrival ms (nan = never within 10 c); hum dB; dip dB; burst dB)")
    for n, rows in allrows.items():
        P("-- " + NAMES[n])
        for m in sorted(set(r[1] for r in rows)):
            rr = [r for r in rows if r[1] == m]
            f = lambda i: np.nanmedian([r[i] for r in rr]) if not all(np.isnan(r[i]) for r in rr) else np.nan
            P("   %-4s n=%2d err %6.1f  arrive %7.1f (nan %d)  hum %6.1f  dip %6.1f  burst %6.1f" % (NN(m), len(rr), f(3), f(4), sum(np.isnan(r[4]) for r in rr), f(5), f(6), f(7)))
    open(os.path.join(CH, "music1_measure.txt"), "w").write("\n".join(out) + "\n")




def outliers():
    """per render: notes whose tracked pitch is more than 50 cents from the written one (the previous string's ring or a wrong dominant pitch), and the mid-note level by pitch.
    Reads the measure output table (music1_measure.txt) and recomputes the levels from the raw renders."""
    cur, tab = None, {}
    for l in open(os.path.join(CH, "music1_measure.txt")):
        if l.startswith("=== ") and "SUMMARY" not in l: cur = l.split()[1]; tab[cur] = []
        elif cur and l[:3].strip().isdigit():
            p = l.split(); tab[cur].append((int(p[0]), p[1], float(p[2]), float(p[3])))
        elif l.startswith("=== SUMMARY"): cur = None
    for n, rows in tab.items():
        bad = [(k, nm, t, e) for k, nm, t, e in rows if abs(e) > 50]
        print("%s: %d of %d notes more than 50 cents off: %s" % (n, len(bad), len(rows), " ".join("%s@%.2f(%+.0f)" % (nm, t, e) for k, nm, t, e in bad)))
    for n, sc, desc in FILES:
        if n.endswith("0.6") or "air" in n: continue
        y = load(os.path.join(RAW, n + ".wav"))
        lv = {}
        for ts, dur, m in score_notes(sc):
            a, b = int((ts + 0.2 * dur) * SR), int((ts + 0.8 * dur) * SR)
            lv.setdefault(m, []).append(10 * np.log10(np.mean(y[a:b] ** 2) + 1e-20))
        print(n, "mid-note level dB by pitch:", " ".join("%s %.1f" % (NN(m), np.median(v)) for m, v in sorted(lv.items())))


if __name__ == "__main__":
    {"level": level, "chart": chart, "measure": measure, "outliers": outliers}[sys.argv[1]]()

"""ALT-CELLO stage 3a: pick the room (RT60 and mic distance) whose release curve matches the Philharmonia cello notes.

The release curve of a note: the level, re the sustain, at 20, 50, 100, 150 and 200 ms after the bow lifts.
  envelope: 25 ms RMS windows every 5 ms, dB.
  Philharmonia (wav_full, 30 notes): the lift is the last frame (after 0.3 s, before the last 0.15 s) whose level is within 1 dB of
  the maximum of the 150 ms before it; the sustain is the median level of the 200 ms before the lift.
  ALT-CELLO: the bow is lifted at 2.0 s by construction (probe --vb-env/--fb-env, force and speed ramped to zero in 20 ms); the sustain
  is the median level of the 200 ms before 2.0 s (plus the direct sound's delay when a room is applied).
The room that fits best minimises the RMS difference, over 50, 100, 150 and 200 ms, between the ALT-CELLO median curve (3 notes G2 D3 A3
open) and the Philharmonia median curve (30 notes).

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/room_match.py [--reuse]
Writes out/room_match/*.wav (dry and wet), out/room_match/match.tsv, and the listening copies in
/Users/alex/Music/foolish_cities_v5/alt_cello/audio/room/.
"""
import os, subprocess, sys
import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "altcello")
OUTD = os.path.join(ROOT, "out", "room_match")
AUDIO = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/room"
PHIL = "/Users/alex/Dev/soundtrack-samples/philharmonia/wav_full/"
TIMES = [0.020, 0.050, 0.100, 0.150, 0.200]
FIT = [0.050, 0.100, 0.150, 0.200]
RT60S = [0.4, 0.6, 0.8, 1.0]
DISTS = [1.0, 1.5, 2.5]
LIFT = 2.0
BETA, VB = 0.1, 0.1
# open strings and the bow force of stage 2c (charts/stage2c_notes.py)
NOTES = [("G2", 43, 0.67082), ("D3", 50, 0.67082), ("A3", 57, 0.49717)]


def load(path):
    sr, y = wavfile.read(path)
    if y.dtype.kind == "i":
        y = y / float(np.iinfo(y.dtype).max)
    y = y.astype(float)
    return (y.mean(1) if y.ndim > 1 else y), sr


def envelope(y, sr, win=0.025, hop=0.005):
    w, h = int(win * sr), int(hop * sr)
    n = (len(y) - w) // h
    p = np.array([np.mean(y[i * h:i * h + w] ** 2) for i in range(n)]) + 1e-20
    t = (np.arange(n) * h + w / 2) / sr
    return t, 10 * np.log10(p)


def find_lift(t, e):
    """last frame, after 0.3 s and before the last 0.15 s, within 1 dB of the maximum of the 150 ms before it"""
    k = 30
    best = None
    for i in range(len(t)):
        if t[i] < 0.3 or t[i] > t[-1] - 0.15 or i < k: continue
        if e[i] >= e[i - k:i + 1].max() - 1.0: best = i
    return t[best]


def curve(y, sr, lift, times):
    """level re sustain (dB) at lift + times; lift = None for the detector"""
    t, e = envelope(y, sr)
    if lift is None: lift = find_lift(t, e)
    sel = (t >= lift - 0.2) & (t <= lift)
    sus = np.median(e[sel])
    return lift, np.interp(lift + np.asarray(times), t, e) - sus


def probe(name, midi, F, prefix, wav):
    cmd = ["nice", EXE, "probe", name, str(BETA), str(VB), "%.5f" % F, "3.0", prefix, "--body", "on", "--strings", "all", "--wav", wav,
           "--vb-env", "0:0,0.03:%g,%g:%g,%g:0" % (VB, LIFT, VB, LIFT + 0.02),
           "--fb-env", "0:0,0.03:%.5f,%g:%.5f,%g:0" % (F, LIFT, F, LIFT + 0.02)]
    subprocess.run(cmd, capture_output=True, text=True, check=True)


def room(dry, wet, rt60, dist):
    r = subprocess.run(["nice", EXE, "room", dry, wet, "--rt60", str(rt60), "--dist", str(dist), "--pattern", "cardioid"],
                       capture_output=True, text=True, check=True)
    hdr = open(wet + "_ir.txt").readline().split()
    return float([h for h in hdr if h.startswith("direct_s=")][0].split("=")[1]), r.stdout


def phil_curves(times):
    rows = []
    for m in range(38, 68):
        y, sr = load(PHIL + "ph_cello_%d.wav" % m)
        lift, c = curve(y, sr, None, times)
        rows.append((m, lift, c))
    return rows


def main():
    reuse = "--reuse" in sys.argv
    os.makedirs(OUTD, exist_ok=True)
    dry = {}
    for name, midi, F in NOTES:
        w = os.path.join(OUTD, "%s_dry.wav" % name)
        if not (reuse and os.path.exists(w)): probe(name, midi, F, os.path.join(OUTD, "r_" + name), w)
        dry[name] = w
    ph = phil_curves(TIMES)
    pc = np.array([c for _, _, c in ph])
    pmed = np.median(pc, axis=0)
    print("Philharmonia (30 notes): release lift times %.2f-%.2f s" % (min(l for _, l, _ in ph), max(l for _, l, _ in ph)))
    print("  level re sustain at %s ms:" % ", ".join("%d" % (1000 * t) for t in TIMES))
    for nm, v in (("p10", np.percentile(pc, 10, axis=0)), ("median", pmed), ("p90", np.percentile(pc, 90, axis=0)),
                  ("min", pc.min(0)), ("max", pc.max(0))):
        print("  %-7s" % nm + " ".join("%7.1f" % x for x in v))
    rows = []
    cd = np.array([curve(load(dry[n])[0], 48000, LIFT, TIMES)[1] for n, _, _ in NOTES])
    rows.append(("dry", None, None, np.median(cd, axis=0), cd))
    for rt in RT60S:
        for d in DISTS:
            cs = []
            for n, _, _ in NOTES:
                wet = os.path.join(OUTD, "%s_rt%.1f_d%.1f.wav" % (n, rt, d))
                dl, _ = room(dry[n], wet, rt, d)
                y, sr = load(wet)
                cs.append(curve(y, sr, LIFT + dl, TIMES)[1])
            cs = np.array(cs)
            rows.append(("room", rt, d, np.median(cs, axis=0), cs))
    idx = [TIMES.index(t) for t in FIT]
    best = None
    with open(os.path.join(OUTD, "match.tsv"), "w") as f:
        f.write("# config rt60 dist " + " ".join("med_%dms" % (1000 * t) for t in TIMES) + " rms_diff_50_200ms\n")
        f.write("philharmonia 0 0 " + " ".join("%.2f" % x for x in pmed) + " 0\n")
        print("\n%-9s %5s %5s | %s | rms diff 50-200 ms vs Philharmonia median" % ("config", "RT60", "dist", " ".join("%6d" % (1000 * t) for t in TIMES)))
        print("%-9s %5s %5s | %s |" % ("TARGET", "", "", " ".join("%6.1f" % x for x in pmed)))
        for kind, rt, d, med, cs in rows:
            sc = float(np.sqrt(np.mean((med[idx] - pmed[idx]) ** 2)))
            nm = "dry" if kind == "dry" else "room"
            f.write("%s %s %s %s %.3f\n" % (nm, "0" if rt is None else "%.1f" % rt, "0" if d is None else "%.1f" % d,
                                            " ".join("%.2f" % x for x in med), sc))
            print("%-9s %5s %5s | %s | %5.2f" % (nm, "-" if rt is None else "%.1f" % rt, "-" if d is None else "%.1f" % d,
                                                " ".join("%6.1f" % x for x in med), sc))
            if kind == "room" and (best is None or sc < best[0]): best = (sc, rt, d)
    print("\nbest: RT60 %.1f s, distance %.1f m (rms difference %.2f dB)" % (best[1], best[2], best[0]))
    with open(os.path.join(OUTD, "best.txt"), "w") as f: f.write("%.1f %.1f %.3f\n" % (best[1], best[2], best[0]))
    # listening copies: dry and wet at the same sounding rms (-20 dBFS), for the lead only
    os.makedirs(AUDIO, exist_ok=True)
    for n, midi, _ in NOTES:
        wet = os.path.join(OUTD, "%s_best.wav" % n)
        room(dry[n], wet, best[1], best[2])
        yd, _ = load(dry[n]); yw, _ = load(wet)
        def act(y):
            fr = y[:len(y) // 960 * 960].reshape(-1, 960); p = (fr ** 2).mean(1)
            return np.sqrt(p[p >= 0.0625 * p.max()].mean())
        g = 0.1 / act(yd)
        gw = 0.1 / act(yw)
        wavfile.write(os.path.join(AUDIO, "%s_dry.wav" % n), 48000, (yd * g).astype(np.float32))
        wavfile.write(os.path.join(AUDIO, "%s_room.wav" % n), 48000, (yw * gw).astype(np.float32))
    print("wrote listening copies to", AUDIO)


if __name__ == "__main__":
    main()

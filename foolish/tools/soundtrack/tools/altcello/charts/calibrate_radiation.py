"""ALT-CELLO radiativity calibration (stage 2 iteration 2).

The radiated pressure is p(t) = sum_k r_k q_k'', r_k = R(f_k) (radiativity.h). R is a smooth curve
(piecewise linear in dB against log2 frequency through 12 control points) and is a stated CALIBRATION:
it is fitted so bowed notes across the range match the harmonic envelope of the TARGET (Philharmonia
solo cello, MIDI 38 to 67).

Procedure (one pass):
  a) render the 12 notes with `altcello probe ... --body on --strings all --finger XF --wav` (3 s each,
     bow force inside each note's Helmholtz band at beta 0.1, found once by a quick search),
  b) measure each wav with analysis/measure.py (one note per file, note window 0.5 s .. 2.5 s),
  c) compare the harmonic levels h2..h16 (dB re h1) with the Philharmonia sample of the same MIDI note,
  d) fit the 12 control points. Because every level is re h1, a sample at k f0 constrains the DIFFERENCE
     R(k f0) - R(f0) (the h1 level moves too), so the fit is a linear least squares in the control point
     changes with the basis difference B(k f0) - B(f0), robust (IRLS, L1 like the median of the
     brief), second differences penalised, the 200 Hz point anchored at 0 dB. The changes are added to
     the current curve, radiativity_table.c is regenerated, altcello rebuilt, and the next pass starts.
  Stops after 3 fits or when the median absolute harmonic error stops falling (the best table is kept).

Run from the altcello directory:
    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/calibrate_radiation.py [--passes 3]
Outputs (plain text) in out/calib/, final wavs in /Users/alex/Music/foolish_cities_v5/alt_cello/audio/stage2b/.
"""
import datetime, os, shutil, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "analysis"))
import measure as M  # noqa: E402

EXE = os.path.join(ROOT, "altcello")
CAL = os.path.join(ROOT, "out", "calib")
AUDIO = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/stage2b"
PHIL = "/Users/alex/Dev/soundtrack-samples/philharmonia/"
L = 0.69
OPEN = {"C2": 65.41, "G2": 98.00, "D3": 146.83, "A3": 220.00}
FS = 96000
BETA, VB = 0.1, 0.1
RAD_F = [80.0, 125.0, 200.0, 315.0, 500.0, 800.0, 1250.0, 2000.0, 3150.0, 5000.0, 8000.0, 12500.0]
ANCHOR = RAD_F.index(200.0)
# (string, MIDI): open strings and stopped notes spanning the Philharmonia range 38..67 (C2 = 36 has no sample)
NOTES = [("C2", 36), ("C2", 38), ("C2", 40), ("G2", 43), ("G2", 45), ("G2", 47), ("D3", 50), ("D3", 53),
         ("A3", 57), ("A3", 60), ("A3", 64), ("A3", 67)]
BANDS = [(100, 300), (300, 1000), (1000, 3000), (3000, 8000)]
LAMBDA = 3.0


def hz(m): return 440.0 * 2 ** ((m - 69) / 12.0)


def finger_pos(string, midi):
    f = hz(midi)
    return None if abs(f / OPEN[string] - 1.0) < 0.02 else L * (1.0 - OPEN[string] / f)


def probe(string, F, secs, prefix, midi, wav=None):
    cmd = ["nice", EXE, "probe", string, str(BETA), str(VB), "%.5f" % F, str(secs), prefix, "--body", "on", "--strings", "all"]
    xf = finger_pos(string, midi)
    if xf is not None: cmd += ["--finger", "%.6f" % xf]
    if wav: cmd += ["--wav", wav]
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return r.stdout


def classify(prefix, f_note):
    d = np.fromfile(prefix + ".f32", dtype="<f4").reshape(-1, 8).astype(float)
    n = len(d); i0 = n - FS
    br, vr = d[i0:, 0], d[i0:, 3]
    seg = br * np.hanning(len(br)); X = np.abs(np.fft.rfft(seg, 1 << 19)); fr = np.fft.rfftfreq(1 << 19, 1 / FS)
    m = (fr > f_note * 0.95) & (fr < f_note * 1.05)
    f = fr[m][np.argmax(X[m])]; P = FS / f
    sl = np.abs(vr) > 1e-4
    on = np.where(sl[1:] & ~sl[:-1])[0] + 1; off = np.where(~sl[1:] & sl[:-1])[0] + 1
    off = off[off > (on[0] if len(on) else 0)]
    k = min(len(on), len(off))
    dur = off[:k] - on[:k]
    keep = dur >= int(0.00015 * FS)
    ratio = keep.sum() / (len(br) / P)
    frac = dur[keep].sum() / len(br)
    lag = int(round(P)); x = br - br.mean()
    corr = max(np.dot(x[:-l], x[l:]) / np.sqrt(np.dot(x[:-l], x[:-l]) * np.dot(x[l:], x[l:])) for l in range(lag - 3, lag + 4))
    beta_eff = BETA * L / (L - (finger_pos_cache.get(f_note, 0.0)))
    ok = 0.93 <= ratio <= 1.07 and corr > 0.9 and abs(frac - beta_eff) < 0.05
    return ok, ratio, frac, corr, f


finger_pos_cache = {}


def search_forces():
    path = os.path.join(CAL, "forces.txt")
    if os.path.exists(path):
        return {int(l.split()[0]): float(l.split()[1]) for l in open(path) if not l.startswith("#")}
    forces = {}
    cand = 0.15 * (3.0 / 0.15) ** (np.arange(11) / 10.0)
    with open(path, "w") as out:
        out.write("# midi force_N  (middle of the longest run of Helmholtz forces at beta 0.1, 3 s, body on, all strings)\n")
        for s, midi in NOTES:
            xf = finger_pos(s, midi); finger_pos_cache[hz(midi)] = xf or 0.0
            res = []
            for F in cand:
                ok, ratio, frac, corr, f = classify_run(s, midi, F)
                res.append(ok)
                print("search %s midi %d F %.3f: onsets/period %.2f slip %.3f corr %.3f f %.2f %s" % (s, midi, F, ratio, frac, corr, f, "OK" if ok else ""), flush=True)
            runs, cur = [], []
            for i, ok in enumerate(res):
                if ok: cur.append(i)
                else:
                    if cur: runs.append(cur)
                    cur = []
            if cur: runs.append(cur)
            if not runs:
                raise SystemExit("no Helmholtz force for %s midi %d" % (s, midi))
            best = max(runs, key=len)
            F = float(cand[best[len(best) // 2]])
            forces[midi] = F
            out.write("%d %.5f\n" % (midi, F)); out.flush()
    return forces


def classify_run(s, midi, F):
    pre = os.path.join(CAL, "search_%d" % midi)
    probe(s, F, 3.0, pre, midi)
    return classify(pre, hz(midi))


def render_all(forces, tag):
    wavs = {}
    for s, midi in NOTES:
        w = os.path.join(CAL, "%s_%d_%s.wav" % (tag, midi, s))
        probe(s, forces[midi], 3.0, os.path.join(CAL, "r_%d" % midi), midi, wav=w)
        wavs[midi] = w
    return wavs


def measure_alt(wavs):
    rows, extras, notes_for_ltas = {}, [], []
    ys = []
    for s, midi in NOTES:
        y = M.load_audio(wavs[midi])
        res = M.analyze_signal(y, M.SR, [(0.5, 2.0, midi)], "alt_%d" % midi)
        rows[midi] = res["rows"][0]
        ys.append((midi, y, res["extras"][0]))
    return rows, ys


def phil_rows(midis):
    rows, extras = [], []
    for midi in midis:
        y = M.load_audio(PHIL + "wav/ph_cello_%d.wav" % midi)
        yf = M.load_audio(PHIL + "wav_full/ph_cello_%d.wav" % midi)
        env = M.rms_env(y); on = int(np.argmax(env >= 0.1 * env.max())) * 0.01
        t0 = max(0.0, on - 0.05); floor = float(np.percentile(M.rms_env(yf), 10))
        r, ex = M.analyze_note(y, M.SR, t0, len(y) / M.SR - t0, midi, floor, None, edge_silent=False)
        r["note"] = len(rows); rows.append(r); extras.append(ex)
    res = M.build_result("phil", rows, extras, M.SR)
    return dict(zip(midis, rows)), res["ltas"]


def ltas_of(extras):
    ps = []
    for e in extras:
        b = e["body"]; ps.append(M.welch_pow(b - b.mean(), M.SR, 16384))
    return M.ltas_curve(ps)


def band_diff(alt, ph):
    out = []
    for lo, hi in BANDS:
        sel = [i for i, f in enumerate(M.LTAS_F) if lo <= f < hi]
        out.append(float(np.mean(alt[sel] - ph[sel])))
    return out


def basis(f):
    """weights of the 12 control points for R(f) (piecewise linear in dB against log2 f, held at the ends)"""
    x = np.log2(np.array(RAD_F)); b = np.zeros(len(RAD_F))
    lf = np.log2(f)
    if lf <= x[0]: b[0] = 1; return b
    if lf >= x[-1]: b[-1] = 1; return b
    j = int(np.searchsorted(x, lf) - 1); u = (lf - x[j]) / (x[j + 1] - x[j]); b[j] = 1 - u; b[j + 1] = u
    return b


def samples(alt_rows, ph_rows, midis):
    """(midi, k, f0, target, model) for harmonics 2..16, re h1"""
    out = []
    for midi in midis:
        a, p = alt_rows[midi], ph_rows[midi]
        f0 = a["f0_hz"]
        for k in range(2, 17):
            t, m = p.get("h%d" % k, np.nan), a.get("h%d" % k, np.nan)
            if np.isnan(t) or np.isnan(m) or not (RAD_F[0] <= k * f0 <= 20000): continue
            out.append((midi, k, f0, t, m))
    return out


def median_abs_err(smp):
    return float(np.median([abs(t - m) for _, _, _, t, m in smp]))


def fit(smp, lam=LAMBDA):
    n = len(RAD_F)
    A = np.array([basis(k * f0) - basis(f0) for _, k, f0, _, _ in smp]); d = np.array([t - m for _, _, _, t, m in smp])
    D2 = np.zeros((n - 2, n))
    for i in range(n - 2): D2[i, i:i + 3] = [1, -2, 1]
    # weak first-difference ridge on the two highest intervals: the harmonics 1..16 do not reach 8 and 12.5 kHz
    D1 = np.zeros((2, n)); D1[0, n - 3:n - 1] = [-1, 1]; D1[1, n - 2:] = [-1, 1]
    keep = [j for j in range(n) if j != ANCHOR]
    w = np.ones(len(d))
    c = np.zeros(n)
    for _ in range(40):
        W = np.sqrt(w)[:, None]
        Ms = np.vstack([A[:, keep] * W, np.sqrt(lam) * D2[:, keep], np.sqrt(0.5 * lam) * D1[:, keep]])
        rhs = np.concatenate([d * np.sqrt(w), np.zeros(n - 2), np.zeros(2)])
        sol, *_ = np.linalg.lstsq(Ms, rhs, rcond=None)
        c = np.zeros(n); c[keep] = sol
        r = d - A @ c
        w = 1.0 / np.maximum(np.abs(r), 0.5)
    return c


MAXSTEP = 11.0  # dB between neighbouring control points (test_radiation allows 12): the harmonics 1..16 cannot see below the mass line of the low modes, so the fit would keep pushing the top points down for ever


def cap_steps(db):
    db = db.copy()
    for j in range(ANCHOR + 1, len(db)): db[j] = min(max(db[j], db[j - 1] - MAXSTEP), db[j - 1] + MAXSTEP)
    for j in range(ANCHOR - 1, -1, -1): db[j] = min(max(db[j], db[j + 1] - MAXSTEP), db[j + 1] + MAXSTEP)
    return db


def write_table(db, pass_no, errtxt, notes_phil):
    path = os.path.join(ROOT, "radiativity_table.c")
    with open(path, "w") as f:
        f.write("/* GENERATED by charts/calibrate_radiation.py on %s. Do not edit.\n" % datetime.date.today().isoformat())
        f.write(" * Radiativity curve R(f) of the body pressure (radiativity.h), a calibration, not first-principles physics.\n")
        f.write(" * Fitted to the TARGET: Philharmonia solo cello (forte arco, MIDI %s), the harmonic levels h2..h16 re h1\n" % ",".join(str(m) for m in notes_phil))
        f.write(" * of the 12 bowed ALT-CELLO notes (open C2, MIDI 36, has no sample and is not fitted); calibration pass %d, median absolute harmonic error %s. */\n" % (pass_no, errtxt))
        f.write('#include "radiativity.h"\n\n')
        f.write("const double radiativity_f[RAD_NPTS] = {%s};\n" % ", ".join("%.1f" % x for x in RAD_F))
        f.write("const double radiativity_db[RAD_NPTS] = {%s};\n" % ", ".join("%.3f" % x for x in db))


def current_table():
    import re
    s = open(os.path.join(ROOT, "radiativity_table.c")).read()
    m = re.search(r"radiativity_db\[RAD_NPTS\] = \{([^}]*)\}", s)
    return np.array([float(x) for x in m.group(1).split(",")])


def rebuild():
    subprocess.run(["nice", "make", "altcello"], cwd=ROOT, check=True, capture_output=True)


def main():
    passes = 3
    if "--passes" in sys.argv: passes = int(sys.argv[sys.argv.index("--passes") + 1])
    os.makedirs(CAL, exist_ok=True); os.makedirs(AUDIO, exist_ok=True)
    for s, midi in NOTES: finger_pos_cache[hz(midi)] = finger_pos(s, midi) or 0.0
    forces = search_forces()
    midis = [m for _, m in NOTES if os.path.exists(PHIL + "wav/ph_cello_%d.wav" % m)]
    ph, ph_ltas = phil_rows(midis)
    np.savetxt(os.path.join(CAL, "phil_ltas.txt"), np.column_stack([M.LTAS_F, ph_ltas]), fmt="%.4f")
    log = open(os.path.join(CAL, "passes.txt"), "w")
    best = None
    db = current_table()
    for p in range(passes + 1):
        wavs = render_all(forces, "pass%d" % p)
        alt, ys = measure_alt(wavs)
        smp = samples(alt, ph, midis)
        err = median_abs_err(smp)
        ltas12 = ltas_of([e for _, _, e in ys])
        ltas11 = ltas_of([e for m, _, e in ys if m in midis])
        bd = band_diff(ltas11, ph_ltas)
        print("PASS %d: median |harmonic error| %.2f dB over %d samples (%d notes, h2..h16); LTAS diff alt-phil in %s: %s; R: %s" % (
            p, err, len(smp), len(midis), BANDS, " ".join("%+.1f" % x for x in bd), " ".join("%.1f" % x for x in db)), flush=True)
        log.write("PASS %d err %.3f bands %s R %s\n" % (p, err, " ".join("%.2f" % x for x in bd), " ".join("%.3f" % x for x in db))); log.flush()
        np.savetxt(os.path.join(CAL, "pass%d_samples.txt" % p), np.array([(m, k, f0, t, mm) for m, k, f0, t, mm in smp]), fmt="%.4f", header="midi k f0 target_dB model_dB")
        np.savetxt(os.path.join(CAL, "pass%d_ltas.txt" % p), np.column_stack([M.LTAS_F, ltas12]), fmt="%.4f")
        if best is None or err < best[0]:
            best = (err, p, db.copy(), {m: wavs[m] for m in wavs})
        elif p > 0:
            print("error stopped falling at pass %d (%.2f >= %.2f): keeping pass %d" % (p, err, best[0], best[1]))
            break
        if p == passes: break
        c = fit(smp)
        db = cap_steps(db + c); db -= db[ANCHOR]
        write_table(db, p + 1, "of the previous table %.2f dB (this one not measured yet)" % err, midis)
        rebuild()
    err, p, dbb, wavs = best
    write_table(dbb, p, "measured with this table %.2f dB" % err, midis)
    rebuild()
    # final wavs from the best table
    wavs = render_all(forces, "final")
    for s, midi in NOTES:
        shutil.copy(wavs[midi], os.path.join(AUDIO, "stage2b_%d_%s.wav" % (midi, s)))
    print("FINAL: pass %d, error %.2f dB, R = %s" % (p, err, " ".join("%.2f" % x for x in dbb)))
    log.write("FINAL pass %d err %.3f R %s\n" % (p, err, " ".join("%.3f" % x for x in dbb))); log.close()


if __name__ == "__main__":
    main()

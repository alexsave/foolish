"""ALT-CELLO stage 2 iteration 3: the 12 calibration notes, rendered and measured against the TARGET.

The notes and the bow forces are those of the iteration 2 calibration (C2 open, D2, E2, G2 open, A2, B2, D3 open, F3, A3 open,
C4, E4, G4; beta 0.1, vB 0.1 m/s, forces below, 3 s each, body on, all strings). Each is rendered with `altcello probe ... --wav`,
measured with analysis/measure.py (note window 0.5 s .. 2.0 s) and compared with the Philharmonia solo cello sample of the same
MIDI note (TARGET): the median absolute harmonic error h2..h16 (dB re h1) and the LTAS difference in four bands.

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/stage2c_notes.py TAG [--audio DIR] [probe knob flags ...]
e.g. --hair 1e4 --eta0 0.012 --eta-exp 0.3 --fc 3000 --bridge-inertia-scale 1.0
Writes out/stage2c/TAG_samples.txt (midi k f0 target_dB model_dB), TAG_ltas.txt and, with --audio, the 12 wavs.
"""
import os, shutil, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "analysis"))
import measure as M  # noqa: E402

EXE = os.path.join(ROOT, "altcello")
OUTD = os.path.join(ROOT, "out", "stage2c")
AUDIO = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/stage2c"
PHIL = "/Users/alex/Dev/soundtrack-samples/philharmonia/"
L = 0.69
OPEN = {"C2": 65.41, "G2": 98.00, "D3": 146.83, "A3": 220.00}
FS = 96000
BETA, VB = 0.1, 0.1
NOTES = [("C2", 36), ("C2", 38), ("C2", 40), ("G2", 43), ("G2", 45), ("G2", 47), ("D3", 50), ("D3", 53),
         ("A3", 57), ("A3", 60), ("A3", 64), ("A3", 67)]
# bow force of each note (N): the iteration 2 calibration forces (middle of each note's Helmholtz range at beta 0.1)
FORCES = {36: 1.22127, 38: 1.22127, 40: 1.64784, 43: 0.67082, 45: 0.90513, 47: 0.36847, 50: 0.67082, 53: 0.49717,
          57: 0.49717, 60: 0.36847, 64: 0.49717, 67: 0.27308}
BANDS = [(100, 300), (300, 1000), (1000, 3000), (3000, 8000)]
IT2 = {"err": 8.56, "bands": [0.1, 2.9, 6.7, 17.8]}  # iteration 2 final, as reported


def hz(m): return 440.0 * 2 ** ((m - 69) / 12.0)


def finger_pos(string, midi):
    f = hz(midi)
    return None if abs(f / OPEN[string] - 1.0) < 0.02 else L * (1.0 - OPEN[string] / f)


def probe(string, midi, F, secs, prefix, wav=None, knobs=()):
    cmd = ["nice", EXE, "probe", string, str(BETA), str(VB), "%.5f" % F, str(secs), prefix, "--body", "on", "--strings", "all"]
    xf = finger_pos(string, midi)
    if xf is not None: cmd += ["--finger", "%.6f" % xf]
    if wav: cmd += ["--wav", wav]
    cmd += list(knobs)
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def render_all(tag, knobs=(), outdir=None):
    os.makedirs(OUTD, exist_ok=True)
    wavs, rtfs = {}, []
    for s, midi in NOTES:
        w = os.path.join(outdir or OUTD, "%s_%d_%s.wav" % (tag, midi, s)) if outdir else os.path.join(OUTD, "%s_%d_%s.wav" % (tag, midi, s))
        out = probe(s, midi, FORCES[midi], 3.0, os.path.join(OUTD, "r_%s_%d" % (tag, midi)), wav=w, knobs=knobs)
        wavs[midi] = w
        rtfs.append(float(out.split("real-time factor")[1].split()[0]))
    return wavs, rtfs


def measure_alt(wavs):
    rows, ys = {}, []
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


def samples(alt_rows, ph_rows, midis):
    """(midi, k, f0, target, model) for harmonics 2..16, re h1"""
    out = []
    for midi in midis:
        a, p = alt_rows[midi], ph_rows[midi]
        f0 = a["f0_hz"]
        for k in range(2, 17):
            t, m = p.get("h%d" % k, np.nan), a.get("h%d" % k, np.nan)
            if np.isnan(t) or np.isnan(m): continue
            out.append((midi, k, f0, t, m))
    return out


def median_abs_err(smp):
    return float(np.median([abs(t - m) for _, _, _, t, m in smp]))


def evaluate(wavs):
    """returns dict: err, bands, smp, ltas12, ltas11, ph_ltas, ph_rows, alt_rows"""
    midis = [m for _, m in NOTES if os.path.exists(PHIL + "wav/ph_cello_%d.wav" % m)]
    ph, ph_ltas = phil_rows(midis)
    alt, ys = measure_alt(wavs)
    smp = samples(alt, ph, midis)
    ltas12 = ltas_of([e for _, _, e in ys])
    ltas11 = ltas_of([e for m, _, e in ys if m in midis])
    return dict(err=median_abs_err(smp), bands=band_diff(ltas11, ph_ltas), smp=smp, ltas12=ltas12, ltas11=ltas11,
                ph_ltas=ph_ltas, ph_rows=ph, alt_rows=alt, midis=midis)


def main():
    args = sys.argv[1:]
    if not args: raise SystemExit(__doc__)
    tag = args.pop(0)
    audio = None
    if "--audio" in args:
        i = args.index("--audio"); audio = args[i + 1]; del args[i:i + 2]
    wavs, rtfs = render_all(tag, tuple(args))
    ev = evaluate(wavs)
    np.savetxt(os.path.join(OUTD, "%s_samples.txt" % tag), np.array(ev["smp"]), fmt="%.4f", header="midi k f0 target_dB model_dB")
    np.savetxt(os.path.join(OUTD, "%s_ltas.txt" % tag), np.column_stack([M.LTAS_F, ev["ltas12"], ev["ltas11"], ev["ph_ltas"]]), fmt="%.4f", header="f alt12 alt11 phil")
    print("RESULT %s knobs [%s]: median |harmonic error| %.2f dB over %d samples; LTAS diff alt-phil in %s: %s; real-time factor mean %.2f (iteration 2: %.2f dB, %s)" % (
        tag, " ".join(args), ev["err"], len(ev["smp"]), BANDS, " ".join("%+.1f" % x for x in ev["bands"]), np.mean(rtfs), IT2["err"], " ".join("%+.1f" % x for x in IT2["bands"])))
    if audio:
        os.makedirs(audio, exist_ok=True)
        for s, midi in NOTES: shutil.copy(wavs[midi], os.path.join(audio, "stage2c_%d_%s.wav" % (midi, s)))


if __name__ == "__main__":
    main()

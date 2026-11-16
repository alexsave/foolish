"""One-factor study of the sympathetic ringing (ITERATION_LOG.md, lowstrings iteration 1). For one setting of the knobs (knobs.h, passed through
the environment variable ALTCELLO_KNOBS to every tool) it measures
  ring    ring_tool: the open string's share of the radiated pressure, D3 stopped on G2 (to E3) and A3 stopped on D3 (to B3), note and after the change
  band    sweep_tool ours at beta 0.1, v_B 0.1, 32 forces 0.03-6 N, 6 s runs: the contiguous Helmholtz band of D3 and A3
  ltas    stage2c_notes.py: LTAS of the 12 calibration notes minus Philharmonia in four bands
  scale   the 7 vibrato-off changes of the scale (scores/scale_voff.txt), expert player, seeds 0 1 2: time to the new pitch within 10 c held 5 periods

    python -I charts/ring_study.py NAME "KNOBS" [parts: ring band ltas scale]       -> out/ring_study/NAME.txt (one RESULT line per measure)
"""
import os, re, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out", "ring_study")
PY = "/Users/alex/Dev/soundtrack-tools/venv/bin/python"


def run(cmd, knobs):
    env = dict(os.environ)
    env["ALTCELLO_KNOBS"] = knobs
    return subprocess.run(["nice"] + cmd, capture_output=True, text=True, check=True, env=env, cwd=ROOT).stdout


def ring(knobs):
    res = {}
    for tag, args in (("D3onG_E3", ["change", "G2", "7", "9"]), ("A3onD_B3", ["change", "D3", "7", "9"]), ("D3onG_lift", ["lift", "G2", "7"]), ("A3onD_lift", ["lift", "D3", "7"])):
        out = run([os.path.join(ROOT, "ring_tool")] + args, knobs)
        d = {}
        for l in out.splitlines():
            f = l.split()
            if f[0] == "NOTE_SUM": d["note_sum"] = float(f[1])
            elif f[0] == "NOTE" and f[1] == "1": d["note_h1"] = float(f[5])
            elif f[0] == "WIN":
                k = "w%d" % round(float(f[1]) * 10)
                d[k + "_ring"] = float(f[4]); d[k + "_broad"] = float(f[6]); d[k + "_total"] = float(f[8])
        res[tag] = d
    return res


def band(knobs, string, semis=0, fmin=0.03, fmax=6.0, nf=32, secs=6.0, vb=0.1, beta=0.1, tag="x"):
    path = os.path.join(OUT, "band_%s.txt" % tag)
    cmd = [os.path.join(ROOT, "sweep_tool"), "ours", string, str(beta), str(fmin), str(fmax), str(nf), str(secs), path, "--vb", str(vb), "--body"]
    if semis: cmd += ["--semis", str(semis)]
    out = run(cmd, knobs)
    m = re.search(r"SUMMARY ours.*simple-classifier" , out)
    ours = [l for l in out.splitlines() if l.startswith("SUMMARY") and "ours-classifier" in l][0]
    simple = [l for l in out.splitlines() if l.startswith("SUMMARY") and "simple-classifier" in l][0]
    # the table definition: a force counts when BOTH classifiers accept (playability_table.py band()), longest contiguous run
    rows = [l.split() for l in open(path) if not l.startswith("#") and l.strip()]
    F = [float(r[0]) for r in rows]
    ok = [int(r[1]) == 1 and int(r[2]) == 1 for r in rows]
    best, i = (0, -1), 0
    while i < len(F):
        if ok[i]:
            j = i
            while j + 1 < len(F) and ok[j + 1]: j += 1
            if j - i + 1 > best[0]: best = (j - i + 1, i)
            i = j + 1
        else: i += 1
    if best[0] == 0: return dict(n=0, lo=0.0, hi=0.0, ratio=0.0, npts=sum(ok))
    lo, hi = F[best[1]], F[best[1] + best[0] - 1]
    return dict(n=best[0], lo=lo, hi=hi, ratio=hi / lo, npts=sum(ok))


def ltas(knobs, tag):
    import stage2c_notes as S
    os.environ["ALTCELLO_KNOBS"] = knobs
    wavs, rtfs = S.render_all("rs_" + tag)
    ev = S.evaluate(wavs)
    return dict(bands=ev["bands"], err=ev["err"])


def scale(knobs, tag, seeds=(0, 1, 2)):
    import onset_lib as L, transitions as T
    os.environ["ALTCELLO_KNOBS"] = knobs
    rows_all = []
    for s in seeds:
        w = os.path.join(L.TMP, "rs_%s_%d.wav" % (tag, s))
        L.render("scale_voff.txt", w, extra=["--seed", str(s), "--vib", "off"])
        rows = T.transitions(w, score=os.path.join(L.SC, "scale_voff.txt"))
        rows_all.append(rows)
        os.remove(w)
    t = np.array([[r[1] for r in rows] for rows in rows_all])  # seeds x 7 ms
    d = np.array([[r[2] for r in rows] for rows in rows_all])
    return dict(median=float(np.nanmedian(t)), max=float(np.nanmax(t)), per_seed_median=[float(np.nanmedian(x)) for x in t], dip_median=float(np.median(d)), n_nan=int(np.isnan(t).sum()), t=t.tolist())


def main():
    name, knobs = sys.argv[1], sys.argv[2]
    parts = sys.argv[3:] or ["ring", "band", "ltas", "scale"]
    os.makedirs(OUT, exist_ok=True)
    f = open(os.path.join(OUT, name + ".txt"), "a")
    def emit(k, v):
        line = "RESULT %s %s %s" % (name, k, repr(v)); print(line, flush=True); f.write(line + "\n"); f.flush()
    emit("knobs", knobs)
    if "ring" in parts: emit("ring", ring(knobs))
    if "band" in parts:
        emit("band_D3", band(knobs, "D3", tag=name + "_D3"))
        emit("band_A3", band(knobs, "A3", tag=name + "_A3"))
    if "ltas" in parts: emit("ltas", ltas(knobs, name))
    if "scale" in parts: emit("scale", scale(knobs, name))


if __name__ == "__main__":
    main()

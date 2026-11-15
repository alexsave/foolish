"""ALT-CELLO diagnosis driver: renders conditions, measures them against Philharmonia, keeps out/diag/results.tsv.

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/diagnose_run.py base            baseline + DIAGNOSIS.md tables
    ... diagnose_run.py cond TAG [PITCH ...]       one named condition (see CONDS) on the given pitches (default all three)
    ... diagnose_run.py table [TAG ...]            print the results table (markdown)
"""
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diagnose as D, diag_render as R  # noqa: E402

RES = os.path.join(R.OUTD, "results.tsv")
KEYS = ["jit", "shim", "ppq5", "wander", "nf0", "nf1", "nf2", "nf3", "hnr", "cent_f0", "h1", "h2", "h3", "b1_3", "b3_8", "att_tilt",
        "mod4_7", "mod10", "lev_sd", "peaky", "sust_ac", "t3db", "tr_db", "nunm", "unm_f", "unm_exc", "t_per", "t_slip", "slip_frac"]
VIB = ("--vib", 11.5, 5.35)
PITCHES = ["G2", "D3", "A3"]


def stop_env(pitch, vb=0.1, F=None):
    F = D.FORCE[pitch] if F is None else F
    return ["--vb-env", "0:0,0.03:%g,2.5:%g,2.52:0" % (vb, vb), "--fb-env", "0:0,0.03:%g,2.5:%g,2.52:0" % (F, F)]


Q500 = ["--q2k", "C2", 300, "--q2k", "G2", 300, "--q2k", "D3", 500, "--q2k", "A3", 500]
C1K = Q500[:] + ["--bridge-R-scale", 3, "--stat-mass-scale", 0.5]
C1F = {"G2": 1.92, "D3": 1.92, "A3": 1.756}  # force scale to the middle of the C1 band
C1S = {"G2": 0.91, "D3": 1.10, "A3": 0.91}  # stopped notes: force scale to the middle of the stopped-string C1 band (diag_band.py --stopped)
MID = {"G2": 1.76, "D3": 1.46, "A3": 1.59}  # force scale to the middle of the Q500 band (diag_band.py, q500)


def conds(pitch):
    F = D.FORCE[pitch]
    c = {
        "base": {},
        "F1a": dict(beta=0.08), "F1b": dict(beta=0.13), "E0a": dict(beta=0.085), "E0b": dict(beta=0.12),
        "F1c": dict(beta=0.08, force_scale=1.398), "F1d": dict(beta=0.13, force_scale=0.6745),
        "E0c": dict(beta=0.085, force_scale=1.276), "E0d": dict(beta=0.12, force_scale=0.7607),
        "E3": dict(knobs=Q500, force_scale=MID[pitch]),
        "E3b": dict(knobs=Q500),
        "E1a": dict(knobs=["--bridge-R-scale", 2]), "E1b": dict(knobs=["--bridge-R-scale", 4]),
        "E2": dict(knobs=["--stat-mass-scale", 0.5]),
        "E123": dict(knobs=Q500 + ["--bridge-R-scale", 2, "--stat-mass-scale", 0.5], force_scale=MID[pitch]),
        "C1g": dict(knobs=C1K + ["--fb-env", "0:0,0.005:%g" % (D.FORCE[pitch] * C1F[pitch]), "--vb-env", "0:0,%g:0.1" % (0.1 / {"G2": 2.0, "D3": 1.0, "A3": 8.0}[pitch])]),
        "C1s": dict(knobs=C1K, force_scale=C1S[pitch], stopped=True),
        "C1v": dict(knobs=C1K + list(VIB), force_scale=C1S[pitch], stopped=True),
        "C1w": dict(knobs=C1K + list(VIB) + ["--walk", 0.05, 2.0], force_scale=C1S[pitch], stopped=True),
        "C1": dict(knobs=C1K, force_scale=C1F[pitch]),
        "E0e": dict(knobs=["--fs", 192000]), "E0f": dict(knobs=["--mode", "m440", 455]),
        "F2a": dict(noise=0.0), "F2b": dict(noise=0.01),
        "F3a": dict(force_scale=0.7), "F3b": dict(force_scale=1.4),
        "F4a": dict(knobs=["--fb-env", "0:0,0.005:%g" % F, "--vb-env", "0:0,0.03:0,0.09:0.1"]),
        "F4b": dict(knobs=["--fb-env", "0:%g,0.001:%g" % (F, F), "--vb-env", "0:0,0.15:0.1"]),
        "F4c": dict(knobs=["--fb-env", "0:0,0.1:%g" % F, "--vb-env", "0:0.1,0.001:0.1"]),
        "F5a": dict(knobs=["--eta0", 0.024]), "F5b": dict(knobs=["--bridge-inertia-scale", 2]),
        "F5c": dict(knobs=["--lead-mass-scale", 2]),
        "F6s": dict(stopped=True), "F6v": dict(stopped=True, knobs=list(VIB)),
        "F7": dict(knobs=["--walk", 0.05, 2.0]),
    }
    return c


_ref = {}
def ref_note(p):
    if p not in _ref: _ref[p] = D.Note(D.PHIL % D.NOTE[p][0], D.NOTE[p][1], "ref")
    return _ref[p]


def evaluate(pitch, wav, prefix):
    nt = D.Note(wav, D.NOTE[pitch][1], "alt")
    return summarize(nt, ref_note(pitch), prefix)


def summarize(nt, rf, prefix=None):
    m = D.window_metrics(nt, D.STEADY)
    g = D.gates(nt)
    at = D.attack_metrics(nt)
    um, nun, npk = D.narrow_bands(nt, rf)
    d = dict(jit=m["jit_pct"], shim=m["shim_db"], ppq5=m["ppq5"], wander=m["wander"], hnr=g["hnr"], cent_f0=m["cent_f0"],
             h1=m["hbal_mean"][0], h2=m["hbal_mean"][1], h3=m["hbal_mean"][2], b1_3=g["b1_3"], b3_8=g["b3_8"], att_tilt=g["att_tilt"],
             mod4_7=g["mod4_7"], mod10=g["mod10"], lev_sd=g["lev_sd"], peaky=g["peaky"], sust_ac=g["sust_ac"], t3db=at["t3db"],
             tr_db=at["tr_db"], nunm=nun, unm_f=um[0]["f"] if um else float("nan"), unm_exc=um[0]["exc"] if um else float("nan"),
             t_per=g["t_per"], t_slip=float("nan"), slip_frac=float("nan"))
    for i in range(4): d["nf%d" % i] = m["nf_tot"][i]
    if prefix and os.path.exists(prefix + ".f32"):
        ss = D.slip_stats(prefix, m["f0"])
        d["t_slip"], d["slip_frac"] = ss["lenient"]
    return d


def save(tag, pitch, d):
    rows = {}
    if os.path.exists(RES):
        for ln in open(RES).read().splitlines()[1:]:
            t = ln.split(); rows[(t[0], t[1])] = t[2:]
    rows[(tag, pitch)] = ["%.5g" % d[k] for k in KEYS]
    with open(RES, "w") as f:
        f.write("tag pitch " + " ".join(KEYS) + "\n")
        for (t, p), v in sorted(rows.items()): f.write("%s %s %s\n" % (t, p, " ".join(v)))


def load():
    out = {}
    if os.path.exists(RES):
        for ln in open(RES).read().splitlines()[1:]:
            t = ln.split(); out[(t[0], t[1])] = dict(zip(KEYS, map(float, t[2:])))
    return out


def run_cond(tag, pitches):
    for p in pitches:
        kw = dict(conds(p)[tag])
        wav, pre = R.render(p, tag + "_", **kw)
        save(tag, p, evaluate(p, wav, pre))
        print("done", tag, p, flush=True)


def ref_row(p):
    rf = ref_note(p)
    return summarize(rf, rf)


def table(tags, pitch):
    res = load()
    cols = ["jit", "shim", "wander", "nf0", "nf1", "nf2", "nf3", "hnr", "cent_f0", "tr_db", "t_slip", "slip_frac", "nunm", "unm_exc", "b1_3", "b3_8", "mod4_7", "lev_sd"]
    s = "| cond | " + " | ".join(cols) + " |\n|" + "---|" * (len(cols) + 1) + "\n"
    rr = ref_row(pitch); rr["t_slip"] = rr["slip_frac"] = float("nan")
    for name, d in [("REF", rr)] + [(t, res[(t, pitch)]) for t in tags if (t, pitch) in res]:
        s += "| %s | " % name + " | ".join(D.fmt(float(d[c]), 2 if c in ("jit", "shim", "slip_frac", "mod4_7") else 1) for c in cols) + " |\n"
    return s


def passes(d, r):
    ok = {}
    ok["bands"] = d["nunm"] == 0
    ok["noise"] = all(abs(d["nf%d" % i] - r["nf%d" % i]) <= 5 for i in range(4))
    ok["attack"] = abs(d["tr_db"] - r["tr_db"]) <= 6
    ok["jitter"] = 0.5 <= d["jit"] / r["jit"] <= 2
    ok["shimmer"] = 0.5 <= d["shim"] / r["shim"] <= 2
    ok["steady"] = d["t_slip"] < 0.3
    return ok


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "cond":
        run_cond(sys.argv[2], sys.argv[3:] or PITCHES)
    elif cmd == "table":
        for p in PITCHES: print("### " + p + "\n" + table(sys.argv[2:] or sorted({t for t, _ in load()}), p))

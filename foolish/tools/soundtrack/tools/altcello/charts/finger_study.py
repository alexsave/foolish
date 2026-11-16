"""Finger and bow-change study (onset_tool fing): per scenario and finger setting, 3 bow-noise seeds, medians.

    python -I charts/finger_study.py OUT.txt SCEN STR [seeds]     SCEN slur|lift|bow, STR the string (G2 for D3 -> E3, D3 for E3 -> open D3 ...)
Prints one line per setting: median t_pitch, t_pitch from the end of the ramp, t_reper, dip, level dip, and how many seeds had the old pitch periodic before."""
import os, subprocess, sys, re
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "onset_tool")
SET = [("base (ramp 10 ms K1 R1 w10)", []), ("ramp 3 ms", ["--ramp", "0.003"]), ("K x0.5", ["--K", "0.5"]), ("K x3", ["--K", "3"]), ("K x10", ["--K", "10"]),
       ("R x0.3", ["--R", "0.3"]), ("R x3", ["--R", "3"]), ("width 6 mm", ["--width", "0.006"]), ("K x10 R x3 ramp 3 ms", ["--K", "10", "--R", "3", "--ramp", "0.003"])]


EXTRA = os.environ.get("FING_EXTRA", "").split()


def run(scen, st, extra, seed):
    out = subprocess.run(["nice", TOOL, "fing", scen, st] + EXTRA + extra + ["--seed", str(seed)], capture_output=True, text=True, check=True).stdout
    g = lambda k: float(re.search(k + r" (\S+)", out).group(1))
    return dict(pre=int(re.search(r"pre_ok (\d)", out).group(1)), tp=g("t_pitch"), tpr=g("t_pitch_ramp"), rep=g("t_reper"), dip=g("dip_c"), lev=g("lev_dip_db"))


def main():
    out, scen, st = sys.argv[1:4]
    seeds = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    f = open(out, "w")
    for name, extra in SET:
        rs = [run(scen, st, extra, s) for s in range(seeds)]
        ok = [r for r in rs if r["pre"]]
        med = lambda k: float(np.median([r[k] for r in ok])) if ok else float("nan")
        f.write("%s | %s | %d/%d | t_pitch %.4f t_pitch_ramp %.4f t_reper %.4f dip_c %.1f lev_dip_db %.1f | all t_pitch %s\n" % (scen, name, len(ok), len(rs), med("tp"), med("tpr"), med("rep"), med("dip"), med("lev"),
                " ".join("%.3f" % r["tp"] for r in rs)))
        f.flush()
    f.close()


main()

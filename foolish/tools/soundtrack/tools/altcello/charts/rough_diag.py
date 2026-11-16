"""Step 1 diagnosis: the contact trace and audio of D2, G2c, D3, A3, vibrato on and off, 3 seeds, the shipped defaults.   python -I charts/rough_diag.py"""
import os, sys, pickle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rough_lib as RL, rich_lib as R  # noqa: E402
out = []
res = {}
for vib in (True, False):
    r = RL.measure_cfg("base", vib=vib)
    res[vib] = r
    out.append("== baseline, vibrato %s\n%s" % ("on" if vib else "off", RL.summary(r)))
print("\n".join(out))
open(os.path.join(RL.OUT, "diag_base.txt"), "w").write("\n".join(out) + "\n")
ref = {k: R.measure(R.ref_note(k), k, "ref" + k) for k in R.NOTES}
print("ref:", {k: (round(v["hnr"], 1), [round(x) for x in v["floor"]]) for k, v in ref.items()})
pickle.dump(res, open(os.path.join(RL.OUT, "diag_base.pkl"), "wb"))

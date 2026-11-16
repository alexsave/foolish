"""Measure one configuration of the roughness work.   python -I charts/rough_run.py TAG "KNOBS" "NOTES" "EXTRA ARGS" [novib]
   -> table on stdout and out/rough/tab_TAG.txt   (NOTES like D2,G2c; EXTRA like "--frac 0.3 0.25")"""
import os, sys, pickle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rough_lib as RL  # noqa: E402
tag = sys.argv[1]
knobs = sys.argv[2] if len(sys.argv) > 2 else ""
notes = tuple((sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else "D2,G2c").split(","))
extra = tuple(sys.argv[4].split()) if len(sys.argv) > 4 else ()
vib = not (len(sys.argv) > 5 and sys.argv[5] == "novib")
suf = sys.argv[6] if len(sys.argv) > 6 else ""
nf = {k: os.path.join(RL.R.OUT, RL.R.NOTES[k][2] + suf + ".txt") for k in notes} if suf else None
res = RL.measure_cfg(tag, knobs, extra, notes, vib=vib, notefiles=nf)
txt = "== %s [%s] %s%s\n%s" % (tag, knobs, " ".join(extra), "" if vib else " novib", RL.summary(res))
print(txt)
open(os.path.join(RL.OUT, "tab_%s.txt" % tag), "a").write(txt + "\n")
pk = os.path.join(RL.OUT, "res_%s.pkl" % tag)
old = pickle.load(open(pk, "rb")) if os.path.exists(pk) else {}
old.update(res)  # a tag keeps the notes of earlier runs (D2,G2c then D3,A3)
pickle.dump(old, open(pk, "wb"))

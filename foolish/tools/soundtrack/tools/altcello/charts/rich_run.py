"""Measure one configuration of the richness work.   python -I charts/rich_run.py TAG [KNOBS]   -> table on stdout, out/rich/res_TAG.pkl, out/rich/tab_TAG.txt"""
import os, sys, pickle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rich_lib as R  # noqa: E402

tag = sys.argv[1]
knobs = sys.argv[2] if len(sys.argv) > 2 else ""
res = R.measure_set(tag, knobs)
rows = R.summarize(res)
txt = R.fmt("%s  [%s]" % (tag, knobs), rows)
print(txt)
open(os.path.join(R.OUT, "tab_%s.txt" % tag), "w").write(txt + "\n")
pickle.dump(res, open(os.path.join(R.OUT, "res_%s.pkl" % tag), "wb"))

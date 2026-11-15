#!/usr/bin/env python3
"""flatten_set_v6.py INSTRUMENT : builds the v6 flat set of a Lyria instrument from its SOURCE cuts with flatten_v6.py.
Reads samples/manifests/lyria_<inst>.txt (roots, layers, rr), the per-file kept attack from the v5 report
/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples/<inst>_flat.md, writes the wavs to
/Users/alex/Dev/soundtrack-samples/lyria/<inst>_flat_v6/ (new files only; refuses to overwrite) and the manifest
samples/manifests/lyria_<inst>_flat_v6.txt (paths /lyria/<inst>_flat_v6/). The v5 set and manifest are not touched.
Report: /Users/alex/Music/foolish_cities_v5/analysis/lyria_samples/<inst>_flat_v6.md (before and after line per sample).
Run niced, one instrument at a time."""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
MAN = os.path.join(HERE, "..", "..", "samples", "manifests")
SAMP = "/Users/alex/Dev/soundtrack-samples/lyria"
REP = "/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples"
inst = sys.argv[1]
keep = {}
for l in open(os.path.join(REP, inst + "_flat.md")):
    c = [x.strip() for x in l.split("|")]
    if len(c) > 4 and c[1].endswith(".wav"):
        keep[c[1]] = float(c[3])
out_dir = os.path.join(SAMP, inst + "_flat_v6")
os.makedirs(out_dir, exist_ok=True)
lines, rep = [], []
for l in open(os.path.join(MAN, "lyria_%s.txt" % inst)):
    if not l.strip():
        continue
    root, layer, rr, path = l.split(None, 3)
    path = path.strip()
    fn = os.path.basename(path)
    out = os.path.join(out_dir, fn)
    if os.path.exists(out):
        sys.exit("refusing to overwrite " + out)
    r = subprocess.run(["nice", "python3", "-I", os.path.join(HERE, "flatten_v6.py"), os.path.join(SAMP, inst, fn), out, "--root", root,
                        "--keep-attack-ms", "%.0f" % keep.get(fn, 0.0)], capture_output=True, text=True)
    msg = (r.stdout + r.stderr).strip().split("\n")[-1]
    print(fn, msg, flush=True)
    rep.append("| %s | %.0f | %s |" % (fn, keep.get(fn, 0.0), msg))
    lines.append("%s %s %s %s" % (root, layer, rr, path.replace("/lyria/%s/" % inst, "/lyria/%s_flat_v6/" % inst)))
open(os.path.join(MAN, "lyria_%s_flat_v6.txt" % inst), "w").write("\n".join(lines) + "\n")
open(os.path.join(REP, inst + "_flat_v6.md"), "w").write("\n".join(["# %s samples flattened, v6 (pitch, level and per-harmonic)" % inst, "",
    "| file | keep ms | before and after (pp over 25 to 90 percent) |", "|---|---|---|"] + rep) + "\n")

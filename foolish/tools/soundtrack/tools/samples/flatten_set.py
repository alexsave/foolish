#!/usr/bin/env python3
"""flatten_set.py INSTRUMENT --manifests DIR --rows ROWS.tsv --att ATT.tsv --out-samples DIR --report REPORT.md
Flattens every sample of DIR/lyria_<instrument>.txt with flatten_sample.py into OUT/<file> and writes the manifest
DIR/lyria_<instrument>_flat.txt (same roots, layers, rr; paths /lyria/<instrument>/ -> /lyria/<instrument>_flat/).
ROWS.tsv is the log of lyria_notes_extract.py (attack natural or legato per file); ATT.tsv has "file<TAB>length ms<TAB>attack ms<TAB>zone ms"
(zone_report). --keep-attack-ms is the measured attack time plus 20 ms for natural attacks and 0 for legato cuts.
A sample the tool calls UNRELIABLE is copied unchanged and listed. The report has the before and after line of every sample."""
import argparse, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("inst")
ap.add_argument("--manifests", required=True)
ap.add_argument("--rows", required=True)
ap.add_argument("--att", required=True)
ap.add_argument("--out-samples", required=True)
ap.add_argument("--report", required=True)
a = ap.parse_args()
attack = {os.path.basename(r.split("\t")[0]): r.split("\t")[7] for r in open(a.rows) if r.strip()}
att = {l.split("\t")[0]: float(l.split("\t")[2]) for l in open(a.att) if l.strip()}
os.makedirs(a.out_samples, exist_ok=True)
lines, rep, bad = [], [], []
for l in open(os.path.join(a.manifests, "lyria_%s.txt" % a.inst)):
    if not l.strip():
        continue
    root, layer, rr, path = l.split(None, 3)
    path = path.strip()
    fn = os.path.basename(path)
    src = "/Users/alex/Dev/soundtrack-samples/lyria/%s/%s" % (a.inst, fn)
    out = os.path.join(a.out_samples, fn)
    keep = att.get(fn, 0.0) + 20.0 if attack.get(fn) == "natural" else 0.0
    r = subprocess.run(["python3", "-I", os.path.join(HERE, "flatten_sample.py"), src, out, "--root", root, "--keep-attack-ms", "%.0f" % keep],
                       capture_output=True, text=True)
    msg = (r.stdout + r.stderr).strip().split("\n")[-1]
    if msg.startswith("UNRELIABLE"):
        bad.append(fn)
    rep.append("| %s | %s | %.0f | %s |" % (fn, attack.get(fn, "?"), keep, msg))
    lines.append("%s %s %s %s" % (root, layer, rr, path.replace("/lyria/%s/" % a.inst, "/lyria/%s_flat/" % a.inst)))
open(os.path.join(a.manifests, "lyria_%s_flat.txt" % a.inst), "w").write("\n".join(lines) + "\n")
open(a.report, "w").write("\n".join(["# %s samples flattened" % a.inst, "",
    "Each row is one sample: attack kind, the kept attack in ms, then the line printed by flatten_sample.py.", "",
    "| file | attack | keep ms | before and after |", "|---|---|---|---|"] + rep +
    ["", "Samples left unflattened (pitch track unreliable, copied as they are): %s." % (", ".join(bad) if bad else "none")]) + "\n")
print("%d samples, %d unreliable: %s" % (len(lines), len(bad), " ".join(bad)))

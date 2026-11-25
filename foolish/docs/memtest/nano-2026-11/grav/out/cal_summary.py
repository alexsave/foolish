# cal_summary.py DIR: for each (group, name, param, k) row present in the passes, the median over passes of the row's p50_ns
# and p99_ns (TSV columns 10 and 11). n is not part of the key: calbench sizes n by a time budget, so it varies by pass.
# per_unit_ns: the p50 divided by the "xN" multiplier in param when present; mem rows (load_latency) are divided by 8,
# the loads per op in b_chase.
import sys, glob, re, statistics, collections, os
d = sys.argv[1]
rows = collections.defaultdict(list)
for f in sorted(glob.glob(os.path.join(d, "p[123]_*.tsv"))):
    for line in open(f):
        if line.startswith("#") or line.startswith("group") or not line.strip():
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) < 17:
            continue
        g, name, param, k = p[0], p[1], p[2], p[3]
        rows[(g, name, param)].append((float(p[9]), float(p[10])))
for key in sorted(rows):
    g, name, param = key; k = "varies"
    v = rows[key]
    m50 = statistics.median(x[0] for x in v); m99 = statistics.median(x[1] for x in v)
    mult = re.search(r"x(\d+)$", param)
    unit = m50 / int(mult.group(1)) if mult else m50
    if g == "mem":
        unit = m50 / 8.0
    print(f"{g}\t{name}\t{param}\tk={k}\treps={len(v)}\tp50_row_ns={m50:.3f}\tp99_row_ns={m99:.3f}\tper_unit_ns={unit:.3f}")

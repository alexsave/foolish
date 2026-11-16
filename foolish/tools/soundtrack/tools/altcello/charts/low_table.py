"""Print the table of out/low_study/*.txt (low_study.py): python -I charts/low_table.py NAME ..."""
import os, sys
D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "low_study")
def load(n):
    r = {}
    for l in open(os.path.join(D, n + ".txt")):
        p = l.split(" ", 3)
        r[p[2]] = eval(p[3], {'nan': float('nan')})
    return r
def table(names):
    rows = []
    for n in names:
        r = load(n)
        row = [n]
        for s, k in (("C2 open", "C2_open"), ("G2 on C", "G2_on_C")):
            b, g = r.get("band_" + k), r.get("guettler_" + k)
            nt = r.get("note", {}).get(s)
            row += ["x%.2f" % b["ratio"] if b else "-", "%d/%d" % (g["fast"], g["steady"]) if g else "-", "%.1f" % nt["hnr_med"] if nt else "-",
                    "%.2f" % nt["t_per_med"] if nt else "-", "%+.1f" % nt["diff"][2] if nt else "-"]
        rows.append(row)
    return rows
if __name__ == "__main__":
    print("%-9s | C2 open: band  fast/steady  HNR  t_per  d1-3k | G2 on C: band fast/steady HNR t_per d1-3k" % "name")
    for r in table(sys.argv[1:]): print("%-9s | %s | %s" % (r[0], "  ".join(r[1:6]), "  ".join(r[6:11])))

"""Print the table of out/ring_study/*.txt (ring_study.py): python -I charts/ring_table.py NAME ..."""
import ast, os, sys
D = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "ring_study")
def load(n):
    r = {}
    for l in open(os.path.join(D, n + ".txt")):
        p = l.split(" ", 3)
        r[p[2]] = eval(p[3], {'nan': float('nan')})
    return r
if __name__ == "__main__": print("%-10s | D3onG: note  ring0.1 ring0.3 | A3onD: note ring0.1 ring0.3 | band D3 A3 | LTAS d(100-300,300-1k,1-3k,3-8k) | scale med max dip" % "name")
def main():
    for n in sys.argv[1:]:
        r = load(n)
        g = r.get("ring", {})
        s = ""
        if g:
            a, b = g["D3onG_E3"], g["A3onD_B3"]
            s += "%+.1f %+.1f %+.1f | %+.1f %+.1f %+.1f" % (a["note_sum"], a["w1_ring"], a["w3_ring"], b["note_sum"], b["w1_ring"], b["w3_ring"])
        bd = " ".join("x%.2f(%.2f-%.2f)" % (r[k]["ratio"], r[k]["lo"], r[k]["hi"]) if k in r else "-" for k in ("band_D3", "band_A3"))
        lt = " ".join("%+.1f" % x for x in r["ltas"]["bands"]) if "ltas" in r else "-"
        sc = "%.0f %.0f %.0f" % (r["scale"]["median"], r["scale"]["max"], r["scale"]["dip_median"]) if "scale" in r else "-"
        print("%-10s | %s | %s | %s | %s" % (n, s, bd, lt, sc))

if __name__ == "__main__":
    main()

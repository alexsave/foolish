"""Iteration 2 gate check (step 4): the three diagnosis notes with the new Instrument defaults at the band centre.

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/iter2_gate.py F_G2 F_G2_for_D3 F_D3_for_A3
G2 open string; D3 stopped on the G string; A3 stopped on the D string (both with vibrato 11.5 c at 5.35 Hz); forces = the
table centres at beta 0.1, v_B 0.1 of the string that is bowed. Writes /Users/alex/Music/foolish_cities_v5/alt_cello/iter2/gate.tsv
and prints the table against the DIAGNOSIS.md C1 rows (copied below) and the Philharmonia TARGET rows.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diagnose as D, diag_render as R, diagnose_run as DR  # noqa: E402

COLS = ["jit", "shim", "wander", "nf0", "nf1", "nf2", "nf3", "hnr", "cent_f0", "tr_db", "t_slip", "slip_frac", "nunm", "b1_3", "b3_8", "mod4_7"]
# DIAGNOSIS.md, one-factor tables: C1 (open) for G2, "C1 stopped + vibrato" for D3 and A3
C1 = {"G2": [0.04, 0.19, 0.9, -31.0, -31.3, -46.2, -60.4, 25.5, 4.3, 2.6, 0.4, 1.00, 27, -8.1, -32.8, 0.23],
      "D3": [0.15, 0.34, 2.1, -27.8, -29.0, -40.6, -49.6, 20.3, 3.0, 3.8, 0.2, 1.00, 54, -10.8, -29.2, 0.89],
      "A3": [0.18, 0.43, 1.9, -24.6, -22.7, -37.2, -50.1, 20.4, 2.6, 1.3, 0.1, 1.00, 19, -5.7, -25.3, 0.84]}
TARGET = {"G2": [0.08, 0.36, 3.4, -28.2, -36.7, -45.4, -54.8, 24.0, 3.0, 0.8, None, None, 0, -22.6, -43.1, 0.80],
          "D3": [0.19, 0.32, 5.2, -27.2, -35.4, -44.5, -49.4, 25.3, 2.2, -5.4, None, None, 0, -17.5, -35.0, 0.68],
          "A3": [0.19, 0.21, 6.5, -20.7, -28.3, -37.6, -37.1, 22.2, 2.9, -4.4, None, None, 0, -8.3, -27.6, 0.37]}
F = dict(zip(["G2", "D3", "A3"], [float(a) for a in sys.argv[1:4]]))
out = "/Users/alex/Music/foolish_cities_v5/alt_cello/iter2/gate.tsv"
rows = []
for p in ["G2", "D3", "A3"]:
    if p == "G2": wav, pre = R.render(p, "i2g_", F=F[p])
    else: wav, pre = R.render(p, "i2g_", F=F[p], stopped=True, knobs=list(DR.VIB))
    d = DR.evaluate(p, wav, pre)
    rows.append((p, [d[c] for c in COLS], d))
with open(out, "w") as f:
    for p, v, d in rows:
        f.write(p + " NEW " + " ".join("%.5g" % x for x in v) + "\n")
        f.write(p + " C1 " + " ".join("%.5g" % x for x in C1[p]) + "\n")
        f.write(p + " REF " + " ".join("nan" if x is None else "%.5g" % x for x in TARGET[p]) + "\n")
print("| note | row | " + " | ".join(COLS) + " |")
for p, v, d in rows:
    for name, vals in (("REF", TARGET[p]), ("C1", C1[p]), ("NEW F=%.3f" % F[p], v)):
        print("| %s | %s | " % (p, name) + " | ".join(D.fmt(x, 2 if c in ("jit", "shim", "slip_frac", "mod4_7") else 1) if x is not None else "n/a" for c, x in zip(COLS, vals)) + " |")

"""Iteration 2: render open-string notes at given (force, noise) conditions and measure HNR and the 4 noise floors against Philharmonia.

    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/iter2_notes.py PITCH TAG:FORCE:NOISE [...]     (prints a table)
Renders 4 s (probe, body on, all strings, beta 0.1, v_B 0.1), steady window 0.5-2.0 s (charts/diagnose.py definitions).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diagnose as D, diag_render as R  # noqa: E402

pitch = sys.argv[1]
ref = D.Note(D.PHIL % D.NOTE[pitch][0], D.NOTE[pitch][1], "ref")
rm = D.window_metrics(ref, D.STEADY)
rg = D.gates(ref)
print("%-14s %7s %6s %6s | %7s %7s %7s %7s | %6s %6s" % ("cond", "F", "noise", "HNR", "nf0", "nf1", "nf2", "nf3", "jit%", "dnf_max"))
print("%-14s %7s %6s %6.1f | %7.1f %7.1f %7.1f %7.1f |" % ("REF", "", "", rg["hnr"], *rm["nf_tot"]))
for spec in sys.argv[2:]:
    tag, F, n = spec.split(":")
    wav, pre = R.render(pitch, "i2_" + tag + "_", F=float(F), noise=float(n))
    nt = D.Note(wav, D.NOTE[pitch][1], "alt")
    m = D.window_metrics(nt, D.STEADY)
    g = D.gates(nt)
    dn = max(abs(a - b) for a, b in zip(m["nf_tot"], rm["nf_tot"]))
    print("%-14s %7.4f %6s %6.1f | %7.1f %7.1f %7.1f %7.1f | %6.2f %6.1f" % (tag, float(F), n, g["hnr"], *m["nf_tot"], m["jit_pct"], dn), flush=True)

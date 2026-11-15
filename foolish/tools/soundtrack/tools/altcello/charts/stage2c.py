"""ALT-CELLO stage 2 iteration 3 chart: physical radiation, wood loss and bridge hill.

Run from the altcello directory after `make`, `bowhair_study out/bowhair.txt` and `charts/stage2c_notes.py base --audio ...stage2c`:
    nice /Users/alex/Dev/soundtrack-tools/venv/bin/python -I charts/stage2c.py [OUT.png]
"""
import os, subprocess, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import stage2c_notes as SN  # noqa: E402
M = SN.M

OUT = sys.argv[1] if len(sys.argv) > 1 else "/Users/alex/Music/foolish_cities_v5/alt_cello/charts/stage2_body_iter3.png"
AUD3 = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/stage2c"
AUD2 = "/Users/alex/Music/foolish_cities_v5/alt_cello/audio/stage2b"
O = lambda n: os.path.join(ROOT, "out", n)
C_T, C_3, C_2 = "#222222", "#d95f02", "#9a9a9a"


def radiation_curves():
    f = np.geomspace(20, 20000, 600)
    ka = 2 * np.pi * f * 0.25 / 343.0
    hc = 20 * np.log10(ka / np.sqrt(1 + ka ** 2))
    sg = np.ones_like(f)
    fc = 3000.0
    a = np.sqrt(f[f < fc] / fc); lam = 343.0 / fc
    pre = 2.0 * lam / (4 * np.pi ** 2 * 0.17)
    s = pre * ((1 - a * a) * np.log((1 + a) / (1 - a)) + 2 * a) / (1 - a * a) ** 1.5
    sg[f < fc] = np.minimum(1.0, s)
    return f, hc, 10 * np.log10(sg)


def plate_filter_curve(f):
    """the realised digital plate filter (radiate_plate_table.c, fc 3000, 96 kHz), read from the table text"""
    txt = open(os.path.join(ROOT, "radiate_plate_table.c")).read()
    blk = txt[txt.index("{3000.0,"):]
    nums = blk[:blk.index("}},") + 2]
    import re
    vals = [float(x) for x in re.findall(r"[-+]?\d+\.?\d*(?:e[-+]?\d+)?", nums.replace("{3000.0,", ""))]
    K = vals[0]; z = vals[1:7]; p = vals[7:13]
    fs = 96000.0
    w = 2 * np.pi * f / fs
    zinv = np.exp(-1j * w)
    H = np.full(len(f), K, dtype=complex)
    c2 = 2 * fs
    for zi, pi in zip(z, p):
        d = 1 + c2 / pi
        H *= ((1 + c2 / zi) / d + (1 - c2 / zi) / d * zinv) / (1 + (1 - c2 / pi) / d * zinv)
    return 20 * np.log10(np.abs(H))


def admittance():
    subprocess.run(["nice", os.path.join(ROOT, "altcello"), "admittance", O("adm3")], check=True)
    d = np.loadtxt(O("adm3_adm.txt"))
    return d[:, 0], d[:, 1]


def hair_table():
    rows = [l.split() for l in open(O("bowhair.txt")) if not l.startswith("#")]
    return [dict(kh=float(r[0]), F=float(r[1]), cls=r[2], onsets=float(r[3]), corr=float(r[4]), slip=float(r[5]),
                 cents=float(r[6]), settle=float(r[7]), hump=float(r[8])) for r in rows]


def hair_summary(rows):
    out = []
    for kh in sorted(set(r["kh"] for r in rows)):
        rr = [r for r in rows if r["kh"] == kh]
        grid = [r for r in rr if abs(r["F"] - 0.526) > 1e-6]
        ref = [r for r in rr if abs(r["F"] - 0.526) < 1e-6][0]
        hel = [r for r in grid if r["cls"] == "helmholtz"]
        out.append(dict(kh=kh, nhel=len(hel), ref=ref,
                        hump_med=float(np.median([r["hump"] for r in hel])) if hel else np.nan,
                        settle_med=float(np.median([r["settle"] for r in hel if r["settle"] >= 0])) if any(r["settle"] >= 0 for r in hel) else np.nan,
                        slip=float(np.mean([r["slip"] for r in hel])) if hel else np.nan,
                        cents=float(np.median([abs(r["cents"]) for r in hel])) if hel else np.nan, hel=hel, all=grid))
    return out


def main():
    f, hc, sg = radiation_curves()
    fa, ya = admittance()
    rows = hair_table()
    summ = hair_summary(rows)
    # the notes
    ev3 = SN.evaluate({m: os.path.join(AUD3, "stage2c_%d_%s.wav" % (m, s)) for s, m in SN.NOTES})
    ev2 = SN.evaluate({m: os.path.join(AUD2, "stage2b_%d_%s.wav" % (m, s)) for s, m in SN.NOTES})
    print("iteration 3: err %.2f bands %s" % (ev3["err"], ev3["bands"]))
    print("iteration 2 re-measured: err %.2f bands %s" % (ev2["err"], ev2["bands"]))

    fig, ax = plt.subplots(2, 2, figsize=(16, 11), dpi=100)
    fig.suptitle("ALT-CELLO - physical radiation (compact modes + plate radiation efficiency), wood loss and bridge hill - stage 2 iteration 3", fontsize=15, fontweight="bold", y=0.985)

    # (a)
    a = ax[0, 0]
    a.semilogx(f, hc, color="#1b9e77", lw=2, label="|H_c|: compact modes, first order, corner ka = 1 (218 Hz)")
    a.semilogx(f, sg, color="#7570b3", lw=2, label="|sqrt(sigma)|: plate modes, baffled plate, f_c 3 kHz")
    a.semilogx(f, plate_filter_curve(f), color="#7570b3", lw=1.2, ls=":", label="realised 6-section IIR (96 kHz)")
    a.set_xlim(20, 20000); a.set_ylim(-45, 6)
    a.set_xlabel("frequency (Hz)"); a.set_ylabel("radiation filter magnitude (dB)")
    a.axvline(218, color="#1b9e77", lw=0.8, ls="--"); a.axvline(3000, color="#7570b3", lw=0.8, ls="--")
    a.text(225, -43, "ka = 1", color="#1b9e77", fontsize=9); a.text(3100, -43, "f_c", color="#7570b3", fontsize=9)
    b = a.twinx()
    b.semilogx(fa, ya, color="#d95f02", lw=1.2, alpha=0.9, label="|Y_uu| bridge admittance (with the Woodhouse hill at about 1.5 kHz)")
    b.set_ylim(-95, -15); b.set_ylabel("|Y_uu| (dB re 1 s/kg)", color="#d95f02")
    h1, l1 = a.get_legend_handles_labels(); h2, l2 = b.get_legend_handles_labels()
    a.legend(h1 + h2, l1 + l2, loc="lower right", fontsize=8.5, framealpha=0.92)
    a.set_title("(a) radiation filters and bridge admittance", fontsize=11)
    a.grid(alpha=0.25, which="both")

    # (b)
    b = ax[0, 1]
    base = summ[0]
    for s in summ:
        x = s["kh"] if s["kh"] > 0 else 9e2
        for r in s["all"]:
            b.plot(x, r["hump"], marker="o" if r["cls"] == "helmholtz" else "x", ms=6 if r["cls"] == "helmholtz" else 7,
                   mfc="#bbbbbb" if r["cls"] == "helmholtz" else "none", mec="#555555", ls="none", alpha=0.8)
    hx = [s["kh"] for s in summ[1:]]
    b.plot(hx, [s["ref"]["hump"] for s in summ[1:]], "s-", color=C_3, lw=1.8, ms=8, label="hump at F_B 0.526 N (the iteration 2 reference force)")
    b.plot(hx, [s["hump_med"] for s in summ[1:]], "D-", color="#1f78b4", lw=1.8, ms=7, label="median hump over the Helmholtz forces of the 6-point grid")
    b.axhline(base["ref"]["hump"], color=C_3, ls="--", lw=1.3, label="no hair, F_B 0.526 N: %.1f dB" % base["ref"]["hump"])
    b.axhline(base["hump_med"], color="#1f78b4", ls="--", lw=1.3, label="no hair, median of its Helmholtz forces: %.1f dB" % base["hump_med"])
    b.axhline(base["ref"]["hump"] - 5, color="#444444", ls=":", lw=1.2, label="needed to enable: 5 dB lower")
    b.set_xscale("log"); b.set_xlim(6e2, 1.6e5); b.set_ylim(-26, 70)
    b.set_xlabel("bow-hair stiffness k_h (N/m)"); b.set_ylabel("bridge-force hump 2.5-3.5 kHz (dB re h1)")
    for s in summ[1:]:
        b.text(s["kh"], 36, "%d/6 H" % s["nhel"], ha="center", fontsize=9, color="#333333")
    b.text(9e2, 36, "%d/6 H" % base["nhel"], ha="center", fontsize=9, color="#333333")
    b.text(9e2, 32, "no hair", ha="center", fontsize=8, color="#333333")
    b.legend(loc="upper center", fontsize=8.5, framealpha=0.92)
    b.set_title("(b) bow-hair study: D3, beta 0.1, v_B 0.1 (dots: forces; x: not Helmholtz; H: Helmholtz of 6)", fontsize=10.5)
    b.grid(alpha=0.25, which="both")

    # (c)
    c = ax[1, 0]
    marks = {36: "C2 open", 50: "D3 open", 64: "E4"}
    cols = {36: "#1b9e77", 50: "#7570b3", 64: "#e7298a"}
    ks = np.arange(1, 17)
    for midi, nm in marks.items():
        def lev(rows):
            return np.array([0.0] + [rows[midi].get("h%d" % k, np.nan) for k in range(2, 17)])
        a3, a2 = lev(ev3["alt_rows"]), lev(ev2["alt_rows"])
        c.plot(ks, a3, "-o", color=cols[midi], ms=5, lw=1.6)
        c.plot(ks, a2, ":", color=C_2, lw=1.6)
        tm = midi if midi in ev3["ph_rows"] else None
        if tm:
            p = np.array([0.0] + [ev3["ph_rows"][tm].get("h%d" % k, np.nan) for k in range(2, 17)])
            c.plot(ks, p, "o", mfc="none", mec=cols[midi], ms=8, mew=1.6, ls="none")
        c.text(16.2, a3[-1], nm, color=cols[midi], fontsize=9, va="center")
    # the C2 open string has no Philharmonia sample: show D2 (MIDI 38) as the nearest target
    p = np.array([0.0] + [ev3["ph_rows"][38].get("h%d" % k, np.nan) for k in range(2, 17)])
    c.plot(ks, p, "s", mfc="none", mec=cols[36], ms=7, mew=1.3, ls="none")
    c.set_xlim(0.5, 18.2); c.set_xticks(ks); c.set_ylim(-64, 54)
    c.set_xlabel("harmonic number"); c.set_ylabel("level re h1 (dB)")
    leg = [Line2D([], [], marker="o", mfc="none", mec="k", ls="none", ms=8, label="TARGET Philharmonia (open; squares: D2, the nearest sample to C2)"),
           Line2D([], [], marker="o", color="k", ms=5, lw=1.6, label="ALT-CELLO iteration 3 (solid)"),
           Line2D([], [], color=C_2, ls=":", lw=1.6, label="ALT-CELLO iteration 2 (dotted grey)")]
    c.legend(handles=leg, loc="lower left", fontsize=8.5, framealpha=0.92)
    c.set_title("(c) harmonic levels, C2 open / D3 open / E4 (colour per note)", fontsize=11)
    c.grid(alpha=0.25)

    # (d)
    d = ax[1, 1]
    d.semilogx(M.LTAS_F, ev3["ph_ltas"], color=C_T, lw=2.4, label="TARGET Philharmonia (11 notes)")
    d.semilogx(M.LTAS_F, ev3["ltas11"], color=C_3, lw=2, label="ALT-CELLO iteration 3")
    d.semilogx(M.LTAS_F, ev2["ltas11"], color=C_2, lw=1.8, ls=":", label="ALT-CELLO iteration 2")
    for lo, hi in SN.BANDS: d.axvspan(lo, hi, color="#000000", alpha=0.03 if lo in (100, 1000) else 0.07)
    d.set_xlim(60, 16000); d.set_xlabel("frequency (Hz)"); d.set_ylabel("long-term average spectrum (dB)")
    d.legend(loc="upper right", fontsize=9, framealpha=0.92)
    d.set_title("(d) LTAS of the 12 calibration notes (bands shaded)", fontsize=11)
    d.grid(alpha=0.25, which="both")

    t3 = "iteration 3: median |harmonic error| h2..h16 %.2f dB;  LTAS ALT - TARGET in 100-300 / 300-1000 / 1000-3000 / 3000-8000 Hz: %s dB" % (ev3["err"], " / ".join("%+.1f" % x for x in ev3["bands"]))
    t2 = "iteration 2 (reported): 8.56 dB; +0.1 / +2.9 / +6.7 / +17.8 dB   (re-measured from the stage 2b wavs: %.2f dB; %s)" % (ev2["err"], " / ".join("%+.1f" % x for x in ev2["bands"]))
    fig.text(0.5, 0.012, t3 + "\n" + t2, ha="center", fontsize=10.5, family="monospace")
    fig.tight_layout(rect=[0, 0.045, 1, 0.965])
    fig.savefig(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()

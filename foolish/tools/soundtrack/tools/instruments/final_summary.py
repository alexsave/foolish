#!/usr/bin/env python3
"""final_summary.py OLD.wav OLD_SECTIONS NEW.wav NEW_SECTIONS OUTDIR : one summary figure per instrument (CELLO, HAEGEUM, TRUMPET), saved as
OUTDIR/summary_<instrument>.png, comparing the OFF half before (old render) and after (new render) with the target (a flat tone: zero)
and showing the ON half of the new render (the engine vibrato must remain). Bars: pitch depth (cents pp), total level depth (dB pp),
largest harmonic 1 to 4 depth (dB pp), each as the median over the five held notes, plus the 3 to 8 Hz peak/floor ratios in text.
Prints the same table. Uses vib_demo_measure.summary."""
import os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vib_demo_measure as dm


def main():
    ow, os_, nw, ns, out = sys.argv[1:6]
    old, new = dm.summary(ow, os_), dm.summary(nw, ns)
    for inst in ("cello", "haegeum", "trumpet"):
        fig, ax = plt.subplots(1, 3, figsize=(15, 5.5))
        fig.suptitle("%s - vibrato OFF half: before (v5 flat set), after (v6 flat set) and target (flat tone); ON half of v6 for reference" % inst.upper(), fontsize=12, fontweight="bold")
        for a, (key, ratio, unit, ttl) in zip(ax, (("pitch_pp", "pitch_ratio", "cents peak to peak", "pitch modulation"), ("level_pp", "level_ratio", "dB peak to peak", "total level modulation"),
                                              ("harm_pp", "harm_ratio", "dB peak to peak", "harmonic 1 to 4 level modulation (largest)"))):
            vals = [old[(inst, "off")][key], new[(inst, "off")][key], 0.0, new[(inst, "on")][key]]
            labs = ["OFF before (v5)", "OFF after (v6)", "target: flat tone", "ON after (v6, engine vibrato)"]
            cols = ["tab:red", "tab:green", "0.5", "tab:blue"]
            b = a.bar(range(4), vals, color=cols)
            for i, v in enumerate(vals):
                r = (old[(inst, "off")][ratio], new[(inst, "off")][ratio], None, new[(inst, "on")][ratio])[i]
                a.text(i, v, "%.2f" % v + ("\nx%.1f" % r if r is not None else ""), ha="center", va="bottom", fontsize=9)
            a.set_xticks(range(4)); a.set_xticklabels(["OFF\nbefore", "OFF\nafter", "target\nflat", "ON\nafter"], fontsize=9)
            a.set_ylabel(unit); a.set_title(ttl, fontsize=10); a.legend(b, labs, fontsize=7, loc="upper left")
        fig.text(0.5, 0.01, "numbers over the bars: depth, and xN = 3 to 8 Hz peak over the 20 to 35 Hz floor (peak/floor, 3 or more = a peak above the floor); median of 5 held notes", ha="center", fontsize=8)
        fig.tight_layout(rect=(0, 0.04, 1, 0.92))
        fig.savefig(os.path.join(out, "summary_%s.png" % inst), dpi=75)
        plt.close(fig)
        for half in ("off", "on"):
            for tag, d in (("v5", old), ("v6", new)):
                v = d[(inst, half)]
                print("%-8s %-3s %s: pitch %.2f c (x%.1f) level %.2f dB (x%.1f) harm %.2f dB (x%.1f) peak at %.1f Hz; worst note pitch %.2f c level %.2f dB; notes with a ratio>=3: %d of %d" % (
                    inst, half, tag, v["pitch_pp"], v["pitch_ratio"], v["level_pp"], v["level_ratio"], v["harm_pp"], v["harm_ratio"], v["peak_hz"], v["pitch_max"], v["level_max"], v["bad"], v["n"]))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""extend_set_v7.py INSTRUMENT [--check] : builds the v7 long set of a Lyria instrument from its v6 flat set with extend_v7.py.
Reads samples/manifests/lyria_<inst>_flat_v6.txt (roots, layers, rr), the per file kept attack from the v5 report
/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples/<inst>_flat.md, the v6 wavs from /Users/alex/Dev/soundtrack-samples/lyria/<inst>_flat_v6/,
writes /Users/alex/Dev/soundtrack-samples/lyria/<inst>_flat_v7/ (new files only; refuses to overwrite) and the manifest
samples/manifests/lyria_<inst>_flat_v7.txt (same lines as v6, path folder _flat_v7). The loop zone is not in the manifest: the engine's mode 2
("long loop") derives it from the sample length (zone from 20 percent of the length to 60 ms before the end, first jump at 'end - 60 ms - crossfade'),
and a sample of 12 s is never reached by a note of up to 8 s read at up to 1.45 times speed, so those notes never loop.
With --check the report /Users/alex/Music/foolish_cities_v5/analysis/lyria_samples/<inst>_flat_v7.md gets one check_v7 line per sample.
Run niced, one instrument at a time."""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
MAN = os.path.join(HERE, "..", "..", "samples", "manifests")
SAMP = "/Users/alex/Dev/soundtrack-samples/lyria"
REP = "/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples"


def main():
    inst = sys.argv[1]
    check = "--check" in sys.argv
    keep = {}
    for l in open(os.path.join(REP, inst + "_flat.md")):
        c = [x.strip() for x in l.split("|")]
        if len(c) > 4 and c[1].endswith(".wav"):
            keep[c[1]] = float(c[3])
    out_dir = os.path.join(SAMP, inst + "_flat_v7")
    os.makedirs(out_dir, exist_ok=True)
    lines, rep, tsv = [], [], []
    for l in open(os.path.join(MAN, "lyria_%s_flat_v6.txt" % inst)):
        if not l.strip():
            continue
        root, layer, rr, path = l.split(None, 3)
        fn = os.path.basename(path.strip())
        out = os.path.join(out_dir, fn)
        if os.path.exists(out):
            sys.exit("refusing to overwrite " + out)
        src = os.path.join(SAMP, inst + "_flat_v6", fn)
        r = subprocess.run(["nice", "python3", "-I", os.path.join(HERE, "extend_v7.py"), src, out, "--root", root,
                            "--keep-attack-ms", "%.0f" % keep.get(fn, 0.0)], capture_output=True, text=True)
        msg = (r.stdout + r.stderr).strip().split("\n")[-1]
        print(fn, msg, flush=True)
        row = "| %s | %.0f | %s |" % (fn, keep.get(fn, 0.0), msg)
        if check:
            import check_v7
            o = check_v7.measure(src, out, int(root), keep.get(fn, 0.0))
            row += " null %.0f/%.0f step %.2f recur %.2f white %.2f chance %.2f ripple %.2f hnr %.1f -> %.1f pp %.2f/%.2f/%.2f |" % (o["null_head"], o["null_tail"], o["step_splice"], o["recur"], o["recur_white"], o["recur_chance"], o["ripple"], o["hnr6"], o["hnr7"], o["vib7"]["pitch_pp"], o["vib7"]["level_pp"], o["vib7"]["harm_pp"])
            print("   ", row, flush=True)
        rep.append(row)
        if check:
            v6, v7 = o["vib6"], o["vib7"]
            tsv.append("\t".join([fn, "%.0f" % o["null_head"], "%.0f" % o["null_tail"], "%.2f" % o["step_splice"], "%.2f" % o["recur"], "%.2f" % o["recur_white"], "%.2f" % o["recur_chance"],
                "%.2f" % o["ripple"], "%.1f" % o["hnr6"], "%.1f" % o["hnr7"], "%.1f" % o["hnr7_same"]] + ["%.3f" % v[k] for v in (v6, v7) for k in ("pitch_depth", "level_depth", "harm_depth", "ratio", "ratio2", "pitch_pp", "level_pp", "harm_pp")]))
        lines.append("%s %s %s %s" % (root, layer, rr, path.strip().replace("/%s_flat_v6/" % inst, "/%s_flat_v7/" % inst)))
    open(os.path.join(MAN, "lyria_%s_flat_v7.txt" % inst), "w").write("\n".join(lines) + "\n")
    if check:
        hdr = ["file", "null_head", "null_tail", "step_splice", "recur", "recur_white", "recur_chance", "ripple", "hnr6", "hnr7", "hnr7_same"] + ["%s_%s" % (v, k) for v in ("v6", "v7") for k in ("pitch_depth", "level_depth", "harm_depth", "ratio", "ratio2", "pitch_pp", "level_pp", "harm_pp")]
        open(os.path.join(REP, inst + "_flat_v7_checks.tsv"), "w").write("\t".join(hdr) + "\n" + "\n".join(tsv) + "\n")
    open(os.path.join(REP, inst + "_flat_v7.md"), "w").write("\n".join(["# %s long samples, v7 (12 s, fresh noise, slow drift)" % inst, "",
        "| file | keep ms | build |", "|---|---|---|"] + rep) + "\n")


if __name__ == "__main__":
    main()

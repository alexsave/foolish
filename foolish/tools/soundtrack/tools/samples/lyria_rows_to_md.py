#!/usr/bin/env python3
"""lyria_rows_to_md.py ROWS.tsv OUT.md - writes LYRIA_SAMPLES.md from the log rows of lyria_notes_extract.py.
Row columns: sample path, source, window start-end s, measured note and cents offset, resampling ratio, purity, what, attack, layer.
The table shows the file, source clip (a mp3 path is shortened to its clip name), window, note, cents corrected (minus the
measured offset), purity, attack and layer. Deterministic."""
import os, re, sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip()]
L = ["# Lyria sampler instruments", "",
     "These samples are cut from AI-generated (Lyria) clips, or from Demucs stems of them.",
     "They are used for the replication work only.",
     "Whether they may ship is undecided.",
     "The wavs live in `/Users/alex/Dev/soundtrack-samples/lyria/<instrument>/` and the manifests in `samples/manifests/lyria_<instrument>.txt`.",
     "Cents corrected is the pitch shift applied to land on equal temperament (the opposite of the measured offset).",
     "A window is in seconds of the original clip; for the second half of wa_22 it already includes the 15.0988 s offset.", "",
     "| file | source | window s | note | cents corrected | purity | attack | layer |", "|---|---|---|---|---|---|---|---|"]
for r in rows:
    path, src, win, note, ratio, pur, what, att, layer = r[:9]
    m = re.match(r"(\S+) ([+-][\d.]+) cents", note)
    src = os.path.splitext(os.path.basename(src))[0] if src.endswith(".mp3") else src
    L.append("| %s | %s | %s | %s | %+.1f | %s | %s | %s |" % (os.path.basename(path), src, win, m.group(1), -float(m.group(2)), pur, att, layer))
L.append("")
L.append("%d samples." % len(rows))
open(sys.argv[2], "w").write("\n".join(L) + "\n")

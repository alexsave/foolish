# build_song.py - assemble one song's score.txt from a scene list: ENGINE scenes copied out of an existing score, LYRIA scenes
#   converted from transcribed notes by ../transcribe/to_score.py. This is the "take the good from both" builder.
# Input: a scene list (plain text, one scene per line, fields separated by " | "; '#' starts a comment):
#   ENGINE | <source score.txt> | <section name prefix> | <tempo or keep> [| bars a-b[,c-d]]
#       copies that section (optionally only those 1-based bar ranges) and sets a bare TEMPO line before it (a step);
#       "keep" copies the source's own tempo lines for that section instead, ramps included.
#   LYRIA  | <notes.tsv> | <start s> | <end s> | <clip bpm> | <downbeat s or auto> | <transpose> | <tempo to play at> | <section name> | <part specs separated by ';'> [| <drums spec>]
#       part specs and the drums spec are passed to to_score.py as --part and --drums.
# Output: the score text on stdout or to --out, with a comment line before each scene saying where it came from.
# Run: python3 -I build_song.py scenes.txt --out ../../3_seoul_v6/score.txt
# Failure modes: a section name that matches nothing stops the build; a LYRIA scene is only as good as its transcription
#   and its downbeat (check to_score's report on stderr, which this passes through).
import sys, os, subprocess, re
HERE = os.path.dirname(os.path.abspath(__file__)); TO_SCORE = os.path.join(HERE, "..", "transcribe", "to_score.py")
META = ("DYN", "FX", "TRANSITION")
def expand(b):
    out = []
    for x in b:
        x = x.strip()
        if x == "%": out.append(out[-1])
        elif x.startswith("%"): out.append(out[int(x[1:]) - 1])
        else: out.append(x)
    return out
def engine_section(path, prefix, ranges):
    L = open(path).read().split("\n")
    hits = [k for k, l in enumerate(L) if l.startswith("SECTION " + prefix)]
    if len(hits) != 1: sys.exit(f"build_song: section '{prefix}' matches {len(hits)} sections in {path}")
    i = hits[0]; j = next((k for k in range(i + 1, len(L)) if L[k].startswith("SECTION ")), len(L))
    # the source's own tempo lines for this section: those between the previous section's last line and this header, and any
    # right after the header ("TEMPO 54 RAMP 16" inside a section is how the old scores write a ritardando)
    before = []; k = i - 1
    while k >= 0 and (not L[k].strip() or L[k].startswith("TEMPO") or L[k].startswith("#")):
        if L[k].startswith("TEMPO"): before.insert(0, L[k])
        k -= 1
    inside = [l for l in L[i + 1:j] if l.startswith("TEMPO")]
    trailing = 0   # tempo lines at the end of the block belong to the NEXT section
    for l in reversed([x for x in L[i + 1:j] if x.strip()]):
        if l.startswith("TEMPO"): trailing += 1
        else: break
    inside = inside[:len(inside) - trailing] if trailing else inside
    m = re.search(r" BARS (\d+) ", L[i]); n = int(m.group(1)); out = []
    engine_section.before = before; engine_section.inside = inside
    rs = ranges or [(1, n)]; n2 = sum(b - a + 1 for a, b in rs)
    out.append(L[i].replace(f" BARS {n} ", f" BARS {n2} "))
    for l in L[i + 1:j]:
        if not l.strip() or l.startswith("TEMPO") or l.startswith("#"): continue
        head = l.split(":")[0]
        if ": " not in l or head in META:
            if ranges and head == "DYN": continue   # bar-numbered dynamics no longer fit a cut section
            if ranges and l.startswith("FX: FILTER"): continue   # nor do bar-numbered filter sweeps
            out.append(l); continue
        part, body = l.split(": ", 1); b = expand(body.split(" | "))
        if len(b) != n: sys.exit(f"build_song: {prefix} / {part} has {len(b)} bars, expected {n}")
        out.append(part + ": " + " | ".join(x for a, z in rs for x in b[a - 1:z]))
    return out
def main():
    args = sys.argv[1:]; out_path = None
    if "--out" in args: k = args.index("--out"); out_path = args[k + 1]; del args[k:k + 2]
    lines = []; base = os.path.dirname(os.path.abspath(args[0]))
    for raw in open(args[0]):
        raw = raw.split("#")[0].strip()
        if not raw: continue
        f = [x.strip() for x in raw.split(" | ")]
        if f[0] == "ENGINE":
            src = f[1] if os.path.isabs(f[1]) else os.path.join(base, f[1]); ranges = None
            if len(f) > 4: ranges = [tuple(int(v) for v in r.split("-")) for r in f[4].replace("bars", "").strip().split(",")]
            sec = engine_section(src, f[2], ranges)
            if f[3] == "keep":   # the source's own tempo lines, ramps included, in their own places
                lines += [f"# scene: ENGINE {f[2]}" + (f" ({f[4]})" if len(f) > 4 else "")] + engine_section.before + [sec[0]] + engine_section.inside + sec[1:] + [""]
            else: lines += [f"# scene: ENGINE {f[2]}" + (f" ({f[4]})" if len(f) > 4 else ""), f"TEMPO {f[3]}"] + sec + [""]
        elif f[0] == "LYRIA":
            cmd = [sys.executable, "-I", TO_SCORE, "--notes", f[1], "--start", f[2], "--end", f[3], "--bpm", f[4], "--transpose", f[6], "--name", f[8], "--key", "x"]
            if f[5] != "auto": cmd += ["--downbeat", f[5]]
            for p in f[9].split(";"):
                if p.strip(): cmd += ["--part", p.strip()]
            if len(f) > 10 and f[10]: cmd += ["--drums", f[10]]
            r = subprocess.run(cmd, capture_output=True, text=True)
            sys.stderr.write(f"--- {f[8]}\n" + r.stderr)
            if r.returncode: sys.exit(f"build_song: to_score failed for {f[8]}")
            body = [l for l in r.stdout.split("\n") if l.strip() and not l.startswith("TEMPO")]
            lines += [f"# scene: LYRIA {os.path.basename(f[1])} {f[2]}-{f[3]} s, clip {f[4]} BPM, transposed {f[6]}", f"TEMPO {f[7]}"] + body + [""]
        else: sys.exit("build_song: unknown scene kind " + f[0])
    text = "\n".join(lines) + "\n"
    if out_path: open(out_path, "w").write(text)
    else: sys.stdout.write(text)
main()

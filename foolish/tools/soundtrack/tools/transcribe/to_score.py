#!/usr/bin/env python3
"""to_score.py - turn a transcribed-notes TSV (and optionally a drums stem) into one section of the engine's text score.

What it does: reads the note table written by window_report.py (columns stem, onset_s, duration_s, midi, name, velocity),
keeps the notes whose onset lies in [--start, --end) of the clip, lays a bar grid over them (--bpm, --downbeat, 4 beats a bar),
quantises onsets and lengths to --quantum beats, and writes the notes of each --part as lines of the text score read by
engine/scoretxt.h: a TEMPO line, a SECTION line with the right BARS count, an FX: comment line, and one PART: line per lane.
Every bar of every line is written to sum to exactly 4 beats (asserted). A note that crosses a bar line is written
"C4:1.5~6" (the cursor moves 1.5 beats to the bar line, the note sounds 6 beats). The same part on several lines is several
lanes. It makes no musical decisions: it only places measured notes on the grid. All choices are command-line arguments,
the output is deterministic.
Inputs: --notes TSV --start S --end E --bpm B [--downbeat T] [--quantum 0.25] --name NAME --key KEY [--transpose N]
  --part "SOURCE=PART[,range=C2-C4][,transpose=n][,minlen=beats][,maxlanes=n]"   (repeatable)
      SOURCE: bass | vocals | mix | other (all notes of that stem);
              other.top    the highest note of the stem at each quantised onset, one monophonic line (a note ends when the
                           next top note starts);
              other.rest   the notes of other that other.top did not take;
              other.chords notes of other that start within one quantum (raw onset difference below the quantum) of at
                           least two other notes of the stem, notes of equal onset and length written as one chord;
              other.low    the lowest note of the stem at each quantised onset, one monophonic line (the lowest line).
      range= folds notes by octaves into the range (a range narrower than an octave takes the nearest member of the pitch
      class, and clamps when none exists). minlen= (beats, default one quantum) lengthens shorter notes. maxlanes= (default 3):
      overlapping notes that cannot be one chord (different onsets or lengths) go to further lanes, the rest are dropped.
  --drums "STEMSDIR,low=KICK,mid=SNARE,high=HAT"  drums.wav in STEMSDIR is read; onsets in the window are classified by
      window_report.drum_onsets (low under 150 Hz, mid 150 Hz to 2 kHz, high above 5 kHz) and written as x hits, one part
      per class, velocity 1.0, 0.7 or 0.45 by the peak of the stem in the 50 ms after the onset (at least 2/3 of the
      class maximum, at least 1/3, below). Classes may be left out.
  --out FILE (default stdout)
  If --downbeat is omitted it is estimated and printed on stderr: the beat phase that puts the most bass onsets and low drum
  onsets within a tenth of a beat of a beat, then of its four beat positions the one holding most of those onsets is called
  the bar line (a guess; pass --downbeat to override).
  Velocity: the transcription velocity is mapped linearly within the part to 0.45..1.0, rounded to 0.05, "@" omitted at 0.8
  (a part whose notes all have one velocity gets 0.8). --quantum must be a power of two fraction of a beat (0.25, 0.5, 0.125 ...)
  so the lengths add up exactly.
Outputs: the score text (stdout or --out) and on stderr a report: notes per part, dropped notes, the share of onsets
  moved more than a third of a quantum by quantisation, pitch range per part before and after folding.
Example: to_score.py --notes notes/3_seoul/se_11_song_club_mix_0-end.tsv --start 0 --end 42 --bpm 160 --name "Club" --key "F#m" \\
  --part bass=BASS808 --part other.top=LEAD --part other.chords=PAD \\
  --drums "stems/htdemucs/se_11_song_club_mix,low=KICK,mid=SNARE,high=HAT" --out section.txt
Known failure modes: a wrong --bpm or --downbeat smears every onset (see the moved share); the transcription's ghost and missing
  notes pass straight through; notes that begin before --start are ignored even if they sound into the window; the engine reads
  only the first word of the KEY text; the section name must not contain " BARS ". Errors exit with status 2 and a message.
"""
import argparse
import math
import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SHARP = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
VEL_PLAIN = 0.8


class Fail(Exception):
    pass


def midi_name(m):
    return "%s%d" % (SHARP[m % 12], m // 12 - 1)


def name_midi(s):
    m = re.fullmatch(r"([A-Ga-g])([#b]*)(-?\d+)", s.strip())
    if not m:
        raise Fail("bad note name %r" % s)
    v = PC[m.group(1).upper()] + sum(1 if c == "#" else -1 for c in m.group(2))
    return v + 12 * (int(m.group(3)) + 1)


def fmt(x):
    return ("%.6f" % x).rstrip("0").rstrip(".")


def read_tsv(path):
    rows = []
    with open(path) as f:
        head = f.readline().rstrip("\n").split("\t")
        want = ["stem", "onset_s", "duration_s", "midi", "name", "velocity"]
        if head[:6] != want:
            raise Fail("%s: header %r, expected %r" % (path, head, want))
        for i, line in enumerate(f):
            p = line.rstrip("\n").split("\t")
            if len(p) < 6:
                continue
            rows.append(dict(i=i, stem=p[0], t=float(p[1]), dur=float(p[2]), midi=int(p[3]), vel=float(p[5])))
    return rows


def fold(m, lo, hi):
    if lo is None:
        return m
    while m < lo:
        m += 12
    while m > hi:
        m -= 12
    if m < lo:   # range narrower than an octave and the pitch class is not in it: nearest edge
        m = lo if abs(m - lo) <= abs(m - hi) else hi
    return m


def parse_part(spec):
    if "=" not in spec:
        raise Fail("bad --part %r" % spec)
    src, rest = spec.split("=", 1)
    bits = rest.split(",")
    p = dict(src=src, name=bits[0], lo=None, hi=None, tr=0, minlen=None, maxlanes=3)
    if src not in ("bass", "vocals", "mix", "other", "other.top", "other.rest", "other.chords", "other.low"):
        raise Fail("unknown source %r" % src)
    for b in bits[1:]:
        k, _, v = b.partition("=")
        if k == "range":
            # note names carry a minus for negative octaves only; "C2-C4" splits on the first dash after the first name
            m = re.fullmatch(r"([A-Ga-g][#b]*-?\d+)-([A-Ga-g][#b]*-?\d+)", v)
            if not m:
                raise Fail("bad range %r" % v)
            p["lo"], p["hi"] = name_midi(m.group(1)), name_midi(m.group(2))
            if p["lo"] > p["hi"]:
                raise Fail("empty range %r" % v)
        elif k == "transpose":
            p["tr"] = int(v)
        elif k == "minlen":
            p["minlen"] = float(v)
        elif k == "maxlanes":
            p["maxlanes"] = int(v)
        else:
            raise Fail("unknown part option %r" % k)
    return p


def estimate_downbeat(onsets, spb):
    """onsets: seconds of bass and low drum hits. Returns (downbeat_s, beat phase_s, score)."""
    if not len(onsets):
        raise Fail("no bass or low-drum onsets to estimate --downbeat from; pass --downbeat")
    on = np.asarray(onsets, float)
    best = None
    for ph in np.arange(0, spb, 0.005):
        d = (on - ph) / spb
        d = d - np.round(d)
        hit = np.abs(d) <= 0.1
        key = (int(hit.sum()), -float(np.abs(d[hit]).sum()))
        if best is None or key > best[0]:
            best = (key, ph)
    ph = best[1]
    idx = np.round((on - ph) / spb).astype(int)
    d = np.abs((on - ph) / spb - idx)
    cnt = [int(np.sum((idx % 4 == k) & (d <= 0.1))) for k in range(4)]
    k = int(np.argmax(cnt))
    return ph + k * spb, ph, best[0][0], cnt


class Ev:
    __slots__ = ("on", "ln", "pitches", "amp")

    def __init__(self, on, ln, pitches, amp):
        self.on, self.ln, self.pitches, self.amp = on, ln, pitches, amp


def select(part, rows, Q, spb, origin):
    """Notes of the part's source as dicts with beat onset (raw) and length (raw beats); no quantisation yet."""
    src = part["src"]
    stem = src.split(".")[0]
    ns = [r for r in rows if r["stem"] == stem]
    out = []
    for r in ns:
        out.append(dict(r, b=(r["t"] - origin) / spb, d=r["dur"] / spb, mono=None))
    if src in ("bass", "vocals", "mix", "other"):
        return out
    q = lambda b: math.floor(b / Q + 0.5)
    if src in ("other.top", "other.rest", "other.low"):
        by = {}
        for n in out:
            by.setdefault(q(n["b"]), []).append(n)
        pick = {}
        for k, g in by.items():
            if src == "other.low":
                pick[k] = min(g, key=lambda n: (n["midi"], -n["d"], n["i"]))
            else:
                pick[k] = max(g, key=lambda n: (n["midi"], n["d"], -n["i"]))
        if src == "other.rest":
            taken = {n["i"] for n in pick.values()}
            return [n for n in out if n["i"] not in taken]
        keys = sorted(pick)
        res = []
        for j, k in enumerate(keys):
            n = dict(pick[k])
            if j + 1 < len(keys):
                n["mono"] = (keys[j + 1] - k) * Q   # a note ends when the next one starts
            res.append(n)
        return res
    if src == "other.chords":
        res = []
        for n in out:
            others = sum(1 for m in out if m is not n and abs(m["b"] - n["b"]) < Q)
            if others >= 2:
                res.append(n)
        return res
    raise Fail("source %r" % src)


def build_part(part, rows, ctx):
    Q, S, nsteps, spb, origin, gtr = ctx["Q"], ctx["S"], ctx["nsteps"], ctx["spb"], ctx["origin"], ctx["transpose"]
    sel = select(part, rows, Q, spb, origin)
    stats = dict(selected=len(sel), written=0, dropped_lanes=0, dropped_outside=0, moved=0, onsets=0)
    if not sel:
        return [], stats, None, None
    minlen = part["minlen"] if part["minlen"] is not None else Q
    minsteps = max(1, int(round(minlen / Q)))
    amps = [n["vel"] for n in sel]
    lo_a, hi_a = min(amps), max(amps)

    def vmap(a):
        if hi_a - lo_a < 1e-9:
            return VEL_PLAIN
        return round((0.45 + 0.55 * (a - lo_a) / (hi_a - lo_a)) * 20) / 20

    before = [n["midi"] for n in sel]
    notes = {}   # (onset step, pitch) -> (len steps, amp)
    after = []
    for n in sel:
        on = int(math.floor(n["b"] / Q + 0.5))
        raw_on = n["b"] / Q
        stats["onsets"] += 1
        if abs(on - raw_on) * Q > Q / 3 + 1e-12:
            stats["moved"] += 1
        d = min(n["d"], n["mono"]) if n["mono"] is not None else n["d"]
        ln = max(minsteps, int(math.floor(d / Q + 0.5)))
        if n["mono"] is not None:
            ln = min(ln, max(1, int(round(n["mono"] / Q))))   # minlen never runs into the next top note
        if on < 0 or on >= nsteps:
            stats["dropped_outside"] += 1
            continue
        ln = min(ln, nsteps - on)
        p = fold(n["midi"] + gtr + part["tr"], part["lo"], part["hi"])
        after.append(p)
        k = (on, p)
        if k not in notes or notes[k][0] < ln:
            notes[k] = (ln, vmap(n["vel"]))
    groups = {}
    for (on, p), (ln, v) in notes.items():
        groups.setdefault((on, ln), []).append((p, v))
    evs = [Ev(on, ln, sorted(p for p, _ in g), max(v for _, v in g)) for (on, ln), g in groups.items()]
    evs.sort(key=lambda e: (e.on, -len(e.pitches), -e.ln, e.pitches))
    lanes = []   # each: [end step, events]
    for e in evs:
        for L in lanes:
            if e.on >= L[0]:
                L[0] = e.on + e.ln
                L[1].append(e)
                break
        else:
            if len(lanes) < part["maxlanes"]:
                lanes.append([e.on + e.ln, [e]])
            else:
                stats["dropped_lanes"] += len(e.pitches)
    stats["written"] = sum(len(e.pitches) for L in lanes for e in L[1])
    stats["lanes"] = len(lanes)
    return [L[1] for L in lanes], stats, (min(before), max(before)), ((min(after), max(after)) if after else None)


def render_lane(events, ctx, drum=False):
    """Bars (list of token lists) of one lane; asserts that each bar sums to S steps."""
    S, nsteps, Q = ctx["S"], ctx["nsteps"], ctx["Q"]
    nbars = nsteps // S
    bars = [[] for _ in range(nbars)]
    cur = 0

    def rest_to(target):
        nonlocal cur
        while cur < target:
            b = cur // S
            end = min(target, (b + 1) * S)
            bars[b].append(("r", end - cur))
            cur = end

    for e in events:
        rest_to(e.on)
        b = e.on // S
        bend = (b + 1) * S
        name = "+".join(midi_name(p) for p in e.pitches) if not drum else "x"
        v = "" if (abs(e.amp - VEL_PLAIN) < 1e-9 and not drum) else "@" + fmt(e.amp)
        if e.on + e.ln <= bend:
            bars[b].append((name + ":", e.ln, "", v))
            cur = e.on + e.ln
        else:
            bars[b].append((name + ":", bend - e.on, "~" + fmt(e.ln * Q), v))
            cur = bend
    rest_to(nsteps)
    out = []
    for b, toks in enumerate(bars):
        total = sum(t[1] for t in toks)
        assert total == S, "bar %d sums to %d steps, expected %d" % (b, total, S)
        s = []
        for t in toks:
            if t[0] == "r":
                s.append("r:" + fmt(t[1] * Q))
            else:
                s.append(t[0] + fmt(t[1] * Q) + t[2] + t[3])
        out.append(" ".join(s))
    return out


def pr(m):
    return "-" if m is None else "%s-%s" % (midi_name(m[0]), midi_name(m[1]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--notes", required=True)
    ap.add_argument("--start", type=float, required=True)
    ap.add_argument("--end", type=float, required=True)
    ap.add_argument("--bpm", type=float, required=True)
    ap.add_argument("--downbeat", type=float)
    ap.add_argument("--quantum", type=float, default=0.25)
    ap.add_argument("--name", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--transpose", type=int, default=0)
    ap.add_argument("--part", action="append", default=[])
    ap.add_argument("--drums")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    try:
        return run(a)
    except Fail as e:
        sys.stderr.write("to_score: %s\n" % e)
        return 2


def run(a):
    Q = a.quantum
    S = 4.0 / Q
    if Q <= 0 or abs(S - round(S)) > 1e-9 or (int(round(S)) & (int(round(S)) - 1)):
        raise Fail("--quantum %g must be 4 / a power of two (0.25, 0.5, 0.125 ...)" % Q)
    S = int(round(S))
    if " BARS " in a.name or "\n" in a.name:
        raise Fail("--name must not contain ' BARS '")
    if a.end <= a.start:
        raise Fail("--end must be after --start")
    spb = 60.0 / a.bpm
    rows = read_tsv(a.notes)
    drums = None
    if a.drums:
        bits = a.drums.split(",")
        drums = dict(dir=bits[0], map={})
        for b in bits[1:]:
            k, _, v = b.partition("=")
            if k not in ("low", "mid", "high") or not v:
                raise Fail("bad --drums option %r" % b)
            drums["map"][k] = v
    drum_ons = None
    if drums:
        import envelopes as env
        import window_report as wr
        x, sr = env.load_audio(os.path.join(drums["dir"], "drums.wav"))
        drum_ons = []
        for t, cls in wr.drum_onsets(x, sr, a.start, a.end):
            if a.start <= t < a.end:
                pk = float(np.max(np.abs(x[int(t * sr):int((t + 0.05) * sr)]))) if int(t * sr) < len(x) else 0.0
                drum_ons.append((t, cls, pk))
    inwin = [r for r in rows if a.start <= r["t"] < a.end]
    if a.downbeat is None:
        ons = [r["t"] for r in inwin if r["stem"] == "bass"]
        if drum_ons:
            ons += [t for t, cls, _ in drum_ons if "L" in cls]
        db, ph, hits, cnt = estimate_downbeat(ons, spb)
        sys.stderr.write("downbeat estimate: %.3f s (beat phase %.3f s, %d of %d onsets within 0.1 beat of a beat, "
                         "onsets per beat position %s)\n" % (db, ph, hits, len(ons), cnt))
    else:
        db = a.downbeat
    barlen = 4 * spb
    origin = db + math.floor((a.start - db) / barlen + 1e-9) * barlen
    nbars = int(math.ceil((a.end - origin) / barlen - 1e-9))
    nsteps = nbars * S
    ctx = dict(Q=Q, S=S, nsteps=nsteps, spb=spb, origin=origin, transpose=a.transpose)
    lines, fx, rep = [], [], []
    moved_all = [0, 0]
    for spec in a.part:
        part = parse_part(spec)
        lanes, st, rb, ra = build_part(part, inwin, ctx)
        moved_all[0] += st["moved"]
        moved_all[1] += st["onsets"]
        if not lanes:
            lanes = [[]]
        for ev in lanes:
            lines.append("%s: %s" % (part["name"], " | ".join(render_lane(ev, ctx))))
        fx.append("%s %s written %d dropped %d" % (part["name"], part["src"], st["written"], st["dropped_lanes"] + st["dropped_outside"]))
        rep.append("part %-10s %-13s selected %4d written %4d dropped %3d (lanes %d, outside grid %d) lanes used %d; "
                   "moved over a third of a quantum %d of %d (%.1f%%); pitch range before folding %s, after %s"
                   % (part["name"], part["src"], st["selected"], st["written"], st["dropped_lanes"] + st["dropped_outside"],
                      st["dropped_lanes"], st["dropped_outside"], st.get("lanes", 0), st["moved"], st["onsets"],
                      100.0 * st["moved"] / max(1, st["onsets"]), pr(rb), pr(ra)))
    if drum_ons is not None:
        for cname, letter in (("low", "L"), ("mid", "M"), ("high", "H")):
            if cname not in drums["map"]:
                continue
            hits = [(t, pk) for t, cls, pk in drum_ons if letter in cls]
            mx = max([pk for _, pk in hits], default=0.0)
            slots = {}
            moved = 0
            for t, pk in hits:
                raw = (t - origin) / spb / Q
                k = int(math.floor(raw + 0.5))
                if abs(k - raw) * Q > Q / 3 + 1e-12:
                    moved += 1
                if not 0 <= k < nsteps:
                    continue
                lvl = 1.0 if pk >= mx * 2 / 3 else 0.7 if pk >= mx / 3 else 0.45
                slots[k] = max(slots.get(k, 0), lvl)
            evs = [Ev(k, 1, [0], v) for k, v in sorted(slots.items())]
            lines.append("%s: %s" % (drums["map"][cname], " | ".join(render_lane(evs, ctx, drum=True))))
            fx.append("%s drums.%s hits %d" % (drums["map"][cname], cname, len(evs)))
            rep.append("part %-10s drums.%-5s onsets %4d hits written %4d (merged on one slot %d); moved over a third of a quantum "
                       "%d of %d (%.1f%%)" % (drums["map"][cname], cname, len(hits), len(evs), len(hits) - len(evs), moved, len(hits),
                                              100.0 * moved / max(1, len(hits))))
            moved_all[0] += moved
            moved_all[1] += len(hits)
    out = ["TEMPO %s" % fmt(a.bpm),
           "SECTION %s BARS %d KEY %s" % (a.name, nbars, a.key),
           "FX: clip %s, window %s to %s s, bpm %s, downbeat %.3f s, first bar line %.3f s, transposition %d, quantum %s; %s"
           % (os.path.basename(a.notes), fmt(a.start), fmt(a.end), fmt(a.bpm), db, origin, a.transpose, fmt(Q), "; ".join(fx))]
    out += lines
    text = "\n".join(out) + "\n"
    if a.out:
        with open(a.out, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    sys.stderr.write("section %s: %d bars, window %s to %s s, first bar line %.3f s\n" % (a.name, nbars, fmt(a.start), fmt(a.end), origin))
    for r in rep:
        sys.stderr.write(r + "\n")
    sys.stderr.write("all onsets: moved over a third of a quantum %d of %d (%.1f%%)\n"
                     % (moved_all[0], moved_all[1], 100.0 * moved_all[0] / max(1, moved_all[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())

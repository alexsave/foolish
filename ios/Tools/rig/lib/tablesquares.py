#!/usr/bin/env python3
"""Track every table pair, frame by frame, and fail on a jump.

    tablesquares.py <take-dir | movie.mp4> [--marks marks.txt] [--csv out.csv] [--jump PT]

The live table draws a coloured square at the centre of each pair's slot when
`dev.ruler` is on (`View.tableSquare`, CollapseRuler.swift): cyan, yellow, green,
repeating; `lib/squares.py` finds them. The horizontal ruler bars cannot see a throw-in - it re-centres the
row SIDEWAYS, and a bar through the table's centre does not move while both
pairs slide 36pt left. These squares move exactly when the layout does.

Reads the frames and `times.txt` that `lib/window.sh` writes. Prints one line
per pair and exits 1 when any pair jumped (see `is_jump`): further than
`--jump` points between neighbouring frames, or too fast across a gap. Exits 2 when there is nothing to
judge (no squares at all - is the ruler on, is this a DEBUG build?).

A SQUARE THAT IS PARTLY COVERED IS NOT A POSITION. A flying card passes over
the table; a square it clips has the wrong centre, so any shape that is not
square-sized is dropped for that frame. A pair that is missing for a while and
comes back somewhere else is still a jump (see `steps`).
"""
import argparse, glob, os, sys
from multiprocessing import Pool
import numpy as np
from PIL import Image

import tween

import squares as sq

def read_frame(p):
    return reading(np.asarray(Image.open(p).convert("RGB")))


def reading(a):
    """One frame: (its squares measured INSIDE THE DRAWER, the ruler's clock).

    IN THE DRAWER, NOT ON THE SCREEN. The first reel reported 30 "jumps", every
    one vertical and every one while a board was opening: Messages slides the
    drawer up, carrying the whole table with it, 24pt then 19 then 16 a frame.
    The second reported 8 more, every one SIDEWAYS and every one while the rig
    left the thread: Messages slides the whole conversation, drawer and all, off
    to the right. Both are the host's presentation, not our layout. So y is
    measured from the drawer's red top bar and x from that bar's left end, and a
    table that jumps inside a moving drawer still moves against both. A frame
    without the bar has no drawer to measure against, so its squares are not a
    position.

    The clock is the ruler's ms-mod-16384 strip, which places this frame on the
    device's own clock - see `clock_offset`.
    """
    box = tween.read_array(a)
    if not box or box.get("topoff") or "top_pt" not in box:
        return None, None, None
    top, left = box["top_pt"], box.get("left_pt", 0.0)
    return ([(n, round(float(x - left), 2), round(float(y - top), 2))
             for n, x, y in sq.squares_in(a)],
            box.get("clock"), top)


def clock_offset(rec0, times, clocks):
    """Seconds from the recorder being STARTED to the movie's first frame.

    The reel's marks are wall-clock seconds from starting the recorder, and the
    movie's clock starts at its first frame - about four seconds later on this
    Mac, which filed the first reel's anomalies under the scenario BEFORE the one
    they happened in. The ruler draws the device's milliseconds mod 16384 on
    every frame, so each readable frame says what the wall clock was: the
    offset is the one delay that makes (start + t + delay) land on that value.
    The median over every readable frame shrugs off a misread cell.
    """
    ds = []
    for t, c in zip(times, clocks):
        if c is None:
            continue
        ds.append((c - (rec0 * 1000.0 + t * 1000.0)) % 16384)
    return float(np.median(ds)) / 1000.0 if ds else None


def from_movie(movie):
    """(squares per frame, time per frame) straight off a movie, no frames on disk.

    A reel is a minute of 1320x2868 frames: written out as PPM that is ~40GB.
    So the movie is decoded down a pipe, one frame at a time, and `showinfo` in
    the SAME run logs each frame's time (lib/window.sh has why it must be the
    same run).
    """
    import subprocess, tempfile
    w, h = (int(v) for v in subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0", movie],
        capture_output=True, text=True, check=True).stdout.strip().split(","))
    with tempfile.TemporaryFile() as err:
        p = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-nostats", "-v", "info", "-copyts", "-i", movie,
             "-vf", "showinfo", "-fps_mode", "passthrough",
             "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"],
            stdout=subprocess.PIPE, stderr=err)
        seen, n = [], w * h * 3
        while True:
            buf = p.stdout.read(n)
            if len(buf) < n:
                break
            seen.append(reading(np.frombuffer(buf, np.uint8).reshape(h, w, 3)))
        p.wait()
        err.seek(0)
        log = err.read().decode("utf-8", "replace")
    import re
    times = [float(t) for t in re.findall(r"showinfo.* pts_time:(\S+)", log)]
    if len(times) != len(seen):
        sys.exit("%s: %d frames decoded but %d times logged" % (movie, len(seen), len(times)))
    return seen, times


def track(frames_seen, times, link_pt=80.0):
    """Squares -> pairs. Same colour, nearest to where that pair last was."""
    tracks = []                      # dicts: colour, samples [(i, t, x, y)]
    for i, seen in enumerate(frames_seen):
        taken = set()
        for name, x, y in seen:
            best, bd = None, link_pt
            for k, tr in enumerate(tracks):
                if tr["colour"] != name or k in taken:
                    continue
                _, _, px, py = tr["samples"][-1]
                d = ((x - px) ** 2 + (y - py) ** 2) ** 0.5
                if d < bd:
                    best, bd = k, d
            if best is None:
                tracks.append({"colour": name, "samples": []})
                best = len(tracks) - 1
            taken.add(best)
            tracks[best]["samples"].append((i, times[i], x, y))
    return tracks


REPEAT_S = 0.012         # a frame this soon after the last, in the same place, is a repeat


def without_repeats(samples):
    """Drop a sample that is a REPEAT of the one before: the recorder writes
    some composited frames twice, ~5ms apart, with nothing moved. Left in, a
    repeat is a zero step beside a real one, and the real one reads as a
    spike (a collapse filmed 581, 581, 609 - two steps of 0 and 28 where the
    card moved 14 and 14)."""
    out = []
    for smp in samples:
        if out and smp[1] - out[-1][1] < REPEAT_S and abs(smp[2] - out[-1][2]) < 0.5 \
                and abs(smp[3] - out[-1][3]) < 0.5:
            continue
        out.append(smp)
    return out


def steps(samples):
    """Every move between two samples of one pair: (from, to, dx, dy, dt).

    Consecutive samples, whether or not frames were missed between them: a pair
    hidden under a flying card that reappears 36pt away did not slide there,
    and skipping the gap is how a jump gets scored as nothing.
    """
    return [(a[0], b[0], b[2] - a[2], b[3] - a[3], b[1] - a[1])
            for a, b in zip(samples, samples[1:])]


NEAR_S = 0.040            # two samples this close are neighbouring frames
DRAWER_PT = 20.0          # the box's own top moving this much in a frame is a collapse
GAP_SPEED = 400.0         # pt/s: across a longer gap, faster than this is a jump


SPIKE = 2.5              # a jump is this many times the steps either side of it


def is_jump(dist, dt, jump_pt, before=0.0, after=0.0, frames=1):
    """A jump is a STEP THAT STANDS OUT, not a fast one.

    Distance alone flagged every collapse: the table rides the drawer down at
    up to ~27pt a frame, smoothly - 17, 13, 12, 10 - and a threshold cannot
    tell that from the throw-in's 35pt between two frames of stillness. What
    tells them apart is the neighbours: a slide's steps are like the ones
    either side of it, a jump's are not. So a step over `jump_pt` between
    neighbouring frames is a jump only when it is SPIKE times the larger of
    the steps before and after it.

    Across a gap (the square covered by a flying card) there are no
    neighbours to compare, so speed decides: the first sample after 92ms
    under the ace was 7.8pt on (85pt/s, a slide), a 35pt jump hidden for the
    same 92ms is 380pt/s.
    """
    if dist <= jump_pt:
        return False
    if frames <= 2 and dt <= NEAR_S:          # neighbours: at most one repeat between
        return dist > SPIKE * max(before, after)
    return dist / max(dt, 1e-6) > GAP_SPEED


HANDOFF_S = 0.040        # two frames: a flight may appear a frame after its card goes


COVER_PT, COVER_S = 90.0, 0.15


def covered_by_flight(seen, times, i):
    """Whether the table squares missing in frame i vanished where a flight was
    just seen. A card landing ON a pair (a cover) or passing over one lays its
    orange square across the pair's, and for a few frames neither reads as a
    whole square - filmed on a replayed cover, 52ms of neither."""
    gone = [(n, x, y) for n, x, y in seen[i - 1] if n in sq.TABLE
            and not any(m == n and abs(x - u) < 20 and abs(y - v) < 20 for m, u, v in seen[i])]
    for k in range(i - 1, -1, -1):
        if times[i] - times[k] > COVER_S:
            break
        for m, u, v in seen[k]:
            if m == sq.FLIGHT and any(((u - x) ** 2 + (v - y) ** 2) ** 0.5 < COVER_PT for _, x, y in gone):
                return True
    return False


def handoff_gaps(seen, boxed, times):
    """Every table card that left the table before its flight existed.

    A card leaves the table only by flying (a sweep, a pickup, an undo), and a
    flying card carries an ORANGE square. So a frame whose table squares drop
    by one or more, with no orange square in it, starts a gap that lasts until
    one appears. Filmed bugs this catches: an undone throw-in gone 108ms before
    its flight home (the live table won over the sweep), and an undone first
    attack drawn at the bottom of the drawer for two frames (87ms) - the square
    cut off by the screen edge, so no square at all. A frame with no drawer to
    measure against (opening, collapsing) is not a disappearance.
    Returns [(time the card was last seen, gap seconds)].
    """
    out = []
    table = lambda q: sum(1 for n, _, _ in q if n in sq.TABLE)
    orange = lambda q: any(n == sq.FLIGHT for n, _, _ in q)
    i = 1
    while i < len(seen):
        if boxed[i] and boxed[i - 1] and table(seen[i]) < table(seen[i - 1]) and not orange(seen[i]) \
                and not covered_by_flight(seen, times, i):
            j = i
            back = False
            while j < len(seen) and not orange(seen[j]) and times[j] - times[i - 1] < 1.0:
                # COVERED, NOT GONE: a dragged card passing over a pair hides its
                # square, and the square comes back before anything flies.
                if boxed[j] and table(seen[j]) >= table(seen[i - 1]):
                    back = True
                    break
                j += 1
            if not back and j < len(seen) and orange(seen[j]):
                gap = times[j] - times[i - 1]
                if gap > HANDOFF_S:
                    out.append((times[i - 1], gap))
            i = j
        i += 1
    return out


def load_marks(path):
    """`seconds name` per line: when each scenario of a reel began, in seconds
    from starting the recorder; a `rec0 EPOCH` line says when that was."""
    out, rec0 = [], None
    for line in open(path):
        t, _, name = line.strip().partition(" ")
        if t == "rec0":
            rec0 = float(name)
        elif t:
            out.append((float(t), name))
    return sorted(out), rec0


def scene_at(marks, t):
    name = "-"
    for mt, mn in marks:
        if mt <= t:
            name = mn
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("take", help="a take directory (frames + times.txt) or a movie")
    ap.add_argument("--csv")
    ap.add_argument("--jump", type=float, default=12.0,
                    help="points between neighbouring frames that count as a jump")
    ap.add_argument("--marks", help="`seconds name` lines naming each scenario")
    ap.add_argument("--setup-prefix", default=None,
                    help="marks starting with this are the rig setting a board up "
                         "(seeding, dragging the drawer open): reported, not scored")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    if os.path.isfile(a.take):
        seen, times = from_movie(a.take)
    else:
        frames = sorted(glob.glob(os.path.join(a.take, "f*.ppm"))) \
            or sorted(glob.glob(os.path.join(a.take, "f*.png")))
        if not frames:
            sys.exit("no frames in %s" % a.take)
        times = tween.frame_times(a.take, len(frames))
        with Pool() as pool:
            seen = pool.map(read_frame, frames, chunksize=8)
    clocks = [c for _, c, _ in seen]
    tops = [t for _, _, t in seen]
    boxed = [q is not None for q, _, _ in seen]
    seen = [q or [] for q, _, _ in seen]
    marks, rec0 = load_marks(a.marks) if a.marks else ([], None)
    t0 = times[0] if times else 0.0
    if marks and rec0 is not None:
        d = clock_offset(rec0, times, clocks)
        if d is not None:
            print("marks aligned by the ruler clock: the movie starts %.2fs after the "
                  "recorder did" % d)
            marks = [(mt - d, mn) for mt, mn in marks]
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("frame,t,colour,x_in_box_pt,y_in_box_pt\n")
            for i, s in enumerate(seen):
                for name, x, y in s:
                    fh.write("%d,%.4f,%s,%.2f,%.2f\n" % (i + 1, times[i], name, x, y))
    gaps = handoff_gaps(seen, boxed, times)
    tracks = [t for t in track(seen, times) if len(t["samples"]) >= 3]
    if not tracks:
        print("NO TABLE SQUARES IN ANY FRAME - is `rig.sh ruler on` set, and is "
              "this a DEBUG build?", file=sys.stderr)
        sys.exit(2)
    jumps, anomalies, drawer_steps = 0, [], 0
    print("%-8s %6s %8s %8s %9s %9s  %s" % ("pair", "frames", "x from", "x to",
                                            "max step", "sum sq", "jumps"))
    for tr in sorted(tracks, key=lambda t: t["samples"][0][2]):
        st = steps(without_repeats(tr["samples"]))
        mags = [(dx * dx + dy * dy) ** 0.5 for _, _, dx, dy, _ in st]
        near = [m if (f1 - f0 <= 2 and dt <= NEAR_S) else 0.0
                for (f0, f1, _, _, dt), m in zip(st, mags)]
        big = [(f0, f1, m) for k, ((f0, f1, _, _, dt), m) in enumerate(zip(st, mags))
               if is_jump(m, dt, a.jump,
                          before=near[k - 1] if k > 0 else 0.0,
                          after=near[k + 1] if k + 1 < len(near) else 0.0,
                          frames=f1 - f0)]
        # A flying card is SUPPOSED to cover ground fast; its square is kept for
        # plotting (the CSV) and never scored as a jump.
        # Nor is a HAND card's: selecting one lifts it and a drag is a finger.
        if tr["colour"] in (sq.FLIGHT, sq.HAND):
            big = []
        # THE DRAWER ITSELF MOVING. In a frame where the box's top moved more
        # than DRAWER_PT, a step measured inside it is the collapse's own frame
        # pacing (tween.py and mse.py measure that), not the table's layout.
        moving = [(f0, f1, m) for f0, f1, m in big
                  if tops[f0] is not None and tops[f1] is not None
                  and abs(tops[f1] - tops[f0]) > DRAWER_PT]
        drawer_steps += len(moving)
        big = [b for b in big if b not in moving]
        jumps += len(big)
        for f0, f1, m in big:
            t = times[f1] - t0
            anomalies.append((t, "t=%6.2fs  %-22s %-6s f%d->f%d  %.1fpt in %.0fms" % (
                t, scene_at(marks, t), tr["colour"], f0 + 1, f1 + 1, m,
                1000 * (times[f1] - times[f0]))))
        sm = tr["samples"]
        print("%-8s %6d %8.1f %8.1f %9.1f %9.1f  %s" % (
            tr["colour"], len(sm), sm[0][2], sm[-1][2], max(mags) if mags else 0.0,
            sum(m * m for m in mags),
            ", ".join("f%d->f%d %.1fpt" % (f0 + 1, f1 + 1, m) for f0, f1, m in big) or "-"))
    for tg, gap in gaps:
        t = tg - t0
        anomalies.append((t, "t=%6.2fs  %-22s card left the table %.0fms before its flight "
                          "existed" % (t, scene_at(marks, t), 1000 * gap)))
    # THE RIG'S OWN HANDS ARE NOT THE APP. While a board is being set up the rig
    # drags the drawer open by hand, and a finger-driven drag moves the table in
    # uneven steps that are nobody's animation.
    if drawer_steps:
        print("\n(%d step(s) while the drawer itself moved, not scored)" % drawer_steps)
    if a.setup_prefix:
        setup = [x for x in anomalies if scene_at(marks, x[0]).startswith(a.setup_prefix)]
        anomalies = [x for x in anomalies if not scene_at(marks, x[0]).startswith(a.setup_prefix)]
        if setup:
            print("\n(%d during setup, not scored)" % len(setup))
    if anomalies:
        print("\nANOMALIES: %d, named by scenario" % len(anomalies))
        for _, line in sorted(anomalies):
            print("  " + line)
        sys.exit(1)
    print("\nsmooth: no pair moved more than %.0fpt between frames" % a.jump)


if __name__ == "__main__":
    main()

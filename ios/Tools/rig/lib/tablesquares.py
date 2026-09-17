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
        return [], None
    top, left = box["top_pt"], box.get("left_pt", 0.0)
    return ([(n, round(x - left, 2), round(y - top, 2)) for n, x, y in sq.squares_in(a)],
            box.get("clock"))


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


def steps(samples):
    """Every move between two samples of one pair: (from, to, dx, dy, dt).

    Consecutive samples, whether or not frames were missed between them: a pair
    hidden under a flying card that reappears 36pt away did not slide there,
    and skipping the gap is how a jump gets scored as nothing.
    """
    return [(a[0], b[0], b[2] - a[2], b[3] - a[3], b[1] - a[1])
            for a, b in zip(samples, samples[1:])]


NEAR_S = 0.040            # two samples this close are neighbouring frames
GAP_SPEED = 250.0         # pt/s: across a longer gap, faster than this is a jump


def is_jump(dist, dt, jump_pt):
    """A jump is DISTANCE between neighbouring frames, SPEED across a gap.

    Speed alone does not work: the recorder writes frames 3ms apart, and a
    2pt step of a perfectly smooth slide is 700pt/s over 3ms. Distance alone
    does not work either: the thrown card flies over a pair's square, and the
    first sample after 92ms under it was 7.8pt on - 85pt/s, a slide. A 35pt
    jump hidden for the same 92ms is 380pt/s. (Hidden for 200ms it would be
    175 and pass - the limit of what a covered square can say.)
    """
    if dist <= jump_pt:
        return False
    return dt <= NEAR_S or dist / max(dt, 1e-6) > GAP_SPEED


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
    ap.add_argument("--jump", type=float, default=8.0,
                    help="points between neighbouring frames that count as a jump")
    ap.add_argument("--marks", help="`seconds name` lines naming each scenario")
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
    clocks = [c for _, c in seen]
    seen = [q for q, _ in seen]
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
    tracks = [t for t in track(seen, times) if len(t["samples"]) >= 3]
    if not tracks:
        print("NO TABLE SQUARES IN ANY FRAME - is `rig.sh ruler on` set, and is "
              "this a DEBUG build?", file=sys.stderr)
        sys.exit(2)
    jumps, anomalies = 0, []
    print("%-8s %6s %8s %8s %9s %9s  %s" % ("pair", "frames", "x from", "x to",
                                            "max step", "sum sq", "jumps"))
    for tr in sorted(tracks, key=lambda t: t["samples"][0][2]):
        st = steps(tr["samples"])
        mags = [(dx * dx + dy * dy) ** 0.5 for _, _, dx, dy, _ in st]
        big = [(f0, f1, m) for (f0, f1, _, _, dt), m in zip(st, mags)
               if is_jump(m, dt, a.jump)]
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
    if jumps:
        print("\nJUMP: %d step(s) over %.0fpt - a table pair moved without sliding"
              % (jumps, a.jump))
        for _, line in sorted(anomalies):
            print("  " + line)
        sys.exit(1)
    print("\nsmooth: no pair moved more than %.0fpt between frames" % a.jump)


if __name__ == "__main__":
    main()

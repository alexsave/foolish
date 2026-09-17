#!/usr/bin/env python3
"""Track every table pair, frame by frame, and fail on a jump.

    tablesquares.py <take-dir | movie.mp4> [--marks marks.txt] [--csv out.csv] [--jump PT]

The live table draws a coloured square at the centre of each pair's slot when
`dev.ruler` is on (`View.tableSquare`, CollapseRuler.swift): cyan, yellow, green,
repeating. The horizontal ruler bars cannot see a throw-in - it re-centres the
row SIDEWAYS, and a bar through the table's centre does not move while both
pairs slide 36pt left. These squares move exactly when the layout does.

Reads the frames and `times.txt` that `lib/window.sh` writes. Prints one line
per pair and exits 1 when any pair jumped (see `is_jump`): further than
`--jump` points between neighbouring frames, or too fast across a gap. Exits 2 when there is nothing to
judge (no squares at all - is the ruler on, is this a DEBUG build?).

THE MAGENTA BAR RUNS THROUGH THE SQUARES. On a single row the table's centre
line is the pairs' centre line, so each square comes out as two halves with a
4pt magenta stripe between them. The halves are joined by a vertical dilation
before labelling, and the square is measured on its RAW pixels, whose outer
edges are the square's own - so the centre is exact either way.

A SQUARE THAT IS PARTLY COVERED IS NOT A POSITION. A flying card passes over
the table; a square it clips has the wrong centre, so any shape that is not
square-sized is dropped for that frame. A pair that is missing for a while and
comes back somewhere else is still a jump (see `steps`).
"""
import argparse, glob, os, sys
from multiprocessing import Pool
import numpy as np
from PIL import Image
from scipy import ndimage

import tween

SIDE_PT = 12.0            # CollapseRuler.squareSide
BAR_PT = 4.0              # CollapseRuler.edge - the magenta bar's thickness
# The ruler's band strip down the left edge is cyan/magenta/yellow cells of
# about this size; nothing on the table comes near it.
STRIP_PX = 80


# 30px at 3x (10pt), under the square's 36px, so a grid point always lands
# inside a WHOLE square. Owner: "if the squares are 36 pixels you might as well
# use 30 pixel strides."
#
# A square the magenta bar runs through is not whole: it is two 12px halves
# with the 12px bar between them, and a 30px grid can step over all three. So
# the rows immediately above and below the bar are scanned as well - every
# square the bar cuts has a half on both of them.
STRIDE_PT = 10.0


def colour_of(px):
    """Which square colour an (r, g, b) pixel is, or None."""
    r, g, b = (int(v) for v in px)
    hi, lo = 140, 90
    if g > hi and b > hi and r < lo:
        return "cyan"
    if r > hi and g > hi and b < lo:
        return "yellow"
    if g > hi and r < lo and b < lo:
        return "green"
    return None


def mask(win, name):
    r, g, b = (win[:, :, i].astype(np.int16) for i in range(3))
    hi, lo = 140, 90
    if name == "cyan":
        return (g > hi) & (b > hi) & (r < lo)
    if name == "yellow":
        return (r > hi) & (g > hi) & (b < lo)
    return (g > hi) & (r < lo) & (b < lo)


def bar_edges(a, s):
    """The rows just outside the magenta bar, found down two columns (one of
    them may be under a flying card)."""
    h, w = a.shape[0], a.shape[1]
    out = set()
    for x in (w // 2, w - 40):
        col = a[:, x, :].astype(np.int16)
        ys = np.nonzero((col[:, 0] > 140) & (col[:, 2] > 140) & (col[:, 1] < 90))[0]
        for run in np.split(ys, np.nonzero(np.diff(ys) > 2)[0] + 1) if len(ys) else []:
            if abs(len(run) - BAR_PT * s) <= 3:
                out.update((int(run[0]) - 3, int(run[-1]) + 3))
    return {y for y in out if 0 <= y < h}


def read_frame(p):
    return reading(np.asarray(Image.open(p).convert("RGB")))


def reading(a):
    """One frame: its squares, with y measured from the BOX'S TOP EDGE.

    IN THE DRAWER, NOT ON THE SCREEN. The first reel reported 30 "jumps", every
    one of them vertical and every one while a board was opening: Messages
    slides the drawer up, carrying the whole table with it, 24pt then 19 then
    16 a frame. That is the host's presentation, not our layout, and a table
    that jumps inside a moving drawer still moves relative to the drawer's own
    top bar (tween.py's red bar). A frame without that bar has no box to
    measure against, so its squares are not a position.
    """
    box = tween.read_array(a)
    if not box or box.get("topoff") or "top_pt" not in box:
        return []
    top = box["top_pt"]
    return [(n, x, round(y - top, 2)) for n, x, y in squares_in(a)]


def squares_in(a):
    """Every whole square in one frame, as (colour, x_pt, y_pt).

    A GRID, NOT A SCAN. Owner: "scan strides of 20 px in a grid" - and then
    "you might as well use 30 pixel strides" for a 36px square. The grid is
    classified in one vectorised pass; only the neighbourhood of a hit is then
    looked at closely.
    """
    h, w = a.shape[0], a.shape[1]
    s = 3 if h >= 2000 else 2
    side = SIDE_PT * s
    reach = int(2 * side)
    stride = int(round(STRIDE_PT * s))
    ys_ = np.array(sorted(set(range(stride // 2, h, stride)) | bar_edges(a, s)))
    xs_ = np.arange(STRIP_PX, w, stride)
    grid = a[ys_][:, xs_]
    hits = []
    for name in ("cyan", "yellow", "green"):
        gy, gx = np.nonzero(mask(grid, name))
        hits += [(name, int(xs_[j]), int(ys_[i])) for i, j in zip(gy, gx)]
    out, done = [], []
    for name, x, y in hits:
        if any(n == name and x0 <= x <= x1 and y0 <= y <= y1 for n, x0, y0, x1, y1 in done):
            continue
        wx0, wy0 = max(STRIP_PX, x - reach), max(0, y - reach)
        win = a[wy0:min(h, y + reach), wx0:min(w, x + reach)]
        m = mask(win, name)
        # Join the halves across the magenta bar (plus a pixel of h264 blur).
        joined = ndimage.binary_dilation(
            m, structure=np.ones((int(BAR_PT * s) + 3, 1), dtype=bool))
        lab, _ = ndimage.label(joined)
        k = lab[y - wy0, x - wx0]
        if k == 0:
            continue
        ys, xs = np.nonzero(m & (lab == k))
        x0, x1, y0, y1 = xs.min() + wx0, xs.max() + wx0, ys.min() + wy0, ys.max() + wy0
        done.append((name, x0, y0, x1, y1))
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        # Square-sized and square-shaped, or it is something else (a bar, a
        # glyph, a square half under a flying card, one cut by the window).
        if not (0.75 * side <= bw <= 1.25 * side and 0.75 * side <= bh <= 1.25 * side):
            continue
        # Most of the box is the colour: the bar may take 4pt of 12.
        if len(xs) < 0.45 * side * side:
            continue
        out.append((name, round((x0 + x1) / 2.0 / s, 2), round((y0 + y1) / 2.0 / s, 2)))
    return out


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
    """`seconds name` per line: when each scenario of a reel began."""
    out = []
    for line in open(path):
        t, _, name = line.strip().partition(" ")
        if t:
            out.append((float(t), name))
    return sorted(out)


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
    marks = load_marks(a.marks) if a.marks else []
    t0 = times[0] if times else 0.0
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("frame,t,colour,x_pt,y_in_box_pt\n")
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

#!/usr/bin/env python3
"""Read the collapse ruler out of a filmed take, frame by frame.

`CollapseRuler` (FoolishKit) draws, behind `dev.ruler`, on the surface's SIZED
BOX - so it measures `boxHeight`, the number the tween actually moves, not the
drawer:

    red bar      the box TOP        full width, 4pt
    green bar    the box BOTTOM     full width, 4pt
    banded strip 10pt bands from the top, 18pt wide, pure primaries
    clock        14 white/black cells, 12pt, ms mod 16384, under the top bar

Do NOT try to find the box from the board's own pixels. The ruler's own file
says why: the collapse is composited by Messages from snapshots of our view over
featureless wool, so "where is the top of our box" is not answerable from
content - and the one landmark that is not wool (the hand) is exactly the thing
whose position is in question.

Two things this reports that a geometry trace alone cannot:

  CLOCK REPEATS. The strip is redrawn by `TimelineView(.animation)` on every
  display refresh, so it freezes on frames we did not draw. A frame whose clock
  repeats while the geometry moves is the HOST interpolating a stale snapshot,
  and no animation curve can fix a frame the app never rendered.

  BAND PITCH. Bands are 10pt apart by construction, so a pitch that is not 10
  means the imagery was SCALED on its way to the screen.

    tween.py <take-dir> [--csv out.csv] [--quiet]
"""
import argparse, glob, os, sys
from multiprocessing import Pool
import numpy as np
from PIL import Image

BAND, CELL, BITS, EDGE = 10.0, 12.0, 14, 4.0


def rows_of(mask, w, frac=0.55):
    """Row indices where `mask` covers at least `frac` of the width."""
    return np.nonzero(mask.sum(axis=1) > w * frac)[0]


def runs(idx, gap=2):
    if not len(idx):
        return []
    return np.split(idx, np.nonzero(np.diff(idx) > gap)[0] + 1)


def read_frame(p):
    """The box's two edges, the band pitch and the clock, from one frame.

    CENTRE COLUMN FIRST, then verify the row. The two bars that matter are FULL
    WIDTH, so a row that does not match at the centre cannot be one of them -
    and one column is ~1.6ms against ~18ms for a whole-frame mask. The verify
    step is what keeps it honest: a card's red suit glyph sits in that column
    too, and only a real bar covers most of the width.
    """
    a = np.asarray(Image.open(p).convert("RGB"))
    h, w = a.shape[0], a.shape[1]
    # Scale from the HEIGHT. The frames are CROPPED to a narrow left-hand slice
    # (cmd_tween), so width no longer says what device this is; height is
    # untouched by that crop.
    s = 3 if h >= 2000 else 2
    col = a[:, w // 2, :].astype(np.int16)
    cr, cg, cb = col[:, 0], col[:, 1], col[:, 2]

    def verify(ys, chan):
        """First candidate row that is really a full-width bar."""
        for y in ys:
            row = a[int(y)].astype(np.int16)
            r_, g_, b_ = row[:, 0], row[:, 1], row[:, 2]
            if chan == "r":
                m = (r_ > 140) & (g_ < 90) & (b_ < 90)
            elif chan == "g":
                m = (g_ > 140) & (r_ < 90) & (b_ < 90)
            elif chan == "m":                        # magenta - the table mark
                m = (r_ > 140) & (b_ > 140) & (g_ < 90)
            else:                                    # yellow
                m = (r_ > 140) & (g_ > 140) & (b_ < 90)
            # 0.55 of the CROP, not of the screen - the bars span the whole
            # frame either way, and a card's suit glyph still cannot.
            if m.mean() > 0.55:
                return int(y), m
        return None, None

    top, topm = verify(np.nonzero((cr > 140) & (cg < 90) & (cb < 90))[0], "r")
    bot, _ = verify(np.nonzero((cg > 140) & (cr < 90) & (cb < 90))[0][::-1], "g")
    # ONE BAR MISSING MEANS TWO DIFFERENT THINGS AND ONLY ONE OF THEM IS A BUG.
    #
    #   no BOTTOM bar - the box was drawn taller than the screen and its bottom
    #   edge, with the hand on it, went off below. That is the failure the
    #   collapse's `hostLead` exists to prevent, and scoring it as a missing
    #   frame reads it as an improvement, because the frames that go wrong are
    #   exactly the ones that stop being counted.
    #
    #   no TOP bar - the box's top went off above. Under the slide
    #   (CollapseTween.slideDuration) that is the DESIGN: the box is held at its
    #   expanded height and translated up, so its top leaves the screen and the
    #   drawer clips it. Nothing is cut off that anyone can see, and the bottom
    #   edge - the only one under judgement - is right there in the frame.
    #
    # So report whichever bars are there, say which is missing, and leave height
    # empty when it cannot be known rather than dropping the frame.
    # OFF THE FRAME IS A POSITION, NOT A GAP. Owner's rule: a bar that has left
    # the screen is scored at the edge it left by - red at 0, green at the
    # phone's full height - rather than dropped. Dropping was actively
    # misleading: the frames where a bar leaves the screen are the worst frames
    # there are, and a metric that skips them rewards the changes that cause
    # them. Clamping also keeps every frame in the series, so the frame-to-frame
    # jerk score has no holes to diff across - a hole reads as one enormous step
    # that is really just a missing sample.
    #
    # The two flags stay, because WHICH bar left still matters: no green is the
    # hand cut off below the screen, the one failure the collapse's `hostLead`
    # exists to prevent; no red is the box top above the screen, which under the
    # slide is the design and is clipped by the drawer.
    # The two MARKER bars, pinned to the views rather than to the box: blue
    # through the table cards' centre, yellow through the first opponent's. They
    # are the only way to answer "did the table move smoothly" now that the box's
    # own top is not a line anything is drawn at. Absent on an older take and on
    # any frame where the view is off screen, so both are optional throughout.
    tab, _ = verify(np.nonzero((cr > 140) & (cb > 140) & (cg < 90))[0], "m")
    opp, _ = verify(np.nonzero((cr > 140) & (cg > 140) & (cb < 90))[0], "y")
    if top is None and bot is None:
        return None
    screen_pt = h / s
    out = {}
    if tab is not None:
        out["table_pt"] = round(tab / s, 1)
    if opp is not None:
        out["opp_pt"] = round(opp / s, 1)
    if top is None:
        out["topoff"] = True
        out["top_pt"] = 0.0
    if bot is None:
        out["offscreen"] = True
        out["bot_pt"] = round(screen_pt, 1)
    if bot is not None:
        out["bot_pt"] = round(bot / s, 1)
    if top is None:
        out["h_pt"] = round(out["bot_pt"], 1)
        return out
    out["top_pt"] = round(top / s, 1)
    out["h_pt"] = round(out["bot_pt"] - out["top_pt"], 1)
    # The box's own left edge, off the bar we just verified.
    xs_ = np.nonzero(topm)[0]
    bx = int(xs_[0]) if len(xs_) else 0
    # Band pitch, from a NARROW vertical strip at the box's leading edge - cyan
    # only, because the bands alternate cyan/magenta and are adjacent, so a mask
    # of both merges the strip into one run.
    strip = a[:, bx:bx + int(18 * s), :].astype(np.int16)
    sr, sg, sb = strip[:, :, 0], strip[:, :, 1], strip[:, :, 2]
    cyan = ((sg > 130) & (sb > 130) & (sr < 90)).mean(axis=1) > 0.5
    ys = sorted(int(np.mean(x)) for x in runs(np.nonzero(cyan)[0]))
    out["pitch_pt"] = round(float(np.median(np.diff(ys))) / s, 2) if len(ys) > 2 else None
    # The clock: 14 cells under the top bar, from the box's leading edge.
    cyr = top + int((EDGE + CELL / 2) * s)
    if 0 <= cyr < h:
        bits = ""
        for i in range(BITS):
            cx = bx + int((i + 0.5) * CELL * s)
            if cx >= w:
                bits = ""; break
            bits += "1" if a[cyr, max(0, cx - 2):cx + 3].mean() > 128 else "0"
        out["clock"] = int(bits, 2) if len(bits) == BITS else None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("take"); ap.add_argument("--csv"); ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    # PPM, because the decode is the cost. ffmpeg writes raw frames 4.5x faster
    # than PNG and PIL reads them 12x faster (3.4ms against 40.8ms) - the frames
    # are big on disk and are deleted once the CSV exists.
    frames = sorted(glob.glob(os.path.join(a.take, "f*.ppm"))) \
        or sorted(glob.glob(os.path.join(a.take, "f*.png")))
    if not frames:
        print("no frames in %s" % a.take, file=sys.stderr); sys.exit(1)
    tp = os.path.join(a.take, "times.txt")
    times = [float(l) for l in open(tp)] if os.path.exists(tp) else []
    # `ffprobe` timed the WHOLE movie while the frames may be a window of it
    # (cmd_tween skips the still lead), so align from the END - the last frame
    # is the last frame either way.
    if len(times) > len(frames):
        times = times[len(times) - len(frames):]

    # A take is hundreds of full-resolution PNGs and the decode is the whole
    # cost, so decode them in parallel - it is the one part of this that is
    # embarrassingly so.
    with Pool() as pool:
        ms = pool.map(read_frame, frames, chunksize=8)
    rows = []
    for i, m in enumerate(ms):
        t = times[i] if i < len(times) else (times[-1] if times else i / 60.0)
        rows.append((i + 1, t, m))
    off = [r for r in rows if r[2] and r[2].get("offscreen")]
    topoff = [r for r in rows if r[2] and r[2].get("topoff")]
    seen = [r for r in rows if r[2] and "h_pt" in r[2]]
    if not seen:
        print("NO RULER IN ANY FRAME - is `rig.sh ruler on` set, and is this a DEBUG build?",
              file=sys.stderr)
        sys.exit(2)

    t0 = seen[0][1]
    if a.csv:
        with open(a.csv, "w") as fh:
            fh.write("frame,t,offset,top_pt,bot_pt,h_pt,pitch_pt,clock_ms,offscreen,topoff,"
                     "table_pt,opp_pt\n")
            for n, t, m in rows:
                m = m or {}
                fh.write("%d,%.4f,%.4f,%s,%s,%s,%s,%s,%s,%s" % (
                    n, t, t - t0, m.get("top_pt", ""), m.get("bot_pt", ""),
                    m.get("h_pt", ""), m.get("pitch_pt", ""),
                    m.get("clock_ms", m.get("clock", "")),
                    "1" if m.get("offscreen") else "",
                    "1" if m.get("topoff") else "") + ",%s,%s\n" % (
                    m.get("table_pt", ""), m.get("opp_pt", "")))
    if not a.quiet:
        print("%-5s %8s %9s %9s %9s %7s  %s" % ("f", "+off", "top", "bot", "height", "pitch", "clock"))
        for n, t, m in rows:
            if m and m.get("offscreen"):
                print("%-5d %8.3f %9.1f   BOTTOM EDGE OFF SCREEN" % (n, t - t0, m["top_pt"]))
                continue
            if m and m.get("topoff"):
                print("%-5d %8.3f %9s %9.1f   (top above the screen)"
                      % (n, t - t0, "-", m["bot_pt"]))
                continue
            if not m:
                print("%-5d %8.3f   (no ruler)" % (n, t - t0)); continue
            print("%-5d %8.3f %9.1f %9.1f %9.1f %7s  %s"
                  % (n, t - t0, m["top_pt"], m["bot_pt"], m["h_pt"],
                     m["pitch_pt"] if m["pitch_pt"] is not None else "-",
                     m.get("clock", "-")))

    hs = [m["h_pt"] for _, _, m in seen]
    # THE ONE WAY THE WINDOW CAN BE WRONG: if the very first frame is already
    # moving, the extraction started too late and the beginning of the tween is
    # simply not in these frames. The movie still has it - lower FOOLISH_TWEEN_SS.
    if len(hs) > 2 and hs[0] != hs[1]:
        print("!! the first frame is ALREADY MOVING - the window starts too late.",
              file=sys.stderr)
        print("   lower FOOLISH_TWEEN_SS (the movie still holds the whole take).",
              file=sys.stderr)
    mv = [i for i in range(1, len(hs)) if hs[i] != hs[i - 1]]
    print("\nframes   %d (%d with a ruler)" % (len(rows), len(seen)))
    print("height   %.1f -> %.1f pt" % (hs[0], hs[-1]))
    if mv:
        span = seen[mv[-1]][1] - seen[mv[0] - 1][1]
        print("tween    %.3fs, %d frames, %.1f -> %.1f pt"
              % (span, mv[-1] - mv[0] + 2, hs[mv[0] - 1], hs[mv[-1]]))
    # A frame we did not draw: the clock repeats while the geometry moves.
    cl = [(i, m.get("clock"), m["h_pt"]) for i, (_, _, m) in enumerate(seen)]
    stale = sum(1 for i in range(1, len(cl))
                if cl[i][1] is not None and cl[i][1] == cl[i - 1][1] and cl[i][2] != cl[i - 1][2])
    if topoff:
        print("top off   %d frame(s) had the box top above the screen (the slide "
              "does this by design; the drawer clips it)" % len(topoff))
    if off:
        print("!! OFF SCREEN: %d frame(s) drew the box taller than the screen - "
              "its bottom edge, and the hand on it, were cut off." % len(off))
    print("stale    %d frame(s) where the clock repeated while the box moved" % stale)
    # DID IT FINISH? The window is trimmed at both ends, so the one thing worth
    # asserting is that the box was still moving when we started looking and had
    # stopped by the time we stopped. A tail that does not agree means the
    # window closed mid-tween - raise FOOLISH_TWEEN_T.
    tailv = hs[-8:]
    if len(hs) > 8 and len(set(tailv)) > 1:
        print("!! the last frames DISAGREE (%s) - the window closed mid-tween."
              % ", ".join("%.1f" % v for v in sorted(set(tailv))), file=sys.stderr)
        print("   raise FOOLISH_TWEEN_T; the movie still holds the whole take.",
              file=sys.stderr)
    else:
        n_tail = 0
        for v in reversed(hs):
            if v != hs[-1]:
                break
            n_tail += 1
        print("settled  yes, and held for %d frame(s) at the end" % n_tail)
    ps = [m["pitch_pt"] for _, _, m in seen if m["pitch_pt"]]
    if ps:
        want = 2 * BAND
        off = sum(1 for p in ps if abs(p - want) > 2.0)
        print("pitch    median %.1fpt (want %.0f), %d frame(s) off%s"
              % (float(np.median(ps)), want, off,
                 " - imagery was SCALED on its way to the screen" if off else ""))


if __name__ == "__main__":
    main()

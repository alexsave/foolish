#!/usr/bin/env python3
"""Does thinking faster make you less likely to be the fool?

Seats alternate fast/slow around one table and the run is scored by who ends up
holding the cards. Each cell is run in both polarities (fast on even seats, then
fast on odd) and pooled, because seat position is not neutral in Durak - the
opening attacker is derived from the deal - and without that the speed effect
and the seat effect would be the same number.

Under the null, the chance the fool is a fast seat is just fast_seats/np. The
z column is how far the observed share is from that.

  python3 tools/speed_sweep.py                 # the default matrix
  python3 tools/speed_sweep.py --games 40 --brains random,handwritten
"""
import argparse, math, re, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, os.pardir, "foolyard")


def run_cell(brain, np_, fast_ms, slow_ms, games, seed, fast_on_even):
    """One table shape, one polarity. Returns (fast_fools, slow_fools, finished, n_fast)."""
    seats = []
    for s in range(np_):
        is_fast = (s % 2 == 0) == fast_on_even
        seats.append(f"{brain}@{fast_ms if is_fast else slow_ms}")

    tables = max(1, min(16, games // 4))
    cmd = [BIN, "--lineup", ",".join(seats), "--games", str(tables),
           "--secs", "1000000", "--until-games", str(games),
           "--kernel-pacing", "0", "--seed", str(seed), "--csv"]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout

    fast_fools = slow_fools = 0
    finished = 0
    n_fast = 0
    for line in out.splitlines():
        if not line.startswith("CSV,"):
            continue
        _, _, seat, _name, think_ms, fools, fin, _moves = line.split(",")
        seat, think_ms, fools, fin = int(seat), int(think_ms), int(fools), int(fin)
        finished = fin
        if think_ms == fast_ms:
            fast_fools += fools
            n_fast += 1
        else:
            slow_fools += fools
    return fast_fools, slow_fools, finished, n_fast


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--brains", default="random,handwritten,octogen")
    ap.add_argument("--sizes", default="2,3,4,5,6,7,8")
    ap.add_argument("--games", type=int, default=40, help="finished games per polarity")
    ap.add_argument("--fast", type=int, default=50, help="fast seat think, ms")
    ap.add_argument("--slow", type=int, default=2000, help="slow seat think, ms")
    args = ap.parse_args()

    print(f"speed sweep: fast={args.fast}ms slow={args.slow}ms, "
          f"{args.games} games per polarity, both polarities pooled\n")
    print(f"{'brain':<14}{'np':>3}{'fast seats':>12}{'games':>8}"
          f"{'expected':>10}{'observed':>10}{'z':>8}")
    print("-" * 65)

    for brain in args.brains.split(","):
        for np_ in [int(x) for x in args.sizes.split(",")]:
            fast_f = slow_f = total = 0
            n_fast_sum = 0
            for polarity, fast_on_even in enumerate([True, False]):
                seed = 1000 + np_ * 10 + polarity
                ff, sf, fin, nf = run_cell(brain, np_, args.fast, args.slow,
                                           args.games, seed, fast_on_even)
                fast_f += ff
                slow_f += sf
                total += fin
                n_fast_sum += nf * fin   # weight each game by that cell's fast-seat count

            if total == 0:
                print(f"{brain:<14}{np_:>3}{'-':>12}{0:>8}")
                continue

            expected = n_fast_sum / (total * np_)      # P(fool is fast) under the null
            observed = fast_f / total
            sd = math.sqrt(expected * (1 - expected) / total) if 0 < expected < 1 else 0
            z = (observed - expected) / sd if sd else 0
            mark = "  <--" if abs(z) >= 2 else ""
            print(f"{brain:<14}{np_:>3}{n_fast_sum/total:>12.1f}{total:>8}"
                  f"{expected:>10.3f}{observed:>10.3f}{z:>8.1f}{mark}")
            sys.stdout.flush()


if __name__ == "__main__":
    main()

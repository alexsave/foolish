#!/usr/bin/env python3
"""What does latency do to one brain, across every table size?

speed_sweep.py asks a yes/no question with two speeds. This asks a graded one:
seat a ladder of think times from --fast to --slow around the table and see
whether the fool rate rises monotonically along it.

Every table size is run with as many rotations as it has seats, so each rung of
the ladder sits in each seat exactly once - Durak's opening seat is derived from
the deal, and without that rotation the ladder would be measuring seat position.

  python3 tools/latency_ladder.py --brain random
  python3 tools/latency_ladder.py --brain octogen --games 20
"""
import argparse, collections, math, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, os.pardir, "foolyard")


def ladder(np_, fast, slow):
    """np_ think times spanning fast..slow, geometrically."""
    if np_ == 1:
        return [fast]
    return [int(round(fast * (slow / fast) ** (i / (np_ - 1)))) for i in range(np_)]


def run_size(brain, np_, fast, slow, games, is_client):
    rungs = ladder(np_, fast, slow)
    fools = collections.Counter()
    total = 0

    # One rotation per seat: each rung visits each seat exactly once.
    for r in range(np_):
        order = rungs[r:] + rungs[:r]
        cmd = [BIN, "--lineup", ",".join(f"{brain}@{ms}" for ms in order),
               "--games", str(max(1, min(8, games // 4))),
               "--secs", "1000000", "--until-games", str(games),
               "--kernel-pacing", "0", "--jitter", "0", "--loss", "0", "--dup", "0",
               "--seed", str(9000 + np_ * 100 + r), "--csv"]
        out = subprocess.run(cmd, capture_output=True, text=True).stdout

        seen_seat0 = False
        for line in out.splitlines():
            if not line.startswith("CSV,"):
                continue
            _, _, seat, _name, ms, f, fin, _moves = line.split(",")
            fools[int(ms)] += int(f)
            if int(seat) == 0 and not seen_seat0:
                total += int(fin)
                seen_seat0 = True
    return rungs, fools, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--brain", default="random")
    ap.add_argument("--sizes", default="2,3,4,5,6,7,8")
    ap.add_argument("--games", type=int, default=40, help="finished games per rotation")
    ap.add_argument("--fast", type=int, default=50)
    ap.add_argument("--slow", type=int, default=2000)
    ap.add_argument("--client", action="store_true",
                    help="seat wellbehaved clients on wires instead of server-side bots")
    args = ap.parse_args()

    name = "wellbehaved" if args.client else args.brain
    print(f"latency ladder: {name}, {args.fast}ms..{args.slow}ms, "
          f"{args.games} games per rotation, all rotations pooled\n")

    for np_ in [int(x) for x in args.sizes.split(",")]:
        rungs, fools, total = run_size(name, np_, args.fast, args.slow,
                                       args.games, args.client)
        if total == 0:
            print(f"np={np_}: no finished games")
            continue

        exp = 1.0 / np_
        sd = math.sqrt(exp * (1 - exp) / total)
        parts, worst = [], 0.0
        for ms in rungs:
            share = fools[ms] / total
            z = (share - exp) / sd if sd else 0.0
            worst = max(worst, abs(z))
            parts.append(f"{ms}ms {share:.3f}({z:+.1f})")
        flat = "flat" if worst < 2 else "GRADED"
        print(f"np={np_}  n={total:<5} expected {exp:.3f}   " + "  ".join(parts) + f"   [{flat}]")
        sys.stdout.flush()


if __name__ == "__main__":
    main()

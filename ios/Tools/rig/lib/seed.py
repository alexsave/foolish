#!/usr/bin/env python3
"""Seed the extension's App Group with a canned board, and name the seats.

`c/build/msg_wire_test` SEARCHES for a state (a dense table, a finished game, a
bout-ending cover) and prints ONE FMSG envelope as hex; the extension reads
that from `dev.fatboard` and opens straight onto it. That is what makes a
photograph of a mid-game board reproducible: the state is found in C, in
milliseconds, and the device only has to render it. Playing to the same state
by hand is neither fast nor repeatable.

TWO RUNS, NOT ONE. The seat the local player occupies is not known until the
searcher has reported which seat is the defender, and the cast has to be
rotated so `Alex` lands in THAT chair - so the search runs once to learn the
seat, then again with `FOOLISH_NAMES` set. The search is deterministic (a fixed
seed sweep), so the second run finds the same state as the first.

  SEAT=      the defender's chair (the fatboard default)
  SEAT=atk   an attacker's - the searcher's "defender=seat N" line plus one
  SEAT=<n>   that seat, whatever it is

Names come from `fixture_name()` in c/tests/msg_wire_test.c and are display
strings only. They match the Messages transcript's cast on purpose (see
lib/transcript.py): a bubble from Kate should sit above a board whose seat
says Kate.
"""
import os
import plistlib
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
TOOL = os.environ.get("FOOLISH_TOOL", os.path.join(REPO, "c", "build", "msg_wire_test"))
GROUP_ID = "group.cards.foolish.msg"

# Alex first: the local player is always swapped into whatever seat we sit in,
# and the rest keep this order so the same faces recur across every frame.
CAST = (os.environ["FOOLISH_CAST"].split(",") if os.environ.get("FOOLISH_CAST")
        else ["Alex", "Kate", "Daniel", "John", "Anna", "Hank", "David", "Mira"])


def group_dir(sim: str) -> str:
    """The App Group container. Its UUID changes on every reinstall, so it is
    looked up by identifier every time rather than pinned in a script."""
    root = os.path.expanduser(
        f"~/Library/Developer/CoreSimulator/Devices/{sim}/data/Containers/Shared/AppGroup")
    if not os.path.isdir(root):
        sys.exit(f"no AppGroup containers on {sim} - install the app first")
    for name in os.listdir(root):
        meta = os.path.join(root, name,
                            ".com.apple.mobile_container_manager.metadata.plist")
        try:
            with open(meta, "rb") as f:
                if plistlib.load(f).get("MCMMetadataIdentifier") == GROUP_ID:
                    return os.path.join(root, name)
        except Exception:
            continue
    sys.exit(f"no {GROUP_ID} container on {sim} - launch the extension once")


def search(args, names=None):
    if not os.path.exists(TOOL):
        sys.exit(f"no seeder at {TOOL} - (cd c && make build/msg_wire_test)")
    env = dict(os.environ)
    if names:
        env["FOOLISH_NAMES"] = ",".join(names)
    r = subprocess.run([TOOL] + args, capture_output=True, text=True, env=env)
    return r.stdout, r.stderr


def is_hex(s: str) -> bool:
    return len(s) > 40 and all(c in "0123456789abcdefABCDEF" for c in s)


def payload_and_notes(out: str, err: str):
    lines = [l.strip() for l in (out + "\n" + err).splitlines() if l.strip()]
    hexes = [l for l in lines if is_hex(l)]
    if not hexes:
        sys.exit("SEED FAILED: the searcher printed no payload\n" + out + err)
    return max(hexes, key=len), [l for l in lines if not is_hex(l)]


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: seed.py <fatboard|endgame|lastdefense|twocover|lastmove> [args...]")
    sim = os.environ.get("FOOLISH_SIM")
    if not sim:
        sys.exit("set FOOLISH_SIM")
    mode, rest = sys.argv[1], sys.argv[2:]
    args = ["--" + mode] + rest

    out, err = search(args)
    _, notes = payload_and_notes(out, err)

    # Which chair. The searcher reports the defender; everything else is
    # derived from it, so a `SEAT=atk` frame reliably shows an ATTACKER's
    # action bar rather than the defender's.
    dseat = None
    for l in notes:
        if "defender=seat" in l:
            dseat = int(l.split("defender=seat")[1].split()[0])
    if mode == "fatboard":
        n_players = int(rest[1]) if len(rest) > 1 else 2
    elif rest:
        n_players = int(rest[0])
    else:
        n_players = 2

    want = os.environ.get("SEAT", "")
    if want == "atk":
        seat = ((dseat + 1) % n_players) if dseat is not None else 1
    elif want != "":
        seat = int(want)
    else:
        seat = dseat if dseat is not None else 0

    names = list(CAST)
    names[0], names[seat] = names[seat], names[0]
    out2, err2 = search(args, names)
    payload, notes2 = payload_and_notes(out2, err2)

    g = group_dir(sim)
    with open(os.path.join(g, "dev.fatboard"), "w") as f:
        f.write(payload)
    with open(os.path.join(g, "dev.seat"), "w") as f:
        f.write(str(seat))
    # A seeded board opens QUIET unless the caller asked for the replay - see
    # MessageDevBoard.seededReplay. `REPLAY=1` turns the arrival animation on.
    replay = os.path.join(g, "dev.replay")
    if os.environ.get("REPLAY"):
        open(replay, "w").close()
    elif os.path.exists(replay):
        os.remove(replay)

    for l in notes2:
        print(l)
    print(f"seated at {seat} as {names[seat]}  cast={names[:n_players]}")


if __name__ == "__main__":
    main()

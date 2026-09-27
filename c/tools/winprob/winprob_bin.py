"""Read the packed bytes cnitro_winprob --bin writes.

The layout is documented in `c/src/winprob.h` and written by `winprob_packed`;
this is the same reader in Python, and it refuses a truncated file rather than
returning a shorter strip - the C reader's rule.

    from winprob_bin import read
    strip = read("game.bin")
    strip.seats[0].name                  # "ALEX"
    strip.win(view="truth", seat=0)       # [(step index, percent), ...]

Nothing here knows the game's rules: it is a file format and the numbers in it.
"""

import struct
from dataclasses import dataclass, field

NAME_MAX = 48
MOVE_CARDS = 6
NONE = 0xFFFF          # "not measured" in any x10000 / x1000 field
WIRE_VERSION = 1

F_TRUTH, F_BELIEF, F_BELIEF_FAIL = 1, 2, 4

SUITS = "SHCD"
VALUES = ["2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"]
KINDS = {0: "deal", 1: "draw", 2: "attack", 3: "cover", 4: "pass",
         5: "pickup", 6: "round end", 7: "good", 0xFF: "deal"}


def card(id_, trump=None):
    """A card id as it is printed: suit * 13 + (value - 1)."""
    if id_ is None or id_ >= 52:
        return None
    return f"{VALUES[id_ % 13]}{SUITS[id_ // 13]}" + ("*" if id_ // 13 == trump else "")


@dataclass
class Seat:
    index: int
    name: str
    place: int | None = None       # 1 = out first, n_players = the fool


@dataclass
class Step:
    index: int
    move: int | None               # the recorded action this position follows
    seat: int | None               # who played it
    kind: str
    cards: list
    target: object
    deck: int
    n_truth: int
    n_belief: int
    truth_fool: list               # per seat, fraction or None
    truth_mean: list               # per seat, finish position or None
    belief_fool: list
    belief_mean: list
    hand: list                     # per seat, cards held or None once out

    def label(self):
        s = self.kind
        for c in self.cards:
            if c is None:
                continue
            s += f" {c}>{self.target}" if self.kind == "cover" and self.target else f" {c}"
        return s


@dataclass
class Strip:
    version: int
    code_version: int
    n_players: int
    trump: str
    fool: int | None
    engine_idx: int
    flags: int
    worlds: int
    belief_worlds: int
    playouts: int
    elapsed_ms: int
    seats: list = field(default_factory=list)
    steps: list = field(default_factory=list)

    @property
    def belief_failed(self):
        return bool(self.flags & F_BELIEF_FAIL)

    def win(self, view="truth", seat=0):
        """(step index, percent chance of NOT being the fool), measured points
        only. The gaps are steps where the view carries nothing for that seat:
        a finished board, a seat already out of the belief view, or a belief
        that broke conservation."""
        key = "truth_fool" if view == "truth" else "belief_fool"
        out = []
        for st in self.steps:
            p = getattr(st, key)[seat]
            if p is not None:
                out.append((st.index, 100.0 * (1.0 - p)))
        return out


def read(path):
    with open(path, "rb") as f:
        buf = f.read()
    return loads(buf)


def loads(buf):
    if len(buf) < 28:
        raise ValueError("winprob: shorter than a header")
    (version, n_players, code_version, trump, fool, engine_idx, flags, _pad,
     n_steps, header_bytes, step_bytes, worlds, belief_worlds, _pad2,
     playouts, elapsed) = struct.unpack_from("<8B6H2I", buf, 0)
    if version != WIRE_VERSION:
        raise ValueError(f"winprob: wire version {version}, expected {WIRE_VERSION}")
    n = n_players
    if not 2 <= n <= 8:
        raise ValueError(f"winprob: {n} players")
    if header_bytes != 28 + n * (2 + NAME_MAX) or step_bytes != 17 + n * 9:
        raise ValueError("winprob: header and step strides disagree with n_players")
    if len(buf) != header_bytes + n_steps * step_bytes:
        raise ValueError(f"winprob: {len(buf)} bytes, expected "
                         f"{header_bytes + n_steps * step_bytes}")

    trump_i = trump
    strip = Strip(version=version, code_version=code_version, n_players=n,
                  trump=SUITS[trump] if trump < 4 else "?",
                  fool=None if fool == 0xFF else fool,
                  engine_idx=engine_idx, flags=flags, worlds=worlds,
                  belief_worlds=belief_worlds, playouts=playouts, elapsed_ms=elapsed)

    at = 28
    elim = list(buf[at:at + n])
    at += n
    for s in range(n):
        ln = buf[at]
        name = buf[at + 1:at + 1 + min(ln, NAME_MAX)].decode("utf-8", "replace")
        at += 1 + NAME_MAX
        strip.seats.append(Seat(index=s, name=name))
    for place, seat in enumerate(elim, start=1):
        if seat != 0xFF:
            strip.seats[seat].place = place
    if strip.fool is not None:
        strip.seats[strip.fool].place = n

    def prob(v):
        return None if v == NONE else v / 10000.0

    def fp(v):
        return None if v == NONE else v / 1000.0

    for i in range(n_steps):
        o = header_bytes + i * step_bytes
        move, seat, kind, n_cards = struct.unpack_from("<HBBB", buf, o)
        cards = list(buf[o + 5:o + 5 + MOVE_CARDS])
        target, deck = buf[o + 11], buf[o + 12]
        n_truth, n_belief = struct.unpack_from("<HH", buf, o + 13)
        q = o + 17
        tf = struct.unpack_from(f"<{n}H", buf, q); q += 2 * n
        tm = struct.unpack_from(f"<{n}H", buf, q); q += 2 * n
        bf = struct.unpack_from(f"<{n}H", buf, q); q += 2 * n
        bm = struct.unpack_from(f"<{n}H", buf, q); q += 2 * n
        hand = list(buf[q:q + n])
        strip.steps.append(Step(
            index=i,
            move=None if move == 0xFFFF else move,
            seat=None if seat == 0xFF else seat,
            kind=KINDS.get(kind, "?"),
            cards=[card(c, trump_i) for c in cards[:min(n_cards, MOVE_CARDS)]],
            target=card(target, trump_i),
            deck=deck,
            n_truth=n_truth, n_belief=n_belief,
            truth_fool=[prob(v) for v in tf],
            truth_mean=[fp(v) for v in tm],
            belief_fool=[prob(v) for v in bf],
            belief_mean=[fp(v) for v in bm],
            hand=[None if h == 0xFF else h for h in hand]))
    return strip


if __name__ == "__main__":
    import sys
    s = read(sys.argv[1])
    print(f"wire v{s.version}, replay code v{s.code_version}, {s.n_players} seats, "
          f"trump {s.trump}, {len(s.steps)} steps, {s.playouts:,} playouts")
    for seat in sorted(s.seats, key=lambda x: x.place or 99):
        w = s.win("truth", seat.index)
        lo = min((y for _, y in w), default=float("nan"))
        print(f"  {seat.place}. {seat.name:20s} low water {lo:5.1f}%")

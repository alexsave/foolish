# foolyard - a discrete-event Durak table

A simulated Foolish server, the real C kernel deciding every rule, and clients
on modelled wires - all inside one deterministic single-threaded process where
time is a number that only moves when an event says so.

Adapted from [tiltyard](../../tiltyard)'s market simulator. What came across is
the timing machinery (a packed-event priority queue, a bucketed scheduler, a
slot freelist, the pop-dispatch main loop); what did not is everything about
order books. The game logic is not reimplemented here either: `awire_apply`,
`bot_drive`, `calculate_legal_moves` and `state_put` are the same
[`../c/src`](../c/src) sources the wasm, iOS and native-server builds link.

```sh
make
./foolyard --help
./foolyard --lineup wellbehaved@200,laggy@800,handwritten@300,octogen@600
```

## why

[`sem_fuzz.c`](../server/impls/native/sem_fuzz.c) already proves the kernel
cannot be cheated: it fires illegal moves at `awire_apply` with full ground
truth. But it plays straight-line games - decide, apply, decide, apply - and
so it can never reach the states that only exist when there is a *wire* and a
*clock* between deciding and applying:

- a move chosen against a board three versions stale, landing anyway
- a bot cycle interleaved with a human move that was already in flight
- a retransmit applied twice because nothing in the protocol says "again"
- a push that overtakes a newer one on a datagram link, so the client renders
  backwards
- a table that simply stops, because everyone believes it is someone else's turn

Those need modelled time, which is what this is.

## the shape of it

| file | what it is |
|---|---|
| `include/constants.h` | the event word's bit layout, and the whole timing budget |
| `src/pq.c` | 4-ary min-heap of packed `u32` events |
| `src/sch.c` | the bucketed scheduler: 256 buckets, 65.5ms each |
| `src/fl.c` | fixed slab + free stack, with occupancy tracking |
| `src/net.c` | the wire: latency, jitter, loss, duplication, ordering |
| `src/server.c` | the game registry: request queue, versions, push fanout, bot trampoline |
| `src/client.c` | client core: decode a view, run the tier, submit, self-wake |
| `src/invariant.c` | the detectors |
| `clients/*.c` | one file per client tier |

### the event word

One `u32` per scheduled event, ordered by plain integer compare:

```
[ prio:16 | type:3 | param:13 ]
```

`prio` is the fire time within its bucket (microseconds), `param` names a
packet slot, a client, or a game+seat. A bucket spans 65536us and there are
256 of them, so the wheel reaches 16.7s ahead; anything further parks in a
timer slot and walks in with `EV_HOP` events that `sch_pop` consumes itself.

Two deliberate departures from tiltyard's `sch.c`:

1. **A bucket entry stores `fire & P_MASK`, not the raw `now + delta` sum.**
   Over there the priority field holds an unmasked sum while `sch->now` is
   itself a within-bucket offset and `sch_now_ns` adds `current_bucket <<
   P_BITS` on top, so an event several buckets out has its lap counted twice.
   Here `cursor_start` carries the laps and the entry carries only the offset.
2. **No slow bucket.** Nothing in a Durak session is scheduled days out, so the
   seconds-resolution second tier, its rounding, and its spreading jitter are
   replaced by the exact hop above.

### what the server models

The native server ([`../server/impls/native`](../server/impls/native)), minus
the sockets: per-game request queues serviced one at a time (the per-game
lock), a version bumped on every board change, per-seat masked views cut with
`state_put`, a push-only fanout, and the bot trampoline - one `bot_drive`
cycle, priced by `bot_cycle_delay_ms`, rescheduled rather than slept through.

One departure, and it is the interesting one: **each bot seat is driven
separately, on its own clock.** The real server drives every eligible bot in
one cycle and paces them uniformly so a human can follow along. Here a seat
waits its own `think_us`, which means the order among simultaneously-eligible
bots is decided by who thinks fastest rather than by `bot_drive`'s fairness
shuffle. That is what makes a speed matchup possible (see below).

### what is on the wire, and what is not

`Packet` is split in two on purpose. The wire half is what a real frame
carries. The `obs_*` half - the version a view was cut at, the version a mover
had last seen, its sequence number - is **not on the wire and could not be**:
an awire frame has no version field and a `/ws` push is `[ok][state bytes]`.
A client can neither prove when it decided nor tell a stale view from a fresh
one, and could lie about both if the protocol let it. Those fields exist so
the harness can say "applied a move chosen three versions ago". Nothing in
`server.c` branches on them.

## clients

One file per tier behind a narrow interface, like tiltyard's `src/strategy/`.
A tier is a `settings` / `on_view` / `on_wake` triple; add a file, add its name
to `CLIENT_TIERS` in `include/client.h`, and it can be seated.

| tier | what it does |
|---|---|
| `wellbehaved` | the reference `/ws` client: decode, think, submit from the newest view |
| `laggy` | same brain, 3x latency and 4x jitter |
| `reconnect` | drops its socket mid-bout and re-subscribes |
| `resender` | retransmits the same frame under the same seq when an ack is slow |
| `stale` | decides the instant it sees a board, then sits on the decision |
| `poller` | no subscription at all: `GET /state` on a timer, the pre-`/ws` path |
| `griefer` | connects, watches, never plays |

Every one picks uniformly from `calculate_legal_moves`, exactly as
`foolish_hammer`'s ws worker does. Play strength is not the point.

## the detectors

Findings are counted and the first 40 are printed with a timestamp. Three are
`sem_fuzz`'s, re-asserted because the sim reaches states it cannot; the rest
only exist once there is a wire.

| finding | what it means |
|---|---|
| `conservation` | an accepted move broke the physical deck (lifted from `sem_fuzz`) |
| `mutation_on_reject` | a rejected move still changed the board (`--deep`) |
| `stall` | a live game with a legal move went quiet |
| `phantom_hand_loss` | a card left a seat's hand with no move of its own |
| `duplicate_applied` | the same `(seat, seq)` was applied twice |
| `view_regression` | a client adopted a view older than one it held |
| `queue_overflow` | a game's request backlog overflowed |
| `seat_mismatch` | a frame arrived for a seat it does not own |

`stall` doubles as a product question rather than a bug report. Foolish has no
turn clock, so one seat that declines to act holds its table forever:

```
$ ./foolyard --games 2 --secs 200 --lineup wellbehaved@200,griefer@0,handwritten@300
[  30.000s] stall   game 1 v1 quiet for 30.0s: defender 2, queue 0,
                    could move 0x2, bots parked 0x4, subscribed 0x3
  games      2 dealt, 0 finished
  moves      2 sent, 2 applied
```

`could move 0x2` is only the griefer, and it never will. Two tables, 200
seconds, two moves. Nothing anywhere breaks the deadlock.

### what a retried move costs

`resender` models an **HTTP retry**, not a TCP one: a `POST /action` whose
reply is slow, sent again by the app. (On `/ws` the app never re-sends a frame
- TCP does that below it.) Neither `/action` nor `/ws` carries an idempotency
key, so the server applies whatever is still legal:

```
$ ./foolyard --games 6 --secs 400 --loss 4 --dup 3 --jitter 400 \
    --lineup wellbehaved@150,laggy@600,reconnect@200,resender@250,stale@700,handwritten@200,random@100
[   4.200s] duplicate_applied  game 5 seat 3: good(n=0) seq 3 applied again
                               (last was 3), chosen at v11, board at v15
  moves      4469 sent, 3054 applied, 2101 applied against a board the mover had not seen
  duplicate_applied    45
```

Every single one is `good(n=0)`, and that is the whole story: a move carrying
cards immunises itself, because the retry names a card that has already left
the hand and is rejected as not-in-hand. `good` carries nothing, so a retry
that arrives 4-9 versions late lands in a **later bout the client never saw**
and says good there - silently forfeiting a throw-in the player still had.
Invisible from the client, and a concrete argument for an idempotency key on
the action path.

## the speed matchup

Because each seat has its own clock, the same brain can be sat at one table at
two speeds and scored by who ends up the fool. `tools/speed_sweep.py` runs that
matrix, in both seat polarities (Durak's opening seat is derived from the deal,
so seat position is not neutral and has to be cancelled out):

```sh
python3 tools/speed_sweep.py --brains random,handwritten --games 40
```

fast = 50ms, slow = 2000ms, alternating seats, 80 games per cell. Under the
null the fool is a fast seat half the time:

```
brain          np  fast seats   games  expected  observed       z
random          2         1.0      80     0.500     0.412    -1.6
random          3         1.5      80     0.500     0.300    -3.6  <--
random          4         2.0      80     0.500     0.237    -4.7  <--
random          5         2.5      80     0.500     0.163    -6.0  <--
random          6         3.0      80     0.500     0.263    -4.2  <--
random          7         3.5      80     0.500     0.087    -7.4  <--
random          8         4.0      80     0.500     0.287    -3.8  <--
handwritten     2         1.0      80     0.500     0.425    -1.3
handwritten     3         1.5      80     0.500     0.512     0.2
handwritten     4         2.0      80     0.500     0.562     1.1
handwritten     5         2.5      80     0.500     0.500     0.0
handwritten     6         3.0      80     0.500     0.425    -1.3
handwritten     7         3.5      80     0.500     0.463    -0.7
handwritten     8         4.0      80     0.500     0.550     0.9
octogen         2         1.0      40     0.500     0.450    -0.6
octogen         3         1.5      40     0.500     0.425    -0.9
octogen         4         2.0      40     0.500     0.450    -0.6
octogen         5         2.5      40     0.500     0.450    -0.6
octogen         6         3.0      40     0.500     0.550     0.6
octogen         7         3.5      40     0.500     0.425    -0.9
octogen         8         4.0      40     0.500     0.400    -1.3
```

For `random`, thinking faster is worth a great deal from three players up. For
`handwritten` and `octogen` it is worth nothing at any size. The difference is
what the policy does with an extra chance to act: a random bot throws in
whenever it legally may, so acting first means shedding cards first and going
out sooner, while the deliberate bots decline the throw-ins they do not want
and gain nothing from being asked earlier. **Speed only pays if your policy
spends cards when given the opportunity.**

At two players nobody is racing anyone - attacker and defender strictly
alternate - and the effect disappears even for `random`, which is the control
this wanted.

A practical reading: the live server's uniform bot slowdown is not quietly
handing the paced-down bots a disadvantage, because the bots it actually
ships are the deliberate kind. It would have, on `random`.

### and the same question for a human's connection

`tools/latency_test.py` asks it of clients instead of bots: four identical
`wellbehaved` seats on a clean wire - no loss, no duplication, no jitter -
differing only in how long they take to answer, with the lineup rotated so each
latency sits in each seat exactly once.

```
600 finished games, 4 rotations pooled

  latency   fool    share       z        (expected 0.250)
     50ms     57    0.095    -8.8
    200ms    100    0.167    -4.7
    800ms    126    0.210    -2.3
   2000ms    317    0.528    15.7
```

Strictly monotonic, and latency is the only variable: the slowest seat is the
fool in more than half of all games, the fastest in one in ten.

Read it with the sweep above, though, not on its own. These clients pick
uniformly from the legal menu - the same policy class as `random`, the one
speed demonstrably helps. What this measures is a *random-playing* client, so
it is an upper bound on the effect, not an estimate of what a thoughtful human
on a bad connection suffers. Giving a client tier a real brain is the obvious
next step, and would turn this from an upper bound into an answer.

## determinism

Same seed plus same code gives the same run, event for event. Every stream is
its own `u64` (`src/rng.c`): the world's, each client's. Nothing here touches
a socket, a clock, or a file.

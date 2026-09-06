# From MAU to concurrent tables

The two business documents state targets in DAU and MAU
([`../docs/MONETIZATION_ROADMAP.md`](../docs/MONETIZATION_ROADMAP.md): 40-78k
DAU; [`../docs/ORACLE_MONETIZATION_ENGINEERING.md`](../docs/ORACLE_MONETIZATION_ENGINEERING.md):
5k / 50k / 250k / 2M MAU). The engineering documents state capacity in
concurrent games and connections. **Nothing in the repo joins the two.**
`DEPLOYMENT.md`'s "1k-10k concurrent games" sizing says of itself that it is
"an extrapolation ... not a new benchmark", and no session length,
games-per-session, or game-duration figure is written down anywhere.

foolyard can measure the missing middle, because it plays whole games at human
pace with the real kernel and counts what comes out.

## What a table actually costs, measured

512 tables, 2 sim-hours, human seats thinking 5-6s per move (+-4s), 90ms wire:

| | 2 humans + 2 bots | 4 humans |
|---|---|---|
| game length | **154 s** (2.6 min) | **252 s** (4.2 min) |
| human moves per game | 28.7 | 75.8 |
| bot actions per game | 49.4 | 0 |
| frames per game | 188 | 385 |
| **frames/sec per live table** | **1.22** | **1.53** |
| **bot decisions/sec per table** | **0.32** | 0 |

Frame rate per table barely moves with the mix, which makes it a usable
constant: **~1.2-1.5 frames/sec per live table**, both directions, and that is
what a connection actually has to carry.

## The bridge

Three inputs the business documents do not state. They are assumptions, not
findings, and every number below moves with them:

- **DAU/MAU 20%** - sticky-card-game territory; chess.com-shaped, as the Oracle
  doc frames the breakout case.
- **8 games/day** for an engaged player. At the measured 4.2 min, that is ~34
  minutes of play a day.
- **peak = 2.5x average** concurrency, for an audience skewed to one region's
  evening.

`avg concurrent players = DAU x minutes played per day / 1440`, then peak, then
divide by seats to get tables.

| scenario (doc's own) | MAU | DAU @20% | peak players | **peak tables** (4-seat) |
|---|---|---|---|---|
| Bear | 5k | 1k | 58 | **15** |
| Base | 50k | 10k | 583 | **146** |
| Strong | 250k | 50k | 2,917 | **729** |
| **Breakout** | **2M** | **400k** | **23,333** | **5,833** |
| Roadmap blend | - | 40k | 2,333 | 583 |
| Roadmap $1M @ $0.035 | - | 78k | 4,550 | 1,138 |

The breakout case lands at ~5,800 concurrent tables, inside the "1k-10k
concurrent games" the native server was sized for. That guess was a good one.

## The walls, in the order you hit them

**1. Supabase free tier, at ~3.4k DAU.** The Oracle doc already flags "200
concurrent Realtime connections is a ~200-CCU cap on live games - the first
marketing spike hits it". Quantified against the numbers above, 200 concurrent
players is **~3,400 DAU, ~17k MAU** - you hit it *before* the Base scenario,
never mind Strong. It is the first thing that breaks and it breaks early.

**2. Memory, ~7 GB at breakout.** 23,333 connections at the measured 250-300
KB/conn (Stage 6 epoll) is 5.8-7 GB, plus 5,833 games at 46.6 KB is another
272 MB. That matches `DEPLOYMENT.md`'s "~10,000 concurrent games ≈ 5-8 GB,
~4-6 shard Machines, roughly $70-120/month". Not a wall so much as a bill.

**3. Bot compute, and this one is sharp.** At 0.32 bot decisions/sec per
bot-bearing table, 5,833 tables would want **~1,870 decisions/sec** if every
table carried bots. The measured octogen ceiling is **53.43 decisions/sec on
the whole 4-core box** (single-thread 30.2). That is **~35 boxes of octogen**,
against a stated fixed burn of $35-45/month.

The mix is what saves it: at 2M MAU most tables are human-vs-human, so the
bot-bearing fraction falls and the bill falls with it. But it means the bot
roster is a **cost decision, not just a difficulty one** - octogen for
everybody does not survive the breakout case, and `handwritten` (a pure
heuristic, no search) costs almost nothing. That tension is not written down
anywhere yet.

**What this cannot tell you:** the cost of an Oracle *analysis*, which is the
thing actually being sold. The sim measures gameplay, not review runs, and no
document states how many positions a review evaluates. Until that number
exists the subscription's own compute cost is unknown - and at 30k payers it
is the number that matters most.

## Can the simulator carry it?

Yes, comfortably, on a laptop:

| | tables | sim time | wall time |
|---|---|---|---|
| one core | 1,024 | 1 hour | **8.1 s** |
| 8 shards | 8,192 | 1 hour | **14.6 s** |
| one core, human-paced | 512 | 2 hours | **2.5 s** |

Sustained ~3.1M events/sec, linear in tables. A full sim-**day** at the
breakout scale of 5,833 tables costs about **6 minutes on one core**.

The per-process ceiling is ~4,000 tables: peak packets in flight runs at 2 per
table against `ID_LIMIT`'s 8,192 slots, which is the 13-bit event param. Past
that, shard - the games never interact, so it is embarrassingly parallel in a
way tiltyard is not (there, every trader hits one order book). The breakout
case needs two processes.

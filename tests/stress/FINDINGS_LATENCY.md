# How high does latency have to be before the bugs show?

Short answer: **not high at all — for moves that land close together, single-digit
to ~tens of milliseconds is enough**, which is below ordinary Supabase Realtime
latency. The threshold is not an absolute latency; it's a *relationship*:

> Animation sequences start arriving out of order once the **variation in delivery
> latency** exceeds the **time gap between consecutive broadcasts**.

Two un-awaited broadcasts emitted `G` ms apart, each delivered after a random
`0..L` ms, swap order with probability `(L−G)²/(2L²)` for `L>G` (and 0 for `L≤G`).
So the onset is the diagonal `L ≈ G`.

## Evidence 1 — capture-once, replay-across-grid (`client_sim.ts --sweep`)

40 real games captured once, replayed under the reordered regime across a grid.
Each cell = % of games whose client table ends up **permanently wrong**:

```
gap\lat      0     1     2     5    10    25    50   100   250  1000   (delivery jitter L, ms)
1           0%    0%   28%   63%   73%   93%   93%   98%   95%   98%
2           0%    0%    0%   35%   60%   83%   93%   83%   90%   95%
5           0%    0%    0%    0%   10%   63%   68%   80%   83%   80%
10          0%    0%    0%    0%    0%   33%   38%   78%   78%   90%
25          0%    0%    0%    0%    0%    0%   18%   45%   73%   95%
50          0%    0%    0%    0%    0%    0%    0%   25%   45%   90%
100         0%    0%    0%    0%    0%    0%    0%    0%   20%   68%
250         0%    0%    0%    0%    0%    0%    0%    0%    0%   48%
1000        0%    0%    0%    0%    0%    0%    0%    0%    0%    0%
(rows = emission gap G between broadcasts, ms)
```

The 0%/non-0% boundary tracks `L ≈ G` exactly. Transient glitches (a covering card
flickering, a phantom card for one bout) appear a notch *earlier* than permanent
corruption.

## Evidence 2 — real emission timing (`stress.ts`, independent)

`stress.ts` doesn't parameterize the gap — broadcasts are emitted at the real
wall-clock times moves commit. Sweeping only the delivery jitter:

```
blatency  0   1   2   4   8   16   32   64   (ms)
regress   0   0   0   0   0  102  373  605
```

Onset between **8 and 16 ms**, which is the real inter-commit gap in a contended
game. Matches the grid.

## What the emission gap actually is in this game

| situation | gap between broadcasts | reorders at |
| --- | --- | --- |
| multi-attacker throw-ins / rapid taps / a human move racing the bot loop | ~0–16 ms (near-simultaneous) | **any realtime latency ≳ 5–16 ms** |
| normal back-and-forth within a bot-driven bout | tens of ms | tens of ms |
| moves spaced by the 3 s bot pacing (`BOT_PROCESSING_DELAY_WITH_HUMANS`) | ~3000 ms | only if latency > ~3 s (≈ never) |

## Verdict against the "only at high latency" suspicion

- **Partly fair, mostly not.** Well-spaced moves (the deliberate 3 s bot pacing)
  genuinely don't reorder until absurd latencies — so a calm, turn-by-turn game
  rarely glitches.
- **But the dangerous case is the opposite.** Whenever moves cluster — multiple
  attackers throwing in, a player tapping quickly, or a human move that kicks the
  bot loop so both broadcast within milliseconds — the gap collapses to a few ms
  and the bug triggers at **ordinary** Supabase Realtime latency (p50 commonly
  ~20–50 ms; tail and free-tier/mobile well into the hundreds). That clustering is
  exactly the high-stakes moment in a Durak bout.
- **The disconnect/packet-loss bug is latency-independent**: dropping the
  round-transition broadcast corrupts the table on *any* network the instant a
  packet is lost, regardless of latency.

So the reordering findings are not artifacts of unrealistic latency. They're
dormant in slow, well-spaced play and active precisely when the game gets busy.
The fix (monotonic version stamp on broadcasts + drop stale sequences, and refetch
on reconnect) removes them independent of latency or move spacing.

Reproduce: `npx tsx tests/stress/client_sim.ts --sweep --trials=40`

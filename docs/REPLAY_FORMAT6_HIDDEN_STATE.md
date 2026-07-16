# Replay Format 6 — hidden-state-lossless, partial-game codec

**Status: shipped end to end, including the producer.** C codec + native test,
wasm exports (rules.wasm + bots.wasm), TS encode/decode bridge, view.ts
consumption, AND `finalizeEndedGame` now emit v6 for every seeded game. The
Oracle marshals exact hands from a v6 replay (no retrodiction). Remaining
nice-to-haves in § Follow-up.

## The producer: `finalizeEndedGame` (utils.ts)

At game end the server holds only the **masked** session log (`logs_packed`,
draws hidden) + the **deal seed** (`games.game_seed`). The masked log can't
supply hidden cards, but a seeded deal is reproducible, so finalize:

1. re-runs just the deal from the stored seed — `reconstructSeededDeal`
   (game_lifecycle.ts) → true initial hands + the face-down **stock in draw
   order** (the kernel pops the top) + the trump;
2. `verifyRoundTripV6({ playerIds, logs, flipped, initialHands, stock })` —
   encodes v6 and re-decodes to prove every info action round-trips;
3. stores that as the `game_snapshots.moves` blob.

Any failure (no seed / deal drift / verify mismatch) logs and **falls back to
the frozen v5 encode**, so game-end never breaks. This blocks game end by
design — the end-game screen should always have a replay code. Cost is
negligible: the re-deal is one `start_game` (~µs) and v6 encode ≈ v5 encode.
`e2e/replay_v6.test.ts` exercises this exact path (seed + masked logs → exact
decoded hands).

## Why this exists

The v5 replay codec is lossy about *hidden state by design*. Every public fact
(played cards, table, counts, eliminations, winner) round-trips exactly, but the
**identity and draw-timing of hidden cards is never stored** — `draw_for`
(`sdk/c/src/replay.c`) emits a count-only `LOG_DRAW` and defers a card's
identity until it is *played*. The replay screen then reconstructs hidden hands
by FIFO retrodiction (`src/replay/view.ts`), which is exact only once every card
has surfaced (end of game) and a **guess mid-game**.

That guess is what produced the "Move Oracle inaccuracy" false positive: at the
`A♣→9♥` cover, octogen truly held one trump (the A♣) and had to cover with it,
but retrodiction back-dated a 9♣ (drawn 7 plays later) into that hand, inventing
a "cheaper cover" the bot never had. The Oracle marshals the retrodicted hand,
so it judged a *forced* move as a mistake.

Format 6 removes the guess: it **entropy-codes every hidden card's identity
inline, at the moment it is dealt or drawn**, so the decoded stream carries real
identities and view.ts / the Oracle never have to retrodict.

## Design (option 3, not §16's seed variant)

`docs/IMESSAGE_GAME_DESIGN.md` §16 sketched a "Format 6" that stores the **deal
seed** and re-derives the deck by re-running the ChaCha shuffle. We deliberately
chose the *other* option:

- **No seed in the replay code.** The seed is 32 bytes (a ChaCha-256 key —
  `game_set_deal_seed_bytes` requires `len >= 32`; §16's "16 bytes" is wrong)
  and would be dead weight here (the in-browser Oracle re-seeds randomly; the
  seed lives in the FMSG iMessage envelope instead). Option 3 codes the residual
  *draw-order entropy* directly, which is far smaller than a flat 32 B.
- **Draw-time reveal.** `code_reveal()` codes each hidden card uniform over the
  `unseen` pool (a uniform shuffle makes the next dealt/drawn card uniform over
  what's still unseen), then moves it `unseen → known[seat]`. In v6 `unknown[]`
  is always 0 — every hand is fully known, so the v5 reveal / hypergeometric /
  complement machinery is never reached.
- **Explicit atom count** (a LEB128 varint after the header) replaces v5's
  "decode until the fool is known", so a stream may terminate **mid-game** with
  no `REPLAY_EINCOMPLETE`. `out[4]` (fool) is `0xFF` for a mid-game cut.
- **v5 stays byte-frozen.** Format 6 is a new version byte (6) with its own
  encode entry point; the shared menu/weight/atom/cascade machinery is reused
  verbatim — only card *revelation* moves earlier.

### Wire (v6)

Encode input adds the real hidden cards (the caller/server holds the true deck):

```
u8 n, u8 trump_id, u8 first_attacker
u16 n_actions      // atoms to code — may be < full game (the mid-game cut)
u16 n_reveals      // real hidden cards, in reveal order:
                   //   first n*6 = initial deal, seat-major
                   //   then one per stock draw, in refill-pop order (the flip
                   //   is never listed — it's the header trump, drawn last)
n_reveals x u8     // wire card ids 0..51
per action (identical to v5): u8 kind, u8 seat, u8 n_pairs, pairs...
```

Decode output is v5's header+log layout except `out[0]=6`, the stream is
prefixed by one `LOG_DRAW` per seat = that seat's real initial hand, and every
later `LOG_DRAW` carries a **real** card id (never `REPLAY_CARD_HIDDEN`).

See `sdk/c/src/replay.h` for the byte-level contract and `replay.c`
(`code_reveal`, `run_replay_v6`, `replay_encode_v6`, the v6 branch in
`replay_decode`) for the implementation.

## Measured

`make -C cnitro build/replay_v6_test && ./sdk/c/build/replay_v6_test 150` —
900 real engine games, pc 2–8, ~787k assertions, all passing. Asserts, per game:
decode carries the true initial hands + every real draw (losslessness), encode
determinism, decode→re-encode fixed point, and a mid-game prefix decodes cleanly
with no fool.

**Size: v6 ≈ +6.4% over v5** (54.8 B vs 51.5 B avg over 900 games). For
comparison, storing the 32-byte seed would be ~+60% on a ~50 B payload. The
overhead is the draw-order residual + the atom-count varint. The initial deal is
coded as an ascending *combination* per hand (see `deal_hand_v6`), so it spends
no bits on within-hand order — that optimization alone cut v6 from +11.5% to
+6.4% (kept because it strictly shrinks the average).

## What's wired (all tested)

- **Codec** — `sdk/c/src/replay.{c,h}`: `code_reveal`, `deal_hand_v6`,
  `code_varint`, `run_replay_v6`, `replay_encode_v6`, v6 branch in
  `replay_decode`. v5 byte-frozen.
- **Native test** — `sdk/c/tests/replay_v6_test.c` (in `make difftests`):
  ~787k assertions, pc 2–8. `REPLAY_STATS` peak = **465 recorded choices**
  (wasm cap `REPLAY_REC_CAP` = 4096) and 34 bignum limbs (cap 2688) — v6 fits
  the tight wasm rules overlay with large margin, so no memory-budget change
  was needed.
- **Wasm** — `wasm_replay_encode_v6` exported from **rules.wasm** and
  **bots.wasm** (the latter because `bots.ts` adopts the engine slot when bots
  run). Decode is version-dispatched, so `wasm_replay_decode` handles v6.
  Both committed modules rebuilt.
- **TS bridge** — `engine.ts` `kernelReplayEncodeV6`; `encode.ts`
  `encodeReplayV6(input, maxActions?)` (builds the reveal stream, supports a
  mid-game cut); `decode.ts` unchanged (generic). e2e: `e2e/replay_v6.test.ts`.
- **view.ts** — a v6 replay carries real deal/draw identities, so
  `buildReplaySteps` seats start empty and fill from the DRAW logs; hands are
  exact at every step with **zero hidden cards and zero retrodicted slots**.
  That is the Oracle fix: it marshals these steps.

## Producer note (important)

Whatever emits v6 must feed `encodeReplayV6` the **real** hidden cards: each
seat's true initial hand plus real DRAW cards in refill-pop order. The server
holds the true deck, so it can. For **seeded** games the draw order is even
simpler — `game.deck` captured at deal time *is* the draw order (the kernel pops
the top: `draw_index`), so no per-draw reconstruction is needed. `game.flipped`
/ `power_suit` are cleared once the trump is drawn late-game — snapshot the trump
at deal time.

## Follow-up (nice-to-have, not blocking)

- A TS/e2e **oracle mirror** for v6 (like `e2e/replay_ts_oracle.ts` polices v5)
  if v6 ever needs cross-impl byte policing.
- **Live / mid-game** producer: the seed + action log are already stored
  incrementally, so a running v6 (or seed+actions) blob would let the Oracle
  analyse an in-progress game. This is the iMessage FMSG path — future scope.
- v6 **mid-game** `buildReplaySteps` sets the closing step's fool to `0xFF`
  (255); the replay screen's end-of-game UI should treat 255 as "no fool yet".
- Old v5 snapshots stay v5 (the codec decodes both); only games finished after
  this change are v6. A backfill re-encode from `game_seed` could upgrade them.

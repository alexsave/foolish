# Replay Format 6 — hidden-state-lossless, partial-game codec

**Status: C codec landed + native-tested. Wasm/TS/view.ts wiring is follow-up
work (§ Integration).**

## Why this exists

The v5 replay codec is lossy about *hidden state by design*. Every public fact
(played cards, table, counts, eliminations, winner) round-trips exactly, but the
**identity and draw-timing of hidden cards is never stored** — `draw_for`
(`cnitro/src/replay.c`) emits a count-only `LOG_DRAW` and defers a card's
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

See `cnitro/src/replay.h` for the byte-level contract and `replay.c`
(`code_reveal`, `run_replay_v6`, `replay_encode_v6`, the v6 branch in
`replay_decode`) for the implementation.

## Measured

`make -C cnitro build/replay_v6_test && ./cnitro/build/replay_v6_test 150` —
900 real engine games, pc 2–8, ~787k assertions, all passing. Asserts, per game:
decode carries the true initial hands + every real draw (losslessness), encode
determinism, decode→re-encode fixed point, and a mid-game prefix decodes cleanly
with no fool.

**Size: v6 ≈ +11.5% over v5** (e.g. 57.4 B vs 51.5 B avg). For comparison,
storing the 32-byte seed would be ~+60% on a ~50 B payload. The overhead is the
draw-order residual + the atom-count varint + a small initial-deal
set-ordering slack (see below).

## Follow-up work (not yet done)

1. **Optimize the initial deal.** The deal is coded as 6 *ordered* uniform picks
   per seat; a hand is a set, so this wastes ~log₂(6!) ≈ 9.5 bits/hand. Coding
   each hand as an ascending *combination* recovers it (~1 B/hand, more at high
   pc). Draws don't have this slack (a draw is genuinely ordered).
2. **Wasm + TS bridge.** Add `wasm_replay_encode_v6` to the rules export list
   (`cnitro/Makefile`) and bridge it in `supabase/functions/_shared/replay/`.
   **Before shipping in wasm, re-measure `REPLAY_STATS` peaks** — v6 codes more
   choices per game (every reveal), so `g_rec`/`g_bn` overlay budgets
   (`REPLAY_REC_CAP`, the `_Static_assert`s in `replay.c`) must be re-checked
   against the tightened wasm caps. Native has huge caps, so this test is green
   regardless.
3. **view.ts consumption.** When a replay decodes as v6, use the real per-seat
   initial-deal draws + real draw identities to seed hands directly instead of
   `slots` retrodiction — that is what actually fixes the Oracle.
4. **A TS/e2e oracle mirror** for v6 (like `e2e/replay_ts_oracle.ts` polices v5)
   if v6 ever becomes a shipped wire format that needs cross-impl policing.
5. **Producer note.** Whatever emits v6 must supply reveals in exact
   refill-pop order (the native test extracts them from the kernel's real
   `LOG_DRAW` stream — the server can do the same).

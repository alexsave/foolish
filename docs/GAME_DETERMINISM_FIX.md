# Mid-game determinism: why a game_seed didn't reproduce its game

## Symptom

A recorded octogen-vs-octogen game (deal seed
`da645ff5…d2327d5f`) could not be reproduced from its stored `game_seed`, and
octogen appeared to "play differently" than a fresh run on the same deal. The
deal LOOKED right — trump, flip (5♠), first attacker, and the opening attack
(Q♣) all matched — but the game diverged a few moves in.

## Investigation (scientific method, each step tested)

1. **The deal is fully reproducible.** Native `dealcheck` on the stored seed
   produced a 36-card layout whose **multiset is identical** to the true deal
   reconstructed from the recorded moves (flip, both opening hands, and the
   whole draw pile — same cards). So the deal seed reproduces the deal
   *contents* exactly. The deal code (`deal_shuffle` in `game.c`,
   `deal_rng.c`) is byte-identical across every build since it landed
   (76a96d7 → HEAD), so no deal-side build drift.

2. **The divergence is mid-game draw ORDER, not the deal.** Driving the
   recorded moves against the seed-dealt deck failed at the first move that
   needed a card the deterministic deck hadn't dealt to that seat yet — e.g.
   `attack 0 3,6`: the 6♥ was in the seed's deck, just not drawn in
   pop-the-top order. Production had drawn it at *random*.

3. **Three independent sources of mid-game randomness** were found and each
   confirmed by isolating it (fixed deal seed, separate processes, stubbed
   `Math.random`):

   - **(a) `wasm_import_state` dropped the deterministic-deck flag.** The bot
     loop applies moves via `runPackedGameAction → marshalGame →
     wasm_import_state`, which reset `deterministic_deck = false`. So mid-game
     refills fell through to `game_random()` and drew a RANDOM card from the
     (otherwise deterministic) deck. The human path loads the durable blob
     (`wasm_state_deserialize`, which *does* restore the flag), which is why it
     was reproducible and the bot path was not.
   - **(b) Per-move `Math.random()` reseed of the draw LCG** (`engine.ts`
     `packedActionCore` / `runKernel`): `wasm_set_seed(Math.random()*2³²)` was
     called before every move "so draws stay unpredictable."
   - **(c) Per-decision `Math.random()` strategy seed** (`bots.ts`
     `wasm_set_strategy_seed(Math.random())`). octogen's Monte-Carlo **rollout
     opponent models** draw from this stream, so a fresh random seed each
     decision made octogen pick differently from *identical* state. This is the
     one that actually changed octogen's moves — proven by stubbing
     `Math.random` and watching two separate processes converge to the same
     game.

## Fix

The whole game is now a pure function of the ONE crypto value drawn at the
deal (`injectDealSeed`'s 32 `crypto.getRandomValues` bytes). No `Math.random`
or `crypto` per move.

- **`wasm_set_deterministic_deck(on)`** (new export): re-asserts the
  deterministic-deck flag after `wasm_import_state`, driven by
  `game.deterministic_deck`. That flag is set at the deal
  (`game_lifecycle.ts`), restored from the durable blob on load
  (`deserializeGameState` reads blob byte 1), and carried through
  `marshalGame`. Seed-dealt games now pop the pre-shuffled deck on the bot path
  too; legacy/test deals (no deal seed) keep their old random-draw behaviour, so
  the parity suite is unchanged.
- **`wasm_seed_rng_deterministic()`** / **`wasm_set_strategy_seed_deterministic()`**
  (new exports): seed the draw LCG and the strategy LCG (distinct salts) from
  `state_fnv()`, replacing the per-move / per-decision `Math.random` reseeds
  (`engine.ts`, `bots.ts`).
- **`wasm_set_rng_base(base)`** (new export) — the security boundary. `state_fnv`
  folds in this secret base, set from `rngBaseFromSeed(game.game_seed)`. The bot
  RNG must NOT be a pure function of the public board: the Monte-Carlo bots'
  rollout opponent models draw from the strategy stream, so if the seed were a
  hash of visible state a source-code holder could recompute it and predict
  octogen's every move. `game_seed` is SERVER-ONLY — never on `PublicGame`, never
  in the state blob or any per-viewer view (`gameToPublicGame` / `personalize_game`
  drop it; the deck in the blob is masked `WIRE_CARD_HIDDEN`), and the commit RPC
  keeps it via `COALESCE(p_game_seed, game_seed)`. So the seed is reproducible
  ONLY to the server that holds `game_seed`, and the base is a one-way hash of it.
  `game.game_seed` is loaded onto the working game in `loadCompleteGame` for the
  bot loop.

Test hooks (`__setDealSeedOverride`, and the existing `seedSource`) are
untouched-in-spirit: under a test seed source the pinned streams still run and
the base stays 0, so the parity/e2e suites stay byte-for-byte. No state-blob
format bump and no migration: the seed already lives in its own server-only
`games.game_seed` column.

## Security: reproducible to the server, unpredictable to players

The determinism must be one-directional — the *server* can replay a game from
`game_seed` (audit/replay), but a *player* must not be able to predict the bots.
An earlier version of this fix seeded the bots from `state_fnv()` of the visible
board alone; that was a regression vs the `Math.random` it replaced, because the
board is (largely) public and, in the endgame, fully reconstructible — so a
source-code holder could recompute the seed and predict octogen. Folding the
server-only `game_seed` into every bot seed closes that: without it the seed is
unrecoverable. Proven by `e2e/deal_determinism.test.ts` — octogen replays
identically *with* the seed but **diverges when the seed is withheld** (the
attacker's view), so its play is not a function of anything a player can see.

## Validation

- `e2e/deal_determinism.test.ts` (new): a whole game replays byte-identically
  from a pinned deal seed, for both simple_heuristic (no RNG — isolates draws)
  and octogen (samples worlds from state). Two *different* seeds still produce
  different games.
- octogen is byte-identical across two **separate processes** on the same seed
  (was 83 vs 82 moves before; 118 vs 118, identical hash, after).
- No regressions: bot_parity 7/7, action_handlers 8/8, test:mem 4/4,
  packed_wire_parity, replay/client-rules validation all pass.

## Note on the recorded game

The recorded game was played on a build with all three randomness sources live,
so it is not retroactively reproducible — its mid-game draws and octogen choices
were genuinely random at play time. These changes make **future** games
reproducible from their `game_seed`. octogen was never "weak" or seed-reading;
it was fed a different, randomly-drawn deck order and a random rollout seed each
decision.

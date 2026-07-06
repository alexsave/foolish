# Foolish

A дурак (Durak) card game for the web — real-time multiplayer, bots, spectating,
and shareable replays. Built with [Next.js](https://nextjs.org) (App Router,
React 19) and [Supabase](https://supabase.com) (Postgres + edge functions +
realtime).

It looks like a simple card game. It is not. Underneath the table sit four
serious, self-contained engineering projects — a native-C bot research lab, a
from-scratch game-playing neural net, an information-theoretic replay codec, and
a fully procedural, offline-capable renderer. See
[The four projects under the hood](#the-four-projects-under-the-hood).

---

## Quick start

```bash
npm install
npm run dev          # http://localhost:3000
```

The dev server is the web client. It talks to a Supabase backend; configure the
two client-exposed variables (note the `NEXT_PUBLIC_` prefix):

- `NEXT_PUBLIC_SUPABASE_URL`
- `NEXT_PUBLIC_SUPABASE_KEY`

Production:

```bash
npm run build        # builds to .next/
npm start            # serve the production build
```

Deploys to Vercel with zero config — Vercel auto-detects Next.js and serves the
`.next` output.

## Scripts

| Script | What it does |
| --- | --- |
| `npm run dev` | Run the client in dev mode (hot reload). |
| `npm run build` / `npm start` | Production build / serve. |
| `npm run test:e2e` | Full-stack tests against a real Postgres (see [`e2e/`](e2e/README.md)). |
| `npm run bot:game` | Run a headless bot-vs-bot game locally (`offlinefun/localtest/bot_loop_core.ts`). |

## Repository layout

```
src/                      Next.js web client (App Router)
  app/                    routes — /, /about, /tutorial, /dashboard, /:game_id
  components/             UI, incl. GameDisplay/* (board, cards, animations)
  contexts/               game state, auth, realtime, drag, animation, theme, i18n
  state/                  realtime animation feed + client reconciliation
  replay/                 client-side replay decode + playback  ← project 3
  backend/                Supabase client singleton
  localization/           en / ru / ko strings
  utils/                  procedural rendering helpers (fractal, textures)  ← project 4

supabase/
  functions/              edge functions: create/join/attack/cover/pass/pickup/
                          good/exit/start/continue/add-bot/bot_bump/meta/...
  functions/_shared/      types, actions, replay codec, and the WASM bridges
                          to the C kernel (rules + bot brains)
  migrations/             schema, incl. CAS concurrency + bot-lease heartbeat
  seed.sql                seeds the bot roster (Cordite, Espresso, Handwritten, …)

cnitro/                   pure-C Durak engine + bot arena  ← project 1
offlinefun/               offline/PWA layer + ML experiments (NEAT, nitro)  ← projects 2 & 4
e2e/                      full-stack test suite (real server code, real Postgres)
docs/                     design / refactor notes; ARCHITECTURE_REVIEW.md is the
                          latest full audit (server flow, client data flow,
                          fixed glitches, ranked improvement backlog)
```

### Shared code (`@shared`) and the C rules kernel

**The game rules have one implementation: C.** The kernel in
`cnitro/src/game.c` + `legal.c` (state transitions, legality, legal-move
enumeration, dealing/refill, the log stream) is compiled to WebAssembly
(`cd cnitro && make wasm`) and embedded as base64 in
`supabase/functions/_shared/wasm/rules_wasm.ts`, so the same 29 KB module
loads with zero asset plumbing in Deno edge functions, Node (tests, offline
sims) and browsers. The TS files in `_shared/actions/` and parts of
`common_utils.ts` are now thin bridges: they marshal the `Game` object into
the kernel, run the action, and reconstruct the exact TS API surface —
mutated state, `game_logs`, error messages, and the AnimationEvent stream
with its per-step snapshots (the kernel fires a hook at every point the old
TS handlers captured one). Before the TS implementations were deleted, a
differential harness replayed ~100k mirrored actions plus ~30k adversarial
probes through both engines with identical seeds and byte-compared states,
logs, events and rejection messages: zero divergence. `e2e/wasm_engine.test.ts`
keeps policing the seams.

**The bot brains are C too.** Every algorithmic strategy (`random`,
`espresso`, `handwritten`, `simple_heuristic`, `champion`,
`ultimate_champion`, `hacker`, `cordite`, `cordite_max`, `fulminate`) lives
in `cnitro/src/*_strategy.c` and ships as a second module, `bots.wasm`
(`make wasm-bots` → `_shared/wasm/bots_wasm.ts`, ~150 KB): the rules kernel
plus all bots plus a choose-move bridge. A bot turn marshals the game in
once and the kernel enumerates legal moves and picks one — only the chosen
index crosses back to TS (`_shared/wasm/bots.ts`,
`WasmBotStrategy` in `bot_strategy.ts`). The seven heuristic bots are
**exact behavioral mirrors** of the TS originals — `e2e/bot_parity.test.ts`
proves the kernel picks the identical move on every decision of thousands
of seeded games (RNG streams pinned on both sides); the retired TS sources
are frozen as oracles in `offlinefun/localtest/frozen/`. cordite/fulminate
run the C originals directly (the TS versions were ports of them), at the
production world budget via the `CD_BUDGET` knob — roughly **15× faster**
per decision than the TS implementation at 4 players (bitboard rollouts +
no GC; a wasm tick-profile pass then took another ~1.5× out of the rollout
inner loops: cached table-value mask, O(1) greedy-cover pick, wasm
bulk-memory copies). The extra speed is banked as latency, not strength:
world-budget sweeps show the deployed budget already sits at the
saturation knee (2×/4× worlds moved win rate 40.0%→40.0%→39.0% at pc4,
26%→25% at pc6 over 400 seeded games each). The only TS-brained
strategies left are the non-algorithmic ones: `gpt` (LLM adapter) and the
experimental `nitro` NN.

Types, constants, the replay codec, meta/lobby actions, and the I/O layer
(DB, broadcast, bot loop) remain TS in `supabase/functions/_shared/`, shared
between client and edge functions via the `@shared/*` path alias (see
`tsconfig.json`) with Deno-style `.ts` extensions, as before. A few thin,
kernel-mirrored projections (`canCover`, `game_done`,
`get_next_player_index`, `shouldBotActCore`) stay in TS for the client's
synchronous use — parity-tested against the kernel, never independently
evolved.

### How the game runs

Each move is an edge function (`attack`, `cover`, `pass`, `pickup`, `good`, …)
that validates and applies the action under an optimistic-concurrency commit
(`commit_game`, a compare-and-swap on a version counter — the older advisory-lock
tables were dropped, see `supabase/migrations/`). State changes broadcast over
Supabase Realtime as **animation events** on per-player channels (you see your own
hand; everyone else sees card backs) plus a public spectator channel. The client
reconciles authoritative broadcasts against its optimistic overlay in
`src/state/clientReconcile.ts`.

Bots are real players. They're seeded as rows in `seed.sql` with a `strategy_key`,
driven by a leased background loop (`bot_bump` + a Postgres heartbeat cron that
renews/fences the lease), and they choose moves through the same strategy
interface used everywhere else — including **Cordite**, the strongest one
(see below).

## Tests

`npm run test:e2e` runs the **actual deployed server modules** (the `_shared`
handlers, `commit_game` and the bot-lease plpgsql, the broadcast path) and the
**actual client reconciliation logic** against a real Postgres — only PostgREST +
Realtime are shimmed by a small `pg` adapter. Nothing about gameplay is mocked;
the fuzz/rearrange tests have already found and fixed real card-duplication
exploits. Setup and the full check matrix are in [`e2e/README.md`](e2e/README.md).

---

## The four projects under the hood

What makes this repo unusual isn't the card game — it's that one canonical
ruleset is implemented and kept in lockstep across several engines, which lets
games flow between them: a match simulated in native C can be replayed in the
browser, shared as a QR code, and re-fought by a Monte-Carlo bot that provably
never cheats.

### 1. `cnitro/` — the C Durak engine (now THE engine) and bot arena

A self-contained C engine whose job began as simulating **millions of games**
to evaluate bots without the TS language-boundary cost — and which is now the
**production rules engine itself**: its kernel (`game.c` + `legal.c`) compiles
to WebAssembly and executes every live move on the server (see
[Shared code and the C rules kernel](#shared-code-shared-and-the-c-rules-kernel)).
A game played in C isn't just *legal* on the production server — it runs the
same machine code path. It ships a full bot ladder — `random` → `espresso`/`handwritten`
→ `robusta`/`firecracker`/`gunpowder` → `blackpowder` → **`cordite`** (ELO #1, beats
every other bot at every player count) — plus tools for head-to-head evals, a
mixed-pool ELO arena, and seeded move-by-move replays. Cordite is a
**belief-constrained determinized Monte-Carlo** player with an exact endgame solver
that derives hidden information by deduction rather than peeking, under a strict
**no-LLM / no-cheating** contract. It runs live as the `cordite` /
`cordite_max` / `fulminate` bots — the C implementation itself, compiled into
`bots.wasm`. See `cnitro/README.md`, `cnitro/CORDITE.md`,
`cnitro/BLACKPOWDER.md`, and `cnitro/CORDITE_RESEARCH.md`.

```bash
cd cnitro && make
./build/cnitro_eval --strategy=cordite --opp=espresso --players=4 --games=500
./build/cnitro_elo  --games=3000 --pcs=2,3,4,5,6,7,8 \
    --pool=random,handwritten,espresso,robusta,firecracker,gunpowder,blackpowder,cordite
```

### 2. The "nitro" ML track — a from-scratch game-playing neural net

`supabase/functions/_shared/strategies/nitro_nn.ts` is a complete **transformer
implemented in plain TypeScript** (embeddings, positional encoding, multi-layer
single-head attention, LayerNorm, FFN) with no ML dependencies, serialized to
`Float32Array` for fast in-browser inference. Trained-weight files are checked in.
It features a canonical trump representation (suits rotated so trump is always
suit 0) and atomic-action decomposition so training matches autoregressive
inference. A separate NEAT neuroevolution experiment lives in `offlinefun/neat/`,
and the full data → train → held-out-eval pipeline is in `offlinefun/localtest/`.
Honest status: both ML tracks plateaued below the hand-written/MC ceiling and were
superseded by Cordite — they remain in the tree as experiments, not as live bots.

### 3. The replay codec — a whole game in a QR code

`src/replay/` and `supabase/functions/_shared/replay/` encode a complete finished
game into a single integer using **rANS entropy coding**, then base32 it into a
URL (`WWW.FOOLISH.CARDS/<code>`) chosen specifically to stay inside QR
alphanumeric mode. One shared driver runs both encode and decode, derived events
(deals, draws, discards) cost zero bits, hidden cards are encoded lazily with a
hypergeometric model, and an optional blob packs player names + per-move timing
spanning nanoseconds to weeks. The format is version-frozen (v2–v5), the server
verifies the round-trip byte-for-byte before persisting, and playback is a
VHS-style transport that can even deduce and reveal the loser's never-played cards
by complement.

### 4. Procedural rendering + offline-first PWA

The client ships **zero texture image files** — every surface is computed in the
browser and cached in IndexedDB: a Barnsley-fern IFS fractal on the card backs
(CPU compute → WebGL render, with a 2D-canvas fallback), woven-wool backgrounds,
parametric wood grain, and seeded-noise concrete. There's an optimistic-animation
system that auto-reverts cards when the server invalidates a move, three languages
(en/ru/ko) where selecting Russian flips the whole app to a sharp-edged "Soviet"
theme, and a PWA/service-worker layer in `offlinefun/` (`sw.js`, `ModeContext`,
`NetworkStatus`) with offline detection and cache warming.
</content>
</invoke>

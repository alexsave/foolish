# Foolish

A дурак (Durak) card game.
Play it in a browser at **[www.foolish.cards](https://www.foolish.cards)**, or inside an iMessage thread with **Foolish for iMessage** on the App Store.

That is the product.
The repository is a stranger thing, and this file exists because the previous version of it described a Next.js app with three languages and a C folder for bot research.
None of that is true any more.

## The one sentence

**There is exactly one implementation of Durak in this repository, it is written in C, and everything else is a way of reaching it.**

The website, the iPhone app, the iMessage extension, the Supabase edge functions, a native C server, a discrete-event network simulator and a set of frozen Rust benchmarks all run on [`c/src`](c/src).
Five separate build trees compile those same files: `c/Makefile`, the iOS xcframework, `server/impls/native/`, `foolyard/` and `rust/`.
None of them contains a rule.
A client that disagrees with the kernel is a bug in the client, and a good deal of the test suite exists only to say so out loud.

This was not a plan, it was a conclusion.
The TypeScript rules engine and the TypeScript bots were **deleted**, after a differential harness replayed about 100,000 mirrored actions plus 30,000 adversarial probes through both engines with identical seeds and byte-compared states, logs, events and rejection messages.
Zero divergence, so one of them was redundant.

By volume this stopped being a web app a while ago:

| | lines |
| --- | --- |
| C and headers | 93,026 |
| Swift | 65,667 |
| TypeScript and TSX | 50,956 |
| Markdown | 35,348 |

The Next.js client is 22,939 lines, which makes it the fourth-largest tree in the repo.
It has seven runtime npm dependencies.
2,024 commits and 201 pull requests since June 2025.

---

## Quick start

```bash
npm install
npm run dev          # http://localhost:3000
```

`predev` runs `npm run gen` first, and that matters more than it looks: **a large part of the TypeScript the client imports does not exist in git.**
[`tools/structgen`](tools/structgen) and [`tools/datagen`](tools/datagen) read the C through libclang at build time and write it.
A checkout without a working `clang` cannot build the website, which is why CI installs libclang and why [`scripts/ci_llvm.sh`](scripts/ci_llvm.sh) exists.

The dev server needs a Supabase backend, configured through two client-exposed variables:

- `NEXT_PUBLIC_SUPABASE_URL`
- `NEXT_PUBLIC_SUPABASE_KEY`

| Script | What it does |
| --- | --- |
| `npm run gen` | Regenerate the TS and Swift modules from the C. Ten other scripts run it first. |
| `npm run dev` / `build` / `start` | The website. |
| `npm run test:e2e` | Full-stack tests against a real Postgres (see [`e2e/README.md`](e2e/README.md)). |
| `npm run test:validate` | The 17 derived gates in `e2e/validation/`. No database, no network, no compiler. |
| `npm run typecheck` | The app and the e2e suite, separately. |
| `npm run bot:game` | Headless bot-vs-bot games through the C kernel. |
| `npm run metrics` | The numbers the `metrics` workflow posts on a pull request. |

```bash
cd c && make tests           # 200 C tests in one binary
make -C c difftests          # the differential suites
make -C c tests-asan         # the same, under ASAN + UBSAN
```

For the Apple side see [`ios/README.md`](ios/README.md); for the engine, [`c/README.md`](c/README.md).

---

## The map

```
c/            THE KERNEL, codename cnitro. 39,417 lines in c/src alone.
  src/        rules, legality, dealing, per-seat view masking, the replay
              codec, four wire formats, the animation planner, the table and
              session layer, the post-game analyser, 21 bot strategies, an
              exact endgame solver, and 8 CLI tools.
  i18n/       every string all three products render, as C data: 25 languages
              x 388 keys, no holes. Linked by nothing - it is a generator's
              source, not a translation unit (see below).
  ios/        the flat `fio_*` C bridge, 117 functions.
  wasm/       the WebAssembly entry points.
  tests/      11 files, 18,134 lines, including three differential harnesses.
  tools/      research tooling: the precomputed endgame book, a retrograde
              solver, a wasm section analyser, transposition-table dataviz.

sdk/          One thin binding per language. No game logic lives here, ever.
  ts/         wasm instances, FFI, packed wire layouts. Loaded by the website,
              the edge functions and the e2e harness.
  swift/      EngineC.swift plus the codecs. Compiled into FoolishKit.

tools/        structgen reads struct LAYOUT and never a value; datagen reads
              table CONTENTS and never a layout. Both via libclang. Both emit
              TypeScript and Swift.

src/          The Next.js client. Routes: /, /:game_id, /about, /tutorial,
              /dashboard, /leaderboard, /history, /m/:payload, /delete-account.
  oracle/     the Infinite Oracle - in-browser move-strength analysis over a
              paused replay decision (docs/INFINITE_ORACLE_DESIGN.md).
  utils/      procedural texture generation: fractal, wool, wood, concrete.

server/       Host-neutral backend. api/ is the contract and is 306 lines,
              because the kernel does the work.
  impls/supabase/  the deployed one: 5 edge functions, seed.sql, the bot loop.
  impls/native/    a single long-lived C process holding every game in RAM,
                   with its own epoll loop, WebSocket and WebTransport stacks,
                   a load client and two fuzzers. Linux only. Not deployed.
                   It exists to prove the server API is language-agnostic.

ios/          Ten Xcode targets, generated from project.yml by xcodegen.
  FoolishKit/      the shared UI framework, extension-safe by construction.
  FoolishMessages/ the iMessage extension - the shipping product.
  FoolishNet/      the only module allowed to link Supabase, enforced by a
                   lint script that fails the build.
  WatchUI/         a finished watchOS design, parked and deliberately not built.
  Tools/rig/       drives the real extension inside Apple's real Messages app
                   on a simulator, deterministically, for QA and store photos.

e2e/          217 files against real server code and a real Postgres.
  validation/ derived gates: tests that walk the repo and assert about it.

foolyard/     a discrete-event simulation of the whole table: simulated server,
              real kernel, clients on modelled wires, time as a number that
              only moves when an event says so.
rust/         a frozen experiment whose answer was "do not rewrite the kernel
              in Rust". Built by nothing. Read its README before assuming.
experiments/  a WASM Component Model bake-off. A recorded negative result.
offlinefun/   the PWA and service-worker layer.
docs/         62 design documents.
cnitro/       one orphaned header from a rename. Nothing builds from it.
```

---

## The kernel

`c/src` is the engine, and the split inside it is roughly half rules and half bots:

| | lines |
| --- | --- |
| pure rules core (`game`, `legal`, `deal_rng`, `card`, `view`) | 3,484 |
| replay codec | 3,743 |
| wire formats (action, events, the iMessage envelope) | 3,840 |
| animation planner | 2,965 |
| table and session layer | 3,098 |
| post-game analyser | 1,504 |
| 21 bot strategies and their infrastructure | 18,796 |

It ships as WebAssembly to the web and the edge, and as a static `xcframework` to Apple, both built from those same sources by a 1,411-line `c/Makefile` that is mostly prose.

**A move is bytes the whole way.**
It leaves the browser as a packed action wire, the exact bytes the client's own wasm validated, POSTed as a binary body to a single `action` edge function.
The server maps the caller to a seat and hands the bytes to the kernel, which in one synchronous section loads the persisted state blob, validates and applies the move, finalizes a win, and emits the new blob plus a per-recipient masked animation stream.
"You only see your own hand" is computed in [`c/src/view.c`](c/src/view.c), not in TypeScript.
The blob commits under an optimistic-concurrency compare-and-swap, then broadcasts over Supabase Realtime on per-player channels plus a public spectator channel.
The client decodes back to JavaScript only at the React render boundary.

**The strings are C too.**
[`c/i18n`](c/i18n) holds 25 languages and 388 keys as designated initializers, one file per language, and `tools/datagen` writes both the website's table and FoolishKit's from them.
Neither host is the other's upstream, which was the whole problem before: the phone carried 25 languages in a 5,063-line Swift table while the website carried three in TypeScript, they shared ten key names, and they disagreed about sixteen of the thirty cells those ten covered.
Per-language files rather than one grid is a bundle decision, so a visitor downloads the one language they read.
None of it is linked into anything, because 25 languages is about 150 KB of string data and the shipped wasm has an 80 KiB budget.
That is not a comment you have to trust: a test gunzips the shipped module and searches it.

**The bot roster is C.**
[`c/src/bot_roster.c`](c/src/bot_roster.c) is the single source of truth for what a named bot *is*, read by the database seed, the edge functions, the iOS bridge and the wasm.
The ladder players meet runs `random` → `simple_heuristic` → `handwritten` → `espresso` → `robusta` → `firecracker` → `gunpowder` → `blackpowder` → `cordite` → `octogen`.
Eleven more strategies exist for the arena only and are deliberately not linked into the shipped module; one of them, `novichok`, is a cheating-ceiling probe that **must** stay unreachable from the roster.
Cordite is a belief-constrained determinized Monte-Carlo player with an exact endgame solver that deduces hidden information rather than peeking, under a strict no-LLM, no-cheating contract.
Octogen is built on it and sits at the top.

---

## Nothing crosses a language boundary by hand

`tools/structgen` asks clang for *shape* and never reads a value.
`tools/datagen` asks for *contents* and never reads a layout.
Between them, the TypeScript and Swift that describe C structures and C tables are build outputs, regenerated on every lane and committed nowhere.

The generated files are not in git on purpose, and the reason is a specific incident: a committed artifact under the generated directory was excused from the freshness check by one `diff -x` flag and rotted for months.
So `gen.sh --check` no longer asks "does the committed copy match".
It generates twice, in two processes into two temporary trees, and refuses a difference.

The other half of that handshake is a layout hash.
Every wasm and iOS module is compiled with `-DSG_LAYOUT_HASH=<hash of its own struct layout>` and the host verifies it on first call.
A stale module is a startup refusal, never a silently wrong byte offset.
That guard exists because the alternative already happened: a kernel fix that shipped, was tested, and was absent.

---

## The side projects

Several things in here would be their own repository anywhere else.

**The replay codec.**
[`c/src/replay.c`](c/src/replay.c) encodes a complete finished game into a single integer with rANS entropy coding and base32s it into a URL short enough to stay inside QR alphanumeric mode.
One shared driver runs encode and decode.
Derived events cost zero bits, every hidden card's identity is entropy-coded at the moment it is dealt, and an optional blob packs player names and per-move timing spanning nanoseconds to weeks.
The server verifies the round trip byte for byte before persisting.

**The iMessage game has no server.**
The entire game state rides the `MSMessage` URL and is replayed locally on each device, 2 to 8 players.
The shipped extension links no network stack at all, and that is verified on the archive rather than asserted: `otool -L` shows no Supabase, CFNetwork or Network.framework, and a symbol grep for supabase, realtime, postgrest, gotrue, websocket and URLSession returns nothing.
Keeping the bot ladder out of that bundle takes enforcement at two levels, C archive and Swift module, because either one alone can be undone by link order.

**Procedural rendering.**
Almost every surface is computed in the browser and cached in IndexedDB rather than downloaded: a Barnsley-fern IFS fractal, woven wool, parametric wood grain, seeded-noise concrete.
The one exception is the Khokhloma card-back pattern, which is a real PNG.

**The Infinite Oracle.**
In-browser move-strength analysis over a paused replay decision, with a second shared-memory-threaded wasm build behind a cross-origin-isolation flag that is off by default because its blast radius is the whole site rather than the replay route.

**foolyard.**
A single-threaded deterministic process simulating the server, the clients and the wires between them, with the real kernel deciding every rule.
It exists to reach states a fuzzer structurally cannot: stale-version moves landing, interleaved bot cycles, double-applied retransmits.
It found that a push-only unordered transport strands a client at zero packet loss.

**rust/.**
Four kernel hot paths ported to safe Rust once, benchmarked against the shipped C, and frozen.
The answer the experiment produced was "do not do the rewrite", and its README opens by telling future readers to stop assuming someone is mid-port.

---

## Tests

`npm run test:e2e` runs the **actually deployed** server modules, the real `commit_game` plpgsql, the real broadcast path and the real client reconciliation against a real Postgres.
Only PostgREST and Realtime are shimmed.
Nothing about gameplay is mocked, and the fuzz and rearrange suites have found real card-duplication exploits.

`e2e/validation/` is a different animal: gates that **derive** their answer by walking the repo, because a hand-written list rots silently and several already did.
Nothing the web bundle imports may live under a path the deploy filter skips.
The generated modules must match what the C currently says.
The i18n source of truth must stay in C.
The Apple App Site Association must name the Team ID the Xcode project actually signs with.
Every one of those was written because the un-derived version of it had already failed green.

On the C side, `c/tests/tests.c` is 9,972 lines and 200 tests in a hand-rolled single-binary harness, alongside three differential suites that check the bitboard rollout engine, the apply path and the solver against the struct engine.
`server/impls/native/sem_fuzz.c` is a semantic anti-cheat fuzzer: it links the kernel directly, deals real games, plays them with the bot driver, and between moves fires well-formed-but-illegal frames at the apply path with full ground truth, asserting invariants including no-phantom-card.

Nine GitHub Actions workflows: `web`, `wasm`, `memory`, `validate`, `coverage`, `deploy`, `foolyard`, `ios` and `metrics`.
The Apple suite (847 Swift tests plus snapshot tests) runs on a Mac by hand via `ios/scripts/mac_tests.sh`, deliberately, because macOS Actions minutes bill at ten times Linux.

---

## Deploy

**CI builds, Vercel never does.**
The Git integration is disconnected on purpose.
[`.github/workflows/web.yml`](.github/workflows/web.yml) generates the modules, builds with libclang present, and uploads a prebuilt bundle.
Previews are **opt-in** via a `preview` label on the pull request, because Vercel's free plan allows 100 deployments per day across the whole account and a day of C and iOS work will otherwise spend them all and lock production out.
A merge to `main` always deploys production, and [`scripts/web_deploy_scope.sh`](scripts/web_deploy_scope.sh) decides whether a change could have reached the bundle at all.

The Supabase side deploys through [`.github/workflows/deploy.yml`](.github/workflows/deploy.yml).
The 39 migrations were folded into `seed.sql`, so a merge to `main` runs no SQL.

The shipped wasm is still built by hand on a Mac and committed; CI compiles every target but never rebuilds the shipped bytes.
[`scripts/check_wasm_freshness.sh`](scripts/check_wasm_freshness.sh) is what keeps that honest, and it exists because `public/oracle.wasm.gz` once sat unrebuilt for three weeks and served one seat the opposite endgame verdict.

---

## Where to read next

- [`c/README.md`](c/README.md) and [`c/CORDITE.md`](c/CORDITE.md) for the engine and the bots.
- [`sdk/README.md`](sdk/README.md) for why the bindings are thin.
- [`ios/README.md`](ios/README.md) for the Apple targets and the bootstrap.
- [`e2e/README.md`](e2e/README.md) for the test matrix.
- [`docs/ARCHITECTURE_REVIEW.md`](docs/ARCHITECTURE_REVIEW.md) for the last full audit.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the wire.
- [`ROADMAP.md`](ROADMAP.md) for the feature-gap review, which is dated July 2026 and predates the C consolidation.

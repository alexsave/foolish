# iOS Milestone B — what landed, and what B4 now needs

**Status: A4 is verified and mergeable. B4's kernel half is done; B4's SwiftUI
half is the next work and is blocked on nothing.**

Branch `claude/ios-design-combined`, commits `21386a7` + `fac52b9` on top of
`8782e2f` (A4).

Read §2 before writing any animation code — it changes what you're rendering
from what the previous handoff described.

---

## 1. P0 is done: A4 is verified

The previous handoff asked for two things before any Swift work. Both are done,
and both answers differ from what it predicted.

| check | result |
|---|---|
| `npm run test:e2e` (full) | **exit 0, 0 failures** |
| `npm run test:validate` | **39/39** |
| `cnitro_tests` | **403/403** |
| `xcodebuild test` | **20/20** |
| `ios-smoke`, `ios-view-test`, goldens | green; goldens byte-unchanged |

**The two e2e failures were not a regression.** They were
`belief_human_freshness` and `belief_logs_wiring`, both failing with
`octogen never chose through the bot loop` (`choices=0`). Both spied on
`WasmBotStrategy.chooseMoveDirect` — a seam A2/F2 moved into the kernel, so the
monkey-patch captured nothing. `bot_actions.ts` (~313-320) still reloads the
session log whenever a human is in and only reuses the resident on a bots-only
cycle; the loop was never wrong. Fixed in `21386a7` — see §3.

**There is no wasm toolchain skew.** The previous handoff's "rebuild with the CI
toolchain (mine is skewed 21 B)" was chasing a ghost:

- `bots.wasm.gz` rebuilds **md5-identical** to the committed artifact,
  compressed and uncompressed. A byte delta means *your change*.
- **CI never rebuilds the wasm** — it ships the committed `.gz`
  (`cnitro/Makefile:264`). There is no "CI toolchain" to match.
- The real trap: the Makefile's `WASM_CC` defaults to plain `clang`, which on
  macOS is Apple clang and **cannot target wasm32 at all**. It is `WASM_CC=`,
  not `CC=`:

```
cd cnitro && make wasm-bots WASM_CC=/opt/homebrew/opt/llvm/bin/clang
```

---

## 2. B4's kernel half landed — read this before rendering

**The events now carry per-step board state.** `GameEvent.state: GameView?`
(`fac52b9`).

Why it matters: `evwire` has always derived the board as of each step and handed
it to the sink as `EvwEvent.snap`, and the web's packed wire carries it per event
(`snap_len`) and commits it as each animation lands. **The iOS JSON sink was
dropping it.** A `bot_drive` cycle applies several actions in one call, so with
only the final view you could draw the END of a cycle and nothing in between —
and the only route back to the intermediate boards would be deriving them in
Swift, i.e. rebuilding the thing `BoardDiff.swift` was cancelled to prevent.

**So the render loop is: play an event → commit its `state` → play the next.**
Not "diff two views". Not "replay events against the final view".

- `state` is `Optional`. `nil` means the kernel emitted no snapshot for that
  event — render nothing extra. It is never a cue to derive one.
- `cards` entries arrive `null` where a card was dealt/drawn into a hand that is
  not the viewer's. Render a back; the identity never crossed the bridge.
- Events are already per-viewer. Do not re-mask.

Spec: `IOS_APP_DESIGN.md` §16.B4 (amended). Wire contract: `cnitro/src/evwire.h`.

---

## 3. The belief probe (why the tests look different now)

`21386a7` re-pointed the two belief guards at the kernel rather than restoring
the dead spy. The reason is worth keeping: **a TS spy can only prove the loop
handed the bytes over — never that `importLogsPacked` spliced them into the
`Game` the strategy actually read.** That gap is exactly where "octogen chose
blind" and the cordite stale-belief bug lived.

```
wasm_belief_probe_reset()   // clear + arm; OFF in production
wasm_belief_probe_dump()    // -> IO buffer; 11 B/record:
                            //    u8 seat, u16 n_logs LE, u64 card mask LE
                            //    (mask bit = suit*16 + value)
```

TS wrappers: `wasmBeliefProbeReset` / `wasmBeliefProbeDump` in `wasm/bots.ts`.

It hangs off the **existing** `bot_drive_pre_action_hook` at
`BOT_DRIVE_PHASE_CHOOSE` — `bot_drive.c` is untouched. A reused preferred move
fires no CHOOSE phase (no search → no belief read) and correctly records
nothing. Cost: +563 B uncompressed / +285 B gzipped; off until armed, so
production drives never pay the log pass.

The guards now assert something real:

```
[human-freshness] octogen decisions=43 (with prior human cards: 39)
[wiring] searches=25 maxKernelLogs=41 persistedSessionLen=43
```

`belief_logs_wiring` deliberately keeps a `mock.module` wrapper on
`wasmBotDrive` for its byte-prefix check — the resident log's concat-and-carry
arithmetic is TS-side work, not something the kernel knows. **If you find
yourself moving that to C, stop:** you'd be inventing kernel machinery to
observe a TS bug.

---

## 4. The next work: B4's SwiftUI half

`TableView.swift:43` still does `.animation(..., value: view)` — springs the
whole board on the value. That is the placeholder §16.B4 calls out.

State: `LocalGame.lastEvents` is published (`LocalGame.swift:31,93,144`), decoded
as `[GameEvent]`, and now carries `state` per event. **Nothing consumes it.**

Build the `AnimationPlan` §16.B4 specs: `matchedGeometryEffect` with a
`@Namespace`, ids = card identity (`"\(s)-\(v)"`, backs get synthetic slot ids),
single §5.2 spring, deal staggered 40 ms. Drive it off the event stream
(`EVW_*` codes) so a card flies by the same plan the website plays.

**Do not write `BoardDiff.swift`.** It is cancelled (§4.4/A3). If you're deriving
which card moved where in Swift, stop — the kernel already said.

Then in order: B3 table composition polish → §5 motion/haptics → tutorial port
(not started; only `FStrings.swift` mentions it). B's DoD is "complete offline
game feels finished; radio-off test passes; **60fps dealing**" — that last one is
a real constraint on the animation design, not a nice-to-have.

---

## 5. Running things (the parts that cost time to rediscover)

**e2e needs its own Postgres — not the supabase container.**
`e2e/adapters/supabase.ts` talks to `127.0.0.1:5432`, `stress/stress/foolish`.
`supabase start` is NOT enough: with only supabase up you get
`relation "games" does not exist`, which means "no e2e postgres", not "missing
migrations" (the supabase DB on :54322 has a perfectly good `games` table —
which is what makes it misleading). CI stands it up as a `postgres:16` service.

```
docker run -d --name foolish-e2e-pg \
  -e POSTGRES_USER=stress -e POSTGRES_PASSWORD=stress -e POSTGRES_DB=foolish \
  -p 5432:5432 postgres:16
```

The full suite is ~20 min (`belief_logs` alone ~100 s). Background it; a quiet
20 minutes is not a hang. **Never run two suites at once** — `resetDb()`
TRUNCATEs shared tables and will corrupt the other run. Also: don't rebuild
`bots.wasm.gz` while a suite is running; it loads the `.gz`.

**iOS loop:**

```
cd cnitro && make ios-lib
cd ios && xcodegen generate
xcodebuild -project Foolish.xcodeproj -scheme Foolish \
  -destination 'platform=iOS Simulator,name=iPhone 16' test
```

⚠️ **`xcodegen generate` silently wipes `ios/FoolishApp/Foolish.entitlements`**
to an empty `<dict/>`, destroying the `applinks:foolish.cards` universal-links
entry (§16.C5). It reports only "Created project"; the file is tracked, so it
lands in your diff looking intentional; everything builds and tests fine without
it, and it would only surface as broken universal links at Milestone F.
**`git checkout ios/FoolishApp/Foolish.entitlements` after every run.** Proper
fix when someone cares: declare the entitlements in `ios/project.yml`.

The Xcode job is **not CI-gated** (commented out, `ios.yml:41-47`), so lean on
`ios-smoke` + the goldens + snapshots. Note `make ios-goldens` **regenerates**
`ios/Fixtures/goldens.json` rather than checking it — verify via `git diff`.

---

## 6. Still deferred (unchanged)

- **Retire `rules.wasm`** — owner's call was "once things work, we can split
  them back", i.e. after the product works. Not now.
- **A5 (replay steps in C)** — gated to "before native replay polish", i.e.
  Milestone C. Do it when C starts; it deletes ~800 lines of TS then.

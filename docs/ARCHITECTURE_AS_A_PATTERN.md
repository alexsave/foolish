# Foolish as an architectural pattern

This document generalizes what the WASM port of Foolish actually *is* — lifted
off the card game — into a reusable way to build web applications, and a
reusable playbook for making them fast and small. It is written for someone who
has never seen Durak and never will. The card game is only ever cited here as a
worked example.

It has three parts:

1. **What this architecture is** — the concrete pattern, extracted.
2. **How to build a web app this way from scratch** — structure and order,
   including the C-first / C-server variant.
3. **The generalized performance/memory/size playbook** — the five moves.

---

## Part 1 — The architecture, extracted

### The one idea

The whole thing hangs off a single principle applied without compromise:

> **The domain logic has exactly one implementation, in a portable compiled
> language, and it runs unchanged in every environment.**

In Foolish the game rules live once in C (`sdk/c/src/game.c` + `legal.c` +
`view.c` + `replay.c`). That C compiles to WebAssembly and executes on the
server (Deno edge functions), in the browser, and in Node (tests, offline
sims). There is no TypeScript copy of the rules "for the client" and no separate
server implementation. The TS that remains is a thin marshaling bridge.

WASM is what makes the "shared core" *real* instead of aspirational. Most
"isomorphic" code sharing degrades into two implementations that drift; a
compiled kernel guarded by a byte-level test seam cannot drift, because there is
only one set of bytes.

### The four layers (true for any app)

| Layer | What it is | Foolish | Generic |
|---|---|---|---|
| **Core** | pure `reduce(state, command) → events` + validation + authorization | `game.c` / `legal.c` | your domain model + business rules |
| **Contract** | packed byte formats for state, commands, events, views | `wasm/wire.h` | one schema, included everywhere |
| **Projections** | `project(state, viewer) → bytes`; the *only* serializer; redaction lives here | `view.c` | per-role / per-tenant views + masking |
| **Shell** | HTTP, DB, sockets, auth, rendering, external APIs — all impure, all thin | edge fns / client | per-environment plumbing |

The Core and Projections compile once and run in every runtime. The Shell is
written per environment and is deliberately dumb.

### The concrete pieces worth stealing

1. **Single kernel, three runtimes.** One C source, compiled to three
   *specialized* modules via compile-time flags:
   - `guards.wasm` — client, validate-only, 64 KB, pinned to one L1-sized page.
   - `rules.wasm` — server, full apply + masking + events.
   - `bots.wasm` — superset with the Monte-Carlo strategies.

   Same files, different `-D` caps and different `--export` allow-lists. You do
   not ship one fat module everywhere; you ship a *tailored* module per call
   site.

2. **The packed-wire boundary.** Game state crosses client→server→client as
   kernel-produced packed bytes; JavaScript objects exist only at the React
   render boundary and the DB edge. A move leaves the browser as the exact
   action-wire bytes the client validated, and those same bytes are what the
   server applies. Measured: per-move server compute 45.9 µs → 14.8 µs (3.1×).

3. **Per-viewer masking computed inside the kernel** (`view.c`). "You only see
   your own hand" is a C function, not a TS filter. The server never
   materializes an unmasked object on the hot path — other players' hidden data
   leaves the kernel already redacted (`0xFE`), with counts preserved. Combined
   with column-level DB grants (RLS cannot hide a column; grants can), the
   masked path is the *only* path. Redaction is a property of the serializer,
   not a step you must remember.

4. **The differential-parity seam.** The one enabling discipline. Before the TS
   engine was deleted, a harness replayed ~100k mirrored actions + ~30k
   adversarial probes through both engines with identical seeds and byte-compared
   everything. The retired TS is frozen as a differential oracle. A single source
   of truth is only trustworthy if a cheap oracle proves the seams.

5. **The replay codec as a transport format.** A whole finished game
   rANS-entropy-coded into one integer, base32'd into a QR-safe URL. Derived
   events (deals, draws) cost zero bits because the decoder re-runs the kernel;
   only genuinely random bits are paid for. The server verifies the round-trip
   byte-for-byte before persisting.

6. **Optimistic overlay + version-gated reconciliation.** Every commit carries a
   monotonic CAS version; out-of-order broadcasts are dropped by version;
   optimistic moves auto-revert on reject. A pure, unit-tested reconciler is
   imported by both the app and its tests — no second copy.

7. **Procedural, asset-free rendering.** Zero texture files; fractals, wool,
   wood grain and concrete are computed in-browser and cached in IndexedDB. You
   ship the generator, not the pixels.

---

## Part 2 — Building a web app this way, from scratch

### Generalize the app to a state machine

Every web app is: `reduce(state, command) → (newState, events)`, plus
projections of state for viewers, plus I/O plumbing. This is the
**functional-core / imperative-shell** principle taken to its literal extreme:
the functional core is not a folder, it is a separately compiled artifact that
*cannot* import a socket or a database because it is freestanding. The boundary
is enforced by the compiler, not by discipline.

In textbook terms this is **event-sourced + CQRS + functional-core**. The novel
part is the last mile: the write model is a *portable compiled artifact*, so the
same validator/reducer also runs on the client — which buys optimistic UI and
offline-first for free from the same code.

### The hard discipline: what goes in the Core

The Core is a pure function. Anything non-deterministic or effectful is turned
into **data crossing the boundary**, never performed inside:

- **Time** → injected. The command carries `now`; the Core never reads a clock.
- **Randomness** → injected as a seed. Determinism is load-bearing (below).
- **External side effects** (charge a card, send email, call an API) → the Core
  does not perform them. It *emits an event describing them*
  (`PaymentRequested{…}`); the Shell performs the effect and feeds the result
  back as a new command (`PaymentSucceeded{…}`). The Core stays a pure decision
  function; the Shell is the only thing that touches the world.
- **Reads / joins the Core doesn't hold** → CQRS. The Core is the write model;
  complex read views are separate projections built from the event stream.

### What you get for free

Once the Core is pure, deterministic, and event-sourced, a lot of hard features
are consequences, not work:

- **Optimistic UI** — the client runs the identical reducer, predicts, reverts
  on disagreement.
- **Offline / local-first** — the Core runs in WASM with no network; sync is
  replaying the command log.
- **Audit log, undo/redo, time-travel debugging** — you already have the command
  stream; state is a fold over it.
- **Crash recovery & migrations** — rebuild state by replaying commands.
- **Testability** — the Core is a pure function; tests are `(state, cmd) →
  assert`. No mocks, no DB. Invariants and fuzzing *are* the spec.
- **Authorization you can't forget** — the masked projection is the only
  serializer.

### Repository structure (generic)

```
/proto     wire.{h,ts} — commands, events, state, views. One schema.
/core      pure reducer: reduce(state, cmd) -> events; validate; authorize;
           project(state, viewer) -> bytes.  NO I/O. Compiles to native + WASM.
/core/test invariants + property/fuzz + golden replays + cross-build agreement
/server    imperative shell: HTTP/WS, auth, persistence (command log + snapshots),
           and the EFFECT RUNNER that executes emitted effects and feeds results back
/client    WASM core (validate + optimistic predict) + a thin rendering skin
/read      CQRS projections for queries that don't belong in the write model
/ops       TLS + static serving (reverse proxy), migrations, deploy
```

The rule that keeps it coherent: `proto/wire.*` is the **only** place the byte
format is defined, `#include`d by every C target and mirrored in exactly one JS
file with a golden test asserting agreement. That single mirror is the entire
residual cross-language surface.

### The C-first insight: the parity tax mostly evaporates

If C comes *first*, there is no TS oracle to match — **the C kernel is the
spec.** What replaces parity testing is cheaper and is testing you want anyway:

1. **Invariant / property tests** — the real correctness oracle (conservation,
   legality, "serialize∘deserialize = identity"). These are domain *truths*, not
   a mirror of a second implementation, so they catch bugs a mirrored oracle
   would happily reproduce in both copies.
2. **Fast-vs-reference difftests, opt-in per optimization** — write the obviously
   correct slow version, difftest the fast version against it, inside one
   language. You pay this only on the parts you actually optimize.
3. **Cross-compile agreement smoke test** — client (WASM) and server (native)
   are the same source compiled twice, so they agree by construction; one corpus
   test catches compiler/flag divergence (e.g. float semantics — which is why
   `-ffast-math` is banned in the WASM build).

### The C-server unlock: in-memory authoritative state

Foolish's honest residual bottleneck was *"DB round trips, not compute"* —
because serverless edge functions are stateless, every move reloads state from
Postgres. A **stateful C (or Rust) server deletes that problem.** Run one
long-lived process that holds hot entities in RAM as the struct, single-writer
per entity (an actor/lock per id). A command becomes:

```
parse wire → lock entity → kernel reduce (operate on the resident struct)
  → append command to a write-ahead log → unlock → broadcast bytes over WS
```

No marshal in/out, no DB read on the hot path. The database is demoted to a
**durability log**: append commands to a WAL, snapshot periodically, and on
restart replay the WAL tail (determinism guarantees exact reconstruction — the
same property that powers replays). On the server, WASM disappears entirely; the
native binary links the Core and calls it directly. WASM is only the browser's
copy.

### Build order (each phase independently testable)

- **Phase 0 — the contract.** Write the packed byte layouts first. Commit to
  bytes before any code. Keep the in-memory struct layout independent of the
  wire so you can reorganize without a data migration.
- **Phase 1 — the Core, pure and headless.** `reduce(State*, Command) →
  events`. No sockets, no files. Build a REPL that plays on stdin/stdout on day
  one. A fuzzer that throws random commands and asserts invariants is your
  executable spec and never goes away.
- **Phase 2 — serialization + masking.** Round-trip identity; masked-blob
  property tests; deterministic golden transcripts as the regression net.
- **Phase 3 — the server, request/response first.** Link the Core. Embedded
  store (SQLite) with the blob as a column. Actor table of in-RAM entities +
  per-entity lock. WAL + snapshot durability. Two `curl` clients play a full
  game.
- **Phase 4 — realtime.** WebSocket hub; per-viewer masked event streams; drop
  stale broadcasts by version.
- **Phase 5 — the WASM Core.** Same source, validate-only + size-first config.
  The cross-build agreement smoke test is the entire residual "parity" cost.
- **Phase 6 — the JS UI shell.** Turn DOM interaction into command-wire bytes,
  gate on the WASM Core, POST the bytes, decode masked view/event blobs into
  objects only at the render boundary.
- **Phase 7 — optimistic overlay + reconciliation.** Pure, unit-tested, imported
  by both app and tests.
- **Phase 8 — replay codec + procedural assets (optional polish).**

### The fit spectrum (be honest)

The payoff scales with how much genuine, shared, deterministic logic the app
has. Ask: **would I need to run this exact logic in more than one place?**
(server authority *and* client optimism, or online *and* offline, or app *and*
worker *and* audit-replay).

- **Ideal:** collaborative editors (Figma literally does this — a C++ core
  compiled to WASM, same client and server), multiplayer/real-time/simulations,
  fintech/regulated (deterministic core + immutable command log = audit trail by
  construction), complex configurators/planners/rules engines/spreadsheets,
  local-first apps.
- **Marginal:** e-commerce/booking/workflow — apply it to the transactional
  heart (cart, pricing, inventory rules), leave catalog/CMS normal.
- **Overkill — do not:** CRUD-over-a-database, content sites, dashboards, most
  admin/marketing apps. If the app is fundamentally "fetch rows, render, write
  rows," there is no meaningful Core — the app *is* the I/O shell, and the wire
  format + build pipeline buys nothing.

### The new taxes of going C-server (honest)

- **Memory safety is on you.** Mitigate with a per-request arena allocator, a
  no-ambient-allocation Core, ASan/UBSan in CI, continuous fuzzing of the Core
  and the HTTP/WS parsers. **Rust gets ~90% of this plan with memory safety and
  the same native+WASM story** — weigh it seriously.
- **No ecosystem for the boring stuff.** Terminate TLS, HTTP/2, gzip, and static
  serving at a proxy (Caddy/nginx); the C app speaks plain HTTP/1.1 + WS on a
  socket and does only the domain API, keeping the hand-written attack surface
  tiny. Use a vetted HTTP parser and WS lib; do not hand-roll framing.
- **Stateful servers are harder to operate.** Restart = drain + snapshot +
  reload-from-WAL. Scale past one box by sharding entities by id (sticky routing)
  — easy because each entity is an independent actor, but a real design step.

---

## Part 3 — The generalized performance/memory/size playbook

Almost every specific optimization in the port is an instance of one of five
general moves. The unifying idea:

> **Remove work or space that the specific situation proves you don't need — and
> make that removal permanent and enforced, not a fragile one-time win.**

Not "make the code faster" but "prove a constraint about this exact deployment,
then collapse the program to fit it."

### 1. Optimize the binding constraint — and only that

Measure what actually gates the user; spend effort only there; recognize when a
curve has gone flat.

- Anchors: perceived latency dominated by the 500 ms animation, not compute;
  server residue is "DB round trips, not compute"; the bot's strength/compute
  curve is at its **saturation knee** (2×/4× search = 40%→40%→39% win rate), so
  the 15× speedup was **banked as lower latency, not strength**.
- Generalized:
  - Find the real bottleneck by measurement, not intuition (in web apps it is
    almost never "JS is slow" — it's round trips, waterfalls, hydration, N+1).
  - Detect flat parts of the curve and stop (cache past the working set,
    precision past the perceptible, search past the knee — all zero return).
  - Bank a surplus where it is felt: convert an unfelt speedup into latency,
    cost, headroom, or a smaller instance.

### 2. Turn limits into build-time invariants

A constraint checked at runtime is a bug waiting to happen; a constraint the
toolchain enforces cannot regress. Move failure left.

- Anchors: linear memory pinned (`--initial-memory == --max-memory`) so a buffer
  over budget **fails the link**; `_Static_assert`s fail the link if an arena
  overlay overflows; caps sized from a measurement harness with clean overflow
  (a dropped animation frame, never corruption).
- Generalized:
  - Budgets enforced by CI (bundle-size budgets that fail the build, a
    max-latency assertion in load tests, a container memory ceiling that
    OOM-kills in staging).
  - Make the safe path the only representable path (types, exhaustiveness,
    newtypes; "the masked serializer is the only serializer").
  - Overflow degrades cleanly, never corrupts (bounded queues shed load, ring
    buffers drop oldest, caps return a well-formed short result — choose where
    the failure lands and make it benign).

### 3. Specialize the artifact — ship only what the call site runs

Don't ship one general binary everywhere; compile a tailored artifact per call
site from one source and strip everything that site never executes.

- Anchors: three modules from one C source; the validate-only build compiles the
  entire logging + deck-refill paths to no-ops; explicit `--export` allow-lists
  let the linker dead-code-eliminate the rest; unused strategies dropped from the
  ship set; default `-Oz` with per-file `-O3` on only the five hot files.
- Generalized:
  - Route/component-level code splitting; `sideEffects:false`; dynamic imports.
  - Differential/per-target builds (modern vs legacy, per-platform, server vs
    client bundles from shared source).
  - Compile feature flags *out* at build time, not branch at runtime; exclude
    debug/instrumentation from prod.
  - Uniform optimization is wrong: optimize the hot 5% hard, build the cold 95%
    for *size* — on cold/short-lived runtimes (edge, serverless) a smaller
    artifact compiles and loads faster and the cold code never warms up.

### 4. Fit the data to the machine and the boundary

Layout against the memory hierarchy, and compactness at boundaries, often beat
the algorithm.

- Anchors: `guards.wasm` pinned to a single 64 KB page sized to fit L1d; the
  transposition table packed 16→8 bytes, made 2-way (one cache line per bucket),
  sized to one page to stay L1-resident; snapshots store only the struct prefix;
  `-mbulk-memory` uses the native `memory.copy`; cards cross as 1 byte, not a
  JSON object.
- Generalized:
  - Size hot working sets to a cache tier (Redis set to RAM, hot struct to L2,
    columnar batch to L1, critical CSS to one round trip).
  - Compact representation at boundaries beats parsing speed (protobuf,
    FlatBuffers, Arrow, binary WS frames; zero-copy reads over deserialization).
  - Copy less: serialize only changed fields, delta encoding, structural sharing
    over deep copies.
  - Use the platform's bulk primitives (SIMD, `memcpy`, columnar ops, bulk DB
    inserts).

### 5. Exploit liveness, stability, and non-overlap to reuse work and space

If two things are never live at once, share their space. If a result is stable,
compute it once. If work already happened, don't repeat it across a boundary.

- Anchors: buffers provably never concurrent (single-threaded module) aliased at
  the same address (~90 KB reclaimed as pure address reuse); state stays resident
  across calls (marshal-once → the follow-up apply is free); a CAS-fenced
  per-isolate cache skips the DB load, trusting the **version fence** not
  freshness; a precomputed endgame oracle terminates stable subtrees; replay's
  derived events cost zero bits.
- Generalized:
  - Overlap-in-time → share space (arena/bump allocators, object pools, buffer
    reuse). The safety argument is always a liveness argument.
  - Cache across the expensive boundary, fenced by a version/hash not freshness
    (ETags, stale-while-revalidate, DataLoader, memoization keyed by version) —
    a stale hit costs one revalidation, never a wrong answer.
  - Precompute the stable, derive the rest (SSG/ISR, CDN edge, materialized
    views, memoized selectors; send the seed, not the sequence).
  - Don't recompute across boundaries (pass the result, batch to amortize
    crossing cost, colocate compute with data so the round trip shrinks).

### The discipline underneath all five

The process matters more than the tactics:

- **Every optimization was measured**, before and after, with a real harness and
  a stated margin. Numbers or it didn't happen.
- **Every one is reversible via a knob** (`WASM_OPT=-O3`, a budget flag, the TT
  bits). Optimizations are hypotheses; keep them toggleable.
- **Every one is gated by correctness tests** (fuzz, parity, difftest). A faster
  wrong answer is worthless; the LTO-table-corruption bug was caught by the fuzz
  suite, which is *why* aggression was safe.
- **Done in the right order:** correctness first, then measure the binding
  constraint, then specialize/shrink, then re-pin the budget lower after each win
  so the ratchet only turns one way.

### The playbook, in one line

**Measure the actual constraint → prove what this deployment doesn't need →
collapse the program to fit → enforce that collapse at build time → gate it with
tests and keep a knob to undo it.**

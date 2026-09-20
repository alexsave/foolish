# Foolish as an architectural pattern

This document generalizes what this repo actually is, lifted off the card game, into a reusable way to build applications, and a reusable playbook for making them fast and small.
It is written for someone who has never seen Durak and never will.
The card game is only ever cited here as a worked example.

It has three parts:

1. **What this architecture is** - the concrete pattern, extracted.
2. **How to build an app this way from scratch** - structure and order, including the C-first / C-server variant.
3. **The generalized performance/memory/size playbook** - the five moves.

The evidence behind the claims here lives in four companion documents, cited rather than repeated:
`docs/C_GAME_SHAPE_MIGRATION.md` is the migration that produced this end state, phase by phase, with every measurement and every gate;
`docs/KERNEL_LIFT_BRIEF.md` is the Swift-to-C campaign and the boundary rule it works from;
`docs/C_CORE_CONSOLIDATION.md` is the audit that inventoried what was still duplicated between the kernel and each platform;
`docs/CODEGEN_ALTERNATIVES.md` is the measured refusal of the Component Model and of `embind`, over a prototype in `experiments/component-model/` that anyone can re-run.

---

## Part 1 - The architecture, extracted

### The one idea

The whole thing hangs off a single principle applied without compromise:

> **The domain logic has exactly one implementation, in a portable compiled language, and it runs unchanged in every environment.**

An earlier version of this document stopped there, and described the host languages as "a thin marshaling bridge" around shared rules.
The end state is stronger, and the stronger form is the interesting one:

> **The kernel owns the SHAPE, not just the rules.**
> No host language declares the domain's types, and no hand-written host code knows a byte layout.

The difference is concrete.
Shared rules still leave the host holding a `Game` interface, a `Player` interface, a status string table, and a hand-written function that walks the kernel's bytes into them - four restatements of the domain that can drift from it, and that drift silently because they typecheck.
Owning the shape means the host holds a pointer, a span, and values that a generator emitted from the kernel's own headers.

In this repo the rules live once in C (`c/src/game.c`, `legal.c`, `view.c`, `replay.c`), the identity of who sits where lives once in C (`roster.c`), the composition of the two that a server operates on lives once in C (`table.c`), the client's masked copy lives once in C (`client_table.c`), and the animation model - what moves, in what order, for how long - lives once in C (`anim_plan.h`).
That C compiles to WebAssembly for the browser, for Deno edge functions and for Node tests, and compiles natively for iOS and for a standalone server.
There is no TypeScript copy of the rules "for the client", no second server implementation, and no TypeScript or Swift declaration of what a game is.

WebAssembly is what makes the shared core real rather than aspirational.
Most "isomorphic" code sharing degrades into two implementations that drift; a compiled kernel behind a byte-level test seam cannot drift, because there is only one set of bytes.

### The mechanism: two static tests, not a convention

A rule this strong survives only if something fails when it is broken, and the something has to be cheap enough to run on every commit.

- `e2e/no_ts_game_shape.test.ts` parses the host source and fails if any file outside the generated directory declares an interface with two or more of the domain's own field names, or reads a kernel buffer with a `DataView` or a bit shift.
  Its shrink-only allowlist - the list of hand-packed entry points it was willing to tolerate - is empty, and the test asserts that it is empty rather than asserting a ceiling.
  Adding a two-byte read to a host file turns it red and names the file and the offending token.
- `e2e/table_no_game_object.test.ts` walks the import graph and fails if any server file imports the retired game, public-game, personal-game or player types, so the shape cannot come back through a type alias.

Anything the team writes later that restates the domain in the host language is caught by a test that nobody has to remember to run.
That is the pattern, and it generalizes past this repo: if a boundary matters, spend the afternoon writing the static check that guards it, because a convention is a boundary that decays at the rate people join the project.

### What the host keeps, deliberately

HTTP and its auth, database calls, realtime topics and subscriptions, timers and scheduling, process lifecycle, and rendering.
That is all.
The host knows column names, an RPC name, a topic format, and that a span is an offset and a length - never a status, never a seat, never a card, never a byte offset.

### The layers (true for any app)

| Layer | What it is | Here | Generic |
|---|---|---|---|
| **Core** | pure `reduce(state, command) -> events`, plus validation and authorization | `game.c`, `legal.c`, `table.c` | your domain model and business rules |
| **Contract** | the byte layouts for state, commands, events and views | the C headers themselves | one schema, and it is source code, not a sidecar IDL |
| **Bindings** | host access to the contract, generated from the contract | `tools/structgen` -> `sdk/ts/gen/*.ts`, `sdk/swift/gen/*.swift` | generated, never hand-written |
| **Projections** | `project(state, viewer) -> bytes`, the only serializer; redaction lives here | `view.c`, `client_table.c` | per-role and per-tenant views, masking |
| **Shell** | HTTP, DB, sockets, auth, rendering, external APIs - all impure, all thin | edge functions, React, SwiftUI, the native server | per-environment plumbing |

The Bindings row is the one this migration added.
Before it, the Contract was mirrored by hand into each host and policed by parity tests; after it, the Contract is parsed by a generator and the mirror does not exist.

### The concrete pieces worth stealing

**1. One kernel, built per target and per call site.**

The earlier version of this document sold "three specialized wasm modules from one source" as the headline.
That is no longer what ships, and the correction is instructive.
Two of the three modules are gone: the validate-only client module (`guards.wasm`) and the server-only rules module (`rules.wasm`) were deleted once the browser needed a real masked board of its own rather than a yes-or-no gate, and once the server stopped marshalling a host-built game into the kernel.
Keeping them would have meant three kernels answering the same questions, which is the drift the pattern exists to prevent.

What specialization survives is sharper than what it replaced:

- **Per target, not per role.** The same headers compile for `wasm32` and for `arm64-apple-ios`, under different capacity flags, and each build gets its own generated bindings and its own layout hash.
- **Per call site, by export allow-list.** The shipped module exports exactly what production calls; a second link of the same objects adds test-only entry points and is never committed (`make -C c wasm-bots-test`).
  Retiring the host-side game shape let 67 exports leave the shipped module, 56 of them rules and bot bridges that only the retired TypeScript shape had ever called.
  A test reads both export lists out of the makefile and holds the shipped module to none of the test entries and both modules to one layout hash.
- **Per workload.** The replay analyser ships as its own modules (`oracle.wasm`, `oracle-mt.wasm`), built from the same sources with a much larger transposition table, because a browser tab asked to think hard has a different memory regime from an edge function answering a move.

**2. Generated bindings as the replacement for hand-written marshalling.**

`tools/structgen` is a small C program that links libclang, split by domain: the clang traversal that asks for shape, the model it builds, and one unit per emitter.
It parses the real headers for one target under one build's flags and emits, from the layout clang computed:

- per-field accessors over the host's view of the struct;
- **snapshot readers** that copy a record out into a frozen value (arrays cut to their count field, `char[N]` to a string, nested records to nested values), so nothing the host holds points into memory the kernel writes next;
- **writers**, the inverse, for the boards a host builds and the kernel must read, refusing rather than truncating a value that does not fit;
- the enum constants and object-like integer defines under a named prefix, so no host copies a status number or an error code;
- a **layout hash** over field path, offset, size, kind and bit range - never over type spellings, so renaming a typedef does not churn it.

Three emitters run off that one clang-derived model: TypeScript accessors and snapshots over linear memory, Swift value types over the struct's own address for a host that links the kernel natively, and the hash itself.
The hash is **per target**, because the same structs do not have the same layout everywhere: the shared roots hash to `0x2c3f0c6d` for `wasm32` and `0xfeacf389` for `arm64-apple-ios15.0`.
A run that ignored that difference would have generated a plausible lie.

The hash is what makes the pair safe to ship:

- the browser compares the module's own `wasm_layout_hash()` against the generated constant at instantiate and throws on a mismatch;
- iOS has no instantiate, so `make ios-lib` bakes the hash into the static library and `KernelLayout.verified` compares them before the first call, for all three slices, because one stamped hash must not be right on two architectures and wrong on the third;
- `tools/structgen/gen.sh --check` in CI regenerates everything and fails on any diff, so a header edit without a regeneration cannot merge.

The generic tests are the honest part: C compiled from the same headers fills a fixture through its own field names, the host reads it back through the generated readers, and the two agreeing is the statement that every emitted offset is the offset `offsetof` would give.
The Swift suite went red by assertion first, with 34 failures against readers that returned zeros.

*Why not an off-the-shelf binding generator.*
The alternatives were measured rather than dismissed, on a prototype that is in the tree and re-runnable: `docs/CODEGEN_ALTERNATIVES.md` has the tool versions, the tables and the commands, over the sources in `experiments/component-model/`.
The Component Model with `jco` needed about 15 times the glue for the same two functions (12,556 B against 814 B gzipped, both minified the same way), made a round trip 12 to 15 times slower (8,053 ns against 543 ns on Node 26), and required an allocator inside a kernel that deliberately has none: 9 `cabi_realloc` calls per `import-state` on a four-player board, one for the record and one for every list in it, with no way to opt out.
Emscripten's `embind` brings a JavaScript runtime, an allocator and a libc into a module whose whole point is that it has none of those, and the same two functions cost 22 imports where the kernel has none, 11.8 times the module, and 34 to 38 times the round trip.
The conclusion worth carrying to another project: a tool of this kind is a **`bindgen`**, not an **`emcc`**.
It should read the types you already wrote and emit host-side access to them; it should not bring a runtime, a memory model or an ABI of its own.
At about 1,570 lines for three emitters it is a few days of work, not a quarter, and libclang is the right parser precisely because it is the same front end that laid the struct out.

**3. The import-free kernel, as a deliberate property with both sides written down.**

The kernel imports nothing.
No host function, no clock, no allocator, no logger, no random source.

What that buys:

- **One instantiation shape across five hosts.** A browser, an edge function, Node, a native server and a phone all instantiate or link the same artifact the same way, with no import object to keep in step.
- **No host can hand the kernel anything.** There is no seam through which a host's bug, a host's clock skew or a host's locale reaches a decision.
- **Determinism is structural, not promised.** A decision is a function of its inputs because there is no other place an input could come from.
- **Nothing to mock.** A test drives the kernel directly.

What it costs, and these are real:

- **No callbacks.** Anything a host would express as a hook has to be expressed as data crossing in or out.
- **No clock.** Every time-dependent answer takes `now_ms` as an argument.
- **Everything the kernel needs ships in it**, so a capability the kernel lacks is a kernel change and not a host injection.
- **Pull, not push.** An API that would naturally be "call me when this happens" has to become "ask me where things stand".

The worked example is the animation pipeline.
The obvious design is a plan plus callbacks, or a plan plus timers the host arms.
The web had exactly that: a serial chain of timeouts, each firing the next, with the arriving-message paths splicing into the queue the timer chain was walking.
A message landing mid-flight could only be answered by mutating that queue, which is where a family of glitches lived.

The import-free version is a pure function of the plan and the clock: `anim_plan_at(plan, now_ms, &frame)` answers where the run stands at a given moment - the step in flight and how far into it, how many have landed (which is the board to commit), the next deadline, the badges as of now, and the cards still in the air.
The host runs one `requestAnimationFrame` loop, asks at `performance.now()`, draws, and schedules nothing.
A step opens at `i * (duration + gap)`, a pure function of its index, so appending to a run in flight leaves every earlier step's timing exactly where it was, which is what lets an arrival be answered by the next call instead of by editing a queue.
A frame the browser skipped lands every step it skipped in that one frame, so a backgrounded tab returns to the board it should be holding instead of replaying the sequence one flight at a time.

The result is that a hidden cost of callbacks becomes visible: the second timer.
The board a predicted move left behind used to ride its own timeout in a different file, coupled to the flight it was meant to follow by nothing but the two reading one constant.
One plan and one clock make the frame where the board had advanced while the card was still in the air unrepresentable.
The audit that drove this is `docs/ANIM_TIMING_AUDIT.md`, written before any code changed so that what moved was a list and not a guess.

**4. The data-plane rule.**

Three rules decide what may cross the boundary and in which direction.

- **Fixed-size value structs cross.**
  A host receives a snapshot: a frozen copy, arrays cut to their counts, no pointer into kernel memory.
  This is also the rule that keeps a host safe across an `await`: the kernel has one resident slot per role, so anything held across a suspension point is something another caller can move underneath you.
  Load, operate, copy the products out, all synchronous.
- **Variable-length wires stay entirely in C.**
  A format with counted or nested records - the durable state blob, the event stream, the response envelope, the share link's codes - is parsed and written only by the kernel.
  A host moves it as opaque bytes and never walks it.
  The test for whether a fold is worth doing is not how many lines it deleted but **whether any host still knows the byte layout**.
- **Pointers may be read, never written.**
  A generated reader may follow a pointer when a count field says how far it goes; the generator refuses to emit a writer for any record that reaches a pointer, so a host can never hand the kernel an address.

The generator enforces the third rule and one more that only a machine would have caught: it refuses a writer over two arrays that share one count field, because a writer emits the count once per array and the second write would clobber the first.
A reader is happy with that pairing and a writer is not.
That refusal cost one planned deletion and is the correct trade: a payload that is correctly formed and wrong is worse than a file that stayed hand-written.

There is a fourth case worth naming, because four Swift files stayed hand-written on purpose.
When the bytes are a **form something travels in** rather than a struct anybody holds - a menu handed straight back to the kernel, a server's HTTP request envelope, a decoder's variable-length record stream - copying them into a value type is the wrong shape, and the property that mattered is not "no bytes here" but "no layout stated twice".

**5. Per-viewer masking computed inside the kernel.**

"You only see your own hand" is a C function, not a host-side filter.
The server never materializes an unmasked object on the hot path: other players' hidden data leaves the kernel already redacted, with counts preserved.
Combined with column-level database grants (row-level security cannot hide a column; grants can), the masked path is the only path.
Redaction is a property of the serializer, not a step someone must remember.

**6. The packed-wire boundary.**

State crosses client to server to client as kernel-produced packed bytes; host objects exist only at the render boundary and the database edge.
A move leaves the browser as the exact action-wire bytes the client validated, and those same bytes are what the server applies.
Historic measurement, from the port that introduced this: per-move server compute 45.9 us to 14.8 us, 3.1x.
Current measurement, after the host-side decoder was deleted in favour of a kernel decode plus a generated snapshot: at or below the retired hand-written reader at every table size (2-seat 1,431 ns/op against 1,612, 8-seat 3,024 against 3,933).
Generated access is not a tax you pay for safety here; it was faster than the hand-written code it replaced.

**7. The durable store: the kernel's blobs, plus the scalars SQL filters on.**

A row is the kernel's state blob, the kernel's roster blob, the few scalars a query actually filters or sorts by, and host bookkeeping (version, timestamps, leases).
The database stopped interpreting the domain: no populating a record from JSON, no status special cases, no second copy of the roster as JSON.
The write function raises on a missing blob, maps a status by index, and otherwise keeps only bookkeeping rules.

The end state was reached by **expand / switch / contract**, as three separate owner deploys, because the deploy pipeline pushes migrations and then functions and there is a window where both generations are live:

1. **Expand** adds the new columns, backfills them from the old ones in SQL, and installs a trigger so that every legacy writer keeps the new columns in step.
   A generation marker lets a legacy write refuse a row the new code already owns, rather than reload stale data and retry.
2. **Switch** deploys functions that read and write only the new shape.
3. **Contract** drops the old columns, the old functions, the bridge and the marker, and applies the final grants.

Measured result of taking JSON off the write path (blobs as base64, arrays as real array parameters): the commit body went from 5,986 to 2,414 bytes, **-60 percent**, and create's from 3,183 to 2,111, -34 percent, with latency indistinguishable before and after, including with a synthetic 10 ms round trip on every database call.
The lesson is the honest one: **bytes were the win, not encoding.**
Nobody could have told the two apart from a latency graph, which is why the benchmark counted body bytes as well as milliseconds.

**8. The replay codec as a transport format.**

A whole finished game is entropy-coded into one integer and base32'd into a link-safe string.
Derived events cost zero bits because the decoder re-runs the kernel; only genuinely random bits are paid for.
The server verifies the round trip byte for byte before persisting.
The same codec is the body of a message in the messaging extension, where it beat the raw encoding by 13x.

**9. Optimistic overlay, with the prediction owned by the kernel.**

Every commit carries a monotonic version; out-of-order broadcasts are dropped by version; optimistic moves revert on reject.
What changed in this migration is who computes the predicted board.
The host used to cache a couple of fields at the moment of the tap and re-impose them on every board that arrived, in four places that disagreed about whether to trust the cache or the server.
Now the host keeps **the action wire it sent** and re-asks the kernel per board, and the kernel answers from a rule it already had: a board whose table already shows the move's cards has already happened and is left exactly as the server wrote it.
No host code encodes a preference between the guess and the server, because the kernel says which kind of board this is, every time it is asked.
Generalized: **do not cache a derived value across a race; keep the input and re-derive.**
A cache is a second opinion, and two opinions in a racing system is a bug generator.

**10. Procedural, asset-free rendering.**

Zero texture files; the wool, wood grain and concrete are computed in the browser and cached in IndexedDB.
You ship the generator, not the pixels.

### What did NOT move, and why

Being explicit about this is what keeps the pattern from becoming a religion.

- **Rendering.** Interpolation and springs, screen coordinates, rects, curves, gesture previews, CSS transitions, `matchedGeometryEffect`.
  The useful test: if the function would give the same answer on a watch with a different screen, it belongs in the kernel; if it would not, it does not.
- **Scheduling and network deadlines.** The frame loop, a toast's dwell time, a replay's speed dial and its clamps over recorded wall-clock gaps, the grace period before a resync.
  One caveat that turned out to matter: a deadline **derived** from an animation duration must be derived from the kernel's constant, not from a host copy of it, or the coupling silently breaks the first time the kernel's number changes.
- **HTTP, auth, database access and process lifecycle.** The kernel never learns what a JWT is.
- **Realtime topics and their policies.** Which topic a client may join is a database policy over a membership table, and it stays there.
- **The independent wire walks in the security scan.** The hidden-information test deliberately re-walks the view and push frames by their own length fields instead of asking the kernel's reader, so that a bug in the reader cannot hide a leak from the scan.
  This is the one place where a second implementation is the point.

### What the discipline actually catches

This is the honest argument for the pattern, and it is not a list of elegant refactors.
Rewriting the shape meant reading every seam, and every seam that was read produced something.
Each item below names the test seam that found it or now holds it.

**Authorization and exposure**

1. **A state-writing database function was callable by anonymous users.**
   A migration dropped and recreated it without repeating an earlier lockdown loop, and a newly created function is executable by `PUBLIC`.
   The suite that should have caught it read only the from-scratch schema file, which did re-run the lockdown, so CI was green while the migrated database was open.
   Found by a test that **replays the migration history in order** and asserts after each step that no privileged function is executable by a client role (`e2e/db_migration_grants.test.ts`, now `e2e/db_platform_grants.test.ts` over seed.sql since the migration history was collapsed into it), which is now a standing gate.
   The relock is a migration on this branch and the deploy is the owner's, so the finding is closed in the repo and open on the deployed database until then.
   Generalization: a schema file and a migration chain are two implementations of one schema, and they drift; hold them equal object by object.
2. **A forged-row path into other players' ratings.**
   Default table grants let any authenticated user insert a game row with a fabricated roster, then drive it to a finish that scored into other players' ELO.
   Found while tracing who may write the table the migration was rewriting; pinned by table-level grant assertions in `e2e/db_grants.test.ts`, and closed by construction once a row's shape is the kernel's and writes go through one privileged function.
3. **Non-members could edit a lobby.**
   The lobby endpoints removed whoever the request body named and never checked that the caller was seated, and the add-a-bot endpoint checked no caller at all.
   The web relied on the first as a shipped "remove player" feature, so it was a policy question and not a silent fix: the behaviour was written down as a decision in the plan, and then closed in C, where the seat is resolved from the authenticated id and a caller who is not seated is refused with a named code.
   Held by **seat-from-auth** (`e2e/security_seat_from_auth.test.ts`, with `e2e/table_server_seat.test.ts`): the seat a request acts as comes from the token and only from the token, for the move endpoint and for every lobby operation, and a body-supplied id never selects the actor.
4. **15 full-state readers were reachable from the web bundle.**
   Found by the **static bundle boundary** test (`e2e/security_client_boundary.test.ts`), which bundles every browser entry with tree-shaking on and fails if any denied module, symbol or wasm export survives into it.
   The denied list is 15 named readers of an unmasked kernel game.
   A dynamic import keeps every export of its target alive in a bundle whatever the importer uses, so pointing the link-preview route at the whole kernel bridge kept the server-side bot drive and, through it, the unmasked state export.
   The fix is a three-export module the route imports instead, and the test pins it.
   Generalization: **reachability is the security property, not usage.**
5. **Two unauthenticated per-seat endpoints in the native server.**
   Its state endpoint served any seat's view, hand included, to anyone who named the seat, and seat 0 to anyone who named none; the HTTP/3 build had a second copy of the same endpoint that was still unauthenticated after the first was fixed.
   A seat's view now requires its owner's token in both, through one shared function, and the spectator view stays public.
   Held by the server's own smoke scripts (401 without a token, 403 with another seat's, 200 for the spectator), and the HTTP/3 case was red-first: all four probes returned the same 59 bytes before the fix.
   The kernel's own comment states the shape this belongs to: two doors onto one rule is a new way for two hosts to disagree, and the door nobody was looking at is the one that stays open.
6. **A realtime policy that refused everyone.**
   A per-user channel policy compared a topic segment split on hyphens with a user id that is itself a hyphenated UUID, so every join was refused - fail-closed, not a leak, but the feature was dead.
   Beside it, three policies for a topic nobody joins let two addresses with the same local part read each other's messages.
   The policies now rebuild the exact topic from the caller's membership row, and a test holds the whole policy set from a frozen copy of the deployed set through both migrations to the schema file's.

**Correctness**

7. **Three animation double-draw bugs**, all found by holding two invariants on **every frame** of a trace: no board ever shows a card twice, and once every message has been delivered the page's board is the server's.
   They were: a confirmation that beat its own flight laid the card a second time and handed a role on twice; a move the server had refused was still on the table when its flight landed, and one refused later stayed there with the hand a card short; and a message whose events were all the local player's committed at once while an older message still queued then committed its boards over it.
   The seam is **frame-by-frame traces**: `e2e/ui_animation_trace.test.ts` drives the real page tree against the real kernel on a virtual clock and holds every frame the page drew, at the exact virtual millisecond it drew it, to a recorded golden.
   That is a stricter record than a film, and it is why the harness exists: these are races that have to be staged to the millisecond, and a later repro in the same harness turned on a single frame at `t=1262` where the local seat's own card lay on the table under the local seat's own role marker for 288 ms.
   Each repro was red by assertion on the unfixed code first, and every re-recorded golden was read board by board to confirm it was the fix and nothing else.
8. **Two bot determinism bugs.**
   A decision drew from a stream whose position depended on what the module instance had done before, so one stored row could choose differently on a fresh instance and on a warm one; found by **two-instance determinism** tests that drive every brain's stored row on a module carrying another history and require identical actions, state, log records and messages, and by the native server's equivalent self-test, which was red for six brains.
   Then a deeper one: with that fixed, a decision was a pure function of the board and the deal seed alone, so a table of random bots could return to an exact earlier board and repeat it forever - measured as one game each at 4, 5, 6 and 7 seats over 40 seeds per seat count, and in production such a table holds its "needs bots" flag forever.
   The fix folds the game's own progress (the session log's length) into the decision seed, which keeps a decision a pure function of the stored row while making a repeated board impossible.
   Verified across 3,520 fixed-seed contested positions per build, against a control build that reseeds from an arbitrary constant, so that "the moves changed" could be shown to be a reseed and not a systematic shift.
9. **A generated Swift writer the generator refused to emit**, because two arrays in that struct share one count field (piece 4 above).
   That refusal is a caught defect, not a missing feature: the writer would have compiled, run, and written a zero count for every second array.

The pattern here is worth stating on its own.
None of these were found by reading code for bugs.
They were found by **moving a boundary and being forced to state, once, something that had been stated three times badly**.

---

## Part 2 - Building an app this way, from scratch

### Generalize the app to a state machine

Every app is `reduce(state, command) -> (newState, events)`, plus projections of state for viewers, plus I/O plumbing.
This is **functional core / imperative shell** taken to its literal extreme: the functional core is not a folder, it is a separately compiled artifact that *cannot* import a socket or a database because it is freestanding.
The boundary is enforced by the compiler, not by discipline.

In textbook terms this is event-sourced plus CQRS plus functional-core.
The novel part is the last mile: the write model is a portable compiled artifact, so the same validator and reducer also run on the client, which buys optimistic UI and offline-first from the same code.

### The hard discipline: what goes in the Core

The Core is a pure function.
Anything non-deterministic or effectful is turned into **data crossing the boundary**, never performed inside:

- **Time** is injected.
  The command carries `now`; the Core never reads a clock.
  This is also what makes a re-askable API possible instead of a callback (Part 1, piece 3).
- **Randomness** is injected as a seed, and determinism is load-bearing.
  Seed a decision from **the stored row**, not from the process: the row is what every instance shares, and anything else makes the same input produce different output on a retry.
  If a decision can revisit an identical input forever, fold a monotonic property of the row's own history into the seed rather than adding a rule to the domain.
- **External side effects** are not performed by the Core.
  It *emits an event describing them*; the Shell performs the effect and feeds the result back as a new command.
- **Reads and joins the Core does not hold** are CQRS: the Core is the write model, and complex read views are separate projections.

### What you get for free

Once the Core is pure, deterministic and event-sourced, several hard features are consequences rather than work: optimistic UI, offline and local-first, an audit log with undo and time travel, crash recovery by replay, tests that are `(state, cmd) -> assert` with no mocks, and authorization you cannot forget because the masked projection is the only serializer.

### Repository structure (generic)

```
/core        pure reducer + validation + authorization + projections. NO I/O.
             Compiles to native and to wasm. THE HEADERS ARE THE SCHEMA.
/core/test   invariants, property and fuzz tests, golden transcripts, cross-build agreement
/tools/gen   the binding generator: parses /core's headers, emits per-host access + a layout hash
/gen         GENERATED host bindings, one module per (build, host language). Never edited.
/server      imperative shell: HTTP, auth, persistence, the effect runner
/client      the core (validate + optimistic predict) + a thin rendering skin
/read        CQRS projections for queries that do not belong in the write model
/ops         proxy, migrations, deploy
```

The rule that keeps it coherent: the wire format is defined **once, in the Core's own headers**, and every other language gets it by generation.
The old form of this rule was "one schema mirrored into exactly one host file with a golden test asserting agreement"; the mirror is what the generator deletes.
`/gen` is a build output that happens to be committed, and CI regenerates it and fails on a diff.

### The C-first insight: the parity tax mostly evaporates

If the kernel comes *first*, there is no host oracle to match - **the kernel is the spec**.
What replaces parity testing is cheaper and is testing you want anyway:

1. **Invariant and property tests**, the real correctness oracle (conservation, legality, serialize-then-deserialize identity).
   These are domain truths, not a mirror of a second implementation, so they catch bugs a mirrored oracle would happily reproduce in both copies.
2. **Fast-versus-reference difftests, opt-in per optimization.**
   Write the obviously correct slow version, difftest the fast one against it, inside one language.
   You pay this only on the parts you actually optimize.
3. **Cross-compile agreement**, because the client and the server are one source compiled twice.
   One corpus test catches compiler and flag divergence, which is why fast-math is banned in the wasm build.
4. **Generated-binding verification**, which is new here and is the cheapest of the four: C fills a fixture through its own field names, the host reads it through the generated readers, and agreement is the statement that every emitted offset is the one the compiler computed.

### Test doctrine, generalized

This migration ran on four rules, and they are portable.

**Red first, by assertion.**
A new test must fail on the unfixed code with an assertion, not a compile error, and then pass.
A compile error proves the test was rebuilt; an assertion proves it was watching.
For a test written *after* the code it covers, run the mutation check: break the thing on purpose, confirm the test names it, restore with a checkout.
Several of this migration's suites were mutation-checked that way, and the check earns its keep by finding tests that assert nothing: one probe in the native server's smoke run sent zero probes and then passed because zero of zero were rejected.

**Retire a parity test when its second implementation is gone.**
A parity test compares two implementations.
When one is deleted, the test compares an implementation against itself and is a tautology that still costs a minute of CI and, worse, still reads like coverage.
When two implementations genuinely still ship - a host writer and the kernel's writer of the same block, in two languages, both live - **keep** the parity test and keep it honest about which half is real.
Half of one suite here was retired and half was kept, for exactly that reason.

**Prefer one gate that cannot be forgotten over a convention.**
Every durable rule in this repo is a gate: a layout hash the artifact carries, a regeneration check that fails on a diff, a freshness check over build artifacts, a static import boundary, a catalog-equality test between the schema file and the migration chain, an export-list test between the shipped module and the test build.
A convention in a README is a gate with a human in the loop.

**An artifact nothing compares is an artifact nothing keeps fresh.**
A generated wasm fixture was excluded from its own freshness diff because two machines' compilers write different bytes, and it silently rotted inside a single phase - every data address in it had shifted.
The exclusion was the bug.
Either compare the artifact or build it on demand; do not commit a thing whose staleness nothing can detect.

### The C-server unlock: in-memory authoritative state

The honest residual bottleneck of a serverless deployment is *"database round trips, not compute"*: stateless functions reload state per request.
A stateful native server deletes that problem.
One long-lived process holds hot entities in RAM as the struct, single-writer per entity:

```
parse wire -> lock entity -> kernel reduce on the resident struct
  -> append the command to a write-ahead log -> unlock -> broadcast bytes
```

No marshal in or out, no database read on the hot path.
The database is demoted to a durability log: append, snapshot periodically, replay the tail on restart, with determinism guaranteeing exact reconstruction.
Two lessons from actually running one here.
First, **the crash-recovery path must round-trip everything the decision depends on**: a recovered game came back with every seat a default brain and its seeded deck drawing at random, because the state codec carried neither the seat kinds nor the deck mode.
Second, a self-test that runs at startup is worth its weight, and a self-test nothing watches is not: this server's had been failing since an unrelated kernel change, so the process exited before it opened a socket, and nothing noticed because nothing asserted that it had started.

### Build order (each phase independently testable)

- **Phase 0 - the contract.** Write the byte layouts first, in the Core's headers.
  Commit to bytes before any code, and keep the in-memory layout independent of the durable one so you can reorganize without a data migration.
- **Phase 1 - the Core, pure and headless.** No sockets, no files.
  Build a REPL on day one; a fuzzer that throws random commands and asserts invariants is your executable spec and never goes away.
- **Phase 2 - the binding generator.** Before the second language exists.
  It is the cheapest it will ever be, and every hand-written marshal you do not write is one you do not have to delete later.
- **Phase 3 - serialization and masking.** Round-trip identity, masked-blob property tests, golden transcripts.
- **Phase 4 - the server, request and response first.** Link the Core, embedded store, per-entity lock, WAL and snapshot durability, two clients playing a full game over `curl`.
- **Phase 5 - realtime.** Per-viewer masked event streams, stale broadcasts dropped by version.
- **Phase 6 - the client core.** Same source, size-first configuration, generated bindings, the layout hash checked at load.
- **Phase 7 - the UI shell.** Interaction becomes command bytes, gated by the Core, decoded into objects only at the render boundary.
- **Phase 8 - optimistic overlay and reconciliation**, with the prediction computed by the Core and the host keeping only the command it sent.
- **Phase 9 - the animation model**, as a plan plus a re-askable per-frame query, if the app animates at all.
- **Phase 10 - replay codec and procedural assets**, optional polish.

### How to run a migration of an existing system this way

This repo's migration was about a hundred commits across eleven phases, some of them run by different agents in one tree, and the process rules mattered as much as the design.

- **Each phase is independently green and committable.**
  Not "green at the end": green at the end of every phase, with its own gates re-measured and recorded.
  A phase that cannot be committed on its own is two phases.
- **Write the "as built" section for each phase, as you finish it, in the plan document.**
  Every phase here differs from its plan somewhere, and the difference is always the interesting part.
  A plan with no as-built sections is a document that stops being true on day two.
- **Own files, not features, when several people or agents share a tree.**
  Phases were scheduled so that parallel work touched disjoint file groups; where two phases both touched one header, they were serialized deliberately so the layout hash changed once rather than twice mid-phase.
- **Anything with durable state gets expand / switch / contract**, with the deploy boundaries written into the plan, because the failure mode is not a bad migration, it is a good migration deployed at the wrong moment relative to the code.
- **Keep artifact conflicts out of human hands.**
  Generated modules, compiled artifacts and stamps are never resolved by picking a side: take either, regenerate, rebuild, and let the freshness check decide.
- **Run the one full end-to-end suite at the end, not per phase.**
  Per phase, run that phase's targeted files plus the fast gates (the Core's suites, the generator's suites, artifact freshness, both typecheckers, the memory test).
  The full suite belongs on the tree that will actually ship.
- **Refuse things, with measurements.**
  Two refusals shaped this migration more than most of its features.
  One was the binding-generator comparison above: the off-the-shelf options were measured and rejected on numbers, not taste.
  The other was a proposal to collapse the migration history into a single baseline.
  It was refused **after** a throwaway database was built and the deployment CLI was actually run against it: with the history intact the push reported up to date, and with the files collapsed it failed with a missing-migrations error whose own suggested repair deletes the tracking rows rather than inserting them - which would have stopped the next deploy dead.
  A hunch would have produced a debate; twenty minutes of measurement produced a decision, a written record, and a standing test that holds the schema file and the migration chain equal object for object (1,390 catalog items, an empty diff).

### The fit spectrum (be honest)

The payoff scales with how much genuine, shared, deterministic logic the app has.
Ask: **would I need to run this exact logic in more than one place?**
Server authority *and* client optimism, or online *and* offline, or app *and* worker *and* audit replay.

- **Ideal:** collaborative editors (Figma does exactly this, a C++ core compiled to wasm, same client and server), multiplayer and simulation, fintech and regulated systems where a deterministic core plus an immutable command log is an audit trail by construction, configurators, planners, rules engines, spreadsheets, local-first apps.
- **Marginal:** commerce, booking and workflow - apply it to the transactional heart and leave catalog and content alone.
- **Overkill:** CRUD over a database, content sites, dashboards, most admin tools.
  If the app is fundamentally "fetch rows, render, write rows", there is no meaningful Core: the app *is* the I/O shell, and the wire format and build pipeline buy nothing.

One extra signal this migration produced: the payoff also scales with **how many hosts you have**.
The break-even for the binding generator arrived at the second language, and the third host paid for the whole thing, because a change to an animation struct now updates the browser and the phone by rerunning one script.

### The taxes, refreshed with what this actually cost

**Size.**
Moving this much logic into the kernel grew the shipped kernel and shrank the shipped host code, and both were measured every phase.

| | Baseline | End of this migration |
|---|---|---|
| kernel module, gzipped | 65,307 B | 80,913 B |
| the two deleted role-specific modules | 18,549 B | 0 |
| web bundle, first-load union, gzipped | 330,504 B | 304,551 B |

Both end-state figures are the tree as it stands, not a number carried forward: the kernel is the committed `sdk/ts/wasm/bots.wasm.gz`, and the bundle is `node scripts/measure_web_bundle.mjs`, which gave 304,551 B identically over two builds.
That is 315 B under the 304,866 B Phase 10 recorded, from the three code commits that landed after it.

The kernel grew about 24 percent and the shipped web bundle fell about 8 percent, so total shipped bytes fell.
That is not an accident of this domain; it is what happens when the code you delete is marshalling code, whose size is proportional to the number of fields, while the code you add is a rule, whose size is proportional to the number of decisions.

**The owner's priority order, applied without exception:**
C over hand-written host code first; then speed and size; and both measured every phase, with each delta recorded in the migration document's gate table whether or not it was a regression.
Several phases recorded a breach with its reason rather than waiving it, which is the only way a budget survives contact with a real project.

**Memory safety is on you.**
Mitigate with an arena per request, a no-ambient-allocation Core, sanitizers in CI, continuous fuzzing of the Core and of every parser.
**Rust gets most of this plan with memory safety and the same native-plus-wasm story**; weigh it seriously (`docs/RUST_VS_C_KERNEL.md`).

**No ecosystem for the boring stuff.**
Terminate TLS, HTTP/2, compression and static serving at a proxy; the native app speaks plain HTTP and WebSocket and does only the domain API, keeping the hand-written attack surface tiny.

**Stateful servers are harder to operate.**
Restart is drain, snapshot, reload; scaling past one box is sharding entities by id with sticky routing, which is easy because each entity is an independent actor, but is a real design step.

**A generator is a dependency on a compiler front end.**
libclang's version must be pinned across developer machines and CI, and the layout hash is the second line of defence when it is not.

---

## Part 3 - The generalized performance, memory and size playbook

Almost every specific optimization here is an instance of one of five general moves.
The unifying idea:

> **Remove work or space that the specific situation proves you do not need - and make that removal permanent and enforced, not a fragile one-time win.**

Not "make the code faster" but "prove a constraint about this exact deployment, then collapse the program to fit it".

### 1. Optimize the binding constraint - and only that

Measure what actually gates the user, spend effort only there, and recognize when a curve has gone flat.

- Anchors: perceived latency is dominated by a 500 ms animation, not by compute; the server's residue is round trips, not compute; the bot's strength-versus-compute curve is at its saturation knee, so a 15x speedup was banked as lower latency rather than strength.
  The durable-store rewrite is the newest anchor: the payload fell 60 percent and the latency did not move, and the benchmark said so because it counted bytes as well as milliseconds.
- Generalized:
  - Find the real bottleneck by measurement (in web apps it is almost never "the host language is slow" - it is round trips, waterfalls, hydration, N+1).
  - Detect flat parts of the curve and stop.
  - Bank a surplus where it is felt: convert an unfelt speedup into latency, cost or headroom.
  - **Measure the thing you can act on**, not only the thing the user feels; a flat latency graph hid a 60 percent payload win that will matter on a worse network than yours.

### 2. Turn limits into build-time invariants

A constraint checked at runtime is a bug waiting to happen; a constraint the toolchain enforces cannot regress.

- Anchors: static assertions that fail the link if an arena overlay overflows; pinned linear memory (`--initial-memory == --max-memory`) on the modules whose budget is fixed, so a buffer over budget fails the *link*; a memory test that asserts the module's initial page count and names every buffer that justifies a raise; caps sized from a measurement harness with clean overflow, so exceeding one drops an animation frame and never corrupts.
- Newer anchors from this migration, all of them the same move applied to *correctness* rather than to size: the layout hash the artifact carries, so a mismatched pair refuses to run; the regeneration check that fails CI on a diff; the static import boundary; the export-list test; the catalog-equality test between schema file and migration chain.
- Generalized:
  - Budgets enforced by CI, so a regression fails the build rather than appearing in a graph.
  - Make the safe path the only representable path.
  - Let overflow degrade cleanly and choose where the failure lands.
  - **A rule with no gate is a rule with a half-life.**

### 3. Specialize the artifact - ship only what the call site runs

Compile a tailored artifact per call site from one source and strip everything that site never executes.

- Anchors: one kernel built per target and per export allow-list, with the test-only entry points living in an uncommitted second link; a shipped module that lost 67 exports when the host-side shape it served was retired; `-Oz` by default with `-O3` on only the three search-core files; the analyser built from the same sources with a much larger search table because a browser tab has a different memory regime from an edge function.
- The correction worth carrying: specializing by *role* (a validate-only client build, a full server build) looked clever and did not survive, because it meant three artifacts answering the same questions.
  Specializing by *target* and by *export surface* survived, because neither duplicates a decision.
- Generalized:
  - Route and component-level splitting, side-effect-free marking, dynamic imports - and remember that a dynamic import keeps every export of its target alive, so a bundle boundary is a module you create on purpose.
  - Per-target builds from shared source.
  - Compile feature flags *out* at build time rather than branching at runtime.
  - Uniform optimization is wrong: optimize the hot 5 percent hard and build the cold 95 percent for size, because on cold or short-lived runtimes a smaller artifact loads faster and the cold code never warms up.

### 4. Fit the data to the machine and the boundary

Layout against the memory hierarchy, and compactness at boundaries, often beat the algorithm.

- Anchors: the transposition table packed from 16 to 8 bytes, made two-way so one bucket is one cache line, and sized to stay resident; snapshots that store only the struct's prefix; bulk-memory operations instead of byte loops; a card crossing as one byte rather than an object.
- Newer anchors: **read a large struct where it lies.**
  The animation plan is 11,752 bytes, and a frame loop that copied it whole once per frame would spend more time copying than animating, so the generated accessors read it in place and only the small frame record is copied out.
  The rule that falls out: **copy what you will hold, read in place what you will ask.**
  And the transport measurement: hex text 3,657 bytes, binary parameters 3,534, base64 2,433, with the encoder itself measured in the actual runtime (3.3 us per 1.5 KB for a table loop, against 17.6 us for the platform's string path in a runtime that lacked the native method).
- Generalized:
  - Size hot working sets to a cache tier.
  - Compact representation at boundaries beats parsing speed.
  - Copy less: send only what changed, share structure, avoid deep copies.
  - Use the platform's bulk primitives, and measure them *in the runtime that will run them*, because a polyfill and a native method are not the same program.

### 5. Exploit liveness, stability and non-overlap to reuse work and space

If two things are never live at once, share their space.
If a result is stable, compute it once.
If work already happened, do not repeat it across a boundary.

- Anchors: buffers provably never concurrent aliased at one address (about 90 KB reclaimed as pure address reuse); state that stays resident across calls so a follow-up apply is free; a version-fenced cache that skips the database load, trusting the fence and not freshness, so a stale hit costs one revalidation and never a wrong answer; a precomputed endgame oracle that terminates stable subtrees; a replay whose derived events cost zero bits.
- The counterweight this migration added: **a resident slot is a shared mutable, and its safety argument is a liveness argument that an `await` destroys.**
  The rule is load, operate, copy the products out, all synchronous; anything held across a suspension point is something another caller can move underneath you.
  One bot cycle here also went from two database reads to one by taking the log on the row's own select, which is the same move in the other direction: do not fetch twice across a boundary what one fetch can carry.
- Generalized:
  - Overlap in time is what permits sharing space (arenas, pools, buffer reuse).
  - Cache across the expensive boundary, fenced by a version or a hash rather than by freshness.
  - Precompute the stable and derive the rest; send the seed, not the sequence.
  - Do not recompute across boundaries: pass the result, batch to amortize the crossing, colocate compute with data.

### The discipline underneath all five

- **Every optimization was measured**, before and after, with a real harness and a stated margin.
  Numbers or it did not happen, and the numbers live in the gate table of `docs/C_GAME_SHAPE_MIGRATION.md` section 4.0, including the ones that went the wrong way.
- **Every one is reversible via a knob.**
  Optimizations are hypotheses; keep them toggleable.
- **Every one is gated by correctness tests.**
  A faster wrong answer is worthless.
- **Done in the right order:** correctness first, then measure the binding constraint, then specialize and shrink, then re-pin the budget lower after each win so the ratchet only turns one way.

### The playbook, in one line

**Measure the actual constraint, prove what this deployment does not need, collapse the program to fit, enforce that collapse at build time, and gate it with tests while keeping a knob to undo it.**

---

## Where this repo is still short of the pattern

Stated plainly, because a doctrine document that only describes its successes is a sales brochure.

- **Some animation questions are still the host's.**
  `docs/ANIM_TIMING_AUDIT.md` closes with six entries the kernel needed, and three of them landed whole: the re-askable per-frame call, the plan and beats builders crossing to the browser, and a hand order that can name a face-down slot.
  Of the rest, **optimistic timing** landed in part - a predicted flight is a step in the kernel's plan with the kernel's duration, and the board it leaves lands with it - but C still has no answer for what a confirming message does to a flight already in progress, so the host drops the confirmation and lets the prediction run out.
  **The refusal return flight** has no place in the plan at all: a refused move's cards are appended to the tail of whatever is running rather than emitted as steps.
  **The monotonic version watermark** is still a `Math.max` in a React effect rather than a kernel rule.
- **A few host files still read a variable-length wire**, on iOS, where the bytes are a form something travels in rather than a struct anybody holds (Part 1, piece 4).
  None of them is a layout stated twice, which is the property that mattered, but each is a candidate for a later pass.
- **Two host wrappers remain hand-written** over the generated modules - one per side of the boundary, thin, synchronous, with no field knowledge beyond the names the generator emitted.
  They are the smallest honest residue, not zero.
- **The plan's own remaining work is listed in `docs/C_GAME_SHAPE_MIGRATION.md` section 7**, including the rebase onto the main line and the deploy ordering for the three durable-store steps, which are the parts a reader should check before treating this document's end state as shipped.
- **Verification is uneven by host.**
  The kernel, the web and the database have suites that run on every commit; the phone's animation behaviour is proved by the kernel's tests, a compile, and one simulator pass per round rather than by continuous integration.
- **One artifact rotted inside a single phase** because it was excluded from its own freshness check, which is the best evidence in this document that the gates are the load-bearing part and not the prose.

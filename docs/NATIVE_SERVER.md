# The native C server (`server/impls/native`)

**This is not the deployed path. Supabase is.**
`server/impls/supabase` is what `.github/workflows/deploy.yml` ships and what the web, iOS and iMessage clients talk to.
`server/impls/native` is a second, fully independent implementation of the same `server/api` contract, kept in the tree as a working proof that the contract is host-neutral and as the starting point if Foolish ever needs a backend that Postgres-plus-edge-functions cannot give it.
Nothing in the product build depends on it: it has no CI job, is not imported by any TypeScript, and deleting it would change no shipped byte.

## What it is

A single long-lived C process that holds every game as a `Game` struct in RAM and links the kernel sources (`c/src/*.c`) directly - the same sources that become `bots.wasm` for the web and `libfoolish.a` for iOS.
It reimplements no Durak rule.
The deal, legality, apply, per-seat masking, the bot cycle and its pacing, and the bot roster are all kernel calls; the server contributes a socket, an in-memory store, a lock, and a thread pool.

That is the point.
`docs/ARCHITECTURE_AS_A_PATTERN.md` claims the server API is language-agnostic because the kernel does the work.
This is the falsification test for that claim, and it passes: swapping Postgres for a hash table and the Deno edge runtime for an epoll loop leaves the *game* byte-for-byte identical.

Beyond the architectural point it is also the only place in the tree where the kernel is exercised **concurrently and adversarially at load**, which is where several of the fixes below came from.

## What it grew into

The original POC on `main` was ~600 lines: HTTP/1.1, one global lock, thread-per-connection, RAM-only.
What lands here is the result of seven hardening stages, each with its own writeup next to the code:

| stage | what it adds | doc |
|---|---|---|
| 1 | per-game locks + work-queue thread routing, replacing one process-wide mutex | [`SERVER_SCALING.md`](../server/impls/native/SERVER_SCALING.md) |
| 2 | SQLite WAL write-behind persistence and `kill -9` crash recovery | [`DURABILITY.md`](../server/impls/native/DURABILITY.md) |
| 3 | TLS on every endpoint including the `/ws` hot loop (HTTPS + WSS) | [`TLS.md`](../server/impls/native/TLS.md) |
| 4 | spectator WebSockets and a server-side octogen bot workload | [`SERVER_SCALING.md`](../server/impls/native/SERVER_SCALING.md) |
| 5 | parallel bot compute - the kernel globals on the `bot_drive`/`awire_apply` path made thread-local, so games stop serializing through one kernel lock | [`SERVER_SCALING.md`](../server/impls/native/SERVER_SCALING.md) |
| 6 | epoll-per-shard connection I/O for plaintext, replacing thread-per-connection | [`SERVER_SCALING.md`](../server/impls/native/SERVER_SCALING.md) |
| 7 | SO_REUSEPORT multi-acceptor, QUIC/HTTP-3/WebTransport front-end, game reclamation, admission control | [`SERVER_SCALING.md`](../server/impls/native/SERVER_SCALING.md) |

Plus, outside the numbered stages: a push-only `/ws` protocol (clients never poll), per-IP rate limiting, a Prometheus `/metrics` endpoint, stateless HMAC-signed binary session tokens, an adversarial fuzz client under AddressSanitizer, and a semantic anti-cheat fuzzer that fires well-formed *illegal* moves at the legality engine.
`PROFILE_HOTPATH.md` is the callgrind record behind the optimizations; `DEPLOYMENT.md` sketches what a real deployment would still need.

### The part that reaches the shipped product

Two kernel changes came out of this work and apply to every host, not just the native server.

**Thread-local kernel globals.**
`engine_snap_hook`, `engine_last_reject`, `bot_drive`'s shared `LegalMoves` scratch, `cordite_sim.c`'s lazily-built card masks, and `game.c`'s log-overflow sink were process-wide statics.
They were safe under one global kernel lock and are safe in every single-threaded host, but they are read/write and write/write races the instant two different games run `bot_drive` concurrently.
They are now `_Thread_local`.
The wasm build neutralizes the qualifier with `-D_Thread_local=` (`c/Makefile`), so the shipped modules are byte-unchanged, and every single-threaded caller - tests, iOS, replay tooling - is unaffected.
`game_force_first_attacker`'s global is deliberately left alone; the audit note in `game.c` says why.

**A `state_put` hoist.**
The per-seat masking loop tested a loop-invariant `unmasked` flag once per card.
A masked view hides the entire deck and every other hand, so those collapse to one `memset` each.
Output is byte-for-byte identical; `state_put` was the server's hottest own code once bot-thread churn was gone.

## Build and run

**`foolish_server` builds on Linux only.**
Stage 6 moved connection I/O onto `epoll`, which has no macOS equivalent, and no kqueue port was attempted.
The `Makefile` fails fast with that message rather than letting it surface as a missing `sys/epoll.h`.
On a Mac, use a container:

```sh
docker run --rm -it -v "$(pwd)":/repo ubuntu:24.04 bash
# inside:
apt-get update && apt-get install -y build-essential libsqlite3-dev libssl-dev
cd /repo/server/impls/native
make            # foolish_server
make run        # ./foolish_server 8099
bash test.sh    # signup 2 humans, add a cordite bot, deal, attack
```

`sem_fuzz` (the semantic anti-cheat fuzzer, kernel-only) and `fuzz_client` (pure sockets) carry no epoll and build natively on macOS.

Other gates that live next to the code: `crash_test.sh` (kill -9 and recover), `tls_test.sh` (throwaway cert, HTTPS + WSS), `fuzz_test.sh` (ASan + adversarial client), `sem_fuzz_test.sh`, `quic_test.sh`, `pacing_test.sh`.
None of them run in CI - they need a Linux box, and several need `valgrind` or a quiche build.

`bench_results/<label>/` keeps only the small human-readable residue of a profiling run: `meta.txt`, `summary.txt`, the CSV tables, and the Helgrind digests.
The `callgrind_annotate` dumps the docs cite by line number are ~1 MB of machine-specific attribution and are gitignored; `profile.sh` regenerates them, and `PROFILE_HOTPATH.md` already quotes the lines the argument rests on.

## The Rust question

`docs/RUST_VS_C_KERNEL.md` is a full examination of whether the kernel should be rewritten in Rust, with `rustpoc/` as its empirical half: four real kernel hot paths ported to safe Rust and measured against the shipped C over workloads generated by the real engine.

The short version:

- **Performance does not decide it.**
  Legal movegen +14% (or +4% with bitmask idioms), MC playouts within noise, wire parsing -5%, and the exact endgame solver +5.5% to +9% - the one honest regression, on a component that is node-budget-capped per decision anyway.
  Three of the four deltas sit inside the gcc-versus-clang band on the same C source.
- **Bit-exact cross-language parity is achievable**, including `f64` rollout math against an `-ffast-math` build and the solver's 46,894,219-node search forest.
  That matters more than the timings: it means the existing differential harnesses could verify a Rust port move-for-move against the C oracle.
- **The real axis is correctness class**, not speed - the doc lays out the memory-safety bug ledger and the fuzz/ASan/Helgrind machinery that a safe language would subsume.
- **No rewrite is planned or started.**
  The doc is a decision record, not a work order, and `rustpoc/` is a frozen snapshot of the measurement - it is not built by anything, not in CI, and will drift from `c/src` as the kernel changes.
  Re-run it before trusting a number.

## Related reading

- [`server/impls/native/README.md`](../server/impls/native/README.md) - endpoints, protocol, the kernel/server split, current status
- [`docs/ARCHITECTURE_AS_A_PATTERN.md`](ARCHITECTURE_AS_A_PATTERN.md) - the doctrine this implementation tests
- [`docs/C_CORE_CONSOLIDATION.md`](C_CORE_CONSOLIDATION.md) - A10, the split into `server/api` + `server/impls/*` and the DAG that `e2e/validation/layering_validation.test.ts` enforces
- [`docs/SERVER_LIFECYCLE_CONSOLIDATION.md`](SERVER_LIFECYCLE_CONSOLIDATION.md) - the follow-on audit of the lobby/result/scoring skin, which found the native server hand-rolling a partial rematch reset instead of calling the kernel's `game_reset_to_lobby` (L2, still open)
- [`c/MAKE_UNMAKE.md`](../c/MAKE_UNMAKE.md) - a banked negative result from the same profiling work: make/unmake in the endgame solver, measured, reverted

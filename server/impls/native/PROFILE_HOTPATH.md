# CPU hot-path profiling — foolish_server, and the octogen solver at two TT sizes

Measured on this box: Linux, 4-core Xeon, clang 18 (Ubuntu). Profilers
present: **valgrind (callgrind), gprof**. Profilers absent: **perf, sample**.
So every capture below went through callgrind — the report says so instead
of pretending otherwise, and the final section explains what changes on a
Mac (`sample`) or a Linux box with `perf`.

All three profiling binaries were built **without `-flto`**, **with `-g
-fno-omit-frame-pointer`**, keeping `-O3 -ffast-math` (T2/T3) or `-O2
-ffast-math` (T1, matching the server's shipped `CFLAGS`) so callgrind
attributes cost to real, individually-named functions. **Production builds
use `-flto`** (`c/Makefile` line 6-7), which inlines the tiny leaf calls
(`can_cover`, `game_done`, `hand_remove_card`, `card_eq`, …) — 100M-300M
calls in a cordite eval — into the rollout hot loops. Concretely, in this
non-LTO T3 build `can_cover` shows up as its own line item, 234,710
instructions over 16,765 calls, a separate cross-TU `call` instruction each
time (`bench_results/T3_octogen_prod/annotated.txt:127`); under `-flto` that
call vanishes and its instructions fold into `sim_gen_moves`/`sim_solve_rec`
directly. So treat every per-function percentage below as "attributed to
this function in a debuggable, non-LTO build" — the shipped binary is ~1.5x
faster with the *same* hot instructions, just credited to different (bigger,
fewer) functions.

T2 and T3 use the production caps `-DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64`.

---

## T1 — the server hit path (random human clients, zero server-side bots)

### What's being measured

`foolish_hammer` signs up N users, seats them as HUMANS in each game (never
`/meta add-bot`), starts every game, then hammers `/action` with
well-**framed**-but-random awire payloads (`awire_encode` over full-range
random card bytes — see `foolish_hammer.c`'s `build_random_frame`), plus
`/state`, `/status`, and occasional fresh-game growth. No Monte-Carlo bot
brain ever runs server-side in this mode: every request is pure HTTP parse +
auth lookup + kernel decode/validate/apply + JSON/packed-view emit.

### Build

```sh
cd server/impls/native
make CC=clang foolish_server_prof     # -g -fno-omit-frame-pointer -O2 -ffast-math (Makefile target)
make CC=clang foolish_server          # the normal, shipped-flags build, for the throughput number
make CC=clang foolish_hammer
```

### Headline throughput (full-speed, NOT under callgrind)

Normal `foolish_server` (plain `-O2 -ffast-math`, no `-g`), hammered for 10s:

```sh
./foolish_server 8097 &
./foolish_hammer --host=127.0.0.1 --port=8097 --games=40 --seats=4 --conns=32 --secs=10 --mode=action
```

```
total requests:  134535  (13401.5 req/s)
  actions sent:  94402  ok=true: 142  (14.1 applied/s)      <- random cards, ~0.15% legal
status codes:    200=40275 400=94260 401=0 404=0 5xx=0 other=0 conn_fail=0
```

Raising `--conns` to 96 did **not** raise throughput (12143.3 req/s) — the
server (4 cores, thread-per-connection, one global mutex) is already the
bottleneck at ~32 client connections on this box, not the client.

`--mode=mixed` (decodes a real `/state` view and submits a genuine legal
move ~20% of the time) at the same scale:

```sh
./foolish_hammer --host=127.0.0.1 --port=8097 --games=30 --seats=4 --conns=32 --secs=10 --mode=mixed
```

```
total requests:  121334  (12085.7 req/s)
  actions sent:  85410  ok=true: 14476  (1441.9 applied/s)
status codes:    200=50400 400=70934 401=0 404=0 5xx=0 other=0 conn_fail=0
```

**Headline overload numbers for this box: ~13,400 req/s pure hit-path,
~12,100 req/s / ~1,440 applied moves/s with games actually progressing.**

### Profiling (callgrind)

`profile.sh --launch` blocks until its command exits, but `foolish_server`
never exits on its own — and wrapping it in GNU `timeout N ...` (the obvious
fix) **breaks valgrind's capture**: `timeout` `fork+exec`s the real target as
an untraced *child* process (valgrind only follows `fork` into
`--trace-children=yes`), so a first attempt profiled nothing but `timeout`'s
own dynamic-linker startup (288K instructions total, all in `ld.so`/`libc`
init — see the note in `bench_results/T1_server/meta.txt`). Fix: launch
`valgrind --tool=callgrind` on the server directly (the exact command
`profile.sh`'s callgrind path runs), let `foolish_hammer` drive it for a
bounded window, then send the valgrind process `SIGTERM` — valgrind forwards
the signal to its guest, the guest's blocking `accept()` returns with
`EINTR`/dies, and valgrind flushes `callgrind.out` before exiting, exactly as
on a natural exit:

```sh
valgrind --tool=callgrind \
  --callgrind-out-file=bench_results/T1_server/raw/callgrind.out \
  -- ./foolish_server_prof 8096 &

./foolish_hammer --host=127.0.0.1 --port=8096 --games=3 --seats=2 --conns=6 --secs=20 --mode=action
kill -TERM <valgrind pid>     # or: profile.sh's own approach for a naturally-exiting target

callgrind_annotate --auto=yes --inclusive=no bench_results/T1_server/raw/callgrind.out \
  > bench_results/T1_server/annotated.txt
```

Under instrumentation, the same hammer settings (6 threads, 20s) measured
**1,166.5 req/s** — about 11.5x slower than the un-instrumented server, well
inside the documented 30-50x envelope (this workload is I/O/lock-bound, not
arithmetic-bound, so callgrind's *instruction*-counting overhead bites less
than on the CPU-bound octogen runs in T2/T3). 2,498,336,561 instructions were
collected over the run.

### Top self-cost functions (Ir, `bench_results/T1_server/annotated.txt`)

| Ir | % | function |
|---|---|---|
| 2,144,628,187 | 85.84% | `ld.so` `memset` (thread-stack/TLS zeroing inside `pthread_create`) |
| 52,611,983 | 2.11% | libc `__vfscanf_internal` (`sscanf(buf, "%7s %255s", method, path)`) |
| 40,461,411 | 1.62% | libc `__strcmp_avx2` |
| 39,582,278 | 1.58% | libc `strcasestr` (header scanning: `content-length:`, `authorization:`) |
| 26,417,619 | 1.06% | **`foolish_server.c:conn_thread`** |
| 19,482,338 | 0.78% | libc `str-two-way.h:critical_factorization` (strstr/strcasestr internals) |
| 18,094,198 | 0.72% | libc `__strstr_sse2_unaligned` |
| 12,774,734 | 0.51% | libc `__printf_buffer_write` |
| 11,860,577 | 0.47% | libc `__printf_buffer` (vfprintf path, i.e. `snprintf`) |
| 6,679,652 | 0.27% | libc `__memset_avx2_unaligned_erms` |
| 6,205,917 | 0.25% | `nptl/allocatestack.c:pthread_create` |
| 5,380,648 | 0.22% | libc `__strchrnul_avx2` |
| 4,805,915 | 0.19% | libc `__memcpy_avx_unaligned_erms` |
| 3,445,836 | 0.14% | `dl-tls.c:_dl_allocate_tls_init` |
| 2,553,673 | 0.10% (own file) | **`foolish_server.c:h_state`** / **`c/src/view.c:state_put`** |

### Notable hot source lines

The dominant cost by a huge margin is **not** the kernel — it's **spinning
up a brand-new OS thread, with a brand-new stack and TLS block, for every
single HTTP request** (`conn_thread` is `pthread_create`d once per accepted
connection, `foolish_server.c:429`, and the server sends `Connection: close`
so every request really is a new connection → new thread). 85.84% of all
instructions retired in this run are the loader's `memset` zeroing each new
thread's freshly-mapped stack/TLS region.

Inside the actual application code, two O(n) linear scans stand out
(`bench_results/T1_server/annotated.txt` lines ~239-251):

```c
static User *user_by_token(const char *token) {
    if (!token || !*token) return NULL;
    for (int i = 0; i < MAX_USERS; i++)                                  //  3,197,869 Ir
        if (g_users[i].used && strcmp(g_users[i].token, token) == 0)     //  8,640,674 Ir
            return &g_users[i];                                         // => __strcmp_avx2: 22,324,116 Ir / 1,077,999 calls
    return NULL;
}
static GameSlot *game_by_id(const char *id) {
    for (int i = 0; i < MAX_GAMES; i++)                                  //  2,196,684 Ir
        if (g_games[i].used && strcmp(g_games[i].id, id) == 0)           //  6,676,479 Ir
            return &g_games[i];                                         // => __strcmp_avx2: 3,285,536 Ir / 148,824 calls
    return NULL;
}
```

Every authenticated request (`/create`, `/meta`, `/action`) linearly scans
up to `MAX_USERS`=512 token slots and up to `MAX_GAMES`=256 game-id slots
with a `strcmp` each — 1,077,999 token `strcmp` calls alone in this run.

Also notable: the ACTUAL kernel work is cheap relative to the plumbing
around it. Self-cost (own instructions, excluding callees):
`h_state` 0.10%, `view.c:state_put` 0.08%, `h_status` 0.05% — each two
orders of magnitude below the thread-spawn `memset`. Looked at
*inclusively* instead (every instruction spent handling the request,
callees included, from `conn_thread`'s dispatch line):
`h_state` costs 14,896,544 Ir over 4,799 calls (**~3,104 instructions per
`/state` request**, `annotated.txt:588`), `h_status` costs 7,058,834 Ir over
2,238 calls (**~3,155 instructions per `/status` request**,
`annotated.txt:590`) — both trivial next to the ~91,800 instructions of
loader `memset` alone that a fresh thread's stack/TLS setup costs per
connection (2,144,628,187 total `memset` instructions / 23,359 requests in
this run).

### macOS multi-core note

callgrind **serializes threads** (all guest execution is single-threaded
under its own JIT scheduler), so this T1 picture is a **single logical
CPU's** view of the server's per-request work, not a real 4-core-under-load
picture. `profile.sh` auto-selects macOS's `sample` when present specifically
for this reason: `sample <pid> <secs> -file <out>` attaches to a *running,
already-multi-threaded* process and does genuine wall-clock sampling across
every core — that's the path to run on a Mac for the real concurrent picture
(`profile.sh --attach <label> <pid> <secs>`, server started separately,
hammered concurrently from another terminal).

---

## T2 — the infinite oracle, hammered directly (no server), TT20

### Build

```sh
cd c
clang -O3 -ffast-math -g -fno-omit-frame-pointer -Isrc -Wno-deprecated-declarations \
  -DCD_TT_BITS=20 -DCD_TT_2WAY -DCD_TT_PACK8 -DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD \
  -DCD_LEAFBOOK -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 \
  $(make -s print-core) src/main_eval.c \
  -o server/impls/native/bench_results/eval_t2_oracle -lm
```

### Run + profile

```sh
cd server/impls/native
./profile.sh --launch T2_octogen_oracle -- \
  ./bench_results/eval_t2_oracle --strategy=octogen --opp=octogen --players=2 --games=1 --seed-start=1
```

Native (no callgrind) this game takes ~1.36s. Under callgrind: **61.7s**
(~45x slower — octogen is CPU-bound MC/solver work, so it sits at the top of
the documented 30-50x band, unlike T1's I/O-bound server). **13,415,090,166**
instructions collected for the single decision-tree of one pc2 game.

### Top self-cost functions (Ir, `bench_results/T2_octogen_oracle/annotated.txt`)

(`'2` is callgrind's suffix for a compiler-cloned second instance of the same
source function — clang split `sim_solve_rec` into two specialized bodies at
`-O3`; both clones are the identical recursive minimax, this is a compiler
artifact, not a second function.)

| Ir | % | function |
|---|---|---|
| 4,854,274,450 | 36.19% | **`cordite_sim.c:sim_solve_rec'2` (the exact endgame solver's recursion)** |
| 2,656,185,916 | 19.80% | `cordite_sim.c:sim_gen_moves` |
| 973,772,394 | 7.26% | libc `__memcpy_avx_unaligned_erms` |
| 944,458,494 | 7.04% | `cordite_sim.c:sim_apply_sol` |
| 884,788,168 | 6.60% | `src/leafbook.h:leafbook_key` |
| 739,490,174 | 5.51% | `cordite_sim.c:cover_assign` |
| 446,091,188 | 3.33% | `cordite_sim.c:sim_refill` |
| 427,401,511 | 3.19% | `cordite_sim.c:sim_handwritten_move` |
| 408,812,757 | 3.05% | `cordite_sim.c:cover_assign'2` |
| 319,843,675 | 2.38% | `cordite_sim.c:cd_sim_playout_pol` |
| 282,754,335 | 2.11% | `cordite_sim.c:sim_apply_cover` |

### Cache-pressure supplement (`--cache-sim=yes`)

Plain Ir counting hides the whole point of a bigger TT (it's a *memory*
knob, not an *instruction-count* knob — T2 and T3 retire almost the
identical instruction count, see T2-vs-T3 below), so a second run enabled
callgrind's cache simulator:

```sh
valgrind --tool=callgrind --cache-sim=yes \
  --callgrind-out-file=bench_results/T2_octogen_oracle/raw_cache/callgrind.out \
  -- ./bench_results/eval_t2_oracle --strategy=octogen --opp=octogen --players=2 --games=1 --seed-start=1
callgrind_annotate --auto=yes --inclusive=no bench_results/T2_octogen_oracle/raw_cache/callgrind.out \
  > bench_results/T2_octogen_oracle/annotated_cache.txt   # gitignored (large); excerpt committed
```

Detected host cache geometry: **D1 = 49,152 B (48 KiB), 12-way; LL ≈ 264
MiB**.

```
D refs:       4,577,483,780
D1  misses:       7,139,942  (0.2%)
LLd misses:         163,977  (0.0%)
```

See the T2-vs-T3 comparison below for what this means and the exact hot
line (`bench_results/T2_octogen_oracle/annotated_cache_excerpt.txt`).

---

## T3 — regular (non-infinite) octogen, production TT12, direct

### Build

```sh
cd c
clang -O3 -ffast-math -g -fno-omit-frame-pointer -Isrc -Wno-deprecated-declarations \
  -DCD_TT_BITS=12 -DCD_TT_2WAY -DCD_TT_PACK8 -DCD_LEAFBOOK \
  -DMAX_LOG_PAIRS=64 -DMAX_BATTLES=64 \
  $(make -s print-core) src/main_eval.c \
  -o server/impls/native/bench_results/eval_t3_prod -lm
```

Same as T2 except `-DCD_TT_BITS=12` (32 KiB solver table, `-DCD_TT_2WAY
-DCD_TT_PACK8` unchanged) and no `-DOG_EXPLAIN_BUILD -DFOOLISH_ORACLE_BUILD`.

### Run + profile

```sh
cd server/impls/native
./profile.sh --launch T3_octogen_prod -- \
  ./bench_results/eval_t3_prod --strategy=octogen --opp=octogen --players=2 --games=1 --seed-start=1
```

Native: ~1.4s. Under callgrind: **60.4s** (~43x). **13,373,127,339**
instructions collected — essentially the same decision tree as T2 (this
seed's game does not hit the deck-empty extended-window branch where
octogen and its TT size can actually change a decision — see `c/OCTOGEN.md`
for when the two builds *do* diverge).

### Top self-cost functions (Ir, `bench_results/T3_octogen_prod/annotated.txt`)

| Ir | % | function |
|---|---|---|
| 4,854,274,450 | 36.30% | **`cordite_sim.c:sim_solve_rec'2`** |
| 2,656,185,916 | 19.86% | `cordite_sim.c:sim_gen_moves` |
| 973,772,394 | 7.28% | libc `__memcpy_avx_unaligned_erms` |
| 944,458,494 | 7.06% | `cordite_sim.c:sim_apply_sol` |
| 884,788,168 | 6.62% | `src/leafbook.h:leafbook_key` |
| 739,490,174 | 5.53% | `cordite_sim.c:cover_assign` |
| 446,091,188 | 3.34% | `cordite_sim.c:sim_refill` |
| 427,401,511 | 3.20% | `cordite_sim.c:sim_handwritten_move` |
| 408,812,757 | 3.06% | `cordite_sim.c:cover_assign'2` |
| 319,843,675 | 2.39% | `cordite_sim.c:cd_sim_playout_pol` |
| 282,754,335 | 2.11% | `cordite_sim.c:sim_apply_cover` |

Instruction counts are (as expected — see above) within noise of T2's table.

### Cache-pressure supplement

```sh
valgrind --tool=callgrind --cache-sim=yes \
  --callgrind-out-file=bench_results/T3_octogen_prod/raw_cache/callgrind.out \
  -- ./bench_results/eval_t3_prod --strategy=octogen --opp=octogen --players=2 --games=1 --seed-start=1
```

```
D refs:       4,536,031,804
D1  misses:       2,678,601  (0.1%)
LLd misses:          33,406  (0.0%)
```

---

## T2 vs T3 — where the 8 MiB vs 32 KiB table actually costs

The two builds differ in exactly two things: `-DCD_TT_BITS` (20 vs 12 →
2²⁰ vs 2¹² slots × 8 B/`CdTTEntry` under `CD_TT_PACK8` = **8 MiB vs 32
KiB**) and the `OG_EXPLAIN`/oracle machinery (dump buffer + hooks, dormant
unless a dump is actually read — negligible here, no dump was read).
Both builds retire essentially the **same instruction count** for the same
seed (13.415B vs 13.373B, +0.3%) and the **same per-function Ir
breakdown** — same algorithm, same node count, same branches taken, because
this game's decisions never enter the extended solve window where the two
octogen variants actually diverge (`c/OCTOGEN.md`). So a plain
instruction-count profile makes the two builds look identical. **They are
not** — the difference is entirely in what those identical instructions
cost per memory access, and `--cache-sim=yes` shows it directly:

| | T3 (TT12, 32 KiB) | T2 (TT20, 8 MiB) | ratio |
|---|---|---|---|
| D1 (L1 data) misses | 2,678,601 (0.1%) | 7,139,942 (0.2%) | **2.7x** |
| LLd (last-level) misses | 33,406 | 163,977 | **4.9x** |

And it's not spread out — it's **one line**. `cordite_sim.c`'s solver probe
(`sim_solve_rec`, the exact endgame minimax) hashes the sampled-world state
into a 2-way-associative bucket and checks slot 0 first
(`cordite_sim.c` inside `sim_solve_rec`, `#ifdef CD_TT_2WAY`):

```c
CdTTEntry *bkt = &tbl[key & tmask & ~1ull];
if (bkt[0].valid && bkt[0].key == CD_TT_KEYTAG(key)) e = &bkt[0];   // <-- this line
else if (bkt[1].valid && bkt[1].key == CD_TT_KEYTAG(key)) e = &bkt[1];
else                                        e = &bkt[0];
```

That single `if` line accounts for:

| | T3 | T2 |
|---|---|---|
| D1 read misses on this line | 1,277,016 (**72.35%** of the whole program's D1 misses) | 4,470,553 (**85.03%** of the whole program's D1 misses) |
| LL read misses on this line | ~0 (below display threshold, program total LLd misses = 1,297) | 131,072 (**99.02%** of the whole program's LL misses) |

(full excerpt: `bench_results/T2_octogen_oracle/annotated_cache_excerpt.txt`,
`bench_results/T3_octogen_prod/annotated_cache_excerpt.txt`)

**Why**: this box's simulated D1 cache is 48 KiB. TT12's whole table is 32
KiB — it fits inside D1 *alongside* the rest of the solver's working set, so
after the first touch nearly every probe of an already-visited slot is a
cache hit; the handful of real misses are genuine one-time compulsory
misses. TT20's table is 8 MiB — **170x bigger than the entire D1 cache** —
so a probe of any slot not touched in the last few thousand accesses is
essentially guaranteed to miss D1, and a large fraction of those distinct
8 MiB-spanning slots also miss the (simulated) last-level cache the first
time they're touched, which is why LLd misses jump ~100x (1,297 → 132,375,
program-wide) even though the *simulated* LL (264 MiB) is nominally big
enough to hold the whole table — it's a cold/compulsory-miss story over a
256x-bigger footprint, not a capacity-eviction story. This is exactly the
tradeoff `c/Makefile`'s own comments describe (TT is "a bot-strength knob"
sized by collision/divergence measurement, not by cache cost) — the
oracle build spends that cache cost deliberately, because it runs once per
click in a browser tab with "hundreds of MB and minutes of attention to
spare" (`docs/INFINITE_ORACLE_DESIGN.md` §3), not thousands of times a
second in a shared server process.

---

## Where to speed up

**T1 (server hit path):**
1. **Thread-per-connection is the single biggest cost by far (85.84% of all
   instructions retired under load).** Every request — even a 400 Bad
   Request — pays a full `pthread_create` (fresh 8 MiB stack + TLS block,
   zeroed by the loader's `memset`). A thread pool (fixed N worker threads,
   an accept-queue) or an epoll/kqueue event loop would collapse this to
   near zero, and is the highest-leverage single change available.
2. `user_by_token`/`game_by_id` are O(`MAX_USERS`)/O(`MAX_GAMES`) linear
   scans with a `strcmp` per slot, on **every** authenticated request
   (1.08M token `strcmp` calls in one 20s run). A `token -> User*` /
   `id -> GameSlot*` open-addressing hash map (both are small, fixed-size
   tables already — `MAX_USERS`=512, `MAX_GAMES`=256 — so even a trivial
   hash table sized to those caps is a strict win with no growth logic
   needed) turns both into O(1).
3. HTTP request-line parsing (`sscanf(buf, "%7s %255s", ...)`, 2.11% of all
   instructions in `__vfscanf_internal`) and header scanning
   (`strcasestr` for `content-length:`/`authorization:`, 1.58% + 0.78% +
   0.72%) are surprisingly expensive for a 2-field split; a hand-rolled
   `strchr(' ')`-based split plus one linear header scan (both headers in a
   single pass) would cut this by roughly half without touching semantics.
4. The actual kernel work — `awire_decode`/`awire_apply`,
   `calculate_legal_moves`, `state_put` — is cheap relative to the above
   (all under 1% individually). The kernel isn't the bottleneck here; the
   HTTP/connection plumbing around it is.

**T2/T3 (octogen solver):**
5. The TT-probe line above is the single hottest cache-miss site in both
   builds, and it *grows* with `CD_TT_BITS` by design. For the shipped
   server-side bot tiers (TT12/13), it already fits in L1 — no action
   needed there. For the oracle build (TT20), the cost is deliberate and
   bounded to one browser tab; if a future variant ever ran a big-TT
   octogen many times concurrently server-side, the fix would be reducing
   `CD_TT_BITS` (a strength/cache tradeoff already instrumented by
   `tools/tt_divergence.sh`) rather than a code change.
6. `sim_gen_moves` (19.8-19.9%) and `cover_assign`/`cover_assign'2`
   (5.5%+3.1%) are the next-biggest non-solver costs — move/cover
   enumeration, called from inside the solver's recursion. Any of the
   documented C6/C3-style TT packing or working-set tricks in
   `cordite_sim.c`'s comments that reduce *recursion node count* (not just
   per-node cost) pay off across both of these simultaneously, since
   they're both called once per visited node.
7. Remember the `-flto` caveat from the top of this document: in the
   *production* binary, `can_cover`/`game_done`/`hand_remove_card`/`card_eq`
   and friends are folded into their hot-loop callers, so a production
   profile (if one could be taken) would show fewer, larger functions than
   this non-LTO T2/T3 breakdown — optimize the *lines*, not just the
   *function boundaries*, since LTO will move the boundaries anyway.

---

## Files

- `foolish_hammer.c` — the load-test client (Deliverable 1).
- `profile.sh` — the portable profiler wrapper (Deliverable 2).
- `Makefile` — OS-aware `LDFLAGS`, plus `foolish_hammer` and
  `foolish_server_prof` targets.
- `bench_results/T1_server/` — server hit-path profile (`annotated.txt`,
  `meta.txt`).
- `bench_results/T2_octogen_oracle/` — oracle-flags octogen profile
  (`annotated.txt`, `annotated_cache_excerpt.txt`, `meta.txt`).
- `bench_results/T3_octogen_prod/` — production-flags octogen profile (same
  layout).
- Raw `callgrind.out*` dumps and the two standalone `eval_t2_oracle` /
  `eval_t3_prod` binaries are gitignored (`.gitignore`) — reproduce them
  with the build/run commands above.

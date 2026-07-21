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

## T1b — WebSocket server + legal-move clients (after items 1-3)

T1's own "where to speed up" list (items 1-3 above) is now implemented and
re-measured. Summary of the changes, all under `server/impls/native/`
(`c/src/*` untouched):

1. **RFC 6455 WebSockets, new files `ws.h`/`ws.c`.** A self-contained SHA-1
   (textbook, KAT-checked against `sha1("")` and the RFC 6455 handshake
   example — see the check below) + base64 encoder compute
   `Sec-WebSocket-Accept`; `WsConn` does frame I/O (masked/unmasked per
   role, 7/16/64-bit length forms, PING/PONG/CLOSE, minimal fragment
   assembly). `GET /ws?game_id=..&seat=..` (Bearer token) validates the
   caller owns that seat exactly like `/action` did, upgrades, then loops:
   client sends a binary frame (an awire move, or empty = "just poll") ->
   server applies it through the SAME `awire_decode`/`awire_apply` `/action`
   used -> answers with one binary frame, `[ok:u8][state_put(seat) bytes]`.
   One handshake per client SESSION, not per move.
2. **Thread-per-connection already amortizes now that connections are
   long-lived.** No change to the `accept()`/`pthread_create` loop was
   needed — `conn_thread` already ran once per accepted socket; the only
   change is that a `/ws` connection no longer closes after one request, so
   the SAME thread now serves a client's entire session. Two bugs this
   surfaced once connections stopped being one-shot: (a) missing
   `TCP_NODELAY` on accepted sockets (only the load client's outbound
   `connect_to()` had it) — invisible on one-shot HTTP, but Nagle batching
   against the peer's delayed ACKs turned every persistent-connection round
   trip into tens of milliseconds; fixed by setting it right after
   `accept()`. (b) an unhandled `SIGPIPE` — a peer that vanishes between a
   read and the next write now kills a THREAD's connection instead of (with
   the default disposition) the whole process; both `foolish_server.c` and
   `foolish_hammer.c` now `signal(SIGPIPE, SIG_IGN)` in `main()`.
3. **`user_by_token`/`game_by_id` are now fixed-size open-addressing hash
   maps** (`token -> User*`, 1024 slots; `game_id -> GameSlot*`, 512 slots;
   FNV-1a, insert-only — see foolish_server.c's comment for why no
   tombstone/growth logic is needed). Same behavior, O(1) instead of
   O(MAX_USERS)/O(MAX_GAMES).
4. **The HTTP request-line/header parser is hand-rolled**
   (`parse_request_line_and_headers`): one pass over the header block with
   `strncasecmp` on already-located header names, replacing
   `sscanf(buf, "%7s %255s", ...)` and the `strcasestr` header scan. Every
   existing endpoint (`/auth/signup`, `/create`, `/meta`, `/action`,
   `/state`, `/status`, `/health`) is unchanged behaviorally — `test.sh`
   still passes (its one `xxd` call was swapped for `od -A x -t x1z`; this
   box has no `xxd`, a pre-existing environment gap unrelated to this
   change) — plus a new WS smoke test at the bottom of `test.sh` that runs
   `foolish_hammer --mode=ws` at a tiny scale end-to-end.
5. **`foolish_hammer --mode=ws`**: setup is identical to `action`/`mixed`
   (signup, `/create`, `/meta join` for every seat, `/meta start` — never
   `add-bot`), then spawns ONE THREAD PER (game, seat) pair — `--conns` is
   ignored in this mode, since a persistent-connection client isn't an
   interchangeable pool worker the way a stateless HTTP requester is. Each
   thread: connect + WS handshake once, then loop receiving the pushed
   `[ok][state]`, `state_get`-ing it, `calculate_legal_moves` for its OWN
   seat, and either `awire_encode`-ing a RANDOMLY CHOSEN legal move (a real
   attack/cover/pass/pickup/good, never a synthesized illegal one) or, if
   this seat currently has none (Durak often has several eligible seats at
   once — attacker(s) + defender — and just as often none for a given
   seat), sending an empty "poll" frame so it still notices when another
   seat's move changes its own eligibility. On game-over it posts
   `/meta continue` + `/meta start` (a rematch, reusing the same
   game_id/seat/connection) so load keeps flowing instead of idling out.

SHA-1 + base64 + handshake KAT check (`ws_sha1("")` and the RFC 6455 doc
example key, run once during development, not part of the build):

```
sha1('')= da39a3ee5e6b4b0d3255bfef95601890afd80709 (expect da39a3ee5e6b4b0d3255bfef95601890afd80709)
accept  = s3pPLMBiTxaQ9kYGzzhZRbK+xOo= (expect s3pPLMBiTxaQ9kYGzzhZRbK+xOo=)
```

### Headline throughput (full-speed, NOT under callgrind)

Same scale as T1's headline (`--games=40 --seats=4 --conns=32 --secs=10`,
`--conns` unused in `--mode=ws` — the actual connection count is
`games*seats` = 160, one per seat):

```sh
./foolish_server 8097 &
./foolish_hammer --host=127.0.0.1 --port=8097 --games=40 --seats=4 --conns=32 --secs=10 --mode=ws
```

Six runs on this shared 4-core box (run-to-run variance is real here — this
is a shared machine, and see the mutex/CPU-contention discussion below):

| run | messages/s | actions submitted | applied (ok=true) | applied/s |
|---|---|---|---|---|
| 1 | 124,675.3 | — | — | 3,287.4 |
| 2 | 126,424.9 | — | — | 1,545.0 |
| 3 | 123,869.9 | — | — | 1,499.4 |
| 4 | 123,074.7 | 18,274 | 17,543 | 1,739.6 |
| 5 | 119,271.3 | 16,337 | 15,607 | 1,549.0 |
| 6 | 120,080.8 | 24,772 | 23,726 | 2,362.4 |

Mean **1,997 applied/s** (range 1,499–3,287/s), messages/s consistently
~120,000–126,000/s. Every submitted action decodes and is genuinely legal
(`calculate_legal_moves` picked it), so `applied/s` is essentially
`actions submitted/s` (~90-95% land — the rest lose a race: the table
changed between this seat computing its legal moves and the move landing,
e.g. another seat covered the same attack first).

**Compare to T1's HTTP baseline: ~13,400 req/s pure-hit-path / ~1,442
applied/s in `--mode=mixed`** (only ~17% of `--mode=mixed`'s submitted
actions were even attempted-legal, `mixed` picks a real legal move only
1-in-5 tries and otherwise sends a random frame). T1b's mean applied/s
(1,997) is **~1.4x T1's mixed-mode number, up to ~2.3x on the best run** —
and, unlike T1's, essentially every submitted move is real, kernel-applied
game progress, not a 400 reject. A `--games=100` run (400 persistent
connections, more independent critical paths for the 4 cores to interleave)
measured **3,040.9 applied/s**, so the ceiling above is the polling
client's tuning + this box's core count, not a hard architectural limit —
see the `WS_IDLE_POLL_US` note in `foolish_hammer.c`: at 40 games the idle
seats (only ~1-2 of a game's seats are ever eligible at once) polling every
1ms is *itself* competing for the same 4 cores as the productive round
trips, and unlike T1's, this new bottleneck is a **tuning knob in the load
client**, not a server design flaw — the server has no artificial pacing
anywhere in the `/ws` hot path.

One fix mattered more than any tuning: accepted sockets never had
`TCP_NODELAY` set before this work (only the outbound client side did).
Before that fix, the SAME 40-games/10s config measured **755-815
applied/s** — Nagle-vs-delayed-ACK stalls on the server's accept side were
silently capping every persistent connection's round-trip rate. `TCP_NODELAY`
alone was worth ~2x here, on top of removing the thread-spawn tax.

### Profiling (callgrind)

Same technique as T1 (`profile.sh`'s own callgrind path): launch
`valgrind --tool=callgrind` on `foolish_server_prof` directly, drive it with
`foolish_hammer`, then `SIGTERM` the valgrind process so it flushes
`callgrind.out` (wrapping in `timeout` would trace only `timeout`'s own
startup, per T1's note above). Scale is deliberately SMALL, like T1's own
callgrind run (`--games=3 --seats=2`, 6 persistent connections instead of
40 games/160) — under 30-50x instrumentation slowdown, spinning up 160 real
WS handshakes would dominate the whole capture:

```sh
valgrind --tool=callgrind \
  --callgrind-out-file=bench_results/T1b_ws/raw/callgrind.out \
  -- ./foolish_server_prof 8096 &

./foolish_hammer --host=127.0.0.1 --port=8096 --games=3 --seats=2 --secs=20 --mode=ws
kill -TERM <valgrind pid>

callgrind_annotate --auto=yes --inclusive=no bench_results/T1b_ws/raw/callgrind.out \
  > bench_results/T1b_ws/annotated.txt
```

**81,096,990** instructions collected over the 20s window: 340 real applied
moves, 97,557 total `[ok][state]` round trips (the rest are empty polls).
Unlike T1's callgrind run — where the un-instrumented server was clearly
request-RATE-bound and callgrind measured an 11.5x slowdown against it —
this small a WS scale (6 connections) is *client poll-interval*-bound, not
server-CPU-bound: the SAME `--games=3 --seats=2 --secs=20` config run
NATIVELY (no callgrind) measured 5,036.1 msgs/s, statistically the same as
the 4,867.9 msgs/s measured UNDER callgrind. Six idle-capable connections
polling at most once per `WS_IDLE_POLL_US` (1ms) top out around
6,000 msgs/s regardless of how fast the server can technically answer, so
this capture's instruction counts are a clean per-message attribution, not
a throughput measurement — that's what the headline numbers above are for.

### Top self-cost functions (Ir, `bench_results/T1b_ws/annotated.txt`)

| Ir | % | function |
|---|---|---|
| 16,922,819 | 20.87% | `view.c:state_put` |
| 14,074,102 | 17.35% | `ws.c:ws_recv_message` |
| 8,390,422 | 10.35% | `ws.c:ws_send_frame` |
| 6,256,736 | 7.72% | libc `nptl/cancellation.c:__pthread_enable_asynccancel` |
| 5,865,675 | 7.23% | libc `nptl/cancellation.c:__pthread_disable_asynccancel` |
| 4,895,635 | 6.04% | `wasm/wire.h:state_put` (a state_put callee, non-LTO — see the T2/T3 `-flto` caveat at the top of this doc) |
| 4,691,664 | 5.79% | libc `read` |
| 4,684,454 | 5.78% | libc `write` |
| 3,940,504 | 4.86% | `foolish_server.c:conn_thread` |
| 3,515,324 | 4.33% | libc `pthread_mutex_lock` |
| 3,190,317 | 3.93% | `ld-linux-x86-64.so.2` `memset` (thread-stack/TLS zeroing — see below) |
| 2,539,499 | 3.13% | libc `pthread_mutex_unlock` |
| 782,092 | 0.96% | libc `nptl/descr.h:__pthread_enable_asynccancel` (inlined variant) |
| 360,984 | 0.45% | `card.h:state_put` (another state_put callee) |
| 118,191 | 0.15% | libc `__memset_avx2_unaligned_erms` |

### The pthread_create / thread-stack-zeroing cost, before vs after

This is the number the whole exercise was about. `pthread_create` is called
**35 times** in this 20s, 97,557-message run (32x for `conn_thread` — HTTP
setup/rematch calls plus the 6 WS upgrades; 3x for the per-game bot-pacing
`bot_thread`, one per game regardless of whether it has a legal move to
make) — **not once per message**. Its total cost, `pthread_create` itself
plus the loader's thread-stack `memset` it triggers, is
**3,145,099 Ir (271,236 + 2,873,863) = 3.88% of the whole run**, down from
**85.84% of 2.5 BILLION instructions** in T1 (2,144,628,187 Ir). The
remaining `ld-linux` `memset` line above (3.93%, 3,190,317 Ir) IS that same
per-thread stack-zeroing cost, at almost exactly T1's own measured
per-connection rate (3,190,317 / 35 ≈ 91,152 Ir/call vs T1's reported
"~91,800 instructions... per connection") — same fixed cost, paid 35 times
in a 20s run that pushed 97,557 messages instead of once per message. **Item
1 is fixed**: `pthread_create` cost now scales with connection count, not
message count.

### Where the cost went instead

Per-line attribution inside `h_ws`'s hot loop (the same
`bench_results/T1b_ws/annotated.txt`, lines ~679-706) shows exactly what
each of the 97,557 round trips actually pays for, INCLUSIVE of callees:

```c
while ((mlen = ws_recv_message(&wc, in, sizeof in, &opcode)) >= 0) {  // 31.09% (ws_recv_message)
    ...
    pthread_mutex_lock(&g_lock);                                      //  4.33% (mutex)
    if (s->used) {
        if (mlen > 0 && s->game.status == GAME_STATUS_PLAYING) {
            if (awire_decode(in, mlen, &a) && awire_apply(&s->game, seat, &a)) {  // 0.15% + 0.02%
                ...
            }
        }
        slen = state_put(&s->game, seat, msg + 1);                    // 27.35% (state_put, EVERY round trip)
    }
    pthread_mutex_unlock(&g_lock);                                    //  3.13% (mutex)
    msg[0] = applied ? 1 : 0;
    if (ws_send_frame(&wc, WS_OP_BIN, msg, slen + 1) < 0) break;       // 24.06% (ws_send_frame)
}
```

`awire_decode`/`awire_apply` — the actual KERNEL work — cost 0.15%+0.02%
combined, and only run on the 340 real moves (not the 97,217 polls), exactly
as light as T1 found them (T1: all kernel calls individually under 1%). The
new cost is almost entirely **WS protocol overhead** (`ws_recv_message` +
`ws_send_frame`, 41.4% combined — frame header parse/build, the read()/
write() syscalls themselves at 5.79%+5.78%, and masking/unmasking) and
**`state_put` running on every single round trip, including pure polls**
(27.35% + the 6.04% + 0.45% callee slivers ≈ 33.8% of the whole program).
That last point is the real, actionable finding here: a poll that finds
nothing to do STILL pays a full masked-view serialization, because the
protocol always answers with fresh state. A cheaper "nothing changed"
signal (e.g. a monotonic per-game version counter the client can compare
before asking for a full re-serialize) would cut a large fraction of this
without changing the wire's meaning — left as a follow-up, not implemented
here (scope: items 1-3 of T1's list, not a new protocol optimization).

**global mutex**: `pthread_mutex_lock`+`unlock` together are 7.46%
(4.33%+3.13%) — real, measurable, and exactly where T1 predicted the next
bottleneck would surface once the thread-spawn tax was gone — but it is
NOT yet the dominant cost; `state_put` and the WS frame I/O are both
several times larger. Consistent with the full-speed finding above (more
games scales better than shrinking the per-poll interval): the lock is a
single serialization point across ALL games, so at higher game counts (see
the `--games=100` measurement, 3,040.9 applied/s) it would be the first
thing to profile next.

### Files (T1b additions)

- `ws.h`/`ws.c` — the WebSocket handshake (SHA-1 + base64) and frame I/O,
  shared by `foolish_server.c` (server role: unmasked outgoing frames) and
  `foolish_hammer.c` (client role: masked outgoing frames).
- `bench_results/T1b_ws/` — this section's profile (`annotated.txt`,
  `meta.txt`). Raw `callgrind.out` gitignored, same pattern as `T1_server/`.

---

## T1c — current WS server: hot lines, latency, memory, and speedups

T1b's own "left as a follow-up" note called out the biggest remaining item:
`state_put` running on *every* `/ws` round trip, including a pure poll that
changes nothing. This section (1) instruments the load client to measure
SERVER-attributable round-trip latency directly, (2) adds a memory sampler so
"how much RAM for N concurrent games/connections" has a real answer, (3)
re-profiles the CURRENT server at the source-LINE level, (4) implements and
measures the clearly-correct speedups, and (5) is honest about what's still
open. Same box, same toolchain, same technique as T1/T1b (clang 18, valgrind
callgrind only — no perf/sample on this box).

### Deliverable A — client-side round-trip latency (`foolish_hammer.c`)

`--mode=ws`'s `ws_worker` now times every round trip: the moment it sends a
move/poll frame (`ws_send_timed`, wrapping `ws_send_frame`) to the moment the
*next* `ws_recv_message` on that connection returns (the server's pushed
`[ok][state]` answer to that specific frame). The very first receive on a
fresh/reconnected connection — the server's post-handshake initial push,
which answers no frame this loop sent — is explicitly excluded
(`have_pending`), so it can't leak a bogus zero-latency or huge-latency
sample. Network latency is ~0 on loopback, so this measures the SERVER's own
latency floor (parse + lock + apply/serialize + write) and how it degrades
under load — exactly the "client-attributable vs server-attributable" split
the task asked for.

Samples are kept in a per-thread fixed-size reservoir (`LAT_RESERVOIR_CAP` =
4096, classic algorithm R) so a long/high-connection-count run reports
correct percentiles from an unbiased random subsample without unbounded
memory — a run's real round-trip count (`lat_seen`) can be, and regularly is
in these measurements, 10-50x the reservoir size. At the end of the run every
thread's reservoir is merged, sorted (`qsort`), and mean/p50/p90/p99/max are
computed from the real sorted sample (not a histogram approximation). Output
includes one grep-able `latency_summary_us: conns=N count=... mean=...
p50=... p90=... p99=... max=...` line per run — the "`--conns`-sweep-friendly"
output the task asked for (`--mode=ws` has no `--conns` knob of its own;
concurrency is `--games`×`--seats`, so a wrapper sweeping those and grepping
this line builds a concurrency-vs-latency table without parsing the whole
summary block).

### Deliverable B — memory sampler (`mem_sample.sh`)

`mem_sample.sh <pid> <duration_secs> [interval_secs]` polls
`/proc/<pid>/status`'s `VmRSS` at a fixed interval (default 0.2s) for the
given window, emitting a `t_secs,vmrss_kb` CSV on stdout and a summary on
stderr: the first sample (~idle-at-start), the mean across the window, the
peak SAMPLED value, and `VmHWM` — the kernel's own resident-set high-water
mark, read once at the end, which can exceed the sampled peak because it
catches spikes between samples. Usage is "start the server, start it
sampling in the background, run the load client, read the summary" — see the
numbers below, all gathered this way.

### Deliverable C — hot-LINE profile of the CURRENT (pre-speedup) server

Same technique as T1/T1b: `valgrind --tool=callgrind` launched directly on
`foolish_server_prof` (never wrapped in `timeout` — see T1's note),
`foolish_hammer --games=3 --seats=2 --secs=20 --mode=ws` (6 persistent
connections) driving it, `SIGTERM` to the valgrind process, then
`callgrind_annotate --auto=yes --inclusive=no`. Digested output:
`bench_results/T1c_ws_lines/annotated.txt` (raw `callgrind.out` gitignored,
same pattern as the other `bench_results/*/raw/` dirs).

**85,441,022 instructions** collected over 93,748 total round trips (787
applied moves, 92,943 empty polls) — consistent with T1b's own capture at
this scale (81M Ir / 97,557 round trips), confirming the workload shape is
stable run to run.

Top self-cost functions (Ir), matching T1b's finding almost exactly (small
run-to-run variance on a shared box, same as T1b's own note):

| Ir | % | function |
|---|---|---|
| 18,682,579 | 21.87% | `view.c:state_put` |
| 13,561,240 | 15.87% | `ws.c:ws_recv_message` (inlines `ws_fill`) |
| 8,062,848 | 9.44% | `ws.c:ws_send_frame` |
| 7,032,150 | 8.23% | `wasm/wire.h:state_put` (a `state_put` callee) |
| 6,028,432 | 7.06% | libc `__pthread_enable_asynccancel` |
| 5,651,640 | 6.61% | libc `__pthread_disable_asynccancel` |
| 4,873,907 | 5.70% | `ld-linux` `memset` (thread-stack zeroing, T1's old #1 — now a rounding error) |
| 4,520,352 | 5.29% | libc `read` |
| 4,502,342 | 5.27% | libc `write` |
| 3,855,712 | 4.51% | `foolish_server.c:conn_thread` |
| 3,380,036 | 3.96% | libc `pthread_mutex_lock` |
| 2,442,328 | 2.86% | libc `pthread_mutex_unlock` |
| 521,499 | 0.61% | `card.h:state_put` (another `state_put` callee) |

**Top 3 hottest source LINES** (file:line, inclusive Ir — i.e. everything
that line's call actually costs, the natural way to rank a call-heavy hot
loop):

1. **`foolish_server.c:596`** (`h_ws`'s loop) — `slen = state_put(&s->game,
   seat, msg + 1);` → **26,233,028 Ir (30.70% of the whole program)**.
   `state_put`'s own body (view.c 21.87% + wire.h 8.23% + card.h 0.61%)
   accounts for essentially all of it. This runs on *every* round trip —
   93,748 times in this capture — even though only 787 of them (0.84%)
   actually changed the board.
2. **`foolish_server.c:584`** — `while ((mlen = ws_recv_message(&wc, in,
   sizeof in, &opcode)) >= 0) {` → **24,294,397 Ir (28.43%)**, inclusive of
   `ws_recv_message`'s whole receive path.
3. **`foolish_server.c:602`** — `if (ws_send_frame(&wc, WS_OP_BIN, msg, slen
   + 1) < 0) break;` → **18,750,493 Ir (21.95%)**, inclusive of the send
   path.

A fourth line worth calling out because it directly motivated a fix: inside
`ws_recv_message`'s receive path, **`ws.c:124`** (`ws_read_full`) —
`ssize_t r = read(fd, p + got, (size_t)(n - got));` → **10,733,157 Ir
(12.56% of the WHOLE PROGRAM), called 188,301 times for 93,748 messages** —
on average **exactly 2 `read()` syscalls per message**, because the old
`ws_fill` asked for each framing field (2-byte header, sometimes a 2/8-byte
extended length, sometimes a 4-byte mask key, then the payload) with its own
EXACT-SIZE blocking read, even when a compliant peer's whole frame had
already arrived in one TCP segment sitting in the kernel's receive buffer.

`awire_decode`/`awire_apply` (the actual kernel work) stayed exactly as
cheap as T1/T1b found them: 0.35%/0.05% here (804 real moves recorded in
this run's line annotation vs. this capture's own 787 applied-move count —
the small gap is moves that decoded but were rejected by the kernel's own
legality check, e.g. a race with another seat).

### Deliverable D — speedups: analysis, what was implemented, what was left open

**1. `state_put` on every round trip — IMPLEMENTED.** `GameSlot` (in
`foolish_server.c`) gained a per-game monotonic `version` counter and, per
seat, a cached `state_put(...)` output (`view_cache[MAX_PLAYERS][1024]` +
`view_cache_len` + `view_cache_version`). `version` is bumped under `g_lock`
every time this slot's `Game` could have changed: a human's move applied via
`/action` or `/ws` (`h_action`, `h_ws` — only on the `ok`/`applied` branch,
never on a no-op poll), any lobby transition (`h_meta`'s join/add-bot/
start/continue — bumped unconditionally at the end since a no-op bump is
harmless, worst case one extra recompute on the next poll), and a bot-drive
cycle that actually applied ≥1 action or ended the game (`bot_thread`,
`drv.n > 0 || drv.ended >= 0` — a bot's move changes the board exactly like
a human's, and it does **not** go through `h_action`/`h_ws`, so it needed its
own bump site). A new `state_put_cached(GameSlot*, seat, out)` helper
recomputes only when `view_cache_version[seat] != version`, else `memcpy`s
the bytes already computed — same wire bytes either way (byte-identical to
calling `state_put` fresh), so the **client needs zero changes** and still
learns about every real state change on its very next round trip (the
version bump always happens under the same lock the cache check runs under,
so there's no window where a reader can observe a stale version alongside a
fresh `Game`). `h_ws` is the only caller (`h_state`'s HTTP polling endpoint
was deliberately left calling `state_put` directly — its `seat` query
parameter is unbounded/unvalidated pre-existing behavior, unlike `h_ws`'s,
which validates `0 <= seat < num_players` at handshake time before ever
touching the cache array; indexing `view_cache[seat]` with an unvalidated
seat would be a new out-of-bounds bug, so the cache stays scoped to the path
that's actually safe and actually hot). A fresh GameSlot's
`view_cache_version` is initialized to `(uint32_t)-1` (never `0`, `version`'s
own starting value) so the very first call for a seat can't spuriously look
"already cached" and return a never-computed zero-length view. `GameSlot`s
are never recycled in this store (no delete — see the README), so that
initialization only ever needs to happen once, at `h_create`.

**2. WS framing (`ws_recv_message`/`ws_send_frame`) — IMPLEMENTED (both
sub-items).** `ws.c`'s `ws_fill` no longer asks the kernel for an
EXACT-SIZE read per framing field; it refills `WsConn.pending` with ONE
opportunistic `read()` of up to the buffer's full capacity whenever it's
exhausted, then serves every subsequent field from that buffer with zero
syscalls until it runs dry again — a compliant peer's whole small frame (or
several) usually arrives in one segment, so this collapses the old ~2
`read()`s/message down toward 1. `ws_send_frame`'s unmasked (server) path —
the one the profile above shows as hot — now builds a 2-entry `struct iovec`
(header, payload) and sends both in one `writev()` (`writev_full`, a
short-write-safe loop mirroring `ws_write_full`) instead of two separate
`write()` calls; no masking on this path means the payload can go out as-is,
with no scratch-buffer copy needed either. The masked (client) path is
unchanged (not the profiled bottleneck — `foolish_hammer` is a load-test
tool, not the product). Both changes are pure I/O-batching: the wire bytes
sent/received are byte-identical to before, so no protocol change and no
correctness risk beyond "does the read/write loop still handle a short
read/write" — which the loop logic (unchanged for reads; a new,
carefully-mirrored loop for `writev`) still does.

**3. Global mutex sharding — ANALYZED, NOT IMPLEMENTED (left open
deliberately).** The task's own framing says to do this only with high
confidence in correctness, and to say so plainly otherwise — this is that
case. What a per-`GameSlot` lock would need to get right, all simultaneously:
(a) `g_lock` today protects not just each game's `Game`/roster, but also the
GLOBAL `g_users`/`g_games` arrays, the `g_token_ht`/`g_game_ht` hash tables,
and slot allocation in `h_create` — sharding the per-game lock still leaves
a lock (or a lock-free scheme) needed for those, and `h_meta`/`h_action`/
`h_ws` all resolve a user AND a game (two different pieces of shared state)
before ever touching a `GameSlot`, so the locking would become two-level
(global maps, then the one game); (b) `seat_of()` reads `s->game` while
walking `s->seat_user`, so a per-slot lock has to cover the WHOLE `GameSlot`,
not just `s->game`, or roster reads and kernel-state reads could tear; (c)
`bot_thread` holds its game's lock across `bot_drive` and releases it only
for the pacing `usleep` — that pattern is fine per-shard, but needs
re-auditing once "the lock" isn't one global object every code path already
serializes through; (d) there is exactly one lock-ordering rule today
("acquire `g_lock`, do the thing, release it") — replacing it with
"(maybe) acquire the maps lock, then a game lock" introduces a real ordering
question (does anything ever need two DIFFERENT games' locks at once? Not
today — but a shard-locked codebase makes that a standing invariant to keep
proving, not a fact `g_lock` made trivially true by construction). None of
this is unsolvable, but it is exactly the kind of change where "confident it
compiles and tests pass" is a materially lower bar than "confident it's
race-free," and this box has no way to close that gap empirically:
**ThreadSanitizer is unavailable here** (`clang -fsanitize=thread` fails to
link — `libclang_rt.tsan-x86_64.a` is not installed in this Ubuntu/clang-18
image), so a lock-sharding change here would ship on code review alone, with
no tooling to catch a subtle miss. Per the task's own instruction, that's
reason enough to leave it analyzed-but-not-done rather than land a "probably
fine" change to the one thing standing between "single game" and "many
games at once" correctness. See "still open" below for what this costs the
"many players" story today.

### Re-measurement: before vs. after

**Callgrind, identical scale** (`--games=3 --seats=2 --secs=20 --mode=ws`,
same technique, `bench_results/T1d_ws_speedups/`):

| | before (`T1c_ws_lines`) | after (`T1d_ws_speedups`) |
|---|---|---|
| Total instructions (Ir) | 85,441,022 | 53,730,985 (**-37.1%**) |
| Round trips completed | 93,748 | 95,548 (slightly *more* work) |
| Ir per round trip | 911.3 | 562.4 (**-38.3%**) |
| `state_put` + callees (self, Ir) | 26,236,228 (30.7%) | 258,736 (0.5%) — **~101x smaller** |
| `read()` self-cost | 4,520,352 (5.29%), 188,301 calls | 2,296,728 (4.27%), 95,660 calls (**~49% fewer calls**) |
| `write`/`writev` self-cost (`write` symbol total, both roles) | 4,502,342 (5.27%) | 2,293,296 (4.27%) |
| `writev()` calls (server unmasked path only, directly counted) | n/a (this call didn't exist yet — 2 `write()`s per unmasked frame instead: header, then payload, by construction of the old `ws_send_frame`) | 95,554 (one per round trip — **half the syscalls of the old 2-`write()`-per-frame path**) |
| `pthread_mutex_lock`+`unlock` (self, Ir) | 5,822,364 (6.82%) | 5,931,417 (11.04%) — flat in absolute terms, **bigger share of a now-smaller pie** |

The mutex row is the headline finding for item 3's "still open": its
absolute cost didn't change (the lock/unlock pair costs the same
instructions whether or not `state_put` runs inside the critical section),
but with `state_put` gone its share of the total nearly doubled — exactly
the "next thing to profile once the bigger cost is gone" T1b predicted, and
now a materially closer second place than it looked before.

**Throughput + latency, real (non-callgrind) runs, fresh server each time**
(`foolish_hammer --games=40 --seats=4` = 160 connections, and `--games=100
--seats=4` = 400 connections, `--secs=10`):

| | 160 conns, before | 160 conns, after | 400 conns, before | 400 conns, after |
|---|---|---|---|---|
| messages/s | 75,369.2 | 104,300-109,900 (3 runs) | 66,630.7 | 86,403.7 |
| latency mean (µs) | 141.2 | 44.2-78.5 | 455.4 | 277.2 |
| latency p50 (µs) | 4.5 | 1.2 | 5.3 | 1.5 |
| latency p90 (µs) | 371.9 | 108.5-241.9 | 1,268.5 | 546.6 |
| latency p99 (µs) | 1,833.9 | 779.4-1,138.5 | 7,899.4 | 5,586.6 |
| latency max (µs) | 32,779.8 | 14,971.9-32,201.1 | 184,440.5 | 96,082.4 |
| applied moves/s | 2,347.4 | 982.7-2,111.5 | 3,621.1 | 3,218.4 |

Messages/s and every latency percentile improved consistently and
substantially at both scales — the p50 drop (4-5µs → ~1.2-1.5µs) is the
clearest single signal of the cache working: the *median* round trip is a
poll that now costs a lock + a `memcpy` instead of a lock + a full
re-serialize. **Applied moves/s is the one metric that did NOT reliably
improve, and this is expected, not a regression**: `foolish_hammer`'s own
`WS_IDLE_POLL_US` note (in `foolish_hammer.c`, unchanged by this work)
already documents that idle-seat polling competes with productive round
trips for this box's 4 cores; since round trips got cheaper, the SAME client
CPU budget now fits more of them, so idle seats poll more often and eat a
larger share of the same 4 cores — a load-CLIENT tuning artifact (as T1b's
own multi-run table already shows: 1,499-3,287 applied/s run-to-run variance
on unmodified code), not evidence the server regressed. The server-side
signal that actually isolates the server's own cost — the callgrind Ir/msg
number above — is unambiguous and reproducible: -38.3%.

**Memory** (`mem_sample.sh`, sampled every 0.25s across the load window,
fresh server each run):

| | idle | 160 conns, before | 160 conns, after | 400 conns, before | 400 conns, after |
|---|---|---|---|---|---|
| mean under load | 2.7 MB | 101.3 MB | 110.8 MB | 114.6 MB | 95.4 MB |
| peak (sampled) | — | 146.1 MB | 142.2 MB | 327.7 MB | 367.5 MB |
| `VmHWM` (kernel) | 2.7 MB | 152.0 MB | 144.4 MB | 328.7 MB | 371.3 MB |

Memory is essentially **unchanged** by this work (peak varies by ~10% run to
run in both directions — normal variance on a shared box, not a trend) —
expected, since neither speedup touches how many threads exist or how big
each one's stack is; the per-seat view cache adds only ~1 KiB × up to 8
seats per game (see the static-footprint math below), noise next to the
per-connection thread-stack cost that dominates every number in this table.
**Marginal RAM per live WS connection**, computed as `(peak - idle) /
connections` across all four before/after runs: 918.2, 832.6, 893.2, 933.2
KB/connection — consistently **~0.83-0.93 MB per persistent connection**,
regardless of the state_put/ws.c changes. This — not the game state itself —
is the dominant term in "how much RAM for N concurrent games."

**Static-footprint math, updated for the view cache.** `sizeof(Game)` is
unchanged at 37,960 B (the kernel struct — untouched by this work).
`sizeof(GameSlot)` (the server's own wrapper, `foolish_server.c`) grew from
38,348 B to **46,608 B** (+8,260 B: `view_cache[8][1024]` = 8,192 B +
`view_cache_len[8]` + `view_cache_version[8]` = 64 B + `version` = 4 B).
`g_games[MAX_GAMES=256]`'s total static reservation grew from ~9.36 MiB to
**~11.38 MiB** (+~2.03 MiB, all demand-paged — only touched for games
actually created, since `h_create`'s `memset` dirties a fresh slot's pages
immediately, including its view cache, whether or not every seat ever
connects). Combined with the measured **~0.9 MB/connection** figure above,
the answer to "how much RAM for N thousand concurrent games": for N=1,000
games at this doc's own 4-seat shape (4,000 connections) —
`idle (2.7 MB) + N × sizeof(GameSlot) (1000 × 46.6 KB ≈ 46.6 MB) +
connections × ~0.9 MB (4,000 × 0.9 MB ≈ 3.6 GB)` ≈ **~3.65 GB**, overwhelmingly
dominated by thread-per-connection overhead, not game state. Getting to
1,000 real concurrent games on THIS server as shipped would also require
raising the compile-time caps (`MAX_GAMES=256`, `MAX_USERS=512`,
`TOKEN_HT_SIZE=1024`, `GAME_HT_SIZE=512` in `foolish_server.c`) and running
4,000 real OS threads on one box — both fine for a POC, both exactly the
kind of thing T1's own item 1 (thread-per-connection) already flagged as the
next architectural question for a real deployment, now with a concrete
memory number attached, not just a CPU one.

### Still open (honest list, toward "ready to swap off Supabase when we scale")

**Solid now:** the `/ws` hot loop's dominant per-message CPU cost
(`state_put`) is cut ~101x via correct, lock-protected version caching, with
zero wire/client changes; WS framing does roughly half the syscalls it used
to; every measured throughput/latency number moved the right direction at
both tested concurrency levels; `test.sh` (HTTP + WS smoke) and repeated
WS+legal stress runs (90-100% of submitted moves applied, matching or
beating the pre-existing bar) stay green throughout.

**Still open, in the order a real deployment would hit them:**

1. **The global mutex** (item 3 above) — analyzed, not implemented. It is
   now a clearly-second-place cost (11.04% of a smaller pie, flat in
   absolute terms) and the honest next target once someone can verify a
   sharded-lock change with ThreadSanitizer (unavailable on this box) or
   equivalent, not before.
2. **Thread-per-connection's memory tax** — ~0.9 MB/connection means
   thousands of concurrent players costs gigabytes in thread stacks alone,
   independent of game count. T1's own item 1 (a thread pool or epoll/kqueue
   event loop) was scoped to the CPU cost of `pthread_create`; this section
   adds the memory argument for the same fix.
3. **Compile-time caps** (`MAX_GAMES`, `MAX_USERS`, the two hash-table
   sizes) are POC-sized (256/512/1024/512) and would need raising — and
   re-validating their O(1) hash-table assumptions still hold — for
   thousand-game scale.
4. **Durability, backpressure, TLS, connection limits, rate limits** — all
   already flagged out of scope by the README at the time this section was
   written, unaffected by this (T1/T1b) work, and all real gaps for a
   production deployment: state was RAM-only (a crash lost every in-flight
   game) — since addressed by SQLite WAL write-behind persistence, see
   [`DURABILITY.md`](DURABILITY.md) — nothing here bounds how many
   connections or games a single process will accept, and there is no rate
   limiting on `/auth/signup` or `/create`.
5. **`h_state`'s unbounded `seat` query parameter** — noticed while scoping
   the view-cache change (not introduced by it, and not touched by it): a
   caller can pass `seat=-2`, which collides with `state_put`'s internal
   `VIEW_UNMASKED` sentinel and would serialize the FULL unmasked state
   (every hand, the deck) to an unauthenticated `/state` request. `/ws`
   already validates its `seat` against the caller's own owned seat before
   ever calling into `state_put`; `/state` does not. Out of this task's
   scope (a correctness/security fix, not a perf one) but worth a follow-up
   issue.

### Files (T1c additions)

- `foolish_hammer.c` — round-trip latency reservoir sampling + percentiles
  (Deliverable A).
- `mem_sample.sh` — the memory sampler (Deliverable B).
- `foolish_server.c` — `GameSlot.version`/`view_cache*` + `state_put_cached`
  (Deliverable D item 1); `version` bump sites in `h_meta`, `h_action`,
  `h_ws`, `bot_thread`.
- `ws.c` — buffered `ws_fill` (fewer `read()`s) + `writev_full`-coalesced
  unmasked `ws_send_frame` (Deliverable D item 2).
- `bench_results/T1c_ws_lines/` — the BEFORE hot-line capture
  (`annotated.txt`, `meta.txt`).
- `bench_results/T1d_ws_speedups/` — the AFTER capture, same scale/technique
  (`annotated.txt`, `meta.txt`). Raw `callgrind.out*` gitignored in both, same
  pattern as every other `bench_results/*/raw/`.

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

This file is CPU hot-path profiling only (callgrind/gprof, single-threaded
cost attribution). For the server's CONCURRENCY work — per-game locks
replacing the single global mutex, work-queue thread routing, the Helgrind
race-freedom gate, and before/after throughput/latency/memory at rising
connection counts — see [`SERVER_SCALING.md`](SERVER_SCALING.md) ("T2a").

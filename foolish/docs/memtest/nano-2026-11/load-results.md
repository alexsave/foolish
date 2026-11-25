# sat2 on the M1, loopback: throughput reproduction

Status: sat2 done (3 runs, fresh server and fresh DB for each).
The roundtrip matrix was stopped at the coordinator's request, and its partial data is listed at the end.
Client and server run on the same 8 cores of one M1, so the numbers include the cost of the 16 client processes competing with the server.
Throughput only: no cache numbers.

## Setup

- Worktree: /Users/alex/Dev/foolish-bench, commit 9c5057b64, not modified.
- Server: /Users/alex/Dev/foolish-bench/c_base/build/cbase serve, started per run by foolish/docs/memtest/nano-2026-11/scripts/sat2_run.sh with the env of /tmp/foolish-bench/start2.sh, plus CBASE_RATED_MIN_AGE=0, CBASE_MAX_CONNS=16384, CBASE_LISTEN_BACKLOG=4096, and every CBASE_LIMIT_* at 1000000.
  Fresh DB per run: /tmp/foolish-bench/sat2_r1.sqlite, sat2_r2.sqlite, sat2_r3.sqlite.
- Client: p3load built from c_base/bench/nano/p3load3.c and lib_client_host.c with the flags of load.sh build (print-human-caps from foolish/c), by foolish/docs/memtest/nano-2026-11/scripts/build_p3load.sh, binary foolish/docs/memtest/nano-2026-11/bin/p3load.
- Scenario (sat2, from load.sh run): 8 pairs (16 p3load processes), think 0-1 ms, P3DURATION_MS=25000, GAMES 100000, BOARDS 0, ready/go rendezvous files.
  Driver: foolish/docs/memtest/nano-2026-11/scripts/load_sat2.sh (load.sh with local paths).
- Load average gate: wait up to 5 min for 1-minute load at or below 3.
  Waits were 155 s, 95 s and 175 s before runs 1, 2 and 3.
- Server sampling: libproc cumulative CPU and RSS, once a second (foolish/docs/memtest/nano-2026-11/scripts/sampler.py).

## Results

Move numbers are the client's own counts from p3load (summ in load.sh): moves over the move window, which was 24.99 to 25.00 s.
Move latency is the client-measured move time.

| run | moves | moves/s | move p50 ms | move p99 ms | server CPU mean % | server CPU peak (1 s) % | server RSS max MB | load1 before / after |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| 1 | 720,044 | 28,808 | 0.08 | 0.47 | 50.7 | 77.9 | 40.6 | 2.75 / 9.82 |
| 2 | 662,022 | 26,483 | 0.08 | 0.74 | 48.9 | 90.2 | 40.4 | 2.99 / 14.28 |
| 3 | 664,920 | 26,613 | 0.08 | 0.30 | 44.8 | 90.0 | 39.5 | 2.96 / 10.45 |

- Median of the three: 26,613 moves/s.
  Spread: 26.5k to 28.8k (about 8 percent from low to high).
- Reference numbers: CBASE_SPEEDUPS.md gives 38.1k moves/s (fresh nano, default CPU knob) and 31.9k without it; GRAVITON.md gives about 24 to 27k at sat2 on the nano.
  This M1 run is at the GRAVITON range, below the CBASE_SPEEDUPS figure.
  It is not a like-for-like comparison: the nano is a different box, the M1 shares its cores with 16 clients, and the knob setting was the default on this server.
- Server CPU per move, from the sampler over the go-to-done window (28 s, which includes sign-up and game creation before the 25 s move window): 19.7, 20.7 and 19.0 us per move (run 1, 2, 3).
  The earlier report's 104 to 106 us per move was a different scenario (two processes, 40 games, full speed, p3client), so this number is not directly comparable to it.

## What the server looked like during the heaviest run

Heaviest = run 1 (highest moves/s and highest mean CPU).

- Server mean CPU over the window: 50.7 percent of one core; peak over any 1 s sample: 77.9 percent.
  The server is essentially one busy thread plus small helpers, and 6 threads in total (ps -M count, header included, at 12 s and at the end).
- RSS: 17.0 MB before the run, 41.6 MB at the end of the window (max 41.6 MB), so about 24.5 MB of growth over the 720,000 moves of that run (the sat2 game count was not recorded).
  After the server was stopped the RSS of the next run started at 17 MB again because each run has a fresh process.
- Load average 2.75 before run 1 and 9.82 right after it: Spotlight (mds_stores) jumped to 293 percent CPU during the run window, so the host was contended during run 1.
  The same spike was present after runs 2 and 3 (load 14.3 and 10.5), so run-to-run spread may partly be this background work.

## Notes and caveats

- The load gate only checks before a run, so the Spotlight burst was not caught in progress.
- CBASE_SPEEDUPS.md's 38.1k figure is for a fresh nano with the default CPU pinning; this Mac has no equivalent pinning, so the knob comparison cannot be made here.
- Wall time from the tool is whole seconds, so the 25 s windows are good to about 0.04 s.
- Raw: foolish/docs/memtest/nano-2026-11/raw/sat2/ (sat2_rN.out, sat2_rN.env, sat2_rN.server.tsv, sat2_rN.server.log, sat2_rN/ with t_go and t_done and per-pair files).
- The server from run 3 is still running (pid 533, http://127.0.0.1:8080, DB /tmp/foolish-bench/sat2_r3.sqlite, same env as above), so `sample 533` can be attached.

## Cpu mode (no network, in-process)

Already run earlier: foolish/docs/memtest/nano-2026-11/raw/cpu_inprocess.out (kernel_action 815 ns, jwt_verify 425 ns, bcrypt 57.8 ms sign-in only).
It is a per-operation model, not the running server.

## Roundtrip matrix (stopped, partial)

These runs happened before the coordinator asked to stop the roundtrip matrix, and they are not part of the sat2 comparison.

- Concurrency 1, 400,000 moves: 33 s wall (raw/rt_c1_r1.out), action p50 42 us, p95 62 us, p99 100 us; view p50 27 us; push_view p50 -8 us; 4,282 games; no errors.
- Concurrency 2, 800,000 moves: 50 s wall (raw/rt_c2.out), 16k moves/s, action p50 62 us, p95 94 us, p99 140 us; no errors.
- Concurrency 4, 1,100,000 moves: 51 s wall (raw/rt_c4_r1.out), 21.5k moves/s, action p50 74 us, p95 147 us, p99 362 us; no errors.
- Concurrency 4, second repeat: stopped with TaskStop at the coordinator's request; its partial output is in raw/rt_c4_r2.* and is not usable.
- Not run: concurrency 8, 16, 32 and the third repeat.

# foolish server: performance findings and handoff (2026-11-11)

Purpose: give this file to a Fable 5.2 agent to optimize the foolish server (c_base `cbase serve`) for the t4g.nano.
Every number below was measured on 2026-11-11 unless it says otherwise.
Raw data is under foolish/docs/memtest/nano-2026-11/ (paths at the end).

## CORRECTION (read first): "sat2" means two different tests

The Oct 20-21 rig defines sat2 as 192 two-player games (384 sockets), think 0-1 ms, 25 s (c_base/bench/analysis_2026-10-20/rig/final_rig.md:58).
The 2026-11-09 GRAVITON.md and today's runs use 8 pairs, 25 s.
The 38.1k, 31.9k and 27.3k figures belong to the 192-game shape (p99 12-18 ms, up to 43-61 ms in the Oct 20 matrix).
The 24.1k figures belong to the 8-pair shape (p99 0.5 ms).
They are not comparable, so the "gap" is mostly a difference in test shape, not a regression.
Also, p3load3.c computes think as `rng % (max-min)` in whole ms, so "0-1 ms" means 0 ms: the client never thinks.
In the 8-pair runs 53% of client time sits in gaps over 2 ms (about 21 ms each, about two per game, 120.7 moves per game), so that shape is dominated by game turnover and is not a saturation test of the move path.
Details and ranked hypotheses: foolish/docs/memtest/nano-2026-11/GAP_38K_VS_24K.md.
The 192-game shape WAS measured on a real nano later the same day (experiment E1, build 9c5057b64, 384 clients, 6 runs interleaved, raw data in grav/out/e1/, boxes up 18.7 min):

| knob | moves/s (3 runs) | mean | p50 | p99 | server CPU us/move |
|---|---|--:|--:|--:|--:|
| auto (shipped default: loop on CPU 0, others on CPU 1) | 40,532 / 37,715 / 37,930 | 38,726 | 4.28 ms | 12.9 ms | 36.4-37.4 |
| off (unpinned) | 32,849 / 33,293 / 33,156 | 33,099 | 5.17 ms | 15.4 ms | 36.3-36.7 |

This reproduces the old 38.1k (default) vs 31.9k (knob off) claim: auto is +17.0% over off, the p99 sits in the old 12-18 ms band.
The knob does not change the work per move (36.8 vs 36.5 us); it raises how busy the server is (about 142% vs 121% of one vCPU), so it is a scheduling win, not a CPU-per-move win.
So the REAL saturation baseline for the nano is 38.7k moves/s at 192 games, 36.8 us of server CPU per move, loop thread 17.9 us per move (auto), p99 12.9 ms.
The 8-pair 24.1k is a game-turnover-dominated shape, not a saturation test.
Note: SRVENV from the SSM shell does not reach the server (srv.sh starts it via systemd-run); the fixed copy is grav/pkg2/srv.sh with a 6th argument (auto or off).
Use `load.sh run LABEL IP 192 0 1 100000 25000` for the baseline and measure CPU per move with grav/pkg2/cpu.sh.
"Reproduces the 2026-11-09 run exactly" means the moves/s match, not the binary (that run was f99698319, today's is 9c5057b64).

## The target and the test

Production is a t4g.nano: Graviton2, Neoverse N1, 2 vCPU, 412 MiB RAM.
The benchmark is "sat2": 8 two-human pairs, think time 0-1 ms, 25 s, fresh server and fresh DB per run, client `p3load` built from c_base/bench/nano/p3load3.c.
The server runs the shipped defaults (loop thread pinned to CPU 0, every other thread to CPU 1).
The load box was a c7g.2xlarge, so the client never limits the server.
The recipe is in foolish/docs/memtest/GRAVITON.md and harness/grav (load.sh, srv.sh, run.sh).
A working copy of the harness (paths fixed, tag-checked teardown, build D only) is in foolish-bench-results/grav/.
Build under test: commit 9c5057b64, static musl aarch64 via c_base/providers/aws/ops/build_bootstrap.sh serve, 8,431,976 bytes.

## Throughput results

| where | moves/s | move p50 / p99 | note |
|---|--:|--:|---|
| real t4g.nano, 4 runs | 24,216 / 24,020 / 24,044 / 24,113 | 0.19 / 0.50 ms | very stable, RSS max 27.6 MiB, VmHWM 28.4 MB |
| M1 Mac, 3 runs | 28,808 / 26,483 / 26,613 | 0.08 / 0.3-0.7 ms | client and server share 8 cores, Spotlight noise |
| CBASE_SPEEDUPS.md:165 (2026-10-21) | 38.1k default, 31.9k other arm | 12.1 / 16.1 ms p99 | fresh nano, pinning confirmation |
| Graviton pass (same doc) | 27.3k | 18.4 ms p99 | MAIN build |
| GRAVITON.md 2026-11-09 | 24.2k, 24.1k, 24.2k | | same build D as today |

Today's 24.1k reproduces the 2026-11-09 run exactly.
It is 37% below the 38.1k, 24% below the 31.9k, 12% below 27.3k.
Open question, handed to an investigator: why is the nano at 24k and not 38k?
Server CPU per move on the M1 was about 19-21 us (whole 28 s window, includes sign-up).
On the nano perf counted about 81k cycles per move, an upper bound (the window includes idle lead-in and tail).
The p99 on the nano today (0.5 ms) is far below the 12-18 ms in CBASE_SPEEDUPS, so those two measurements were not the same shape of test.

## Graviton2 (Neoverse N1) cache sizes, from lscpu and sysfs

| level | size | note |
|---|--:|---|
| L1d | 64 KiB per core | 4-way, 256 sets |
| L1i | 64 KiB per core | |
| L2 | 1 MiB per core | |
| L3 | 32 MiB | shared, one instance |

Kernel 6.18.56 amzn2023 aarch64, THP never.

## Load latency per dependent load (calbench `mem`, pointer chase, median of 3 passes, ns)

| working set | ns | level |
|--:|--:|---|
| 4 KB | 1.60 | L1 |
| 64 KB | 1.67 | L1 (edge) |
| 128 KB | 5.41 | L2 |
| 256 KB | 5.70 | L2 |
| 512 KB | 6.36 | L2 |
| 768 KB | 11.16 | L2 pressure |
| 1 MB | 16.72 | L2 edge |
| 1.5 MB | 29.63 | L3 |
| 2 MB | 34.12 | L3 |
| 3 MB | 36.05 | L3 |
| 4 MB | 36.28 | L3 |
| 6 MB | 81.45 | L3 edge |
| 8 MB | 101.54 | RAM |
| 16 MB | 111.20 | RAM |
| 32 MB | 113.36 | RAM |
| 64 MB | 115.43 | RAM |
| 128 MB | 118.72 | RAM |

Reading: an L1 hit is about 1.6 ns (4 cycles at 2.5 GHz).
An L2 hit is about 5.5-6.4 ns, roughly 3.5x an L1 hit.
An L3 hit is about 30-36 ns, roughly 20x an L1 hit.
A RAM access is about 100-119 ns, roughly 70x an L1 hit.
So a miss out of L2 costs 5-6 times an L2 hit, and the working set matters most at the 1 MiB boundary.
Anything the loop thread touches per move that fits in 64 KiB costs almost nothing; staying under 1 MiB costs about 4x an L1 hit; leaving 1 MiB costs 30 ns or more per miss.

## Instruction costs (calbench `core`, ns per op)

| operation | ns |
|---|--:|
| dependent add | 0.40 (one cycle at 2.5 GHz, inferred) |
| 4 independent add chains, per add | 0.17 |
| dependent multiply | 1.60 |
| dependent float divide | 4.81 |
| 64-bit integer divide (dependent) | 8.41 |
| predictable branch | 0.81 |
| random 50% branch | 3.12 |
| uncontended atomic add | 7.62 |
| compare-and-swap | 8.77 |
| mutex lock plus unlock | 16.8 |

The contended-lock rows were unusable (means 50 us to 2 ms, p99 up to seconds), so there is no contended-lock figure.
The earlier 3.5 us contended-mutex number was not reproduced.

## memcpy and streaming (calbench `str` and `stream`)

| item | value |
|---|---|
| memcpy 12 B | 4.0 ns |
| memcpy 128 B | 5.5-7.1 ns |
| memcpy 1,024 B | 30 ns aligned, 44 ns misaligned |
| memcpy 1,500 B | 43 ns aligned, 63 ns misaligned |
| stream copy | 0.84-0.86 ns per element, about 19 GB/s |
| stream scale | 0.65-0.66 ns per element, about 24 GB/s |
| stream add and triad | 0.81-0.97 ns per element, about 25 GB/s |

## What could not be measured, and why

The nano does not expose instruction, L1D load, L1D miss or branch-miss counters; only `cycles` counts, and the L2D events read 0.
So there are no measured L1 or L2 hit rates for the server on real hardware.
The calbench contended rows, the clock group, the 256 MB point and the 15-minute pass gap were not done.
A cachegrind simulation of the nano's caches in Docker, and a hot-function and assembly profile with `sample`, were requested next (see the follow-up files in this folder if they exist).

## Cachegrind model of the nano (done; foolish/docs/memtest/nano-2026-11/cachesim/SUMMARY.md)

A model of the instruction stream (valgrind, glibc build of 9c5057b64, 2 pairs, per move values), not a hardware measurement.
Kernel time (sockets, epoll) is not counted by valgrind.
Per move, N1 model (L1 64 KiB 4-way, LL 1 MiB 8-way): 47,687 instructions, 11,461 data reads, 8,038 data writes.
D1 misses 81.6 read plus 60.4 write; LL misses 3.2 read plus 3.8 write; I1 misses 370; branches 7,171 with 588 mispredicted (8.2%).
M1 geometry: D1 misses drop about 3.5x but I1 misses stay at 366, so the instruction footprint is the main L1 miss source.
Hot spots: flush_txn (journal.c) leads D1 and LL read misses; memset and memcpy lead D1 write misses; sorted_known, memcpy, memset, run_replay_v6, strlen, pub_deliver, state_put_rules lead instructions.
About 23% of instructions and 26% of D1 read misses are in code valgrind could not name (stripped libc, sqlite, libssl).
About 63 integer divides and 8 atomics per move in the mapped code.
Time model: 13.8 to 24.8 us per move against 41.5 us measured, so a third to a half is user-space compute and cache; the rest is kernel and per-instruction cost above 0.4 ns.

## The 63 integer divides per move, named (script: cachesim/scripts/divs.py; data: callgrind play minus setup, 25,246 moves)

Total 62.7 executed udiv/sdiv per move over 34 sites, all in the game replay codec (replay.c) and the game rules (game.c).
They are amortised: the codec runs once per finished game over the whole game (about 120 moves), and the cost is divided by all moves.
That makes it a game-end cost (the finalize job), not a per-move-path cost.

| per move | where | what |
|--:|---|---|
| 9.14 | coder_code (decode) | one 64/32-bit `udiv` per 32-bit limb inside `bn_divmod` (replay.c:127), the rANS decoder dividing a big integer by the menu weight total M |
| 7.35 | coder_finish (encode) | the same `bn_divmod`, once per recorded choice, in reverse |
| 5.54 | coder_uniform | the same `bn_divmod` with d = n |
| 21.9 | subtotal, big-number division | each division walks the whole big integer (x->n limbs), so cost per choice grows with the game length: the whole game is ONE big number, which makes encode/decode quadratic in the number of choices |
| 13.12 | run_replay_v6 | `sdiv` with a `smull` after it, a divide by a variable (not a modulo), exact source line unknown (no line table for the inlined code) |
| 8.12 + 5.81 + 0.84 + 0.48 | run_replay_v6 | `sdiv` + `msub`, a `%` by a runtime value; the seat rotation `(cur + 1) % m->n` (replay.c:398-399 and 1409) is the likely source (inferred, m->n is 2 to 6 so it never constant folds) |
| 4.24 + 1.54 + 0.91 + 0.42 + 0.37 | atom_attack | the same seat rotation pattern, plus a divide |
| about 3 | draw_for, deal_rng_bounded, atom_pass, apply_pickup, refill*, handle_*, game.c | bounded random draws and modulo by small variable counts |

Facts: the three coder rows are `bn_divmod` (observed in the source); the seat-rotation attribution is inferred from the sdiv+msub pair and the source grep.
Candidate fixes (to test, not conclusions): replace `% m->n` by compare-and-wrap (`nx = cur + 1 == n ? 0 : cur + 1`), which removes about 35 divides per move; make the big-integer rANS linear instead of quadratic (state-of-the-art rANS keeps a 32-bit or 64-bit state and streams out bytes), or run the codec only when the game ends but off the loop thread; both change the frozen wire format, so only the first is safe without a format bump.
Cost: a 64-bit udiv on the N1 is about 8.4 ns (measured), so 63 divides is about 0.5 us per move, about 1.3% of the 41.5 us per move; this is a small prize.
The sdiv/msub pair is cheaper (32-bit) than the 64-bit figure, so the real prize is under 1.3%.

## PRIOR WORK (read before spending anything): the Oct 20-21 analysis already measured most of this

Today's calbench numbers (cache latencies, instruction costs, atomics, mutex, memcpy) reproduce the Oct 20 five-pass calibration of the same t4g.nano and c6g.large almost exactly (L1 1.6 ns, L2 5.5, L3 35, DRAM 113-115, udiv 8.8, atomic 7.6/8.8, mutex 16.8).
Do not re-measure them.
Where it lives (in the foolish repo history; the directory is not in the HEAD working tree, read it with `git show` at commit febfe0b76 or be9245255):
- c_base/bench/analysis_2026-10-20/NANO_MODEL.md: per-primitive costs of the nano (compute, caches, strings, malloc, syscalls, loopback and ENA TCP, fdatasync), what the nano shares with the c6g and where it differs (cross-vCPU wakes 2-6x, contended lines 25x, a 45 us cold start per thread wake-up).
- c_base/bench/analysis_2026-10-20/OPTIMIZED_DESIGN.md and move_path_verdicts.md (Fable's move-path verdicts and the inline/one-owner design).
- c_base/bench/analysis_2026-10-20/graviton/runs/prof2_hum2_NOW/ (cycles-sampled perf profile on a real nano: perf.txt, rec_self.txt, rec_children.txt, rec_dso.txt, bpf.txt) and graviton/tables.md; rig/final_rig.md and rig/results.txt (the 192-game sat2 numbers).
- c_base/bench/calib/ (calbench) with results_2026-10-20 (t4g, c6g) and results_2026-10-20_sqlite.
- foolish/CBASE_SPEEDUPS.md (the Graviton pass: server CPU per move on the nano 116 to 29.8 us at hum2, 107 to 45.1 at sat2, BASE to MAIN).
Findings from it that matter here:
- Kernel share at hum2 (prof2_hum2_NOW, 83.3 us of cycles per move): syscalls are 45.4% (37.7 us): sendto 26.6% (22.1 us, 11 us per send for the first send of a pass, 3.5-4 us per further socket), recvfrom 9.8% (8.1 us), epoll_pwait 9.9% (8.2 us).
- `perf stat` on the nano inflates task-clock: the hypervisor emulates the PMU, costing 8 us + 28.5 us per context switch of the measured task. The honest per-move CPU is cycles/2.5 GHz. Production runs without perf. Use `perf record` for sampling only and never trust task-clock under perf stat.
- At sat2 the Oct 20 note records 47 us of cycles per move (main 4aa1c22e3); today's measurement (build 9c5057b64, no perf attached) is 36.8 us per move, consistent with the Oct 21 speedups.
What is new today (not in the Oct 20 work, as far as the history shows): the 192-pair reproduction with server CPU per move captured by the harness, the cachegrind model of misses per function on the nano's cache sizes, the named divide sites, and the instruction-class breakdown below.

## Instruction-class breakdown per move (cachegrind and callgrind, play minus setup, 25,246 moves)

Total 47,687 instructions per move; 27,684 per move (58%) are in the server binary and can be classified; the other 20,000 are in libc, sqlite and openssl code valgrind could not name (memcpy, memset, strlen, malloc and so on, probably load and store heavy).
The classes below describe the server's own code only.

| class | per move | share of mapped |
|---|--:|--:|
| integer ALU (add, mov, cmp, sub, lsr, and...) | 11,384 | 41.1% |
| branches (including calls and returns) | 8,894 | 32.1% |
| loads (single) | 3,458 | 12.5% |
| stores (single) | 1,599 | 5.8% |
| store pairs (stp) | 954 | 3.4% |
| load pairs (ldp) | 759 | 2.7% |
| multiplies (mul, madd, msub) | 519 | 1.9% |
| divides | 63 | 0.2% |
| SIMD | 47 | 0.2% |
| atomics | 8 | ~0% |

Top instructions per move: bl 4,464; add 2,953; mov 2,785; cmp 2,374; ldr 2,202; b.ne 1,156; str 977; stp 954; ldrb 849; ldp 759; strb 533; sub 511; cbz 509; lsr 391; b.eq 388; tbz 343; cbnz 338; and 333; ret 307.
Whole-stream totals from cachegrind: 11,461 data reads, 8,038 data writes, 7,171 conditional and indirect branches of which 588 mispredicted.
Observations: about 4,460 calls per move is one call per 6 mapped instructions (one per 10 of all instructions), which drives the stp/ldp register saves and the returns; many small non-inlined functions on the move path is a lever (inlining, merging hot call chains).
There is almost no SIMD and few multiplies.
The classifier (cachesim/scripts/iclass.py) files bic and bfi under branches, so the branch share is overstated by about 5%.

## QUEUED first task for the Fable agent: where does the rest of the 36.8 us per move go at sat2?

The cachegrind model covers only 13.8 to 24.8 us of the measured 36.8 us per move (192-game shape) or 41.5 us (8-pair shape).
Valgrind does not count kernel time, so the missing share is probably syscalls, sockets and epoll (the hum2 split is above), plus per-instruction costs above 0.4 ns.
The hum2 kernel profile already exists (PRIOR WORK above); what is missing is the same split at the SATURATION shape (sat2, 192 games), where sends batch differently, and per thread (loop thread on CPU 0 vs the rest on CPU 1).
The nano exposes only the `cycles` PMU event, but `perf record -e cycles` (or `-F 999 -g`) works and attributes cycles by function, user and kernel; sample only, never read perf stat counters (inflated).
Plan (one real-nano trip, about $0.15, same harness as E1: grav/out/e1/e1.sh, grav/pkg2/):
1. Run the 192-pair baseline (`load.sh run LABEL IP 192 0 1 100000 25000`, default pinning) with `perf record -a -g -F 999` on the server box for the 25 s window (dnf install perf, as before; perf 6.1.190 worked).
2. Report cycles by symbol split into kernel and user, by thread (loop thread on CPU 0 vs the rest on CPU 1), and the top 30 symbols of each, with the kernel call graph for the top syscalls (epoll_wait, recv, send, futex, write/pwrite, fdatasync).
3. Count syscalls per move with `strace -c -f -p PID` (or `perf trace -s`) for a shorter window (strace slows the server; use it only for the counts).
4. Compare syscalls per move and kernel cycles per move with the io_uring numbers in CBASE_SPEEDUPS.md (6.4 syscalls per move with epoll, 4.8 with io_uring).
Then combine with the cachegrind hot spots (flush_txn, memset/memcpy, I1 misses 370 per move) and the divides (about 1.3%) to decide the order of optimizations.
Always tear down with grav/down.sh and verify zero leftovers; production i-REDACTED01 is never touched.

## Ideas for the optimizer (to check, not conclusions)

1. Find the per-move working set of the loop thread (game state, journal buffers, connection structs, output buffers).
   Keep the hot part under 64 KiB; keep everything the loop touches per move under 1 MiB.
2. Look for pointer chasing on the move path, since each dependent miss out of L2 is 30 ns and out of L3 is 100 ns.
3. Look for atomics and mutexes on the move path: an uncontended atomic is 7.6 ns, which is 20 dependent adds.
4. Look for divides on the move path (8.4 ns each for 64-bit integers).
5. The first move of the optimizer should be to explain the 24k versus 38.1k gap, because it may be one flag or one build difference.
6. Every change needs the sat2 A/B on a real nano (the harness in foolish-bench-results/grav does that for under $0.20 a trip) with at least 3 runs per arm.

## Files

| what | path |
|---|---|
| this file | foolish/docs/memtest/nano-2026-11/FABLE_HANDOFF.md |
| M1 sat2 report | foolish/docs/memtest/nano-2026-11/load-results.md |
| M1 raw runs and scripts | foolish/docs/memtest/nano-2026-11/raw/sat2/ and scripts/ |
| Graviton harness copy | foolish/docs/memtest/nano-2026-11/grav/ |
| Graviton sat2 raw | foolish/docs/memtest/nano-2026-11/grav/out/raw/srv/ and out/raw/load/ |
| Graviton calbench TSVs and summary | foolish/docs/memtest/nano-2026-11/grav/out/cal/ and out/cal_summary.txt |
| perf attempt | foolish/docs/memtest/nano-2026-11/grav/out/raw/srv/perf_p1_D.txt |
| teardown proof | foolish/docs/memtest/nano-2026-11/grav/out/teardown.txt |
| the server binary that ran | foolish/docs/memtest/nano-2026-11/grav/bin/cbase_D |
| older Graviton data (2026-10-20) | /Users/alex/Dev/cbase/bench/calib/results_2026-10-20 |
| the 38.1k claim | /Users/alex/Dev/foolish/foolish/CBASE_SPEEDUPS.md line 165 |
| the test recipe | /Users/alex/Dev/foolish/foolish/docs/memtest/GRAVITON.md |

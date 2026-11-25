# Nano optimization campaign: orchestrator notes

Started 2026-11-14 by the Fable orchestrator.
Source of truth for raw findings is ../FABLE_HANDOFF.md; this file holds the campaign's own notes and decisions.
Baseline under test: foolish main 9c5057b64, 192 two-player games, 38.7k moves/s, 36.8 us of server CPU per move.

## Observations from the E1 raw data before any new trip

Per-thread CPU per move in the three auto runs (grav/out/e1/steps.log): loop 17.9 to 20.2 us, an unnamed cbase thread on CPU 1 at 9.6 to 10.5 us, job-0 4.2 to 5.0 us, req-0 2.2 to 2.7 us.
The loop thread used 18.1 to 19.1 s of the 25 s window, so by tick accounting CPU 0 was only 73 to 76 percent busy with the loop.
The box was 75 to 79 percent busy across both vCPUs, and the process accounts for 138 to 148 percent of one vCPU.
So neither vCPU looks saturated by the server's own threads, while the move p50 is 4.3 ms with zero client think time.
Open question for trip 1: what bounds throughput (softirq on CPU 0, a lock shared with the CPU 1 threads, the valve, hypervisor descheduling, or the load box itself).
The load box's CPU was never measured in the 192-game shape, so "the client never limits the server" is unproven there.
About half of the server's CPU per move is off the loop thread (the unnamed thread is probably the journal flusher writing game_moves rows to SQLite, and job-0 is the finalize job with the replay codec).

## AWS trips

| trip | purpose | box time | cost estimate | teardown proof |
|---|---|---|---|---|
| 1 | perf profile of the 192-game baseline (7 runs) | 17.6 min (14:11:53Z to 14:29:38Z, 2026-11-14) | about $0.10 | trip1/out/teardown.txt, re-checked by the orchestrator: 0 instances, volumes, SGs, roles, profiles, buckets |
| 2 | A/B matrix: base, PR 496, PR 497, both, neoverse-n1 build, two interrupt layouts, 21 runs plus 2 human-pace runs | 34.3 min (15:48:42Z to 16:22:59Z, 2026-11-14) | about $0.17 | trip2/out/teardown.txt: 0 instances, volumes, SGs, roles, profiles, buckets; production running |
| 3 | final matrix on merged code: pre-496, main with 496, with 496+497, current main, PR 500; syscall counts; perf of main; 18 runs | 30.6 min (18:29:38Z to 19:00:15Z, 2026-11-14) | about $0.15 | trip3/out/teardown.txt, re-checked by the orchestrator: 0 instances, volumes, SGs, roles, profiles, buckets; production running |

## Code map leads at 9c5057b64 (Sonnet investigator, read-only, unverified until the profile confirms them)

The flusher writes one `UPDATE games SET move_list = move_list || chunk` per waiting move (rows_sql.c:652-657, statement at rows_sql.c:171), each rewriting the game's whole blob, while the chunks are already contiguous in the game's list (journal.c:1479-1488).
The flusher holds `ov_mu` and the writer role during `snapshot_take` (journal.c:1639-1646), so the loop blocks on the mutex or gets WOULD_BLOCK and the move is re-run on req-0 after the loop already built products and casts.
Under epoll the actor's socket is sent inside `ws_frames` (server.c:712-715), not at pass end, so a move costs about two sends.
`cb_thread_cpu_us` (a real syscall) is read twice per move from the third inline run of a pass onward (server.c:496-498, 510).
Each move is its own journal batch: 13 B of header, two CRC calls, two `j->mu` pairs, one `ov_mu` pair and five atomic stats updates (journal.c:1166-1170).
The build passes no `-mcpu`, so atomics are not LSE; the Makefile already accepts `CPU=neoverse-n1` (c_base/Makefile:72-76), and LTO was measured at no gain in October.
`coder_finish` calls `bn_divmod` with divisor 1 for every uniform choice and still walks every limb (replay.c:255-273); skipping it does not change the output bytes.
The seat rotation `%` sites (game.c:195-197, replay.c:398-399, 1409) run at round transitions and in the job-0 replay, not once per applied move.
Per applied move the loop zeroes or copies about 15 KB in small fixed structs (cb_http_req 1.2 KB, job 0.7 KB, cb_user 0.47 KB twice, NwFrame 1.1 KB per publish, cb_fanout 1.3 KB, the section free 3 to 5 KB).
Canaries an optimization PR must pass: rep_head_copies.sh, unit_json_gate, unit_num_gate, unit_name_gate, compiler-gate, the coverage floors, and -Werror -Wshadow -Wvla.

## Trip 1 status (2026-11-14): blocked before launch, nothing created, $0 spent

`aws sts get-caller-identity` fails with "Your session has expired. Please reauthenticate using 'aws login'" (executor twice, orchestrator once).
No AWS resource was created, grav/state.env is unchanged (cmp against campaign/trip1/state.env.before), and no trip of the 6 is used.
Ready for the trip: the unstripped twin of the baseline (campaign/trip1/pkg/cbase_D.unstripped, same BuildID fc51bf0e..., stripped rebuild reproduces sha256 4eae8437...b7), the driver (campaign/trip1/scripts/trip1.sh), the launch script (scripts/up_trip1.sh) and the box-side scripts (pkg/prof.sh, trip.bt, cpu.sh, load.sh), all dry-checked in an arm64 amazonlinux:2023 container.
Untested on the real nano: the bpftrace program on kernel 6.18 (strace fallback exists) and the ENA interrupt name pattern.
Run order after `aws login`: up_trip1.sh, then trip1.sh check smoke base1, then perfcy proc:perfcy perfclk bpf1 base2, then irq1 irq0 if under 28 box minutes, then proc:perfclk push sync, then grav/down.sh and the leftover checks.

## Trip 1 results (2026-11-14, details in trip1/out/TRIP1_RESULTS.md, numbers re-checked by the orchestrator against trip1/out/analysis/)

The loop thread is not CPU-bound: CPU 0 is idle 20.5 percent of play, and the loop sleeps 13.6 percent of the window in futex on `ov_mu` (cb_journal_game_acquire, journal.c:1812).
The holder is the flusher (inferred from source lines and samples, no lock-hold trace): every flush transaction walks all 1024 buckets three times and, because ended games stay resident 60 s and the count stays over max_games 4096, mallocs and qsorts every idle game in ov_evict, all under `ov_mu`.
The server log shows 4235 flush transactions for 1.0M moves in one run, so the full walk and sort run about 150 times a second.
Flusher cost is 8.40 us per move of cycles: about 3.8 under `ov_mu` (ov_evict and snapshot_take, the qsort alone 2.41), SQLite 2.95, list_write only 0.22.
Cycles per move (perf, 2.5 GHz): total 37.3, loop 18.6 (user 8.0, kernel 10.7), flusher 8.4, job-0 4.35, req-0 2.17, softirq in the idle task 2.4.
Loop inclusive: sendto 5.70 (1.84 calls per move), h_action 7.89 (cb_section_commit 5.43, of which fan-out publish 2.63, journal commit 0.97, products 0.59; game acquire 1.01), recvfrom 1.47, thread-CPU clock 0.85 to 1.04 (1.97 clock_gettime syscalls per move), epoll 0.56.
Throughput follows where the two ENA queue interrupts land, and irqbalance moves them between runs: 45.5k moves/s with both on CPU 1 (single run), 40.1k at 19 percent on CPU 0, 37.5k at 50 percent, 34.6k with both on CPU 0 (single run).
So the 38.7k baseline has an uncontrolled plus or minus 10 percent from interrupt placement, and every A/B from now on must pin the interrupts and stop irqbalance.
The load box does not limit the test: at most 42.7 percent busy, and it drove 45.5k moves/s.
The valve never tripped, and only 0.16 percent of requests went to the pool, so the WOULD_BLOCK re-run lead is dead.
Atomics (0.11 us) and crc32c (0.035 us) are noise; the codec's coder_* functions are 0.23 us per move, so the rANS format bump is not worth proposing on CPU grounds.

## Ranked plan (posted to the dispatcher 2026-11-14)

1. Journal: the flusher's lock hold becomes O(waiting games) and O(evicted games) instead of O(resident games log n) per transaction (intrusive lists for the waiting set and the residency order).
2. Ops: the ENA interrupts pinned to CPU 1 and irqbalance off on the nano, pending a human-pace latency check.
3. Loop: the thread-CPU clock read once per pass instead of twice per move.
4. Loop: the publish path (2.63 us per move), starting with views that are built and then dropped for sockets in the game's room.
5. Build: CPU=neoverse-n1 as a flag-only A/B arm.
6. job-0 finalize (4.35 us per move, off the bound): look only after 1 to 5.
Dropped: seat-rotation modulo (not on the per-move path), the rANS rewrite (0.23 us per move), send batching (1.84 sends per move is one per hearing socket), io_uring.

## Work log

2026-11-14: item 1 is PR 496 (branch journal-flush-lists, head 5d942b0a7, worktree /Users/alex/Dev/foolish-wt-journal-flush-lists), opened and not merged, nano A/B pending.
The orchestrator read the journal.c diff: the waiting list and the touch-ordered residency list replace every per-transaction bucket walk and the qsort, the rules are unchanged, and the ratings table got the same treatment.
Executor's local lanes: make test 72 units green, smoke green, test-asan green, smoke-asan green; a Mac micro-measurement of one flush call went from 118 us to 1.3 us with 4096 residents at the cap.
Pre-existing reds found on main, not from this campaign: `make coverage` fails locally on 8 never-called functions in core/src/net, and the GitHub coverage workflow fails in server-client-ts with ENOENT on .v8cov.
2026-11-14: item 3 started as branch inline-pass-budget-wall-clock (worktree /Users/alex/Dev/foolish-wt-inline-pass-budget).
The first idea for item 3 (the loop's CPU once per pass) was already measured and rejected in the code's own comment (40 thousand requests a run to the pool), so the design is: a run quick by the wall clock is charged its wall time with no syscall, and only a suspect run turns the CPU clock on for the next one.

## Trip 2 results (2026-11-14; per-run data in trip2/out/steps.log and trip2/out/analysis/pushlat.txt, tables re-derived by the orchestrator with trip2/orch/steps_table.py)

All runs had irqbalance stopped and the interrupts pinned; "split" is one ENA queue interrupt per vCPU (48 to 51 percent on CPU 0 measured), "cpu1" is both on CPU 1 (0 percent on CPU 0 measured).
Means of 3 interleaved runs, play window, 192 games.

| arm | moves/s | p50 ms | p99 ms | slowest move ms | CPU us/move | loop | flusher | pushes over 100 ms per run | to pool |
|---|--:|--:|--:|---|--:|--:|--:|---|--:|
| A main, split | 37,086 (36.0k to 37.7k) | 4.53 | 12.62 | 89 to 109 | 37.59 | 20.25 | 10.27 | 4, 8, 9 | 0.15% |
| B PR 496, split | 50,280 (49.4k to 51.0k) | 3.33 | 6.68 | 78 to 99 | 29.85 | 19.12 | 3.86 | 5, 11, 6 | 0.19% |
| C PR 497, split | 38,385 (37.3k to 39.2k) | 4.41 | 12.36 | 100 to 109 | 36.68 | 19.39 | 10.23 | 3, 2, 10 | 0.18% |
| E both PRs, split | 49,502 (48.5k to 50.6k) | 3.41 | 6.88 | 76 to 89 | 30.34 | 19.38 | 3.96 | 4, 6, 1 | 0.21% |
| N main built with CPU=neoverse-n1, split | 38,574 (38.4k to 38.8k) | 4.42 | 12.09 | 87 to 112 | 36.49 | 19.41 | 10.02 | 7, 6, 11 | 0.16% |
| F main, cpu1 | 47,304 (45.0k to 49.6k) | 3.20 | 9.93 | 286 to 431 | 34.20 | 15.51 | 9.50 | 15, 12, 16 | 0.22% |
| G both PRs, cpu1 | 60,178 (59.7k to 61.1k) | 2.23 | 6.57 | 249 to 286 | 28.91 | 14.85 | 4.95 | 15, 18, 24 | 0.28% |

VmHWM was 29.9 to 31.5 MB in every run, and the valve never tripped.
PR 496 meets the acceptance rule with non-overlapping runs: +35.6 percent moves/s, p99 halved, CPU per move down 20.6 percent, pushes and RSS not worse; with it the loop's vCPU goes from 21 percent idle to 2 percent idle, so the loop is now the bound.
PR 497 is neutral within the spread: +3.5 percent moves/s and -2.4 percent CPU per move against main with overlapping runs, and -1.5 percent moves/s and +1.6 percent CPU per move against PR 496 alone; it meets "not worse", and its gain is not proven.
The neoverse-n1 build is a small consistent gain: +4.0 percent moves/s and -2.9 percent CPU per move, runs not overlapping, p99 flat.
moves/s counts every answered move, about 3.8 percent of them refused in every arm.

## DECISION FOR ALEX: where the ENA interrupts go on the nano (a provisioning change, not server code)

Fact: with both queue interrupts on CPU 1 and irqbalance off, main serves 47.3k moves/s against 37.1k with one interrupt per vCPU (+27.6 percent), p99 9.9 against 12.6 ms, CPU per move 34.2 against 37.6 us; with both PRs it is 60.2k against 49.5k (+21.6 percent).
Fact: the tail gets worse in the same runs: the slowest move is 249 to 431 ms against 76 to 109 ms, and push receipts over 100 ms are 12 to 24 per run against 1 to 11, so this layout fails the "push receipts over 100 ms not worse" rule at saturation.
Fact: with both interrupts on CPU 1 that vCPU is 5 to 8 percent idle at saturation (it carries the flusher, the job and request workers and all network softirq), so the bound moves from the loop's vCPU to CPU 1.
Fact, single runs only: at human pace (192 pairs, think 2 to 4 s) move p50 is 0.21 ms in both layouts, p99 0.39 against 0.31 ms, the slowest move 4.5 against 2.7 ms, and server CPU per move 56.1 against 53.3 us.
Fact: today production leaves the interrupts to irqbalance, which moved them between runs in trip 1, so production throughput at saturation varies by about plus or minus 10 percent from boot to boot.
Inference: the human-pace cost is the cross-vCPU wake the October design predicted (the request's interrupt lands on the vCPU the loop is not on), and it is small in absolute terms (under 0.1 ms at p99).
Options: (a) leave it to irqbalance (today); (b) pin one queue per vCPU and stop irqbalance (deterministic, today's average); (c) both on CPU 1 (more throughput and a lower p99 at saturation, a worse extreme tail at saturation, slightly slower and dearer at human pace).
Orchestrator's view: production is human pace, where (c) is slightly worse and the saturation gain is not needed, so (c) is not an obvious win; (b) costs nothing and removes the boot-to-boot variance; a 3-run human-pace A/B would be needed before choosing (c).
No change has been made; this needs Alex's decision.

## After trip 2

The trip 2 executor's own tables (trip2/out/TRIP2_RESULTS.md) match the orchestrator's parse of steps.log on every arm.
Caveat on the neoverse-n1 build: its runs do not overlap main's, but the +4.0 percent is smaller than main's own run-to-run range (1,618 moves/s), so it is a plausible small gain, not a proven one.
Caveat on PR 497: the nano has shown its outcome only (flat within the spread), not its mechanism; the clock_gettime count per move is to be measured in the last trip.
2026-11-14: Alex merged PR 496 (rebase); main is 77fef4bef.
Budget revised by Alex through the dispatcher: at most 2 more trips (4 of 6), planned as one, and no trip before the item 4 PR is ready.
The single remaining trip carries: the clock_gettime count for PR 497 before and after, the item 4 A/B, and merged main against the pre-496 baseline, all with interrupts split and pinned.
Item 4 started as branch views-only-when-heard (worktree /Users/alex/Dev/foolish-wt-views-only-when-heard): rt_publish_view costs 1.40 us per move on the loop and its frames are dropped for every connection in the game's room, which is every player in the benchmark.
Added by Alex through the dispatcher (2026-11-14): measure the per-move cost of the list upkeep PR 496 put on the loop (the residency touch with its walk-back, the waiting-list insert and remove, the ratings list) before deciding anything.
Method: a Mac micro-benchmark of the acquire and commit path with a realistic mix, merged main against pre-496, cachegrind if cheap, and one perf cycles run of merged main in the single trip attributed by source line.
Only if the upkeep is above about 0.3 us per move or a visible symbol: a new PR with the waiting list as a dense array with swap-remove and the residency order as a CLOCK ring, stating which eviction behaviour changes, the seeded-model test adapted and mutation checked, no allocation under ov_mu on the hot path, no format change.
Orchestrator's estimate before measuring (from the code, not a measurement): 0.02 to 0.05 us per move, because a touch is about six pointer writes to lines of recently active games and the other two lists change per game, not per move.
2026-11-14 later: Alex said "merge everything"; the dispatcher rebase-merged PRs 497 and 498, so main is 59c115054 (77fef4bef is main with 496 only, 05cf2501a is 496 plus 497).
PRODUCTION GUARD: pushes to main deploy live when the GitHub gate `static exports / foolish` is green; it is red from the pre-existing structgen break, which no agent of this campaign may fix without asking the dispatcher.
Priority before the trip: a test of a spectator's /v1/wait waking on a move when nobody is in the game's room (branch spectator-wait-wakes-on-a-move, worktree /Users/alex/Dev/foolish-wt-spectator-wait), with a fix PR if it shows a bug; the same executor then runs the Mac micro-benchmark of the list upkeep.
Trip 3 arms, since everything is merged: 9c5057b64 (pre-496, grav/bin/cbase_D), 77fef4bef (trip2/bin/cbase_P1 is the same tree), 05cf2501a, 59c115054, 3 interleaved runs each with interrupts split and pinned, plus a clock_gettime count on 77fef4bef and 05cf2501a and one perf cycles run of 59c115054.

## Spectator long poll (2026-11-14)

A spectator's /v1/wait parks on the game id, and the only push keyed by the game is the spectators' cast, which is not built when no socket is in the room (since e88ba0e5b, 2026-10-20), so the poll was answered only at its 25 s deadline.
The new smoke test is red on 05cf2501a (25,319 ms) and on 59c115054 (25,325 ms), so the bug is older than PR 498, and green with the fix (1 ms).
Fix: PR 500 (branch spectator-wait-wakes-on-a-move, head 11b34d054, opened, not merged): when the cast is not built the fan-out still wakes the game's key (cb_rt_wake, a PUB_WAKE in the pushes' order), and the cast stays unbuilt.
Lanes on the fix: make test 633 tests, smoke 6 programs, test-asan 633 tests, all green; two mutation checks.

## List upkeep of PR 496, measured on the Mac (campaign/listbench/results.txt; M1, loaded machine, so timings are noisy)

Acquire plus commit plus release per move: 289.6 ns with the lists (59c115054) against 274.2 ns before PR 496 (9c5057b64), median of 7 interleaved rounds; the difference per round ran from 0.8 to 28.8 ns, median about 19 ns.
The residency relink alone is about 14 ns per move (A against A with the relink removed), and ov_touch in a tight loop is about 4.3 ns per touch.
Counts that do not depend on load: exactly 2 touches per move, 1.72 walk-back steps per touch with 1 touch in 8 stamped later, about 8 pointer stores per relink of which 4 go to other games' lines, and 1 wait_begin plus 1 wait_end per 75 moves.
Estimate for the nano (not a measurement): 0.02 us per move if the neighbours are in L1 or L2, 0.03 to 0.08 typical, about 0.2 in a constructed worst case of 5 L3 hits per move.
Decision: below the 0.3 us threshold on every reading, so the lists stay and no array or CLOCK-ring PR is built; the trip's perf sample of main is to confirm it (look for ov_touch, journal.c 459-485, called from cb_journal_game_acquire and ov_apply).
The ratings list was not in the bench; it changes only at rated game ends and seat rating reads.


## Trip 3 results (2026-11-14; trip3/out/TRIP3_RESULTS.md, arm table re-derived by the orchestrator from trip3/out/steps.log)

All runs: irqbalance stopped, one ENA queue interrupt per vCPU (48.5 to 50.4 percent on CPU 0), 192 games, play window, 3 interleaved runs per arm.

| arm | moves/s | p50 ms | p99 ms | CPU us/move | loop | flusher | pushes over 100 ms per run |
|---|--:|--:|--:|--:|--:|--:|---|
| D 9c5057b64, before the campaign | 37,040 (36.4k to 37.7k) | 4.65 | 12.87 | 37.64 | 20.33 | 10.21 | 5, 10, 11 |
| M1 77fef4bef, with PR 496 | 48,586 (47.0k to 50.6k) | 3.48 | 6.92 | 30.85 | 19.76 | 3.99 | 7, 10, 10 |
| M2 05cf2501a, with 496 and 497 | 50,451 (49.9k to 50.9k) | 3.26 | 6.72 | 29.63 | 18.67 | 3.90 | 8, 7, 14 |
| M3 59c115054, with 496, 497 and 498 | 50,802 (49.9k to 52.2k) | 3.28 | 6.88 | 29.94 | 18.79 | 4.03 | 8, 13, 9 |
| M4 11b34d054, M3 plus PR 500 | 51,249 (50.3k to 52.4k) | 3.22 | 6.56 | 29.48 | 18.42 | 3.94 | 12, 6, 6 |

Everything merged against the start (M3 against D): +37.2 percent moves/s, p99 12.87 to 6.88 ms, CPU per move 37.64 to 29.94 us (-20.4 percent), RSS flat at about 30.7 MB, pushes over 100 ms 8.7 to 10.0 per run with 37 percent more pushes, valve trips 0.
PR 496 is the only change whose gain is outside the run-to-run spread (+31.2 percent here, +35.6 percent in trip 2).
PR 497: its mechanism is confirmed (clock_gettime on the loop 1.954 to 0.0022 per move, loop syscalls 5.10 to 3.15 per move, the two inline timing functions 1.02 to 0.10 us per move in the profile); its throughput and CPU effect (+3.8 percent, -3.9 percent) is in the expected direction and inside the spread.
PR 498 is flat (+0.7 percent moves/s, +1.1 percent CPU per move, inside the spread): rt_publish_view fell from 1.40 to 0.01 us per move, but the question that replaced it, local_user_heard, costs 0.80, and rt_publish_cast reads 1.57 against 1.08, so the fan-out went from 2.63 to 2.43 us per move in all.
PR 500 is neutral (+0.9 percent moves/s, inside the spread).
The loop's time asleep in futex fell from 4.45 us per move (trip 1) to 0.58 to 0.70, and the loop's vCPU is 2 to 6 percent idle against 20 to 22 percent before.
Cycles per move on M3 (perf, 2.5 GHz): all threads 30.53 (trip 1: 37.34), loop 17.47 (18.63), flusher 3.68 (8.40, of which SQLite 3.16 now), job-0 4.32, req-0 1.99.

## List upkeep on the nano (Alex's question, threshold 0.3 us per move)

From the perf sample of M3 attributed by source line: 0.045 us per move on the loop thread (0.26 percent of the loop), 0.006 on req-0, 0.051 over all threads; about 47 samples, so roughly plus or minus 15 percent.
Most of it is the relink in ov_touch (0.022 in lru_out, 0.012 in the in-place test, 0.007 in the walk-back loop); the ratings list is 0.002 and the waiting list drew no samples.
The Mac micro-benchmark (under load) gave about 0.02 us per move.
Verdict: below the threshold by a factor of about 6, so the lists stay and no array or CLOCK-ring PR was built.

## What is left on the loop thread (M3, inclusive us per move, facts)

sendto 5.72 (1.84 calls per move, 1.38 of it softirq taken inside the call), cb_section_commit 5.06, softirq outside any syscall 2.26, cb_fanout_publish 1.62, conn_io with recvfrom 1.61, cb_journal_game_acquire 1.10, j_game_commit 0.99, pub_deliver 0.99, local_user_heard 0.80.
Candidates not built: make local_user_heard cheap (0.80 us per move for a yes or no; it walks the user's connections with string compares and a lookup per connection); cb_journal_game_acquire's ov_slot lookup (0.34) and take_game copy (0.59); the flusher's SQLite work (3.16 us per move off the loop); job-0's finalize (4.32 us per move off the loop).
Main moved during trip 3 to c38ff8d94 (deploy tooling and the cbase submodule pin only, no server sources); trip 3 measured 59c115054.

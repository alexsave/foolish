# Why sat2 on the nano is 24.1k today and 38.1k in the #425 note

Read-only investigation, no AWS calls, no code or doc changes, nothing committed.

## Bottom line

The 38.1k and today's 24.1k were not measured with the same load, so the gap is mostly a test-shape difference, not a server regression or a knob difference.
Today's "sat2" is 8 two-human pairs (16 clients) with an effective think time of 0 ms, and the 38.1k figure is almost certainly from the older "sat2" of 192 two-player games (384 clients), which is a different measurement.
The p99 gap (0.5 ms today against 12-18 ms in the notes) is explained by the same shape difference, and the knob cannot explain the gap because today's run has the knob on, as the logs show.
The 38.1k run itself has no raw file, no commit, and no recorded harness in any repo or scratch folder I could read, so it cannot be reproduced from the record; the ranking below is inferred from the surrounding evidence.

## Where things are (path corrections)

CBASE_SPEEDUPS.md is at /Users/alex/Dev/foolish/CBASE_SPEEDUPS.md, not /Users/alex/Dev/foolish/foolish/CBASE_SPEEDUPS.md.
Line 165 of that file is the #425 claim: "A confirmation on a fresh nano with the shipped default gave 38.1k against 31.9k moves/s and a p99 of 12.1 against 16.1 ms at sat2."
GRAVITON.md is at /Users/alex/Dev/foolish/foolish/docs/memtest/GRAVITON.md, and it exists as described.

## What the 38.1k test was: what is known

The claim was added in commit 4f9fef79d (2026-10-21 18:33 -0400, docs only), and the doc header says its numbers "come from the PR bodies, commit messages and session notes, not from new runs".
No raw output, env file, load log, or driver script for 38.1k or 31.9k exists under /Users/alex/Dev/foolish, foolish/docs/memtest/nano-2026-11, /Users/alex/Dev/cbase, or the Claude scratch folders; grep for 38.1, 38,1xx, 31.9 and 31,9xx found only the docs and the handoff notes.
git log -S"38.1k" and -S"31.9k" return only 4f9fef79d; git log --grep 425 returns only the same docs commit.
The knob that the claim is about is commit 0325ea4e2 (2026-10-21 15:06 -0400, "the CPU knob is auto by default; off unpins"), with 7a72fbe2f (the knob itself) and a7bab4e1a (its smoke test) the same day.
Before 0325ea4e2 the knob was off, and DESIGN.md at that commit says "production leaves it off" and "the nano rig runs off".
The rig's entry.sh (c_base/bench/nano/entry.sh, added in 0325ea4e2) forces CBASE_LOOP_CPU and CBASE_BG_CPU to off whenever the binary knows auto, unless XENV pins them.
So the 38.1k run is "auto" only if it did not go through that rig path, and that is not recorded.
Unknown for the 38.1k and 31.9k runs: the load client binary (p3load3 or p3load2-linux), the pairs or games count, the think setting, the duration, the boards, the credit mode, the server binary and its build (libc, flags), the box count, the launch method, and whether the 31.9k arm was the same build or a different one.

## What "sat2" meant when the numbers were taken

The Oct 20 analysis (commit be9245255, c_base/bench/analysis_2026-10-20/graviton/tables.md and rig/final_rig.md) defines sat2 as 192 two-player games (384 sockets), think 0-1 ms, 25 s, on a t4g.nano with unlimited credits.
In that shape NOW (4aa1c22e3, no knob) gave a median of 23.4k moves/s with move p99 of 43-51 ms and 384 clients.
The Oct 21 Graviton pass figures (MAIN 27.3k at p99 18.4 ms, BASE 11.6k at p99 61.8 ms) fit the same 192-game shape, and so do the earlier "sat2 p99 fell from 56 to 20 ms" figures.
The Nov 9 memtest (GRAVITON.md) and today's runs define sat2 as 8 pairs (16 clients), think 0-1 ms, 25 s, and they define hum2 as 192 pairs at 2-4 s think, which is the old sat2 under a different name.
The M1 run (foolish/docs/memtest/nano-2026-11/load-results.md) is also the 8-pair shape, and it reports p99 0.3-0.7 ms.

## What today's run actually used (observed)

Build: 9c5057b64, static-pie musl aarch64, 8,431,976 bytes, from build_bootstrap.sh serve (grav/out/build_D.log); foolish/docs/memtest/nano-2026-11/grav/bin/cbase_D has sha256 4eae8437...b7 (the same sha256 as the dist file in build_D.log).
Server env (grav/pkg/srv.sh): CBASE_STORE=sqlite, CBASE_RATED_MIN_AGE=0, CBASE_MAX_CONNS=16384, CBASE_LISTEN_BACKLOG=4096, all CBASE_LIMIT_* raised, CBASE_BOT_SPEEDUP=1, CBASE_REQ_WORKERS=1 and CBASE_BOT_WORKERS=1 (defaults), CBASE_LOOP_CPU and CBASE_BG_CPU unset (auto), CBASE_IO_URING=0, CBASE_CAST_DELTA=1.
Pinning: all four server logs say "serve: pinned: the loop to CPU 0, every other thread to CPU 1 (auto)", and the mid-run status file has Cpus_allowed_list 0 for the main thread.
Load: load.sh run s1_D <ip> 8 0 1 100000 25000 (8 pairs, THINK_MIN 0, THINK_MAX 1, games 100000, 25 s) from a c7g.2xlarge, with the p3load3.c client.
Server box: t4g.nano launched with CpuCredits=unlimited (grav/up.sh).
Kernel: AL2023 kernel 6.18 (the same AMI parameter as Nov 9).
Results: 605,436 moves in the 25 s window for s1 (24,216/s), with move p50 0.19 ms and p99 0.51 ms.
The other three runs were 24,020, 24,044 and 24,113 moves/s.

## Environment comparison with the 38.1k run

Build and libc: today's binary is static musl from build_bootstrap.sh; the Oct 21 binary build is not recorded, but the rig's binary name (cbase-serve-linux-arm64) is the output name of the same script, so static musl is the likely build for both.
Compile flags: c_base/Makefile CB_CFLAGS (-std=gnu11 -O2 -g -Wall -Wextra -Werror) did not change between 0325ea4e2 and 9c5057b64, and build_bootstrap.sh changed only the output file name in that window.
Knob: today's runs have the knob on and pinned (observed); the 38.1k run had the knob on (claimed) and the 31.9k run had it off (claimed), so the knob explains at most the 38.1k-to-31.9k difference, about 19%.
Credit mode: today's box is unlimited; the Nov 9 boxes were unlimited too, and the Oct 20 README says its boxes were unlimited; the Oct 21 mode is not recorded.
Under standard credits the Nov 9 run G7 collapsed to about 2.3k moves/s after a 40 s burst, so throttling could only lower a number, not explain a higher one like 38.1k.
Client: p3load3.c is byte-identical between 0325ea4e2 and 9c5057b64 (git diff shows no change), and the grav tarball copy could not be compared automatically, so this is inferred from git.
Box count and launch: the Oct 21 pass says 58 runs on ten boxes, while the Nov 9 and today runs used one box per build; the box-to-box spread on Oct 21 is not recorded.

## The think time is effectively zero (observed in code)

c_base/bench/nano/p3load3.c (lines 209-210) computes the think time as think_min + (span > 0 ? rng_u32 % span : 0), with span = think_max - think_min, in whole milliseconds.
With "0 1" the span is 1, so the draw is always 0, and every client moves as soon as it can.
This line dates from commit 25b2ba887 (2026-10-19), so every run in the window had it, in both shapes.
So "think 0-1 ms" in GRAVITON.md, FABLE_HANDOFF.md and CBASE_SPEEDUPS.md describes a think time the client never takes.

## What the 8-pair run measures (observed from today's event files)

I read the per-client event files of s1_D (rdv*/ev.*, 'M' lines with send and reply times).
Across 605,436 moves, a move's round trip (send to reply) has p50 0.186 ms and mean 0.213 ms.
The gap from a reply to the next send has p50 0.047 ms but mean 0.447 ms, and 10,176 gaps are over 2 ms, with mean 21 ms.
Those long gaps are 53% of client time, while the move round trips are 32% of client time.
There were 5,018 distinct games, with 120.7 moves per game on average, so the run is dominated by game turnover and waiting, not by back-to-back moves.
The p3load summary also counts refused moves at 2,544 per client (about 6.6% of 38,311), which is a client-side retry cost in the same loop.
The M1 run is consistent with this: 26.6k median at 8 pairs with server CPU averaging 50% of one core, so the 8-pair shape does not saturate even a faster machine.
The conclusion is that the 8-pair "sat2" is a closed-loop, latency and lifecycle test, and its moves/s says little about the server's per-move cost.

## Build differences and code between the builds (inferred)

Nov 9 builds A (400a65d09), B, C and D (f99698319) all contain the knob (0325ea4e2), and all ran the 8-pair shape at 24.0k, 24.4k, 24.0k and 24.2k.
Today's 9c5057b64 gives 24.1k, so the Nov 9 to Nov 11 code (23 files, about 979 insertions in c_base core, providers and shared/c, plus the kernel changes) did not move the 8-pair number.
The binary sha for "build D" in GRAVITON.md (d8acf501...) is not the sha of today's cbase_D (4eae8437...), even though both are 8,431,976 bytes; the "reproduces exactly" claim in FABLE_HANDOFF.md is a match of the moves/s numbers, not of the binary.
Between the knob commit and build A there are move-path changes that could plausibly cost CPU per move at the 192-game shape: 52ce64c74 (repetition draw, per-move position check), 61050fd22, 63000f53c, dbfe03416, 5d79ab800 (the session move list and the replay path), 007048957, 1d73bc44f, 37d500c3c and c467f9293 (bot memory on the table and the state blob), b4bf57586 and 9a010866f (metrics registry), and 04b357f2b (cb_lanes).
None of these was measured on the nano in CPU per move, because no Nov run recorded server CPU per move (the sampler reads RSS only, and the perf capture is one cycles-only run).
Between 9c5057b64 and build A, the changes that matter are the listener and drain code (cb964b4c0, 67cde5316 and 9ffa0cf7f, which are not on the move path), the roster format (621029910 and 1bd53b950), and the auth and store work, none of which is on the per-move path.

## Hypotheses, ranked

1. Test shape (most likely; inferred from strong evidence).
   For: the p99 values only fit the 192-game shape (43-61 ms in Oct 20, 18.4 and 12.1 ms in Oct 21) and the 8-pair shape gives 0.3-0.7 ms in both the M1 and the nano; the sat2 name was used for 192 games in the Oct 20 analysis and for 8 pairs from Nov 9 on; the event files show the 8-pair run is turnover-dominated.
   Against: nothing in the repo proves the 38.1k run used 192 pairs, so this is the best fit, not a confirmed match.
2. Box-to-box or era variance on fresh nanos (plausible; unmeasured on Oct 21).
   For: the Oct 21 pre-knob MAIN (27.3k) and the knob-off arm (31.9k) differ by 17%, and the Nov 9 same-shape runs agree within about 2%, so the spread on Oct 21 boxes is unknown but could be that large.
   Against: the Nov 9 spread is small, and the four Nov 9 and today runs agree closely.
3. A real server regression between 0325ea4e2 and today at the 192-game shape (unproven; candidates listed above).
   For: the window contains per-move changes, and the Oct 21 CPU per move (45 us at MAIN, pre-knob) was never repeated.
   Against: the 8-pair shape cannot see per-move cost because it is turnover-dominated, and no run in the window measured the 192-game shape on the nano after Oct 20.
4. The knob (explains part of 38.1k against 31.9k, but not today's gap).
   For: the knob was +12 to 18% in six paired runs and the 38.1k against 31.9k difference is +19%.
   Against: today's run is pinned, as the logs show, so the knob is not missing today.
5. Libc or flags (low).
   Against: both are the same build script and flags; the rig's binary name matches build_bootstrap.sh output, so static musl is likely in both.
6. Credit mode (low).
   Against: throttling lowers throughput; a higher 38.1k cannot come from it, and today's box is unlimited.
7. Client binary or version (low, unknown).
   Against: p3load3.c is unchanged across the window, but the 38.1k client binary is not recorded, and botg.sh uses p3load2-linux, so an older or different client is possible.
8. The zero-think bug (affects interpretation, not the gap).
   It applies to both shapes, so it does not explain 24.1k against 38.1k, but it means the 8-pair number is not a saturation number.

## Experiments, in the order to run them (none run)

E0, free, local: already done for today's four runs (the event-file decomposition above); repeat it for s2 to s4 and record the gap split in the handoff.
E1, one nano trip (about $0.20 to $0.30 with the grav harness): today's binary cbase_D (9c5057b64) at the 192-pair shape, which is "load.sh run LABEL $IP 192 0 1 100000 25000" (with the same srv.sh start and stop), three runs with the knob auto and three with SRVENV="CBASE_LOOP_CPU=off CBASE_BG_CPU=off", interleaved.
Expected if hypothesis 1 holds: auto runs of about 30 to 38k with p99 in the tens of ms, and an auto-over-off gain of 12 to 19%.
Expected if hypothesis 2 or 3 holds: about 23 to 27k at 192 pairs, which would mean the 38.1k cannot be reproduced on today's build.
E2, one nano trip (about $0.30, including a build on the c7g load box, not the Mac, so Docker CPU is not shared): build 0325ea4e2 (the knob-era commit, the closest to the 38.1k build) and 284f0dc4f (pre-knob MAIN) with build_bootstrap.sh serve, then run each at the 192-pair shape, three runs each, interleaved with cbase_D.
Expected: 0325ea4e2 with auto near 31 to 38k reproduces the claim in-tree; 284f0dc4f near 27k reproduces the pre-knob arm.
If 0325ea4e2 is far below 9c5057b64 at the same shape, hypothesis 3 is real.
E3, only if E2 shows a gap of 20% or more between 0325ea4e2 and 9c5057b64: bisect the window with the 192-pair shape and per-thread CPU per move, starting from 52ce64c74, 61050fd22 and b4bf57586.
E4, no AWS cost until the next trip: add per-thread utime and stime from /proc/PID/task/*/stat to the sampler in grav/pkg/srv.sh, so every run reports CPU microseconds per move for the loop and the other threads without perf.
This is the number missing from every Nov run.
E5, a question for Alex, no AWS: whether the 38.1k run went through the rig (entry.sh, which forces off) or a direct cbase.service-style launch, and whether it used p3load3 with 192 pairs; the answer decides whether E1 or E2 comes first.
E6, proposal only, needs explicit approval because it calls AWS: the CloudWatch CPUCreditBalance and CPUSurplus metrics for the Oct 21 nano instances, if the instance IDs can be found, to rule the credit mode in or out.

## Corrections to the existing notes

FABLE_HANDOFF.md says the sat2 target is "the server's saturation throughput" and that "the load box never limits the server"; at the 8-pair shape neither claim holds, because the run is turnover-dominated and the server's CPU share was not measured.
GRAVITON.md and FABLE_HANDOFF.md describe think time as "0-1 ms"; the client takes 0 ms in every run.
GRAVITON.md's sat2 is not the sat2 of CBASE_SPEEDUPS.md; the two should not be compared without this note.
The "reproduces the 2026-11-09 run exactly" claim in FABLE_HANDOFF.md is a match of moves/s and not of the binary sha.

## Observed versus inferred

Observed: the file paths; the 38.1k and 31.9k text and its commit (4f9fef79d); that no raw exists for them; the knob commit and its date (0325ea4e2); the pinned log lines in today's runs; the think-time code and its span arithmetic; the event-file numbers (move round trip p50 0.186 ms, 53% of client time in gaps over 2 ms, 120.7 moves per game); the p99 values listed; the Nov 9 and today 8-pair throughput values; the build shas and sizes.
Inferred: that the 38.1k run used the 192-game shape (the best fit, not proven); that the Oct 21 runs used p3load3 with the zero-think bug; that the gap is shape and not code or box; that the Nov 9 to today window has no sat2 effect at the 8-pair shape; that the static musl build is the same in both.

# Trip 2 results: A/B of PR 496, PR 497, both, and CPU=neoverse-n1, with the ENA interrupts pinned

Real t4g.nano server (kernel 6.18.62-127.179), c7g.2xlarge load box, us-east-1, 2026-11-14.
Every run: fresh server and DB (srv.sh start/stop), knob auto (loop on CPU 0, every other thread on CPU 1), `load.sh run LABEL IP 192 0 1 100000 25000`, irqbalance stopped and the ENA interrupts pinned by pkg/irq.sh before the run.
Box time: run-instances 15:48:42Z (server) and 15:48:44Z (load), both terminated by 16:22:59Z (grav/down.sh returned), about 34.3 minutes.
Teardown proof: out/teardown.txt (zero leftovers, production i-REDACTED01 running).
All paths below are relative to foolish/docs/memtest/nano-2026-11/campaign/trip2/.

## Builds

All from the `serve` recipe (scripts/build.sh, a copy of trip1/scripts/build_twin.sh), alpine:3.24 pinned image, gcc 15.2.0, `--cpus=2`, stripped.
Logs: out/builds/build_NAME.log and make_NAME.log; sha256 in out/builds/sha256.txt.

| binary | tree | sha256 |
|---|---|---|
| cbase_D | 9c5057b64 (grav/bin/cbase_D, not rebuilt) | 4eae8437fe5ba915682b752ecfd71cd9a40b77051807d186ad4a593ea96da0b7 |
| cbase_P1 | PR 496, journal-flush-lists 5d942b0a7 | 9ca3b791ebcfc586b2696bff031f8b7ac1edd9c4de8b5840f1e93677033bebbf |
| cbase_P3 | PR 497, inline-pass-budget-wall-clock f18f5d981 | 2388945aa5e9f99bf057ed77d252fcb4d0a33673b7adfa5a6d2bfc4fed5dc497 |
| cbase_ALL | 9c5057b64 + `git merge --no-ff` of both branches (local throwaway worktree, merged clean, removed) | 53e1cd472fa1d9945c8fd876634dea7cd5203ebf186240891123792ff9ed422b |
| cbase_N1 | 9c5057b64 with `make ... STATIC=1 CPU=neoverse-n1 build/cbase` | 808af8ceccdbe2eefc5b3133c770258edd802a089e365063290f9acce29b68a9 |

Toolchain check: the same recipe on a clean 9c5057b64 worktree reproduced cbase_D's sha256 exactly (out/builds/build_Dcheck.log), so the arms differ only by source or flag, not by a moved package.
N1 flag check (out/builds/n1_check.txt): all 143 compile lines carry -mcpu=neoverse-n1; objdump of the unstripped binaries shows 111 outlined `__aarch64_*` call sites against 192 in the baseline and 90 inline LSE instructions against 16; the remaining outlined calls are all in the prebuilt static OpenSSL, while cbase functions (write_batch, flush_txn, cb_journal_game_acquire, stat_rate_add, pm_malloc) call the helpers in the baseline and none in N1.

## Interrupt layout

pkg/irq.sh split: ens5-Tx-Rx-0 on CPU 1, ens5-Tx-Rx-1 on CPU 0, ena-mgmnt on CPU 1 (trip 1's base2 layout).
pkg/irq.sh cpu1: both queues and ena-mgmnt on CPU 1.
Fact: before and after every run irq.sh printed `irqbalance=inactive procs=0` and the effective affinity asked for (IRQ_OK in every pre and post block of out/steps.log).
Fact: /proc/interrupts snapshots around every run (raw/srv/irq_LABEL_start.txt and _end.txt, `irq.sh pct`) give 47.3 to 50.7 percent of queue interrupts on CPU 0 in every split run and 0.0 percent in every cpu1 run.
Fact: chronyd was active on the server at check (System time 0.000005 s off NTP), no package was installed on the server (perf and bpftrace "not installed"), and dmesg had no OOM line.

## Method

Same as trip 1: the play window is [t_go+2 s, t_go+23 s] on the load box clocks, moves counted by t_reply_us from the ev.* M lines (load.sh playwin), server CPU from cpu.sh report (per-thread /proc ticks) and report2 (schedstat on-CPU and run-queue wait) over that window.
Loop = the main thread (tid = pid, CPU 0); flusher = the unnamed thread on CPU 1 with the most CPU (the checkpointer is the other unnamed one, about 0.35 us/move).
"runq wait" is the sum over all threads of schedstat run-queue wait per move.
Push latency (scripts/pushlat.py on raw/load/LABEL.tgz): for each applied move (an M line not preceded by an X refusal line for the same game and version), the time from the mover's t_send_us to the first R line in the OTHER player's ev file of the same game with a version at least the move's resulting version.
Both clients run on the load box and stamp CLOCK_MONOTONIC, so no cross-box clock is involved; no receipt was earlier than its send (push_neg=0) and R versions never went down (r_nonmono=0) in any run; 3 to 15 applied moves per run had no later receipt (push_missing, game ends).
The moves/s count includes refused moves (about 4 percent), exactly as trip 1 counted.
Tables: scripts/analyze.py reads out/steps.log and out/raw, writes out/analysis/runs.tsv, table_runs.md, table_arms.md, table_cmp.md and pushlat.txt.

## Per-run table

Every run, in run order; wA is the throwaway warm-up and is not counted in any arm.
Throughput runs: think 0-1 ms, 25 s; hum_* runs: think 2-4 s, 45 s, window [t_go+5, t_go+43].
Server log, every run: valve tripped 0 times, 0 over budget; err_files 0 in every run.

| run | arm | build | irq | moves/s | p50 ms | p99 ms | CPU us/move | loop | flusher | job-0 | req-0 | runq wait us/move | VmHWM kB | RSS max kB | err_files | queue IRQs on CPU 0 % | load busy % (busiest vCPU) | to pool | over budget | valve | journal records / flushes / WAL frames | push n | push p50 / p99 ms | push > 100 ms |
|---|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| wA (warm-up, not counted) | A | D | split | 38,016.1 | 4.48 | 12.22 | 36.91 | 19.81 | 10.03 | 4.41 | 2.31 | 14.81 | 30,364 | 30,064 | 0 | 47.8 | 38.6 (39.5) | 1461 | 0 | 0 | 947175 / 6174 / 152710 | 767679 | 6.80 / 15.12 | 3 |
| r1A | A | D | split | 37,652.3 | 4.43 | 12.18 | 37.05 | 20.07 | 9.94 | 4.38 | 2.31 | 14.82 | 30,664 | 30,520 | 0 | 48.9 | 38.8 (40.0) | 1572 | 0 | 0 | 937698 / 6159 / 150856 | 760200 | 6.93 / 15.17 | 4 |
| r1B | B | P1 | split | 50,429.1 | 3.32 | 6.77 | 29.80 | 19.06 | 3.85 | 4.31 | 2.20 | 12.02 | 30,756 | 30,672 | 0 | 49.3 | 50.4 (52.8) | 2505 | 0 | 0 | 1246683 / 8476 / 200068 | 1018490 | 5.27 / 8.50 | 5 |
| r1C | C | P3 | split | 39,245.5 | 4.36 | 11.92 | 36.04 | 18.89 | 10.08 | 4.39 | 2.33 | 15.18 | 30,660 | 30,700 | 0 | 48.6 | 40.2 (40.9) | 1749 | 0 | 0 | 979574 / 6337 / 157024 | 792707 | 6.52 / 14.60 | 3 |
| r1E | E | ALL | split | 50,588.8 | 3.31 | 6.70 | 29.78 | 18.96 | 3.90 | 4.31 | 2.24 | 12.19 | 29,852 | 29,676 | 0 | 49.1 | 49.5 (52.3) | 2689 | 0 | 0 | 1247782 / 8490 / 200878 | 1021454 | 5.24 / 8.37 | 4 |
| r1F | F | D | cpu1 | 49,567.3 | 2.75 | 9.18 | 33.04 | 14.91 | 8.87 | 5.57 | 3.27 | 21.82 | 31,484 | 31,264 | 0 | 0.0 | 49.0 (50.5) | 2366 | 0 | 0 | 1227481 / 4388 / 178901 | 998179 | 4.22 / 10.87 | 15 |
| r1G | G | ALL | cpu1 | 59,674.5 | 2.55 | 6.90 | 29.44 | 15.39 | 5.04 | 5.43 | 3.12 | 18.93 | 31,320 | 31,096 | 0 | 0.0 | 60.6 (61.4) | 4033 | 0 | 0 | 1472014 / 6950 / 224192 | 1203444 | 3.99 / 8.21 | 15 |
| r1N | N | N1 | split | 38,521.5 | 4.44 | 12.18 | 36.45 | 19.42 | 9.97 | 4.39 | 2.32 | 15.03 | 30,800 | 30,624 | 0 | 48.7 | 39.4 (39.9) | 1722 | 0 | 0 | 961475 / 6218 / 154366 | 777901 | 6.68 / 14.85 | 7 |
| r2A | A | D | split | 36,034.8 | 4.66 | 13.36 | 38.52 | 20.81 | 10.59 | 4.41 | 2.39 | 14.52 | 30,708 | 30,564 | 0 | 50.7 | 36.2 (36.8) | 1403 | 0 | 0 | 899900 / 5954 / 145965 | 728023 | 7.14 / 16.68 | 8 |
| r2B | B | P1 | split | 51,003.3 | 3.26 | 6.52 | 29.74 | 18.81 | 3.92 | 4.36 | 2.26 | 12.46 | 30,868 | 30,676 | 0 | 48.5 | 51.7 (52.2) | 2453 | 0 | 0 | 1257711 / 8431 / 202860 | 1030296 | 5.19 / 8.29 | 11 |
| r2C | C | P3 | split | 38,593.8 | 4.38 | 12.27 | 36.48 | 19.37 | 10.08 | 4.39 | 2.31 | 14.92 | 30,680 | 30,508 | 0 | 49.5 | 39.8 (40.8) | 1956 | 0 | 0 | 962573 / 6180 / 154332 | 779369 | 6.68 / 15.15 | 2 |
| r2E | E | ALL | split | 49,418.5 | 3.40 | 7.08 | 30.53 | 19.33 | 4.02 | 4.41 | 2.38 | 12.64 | 30,864 | 30,676 | 0 | 47.8 | 50.5 (51.4) | 2724 | 0 | 0 | 1222879 / 8222 / 196823 | 998217 | 5.35 / 9.09 | 6 |
| r2F | F | D | cpu1 | 45,029.0 | 3.67 | 10.80 | 35.32 | 16.07 | 10.17 | 5.57 | 3.13 | 21.85 | 30,996 | 30,828 | 0 | 0.0 | 46.2 (47.1) | 2040 | 0 | 0 | 1126079 / 5005 / 171711 | 908470 | 5.30 / 12.60 | 12 |
| r2G | G | ALL | cpu1 | 61,103.0 | 2.03 | 6.28 | 28.46 | 14.39 | 4.89 | 5.39 | 3.32 | 17.74 | 31,416 | 31,260 | 0 | 0.0 | 60.8 (62.2) | 3188 | 0 | 0 | 1505200 / 5311 / 217757 | 1228843 | 3.27 / 7.60 | 18 |
| r2N | N | N1 | split | 38,835.0 | 4.38 | 11.82 | 36.54 | 19.27 | 10.06 | 4.49 | 2.41 | 15.31 | 30,844 | 30,640 | 0 | 47.3 | 39.0 (39.5) | 1532 | 0 | 0 | 967687 / 6206 / 155069 | 783957 | 6.66 / 14.48 | 6 |
| r3A | A | D | split | 37,569.8 | 4.51 | 12.33 | 37.20 | 19.86 | 10.27 | 4.42 | 2.32 | 14.93 | 30,460 | 30,364 | 0 | 48.2 | 37.4 (38.3) | 1438 | 0 | 0 | 937511 / 6141 / 151339 | 759146 | 6.89 / 14.98 | 9 |
| r3B | B | P1 | split | 49,408.7 | 3.40 | 6.75 | 30.02 | 19.50 | 3.81 | 4.24 | 2.15 | 11.67 | 30,860 | 30,652 | 0 | 49.7 | 50.9 (51.6) | 2332 | 0 | 0 | 1219229 / 8389 / 196346 | 997781 | 5.38 / 8.46 | 6 |
| r3C | C | P3 | split | 37,314.9 | 4.49 | 12.88 | 37.53 | 19.90 | 10.54 | 4.42 | 2.35 | 15.11 | 30,680 | 30,584 | 0 | 49.6 | 37.9 (39.1) | 1610 | 0 | 0 | 924835 / 5981 / 148644 | 753820 | 6.88 / 16.12 | 10 |
| r3E | E | ALL | split | 48,499.9 | 3.53 | 6.87 | 30.71 | 19.86 | 3.96 | 4.29 | 2.25 | 12.17 | 30,668 | 30,472 | 0 | 50.7 | 49.9 (51.7) | 2463 | 0 | 0 | 1198593 / 8167 / 192962 | 979552 | 5.47 / 8.65 | 1 |
| r3F | F | D | cpu1 | 47,316.5 | 3.19 | 9.81 | 34.25 | 15.56 | 9.45 | 5.58 | 3.23 | 22.16 | 31,208 | 31,000 | 0 | 0.0 | 47.7 (48.4) | 2614 | 0 | 0 | 1183303 / 4546 / 175805 | 954061 | 4.76 / 11.79 | 16 |
| r3G | G | ALL | cpu1 | 59,756.3 | 2.10 | 6.52 | 28.84 | 14.77 | 4.93 | 5.35 | 3.33 | 18.18 | 31,412 | 31,088 | 0 | 0.0 | 59.0 (61.5) | 3269 | 0 | 0 | 1467706 / 5146 / 210830 | 1202712 | 3.36 / 7.97 | 24 |
| r3N | N | N1 | split | 38,364.5 | 4.45 | 12.26 | 36.47 | 19.54 | 10.04 | 4.35 | 2.24 | 14.77 | 30,812 | 30,620 | 0 | 49.5 | 39.6 (41.1) | 1603 | 0 | 0 | 961465 / 6259 / 154995 | 774486 | 6.65 / 15.06 | 11 |
| hum_split | hum_split | D | split | 98.8 | 0.21 | 0.31 | 53.26 | 55.93 | n/a | n/a | n/a | 5.62 | 11,280 | 11,452 | 0 | 50.2 | 0.8 (1.6) | 0 | 0 | 0 | 4796 / 3 / 4663 | 3755 | 0.21 / 0.32 | 0 |
| hum_cpu1 | hum_cpu1 | D | cpu1 | 98.5 | 0.21 | 0.39 | 56.10 | 53.43 | n/a | n/a | n/a | 13.53 | 11,152 | 11,380 | 0 | 0.0 | 0.8 (1.4) | 0 | 0 | 0 | 4791 / 3 / 4619 | 3743 | 0.22 / 0.41 | 0 |

Tail and push latency per run (out/analysis/pushlat.txt, scripts/pushlat.py, same play window):

| run | move p95 ms | move max ms | push p50 ms | push p99 ms | push max ms | push > 100 ms | applied moves without a later receipt |
|---|--:|--:|--:|--:|--:|--:|--:|
| wA | 9.75 | 87.86 | 6.80 | 15.12 | 686.63 | 3 | 6 |
| r1A | 9.72 | 89.27 | 6.93 | 15.17 | 635.65 | 4 | 7 |
| r1B | 5.81 | 77.91 | 5.27 | 8.50 | 577.30 | 5 | 7 |
| r1C | 9.50 | 99.67 | 6.52 | 14.60 | 841.43 | 3 | 3 |
| r1E | 5.73 | 75.79 | 5.24 | 8.37 | 567.99 | 4 | 7 |
| r1F | 6.75 | 430.57 | 4.22 | 10.87 | 662.98 | 15 | 3 |
| r1G | 4.38 | 249.42 | 3.99 | 8.21 | 524.17 | 15 | 8 |
| r1N | 9.57 | 86.49 | 6.68 | 14.85 | 998.85 | 7 | 6 |
| r2A | 10.40 | 108.66 | 7.14 | 16.68 | 706.00 | 8 | 5 |
| r2B | 5.66 | 99.14 | 5.19 | 8.29 | 534.34 | 11 | 4 |
| r2C | 9.66 | 106.81 | 6.68 | 15.15 | 640.87 | 2 | 8 |
| r2E | 5.98 | 88.70 | 5.35 | 9.09 | 538.61 | 6 | 9 |
| r2F | 8.18 | 286.37 | 5.30 | 12.60 | 670.81 | 12 | 5 |
| r2G | 3.97 | 285.66 | 3.27 | 7.60 | 360.03 | 18 | 7 |
| r2N | 9.43 | 92.33 | 6.66 | 14.48 | 665.88 | 6 | 3 |
| r3A | 9.86 | 89.98 | 6.89 | 14.98 | 1020.73 | 9 | 7 |
| r3B | 5.93 | 79.33 | 5.38 | 8.46 | 637.56 | 6 | 7 |
| r3C | 10.15 | 108.57 | 6.88 | 16.12 | 898.27 | 10 | 7 |
| r3E | 6.19 | 77.44 | 5.47 | 8.65 | 389.62 | 1 | 5 |
| r3F | 7.44 | 346.85 | 4.76 | 11.79 | 722.52 | 16 | 4 |
| r3G | 4.16 | 273.37 | 3.36 | 7.97 | 589.45 | 24 | 15 |
| r3N | 9.64 | 112.43 | 6.65 | 15.06 | 949.00 | 11 | 12 |
| hum_split | 0.28 | 2.68 | 0.21 | 0.32 | 2.69 | 0 | 0 |
| hum_cpu1 | 0.29 | 4.45 | 0.22 | 0.41 | 4.46 | 0 | 0 |

## Per-arm table

Mean, then the three runs in round order (r1, r2, r3); source out/analysis/table_arms.md.
CPU us/move is the whole process from /proc ticks; loop and flusher are per-thread ticks (schedstat on-CPU agrees closely, see the sched lines in out/steps.log).

| arm | n | moves/s mean (values) | p50 ms mean (values) | p99 ms mean (values) | CPU us/move mean (values) | loop us/move mean (values) | flusher us/move mean (values) | RSS max kB mean (values) | push > 100 ms mean (values) | to pool mean (values) | valve trips mean (values) |
|---|--:|---|---|---|---|---|---|---|---|---|---|
| A (D / split) | 3 | 37,086 (37,652, 36,035, 37,570) | 4.53 (4.43, 4.66, 4.51) | 12.62 (12.18, 13.36, 12.33) | 37.59 (37.05, 38.52, 37.20) | 20.25 (20.07, 20.81, 19.86) | 10.27 (9.94, 10.59, 10.27) | 30,483 (30,520, 30,564, 30,364) | 7 (4, 8, 9) | 1,471 (1,572, 1,403, 1,438) | 0 (0, 0, 0) |
| B (P1 / split) | 3 | 50,280 (50,429, 51,003, 49,409) | 3.33 (3.32, 3.26, 3.40) | 6.68 (6.77, 6.52, 6.75) | 29.85 (29.80, 29.74, 30.02) | 19.12 (19.06, 18.81, 19.50) | 3.86 (3.85, 3.92, 3.81) | 30,667 (30,672, 30,676, 30,652) | 7 (5, 11, 6) | 2,430 (2,505, 2,453, 2,332) | 0 (0, 0, 0) |
| C (P3 / split) | 3 | 38,385 (39,246, 38,594, 37,315) | 4.41 (4.36, 4.38, 4.49) | 12.36 (11.92, 12.27, 12.88) | 36.68 (36.04, 36.48, 37.53) | 19.39 (18.89, 19.37, 19.90) | 10.23 (10.08, 10.08, 10.54) | 30,597 (30,700, 30,508, 30,584) | 5 (3, 2, 10) | 1,772 (1,749, 1,956, 1,610) | 0 (0, 0, 0) |
| E (ALL / split) | 3 | 49,502 (50,589, 49,418, 48,500) | 3.41 (3.31, 3.40, 3.53) | 6.88 (6.70, 7.08, 6.87) | 30.34 (29.78, 30.53, 30.71) | 19.38 (18.96, 19.33, 19.86) | 3.96 (3.90, 4.02, 3.96) | 30,275 (29,676, 30,676, 30,472) | 4 (4, 6, 1) | 2,625 (2,689, 2,724, 2,463) | 0 (0, 0, 0) |
| F (D / cpu1) | 3 | 47,304 (49,567, 45,029, 47,316) | 3.20 (2.75, 3.67, 3.19) | 9.93 (9.18, 10.80, 9.81) | 34.20 (33.04, 35.32, 34.25) | 15.51 (14.91, 16.07, 15.56) | 9.50 (8.87, 10.17, 9.45) | 31,031 (31,264, 30,828, 31,000) | 14 (15, 12, 16) | 2,340 (2,366, 2,040, 2,614) | 0 (0, 0, 0) |
| G (ALL / cpu1) | 3 | 60,178 (59,674, 61,103, 59,756) | 2.23 (2.55, 2.03, 2.10) | 6.57 (6.90, 6.28, 6.52) | 28.91 (29.44, 28.46, 28.84) | 14.85 (15.39, 14.39, 14.77) | 4.95 (5.04, 4.89, 4.93) | 31,148 (31,096, 31,260, 31,088) | 19 (15, 18, 24) | 3,497 (4,033, 3,188, 3,269) | 0 (0, 0, 0) |
| N (N1 / split) | 3 | 38,574 (38,522, 38,835, 38,364) | 4.42 (4.44, 4.38, 4.45) | 12.09 (12.18, 11.82, 12.26) | 36.49 (36.45, 36.54, 36.47) | 19.41 (19.42, 19.27, 19.54) | 10.02 (9.97, 10.06, 10.04) | 30,628 (30,624, 30,640, 30,620) | 8 (7, 6, 11) | 1,619 (1,722, 1,532, 1,603) | 0 (0, 0, 0) |

## Comparisons

X minus Y on the arm means; "range" is max minus min of an arm's three runs (the run-to-run spread).
Rule used: a difference is called outside the spread only if the two arms' run sets do not overlap AND the difference of the means is larger than the larger of the two ranges; anything else is "inside the spread" and is not called a win.
Source out/analysis/table_cmp.md.

| comparison | metric | X mean | Y mean | X - Y | % | range X | range Y | sets overlap? | outside spread? |
|---|---|--:|--:|--:|--:|--:|--:|---|---|
| B vs A: P1 vs baseline (split) | moves/s | 50,280 | 37,086 | +13,195 | +35.6% | 1,595 | 1,618 | no | YES |
| B vs A: P1 vs baseline (split) | CPU us/move | 29.85 | 37.59 | -7.74 | -20.6% | 0.28 | 1.47 | no | YES |
| B vs A: P1 vs baseline (split) | loop us/move | 19.12 | 20.25 | -1.12 | -5.5% | 0.69 | 0.95 | no | YES |
| B vs A: P1 vs baseline (split) | flusher us/move | 3.86 | 10.27 | -6.41 | -62.4% | 0.11 | 0.65 | no | YES |
| B vs A: P1 vs baseline (split) | p99 ms | 6.68 | 12.62 | -5.94 | -47.1% | 0.25 | 1.18 | no | YES |
| C vs A: P3 vs baseline (split) | moves/s | 38,385 | 37,086 | +1,299 | +3.5% | 1,931 | 1,618 | yes | no |
| C vs A: P3 vs baseline (split) | CPU us/move | 36.68 | 37.59 | -0.91 | -2.4% | 1.49 | 1.47 | yes | no |
| C vs A: P3 vs baseline (split) | loop us/move | 19.39 | 20.25 | -0.86 | -4.2% | 1.01 | 0.95 | yes | no |
| C vs A: P3 vs baseline (split) | flusher us/move | 10.23 | 10.27 | -0.03 | -0.3% | 0.46 | 0.65 | yes | no |
| C vs A: P3 vs baseline (split) | p99 ms | 12.36 | 12.62 | -0.27 | -2.1% | 0.96 | 1.18 | yes | no |
| E vs A: ALL vs baseline (split) | moves/s | 49,502 | 37,086 | +12,417 | +33.5% | 2,089 | 1,618 | no | YES |
| E vs A: ALL vs baseline (split) | CPU us/move | 30.34 | 37.59 | -7.25 | -19.3% | 0.93 | 1.47 | no | YES |
| E vs A: ALL vs baseline (split) | loop us/move | 19.38 | 20.25 | -0.86 | -4.3% | 0.90 | 0.95 | yes | no |
| E vs A: ALL vs baseline (split) | flusher us/move | 3.96 | 10.27 | -6.31 | -61.4% | 0.12 | 0.65 | no | YES |
| E vs A: ALL vs baseline (split) | p99 ms | 6.88 | 12.62 | -5.74 | -45.5% | 0.38 | 1.18 | no | YES |
| N vs A: N1 vs baseline (split) | moves/s | 38,574 | 37,086 | +1,488 | +4.0% | 470 | 1,618 | no | no |
| N vs A: N1 vs baseline (split) | CPU us/move | 36.49 | 37.59 | -1.10 | -2.9% | 0.09 | 1.47 | no | no |
| N vs A: N1 vs baseline (split) | loop us/move | 19.41 | 20.25 | -0.84 | -4.1% | 0.27 | 0.95 | no | no |
| N vs A: N1 vs baseline (split) | flusher us/move | 10.02 | 10.27 | -0.24 | -2.4% | 0.09 | 0.65 | yes | no |
| N vs A: N1 vs baseline (split) | p99 ms | 12.09 | 12.62 | -0.54 | -4.3% | 0.44 | 1.18 | yes | no |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | moves/s | 47,304 | 37,086 | +10,219 | +27.6% | 4,538 | 1,618 | no | YES |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | CPU us/move | 34.20 | 37.59 | -3.39 | -9.0% | 2.28 | 1.47 | no | YES |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | loop us/move | 15.51 | 20.25 | -4.73 | -23.4% | 1.16 | 0.95 | no | YES |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | flusher us/move | 9.50 | 10.27 | -0.77 | -7.5% | 1.30 | 0.65 | yes | no |
| F vs A: interrupt layout alone (cpu1 vs split, cbase_D) | p99 ms | 9.93 | 12.62 | -2.69 | -21.3% | 1.62 | 1.18 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | moves/s | 60,178 | 47,304 | +12,874 | +27.2% | 1,428 | 4,538 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | CPU us/move | 28.91 | 34.20 | -5.29 | -15.5% | 0.98 | 2.28 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | loop us/move | 14.85 | 15.51 | -0.66 | -4.3% | 1.00 | 1.16 | yes | no |
| G vs F: the PRs under cpu1 (ALL vs D) | flusher us/move | 4.95 | 9.50 | -4.54 | -47.8% | 0.15 | 1.30 | no | YES |
| G vs F: the PRs under cpu1 (ALL vs D) | p99 ms | 6.57 | 9.93 | -3.36 | -33.9% | 0.62 | 1.62 | no | YES |
| G vs A: ALL cpu1 vs baseline split | moves/s | 60,178 | 37,086 | +23,092 | +62.3% | 1,428 | 1,618 | no | YES |
| G vs A: ALL cpu1 vs baseline split | CPU us/move | 28.91 | 37.59 | -8.68 | -23.1% | 0.98 | 1.47 | no | YES |
| G vs A: ALL cpu1 vs baseline split | loop us/move | 14.85 | 20.25 | -5.40 | -26.7% | 1.00 | 0.95 | no | YES |
| G vs A: ALL cpu1 vs baseline split | flusher us/move | 4.95 | 10.27 | -5.31 | -51.8% | 0.15 | 0.65 | no | YES |
| G vs A: ALL cpu1 vs baseline split | p99 ms | 6.57 | 12.62 | -6.06 | -48.0% | 0.62 | 1.18 | no | YES |

### Reading the comparisons

- B vs A (PR 496, the journal flush lists), fact: +13,195 moves/s (+35.6 percent), spread 1,595 (B) and 1,618 (A), no overlap; outside the spread.
  CPU per move falls 7.74 us (-20.6 percent), almost all of it the flusher (10.27 to 3.86 us/move, -62.4 percent); the loop also drops 1.12 us/move (outside the spread), and move p99 halves (12.62 to 6.68 ms).
- C vs A (PR 497, the inline pass budget by wall clock), fact: +1,299 moves/s (+3.5 percent) with spread 1,931 and 1,618 and overlapping run sets; inside the spread, not a win.
  The loop is 0.86 us/move lower on the mean, also inside the spread (ranges 1.01 and 0.95).
- E vs A (both PRs), fact: +12,417 moves/s (+33.5 percent), outside the spread; flusher -6.31 us/move, outside; loop -0.86 us/move, inside the spread.
  E vs B (not asked, from the same table): E's mean is 778 moves/s below B's with overlapping sets, so adding PR 497 to PR 496 shows no measurable effect here.
- N vs A (CPU=neoverse-n1), fact: +1,488 moves/s (+4.0 percent) and -1.10 us/move; the run sets do not overlap for moves/s, CPU and loop, but each difference is smaller than A's range (1,618 moves/s, 1.47 us/move, 0.95 us/move loop), so by the rule it is inside the spread and not a win.
  Inference: N's own range is small (470 moves/s, 0.09 us/move), so a real gain of a few percent is plausible; three more A/N pairs would settle it.
- F vs A (the interrupt layout alone, cbase_D), fact: +10,219 moves/s (+27.6 percent), outside the spread (F's range is the largest of all arms, 4,538: 49.6k, 45.0k, 47.3k); the loop drops 4.73 us/move (the network softirq leaves CPU 0), the flusher change is inside the spread.
  This repeats trip 1's single irq1 hint (45.5k) with three pinned runs.
- G vs F (both PRs under cpu1), fact: +12,874 moves/s (+27.2 percent), outside the spread; flusher -4.54 us/move, outside; loop -0.66 us/move, inside.
- G vs A (both PRs plus cpu1 against the baseline layout), fact: +23,092 moves/s (+62.3 percent), CPU per move -8.68 us (-23.1 percent), move p99 12.62 to 6.57 ms; all outside the spread.

### Tails and pushes (facts, with inferences marked)

- Move max is 76 to 112 ms in every split run and 249 to 431 ms in every cpu1 run (F and G), and cpu1 runs have more pushes over 100 ms (F 12-16, G 15-24, against A 4-9, B 5-11, E 1-6).
  Inference: with both queues on CPU 1, CPU 1 carries all the network softirq plus the flusher, job-0 and req-0, and its run-queue wait rises (total runq wait 21.8-22.2 us/move in F against 14.5-14.9 in A); rare long stalls of the CPU 1 threads would explain the longer max, but no trace of individual stalls was taken.
- Push max is 360 to 1,021 ms in every arm, including the baseline, while move max under split is about 100 ms; push receipts over 100 ms are at most 24 out of about a million per run (0.002 percent).
  The cause of these rare late pushes is not identified in this trip.
- The p99 of push latency follows move latency: about 15 ms in A, C, N; about 8.5 ms in B and E; 11-12.6 ms in F; about 8 ms in G.

## Human-pace runs (single runs, hints only)

Ran because box time was 27.5 minutes after the three rounds (under 34): `load.sh run LABEL IP 192 2000 4000 100000 45000`, cbase_D, window [t_go+5 s, t_go+43 s].

| run | irq | moves/s | move p50 / p95 / p99 / max ms | push p50 / p99 / max ms | push > 100 ms | server CPU us/move (ticks) | loop schedstat us/move | queue IRQs on CPU 0 % |
|---|---|--:|---|---|--:|--:|--:|--:|
| hum_split | split | 98.8 | 0.21 / 0.28 / 0.31 / 2.68 | 0.21 / 0.32 / 2.69 | 0 | 53.26 | 53.89 | 50.2 |
| hum_cpu1 | cpu1 | 98.5 | 0.21 / 0.29 / 0.39 / 4.45 | 0.22 / 0.41 / 4.46 | 0 | 56.10 | 54.55 | 0.0 |

Fact: at human pace both layouts give the same median (0.21 ms) and no push over 100 ms; cpu1's p99 is 0.08 ms higher and its max 1.8 ms higher, in one run each.
Fact: CPU per move is about 54 us at about 99 moves/s, but the process used only 0.20 to 0.21 s in the 38 s window, a count of 20 to 21 ticks, so the per-move CPU carries about 5 percent quantization and is mostly the loop's per-wakeup overhead (about one voluntary switch per move); it does not compare with the throughput runs.
The other threads had no tick in the window (the flusher wrote 3 flushes in the whole run).
Inference: the cpu1 layout does not hurt a human-pace game in these two runs; whether its longer max under saturation (above) matters for production load is open.

## What was dropped or went wrong

- Nothing was dropped: the warm-up, 21 counted runs (3 per arm) and both human-pace runs ran; no run failed or was retried, and no DROP or STOP line was printed by the budget logic.
- Warm-up wA: 38,016 moves/s with 47.8 percent of queue IRQs on CPU 0, inside the 35-41k check.
- One parsing defect, fixed: r2C's on-box `rssmax` line came from a sampler line with an empty RSS field; analyze.py now takes RSS max from raw/srv/rss_LABEL.log, skipping incomplete lines (r2C: 30,508 kB).
- The server log has 0 to 9 WARN lines per run, all "game id XXXXXX is taken (draw 1/5), drawing again" (random game id collisions retried), in every arm.

## Facts versus inferences, and gaps

- Facts are numbers read from the named files; inferences are marked.
- Arm order was interleaved (A B C E F G N per round), so slow drift would spread across arms; A's three runs are 37.7k, 36.0k, 37.6k (r2A the low one), consistent with trip 1's base2 (37.5k at 50 percent on CPU 0).
- No profiler ran; the per-thread split is ticks and schedstat only, so where inside the loop the 0.86 to 1.12 us/move of B and E went is not measured.
- PR 497's own counters (how many runs were CPU-timed) are not in the shutdown log, so its mechanism was not observed directly; only its outcome (loop -0.86 us/move, inside the spread).
- Push latency includes the server's fan-out and the other client's scheduling on a 384-process load box (38 to 62 percent busy, busiest vCPU at most 62.2 percent in G).

## Files

- Scripts: scripts/build.sh, scripts/up_trip2.sh (launch), scripts/phase1.sh, scripts/phase2.sh, scripts/trip2.sh (driver), scripts/watchdog.sh (teardown at minute 44 if needed; it exited unused at 16:19:15Z), scripts/waitready.sh, scripts/leftovers.sh, scripts/analyze.py, scripts/pushlat.py; grav/down.sh and grav/ssm.sh unchanged.
- Box side: pkg/irq.sh (new), pkg/load.sh (window from PW0/PW1, p95 and max), pkg/srv.sh and pkg/cpu.sh (trip 1's, unchanged).
- Raw: out/raw/srv (cpu_LABEL.log samplers, irq_LABEL_start/end.txt, softirq snapshots, irqmode_LABEL.txt, server_LABEL.log, rss_LABEL.log, status files), out/raw/load (LABEL.tgz with the ev.* files and t_go_clk, lcpu_LABEL.log).
- Log: out/steps.log (every on-box summary), out/phase1.txt, out/phase2.txt, out/launch.txt, out/teardown.txt, out/watchdog.txt.
- Analysis: out/analysis/runs.tsv, table_runs.md, table_arms.md, table_cmp.md, table_tail.md, pushlat.txt.
- Builds: bin/cbase_{D,P1,P3,ALL,N1}, bin/unstripped/, out/builds/.

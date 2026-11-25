# Trip 3 results: the merged campaign on the real nano, PR 500, the clock count of PR 497, and the list upkeep of PR 496

Real t4g.nano server (i-REDACTED10), c7g.2xlarge load box (i-REDACTED11), us-east-1, 2026-11-14.
Box time: run-instances 18:29:38Z (server) and 18:29:40Z (load), both terminated by 19:00:15Z (grav/down.sh), 30.6 minutes.
Every run: irqbalance stopped and verified, interrupts in the `split` layout (ens5-Tx-Rx-0 on CPU 1, ens5-Tx-Rx-1 on CPU 0, ena-mgmnt on CPU 1), /proc/interrupts before and after, fresh server and DB, knob auto, `load.sh run LABEL IP 192 0 1 100000 25000`, play window [t_go+2 s, t_go+23 s].
All paths are relative to foolish/docs/memtest/nano-2026-11/campaign/trip3/.
Teardown proof: out/teardown.txt (copied at the end of this file).

## Builds

| arm | tree | stripped sha256 | BuildID (stripped = unstripped twin) |
|---|---|---|---|
| D | 9c5057b64 (pre-496), grav/bin/cbase_D, not rebuilt | 4eae8437fe5ba915682b752ecfd71cd9a40b77051807d186ad4a593ea96da0b7 | fc51bf0e... (trip 1) |
| M1 | 77fef4bef (main with PR 496) | 9ca3b791ebcfc586b2696bff031f8b7ac1edd9c4de8b5840f1e93677033bebbf | f7f52c42e2f285f90ed622114249cf11995b0030 |
| M2 | 05cf2501a (PR 496 + 497) | 53e1cd472fa1d9945c8fd876634dea7cd5203ebf186240891123792ff9ed422b | 4afa49a51d35c156c58e68b0e0bc74139cdddb3f |
| M3 | 59c115054 (PR 496 + 497 + 498) | 90af1e0cd88e721217699c34a937eb66116258b39404a05ae862b38f23cc98ff | e46155ba4134013186d7f028522d1e4efa86b5e9 |
| M4 | 11b34d054 (PR 500 head, M3 plus the spectator wake) | e3844e1e1f1b4ad46c0384a0caafbb59c4e907667dcab42f40083bb28df1ec79 | b74912a5fb90559069cfec559750582663dbead6 |

Fact: cbase_M1 is byte-identical to trip 2's cbase_P1 (9ca3b791..., PR 496's head 5d942b0a7, whose tree equals 77fef4bef's tree f2b5e301c).
Fact: cbase_M2 is byte-identical to trip 2's cbase_ALL (53e1cd47...).
Inference: the build recipe (scripts/build.sh, a copy of trip 2's with nice -n 10 and outputs under trip3) is reproducible across sessions.
Builds: scripts/build_all.sh, logs out/builds/build_M*.log, clean detached worktrees /Users/alex/Dev/foolish-wt-trip3-M1..M4, all removed.
Fact: origin/main moved to c38ff8d94 after the brief (deploy tooling in c_base/providers/aws/ops and the cbase submodule pin only, no server sources); M3 was built from 59c115054 as briefed.

## Setup checks

Fact: perf 6.1.190, bpftrace v0.17.0 and strace 6.12 were installed by hand (scripts/trip3.sh install: a 768 MB swapfile first, dnf rc 0, swapfile off again before any run); chronyd active and 0 OOM lines in dmesg before and after the install and after the symbol check (out/steps.log).
Fact: the AMI has a 412 MB zram swap device enabled by default (seen before the install), so trip 2's runs had it too.
Fact: user symbols of stripped cbase_M3 resolve through the unstripped twin (HOME=/root for the build-id cache): smoke unknown_share 0.000 over 3,982 cbase_M3 samples on the server; on the load box resolve_perfM3.txt gives /opt/mt/cbase_M3 66.98% of samples with unknown_within 0.0000.
Fact: bpftrace attached with trip.bt (arrow syntax) in the check and in both count runs; the strace fallback was not needed.
Fact: warm-up wD (not counted): 36,969 moves/s, 49.0% of queue interrupts on CPU 0 (expected 36-39k and 47-51%).

## 1. Runs

Source: out/steps.log, out/raw/load/LABEL.tgz, out/raw/srv/rss_LABEL.log; scripts/analyze.py (trip 2's, arms renamed) writes analysis/runs.tsv, table_runs.md, table_arms.md, table_cmp.md, pushlat.txt; scripts/extra.py writes table_extra.md and table_accept.md.
Columns as trip 2: CPU and per-thread us per move from /proc ticks in the play window; flusher = the busiest unnamed cbase thread; runq wait is the process total from schedstat; load busy is the load box over all 8 vCPUs; push columns from scripts/pushlat.py.

| run | arm | build | irq | moves/s | p50 ms | p99 ms | CPU us/move | loop | flusher | job-0 | req-0 | runq wait us/move | VmHWM kB | RSS max kB | err_files | queue IRQs on CPU 0 % | load busy % (busiest vCPU) | to pool | over budget | valve | journal records / flushes / WAL frames | push n | push p50 / p99 ms | push > 100 ms |
|---|---|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| wD (warm-up, not counted) | D | D | split | 36,969.4 | 4.60 | 12.89 | 37.84 | 20.46 | 10.20 | 4.47 | 2.40 | 15.15 | 30,536 | 30,320 | 0 | 49.0 | 34.9 (36.1) | 1904 | 0 | 0 | 923519 / 6086 / 148502 | 746599 | 6.94 / 15.91 | 10 |
| r1D | D | D | split | 36,427.7 | 4.78 | 13.30 | 38.19 | 20.38 | 10.66 | 4.40 | 2.39 | 14.97 | 30,724 | 30,656 | 0 | 49.6 | 35.5 (36.9) | 1427 | 0 | 0 | 905465 / 5963 / 145795 | 735321 | 6.97 / 16.18 | 5 |
| r1M1 | M1 | M1 | split | 48,066.0 | 3.52 | 6.86 | 31.09 | 19.98 | 4.04 | 4.35 | 2.36 | 12.41 | 29,576 | 29,292 | 0 | 49.5 | 46.3 (48.7) | 2218 | 0 | 0 | 1186128 / 8093 / 191309 | 970629 | 5.53 / 8.54 | 7 |
| r1M2 | M2 | M2 | split | 50,947.6 | 3.28 | 6.72 | 29.73 | 18.75 | 3.94 | 4.36 | 2.31 | 12.73 | 30,888 | 30,804 | 0 | 49.1 | 50.0 (51.8) | 2669 | 0 | 0 | 1259072 / 8329 / 202013 | 1028323 | 5.17 / 8.49 | 8 |
| r1M3 | M3 | M3 | split | 50,252.2 | 3.32 | 6.78 | 30.09 | 18.99 | 4.03 | 4.36 | 2.34 | 12.79 | 31,096 | 30,964 | 0 | 49.4 | 48.7 (49.7) | 2760 | 0 | 0 | 1239222 / 8062 / 197585 | 1014622 | 5.19 / 8.49 | 8 |
| r1M4 | M4 | M4 | split | 51,087.0 | 3.26 | 6.73 | 29.45 | 18.55 | 3.90 | 4.34 | 2.31 | 12.72 | 30,296 | 30,040 | 0 | 50.1 | 50.1 (51.1) | 3277 | 0 | 0 | 1259378 / 8148 / 200265 | 1031284 | 5.08 / 8.40 | 12 |
| r2D | D | D | split | 37,026.7 | 4.53 | 12.82 | 37.58 | 20.51 | 10.03 | 4.37 | 2.33 | 14.63 | 30,648 | 30,668 | 0 | 50.3 | 34.7 (35.5) | 1654 | 0 | 0 | 915987 / 5992 / 147817 | 748051 | 6.99 / 15.76 | 10 |
| r2M1 | M1 | M1 | split | 47,045.4 | 3.63 | 7.29 | 31.54 | 20.40 | 3.98 | 4.42 | 2.36 | 12.28 | 30,752 | 30,512 | 0 | 49.0 | 44.2 (45.5) | 2255 | 0 | 0 | 1164228 / 7863 / 187598 | 950178 | 5.63 / 9.09 | 10 |
| r2M2 | M2 | M2 | split | 50,514.2 | 3.34 | 6.70 | 30.06 | 18.94 | 3.99 | 4.40 | 2.34 | 12.88 | 30,944 | 30,712 | 0 | 49.2 | 50.3 (50.8) | 2652 | 0 | 0 | 1247785 / 8267 / 200166 | 1020362 | 5.22 / 8.44 | 7 |
| r2M3 | M3 | M3 | split | 49,910.2 | 3.35 | 7.15 | 30.41 | 19.11 | 4.12 | 4.41 | 2.39 | 12.98 | 31,072 | 30,900 | 0 | 48.7 | 47.8 (48.7) | 2991 | 0 | 0 | 1231144 / 7847 / 196039 | 1007467 | 5.17 / 9.02 | 13 |
| r2M4 | M4 | M4 | split | 52,393.4 | 3.19 | 6.44 | 29.29 | 18.22 | 3.95 | 4.38 | 2.33 | 12.87 | 31,000 | 30,776 | 0 | 48.9 | 50.3 (51.2) | 3117 | 0 | 0 | 1295436 / 8450 / 207654 | 1057931 | 4.98 / 8.04 | 6 |
| r3D | D | D | split | 37,665.5 | 4.64 | 12.48 | 37.15 | 20.10 | 9.95 | 4.37 | 2.38 | 15.00 | 30,744 | 30,724 | 0 | 48.8 | 37.7 (38.6) | 1551 | 0 | 0 | 941140 / 6049 / 150059 | 760597 | 6.75 / 15.24 | 11 |
| r3M1 | M1 | M1 | split | 50,645.5 | 3.28 | 6.60 | 29.91 | 18.90 | 3.95 | 4.38 | 2.32 | 12.55 | 30,820 | 30,712 | 0 | 49.2 | 49.6 (50.3) | 2377 | 0 | 0 | 1250052 / 8391 / 201021 | 1021918 | 5.25 / 8.33 | 10 |
| r3M2 | M2 | M2 | split | 49,891.0 | 3.15 | 6.75 | 29.10 | 18.32 | 3.78 | 4.32 | 2.30 | 12.38 | 31,028 | 30,876 | 0 | 48.5 | 46.5 (47.6) | 2552 | 0 | 0 | 1236699 / 7666 / 195541 | 1006629 | 4.94 / 8.47 | 14 |
| r3M3 | M3 | M3 | split | 52,244.3 | 3.18 | 6.72 | 29.33 | 18.26 | 3.93 | 4.40 | 2.35 | 12.87 | 30,824 | 30,656 | 0 | 49.5 | 51.8 (52.8) | 3125 | 0 | 0 | 1285090 / 8330 / 205122 | 1054723 | 5.00 / 8.35 | 9 |
| r3M4 | M4 | M4 | split | 50,265.8 | 3.20 | 6.50 | 29.70 | 18.48 | 3.96 | 4.47 | 2.42 | 13.44 | 31,104 | 30,924 | 0 | 48.7 | 49.1 (50.4) | 2840 | 0 | 0 | 1247463 / 7701 / 196690 | 1014708 | 4.98 / 8.13 | 6 |
| bpfM1 (profiled, not counted) | bpfM1 | M1 | split | 30,043.3 | 5.01 | 10.09 | 38.34 | 26.89 | 3.72 | 4.62 | 2.70 | 11.78 | 29,064 | 28,784 | 0 | 49.7 | 28.0 (29.0) | 923 | 0 | 0 | 768221 / 4342 / 119317 | 606069 | 6.83 / 12.66 | 15 |
| bpfM2 (profiled, not counted) | bpfM2 | M2 | split | 38,880.9 | 4.51 | 9.15 | 35.31 | 23.71 | 3.99 | 4.57 | 2.65 | 12.05 | 30,316 | 30,220 | 0 | 50.4 | 37.2 (38.5) | 1616 | 0 | 0 | 969579 / 6005 / 153154 | 784658 | 6.32 / 11.71 | 14 |
| perfM3 (profiled, not counted) | perfM3 | M3 | split | 49,469.7 | 3.39 | 7.09 | 30.90 | 19.26 | 4.14 | 4.63 | 2.48 | 13.60 | 30,124 | 30,028 | 0 | 49.1 | 48.5 (48.9) | 3241 | 0 | 0 | 1220959 / 7949 / 196395 | 999004 | 5.25 / 8.91 | 6 |

Per-run tail, push latency, run-queue wait per thread and per-CPU shares (analysis/table_extra.md):

| run | move p95 ms | move max ms | push p50 / p99 / max ms | push missing | runq wait us/move loop / flusher / job-0 / req-0 | CPU 0 busy / softirq / idle % | CPU 1 busy / softirq / idle % | requests on the loop | warn lines |
|---|--:|--:|---|--:|---|---|---|--:|--:|
| wD | 10.04 | 86.24 | 6.94 / 15.91 / 897.3 | 7 | 0.43 / 3.96 / 4.10 / 6.26 | 79.2 / 13.6 / 20.8 | 73.8 / 20.8 / 25.0 | 943976 | 2 |
| r1D | 10.49 | 100.69 | 6.97 / 16.18 / 928.4 | 6 | 0.49 / 3.84 / 4.04 / 6.24 | 78.4 / 14.3 / 21.6 | 73.4 / 20.8 / 25.4 | 925837 | 2 |
| r1M1 | 6.12 | 107.22 | 5.53 / 8.54 / 539.7 | 7 | 0.21 / 2.97 / 3.63 / 5.25 | 97.7 / 18.0 / 2.3 | 66.4 / 25.2 / 31.3 | 1212977 | 0 |
| r1M2 | 5.67 | 91.90 | 5.17 / 8.49 / 607.9 | 10 | 0.28 / 3.08 / 3.52 / 5.53 | 97.9 / 18.0 / 2.1 | 68.1 / 25.1 / 29.5 | 1288326 | 6 |
| r1M3 | 5.78 | 244.78 | 5.19 / 8.49 / 532.4 | 8 | 0.27 / 3.07 / 3.61 / 5.49 | 97.7 / 18.7 / 2.3 | 68.3 / 24.6 / 28.8 | 1267449 | 4 |
| r1M4 | 5.72 | 424.42 | 5.08 / 8.40 / 605.9 | 9 | 0.26 / 3.10 / 3.60 / 5.44 | 97.1 / 17.3 / 2.9 | 68.4 / 26.5 / 28.0 | 1288343 | 1 |
| r2D | 10.05 | 106.46 | 6.99 / 15.76 / 931.9 | 5 | 0.42 / 3.81 / 3.97 / 6.05 | 80.0 / 13.5 / 20.0 | 72.5 / 20.6 / 26.3 | 935903 | 2 |
| r2M1 | 6.39 | 106.42 | 5.63 / 9.09 / 598.8 | 10 | 0.24 / 3.02 / 3.55 / 5.12 | 97.9 / 18.1 / 2.1 | 66.5 / 26.9 / 31.0 | 1190328 | 5 |
| r2M2 | 5.83 | 88.14 | 5.22 / 8.44 / 498.1 | 1 | 0.24 / 3.18 / 3.71 / 5.38 | 97.7 / 18.2 / 2.3 | 68.7 / 25.8 / 29.0 | 1275821 | 3 |
| r2M3 | 5.92 | 281.82 | 5.17 / 9.02 / 547.3 | 9 | 0.31 / 3.21 / 3.71 / 5.38 | 97.9 / 17.5 / 2.1 | 68.1 / 24.0 / 28.5 | 1259705 | 2 |
| r2M4 | 5.50 | 114.21 | 4.98 / 8.04 / 499.2 | 6 | 0.24 / 3.04 / 3.74 / 5.47 | 97.7 / 19.4 / 2.3 | 70.2 / 25.8 / 27.4 | 1324751 | 4 |
| r3D | 9.82 | 130.55 | 6.75 / 15.24 / 884.9 | 11 | 0.41 / 3.87 / 4.19 / 6.14 | 79.5 / 14.2 / 20.5 | 73.4 / 20.0 / 24.8 | 961558 | 2 |
| r3M1 | 5.59 | 80.16 | 5.25 / 8.33 / 499.2 | 7 | 0.27 / 2.95 / 3.63 / 5.36 | 97.9 / 17.2 / 2.1 | 68.3 / 25.5 / 29.1 | 1279319 | 4 |
| r3M2 | 5.50 | 685.75 | 4.94 / 8.47 / 436.6 | 7 | 0.32 / 2.85 / 3.59 / 5.28 | 94.0 / 16.1 / 6.0 | 65.4 / 24.0 / 28.0 | 1265751 | 1 |
| r3M3 | 5.51 | 214.45 | 5.00 / 8.35 / 441.4 | 6 | 0.29 / 3.13 / 3.72 / 5.35 | 98.0 / 18.8 / 2.0 | 69.3 / 24.6 / 28.0 | 1314679 | 3 |
| r3M4 | 5.61 | 424.02 | 4.98 / 8.13 / 485.4 | 8 | 0.25 / 3.23 / 3.86 / 5.67 | 95.0 / 17.8 / 5.0 | 69.0 / 25.5 / 26.0 | 1276157 | 3 |
| bpfM1 | 9.14 | 1,557.00 | 6.83 / 12.66 / 807.1 | 3 | 0.25 / 2.83 / 3.87 / 4.49 | 82.7 / 13.6 / 17.3 | 49.0 / 20.7 / 29.0 | 786165 | 1 |
| bpfM2 | 8.05 | 598.10 | 6.32 / 11.71 / 725.3 | 1 | 0.34 / 2.87 / 3.75 / 4.76 | 94.5 / 14.5 / 5.4 | 59.7 / 22.0 / 31.6 | 992032 | 1 |
| perfM3 | 5.98 | 205.58 | 5.25 / 8.91 / 568.4 | 3 | 0.32 / 3.24 / 3.88 / 5.75 | 97.7 / 19.4 / 2.3 | 69.6 / 26.2 / 27.8 | 1248655 | 3 |

Fact: the queue interrupt share on CPU 0 was 48.5 to 50.4% in every run, so the interrupt layout is the same in all arms.
Fact: valve tripped 0 times and 0 runs over budget in every run; err_files 0 in every run.
Fact: the shutdown journal line, e.g. r3M3: 1,285,090 records, 8,330 flushes, 205,122 WAL frames (all runs in the table above, raw/srv/server_LABEL.log).
Fact: on M1 to M4 CPU 0 (the loop's vCPU) is 2.0 to 6.0% idle, against 20.0 to 21.6% on D.

## 2. Per arm (means of the 3 interleaved runs, the three values in brackets)

| arm | n | moves/s mean (values) | p50 ms mean (values) | p99 ms mean (values) | CPU us/move mean (values) | loop us/move mean (values) | flusher us/move mean (values) | RSS max kB mean (values) | push > 100 ms mean (values) | to pool mean (values) | valve trips mean (values) |
|---|--:|---|---|---|---|---|---|---|---|---|---|
| D (9c5057b64 pre-496) | 3 | 37,040 (36,428, 37,027, 37,666) | 4.65 (4.78, 4.53, 4.64) | 12.87 (13.30, 12.82, 12.48) | 37.64 (38.19, 37.58, 37.15) | 20.33 (20.38, 20.51, 20.10) | 10.21 (10.66, 10.03, 9.95) | 30,683 (30,656, 30,668, 30,724) | 9 (5, 10, 11) | 1,544 (1,427, 1,654, 1,551) | 0 (0, 0, 0) |
| M1 (77fef4bef +496) | 3 | 48,586 (48,066, 47,045, 50,646) | 3.48 (3.52, 3.63, 3.28) | 6.92 (6.86, 7.29, 6.60) | 30.85 (31.09, 31.54, 29.91) | 19.76 (19.98, 20.40, 18.90) | 3.99 (4.04, 3.98, 3.95) | 30,172 (29,292, 30,512, 30,712) | 9 (7, 10, 10) | 2,283 (2,218, 2,255, 2,377) | 0 (0, 0, 0) |
| M2 (05cf2501a +496+497) | 3 | 50,451 (50,948, 50,514, 49,891) | 3.26 (3.28, 3.34, 3.15) | 6.72 (6.72, 6.70, 6.75) | 29.63 (29.73, 30.06, 29.10) | 18.67 (18.75, 18.94, 18.32) | 3.90 (3.94, 3.99, 3.78) | 30,797 (30,804, 30,712, 30,876) | 10 (8, 7, 14) | 2,624 (2,669, 2,652, 2,552) | 0 (0, 0, 0) |
| M3 (59c115054 +496+497+498) | 3 | 50,802 (50,252, 49,910, 52,244) | 3.28 (3.32, 3.35, 3.18) | 6.88 (6.78, 7.15, 6.72) | 29.94 (30.09, 30.41, 29.33) | 18.79 (18.99, 19.11, 18.26) | 4.03 (4.03, 4.12, 3.93) | 30,840 (30,964, 30,900, 30,656) | 10 (8, 13, 9) | 2,959 (2,760, 2,991, 3,125) | 0 (0, 0, 0) |
| M4 (11b34d054 M3+PR 500) | 3 | 51,249 (51,087, 52,393, 50,266) | 3.22 (3.26, 3.19, 3.20) | 6.56 (6.73, 6.44, 6.50) | 29.48 (29.45, 29.29, 29.70) | 18.42 (18.55, 18.22, 18.48) | 3.94 (3.90, 3.95, 3.96) | 30,580 (30,040, 30,776, 30,924) | 8 (12, 6, 6) | 3,078 (3,277, 3,117, 2,840) | 0 (0, 0, 0) |

## 3. Comparisons (trip 2's rule: "outside the spread" = run sets do not overlap AND the difference exceeds the larger of the two ranges)

| comparison | metric | X mean | Y mean | X - Y | % | range X | range Y | sets overlap? | outside spread? |
|---|---|--:|--:|--:|--:|--:|--:|---|---|
| M1 vs D: PR 496 on merged code | moves/s | 48,586 | 37,040 | +11,546 | +31.2% | 3,600 | 1,238 | no | YES |
| M1 vs D: PR 496 on merged code | CPU us/move | 30.85 | 37.64 | -6.79 | -18.0% | 1.63 | 1.04 | no | YES |
| M1 vs D: PR 496 on merged code | loop us/move | 19.76 | 20.33 | -0.57 | -2.8% | 1.50 | 0.41 | yes | no |
| M1 vs D: PR 496 on merged code | flusher us/move | 3.99 | 10.21 | -6.22 | -60.9% | 0.09 | 0.71 | no | YES |
| M1 vs D: PR 496 on merged code | p99 ms | 6.92 | 12.87 | -5.95 | -46.2% | 0.69 | 0.82 | no | YES |
| M1 vs D: PR 496 on merged code | push > 100 ms | 9.0 | 8.7 | +0.3 | +3.8% | 3.0 | 6.0 | yes | no |
| M1 vs D: PR 496 on merged code | RSS max kB | 30,172 | 30,683 | -511 | -1.7% | 1,420 | 68 | yes | no |
| M2 vs M1: PR 497 | moves/s | 50,451 | 48,586 | +1,865 | +3.8% | 1,057 | 3,600 | yes | no |
| M2 vs M1: PR 497 | CPU us/move | 29.63 | 30.85 | -1.22 | -3.9% | 0.96 | 1.63 | yes | no |
| M2 vs M1: PR 497 | loop us/move | 18.67 | 19.76 | -1.09 | -5.5% | 0.62 | 1.50 | yes | no |
| M2 vs M1: PR 497 | flusher us/move | 3.90 | 3.99 | -0.09 | -2.2% | 0.21 | 0.09 | yes | no |
| M2 vs M1: PR 497 | p99 ms | 6.72 | 6.92 | -0.19 | -2.8% | 0.05 | 0.69 | yes | no |
| M2 vs M1: PR 497 | push > 100 ms | 9.7 | 9.0 | +0.7 | +7.4% | 7.0 | 3.0 | yes | no |
| M2 vs M1: PR 497 | RSS max kB | 30,797 | 30,172 | +625 | +2.1% | 164 | 1,420 | yes | no |
| M3 vs M2: PR 498 | moves/s | 50,802 | 50,451 | +351 | +0.7% | 2,334 | 1,057 | yes | no |
| M3 vs M2: PR 498 | CPU us/move | 29.94 | 29.63 | +0.31 | +1.1% | 1.08 | 0.96 | yes | no |
| M3 vs M2: PR 498 | loop us/move | 18.79 | 18.67 | +0.12 | +0.6% | 0.85 | 0.62 | yes | no |
| M3 vs M2: PR 498 | flusher us/move | 4.03 | 3.90 | +0.12 | +3.2% | 0.19 | 0.21 | yes | no |
| M3 vs M2: PR 498 | p99 ms | 6.88 | 6.72 | +0.16 | +2.4% | 0.43 | 0.05 | yes | no |
| M3 vs M2: PR 498 | push > 100 ms | 10.0 | 9.7 | +0.3 | +3.4% | 5.0 | 7.0 | yes | no |
| M3 vs M2: PR 498 | RSS max kB | 30,840 | 30,797 | +43 | +0.1% | 308 | 164 | yes | no |
| M4 vs M3: PR 500 | moves/s | 51,249 | 50,802 | +447 | +0.9% | 2,128 | 2,334 | yes | no |
| M4 vs M3: PR 500 | CPU us/move | 29.48 | 29.94 | -0.46 | -1.5% | 0.41 | 1.08 | yes | no |
| M4 vs M3: PR 500 | loop us/move | 18.42 | 18.79 | -0.37 | -2.0% | 0.33 | 0.85 | yes | no |
| M4 vs M3: PR 500 | flusher us/move | 3.94 | 4.03 | -0.09 | -2.2% | 0.06 | 0.19 | yes | no |
| M4 vs M3: PR 500 | p99 ms | 6.56 | 6.88 | -0.33 | -4.7% | 0.29 | 0.43 | yes | no |
| M4 vs M3: PR 500 | push > 100 ms | 8.0 | 10.0 | -2.0 | -20.0% | 6.0 | 5.0 | yes | no |
| M4 vs M3: PR 500 | RSS max kB | 30,580 | 30,840 | -260 | -0.8% | 884 | 308 | yes | no |
| M3 vs D: everything merged | moves/s | 50,802 | 37,040 | +13,762 | +37.2% | 2,334 | 1,238 | no | YES |
| M3 vs D: everything merged | CPU us/move | 29.94 | 37.64 | -7.70 | -20.4% | 1.08 | 1.04 | no | YES |
| M3 vs D: everything merged | loop us/move | 18.79 | 20.33 | -1.54 | -7.6% | 0.85 | 0.41 | no | YES |
| M3 vs D: everything merged | flusher us/move | 4.03 | 10.21 | -6.19 | -60.6% | 0.19 | 0.71 | no | YES |
| M3 vs D: everything merged | p99 ms | 6.88 | 12.87 | -5.98 | -46.5% | 0.43 | 0.82 | no | YES |
| M3 vs D: everything merged | push > 100 ms | 10.0 | 8.7 | +1.3 | +15.4% | 5.0 | 6.0 | yes | no |
| M3 vs D: everything merged | RSS max kB | 30,840 | 30,683 | +157 | +0.5% | 308 | 68 | yes | no |

Acceptance per criterion (analysis/table_accept.md; "pass (worse within the spread: neutral)" means the mean is worse but not outside the spread):

| comparison | moves/s up or neutral | CPU per move not worse | p99 not worse | pushes over 100 ms not worse | RSS max not worse |
|---|---|---|---|---|---|
| M1 vs D (PR 496 on merged code) | pass (better, outside the spread); +11,545.67 | pass (better, outside the spread); -6.79 | pass (better, outside the spread); -5.95 | pass (worse within the spread: neutral); +0.33 | pass (better or equal, within the spread); -510.67 |
| M2 vs M1 (PR 497) | pass (better or equal, within the spread); +1,865.30 | pass (better or equal, within the spread); -1.22 | pass (better or equal, within the spread); -0.19 | pass (worse within the spread: neutral); +0.67 | pass (worse within the spread: neutral); +625.33 |
| M3 vs M2 (PR 498) | pass (better or equal, within the spread); +351.30 | pass (worse within the spread: neutral); +0.31 | pass (worse within the spread: neutral); +0.16 | pass (worse within the spread: neutral); +0.33 | pass (worse within the spread: neutral); +42.67 |
| M4 vs M3 (PR 500) | pass (better or equal, within the spread); +446.50 | pass (better or equal, within the spread); -0.46 | pass (better or equal, within the spread); -0.33 | pass (better or equal, within the spread); -2.00 | pass (better or equal, within the spread); -260.00 |
| M3 vs D (everything merged) | pass (better, outside the spread); +13,762.27 | pass (better, outside the spread); -7.70 | pass (better, outside the spread); -5.98 | pass (worse within the spread: neutral); +1.33 | pass (worse within the spread: neutral); +157.33 |

Summary.
- M1 vs D (PR 496 confirmed on merged code): +11,546 moves/s (+31.2%), CPU per move -6.79 us (-18.0%), p99 -5.95 ms (-46.2%), all outside the spread; pushes over 100 ms +0.3 per run and RSS -511 kB within the spread; pass on every criterion.
- M2 vs M1 (PR 497): +1,865 moves/s (+3.8%), CPU per move -1.22 us (-3.9%), loop -1.09 us (-5.5%), p99 -0.19 ms, all within the spread; pushes +0.7 per run and RSS +625 kB within the spread; pass (neutral to better, gain not proven by the spread rule).
- M3 vs M2 (PR 498): +351 moves/s (+0.7%), CPU per move +0.31 us (+1.1%), p99 +0.16 ms, pushes +0.3, RSS +43 kB, all within the spread; pass as neutral; no gain shown on the nano.
- M4 vs M3 (PR 500): +447 moves/s (+0.9%), CPU per move -0.46 us, p99 -0.33 ms, pushes over 100 ms -2.0 per run, RSS -260 kB, all within the spread; pass (no cost visible).
- M3 vs D (everything merged): +13,762 moves/s (+37.2%), CPU per move -7.70 us (-20.4%), loop -1.54 us (-7.6%), flusher -6.19 us (-60.6%), p99 -5.98 ms (-46.5%), all outside the spread; pushes over 100 ms +1.3 per run (8.7 to 10.0) and RSS +157 kB within the spread; pass on every criterion.
Fact: moves/s counts every answered move, refusals included (about 3.8% in every arm, pushlat.txt refused=).
Fact: the slowest move on M1 to M4 runs is 80 to 686 ms against 101 to 131 ms on D; the push max is 437 to 608 ms on M arms against 885 to 932 ms on D (table_extra.md).

## 4. Syscalls per move on the loop thread (bpftrace runs, counts only)

Source: raw/srv/bpf_bpfM1.txt and bpf_bpfM2.txt, scripts/perf3.py bpf (trip 1's analyze.py bpf, unchanged) to analysis/bpf_bpfM1.txt and bpf_bpfM2.txt; 20.0 s of whole TICKs inside the play window, 595,542 (M1) and 775,263 (M2) moves in that span.
bpftrace cost moves/s: bpfM1 30,043 against the M1 mean 48,586 (-38%), bpfM2 38,881 against the M2 mean 50,451 (-23%); their moves/s are not used.
Inference: the larger slowdown on M1 is the extra traced syscall (about 2 clock_gettime per move) paying the probe cost.

| loop syscall | M1 per move | M2 per move | trip 1 (pre-496, bpf1) per move |
|---|--:|--:|--:|
| clock_gettime | 1.954 | 0.002 | 1.967 |
| sendto | 1.846 | 1.841 | 1.843 |
| recvfrom | 1.057 | 1.056 | 1.056 |
| futex | 0.095 | 0.116 | 0.117 |
| epoll_pwait | 0.026 | 0.011 | 0.016 |
| epoll_ctl | 0.113 | 0.111 | 0.113 |
| all loop syscalls | 5.102 | 3.146 | |

Fact: PR 497's prediction holds: the loop's clock_gettime syscalls fall from 1.95 per move to 0.0022 per move (one in about 450 moves).
Fact: the loop's time inside futex (enter to exit, blocking included) is 1.01 us per move on M1 and 1.23 us per move on M2, against 5.00 us per move in trip 1 on the pre-496 tree; 80 to 75% of it is gone.
Fact: the loop's off-CPU sleep inside futex is 346.7 ms of 20 s on M1 (1.73%, 36.4 us per sleep, 0.58 us per move) and 545.8 ms on M2 (2.73%, 35.3 us per sleep, 0.70 us per move), against 2,583 ms (13.6%, 142 us per sleep, 4.45 us per move) in trip 1.
Fact: on M1 the loop sleeps 17.8% of the span in epoll_pwait (448.8 us per sleep), on M2 4.5%.
Inference: under bpftrace the M1 loop waits for work more because the traced clock syscalls slow it more than the clients; not a property of M1 without the tracer.
Note: syscall 287 on the CPU 1 threads is pwritev2 (aarch64), as trip 1 reported.

## 5. Profile of M3 (perf record -a -g -F 999 -e cycles -k CLOCK_MONOTONIC, run perfM3)

Source: raw/perf/perf_perfM3.data post-processed on the load box by pkg/prof.sh process (raw/proc/perfM3/cc_perfM3.txt.gz with callchains, flat_perfM3.txt.gz, rep_self and rep_children reports), scripts/perf3.py perf and children (trip 1's analyze.py functions unchanged) to analysis/perf_perfM3.txt and children_perfM3_loop.txt.
Play window server mono [1544.522, 1565.522], 1,038,864 moves (perfM3 ran at 49,470 moves/s, 2.6% below the M3 mean), 38,810 samples, cycles at 2.5 GHz.
Trip 1's numbers are its perfcy run on cbase_D, recomputed with the same function and list by scripts/before.py (analysis/children_perfcy_loop.txt; identical to trip1/out/analysis/children_perfcy_loop.txt where both list a symbol).
Caveat: trip 1's perfcy had 44% of queue interrupts on CPU 0, perfM3 49.1%.

Cycles per move by thread (us at 2.5 GHz):

| thread | trip 1 D user / kernel / total | trip 3 M3 user / kernel / total |
|---|---|---|
| all sampled | 18.77 / 18.57 / 37.34 | 13.75 / 16.79 / 30.53 |
| loop (CPU 0) | 7.97 / 10.66 / 18.63 | 7.33 / 10.14 / 17.47 |
| flusher (CPU 1) | 6.29 / 2.11 / 8.40 | 2.16 / 1.52 / 3.68 |
| job-0 | 2.89 / 1.46 / 4.35 | 2.84 / 1.48 / 4.32 |
| req-0 | 1.20 / 0.97 / 2.17 | 1.10 / 0.89 / 1.99 |
| checkpointer | 0.03 / 0.25 / 0.28 | 0.03 / 0.30 / 0.33 |
| idle task (softirq in idle) | 0 / 2.44 / 2.44 | 0 / 1.93 / 1.93 |

Loop contexts on M3: user 7.33, syscall 5.54, softirq 4.26, kernel other 0.33 us per move.
Flusher on M3: SQLite inclusive 3.16 of its 3.68 us per move.

Loop subsystems, inclusive us per move, before and after the campaign:

| symbol | trip 1 D (9c5057b64) | trip 3 M3 (59c115054) | difference |
|---|--:|--:|--:|
| cb_loop_run (whole loop) | 18.626 | 17.470 | -1.156 |
| ws_frames | 13.024 | 12.069 | -0.955 |
| pump | 9.596 | 8.509 | -1.087 |
| h_action | 7.893 | 7.688 | -0.205 |
| cb_table_run | 7.393 | 7.103 | -0.290 |
| cb_section_commit | 5.431 | 5.061 | -0.370 |
| cb_fanout_publish | 2.627 | 1.623 | -1.004 |
| rt_publish_view | 1.400 | 0.014 | -1.386 |
| rt_publish_cast | 1.081 | 1.573 | +0.492 |
| local_user_heard (new in PR 498, under cb_section_commit) | 0 | 0.803 | +0.803 |
| pub_deliver | 1.552 | 0.989 | -0.563 |
| j_game_commit | 0.965 | 0.993 | +0.028 |
| cb_journal_game_acquire | 1.009 | 1.102 | +0.093 |
| flush_now (sendto under it) | 5.762 (5.695) | 5.771 (5.720) | +0.009 |
| conn_io (recvfrom under it) | 1.605 (1.465) | 1.613 (1.474) | +0.008 |
| cb_server_inline_begin | 0.481 | 0.045 | -0.436 |
| cb_server_inline_done | 0.540 | 0.053 | -0.487 |
| cb_thread_cpu_us | 0.885 | 0.005 | -0.880 |
| __clock_gettime (clock_gettime as a symbol has no samples in either) | 1.041 | 0.155 | -0.886 |
| cb_poller_wait (epoll_pwait) | 0.557 | 0.532 | -0.025 |

Fact: the clock reads (PR 497) and the view build (PR 498) are gone from the loop's profile: cb_thread_cpu_us 0.885 to 0.005, rt_publish_view 1.400 to 0.014.
Fact: PR 498's own new check, local_user_heard, costs 0.803 us per move, and rt_publish_cast grew 0.49, so the fan-out area (cb_fanout_publish plus local_user_heard) went from 2.63 to 2.43 us per move.
Inference: that is why M3 vs M2 is flat on the nano: the view is no longer built, but deciding that it is not heard costs most of what it saved.
Fact: local_user_heard's self time is 0.107 us and user_conns.constprop.0 (str_hash inlined, by the symbolizer) 0.404 us self per move on the loop.

Top 25 self symbols, loop thread, user (us per move, share of loop user 7.33):

| us | share | symbol |
|--:|--:|---|
| 0.564 | 7.7% | strcmp |
| 0.553 | 7.5% | memcpy |
| 0.404 | 5.5% | user_conns.constprop.0 |
| 0.228 | 3.1% | strlen |
| 0.150 | 2.1% | memset |
| 0.125 | 1.7% | __pthread_mutex_unlock |
| 0.125 | 1.7% | pub_deliver |
| 0.123 | 1.7% | memchr |
| 0.117 | 1.6% | __pthread_mutex_lock |
| 0.115 | 1.6% | [unknown] |
| 0.113 | 1.5% | room_find |
| 0.107 | 1.5% | local_user_heard |
| 0.105 | 1.4% | ov_apply |
| 0.093 | 1.3% | sccp |
| 0.091 | 1.2% | cb_section_commit |
| 0.089 | 1.2% | cb_ws_send |
| 0.089 | 1.2% | conn_io |
| 0.086 | 1.2% | table_commit_products_flags |
| 0.074 | 1.0% | cb_loop_set |
| 0.073 | 1.0% | ws_frames |
| 0.072 | 1.0% | nw_frame_encode |
| 0.069 | 0.9% | pump |
| 0.069 | 0.9% | unpack_v2 |
| 0.067 | 0.9% | strcspn |
| 0.065 | 0.9% | __aarch64_ldadd8_relax |

Top 25 self symbols, loop thread, kernel (share of loop kernel 10.14):

| us | share | symbol |
|--:|--:|---|
| 1.865 | 18.4% | handle_softirqs |
| 0.293 | 2.9% | ena_start_xmit |
| 0.233 | 2.3% | el0_svc |
| 0.217 | 2.1% | fdget |
| 0.185 | 1.8% | __pi_memcpy_generic |
| 0.180 | 1.8% | ena_com_prepare_tx |
| 0.168 | 1.7% | sch_direct_xmit |
| 0.135 | 1.3% | arch_local_irq_restore |
| 0.134 | 1.3% | tcp_sendmsg_locked |
| 0.130 | 1.3% | __slab_free |
| 0.123 | 1.2% | __wake_up_sync_key |
| 0.121 | 1.2% | __tcp_transmit_skb |
| 0.115 | 1.1% | __dev_xmit_skb |
| 0.113 | 1.1% | sock_poll |
| 0.111 | 1.1% | __mod_timer |
| 0.108 | 1.1% | arch_counter_get_cntvct |
| 0.104 | 1.0% | selinux_socket_recvmsg |
| 0.094 | 0.9% | tcp_write_xmit |
| 0.092 | 0.9% | __arch_copy_to_user |
| 0.088 | 0.9% | sock_has_perm |
| 0.087 | 0.9% | __arch_copy_from_user |
| 0.082 | 0.8% | ip_finish_output2 |
| 0.080 | 0.8% | tcp_clean_rtx_queue |
| 0.079 | 0.8% | tcp_recvmsg_locked |
| 0.077 | 0.8% | tcp_established_options |

Top 25 self symbols, flusher (cbase_M3 tid 48248), user (share of flusher user 2.16):

| us | share | symbol |
|--:|--:|---|
| 0.544 | 25.2% | sqlite3VdbeExec |
| 0.125 | 5.8% | __pthread_mutex_unlock |
| 0.121 | 5.6% | sqlite3BtreeIndexMoveto |
| 0.117 | 5.4% | memcpy |
| 0.078 | 3.6% | __pthread_mutex_trylock_owner |
| 0.065 | 3.0% | pcache1Fetch |
| 0.051 | 2.4% | sqlite3BtreeInsert |
| 0.050 | 2.3% | memcmp |
| 0.049 | 2.3% | flush_txn |
| 0.049 | 2.3% | sqlite3BtreeTableMoveto |
| 0.047 | 2.2% | vdbeRecordCompareString |
| 0.040 | 1.9% | releaseMemArray.part.0 |
| 0.037 | 1.7% | walChecksumBytes |
| 0.032 | 1.5% | sqlite3VdbeSerialGet |
| 0.032 | 1.5% | trim |
| 0.030 | 1.4% | sqlite3DbMallocRawNN |
| 0.028 | 1.3% | getAndInitPage |
| 0.026 | 1.2% | sqlite3VdbeHalt |
| 0.026 | 1.2% | sqlite3VdbeMemGrow |
| 0.025 | 1.2% | sqlite3VdbeFreeCursorNN |
| 0.024 | 1.1% | pcache1TruncateUnsafe |
| 0.021 | 1.0% | getPageNormal |
| 0.019 | 0.9% | allocateCursor |
| 0.019 | 0.9% | moveToRoot |
| 0.019 | 0.9% | pcache1Unpin |

Top 10 self symbols, flusher, kernel (share of flusher kernel 1.52):

| us | share | symbol |
|--:|--:|---|
| 0.399 | 26.3% | handle_softirqs |
| 0.041 | 2.7% | ena_clean_tx_irq |
| 0.035 | 2.3% | finish_task_switch.isra.0 |
| 0.034 | 2.2% | tlb_flush |
| 0.033 | 2.2% | __slab_free |
| 0.032 | 2.1% | skb_release_data |
| 0.031 | 2.1% | __arch_copy_from_user |
| 0.021 | 1.4% | __pi_memcpy_generic |
| 0.019 | 1.3% | __wake_up_sync_key |
| 0.019 | 1.2% | el0_svc |

## 6. The list upkeep of PR 496 on the nano (threshold 0.3 us per move)

Source: scripts/upkeep.py perfM3 (analysis/upkeep_perfM3.txt), method of trip 1's srclines.py: every cbase_M3 leaf IP in the play window symbolized with inlined frames by llvm-symbolizer on bin/unstripped/cbase_M3 (load base recovered as 0x0; scripts/upkeep_check.py shows the remaining 3.5% name differences are aliases and inline names, e.g. __pthread_mutex_unlock against pthread_mutex_unlock).
Symbols at 59c115054: lru_in, ov_touch, rating_touch and rating_lru_in are their own functions; lru_out is inlined into ov_touch; wait_begin and wait_end are inlined into ov_apply, ov_flushed and ov_unlink and drew no sample.
A sample counts when its leaf symbol or an inlined frame at its IP is one of lru_out, lru_in, ov_touch, ov_link, wait_begin, wait_end, rating_lru_out, rating_lru_in, rating_touch, rating_link (self cycles; these functions call nothing else).

```
# upkeep perfM3: play window server mono [1544.522,1565.522], moves 1038864, pid 48241; base interval [-0x3, 0x0] -> base 0x0
# symbolizer outer frame differs from perf symbol on 3.53% of cbase_M3 cycles (base check)
list upkeep, all threads: 0.0509 us/move (1 run, 132284461 cycles)
loop                     thread  17.473 us/move  upkeep 0.0447 us/move  share 0.256%
job-0:48246              thread   4.322 us/move  upkeep 0.0000 us/move  share 0.000%
cbase_M3:48248           thread   3.675 us/move  upkeep 0.0000 us/move  share 0.000%
req-0:48245              thread   1.991 us/move  upkeep 0.0062 us/move  share 0.312%
cbase_M3:48247           thread   0.332 us/move  upkeep 0.0000 us/move  share 0.000%
-- loop by function
   0.0214  lru_out (in ov_touch)
   0.0120  ov_touch (in ov_touch)
   0.0095  lru_in (in lru_in)
   0.0009  rating_touch (in rating_touch)
   0.0009  rating_lru_in (in rating_lru_in)
-- loop by source line
   0.0129  journal.c:464:17 lru_out <- ov_touch
   0.0112  journal.c:482:8 ov_touch <- ov_touch
   0.0077  journal.c:462:47 lru_out <- ov_touch
   0.0043  journal.c:470:14 lru_in <- lru_in
   0.0026  journal.c:470:34 lru_in <- lru_in
   0.0009  journal.c:471:17 lru_in <- lru_in
   0.0009  journal.c:472:35 lru_in <- lru_in
   0.0009  journal.c:540:8 rating_touch <- rating_touch
   0.0009  journal.c:472:17 lru_in <- lru_in
   0.0009  journal.c:529:14 rating_lru_in <- rating_lru_in
   0.0009  journal.c:462:44 lru_out <- ov_touch
   0.0008  journal.c:481:19 ov_touch <- ov_touch
-- req-0:48245 by function
   0.0028  ov_touch (in ov_touch)
   0.0018  lru_in (in lru_in)
   0.0009  lru_out (in ov_touch)
   0.0008  rating_lru_out (in rating_touch)
-- req-0:48245 by source line
   0.0021  journal.c:481:19 ov_touch <- ov_touch
   0.0018  journal.c:470:14 lru_in <- lru_in
   0.0009  journal.c:462:47 lru_out <- ov_touch
   0.0008  journal.c:522:47 rating_lru_out <- rating_touch
   0.0007  journal.c:482:8 ov_touch <- ov_touch
resolution: one sample = 2502503 cycles = 0.00096 us/move over 1038864 moves
```

Fact: the list upkeep costs 0.045 us per move on the loop thread (0.26% of the loop's 17.47 us), 0.006 us per move on req-0, 0 on the flusher and job-0; 0.051 us per move in all.
Fact: that is below the 0.3 us per move threshold by a factor of about 6.
Fact: by line, most of it is the relink in ov_touch: lru_out's stores at journal.c:462 and 464 (0.022 us) and the in-place test at 482 (0.012 us); the walk-back loop at journal.c:470 is 0.007 us per move.
Fact: the resolution of this sample: one sample is 2.5e9/999 = 2.5 million cycles, 0.00096 us per move over 1,038,864 moves, so the loop's 0.045 us is about 47 samples (counting error about plus or minus 15%).
Inference: consistent with the Mac estimate in NOTES.md (0.03 to 0.08 us per move typical); the lists stay as they are.

## 7. The largest remaining costs on the loop thread (M3, facts, ranked by inclusive us per move)

Pure wrappers left out (cb_loop_run 17.47, ws_frames 12.07, pump 8.51, job_run_frame 7.88, h_action 7.69, cb_table_run 7.10); items nest where noted.

1. sendto (flush_now): 5.72 us per move, 1.84 calls per move (1.38 us of it is softirq taken inside sendto).
   Separately, the loop takes 2.26 us per move of softirq outside any syscall (interrupting its user code) and 0.29 inside recvfrom; handle_softirqs self is 1.87 us per move on the loop (analysis/perf_perfM3.txt, softirq section).
2. cb_section_commit: 5.06 (contains items 3, 6 and 8, and table_commit_products_flags 0.39, section_cast 0.22).
3. cb_fanout_publish: 1.62, almost all rt_publish_cast 1.57.
4. conn_io: 1.61, of which recvfrom 1.47 (1.06 calls per move).
5. cb_journal_game_acquire: 1.10 (take_game 0.59, ov_slot 0.34, ov_touch 0.04).
6. j_game_commit: 0.99 (loop_write 0.74).
7. pub_deliver: 0.99 (cb_ws_send 0.38, user_conns 0.38).
8. local_user_heard: 0.80.
Next: table_act 0.51, cb_poller_wait (epoll) 0.53, conn_try_parse 0.33.
Self symbols on the loop's user side are led by strcmp 0.56, memcpy 0.55 and user_conns 0.40 us per move.

## What was dropped or went wrong

- Nothing was dropped: all 15 counted runs, both count runs and the perf run ran, and the teardown started at minute 26.5.
- The drop logic of scripts/trip3.sh was never exercised.
- The steps.log quirk of trip 2 is kept on purpose: the post block of run X is logged after the header of run X+1; analyze.py reads by label.
- bpftrace slowed M1 by 38%, more than the expected 20%; only its counts are used.
- One D run (r1D) is below the warm-up and the other two (36.4k against 37.0k and 37.7k), within D's usual range.
- The 1.56 s slowest move in bpfM1 and 0.69 s in r3M2 are single moves; the p99 is unaffected.

## Files

- Scripts: scripts/build.sh, build_all.sh, up_trip3.sh, trip3.sh (driver), phase1.sh, phase2.sh, watchdog.sh, waitready.sh, leftovers.sh, analyze.py, extra.py, pushlat.py, perf3.py, before.py, upkeep.py, upkeep_check.py.
- Box side: pkg/srv.sh, cpu.sh, irq.sh (trip 2, unchanged), load.sh (trip 2 plus perf in the build), prof.sh (trip 1, binary as an argument, profiler stopped by its recorded PID), trip.bt (trip 1, unchanged).
- Raw: out/raw/srv, out/raw/load, out/raw/perf, out/raw/proc/perfM3; logs out/steps.log, out/phase1.txt, out/phase2.txt, out/launch.txt, out/teardown.txt.
- Before the trip: state.env.before (grav/state.env as it was).

## Teardown proof (out/teardown.txt, last block)

```
== trip3 leftover proof 2026-11-14T19:00:15Z
instances purpose=cbase-mem-test pending/running/stopping/stopped:        0
trip instances state: i-REDACTED10 terminated i-REDACTED11 terminated 
volumes purpose=cbase-mem-test:        0
security groups purpose=cbase-mem-test:        0
IAM roles cbase-mem-test-*:        0
instance profiles cbase-mem-test-*:        0
S3 buckets cbase-mem-test-*:        0
production i-REDACTED01: running
```

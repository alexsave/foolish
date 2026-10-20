# Final rig measurement: `cbase serve` before and after the perf rounds

Date: 2026-10-20. Docker arm64 rig (c_base/bench/nano scripts), on this Mac only, with no AWS.
Status: the three rounds of the rig matrix are complete (45 runs).
Alex paused the job when round 3 ended, so the count-build runs and the macOS `sample` profiles were not run (see "Not done").

## Builds

| name | commit | config |
|---|---|---|
| BASE | 5d5ef505b (main and the production release before the rounds) | no journal; SQLite on every move; epoll (its log line names no backend) |
| NOW-epoll | 4aa1c22e3 (current main) | default: move journal (mmap), one owner, inline moves; epoll |
| NOW-uring | 4aa1c22e3 | `CBASE_IO_URING=1`; every run's log says `listening on ... (io_uring)` |

Both builds are static alpine arm64 builds, the same as the previous rounds (`final/build.sh`).
They were built from detached worktrees `final/wt_base` and `final/wt_now`.
Every run used `CBASE_REQ_WORKERS=1`.

### io_uring and seccomp

Docker's default seccomp profile blocks io_uring.
With it, `io_uring_setup` returns EPERM and the server logs `io_uring asked for and not used (io_uring_setup: Operation not permitted): epoll`.
That is the fallback the previous agent saw.
`--security-opt seccomp=unconfined` and `--privileged` both let io_uring run (probed directly).
The rig's server container already runs `--privileged` (it loop-mounts the data disk), so io_uring really ran in the rig.
`fin.py` reads the backend from the log line of every run, and it also flags any "not used" warning (`uring_fallback`).
All 15 NOW-uring runs say `io_uring`, and none fell back.

## Methodology (and what changed in the scripts)

The scripts are in `scratchpad/final/`: `fr.sh` (one run), `fin.py` (one run's numbers), `agg.py` (the tables) and `matrix.sh` (the order of runs).
The rig copy, with `cgsamp.sh`, the patched `summ.py`, and `step3.sh`, is in `final/rig/`.
Raw data: `final/rig/runs/<label>` (client event files), `<label>_m/` (perf intervals and cgroup samples), `<label>_srv.txt` (server log lines), `<label>_step.txt`.
`final/results_final.txt` holds one line per run, and `final/tables.md` holds the aggregate.

Changes from the earlier rig (q.sh, rq.sh and win.py), following the verdicts in Fable's analysis:

- **The human-pace window starts after setup.** It starts 0.5 s after the last game's first move, so every create, join and start is outside it (setup took 3.7-4.1 s in 2p and 8p, and 6.1-6.9 s in the bot runs). It ends 2 s before the clients' deadline, for about 37 s of measured window. The earlier window opened 8 s after release with a fixed offset.
- **PLAY_CPUS=1.0 in every scenario**, human pace included (it was 0.1). Sign-up still runs at 1.6 CPUs.
- **The measurement is post-hoc and covers the middle of the run.** `perf stat -I 1000 -x, -e task-clock,raw_syscalls:sys_enter` runs on the server process from release to the end. The window is then cut from those 1 s intervals, so it covers the middle of the run and never the setup or the tail.
- **Server CPU per move = server cgroup CPU over the window ÷ applied moves in the window.** A new sampler (`cgsamp.sh`) reads `usage_usec` from the server's cgroup every 0.25 s. It runs in its own container with `--cgroupns=host`, so its cost is not charged to the server. The earlier figure was 100 Hz utime+stime ticks, or task-clock with setup inside the window. The perf task-clock figure is reported next to it, and the two agree within 1-7%.
- **A `nan` can no longer appear silently.** Every division is guarded, and the move count, the CPU and the window are printed beside it. Saturation now uses 192 pairs; it used 8 pairs, and those runs were client-bound at about 0.53 cores. In every saturation run here the server cgroup is pinned at 0.93-1.0 cores.
- **Refused moves are not counted as moves.** The client writes `X` just before the `M` of a refused attempt. The earlier scripts counted refused attempts as moves (about 8% at 2p saturation and 13% at 8p).
- **Lost pushes are defined properly now.** The old count flagged two client artifacts as lost pushes: a game's last move logged under the next game's id, and moves in the last instant before a receiver hit its deadline. The trial run showed 1041 false "lost" pushes this way. A push now counts as lost only if the game went on (a later applied move at a higher version), no other viewer ever received a later version, and the move ended at least 2 s before the deadline. I mutation-checked this: deleting one receiver's receipts from version 6 on gives `lost_push=9`, and the real data gives 0.
- **Push latency counts every receiver.** It is measured from the move's start to each other viewer's first receipt of a later version, seats and spectators alike (the earlier scripts counted only the first receiver).
- **The rig's `summ.py` push matching is now bisect instead of quadratic.** The quadratic version took about 8 minutes per 8-player saturation run. The results are unchanged (checked on a 2p run).
- **New disk metric: data-disk bytes written per move.** This is the server's loop device sectors written × 512, from release to after shutdown plus `sync`, over every applied move of the run. The metric covers journal, WAL, DB checkpoints and ext4 metadata, after page-cache coalescing.
- **Journal bytes and WAL frames come from NOW's shutdown log line** (whole run). WAL bytes = frames × 4120. BASE has no such counter, so its WAL cannot be separated from its disk total.
- **Labels are unique per run** (`f<scen>_<build>[u]_<epoch>`), to avoid Docker's stale-directory views.

Caveat on human-pace CPU: **the human-pace µs/move numbers are inflated by the Docker VM running at a low clock at about 1% utilization.** At that load, macOS gives the VM's vCPU little clock (and possibly an efficiency core), so each instruction costs 2-3x what it costs at saturation. Use the human-pace numbers to compare builds with each other, never as the cost of a move. The saturation numbers are the per-move cost.

Scenarios. Each build ran each scenario three times. The order was interleaved (round → scenario → BASE, NOW-epoll, NOW-uring), so drift spreads across all three builds.

| key | clients | think | duration |
|---|---|---|---|
| hum2 | 192 two-player games (384 clients) | 2-4 s | 45 s |
| sat2 | 192 two-player games | 0-1 ms | 25 s (window 6 s after go to 2 s before end) |
| hum8 | 48 eight-player tables + 3 spectators each (528 clients) | 2-4 s | 45 s |
| sat8 | the same | 0-1 ms | 25 s |
| bot | 192 tables, 1 human (2-4 s) + 1 octogen bot, `CBASE_BOT_SPEEDUP=100` (the client's existing `P3BOT`/botg.sh mode) | 2-4 s human | 45 s |

The tables give the median, with [min-max] over the 3 runs.
Mallocs/move exist for only one run: a trial run of the NOW count build at sat2 (2.07). The BASE count build and the rest were not run (see "Not done").

## Headline (median of 3)

| | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| sat2 server µs/move | 58.8 | 47.6 (-19%) | 36.1 (-39%) |
| sat2 moves/s at 1 core | 16 933 | 20 882 | 27 537 |
| sat2 syscalls/move | 20.96 | 8.64 | 4.23 |
| sat2 move p95 / p99 ms | 48.6 / 55.4 | 23.9 / 31.8 | 19.7 / 31.1 |
| sat8 server µs/move | 94.0 | 56.3 (-40%) | 43.9 (-53%) |
| sat8 moves/s | 10 608 | 16 634 | 21 276 |
| sat8 move p95 ms | 41.1 | 8.4 | 8.8 |
| hum2 server µs/move (VM-clock inflated) | 176.0 | 76.9 | 78.8 |
| hum2 syscalls/move | 21.26 | 3.96 | 1.98 |
| hum2 move p50 / p99 ms | 0.266 / 0.647 | 0.145 / 0.405 | 0.152 / 0.412 |
| hum8 server µs/move (inflated) | 390.4 | 220.9 | 218.6 |
| data-disk bytes/move, sat2 | 11 747 | 1 228 | 1 233 |
| data-disk bytes/move, hum2 | 11 980 | 1 437 | 1 489 |
| server RSS, sat2 (MB) | 14.1 | 65.3 | 69.4 |
| server RSS, hum2 (MB) | 10.6 | 12.5 | 12.9 |
| lost pushes (all scenarios) | 0 | 0 | 0 |

## Tables per scenario

### 1. Human pace, 192 two-player games

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 176.0 [172.1-177.4] | 76.9 [73.8-77.8] | 78.8 [73.8-78.9] |
| server µs/move (perf task-clock) | 164.5 [159.8-164.8] | 72.1 [69.4-73.0] | 74.0 [69.3-74.5] |
| server cores used | 0.017 | 0.007 [0.007-0.008] | 0.008 [0.007-0.008] |
| syscalls/move | 21.26 [21.19-21.40] | 3.96 [3.96-3.97] | 1.98 [1.96-1.98] |
| moves/s | 97 [97-97] | 97 [97-98] | 98 [98-98] |
| moves in window | 3606 [3587-3611] | 3602 [3594-3641] | 3625 [3622-3626] |
| move p50 ms | 0.266 [0.266-0.268] | 0.145 [0.139-0.146] | 0.152 [0.149-0.157] |
| move p95 ms | 0.484 [0.472-0.523] | 0.295 [0.275-0.295] | 0.301 [0.286-0.312] |
| move p99 ms | 0.647 [0.640-0.959] | 0.405 [0.394-0.433] | 0.412 [0.411-0.453] |
| push p50 ms | 0.263 [0.263-0.266] | 0.160 [0.153-0.162] | 0.151 [0.148-0.157] |
| push p95 ms | 0.461 [0.460-0.480] | 0.296 [0.278-0.296] | 0.283 [0.265-0.285] |
| push max ms | 3.9 [1.9-10.4] | 1.7 [1.1-5.0] | 2.0 [0.5-3.1] |
| move max ms | 3.9 [2.0-10.5] | 1.9 [1.4-3.0] | 2.0 [1.9-3.1] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 10.6 [10.6-10.7] | 12.5 [12.3-12.5] | 12.9 [12.9-13.0] |
| server RSS high-water (MB) | 10.6 [10.6-10.7] | 12.5 [12.4-12.5] | 12.9 [12.9-13.0] |
| data-disk bytes written/move (whole run, after sync) | 11980 [11658-12733] | 1437 [1423-1442] | 1489 [1460-1553] |
| journal bytes/move (whole run) | - | 217 [217-218] | 217 |
| WAL bytes/move (frames x 4120, whole run) | - | 4684 [4666-4708] | 4731 [4705-4780] |
| refused moves in window | 30 [23-31] | 20 [19-27] | 21 [14-24] |
| games finished | 0 | 0 | 0 |
| window s | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] |
| setup s (before window) | 4.0 [3.9-4.1] | 3.9 [3.8-4.0] | 4.0 [4.0-4.0] |

### 2. Saturation, 192 two-player games

runs: BASE 3 (+0 count), NOW-epoll 3 (+1 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 58.8 [58.3-59.3] | 47.6 [45.3-50.0] | 36.1 [35.8-45.8] |
| server µs/move (perf task-clock) | 59.2 [58.6-59.6] | 45.3 [43.5-47.6] | 36.3 [35.9-44.4] |
| server cores used | 0.995 [0.994-0.996] | 0.994 [0.990-0.995] | 0.993 [0.992-0.994] |
| syscalls/move | 20.96 [20.93-20.99] | 8.64 [7.90-8.86] | 4.23 [4.16-6.05] |
| mallocs/move (count build) | - | 2.07 | - |
| moves/s | 16933 [16763-17100] | 20882 [19793-21956] | 27537 [21668-27748] |
| moves in window | 271548 [268791-274339] | 334699 [317292-352059] | 441721 [347376-444741] |
| move p50 ms | 8.640 [8.637-8.843] | 11.038 [10.168-11.346] | 7.194 [7.074-10.606] |
| move p95 ms | 48.562 [48.491-49.069] | 23.907 [23.162-24.596] | 19.671 [19.628-25.484] |
| move p99 ms | 55.375 [53.822-57.870] | 31.804 [31.276-36.682] | 31.129 [30.807-33.655] |
| push p50 ms | 8.567 [8.566-8.763] | 11.093 [10.474-11.406] | 6.349 [6.240-10.167] |
| push p95 ms | 48.367 [48.330-48.907] | 23.543 [22.887-24.310] | 18.993 [18.839-24.666] |
| push max ms | 1155.1 [1110.0-1427.2] | 1747.5 [1723.3-2042.3] | 1777.9 [1710.2-1839.4] |
| move max ms | 96.8 [92.7-104.2] | 149.5 [123.8-208.6] | 143.3 [119.6-188.7] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 14.1 [14.0-14.2] | 65.3 [64.5-65.8] | 69.4 [68.6-69.6] |
| server RSS high-water (MB) | 14.2 [14.1-14.3] | 65.8 [65.6-65.8] | 69.4 [69.3-69.9] |
| data-disk bytes written/move (whole run, after sync) | 11747 [11732-11760] | 1228 [1224-1228] | 1233 [1233-1234] |
| journal bytes/move (whole run) | - | 59 [58-59] | 58 |
| WAL bytes/move (frames x 4120, whole run) | - | 1024 [1012-1027] | 1020 [1015-1025] |
| refused moves in window | 26636 [26409-27023] | 25532 [25148-26218] | 28254 [26890-28984] |
| games finished | 4104 [3975-4112] | 4542 [4466-4671] | 5604 [5576-5653] |
| window s | 16.0 | 16.0 | 16.0 [16.0-16.0] |

### 3a. Human pace, 48 eight-player tables, 3 spectators each

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 390.4 [384.3-401.2] | 220.9 [219.6-223.0] | 218.6 [198.6-220.7] |
| server µs/move (perf task-clock) | 378.5 [371.7-390.1] | 217.2 [214.7-218.4] | 215.4 [195.3-217.0] |
| server cores used | 0.032 [0.031-0.033] | 0.018 | 0.018 [0.016-0.018] |
| syscalls/move | 34.58 [33.87-35.03] | 12.39 [12.37-12.48] | 2.01 |
| moves/s | 82 [81-82] | 81 [81-81] | 81 [80-82] |
| moves in window | 3028 [3017-3035] | 3008 [3007-3014] | 2992 [2984-3044] |
| move p50 ms | 0.447 [0.444-0.447] | 0.194 [0.188-0.195] | 0.265 [0.262-0.270] |
| move p95 ms | 0.989 [0.943-1.028] | 0.429 [0.412-0.473] | 0.611 [0.574-0.633] |
| move p99 ms | 1.445 [1.357-1.516] | 0.743 [0.688-0.810] | 0.946 [0.900-1.022] |
| push p50 ms | 0.421 [0.418-0.422] | 0.274 [0.269-0.274] | 0.270 [0.264-0.276] |
| push p95 ms | 0.894 [0.849-0.971] | 0.596 [0.578-0.648] | 0.618 [0.548-0.649] |
| push max ms | 5.1 [4.3-12.2] | 3.1 [3.0-6.3] | 4.0 [2.8-4.2] |
| move max ms | 5.0 [4.2-12.1] | 2.8 [1.2-3.9] | 2.8 [2.4-2.8] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 11.9 [10.7-12.0] | 14.2 [14.0-14.2] | 14.2 [14.2-14.6] |
| server RSS high-water (MB) | 11.9 [10.8-12.0] | 14.2 [14.0-14.2] | 14.2 [14.2-14.6] |
| data-disk bytes written/move (whole run, after sync) | 10722 [10373-12677] | 1618 [1555-1736] | 1618 [1539-1792] |
| journal bytes/move (whole run) | - | 325 [324-329] | 326 [323-329] |
| WAL bytes/move (frames x 4120, whole run) | - | 7139 [7032-7184] | 7084 [6965-7257] |
| refused moves in window | 33 [28-35] | 22 [15-25] | 25 [22-29] |
| games finished | 0 | 0 | 0 |
| window s | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] |
| setup s (before window) | 3.9 [3.7-4.0] | 3.8 [3.8-3.9] | 3.9 [3.9-3.9] |

### 3b. Saturation, 48 eight-player tables, 3 spectators each

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 94.0 [93.8-95.7] | 56.3 [55.1-57.7] | 43.9 [42.8-45.7] |
| server µs/move (perf task-clock) | 95.0 [94.8-96.7] | 56.7 [55.5-58.1] | 45.9 [44.8-47.9] |
| server cores used | 0.997 | 0.943 [0.936-0.949] | 0.933 [0.933-0.935] |
| syscalls/move | 24.86 [24.64-25.11] | 6.59 [6.58-6.61] | 2.15 [2.11-2.17] |
| moves/s | 10608 [10419-10623] | 16634 [16449-17120] | 21276 [20467-21796] |
| moves in window | 170084 [167041-170235] | 266697 [263914-274511] | 341389 [328359-350167] |
| move p50 ms | 8.190 [8.166-8.401] | 4.626 [4.469-4.633] | 5.755 [5.619-5.866] |
| move p95 ms | 41.108 [40.392-41.274] | 8.438 [7.976-8.589] | 8.773 [8.741-9.489] |
| move p99 ms | 46.448 [46.368-46.487] | 11.117 [9.963-11.601] | 11.556 [11.330-13.359] |
| push p50 ms | 8.062 [8.044-8.281] | 6.689 [6.601-6.709] | 5.559 [5.440-5.664] |
| push p95 ms | 40.782 [40.143-40.890] | 12.293 [11.858-12.471] | 8.385 [8.373-9.022] |
| push max ms | 2110.8 [1984.1-2696.4] | 2274.8 [2066.4-2373.4] | 1596.0 [1550.7-1721.0] |
| move max ms | 80.9 [54.7-87.7] | 106.9 [98.9-109.5] | 126.8 [118.9-215.0] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 15.4 [15.0-15.5] | 41.7 [41.7-43.9] | 49.8 [49.0-49.9] |
| server RSS high-water (MB) | 15.7 [15.4-15.8] | 48.2 [47.2-49.1] | 55.4 [53.2-56.7] |
| data-disk bytes written/move (whole run, after sync) | 10623 [10600-10628] | 462 [458-479] | 475 [471-486] |
| journal bytes/move (whole run) | - | 59 | 60 [59-60] |
| WAL bytes/move (frames x 4120, whole run) | - | 416 [412-418] | 423 [422-425] |
| refused moves in window | 32220 [31946-32784] | 40206 [40095-41361] | 48512 [47506-49605] |
| games finished | 487 [477-495] | 754 [749-779] | 1005 [975-1029] |
| window s | 16.0 | 16.0 [16.0-16.0] | 16.1 [16.0-16.1] |

### 4. 192 games, one human (2-4 s think) against one octogen bot, bot timers / 100

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs per table version (human+bot moves, cgroup) | 9835.9 [9756.6-9844.2] | 9837.7 [9828.7-9939.1] | 9830.2 [9728.8-9989.4] |
| server µs per table version (perf) | 9829.9 [9751.4-9838.5] | 9836.2 [9827.3-9933.5] | 9825.0 [9724.3-9984.5] |
| server cores used | 0.999 | 0.998 [0.997-0.998] | 0.998 [0.997-0.998] |
| syscalls per table version | 22.64 [22.61-22.72] | 6.07 [6.04-6.13] | 5.08 [5.05-5.09] |
| human moves/s | 48 [48-49] | 48 [48-49] | 48 [48-48] |
| human moves in window | 1703 [1650-1709] | 1678 [1667-1701] | 1694 [1647-1697] |
| move p50 ms | 0.162 [0.161-0.170] | 0.092 [0.091-0.093] | 0.090 [0.088-0.090] |
| move p95 ms | 0.348 [0.320-0.354] | 0.198 [0.193-0.198] | 0.194 [0.183-0.194] |
| move p99 ms | 0.545 [0.497-0.589] | 0.279 [0.271-0.281] | 0.280 [0.239-0.290] |
| move max ms | 2.1 [1.6-3.5] | 1.2 [0.6-2.0] | 3.4 [0.7-18.1] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 10.6 [10.6-10.6] | 11.1 [11.1-11.2] | 11.7 [11.6-11.7] |
| server RSS high-water (MB) | 10.6 [10.6-10.7] | 11.2 [11.1-11.2] | 11.7 [11.7-11.7] |
| data-disk bytes written/move (whole run, after sync) | 26556 [26268-27718] | 2516 [2513-2544] | 2577 [2570-2614] |
| journal bytes/move (whole run) | - | 299 [299-300] | 300 [299-300] |
| WAL bytes/move (frames x 4120, whole run) | - | 5145 [5132-5166] | 5161 [5135-5203] |
| refused moves in window | 0 [0-1] | 0 | 0 |
| games finished | 0 | 0 | 0 |
| window s | 35.1 [34.1-35.1] | 35.1 [34.1-35.1] | 35.1 [34.1-35.1] |
| setup s (before window) | 6.3 [6.3-6.9] | 6.5 [6.4-6.5] | 6.2 [6.1-6.6] |

## Profiles

Not taken.
Native builds of both commits exist (`final/wt_base/c_base/build/cbase` and `final/wt_now/c_base/build/cbase`), along with `final/mac_prof.sh`.
`final/samp2.py` is also ready: it gives the top-N functions by self samples, plus the heaviest function+offset instructions of the top 3, which can be disassembled with `otool -tV`.
The pause came before they ran.

## Odd things

1. **Slow game-end pushes at saturation got worse in NOW.** Push receipts over 100 ms per sat2 run: BASE 2-9, NOW-epoll 566-787, NOW-uring 855-915 (out of about 0.5 M). In sat8: BASE 30-70, NOW-epoll 2520-2580, NOW-uring 3710-3970 (out of 2.6-5.5 M); more than 1 s: BASE 30-70, NOW 1720-1924.
   About 95% of these receipts are on the move that ended a game, matched by its version against the game ends that table's clients logged (`final/pushmax.py`, `final/pushtail.txt`).
   Example: the final move of a game is acknowledged to its mover in 30 ms, but the other 10 viewers receive that version 2.78 s later, all at once, together with the next game's versions.
   This looks like the end-of-game work (the end goes around the journal, plus the replay job) waiting behind the single pool worker while the loop is saturated. This is a guess and was not traced.
   It shows only at saturation, and only in the max column: p95 and p99 improved a lot. At human pace no game ends in the 45 s, so these runs do not exercise game ends.
2. **NOW's RSS grows at saturation:** 65-70 MB in NOW against 14 MB in BASE (sat2), and 42-50 MB against 15 MB (sat8). At human pace the gap is only about 2 MB. This is probably the mapped journal segments (8.4 MB `cbase.sqlite-journal.*` files) plus resident games, but it was not attributed with smaps.
3. **sat2 NOW-uring is bimodal.** Two runs measured 35.8-36.1 µs/move at 27.5-27.7 k moves/s, and one measured 45.8 µs at 21.7 k, which is about the epoll figure. In that run, syscalls/move were 6.05 against 4.2. The cause was not investigated.
4. **The bot scenario measures the bot, not the move path.** With octogen and `CBASE_BOT_SPEEDUP=100`, the server cgroup is pinned at 1.0 core in every build, at about 9.8 ms of server CPU per table version: the bots' Monte Carlo search. So the builds cannot be told apart on CPU there, and only syscalls (22.6 → 6.1 → 5.1), human-move latency (p50 0.16 → 0.09 ms) and disk bytes show a difference.
   A useful bot scenario needs a cheap bot (handwritten) or a few tables at a low speedup.
5. **NOW-uring is slightly worse at human pace in 8p:** move p50 0.265 ms against 0.194 ms for epoll. It makes 6x fewer syscalls (2.0 against 12.4), and its CPU is the same.
6. **At saturation NOW's move p50 is higher than BASE's in 2p** (11.0 ms against 8.6 ms), while p95 and p99 are about halved. NOW moves 23% more per second through the same core, and all 384 clients pipeline. This is a queueing effect, not slower moves.
7. **At human pace NOW's SQLite is written only at shutdown:** "1 flush(es) of N moves" with the close's flush of all of them. So NOW's disk bytes at human pace are almost all the final flush. With longer runs, the periodic flush would show up in the numbers.
8. **The data-disk bytes are lower than NOW's WAL bytes** (1.2-1.6 KB against 4.7-7.1 KB at human pace) because ext4 writeback coalesces repeated rewrites of the same WAL pages before they reach the device.
9. **summ.py's `games_over` is always 0 for step3 runs**, because it reads `a*.err` but step3 names the files `A*.err`. `fin.py` counts from `A*.err` instead.

## Not done (paused by Alex after round 3)

- Count-build runs, for mallocs/move: BASEc, NOWc and NOWc+uring in every scenario. Only one trial run exists: NOWc sat2 = 2.07 mallocs/move. Command: `COUNT=1 final/fr.sh <BASEc|NOWc> <scen> [u]`; the binaries are built (`final/rig/bin/serve-{BASEc,NOWc}`).
- macOS `sample` profiles at saturation for BASE and NOW-epoll (native builds and scripts ready, see "Profiles").
- Removing the worktrees `final/wt_base` and `final/wt_now`. They are kept on purpose for the profiles; remove them with `git worktree remove` when the job is finished.

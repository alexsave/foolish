# Trip 1 results: where the server CPU per move goes at 192 games, and what bounds throughput

Real t4g.nano server, c7g.2xlarge load box, build 9c5057b64 (stripped cbase_D, sha256 4eae8437...b7), knob auto (loop on CPU 0, others on CPU 1), `load.sh run LABEL IP 192 0 1 100000 25000`, fresh server and DB per run.
Date 2026-11-14.
Box time: run-instances 14:11:53Z and 14:11:56Z, terminated by 14:29:38Z (down.sh), about 17.6 minutes for both boxes.
Teardown proof: out/teardown.txt (zero leftovers, production i-REDACTED01 running).
All paths below are relative to foolish/docs/memtest/nano-2026-11/campaign/trip1/.

## Headline

Fact: the loop thread is NOT CPU-bound.
CPU 0 is pure-idle 20.5% of the play window (cpu-clock profile), and the loop thread is asleep 19% of the window: 13.6% in futex, 5.4% in epoll_pwait (bpftrace).
Fact: the futex sleeps of the loop come from `__pthread_mutex_timedlock` in cb_journal_game_acquire (then cb_journal_game_release and write_batch), whose source takes `j->ov_mu`.
Fact: the flusher thread holds ov_mu while it runs ov_evict and snapshot_take, which cost it about 3.8 us per move (qsort of every idle resident game with touched_cmp 2.41 us, the ov_evict bucket walk 0.78 us, flush_due/snapshot_take 0.58 us).
Inference: the loop waits on ov_mu held by the flusher during ov_evict's qsort; that wait is the largest single throughput bound found (about 4.4 us of loop wall time per move, 142 us per sleep).
Fact: network softirq is the second bound: the share of ENA queue interrupts that irqbalance put on CPU 0 tracks throughput run by run (0% gives 45.5k moves/s, 19% gives 40.1k, about 50% gives 37.5k, 100% gives 34.6k).
Fact: the load box is not the limit: 27-43% of its 8 vCPUs busy during play, the busiest vCPU at most 43.5%.

## 1. Runs

Source: out/steps.log (on-box summaries), out/analysis/runs.txt (scripts/runs.py), out/analysis_moves.txt (scripts/analyze.py moves, recounted offline from the ev.* M lines in raw/load/LABEL.tgz; identical to the on-box counts).
Play window = [t_go+2 s, t_go+23 s] on the load box clocks (t_go_clk: epoch and CLOCK_MONOTONIC taken together); moves counted by t_reply_us in that window.
The E1-compatible window is t_go to t_go+window_s (about 25 s).

| run | profiler | moves/s E1 window | moves/s play | p50 ms | p99 ms | err_files | server CPU us/move (play, /proc ticks) | VmHWM kB | RSS max kB | ENA queue IRQs on CPU 0 |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| base1 | none | 40,779 | 40,091 | 3.21 | 12.02 | 0 | 34.73 | 30,200 | 29,920 | 19% |
| perfcy | perf cycles | 35,564 | 35,089 | 4.07 | 13.38 | 0 | 37.58 | 30,764 | 30,684 | 44% |
| perfclk | perf cpu-clock | 37,344 | 36,836 | 4.34 | 12.93 | 0 | 37.35 | 29,508 | n/a | 48% |
| bpf1 | bpftrace | 28,695 | 29,028 | 5.23 | 13.94 | 0 | 41.52 | 27,604 | 27,520 | 50% |
| base2 | none | 37,692 | 37,463 | 4.45 | 12.72 | 0 | 37.09 | 30,096 | 30,008 | 50% |
| irq1 (hint, single run) | none, all ENA IRQs on CPU 1 | 46,293 | 45,516 | 3.19 | 10.04 | 0 | 34.24 | 31,252 | 31,088 | 0% |
| irq0 (hint, single run) | none, all ENA IRQs on CPU 0 | 34,797 | 34,623 | 5.03 | 12.76 | 0 | 37.19 | 30,820 | 30,560 | 100% |

The IRQ column is from out/analysis/irqs.txt (scripts/irqs.py, /proc/interrupts start and end snapshots of each run): the share of ens5-Tx-Rx-0 and -1 interrupts counted on CPU 0.
"n/a": the RSS sampler line for perfclk did not parse in steps.log; raw/srv/rss_perfclk.log has it.
Server log, every run: valve tripped 0 times, 0 over budget, and 0.09-0.17% of requests sent on to the pool (base1: 1,027,558 requests on the loop thread, 1,682 sent on to the pool; raw/srv/server_LABEL.log).
The build prints "inline dispatch: N requests on the loop thread (M sent on to the pool), 0 over budget, valve tripped 0 times", not ran/to_pool/cpu_timed fields.

Per-thread CPU, base1 play window (schedstat on-CPU ns, out/analysis/runs.txt):

| thread | CPU | on-CPU us/move | run-queue wait us/move | vol / invol switches per move |
|---|---|--:|--:|--:|
| loop (main, tid = pid) | 0 | 17.27 | 0.49 | 0.0427 / 0.0018 |
| flusher (unnamed cbase_D, `flusher`/flush_pass) | 1 | 9.18 | 4.72 | 0.0080 / 0.0127 |
| job-0 (finalize) | 1 | 4.97 | 5.58 | 0.0063 / 0.0149 |
| req-0 (HTTP pool) | 1 | 2.97 | 6.68 | 0.0413 / 0.0015 |
| WAL checkpointer (unnamed cbase_D, `ck_main`) | 1 | 0.36 | 0.42 | 0.0013 / 0.0002 |

base2 gives loop 20.33, flusher 9.79, job-0 4.35, req-0 2.29, checkpointer 0.33 us/move.
Per-CPU /proc/stat shares, base1 play window: CPU 0 busy 72.8% (user 35.4, system 31.9, softirq 5.4, steal 0.0), idle 26.8%; CPU 1 busy 79.0% (user 43.0, system 8.1, softirq 27.9, steal 0.0), idle 13.6%, iowait 7.4%.
base2: CPU 0 busy 80.0% (softirq 16.2), idle 19.8%; CPU 1 busy 72.3% (softirq 20.3), idle 25.5%.
Steal is 0.0% on both CPUs in every run.

Drift and profiler overhead.
Fact: base1 and base2 differ by 6.6% (40.1k vs 37.5k) with the same binary and settings.
Inference: the difference is the IRQ placement (19% vs 50% of queue IRQs on CPU 0), not drift; base2's loop takes 3.1 us/move more on-CPU than base1's, matching the extra softirq on CPU 0.
Against base2 (similar IRQ split): perf cycles costs 6.3% of moves/s, perf cpu-clock 1.7%, bpftrace 22.5%.
Against the base1/base2 mean (38.8k): perfcy -9.5%.

## 2. Cycles per move (perfcy, play window)

Source: out/analysis/perf_perfcy.txt (scripts/analyze.py perf perfcy, from raw/proc/perfcy/cc_perfcy.txt.gz), 35,889 samples, 736,858 moves, us = cycles / 2.5 GHz / moves.
User symbols resolved 100% (resolve_perfcy.txt: /opt/mt/cbase_D 71.2% of samples, unknown_within 0.0000), through the unstripped twin of the same build id.

Total sampled: 37.34 us/move (user 18.77, kernel 18.57), CPU 0 19.76 (user 8.23, kernel 11.53), CPU 1 17.58 (user 10.54, kernel 7.04).

| thread | user | kernel | total | of which softirq | of which syscall |
|---|--:|--:|--:|--:|--:|
| loop (CPU 0) | 7.97 | 10.66 | 18.63 | 3.85 | 6.27 |
| flusher (CPU 1) | 6.29 | 2.11 | 8.40 | 1.37 | 0.53 |
| job-0 (CPU 1) | 2.89 | 1.46 | 4.35 | 1.12 | 0.26 |
| req-0 (CPU 1) | 1.20 | 0.97 | 2.17 | 0.59 | 0.34 |
| checkpointer (CPU 1) | 0.03 | 0.25 | 0.28 | 0.06 | 0.18 |
| idle task (softirq taken in idle) | 0 | 2.44 | 2.44 | 2.02 | 0 |
| others (bash sampler, sleep, kworkers, kswapd) | 0.39 | 0.65 | 1.04 | | |

Comparison with /proc in the same run: the server threads sum to 33.83 us/move of cycles against 37.57 us/move of schedstat on-CPU time (loop 18.63 vs 20.81, flusher 8.40 vs 9.15, job-0 4.35 vs 4.68, req-0 2.17 vs 2.61).
Unsure: cycles come out about 10% below on-CPU time on every thread; either the effective cycle rate is about 2.25 GHz, or cycles stop counting in some states under the hypervisor PMU; I did not resolve which.
Softirq total (all contexts, both CPUs): about 9.0 us/move of the 37.3 (24%).
Softirq charged inside loop syscalls on CPU 0: sendto 1.19, recvfrom 0.24, epoll_pwait 0.21, clock_gettime 0.09 us/move; on the loop outside a syscall (an interrupt from user mode): 2.02 us/move.
On CPU 1: the flusher 1.20, job-0 1.01, req-0 0.44 us/move outside syscalls, and 1.70 us/move in the idle task.
Caveat (inference): arm64 without pseudo-NMI masks the PMU interrupt while IRQs are off, so samples in hardirq and IRQ-off regions land where IRQs come back on; `handle_softirqs` self time (1.81 us/move on the loop) is mostly that, not the function's own code.

## 3. Top symbols

Loop thread user, self (us/move, share of loop user):

| us | share | symbol |
|--:|--:|---|
| 0.682 | 8.6% | strcmp |
| 0.641 | 8.1% | memcpy |
| 0.497 | 6.2% | pub_deliver |
| 0.257 | 3.2% | strlen |
| 0.242 | 3.0% | memset |
| 0.209 | 2.6% | [unknown] |
| 0.176 | 2.2% | state_put_rules |
| 0.133 | 1.7% | __pthread_mutex_unlock |
| 0.132 | 1.7% | __pthread_mutex_lock |
| 0.127 | 1.6% | memchr |
| 0.122 | 1.5% | cb_ws_send |
| 0.105 | 1.3% | room_find |
| 0.099 | 1.2% | conn_io |
| 0.095 | 1.2% | cb_section_commit |
| 0.089 | 1.1% | ws_frames |

Loop thread kernel, self: handle_softirqs 1.810 (17.0%, see the caveat), el0_svc 0.412, ena_start_xmit 0.281, task_sched_runtime 0.278 (the CLOCK_THREAD_CPUTIME_ID reads), fdget 0.197, sch_direct_xmit 0.171, ena_com_prepare_tx 0.170, tcp_sendmsg_locked 0.161, __tcp_transmit_skb 0.155, __mod_timer 0.137, __pi_memcpy_generic 0.137, __slab_free 0.133, __dev_xmit_skb 0.119, sock_poll 0.118, __arch_copy_to_user 0.114.

CPU 1 threads, user self:
- flusher: flush_txn 1.387 (22.1%), touched_cmp 0.773, sift 0.578, sqlite3VdbeExec 0.502, memcpy 0.448, trim 0.347, cycle 0.304, trinkle 0.298, flusher 0.297, sqlite3BtreeIndexMoveto 0.108, __pthread_mutex_unlock 0.095, pcache1Fetch 0.062, __pthread_mutex_trylock_owner 0.059, __qsort_r 0.046, sqlite3BtreeInsert 0.043 (sift, trinkle, cycle and trim are musl's smoothsort, i.e. qsort).
- job-0: sorted_known 0.609 (21.1%), replay_extras_encode_parts 0.440, replay_extras_decode 0.414, run_replay_v6 0.303, coder_code 0.116, atom_attack 0.078, can_cover 0.069, draw_for 0.063, move_list_next 0.063, sqlite3VdbeExec 0.061, walChecksumBytes 0.039, coder_finish 0.037, sqlite3BtreeIndexMoveto 0.028, apply_pickup 0.026, move_list_check 0.023.
- req-0: __pthread_mutex_lock 0.055, sqlite3VdbeExec 0.050, memcpy 0.043, b64_decode 0.041, __pthread_mutex_unlock 0.041, sqlite3BtreeIndexMoveto 0.037, cb_jwt_verify 0.026, walChecksumBytes 0.024 (spread thin; its work is h_meta 1.00 and h_create 0.92 us/move inclusive).
- Kernel self on every CPU 1 thread and the idle task is network receive and transmit completion: handle_softirqs, ena_clean_tx_irq, __slab_free, skb_release_data, __inet_lookup_established, tcp_v4_rcv, tcp_ack.
The full top-30 self and top-60 inclusive lists of every thread are in out/analysis/perf_perfcy.txt.

Thread names (from stacks): the 8-10 us/move unnamed thread is the journal flusher (`__clone <- start <- flusher <- flush_pass <- flush_txn`); the 0.3 us/move unnamed thread is the WAL checkpointer (`ck_main <- sqlite3_wal_checkpoint_v2`).

## 4. Loop thread subsystems (inclusive, perfcy, us/move)

Source: out/analysis/children_perfcy_loop.txt (scripts/analyze.py children).
- cb_loop_run 18.63 = ws_frames 13.02 + flush_dirty 2.87 + conn_io 1.61 (recvfrom 1.47) + epoll wait 0.56 + conn_try_parse 0.32 + drain_posts 0.16.
- ws_frames 13.02 = pump 9.60 + flush_now 3.09 (sendto) + frame parse and unmask about 0.13.
- pump 9.60 = job_run_frame 8.02 (h_action 7.88) + cb_server_inline_begin 0.48 + cb_server_inline_done 0.53 + memcpy 0.13 + job_settle 0.12 + cb_router_match 0.09.
- h_action 7.89 = cb_table_run 7.39 + cb_game_id_ok 0.15 + wrap400 0.13 + cb_res_send 0.12.
- cb_table_run 7.39 = cb_section_commit 5.43 + cb_journal_game_acquire 1.01 + table_act 0.47.
- cb_section_commit 5.43 = cb_fanout_publish 2.63 (rt_publish_view 1.40, rt_publish_cast 1.08) + j_game_commit 0.97 (loop_write 0.74) + table_commit_products_flags 0.59 + section_cast 0.23 + table_resident_into 0.17 + section_row 0.17 + local_room_heard 0.15.
- Sends: flush_now 5.76 in total (3.09 from ws_frames, 2.67 from flush_dirty), of which sendto 5.70.
- pub_deliver 1.55 inclusive (self 0.50, cb_ws_send 0.38, strcmp 0.34).

## 5. CPU 1 subsystems (inclusive, perfcy, us/move)

Source: out/analysis/children_perfcy_32997.txt, children_perfcy_32995.txt, children_perfcy_32994.txt.
- flusher 8.40 = flush_txn 8.00 = cb_rows_sql_flush 3.08 + __qsort_r 2.41 + flush_txn self 1.39 + trim 0.40 + interrupts 0.35.
- cb_rows_sql_flush 3.08 = cb_sql_write = flush_step 2.67 (cb_sql_run 1.75, rate_rows 0.59, list_write 0.22) + sqlite3_step 0.39.
- SQLite inclusive (any sqlite3* frame) on the flusher: 2.95; on job-0 0.74; on req-0 0.74.
- flush_txn self, by source line (scripts/srclines.py, DWARF of the twin; out/analysis/srclines_perfcy_flusher.txt): ov_evict journal.c:1620 0.567 and :1629 0.128, flush_due via snapshot_take journal.c:1472 0.429, snapshot_take 0.15, ratings_evict 0.04.
- The qsort's comparator is touched_cmp (journal.c:1606-1607), which only ov_evict uses (journal.c:1630).
- job-0 4.35 = finalize_game 4.25 = table_replay_code_list 1.41 + cb_table_snap_summary 1.16 + cb_sql_write 0.71 + table_replay_extras_list 0.68 + j_game_list 0.11 + cb_log 0.10.
- run_replay_v6 1.81 (sorted_known 0.66, self 0.30, atom_attack 0.28, apply_pickup 0.14, coder_code 0.13); replay_extras_encode_parts 0.63; replay_extras_decode 0.57; coder_code 0.15, coder_finish 0.05, coder_uniform 0.03 (bn_divmod has no symbol: it is inlined into the coder_* functions).
- req-0 2.17 = job_run 2.12 = cb_handle 1.93 (h_meta 1.00, h_create 0.92, cb_table_create 0.73) + job_run_frame 0.15.

## 6. Syscalls (bpftrace, bpf1)

Source: out/analysis/bpf_bpf1.txt (scripts/analyze.py bpf bpf1, raw/srv/bpf_bpf1.txt), 19.0 s of whole TICKs inside the play window, 581,014 moves in that span.
bpftrace slows the server 22.5%, so the counts per move are the deliverable, not the times; the us figures include probe overhead.

| thread | syscall | per move | us inside per move | us per call |
|---|---|--:|--:|--:|
| loop | clock_gettime | 1.967 | 1.35 | 0.69 |
| loop | sendto | 1.843 | 6.82 | 3.70 |
| loop | recvfrom | 1.056 | 1.80 | 1.70 |
| loop | futex | 0.117 | 5.00 | 42.9 |
| loop | epoll_ctl | 0.113 | 0.16 | 1.44 |
| loop | epoll_pwait | 0.016 | 2.40 | 148.7 |
| loop | mmap / munmap | 0.008 / 0.007 | 0.16 | |
| flusher | pwritev2 (nr 287) | 0.122 | 1.97 | 16.2 |
| flusher | futex | 0.046 | 22.6 | 492 |
| req-0 | fcntl / pwritev2 / futex | 0.084 / 0.084 / 0.061 | 0.10 / 1.61 / 28.1 | |
| job-0 | pwritev2 / fcntl / writev / futex | 0.101 / 0.056 / 0.016 / 0.014 | 1.38 / 0.07 / 0.10 / 25.7 | |

Totals per move: loop 5.13, req-0 0.27, flusher 0.23, job-0 0.20, checkpointer 0.07, all threads 5.91 (CBASE_SPEEDUPS.md's epoll figure was 6.4).
epoll: 0.016 waits per move, mean about 22.9 events per wake; 2,702 of the 9,368 wakes in the span (29%) returned the maximum of 64 events.
sendto: 1.84 per move, 82% of sends in the [64, 128) bytes bucket, 12% in [128, 256), mean about 140 bytes (approximate, from bucket midpoints).
recvfrom: 1.06 per move, 96% in [32, 64) bytes.
Futex blocked time per move: loop 5.0 us, req-0 28.1, job-0 25.7, flusher 22.6 (the CPU 1 threads idle in futex when they have no work).
Context switches per move (sched_switch out): loop 0.036 sleeping and 0.004 runnable; req-0 0.044; job-0 0.017; flusher 0.018.

Loop thread off-CPU time in the span (19.0 s):
- asleep in futex: 2,583 ms (13.6%), 18,153 times, 142 us each;
- asleep in epoll_pwait: 1,018 ms (5.4%), 2,850 times, 357 us each;
- preempted: 105 ms (0.55%), 2,390 times, 44 us each.
Run-queue latency of the loop after a wakeup: 0.38 us per move in total, median bucket 1-2 us, 99% under 32 us.

## 7. Wall time per CPU (cpu-clock, perfclk)

Source: out/analysis/clk_perfclk.txt (scripts/analyze.py clk perfclk), 21.0 s sampled per CPU.

| CPU 0 | share |
|---|--:|
| loop user | 33.2% |
| loop kernel | 29.6% |
| loop softirq | 12.0% |
| idle (default_idle_call) | 20.5% |
| softirq in the idle task | 2.7% |
| sampler bash and sleep | 1.6% |

| CPU 1 | share |
|---|--:|
| flusher (user 27.6, softirq 4.0, kernel 3.7) | 35.2% |
| job-0 (user 12.2, softirq 3.3, kernel 1.2) | 16.8% |
| req-0 (user 4.7, softirq 1.6, kernel 1.5) | 7.8% |
| checkpointer | 1.1% |
| idle | 23.4% |
| softirq in the idle task | 13.5% |
| sampler bash and sleep | 1.6% |

Answer: yes, CPU 0 is idle during play, 20.5% of the window (plus 2.7% servicing softirq from idle).
Answer: when the loop is not running it is mostly asleep on a futex (13.6% of the window, from cb_journal_game_acquire on ov_mu), then asleep in epoll_pwait (5.4%); preemption (0.55%) and run-queue wait (0.38 us/move) are small.
Fact: CPU 1 is also not saturated (23.4% idle) while its threads wait about 17 us per move in the run queue (schedstat, base1: req-0 6.7, job-0 5.6, flusher 4.7).
Inference: CPU 1's softirq bursts (13.5% in idle plus about 9% inside the threads) delay the flusher, and the flusher is preempted 0.013 times per move; a preemption while it holds ov_mu would stretch the loop's 142 us futex sleeps.
That last link is not measured: I have no lock-hold-time trace.

## 8. Load box

Source: out/steps.log lines `play loadcpu` (cpu.sh lreport over [t_go+2, t_go+23]) and raw/load/lcpu_LABEL.log.

| run | busy % of 8 vCPUs | busiest vCPU % | procs_running mean | max |
|---|--:|--:|--:|--:|
| base1 | 37.6 | 38.4 | 9.66 | 118 |
| perfcy | 31.2 | 32.3 | 13.53 | 107 |
| perfclk | 33.6 | 34.8 | 14.29 | 116 |
| bpf1 | 27.4 | 28.0 | 6.79 | 65 |
| base2 | 34.6 | 36.0 | 10.04 | 110 |
| irq1 | 42.7 | 43.5 | 21.13 | 129 |
| irq0 | 31.8 | 33.4 | 9.22 | 100 |

Answer: no, the c7g.2xlarge does not limit the test: at most 42.7% of its vCPUs are busy and no vCPU passes 43.5%.
The same box drove 45.5k moves/s in irq1, 13% above base1.
procs_running includes the sampler; its mean above 8 and maxima of 100+ show bursty client wakeups (384 processes), so some client-side scheduling delay is possible inside the 3-5 ms p50, but idle capacity is above 56% in every run.

## 9. The code-read leads

(a) "The unnamed CPU 1 thread is the journal flusher, and most of its time is one UPDATE ... per waiting move (rows_sql.c list_write, sqlite3 inclusive)."
The first half is supported: its stack is `flusher <- flush_pass <- flush_txn`, 8.40 us/move.
The second half is not: list_write is 0.22 us/move (2.6% of the thread), all SQLite on the thread is 2.95 us/move (35%), while ov_evict and snapshot_take under ov_mu take about 3.8 us/move (45%), 2.41 of it in qsort with touched_cmp.

(b) "The loop blocks on a mutex held by the flusher, or moves get re-run on req-0 after WOULD_BLOCK."
The mutex half is supported: the loop sleeps in futex 13.6% of the window (5.0 us/move with bpf on), and those sleeps come from `__pthread_mutex_timedlock` in cb_journal_game_acquire, which takes ov_mu (journal.c:1812), the lock that flush_txn holds over ov_evict (journal.c:1656-1670) and snapshot_take (1642-1645).
On-CPU futex samples of the loop that are waits: 0.032 us/move from cb_journal_game_acquire, 0.007 from cb_journal_game_release, 0.006 from write_batch (out/analysis/futex_callers_perfcy_loop.txt; out of 0.175 us/move futex on-CPU, the rest are wakes: cond signals in j_game_commit and cb_pool_submit).
That the holder is the flusher is an inference from the source (ov_mu is also taken by other threads), but the flusher is the only thread with seconds of work under it.
The re-run half is not supported: h_action on req-0 is 0.144 us/move, and the server log sends 0.16% of requests to the pool (1,682 of 1,027,558 in base1), with 0 over budget and the valve never tripped.

(c) "About two sendto per move on the loop": supported, 1.84 per move, about 140 bytes each, 5.70 us/move inclusive (cycles), the largest single kernel cost of the loop.

(d) "clock_gettime as a real syscall (CLOCK_THREAD_CPUTIME_ID) about twice per move on the loop": supported, 1.97 per move; the kernel path is thread_cpu_clock_get / task_sched_runtime (0.51 us/move inside the syscall, 1.04 us/move including the user wrapper); 0.85 of it comes from cb_thread_cpu_us called by cb_server_inline_begin and cb_server_inline_done (out/analysis/clock_gettime_callers_perfcy.txt).

(e) "Cycles in atomics helpers and crc32c on the loop": small.
Self time in `__aarch64_*` outlined atomics on the loop is 0.114 us/move (0.6% of the loop), crc32c 0.035 us/move.

(f) "bn_divmod / coder_finish / coder_code / run_replay_v6 share of job-0": run_replay_v6 is 1.81 of job-0's 4.35 us/move (42%); the codec (coder_code 0.15, coder_finish 0.05, coder_uniform 0.03, with bn_divmod inlined into them) is 0.23 us/move (5%); replay_extras encode and decode are 1.20 (28%); sorted_known alone is 0.61 self (14%).

## 10. irq0 / irq1 (hints, one run each)

Source: out/analysis/irqs.txt, out/analysis/runs.txt.
By default irqbalance was active and both ENA queue IRQs had affinity CPU 1 at the check, but irqbalance moved them: base1 had 19% of queue interrupts on CPU 0, base2 50%, perfcy 44%, perfclk 48%, bpf1 50%.
irq1 (irqbalance stopped, all ENA IRQs on CPU 1): 45.5k moves/s play, p99 10.0 ms, loop 16.25 us/move, CPU 0 softirq 0.0%, CPU 1 89.2% busy.
irq0 (all on CPU 0): 34.6k moves/s, loop 22.98 us/move, CPU 0 softirq 22.7%, CPU 1 46.3% idle.
Inference: pinning the network interrupts to CPU 1 (and stopping irqbalance) is worth about +13% over base1 and +21% over base2; this needs a 3-run A/B before it is a conclusion.

## Facts versus inferences, and gaps

- Facts are numbers read from the named files; the inferences are marked "Inference" above.
- chronyd on the server was OOM-killed 169 s after boot (dmesg in out/steps.log, bpf1 stop) by the user-data dnf install, which also died; perf and bpftrace were then installed by hand at 14:13-14:15Z before any run.
  The server clock ran undisciplined afterwards; the play window crosses boxes by epoch, so a drift of a few ms at most moves the 21 s window edges (under 0.1% of the moves).
- The AMI moved: kernel 6.18.62-127.179 here vs 6.18.56 in E1.
- Cycles per move are about 10% below schedstat on-CPU time on every thread (section 2); unresolved.
- Frame-pointer call graphs: leaf functions without frame records (memcpy, malloc, strcmp) lose their immediate caller, so their callers' inclusive figures are slightly low.
- handle_softirqs self time stands for IRQ-masked time (section 2 caveat).
- No lock hold-time trace: "the flusher holds ov_mu while the loop waits" comes from the source plus the sampled lock callers, not from a measured hold.
- One trip, one run per profiler and per IRQ setting; the irq1/irq0 numbers are hints.

## Files

- Scripts: scripts/up_trip1.sh (launch), scripts/trip1.sh (driver), scripts/waitready.sh, scripts/leftovers.sh, scripts/build_twin.sh, scripts/ctest.sh (local container check), scripts/analyze.py (moves, perf, children, clk, bpf), scripts/callers.py, scripts/srclines.py, scripts/runs.py, scripts/irqs.py.
- Box side: pkg/cpu.sh, pkg/load.sh, pkg/prof.sh, pkg/trip.bt, pkg/srv.sh (E1, unchanged).
- Raw: out/raw/srv (samplers cpu_LABEL.log with S/P/T/C/R/Q lines, irq and softirq snapshots, server logs, bpf_bpf1.txt, clk_prof_LABEL.txt), out/raw/load (LABEL.tgz with ev.* files and t_go_clk/t_done_clk, lcpu_LABEL.log), out/raw/perf (perf.data, kallsyms, modules), out/raw/proc/LABEL (flat_LABEL.txt.gz without callchains, cc_LABEL.txt.gz with callchains, rep_self and rep_children perf reports, resolve_LABEL.txt).
- Analysis: out/analysis/ (perf_perfcy.txt, children_perfcy_*.txt, clk_perfclk.txt, bpf_bpf1.txt, runs.txt, irqs.txt, clock_gettime_callers_perfcy.txt), out/analysis_moves.txt.
- Log and teardown: out/steps.log, out/batch2.txt to batch4.txt, out/launch.txt, out/teardown.txt.
- Twin: pkg/cbase_D.unstripped (BuildID fc51bf0e94a65b51749fe2597580f25de5640f19).

# A quantitative CPU model of the t4g.nano for `cbase serve`

Inputs: calibration/raw/{t4g,c6g}/p1..p5_*.tsv (calbench.c, five passes each), graviton/results.txt and graviton/runs/* (the real t4g.nano matrix and the c6g.large instruction-count runs), graviton/runs/prof2_hum2_NOW (perf record -g of hum2 on main 4aa1c22e3), and rig/tables.md (the Docker arm64 rig).
Every number below is a median over passes or runs unless it says min or p99.
Units: ns and us are nanoseconds and microseconds of wall time on the measuring core; "cycles" are the guest's PMCCNTR at 2.5 GHz (0.400 ns per cycle), B are bytes, KiB and MiB are powers of two.

## 0. The headline

- The t4g.nano's core is a c6g.large's core.
  Of 316 calibration rows that both boxes ran, the median t4g/c6g ratio of the p50 is 1.00, and no row of compute, memory, syscall, loopback TCP, io_uring or SQLite differs by more than the pass-to-pass noise.
  The clock is 2.465 GHz on both (add_chain256: 102.4 ns per 256 dependent adds, 0.4001 ns per add), sustained for 120 s with both vCPUs busy, with zero steal.
- The nano differs in exactly two places: anything that crosses its two vCPUs (a wake-up, an IPI, a contended cache line or mutex), which costs 2x to 25x more and more the longer the target vCPU idled, and the cold start of a vCPU that idled for milliseconds, which costs the server about 45 us of extra cycles per wake-up.
  EBS fdatasync is not a nano penalty: both boxes write a 4 KiB page and fdatasync in 0.90 ms (p50); the c6g sometimes runs a 0.35 ms mode the nano never showed in five passes.
- The matrix's "1.8 GHz" is not the hardware slowing down.
  In every one of the 63 runs, task-clock minus cycles/2.5 GHz is 8 us + 28.5 us per context switch of the measured task (fit over 63 runs, both boxes, residual sd 22 us dominated by the bot runs' 18 ms moves).
  That is the hypervisor's emulation of the PMU under perf stat: every switch in or out of a task that owns a hardware perf event traps a handful of PMU register accesses to EL2, during which the guest's clock runs and its cycle counter does not.
  The honest per-move CPU is cycles/2.5 GHz: 83 us at hum2, 47 us at sat2, 124 us at hum8, 56 us at sat8 (main 4aa1c22e3).
  Production runs without perf and pays none of it.

## 1. Primitive costs (both boxes unless a column says otherwise)

Each row gives min / p50 / p99 of the per-op time in ns, from the five-pass medians, with the fitted curve where the cost depends on a size.
A per-op time is the time of one operation; where calbench timed k operations per sample the row says so and the division is already done.

### 1.1 Core (identical on both boxes)

| primitive | cost | note |
|---|---|---|
| dependent 64-bit add | 0.40 ns (1 cycle) | the clock |
| independent adds | 0.16 ns each (2.5 per cycle sustained from a 4-wide front end with the loop overhead) | |
| 64-bit mul, dependent | 1.6 ns (4 cycles) | 1.2 ns independent |
| udiv/sdiv 64-bit | 2.8 ns + 0.1 ns per significant quotient bit above 8: n1 2.8, n32 5.6, n64 8.8 (divisor 2); a wide divisor cuts it (n64_d32 6.0) | the N1 early-out |
| fdiv_d dependent | 4.8 ns (12 cycles); fsqrt 2.8 ns | |
| predictable branch | 0.8 ns; alternating 1.4; random 50% 3.1 (about 6 cycles of misprediction half the time) | |
| uncontended atomic add / cas | 7.6 / 8.8 ns | |
| uncontended mutex lock+unlock | 16.8 ns | musl |
| contended atomic add (other vCPU hammering the line) | t4g p50 1237 ns, p99 24 ms (!); c6g p50 41 ns, p99 49 ns | 25x: see 2.2 |
| contended mutex lock+unlock (other vCPU hammering it) | t4g min 58, p50 3486, p99 42884 ns; c6g min 115, p50 140, p99 194 ns | 25x: see 2.2 |

### 1.2 Memory (identical)

The chase rows time 8 dependent loads per op; the per-load latency is the row's value divided by 8.

| working set | per load | level |
|---|---|---|
| <= 64 KiB | 1.6 ns (4 cycles) | L1d (64 KiB) |
| 96 KiB to 384 KiB | 5.5 ns (14 cycles) | L2 (1 MiB per core) |
| 512 KiB | 6.9 ns; 768 KiB 10.8 ns | L2 tail |
| 1 MiB | 17.5 ns; 2 to 4 MiB 35 ns (88 cycles) | L3 (32 MiB shared) |
| 8 MiB 52 ns; 16 MiB 108 ns | L3 to DRAM |
| >= 32 MiB | 113 to 115 ns | DRAM |

STREAM: copy 28 ns per KiB in-cache (36 GB/s), 1.7 us per 64 KiB memcpy.
A first-touch page fault costs 0.97 us per 4 KiB page (mmap_touch_munmap pages1024: 998 us), a refault after MADV_DONTNEED 0.96 us per page, and mmap+munmap of any size 1.9 to 2.4 us.
A 3.25 MB malloc that musl satisfies with mmap therefore costs about 2.4 us + 800 faults x 0.97 us = 0.8 ms to touch, every time it is freed and taken again.

### 1.3 Strings and copies (identical; musl)

| primitive | fit | points |
|---|---|---|
| memcpy | 3.6 ns for <= 64 B, then 3.6 + 0.026 ns/B: 256 B 10.7, 1 KiB 30, 1200 B 35, 4 KiB 107, 8 KiB 210, 64 KiB 1711 | 38 GB/s past 1 KiB |
| memset | 2.4 to 5.2 ns for <= 128 B, then 0.026 ns/B: 1 KiB 27.6, 1200 B 32, 4 KiB 104 | |
| memcmp (equal) | 2.0 ns + 0.81 ns/B: 128 B 106, 1 KiB 833, 1227 B about 1000 | byte at a time |
| strcmp (equal) | 2.1 ns + 0.81 ns/B: 36 B about 31, 1 KiB 834 | byte at a time |
| strcmp/memcmp differing at byte d | 2.0 + 0.81 d ns | |
| strlen | 4.4 ns + 0.098 ns/B: 36 B 7.6, 1 KiB 101 | vectorized |
| snprintf of a 36-char id ("%s") | about 150 ns (not in the suite: printf_core 1.2% of the pre-journal profile at ~18 calls) | |

### 1.4 malloc (identical; musl mallocng)

| case | cost |
|---|---|
| malloc+free, one live, <= 512 B | 230 to 290 ns (the lock, the meta walk) |
| malloc+free, one live, 1 to 16 KiB | 74 to 85 ns p50 but p99 7 to 10 us (the group is unmapped when its last slot frees and mapped again) |
| 64 live then freed, per pair | 40 ns at 8 to 32 B, 150 ns at 48 to 128 B, 420 ns at 256 B, 1.0 us at 1 KiB, 1.4 us at 2 KiB, 2.0 us at 4 KiB, 7.1 us at 64 KiB |
| the other vCPU allocating at the same time | 1.5 to 2 us p50 per pair, p99 20 to 60 us (both boxes; the global lock) |
| 1 MiB malloc+free | 34 us (mmap + munmap + faults on first touch) |

Rule: a live set of large slots costs a microsecond per allocation; a steady stream of small ones a quarter microsecond each; any concurrent allocator traffic from another thread multiplies both by 5 to 10.

### 1.5 Syscalls (identical)

| syscall | cost |
|---|---|
| getpid / ENOSYS entry+exit | 115 / 112 ns |
| clock_gettime MONOTONIC (vdso) | 30.8 ns; cntvct read 18.6 ns; COARSE 8.4 ns; THREAD_CPUTIME 255 ns (a real syscall) |
| epoll_wait, one fd ready, LT | 242 ns; timeout 0 with 64 idle fds 150 ns |
| eventfd write + read, same thread | 406 ns |
| futex wake with no waiter | 152 ns; sched_yield 284 ns |
| pipe write+read same thread | 510 ns for <= 64 B, +0.03 ns/B |
| io_uring_enter, NOPs | 264 ns for 1 SQE (t4g; c6g 307), 314 for 2, 439 for 4, 819 for 11, 1932 for 32: 240 + 52 ns per SQE |
| io_uring_enter, 4 KiB page-cache writes | 1198 ns for 1, 3475 for 4, 8996 for 11: 300 + 790 ns per write (the write itself) |
| pwrite to the page cache | 740 ns for 64 B, 800 for 1200 B, 1080 for 4 KiB, 1450 for 16 KiB, 3850 for 64 KiB: 720 + 0.05 ns/B |
| pread from the page cache | 330 ns for 64 B, 387 for 1200 B, 513 for 4 KiB: 320 + 0.05 ns/B |
| fdatasync after a 4 KiB overwrite (EBS gp3) | t4g 0.90 ms p50 (p99 0.95 to 1.25 ms) in all five passes; c6g 0.90 ms in three passes and 0.35 ms in two |
| fdatasync after an appending write (file growth) | 1.95 ms p50 both (the extent and the inode go too) |
| msync of one dirty 4 KiB page, MS_SYNC | 1.33 ms both |

The "t4g fdatasync is 2.5 to 2.9x slower" of the preliminary note was pass 1 against the c6g's fast mode.
The five-pass medians are equal; what is true is that the nano never showed the fast mode.
Model fdatasync as 0.9 ms (p50), 1.0 to 1.25 ms (p99), 2 ms when the file grows.

### 1.6 TCP over loopback (identical) and over ENA (the nano profile)

Loopback, one socket, TCP_NODELAY, both ends in the measuring thread:

| primitive | cost |
|---|---|
| send() alone, 16 B to 1500 B | 2.08 to 2.27 us (16 B 2.08, 512 B 2.11, 1200 B 2.23, 1500 B 2.27); 4 KiB 2.50; 16 KiB 3.17; 64 KiB 7.9 |
| recv() alone | 0.45 us for 64 B, 0.50 for 16 B, 0.56 for 512 B, 0.62 for 1200 B, 0.80 for 4 KiB, 1.45 for 16 KiB |
| send then recv, same thread | 4.33 to 4.49 us up to 1500 B |
| fan-out to N sockets then drain all | 4.29 us for 1, 8.27 for 2, 12.2 for 3, 16.0 for 4, 24.0 for 6, 31.6 for 8, 43.1 for 11, 62.8 for 16: 0.3 + 3.95 us per socket, independent of 64 to 1500 B |

A loopback send includes the receiver's softirq (netif_rx runs the RX path before send returns), so "send" here is TX + RX of one segment: about 2.1 us, of which the entry is 0.11 us and the TCP write path about 1.2 us.

On the nano over ENA to the load box (prof2_hum2_NOW, cycles-sampled, 83.3 us of cycles per move), the kernel side of a move is:

| path | share of the move | us per move | per call |
|---|---|---|---|
| sendto (2 sends: the actor's REPLY+VIEW+CAST, the opponent's VIEW+CAST) | 26.6% | 22.1 | 11 us per send, of which tcp_sendmsg_locked 22.6%, ip6_xmit 10.3%, __dev_queue_xmit 7.7%, ena_start_xmit 3.75% (1.73% self: the descriptor and the doorbell), and the softirqs run at the syscall's exit (handle_softirqs 0.44% self): the ACKs of the previous move's segments are processed here |
| recvfrom (1) | 9.8% | 8.1 | tcp_recvmsg_locked 4.7%, skb_copy_datagram_iter 2.6% |
| epoll_pwait (1 per move at human pace) | 9.9% | 8.2 | ep_poll 6.45% of which schedule_hrtimeout_range 2.6% (the sleep and its hrtimer: hrtimer_start_range_ns 0.94% self, hrtimer_try_to_cancel 0.41%), ep_send_events 2.7% |
| all syscalls | 45.4% | 37.7 | |

The marginal send is cheaper than 11 us: hum8 (9.5 sends per move) costs 124.4 us of cycles against hum2's 83.3 with 2 sends, and the 41 us difference also holds 7 more casts and 6 more envelopes (about 15 us), so an extra send to another socket is about 3.5 us, close to the loopback 4 us per socket.
The first send of a move carries the fixed network cost: route and neighbour lookups cold after the idle gap, the TX ring, and the deferred ACK processing.
Model: 11 us for the first send of a pass, 3.5 to 4 us for each further socket, +0.03 ns per byte below 1500 B, one segment per socket per pass.

### 1.7 OpenSSL and SQLite (identical)

| primitive | cost |
|---|---|
| HMAC-SHA256 one-shot | about 1.9 us for <= 55 B, +9 ns per 64 B block (OpenSSL 3.5 EVP fetch included) |
| SHA256 one-shot | 0.35 us for <= 55 B |
| SQLite insert autocommit, WAL + synchronous NORMAL, 64 to 1200 B row | 9 to 12 us; 4 KiB 18 us; 8 KiB 29 us |
| SQLite update by pk | 11 to 14 us; select by pk 1.3 to 2 us |
| 4 inserts in one txn | 20 us (5 per row) |

These are warm-cache numbers; a rows_sql flush of a game writes its row and one game_moves row per waiting move: about 5 us per move row inside one transaction plus 10 to 30 us per transaction.

## 2. What differs on the nano and why

### 2.1 Cross-vCPU wake-ups (the burstable instance's scheduler)

wake group: the main thread (pinned to vCPU 1) sleeps 0, 1, 10 or 100 ms, then wakes a waiter thread on vCPU 0 (crosscore) or on its own vCPU (samecore).

| what | t4g by idle 0 / 1 / 10 / 100 ms | c6g by idle 0 / 1 / 10 / 100 ms | ratio |
|---|---|---|---|
| wake latency, futex, cross-core, p50 | 9.4 / 12.0 / 23.2 / 38.7 us | 4.55 / 5.08 / 5.84 / 6.49 us | 2.1 / 2.4 / 4.0 / 6.0 |
| wake latency, eventfd through epoll, cross-core | 10.4 / 13.6 / 27.7 / 44.8 us | 5.56 / 6.18 / 6.94 / 7.64 us | 1.9 / 2.2 / 4.0 / 5.9 |
| the waker's futex syscall, cross-core | 2.46 / 3.10 / 6.29 / 9.29 us | 2.04 / 2.36 / 2.50 / 2.73 us | 1.2 / 1.3 / 2.5 / 3.4 |
| the wakee's CPU charged per cycle, cross-core | 2.36 / 3.42 / 9.33 / 20.1 us | 1.83 / 2.13 / 2.83 / 3.35 us | 1.3 / 1.6 / 3.3 / 6.0 |
| wake latency, futex, same-core, p50 | 7.5 / 10.7 / 13.0 / 18.8 us | 15.9 / 13.0 / 16.7 / 21.3 us | 0.5 to 0.9 (see 2.4) |
| nanosleep overshoot, 1 / 10 / 100 ms | 73 to 81 / 88 to 98 / 116 to 125 us | 76 / 87 / 89 us | 1.0 / 1.1 / 1.35 |

Reading: on the dedicated c6g a cross-core wake is 4.5 us however long the target idled, because its core is ours and stays warm in WFI.
On the nano the two vCPUs are time slices of host cores shared with other tenants; a vCPU that goes idle is parked by the hypervisor, and waking it is an exit on the waker (the SGI write traps: +0.4 to 7 us on the waker's syscall) plus the host scheduling the vCPU back in (+5 to 30 us of latency), plus the resumed vCPU's own cold restart (the wakee is charged 2 to 20 us of CPU for work that is 2 us warm).
The longer the vCPU slept, the colder and the further away it comes back.
This is the cost of EVERY cross-thread hand-off on the nano when the other thread's vCPU is idle, which at production pace it always is: 10 to 40 us of latency and 10 to 30 us of CPU, against 5 us and 2 us on the c6g.

### 2.2 Contended lines and mutexes

A cache line both vCPUs write costs 1.2 us per seq_cst add on the nano (41 ns on the c6g) with a p99 of 24 ms; a mutex both hammer costs 3.5 us per lock+unlock (0.14 us) with a p99 of 43 us.
The two vCPUs of a nano are not co-scheduled: when the holder's vCPU is descheduled mid-critical-section the other spins and then sleeps, and the futex wake pays 2.1.
No shared state may be hot across the nano's two vCPUs; a lock on the move path that another thread ever takes costs microseconds, not the 17 ns of the uncontended case.

### 2.3 Cold start after idle: the human-pace cycle penalty

Same binary, same hum2 scenario, same 98 moves per second:

| box | cycles per move | instructions per move | IPC |
|---|---|---|---|
| c6g.large, hum2 NOW | 81.7k (32.7 us) | 64.0k | 0.78 |
| t4g.nano, hum2 NOW | 208k (83.3 us) | (no counter on the nano; the same binary and work: 64k) | 0.31 |
| c6g.large, sat2 NOW | 106k (42.4 us) | 123.5k | 1.17 |
| t4g.nano, sat2 NOW | 118.5k (47.4 us) | 123.5k | 1.04 |

At saturation the nano needs 12% more cycles than the c6g (its two vCPUs contend with each other and with the host); at human pace it needs 2.5x.
Between two moves of hum2 the loop sleeps about 10 ms; on the c6g its core keeps L1, L2 and the TLB warm through WFI, on the nano the host runs other tenants on that core and the vCPU comes back to cold caches.
The move touches 200 to 300 KiB (the kernel's TCP and ENA paths, musl, the game kernel, the section, the socket buffers); at 100 to 115 ns per line from DRAM, 126k extra cycles is about 1100 serialized misses, or 2000 with the memory-level parallelism a move's dependent walks allow.
BASE shows the same: 596k cycles on the nano against 361k on the c6g at hum2, 235k extra for 3 thread wake-ups per move, about 80k per wake-up.
Model: on the nano, every thread that wakes from an idle vCPU to take part in a move pays 80k to 125k cycles (32 to 50 us) of cold start.
That is why the number of threads that touch a move is the first design variable (OPTIMIZED_DESIGN.md section 3).

### 2.4 The oddity: the c6g's same-core wake is slower than the nano's

calbench opens five PMU events per thread (pmu_open: cycles, instructions, branch misses, L1d misses, LLC misses; attr.inherit = 0, this thread only).
On the c6g they all opened and counted; on the t4g only cycles opened and its reads failed, so no event was ever scheduled on the nano's measuring thread.
Every context switch of a thread with hardware perf events saves and restores its counters, and in a KVM guest on arm64 every PMU register access traps to the hypervisor: the switch out reads and disables five counters, the switch in programs and enables them again, each a trap of 1 to 3 us.
The same-core wake rows are the only wake rows in which the measuring thread itself is switched out and back (the wakee preempts it), so they carry 10 to 20 traps per round trip on the c6g and none on the t4g: c6g 15.9 us against t4g 7.5 us at idle 0, and the c6g's waker_syscall p50 of 49 us against the t4g's 22 us (the syscall returns only when the waker runs again).
The cross-core rows do not switch the measuring thread, so they compare the hardware fairly, and there the nano is the slow one.
The pingpong rows (net group) switch the measuring thread twice per round trip and show the same contamination (c6g 35 us against t4g 21 us same-core).
Corrected for it, a same-core switch in these guests costs about 10 us of wall time round trip per switch pair even without PMU events (t4g futex_pingpong samecore 20.9 us for two switches), five times a bare-metal switch; the hypervisor's share of a switch (the virtual timer reprogramming and the trap on the way back from WFI) is the rest.

### 2.5 The GHz gap: task-clock against cycles

| scenario (median of 3) | task-clock us | cycles/2.5 GHz us | context switches per move | excess us | excess per switch |
|---|---|---|---|---|---|
| hum2 BASE | 325.6 | 238.5 | 3.01 | 86.6 | 28.1 |
| hum2 NOW | 116.0 | 83.3 | 0.95 | 32.8 | 34.5 |
| sat2 BASE | 164.1 | 123.4 | 1.57 | 40.7 | 25.3 |
| sat2 NOW | 60.9 | 47.4 | 0.52 | 13.3 | 25.6 |
| hum8 NOW | 158.3 | 124.4 | 0.87 | 33.9 | 38.6 |
| sat8 NOW | 60.9 | 56.0 | 0.11 | 4.9 | 45.5 |
| bot NOW (per table version) | 18542.9 | 18372.5 | 1.96 | 93.0 | 46.9 |
| c6g hum2 NOW | 61.2 | 32.8 | 0.97 | 28.4 | 29.2 |
| c6g sat2 NOW | 66.0 | 42.9 | 0.70 | 23.1 | 33.0 |
| fit over all 63 runs | | | | 8.4 + 28.5 x switches | |

The excess is a per-switch constant, the same on the dedicated c6g (no steal possible) and the nano, and it vanishes where switches are rare (sat8: 0.11 per move, 4.9 us; the bot runs: 0.5% of 18.5 ms).
It is the PMU emulation of 2.4 under `perf stat -e cycles`: perf's task-clock runs from the task's switch-in to its switch-out by the guest's clock; the guest's cycle counter is a host perf event that counts only in guest mode; each switch traps about ten PMU accesses to the hypervisor at 2 to 3 us each, inside the task-clock and outside the cycles.
Three facts rule out the alternatives: the calibration clock holds 2.465 GHz with zero steal for two minutes in both idle and busy states (steal group, 480 samples per pass), so the core does not run slow; the excess does not scale with idle exits (hum2 6.5 per move, c6g 11.0 per move, same excess) nor with syscalls (BASE 21.7, NOW 3.9, same excess per switch), so it is not WFI or interrupt time; and it is identical on a box with dedicated cores.
Production has no perf attached and so no PMU traps; the per-switch cost that remains in production is the real one of 2.1 and 2.4 (about 10 us of wall per same-core switch pair, 10 to 40 us per cross-vCPU wake), which is already in the cycles and the latencies.
Use cycles/2.5 GHz as the per-move CPU of the matrix, and read task-clock as cycles + 28 us per switch.
For the same reason, utime+stime from /proc (tick sampled) matches task-clock, not cycles: the ticks land on whoever the guest thinks is running.

### 2.6 Idle exits

bpf counted power:cpu_idle exits on both vCPUs: 6.5 per move at hum2 NOW, 9.1 at hum8 NOW, 8.9 at hum2 BASE; the c6g 11.0 (it has a cpuidle driver and a tick the nano's WFI-only idle does not).
At hum2 one move causes: the request's RX interrupt (1), the two sends' TX completion interrupts (1 to 2, ENA moderates them), the two clients' ACKs (2), and the loop's own wake (counted in the first when it lands on the loop's vCPU, otherwise an IPI and one more exit).
ENA has one queue per vCPU and hashes the clients' flows across both, so about half of the request interrupts land on the vCPU the loop is not on: the wake is then the cross-vCPU kind of 2.1 (an IPI into a parked vCPU) rather than a same-core one.
Each exit costs the box 10 to 40 us of wall and 2 to 20 us of CPU outside the server's task-clock (the wakee_cpu rows), which the server's numbers do not show and the box's power budget does.
Steering ENA's interrupts to the loop's vCPU (IRQ affinity, or one combined queue) turns the IPI wake into a same-core one and halves the exits per move; it is an ops change, measured by this counter.

## 3. The rig-to-nano transform

The rig is Docker arm64 on an Apple Silicon Mac.
Its saturation numbers are honest per-instruction costs of the server; its human-pace numbers are not (the Mac runs the VM's vCPU at a low clock and on an efficiency core at 1% utilization: rig hum2 NOW 76.9 us against 83.3 us of cycles on the nano, rig sat2 47.6 against 47.4), and it has no PMU, no ENA and no parked vCPUs.

Decompose a rig run into:

- compute C: the rig's saturation CPU per move minus its sends and syscalls (rig sat2 NOW: 47.6 us total with 8.6 syscalls of which about 2.1 sends and 1 recv at the rig's 3 to 5 us per send, so C is about 30 us);
- sends per move n_s (syscalls minus recv minus the epoll share; the rig and the nano agree to 2%);
- bytes per send (below 1500 B they do not matter);
- hand-offs per move h (cross-thread wakes on the move path: BASE 1.5 at saturation, 3 at human pace; NOW 0);
- wake-ups per move w (threads that wake from an idle vCPU for the move: at saturation 0, at human pace one per thread that takes part);
- fsyncs per move f (0 for the journal, 0 for SQLite WAL+NORMAL except checkpoints).

Then the nano's cycles per move are

    T_nano = C + 11 us x min(n_s, 1) + 3.75 us x max(n_s - 1, 0) + 2.5 us (recv + epoll share at saturation) + 40 us x h + 45 us x w + 0.9 ms x f

where 40 us per hand-off is the cross-vCPU cache migration of the move's working set measured by BASE (sat2: 123.4 - 58.8 = 64.6 us of extra cycles for 1.57 switches; sat8: 93 us for 1.53) and 45 us per wake-up is the cold start of 2.3 (hum2 NOW: 83.3 - 47.4 + the 10 us of saturation-only work hidden in the sat number).
Saturation-only work (game ends with their finalize and SQLite flush, refused moves, RX softirq charged to the task) is about 10 us per move at sat2 and is in both the rig's and the nano's saturation numbers; it is removed when predicting human pace.
Task-clock as perf reports it on the nano is then T_nano + 28 us x context switches per move.

### 3.1 Validation against the Graviton matrix

| scenario | rig input (us, cgroup) | prediction (us of cycles) | measured nano cycles (us) | error |
|---|---|---|---|---|
| sat2 NOW | 47.6; n_s 2.1; h 0 | 47.6 (the rig's sends and the nano's cost the same within noise) | 47.4 | +0.4% |
| sat8 NOW | 56.3; n_s 4.9 (coalesced); h 0 | 56.3 | 56.0 | +0.5% |
| sat2 BASE | 58.8; h 1.57 | 58.8 + 40 x 1.57 = 121.6 | 123.4 | -1.5% |
| sat8 BASE | 94.0; h 1.53 | 94.0 + 40 x 1.53 = 155 | 187.2 | -17% |
| hum2 NOW | from sat2 NOW: 47.6 - 10 + 45 x 1 | 82.6 | 83.3 | -0.8% |
| hum8 NOW | from sat8 NOW: 56.3 - 10 + 45 + 3.75 x (9.5 - 4.9) + 2 x 4 more casts built uncoalesced | 116 | 124.4 | -7% |
| hum2 BASE | from sat2 BASE: 58.8 - 10 + 45 x 3 + 40 x 3 (each of the three wakes is also a cross-vCPU hand-off) | 304 | 238.5 | +27% |
| hum8 BASE | same, from 94.0 | 339 | 355.3 | -5% |
| task-clock, hum2 NOW | 83.3 + 28 x 0.95 | 110 | 116.0 | -5% |
| task-clock, sat2 BASE | 123.4 + 28 x 1.57 | 167 | 164.1 | +2% |

Where it holds: NOW at every pace within 1% at 2 seats and 7% at 8 seats; BASE at saturation at 2 seats; the task-clock transform everywhere.
Where it fails: BASE at 8 seats and saturation (-17%: the model has no term for the 48 syscalls per move of BASE sat8, mostly SQLite fcntl locks and WAL pwrites, each 0.7 to 1 us, about 30 us) and BASE at human pace (+27%: a wake that is also a hand-off is counted twice; the cold start and the migration overlap, so the right term is max(45, 40) + 20 per combined wake-and-hand-off, which gives 243 against 238.5).
The model has no rig term for human pace at all: the rig's human-pace CPU is the Mac's idle clock and must not be used.
The nano is the only instrument for wake-ups and cold starts; the c6g is the only instrument for instruction counts; the rig is good for saturation compute and for A/B of builds.

## 4. What this means for the nano as a production box

- Per-move CPU at human pace is 83 us of cycles (NOW), of which about 45 are the cold start of the loop's vCPU and 38 are the move: 37 us of kernel (two sends, one recv, one wait) and the rest the handler.
  At production's pace of a few moves a minute the box's CPU is free; what the model governs is latency (every cross-vCPU hop adds 10 to 40 us and each is also a cold start) and memory (512 MB, section 5 of OPTIMIZED_DESIGN.md).
- The nano's baseline is 5% of each vCPU; at 83 us per move that is 600 moves per second sustained without credits, 23k with them.
- Nothing hot may be shared between threads on different vCPUs; a thread that participates in a move costs 45 us before it does anything; a futex or eventfd hand-off costs 10 to 40 us of latency and tens of us of CPU across both vCPUs.
- EBS: a durable write costs 0.9 ms, 2 ms if the file grows; the journal's design (mmapped, fallocated segments, no msync) costs none of it per move, and the flush's SQLite transaction costs one or two at its end.

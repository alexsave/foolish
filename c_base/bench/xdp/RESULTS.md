# AF_XDP + lwIP against kernel TCP on a t4g.nano

Research spike, 2026-10-20, branch `xdp-spike` (not for merging).
Code: `c_base/bench/xdp/` (server `srv.c` + `be_xdp.c` behind `be.h`, XDP program `xdp_prog.c`, client `load.c`, harness `aws/`, tables `analyze.py`).
Raw data: `c_base/bench/xdp/results_2026-10-20/` (`results.txt` holds every run's client line and the server snapshots at the start and end of its window; `srv/` holds every server start/stop log, the NIC ladder, facts and coexistence checks; `master.log` is the run order).
148 runs: 3 interleaved repetitions of the matrix, plus correctness smokes, drop tests and idle floors.

## Bottom line

AF_XDP + lwIP works on a t4g.nano: driver-mode XDP, zero-copy AF_XDP on both ENA queues, need-wakeup, no busy polling, and SSH, SSM, IMDS, DNS and S3 all keep working through the kernel.
At human pace it cuts the box CPU per move by 26 to 76 us (25 to 40%), but the move still costs 83 to 146 us of box CPU, not 10 to 15 us: the cold start stays, and lwIP adds a wake per move for the clients' ACKs that the kernel used to absorb in softirq.
Latency at human pace is the same within 5 to 18 us.
Under real packet loss lwIP's recovery is far weaker than Linux's: when the nano's network allowance ran out, kernel TCP still delivered every move (p99 311 ms), lwIP left 89 and 16 moves undelivered after a 10 s drain.
Verdict: not worth building into cbase.

## The setup

- Server: t4g.nano, unlimited credits, AL2023, kernel 6.18.54-121.166.amzn2023.aarch64, ENA driver 2.17.4g (the out-of-tree driver AL2023 ships as `kernel/drivers/amazon/net/ena`), `CONFIG_XDP_SOCKETS=y`, 2 combined queues (the maximum), default coalescing rx-usecs 20, tx-usecs 64, IPv6-only subnet.
- Client: c7g.large in the same subnet, kernel TCP, one epoll thread (`load.c`).
- The app (`srv.c`): N TCP connections in groups of K; each MOVE received spins a calibrated handler (a loop that reads and writes a 64 KiB buffer, 20 us or 0) and sends one FRAME of 300 B or 1.2 KB to each of the group's K connections, the mover's included; one recv plus K sends per move.
- Backends behind one interface (`be.h`: `send`, and `app_on_open/data/close`):
  - `kern`: epoll, one recv per readable socket, one `send()` per destination, TCP_NODELAY, on the production NIC config (MTU 9001, no XDP);
  - `kernx`: the same kernel backend with the XDP program attached and passing everything (the XDP NIC config: MTU 3498), which isolates the cost of XDP itself;
  - `xdp`: AF_XDP + lwIP 2.2.1 (raw API, NO_SYS=1, IPv6 only, TCP_MSS 1440, no SACK), one thread; `poll()` on the AF_XDP fds and the control fd with lwIP's next timer as the timeout; per pass: drain the RX rings into `ip6_input` (frames copied into pbufs, UMEM frame straight back to the fill ring), lwIP timers, `tcp_output()` on every connection written to, one `sendto()` TX kick, completion-ring reclaim;
  - `xdpP`: the same with no `sendto()` kick: the next `poll()` drives TX (`xsk_poll` calls the zero-copy wakeup).
- Scenarios: hum (300 or 304 connections, each sends a move every U(2,4) s, about 100 moves/s, 10 s warm-up, 45 s window) and sat (closed loop, every group has one move in flight, 5 s warm-up, 20 s window).
- Metrics from the server's own snapshot over the window (`srv.c` `snapshot()`): server CPU from `/proc/self/schedstat` (ns), box CPU and softirq from `/proc/stat` per vCPU (10 ms ticks), idle exits from the `power:cpu_idle` tracepoint counted per vCPU with `perf_event_open` (filter `state == 4294967295`, no sampling), context switches from `getrusage`, syscalls counted in-app at every call site (validated against the `raw_syscalls:sys_enter` tracepoint in the smokes: 3.46 vs 3.46, 9.49 vs 9.50, 0.77 vs 0.78, 1.81 vs 1.81 per move), NIC interrupts from `/proc/interrupts`.
- No perf stat anywhere.

## What worked on ENA and the t4g kernel

| Step | Result |
|---|---|
| Driver-mode XDP at MTU 9001, 2 queues | Refused: `ena: Failed to set xdp program, the current MTU (9001) is larger than the maximum allowed MTU (3502) while xdp is on` |
| Driver-mode XDP at MTU 3498, 2 queues | Works. dmesg: `XDP program is set, changing the max_mtu from 9216 to 3502`. No device reset seen: the network answered 0.01 s later. No need to drop to one queue, no generic mode |
| AF_XDP zero-copy, need-wakeup, queue 0 and queue 1 | Both bind zero-copy (`XSK q=0 zerocopy bind: ok`, `q=1` too), in every run |
| Binding an xsk per run | No reset, no network gap measured (srvctl pings the client after every start) |
| Coexistence with the redirect live | IMDSv2 over IPv6, DNS, S3 put over dual-stack, the SSM endpoint (HTTP 400 in 17 ms, i.e. reachable), sshd banner over the NIC, and an SSM `send-command` from the laptop during the AF_XDP drop test (16:01:33 UTC) all succeeded. The SSM agent's `ec2messages` lookup error predates the XDP attach and is unrelated |
| Steering | Not needed: one socket per queue in one XSKMAP keyed by `rx_queue_index`; TX goes out of queue 0's socket |

The ENA XDP limit is the MTU (3502 with 4 KiB pages), not the queue count, on this driver.
The production path to CloudFront is 1500 anyway, so the MTU cap costs nothing in production.

## Headline: human pace (about 100 moves/s), mean of 3 repetitions

Box CPU is all of both vCPUs (user, system, irq, softirq) above that repetition's idle floor with the same connections open, per move.

| K | frame | spin | backend | box CPU us/move | server CPU us/move | softirq us/move | syscalls/move | ctx sw/move | idle exits/move | NIC irqs/move | p50 us | p99 us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 300 | 0 | kern | 118.0 | 31.3 | 90.0 | 4.0 | 0.95 | 6.79 | 4.70 | 192.8 | 282.9 |
| 2 | 300 | 0 | kernx | 99.9 | 28.3 | 82.2 | 4.0 | 0.95 | 6.80 | 4.67 | 192.3 | 285.8 |
| 2 | 300 | 0 | xdp | 83.3 | 38.7 | 57.8 | 3.1 | 2.09 | 5.84 | 3.87 | 175.2 | 264.6 |
| 2 | 300 | 20 | kern | 122.7 | 51.0 | 83.3 | 4.0 | 0.95 | 6.77 | 4.74 | 209.5 | 299.2 |
| 2 | 300 | 20 | xdp | 97.1 | 63.2 | 49.5 | 3.1 | 2.14 | 5.90 | 3.88 | 204.8 | 300.0 |
| 2 | 300 | 20 | xdpP | 115.5 | 54.7 | 53.5 | 2.2 | 2.16 | 6.06 | 3.93 | 191.8 | 291.1 |
| 8 | 300 | 0 | kern | 188.1 | 47.3 | 146.1 | 10.0 | 0.95 | 9.71 | 7.96 | 226.0 | 318.5 |
| 8 | 300 | 0 | kernx | 174.2 | 45.2 | 132.7 | 10.0 | 0.95 | 10.01 | 8.27 | 226.0 | 315.0 |
| 8 | 300 | 0 | xdp | 111.6 | 56.7 | 77.5 | 4.3 | 2.78 | 8.74 | 6.80 | 228.2 | 314.9 |
| 8 | 300 | 20 | kern | 216.2 | 69.3 | 132.0 | 10.0 | 0.95 | 10.26 | 8.61 | 248.7 | 342.7 |
| 8 | 300 | 20 | xdp | 145.7 | 78.2 | 69.4 | 4.4 | 2.81 | 8.73 | 6.89 | 247.4 | 345.1 |
| 8 | 1200 | 20 | kern | 183.0 | 63.7 | 133.7 | 10.0 | 0.95 | 10.11 | 8.42 | 237.7 | 336.7 |
| 8 | 1200 | 20 | xdp | 149.8 | 78.0 | 87.1 | 4.4 | 2.81 | 9.00 | 7.00 | 246.2 | 349.5 |

Box CPU at human pace comes from 10 ms ticks over 45 s windows (about 4500 moves), so single repetitions vary by up to +-40 us; the full tables below give each cell's range.
The server and syscall columns are exact.

## Headline: saturation (warm path, many moves per pass)

The `s300_w0` cells and `k8_s1200_w20` use repetitions 1 and 2 only (see the failure analysis: repetition 3 ran under network shaping).

| K | frame | spin | backend | box CPU us/move | server CPU us/move | softirq us/move | syscalls/move | moves/s | p50 us | p99 us |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 300 | 0 | kern | 10.5 | 7.5 | 5.0 | 3.01 | 133534 | 1116 | 1708 |
| 2 | 300 | 0 | kernx | 10.4 | 7.2 | 5.3 | 3.01 | 137830 | 1080 | 1762 |
| 2 | 300 | 0 | xdp | 6.6 | 4.6 | 2.9 | 0.70 | 108494 | 1373 | 1623 |
| 2 | 300 | 20 | kern | 39.8 | 32.1 | 11.2 | 3.0 | 31097 | 4801 | 5176 |
| 2 | 300 | 20 | xdp | 28.7 | 26.1 | 3.3 | 0.0 | 38287 | 3169 | 5695 |
| 8 | 300 | 0 | kern | 32.6 | 23.5 | 14.7 | 9.04 | 42430 | 889 | 1096 |
| 8 | 300 | 0 | kernx | 32.6 | 22.7 | 16.2 | 9.04 | 43906 | 863 | 1056 |
| 8 | 300 | 0 | xdp | 24.3 | 17.3 | 10.0 | 1.89 | 29206 | 1287 | 1529 |
| 8 | 300 | 20 | kern | 61.8 | 48.3 | 21.0 | 9.0 | 20668 | 1830 | 2011 |
| 8 | 300 | 20 | xdp | 41.4 | 35.9 | 7.2 | 0.1 | 27870 | 1357 | 1754 |
| 8 | 1200 | 20 | kern | 65.0 | 51.5 | 22.1 | 9.03 | 19389 | 1959 | 2122 |
| 8 | 1200 | 20 | xdp | 43.3 | 41.0 | 5.7 | 0.16 | 24166 | 1570 | 2262 |

At spin 0 the xdp backend does fewer moves per second than kernel TCP while using less CPU per move, and the box is not CPU-bound (xdp box CPU 27 to 37%); the single-threaded client used 64 to 77% of its thread in these runs, so the spin-0 throughput numbers are partly client-bound and the closed loop's round trip limits them; the per-move CPU numbers stand.

## Does the 10-15 us hypothesis hold?

No, not as stated.
- Warm, with many moves batched per pass (saturation), AF_XDP + lwIP cuts the per-move network cost by a third to a half: spin 0, box CPU per move 10.5 -> 6.6 us at K=2 and 32.6 -> 24.3 us at K=8; with the 20 us handler, 39.8 -> 28.7 (K=2) and 61.8 -> 41.4 (K=8), i.e. the network part (box minus handler) goes from about 20 to 9 us at K=2 and about 42 to 21 us at K=8. The syscalls disappear (3 -> 0.7 per move at K=2, 9 -> 0.1 to 1.9 at K=8). At K=2 the warm cost does land near 10 us; at K=8 it does not, because lwIP's per-segment work (checksums in software, pbuf copies, one UMEM copy per segment) replaces the kernel's.
- At human pace, which is production, every move arrives at a parked box: the move costs 83 to 150 us of box CPU with AF_XDP + lwIP against 118 to 216 us with kernel TCP. The saving is 26 to 76 us per move (K=2: 26 to 35, K=8: 33 to 76), most of it softirq (the kernel's TCP RX and ACK processing).
- The cold start stays: idle exits per move only fall from 6.8 to 5.8 (K=2) and 10.1 to 8.7 (K=8), and NIC interrupts per move from 4.7 to 3.9 and 8.4 to 6.9.
- lwIP adds wake-ups: context switches per move go from 0.95 to 2.1 (K=2) and 2.8 (K=8), because the clients' pure ACKs (one per frame sent) now wake the user thread through the RX ring, where kernel TCP consumed them in softirq without waking the server.
- XDP alone costs nothing measurable: kernx is within noise of kern everywhere (the program runs on every packet; 3 per-cpu map updates).
- `xdpP` (no `sendto()` kick, the next `poll()` transmits) saves about 0.9 syscalls per move at human pace and is otherwise within noise; the `sendto()` kick is not where the cost is.
- Latency at human pace is the same: p50 175 vs 193 us (K=2, spin 0) and within 10 us elsewhere; the client's kernel and the wire dominate.

## Credit cost

At about 100 moves/s the whole box used 0.47 to 0.81% CPU with xdp and 0.57 to 1.18% with kernel TCP (both vCPUs, so a credit burn of that fraction of 2 vCPUs); the t4g.nano baseline is 5% per vCPU, so both stay far below it and neither burns credits.
Idle with 300 connections open, the xdp server costs 254 to 275 us of CPU per second (lwIP's 250 ms TCP timer walking 300 PCBs, 4 wakes a second) against 7 to 11 us for the kernel server; box idle exits per second 74 to 87 (xdp) against 65 to 94 (kernel): no continuous polling, no measurable credit cost at idle.
Saturation runs are not a credit question (unlimited mode, short windows).

## Correctness

| Run | Moves | Lost | Dup | Gaps | Bad bytes | p99 | Notes |
|---|---|---|---|---|---|---|---|
| smoke_xdp_k2_300 | 10000 | 0 | 0 | 0 | 0 | 285 us | zero-copy, both queues |
| smoke_xdp_k8_1200 | 10001 | 0 | 0 | 0 | 0 | 402 us | |
| smoke_xdpP_k2_300 | 10000 | 0 | 0 | 0 | 0 | 283 us | poll as the TX kick |
| drop_rx_xdp_k2_300 (XDP drops 1% inbound) | 10000 | 0 | 0 | 0 | 0 | 417 us | 184 drops; the client's Linux retransmits |
| drop_tx_xdp_k2_300 (lwIP drops 1% outbound) | 10000 | 0 | 0 | 0 | 0 | 1.496 s | 218 drops; lwIP's own retransmission |
| drop_both_xdp_k8_1200 (1% each way) | 10000 | 0 | 0 | 0 | 0 | 1.500 s | 649 + 846 drops |
| smoke_kern / smoke_kernx | 10000 each | 0 | 0 | 0 | 0 | 308 / 403 us | |
| every hum and sat run except the two below | 2.5k to 2.7M each | 0 | 0 | 0 | 0 | | |

Every frame was checked by the client: length, group, mover, the move's send time, a per-connection server sequence number (no gap, no duplicate) and every body byte.
The p99 of 1.5 s with outbound loss is lwIP's retransmission timeout: its TCP timers run from a 500 ms slow timer and it has no SACK, RACK or tail-loss probe; on a sparse flow the lost segment waits for the retransmission timeout, observed here at about 1.5 s, instead of Linux's 200 ms minimum.
The `rexmit` counter in the server undercounts under simulated outbound drops (a segment dropped before it is sent never reaches the counter, so its resend looks new); it is exact for losses on the wire.

## The failure: sat_k8_s300_w0_xdp_r3 (lost 16) and sat_k2_s300_w0_xdp_r3 (lost 89)

These are the only two runs out of 148 with any error, and the only errors are `lost` (moves not completed when the client gave up 10 s after the window); dup, gaps, bad bytes and bad metadata are 0.

What the counters say:
- The server handled every move and wrote every frame (k8: moves 37857, frames 302856 = 8 x 37857; k2: 153385 and 306770 = 2 x 153385); `tx_full` 0 (the TX frame pool and the TX ring never filled), `rx_nomem` 0 (pbuf allocation never failed), `xdp_other` 0 (no redirect into a full or missing socket), xsk RX ring: no frames refused, zero-copy on both queues as in every run. So it is not a ring overflow, not the pbuf pool, not the RX ring, not TX completions and not the MTU (3498 throughout, frames are one segment).
- The client received fewer frames than the server sent (k2: 306638 of 306770), with no sequence gaps: each affected stream simply stopped short, which is TCP holding later bytes behind an unrepaired loss, not data lost or reordered by the stack.
- The server retransmitted 302 and 337 segments in the window (651 and 647 over the run); healthy xdp runs retransmit 0.
- The slowest completed move took 2.5 s (k2) and 10.5 s (k8): moves were still trickling in at the drain deadline.

Why there was loss: the nano's network allowance ran out.
- Throughput in the two failed runs, and in kernel TCP's runs just before them (`kernx`), collapsed to 31 to 32 Mbit/s of outbound wire traffic (k2 xdp 32.0, k8 xdp 32.0, k8 kernx 31.0, k2 kernx 47.4 counting one header per frame, an overestimate for the kernel, which coalesces frames into fewer segments), which is the t4g.nano's baseline network bandwidth (0.032 Gbps; "up to 5 Gbps" is a burst).
- The run just before, `sat_k8_s1200_w20` (kernx and xdp in repetition 3), pushed 1.59 Gbit/s outbound for 25 s each, after two earlier repetitions of the same; in repetition 3 both stacks were capped at the same 155.8k frames/s where xdp had done 190k to 200k in repetitions 1 and 2, and xdp already retransmitted 351 segments there.
- Both stacks degraded identically in rate; the kernel backend's runs show p99 311 ms and 316 ms and max 6.8 s, so the kernel also saw heavy loss.
- ENA's `bw_out_allowance_exceeded` counter was captured only at the start (all allowance counters 0) and the box terminated itself before it could be read again, so this is an inference from the rate, not a counter; it fits every number.

Why lwIP lost moves where kernel TCP did not: under the same shaping, kernel TCP (SACK, RACK-TLP, 200 ms minimum RTO, pacing) delivered every move within the drain; lwIP (no SACK, 500 ms timer tick, exponential RTO backoff, fast retransmit only after 3 duplicate ACKs, which sparse per-connection traffic rarely produces) did not finish repairing 89 and 16 streams within 10 s.
This is not a correctness bug in the prototype (bytes, order and sequence were right), but it is a real robustness gap: lwIP's loss recovery is orders of magnitude slower than Linux's, and on the CloudFront path a lost segment would freeze a player's stream for seconds.
The repetition-3 saturation cells affected (`k8_s1200_w20` partly, both `s300_w0` cells fully, for kernx and xdp) are excluded from the saturation headline; the human-pace runs all ran before or between saturation blocks and are unaffected (0 retransmits).

## Verdict: is it worth building into cbase?

No.
- The saving is real but small in absolute terms: 26 to 76 us of CPU per move at human pace, about 25 to 40% of the network-side cost; at production's few moves a minute that is nothing, and even at 100 moves/s the box already sits under 1.2% CPU.
- It does not touch the dominant cost: the cold start (idle exits, interrupts) stays, and lwIP adds a user-space wake per ACK.
- It swaps Linux TCP for a stack whose loss recovery is measured here at about 1.5 s per lost segment and that failed to repair 89 streams within 10 s under shaping that Linux rode out; cbase's clients come over the internet through CloudFront, where loss is normal.
- What building it would take:
  - the IO-layer swap: cbase's epoll loop would own an AF_XDP/lwIP netif, and every accepted connection, HTTP upgrade and WebSocket frame would go through `tcp_write`/`tcp_recv` callbacks instead of fds; TLS, if the origin ever uses HTTPS, would need a user-space TLS over lwIP;
  - TCP hardening: SACK in and out, RACK/TLP or at least a finer timer, window scaling, keepalive, SYN-flood protection, PMTU handling, and fuzzing of all of it, because cbase would then own TCP the way it owns its database layer;
  - IPv6: lwIP does no neighbour discovery here (the prototype writes the router/neighbour MAC the kernel uses); production would need NDP with the kernel's neighbour table or lwIP's ND6 on a shared address, plus handling of router MAC changes;
  - SSM coexistence: proven fine in this spike (only the test port is redirected), but the production NIC would carry an XDP program and an MTU of at most 3502, and a crashed server leaves TCP to the port unanswered until it restarts (the program falls back to XDP_PASS only when no socket is bound);
  - ops risk: a second TCP stack on the production box, kernel/driver coupling (the out-of-tree ENA 2.17.4g zero-copy path), and no `ss`, `tcpdump`-at-the-socket or kernel TCP stats for the game connections.
- Cheaper ways to win most of the same CPU, from the feasibility report and this data: the kernel-side cost per move at human pace is mostly softirq on ACKs and RX, so ENA coalescing and IRQ affinity (one vCPU for the NIC and the loop) and io_uring batched sends are the levers to measure next; none needs a user-space TCP.

## Open questions for deeper analysis

- Why xdp's spin-0 saturation throughput is lower (108k vs 134k moves/s at K=2) with the box at 27 to 37% CPU; the client thread was 64 to 77% busy, so instrument or parallelise the client first.
- Where lwIP's 4.6 to 17 us per move at saturation goes (checksums, pbuf copies, `tcp_output` per connection); a `sample`-style profile of `xsrv` would split it.
- The hum box CPU numbers carry 10 ms tick noise (ranges in the full tables); a finer box measure (per-cpu `schedstat` plus irq time accounting) would tighten them.
- Read `bw_out_allowance_exceeded` at the end of every run next time.

## Full tables (from analyze.py)

### CPU per move (hum)

| K | size | spin | backend | server CPU us/move | box CPU us/move | box CPU us/move above idle | softirq us/move | irq us/move | syscalls/move | ctx sw/move | idle exits/move | NIC irqs/move | moves/s | box CPU % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 300 | 20 | kern | 51.0 (50.8-51.5) | 134.9 (86.3-172.2) | 122.7 (73.0-149.0) | 83.3 (50.9-99.7) | 0.0 (0.0-0.0) | 4.0 (4.0-4.0) | 0.95 (0.95-0.95) | 6.77 (6.66-6.89) | 4.74 (4.65-4.84) | 100 (100-101) | 0.68 (0.43-0.87) |
| 2 | 300 | 20 | kernx | 52.0 (48.3-54.0) | 151.1 (116.0-199.5) | 137.8 (101.2-196.1) | 94.9 (86.8-110.8) | 0.0 (0.0-0.0) | 4.0 (4.0-4.0) | 0.95 (0.95-0.95) | 6.86 (6.80-6.95) | 4.80 (4.75-4.89) | 100 (100-100) | 0.76 (0.58-1.00) |
| 2 | 300 | 20 | xdp | 63.2 (56.8-70.2) | 107.1 (91.2-130.7) | 97.1 (67.8-124.1) | 49.5 (42.3-55.4) | 0.0 (0.0-0.0) | 3.1 (3.1-3.2) | 2.14 (2.09-2.21) | 5.90 (5.82-5.98) | 3.88 (3.85-3.91) | 100 (100-101) | 0.54 (0.46-0.66) |
| 2 | 300 | 20 | xdpP | 54.7 (50.0-61.0) | 125.5 (102.4-139.5) | 115.5 (79.0-134.6) | 53.5 (44.5-58.3) | 0.0 (0.0-0.0) | 2.2 (2.2-2.3) | 2.16 (2.12-2.22) | 6.06 (5.98-6.14) | 3.93 (3.90-3.94) | 100 (99-100) | 0.63 (0.51-0.70) |
| 2 | 300 | 0 | kern | 31.3 (27.1-37.6) | 130.2 (118.1-149.7) | 118.0 (94.7-136.3) | 90.0 (82.4-102.7) | 0.0 (0.0-0.0) | 4.0 (4.0-4.0) | 0.95 (0.95-0.96) | 6.79 (6.77-6.82) | 4.70 (4.65-4.75) | 100 (99-100) | 0.65 (0.59-0.74) |
| 2 | 300 | 0 | kernx | 28.3 (24.7-30.5) | 113.3 (99.7-131.3) | 99.9 (94.6-108.9) | 82.2 (68.7-102.4) | 0.0 (0.0-0.0) | 4.0 (4.0-4.0) | 0.95 (0.95-0.96) | 6.80 (6.73-6.86) | 4.67 (4.61-4.74) | 100 (100-100) | 0.57 (0.50-0.66) |
| 2 | 300 | 0 | xdp | 38.7 (32.6-44.2) | 93.3 (71.4-121.8) | 83.3 (63.4-115.1) | 57.8 (53.5-60.1) | 0.0 (0.0-0.0) | 3.1 (3.1-3.1) | 2.09 (2.06-2.12) | 5.84 (5.77-5.88) | 3.87 (3.85-3.89) | 100 (100-100) | 0.47 (0.36-0.61) |
| 2 | 1200 | 20 | kern | 48.9 (47.7-50.6) | 135.3 (117.5-168.1) | 123.0 (94.3-168.1) | 86.5 (73.2-108.4) | 0.0 (0.0-0.0) | 4.0 (4.0-4.0) | 0.96 (0.95-0.96) | 6.82 (6.75-6.90) | 4.76 (4.69-4.83) | 100 (100-100) | 0.68 (0.59-0.84) |
| 2 | 1200 | 20 | kernx | 52.8 (49.7-57.2) | 142.5 (102.7-173.6) | 129.1 (102.7-170.3) | 89.8 (78.1-97.8) | 0.0 (0.0-0.0) | 4.0 (4.0-4.0) | 0.95 (0.95-0.95) | 6.77 (6.68-6.87) | 4.75 (4.66-4.84) | 100 (100-100) | 0.71 (0.51-0.87) |
| 2 | 1200 | 20 | xdp | 60.1 (56.0-65.8) | 107.5 (71.0-129.0) | 97.6 (47.7-122.7) | 49.7 (28.8-64.5) | 0.0 (0.0-0.0) | 3.1 (3.0-3.2) | 2.11 (2.04-2.23) | 5.97 (5.86-6.03) | 3.85 (3.71-3.95) | 100 (100-100) | 0.54 (0.36-0.64) |
| 8 | 300 | 20 | kern | 69.3 (64.9-73.4) | 228.2 (195.4-257.9) | 216.2 (182.3-235.0) | 132.0 (118.6-139.9) | 0.0 (0.0-0.0) | 10.0 (9.9-10.0) | 0.95 (0.94-0.95) | 10.26 (10.15-10.47) | 8.61 (8.50-8.78) | 102 (101-102) | 1.16 (0.99-1.31) |
| 8 | 300 | 20 | kernx | 73.3 (70.2-76.3) | 231.5 (225.2-242.5) | 218.4 (206.4-225.2) | 155.8 (143.9-166.2) | 0.0 (0.0-0.0) | 10.0 (10.0-10.0) | 0.95 (0.95-0.95) | 10.18 (9.78-10.61) | 8.65 (8.29-9.06) | 102 (102-102) | 1.18 (1.14-1.23) |
| 8 | 300 | 20 | xdp | 78.2 (72.5-83.6) | 155.6 (131.5-177.8) | 145.7 (108.5-171.2) | 69.4 (48.2-87.8) | 0.0 (0.0-0.0) | 4.4 (4.4-4.5) | 2.81 (2.80-2.82) | 8.73 (8.48-8.92) | 6.89 (6.51-7.16) | 101 (101-102) | 0.79 (0.67-0.90) |
| 8 | 300 | 20 | xdpP | 79.3 (78.4-79.8) | 145.0 (127.8-157.7) | 135.1 (104.7-157.7) | 85.7 (72.7-94.2) | 0.0 (0.0-0.0) | 3.3 (3.2-3.5) | 2.77 (2.69-2.87) | 8.65 (8.11-9.06) | 6.89 (6.45-7.50) | 101 (101-101) | 0.73 (0.64-0.80) |
| 8 | 300 | 0 | kern | 47.3 (46.0-48.1) | 200.1 (176.9-222.4) | 188.1 (154.0-222.4) | 146.1 (139.9-156.4) | 0.0 (0.0-0.0) | 10.0 (10.0-10.0) | 0.95 (0.95-0.96) | 9.71 (9.40-10.26) | 7.96 (7.67-8.49) | 101 (101-102) | 1.01 (0.90-1.12) |
| 8 | 300 | 0 | kernx | 45.2 (43.7-46.7) | 187.3 (155.3-220.8) | 174.2 (119.2-217.5) | 132.7 (107.2-161.7) | 0.0 (0.0-0.0) | 10.0 (10.0-10.0) | 0.95 (0.95-0.95) | 10.01 (9.73-10.51) | 8.27 (7.99-8.80) | 102 (102-102) | 0.95 (0.79-1.12) |
| 8 | 300 | 0 | xdp | 56.7 (55.8-57.2) | 121.4 (108.2-132.0) | 111.6 (101.3-125.4) | 77.5 (66.2-89.3) | 0.0 (0.0-0.0) | 4.3 (4.2-4.5) | 2.78 (2.71-2.89) | 8.74 (8.62-8.85) | 6.80 (6.35-7.38) | 101 (101-102) | 0.61 (0.54-0.67) |
| 8 | 1200 | 20 | kern | 63.7 (59.4-67.2) | 195.1 (162.4-212.4) | 183.0 (139.3-212.4) | 133.7 (109.7-157.9) | 0.0 (0.0-0.0) | 10.0 (10.0-10.0) | 0.95 (0.94-0.95) | 10.11 (9.63-10.76) | 8.42 (7.93-9.04) | 101 (101-101) | 0.99 (0.82-1.08) |
| 8 | 1200 | 20 | kernx | 74.1 (71.0-79.6) | 213.7 (199.6-227.0) | 200.5 (163.4-227.0) | 133.2 (120.3-142.5) | 0.0 (0.0-0.0) | 10.0 (10.0-10.0) | 0.95 (0.95-0.96) | 10.09 (9.67-10.35) | 8.53 (8.13-8.84) | 101 (101-102) | 1.08 (1.01-1.14) |
| 8 | 1200 | 20 | xdp | 78.0 (77.4-78.9) | 159.7 (135.5-182.3) | 149.8 (112.6-175.7) | 87.1 (75.1-107.6) | 0.0 (0.0-0.0) | 4.4 (4.3-4.5) | 2.81 (2.71-2.90) | 9.00 (8.71-9.25) | 7.00 (6.73-7.42) | 101 (101-102) | 0.81 (0.69-0.92) |

### latency and integrity (hum)

| K | size | spin | backend | p50 us | p95 us | p99 us | max us | lost | dup | gaps | bad bytes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 300 | 20 | kern | 209.5 (205.6-213.6) | 271.9 (264.7-277.9) | 299.2 (291.2-303.3) | 410 (329-465) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 20 | kernx | 215.1 (206.4-223.5) | 278.5 (266.9-292.2) | 305.0 (294.9-321.2) | 526 (357-804) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 20 | xdp | 204.8 (197.2-214.3) | 270.9 (257.5-285.7) | 300.0 (286.2-318.0) | 410 (359-444) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 20 | xdpP | 191.8 (183.3-205.4) | 255.2 (242.2-271.8) | 291.1 (272.6-306.1) | 678 (419-885) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 0 | kern | 192.8 (181.9-208.9) | 252.3 (240.3-267.3) | 282.9 (273.0-298.4) | 421 (375-481) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 0 | kernx | 192.3 (182.8-197.6) | 257.1 (250.8-262.4) | 285.8 (276.0-292.4) | 411 (374-440) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 0 | xdp | 175.2 (162.6-184.2) | 236.0 (220.8-247.4) | 264.6 (249.0-273.9) | 623 (329-1122) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 1200 | 20 | kern | 204.3 (199.3-211.1) | 263.9 (255.7-278.7) | 295.3 (281.8-322.1) | 420 (378-441) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 1200 | 20 | kernx | 216.5 (208.9-228.9) | 281.3 (269.4-294.6) | 307.7 (293.1-319.8) | 622 (374-1054) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 1200 | 20 | xdp | 200.5 (194.3-209.2) | 266.5 (259.7-278.2) | 300.5 (292.1-311.4) | 461 (411-513) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | kern | 248.7 (241.8-254.5) | 308.6 (298.3-317.4) | 342.7 (330.8-355.4) | 1149 (696-1471) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | kernx | 259.8 (253.1-263.4) | 320.9 (308.8-328.8) | 362.2 (344.7-375.2) | 505 (480-532) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | xdp | 247.4 (239.4-253.8) | 311.4 (300.2-319.1) | 345.1 (338.1-352.3) | 471 (458-483) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | xdpP | 251.3 (249.7-252.1) | 317.7 (316.1-318.8) | 355.5 (351.4-358.5) | 563 (418-795) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 0 | kern | 226.0 (224.6-226.9) | 285.5 (283.3-288.4) | 318.5 (315.8-320.6) | 583 (479-741) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 0 | kernx | 226.0 (225.4-226.7) | 284.0 (278.2-292.2) | 315.0 (306.6-319.8) | 1168 (455-1735) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 0 | xdp | 228.2 (225.6-231.7) | 286.5 (284.4-289.0) | 314.9 (311.2-318.1) | 569 (380-898) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 1200 | 20 | kern | 237.7 (230.9-242.8) | 295.0 (281.4-303.9) | 336.7 (322.3-347.7) | 509 (480-544) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 1200 | 20 | kernx | 259.4 (252.6-267.5) | 322.7 (316.9-332.5) | 365.4 (352.9-375.6) | 733 (488-1071) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 1200 | 20 | xdp | 246.2 (244.1-248.2) | 312.1 (309.3-315.9) | 349.5 (343.9-354.3) | 439 (423-452) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |

### CPU per move (sat)

| K | size | spin | backend | server CPU us/move | box CPU us/move | box CPU us/move above idle | softirq us/move | irq us/move | syscalls/move | ctx sw/move | idle exits/move | NIC irqs/move | moves/s | box CPU % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 300 | 20 | kern | 32.1 (32.0-32.3) | 39.8 (39.5-40.0) | 39.7 (39.5-39.9) | 11.2 (10.7-11.6) | 0.0 (0.0-0.0) | 3.0 (3.0-3.0) | 0.00 (0.00-0.00) | 0.53 (0.52-0.55) | 1.05 (1.01-1.07) | 31097 (30933-31203) | 61.83 (61.65-61.98) |
| 2 | 300 | 20 | kernx | 32.9 (32.1-34.2) | 40.8 (38.9-42.9) | 40.8 (38.9-42.9) | 11.8 (10.5-12.8) | 0.0 (0.0-0.0) | 3.0 (3.0-3.0) | 0.00 (0.00-0.00) | 0.57 (0.55-0.60) | 1.08 (1.05-1.14) | 30374 (29172-31154) | 61.96 (60.62-62.65) |
| 2 | 300 | 20 | xdp | 26.1 (25.9-26.6) | 28.7 (27.9-29.5) | 28.7 (27.9-29.5) | 3.3 (2.6-4.0) | 0.0 (0.0-0.0) | 0.0 (0.0-0.0) | 0.00 (0.00-0.00) | 0.31 (0.13-0.41) | 0.44 (0.43-0.45) | 38287 (37625-38677) | 54.99 (52.45-57.00) |
| 2 | 300 | 20 | xdpP | 26.3 (26.0-27.0) | 28.5 (27.4-29.0) | 28.4 (27.3-29.0) | 2.8 (1.7-3.4) | 0.0 (0.0-0.0) | 0.0 (0.0-0.0) | 0.00 (0.00-0.00) | 0.28 (0.04-0.40) | 0.45 (0.43-0.47) | 37975 (36982-38496) | 54.06 (50.67-55.75) |
| 2 | 300 | 0 | kern | 7.5 (7.4-7.5) | 10.4 (10.4-10.6) | 10.4 (10.4-10.6) | 5.0 (4.9-5.1) | 0.0 (0.0-0.0) | 3.0 (3.0-3.0) | 0.00 (0.00-0.00) | 0.11 (0.11-0.11) | 0.21 (0.21-0.21) | 133698 (132755-134313) | 69.82 (69.52-70.33) |
| 2 | 300 | 0 | kernx | 8.2 (7.2-10.2) | 24.6 (10.2-53.1) | 24.6 (10.2-53.1) | 18.2 (4.9-44.1) | 0.0 (0.0-0.0) | 3.2 (3.0-3.7) | 0.21 (0.00-0.63) | 0.89 (0.10-2.46) | 0.89 (0.20-2.28) | 94526 (7919-138383) | 54.90 (21.03-73.43) |
| 2 | 300 | 0 | xdp | 6.6 (4.6-10.5) | 18.8 (6.4-43.2) | 18.6 (6.4-42.6) | 13.5 (2.9-34.9) | 0.0 (0.0-0.0) | 1.1 (0.7-2.0) | 0.39 (0.09-0.98) | 1.33 (0.21-3.56) | 1.22 (0.18-3.29) | 74110 (5344-108933) | 27.87 (11.54-37.12) |
| 2 | 1200 | 20 | kern | 33.9 (33.5-34.4) | 43.2 (42.6-44.1) | 43.1 (42.5-44.1) | 13.1 (12.4-14.2) | 0.0 (0.0-0.0) | 3.0 (3.0-3.0) | 0.00 (0.00-0.00) | 0.63 (0.58-0.69) | 1.18 (1.11-1.27) | 29477 (29049-29775) | 63.66 (62.32-65.65) |
| 2 | 1200 | 20 | kernx | 33.4 (33.3-33.5) | 42.1 (41.4-42.9) | 42.1 (41.4-42.9) | 12.4 (11.8-13.0) | 0.0 (0.0-0.0) | 3.0 (3.0-3.0) | 0.00 (0.00-0.00) | 0.60 (0.59-0.61) | 1.15 (1.13-1.19) | 29923 (29857-29984) | 62.98 (61.75-64.23) |
| 2 | 1200 | 20 | xdp | 28.0 (27.3-28.5) | 30.5 (29.0-31.3) | 30.5 (29.0-31.2) | 3.2 (1.9-4.2) | 0.0 (0.0-0.0) | 0.0 (0.0-0.0) | 0.00 (0.00-0.00) | 0.30 (0.05-0.44) | 0.48 (0.46-0.51) | 35763 (35028-36629) | 54.54 (50.78-57.15) |
| 8 | 300 | 20 | kern | 48.3 (48.1-48.6) | 61.8 (61.4-62.2) | 61.7 (61.4-62.2) | 21.0 (20.8-21.3) | 0.0 (0.0-0.0) | 9.0 (9.0-9.0) | 0.00 (0.00-0.00) | 0.79 (0.78-0.80) | 1.52 (1.51-1.53) | 20668 (20563-20779) | 63.84 (63.73-63.97) |
| 8 | 300 | 20 | kernx | 48.6 (48.0-49.0) | 61.8 (61.4-62.3) | 61.7 (61.4-62.2) | 21.4 (21.3-21.6) | 0.0 (0.0-0.0) | 9.0 (9.0-9.0) | 0.01 (0.00-0.01) | 0.79 (0.76-0.81) | 1.52 (1.49-1.54) | 20537 (20368-20814) | 63.42 (62.98-63.87) |
| 8 | 300 | 20 | xdp | 35.9 (35.5-36.5) | 41.4 (41.1-41.9) | 41.4 (41.1-41.9) | 7.2 (6.3-7.9) | 0.0 (0.0-0.0) | 0.1 (0.1-0.1) | 0.00 (0.00-0.00) | 0.53 (0.42-0.58) | 0.72 (0.71-0.74) | 27870 (27357-28129) | 57.74 (56.20-58.97) |
| 8 | 300 | 20 | xdpP | 36.5 (35.5-37.5) | 40.7 (38.5-42.0) | 40.6 (38.4-42.0) | 6.5 (4.3-8.0) | 0.0 (0.0-0.0) | 0.1 (0.1-0.1) | 0.00 (0.00-0.00) | 0.39 (0.15-0.57) | 0.73 (0.71-0.74) | 27357 (26669-28118) | 55.68 (51.33-59.00) |
| 8 | 300 | 0 | kern | 23.3 (22.8-23.5) | 32.4 (32.0-32.7) | 32.3 (31.9-32.7) | 14.6 (14.4-14.8) | 0.0 (0.0-0.0) | 9.0 (9.0-9.0) | 0.00 (0.00-0.00) | 0.34 (0.32-0.34) | 0.64 (0.63-0.65) | 42891 (42420-43812) | 69.43 (68.65-70.15) |
| 8 | 300 | 0 | kernx | 25.0 (22.5-29.6) | 93.5 (32.4-215.1) | 93.4 (32.4-215.1) | 74.8 (15.9-192.0) | 0.0 (0.0-0.0) | 9.4 (9.0-10.0) | 0.34 (0.00-1.00) | 4.58 (0.32-13.11) | 4.65 (0.62-12.72) | 29702 (1295-44295) | 52.37 (13.93-72.60) |
| 8 | 300 | 0 | xdp | 24.1 (16.1-37.7) | 69.1 (23.4-158.6) | 68.3 (23.4-156.4) | 52.7 (9.9-138.0) | 0.0 (0.0-0.0) | 3.0 (1.9-5.3) | 1.65 (0.33-4.25) | 4.97 (0.71-13.33) | 4.57 (0.66-12.36) | 19916 (1337-29268) | 27.20 (10.60-36.92) |
| 8 | 1200 | 20 | kern | 51.3 (50.7-51.7) | 64.4 (63.1-65.9) | 64.3 (63.0-65.9) | 21.3 (19.8-22.9) | 0.0 (0.0-0.0) | 9.0 (9.0-9.0) | 0.01 (0.00-0.01) | 0.83 (0.81-0.84) | 1.59 (1.57-1.62) | 19489 (19331-19690) | 62.71 (62.15-63.73) |
| 8 | 1200 | 20 | kernx | 51.0 (50.3-51.5) | 64.8 (64.8-65.0) | 64.8 (64.8-64.9) | 23.1 (21.4-25.2) | 0.0 (0.0-0.0) | 9.0 (9.0-9.0) | 0.01 (0.00-0.01) | 0.82 (0.80-0.84) | 1.59 (1.56-1.61) | 19587 (19413-19870) | 63.51 (62.87-64.55) |
| 8 | 1200 | 20 | xdp | 40.0 (38.1-41.9) | 43.5 (40.9-45.6) | 43.5 (40.9-45.6) | 6.3 (4.5-7.5) | 0.0 (0.0-0.0) | 0.1 (0.1-0.2) | 0.01 (0.00-0.02) | 0.41 (0.16-0.69) | 0.84 (0.79-0.91) | 22600 (19468-24981) | 49.08 (42.88-53.27) |

### latency and integrity (sat)

| K | size | spin | backend | p50 us | p95 us | p99 us | max us | lost | dup | gaps | bad bytes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 300 | 20 | kern | 4801.2 (4798.7-4805.4) | 5040.0 (4899.2-5221.1) | 5176.0 (4999.2-5403.5) | 9660 (9586-9784) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 20 | kernx | 4922.9 (4791.4-5121.7) | 5124.8 (4990.6-5359.8) | 5296.1 (5184.2-5516.4) | 9867 (9626-10174) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 20 | xdp | 3168.9 (3084.0-3263.7) | 5578.4 (5401.9-5684.3) | 5695.4 (5546.7-5794.4) | 6689 (6179-7295) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 20 | xdpP | 3200.7 (3042.3-3317.1) | 5649.8 (5572.4-5791.5) | 5775.3 (5635.2-5896.8) | 7190 (6236-7817) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 0 | kern | 1114.9 (1104.2-1127.0) | 1274.7 (1226.6-1326.6) | 1728.9 (1291.3-2123.9) | 4347 (3319-4994) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 0 | kernx | 1908.6 (1078.8-3566.7) | 8897.4 (1171.7-24190.6) | 104887.4 (1229.0-311138.2) | 2281875 (2700-6839181) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 300 | 0 | xdp | 4715.7 (1369.5-11400.9) | 4812.5 (1504.0-11415.0) | 4915.3 (1621.0-11500.3) | 839198 (2214-2511069) | 30 (0-89) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 1200 | 20 | kern | 5066.1 (5005.6-5157.5) | 5321.9 (5277.5-5402.6) | 5477.4 (5357.3-5547.5) | 10235 (10015-10550) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 1200 | 20 | kernx | 4983.1 (4973.8-4995.2) | 5249.9 (5230.6-5259.9) | 5347.3 (5341.1-5351.2) | 10052 (9970-10105) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 2 | 1200 | 20 | xdp | 3353.9 (3290.4-3397.2) | 6024.1 (5764.6-6357.4) | 6141.1 (5875.6-6498.8) | 7484 (6715-8484) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | kern | 1830.0 (1820.4-1846.4) | 1912.4 (1889.3-1931.3) | 2011.4 (1965.4-2063.2) | 4627 (3879-5121) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | kernx | 1849.7 (1819.1-1866.6) | 1911.6 (1891.6-1922.0) | 1988.9 (1975.8-2014.5) | 3845 (3792-3916) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | xdp | 1356.7 (1344.4-1378.9) | 1628.5 (1588.7-1682.1) | 1754.1 (1700.3-1831.6) | 3938 (3314-4707) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 20 | xdpP | 1384.0 (1343.8-1426.7) | 1664.9 (1620.1-1697.1) | 1783.9 (1728.8-1811.8) | 2875 (2706-3046) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 0 | kern | 879.5 (860.2-889.2) | 966.0 (945.4-982.1) | 1094.3 (1081.3-1110.0) | 3318 (2404-4083) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 0 | kernx | 7009.1 (852.9-19302.2) | 7735.8 (934.9-21325.0) | 106049.1 (1036.7-316034.4) | 344700 (1888-1029778) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 300 | 0 | xdp | 4598.5 (1283.7-11222.2) | 4940.3 (1423.7-11972.8) | 5038.7 (1527.2-12058.8) | 3502456 (2739-10500760) | 5 (0-16) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 1200 | 20 | kern | 1946.3 (1920.8-1962.9) | 2021.4 (1997.2-2048.8) | 2102.8 (2064.8-2142.9) | 4024 (3965-4097) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 1200 | 20 | kernx | 1942.1 (1914.0-1958.8) | 1999.1 (1964.5-2020.0) | 2065.3 (2012.4-2111.6) | 4050 (3974-4095) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |
| 8 | 1200 | 20 | xdp | 1528.3 (1445.8-1610.5) | 1873.5 (1716.2-2094.4) | 2138.1 (1889.6-2581.0) | 2503669 (3190-7502896) | 0 (0-0) | 0 (0-0) | 0 (0-0) | 0 (0-0) |

### idle floors (no moves, connections open)

| label | conns | box CPU us/s | box CPU % | softirq us/s | server us/s | idle exits/s | NIC irqs/s |
|---|---|---|---|---|---|---|---|
| idle_kern_r1_hum | 300 | 2333 | 0.12 | 333 | 7 | 74 | 2.7 |
| idle_kern_r1_sat | 300 | 333 | 0.02 | 0 | 10 | 66 | 1.2 |
| idle_kern_r2_hum | 300 | 0 | 0.00 | 0 | 10 | 69 | 0.9 |
| idle_kern_r2_sat | 300 | 333 | 0.02 | 0 | 8 | 65 | 0.9 |
| idle_kern_r3_hum | 300 | 1333 | 0.07 | 0 | 10 | 71 | 0.8 |
| idle_kern_r3_sat | 300 | 3333 | 0.17 | 333 | 10 | 76 | 2.8 |
| idle_kernx_r1_hum | 300 | 3667 | 0.18 | 0 | 10 | 83 | 2.6 |
| idle_kernx_r1_sat | 300 | 333 | 0.02 | 333 | 9 | 94 | 3.7 |
| idle_kernx_r2_hum | 300 | 333 | 0.02 | 0 | 10 | 69 | 0.7 |
| idle_kernx_r2_sat | 300 | 667 | 0.03 | 0 | 8 | 76 | 3.9 |
| idle_kernx_r3_hum | 300 | 0 | 0.00 | 0 | 10 | 66 | 0.7 |
| idle_kernx_r3_sat | 300 | 0 | 0.00 | 0 | 11 | 73 | 0.8 |
| idle_xdp_r1_hum | 300 | 667 | 0.03 | 333 | 271 | 87 | 4.3 |
| idle_xdp_r1_sat | 300 | 333 | 0.02 | 0 | 264 | 77 | 1.0 |
| idle_xdp_r2_hum | 300 | 0 | 0.00 | 0 | 275 | 74 | 1.1 |
| idle_xdp_r2_sat | 300 | 0 | 0.00 | 0 | 261 | 74 | 0.7 |
| idle_xdp_r3_hum | 300 | 2333 | 0.12 | 0 | 262 | 87 | 5.6 |
| idle_xdp_r3_sat | 300 | 3000 | 0.15 | 0 | 254 | 85 | 2.6 |

### correctness and smoke runs

| label | moves sent | completed | lost | dup | gaps | bad bytes | bad meta | p50 us | p99 us | server rexmit segs | xdp drops | lwIP tx drops | syscalls/move (in-app) | syscalls/move (tracepoint) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| coexist_xdp | 2000 | 2000 | 0 | 0 | 0 | 0 | 0 | 195.2 | 302.5 | 0 | 0 | 0 | 0.82 | - |
| drop_both_xdp_k8_1200 | 10000 | 10000 | 0 | 0 | 0 | 0 | 0 | 202.5 | 1500046.6 | 86 | 649 | 846 | 2.86 | - |
| drop_rx_xdp_k2_300 | 10000 | 10000 | 0 | 0 | 0 | 0 | 0 | 150.8 | 417.0 | 0 | 184 | 0 | 1.58 | - |
| drop_tx_xdp_k2_300 | 10000 | 10000 | 0 | 0 | 0 | 0 | 0 | 150.9 | 1496238.4 | 0 | 0 | 218 | 1.51 | - |
| smoke_kern_k2_300 | 10002 | 10002 | 0 | 0 | 0 | 0 | 0 | 197.0 | 307.6 | 0 | 0 | 0 | 3.46 | 3.46 |
| smoke_kern_k8_1200 | 10001 | 10001 | 0 | 0 | 0 | 0 | 0 | 271.4 | 345.2 | 0 | 0 | 0 | 9.49 | 9.50 |
| smoke_kernx_k2_300 | 10000 | 10000 | 0 | 0 | 0 | 0 | 0 | 194.0 | 403.1 | 0 | 0 | 0 | 3.48 | 3.48 |
| smoke_xdpP_k2_300 | 10000 | 10000 | 0 | 0 | 0 | 0 | 0 | 182.9 | 282.6 | 0 | 0 | 0 | 0.54 | 0.54 |
| smoke_xdp_k2_300 | 10000 | 10000 | 0 | 0 | 0 | 0 | 0 | 187.1 | 284.6 | 0 | 0 | 0 | 0.77 | 0.78 |
| smoke_xdp_k8_1200 | 10001 | 10001 | 0 | 0 | 0 | 0 | 0 | 282.9 | 401.7 | 0 | 0 | 0 | 1.81 | 1.81 |

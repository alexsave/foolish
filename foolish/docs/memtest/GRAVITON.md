# The Graviton confirmation of the memory series (2026-11-09)

The throwaway run at the end of the server memory series (foolish #480, #481, #483), against HYPOTHESES.md G1 to G11 and each PR's own hypotheses.
Written after measuring; the predictions were written before, in HYPOTHESES.md and the three PR docs.

## Setup

Server: one fresh t4g.nano (2 vCPU, 412 MiB, AL2023 arm64 ami from the al2023-ami-kernel-default-arm64 parameter, kernel 6.18.56-125.164.amzn2023), unlimited credits for every run but G7.
Load box: one c7g.2xlarge (8 vCPU), same AMI, in the same subnet, over private IPv4 to port 8080.
Network: the default VPC (us-east-1a, subnet-054f60db6d48ca288), not the cbase-vm stack's subnet the morning run used, so nothing of production was in the path.
Access: SSM only, no SSH, no inbound rule but 8080-8081 from the group itself.
Safety: user data scheduled `shutdown -h +80` and the instances had instance-initiated-shutdown-behavior=terminate.
Everything was tagged purpose=cbase-mem-test, owner=cdom.

Builds: production's way, each sha's own `c_base/providers/aws/ops/build_bootstrap.sh serve` (the pinned alpine:3.24 image, `make STATIC=1 build/cbase`, strip), static-pie musl, 8,431,976 bytes each.

| build | sha | what | sha256 of the binary |
|---|---|---|---|
| A | 400a65d097557132a2eb057b4f0833a9b7929ff9 | main before the series (parent of #480's first commit) | 5b717b68...fa6ff09d |
| B | 57a40a1f54eb0e394402c0b0f045d024d4392a20 | after #480 (roster compaction) | 1327ebd8...93c61eff |
| C | 1270832e06bf9b6b1e1768c229855929cf0bdb2d | after #481 (journal state buffer) | e937d581...998ba89 |
| D | f99698319fd6eabade714e901a18912de6b51ca5 | merged main after #483 (bot memory) | d8acf501...88fce |

Server env: the cbase.service shape (CBASE_STORE=sqlite, CBASE_DB, CBASE_LISTEN, SSL_CERT_FILE) plus JWT and admin secrets made on the box and never printed, CBASE_PUBLIC_URL, the bench limits raised (as in the morning run), and CBASE_BOT_SPEEDUP=100 for the bot-heavy load only.
THP was left as the host has it (G6: never), so no prctl wrapper was used, unlike the Docker runs.
Every run started a fresh server with an empty database.
Load clients: built on the load box from f99698319's c_base/bench/nano (byte-identical to 400a65d09's): p3load3.c, a rematch variant of it (grav/pkg/rematch.c, P3REMATCH_MS) and tlsres (grav/pkg/tlsres.c, each thread's TLS block resident pages through ptrace and /proc/PID/pagemap).
Sampler: VmRSS, VmHWM and RssAnon every 0.2 s; p50 and p99 are over the samples in the play window (t_go to t_done on the load box's clock).
Mid capture: status, smaps, smaps_rollup and tlsres 21.8 to 22.2 s after go in every sat2 run (17 to 20 s in the bot runs, 5.5 to 6.2 s in hum2).
Scenarios: sat2 = 8 pairs, think 0-1 ms, 25 s; hum2 = 192 pairs, think 2-4 s, 40 games, 30 s; bot = 64 tables of one human and one octogen, 25 s.
Order, interleaved: sat2 round 1 A B C D, round 2 D C B A, round 3 B D A C; hum2 C A D B; bot C D D C.
Scripts and raw data: foolish/docs/memtest/harness/grav (up.sh, down.sh, run.sh, ssm.sh, pkg/, analyze.py, out/analysis.txt, out/steps.log, results/).

## sat2, three runs per build

| build | moves/s | move p50 ms | move p99 ms | RSS p50 MiB | RSS max MiB | VmHWM MiB | RssAnon mid kB |
|---|---|---|---|---|---|---|---|
| A | 23,898 / 24,021 / 24,059 | 0.192 / 0.189 / 0.189 | 0.540 / 0.534 / 0.530 | 39.9 / 40.8 / 41.7 | 53.6 / 53.9 / 53.4 | 53.8 / 54.0 / 54.0 | 41,652 / 41,704 / 41,708 |
| B | 24,363 / 24,045 / 24,118 | 0.188 / 0.189 / 0.189 | 0.479 / 0.498 / 0.521 | 30.7 / 31.0 / 30.5 | 40.9 / 41.6 / 41.3 | 41.8 / 41.8 / 41.4 | 29,072 / 29,076 / 29,064 |
| C | 24,005 / 24,060 / 24,178 | 0.193 / 0.191 / 0.188 | 0.509 / 0.518 / 0.485 | 23.9 / 24.4 / 24.6 | 31.6 / 31.4 / 31.5 | 31.7 / 31.8 / 31.7 | 18,776 / 18,740 / 18,736 |
| D | 24,234 / 24,112 / 24,180 | 0.190 / 0.191 / 0.192 | 0.491 / 0.484 / 0.485 | 21.3 / 21.2 / 21.4 | 27.4 / 27.5 / 27.3 | 27.5 / 27.7 / 27.7 | 14,668 / 14,668 / 14,708 |

Runs are listed in round order (s1, s2, s3).
Series total on the nano: sat2 RSS p50 40.8 to 21.3 MiB (medians, -48%), RSS max 53.6 to 27.4 MiB, RssAnon 41,704 to 14,668 kB, with moves/s and p99 no worse.

## hum2, one run per build

| build | moves/s | move p50 / p99 ms | RSS p50 MiB | RssAnon mid kB |
|---|---|---|---|---|
| A | 99.0 | 0.212 / 0.356 | 13.4 | 7,632 |
| B | 97.4 | 0.211 / 0.331 | 11.6 | 6,516 |
| C | 97.8 | 0.213 / 0.331 | 11.1 | 5,988 |
| D | 97.8 | 0.217 / 0.324 | 10.9 | 5,800 |

## Bot-heavy load (C and D, two runs each)

| run | humans' moves/s | move p50 / p99 ms | RSS p50 MiB | RssAnon mid kB | job-0 TLS kB | req-0 TLS kB |
|---|---|---|---|---|---|---|
| C1 | 32.5 | 0.193 / 0.410 | 8.6 | 3,548 | 164 | 20 |
| D1 | 34.8 | 0.254 / 0.400 | 8.7 | 3,612 | 152 | 20 |
| D2 | 34.0 | 0.194 / 0.395 | 8.8 | 3,748 | 152 | 20 |
| C2 | 32.6 | 0.172 / 0.283 | 8.8 | 3,744 | 156 | 20 |

## HYPOTHESES.md

- G1 confirmed: RssAnon mid-sat2 fell 12,632 kB from A to B (medians 41,704 to 29,072), 3,158 B per resident game at OV_MAX_GAMES 4,096, against a prediction of about 3,100 B and a refutation threshold of 2,400 B.
- G2 confirmed, below its prediction: sat2 RSS p50 fell 10.1 MiB from A to B (medians 40.8 to 30.7), against a prediction of about 12.5 MiB and a refutation threshold of 9 MiB.
  RSS max fell 12.3 MiB (53.6 to 41.3), the same as Docker's 12.3.
  The p50 falls less than in Docker because the nano's base p50 is lower (40.8 against Docker's 46.9): the median sample comes earlier in the overlay's fill.
- G3 confirmed: every B run is above A's slowest moves/s (23,898), inside A's p50 band (worst 0.192) and below A's best p99 (0.530).
- G4 mostly confirmed, with two exceptions by the letter, listed under the PR hypotheses below: #481 H2's RSS p50 fell 6.3 MiB, above its 6 MiB refutation threshold but below its 8 MiB prediction, and one C run's move p50 (0.193 ms) is 4 us above B's worst against a band of 1 us.
- G5 confirmed: idle VmRSS is 6,984 to 7,048 kB (6.8 to 6.9 MiB) on every build, of which Private_Dirty is 1,988 kB (1.94 MiB) and RssAnon 1,956 kB, against 6.5 to 7.5 and 1.7 to 2.2 MiB.
  The first idle start of build A read 3,432 kB (RssFile 1,476 kB, VmHWM 6,816 kB) with the same 1,956 kB anon: its file pages were dropped from the mapping during the first seconds after boot; its rerun read 7,048 kB.
- G6 confirmed: /sys/kernel/mm/transparent_hugepage/enabled is "always madvise [never]" (defrag "[never]"), AnonHugePages is 0 in every capture, and idle RSS is 6.9 MiB, under 9.
- G7 confirmed: after the switch to standard credits (balance 0, the unlimited runs had spent it), build D at sat2 ran at burst for about 40 s (24.7k, 24.6k, 21.5k, 21.5k moves/s per 10 s), then 4.4k, then 1.4k to 2.4k, settling at 2,200 to 2,350 moves/s over the last 70 s (mean about 2,290, 9.5% of burst), inside 2,000 to 3,500.
  CPU steal on the nano went from 3 to 16,283 jiffies over the 152 s run; move p99 over the whole run was 2.58 ms.
- G8 confirmed: a second instance of build D (port 8081, its own database) took MemAvailable down by 1,632 kB and "used" up by 1,448 kB, against a bound of its Private_Dirty (1,988 kB) plus 0.5 MiB and a refutation threshold of 3 MiB.
  The second instance's 4,996 kB of text and rodata were all Shared_Clean, and its Pss halved the first's file part (5,024 to 2,528 kB).
- G9 refuted on its main claim: under the bot-heavy load job-0 had 152 to 164 kB of its 1,724,064-byte TLS block resident, under the 0.2 MiB refutation threshold (prediction 0.4 to 1.6 MiB).
  Its second claim holds: no other thread's TLS exceeded 20 kB (req-0 20 kB, the main thread 20 kB, the two others 8 kB).
  This load's octogen decisions touch about a tenth of the solver scratch the prediction assumed.
- G10 confirmed: humans-only, req-0 and job-0 had 40 and 36 kB of TLS resident at sat2 and 20 and 8 kB at hum2, under 64 KiB.
- G11 confirmed: build D, 30 pairs rematching each game twice (ASK_CONTINUE timed on the client), 20 rematches per wait: after 2 s p50 0.325 ms (mean 0.359), after 50 s p50 0.380 ms (mean 0.394), after 70 s, past IDLE_EVICT_MS (60 s), p50 0.793 ms (mean 0.863, p90 0.999, max 2.215).
  The eviction adds 0.41 ms at p50 and 0.47 ms in the mean against the 50 s rematch (0.47 and 0.50 against the 2 s one), under the 0.5 ms prediction at p50 and at it in the mean, and under the 2 ms refutation threshold.
  Eviction is inferred from the rule and the timing, not read from the journal's stats.

## The PRs' own hypotheses on real hardware

- #480 H1 confirmed: 3,158 B per game (prediction 2,970, threshold 2,400).
- #480 H2 confirmed: RSS p50 -10.1 MiB (at least 9 predicted, 6 the threshold).
- #480 H3 confirmed: as G3.
- #480 H4 (cbase-bench cpu) not run: a microbench, not a hardware hypothesis, and not in the 60-minute box.
- #481 H1 confirmed: RssAnon -10,332 kB from B to C, 2,583 B per game (Docker 2,592 B, against a 2,720-byte slot).
- #481 H2 confirmed on RssAnon (-10,332 kB, at least 8,000 predicted) and not refuted on RSS p50 (-6.3 MiB, medians 30.7 to 24.4), which is above the 6 MiB threshold but below the 8 MiB prediction; RSS max fell 9.8 MiB.
- #481 H3 confirmed for moves/s and p99 (every C run inside B's band); refuted by the letter for p50: run s1_C's 0.193 ms is 4 us above B's worst (0.189), more than B's 1 us band, while the other two C runs (0.191, 0.188) are inside.
- #481 H4 to H6 (microbench and cbase-bench) not run.
- #483 H1 confirmed: RssAnon -4,072 kB from C to D, 1,018 B per game (prediction 1,024).
- #483 H2 confirmed: RSS p50 -3.1 MiB (medians 24.4 to 21.3; at least 3 predicted, 2.5 the threshold), RssAnon -4,072 kB (at least 3,000).
- #483 H3 confirmed: on the bot-heavy load RSS p50 is 8.6 and 8.8 MiB on C against 8.7 and 8.8 on D, a change under 1 MiB.
- #483 H4 confirmed: every D run is inside or above C's band for moves/s, p50 and p99.
- #483 H6 not decided: with two bot runs a side (the hypothesis names three), D's humans' moves/s (34.8, 34.0) are above C's (32.5, 32.6) and its p99 inside, but run D1's p50 (0.254 ms) is above C's worst (0.193) by more than C's band (0.021); the loads move about 850 human moves a run, so the sample is small.
- #483 H5 and H7 (microbench, bot behaviour gate) not run here: they are not hardware hypotheses, and #483 measured them before merging.

## Cost

Instance time: the nano ran from 22:33:48 to 23:08:20 UTC (34.5 min) and the load box from 22:34:15 to 23:08:20 (34.1 min), inside the 60-minute box.
c7g.2xlarge 34.1 min at $0.289/h: about $0.164.
t4g.nano 34.5 min at $0.0042/h: about $0.002, plus about 10 surplus credits from the unlimited runs (CPUSurplusCreditBalance 8.9 before the switch): about $0.007.
Two public IPv4 addresses for 0.57 h: about $0.006; two 8 GB gp3 root volumes: under $0.001.
S3: 82,727 result objects (the clients' per-player event files) were pushed and fetched one by one: about $0.41 in PUT and $0.03 in GET requests.
Total about $0.62, three times the 20 cents planned, because of the S3 requests; a tar of the results would have made that one PUT.

## Teardown proof

`grav/down.sh` (output in grav/out/teardown.txt): instances i-01fd4ef7421e2d01f (t4g.nano) and i-0963ed8d910d215d0 (c7g.2xlarge) terminated, bucket cbase-mem-test-<suffix> emptied and deleted, security group sg-0f82f2ee4958507ee deleted, role and instance profile cbase-mem-test-role-<suffix> deleted.
Checked by tag and name afterwards: 0 instances (pending, running, stopping or stopped) and 0 volumes tagged purpose=cbase-mem-test, 0 security groups with that tag, 0 buckets, roles or instance profiles named cbase-mem-test*.
Production i-08f26ffb900294ee8 (cbase-vm, t4g.nano): running, launch time 2026-10-19T22:05:23Z unchanged, never addressed by any command of this run.
No cbase-web, cbase-vm, cbase-app-* or foolish stack, cbase-web-* bucket or CloudFront distribution was read or changed.

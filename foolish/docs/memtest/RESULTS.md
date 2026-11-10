# cbase serve memory on a real t4g.nano (2026-11-09)

## Setup

Server build: foolish 284f0dc4f (MAIN of the Graviton pass of 2026-10-21), `make -j8 build/cbase` on the load box (AL2023, gcc, dynamic glibc, system libsqlite3 and OpenSSL 3.5.8), not stripped.
This is not the pass's static Alpine/musl build, so the allocator is glibc malloc, not musl, and shared libs appear as file-backed mappings.
Server box: t4g.nano (unlimited credits, AL2023 arm64 ami-0951c836c26e34fc8, IPv6-only subnet), cbase serve with production defaults plus the rig's srv_up.sh env (sqlite store on the root gp3 volume, CBASE_MAX_CONNS=16384, limits raised), started fresh with an empty DB for every scenario.
Load box: c7g.2xlarge, the pass's client p3load3.c + lib_client_host.c from the same foolish tree, over IPv6 to port 8080, driven by a copy of step.sh's clients.sh logic.
Sampler: VmRSS and VmHWM from /proc/<pid>/status every 0.2 s; p50/p99 are over the samples inside the play window (t_go to t_done on the load box); VmHWM is read from /proc/<pid>/status at the end of the run.
hum2 = 192 two-player pairs, think 2000-4000 ms, 40 games, 30 s (run.sh); sat2 = 8 pairs, think 0-1 ms, 100000 games, 25 s (sat.sh).
hum8 and sat8 were not run: the pass's table counts for them were not in the digest and there was no time to recover them.
Scripts: up.sh, up_resume.sh, ssm.sh, down.sh, pkg/srv.sh, pkg/load.sh, analyze.py; raw data in results/.

## RSS (MiB)

| scenario | pairs | moves/s | move p50/p99 ms | idle RSS at start | VmRSS p50 | VmRSS p99 | VmHWM |
|---|---|---|---|---|---|---|---|
| idle (20 s, no clients) | 0 | 0 | - | 14.6 | 14.7 | 14.7 | 14.7 |
| hum2 | 192 (384 clients) | 97.4 | 0.29/0.37 | 14.6 | 22.8 | 22.9 | 22.9 |
| sat2 r1 | 8 (16 clients) | 23992 | 0.18/0.85 | 14.6 | 41.5 | 52.2 | 52.4 |
| sat2 r2 | 8 (16 clients) | 23480 | 0.18/0.89 | 14.6 | 41.7 | 52.1 | 52.5 |
| sat2 r3 (smaps run) | 8 (16 clients) | 23706 | 0.18/0.88 | 14.5 | - | - | 52.4 |

The nano has 412 MiB of RAM; at peak sat2 the box still showed about 225 MiB available, and the server never swapped (VmSwap 0).
Throughput matches the pass: hum2 98 moves/s there, 97.4 here; sat2 27.3k there, 23.5k-24.0k here (different load box instance, no CPU pinning set, glibc build).

## Where the RSS goes (KiB, from /proc/<pid>/smaps, idle vs mid-sat2)

| mapping | idle Rss | Shared_Clean | Private_Clean | Private_Dirty | sat2 Rss | Shared_Clean | Private_Clean | Private_Dirty |
|---|---|---|---|---|---|---|---|---|
| anonymous mmaps (bss, thread stacks/TLS, malloc arenas, buffers) | 8720 | 0 | 0 | 8720 | 36244 | 0 | 0 | 36244 |
| libcrypto | 1528 | 176 | 896 | 456 | 3724 | 840 | 2428 | 456 |
| libc | 1308 | 1284 | 0 | 24 | 1380 | 1356 | 0 | 24 |
| [heap] (brk) | 612 | 0 | 0 | 612 | 1284 | 0 | 0 | 1284 |
| libsqlite3 | 1180 | 0 | 1148 | 32 | 1180 | 0 | 1148 | 32 |
| journal segment (mmapped, MAP_SHARED file) | 0 | 0 | 0 | 0 | 896 | 0 | 0 | 896 |
| cbase text | 704 | 0 | 704 | 0 | 768 | 0 | 768 | 0 |
| libssl | 256 | 48 | 152 | 56 | 256 | 48 | 152 | 56 |
| ld-linux | 192 | 176 | 0 | 16 | 192 | 176 | 0 | 16 |
| cbase rodata | 144 | 0 | 140 | 4 | 144 | 0 | 140 | 4 |
| libm | 116 | 32 | 76 | 8 | 116 | 32 | 76 | 8 |
| cbase.sqlite-shm | 32 | 0 | 0 | 32 | 96 | 0 | 0 | 96 |
| libz | 72 | 68 | 0 | 4 | 72 | 68 | 0 | 4 |
| [stack] (main thread) | 24 | 0 | 0 | 24 | 36 | 0 | 0 | 36 |
| cbase .data | 4 | 0 | 0 | 4 | 4 | 0 | 0 | 4 |
| [vdso] | 4 | 4 | 0 | 0 | 4 | 4 | 0 | 0 |
| TOTAL | 14896 | 1788 | 3116 | 9992 | 46396 | 2524 | 4712 | 39160 |

Shared_Dirty and Swap were 0 everywhere; Threads 5 in both captures.
"Private_Clean" for libsqlite3, libcrypto and the cbase text only means no other process had those pages mapped at that moment; they are file-backed and reclaimable, not allocations.
The process's own allocations are the Private_Dirty column: about 9.8 MiB idle and 38.2 MiB mid-sat2.
Idle anon detail: four 10 MiB anon mappings with about 1.7 MiB resident each plus a 2.2 MiB one fully resident (likely thread stacks with per-thread storage such as the commit batch and kernel section, and bss; not verified against symbols).
What grows under sat2: a new 26 MiB anon mapping with 22.4 MiB resident (most likely a glibc malloc arena of a worker thread, not verified), a new 3.4 MiB anon mapping, the brk heap (+0.7 MiB), libcrypto pages touched (+2.2 MiB, file-backed), and the journal segment (0.9 MiB dirty of 8.1 MiB mapped).
The 26 MiB mapping is the one to look at first if the peak matters: it is roughly the gap between idle and the sat2 p50.

## Caveats

One box, one run each for idle and hum2, three for sat2; the sat2 numbers agree within 0.1 MiB.
The build is dynamic glibc, not the pass's static musl binary, so the allocator and the lib split differ; a musl build would fold the libs into the binary and use musl's malloc.
The idle smaps capture is from the third server start (the smaps run), not the first idle run; their VmRSS agree (14.5 vs 14.7 MiB).
The mid-sat2 capture was taken about 28 s into a 25 s play window plus signup, so it is near the end of play, below the transient VmHWM peak (45.3 vs 52.4 MiB).
VmHWM peaks are about 11 MiB above the sat2 p50 and are hit briefly; the 0.2 s sampler caught them (sampled max 52.4 equals VmHWM).
hum8 and sat8 were not measured.

## Resources (all terminated or deleted, verified)

Instances i-02b67616e0b417338 (t4g.nano server) and i-0a84466a0771a8391 (c7g.2xlarge load): terminated.
Bucket cbase-mem-test-39bff08d, security group sg-01c429bb59c7d3118, role and instance profile cbase-mem-test-role-39bff08d: deleted.
No instance or volume tagged purpose=cbase-mem-test remains; production i-08f26ffb900294ee8 untouched and running.

# VM fixes: perf sniper handoff (written 2026-10-19)

This is the zero-re-exploration state of the `cbase serve` speed and memory work on branch `vm-fixes`.
It sits next to VM_FIXES.md, which holds the other items of the batch.

## State
The branch is pushed to origin and the tree is clean.
No PR yet.
Commits of this pass, oldest first (the first three were already there):
- 1beae4e5b sqlite store: group commit (savepoint per request, prepared BEGIN/COMMIT).
- 912ce44b7 loop: one wake byte per drained batch of posts.
- 597955059 server: an idle connection owns no buffers.
- 825b398be config: the request-worker default follows the usable CPU (cgroup cpu.max, else online CPUs, rounded up, cap 4).
- eab005d24 jwt: a per-thread cache of verified tokens (mutation-checked in unit_jwt).
- e71771da8 + e6564b66c loop: the wake channel is an eventfd on Linux (the second commit fixes the unused-function error of the first on Linux).
- 68697b57f server: CBASE_MAX_CONNS default 16384, startup lifts RLIMIT_NOFILE to cap+64.
- the commit carrying this file: the rig under c_base/bench/nano/.

## Numbers (nano: 0.1 CPU while playing, 512 MB, human pace 2-4 s per move, 4 CPU-s of signup at 1.6 CPU)
All "µs/move" are server cgroup CPU divided by client-counted moves, so they include the idle floor.

| build | 64 games | 192 games | RSS 64/192 | saturation (1 CPU) |
|---|---|---|---|---|
| S0 = HEAD at start (597955059), 4 workers | 640 µs, p95 1.2 ms | 370 µs, p95 0.6 ms | 8.1 / 10.6 MB | 5.8k moves/s, 172 µs |
| S3 = final, 1 worker | 460 µs, p95 1.3 ms | 320 µs, p95 0.7 ms | 7.0 / 9.6 MB | 13.1k moves/s, 76 µs |

Older history for context: baseline 1550/1390 µs and 14.5/28 MB; after the batched hub 520/340 µs.
S3 at 512 games: 0.1 CPU gives 270 µs/move, p95 0.5 ms, RSS 12.2 MB; 1.0 CPU gives 260 µs, p95 0.5 ms, RSS 11.0 MB.
Memory per game from the S3 deltas is about 11.5 KB (64 to 512) and 19 KB (64 to 192, with fewer settled caches).
8-seat tables with 3 spectators each: 16 tables 1000 µs/move RSS 7.2 MB; 64 tables (704 connections) 600 µs/move, p95 1.4 ms, RSS 11.0 MB.
Octogen ("%Moscow N", CBASE_BOT_SPEEDUP=100), 2 seats, one human per table: about 6.3 ms of server CPU per bot move against 0.3 ms per human move.
At 0.1 CPU the box is CPU-bound from about 25 concurrent octogen games: past that the games run slower than human pace (bot replies queue) while the human ack p95 stays 65-70 ms (the CFS 100 ms throttle period) up to 192 games.
At 1.0 CPU, 96 such games use 0.2 CPU and the human ack p95 is 0.7 ms.
One 8-seat table with 7 octogen bots: 12.9 ms of CPU per human move, which is about 1.7 ms per bot move if each human move is followed by 7 bot moves (not counted by the rig, so unverified).

## What the profile said (perf, 1 CPU saturation, S0)
The top self-time symbol was wake_up_q at 10.8%.
Its callers were NOT the store lock: 5.2% came from `__unlock` of musl's malloc lock (4.55% from malloc itself), because four worker threads share one global malloc lock and a preempted lock holder makes the others futex-wait.
So group commit does not remove the futex wakes; fewer threads does: `CBASE_REQ_WORKERS=1` at a 1 CPU quota moved 12.4k moves/s against 5.8k with 4.
The default now follows the cgroup quota.
After that the profile is flat: loopback `sock_def_readable` 6.4% (a rig artifact: client and server share a kernel), el0_svc 4.8%, malloc 3.1%, memcpy 3.1%, the wake write 3.0%, memset 2.6% (cb_section_free zeroing Game, Table and the arena prefix, table_ops.c:205), strlen 1.8%, printf_core 1.7%, cb_jwt_verify 1.3%.
JWT verify before the cache cost about 7% (b64_decode 1.8, skip_string 2.4, json unescape and memcpy about 1.5, HMAC).

## Not done, and why
- (b) snprintf of keys: printf_core is 1.7% total and splits across cb_fanout_publish, cb_section_commit, cb_buf_vprintf (cb_res_serialize, CORS); no single site is worth a commit by itself.
- (c) strlen in cb_item_add / cb_item_size / cb_av_size: 1.8% total, 0.5% in cb_item_add; not done.
- (d) writev per publish: not done. The hub does one calloc + malloc + loop post per delivery (rt_local.c local_deliver); a batch per move would remove more than a writev would.
- (a) a dedicated writer thread: not needed once workers default to the CPU count; with 2+ CPUs the malloc-lock contention returns, so a per-thread arena or fewer allocations per move is the real fix.
- (e) memory: the anon RSS at 192 games is 5.3 MB for 384 connections; the largest anon maps are 0.9 MB, 0.8 MB, 0.5 MB, 0.4 MB. What fills them was NOT broken down (could be the sqlite page cache, the hub, or the conn arrays). cb_section_free's memset suggests a big kernel struct per section; sizeof(cb_section) was not measured.
- Unexplained: 78 and 221 `400 /v1/meta` + "ask refused" in the 8-seat runs (spectators, probably the client asking before the table starts; not looked at). `lost=134` in bot mode is the client not seeing an opponent push, a rig artifact.
- The Linux eventfd path is compiled and unit_loop passes on Linux (alpine), but there is no mutation check for it.
- MAX_CONNS: a connection is calloc'ed on demand, the fd table grows on demand, so the cap reserves nothing; the real limits are RLIMIT_NOFILE (now lifted at startup, warns when the hard limit is lower) and memory at about 10 to 14 KB of anon per connection (192 games = 384 connections = 5.3 MB anon), so 16384 connections is about 160-230 MB: on a 512 MB nano the cap is not the bound, memory is, at roughly 10-15k connections.

## The rig (c_base/bench/nano/)
Every script derives its paths from its own directory (NANO_RIG overrides) and the repo from git (NANO_REPO).
Create `secrets.env` there (git-ignored, never commit it) with CBASE_JWT_SECRET, CBASE_ADMIN_TOKEN, CBASE_ORIGIN_SECRET (32+ chars each), and `client.env` with `P3ORIGIN=<the origin secret>` (see client.env.example).
Two local images are needed: `nano-perf:al2023` (amazonlinux:2023 + `dnf install e2fsprogs util-linux procps-ng bash`) and `nano-perf:perf` (that + `dnf install perf`).
Scripts:
- `build.sh LABEL` (UT=1 also runs unit_loop on Linux) writes bin/serve-LABEL and .debug; `build_client_linux2.sh` builds bin/p3load2-linux from p3load3.c (the human-pace load client).
- `run.sh LABEL` 64 then 192 games at 0.1 CPU; `runbig.sh LABEL PLAYCPUS GAMES` e.g. `runbig.sh S3 0.1 512`.
- `sat.sh LABEL` saturation without perf; `prof2.sh LABEL 8` records perf (TMIN=0 TMAX=1 for saturation); `rep3.sh` (self time + source lines), `rep4.sh/rep5.sh/rep6.sh` (callers; set L, S, N) run INSIDE `docker run --privileged -v $PWD:/nano nano-perf:perf bash /nano/rep3.sh`.
- `mem.sh LABEL 192` + `python3 smaps.py runs/smaps_192.txt` for the smaps breakdown.
- `big8.sh LABEL TABLES` 8 seats + 3 spectators; `botg.sh LABEL GAMES SEATS` (CPU=1.0 DUR=... env) octogen games.
- Pass server env with `XENV="-e CBASE_REQ_WORKERS=1"`. srv_up.sh starts the server at 1.6 CPU for signup, then step.sh `docker update`s it to PLAY_CPUS, so the CPU-derived worker default sees 1.6 (2 workers) in the rig: pass CBASE_REQ_WORKERS=1 to model a real 0.1 CPU box.
- Always one run at a time; clean up with `docker rm -f nano-perf-srv nano-perf-load nano-perf-sampler nano-perf-prof`.
- mac_prof.sh (native `sample`) did not work: the native clients hit "deadline in game 0" after 7 moves; use the container rig.

## If you are starting this cold
```
cd <repo>/.claude/worktrees/vm-fixes && git pull
cd c_base/bench/nano   # create secrets.env and client.env first
./build_client_linux2.sh && ./build.sh T0
XENV="-e CBASE_REQ_WORKERS=1" ./run.sh T0      # compare with the table above
XENV="-e CBASE_REQ_WORKERS=1" ./sat.sh T0 8 20000
cd ../.. && make -s -j2 build/test/unit_loop && build/test/unit_loop
```

## The rest of the vm-fixes batch (not done here)
- The backup job (VACUUM INTO + S3 upload when changed, restore on startup if the DB is missing).
- The systemd unit (nofile ulimit, restart) + the no-dnf boot script (curl the static release, verify sha256) + the VM runbook (CA bundle at /etc/pki/tls/certs/ca-bundle.crt).
- LTO in shipped builds + a build_bootstrap.sh CPU cap.
- The shared/c/wasm/libm.c quad() NaN/inf guard.
- c_base/DESIGN.md lines 394 and 426 are stale.
- The batch test pass: the group-commit kill-mid-group durability test, the smoke tests that need CBASE_DEV=1, a test for the CPU-derived worker default and the eventfd path; then one PR, CI, rebase-merge.
- Rerun the nano rehearsal for before/after, and decide on a lower-contention allocator or per-move allocation cuts before anyone runs serve on 2+ CPUs.


## Pass 2 (2026-10-19, second agent)
Commits, in order: 0b243c929-ish "bots: a pause holds no thread under serve" (job_start_after on the provider, a loop timer; CAS backoff does not sleep under serve; loop cancel O(log n) via a slot in the timer id), "server: a pooled request is one block from a loop-thread free list", "http: the method is parsed once into an int", "make: CPU=<name>", and "bench: botg.sh takes SPEEDUP".
Use `git log --oneline` for the SHAs.
Measured:
- Real pacing (CBASE_BOT_SPEEDUP=1, 20 octogen games, 2 seats, 0.1 CPU, 38 s window): HEAD 76ced0515 29 human moves = 0.76 moves/s (each followed by one bot move); after fix 1, 118 = 3.11 moves/s.
  p95 move ack went 1.3 ms to 61 ms only because the box is now throttled (305 of 346 CFS periods) doing 4x the work.
- Fixes 2 and 3 are unit-tested (unit_http, unit_net, unit_server_edges, unit_app pass) but NOT benchmarked.
Unfinished:
1. Human-pace run.sh (step.sh) plays 0 moves on B1 and B2 ("deadline in game 0 at v2"), 2-seat tables never deal.
   run.sh uses step.sh, which does not use the fixed p3load2-linux client; B0 at 192 was fine, so a server regression from a1edd8a44 is possible.
   The coordinator took this one.
2. Bench fixes 2 and 3 (B2 vs B1 at 64/192 games) once item 1 is solved.
3. Fix 4: cb_fanout_publish already batches (cb_rt_batch_begin/end) and builds one spectator cast; the per-viewer cast build and per-frame encode are unprofiled. Run big8.sh then prof, then decide on encode-once and writev.
4. Fixes 5 and 7 not started. 6: build with MK="CPU=neoverse-n1" ./build.sh N1 and compare with B2 (saturation and 192 games); the Makefile variable is in place, shipped flags not changed.
5. Test pitfall: macOS sed needs -i '' and make's same-second rebuild trap bit twice while mutation checking (sleep 1 and touch after restoring).

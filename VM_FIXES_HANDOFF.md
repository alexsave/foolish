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
Update after the client fix (c36b533e2), all on the emulator at 0.1 CPU, CBASE_REQ_WORKERS=1:

| build | 64 games | 192 games | saturation (1 CPU, 2 seats) |
|---|---|---|---|
| B0 = 76ced0515 (before everything) | 460 us/move, p95 1.2 ms, 7.1 MB | 310 us, p95 0.7 ms, 9.7 MB | 13.4k moves/s |
| B2 = + no-sleep, free list, method enum | 470 us, p95 1.2 ms, 7.2 MB | 320 us, p95 0.7 ms, 10.2 MB | 13.1k-13.6k |
| N1 = B2 with CPU=neoverse-n1 | 570 us (noise), p95 1.2 ms | 320 us | 13.0k (3% worse) |
| B3 = B2 + sorted router | not run | not run | 12.86k vs B2 13.05-13.1k, repeated twice |

- Fixes 2 and 3 (free list, method enum) move nothing measurable at this resolution; they are kept as removals of per-request mallocs and strcmps.
- -mcpu=neoverse-n1 does not win here: not shipped. `make CPU=...` stays as an opt-in variable.
- The sorted router reads about 1.7% slower than B2 at saturation in two A/B pairs, where the router is not in the profile at all, so this is probably code layout and not the algorithm; kept because it was asked for and it removes a strlen per route per request. Worth one more A/B with a different build order before anyone concludes anything.
- Bot games at real pacing (SPEEDUP=1, 20 games, 2 seats): 0.76 -> 3.11 human moves/s (each followed by one bot move) with fix 1.
- 8 seats + 3 spectators, human pace, 0.1 CPU: 16 tables 26 moves/s, 0.86 ms CPU/move, p95 2.5 ms; 64 tables 102 moves/s, p95 1.5 ms (p99 54 ms), 16 MB RSS; 128 tables 190 moves/s, p95 142 ms, p99 206 ms, the box at 0.102 CPU (CPU bound).
  So the max before p95 > 250 ms is roughly 130-150 concurrent 8-seat tables (about 0.54 ms of CPU per move including the idle floor).
  At 1 CPU saturation one move costs 0.18 ms (24 tables, 4.7k moves/s, p95 55 ms).
- Fix 7: with the fixed client there are NO /v1/meta refusals (every "ask done" line has why 0, code 0, status 0, 128 of them in 16 tables).
  The 10 "why 0 st 1" are PLAY actions the kernel answered REJECTED (TABLE_STATUS_REJECTED = 1), not asks: p3load3.c on_done NC_DO_PLAY counts action_status != APPLIED. Not examined further (a client move chosen against a stale view is the likely cause; each X line in runs/*/events has game, version).
- Fix 4 (8-player fan-out): the profile does not point at it. cb_fanout_publish already batches (cb_rt_batch_begin/end) and builds the spectator cast once; per-viewer cost is flat. Top self time at 1 CPU, 24 8-seat tables: loopback sock_def_readable 14% (rig artifact: client and server share a kernel), el0_svc 4.3%, memcpy 3.1%, wake_up_q 2.7%, malloc 2.2%, strcmp 1.5%, memset 1.4%, printf_core 1.3%, cb_rt_topic 1.1%. Nothing user-space above 3%, so no fan-out change was made.
- Fix 5 (snprintf/strlen): printf_core 1.3-1.5% and strlen 1.0-1.6% split over many sites; not done.

Final profile of the FINAL build (B4 = HEAD with the token change), saturation, 1 CPU, 8 two-seat games, 12.56k moves/s:
top self %: sock_def_readable 5.9 (kernel: waking the client's blocked reader on a loopback write, offset +72 is the wake itself), el0_svc 5.3 (syscall entry, offset +32), wake_up_q 4.1 (kernel: the wake of a pool or loop thread, +104), vfs_write 3.4, memcpy 2.9, malloc 2.7, memset 2.5 (memset.S:95 is cb_section_free zeroing the pooled section), tlb_flush 1.6 and __arm64_sys_mmap 1.2 (malloc growing by mmap from alloc_slot in a request thread), strlen 1.6, printf_core 1.5.
The three hottest are all kernel wake and syscall paths, i.e. per-move syscalls (one write per delivery plus the post wake), not instructions of ours; perf annotate gave no per-line split for kernel symbols.

Done in the later part of this pass (commits after a8617b79e): router sorted at add (net/router.c, first fit by score, memcmp), access token as a fixed binary layout (auth/jwt.c is the one owner; cb_js_jwt_* and the issuer argument are gone; no client decodes the token, checked in foolish/web, ios, netclient.c, shared; old tokens stop verifying), step.sh/step3.sh bounded waits, prof8.sh.
smoke_authz fails one check on this branch: "GET /health as edgeless: 200, want 403" (the loopback probe exemption in handler.c is_local_probe; not touched by the token change, not investigated).
smoke_matrix passes (its alg-none and swapped forgeries were re-cut as truncated and spliced tokens).

Unfinished:
1. In-memory bot leases on the VM (not started beyond reading).
   The lease is not a separate row: it lives on the SCAN marker (rows_foolish.c ~:967) and acquire/renew also move the marker's updated_at, which is the heartbeat scan's staleness clock (games_needing_bots), and unit_store tests that clock on SQLite.
   A faithful in-memory version needs: a mutex-guarded map keyed by game id (inside the store, so tests with many stores do not alias); acquire = marker exists (a read) and no live entry; renew = token match; release = token match; lease_held = live entry; bots_idle and game delete drop the entry; games_needing_bots must skip games with a live entry (replacing the updated_at bump); and the local provider must switch it on (a flag set from provider_local), with unit_store keeping the item path for the DynamoDB and test stores.
   Then bench botg.sh SPEEDUP=1 before and after.
   The table_ops stripe lock covering same-game CAS on serve was not looked at.
2. Make sat.sh / run.sh print the p95 of the human ack so a 1.7% throughput difference can be judged against run-to-run noise (two reruns of the same build differ by about 0.5%).
3. Unexplained 400s and "ask refused" from the earlier handoff are gone with the fixed client; the 10 REJECTED plays per 16 tables are worth a look in the client's move choice.
4. smoke_authz /health edgeless check (above).


## Pass 3 (profile-driven, 2026-10-19)
Saturation (1 CPU, 8 two-seat games, emulator, CBASE_REQ_WORKERS=1), per commit:

| step | moves/s | per move: sendto / futex / mmap+munmap |
|---|---|---|
| B4 (before this pass) | 12.6k | 4.7 / 7.6 / 2.2+2.2 |
| + sqlite store buffers pooled (item_apply scratch per thread, SQLite SQLITE_CONFIG_MALLOC size-class free lists) | 15.0k | 4.2 / 3.7 / 0.4+0.4 |
| + cb_ws_send queues, the loop flushes each marked connection once per iteration (cb_loop_after) | 15.35k | 2.7 / 3.0 / 0.4 |
| + cb_item_size from stored lengths | 15.4k | - |

- The mmap churn was musl mallocng unmapping an 8 KB group each time its last slot is freed; the sqlite row buffers were exactly such groups (perf with dwarf unwinding, runs/mm_8.txt: cb_item_add realloc, SQLite VdbeMemGrow).
  Futex per move halved with it (malloc lock traffic).
  Cost: RSS about +0.7 MB at 64 games, +2 MB at 192 (the pools keep 8 blocks per class up to 64 KB; POOL_KEEP in store_sqlite.c).
  Human pace, 0.1 CPU: 64 games 400 us/move (was 460-470), 192 games p95 0.8 ms.
- Run-to-run noise at saturation is about 0.5%, but one B8 run read 11.0k with another agent on the rig; compare pairs only.
- Not done, with reasons:
  * Wake-ups (item 2): the remaining 3 futex per move are the request pool handoff and the loop post; running h_action on the loop thread would block the loop for a sqlite commit (about 40 us, an fsync at checkpoint), a design decision for Alex, not done.
    The group commit already runs on the calling thread (leader/follower in store_sqlite.c submit), it adds none.
  * memset in cb_section_free (item 4, 2-2.7%): 36 of the Game's 37 KB is logs[]. Narrowing the zero to num_logs fails: num_logs is reset to 0 (table_create, the suite hit it 163+ times) while logs hold entries.
    It needs a log high-water field in Game (kernel + structgen).
    unit_app now checks the pooled section is all zero after every request (cb_section_pooled_is_clean, mutation-checked), so any narrowing has a regression guard.
  * printf_core 1.7% (snprintf 1.0 over many sites, cb_buf_vprintf 0.6) and strlen 1.7% (cb_item_add 0.5, cb_av_size 0.5): cb_item_size now uses stored lengths; cb_item_add still strlen's the value (its callers pass C strings); the snprintf sites were not converted.
  * In-memory bot leases: still the design in the Pass 2 list item 1; not started.
Rig additions: prof_sc.sh (syscalls per move via perf stat tracepoints), prof_mm.sh (callers of mmap), prof8.sh (8 seats).


## Pass 4 (2026-10-19)
- Rig image: c_base/bench/nano/Dockerfile.al2023 + build_images.sh (fresh amazonlinux:2023, dnf upgrade --releasever=latest); `dnf check-update --security` is empty.
- Build base: Alpine 3.24 by digest (OpenSSL 3.5.9 LTS, SQLite 3.53.4), `apk upgrade` + a pending-upgrade check in build_bootstrap.sh and build.sh, eol_guard.sh in the CI bootstrap job. zlib is not linked into the binary. EOL dates (alpinelinux.org/releases, openssl-library.org release strategy, 2026-10-19): Alpine 3.20 2026-04-01 (past), 3.21 2026-11-01, 3.22 2027-05-01, 3.23 2027-11-01, 3.24 2028-06-01; OpenSSL 3.4 2026-10-22, 3.5 LTS 2030-04-08, 3.6 2026-11-01, 4.0 2027-05-14; 3.0 and 3.3 are not supported (3.0/3.3 dates in the guard's table are not on that page).
- Inline dispatch: CB_ROUTE_INLINE on GET /v1/games/* and /v1/action; inline_ok in net/server.c; unit_inline. At saturation it almost never fires (the pool is never idle with 16 clients); at human pace, 192 games, futex per move 2.4 -> 1.44, mmap per move rose 0.6 -> 1.5 (cause not investigated: cross-thread frees on the loop thread are the suspect), CPU per move 0.28 -> 0.27 ms. 64 games is inside the noise. ready_left has no test of its own.
- Game.logs is no longer zeroed (poison 0xAA under ASan and CBASE_POISON_LOGS, which the Makefile exports); nothing tripped. memset left the top of the saturation profile; moves/s is inside noise (11.8-13.1k vs 12.06k).
- OPEN: unit_oom still fails (12 checks): the sweeps count SQLite's per-store allocations (kept prepared statements, page cache), which now go through the pool allocator the test counts. Boot and the 500 answers are fixed by cb_section_drain, cb_rows_batch_drain, cb_store_local_drain. A warm-up run did not fix it.
- Not done: item 5 (snprintf of keys).


## Pass 5 (2026-10-19)
- unit_oom is green (10 consecutive runs) and `make -k test` shows no failure. Cause: the sqlite pool allocator made SQLite's own blocks visible to the failing-allocator seam (kept statements, schema affinities, page-cache churn); they are now tallied by the store (cb_store_local_mark/blocks) and checked on their own. It also exposed a real bug: ROLLBACK prepared lazily, so an OOM in that prepare left the transaction open (begin() now prepares the transaction-ending statements). unit_unfurl had not compiled since the method enum change; fixed.
- Backup job built (providers/local/src/store/backup.[ch], util/blob.h, providers/aws/src/aws/s3_blob.[ch], unit_backup, unit_imds, runbook, vm.yaml SSM params). NOT run against real S3 or IMDS: first deploy must watch for `backed up to` in the journal, and check the stack's role can PutObject (it can on paper) and that GET of a missing key answers 404 (ListBucket is granted).
- NOT DONE: item 2 (mmap 0.6 -> 1.5 per move after inline dispatch) was not measured or fixed; the suspect is still the loop thread allocating per-request buffers without the per-thread pools; the inline "last ready event" gate (cb_loop_ready_left) still has no test of its own.


## Pass 6 (2026-10-19)
- Item 1, mmap per move at human pace (192 games, perf stat 20 s): 1.07 -> 0.02.
  Two sources, found with perf dwarf callers (bench/nano/prof_mm_h.sh): the SQLite row buffer of 1.5-1.8 KB, below the pool's old 2 KB floor (now 1 KB, classes 2K..64K), and the calloc of the local realtime delivery record in local_deliver (now a 64-entry free list holding the packet buffer too).
  The cause was not the loop thread allocating from per-thread pools; the pools are global, and the churn was musl unmapping a group whenever its last slot was freed.
  CPU per move 270-280 us before, 230-250 us after (noise about 20 us).
- Item 2: unit_inline last_ready, mutation-checked (dropping the ready_left term fails it).
- Item 3: (a) and (b) were already done (single group skips the savepoint; BEGIN/COMMIT are prepared via ctl).
  PINGREQ is now answered on the loop (unit_rt_local holds every pool worker and still gets the PINGRESP, mutation-checked); the rig does not ping, so no us number.
  (c) commit-batching window: a 1 ms leader wait was tried (experiment, not committed); with a human-pace arrival rate near 100 per second only about 10% of lone writes would find company, and it would stall the loop thread under inline dispatch; the experimental binary also produced no result under load. Rejected.
- Item 4: LTO=1 on the kernel and c_base objects: 10.93k / 11.09k moves/s against 11.02k / 11.48k without (8 games, 1 CPU), binary 4% smaller. Not adopted. build_bootstrap.sh caps the container (BUILD_CPUS, default 2).
- Item 5: libm quad() guard, test with NaN, +/-inf; the web wasm still links with the Homebrew clang.
- Item 6: DESIGN.md "The VM path" section. The two stale lines named in VM_FIXES.md were already fixed.
- Item 7: unit_groupcommit_kill (fork, SIGKILL, 6 rounds), mutation-checked.
- Item 8: eol_guard OpenSSL 3.0 and 3.3 rows removed (the page says 3.0 is unsupported, lists no 3.3).
- Item 9: the smokes did not set CBASE_DEV=1 and serve refused to start; fixed, all six smokes green (smoke_authz /health too).
- Also fixed: unit_name_gate (store_priv.h comment from the creds commit).
- NOT DONE: item 10, in-memory bot leases (design in Pass 2 item 1 above).


## Sniper 5 paused (2026-10-19, stopped for "proper SQL first")
Everything below is committed and pushed on vm-fixes; the tree is clean, no WIP.
Commits, oldest first: 5156ecd64 (item 1: LOG# items), the one-lock commit (item 2), the in-memory-lease commit (item 3), 8bc0df4e0 (item 5: tally deleted).
`make -k test` and `make smoke` were green at 8bc0df4e0.

### Item 1 (partial): the session log left the game row
A commit PUTs its log records as one item pk `LOG#<game>`, sk = the new version zero-padded to 20 digits; the ROW keeps only `log_from` (version where the session log starts).
`game_logs` (rows_foolish.c cb_rows_game_logs) reads it for the bot drive and finalize; `game_get` never carries logs.
cb_game_commit has log_add/log_add_len/log_clear; a deal or lobby clears (old items pruned after the commit); game_delete prunes all.
NOT done: native relational tables for any family, export.c (unchanged: it maps Dynamo families to CSV and the import writes one LOG item), items table unchanged (WITHOUT ROWID on items broke admin browse, which pages by rowid, so it was reverted).
BLOCKER on the item-table schema: the product's rows are written through the generic item layer (items.h) shared with the aws provider and the in-memory test store, so a native schema means replacing cb_store_ops item_* and rewriting rows_auth.c and rows_foolish.c together; the admin browse (rowid cursor) and unit_dynamo_ops (exact request sequences) are the other dependents.
MEASURED REGRESSION to fix first in the SQL rewrite: saturation (sat.sh, 8 two-seat games, 1 CPU, CBASE_REQ_WORKERS=1) went from 16.8k moves/s (I0, HEAD before this pass) to 11.7k (I1 = item 1 alone; I2, I3 the same within noise).
Human pace is unchanged (370/280 us at 64/192 games against 380/260 the same day).
Cause: every move is now an item_txn of two writes (ROW update + LOG put) instead of the one-write fast path, and touches two pages (walChecksumBytes 2.7% against 1.1%, vfs_write, VdbeExec, memcmp up).
A native game_moves INSERT in the same transaction as the games UPDATE should cost far less than a second generic item write; or keep the last few records inline in the row.
Long-game benefit (no O(log) copy, no 400 KB cap) is real but was not measured (no WAL bytes per move number was taken; the rig only reports db size).

### Item 2 (done)
cb_table_run takes cb_table_order_lock (64 stripes) at the start and holds it through the commit and pushes; cb_section_commit and the TABLE_EMPTY path no longer lock; contention_pause and CB_TABLE_BACKOFF_MS are gone; a lost fence is an error (500), not a retry; the attempt loop remains only for the cache recheck (2 attempts).
The bot cycle (bots.c run_cycle) holds the same stripe lock through its compute, so a human move at a table waits for a bot drive (up to ~6 ms for octogen), and unrelated games on the same stripe too; stripes were not resized.
Left: the row cache is still not authoritative (the cache-drop branches remain); the bot loop's CAS retry and its tests (steal_commits) remain.
Tests: a_lost_fence_is_an_error_not_a_retry, a_lost_delete_is_an_error (mutation-checked). No threaded test of the lock.

### Item 3 (partial)
Under serve (ctx->job_start_after set) the bot lease is an in-memory list behind a mutex (bots.c mem_lease_*), 2x wall-ceiling guard against a hung drive; kick_wanted reads it; a failed kick is retried once from a timer (not on NOMEM).
The 10 s scan now runs once after start (cb_schedule.serve_once) as the recovery pass.
Test: unit_bots_kick serve_leases_live_in_memory (mutation-checked).
NOT done: the store lease_acquire/renew/release/lease_held, bots_idle, the SCAN marker upkeep (scan_write, needs_bots on every commit) and games_needing_bots on the SCAN partition are still there and used by the aws provider and ~40 tests; a dual path (in_memory()) exists on purpose until the store ops go. Planned replacement: a gsi2 "BOTSDUE" entry on the ROW instead of the SCAN item, queried only at startup.

### Item 4 (not started)
Per Alex: a plain binary WebSocket with netwire frames both ways, no MQTT on the VM path, a user -> connections map plus game -> viewers, ws ping/pong, HTTP fallback stays, iOS FoolishNet out of scope (list what it needs: it speaks the same MQTT frames through netclient.c).
The web host does not speak MQTT itself: see scratchpad/vm_simplify_codemap.md (backend.ts:246-312 moves opaque binary frames; MQTT encoding is in foolish/c/src/netclient.c and shared/c/mqtt), so the web needs at most a subprotocol-string change.
Server pieces to replace: c_base/core/src/realtime/stream_mqtt.c (cb_mqtt_authorize, on_packet), providers/local/src/realtime/rt_local.c (lconn ix[], topic index, job/pump).
Security requirement (Alex): sign-out, session revocation and account deletion must close that user's sockets (JWT verify never reads the SESSION row, so a revoked access token stays valid until expiry); check the session is live at socket connect; mutation-checked test: sign out, the socket closes, further frames are refused.

### Item 5 (partial)
Deleted: cb_store_tally and its producers (store_dynamo.c), handler.c and bots.c debug lines, bench_dynamo (Makefile target too); name gate allow.txt lowered.
NOT done: budget_left/deadline/rekick in bots.c (cold: INT64_MAX under serve; they share ctx->deadline_ms with main_lambda.c) and the after-phase (after.c, cb_after_fanout, cb_after_kick; never opened under serve).

### Item 6 (measured only)
RSS at human pace, 0.1 CPU, 1 worker: 64 games 9.5 MB, 192 games 13.2 MB, 384 games 14.8 MB; the steady slope is about 4 KB per connection (384 connections for 192 games), the 64 -> 192 step (14 KB per connection) is heap high water from signup.
The structs are about 1.2 KB per connection (cb_ws ~390, lconn ~360, cb_mqtt_conn ~350), so shrinking them (ix[32] and subs[32] are 512 B) would save under 10%; SO_SNDBUF/SO_RCVBUF are kernel memory, not RSS, and setting them disables autotuning. Nothing changed.

### Final numbers (I3 = all of the above)
Human pace 0.1 CPU: 64 games 390 us/move p95 0.9 ms RSS 9.5 MB; 192 games 250 us p95 0.6 ms RSS 13.2 MB; 384 games 220 us p95 0.5 ms RSS 14.8 MB.
Saturation: 11.7k moves/s against 16.8k for the pass's start (see item 1).
Saturation top self time on I3: sock_def_readable 7.0, vfs_write 4.7, el0_svc 4.1, __arch_copy_from_user 3.5, memcpy 2.8, walChecksumBytes 2.7, malloc 2.1, memcmp 2.1, printf_core 1.9, strlen 1.8 (I0: sock_def_readable 8.9, el0_svc 4.8, memcpy 3.7, vfs_write 3.7, malloc 2.7, printf_core 1.8, strcmp 1.7).
Rig note: binaries bin/serve-I0 (start), I1, I2, I3 are in c_base/bench/nano/bin; the lock script is scratchpad/lrun.sh.


## Proper SQL (2026-10-19, after sniper 5)
Commits 846f948c4 .. b11b99ae1 on vm-fixes, pushed. `make -k test` and `make smoke` green (gates included); unit_export passes against DynamoDB Local.

### Schema (7)
- Platform (c_base/providers/local/sql/schema.sql): users (id BLOB 16, email UNIQUE, username with a UNIQUE index on the canonical name auth/names.h stores, kept_session BLOB), sessions, refresh_tokens (token_hash BLOB 32), signin_fails. WITHOUT ROWID; indexes only sessions(user_id), refresh_tokens(user_id). Schemas 1-5's tables are dropped before 7 is created; 6's `items` is dropped.
- foolish (foolish/server/sql/schema.sql, embedded as cb_foolish_schema_sql, applied in the same transaction): games (rowid table, TEXT id = the short code unchanged, fixed size: state, roster, seed, log_from, no log), game_moves(game_id, seq, action) WITHOUT ROWID, game_members (+ index user_id), game_bots, bots, ratings (+ partial index for the leaderboard), replays (rowid; players as 16-byte ids, moves, extras, fool/dur summary, partial index on game_id), replay_players(user_id, created_at, replay_id) WITHOUT ROWID, chat(game_id, created_at, id) WITHOUT ROWID (+ index user_id).
- No lease, bots_idle or SCAN in SQL: bots.c keeps the lease in memory under serve and wherever the store has no lease op; the startup scan reads games.needs_bots.
- Replays keep their own v6 code (not a pointer into game_moves): different compact format, outlives the game and the session log a rematch clears.
- Store API for products: core/src/store/store_sql.h (cb_sql_read, cb_sql_write = the group commit as a transaction of callbacks, cb_sql_stmt for the product's prepared statements, uuid bind/column helpers). Every statement is prepared at open. foolish's rows: foolish/server/src/store/rows_sql.c; positions are named (G_*, B_*, ... and P()/COL()).
- The DynamoDB item path (rows_foolish.c, rows_auth.c, store_dynamo.c, items.c) is unchanged except: logs_packed back in the row (production's layout, LOG# items removed). It is only for Lambda and cbase-export's read side.
- Ids: UUIDs are 16-byte BLOBs in storage (cb_uuid_pack/unpack in util/uuid.c, the one owner, jwt.c uses it). In memory the row structs still carry text cb_uuid (not converted; a handler-wide change). Wire and URLs unchanged (text).

### Numbers (rig, CBASE_REQ_WORKERS=1; Q1 = bc0cad859 behaviour, I0/I3 as in sniper 5)
| | 64 games | 192 games | saturation (8 games, 1 CPU) |
|---|---|---|---|
| I3 (302851836, before) | 390 us/move, p95 0.9 ms, 9.5 MB | 250 us, p95 0.6 ms, 13.2 MB | 11.7k |
| I0 (sniper 5 start), same session as Q1 | - | - | 16.76k at 0.98 cores (145/202 throttled), 59 us CPU/move |
| Q1 | 330 us, p95 1.0 ms, 8.2 MB | 210 us, p95 0.5 ms, 10.5 MB | 15.9k at 0.83 cores (0 throttled), 52 us CPU/move, p99 10 ms |
- Saturation does NOT beat 16.8k yet, but CPU per move does (-11%): the server sits idle 17% of the time. Suspected cause: SQLite's automatic checkpoint runs inside the group's COMMIT with the connection locked (its fsync stalls every queued request); a commit now dirties two pages (games + game_moves). Profile this first (perf/sample at saturation, look for fsync/walCheckpoint under the leader).
- NOTE (idea, not code): a background PASSIVE checkpointer on its own connection with the writer's autocheckpoint off was tried and FAILED (WAL grew to 1.1 GB under constant writes: a passive checkpoint never catches the moving end, so the WAL never restarts). Patch: scratchpad/checkpointer_experiment.patch. A workable variant needs a bounded fallback (writer checkpoints past N frames) or a RESTART at a quiet moment.
- WAL bytes per move: NOT measured (the rig has no WAL meter). db file growth per move at saturation: 78 B (Q1) vs 97 B (I0).
- Long-game scenario: NOT run. Structurally a move writes two small rows (the fixed games row and one game_moves row) whatever the game's length; before, the row carried the whole cumulative log.

### Export (foolish/server/migrate/export.c + foolish/server/src/store/migrate_rows.c)
- Keys from DynamoDB, sorted by family rank then key; each item decoded by its family codec into its row and written through the SQL store's operations; counts per family vs per table; a second read compared by natural key. Unknown family, duplicate or non-canonical username, refused row: loud failure. Expired sessions/tokens/sign-in counts and SCAN markers are skipped and counted. store_copy and unit_store_copy are gone.
- Tests: unit_migrate (no table, mutation-checked), unit_export (DynamoDB Local, mutation-checked). Run unit_export only against a private DynamoDB Local (not cbase-dynamodb-local).
- cbase-import counts SQL rows per family with a per-table SELECT of ids (cb_rows_sql_keys) for its "extra" report.
- The production dry run has not been re-run.

### Left
1. Saturation over 16.8k (profile the checkpoint stall first, see above); WAL meter and the long-game scenario on the rig.
2. In-memory ids as 16 bytes in the row structs (storage already is).
3. Delete the aws provider after cutover (store_dynamo, items.c, rows_auth.c, rows_foolish.c item path, the dynamo tests).
4. The admin console now lists tables; its Games panel uses "games" (works); other panels unchecked by hand.

## vm-ws (branch vm-ws, from vm-fixes 302851836): plain WebSocket replaces MQTT on the VM path
WIP checkpoint 61505bd06 plus later commits. Tree is NOT green yet (list below). No overlap with the SQL agent's files.

### Frame protocol (shared/c/stream_frame.h, header-only, both ends)
GET /ws, plain binary WebSocket, no subprotocol. One WS message = one frame `[1][kind]...`, little-endian.
Client to server: HELLO(60) u16 n + token (first frame, only once); SUB(62)/UNSUB(63) u32 id, u8 what (NW_SUB_*), u8 n, gid; REQ(64) u32 id, u8 meth(1 GET,2 POST), u16 n, path, body; PING(66).
Server to client: READY(61); REPLY(65) u32 id, u16 status, body (answer to SUB/UNSUB with empty body, and to REQ with the HTTP body); PONG(67); pushes are netwire frames NW_WS_VIEW 51, CAST 52, CHAT 53, SPECT 57 (new: spectators' cast, same layout as CAST).
SUB ids carry the top bit (client next_pid), action ids are the pump's request ids.
A malformed frame, a non-HELLO first frame, a second HELLO, a text message: close 1008/1003. Refusals have no wire reason.

### Done (file:function)
- c_base/providers/local/src/realtime/rt_local.c: whole hub rewritten. maps_add/user_add/user_del (user -> connections chained through the by-user hash), game_add/game_del (game -> spectators), pump (SUB/UNSUB/PING inline on the loop, HELLO/REQ as pool jobs, one per connection), run_req (REQ = synthetic cb_http_req through cb_router_match, handler runs as for HTTP; loop routes and /v1/auth/* refused), local_deliver (family pattern containing "{user}" = all of that user's connections, else game spectators), local_forget/end_on_loop.
- c_base/core/src/net/http.h: `auth_user` on cb_http_req; auth.c cb_auth_verify_bearer returns it when set (only rt_local sets it; the parser never does).
- c_base/core/src/auth/auth.c: cb_auth_verify_token now reads session + user (cb_auth_session_live, new in auth.h); cb_auth_logout/change_password/refresh-reuse/delete_user call cb_rt_end_sessions (realtime.c/h, new; transport `forget` got session_id/keep_session args). realtime.c live_verify just calls verify_token.
- c_base/core/src/net/server.c: server sends WS PING after ws_ping_ms (30 s) of silence; ws_idle_ms (120 s) still closes. ping_sent resets on any read.
- foolish/c/src/netclient.c: stream part rewritten (take_frame, take_suback(id,status), nc_stream_next one frame per call, action_frame, wire_lost, nc_stream_hear_all test seam). nc_req_stream path is "/ws". Actions (NC_DO_PLAY) go as REQ when the socket is LIVE (slot.wire 1 queued, 2 sent); the REPLY becomes a NcDone via g_dropped / nc_next_done (web pump() drains it after streamIn); an unanswered one at close is answered as status 0. Pushes for something not subscribed are dropped (the server sends a user all their views/seat casts/chat on every connection).
- foolish/c/src/netwire.[ch]: NW_WS_SPECT=57. foolish/server/src/app/pushes.c: rt_publish_cast uses it when no user; functions_http.c: removed the duplicate cb_rt_forget_user on delete (auth owns it).
- foolish/server/test/unit_netclient.c ported (fake server helpers next_pkt/connack/suback/feed, action_answer); 39 tests pass. packets_across_messages test deleted (no reassembly any more).

### Design decisions
- auth_user on cb_http_req: a socket request is authenticated once at HELLO; handlers see the same cb_user a bearer would give, so there is ONE handler path and no token stored per connection.
- End-generation counter (local->end_gen, atomic, bumped in local_forget on the calling thread): each REQ job compares it with the connection's checked_gen and re-reads the session (cb_auth_session_live) when it moved, so a REQ that starts after a revocation but before the loop closes the socket is refused. A HELLO job in flight when it moves is closed (job_done).
- Connection ends at the token's exp (checked per frame in pump); no min/max life constants.
- Client SF_PING/PONG kept only because a browser cannot see WS ping frames (dead-socket detection NC_PONG_MS); server liveness is WS ping/pong.
- aws provider untouched: shared/c/mqtt, stream_mqtt.*, rt_iot.c, cb_rt_topic stay (aws still uses them). Lambda's stream no longer matches the client.

### Status (rebased onto origin/vm-fixes fd9bd7ab5, no conflicts)
`make -k test` and `make smoke` (c_base) are green; every new test below was mutation-checked (change the code, see the named assertion fail, restore).
- c_base/providers/local/test/unit_rt_local.c: 7 tests (upgrade route, pushes by user and by watched game, many sockets, REQ as the user, ended sessions close sockets, refusals, parked requests). Mutations: dropping the end-generation check fails ended_sessions_close_their_sockets (REPLY arrives on a revoked session); ignoring the session filter in end_on_loop fails it; dropping the wake in server.c wake_conn fails parked_requests_hold_no_thread.
- c_base/core/test/unit_auth.c: after local logout / global logout / password change the other tokens stop verifying (mutation: session_over ignored, 5 failures).
- foolish/server/test/unit_netclient.c actions_ride_the_socket (mutations: socket branch off, wire_lost silent).
- Smokes ported: smoke.c, smoke_matrix.c (stream_frames), smoke_authz.c (ws_matrix: HELLO rows, sign-out closes the socket and the token never connects again, delete closes it, long poll rows). Behaviour change they now assert: a deleted account's or a never-begun session's token is 401 on EVERY authed route (it used to read as an empty user).
- Long poll: GET /v1/wait/<gid>[?v=N] (v1_http.c h_wait). Answers the view at once if version > N (no v: now), else the request PARKS: http.h park_ms/park_key on the response, server.c job_done/resubmit/wake_conn/cb_server_wake (a wake counter stops a lost wake-up), rt_local.c pub_on_loop wakes key = user id (a seated caller) or game id (a spectator). Time up (25 s, server sweep 1 s granularity) runs the handler once more with req->park_expired and it answers 204. No thread is held while parked. Client disconnects of a parked request are noticed only at the next wake/deadline (reads stay paused).
- Web host: streamProtocol removed (sdk/ts/net/net_client.ts, backend.ts `new WebSocket(url)`, e2e stub); Swift SDK constant and ios/FoolishNet/Stream.swift likewise (not compiled here).
- Fuzzers fz_netclient.c ported (syntax-checked only; fuzz-smoke does not build on this Mac because of aws s3_blob.c sprintf, pre-existing); fz_sequence.c only got the new `forget` signature: it still models the MQTT/IoT hub through stream_mqtt (aws), not the local hub. No fuzzer for rt_local's frame handling or sf_parse yet.

### Rig numbers (nano, 0.1 CPU, CBASE_REQ_WORKERS=1, human pace, same day, bins serve-I3 + old client vs serve-W1 + new client, actions over the socket)
| games | | server cpu ms/move | move p50 | move p95 | rss_max kB |
| 64 | before | 0.48 | 0.4 | 1.1 | 9456 |
| 64 | after  | 0.43 | 0.4 | 1.0 | 9612 |
| 192 | before | 0.29 | 0.3 | 1.1 (p99 91) | 13392 |
| 192 | after  | 0.23 | 0.3 | 0.5 (p99 1.8) | 12992 |
Marginal RSS per connection (64 to 192 games, 256 more sockets): before 15.4 kB, after 13.2 kB (includes signup heap high water; cb_user is heap-allocated per open socket, about 500 B). Syscalls per move were NOT measured (no strace in the container; sat.sh saturation run was not repeated). The rig is flaky: summ.py `NameError: name 'a'` appeared 3 times (cgroup stat missing at the end of a run); rerun the single size with /private/tmp/claude-501/ws_one.sh. Run logs: /private/tmp/claude-501/m_after64.log, rig_after.log (192), m_before192.log, rig_before.log (64). Bins: c_base/bench/nano/bin/serve-W1 and p3load2-linux (untracked, in vm-ws worktree; secrets.env and client.env copied from the vm-fixes worktree, ignored by git).

### Not done
1. Syscalls per move and a saturation (sat.sh) run: measure with `strace -c -f` or perf in the container.
2. Shrinking cb_user per socket (keep id, session id, username only).
3. SF_PING could go if the browser host stops needing client-side dead-socket detection.
4. aws provider still on MQTT (stream_mqtt.*, rt_iot.c, shared/c/mqtt, unit_mqtt, fz_mqtt*, iot authorizer): delete with the provider. foolish/c/Makefile and foolish/foolyard/Makefile still compile mqtt.c into the client (no longer used there); c_base/Makefile coverage filter still lists shared/c/mqtt (still true for aws tests).
5. Benches still speaking MQTT: c_base/bench/net.c, foolish/server/bench/mqtt.c, roundtrip.c, cpu.c (not built by make test).
6. iOS FoolishNet: Stream.swift opens `webSocketTask(with: url)` now; it needs nothing else because netclient.c does the framing, but it must be built and run on a device/simulator (wasm/xcframework rebuild of netclient.c) and the host loop must drain `nc_next_done` after each stream message (the web pump() already does).
7. Password-change socket close is covered by unit tests (rt_local end_sessions with keep, auth) but not by a smoke over HTTP; the sign-out one is.

### Landmines
- macOS sed has no `-i` without '' and no `\n` in replacements: use python for edits.
- grep "Werror" matches every compile line; filter on `error:`.
- cb_user is ~500 B, heap-allocated per open connection (RSS per connection); could be trimmed.
- verify_token now costs two store point reads per HTTP request; measure before/after.
- HELLO closes if ANY session ended during its flight (cheap and safe, rare).


## Cleanup pass (2026-10-19, after vm-ws; final state of this pass)
Every commit below is pushed on vm-fixes, and `make -k test` and `make smoke` were green at each one.
No PR (merges wait for the cutover).

### Done
- 80cbab25b checkpoints off the commit path (store_sqlite.c "the checkpointer"): a thread with its own connection runs PASSIVE passes every 1000 frames, and at 8192 frames the next group waits for a pass that starts after it stopped, so the WAL rewinds; the WAL hook is the meter (`cb_store_local_wal_frames`, logged at close).
  Cause, from `bench/nano/prof_off.sh` (perf trace): 354 fsyncs in 12 s inside commits, 4.1 ms average, 42 ms max, with the one worker holding the connection.
  Saturation 16.5k to 19.5k moves/s on a quiet host, p99 10.3 to 0.9 ms, 0.74 to 0.96 cores.
  `unit_checkpoint` runs on a VFS whose syncs take 20 ms; the first read before a pass matters only on Linux (Alpine's SQLite), where its mutant fails.
- 2bb1034a4 a true per-game lock (`cb_table_lock`/`cb_table_unlock` in table_ops.c); a bot computes unlocked and takes the lock only to commit (the store's version fence validates it).
  48 four-seat octogen tables at 1 CPU: human ack p99 1.3 to 0.8 ms.
- 9cbdf5a67 sessions found live are held in memory under serve (auth.c, `CB_PROV_ONE_PROCESS`): 5.7 to 1.3 us per HTTP token check; filled on first check rather than loaded at start.
- a2f4fe2af one instance per database (flock on `<db>.serve-lock`, boot.c).
- 682a0e345 unit_v1's flaky second-game pick (a stale `g_seen`), and s3_blob's sprintf (it broke the macOS ASan build).
- 4a55481ff staging fixes from the cutover prep, plus vm_boot.sh paging GetParametersByPath.
- aec8f168d export: schema 8 (`username_key` unique, `username` as shown), orphan chat skipped and counted, bulk reads paced to the table's RCU instead of the 5 RCU of an index the export never reads.
  The account merge and ELO reset belong to another agent now: stay out of foolish/server/migrate/.
- DESIGN.md "The VM path" rewritten to match the code.

### Numbers (final build F1, rig, CBASE_REQ_WORKERS=1)
- Human pace at 0.1 CPU: 64 games 390 us/move, p95 1.0 ms, RSS 8.5 MB; 192 games 250 us/move, p95 0.5 ms, p99 0.7 ms, RSS 10.9 MB.
- Saturation: 18.3k moves/s, p99 1.1 ms (C4 beside it 18.6k).
  The host's load average was 5 to 10 during this pass (other agents), and the same binary read anywhere from 7.8k to 18.3k, so compare only runs made back to back.
- WAL: 9.8 KB per move (463190 frames over 194089 moves, 2.4 frames per move: the games row's page and the game_moves page); 186603 commits for 194089 moves, so group commit rarely finds company at 16 clients.
  A smaller page size (1 KB pages: about 2.5 KB per move) is the obvious next experiment.
- 1024 games at 0.1 CPU (Alex asked for it over 192): started with `runbig.sh F1 0.1 1024`, result not collected before the stop; rerun it.

### Not done (in the order the list gave them)
1. Item 4, the rest of the Lambda leftovers on the serve path:
   - the store's `lease_*`, `bots_idle`, `games_needing_bots` and the SCAN marker: rows_foolish.c/.h, rows_sql.c (4 refs), bots.c (`in_memory()`, `drive_round`), lib_app_store_mem.c, lib_faultstore.c, unit_store.c (43 refs), unit_dynamo_ops.c, fz_dynamo_store.c, admin_panels.c;
   - bots.c `budget_left`, `BOT_DEADLINE_MARGIN_MS`, `rekick` and the `ctx->deadline_ms` branches (`main_lambda.c` sets it);
   - the after-phase (`core/src/after.c`, `cb_after_fanout` in fanout.c `held_run`, `cb_after_kick` in bots.c ~:422);
   - the row cache (table_ops.c `g_cache`, `cb_row_load`'s cache path, the 2-attempt recheck in `cb_table_run`), to be replaced by game state held under the per-game lock.
2. Item 5, 16-byte ids in the in-memory row structs (`cb_uuid` text in store.h rows, `cb_user`), text only at URLs and the wire.
3. Item 6: the long-game scenario (games forced past 100 moves, before = bin/serve-I0, which needs the old MQTT client `bin/p3load2-linux.new`), the smaps split at 192 vs 384 games (`mem.sh`, `smaps.py`), the open connection count, and sizing the per-thread pools and SQLite's cache to one worker.
4. A fresh saturation top-10 profile on a quiet host (`prof2.sh F1 8` with TMIN=0 TMAX=1, then `rep3.sh` in the perf image).

### Landmines found in this pass
- macOS make 3.81 compares mtimes in whole seconds: a mutation written in the same second as the last build is not rebuilt, and the test "passes".
  `scratchpad/mut1.sh` sleeps 1.2 s around each edit; earlier batch results without the sleep are not to be trusted.
- A fresh SQLite connection does not know the file is in WAL mode until its first read; on Linux a checkpoint before that is a silent no-op (log -1).
- `docker rm -f` kills the server without SIGTERM, so the WAL meter's close line needs `WAL=1` with `CBASE_RESTART=1`.
- The users.email column is a Supabase-ism (a transform of the username); it can go once sign-in resolves users through `username_key` everywhere.

## Move journal (branch move-journal, 2026-10-20)
Pushed, no PR (main decides). Commits on top of origin/main cef6bcb3b; `git log --oneline origin/main..move-journal` lists them.
`make -k test` and `make smoke` green on macOS.

### What it is (c_base/DESIGN.md "The move journal" has the full rule set)
- Kernel: `foolish/c/src/replay.c` exports the v10 rANS coder (`replay_coder_*`, replay.h); `foolish/c/src/replay_move.[ch]` (native kernel only, in KERNEL_SRC, not in the wasm lists) codes one move as seat + menu size (varint) + index into `legal_moves_into` (cap 1024) or an escape with the awire bytes; `replay_move_drive_walk` replays a bot cycle's moves on a scratch board with the drive's APPLY seeding (`table_drive_seeding`, table.c).
- v10 has no standalone per-step encoder (its model is a whole game with hidden-card reveals), so the record codes the index into the kernel menu the v6 family codes against, on v10's own coder; v10's move timing is a lossy log curve (7%), and the board clock needs exact ms, so time is an exact ms delta on v10's varint.
- Server: `foolish/server/src/store/journal.[ch]` (segments, batch codec with the slot table, overlay, flush thread, recovery, the decorating `cb_rows_ops`); `rows_sql.c cb_rows_sql_flush` (one transaction: dirty games fenced on their table version, then an optional commit or delete); `table_ops.c` (`section_row` shared by commit and replay, `cb_table_replay_move`, the eligibility rule in `cb_section_commit`); `bots.c` codes a cycle's moves around `table_bot_drive`.
- c_base: `cb_app.serve_open/serve_close` (boot.c, one-process providers), `cb_store.app_state/app_settle` + `cb_store_settle` (store.h), backup.c settles before VACUUM INTO, admin browse/query settle first.
- Env: `CBASE_JOURNAL=0` off, `CBASE_JOURNAL_FLUSH_MS` (2000), `CBASE_JOURNAL_FLUSH_MOVES` (4096), `CBASE_JOURNAL_SYNC=1` (fdatasync per batch).

### Tests (foolish/server/test/unit_journal.c, each mutation-checked with scratchpad mut.py, which bumps the restored file's mtime past make's same-second trap)
- codec round trip (3 batches, 5 games, a version jump, a third of the records bot cycles, escapes), size bound under 6 B per coded move;
- torn tail (mid-batch, mid-header) and flipped payload / checksum bits drop only that batch;
- a move on a board whose menu differs is refused (ENOTINMENU);
- for two humans and for a human against a bot: the flush writes rows byte-equal to per-move commits (games row + game_moves), records == commits since the deal; a backup mid-game holds every acknowledged move; 6 rounds of fork + SIGKILL after 3..23 acknowledgements plus a torn half-batch appended: every acknowledged move present, row + session log hash equal to the pre-crash process's, and the same after a reopen without the journal.
- Mutations that failed the named assertions: encoder follows-flag, checksum check, recovery skipped, torn tail made fatal, flush drops a move, backup without settle, replay_move index off by one, escape loses its bytes, menu-size check, bot cycles not journaled.
- Survived (documented, not covered): removing the APPLY reseed in replay_move_drive_walk (every rig/test game is a deterministic-deck deal, so the seed changes nothing there; it mirrors bot_drive for legacy boards), and the move-for-move check after the recovered bot drive (unreachable while prefs are reused; a defensive check).
- unit_app flake fixed (a_bot_and_a_human_on_one_game_lose_nothing): the hammer counted 150 looks, and under load every look came before the bot's first drive committed (instrumented: all polls at the start version, no move for the human, one drive in flight). Counts tries now; 6/72 loaded runs failed before, 0/72 after. Not a production race.

### Numbers (rig, CBASE_REQ_WORKERS=1, J0 = cef6bcb3b, J1 = this branch; `bench/nano/journal_meas.sh LABEL sat|h192` under the scratchpad nano.lock)
| | J0 | J1 |
|---|---|---|
| saturation, 8 games, 1 CPU | 19.1k moves/s, p99 1.0 ms, 50 us CPU/move | 19.9k moves/s, p99 0.8 ms, 50 us |
| bytes written per move (saturation) | 9.83 KB WAL (2.4 frames) | 14.7 B journal + 1.05 KB WAL |
| syscalls per move (perf stat raw_syscalls, 8 s) | 19.6 | 11.6 |
| 192 games, human pace, 0.1 CPU | 0.21 ms CPU/move, p99 0.6 ms | 0.20 ms, p99 0.6 ms, 24.7 B journal/move |
- The 1.05 KB of WAL left at saturation is the games that end every ~114 moves (finalize, replay, ratings, the rig's next create/join/start) and the 2 s flushes.
- With one request worker each batch held exactly one record (one move in flight at a time): the group commit coalesces only with more workers or bot jobs.
- At human pace each 2 s segment re-announces its games (a game code per game per segment), so a record there is about 25 B.

### Left
1. A real-hardware (Graviton2) measurement.
2. Recovery replays bot cycles through `table_bot_drive` with prefs; a seat the original drive tried and got rejected would search again (deterministic, so the same result, only slower).
3. `games_of_user` (the dashboard list) flushes first; if dashboards are hot under load, merge the overlay's updated_at instead (the list is ordered by it, with a LIMIT).
4. Admin SQL writes to `games` behind the journal's back: the next flush's version fence fails loudly; the overlay is not invalidated.
5. Longer segments (rotate on size, not every flush) would cut the per-segment game announcements at human pace.

# Verification of the RESULTS.md memory attributions (2026-11-09)

## Setup

Source: `git archive 284f0dc4f` of foolish, exported to verify/src/.
glibc build: amazonlinux:2023 arm64 container on the Mac (Docker), `make -j6 build/cbase` plus the p3load client exactly as pkg/load.sh builds it (scripts/build_al.sh).
Versions in that container: glibc-2.34-231.amzn2023.0.5, openssl-libs-3.5.8-1.amzn2023.0.1, sqlite-libs-3.40.0, gcc 11.5 (out/al/versions.txt), so the same glibc and OpenSSL as the nano.
musl build: alpine:3.24 (the digest pinned in bench/nano/build.sh), `make -j6 STATIC=1 build/cbase`, run in the same AL2023 container (static-pie, so no libs).
Server env: pkg/srv.sh's env, server pinned to 2 CPUs (taskset -c 0,1), client on CPUs 2-5, loopback (scripts/srv.sh, scripts/load.sh, scripts/scn.sh).
One load run at a time.
Raw data: verify/out/run/ (smaps, status, rss logs, malloc_info XML, preload dumps, gdb output).

## Reproduction first

glibc idle VmRSS 14,996 KiB (14.6 MiB), nano 14.6 MiB.
glibc sat2: p50 45.4, max 52.4, VmHWM 52.3 MiB at 33.8k moves/s (out/run/load_sat2a.txt); nano p50 41.6, peak 52.5 at 23.7k moves/s.
A second glibc sat2 gave p50 47.8, max 56.2 (sat2b), so run to run spread is a few MiB.
The mid-sat2 smaps has the same shape as the nano's: a 25,932 KiB anon mapping at a 64 MiB boundary with 22,872 KiB resident, a 3,512 KiB one fully resident, four thread stacks at about 1,700 KiB resident, and a 2,256 KiB block at 2,252 KiB (out/run/smaps_sat2a_mid.txt).
The numbers land near the nano's, so the attributions below are made on this reproduction.

## 1. The four 10 MiB stack mappings with about 1.7 MiB resident each

Verdict: the mechanism (static TLS at the top of each thread stack, made resident at thread start) is confirmed; the suspected objects are refuted; the TLS is the game kernel's, not c_base's.
`readelf -lW build/cbase`: `TLS ... 0x0000e8 0x1a4670`, so PT_TLS filesz 232 B and memsz 1,722,992 B (1,683 KiB).
The 254 TLS symbols add up to 1,721,764 B with no unlabelled gap (scripts/tlsgaps.py).
TLS bytes per object file (scripts/tls_by_obj.sh, out/tls_by_obj.txt):
- foolish/c/src/cordite_sim.o 832,344 B: solve_ws 278,592, world_slot / trial_slot / diff_slot 135,952 each, sim_rec_moves 86,400, solve_child_scratch 37,632, sim_rec_child 15,744.
- foolish/c/src/astrolite_strategy.o 276,148 B: trial.0 and world.1 135,944 each.
- foolish/c/src/bot_drive.o 237,580 B: g_scratch (LegalMoves) 237,572, bot_drive.c:87.
- foolish/c/src/analyse.o 135,944 B: an_world_g (Game), analyse.c:456.
- foolish/c/src/distill_feat.o 57,512 B, replay.o 15,632 B, replay_extras.o 12,288 B.
- The game kernel (foolish/c) totals about 1,567 KB of the 1,723 KB, 91%.
- foolish/server: table_ops.o 83,032 B (buf.3 65,536), rows_foolish.o 40,009 B (sks.5 40,000), v1_http.o 10,424 B, pushes.o 3,560 B.
- The suspected c_base objects are small: realtime.o t_batch 5,136 B, items.o strs 4,096 B, stream_mqtt.o in 4,096 B, sigv4.o t_keys 936 B, together about 14 KB, 0.8% of the block.
chuiniu is not linked: no chuiniu object or cn_bot symbol in the binary's TLS list.
Placement: gdb `p/x $tpidr` per thread, e.g. req-0 tp 0xffff982ef260 inside the stack mapping ffff97a94000-ffff98494000, 1.65 MiB below its top, and AArch64 TLS sits above tp, so the block fills the top of the stack.
Residency: each stack shows 1,692-1,708 KiB Rss, which is the 1,683 KiB block plus 2-6 pages of real stack, on threads that never run game code (the checkpointer and the flusher show the same 1,692 KiB).
Experiment (verify/exp/tls_touch.c, a 1,700,000 B _Thread_local array never touched by the threads, same glibc): VmRSS 2,832 KiB, then +1,884, +1,668, +1,668, +1,668 KiB per idle thread, so glibc 2.34 dirties the whole static TLS block at pthread_create.
The 5 threads (gdb `thread apply all bt`): main (the event loop, cb_loop_run, its TLS in item 2's block), req-0 and job-0 (pool.c:58 worker, CBASE_REQ_WORKERS=1, CBASE_BOT_WORKERS=1), the sqlite checkpointer ck_main (store_sqlite.c:444, started at :478), and the journal flusher (journal.c:1684, started at :2664).
store.c:75's side thread is not running in this config.
Glibc static TLS cost: 5 threads x 1,683 KiB = 8.2 MiB of the 14.6 MiB idle RSS.
Why 10 MiB: the mapping size is RLIMIT_STACK, not code; with `ulimit -s 8192` the container's stacks are 8,192 KiB, and with `ulimit -s 10240` they are 10,240 KiB with the same Rss, and no pthread_attr_setstacksize is used for these threads (pool.c:84, store_sqlite.c:478, journal.c:2664 pass NULL attrs).
The nano's 10 MiB therefore means its systemd-run unit had a 10 MiB stack limit (inferred from this, not read on the nano); it is virtual only.
Fixable: yes; replay.c:193-205 already documents this exact glibc cost and moved its 936 KB scratch to a lazy per-thread heap block (thread_heap_calloc); the same move for cordite_sim, astrolite_strategy, bot_drive, analyse and distill_feat would leave the checkpointer, the flusher and any thread that never runs a bot at about 150 KB of TLS.

## 2. The 2.2 MiB anon block just below libc

Verdict: identified; it is two allocations in one merged VMA, the main thread's static TLS plus the log ring.
gdb breakpoint on mmap at startup (scripts/gdb_mmap.gdb, out/run/gdb_mmap.txt):
- `__mmap64 (len=1732608)` from `rtld_malloc (n=1725832)` from `_dl_allocate_tls_storage () at dl-tls.c:465` from `init_tls` in rtld.c:800: the main thread's 1,683 KiB TLS block plus its TCB.
- `mmap64` from `calloc` from `cb_log_init (min_level=1, ...) at core/src/util/log.c:27`: 577,536 B, the 2,048-entry log ring (282 B per entry), placed directly below and merged into the same VMA.
1,732,608 + 577,536 = 2,310,144 B = 2,256 KiB, the mapping's size.
gdb: the main thread's tp 0xffff985217e0 lies in that block (ffff98494000-ffff986c8000).
Idle Rss 1,700 KiB is the TLS block; sat2 Rss 2,252 KiB is the TLS block plus a full log ring once 2,048 lines were written.
Fixable: the TLS part goes with item 1; the ring is 564 KiB by design (a smaller cap or a smaller entry would shrink it).

## 3. The bss mapping 0x541000-0x1384000

Verdict: the 14.3 MiB is virtual only; it is not a cost.
Resident: 12 KiB idle and 72 KiB mid-sat2 on the nano (cap_idle_smaps.txt, cap_sat2mid_smaps.txt), 12 KiB idle here.
.bss holds 169 objects, 14,954,506 B (scripts/bss.py), the largest being analyse.o's g_nodes 10,739,712, g_cells 2,088,960, g_acts 278,528, g_perms 262,144, g_menu 237,572, and replay_steps.o's acts.1 278,528.
analyse.c is analysis code linked into the server but never run by it, so those pages are never touched.
Fixable: only cosmetically (VmSize); dropping analyse.o from the serve link would also drop its 133 KiB an_world_g TLS from item 1.

## 4. The 26 MiB and 3.4 MiB mappings under sat2

Verdict: both are glibc thread arenas (confirmed); the 26 MiB one is mostly live data, the journal's resident-game overlay, not freed memory the arena keeps.
malloc_info at mid-sat2 (gdb `call malloc_info`, out/run/mi_sat2b.xml, scripts/mi.py): heap 4 system 29,328 KiB, free 7,995 KiB in 3,826 chunks, so about 21.3 MiB in use (73%); heap 1 3,516 KiB with 139 KiB free.
Who: an LD_PRELOAD heap attributor (verify/exp/mtrace_live.c, 7-frame backtrace and allocating thread per allocation, dumped mid-sat2, out/run/mtl_sat2m2_sym.txt):
- Live bytes per allocating thread: req-0 19,382,125, flusher 6,167,968, job-0 5,445,852, main 1,943,399, checkpointer 269,240.
- res_copy journal.c:561 (TableResident copied into the overlay, from cb_journal_game_created, cb_table_create table_ops.c:1376), req-0: 8,556,624 B in 4,098 blocks, 2,088 B each.
- grow_copy journal.c:503 via row_assign / ov_insert journal.c:617 (the row's fixed-width roster, ROSTER_BYTES 1,227), req-0: 5,025,792 B in 4,096 blocks.
- ov_insert journal.c:615 (the jgame struct), req-0: 3,833,856 B in 4,096 blocks, 936 B each.
- row_refresh journal.c:530 (state buffer grown to TABLE_STATE_MAX, 824 B), flusher: 2,755,456 B in 3,344 blocks, job-0: 588,336 B in 714.
- The block counts sit at 4,096, which is OV_MAX_GAMES (journal.c:39, "games the overlay keeps resident", journal.h:93), with no env knob.
- So the overlay holds about 5.0 KiB per resident game x 4,096 games = about 20 MiB at saturation; sat2 creates games far faster than the 60 s idle eviction (IDLE_EVICT_MS) retires them.
- finalize_game table_ops.c:492, job-0: one 3,407,872 B block, the per-section kept replay scratch (CODE_CAP 2 MiB + SCRATCH_CAP 1 MiB + EXTRAS_CAP 256 KiB); this is the 3.4 MiB arena's content.
- coder_scratch_ready (replay.c thread_heap block): 958,484 B on req-0 and again on job-0.
- sqlite page cache (pm_malloc store_sqlite.c:663) on the flusher: about 3.3 MiB.
Allocator tuning does not help, which confirms the data is live: MALLOC_ARENA_MAX=1 gave p50 46.7 / max 54.3 MiB, and MALLOC_ARENA_MAX=1 MALLOC_TRIM_THRESHOLD_=65536 MALLOC_TOP_PAD_=0 gave p50 46.7 / max 53.8 MiB, against 45.4 / 52.4 baseline.
The roughly 8 MiB of free chunks inside the big arena is real fragmentation, but it is churn between live overlay rows, not trim failure.
Fixable: yes, by design choices rather than allocator flags: a smaller resident cap, evicting a game from the overlay when it finishes, not keeping both the roster bytes and the decoded TableResident, and sizing the state buffer to the state; and freeing the 3.25 MiB finalize scratch (and the 936 KB coder scratch) after the game end instead of keeping it per section.

## 5. musl (static, as the Graviton pass ran it)

Verdict: the idle number is largely a glibc artifact; the sat2 growth is not.
musl static build PT_TLS memsz is the same 0x1a46b0 (1,723 KB).
musl idle VmRSS 4,796 KiB (4.7 MiB) against glibc 14.6 MiB (out/run/status_musl_idle.txt), because musl does not dirty .tbss at pthread_create and links everything into one file.
musl sat2: p50 36.4, max 43.2, VmHWM 45.1 MiB at 28.4k moves/s (out/run/load_musl_sat2.txt), against glibc p50 45.4, max 52.4.
musl sat2 still has about 31 MiB anonymous (smaps Anonymous 31,020 KiB), consistent with the 20 MiB overlay plus the finalize and coder scratch, which are allocator independent.
Unproven: how much of each thread's game TLS a musl thread touches under bot load (not measured per mapping).

## 6. libcrypto 1.5 MiB idle, 3.7 MiB sat2

Verdict: mostly dynamic-linking and file-backed page cost; the only OpenSSL call on the play path is RAND_bytes.
Per segment (cap_idle_smaps.txt vs cap_sat2mid_smaps.txt): first r--p segment (dynsym, dynstr, hash, relocations) 880 KiB in both, clean file-backed; text 128 KiB idle, 1,948 KiB sat2, clean file-backed; rodata 64 -> 440 KiB; RELRO 444 KiB private dirty in both; .data 12 KiB.
Only the 444 + 12 KiB RELRO and .data are private memory; the rest is reclaimable page cache mapped into the process.
gdb breakpoints on HMAC, SHA256, SHA1, RAND_bytes, EVP_DigestInit_ex and CRYPTO_memcmp for 4 s of sat2 play (scripts/crypto.gdb, out/run/gdb_crypto.txt): 3,004 hits, all RAND_bytes via cb_random_bytes (random.c:15).
- 1,801 from cb_table_run table_ops.c:1229 (h_meta), 602 from cb_uuid_v4 in finalize_game table_ops.c:512, 601 from cb_game_id_random table_ops.c:62.
- No HMAC: jwt.c keeps a per-thread verified-token cache (jwt.c:67).
The binary imports 35 OpenSSL symbols (`nm -D --undefined-only`): RAND_bytes, HMAC/EVP_sha256/SHA256 (jwt.c, auth.c:303, admin.c:76), SHA1 (ws.c:88, the WebSocket handshake), and SSL_* used only by providers/aws/src/aws/https.c (SSM, IoT, AWS APIs), which this local-sqlite config never calls.
Is OpenSSL needed here: not for TLS; RAND_bytes could be getrandom(2) and SHA256 / HMAC could use shared/c/sha256.c, which is already in the tree and tested against OpenSSL (shared/c/sha256_openssl_test.c).
Unproven: which libcrypto functions behind RAND_bytes (provider DRBG) account for the 1.8 MiB of text, since kernel fault-around also maps neighbouring cached pages and inflates file-backed Rss.

## Top memory wins, ranked by MiB saved per app (not implemented)

1. The resident-game overlay (journal.c OV_MAX_GAMES 4,096 x about 5.0 KiB): up to about 20 MiB at saturation on either libc; evict finished games, drop the duplicate roster and state copies, or lower the cap.
2. Static TLS of the game kernel (cordite_sim, astrolite_strategy, bot_drive, analyse, distill_feat; 1,567 KB per thread): 8.2 MiB at idle on glibc (5 threads x 1,683 KiB), measured gone under musl (4.7 MiB idle); move it to the lazy thread_heap block replay.c already uses.
3. Kept per-thread replay scratch: finalize_game's 3.25 MiB section buffer (table_ops.c:492) plus the 936 KB coder scratch on each of req-0 and job-0, about 5.1 MiB once both threads have ended a game; free after the game end or size it to the game.
Smaller items: the 564 KiB log ring (log.c:27), and the 444 KiB OpenSSL RELRO plus about 1-2 MiB of reclaimable libcrypto page cache if RAND_bytes and SHA256 move off OpenSSL.

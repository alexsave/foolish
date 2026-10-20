# The server the t4g.nano wants

Design against main 4aa1c22e3, read-only, with the model of NANO_MODEL.md.
Every file:line is an edit point in this tree.
"us" below are microseconds of cycles at 2.5 GHz on the nano (NANO_MODEL.md 2.5 says why not task-clock); "warm" means the move without the 45 us cold start a parked vCPU pays at human pace.

## 1. Where a move's cycles go on the real nano

Source: graviton/runs/prof2_hum2_NOW (perf record -g, cycles, 30 s of hum2 on NOW: 192 two-player games, 98 moves per second, one human move per request over the socket), scaled to the 83.3 us of cycles per move of the three hum2 NOW runs.
The children column of rec_children.txt is the attribution; self percentages name the heavy leaves.

### 1.1 hum2 NOW: 83.3 us per move, of which about 45 are the cold start

| where | share | us | what it is |
|---|---|---|---|
| sendto, 2 per move | 26.6% | 22.1 | the actor's REPLY+VIEW+CAST in one segment (flush inside ws_frames, server.c:659) and the opponent's VIEW+CAST (flush_dirty, server.c:683): tcp_sendmsg_locked 22.6%, ip6_xmit 10.3%, dev_queue_xmit 7.7%, ena_start_xmit 3.75%; the softirqs that run at the syscall's exit process the previous move's ACKs here |
| recvfrom, 1 | 9.8% | 8.2 | the request frame (about 100 B): tcp_recvmsg_locked 4.7%, skb_copy_datagram_iter 2.6% |
| epoll_pwait, 1 | 9.9% | 8.2 | ep_poll 6.45%: the sleep's schedule_hrtimeout_range 2.6% (hrtimer_start_range_ns 0.94% self and hrtimer_try_to_cancel 0.41%: the loop arms a timeout on every wait because the 1 s sweep timer is always pending, loop.c:418-422), ep_send_events 2.7% |
| kernel, all | 46.1% | 38.4 | 45.4% of it inside the three syscalls above; 0.7% interrupts and softirq charged while the task ran |
| h_action (functions_http.c:135) | 35.5% | 29.6 | the handler, every byte of the game |
| of which cb_section_commit (table_ops.c:871) | 24.9% | 20.7 | products, casts, journal, fan-out |
| of which cb_fanout_publish (fanout.c:97) | 8.0% | 6.6 | 5 pushes: rt_publish_view 5.35% (2 views + the 2 "left" checks), rt_publish_cast 2.24%; local_deliver 6.0% of which pub_deliver 4.67% (1.83% self: the user bucket walk with strcmp of 36-byte ids, cb_server_wake's parked walk, cb_ws_send -> queue_frame -> 2 memcpy), twoway_strstr 1.29% self (local_deliver's strstr(pattern, "{user}") per push, rt_local.c:801, plus octet_body's strstr on the content type) |
| of which j_game_commit (journal.c:1515) | 6.7% | 5.6 | write_batch 4.63%: cb_journal_batch_encode 2.8% (the bignum range coder for a 10 B record: coder_finish 0.53%, code_varint 0.45%, sink_packed 0.46%), rotates/seg bookkeeping and ov_apply with the 2 KB resident copy 1.6%; the fence and the record 0.6% |
| of which table_commit_products_flags (table.c:341) | 5.2% | 4.4 | envelope_put x3 2.4% (state_put_rules 0.83% self, roster_trailer_write_validated 1.04% self, roster_validate 0.75% self, id_ok 0.31%), op_stream's evwire_walk 0.7%, the log records, next_clock |
| of which section_cast x3 (table_ops.c:388, evwire_serialize) | about 3.5% | 2.9 | three serializations of the same two events: the actor's, the opponent's, the spectators' (nobody listens at hum2) |
| of which section_row, fan-out build, hex_encode of the seed, table_resident | about 2% | 1.7 | hex_encode 0.35% self (table_ops.c:993-995, the seed re-hexed on every commit), game_state_seed 0.38% |
| table_act (table.c:108) | 2.6% | 2.2 | awire_decode 0.44%, the apply with its snapshots (handle_attack 0.40%, handle_cover 0.41%, game_shown_good_mask 0.36%), finalize |
| wrap400 (functions_http.c:135 -> cb_auth_verify_bearer 0.33%) | 2.5% | 2.1 | the user copy and the content-type strstr; no HMAC on the socket path |
| game_acquire + table_adopt + cb_section_new/free | about 1.5% | 1.2 | memset 1.12% self (cb_section_free, table_ops.c:310-314: the Game head, the host fields, the arena prefix), the 2 KB adopt memcpy |
| socket plumbing (ws_frames, pump, job_run_frame, sf_parse, job_settle, reply_to) | about 3% | 2.5 | |
| conn_io self + the recv copy into c->in | 2% | 1.7 | |
| cb_poller_wait self 1.34%, cb_loop_run self 1.46%, cb_loop_set 0.41%, mark_dirty 0.40%, flush_dirty | about 3.6% | 3.0 | the event array walk and the watch lookups |
| sweep (server.c:858) | 0.76% | 0.6 | the 1 s walk of 384 connections, amortized over 98 moves |
| clock reads (vdso 0.86%) | 0.9% | 0.7 | about 20 clock_gettime per move at 31 ns (cb_mono_us x2 for the valve, cb_app_now, cb_mono_ms in the loop, the journal, the overlay) |
| memcpy 2.57% self, strcmp 1.68%, strlen 0.88%, malloc 0.77% | | | the leaves, already inside the rows above |

The rows sum to 77 of the 83 us; the remaining 6 us are leaves under rec_children's 2% cutoff (the sccp syscall stub 0.82%, malloc 0.77%, the vdso, and the tails of memcpy, strcmp and strlen outside the rows that name them).
Warm, the move is about 38 us: 37 us of kernel for two sends, a recv and a wait, and the handler's user side is under 25 us of the 83.
At human pace the cold start multiplies everything by about 2.2, which is why the per-function shares are what to optimize and the thread count is what to design.

### 1.2 The other scenarios

| scenario | cycles us per move | what is in it beyond hum2 |
|---|---|---|
| sat2 NOW | 47.4 | warm; sends 2.1; plus about 10 us of saturation-only work per move: a game end every 75 moves (the pool bounce for the ratings, around()'s SQLite transaction, the finalize job), three setup commits per game, 7% refused moves at 20 us each, RX softirq charged to the task (handle_softirqs 5 to 6%) |
| hum8 NOW | 124.4 | cold start 45; 9.5 sends (11 hearers, some coalesced into one segment); 9 casts and 8 envelopes built from a bigger board; 11 pub_deliver walks |
| sat8 NOW | 56.0 | warm; 4.9 sends per move because a socket hearing several moves in one pass gets one segment; the same 9 casts and 8 envelopes |
| bot NOW | 18372 per table version | octogen's Monte Carlo: 10 ms per drive on the bot thread; the move path is invisible under it; the 1.96 switches per version are the kick (loop -> job pool) and the drive's commit (bot thread -> loop writer -> bot thread) |

The c6g instruction counts say the same from the other side: hum2 NOW is 64k instructions per move, sat2 NOW 123k.
The 60k instructions a saturation move carries over a human-pace one are the game churn (finalize re-plays a whole game, the SQLite flush writes a row per move, the setup commits) spread over the moves, not the move itself.
BASE was 250k instructions per move (4x), of which SQLite's own path was the bulk.

## 2. The idle and human-pace story

- The 1.8 GHz is perf's PMU emulation cost, 28.5 us of task-clock per context switch during which the guest counts no cycles (NANO_MODEL.md 2.5).
  Production has no perf and no such cost; the loop's 0.95 switches per move are its own sleep in epoll_wait and its wake by the request, and cost what section 2.1 of the model says a same-core wake costs, about 10 us of wall, not 28.
- 6.9 idle exits per move at hum2 (the IRQ experiment's count, no perf attached) are: 4.7 ENA interrupts (the request in, the two segments' TX completions and the two ACKs, moderated), 1.7 timer interrupts, 0.6 function-call IPIs of which 0.13 to 0.47 are the loop's wake landing on the vCPU it is not on.
  The cross-vCPU wake costs 2 to 10 us of latency more than a same-core one at these idle intervals (the vCPU wakes every 5 to 10 ms, so the model's 1 to 10 ms rows apply, not the 100 ms one), on 0.13 to 0.47 wakes per move: under the noise of a 240 us round trip.
  The c6g shows 11 exits per move because it has a cpuidle driver and keeps a tick; the nano's WFI-only idle has fewer and each is dearer.
- The box pays about 157 us per hum2 move around the server's 65: 48 us of softirq (NET_RX 37, TIMER 4.5), 18 us of hardirq (7 interrupts at 2.6 us each including the hypervisor's injection) and 25 to 30 us of idle entry and exit, scheduler and hypervisor time (NANO_MODEL.md 2.7).
  Only the softirq that happens to be pending at the end of one of the loop's own syscalls is charged to the loop (5 to 7 us per move; variant E of the IRQ experiment, with every interrupt on the other vCPU, took exactly that off the server's number and nothing off the box's).
  The kernel's receive path is as cold as the server's at human pace: NET_RX costs 15 us per NAPI poll at hum2 against 7 us at saturation, 8x per packet, and the AF_XDP spike shows the same 8x (90 us against 11 for the same two segments).
- 0.95 context switches per move in NOW: one, the loop's.
  No other thread wakes for a human move in a human game: the journal writes on the loop (journal.c:956 loop_append), the pushes are delivered on the loop (rt_local.c:804), the flusher wakes once a second on its own tick (journal.c:1282, TICK_MS 1000) whether or not anything waits, the server sweep once a second on the loop (server.c:895), the bot scan every 10 s (foolish_app.c:118, CB_BOT_SCAN_EVERY_MS), the reap timer when a connection closed.
  In a bot game the move also wakes the job pool (the kick: functions_http.c:166 -> provider_local.c:77 local_job_start -> cb_pool_submit, a futex wake into a parked vCPU) and the bot thread's commit wakes the loop (journal.c:887 loop_tell: cb_loop_post + eventfd) and waits on a condvar for the loop's write (append, journal.c:999-1006): three cross-vCPU hops per bot cycle, each 10 to 40 us of latency and tens of us of CPU on the nano.
- At a game end, a deal, a join, a create and a leave the move path leaves the loop: cb_section_commit returns CB_E_WOULD_BLOCK for the ratings (table_ops.c:895) and journal.c:1458 for around(), the request runs again on the request pool (rt_local.c:604 after the inline attempt did the whole apply and products for nothing), reads accounts and ratings from SQLite (table_ops.c:758-788), writes a SQLite transaction under the global flush lock (journal.c:1459), and publishes its pushes from the pool through the pq copy and a post (rt_local.c:811-839).
  That is two cross-vCPU hops and a cold start of the pool thread per such commit, and the pushes of the commit leave one post later than a loop-side commit's.
- Production pace (a few moves a minute, every vCPU parked between them): the per-move CPU is about 83 us of which 45 is cold start; latency is dominated by the vCPU wake (10 to 40 us) and the two sends.
  The fixed background that wakes a parked vCPU with nothing to do is the whole idle cost: the flusher's tick (86400 wakes a day), the sweep (86400), the bot scan (8640), each about 20 us of CPU plus a hypervisor exit.
  None is needed when nothing is waiting, nothing is connected and no bot game is live.

## 3. The thread architecture this hardware wants

The nano's rule (NANO_MODEL.md 2.1 to 2.3): a thread that takes part in a move costs 45 us before it does anything, a hand-off between vCPUs costs 40 us of cycles for the data that moves and 10 to 40 us of latency, and anything hot shared across vCPUs costs microseconds per touch.
Today's six threads: the loop, two request workers (CBASE_REQ_WORKERS=2 in production), one bot worker, one job worker, the journal flusher.

Threads that should exist:

1. THE LOOP.
   Every move, every push, every journal write, every timer, every read of a resident game (views, the dashboard list, the bot scan's due list) runs here.
   Not pinned, and the interrupts not steered, for production: the IRQ experiment (NANO_MODEL.md 2.8) tried every placement and none moved latency, box CPU or credits at human pace; steering the interrupts to the loop's vCPU cut the idle exits from 6.9 to 5.5 per move and the cross-vCPU wakes to nil, and the loop's cold start stayed where it was, because the cold state is the parked vCPU's and not the TCP path's.
   The first draft's prediction that the RX softirq would warm the loop's core was wrong (2.8 item 2).
   At saturation only, the loop alone on one vCPU with every other server thread on the other (C2) is worth +6% throughput, -11% box CPU per move and -30% push p95, from the loop never being preempted by the pool or the flusher; it is a knob (section 9 step 5), default off, never `CPUAffinity=` on the unit (that pins the pool too, costs 17% at saturation, and halves the request pool because the pool is sized from the allowed CPUs).
2. THE TABLES THREAD, vCPU 1: the one owner of SQLite.
   The flush, around() (rated ends, deletes), the ratings and account reads a rated end needs, sign-in and sign-up, history and replay reads, snapshot_put.
   One thread removes flush_mu as a contention point (section 4) and makes "the loop never waits on SQLite" structural: a request that needs the tables is handed to this thread and answered by a post, as the pool does today, and there is exactly one such thread, so two SQLite users never wait on each other inside the process.
   This is today's request pool with CBASE_REQ_WORKERS=1 and the flusher folded in as a task that the loop schedules (section 4.3), not a thread with a tick.
3. THE COMPUTE THREAD, vCPU 1: bot drives and the replay re-encode of a finished game, which are CPU and nothing else.
   finalize's snapshot_put goes to the tables thread as a task; a bot drive's commit goes to the loop as today (loop_tell).
   This is the job pool and the bot pool as one pool of one thread.

Four threads, three of them asleep at production pace, and the move path touches one.
What moves between them is per game (an end, a deal, a seat change, a bot cycle), never per move.
A pool of one thread is also what musl's allocator wants: the malloc_free_2thr rows (NANO_MODEL.md 1.4) show any concurrent allocator traffic from the other vCPU making every malloc on the loop 5 to 10x dearer, so the compute thread's finalize and bot scratch must be kept per thread (the WIP's s->fin, section 5) and never allocated per game.

## 4. The game-end push regression and the valve

### 4.1 What the rig saw

At saturation NOW delivers the game-ending move's pushes to the other viewers up to 2.8 s late (rig/final_rig.md "Odd things" 1): 566 to 787 receipts over 100 ms per sat2 run, 1720 to 1924 over 1 s per sat8 run, 95% on the move that ended a game, while the mover's answer is fast.
On the nano the same tails show as push max 3.3 s (sat2) and 1.9 s (sat8) against move max 0.55 and 0.09 s, and 1 to 5 lost pushes per sat2 run.

### 4.2 What the code does at a game end

- The ending move runs inline on the loop: table_act, products, then cb_section_commit sees wants_ratings and returns CB_E_WOULD_BLOCK (table_ops.c:895) with nothing written.
- pump resubmits the same frame to the request pool (rt_local.c:598-605); the handler runs again from the start on a pool thread: adopt, apply, products, accounts_old_enough (one SQLite read per human seat, table_ops.c:766), ratings_for (table_ops.c:785), table_rate, products again, section_row.
- j_game_commit sees n_rated > 0 and takes around() (journal.c:1524 -> 1456): flush_mu, then cb_rows_sql_flush writes the game's row, its waiting moves (hundreds to thousands of game_moves rows at saturation) and the ratings in one transaction (journal.c:1476).
- finalize_later queues the replay job (table_ops.c:1015 -> 608), and cb_fanout_commit publishes the 11 pushes from the pool thread: each is copied into a pub and queued, one cb_loop_post tells the loop (rt_local.c:811-839); the reply follows as another post (rt_local.c:491).
- The loop delivers the pushes and the reply in the same drain.

Three things on that path can hold the pushes for seconds, and the logs show two of them firing:

1. THE VALVE.
   sat2_NOW_r1 and sat2_NOWu_r1 both log "3 inline requests over budget within 1000 ms (the last 2673 us, budget 2000): inline dispatch is off for 5000 ms", tripped twice per run.
   cb_server_inline_done (server.c:467) times the handler by wall clock (cb_mono_us, server.c:508-510 and rt_local.c:589-591); on the nano at saturation both vCPUs are busy and the hypervisor deschedules them for milliseconds (the contended-atomic row's p99 of 24 ms and max of 96 ms in NANO_MODEL.md 1.1 are such holes), so a 20 us handler reads as 2.7 ms three times in a second and the valve trips.
   For the next 5 s EVERY move goes to the two request workers (4760 "sent on to the pool" in that run): each then pays a pool hand-off in, a journal write that posts to the loop and waits on a condvar (journal.c:999-1006), a pq push copy and post out, and a job_done post; two workers at that cost serve a few thousand moves per second while the clients offer 23k, so the pool queue grows for 5 s and everything behind it, game-end pushes included, waits seconds.
   This is NOW-specific (BASE had no valve) and saturation-specific, which is the regression's signature; sat8 NOW did not trip on the nano in r1, so it is not the only cause.
2. THE FLUSH LOCK.
   around() takes flush_mu (journal.c:1459) and the flusher holds it for the whole of flush_locked (journal.c:1227): snapshot_take, one SQLite transaction for every due game, ov_flushed, trim.
   At saturation the flush is due by game_bytes every few seconds and writes tens of thousands of game_moves rows per pass (sat2_NOW_r1: 136690 WAL frames over 15455 commits in 65 s); a pass of 0.5 to 2 s blocks every rated end, deal, join, create and delete behind it, on a pool thread that holds the game's lock, with the pushes of that commit unsent.
3. THE POOL QUEUE.
   Every non-plain commit of every game at saturation (280 per second at sat2: an end and three setup commits per 75 moves) runs on two workers, each a SQLite transaction behind flush_mu; the queue's tail is seconds when 2 fires.

### 4.3 The fix: the push never waits for anything but its own record

The push of a commit must leave in the same loop pass as the commit's answer, from the loop, after the journal record and nothing else.
That is already true of a plain move; it must become true of a game end.

Step E1, the end commit stays on the loop (design):

- The ratings and account ages a rated end needs are read when the seat is taken, not when the game ends: cb_table_run for a join or a create already runs on the tables thread (it writes SQLite), so it reads `rating` and `created_at` of the seat's account there (table_ops.c:758-788 moved to the seat's commit) and stores them in the resident roster seat (foolish/c/src/roster.h RosterSeat gains `int32_t rating; int64_t account_created_ms;`, carried by the journal's full record like the rest of the roster).
  The end commit then rates from the resident (table_rate, table.c:802) with no read.
- A rated end becomes a journal full record that also carries the rating deltas (journal.c:671 full_encode gains the n_rated deltas; recover_full applies them to the resident), written by loop_append like any other commit; the tables thread writes the rating rows and the leaderboard at the flush (journal.c:1227 flush_locked passes the deltas to cb_rows_sql_flush), as it already writes every other row.
  The leaderboard and the profile read ratings from the tables, so a flush must follow a rated end promptly: the end marks the game due (flush_due, journal.c:1132 gains `g->rated_pending`) and kicks the flusher, which writes it within one tick.
  Durability is unchanged: the record is in the mapped segment before the answer (journal.c:833-839); the rating rows are derived from it at recovery as the game rows are.
- cb_section_commit (table_ops.c:895) loses its CB_E_WOULD_BLOCK; j_game_commit (journal.c:1524) routes a rated end to the journal, and around() is left to deletes.
- Saving per game end: two cross-vCPU hops (about 80 us of cycles and 20 to 80 us of latency), one cold start of a pool thread (45 us), 2 to 8 SQLite reads, one SQLite transaction on the request path, and the pushes leave with the reply.
- Tests: unit_journal gains "a rated end is a journal record": commit a rated end on the loop thread, assert no SQLite write happened before the answer (a cb_rows_sql hook counting writes) and that the flush wrote the rating rows and the deltas match table_rate's; mutation: drop the deltas from full_encode and the recovered rating differs.
  The SIGKILL recovery test gains a rated end before the kill; mutation: skip applying the deltas in recover_full and the leaderboard after recovery differs.
  The existing e2e game-over flow (web and iMessage are unaffected: both read the end from the cast).

Step E2, the valve stops reading the hypervisor (quick, lands first):

- cb_server_inline_done (server.c:467) counts a run as over budget only when the loop's pass as a whole is late: measure `took_us` with CLOCK_THREAD_CPUTIME_ID (255 ns per read, two per move, 0.5 us) so a descheduled vCPU does not count; or keep the wall clock and require inline_trips over budget in a row within one pass (server.c:473-478) rather than within a second.
  Both remove the trip on a 2.7 ms hole; the thread clock is the honest one.
- When the valve trips, /v1/action does not go to the pool: a route marked CB_ROUTE_INLINE whose handler returns to_pool on its own already names the one case it must leave the loop; the valve's job is to protect the loop from a handler that is slow on the loop, and a move on the pool is slower for everyone (section 4.2 item 1).
  Keep the valve for routes that are inline only as an optimization (GET /v1/games/*), and make the budget per route (router.h cb_route.inline_us is already read at server.c:470).
- Expected: zero trips at saturation on the nano (sat2 NOW r1 and NOWu r1 tripped twice each; sat8 and the human-pace runs never), move max at sat2 from 550 to 777 ms down to the sat8 figure (about 90 ms), lost pushes 0.
- Test: unit_server_edges (or a new unit) feeds a handler that sleeps 3 ms of wall but 20 us of CPU (nanosleep inside) three times in a second and asserts tripped == 0; mutation: time by cb_mono_us again and it trips.

Step E3, the flush is many small transactions, and flush_mu holds only the bookkeeping (quick):

- flush_locked (journal.c:1227) takes at most FLUSH_TXN_MOVES (2000) waiting moves per transaction and loops, releasing flush_mu between transactions; snapshot_take (journal.c:1138) selects the due games up to that budget, oldest first_ms first.
- around() (journal.c:1456) then waits at most one bounded transaction; with E1 only deletes come here.
- The WIP's game_moves cap (section 5) becomes unnecessary: the transaction size is bounded here, its one owner.
- Test: unit_journal with 10 games of 5000 waiting moves each, a flush, assert every cb_rows_sql_flush call carried <= 2000 moves and that a concurrent around() waited less than one transaction (a hook stamps flush_mu's hold times); mutation: remove the budget and the assertion on the longest hold fails.

Step E4, the flusher without a tick (quick, the idle story):

- flusher (journal.c:1280) waits on fcv with no timeout; the loop arms a timer when the first move of a game starts waiting (ov_apply, journal.c:486: g->first_ms is set there) for game_age_ms, and kicks the flusher when it fires or when game_bytes or total_bytes is reached (write_batch already kicks, journal.c:858).
  prepare_next (the segment made ahead) is kicked by seg_open's ask_prepare as today.
- Saving: 86400 wakes a day of a parked vCPU with nothing to flush; nothing per move.
- Test: unit_journal asserts the flusher's wake count (a counter in flusher) is 0 over 5 s with nothing waiting and 1 after a move plus game_age_ms; mutation: restore the tick and the first assertion fails.

## 5. Memory

### 5.1 Where the RSS is

sat2 NOW on the nano: VmRSS 68 MB, RssAnon 58 MB, RssFile 9.7 MB (graviton/runs/sat2_NOW_r1/status_end.txt, smaps_rollup Anonymous 58480 kB).
hum2 NOW: 12.4 MB (anon 6.7, file 5.7); BASE hum2 9.5 MB.

The anonymous 58 MB is the waiting set the flush rule keeps (journal.c:1133: a game flushes at 600 s or 256 KiB; everything at 64 MiB): about 450k waiting moves at sat2, each a cb_game_move of 32 B in the game's move array (journal.c:223, 478-484) plus its log delta of about 55 B in its own malloc (dup_bytes at journal.c:1557, owned by the overlay at 500-502) with musl's 16 B header and size-class rounding to a 80 B slot: about 115 B per waiting move, 52 MB.
The rest: 192 residents of about 3 KB (TableResident 2 KB plus the row's grown state and roster buffers), the current 8 MiB segment's dirty pages (file-backed, in RssFile), the loop's and the workers' sections (217 KB each), SQLite's page cache (the flush's connection), the 128 spare 32 KiB socket buffers (server.c:78-79, up to 4 MB), and the binary's 7.5 MB of text (RssFile).
At production pace the waiting set is moves of the last 10 minutes: kilobytes.
Saturation on a 512 MB box with the OS and SQLite is where the cap matters, and 64 MiB of logs plus the move arrays (not counted in w_bytes: move_bytes counts sizeof(cb_game_move) + len, so they are) is a quarter of the box.

### 5.2 The next-fixes WIP (d3c4efc44), reviewed

What it does: one grown log buffer per game (jmove {seq, off, len, clear} of 24 B, the deltas back to back in jgame.logs), a 16 MiB total cap and a 2000-move per-game cap, the finalize scratch kept per thread (s->fin, 3.25 MB once), and every viewer's cast from one walk (evwire_serialize_holes, table_casts, table_cast_seat) with tests in foolish/c/tests/tests.c and unit_journal.c.

| piece | verdict | why |
|---|---|---|
| one log buffer per game | take it | per waiting move 24 B + 55 B = 79 B against 115: -31% of the waiting set, and one malloc and one free per move gone (the dup at journal.c:1557 and the free at ov_flushed 1173), which NANO_MODEL.md 1.4 prices at 0.25 us each warm and 2 us when another thread allocates. ov_flushed's memmove of the tail is bounded by the per-game cap. moves_for_flush copies the whole log buffer per flush (256 KiB at most per game): fine, amortized over thousands of moves |
| 16 MiB total cap | take it only with E3 | without bounded transactions a smaller cap means more frequent whole-set flushes, each a 200k-row transaction holding flush_mu for seconds, which is the regression's second cause (4.2 item 2). With E3 the cap is purely an RSS bound: 16 MiB of counted bytes is about 200k waiting moves, RSS about 30 MB at saturation |
| 2000 waiting moves per game | drop it | it bounds the same thing twice (game_bytes already does, and E3 bounds the transaction). One rule, one owner: flush_due by age and bytes; the transaction budget in flush_locked. A second cap is the "double band-aid" the house rules forbid |
| finalize scratch per thread | take it | 3 mallocs of 3.25 MB per game end were an mmap, 800 first-touch faults at 0.97 us and a munmap each time: about 0.8 ms per game end (NANO_MODEL.md 1.2). One 3.25 MB allocation per compute thread, resident forever: the right trade on 512 MB. Also removes the second table_load (the extras take the Table const) |
| every viewer's cast from one walk | superseded by CAST_DELTAS.md (same branch): a delta cast of the move list and the viewer's draws, rebuilt by the kernel's client into the same evwire sequence, sends neither the boards nor the spans; the two are alternatives, and if the delta lands the holes model must not | hum8 builds 9 casts of the same events per move (section 1.1 prices a cast at about 1 us at 2 seats and 2 to 3 us at 8); the holes model would be -14 us per move at hum8 and -2 at hum2 for one walk plus a copy and a patch per seat; the delta is -2.5 to -4.5 us WARM at eight and 90 to 95 percent fewer cast bytes, which at human pace is the only part that touches the box (section 7: the segment, not the bytes, is the per-hearer cost, so neither changes the softirq). Whichever lands, the test is byte identity against table_cast per seat over 1000 random games with deals, lobby events and roster trailers, and a mutation that drops the awaiting byte |
| variable-length roster in R records (planned next) | later | disk and backup only (a 2-seat setup record 1.5 KB -> 0.5 KB); not a CPU or RSS item on the nano; roster.c's 1227 B invariant is load-bearing for the trailer readers, so the R record gets its own prefix encoding, not a roster format change |

Two more memory items the WIP does not have:

- The move arrays grow by doubling (journal.c:478-484) and never shrink; after a flush a game of 4000 moves keeps a 4096-entry array (98 KB per game at 24 B).
  ov_flushed should hand back the array when n_moves drops to 0 (as the WIP does for logs) and shrink it when under a quarter.
- The 128 spare 32 KiB socket buffers (server.c:78-79): up to 4 MB kept after a burst; SPARE_MAX_CAP of 8 KiB would keep the common frame sizes and let the rare large ones go.

## 6. io_uring on the nano: off

| scenario | epoll | io_uring | syscalls | latency p50 |
|---|---|---|---|---|
| hum2 | 83.3 us | 89.1 us (+7%) | 3.92 -> 1.97 | 0.333 -> 0.345 ms |
| sat2 | 47.4 | 48.4 (+2%), one run at 52 with 5.1 syscalls | 5.69 -> 4.41 | 5.27 -> 7.31 ms |
| hum8 | 124.4 | 129.4 (+4%) | 11.47 -> 1.94 | 0.366 -> 0.406 ms |
| sat8 | 56.0 | 52.6 (-6%) | 6.15 -> 1.93 | 5.17 -> 6.66 ms |

A syscall entry costs 0.11 us on this box and io_uring_enter 0.24 + 0.05 us per SQE (NANO_MODEL.md 1.5); a send's 3.5 to 11 us is TCP and ENA work that the ring does exactly as send(2) does, plus an SQE, a CQE, the deferred completion work of COOP_TASKRUN at the next enter, and the connection's wbuf hand-over (server.c:239-280).
Batching pays only when a pass has many sends (sat8: 5 per move, -6%); at human pace, which is production, a pass has two and the ring costs 5 us and 12 us of p50.
The arm64 syscall entry carries no mitigation worth the trip on Neoverse N1 (spec_*.txt: Spectre v2 mitigated in hardware, no retpoline-style cost), which was the open question the rig left.
Keep it behind CBASE_IO_URING=0; revisit if a product ever has 8 hearers per move at saturation as its normal state.
The AF_XDP write-up names io_uring batched sends and IRQ affinity as the levers to measure next; both have now been measured on the nano (this section and NANO_MODEL.md 2.8) and neither pays at human pace.

## 7. The per-viewer fan-out

Per hearer today: one envelope (state_put + roster trailer, about 0.7 us), one cast (evwire_serialize, 1 to 3 us), two frames encoded (nw_frame_encode into t_push, 0.2 us), two pub_deliver walks (0.6 us each: the strstr on the family pattern, the user bucket walk with strcmp, cb_server_wake's parked walk), two queue_frame copies (0.1 us), and one send (3.5 to 4 us marginal; 11 us when it is the pass's first).
About 4 us per hearer of user work plus 4 us of kernel; the kernel's is per socket and irreducible on TCP.
The AF_XDP spike prices the kernel's share exactly (NANO_MODEL.md 1.6): warm, 2.7 us in the task and 1.0 to 1.6 us of softirq per extra hearer, 3.7 all in; cold, at human pace, the six extra hearers of K=8 cost the box 70 us more than K=2 (188 against 118), about 12 us per hearer, three quarters of it softirq on the ACKs and TX completions that each hearer's segment brings back.
A user-space stack (lwIP on AF_XDP) halves the warm share (2.1 us per hearer) and cuts a third of the cold one, at the price of a wake per ACK and a loss recovery that dropped moves under the nano's bandwidth shaping: rejected (section 10).
So the per-hearer cost that matters on the box at human pace is the segment, not the bytes or the user work; the only levers are fewer sockets hearing a move (one segment per hearer per pass is already the coalescing) and fewer moves, neither of which is the server's to choose.

What can still be cut:

1. The cast once per move (the WIP's table_casts, section 5.2): -1 to 2.5 us per hearer.
2. The strstr per push (rt_local.c:801) and the content-type strstr (functions_http.c:78): 1.1 us per move today for three pushes; store `bool user` in cb_topic_family at definition (pushes.c:25-28) and compare the content type with memcmp of the known prefix.
3. One hub walk per hearer, not per push: cb_fanout_publish (fanout.c:107-132) publishes a user's view then its cast as two pushes; pub_deliver resolves the user's connections twice.
   Give cb_rt_publish a two-frame variant (realtime.h) that local_deliver resolves once and queue_frame's twice: -0.6 us per hearer.
4. The spectator cast when no room is watched: section_cast builds it always (table_ops.c:970) and pub_deliver finds no room (rt_local.c:754); the hub knows (room_find) and the loop can ask it before building: -1 us per move at hum2, where nobody watches.
5. The view for a connection that follows the game by casts: see section 8; it is the one change that is not server-only.
6. The frame straight into c->out: nw_frame_encode writes into the thread's t_push and queue_frame copies header and payload (server.c:571-578); with the loop as the one publisher the frame can be encoded into each hearer's out buffer directly.
   Saves one 300 to 600 B copy per push (15 ns): not worth its own step; it rides on 3.

## 8. The per-move VIEW push: a separate question, traced

Traced across every host (read-only, this tree):

- The web's on-screen game never consumes a per-move VIEW: foolish/c/src/netclient.c:1540-1546 holds back a VIEW for any game the client follows by casts (NW_SUB_GAME) and only arms a 1 s refetch if the view is newer than the board; the animation plan's end state is the CAST's own final board (foolish/web/src/contexts/AnimationContext.tsx:286-308 decodeCast -> read.final; useAnimationRun.ts:194 planFor with the page's held board), and anim_build_plan takes no VIEW.
  So the hypothesis "the VIEW is the animation queue's final expected state" is false for the web.
- The web's dashboard list of games NOT on screen is driven by the user-wide VIEW listener (foolish/web/src/contexts/ServerContext.tsx:276-312): hard for liveness of that list.
- The iOS app (FoolishNet) is a boards host: a seated player subscribes to NW_SUB_GAMES only (foolish/ios/FoolishNet/Stream.swift:23-25) and adopts each VIEW envelope (OnlineGame.swift:52-86); casts are turned into refetches whose answers arrive as views.
  Hard today; the owner has said the main iOS app is not a priority and is not shipped, and the iMessage app has no network at all (foolish/ios/project.yml:207-208) and never sees a push.
- Tests and bench that count VIEW frames: foolish/server/test/smoke.c:334, 684-706; smoke_matrix.c:378-383; fuzz/fz_sequence.c:330-379; unit_netclient.c:521-596, 897-901, 995-1017, 1328-1361; bench/roundtrip.c:87, bench/cpu.c:221.
- /v1/wait parks on the user id and is woken by any push to that key; the seat cast is user-keyed too (rt_local.c:736): soft.

The server-only change that is safe for every host: skip a game's VIEW for a connection that SUBscribed that game's room (rt_local.c do_sub:503-523 records it in c->watch and room_add; pub_deliver's PUB_USER walk at rt_local.c:741 can test watch_index(c, game_id) when the frame is a view), and keep sending it to the user's other connections (a dashboard tab, the iOS app, which never SUBs the room).
That matches what netclient.c:1540 already does on the client side, so no host changes behaviour; it loses only the 1 s lost-cast hint (the quiet timer at netclient.c:1437 and the next cast's gap still recover).
Saving: at hum2 both hearers are in the room, so 2 view frames per move (about 1 KB of TCP payload and 1.3 us of encode and walk) go; no send is saved because the view rides the cast's segment.
It needs: the frame kind to be visible to pub_deliver (pushes.c publish passes the NwFrame kind through cb_rt_to or a flag), the smoke, smoke_matrix, fz_sequence and roundtrip counts updated to expect views only on non-room connections, and a unit_netclient case that a room SUB stops the views.
Ranked low (section 9 step 9); the question of dropping views altogether for hosts that apply casts stays open and is the iOS app's to answer when it matters.

## 9. The plan, ranked

Each step ships alone and is gated by its tests; savings are per move of cycles on the nano unless they say per game end or latency.

| # | step | expected | edit points | tests (mutation target) | risk |
|---|---|---|---|---|---|
| 1 | E2 the valve reads thread CPU time and never routes moves to the pool | sat2 move max 550 to 777 ms -> about 90 ms; lost pushes 0; the regression's first cause gone | server.c:467-484, 508-510; rt_local.c:589-591; router.h inline_us per route | a 3 ms wall / 20 us CPU handler x3 in 1 s: tripped == 0 (time by wall again -> trips) | a handler genuinely slow on the loop (a deal's 8-seat products, about 1 ms) is bounded by inline_pass_us; none exists over 2 ms of CPU |
| 2 | E3 bounded flush transactions, flush_mu released between them | around() waits <= one transaction; the regression's second cause gone; the 16 MiB cap becomes safe | journal.c:1138 snapshot_take, 1227 flush_locked | 10 games x 5000 moves: every flush call <= 2000 moves, longest flush_mu hold < 1 transaction (remove the budget -> fails) | a flush pass takes more wall time in total (many transactions); at saturation only |
| 3 | WIP: one log buffer per game, finalize scratch per thread, casts from one walk; drop the game_moves cap; take the 16 MiB total cap after 2 | RSS at sat2 68 -> about 30 MB; -0.8 ms per game end; -14 us per move at hum8, -2 us at hum2 | d3c4efc44 as is, minus game_moves (journal.h:81, journal.c game_full) | the WIP's unit_journal and tests.c cases plus byte identity of every seat's cast over 1000 random games including deals and roster trailers (drop the awaiting byte in put_seat -> differs) | the hole model missing a viewer-dependent byte: caught by the identity test |
| 4 | E1 the rated end stays on the loop: ratings resident in the roster seat, the rating as a journal full record, around() only for deletes | per game end: -2 cross-vCPU hops (about 80 us), -1 cold start (45 us), -2 to 8 SQLite reads, -1 SQLite transaction; pushes leave with the reply | table_ops.c:758-788, 895, 1002; journal.c:671, 1524, 1132; roster.h RosterSeat; table.c:802 | rated end on the loop writes no SQLite before the answer, the flush writes the deltas (drop them from full_encode -> recovered rating differs); SIGKILL after a rated end recovers the leaderboard (skip the deltas in recover_full -> differs) | the leaderboard lags a rated end by one flush tick: the end marks the game due and kicks; a stale rating read by a join during that tick is the resident's, which E1 makes authoritative |
| 5 | WITHDRAWN as a production change (measured: NANO_MODEL.md 2.8). Replaced by a saturation knob: CBASE_LOOP_CPU / CBASE_BG_CPU, default unset; when set, the loop pins itself to one vCPU and every pool and the flusher to the other, and the pools are sized from config, never from sched_getaffinity | nothing at human pace (idle exits 6.9 -> 5.5 with no change in latency, box CPU or credits); at saturation +6% moves/s, -11% box CPU per move, push p95 18.5 -> 13.0 ms (C2); to test next: the same with the ENA interrupts on the background vCPU (E's shape for the softirq, C2's for the loop) | main_serve.c after the pools start (pthread_setaffinity_np on the loop and each worker); the pool sizing that reads the allowed CPUs | the smoke matrix with the knob set and unset; a sat2 on a nano asserting involuntary switches of the loop < 0.05 per move with the knob (unset -> 0.17) | irqbalance moves ENA's interrupts between vCPUs on its own; a layout that depends on their placement needs irqbalance off and the affinity written at boot, which is an image change; production gains nothing from any of it |
| 6 | E4 the flusher without a tick; the sweep and the bot scan only while something is live | the idle box wakes a vCPU 0 times a second instead of 2.1; nothing per move | journal.c:1280-1307, 486; server.c:895; foolish_app.c:118 | flusher wake count 0 over 5 s idle, 1 after a move plus game_age (restore the tick -> fails) | a flush owed to a game whose timer was lost: the kick on bytes and at close remain |
| 7 | Hot-path cuts on the loop: strstr per push and per request -> a flag and a memcmp; the seed re-hexed per commit -> the hex kept in the resident; cb_section_free's memsets -> only what table_init and table_adopt do not overwrite; one hub walk per hearer for view+cast; no spectator cast when no room watches; the loop waits with -1 when only the sweep is pending (a timerfd for the sweep) | about 5 to 6 us warm per move (1.1 strstr, 0.3 hex, 0.9 memset, 0.6 walk, 1.0 spectator cast, 1.1 hrtimer) of the 38 | rt_local.c:801; functions_http.c:78; table_ops.c:993-995, 310-314, 970; fanout.c:107-132 with realtime.h; loop.c:418-422 | each has a unit assertion: the hrtimer one counts hrtimer_start via a bpf probe in the bench, the others count calls through hooks (restore the call -> count rises) | none beyond review; each is a few lines |
| 8 | The journal record as a fixed layout instead of the range coder for the plain-move path | -2 us per move (cb_journal_batch_encode 2.8%); +5 to 8 B per move on disk (17 -> about 24 B; 59 B/move today at sat2 including setup records) | journal.c:117 cb_journal_batch_encode, 169 decode, replay_move.h | recovery of a mixed segment (coded and fixed records) replays identically; the existing SIGKILL test (flip the format byte -> refused) | a format bump of the segment (FORMAT at journal.c:30); the owner accepts breaking old on-disk data |
| 9 | Views skipped for connections in the game's room (section 8) | -1.3 us and -1 KB per move at hum2; no send saved | rt_local.c:741-752 with watch_index; pushes.c:48 passing the kind; the tests of section 8 | a connection with a room SUB gets casts and no views, one without gets both (drop the test -> views arrive) | the iOS seated player is not in the room and keeps its views; the 1 s lost-cast hint goes |
| 10 | Three threads instead of six: request pool of one as the tables thread, bot and job pools as one compute thread | no per-move change; per game end and per bot cycle one fewer thread to wake and no allocator contention from a second worker | main_serve.c pool sizes; provider_local.c job pool; bots.c; config CBASE_REQ_WORKERS default 1 | the existing smoke matrix at CBASE_REQ_WORKERS=1 and CBASE_BOT_WORKERS=1; a bot game with a human move during a drive still commits (the trylock path) | a long SQLite task (a flush transaction) delays a sign-in behind it by one bounded transaction (E3) |

What the plan does not touch and why: the two sends and the recv (37 us warm in the task, 12 us of it the kernel's own network work) are the TCP path the clients need; the 48 us of softirq and 18 us of hardirq the box pays per hum2 move are the kernel's cold receive path and no placement or user-space stack removes them without giving up Linux's TCP (section 10); the kernel apply (2.2 us) is the game; the journal's mapped write is already syscall-free; the per-viewer envelope and cast stay as the clients' wire format (section 8 is the only question there).
After steps 1 to 8 a warm 2-player move is about 30 us of cycles in the task (37 kernel - 1 hrtimer, 20 handler - 6 cuts - 2 coder, 4 plumbing) against 38; the human-pace move keeps its 30 to 50 us of cold start whatever the layout, so the server's own cost goes from 60 to 70 us to about 50 to 60 and the box's from 157 to about 145; the game end costs what a move costs; the box wakes only for packets; and RSS at saturation stays under 35 MB.

## 10. Two experiments since the first draft, and what they overturn

- ENA moderation and pinning (NANO_MODEL.md 2.8): step 5 withdrawn.
  Pinning the loop and the interrupts together removed the cross-vCPU wakes and 1.4 idle exits per move as predicted and changed nothing a user or the bill sees; the cold start is per parked vCPU and does not shrink when the RX softirq runs first on the same core.
  Turning RX moderation off adds interrupts and no latency.
  The one gain, C2 (the loop alone on its vCPU, the other threads on the other), is a saturation knob: step 5 is now that knob, default off.
  The first draft's error was in the counts and the figure of merit, not the primitives: it priced the removed wake at the 100 ms-idle row when the vCPUs wake every 5 to 10 ms, it expected a warming effect the data does not show, and it read the server's task time (which moves with where the softirq lands) where the box's CPU (which does not) is what matters.
- AF_XDP + lwIP (NANO_MODEL.md 1.6): no change to the plan's ranking, and a firm no to a user-space TCP.
  Box CPU per move at human pace falls 118 to 83 us (K=2) and 188 to 112 (K=8), the warm network cost halves, and none of it matters at production's pace; it adds a user-space wake per ACK (context switches 0.95 to 2.1 per move), keeps the cold start, and lost 89 and 16 moves under the nano's bandwidth shaping where kernel TCP (SACK, RACK-TLP, a 200 ms minimum RTO) lost none.
  What it settles for the model: the kernel's warm network cost is 12 us per move all in plus 3.7 us per extra hearer, two thirds in the task and one third in softirq, and the cold multiplier is 2.6x on the task's part and 8x on the softirq's.
- Softirq accounting (NANO_MODEL.md 2.7): 5 to 7 us of the server's hum2 CPU is other packets' NET_RX run at its syscall exits; the other 40 us of softirq per move is the box's and invisible to task-clock, schedstat and cycles alike.
  Comparisons of server CPU across layouts are therefore not comparisons of work; the box's CPU and the credits are.

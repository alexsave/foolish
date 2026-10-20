# Design: the move on the loop (verdict 1) and one owner of game state (verdicts 3 + 4)

Read-only design against branch `move-journal` at ce63ca7a8 (worktree /Users/alex/Dev/foolish/.claude/worktrees/move-journal).
Every file:line is an edit point an implementing agent can open directly.
Each migration step is shippable on its own and is gated by the tests named under it.

---

# Part 1. /v1/action fully on the loop thread

## 1.1 What is true today

- `/v1/action` already carries CB_ROUTE_INLINE (foolish/server/src/app/functions_http.c:458), and `GET /v1/games/*` too (foolish/server/src/app/v1_http.c:691).
- inline_ok (c_base/core/src/net/server.c:328-334) runs the handler on the loop only when: the budget is on, the valve is not tripped, the request pool is idle, AND this is the last ready event of the poll pass (`cb_loop_ready_left == 0`, c_base/core/src/net/loop.c:375).
- So at human pace over HTTP the handler itself usually already runs on the loop; at saturation (several ready fds per pass) and whenever a slow request is on the pool, every move goes to the pool.
- Even inline, the move still costs a hand-off: the 5 pushes of cb_fanout_publish (foolish/server/src/app/fanout.c:107-129) each go through local_deliver (c_base/providers/local/src/realtime/rt_local.c:644) which copies the frame into a pub and cb_loop_post()s it (loop.c:276), even when the caller IS the loop thread: 5 post mallocs, 5 post_lock pairs, 5 pub_get/pub_free lock pairs, one eventfd write (loop.c:272) and one eventfd read (loop.c:300) per move, and the pushes leave one poll pass later than the response (finish() sends the response at once, server.c:243; the pushes wait in the post queue for drain_posts).
- This is why eventfd_write is the top kernel symbol in all three Graviton human-pace profiles (ec2_all.txt M0_h192 2.24%, J2_h192 2.24%, J3_h192 2.80%) although the handler is inline.
- A socket request (SF_REQ) never runs inline: pump (rt_local.c:474) always cb_pool_submit()s the job, and job_done comes back by cb_loop_post (rt_local.c:437).
- The valve (server.c:355-363) trips on any inline request over inline_budget_us (2000, server.c:111) and turns inline off for inline_off_ms (5000): the game-ending commit runs finalize_game inside h_action (foolish/server/src/app/table_ops.c:714 -> :396: 2 MB + 1 MB + 256 KB mallocs, a full replay re-encode, a SQLite snapshot_put) and every non-plain commit (a deal, a lobby edit, a game end) writes SQLite synchronously inside the game lock (foolish/server/src/store/journal.c:875 around -> cb_rows_sql_flush), both well over 2 ms on the nano, so at saturation (87 game ends/s) the valve is permanently tripped and every move is pooled.
- The game lock (table_ops.c:461 cb_table_lock) is a blocking pthread mutex; the only other holder under serve is a bot drive's commit (foolish/server/src/app/bots.c:135-138), which holds it for one cb_section_commit (microseconds for a plain cycle, milliseconds when the cycle's commit is non-plain and goes to SQLite).
- The journal's append (journal.c:567) is a group commit: a writer that finds `j->writing` sleeps on j->cv (a futex) until the batch in flight is done; ce63ca7a8 removed the flush thread's wake per move and the writer wakes only when `j->waiters > 0`, so on the loop with no concurrent drive the path is already lock, write_batch, unlock, no sleep.

## 1.2 Target shape

- One poll pass handles a move end to end: recv, parse, handler (kernel apply, journal record written into the mapped segment), the response and the pushes appended into each socket's `out` buffer, and `flush_dirty` (server.c:532, registered as the loop's after hook at :772) writes each touched socket once.
- No eventfd, no futex, no post malloc, no pub copy on the move path; the pool and the posts remain for slow routes, HELLO, and bot drives.
- A socket request's REPLY and the actor's own VIEW and CAST pushes land in the same `out` buffer and leave in one send().
- The journal stays correct for two writers (loop and bot drives), but the loop never sleeps on it.

## 1.3 Edit points, step by step (each step ships alone)

### Step V1-A: pushes delivered on the loop without a post (smallest, highest value per line)

- rt_local.c:644 local_deliver: add at the top `if (cb_loop_on_thread(rt->ctx->loop)) { pub p = { .rt = rt, .scope = ..., .len = len, .frame = (uint8_t *)frame }; cb_strlcpy(p.key, ...); pub_on_loop_borrowed(loop, &p); return 0; }`.
- rt_local.c:597 pub_on_loop: split into `pub_deliver(local *L, pub *p)` (the body minus `pub_free(p)`) and keep pub_on_loop as `pub_deliver(...); pub_free(p);` so the borrowed path never frees a stack pub nor touches g_pub_mu.
- `cb_loop_on_thread` exists (loop.h:69); it compares `l->thread` set at loop.c:328.
- Invariant: the frame pointer passed to pub_deliver is only read before return (send_to -> cb_ws_send -> queue_frame copies it into c->out, server.c:419-425), so borrowing the caller's buffer is safe.
- Also rt_local.c:600 `cb_server_wake(L->ctx->server, p->key)` stays: parked long polls are woken the same way.
- Saves per move: 5 post mallocs/frees, 5 post_lock pairs, 5 pub_get/pub_free lock pairs, 1 eventfd write + 1 eventfd read, 1 extra poll pass; the pushes now reach `out` in the same pass as the response.
- Test: c_base realtime unit test that publishes from the loop thread and asserts the frame is in the socket's out buffer before the next poll pass (no post queued: expose `cb_loop_posts_pending(l)` for tests or count eventfd writes via a wrapper hook); mutation: remove the on-thread branch and the assertion must fail; also the existing push ordering tests (a game's pushes in commit order) must stay green.

### Step V1-B: defer the inline response to flush_dirty (coalescing, and the ack-after-commit order)

- server.c:219 finish(): replace the unconditional `flush(c)` at :243 with `mark_dirty(c)` when `cb_loop_on_thread` and the connection is not upgrading/closing; keep the direct flush for close_after and upgrade_after.
- mark_dirty (server.c:518) is already safe for HTTP connections (it only appends the conn to the dirty list); flush_dirty (server.c:532) then does one send() per touched socket at the end of the pass.
- Effect: the HTTP response and any pushes to the same socket (SF_REQ in step V1-C) leave in one send; and the response cannot leave before the journal batch of this pass has its magic byte written, because write_batch runs synchronously inside the handler (see V1-D) and flush_dirty runs after every handler of the pass.
- Risk: a response is delayed by at most the remainder of the pass (the other ready events, microseconds); `conn_try_parse` for a pipelined next request (server.c:243 tail) must still run: keep `if (c->in.len) conn_try_parse(c)` after mark_dirty.
- Test: an HTTP keep-alive client sends two moves back to back; both answered, order kept; mutation: drop the conn_try_parse call and the pipelined test hangs.

### Step V1-C: SF_REQ inline on the loop

- rt_local.c:474 pump: before `cb_pool_submit`, when `inline_ok`-equivalent holds (expose `bool cb_server_inline_ok(cb_server *s, const cb_http_req *req)` from server.c:328 and reuse the valve bookkeeping at :355-363), build the job as today, call `job_run_frame(j)` then `job_done(L->ctx->loop, j)` directly (the "no pool" branch at rt_local.c:479-481 already does exactly this, so the code exists).
- `job_run_frame` -> run_req (rt_local.c:316) runs the route's fn with `req.auth_user` set, so auth is a struct copy (c_base/core/src/auth/auth.c:246), and HELLO frames must stay on the pool (they read the store: cb_rt_deps_g.verify_token).
- The valve timing wraps the two calls like dispatch does (server.c:353-363).
- job_done -> reply_to -> send_to -> cb_ws_send -> mark_dirty, and the actor's own view + cast (V1-A) are appended to the same conn's `out` inside the handler: one send() for reply + 2 pushes.
- Recursion: job_done calls pump again for the next queued frame (rt_local.c:446); inline-in-pump then recurses once per queued frame; bound it by running at most one inline frame per pump call (set a local flag and `return` after job_done, letting the next poll pass pump again; INBOX_MAX already bounds the queue).
- Test: the stream unit test sends a REQ move over the socket and asserts a single WebSocket write carrying REPLY, VIEW, CAST (count send() calls via a test hook on flush, or assert `c->out` holds all three frames before flush_dirty); mutation: remove mark_dirty coalescing (flush at once in cb_ws_send) and the count becomes 3.

### Step V1-D: the journal never sleeps on the loop; one record per move is the batch

- journal.c:567 append(): add a loop-thread path: `if (cb_loop_on_thread(loop))` then `pthread_mutex_lock(&j->mu); while (j->writing) { unlock; sched_yield(); lock; }` is NOT acceptable as a design (a drive's non-plain commit is not in write_batch, only plain ones are; write_batch itself has no syscalls since d38b1b7fc, so a spin is bounded by microseconds, but a spin on a 1-vCPU box against a descheduled writer is a full timeslice).
- Instead make the loop the ONLY batch writer: `j_game_commit` on a pool thread (a bot drive, bots.c:137 -> cb_section_commit -> CB_ROWS_CALL game_commit) posts its jrec to the loop with cb_loop_post (one eventfd per bot cycle, not per human move) and waits on its own per-record condvar (a drive already sleeps between cycles); the loop's j_game_commit calls write_batch directly with `j->writing` becoming a loop-thread-only flag.
- Then `j->mu`/`j->cv` leave the move path; `writer_hold` (journal.c:594, the flush thread's segment rotation) becomes a posted request too: the flush thread posts `hold` to the loop, which writes any pending batch, sets `held`, and signals the flush thread; `writer_release` likewise, so the loop's write never waits on the flush thread (if `held` is set when a move arrives, the record is queued on a loop-owned list and written when release arrives, and its response/pushes are built then: see the completion note below).
- The record's row copy: j_game_commit at journal.c:914 (post-ce63) stores `r.row = *row` by pointer into the caller's section arena and ov_apply copies it into the game's grown buffers (row_assign); on the loop that is one copy, as now.
- Batch boundary: one record per move is one batch (8 B header + the coder's bytes, journal.c:111); grouping several moves of one pass into one batch saves only the header and one checksum per record and is not worth a completion queue; keep one batch per commit.
- Completion when a record must wait (writer held by the flush thread, rare): cb_section_commit (table_ops.c:702) cannot return "pending", so for V1-D keep it synchronous: the loop writes the record immediately when not held; when held, the loop falls back to the pool for THIS request (dispatch cannot know in advance, so cb_table_run returns a new CB_E_WOULD_BLOCK from a `no_wait` flag on the op, and dispatch's inline branch resubmits the job to the pool exactly like `resubmit` at server.c:251 does for parked requests).
- Invariant J1: a record's magic byte (journal.c:154) is written before any byte of its response or its pushes reaches a socket; with V1-B this holds because write_batch is inside the handler and sends happen in flush_dirty.
- Invariant J2: at most one thread writes a batch at a time: the loop; a pool thread never calls write_batch after V1-D.
- Test: unit_journal gains a two-writer test (a loop-thread commit and a drive-thread commit interleaved) asserting both recover and the loop thread never blocked (measure with a hook that records any cond wait on the loop thread); mutation: let the drive write directly and the "never waits" assertion must fail under a held writer.

### Step V1-E: the game lock on one thread

- table_ops.c:461 cb_table_lock is blocking; on the loop, taking it while a bot drive holds it for a non-plain commit (SQLite, ms) stalls every connection.
- Add `cb_game_lock *cb_table_trylock(const char *gid)` (same body, `pthread_mutex_trylock` at :473, `users--` and the free-when-zero path on failure).
- cb_table_run (table_ops.c:878) takes a `no_wait` flag through cb_table_op (table_ops.h:60-70); when set and the trylock fails it returns CB_E_WOULD_BLOCK without touching the section; h_action maps that to "resubmit to the pool", not to a refusal.
- dispatch (server.c:341): the inline branch sets a thread-local `cb_inline_no_wait = true` around cb_handle, and h_action reads it to set op.no_wait; when the handler returns with the response status set to a sentinel (res.park_ms-like field `res.resubmit = true`), dispatch builds the job and submits it to the pool (the request is still intact in g_req: copy it into the job as :370 does).
- Semantics on one thread: a game's commits remain serialized by the game lock across the loop and the drives; within the loop no two moves of one game interleave (one handler at a time); the fence (version == expected, journal.c:912) stays the correctness guarantee.
- Test: a test drive holds the lock (a hook sleeping inside cb_section_commit) while a move arrives; the move is answered correctly after the drive releases and the loop's other connections were served meanwhile (a /health probe answered within 1 ms); mutation: make trylock a lock and the probe's latency assertion fails.

### Step V1-F: the valve stops tripping on normal play

- Move finalize_game off the request path: table_ops.c:714 `if (c->ended) finalize_game(ctx, s, &kept);` becomes `cb_job_start(ctx, CB_FINALIZE_JOB, row->id)` (jobs.c:26; provider_local runs it on the job pool), and the job reloads the row with game_logs and runs today's body (finalize_game already reloads the table at :404-411 and reads the log at :402).
- Ordering rule that moves with it: "the replay is stored before the push that ends the game" (table_ops.c:712-713) becomes "the replay GET parks until the snapshot exists": v1_http's replay route uses the existing park mechanism (res.park_ms + park_key = the game id, server.c:285-306) and the finalize job calls cb_server_wake(game id) when the snapshot row is written; the clients already retry a 404 replay once (verify in netclient.c before relying on it; otherwise park).
- Non-plain commits (deal, lobby, end) still write SQLite inline under the game lock (journal.c:875 around); on the nano that is 1-5 ms, so: (a) raise inline_budget_us to 10 ms for `/v1/action` only (a per-route budget in cb_route, router.h:35-37) so a deal does not trip the valve, and (b) shorten inline_off_ms to 500 ms, or (c) make the valve count trips (3 in a second) rather than one; (a)+(c) recommended; the full fix is Part 2 step O-4 (every commit through the overlay, SQLite only at flush).
- Inline gate: drop the `cb_loop_ready_left(ctx->loop) != 0` rule (server.c:332) for routes flagged INLINE; replace it with a per-pass inline budget (e.g. 4 ms of inline handler time per poll pass, then the rest of the pass goes to the pool), which keeps other ready fds from starving under a burst while letting a pass with 3 ready moves run all three inline; keep the `cb_pool_idle` rule only for ordering across the SAME connection, which is already guaranteed by C_BUSY pausing reads (server.c:608-611), so it can go too; a pooled slow GET no longer forces every move to the pool.
- Test: a 60 s run with game ends asserts `cb_server_inline_stats` tripped == 0 and inline_ran == moves; mutation: put finalize back inline and tripped > 0.

### Step V1-G (later): the syscall floor and io_uring

- After V1-A..F a 2-human move over SF_REQ is: 1 recv + 1 send (actor: REPLY+VIEW+CAST) + 1 send (opponent: VIEW+CAST) = 3 syscalls, plus epoll_wait shared across the pass.
- io_uring would turn the pass's recvs and sends into one io_uring_enter; blockers: the poller abstraction (c_base/core/src/net/poller.c) is readiness-based with per-conn buffers freed by buf_idle (server.c:70) so sends need owned buffers until completion; Alpine/musl static build (c_base/bench/nano/build.sh:11) has no liburing (hand-rolled SQ/CQ over the raw syscalls); gain is ~2 syscall entries per move (~5 us on Graviton2), below everything above, so it is last.

## 1.4 Failure modes and their guards

- A slow inline handler freezes every socket: the per-route budget and the counting valve (V1-F); finalize and SQLite off the move path.
- A lock held by a drive: trylock + resubmit (V1-E).
- A journal writer held by the flush thread: WOULD_BLOCK + resubmit (V1-D); never a sleep on the loop.
- A response sent before its record landed: structurally impossible after V1-B/V1-D (write in handler, send in after hook); test it with the SIGKILL recovery test asserting every acknowledged move recovers (unit_journal's existing SIGKILL test, extended to the inline path).
- Reentrancy: a handler on the loop calling cb_ws_send/cb_ws_close on the connection it is answering (rt_local's reply_to) is already how job_done runs today; the dirty list is walked by index and tolerates closes (server.c:536-543).
- Fairness: a burst of moves on one pass delays unrelated fds by the per-pass inline budget (4 ms worst case) which is below the p99 move latency measured on the nano (0.6-0.8 ms per move at human pace); acceptable, and the budget is a config knob.

## 1.5 Migration order (each step ships green)

1. V1-A (on-loop push delivery): ~30 lines in rt_local.c; measurable at once (eventfd_write disappears from the human-pace profile).
2. V1-B (deferred flush): ~10 lines in server.c finish.
3. V1-C (SF_REQ inline): ~40 lines in rt_local.c pump + a server.c export; the web and iOS clients that send moves over the socket get the one-send reply.
4. V1-F (finalize job, per-route budget, counting valve, gate change): ~80 lines across table_ops.c, server.c, router.h, jobs; ships with the "tripped == 0" test.
5. V1-E (trylock + resubmit): ~60 lines in table_ops.c, functions_http.c, server.c.
6. V1-D (single journal writer on the loop): ~120 lines in journal.c; needs the two-writer unit test.
7. V1-G when the rest is done and measured.

---

# Part 2. One owner of game state: the overlay holds the decoded game

## 2.1 What is true today

- Four encoded copies of the same PLAYING row exist per move: the request's (table_ops.c:58 row_copy out of g_cache), `kept` (:696), the journal overlay's (journal.c row_assign into jgame.row, ce63ca7a8), and g_cache's (table_ops.c:127 row_copy in cb_row_cache_note).
- Two caches decide residency: g_cache (CB_ROW_CACHE_CAP 256, LRU by tick, PLAYING only, app_internal.h:40, table_ops.c:81-147, scanned by strcmp at :91) and the overlay (OV_BUCKETS 1024 hashed, journal.c:280, evicted when idle 60 s with nothing waiting, :753; a non-plain commit `around()` ov_unlinks the game, :892).
- After a non-plain commit the overlay forgets the game, so the next move's cb_row_load hits g_cache; without g_cache that move would read SQLite; this is the one dependency between the two caches that the design must replace.
- Every move decodes the row (table.c:66 table_load: roster_decode with its inner canonical re-encode and 1227 B compare roster.c:131-154, state_blob_load_rated, table_seat_kinds) and encodes the new row (table.c:376 state_blob_put_rated_at, :384 roster_encode) although the same process produced it one move earlier.
- The kernel mutates the Game in place: a REJECTED apply (table.c:131) leaves the section's Game as awire_apply left it; today that is harmless because the section is thrown away and the row is reloaded.
- A Game is ~37 KB of which the `logs[MAX_LOGS]` array is 36 KB (foolish/c/src/game.h:223); the state blob carries no log (table.c:99-100), the session log lives in LOG# items and in the overlay's per-move log deltas (journal.c:333 cb_game_move.log); table_load leaves num_logs at what the blob sets (0) and `t->log_len = g->num_logs`.
- Readers of rows other than the move: cb_table_view and cb_table_views_of_user (table_ops.c:1120-1166, game_get and games_get_views), bots run_cycle and due_now (bots.c:63, :335 cb_row_load fresh), admin panels (admin_panels.c:106,:126), the flush (journal.c:734 cb_rows_sql_flush needs cb_game_row bytes), the backup (copies the SQLite file plus segments), recovery (journal.c:1231 recover_move -> cb_table_replay_move needs encoded rows as its base).

## 2.2 Target data structure

- In journal.c jgame (:205) add the resident decoded state and make the encoded row lazy:

```
typedef struct {
    uint8_t      head[GAME_HEAD_BYTES];   // the Game up to and including num_logs, logs[] excluded
    Roster       r;
    BoardClock   clock;
    GameRatings  ratings;
} cb_resident;                            // kernel-owned layout: declare it in foolish/c/src/table.h

typedef struct jgame {
    ...
    cb_resident  res;        // THE game: valid when res_valid
    bool         res_valid;
    cb_game_row  row;        // the encoded row: metadata always valid; state/roster bytes valid when row_fresh
    bool         row_fresh;
    size_t       state_cap, roster_cap;
    ...
} jgame;
```

- GAME_HEAD_BYTES = offsetof(Game, logs) (game.h:223 places logs last); table.h already has TABLE_SNAP_BYTES = offsetof(Game, num_logs) (table.h:155) for the snapshot slots; the resident needs num_logs too (set to 0 on adopt since the board carries no log), so a new constant beside it.
- Kernel entry points (C, in table.c, next to table_load :66 and table_commit_products :327):
  - `int table_adopt(Table *t, const cb_resident *r)`: memcpy head into t->g, `t->g->num_logs = 0`, `t->r = r->r`, `t->clock = r->clock`, `t->ratings = r->ratings`, the strategy_key per seat from table_seat_kinds (table.c:56) applied to t->g->players (as :83 does), scope_open, `t->log_len = 0`, `t->loaded = true`; no decode, no validation (the resident was validated when it was made).
  - `void table_resident(const Table *t, const TableCommit *c, cb_resident *out)`: memcpy head out of t->g, `out->r = t->r`, `out->clock = c->clock` (the clock commit_products computed, table.c:380), `out->ratings = t->ratings`.
  - `int table_resident_encode(const cb_resident *r, uint8_t *state, int *state_len, uint8_t *roster, int *roster_len)`: state_blob_put_rated_at + roster_encode from a resident, for the lazy row.
  - `int table_resident_from_row(cb_resident *out, const uint8_t *state, int n, const uint8_t *roster, int m)`: today's table_load into a scratch Table then table_resident, for the first load of a game from SQLite and for recovery.
- Size: head ~1.1 KB (Game minus logs) + Roster 1.3 KB + clock/ratings ~100 B = ~2.5 KB per resident; 4096 residents = 10 MB; the overlay gets a cap (OV_MAX_RESIDENTS 4096) with the idle eviction it already has (journal.c:753) plus an LRU pass when over the cap.

## 2.3 The move path after the change

- cb_row_load (table_ops.c:149) becomes `cb_game_acquire(ctx, id, cb_section *s, cb_game_meta *m)`: under ov_mu, find or create the jgame; if `!res_valid`, read SQLite (cb_rows_sql.game_get), `table_resident_from_row`, store; then `table_adopt(&s->t, &g->res)` (one 2.5 KB memcpy, the copy-on-apply) and copy the metadata (version, round_epoch, log_from, seed, status, needs_bots, created_at) into `m`; release ov_mu.
- Copy-on-apply: the section's Game is the working copy; a REJECTED/MOOT/STALE op (table.c:131, cb_table_run :936-947) touches nothing resident; only a committed op replaces `g->res` (under the game lock, in j_game_commit's ov_apply), so invariant R1 holds by construction.
- cb_section_commit (table_ops.c:598): table_commit_products still produces the envelopes, casts, log records and clock (those are the wire products); section_row (:539) fills a cb_game_row whose state/roster point into the arena as now (needed for the SQLite path of non-plain commits until step O-4); `kept` (:696) goes away: the journal takes the resident by value from the section (`table_resident(&s->t, &s->c, &rec.res)`, 2.5 KB memcpy into the jrec, then into the jgame by struct assignment under ov_mu; or write it straight into the jgame under ov_mu since the caller holds the game lock).
- j_game_commit (journal.c:896): the fence reads `g->row.version` as now (:905); ov_apply (:313) assigns `g->res = r->res; g->res_valid = true; g->row_fresh = false;` and updates the metadata fields of g->row (version, updated_at, round_epoch, log_from, needs_bots, status); no state/roster bytes are copied.
- ov_get (journal.c:821), used by every other reader (views, admin, bots until O-5, the flush): under ov_mu, if `!row_fresh` then `table_resident_encode` into the grown buffers (`grow_copy` exists post-ce63) and set row_fresh; then row_dup as now; encode-on-demand costs ~1 us and happens once per move only if someone reads (the flush reads once per flush, not per move).
- cb_row_cache_note/drop/clear (table_ops.c:111-147) and g_cache are deleted; `cb_app.store_forget` (ab53a9a8a, used by the admin's SQL write) now calls a new `cb_journal_forget(store, id)` that ov_unlinks the game, so an admin row write is seen by the next move (invariant R5).
- around() (journal.c:875): after cb_rows_sql_flush succeeds, do NOT ov_unlink; instead set `g->res` from the committing section (the caller passes the resident with the commit: add `const cb_resident *res` to cb_game_commit, rows_foolish.h:66), `g->sql_version = version`, `row_fresh = false`; the game stays resident across deals and lobby edits, which is what replaces g_cache's role; a deleted game (j_game_delete :937) does ov_unlink.
- A CB_E_CONFLICT from around or from the fence (journal.c:912) evicts the resident (`res_valid = false`) so the next load reads SQLite, exactly as cb_row_cache_drop did (table_ops.c:703).
- Recovery (journal.c:1231 recover_move -> table_ops.c:784 cb_table_replay_move) keeps working on encoded rows from SQLite (the recovered base is a SQLite row; the replay's result row is written back to the overlay as a resident via table_resident_from_row at the end of recovery, or left encoded with res_valid false and adopted lazily).

## 2.4 What disappears per move (2 humans, one event)

- Decodes: roster_decode (with its inner roster_encode + roster_validate + 1227 B bytes_eq), state_blob_load_rated, table_seat_kinds: 3 decodes and 2 validations gone.
- Encodes: state_blob_put_rated_at and roster_encode for the row (table.c:376, :384) gone in step O-3 (kernel flag `TABLE_PRODUCTS_NO_ROW` or a split entry point); the per-viewer envelopes and casts stay (they are the clients' wire format).
- Row copies: cache -> request (1.6 KB + 2-3 mallocs), kept (1.6 KB + 2-3 mallocs), cache note (1.6 KB + 2-3 mallocs + 3 frees) gone; the overlay's row_assign copy (1.4 KB memcpy, no malloc) becomes a 2.5 KB struct copy; the adopt is a 2.5 KB memcpy (replacing a 1.6 KB copy plus the decode).
- Mallocs: 7-9 malloc/free pairs of the ~42 per move gone (every one through musl's global lock), plus cb_game_row_free x3.
- Locks: g_cache_mu x2 gone; ov_mu stays x2.
- Scans: cache_find's 2 x 256 strcmps (table_ops.c:91, visible as strcmp 1.9% + cb_row_load 1.2% in J3_h192) gone; ov_slot is hashed.
- Compares: the 1227 B roster memcmp for journal eligibility (table_ops.c:619) becomes `!c->roster_changed` (TableCommit already carries it, table.c:349).
- Estimated saving: 5-7 us on the rig (10-14%), 15-20 us on Graviton2 at human pace (12-15% of 137), where malloc + musl locks are 13-14% of all samples and roster + strcmp another 5%.

## 2.5 Invariants

- R1: a game's resident changes only under the game lock, by a committed operation, and never by a refused one (copy-on-apply: the section adopts a copy).
- R2: `g->row.version` equals the acknowledged version and is the only fence the journal compares against (journal.c:905-912); SQLite's version is `sql_version <= row.version`.
- R3: when `row_fresh`, `encode(res) == row.state/roster` byte for byte; when `res_valid` and not fresh, the row's bytes are stale and must not be read (every reader goes through ov_get, which refreshes).
- R4: the flush writes rows produced by `table_resident_encode` of the resident at snapshot time (journal.c:680 snapshot_take calls ov_get's refresh under ov_mu before row_dup).
- R5: an admin SQL write (cb_store_admin_query) evicts the resident (store_forget -> cb_journal_forget), and a fence conflict evicts it.
- R6: residency is bounded (OV_MAX_RESIDENTS) and a game with waiting moves is never evicted (today's rule, journal.c:753).
- R7: one row-shaped owner per game in the process: the jgame; the section holds a working copy only while an operation runs; no other cache of rows exists (grep for row_copy/cb_row_cache must be empty after step O-1).

## 2.6 Failure modes

- A stale encoded row served to a GET: guarded by R3 and the "GET after move shows the new version" test.
- A resident kept after a conflict builds on a wrong base: the fence refuses it (journal.c:912) and eviction follows; test with two interleaved commits.
- Memory: a slow leak of residents under many short games: the cap and the idle sweep; a test creates 2 x OV_MAX_RESIDENTS games and asserts n_games <= cap.
- Recovery of a crash after step O-2: residents are process memory; recovery rebuilds from SQLite rows + segments as today, so nothing on disk changes format; the backup is unaffected.
- A kernel layout change (Game fields) silently changes GAME_HEAD_BYTES: it is only process memory, never persisted; a _Static_assert that `offsetof(Game, logs) > offsetof(Game, num_logs)` and that cb_resident is not written to disk anywhere (grep in tests).
- Bots run_cycle (bots.c:63) reads with the log via game_logs; it keeps using ov_get (encoded, refreshed) until step O-5 gives it table_adopt + the overlay's log deltas (journal.c logs_of :838 already concatenates them).

## 2.7 Test plan with mutation targets

- T1 (R1): a REJECTED move then `GET /v1/games/{id}`: board identical to before; mutation: apply on the resident in place (skip table_adopt's copy) -> the GET shows a mutated board.
- T2 (R3): a move then a GET on another connection shows version+1 and the new board; mutation: never clear `row_fresh` in ov_apply -> stale bytes served.
- T3 (R5): ab53a9a8a's unit_journal admin test ("the next move builds on the admin's row") stays green; mutation: drop cb_journal_forget -> fails.
- T4 (conflict eviction): two commits with the same expected_version (a drive and a move racing through a test hook); the loser gets CB_E_CONFLICT and the next load reads SQLite; mutation: keep the resident on conflict -> the next move's base is wrong and the fence refuses it twice.
- T5 (around keeps the resident): a deal then a move with SQLite reads counted (a cb_rows_sql hook): 0 reads for the move; mutation: ov_unlink in around -> 1 read.
- T6 (flush correctness): the existing SIGKILL recovery test and the three-backups restore drill (9799f8234) unchanged; mutation: encode the flush rows from a stale resident (skip the refresh) -> recovery replays onto the wrong base and is refused.
- T7 (cap): create 2 x cap games, assert `cb_journal_stats.games <= cap` and that games with waiting moves are all still resident.
- T8 (no second owner): a test greps the server sources for `row_copy(`, `cb_row_cache_` and fails if any remain (R7), or a link-time check that the symbols are gone.
- T9 (byte identity, step O-3): for 1000 random moves, `table_resident_encode` equals the bytes table_commit_products would have written (keep the row encode under a test flag to compare); mutation: drop the clock from the resident -> differs.

## 2.8 Migration order (each step shippable)

- O-1: overlay as the read cache; g_cache deleted. ov_get serves every PLAYING game; cb_row_load becomes "overlay first, SQLite on miss, insert on miss"; around() keeps the game resident (encoded, as today's row_assign); store_forget -> cb_journal_forget; the cap and LRU; tests T3, T4, T5, T7, T8. Removes 2 of the 4 row copies, 4-6 mallocs, the strcmp scans and g_cache_mu. Rows stay encoded; no kernel change.
- O-2: the resident decoded state. table_adopt / table_resident / table_resident_from_row / table_resident_encode in table.c; jgame gets cb_resident + res_valid/row_fresh; ov_apply stores the resident; ov_get encodes on demand; cb_section_load (table_ops.c:285) adopts; `kept` removed; tests T1, T2, T6, T9 (with the row encode still on). Removes the decode per move.
- O-3: drop the per-move row encode. commit_products split so the row's state/roster are not written for a journal-eligible commit (the arena then holds only logs, envelopes, casts); section_row points the row at the resident's lazily-encoded bytes only for non-plain commits; test T9 flips to compare on the SQLite path only.
- O-4: every commit through the overlay. around() no longer writes SQLite inline: a non-plain commit becomes a "full row" journal record (a new record kind carrying the encoded row and the log delta, written to the segment like a move) and the flush writes it; the deal, lobby edits and the game end then cost no SQLite on the request path, which is what finally lets Part 1's valve never trip; recovery replays full-row records by adopting them as the new base. This is the largest step and the last; it needs its own recovery tests (a crash between a deal's record and its flush).
- O-5: bots adopt the resident: run_cycle (bots.c:63) uses cb_game_acquire + the overlay's log deltas instead of game_get + game_logs, removing a SQLite read of LOG# items per bot cycle.

## 2.9 Interaction with Part 1

- Part 1 V1-D (the loop as the only journal writer) and O-1/O-2 are independent and can land in either order.
- O-4 is what makes Part 1's inline path safe for every commit kind; until then the per-route budget and the counting valve carry non-plain commits.
- With both done, a 2-human move is: recv, parse, adopt (2.5 KB memcpy), apply, products (envelopes + casts), one journal record into the mapped page, the resident replaced (2.5 KB), reply and pushes appended into two `out` buffers, two send()s; no malloc on the path except the per-push frame buffer (which can become the socket's `out` directly), no mutex but ov_mu, no wake-ups.

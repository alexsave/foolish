# The two parts the server owns: rule application and the move record, against a 500 ns target

Read-only design analysis of main at ebeaf94a6 (which is the profiled 4aa1c22e3 plus calibration files: no server or kernel code differs).
The move is a plain 2-player human move over the socket, inline on the loop: `POST /v1/action` -> h_action (foolish/server/src/app/functions_http.c:135) -> cb_table_run (foolish/server/src/app/table_ops.c:1144) -> table_act (foolish/c/src/table.c:108) -> cb_section_commit (table_ops.c:871) -> j_game_commit (foolish/server/src/store/journal.c:1515) -> cb_fanout_commit.
Costs come from two instruments and are labelled throughout:

- COLD: graviton/runs/prof2_hum2_NOW, `perf record -g` of hum2 on the t4g.nano, cycles sampled, scaled to the 83.3 us of cycles per move of the three hum2 NOW runs (208k cycles at 2.48 GHz).
  At human pace the loop's vCPU is parked between moves and comes back to cold caches (NANO_MODEL.md 2.3: IPC 0.31 against 0.78 on a c6g for the same 64k instructions), so a COLD number is mostly DRAM lines, not instructions.
- WARM: the nano model's primitive costs (NANO_MODEL.md 1.1 to 1.4) applied to the bytes and calls the code makes, which is the cycles-of-work figure the 500 ns target has to be read against.
  500 ns is 1,240 cycles at 2.48 GHz.

The section numbers below follow the brief: what runs today, the floor, the changes, the verdict, for each part.

## 0. The headline

| | COLD today (nano, hum2) | WARM today (model) | WARM after steps 3, 7, 8 and the changes below | WARM floor | 500 ns |
|---|---|---|---|---|---|
| Part 1a: the apply (table_act) | 2.2 us | 0.25 to 0.30 us | 0.25 us | 0.1 us | already under it, warm |
| Part 1b: the state in and the products out (acquire, adopt, products, casts, row, resident, free, lock) | 11.6 us | 2.6 us | 0.7 to 1.0 us | 0.4 to 0.5 us | not with today's wire products; the floor is about 1 us |
| Part 2: the record (j_game_commit) | 5.6 us | 1.2 us | 0.25 us | 0.1 us | yes, with step 8 plus two small cuts |

The cold numbers do not move with any of this: they are the cost of touching 80 to 120 cache lines from DRAM after the vCPU was parked (about 3 us for part 1, 0.5 to 1 us for part 2, with the memory-level parallelism a dependent walk allows).
That is OPTIMIZED_DESIGN.md step 5 (the loop pinned, ENA's interrupts steered), an ops change, and at production pace it is the whole story: a move of 1 us of work costs 45 us of cold start before it does anything.
Everything below is about the work.

## 1. Part 1: rule application

### 1.1 What runs today for one plain move

The order is the code's.
"x N" is how many times per move; bytes are the real struct sizes (Game 37,576 B of which the head before `logs` is 712 B; Table 1,512 B of which the Roster is 1,235 B; TableResident 2,024 B; TableCommit 144 B; cb_game_row about 200 B; cb_game_commit about 2 KB; cb_fanout about 1.2 KB; ReplayMove 76 B; cb_jmove 722 B).

| # | what runs | where | work | COLD us | WARM ns |
|---|---|---|---|---|---|
| 1 | cb_section_new: the thread's pooled 217 KB section | table_ops.c:265 | a TLS read and two flags | 0.5 (0.62% self: a cold line) | 5 |
| 2 | cb_table_trylock -> lock_entry: fnv1a32 of the id, g_locks_mu, a bucket walk with strcmp, a spare or calloc, pthread_mutex_trylock; cb_table_unlock -> lock_entry_put: g_locks_mu again, unlink | table_ops.c:674-750, 1165, 1288 | 1 hash of 12 B, 2 strcmp, 4 mutex ops (2 pairs + lock/unlock of the game's) | 0.6 (lock_entry 0.33% self + its share of __pthread_mutex_lock 0.40%) | 100 |
| 3 | game_acquire -> cb_journal_game_acquire: ov_mu, ov_slot (fnv1a32 + strcmp), cb_mono_ms, take_game: table_init (memset the 1,512 B Table), table_adopt (memcpy 712 B head, Roster struct copy 1,235 B, clock, ratings, scope_open with game_shown_good_mask), `*w->out = *row` (200 B), cb_strlcpy of the gid, section_seats (2 x cb_strncpy0 of 36 B) | table_ops.c:140-170, journal.c:1332-1345, table.c:840 | 1 hash, 1 strcmp, 1 mutex pair, 1 vdso clock, 3.7 KB of memset/memcpy, 2 id copies | 0.7 (memcpy and memset shares; the 2 KB adopt is 32 cold lines) | 230 |
| 4 | replay_move_raw: memset 76 B, awire_decode (3 B wire), memcpy 3 B; table_seat_of for it | table_ops.c:1185-1188, replay_move.c:64 | the wire decoded a first time | 0.1 | 15 |
| 5 | **table_act**: the guards, roster_seat_of (bytes_eq from the end over 2 ids), awire_decode (second time), scope_open (game_shown_good_mask), game_state_seed (FNV over about 20 board bytes + 4 per battle), awire_apply -> handle_attack or handle_cover (all_in_hand over a 6-card hand, has_dup, table_has_value or the exact battle match, count_uncovered, hand_remove_card, log_alloc, the awaiting flags), 1 to 3 table_snap (memcpy 708 B each) through the hook, game_settle_status (game_done), finalize | table.c:108-137, game.c:963, 1029, 1359, 150, 21 | about 1,100 instructions on a 2p board; 0.7 to 2.1 KB of snapshot memcpy | **2.16** (table_act 0.53% self, awire_decode 0.44%, handle_* 0.40%, game_state_seed 0.38%, game_shown_good_mask 0.36%) | **250 to 300** |
| 6 | cb_section_commit prologue: strlen(gid), cb_app_now (vdso), cb_bots_board_now, the NO_ROW decision | table_ops.c:876-887 | 1 clock, 1 strlen | 0.1 | 40 |
| 7 | table_commit_products_flags: memset TableCommit, table_bots_due (roster_bot_mask, game_done, bot_drive_eligible_mask: nothing more at 2 humans, see 1.3 item P5 for bot tables), game_shown_good_mask again, op_stream (snap_refs + evwire_walk once: find_log x2-3, 1 to 3 ev_emit), next_clock -> anim_stream_ms (beat_shape + beat_clock), **roster_validate** (utf8 scan of the title, 2 ids by id_ok, 2 names, brain_ok, the O(n^2) duplicate check), 1 to 3 log_record_put + le_put_u48 | table.c:340-421, 270, 295, roster.c:84 | the walk, the clock arithmetic, about 120 B of utf8 scanned | 2.3 (products self 0.48%, evwire_walk 0.68% shared with the casts, roster_validate 0.75% incl. id_ok 0.31%) | 340 (validate 150, walk 60, clock 50, the rest) |
| 8 | **envelope_put x3** (seat 0, seat 1, spectator): env_header_write, state_put (about 70 B at 2p: the deck and the other hand as memset runs), **roster_trailer_write_validated** (tw_u8 byte-at-a-time with a bounds compare per byte: game id, title, status, 2 names, 2 ids, ai bits, goods, has_ts; about 130 to 150 B at 2p) | table.c:213-231, 422-432, view.c:14, 354, roster.c:291 | 3 full state encodes, 3 identical trailers (about 420 B at about 2.5 cycles per byte) | 2.0 (envelope_put 2.38%: roster_trailer_write_validated 1.04% self, state_put_rules 0.83% shared with the casts) | 630 (210 each: header 10, state 60, trailer 140) |
| 9 | section_row: memset cb_game_row (200 B) and cb_game_commit (about 2 KB), cb_strlcpy, memcpy prev_humans/prev_bots (2 x 296 B), seat_id x2 into gp->humans | table_ops.c:798-870 | 2.5 KB zeroed and copied that a plain move's journal record never reads | 0.5 (section_row 0.30% self + memset share) | 120 |
| 10 | **section_cast x3** -> table_cast -> table_push -> evwire_serialize: snap_refs, evwire_walk (second, third, fourth walk of the same 1 to 3 snapshots), per event put_event (byte-wise put_u8) + put_snapshot (state_put), the final snapshot (state_put), the as3 flags byte; written into the arena past the products | table_ops.c:388-414, 955-965, table.c:307-335, evwire.c:250 | 3 walks, (events + 1) x 3 = 6 to 12 state_puts, about 750 B of output at about 2 cycles per byte through put_u8 | 2.9 (sink_packed 0.46% self, evwire_walk and state_put_rules shares, table_push, table_cast) | 660 (220 each) |
| 11 | the fan-out build: memset cb_fanout, seat_id x2, the joined/left diff (in_list + strcmp, 4 strcmp of 36 B), actor_env: none (h_action passes no_envelope) | table_ops.c:945-977 | 1.2 KB zeroed, 4 strcmp | 0.3 | 100 |
| 12 | hex_encode of the 32 B seed (64 chars), table_set_deal_seed -> bot_drive_seed_base (fnv1a32 over the 64 hex bytes), **table_resident**: memcpy 712 B head, state_canonical, Roster copy 1,235 B, clock, ratings (into a stack TableResident that the journal copies again, part 2 item 8) | table_ops.c:985-992, table.c:854, 883, bot_drive.c:194, str.c hex_encode | 2 KB copied, 64 B hashed, 64 chars formatted | 0.7 (hex_encode 0.35% self, game_state_seed-like fnv, memcpy share) | 240 |
| 13 | cb_section_free: memset the Game head (712 B), the host fields gid..arena (about 1.5 KB: old_humans, old_bots, mv[9] of 76 B), the arena prefix the products and casts wrote (about 2.5 KB) | table_ops.c:302-320 | about 4.7 KB of memset | 0.9 (memset 1.12% self) | 140 |
| | **Part 1 total** | | | **13.8** (cb_table_run 32.02% = 26.7 us, minus j_game_commit 6.71% and cb_fanout_publish 7.98%; the rows sum to 13.8, the difference is the under-0.30% leaves) | **2,900** |

Reconciliation: cb_table_run is 26.7 us COLD; the journal (part 2) is 5.6 and the fan-out publish 6.6 (OPTIMIZED_DESIGN.md 1.1, not this document's subject); the 14.5 us left is the table above within 5%.
The WARM figure of about 2.9 us is a model sum, not a measurement; the c6g's whole-move ratio (32.7 against 83.3 us) is 2.5x, and the user-side ratio is higher than the kernel TCP path's because the user side is almost all dependent pointer chases over cold structures, so 4 to 5x (13.8 against 2.9) is consistent.

What is decoded and encoded: the wire is decoded twice (items 4 and 5); the board is encoded 9 to 12 times (3 envelopes, 3 cast finals, 3 to 6 event snapshots) although only the hands differ per viewer; the roster is validated once (item 7; it was 2 + the human seats times per move before the `validated` flag) and written 3 times as an identical trailer (item 8); the event stream is walked 4 times (items 7 and 10); the row is NOT encoded (TABLE_PRODUCTS_NO_ROW, table_ops.c:886, lands on a plain move); the Game head crosses by memcpy 3 to 5 times (adopt, 1 to 3 snapshots, resident) and the Roster struct 3 times (adopt, resident, and the journal's copy in part 2), all cheap warm (22 to 36 ns each) and all cold lines at human pace.

What is recomputed that could be kept: the roster's validity (item 7), the trailer bytes (item 8), the seed's hex and its hash (item 12, step 7 of OPTIMIZED_DESIGN.md), the cb_game_commit's seat lists and the joined/left diff when the roster did not change (items 9 and 11), and for bot tables the whole legal menu per bot seat per commit (1.3 item P5).

### 1.2 The minimal work a move intrinsically needs, and its floor

A plain move must: resolve the actor to a seat; decode the wire; check the rules against the board; mutate the board and append its log records; and produce what the pushes carry.
What the pushes carry is the clients' wire format and is fixed by the web and iMessage readers (netclient.c NW_WS_CAST, AnimationContext decodeCast -> read.final): per hearer a CAST holding the event stream masked for that viewer plus that viewer's final board, and per hearer a VIEW envelope (header, that viewer's board, the roster trailer) that the on-screen web client holds back for a game it follows by casts (OPTIMIZED_DESIGN.md section 8).

| intrinsic step | WARM ns | why that much |
|---|---|---|
| seat lookup + wire decode | 15 | 2 ids compared from the end, 3 bytes decoded |
| the apply (validate + mutate + log + 1 to 3 snapshots) | 100 to 150 | about 600 to 900 instructions on a 2p board; the snapshots 22 ns each |
| the event stream derived once | 60 | one evwire_walk over 1 to 3 snapshots |
| the viewer-independent bytes written once (event skeleton, spectator final board, roster trailer): about 450 B | 40 | at memcpy-grade writing (0.07 cycles per byte); 10x that through a byte-wise writer |
| per hearer: copy the skeleton and patch the hand spans (2 hearers) | 2 x 30 | memcpy 300 B + 2 patches |
| per hearer: the VIEW envelope, if a hearer wants one (header + board + trailer memcpy) | 2 x 70 | the board is a second encode of the hands; the trailer a memcpy |
| the resident updated (copy-on-apply back) | 60 | 2 KB copied, or a pointer swap |
| **floor, 2 humans, views kept** | **about 480** | 1,200 cycles |
| **floor, views not built for room sockets** | **about 340** | 850 cycles |

So the floor of part 1 is right at the 500 ns line with today's wire products, and under it only when the VIEW is not built for a hearer that follows the game by casts (step 9 of OPTIMIZED_DESIGN.md, plus a products flag so it is not built, 1.3 item P4b).
Nothing in the floor is the apply: the apply is 100 to 150 ns and already runs in 250 to 300 ns.
The floor is the output bytes: about 1.2 KB of cast and envelope per 2-player move, which at today's byte-wise writers costs 2,500 to 3,000 cycles on its own and at memcpy grade about 100.

### 1.3 The changes, ranked by cycles saved per line

Server-only unless marked KERNEL; a KERNEL change is in foolish/c/src and is linked by the web and iMessage wasm too, so it must be byte-identical on the wire (the tests named exist or are one case away).
Overlaps with the steps another agent is implementing now are marked and not re-proposed.

| rank | change | saves WARM (COLD) | lines | risk | overlap |
|---|---|---|---|---|---|
| P4 | **No spectator envelope on a move.** table_commit_products_flags builds the spectator envelope every commit (table.c:426-431); the server reads it only as actor_env for an unseated actor (table_ops.c:979-984), which /v1/action never has (no_envelope, functions_http.c:152) and the fan-out never publishes it (fanout.c:107-132 publishes seats' envelopes and casts only). A TABLE_PRODUCTS_NO_SPECTATOR_VIEW flag set beside NO_ROW. KERNEL flag, server use; the wasm hosts never pass it | 210 ns (0.7 us) | 6 | none: the span stays empty, the server's only other reader is arena_note's bound (table_ops.c:292), and the kernel tests that read c.spectator (tests.c:10714, 14076) run with the default flags | - |
| P1 | **Validate the roster once per resident, not per commit.** table_adopt marks the Table's roster validated (it was validated by the load or the commit that made the resident); table_commit_products_flags runs roster_validate (table.c:376) only when `!validated || t->roster_changed`. KERNEL | 150 ns (0.6 us) | 10 | a host that edits t->r without a roster_* call: none exist (grep); the lobby edits set roster_changed | - |
| P2 | **Write the roster trailer once per commit and memcpy it into each envelope.** The trailer (roster.c:291) is viewer-independent; envelope_put writes it per viewer through tw_u8 (2 compares + a store per byte, about 200 B each). Write it once into the arena, then memcpy. KERNEL, wire-identical; the existing envelope byte-identity tests cover it | 280 ns (0.6 us) | 15 | none | - |
| P20 | **No cb_game_commit seat lists on a plain move.** section_row (table_ops.c:798) zeroes 2 KB and fills humans/bots/prev_humans/prev_bots and seat ids that j_game_commit never reads for a plain record (journal.c:1515-1556 reads row, moves, resident, log_add, expected_version, log_clear); the joined/left diff (table_ops.c:966-974) is 4 strcmp that `!c->roster_changed` answers. Fill them only when the commit is not plain | 150 ns (0.5 us) | 15 | the non-plain path must still fill them: keep one function with a `plain` flag, tested by the existing full-record journal tests | - |
| P16 | **One clock read per move.** cb_app_now (table_ops.c:878) plus cb_mono_ms in cb_journal_game_acquire (journal.c:1339) and in ov_apply (journal.c:485): 3 vdso reads at 31 ns; the loop already stamps the pass (cb_loop_now). Pass it down | 60 ns (0.2 us of the 0.86% vdso) | 8 | the journal's touched_ms/first_ms become pass-granular (ms already) | - |
| P5 | **Bot tables: ask "is there any legal move", not "the whole menu".** table_bots_due (table.c:147) -> bot_drive_eligible_mask (bot_drive.c:95) -> seat_can_act (:88) -> calculate_legal_moves (legal.c:459) for every bot seat on every commit, only to test `lm->n > 0`; legal.c:236-290's attack subset recursion is ~2^n in the matching cards. A `legal_any(g, seat)` that returns at the first legal attack card, cover pair, or pickup/good condition is O(hand x battles). KERNEL; the answer must equal `legal_moves_into(...) > 0` over random boards (a tests.c case, mutation: drop the cover branch) | 1 to 5 us per move in a game with a bot seat; 0 at hum2 | 40 | none beyond the equality proof; bot_drive's own menu for the drive is untouched | - |
| P22 | **Block-checked writers instead of byte-checked.** evwire's put_u8 (evwire.c:19) and roster's tw_u8 (roster.c:271) test the cap on every byte; put_event and put_snapshot know their length up front (put_snapshot already reserves EVW_SNAP_MAX). Reserve once per event and per trailer, then plain stores. KERNEL, byte-identical | 400 to 600 ns (about 1 us) on 1.2 KB of output; after P2 and P4 about 300 ns | 30 | none with the byte-identity tests (tests.c cast and envelope cases) | - |
| P13 | **The jgame pointer rides the section.** The move hashes and strcmp's the game id into the overlay three times (acquire journal.c:1336, the fence :1527, ov_apply :467) under three ov_mu pairs; the section acquired the game under its lock and could carry `jgame *` (with a `held` count that ov_evict respects, journal.c:1206) so the fence is a field read and ov_apply a pointer deref | 100 ns (0.5 us) | 25 | eviction while held: the count; a conflict still evicts | - |
| P15 | **The resident out by swap, not by two copies.** table_resident writes a stack TableResident (table_ops.c:989, 2 KB) that game_set copies again (journal.c:401). Give the section a TableResident slot, write it there, and swap it with the jgame's | 60 ns (0.4 us) | 20 | jgame.res becomes a pointer; recovery's ov_apply path keeps a copy | - |
| P12 | **The game lock as the resident's owner bit on the loop path.** lock_entry's hash, bucket walk, two g_locks_mu pairs and the trylock (table_ops.c:674-750) guard against one other holder, a bot drive on the pool; the loop could test an `owner` field on the jgame under the ov_mu it already takes, and the pool path keep the mutex | 80 ns (0.5 us) | 30 | two lock disciplines for one resource: only if P13 lands first, so the owner bit and the resident have one home | - |
| - | hex_encode of the seed per commit; cb_section_free's memsets; the spectator cast when no room watches; one walk for every viewer's cast | 160 + 90 + 50 + 400 ns (0.3 + 0.9 + 1.0 + 2.0 us) | - | - | **steps 7, 7, 7 and 3: being implemented, not re-proposed** |

Not worth a change: game_state_seed per move (30 ns warm; it must precede the apply because a refill draws from it), the double awire_decode (10 ns), the Game head memcpys (22 ns each), the cb_fanout memset (35 ns), strlen of the gid x5 (40 ns).

Not server-only and not proposed: dropping the final board from the CAST (the web's animation plan reads it as its end state, AnimationContext.tsx:286-308) or sending deltas instead of whole boards in the envelope (the iOS boards host adopts envelopes whole, OnlineGame.swift:52-86).

After steps 3, 7, 8 and P1, P2, P4, P13, P15, P16, P20, P22, part 1 WARM is about 1.0 us with both hearers' envelopes built, and about 0.7 us when the envelope is not built for a hearer in the game's room (step 9 plus a products mask for the seats that want one); the apply itself stays at about 0.25 us.

### 1.4 Verdict on 500 ns for part 1

- The apply (table_act) is 250 to 300 ns of work today; it is under 500 ns warm already, and nothing in it is worth touching.
  Its 2.2 us COLD is 15 to 20 DRAM lines (the Game head, the Roster, the handler's code, a snapshot slot), which only a warm vCPU fixes.
- The apply plus the deltas the pushes need (part 1 as the brief defines it) does not reach 500 ns with today's wire products: two casts with final boards plus two envelopes with trailers is about 1.2 KB of viewer-specific output, and the realistic floor for producing it is about 1 us warm (2,500 cycles) after every cut above, 0.7 us with envelopes unbuilt for room sockets.
  The 500 ns line needs the VIEW gone for cast followers (a protocol-visible rule the clients already honour, OPTIMIZED_DESIGN.md section 8) AND the casts from one walk AND memcpy-grade writers; that lands at about 0.5 to 0.6 us.
- At production pace (every vCPU parked between moves) the cold start is 2 to 3 us for this part whatever the code does; step 5 of OPTIMIZED_DESIGN.md is the only lever on it.

## 2. Part 2: recording the move

### 2.1 What runs today for one plain move

j_game_commit (journal.c:1515) on the loop, 6.71% COLD = 5.6 us, of which loop_write 4.82% = 4.0 us and inside it write_batch 4.63% = 3.9 us and cb_journal_batch_encode 2.80% = 2.3 us.

| # | what runs | where | work | COLD us | WARM ns |
|---|---|---|---|---|---|
| 1 | the fence: atomic epoch load; ov_mu lock; ov_slot (fnv1a32_str of the 12-char id + strcmp); read g->row.version; unlock; compare with expected_version | journal.c:1525-1540, 315 | 1 hash, 1 strcmp, 1 mutex pair | 0.4 | 60 |
| 2 | the jrec built: memset of about 1 KB (cb_jmove is 722 B: 9 ReplayMove of 76 B), cb_strlcpy of the id, memcpy of 1 ReplayMove (76 B), `r.row = *row` (200 B), **dup_bytes of the log delta** (malloc 20 to 60 B + memcpy) | journal.c:1541-1556 | 1 malloc (freed by the flush later: a live small slot), 1.3 KB of memset/memcpy | 0.9 (j_game_commit 0.56% self, malloc 0.77% shared) | 320 (malloc 250) |
| 3 | loop_append: loop_writes test (cb_loop_running atomic + loop_gone atomic); j->mu lock; loop_take; unlock | journal.c:956-968, 918 | 1 mutex pair, 2 atomics | 0.15 | 35 |
| 4 | loop_write: batch_need (a walk of 1 record), rotates (5 field reads); seg_mu never taken on the common path | journal.c:898-916, 758-778 | branches | 0.15 | 15 |
| 5 | write_batch prologue: the epoch loop, batch_need again, rotates again, the segment header only on a fresh segment | journal.c:780-805 | branches | 0.15 | 15 |
| 6 | `ms[k++] = r->m`: the cb_jmove copied into j->ms (722 B) | journal.c:820 | memcpy 722 B | 0.1 | 22 |
| 7 | **cb_journal_batch_encode**: replay_coder_begin_encode (memset 24 B, the 936 KB per-thread CoderScratch's first lines), varint n, varint zigzag(rel), **slot_find** (fnv1a32_str + strcmp against the segment's slot table), uniform(N+1), uniform(2) follows, varint(ms - base), uniform(2) bots, replay_move_code: uniform(8) seat, varint(1025) menu_n = 2 choices, code_wire: varint(3) + 3 x uniform(256); about 12 recorded choices of 12 B each; **coder_finish**: 12 x (bn_divmod over 2 to 3 limbs with a 64-bit udiv per limb, bn_mul_add) building a 10 B integer in reverse; bn_to_bytes_be; batch_sum = fnv1a32 over 4 + 10 B (a 4-cycle dependent multiply per byte); put_u16 x2, put_u32; the release store of the magic byte | journal.c:117-166, 113, replay.c:248-272, 108-135 | about 12 coder records, 36 udiv/umul limb steps, a 14 B FNV, 18 B stored into the mapping | 2.3 (cb_journal_batch_encode 0.33% self, coder_finish 0.53%, code_varint 0.45%, slot_find 0.39%, and the coder scratch's cold lines) | 550 (finish 350, choices 60, slot 60, fnv 25, rest) |
| 8 | write_batch epilogue: seg_len release store; 4 to 5 atomic_fetch_add (batches, writes, loop_writes, records, bytes); ov_mu lock; **ov_apply**: ov_slot again (hash + strcmp), the moves array (realloc amortized), cb_mono_ms, `game_set`: **`g->res = *r->res` (2,024 B struct copy)** + row_set_columns (2 x 200 B), the moves[] entry (32 B), the bytes accounting; the due test; unlock; kick_flusher only when due | journal.c:845-877, 466-510, 399-410 | 5 atomics, 1 hash, 1 strcmp, 1 mutex pair, 1 vdso clock, 2.5 KB copied | 1.2 (ov_apply, memcpy share, atomics) | 220 (copy 60, atomics 40, clock 31, hash 40, mutex 17, rest) |
| 9 | loop_give: j->mu lock, done flags, unlock, no broadcast (no waiters); free(NULL) | journal.c:932-954, 1554 | 1 mutex pair | 0.15 | 20 |
| 10 | the mapped page: a first write to each new 4 KiB page of the fallocated, MAP_SHARED segment faults it in (0.97 us per page, NANO_MODEL.md 1.2); at about 18 B per batch that is one fault per 220 moves | journal.c:520-553, 806 | amortized | - | 5 (and a 1 us tail once per 220 moves) |
| | **Part 2 total** | | | **5.6** | **about 1,260** |

Reconciliation: the rows sum to 5.5 us COLD against j_game_commit's 6.71% = 5.6 (j_game_commit outside loop_write is 1.6 us: items 1, 2, 3 and 9; loop_write 4.82% = 4.0 us holds items 4 to 8, of which cb_journal_batch_encode is 2.3); WARM about 1.26 us, of which the range coder is 45% and the malloc 20%.
The brief's "about 1 us or less today" is this WARM figure; on the nano at human pace the same work costs 5.6 us of cold lines (the 722 B cb_jmove and the 1 KB jrec on the stack, the 2 KB resident in the jgame, the coder's scratch 936 KB away from its Bn, the slot table, the mapped page).

What is encoded: one record through a bignum range coder whose cost is 12 limb-divisions for an 18 B result; the payload it saves against a fixed layout is about 6 to 8 B per move (17 B against 24 B).
What is copied: the ReplayMove into a cb_jmove (722 B struct) into j->ms (722 B again); the log delta into a malloc; the row columns twice; the resident 2 KB (after part 1 copied it once already into the stack TableResident).
What is recomputed: the game id's hash and slot three times (part 1 item 3, items 1 and 8 here) plus the segment slot table's hash (item 7).

### 2.2 The minimal work a record intrinsically needs, and its floor

Durability is fixed: the ack follows the record's commit marker in the mapped page (the release store of the magic byte, journal.c:160; a process crash keeps the page cache).
Recovery needs (journal.c:1899 recover_move -> cb_table_replay_move table_ops.c:1055): the game, the base version, the time, the seat, the awire bytes, and the bots flag; the flush needs (journal.c:1138 snapshot_take, cb_game_move) the version, the log delta and the clear bit, which the overlay holds in memory.

| intrinsic step | WARM ns | why |
|---|---|---|
| the record's fields written into the mapping: game slot, base version, ms delta, seat, kind, wire length, 3 to 7 wire bytes: 24 B fixed | 5 | plain stores |
| a checksum over 20 B | 5 with CRC32C (Neoverse N1: crc32cx, 8 B per 1 to 2 cycles); 40 with FNV-1a byte-wise | torn-write detection; the magic byte alone is not enough against a partial page write |
| the commit marker: one release store | 1 | |
| the overlay: the version, the log delta appended (20 to 60 B memcpy into a grown buffer), one 16 B move entry | 15 | |
| the resident replaced: a pointer swap, or a 2 KB copy | 2 or 60 | copy-on-apply needs the new board to replace the old one; the section can own the buffer it goes in |
| **floor** | **about 30 to 90** | 75 to 220 cycles |

500 ns leaves a 5x margin over this floor warm.
Cold, the record touches the mapping's current page (1 line), the jgame (the 2 KB resident is 32 lines written: a write-allocate miss each, about 0.5 us with MLP 8; a pointer swap touches 1), the moves array (1) and the slots (2): about 0.15 us with the swap, 0.7 us with the copy.

### 2.3 The changes, ranked by cycles saved per line

| rank | change | saves WARM (COLD) | lines | risk | overlap |
|---|---|---|---|---|---|
| J8 | **Fixed-layout record instead of the range coder** | 500 ns (2.3 us) | - | a FORMAT bump of the segment; accepted | **step 8: being implemented.** Requirements the numbers above assume of its shape: (a) built from the section's ReplayMove or awire bytes straight into the mapping, no 722 B cb_jmove and no 1 KB jrec memset (item 2 and 6: 60 ns more); (b) CRC32C, not the byte-wise FNV (35 ns); (c) the game named by a slot cached on the jgame (J6 below), not found by hash per record |
| J1 | **No malloc for the log delta** | 250 ns (0.4 us) | - | - | **step 3: being implemented** (one grown log buffer per game). Requirement: the record borrows c->log_add from the section arena, which outlives the append (cb_section_free is after cb_section_commit returns), so ov_apply copies straight from the arena and dup_bytes at journal.c:1553 goes |
| J3 | **Plain counters for the loop's stats.** 5 atomic_fetch_add per write (journal.c:865-869) at 7.6 ns each, plus the seg_len release store, on a path with one writer; relaxed stores (the stats reader is already racy by design) | 35 ns (0.1 us) | 5 | none | - |
| J6 | **The slot cached on the jgame.** slot_find (journal.c:61) hashes and strcmp's the id against the segment's slot table per record; the jgame can hold `(seg_seq, slot)` and skip it while the segment is the same | 60 ns (0.3 us) | 10 | a rotation invalidates it: compare seg_seq | folds into step 8's shape |
| P13 | **The jgame pointer rides the section** (part 1 rank P13): the fence (item 1) and ov_apply's lookup (item 8) become field reads; two ov_mu pairs of three stay one | 100 ns (0.5 us) | 25 (shared with part 1) | the held count | - |
| P15 | **The resident out by swap** (part 1 rank P15): the 2 KB copy in game_set (item 8) becomes a pointer swap | 60 ns (0.4 us, 32 cold lines) | 20 (shared) | jgame.res a pointer | - |
| J2 | **No j->mu on the loop path when the queue is empty.** loop_take and loop_give (items 3 and 9) take j->mu twice to find an empty queue and set `writing`; an atomic `head` check skips both locks when nothing is queued, and the pool path (append, journal.c:986) keeps the mutex and its wait | 35 ns (0.2 us) | 20 | `writing` is read by other threads under j->mu: make it atomic too, or keep one pair and drop the other | - |
| J5 | **MAP_POPULATE on the segment made ahead** (seg_make, journal.c:543, which runs on the flush thread) so the loop never takes the first-touch fault | 5 ns amortized; removes a 1 us tail once per 220 moves | 1 | 8 MiB of page cache touched per segment up front: the segment is fallocated already, so the pages are allocated anyway | - |
| J4 | **CRC32C instead of FNV-1a for the record checksum** | 35 ns (0.1 us) | 10 | arm64-only instruction: the wasm never writes a journal, and a portable fallback is the same 10 lines | folds into step 8 |

After step 8 (with its requirements), step 3 (with J1) and J3, J6, P13, P15, J2: part 2 WARM is about 0.25 us (620 cycles); step 8 and step 3 alone bring it to about 0.45 us.

### 2.4 Verdict on 500 ns for part 2

Yes, warm.
Step 8 plus the dup removal in step 3 land under 500 ns on their own (about 450 ns); J3, J6, P13 and P15 take it to about 250 ns, and the floor is 30 to 90 ns.
Cold, the record is about 0.7 us with the resident copied and 0.15 us with it swapped (P15): the swap is the one change here that moves the cold number, because it turns 32 written lines into one.

## 3. Interactions and what is server-only

- Steps 3 (casts from one walk, one log buffer, finalize scratch), 7 (hot-path cuts) and 8 (fixed record) are being implemented now and are referenced, not re-proposed; two requirements on step 8's shape (no cb_jmove copy, CRC32C, slot cached) and one on step 3 (the record borrows the arena's log delta) are stated above because the floor depends on them.
- Steps 1 and 2 (the valve, bounded flush transactions, branch nano-valve-flush) do not touch these two parts.
- KERNEL changes (shared with the web and iMessage wasm): P1, P2, P4 (a flag with the old default), P5, P22.
  Each is byte-identical on the wire and gated by the existing tests.c byte-identity cases (casts, envelopes, trailers) plus one new equality test for P5.
  P5 changes no answer and no wire; it changes an exponential to a linear step on bot tables.
- Server-only: P12, P13, P15, P16, P20, J2, J3, J4, J5, J6.
- On-disk: only step 8's FORMAT bump (accepted); nothing here changes the segment layout further.
- Durability: unchanged; every variant keeps the record's commit marker as a release store before any byte of the answer or a push reaches a socket (the sends are in flush_dirty after the handler).

## 4. The order to do them in

1. Let steps 3, 7, 8 land (they carry 4.9 us COLD of these two parts' 19.4).
2. P4, P1, P2, P20, P16, J3 (about 60 lines, about 1 us WARM and 2.7 us COLD together, each a few lines with an existing test).
3. P22 (the writers), P13 + P15 + J6 together (one owner for the jgame pointer, the resident slot and the segment slot), then J2 and P12 only if P13 landed.
4. P5 whenever a bot table's move is measured: it is the largest single item on that path and nothing else here touches it.
5. Step 9 (views skipped for room sockets) with a products mask so they are not built either: it is the one change that takes part 1 from about 1 us to about 0.7 us, and it is protocol-visible only in that a client already ignoring the VIEW stops receiving it.

# Queue batch notes

Shared by the batch's items. The final agent writes the owed tests from this file and deletes it before the PR.

## 1. History list stops sending full replays

What changed:
- NW_HISTORY rows (netwire NwHistoryRow) no longer carry moves, extras or the player ids.
  A row is now: str8 id, u48 created, u8 n_players, u8 seat, u8 rank, u8 fool, u32 duration_s.
  255 means none for seat and fool, 0xffffffff none for the duration, rank 0 "no summary".
  The id is the replay's (snapshot) id.
- The summary choice: the pointer copy (SNAP#<id> PLAYER#<user>) now holds the summary as numbers: n_players, seat, rank, fool, dur.
  No names (the list shows "#1 #2 ..." for the seats; the viewer is bold, the fool has its icon).
  It is cut once at game end in table_ops.c snap_summary (replay_summary_v6 + the sum of the extras' gaps) and written by cb_rows_snapshot_put.
  GET /v1/history is therefore ONE Query of the pointers (cb_snapshot_query.summary_only), no BatchGetItem of the replays.
  Nothing new to redact on account delete: a pointer holds no name.
- GET /v1/replay/<id> accepts the replay id as well as the game id (cb_snapshot_query.ref), since a deleted game leaves a replay with no game id.
  The web history row is opened by tap: replay(id), then router.push of the link built from the moves and extras.
- DynamoDB shape: pointer items gain the number attributes n_players, seat, rank, fool, dur. No key or index change. Older pointers lack them and read as rank 0 (the web skips them).
- Files: foolish/c/src/netwire.[ch], netclient.[ch], foolish/tools/structgen/specs/net_layout.args (NcId and the players count are gone),
  foolish/server/src/store/rows_foolish.[ch], foolish/server/src/app/{v1_http,table_ops}.c, foolish/server/test/lib_app_store_mem.c (ref + seat/rank),
  foolish/server/test/smoke_matrix.c (replay_names fetches each replay), foolish/sdk/ts/net/net_client.ts, foolish/src/common/backend.ts,
  foolish/src/components/{MatchHistory.tsx,matchHistoryRow.ts}.

Tests to fix (broken by this change, not run or fixed now; smoke_matrix.c replay_names and lib_app_store_mem.c were already adapted and compile, but were not run):

Owed tests (nothing new was written; these still reference the old row and will not compile or pass):
- foolish/server/test/unit_netwire.c, unit_netclient.c, unit_leftovers.c (hr = nc_row), unit_v1.c (hr), smoke.c, fuzz/fz_netwire.c, fz_netclient.c (moves_off check), c_base/fuzz/seeds.c: move to the new row fields.
- unit: nw_put_history/nw_get_history round trip of the new row, and its bounds (n_players > MAX_PLAYERS, id too long).
- unit (rows_foolish / mem store): snapshot_put with sum.has writes pointer attrs n_players/seat/rank/fool/dur; snapshots_query summary_only returns them with no moves/extras and does one Query (no item_batch_get); rank 1 = first out, n = the fool; a pre-summary pointer reads has=false, rank 0.
- unit: cb_snapshot_query.ref matches the replay id and the game id, and nothing else; GET /v1/replay/<replay id> for a replay whose game was deleted returns 200.
- unit: table_ops snap_summary: fool, elimination order, duration = rounded sum of gaps, -1 with no times; unreadable code leaves has=false.
- smoke: GET /v1/history carries no moves or extras; its row id opens GET /v1/replay/<id>; a deleted account's name appears in no history body.
- web: historyEntry (matchHistoryRow.ts) maps rank 0 / seat 255 to null, fool 255 to -1, duration 0xffffffff to null; MatchHistory tap fetches the replay and navigates.
- Mutation-check each.

## 8. Store fixes found by the stage 8 test work

Commits: efd0e78e8 (cherry-pick -x of a84301973 from origin/stage8-owed-tests: cb_iwrite_check / cb_ikey_check / cb_iquery_check in c_base/core/src/store/items.c are called by both backends; no conflict) and the cb_rows_user_import fix below.

cb_rows_user_import (c_base/core/src/store/rows_auth.c) replaced the whole profile and wrote no session_gen, so a re-import reset it to 0 and sessions ended by a global sign-out passed the generation check again.
Now it reads the existing profile first.
When the stored session_gen is >= the imported one it keeps the stored session_gen and kept_session; a higher imported value wins.
Why max: an import must never lower a generation (that revives ended sessions), but an export taken after more sign-outs should still raise it.
It is read then write, not atomic, which is fine for the one-shot cbase-import run on a quiet store.
acct_writes now also writes session_gen (when nonzero) and kept_session (when set), so a fresh import carries them; profile[] grew from 8 to 10 attributes.

Owed tests:
- Pull the stage8-owed-tests branch's tests that cover a84301973: 2bf3960ce (core/test/conformance/store.{c,h}, unit_conf_store_local.c, unit_conf_store_dynamo.c; WIP, green, not mutation-checked; the dynamo runner skips without CBASE_STORE=dynamo and is not yet on DYNAMO_UNITS) and the unit_core.c / unit_jobs.c additions (b8c2aaf1c, ab60c0390). Mutation-check them against the three bugs: SQLite cond_holds read at n_cond 5, the 1024-byte pk dropped by DynamoDB batch get and the key walk, and keys-only queries answering every attribute.
- unit (rows_auth): import over an existing user keeps session_gen and kept_session; import with a higher session_gen takes it; import of a new user writes the imported session_gen; end_sessions then import leaves the old sessions dead. Mutation-check by writing session_gen 0 again.

## 9. One shared wasm libm, with sincos; fast-math leftovers removed

shared/c/wasm/libm.c gained sincos and sincosf (one quad() reduction, both series, quadrant swap/negate), declared in shared/c/wasm/include/math.h.
Bit-identical to separate sin()/cos(): 0 mismatches over 100001 angles, float and double (throwaway program, not kept).
shared/c/sincos.h gives sc_sincosf / sc_sincos: sincosf on wasm, sinf + cosf elsewhere (macOS and glibc have no portable sincos).
uttt sites switched: uttt_pen.c hachure rotation, ellipse points, join arc fan; uttt_draw.c tutorial dashed ring; uttt_anim.c spring.
uttt_pen.c round caps use a 5-entry static table (cos/sin of pi*j/6, zero calls); pen_w computes sinf(pi t) once and shares it.
uttt_draw.c: powf(x, .75) is sqrtf(x) * sqrtf(sqrtf(x)) (rough_for, mro_for, mark_geom); the constant powf pair near the logo is two literals.
chuiniu: cn_roll.c and cn_geom.c lost their private sin_k/cos_k/quad copies (59 lines removed, 11 added).
chuiniu/c/src/cn_libm.c includes shared/c/wasm/libm.c under cn_libm_ names (natively the platform has sin/cos, so every name is renamed); it is in the Makefile SRC (native) and ROLL_WASM_SRC (wasm).
cn_roll_test (golden) and cn_geom_test pass unchanged, so the series is bit-identical to the copies it replaced.

Outputs that may change:
- uttt drawing (wasm only): powf(x,.75) vs sqrt identity differs by float rounding; round-cap angles are exact now (cos(pi/2) was -4.4e-8, now 0); the logo roughness/bowing literals are rounded to float.
  Native/iOS keep the platform sinf/cosf/powf for the non-.75 calls but also take the sqrt identity and the cap table, so their drawing goldens can move by a ulp too.
- chuiniu: none intended (golden passes).

Workarounds removed (fast-math is gone from foolish/c CFLAGS and c_base KERNEL_CFLAGS since #385):
- hand_layout.c pragmas (float_control, fp contract) and the `product` volatile helper are deleted; table.c record_seconds lost its volatiles.
- Not every build had -ffp-contract=off: ASAN_CFLAGS, COV_FLAGS, IOS_CFLAGS (device/simulator slices and the ioshost smoke build) and TABLE_HOST_CFLAGS did not, and clang on arm64 contracts by default.
  Added the flag to those four (one owner: the flag). Checked on arm64 by compiling hand_layout.c to asm under each flag set: 0 fused ops with the flag, 13 without (so the flag is load-bearing).
- wasm: strict IEEE, no fused multiply-add instruction, nothing needed.
- Stale -ffast-math comments rewritten in hand_layout.c/.h, table.c, replay_extras.c, fm_exp.c/.h, fulminate_strategy.c, tests.c, fm_exp_native_bits.c.

Owed tests:
- libm: a test of sincos/sincosf against sin/cos (bitwise) and sincos on the quadrant edges, NaN and inf; mutation-check by swapping the case 1 and case 3 outputs.
- uttt: a drawing test that the cap table equals cos/sin(pi*j/6) within 1e-6, and pow75 against powf within 2 ulp (mutation: change .25 root to .5).
- foolish: run ios-goldens, ios-smoke and the xcframework build once (IOS_CFLAGS changed), plus tests-asan and the coverage build; confirm structgen layout hashes are unchanged (they are fed IOS_CFLAGS and TABLE_HOST_CFLAGS).
- hand_layout: a native test that fails if contraction is on (compile hand_layout.c with -ffp-contract=on in a test lane and compare to the Swift golden bits) so a new build tree without the flag is caught.

Tests to fix:
- chuiniu docs/UI.html embeds cn_roll.wasm: `make docs-roll-check` will report stale until `make docs-roll` is run (cn_libm.c is linked in).
- uttt drawing goldens (wasm and native) may move by a ulp; regenerate after reviewing.

Fixes to make:
- foolish/rust/Makefile and README still say the production flags are -O3 -ffast-math -flto; they are -ffp-contract=off -fno-math-errno now, and the rust POC bench builds with the old flags.
- docs mentioning -ffast-math as production flags (foolish/docs/NATIVE_SERVER.md, RUST_VS_C_KERNEL.md, c/MAKE_UNMAKE.md and other tool scripts under foolish/c/tools) are stale.
- uttt/c/Makefile line 10 table is a historical measurement, fine, but check it.

## Item: uttt settled-frame cache (uw_frame)

- Only the web page calls uw_frame (uttt/c/wasm/uttt_web.c, via uttt/web/lib/kernel.ts); the iOS app composes its own frames. No host code changed.
- uw_frame is a pure function of (cur position, look byte, UtttFrame from uttt_motion_at). The board is 0..1 normalised, so there is no viewport size, device scale, theme, seed or wall clock in the drawing; the pen seed is the look byte.
- Cache key: the whole UtttFrame (memcmp, all 4-byte fields, no padding: wash rect + rgba, mark_t, fall_t, line_t, outline, outline_t, outline_a, running...) plus a valid flag that uw_load, uw_seek and uw_motion clear (those are the only places cur, look or the motion change).
  A frame with any animation value in flight differs from the previous one, so it rebuilds; a settled or paused page returns the kept display list (dl, pt, poly are untouched statics).
- Overlay split NOT done: uttt_draw_board takes mark_t/fall_t/meta_t as one call and builds board, marks and win line into one list, so a clean split needs the draw kernel to emit the settled part and the moving part separately.

Owed tests:
- A cached frame is byte-identical to a rebuilt one: for a few replay codes and plies, call uw_frame(t) twice and compare uw_points/uw_poly_* against a build with the cache disabled (cache_ok = 0 before each call).
- Every key input invalidates: after uw_frame, each of uw_seek, uw_motion, uw_load (other code) must change the list; a now_ms mid-ink must differ from the settled frame; mutation: drop the cache_ok = 0 in uw_seek and the seek test must fail, and make the memcmp ignore the last field and a late-outline frame must fail.
- Measured per-frame cost before and after (not run: batch limit): time uw_frame over 1000 settled calls at the end of a 40 ply game in node, cache on vs off; expect the repeat call to be a ~100 byte memcmp.

Tests to fix:
- none known; the display list is unchanged by construction.

Fixes to make:
- Finer cache: split uttt_draw_board into settled board and animated overlay (last mark, big mark, win line, outline) so the in-flight frames only rebuild the moving part.

## Fixes to make (collected; done at the end of the list, before the test pass)

- History names restore (item 1 regression, UI): the web history list shows `#1 #2 ...` instead of player names.
  Plan: /v1/history does its Query of pointers, then ONE BatchGetItem of the main SNAP#<id>/SNAP items projecting only what names need (extras; SQLite: the column subset); names go in the history row; pointers stay name-free so account delete still redacts only the main item.
  Keep the replay fetch-on-tap.
  Old pointers (written before item 1, no summary, rank 0) are SKIPPED by historyEntry, so every old replay vanishes from the list: in the same batch read, compute the summary from the main item with snap_summary (table_ops.c, moves + extras projected), so old and new replays both show, with names.
  No backfill unless the per-read cost matters.
  The list must render identically to before b092fe333: names, bold viewer seat, fool icon.
  Note the netwire row shape change when done.
- cb_rows_user_import is not atomic (read then write); fine for cbase-import, noted in case another caller appears.

## Item: hand-rolled security follow-ups (admin token, SigV4 wipes, product fuzz)

- Admin token: c_base/core/src/admin/admin.c const_eq now hashes both strings with SHA-256 and compares the digests with CRYPTO_memcmp, so neither content nor length shows in the timing (the old loop ran strlen on the secret and branched on the length).
- SigV4 (c_base/providers/aws/src/aws/sigv4.c): OPENSSL_cleanse on derive()'s k0/kd/kr/ks, hmac()'s k/ipad/opad/inner (also on the early return), the signing key kk in cb_sigv4_debug, and the cache entry that a new key replaces (it held the old secret and key).
- Fuzz targets: shared/fuzz/ (own Makefile, `make -C shared/fuzz build/<t>`, `make seeds`): msg_whole (shared envelope reader + link text + seat tag, under pickemup's spec), pickemup, tallybones, chuiniu (message bytes + link text, decode/encode/re-decode property), uttt (utm_*, utb_* header/link readers), foolish_wire (msg_decode, msg_chain_key, replay_decode, awire_decode). Seeds in shared/fuzz/corpus/<t> come from the products' real encoders (fz_seeds in each target). All six link and ran 2000 corpus-only runs clean. Nightly: .github/workflows/product_fuzz_nightly.yml (10 min per target). Werewolf skipped (paused).
- Gaps: foolish_wire has no started-game seed (needs a played Game and msg_seal; add one from msg_wire_test's play_game); utb_msg_decode is only fed a blank board so it stops at the CRC; the re-decode abort in the whole-product targets is a design property (pk_msg.h says a host cannot emit a payload it would refuse) and a nightly failure there is a real finding.

Owed tests (files written, NOT yet mutation-checked):
- shared/c/deal_rng_openssl_test.c and shared/c/sha256_openssl_test.c (native, link -lcrypto; both pass, 1600 and 4522 assertions, under 1 s). Mutate: drop the feed-forward add in deal_rng_block; change one padding constant in sha256.c. Neither is wired into a Makefile or CI lane yet (they need OpenSSL on the link line).
- c_base/providers/aws/test/unit_sigv4.c hmac_long_key_boundaries (secrets of 1, 59, 60, 61, 62, 100, 127 chars against libcrypto HMAC, the kept key re-signed). Not run (c_base build is over the 5 s budget). Mutate: hash the key at klen >= 64 instead of > 64 in hmac().
- Wipe tests are not possible from outside (the buffers are locals); check once by reading the asm or with a valgrind-style stack scan if wanted.

## Item: near-duplicate families 7-18 (dup_audit.md)

Lifts, all header-only in shared/ unless noted. Every change is compile-checked only (nothing over 5 s was run).

### Family 7, FNV-1a (shared/c/fnv.h, fnv_test.c)
- Cherry-picked the parked WIP (origin/claude/dedup-fnv, now deleted) and fixed the c_base paths (c_base/core/src/rate_limits.c, foolish/server/src/app/table_ops.c).
- ONE 64-bit basis: FNV64_OFFSET = 14695981039346656037. The truncated 1469598103934665603 (the WIP's FNV64_OFFSET_SHORT) is gone from C, from foolish/rust/rs/src/lib.rs and from foolish/ios/FoolishTests/EngineGoldenTests.swift.
- Also moved: netclient.c quiet_jitter (32-bit fold), tests/fm_exp_digest.h (already the right basis, now through fnv1a64_step).
- Goldens / pins that change (the 64-bit users that were on the short basis):
  - foolish/ios/Fixtures/goldens.json (dealHash, seat0LegalHash, playHash) - regenerate with foolish/c/ios/ios_goldens.c; EngineGoldenTests.swift compares against them with the new basis.
  - foolish/c/tests/tests.c (the four digests built from FNV64_OFFSET near lines 11555, 11640, 11806, 11824) and foolish/c/tests/replay_overlay_test.c (overlay digest): expected values.
  - foolish/c/src/main_eval.c GAME_SIG outputs (any recorded GAME_SIG baseline).
  - chuiniu: cn.c game hash and chuiniu/c/tests/cn_scene_frame.h (frame digest) pins in cn_test / scene tests.
  - tallybones: tb.c game hash pins; uttt: uttt_bots.c cm hash pins in uttt_bots_test.
  - foolish/rust bench_common.h FNV_INIT and rs/src/lib.rs: the bench checksum the Rust port must reproduce changes (C and Rust moved together).
  - c_base/core/src/rate_limits.c hash is internal (slot index), nothing pinned.
- Unchanged by construction: every 32-bit fold (roster hash, game seed, texture pins, table stripe).

### Family 8, endian bytes (shared/c/le_bytes.h + new be_bytes.h)
- le_bytes.h gained le_put_u64/le_get_u64; be_bytes.h is the big-endian half (be_put/get_u16/u32). le_bytes_test.c covers both (60k assertions pass).
- Switched: foolish msg_wire.c (6 helpers), replay_extras.c, shared deal_rng.c, chuiniu cn_tex.c / cn_texgen.c / cn_tex_test.c / cn_icon.c (PNG chunks, TrueType), uttt uttt_big_msg.c and uttt_msg.c (big-endian headers), shared mqtt.c, c_base items.c.
- Left on purpose: werewolf (paused), pk_msg/tb_msg/cn_msg (already msg_whole), deal_rng_openssl_test.c (an independent oracle).
- ctl_wire.c no longer exists (native server retired).

### Family 9, card labels (foolish/c/cardtext/card_text.h)
- Added card_rank_text(value), card_suit_letter(suit) and CARD_SUIT_LETTER (freestanding, so wasm may use them). datagen reads only the two named tables, so the generated TS is unchanged.
- main_replay.c was already fixed (the drift in the audit is gone). Switched: main_analyse, main_winprob, main_eval, octogen_strategy, oracle_answer_test, msg_wire_test (3 sites).
- Not changed: tests.c:9794 (a parser table, lowercase), tools/endgame_retro/{find_crawl,trace_path,verify_crawl,alphabeta_probe}.c (their own 5..13 "T"-style or six-up tables and no -Isrc to cardtext), foolyard trace.c.
- TS/Swift/Python label tables that are NOT generated and stay: src/components/OracleOverlay.tsx, SegmentDisplay.tsx, src/oracle/explain.ts, src/oracle/types.ts, scripts/decode_replay.mjs, e2e/helpers/table_mem.ts, table_play.ts, e2e/ui_animation_trace.test.ts, ios/FoolishMessages/MessagesViewController.swift, c/tools/winprob/winprob_bin.py. (sdk/ts/gen/card_rank.ts and card_suit.ts are the generated ones; moving the web onto them is a host edit.)

### Family 13, UTF-8 (shared/c/text_util/utf8.h, header-only)
- text_utf8_next (strict decoder) and text_utf8_scan(flags, cps, utf16). TEXT_UTF8_NO_CONTROLS is the one policy bit (display names); code point and UTF-16 unit counts are outputs.
- Policies kept explicit: roster.c (controls allowed, UTF-16 units for the 50-unit title cap, ASCII fast path kept), msg_seat_tag (no controls, code points), aws_json body check (controls allowed), c_base names.c (decoder only). unfurl_http.c is a lenient replacement walker (different policy) and stays; text_util.c text_next_cp is the other lenient one.
- c_base auth.c password length counts continuation bytes without validating; left (it is a password, not a trust boundary for text).
- text_util_test.c got 15 utf8 checks (pass). Owed: mutation check (break the surrogate range, the overlong minimum, the C1 range).

### Family 15, clocks (shared/c/clock.h, host-only)
- host_mono_ns/us/ms/s, host_real_ms, host_thread_cpu_ms, host_process_cpu_ns/s, host_sleep_ms, host_deadline_after_ms. c_base clock.c (cb_now_ms, cb_mono_ms, cb_sleep_ms) is now a thin wrapper.
- 25 sites switched (c_base bench/test/aws, foolish analyse/winprob/main_* /tests/rust/server bench+test, chuiniu bench, tallybones tb_solve, uttt_ladder). Left: inline timespec subtraction in main_eval/main_showcase/alphabeta_probe/solve/uttt_analyse/cn_arena (elapsed-time pairs; a host_elapsed helper would be new API), uttt tests that use clock() on purpose.
- c_base util/str and util/uuid: already built on shared/c/hex; nothing duplicated to lift (cb_url_decode has no twin).

### Family 11, easing (shared/c/ease.h, ease_test.c)
- ease_cubic_bezier is uttt's solver verbatim (uttt output unchanged). pickemup pk_ease now calls it: pickemup's old solver had a 1e-4 tolerance, so pk_ease values move by up to ~1e-4. REGENERATE: pickemup beats / frame goldens that hash or pin pk_ease output (pk_beats_test expectations, docs/MOTION_REPORT.md dump if pinned).
- Left: roleMotion.ts solver (TS, host), Swift literals, cn_beats / tb_beats / cn_cam named eases (no solver in them).

### Family 10, test harness (shared/c/test/check.h)
- check.h gained OK(cond, "what"), CHECK_PRINT_CAP (default 5 per test; suites with no TEST() names raise it so every failure prints, as their private OK did) and CHECK_ZERO_IS_FAIL (c_base's "zero checks is a failure" rule as an option).
- 17 suites dropped their private counters and OK macros: shared collapse/msg_lobby_roster/msg_seat_tag/msg_stage/msg_whole/text_util/stats/wire_check tests, uttt msg/anim/ink/big/big_msg tests, pickemup/chuiniu/tallybones api smokes. Output changed: failures go to stderr in CHECK's format, the tally line reads "N assertions".
- Mutation-check counts per file must be re-recorded (assertion counts are unchanged except text_util_test, +15).
- NOT converted: foolish tests (tests.c, msg_wire_test, analyse_test, oracle_answer_test, replay_* tests, bot_drive_threads, msg_expand, anim_plan: their CHECK(cond, msg) takes the message as a plain string and some count n_pass for a summary), uttt_analyse/bots/lang tests (no checks counter, variadic OK), uttt_api_smoke (no counter), werewolf, motion_test.c, c_base core/test/t.h (own T_EQ_* family; its zero-check rule is now check.h's CHECK_ZERO_IS_FAIL, but t_done's "ok  file: N tests" line is parsed by the c_base runner), twophone.h.

### Family 12, build scaffolding
- Done: chuiniu/ios/scripts/mac_tests.sh (176 -> about 45 lines) is a thin wrapper over shared/scripts/ios_mac_tests.sh, which gained POST_LIB_CMD/POST_LIB_LABEL (the architecture lint runs after the readers exist, also with --no-lib). Behaviour change: it now tests on the shared locked "ios-mac-tests" simulator instead of "iPhone 17e".
- Not done (risk or unverifiable without a Mac run): tallybones/ios/scripts/mac_tests.sh (needs Mac Catalyst support in the shared script: extra build settings, `env -u DEST`, ios-lib-catalyst), the three project.yml files (xcodegen base spec; signing and UI risk), the c/Makefile PROG template (shared .mk already holds most; the rest differs per product), the three product workflows (reusable workflow_call), wasm.yml's own clang apt-install beside shared/scripts/ci_llvm.sh.

### Family 16, collapse spring in foolish Swift
- Skipped. No Swift target imports CCollapse today (pickemup and uttt read the constant through their kernels' C headers: pk_api.h, uttt_anim.h), so foolish's CollapseTween.hostResponse (0.338) needs either a SWIFT_INCLUDE_PATHS edit in foolish's project.yml or a foolish kernel export plus a regenerated reader; both touch the Xcode project and cannot be checked here. 338 ms is shared/c/collapse/collapse.h COLLAPSE_RESPONSE_MS. Smallest safe route: export it from the foolish kernel (one const in the ios API header, picked up by structgen) and set `hostResponse = Double(FOOLISH_COLLAPSE_RESPONSE_MS) / 1000`.

### Family 17, small helpers
- shared/c/stats/percentile.h: stat_pct_i64/f64 (nearest rank, no libm), stat_cmp_f64. c_base bs_percentile is a wrapper (unit_bench expectations unchanged: checked by hand for 7, 95, 100, 20, 21). bench_dynamo, cn_scene_bench (p95 moves from a floor index to nearest rank) and uttt_test (comparator only) use it.
- Not lifted: pickemup pk_msg_test pct() (rounded index; its assertions would shift), uttt_test p50/p95 index math (printed only), native hammer/wt_client (retired).
- shared/c/util.h: ARRAY_LEN, util_clampi/f/d, util_mini/maxi/mind/maxd, util_popcount32/64. Applied to cn_scene, cn_stage, cn_lay, uttt_big_bot, tb_beats, tb_fuzz. The 174 inline `sizeof x / sizeof x[0]` are NOT rewritten (a churn-only sweep needing an include in each file); cn_tex clamp01 and hand_layout/analyse/uttt_anim clamps have a different shape and stay.

### Family 18, research-tool clones
- foolish/c/tools/endgame_retro/common.h: two_in, actor_of, mask_of (7 tools). The positions tables and interning stay per tool (sizes differ, one-liners are tuned).
- foolish/rust/bench_game.h: state_to_game and GAME_PREFIX once for bench_legal, bench_playout, bench_solve (playout now clears the prefix like the other two; num_logs stays 0).
- foolish/c/tests/difftest_step.h: struct_step once for sim_difftest and solver_difftest.
- structgen: backtick_name once for sg_kotlin kt_name and sg_swift swift_name (compile-checked with homebrew llvm headers; regenerated output must be byte-identical: run the structgen golden).
- Not done: foolyard clients c01/c02/c08/c09 on_view/on_wake, tools/leafbook build_book/enumerate and the other endgame_retro intern tables, c_base listed() (gone).
- leafbook/c_base: nothing.

### Deferred, per the first plan (superseded by the scope change above): none.

### Final-pass checklist for this item
- Regenerate: foolish/ios/Fixtures/goldens.json (+ any other fnv-pinned digests above), pickemup beats pins, structgen outputs (expect no diff).
- Run: shared tests (make -C uttt/c or pickemup/c run for the shared lanes), foolish/c tests, chuiniu/tallybones/pickemup/uttt run + asan, c_base unit_bench, unit_event utf8_rule, roster tests (tests.c ROSTER cases), a CI pass for the wasm builds (roster.c, analyse.c, winprob.c, msg_wire.c, octogen_strategy.c all changed and are in wasm).
- Mutation-check: utf8.h (surrogate, overlong, C1), ease_test (clamp ends), le_bytes u64/be tests, check.h CHECK_PRINT_CAP.

## Item: half-open socket detection + dashboard-view hint (netclient.c)

### What changed (foolish/c/src/netclient.c, netclient.h)
- PINGRESP bound: NC_PONG_MS = 10000. g_ping_at is set when a PINGREQ goes out with none outstanding, and cleared by ANY packet from the broker (take_packet: a stream that delivers pushes is alive, so a busy socket never false-positives behind a late PINGRESP). Ten seconds after the PINGREQ with nothing heard, nc_stream_next hands out ONE frame of len 0 (the "close this socket" signal), then NULL; nc_stream_due_ms returns 0 until that frame is out and folds g_ping_at + NC_PONG_MS into its wake. Keepalive (NC_KEEPALIVE_S 60, NC_PING_MS 30000) unchanged: 10 s is the NC_ACK_WAIT_MS precedent and is far inside the broker's 1.5 x 60 s drop.
- Dashboard view hint: in take_push, a u/<uid>/v view for a game held on its own stream is still not applied, but if its version > any_top(gate) the client queues refetch_queue(gid, now_ms + NC_RECONCILE_MS, version). Deliberate deviation from "immediately": the seat's cast normally follows the view in the next feed and own_settle drops the queued read for free; only a lost cast pays the read, 1 s later. Equal or older view: nothing.

### Fixes to make (host hookup owed, not done: batch rule is no host code growth)
- The three hosts must treat an nc_stream_next frame with len 0 as "close the socket": foolish/ios/FoolishNet/Stream.swift send() (cancel the task, then call closed()), foolish/sdk/ts/net/net_client.ts streamNext() consumers (web: ws.close(), then the existing onclose path), foolish/sdk/swift/NetClient.swift streamNext (today it would send an empty binary frame on the dead socket, which does nothing; the detection is inert until this lands). Also the readNcOut wrappers must not drop a len-0 frame as "none".
- Check wasm_net_api.c / ios_api_net.c need no change (NcOut layout unchanged).

### Owed tests (foolish/server/test/unit_netclient.c style; each natively, under 5 s, simulated clock)
1. Half-open: live socket, advance to NC_PING_MS, next() yields PINGREQ; advance NC_PONG_MS - 1: next() NULL and due_ms > 0 and <= 1; advance to NC_PONG_MS: next() yields a len-0 frame, then NULL, due_ms -1; nc_stream_closed returns the backoff wait; opened + CONNACK: every game sub is SUBSCRIBEd again and its SUBACK queues the catch-up read.
2. due_ms folds the deadline: right after the PINGREQ due_ms == NC_PONG_MS.
3. No false positive: PINGRESP at +100 ms after each PINGREQ for 20 cycles, never a len-0 frame; also a non-PINGRESP packet (a PUBLISH) clears the wait; a PINGRESP late by NC_PONG_MS - 1 is fine.
4. After the dead frame, opened()/closed() reset g_dead and g_ping_at (a second connection pings normally and can die again).
5. View hint: game held via nc_stream_sub(NW_SUB_GAME), board at version 5 (cast applied). View v=6 on u/<uid>/v: exactly one own refetch queued (not_before = now + NC_RECONCILE_MS), event list has no NC_EV_VIEW, board unchanged; a second view v=6 or v=7: still one slot (deduped, min_version raised to 7). View v=5 and v=4: none. View v=6 followed by the cast v=6 in the next feed: queued read is dropped (own_settle).
6. A game with no own subscription (g_boards false): the view still applies as before (regression).

### Mutation checks owed
- NC_PONG_MS test bound +1 / -1 (tests 1 and 3 must fail on the edge assertions).
- Drop `g_ping_at = 0` in take_packet: test 3 fails. Drop `g_ping_at = ...` set on PINGREQ: test 1 fails. Drop g_ping_at fold in due_ms: test 2 fails. Drop g_dead reset in opened/closed: test 4 fails.
- View hint: `>` to `>=` (test 5 equal-version case fails); remove the refetch_queue call (test 5 fails); apply the view to the board (board-unchanged assertion fails); remove NC_RECONCILE_MS (settle-for-free case fails).

### Tests to fix
- Any existing unit_netclient / unit_leftovers / fz_netclient / unit_stream case that runs next() past a PINGREQ for more than 10 s without a PINGRESP will now see a len-0 frame (grep for NC_PING_MS users); and any that asserts a dashboard view of a held game produces no read.

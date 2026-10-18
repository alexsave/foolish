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

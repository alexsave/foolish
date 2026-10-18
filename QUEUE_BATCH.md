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

## Fixes to make (collected; done at the end of the list, before the test pass)

- History names restore (item 1 regression, UI): the web history list shows `#1 #2 ...` instead of player names.
  Plan: /v1/history does its Query of pointers, then ONE BatchGetItem of the main SNAP#<id>/SNAP items projecting only what names need (extras; SQLite: the column subset); names go in the history row; pointers stay name-free so account delete still redacts only the main item.
  Keep the replay fetch-on-tap.
  Old pointers (written before item 1, no summary, rank 0) are SKIPPED by historyEntry, so every old replay vanishes from the list: in the same batch read, compute the summary from the main item with snap_summary (table_ops.c, moves + extras projected), so old and new replays both show, with names.
  No backfill unless the per-read cost matters.
  The list must render identically to before b092fe333: names, bold viewer seat, fool icon.
  Note the netwire row shape change when done.
- cb_rows_user_import is not atomic (read then write); fine for cbase-import, noted in case another caller appears.

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

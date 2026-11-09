# The journal's state buffer: 2,503 bytes per resident game for a 128-byte state

## What it was

Every resident game in the journal's overlay (`server/src/store/journal.c`, `jgame`) keeps the row it last acknowledged.
When the game is held decoded (`TableResident`), the row's state bytes are not kept up to date on every commit: they are encoded from the resident when something reads them (`row_refresh`, the "R3" rule), and the flush is one such reader (R4).
`row_refresh` grew `row.state` to `TABLE_STATE_MAX` before encoding, because `table_resident_encode` wants a buffer that large, and the buffer then stayed with the game until it left the overlay.
`TABLE_STATE_MAX` is 2,503 bytes at the server's caps, a 2,720-byte slot in musl's mallocng.

The state it holds is small.
After a sat2 run on the base build, the 6,100 rows in `games` have states of 92 to 241 bytes, 128 on average.
So a resident game that was flushed once holds about 2,700 bytes of allocation for about 128 bytes of content, and at sat2 the overlay holds its cap of 4,096 games (`OV_MAX_GAMES`).

## Who reads the cached bytes

- The flush (`snapshot`): it refreshes each due game, then copies the row (`row_dup`) into its snapshot; the copy is what it writes.
  The cached bytes are not read again by the flush.
- `ov_get` (`j_game_get`, the store's `game_get` under the journal): the view (`cb_table_view`), the games lists (`cb_store_games_get_each`), the admin panels and the finalize of a pending replay.
  It refreshes and copies the row.
- `around` (an admin's or import's write of one game): it refreshes, writes the row, then replaces it with the commit's.
- The operations themselves (`cb_journal_game_acquire`, `_peek`) never read the state bytes of a game held decoded: they adopt the resident.

Between two commits of a game, the cached bytes help only a second read after the first, and every commit makes them stale.
The flush, the only reader every resident game has, never reads them back.

## The design

1. A game held decoded keeps no state bytes that are stale: the commit that hands over a new resident frees them (`row_set_columns`).
2. The flush encodes each due game's state into a stack buffer and copies it straight into its snapshot (`row_dup_encoded`), and keeps nothing.
   That is the work it did before (one encode, one copy), less the buffer.
3. `ov_get` and `around` still refresh and keep the bytes for the next read, as before, but in a buffer of the state's size: the encode goes to a stack buffer and is copied into the game's (`row_refresh`).
   A second read before the next commit is the same copy as before; the first pays an extra copy of about 128 bytes and an allocation of that size.
4. A game read from the tables, recovered, or committed whole keeps its bytes as before (they are its row, sized exactly by `grow_copy`).

## Hypotheses (written before measuring)

The musl numbers are the production build (static musl, THP disabled) at sat2 (8 pairs, think 0 to 1 ms, 25 s), with the harness of `docs/ROSTER_COMPACT.md`.

- H1 (bytes per game): a resident game held decoded holds 0 state bytes after its flush, against a 2,503-byte buffer (a 2,720-byte mallocng slot) before.
  At 4,096 resident games that is about 10.6 MiB of allocation.
- H2 (RSS at sat2): musl sat2 RSS p50 falls by at least 8 MiB (expected about 10 MiB: nearly every resident game at the cap has been flushed at least once), and RssAnon mid-sat2 by at least 8,000 kB.
  Refuted if RSS p50 falls by less than 6 MiB.
- H3 (speed at sat2): sat2 moves/s, move p50 and move p99 stay inside the noise band of 3 interleaved baseline runs on the same machine and build.
  Refuted if moves/s is below the slowest baseline run, or p50 or p99 above the worst baseline run, by more than the band's width.
- H4 (the flush's row): the flush's per-game row copy (encode and copy) costs the same as before within 3%: the same encode and the same copy, into a stack buffer instead of the game's.
  Refuted if the microbench's median is more than 3% above the base's.
- H5 (reads): a read of a fresh row (the second read after a commit) is unchanged within 3%; the first read after a commit is at most 5% slower (an extra 128-byte copy and a small malloc against an encode and two copies).
  Refuted beyond those.
- H6 (`cbase-bench cpu`): kernel_action and every other row are unchanged within their run-to-run spread (none of them goes through the journal).

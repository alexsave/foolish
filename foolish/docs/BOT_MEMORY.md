# The resident's bot memory: 1,024 bytes per game that most games never fill

## What it is

Every game the server's journal holds decoded is a `TableResident` (`c/src/table.h`), 2,016 bytes at the server's caps, one malloc'd buffer per game (`server/src/store/journal.c` `jgame.res`), a 2,032-byte slot in musl's mallocng.
At sat2 the overlay holds its cap of 4,096 games (`OV_MAX_GAMES`), so the residents are about 8.1 MiB.

The resident's layout at the server's caps:

| field | offset | bytes |
|---|---|---|
| `head` (the Game up to `logs`, `TABLE_HEAD_BYTES`) | 0 | 776 |
| `rep` (the repetition ring) | 776 | 136 |
| `clock` | 912 | 16 |
| `ratings` | 928 | 52 |
| `rng_base` | 980 | 4 |
| `mem` (`BotMemory`) | 984 | 1,024 |
| `mem_valid` | 2,008 | 1 (+7 padding) |

`BotMemory` (`c/src/bot_memory.h`) is what the belief bots remember of the game: every seat's publicly located cards, voids, floors, tells, the current bout and the discard set, folded one session-log record at a time.
It is the same 1,024 bytes whether or not anything at the table reads it.

## Who reads and writes it

The memory lives in three places: the operation's `Table.mem` (with `mem_valid` and `mem_at`), the resident's copy (`TableResident.mem`, `mem_valid`), and the row (the state blob's section 1, `STATE_BLOB_SECTION_BOT_MEMORY`, `bot_mem_encode`'s sparse durable form, in `games.state`).

Writers of `Table.mem`, all in `c/src/table.c`:

- `mem_sync` / `mem_fold_board`: THE ONE FUNNEL. Every record the board gains is folded as the store holds it (masked, `bot_mem_fold_masked`), at every operation's opening (`scope_open`) and before every bot decision of a drive (`table_drive_seed`).
  The deal's `LOG_GAME_START` picks the block's kind from the seats (`mem_wanted`): a table where some brain reads the memory folds every record whole; a table of humans and brains that read nothing (`random`, `handwritten`, `simple_heuristic`: `bots.c` `uses_logs = 0`) only counts the records (`bot_mem_count`).
- `mem_load` (`table_load`): the row's section decoded (`bot_mem_decode`, which accepts only the canonical bytes this kernel writes), valid only for a block of this board's seat count.
- `mem_backfill` (`table_set_session_log`): a row that carried no memory has its stored session log folded once, of the kind the seats want; the server does this when `table_bots_need_logs` says so (`server/src/app/bots.c`).
- `mem_forget` (`table_create`), `mem_level_check` (a counts-only block on a table that wants the memory is no memory, and the log is folded instead).
- `table_adopt`: the resident's copy becomes the table's.

Readers:

- The belief brains, through the binding (`bot_mem_bind` in `table_bot_drive_with` and `table_drive_seed`; `bot_mem_of` / `bot_mem_for` / `bot_mem_bound_read`): `robusta_strategy.c` and its rollout family, `blackpowder_strategy.c`, `cordite_strategy.c`, `octogen_strategy.c` and fusion, `espresso_prod_strategy.c`, `bot_common.c` `bot_mem_belief` / `bot_mem_discards`, and a sampled world's discards (`bot_mem_discard_logs`).
  A brain bound to a counts-only block gets no block (`bot_mem_bound_read` returns NULL, and aborts the test builds), and folds its Game's own log as before.
- Every decision seed: `bot_mem_records` reads `n_records` of the bound block, counts-only or not (`bot_drive.c`).
- The row: `table_commit_products` and `table_resident_encode` write the section (`mem_section`); a counts-only block encodes as its record count, seat count and a kind byte (6 to 10 bytes).
- `table_resident` copies the table's memory, folded to the board (`mem_now`), into the resident.

Persistence: the memory is in every row the kernel writes (section 1 of a v6 state blob), so eviction and reload (`table_load` from `games.state`), crash recovery (the journal replays its moves on the tables' row, and a bot cycle replayed after a reload writes the bytes the live table wrote, `mem_backfill`'s comment) and the flush (`table_resident_encode`) all carry it.
Other hosts: the browser's and iOS's local table (`c/src/local_table.c`) keeps one full `TableResident` in its struct; the schema upgrade (`server/src/store/rows_sql.c`) and the tests keep theirs on the stack or in statics.
`analyse.c` is not touched.

## Can a table without a memory-reading bot get one later?

Not within a game.
The seats change only in the lobby: `table_join`, `table_leave`, `table_add_bot`, `table_remove_bot` and `table_reseat` all refuse a table that is not `GAME_STATUS_WAITING`.
The server never plays a human's seat with a brain: a drive moves only the seats `game_human_mask` leaves out, and there is no timeout takeover, no hint and no auto-play in `server/src` (searched: autoplay, takeover, forfeit, kick, hint).
Spectators read envelopes, never the memory.
A rematch goes back to the lobby (`table_continue`), where bots can be added, and its deal's `LOG_GAME_START` resets the block and picks its kind again from the seats as they are then.

So a table's memory is counts-only (or not yet valid, in the lobby) for the whole of a game exactly when no seat has a brain that reads it, and what such a block holds is fixed by the kernel: `bot_mem_clear`, then `num_players`, `counts_only = 1` and `n_records` (`bot_mem_count`, `mem_backfill`'s counts-only start, and `bot_mem_decode`, which refuses a counts-only encoding with any other field set).
An invalid block is `bot_mem_clear` exactly (`table_init`, `mem_forget`, `mem_level_check`, `mem_load`'s refusal).
Such a block's 1,024 bytes carry 6 bytes of information, and its record count must be kept exactly: the bots' decision seeds read it, and the row writes it.

## The design

1. The resident's memory moves to its end, after a summary of what a counts-only or invalid block holds: `mem_valid`, `mem_full`, `mem_players` and `mem_records`.
   `table_resident` writes the block (`mem`) only for a memory that is valid and not counts-only (`mem_full`); otherwise only the summary.
   `table_adopt` and `table_resident_encode` read `mem` only when `mem_full`, and otherwise rebuild the block from the summary: `bot_mem_clear`, then (valid) `num_players`, `counts_only` and `n_records`.
   The native test builds (`BOT_MEM_ASSERT`) check at every `table_resident` that the rebuilt block is the table's, byte for byte.
2. `table_resident_bytes(res)`: the bytes a resident needs, `offsetof(TableResident, mem)` (992) without the block and `sizeof(TableResident)` (2,016) with it.
3. The journal holds each game's resident in a buffer of that size (`jgame.res_cap`).
   A commit handing over a resident with the block swaps buffers as before (no copy); one without copies its 992 bytes into the game's own buffer, made at that size, and the caller keeps its full one.
   The caller only ever gets back a full buffer or NULL (it makes one at its next commit, as it does now).

Nothing a bot reads changes: a block with memory in it is held, adopted and encoded exactly as before, and a counts-only block is rebuilt to the bytes it had.

## Hypotheses (written before measuring)

The musl numbers are the production build (static musl, THP disabled) at sat2 (8 pairs of human clients, think 0 to 1 ms, 25 s), with the harness of `docs/ROSTER_COMPACT.md` and `docs/STATE_BUFFER.md`.
sat2's tables are two humans each, so every resident's memory is counts-only.

- H1 (bytes per game): a resident without a memory-reading bot is held in 992 bytes (a 1,008-byte mallocng slot, and a 1,008-byte glibc chunk) instead of 2,016 (2,032): 1,024 bytes less per game, about 4.0 MiB at 4,096 games.
  A resident with a memory-reading bot is unchanged (2,016).
- H2 (RSS, humans-only sat2): musl sat2 RSS p50 falls by at least 3 MiB (expected about 4 MiB), and RssAnon mid-sat2 by at least 3,000 kB (expected about 4,000 kB).
  Refuted if RSS p50 falls by less than 2.5 MiB.
- H3 (RSS, bot-heavy load): with every table one human and one octogen bot (which reads the memory), RSS p50 changes by less than 1 MiB either way, since nearly every resident keeps its block.
  Refuted if it falls or rises by more than 1 MiB.
- H4 (speed, sat2): sat2 moves/s, move p50 and move p99 stay inside the noise band of 3 interleaved baseline runs on the same machine and build.
  Refuted if moves/s is below the slowest baseline run, or p50 or p99 above the worst baseline run, by more than the band's width.
- H5 (the commit, microbench): a commit handing over a humans-only resident costs at most 30 ns more on musl and 15 ns more on macOS (a 992-byte copy where a swap was); the flush's row and the reads stay within 3% (the encode rebuilds a counts-only block on the stack instead of reading the resident's).
  Refuted beyond those.
- H6 (bot speed): on the bot-heavy load, moves/s and move p50 and p99 stay inside the band of 3 interleaved baseline runs; `cbase-bench cpu` kernel_action and every other row are unchanged within their run-to-run spread.
- H7 (bot behaviour): seeded bot games, bot-only and mixed, with every brain the server seeds and the ones it does not, played through the server's path (each operation adopted from the previous commit's resident, held at its `table_resident_bytes` size), make exactly the same moves and write exactly the same rows on base and change.
  Refuted by any difference in any move or any row byte; that stops the change.

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
   `table_adopt` and `table_resident_encode` read `mem` only when `mem_full`.
   Otherwise the adopt rebuilds the block from the summary (`bot_mem_clear`, then, when valid, `num_players`, `counts_only` and `n_records`), and the encode writes the counts-only section from it (`bot_mem_encode_counts`, which `bot_mem_encode` uses for such a block too).
   The native test builds (`BOT_MEM_ASSERT`) check at every `table_resident` that the rebuilt block is the table's, byte for byte.
2. `table_resident_bytes(res)`: the bytes a resident needs, `TABLE_RESIDENT_SMALL` (`offsetof(TableResident, mem)`, 992) without the block and `sizeof(TableResident)` (2,016) with it.
3. `table_resident_into(t, clock, whole, small)` writes the resident into the buffer of its size and returns it; `table_resident` is it with one whole buffer for both, as every other host keeps.
4. The journal holds each game's resident in a buffer of that size (`jgame.res_cap`), and the section keeps one buffer of each size (`cb_section.res`, `.res_small`).
   A commit hands over the buffer `table_resident_into` wrote, and the journal swaps it with the game's as before (no copy); when the game's old buffer is of the other size (a game changes size only at a deal), it is freed and the section makes one at its next commit.
   A resident copied in (`res_copy`: a created game, a decoded one) is copied at its size.

A first prototype had the section write every resident into its whole buffer and the journal copy a small one's 992 bytes into the game's: +15 ns per humans-only commit in the microbench, where the swap had cost 9 ns, so it was replaced by the buffer of each size.

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

## The bot behaviour gate

`/Users/alex/Dev/cdom-memtest/mem3/gate/gate.c` plays seeded games the server's way: every operation runs on a table adopted from the previous commit's resident, held as the journal holds it (the change: `table_resident_into` into two buffers of exactly their sizes, the one written kept; the base: the whole struct), with an eviction every 7th operation (the row loaded with its session log, as a game evicted from the overlay is).
Bots move by their brains through `table_bot_drive`; human seats by the gate's own seeded draw (one xorshift, seeded by a named constant and the game's index).
Each commit prints the moves it applied (seat and action wire) and the row's state bytes in full, and checks that the held resident encodes to exactly those bytes.
Base and change ran with the same arguments and their outputs were compared byte for byte.

The tables, per seed: for each of `random`, `robusta`, `firecracker`, `blackpowder`, `cordite`, `handwritten`, `octogen`, `fusion` (every brain the server seeds) and `gunpowder`, `simple_heuristic`, `espresso`, a human and one bot, a human and two (on the first seed with a rematch that seats an octogen in the lobby), and four bots alone; then two humans with a rematch that seats an octogen, three humans, a human with `handwritten` and `random` with a rematch that seats a `cordite`, a human with `octogen`, `random` and `handwritten`, a human with `cordite`, `robusta` and `blackpowder`, `random`, `handwritten` and `simple_heuristic` alone, and `fusion`, `octogen`, `firecracker` and `espresso` alone.

| build | tables | commits (rows compared) | moves compared | of them bot actions | small residents held | differences |
|---|---|---|---|---|---|---|
| native host build (-O3, LTO, `BOT_MEM_ASSERT`) | 400 | 41,586 | 50,438 | 38,782 in 29,427 drives | 14,046 | 0 |
| the server's own kernel objects (c_base, its caps and flags) | 400 | 41,586 | 50,438 | 38,782 in 29,427 drives | 14,046 | 0 |
| change only, ASan and UBSan, exact-size buffers | 80 | 9,125 | (the first 80 tables of the base's output) | | 2,681 | 0 |

The native build also checks, at every `table_resident` (`BOT_MEM_ASSERT`), that the block a small resident's summary rebuilds is the table's byte for byte; it never fired.

## Results

Measured 2026-11-09 on Alex's M1 Mac, in Docker (6 CPUs), the server pinned to 2 CPUs and the load client to 4, the base build's p3load client for every run, base = main at 1270832e0.
Base and change were run interleaved, one at a time, three rounds in each order.
The harness and the raw numbers are in /Users/alex/Dev/cdom-memtest/mem3 (musl2/out/gate_round1.txt, gate_round2.txt, botgate.txt, jbench_musl.txt; bench/jbench_native5.txt, bench/cpu/; gate/).
macOS Spotlight (mds_stores at 120 to 140% of a core) and softwareupdated ran through the second sat2 round, and those runs are the slow ones of both builds.

### Memory

| load | metric | base | change | delta |
|---|---|---|---|---|
| sat2 (humans only), musl, THP off (prod) | RSS p50 | median 24.2 MiB (23.8 to 25.7, 6 runs) | median 21.2 MiB (20.8 to 22.0, 6 runs) | -3.0 MiB |
| sat2 (humans only), musl | RSS max | 31.1 to 31.3 MiB | 27.1 to 27.3 MiB | -4.0 MiB |
| sat2 (humans only), musl | RssAnon mid-sat2 | median 18,788 kB (18,748 to 18,856) | median 14,694 kB (14,660 to 14,776) | -4,094 kB |
| bot-heavy (256 tables of a human and an octogen), musl | RSS p50 | 10.5, 10.6, 10.5 MiB | 10.5, 10.6, 10.6 MiB | 0 |
| bot-heavy, musl | RssAnon mid-run | 5,488, 5,580, 5,488 kB | 5,512, 5,552, 5,584 kB | +31 kB (inside the spread) |

Per resident game (4,096 at sat2), the musl RssAnon delta is 4,094 kB / 4,096 = 1,023 B: the 2,032-byte mallocng slot became a 1,008-byte one.
The bot-heavy load holds few games (one human's moves wait for an octogen's on one bot worker, so a table plays about one game in 25 s), and nearly all of them hold their block, as predicted.

### Speed

| load | metric | base | change |
|---|---|---|---|
| sat2, musl (prod) | moves/s | 23,911, 29,695, 32,791, 22,960, 28,431, 23,868 | 24,005, 32,991, 32,542, 23,738, 31,155, 24,551 |
| sat2, musl | move p50 | 0.05 to 0.12 ms | 0.05 to 0.12 ms |
| sat2, musl | move p99 | 0.43 to 0.95 ms | 0.40 to 0.94 ms |
| bot-heavy, musl | the human's moves/s | 75.1, 76.2, 76.1 | 77.4, 77.3, 75.7 |
| bot-heavy, musl | bot cycles in 25 s (journal records less human moves) | 1,874, 1,845, 1,911 | 1,863, 1,892, 1,864 |
| bot-heavy, musl | move p50 / p99 | 0.07 to 0.12 / 0.47 to 1.57 ms | 0.08 to 0.10 / 0.66 to 1.47 ms |
| the gate, the server's kernel build, native (M1), 120 tables run alone | bot drive time | 2,170 / 2,088 us per drive | 2,166 / 2,098 us per drive |
| native (M1) | `cbase-bench cpu` kernel_action | 831 / 830 / 830 ns | 832 / 833 / 832 ns |
| native (M1) | `cbase-bench cpu` dynamo_get_parse | 2,779 / 2,791 / 2,841 ns | 2,812 / 2,776 / 2,781 ns |

In the sat2 pairs the change was the faster run in 5 of 6 (+94, +3,296, -249, +778, +2,724 and +683 moves/s); the pairs that fell to 23 to 24k fell together.

The microbench (/Users/alex/Dev/cdom-memtest/mem3/bench/jbench.c, as in `docs/STATE_BUFFER.md`, with two more paths: an operation's adopt of the game's resident and a commit's making of one) runs on a two-seat humans-only game 8 moves in.
ns per call, medians of the interleaved runs:

| build | path | base | change |
|---|---|---|---|
| musl (prod), in Docker, pinned | commit (the buffers swap) | 43.5 | 44.3 |
| musl | commit + flush row | 162.3 | 152.4 |
| musl | commit + first read | 164.5 | 153.4 |
| musl | second read | 48.2 | 48.9 |
| musl | adopt | 93.8 | 87.2 |
| musl | make the resident (`table_resident` / `_into`) | 43.1 | 27.6 |
| native (M1) | commit | 9.1 | 9.4 |
| native (M1) | commit + flush row | 97.2 | 92.6 |
| native (M1) | commit + first read | 100.8 | 96.7 |
| native (M1) | second read | 38.5 | 38.4 |
| native (M1) | adopt | 75.7 | 74.5 |
| native (M1) | make the resident | 49.0 | 37.9 |

A commit now looks up its resident's size (`table_resident_bytes`, inline) before the swap: 0.3 ns on macOS, 0.8 on musl.
Making a small resident skips the 1,024-byte block copy, and its encode writes the counts-only section from the summary (`bot_mem_encode_counts`, which `bot_mem_encode` also uses for such a block, so the format has one writer).

### The hypotheses

- H1 confirmed: a resident without a memory-reading bot is 992 bytes (a 1,008-byte slot) against 2,016 (2,032); measured 1,023 B less per game. A resident with one is unchanged.
- H2 confirmed at its lower bound: musl sat2 RSS p50 fell 3.0 MiB (predicted at least 3, about 4), RSS max 4.0 MiB and RssAnon 4,094 kB (predicted at least 3,000, about 4,000).
  RSS p50 falls less than RssAnon because the overlay fills to its cap during the run, so the median sample is taken before every game is resident.
- H3 confirmed: on the bot-heavy load RSS p50 is unchanged and RssAnon inside its run-to-run spread.
- H4 confirmed: every sat2 run of the change is inside or above the base band for moves/s, p50 and p99.
- H5 confirmed for the final design: the commit is 0.3 ns (macOS) and 0.8 ns (musl) slower, inside the 15 and 30 ns bounds, and the flush's row and the first read are faster (4 to 7%), the second read within 1.5%.
  The first prototype (the copy) met the commit's bound on macOS (+15 ns) but refuted the 3% bound of the flush's row and the first read (+17%), which is why it was replaced.
- H6 confirmed: the bot-heavy load's bot cycles and the human's moves/s are inside the base runs' band, the gate's drive time is within 0.5%, and kernel_action is within 0.4%.
- H7 confirmed: 0 differences in 50,438 moves and 41,586 rows on each of two builds.

## Tests

- `c/tests/tests.c` `test_table_resident_holds_memory_only_where_read`: humans-only tables, tables of `random` and `handwritten` bots and tables with an octogen, played the server's way on residents held at exactly their size; at every commit the resident is of the size its seats call for, encodes to the row's bytes, and adopts to the memory the whole struct adopts to; a humans-only table that seats an octogen in its rematch's lobby is small through the lobby and whole from the deal (mutation-checked, below).
- `server/test/unit_journal.c` `a_resident_is_held_at_its_size`: through the app, a two-human game is held in `TABLE_RESIDENT_SMALL` bytes in its lobby, at its deal and after each move, and a game with an octogen whole once dealt (`cb_journal_game_facts.resident_bytes`, new) (mutation-checked, below).
- Run: the kernel suite (`build/cnitro_tests`, 39,618 checks) and `bot_memory_test`; `unit_journal`, `unit_store`, `unit_app`, `unit_bots_kick`, `unit_inline_moves`, `unit_kernel_threads`, `unit_import`.

### Mutation checks (2026-11-09, after the merge)

Harness: /Users/alex/Dev/cdom-memtest/final/mutate.py on a clone at f99698319 (merged main), results in final/out/mutate.txt and mutate_crashes.txt; every mutant was restored and touched, and clean builds before and after passed (`git diff` of the clone empty).

| mutant | what it breaks | result |
|---|---|---|
| 483-K1 | `table_resident_into`: `full = valid` (a counts-only block held whole) | red: `every resident is held at the size its seats call for` |
| 483-K2 | `full = false` (an octogen's block held as its summary) | red: the `BOT_MEM_ASSERT` check in `table_resident_into` aborts (`a block without its memory is not its summary`), which `cnitro_tests` builds with |
| 483-K3 | `mem_records = 0` (the summary loses its record count) | red: the same `BOT_MEM_ASSERT` abort |
| 483-K4 | `mem_from_resident` leaves `counts_only` 0 | red: the same `BOT_MEM_ASSERT` abort |
| 483-K6 | `mem_from_resident` drops `num_players` | red: the same `BOT_MEM_ASSERT` abort |
| 483-K5 | `bot_mem_encode_counts` writes `cur_def` 0 | red, but not in the new test: the row and the resident encode through the same function, so `test_table_resident_holds_memory_only_where_read` agrees with itself; the pinned bytes catch it (`deal commit bytes unchanged`, `load then commit is byte-identical`, `bot memory: the dealt row is the current format`) |
| 483-J1 | `res_take`: `cap = sizeof(TableResident)` for every buffer | red by a crash (SIGSEGV: a 2,016-byte copy into a 992-byte buffer), before the assertion |
| 483-J2 | `res_copy`: `need = sizeof(TableResident)` | red: `[a_resident_is_held_at_its_size] facts(s, gid, &rc).resident_bytes == TABLE_RESIDENT_SMALL` |
| 483-J3 | `table_ops.c`: the whole buffer passed as both, so the small one is never used | red by a crash (SIGSEGV after a buffer of the wrong size comes back); an earlier run with stale objects showed `a_resident_is_held_at_its_size` red on `resident_bytes` |

So every mutant goes red, the two new tests on the right assertion where the mutant leaves memory intact; four are caught first by the summary's own debug check, and two only crash.
Measured on a real t4g.nano on 2026-11-09 (H1 to H4 confirmed, H6 not decided with two bot runs a side): /Users/alex/Dev/cdom-memtest/GRAVITON.md.

## Follow-ups

- The largest per-game item left is the board head (`TABLE_HEAD_BYTES`, 776 of a humans-only resident's 992 bytes).
  This is the last step of the memory series (Alex: after the bot memory "we are good to stop optimizing foolish memory"), so it is not pursued.

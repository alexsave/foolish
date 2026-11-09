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

1. The flush encodes each due game's state into a stack buffer and copies it straight into its snapshot (`row_dup_encoded`), and keeps nothing.
   That is the work it did before (one encode, one copy), less the buffer.
2. `ov_get` and `around` still refresh and keep the bytes for the next read, as before, but in a buffer of the state's size: the encode goes to a stack buffer and is copied into the game's (`row_refresh`, through `grow_copy`, which grows the buffer only when a state is longer than any before).
   A commit leaves that buffer in place (stale, as before), so the next read after a commit reuses it and allocates nothing.
3. A game read from the tables, recovered, created or committed whole keeps its bytes as before (they are its row, sized exactly by `grow_copy`).
4. So a resident game holds at most a buffer of its longest state read or written whole (about 130 to 240 bytes for two seats), never `TABLE_STATE_MAX`.
   `cb_journal_game_facts.state_bytes` shows it to a test.

A first prototype also freed the buffer at every commit that hands a resident over, so a game that only plays would hold none at all.
It made the first read after a commit allocate again (+20% in that read's microbench on macOS), so it was dropped: the stale buffer is at most a state's size.

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

## Results

Measured 2026-11-09 on Alex's M1 Mac, in Docker (6 CPUs), the server pinned to 2 CPUs and the load client to 4, the base build's p3load client for every run, base = main at 57a40a1f5.
Base and change were run interleaved, one at a time, three rounds in each order.
The harness and the raw numbers are in /Users/alex/Dev/cdom-memtest/mem2 (musl2/out/gate_round*.txt, out/gate_al_r*.txt, bench/).
macOS Spotlight (mds_stores at 70 to 210% of a core) ran through every measurement.

### Memory

| build | metric | base | change | delta |
|---|---|---|---|---|
| musl, THP off (prod) | sat2 RSS p50 | median 34.5 MiB (31.5 to 34.9, 6 runs) | median 25.1 MiB (24.0 to 25.3, 6 runs) | -9.4 MiB |
| musl, THP off (prod) | sat2 RSS max | 41.2 to 41.3 MiB | 31.1 to 31.2 MiB | -10.1 MiB |
| musl, THP off (prod) | RssAnon mid-sat2 | median 29,160 kB | median 18,794 kB | -10,366 kB |
| glibc | sat2 RSS p50 | median 45.3 MiB (44.0 to 47.1, 4 runs) | median 35.5 MiB (35.2 to 37.2, 4 runs) | -9.8 MiB |
| glibc | RssAnon mid-sat2 | median 37,842 kB | median 27,426 kB | -10,416 kB |

Per resident game (4,096 at sat2), the musl RssAnon delta is 10,366 kB / 4,096 = 2,592 B: the 2,720-byte slot, less the buffer of the state's size that a game created or read still holds.

### Speed

| build | metric | base | change |
|---|---|---|---|
| musl (prod) | sat2 moves/s | 24,253 (host noise), 31,317, 31,883, 31,940, 31,155, 32,711 | 24,105 (host noise), 32,366, 32,975, 32,675, 32,953, 32,825 |
| musl (prod) | move p50 | 0.05 ms (0.11 in the noisy pair) | 0.05 ms (0.11 in the noisy pair) |
| musl (prod) | move p99 | 0.41 to 0.78 ms (0.90 in the noisy pair) | 0.39 to 0.47 ms (0.92 in the noisy pair) |
| glibc | sat2 moves/s | 33,591, 32,861, 34,196, 33,885 | 31,487, 33,647, 34,181, 34,424 |
| glibc | move p99 | 0.31 to 0.46 ms | 0.33 to 0.54 ms |
| native (M1) | `cbase-bench cpu` kernel_action | 816 / 816 / 816 ns | 811 / 814 / 820 ns |
| native (M1) | `cbase-bench cpu` dynamo_get_parse | 2,746 / 2,740 / 2,752 ns | 2,784 / 2,881 / 2,724 ns |

The microbench (`bench/jbench.c`) includes journal.c and times its own functions on a two-seat game 8 moves in (a 134-byte state): a commit handing a resident over, the flush's row (the commit, then the encode and the copy into the snapshot), the first read after a commit (`row_refresh` and `row_dup`) and a second read.
Medians of 5 interleaved runs, ns per call:

| build | path | base | change |
|---|---|---|---|
| musl (prod), in Docker, pinned | commit | 43.6 | 43.6 |
| musl (prod), in Docker, pinned | commit + flush row | 198.0 | 162.6 |
| musl (prod), in Docker, pinned | commit + first read | 197.7 | 164.7 |
| musl (prod), in Docker, pinned | second read | 93.4 | 48.6 |
| native (M1) | commit | 9.0 | 9.0 |
| native (M1) | commit + flush row | 96.8 | 95.5 |
| native (M1) | commit + first read | 96.8 | 98.8 |
| native (M1) | second read | 38.0 | 37.8 |

The musl second read runs the same code in both builds, so its 45 ns are musl's allocator, not the code: with a 2,720-byte slot beside them, the read's two small allocations cost more.
On macOS the first read after a commit is 2 ns (2%) slower, the copy of the state from the stack; on musl, the production allocator, it is 17% faster.

In every musl pair but the noisy first one the change was the faster run (+1,049, +1,092, +735, +1,798 and +114 moves/s); the first pair ran while Spotlight held the host, and both builds fell to 24k together.
On glibc the first pair is 2,104 moves/s apart with the change behind; the next three are within 800 either way, two of them with the change ahead.

### The hypotheses

- H1 confirmed: a flushed resident game holds no state buffer of the bound; the measured saving is 2,592 B per game against a 2,720-byte slot.
- H2 confirmed: musl sat2 RSS p50 fell 9.4 MiB and RssAnon 10,366 kB, against a prediction of at least 8 MiB (about 10) and a refutation threshold of 6 MiB.
- H3 confirmed on the production (musl) build: every run of the change is inside or above the base band for moves/s and inside it for p50 and p99. On glibc one of four runs is below the base band by more than its width, in the order where it ran first after a build; the host's noise is too wide there to decide.
- H4 confirmed: the flush's row is 1.3% faster on macOS and 18% faster on musl.
- H5 confirmed: a second read is unchanged on macOS (and faster on musl); the first read after a commit is 2% slower on macOS, inside the 5% bound, and 17% faster on musl.
- H6 confirmed: kernel_action and the other rows are within their spread.

## Tests

- `server/test/unit_journal.c` `a_resident_keeps_state_bytes_of_its_size`: after each move and flush, and after a read, the game's state buffer is under `TABLE_STATE_MAX / 4`, and the tables and the read hold the same state (mutation check pending).

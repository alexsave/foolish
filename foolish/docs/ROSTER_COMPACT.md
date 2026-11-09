# Roster compaction: why the roster was 1227 bytes, and what it is now

## Why it was fixed width

The durable roster (`games.roster`, `c/src/roster.h`) was a fixed 1227-byte record: a 203-byte header with a 200-byte title slot, then 8 seat records of 128 bytes, all 8 written, the unused ones zero.
A real 2-seat table holds about 70 bytes of content and a 4-seat one about 150.

The reason is in `docs/C_GAME_SHAPE_MIGRATION.md` 3.2 and 3.4.
The roster replaced the Postgres `players` JSONB column, and the conversion of live rows was done once, in SQL, by a plpgsql function (`legacy_roster_hex`) that built the bytes with `lpad`/`rpad`.
Fixed width made that function a straight concatenation.
The other two reasons given there were that SQL could read a seat's id without a parser "if it ever must (nothing in the target does)", and that the size was about that of the JSONB it replaced.

None of that holds any more.
The conversion function was deleted in the contract step, the store is SQLite with `roster BLOB NOT NULL` and no constraint on it (`server/sql/schema.sql`), and no SQL reads inside the column.
`git log -S ROSTER_BYTES` reaches only the move of foolish into `foolish/`, so the layout came over unchanged from the Supabase era.

## Who reads the roster bytes

- The kernel: `roster_decode` / `roster_encode` (`c/src/roster.c`), through `table_load`, `table_seal` and `table_commit_products` (`c/src/table.c`).
- The server: the journal's resident games (`server/src/store/journal.c`), the section (`server/src/app/table_ops.c`), the SQL rows (`server/src/store/rows_sql.c`, opaque), the schema-9 upgrade (`rows_sql.c` `up_game`), the account-merge migration (`server/src/store/migrate_merge.c`), the import (`server/src/store/store_import.c`, through `table_load`), and the admin game panel (`server/src/app/admin_panels.c`, through `roster_decode`).
- c_base: `core/src/core_json.c` prints only `roster_len`; the DynamoDB provider and its tests carry the bytes opaquely; `fuzz/seeds.c` writes seed rows.
- Tests and fixtures: `c/tests/tests.c`, `c/tests/move_list_test.c`, `c/tests/bench_engine.c`, `server/test/unit_journal.c`, `server/test/unit_store.c`, `server/bench/cpu.c`, the wasm test entries (`c/wasm/wasm_table_api.c`, `c/wasm/wasm_bots_api.c`) and their e2e helpers (`e2e/helpers/table_fixture.ts`, `e2e/helpers/roster_kernel.ts`, which forward the bytes opaquely).
- Clients: none.
The durable roster never goes over the wire.
Clients get the ENVELOPE TRAILER (`roster.h`, section 2), a separate variable-length layout that this change does not touch, so shipped iOS builds and open web tabs read exactly what they read before.
The iOS app's "packed roster" (`c/ios/ios_api_identity.c`, `sdk/swift/RosterWire.swift`) is the iMessage lobby's own format and unrelated.

## Why the ids are text

Every id that is seated on the server is already a canonical lowercase UUID: users (the JWT carries `sub` packed to 16 bytes, `c_base/core/src/auth/jwt.c`) and bots (`server/src/store/bot_seed.c`), and every SQLite id column is a 16-byte BLOB.
Text survives in memory (`RosterSeat.id`, `cb_uuid`), in the roster record, and in three wire formats the shipped iOS kernel speaks (the envelope trailer, the ask bodies, the netwire responses).
The only non-UUID seat ids are the browser's local table (`"local-you"`, `"local-bot"`, `c/src/local_table.h`) and short test ids.

This change takes the part that is free: the durable format 2 stores a canonical lowercase UUID as 16 bytes (an id length byte of 0) and keeps any other id as text.
Binary ids end to end (`RosterSeat.id` as 16 bytes, `cb_uuid` as bytes, the journal and section lists, the JWT decode) is a follow-up, and the three client wire formats would need a format bump gated on what old builds can read.

## The design

1. Durable format 2: the version, n, then every field as a length byte and its bytes, nothing padded, and UUID ids packed.
   `roster_decode` still reads format 1 (exactly 1227 bytes, every pad byte zero, as before), so every stored row loads; its next whole commit writes it as format 2.
   One canonical byte string per roster still holds: the lengths are exact, nothing may follow the last seat, and a UUID written as text is refused (`ROSTER_E_CANON`).
2. One copy in memory: `TableResident` no longer carries a decoded `Roster` (1235 bytes, mostly padding).
   The host keeps the roster bytes beside the resident and `table_adopt` decodes them; every commit's row carries them.
3. The journal no longer keeps `humans[8]` and `bots[8]` (592 bytes of `cb_uuid` text per game); it reads who sits where from the roster bytes (`roster_seat_refs`, which walks either format without decoding).

## Hypotheses (written before measuring)

Struct sizes at the server's caps: `Roster` 1235 B; `TableResident` 3256 B before, 2016 B after; `jgame` loses 2 x 8 x 37 + 8 = 600 B.
The musl numbers below are the production build (static musl, THP disabled), per resident game in the journal overlay.

- H1 (bytes per game): the overlay's allocations per resident game drop from about 7,920 B (resident 3,256 + roster 1,227 + jgame ~936 + state buffer ~2,500) to about 4,950 B (resident 2,016 + roster ~80 + jgame ~340 + state ~2,500), a saving of about 2,970 B.
  Refuted if the measured saving is under 2,400 B per game.
- H2 (RSS at sat2): sat2 RSS p50 on the musl build falls by at least 9 MiB (about 4,096 resident games x 2.9 KiB, less allocator rounding).
  Refuted if it falls by less than 6 MiB.
- H3 (speed): sat2 moves/s, move p50 and move p99 stay inside the noise band of 3 baseline runs on the same machine and build.
  Refuted if moves/s is below the slowest baseline run, or p50 or p99 above the worst baseline run, by more than the band's width.
- H4 (CPU of one action): `cbase-bench cpu` kernel_action does not get slower beyond its run-to-run spread; the roster decode added to every adopt costs less than the 1235-byte copy it replaces plus the 1235-byte copy into each new resident.
  Refuted if kernel_action's median is more than 3% above the baseline's.

## Results

Measured 2026-11-09 on Alex's M1 Mac, in Docker (6 CPUs), the server pinned to 2 CPUs and the load client to 4, the BASE build's p3load client for every run, base = main at 400a65d09.
The harness and the raw numbers are in /Users/alex/Dev/cdom-memtest/roster (musl2/out/gate_round*.txt, out/gate_al*.txt).
Base and change were run interleaved, one at a time, in both orders.

### Memory

| build | metric | base | change | delta |
|---|---|---|---|---|
| musl, THP off (prod) | sat2 RSS p50 | 46.9 MiB (45.7 to 47.4, 9 runs) | 34.4 MiB (34.3 to 34.5, 9 runs) | -12.5 MiB |
| musl, THP off (prod) | sat2 RSS max | 53.6 MiB | 41.3 MiB | -12.3 MiB |
| musl, THP off (prod) | RssAnon mid-sat2 | 41,650 kB | 29,160 kB | -12,490 kB |
| glibc | sat2 RSS p50 | median 54.2 MiB (50.5 to 55.4, 6 runs) | median 45.4 MiB (44.1 to 46.1, 6 runs) | -8.9 MiB |
| glibc | RssAnon mid-sat2 | median 49,840 kB | median 38,740 kB | -11,100 kB |

Per resident game (the overlay holds OV_MAX_GAMES = 4,096 at sat2), the musl RssAnon delta is 12,490 kB / 4,096 = 3,120 B.
By struct size: `TableResident` 3,256 B to 2,016 B, the roster bytes 1,227 B to 63 B for a sat2 table (two UUID seats), and `jgame` 600 B smaller, about 7,920 B to 4,860 B allocated per game.

### Speed

| build | metric | base | change |
|---|---|---|---|
| musl (prod) | sat2 moves/s | mean 31,451 (30,654 to 31,925, 9 runs) | mean 32,078 (31,745 to 32,526, 3 runs of the final build) |
| musl (prod) | move p50 | 0.05 ms (one run 0.06) | 0.05 ms |
| musl (prod) | move p99 | 0.48 to 0.80 ms, median 0.58 | 0.47 to 0.56 ms |
| glibc | sat2 moves/s | 31,731, 32,689, 29,304, 29,815, 27,384, 27,690, 19,829, 27,707 | 33,305, 32,480, 27,001, 28,347, 30,788, 26,669, 26,857, 27,718 |
| native (M1) | `cbase-bench cpu` kernel_action | 1,247 / 1,233 / 1,230 ns | 829 / 824 / 825 ns |
| native (M1) | `cbase-bench cpu` dynamo_get_parse | 4,603 / 4,544 / 4,556 ns | 2,825 / 2,827 / 2,816 ns |
| native (M1) | adopt + resident cycle (scratch microbench) | 117 ns | 128 ns |

In each final musl pair the change was the faster run (+269, +954 and +49 moves/s).
The first prototype, which judged the roster's rules on every adopt, cost 183 ns in the adopt cycle; the unpack for shape only brought it to 128 ns.
The glibc runs were taken while macOS's mediaanalysisd and Spotlight held 1 to 2.5 cores of the host, so their spread (19.8k to 33.3k) is the host's, not the build's; their paired differences have a median of about -100 moves/s with a band of thousands, so they neither show nor rule out a difference smaller than that.
The adopt cycle is 11 ns slower (a 1,235-byte memset and a 2 x 16-byte hex expansion, against two 1,235-byte copies before); at about 60 us of server CPU per move that is under 0.02%, and the same move saves the roster encode at every flush, smaller journal records and smaller rows.

### The hypotheses

- H1 confirmed: about 3,120 B per resident game (RssAnon), against a prediction of 2,970 B and a refutation threshold of 2,400 B.
- H2 confirmed: musl sat2 RSS p50 fell 12.5 MiB, against a prediction of at least 9 MiB and a refutation threshold of 6 MiB.
- H3 confirmed on the production (musl) build: every final run is inside or above the base band for moves/s and inside it for p50 and p99. On glibc the host's noise is too wide to decide.
- H4 confirmed: kernel_action is 33% faster (the format 1 decode re-encoded all 1,227 bytes to check the padding, and every row and products write was 1,227 bytes).

## Tests

- `c/tests/tests.c`: `test_roster_reads_format_1`, `test_roster_decode_refuses_each_malformed_field`, `test_roster_unpack`, `test_roster_seat_refs`, and the table tests moved to the variable-length bytes.
- `server/test/unit_journal.c`, `unit_store.c`: the resident and the stored rows carry the roster's own length.

### Mutation checks (2026-11-09, after the series merged)

Harness: /Users/alex/Dev/cdom-memtest/final/mutate.py on a clone at f99698319 (merged main), results in final/out/mutate.txt and mutate_crashes.txt; every mutant was restored and touched, and clean builds before and after passed (`git diff` of the clone empty).

| mutant | what it breaks | result |
|---|---|---|
| 480-K1 | no seat id is packed | red: `a UUID id costs 17 bytes` |
| 480-K2 | a canonical UUID written as text is accepted | red: `a canonical UUID written as text is refused` |
| 480-K3 | bytes after the last seat are accepted | red: `a byte after the last seat is refused`, `unpack refuses a byte after the last seat` |
| 480-K4 | format 1's padding is not checked | red: `format 1: a byte past the title is refused` (and past an id, a name) |
| 480-K5 | `roster_seat_refs` (format 1) calls every seat human | red: `each seat's kind is the roster's` |
| 480-K6 | `roster_ref_is` takes any packed seat for any UUID | red: `a packed seat is not another UUID` |
| 480-K7 | `roster_unpack` judges the rules | red: `unpack leaves the rules to the load that made the resident` |
| 480-K8 | the packed id's dash positions swapped | red: `a packed id decodes as its text` |
| 480-S2 | the journal ignores a commit's roster bytes | red: many `unit_journal` tests (`a_move_allocates_almost_nothing` first) |
| 480-S1 | the journal's membership walk counts bot seats too (`!refs[i].bot` dropped) | survived: equivalent unless a bot's id equals a member's, which no test (and no server) seats |
| 480-S3 | `table_ops.c` drops the fallback to the loaded row's roster when a commit carries none | survived: a commit without a roster is a plain move under the journal, and `row_set_columns` already keeps the game's roster when `roster_len` is 0, so the fallback is not reached by any test; whether it is dead code (a second guard behind `row_set_columns`) is a follow-up |

Measured on a real t4g.nano on 2026-11-09 (H1 to H3 confirmed: 3,158 B per game, sat2 RSS p50 -10.1 MiB, no slowdown): /Users/alex/Dev/cdom-memtest/GRAVITON.md.

## Follow-ups

- 480-S3 above: prove the fallback in `cb_section_commit` (`np->roster = c->roster.len ? ... : row->roster`) redundant with `row_set_columns`, and keep one, or write down what each covers.

- Binary ids end to end (see "Why the ids are text"), including a format bump for the trailer, the asks and the netwire answers that old iOS builds can still read.
- The journal's state buffer is grown to TABLE_STATE_MAX (about 2.5 KB) per game, whatever the state's size: sizing it to the state is the next largest per-game item.

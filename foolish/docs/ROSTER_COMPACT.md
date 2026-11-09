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

To be filled in from the measurements.

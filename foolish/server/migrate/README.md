# Migrating the hosted Supabase data into c_base

> **Since then (2026-10-17):** the migration is done.
> The hosted data was imported into the production DynamoDB table on 2026-10-14, every client has moved to c_base, and nothing deploys to Supabase any more (RUNBOOK.md, steps 10 and 14).
> `export.sh` is kept as the record of how the data left; `cbase-import` is still built and tested (`make check`, `make check-dynamo`).

Two steps: a read-only export from the hosted project, then `cbase-import` writing it through the store interface.
The same import fills a local SQLite file (the rehearsal) and the production DynamoDB table.

## 1. Export (read-only)

```
foolish/server/migrate/export.sh            # writes foolish/server/migrate/out/
```

It needs the Supabase CLI to be logged in on this Mac: the token is read from the keychain (service "Supabase CLI", account supabase) and never printed.
Every statement is a `SELECT`, sent to the Management API's read-only query endpoint (`/v1/projects/<ref>/database/query/read-only`), so the script cannot change the hosted database.

It writes one CSV per table, and `counts.txt` with each table's row count as the database reported it:

| file | source | columns |
|---|---|---|
| `users.csv` | `auth.users` | id, email (lowercased), encrypted_password, username (`raw_user_meta_data->>'username'`), created_at, last_sign_in_at |
| `bots.csv` | `bots` | id, nickname, strategy_key, elo_rating, previous_elo, games_played, created_at, updated_at |
| `user_elo_ratings.csv` | `user_elo_ratings` | user_id, username, elo_rating, previous_elo, games_played, updated_at |
| `games.csv` | `games` | id, status, state, roster, needs_bots, game_seed, logs_packed, version, round_epoch, created_at, updated_at, last_commit_at |
| `player_hands.csv` | `player_hands` | game_id, player_id, joined_at |
| `bot_hands.csv` | `bot_hands` | game_id, bot_id |
| `game_snapshots.csv` | `game_snapshots` | id, game_id, player_ids, moves, extras, created_at |
| `chat_messages.csv` | `chat_messages` | id, game_id, user_id, message, is_system, created_at |

The dialect: RFC 4180 with a header row; NULL is an empty unquoted field and every other value is quoted; bytea is lowercase hex; every timestamp is integer milliseconds since the epoch; `player_ids` is joined by `;`.

**The export holds password hashes.**
`out/` is gitignored, created mode 0700 with files 0600.
Delete it as soon as the import has run: `rm -rf foolish/server/migrate/out`.

Refresh tokens, sessions, the view caches (`player_views`, `spectator_views`) and the bot leases are not exported: everyone signs in again once, and views are computed on demand.

## 2. Import

```
make -C c_base build/cbase-import
CBASE_STORE=sqlite CBASE_DB=/path/cbase.sqlite c_base/build/cbase-import foolish/server/migrate/out
CBASE_STORE=dynamo CBASE_TABLE=cbase            c_base/build/cbase-import foolish/server/migrate/out   # with AWS_* credentials and AWS_REGION set
```

It writes users as they are (GoTrue's bcrypt hashes unchanged, so passwords keep working), replaces the seeded bots with the hosted rows (the hosted rosters name the hosted bot ids), then ratings, games at their own versions with their seats, replays and chat.
A rerun is safe: rows are replaced, and a replay or message already present is counted as present.

It prints read, written and stored counts per table and exits non-zero unless every hash is bcrypt-shaped (`$2a$`/`$2b$`, a cost, 53 characters), every game loads through the kernel (`table_load` on its state and roster), and every count agrees with `counts.txt` (and, on SQLite, with the store's own count).

## Rehearsal, 2026-10-13

Against the real export into a scratch SQLite file: 41 users, 49 bots, 35 ratings, 60 games, 47 player seats, 44 bot seats, 144 replays and 42 chat messages, every count equal to the export's.
41 of 41 hashes parse as bcrypt and 60 of 60 games load through the kernel.
Every game's state, roster, log, seed and scalars and every replay's bytes compared equal to the CSV.
The export and the scratch database were then deleted.

Then into DynamoDB Local (`providers/aws/ops/dynamo_local.sh`, a table made from `providers/aws/ops/cbase.yaml`), seeded first with `cbase-import --seed-bots` as a deploy is: 41 users, 49 bots, 35 ratings, 67 games, 47 player seats, 47 bot seats, 144 replays and 42 chat messages, every count equal to the CSV's in the import's own check (a counting Scan per table) and in an independent `aws dynamodb scan --select COUNT`.
41 of 41 hashes parse and `cb_bcrypt_verify` refuses a wrong password on each without an error; 67 of 67 games load through the kernel; a rerun counts replays and messages as present.
The export and the table were then deleted.

# Replay format 11: the one-time transcode of stored replays (2026-10-21)

THROWAWAY.
`cbase-replay-v11` (`replay_v11.c`), its unit test (`foolish/server/test/unit_replay_v11.c`), its Makefile line, its case in `build_bootstrap.sh` and this section are deleted in the PR after the production run.

## Why

The kernel turns an Ace as the trump when an 8-seat deal leaves a stock of the four Aces, so the replay header's trump symbol is now coded over the whole deck (format 11) instead of the deck without its Aces (format 10).
The shipped kernel reads and writes 11 only.
Codes held outside the database (shared links, iMessage threads, replays saved on a phone) stop decoding; that was accepted.
The stored replays are rewritten once.

## What it rewrites, and what it does not need to

- `replays.moves` in the SQLite file `cbase serve` opens on the cbase-vm instance (`/var/lib/cbase/cbase.sqlite`): the only column production keeps replay codes in.
- Not `replays.extras` (names and gaps, no trump), `replays.fool` / `dur` or `replay_players` (summaries of the same game, unchanged), `games.state` (a kernel state blob, no code), `game_moves` (session-log records) or the move journal's segments (moves, not codes).
- iMessage FMSG bodies are not stored by the server: the unfurl route decodes them from the URL and keeps nothing.
- The DynamoDB table `cbase` has been frozen since the VM cutover (2026-10-19; the Lambda is at reserved concurrency 0 and nothing reads the table).
  It is only the rollback source until the Lambda/DynamoDB resources are deleted, and a rollback to it would also be a rollback to a kernel that reads 10, so it is left as it is.
- The hosted Supabase project is retired: nothing deploys to it or reads it (see the top of this file).

## How

A transcode, not a rebuild.
A replay outlives its session log (table.h: only a code may retire the log), so a stored game cannot be re-encoded from its moves or from its seed; the code is the only record.
The coder is mixed-radix rANS and the header's first four symbols are uniform (version 16, pass mode 2, seats - 2 7, trump), so the tool pops them with format 10's radices, maps the trump to the same card's index in the whole deck, and pushes them back with format 11's radices; the rest of the integer, the whole game, is never touched.
No v10 decoder exists anywhere for this, in the tool or the kernel.
Every row is gated by the shipped kernel: the new code decodes as 11, names the same seats, rules and trump card, the row's own fool, and rebuilds through `replay_steps_v6`.
One transaction; a dry run rolls it back; a second run finds every row at 11 and writes nothing; a row at any other version is listed and left.

Proved locally (2026-10-21), against a file the OLD kernel wrote: 41 replays from real games played through main's `cbase serve` by `cbase-bench roundtrip`, plus 84 of main's own seeded games at 2 to 8 seats, both rules, with forced openings.
125 of 125 transcoded; ids, `fool`, `dur`, `extras`, `players` and `replay_players` byte-identical before and after; every row decoded by main's `cnitro_replay` before and this branch's after prints the same game (header, names, every event); the new kernel refuses every pre-migration code; a second run wrote nothing.
The 84 seeded games transcode to exactly the bytes the new encoder cuts for the same games.
Run again with the new `cbase serve` holding the file open and finishing games while the tool ran: no error in the server's log, and the 13 replays the server wrote as 11 meanwhile were left alone.

## The production run (needs Alex's go)

Order: AFTER the CI deploy of this PR, as soon as both lanes are green (the VM's `vm_release.sh` and the site's `web_deploy.sh`).
Before it, the stored rows are 10 and the deployed kernel reads 10, so running first would break every stored replay until the deploy lands; after it, the stored rows are unreadable until the run, so the gap between the two is the window, and it should be minutes.
The history list is not affected in that window (its summaries are stored); opening an old replay is.

1. Build the tool for the instance, from the merged main: `bash c_base/providers/aws/ops/build_bootstrap.sh replay-v11` writes `c_base/dist/cbase-replay-v11-linux-arm64` and its `.sha256`.
2. Upload both to `s3://<CodeBucket>/vm/cbase-replay-v11-<short sha>` (the instance role reads `vm/*`).
3. On the instance (Session Manager), download it and check the sha256 the way `vm_boot.sh` fetches the server (the instance has no aws CLI: `curl --aws-sigv4` with the instance role's credentials, its `aws_curl`), and check there is room for one more copy of the database (`df -h /var/lib/cbase`, `ls -l /var/lib/cbase/cbase.sqlite`).
4. Dry run, as the service user: `sudo -u cbase ./cbase-replay-v11 --dry-run /var/lib/cbase/cbase.sqlite`.
   Expect every row "transcoded 10 -> 11", none failed; stop here if any failed or any row is at another version than 10 or 11.
5. Run, with its backup first: `sudo -u cbase ./cbase-replay-v11 --backup /var/lib/cbase/pre_v11.sqlite /var/lib/cbase/cbase.sqlite`.
   The backup is `VACUUM INTO` a new file, checked with `PRAGMA integrity_check` and its replay count, before anything is written.
   The service can stay up: the file is in WAL mode, the tool holds the write lock for one short transaction, and `cbase serve` waits on its busy timeout.
6. Copy the backup off the instance to `s3://<BackupBucket>/cbase/pre_v11.sqlite`, a signed PUT the same way (the instance role writes the backup bucket; the bucket also keeps 30 days of `cbase/latest.sqlite` versions, and the server's own next upload is the migrated file).
7. Verify: a second `--dry-run` reports every row "already 11"; open two or three old replays from the history list on the web.
8. Delete the binary from the instance and from S3; delete the tool from the repo in the follow-up PR; delete `pre_v11.sqlite` (instance and S3) once nobody needs a way back.

Rollback.
The data alone cannot go back while the v11 kernel serves (a v10 row is unreadable to it), so a rollback is of both:
release the previous server binary (`vm_release.sh` with the previous sha) and the previous site, then on the instance stop `cbase`, move `pre_v11.sqlite` into place as `cbase.sqlite` (owner `cbase`), remove any `-wal` and `-shm` beside it, start `cbase` and check `/health`.
Everything written between step 5 and the rollback is lost (VM_RUNBOOK.md, Backups, the same restore).
Rolling forward again after a rollback is the same run.

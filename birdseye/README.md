# birdseye

A birds-eye view of foolish: the server's 49 bots playing each other through the real server (a league at a human's pace, and tournaments), and a site that shows every live table, every bot's rating over time and the tournament bracket at a glance.
The earlier mixed mode (fake humans and some bots) is still there as `BE_MODE=mixed`.
One C process (`birdseye serve`) runs the fake humans, keeps every table as one fixed binary row, and publishes each change through c_base realtime; a Next.js site draws the tables, the numbers and a feed, with a click-through to watch one table play out.
Nothing in foolish's server changes: the fake humans are ordinary clients on `/v1` and `/ws`, speaking the kernel's own codecs.
[DESIGN.md](DESIGN.md) has the design and the measured numbers.

## Build and run

One command brings back the league and the stored tournament view:

```
birdseye/tools/run.sh            # builds, then starts the game server, birdseye and the site
open http://127.0.0.1:5811/      # the site (birdseye is http://127.0.0.1:8812, the game server 127.0.0.1:8811)
birdseye/tools/run.sh status
birdseye/tools/run.sh stop
```

Needs what c_base needs (`brew install openssl@3 sqlite` on macOS), Homebrew LLVM for structgen and the wasm (`/opt/homebrew/opt/llvm`), and `node` for the site.
`run.sh` keeps PIDs and logs under `birdseye/data/run/` and stops only what it started; the two SQLite files live under `birdseye/data/`.
The sim's size is `BE_PLAYERS` (fake humans, default 60), `BE_TABLES` (tables kept live, default half the players), `BE_THREADS`; the rest of the knobs (think time, bots, podkidnoy, hostile moves, rematches, the seed) are listed at the top of `server/src/birdseye_app.c`.
`WEB=0 birdseye/tools/run.sh` leaves the site out; `WEB_DEV=1` runs `next dev` instead of the production build.

### The league, the tournament

- The league (the default, `BE_MODE=league`): `BE_LEAGUE_TABLES` tables (40) of bots alone, made by the game server's dev-only `POST /dev/bot_table` (foolish/server/src/app/dev_bot_table.h), each bot at up to `BE_LEAGUE_PER_BOT` (4) at once, sizes 2 to 8, 30% podkidnoy, played at a human's pace by the server's dev-only `CBASE_BOT_THINK_MS=1500-5000`.
  Every finished game's rating stamp is a result in `data/league.bin`; the site's League view draws every bot's rating after every game.
- A tournament: `BE_MODE=tournament birdseye/tools/run.sh` goes on with the stored tournament, or draws a new one when the last one is finished (or none exists): 48 of the 49 bots (the lowest-rated random one sits out), seeded by the server's ratings, single elimination, two-player best of five, the 16 top seeds straight into the round of 32.
  It plays at the kernel's own bots-only pace (300 ms a move) on 16 tables and takes about a quarter of an hour; the bracket is kept in `data/tournament.bin` and every mode's site shows it under Tournament.
  Every round is a barrier: no match of the next round starts until every match of this one is decided.
  To start a new tournament while one is running, stop, remove `birdseye/data/tournament.bin`, and run the command again.
- A tournament with you in it: `BE_HUMAN=1 BE_MODE=tournament birdseye/tools/run.sh`.
  The first run writes `birdseye/data/human.env` (mode 600, never printed): `BE_HUMAN_NAME=alex` and a generated `BE_HUMAN_PASSWORD`.
  birdseye signs that account up on the local game server, adds it as one more row of the bot list (key `human`, never seated by the league), and a freshly drawn tournament gives it `BE_HUMAN_SEED` (24) in place of the bot drawn there.
  `run.sh` also starts the real foolish web UI (`next dev` from `foolish/`) at http://127.0.0.1:3811/: sign in with the name and password in `human.env`; your live game is on the dashboard and linked ("play") from your row in the bracket.
  Your games deal when you press Ready, and wait for you as long as you like (no stall limit applies to a human's table); the bots' matches go on at their own pace meanwhile, and the next round starts once yours is decided.
  A stored bracket that is still running is not redrawn: stop and delete `birdseye/data/tournament.bin` for a fresh draw with you in it.
- The gauntlet: `BE_HUMAN=1 BE_MODE=gauntlet birdseye/tools/run.sh`.
  You against one bot of each of the seven brains, one at a time, weakest first by the kernel roster's strength tiers (foolish/c/src/bot_roster.c: random, handwritten, robusta, firecracker, blackpowder, cordite, octogen); at the start of every run each brain's highest-rated bot by the server's ratings is its rung's opponent.
  Each rung is a two-player best of five in a locked lobby titled like "Gauntlet 2/7: Madrid 2 - Game 1 of 5" (the lobby's title rule allows 50 characters); each next game opens as a new table on your dashboard, eight seconds after the last one ends.
  Win a rung and you climb; lose one and the run is over, and the best run (the highest rung reached) is kept.
  A new run starts from rung 1 at the next start, or by `POST /be/gauntlet/new` with the admin token (409 while a run is going on).
  Eight seconds after each game ends birdseye has the game server delete the finished table (its dev-only `POST /dev/table_drop`, foolish/server/src/app/dev_bot_table.h: the row goes as when its last seat leaves, the replay stays, a game in play is refused), so your dashboard holds only your live lobby.
  The run is kept in `data/gauntlet.bin` (a fixed binary layout); a restart goes on with it and watches the game that was in play again.
  The site's Gauntlet view draws the ladder: each rung's opponent, rating, games and score, your current rung and a "play" link to its live lobby.
- Both routes the server needs are dev-only (refused or not routed without `CBASE_DEV=1`); the kernel is unchanged. `birdseye/tools/hotpath_proof.sh` shows the move and push path compiles to the same instructions as before them.

By hand:

```
make -C birdseye/server                   # build/birdseye, web/src/gen/*.ts, web/public/sf.wasm
cd birdseye/server && CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:8812 CBASE_DB=../data/birdseye.sqlite BE_SERVER=127.0.0.1:8811 build/birdseye serve
cd birdseye/web && npm install && NEXT_PUBLIC_BE_SERVER=http://127.0.0.1:8812 npx next dev -p 5811
```

The game server is c_base's own `cbase serve` with foolish's app, unchanged, started by `run.sh` in dev mode with throwaway secrets, its rate limits lifted (every fake human shares 127.0.0.1) and a week-long token.
Nothing here touches AWS or any real secret.

## Tests

```
make -C birdseye/server check        # every unit test and the smoke (a real game server, a real sim, a game played to its fool, the stream heard)
make -C birdseye/server check-asan   # the same under ASan + UBSan
make -C c_base build/test/smoke_dev_bot_table build/test/unit_bots_think   # the dev-only server routes and knob
```

Every test was mutation-checked when written: the rule it covers was broken, the test went red for that assertion, the rule was restored (see DESIGN.md).

## Layout

```
birdseye/server/src     wire.h (the binary surface), world.h (every table as the site sees it),
                        sim.h (the fake humans), be_metrics.h (the names the platform's registry counts),
                        routes.c, birdseye_app.c (the cb_app)
birdseye/server/wasm    sf_wasm.c: the stream's frames for the browser, over shared/c/stream_frame.h
birdseye/server/test    unit_*.c, smoke.c
birdseye/web            the Next.js site; src/gen/*.ts and public/sf.wasm are generated, never edited
birdseye/tools/run.sh   start | stop | status
birdseye/data           the two SQLite files, run logs (not in git)
```

The server links c_base's core and providers by source from `c_base/`, as adventurer does, and the kernel from `foolish/c` by its own source list, as c_base does; no file is copied and `make -C c_base check` is untouched.

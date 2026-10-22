# birdseye

A birds-eye view of foolish: a simulator that plays many games of foolish at once through the real server, and a site that shows every live table at a glance.
One C process (`birdseye serve`) runs the fake humans, keeps every table as one fixed binary row, and publishes each change through c_base realtime; a Next.js site draws the tables, the numbers and a feed, with a click-through to watch one table play out.
Nothing in foolish's server changes: the fake humans are ordinary clients on `/v1` and `/ws`, speaking the kernel's own codecs.
[DESIGN.md](DESIGN.md) has the design and the measured numbers.

## Build and run

```
birdseye/tools/run.sh            # builds, then starts the game server, birdseye and the site
open http://127.0.0.1:5811/      # the site (birdseye is http://127.0.0.1:8812, the game server 127.0.0.1:8811)
birdseye/tools/run.sh status
birdseye/tools/run.sh stop
```

Needs what c_base needs (`brew install openssl@3 sqlite` on macOS), Homebrew LLVM for structgen and the wasm (`/opt/homebrew/opt/llvm`), and `node` for the site.
`run.sh` keeps PIDs and logs under `birdseye/data/run/` and stops only what it started; the two SQLite files live under `birdseye/data/`.
The sim's size is `BE_PLAYERS` (fake humans, default 60), `BE_TABLES` (tables kept live, default half the players), `BE_THREADS`; the rest of the knobs (think time, bots, podkidnoy, hostile moves, rematches, the seed) are listed at the top of `server/src/birdseye_app.c`.
`WEB=0 birdseye/tools/run.sh` leaves the site out.

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

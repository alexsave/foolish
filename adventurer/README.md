# adventurer

wandrer.earth rebuilt as a second product on c_base, as a proof of concept: every road you travel for the first time scores, per region, per activity type, with the untraveled roads drawn live.
One C process does all of it: the road graph is a flat file mmap'd from an OpenStreetMap extract, GPS traces are map-matched by an HMM in C, coverage lives in memory as sorted segment masks, vector tiles are rendered per request straight into MVT protobuf, SQLite is only the journal each result is appended to and rebuilt from at start, and a Next.js site reads a fixed binary wire (no JSON anywhere on a shipped path).
There is no Strava: `adv-gen` plays a few thousand fake users riding, running and walking the real roads, with GPS noise, dropouts, spikes and hostile garbage, and because it knows the true path the server scores its own matcher.
[DESIGN.md](DESIGN.md) has the design and the measured numbers.

## Build and run

```
adventurer/tools/run.sh          # builds, downloads the New York extract once, imports it, starts all three
open http://127.0.0.1:5711/      # the site (the server is http://127.0.0.1:8711, the feed ws://127.0.0.1:8711/ws/public)
adventurer/tools/run.sh stop
```

One command starts the New York version: the graph of the five boroughs with Nassau, Westchester, Hudson and Bergen (graph v4, with each region's population), and 600 fake users whose homes follow where New Yorkers live, eight uploads a second, six "regulars" taking a fifth of them so a few maps grow fast enough to watch.
The generator runs far faster than life: the ops panel's "Simulated time" says how many months of riding the uploads so far stand for, at three activities a week a person.
After a change of graph, start with `FRESH=1` (the journal of another graph must not be replayed).

Needs what c_base needs (`brew install openssl@3 sqlite` on macOS; `build-essential libssl-dev libsqlite3-dev zlib1g-dev` on Ubuntu), plus `node` for the site.
`run.sh` keeps PIDs and logs under `adventurer/data/run/` and stops only what it started.

By hand:

```
make -C adventurer/server                 # build/adventurer, build/adv-import, build/adv-gen, build/adv-bench, web/src/gen/wire.ts (structgen) and the platform client (web/src/gen/cbase, web/public/cbase_client.wasm)
adventurer/tools/fetch_osm.sh             # adventurer/data/region.osm.pbf (Geofabrik; another URL as the first argument)
make -C adventurer/server graph           # adventurer/data/region.graph
cd adventurer/server && CBASE_DEV=1 CBASE_LISTEN=127.0.0.1:8711 CBASE_DB=../data/adventurer.sqlite ADV_GRAPH=../data/region.graph build/adventurer serve
cd adventurer/server && build/adv-gen ../data/region.graph --server 127.0.0.1:8711 --users 3000 --rate 5
cd adventurer/web && npm install && NEXT_PUBLIC_ADV_SERVER=http://127.0.0.1:8711 npx next dev -p 5711
```

## Tests

```
make -C adventurer/server check        # every unit test and the end-to-end smoke (a real server, uploads, tiles, a restart)
make -C adventurer/server check-asan   # the same under ASan + UBSan
make -C adventurer/server fuzz-smoke   # the PBF reader, the trace parser + matcher, the tile request (libFuzzer, Homebrew LLVM on macOS)
adventurer/server/build/adv-bench adventurer/data/region.graph 1000   # the matcher on real roads, in process: CPU per ride, recall, precision
```

## Layout

```
adventurer/server/src     the product: graph.h (the file), match.h (the matcher), coverage.h, world.h (the state),
                          journal.h (SQLite), work.h (the queue and workers), mvt.h (tiles), wire.h (the binary surface),
                          routes.c, feed.c, adventurer_app.c (the cb_app), pbf.h + osm_tags.h + import.h (the importer),
                          fake.h (the fake world the generator and its test share)
adventurer/server/tools   main_serve.c, import_main.c, bench_match.c
adventurer/server/gen     adv-gen: the HTTP client and the scheduler
adventurer/server/test    unit_*.c, smoke.c, lib_fakegraph (a synthetic street grid through the real builder)
adventurer/server/fuzz    fz_*.c
adventurer/web            the Next.js site; src/gen/wire.ts is generated, never edited
adventurer/data           downloads, the graph, the journal, run logs (not in git)
```

The server links c_base's core and providers by source from `c_base/`, exactly as `cbase` itself is built, with no file copied and nothing of foolish's kernel; `make -C c_base check` is untouched.

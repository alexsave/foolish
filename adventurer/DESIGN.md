# adventurer: design and measurements

A proof of concept, built 2026-10-21/22 on branch `adventurer`, of wandrer.earth as a second product on c_base.
Every number below was measured on this Mac (Apple M-series, 8 cores, macOS) against the Rhode Island OpenStreetMap extract, with the machine otherwise quiet unless said.
Wandrer's own stack, from their engineering blog, is Postgres + PostGIS with a 100 GB per-user segments table, tippecanoe-pregenerated per-user MBTiles archives served by nginx, untraveled tiles computed on demand in seconds and cached for five minutes, and processing delays of hours on a weekend.
Adventurer is one C process.

## The shape

One `cbase serve` process (c_base's loop, request pool, HTTP, WebSocket, SQLite store, config and CPU knob, linked by source with no file copied), with adventurer as the linked `cb_app`.
State is authoritative in memory: every user's coverage as sorted `(segment, bike mask, foot mask)` entries, per-user region progress and achievements, per-region monthly and yearly top-3 boards, and one owner word per segment for super-unique.
SQLite is an append-only journal of per-activity results (the hits the matcher found and the numbers the feed showed), written through the platform's group commit and replayed through the same apply at start; no request reads it.
The road graph is one flat file, mmap'd: segments between intersections ordered on a Hilbert curve, int32 first point and int16 shape deltas, intersection nodes with adjacency, a dense z16 cell index, regions from OSM administrative boundaries, a CRC-32C and every index checked at open.
The matcher is an HMM in C: downsample to 15 m, candidates from the cell index within 35 m, Viterbi with arithmetic transitions on the same segment, across a shared node or two hops through one, a bounded Dijkstra for the rest with its answers cached per pair for the match, an off-road state, a CPU budget per activity, one 3.4 MB scratch per thread and no allocation per match.
Tiles are rendered on request straight into MVT protobuf from the graph and the user's coverage: roads, traveled, untraveled (the inverted mask, from z11 up) and unique layers; base tiles are cached per graph; user tiles carry an ETag of the coverage version.
The wire to the web is fixed little-endian structs with a layout hash in every answer; the decoder is generated from the compiler's `offsetof` and `sizeof` (tools/wiregen.c), and no TypeScript spells a byte offset.
Uploads wait in two byte-bounded lanes, live before backfill, and matcher workers apply results under a striped per-user lock; tiles render inline on the loop thread with a try-lock, so a merge in progress sends the tile to the pool instead of stalling the loop.
A fake world (adv-gen) stands in for Strava: 3000 deterministic users of five kinds riding, running and walking the real roads with AR(1) GPS error, dropouts, spikes and a hostile upload every fiftieth; each upload carries the true coverage so the server scores its own matcher.

## The graph (Rhode Island)

| | |
|---|---|
| Source | `rhode-island-latest.osm.pbf`, 52.4 MB, Geofabrik |
| Import | 6.2 s wall, 189 MB peak RSS (two passes: ways and relations, then the 774,717 nodes they need of 5,852,755) |
| Graph file | 16.6 MB: 182,674 segments, 139,162 nodes, 746,930 shape deltas, 30,163 cells (229,775 entries) |
| Regions | 39, exactly the state's towns (admin_level 8): Providence 1274 km down to Central Falls 63 km |
| Road length | 16,159 km scoreable; 13,313 km by bike, 15,993 km on foot |

## The numbers against the estimates

| Metric | Estimated in the plan | Measured |
|---|---|---|
| CPU per ~5k-point activity | 5-20 ms | matcher alone on real roads: mean 1.56 ms, p50 1.13, p90 3.5, p99 7.0, max 9.2 (adv-bench, 1000 rides, 4425 points each); server match + merge: p50 1.5 ms, p99 20 ms (live, hostile uploads included) |
| z14 tile | 0.1-0.5 ms | user tiles over downtown Providence: p50 0.47 ms wall, max 1.24 (39 KB median, 213 KB max); rural: 0.23 ms; server-side render histogram over every zoom: p50 56 us, p99 1.5 ms |
| Upload to points visible | under 1 s | live uploads at 4.4/s: p50 4.1 ms, p99 25 ms (arrival to the feed frame, after the stats were updated) |
| RSS | | 55 MB serving 3000 users and 15,000 activities; 144 MB with 3000 uploads queued |
| Throughput | | 3000 uploads accepted in 3.1 s (1013/s; 44 refused 503 at the lane cap and retried); drained at about 250/s on 2 matcher threads with the first matcher, about 400/s with the tuned one |
| Matcher accuracy | | recall 0.980 mean (0.709 worst ride), precision 0.981 on real roads; 0.9995 / 0.996 on the synthetic grid; the live average over every scored upload 97.8% |
| Recovery | | 19,640 activities replayed from the journal in 146 ms (7.4 us each) |
| Journal | | about 1.8 KB per activity on disk |

Timings are as taken: the live figures came during a window with the load average between 2 and 4; figures taken while a parallel replay analysis held the load at 13-15 were discarded.

## Where the time goes

`sample` over a no-inline build of adv-bench (the matcher on Rhode Island), before and after the tuning pass:

| | before | after |
|---|---|---|
| gather (the candidate walk over the cells, bbox tests, dedupe) | 37% | 27% |
| local search (Dijkstra, heap, node table) | 33% | 24% |
| the Viterbi loop itself | 16% | 26% |
| projection onto shapes (point-segment distance, delta decode) | 8% | 13% |
| mercator (log, cos) for the cell of each sample | 4% | 4% |

The pass: a neighbour cell is walked only when the 35 m radius reaches into it (a cell is 600 m); a transition that skips one short segment at a crossing is priced by arithmetic (two hops through a shared node); a local search's answer for a pair of segments is cached for the rest of the match, since candidates persist for many samples at 15 m spacing.
Result: mean 2718 to 1563 us per ride, p99 14.5 to 7.0 ms, searches 2.33 M to 0.48 M per 1000 rides, recall and precision unchanged to the third decimal.
Finer cells (z17) were tried and reverted: 2% faster, a little less precise.
What is left: the cell walk is bbox tests over a few hundred listed segments per sample in a city cell, and a quarter of the searches still fail (parallel paths not joined within the cutoff) at the full expansion cap.

Inside the server the matcher is 90% of the CPU under a burst; the loop thread's own work (HTTP, the feed, the tiles) is a few percent, and the journal's group commit a few more.

## Real, simplified, and missing

Real: the OSM extract and its legal access rules, the graph, the matcher, coverage and region scoring, achievements, monthly and yearly boards, super-unique, the tiles, the journal and recovery, the feed, the site.
Simplified for the proof of concept: no accounts or Strava (the roster is posted by the generator and uploads carry a user id), one region file, the boards keyed by the activity's start month, super-unique at segment rather than sixteenth granularity, the accuracy metric from a truth trailer only a fake upload has.
Missing against Wandrer: no Garmin or Strava connector, no pretty basemap (our own grey roads over a paper background), no history of activities per user in the UI, no bonus points for achievements (the thresholds light up, no score is added), no per-user coverage of more than one region file.

## Known gaps

- A quarter of the local searches on real roads fail at the expansion cap: parallel footways not joined within 3 times the sample spacing plus 200 m; each costs about 30 expansions.
- The route cache's stored distance is covered only by adv-bench's recall staying put; the unit tests pass with it zeroed, because on a grid every transition is arithmetic.
- The worst ride in the bench matched 71% of its truth: not yet looked at.
- A rejected upload's activity id is reused after a restart (it was never journaled).
- z11 and z12 base tiles of the city core are 300-650 KB; a client cache (the ETag is the graph's CRC) makes that a one-time cost.
- `next dev` writes an AGENTS.md into web/ (gitignored); the production build is untested in this session.
- Super-unique is per segment: a sixteenth of a segment another user touched makes the whole segment shared.

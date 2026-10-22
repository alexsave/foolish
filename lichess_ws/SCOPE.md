# Scope: the lichess backend on c_base

Written 2026-10-22 from reading lila-ws f0e2e0e (2026-10-18), lila f004191 (2026-10-22) and lila-docker at the same date, all AGPL-3.0, cloned into a scratch directory and never copied into this repo.
The question Alex asked: can a c_base implementation of lichess's backend be shown faster than the real one, on the same machine and the same workload, and if so, in what order should the pieces be built.
The answer is in three parts: what lichess's backend is (section 1), the ranking of its components by the gain a c_base rewrite would bring and by how honestly that gain can be measured (section 2), and the plan for the first component, which the ranking puts first (section 3).

## 1. What the backend is

lichess is three processes that matter and a handful that do not:

- **lila** (Scala 3, Play framework, 163k lines under `modules/`, 19k under `app/`, 94k lines of TypeScript under `ui/`): every HTTP page and API, every game rule call (through scalachess, a separate library), ratings, pairing, tournaments, studies, puzzles, accounts, moderation.
  It talks to MongoDB (reactivemongo), to Redis pub/sub (for lila-ws), to Elasticsearch through lila-search, and to a set of external services (fishnet for engine analysis, lila-engine, lila-gif, lila-push, the opening explorer).
  It is one JVM process in production (one big box), with in-process asynchronous actors per game, per lobby, per tournament.
- **lila-ws** (Scala 3, 7.3k lines): the WebSocket gateway.
  Every browser socket terminates here, never in lila.
  It authenticates a socket's session cookie against MongoDB, holds the crowd of each room, the last 20 versioned events of each room and game (for reconnect resync), per-user lag estimates, the lobby's SRI set, friend lists, the eval cache, and forwards everything else to lila over Redis pub/sub as a line-oriented text protocol.
  lila never holds a socket.
- **MongoDB**: every collection (`game5`, `user4`, `security`, `tournament2`, `study`, `puzzle2_puzzle`, ...).
- **Redis**: only pub/sub between lila and lila-ws (and lila-fishnet).
- Not Scala and not in scope: lila-search (Elasticsearch), lila-openingexplorer (Rust), lila-engine (Rust), lila-gif (Rust), lila-push (Rust), fishnet (Rust, runs Stockfish), picfit.

### 1.1 lila-ws in detail (the first component)

**Endpoints** (`Router.scala`), all `GET`, all WebSocket upgrades, all taking `?sri=<12 chars>` and optionally `?v=<socket version>` and `?flag=<api|embed|simul|tournament>`:

| path | actor | what it needs before the upgrade |
|---|---|---|
| `/socket`, `/socket/v<n>`, `/analysis/socket[/v<n>]` | site | auth only |
| `/api/socket` | site, flag=api | nothing (anonymous by design) |
| `/lobby/socket[/v<n>]` | lobby | auth |
| `/play/<gameId+playerId>/v<n>` | round, as a player | auth, the game's players from `game5` (`is`, `us`, `tid`, `sid`, `iid`), troll mark |
| `/watch/<gameId>/<color>/v<n>` | round, as a watcher | auth, the game's players, troll mark, a block check in `relation` |
| `/tournament/<id>/socket/v<n>`, `/simul/<id>/socket/v<n>`, `/study/<id>/socket/v<n>`, `/challenge/<id>/socket/v<n>`, `/team/<id>`, `/swiss/<id>`, `/racer/<id>` | room actors | existence checks in their collections |

Auth (`Auth.scala`): the `lila2` cookie holds `sessionId=<id>`; `security` is read for `{_id: sid, up: true}` to get `user`; then `user4` for `{_id: user, enabled: true}`.
A Bearer token (header, `<token>:<hmac-sha1 sig>`) is looked up in `oauth2_access_token` by sha256 of the token.
`seenAt` on `user4` is updated at most once per five minutes per user.
CSRF: the `Origin` header must be absent, the configured origin, or a known app origin, else 403.
Rate limit per socket: credits per interval by endpoint (site 50/20s, lobby 30/30s, round play 100/20s, round watch 50/20s); `p` pings are free; over the limit the socket is closed with 1008.

**Client protocol** (`ClientOut.scala`, `ClientIn.scala`): text frames, JSON objects `{"t": <type>, "d": <data>}`.
Client to server: `p` (or `null`, or `{"t":"p"}`, or `{"t":"p","l":<lag ms>}`) answered by the single character `0`; `startWatching` (space-separated game ids, up to 16, answered by `fen` pushes), `moveLat`, `notified`, `following_onlines`, `evalGet`/`evalPut`/`evalGetMulti`, `msgType`, `msgSend`/`msgRead` (forwarded to lila as `tell/user`), anything else on a site socket forwarded as `tell/sri`.
Lobby: `idle`, `join` (gated by anonymous flood limits), `cancel`/`joinSeek`/`cancelSeek`/`poolIn`/`poolOut`/`hookIn`/`hookOut`, all forwarded as `tell/sri` on the lobby channel.
Round: `move` (`d.u` the UCI, `d.b` blur, `d.l` client lag ms, `d.s` move time in base 36 centis, `d.a` an ack id answered by `{"t":"ack","d":<id>}` plus a WebSocket PING frame carrying the 8-byte wall clock, whose PONG measures the frame lag), `drop`, `hold`, `berserk`, `rep`, `flag`, `bye2`, and the player actions `resign`, `abort`, `draw-yes`, `takeback-yes`, `moretime`, `rematch-yes`, `outoftime`, ... forwarded whole as `r/do`.
Chat: `talk`, `timeout`.
Server to client: `0` (pong), `{"t":"n","d":<members>,"r":<rounds>}` (the lobby's pong), `{"t":"resync"}`, versioned events `{"t":<type>,"v":<version>,"d":<data>}` or the skip form `{"v":<version>}` when the event is not for this client, `{"t":"crowd","d":{"white":true,"black":true,"watchers":{"nb":N}}}`, `{"t":"fen","d":{...}}`, `{"t":"finish",...}`, `{"t":"redirect","d":{"id":<fullId>,"url":"/<fullId>"}}`, `{"t":"mlat","d":<ms>}`, `{"t":"gone","d":bool}`, `{"t":"goneIn","d":<s>}`, `{"t":"batch","d":[...]}`, `{"t":"ack"}`.
A socket silent for more than 30 s (no ping) is closed by the broom that runs every 7.2 s.

**Redis protocol** (`Lila.scala`, `LilaIn.scala`, `LilaOut.scala`): plain pub/sub, one channel pair per domain: `site-in`/`site-out`, `lobby-in`/`lobby-out`, `tour-`, `simul-`, `team-`, `swiss-`, `study-`, `chal-`, `racer-`, and the round channels `r-in:<0..15>` (lila-ws publishes to the lane `|hashCode(message)| % 16`, Java's `String.hashCode`) and `r-out`, `r-out:<0..16>` (lila publishes sticky by game id).
Messages are text lines, `<path> <space-separated args>`, with `-` for an absent value, `+`/`-` for booleans, commas for lists, and a JSON payload as the last argument where there is one.
lila-ws to lila (`LilaIn`): `tell/sri <sri> <user|-> <json>`, `tell/user <user> <json>`, `notified/batch <users>`, `lags <user:ms,...>`, `connect/user <user>`, `disconnect/users <users>`, `boot`, `ping <uptime ms>`, `connect/sris <sri user,...>`, `disconnect/sris <sris>`, `counters <members> <rounds>`, `room/alives <ids>`, `chat/say <room> <user> <text>`, `chat/say/w ...`, `chat/timeout ...`, `room/versions <id:v,...>`, `r/do <fullId> <json>`, `r/move <fullId> <uci> <blur +/-> <lag centis|-> <movetime centis|-> <frame lag centis|->`, `r/berserk`, `r/hold`, `r/report`, `r/flag <gameId> <w|b> <playerId|->`, `r/bye <fullId>`, `r/ons <gameId+-,...>` (which colors are online, batched every 700 ms), `r/latency <ms>`, `r/get <reqId> <id> <user|->`, `req/response <reqId> <value>`.
lila to lila-ws (`LilaOut`): `mlat <ms>`, `tell/flag`, `tell/users <users> <json>`, `tell/sri`, `tell/sris`, `tell/all`, `disconnect/user`, `lobby/pairings <sri:fullId,...>`, `tell/lobby <json>`, `tell/lobby/active <json>`, `tell/lobby/users <users> <json>`, `mod/troll/set`, `mod/impersonate`, `rel/follow`, `rel/unfollow`, `tell/room <room> <json>`, `tell/room/version <room> <v> <troll +/-> <json>`, `tell/room/user`, `tell/room/users`, `tell/room/chat`, `room/stop <room>`, `room/present`, `room/filter-present`, `req/response`, `tour/get/waiting`, `r/ver <gameId> <v> <flags> <type> <json>` (flags: `s` watchers only, `p` owners only, `w`/`b` one color only, `W`/`B` moved by that color, which strips `dests` for the mover, `t` troll, `-` none), `r/tour/standing`, `r/resync/player <fullId>`, `r/gone <fullId> <+/->`, `r/goneIn <fullId> <s>`, `r/bot/online`, `r/start <users>`, `r/finish <gameId> <w|b|-> <users>`, `tv/select`, `racer/state`, `api/online`, `streamers/online <json>`, `pong <uptime ms>`, `announce/update`, `boot`, `lila/stop <reqId>`, `r/versioning-ready`.
At its own boot lila-ws publishes `boot` on every `-in` channel; when lila boots it publishes `boot` on `r-out`, lila-ws answers with `room/versions` listing every game's last known version so lila can continue the numbering, and lila answers `r/versioning-ready`.

**State lila-ws holds**: connections by user (`Users`), the crowd per room and per game with the online colors (`RoomCrowd`, `RoundCrowd`, published to lila as `r/ons` batches and to clients as `crowd` events at most every 700 ms / 1 s), the last 20 versioned events per room and per game (`History`, 20 x up to 65k games), watched-game FEN subscriptions (`Fens`, which parses each move event's JSON with a regex to extract uci, fen and clocks), per-user trusted lag (an EWMA of PING/PONG frame round trips, capped at 5 s) and client-reported lags batched to lila every ~1 s, the lobby's SRI connect/disconnect batches, pending-game anti-flood counters per IP, the friend list / social graph, the eval cache (read-through to MongoDB), impersonations, the announce, TV channels.
Three timers: broom every 7.2 s, lobby counters every 1.2 s, `ping` to lila every 1.9 s on site and round.

**Deployment**: one JVM (Temurin 25) per box, Netty with native epoll, the flush queue (1 ms interval, 500 channels per step) batching writes, Kamon metrics.
lichess runs a few instances behind nginx, each holding tens of thousands of sockets.

### 1.2 lila, module by module

Sizes are Scala lines under `modules/<name>` at f004191.

| module | lines | what it does | depends on |
|---|---|---|---|
| tournament | 9430 | arena tournaments: scheduling, pairing every few seconds, standings, Mongo `tournament2/_player/_pairing` | round, game, user, rating, socket |
| relay | 9301 | broadcast relays (PGN ingestion from external sources into studies) | study |
| study | 8885 | collaborative analysis trees, chapters, Mongo `study`, `study_chapter_flat` | tree, socket, chat |
| security | 7656 | sessions, login, signup, password hashing, 2FA, IP and email intelligence, firewall, garbage collection of accounts | user, mailer, oauth |
| web | 6306 | page layout, shared UI (scalatags) | everything |
| fishnet | 5832 | engine analysis job queue for external Stockfish workers | game, tree |
| game | 5304 | the `Game` model, BSON codecs (`BinaryFormat`: moves, clocks, castles as bytes), repository, PGN export, crosstable | core, db |
| core | 4408 | shared types and interfaces | - |
| round | 4170 | live game play: `RoundAsyncActor` per game (in memory, flushes to Mongo on a schedule), `MovePlayer`, clocks, draws, takebacks, resign, abort, berserk, moretime, `Finisher` (ratings, history, perf stats), socket protocol | game, rating, chat, socket, user |
| plan | 4135 | patron payments (Stripe, PayPal) | third party |
| swiss | 4134 | Swiss tournaments (bbpPairings, an external binary) | round, game |
| mod | 4103 | moderation tools | everything |
| user | 4020 | the user model, `user4` BSON, perfs, profile | rating |
| puzzle | 3713 | puzzle selection (`$sample` aggregations on `puzzle2_puzzle` by theme and rating path), puzzle ratings (Glicko), streaks, storm | rating, tree |
| api | 3597 | the public REST API and NDJSON streams | game, user, round |
| coreI18n | 3463 | translation keys | - |
| team | 3258 | teams | user |
| clas | 2963 | classes for teachers | user |
| tutor | 2916 | tutor (aggregated insight reports) | insight |
| gathering | 2855 | shared tournament/swiss code | - |
| insight | 2804 | per-move insight statistics | game |
| pref | 2534 | user preferences | user |
| ublog | 2462 | user blogs | user |
| ui | 2439 | shared view helpers | - |
| forum | 2407 | forum | user, team |
| streamer | 2201 | streamer pages, Twitch/YouTube polling | third party |
| challenge | 2019 | challenges (direct game invitations), open challenges, AI challenges | game, round, setup |
| report | 1909 | user reports | mod |
| fide | 1849 | FIDE player database | - |
| simul | 1837 | simultaneous exhibitions | round |
| common | 1788 | utilities | - |
| oauth | 1636 | OAuth2 server | user |
| appeal | 1483 | appeals | mod |
| opening | 1460 | opening explorer pages | explorer (Rust) |
| activity | 1457 | user activity timeline | game, puzzle |
| memo | 1437 | caches | - |
| lobby | 1423 | hooks (anonymous or rated quick pairing), seeks (correspondence), the sync actor that matches them on a tick, pool pairing hand-off | game, setup, socket |
| analyse | 1352 | analysis board pages | tree |
| msg | 1296 | private messages | user |
| db | 1291 | Mongo plumbing | - |
| tree | 1279 | analysis tree model | - |
| setup | 1254 | game creation forms and `Processor`: makes the `Game` document for a hook, seek, challenge or AI game | game, lobby |
| video | 1132 | video library | - |
| perfStat | 1061 | per-perf statistics | game |
| push | 1021 | mobile push | third party |
| irwin | 1019 | cheat detection client | third party |
| chat | 1006 | chat lines and timeouts | user |
| mon | 991 | metrics | - |
| shutup | 949 | chat moderation | - |
| coach | 907 | coach profiles | - |
| rating | 871 | Glicko-2 wrapper around scalachess-rating, perf types, rating ranges, `RatingRegulator` | scalachess |
| tv | 808 | TV channel selection | round |
| i18n | 787 | translation loading | - |
| relation | 746 | follows and blocks | user |
| gameSearch | 729 | game search (lila-search) | search |
| title | 714 | title verification | - |
| notify | 694 | notifications | user |
| storm | 681 | puzzle storm | puzzle |
| playban | 641 | play bans | round |
| evaluation | 613 | player evaluation | - |
| bot | 593 | bot API (board/bot streams) | round, api |
| markdown | 579 | markdown rendering | - |
| mailer | 575 | email | third party |
| practice | 565 | practice | study |
| event | 554 | site events | - |
| pool | 498 | pool pairing (the quick pairing pools: a tick that pairs by rating range) | lobby, round |
| recap | 470 | yearly recap | - |
| racer | 466 | puzzle racer | puzzle |
| timeline | 440 | timeline | user |
| irc | 439 | zulip/irc notifications | third party |
| cms | 416 | pages | - |
| feed | 382 | feed | - |
| socket | 375 | `RemoteSocket`: the Redis side of the lila-ws protocol, the online user set | - |
| history | 302 | rating history | user |
| jsBot | 278 | js bots | - |
| evalCache | 182 | eval cache writes | - |
| learn, coordinate, room, bookmark, search, studySearch, explorer, forumSearch, teamSearch | < 200 each | small | - |

Three libraries outside lila carry rules: **scalachess** (board, move generation, SAN, FEN, variants, clocks, Glicko-2 in `scalachess-rating`), **scalalib** (actors, caches, utilities), **bbpPairings** (a C++ binary for Swiss pairing).

### 1.3 Storage

MongoDB, one database, the collections named in the table above.
Games are `game5`: an 8-character id, the two player ids (`is`, 4 characters each), user ids `us`, the move list as `hp` (Huffman-coded, `BinaryFormat`), clock state as bytes, status, variant, ratings and rating diffs, PGN metadata, and dozens of optional fields.
Indexes include `us` with `ca` (created at) for a user's game list, `ra` (rated), `s` (status) for ongoing games, and more (`bin/mongodb/indexes.js`).
Users are `user4` with embedded `perfs` (one Glicko triple plus a recent-games count per perf type).
Sessions are `security` (`_id` = session id, `user`, `up`, `date`, IP, UA, fingerprint).

Realistic data at scale is public: `database.lichess.org` publishes every rated game ever played as monthly PGN dumps (a recent month is about 100M games, 30 GB compressed; a 2013 month is about 120k games, a few MB), plus the puzzle database (5M puzzles, CSV) and the evaluation database.
A month from 2013 or 2014 is the right import for history, profiles and export tests on this Mac; a recent month would need the full box.

### 1.4 The frontend

`ui/` builds to `public/compiled/*.js` and is served statically by Caddy or nginx.
The pages are server-rendered by lila (scalatags) and embed each page's data as a JSON blob the module reads (`site.load` and the module's `lichess.load`), so "unmodified frontend against our server" means our server must produce the page HTML with the same embedded JSON for every page we claim, and answer the same XHR and WebSocket calls.
The mono Docker image (`ghcr.io/lichess-org/lila-docker:main`, linux/arm64 available) ships the built `public/`, the staged lila and lila-ws JVM apps, a seeded MongoDB, Redis and Caddy under supervisord, which gives a real lila to race and to validate against without compiling it on this Mac.

## 2. The ranking

Alex's rule: build in the order of the gain a c_base rewrite would bring (CPU, latency p50 and p99, memory, throughput), weighted by how measurable a head-to-head race is.
A component that can be raced alone against the real one, behind its existing protocol, beats one whose gain only shows end to end.

The estimates below are reasoned from what each component does, not measured; the race tables in DESIGN.md replace them one by one.
"Isolatable" says whether the real component can be unplugged and ours plugged in behind the same wire, without touching the rest.

### 2.1 Per component

**lila-ws (the WebSocket gateway)**.
What it does per message: Netty decodes the frame, `play-json` parses the text into a `JsObject` tree, pattern matching builds a case class, a Pekko actor mailbox hop delivers it to the connection's actor, which formats a Redis line by string interpolation and publishes it through Lettuce (another thread hop); every event from lila is parsed, stored in history (an immutable list per game, copied on add), and fanned out by an event bus to each subscriber actor, each of which lazily builds the versioned payload string and hands it to Netty's flush queue.
Memory per connection: a Netty channel with its pipeline, a typed actor with its state case classes, bus subscriptions in several hash maps, the JVM object headers on all of it: 10 to 30 KB; plus the JVM's heap headroom.
c_base: an idle connection owns no buffers (the loop reads into one scratch), a message is parsed by a hand-written scanner over the 3 or 4 keys the protocol uses, the Redis line is written into a per-loop buffer and sent on the loop thread, fan-out writes one preformatted frame into each subscriber's outgoing buffer, and history is a 20-slot ring per game.
Expected: CPU per message 5 to 20x lower; memory per connection 20 to 50x lower (a few hundred bytes plus the kernel's socket); p50 move relay latency lower by the actor and flush-queue hops (the flush queue alone adds up to 1 ms by design); p99 much lower (no GC, no thread hand-offs); throughput bounded by the kernel's send path, so 3 to 10x more messages per second per core.
Cost: medium (about 4k lines of C for the subset in section 3, including a Redis client and a MongoDB client).
Isolatable: **yes, completely**: the same WebSocket URLs and messages toward the browser, the same Redis lines toward lila, the same MongoDB reads.
Measurable alone: **yes**: a load generator drives both through the identical protocol with a fake lila on Redis, or with the real lila.

**round (live game play)**.
What it does per move: lila-ws's `r/move` line arrives on a Redis lane, is parsed, handed to the game's `RoundAsyncActor` (an in-memory game held by `GameProxy`), `MovePlayer` applies the UCI through scalachess (immutable board and history, `destinations` computed for every legal move with bitboards, SAN generation, threefold and fifty-move checks), the clock is updated, the `Event.Move` JSON is built (with `dests` as a map of squares to strings), the game document is marked dirty and flushed to Mongo on a schedule (immediately when the game ends), `r/ver` is published, and at the end `Finisher` runs Glicko-2 for both players, updates `user4` perfs, `history`, perf stats, activity, timeline, the crosstable and the playbans.
scalachess is allocation-heavy by design (every move is a new `Position` with new bitboard records and a new history list) and lila wraps each step in futures on an executor.
Expected: CPU per move 10 to 30x lower (a C bitboard move generator plus SAN and FEN is about a microsecond; the JSON event is a few hundred bytes written once); latency p50 and p99 lower by the same; memory per live game 5 to 10x lower (lila holds the game, its proxy, its actor, scalachess's move history and a promise chain); throughput several times higher.
Cost: high: chess rules in C (standard chess first: move generation, legality, check, mate, stalemate, castling, en passant, promotion, SAN, FEN, repetition, fifty moves, insufficient material; the variants later), clocks with lag compensation and berserk, the draw/takeback/abort/resign/flag state machine, the game document and its persistence, Glicko-2 matching scalachess-rating exactly, and the move event JSON byte for byte, because the browser's round module reads it.
Isolatable: **partly**: behind lila-ws it is a Redis protocol (`r-in`/`r-out`), and our server can take those lanes while real lila keeps everything else; but the game must exist in both worlds (lila's lobby writes the `game5` document; ours must read the same document to start, and write its result back in lila's BSON format for the profile, the game page and the export to see it).
Measurable alone: **yes, for the move path**: a driver publishes `r/move` and measures `r/ver` on the same Redis against real lila and against ours, for games created the same way.
The finish path (ratings written to `user4`) is measurable by a differential: the same games, the same Glicko inputs, the same numbers.

**game storage and export** (`game`, `api` streams, `perfStat`, `history`, the profile's numbers).
What it does: Mongo reads of `game5`, `BinaryFormat` decoding (moves, clocks, castling rights), PGN generation with scalachess replaying the moves for SAN, NDJSON streams of a user's games, crosstables, the profile page's counts.
Expected: throughput of `/api/games/user/<u>` and `/game/export/<id>` 10 to 50x higher per core (lila's export streams are CPU-bound on move replay and JSON, and each game is a Mongo round trip); storage size 2 to 4x smaller with a fixed layout; profile page latency lower; memory lower.
Cost: medium: a PGN reader (needed for the import anyway), a compact game record, a PGN writer, the user's game list index, the JSON shapes of the API.
Isolatable: **yes, read side**: the HTTP paths are ours to answer with the same bytes from the same imported data, in front of real lila for everything else.
Measurable alone: **yes**: import the same month into Mongo (lila's format, through lila's own importer or `lila-db-seed`) and into ours, then race the same request list.

**puzzle** (`puzzle`, `storm`, `racer`).
What it does: next-puzzle selection is a Mongo aggregation (`$sample` within a rating-and-theme path), then a round trip per attempt to update the puzzle's and the user's Glicko.
Expected: next-puzzle latency 10x lower with an in-memory index over the 5M-puzzle CSV; CPU lower.
Cost: medium (the CSV import, a theme and rating index, the puzzle round JSON, Glicko for puzzles).
Isolatable: **yes**, over HTTP.
Measurable alone: yes.
Ranked below game storage because the gain is a single endpoint's latency and the data path is simpler to compare.

**lobby and pool pairing** (`lobby`, `pool`, `setup`).
What it does: a sync actor ticks every ~2 s over a few thousand hooks, matches compatible ones, creates games (through `setup.Processor` and `game5` inserts), tells lila-ws to redirect both SRIs.
Expected: CPU per tick 10x lower, but the tick is small to begin with; latency of a pairing is dominated by the tick period, not the work; memory small.
Cost: medium (hook compatibility rules, the rating range logic, the game creation path which is shared with round).
Isolatable: partly (`lobby-in`/`lobby-out` on Redis plus the `/setup/hook` HTTP form).
Measurable alone: weakly: the numbers are small and the pairing latency is a tick.
Needed anyway as the game creation path once round is built.

**tournament (arena)** and **swiss**.
What it does: a pairing pass every few seconds over every waiting player of every running tournament (thousands of players in the big ones), standings recomputed and cached, Mongo `tournament_player` and `_pairing` writes, `tourStanding` pushes through lila-ws.
Expected: CPU of the pairing pass 10 to 30x lower; standings latency lower.
Cost: high (9.4k lines, plus round and lobby as prerequisites).
Isolatable: **no**: the pairing creates games through round and reads players through user; it cannot be swapped alone.
Measurable alone: only by a synthetic pairing benchmark, which is not a race against the real thing.

**security, user, oauth** (accounts, sessions, login).
What it does: bcrypt (equal in both), Mongo reads per request (cached), signup with email confirmation, 2FA, IP intelligence.
Expected: small gain per request (bcrypt dominates login; session reads are cached in lila); memory lower.
Cost: medium; moderation-heavy and third-party-heavy parts out of scope.
Isolatable: no (every page depends on the session).
Needed as a dependency of the playable core, not for its own gain.

**study, relay, analyse, tree**.
Large documents, collaborative editing over the socket, a tree model: a high cost rewrite with moderate gain; not isolatable without the socket side (which is lila-ws, done) and the study pages.
Out of scope.

**fishnet, evalCache, analysis**.
A job queue for external workers; faithful stub possible (a local Stockfish answering the same HTTP as fishnet does), but the lila side has little CPU to win.
Out of scope.

**mod, report, appeal, shutup, irwin, playban, plan, mailer, push, irc, streamer, video, cms, title, fide, clas, coach, ublog, forum, msg, notify, team, timeline, activity, insight, tutor, recap**.
Moderation, payments, third-party integrations and content CRUD: low traffic, low gain, and either moderation-heavy or third-party-heavy, which Alex ruled out.
Out of scope.

**search, explorer, engine, gif, push (the Rust services)**.
Not Scala; out of scope.

### 2.2 The order

| rank | component | expected gain | measurable alone | cost | why here |
|---|---|---|---|---|---|
| 1 | lila-ws | high on every axis, especially memory per connection and p99 | yes, perfectly | medium | the only component with its own process and wire; the cleanest head-to-head there is |
| 2 | round: moves, clocks, finish, Glicko-2 | highest CPU gain in lila (scalachess allocation, event JSON, future chains) | yes for the move path through Redis; differential for ratings | high | the hot path of the whole site; its wire is the one lila-ws already speaks |
| 3 | game storage, export and profile numbers | high on throughput and storage size | yes, over HTTP, with public data at scale | medium | lila's export streams are its heaviest HTTP; the PGN reader is needed for the import anyway |
| 4 | puzzle API | high on one endpoint's latency | yes, over HTTP | medium | public data; a clean race |
| 5 | lobby and pool pairing, setup | low to medium | weakly | medium | needed as round's game creation path; its own gain is small |
| 6 | accounts and sessions | low | no | medium | a dependency of the playable core, not a win |
| 7 | tournament, swiss | high at scale | no | high | cannot be swapped alone; after 2 and 5 exist |
| out | study, relay, analyse, fishnet, mod and everything moderation- or third-party-heavy, the Rust services | - | - | - | see 2.1 |

So lila-ws stays first by this ranking too, and the sequence is lila-ws, round, game storage and export, puzzle, lobby, accounts, tournament, each one racing the real component on the same machine before the next starts.

## 3. Milestone 1: lila-ws

### 3.1 The subset

A drop-in for the sockets lichess serves at volume, exact on the wire:

- The site socket (`/socket`, `/socket/v5`, `/analysis/socket`), the API socket (`/api/socket`), the lobby socket (`/lobby/socket`), the round sockets (`/play/<fullId>/v<n>`, `/watch/<gameId>/<color>/v<n>`), with the same query parameters, cookie auth against `security` and `user4`, CSRF origin check, the per-endpoint rate limits and the 30 s broom.
- Every client message of those four sockets parsed and acted on exactly as listed in 1.1: pings answered with `0` (the lobby's with `n`), `startWatching` with `fen` pushes, `moveLat` with `mlat`, `move` (with ack and the PING frame lag probe), `drop`, the `r/do` actions, `flag`, `bye2`, `berserk`, `hold`, `rep`, `talk`, `idle`, `join` and the lobby forwards, and the site forwards (`tell/sri`, `tell/user`).
- Every Redis line in both directions that those sockets produce or consume: the full `site`, `lobby` and `r` channel vocabularies, the `boot` handshake, `room/versions`, `ping`/`pong`, `r/ons`, `lags`, `connect/user`/`disconnect/users` (batched every 5 s), `connect/sris`/`disconnect/sris` (batched), `counters`, and the round-robin lane hash (`String.hashCode`, matching lila's).
- Versioned round history: 20 events per game, the resync rule (a client at version `v` gets exactly the events after `v` when they are all in history, else `resync`), the per-client filtering by flags (`skip`, `noDests`, `full`), the batch form for room events.
- Crowds: room and round crowds with the 700 ms / 1 s batching and the "only send when it changed, and only at round numbers above 100 members" rule, `r/ons` to lila.
- Users online, `tell/users`, `tell/sri`, `tell/all`, `tell/flag`, `disconnect/user`, `lobby/pairings` redirects, `tell/lobby[/active]`, `mlat`, `r/resync/player`, `r/gone`, `r/goneIn`, `r/start`, `r/finish` (with the `fen` watchers' `finish`), `room/stop`, `lila/stop` with `serverRestart` to players and the critical-message buffer while lila is away.
- Trusted lag (EWMA of PING/PONG), client-reported lags, `r/latency`.

Left out, each with the reason:

- Tournament, simul, study, team, swiss, challenge, racer rooms: the same room machinery (`tell/room/version`, crowd, chat) as the round socket, with existence checks in their collections; they add vocabulary, not load. The room code is written so they are an endpoint table entry each when wanted.
- Eval cache (`evalGet`/`evalPut`, MongoDB `eval_cache2` reads and writes): a cache with its own BSON format; a separate feature.
- Friend list and social graph (`following_onlines`), streamers, announce, TV channels, storm signing, the mobile app SRI chain, impersonation, inquirers, Prometheus: features whose cost is not in the hot path.
- OAuth bearer auth and `seenAt` writes: the session cookie is what browsers use; bearer is for the mobile app. `seenAt` is a write per user per 5 minutes; cheap to add, not in the race.

### 3.2 The implementation shape

A second product on c_base, as adventurer is: `lichess_ws/server/` with its own Makefile compiling c_base's `core/src` and `providers/local/src` by source, plus `lichess_ws/server/src/*.c`.
The app registers its WebSocket routes as request-pool routes (the MongoDB lookups happen there, blocking only a pool thread), and completes the upgrade with `cb_ws_upgrade`, which is pure, so the loop thread takes the connection over once the 101 is written (`net/server.c finish`).
Everything after the upgrade runs on the loop thread: frame parsing, the Redis publish, fan-out to subscribers, timers.
The Redis client is a small RESP2 client over `cb_loop` (one publishing connection, one subscribing connection per channel group, auto-resubscribe), and the MongoDB client a minimal OP_MSG client (`find` with filter, projection and limit 1; `findAndModify` for `seenAt` if added), with one blocking connection per pool worker.
JSON is confined to `json_wire.c` (the client messages read, the client messages written); the Redis lines to `lila_wire.c`.
No other file sees a JSON byte.

### 3.3 The race

Same machine (this Mac, Apple M1, 8 cores, 16 GB), both servers native (no Docker between the client and the server): the real lila-ws from the staged JVM app inside `ghcr.io/lichess-org/lila-docker:main` (the same jars lichess ships), on Temurin or OpenJDK 21 with its default flags plus the heap it needs, warmed up before every measurement; ours as a native arm64 binary.
Same Redis (`redis-server` from Homebrew on port 9679), same MongoDB (`mongo:7.0` in Docker on 27117, used only at connect time by both), same seeded data (sessions, users, games).
One load generator in C (`lichess_ws/server/tools/wsbench.c`), driving either server through the identical protocol, with a fake lila on Redis built into it: N round sockets (players and watchers) over M games, each player sending `move` every T ms, the fake lila answering each `r/move` with an `r/ver` move event of realistic size (with `dests`, `fen`, `clock`), plus site sockets pinging every 2 s, plus lobby sockets.
Measured: end-to-end move relay latency from the player's `move` frame to the watcher's `r/ver` frame (p50, p99, p999), messages per second sustained, connections held, RSS of the server process, CPU time of the server per message (from `rusage`), and connect rate.
The load average is recorded next to every number, and a run under load from the other agents on this Mac is rerun.
The drop-in is validated by running our binary inside the mono container in place of its lila-ws (same Caddy, same Mongo, same Redis, real lila), and playing a game in a browser.

Where lila-ws is expected to win or tie: nothing on this list, honestly; the JVM's JIT makes string formatting fast and its epoll path is native, so the CPU gap per message may be smaller than the memory gap.
The race decides.

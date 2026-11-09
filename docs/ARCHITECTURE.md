# The architecture: cdom, cbase and the apps on them

This document describes the whole stack Alex's apps are built on, end to end, in one place.
It was written jointly on 2026-11-09 by the agents that own each part: cdom (the page), cbase (the server and its clients), amgi (the first app on both) and foolish (the assembly, and the incident section).
Each section names its owner; a change to a section goes past that owner.
For the same ideas worked through one specific product, the Durak card game, see `ARCHITECTURE_AS_A_PATTERN.md` beside this file.

Paths are written `repo: path`, for example `cbase: core/include/cbase/app.h` or `cdom: src/cdom.h`.
The repositories are github.com/alexsave/cdom, github.com/alexsave/cbase and github.com/alexsave/amgi; foolish is this repository.

## 1. What this is

This is a way to build apps where the logic is C from end to end: the server, the client's protocol, and the UI.
Three repositories make it up, and each also works alone.
cdom is the UI: an app written in C owns the browser's DOM from WebAssembly.
cbase is the backend: a C server you run anywhere, plus client SDKs (C, TypeScript, Swift) over one C protocol core.
An app such as amgi uses both.

### The layers

On the client, the page loads one WebAssembly module, made of the app's C, cbase's C client SDK (when the app talks to a cbase server) and cdom's C.
Beside it sits a fixed JavaScript shim (cdom's `cdom.min.js`), which is the module's only way to reach the browser: one import per DOM operation, and events posted back.
Think of the shim as the system calls of the page: the C asks, the shim does the browser's part, and nothing else in JavaScript knows the app.

On the server, the app's routes run on cbase, which runs on the program's libc, OpenSSL and SQLite, on the operating system.

### The dependency rule

There are no cycles.
cbase may depend on cdom, and only in one place: its browser adapter (`client/hosts/cdom`), which runs cbase's C client on cdom's network, timer and store calls.
cdom never depends on cbase, and a gate in each repo enforces its side.
cdom keeps the calls that adapter uses stable: they only ever change additively, and a test fails to compile if one changes in place.

### Each repo stands alone

With cdom alone you can build a static site: an html page, the shim, CSS and a wasm module, served by any web server as plain GETs.
With cbase alone you can serve a Swift or TypeScript client, with no browser C at all.

## 2. cdom

cdom lets C own the DOM.
The tree, the state and the keyed diff live in C, in WebAssembly linear memory.
A fixed shim applies what C decides, through one import per DOM operation: there is no virtual DOM in JavaScript, and no per-app JavaScript at all.

### The API

The API is `src/cdom.h`: open and close elements, set text, attributes, classes and styles, name a node to change it later (`cd_name_el`, `cd_name_text`), and handle events with `cd_on(id, event, fn, ctx)`.
Platform modules sit beside it, each an opt-in source file: net (HTTP and WebSockets, `cdom_net.h`), store (IndexedDB, `cdom_store.h`), random, audio, url, locale, share and fonts.
Asynchronous calls take a callback and a context (`cd_http_cb`, `cd_ws_open_cb`, `cd_timer_cb`, `cd_store_get_cb`), and cdom routes each answer to its caller.
A program's libc comes from cdom: `src/libc/string.h` maps memcpy, memmove and memset to the compiler's bulk-memory builtins, and `src/mini.c` gives the rest a program needs.

### Building and starting an app

An app's Makefile includes `cdom.mk`, which provides:
- one flag set (`CDOM_CFLAGS`, `CDOM_LDFLAGS`), the same one cdom's own tests build with;
- the sources (`CDOM_SRC`, plus one `CDOM_<NAME>_SRC` per module);
- `APP_WASM`, a rule that builds the module from `APP_SRC`, `APP_MODULES` and the app's own `APP_CFLAGS` and `APP_LDFLAGS`;
- `APP_PAGE`, a rule that inlines the minified shim where the page says `/*CDOM_SHIM*/`.
No Node or npm is needed to build or run an app.
The page starts the app with exactly one call, `cdom.start(url, mount, env, options)`, documented in cdom's README ("Starting an app").

### What it costs

The minified shim is about 15 KB, or about 5.8 KB with brotli.
A real app's module, amgi's UI, is about 72 KB, or about 30 KB with brotli.
A click event costs about 7.6 microseconds from the browser to C and back.

### Old browsers

A browser without WebAssembly can run the same app through wasm2js on an ES5 shim.
This is opt-in: `cdom.mk` never builds it.
An app that wants it starts through `shim/boot.js` and builds the fallback with `tools/legacy/build.mjs`, the only step that needs Node.

## 3. cbase, the server

cbase is a backend platform in C that a product links into its own server binary.
It is taken a la carte: a product links only the parts it needs, and build gates prove that nothing else got in.
`cbase: README.md` is the entry point, `cbase: docs/PLATFORM.md` is the app API, and `cbase: RUNBOOK.md` covers operating a product.

### Serve, products and routes

`cbase serve` is the server.
It is the product's own binary, built from the core plus a provider plus the product's code, and it is started as `serve`.
Once the socket is bound it prints exactly one stdout line, `cbase: listening 127.0.0.1:3000`, and logs to stderr.
`CBASE_*` environment variables are its settings (`cbase: core/src/config.h`).

A product defines one const `cb_app` and points `cb_main_app` at it (`cbase: core/include/cbase/app.h`).
The struct holds the routes, CORS origins, jobs, schedules, admin panels and the store's rows.
A route is a `cb_route` with a `method`, a `pattern`, `flags` and a handler `fn`.
A pattern is exact, a prefix (`/v1/items/*`) or has path parameters (`/decks/:id`).
The handler signature is `void (*cb_handler)(cb_ctx *ctx, const cb_http_req *req, cb_http_res *res)` (`cbase: core/src/net/router.h`).
Handlers run on the request pool by default (`CB_ROUTE_POOL`), or on the loop thread (`CB_ROUTE_LOOP`), which must never block.
`cbase: example/src/example_app.c` is the template to copy.

### Storeless and stored, providers

A stored app uses the platform's store (`cbase: core/src/store/store.h`) for accounts, sessions and its own rows.
A storeless app sets `.storeless = true`, opens no platform store, and opens its own data in the `serve_open` hook.
`cbase: example/storeless/` shows it.
The provider is chosen at link time by `CBASE_PROVIDERS`:

- `local` is one process on SQLite (`cbase: providers/local/`).
- `aws` is Lambda, DynamoDB and IoT Core (`cbase: providers/aws/`).
  It is built and tested, but parked: no product runs on it today.

### HTTP/2 and gRPC

`cbase: core/src/net/h2.h` and `cbase: core/src/net/grpc.h` are an outbound HTTP/2 and gRPC client, with no protobuf library.
`cb_grpc_unary` makes a unary call from bytes to bytes and returns the status and trailers.
amgi uses it for its Gemini calls.
The module links alone (`make h2-alacarte`), and `cbase: docs/H2.md` gives the threading, the retry rule and the limits.

### The slow pool

A handler that holds its thread on a remote call for seconds, such as a Gemini call, would starve the request pool.
Mark its route `CB_ROUTE_SLOW` and it runs on a separate slow pool (`cb_app.slow_workers`, default 4, `CBASE_SLOW_WORKERS`).
`cb_app.slow_queue` and `cb_app.slow_full` say what happens when every slow worker is busy.
Serve starts the pool only when some route is slow.
`CB_ROUTE_SLOW` never combines with the loop flags.

### Security defaults

- CORS is deny by default.
  `cb_app.cors_origins` lists the allowed origins, and a WebSocket upgrade is held to the same list.
- `cb_app.host_allow` is the Host allow-list against DNS rebinding.
  Once declared, a request with any other Host gets 403, and a missing or malformed Host gets 400.
  A server on a user's own machine should declare it.
- Request bodies are capped at `CBASE_MAX_BODY` (1 MiB), or a route's own `max_body` (at most 1 GiB).
  A longer Content-Length is answered 413 from the head alone.
- `CBASE_ENV_FILE` is read strictly: a bad line refuses the boot and names its number.
  A variable already in the environment wins over the file.
- Outside `CBASE_DEV=1`, serve needs `CBASE_JWT_SECRET` (32 characters or more) and `CBASE_PUBLIC_URL`.

### Files

`cbase: core/src/net/files.h` has `cb_send_file`, `cb_serve_dir` and `cb_path_join` to serve an app's pages, assets and media from disk.
A path is opened as given, so a relative one resolves against serve's working directory, not the source file or the binary.
Under `make serve` that directory is `APP_SERVE_DIR`.

### make serve, cbase.mk and versions

`cbase: cbase.mk` is the make fragment a product includes.
Its header lists every variable a product may set.
The product sets `CBASE`, `CBASE_PROVIDERS`, `APP_SRC_ROOTS`, `APP_TEST_ROOTS` and the rest, then includes it.
There are no file lists to edit: a new `.c` under a source root joins every program.
`make serve` builds `build/$(APP_SERVE)` and execs it through `cbase: tools/serve.sh`:

- `APP_SERVE_DIR` is the working directory, by default the directory of the Makefile make read.
- `APP_ENV_FILE` (default `.env.local`) is named to serve as `CBASE_ENV_FILE` when it exists.
- `APP_SERVE_LISTEN` is `CBASE_LISTEN`, by default `127.0.0.1:$(PORT)`, or 3000 with no `PORT`.
  3000 is amgi's port, so a second product on one machine sets its own.

The release number lives in `cbase: VERSION`.
`CB_VERSION` in `cbase: core/src/ctx.h` must equal it, and `cbase: core/test/unit_version.c` fails otherwise.
A release is tagged `v<VERSION>`, and the `ci.yml` version job rejects any other tag.
The API is pre-1.0, and breaking changes are recorded in `cbase: docs/PLATFORM.md` section 2.9.

## 4. cbase, the clients

`cbase: client/` is the other half of cbase's protocol: the same behavior in every language, from one C core.
`cbase: docs/CLIENT_DESIGN.md` is the design, and `cbase: client/include/cbase/client.h` is the contract.

### The core and the hosts

The core (`cbase: client/src/client.c`) is sans-IO.
It owns the session, the socket protocol, calls, retries and timers.
The host owns the socket, HTTP, the timer and the clock, and passes bytes and `now` in.
The core allocates nothing and calls only five libc functions (`memcpy`, `memmove`, `memset`, `memcmp`, `strlen`).
`cbase: client/test/requires_gate.sh` holds it to that, so a freestanding wasm32 module can link it.

The shared host (`cbase: client/hosts/common/cbc_host.h`) is everything a C host needs that does not depend on the platform.
An adapter supplies a `cbc_host_env` (socket, fetch, timers, clock, store, entropy) and calls `cbc_host_init`, `cbc_host_pump` and the `cbc_host_ws_*` and `cbc_host_http_answered` functions.
`cbc_host_stop` stops a host from the loop or from inside the app's own callback: timers cancelled, socket closed, nothing more sent or told the app.
A restart is `cbc_host_init` again from the loop.
Every host passes one suite against a scripted server (`cbase: client/test/host/host_suite.h`).

The adapters:

- POSIX: `cbase: client/hosts/posix/` puts the shared host on a native event loop.
- TypeScript: `cbase: client/ts/socket.ts` holds one WebSocket, `fetch`, one timer and `localStorage` over the wasm module, plus a React Apple button.
- Swift: `cbase: client/swift/CBaseClient/CbClient.swift` is the HTTP half.
  The socket half is not written yet.
- cdom: `cbase: client/hosts/cdom/` runs the client inside a cdom app's own wasm module.
  An app includes `cdom.mk`, `cbase: client/client.mk`, then `cbase: client/hosts/cdom/adapter.mk` (which stops with a message naming the fix if `client.mk` or `cdom.mk` was not included before it).
  `CBC_CDOM_SRC` and `CBC_B64_SRC` are added to `APP_SRC`, `CBC_CDOM_CFLAGS` to the flags, and `CBC_CDOM_MODULES` (`net store random`) to `APP_MODULES`.
  `cbase: client/hosts/cdom/README.md` pins the cdom commit it is tested against (`CDOM_PIN` in `host.mk`).
  The dependency runs one way: cbase knows cdom here, and cdom never knows cbase.

### Modes, wire and timing

A client is anonymous or signed in.
An anonymous client uses `GET /ws/public`, sends an empty HELLO, keeps no session, and makes every call over HTTP.
A signed-in client signs in over `/v1/auth` and authenticates once per socket with HELLO.
Its calls then ride the socket as `SF_REQ` with no token.
The socket is open only while a room is held (`cbc_stream_wanted`), so a client with no subscriptions keeps no connection.
The client refreshes its token 60 s early and rolls the socket with the new token.

The wire is binary, never JSON.
The frame codec is `cbase: lib/stream_frame.h`, and the session wire is `cbase: client/include/cbase/auth_wire.h`.
`cbase: tools/structgen` reads `client.h` and generates the readers for each language, so no host spells a byte offset.

Retry timing is in `client.h` and `cbc_cfg`:

- `CBC_TRIES` is 4: a read (`CBC_C_READ`) is sent at most four times after losses, waiting 500, 1000 and 2000 ms (`retry_first_ms` doubling, capped by `retry_max_ms`).
  A server that refuses connections is heard about 3.5 s after the call.
  A server that never answers takes 4 x `http_wait_ms` + 3.5 s, which is 63.5 s with the defaults.
- A write is never retried: if it is lost, `CBC_EV_DONE` says `CBC_WHY_LOST` at once.
- `CBC_BUSY_TRIES` is 3 and `CBC_BUSY_FIRST_MS` is 250: a 503 read waits 250, 500 and 1000 ms.
- Reconnects use decorrelated jitter from the floor (500 ms) to `retry_max_ms` (10 s), seeded by the host.
- Other defaults: `ping_ms` 30 s, `pong_ms` 10 s, `call_wait_ms` 15 s, `http_wait_ms` 15 s.

## 5. Building an app on both

### The page side (cdom)

An app on both keeps two Makefiles, `server/` and `ui/`, because `cbase.mk` and `cdom.mk` both read `APP_CFLAGS`: one for native objects, the other for wasm.
The UI's Makefile includes `cdom.mk`, then cbase's `client/client.mk` and `client/hosts/cdom/adapter.mk`, then `cdom.mk` again, now with `APP_*` set (a known wart: cdom will make one include enough).
`APP_MODULES` takes the adapter's `CBC_CDOM_MODULES` (net, store, random).
The page is one html file with `/*CDOM_SHIM*/` in a script and one call to `cdom.start`.
Measured, from the docs alone: a fresh agent built the hello world (a page that greets you through a cbase route) in 18 min 26 s the first time, and in 11 min 33 s after one round of fixes, working end to end at 4 min 54 s.

### The server side (cbase)

The server side of an app is a `server/` directory.
Its `Makefile` sets `CBASE`, `CBASE_PROVIDERS := local`, `APP_SRC_ROOTS` and the rest, then includes `cbase: cbase.mk`.
Its source defines the `cb_app` and the routes (section 3).
`cbase: example/Makefile` is a working one.
If the Makefile sits in `server/` and the app's files are at the repository root, set `APP_SERVE_DIR := ..`.

```make
CBASE           := ../cbase
CBASE_PROVIDERS := local
APP_SRC_ROOTS   := src
APP_SERVE       := myapp
APP_SERVE_DIR   := ..
include $(CBASE)/cbase.mk
```

`make -C server serve` builds and runs it, and `make -C server check` runs the tests and gates.
Each name means one thing: `APP_CFLAGS` in `cbase: cbase.mk` is the server's native flags, and the UI's wasm flags have a name of their own in `cdom.mk`, so one Makefile may include both.
`cbase.mk` and `cdom.mk` both read `APP_CFLAGS` today (native flags in one, wasm flags in the other), so one Makefile including both would mix the two sets.
The fix is one meaning per name: the wasm side's flags get their own name (proposed: `APP_WASM_CFLAGS` and `APP_WASM_LDFLAGS`, next to `APP_WASM`), pending the owner's ruling.
Until it lands, keep the server and the UI in separate Makefiles (`server/` and `ui/`), as amgi and the hello-world apps do.

## 6. A real app: amgi

### 6a. A real app: how amgi joins cdom and cbase

amgi is a local-first Anki card maker.
It is one C server on cbase (`server/`) and one C page on cdom (`ui/`), and nothing else runs.
The server serves the page, reads and writes the user's Anki collection, and calls Gemini for card text and speech.
The page is C compiled to WebAssembly on cdom.
Page and server talk over a binary wire layout (`ui/wire/wire.c`), never JSON.
JSON stays only where something outside amgi forces it: the model's structured answer (`server/json/`) and Anki's own formats, each behind a scan test.
amgi is storeless on cbase: it keeps no database of its own, and the user's Anki collection is the only store.
Everything starts with `make start`; there is no Node on the start or build path.
npm stays only for eslint and the node:test suites.

How amgi reaches the collection, two transports behind one interface (`server/collection/transport.c`):
- **Direct**, when Anki is closed: the server opens `collection.anki2` itself with SQLite, through its own port of Anki's note, deck and note type rules (`notes.c`, `decks.c`, `direct.c`), including Anki's unicase collation and field checksum.
- **Bridge**, when Anki is open: Anki holds the file, so a small add-on (`anki/addon/amgi_bridge`) runs a local HTTP server inside Anki and the server talks to it with a binary protocol (`bridge.c`).
  `WIRE_BRIDGE_PROTOCOL` is checked on every request, so a stale add-on is reported as outdated instead of misread.
- Every request carries the user's settings as headers (`X-Amgi-*`, percent-encoded): the Anki folder, collection, profile, bridge address and token.
  No settings means "unconfigured", and no collection is opened.

The model calls:
- Gemini is called over gRPC from C (`server/amgi/gemini.c`).
- One shared rate gate (`server/amgi/gate.c`) backs off with jitter, learns a per-minute cap, and refuses a start before any work begins, so a bulk run never trips the limit twice.
- The jitter comes from the one seedable random source (`server/collection/rng.c`), so a test can fix it with one line.

What this shows about the stack:
- A real product needed cbase for the server and cdom for the page, and little else.
- The one hard bug was neither's code but a platform fact (Anki's lock behaviour) that only a real user on a real collection found; section 6b has it.

### 6b. Writing a real Anki collection safely (incident-1842)

amgi's C server reads and writes the user's own Anki collection, a SQLite file that Anki itself also holds open.
On 2026-11-08 at 18:42 that collection was destroyed: 56 of its 58 pages were zeroed, and only a backup brought it back.
The full write-up is ~/Dev/foolish-reports/incident-1842.md; this section keeps the rules it left behind.

What happened, in order:
1. Anki opens its collection in WAL mode with `locking_mode=EXCLUSIVE`, so its WAL index lives in Anki's own memory and no `-shm` file exists.
2. Anki's backup on close reads the collection file through a plain second handle and closes it, and on POSIX that close drops every lock the process holds on the file, Anki's exclusive lock included.
3. Anki keeps writing to its `-wal`, still believing it owns the file.
4. Any second SQLite connection can now open the file, and its open or close checkpoints and resets Anki's live `-wal` underneath it.
5. Anki's next small write lands after a hole of zeros, and Anki's own close then copies every page from that hole into `collection.anki2` as zero bytes.

The rules, each with one owner in the code:
- **Never open a collection that has a `-wal` or `-shm` beside it.** `collection_openable` in `server/collection/db.c` reports such a file as `locked` and never calls `sqlite3_open_v2`; it checks again right after an open, before any statement, to close the race with Anki starting.
- **Never open a file without a SQLite header.** A file whose first 16 bytes are not `SQLite format 3\0` is reported as `damaged` and never opened, so a leftover `-wal`, which may hold the only good copy of the newest pages, is never checkpointed or deleted.
- **Never checkpoint.** No amgi code runs `wal_checkpoint` or any explicit checkpoint; `test/no-checkpoint.test.mjs` scans the shipped C, JS and Python and fails on one.
- **Never create a collection.** Opens never pass `SQLITE_OPEN_CREATE`.
- **Back up before writing.** Every write goes through `with_write`, which applies the backup policy in `backup.c` on the writing connection: a copy when this process has not backed up the path yet, when the file changed since amgi last left it, or when the last backup is over 30 minutes old, keeping the newest ten.
- **The automatic install always takes a fresh one.** The page-load install writes the card type without a user asking, so creating the note type calls `ensure_fresh_backup_on` once it knows it will write (`notes.c`), unless that same connection already took one, whatever the 30-minute policy says.
- **A backup is never torn.** It is taken through SQLite's backup API to a temporary name, synced, then renamed into place, so a crash never leaves a partial file under a valid backup name.
- **Tests never see the real profile.** Every server, CLI and Anki stand-in a test starts gets its own temp `HOME` (`test/sandbox-home.cjs`), and `test/sandbox-home.test.mjs` fails on any spawn that doesn't; the C server's default profile folder is `$HOME/Library/Application Support/Anki2`.

How it is proven:
- `server/test/incident_1842.c` replays the mechanism with a child process playing Anki through SQLite, and is red without the `-wal` rule and green with it.
- `ui/tools/incident1842.sh` drives the real page against the C server through the same sequence (Anki holding the file, its backup, the page loading twice so the automatic install fires twice) and ends with the anki library opening the file and Check Database clean.
- The lock rule was mutation-checked: removing the `-wal` half alone lets the automatic install write into the live file again.

What the incident taught about process:
- A faithful port copies its original's bugs: both the Node server and its C port ran `wal_checkpoint(TRUNCATE)` on every open.
- Dropping that checkpoint alone did not fix it, because any second connection's close checkpoints too; only refusing to open did.
- A real user found it in minutes, on the exact path no suite simulated: Anki just closed, the page reloading.

## 7. Rules of the house

These rules hold across every repo; each repo enforces its own share with a test or a gate.

### cdom

A program's libc belongs to the program: on the web it is cdom's `src/libc/string.h` and `src/mini.c`, and neither cbase nor an app defines its own memcpy.
One seedable random source: cdom's `cd_seed` (the page's crypto at start, or `options.seed`), and no `Math.random`, no `Date.now` seed and no stray `rand()`.
The shim indexes C memory by no literal offset: C exports its struct offsets once (`cd_layout`), and a scan fails on any number in a reach into C memory.
cdom's I/O contract changes only additively; a breaking change needs a new name and a deprecation, then the old name is deleted once its callers moved.
Every new test is mutation-checked: break the code, see it fail on the right assertion, restore it.
Small changes run only the checks they can break; full suites are for a large change's last check.
Nothing durable lives in /tmp or a session scratchpad.

### cbase

Each rule has a tool or test that fails when the rule breaks, run by `make check` or by CI.

- The gates share one C scanner (`cbase: core/test/lib_cscan.h`).
  It skips comments, character literals and `#include` lines, so a gate reads code and not prose.
  - name and repo gate: core names no provider or product, and nothing reaches outside the repo (`cbase: core/test/unit_name_gate.c`, `cbase: tools/repo_gate_test.sh`).
  - symbol gate: a local-only binary carries no aws symbol (`cbase: tools/symbol_gate.sh`).
  - libc gate: no cbase object defines a libc name (`cbase: tools/libc_gate.sh`).
  - random gate: `rand`, `random`, `arc4random`, `RAND_bytes`, `getentropy` and `/dev/urandom` are banned (`cbase: core/test/unit_random_gate.c`).
  - JSON gate: JSON stays out of shipped paths (`cbase: core/test/unit_json_gate.c`).
  - offline build: the local-only example builds in a stripped tree with the network denied (`cbase: tools/offline_build_test.sh`).
- Coverage floors are in `cbase: tools/cov_floor.txt`, and `make coverage` fails below them or on a function no test calls.
- Every test is mutation-checked: the code was broken, the named assertion failed, and the code was restored (`cbase: docs/mutation_checks.md`).
- There is one seedable random source per kind: secrets from `cb_random_bytes`, seeded streams from `cbase: lib/rng.h` (splitmix64), and the client seed from its host.
- No JSON on shipped paths.
  Clients speak binary, and JSON is contained in `cbase: core/src/core_json.c` and `cbase: providers/aws/src/aws_json.c` for foreign APIs and the browser console.

### amgi and foolish

- A port of a working program is checked against the real thing it talks to, not only against the program it replaces: amgi's C collection core is tested against the real Anki library, because the Node code it ported had the same data-loss bug.
- Every process a test starts gets its own temporary HOME, so no test can reach a person's real data; Chrome keeps the real HOME and passes `--use-mock-keychain --password-store=basic`.
- A data-safety bug is reproduced end to end, red, before it is fixed, and its test stays.
- Merges are rebase merges, never squashes, and a merged branch and its worktree are deleted.

## 8. Where to read more

- cdom: README.md (building and starting an app), src/cdom.h (the API, and the I/O contract at its top), cdom.mk (the flags, sources and rules), BRIDGE.md (why one import per operation), RESULTS.md and PERF_LOG.md (measurements, and the no-WebAssembly path), demo/click.c (a small app).
- The hello-world apps: /Users/alex/Dev/hello-world and hello-world-2, each with a TIMING.md (the timed builds and their friction lists).
- cbase: README.md (what it is, using it as a submodule), docs/PLATFORM.md (the app API, upgrading notes, the env file and serving files), docs/H2.md (HTTP/2 and gRPC), client/hosts/common/cbc_host.h ("WRITING A HOST"), client/hosts/cdom/README.md (the browser adapter and its tested cdom pin), docs/mutation_checks.md.
- amgi: README.md (quick start with make start, the app's API), skills/amgi (using it), .claude/skills/e2e-ui (the browser drives), skills/amgi/references/collection-internals.md (Anki's file and its locking), scripts/verify-c-backend.sh (the fresh-clone check).
- foolish: docs/ARCHITECTURE_AS_A_PATTERN.md (the pattern, through one game); the incident write-up is ~/Dev/foolish-reports/incident-1842.md.

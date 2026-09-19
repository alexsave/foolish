// foolish_server.c - a DEDICATED, in-memory Foolish server, in C.
//
// A second backend under server/impls (sibling to supabase/), to prove the
// server API is language-agnostic: same game, no TypeScript, no edge runtime,
// no Postgres. A long-lived process holds every game as a `Game` struct in RAM
// (the "in-memory authoritative state" of docs/ARCHITECTURE_AS_A_PATTERN.md),
// guarded by per-game locks (see registry.h's "Locking"), and the C KERNEL
// drives all of it - this server only starts a socket, routes requests, and
// hands them to the kernel. Every rule (deal, legality, apply, refill,
// who-is-the-fool, the masked per-seat view) is c/src/*.c, exactly as the
// wasm/edge build uses it. Swap Postgres for a hash table and the edge runtime
// for a thread pool and the game is unchanged.
//
// THIS FILE is the entry point and nothing else: the CLI, the bring-up order,
// and the accept loop it hands off to. The server proper is split by what each
// part is ABOUT:
//
//   registry.c    the live table of games and users, and the locks over it
//   session.c     accounts and the stateless signed session token
//   lobby.c       /create and the /meta verbs (join, start, add-bot, continue)
//   play.c        the action path: apply a move, serve the masked view
//   bots.c        the per-game bot trampoline and its decision counters
//   snapshot.c    the durable form of a game and a user (SQLite write-behind)
//   metrics.c     /stats and /metrics
//   ctl_wire.c    the packed control-plane wire the endpoints speak
//   httpd.c       the HTTP front door: parse, respond, route, accept
//   wsconn.c      what a /ws session is, and the --tls thread-per-conn loop
//   shard.c       epoll-per-shard connection multiplexing (plaintext)
//   push.c        telling everyone watching a game that it changed
//   game_bridge.c the QUIC/WebTransport front-end onto the same games
//
// POC scope: HTTP/1.1 (hand-rolled - a real deployment would drop in mongoose
// or civetweb), token auth with no session store (a signed blob, no JWT).
// Every body on the wire is PACKED BYTES, in both directions: the control
// plane is ctl_wire.h, a move is an awire frame, a state is view.c's
// state_put. There is no JSON anywhere in this server. Endpoints mirror the
// contract:
//   POST /auth/signup  CTL_AUTH   -> CTL_SESSION {user_id, username, token}
//   POST /auth/signin  CTL_AUTH   -> CTL_SESSION (same act: a username
//                                    that exists is re-claimed)
//   POST /create             (bearer) -> CTL_GAME {game_id}
//   POST /meta  CTL_META     (bearer) -> CTL_LOBBY {game_id, status}
//   POST /action?game_id=..  (bearer)  body = the packed awire move
//                                     -> CTL_APPLIED {ok, status}
//   GET  /state?game_id=..&seat=..    -> the kernel's masked view, packed
//   GET  /status?game_id=..           -> CTL_STATUS {status}
//   GET  /health                      -> CTL_HEALTH
//   GET  /stats                       -> CTL_STATS (bot-decision counters etc)
//   GET  /metrics                     -> Prometheus text (the one deliberate
//                                        exception - a scrape can only read text)
//   GET  /ws?game_id=..&seat=..  (bearer, Upgrade: websocket)
//                                         -> RFC 6455 WebSocket. ONE persistent
//     connection per (authenticated, seated) client replaces the HTTP
//     action+state round trip for the hot loop: every binary frame the
//     client sends is applied as an awire move (or, if empty, treated as a
//     "give me current state" poll) and answered with one binary frame,
//     [ok:u8][state_put masked view bytes]. See wsconn.h and the "T1b"
//     section of PROFILE_HOTPATH.md for why: thread-per-CONNECTION instead
//     of thread-per-REQUEST amortizes pthread_create's cost (a fresh
//     thread's zeroed stack/TLS was 85.8% of instructions under load,
//     T1) over a client's entire session instead of paying it per move.
//   GET  /ws?game_id=..&spectator=1  (bearer, Upgrade: websocket)
//                                         -> Stage 4: the same WebSocket, but
//     for a read-only watcher that owns no seat - masked with VIEW_SPECTATOR
//     (every hand AND the deck hidden), and any frame it sends is ignored,
//     never applied. See wsconn.h for the full design.
//
// TLS (Stage 3, TLS.md): pass `--tls --cert=PATH --key=PATH` to serve every
// endpoint above over TLS instead - https:// for the one-shot endpoints,
// wss:// for /ws - off ONE shared, read-only-after-setup SSL_CTX with a
// fresh per-connection SSL* (see conn.h/conn.c). Without --tls the listener
// is plain HTTP/WS, byte-for-byte unchanged from every earlier stage.
//
// Concurrency ("T2a", PROFILE_HOTPATH.md / SERVER_SCALING.md): acceptor
// threads read + parse each request and route it either onto a game-sharded
// epoll worker (plaintext - shard.h) or, under --tls, to a dedicated
// per-connection thread (a /ws upgrade - wsconn.h) or a typed work-queue pool
// (every other endpoint - httpd.c). Per-game state is guarded by that
// GameSlot's own lock, not one process-wide mutex - see registry.h's "Locking"
// for the two-tier scheme and its lock-order invariant.

#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bots.h"
#include "conn.h"
#include "game.h"
#include "httpd.h"
#include "persist.h"
#include "play.h"
#include "push.h"
#include "registry.h"
#include "session.h"
#include "shard.h"
#include "snapshot.h"
#include "srvthread.h"
#include "strategy.h"
#ifdef FOOLISH_QUIC
#include "game_bridge.h"  // the QUIC/HTTP3/WebTransport front-end onto the shared game
#endif

#ifdef FOOLISH_QUIC
// QUIC/HTTP3/WebTransport listener (foolish_server_quic build): --quic turns it
// on, --quic-port=N sets its UDP port (default = the TCP port; UDP and TCP are
// separate namespaces so they may share the number). Reuses --cert/--key for
// its TLS 1.3 (QUIC has no plaintext mode). See quic_wt.c / game_bridge.h.
static bool g_want_quic = false;
static int  g_quic_port = 0;
static int  g_quic_workers = 2;   // sharded QUIC event loops (--quic-workers=N); each its own SO_REUSEPORT UDP socket
#endif

// --------------------------------------------------------------------------
// --bench-fanout=N: in-process scaling probe. The container's hard ulimit -n
// (4096) makes N real sockets impossible, but the SERVER's own scaling concerns
// are memory + the per-connection fan-out cost, neither of which needs a real
// fd. This builds N real EConns (2 per real dealt GameSlot) on one worker's
// ws_head and runs the actual push path (econn_push_view - state_put_cached_ptr
// + ws_send_frame2 into each wbuf, the exact code worker_push_stale runs),
// reporting RSS and fan-out throughput. Proves the data structures + hot loop
// hold at N; the socket count itself is an OS ulimit knob, not a server limit.
// --------------------------------------------------------------------------
static long bench_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256]; long kb = 0;
    while (fgets(line, sizeof line, f)) if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    fclose(f); return kb;
}
static void run_bench_fanout(int n_conns) {
    const int seats = 2;
    int n_games = (n_conns + seats - 1) / seats;
    fprintf(stderr, "[bench] target %d connections across %d 2-seat games (fd-free, in-process)\n", n_conns, n_games);
    long rss0 = bench_rss_kb();

    GameSlot **games = malloc(sizeof(GameSlot *) * (size_t)n_games);
    if (!games) { fprintf(stderr, "[bench] OOM games array\n"); return; }
    int gmade = 0;
    for (int gi = 0; gi < n_games; gi++) {
        int idx = g_games_count;
        GameSlot *s = game_slot_ensure(idx);
        if (!s) { fprintf(stderr, "[bench] game slot alloc failed at %d\n", gi); break; }
        g_games_count++;
        memset(s, 0, sizeof *s);
        s->slot_idx = idx;
        pthread_mutex_init(&s->lock, NULL);
        pthread_cond_init(&s->cond, NULL);
        for (int k = 0; k < MAX_PLAYERS + 1; k++) s->view_cache_version[k] = (uint32_t)-1;
        s->used = true;
        s->game.num_players = seats;
        for (int p = 0; p < seats; p++) s->game.players[p].strategy_key = STRATEGY_KEY_HUMAN;
        game_seat_and_deal(&s->game, NULL, seats);
        s->version = 1;
        games[gmade++] = s;
    }
    long rss_g = bench_rss_kb();

    Worker *w = &g_workers[0];
    int cmade = 0;
    for (int gi = 0; gi < gmade && cmade < n_conns; gi++)
        for (int seat = 0; seat < seats && cmade < n_conns; seat++) {
            EConn *ec = calloc(1, sizeof(EConn));
            if (!ec) { fprintf(stderr, "[bench] econn alloc failed at %d\n", cmade); goto built; }
            ec->kind = ECONN_WS; ec->fd = -1; ec->slot = games[gi];
            ec->seat = seat; ec->spectator = false; ec->cache_idx = seat; ec->viewer = seat;
            ec->last_pushed_version = 0;
            ec->wbuf_cap = WS_WBUF_CAP; ec->wbuf = malloc(WS_WBUF_CAP);
            if (!ec->wbuf) { fprintf(stderr, "[bench] wbuf OOM at %d\n", cmade); free(ec); goto built; }
            ec->prev = NULL; ec->next = w->ws_head;
            if (w->ws_head) w->ws_head->prev = ec;
            w->ws_head = ec; cmade++;
        }
built:;
    long rss_c = bench_rss_kb();
    fprintf(stderr, "[bench] built %d connections across %d games\n", cmade, gmade);
    fprintf(stderr, "[bench] RSS  start=%ld MB  +games=%ld MB  +conns=%ld MB  TOTAL=%ld MB\n",
            rss0 / 1024, (rss_g - rss0) / 1024, (rss_c - rss_g) / 1024, rss_c / 1024);
    if (gmade) fprintf(stderr, "[bench]   per-game=%.1f KB   per-conn=%.1f KB\n",
            (double)(rss_g - rss0) / gmade, cmade ? (double)(rss_c - rss_g) / cmade : 0.0);

    // Fan-out sweeps: bump every game's version so all conns are stale, then run
    // the real per-connection push encode over the whole ws_head list.
    int rounds = 5; long pushes = 0;
    for (EConn *ec = w->ws_head; ec; ec = ec->next) { uint32_t v; econn_push_view(ec, NULL, 0, false, &v, NULL); ec->wlen = ec->woff = 0; ec->last_pushed_version = 0; }  // warm caches
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < rounds; r++) {
        for (int gi = 0; gi < gmade; gi++) games[gi]->version++;
        for (EConn *ec = w->ws_head; ec; ec = ec->next) {
            uint32_t v; if (econn_push_view(ec, NULL, 0, true, &v, NULL) == 1) { ec->last_pushed_version = v; ec->wlen = ec->woff = 0; pushes++; }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "[bench] fan-out: %ld pushes in %.3fs = %.0f pushes/s (%.3f us/push); full sweep of %d conns = %.1f ms\n",
            pushes, secs, secs > 0 ? pushes / secs : 0, pushes ? secs / pushes * 1e6 : 0, cmade, rounds ? secs / rounds * 1000 : 0);
    fprintf(stderr, "[bench] DONE - no crash at %d connections / %d games, RSS %ld MB\n", cmade, gmade, rss_c / 1024);
}

int main(int argc, char **argv) {
    thread_disable_cancellation();   // main is acceptor[0]; runs the accept() loop - same cancel-bracket tax, see thread_disable_cancellation
    int port = 8099;
    // Default: DB ON (see DURABILITY.md - "Stage 2: persistence"). --no-db
    // opts all the way out (pure in-memory, e.g. for tests/benchmarks that
    // don't want a stray file); --db=PATH points at a specific file instead
    // of the default; --persist-interval-ms tunes the write-behind period
    // (50-100ms is the documented sweet spot - see persist.h).
    const char *db_path = "./foolish.db";
    int persist_interval_ms = 75;
    // Stage 3: TLS is OFF by default (plaintext, byte-for-byte the same as
    // every earlier stage) - `--tls --cert=PATH --key=PATH` turns the WHOLE
    // listen socket over to TLS (HTTPS + WSS); there is no mixed
    // plaintext+TLS listener in this design (see TLS.md for why a single
    // `--tls`-flips-the-listener design was chosen over a second
    // `--tls-port`).
    bool want_tls = false;
    const char *cert_path = NULL, *key_path = NULL;
    int bench_fanout = 0;
    bots_install_seed_hook();   // before any thread, and before the self-tests
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--self-test-bot-seeding")) return run_bot_seeding_self_test();
        if (!strncmp(argv[i], "--bench-fanout=", 15)) { bench_fanout = atoi(argv[i] + 15); continue; }
        if (!strncmp(argv[i], "--max-conns=", 12)) { g_max_conns = atoi(argv[i] + 12); continue; }
        if (!strncmp(argv[i], "--accept-threads=", 17)) {
            int nw = atoi(argv[i] + 17);
            if (nw > 0 && nw <= MAX_ACCEPT_THREADS) g_n_accept_threads = nw;
            continue;
        }
        if (!strncmp(argv[i], "--game-idle-ttl-s=", 18)) { int v = atoi(argv[i] + 18); if (v >= 0) g_game_idle_ttl_us = (uint64_t)v * 1000000ULL; continue; }
        if (!strncmp(argv[i], "--reap-interval-s=", 18)) { int v = atoi(argv[i] + 18); if (v > 0) g_reap_interval = v; continue; }
        if (!strncmp(argv[i], "--ratelimit-rpm=", 16)) { int v = atoi(argv[i] + 16); if (v >= 0) g_rl_rate = (float)v / 60.0f; continue; }        // 0 disables
        if (!strncmp(argv[i], "--ratelimit-burst=", 18)) { int v = atoi(argv[i] + 18); if (v > 0) g_rl_burst = (float)v; continue; }
        if (!strncmp(argv[i], "--io-timeout-s=", 15)) { int v = atoi(argv[i] + 15); if (v >= 0) g_io_timeout_s = v; continue; }   // slowloris guard; 0 = off
#ifdef FOOLISH_QUIC
        if (!strcmp(argv[i], "--quic")) { g_want_quic = true; continue; }
        if (!strncmp(argv[i], "--quic-port=", 12)) { g_quic_port = atoi(argv[i] + 12); continue; }
        if (!strncmp(argv[i], "--quic-workers=", 15)) { int n = atoi(argv[i] + 15); if (n > 0) g_quic_workers = n; continue; }
#endif
        if (!strncmp(argv[i], "--game-workers=", 15)) {
            int nw = atoi(argv[i] + 15);
            if (nw > 0 && nw <= MAX_GAME_WORKERS) g_n_game_workers = nw;
        } else if (!strncmp(argv[i], "--meta-workers=", 15)) {
            int nw = atoi(argv[i] + 15);
            if (nw >= 0 && nw <= MAX_META_WORKERS) g_n_meta_workers = nw;   // 0 = fold /meta onto the game pool
        } else if (!strncmp(argv[i], "--create-workers=", 17)) {
            int nw = atoi(argv[i] + 17);
            if (nw > 0 && nw <= MAX_CREATE_WORKERS) g_n_create_workers = nw;
        } else if (!strcmp(argv[i], "--no-db")) {
            db_path = NULL;
        } else if (!strncmp(argv[i], "--db=", 5)) {
            db_path = argv[i] + 5;
        } else if (!strncmp(argv[i], "--persist-interval-ms=", 22)) {
            int v = atoi(argv[i] + 22);
            if (v > 0) persist_interval_ms = v;
        } else if (!strcmp(argv[i], "--tls")) {
            want_tls = true;
        } else if (!strncmp(argv[i], "--cert=", 7)) {
            cert_path = argv[i] + 7;
        } else if (!strncmp(argv[i], "--key=", 6)) {
            key_path = argv[i] + 6;
        } else {
            int p = atoi(argv[i]);
            if (p > 0) port = p;
        }
    }
    srand((unsigned)(time(NULL) ^ getpid()));   // only ever called here, before any worker thread exists
    if (bench_fanout > 0) { run_bench_fanout(bench_fanout); return 0; }   // in-process scaling probe, then exit - see run_bench_fanout
    bots_resolve_octogen_strat();   // before any bot_thread can run - see its own doc
    // Long-lived /ws connections mean a peer can vanish (crash, network
    // reset, the load client's own `timeout` cutting it off) between our
    // read and our next write; the default SIGPIPE action is to kill the
    // WHOLE PROCESS on that write. Ignore it - write() already reports the
    // same failure as -1/EPIPE, which every write path here already checks
    // (and, under TLS, conn_read/conn_write (conn.c) translate the
    // equivalent SSL_ERROR_SYSCALL the same way - see their doc).
    signal(SIGPIPE, SIG_IGN);

    // Stage 3: build the shared server SSL_CTX ONCE, before any worker pool
    // or the accept loop starts (same "set up before any other thread can
    // touch it" posture persist_start takes for crash recovery, just for a
    // different piece of startup state) - see g_tls_ctx's own doc in httpd.h.
    // A REQUESTED (--tls passed) but failed TLS setup is fatal, never a
    // silent downgrade to plaintext: same posture Stage 2 takes for a
    // requested-but-failed --db (see persist_start's doc) - serving
    // plaintext when the operator asked for TLS would be a silent security
    // regression, worse than refusing to start.
    if (want_tls) {
        if (!cert_path || !key_path) {
            fprintf(stderr, "fatal: --tls requires --cert=PATH --key=PATH\n");
            return 1;
        }
        g_tls_ctx = tls_server_ctx_create(cert_path, key_path);
        if (!g_tls_ctx) {
            fprintf(stderr, "fatal: TLS setup failed (--cert=%s --key=%s) - "
                             "check the cert/key are valid PEM and the key matches the cert\n",
                    cert_path, key_path);
            return 1;
        }
    }

    // Stage 2: the round-trip codec gate (always runs, --no-db or not - it's
    // a pure in-memory check of serialize_slot/deserialize_slot, not the DB
    // itself), then registering the two durable tables and starting
    // persistence - BEFORE any worker pool or the accept loop exists, so
    // crash recovery's synchronous load (inside persist_start) runs with no
    // other thread able to touch g_users[]/g_games[] yet. A requested
    // (non-NULL) --db that fails to open/configure is fatal: silently
    // downgrading a requested durability guarantee to "pretend it's fine"
    // would be worse than refusing to start.
    persist_self_test();
    snapshot_register_tables();
    if (!persist_start(db_path, persist_interval_ms)) {
        fprintf(stderr, "fatal: persistence failed to start (--db=%s)\n", db_path ? db_path : "(null)");
        return 1;
    }

    token_secret_init();   // stateless-token HMAC key (from $FOOLISH_TOKEN_SECRET or random) - before any signup

    httpd_queues_init();
    httpd_start_create_workers();

    // Game reclamation reaper (both modes): recycle quiescent finished/abandoned
    // games so RAM is bounded by peak concurrent games, not cumulative created.
    pthread_t reaper;
    if (pthread_create(&reaper, NULL, game_reaper_thread, NULL) == 0) pthread_detach(reaper);
    else fprintf(stderr, "warning: game reaper thread failed to start (memory will not be reclaimed)\n");

    if (g_tls_ctx) {
        httpd_start_tls_fallback_workers();
    } else {
        // Stage 6: plaintext runs epoll-per-shard instead of the typed game/
        // meta queues + thread-per-/ws-connection - `--game-workers=N` now
        // ALSO sizes the epoll shard count (same knob, see
        // SERVER_SCALING.md). The typed queues stay fully intact in httpd.c
        // (unused in this mode) purely for the --tls fallback.
        if (!shard_start_workers()) return 1;
    }

#ifdef FOOLISH_QUIC
    if (g_want_quic) {
        if (!cert_path || !key_path) {
            fprintf(stderr, "fatal: --quic requires --cert=PATH --key=PATH (QUIC has no plaintext mode)\n");
            return 1;
        }
        if (!quic_bridge_start(g_quic_port > 0 ? g_quic_port : port, g_quic_workers, cert_path, key_path))
            fprintf(stderr, "warning: QUIC listener thread failed to start\n");
    }
#endif

    // SO_REUSEPORT multi-acceptor bring-up. Create g_n_accept_threads
    // listeners, each bound to `port` with SO_REUSEPORT so the kernel spreads
    // inbound connections across them; run one acceptor loop per listener.
    // (See make_listener / acceptor_main and the g_n_accept_threads doc.)
    int listeners[MAX_ACCEPT_THREADS];
    int n_listeners = 0;
    for (int i = 0; i < g_n_accept_threads; i++) {
        int fd = make_listener(port, /*reuseport=*/true);
        if (fd < 0) {
            if (n_listeners == 0) return 1;   // couldn't bind even one - fatal
            fprintf(stderr, "warning: bound only %d/%d acceptor listeners\n",
                    n_listeners, g_n_accept_threads);
            break;
        }
        listeners[n_listeners++] = fd;
    }

    fprintf(stderr, "foolish native server (kernel-driven, in-memory + SQLite write-behind%s) on :%d "
            "(accept-threads=%d game-workers=%d meta-workers=%d create-workers=%d db=%s interval=%dms)\n",
            g_tls_ctx ? " + TLS (https/wss)" : "",
            port, n_listeners, g_n_game_workers, g_n_meta_workers, g_n_create_workers,
            db_path ? db_path : "off (--no-db)", persist_interval_ms);

    // Spawn acceptors [1..n_listeners-1] as detached threads; run acceptor
    // [0] on this (main) thread so main() blocks here forever, exactly as the
    // old single accept loop did. The kernel decides which listener each new
    // connection lands on (4-tuple hash).
    for (int i = 1; i < n_listeners; i++) {
        pthread_t at;
        if (pthread_create(&at, NULL, acceptor_main, (void *)(intptr_t)listeners[i]) == 0)
            pthread_detach(at);
        else
            fprintf(stderr, "warning: acceptor thread %d failed to start\n", i);
    }
    acceptor_main((void *)(intptr_t)listeners[0]);   // never returns
    return 0;
}

// sim.c - sim.h. Every byte to and from the game server is written and read by the kernel's own
// codecs (netwire.h, ask.h, table.h, awire.h, client_table.h) and the platform's stream frames
// (shared/c/stream_frame.h); the client library (netclient.h) is one client per process, so the
// players use the codecs directly, as the roundtrip bench and foolyard do.
//
// A PLAYER signs up over HTTP (POST /v1/auth/signup), opens /ws, says HELLO, and from then on
// sends every request on the socket (SF_REQ: create, meta, action, a game read) and hears every
// push there: its own seat's casts (NW_WS_CAST), applied to its client table; the spectators'
// casts (NW_WS_SPECT) it drops while seated, as the web client does. A cast the table cannot
// apply (a gap, a divergence) is answered by reading the game whole (GET /v1/games/<id>), as the
// web does, and counted as a resync.
//
// A TABLE is planned by its thread: the seat count (2 most of the time), how many of them bots,
// the rules; the creator creates and sets up (rules, bots, one ask at a time), the other humans
// join, and every human says START once the table is full. Moves come from the kernel's legal
// moves on the masked board - the handwritten brain most of the time, a random legal move now and
// then - after a think time; one move in N is hostile. After the fool is known the seats linger,
// then rematch (ASK_CONTINUE) or leave (ASK_EXIT; the last one out deletes the table).
#include "sim.h"

#include "be_metrics.h"
#include "board.h"
#include "client.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"
#include "world.h"
#include "lib_client.h"
#include "net/ws.h"

#include "ask.h"
#include "awire.h"
#include "client_table.h"
#include "legal.h"
#include "netwire.h"
#include "strategy.h"
#include "table.h"
#include "../../../shared/c/rng.h"
#include "../../../shared/c/stream_frame.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define REQ_TIMEOUT_MS   15000
#define LINGER_MS        6000     // after the fool is known, before leaving or a rematch
#define IDLE_MIN_MS      1500     // between a player's games
#define IDLE_MAX_MS      6000
#define STALL_MS         90000    // a table nobody moved at for this long is left
#define RECONNECT_MS     2000
#define RANDOM_MOVE_PCT  15       // moves taken at random off the menu, so games differ
#define MAX_BOT_IDS      32
#define NAME_POOL        24

// ---- the players ------------------------------------------------------------------------------

enum { P_IDLE, P_SETUP, P_LOBBY, P_PLAYING, P_OVER, P_LEAVING };
enum { R_CREATE = 1, R_ASK, R_ACTION, R_VIEW, R_EXIT, R_START, R_CONTINUE };

typedef struct sim_table sim_table;

typedef struct {
    int       index, thread;
    be_client c;                  // the account and its socket (client.h)
    int64_t   reopen_ms;
    // the game
    sim_table *table;
    ClientSlot  slot;
    ClientTable ct;
    ClientDelta *delta;
    bool      have;               // ct holds a game
    int       state;              // P_*
    uint32_t  played_v;           // moved on this version already
    int64_t   think_due_ms;       // -1 none
    int64_t   idle_until_ms;
    bool      ready_sent, subscribed, refetching;
    // the move in flight, for the push latency: its send time and the version it was made on
    int64_t   wait_t0_us;
    uint32_t  wait_v;
    uint64_t  rng;
} player;

struct sim_table {
    bool      used;
    char      gid[NW_GID_MAX + 1];
    int       gid_len;
    int       n_seats, n_bots, n_humans;
    int       humans[MAX_PLAYERS];   // player indices, the creator first
    int       bots[MAX_PLAYERS];     // bots to seat, by index into g_bot_pool
    bool      podkidnoy;
    int       state;                 // P_SETUP .. P_LEAVING, from the creator's point of view
    int       step;                  // setup: the next ask (rules, then bots, then joins)
    bool      step_pending;
    int64_t   created_ms, dealt_ms, over_ms, last_change_ms;
    uint32_t  version;
    uint32_t  moves;
    int       exits_pending;
    bool      rematch;
};

typedef struct {
    int        index;
    pthread_t  tid;
    player    *players;
    int        n_players;
    sim_table *tables;
    int        n_tables, target;     // live tables this thread keeps
    uint64_t   rng;
    LegalMoves moves;               // the thread's scratch
    struct pollfd *fds;
    bool       ready;
} sim_thread;

static be_sim_opts  g_o;
static int          g_port;
static sim_thread   g_threads[BE_SIM_MAX_THREADS];
static atomic_bool  g_stop;
static atomic_uint  g_seated, g_bots_seated;
static atomic_ullong g_hostile_seq;
// The bots the mixed sim seats: indices into board.h's roster, the kinds it picks (fetch_bots).
static int          g_bot_pool[MAX_BOT_IDS];
static int          g_n_bots;
static pthread_mutex_t g_ready_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ready_cv = PTHREAD_COND_INITIALIZER;
static int          g_n_ready;
static const char *const FIRST_NAMES[NAME_POOL] = {
    "Anya", "Boris", "Clara", "Dmitri", "Elena", "Felix", "Greta", "Hugo", "Irina", "Jonas", "Katya", "Leon",
    "Mila", "Nikita", "Olga", "Pavel", "Rosa", "Sasha", "Tamara", "Ulrik", "Vera", "Wanda", "Yuri", "Zoya",
};

static void nap_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t rnd(uint64_t *rng, uint32_t n) { return n ? rng_u32(rng) % n : 0; }

// ---- the socket (client.h) ----------------------------------------------------------------------

static bool send_req(player *p, int kind, int meth, const char *path, const uint8_t *body, size_t n, uint8_t hostile,
                     uint32_t intent) {
    return be_client_req(&p->c, kind, meth, path, body, n, hostile, intent);
}

static bool send_ask(player *p, int kind, const TableAsk *a, int req_kind) {
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(a, body, sizeof body);
    if (n <= 0) return false;
    return send_req(p, req_kind ? req_kind : R_ASK, SF_METH_POST, "/v1/meta", body, (size_t)n, 0, 0);
}

static void ask_init(TableAsk *a, int kind, const sim_table *t) {
    memset(a, 0, sizeof *a);
    a->kind = (uint8_t)kind;
    a->gid_len = (uint8_t)t->gid_len;
    memcpy(a->gid, t->gid, (size_t)t->gid_len);
}

static void send_sub(player *p, const sim_table *t, bool unsub) {
    be_client_sub(&p->c, t->gid, t->gid_len, unsub);
    p->subscribed = !unsub;
}

// Opens the socket; a player at a table hears its room again.
static bool open_stream(player *p) {
    p->subscribed = false;
    if (!be_client_open(&p->c, g_port)) return false;
    if (p->table && p->table->gid_len) send_sub(p, p->table, false);
    return p->c.ws >= 0;
}

static void close_stream(player *p) {
    be_client_close(&p->c);
    p->subscribed = false;
}

// ---- the world's view of a table ----------------------------------------------------------------

static void event_at(sim_table *t, int kind, int seat, const Card *cards, int n_cards, uint32_t version, uint32_t lat_us,
                     int refused) {
    be_w_event e;
    memset(&e, 0, sizeof e);
    e.gid_len = (uint8_t)t->gid_len;
    memcpy(e.gid, t->gid, (size_t)t->gid_len);
    e.kind = (uint8_t)kind;
    e.seat = (int8_t)seat;
    e.refused = (uint8_t)refused;
    if (n_cards > BE_EVENT_CARDS) n_cards = BE_EVENT_CARDS;
    e.n_cards = (uint8_t)n_cards;
    for (int i = 0; i < n_cards; i++) e.cards[i] = (be_w_card){ cards[i].suit, cards[i].value };
    e.version = version;
    e.lat_us = lat_us;
    e.at_ms = cb_now_ms();
    be_world_lock(&be_world_g);
    be_table *bt = be_world_find(&be_world_g, t->gid, t->gid_len);
    be_world_event(&be_world_g, bt, &e);
    be_world_unlock(&be_world_g);
}

static void hostile_event(sim_table *t, int seat, int kind, int refused) {
    Card c = { 0, (int8_t)kind };
    event_at(t, BE_EV_HOSTILE, seat, &c, 1, t->version, 0, refused);
}

// The row from this player's view (the creator reports the board; every human reports its hand).
static void report(player *p, int64_t now_ms) {
    sim_table *t = p->table;
    if (!t || !p->have) return;
    const TableView *v = &p->ct.view;
    bool reporter = t->humans[0] == p->index;
    be_world_lock(&be_world_g);
    be_table *bt = be_world_find(&be_world_g, t->gid, t->gid_len);
    if (!bt) {
        be_world_unlock(&be_world_g);
        return;
    }
    if (v->my_seat >= 0 && v->my_seat < BE_SEATS) {
        be_w_hand *h = &bt->hands[v->my_seat];
        int n = v->my_hand_count > BE_HAND ? BE_HAND : v->my_hand_count;
        h->n = (uint8_t)n;
        for (int i = 0; i < n; i++) h->cards[i] = (be_w_card){ v->my_hand[i].suit, v->my_hand[i].value };
    }
    if (reporter) be_board_fill(&be_world_g, bt, v, t->n_seats, t->moves);
    be_world_unlock(&be_world_g);
}

static void set_gone(sim_table *t, int64_t now_ms) {
    be_world_lock(&be_world_g);
    be_table *bt = be_world_find(&be_world_g, t->gid, t->gid_len);
    if (bt) {
        be_world_set_phase(&be_world_g, bt, BE_PHASE_GONE);
        be_world_changed(&be_world_g, bt);
    }
    be_world_unlock(&be_world_g);
}

// ---- the client table ----------------------------------------------------------------------------

static void adopt_env(player *p, const uint8_t *env, int n, int64_t now_ms) {
    client_init(&p->ct, &p->slot);
    client_delta_attach(&p->ct, p->delta);
    if (client_adopt_envelope(&p->ct, env, n) < 0) {
        CB_WARN("birdseye", "%s: an envelope of %d bytes was refused (%d)", p->c.name, n, p->ct.detail);
        return;
    }
    p->have = true;
    if (p->table) {
        if (p->ct.view.version > p->table->version) p->table->version = p->ct.view.version;
        p->table->last_change_ms = now_ms;
    }
    report(p, now_ms);
}

// The push's steps become the table's events (the creator reports them, once per table).
static void push_events(player *p, sim_table *t) {
    bool reporter = t->humans[0] == p->index;
    int rc;
    while ((rc = client_push_next(&p->ct)) == 1) {
        if (!reporter) continue;
        const PushEvent *e = &p->ct.event;
        int kind = be_board_event_kind(e);
        if (kind == BE_EV_DEALT) {
            if (t->dealt_ms == 0) {
                t->dealt_ms = cb_mono_ms();   // the sim's own clock, like every t->*_ms
                event_at(t, kind, -1, NULL, 0, p->ct.version, 0, 0);
            }
        } else if (kind) event_at(t, kind, e->seat, e->cards, e->n_cards, p->ct.version, 0, 0);
    }
}

static void refetch(player *p) {
    if (!p->table || p->refetching || p->c.ws < 0) return;
    char path[96];
    snprintf(path, sizeof path, "/v1/games/%s", p->table->gid);
    if (send_req(p, R_VIEW, SF_METH_GET, path, NULL, 0, 0, 0)) {
        p->refetching = true;
        cb_metrics_add(be_mx, BE_M_RESYNCS, 1);
        event_at(p->table, BE_EV_RESYNC, p->ct.view.my_seat, NULL, 0, p->ct.view.version, 0, 0);
    }
}

static void apply_cast(player *p, const uint8_t *cast, int n, uint32_t version, int64_t now_ms) {
    if (!p->have || !p->table) return;
    uint8_t ident[4096];
    int il = client_identity(&p->ct, ident, sizeof ident);
    if (il < 0) il = 0;
    uint8_t board[CLIENT_BOARD_KEEP_MAX];
    int bl = client_board_keep(&p->ct, board, sizeof board);
    if (bl < 0) bl = 0;
    int64_t t0 = cb_mono_us();
    int rc = client_cast_open(&p->ct, cast, n, ident, il, board, bl);
    if (rc < 0) {
        if (version > p->ct.view.version) {
            CB_DEBUG("birdseye", "%s: cast v%u on v%u refused (%d, detail %d): refetching", p->c.name, version, p->ct.view.version, rc, p->ct.detail);
            refetch(p);   // a gap or a divergence; a stale cast is dropped
        }
        return;
    }
    push_events(p, p->table);
    rc = client_push_final(&p->ct);
    cb_metrics_observe(be_mx, BE_M_CAST_DECODE, (uint64_t)(cb_mono_us() - t0));
    if (rc < 0) {
        refetch(p);
        return;
    }
    if (p->ct.view.version > p->table->version) p->table->version = p->ct.view.version;
    p->table->last_change_ms = now_ms;
    if (p->wait_t0_us && p->ct.view.version > p->wait_v) {
        cb_metrics_observe(be_mx, BE_M_MOVE_PUSH, (uint64_t)(cb_mono_us() - p->wait_t0_us));
        p->wait_t0_us = 0;
    }
    report(p, now_ms);
}

// ---- moves ---------------------------------------------------------------------------------------

// A legal move for this player's seat into wire: its length, or 0 when the seat has none. The
// handwritten brain chooses most of the time; a random legal move otherwise.
static int legal_wire(sim_thread *th, player *p, uint8_t *wire, int cap) {
    if (!p->have) return 0;
    return be_legal_wire(&p->ct, &th->moves, &p->rng, RANDOM_MOVE_PCT, wire, cap);
}

// A hostile move of `kind` into wire (and the gid to send it for): its length.
static int hostile_wire(player *p, int kind, uint8_t *wire, int cap, const char **gid, int *gid_len) {
    const TableView *v = &p->ct.view;
    *gid = p->table->gid;
    *gid_len = p->table->gid_len;
    AwireAction a;
    memset(&a, 0, sizeof a);
    switch (kind) {
    case BE_HOSTILE_FOREIGN_CARD: {
        // The first card of the deck's range that is neither in my hand nor on the table.
        int lo = min_value_for(v->num_players);
        for (int s = 0; s < NUM_SUITS; s++)
            for (int val = lo; val <= ACE_VALUE; val++) {
                Card c = { (int8_t)s, (int8_t)val };
                bool mine = false;
                for (int i = 0; i < v->my_hand_count && !mine; i++) mine = card_eq(v->my_hand[i], c);
                for (int i = 0; i < v->num_battles && !mine; i++)
                    mine = card_eq(v->battles[i].attack, c) || card_eq(v->battles[i].defense, c);
                if (mine) continue;
                a.kind = AWIRE_ATTACK;
                a.n = 1;
                a.cards[0] = c;
                return awire_encode(&a, wire, cap);
            }
        return 0;
    }
    case BE_HOSTILE_MALFORMED:
        if (cap < 3) return 0;
        wire[0] = 9;   // no such kind
        wire[1] = 200;
        wire[2] = 7;
        return 3;
    case BE_HOSTILE_OVERSIZE:
        if (v->my_hand_count == 0) return 0;
        a.kind = AWIRE_ATTACK;
        a.n = AWIRE_MAX_CARDS;
        for (int i = 0; i < AWIRE_MAX_CARDS; i++) a.cards[i] = v->my_hand[0];
        return awire_encode(&a, wire, cap);
    case BE_HOSTILE_WRONG_TABLE: {
        // A move at another live table of this thread, where this player has no seat.
        sim_thread *th = &g_threads[p->thread];
        for (int i = 0; i < th->n_tables; i++) {
            sim_table *o = &th->tables[i];
            if (!o->used || o == p->table || !o->gid_len || o->state != P_PLAYING) continue;
            *gid = o->gid;
            *gid_len = o->gid_len;
            a.kind = AWIRE_GOOD;
            a.n = 0;
            return awire_encode(&a, wire, cap);
        }
        return 0;
    }
    default: return 0;
    }
}

static void send_move(sim_thread *th, player *p, int64_t now_ms) {
    sim_table *t = p->table;
    uint8_t buf[NW_GID_MAX + 8 + 2 + 2 * AWIRE_MAX_CARDS];
    uint8_t wire[2 + 2 * AWIRE_MAX_CARDS];
    uint8_t hostile = 0;
    const char *gid = t->gid;
    int gid_len = t->gid_len;
    int n;
    uint64_t seq = atomic_fetch_add(&g_hostile_seq, 1) + 1;
    if (g_o.hostile_every > 0 && seq % (uint64_t)g_o.hostile_every == 0) {
        hostile = (uint8_t)(1 + (seq / (uint64_t)g_o.hostile_every) % BE_HOSTILE_N);
        n = hostile_wire(p, hostile, wire, sizeof wire, &gid, &gid_len);
        if (n <= 0) hostile = 0;
    }
    if (!hostile) n = legal_wire(th, p, wire, sizeof wire);
    if (n <= 0) return;
    memcpy(buf, gid, (size_t)gid_len);
    memcpy(buf + gid_len, wire, (size_t)n);
    uint32_t intent = p->ct.view.version;
    int len = table_request_encode(buf, sizeof buf, gid_len, n, true, intent);
    if (len <= 0) return;
    if (!send_req(p, R_ACTION, SF_METH_POST, "/v1/action", buf, (size_t)len, hostile, intent)) return;
    if (hostile) cb_metrics_add(be_mx, BE_M_HOSTILE, 1);
    else {
        p->played_v = intent;
        p->wait_t0_us = cb_mono_us();
        p->wait_v = intent;
        cb_metrics_rate(be_mx, BE_M_MOVES, cb_now_ms(), 1);
        t->moves++;
    }
}

// ---- replies -------------------------------------------------------------------------------------

static void on_reply(sim_thread *th, player *p, const sf_frame *f, int64_t now_ms) {
    be_pending q;
    if (!be_client_take(&p->c, f->id, &q)) return;   // a subscription's reply, or one forgotten
    uint32_t dt = (uint32_t)(cb_mono_us() - q.t0_us);
    sim_table *t = p->table;
    switch (q.kind) {
    case R_ACTION: {
        cb_metrics_observe(be_mx, BE_M_MOVE_ACK, dt);
        TableResponse r;
        bool applied = f->status == 200 && table_response_decode(f->body, (int)f->body_len, &r) == 0 && r.status == TABLE_STATUS_APPLIED;
        if (q.hostile) {
            if (applied) {
                cb_metrics_add(be_mx, BE_M_HOSTILE_TAKEN, 1);
                CB_WARN("birdseye", "%s: a hostile move of kind %d was APPLIED at %s (status %d)", p->c.name, q.hostile, t ? t->gid : "?", f->status);
            }
            if (t) hostile_event(t, p->ct.view.my_seat, q.hostile, !applied);
            break;
        }
        if (!applied) {
            // A race (the board moved while the move was on its way: the server answered at a later
            // version than the move was made on, a pass that made this seat the defender among them)
            // is the game's own business; a move refused on the very board it was made on is a finding.
            bool raced = f->status == 200 && table_response_decode(f->body, (int)f->body_len, &r) == 0 && be_move_raced(&r, q.intent);
            if (!raced) {
                AskRefusal why = { 0, 0, 0 };
                bool told = f->status != 200 && ask_refusal_decode(f->body, (int)f->body_len, &why) == ASK_OK;
                bool resp = f->status == 200 && table_response_decode(f->body, (int)f->body_len, &r) == 0;
                CB_WARN("birdseye", "%s: a move at %s on v%u was refused: HTTP %d, response %d reject %d, why %d code %d", p->c.name,
                        t ? t->gid : "?", q.intent, f->status, resp ? r.status : -1, resp ? r.reject_code : -1, told ? why.why : -1, why.code);
                cb_metrics_add(be_mx, BE_M_REFUSED, 1);
                if (t) event_at(t, BE_EV_REFUSED, p->ct.view.my_seat, NULL, 0, q.intent, dt, 1);
            }
            p->played_v = 0;   // try again on the next board
            p->wait_t0_us = 0;
            if (f->status != 200 && t) refetch(p);
        } else if (t) {
            be_world_lock(&be_world_g);
            be_table *bt = be_world_find(&be_world_g, t->gid, t->gid_len);
            if (bt) bt->w.last_act_us = dt;
            be_world_unlock(&be_world_g);
        }
        break;
    }
    case R_CREATE:
        cb_metrics_observe(be_mx, BE_M_REQ_CREATE, dt);
        if (f->status == 200 && t) {
            adopt_env(p, f->body, (int)f->body_len, now_ms);
            if (p->have && p->ct.gid_len > 0 && p->ct.gid_len <= NW_GID_MAX) {
                t->gid_len = p->ct.gid_len;
                memcpy(t->gid, p->ct.gid, (size_t)t->gid_len);
                t->gid[t->gid_len] = 0;
                be_world_lock(&be_world_g);
                be_table *bt = be_world_open(&be_world_g, t->gid, t->gid_len);
                if (bt) {
                    bt->w.n_seats = (uint8_t)t->n_seats;
                    bt->w.podkidnoy = (uint8_t)t->podkidnoy;
                    be_world_changed(&be_world_g, bt);
                }
                be_world_unlock(&be_world_g);
                event_at(t, BE_EV_NEW, 0, NULL, 0, 0, dt, 0);
                send_sub(p, t, false);
                report(p, now_ms);
            } else t->state = P_LEAVING;
        } else if (t) {
            CB_WARN("birdseye", "%s: create answered %d", p->c.name, f->status);
            t->state = P_LEAVING;
        }
        t->step_pending = false;
        break;
    case R_ASK:
    case R_START:
    case R_CONTINUE:
        cb_metrics_observe(be_mx, BE_M_REQ_ASK, dt);
        if (f->status == 200) adopt_env(p, f->body, (int)f->body_len, now_ms);
        else {
            AskRefusal why;
            int w = ask_refusal_decode(f->body, (int)f->body_len, &why) == ASK_OK ? why.why : -1;
            CB_WARN("birdseye", "%s: an ask (%d) at %s answered %d, why %d code %d", p->c.name, q.kind, t ? t->gid : "?", f->status, w,
                    w >= 0 ? why.code : 0);
            if (t && q.kind == R_ASK && t->state == P_SETUP) t->state = P_LEAVING;   // a setup that fails ends the table
        }
        if (t) t->step_pending = false;
        break;
    case R_VIEW:
        cb_metrics_observe(be_mx, BE_M_REQ_READ, dt);
        p->refetching = false;
        if (f->status == 200) {
            uint32_t version;
            NwBlob env;
            if (nw_view_decode(f->body, (int)f->body_len, &version, &env) == NW_OK && env.len) adopt_env(p, env.p, (int)env.len, now_ms);
        } else if (f->status == 404 && t) t->state = P_LEAVING;
        break;
    case R_EXIT:
        cb_metrics_observe(be_mx, BE_M_REQ_ASK, dt);
        if (t) t->exits_pending--;
        break;
    default: break;
    }
}

// ---- frames in --------------------------------------------------------------------------------

static void on_message(sim_thread *th, player *p, const uint8_t *msg, size_t n, int64_t now_ms) {
    int64_t wall = cb_now_ms();
    cb_metrics_rate(be_mx, BE_M_PUSHES, wall, 1);
    cb_metrics_rate(be_mx, BE_M_PUSH_BYTES, wall, n);
    sf_frame f;
    if (sf_parse(msg, n, &f) == 0) {
        if (f.kind == SF_REPLY) on_reply(th, p, &f, now_ms);
        return;   // READY, PONG
    }
    if (!sf_is_push(msg, n)) return;
    NwFrame nf;
    if (nw_frame_decode(msg, (int)n, &nf) != NW_OK) return;
    sim_table *t = p->table;
    if (!t || nf.gid_len != t->gid_len || memcmp(nf.gid, t->gid, (size_t)t->gid_len) != 0) return;
    if (nf.kind == NW_WS_VIEW) {
        if (nf.blob.len) adopt_env(p, nf.blob.p, (int)nf.blob.len, now_ms);
        else if (t->state != P_LEAVING) { p->have = false; }   // the seat is gone
    } else if (nf.kind == NW_WS_CAST) {
        // Before the seat's envelope (the create's or the join's reply) a cast has no board to
        // land on; the reply carries the newest version, so it is dropped, not fetched.
        if (p->have) apply_cast(p, nf.blob.p, (int)nf.blob.len, nf.version, now_ms);
    }
    // NW_WS_SPECT: the spectators' cast, dropped while seated (the web's rule); NW_WS_CHAT: none sent.
}

typedef struct { sim_thread *th; player *p; int64_t now_ms; } msg_ctx;

static void on_socket_message(void *ud, const uint8_t *msg, size_t n) {
    msg_ctx *m = ud;
    on_message(m->th, m->p, msg, n, m->now_ms);
}

static void read_socket(sim_thread *th, player *p, int64_t now_ms) {
    msg_ctx m = { th, p, now_ms };
    be_client_read(&p->c, on_socket_message, &m);
    if (p->c.ws < 0) p->subscribed = false;
}

// ---- tables ----------------------------------------------------------------------------------------

static int bot_pick(sim_thread *th) {
    if (g_n_bots == 0) return -1;
    return (int)rnd(&th->rng, (uint32_t)g_n_bots);
}

static int idle_players(sim_thread *th) {
    int n = 0;
    for (int i = 0; i < th->n_players; i++)
        if (th->players[i].state == P_IDLE && th->players[i].c.ws >= 0 && th->players[i].idle_until_ms <= cb_mono_ms()) n++;
    return n;
}

// Plans and starts one table: seats, bots, rules, the creator's create.
static bool table_new(sim_thread *th, int64_t now_ms) {
    sim_table *t = NULL;
    for (int i = 0; i < th->n_tables; i++)
        if (!th->tables[i].used) { t = &th->tables[i]; break; }
    if (!t) return false;
    int n = 2;
    uint32_t r = rnd(&th->rng, 100);
    if (r >= 90) n = r < 94 ? 3 : r < 97 ? 4 : r < 99 ? 5 : 6;
    int bots = 0;
    if (g_n_bots > 0 && (int)rnd(&th->rng, 100) < g_o.bot_share) bots = n == 2 ? 1 : 1 + (int)rnd(&th->rng, (uint32_t)(n - 1));
    int humans = n - bots;
    int idle = idle_players(th);
    if (idle < humans) {
        // Fill the missing humans with bots, or wait.
        if (g_n_bots > 0 && idle >= 1) { bots = n - idle; humans = idle; }
        else return false;
    }
    memset(t, 0, sizeof *t);
    t->used = true;
    t->n_seats = n;
    t->n_bots = bots;
    t->n_humans = humans;
    t->podkidnoy = (int)rnd(&th->rng, 100) < g_o.podkidnoy;
    for (int b = 0; b < bots; b++) t->bots[b] = bot_pick(th);
    int k = 0;
    for (int i = 0; i < th->n_players && k < humans; i++) {
        player *p = &th->players[i];
        if (p->state != P_IDLE || p->c.ws < 0 || p->idle_until_ms > now_ms) continue;
        t->humans[k++] = p->index;
        p->table = t;
        p->state = P_SETUP;
        p->have = false;
        p->played_v = 0;
        p->think_due_ms = -1;
        p->ready_sent = false;
        p->wait_t0_us = 0;
        atomic_fetch_add(&g_seated, 1);
    }
    t->state = P_SETUP;
    t->step = 0;
    t->created_ms = t->last_change_ms = now_ms;
    player *creator = &g_threads[th->index].players[t->humans[0] - th->players[0].index];
    t->step_pending = send_req(creator, R_CREATE, SF_METH_POST, "/v1/create", NULL, 0, 0, 0);
    if (!t->step_pending) t->state = P_LEAVING;
    return true;
}

static player *player_of(sim_thread *th, int index) { return &th->players[index - th->players[0].index]; }

static void table_free(sim_thread *th, sim_table *t, int64_t now_ms) {
    for (int i = 0; i < t->n_humans; i++) {
        player *p = player_of(th, t->humans[i]);
        if (p->table != t) continue;
        if (p->subscribed && p->c.ws >= 0 && t->gid_len) send_sub(p, t, true);
        p->table = NULL;
        p->have = false;
        p->state = P_IDLE;
        p->think_due_ms = -1;
        p->idle_until_ms = now_ms + IDLE_MIN_MS + (int64_t)rnd(&p->rng, IDLE_MAX_MS - IDLE_MIN_MS);
        atomic_fetch_sub(&g_seated, 1);
    }
    if (t->gid_len) set_gone(t, now_ms);
    atomic_fetch_sub(&g_bots_seated, (unsigned)t->n_bots);
    t->used = false;
}

// The setup asks, one at a time on the creator's socket (rules, bots) and the joiners' (joins).
static void table_setup(sim_thread *th, sim_table *t, int64_t now_ms) {
    if (t->step_pending || !t->gid_len) return;
    player *creator = player_of(th, t->humans[0]);
    TableAsk a;
    int step = t->step++;
    if (step == 0 && t->podkidnoy) {
        ask_init(&a, ASK_SET_RULES, t);
        a.passing = 0;
        t->step_pending = send_ask(creator, ASK_SET_RULES, &a, R_ASK);
        return;
    }
    if (step == 0) step = t->step++;   // no rules to set
    int b = step - 1;
    if (b < t->n_bots) {
        ask_init(&a, ASK_ADD_BOT, t);
        int id = t->bots[b];
        if (id >= 0) {
            const char *bid = be_bots_at(g_bot_pool[id])->id;
            a.bot_len = (uint8_t)strlen(bid);
            memcpy(a.bot, bid, a.bot_len);
        }
        t->step_pending = send_ask(creator, ASK_ADD_BOT, &a, R_ASK);
        if (t->step_pending) atomic_fetch_add(&g_bots_seated, 1);
        return;
    }
    int j = b - t->n_bots + 1;   // the joiner: humans[1..]
    if (j < t->n_humans) {
        player *p = player_of(th, t->humans[j]);
        if (p->c.ws < 0) { t->state = P_LEAVING; return; }
        send_sub(p, t, false);
        ask_init(&a, ASK_JOIN, t);
        t->step_pending = send_ask(p, ASK_JOIN, &a, R_ASK);
        return;
    }
    t->state = P_LOBBY;
    for (int i = 0; i < t->n_humans; i++) player_of(th, t->humans[i])->state = P_LOBBY;
}

static void table_tick(sim_thread *th, sim_table *t, int64_t now_ms) {
    if (!t->used) return;
    player *creator = player_of(th, t->humans[0]);
    switch (t->state) {
    case P_SETUP:
        if (creator->c.ws < 0) { t->state = P_LEAVING; break; }
        table_setup(th, t, now_ms);
        if (now_ms - t->created_ms > 60000) t->state = P_LEAVING;   // a setup that never completes
        break;
    case P_LOBBY: {
        // Every human readies once the table is full; the server deals when all seats are ready.
        bool playing = false;
        for (int i = 0; i < t->n_humans; i++) {
            player *p = player_of(th, t->humans[i]);
            if (!p->have) continue;
            if (p->ct.view.status == GAME_STATUS_PLAYING) playing = true;
            if (!p->ready_sent && p->ct.view.num_players >= t->n_seats && p->c.ws >= 0) {
                TableAsk a;
                ask_init(&a, ASK_START, t);
                p->ready_sent = send_ask(p, ASK_START, &a, R_START);
            }
        }
        if (playing) {
            t->state = P_PLAYING;
            for (int i = 0; i < t->n_humans; i++) player_of(th, t->humans[i])->state = P_PLAYING;
            // The deal is shown once: from the push that dealt, or, when the seat read the dealt
            // board whole (its START reply came after the deal's cast), here.
            if (!t->dealt_ms) {
                t->dealt_ms = now_ms;
                event_at(t, BE_EV_DEALT, -1, NULL, 0, t->version, 0, 0);
            }
        } else if (now_ms - t->last_change_ms > STALL_MS) {
            cb_metrics_add(be_mx, BE_M_STALLS, 1);
            event_at(t, BE_EV_STALL, -1, NULL, 0, t->version, 0, 0);
            t->state = P_LEAVING;
        }
        break;
    }
    case P_PLAYING: {
        bool over = false;
        for (int i = 0; i < t->n_humans; i++) {
            player *p = player_of(th, t->humans[i]);
            if (p->have && p->ct.view.status == GAME_STATUS_GAME_OVER) over = true;
        }
        if (over) {
            t->state = P_OVER;
            t->over_ms = now_ms;
            cb_metrics_rate(be_mx, BE_M_GAMES, cb_now_ms(), 1);
            if (t->dealt_ms) cb_metrics_observe(be_mx, BE_M_GAME_S, (uint64_t)((now_ms - t->dealt_ms) / 1000));
            event_at(t, BE_EV_OVER, creator->have ? creator->ct.view.fool : -1, NULL, 0, t->version, 0, 0);
            t->rematch = (int)rnd(&th->rng, 100) < g_o.rematch;
            for (int i = 0; i < t->n_humans; i++) player_of(th, t->humans[i])->state = P_OVER;
        } else if (now_ms - t->last_change_ms > STALL_MS) {
            cb_metrics_add(be_mx, BE_M_STALLS, 1);
            event_at(t, BE_EV_STALL, -1, NULL, 0, t->version, 0, 0);
            CB_WARN("birdseye", "table %s stalled at v%u: leaving it", t->gid, t->version);
            t->state = P_LEAVING;
        }
        break;
    }
    case P_OVER:
        if (now_ms - t->over_ms < LINGER_MS) break;
        if (t->rematch && creator->c.ws >= 0) {
            TableAsk a;
            ask_init(&a, ASK_CONTINUE, t);
            if (send_ask(creator, ASK_CONTINUE, &a, R_CONTINUE)) {
                t->rematch = false;
                t->state = P_LOBBY;
                t->dealt_ms = 0;
                t->moves = 0;
                t->last_change_ms = now_ms;
                event_at(t, BE_EV_REMATCH, -1, NULL, 0, t->version, 0, 0);
                for (int i = 0; i < t->n_humans; i++) {
                    player *p = player_of(th, t->humans[i]);
                    p->state = P_LOBBY;
                    p->ready_sent = false;
                    p->played_v = 0;
                }
                break;
            }
        }
        t->state = P_LEAVING;
        t->exits_pending = 0;
        for (int i = 0; i < t->n_humans; i++) {
            player *p = player_of(th, t->humans[i]);
            if (p->c.ws < 0) continue;
            TableAsk a;
            ask_init(&a, ASK_EXIT, t);
            if (send_ask(p, ASK_EXIT, &a, R_EXIT)) t->exits_pending++;
            p->state = P_LEAVING;
        }
        t->over_ms = now_ms;
        event_at(t, BE_EV_GONE, -1, NULL, 0, t->version, 0, 0);
        break;
    case P_LEAVING:
        if (t->exits_pending > 0 && now_ms - t->over_ms < 5000) break;
        table_free(th, t, now_ms);
        break;
    default: break;
    }
}

// A player's own clock: a move after its think time, a ping, a socket to reopen.
static void player_tick(sim_thread *th, player *p, int64_t now_ms) {
    if (p->c.ws < 0) {
        if (now_ms < p->reopen_ms) return;
        p->reopen_ms = now_ms + RECONNECT_MS;
        if (be_client_sign_in(&p->c, g_port) && open_stream(p) && p->table && p->table->gid_len) refetch(p);
        return;
    }
    be_client_ping(&p->c, now_ms);
    if (p->state != P_PLAYING || !p->have || !p->table) return;
    const TableView *v = &p->ct.view;
    if (v->status != GAME_STATUS_PLAYING) return;
    if (p->think_due_ms < 0) {
        if (v->version <= p->played_v || p->wait_t0_us) return;   // moved on this board, or a move is in flight
        uint8_t w[2 + 2 * AWIRE_MAX_CARDS];
        if (legal_wire(th, p, w, sizeof w) <= 0) return;
        int span = g_o.think_max_ms - g_o.think_min_ms;
        p->think_due_ms = now_ms + g_o.think_min_ms + (span > 0 ? (int64_t)rnd(&p->rng, (uint32_t)span) : 0);
        return;
    }
    if (now_ms < p->think_due_ms) return;
    p->think_due_ms = -1;
    if (v->version <= p->played_v) return;
    send_move(th, p, now_ms);
}

// ---- the thread ------------------------------------------------------------------------------------

static void thread_signup(sim_thread *th) {
    for (int i = 0; i < th->n_players && !atomic_load(&g_stop); i++) {
        player *p = &th->players[i];
        for (int tries = 0; tries < 5 && !be_client_sign_in(&p->c, g_port); tries++) nap_ms(500);
        if (p->c.token[0]) open_stream(p);
    }
}

// GET /v1/bots (board.h's roster, every bot), and the kinds the mixed sim seats.
static void fetch_bots(player *p) {
    int n = be_bots_fetch(g_port, p->c.token);
    static const char *const WANTED[] = { "handwritten", "robusta", "cordite", "firecracker" };
    for (int i = 0; i < n && g_n_bots < MAX_BOT_IDS; i++)
        for (size_t k = 0; k < sizeof WANTED / sizeof *WANTED; k++)
            if (strcmp(be_bots_at(i)->key, WANTED[k]) == 0) g_bot_pool[g_n_bots++] = i;
    CB_INFO("birdseye", "bots: %d of %d on the server are the kinds the sim seats", g_n_bots, n);
}

static void *thread_main(void *arg) {
    sim_thread *th = arg;
    thread_signup(th);
    if (th->index == 0 && th->n_players > 0 && th->players[0].c.token[0]) fetch_bots(&th->players[0]);
    // Every thread's sign-ups done before any table is made, so the bot list is complete.
    pthread_mutex_lock(&g_ready_mu);
    g_n_ready++;
    pthread_cond_broadcast(&g_ready_cv);
    while (g_n_ready < g_o.threads && !atomic_load(&g_stop)) pthread_cond_wait(&g_ready_cv, &g_ready_mu);
    pthread_mutex_unlock(&g_ready_mu);

    while (!atomic_load(&g_stop)) {
        int64_t now_ms = cb_mono_ms();
        int live = 0;
        for (int i = 0; i < th->n_tables; i++) {
            table_tick(th, &th->tables[i], now_ms);
            if (th->tables[i].used) live++;
        }
        while (live < th->target && table_new(th, now_ms)) live++;
        int64_t next = now_ms + 100;
        int nfd = 0;
        for (int i = 0; i < th->n_players; i++) {
            player *p = &th->players[i];
            player_tick(th, p, now_ms);
            if (p->think_due_ms >= 0 && p->think_due_ms < next) next = p->think_due_ms;
            if (p->c.ws >= 0) {
                th->fds[nfd].fd = p->c.ws;
                th->fds[nfd].events = POLLIN;
                th->fds[nfd].revents = 0;
                nfd++;
            }
        }
        int to = (int)(next - cb_mono_ms());
        if (to < 1) to = 1;
        if (nfd == 0) {
            nap_ms(to);
            continue;
        }
        int r = poll(th->fds, (nfds_t)nfd, to);
        if (r <= 0) continue;
        now_ms = cb_mono_ms();
        int k = 0;
        for (int i = 0; i < th->n_players; i++) {
            player *p = &th->players[i];
            if (p->c.ws < 0) continue;
            // fds[k] is this player's: the list was built in player order over open sockets.
            if (th->fds[k].fd == p->c.ws && (th->fds[k].revents & (POLLIN | POLLHUP | POLLERR))) read_socket(th, p, now_ms);
            k++;
        }
    }
    // Leave politely: every seated player exits its table, then the sockets close.
    for (int i = 0; i < th->n_tables; i++) {
        sim_table *t = &th->tables[i];
        if (!t->used || !t->gid_len) continue;
        for (int h = 0; h < t->n_humans; h++) {
            player *p = player_of(th, t->humans[h]);
            TableAsk a;
            ask_init(&a, ASK_EXIT, t);
            if (p->c.ws >= 0) send_ask(p, ASK_EXIT, &a, R_EXIT);
        }
    }
    nap_ms(300);
    for (int i = 0; i < th->n_players; i++) close_stream(&th->players[i]);
    return NULL;
}

// ---- start, stop, ops ------------------------------------------------------------------------------

int be_sim_start(const be_sim_opts *o, char *err, size_t cap) {
    g_o = *o;
    const char *colon = strrchr(o->server, ':');
    g_port = colon ? atoi(colon + 1) : 0;
    if (g_port <= 0 || g_port > 65535) {
        snprintf(err, cap, "birdseye: BE_SERVER '%s' names no port", o->server);
        return CB_E_INVALID;
    }
    if (o->threads > o->players) g_o.threads = o->players;
    atomic_store(&g_stop, false);
    g_n_ready = 0;
    int per = o->players / g_o.threads, extra = o->players % g_o.threads;
    int tables_per = o->tables / g_o.threads, tables_extra = o->tables % g_o.threads;
    int next_index = 0;
    for (int t = 0; t < g_o.threads; t++) {
        sim_thread *th = &g_threads[t];
        memset(th, 0, sizeof *th);
        th->index = t;
        th->n_players = per + (t < extra ? 1 : 0);
        th->target = tables_per + (t < tables_extra ? 1 : 0);
        th->n_tables = th->n_players + 1;   // never more tables than players
        th->rng = (o->seed + 1) * 0x9E3779B97F4A7C15ull ^ (uint64_t)(t + 1) * 0xD1B54A32D192ED03ull;
        th->players = calloc((size_t)th->n_players, sizeof *th->players);
        th->tables = calloc((size_t)th->n_tables, sizeof *th->tables);
        th->fds = calloc((size_t)th->n_players, sizeof *th->fds);
        if (!th->players || !th->tables || !th->fds) {
            snprintf(err, cap, "birdseye: no memory for %d players", o->players);
            return CB_E_NOMEM;
        }
        for (int i = 0; i < th->n_players; i++) {
            player *p = &th->players[i];
            p->index = next_index++;
            p->thread = t;
            p->think_due_ms = -1;
            p->rng = th->rng ^ ((uint64_t)(p->index + 1) * 0xBF58476D1CE4E5B9ull);
            p->delta = calloc(1, sizeof *p->delta);
            // Names: a first name from the pool, the seed and the index, upper-cased as the hosts do.
            char buf[32], name[32], password[40];
            snprintf(buf, sizeof buf, "%s%llX%d", FIRST_NAMES[p->index % NAME_POOL], (unsigned long long)(o->seed & 0xFFF), p->index);
            size_t k = 0;
            for (; buf[k]; k++) name[k] = (char)(buf[k] >= 'a' && buf[k] <= 'z' ? buf[k] - 32 : buf[k]);
            name[k] = 0;
            snprintf(password, sizeof password, "birdseye-pass-%llu", (unsigned long long)o->seed);
            if (!be_client_init(&p->c, name, password) || !p->delta) {
                snprintf(err, cap, "birdseye: no memory for %d players", o->players);
                return CB_E_NOMEM;
            }
        }
    }
    for (int t = 0; t < g_o.threads; t++)
        if (pthread_create(&g_threads[t].tid, NULL, thread_main, &g_threads[t]) != 0) {
            snprintf(err, cap, "birdseye: cannot start thread %d", t);
            atomic_store(&g_stop, true);
            return CB_E_IO;
        }
    return CB_OK;
}

void be_sim_stop(void) {
    atomic_store(&g_stop, true);
    pthread_mutex_lock(&g_ready_mu);
    pthread_cond_broadcast(&g_ready_cv);
    pthread_mutex_unlock(&g_ready_mu);
    for (int t = 0; t < g_o.threads; t++) {
        sim_thread *th = &g_threads[t];
        if (th->tid) pthread_join(th->tid, NULL);
        for (int i = 0; i < th->n_players; i++) {
            be_client_free(&th->players[i].c);
            free(th->players[i].delta);
        }
        free(th->players);
        free(th->tables);
        free(th->fds);
        memset(th, 0, sizeof *th);
    }
}

void be_sim_gauges(void) {
    cb_metrics_set(be_mx, BE_M_PLAYERS, (uint64_t)g_o.players);
    cb_metrics_set(be_mx, BE_M_PLAYERS_SEATED, atomic_load(&g_seated));
    cb_metrics_set(be_mx, BE_M_BOTS_SEATED, atomic_load(&g_bots_seated));
    cb_metrics_set(be_mx, BE_M_THREADS, (uint64_t)g_o.threads);
}

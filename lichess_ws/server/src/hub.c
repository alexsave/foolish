// hub.c - the loop-thread half of the gateway (lws.h): every socket after its upgrade, every
// line from lila on Redis, every timer. One thread, no locks: a message is parsed, acted on
// and its fan-out written into each hearer's send buffer before the next one is read, and the
// loop flushes the buffers once per pass (net/server.c).
//
// The behaviours here are lila-ws's, as read in SCOPE.md 1.1: the names of the sections below
// are the names of its files (Users, RoundCrowd, History, Fens, Lobby, Lag, KeepAlive, the
// handlers of LilaHandler and the actors), so a reader of either can find the other.
#include "lws.h"

#include "json_wire.h"
#include "lila_wire.h"
#include "resp.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD "lws"

// Channels toward lila (Lila.scala chans): one lane each, but the round's 16.
typedef enum { CH_SITE = 0, CH_LOBBY, CH_ROUND, CH_TOUR, CH_SWISS, CH_SIMUL, CH_N } chan_kind;
static const char *const CHAN_IN[CH_N] = { "site-in", "lobby-in", "r-in", "tour-in", "swiss-in", "simul-in" };
#define ROUND_LANES 16

// The flags a ?flag= can carry, each its own bus channel (Bus.channel.flag).
static const char *const FLAGS[] = { "api", "embed", "simul", "tournament" };
#define N_FLAGS 4

typedef struct { lnode conns; int n; } lws_user;   // Users: a user's open sockets
typedef struct { lnode conns; int n; } lws_sri;    // Bus.channel.sri
typedef struct { int lag_ms; int64_t at_ms; } lag_rec;   // Lag.trustedStats
typedef struct { char ids[5][9]; int n; int64_t at_ms; } pending_games;   // Lobby.PendingGamesPerIp
typedef struct { int count; int64_t day_ms; } ip_joins;                   // Lobby.byIpRateLimit

static struct {
    cb_ctx      *ctx;
    cb_loop     *loop;
    resp_client *redis;
    smap         rooms, users, sris;
    lnode        all, lobby, round_players, mlat, flags[N_FLAGS];
    int          n_conns;
    bool         online;          // lila (Lila.currentStatus)
    cb_buf       critical;        // lines to resend once lila is back: [u16 chan][u32 len][bytes]...
    int64_t      started_ms;
    // RoundCrowd's batch
    lws_room   **dirty; int n_dirty, cap_dirty; uint64_t dirty_timer;
    // Lag
    smap         client_lags;     // user -> (uintptr_t) ms, the batch toward lila
    int          n_client_lags; uint64_t lags_timer;
    smap         trusted;         // user -> lag_rec
    // Lobby's batches
    cb_buf       lobby_connects; int n_lobby_connects; uint64_t lobby_connect_timer;
    cb_buf       lobby_disconnects; int n_lobby_disconnects; uint64_t lobby_disconnect_timer;
    smap         pending, ip_joins;
    // Users' disconnect batch, the notified batch
    smap         disconnects, notified; uint64_t notified_timer;
    // the lobby's pong (Monitor.periodicMetrics)
    int          pong_members, pong_rounds;
    long         events, dropped;
    cb_buf       scratch, line;
} H;

static void send_buf(lws_conn *c, const cb_buf *b) { cb_ws_send_text(c->ws, cb_buf_cstr(b), b->len); }
static void send_str(lws_conn *c, const char *s) { cb_ws_send_text(c->ws, s, strlen(s)); }
static int now_s(void) { return (int)(cb_now_ms() / 1000); }

// ---- toward lila (Lila.scala emit) ------------------------------------------------------------

static void chan_name(chan_kind k, const char *line, size_t n, char out[32]) {
    if (k == CH_ROUND) snprintf(out, 32, "r-in:%d", li_round_lane(line, n));
    else snprintf(out, 32, "%s", CHAN_IN[k]);
}

static void buffer_critical(chan_kind k, const cb_buf *line) {
    uint16_t ck = (uint16_t)k;
    uint32_t n = (uint32_t)line->len;
    cb_buf_append(&H.critical, &ck, 2);
    cb_buf_append(&H.critical, &n, 4);
    cb_buf_append(&H.critical, line->p, line->len);
}

static void emit_line(chan_kind k, const cb_buf *line, bool critical, bool always) {
    char chan[32];
    chan_name(k, cb_buf_cstr(line), line->len, chan);
    if (H.online || always) {
        if (resp_publish(H.redis, chan, cb_buf_cstr(line), line->len) != CB_OK && critical) buffer_critical(k, line);
    } else if (critical) buffer_critical(k, line);
    else H.dropped++;
}
static void emit(chan_kind k, const cb_buf *line) { emit_line(k, line, false, false); }

static void flush_critical(void) {
    size_t at = 0;
    while (at + 6 <= H.critical.len) {
        uint16_t ck; uint32_t n;
        memcpy(&ck, H.critical.p + at, 2);
        memcpy(&n, H.critical.p + at + 2, 4);
        at += 6;
        if (at + n > H.critical.len) break;
        char chan[32];
        chan_name((chan_kind)ck, (const char *)H.critical.p + at, n, chan);
        resp_publish(H.redis, chan, (const char *)H.critical.p + at, n);
        at += n;
    }
    cb_buf_reset(&H.critical);
}

// ---- rooms ------------------------------------------------------------------------------------

static lws_room *room_get(const char *id, bool create, bool round) {
    lws_room *r = smap_get(&H.rooms, id);
    if (r || !create) return r;
    r = calloc(1, sizeof *r);
    if (!r) return NULL;
    cb_strlcpy(r->id, id, sizeof r->id);
    r->round = round;
    lnode_init(&r->members);
    smap_init(&r->users);
    if (!smap_put(&H.rooms, id, r)) { free(r); return NULL; }
    return r;
}

static void event_free(lws_event *e) { free(e->full); free(e->nodests); memset(e, 0, sizeof *e); }

static void room_free_if_empty(lws_room *r) {
    if (r->n_members || r->hist_n || r->n_fen || r->players[0] || r->players[1] || r->crowd_dirty) return;
    smap_del(&H.rooms, r->id);
    smap_free(&r->users);
    free(r->fen_watchers);
    for (int i = 0; i < LWS_HIST; i++) event_free(&r->hist[i]);
    free(r);
}

static bool room_crowded(const lws_room *r) { return r->n_members > 0 || r->players[0] > 0 || r->players[1] > 0; }

static void room_publish(lws_room *r, const cb_buf *b) {
    LNODE_EACH(&r->members, it, nx) send_buf(LNODE_OWNER(it, lws_conn, n_room), b);
}

// ---- RoundCrowd: who is in a game, batched toward lila (r/ons) and the room (crowd) ----------

static void crowd_flush(cb_loop *l, void *ud);

static void crowd_mark(lws_room *r) {
    if (r->crowd_dirty) return;
    if (H.n_dirty == H.cap_dirty) {
        int ncap = H.cap_dirty ? H.cap_dirty * 2 : 64;
        lws_room **nd = realloc(H.dirty, (size_t)ncap * sizeof *nd);
        if (!nd) return;
        H.dirty = nd;
        H.cap_dirty = ncap;
    }
    r->crowd_dirty = true;
    H.dirty[H.n_dirty++] = r;
    if (H.n_dirty >= 512) crowd_flush(H.loop, NULL);
    else if (!H.dirty_timer) H.dirty_timer = cb_loop_timer(H.loop, 700, crowd_flush, NULL);
}

static void crowd_connect(lws_room *r, lws_conn *c) {
    if (c->is_player) r->players[c->color]++;
    else if (c->user[0]) {
        uintptr_t n = (uintptr_t)smap_get(&r->users, c->user);
        smap_put(&r->users, c->user, (void *)(n + 1));
    } else r->anons++;
    crowd_mark(r);
}

static void crowd_disconnect(lws_room *r, lws_conn *c) {
    if (c->is_player) { if (r->players[c->color] > 0) r->players[c->color]--; }
    else if (c->user[0]) {
        uintptr_t n = (uintptr_t)smap_get(&r->users, c->user);
        if (n <= 1) smap_del(&r->users, c->user);
        else smap_put(&r->users, c->user, (void *)(n - 1));
    } else r->anons--;
    crowd_mark(r);
}

// The names of a round's watchers (at most 10, else the count): each socket carries its own.
static int crowd_names(lws_room *r, const char *names[10]) {
    int n = 0;
    LNODE_EACH(&r->members, it, nx) {
        lws_conn *c = LNODE_OWNER(it, lws_conn, n_room);
        if (c->is_player || !c->user[0] || !c->name[0] || strncmp(c->name, "BOT ", 4) == 0) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) if (strcmp(names[i], c->name) == 0) dup = true;
        if (dup) continue;
        if (n < 10) names[n] = c->name;
        n++;
    }
    return n;
}

static void crowd_flush(cb_loop *l, void *ud) {
    H.dirty_timer = 0;
    if (!H.n_dirty) return;
    // r/ons: every dirty game, "<id>" when no player is online, "<id><w><b>" otherwise
    cb_buf *csv = &H.scratch;
    cb_buf_reset(csv);
    for (int i = 0; i < H.n_dirty; i++) {
        lws_room *r = H.dirty[i];
        if (!r->round) continue;
        if (csv->len) cb_buf_putc(csv, ',');
        cb_buf_puts(csv, r->id);
        if (r->players[0] || r->players[1]) cb_buf_printf(csv, "%c%c", r->players[0] ? '+' : '-', r->players[1] ? '+' : '-');
    }
    if (csv->len) {
        cb_buf_reset(&H.line);
        li_r_ons(&H.line, cb_buf_cstr(csv));
        emit(CH_ROUND, &H.line);
    }
    for (int i = 0; i < H.n_dirty; i++) {
        lws_room *r = H.dirty[i];
        r->crowd_dirty = false;
        if (!r->round) { room_free_if_empty(r); continue; }
        const char *names[10];
        int nb = (int)smap_size(&r->users);
        int nn = crowd_names(r, names);
        cb_buf_reset(&H.line);
        ci_round_crowd(&H.line, r->players[0] > 0, r->players[1] > 0, nb, nn <= 10 ? names : NULL, nn <= 10 ? nn : 0);
        // the "d" object is what RoundClientActor compares; the frame is {"t":"crowd","d":...}
        const char *d = cb_buf_cstr(&H.line) + 18;
        size_t dlen = H.line.len - 19;
        LNODE_EACH(&r->members, it, nx) {
            lws_conn *c = LNODE_OWNER(it, lws_conn, n_room);
            if (c->last_crowd && strlen(c->last_crowd) == dlen && memcmp(c->last_crowd, d, dlen) == 0) continue;
            free(c->last_crowd);
            c->last_crowd = strndup(d, dlen);
            send_buf(c, &H.line);
        }
        room_free_if_empty(r);
    }
    H.n_dirty = 0;
}

// On lila's VersioningReady, 15 s later: every game with a player online (RoundCrowd.emitAllOnline).
static bool emit_online_each(const char *key, void *val, void *ud) {
    lws_room *r = val;
    if (!r->round || (!r->players[0] && !r->players[1])) return true;
    cb_buf *csv = ud;
    if (csv->len) cb_buf_putc(csv, ',');
    cb_buf_printf(csv, "%s%c%c", r->id, r->players[0] ? '+' : '-', r->players[1] ? '+' : '-');
    if (csv->len > 1024 * 12) {   // lila-ws groups 1024 rooms per line
        cb_buf_reset(&H.line);
        li_r_ons(&H.line, cb_buf_cstr(csv));
        emit(CH_ROUND, &H.line);
        cb_buf_reset(csv);
    }
    return true;
}
static void emit_all_online(cb_loop *l, void *ud) {
    cb_buf csv;
    cb_buf_init(&csv);
    smap_each(&H.rooms, emit_online_each, &csv);
    if (csv.len) {
        cb_buf_reset(&H.line);
        li_r_ons(&H.line, cb_buf_cstr(&csv));
        emit(CH_ROUND, &H.line);
    }
    cb_buf_free(&csv);
}

// ---- History: the last 20 versioned events of a room, for resync -----------------------------

static lws_event *history_push(lws_room *r) {
    lws_event *e = &r->hist[r->hist_head];
    event_free(e);
    r->hist_head = (r->hist_head + 1) % LWS_HIST;
    if (r->hist_n < LWS_HIST) r->hist_n++;
    return e;
}
static lws_event *history_at(lws_room *r, int newest_minus) {   // 0: the newest
    return &r->hist[(r->hist_head - 1 - newest_minus + 2 * LWS_HIST) % LWS_HIST];
}

static const char *event_for(lws_conn *c, lws_event *e, size_t *len) {
    lws_room *r = c->room;
    if (r->round) {
        if ((e->f_troll && !c->troll) || (e->f_owner && !c->is_player) || (e->f_watcher && c->is_player) ||
            (e->f_player >= 0 && (!c->is_player || c->color != e->f_player))) {
            cb_buf_reset(&H.scratch);
            ci_skip(&H.scratch, e->v);
            *len = H.scratch.len;
            return cb_buf_cstr(&H.scratch);
        }
        if (e->f_move_by >= 0 && (!c->is_player || c->color == e->f_move_by)) {
            if (!e->nodests) {
                cb_buf b;
                cb_buf_init(&b);
                ci_strip_dests(&b, e->full, e->full_len);
                e->nodests = (char *)cb_buf_take(&b, &e->nodests_len);
                if (!e->nodests) { *len = e->full_len; return e->full; }
            }
            *len = e->nodests_len;
            return e->nodests;
        }
        *len = e->full_len;
        return e->full;
    }
    if (!e->f_troll || c->troll) { *len = e->full_len; return e->full; }
    cb_buf_reset(&H.scratch);
    ci_skip(&H.scratch, e->v);
    *len = H.scratch.len;
    return cb_buf_cstr(&H.scratch);
}

static void send_event(lws_conn *c, lws_event *e) {
    size_t n;
    const char *p = event_for(c, e, &n);
    cb_ws_send_text(c->ws, p, n);
}

// History.getFrom: no version asked: the newest 5, oldest first; a version: every event after
// it when they are all here, nothing when it is current, else a resync.
static void history_catch_up(lws_conn *c) {
    lws_room *r = c->room;
    if (!c->has_from) {
        int n = r->hist_n < 5 ? r->hist_n : 5;
        for (int i = n - 1; i >= 0; i--) send_event(c, history_at(r, i));
        return;
    }
    if (r->hist_n == 0 || history_at(r, 0)->v <= c->from_version) return;
    int n = 0;
    while (n < r->hist_n && history_at(r, n)->v > c->from_version) n++;
    if (n != history_at(r, 0)->v - c->from_version) { send_str(c, "{\"t\":\"resync\"}"); return; }
    for (int i = n - 1; i >= 0; i--) send_event(c, history_at(r, i));
}

// ---- Fens: the position of watched games, pushed to the watchers ------------------------------

static void fens_watch(lws_conn *c, lws_room *r) {
    if (r->n_fen == r->cap_fen) {
        int ncap = r->cap_fen ? r->cap_fen * 2 : 4;
        lws_conn **nw = realloc(r->fen_watchers, (size_t)ncap * sizeof *nw);
        if (!nw) return;
        r->fen_watchers = nw;
        r->cap_fen = ncap;
    }
    r->fen_watchers[r->n_fen++] = c;
    if (r->has_pos) {
        cb_buf_reset(&H.line);
        ci_fen(&H.line, r->id, r->pos_uci, r->pos_fen, r->pos_turn, r->pos_clock, r->pos_wc, r->pos_bc);
        send_buf(c, &H.line);
    }
}

static void fens_unwatch(lws_conn *c, lws_room *r) {
    for (int i = 0; i < r->n_fen; i++)
        if (r->fen_watchers[i] == c) { r->fen_watchers[i] = r->fen_watchers[--r->n_fen]; break; }
    if (r->n_fen == 0) { r->has_pos = false; room_free_if_empty(r); }
}

static void conn_unwatch_all(lws_conn *c) {
    for (int i = 0; i < c->n_watched; i++) fens_unwatch(c, c->watched[i]);
    c->n_watched = 0;
}

// ClientOut.Watch: a bounded set of 16, the oldest evicted (SmallBoundedQueueSet).
static void watch(lws_conn *c, const co_msg *m) {
    for (int i = 0; i < m->n_watch; i++) {
        bool have = false;
        for (int j = 0; j < c->n_watched; j++) if (strcmp(c->watched[j]->id, m->watch[i]) == 0) have = true;
        if (have) continue;
        lws_room *r = room_get(m->watch[i], true, true);
        if (!r) continue;
        if (c->n_watched == LWS_WATCH_MAX) {
            fens_unwatch(c, c->watched[0]);
            memmove(c->watched, c->watched + 1, (LWS_WATCH_MAX - 1) * sizeof c->watched[0]);
            c->n_watched--;
        }
        c->watched[c->n_watched++] = r;
        fens_watch(c, r);
    }
}

static void fens_move(lws_room *r, const char *data, size_t n, int move_by) {
    if (r->n_fen == 0) return;   // only watched games keep a position (Fens.games.computeIfPresent)
    char uci[8], fen[96];
    bool clock; int wc, bc;
    if (!co_move_position(data, n, uci, fen, &clock, &wc, &bc)) return;
    r->has_pos = true;
    cb_strlcpy(r->pos_uci, uci, sizeof r->pos_uci);
    cb_strlcpy(r->pos_fen, fen, sizeof r->pos_fen);
    r->pos_clock = clock;
    r->pos_wc = wc;
    r->pos_bc = bc;
    r->pos_turn = move_by < 0 ? 'w' : (move_by == 0 ? 'b' : 'w');
    cb_buf_reset(&H.line);
    ci_fen(&H.line, r->id, r->pos_uci, r->pos_fen, r->pos_turn, r->pos_clock, r->pos_wc, r->pos_bc);
    for (int i = 0; i < r->n_fen; i++) send_buf(r->fen_watchers[i], &H.line);
}

static void fens_finish(lws_room *r, int winner) {
    if (r->n_fen == 0) return;
    cb_buf_reset(&H.line);
    ci_finish(&H.line, r->id, winner);
    for (int i = 0; i < r->n_fen; i++) {
        lws_conn *c = r->fen_watchers[i];
        send_buf(c, &H.line);
        for (int j = 0; j < c->n_watched; j++)
            if (c->watched[j] == r) { memmove(c->watched + j, c->watched + j + 1, (size_t)(c->n_watched - j - 1) * sizeof c->watched[0]); c->n_watched--; break; }
    }
    r->n_fen = 0;
    r->has_pos = false;
}

// ---- Users and sris -----------------------------------------------------------------------------

static void users_connect(lws_conn *c, bool silently) {
    lws_user *u = smap_get(&H.users, c->user);
    if (!u) {
        u = calloc(1, sizeof *u);
        if (!u || !smap_put(&H.users, c->user, u)) { free(u); return; }
        lnode_init(&u->conns);
        if (!smap_del(&H.disconnects, c->user) && !silently) {
            cb_buf_reset(&H.line);
            li_connect_user(&H.line, c->user);
            emit(CH_SITE, &H.line);
        }
    }
    lnode_push(&u->conns, &c->n_user);
    u->n++;
}

static void users_disconnect(lws_conn *c) {
    lws_user *u = smap_get(&H.users, c->user);
    if (!u || !lnode_linked(&c->n_user)) return;
    lnode_unlink(&c->n_user);
    if (--u->n == 0) {
        smap_del(&H.users, c->user);
        free(u);
        smap_put(&H.disconnects, c->user, (void *)1);
    }
}

static void users_tell(const char *user, const cb_buf *b, lws_kind only_kind, const lws_room *only_room, bool any) {
    lws_user *u = smap_get(&H.users, user);
    if (!u) return;
    LNODE_EACH(&u->conns, it, nx) {
        lws_conn *c = LNODE_OWNER(it, lws_conn, n_user);
        if (any || (c->kind == only_kind && (!only_room || c->room == only_room))) send_buf(c, b);
    }
}

static void sris_add(lws_conn *c) {
    lws_sri *s = smap_get(&H.sris, c->sri);
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s || !smap_put(&H.sris, c->sri, s)) { free(s); return; }
        lnode_init(&s->conns);
    }
    lnode_push(&s->conns, &c->n_sri);
    s->n++;
}

static void sris_remove(lws_conn *c) {
    lws_sri *s = smap_get(&H.sris, c->sri);
    if (!s || !lnode_linked(&c->n_sri)) return;
    lnode_unlink(&c->n_sri);
    if (--s->n == 0) { smap_del(&H.sris, c->sri); free(s); }
}

static void sri_tell(const char *sri, const cb_buf *b) {
    lws_sri *s = smap_get(&H.sris, sri);
    if (!s) return;
    LNODE_EACH(&s->conns, it, nx) send_buf(LNODE_OWNER(it, lws_conn, n_sri), b);
}

static int flag_index(const char *flag) {
    for (int i = 0; i < N_FLAGS; i++) if (strcmp(flag, FLAGS[i]) == 0) return i;
    return -1;
}

// ---- Lobby: connect/disconnect batches, anonymous flood control --------------------------------

static void lobby_connect_flush(cb_loop *l, void *ud) {
    H.lobby_connect_timer = 0;
    if (!H.n_lobby_connects) return;
    cb_buf_reset(&H.line);
    li_connect_sris(&H.line, cb_buf_cstr(&H.lobby_connects));
    emit(CH_LOBBY, &H.line);
    cb_buf_reset(&H.lobby_connects);
    H.n_lobby_connects = 0;
}
static void lobby_on_connect(lws_conn *c) {
    if (H.lobby_connects.len) cb_buf_putc(&H.lobby_connects, ',');
    cb_buf_puts(&H.lobby_connects, c->sri);
    if (c->user[0]) { cb_buf_putc(&H.lobby_connects, ' '); cb_buf_puts(&H.lobby_connects, c->user); }
    if (++H.n_lobby_connects >= 6) lobby_connect_flush(H.loop, NULL);
    else if (!H.lobby_connect_timer) H.lobby_connect_timer = cb_loop_timer(H.loop, 479, lobby_connect_flush, NULL);
}
static void lobby_disconnect_flush(cb_loop *l, void *ud) {
    H.lobby_disconnect_timer = 0;
    if (!H.n_lobby_disconnects) return;
    cb_buf_reset(&H.line);
    li_disconnect_sris(&H.line, cb_buf_cstr(&H.lobby_disconnects));
    emit(CH_LOBBY, &H.line);
    cb_buf_reset(&H.lobby_disconnects);
    H.n_lobby_disconnects = 0;
}
static void lobby_on_disconnect(lws_conn *c) {
    if (H.lobby_disconnects.len) cb_buf_putc(&H.lobby_disconnects, ',');
    cb_buf_puts(&H.lobby_disconnects, c->sri);
    if (++H.n_lobby_disconnects >= 50) lobby_disconnect_flush(H.loop, NULL);
    else if (!H.lobby_disconnect_timer) H.lobby_disconnect_timer = cb_loop_timer(H.loop, 487, lobby_disconnect_flush, NULL);
}

static pending_games *pending_get(const char *ip) {
    pending_games *p = smap_get(&H.pending, ip);
    if (p && cb_now_ms() - p->at_ms > 3600 * 1000) { smap_del(&H.pending, ip); free(p); p = NULL; }
    return p;
}
static bool anon_can_join(lws_conn *c) {
    ip_joins *j = smap_get(&H.ip_joins, c->ip);
    int64_t now = cb_now_ms();
    if (!j) {
        j = calloc(1, sizeof *j);
        if (!j || !smap_put(&H.ip_joins, c->ip, j)) { free(j); return true; }
        j->day_ms = now;
    }
    if (now - j->day_ms > 86400 * 1000) { j->count = 0; j->day_ms = now; }
    if (++j->count > 250) return false;
    if (c->user[0]) return true;
    pending_games *p = pending_get(c->ip);
    if (p && p->n >= 5) { CB_INFO(MOD, "anon %s cannot join a lobby game: %d pending", c->ip, p->n); return false; }
    return true;
}
// A redirect sent to an anonymous lobby socket counts as a pending game for its IP.
static void anon_tap(lws_conn *c, const cb_buf *b) {
    if (c->user[0] || c->kind != K_LOBBY) return;
    const char *s = cb_buf_cstr(b);
    if (!strstr(s, "t\":\"redirect\",")) return;
    const char *id = strstr(s, "\"id\":\"");
    if (!id || strlen(id) < 14) return;
    id += 6;
    pending_games *p = pending_get(c->ip);
    if (!p) {
        p = calloc(1, sizeof *p);
        if (!p || !smap_put(&H.pending, c->ip, p)) { free(p); return; }
    }
    p->at_ms = cb_now_ms();
    for (int i = 0; i < p->n; i++) if (strncmp(p->ids[i], id, 8) == 0) return;
    if (p->n < 5) { memcpy(p->ids[p->n], id, 8); p->ids[p->n][8] = 0; p->n++; }
}
static void anon_played(lws_conn *c) {
    if (c->user[0]) return;
    pending_games *p = pending_get(c->ip);
    if (!p) return;
    for (int i = 0; i < p->n; i++)
        if (strcmp(p->ids[i], c->room->id) == 0) { p->ids[i][0] = 0; memmove(p->ids + i, p->ids + i + 1, (size_t)(p->n - i - 1) * sizeof p->ids[0]); p->n--; break; }
    if (p->n == 0) { smap_del(&H.pending, c->ip); free(p); }
}

// ---- Lag ----------------------------------------------------------------------------------------

static bool lags_each(const char *key, void *val, void *ud) {
    cb_buf *csv = ud;
    if (csv->len) cb_buf_putc(csv, ',');
    cb_buf_printf(csv, "%s:%d", key, (int)(uintptr_t)val);
    return true;
}
static void lags_flush(cb_loop *l, void *ud) {
    H.lags_timer = 0;
    if (!smap_size(&H.client_lags)) return;
    cb_buf_reset(&H.scratch);
    smap_each(&H.client_lags, lags_each, &H.scratch);
    cb_buf_reset(&H.line);
    li_lags(&H.line, cb_buf_cstr(&H.scratch));
    emit(CH_SITE, &H.line);
    smap_free(&H.client_lags);
    H.n_client_lags = 0;
}
static void lag_client(lws_conn *c, int ms) {
    if (!c->user[0]) return;
    smap_put(&H.client_lags, c->user, (void *)(uintptr_t)ms);
    if (++H.n_client_lags >= 256) lags_flush(H.loop, NULL);
    else if (!H.lags_timer) H.lags_timer = cb_loop_timer(H.loop, 947, lags_flush, NULL);
}
static int lag_session(const char *user) {   // ms, or -1
    lag_rec *r = smap_get(&H.trusted, user);
    if (!r) return -1;
    if (cb_now_ms() - r->at_ms > 3600 * 1000) { smap_del(&H.trusted, user); free(r); return -1; }
    return r->lag_ms;
}
static void lag_trusted(lws_conn *c, int ms) {
    if (!c->user[0]) return;
    int capped = ms < 5000 ? ms : 5000;
    lag_rec *r = smap_get(&H.trusted, c->user);
    if (!r) {
        r = calloc(1, sizeof *r);
        if (!r || !smap_put(&H.trusted, c->user, r)) { free(r); return; }
        r->lag_ms = capped;
    } else r->lag_ms = (int)(r->lag_ms * 0.9f + capped * 0.1f);
    r->at_ms = cb_now_ms();
}

static int centis_of_ms(int ms) { return (ms > 0 ? ms + 5 : ms - 4) / 10; }

// ---- the notified batch ----

static bool notified_each(const char *key, void *val, void *ud) { cb_buf *csv = ud; if (csv->len) cb_buf_putc(csv, ','); cb_buf_puts(csv, key); return true; }
static void notified_flush(cb_loop *l, void *ud) {
    H.notified_timer = 0;
    if (!smap_size(&H.notified)) return;
    cb_buf_reset(&H.scratch);
    smap_each(&H.notified, notified_each, &H.scratch);
    cb_buf_reset(&H.line);
    li_notified_batch(&H.line, cb_buf_cstr(&H.scratch));
    emit(CH_SITE, &H.line);
    smap_free(&H.notified);
}

// ---- the sockets ------------------------------------------------------------------------------

lws_conn *lws_conn_new(lws_kind kind, const char *sri, const char *user, const char *ip, const char *flag, int credits, int interval_s) {
    lws_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->kind = kind;
    cb_strlcpy(c->sri, sri, sizeof c->sri);
    cb_strlcpy(c->user, user ? user : "", sizeof c->user);
    cb_strlcpy(c->ip, ip, sizeof c->ip);
    cb_strlcpy(c->flag, flag ? flag : "", sizeof c->flag);
    c->color = -1;
    c->credits = c->max_credits = credits;
    c->interval_ms = interval_s * 1000;
    c->clear_at_ms = cb_now_ms() + c->interval_ms;
    return c;
}

void lws_conn_free(lws_conn *c) {
    if (!c) return;
    free(c->last_crowd);
    free(c);
}

// RateLimit.apply: pings are free; a credit when there is one; a refill once the interval is
// over; else refused (and logged once per socket).
static bool rate_ok(lws_conn *c, const char *msg, size_t n) {
    if (n == 1 && msg[0] == 'p') return true;
    if (c->credits > 0) { c->credits--; return true; }
    int64_t now = cb_now_ms();
    if (c->clear_at_ms < now) { c->credits = c->max_credits; c->clear_at_ms = now + c->interval_ms; return true; }
    if (!c->rl_logged) { c->rl_logged = true; CB_INFO(MOD, "rate limit %d/%ds: %.*s", c->max_credits, c->interval_ms / 1000, (int)(n < 80 ? n : 80), msg); }
    return false;
}

static void on_open(cb_ws *ws, void *ud) {
    lws_conn *c = ud;
    c->ws = ws;
    c->last_ping_s = now_s();
    if (c->kind == K_ROUND && !(c->room = room_get(c->room_id, true, true))) { cb_ws_close(ws, 1011); return; }
    H.n_conns++;
    lnode_push(&H.all, &c->n_all);
    sris_add(c);
    int fi = flag_index(c->flag);
    if (fi >= 0) lnode_push(&H.flags[fi], &c->n_flag);
    switch (c->kind) {
    case K_SITE:
    case K_API:
        if (c->user[0]) users_connect(c, false);
        break;
    case K_LOBBY:
        if (c->user[0]) users_connect(c, true);
        lobby_on_connect(c);
        lnode_push(&H.lobby, &c->n_kind);
        break;
    case K_ROUND:
        if (c->user[0]) users_connect(c, false);
        if (c->is_player) {
            lnode_push(&H.round_players, &c->n_kind);
            anon_played(c);
        }
        lnode_push(&c->room->members, &c->n_room);
        c->room->n_members++;
        crowd_connect(c->room, c);
        history_catch_up(c);
        break;
    }
}

static void on_close(cb_ws *ws, void *ud) {
    lws_conn *c = ud;
    if (lnode_linked(&c->n_all)) H.n_conns--;   // a socket refused at on_open was never counted
    lnode_unlink(&c->n_all);
    lnode_unlink(&c->n_flag);
    lnode_unlink(&c->n_mlat);
    lnode_unlink(&c->n_kind);
    sris_remove(c);
    conn_unwatch_all(c);
    if (c->user[0]) users_disconnect(c);
    if (c->kind == K_LOBBY) lobby_on_disconnect(c);
    if (c->kind == K_ROUND && c->room) {
        lnode_unlink(&c->n_room);
        c->room->n_members--;
        crowd_disconnect(c->room, c);
        room_free_if_empty(c->room);
    }
    lws_conn_free(c);
}

static void on_pong(cb_ws *ws, const uint8_t *data, size_t n, void *ud) {
    lws_conn *c = ud;
    if (n < 8) return;
    uint64_t t = 0;
    for (int i = 0; i < 8; i++) t = t << 8 | data[i];
    int64_t lag = cb_now_ms() - (int64_t)t;
    if (lag < 0) lag = 0;
    if (lag > INT32_MAX) lag = INT32_MAX;
    lag_trusted(c, (int)lag);
}

static chan_kind ext_chan(char kind) { return kind == 't' ? CH_TOUR : kind == 'w' ? CH_SWISS : CH_SIMUL; }

static void forward_sri(lws_conn *c, chan_kind k, const co_msg *m) {
    cb_buf_reset(&H.line);
    li_tell_sri(&H.line, c->sri, c->user[0] ? c->user : NULL, m->raw.p, m->raw.n);
    emit(k, &H.line);
}

static void round_message(lws_conn *c, const co_msg *m) {
    char full_id[16];
    snprintf(full_id, sizeof full_id, "%s%s", c->room->id, c->player_id);
    cb_buf_reset(&H.line);
    switch (m->kind) {
    case CO_ROUND_MOVE:
        if (!c->is_player) return;
        {
            int64_t t = cb_now_ms();
            uint8_t be[8];
            for (int i = 7; i >= 0; i--) { be[i] = (uint8_t)t; t >>= 8; }
            cb_ws_send(c->ws, CB_WS_OP_PING, be, 8);
        }
        if (m->has_ack) ci_msg_int(&H.line, "ack", m->ack); else ci_msg_bare(&H.line, "ack");
        send_buf(c, &H.line);
        {
            int frame = c->user[0] ? lag_session(c->user) : -1;
            cb_buf_reset(&H.line);
            li_r_move(&H.line, full_id, m->uci, m->blur, m->has_lag_ms ? centis_of_ms(m->lag_ms) : -1, m->has_mt ? m->mt_centis : -1,
                      frame >= 0 ? centis_of_ms(frame) : -1);
            emit_line(CH_ROUND, &H.line, true, false);
        }
        return;
    case CO_ROUND_FORWARD:
        if (!c->is_player) return;
        li_r_do(&H.line, full_id, m->raw.p, m->raw.n);
        emit(CH_ROUND, &H.line);
        return;
    case CO_ROUND_FLAG:
        li_r_flag(&H.line, c->room->id, m->color, c->is_player ? c->player_id : NULL);
        emit(CH_ROUND, &H.line);
        return;
    case CO_ROUND_BYE:
        if (!c->is_player) return;
        li_r_bye(&H.line, full_id);
        emit(CH_ROUND, &H.line);
        return;
    case CO_ROUND_BERSERK:
        if (!c->is_player || !c->user[0]) return;
        if (m->has_ack) ci_msg_int(&H.line, "ack", m->ack); else ci_msg_bare(&H.line, "ack");
        send_buf(c, &H.line);
        cb_buf_reset(&H.line);
        li_r_berserk(&H.line, c->room->id, c->user);
        emit(CH_ROUND, &H.line);
        return;
    case CO_ROUND_HOLD:
        if (!c->is_player) return;
        li_r_hold(&H.line, full_id, c->ip, m->mean, m->sd);
        emit(CH_ROUND, &H.line);
        return;
    case CO_ROUND_REPORT:
        if (!c->is_player) return;
        li_r_report(&H.line, full_id, c->ip, c->user[0] ? c->user : NULL, m->text);
        emit(CH_ROUND, &H.line);
        return;
    case CO_CHAT_SAY:
        if (!c->is_player) {
            if (!c->user[0]) return;
            li_chat_say_w(&H.line, c->room->id, c->user, m->text);
            emit(CH_ROUND, &H.line);
        } else if (c->ext_kind) {
            if (!c->user[0]) return;
            li_chat_say(&H.line, c->ext_id, c->user, m->text);
            emit(ext_chan(c->ext_kind), &H.line);
        } else {
            li_chat_say(&H.line, c->room->id, c->user[0] ? c->user : (c->color == 0 ? "w" : "b"), m->text);
            emit(CH_ROUND, &H.line);
        }
        return;
    case CO_CHAT_TIMEOUT:
        if (!c->user[0]) return;
        if (c->is_player && (c->ext_kind == 't' || c->ext_kind == 'm')) {
            li_chat_timeout(&H.line, c->ext_id, c->user, m->user, m->reason, m->text);
            emit(ext_chan(c->ext_kind), &H.line);
        } else {
            li_chat_timeout(&H.line, c->room->id, c->user, m->user, m->reason, m->text);
            emit(CH_ROUND, &H.line);
        }
        return;
    default: return;
    }
}

static void on_message(cb_ws *ws, int opcode, const uint8_t *data, size_t n, void *ud) {
    lws_conn *c = ud;
    if (opcode != CB_WS_OP_TEXT) { cb_ws_close(ws, 1003); return; }
    if (n == 0) return;
    const char *s = (const char *)data;
    if (!rate_ok(c, s, n)) { cb_ws_close(ws, 1008); return; }
    co_msg m;
    co_parse(s, n, &m);
    switch (m.kind) {
    case CO_PING:
        if (c->kind == K_LOBBY) {
            cb_buf_reset(&H.line);
            ci_lobby_pong(&H.line, H.pong_members, H.pong_rounds);
            send_buf(c, &H.line);
        } else cb_ws_send_text(ws, "0", 1);
        if (m.has_lag) lag_client(c, m.lag_ms);
        c->last_ping_s = now_s();
        return;
    case CO_WATCH: watch(c, &m); return;
    case CO_TV_CHANNELS: return;
    case CO_UNEXPECTED:
        CB_INFO(MOD, "unexpected %.*s", (int)(n < 120 ? n : 120), s);
        cb_ws_close(ws, 1011);
        return;
    default: break;
    }
    if (c->kind == K_API) { CB_INFO(MOD, "API socket doesn't support %s", m.t); return; }
    switch (m.kind) {
    case CO_MOVELAT:
        if (!lnode_linked(&c->n_mlat)) lnode_push(&H.mlat, &c->n_mlat);
        return;
    case CO_NOTIFIED:
        if (c->user[0]) {
            smap_put(&H.notified, c->user, (void *)1);
            if (!H.notified_timer) H.notified_timer = cb_loop_timer(H.loop, 1000, notified_flush, NULL);
        }
        return;
    case CO_FOLLOWING: case CO_IGNORE: case CO_STUDY_FORWARD: case CO_CHALLENGE_PING: return;
    case CO_MSGTYPE:
        if (c->user[0]) {
            cb_buf_reset(&H.line);
            ci_msg_str(&H.line, "msgType", c->user);
            users_tell(m.user, &H.line, 0, NULL, true);
        }
        return;
    case CO_USER_FORWARD:
        if (c->user[0]) {
            cb_buf_reset(&H.line);
            li_tell_user(&H.line, c->user, m.raw.p, m.raw.n);
            emit(CH_SITE, &H.line);
        }
        return;
    case CO_LOBBY_JOIN:
        if (c->kind == K_LOBBY && (c->user[0] || anon_can_join(c))) forward_sri(c, CH_LOBBY, &m);
        return;
    case CO_LOBBY_FORWARD:
        if (c->kind == K_LOBBY) forward_sri(c, CH_LOBBY, &m);
        return;
    case CO_LOBBY_IDLE:
        if (c->kind == K_LOBBY) { forward_sri(c, CH_LOBBY, &m); c->idle = m.flag; }
        return;
    default:
        if (c->kind == K_ROUND) round_message(c, &m);
        return;
    }
}

const cb_ws_handlers lws_ws_handlers = { .on_open = on_open, .on_message = on_message, .on_close = on_close, .on_pong = on_pong };

// ---- from lila (LilaHandler) --------------------------------------------------------------------

static void tell_all(lnode *list, size_t off, const cb_buf *b) {
    LNODE_EACH(list, it, nx) send_buf((lws_conn *)((char *)it - off), b);
}

static void tell_users_csv(cb_span users, const cb_buf *b, lws_kind only_kind, const lws_room *only_room, bool any) {
    char u[LWS_USER_MAX];
    size_t it = 0;
    while (lo_commas(users, &it, u, sizeof u)) users_tell(u, b, only_kind, only_room, any);
}

static void close_user(const char *user) {
    lws_user *u = smap_get(&H.users, user);
    if (!u) return;
    LNODE_EACH(&u->conns, it, nx) cb_ws_close(LNODE_OWNER(it, lws_conn, n_user)->ws, 4010);
}

static void set_troll(const char *user, bool v) {
    lws_user *u = smap_get(&H.users, user);
    if (!u) return;
    LNODE_EACH(&u->conns, it, nx) LNODE_OWNER(it, lws_conn, n_user)->troll = v;
}

static void round_version(const lo_msg *m) {
    char id[LWS_ROOM_MAX], type[16];
    int v;
    lo_str(m->arg[0], id, sizeof id);
    if (!lo_int(m->arg[1], &v)) return;
    lo_str(m->arg[3], type, sizeof type);
    lws_room *r = room_get(id, true, true);
    if (!r) return;
    lws_event *e = history_push(r);
    e->v = v;
    cb_strlcpy(e->type, type, sizeof e->type);
    e->f_player = e->f_move_by = -1;
    for (size_t i = 0; i < m->arg[2].n; i++) {
        switch (m->arg[2].p[i]) {
        case 's': e->f_watcher = 1; break;
        case 'p': e->f_owner = 1; break;
        case 'w': e->f_player = 0; break;
        case 'b': e->f_player = 1; break;
        case 'W': e->f_move_by = 0; break;
        case 'B': e->f_move_by = 1; break;
        case 't': e->f_troll = 1; break;
        default: break;
        }
    }
    cb_buf b;
    cb_buf_init(&b);
    ci_versioned(&b, type, v, m->arg[4].p, m->arg[4].n);
    e->full = (char *)cb_buf_take(&b, &e->full_len);
    if (!e->full) { event_free(e); return; }
    H.events++;
    LNODE_EACH(&r->members, it, nx) send_event(LNODE_OWNER(it, lws_conn, n_room), e);
    if (strcmp(type, "move") == 0 || strcmp(type, "drop") == 0) fens_move(r, m->arg[4].p, m->arg[4].n, e->f_move_by);
}

static void room_version(const lo_msg *m, bool chat) {
    char id[LWS_ROOM_MAX];
    int v;
    lo_str(m->arg[0], id, sizeof id);
    if (!lo_int(m->arg[1], &v)) return;
    lws_room *r = room_get(id, true, false);
    if (!r) return;
    lws_event *e = history_push(r);
    e->v = v;
    e->f_player = e->f_move_by = -1;
    e->f_troll = lo_bool(m->arg[2]);
    cb_buf b;
    cb_buf_init(&b);
    ci_room_versioned(&b, v, m->arg[3].p, m->arg[3].n);
    e->full = (char *)cb_buf_take(&b, &e->full_len);
    if (!e->full) { event_free(e); return; }
    H.events++;
    LNODE_EACH(&r->members, it, nx) send_event(LNODE_OWNER(it, lws_conn, n_room), e);
    (void)chat;   // external chat subscribers (tournament players) are not served
}

// On lila's boot: every game's last version, so lila continues the numbering.
static bool versions_each(const char *key, void *val, void *ud) {
    lws_room *r = val;
    if (!r->round || !r->hist_n) return true;
    cb_buf *csv = ud;
    if (csv->len) cb_buf_putc(csv, ',');
    cb_buf_printf(csv, "%s:%d", r->id, history_at(r, 0)->v);
    return true;
}
static void send_versions(void) {
    cb_buf csv;
    cb_buf_init(&csv);
    smap_each(&H.rooms, versions_each, &csv);
    cb_buf_reset(&H.line);
    li_room_versions(&H.line, cb_buf_cstr(&csv));
    emit_line(CH_ROUND, &H.line, false, true);
    cb_buf_free(&csv);
}

static void room_stop(const char *id) {
    lws_room *r = room_get(id, false, false);
    if (!r) return;
    for (int i = 0; i < LWS_HIST; i++) event_free(&r->hist[i]);
    r->hist_n = r->hist_head = 0;
    LNODE_EACH(&r->members, it, nx) cb_ws_close(LNODE_OWNER(it, lws_conn, n_room)->ws, 4010);
    room_free_if_empty(r);
}

static void to_player(const char *full_id, bool same, const cb_buf *b) {
    char id[9];
    if (strlen(full_id) < 9) return;
    memcpy(id, full_id, 8);
    id[8] = 0;
    lws_room *r = room_get(id, false, true);
    if (!r) return;
    LNODE_EACH(&r->members, it, nx) {
        lws_conn *c = LNODE_OWNER(it, lws_conn, n_room);
        if (!c->is_player) continue;
        bool match = strcmp(c->player_id, full_id + 8) == 0;
        if (match == same) send_buf(c, b);
    }
}

static void on_redis(const char *chan, size_t clen, const char *msg, size_t n, void *ud) {
    lo_msg m;
    lo_parse(msg, n, &m);
    if (m.kind == LO_UNKNOWN) { CB_WARN(MOD, "cannot parse on %s: %.*s", chan, (int)(n < 100 ? n : 100), msg); return; }
    if (m.n < m.want) { CB_WARN(MOD, "short line on %s: %.*s", chan, (int)(n < 100 ? n : 100), msg); return; }
    bool round_chan = chan[0] == 'r' && chan[1] == '-';
    bool lobby_chan = strcmp(chan, "lobby-out") == 0;
    char a0[LWS_ROOM_MAX], a1[LWS_USER_MAX];
    cb_buf *b = &H.line;
    cb_buf_reset(b);
    switch (m.kind) {
    case LO_R_VER: round_version(&m); return;
    case LO_MLAT:
        ci_msg_raw(b, "mlat", m.arg[0].p, m.arg[0].n);
        tell_all(&H.mlat, offsetof(lws_conn, n_mlat), b);
        return;
    case LO_TELL_FLAG: {
        lo_str(m.arg[0], a0, sizeof a0);
        int fi = flag_index(a0);
        if (fi < 0) return;
        cb_buf_append(b, m.arg[1].p, m.arg[1].n);
        tell_all(&H.flags[fi], offsetof(lws_conn, n_flag), b);
        return;
    }
    case LO_TELL_SRI:
        lo_str(m.arg[0], a0, sizeof a0);
        cb_buf_append(b, m.arg[1].p, m.arg[1].n);
        sri_tell(a0, b);
        return;
    case LO_TELL_SRIS: {
        cb_buf_append(b, m.arg[1].p, m.arg[1].n);
        size_t it = 0;
        while (lo_commas(m.arg[0], &it, a0, sizeof a0)) sri_tell(a0, b);
        return;
    }
    case LO_TELL_ALL:
        cb_buf_append(b, m.arg[0].p, m.arg[0].n);
        tell_all(&H.all, offsetof(lws_conn, n_all), b);
        return;
    case LO_TELL_USERS:
        cb_buf_append(b, m.arg[1].p, m.arg[1].n);
        tell_users_csv(m.arg[0], b, 0, NULL, true);
        return;
    case LO_DISCONNECT_USER:
        lo_str(m.arg[0], a1, sizeof a1);
        close_user(a1);
        return;
    case LO_TELL_ROOM_USER: {
        lo_str(m.arg[0], a0, sizeof a0);
        lo_str(m.arg[1], a1, sizeof a1);
        lws_room *r = room_get(a0, false, false);
        if (!r) return;
        cb_buf_append(b, m.arg[2].p, m.arg[2].n);
        users_tell(a1, b, K_ROUND, r, false);
        return;
    }
    case LO_TELL_ROOM_USERS: {
        lo_str(m.arg[0], a0, sizeof a0);
        lws_room *r = room_get(a0, false, false);
        if (!r) return;
        cb_buf_append(b, m.arg[2].p, m.arg[2].n);
        tell_users_csv(m.arg[1], b, K_ROUND, r, false);
        return;
    }
    case LO_SET_TROLL:
        lo_str(m.arg[0], a1, sizeof a1);
        set_troll(a1, lo_bool(m.arg[1]));
        return;
    case LO_PONG: {
        long long at;
        if (!lo_i64(m.arg[0], &at)) return;
        if (round_chan) {
            int ms = (int)(cb_now_ms() - H.started_ms - at);
            li_r_latency(b, ms);
            emit(CH_ROUND, b);
        }
        return;
    }
    case LO_LILA_STOP: {
        int req;
        if (!lo_int(m.arg[0], &req)) return;
        CB_INFO(MOD, "******************** LILA STOP ********************");
        li_req_response(b, req, "See you on the other side");
        emit(CH_SITE, b);
        H.online = false;
        cb_buf_reset(b);
        ci_msg_bare(b, "serverRestart");
        tell_all(&H.round_players, offsetof(lws_conn, n_kind), b);
        return;
    }
    case LO_TELL_LOBBY_USERS:
        cb_buf_append(b, m.arg[1].p, m.arg[1].n);
        tell_users_csv(m.arg[0], b, K_LOBBY, NULL, false);
        return;
    case LO_TELL_LOBBY:
        cb_buf_append(b, m.arg[0].p, m.arg[0].n);
        LNODE_EACH(&H.lobby, it, nx) { lws_conn *c = LNODE_OWNER(it, lws_conn, n_kind); anon_tap(c, b); send_buf(c, b); }
        return;
    case LO_TELL_LOBBY_ACTIVE:
        cb_buf_append(b, m.arg[0].p, m.arg[0].n);
        LNODE_EACH(&H.lobby, it, nx) { lws_conn *c = LNODE_OWNER(it, lws_conn, n_kind); if (!c->idle) send_buf(c, b); }
        return;
    case LO_LOBBY_PAIRINGS: {
        size_t it = 0;
        char pair[80];
        while (lo_commas(m.arg[0], &it, pair, sizeof pair)) {
            char *colon = strchr(pair, ':');
            if (!colon) continue;
            *colon = 0;
            cb_buf_reset(b);
            ci_redirect(b, colon + 1);
            lws_sri *s = smap_get(&H.sris, pair);
            if (!s) continue;
            LNODE_EACH(&s->conns, it2, nx2) { lws_conn *c = LNODE_OWNER(it2, lws_conn, n_sri); anon_tap(c, b); send_buf(c, b); }
        }
        return;
    }
    case LO_TELL_ROOM: {
        lo_str(m.arg[0], a0, sizeof a0);
        lws_room *r = room_get(a0, false, false);
        if (!r) return;
        cb_buf_append(b, m.arg[1].p, m.arg[1].n);
        room_publish(r, b);
        return;
    }
    case LO_TELL_ROOM_VERSION: room_version(&m, false); return;
    case LO_TELL_ROOM_CHAT: room_version(&m, true); return;
    case LO_ROOM_STOP:
        lo_str(m.arg[0], a0, sizeof a0);
        room_stop(a0);
        return;
    case LO_R_RESYNC_PLAYER:
        lo_str(m.arg[0], a0, sizeof a0);
        ci_msg_bare(b, "resync");
        to_player(a0, true, b);
        return;
    case LO_R_GONE:
        lo_str(m.arg[0], a0, sizeof a0);
        ci_msg_bool(b, "gone", lo_bool(m.arg[1]));
        to_player(a0, false, b);
        return;
    case LO_R_GONE_IN: {
        int s;
        lo_str(m.arg[0], a0, sizeof a0);
        if (!lo_int(m.arg[1], &s)) return;
        ci_msg_int(b, "goneIn", s);
        to_player(a0, false, b);
        return;
    }
    case LO_R_BOT_ONLINE: {
        lo_str(m.arg[0], a0, sizeof a0);
        lws_room *r = room_get(a0, true, true);
        if (!r) return;
        int color = m.arg[1].n == 1 && m.arg[1].p[0] == 'w' ? 0 : 1;
        if (lo_bool(m.arg[2])) r->players[color]++;
        else if (r->players[color] > 0) r->players[color]--;
        crowd_mark(r);
        return;
    }
    case LO_R_TOUR_STANDING: {
        lo_str(m.arg[0], a0, sizeof a0);
        ci_msg_raw(b, "tourStanding", m.arg[1].p, m.arg[1].n);
        LNODE_EACH(&H.round_players, it, nx) {
            lws_conn *c = LNODE_OWNER(it, lws_conn, n_kind);
            if (c->ext_kind == 't' && strcmp(c->ext_id, a0) == 0) send_buf(c, b);
        }
        return;
    }
    case LO_R_FINISH: {
        lo_str(m.arg[0], a0, sizeof a0);
        lws_room *r = room_get(a0, false, true);
        if (!r) return;
        int winner = lo_absent(m.arg[1]) ? -1 : (m.arg[1].p[0] == 'w' ? 0 : 1);
        fens_finish(r, winner);
        room_free_if_empty(r);
        return;
    }
    case LO_BOOT:
        if (round_chan) {
            CB_INFO(MOD, "#################### LILA BOOT ####################");
            send_versions();
        }
        return;
    case LO_VERSIONING_READY:
        CB_INFO(MOD, "#################### LILA VERSIONING READY ####################");
        H.online = true;
        flush_critical();
        cb_loop_timer(H.loop, 15000, emit_all_online, NULL);
        return;
    case LO_R_START: case LO_TV_SELECT: case LO_REQ_RESPONSE: case LO_API_ONLINE: case LO_STREAMERS_ONLINE:
    case LO_IMPERSONATE: case LO_FOLLOW: case LO_UNFOLLOW: case LO_ANNOUNCE_UPDATE: case LO_RACER_STATE:
    case LO_ROOM_PRESENT: case LO_ROOM_FILTER_PRESENT: case LO_TOUR_GET_WAITING:
        CB_DEBUG(MOD, "not served: %.*s", (int)(n < 60 ? n : 60), msg);
        return;
    default:
        (void)lobby_chan;
        return;
    }
}

// ---- the timers (LilaWsServer, Users, Monitor) --------------------------------------------------

static void broom(cb_loop *l, void *ud) {
    int old = now_s() - 30;
    LNODE_EACH(&H.all, it, nx) {
        lws_conn *c = LNODE_OWNER(it, lws_conn, n_all);
        if (c->last_ping_s < old && c->kind != K_API && strcmp(c->flag, "api") != 0) cb_ws_close(c->ws, 1001);
    }
    cb_loop_timer(l, 7211, broom, NULL);
}

static void counters(cb_loop *l, void *ud) {
    cb_buf_reset(&H.line);
    li_counters(&H.line, H.pong_members, H.pong_rounds);
    emit(CH_LOBBY, &H.line);
    cb_loop_timer(l, 1201, counters, NULL);
}

static void ping_lila(cb_loop *l, void *ud) {
    cb_buf_reset(&H.line);
    li_ping(&H.line, (long long)(cb_now_ms() - H.started_ms));
    emit(CH_SITE, &H.line);
    emit(CH_ROUND, &H.line);
    cb_loop_timer(l, 1879, ping_lila, NULL);
}

static bool count_rounds_each(const char *key, void *val, void *ud) { if (room_crowded(val)) (*(int *)ud)++; return true; }
static void metrics(cb_loop *l, void *ud) {
    int rounds = 0;
    smap_each(&H.rooms, count_rounds_each, &rounds);
    H.pong_members = H.n_conns;
    H.pong_rounds = rounds;
    cb_loop_timer(l, 1949, metrics, NULL);
}

static bool disconnects_each(const char *key, void *val, void *ud) { cb_buf *csv = ud; if (csv->len) cb_buf_putc(csv, ','); cb_buf_puts(csv, key); return true; }
static void user_disconnects(cb_loop *l, void *ud) {
    cb_buf_reset(&H.scratch);
    smap_each(&H.disconnects, disconnects_each, &H.scratch);
    cb_buf_reset(&H.line);
    li_disconnect_users(&H.line, cb_buf_cstr(&H.scratch));
    emit(CH_SITE, &H.line);
    smap_free(&H.disconnects);
    cb_loop_timer(l, 5000, user_disconnects, NULL);
}

static void on_redis_state(bool up, void *ud) {
    CB_INFO(MOD, "redis %s", up ? "connected and subscribed" : "down");
    if (!up) return;
    // WsBoot on every channel lila-ws speaks on (critical: buffered while lila is away)
    cb_buf_reset(&H.line);
    li_boot(&H.line);
    for (int k = 0; k < CH_N; k++) emit_line((chan_kind)k, &H.line, true, false);
}

// ---- open and close -----------------------------------------------------------------------------

int lws_hub_open(cb_ctx *ctx, const char *redis_host, int redis_port, char *err, size_t cap) {
    memset(&H, 0, sizeof H);
    H.ctx = ctx;
    H.loop = ctx->loop;
    H.online = true;
    H.started_ms = cb_now_ms();
    smap_init(&H.rooms); smap_init(&H.users); smap_init(&H.sris);
    smap_init(&H.client_lags); smap_init(&H.trusted); smap_init(&H.pending); smap_init(&H.ip_joins);
    smap_init(&H.disconnects); smap_init(&H.notified);
    lnode_init(&H.all); lnode_init(&H.lobby); lnode_init(&H.round_players); lnode_init(&H.mlat);
    for (int i = 0; i < N_FLAGS; i++) lnode_init(&H.flags[i]);
    cb_buf_init(&H.critical); cb_buf_init(&H.lobby_connects); cb_buf_init(&H.lobby_disconnects);
    cb_buf_init(&H.scratch); cb_buf_init(&H.line);
    H.redis = resp_new(H.loop, redis_host, redis_port, on_redis, on_redis_state, NULL);
    if (!H.redis) { snprintf(err, cap, "lws: no memory for the redis client"); return CB_E_NOMEM; }
    static const char *const OUT[] = { "site-out", "lobby-out", "r-out", "tour-out", "swiss-out", "simul-out", "chal-out" };
    for (size_t i = 0; i < sizeof OUT / sizeof OUT[0]; i++) resp_subscribe(H.redis, OUT[i]);
    for (int i = 0; i <= ROUND_LANES; i++) {   // lila publishes on r-out:0..16 (parallelism 16, inclusive)
        char ch[16];
        snprintf(ch, sizeof ch, "r-out:%d", i);
        resp_subscribe(H.redis, ch);
    }
    resp_start(H.redis);
    cb_loop_timer(H.loop, 30000, broom, NULL);
    cb_loop_timer(H.loop, 4000, counters, NULL);
    cb_loop_timer(H.loop, 1000, ping_lila, NULL);
    cb_loop_timer(H.loop, 5000, metrics, NULL);
    cb_loop_timer(H.loop, 7000, user_disconnects, NULL);
    return CB_OK;
}

static bool free_room_each(const char *key, void *val, void *ud) {
    lws_room *r = val;
    smap_free(&r->users);
    free(r->fen_watchers);
    for (int i = 0; i < LWS_HIST; i++) event_free(&r->hist[i]);
    free(r);
    return true;
}
static bool free_val_each(const char *key, void *val, void *ud) { free(val); return true; }

void lws_hub_close(void) {
    resp_free(H.redis);
    H.redis = NULL;
    smap_each(&H.rooms, free_room_each, NULL);
    smap_free(&H.rooms);
    smap_each(&H.users, free_val_each, NULL);
    smap_free(&H.users);
    smap_each(&H.sris, free_val_each, NULL);
    smap_free(&H.sris);
    smap_free(&H.client_lags);
    smap_each(&H.trusted, free_val_each, NULL);
    smap_free(&H.trusted);
    smap_each(&H.pending, free_val_each, NULL);
    smap_free(&H.pending);
    smap_each(&H.ip_joins, free_val_each, NULL);
    smap_free(&H.ip_joins);
    smap_free(&H.disconnects);
    smap_free(&H.notified);
    free(H.dirty);
    cb_buf_free(&H.critical); cb_buf_free(&H.lobby_connects); cb_buf_free(&H.lobby_disconnects);
    cb_buf_free(&H.scratch); cb_buf_free(&H.line);
}

static bool count_crowded_each(const char *key, void *val, void *ud) { if (room_crowded(val)) (*(long *)ud)++; return true; }
void lws_hub_stats(lws_stats *out) {
    memset(out, 0, sizeof *out);
    out->conns = H.n_conns;
    out->users = (long)smap_size(&H.users);
    out->rooms = (long)smap_size(&H.rooms);
    smap_each(&H.rooms, count_crowded_each, &out->rounds_crowded);
    out->events = H.events;
    out->dropped_offline = H.dropped;
    out->lila_online = H.online;
    out->redis_up = H.redis && resp_up(H.redis);
    if (H.redis) resp_stats(H.redis, &out->redis_published, &out->redis_received);
}

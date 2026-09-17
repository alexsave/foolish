// table.c - a game plus its roster. See table.h.
#include "table.h"
#include "awire.h"
#include "view.h"
#include "bot_roster.h"
#include "bot_drive.h"
#include "anim_plan.h"
#include "../wasm/wire.h"
#include <string.h>

// ---------- the action request and response -------------------------------------

int table_request_decode(const uint8_t *p, int len, TableRequest *out) {
    memset(out, 0, sizeof(*out));
    if (!p || len < 2) return TABLE_E_WIRE;
    const int fmt = p[0], gid_len = p[1];
    int at = 2 + gid_len;
    if (fmt == TABLE_REQ_FORMAT_V2) {
        if (len < at + 4) return TABLE_E_WIRE;
        out->has_intent = true;
        out->intent = (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8)
                    | ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
        at += 4;
    } else if (fmt != TABLE_REQ_FORMAT_V1) {
        return TABLE_E_WIRE;
    }
    if (len < at + 2) return TABLE_E_WIRE;   // the shortest wire is two bytes
    out->fmt = (int8_t)fmt;
    out->gid_len = (uint8_t)gid_len;
    out->gid_off = 2;
    out->wire_off = at;
    out->wire_len = len - at;
    return 0;
}

static void put_u32(uint8_t *q, uint32_t v) {
    q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16); q[3] = (uint8_t)(v >> 24);
}

int table_action_response(int result, int reject, uint32_t version, uint8_t *out, int cap) {
    if (!out || cap < TABLE_RESP_BYTES) return TABLE_E_CAP;
    int status = TABLE_STATUS_APPLIED, code = 0;
    if (result == TABLE_REJECTED) { status = TABLE_STATUS_REJECTED; code = reject; }
    else if (result == TABLE_STALE_ROUND) { status = TABLE_STATUS_REJECTED; code = TABLE_REJECT_STALE_ROUND; }
    else if (result == TABLE_MOOT) status = TABLE_STATUS_MOOT;
    out[0] = TABLE_RESP_FORMAT;
    out[1] = (uint8_t)status;
    out[2] = (uint8_t)code;
    put_u32(out + 3, version);
    return TABLE_RESP_BYTES;
}

// ---------- snapshots -----------------------------------------------------------

// The table an operation is capturing into. Thread-local for the same reason
// engine_snap_hook is (game.h): two games' operations on two threads.
static _Thread_local TableSnaps *t_capture;

static void table_snap(const Game *g, int tag, int aux) {
    TableSnaps *s = t_capture;
    if (!s || s->n >= MAX_SNAPS) return;
    memcpy(s->slot[s->n].bytes, g, TABLE_SNAP_BYTES);
    s->tag[s->n] = tag;
    s->aux[s->n] = aux;
    s->n++;
}

// Opens an operation: forgets the last one's products and records what the
// DRAW-privacy rule and the event walk need from before it.
static void scope_open(Table *t, int actor) {
    t->snaps->n = 0;
    t->ended = t->dealt_now = t->roster_changed = t->lobby_event = false;
    t->pre_has_flip = t->g->has_flipped;
    t->pre_flip = t->g->flipped;
    t->actor = (int8_t)actor;
    t->log_start = t->g->num_logs;
    t->reject = 0;
}

// ---------- load ----------------------------------------------------------------

void table_init(Table *t, Game *g, TableSnaps *snaps) {
    memset(t, 0, sizeof(*t));
    t->g = g;
    t->snaps = snaps;
    t->actor = -1;
    if (snaps) snaps->n = 0;
}

int table_load(Table *t, const uint8_t *state, int state_len, const uint8_t *roster, int roster_len) {
    Roster r;
    int8_t kinds[MAX_PLAYERS];
    t->loaded = false;
    t->detail = 0;
    if (!state || state_len < 4 || state[0] != TABLE_STATE_FORMAT) return TABLE_E_STATE_VERSION;
    const int rc = roster_decode(&r, roster, roster_len);
    if (rc != ROSTER_OK) { t->detail = rc; return TABLE_E_ROSTER; }
    // The seat count is the state's second byte; checked before the import so a
    // refusal adopts nothing.
    if (state[2 + 1] != (uint8_t)r.n) return TABLE_E_MISMATCH;
    for (int s = 0; s < r.n; s++) {
        kinds[s] = STRATEGY_KEY_HUMAN;
        if (r.seats[s].brain_len == 0) continue;
        const int idx = bot_roster_find(r.seats[s].brain);
        if (idx < 0 || !bot_roster_linked(idx)) return TABLE_E_UNKNOWN_BRAIN;
        kinds[s] = (int8_t)bot_roster_at(idx)->strat;
    }
    const int v = state_import(t->g, state + 2, 0);
    if (v != GAME_VALID) return v;
    t->g->deterministic_deck = state[1] != 0;
    t->g->rules = 0;   // online play is the classic game (Q18); a previous FMSG decode may have left a variant
    for (int s = 0; s < r.n; s++) t->g->players[s].strategy_key = kinds[s];
    t->r = r;
    scope_open(t, -1);
    t->loaded = true;
    return TABLE_OK;
}

// ---------- act -----------------------------------------------------------------

// The kernel's end of a game: GAME_OVER, bots parked READY and humans IDLE.
// Returns the fool's seat, or -1 while the game runs (state untouched).
static int finalize(Table *t) {
    Game *g = t->g;
    const int fool = game_done(g);
    if (fool < 0) return -1;
    const uint32_t bots = roster_bot_mask(&t->r);
    g->status = GAME_STATUS_GAME_OVER;
    for (int i = 0; i < g->num_players; i++)
        g->players[i].status = ((bots >> i) & 1u) ? PLAYER_STATUS_READY : PLAYER_STATUS_IDLE;
    return fool;
}

int table_act(Table *t, const char *actor_id, int id_len, const uint8_t *awire, int wire_len,
              int64_t intent_version, int64_t round_epoch) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    Game *g = t->g;
    // A move that lost the race to the game's end is a no-op, and that outranks
    // the round guard: the client resolves it as a success, not a toast.
    if (g->status == GAME_STATUS_GAME_OVER) return TABLE_MOOT;
    if (intent_version >= 0 && intent_version < round_epoch) return TABLE_STALE_ROUND;
    const int seat = roster_seat_of(&t->r, actor_id, id_len);
    if (seat < 0) return TABLE_E_NOT_SEATED;
    AwireAction a;
    if (!awire || wire_len < 0 || !awire_decode(awire, wire_len, &a)) return TABLE_E_WIRE;

    scope_open(t, seat);
    // Mid-game draws are seeded from the board, never from a clock, so the game
    // replays from its deal seed (wasm_api.c wasm_seed_rng_deterministic).
    game_rng_set(game_state_seed(g, t->rng_base, 0u));
    void (*const prev)(const Game *, int, int) = engine_snap_hook;
    t_capture = t->snaps;
    engine_snap_hook = table_snap;
    const bool ok = awire_apply(g, seat, &a);
    engine_snap_hook = prev;
    t_capture = 0;
    if (!ok) {
        t->reject = engine_last_reject;
        return TABLE_REJECTED;
    }
    t->ended = finalize(t) >= 0;
    return TABLE_APPLIED;
}

bool table_needs_bots(const Table *t) {
    if (!t->loaded || t->g->status != GAME_STATUS_PLAYING) return false;
    const uint32_t bots = roster_bot_mask(&t->r);
    for (int s = 0; s < t->g->num_players; s++)
        if (((bots >> s) & 1u) && t->g->players[s].status == PLAYER_STATUS_IN) return true;
    return false;
}

bool table_bots_need_logs(const Table *t) {
    if (!t->loaded || t->g->status != GAME_STATUS_PLAYING) return false;
    const uint32_t bots = roster_bot_mask(&t->r);
    const uint32_t eligible = bot_drive_eligible_mask(t->g, ~bots);
    for (int s = 0; s < t->g->num_players; s++) {
        if (!((eligible >> s) & 1u)) continue;
        const BotRosterEntry *e = bot_roster_at(bot_roster_find(t->r.seats[s].brain));
        if (e && e->uses_logs) return true;
    }
    return false;
}

// ---------- products --------------------------------------------------------------

static int unnamed_seat(const Game *g);

// The state blob: [format][deterministic deck][state_put unmasked].
static int put_state_blob(const Game *g, uint8_t *out) {
    out[0] = TABLE_STATE_FORMAT;
    out[1] = g->deterministic_deck ? 1 : 0;
    return 2 + state_put(g, VIEW_UNMASKED, out + 2);
}

// A bound on one state_put, for reserving space before writing it.
#define TABLE_STATE_MAX (2 + 24 + MAX_DECK + 2 * MAX_BATTLES + MAX_PLAYERS * (3 + MAX_HAND_SIZE) + 1 + MAX_PLAYERS)
// A bound on one session-log record with its timestamp.
#define TABLE_LOG_MAX   (6 + 4 + 2 * MAX_LOG_PAIRS)

static uint8_t game_status_byte(const Game *g) { return (uint8_t)g->status; }

int table_envelope(const Table *t, const char *game_id, int gid_len, int viewer, uint32_t version,
                   uint8_t *out, int cap) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const Game *g = t->g;
    if (viewer >= g->num_players) viewer = -1;
    if (!out || cap < 11 + 2 + TABLE_STATE_MAX) return TABLE_E_CAP;
    out[0] = 1;                                                     // GAME_RESP_FORMAT
    out[1] = (uint8_t)((viewer >= 0 ? 0x01 : 0) | 0x02);            // seated | packed roster trailer
    out[2] = viewer >= 0 ? (uint8_t)viewer : 0xFF;
    put_u32(out + 3, version);
    out[7] = 0; out[8] = 0;                                         // the retired JSON roster island
    const int view_at = 11;
    out[view_at] = VIEW_FORMAT_VERSION;
    out[view_at + 1] = viewer >= 0 ? (uint8_t)viewer : 0xFF;
    const int view_len = 2 + state_put(g, viewer >= 0 ? viewer : VIEW_SPECTATOR, out + view_at + 2);
    out[9] = (uint8_t)view_len; out[10] = (uint8_t)(view_len >> 8);
    const int at = view_at + view_len;
    const int n = roster_trailer_write(&t->r, game_id, gid_len, game_status_byte(g), g->good_players_mask,
                                       out + at, cap - at);
    if (n < 0) return n == ROSTER_E_CAP ? TABLE_E_CAP : TABLE_E_ROSTER;
    return at + n;
}

// The hook snapshots as the event walk takes them.
static int snap_refs(const Table *t, EvSnap *refs) {
    const TableSnaps *s = t->snaps;
    for (int i = 0; i < s->n; i++) {
        refs[i].g = (const Game *)(const void *)s->slot[i].bytes;
        refs[i].tag = s->tag[i];
        refs[i].aux = s->aux[i];
    }
    return s->n;
}

static void count_event(void *ctx, const EvwEvent *ev) { (void)ev; (*(int *)ctx)++; }

static int event_count(const Table *t) {
    EvSnap refs[MAX_SNAPS];
    const int n = snap_refs(t, refs);
    int count = 0;
    evwire_walk(refs, n, t->g->logs + t->log_start, t->g->num_logs - t->log_start, VIEW_SPECTATOR,
                count_event, &count);
    return count + ((t->ended || t->lobby_event) ? 1 : 0);
}

int table_push(const Table *t, const char *game_id, int gid_len, int viewer, uint8_t *out, int cap) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const Game *g = t->g;
    if (viewer >= g->num_players || (viewer >= 0 && t->r.seats[viewer].brain_len > 0)) return TABLE_E_NOT_SEATED;
    EvSnap refs[MAX_SNAPS];
    const int n_snaps = snap_refs(t, refs);
    const int seq = evwire_serialize(refs, n_snaps, g->logs + t->log_start, g->num_logs - t->log_start, g,
                                     viewer >= 0 ? viewer : VIEW_SPECTATOR, t->actor,
                                     (t->ended || t->lobby_event) ? 1 : 0, out, cap);
    if (seq < 0 || seq + 1 > cap) return TABLE_E_CAP;
    out[seq] = t->roster_changed ? EVW_AS3_ROSTER : 0;
    if (!t->roster_changed) return seq + 1;
    const int tr = roster_trailer_write(&t->r, game_id, gid_len, game_status_byte(g), g->good_players_mask,
                                        out + seq + 1, cap - seq - 1);
    if (tr < 0) return tr == ROSTER_E_CAP ? TABLE_E_CAP : TABLE_E_ROSTER;
    return seq + 1 + tr;
}

int table_commit_products(const Table *t, const char *game_id, int gid_len, uint32_t next_version,
                          int64_t now_ms, TableCommit *out, uint8_t *arena, int cap) {
    memset(out, 0, sizeof(*out));
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const Game *g = t->g;
    int at = 0;

    out->status = (int8_t)g->status;
    out->fool = (int8_t)(g->status == GAME_STATUS_GAME_OVER ? unnamed_seat(g) : -1);
    out->num_players = g->num_players;
    out->needs_bots = table_needs_bots(t);
    out->ended = t->ended;
    out->dealt_now = t->dealt_now;
    out->roster_changed = t->roster_changed;
    const int n_events = event_count(t);
    out->n_events = (uint8_t)(n_events > 255 ? 255 : n_events);

    if (cap - at < TABLE_STATE_MAX) return TABLE_E_CAP;
    out->state.off = at;
    out->state.len = put_state_blob(g, arena + at);
    at += out->state.len;

    const int rl = roster_encode(&t->r, arena + at, cap - at);
    if (rl < 0) return rl == ROSTER_E_CAP ? TABLE_E_CAP : TABLE_E_ROSTER;
    out->roster.off = at;
    out->roster.len = rl;
    at += rl;

    // This operation's records, each stamped with the one clock value, DRAW
    // identities masked (view.c log_record_put).
    out->logs.off = at;
    for (int i = t->log_start; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        if (cap - at < TABLE_LOG_MAX) return TABLE_E_CAP;
        int64_t ms = now_ms;
        for (int b = 0; b < 6; b++) { arena[at + b] = (uint8_t)(ms & 0xff); ms >>= 8; }
        at += 6;
        at += log_record_put(l, 1, t->pre_has_flip, t->pre_flip, g->has_flipped, arena + at);
        if (l->log_type == LOG_GAME_START) out->logs_reset = true;
        if (l->log_type == LOG_PICKUP || l->log_type == LOG_DISCARD) out->closed_round = true;
    }
    out->logs.len = at - out->logs.off;
    if (out->logs.len == 0) out->logs.off = 0;
    if (out->logs_reset) out->closed_round = false;

    for (int s = 0; s < g->num_players; s++) {
        if (t->r.seats[s].brain_len > 0) continue;
        const int n = table_envelope(t, game_id, gid_len, s, next_version, arena + at, cap - at);
        if (n < 0) return n;
        out->views[s].off = at;
        out->views[s].len = n;
        at += n;
    }
    const int n = table_envelope(t, game_id, gid_len, -1, next_version, arena + at, cap - at);
    if (n < 0) return n;
    out->spectator.off = at;
    out->spectator.len = n;
    at += n;
    return at;
}

// ---------- the end of a game -----------------------------------------------------

// The one seat the elimination order does not name. A finished game's seats
// are parked READY/IDLE by the finalize, so game_done (which reads IN/OUT) can
// no longer say; the elimination order still can.
static int unnamed_seat(const Game *g) {
    uint32_t out_mask = 0;
    for (int i = 0; i < g->num_eliminated; i++) out_mask |= 1u << g->elimination_order[i];
    for (int s = 0; s < g->num_players; s++)
        if (!((out_mask >> s) & 1u)) return s;
    return -1;
}

int table_rankings(const Table *t, int8_t seats_out[MAX_PLAYERS]) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const Game *g = t->g;
    unsigned char elim[MAX_PLAYERS];
    for (int i = 0; i < g->num_eliminated; i++) elim[i] = (unsigned char)g->elimination_order[i];
    const int fool = unnamed_seat(g);
    AnimFinishRow rows[MAX_PLAYERS + 1];
    const int n = anim_finish_rows(elim, g->num_eliminated, fool, g->num_players, -1, rows, MAX_PLAYERS + 1);
    if (n < 0) return n;
    for (int i = 0; i < n && i < MAX_PLAYERS; i++) seats_out[i] = (int8_t)rows[i].seat;
    return n;
}

// The rating differences (opponent minus player) at which a WIN is worth one
// more point: Math.round(10 * (1 - 1 / (1 + 10^(d / 400)))) steps up by one at
// each of these, from 0 below the first to 10 at and past the last. Computed from
// that expression in double precision over every integer d in [-20000, 20000]
// (translation-invariant, monotone); a LOSS is the same step minus 10.
static const int16_t ELO_STEPS[10] = { -511, -301, -190, -107, -34, 35, 108, 191, 302, 512 };

static int elo_win_change(int32_t d) {
    int c = 0;
    for (int i = 0; i < 10; i++) c += d >= ELO_STEPS[i];
    return c;
}

int elo_deltas(const int32_t *ratings, const int8_t *order, int n, int32_t *out) {
    if (!ratings || !order || !out || n < 0 || n > MAX_PLAYERS) return TABLE_E_WIRE;
    uint32_t seen = 0;
    for (int i = 0; i < n; i++) {
        if (order[i] < 0 || order[i] >= n || ((seen >> order[i]) & 1u)) return TABLE_E_WIRE;
        seen |= 1u << order[i];
    }
    for (int i = 0; i < n; i++) {
        const int me = order[i];
        int32_t total = 0;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            const int c = elo_win_change(ratings[order[j]] - ratings[me]);
            total += i < j ? c : c - 10;
        }
        out[me] = total;
    }
    return n;
}

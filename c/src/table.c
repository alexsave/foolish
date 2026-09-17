// table.c - a game plus its roster. See table.h.
#include "table.h"
#include "awire.h"
#include "view.h"
#include "bot_roster.h"
#include "bot_drive.h"
#include "anim_plan.h"
#include "replay.h"
#include "replay_extras.h"
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

// ---------- fixtures --------------------------------------------------------------

int table_seal(Table *t, const Game *g, const Roster *r, uint8_t *out, int cap) {
    t->loaded = false;
    t->detail = 0;
    if (cap < TABLE_STATE_MAX + ROSTER_BYTES) return TABLE_E_CAP;
    // state_put walks the counts of a board nothing has judged yet: refuse one
    // past its array here, with the code the import would give it.
    if (g->num_players < 0 || g->num_players > MAX_PLAYERS) return GAME_INVALID_NUM_PLAYERS;
    if (g->deck_count < 0 || g->deck_count > MAX_DECK || g->num_battles < 0 || g->num_battles > MAX_BATTLES)
        return GAME_INVALID_COUNT;
    if (g->num_eliminated < 0 || g->num_eliminated > MAX_PLAYERS) return GAME_INVALID_ELIMINATION;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].hand_count < 0 || g->players[i].hand_count > MAX_HAND_SIZE) return GAME_INVALID_COUNT;
    const int state_len = put_state_blob(g, out);
    const int rc = roster_encode(r, out + state_len, cap - state_len);
    if (rc < 0) { t->detail = rc; return TABLE_E_ROSTER; }
    const int loaded = table_load(t, out, state_len, out + state_len, rc);
    return loaded == TABLE_OK ? state_len : loaded;
}
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

// ---------- lobby edits (Phase 3.ii) ----------------------------------------------

// The seat of a seated actor, after the one check every lobby edit but a join
// makes (Q9): the actor must be at the table.
static int actor_seat(const Table *t, const char *actor_id, int id_len) {
    return roster_seat_of(&t->r, actor_id, id_len);
}

static int roster_refusal(Table *t, int rc) {
    t->detail = rc;
    return TABLE_E_ROSTER;
}

// A lobby edit the pushes announce: one MAGIC_TRANSITION on the committed board.
static void lobby_edit(Table *t, int roster_changed) {
    scope_open(t, -1);
    t->lobby_event = true;
    t->roster_changed = roster_changed != 0;
}

// Deals the lobby from `seed` (FOOLISH_SEED_LEN bytes), capturing the deal's
// hook snapshots. The deal RNG is put back afterwards: its wide mode is read by
// every draw on this thread, so a deal must not leave it on for the next table.
static void deal(Table *t, const uint8_t *seed) {
    unsigned char saved[GAME_DEAL_RNG_STATE_MAX];
    game_deal_rng_get(saved);
    game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);
    void (*const prev)(const Game *, int, int) = engine_snap_hook;
    t_capture = t->snaps;
    engine_snap_hook = table_snap;
    start_game(t->g);
    engine_snap_hook = prev;
    t_capture = 0;
    game_deal_rng_set(saved);
    t->dealt_now = true;
    t->lobby_event = false;   // the deal's own events announce it
}

static const char TITLE_SUFFIX[] = "'s Game";
#define TITLE_SUFFIX_LEN ((int)sizeof(TITLE_SUFFIX) - 1)

// A new table's title: the creator's name, whole scalars only, then "'s Game".
// Returns its length (at most ROSTER_TITLE_MAX).
static int default_title(const char *name, int name_len, char *title) {
    int keep = name_len < ROSTER_TITLE_MAX - TITLE_SUFFIX_LEN ? name_len : ROSTER_TITLE_MAX - TITLE_SUFFIX_LEN;
    while (keep > 0 && keep < name_len && ((uint8_t)name[keep] & 0xc0) == 0x80) keep--;
    memcpy(title, name, (size_t)keep);
    memcpy(title + keep, TITLE_SUFFIX, (size_t)TITLE_SUFFIX_LEN);
    return keep + TITLE_SUFFIX_LEN;
}

// Is the title the default one a seat's player was given at create? The title
// may hold more of the name than the seat does (a seat keeps ROSTER_NAME_MAX
// bytes, a title up to its own cap), so the test is that the seat's name is the
// title's name trimmed the way a seat trims one.
static bool bytes_same(const char *a, const char *b, int n) {   // no memcmp in the wasm builds
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}
static bool title_is_default_for(const Roster *r, int seat) {
    const int p = r->title_len - TITLE_SUFFIX_LEN;
    const RosterSeat *s = &r->seats[seat];
    if (p < 0 || !bytes_same(r->title + p, TITLE_SUFFIX, TITLE_SUFFIX_LEN)) return false;
    return roster_name_trim(r->title, p) == s->name_len && bytes_same(r->title, s->name, s->name_len);
}

int table_create(Table *t, const char *actor_id, int id_len, const char *name, int name_len) {
    Roster r;
    char title[ROSTER_TITLE_MAX];
    memset(&r, 0, sizeof(r));
    int rc = roster_seat_add(&r, actor_id, id_len, name, name_len, "", 0);
    if (rc < 0) return roster_refusal(t, rc);
    if ((rc = roster_set_title(&r, title, default_title(name, name_len, title))) != ROSTER_OK) return roster_refusal(t, rc);

    Game *g = t->g;
    memset(g, 0, offsetof(Game, logs));
    g->num_logs = 0;
    game_lobby_seat(g, STRATEGY_KEY_HUMAN);
    t->r = r;
    t->loaded = true;
    lobby_edit(t, 1);
    t->lobby_event = false;   // nobody is watching a table that did not exist
    return TABLE_OK;
}

int table_join(Table *t, const char *actor_id, int id_len, const char *name, int name_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_E_NOT_WAITING;
    const int rc = roster_seat_add(&t->r, actor_id, id_len, name, name_len, "", 0);
    if (rc < 0) return roster_refusal(t, rc);
    game_lobby_seat(t->g, STRATEGY_KEY_HUMAN);
    lobby_edit(t, 1);
    return TABLE_OK;
}

// Removes a seat from both halves of the table.
static int unseat(Table *t, int seat) {
    roster_seat_remove(&t->r, seat);
    game_lobby_unseat(t->g, seat);
    if (t->r.n == 0) return TABLE_EMPTY;
    lobby_edit(t, 1);
    return TABLE_OK;
}

int table_leave(Table *t, const char *actor_id, int id_len, const char *target_id, int target_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (actor_seat(t, actor_id, id_len) < 0) return TABLE_E_NOT_SEATED;
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_E_NOT_WAITING;
    const int seat = roster_seat_of(&t->r, target_id, target_len);
    if (seat < 0) return TABLE_E_NOT_SEATED;
    // A bot is removed as a bot (table_remove_bot), never as a player leaving.
    if (t->r.seats[seat].brain_len > 0) return TABLE_E_FORBIDDEN;
    return unseat(t, seat);
}

int table_add_bot(Table *t, const char *actor_id, int id_len, const char *bot_id, int bot_len,
                  const char *nick, int nick_len, const char *brain, int brain_len, const uint8_t *deal_seed) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (actor_seat(t, actor_id, id_len) < 0) return TABLE_E_NOT_SEATED;
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_E_NOT_WAITING;
    char key[ROSTER_BRAIN_MAX + 1];
    if (brain_len <= 0 || brain_len > ROSTER_BRAIN_MAX) return TABLE_E_UNKNOWN_BRAIN;
    memcpy(key, brain, (size_t)brain_len);
    key[brain_len] = 0;
    const int idx = bot_roster_find(key);
    if (idx < 0 || !bot_roster_linked(idx)) return TABLE_E_UNKNOWN_BRAIN;
    const int rc = roster_seat_add(&t->r, bot_id, bot_len, nick, nick_len, brain, brain_len);
    if (rc < 0) return roster_refusal(t, rc);
    game_lobby_seat(t->g, bot_roster_at(idx)->strat);
    lobby_edit(t, 1);
    if (game_lobby_can_deal(t->g)) deal(t, deal_seed);
    return TABLE_OK;
}

int table_remove_bot(Table *t, const char *actor_id, int id_len, const char *bot_id, int bot_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (actor_seat(t, actor_id, id_len) < 0) return TABLE_E_NOT_SEATED;
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_E_NOT_WAITING;
    const int seat = roster_seat_of(&t->r, bot_id, bot_len);
    if (seat < 0 || t->r.seats[seat].brain_len == 0) return TABLE_E_NOT_SEATED;
    return unseat(t, seat);
}

int table_ready(Table *t, const char *actor_id, int id_len, const uint8_t *deal_seed) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const int seat = actor_seat(t, actor_id, id_len);
    if (seat < 0) return TABLE_E_NOT_SEATED;
    // Readying a game that is already dealt changes nothing: a late second tap.
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_MOOT;
    game_lobby_ready(t->g, seat);
    lobby_edit(t, 0);
    if (game_lobby_can_deal(t->g)) deal(t, deal_seed);
    return TABLE_OK;
}

int table_reseat(Table *t, const char *actor_id, int id_len, const uint8_t *ids, int ids_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (actor_seat(t, actor_id, id_len) < 0) return TABLE_E_NOT_SEATED;
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_E_NOT_WAITING;
    int8_t perm[MAX_PLAYERS];
    int n = 0, at = 0;
    while (at < ids_len) {
        const int len = ids[at];
        if (n >= MAX_PLAYERS || at + 1 + len > ids_len) return roster_refusal(t, ROSTER_E_PERM);
        const int seat = roster_seat_of(&t->r, (const char *)ids + at + 1, len);
        if (seat < 0) return roster_refusal(t, ROSTER_E_PERM);
        perm[n++] = (int8_t)seat;
        at += 1 + len;
    }
    const int rc = roster_reorder(&t->r, perm, n);
    if (rc != ROSTER_OK) return roster_refusal(t, rc);
    game_lobby_reorder(t->g, perm, n);
    lobby_edit(t, 1);
    return TABLE_OK;
}

// JavaScript's String.prototype.trim whitespace, as UTF-8 (ECMA-262 WhiteSpace
// and LineTerminator): the length of the whitespace scalar at p, or 0.
static int js_space(const uint8_t *p, int n) {
    if (n >= 1 && ((p[0] >= 0x09 && p[0] <= 0x0d) || p[0] == 0x20)) return 1;
    if (n >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;                               // U+00A0
    if (n >= 3 && p[0] == 0xe1 && p[1] == 0x9a && p[2] == 0x80) return 3;               // U+1680
    if (n >= 3 && p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8a)       // U+2000-200A
        || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf)) return 3;                      // U+2028, 2029, 202F
    if (n >= 3 && p[0] == 0xe2 && p[1] == 0x81 && p[2] == 0x9f) return 3;               // U+205F
    if (n >= 3 && p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;               // U+3000
    if (n >= 3 && p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) return 3;               // U+FEFF
    return 0;
}

// The title rule the lobby has always kept: at most 50 characters as the web
// counts them (UTF-16 code units, before trimming), and not empty once trimmed.
int table_retitle(Table *t, const char *actor_id, int id_len, const char *title, int title_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (actor_seat(t, actor_id, id_len) < 0) return TABLE_E_NOT_SEATED;
    if (t->g->status != GAME_STATUS_WAITING) return TABLE_E_NOT_WAITING;
    const uint8_t *p = (const uint8_t *)title;
    if (title_len < 0 || (title_len > 0 && !p)) return roster_refusal(t, ROSTER_E_TITLE);
    int units = 0;
    for (int i = 0; i < title_len; i++) {
        if ((p[i] & 0xc0) != 0x80) units += p[i] >= 0xf0 ? 2 : 1;
    }
    if (units > 50) return roster_refusal(t, ROSTER_E_TITLE);
    int lo = 0, hi = title_len, k;
    while (lo < hi && (k = js_space(p + lo, hi - lo)) > 0) lo += k;
    for (int moved = 1; moved && hi > lo; ) {
        moved = 0;
        for (int w = 3; w >= 1; w--) {
            if (hi - w >= lo && js_space(p + hi - w, w) == w) { hi -= w; moved = 1; break; }
        }
    }
    if (hi == lo) return roster_refusal(t, ROSTER_E_TITLE);
    const int rc = roster_set_title(&t->r, title + lo, hi - lo);
    if (rc != ROSTER_OK) return roster_refusal(t, rc);
    lobby_edit(t, 1);
    return TABLE_OK;
}

int table_continue(Table *t, const char *actor_id, int id_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (actor_seat(t, actor_id, id_len) < 0) return TABLE_E_NOT_SEATED;
    if (t->g->status != GAME_STATUS_GAME_OVER) return TABLE_E_NOT_OVER;
    game_reset_to_lobby(t->g, roster_bot_mask(&t->r));
    // A lobby has no deck to draw from, so its blob says so: the flag byte of every
    // lobby blob is 0, as the expand migration writes it (plan 3.4), and the next
    // deal sets it again.
    t->g->deterministic_deck = false;
    lobby_edit(t, 0);
    return TABLE_OK;
}

int table_rearrange_hand(Table *t, const char *actor_id, int id_len, const uint8_t *idx, int n) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const int seat = actor_seat(t, actor_id, id_len);
    if (seat < 0) return TABLE_E_NOT_SEATED;
    scope_open(t, seat);
    if (!game_rearrange_hand(t->g, seat, idx, n)) return TABLE_E_WIRE;
    return TABLE_OK;
}

int table_redact(Table *t, const char *user_id, int id_len, const char *name, int name_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    const int seat = roster_seat_of(&t->r, user_id, id_len);
    // The default title carries the player's name into every envelope, so it is
    // renamed with them; a title the players chose is theirs to keep.
    const bool retitle = seat >= 0 && title_is_default_for(&t->r, seat);
    Roster r = t->r;
    const int rc = roster_redact(&r, user_id, id_len, name, name_len);
    if (rc == ROSTER_E_SEAT) return TABLE_E_NOT_SEATED;
    if (rc < 0) return roster_refusal(t, rc);
    if (retitle) {
        char title[ROSTER_TITLE_MAX];
        const int tc = roster_set_title(&r, title, default_title(name, name_len, title));
        if (tc != ROSTER_OK) return roster_refusal(t, tc);
    }
    t->r = r;
    scope_open(t, -1);
    t->roster_changed = true;
    return TABLE_OK;
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

// ---------- the bot cycle and the end of a game (Phase 4b) --------------------------

int table_seat_of(const Table *t, const char *id, int id_len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    return roster_seat_of(&t->r, id, id_len);
}

int table_set_deal_seed(Table *t, const char *seed_hex, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)seed_hex[i]) * 16777619u;
    t->rng_base = len > 0 ? h : 0u;
    return TABLE_OK;
}

// One session-log record at `at`: [u48 ms][type][seat][defender][n_pairs][pairs].
// Returns its length, 0 at a truncated tail, TABLE_E_WIRE for an unknown type.
static int log_record_at(const uint8_t *log, int len, int at) {
    if (at + 10 > len) return 0;
    if (log[at + 6] > LOG_DRAW) return TABLE_E_WIRE;
    const int n = 10 + 2 * log[at + 9];
    return at + n > len ? 0 : n;
}

static int64_t log_record_ms(const uint8_t *rec) {
    int64_t ms = 0;
    for (int b = 5; b >= 0; b--) ms = ms * 256 + rec[b];
    return ms;
}

int table_import_session_log(Table *t, const uint8_t *log, int len) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    Game *g = t->g;
    int n = 0, k;
    for (int at = 0; (k = log_record_at(log, len, at)) != 0; at += k)
        if (k < 0) return k;
    for (int at = 0; n < MAX_LOGS && (k = log_record_at(log, len, at)) > 0; at += k) {
        const uint8_t *r = log + at + 6;
        GameLog *l = &g->logs[n++];
        l->log_type = (int8_t)r[0];
        l->player_idx = (int8_t)r[1];
        l->defender_index = (int8_t)r[2];
        l->num_pairs = (int8_t)(r[3] < MAX_LOG_PAIRS ? r[3] : MAX_LOG_PAIRS);
        for (int j = 0; j < l->num_pairs; j++) {
            l->pairs[j].primary = card_from_wire_pair(r[4 + 2 * j]);
            l->pairs[j].target = card_from_wire_pair(r[5 + 2 * j]);
        }
    }
    g->num_logs = n;
    t->log_start = n;
    return n;
}

// Preferred moves: n x { u8 seat, u8 type, u8 n_cards, n_cards wire cards, and
// for a cover its n_cards attack cards } (bot_drive.c compares nothing else).
static int prefs_decode(Table *t, const uint8_t *p, int len) {
    int n = 0, at = 0;
    while (at < len) {
        if (n >= MAX_PLAYERS || at + 3 > len) return TABLE_E_WIRE;
        const int k = p[at + 2], cover = p[at + 1] == MOVE_COVER;
        const int span = 3 + (cover ? 2 : 1) * k;
        if (k > MAX_MOVE_CARDS || at + span > len) return TABLE_E_WIRE;
        for (int c = 3; c < span; c++) if (p[at + c] > 51) return TABLE_E_WIRE;
        BotDrivePref *pr = &t->prefs[n++];
        memset(pr, 0, sizeof(*pr));
        pr->seat = (int8_t)p[at];
        pr->move.type = (int8_t)p[at + 1];
        pr->move.n_cards = (int8_t)k;
        for (int c = 0; c < k; c++) {
            pr->move.cards[c] = card_of_id(p[at + 3 + c]);
            if (cover) pr->move.attack_cards[c] = card_of_id(p[at + 3 + k + c]);
        }
        at += span;
    }
    return n;
}

// The drive's per-decision seeding (bot_drive.h bot_drive_pre_action_hook): the
// strategy stream as a decision starts and the draw stream as its move applies,
// both from the board in front of it and the table's secret base - what the wasm
// bridge's drive does with its own resident game.
static _Thread_local uint32_t t_drive_base;
void (*table_choose_observer)(const Game *g, int seat) = 0;

static void table_drive_seed(const Game *g, int seat, int phase) {
    if (phase == BOT_DRIVE_PHASE_CHOOSE) {
        if (table_choose_observer) table_choose_observer(g, seat);
        random_strategy_set_seed(game_state_seed(g, t_drive_base, 0x9E3779B9u));
    } else {
        game_rng_set(game_state_seed(g, t_drive_base, 0u));
    }
}

int table_bot_drive(Table *t, const uint8_t *prefs, int prefs_len, int max_actions, BotDriveOut *out) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (!out || prefs_len < 0 || (prefs_len > 0 && !prefs)) return TABLE_E_WIRE;
    const int n_prefs = prefs_decode(t, prefs, prefs_len);
    if (n_prefs < 0) { t->n_prefs = 0; return n_prefs; }
    t->n_prefs = (int8_t)n_prefs;

    scope_open(t, -1);   // the records start above whatever the board holds: an imported session log
    void (*const prev_snap)(const Game *, int, int) = engine_snap_hook;
    void (*const prev_seed)(const Game *, int, int) = bot_drive_pre_action_hook;
    t_capture = t->snaps;
    engine_snap_hook = table_snap;
    t_drive_base = t->rng_base;
    bot_drive_pre_action_hook = table_drive_seed;
    const int n = bot_drive(t->g, game_human_mask(t->g), max_actions, n_prefs ? t->prefs : 0, n_prefs, out);
    bot_drive_pre_action_hook = prev_seed;
    engine_snap_hook = prev_snap;
    t_capture = 0;
    if (n < 0) return TABLE_E_WIRE;
    if (n > 0) {
        t->actor = out->actions[n - 1].seat;
        t->ended = finalize(t) >= 0;
    }
    return n;
}

static int pref_put(const BotDrivePref *p, uint8_t *out, int at, int cap) {
    const int k = p->move.n_cards, cover = p->move.type == MOVE_COVER;
    if (at + 3 + (cover ? 2 : 1) * k > cap) return TABLE_E_CAP;
    out[at] = (uint8_t)p->seat;
    out[at + 1] = (uint8_t)p->move.type;
    out[at + 2] = (uint8_t)k;
    for (int c = 0; c < k; c++) {
        out[at + 3 + c] = wire_from_card(p->move.cards[c]);
        if (cover) out[at + 3 + k + c] = wire_from_card(p->move.attack_cards[c]);
    }
    return at + 3 + (cover ? 2 : 1) * k;
}

int table_drive_prefs(const Table *t, const BotDriveOut *drv, uint8_t *out, int cap) {
    BotDrivePref merged[MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < t->n_prefs; i++) merged[n++] = t->prefs[i];
    for (int i = 0; drv && i < drv->n; i++) {
        int j = 0;
        while (j < n && merged[j].seat != drv->actions[i].seat) j++;
        if (j == n) {
            if (n >= MAX_PLAYERS) return TABLE_E_WIRE;
            n++;
        }
        merged[j].seat = drv->actions[i].seat;
        merged[j].move = drv->actions[i].move;
    }
    int at = 0;
    for (int i = 0; i < n; i++)
        if ((at = pref_put(&merged[i], out, at, cap)) < 0) return at;
    return at;
}

int table_cycle_delay_ms(const Table *t, const BotDriveOut *drv) {
    return bot_cycle_delay_ms(t->g, game_human_mask(t->g), drv);
}

static bool info_type(int type) {
    return type == LOG_ATTACK || type == LOG_COVER || type == LOG_PASS || type == LOG_PICKUP;
}

// Where the log's last GAME_START session begins (0 when it has none).
static int session_start(const uint8_t *log, int len) {
    int start = 0, k;
    for (int at = 0; (k = log_record_at(log, len, at)) > 0; at += k)
        if (log[at + 6] == LOG_GAME_START) start = at;
    return start;
}

// A whole session log: every record complete and of a known type. Returns 0 or TABLE_E_WIRE.
static int log_whole(const uint8_t *log, int len) {
    int at = 0, k;
    while ((k = log_record_at(log, len, at)) > 0) at += k;
    return k < 0 || at != len ? TABLE_E_WIRE : 0;
}

// A card byte as the TS readers on both sides of the gate took it: the hidden
// card and (for a target) no card kept, anything else past the last card clamped.
static int gate_card(int b, int target) {
    if (b == 0xFE || (target && b == 0xFF)) return b;
    return b > 51 ? 51 : b;
}

// THE ROUND-TRIP GATE (server/api/common/replay/encode.ts checkInfoActionsMatch):
// the ATTACK, COVER, PASS and PICKUP records of the log's last GAME_START
// session, against the decoded code's, in order - same count, same type, the
// same seat (one the table has), the same card pairs.
static bool replay_verify(int n_seats, const uint8_t *log, int len, const uint8_t *dec, int dec_len) {
    if (dec_len < REPLAY_DEC_HDR) return false;
    const uint32_t n_dec = (uint32_t)dec[16] | ((uint32_t)dec[17] << 8) | ((uint32_t)dec[18] << 16) | ((uint32_t)dec[19] << 24);
    int d = REPLAY_DEC_HDR, at = session_start(log, len), k = 0;
    uint32_t read = 0;
    for (;;) {
        // The next info record on each side.
        while ((k = log_record_at(log, len, at)) > 0 && !info_type(log[at + 6])) at += k;
        const uint8_t *dr = 0;
        while (read < n_dec) {
            if (d + 4 > dec_len || d + 4 + 2 * dec[d + 3] > dec_len) return false;
            const uint8_t *r = dec + d;
            d += 4 + 2 * r[3];
            read++;
            if (info_type(r[0])) { dr = r; break; }
        }
        if (k <= 0 || !dr) return k <= 0 && !dr;
        const uint8_t *lr = log + at + 6;
        if (lr[0] != dr[0] || lr[1] >= n_seats || lr[1] != dr[1] || lr[3] != dr[3]) return false;
        for (int j = 0; j < lr[3]; j++) {
            if (gate_card(lr[4 + 2 * j], 0) != gate_card(dr[4 + 2 * j], 0)) return false;
            if (gate_card(lr[5 + 2 * j], 1) != gate_card(dr[5 + 2 * j], 1)) return false;
        }
        at += k;
    }
}

int table_replay_code(Table *t, const uint8_t *seed, int seed_len, const uint8_t *log, int log_len,
                      uint8_t *out, int cap, uint8_t *scratch, int scratch_cap) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (log_len < 0 || (log_len > 0 && !log) || log_whole(log, log_len) != 0) return TABLE_E_WIRE;
    const int imported = table_import_session_log(t, log, log_len);
    if (imported < 0) return imported;
    const int n = replay_encode_v6_from_game(t->g, seed, seed_len, 1 << 30, out, cap);
    if (n < 0) return n;
    const int dn = replay_decode(out, n, scratch, scratch_cap);
    if (dn < 0) return dn;
    return replay_verify(t->g->num_players, log, log_len, scratch, dn) ? n : TABLE_E_REPLAY_VERIFY;
}

// The session's move times, one at a time (replay_extras.h ReplayExtrasGap): the
// GAME_START and info records of the last session, in order. A cursor, so the
// encoder's in-order walks cost one pass each.
typedef struct {
    const uint8_t *log;
    int len, start;
    int at, index;        // the record `index` is at, after the one at `start`
    double prev;
} TimesCursor;

static bool timed_type(int type) { return type == LOG_GAME_START || info_type(type); }

// The next timed record at or after `at`, or -1.
static int timed_next(const TimesCursor *c, int at) {
    int k;
    while ((k = log_record_at(c->log, c->len, at)) > 0) {
        if (timed_type(c->log[at + 6])) return at;
        at += k;
    }
    return -1;
}

// Exactly the JavaScript arithmetic (Date.parse(created_at) / 1000, then a
// difference): volatile, so a -ffast-math native build cannot fold the division
// and the subtraction into something that rounds differently.
static double record_seconds(const uint8_t *rec) {
    volatile double ms = (double)log_record_ms(rec);
    volatile double s = ms / 1000;
    return s;
}

static double times_gap(void *ctx, int i) {
    TimesCursor *c = (TimesCursor *)ctx;
    if (i == 0 || i != c->index + 1) {   // restart from the first gap
        c->at = timed_next(c, c->start);
        c->prev = record_seconds(c->log + c->at);
        c->index = -1;
        for (int j = 0; j < i; j++) times_gap(ctx, j);
    }
    c->at = timed_next(c, c->at + log_record_at(c->log, c->len, c->at));
    volatile double t = record_seconds(c->log + c->at);
    volatile double g = t - c->prev;
    c->prev = t;
    c->index = i;
    return g > 0 ? g : 0;
}

int table_replay_extras(const Table *t, const uint8_t *log, int log_len, uint8_t *out, int cap) {
    if (!t->loaded) return TABLE_E_NOT_LOADED;
    if (log_len < 0 || (log_len > 0 && !log) || log_whole(log, log_len) != 0) return TABLE_E_WIRE;
    const uint8_t *names[MAX_PLAYERS];
    int lens[MAX_PLAYERS];
    for (int s = 0; s < t->r.n; s++) { names[s] = (const uint8_t *)t->r.seats[s].name; lens[s] = t->r.seats[s].name_len; }
    TimesCursor c = { log, log_len, session_start(log, log_len), 0, -1, 0 };
    int n_times = 0;
    for (int at = timed_next(&c, c.start); at >= 0; at = timed_next(&c, at + log_record_at(log, log_len, at))) n_times++;
    if (n_times - 1 > 0xffff) return TABLE_E_WIRE;
    const int first = timed_next(&c, c.start);
    const int n = replay_extras_encode_parts(names, lens, t->r.n, n_times > 0,
                                             n_times > 0 ? record_seconds(log + first) : 0,
                                             n_times > 0 ? n_times - 1 : 0, times_gap, &c, out, cap);
    return n == -REPLAY_EXTRAS_ECAP ? TABLE_E_CAP : n;
}

// ios_api.c — implementation of the Swift-visible bridge (see ios/include/ios_api.h).
//
// One static Game, no threads inside (the Swift EngineC wrapper serializes every
// call onto a single queue). Everything Swift reads leaves here as JSON built by
// the tiny appender below; the one move Swift sends arrives as JSON parsed by the
// tiny scanner below. No Durak rule lives here — this file only marshals to and
// from game.c / legal.c / view.c / replay.c (docs/IOS_APP_DESIGN.md §3, §16.0).

#include "ios_api.h"

#include "game.h"
#include "legal.h"
#include "view.h"
#include "replay.h"
#include "replay_steps.h"
#include "strategy.h"
#include "bot_roster.h"
#include "bot_drive.h"
#include "evwire.h"
#include "msg_wire.h"
#include "anim_plan.h"
#include "awire.h"
#include "sha256.h"
#include "json_out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// This file used to carry its own JSON appender and board emitter. They live in
// src/json_out.c now, because the web needed exactly the same decode and the
// only alternative was a third implementation of the layout (A8/F7). The error
// codes line up so json_out's returns pass straight through untranslated —
// pinned here rather than trusted.
_Static_assert(JSON_EBADARG == FIO_EBADARG, "json_out/ios error codes diverged");
_Static_assert(JSON_ECAP    == FIO_ECAP,    "json_out/ios error codes diverged");
_Static_assert(JSON_EPARSE  == FIO_EPARSE,  "json_out/ios error codes diverged");

// ---------- the one game --------------------------------------------------

static Game  g_game;
static int   g_has_game = 0;
static int   g_last_reject = 0;
static int   g_last_replay_error = 0;

// The deal seed this game was dealt from, kept from fio_new_game (see there).
// Zero when the game came from a short/absent seed, i.e. the legacy LCG deal —
// which cannot be re-derived, so those games can only ever encode as v5.
static uint8_t g_deal_seed[FOOLISH_SEED_LEN];
static int     g_has_deal_seed = 0;

// ---------- strategy roster (offline bots, §7.2) --------------------------
//
// The roster itself lives in the kernel (src/bot_roster.c) — one table shared
// with the server and every future client, so a bot named "cordite" here is
// the same brain at the same tuning as the website's cordite. This file used
// to carry its own copy, which mapped the player-facing rungs at the arena
// variants of handwritten/espresso and applied no tuning knobs at all; both
// bugs are gone with the table (docs/C_CORE_CONSOLIDATION.md §3.1, §4.1).
//
// A FIO strategy id is an index into the OFFLINE projection of the roster (the
// picker's rungs in strength order, docs/IOS_BOT_NAMING.md §1) — not a raw
// roster index and not a STRAT_* id, so the ids stay stable as unshipped
// research brains come and go from the table.
static int fio_roster_idx(int strategy_id) {
    return bot_roster_offline_at(strategy_id);
}

// Per-seat roster index for the seats fio_set_seat_strategy assigned.
// players[].strategy_key cannot carry this: it holds a STRAT_* id by
// kernel-wide convention and the kernel reads it (espresso_prod_strategy.c
// checks strategy_key == STRAT_RANDOM to mirror the TS bot's random-opponent
// special case). Seats default to the `random` entry, matching the old
// strategy_key == 0 == STRAT_RANDOM default: a seat nobody assigned but that
// fio_bot_step_json(-1) drives anyway still plays random, as before.
static int8_t g_seat_roster[MAX_PLAYERS];

// ---------- legal moves ----------------------------------------------------

static const char *move_type_name(int t) {
    switch (t) {
        case MOVE_ATTACK: return "attack";
        case MOVE_COVER:  return "cover";
        case MOVE_PASS:   return "pass";
        case MOVE_PICKUP: return "pickup";
        case MOVE_GOOD:   return "good";
        case MOVE_WAIT:   return "wait";
        default:          return "unknown";
    }
}

// Emit one LegalMove as a JSON object (no leading comma).
static void emit_move_obj(J *j, const LegalMove *m) {
    j_puts(j, "{\"type\":"); j_putstr(j, move_type_name(m->type));
    j_puts(j, ",\"cards\":[");
    for (int c = 0; c < m->n_cards; c++) { if (c) j_putc(j, ','); j_card(j, m->cards[c]); }
    j_putc(j, ']');
    if (m->type == MOVE_COVER) {
        j_puts(j, ",\"attackCards\":[");
        for (int c = 0; c < m->n_cards; c++) { if (c) j_putc(j, ','); j_card(j, m->attack_cards[c]); }
        j_putc(j, ']');
    }
    j_putc(j, '}');
}

static int emit_legal_of(const Game *g, int seat, char *out, int cap) {
    LegalMoves moves;
    calculate_legal_moves(g, seat, &moves);
    J j; j_init(&j, out, cap);
    j_putc(&j, '[');
    for (int i = 0; i < moves.n; i++) { if (i) j_putc(&j, ','); emit_move_obj(&j, &moves.moves[i]); }
    j_putc(&j, ']');
    return j_finish(&j);
}

int fio_legal_moves_json(int seat, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (seat < 0 || seat >= g_game.num_players) return FIO_EBADARG;
    return emit_legal_of(&g_game, seat, out, cap);
}

// ---------- packed emitters (no JSON — the packed kernel wire) --------------
//
// Client and server are kernel-to-kernel, so these hand out the SAME bytes the
// wasm build does and Swift decodes them directly (MaskedView / MoveWire). The
// _json twins above are being retired (owner: wipe the JSON).

// The seat's legal moves as the packed move wire — identical layout to
// wasm_export_moves: u32 count, then per move {type, n_cards, cards[n_cards],
// attacks[n_cards]}. Cards use card_to_id (== wire_from_card for real cards);
// the reader ignores the attack bytes except on COVER.
static int emit_legal_packed(const Game *g, int seat, char *out, int cap) {
    static LegalMoves lm;
    calculate_legal_moves(g, seat, &lm);
    // NOTE: GOOD is intentionally NOT filtered here. The kernel menu stays the
    // full legal set (so fio_actor_mask and this packed menu agree, and bots see
    // GOOD as always). The owner's "a human only gets Good once the table is
    // fully covered, and it disappears when someone throws in" rule is enforced
    // in the UI (CardPlay.canSayGood = has(.good) && all covered), and the
    // "your move with no live button" status case is handled in the board's
    // status logic, not by rewriting the kernel's legal set.
    if (cap < 4) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    unsigned int n = (unsigned int)lm.n;
    *q++ = n & 0xff; *q++ = (n >> 8) & 0xff; *q++ = (n >> 16) & 0xff; *q++ = (n >> 24) & 0xff;
    for (int i = 0; i < lm.n; i++) {
        const LegalMove *m = &lm.moves[i];
        if ((int)((char *)q - out) + 2 + 2 * m->n_cards > cap) return FIO_ECAP;
        *q++ = (unsigned char)m->type;
        *q++ = (unsigned char)m->n_cards;
        for (int j = 0; j < m->n_cards; j++) *q++ = (unsigned char)card_to_id(m->cards[j]);
        for (int j = 0; j < m->n_cards; j++) *q++ = (unsigned char)card_to_id(m->attack_cards[j]);
    }
    return (int)((char *)q - out);
}

// The resident game's masked view for `viewer`, as the packed state wire
// (view.c state_put). Swift's MaskedView decoder reads it.
int fio_state_packed(int viewer, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (cap < 1024) return FIO_ECAP;   // state_put is unbounded; guard the buffer
    return state_put(&g_game, viewer, (unsigned char *)out);
}

// The resident game's legal moves for `seat`, packed.
int fio_legal_packed(int seat, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (seat < 0 || seat >= g_game.num_players) return FIO_EBADARG;
    return emit_legal_packed(&g_game, seat, out, cap);
}

// Legal moves for `seat` computed from a SERVER packed masked view, packed out.
int fio_legal_from_packed(const uint8_t *buf, int len, int seat, char *out, int cap) {
    if (!buf || len <= 0) return FIO_EBADARG;
    static Game tmp;
    memset(&tmp, 0, sizeof tmp);
    state_get(&tmp, buf, /*masked=*/1);
    if (tmp.num_players < 2 || tmp.num_players > MAX_PLAYERS) return FIO_EPARSE;
    if (seat < 0 || seat >= tmp.num_players) return FIO_EBADARG;
    return emit_legal_packed(&tmp, seat, out, cap);
}

// ---------- animation events (§4.4 / A3) -----------------------------------
//
// The animation plan — which card flies where, in what order — is derived by
// the kernel and always has been: the website decodes the evwire stream and
// plays it, never deriving anything. Offline there is no server to send that
// stream, and the iOS design's answer used to be a Swift diff engine
// (`BoardDiff.swift`, "given (old GameView, new GameView) produce moves").
// That would have been a THIRD implementation of the same derivation, and
// legacy the day it was written.
//
// So: the same kernel hooks that feed the web's evwire feed this too
// (evwire_walk, the one derivation), and Swift gets JSON — never packed bytes,
// per this file's bridge rule.

#define FIO_MAX_SNAPS 48
#define GAME_PREFIX_SIZE (__builtin_offsetof(Game, num_logs))
typedef struct { _Alignas(8) unsigned char bytes[GAME_PREFIX_SIZE]; } FioSnapSlot;

static FioSnapSlot g_snaps[FIO_MAX_SNAPS];
static int         g_snap_tags[FIO_MAX_SNAPS];
static int         g_snap_aux[FIO_MAX_SNAPS];
static int         g_n_snaps = 0;
// Where the last apply's / drive's own logs begin in the resident game log.
static int         g_last_event_log_start = 0;

// state_put/put_state only read prefix fields, which is exactly what a slot
// holds — so a slot can stand in for a Game when an event is rendered.
static void fio_snap_cb(const Game *g, int tag, int aux) {
    if (g_n_snaps >= FIO_MAX_SNAPS) return;
    memcpy(g_snaps[g_n_snaps].bytes, g, GAME_PREFIX_SIZE);
    g_snap_tags[g_n_snaps] = tag;
    g_snap_aux[g_n_snaps] = aux;
    g_n_snaps++;
}

static void fio_snaps_reset(void) { g_n_snaps = 0; }

// The JSON sink for evwire_walk. Mirrors the evwire field names so the Swift
// decoder and the web's decoder describe the same event.
typedef struct { J *j; int n; int viewer; } FioEvCtx;

static void fio_ev_sink(void *ctx, const EvwEvent *ev) {
    FioEvCtx *c = (FioEvCtx *)ctx;
    if (c->n++) j_putc(c->j, ',');
    j_puts(c->j, "{\"type\":");   j_puti(c->j, ev->type);
    j_puts(c->j, ",\"seat\":");   j_puti(c->j, ev->seat);
    j_puts(c->j, ",\"msg\":");    j_puti(c->j, ev->msg);
    j_puts(c->j, ",\"from\":");   j_puti(c->j, ev->from);
    j_puts(c->j, ",\"to\":");     j_puti(c->j, ev->to);
    j_puts(c->j, ",\"cards\":[");
    for (int i = 0; i < ev->n_cards; i++) {
        if (i) j_putc(c->j, ',');
        // The DEAL/REFILL redaction: a card bound for someone else's hand is a
        // card back. The kernel masks it; the app cannot leak what it never got.
        if (ev->mask_cards) j_puts(c->j, "null");
        else j_card(c->j, ev->cards[i]);
    }
    j_putc(c->j, ']');
    if (ev->has_target) { j_puts(c->j, ",\"target\":"); j_card(c->j, ev->target); }
    if (ev->has_battle) { j_puts(c->j, ",\"battle\":"); j_puti(c->j, ev->battle); }
    // The board AS OF this step, masked for this viewer — the same thing the
    // web's evwire carries per event (the `snap_len` payload) and commits when
    // the step's animation lands. Without it a multi-action cycle could only be
    // drawn at its final state, and the only way back to the intermediate boards
    // would be for the client to re-derive them — which is precisely what
    // BoardDiff.swift was cancelled for (§16.B4).
    if (ev->snap) { j_puts(c->j, ",\"state\":"); json_state(c->j, ev->snap, c->viewer); }
    j_putc(c->j, '}');
}

// Emit the events captured since the last reset, as seen by `viewer`.
//
// `log_start` is where THIS action's logs begin. The wasm bridge can hand
// evwire the whole log buffer because it clears it per call (it re-marshals
// the game every time); this file keeps ONE resident Game whose log is the
// game's entire history — the replay encoder and the belief bots both read it
// — so the fresh entries are sliced off, never cleared.
static void j_events(J *j, int viewer, int log_start) {
    EvSnap refs[FIO_MAX_SNAPS];
    for (int i = 0; i < g_n_snaps; i++) {
        refs[i].g = (const Game *)(const void *)g_snaps[i].bytes;
        refs[i].tag = g_snap_tags[i];
        refs[i].aux = g_snap_aux[i];
    }
    FioEvCtx ctx = { j, 0, viewer };
    j_putc(j, '[');
    evwire_walk(refs, g_n_snaps, g_game.logs + log_start, g_game.num_logs - log_start,
                viewer, fio_ev_sink, &ctx);
    j_putc(j, ']');
}

// ---------- a tiny JSON move parser ---------------------------------------
//
// Only the exact move shape is parsed: {"type":"attack","cards":[{"s":0,"v":6}],
// "attackCards":[...]}. Not a general JSON parser — a scanner that pulls the
// "type" token and the {s,v} pairs out of the named arrays. Anything malformed
// returns a parse failure and the move is refused (never guessed).

// Point `p` just past the value of key `"<key>"` (past the following ':'), or
// return NULL if the key is absent.
static const char *find_key(const char *json, const char *key) {
    char pat[32];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// Parse a leading integer (optional '-') at *p, advancing *p past it.
static long scan_int(const char **p) {
    long sign = 1, v = 0;
    if (**p == '-') { sign = -1; (*p)++; }
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return sign * v;
}

// Parse the array of {"s":..,"v":..} objects that is the value at `arr` (which
// must point at a '['). Fills up to `cap` cards; returns the count, or -1 on
// malformed input. Stops at the matching ']'.
static int parse_card_array(const char *arr, Card *out, int cap) {
    if (!arr || *arr != '[') return -1;
    const char *p = arr + 1;
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (*p == ']') return n;
        if (*p != '{') return -1;
        const char *sp = find_key(p, "s");
        const char *vp = find_key(p, "v");
        if (!sp || !vp) return -1;
        long s = scan_int(&sp);
        long v = scan_int(&vp);
        if (n >= cap) return -1;
        out[n].suit  = (int8_t)s;
        out[n].value = (int8_t)v;
        n++;
        // advance p past this object's closing '}'
        const char *close = strchr(p, '}');
        if (!close) return -1;
        p = close + 1;
    }
}

// {"type":"cover","cards":[...],"attackCards":[...]} -> AwireAction.
// Factored out of fio_apply_json so the FMSG rebase path parses a move exactly
// the way the play path does — two parsers would be two move languages.
static int fio_move_to_awire(const char *move_json, AwireAction *out) {
    const char *tp = find_key(move_json, "type");
    if (!tp || *tp != '"') return FIO_EPARSE;
    tp++;
    char type[16] = {0};
    int ti = 0;
    while (*tp && *tp != '"' && ti < (int)sizeof(type) - 1) type[ti++] = *tp++;

    Card cards[MAX_MOVE_CARDS], acards[MAX_MOVE_CARDS];
    int nc = 0, nac = 0;
    const char *cp = find_key(move_json, "cards");
    if (cp) { nc = parse_card_array(cp, cards, MAX_MOVE_CARDS); if (nc < 0) return FIO_EPARSE; }
    const char *ap = find_key(move_json, "attackCards");
    if (ap) { nac = parse_card_array(ap, acards, MAX_MOVE_CARDS); if (nac < 0) return FIO_EPARSE; }

    if      (!strcmp(type, "attack")) out->kind = AWIRE_ATTACK;
    else if (!strcmp(type, "pass"))   out->kind = AWIRE_PASS;
    else if (!strcmp(type, "pickup")) out->kind = AWIRE_PICKUP;
    else if (!strcmp(type, "good"))   out->kind = AWIRE_GOOD;
    else if (!strcmp(type, "cover")) {
        if (nac != nc) return FIO_EPARSE;
        out->kind = AWIRE_COVER;
    } else return FIO_EPARSE;

    if (out->kind == AWIRE_PICKUP || out->kind == AWIRE_GOOD) { out->n = 0; return FIO_EOK; }
    if (nc > AWIRE_MAX_CARDS) return FIO_EPARSE;
    out->n = nc;
    for (int i = 0; i < nc; i++) {
        out->cards[i] = cards[i];
        if (out->kind == AWIRE_COVER) out->attacks[i] = acards[i];
    }
    return FIO_EOK;
}

// The client sends the awire action bytes (no JSON move) — fio_apply_awire is the
// one apply entry now. The FMSG rebase path still parses a move from JSON via
// fio_move_to_awire (above); a plain apply never does.
int fio_apply_awire(int actor_seat, const uint8_t *buf, int len) {
    if (!g_has_game) return FIO_ENOGAME;
    if (!buf || len <= 0) return FIO_EBADARG;
    if (actor_seat < 0 || actor_seat >= g_game.num_players) return FIO_EBADARG;
    AwireAction a;
    if (!awire_decode(buf, len, &a)) return FIO_EPARSE;

    fio_snaps_reset();
    g_last_event_log_start = g_game.num_logs;
    engine_snap_hook = fio_snap_cb;
    engine_last_reject = ENGINE_REJECT_NONE;
    bool ok = awire_apply(&g_game, actor_seat, &a);   // the kernel owns the switch
    engine_snap_hook = 0;
    if (!ok) { fio_snaps_reset(); g_last_reject = engine_last_reject; return FIO_EREJECT; }
    g_last_reject = 0;
    return FIO_EOK;
}

int fio_last_events_json(int viewer, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    J j; j_init(&j, out, cap);
    j_events(&j, viewer, g_last_event_log_start);
    return j_finish(&j);
}

// ---------- animation core (c/src/anim_plan.h) -----------------------------
//
// The plan/policy the web already runs through wasm, exposed to Swift. The plan
// builder consumes the SAME evwire_walk output j_events emits (EVW_T_*/EVW_LOC_*
// numbering is identical to ANIM_EVT_*/ANIM_LOC_*, so the EvwEvent copies
// straight into an AnimEvent), so a plan is derived from exactly the events the
// board would otherwise re-walk. No allocation: the events and their cards live
// in file-static scratch (this file is single-threaded by contract).

typedef struct { AnimEvent *ev; Card *pool; int cap_ev; int cap_pool; int n; int pool_n; } FioPlanCtx;

static void fio_plan_sink(void *ctx, const EvwEvent *e) {
    FioPlanCtx *c = (FioPlanCtx *)ctx;
    if (c->n >= c->cap_ev) return;
    if (e->n_cards > 0 && c->pool_n + e->n_cards > c->cap_pool) return;
    AnimEvent *a = &c->ev[c->n];
    a->type = e->type;   // EVW_T_* == ANIM_EVT_*
    a->seat = e->seat;   // -1 == ANIM_SEAT_NONE
    a->from = e->from;   // EVW_LOC_* == ANIM_LOC_*
    a->to   = e->to;
    a->mask_cards = e->mask_cards;
    a->cards = &c->pool[c->pool_n];
    a->n_cards = e->n_cards;
    for (int i = 0; i < e->n_cards; i++) c->pool[c->pool_n++] = e->cards[i];
    c->n++;
}

int fio_anim_plan_json(int viewer, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;

    // Walk the viewer's last-move events into AnimEvent[] (same snapshots as
    // j_events), then build the plan against the resident (final) board's counts.
    EvSnap refs[FIO_MAX_SNAPS];
    for (int i = 0; i < g_n_snaps; i++) {
        refs[i].g = (const Game *)(const void *)g_snaps[i].bytes;
        refs[i].tag = g_snap_tags[i];
        refs[i].aux = g_snap_aux[i];
    }
    static AnimEvent s_ev[ANIM_MAX_STEPS];
    static Card s_pool[ANIM_MAX_STEPS * ANIM_MAX_CARDS];
    FioPlanCtx ctx = { s_ev, s_pool, ANIM_MAX_STEPS,
                       (int)(sizeof(s_pool)/sizeof(s_pool[0])), 0, 0 };
    evwire_walk(refs, g_n_snaps, g_game.logs + g_last_event_log_start,
                g_game.num_logs - g_last_event_log_start, viewer, fio_plan_sink, &ctx);

    const int np = g_game.num_players;
    int final_hand[MAX_PLAYERS];
    for (int s = 0; s < np; s++) final_hand[s] = g_game.players[s].hand_count;

    static AnimPlan plan;
    const int rc = anim_build_plan(s_ev, ctx.n, np, g_game.deck_count,
                                   g_game.discard_pile_length, final_hand, &plan);
    if (rc != ANIM_EOK) return FIO_EBADARG;

    J j; j_init(&j, out, cap);
    j_puts(&j, "{\"durationMs\":"); j_puti(&j, ANIM_TIME_MS);
    j_puts(&j, ",\"gapMs\":");      j_puti(&j, ANIM_GAP_MS);
    j_puts(&j, ",\"totalMs\":");    j_puti(&j, plan.total_ms);
    j_puts(&j, ",\"nPlayers\":");   j_puti(&j, np);
    j_puts(&j, ",\"pre\":{\"deck\":"); j_puti(&j, plan.pre.deck);
    j_puts(&j, ",\"discard\":");    j_puti(&j, plan.pre.discard);
    j_puts(&j, ",\"hand\":[");
    for (int s = 0; s < np; s++) { if (s) j_putc(&j, ','); j_puti(&j, plan.pre.hand[s]); }
    j_puts(&j, "]},\"veil\":[");
    for (int i = 0; i < plan.n_veil; i++) { if (i) j_putc(&j, ','); j_puti(&j, plan.veil_ids[i]); }
    j_puts(&j, "],\"steps\":[");
    for (int i = 0; i < plan.n_steps; i++) {
        const AnimPlanStep *st = &plan.steps[i];
        if (i) j_putc(&j, ',');
        j_puts(&j, "{\"type\":");   j_puti(&j, st->type);
        j_puts(&j, ",\"seat\":");   j_puti(&j, st->seat);
        j_puts(&j, ",\"from\":");   j_puti(&j, st->from);
        j_puts(&j, ",\"to\":");     j_puti(&j, st->to);
        j_puts(&j, ",\"nCards\":"); j_puti(&j, st->n_cards);
        j_puts(&j, ",\"durationMs\":"); j_puti(&j, st->duration_ms);
        j_puts(&j, ",\"startMs\":");     j_puti(&j, st->start_ms);
        j_puts(&j, ",\"deck\":");        j_puti(&j, st->deck);
        j_puts(&j, ",\"discard\":");     j_puti(&j, st->discard);
        j_puts(&j, ",\"inFlightFromDeck\":");  j_puti(&j, st->in_flight_from_deck);
        j_puts(&j, ",\"inFlightToFlipped\":"); j_puti(&j, st->in_flight_to_flipped);
        j_puts(&j, ",\"hand\":[");
        for (int s = 0; s < np; s++) { if (s) j_putc(&j, ','); j_puti(&j, st->hand[s]); }
        j_puts(&j, "]}");
    }
    j_puts(&j, "]}");
    return j_finish(&j);
}

int fio_anim_should_drop_stale(int has_last, int last, int has_incoming, int incoming) {
    return anim_should_drop_stale(has_last, last, has_incoming, incoming);
}

int fio_last_reject(void) { return g_last_reject; }

// ---------- lifecycle & turn queries --------------------------------------

int fio_new_game(const uint8_t *seed, int seed_len, int n_players) {
    if (n_players < 2 || n_players > MAX_PLAYERS) return FIO_EBADARG;

    // Deal RNG: wide (ChaCha) mode when 32+ seed bytes are supplied — the whole
    // deal space, reproducible on any platform (deal_rng.h). Otherwise fall back
    // to the legacy 32-bit LCG seed from the first 4 bytes (golden fixtures).
    g_has_deal_seed = 0;
    if (seed && seed_len >= FOOLISH_SEED_LEN) {
        game_set_deal_seed_bytes(seed, seed_len);
        // Keep it: it is what makes this game's replay v6 (exact hands) instead
        // of v5 (retrodicted). Held here rather than handed back to Swift at
        // share time so the app never has to know a replay needs a deal seed —
        // fio_replay_encode_v6_b32 takes no arguments.
        memcpy(g_deal_seed, seed, FOOLISH_SEED_LEN);
        g_has_deal_seed = 1;
    } else {
        uint32_t s = 0;
        if (seed) for (int i = 0; i < seed_len && i < 4; i++) s |= ((uint32_t)seed[i]) << (8 * i);
        game_set_seed(s);
        random_strategy_set_seed(s);
    }

    memset(&g_game, 0, sizeof(g_game));
    // Identity (player_id) and the host-side roster mirror are ours to set; the
    // seat count, per-seat kind, and the deal are the kernel's (game_seat_and_deal).
    int8_t strategies[MAX_PLAYERS];
    for (int i = 0; i < n_players; i++) {
        strategies[i] = STRATEGY_KEY_HUMAN;  // all human until fio_set_seat_strategy
        g_seat_roster[i] = (int8_t)bot_roster_find("random");
        snprintf(g_game.players[i].player_id, sizeof(g_game.players[i].player_id), "p%d", i);
    }
    game_seat_and_deal(&g_game, strategies, n_players);
    g_has_game = 1;
    g_last_reject = 0;
    return FIO_EOK;
}

// Re-deal the CURRENT resident game's own LOCKED seed at a different player
// count — the iMessage lobby's "Start" action (docs/IMESSAGE_LOBBY_V2.md): a
// group lobby is created OPEN (fio_new_game with the wire's max capacity, 8,
// §5.2) so seats stay free to fill; when the joined players decide to start,
// this re-derives the SAME seed's deal at the ACTUAL joined count (seats are
// claimed lowest-first, so it is always a contiguous 0..<n) — never a new
// random seed, which is the "locked at create" guarantee the lobby promises.
//
// Just fio_new_game fed g_deal_seed back to itself: the seed already lives in
// the resident-game statics (kept there from whichever call last dealt or
// decoded it — fio_new_game or fio_msg_decode_packed), so it never has to
// cross back out to Swift and back in, mirroring the same "the kernel keeps
// the seed, the app never touches it" discipline fio_replay_encode_v6_b32
// already relies on. Returns FIO_ENOSEED if no wide seed is resident (nothing
// to re-derive from — a lobby is always created wide-seeded, so this is only
// reachable by calling it out of order), or whatever fio_new_game returns for
// a bad n_players.
int fio_reseat_game(int n_players) {
    if (!g_has_deal_seed) return FIO_ENOSEED;
    uint8_t seed[FOOLISH_SEED_LEN];
    memcpy(seed, g_deal_seed, FOOLISH_SEED_LEN);   // copy out first: fio_new_game
    return fio_new_game(seed, FOOLISH_SEED_LEN, n_players);  // will overwrite g_deal_seed
}

int fio_set_seat_strategy(int seat, int strategy_id) {
    if (!g_has_game) return FIO_ENOGAME;
    if (seat < 0 || seat >= g_game.num_players) return FIO_EBADARG;
    int idx = fio_roster_idx(strategy_id);
    const BotRosterEntry *e = bot_roster_at(idx);
    if (!e) return FIO_ENOSTRAT;
    g_seat_roster[seat] = (int8_t)idx;
    g_game.players[seat].strategy_key = (int8_t)e->strat;
    return FIO_EOK;
}

int fio_has_game(void) { return g_has_game; }

// A server packed-view blob decodes to a GameView in pure Swift (MaskedView),
// and legal moves from that blob come through the PACKED fio_legal_from_packed
// (view.ts / MoveWire) — so the JSON packed-view bridges that lived here
// (fio_view_from_packed_json / fio_legal_from_packed_json) are gone with the
// JSON surface, along with fio_public_state_json (unused: publicState() reads
// fio_state_packed).

int fio_actor_mask(void) {
    if (!g_has_game) return FIO_ENOGAME;
    int mask = 0;
    for (int i = 0; i < g_game.num_players; i++) if (should_bot_act(&g_game, i)) mask |= (1 << i);
    return mask;
}

int fio_game_over(void) {
    if (!g_has_game) return FIO_ENOGAME;
    return game_done(&g_game);
}

// (The legacy fio_bot_step_json is gone: it re-implemented the handle_* dispatch
// and drifted from the canonical cycle. Everything drives through the packed
// fio_bot_drive_packed now — the same cycle the app and website run.)

// Emit one applied action: the move's fields, plus its seat and pacing class.
static void j_drive_action(J *j, const BotDriveAction *a) {
    const LegalMove *m = &a->move;
    j_puts(j, "{\"seat\":");  j_puti(j, a->seat);
    j_puts(j, ",\"pace\":");  j_puti(j, a->pacing_class);
    j_puts(j, ",\"type\":");  j_putstr(j, move_type_name(m->type));
    j_puts(j, ",\"cards\":[");
    for (int c = 0; c < m->n_cards; c++) { if (c) j_putc(j, ','); j_card(j, m->cards[c]); }
    j_putc(j, ']');
    if (m->type == MOVE_COVER) {
        j_puts(j, ",\"attackCards\":[");
        for (int c = 0; c < m->n_cards; c++) { if (c) j_putc(j, ','); j_card(j, m->attack_cards[c]); }
        j_putc(j, ']');
    }
    j_putc(j, '}');
}

int fio_bot_drive_json(int human_mask, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;

    // Events for the WHOLE cycle: the hooks accumulate across the bundled
    // actions, which is exactly the sequence the board should play.
    fio_snaps_reset();
    const int log_start = g_game.num_logs;
    g_last_event_log_start = log_start;
    engine_snap_hook = fio_snap_cb;

    static BotDriveOut drv;   // ~1KB of LegalMoves; not a stack citizen
    bot_drive(&g_game, (uint32_t)human_mask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv);

    engine_snap_hook = 0;

    // A human still in the game is what makes a pause worth taking. Offline
    // that is the seat the app is playing; a spectate/replay drive passes
    // human_mask = 0 and gets the bots-only pace.
    int humans_present = 0;
    for (int seat = 0; seat < g_game.num_players; seat++)
        if ((human_mask & (1 << seat)) && g_game.players[seat].status == PLAYER_STATUS_IN)
            humans_present = 1;

    // The cycle waits for the most visible thing that happened in it.
    int pace = BOT_PACE_NONE;
    for (int i = 0; i < drv.n; i++)
        if (drv.actions[i].pacing_class > pace) pace = drv.actions[i].pacing_class;

    // The lowest human seat is the viewer — offline that is the seat the app
    // plays. A spectate/replay drive (human_mask 0) watches as a spectator.
    int viewer = VIEW_SPECTATOR;
    for (int seat = 0; seat < g_game.num_players; seat++)
        if (human_mask & (1 << seat)) { viewer = seat; break; }

    J j; j_init(&j, out, cap);
    j_puts(&j, "{\"actions\":[");
    for (int i = 0; i < drv.n; i++) { if (i) j_putc(&j, ','); j_drive_action(&j, &drv.actions[i]); }
    j_puts(&j, "],\"events\":");
    j_events(&j, viewer, log_start);
    j_puts(&j, ",\"stop\":");    j_puti(&j, drv.stop);
    j_puts(&j, ",\"ended\":");   j_puti(&j, drv.ended);
    j_puts(&j, ",\"delayMs\":"); j_puti(&j, bot_pacing_ms(pace, humans_present));
    j_putc(&j, '}');
    return j_finish(&j);
}

// PACKED bot-drive — same one kernel cycle, result packed instead of JSON:
//   u32 n_actions, per action {seat, pace, type, n_cards, cards[], attacks[]},
//   then i32 stop, i32 ended, i32 delayMs (LE).
// Events are NOT carried (the app doesn't consume them until B4 animation; they
// come back as packed evwire then). Owner: wipe the JSON.
static void le_i32(unsigned char **q, int v) {
    unsigned int u = (unsigned int)v;
    *(*q)++ = u & 0xff; *(*q)++ = (u >> 8) & 0xff; *(*q)++ = (u >> 16) & 0xff; *(*q)++ = (u >> 24) & 0xff;
}
int fio_bot_drive_packed(int human_mask, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    static BotDriveOut drv;
    bot_drive(&g_game, (uint32_t)human_mask, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv);
    const int delay_ms = bot_cycle_delay_ms(&g_game, (uint32_t)human_mask, &drv);

    if (cap < 4) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    unsigned int n = (unsigned int)drv.n;
    *q++ = n & 0xff; *q++ = (n >> 8) & 0xff; *q++ = (n >> 16) & 0xff; *q++ = (n >> 24) & 0xff;
    for (int i = 0; i < drv.n; i++) {
        const BotDriveAction *a = &drv.actions[i];
        const LegalMove *m = &a->move;
        if ((int)((char *)q - out) + 4 + 2 * m->n_cards + 12 > cap) return FIO_ECAP;
        *q++ = (unsigned char)a->seat;
        *q++ = (unsigned char)a->pacing_class;
        *q++ = (unsigned char)m->type;
        *q++ = (unsigned char)m->n_cards;
        for (int c = 0; c < m->n_cards; c++) *q++ = (unsigned char)card_to_id(m->cards[c]);
        for (int c = 0; c < m->n_cards; c++) *q++ = (unsigned char)card_to_id(m->attack_cards[c]);
    }
    le_i32(&q, drv.stop);
    le_i32(&q, drv.ended);
    le_i32(&q, delay_ms);
    return (int)((char *)q - out);
}

// ---------- strategies -----------------------------------------------------

int fio_strategy_count(void) { return bot_roster_offline_count(); }

int fio_strategy_name(int id, char *out, int cap) {
    const BotRosterEntry *e = bot_roster_at(fio_roster_idx(id));
    if (!e) return FIO_ENOSTRAT;
    J j; j_init(&j, out, cap);
    j_puts(&j, e->key);
    return j_finish(&j);
}

// ---------- replays (§16.C) ------------------------------------------------
//
// DECODE is implemented: base32 (RFC 4648 uppercase, no padding — the web's
// codec.ts alphabet) → the replay integer bytes → replay_decode() → JSON. This
// is byte-parity with the server by construction (shared replay.c), so a
// web-generated code plays natively. ENCODE (share-your-game) still needs the
// encode.ts log→action synthesis and lands later this milestone.

// RFC 4648 base32 decode, MSB-first bit packing (mirrors codec.ts base32Decode).
// Ignores any char outside A-Z/2-7 (so a `-extras` suffix or stray chars are
// skipped). Returns bytes written, or -1 on overflow.
static int b32_decode(const char *s, unsigned char *out, int cap) {
    int bits = 0, value = 0, n = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '-') break;                 // extras suffix begins here
        int idx = -1;
        if (c >= 'A' && c <= 'Z') idx = c - 'A';
        else if (c >= 'a' && c <= 'z') idx = c - 'a';   // accept lowercase
        else if (c >= '2' && c <= '7') idx = c - '2' + 26;
        else continue;                       // ignore stray chars ('.', '/', ...)
        value = (value << 5) | idx;
        bits += 5;
        if (bits >= 8) {
            if (n >= cap) return -1;
            out[n++] = (unsigned char)((value >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return n;
}

// Decode one wire card byte into JSON: null / hidden {-1,-1} / real {s,v}.

// card → wire byte (suit*13 + value-1).
static unsigned char wire_of(Card c) {
    if (card_is_none(c)) return REPLAY_CARD_NONE;
    if (c.suit < 0 || c.value < 0) return REPLAY_CARD_HIDDEN;
    return (unsigned char)(c.suit * 13 + (c.value - 1));
}

// RFC 4648 base32 encode, MSB-first, no padding (mirrors codec.ts base32Encode).
static const char B32_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
static int b32_encode(const unsigned char *in, int n, char *out, int cap) {
    int bits = 0, value = 0, w = 0;
    for (int i = 0; i < n; i++) {
        value = (value << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            if (w >= cap - 1) return -1;
            out[w++] = B32_ALPHA[(value >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) { if (w >= cap - 1) return -1; out[w++] = B32_ALPHA[(value << (5 - bits)) & 31]; }
    out[w] = 0;
    return w;
}

// makeSource (encode.ts / replay_difftest.c build_encode_input): the info logs
// (attack/cover/pass/pickup) plus a round_end marker for every DISCARD directly
// preceded by a GOOD. Header = [n][trump_id][first_attacker][u16 n_actions].
static int build_encode_input(const Game *g, unsigned char *out, int cap) {
    if (g->num_logs >= MAX_LOGS) return -1;   // log buffer overflowed → untrusted
    int trump_id = g->flipped.suit * 13 + (g->flipped.value - 1);
    int q = 5, n_actions = 0;
    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *l = &g->logs[i];
        int info = l->log_type == LOG_ATTACK || l->log_type == LOG_COVER
                || l->log_type == LOG_PASS || l->log_type == LOG_PICKUP;
        if (info) {
            if (q + 3 + l->num_pairs * 2 > cap) return -1;
            out[q++] = (unsigned char)l->log_type;
            out[q++] = (unsigned char)l->player_idx;
            out[q++] = (unsigned char)l->num_pairs;
            for (int jx = 0; jx < l->num_pairs; jx++) {
                out[q++] = wire_of(l->pairs[jx].primary);
                out[q++] = wire_of(l->pairs[jx].target);
            }
            n_actions++;
        } else if (l->log_type == LOG_DISCARD && i > 0 && g->logs[i - 1].log_type == LOG_GOOD) {
            if (q + 3 > cap) return -1;
            out[q++] = (unsigned char)REPLAY_ROUND_END;
            out[q++] = 0xFF;
            out[q++] = 0;
            n_actions++;
        }
    }
    out[0] = (unsigned char)g->num_players;
    out[1] = (unsigned char)trump_id;
    // Header slot 2 is "the seat that made the OPENING attack" — the decoder
    // seeds its model with it and, on the first atom, offers attack options to
    // that seat alone (replay.c build_top_menu). g->first_attacker cannot be
    // used: it is set at the deal (lowest trump) but then REASSIGNED on every
    // bout transition (game.c), so by the time a finished game is encoded it
    // holds the LAST round's attacker. Whenever the game's last attacker was
    // not its first, the opening ATTACK log had no matching menu option and
    // encode died with REPLAY_ENOTINMENU on step 0 — ~50% of 2p and ~75% of 4p
    // finished games. Derive it from the log, as the other three encoders
    // already do (tests/replay_difftest.c, tests/replay_v6_test.c,
    // _shared/common/replay/encode.ts). NOTE: fio_state_json's read of
    // g->first_attacker is correct and must stay — a live view wants the
    // CURRENT round's attacker.
    int first_attacker = -1;
    for (int i = 0; i < g->num_logs && first_attacker < 0; i++)
        if (g->logs[i].log_type == LOG_ATTACK) first_attacker = g->logs[i].player_idx;
    if (first_attacker < 0) return -1;   // no attack logged → nothing to encode
    out[2] = (unsigned char)first_attacker;
    out[3] = (unsigned char)(n_actions & 0xff);
    out[4] = (unsigned char)((n_actions >> 8) & 0xff);
    return q;
}

// v5: no seed needed, and no exact hidden state — a decoder retrodicts the
// hands. Kept for games this process did not deal from a wide seed.
int fio_replay_encode_b32(char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    g_last_replay_error = 0;
    static unsigned char encin[65536];
    static unsigned char encout[16384];
    int inlen = build_encode_input(&g_game, encin, sizeof(encin));
    if (inlen < 0) { g_last_replay_error = REPLAY_EINPUT; return FIO_EREPLAY; }
    int enclen = replay_encode(encin, inlen, encout, sizeof(encout));
    if (enclen < 0) { g_last_replay_error = -enclen; return FIO_EREPLAY; }
    int w = b32_encode(encout, enclen, out, cap);
    if (w < 0) return FIO_ECAP;
    return w;
}

// v6: the exact game, hidden state and all. One kernel call — the deal seed was
// kept at fio_new_game, the actions are this game's own logs, and the reveal
// stream is re-derived inside the kernel, so the app assembles nothing. This is
// the same replay_encode_v6_from_game the server calls through
// wasm_replay_encode_v6_from_game; an offline share is now byte-identical in
// KIND to one the site produces, and the Oracle gets exact hands instead of
// v5's retrodiction.
//
// Returns FIO_ENOSEED for a game dealt without a wide seed (nothing to
// re-derive) — the caller should fall back to fio_replay_encode_b32.
int fio_replay_encode_v6_b32(char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (!g_has_deal_seed) return FIO_ENOSEED;
    g_last_replay_error = 0;
    static unsigned char encout[16384];
    int enclen = replay_encode_v6_from_game(&g_game, g_deal_seed, FOOLISH_SEED_LEN,
                                            1 << 30, encout, sizeof(encout));
    if (enclen < 0) { g_last_replay_error = -enclen; return FIO_EREPLAY; }
    int w = b32_encode(encout, enclen, out, cap);
    if (w < 0) return FIO_ECAP;
    return w;
}

// The best code this game can produce — v6 when its deal can be re-derived,
// else v5. WHICH FORMAT TO SHARE IS NOT AN APP DECISION: the server already
// makes exactly this choice (finalizeEndedGame prefers v6 and falls back to v5),
// and if it lived in Swift then the watch, the iMessage extension and every
// later client would each reimplement the same three lines and drift on them.
// The explicit fio_replay_encode_b32 / _v6_b32 stay for callers that must pin
// one format (tests, fixtures).
int fio_replay_share_code_b32(char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (g_has_deal_seed) {
        int w = fio_replay_encode_v6_b32(out, cap);
        if (w >= 0) return w;
        // v6 could not be built for this game (see fio_last_replay_error): a
        // share still beats no share, and v5 encodes from the logs alone.
    }
    return fio_replay_encode_b32(out, cap);
}

// A replay, as the event stream the board already animates.
//
// This is the whole point of A5 for the app: a v6 code plays natively STEP FOR
// STEP because the kernel rebuilds the game and plays it back through the
// engine (replay_steps.c), so these are the same events — same shape, same
// per-step `state`, same redaction — that fio_bot_drive_json and
// fio_last_events_json emit for live play. Swift decodes them with the
// GameEvent it already has; there is no replay-specific rendering path to
// write, and nothing to keep in step with live play, because it IS live play.
//
// v6 only: a v5 code returns FIO_EREPLAY with REPLAY_EVERSION (v5 hides the
// deal, so there is no game to rebuild). Callers wanting the flat log listing
// have fio_replay_decode_packed.
int fio_replay_events_json(const char *code, int viewer, char *out, int cap) {
    if (!code) return FIO_EBADARG;
    g_last_replay_error = 0;

    static unsigned char intbuf[16384];
    int ilen = b32_decode(code, intbuf, sizeof(intbuf));
    if (ilen < 0) return FIO_ECAP;

    J j;
    j_init(&j, out, cap);
    j_putc(&j, '[');
    FioEvCtx ctx = { &j, 0, viewer };
    int r = replay_steps_v6(intbuf, ilen, viewer, 0, fio_ev_sink, &ctx);
    if (r < 0) { g_last_replay_error = -r; return FIO_EREPLAY; }
    j_putc(&j, ']');
    return j_finish(&j);
}

// The animations of the chain's LAST TURN, as packed evwire frames — the "what
// just happened" an iMessage receiver sees on opening a bubble. Same packed
// evwire the website renders and live play broadcasts (no JSON crosses this
// boundary, §zero-JSON); Swift reads it with EvWire.decodeFrames.
//
// THE KERNEL decides the group; the client passes only the encoded chain, never
// "where I last looked" (there was no server to emit events at move time, and
// the boundary is a rules question, so it stays in C).
//
// A turn is not an action. This used to hand back the final step alone, on the
// reasoning that a v6 replay is the deal (step 0) then exactly one step per
// action, and that each step already bundles an action with ALL its
// kernel-internal consequences — a `pickup` step carries the PICKUP, every
// seat's refill draws, and the defender change, from the one handle_pickup
// call. All true, and still one action short of what a BUBBLE carries: a
// player stages as many actions as they like before sending, so a defender who
// covers two attacks sends one bubble holding two cover steps. Replaying only
// the last of them showed the first cover already sitting on the table, landed
// and rotated, while the second flew in — "if it's a double cover, the first
// cover will just already be there, and only the second one will play."
//
// The group is therefore the trailing run of steps by ONE acting seat: walk
// back from the end over the seatless tail (ROUND_END belongs to whoever caused
// it), then back over every immediately preceding step by that same seat.
//
// KNOWN LIMIT, accepted deliberately (owner's call). That run is the last
// BUBBLE only when the sender staged its actions together and sent once. A
// player who covers, sends, covers, sends, covers, sends puts three cover steps
// on the chain that are indistinguishable from three staged at once — so
// opening the third bubble replays all three. Nothing in the payload can tell
// them apart: the envelope carries `turn` (total actions) and `parent8` (the
// parent's DIGEST), but not the parent's turn, so a receiver cannot subtract.
//
// Two ways out were considered:
//
//   1. Diff against the previous chain this device held. Rejected: it makes the
//      animation a property of one device's CACHE rather than of the bubble, so
//      a wiped store, a reinstall or a new phone silently changes what replays.
//   2. Bump the FMSG format and add a u8 "actions in this bubble", written at
//      seal time from the pending count. This is the correct fix and the one to
//      reach for if the case ever stops being rare — it is exact, it is a
//      property of the bubble, and it costs one byte. It was not done now only
//      because it invalidates every format-2 payload already in a thread, which
//      is a poor trade against how seldom anyone sends one cover at a time.
//
// So this stays a heuristic on purpose, and it is the RIGHT one for the common
// case (a staged double cover, which used to replay only its last cover).
//
// Every frame is masked for `viewer` exactly like live play: the viewer's own
// drawn/picked-up cards carry real identities (fixing "my own refill never
// animated on reopen"), everyone else's are hidden backs.
//
// Frames come back in the shape replay_steps_frames_v6 writes them — each
// preceded by a u16 LE length, in play order. v6 only. Returns bytes written
// (0 if the turn produced nothing to animate), or a negative error.
int fio_replay_last_events_packed(const char *code, int viewer,
                                  unsigned char *out, int cap) {
    if (!code || !out) return FIO_EBADARG;
    g_last_replay_error = 0;

    static unsigned char intbuf[16384];
    int ilen = b32_decode(code, intbuf, sizeof(intbuf));
    if (ilen < 0) return FIO_ECAP;

    // What each step IS (kind + acting seat), so the run can be found without
    // decoding a single frame first. The ceiling is the same one `intbuf`
    // already implies — a chain that decodes to more actions than this could
    // not have fit in the 16KB buffer above in the first place — and
    // replay_steps_index_v6 returns -REPLAY_ECAP rather than truncating.
    #define FIO_MAX_REPLAY_STEPS 2048
    static unsigned char idx[RS_INDEX_STRIDE * FIO_MAX_REPLAY_STEPS];
    int ilen_idx = replay_steps_index_v6(intbuf, ilen, 0, idx, sizeof idx);
    if (ilen_idx < 0) { g_last_replay_error = -ilen_idx; return FIO_EREPLAY; }
    const int n = ilen_idx / RS_INDEX_STRIDE;
    if (n <= 0) return 0;

    const int last = n - 1;
    int from = last;
    // Back over the seatless tail (ROUND_END) to the acting step that caused it.
    int a = last;
    while (a > 0 && idx[a * RS_INDEX_STRIDE + 1] == RS_SEAT_NONE) a--;
    const unsigned char actor = idx[a * RS_INDEX_STRIDE + 1];
    if (actor != RS_SEAT_NONE) {
        from = a;
        // …then back over every step that seat played immediately before it.
        // Never across another seatless step: that is a closed bout, and the
        // run on its far side is a different turn.
        while (from > 1 && idx[(from - 1) * RS_INDEX_STRIDE + 1] == actor) from--;
    }

    // The group runs to the end of the stream, so asking for [from, ...) is
    // exactly it. Length-prefixed frames, in play order, as written.
    int n_frames = 0, next_step = 0;
    int r = replay_steps_frames_v6(intbuf, ilen, viewer, from, 0,
                                   out, cap, &n_frames, &next_step);
    if (r < 0) { g_last_replay_error = -r; return FIO_EREPLAY; }
    if (n_frames <= 0) return 0;                   // nothing to animate
    // A short buffer must not silently drop the END of the turn — the newest
    // action is the one the viewer most needs to see. FIO_ECAP, not
    // FIO_EREPLAY: a turn of several frames (each carrying a masked board) can
    // outgrow the caller's first guess, and ECAP is the code that makes the
    // Swift side retry with a bigger buffer instead of giving up on the
    // animation. A real decode failure is still FIO_EREPLAY above.
    if (next_step <= last) { g_last_replay_error = REPLAY_ECAP; return FIO_ECAP; }
    return r;
}

// The replay decode as its RAW binary (replay.h DECODE layout: a 20-byte header
// then n_logs records of [type,seat,defIdx,n_pairs] + n_pairs*[primary,target]
// wire-card bytes). Swift parses this directly (DecodedReplay.decode) — the same
// bytes fio_replay_decode_json used to walk into JSON, now handed over whole so
// no JSON crosses the boundary. Card bytes: 0xFF none, 0xFE hidden, else id.
int fio_replay_decode_packed(const char *code, unsigned char *out, int cap) {
    if (!code) return FIO_EBADARG;
    g_last_replay_error = 0;
    static unsigned char intbuf[16384];
    int ilen = b32_decode(code, intbuf, sizeof(intbuf));
    if (ilen < 0) return FIO_ECAP;
    int dlen = replay_decode(intbuf, ilen, out, cap);
    if (dlen < 0) { g_last_replay_error = -dlen; return FIO_EREPLAY; }
    if (dlen < REPLAY_DEC_HDR) { g_last_replay_error = REPLAY_EINPUT; return FIO_EREPLAY; }
    return dlen;
}

int fio_last_replay_error(void) { return g_last_replay_error; }

// ---------- FMSG: the iMessage envelope (src/msg_wire.h) -------------------
//
// The phone's door onto the SAME envelope the web reads. Nothing here decides
// anything: decode/seal/Rule P/rebase are all msg_wire.c, and this file only
// marshals. That is what makes a phone and a browser agree on a payload by
// construction rather than by two implementations staying in step —
// e2e/msg_wire.test.ts pins the wasm side against fixtures the NATIVE kernel
// sealed, and ios_api_smoke drives these against the same bytes.

static int g_last_msg_error = 0;
static int g_msg_round = -1;      // the adopted chain's round — Rule R's guard input

int fio_last_msg_error(void) { return g_last_msg_error; }

// The FMSG envelope decode+adopt hands the metadata back as a PACKED
// fixed-layout blob (Swift parses it with MessageEnvelope.decode) — no JSON, and
// no embedded state/moves (the phone
// reads those through fio_state_packed / fio_legal_packed in the same actor).
// Layout: phase(1) n_players(1) last_actor_seat(1) round(1) turn(u16 LE)
//   game_id(u64 LE) parent8(8) digest(32) sent_at(u16 LE) n_joins(1)
//   then n_joins * { seat(1) name_len(1) name[name_len] }.
// ROUND 16: sent_at is the envelope's send clock (unix seconds mod 65536, 0 on
// a format-2 chain that carries none). It sits at the END of the fixed header,
// after the digest, so every offset the Swift parser already knew is unchanged
// and only n_joins moves - this blob is a private ABI between two files in one
// repo, but keeping the prefix stable is what makes the diff readable.
// 1.0(6) DIAGNOSTIC: the replay codec version (5/6/7) of the body the last
// fio_msg_decode_packed replayed, or -1 for an empty-body message. Set through
// msg_last_body_version (msg_wire.c).
int fio_msg_last_body_version(void) { return msg_last_body_version; }

int fio_msg_decode_packed(const uint8_t *payload, int len, unsigned char *out, int cap) {
    if (!payload || !out || cap <= 0) return FIO_EBADARG;
    g_last_msg_error = 0;
    msg_last_body_version = -1;   // 1.0(6) diagnostic reset

    MsgEnvelope e;
    int rc = msg_decode(payload, len, &e);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    // Digest BEFORE anything reuses the bytes — it is the child's parent8 and
    // Rule P's tiebreak.
    uint8_t digest[SHA256_DIGEST_LEN];
    msg_digest(payload, len, digest);

    rc = msg_replay(&e, &g_game);   // validation IS replay
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    g_has_game = 1;
    g_msg_round = e.round;
    memcpy(g_deal_seed, e.seed, FOOLISH_SEED_LEN);
    g_has_deal_seed = 1;
    for (int i = 0; i < e.n_players; i++) g_seat_roster[i] = (int8_t)bot_roster_find("random");

    int need = 4 + 2 + 8 + MSG_PARENT_LEN + SHA256_DIGEST_LEN + 2 + 1;
    for (int i = 0; i < e.n_joins; i++) need += 2 + e.joins[i].name_len;
    if (cap < need) return FIO_ECAP;

    unsigned char *q = out;
    *q++ = e.phase;
    *q++ = e.n_players;
    *q++ = e.last_actor_seat;
    *q++ = e.round;
    *q++ = (unsigned char)(e.turn & 0xff);
    *q++ = (unsigned char)((e.turn >> 8) & 0xff);
    for (int i = 0; i < 8; i++) *q++ = (unsigned char)((e.game_id >> (8 * i)) & 0xff);
    memcpy(q, e.parent8, MSG_PARENT_LEN); q += MSG_PARENT_LEN;
    memcpy(q, digest, SHA256_DIGEST_LEN); q += SHA256_DIGEST_LEN;
    *q++ = (unsigned char)(e.sent_at & 0xff);
    *q++ = (unsigned char)((e.sent_at >> 8) & 0xff);
    *q++ = (unsigned char)e.n_joins;
    for (int i = 0; i < e.n_joins; i++) {
        *q++ = e.joins[i].seat;
        *q++ = e.joins[i].name_len;
        memcpy(q, e.joins[i].name, e.joins[i].name_len); q += e.joins[i].name_len;
    }
    return (int)(q - out);
}

// joins: [{"seat":0,"name":"Sveta"},...] → e->joins / e->n_joins. Shared by the
// action seal (fio_msg_encode) and the empty-body lobby seal
// (fio_msg_encode_waiting). Returns FIO_EOK or FIO_EPARSE.
static int fio_parse_joins(const char *joins_json, MsgEnvelope *e) {
    const char *p = joins_json;
    while (*p && *p != '[') p++;
    if (*p != '[') return FIO_EPARSE;
    p++;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        if (*p != '{') return FIO_EPARSE;
        if (e->n_joins >= MSG_MAX_JOINS) return FIO_EPARSE;
        const char *sp = find_key(p, "seat");
        const char *np = find_key(p, "name");
        if (!sp || !np || *np != '"') return FIO_EPARSE;
        MsgJoin *jn = &e->joins[e->n_joins];
        jn->seat = (uint8_t)atoi(sp);
        np++;
        int n = 0;
        while (*np && *np != '"' && n < MSG_MAX_NAME) jn->name[n++] = *np++;
        if (*np != '"') return FIO_EPARSE;   // > MSG_MAX_NAME bytes, or unterminated
        jn->name_len = (uint8_t)n;
        e->n_joins++;
        const char *close = strchr(p, '}');
        if (!close) return FIO_EPARSE;
        p = close + 1;
    }
    return FIO_EOK;
}

int fio_msg_encode(int phase, int last_actor_seat, uint64_t game_id,
                   const uint8_t parent8[8], const char *joins_json,
                   int sent_at, uint8_t *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (!out || cap <= 0 || !joins_json) return FIO_EBADARG;
    if (!g_has_deal_seed) return FIO_ENOSEED;   // no seed, no serverless game
    g_last_msg_error = 0;

    MsgEnvelope e;
    memset(&e, 0, sizeof(e));
    e.format = MSG_FORMAT_V6;
    e.flags = 0;
    e.phase = (uint8_t)phase;
    e.game_id = game_id;
    e.last_actor_seat = (uint8_t)last_actor_seat;
    e.n_players = (uint8_t)g_game.num_players;
    e.variant = 0;
    // ROUND 16: the caller's clock, unix seconds mod 65536, or 0 for "do not
    // stamp this one" - which seals a format-2 envelope exactly as before. The
    // time is passed IN rather than read here on purpose: a kernel that called
    // time() would answer differently on two devices holding the same bytes.
    e.sent_at = (uint16_t)(sent_at & 0xffff);
    if (parent8) memcpy(e.parent8, parent8, MSG_PARENT_LEN);
    memcpy(e.seed, g_deal_seed, FOOLISH_SEED_LEN);

    const int jrc = fio_parse_joins(joins_json, &e);
    if (jrc != FIO_EOK) return jrc;

    static unsigned char body[1024];   // a v6 body measures ~68 B at 8 players
    static Game scratch;
    const int rc = msg_seal(&e, &g_game, body, (int)sizeof body, &scratch);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    const int n = msg_encode(&e, out, cap);
    if (n < 0) { g_last_msg_error = n; return n == MSG_ECAP ? FIO_ECAP : FIO_EMSG; }
    return n;
}


// ROUND 16 — the pickup hold, on the resident game. Pure relay: the rule is
// msg_pickup_hold_remaining (msg_wire.c) and this only supplies the game.
int fio_msg_pickup_hold(int seat, int sent_at, int now) {
    if (!g_has_game) return FIO_ENOGAME;
    return msg_pickup_hold_remaining(&g_game, seat,
                                     (uint16_t)(sent_at & 0xffff),
                                     (uint16_t)(now & 0xffff));
}

int fio_msg_rule_p(const uint8_t *a, int a_len, const uint8_t *b, int b_len) {
    if (!a || !b) return FIO_EBADARG;
    g_last_msg_error = 0;
    MsgChainKey ka, kb;
    int rc = msg_chain_key(a, a_len, &ka);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    rc = msg_chain_key(b, b_len, &kb);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    return msg_rule_p(&ka, &kb);
}

int fio_msg_rebase(int pending_round, int seat, const char *move_json) {
    if (!g_has_game) return FIO_ENOGAME;
    if (!move_json) return FIO_EBADARG;
    if (g_msg_round < 0) return FIO_ENOGAME;   // nothing adopted to rebase onto
    if (seat < 0 || seat >= g_game.num_players) return FIO_EBADARG;

    AwireAction a;
    const int rc = fio_move_to_awire(move_json, &a);
    if (rc != FIO_EOK) return rc;
    return msg_rebase_one(&g_game, g_msg_round, pending_round, seat, &a);
}

// Rule R over the AWIRE frame — the JSON-free twin of fio_msg_rebase, and the one
// Swift actually calls (the phone stages moves as awire, never JSON; the pending
// ledger holds them the same way). Same contract as wasm_msg_rebase: decode the
// action, then msg_rebase_one against the adopted chain's round (g_msg_round, set
// by the last fio_msg_decode_packed). Returns MSG_REBASE_* (0 re-applied and
// APPLIED to the resident game, 1 discarded by the round guard, 2 discarded as
// illegal), or a negative MSG_E*.
int fio_msg_rebase_awire(int pending_round, int seat, const uint8_t *buf, int len) {
    if (!g_has_game) return FIO_ENOGAME;
    if (!buf || len <= 0) return FIO_EBADARG;
    if (g_msg_round < 0) return FIO_ENOGAME;   // nothing adopted to rebase onto
    if (seat < 0 || seat >= g_game.num_players) return FIO_EBADARG;

    AwireAction a;
    if (!awire_decode(buf, len, &a)) return FIO_EPARSE;
    return msg_rebase_one(&g_game, g_msg_round, pending_round, seat, &a);
}

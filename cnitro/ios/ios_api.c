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
#include "strategy.h"
#include "bot_roster.h"
#include "bot_drive.h"
#include "evwire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// ---------- a tiny JSON string builder ------------------------------------
//
// Bounded append helpers. `w` tracks bytes written; every append checks the cap
// and, on overflow, sets *ok=0 so the caller can return FIO_ECAP. The buffer is
// always NUL-terminated when there is room.

typedef struct { char *buf; int cap; int w; int ok; } J;

static void j_init(J *j, char *buf, int cap) { j->buf = buf; j->cap = cap; j->w = 0; j->ok = (cap > 0); }
static void j_putc(J *j, char c) {
    if (!j->ok) return;
    if (j->w + 1 >= j->cap) { j->ok = 0; return; }
    j->buf[j->w++] = c;
}
static void j_puts(J *j, const char *s) { while (*s) j_putc(j, *s++); }
static void j_puti(J *j, long v) {
    char t[24]; int n = snprintf(t, sizeof(t), "%ld", v);
    for (int i = 0; i < n; i++) j_putc(j, t[i]);
}
// Emit a JSON escaped string (names may contain quotes/backslashes/control).
static void j_putstr(J *j, const char *s) {
    j_putc(j, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { j_putc(j, '\\'); j_putc(j, (char)c); }
        else if (c == '\n') { j_putc(j, '\\'); j_putc(j, 'n'); }
        else if (c == '\t') { j_putc(j, '\\'); j_putc(j, 't'); }
        else if (c < 0x20)  { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); j_puts(j, t); }
        else j_putc(j, (char)c);
    }
    j_putc(j, '"');
}
// {"s":<suit>,"v":<value>}  — the wire-independent Card JSON (§16.A2).
static void j_card(J *j, Card c) {
    j_puts(j, "{\"s\":"); j_puti(j, c.suit);
    j_puts(j, ",\"v\":"); j_puti(j, c.value); j_putc(j, '}');
}
static int j_finish(J *j) {
    if (!j->ok) return FIO_ECAP;
    j->buf[j->w] = 0;
    return j->w;
}

// ---------- state serialization -------------------------------------------
//
// Per-viewer masked view. viewer >= 0 shows only that seat's real hand; every
// other hand collapses to a count; the deck and flip-under-deck stay hidden.
// viewer == VIEW_SPECTATOR masks every hand. This is the "you only see your own
// hand" rule (view.h) rendered as JSON rather than the packed blob.
//
// Written into an EXISTING writer so a state object can nest inside a larger
// document — the event stream carries one per step (j_events). emit_state_of
// below is the standalone-buffer form.
static void j_state(J *j, const Game *g, int viewer) {
    j_puts(j, "{\"status\":");        j_puti(j, g->status);
    j_puts(j, ",\"numPlayers\":");    j_puti(j, g->num_players);
    j_puts(j, ",\"powerSuit\":");     j_puti(j, g->power_suit);
    j_puts(j, ",\"deckCount\":");     j_puti(j, g->deck_count);
    j_puts(j, ",\"discardCount\":");  j_puti(j, g->discard_pile_length);
    j_puts(j, ",\"hasFlipped\":");    j_puts(j, g->has_flipped ? "true" : "false");
    j_puts(j, ",\"firstAttacker\":"); j_puti(j, g->first_attacker);
    j_puts(j, ",\"defender\":");      j_puti(j, g->defender);
    j_puts(j, ",\"viewer\":");        j_puti(j, viewer);
    j_puts(j, ",\"goodMask\":");      j_puti(j, (long)g->good_players_mask);
    j_puts(j, ",\"gameOver\":");      j_puti(j, game_done(g));

    // The flipped trump card is public whenever it is still known (has_flipped).
    j_puts(j, ",\"flipped\":");
    if (g->has_flipped) j_card(j, g->flipped); else j_puts(j, "null");

    // Battles: attack always real; defense null when uncovered.
    j_puts(j, ",\"battles\":[");
    for (int i = 0; i < g->num_battles; i++) {
        if (i) j_putc(j, ',');
        const Battle *b = &g->table_battles[i];
        j_puts(j, "{\"attack\":"); j_card(j, b->attack);
        j_puts(j, ",\"defense\":");
        if (card_is_none(b->defense)) j_puts(j, "null"); else j_card(j, b->defense);
        j_putc(j, '}');
    }
    j_putc(j, ']');

    // Elimination order (seat indices, in the order they cleared).
    j_puts(j, ",\"eliminationOrder\":[");
    for (int i = 0; i < g->num_eliminated; i++) { if (i) j_putc(j, ','); j_puti(j, g->elimination_order[i]); }
    j_putc(j, ']');

    // Players. Every seat exposes seat/name/status/handCount/awaitingAttack/
    // strategyKey; ONLY the viewer seat exposes its real `hand`. Others emit
    // "hand":null so Swift renders card backs from handCount.
    j_puts(j, ",\"players\":[");
    for (int p = 0; p < g->num_players; p++) {
        if (p) j_putc(j, ',');
        const Player *pl = &g->players[p];
        int is_viewer = (viewer == p);
        j_puts(j, "{\"seat\":");         j_puti(j, p);
        j_puts(j, ",\"name\":");         j_putstr(j, pl->name);
        j_puts(j, ",\"status\":");       j_puti(j, pl->status);
        j_puts(j, ",\"handCount\":");    j_puti(j, pl->hand_count);
        // awaiting_attack is private turn state — only surface it for the viewer.
        j_puts(j, ",\"awaitingAttack\":");
        j_puts(j, (is_viewer && pl->awaiting_attack) ? "true" : "false");
        j_puts(j, ",\"strategyKey\":");  j_puti(j, pl->strategy_key);
        j_puts(j, ",\"hand\":");
        if (is_viewer) {
            j_putc(j, '[');
            for (int c = 0; c < pl->hand_count; c++) { if (c) j_putc(j, ','); j_card(j, pl->hand[c]); }
            j_putc(j, ']');
        } else {
            j_puts(j, "null");
        }
        j_putc(j, '}');
    }
    j_putc(j, ']');
    j_putc(j, '}');
}

// The standalone-buffer form: one masked state as a whole JSON document.
static int emit_state_of(const Game *g, int viewer, char *out, int cap) {
    J j; j_init(&j, out, cap);
    j_state(&j, g, viewer);
    return j_finish(&j);
}

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
    if (ev->snap) { j_puts(c->j, ",\"state\":"); j_state(c->j, ev->snap, c->viewer); }
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

int fio_apply_json(int actor_seat, const char *move_json) {
    if (!g_has_game) return FIO_ENOGAME;
    if (!move_json) return FIO_EBADARG;
    if (actor_seat < 0 || actor_seat >= g_game.num_players) return FIO_EBADARG;

    const char *tp = find_key(move_json, "type");
    if (!tp || *tp != '"') return FIO_EPARSE;
    tp++;
    char type[16] = {0};
    int ti = 0;
    while (*tp && *tp != '"' && ti < (int)sizeof(type) - 1) type[ti++] = *tp++;

    Card cards[MAX_MOVE_CARDS];
    Card acards[MAX_MOVE_CARDS];
    int nc = 0, nac = 0;

    const char *cp = find_key(move_json, "cards");
    if (cp) { nc = parse_card_array(cp, cards, MAX_MOVE_CARDS); if (nc < 0) return FIO_EPARSE; }
    const char *ap = find_key(move_json, "attackCards");
    if (ap) { nac = parse_card_array(ap, acards, MAX_MOVE_CARDS); if (nac < 0) return FIO_EPARSE; }

    // Capture this move's animation events too — a human's card flies exactly
    // like a bot's, and both plans come from the kernel (§4.4).
    fio_snaps_reset();
    g_last_event_log_start = g_game.num_logs;
    engine_snap_hook = fio_snap_cb;

    engine_last_reject = ENGINE_REJECT_NONE;
    bool ok = false;
    if      (!strcmp(type, "attack")) ok = handle_attack(&g_game, actor_seat, cards, nc);
    else if (!strcmp(type, "pass"))   ok = handle_pass(&g_game, actor_seat, cards, nc);
    else if (!strcmp(type, "pickup")) ok = handle_pickup(&g_game, actor_seat);
    else if (!strcmp(type, "good"))   ok = handle_good(&g_game, actor_seat);
    else if (!strcmp(type, "cover")) {
        engine_snap_hook = 0;
        if (nac != nc) return FIO_EPARSE;
        engine_snap_hook = fio_snap_cb;
        ok = handle_cover(&g_game, actor_seat, cards, acards, nc);
    } else { engine_snap_hook = 0; return FIO_EPARSE; }

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
    g_game.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g_game.players[i].status = PLAYER_STATUS_READY;
        g_game.players[i].strategy_key = 0; // all human until fio_set_seat_strategy
        g_seat_roster[i] = (int8_t)bot_roster_find("random");
        snprintf(g_game.players[i].player_id, sizeof(g_game.players[i].player_id), "p%d", i);
    }
    start_game(&g_game);
    g_has_game = 1;
    g_last_reject = 0;
    return FIO_EOK;
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

int fio_state_json(int viewer_seat, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (viewer_seat < 0 || viewer_seat >= g_game.num_players) return FIO_EBADARG;
    return emit_state_of(&g_game, viewer_seat, out, cap);
}
int fio_public_state_json(char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    return emit_state_of(&g_game, VIEW_SPECTATOR, out, cap);
}

// Decode a SERVER packed-view blob (the player_views / spectator_views wire, the
// same state_put/state_get layout view.h single-sources) into the app's GameView
// JSON — so online play renders through the SAME kernel decode as offline, never
// a reimplemented wire in Swift (§8, §16.D4). `viewer` is the seat whose hand is
// real in this blob (the local player's seat), or VIEW_SPECTATOR for the public
// feed. Does not touch the current game.
int fio_view_from_packed_json(const uint8_t *buf, int len, int viewer, char *out, int cap) {
    if (!buf || len <= 0) return FIO_EBADARG;
    static Game tmp;
    memset(&tmp, 0, sizeof(tmp));
    state_get(&tmp, buf, /*masked=*/1);
    if (tmp.num_players < 2 || tmp.num_players > MAX_PLAYERS) return FIO_EPARSE;
    if (viewer != VIEW_SPECTATOR && (viewer < 0 || viewer >= tmp.num_players)) return FIO_EBADARG;
    return emit_state_of(&tmp, viewer, out, cap);
}

// Legal moves for `seat` computed from a SERVER packed masked-view blob, so
// online enable-states are kernel-driven like offline (§3). My own hand and the
// table are real in my masked view — all that MY legal moves depend on — so the
// kernel enumerates them correctly even though other seats are counts-only.
int fio_legal_from_packed_json(const uint8_t *buf, int len, int seat, char *out, int cap) {
    if (!buf || len <= 0) return FIO_EBADARG;
    static Game tmp;
    memset(&tmp, 0, sizeof(tmp));
    state_get(&tmp, buf, /*masked=*/1);
    if (tmp.num_players < 2 || tmp.num_players > MAX_PLAYERS) return FIO_EPARSE;
    if (seat < 0 || seat >= tmp.num_players) return FIO_EBADARG;
    return emit_legal_of(&tmp, seat, out, cap);
}

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

// Drive one eligible seat other than `human_seat`. Choosing + applying happen
// here so the Swift bot loop is a simple "call until it returns 0". The seat's
// own strategy_key selects the chooser (dispatch_choose). human_seat == -1
// drives any seat (replays/spectate playthroughs).
int fio_bot_step_json(int human_seat, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (game_done(&g_game) >= 0) return 0;

    for (int seat = 0; seat < g_game.num_players; seat++) {
        if (seat == human_seat) continue;
        if (!should_bot_act(&g_game, seat)) continue;

        LegalMoves moves;
        calculate_legal_moves(&g_game, seat, &moves);
        if (moves.n == 0) continue;

        int idx = bot_roster_choose(g_seat_roster[seat], &g_game, seat, &moves);
        if (idx < 0 || idx >= moves.n) idx = 0; // never freeze: fall back to first legal move
        const LegalMove *m = &moves.moves[idx];

        engine_last_reject = ENGINE_REJECT_NONE;
        bool ok = false;
        switch (m->type) {
            case MOVE_ATTACK: ok = handle_attack(&g_game, seat, m->cards, m->n_cards); break;
            case MOVE_COVER:  ok = handle_cover (&g_game, seat, m->cards, m->attack_cards, m->n_cards); break;
            case MOVE_PASS:   ok = handle_pass  (&g_game, seat, m->cards, m->n_cards); break;
            case MOVE_PICKUP: ok = handle_pickup(&g_game, seat); break;
            case MOVE_GOOD:   ok = handle_good  (&g_game, seat); break;
            default: break;
        }
        if (!ok) continue;

        J j; j_init(&j, out, cap);
        j_puts(&j, "{\"seat\":"); j_puti(&j, seat); j_putc(&j, ',');
        // splice the move object's fields in after "seat"
        // (emit_move_obj writes a full {..}; we re-open it inline instead)
        j_puts(&j, "\"type\":"); j_putstr(&j, move_type_name(m->type));
        j_puts(&j, ",\"cards\":[");
        for (int c = 0; c < m->n_cards; c++) { if (c) j_putc(&j, ','); j_card(&j, m->cards[c]); }
        j_putc(&j, ']');
        if (m->type == MOVE_COVER) {
            j_puts(&j, ",\"attackCards\":[");
            for (int c = 0; c < m->n_cards; c++) { if (c) j_putc(&j, ','); j_card(&j, m->attack_cards[c]); }
            j_putc(&j, ']');
        }
        j_putc(&j, '}');
        return j_finish(&j);
    }
    return 0; // no eligible bot seat
}

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

// ---------- strategies -----------------------------------------------------

int fio_strategy_count(void) { return bot_roster_offline_count(); }

int fio_strategy_name(int id, char *out, int cap) {
    const BotRosterEntry *e = bot_roster_at(fio_roster_idx(id));
    if (!e) return FIO_ENOSTRAT;
    J j; j_init(&j, out, cap);
    j_puts(&j, e->key);
    return j_finish(&j);
}

int fio_bot_choose_json(int strategy_id, int seat, char *out, int cap) {
    if (!g_has_game) return FIO_ENOGAME;
    if (seat < 0 || seat >= g_game.num_players) return FIO_EBADARG;
    int ridx = fio_roster_idx(strategy_id);
    if (!bot_roster_at(ridx)) return FIO_ENOSTRAT;
    LegalMoves moves;
    calculate_legal_moves(&g_game, seat, &moves);
    if (moves.n == 0) { J j; j_init(&j, out, cap); j_puts(&j, "null"); return j_finish(&j); }
    int idx = bot_roster_choose(ridx, &g_game, seat, &moves);
    if (idx < 0 || idx >= moves.n) idx = 0;
    J j; j_init(&j, out, cap);
    emit_move_obj(&j, &moves.moves[idx]);
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
static void j_wire_card(J *j, unsigned char b) {
    if (b == REPLAY_CARD_NONE) { j_puts(j, "null"); return; }
    if (b == REPLAY_CARD_HIDDEN) { j_puts(j, "{\"s\":-1,\"v\":-1}"); return; }
    int v = b > 51 ? 51 : b;
    Card c; c.suit = (int8_t)(v / 13); c.value = (int8_t)((v % 13) + 1);
    j_card(j, c);
}

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
    // _shared/replay/encode.ts). NOTE: fio_state_json's read of
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

int fio_replay_decode_json(const char *code, char *out, int cap) {
    if (!code) return FIO_EBADARG;
    g_last_replay_error = 0;

    static unsigned char intbuf[16384];   // the replay integer bytes
    static unsigned char dec[262144];      // replay_decode output (header + logs)

    int ilen = b32_decode(code, intbuf, sizeof(intbuf));
    if (ilen < 0) return FIO_ECAP;

    int dlen = replay_decode(intbuf, ilen, dec, sizeof(dec));
    if (dlen < 0) { g_last_replay_error = -dlen; return FIO_EREPLAY; }
    if (dlen < REPLAY_DEC_HDR) { g_last_replay_error = REPLAY_EINPUT; return FIO_EREPLAY; }

    // Parse the decoded binary (layout: replay.h DECODE output) into JSON.
    const unsigned char *p = dec;
    int version = p[0], n = p[1], trump = p[2], firstAtt = p[3], fool = p[4];
    int discard = p[5] | (p[6] << 8);
    int n_elim = p[7];
    const unsigned char *elim = &p[8];
    uint32_t n_logs = (uint32_t)p[16] | ((uint32_t)p[17] << 8)
                    | ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24);

    J j; j_init(&j, out, cap);
    j_puts(&j, "{\"version\":");       j_puti(&j, version);
    j_puts(&j, ",\"nPlayers\":");      j_puti(&j, n);
    j_puts(&j, ",\"trump\":");         j_wire_card(&j, (unsigned char)trump);
    j_puts(&j, ",\"firstAttacker\":"); j_puti(&j, firstAtt);
    j_puts(&j, ",\"fool\":");          j_puti(&j, fool == 0xFF ? -1 : fool);
    j_puts(&j, ",\"discardCount\":");  j_puti(&j, discard);
    j_puts(&j, ",\"eliminationOrder\":[");
    for (int i = 0, first = 1; i < n_elim && i < 8; i++) {
        if (elim[i] == 0xFF) continue;
        if (!first) j_putc(&j, ','); first = 0;
        j_puti(&j, elim[i]);
    }
    j_putc(&j, ']');

    j_puts(&j, ",\"logs\":[");
    const unsigned char *q = &dec[REPLAY_DEC_HDR];
    const unsigned char *end = &dec[dlen];
    for (uint32_t li = 0; li < n_logs && q + 4 <= end; li++) {
        if (li) j_putc(&j, ',');
        int log_type = q[0], seat = q[1], defIdx = q[2], n_pairs = q[3];
        q += 4;
        j_puts(&j, "{\"type\":");         j_puti(&j, log_type);
        j_puts(&j, ",\"seat\":");         j_puti(&j, seat == 0xFF ? -1 : seat);
        j_puts(&j, ",\"defenderIndex\":");j_puti(&j, defIdx == 0xFF ? -1 : defIdx);
        j_puts(&j, ",\"pairs\":[");
        for (int pr = 0; pr < n_pairs && q + 2 <= end; pr++) {
            if (pr) j_putc(&j, ',');
            j_puts(&j, "{\"primary\":"); j_wire_card(&j, q[0]);
            j_puts(&j, ",\"target\":");  j_wire_card(&j, q[1]);
            j_putc(&j, '}');
            q += 2;
        }
        j_puts(&j, "]}");
    }
    j_puts(&j, "]}");
    return j_finish(&j);
}

int fio_last_replay_error(void) { return g_last_replay_error; }

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

// ROUND 16 - THE LOG MARK: g_game's log count at the moment the RESIDENT game
// was established from a chain (a decode), or from a fresh deal. Everything
// logged since is what this device is about to send, so `fio_msg_encode` hands
// the mark to msg_seal, which asks the encoder how many atoms of the body come
// after it - the bubble delta (msg_wire.h's n_new), which is how a receiver
// knows to animate this move and not the one before it as well.
//
// A LOG mark rather than the parent's atom count (which is what this was until
// the same round): the atom stream is re-derived from the whole log every time
// it is encoded, so a pending good stops being an atom the moment anything
// follows it, and two atom counts subtracted lose exactly those. The log only
// ever grows, so a mark into it is stable.
//
// -1 = unknown, and every path that makes a game resident without a chain to
// measure from must say so, because a stale base would name the WRONG suffix.
// It is a static for the same reason g_msg_round is: the fact belongs to the
// adopted chain, this file is the one place that adopts one, and asking Swift
// to carry it back down at seal time would put a rules input in app code (and
// in every other host's app code) for nothing.
static int g_msg_base_logs = -1;

// The send clock of the adopted chain (msg_wire.h's sent_at). A bubble that
// adds NOTHING repeats it rather than stamping now: the defender's pickup hold
// measures from when the attack was actually sent, and an undo-to-empty re-seal
// did not re-send that attack. Without this, cancelling a staged move handed
// every recipient a fresh 15 seconds of hold on a board nobody touched.
static uint16_t g_msg_base_sent_at = 0;

// THE FOOL'S PENALTY, on the resident game: the seat this game OPENED on, or
// MSG_NO_OPENING for the ordinary lowest-trump derivation. It belongs to the
// resident game exactly as `g_deal_seed` does - it is a term of the deal, not
// of any one bubble - so it is established once (by the Start that resolved it,
// or by decoding a chain that carries it) and then repeated by every seal of
// that game without Swift having to carry it down each time. A fresh deal
// clears it: an ordinary new game punishes nobody.
static uint8_t g_msg_opening = MSG_NO_OPENING;

// The other half, and the LOBBY's half: the rematch carry a WAITING envelope
// hands forward until someone taps Start. Sticky for the same reason - every
// join re-seals the lobby, and the question must survive each re-seal - and
// cleared by the deal that answers it (fio_new_game), so a live game never
// carries a lobby's question alongside its own answer.
static uint32_t g_msg_carry_key  = 0;
static uint8_t  g_msg_carry_fool = MSG_NO_FOOL;

// THE RULES THE RESIDENT GAME IS PLAYED UNDER, kept beside the seed for the same
// reason: a re-deal (fio_reseat_game, the rematch Start) rebuilds the Game from
// the locked seed and would otherwise drop them, and the lobby that CHOSE them
// is several bubbles back by then. Set by decoding a chain (which states them),
// or by fio_set_passing before a lobby's first seal; repeated by every seal of
// that game. A fresh deal restores the default, which is the classic passing
// game (game.h GAME_RULE_NO_PASS is what a variant costs, not what a default
// does).
static int8_t g_msg_rules = 0;

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
    // GOOD as always - it is how a seat leaves the eligible set, see legal.c).
    // The owner's "a human only gets Good once the table is fully covered, and
    // it disappears when someone throws in" rule is a narrowing applied on the
    // way to a board (play_can_say_good / play_human_menu), and the "your move
    // with no live button" status case is handled in the board's status logic,
    // not by rewriting the kernel's legal set.
    const int n = legal_menu_write(&lm, 0, -1, (unsigned char *)out, cap);
    return n == LEGAL_WIRE_ECAP ? FIO_ECAP : n;
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

// ---------- what a gesture on a board means (the board rules) ---------------
//
// The rules between a finger and a move - which menu entry a drop resolves to,
// which battles a selection could cover, which one the Cover button aims at,
// which moves a human may make at all. They live in legal.c (play_*); this is
// the crossing.
//
// THESE READ NOTHING BUT THEIR ARGUMENTS. Not the resident game, not a static -
// which is what makes them safe to call from a SwiftUI render pass, where the
// resident game is behind an actor and unreachable. What a board asks about is
// its own PUBLISHED pair anyway: the menu it was handed and the table it was
// handed, which for the iMessage board is sometimes deliberately not the live
// position (an empty menu while a bout settlement is held back). See the note
// on PlayBoard in legal.h.

// Fill a PlayBoard from the crossing arguments. `table` is 2 bytes per battle,
// the attack then its cover or LEGAL_WIRE_NONE.
static PlayBoard fio_play_board(const uint8_t *menu, int menu_len,
                                const uint8_t *table, int n_battles,
                                int power_suit, int is_defender) {
    PlayBoard b;
    b.menu = menu; b.menu_len = menu_len;
    b.table = table; b.n_battles = n_battles;
    b.power_suit = power_suit; b.is_defender = is_defender;
    return b;
}

// ONE ANSWER, so a board cannot paint a highlight that the release then
// refuses: the resolved move, the coverable set and the button states all come
// out of one walk of one menu. Layout (LE):
//
//   0   u8    flags: 1 = attack legal with this selection, 2 = pass legal,
//                    4 = this seat may say good
//   1   i8    the battle the Cover button aims at, -1 for none
//   2   u64   bitmask of battles this selection could cover
//   10  ...   the resolved move as a ONE-ENTRY menu wire (count 0 or 1), so
//             MoveWire decodes it with no second format
int fio_play_probe(const uint8_t *menu, int menu_len,
                   const uint8_t *table, int n_battles,
                   int power_suit, int is_defender,
                   const uint8_t *sel, int n_sel, int target,
                   char *out, int cap) {
    if (!menu || menu_len < 0 || n_battles < 0 || n_sel < 0) return FIO_EBADARG;
    if (cap < FIO_PLAY_PROBE_HEAD + 4) return FIO_ECAP;
    const PlayBoard b = fio_play_board(menu, menu_len, table, n_battles,
                                       power_suit, is_defender);

    const uint64_t mask = play_coverable_battles(&b, sel, n_sel);
    const int best = play_best_cover_target(&b, sel, n_sel);
    const int idx  = play_resolve(&b, sel, n_sel, target);

    unsigned char *q = (unsigned char *)out;
    q[0] = (unsigned char)((play_has_verb(&b, MOVE_ATTACK, sel, n_sel) ? 1 : 0)
                         | (play_has_verb(&b, MOVE_PASS,   sel, n_sel) ? 2 : 0)
                         | (play_can_say_good(&b)                      ? 4 : 0));
    q[1] = (unsigned char)(signed char)best;
    for (int i = 0; i < 8; i++) q[2 + i] = (unsigned char)((mask >> (8 * i)) & 0xff);

    // The move itself, copied straight off the menu it was found on.
    unsigned char *m = q + FIO_PLAY_PROBE_HEAD;
    m[0] = m[1] = m[2] = m[3] = 0;
    if (idx < 0) return FIO_PLAY_PROBE_HEAD + 4;

    MenuWalk w;
    MenuMove mm;
    if (legal_menu_begin(&w, menu, menu_len) < 0) return FIO_PLAY_PROBE_HEAD + 4;
    while (legal_menu_next(&w, &mm) == 1 && w.index != idx) { }
    if (w.index != idx) return FIO_PLAY_PROBE_HEAD + 4;
    if (FIO_PLAY_PROBE_HEAD + 4 + 2 + 2 * mm.n_cards > cap) return FIO_ECAP;
    m[0] = 1;
    m[4] = (unsigned char)mm.type;
    m[5] = (unsigned char)mm.n_cards;
    for (int i = 0; i < mm.n_cards; i++) m[6 + i] = mm.cards[i];
    for (int i = 0; i < mm.n_cards; i++) m[6 + mm.n_cards + i] = mm.attacks[i];
    return FIO_PLAY_PROBE_HEAD + 4 + 2 + 2 * mm.n_cards;
}

// The moves a HUMAN may make on this board, as the same menu wire in.
int fio_play_human_menu(const uint8_t *menu, int menu_len,
                        const uint8_t *table, int n_battles,
                        char *out, int cap) {
    if (!menu || menu_len < 0 || n_battles < 0) return FIO_EBADARG;
    const PlayBoard b = fio_play_board(menu, menu_len, table, n_battles, -1, 0);
    const int n = play_human_menu(&b, (unsigned char *)out, cap);
    if (n == LEGAL_WIRE_ECAP) return FIO_ECAP;
    if (n < 0) return FIO_EPARSE;
    return n;
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
// The layout is documented once, in ios_api.h. Like fio_beats_packed, this
// reads nothing but its arguments: the stream crosses WITH the question because
// a board animates the stream it was HANDED (a staged bout end is cut in half
// and the settlement withheld), and because a SwiftUI body cannot await the
// actor the resident game lives behind.

_Static_assert(FIO_PLAN_SEATS == MAX_PLAYERS,
               "the plan wire's seat block must be the kernel's table size");

int fio_anim_plan_packed(const uint8_t *in, int len, char *out, int cap) {
    if (!in || !out || len < 5) return FIO_EBADARG;
    if (in[0] != FIO_PLAN_VERSION) return FIO_EPARSE;
    const int np = in[1];
    const int n = in[2];
    if (np < 2 || np > MAX_PLAYERS) return FIO_EPARSE;
    if (n > ANIM_MAX_STEPS) return FIO_ECAP;

    int p = 5;
    if (p + np > len) return FIO_EPARSE;
    int final_hand[MAX_PLAYERS];
    for (int s = 0; s < np; s++) final_hand[s] = in[p++];

    static AnimPlanEvent evs[ANIM_MAX_STEPS];
    static int  hands[ANIM_MAX_STEPS][MAX_PLAYERS];
    static Card pool[ANIM_MAX_CARD_POOL];
    int n_pool = 0;
    for (int i = 0; i < n; i++) {
        // The WHOLE event is bounded before any of it is read - the counts that
        // decide its length (n_cards, n_ids) first, then the extent they imply.
        // A bound checked after the reads it guards is not a bound; an optimiser
        // is free to sink the loads past it.
        if (p + 6 > len) return FIO_EPARSE;
        const int n_cards = in[p + 4], n_ids = in[p + 5];
        if (n_cards > ANIM_MAX_CARDS || n_ids > n_cards) return FIO_ECAP;
        if (p + 9 + np + n_ids > len) return FIO_EPARSE;
        if (n_pool + n_ids > ANIM_MAX_CARD_POOL) return FIO_ECAP;
        const int type = in[p], seat = in[p + 1], from = in[p + 2], to = in[p + 3];
        const int has_counts = in[p + 6], deck = in[p + 7], discard = in[p + 8];
        p += 9;
        for (int s = 0; s < np; s++) hands[i][s] = in[p + s];
        p += np;
        Card *ids = &pool[n_pool];
        for (int k = 0; k < n_ids; k++) {
            if (in[p + k] >= 52) return FIO_EPARSE;
            ids[k] = card_of_id(in[p + k]);
        }
        n_pool += n_ids;
        p += n_ids;
        evs[i].type = type;
        evs[i].seat = (seat == 0xFF) ? ANIM_SEAT_NONE : seat;
        evs[i].from = from;
        evs[i].to = to;
        evs[i].cards = ids;
        evs[i].n_cards = n_cards;
        // Only REAL identities travel; a viewer-masked step lists none, and the
        // veil must leave its cards alone rather than veil a back.
        evs[i].mask_cards = (n_ids < n_cards);
        evs[i].has_counts = has_counts ? 1 : 0;
        evs[i].deck = deck;
        evs[i].discard = discard;
        evs[i].hand = hands[i];
    }

    static AnimPlan plan;
    const int rc = anim_build_plan(evs, n, np, in[3], in[4], final_hand, &plan);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    if (rc != ANIM_EOK) return FIO_EBADARG;

    const int need = FIO_PLAN_HEAD + plan.n_steps * FIO_PLAN_STRIDE + plan.n_veil;
    if (cap < need) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    for (int i = 0; i < need; i++) q[i] = 0;
    q[0] = FIO_PLAN_VERSION;
    q[1] = (unsigned char)plan.n_steps;
    q[2] = (unsigned char)np;
    q[3] = (unsigned char)plan.n_veil;
    for (int i = 0; i < 4; i++) q[4 + i] = (unsigned char)((plan.total_ms >> (8 * i)) & 0xff);
    q[8] = (unsigned char)plan.pre.deck;
    q[9] = (unsigned char)plan.pre.discard;
    for (int s = 0; s < np; s++) q[10 + s] = (unsigned char)plan.pre.hand[s];
    for (int i = 0; i < plan.n_steps; i++) {
        const AnimPlanStep *st = &plan.steps[i];
        unsigned char *e = q + FIO_PLAN_HEAD + i * FIO_PLAN_STRIDE;
        e[0] = (unsigned char)st->type;
        e[1] = (unsigned char)(st->seat < 0 ? 0xFF : st->seat);
        e[2] = (unsigned char)st->from;
        e[3] = (unsigned char)st->to;
        e[4] = (unsigned char)st->n_cards;
        e[5] = (unsigned char)(st->duration_ms & 0xff);
        e[6] = (unsigned char)((st->duration_ms >> 8) & 0xff);
        for (int k = 0; k < 4; k++) e[7 + k] = (unsigned char)((st->start_ms >> (8 * k)) & 0xff);
        e[11] = (unsigned char)st->deck;
        e[12] = (unsigned char)st->discard;
        e[13] = (unsigned char)st->in_flight_from_deck;
        e[14] = (unsigned char)st->in_flight_to_flipped;
        for (int s = 0; s < np; s++) e[15 + s] = (unsigned char)st->hand[s];
    }
    unsigned char *v = q + FIO_PLAN_HEAD + plan.n_steps * FIO_PLAN_STRIDE;
    for (int i = 0; i < plan.n_veil; i++) v[i] = plan.veil_ids[i];
    return need;
}

int fio_anim_should_drop_stale(int has_last, int last, int has_incoming, int incoming) {
    return anim_should_drop_stale(has_last, last, has_incoming, incoming);
}

// ---------- the shape of a sequence ----------------------------------------
//
// The layout is documented once, in ios_api.h. Reads nothing but its arguments:
// the stream crosses WITH the question because the board's stream is often not
// the resident game's (a staged bout end is cut in half and the settlement
// withheld), and because a SwiftUI body cannot await the actor it lives behind.

// Every dense id the input names, across the whole stream. A turn is at most
// ANIM_MAX_BEATS events and no event names more cards than a full table sweep.
#define FIO_BEATS_MAX_IDS 1024

int fio_beats_packed(const uint8_t *in, int len, char *out, int cap) {
    if (!in || !out || len < 2) return FIO_EBADARG;
    if (in[0] != FIO_BEATS_VERSION) return FIO_EPARSE;
    const int n = in[1];
    if (n > ANIM_MAX_BEATS) return FIO_ECAP;

    AnimBeatEvent evs[ANIM_MAX_BEATS];
    Card ids[FIO_BEATS_MAX_IDS];
    int n_ids = 0, p = 2;
    for (int i = 0; i < n; i++) {
        if (p + 5 > len) return FIO_EPARSE;
        const int type = in[p];
        const int seat = in[p + 1];
        const int has_good = in[p + 2];
        const int good = in[p + 3];
        const int k = in[p + 4];
        p += 5;
        if (p + k > len) return FIO_EPARSE;
        if (n_ids + k > FIO_BEATS_MAX_IDS) return FIO_ECAP;
        evs[i].type = type;
        evs[i].seat = (seat == 0xFF) ? ANIM_SEAT_NONE : seat;
        evs[i].cards = &ids[n_ids];
        evs[i].n_cards = k;
        evs[i].mask_cards = 0;   // only real identities are ever listed
        evs[i].good_mask = has_good ? good : ANIM_NO_MASK;
        for (int c = 0; c < k; c++) {
            if (in[p + c] >= 52) return FIO_EPARSE;
            ids[n_ids + c] = card_of_id(in[p + c]);
        }
        n_ids += k;
        p += k;
    }

    AnimBeats b;
    const int r = anim_build_beats(evs, n, &b);
    if (r == ANIM_ECAP) return FIO_ECAP;
    if (r < 0) return FIO_EBADARG;
    if (cap < FIO_BEATS_HEAD + b.n_beats * FIO_BEATS_STRIDE) return FIO_ECAP;

    unsigned char *q = (unsigned char *)out;
    q[0] = FIO_BEATS_VERSION;
    q[1] = (unsigned char)b.n_beats;
    q[2] = (unsigned char)(b.first_good_mask == ANIM_NO_MASK ? 0 : 1);
    q[3] = (unsigned char)(b.first_good_mask == ANIM_NO_MASK ? 0 : b.first_good_mask);
    for (int i = 0; i < 8; i++) q[4 + i] = (unsigned char)((b.placed_ids >> (8 * i)) & 0xff);
    for (int g = 0; g < b.n_beats; g++) {
        const AnimBeat *bt = &b.beats[g];
        unsigned char *e = q + FIO_BEATS_HEAD + g * FIO_BEATS_STRIDE;
        e[0] = (unsigned char)bt->first;
        e[1] = (unsigned char)bt->n_events;
        e[2] = (unsigned char)bt->type;
        e[3] = (unsigned char)(bt->seat < 0 ? 0xFF : bt->seat);
        e[4] = (unsigned char)bt->flags;
        e[5] = (unsigned char)(bt->outs_mask & 0xff);
        e[6] = (unsigned char)(bt->attack_pass_seats & 0xff);
        e[7] = (unsigned char)(bt->good_mask == ANIM_NO_MASK ? 0 : 1);
        e[8] = (unsigned char)(bt->good_mask == ANIM_NO_MASK ? 0 : bt->good_mask);
        for (int i = 0; i < 8; i++) e[9 + i] = (unsigned char)((bt->placed_ids >> (8 * i)) & 0xff);
    }
    return FIO_BEATS_HEAD + b.n_beats * FIO_BEATS_STRIDE;
}

// ---------- the pre-bout table ---------------------------------------------
//
// The layout is documented once, in ios_api.h. Like fio_beats_packed this reads
// nothing but its arguments, and the prior board travels with the stream
// because a single-action pickup turn has no earlier board of its own.

_Static_assert(FIO_PRETABLE_NONE == ANIM_TABLE_NONE,
               "one 'no card here' byte for a table, not two");
_Static_assert(ANIM_TABLE_NONE == LEGAL_WIRE_NONE,
               "…and it is the same byte the menu wire and PlayBoard use");

// One table off the wire: `n` battles at `p`, bounded against `end`. Returns
// the bytes consumed, or -1 for a table that runs off the buffer or names a
// card that is not one.
static int pretable_read(const uint8_t *p, const uint8_t *end, int n) {
    if (n < 0 || p + 2 * n > end) return -1;
    for (int i = 0; i < 2 * n; i++)
        if (p[i] >= 52 && p[i] != ANIM_TABLE_NONE) return -1;
    return 2 * n;
}

int fio_pre_bout_table_packed(const uint8_t *in, int len, char *out, int cap) {
    if (!in || !out || len < 3) return FIO_EBADARG;
    if (in[0] != FIO_PRETABLE_VERSION) return FIO_EPARSE;
    const int n = in[1];
    if (n > ANIM_MAX_STEPS) return FIO_ECAP;
    const uint8_t *end = in + len;

    int p = 2;
    const int prior_n = (in[p] == FIO_PRETABLE_NONE) ? ANIM_NO_BOARD : in[p];
    p++;
    const uint8_t *prior = in + p;
    if (prior_n > 0) {
        const int took = pretable_read(in + p, end, prior_n);
        if (took < 0) return FIO_EPARSE;
        p += took;
    }

    static AnimPreEvent evs[ANIM_MAX_STEPS];
    for (int i = 0; i < n; i++) {
        // The WHOLE event is bounded before any of it is read - the counts that
        // decide its length first, then the extent they imply. A bound checked
        // after the reads it guards is not a bound.
        if (in + p + 2 > end) return FIO_EPARSE;
        const int type = in[p];
        const int n_bat = (in[p + 1] == FIO_PRETABLE_NONE) ? ANIM_NO_BOARD : in[p + 1];
        p += 2;
        evs[i].type = type;
        evs[i].n_battles = n_bat;
        evs[i].battles = in + p;
        if (n_bat > 0) {
            const int took = pretable_read(in + p, end, n_bat);
            if (took < 0) return FIO_EPARSE;
            p += took;
        }
        if (in + p + 1 > end) return FIO_EPARSE;
        const int n_cards = in[p];
        p++;
        if (in + p + n_cards > end) return FIO_EPARSE;
        for (int k = 0; k < n_cards; k++) if (in[p + k] >= 52) return FIO_EPARSE;
        evs[i].n_cards = n_cards;
        evs[i].cards = in + p;
        p += n_cards;
    }

    static AnimPreTable t;
    const int rc = anim_pre_bout_table(evs, n, prior_n, prior, &t);
    if (rc == ANIM_ECAP) return FIO_ECAP;
    if (rc < 0) return FIO_EBADARG;

    const int need = FIO_PRETABLE_HEAD + 2 * t.n_battles;
    if (cap < need) return FIO_ECAP;
    unsigned char *q = (unsigned char *)out;
    q[0] = FIO_PRETABLE_VERSION;
    q[1] = (unsigned char)t.n_battles;
    q[2] = (unsigned char)(t.paired ? 1 : 0);
    for (int i = 0; i < 2 * t.n_battles; i++) q[FIO_PRETABLE_HEAD + i] = t.battles[i];
    return need;
}

int fio_badge_drops_as_cards_leave(int type) {
    return anim_badge_drops_as_cards_leave(type);
}

static int fio_roles_answer(int changed, const AnimRoles *r, int *out) {
    if (!changed) return 0;
    out[0] = r->defender;
    out[1] = r->first_attacker;
    out[2] = r->good_mask;
    return 1;
}

int fio_roles_goods_opening(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int first_good_mask, int *out) {
    if (!out) return FIO_EBADARG;
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    AnimRoles r;
    return fio_roles_answer(anim_goods_opening(shown, first_good_mask, &r), &r, out);
}

int fio_roles_goods_cleared(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int step_good_mask, int *out) {
    if (!out) return FIO_EBADARG;
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    AnimRoles r;
    return fio_roles_answer(anim_goods_cleared(shown, step_good_mask, &r), &r, out);
}

int fio_roles_pass_hand_off(int shown_defender, int shown_first_attacker,
                            int shown_good_mask, int attack_pass_seats,
                            int final_defender, int *out) {
    if (!out) return FIO_EBADARG;
    const AnimRoles shown = { shown_defender, shown_first_attacker, shown_good_mask };
    AnimRoles r;
    return fio_roles_answer(
        anim_pass_hand_off(shown, (unsigned)attack_pass_seats, final_defender, &r), &r, out);
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
    // A GENUINELY fresh game is the classic one: a variant is chosen for a
    // table, and this deal is not that table (fio_reseat_game, which re-derives
    // the SAME locked seed when a lobby starts, carries the rules across
    // itself). Without this a lobby played podkidnoy would leave the next local
    // game podkidnoy too, in a process that never restarts.
    g_msg_rules = 0;
    g_game.rules = g_msg_rules;
    g_has_game = 1;
    g_last_reject = 0;
    g_msg_base_logs = g_game.num_logs;   // a fresh deal continues nothing:
                                         // every atom after this is new
    g_msg_base_sent_at = 0;              // …and nobody has sent it anywhere
    // The pin is consumed by the deal above, never left standing: a later
    // ordinary game must not inherit a penalty that was owed to someone else.
    // fio_msg_start_rematch is the one caller that sets both, in that order.
    g_msg_opening = MSG_NO_OPENING;
    g_msg_carry_key = 0;
    g_msg_carry_fool = MSG_NO_FOOL;
    game_open_at_seat(-1);
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
    // …and the same for the RULES. This is the same table dealt again, not a new
    // one, so the variant the lobby chose crosses the re-deal (fio_new_game
    // clears it, deliberately, for the fresh-game case it also serves).
    const int8_t rules = g_msg_rules;
    const int rc = fio_new_game(seed, FOOLISH_SEED_LEN, n_players);  // overwrites g_deal_seed
    if (rc == FIO_EOK) { g_msg_rules = rules; g_game.rules = rules; }
    return rc;
}

// SET THE TABLE'S RULES before a lobby is sealed - the iMessage lobby's passing
// checkbox, and the only way this variant is ever chosen. `passing` is 1 for
// perevodnoy (the transfer, the default) and 0 for podkidnoy.
//
// It writes both the resident game and the sticky copy, because the two answer
// different questions: the game's own rules decide what is LEGAL right now (and
// what the body of the next seal is coded against), and the sticky copy is what
// survives the re-deal Start performs. A caller sets this AFTER adopting the
// lobby it is changing, and the very next seal states it on the wire.
int fio_set_passing(int passing) {
    g_msg_rules = passing ? 0 : (int8_t)GAME_RULE_NO_PASS;
    if (g_has_game) g_game.rules = g_msg_rules;
    return FIO_EOK;
}

// The resident game's rules, as the same 1/0 fio_set_passing takes. 1 with no
// game resident: nothing has said otherwise, and the classic game is what a
// fresh one would be.
int fio_passing_allowed(void) {
    return (g_msg_rules & GAME_RULE_NO_PASS) ? 0 : 1;
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
// THE KERNEL decides the group; the client passes the encoded chain and the one
// fact the chain cannot hold - `atoms_before`, how many atoms were on it BEFORE
// this bubble (-1 for "cannot say"). That is a property of the BUBBLE (its
// `turn` minus the delta it carries, msg_wire.h's n_new), never "where I last
// looked": a device's cache must not decide what animates.
//
// Why the count BEFORE rather than the count added, when the wire carries the
// latter: because the two are equivalent for a receiver and only this one is
// answerable by a SENDER. A device animating its own just-played move knows
// exactly what it adopted (the parent's turn) but not how many atoms its moves
// became - the codec is not 1:1 with actions, it folds a bout's closing goods
// into one round_end atom and can expand a closing good into two. Taking the
// base lets the kernel do that arithmetic against its own step count, so
// neither side has to guess.
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
// So the group is a SUFFIX of the step stream, and `atoms_before` says where it
// starts. A v6 replay is the deal then exactly one step per atom (replay_steps.c's
// rs_collect keeps every atom but DEAL/DRAW, which is precisely the set
// msg_replay counts into `turn`), so a chain with B atoms behind it opens its
// bubble at step B+1 and runs to the end - the atoms this bubble put on the
// chain, and nothing that was already there. B == the atom count means the
// bubble added nothing and nothing animates, which is the honest answer for the
// one bubble that can do it (an undo-to-empty re-seal, §10).
//
// WITHOUT a base (-1: a format-2 chain sealed before round 16, or a chain whose
// delta did not fit) it falls back to the guess that shipped before the field
// existed: the trailing run of steps by ONE acting seat - walk back
// over the seatless tail (ROUND_END belongs to whoever caused it), then back
// over every immediately preceding step by that same seat. That is right only
// when the sender staged its whole run and sent once, and the owner hit both
// ways it is wrong: covering, sending, covering, sending replays BOTH covers on
// the second bubble; and a cover that ends the bout with no ROUND_END atom (the
// defender's last card - handle_cover discards inline) sits directly before
// that same seat's opening attack of the next bout, so replaying the attack
// replays the cover with it. Both are exact with a base, which is why the wire
// field was added rather than the heuristic sharpened - no walk over the steps
// can separate two bubbles that a single bubble could have produced.
//
// Every frame is masked for `viewer` exactly like live play: the viewer's own
// drawn/picked-up cards carry real identities (fixing "my own refill never
// animated on reopen"), everyone else's are hidden backs.
//
// Frames come back in the shape replay_steps_frames_v6 writes them — each
// preceded by a u16 LE length, in play order. v6 only. Returns bytes written
// (0 if the turn produced nothing to animate), or a negative error.
int fio_replay_last_events_packed(const char *code, int viewer, int atoms_before,
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
    if (atoms_before >= 0) {
        // The bubble told us where it starts: step 0 is the DEAL, which no
        // bubble adds, so B atoms behind it means step B+1 onward…
        from = atoms_before + 1;
        if (from < 1) from = 1;
        if (from > n) from = n;          // added nothing: animate nothing
        // …except on a chain that IS only the deal (a genesis or the lobby's
        // LIVE handoff, n == 1), where the deal is the one thing to show.
        if (n == 1) from = 0;
    } else {
        // No delta: the pre-round-16 guess. See the note above for what it
        // cannot separate.
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

// Straight through to the rule in evwire.c - see ios_api.h for why the
// extension asks the kernel rather than switching on the type itself.
int fio_evw_is_settlement(int type) { return evw_is_settlement(type); }

// THE CUT, over the frame stream fio_replay_last_events_packed just handed
// back. The clients flatten those frames into one event list, so the answer is
// an index into the FLATTENED list; the walk across frames is in evwire.c and
// this is one line so it stays that way.
int fio_evw_frames_settlement_cut(const unsigned char *frames, int len) {
    return evwire_frames_settlement_cut(frames, len);
}

// Where THIS DEVICE's own staged run starts in the resident game's atom stream
// - the same question msg_seal answers for the bubble delta, asked for the
// animation instead of for the wire, and answered from the same log mark.
//
// It has to be the mark. A board animating its own move used to pass the atom
// count of the chain it ADOPTED, on the reasoning that everything past it is
// mine; but the atom stream is re-derived from the whole log on every encode
// (see g_msg_base_logs above), so that count can be HIGHER than the number of
// atoms the same history now encodes to. Passed as a starting point it lands
// past the end of the stream, and the kernel dutifully reports that this turn
// added nothing - the sender's own bout end animating not at all, and (round
// 16) its settlement not being recognised as one to withhold.
int fio_msg_staged_atoms_before(void) {
    if (g_msg_base_logs < 0) return -1;
    return replay_atoms_before_log(g_game.logs, g_game.num_logs, g_msg_base_logs);
}

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
//   game_id(u64 LE) parent8(8) digest(32) sent_at(u16 LE) n_new(1)
//   opening(1) carry_key(u32 LE) carry_fool(1) passing(1) n_joins(1)
//   then n_joins * { seat(1) name_len(1) name[name_len] }.
// ROUND 16: sent_at is the envelope's send clock (unix seconds mod 65536, 0 on
// a format-2 chain that carries none), and n_new is the bubble delta - how many
// atoms THIS bubble added (0 = the chain does not say; see msg_wire.h). Both
// sit at the END of the fixed header, after the digest, so every offset the
// Swift parser already knew is unchanged and only n_joins moves - this blob is
// a private ABI between two files in one repo, but keeping the prefix stable is
// what makes the diff readable.
// THE FOOL'S PENALTY (format 4) appends on the same principle: `opening` is the
// seat this deal opened on (0xFF = the ordinary lowest-trump derivation), and
// carry_key/carry_fool are a WAITING lobby's rematch carry (0 / 0xFF = none).
// The phone needs all three - it shows whose penalty is pending in the lobby,
// and it hands the carry back to the kernel at Start.
// THE RULES (format 5/6) append after them: `passing` is 1 when the defender may
// transfer and 0 for podkidnoy. The lobby draws its checkbox from it, and every
// later bubble repeats it - already resolved against the envelope's format, so
// there is nothing here for Swift to interpret.
// 1.0(6) DIAGNOSTIC: the replay codec version (5/6/7) of the body the last
// fio_msg_decode_packed replayed, or -1 for an empty-body message. Set through
// msg_last_body_version (msg_wire.c).
int fio_msg_last_body_version(void) { return msg_last_body_version; }

// The packed blob itself, written from an already-decoded envelope + its
// digest. Shared by the ADOPTING decode below and by the non-adopting peek, so
// the two can never come to describe a payload differently.
static int fio_msg_pack(const MsgEnvelope *e, const uint8_t *digest,
                        unsigned char *out, int cap) {
    int need = 4 + 2 + 8 + MSG_PARENT_LEN + SHA256_DIGEST_LEN + 2 + 1 + 1 + 4 + 1 + 1 + 1;
    for (int i = 0; i < e->n_joins; i++) need += 2 + e->joins[i].name_len;
    if (cap < need) return FIO_ECAP;

    unsigned char *q = out;
    *q++ = e->phase;
    *q++ = e->n_players;
    *q++ = e->last_actor_seat;
    *q++ = e->round;
    *q++ = (unsigned char)(e->turn & 0xff);
    *q++ = (unsigned char)((e->turn >> 8) & 0xff);
    for (int i = 0; i < 8; i++) *q++ = (unsigned char)((e->game_id >> (8 * i)) & 0xff);
    memcpy(q, e->parent8, MSG_PARENT_LEN); q += MSG_PARENT_LEN;
    memcpy(q, digest, SHA256_DIGEST_LEN); q += SHA256_DIGEST_LEN;
    *q++ = (unsigned char)(e->sent_at & 0xff);
    *q++ = (unsigned char)((e->sent_at >> 8) & 0xff);
    *q++ = e->n_new;
    *q++ = e->opening;
    for (int i = 0; i < 4; i++) *q++ = (unsigned char)((e->carry_key >> (8 * i)) & 0xff);
    *q++ = e->carry_fool;
    // THE RULES, as the one question a UI ever asks of them: may the defender
    // transfer (1) or not (0). Derived here rather than handed over raw, so
    // Swift never has to know which envelope formats carry a variant byte and
    // which are the passing game by definition (msg_pass_allowed).
    *q++ = (unsigned char)(msg_pass_allowed(e) ? 1 : 0);
    *q++ = (unsigned char)e->n_joins;
    for (int i = 0; i < e->n_joins; i++) {
        *q++ = e->joins[i].seat;
        *q++ = e->joins[i].name_len;
        memcpy(q, e->joins[i].name, e->joins[i].name_len); q += e->joins[i].name_len;
    }
    return (int)(q - out);
}

// READ a payload's header and CHANGE NOTHING: the same packed blob as
// fio_msg_decode_packed, without the replay and without touching one byte of
// the resident game or of the base a later seal measures its bubble against.
//
// ROUND 16 - because a decode is not a read. The composer decodes the payload
// it has just sealed, purely to read the joins and the summary out of it, and
// that decode used to ADOPT: it told the kernel "the chain up to and including
// my staged move is history somebody else made", so the NEXT action of the
// same turn measured its delta from the middle of its own bubble. A bubble
// carrying two actions then claimed one, and its caption and its recipient's
// animation both dropped everything but the last (owner: the bubble caption
// naming the wrong span of a turn). The base belongs to the chain this device
// ADOPTED - the bubble it opened, or its own bubble once sent - so composing
// one must not move it, and now it cannot.
//
// A peek can be asked of ANY payload, including one this device could not
// replay: nothing here validates the body, so the fields are the sender's
// claims. Use `fio_msg_decode_packed` for a chain that is about to be PLAYED -
// there validation is the replay, and the replay is the point.
int fio_msg_peek_packed(const uint8_t *payload, int len, unsigned char *out, int cap) {
    if (!payload || !out || cap <= 0) return FIO_EBADARG;
    g_last_msg_error = 0;

    MsgEnvelope e;
    const int rc = msg_decode(payload, len, &e);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }

    uint8_t digest[SHA256_DIGEST_LEN];
    msg_digest(payload, len, digest);
    return fio_msg_pack(&e, digest, out, cap);
}

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
    // This chain is now the base every later seal measures its bubble against -
    // the log mark it ends at (which is also what says the game has not moved
    // past it yet) and the clock a bubble that adds nothing must repeat.
    g_msg_base_logs = g_game.num_logs;
    g_msg_base_sent_at = e.sent_at;
    // …and this chain's opening seat is now the resident game's, so every seal
    // of it repeats the term of the deal the chain arrived with.
    g_msg_opening = e.opening;
    g_msg_carry_key = e.carry_key;
    g_msg_carry_fool = e.carry_fool;
    // …and so are its RULES: msg_replay has already stamped them onto the game
    // it dealt, and this is the copy that survives the re-deal at Start.
    g_msg_rules = msg_pass_allowed(&e) ? 0 : (int8_t)GAME_RULE_NO_PASS;
    memcpy(g_deal_seed, e.seed, FOOLISH_SEED_LEN);
    g_has_deal_seed = 1;
    for (int i = 0; i < e.n_players; i++) g_seat_roster[i] = (int8_t)bot_roster_find("random");

    return fio_msg_pack(&e, digest, out, cap);
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
    msg_envelope_init(&e);   // NOT memset: the rematch fields have sentinels
    e.format = MSG_FORMAT_V6;
    e.flags = 0;
    e.phase = (uint8_t)phase;
    e.game_id = game_id;
    e.last_actor_seat = (uint8_t)last_actor_seat;
    e.n_players = (uint8_t)g_game.num_players;
    // The rules are stamped by msg_seal, off the game itself - a host does not
    // get to state them independently of what it is sealing.
    // ROUND 16: the caller's clock, unix seconds mod 65536, or 0 for "do not
    // stamp this one" - which seals a format-2 envelope exactly as before. The
    // time is passed IN rather than read here on purpose: a kernel that called
    // time() would answer differently on two devices holding the same bytes.
    e.sent_at = (uint16_t)(sent_at & 0xffff);
    // …EXCEPT on the bubble that adds nothing (§10's undo-to-empty re-seal),
    // which repeats the adopted chain's stamp instead. `sent_at` is not "when
    // these bytes were made", it is when the move in them was played, and this
    // bubble carries no move: stamping now would restart the defender's pickup
    // hold on an attack that was sent minutes ago, every time somebody changed
    // their mind. See g_msg_base_sent_at.
    const int seal_base = msg_seal_base(&g_game, g_msg_base_logs);
    if (seal_base == MSG_BASE_NOTHING) e.sent_at = g_msg_base_sent_at;
    // The resident game's opening seat, repeated (see g_msg_opening). Not a
    // caller's argument: a host that could choose it per bubble could re-point
    // the deal mid-chain, and msg_replay would reject the result anyway.
    e.opening = g_msg_opening;
    e.carry_key = g_msg_carry_key;
    e.carry_fool = g_msg_carry_fool;
    if (parent8) memcpy(e.parent8, parent8, MSG_PARENT_LEN);
    memcpy(e.seed, g_deal_seed, FOOLISH_SEED_LEN);

    const int jrc = fio_parse_joins(joins_json, &e);
    if (jrc != FIO_EOK) return jrc;

    static unsigned char body[1024];   // a v6 body measures ~68 B at 8 players
    static Game scratch;
    // ROUND 16: everything played since the resident game was established is
    // what this bubble adds, so the base is the delta msg_seal writes as n_new
    // - or MSG_BASE_NOTHING when nothing was played at all (msg_seal_base).
    const int rc = msg_seal(&e, &g_game, seal_base, body, (int)sizeof body, &scratch);
    if (rc != MSG_EOK) { g_last_msg_error = rc; return FIO_EMSG; }
    const int n = msg_encode(&e, out, cap);
    if (n < 0) { g_last_msg_error = n; return n == MSG_ECAP ? FIO_ECAP : FIO_EMSG; }
    return n;
}


// ---------- Rule F: the fool's penalty ------------------------------------

// Parse a joins JSON into a bare array, the shape msg_roster_key wants. Shares
// fio_parse_joins so the two entries below cannot read a roster differently
// from the way a seal writes one.
static int fio_joins_of(const char *joins_json, MsgJoin *out, int *n_out) {
    static MsgEnvelope tmp;   // static: MsgEnvelope is large, and this is a
    msg_envelope_init(&tmp);  // single-threaded actor (see the file header)
    const int rc = fio_parse_joins(joins_json, &tmp);
    if (rc != FIO_EOK) return rc;
    if (tmp.n_joins < 2 || tmp.n_joins > MSG_MAX_JOINS) return FIO_EBADARG;
    for (int i = 0; i < tmp.n_joins; i++) out[i] = tmp.joins[i];
    *n_out = tmp.n_joins;
    return FIO_EOK;
}

int fio_msg_carry(const char *joins_json, int fool_seat,
                  uint32_t *key_out, int *fool_index_out) {
    if (!joins_json || !key_out || !fool_index_out) return FIO_EBADARG;
    MsgJoin joins[MSG_MAX_JOINS];
    int n = 0;
    const int rc = fio_joins_of(joins_json, joins, &n);
    if (rc != FIO_EOK) return rc;
    if (fool_seat < 0 || fool_seat >= n) return FIO_EBADARG;

    uint32_t key = 0;
    int rot = 0;
    if (msg_roster_key(joins, n, &key, &rot) != MSG_EOK) return FIO_EBADARG;
    *key_out = key;
    // Back out of the seating into the canonical rotation the key was taken
    // over: canonical[k] == seated[(k + rot) % n], so seat s is index s - rot.
    *fool_index_out = ((fool_seat - rot) % n + n) % n;
    return FIO_EOK;
}

int fio_msg_set_carry(uint32_t key, int fool_index) {
    if (key == 0 || fool_index < 0 || fool_index >= MSG_MAX_JOINS) {
        g_msg_carry_key = 0;
        g_msg_carry_fool = MSG_NO_FOOL;
        return FIO_EOK;
    }
    g_msg_carry_key = key;
    g_msg_carry_fool = (uint8_t)fool_index;
    return FIO_EOK;
}

int fio_msg_penalty_fool_seat(const char *joins_json, uint32_t carry_key, int carry_fool) {
    if (!joins_json) return -1;
    MsgJoin joins[MSG_MAX_JOINS];
    int n = 0;
    if (fio_joins_of(joins_json, joins, &n) != FIO_EOK) return -1;
    const uint8_t fool = (carry_fool < 0 || carry_fool > 0xFF)
                       ? (uint8_t)MSG_NO_FOOL : (uint8_t)carry_fool;
    return msg_rematch_fool_seat(joins, n, carry_key, fool);
}

int fio_msg_start_rematch(const char *joins_json, uint32_t carry_key,
                          int carry_fool, int *opening_out) {
    if (!joins_json || !opening_out) return FIO_EBADARG;
    MsgJoin joins[MSG_MAX_JOINS];
    int n = 0;
    const int rc = fio_joins_of(joins_json, joins, &n);
    if (rc != FIO_EOK) return rc;

    const uint8_t fool = (carry_fool < 0 || carry_fool > 0xFF)
                       ? (uint8_t)MSG_NO_FOOL : (uint8_t)carry_fool;
    const int opening = msg_rematch_opening(joins, n, carry_key, fool);

    // Pin BEFORE the deal and set the resident term AFTER it: fio_new_game
    // (which fio_reseat_game runs) consumes the pin and then clears both, so
    // this order is the one that survives it.
    if (opening >= 0) game_open_at_seat(opening);
    const int drc = fio_reseat_game(n);
    if (drc != FIO_EOK) { game_open_at_seat(-1); return drc; }
    g_msg_opening = (opening >= 0) ? (uint8_t)opening : (uint8_t)MSG_NO_OPENING;

    *opening_out = opening;
    return FIO_EOK;
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

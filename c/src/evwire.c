#include "evwire.h"
#include "view.h"
#include "../wasm/wire.h"

// Emission state threaded through the per-event writers. `fail` latches on
// the first overflow so callers can bail with -1 (the buffer is bounds-
// checked before every byte run — a hostile/corrupt snapshot set degrades to
// an error, never an OOB write).
typedef struct {
    unsigned char *out;
    int cap;
    int len;
    int fail;
    int viewer;
    int n_events; // backpatched into the header
} Emit;

static void put_u8(Emit *e, unsigned char b) {
    if (e->fail || e->len + 1 > e->cap) { e->fail = 1; return; }
    e->out[e->len++] = b;
}

// The largest state_put payload: 17 fixed + deck + 2*battles + per-player
// header/hand + elimination. Bound it generously for the reserve check.
#define EVW_SNAP_MAX (24 + MAX_DECK + 2 * MAX_BATTLES + MAX_PLAYERS * (3 + MAX_HAND_SIZE) + MAX_PLAYERS)

static void put_snapshot(Emit *e, const Game *g) {
    if (e->fail || e->len + 2 + EVW_SNAP_MAX > e->cap) { e->fail = 1; return; }
    const int n = state_put(g, e->viewer, e->out + e->len + 2);
    e->out[e->len] = (unsigned char)(n & 0xff);
    e->out[e->len + 1] = (unsigned char)((n >> 8) & 0xff);
    e->len += 2 + n;
}

// Shared event header + card list. `mask_cards` implements the DEAL/REFILL
// redaction: hand-bound cards become card backs for every viewer except the
// receiving seat (spectators included) — the shouldSanitizeCards rule.
static void put_event(Emit *e, int type, int seat, int msg, int from, int to,
                      const Card *cards, int n_cards, int mask_cards,
                      int has_target, Card target, int has_battle, int battle,
                      const Game *snap) {
    put_u8(e, (unsigned char)type);
    put_u8(e, seat < 0 ? EVW_SEAT_NONE : (unsigned char)seat);
    put_u8(e, (unsigned char)msg);
    put_u8(e, (unsigned char)from);
    put_u8(e, (unsigned char)to);
    put_u8(e, (unsigned char)((has_target ? 1 : 0) | (has_battle ? 2 : 0)));
    put_u8(e, (unsigned char)n_cards);
    for (int i = 0; i < n_cards; i++) {
        put_u8(e, mask_cards ? (unsigned char)WIRE_CARD_HIDDEN : wire_from_card(cards[i]));
    }
    if (has_target) put_u8(e, wire_from_card(target));
    if (has_battle) put_u8(e, (unsigned char)battle);
    put_snapshot(e, snap);
    e->n_events++;
}

// ---------- the one derivation ---------------------------------------------
//
// evwire_walk below turns (hook snapshots + this action's logs) into the event
// sequence, and hands each event to a SINK. The packed evwire writer is one
// sink; the iOS bridge's JSON emitter is another (docs/C_CORE_CONSOLIDATION.md
// F4). Which card flies where is therefore derived exactly once, whatever the
// destination — a second derivation per platform is the whole finding.

static void sink_packed(void *ctx, const EvwEvent *ev) {
    Emit *e = (Emit *)ctx;
    put_event(e, ev->type, ev->seat, ev->msg, ev->from, ev->to,
              ev->cards, ev->n_cards, ev->mask_cards,
              ev->has_target, ev->target, ev->has_battle, ev->battle, ev->snap);
}

// Same argument order as put_event, so the derivation below reads unchanged.
static void ev_emit(EvwSink sink, void *ctx, int type, int seat, int msg,
                    int from, int to, const Card *cards, int n_cards,
                    int mask_cards, int has_target, Card target,
                    int has_battle, int battle, const Game *snap) {
    EvwEvent ev;
    ev.type = type; ev.seat = seat; ev.msg = msg;
    ev.from = from; ev.to = to;
    ev.cards = cards; ev.n_cards = n_cards; ev.mask_cards = mask_cards;
    ev.has_target = has_target; ev.target = target;
    ev.has_battle = has_battle; ev.battle = battle;
    ev.snap = snap;
    sink(ctx, &ev);
}

// First log of a type, or NULL — buildEvents' `find`.
static const GameLog *find_log(const GameLog *logs, int n, int type) {
    for (int i = 0; i < n; i++) if (logs[i].log_type == type) return &logs[i];
    return 0;
}

// Copy a log's primaries into a flat card list (log pairs cap the count).
static int log_primaries(const GameLog *l, Card *buf) {
    if (!l) return 0;
    for (int i = 0; i < l->num_pairs; i++) buf[i] = l->pairs[i].primary;
    return l->num_pairs;
}

// See evwire.h - the consequence steps a bout-ender runs inside its own
// handle_*, as opposed to the acting seat's own play.
int evw_is_settlement(int type) {
    return type == EVW_T_MAGIC_TRANSITION
        || type == EVW_T_DISCARD
        || type == EVW_T_REFILL
        || type == EVW_T_CARDS_TO_TRASH;
}

void evwire_walk(const EvSnap *snaps, int n_snaps,
                 const GameLog *logs, int n_logs, int viewer,
                 EvwSink sink, void *ctx) {
    // Sequential readers (each DRAW/COVER hook consumes the next matching
    // log) and single-instance logs — mirrors buildEvents exactly.
    const GameLog *discard_log = find_log(logs, n_logs, LOG_DISCARD);
    int draw_i = 0, cover_i = 0;
    Card cards[MAX_LOG_PAIRS];

    for (int si = 0; si < n_snaps; si++) {
        const EvSnap *s = &snaps[si];
        switch (s->tag) {
            case ENGINE_HOOK_ATTACK: {
                const int n = log_primaries(find_log(logs, n_logs, LOG_ATTACK), cards);
                ev_emit(sink, ctx, EVW_T_ATTACK_PASS, s->aux, EVW_MSG_ATTACKED,
                          EVW_LOC_HAND, EVW_LOC_TABLE, cards, n, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_PASS: {
                const int n = log_primaries(find_log(logs, n_logs, LOG_PASS), cards);
                ev_emit(sink, ctx, EVW_T_ATTACK_PASS, s->aux, EVW_MSG_PASSED,
                          EVW_LOC_HAND, EVW_LOC_TABLE, cards, n, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_OUT:
                ev_emit(sink, ctx, EVW_T_OUT, s->aux, EVW_MSG_OUT,
                          EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            case ENGINE_HOOK_COVER: {
                // Next COVER log: [cover card, attack card being covered].
                const GameLog *l = 0;
                for (int i = 0, seen = 0; i < n_logs; i++) {
                    if (logs[i].log_type == LOG_COVER && seen++ == cover_i) { l = &logs[i]; break; }
                }
                cover_i++;
                if (!l || l->num_pairs < 1) break; // corrupt input: skip, never crash
                // The seat that covered is the defender at snapshot time.
                ev_emit(sink, ctx, EVW_T_COVER, s->g->defender, EVW_MSG_COVERED,
                          EVW_LOC_HAND, EVW_LOC_TABLE, &l->pairs[0].primary, 1, 0,
                          1, l->pairs[0].target, 1, s->aux, s->g);
                break;
            }
            case ENGINE_HOOK_DISCARD: {
                const int n = log_primaries(discard_log, cards);
                ev_emit(sink, ctx, EVW_T_DISCARD, -1, EVW_MSG_DISCARDED,
                          EVW_LOC_TABLE, EVW_LOC_DISCARD, cards, n, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_TRASH: {
                const int n = log_primaries(discard_log, cards);
                if (n > 0) {
                    ev_emit(sink, ctx, EVW_T_CARDS_TO_TRASH, -1, EVW_MSG_DISCARDED,
                              EVW_LOC_TABLE, EVW_LOC_DISCARD, cards, n, 0,
                              0, CARD_NONE, 0, 0, s->g);
                }
                break;
            }
            case ENGINE_HOOK_DRAW: {
                // Next DRAW log; real identities only for the drawing seat.
                const GameLog *l = 0;
                for (int i = 0, seen = 0; i < n_logs; i++) {
                    if (logs[i].log_type == LOG_DRAW && seen++ == draw_i) { l = &logs[i]; break; }
                }
                draw_i++;
                const int n = log_primaries(l, cards);
                ev_emit(sink, ctx, EVW_T_REFILL, s->aux, EVW_MSG_DREW,
                          EVW_LOC_DECK, EVW_LOC_HAND, cards, n, viewer != s->aux,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_DEFENDER_MOVE:
                ev_emit(sink, ctx, EVW_T_DEFENDER_MOVE, s->aux, EVW_MSG_DEFENDER_MOVE,
                          EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            case ENGINE_HOOK_PICKUP: {
                const int n = log_primaries(find_log(logs, n_logs, LOG_PICKUP), cards);
                ev_emit(sink, ctx, EVW_T_PICKUP, s->aux, EVW_MSG_PICKUP,
                          EVW_LOC_TABLE, EVW_LOC_HAND, cards, n, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_MAGIC_TRANSITION:
                ev_emit(sink, ctx, EVW_T_MAGIC_TRANSITION, -1, EVW_MSG_GOOD_TRANSITION,
                          EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            case ENGINE_HOOK_START_MAGIC:
                ev_emit(sink, ctx, EVW_T_MAGIC_TRANSITION, -1, EVW_MSG_START_MAGIC,
                          EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            case ENGINE_HOOK_DEAL: {
                // Cards = the dealt hand at snapshot time, masked per viewer.
                const Player *pl = (s->aux >= 0 && s->aux < s->g->num_players)
                    ? &s->g->players[s->aux] : 0;
                ev_emit(sink, ctx, EVW_T_DEAL, s->aux, EVW_MSG_NONE,
                          EVW_LOC_DECK, EVW_LOC_HAND,
                          pl ? pl->hand : 0, pl ? pl->hand_count : 0,
                          viewer != s->aux,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_FLIPPED: {
                Card f = s->g->flipped;
                ev_emit(sink, ctx, EVW_T_FLIPPED, -1, EVW_MSG_NONE,
                          EVW_LOC_DECK, EVW_LOC_FLIPPED, &f, 1, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            }
            case ENGINE_HOOK_START_DEFENDER:
                ev_emit(sink, ctx, EVW_T_DEFENDER_MOVE, s->aux, EVW_MSG_NONE,
                          EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                          0, CARD_NONE, 0, 0, s->g);
                ev_emit(sink, ctx, EVW_T_MAGIC_TRANSITION, -1, EVW_MSG_FIRST_ATTACKER,
                          EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                          0, CARD_NONE, 0, 0, s->g);
                break;
            default:
                break; // unknown hook: skip
        }
    }
}

int evwire_serialize(const EvSnap *snaps, int n_snaps,
                     const GameLog *logs, int n_logs,
                     const Game *final_g, int viewer, int actor,
                     int append_final_transition,
                     unsigned char *out, int cap) {
    Emit e = { out, cap, 0, 0, viewer, 0 };
    put_u8(&e, EVWIRE_FORMAT_VERSION);
    put_u8(&e, viewer < 0 ? EVW_SEAT_NONE : (unsigned char)viewer);
    put_u8(&e, actor < 0 ? EVW_SEAT_NONE : (unsigned char)actor);
    const int count_at = e.len;
    put_u8(&e, 0); // n_events, backpatched

    evwire_walk(snaps, n_snaps, logs, n_logs, viewer, sink_packed, &e);
    if (e.fail) return -1;

    // The game-over MAGIC_TRANSITION executeWithGameLock appended after
    // finalizeEndedGame — final state as its snapshot, no message.
    if (append_final_transition) {
        ev_emit(sink_packed, &e, EVW_T_MAGIC_TRANSITION, -1, EVW_MSG_NONE,
                EVW_LOC_NONE, EVW_LOC_NONE, 0, 0, 0,
                0, CARD_NONE, 0, 0, final_g);
    }

    // Trailer: the committed final state (the JSON payload's `game`).
    put_snapshot(&e, final_g);

    if (e.fail || e.n_events > 255) return -1;
    out[count_at] = (unsigned char)e.n_events;
    return e.len;
}

// ---------- the reader ------------------------------------------------------
//
// The counterpart of put_event/put_snapshot above, and deliberately in the same
// file: a format whose writer and reader live apart is two formats wearing one
// name. Every offset is range-checked against the caller's `len` before it is
// read, and a sequence that does not decode whole is refused outright.

int evwire_read_header(const unsigned char *buf, int len, EvwHeader *out) {
    if (!buf || len < 4) return EVW_EBADARG;
    if (buf[0] != EVWIRE_FORMAT_VERSION) return EVW_EPARSE;
    if (out) {
        out->version  = buf[0];
        out->viewer   = (buf[1] == EVW_SEAT_NONE) ? -1 : (int)buf[1];
        out->actor    = (buf[2] == EVW_SEAT_NONE) ? -1 : (int)buf[2];
        out->n_events = (int)buf[3];
    }
    return 0;
}

int evwire_read(const unsigned char *buf, int len,
                EvwHeader *out_hdr,
                const unsigned char **out_final, int *out_final_len,
                EvwReadSink sink, void *ctx) {
    EvwHeader h;
    const int rc = evwire_read_header(buf, len, &h);
    if (rc != 0) return rc;
    if (out_hdr) *out_hdr = h;

    int q = 4;
    for (int i = 0; i < h.n_events; i++) {
        // The fixed header, then the variable tail it describes. Both bounds
        // are taken before a single byte of either is read.
        if (q + 7 > len) return EVW_EPARSE;
        EvwRead ev;
        ev.type            = buf[q++];
        const int seat_b   = buf[q++];
        ev.seat            = (seat_b == EVW_SEAT_NONE) ? -1 : seat_b;
        ev.msg             = buf[q++];
        ev.from            = buf[q++];
        ev.to              = buf[q++];
        const int flags    = buf[q++];
        ev.n_cards         = buf[q++];
        ev.has_target      = (flags & 1) != 0;
        ev.has_battle      = (flags & 2) != 0;
        if (q + ev.n_cards + ev.has_target + ev.has_battle + 2 > len) return EVW_EPARSE;

        ev.cards_wire = buf + q; q += ev.n_cards;
        ev.target_wire = 0;
        ev.battle = 0;
        if (ev.has_target) ev.target_wire = buf[q++];
        if (ev.has_battle) ev.battle = buf[q++];

        ev.snap_len = buf[q] | (buf[q + 1] << 8); q += 2;
        if (ev.snap_len < 0 || q + ev.snap_len > len) return EVW_EPARSE;
        ev.snap = buf + q;
        q += ev.snap_len;

        if (sink) sink(ctx, i, &ev);
    }

    // Trailer: the committed final board - the sequence's `game`.
    if (q + 2 > len) return EVW_EPARSE;
    const int fin_len = buf[q] | (buf[q + 1] << 8); q += 2;
    if (fin_len < 0 || q + fin_len > len) return EVW_EPARSE;
    if (out_final) *out_final = buf + q;
    if (out_final_len) *out_final_len = fin_len;

    return h.n_events;
}

// The cut, counted across frames. `flat` is the running index into the
// flattened event list every client builds from these frames; `cut` latches the
// first settlement step and never moves again.
typedef struct { int flat; int cut; } EvwCut;

static void sink_cut(void *ctx, int index, const EvwRead *ev) {
    EvwCut *c = (EvwCut *)ctx;
    (void)index;
    if (c->cut < 0 && evw_is_settlement(ev->type)) c->cut = c->flat;
    c->flat++;
}

int evwire_frames_settlement_cut(const unsigned char *frames, int len) {
    // A turn that animates nothing is a turn that settled nothing, which is what
    // an empty stream honestly means - not an error.
    if (len == 0) return -1;
    if (!frames || len < 0) return EVW_EPARSE;

    EvwCut c = { 0, -1 };
    int p = 0;
    while (p < len) {
        if (p + 2 > len) return EVW_EPARSE;
        const int flen = frames[p] | (frames[p + 1] << 8);
        p += 2;
        if (flen <= 0 || p + flen > len) return EVW_EPARSE;
        const int n = evwire_read(frames + p, flen, 0, 0, 0, sink_cut, &c);
        if (n < 0) return EVW_EPARSE;
        p += flen;
    }
    return c.cut;
}

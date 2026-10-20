#include "evwire.h"
#include "view.h"
#include "../wasm/wire.h"
#include "../../../shared/c/le_bytes.h"

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
    le_put_u16(e->out + e->len, (uint16_t)n);
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
// sink; the settlement cut is another (docs/C_CORE_CONSOLIDATION.md F4). Which
// card flies where is therefore derived exactly once, whatever the destination -
// a second derivation per platform is the whole finding.

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
                // ONE card: the one that just arrived, masked per viewer. The
                // opening deal goes round the table a card at a time
                // (game.c deal_initial), one hook per card, and the card is the
                // newest in the receiving seat's hand at that snapshot.
                //
                // Read off the SNAPSHOT, not off a log the way DRAW is. A DRAW
                // is a logged move (LOG_DRAW, consumed in order by draw_i); the
                // deal logs nothing, and giving it a record per card would put
                // new bytes in the session log, the replay codes and the FMSG
                // chains that are all built from the log. The snapshot already
                // holds the answer, honestly: the reveal appends, so the
                // arrival is hand[hand_count - 1].
                const Player *pl = (s->aux >= 0 && s->aux < s->g->num_players)
                    ? &s->g->players[s->aux] : 0;
                const int n = (pl && pl->hand_count > 0) ? 1 : 0;
                ev_emit(sink, ctx, EVW_T_DEAL, s->aux, EVW_MSG_NONE,
                          EVW_LOC_DECK, EVW_LOC_HAND,
                          n ? &pl->hand[pl->hand_count - 1] : 0, n,
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

    // Trailer: the committed final state (the sequence's `game`).
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

        ev.snap_len = le_get_u16(buf + q); q += 2;
        if (ev.snap_len < 0 || q + ev.snap_len > len) return EVW_EPARSE;
        ev.snap = buf + q;
        q += ev.snap_len;

        if (sink) sink(ctx, i, &ev);
    }

    // Trailer: the committed final board - the sequence's `game`.
    if (q + 2 > len) return EVW_EPARSE;
    const int fin_len = le_get_u16(buf + q); q += 2;
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
        const int flen = le_get_u16(frames + p);
        p += 2;
        if (flen <= 0 || p + flen > len) return EVW_EPARSE;
        const int n = evwire_read(frames + p, flen, 0, 0, 0, sink_cut, &c);
        if (n < 0) return EVW_EPARSE;
        p += flen;
    }
    return c.cut;
}

int evwire_frames(const unsigned char *frames, int len, int *off, int *flen, int cap) {
    if (len == 0) return 0;
    if (!frames || len < 0 || cap < 0) return EVW_EBADARG;
    int p = 0, n = 0;
    while (p < len) {
        if (p + 2 > len) return EVW_EPARSE;
        const int f = le_get_u16(frames + p);
        p += 2;
        if (f <= 0 || p + f > len) return EVW_EPARSE;
        if (off || flen) {
            if (n >= cap) return EVW_ECAP;
            if (off) off[n] = p;
            if (flen) flen[n] = f;
        }
        n++;
        p += f;
    }
    return n;
}

int evwire_as3_split(const unsigned char *buf, int len, int *seq_len, int *flags, int *block_off) {
    const unsigned char *fin = 0;
    int fin_len = 0;
    const int n = evwire_read(buf, len, 0, &fin, &fin_len, 0, 0);
    if (n < 0) return EVW_EPARSE;
    const int seq = (int)(fin - buf) + fin_len;
    if (seq >= len) return EVW_EPARSE;                    // no flags byte
    const int f = buf[seq];
    if (f & ~EVW_AS3_ROSTER) return EVW_EPARSE;
    if (!(f & EVW_AS3_ROSTER) && seq + 1 != len) return EVW_EPARSE;
    if ((f & EVW_AS3_ROSTER) && seq + 1 >= len) return EVW_EPARSE;
    if (seq_len) *seq_len = seq;
    if (flags) *flags = f;
    if (block_off) *block_off = seq + 1;
    return 0;
}

// ---------- the cast header -----------------------------------------------------

int evwire_cast_header_put(uint32_t version, unsigned char *out, int cap) {
    if (!out || cap < EVW_CAST_HEADER_BYTES) return EVW_ECAP;
    out[0] = EVW_CAST_FORMAT;
    le_put_u32(out + 1, version);
    return EVW_CAST_HEADER_BYTES;
}

int evwire_cast_header_get(const unsigned char *buf, int len, uint32_t *version) {
    if (!buf || len < EVW_CAST_HEADER_BYTES || buf[0] != EVW_CAST_FORMAT) return EVW_EPARSE;
    if (version) *version = le_get_u32(buf + 1);
    return EVW_CAST_HEADER_BYTES;
}

// ---------- the delta cast (evwire.h EVW_CAST_DELTA) -------------------------------

int evwire_moves_put(const EvwMove *moves, int n, unsigned char *out, int cap) {
    if (!moves || n < 1 || n > EVW_DELTA_MAX_MOVES || !out) return EVW_EBADARG;
    int at = 0;
    if (cap < 1) return EVW_ECAP;
    out[at++] = (unsigned char)n;
    for (int i = 0; i < n; i++) {
        const EvwMove *m = &moves[i];
        if (m->seat < 0 || m->seat >= MAX_PLAYERS || m->wire_len < 2 || m->wire_len > EVW_DELTA_WIRE_MAX) return EVW_EBADARG;
        if (at + 2 + m->wire_len > cap) return EVW_ECAP;
        out[at++] = (unsigned char)m->seat;
        out[at++] = m->wire_len;
        for (int k = 0; k < m->wire_len; k++) out[at++] = m->wire[k];
    }
    return at;
}

int evwire_moves_read(const unsigned char *buf, int len, EvwMove *out, int cap, int *n) {
    if (!buf || !n || len < 1) return EVW_EPARSE;
    const int count = buf[0];
    if (count < 1 || count > EVW_DELTA_MAX_MOVES || (out && count > cap)) return EVW_EPARSE;
    int at = 1;
    for (int i = 0; i < count; i++) {
        if (at + 2 > len) return EVW_EPARSE;
        const int seat = buf[at], wl = buf[at + 1];
        at += 2;
        // A whole awire frame and nothing else: the frame's own length must be
        // the one the list gives it.
        if (seat >= MAX_PLAYERS || wl < 2 || wl > EVW_DELTA_WIRE_MAX || at + wl > len
            || awire_frame_len(buf + at, wl) != wl) return EVW_EPARSE;
        if (out) {
            out[i].seat = (int8_t)seat;
            out[i].wire_len = (uint8_t)wl;
            for (int k = 0; k < wl; k++) out[i].wire[k] = buf[at + k];
        }
        at += wl;
    }
    *n = count;
    return at;
}

#define EVW_DELTA_HEAD 9   // format, u32 version, kernel, viewer, flags; the move count is the list's

int evwire_delta_put(uint32_t version, int viewer, int flags, const EvwMove *moves, int n_moves,
                     const unsigned char *reveal, int n_reveal, uint32_t check, unsigned char *out, int cap) {
    if (!out || (flags & ~EVW_DELTA_FLAGS) || viewer >= MAX_PLAYERS || n_reveal < 0 || n_reveal > EVW_DELTA_MAX_REVEAL
        || (n_reveal > 0 && (!reveal || viewer < 0))) return EVW_EBADARG;
    if (cap < EVW_DELTA_HEAD) return EVW_ECAP;
    out[0] = EVW_CAST_DELTA;
    le_put_u32(out + 1, version);
    out[5] = EVW_DELTA_KERNEL;
    out[6] = viewer < 0 ? EVW_SEAT_NONE : (unsigned char)viewer;
    out[7] = (unsigned char)flags;
    int at = 8;
    const int m = evwire_moves_put(moves, n_moves, out + at, cap - at);
    if (m < 0) return m;
    at += m;
    if (at + 1 + n_reveal + 4 > cap) return EVW_ECAP;
    out[at++] = (unsigned char)n_reveal;
    for (int i = 0; i < n_reveal; i++) {
        if (reveal[i] > 51) return EVW_EBADARG;
        out[at++] = reveal[i];
    }
    le_put_u32(out + at, check);
    return at + 4;
}

int evwire_delta_read(const unsigned char *buf, int len, EvwDelta *out) {
    if (!buf || !out || len < 8 + 1 + 1 + 4 || buf[0] != EVW_CAST_DELTA) return EVW_EPARSE;
    out->version = le_get_u32(buf + 1);
    out->kernel = buf[5];
    out->viewer = buf[6] == EVW_SEAT_NONE ? -1 : buf[6];
    out->flags = buf[7];
    if (out->viewer >= MAX_PLAYERS || (out->flags & ~EVW_DELTA_FLAGS)) return EVW_EPARSE;
    int at = 8;
    const int list = evwire_moves_read(buf + at, len - at, NULL, 0, &out->n_moves);
    if (list < 0) return EVW_EPARSE;
    out->moves = buf + at + 1;
    at += list;
    if (at + 1 > len) return EVW_EPARSE;
    out->n_reveal = buf[at++];
    if (out->n_reveal > EVW_DELTA_MAX_REVEAL || (out->n_reveal > 0 && out->viewer < 0) || at + out->n_reveal + 4 != len)
        return EVW_EPARSE;
    out->reveal = buf + at;
    for (int i = 0; i < out->n_reveal; i++) if (out->reveal[i] > 51) return EVW_EPARSE;
    at += out->n_reveal;
    out->check = le_get_u32(buf + at);
    return 0;
}

int evwire_cast_version(const unsigned char *buf, int len, uint32_t *version) {
    if (!buf || len < EVW_CAST_HEADER_BYTES || (buf[0] != EVW_CAST_FORMAT && buf[0] != EVW_CAST_DELTA)) return EVW_EPARSE;
    if (version) *version = le_get_u32(buf + 1);
    return buf[0];
}

int evwire_cast_over(const unsigned char *buf, int len) {
    EvwDelta d;
    if (len > 0 && buf && buf[0] == EVW_CAST_DELTA)
        return evwire_delta_read(buf, len, &d) == 0 ? (d.flags & EVW_DELTA_OVER) != 0 : EVW_EPARSE;
    uint32_t v;
    const int at = evwire_cast_header_get(buf, len, &v);
    const unsigned char *fin = 0;
    int fin_len = 0;
    if (at < 0 || evwire_read(buf + at, len - at, 0, &fin, &fin_len, 0, 0) < 0 || fin_len < 1) return EVW_EPARSE;
    return fin[0] == GAME_STATUS_GAME_OVER;
}

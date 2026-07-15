// FMSG v1 envelope codec — see msg_wire.h for the layout and the two-layer
// (structure vs semantics) split this file implements.
#include "msg_wire.h"
#include "awire.h"
#include "replay.h"
#include "../wasm/wire.h"
#include <string.h>

// ---------- little-endian readers/writers -------------------------------
// Explicit, byte-at-a-time: the payload's endianness is a WIRE property and
// must not inherit the host's. (Every target here is little-endian today; that
// is exactly the kind of accident that rots.)

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint64_t rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8);
}

static void wr64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

// ---------- shared validation -------------------------------------------
// Encode and decode run the SAME checks, so this host can never emit a payload
// it would itself reject — the property that keeps a chain replayable after any
// number of hops through any number of devices.

static int seed_is_zero(const uint8_t *seed) {
    unsigned char acc = 0;
    for (int i = 0; i < MSG_SEED_LEN; i++) acc |= seed[i];
    return acc == 0;
}

static int name_is_clean(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)s[i];
        // Control bytes only. UTF-8 continuation/lead bytes (>= 0x80) pass:
        // names are display strings, and the length cap is the real defense.
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

// Walks the packed chain, proving every frame well-formed and every actor seat
// in range. Returns MSG_EOK and sets *n_out to the frame count, or a negative
// MSG_E*. Reads nothing past `len`.
static int walk_actions(const unsigned char *p, int len, int n_players, int *n_out) {
    int off = 0, count = 0;
    while (off < len) {
        if (count >= MSG_MAX_ACTIONS) return MSG_EACTION;
        const int seat = p[off];
        if (seat >= n_players) return MSG_ESEAT;
        off++;
        const int flen = awire_frame_len(p + off, len - off);
        if (flen == 0) return MSG_EACTION; // malformed head, or runs off the end
        off += flen;
        count++;
    }
    *n_out = count;
    return MSG_EOK;
}

static int validate_fields(const MsgEnvelope *e) {
    if (e->format < MSG_FORMAT_RAW || e->format > MSG_FORMAT_MAX) return MSG_EFORMAT;
    if (e->flags & ~(unsigned)(MSG_FLAG_FAIR_DEAL | MSG_FLAG_GZIP)) return MSG_EFLAGS;
    // Both defined flags are spec'd but unbuilt (fair-deal is v2 per §15; gzip
    // never paid for itself at these sizes). Refusing them is what keeps the
    // version byte honest: a build that silently ignored a flag would read a
    // DIFFERENT game than the sender wrote.
    if (e->flags & MSG_FLAG_FAIR_DEAL) return MSG_EFLAGS;
    if (e->flags & MSG_FLAG_GZIP) return MSG_EFLAGS;

    if (e->phase > MSG_PHASE_FINISHED) return MSG_EPHASE;
    // ACCEPT exists only to carry fair-deal's commit-reveal round, which the
    // flag check above already rejected.
    if (e->phase == MSG_PHASE_ACCEPT) return MSG_EPHASE;

    if (e->n_players < 2 || e->n_players > MAX_PLAYERS) return MSG_EPLAYERS;
    if (e->variant != 0) return MSG_EVARIANT;
    if (e->last_actor_seat >= e->n_players) return MSG_ESEAT;

    // The seed is all-zero ONLY in fair-deal's pre-reveal phases; without that
    // flag an all-zero seed is a dead game (every device would deal the legacy
    // LCG's fixed deck), so it can only be corruption or a truncating encoder.
    if (seed_is_zero(e->seed)) return MSG_ESEED;

    if (e->n_joins < 1 || e->n_joins > e->n_players) return MSG_EJOINS;
    uint32_t seen = 0;
    for (int i = 0; i < e->n_joins; i++) {
        const MsgJoin *j = &e->joins[i];
        if (j->seat >= e->n_players) return MSG_ESEAT;
        if (seen & (1u << j->seat)) return MSG_ESEAT; // two claims on one seat
        seen |= 1u << j->seat;
        if (j->name_len > MSG_MAX_NAME) return MSG_ENAME;
        if (!name_is_clean(j->name, j->name_len)) return MSG_ENAME;
    }

    // A lobby has no kernel actions yet: nothing has been dealt to act on.
    if (e->phase == MSG_PHASE_WAITING && (e->n_actions != 0 || e->round != 0)) return MSG_EPHASE;

    if (e->n_actions < 0 || e->n_actions > MSG_MAX_ACTIONS) return MSG_EACTION;
    if (e->actions_len < 0 || e->actions_len > MSG_MAX_ACTION_BYTES) return MSG_EACTION;
    // `turn` is a claim the chain must back: Rule P orders on it before anyone
    // replays, so a header that inflates it would jump the queue for free.
    if (e->turn != (uint16_t)e->n_actions) return MSG_ETURN;
    return MSG_EOK;
}

// ---------- decode -------------------------------------------------------

int msg_decode(const unsigned char *in, int in_len, MsgEnvelope *out) {
    if (in_len < MSG_HEADER_LEN) return MSG_ESHORT;
    if (in[0] != MSG_MAGIC) return MSG_EMAGIC;
    if (in[1] < MSG_FORMAT_RAW || in[1] > MSG_FORMAT_MAX) return MSG_EFORMAT;

    memset(out, 0, sizeof(*out));
    out->format          = in[1];
    out->flags           = in[2];
    out->phase           = in[3];
    out->game_id         = rd64(in + 4);
    out->turn            = rd16(in + 12);
    out->last_actor_seat = in[14];
    out->n_players       = in[15];
    out->variant         = in[16];
    out->round           = in[17];
    memcpy(out->parent8, in + 18, MSG_PARENT_LEN);
    memcpy(out->seed, in + 26, MSG_SEED_LEN);

    const int n_joins = in[58];
    // Bound the count BEFORE the loop writes: n_joins is attacker-controlled
    // and joins[] is fixed at MSG_MAX_JOINS.
    if (n_joins < 1 || n_joins > MSG_MAX_JOINS) return MSG_EJOINS;
    out->n_joins = n_joins;

    int off = MSG_HEADER_LEN;
    for (int i = 0; i < n_joins; i++) {
        if (off + 2 > in_len) return MSG_ESHORT;
        const int seat = in[off];
        const int nlen = in[off + 1];
        off += 2;
        if (nlen > MSG_MAX_NAME) return MSG_ENAME;
        if (off + nlen > in_len) return MSG_ESHORT;
        out->joins[i].seat     = (uint8_t)seat;
        out->joins[i].name_len = (uint8_t)nlen;
        memcpy(out->joins[i].name, in + off, (size_t)nlen);
        off += nlen;
    }

    if (off + 2 > in_len) return MSG_ESHORT;
    const int n_actions = rd16(in + off);
    off += 2;
    if (n_actions > MSG_MAX_ACTIONS) return MSG_EACTION;

    const int actions_len = in_len - off;
    if (actions_len < 0) return MSG_ESHORT;
    if (actions_len > MSG_MAX_ACTION_BYTES) return MSG_EACTION;

    // n_players must be sane before walk_actions can range-check actor seats.
    if (in[15] < 2 || in[15] > MAX_PLAYERS) return MSG_EPLAYERS;

    if (out->format == MSG_FORMAT_RAW) {
        int walked = 0;
        const int rc = walk_actions(in + off, actions_len, in[15], &walked);
        if (rc != MSG_EOK) return rc;
        // The chain is self-delimiting, so a count that disagrees with the frames
        // means the header and the body describe different games. Never guess which.
        if (walked != n_actions) return MSG_EACTION;
    }
    // A format-2 body is an entropy-coded integer: it has no structure this
    // layer can check, and only the codec can say whether it is well-formed.
    // msg_replay is where it earns trust (and where n_actions is confirmed).

    out->n_actions   = n_actions;
    out->actions_len = actions_len;
    out->actions     = in + off;   // borrowed — see msg_wire.h

    return validate_fields(out);
}

// ---------- encode -------------------------------------------------------

int msg_encode(const MsgEnvelope *e, unsigned char *out, int out_cap) {
    const int rc = validate_fields(e);
    if (rc != MSG_EOK) return rc;

    if (e->actions_len > 0 && !e->actions) return MSG_EACTION;
    if (e->format == MSG_FORMAT_RAW) {
        // Re-walk the borrowed chain: the caller may have appended to it since
        // it was decoded (that IS the send path — Rule R rebases by appending),
        // so its framing is not proven just because it once was.
        int walked = 0;
        const int wrc = walk_actions(e->actions, e->actions_len, e->n_players, &walked);
        if (wrc != MSG_EOK) return wrc;
        if (walked != e->n_actions) return MSG_EACTION;
    }

    int need = MSG_HEADER_LEN;
    for (int i = 0; i < e->n_joins; i++) need += 2 + e->joins[i].name_len;
    need += 2 + e->actions_len;
    if (need > out_cap) return MSG_ECAP;

    out[0] = MSG_MAGIC;
    out[1] = e->format;
    out[2] = e->flags;
    out[3] = e->phase;
    wr64(out + 4, e->game_id);
    wr16(out + 12, e->turn);
    out[14] = e->last_actor_seat;
    out[15] = e->n_players;
    out[16] = e->variant;
    out[17] = e->round;
    memcpy(out + 18, e->parent8, MSG_PARENT_LEN);
    memcpy(out + 26, e->seed, MSG_SEED_LEN);
    out[58] = (unsigned char)e->n_joins;

    int off = MSG_HEADER_LEN;
    for (int i = 0; i < e->n_joins; i++) {
        out[off++] = e->joins[i].seat;
        out[off++] = e->joins[i].name_len;
        memcpy(out + off, e->joins[i].name, e->joins[i].name_len);
        off += e->joins[i].name_len;
    }
    wr16(out + off, (uint16_t)e->n_actions);
    off += 2;
    // memmove, not memcpy: in-place encode is a documented caller pattern and
    // the regions can overlap.
    memmove(out + off, e->actions, (size_t)e->actions_len);
    off += e->actions_len;
    return off;
}

// ---------- replay (validation) ------------------------------------------

// Deals the game the envelope's seed describes. Public kernel calls only — the
// same sequence fio_new_game uses, never a memcpy into a Game (§7.3).
static void deal_from_envelope(const MsgEnvelope *e, Game *g) {
    game_set_deal_seed_bytes(e->seed, MSG_SEED_LEN);
    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)e->n_players;
    for (int i = 0; i < e->n_players; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        g->players[i].strategy_key = 0;
        // Seats are positional here; the joins list is protocol-layer identity
        // the kernel never sees (§4.1).
        g->players[i].player_id[0] = 'p';
        g->players[i].player_id[1] = (char)('0' + i);
        g->players[i].player_id[2] = '\0';
    }
    start_game(g);
}

// Applies one action, counting round closures.
//
// A round closed iff the table emptied. The kernel clears num_battles at
// exactly three places and they are precisely the round closures: pickup
// (game.c handle_pickup), the all-good transition (execute_round_transition),
// and a cover that empties the defender's hand (handle_cover discards inline).
// Counting log records instead would be WRONG in rules.wasm, which builds at
// MAX_LOGS=128 and silently drops a full game's overflow.
static int apply_one(Game *g, int seat, const AwireAction *a, int *rounds) {
    const int battles_before = g->num_battles;
    bool ok;
    switch (a->kind) {
        case AWIRE_ATTACK: ok = handle_attack(g, seat, a->cards, a->n); break;
        case AWIRE_COVER:  ok = handle_cover(g, seat, a->cards, a->attacks, a->n); break;
        case AWIRE_PASS:   ok = handle_pass(g, seat, a->cards, a->n); break;
        case AWIRE_PICKUP: ok = handle_pickup(g, seat); break;
        case AWIRE_GOOD:   ok = handle_good(g, seat); break;
        default:           return MSG_EACTION;
    }
    // Validation IS replay (§7.3): the kernel's rejection is the only verdict,
    // and one bad action condemns the whole chain.
    if (!ok) return MSG_ECHAIN;
    if (battles_before > 0 && g->num_battles == 0) (*rounds)++;
    return MSG_EOK;
}

// format 1: walk the raw frames.
static int replay_raw(const MsgEnvelope *e, Game *g, int *rounds, int *applied) {
    int off = 0;
    for (int i = 0; i < e->n_actions; i++) {
        const int seat = e->actions[off];
        off++;
        const int flen = awire_frame_len(e->actions + off, e->actions_len - off);
        if (flen == 0) return MSG_EACTION;   // decode proved this; belt and braces
        AwireAction a;
        if (!awire_decode(e->actions + off, flen, &a)) return MSG_EACTION;
        off += flen;
        const int rc = apply_one(g, seat, &a, rounds);
        if (rc != MSG_EOK) return rc;
        (*applied)++;
    }
    return MSG_EOK;
}

// format 2: expand the v6 code into a log stream, then re-apply its action logs.
//
// The decoded stream is v5-shaped (replay.h "DECODE output"). We re-apply rather
// than trust it: the codec tells us WHAT was played, the kernel decides whether
// it was legal, so a hostile code gets the same verdict as a hostile raw chain.
// Note the codec models a round's GOOD declarations as one round_end atom and
// re-emits a LOG_GOOD per attacker on decode — so the goods we apply here are
// the codec's reconstruction, not the original bytes. That is exactly why
// msg_body_from_game refuses a game with goods still pending (see there).
static int replay_v6_body(const MsgEnvelope *e, Game *g, int *rounds, int *applied,
                          unsigned char *scratch, int scratch_cap) {
    if (!scratch || scratch_cap < MSG_REPLAY_SCRATCH) return MSG_ESCRATCH;
    const int d = replay_decode(e->actions, e->actions_len, scratch, scratch_cap);
    if (d < 0) return MSG_EBODY;
    if (d < REPLAY_DEC_HDR) return MSG_EBODY;

    const unsigned char *p = scratch;
    if (p[1] != e->n_players) return MSG_EBODY;   // the code disagrees about the table

    const uint32_t n_logs = (uint32_t)p[16] | ((uint32_t)p[17] << 8) |
                            ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24);
    int off = REPLAY_DEC_HDR;
    for (uint32_t i = 0; i < n_logs; i++) {
        if (off + 4 > d) return MSG_EBODY;
        const int lt = p[off], seat = p[off + 1], npairs = p[off + 3];
        off += 4;
        if (off + npairs * 2 > d) return MSG_EBODY;
        const unsigned char *pairs = p + off;
        off += npairs * 2;

        AwireAction a;
        switch (lt) {
            case LOG_ATTACK: a.kind = AWIRE_ATTACK; break;
            case LOG_COVER:  a.kind = AWIRE_COVER;  break;
            case LOG_PASS:   a.kind = AWIRE_PASS;   break;
            case LOG_PICKUP: a.kind = AWIRE_PICKUP; break;
            case LOG_GOOD:   a.kind = AWIRE_GOOD;   break;
            default: continue;   // DRAW/DISCARD/DEFENDER_CHANGE/OUT: consequences, not actions
        }
        if (a.kind == AWIRE_PICKUP || a.kind == AWIRE_GOOD) {
            a.n = 0;
        } else {
            if (npairs > AWIRE_MAX_CARDS) return MSG_EBODY;
            a.n = npairs;
            for (int k = 0; k < npairs; k++) {
                a.cards[k]   = card_from_wire_state(pairs[k * 2]);
                a.attacks[k] = card_from_wire_state(pairs[k * 2 + 1]);
            }
        }
        if (seat < 0 || seat >= e->n_players) return MSG_EBODY;
        const int rc = apply_one(g, seat, &a, rounds);
        if (rc != MSG_EOK) return rc;
        (*applied)++;
    }
    return MSG_EOK;
}

int msg_replay(const MsgEnvelope *e, Game *g, unsigned char *scratch, int scratch_cap) {
    deal_from_envelope(e, g);

    int rounds = 0, applied = 0;
    const int rc = (e->format == MSG_FORMAT_V6)
        ? replay_v6_body(e, g, &rounds, &applied, scratch, scratch_cap)
        : replay_raw(e, g, &rounds, &applied);
    if (rc != MSG_EOK) return rc;

    // `turn` is Rule P's input and is read BEFORE anyone replays, so it must be
    // backed by the chain. For format 1 decode already proved it; for format 2
    // this is the first moment the claim can be checked at all.
    if (applied != (int)e->turn) return MSG_ETURN;

    if (rounds > 255) return MSG_EROUND;               // the wire field is a u8
    if (e->round != (uint8_t)rounds) return MSG_EROUND; // header vs. reality

    const int over = game_done(g) >= 0 || g->status == GAME_STATUS_GAME_OVER;
    if (over && e->phase != MSG_PHASE_FINISHED) return MSG_EPHASE;
    if (!over && e->phase == MSG_PHASE_FINISHED) return MSG_EPHASE;

    return MSG_EOK;
}

int msg_body_from_game(const MsgEnvelope *e, const Game *g,
                       unsigned char *body, int body_cap) {
    const int n = replay_encode_v6_from_game(g, e->seed, MSG_SEED_LEN,
                                             1 << 30, body, body_cap);
    if (n < 0) return MSG_EBODY;
    return n;
}

void msg_digest(const unsigned char *envelope, int len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256(envelope, (size_t)(len < 0 ? 0 : len), out);
}

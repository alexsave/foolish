// FMSG envelope codec — see msg_wire.h for the layout, the two-layer
// (structure vs semantics) split, and why the body is a v6 replay code.
#include "msg_wire.h"
#include "awire.h"
#include "replay.h"
#include <string.h>

// ---------- little-endian readers/writers -------------------------------
// Explicit, byte-at-a-time: the payload's endianness is a WIRE property and
// must not inherit the host's. (Every target here is little-endian today; that
// is exactly the kind of accident that rots.)

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint64_t rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8);
}

static void wr32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static void wr64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

// Overlap-safe copy. In-place encode (out aliasing the body) is a documented
// caller pattern, and the wasm build is freestanding: its string.h shim offers
// only memcpy/memset (wasm/include/string.h), so memmove is not available to
// reach for. Copying downward is sufficient here — the body always moves toward
// a LOWER address (the header it follows is fixed at 59 + joins bytes, which is
// never longer than the envelope it came from).
static void move_down(unsigned char *dst, const unsigned char *src, int n) {
    if (dst == src || n <= 0) return;
    for (int i = 0; i < n; i++) dst[i] = src[i];
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

// 1.0(6) DIAGNOSTIC: the replay codec version (9 or 10) of the last body decoded
// by msg_replay; -1 if the last message had no body (an empty lobby/handoff).
int msg_last_body_version = -1;

static int validate_fields(const MsgEnvelope *e) {
    if (e->format != MSG_FORMAT_V6 && e->format != MSG_FORMAT_CLOCK
        && e->format != MSG_FORMAT_REMATCH) return MSG_EFORMAT;
    // The clock is what format 3 IS, so the two must agree in both directions:
    // a format-2 envelope carrying a stamp would encode to bytes that decode
    // back without it, and the wire must round-trip to itself.
    if (e->format == MSG_FORMAT_V6 && e->sent_at != 0) return MSG_EFORMAT;
    // Same both-directions rule for the bubble delta: format 2 has nowhere to
    // put it, so an envelope claiming one is not a format-2 envelope.
    if (e->format == MSG_FORMAT_V6 && e->n_new != 0) return MSG_EFORMAT;
    // bit2 (0x04) is the LEGACY passing-allowed marker 1.0(3) briefly set on
    // every seal. The pass/perevod mode now lives in the replay code (the v7
    // bit, replay.h), so this build no longer sets or reads it - but bit2 is
    // still TOLERATED here so a bubble sealed by 1.0(3) still decodes (and
    // re-encodes to itself, keeping the wire canonical). No meaning is attached.
    if (e->flags & ~(unsigned)(MSG_FLAG_FAIR_DEAL | MSG_FLAG_GZIP | 0x04u)) return MSG_EFLAGS;
    // fair_deal / gzip are spec'd but unbuilt (fair-deal is v2 per §15; gzip
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
    // …and the bubble cannot have added more atoms than the whole chain holds.
    // A delta that overran `turn` would point the animation group at steps
    // before the deal, so it is refused here rather than clamped at read time:
    // this is the layer that decides whether bytes are an envelope at all.
    // MSG_NEW_NOTHING is exempt: it is not a count, it is the statement that
    // there is no count, and it is legal on a chain of any length (an
    // undo-to-empty re-seal can happen on turn 3 or turn 300).
    if (e->n_new != MSG_NEW_NOTHING && (int)e->n_new > e->n_actions) return MSG_ETURN;

    // The fool's penalty rides format 4 and nothing earlier: a format-2/3
    // header has nowhere to put these, so an envelope that claims one and
    // cannot carry it is not an envelope. (Same shape as the clock/delta rule
    // above - the format byte and the fields it implies must agree in both
    // directions, or a re-encode would silently drop a term of the deal.)
    if (e->format != MSG_FORMAT_REMATCH) {
        if (e->opening != MSG_NO_OPENING) return MSG_EFORMAT;
        if (e->carry_key != 0 || e->carry_fool != MSG_NO_FOOL) return MSG_EFORMAT;
    }
    if (e->opening != MSG_NO_OPENING && e->opening >= e->n_players) return MSG_ESEAT;
    if (e->carry_fool != MSG_NO_FOOL && e->carry_fool >= e->n_players) return MSG_ESEAT;
    // The carry is a lobby's question and the opening is a live game's answer;
    // a chain that carries both has confused the two phases.
    if (e->carry_key != 0 && e->opening != MSG_NO_OPENING) return MSG_EFORMAT;
    // Half a carry decides nothing and would read as an ordinary lobby on one
    // device and a penalty on another, so it is refused rather than ignored.
    if ((e->carry_key != 0) != (e->carry_fool != MSG_NO_FOOL)) return MSG_EFORMAT;
    return MSG_EOK;
}

// How long a format's fixed header is. Every format shares one prefix and
// appends; n_joins is always the last byte of it.
static int hdr_len_for(uint8_t format) {
    if (format == MSG_FORMAT_REMATCH) return MSG_HEADER_LEN_REMATCH;
    if (format == MSG_FORMAT_CLOCK)   return MSG_HEADER_LEN_CLOCK;
    return MSG_HEADER_LEN;
}

// ---------- decode -------------------------------------------------------

int msg_decode(const unsigned char *in, int in_len, MsgEnvelope *out) {
    if (in_len < MSG_HEADER_LEN) return MSG_ESHORT;
    if (in[0] != MSG_MAGIC) return MSG_EMAGIC;
    if (in[1] != MSG_FORMAT_V6 && in[1] != MSG_FORMAT_CLOCK
        && in[1] != MSG_FORMAT_REMATCH) return MSG_EFORMAT;
    // Format 3 is format 2 with two more header bytes at MSG_CLOCK_OFF, and
    // format 4 is format 3 with three more after those, so the whole prefix
    // below is read the same way and only n_joins onward shifts.
    const int has_clock   = (in[1] == MSG_FORMAT_CLOCK || in[1] == MSG_FORMAT_REMATCH);
    const int has_rematch = (in[1] == MSG_FORMAT_REMATCH);
    const int hdr_len     = hdr_len_for(in[1]);
    if (in_len < hdr_len) return MSG_ESHORT;

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

    out->sent_at = has_clock ? rd16(in + MSG_CLOCK_OFF) : 0;
    out->n_new   = has_clock ? in[MSG_NEW_OFF] : 0;

    // A format that cannot carry the rematch block decodes to the "no penalty"
    // sentinels, which is what every chain sealed before this format means.
    out->opening    = has_rematch ? in[MSG_OPEN_OFF] : MSG_NO_OPENING;
    out->carry_key  = has_rematch ? rd32(in + MSG_CARRY_OFF) : 0u;
    out->carry_fool = has_rematch ? in[MSG_FOOL_OFF] : MSG_NO_FOOL;

    const int n_joins = in[hdr_len - 1];
    // Bound the count BEFORE the loop writes: n_joins is attacker-controlled
    // and joins[] is fixed at MSG_MAX_JOINS.
    if (n_joins < 1 || n_joins > MSG_MAX_JOINS) return MSG_EJOINS;
    out->n_joins = n_joins;

    int off = hdr_len;
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

    // n_players must be sane before anything range-checks a seat.
    if (in[15] < 2 || in[15] > MAX_PLAYERS) return MSG_EPLAYERS;

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

    const int has_clock   = (e->format == MSG_FORMAT_CLOCK || e->format == MSG_FORMAT_REMATCH);
    const int has_rematch = (e->format == MSG_FORMAT_REMATCH);
    const int hdr_len     = hdr_len_for(e->format);

    int need = hdr_len;
    for (int i = 0; i < e->n_joins; i++) need += 2 + e->joins[i].name_len;
    need += 2 + e->actions_len;
    if (need > out_cap) return MSG_ECAP;

    out[0] = MSG_MAGIC;
    out[1] = e->format;
    out[2] = e->flags;   // faithful: whatever flags the envelope carries
    out[3] = e->phase;
    wr64(out + 4, e->game_id);
    wr16(out + 12, e->turn);
    out[14] = e->last_actor_seat;
    out[15] = e->n_players;
    out[16] = e->variant;
    out[17] = e->round;
    memcpy(out + 18, e->parent8, MSG_PARENT_LEN);
    memcpy(out + 26, e->seed, MSG_SEED_LEN);
    if (has_clock) {
        wr16(out + MSG_CLOCK_OFF, e->sent_at);
        out[MSG_NEW_OFF] = e->n_new;
    }
    if (has_rematch) {
        out[MSG_OPEN_OFF] = e->opening;
        wr32(out + MSG_CARRY_OFF, e->carry_key);
        out[MSG_FOOL_OFF] = e->carry_fool;
    }
    out[hdr_len - 1] = (unsigned char)e->n_joins;

    int off = hdr_len;
    for (int i = 0; i < e->n_joins; i++) {
        out[off++] = e->joins[i].seat;
        out[off++] = e->joins[i].name_len;
        memcpy(out + off, e->joins[i].name, e->joins[i].name_len);
        off += e->joins[i].name_len;
    }
    wr16(out + off, (uint16_t)e->n_actions);
    off += 2;
    move_down(out + off, e->actions, e->actions_len);
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
    // The fool's penalty travels with the chain, so re-dealing it is the whole
    // of honouring it: every device that holds these bytes deals the identical
    // board. Cleared immediately after - the override is a property of THIS
    // envelope, not of the process.
    if (e->opening != MSG_NO_OPENING) game_open_at_seat((int)e->opening);
    start_game(g);
    game_open_at_seat(-1);
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

// Applying the body's atoms onto the seeded deal.
//
// The atoms come from replay_decode_atoms_v6 — the codec's own level, not the
// log stream, so this costs no log buffer and reads the same GOOD/ROUND_END
// atoms replay_steps.c does. A sink cannot fail, so the first error is latched
// and the remaining atoms are ignored.
//
// DEAL and DRAW atoms are DELIBERATELY SKIPPED. This is the whole difference
// between an FMSG continuation and a replay, and it is the reason FMSG carries a
// seed at all. replay_steps.c rebuilds a deck from those atoms and fills the
// never-drawn tail in canonical order — correct for rendering a finished game
// ("its identities do not [matter] — nothing reveals them", rs_build_deck), but
// WRONG to play on from: a continuation draws from that tail, and canonical
// order is not the shuffled stock the deal produced. So a continuation must deal
// the TRUE deck from the seed (deal_from_envelope) and let the atoms supply only
// the ACTIONS. The seed is the future; the code is the past.
typedef struct {
    Game *g;
    int   n_players;
    int   rounds;
    int   applied;
    int   err;      // first MSG_E*, latched
} MsgApply;

static void msg_atom(void *ctx, const ReplayAtom *a) {
    MsgApply *m = (MsgApply *)ctx;
    if (m->err != MSG_EOK) return;
    if (a->kind == REPLAY_ATOM_DEAL || a->kind == REPLAY_ATOM_DRAW) return; // the seed's job

    Game *g = m->g;
    const int battles_before = g->num_battles;

    if (a->kind == REPLAY_ATOM_ROUND_END) {
        // Mirrors replay_steps.c's rs_apply: the bout closed because every IN
        // attacker said good, and the last one triggered the transition. A round
        // whose attackers are ALL out says nothing and would hang, so the
        // transition is run directly — that branch is exactly why the codec
        // reports ROUND_END as an atom instead of leaving it to be inferred.
        for (int s = 0; s < g->num_players; s++) {
            if (s == g->defender) continue;
            if (g->players[s].status != PLAYER_STATUS_IN) continue;
            handle_good(g, s);
        }
        if (g->num_battles > 0) engine_run_round_transition(g);
        if (battles_before > 0 && g->num_battles == 0) m->rounds++;
        m->applied++;
        return;
    }

    if (a->seat < 0 || a->seat >= m->n_players) { m->err = MSG_EBODY; return; }

    AwireAction w;
    switch (a->kind) {
        case REPLAY_ATOM_ATTACK: w.kind = AWIRE_ATTACK; break;
        case REPLAY_ATOM_COVER:  w.kind = AWIRE_COVER;  break;
        case REPLAY_ATOM_PASS:   w.kind = AWIRE_PASS;   break;
        case REPLAY_ATOM_PICKUP: w.kind = AWIRE_PICKUP; break;
        case REPLAY_ATOM_GOOD:   w.kind = AWIRE_GOOD;   break;
        default:                 m->err = MSG_EBODY; return;
    }
    if (w.kind == AWIRE_PICKUP || w.kind == AWIRE_GOOD) {
        w.n = 0;
    } else if (w.kind == AWIRE_COVER) {
        // A COVER atom is one pair: the card, and the attack it covers.
        w.n = 1;
        w.cards[0] = a->cards[0];
        w.attacks[0] = a->target;
    } else {
        if (a->n_cards > AWIRE_MAX_CARDS) { m->err = MSG_EBODY; return; }
        w.n = a->n_cards;
        for (int i = 0; i < a->n_cards; i++) w.cards[i] = a->cards[i];
    }

    const int rc = apply_one(g, a->seat, &w, &m->rounds);
    if (rc != MSG_EOK) { m->err = rc; return; }
    m->applied++;
}

int msg_replay(const MsgEnvelope *e, Game *g) {
    deal_from_envelope(e, g);
    // Read the opening seat NOW: g->first_attacker is reassigned every bout, so
    // by the end of the chain it holds the LAST round's attacker.
    const int opened_on = g->first_attacker;

    MsgApply m;
    m.g = g; m.n_players = e->n_players;
    m.rounds = 0; m.applied = 0; m.err = MSG_EOK;

    // A 0-action bubble carries no v6 body — the deal alone IS the state. Two
    // phases produce one: a WAITING lobby (§5.2, seats still filling) and the
    // last-joiner LIVE handoff that "applies nothing" to start the game. The atom
    // decoder needs a header these bodies don't have, so skip it; applied/rounds
    // stay 0 and the turn/round/phase checks below still gate the claim.
    if (e->actions_len != 0 || e->n_actions != 0) {
        ReplayHeader hdr;
        const int d = replay_decode_atoms_v6(e->actions, e->actions_len, &hdr, msg_atom, &m);
        if (d < 0) return MSG_EBODY;
        msg_last_body_version = hdr.version;   // 1.0(6) diag: the body's replay codec version
        if (m.err != MSG_EOK) return m.err;
        // The code's own header must describe the table the envelope claims.
        if (hdr.n != e->n_players) return MSG_EBODY;
        // …including who opened it. The body records the opener independently
        // (replay.h's v8 header), so this is the header's `opening` byte being
        // checked against the game it claims to describe rather than trusted:
        // a chain that names one opening seat and plays another does not
        // replay. It also catches the reverse - a rematch chain stripped of its
        // format-4 block by a re-encode would deal the lowest trump here and
        // disagree with its own body.
        if (hdr.first_attacker != opened_on) return MSG_EBODY;
    }

    const int rounds = m.rounds, applied = m.applied;

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

// Which format a seal writes: what it has to SAY decides, not the caller. A
// host that stamped `sent_at`, or that knows how much of the chain this bubble
// added, needs the format-3 header to carry it; one that knows neither (a test,
// a lobby handoff, the harness) keeps writing the format every shipped build can
// read. That keeps the pairing in one place instead of asking every caller to
// set the fields consistently - and validate_fields rejects the mismatch in
// both directions, so a caller that got it wrong would only find out at encode
// time.
static uint8_t seal_format(const MsgEnvelope *e) {
    if (e->opening != MSG_NO_OPENING || e->carry_key != 0
        || e->carry_fool != MSG_NO_FOOL) return MSG_FORMAT_REMATCH;
    return (e->sent_at != 0 || e->n_new != 0) ? MSG_FORMAT_CLOCK : MSG_FORMAT_V6;
}

int msg_seal_base(const Game *g, int base_logs) {
    if (base_logs < 0) return MSG_NO_BASE;
    // A saturated log buffer stops growing while the game keeps moving, so "the
    // count is unchanged" would stop meaning "nothing happened". The seal of
    // such a game fails anyway (the v6 producer refuses at MAX_LOGS, see
    // msg_seal's MSG_EBODY note), but this must not be the thing that decides
    // what it says on the way there.
    if (base_logs < MAX_LOGS && g->num_logs == base_logs)
        return MSG_BASE_NOTHING;
    return base_logs;
}

int msg_seal(MsgEnvelope *e, const Game *g, int base_logs,
             unsigned char *body, int body_cap, Game *scratch) {
    // A 0-action game seals to an EMPTY body: a WAITING lobby, or the last-joiner
    // LIVE handoff that "applies nothing" (§5.2). The v6 producer is an action-run
    // codec keyed on the logged opening attack — it has nothing to encode and no
    // first attacker to key on, so it (correctly) refuses. The deal alone is the
    // state; emit no body and let msg_replay's 0-action path rebuild from the seed.
    // "No opening attack logged" is the same fact the encoder keys on.
    if (replay_first_attacker_from_logs(g->logs, g->num_logs) < 0) {
        (void)scratch; (void)body_cap; (void)base_logs;
        e->actions     = body;   // unused (len 0), but a valid non-null buffer
        e->actions_len = 0;
        e->n_actions   = 0;
        e->turn        = 0;
        e->round       = 0;
        e->n_new       = 0;      // nothing was added; there is nothing to animate
        e->format      = seal_format(e);
        return MSG_EOK;
    }

    // 1 << 30 = every atom: a cut is what the CALLER already played to, not
    // something this decides.
    const int n = replay_encode_v6_from_game(g, e->seed, MSG_SEED_LEN,
                                             1 << 30, body, body_cap);
    if (n < 0) return MSG_EBODY;

    e->actions     = body;
    e->actions_len = n;

    // Read our own body back for the counts it — and only it — knows.
    deal_from_envelope(e, scratch);
    MsgApply m;
    m.g = scratch; m.n_players = e->n_players;
    m.rounds = 0; m.applied = 0; m.err = MSG_EOK;
    ReplayHeader hdr;
    const int d = replay_decode_atoms_v6(body, n, &hdr, msg_atom, &m);
    if (d < 0) return MSG_EBODY;
    if (m.err != MSG_EOK) return m.err;
    if (m.applied > MSG_MAX_ACTIONS) return MSG_EACTION;
    if (m.rounds > 255) return MSG_EROUND;

    e->n_actions = m.applied;
    e->turn      = (uint16_t)m.applied;
    e->round     = (uint8_t)m.rounds;

    // The bubble delta: the atoms this body devotes to what came AFTER the log
    // mark the caller adopted at. Three ways to end up saying nothing (0), and
    // all three degrade to the same documented fallback - the receiver guesses
    // the boundary, exactly as builds before this field did:
    //
    //   - MSG_NO_BASE (or any other negative): this host cannot say where the
    //     parent ended. MSG_BASE_NOTHING is NOT one of these - see below.
    //   - a delta past MSG_MAX_NEW: it will not fit the byte. Deliberately 0
    //     rather than a clamp - 255 would name a suffix that STARTS inside the
    //     bubble, which is a confident wrong answer, where 0 is an honest one.
    //     Unreachable in play: one bubble is one seat's staged turn, and the
    //     36-card deck caps how many atoms that can be even when a fat pickup
    //     leaves the defender covering a dozen attacks. The number of atoms in
    //     the GAME is not this field's problem - that is `turn`, a u16
    //     (MSG_MAX_ACTIONS 1024), and a 300-move game is 300 bubbles of delta
    //     1, not one delta of 300.
    //
    // MEASURED AGAINST THE LOG, not against the parent's atom count, because
    // the two subtract differently: a chain APPENDS logs but its atom stream is
    // re-derived from all of them, and a good that was an atom while it was
    // pending stops being one the moment anything follows it
    // (replay_atoms_before_log). Two atom counts subtracted therefore lose one
    // atom per superseded good - a defender who covered twice into one bubble
    // sealed a delta of 1, and both the caption on the bubble and the animation
    // its recipient played dropped the first cover. Asking the encoder where
    // MY logs begin has no such gap, and needs no clamp: an action that
    // supersedes more than it adds still contributes its own atom.
    int newly = 0;
    if (base_logs == MSG_BASE_NOTHING) {
        // THE THIRD STATE. The caller is not describing a boundary, it is
        // saying there is no move in this bubble at all - the undo-to-empty
        // re-seal that §10 uses to cancel a staged move, whose body is the
        // board the chain was already in. This cannot be derived here: a fold
        // (below) hands back the parent's atom count too, so a seal that
        // measured instead of listening would call one of them the other. Only
        // the host knows, because only the host knows whether anything was
        // applied since it adopted the chain.
        newly = MSG_NEW_NOTHING;
    } else if (base_logs >= 0 && m.applied > 0) {
        // Where MY logs start, in the atoms of the body just written. The
        // count comes from the game's own log array rather than from `scratch`
        // (the read-back) because the log mark is an index into THAT array -
        // and the two agree by construction, the body having been encoded from
        // it.
        const int before = replay_atoms_before_log(g->logs, g->num_logs, base_logs);
        newly = m.applied - before;
        // A chain that did not GROW still moved: the codec folds a bout's
        // closing goods into the ONE round_end atom that replaces them, so a
        // seal whose action closed the bout can come back with the same atom
        // count as its parent, or fewer - the atom is still this bubble's, and
        // the count above says so without a clamp. What is left is the
        // impossible answer: a base past the end of the body describes no
        // suffix at all, which is the wire's "does not say" rather than a
        // guess dressed up as a number.
        if (newly < 1) newly = 0;
        if (newly > MSG_MAX_NEW) newly = 0;
    }
    e->n_new  = (uint8_t)newly;

    // Format LAST: it is decided by what the header ends up carrying, and n_new
    // is only known here.
    e->format = seal_format(e);
    return MSG_EOK;
}

// ---------- Rule P --------------------------------------------------------

int msg_chain_key(const unsigned char *envelope, int len, MsgChainKey *out) {
    MsgEnvelope e;
    const int rc = msg_decode(envelope, len, &e);
    if (rc != MSG_EOK) return rc;
    out->phase = e.phase;
    out->round = e.round;
    out->turn  = e.turn;
    out->n_joins = (uint8_t)e.n_joins;
    msg_digest(envelope, len, out->digest);
    return MSG_EOK;
}

int msg_rule_p(const MsgChainKey *a, const MsgChainKey *b) {
    // A dealt game outranks the invite it grew out of, whatever the digests say
    // — see the header's rule 0 for the fork this closes. Only the boundary is
    // compared (started vs not), never FINISHED > LIVE: round/turn already order
    // those correctly, and a finished chain always has more of both.
    const int sa = a->phase >= MSG_PHASE_LIVE, sb = b->phase >= MSG_PHASE_LIVE;
    if (sa != sb) return sa ? -1 : 1;
    if (a->round != b->round) return a->round > b->round ? -1 : 1;
    if (a->turn  != b->turn)  return a->turn  > b->turn  ? -1 : 1;
    // The fuller roster wins the turn-0 tie (header rule 3): two Starts sealed
    // from different lobby states are DIFFERENT deals of the same seed, and the
    // digest was a coin flip between them — see the header for the deadlock
    // that produced. Below turn on purpose: a played-on chain is never clobbered
    // by a stale wider Start.
    if (a->n_joins != b->n_joins) return a->n_joins > b->n_joins ? -1 : 1;
    // Lexicographic over the full digest. Arbitrary, total, and identical on
    // every device — which is all a tiebreak has to be.
    for (int i = 0; i < SHA256_DIGEST_LEN; i++) {
        if (a->digest[i] != b->digest[i]) return a->digest[i] < b->digest[i] ? -1 : 1;
    }
    return 0;
}

// ---------- Rule R --------------------------------------------------------

int msg_rebase_one(Game *adopted, int adopted_round, int pending_round,
                   int seat, const AwireAction *a) {
    if (pending_round < adopted_round) return MSG_REBASE_DISCARD_ROUND;
    if (seat < 0 || seat >= adopted->num_players) return MSG_REBASE_DISCARD_ILLEGAL;

    // Ask by doing, on a clone: the handler's verdict IS legality, and a clone
    // means a refusal cannot leave the adopted game half-mutated (some handlers
    // reject after touching state — handle_pass's overflow branch aborts the
    // game, see game.c).
    static Game probe;
    game_clone(&probe, adopted);
    int rounds_ignored = 0;
    if (apply_one(&probe, seat, a, &rounds_ignored) != MSG_EOK) return MSG_REBASE_DISCARD_ILLEGAL;

    game_clone(adopted, &probe);
    return MSG_REBASE_REAPPLY;
}

void msg_digest(const unsigned char *envelope, int len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256(envelope, (size_t)(len < 0 ? 0 : len), out);
}

// ---------- the pickup hold (round 16) -----------------------------------

// Is a hold owed to this seat as a matter of STATE alone - before any clock is
// consulted? See msg_wire.h for each clause and why it is there.
static int pickup_is_held_by_state(const Game *g, int seat) {
    if (!g || seat < 0 || seat >= g->num_players) return 0;
    if (g->defender != seat) return 0;
    if (g->num_logs <= 0) return 0;

    // The LAST logged action, not "an attack happened this bout": a defender who
    // has already covered is not sitting in front of anything new.
    const int last = g->logs[g->num_logs - 1].log_type;
    if (last != LOG_ATTACK && last != LOG_PASS) return 0;

    // Spare capacity, or nothing can be thrown in anyway.
    int uncovered = 0;
    for (int i = 0; i < g->num_battles; i++) {
        if (card_is_none(g->table_battles[i].defense)) uncovered++;
    }
    return uncovered < g->players[seat].hand_count;
}

int msg_pickup_hold_remaining(const Game *g, int seat, uint16_t sent_at, uint16_t now) {
    if (sent_at == 0) return 0;                       // no clock: nothing to measure
    if (!pickup_is_held_by_state(g, seat)) return 0;
    // Unsigned 16-bit on purpose: a rollover between the two clocks cancels, and
    // a stamp that reads as being in the future becomes a large delta, which
    // releases the hold rather than maxing it (msg_wire.h).
    const uint16_t elapsed = (uint16_t)(now - sent_at);
    if (elapsed >= MSG_PICKUP_HOLD_S) return 0;
    return MSG_PICKUP_HOLD_S - (int)elapsed;
}

// ---------- Rule F: the fool's penalty -----------------------------------

void msg_envelope_init(MsgEnvelope *e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->opening    = MSG_NO_OPENING;
    e->carry_fool = MSG_NO_FOOL;
}

// One seat's contribution to the roster key, as the bytes that identify it:
// the name length then the name. Seat NUMBERS are deliberately absent - the
// key is about who is at the table and in what cycle, and the numbers are what
// rotates.
static int roster_cmp_at(const MsgJoin *joins, int n, int a, int b) {
    for (int k = 0; k < n; k++) {
        const MsgJoin *x = &joins[(a + k) % n];
        const MsgJoin *y = &joins[(b + k) % n];
        if (x->name_len != y->name_len) return x->name_len < y->name_len ? -1 : 1;
        for (int i = 0; i < x->name_len; i++) {
            const unsigned char cx = (unsigned char)x->name[i];
            const unsigned char cy = (unsigned char)y->name[i];
            if (cx != cy) return cx < cy ? -1 : 1;
        }
    }
    return 0;
}

int msg_roster_key(const MsgJoin *joins, int n, uint32_t *hash, int *rot) {
    if (!joins || !hash || n < 2 || n > MSG_MAX_JOINS) return MSG_EJOINS;

    // Sort the joins into SEAT order first. The array arrives in whatever order
    // the joins were appended, and "the cycle" is the seating, not the arrival
    // order - two devices that appended the same people differently must key
    // the same.
    MsgJoin seated[MSG_MAX_JOINS];
    for (int s = 0; s < n; s++) seated[s].name_len = 0xFF;   // "seat empty"
    for (int i = 0; i < n; i++) {
        if (joins[i].seat >= n) return MSG_ESEAT;
        if (seated[joins[i].seat].name_len != 0xFF) return MSG_ESEAT;  // duplicate
        seated[joins[i].seat] = joins[i];
    }
    for (int s = 0; s < n; s++) if (seated[s].name_len == 0xFF) return MSG_EJOINS;

    // The canonical rotation: the one whose byte sequence compares smallest.
    int best = 0;
    for (int r = 1; r < n; r++)
        if (roster_cmp_at(seated, n, r, best) < 0) best = r;

    // FNV-1a over the player count and then the canonical rotation's names.
    uint32_t h = 2166136261u;
    #define FNV_BYTE(b) do { h ^= (uint32_t)(unsigned char)(b); h *= 16777619u; } while (0)
    FNV_BYTE(n);
    for (int k = 0; k < n; k++) {
        const MsgJoin *j = &seated[(best + k) % n];
        FNV_BYTE(j->name_len);
        for (int i = 0; i < j->name_len; i++) FNV_BYTE(j->name[i]);
    }
    #undef FNV_BYTE

    *hash = h ? h : 1u;   // 0 is the wire's "no carry" sentinel
    if (rot) *rot = best;
    return MSG_EOK;
}

int msg_rematch_opening(const MsgJoin *joins, int n,
                        uint32_t carry_key, uint8_t carry_fool) {
    if (carry_key == 0 || carry_fool == MSG_NO_FOOL) return -1;
    if (n < 2 || n > MSG_MAX_JOINS || carry_fool >= n) return -1;

    uint32_t key = 0;
    int rot = 0;
    if (msg_roster_key(joins, n, &key, &rot) != MSG_EOK) return -1;
    // The owner's guard: the same people, in the same cycle, as the lobby was
    // born with. Anyone joined, left or renamed and this is simply a new game.
    if (key != carry_key) return -1;

    // carry_fool indexes the CANONICAL rotation; the seating in front of us is
    // rotated by `rot` against it (canonical[k] == seated[(k + rot) % n]).
    const int fool_seat = ((int)carry_fool + rot) % n;
    // The seat to the fool's RIGHT - the one whose attack lands on the fool,
    // because attacks travel to the attacker's left.
    return (fool_seat - 1 + n) % n;
}

int msg_rematch_fool_seat(const MsgJoin *joins, int n,
                          uint32_t carry_key, uint8_t carry_fool) {
    const int opening = msg_rematch_opening(joins, n, carry_key, carry_fool);
    if (opening < 0) return -1;
    // The fool is the opener's LEFT-hand neighbour, which is what makes them
    // the first defender. Derived from the opener rather than recomputed, so
    // the two answers cannot drift apart.
    return (opening + 1) % n;
}

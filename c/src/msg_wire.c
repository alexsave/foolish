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

static int validate_fields(const MsgEnvelope *e) {
    if (e->format != MSG_FORMAT_V6) return MSG_EFORMAT;
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
    if (in[1] != MSG_FORMAT_V6) return MSG_EFORMAT;

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
        if (m.err != MSG_EOK) return m.err;
        // The code's own header must describe the table the envelope claims.
        if (hdr.n != e->n_players) return MSG_EBODY;
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

int msg_seal(MsgEnvelope *e, const Game *g, unsigned char *body, int body_cap,
             Game *scratch) {
    // A 0-action game seals to an EMPTY body: a WAITING lobby, or the last-joiner
    // LIVE handoff that "applies nothing" (§5.2). The v6 producer is an action-run
    // codec keyed on the logged opening attack — it has nothing to encode and no
    // first attacker to key on, so it (correctly) refuses. The deal alone is the
    // state; emit no body and let msg_replay's 0-action path rebuild from the seed.
    // "No opening attack logged" is the same fact the encoder keys on.
    if (replay_first_attacker_from_logs(g->logs, g->num_logs) < 0) {
        (void)scratch; (void)body_cap;
        e->format      = MSG_FORMAT_V6;
        e->actions     = body;   // unused (len 0), but a valid non-null buffer
        e->actions_len = 0;
        e->n_actions   = 0;
        e->turn        = 0;
        e->round       = 0;
        return MSG_EOK;
    }

    // 1 << 30 = every atom: a cut is what the CALLER already played to, not
    // something this decides.
    const int n = replay_encode_v6_from_game(g, e->seed, MSG_SEED_LEN,
                                             1 << 30, body, body_cap);
    if (n < 0) return MSG_EBODY;

    e->format      = MSG_FORMAT_V6;
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
    return MSG_EOK;
}

// ---------- Rule P --------------------------------------------------------

int msg_chain_key(const unsigned char *envelope, int len, MsgChainKey *out) {
    MsgEnvelope e;
    const int rc = msg_decode(envelope, len, &e);
    if (rc != MSG_EOK) return rc;
    out->round = e.round;
    out->turn  = e.turn;
    msg_digest(envelope, len, out->digest);
    return MSG_EOK;
}

int msg_rule_p(const MsgChainKey *a, const MsgChainKey *b) {
    if (a->round != b->round) return a->round > b->round ? -1 : 1;
    if (a->turn  != b->turn)  return a->turn  > b->turn  ? -1 : 1;
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

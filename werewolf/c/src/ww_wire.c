#include "ww_wire.h"

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}
static uint64_t rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void wr64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

void ww_envelope_init(WwEnvelope *e) {
    unsigned char *q = (unsigned char *)e;
    for (unsigned i = 0; i < sizeof *e; i++) q[i] = 0;
    e->format = WW_FORMAT;
    e->last_actor_seat = WW_NO_SEAT;
}

// A record frame's length, read from the frame itself, reading no byte at or
// past p + avail. 0 means "does not fit / not a frame", which is the only answer
// a hostile buffer gets: every caller treats 0 as a refusal and stops.
static int ww_frame_len(const unsigned char *p, int avail) {
    if (avail < 4) return 0;
    if (!(p[3] & WW_REC_HAS_CHAT)) return 4;
    if (avail < 5) return 0;
    const int n = p[4];
    if (n > WW_CHAT_MAX) return 0;
    return (5 + n <= avail) ? 5 + n : 0;
}

// A frame's fields, judged for RANGE only - whether the record is legal is
// replay's question, not this layer's.
static int frame_fields_ok(const unsigned char *p) {
    const int seat = p[0], target = p[1], night = p[2], flags = p[3];
    if (seat != WW_NO_SEAT && seat >= WW_MAX_PLAYERS) return 0;
    if (target != WW_NO_SEAT && target >= WW_MAX_PLAYERS) return 0;
    if (night >= WW_MAX_NIGHTS) return 0;
    // Unknown flag bits are refused rather than ignored. A decoder that ignores
    // them will happily accept a future format's record as a present-day one and
    // replay it wrong, which is the worst of the three possible behaviours.
    if (flags & ~(WW_REC_AUTO_PASS | WW_REC_HAS_CHAT | WW_REC_LYNCH)) return 0;
    // An auto-pass is a pass: it names nobody and says nothing. Enforced here so
    // no later layer has to wonder what a carried record with a target means.
    if ((flags & WW_REC_AUTO_PASS) && (target != WW_NO_SEAT || (flags & WW_REC_HAS_CHAT)))
        return 0;
    return 1;
}

static int seed_is_zero(const uint8_t *seed) {
    for (int i = 0; i < 32; i++) if (seed[i]) return 0;
    return 1;
}

// A roster name must be printable and must not be empty. Not a nicety: the name
// is the only identity this wire carries, so a name made of control bytes is a
// seat nobody can be told apart from another.
static int name_is_clean(const char *s, int len) {
    if (len <= 0 || len > WW_NAME_MAX) return 0;
    for (int i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

int ww_msg_decode(const unsigned char *in, int in_len, WwEnvelope *out) {
    if (!in || in_len < WW_HDR_LEN) return WW_MSG_ESHORT;
    if (in[0] != WW_MAGIC) return WW_MSG_EMAGIC;
    if (in[1] != WW_FORMAT) return WW_MSG_EFORMAT;
    ww_envelope_init(out);
    out->format = in[1];
    out->flags = in[2];
    out->phase = in[3];
    out->game_id = rd64(in + 4);
    out->turn = rd16(in + 12);
    out->last_actor_seat = in[14];
    out->n_players = in[15];
    out->night = in[16];
    out->round = in[17];
    for (int i = 0; i < WW_PARENT_LEN; i++) out->parent8[i] = in[18 + i];
    for (int i = 0; i < 32; i++) out->seed[i] = in[26 + i];
    out->sent_at = rd16(in + 58);
    out->n_joins = in[60];

    if (out->flags != 0) return WW_MSG_EFIELD;          // reserved means reserved
    if (out->phase > WW_PHASE_OVER) return WW_MSG_EFIELD;
    if (out->night >= WW_MAX_NIGHTS) return WW_MSG_EFIELD;
    if (out->round > out->night + 1) return WW_MSG_EFIELD;
    if (out->n_joins > WW_MAX_JOINS) return WW_MSG_EFIELD;
    if (out->last_actor_seat != WW_NO_SEAT && out->last_actor_seat >= WW_MAX_PLAYERS)
        return WW_MSG_EFIELD;
    if (out->phase == WW_PHASE_LOBBY) {
        if (out->n_players > WW_MAX_PLAYERS) return WW_MSG_EFIELD;
    } else if (out->n_players < WW_MIN_PLAYERS || out->n_players > WW_MAX_PLAYERS) {
        return WW_MSG_EFIELD;
    }
    // The seed is checked even in a lobby: it is minted when the invite is
    // created, so an all-zero one is a bug or a forgery either way, and an
    // envelope that carries no deal is the one kind that silently plays a
    // different game on every device.
    if (seed_is_zero(out->seed)) return WW_MSG_ESEED;

    int q = WW_HDR_LEN;
    for (int i = 0; i < out->n_joins; i++) {
        if (q + 2 > in_len) return WW_MSG_ESHORT;
        const int seat = in[q], nl = in[q + 1];
        if (q + 2 + nl > in_len) return WW_MSG_ESHORT;
        if (seat >= WW_MAX_PLAYERS) return WW_MSG_EFIELD;
        if (!name_is_clean((const char *)(in + q + 2), nl)) return WW_MSG_EFIELD;
        out->joins[i].seat = (uint8_t)seat;
        out->joins[i].name_len = (uint8_t)nl;
        for (int j = 0; j < nl; j++) out->joins[i].name[j] = (char)in[q + 2 + j];
        q += 2 + nl;
    }

    if (q + 2 > in_len) return WW_MSG_ESHORT;
    out->n_records = rd16(in + q);
    q += 2;
    if (out->n_records > WW_MAX_RECORDS) return WW_MSG_EFIELD;
    out->body = in + q;
    out->body_len = in_len - q;

    // Walk the frames and prove the count AND the length exactly. Exactly, both
    // ways: a body with a trailing byte is as broken as a truncated one, and
    // tolerating either is how a decoder ends up reading a field that is really
    // somebody else's padding.
    int w = 0;
    for (int i = 0; i < out->n_records; i++) {
        const int n = ww_frame_len(out->body + w, out->body_len - w);
        if (n == 0) return WW_MSG_ESHORT;
        if (!frame_fields_ok(out->body + w)) return WW_MSG_EFIELD;
        w += n;
    }
    if (w != out->body_len) return WW_MSG_ESHORT;
    return WW_MSG_EOK;
}

int ww_msg_measure(const WwEnvelope *e) {
    int n = WW_HDR_LEN;
    for (int i = 0; i < e->n_joins; i++) n += 2 + e->joins[i].name_len;
    n += 2 + e->body_len;
    return n;
}

int ww_msg_encode(const WwEnvelope *e, unsigned char *out, int out_cap) {
    const int need = ww_msg_measure(e);
    if (need > out_cap) return WW_MSG_ESHORT;
    for (int i = 0; i < WW_HDR_LEN; i++) out[i] = 0;
    out[0] = WW_MAGIC;
    out[1] = WW_FORMAT;
    out[2] = e->flags;
    out[3] = e->phase;
    wr64(out + 4, e->game_id);
    wr16(out + 12, e->turn);
    out[14] = e->last_actor_seat;
    out[15] = e->n_players;
    out[16] = e->night;
    out[17] = e->round;
    for (int i = 0; i < WW_PARENT_LEN; i++) out[18 + i] = e->parent8[i];
    for (int i = 0; i < 32; i++) out[26 + i] = e->seed[i];
    wr16(out + 58, e->sent_at);
    out[60] = e->n_joins;
    int q = WW_HDR_LEN;
    for (int i = 0; i < e->n_joins; i++) {
        out[q++] = e->joins[i].seat;
        out[q++] = e->joins[i].name_len;
        for (int j = 0; j < e->joins[i].name_len; j++) out[q++] = (unsigned char)e->joins[i].name[j];
    }
    wr16(out + q, e->n_records); q += 2;
    for (int i = 0; i < e->body_len; i++) out[q + i] = e->body[i];
    return q + e->body_len;
}

int ww_msg_replay(const WwEnvelope *e, WwGame *g) {
    if (e->phase == WW_PHASE_LOBBY) {
        // A lobby has no deal to replay. It is still a legitimate chain - it is
        // how the roster fills - so it is not an error; it simply produces no
        // game, and the caller sees that in the phase.
        unsigned char *q = (unsigned char *)g;
        for (unsigned i = 0; i < sizeof *g; i++) q[i] = 0;
        g->phase = WW_PHASE_LOBBY;
        // ZERO SEATS, not the envelope's capacity. A lobby has no table, and a
        // game that claimed five players before anybody was dealt would let a
        // caller ask it for a role. The capacity stays on the envelope, where the
        // lobby screen reads it.
        g->n_players = 0;
        g->winner = WW_TEAM_NONE;
        return e->n_records == 0 ? WW_MSG_EOK : WW_MSG_EREPLAY;
    }
    if (ww_deal(g, e->seed, e->n_players) != WW_OK) return WW_MSG_EREPLAY;

    int w = 0;
    for (int i = 0; i < e->n_records; i++) {
        const unsigned char *p = e->body + w;
        const int n = ww_frame_len(p, e->body_len - w);
        if (n == 0) return WW_MSG_EREPLAY;
        const int seat = p[0], target = p[1], flags = p[3];
        int rc;
        if (flags & WW_REC_LYNCH) {
            rc = ww_day_lynch(g, target);
        } else if (flags & WW_REC_AUTO_PASS) {
            rc = ww_night_pass_for(g, seat);
        } else {
            const char *chat = (flags & WW_REC_HAS_CHAT) ? (const char *)(p + 5) : 0;
            const int chat_len = (flags & WW_REC_HAS_CHAT) ? p[4] : 0;
            rc = ww_night_act(g, seat, target, chat, chat_len, 0);
        }
        if (rc != WW_OK) return WW_MSG_EREPLAY;
        // Each record must land in the night it claims. The night a record gets
        // is the game's, not the frame's, so a frame that lies about it does not
        // corrupt the state - which is exactly why this has to be refused rather
        // than ignored. Two different byte strings that replay to the same game
        // make the DIGEST malleable, and the digest is Rule P's last tiebreak
        // and every envelope's parent link: somebody could mint a hundred
        // equivalent chains and send the one whose hash sorts first.
        if (g->rec[g->n_records - 1].night != p[2]) return WW_MSG_EREPLAY;
        w += n;
    }
    if (w != e->body_len) return WW_MSG_EREPLAY;
    // The header states the game the body produces. Disagreeing means one of the
    // two was edited, and a device that trusted the header would show a state
    // nobody ever played.
    //
    // `turn` IS THE SECURITY-RELEVANT ONE, and it is why this check earns its
    // place rather than being belt-and-braces. Rule P clause 2 prefers the chain
    // with the higher `turn`, and clause 2 is read off the HEADER, before
    // anything is replayed - that is what makes adoption cheap. So a bubble that
    // simply claimed turn 9000 would beat every honest chain in the thread
    // forever. This is the line that makes the claim worthless: a chain wins a
    // race on its header, but it is only adopted after it replays, and it only
    // replays if the header was telling the truth.
    if (g->turn != e->turn) return WW_MSG_EREPLAY;
    if (g->phase != e->phase || g->night != e->night) return WW_MSG_EREPLAY;
    return WW_MSG_EOK;
}

int ww_msg_seal(WwEnvelope *e, const WwGame *g, unsigned char *body, int body_cap) {
    e->format = WW_FORMAT;
    e->flags = 0;
    e->phase = g->phase;
    e->turn = g->turn;
    // IN A LOBBY, `n_players` IS THE CAPACITY and the caller owns it - there is no
    // table yet, so there is no table size to read off the game. The count that
    // becomes n_players is decided at Start by who actually joined (ww_lobby.h),
    // which is the whole reason there is no player-count picker.
    if (g->phase != WW_PHASE_LOBBY) e->n_players = g->n_players;
    e->night = g->night;
    // Resolved nights. A night is settled the moment it stops being the night in
    // play, which is what Rule P clause 1 wants: settled history outranks a
    // chain still arguing about tonight.
    e->round = (uint8_t)(g->night + (g->phase == WW_PHASE_NIGHT ? 0 : 1));
    e->n_records = g->n_records;

    int w = 0;
    for (int i = 0; i < g->n_records; i++) {
        const WwRecord *r = &g->rec[i];
        // THE PRUNE. Past nights' wolf lines are dropped: no rule reads them, so
        // carrying them would grow every bubble for the rest of the game to show
        // a channel that closed. Deterministic in `g`, so two devices sealing the
        // same state seal the same bytes.
        const int keep_chat = (r->flags & WW_REC_HAS_CHAT) && r->night == g->night;
        const int n = keep_chat ? 5 + r->chat_len : 4;
        if (w + n > body_cap) return WW_MSG_ESHORT;
        body[w + 0] = r->seat;
        body[w + 1] = r->target;
        body[w + 2] = r->night;
        body[w + 3] = (uint8_t)(keep_chat ? r->flags : (r->flags & ~WW_REC_HAS_CHAT));
        if (keep_chat) {
            body[w + 4] = r->chat_len;
            for (int j = 0; j < r->chat_len; j++) body[w + 5 + j] = (unsigned char)r->chat[j];
        }
        w += n;
    }
    e->body = body;
    e->body_len = w;
    return WW_MSG_EOK;
}

void ww_msg_digest(const unsigned char *envelope, int len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256(envelope, (size_t)len, out);
}

int ww_chain_key(const unsigned char *envelope, int len, WwChainKey *out) {
    WwEnvelope e;
    const int rc = ww_msg_decode(envelope, len, &e);
    if (rc != WW_MSG_EOK) return rc;
    out->phase = e.phase;
    out->round = e.round;
    out->turn = e.turn;
    out->n_joins = e.n_joins;
    for (int i = 0; i < WW_PARENT_LEN; i++) out->parent8[i] = e.parent8[i];
    ww_msg_digest(envelope, len, out->digest);
    return WW_MSG_EOK;
}

// Does `parent8` name `digest`? A plain byte walk rather than memcmp, because
// this file is meant to compile freestanding into the phone kernel with no libc
// beyond what clang lowers struct copies to.
static int names_parent(const uint8_t *parent8, const uint8_t *digest) {
    for (int i = 0; i < WW_PARENT_LEN; i++)
        if (parent8[i] != digest[i]) return 0;
    return 1;
}

int ww_rule_p(const WwChainKey *a, const WwChainKey *b) {
    // Ancestry first: a chain's own direct child outranks it, whatever the
    // counters say. In this product the counters happen to be monotone along a
    // chain (a record is never superseded, so `turn` only grows), which makes
    // this clause quiet today. It is kept because the fork learned the hard way
    // what happens when it is absent and the counters stop being monotone: a
    // parent delivered after its child sent the board backwards, and the bug
    // presented as an animation glitch for weeks. Any future role that retracts
    // or replaces a record reintroduces exactly that, and this clause is what
    // makes it a non-event.
    const int a_is_child = names_parent(a->parent8, b->digest);
    const int b_is_child = names_parent(b->parent8, a->digest);
    if (a_is_child != b_is_child) return a_is_child ? -1 : 1;
    // Clause 0: a dealt game outranks the invite it grew out of. Only the
    // boundary is compared, never OVER > DAY - round and turn already order
    // those correctly.
    const int sa = a->phase >= WW_PHASE_NIGHT, sb = b->phase >= WW_PHASE_NIGHT;
    if (sa != sb) return sa ? -1 : 1;
    // Clause 1: a resolved night is settled history.
    if (a->round != b->round) return a->round > b->round ? -1 : 1;
    // CLAUSE 2, the one the night leans on: MORE ACCEPTED RECORDS WINS. A player
    // who carried a late seat's pass and then made their own move has two
    // records where the seat they passed for has one, so the carrier's chain
    // wins - whichever bubble Messages happened to deliver last. That is what
    // makes the night unstallable: racing it always lengthens somebody's chain.
    if (a->turn != b->turn) return a->turn > b->turn ? -1 : 1;
    // Clause 3: the fuller roster is strictly later history. Below turn on
    // purpose, so a played-on chain is never clobbered by a stale wider invite.
    if (a->n_joins != b->n_joins) return a->n_joins > b->n_joins ? -1 : 1;
    // Clause 4: arbitrary, total, and identical on every device, which is all a
    // tiebreak has to be.
    for (int i = 0; i < SHA256_DIGEST_LEN; i++)
        if (a->digest[i] != b->digest[i]) return a->digest[i] < b->digest[i] ? -1 : 1;
    return 0;
}

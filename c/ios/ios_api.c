#include "ww_api.h"
#include "ww_game.h"
#include "ww_view.h"
#include "ww_wire.h"
#include "ww_seat.h"
#include "ww_lobby.h"

// The one resident session. Static, not malloc'd: an iMessage extension has no
// good moment to free anything and a fixed footprint is the only footprint that
// can be reasoned about against its memory ceiling.
static struct {
    WwGame     game;
    WwEnvelope env;            // the roster + identity the next seal will carry
    unsigned char body[2048];  // seal's scratch; e.body points into it
    int        have_game;
} S;

static void reset_env(void) {
    ww_envelope_init(&S.env);
    S.env.body = S.body;
    S.env.body_len = 0;
}

int wwi_new_game(const uint8_t *seed, int n_players) {
    if (!seed) return WW_ECOUNT;
    const int rc = ww_deal(&S.game, seed, n_players);
    if (rc != WW_OK) return rc;
    reset_env();
    for (int i = 0; i < 32; i++) S.env.seed[i] = seed[i];
    S.env.n_players = (uint8_t)n_players;
    S.have_game = 1;
    return WW_OK;
}

int wwi_create_lobby(const uint8_t *seed, int chat_is_dm,
                     const uint8_t *name, int name_len) {
    if (!seed) return WW_ECOUNT;
    // A LOBBY, NOT A GAME. The resident game is emptied to a lobby rather than
    // dealt, so there is nothing here for any accessor to read a role out of.
    unsigned char *q = (unsigned char *)&S.game;
    for (unsigned i = 0; i < sizeof S.game; i++) q[i] = 0;
    S.game.phase = WW_PHASE_LOBBY;
    S.game.winner = WW_TEAM_NONE;
    reset_env();
    S.env.phase = WW_PHASE_LOBBY;
    // A lobby's n_players is its CAPACITY - there is no table yet, and the count
    // that becomes a table is decided at Start by who joined.
    S.env.n_players = (uint8_t)ww_lobby_capacity(chat_is_dm);
    for (int i = 0; i < 32; i++) S.env.seed[i] = seed[i];
    S.have_game = 1;
    const int rc = wwi_roster_set(0, name, name_len);
    if (rc != WW_OK) return rc;
    S.env.last_actor_seat = 0;
    return WW_OK;
}

int wwi_lobby_capacity(int chat_is_dm) { return ww_lobby_capacity(chat_is_dm); }

int wwi_lobby_join(const uint8_t *name, int name_len) {
    if (S.env.phase != WW_PHASE_LOBBY) return WW_EPHASE;
    uint8_t claimed[WW_MAX_PLAYERS];
    for (int i = 0; i < S.env.n_joins; i++) claimed[i] = S.env.joins[i].seat;
    if (S.env.n_joins >= S.env.n_players) return WW_ECOUNT;     // no room in THIS chat
    const int seat = ww_lobby_free_seat(claimed, S.env.n_joins);
    if (seat == WW_NO_SEAT) return WW_ECOUNT;
    const int rc = wwi_roster_set(seat, name, name_len);
    if (rc != WW_OK) return rc;
    S.env.last_actor_seat = (uint8_t)seat;
    return seat;
}

int wwi_lobby_offered(int my_seat, int i_sent_the_newest) {
    return ww_lobby_offered(my_seat, S.env.n_joins, S.env.n_players, i_sent_the_newest);
}

int wwi_lobby_needs(void) { return ww_lobby_needs(S.env.n_joins); }
int wwi_lobby_can_exit(int my_seat) { return ww_lobby_can_exit(my_seat, S.env.n_joins); }
int wwi_lobby_joined(void) { return S.env.n_joins; }

int wwi_lobby_start(const uint8_t *lobby_payload, int len) {
    // RE-ADOPT FIRST, and this line is load-bearing rather than tidy. Without it
    // Start reseats whatever game happens to be resident and deals from its seed,
    // which is a different game with different roles and no error anywhere.
    const int rc = wwi_adopt(lobby_payload, len);
    if (rc != WW_MSG_EOK) return rc;
    if (S.env.phase != WW_PHASE_LOBBY) return WW_EPHASE;
    const int joined = S.env.n_joins;
    WwGame dealt;
    const int rd = ww_lobby_start(&dealt, S.env.seed, joined);
    if (rd != WW_OK) return rd;
    S.game = dealt;
    S.env.phase = WW_PHASE_NIGHT;
    S.env.n_players = (uint8_t)joined;
    return WW_OK;
}

int wwi_adopt(const uint8_t *payload, int len) {
    WwEnvelope e;
    const int rc = ww_msg_decode(payload, len, &e);
    if (rc != WW_MSG_EOK) return rc;
    // Replay into a scratch game and only then adopt. A half-applied chain is
    // the one state a device must never be left in: it would seal and send it.
    static WwGame scratch;
    const int rr = ww_msg_replay(&e, &scratch);
    if (rr != WW_MSG_EOK) return rr;
    S.game = scratch;
    // The roster and identity travel with the bubble, so they come from it.
    // `body` must not be borrowed from the caller's buffer past this call, so it
    // is re-pointed at our own scratch; the records themselves already live in
    // S.game.
    const WwEnvelope arrived = e;
    reset_env();
    S.env.game_id = arrived.game_id;
    for (int i = 0; i < 32; i++) S.env.seed[i] = arrived.seed[i];
    S.env.n_players = arrived.n_players;
    S.env.last_actor_seat = arrived.last_actor_seat;
    S.env.sent_at = arrived.sent_at;
    S.env.n_joins = arrived.n_joins;
    for (int i = 0; i < arrived.n_joins; i++) S.env.joins[i] = arrived.joins[i];
    S.have_game = 1;
    return WW_MSG_EOK;
}

int wwi_prefer(const uint8_t *a, int a_len, const uint8_t *b, int b_len) {
    WwChainKey ka, kb;
    const int oa = ww_chain_key(a, a_len, &ka) == WW_MSG_EOK;
    const int ob = ww_chain_key(b, b_len, &kb) == WW_MSG_EOK;
    // A payload that will not decode cannot be preferred over one that will, and
    // two that will not decode are not rankable - there is nothing in either to
    // prefer, and inventing an order would make the caller believe one is a game.
    if (oa != ob) return oa ? -1 : 1;
    if (!oa) return 0;
    return ww_rule_p(&ka, &kb);
}

int wwi_seal(uint8_t *out, int cap, uint64_t game_id, uint16_t sent_at,
             const uint8_t *parent, int parent_len) {
    if (!S.have_game) return WW_MSG_EREPLAY;
    S.env.game_id = game_id;
    S.env.sent_at = sent_at;
    // parent8 is computed here rather than in Swift: it is a commitment to the
    // exact bytes of the previous bubble, and a caller that hashes a
    // re-serialized copy of "the same" envelope commits to a different chain.
    for (int i = 0; i < WW_PARENT_LEN; i++) S.env.parent8[i] = 0;
    if (parent && parent_len > 0) {
        uint8_t d[SHA256_DIGEST_LEN];
        ww_msg_digest(parent, parent_len, d);
        for (int i = 0; i < WW_PARENT_LEN; i++) S.env.parent8[i] = d[i];
    }
    const int rc = ww_msg_seal(&S.env, &S.game, S.body, (int)sizeof S.body);
    if (rc != WW_MSG_EOK) return rc;
    return ww_msg_encode(&S.env, out, cap);
}

int wwi_roster_set(int seat, const uint8_t *name, int name_len) {
    if (seat < 0 || seat >= WW_MAX_PLAYERS) return WW_ESEAT;
    if (!name || name_len <= 0 || name_len > WW_NAME_MAX) return WW_ECOUNT;
    for (int i = 0; i < S.env.n_joins; i++) {
        if (S.env.joins[i].seat != (uint8_t)seat) continue;
        S.env.joins[i].name_len = (uint8_t)name_len;
        for (int j = 0; j < name_len; j++) S.env.joins[i].name[j] = (char)name[j];
        return WW_OK;
    }
    if (S.env.n_joins >= WW_MAX_JOINS) return WW_ECOUNT;
    WwJoin *j = &S.env.joins[S.env.n_joins++];
    j->seat = (uint8_t)seat;
    j->name_len = (uint8_t)name_len;
    for (int i = 0; i < name_len; i++) j->name[i] = (char)name[i];
    return WW_OK;
}

int wwi_roster_count(void) { return S.env.n_joins; }

int wwi_roster_seat(int i) {
    if (i < 0 || i >= S.env.n_joins) return WW_NO_SEAT;
    return S.env.joins[i].seat;
}

int wwi_roster_name(int i, uint8_t *out, int cap) {
    if (i < 0 || i >= S.env.n_joins) return -1;
    const int n = S.env.joins[i].name_len;
    if (n > cap) return -1;
    for (int j = 0; j < n; j++) out[j] = (uint8_t)S.env.joins[i].name[j];
    return n;
}

int wwi_seat_on_board(int cached_seat, int sender_is_local, int last_actor_seat,
                      int chat_is_dm, const uint8_t *name, int name_len) {
    return ww_seat_resolve_on_board(S.env.joins, S.env.n_joins, cached_seat,
                                    sender_is_local, S.env.n_players,
                                    last_actor_seat, chat_is_dm,
                                    (const char *)name, name_len);
}

int wwi_seat_in_lobby(int cached_seat, int sender_is_local, int last_actor_seat,
                      int chat_is_dm, const uint8_t *name, int name_len) {
    return ww_seat_resolve_in_lobby(S.env.joins, S.env.n_joins, cached_seat,
                                    sender_is_local, S.env.n_players,
                                    last_actor_seat, chat_is_dm,
                                    (const char *)name, name_len);
}

int wwi_name_taken(const uint8_t *name, int name_len) {
    return ww_name_taken(S.env.joins, S.env.n_joins, (const char *)name, name_len);
}

int wwi_night_act(int seat, int target, const uint8_t *chat, int chat_len, int carry) {
    if (!S.have_game) return WW_EPHASE;
    const int rc = ww_night_act(&S.game, seat, target,
                                (const char *)chat, chat ? chat_len : 0, carry);
    if (rc == WW_OK) S.env.last_actor_seat = (uint8_t)seat;
    return rc;
}

int wwi_day_lynch(int target) {
    if (!S.have_game) return WW_EPHASE;
    return ww_day_lynch(&S.game, target);
}

int wwi_send_floor_remaining(uint16_t opened_at, uint16_t now) {
    return ww_send_floor_remaining(opened_at, now);
}

int wwi_may_carry(uint16_t last_seal_at, uint16_t now) {
    return ww_may_carry(last_seal_at, now);
}

// ---------------------------------------------------------------- the view --
//
// A walker over ww_view_put's output. EVERY accessor below goes through this, so
// the masking has exactly one implementation and the bridge is its reader. A
// leak cannot be introduced in this file without first existing in ww_view.c,
// which is where the tests are.
typedef struct {
    unsigned char buf[WW_VIEW_MAX];
    int len;
    int off_roles, n_roles;
    int off_rows, n_rows;
    int off_own;                  // have_own, own_target, decider
    int off_chat, n_chat;
    int off_read, n_read;
    int off_hist, n_hist;
    int ok;
} View;

static View view_of(int viewer) {
    View v;
    v.ok = 0;
    v.len = 0;
    if (!S.have_game) return v;
    v.len = ww_view_put(&S.game, viewer, v.buf);
    int q = 11;                                   // past the fixed head
    v.n_roles = v.buf[q++]; v.off_roles = q; q += 2 * v.n_roles;
    v.n_rows  = v.buf[q++]; v.off_rows  = q; q += 2 * v.n_rows;
    v.off_own = q; q += 3;                        // have_own, own_target, decider
    v.n_chat  = v.buf[q++]; v.off_chat  = q;
    for (int i = 0; i < v.n_chat; i++) q += 2 + v.buf[q + 1];
    v.n_read  = v.buf[q++]; v.off_read  = q; q += 2 * v.n_read;
    v.n_hist  = v.buf[q++]; v.off_hist  = q; q += 3 * v.n_hist;
    v.ok = (q == v.len);
    return v;
}

int wwi_view_phase(int viewer)     { const View v = view_of(viewer); return v.len ? v.buf[1] : -1; }
int wwi_view_n_players(int viewer) { const View v = view_of(viewer); return v.len ? v.buf[2] : 0; }
int wwi_view_night(int viewer)     { const View v = view_of(viewer); return v.len ? v.buf[3] : -1; }
int wwi_view_winner(int viewer)    { const View v = view_of(viewer); return v.len ? v.buf[8] : WW_TEAM_NONE; }
int wwi_view_my_role(int viewer)   { const View v = view_of(viewer); return v.len ? v.buf[10] : WW_ROLE_UNKNOWN; }

int wwi_view_turn(int viewer) {
    const View v = view_of(viewer);
    return v.len ? (v.buf[6] | (v.buf[7] << 8)) : 0;
}

int wwi_view_alive(int viewer, int seat) {
    const View v = view_of(viewer);
    if (!v.len || seat < 0 || seat >= WW_MAX_PLAYERS) return 0;
    const int mask = v.buf[4] | (v.buf[5] << 8);
    return (mask >> seat) & 1;
}

int wwi_view_role_of(int viewer, int seat) {
    const View v = view_of(viewer);
    if (!v.ok) return WW_ROLE_UNKNOWN;
    for (int i = 0; i < v.n_roles; i++)
        if (v.buf[v.off_roles + 2 * i] == (unsigned char)seat)
            return v.buf[v.off_roles + 2 * i + 1];
    return WW_ROLE_UNKNOWN;
}

int wwi_view_sent(int viewer, int seat) {
    const View v = view_of(viewer);
    if (!v.ok) return WW_SENT_NO;
    for (int i = 0; i < v.n_rows; i++)
        if (v.buf[v.off_rows + 2 * i] == (unsigned char)seat)
            return v.buf[v.off_rows + 2 * i + 1];
    return WW_SENT_NO;
}

int wwi_view_own_target(int viewer) {
    const View v = view_of(viewer);
    if (!v.ok || !v.buf[v.off_own]) return WW_NO_SEAT;
    return v.buf[v.off_own + 1];
}

int wwi_view_decider(int viewer) {
    const View v = view_of(viewer);
    return v.ok ? v.buf[v.off_own + 2] : WW_NO_SEAT;
}

int wwi_view_chat_count(int viewer) {
    const View v = view_of(viewer);
    return v.ok ? v.n_chat : 0;
}

// The i'th channel row's offset inside the blob, or -1. Walked rather than
// indexed because the rows are variable length.
static int chat_row(const View *v, int i) {
    if (!v->ok || i < 0 || i >= v->n_chat) return -1;
    int q = v->off_chat;
    for (int k = 0; k < i; k++) q += 2 + v->buf[q + 1];
    return q;
}

int wwi_view_chat_seat(int viewer, int i) {
    const View v = view_of(viewer);
    const int q = chat_row(&v, i);
    return q < 0 ? WW_NO_SEAT : v.buf[q];
}

int wwi_view_chat_line(int viewer, int i, uint8_t *out, int cap) {
    const View v = view_of(viewer);
    const int q = chat_row(&v, i);
    if (q < 0) return -1;
    const int n = v.buf[q + 1];
    if (n > cap) return -1;
    for (int j = 0; j < n; j++) out[j] = v.buf[q + 2 + j];
    return n;
}

int wwi_view_reading(int viewer, int seat) {
    const View v = view_of(viewer);
    if (!v.ok) return WW_TEAM_NONE;
    // The LAST reading of that seat, because the seer may legitimately ask twice
    // and the newer answer is the one that still stands (a role can change hands
    // in no version of this game, but a re-ask must not read as two answers).
    int team = WW_TEAM_NONE;
    for (int i = 0; i < v.n_read; i++)
        if (v.buf[v.off_read + 2 * i] == (unsigned char)seat)
            team = v.buf[v.off_read + 2 * i + 1];
    return team;
}

int wwi_view_victim(int viewer, int night) {
    const View v = view_of(viewer);
    if (!v.ok || night < 0 || night >= v.n_hist) return WW_NO_SEAT;
    return v.buf[v.off_hist + 3 * night + 1];
}

int wwi_view_lynched(int viewer, int night) {
    const View v = view_of(viewer);
    if (!v.ok || night < 0 || night >= v.n_hist) return WW_NO_SEAT;
    return v.buf[v.off_hist + 3 * night + 2];
}

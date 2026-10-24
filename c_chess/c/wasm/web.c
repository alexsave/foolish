/* web.c - the browser's kernel: the client (client.h) and the rules, as wasm exports the page calls.
 * One client per page. The page moves bytes and draws: io_ptr() is a buffer it fills with what the
 * socket or a fetch received before calling in a function, and reads what to send from after one. */
#include "client.h"
#include "web.h"

#include <string.h>

#define EXPORT(name) __attribute__((export_name(#name)))
#define IO_CAP 65536

static cc_client C;
static uint8_t IO[IO_CAP];
static CvView V;
static CvMoves M;
static CvLobby L;
static CvProfile P;
static CvGames G;
static CvTv T;
static CvTop TOP;

EXPORT(cv_layout_hash) uint32_t cv_layout_hash(void) { return SG_LAYOUT_HASH; }
EXPORT(io_ptr) uint8_t *io_ptr(void) { return IO; }
EXPORT(io_cap) uint32_t io_cap(void) { return IO_CAP; }

EXPORT(cv_init) void cv_init(void) { cc_client_init(&C); }

/* ---- the account ---- */
/* io holds name then password (n1 + n2 bytes): writes the POST /c/auth body into io, returns its length. */
EXPORT(cv_auth_body) uint32_t cv_auth_body(uint32_t n1, uint32_t n2, int mode) {
    char name[CW_NAME + 1], pw[CW_PASSWORD + 1];
    if (n1 > CW_NAME) n1 = CW_NAME;
    if (n2 > CW_PASSWORD) n2 = CW_PASSWORD;
    memcpy(name, IO, n1); name[n1] = 0;
    memcpy(pw, IO + n1, n2); pw[n2] = 0;
    cw_auth_req q;
    cc_client_auth_body(name, pw, mode, &q);
    memcpy(IO, &q, sizeof q);
    memset(pw, 0, sizeof pw);
    return sizeof q;
}
EXPORT(cv_refresh_body) uint32_t cv_refresh_body(void) {
    cw_refresh_req q;
    cc_client_refresh_body(&C, &q);
    memcpy(IO, &q, sizeof q);
    return sizeof q;
}
/* io holds the answer's n bytes. */
EXPORT(cv_auth_answer) void cv_auth_answer(int status, uint32_t n) { cc_client_auth_answer(&C, status, IO, n); }
EXPORT(cv_hello) void cv_hello(void) { cc_client_hello(&C); }

/* A new guest from the random bytes in io (at least 32): the record into io, its length. */
EXPORT(cv_guest_new) uint32_t cv_guest_new(void) {
    static const char abc[] = "abcdefghijkmnpqrstuvwxyz23456789";
    CvGuest g;
    memset(&g, 0, sizeof g);
    memcpy(g.name, "guest-", 6);
    for (int i = 0; i < 6; i++) g.name[6 + i] = abc[IO[i] % 32];
    for (int i = 0; i < 20; i++) g.password[i] = abc[IO[6 + i] % 32];
    memcpy(IO, &g, sizeof g);
    return sizeof g;
}
EXPORT(cv_guest_size) uint32_t cv_guest_size(void) { return sizeof(CvGuest); }
/* io holds a guest record: the POST /c/auth body for it into io, its length. */
EXPORT(cv_guest_auth_body) uint32_t cv_guest_auth_body(int mode) {
    CvGuest g;
    memcpy(&g, IO, sizeof g);
    g.name[CV_NAME - 1] = 0;
    g.password[CV_GUEST_PW - 1] = 0;
    cw_auth_req q;
    cc_client_auth_body(g.name, g.password, mode, &q);
    memcpy(IO, &q, sizeof q);
    memset(&g, 0, sizeof g);
    return sizeof q;
}

/* A GET over the socket; the answer comes as CC_EV_READ (cv_read_take). io holds the path. */
EXPORT(cv_read) void cv_read(uint32_t path_len, uint32_t tag) {
    char path[300];
    if (path_len >= sizeof path) return;
    memcpy(path, IO, path_len);
    path[path_len] = 0;
    cc_client_read(&C, path, tag);
}
/* The last read's answer into io: its length. */
EXPORT(cv_read_take) uint32_t cv_read_take(void) {
    memcpy(IO, C.read_buf, C.read_len);
    return C.read_len;
}

/* ---- the socket ---- */
EXPORT(cv_in) void cv_in(uint32_t n, double now) { cc_client_in(&C, IO, n, (int64_t)now); }
/* The next frame to send, copied into io: its length, 0 when none. */
EXPORT(cv_out) uint32_t cv_out(void) {
    const uint8_t *p;
    size_t n = cc_client_next_out(&C, &p);
    if (n > IO_CAP) return 0;
    memmove(IO, p, n);
    return (uint32_t)n;
}

/* ---- requests ---- */
EXPORT(cv_seek) void cv_seek(int base_s, int inc_s, int rated) { cc_client_seek(&C, base_s, inc_s, rated); }
EXPORT(cv_unseek) void cv_unseek(void) { cc_client_unseek(&C); }
EXPORT(cv_challenge) void cv_challenge(int base_s, int inc_s, int rated) { cc_client_challenge(&C, base_s, inc_s, rated); }
EXPORT(cv_accept) void cv_accept(uint32_t id) { cc_client_accept(&C, id); }
EXPORT(cv_ai) void cv_ai(int base_s, int inc_s, int level, int color) { cc_client_ai(&C, base_s, inc_s, level, color); }
EXPORT(cv_lobby) void cv_lobby(void) { cc_client_lobby(&C); }
EXPORT(cv_lobby_leave) void cv_lobby_leave(void) { cc_client_lobby_leave(&C); }
EXPORT(cv_open) void cv_open(uint32_t game) { cc_client_open(&C, game); }
EXPORT(cv_close) void cv_close(void) { cc_client_close_game(&C); }
EXPORT(cv_move) int cv_move(int from, int to, int promo, double now) { return cc_client_move(&C, from, to, promo, (int64_t)now); }
EXPORT(cv_act) void cv_act(int act) { cc_client_act(&C, act); }

/* ---- what to draw ---- */
EXPORT(cv_targets_lo) uint32_t cv_targets_lo(int from) { return (uint32_t)cc_client_targets(&C, from); }
EXPORT(cv_targets_hi) uint32_t cv_targets_hi(int from) { return (uint32_t)(cc_client_targets(&C, from) >> 32); }
EXPORT(cv_premove) int cv_premove(int from, int to, int promo) { return cc_client_premove(&C, from, to, promo); }
EXPORT(cv_premove_cancel) void cv_premove_cancel(void) { cc_client_premove_cancel(&C); }
EXPORT(cv_premove_targets_lo) uint32_t cv_premove_targets_lo(int from) { return (uint32_t)cc_client_premove_targets(&C, from); }
EXPORT(cv_premove_targets_hi) uint32_t cv_premove_targets_hi(int from) { return (uint32_t)(cc_client_premove_targets(&C, from) >> 32); }
EXPORT(cv_is_promotion) int cv_is_promotion(int from, int to) { return cc_client_is_promotion(&C, from, to); }

static void copy_name(char *dst, const char *src) {
    memset(dst, 0, CV_NAME);
    size_t n = strlen(src);
    memcpy(dst, src, n < CV_NAME - 1 ? n : CV_NAME - 1);
}

EXPORT(cv_view) const CvView *cv_view(double now) {
    V.events = cc_client_events(&C);
    V.game = C.game;
    V.color = C.color;
    V.synced = C.synced;
    V.ready = C.ready;
    V.status = C.g.status;
    V.winner = C.g.winner;
    V.stm = C.g.pos.stm;
    V.my_turn = (uint8_t)cc_client_my_turn(&C);
    V.clock_running = C.clock_running;
    V.draw_offer = C.g.draw_offer;
    V.rematch_offer = C.rematch_offer;
    V.premove_from = C.premove ? (uint8_t)CH_FROM(C.premove) : CV_NO_SQ;
    V.premove_to = C.premove ? (uint8_t)CH_TO(C.premove) : CV_NO_SQ;
    V.ply = C.g.ply;
    V.base_s = C.g.base_ms / 1000 > 0xffff ? 0xffff : (uint16_t)(C.g.base_ms / 1000);
    V.inc_s = (uint8_t)(C.g.inc_ms / 1000);
    V.rated = C.base_rated[1];
    V.last_from = V.last_to = CV_NO_SQ;
    if (C.g.ply) { V.last_from = (uint8_t)CH_FROM(C.g.moves[C.g.ply - 1]); V.last_to = (uint8_t)CH_TO(C.g.moves[C.g.ply - 1]); }
    V.check_sq = CV_NO_SQ;
    if (ch_in_check(&C.g.pos)) {
        uint64_t k = C.g.pos.bb[C.g.pos.stm][CH_KING];
        if (k) V.check_sq = (uint8_t)__builtin_ctzll(k);
    }
    for (int c = 0; c < 2; c++) {
        V.clock_ms[c] = (uint32_t)cc_client_clock(&C, c, (int64_t)now);
        V.rating[c] = C.rating[c];
        V.diff[c] = C.diff[c];
        copy_name(c ? V.black : V.white, C.names[c]);
    }
    V.next_game = C.next_game;
    V.start_game = C.start_game;
    V.start_color = C.start_color;
    V.challenge_id = C.challenge_id;
    V.read_tag = C.read_tag;
    V.read_status = C.read_status;
    V.err_status = C.err_status;
    V.err_code = C.err.code;
    V.err_rc = C.err.rc;
    V.auth_status = C.auth_status;
    V.auth_code = C.auth_code;
    copy_name(V.me, C.name);
    for (int s = 0; s < 64; s++) {
        int col, t = ch_piece_at(&C.g.pos, s, &col);
        V.board[s] = t < 0 ? 0 : (uint8_t)(1 + t + 8 * col);
    }
    return &V;
}

EXPORT(cv_moves) const CvMoves *cv_moves(void) {
    M.n = C.g.ply;
    for (int i = 0; i < C.g.ply && i < CV_PLY; i++) memcpy(M.moves[i].san, C.san[i], 8);
    return &M;
}

EXPORT(cv_lobby_view) const CvLobby *cv_lobby_view(void) {
    L.n = C.lobby.n > CV_SEEKS ? CV_SEEKS : C.lobby.n;
    L.games = C.lobby.games;
    L.players = C.lobby.players;
    for (int i = 0; i < L.n; i++) {
        const cw_seek_row *r = &C.lobby.seeks[i];
        L.seeks[i].id = r->id;
        L.seeks[i].base_s = r->base_s;
        L.seeks[i].inc_s = r->inc_s;
        L.seeks[i].rated = r->rated;
        L.seeks[i].rating = r->rating;
        memset(L.seeks[i].name, 0, CV_NAME);
        size_t k = 0;
        while (k < CW_NAME && r->name[k]) k++;
        memcpy(L.seeks[i].name, r->name, k);
    }
    return &L;
}

/* The board at an earlier ply (the replay's back and forward): fills the view's board only. */
EXPORT(cv_board_at) const CvView *cv_board_at(int ply) {
    chess_pos p, q;
    ch_startpos(&p);
    if (ply > C.g.ply) ply = C.g.ply;
    for (int i = 0; i < ply; i++) { ch_make(&p, C.g.moves[i], &q); p = q; }
    for (int s = 0; s < 64; s++) {
        int col, t = ch_piece_at(&p, s, &col);
        V.board[s] = t < 0 ? 0 : (uint8_t)(1 + t + 8 * col);
    }
    V.last_from = V.last_to = CV_NO_SQ;
    if (ply) { V.last_from = (uint8_t)CH_FROM(C.g.moves[ply - 1]); V.last_to = (uint8_t)CH_TO(C.g.moves[ply - 1]); }
    V.check_sq = CV_NO_SQ;
    if (ch_in_check(&p)) V.check_sq = (uint8_t)__builtin_ctzll(p.bb[p.stm][CH_KING]);
    return &V;
}

static void copy_fixed(char *dst, const char *src, size_t n) {
    memset(dst, 0, CV_NAME);
    size_t k = 0;
    while (k < n && k < CV_NAME - 1 && src[k]) k++;
    memcpy(dst, src, k);
}

/* io holds GET /c/user/<name>'s n bytes (status 200). */
EXPORT(cv_profile) const CvProfile *cv_profile(uint32_t n) {
    memset(&P, 0, sizeof P);
    cw_profile pr;
    if (n < sizeof pr) return &P;
    memcpy(&pr, IO, sizeof pr);
    if (n != sizeof pr + (size_t)pr.n_points * sizeof(cw_point)) return &P;
    P.ok = 1;
    P.rating = pr.rating;
    P.rd = pr.rd;
    P.games = pr.games; P.wins = pr.wins; P.losses = pr.losses; P.draws = pr.draws;
    copy_fixed(P.name, pr.name, CW_NAME);
    P.n = pr.n_points > CV_POINTS ? CV_POINTS : pr.n_points;
    for (int i = 0; i < P.n; i++) {
        cw_point pt;
        memcpy(&pt, IO + sizeof pr + (size_t)i * sizeof pt, sizeof pt);
        P.points[i].at = (double)pt.at;
        P.points[i].rating = pt.rating;
    }
    return &P;
}

/* io holds GET /c/games/<name>'s n bytes. */
EXPORT(cv_games) const CvGames *cv_games(uint32_t n) {
    memset(&G, 0, sizeof G);
    if (n < 8) return &G;
    uint16_t cnt;
    memcpy(&cnt, IO, 2);
    if (cnt > CW_LIST_MAX || n != 8 + (size_t)cnt * sizeof(cw_game_row)) return &G;
    G.n = cnt;
    for (int i = 0; i < cnt; i++) {
        cw_game_row r;
        memcpy(&r, IO + 8 + (size_t)i * sizeof r, sizeof r);
        CvGameRow *o = &G.rows[i];
        o->game = r.game; o->status = r.status; o->winner = r.winner; o->color = r.color; o->rated = r.rated;
        o->base_s = r.base_s; o->ply = r.ply; o->inc_s = r.inc_s; o->diff = r.diff; o->ended_at = (double)r.ended_at;
        copy_fixed(o->opponent, r.opponent, CW_NAME);
    }
    return &G;
}

EXPORT(cv_tv) const CvTv *cv_tv(double now) {
    T.n = C.tv.n > CV_TV ? CV_TV : C.tv.n;
    for (int i = 0; i < T.n; i++) {
        const cw_tv_game *g = &C.tv.games[i];
        CvTvGame *o = &T.games[i];
        o->game = g->game; o->ply = g->ply; o->base_s = g->base_s; o->inc_s = g->inc_s; o->status = g->status; o->stm = g->stm;
        o->clock_running = g->clock_running; o->last_from = g->last_from; o->last_to = g->last_to;
        for (int c = 0; c < 2; c++) {
            o->rating[c] = g->rating[c];
            int64_t t = g->left_ms[c];
            if (g->clock_running && g->stm == c) t -= (int64_t)now - C.tv_at;
            o->clock_ms[c] = t < 0 ? 0 : (uint32_t)t;
        }
        copy_fixed(o->white, g->name[0], CW_NAME);
        copy_fixed(o->black, g->name[1], CW_NAME);
        for (int sq = 0; sq < 64; sq++) o->board[sq] = (uint8_t)((g->board[sq / 2] >> (sq & 1 ? 4 : 0)) & 15);
    }
    return &T;
}

/* io holds GET /c/top's n bytes. */
EXPORT(cv_top) const CvTop *cv_top(uint32_t n) {
    memset(&TOP, 0, sizeof TOP);
    if (n < 8) return &TOP;
    uint16_t cnt;
    memcpy(&cnt, IO, 2);
    if (cnt > CW_TOP_MAX || n != 8 + (size_t)cnt * sizeof(cw_top_row)) return &TOP;
    TOP.n = cnt;
    for (int i = 0; i < cnt; i++) {
        cw_top_row r;
        memcpy(&r, IO + 8 + (size_t)i * sizeof r, sizeof r);
        copy_fixed(TOP.rows[i].name, r.name, CW_NAME);
        TOP.rows[i].rating = r.rating;
        TOP.rows[i].rd = r.rd;
        TOP.rows[i].games = r.games;
    }
    return &TOP;
}

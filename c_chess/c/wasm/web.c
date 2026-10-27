/* web.c - the browser's kernel: the client (client.h) and the rules, as wasm exports the page calls.
 * One client per page. The page moves bytes and draws: io_ptr() is a buffer it fills with what the
 * socket or a fetch received before calling in a function, and reads what to send from after one. */
#include "client.h"
#include "chat.h"
#include "puzzle.h"
#include "rating.h"
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
    V.arena = C.game_arena;
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

/* A time control's speed (speed.h): which rating a game of it moves. */
EXPORT(cv_speed) int cv_speed(int base_s, int inc_s) { return cc_speed(base_s, inc_s); }

static uint8_t provisional(unsigned rd) { return rd >= CC_PROVISIONAL_RD; }

/* io holds GET /c/user/<name>'s n bytes (status 200). The points come by speed, so each speed's
 * are a run: from[] and count[] say where. */
EXPORT(cv_profile) const CvProfile *cv_profile(uint32_t n) {
    memset(&P, 0, sizeof P);
    cw_profile pr;
    if (n < sizeof pr) return &P;
    memcpy(&pr, IO, sizeof pr);
    if (pr.n_points > CV_POINTS || n != sizeof pr + (size_t)pr.n_points * sizeof(cw_point)) return &P;
    P.ok = 1;
    for (int k = 0; k < CC_SPEEDS; k++) {
        P.rating[k] = pr.rating[k];
        P.rd[k] = pr.rd[k];
        P.rated[k] = pr.rated[k];
        P.provisional[k] = provisional(pr.rd[k]);
    }
    P.puzzle = pr.puzzle;
    P.puzzle_rd = pr.puzzle_rd;
    P.puzzle_provisional = provisional(pr.puzzle_rd);
    P.puzzles_tried = pr.puzzles_tried;
    P.puzzles_solved = pr.puzzles_solved;
    P.games = pr.games; P.wins = pr.wins; P.losses = pr.losses; P.draws = pr.draws;
    copy_fixed(P.name, pr.name, CW_NAME);
    P.n = pr.n_points;
    int last = -1;
    for (int i = 0; i < P.n; i++) {
        cw_point pt;
        memcpy(&pt, IO + sizeof pr + (size_t)i * sizeof pt, sizeof pt);
        if (pt.speed >= CC_SPEEDS || pt.speed < last) { memset(&P, 0, sizeof P); return &P; }   /* not by speed */
        if (pt.speed != last) P.from[pt.speed] = (uint16_t)i;
        last = pt.speed;
        P.count[pt.speed]++;
        P.points[i].at = (double)pt.at;
        P.points[i].rating = pt.rating;
        P.points[i].speed = pt.speed;
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

/* io holds GET /c/top/<speed>'s n bytes. */
EXPORT(cv_top) const CvTop *cv_top(uint32_t n) {
    memset(&TOP, 0, sizeof TOP);
    if (n < 8) return &TOP;
    uint16_t cnt;
    memcpy(&cnt, IO, 2);
    if (cnt > CW_TOP_MAX || n != 8 + (size_t)cnt * sizeof(cw_top_row)) return &TOP;
    TOP.n = cnt;
    TOP.speed = IO[2];
    for (int i = 0; i < cnt; i++) {
        cw_top_row r;
        memcpy(&r, IO + 8 + (size_t)i * sizeof r, sizeof r);
        copy_fixed(TOP.rows[i].name, r.name, CW_NAME);
        TOP.rows[i].rating = r.rating;
        TOP.rows[i].rd = r.rd;
        TOP.rows[i].games = r.games;
        TOP.rows[i].provisional = provisional(r.rd);
    }
    return &TOP;
}

/* ---- arenas ---- */

static CvArena AR;
static CvArenaList AL;

static void copy_n(char *dst, size_t cap, const char *src, size_t n) {
    memset(dst, 0, cap);
    size_t k = 0;
    while (k < n && k < cap - 1 && src[k]) k++;
    memcpy(dst, src, k);
}

static int32_t clamp_ms(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < -INT32_MAX ? -INT32_MAX : (int32_t)v; }

/* The arena shown, its countdowns as of now (the page's clock). */
EXPORT(cv_arena) const CvArena *cv_arena(double now) {
    memset(&AR, 0, sizeof AR);
    if (C.arena_len < sizeof(cw_arena_hdr)) return &AR;
    cw_arena_hdr h;
    memcpy(&h, C.arena, sizeof h);
    AR.ok = 1;
    AR.status = h.status; AR.speed = h.speed; AR.rated = h.rated; AR.id = h.id;
    AR.base_s = h.base_s; AR.inc_s = h.inc_s; AR.me_in = h.me_in;
    AR.n_players = h.n_players; AR.n = h.n_rows; AR.playing = h.playing; AR.games = h.games;
    AR.me_rank = h.me_rank; AR.me_score = h.me_score; AR.me_games = h.me_games; AR.me_fire = h.me_fire;
    int64_t since = (int64_t)now - C.arena_at;
    AR.starts_in_ms = clamp_ms(h.starts_at - h.now - since);
    AR.ends_in_ms = clamp_ms(h.ends_at - h.now - since);
    AR.starts_at = (double)h.starts_at;
    AR.ends_at = (double)h.ends_at;
    copy_n(AR.name, CV_ARENA_NAME, h.name, CW_ARENA_NAME);
    copy_n(AR.by, CV_NAME, h.by, CW_NAME);
    for (int i = 0; i < h.n_rows && i < CV_ARENA_ROWS; i++) {
        cw_arena_row r;
        memcpy(&r, C.arena + sizeof h + (size_t)i * sizeof r, sizeof r);
        CvArenaRow *o = &AR.rows[i];
        copy_n(o->name, CV_NAME, r.name, CW_NAME);
        o->score = r.score; o->games = r.games; o->rank = r.rank; o->rating = r.rating;
        o->fire = r.fire; o->withdrawn = r.withdrawn; o->playing = r.playing;
        o->me = C.name[0] && strcmp(o->name, C.name) == 0;
        if (o->me) AR.me_listed = 1;
    }
    return &AR;
}

/* io holds the answer of GET /c/arena/<id>, a join or a withdrawal (n bytes): the arena, or ok 0. */
EXPORT(cv_arena_read) const CvArena *cv_arena_read(uint32_t n, double now) {
    if (cc_client_arena_take(&C, IO, n, (int64_t)now, 1)) return cv_arena(now);
    memset(&AR, 0, sizeof AR);
    if (n == sizeof(cw_err)) AR.err = IO[0];   /* a refusal: cw_err, its code first */
    return &AR;
}

static void arena_room(uint32_t id, char *out) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + id % 10); id /= 10; } while (id);
    *out++ = 'a';
    while (n) *out++ = t[--n];
    *out = 0;
}
EXPORT(cv_arena_watch) void cv_arena_watch(uint32_t id) { char r[16]; arena_room(id, r); cc_client_watch(&C, r); }
EXPORT(cv_arena_unwatch) void cv_arena_unwatch(uint32_t id) { char r[16]; arena_room(id, r); cc_client_unwatch(&C, r); }
EXPORT(cv_arena_join) void cv_arena_join(uint32_t id, uint32_t tag) { cw_id q = { id }; cc_client_post(&C, "/c/arena/join", &q, sizeof q, tag); }
EXPORT(cv_arena_withdraw) void cv_arena_withdraw(uint32_t id, uint32_t tag) { cw_id q = { id }; cc_client_post(&C, "/c/arena/withdraw", &q, sizeof q, tag); }

/* io holds a name of n bytes: whether an arena may have it (arena.h). */
EXPORT(cv_arena_name_ok) int cv_arena_name_ok(uint32_t n) { return n <= CW_ARENA_NAME && cc_arena_name_ok((const char *)IO, n); }

/* io holds the name (n bytes): POST /c/arena, answered as CC_EV_READ with this tag (then cv_arena_read). */
EXPORT(cv_arena_create) void cv_arena_create(int base_s, int inc_s, int rated, uint32_t start_in_s, uint32_t duration_s, uint32_t n, uint32_t tag) {
    cw_arena_req q;
    memset(&q, 0, sizeof q);
    q.base_s = (uint16_t)base_s;
    q.inc_s = (uint8_t)inc_s;
    q.rated = (uint8_t)(rated != 0);
    q.start_in_s = start_in_s;
    q.duration_s = duration_s;
    memcpy(q.name, IO, n < CW_ARENA_NAME ? n : CW_ARENA_NAME);
    cc_client_post(&C, "/c/arena", &q, sizeof q, tag);
}

/* io holds GET /c/arenas's n bytes. */
EXPORT(cv_arena_list) const CvArenaList *cv_arena_list(uint32_t n) {
    memset(&AL, 0, sizeof AL);
    if (n < 16) return &AL;
    uint16_t cnt;
    int64_t now;
    memcpy(&cnt, IO, 2);
    memcpy(&now, IO + 8, 8);
    if (cnt > CW_ARENAS_MAX || cnt > CV_ARENAS || n != 16 + (size_t)cnt * sizeof(cw_arena_item)) return &AL;
    AL.n = cnt;
    for (int i = 0; i < cnt; i++) {
        cw_arena_item r;
        memcpy(&r, IO + 16 + (size_t)i * sizeof r, sizeof r);
        CvArenaItem *o = &AL.rows[i];
        o->id = r.id; o->status = r.status; o->speed = r.speed; o->inc_s = r.inc_s; o->rated = r.rated;
        o->base_s = r.base_s; o->n_players = r.n_players;
        o->starts_in_ms = clamp_ms(r.starts_at - now);
        o->ends_in_ms = clamp_ms(r.ends_at - now);
        o->starts_at = (double)r.starts_at;
        o->ends_at = (double)r.ends_at;
        copy_n(o->name, CV_ARENA_NAME, r.name, CW_ARENA_NAME);
        copy_n(o->winner, CV_NAME, r.winner, CW_NAME);
    }
    return &AL;
}

static CvEval EV;

EXPORT(cv_eval_ask) void cv_eval_ask(int ply) { cc_client_eval(&C, ply); }

/* The last answer, its line written out in SAN from its position. */
EXPORT(cv_eval) const CvEval *cv_eval(void) {
    memset(&EV, 0, sizeof EV);
    const cw_eval *e = &C.eval;
    EV.ply = e->ply;
    EV.score = e->score;
    EV.mate = e->mate;
    EV.depth = e->depth;
    EV.best_from = EV.best_to = CV_NO_SQ;
    if (e->v != CW_V || e->ply > C.g.ply) return &EV;
    chess_pos p, q;
    ch_startpos(&p);
    for (int i = 0; i < e->ply; i++) { ch_make(&p, C.g.moves[i], &q); p = q; }
    if (p.key != e->key) return &EV;   /* an answer about another game's position */
    size_t o = 0;
    for (int i = 0; i < e->npv && i < CW_PV_MAX; i++) {
        ch_move m = e->pv[i];
        if (!ch_is_legal(&p, m)) break;
        if (i == 0) { EV.best_from = (uint8_t)CH_FROM(m); EV.best_to = (uint8_t)CH_TO(m); }
        char san[8];
        int n = ch_san(&p, m, san);
        if (o + (size_t)n + 2 >= sizeof EV.line) break;
        if (o) EV.line[o++] = ' ';
        memcpy(EV.line + o, san, (size_t)n);
        o += (size_t)n;
        ch_make(&p, m, &q);
        p = q;
    }
    return &EV;
}

/* ---- puzzles: solved here, on the kernel's board; the server hands them out and rates them ---- */

static cp_solve PS;
static CvPuzzle PV;

/* The view of a position: the board, the last move, the check. */
static void puzzle_fill_pos(const chess_pos *pos, ch_move last) {
    PV.state = (uint8_t)PS.state;
    PV.at = (uint8_t)PS.at;
    PV.n = PS.p.n;
    if (last != CH_MOVE_NONE) { PV.last_from = (uint8_t)CH_FROM(last); PV.last_to = (uint8_t)CH_TO(last); }
    PV.hint_from = PS.at >= 1 && PS.at < PS.p.n ? (uint8_t)CH_FROM(PS.p.moves[PS.at]) : CV_NO_SQ;
    PV.check_sq = CV_NO_SQ;
    if (ch_in_check(pos)) {
        uint64_t k = pos->bb[pos->stm][CH_KING];
        if (k) PV.check_sq = (uint8_t)__builtin_ctzll(k);
    }
    for (int sq = 0; sq < 64; sq++) {
        int col, t = ch_piece_at(pos, sq, &col);
        PV.board[sq] = t < 0 ? 0 : (uint8_t)(1 + t + 8 * col);
    }
}
static void puzzle_fill(ch_move last) { puzzle_fill_pos(&PS.pos, last); }

/* Asks for the next puzzle (GET /c/puzzle/next): CC_EV_READ with this tag, then cv_puzzle_load. */
EXPORT(cv_puzzle_ask) void cv_puzzle_ask(uint32_t tag) { cc_client_read(&C, "/c/puzzle/next", tag); }

/* io holds the answer's n bytes (cw_puzzle). */
EXPORT(cv_puzzle_load) const CvPuzzle *cv_puzzle_load(uint32_t n) {
    memset(&PV, 0, sizeof PV);
    PV.last_from = PV.last_to = PV.check_sq = PV.hint_from = CV_NO_SQ;
    cw_puzzle w;
    if (n != sizeof w) return &PV;
    memcpy(&w, IO, sizeof w);
    if (cp_begin(&PS, &w.p) != 0) return &PV;
    PV.ok = 1;
    PV.id = w.p.id;
    PV.themes = w.p.themes;
    PV.rating = w.rating; PV.rd = w.rd;
    PV.puzzle_rating = w.p.rating; PV.puzzle_rd = w.p.rd;
    PV.tried = w.tried; PV.solved = w.solved; PV.plays = w.plays;
    PV.color = (uint8_t)PS.pos.stm;
    PV.wrong_sq = CV_NO_SQ;
    chess_pos before;
    ch_from_fen(&before, w.p.fen);   /* cp_begin read it: it is good */
    puzzle_fill_pos(&before, CH_MOVE_NONE);
    PV.intro = 1;
    return &PV;
}

/* The opponent's move that sets the puzzle, drawn after the position it was played in. */
EXPORT(cv_puzzle_begin) const CvPuzzle *cv_puzzle_begin(void) {
    if (PV.intro) { PV.intro = 0; puzzle_fill(PS.p.moves[0]); }
    return &PV;
}

EXPORT(cv_puzzle) const CvPuzzle *cv_puzzle(void) { return &PV; }

/* The solver's move: the state after it. A right move with a reply to come is drawn alone
 * (reply_pending); cv_puzzle_reply then draws the reply, which the kernel has already played. */
EXPORT(cv_puzzle_try) int cv_puzzle_try(int from, int to, int promo) {
    if (!PV.ok || PV.intro) return CP_FAILED;
    ch_move m = CH_MOVE(from, to, promo);
    chess_pos before = PS.pos;
    int st = cp_try(&PS, m);
    if (st == CP_FAILED) {   /* the wrong move stays on the board, marked, until the solution is shown */
        chess_pos after;
        if (ch_is_legal(&before, m)) { ch_make(&before, m, &after); puzzle_fill_pos(&after, m); PV.wrong_sq = (uint8_t)CH_TO(m); }
        else puzzle_fill(CH_MOVE_NONE);
        return st;
    }
    if (PS.last[1] == CH_MOVE_NONE) { puzzle_fill(m); return st; }
    chess_pos mid;
    ch_make(&before, m, &mid);
    puzzle_fill_pos(&mid, m);
    PV.reply_pending = 1;
    return st;
}
EXPORT(cv_puzzle_reply) const CvPuzzle *cv_puzzle_reply(void) {
    if (PV.reply_pending) { PV.reply_pending = 0; puzzle_fill(PS.last[1]); }
    return &PV;
}

/* The squares the piece on `from` may go to while solving. */
static uint64_t puzzle_targets(int from) {
    if (!PV.ok || PV.intro || PS.state != CP_SOLVING) return 0;
    ch_moves ms;
    ch_legal(&PS.pos, &ms);
    uint64_t t = 0;
    for (int i = 0; i < ms.n; i++) if (CH_FROM(ms.m[i]) == from) t |= 1ull << CH_TO(ms.m[i]);
    return t;
}
EXPORT(cv_puzzle_targets_lo) uint32_t cv_puzzle_targets_lo(int from) { return (uint32_t)puzzle_targets(from); }
EXPORT(cv_puzzle_targets_hi) uint32_t cv_puzzle_targets_hi(int from) { return (uint32_t)(puzzle_targets(from) >> 32); }
EXPORT(cv_puzzle_is_promotion) int cv_puzzle_is_promotion(int from, int to) {
    if (!PV.ok) return 0;
    int col, t = ch_piece_at(&PS.pos, from, &col);
    return t == CH_PAWN && (to / 8 == 7 || to / 8 == 0);
}

/* Shows the line's next move: 1, or 0 at its end. */
EXPORT(cv_puzzle_reveal) int cv_puzzle_reveal(void) {
    ch_move m;
    if (!PV.ok || !cp_reveal(&PS, &m)) return 0;
    PV.wrong_sq = CV_NO_SQ;
    puzzle_fill(m);
    return 1;
}

/* Says how it went (POST /c/puzzle/result): CC_EV_READ with this tag, then cv_puzzle_done. */
EXPORT(cv_puzzle_send) void cv_puzzle_send(int win, uint32_t tag) {
    cw_puzzle_req q = { PV.id, (uint8_t)(win != 0), { 0, 0, 0 } };
    cc_client_post(&C, "/c/puzzle/result", &q, sizeof q, tag);
}

/* io holds that answer's n bytes (cw_puzzle_done). */
EXPORT(cv_puzzle_done) const CvPuzzle *cv_puzzle_done(uint32_t n) {
    cw_puzzle_done d;
    if (n != sizeof d) return &PV;
    memcpy(&d, IO, sizeof d);
    PV.done = 1;
    PV.before = d.before;
    PV.after = d.after;
    PV.rating = d.after;
    PV.rd = d.rd;
    PV.puzzle_rating = d.puzzle;
    PV.tried = d.tried;
    PV.solved = d.solved;
    return &PV;
}

/* ---- chat ---- */
static CvChat CH;
static CvChatErr CE;

/* io holds a room's name (n bytes) into room (CW_ROOM): false when it does not fit. */
static bool io_room(uint32_t n, char *room) {
    if (n == 0 || n >= CW_ROOM) return false;
    memset(room, 0, CW_ROOM);
    memcpy(room, IO, n);
    return true;
}

/* io holds a room's name (n bytes): its chat as kept. */
EXPORT(cv_chat) const CvChat *cv_chat(uint32_t n) {
    memset(&CH, 0, sizeof CH);
    CH.guest = C.name[0] && cc_name_guest(C.name);
    char room[CW_ROOM];
    if (!io_room(n, room)) return &CH;
    memcpy(CH.room, room, CW_ROOM);
    const cc_chat_room *r = cc_client_chat(&C, room);
    for (int i = 0; r && i < r->n && i < CV_CHAT_LINES; i++) {
        const cw_chat_line *l = &r->l[i];
        CvChatLine *o = &CH.lines[i];
        o->at = (double)l->at;
        copy_n(o->name, CV_NAME, l->name, CW_NAME);
        memcpy(o->text, l->text, l->len);
        o->mine = C.name[0] && strcmp(o->name, C.name) == 0;
        CH.n++;
    }
    return &CH;
}
/* io holds a room's name (n bytes): its log read (CC_EV_CHAT when it arrives). */
EXPORT(cv_chat_log) void cv_chat_log(uint32_t n) {
    char room[CW_ROOM];
    if (io_room(n, room)) cc_client_chat_log(&C, room);
}
/* io holds the room's name (rn bytes) then the line (tn bytes): POST /c/chat, answered as a read
 * with this tag (200, or a refusal: cv_chat_refused). */
EXPORT(cv_chat_say) void cv_chat_say(uint32_t rn, uint32_t tn, uint32_t tag) {
    char room[CW_ROOM], text[CW_CHAT_MAX + 1];
    if (!io_room(rn, room) || rn + tn > IO_CAP) return;
    if (tn > CW_CHAT_MAX + 1) tn = CW_CHAT_MAX + 1;   /* the server says it is too long */
    memcpy(text, IO + rn, tn);
    cc_client_chat_say(&C, room, text, tn, tag);
}
/* io holds a line (n bytes): 0 when the server would take its text, else why not (CC_CHAT_ for the
 * line itself, CV_CHAT_BANNED for a banned word), so the page warns before it sends. */
EXPORT(cv_chat_check) int cv_chat_check(uint32_t n) {
    if (n > IO_CAP) n = IO_CAP;
    int v = cc_chat_valid((const char *)IO, (int)n);
    if (v != CC_CHAT_OK) return v;
    return cc_chat_banned((const char *)IO, (int)n) ? CV_CHAT_BANNED : 0;
}
/* io holds a refusal of POST /c/chat (n bytes): its code, reason and wait (code 0: not one). */
EXPORT(cv_chat_refused) const CvChatErr *cv_chat_refused(uint32_t n) {
    memset(&CE, 0, sizeof CE);
    cw_chat_err e;
    if (n != sizeof e) return &CE;
    memcpy(&e, IO, sizeof e);
    CE.code = e.code;
    CE.why = (uint8_t)e.rc;
    CE.wait_s = e.wait_s;
    return &CE;
}

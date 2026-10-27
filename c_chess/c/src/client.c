/* client.c - see client.h. The outbox holds frames as [u32 length][frame], taken in order. */
#include "client.h"

#include "stream_frame.h"

#include <string.h>

#define SF_WHAT_ROOM 2   /* the hub's room subscription (rt_local.c do_sub) */

void cc_client_init(cc_client *c) {
    memset(c, 0, sizeof *c);
    ch_init();
    c->next_id = 1;
    c->color = -1;
}

uint32_t cc_client_events(cc_client *c) {
    uint32_t e = c->events;
    c->events = 0;
    return e;
}

size_t cc_client_next_out(cc_client *c, const uint8_t **p) {
    if (c->out_head >= c->out_len) { c->out_len = 0; c->out_head = 0; return 0; }
    uint32_t n;
    memcpy(&n, c->out + c->out_head, 4);
    *p = c->out + c->out_head + 4;
    c->out_head += 4 + n;
    if (c->out_head >= c->out_len) { /* the last frame: the buffer is free once the host has sent it */
        c->out_len = 0;
        c->out_head = 0;
        /* *p still points at the bytes, which nothing overwrites before the next call */
    }
    return n;
}

/* Reserves a frame of up to cap bytes at the outbox's end: its start, or NULL when full. */
static uint8_t *out_begin(cc_client *c, size_t cap) {
    if (c->out_len + 4 + cap > CC_OUT_CAP) return NULL;
    return c->out + c->out_len + 4;
}
static void out_commit(cc_client *c, int n) {
    if (n <= 0) return;
    uint32_t len = (uint32_t)n;
    memcpy(c->out + c->out_len, &len, 4);
    c->out_len += 4 + len;
}

static void pend_add(cc_client *c, uint32_t id, int kind, uint32_t game) {
    cc_pending *p = &c->pend[id % CC_PENDING];
    p->id = id;
    p->kind = (uint8_t)kind;
    p->game = game;
}

static uint32_t req(cc_client *c, int kind, int meth, const char *path, const void *body, size_t bn, uint32_t game) {
    size_t pn = strlen(path);
    uint8_t *o = out_begin(c, 9 + pn + bn);
    if (!o) return 0;
    uint32_t id = c->next_id++;
    out_commit(c, sf_put_req(o, 9 + pn + bn, id, (uint8_t)meth, path, pn, body, bn));
    pend_add(c, id, kind, game);
    return id;
}

static void sub(cc_client *c, int kind, const char *room) {
    size_t n = strlen(room);
    uint8_t *o = out_begin(c, 8 + n);
    if (!o) return;
    uint32_t id = c->next_id++;
    out_commit(c, sf_put_sub(o, 8 + n, (uint8_t)kind, id, SF_WHAT_ROOM, room, n));
    pend_add(c, id, CC_RQ_SUB, 0);
}

void cc_client_watch(cc_client *c, const char *room) { sub(c, SF_SUB, room); }
void cc_client_unwatch(cc_client *c, const char *room) { sub(c, SF_UNSUB, room); }

static void room_name(uint32_t game, char *out) {
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + game % 10); game /= 10; } while (game);
    *out++ = 'g';
    while (n) *out++ = t[--n];
    *out = 0;
}

/* prefix then the decimal of v: "/c/game/" 12 -> "/c/game/12" */
static void num_path(char *out, const char *prefix, uint32_t v) {
    size_t n = strlen(prefix);
    memcpy(out, prefix, n);
    char t[12];
    int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) out[n++] = t[--k];
    out[n] = 0;
}

/* ---- the account ---- */

void cc_client_auth_body(const char *name, const char *password, int mode, cw_auth_req *out) {
    memset(out, 0, sizeof *out);
    size_t nn = strlen(name), pn = strlen(password);
    memcpy(out->name, name, nn < CW_NAME ? nn : CW_NAME);
    memcpy(out->password, password, pn < CW_PASSWORD ? pn : CW_PASSWORD);
    out->mode = (uint8_t)mode;
}

void cc_client_refresh_body(const cc_client *c, cw_refresh_req *out) {
    memset(out, 0, sizeof *out);
    memcpy(out->refresh, c->refresh, strlen(c->refresh));
}

void cc_client_auth_answer(cc_client *c, int status, const uint8_t *p, size_t n) {
    c->auth_status = status;
    c->auth_code = 0;
    c->events |= CC_EV_AUTH;
    if (status != 200) {
        if (n >= 1) c->auth_code = p[0];
        return;
    }
    cw_session s;
    if (n < sizeof s) { c->auth_status = 502; return; }
    memcpy(&s, p, sizeof s);
    if (s.access_len > CC_TOKEN_MAX || n != sizeof s + s.access_len) { c->auth_status = 502; return; }
    memcpy(c->token, p + sizeof s, s.access_len);
    c->token[s.access_len] = 0;
    c->token_len = s.access_len;
    memcpy(c->user_id, s.user_id, CW_UUID);
    c->user_id[CW_UUID] = 0;
    memcpy(c->name, s.name, CW_NAME);
    c->name[CW_NAME] = 0;
    memcpy(c->refresh, s.refresh, CW_REFRESH);
    c->refresh[CW_REFRESH] = 0;
    c->token_expires_in = s.expires_in;
}

void cc_client_hello(cc_client *c) {
    c->ready = 0;
    uint8_t *o = out_begin(c, 4 + c->token_len);
    if (o) out_commit(c, sf_put_hello(o, 4 + c->token_len, c->token, c->token_len));
}

/* ---- requests ---- */

void cc_client_seek(cc_client *c, int base_s, int inc_s, int rated) {
    cw_seek s = { (uint16_t)base_s, (uint8_t)inc_s, (uint8_t)(rated != 0) };
    req(c, CC_RQ_SEEK, SF_METH_POST, "/c/seek", &s, sizeof s, 0);
}

void cc_client_unseek(cc_client *c) { req(c, CC_RQ_UNSEEK, SF_METH_POST, "/c/unseek", NULL, 0, 0); }

void cc_client_challenge(cc_client *c, int base_s, int inc_s, int rated) {
    cw_seek s = { (uint16_t)base_s, (uint8_t)inc_s, (uint8_t)(rated != 0) };
    req(c, CC_RQ_CHALLENGE, SF_METH_POST, "/c/challenge", &s, sizeof s, 0);
}

void cc_client_accept(cc_client *c, uint32_t challenge) {
    cw_id q = { challenge };
    req(c, CC_RQ_ACCEPT, SF_METH_POST, "/c/accept", &q, sizeof q, 0);
}

void cc_client_ai(cc_client *c, int base_s, int inc_s, int level, int color) {
    cw_ai_req q = { (uint16_t)base_s, (uint8_t)inc_s, (uint8_t)level, (uint8_t)color, { 0, 0, 0 } };
    req(c, CC_RQ_ACCEPT, SF_METH_POST, "/c/ai", &q, sizeof q, 0);   /* answered as an accept is: the game's id */
}

void cc_client_lobby(cc_client *c) {
    sub(c, SF_SUB, "lobby");
    req(c, CC_RQ_LOBBY, SF_METH_GET, "/c/lobby", NULL, 0, 0);
    req(c, CC_RQ_TV, SF_METH_GET, "/c/tv", NULL, 0, 0);
}

static void take_tv(cc_client *c, const uint8_t *p, size_t n, int64_t now) {
    if (n < 8 || n > sizeof c->tv) return;
    uint16_t k;
    memcpy(&k, p + 2, 2);
    if (k > CW_TV_MAX || n != 8 + (size_t)k * sizeof(cw_tv_game)) return;
    memcpy(&c->tv, p, n);
    c->tv_at = now;
    c->events |= CC_EV_TV;
}

void cc_client_lobby_leave(cc_client *c) { sub(c, SF_UNSUB, "lobby"); }

static void game_reset(cc_client *c, uint32_t game) {
    c->game = game;
    c->synced = 0;
    c->color = -1;
    cc_game_new(&c->g, 0, 0, 0);
    memset(c->names, 0, sizeof c->names);
    memset(c->san, 0, sizeof c->san);
    c->confirmed_ply = 0;
    c->next_game = 0;
    c->game_arena = 0;
    c->rematch_offer = 0;
    c->clock_running = 0;
    c->move_sent_at = 0;
    c->premove = CH_MOVE_NONE;
    c->turn_seen_at = 0;
}

void cc_client_open(cc_client *c, uint32_t game) {
    if (c->game && c->game != game) cc_client_close_game(c);
    game_reset(c, game);
    char room[12], path[24];
    room_name(game, room);
    sub(c, SF_SUB, room);
    num_path(path, "/c/game/", game);
    req(c, CC_RQ_SNAP, SF_METH_GET, path, NULL, 0, game);
}

void cc_client_close_game(cc_client *c) {
    if (!c->game) return;
    char room[12];
    room_name(c->game, room);
    sub(c, SF_UNSUB, room);
    c->game = 0;
    c->synced = 0;
}

static void push_san(cc_client *c, ch_move m) {
    if (c->g.ply < CC_MAX_PLY) ch_san(&c->g.pos, m, c->san[c->g.ply]);
}

/* Plays m on the mirror (the move is known legal there), clocks untouched. */
static int mirror_move(cc_client *c, ch_move m) {
    push_san(c, m);
    /* the mirror keeps no time (the server's pushes do): the rules alone decide here */
    int64_t keep[2] = { c->g.left_ms[0], c->g.left_ms[1] };
    c->g.left_ms[0] = c->g.left_ms[1] = INT64_MAX / 4;
    int rc = cc_game_move(&c->g, c->g.pos.stm, c->g.ply, m, c->g.turn_at);
    c->g.left_ms[0] = keep[0];
    c->g.left_ms[1] = keep[1];
    return rc;
}

int cc_client_move(cc_client *c, int from, int to, int promo, int64_t now) {
    if (!c->game || !c->synced || c->color < 0) return CC_E_STATE;
    if (cc_game_over(&c->g)) return CC_E_OVER;
    if (c->g.pos.stm != c->color) return CC_E_TURN;
    ch_move m = CH_MOVE(from, to, promo);
    if (!ch_is_legal(&c->g.pos, m)) return CC_E_ILLEGAL;
    int64_t think = c->turn_seen_at ? now - c->turn_seen_at : -1;
    cw_move_req q = { c->game, c->g.ply, m, think < 0 || think > 3600000 ? 0xffffffffu : (uint32_t)think };
    if (!req(c, CC_RQ_MOVE, SF_METH_POST, "/c/move", &q, sizeof q, c->game)) return CC_E_STATE;
    /* the clock as the server will see it: ours stops, theirs starts */
    int64_t mine = cc_client_clock(c, c->color, now);
    c->left_ms[c->color] = (uint32_t)(mine + (c->clock_running ? c->g.inc_ms : 0));
    c->left_ms[c->color ^ 1] = (uint32_t)cc_client_clock(c, c->color ^ 1, now);
    c->left_at = now;
    mirror_move(c, m);
    if (cc_game_over(&c->g)) { c->clock_running = 0; c->events |= CC_EV_OVER; }
    else if (c->g.ply >= 2) c->clock_running = 1;
    c->move_sent_at = now;
    c->move_sent_ply = c->g.ply;
    c->moves_sent++;
    c->events |= CC_EV_GAME;
    return CC_OK;
}

int cc_client_premove(cc_client *c, int from, int to, int promo) {
    if (!c->game || !c->synced || c->color < 0 || cc_game_over(&c->g) || c->g.pos.stm == c->color) return CC_E_STATE;
    if (!(cc_client_premove_targets(c, from) & (1ULL << to))) return CC_E_ILLEGAL;
    c->premove = CH_MOVE(from, to, promo);
    c->events |= CC_EV_GAME;
    return CC_OK;
}

void cc_client_premove_cancel(cc_client *c) {
    if (c->premove) c->events |= CC_EV_GAME;
    c->premove = CH_MOVE_NONE;
}

uint64_t cc_client_premove_targets(const cc_client *c, int from) {
    if (!c->synced || c->color < 0 || cc_game_over(&c->g) || from < 0 || from > 63) return 0;
    int col, t = ch_piece_at(&c->g.pos, from, &col);
    if (t < 0 || col != c->color) return 0;
    chess_pos p = c->g.pos;
    p.stm = (uint8_t)c->color;
    p.ep = CH_NO_SQ;
    ch_moves ms;
    ch_pseudo(&p, &ms);
    uint64_t out = 0;
    for (int i = 0; i < ms.n; i++)
        if (CH_FROM(ms.m[i]) == from) out |= 1ULL << CH_TO(ms.m[i]);
    if (t == CH_PAWN) {   /* a pawn may premove a capture onto a square the other side may yet occupy */
        int fwd = c->color == CH_WHITE ? 8 : -8, r = from / 8, f = from % 8;
        if (r + (c->color == CH_WHITE ? 1 : -1) >= 0 && r + (c->color == CH_WHITE ? 1 : -1) <= 7) {
            if (f > 0) out |= 1ULL << (from + fwd - 1);
            if (f < 7) out |= 1ULL << (from + fwd + 1);
        }
    }
    return out;
}

void cc_client_read(cc_client *c, const char *path, uint32_t tag) {
    req(c, CC_RQ_READ, SF_METH_GET, path, NULL, 0, tag);   /* the tag rides in the request's game slot */
}

void cc_client_post(cc_client *c, const char *path, const void *body, size_t n, uint32_t tag) {
    req(c, CC_RQ_READ, SF_METH_POST, path, body, n, tag);
}

void cc_client_eval(cc_client *c, int ply) {
    if (ply < 0 || ply > c->g.ply) return;
    uint8_t body[sizeof(cw_eval_req) + CC_MAX_PLY * 2];
    cw_eval_req q = { (uint16_t)ply, { 0, 0 } };
    memcpy(body, &q, sizeof q);
    memcpy(body + sizeof q, c->g.moves, (size_t)ply * 2);
    req(c, CC_RQ_NONE, SF_METH_POST, "/c/eval", body, sizeof q + (size_t)ply * 2, 0);
}

void cc_client_act(cc_client *c, int act) {
    if (!c->game) return;
    cw_act_req q = { c->game, (uint8_t)act, { 0, 0, 0 } };
    req(c, CC_RQ_ACT, SF_METH_POST, "/c/act", &q, sizeof q, c->game);
}

/* ---- what arrives ---- */

static void resync(cc_client *c) {
    char path[24];
    num_path(path, "/c/game/", c->game);
    req(c, CC_RQ_SNAP, SF_METH_GET, path, NULL, 0, c->game);
}

static void take_snapshot(cc_client *c, const uint8_t *p, size_t n, int64_t now) {
    cw_snap s;
    if (n < sizeof s) return;
    memcpy(&s, p, sizeof s);
    if (s.game != c->game || n != sizeof s + (size_t)s.ply * 2) return;
    uint32_t game = c->game;
    game_reset(c, game);
    c->g.base_ms = (int32_t)s.base_s * 1000;
    c->g.inc_ms = (int32_t)s.inc_s * 1000;
    for (int i = 0; i < s.ply; i++) {
        uint16_t m;
        memcpy(&m, p + sizeof s + (size_t)i * 2, 2);
        if (mirror_move(c, (ch_move)m) != CC_OK) break;
    }
    c->g.status = s.status;
    c->g.winner = s.winner;
    c->g.draw_offer = s.draw_offer;
    c->confirmed_ply = c->g.ply;
    c->rematch_offer = s.rematch_offer;
    c->next_game = s.next_game;
    c->game_arena = s.arena;
    c->clock_running = s.clock_running;
    c->base_rated[0] = (uint8_t)(s.base_s > 255 ? 255 : s.base_s);
    c->base_rated[1] = s.rated;
    for (int k = 0; k < 2; k++) {
        memcpy(c->names[k], s.name[k], CW_NAME);
        c->names[k][CW_NAME] = 0;
        c->rating[k] = s.rating[k];
        c->diff[k] = s.diff[k];
        c->left_ms[k] = s.left_ms[k];
    }
    /* the side to move has been thinking turn_ms already */
    if (s.clock_running) {
        int stm = c->g.pos.stm;
        c->left_ms[stm] = s.left_ms[stm] > s.turn_ms ? s.left_ms[stm] - s.turn_ms : 0;
    }
    c->left_at = now;
    c->color = -1;
    for (int k = 0; k < 2; k++)
        if (c->name[0] && strcmp(c->names[k], c->name) == 0) c->color = (int8_t)k;
    c->synced = 1;
    c->turn_seen_at = c->color == c->g.pos.stm ? now : 0;
    c->premove = CH_MOVE_NONE;
    c->events |= CC_EV_GAME;
}

static void take_state(cc_client *c, const uint8_t *p, size_t n, int64_t now) {
    cw_state s;
    if (n != sizeof s) return;
    memcpy(&s, p, sizeof s);
    if (s.game != c->game || !c->synced) return;
    c->pushes++;
    bool was_over = cc_game_over(&c->g), premove_due = false;
    if (s.ply == c->g.ply + 1) {   /* the other side's move (or ours, from another socket) */
        if (mirror_move(c, (ch_move)s.move) != CC_OK) { resync(c); return; }
        if (c->color >= 0 && (s.ply & 1) == (c->color == CH_WHITE ? 0 : 1)) {
            c->events |= CC_EV_MOVED;
            c->turn_seen_at = now;
            premove_due = true;
        }
    } else if (s.ply > c->g.ply + 1) { resync(c); return; }
    else if (s.ply < c->g.ply) {
        /* our move is in flight and this is older news: take the offers, keep the board */
        if (s.ply < c->confirmed_ply) return;
    } else if (s.ply == c->g.ply && s.ply && c->g.moves[s.ply - 1] != s.move) { resync(c); return; }
    if (s.ply >= c->confirmed_ply) {
        c->confirmed_ply = s.ply;
        if (c->move_sent_at && s.ply == c->move_sent_ply) c->moves_confirmed++;
        c->g.status = s.status;
        c->g.winner = s.winner;
        c->g.draw_offer = s.draw_offer;
        c->rematch_offer = s.rematch_offer;
        c->next_game = s.next_game;
        c->clock_running = s.clock_running;
        if (s.ply == c->g.ply) {   /* the clocks are for the position we show */
            c->left_ms[0] = s.left_ms[0];
            c->left_ms[1] = s.left_ms[1];
            c->left_at = now;
        }
        for (int k = 0; k < 2; k++) { c->diff[k] = s.diff[k]; c->rating[k] = s.rating[k]; }
    }
    c->events |= CC_EV_GAME;
    if (!was_over && cc_game_over(&c->g)) c->events |= CC_EV_OVER;
    if (premove_due && c->premove) {
        ch_move m = c->premove;
        c->premove = CH_MOVE_NONE;
        cc_client_move(c, CH_FROM(m), CH_TO(m), CH_PROMO(m), now);   /* which refuses it unless legal now */
    }
}

static void take_reply(cc_client *c, const sf_frame *f, int64_t now) {
    cc_pending *p = &c->pend[f->id % CC_PENDING];
    if (p->id != f->id) return;
    int kind = p->kind;
    uint32_t game = p->game;
    p->id = 0;
    if (kind == CC_RQ_READ) {   /* any status: the page reads the answer it asked for */
        c->read_tag = game;
        c->read_status = f->status;
        c->read_len = f->body_len > CC_READ_CAP ? 0 : (uint32_t)f->body_len;
        if (c->read_len) memcpy(c->read_buf, f->body, c->read_len);
        else if (f->body_len) c->read_status = 507;   /* too large to keep */
        c->events |= CC_EV_READ;
        return;
    }
    if (f->status != 200 && kind != CC_RQ_SUB) {
        memset(&c->err, 0, sizeof c->err);
        if (f->body_len >= sizeof c->err) memcpy(&c->err, f->body, sizeof c->err);
        c->err_status = f->status;
        c->events |= CC_EV_ERROR;
        if (kind == CC_RQ_MOVE && game == c->game) resync(c);   /* the board we showed was wrong */
        return;
    }
    switch (kind) {
    case CC_RQ_SEEK: {
        cw_seek_ans a;
        if (f->body_len != sizeof a) return;
        memcpy(&a, f->body, sizeof a);
        if (a.game) { c->start_game = a.game; c->start_color = a.color; c->events |= CC_EV_START; }
        else c->events |= CC_EV_SEEK;
        return;
    }
    case CC_RQ_CHALLENGE:
    case CC_RQ_ACCEPT: {
        cw_id a;
        if (f->body_len != sizeof a) return;
        memcpy(&a, f->body, sizeof a);
        if (kind == CC_RQ_CHALLENGE) { c->challenge_id = a.id; c->events |= CC_EV_CHALLENGE; }
        else { c->start_game = a.id; c->events |= CC_EV_START; }
        return;
    }
    case CC_RQ_SNAP:
        if (game == c->game) take_snapshot(c, f->body, f->body_len, now);
        return;
    case CC_RQ_TV:
        take_tv(c, f->body, f->body_len, now);
        return;
    case CC_RQ_LOBBY:
        if (f->body_len >= 12 && f->body_len <= sizeof c->lobby) {
            memcpy(&c->lobby, f->body, f->body_len);
            c->events |= CC_EV_LOBBY;
        }
        return;
    default:
        return;
    }
}

void cc_client_in(cc_client *c, const uint8_t *p, size_t n, int64_t now) {
    if (sf_is_push(p, n)) {
        switch (p[1]) {
        case CW_P_STATE: take_state(c, p, n, now); return;
        case CW_P_START: {
            cw_start s;
            if (n != sizeof s) return;
            memcpy(&s, p, sizeof s);
            c->start_game = s.game;
            c->start_color = s.color;
            c->events |= CC_EV_START;
            return;
        }
        case CW_P_TV: take_tv(c, p, n, now); return;
        case CW_P_EVAL:
            if (n == sizeof c->eval) { memcpy(&c->eval, p, n); c->events |= CC_EV_EVAL; }
            return;
        case CW_P_LOBBY:
            if (n >= 12 && n <= sizeof c->lobby) { memcpy(&c->lobby, p, n); c->events |= CC_EV_LOBBY; }
            return;
        case CW_P_ARENA: cc_client_arena_take(c, p, n, now, 0); return;
        default: return;
        }
    }
    sf_frame f;
    if (sf_parse(p, n, &f) != 0) return;
    if (f.kind == SF_READY) { c->ready = 1; c->events |= CC_EV_READY; }
    else if (f.kind == SF_REPLY) take_reply(c, &f, now);
}

int cc_client_arena_take(cc_client *c, const uint8_t *p, size_t n, int64_t now, int answer) {
    cw_arena_hdr h;
    if (n < sizeof h) return 0;
    memcpy(&h, p, sizeof h);
    if (h.v != CW_V || h.kind != CW_P_ARENA || h.n_rows > CW_ARENA_ROWS || n != sizeof h + (size_t)h.n_rows * sizeof(cw_arena_row)) return 0;
    if (!answer) {   /* a push: our place as we last knew it */
        cw_arena_hdr was;
        memcpy(&was, c->arena, sizeof was);
        bool same = c->arena_len >= sizeof was && was.id == h.id;
        h.me_in = same ? was.me_in : 0;
        h.me_fire = same ? was.me_fire : 0;
        h.me_rank = same ? was.me_rank : 0;
        h.me_score = same ? was.me_score : 0;
        h.me_games = same ? was.me_games : 0;
    }
    char me[CW_NAME];
    memset(me, 0, sizeof me);
    for (int i = 0; i < CW_NAME && c->name[i]; i++) me[i] = c->name[i];   /* (the browser's libc has no strnlen) */
    for (int i = 0; me[0] && i < h.n_rows; i++) {
        cw_arena_row r;
        memcpy(&r, p + sizeof h + (size_t)i * sizeof r, sizeof r);
        if (memcmp(r.name, me, CW_NAME) != 0) continue;
        h.me_in = r.withdrawn ? CW_ARENA_WITHDRAWN : CW_ARENA_IN;
        h.me_fire = r.fire;
        h.me_rank = r.rank;
        h.me_score = r.score;
        h.me_games = r.games;
        break;
    }
    memcpy(c->arena, &h, sizeof h);
    memcpy(c->arena + sizeof h, p + sizeof h, n - sizeof h);
    c->arena_len = (uint32_t)n;
    c->arena_at = now;
    c->events |= CC_EV_ARENA;
    return 1;
}

/* ---- what to draw ---- */

int64_t cc_client_clock(const cc_client *c, int color, int64_t now) {
    int64_t t = c->left_ms[color];
    if (c->clock_running && !cc_game_over(&c->g) && c->g.pos.stm == color) t -= now - c->left_at;
    return t < 0 ? 0 : t;
}

uint64_t cc_client_targets(const cc_client *c, int from) {
    if (!c->synced || cc_game_over(&c->g) || c->color != c->g.pos.stm || from < 0 || from > 63) return 0;
    ch_moves ms;
    ch_legal(&c->g.pos, &ms);
    uint64_t t = 0;
    for (int i = 0; i < ms.n; i++)
        if (CH_FROM(ms.m[i]) == from) t |= 1ULL << CH_TO(ms.m[i]);
    return t;
}

int cc_client_is_promotion(const cc_client *c, int from, int to) {
    int col, t = ch_piece_at(&c->g.pos, from, &col);
    return t == CH_PAWN && (to / 8 == 7 || to / 8 == 0);
}

int cc_client_my_turn(const cc_client *c) {
    return c->synced && c->color >= 0 && !cc_game_over(&c->g) && c->g.pos.stm == c->color;
}

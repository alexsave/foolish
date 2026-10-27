// unit_client.c - the client without a server: frames built by hand go in, the frames it sends come
// out. A snapshot, a move shown before the server answers, the thinking time it reports, the
// other side's move, a premove played the moment that move arrives (or dropped when it became
// illegal), and a push from the future asking for the snapshot again. An arena: its room watched, its
// pushes kept, the reader's own place kept across pushes and renewed by its own row. Chat: lines
// pushed kept per room (three rooms, the one touched longest ago given up), the last 64, a log read
// merged with the lines pushed while it was in flight, a line said, refusals read as reads.
#include "client.h"
#include "stream_frame.h"
#include "t.h"

static uint8_t buf[16 + CW_CHAT_LOG_MAX];

/* The REPLY to the last request the client queued, with body. */
static void reply(cc_client *c, uint32_t id, int status, const void *body, size_t n, int64_t now) {
    buf[0] = SF_V;
    buf[1] = SF_REPLY;
    sf_w32(buf + 2, id);
    sf_w16(buf + 6, (uint16_t)status);
    memcpy(buf + 8, body, n);
    cc_client_in(c, buf, 8 + n, now);
}

static void push_state(cc_client *c, uint32_t game, int ply, ch_move m, int64_t now) {
    cw_state s;
    memset(&s, 0, sizeof s);
    s.v = CW_V;
    s.kind = CW_P_STATE;
    s.status = CC_PLAYING;
    s.winner = CC_NO_WINNER;
    s.game = game;
    s.ply = (uint16_t)ply;
    s.move = m;
    s.left_ms[0] = s.left_ms[1] = 60000;
    s.clock_running = ply >= 2;
    cc_client_in(c, (const uint8_t *)&s, sizeof s, now);
}

/* Drains the outbox; the last REQ's path and body into path/body. How many frames. */
static int drain(cc_client *c, char *path, cw_move_req *mv) {
    const uint8_t *f;
    size_t n;
    int k = 0;
    path[0] = 0;
    while ((n = cc_client_next_out(c, &f)) > 0) {
        k++;
        sf_frame fr;
        if (sf_parse(f, n, &fr) == 0 && fr.kind == SF_REQ) {
            memcpy(path, fr.path, fr.path_len);
            path[fr.path_len] = 0;
            if (mv && fr.body_len == sizeof *mv) memcpy(mv, fr.body, sizeof *mv);
        }
    }
    return k;
}

static ch_move uci(const cc_client *c, const char *s) { return ch_parse_uci(&c->g.pos, s, strlen(s)); }

/* An arena frame: id, then rows of (name, score, rank), with me_ fields when me_rank. */
static size_t arena_frame(uint8_t *out, uint32_t id, int n, const char *const *names, int me_rank) {
    cw_arena_hdr h;
    memset(&h, 0, sizeof h);
    h.v = CW_V;
    h.kind = CW_P_ARENA;
    h.status = CC_ARENA_RUNNING;
    h.id = id;
    h.n_rows = (uint16_t)n;
    h.n_players = 150;
    if (me_rank) { h.me_in = CW_ARENA_IN; h.me_rank = (uint16_t)me_rank; h.me_score = 3; h.me_games = 2; }
    memcpy(out, &h, sizeof h);
    for (int i = 0; i < n; i++) {
        cw_arena_row r;
        memset(&r, 0, sizeof r);
        memcpy(r.name, names[i], strlen(names[i]));
        r.rank = (uint16_t)(i + 1);
        r.score = (uint16_t)(20 - i);
        r.games = 9;
        r.fire = i == 0;
        r.withdrawn = i == 2;
        memcpy(out + sizeof h + (size_t)i * sizeof r, &r, sizeof r);
    }
    return sizeof h + (size_t)n * sizeof(cw_arena_row);
}

static cw_arena_hdr arena_hdr(const cc_client *c) {
    cw_arena_hdr h;
    memcpy(&h, c->arena, sizeof h);
    return h;
}

static void arenas(void) {
    cc_client c;
    cc_client_init(&c);
    strcpy(c.name, "me");
    char path[300];
    TEST("an arena's room is watched by name");
    cc_client_watch(&c, "a12");
    const uint8_t *f;
    size_t n = cc_client_next_out(&c, &f);
    sf_frame fr;
    CHECK(n && sf_parse(f, n, &fr) == 0 && fr.kind == SF_SUB && fr.gid_len == 3 && !memcmp(fr.gid, "a12", 3), "a SUB to a12");
    cc_client_unwatch(&c, "a12");
    n = cc_client_next_out(&c, &f);
    CHECK(n && sf_parse(f, n, &fr) == 0 && fr.kind == SF_UNSUB && fr.gid_len == 3, "an UNSUB");
    drain(&c, path, NULL);

    static uint8_t fb[CW_ARENA_FRAME_MAX + 64];
    static const char *const top[] = { "ann", "ben", "me" }, *const others[] = { "ann", "ben" };
    TEST("an answer with our place below the rows shown");
    n = arena_frame(fb, 12, 2, others, 150);
    CHECK(cc_client_arena_take(&c, fb, n, 100, 1) == 1, "taken");
    CHECK((cc_client_events(&c) & CC_EV_ARENA) && c.arena_len == n && c.arena_at == 100, "event, length %u", c.arena_len);
    CHECK(arena_hdr(&c).me_in == CW_ARENA_IN && arena_hdr(&c).me_rank == 150 && arena_hdr(&c).me_score == 3, "me %u", arena_hdr(&c).me_rank);
    TEST("a push of the same arena keeps our place");
    n = arena_frame(fb, 12, 2, others, 0);
    cc_client_in(&c, fb, n, 200);
    CHECK((cc_client_events(&c) & CC_EV_ARENA) && c.arena_at == 200, "%s", "the push was not taken");
    CHECK(arena_hdr(&c).me_in == CW_ARENA_IN && arena_hdr(&c).me_rank == 150 && arena_hdr(&c).me_games == 2, "me %u", arena_hdr(&c).me_rank);
    TEST("our own row renews it");
    n = arena_frame(fb, 12, 3, top, 0);
    cc_client_in(&c, fb, n, 300);
    CHECK(arena_hdr(&c).me_rank == 3 && arena_hdr(&c).me_score == 18 && arena_hdr(&c).me_games == 9, "me %u score %u", arena_hdr(&c).me_rank, arena_hdr(&c).me_score);
    CHECK(arena_hdr(&c).me_in == CW_ARENA_WITHDRAWN && !arena_hdr(&c).me_fire, "withdrawn %u fire %u", arena_hdr(&c).me_in, arena_hdr(&c).me_fire);
    TEST("another arena's push holds no place of ours");
    n = arena_frame(fb, 13, 2, others, 0);
    cc_client_in(&c, fb, n, 400);
    CHECK(arena_hdr(&c).id == 13 && arena_hdr(&c).me_in == CW_ARENA_OUT && !arena_hdr(&c).me_rank, "me %u in %u", arena_hdr(&c).me_rank, arena_hdr(&c).me_in);
    TEST("a frame of the wrong length or too many rows is dropped");
    cc_client_events(&c);
    n = arena_frame(fb, 14, 2, others, 0);
    CHECK(!cc_client_arena_take(&c, fb, n - 1, 500, 1) && !cc_client_arena_take(&c, fb, n + 32, 500, 1), "a short or long frame taken");
    cw_arena_hdr bad;
    memcpy(&bad, fb, sizeof bad);
    bad.n_rows = CW_ARENA_ROWS + 1;
    memcpy(fb, &bad, sizeof bad);
    CHECK(!cc_client_arena_take(&c, fb, sizeof bad + (CW_ARENA_ROWS + 1) * sizeof(cw_arena_row), 500, 1), "101 rows taken");
    CHECK(!(cc_client_events(&c) & CC_EV_ARENA) && arena_hdr(&c).id == 13, "%s", "a dropped frame changed the arena");
}

static cw_chat_line chat_line(const char *room, const char *who, const char *text, int64_t at) {
    cw_chat_line l;
    memset(&l, 0, sizeof l);
    l.v = CW_V;
    l.kind = CW_P_CHAT;
    memcpy(l.room, room, strlen(room));
    memcpy(l.name, who, strlen(who));
    l.len = (uint8_t)strlen(text);
    memcpy(l.text, text, l.len);
    l.at = at;
    return l;
}
static void push_chat(cc_client *c, const char *room, const char *who, const char *text, int64_t at) {
    cw_chat_line l = chat_line(room, who, text, at);
    cc_client_in(c, (const uint8_t *)&l, sizeof l, 0);
}
static int chat_n(const cc_client *c, const char *room) {
    const cc_chat_room *r = cc_client_chat(c, room);
    return r ? r->n : -1;
}

static void chats(void) {
    static cc_client c;
    cc_client_init(&c);
    strcpy(c.name, "me");
    char path[300];
    TEST("a pushed line is kept in its room's slot");
    push_chat(&c, "g7", "ann", "gl hf", 100);
    CHECK(cc_client_events(&c) & CC_EV_CHAT, "%s", "no chat event");
    const cc_chat_room *r = cc_client_chat(&c, "g7");
    CHECK(r && r->n == 1 && !strcmp(r->l[0].text, "gl hf") && !strcmp(r->l[0].name, "ann") && r->l[0].at == 100, "the line");
    CHECK(cc_client_chat(&c, "g8") == NULL && cc_client_chat(&c, "") == NULL && cc_client_chat(&c, "g123456789012") == NULL, "no other room");
    TEST("three rooms at once; a fourth takes the slot touched longest ago");
    push_chat(&c, "lobby", "ben", "hi", 110);
    push_chat(&c, "a3", "cat", "go", 120);
    push_chat(&c, "g7", "ann", "again", 130);   // g7 touched last: lobby is now the oldest
    CHECK(chat_n(&c, "g7") == 2 && chat_n(&c, "lobby") == 1 && chat_n(&c, "a3") == 1, "three rooms");
    push_chat(&c, "g8", "dan", "new", 140);
    CHECK(chat_n(&c, "g8") == 1 && chat_n(&c, "lobby") == -1 && chat_n(&c, "g7") == 2 && chat_n(&c, "a3") == 1, "lobby given up: %d", chat_n(&c, "lobby"));
    TEST("a room keeps its last 64 lines");
    for (int i = 0; i < 65; i++) {
        char t[16];
        snprintf(t, sizeof t, "line %d", i);
        push_chat(&c, "a3", "cat", t, 200 + i);
    }
    r = cc_client_chat(&c, "a3");
    CHECK(r && r->n == CW_CHAT_LOG && !strcmp(r->l[0].text, "line 1") && !strcmp(r->l[63].text, "line 64"), "%s .. %s",
          r ? r->l[0].text : "-", r ? r->l[63].text : "-");
    TEST("a line of the wrong length, too long a text, or a room with no end is dropped");
    cc_client_events(&c);
    cw_chat_line bad = chat_line("g8", "x", "bad", 300);
    cc_client_in(&c, (const uint8_t *)&bad, sizeof bad - 1, 0);
    bad.len = CW_CHAT_MAX + 1;
    cc_client_in(&c, (const uint8_t *)&bad, sizeof bad, 0);
    bad = chat_line("g8", "x", "bad", 300);
    memset(bad.room, 'g', CW_ROOM);
    cc_client_in(&c, (const uint8_t *)&bad, sizeof bad, 0);
    CHECK(!(cc_client_events(&c) & CC_EV_CHAT) && chat_n(&c, "g8") == 1, "%d", chat_n(&c, "g8"));

    TEST("a log read: its lines, then those pushed meanwhile that it does not hold");
    drain(&c, path, NULL);
    cc_client_chat_log(&c, "g8");
    uint32_t log_id = c.next_id - 1;
    drain(&c, path, NULL);
    STREQ(path, "/c/chat/g8");
    push_chat(&c, "g8", "eve", "said meanwhile", 500);
    push_chat(&c, "g8", "eve", "also in the log", 450);
    static uint8_t lb[CW_CHAT_LOG_MAX];
    cw_chat_log h = { 3, { 0 } };
    memcpy(lb, &h, sizeof h);
    cw_chat_line l0 = chat_line("g8", "ann", "first", 50), l1 = chat_line("g8", "ben", "second", 60),
                 l2 = chat_line("g8", "eve", "also in the log", 450);
    memcpy(lb + 8, &l0, sizeof l0);
    memcpy(lb + 8 + sizeof l0, &l1, sizeof l1);
    memcpy(lb + 8 + 2 * sizeof l0, &l2, sizeof l2);
    cc_client_events(&c);
    reply(&c, log_id, 200, lb, 8 + 3 * sizeof l0, 0);
    CHECK(cc_client_events(&c) & CC_EV_CHAT, "%s", "no event");
    r = cc_client_chat(&c, "g8");
    CHECK(r && r->n == 4, "lines %d", r ? r->n : -1);
    if (r && r->n == 4)
        CHECK(!strcmp(r->l[0].text, "first") && !strcmp(r->l[1].text, "second") && !strcmp(r->l[2].text, "also in the log") &&
              !strcmp(r->l[3].text, "said meanwhile"), "order %s %s %s %s", r->l[0].text, r->l[1].text, r->l[2].text, r->l[3].text);
    TEST("a log for a room the client has not heard takes a slot");
    cc_client_chat_log(&c, "lobby");
    log_id = c.next_id - 1;
    drain(&c, path, NULL);
    h.n = 1;
    memcpy(lb, &h, sizeof h);
    cw_chat_line ll = chat_line("lobby", "ann", "welcome", 10);
    memcpy(lb + 8, &ll, sizeof ll);
    reply(&c, log_id, 200, lb, 8 + sizeof ll, 0);
    CHECK(chat_n(&c, "lobby") == 1, "lobby %d", chat_n(&c, "lobby"));
    TEST("a log of the wrong length is dropped; a room gone is no error");
    cc_client_chat_log(&c, "lobby");
    log_id = c.next_id - 1;
    drain(&c, path, NULL);
    cc_client_events(&c);
    reply(&c, log_id, 200, lb, 8 + sizeof ll - 1, 0);
    CHECK(chat_n(&c, "lobby") == 1 && !(cc_client_events(&c) & CC_EV_CHAT), "%s", "a short log was taken");
    cc_client_chat_log(&c, "g99");
    log_id = c.next_id - 1;
    drain(&c, path, NULL);
    cw_chat_err ce = { CW_E_CHAT_ROOM, 0, { 0, 0 }, 0 };
    reply(&c, log_id, 404, &ce, sizeof ce, 0);
    CHECK(!(cc_client_events(&c) & CC_EV_ERROR), "%s", "a 404 log raised an error");

    TEST("a line said: POST /c/chat with its room and text, the answer read as a read");
    cc_client_chat_say(&c, "g8", "gg", 2, 77);
    uint32_t say_id = c.next_id - 1;
    const uint8_t *f;
    size_t n = cc_client_next_out(&c, &f);
    sf_frame fr;
    cw_chat_req q;
    memset(&q, 0, sizeof q);
    CHECK(n && sf_parse(f, n, &fr) == 0 && fr.kind == SF_REQ && fr.path_len == 7 && !memcmp(fr.path, "/c/chat", 7) && fr.body_len == sizeof q,
          "the request");
    if (n && fr.body_len == sizeof q) memcpy(&q, fr.body, sizeof q);
    CHECK(!strcmp(q.room, "g8") && q.len == 2 && !memcmp(q.text, "gg", 2) && q.text[2] == 0, "the body: %s %u", q.room, q.len);
    ce.code = CW_E_CHAT_FAST;
    ce.wait_s = 1;
    reply(&c, say_id, 429, &ce, sizeof ce, 0);
    uint32_t ev = cc_client_events(&c);
    CHECK((ev & CC_EV_READ) && !(ev & CC_EV_ERROR) && c.read_tag == 77 && c.read_status == 429 && c.read_len == sizeof ce, "the answer");
    TEST("too long a line goes as 141 bytes, for the server to refuse");
    char big[200];
    memset(big, 'x', sizeof big);
    cc_client_chat_say(&c, "g8", big, sizeof big, 78);
    n = cc_client_next_out(&c, &f);
    CHECK(n && sf_parse(f, n, &fr) == 0 && fr.body_len == sizeof q && ((const cw_chat_req *)(const void *)fr.body)->len == CW_CHAT_MAX + 1, "len");
    TEST("a room name too long is not sent");
    cc_client_chat_say(&c, "g12345678901", "hi", 2, 79);   // 12 bytes: no room for the NUL
    cc_client_chat_log(&c, "g12345678901");
    CHECK(cc_client_next_out(&c, &f) == 0, "%s", "sent");
}

int main(void) {
    cc_client c;
    char path[300];
    cw_move_req mv;
    cc_client_init(&c);
    strcpy(c.name, "me");

    TEST("the snapshot");
    cc_client_open(&c, 7);
    uint32_t snap_id = c.next_id - 1;
    CHECK(drain(&c, path, NULL) == 2, "SUB and GET");
    STREQ(path, "/c/game/7");
    cw_snap s;
    memset(&s, 0, sizeof s);
    s.v = CW_V; s.kind = CW_SNAP; s.status = CC_PLAYING; s.winner = CC_NO_WINNER; s.game = 7; s.base_s = 60;
    s.left_ms[0] = s.left_ms[1] = 60000;
    strcpy(s.name[0], "me");
    strcpy(s.name[1], "them");
    s.arena = 4;
    reply(&c, snap_id, 200, &s, sizeof s, 1000);
    CHECK(c.synced && c.color == CH_WHITE && cc_client_my_turn(&c), "color %d", c.color);
    CHECK(c.game_arena == 4, "the game's arena %u", c.game_arena);

    TEST("our move: shown at once, its thinking time reported");
    CHECK(cc_client_move(&c, 12, 28, 0, 1700) == CC_OK, "e2e4");
    CHECK(c.g.ply == 1 && !cc_client_my_turn(&c), "ply %d", c.g.ply);
    drain(&c, path, &mv);
    STREQ(path, "/c/move");
    CHECK(mv.game == 7 && mv.ply == 0 && mv.move == CH_MOVE(12, 28, 0) && mv.think_ms == 700, "think %u", mv.think_ms);
    CHECK(cc_client_move(&c, 11, 27, 0, 1800) == CC_E_TURN, "moved twice");

    TEST("a premove, played the moment the other side's move arrives");
    CHECK(cc_client_premove_targets(&c, 6) == ((1ULL << 12) | (1ULL << 21) | (1ULL << 23)), "knight targets %llx", (unsigned long long)cc_client_premove_targets(&c, 6));
    CHECK(cc_client_premove_targets(&c, 62) == 0, "the other side's knight");
    CHECK(cc_client_premove(&c, 6, 30, 0) == CC_E_ILLEGAL, "g1g4 kept");
    CHECK(cc_client_premove(&c, 6, 21, 0) == CC_OK, "g1f3");
    push_state(&c, 7, 1, CH_MOVE(12, 28, 0), 1750);   /* our own move confirmed: the premove waits */
    CHECK(c.g.ply == 1 && c.premove, "premove spent on our own push");
    CHECK(drain(&c, path, NULL) == 0, "sent early");
    push_state(&c, 7, 2, uci(&c, "e7e5"), 2500);
    CHECK(c.g.ply == 3 && !c.premove, "ply %d", c.g.ply);
    drain(&c, path, &mv);
    STREQ(path, "/c/move");
    CHECK(mv.ply == 2 && mv.move == CH_MOVE(6, 21, 0) && mv.think_ms == 0, "premove sent as ply %d move %x think %u", mv.ply, mv.move, mv.think_ms);

    TEST("a pawn's premove capture: dropped when nothing came to take");
    push_state(&c, 7, 3, CH_MOVE(6, 21, 0), 2600);   /* our Nf3 confirmed: black to move */
    CHECK(cc_client_premove(&c, 28, 35, 0) == CC_OK, "%s", "exd5 (an empty square: a pawn may premove a capture)");
    push_state(&c, 7, 4, uci(&c, "b8c6"), 3000);
    CHECK(c.g.ply == 4 && !c.premove, "ply %d, premove left %x", c.g.ply, c.premove);
    CHECK(drain(&c, path, NULL) == 0, "%s", "an illegal premove was sent");
    CHECK(cc_client_move(&c, 5, 26, 0, 3100) == CC_OK, "Bc4");
    push_state(&c, 7, 5, CH_MOVE(5, 26, 0), 3150);
    TEST("...and played when it did");
    CHECK(cc_client_premove(&c, 26, 53, 0) == CC_OK, "%s", "Bxf7 premoved");
    push_state(&c, 7, 6, uci(&c, "a7a6"), 3500);   /* (d7d5 would have blocked it, and dropped it) */
    CHECK(c.g.ply == 7, "ply %d", c.g.ply);
    drain(&c, path, &mv);
    CHECK(mv.move == CH_MOVE(26, 53, 0) && mv.ply == 6, "sent move %x ply %d", mv.move, mv.ply);
    push_state(&c, 7, 7, CH_MOVE(26, 53, 0), 3550);
    push_state(&c, 7, 7, CH_MOVE(26, 53, 0), 3600);   /* a repeat of the same push changes nothing */
    CHECK(c.g.ply == 7, "ply %d", c.g.ply);
    cc_client_premove(&c, 21, 38, 0);
    cc_client_premove_cancel(&c);
    CHECK(!c.premove, "%s", "cancel");

    TEST("a push from the future asks for the snapshot again");
    drain(&c, path, NULL);
    push_state(&c, 7, 9, CH_MOVE(1, 18, 0), 4000);
    drain(&c, path, NULL);
    STREQ(path, "/c/game/7");
    CHECK(c.g.ply == 7, "ply %d", c.g.ply);

    TEST("another game's push is ignored");
    push_state(&c, 8, 8, CH_MOVE(1, 18, 0), 4000);
    CHECK(c.g.ply == 7, "ply %d", c.g.ply);

    TEST("a refused move resyncs");
    push_state(&c, 7, 8, uci(&c, "e8f7"), 4050);   /* Kxf7 */
    CHECK(c.g.ply == 8, "ply %d", c.g.ply);
    CHECK(cc_client_move(&c, 3, 12, 0, 4100) == CC_OK, "Qe2");
    uint32_t mv_id = c.next_id - 1;
    drain(&c, path, NULL);
    cw_err e = { CW_E_RULES, CC_E_PLY, { 0, 0 } };
    reply(&c, mv_id, 409, &e, sizeof e, 4200);
    CHECK(cc_client_events(&c) & CC_EV_ERROR, "%s", "no error event");
    drain(&c, path, NULL);
    STREQ(path, "/c/game/7");
    arenas();
    chats();
    return report("unit_client");
}

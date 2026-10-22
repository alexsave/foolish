// unit_client.c - the client without a server: frames built by hand go in, the frames it sends come
// out. A snapshot, a move shown before the server answers, the thinking time it reports, the
// other side's move, a premove played the moment that move arrives (or dropped when it became
// illegal), and a push from the future asking for the snapshot again.
#include "client.h"
#include "stream_frame.h"
#include "t.h"

static uint8_t buf[4096];

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
    reply(&c, snap_id, 200, &s, sizeof s, 1000);
    CHECK(c.synced && c.color == CH_WHITE && cc_client_my_turn(&c), "color %d", c.color);

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
    return report("unit_client");
}

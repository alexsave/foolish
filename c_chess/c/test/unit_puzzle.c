// unit_puzzle.c - a solve: the opponent's move played for the solver, right and wrong answers,
// the reply, any mate accepted, and records the generator could have got wrong refused.
#include "puzzle.h"
#include "t.h"

static ch_move u(const chess_pos *p, const char *s) { return ch_parse_uci(p, s, strlen(s)); }

/* A record from a FEN and a UCI line. */
static cp_puzzle make(const char *fen, const char *line) {
    cp_puzzle p;
    memset(&p, 0, sizeof p);
    snprintf(p.fen, sizeof p.fen, "%s", fen);
    chess_pos a, b;
    ch_from_fen(&a, fen);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", line);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {
        ch_move m = u(&a, t);
        p.moves[p.n++] = m;
        if (m != CH_MOVE_NONE) { ch_make(&a, m, &b); a = b; }
    }
    return p;
}

static void solves(void) {
    cp_solve s;
    TEST("a fork: right, reply, right");
    /* White's Ng5 (moves[0]) and Black's d5 line, as a record: two solver moves with a reply between */
    cp_puzzle p = make("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
                       "f3g5 d7d5 e4d5 f6d5");
    CHECK(cp_begin(&s, &p) == 0, "begin");
    CHECK(s.pos.stm == CH_BLACK, "solver is black");
    CHECK(cp_try(&s, u(&s.pos, "d7d5")) == CP_SOLVING, "first right");
    CHECK(s.last[1] == p.moves[2], "reply played");
    CHECK(cp_try(&s, u(&s.pos, "f6d5")) == CP_SOLVED, "solved");
    CHECK(cp_try(&s, CH_MOVE_NONE) == CP_SOLVED, "stays solved");

    TEST("a wrong move fails and leaves the board");
    cp_begin(&s, &p);
    chess_pos before = s.pos;
    CHECK(cp_try(&s, u(&s.pos, "h7h6")) == CP_FAILED, "wrong");
    CHECK(s.pos.key == before.key, "board moved on a wrong answer");
    CHECK(cp_try(&s, u(&s.pos, "d7d5")) == CP_FAILED, "stays failed");

    TEST("a failed solve shows the line to its end");
    cp_begin(&s, &p);
    cp_try(&s, u(&s.pos, "h7h6"));
    ch_move shown[CP_MOVES];
    int k = 0;
    while (k < CP_MOVES && cp_reveal(&s, &shown[k])) k++;
    CHECK(k == 3 && shown[0] == p.moves[1] && shown[1] == p.moves[2] && shown[2] == p.moves[3], "shown %d", k);
    CHECK(s.state == CP_FAILED, "a shown line stays failed");
    cp_begin(&s, &p);
    cp_try(&s, u(&s.pos, "d7d5"));
    CHECK(cp_reveal(&s, &shown[0]) && shown[0] == p.moves[3], "after a right move, the next is the solver's");

    TEST("any mate is accepted");
    /* a back rank where Rd8 (the line's) and Re8 both mate */
    cp_puzzle m = make("6k1/p4ppp/8/8/8/8/5PPP/3RR1K1 b - - 0 1", "a7a6 d1d8");
    CHECK(cp_begin(&s, &m) == 0, "begin");
    CHECK(cp_try(&s, u(&s.pos, "e1e8")) == CP_SOLVED, "the other mate");
    cp_puzzle longer = make("6k1/p4ppp/8/8/8/8/5PPP/3RR1K1 b - - 0 1", "a7a6 d1d7 a6a5 d7d8");
    CHECK(cp_begin(&s, &longer) == 0, "begin");
    CHECK(cp_try(&s, u(&s.pos, "e1e8")) == CP_SOLVED, "a mate before the line's end ends it");
    cp_begin(&s, &m);
    CHECK(cp_try(&s, u(&s.pos, "d1d7")) == CP_FAILED, "a non-mate is not accepted");
    cp_begin(&s, &m);
    CHECK(cp_try(&s, u(&s.pos, "d1d8")) == CP_SOLVED, "the line's mate");

    TEST("a check that is not mate, and an illegal move, are wrong");
    cp_puzzle c = make("4k3/8/8/8/8/8/8/3QK3 b - - 0 1", "e8f8 d1d7");
    cp_begin(&s, &c);
    CHECK(cp_try(&s, u(&s.pos, "d1f3")) == CP_FAILED, "a check accepted as a mate");
    cp_begin(&s, &m);   /* Re1-d8 is no rook move, though the rook would mate there */
    CHECK(cp_try(&s, CH_MOVE(4, 59, 0)) == CP_FAILED, "an illegal move accepted");

    TEST("bad records refused");
    cp_puzzle bad = p;
    bad.moves[3] = bad.moves[1];   /* d7d5 again: illegal there */
    CHECK(cp_begin(&s, &bad) == -1, "illegal line accepted");
    bad = p;
    snprintf(bad.fen, sizeof bad.fen, "not a fen");
    CHECK(cp_begin(&s, &bad) == -1, "bad fen accepted");
    bad = p;
    bad.n = 1;
    CHECK(cp_begin(&s, &bad) == -1, "a line of one accepted");
    /* thirteen legal moves, the last in the bytes after the line (the rating): past CP_MOVES */
    bad = make("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
               "e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 d2d3 f8c5 c2c3 d7d6 e1g1 e8g8");
    CHECK(bad.n == CP_MOVES && cp_begin(&s, &bad) == 0, "twelve accepted");
    {
        chess_pos a, b;
        ch_from_fen(&a, bad.fen);
        for (int i = 0; i < CP_MOVES; i++) { ch_make(&a, bad.moves[i], &b); a = b; }
        uint16_t h3 = u(&a, "h2h3");
        memcpy(&bad.rating, &h3, sizeof h3);
    }
    bad.n = CP_MOVES + 1;
    CHECK(cp_begin(&s, &bad) == -1, "a line too long accepted");
    bad = p;
    memset(bad.fen, 'x', sizeof bad.fen);   /* no NUL: read as 92 bytes, not past them */
    CHECK(cp_begin(&s, &bad) == -1, "an unterminated FEN accepted");
}

int main(void) {
    ch_init();
    solves();
    return report("unit_puzzle");
}

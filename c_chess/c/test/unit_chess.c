// unit_chess.c - the rules: perft node counts against the published values (the six positions of
// the Chess Programming Wiki's "Perft Results" page), FEN round trips, UCI and SAN, the
// en-passant square in FEN, repetition keys, and insufficient material.
#include "chess.h"
#include "t.h"

typedef struct { const char *fen; int depth; uint64_t nodes; } perft_case;

static const perft_case PERFT[] = {
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 1, 20 },
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 3, 8902 },
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609 },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 1, 48 },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2, 2039 },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603 },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624 },
    { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },
    { "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1", 4, 422333 },   /* the mirror */
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487 },
    { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594 },
};

static void play(chess_pos *p, const char *uci) {
    ch_move m = ch_parse_uci(p, uci, strlen(uci));
    CHECK(m != CH_MOVE_NONE, "illegal %s", uci);
    chess_pos q;
    ch_make(p, m, &q);
    *p = q;
}

static void san_of(const char *fen, const char *uci, const char *want) {
    chess_pos p;
    CHECK(ch_from_fen(&p, fen) == 0, "%s", "");
    ch_move m = ch_parse_uci(&p, uci, strlen(uci));
    CHECK(m != CH_MOVE_NONE, "illegal %s in %s", uci, fen);
    char san[8];
    ch_san(&p, m, san);
    STREQ(san, want);
}

int main(void) {
    ch_init();
    for (size_t i = 0; i < sizeof PERFT / sizeof *PERFT; i++) {
        chess_pos p;
        CHECK(ch_from_fen(&p, PERFT[i].fen) == 0, "%s", "");
        uint64_t n = ch_perft(&p, PERFT[i].depth);
        CHECK(n == PERFT[i].nodes, "perft(%d) of %s = %llu, want %llu", PERFT[i].depth, PERFT[i].fen,
                    (unsigned long long)n, (unsigned long long)PERFT[i].nodes);
    }

    /* FEN round trips, and the en-passant square written only when the capture is legal */
    char fen[100];
    chess_pos p;
    ch_startpos(&p);
    ch_to_fen(&p, fen);
    STREQ(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    play(&p, "e2e4");
    ch_to_fen(&p, fen);
    STREQ(fen, "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    play(&p, "d7d5"); play(&p, "e4e5"); play(&p, "f7f5");
    ch_to_fen(&p, fen);
    STREQ(fen, "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3");
    play(&p, "e5f6");   /* en passant */
    ch_to_fen(&p, fen);
    STREQ(fen, "rnbqkbnr/ppp1p1pp/5P2/3p4/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 3");
    /* a pinned pawn cannot take en passant, so no square is written */
    CHECK(ch_from_fen(&p, "8/8/8/KPp4r/8/8/8/7k w - c6 0 1") == 0, "%s", "");
    ch_to_fen(&p, fen);
    STREQ(fen, "8/8/8/KPp4r/8/8/8/7k w - - 0 1");
    CHECK(ch_from_fen(&p, "8/8/8/1Pp4r/8/8/K7/7k w - c6 0 1") == 0, "%s", "");
    ch_to_fen(&p, fen);
    STREQ(fen, "8/8/8/1Pp4r/8/8/K7/7k w - c6 0 1");
    const char *bad[] = { "", "8/8/8/8/8/8/8/8 w - - 0 1", "rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                          "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",
                          "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e5 0 1", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - x 1" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(ch_from_fen(&p, bad[i]) != 0, "accepted %s", bad[i]);

    /* UCI: castling both ways, promotion letters, nonsense */
    CHECK(ch_from_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1") == 0, "%s", "");
    CHECK(ch_parse_uci(&p, "e1g1", 4) == CH_MOVE(4, 6, 0), "%s", "");
    CHECK(ch_parse_uci(&p, "e1h1", 4) == CH_MOVE(4, 6, 0), "%s", "");
    CHECK(ch_parse_uci(&p, "e1a1", 4) == CH_MOVE(4, 2, 0), "%s", "");
    CHECK(ch_parse_uci(&p, "e1c1", 4) == CH_MOVE(4, 2, 0), "%s", "");
    CHECK(ch_parse_uci(&p, "e1e3", 4) == CH_MOVE_NONE, "%s", "");
    CHECK(ch_parse_uci(&p, "z1e3", 4) == CH_MOVE_NONE, "%s", "");
    CHECK(ch_parse_uci(&p, "e1g1q", 5) == CH_MOVE_NONE, "%s", "");
    CHECK(ch_from_fen(&p, "8/P7/8/8/8/8/8/k6K w - - 0 1") == 0, "%s", "");
    CHECK(ch_parse_uci(&p, "a7a8n", 5) == CH_MOVE(48, 56, CH_KNIGHT), "%s", "");
    CHECK(ch_parse_uci(&p, "a7a8", 4) == CH_MOVE_NONE, "%s", "");
    CHECK(ch_parse_uci(&p, "a7a8k", 5) == CH_MOVE_NONE, "%s", "");
    char u[6];
    CHECK(ch_uci(CH_MOVE(48, 56, CH_QUEEN), u) == 5, "%s", "");
    STREQ(u, "a7a8q");
    /* castling rights go when the king or a rook moves, or a rook is taken at home */
    CHECK(ch_from_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1") == 0, "%s", "");
    play(&p, "a1a8");
    ch_to_fen(&p, fen);
    STREQ(fen, "R3k2r/8/8/8/8/8/8/4K2R b Kk - 0 1");
    play(&p, "e8f7");
    ch_to_fen(&p, fen);
    STREQ(fen, "R6r/5k2/8/8/8/8/8/4K2R w K - 1 2");

    /* SAN: disambiguation by file, by rank, by both; captures, promotions, castling, check, mate */
    san_of("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "g1f3", "Nf3");
    san_of("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1", "e1g1", "O-O");
    san_of("4k3/8/8/8/8/8/8/R3K2R w KQ - 0 1", "e1c1", "O-O-O");
    san_of("4k3/8/8/8/8/8/8/R4RK1 w - - 0 1", "a1d1", "Rad1");
    san_of("4k3/8/8/R7/8/8/8/R5K1 w - - 0 1", "a1a3", "R1a3");
    san_of("7k/8/8/8/2Q1Q3/8/2Q5/6K1 w - - 0 1", "c4d3", "Qc4d3");
    san_of("4k3/8/8/3p4/4P3/8/8/6K1 w - - 0 1", "e4d5", "exd5");
    san_of("4k3/1P6/8/8/8/8/8/6K1 w - - 0 1", "b7b8q", "b8=Q+");
    san_of("2r1k3/1P6/8/8/8/8/8/6K1 w - - 0 1", "b7c8n", "bxc8=N");
    san_of("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", "a1a8", "Ra8#");
    san_of("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3", "e5f6", "exf6");

    /* repetition: the same position by a different route has the same key; ep counts only when
     * a capture exists */
    chess_pos a, b;
    ch_startpos(&a);
    play(&a, "g1f3"); play(&a, "g8f6"); play(&a, "f3g1"); play(&a, "f6g8");
    ch_startpos(&b);
    CHECK(a.key == b.key, "%s", "");
    play(&a, "e2e4");
    ch_from_fen(&b, "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    CHECK(a.key == b.key, "%s", "");   /* no black pawn can take on e3 */
    ch_from_fen(&a, "rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    ch_from_fen(&b, "rnbqkbnr/ppp1pppp/8/8/3pP3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    CHECK(a.key != b.key, "%s", "");
    /* a FEN's en-passant square that no pawn can use is the same position as without it */
    ch_from_fen(&a, "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    ch_from_fen(&b, "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    CHECK(a.key == b.key, "%s", "an unusable en-passant square changed the key");
    ch_from_fen(&b, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    ch_startpos(&a);
    CHECK(a.key != b.key, "%s", "");   /* the side to move counts */

    /* check, mate, stalemate */
    ch_moves ms;
    ch_from_fen(&p, "rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
    ch_legal(&p, &ms);
    CHECK(ch_in_check(&p) && ms.n == 0, "%s", "");
    ch_from_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    ch_legal(&p, &ms);
    CHECK(!ch_in_check(&p) && ms.n == 0, "%s", "");

    /* insufficient material */
    const struct { const char *fen; bool both; bool white_alone; } MAT[] = {
        { "4k3/8/8/8/8/8/8/4K3 w - - 0 1", true, true },
        { "4k3/8/8/8/8/8/8/4KN2 w - - 0 1", true, true },
        { "4k3/8/8/8/8/8/8/4KB2 w - - 0 1", true, true },
        { "4kb2/8/8/8/8/8/8/4KB2 w - - 0 1", false, true },     /* f8 dark, f1 light */
        { "4k1b1/8/8/8/8/8/8/4KB2 w - - 0 1", true, true },     /* g8 light, f1 light */
        { "4kn2/8/8/8/8/8/8/4KN2 w - - 0 1", false, true },
        { "4k3/8/8/8/8/8/8/3NKN2 w - - 0 1", false, false },
        { "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1", false, false },
        { "4k3/p7/8/8/8/8/8/4K3 w - - 0 1", false, true },
    };
    for (size_t i = 0; i < sizeof MAT / sizeof *MAT; i++) {
        CHECK(ch_from_fen(&p, MAT[i].fen) == 0, "%s", "");
        CHECK(ch_insufficient(&p) == MAT[i].both, "insufficient(%s)", MAT[i].fen);
        CHECK(ch_insufficient_for(&p, CH_WHITE) == MAT[i].white_alone, "insufficient_for white (%s)", MAT[i].fen);
    }
    return report("unit_chess");
}

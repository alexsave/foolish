// unit_puzzles.c - the server's puzzles without a server: a file read (bad and repeated records
// dropped), picks near the player's rating and never one of the last it saw, the order by rating
// kept as ratings move, and an answer accepted only for the puzzle handed out, once.
#include "puzzles.h"
#include "util/err.h"
#include "../../c/test/t.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define N 201   // ratings 500, 510, ... 2500

static char g_path[256];

static cp_puzzle rec(int i) {
    cp_puzzle p;
    memset(&p, 0, sizeof p);
    snprintf(p.fen, sizeof p.fen, "6k1/p4ppp/8/8/8/8/5PPP/3RR1K1 b - - 0 %d", i + 1);   // one position, many move numbers
    chess_pos a, b;
    ch_from_fen(&a, p.fen);
    p.moves[0] = ch_parse_uci(&a, "a7a6", 4);
    ch_make(&a, p.moves[0], &b);
    p.moves[1] = ch_parse_uci(&b, "d1d8", 4);
    p.n = 2;
    p.rating = (int16_t)(500 + 10 * i);
    p.rd = 80;
    return p;
}

static void write_file(void) {
    snprintf(g_path, sizeof g_path, "/tmp/unit_puzzles_%d.bin", (int)getpid());
    FILE *f = fopen(g_path, "wb");
    uint32_t n = N + 2;
    fwrite(PUZZLE_FILE_MAGIC, 8, 1, f);
    fwrite(&n, 4, 1, f);
    for (int i = N - 1; i >= 0; i--) { cp_puzzle p = rec(i); fwrite(&p, sizeof p, 1, f); }   // highest first: the load sorts
    cp_puzzle bad = rec(0);
    bad.moves[1] = CH_MOVE(3, 63, 0);
    fwrite(&bad, sizeof bad, 1, f);
    cp_puzzle again = rec(7);
    fwrite(&again, sizeof again, 1, f);
    fclose(f);
}

static cc_player fresh(double rating) {
    cc_player p;
    memset(&p, 0, sizeof p);
    p.pz = cc_rating_new();
    p.pz.rating = rating;
    p.pz.deviation = 80;
    return p;
}

static void loading(void) {
    char err[256];
    TEST("a file read");
    CHECK(cc_puzzles_load(g_path, err, sizeof err) == CB_OK, "%s", err);
    CHECK(cc_puzzles_count() == N, "count %u", cc_puzzles_count());
    CHECK(cc_puzzles_ordered(), "not in order after the load");
    TEST("not a puzzle file");
    CHECK(cc_puzzles_load("/etc/hosts", err, sizeof err) != CB_OK, "read /etc/hosts");
    CHECK(cc_puzzles_count() == 0, "count %u", cc_puzzles_count());
    CHECK(cc_puzzles_load(g_path, err, sizeof err) == CB_OK, "%s", err);
}

static void picking(void) {
    TEST("near the rating");
    for (int t = 0; t < 50; t++) {
        cc_player p = fresh(900 + 20 * t);
        uint32_t plays;
        const cp_puzzle *z = cc_puzzle_pick(&p, &plays, (uint32_t)t * 7919u);
        CHECK(z && abs(z->rating - (int)p.pz.rating) <= 250, "rating %d for a player at %.0f", z ? z->rating : 0, p.pz.rating);
        CHECK(p.pz_open == z->id, "the open puzzle");
    }
    TEST("not one of the last seen");
    for (int t = 0; t < 20; t++) {
        cc_player p = fresh(1500);
        uint32_t ids[16], plays;
        for (int k = 0; k < 16; k++) {
            ids[k] = cc_puzzle_pick(&p, &plays, (uint32_t)(t * 100 + k))->id;
            for (int j = 0; j < k; j++) CHECK(ids[j] != ids[k], "pick %d repeats pick %d", k, j);
        }
    }
    TEST("past the ends of the ratings");
    cc_player lo = fresh(100), hi = fresh(3200);
    uint32_t plays;
    for (uint32_t t = 0; t < 40; t++) {   // every offset from the ends (the asan build sees a read past them)
        lo.pz_open = hi.pz_open = 0;
        memset(lo.pz_seen, 0, sizeof lo.pz_seen);
        memset(hi.pz_seen, 0, sizeof hi.pz_seen);
        CHECK(cc_puzzle_pick(&lo, &plays, t)->rating <= 500 + 10 * 25, "low");
        CHECK(cc_puzzle_pick(&hi, &plays, t)->rating >= 2500 - 10 * 25, "high");
    }
}

static void answering(void) {
    TEST("once, and only the one handed out");
    cc_player p = fresh(1500);
    uint32_t plays;
    const cp_puzzle *z = cc_puzzle_pick(&p, &plays, 3);
    uint32_t id = z->id;
    int16_t pr = z->rating;
    cw_puzzle_done d;
    CHECK(!cc_puzzle_result(&p, id + 1, true, &d), "another id answered");
    CHECK(!cc_puzzle_result(&p, 0, true, &d), "id 0 answered");
    CHECK(cc_puzzle_result(&p, id, true, &d), "the one handed out refused");
    CHECK(d.after > d.before && d.puzzle < pr, "a win: %d -> %d, the puzzle %d -> %d", d.before, d.after, pr, d.puzzle);
    CHECK(d.tried == 1 && d.solved == 1, "counts %u %u", d.tried, d.solved);
    CHECK(!cc_puzzle_result(&p, id, true, &d), "answered twice");
    z = cc_puzzle_pick(&p, &plays, 4);
    pr = z->rating;
    CHECK(cc_puzzle_result(&p, z->id, false, &d), "a loss refused");
    CHECK(d.after < d.before && d.puzzle > pr, "a loss: %d -> %d, the puzzle %d -> %d", d.before, d.after, pr, d.puzzle);
    CHECK(d.tried == 2 && d.solved == 1, "counts %u %u", d.tried, d.solved);

    TEST("the order holds as ratings move");
    for (int t = 0; t < 400; t++) {
        cc_player q = fresh(800 + (t * 37) % 1400);
        const cp_puzzle *y = cc_puzzle_pick(&q, &plays, (uint32_t)t);
        cc_puzzle_result(&q, y->id, t % 3 != 0, &d);
    }
    CHECK(cc_puzzles_ordered(), "out of order after 400 answers");

    TEST("a journaled rating lands on its puzzle");
    g2_player r = { 2222, 60, 0.06 };
    cc_puzzle_recovered(id, &r, 9);
    cc_player w = fresh(2222);
    bool seen_it = false;
    for (int t = 0; t < 200 && !seen_it; t++) {
        w.pz_seen_at = 0;
        memset(w.pz_seen, 0, sizeof w.pz_seen);
        const cp_puzzle *y = cc_puzzle_pick(&w, &plays, (uint32_t)t);
        if (y->id == id) { seen_it = true; CHECK(y->rating == 2222 && plays == 9, "rating %d plays %u", y->rating, plays); }
    }
    CHECK(seen_it, "the recovered puzzle is not near its new rating");
    CHECK(cc_puzzles_ordered(), "out of order after recovery");
    cc_puzzle_recovered(12345, &r, 1);   // a puzzle the file no longer has: nothing
    CHECK(cc_puzzles_ordered(), "out of order after an unknown id");
}

int main(void) {
    ch_init();
    write_file();
    loading();
    picking();
    answering();
    cc_puzzles_free();
    unlink(g_path);
    return report("unit_puzzles");
}

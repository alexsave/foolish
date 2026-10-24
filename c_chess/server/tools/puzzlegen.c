// puzzlegen.c - makes puzzles from games the engine plays against itself: weakened players (low
// "Skill Level", few nodes) blunder the way people do; a full-strength look at every position finds
// the moves where one side could win and only one move does it; the line that follows, as long as
// each of the solver's moves stays the only good one, is the puzzle.
//
//   CC_ENGINE=<stockfish> puzzlegen <out.bin> <count> [engines]
//   puzzlegen --show <file>            each puzzle as its FEN and its line in SAN
//
// The engines run in the foreground (an offline tool). The output is puzzle.h's file.
#include "engine.h"
#include "puzzle.h"
#include "game.h"

#include "util/err.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int done; cc_engine_answer a; } waiter;

static void got(void *ud, const cc_engine_answer *a) {
    waiter *w = ud;
    pthread_mutex_lock(&w->mu);
    w->a = *a;
    w->done = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

/* One search, waited for. */
static cc_engine_answer search(const char *fen, const ch_move *moves, int n, int skill, uint64_t nodes, int multipv) {
    waiter w = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, { 0 } };
    cc_engine_ask ask = { moves, n, skill, 10000, 0, nodes, multipv, fen };
    while (cc_engine_search(&ask, got, &w) != CB_OK) { struct timespec t = { 0, 10000000 }; nanosleep(&t, NULL); }
    pthread_mutex_lock(&w.mu);
    while (!w.done) pthread_cond_wait(&w.cv, &w.mu);
    pthread_mutex_unlock(&w.mu);
    return w.a;
}

/* A score as one number for the side to move: mate is big, sooner bigger. */
static int value(int cp, int mate) { return mate > 0 ? 100000 - mate : mate < 0 ? -100000 - mate : cp; }

static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *OUT;
static int made, want, games, VERBOSE;

#define SCAN_NODES 50000    /* every position of a game: is the side to move suddenly winning? */
#define LOOK_NODES 300000   /* a candidate's line: is each move the only one? */

/* From position `fen` + moves[0..n), where the side to move could win: the line, each solver move
 * kept only while it is the only good one. Returns the line's length (0: not a puzzle). */
static int line_from(const char *fen, ch_move *moves, int n, ch_move *line) {
    int k = 0;
    ch_move m[CC_MAX_PLY + 2 * CP_MOVES];
    memcpy(m, moves, sizeof *m * (size_t)n);
    int len = n;
    for (int step = 0; step < 4; step++) {
        cc_engine_answer a = search(fen, m, len, 20, LOOK_NODES, 2);
        if (a.best == CH_MOVE_NONE || a.nmulti < 1) break;
        int best = value(a.multi[0].score_cp, a.multi[0].mate);
        int second = a.nmulti >= 2 ? value(a.multi[1].score_cp, a.multi[1].mate) : -100000;
        bool mate_now = a.multi[0].mate == 1;
        bool only = mate_now || (best >= 250 && best - second >= 250) || (a.multi[0].mate > 0 && second < 400);
        if (!only) break;
        line[k++] = a.multi[0].move;
        m[len++] = a.multi[0].move;
        if (mate_now || k + 2 > CP_MOVES - 1) break;
        cc_engine_answer r = search(fen, m, len, 20, LOOK_NODES / 2, 1);   /* the defender's best reply */
        if (r.best == CH_MOVE_NONE) break;
        line[k++] = r.best;
        m[len++] = r.best;
    }
    /* end on the solver's move */
    if (k % 2 == 0 && k) k--;
    return k;
}

static void *worker(void *ud) {
    (void)ud;
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)&ud;
    while (1) {
        pthread_mutex_lock(&out_mu);
        int left = want - made;
        games++;
        pthread_mutex_unlock(&out_mu);
        if (left <= 0) return NULL;
        /* a game between two weakened players */
        int skill[2] = { 1 + rand_r(&seed) % 9, 1 + rand_r(&seed) % 9 };
        ch_move moves[CC_MAX_PLY];
        int prev = 0;   /* the last full-strength look, from the view of the side to move next */
        cc_game g;
        cc_game_new(&g, 0, 0, 0);
        g.left_ms[0] = g.left_ms[1] = INT64_MAX / 4;
        int n = 0;
        while (n < 160 && !cc_game_over(&g)) {
            cc_engine_answer a = search(NULL, moves, n, skill[n & 1], 3000 + (uint64_t)(rand_r(&seed) % 20000), 1);
            if (a.best == CH_MOVE_NONE || cc_game_move(&g, g.pos.stm, n, a.best, 0) != CC_OK) break;
            moves[n++] = a.best;
            if (n < 12) continue;
            /* the side to move now: could it win, where a moment ago its opponent was fine? */
            cc_engine_answer look = search(NULL, moves, n, 20, SCAN_NODES, 1);   /* a quick look; a candidate gets a deep one */
            if (look.best == CH_MOVE_NONE) continue;
            int now = value(look.score_cp, look.mate);
            if (VERBOSE) fprintf(stderr, "ply %d now %d was %d\n", n, now, prev);
            int was = prev;   /* the position before the opponent's move, from the side to move's view */
            prev = -now;      /* the next mover's view of this one */
            if (now > 1500 || now < -1500) break;     /* decided: what follows is mopping up */
            if (now < 250 || was > 150) continue;   /* not winning now, or already winning before the blunder */
            ch_move line[CP_MOVES];
            int k = line_from(NULL, moves, n, line);
            if (k < 1 || (k == 1 && look.mate != 1)) continue;   /* one move only when it mates */
            cp_puzzle p;
            memset(&p, 0, sizeof p);
            chess_pos s, q;
            ch_startpos(&s);
            for (int i = 0; i < n - 1; i++) { ch_make(&s, moves[i], &q); s = q; }
            ch_to_fen(&s, p.fen);
            p.moves[0] = moves[n - 1];   /* the blunder */
            for (int i = 0; i < k; i++) p.moves[1 + i] = line[i];
            p.n = (uint8_t)(1 + k);
            p.themes = (uint8_t)((look.mate > 0 ? CP_T_MATE : 0) | (k == 1 ? CP_T_ONEMOVE : k <= 3 ? CP_T_SHORT : CP_T_LONG));
            p.rating = (int16_t)(1100 + 150 * k + (look.mate > 0 ? 0 : 150));
            p.rd = 350;
            pthread_mutex_lock(&out_mu);
            if (made < want) {
                p.id = (uint32_t)++made;
                fwrite(&p, sizeof p, 1, OUT);
                if (VERBOSE || made % 25 == 0) {   /* the count too: the file is whole at every 25 */
                    uint32_t c = (uint32_t)made;
                    fseek(OUT, 8, SEEK_SET);
                    fwrite(&c, 4, 1, OUT);
                    fseek(OUT, 0, SEEK_END);
                    fflush(OUT);
                    fprintf(stderr, "%d puzzles from %d games\n", made, games);
                }
            }
            pthread_mutex_unlock(&out_mu);
            break;   /* one puzzle a game: the rest of a game after a blunder is lopsided */
        }
    }
}

static int show(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    char magic[8];
    uint32_t count;
    if (fread(magic, 8, 1, f) != 1 || memcmp(magic, PUZZLE_FILE_MAGIC, 8) != 0 || fread(&count, 4, 1, f) != 1) { fprintf(stderr, "%s: not a puzzle file\n", path); fclose(f); return 1; }
    cp_puzzle p;
    for (uint32_t i = 0; i < count && fread(&p, sizeof p, 1, f) == 1; i++) {
        chess_pos a, b;
        printf("%4u %4d %s |", i, p.rating, p.fen);
        if (ch_from_fen(&a, p.fen) != 0) { printf(" bad fen\n"); continue; }
        for (int k = 0; k < p.n; k++) {
            char san[8];
            if (!ch_is_legal(&a, p.moves[k])) { printf(" (illegal)"); break; }
            san[ch_san(&a, p.moves[k], san)] = 0;
            printf(" %s%s", k == 0 ? "[" : "", san);
            if (k == 0) printf("]");
            ch_make(&a, p.moves[k], &b);
            a = b;
        }
        printf("\n");
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    ch_init();
    if (argc == 3 && strcmp(argv[1], "--show") == 0) return show(argv[2]);
    if (argc < 3) { fprintf(stderr, "usage: CC_ENGINE=<stockfish> puzzlegen <out.bin> <count> [engines]\n"); return 2; }
    setenv("CC_ENGINE_FOREGROUND", "1", 1);
    int engines = argc > 3 ? atoi(argv[3]) : 4;
    char n[16];
    snprintf(n, sizeof n, "%d", engines);
    setenv("CC_ENGINES", n, 1);
    ch_init();
    if (cc_engine_start() != CB_OK || !cc_engine_available()) { fprintf(stderr, "puzzlegen: no engine (CC_ENGINE)\n"); return 1; }
    want = atoi(argv[2]);
    VERBOSE = getenv("PUZZLEGEN_VERBOSE") != NULL;
    if (!(OUT = fopen(argv[1], "wb"))) { perror(argv[1]); return 1; }
    uint32_t count = 0;
    fwrite(PUZZLE_FILE_MAGIC, 8, 1, OUT);
    fwrite(&count, 4, 1, OUT);
    pthread_t th[64];
    int workers = engines * 2 > 64 ? 64 : engines * 2;   /* two waiting per engine keeps each busy */
    for (int i = 0; i < workers; i++) pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < workers; i++) pthread_join(th[i], NULL);
    count = (uint32_t)made;
    fseek(OUT, 8, SEEK_SET);
    fwrite(&count, 4, 1, OUT);
    fclose(OUT);
    fprintf(stderr, "puzzlegen: %d puzzles from %d games\n", made, games);
    cc_engine_stop();
    return 0;
}

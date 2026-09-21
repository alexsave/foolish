/* The mate search against an exhaustive one: two independent answers to one
 * question, which is the only kind of check worth having for a proof.
 *
 *     make -C uttt/c mate
 *
 * `uttt_mate_in` is an AND/OR search with a node budget and iterative
 * deepening. This file re-answers the same question the slow obvious way -
 * full minimax, no budget, no deepening - on positions small enough to
 * exhaust, and compares BOTH the verdict and the DISTANCE.
 *
 * THE DISTANCE IS THE HALF THAT MATTERS. A first version of this checked
 * only "is it a win", and a mutation that turned the opponent's AND node
 * into an OR node - one escape being enough instead of none - sailed
 * straight through it: the verdict stayed right and the ply count came back
 * short, which is a bot playing the wrong move for a stated reason that is
 * false. The positions also have to be DEEP enough to tell the two apart. At
 * eleven empty cells every proved mate was mate in one or three, where the
 * shortest and the longest reply are the same number; it takes fifteen to
 * see a mate in five, and that is where the mutation dies. */
#include "../src/uttt.h"
#include "../src/uttt_bots.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t RS = 0x243F6A8885A308D3ull;
static uint32_t rnd(uint32_t n)
{ RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return (uint32_t)(RS % n); }

/* An independent exact solver: +1 win for side to move, 0 draw, -1 loss. */
static int solve(UtttGame *g)
{
    if (g->over) return g->over == UTTT_DRAW ? 0 : (g->over == g->turn ? 1 : -1);
    uint8_t list[81];
    int n = uttt_legal(g, list), best = -1;
    for (int i = 0; i < n; i++) {
        UtttGame t = *g; uttt_play(&t, list[i]);
        int v = -solve(&t);
        if (v > best) best = v;
        if (best == 1) break;
    }
    return n ? best : 0;
}

/* The exact distance to a forced win for the side to move, exhaustively, or
 * MATE_NONE. An independent second opinion on the LENGTH and not just the
 * verdict - a search that gets the verdict right and the distance wrong
 * still picks the wrong move, and a check that only asks "is it a win"
 * cannot see that. */
#define MATE_NONE 9999
static int exact_mate(UtttGame *g)
{
    if (g->over) return MATE_NONE;              /* nobody moves from here */
    uint8_t list[81];
    int n = uttt_legal(g, list), best = MATE_NONE;
    const uint8_t me = g->turn;
    for (int i = 0; i < n; i++) {
        UtttGame t = *g; uttt_play(&t, list[i]);
        if (t.over) { if (t.over == me && best > 1) best = 1; continue; }
        uint8_t rl[81];
        int m = uttt_legal(&t, rl), worst = 0;
        for (int j = 0; j < m; j++) {
            UtttGame u = t; uttt_play(&u, rl[j]);
            if (u.over) { worst = MATE_NONE; break; }
            int v = exact_mate(&u);
            if (v >= MATE_NONE) { worst = MATE_NONE; break; }
            if (v > worst) worst = v;
        }
        if (worst < MATE_NONE && 2 + worst < best) best = 2 + worst;
    }
    return best;
}

static int empties(const UtttGame *g)
{ int e = 0; for (int i = 0; i < 81; i++) if (!g->cell[i]) e++; return e; }

int main(int argc, char **argv)
{
    int want = argc > 1 ? atoi(argv[1]) : 400;
    long budget = argc > 2 ? atol(argv[2]) : 40000000L;
    int checked = 0, false_pos = 0, missed = 0, found = 0, wins = 0, wrong_len = 0;
    int hist[16]; memset(hist, 0, sizeof hist);
    while (checked < want) {
        UtttGame g; uttt_init(&g);
        uint8_t list[81];
        int target = 56 + (int)rnd(12);
        while (g.n_plies < target && !g.over) {
            int n = uttt_legal(&g, list);
            if (n <= 0) break;
            uttt_play(&g, list[rnd((uint32_t)n)]);
        }
        if (g.over || empties(&g) > 15 || empties(&g) < 10) continue;
        checked++;

        UtttGame t = g;
        int truth = solve(&t);                 /* exhaustive, no budget */
        uint8_t mv = 0;
        int d = uttt_mate_in(&g, budget, &mv);

        if (d && truth != 1) false_pos++;      /* claimed a win there is not */
        if (d) {
            found++;
            /* the move it names must be legal, and playing it must not lose */
            UtttGame u = g;
            if (!uttt_play(&u, mv)) false_pos++;
            else if (solve(&u) == 1) false_pos++;   /* +1 for THEM = we lost */
        }
        if (!d && truth == 1) missed++;
        if (truth == 1) wins++;

        /* AND THE DISTANCE, not just the verdict. */
        UtttGame e = g;
        int ex = exact_mate(&e);
        if (d && ex >= MATE_NONE) wrong_len++;
        else if (!d && ex < MATE_NONE) wrong_len++;
        else if (d && d != ex) wrong_len++;
        if (d && d < 16) hist[d]++;
    }
    printf("mate search vs an exhaustive solver: %d positions, %d are wins\n",
           checked, wins);
    printf("  proved %d of them, %d claimed falsely, %d real wins not proved\n",
           found, false_pos, missed);
    printf("  %d disagreed with the exhaustive distance\n", wrong_len);
    printf("  proved distances:");
    for (int i = 1; i < 16; i += 2) if (hist[i]) printf(" %d:%d", i, hist[i]);
    printf("\n");
    return (false_pos || wrong_len) ? 1 : 0;
}

#include "uttt.h"
#include <string.h>

/* THREE IN A LINE, AND IT IS 62% OF EVERY BOT'S RUNTIME. `sample` on a
 * ladder at 400 rollouts put 4,670 of 7,495 stacks here, ahead of
 * `score_move` at 1,489 and `uttt_legal` at 766 - which makes sense once you
 * see it: a playout calls `uttt_play`, `uttt_play` calls this three times,
 * and a tree search does hundreds of thousands of playouts.
 *
 * So it is a nine-bit mask and one lookup rather than eight triples of
 * compares with an unpredictable branch on each. Nothing about the answer
 * changes: a slot holding UTTT_DRAW, or the other mark, or nothing, is not
 * `mark`, so it contributes a zero exactly as it did before. Proved
 * exhaustively against the old version over all 4^9 boards and both marks -
 * see tests/uttt_line.c, which still holds the old one to compare against. */
static const uint16_t LINE_MASKS[8] = {
    0007, 0070, 0700, 0111, 0222, 0444, 0421, 0124
};

/* HAS_LINE[m] for a nine-bit occupancy mask m. Built once, because a 512
 * byte table is smaller than the code that would avoid it. */
static uint8_t HAS_LINE[512];
static int has_line_ready;

static void build_has_line(void)
{
    for (unsigned m = 0; m < 512; m++) {
        uint8_t hit = 0;
        for (int i = 0; i < 8; i++)
            if ((m & LINE_MASKS[i]) == LINE_MASKS[i]) { hit = 1; break; }
        HAS_LINE[m] = hit;
    }
    has_line_ready = 1;
}

int uttt_line(const uint8_t *nine, uint8_t mark)
{
    if (!has_line_ready) build_has_line();
    unsigned m = 0;
    for (int i = 0; i < 9; i++) m |= (unsigned)(nine[i] == mark) << i;
    return HAS_LINE[m];
}

void uttt_init(UtttGame *g)
{
    memset(g, 0, sizeof *g);
    g->forced = UTTT_ANY;
    g->turn   = UTTT_X;
}

int uttt_legal(const UtttGame *g, uint8_t *out)
{
    int n = 0;
    if (g->over) return 0;

    int lo = 0, hi = 9;
    if (g->forced != UTTT_ANY && g->block[g->forced] == UTTT_OPEN) {
        lo = g->forced; hi = g->forced + 1;
    }
    for (int b = lo; b < hi; b++) {
        if (g->block[b] != UTTT_OPEN) continue;
        for (int c = 0; c < 9; c++)
            if (g->cell[b * 9 + c] == UTTT_OPEN) out[n++] = (uint8_t)(b * 9 + c);
    }
    return n;
}

int uttt_undo(UtttGame *g)
{
    if (g->n_plies == 0) return 0;
    uint8_t moves[UTTT_MAX_PLIES];
    int n = g->n_plies - 1;
    memcpy(moves, g->move, (size_t)n);
    uttt_init(g);
    for (int i = 0; i < n; i++) uttt_play(g, moves[i]);
    return 1;
}

int uttt_play(UtttGame *g, uint8_t mv)
{
    uint8_t list[81];
    int n = uttt_legal(g, list), ok = 0;
    for (int i = 0; i < n; i++) if (list[i] == mv) { ok = 1; break; }
    if (!ok) return 0;

    int b = mv / 9, c = mv % 9;
    g->cell[mv] = g->turn;
    g->move[g->n_plies++] = mv;

    if (uttt_line(&g->cell[b * 9], g->turn)) {
        g->block[b] = g->turn;
    } else {
        int full = 1;
        for (int i = 0; i < 9; i++) if (g->cell[b * 9 + i] == UTTT_OPEN) full = 0;
        if (full) g->block[b] = UTTT_DRAW;
    }

    if (uttt_line(g->block, UTTT_X))      g->over = UTTT_X;
    else if (uttt_line(g->block, UTTT_O)) g->over = UTTT_O;
    else {
        int any_open = 0;
        for (int i = 0; i < 9; i++) if (g->block[i] == UTTT_OPEN) any_open = 1;
        if (!any_open) g->over = UTTT_DRAW;
    }

    g->forced = (uint8_t)c;
    g->turn   = (uint8_t)(g->turn == UTTT_X ? UTTT_O : UTTT_X);
    return 1;
}

#include "uttt.h"
#include <string.h>

static const uint8_t LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}
};

int uttt_line(const uint8_t *nine, uint8_t mark)
{
    for (int i = 0; i < 8; i++)
        if (nine[LINES[i][0]] == mark &&
            nine[LINES[i][1]] == mark &&
            nine[LINES[i][2]] == mark) return 1;
    return 0;
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

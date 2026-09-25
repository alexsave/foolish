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

/* WIN_CELLS[m]: the empty squares that would give `m` a line. Built from the
 * same eight masks, read the other way round - a line missing exactly one of
 * its three squares contributes that square. */
static uint16_t WIN_CELLS[512];
static int has_line_ready;

static void build_has_line(void)
{
    for (unsigned m = 0; m < 512; m++) {
        uint8_t hit = 0;
        for (int i = 0; i < 8; i++)
            if ((m & LINE_MASKS[i]) == LINE_MASKS[i]) { hit = 1; break; }
        HAS_LINE[m] = hit;
    }
    /* SECOND PASS, because this one reads the first. A square wins for `m`
     * if `m` plus that square has a line - which is the same question
     * `wins_block` asks, so the two cannot drift. Nine squares by five
     * hundred masks is four and a half thousand iterations, once. */
    for (unsigned m = 0; m < 512; m++) {
        unsigned wins = 0;
        for (int c = 0; c < 9; c++) {
            if (m & (1u << c)) continue;
            if (HAS_LINE[m | (1u << c)]) wins |= 1u << c;
        }
        WIN_CELLS[m] = (uint16_t)wins;
    }
    has_line_ready = 1;
}

unsigned uttt_line_mask(int i)
{
    return (i >= 0 && i < 8) ? LINE_MASKS[i] : 0u;
}

int uttt_won_line(const UtttGame *g)
{
    if (g->over != UTTT_X && g->over != UTTT_O) return -1;
    unsigned held = g->bm[g->over - 1];
    for (int i = 0; i < 8; i++)
        if ((held & LINE_MASKS[i]) == LINE_MASKS[i]) return i;
    return -1;
}

unsigned uttt_mask_wins(unsigned mask)
{
    if (!has_line_ready) build_has_line();
    return WIN_CELLS[mask & 0x1ffu];
}

int uttt_mask_line(unsigned mask)
{
    if (!has_line_ready) build_has_line();
    return HAS_LINE[mask & 0x1ffu];
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
    g->live   = 0x1ff;
}

unsigned uttt_legal_blocks(const UtttGame *g)
{
    if (g->over) return 0;
    if (g->forced != UTTT_ANY && ((g->live >> g->forced) & 1u))
        return 1u << g->forced;
    return g->live;
}

unsigned uttt_open_cells(const UtttGame *g, int b)
{
    return ~(unsigned)(g->cm[0][b] | g->cm[1][b]) & 0x1ffu;
}

int uttt_legal(const UtttGame *g, uint8_t *out)
{
    int n = 0;
    unsigned blocks = uttt_legal_blocks(g);
    while (blocks) {
        int b = __builtin_ctz(blocks);
        blocks &= blocks - 1;
        unsigned open = uttt_open_cells(g, b);
        while (open) {
            int c = __builtin_ctz(open);
            open &= open - 1;
            out[n++] = (uint8_t)(b * 9 + c);
        }
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
    /* LEGALITY IS THREE QUESTIONS, not a list of every answer.
     *
     * This used to generate all the legal moves into an eighty-one byte
     * buffer and scan it for `mv`, on every play - including inside a
     * playout, where the move had just come out of exactly that list. It was
     * 24% of a bot's runtime, second only to the heuristic.
     *
     * The three questions are the same three `uttt_legal` walks the board to
     * express: the block is open, the square is empty, and either you were
     * sent nowhere in particular or you were sent here. Same answer, no
     * list - and the check stays in `uttt_play`, so nothing anywhere gets an
     * unvalidated version of this to hold wrong. */
    if (mv > 80 || g->over) return 0;
    int b = mv / 9, c = mv % 9;
    if (!((g->live >> b) & 1u)) return 0;
    if (((g->cm[0][b] | g->cm[1][b]) >> c) & 1u) return 0;
    if (g->forced != UTTT_ANY && ((g->live >> g->forced) & 1u)
        && b != g->forced) return 0;

    const int me = g->turn - 1;
    g->cm[me][b] |= (uint16_t)(1u << c);
    g->move[g->n_plies++] = mv;

    if (uttt_mask_line(g->cm[me][b])) {
        g->bm[me] |= (uint16_t)(1u << b);
        g->live   &= (uint16_t)~(1u << b);
    } else if ((g->cm[0][b] | g->cm[1][b]) == 0x1ffu) {
        g->bdrawn |= (uint16_t)(1u << b);
        g->live   &= (uint16_t)~(1u << b);
    }

    if (uttt_mask_line(g->bm[0]))      g->over = UTTT_X;
    else if (uttt_mask_line(g->bm[1])) g->over = UTTT_O;
    else if (!g->live)                 g->over = UTTT_DRAW;

    g->forced = (uint8_t)c;
    g->turn   = (uint8_t)(g->turn == UTTT_X ? UTTT_O : UTTT_X);
    return 1;
}

int uttt_active(const UtttGame *g)
{
    if (g->over) return -1;
    if (g->forced != UTTT_ANY && uttt_block(g, g->forced) == UTTT_OPEN)
        return g->forced;
    return 9;
}

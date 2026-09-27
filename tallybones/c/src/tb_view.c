/* Tallybones - the view. See tb_view.h. */
#include "tb_view.h"
#include <string.h>

void tb_view(const TbGame *g, TbView *out)
{
    memset(out, 0, sizeof *out);
    out->n = g->n;
    out->turn = g->turn;
    out->roll = g->roll;
    out->over = g->over;
    out->draft = g->draft;
    out->pending_kind = g->draft ? g->pending.kind : 0;
    out->pending_arg = g->draft ? g->pending.arg : 0;
    out->kept = g->kept;
    memcpy(out->dice, g->dice, TB_DICE);
    int all = 1;
    for (int i = 0; i < TB_DICE; i++) {
        if (g->dice[i]) out->known |= (uint8_t)(1u << i);
        else all = 0;
    }
    out->rolls_left = (uint8_t)(g->over ? 0 : TB_ROLLS - g->roll);
    out->winners = (uint8_t)tb_winners(g);
    out->turns = g->turns;
    out->bubbles = (uint16_t)(g->hist_n + (g->draft ? 1 : 0));
    if (!g->over && all && g->turn < g->n)
        for (int c = 0; c < TB_CATS; c++)
            if (!(g->filled[g->turn] >> c & 1)) out->would[c] = (uint8_t)tb_score_of(g->dice, c);
    for (int s = 0; s < g->n; s++) {
        TbCard *k = &out->seat[s];
        k->filled = g->filled[s];
        k->total = (uint16_t)tb_total(g, s);
        k->upper = (uint16_t)tb_upper(g, s);
        k->bonus = (uint8_t)tb_bonus(g, s);
        k->bonus_known = (uint8_t)tb_bonus_known(g, s);
        k->still_in = (uint8_t)tb_is_in(g, s);
        k->winner = (uint8_t)(out->winners >> s & 1);
        memcpy(k->score, g->score[s], TB_CATS);
    }
}

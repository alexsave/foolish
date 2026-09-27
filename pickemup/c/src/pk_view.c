/* Pick 'Em Up - the masked per-seat view. See pk_view.h. */
#include "pk_view.h"
#include <string.h>

void pk_view(const PkGame *g, int viewer, PkView *v)
{
    memset(v, 0, sizeof *v);
    int seat = viewer >= 0 && viewer < g->n ? viewer : -1;

    v->n = g->n;
    v->turn = g->turn;
    v->dir = g->dir > 0 ? PK_DIR_CW : PK_DIR_ACW;
    v->show_dir = g->n > 2;
    v->live_suit = g->live_suit;
    v->over = g->over;
    v->winner = g->winner;
    v->top = g->stack_n ? g->stack[g->stack_n - 1] : PK_CARD_NONE;
    v->deck_n = g->deck_n;
    v->stack_n = g->stack_n;
    v->said = g->said;
    v->me = seat < 0 ? PK_SEAT_NONE : (uint8_t)seat;
    v->draft_call = PK_SEAT_NONE;

    if (seat >= 0) {
        PkAct say = { PK_A_SAY_IT, 0, 0, 0 };
        PkAct draw = { PK_A_DRAW, 0, 0, 0 };
        PkAct pass = { PK_A_PASS, 0, 0, 0 };
        v->my_exposed = (uint8_t)pk_is_legal(g, seat, say);
        v->can_draw = (uint8_t)pk_is_legal(g, seat, draw);
        v->can_pass = (uint8_t)pk_is_legal(g, seat, pass);
        int mine = g->b_open && g->b_sender == seat;
        v->can_seal = (uint8_t)(mine && pk_can_seal(g));
        v->can_undo = (uint8_t)(mine && g->hist_n > g->b_floor);
        v->draft_open = (uint8_t)mine;
        v->draft_said = (uint8_t)(mine && g->b_said);
        if (mine) v->draft_call = g->b_call;
        for (int t = 0; t < g->n; t++) {
            PkAct c = { PK_A_CALL_OUT, (uint8_t)t, 0, 0 };
            if (pk_is_legal(g, seat, c)) v->can_call |= (uint8_t)(1u << t);
        }
        v->my_n = g->hand_n[seat];
        memcpy(v->my_hand, g->hand[seat], g->hand_n[seat]);
        for (int p = 0; p < g->hand_n[seat]; p++)
            v->my_playable[p] = (uint8_t)pk_can_play(g, seat, p);
    }

    if (g->over || viewer == PK_VIEW_ALL)
        for (int s = 0; s < g->n; s++) {
            v->reveal_n[s] = g->hand_n[s];
            memcpy(v->reveal_hand[s], g->hand[s], g->hand_n[s]);
        }
}

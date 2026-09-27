/* Chui Niu - the masked per-seat view. See cn_view.h. */
#include "cn_view.h"
#include <string.h>

static void sorted(uint8_t *out, const uint8_t *in, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t d = in[i];
        int j = i;
        while (j > 0 && out[j - 1] > d) { out[j] = out[j - 1]; j--; }
        out[j] = d;
    }
}

void cn_view(const CnGame *g, int viewer, CnView *v)
{
    memset(v, 0, sizeof *v);
    const int seat = viewer >= 0 && viewer < g->n ? viewer : -1;
    v->viewer = (int8_t)(seat >= 0 ? seat : viewer == CN_VIEW_ALL ? CN_VIEW_ALL : CN_VIEW_SPECTATOR);
    v->n = g->n;
    v->phase = g->phase;
    v->round = g->round;
    v->turn = g->turn;
    v->bid_q = g->bid_q;
    v->bid_f = g->bid_f;
    v->bidder = g->bidder;
    v->total = g->total;
    v->winner = g->winner;
    memcpy(v->dice_n, g->dice_n, sizeof v->dice_n);

    const int live = g->phase != CN_PH_OVER;
    if (seat >= 0 && live) {
        v->my_n = g->dice_n[seat];
        sorted(v->my_dice, g->dice[seat], g->dice_n[seat]);
    }
    if (viewer == CN_VIEW_ALL && live)
        for (int s = 0; s < g->n; s++) sorted(v->all + s * CN_START_DICE, g->dice[s], g->dice_n[s]);

    if (seat >= 0 && live && g->turn == seat) {
        int q, f;
        v->my_turn = 1;
        v->can_call = (uint8_t)cn_can_call(g);
        if (cn_min_raise(g, &q, &f)) {
            v->can_raise = 1;
            v->min_q = (uint8_t)q;
            v->min_f = (uint8_t)f;
            v->max_q = g->total;
        }
    }

    v->call_seat = g->call_seat;
    if (g->call_seat != CN_SEAT_NONE) {
        v->revealed = g->phase == CN_PH_REVEALED || g->phase == CN_PH_OVER;
        v->call_bidder = g->call_bidder;
        v->call_q = g->call_q;
        v->call_f = g->call_f;
        v->call_count = g->call_count;
        v->call_loser = g->call_loser;
        v->call_true = g->call_loser == g->call_seat;
        memcpy(v->shown_n, g->shown_n, sizeof v->shown_n);
        for (int s = 0; s < g->n; s++) sorted(v->shown + s * CN_START_DICE, g->shown[s], g->shown_n[s]);
    } else {
        v->call_bidder = v->call_loser = CN_SEAT_NONE;
    }
}

/* Tallybones - the layout numbers, so Swift derives none (tb_api.h "the
 * layout"). Pure functions: no resident, no game. */
#include "include/tb_api.h"
#include "../src/tb.h"

float tb_lay_collapse(float view_h)
{
    if (view_h >= 440.0f) return 0.0f;
    if (view_h <= 340.0f) return 1.0f;
    return (440.0f - view_h) / 100.0f;
}

/* The die's side for a board: the largest of 40..64 that lets five dice and
 * four gaps fit the width. */
static float side_for(float board_w)
{
    float s = (board_w - 4.0f * TB_LAY_DIE_GAP) / 5.0f;
    if (s > TB_LAY_DIE_MAX) s = TB_LAY_DIE_MAX;
    if (s < TB_LAY_DIE_MIN) s = TB_LAY_DIE_MIN;
    return s;
}

void tb_lay_tray(float board_w, float board_h, float collapse, float *x, float *y, float *w, float *h)
{
    float s = side_for(board_w);
    float tw = 5.0f * s + 4.0f * TB_LAY_DIE_GAP, th = s + TB_LAY_KEEP_LIFT;
    /* expanded: a quarter of the way down; the drawer: centred */
    float top = board_h * (0.25f + 0.25f * collapse) - th / 2.0f;
    if (top < 0.0f) top = 0.0f;
    if (x) *x = (board_w - tw) / 2.0f;
    if (y) *y = top;
    if (w) *w = tw;
    if (h) *h = th;
}

int tb_lay_die(int i, float board_w, float board_h, float collapse, int kept, float *x, float *y, float *side)
{
    if (i < 0 || i >= TB_DICE) return -1;
    float tx, ty;
    tb_lay_tray(board_w, board_h, collapse, &tx, &ty, 0, 0);
    float s = side_for(board_w);
    if (x) *x = tx + (float)i * (s + TB_LAY_DIE_GAP);
    if (y) *y = ty + (kept ? TB_LAY_KEEP_LIFT : 0.0f);
    if (side) *side = s;
    return 0;
}

int tb_lay_pip(int face, int k, float *fx, float *fy)
{
    static const signed char at[7][6][2] = {
        { { 0 } },
        { { 1, 1 } },
        { { 0, 0 }, { 2, 2 } },
        { { 0, 0 }, { 1, 1 }, { 2, 2 } },
        { { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 } },
        { { 0, 0 }, { 2, 0 }, { 1, 1 }, { 0, 2 }, { 2, 2 } },
        { { 0, 0 }, { 2, 0 }, { 0, 1 }, { 2, 1 }, { 0, 2 }, { 2, 2 } },
    };
    if (face < 1 || face > 6) return -1;
    if (k >= 0 && k < face) {
        if (fx) *fx = 0.25f + 0.25f * (float)at[face][k][0];
        if (fy) *fy = 0.25f + 0.25f * (float)at[face][k][1];
    }
    return face;
}

int tb_lay_row_cat(int row)
{
    if (row >= TB_ROW_ONES && row <= TB_ROW_SIXES) return row;
    if (row >= TB_ROW_THREE_ALIKE && row <= TB_ROW_ANY) return row - 2;
    return -1;
}

int tb_lay_row_y(int row, float *y)
{
    if (row < 0 || row >= TB_ROW_N) return -1;
    float at = (float)row * TB_LAY_ROW_H;
    if (row >= TB_ROW_THREE_ALIKE) at += TB_LAY_GAP_H;
    if (row == TB_ROW_TOTAL) at += TB_LAY_GAP_H;
    if (y) *y = at;
    return 0;
}

float tb_lay_card_h(void)
{
    float y = 0.0f;
    tb_lay_row_y(TB_ROW_TOTAL, &y);
    return y + TB_LAY_ROW_H;
}

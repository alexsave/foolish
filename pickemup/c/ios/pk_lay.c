/* Pick 'Em Up - the layout numbers of pickemup/docs/UI.html. See the layout
 * section of include/pk_api.h.
 *
 * WHY C. Every number here is foolish's (FHandFan, MessageTableView+Seats,
 * FDeckWell, FSeatBadge, FActionBar) or the study's (O4, U2, U3, U6, U7, U9),
 * and REUSE_AUDIT.md 3.2 recommends moving the pure geometry out of Swift.
 * Written once here, the host draws what it is told and a test on any machine
 * pins the thresholds (pk_api_smoke.c, "layout").
 *
 * Pure arithmetic: no resident, no game, no allocation. sinf / cosf are the
 * only libm this needs, and only the iOS bridge links this file. */
#include "include/pk_api.h"
#include <math.h>
#include <stddef.h>

/* ---- foolish's hand row (FHandFan.swift) ------------------------------------ */
#define MAX_W      52.0f   /* never wider than a card's proper aspect          */
#define MIN_W      22.0f   /* the flat floor                                   */
#define GAP         4.0f
#define TWO_ROW    34.0f   /* one flat row narrower than this goes to two      */
#define ROW_GAP     6.0f
#define STRIP_MIN  16.0f   /* O4: overlap down to a 16pt visible strip, then scroll */

/* foolish's collapse anchors (MessageTableView.collapseFraction) */
#define COMPACT_H 340.0f
#define EXPANDED_H 440.0f

float pk_lay_collapse(float view_h)
{
    if (view_h <= COMPACT_H) return 1.0f;
    if (view_h >= EXPANDED_H) return 0.0f;
    return (EXPANDED_H - view_h) / (EXPANDED_H - COMPACT_H);
}

int pk_lay_max_rows(float view_h)
{
    return pk_lay_collapse(view_h) >= 0.5f ? 1 : 2;     /* U7 */
}

/* Flat card width for k cards in a row of `width`, before the clamp. */
static float flat_w(int k, float width)
{
    return (width - GAP * (float)(k + 1)) / (float)k;
}

typedef struct {
    int   mode, rows, top_n, per_row;
    float card_w, step, content_w, box_h;
} Hand;

static Hand hand(int n, float width, int max_rows)
{
    Hand h = { PK_LAY_FLAT, 1, 0, n, MAX_W, MAX_W + GAP, 0, PK_LAY_ROW_H };
    if (n < 1 || width <= 0) { h.per_row = 0; return h; }
    if (max_rows > 2) max_rows = 2;
    if (max_rows < 1) max_rows = 1;
    float w1 = flat_w(n, width);
    if (n == 1 || (w1 >= MIN_W && (max_rows == 1 || w1 >= TWO_ROW))) {
        h.card_w = w1 > MAX_W ? MAX_W : w1;
    } else {
        if (max_rows == 2) {
            h.rows = 2;
            h.top_n = n / 2;                 /* the smaller half goes on top */
            h.per_row = n - h.top_n;
            h.box_h = PK_LAY_ROW_H * 2 + ROW_GAP;
        }
        float w = flat_w(h.per_row, width);
        if (h.rows == 2 && w >= MIN_W) {
            h.card_w = w > MAX_W ? MAX_W : w;
        } else {
            /* O4 / U8: past the floor the cards keep a 40pt face and overlap */
            h.card_w = PK_LAY_FACE_W;
            h.step = (width - 2 * GAP - PK_LAY_FACE_W) / (float)(h.per_row - 1);
            h.mode = PK_LAY_OVERLAP;
            if (h.step < STRIP_MIN) { h.step = STRIP_MIN; h.mode = PK_LAY_SCROLL; }
        }
    }
    if (h.mode == PK_LAY_FLAT) h.step = h.card_w + GAP;
    if (h.rows == 1) h.top_n = 0;
    float row = h.card_w + h.step * (float)(h.per_row - 1);
    h.content_w = h.mode == PK_LAY_SCROLL ? row + 2 * GAP : width;
    return h;
}

int pk_lay_hand(int n, float width, int max_rows, float *card_w, float *step, int *rows,
                int *top_n, float *content_w, float *box_h)
{
    Hand h = hand(n, width, max_rows);
    if (card_w) *card_w = h.card_w;
    if (step) *step = h.step;
    if (rows) *rows = h.rows;
    if (top_n) *top_n = h.top_n;
    if (content_w) *content_w = h.content_w;
    if (box_h) *box_h = h.box_h;
    return h.mode;
}

int pk_lay_hand_slot(int n, float width, int max_rows, int i, float *x, float *y)
{
    if (i < 0 || i >= n) return -1;
    Hand h = hand(n, width, max_rows);
    int r = h.rows == 2 && i >= h.top_n ? 1 : 0;
    int k = h.rows == 2 ? (r ? h.per_row : h.top_n) : n;   /* cards in this row  */
    int c = r ? i - h.top_n : i;
    float row = h.card_w + h.step * (float)(k - 1);
    float left = h.mode == PK_LAY_SCROLL ? GAP : (width - row) / 2;
    float stack = (float)h.rows * PK_LAY_CARD_H + (float)(h.rows - 1) * ROW_GAP;
    float top = (h.box_h - stack) / 2;
    if (x) *x = left + h.step * (float)c;
    if (y) *y = top + (float)r * (PK_LAY_CARD_H + ROW_GAP);
    return 0;
}

/* ---- foolish's ring (MessageTableView+Seats.swift ringPoint) ---------------- */

void pk_lay_seat(int seat, int me, int n, float board_w, float board_h, float collapse,
                 float *x, float *y)
{
    if (n < 1) n = 1;
    int visual = me < 0 ? seat % n : ((seat - me) % n + n) % n;
    float rad = 2.0f * 3.14159265f * (float)visual / (float)n;
    float ry = 0.35f + 0.03f * collapse;
    if (x) *x = (-sinf(rad) * 0.42f + 0.5f) * board_w;
    if (y) *y = (cosf(rad) * ry + 0.5f) * board_h;
}

/* ---- the fan (FSeatBadge's 28 x 40 backs, U6's 96pt cap) --------------------- */
#define FAN_STEP 10.0f
#define FAN_MIN   3.0f
#define FAN_MAX  96.0f

float pk_lay_fan_step(int backs)
{
    if (backs < 2) return FAN_STEP;
    float s = (FAN_MAX - PK_LAY_FAN_CARD_W) / (float)(backs - 1);
    return s > FAN_STEP ? FAN_STEP : s < FAN_MIN ? FAN_MIN : s;
}

/* ---- the deck (FDeckWell.layers) -------------------------------------------- */

int pk_lay_deck_layers(int deck_n)
{
    if (deck_n <= 0) return 0;
    if (deck_n <= 6) return deck_n;
    return deck_n <= 11 ? 7 : 8;
}

/* ---- the pile and the deck beside it (U2, U3) -------------------------------- */
#define PILE_LIFT 24.0f      /* exactly a pill row's clearance in the drawer     */
#define DECK_GAP  10.0f      /* the deck sits 10pt left of the pile              */

void pk_lay_pile(float board_w, float board_h, float collapse, float *cx, float *cy)
{
    if (cx) *cx = board_w / 2;
    if (cy) *cy = board_h / 2 - (collapse >= 0.5f ? PILE_LIFT : 0);
}

void pk_lay_deck(float board_w, float board_h, float collapse, float *x, float *y)
{
    float cx, cy;
    pk_lay_pile(board_w, board_h, collapse, &cx, &cy);
    if (x) *x = cx - PK_LAY_PILE_W / 2 - DECK_GAP - PK_LAY_DECK_W;
    if (y) *y = cy - PK_LAY_DECK_H / 2;
}

/* ---- the pill row (U9) ------------------------------------------------------ */

void pk_lay_pills(int can_draw, int my_turn, int selected, int can_pass, int can_undo,
                  int *trailing, int *leading)
{
    int want[3], n = 0;
    if (selected && my_turn) want[n++] = PK_PILL_PLAY;
    if (can_pass) want[n++] = PK_PILL_PASS;
    if (can_undo) want[n++] = PK_PILL_UNDO;
    int t = PK_PILL_NONE, l = PK_PILL_NONE;
    if (can_draw) {
        t = PK_PILL_DRAW;
        if (n) l = want[0];
    } else {
        if (n > 0) t = want[0];
        if (n > 1) l = want[1];
    }
    if (trailing) *trailing = t;
    if (leading) *leading = l;
}

/* ---- the suit picker (U14) ----------------------------------------------------- */
#define PICKER_REACH_X 96.0f     /* east and west of the pile's centre           */
#define PICKER_REACH_Y 104.0f    /* north and south                              */

void pk_lay_picker(int tile, float cx, float cy, float *x, float *y)
{
    static const float dx[5] = { 0, 1, 0, -1, 1 }, dy[5] = { -1, 0, 1, 0, -1 };
    if (tile < 0 || tile > 4) tile = 4;
    if (x) *x = cx + dx[tile] * PICKER_REACH_X;
    if (y) *y = cy + dy[tile] * PICKER_REACH_Y;
}

/* ---- the board's zones (I31) -------------------------------------------------- */
#define BAND_UP     64.0f    /* U24: foolish's hand band, grown 64 up ...         */
#define BAND_DOWN   24.0f    /* ... and 24 down                                   */
#define DROP_MARGIN  8.0f    /* a dragged card over the pile's edge still plays   */
#define PILL_GAP     4.0f    /* the pill row sits 4 above the hand                */
#define TOAST_UP    64.0f    /* the toast's centre above the hand                 */
#define DIR_W       78.0f    /* UI.html's direction box                           */
#define DIR_H       68.0f
#define DIR_Y       -3.0f

int pk_lay_zone(int zone, float board_w, float board_h, float collapse, float hand_box_h,
                float *x, float *y, float *w, float *h)
{
    float hand_top = board_h - hand_box_h, rx = 0, ry = 0, rw = 0, rh = 0;
    switch (zone) {
    case PK_ZONE_DRAW_BAND:
        rx = PK_LAY_HAND_PAD;
        ry = hand_top - BAND_UP;
        rw = board_w - 2 * PK_LAY_HAND_PAD;
        rh = hand_box_h + BAND_UP + BAND_DOWN;
        break;
    case PK_ZONE_PILE_DROP: {
        float cx, cy;
        pk_lay_pile(board_w, board_h, collapse, &cx, &cy);
        rx = cx - PK_LAY_PILE_W / 2 - DROP_MARGIN;
        ry = cy - PK_LAY_PILE_H / 2 - DROP_MARGIN;
        rw = PK_LAY_PILE_W + 2 * DROP_MARGIN;
        rh = PK_LAY_PILE_H + 2 * DROP_MARGIN;
        break;
    }
    case PK_ZONE_PILLS:
        ry = hand_top - PILL_GAP - PK_LAY_PILL_H;
        rw = board_w;
        rh = PK_LAY_PILL_H;
        break;
    case PK_ZONE_TOAST:
        rx = board_w / 2;
        ry = hand_top - TOAST_UP;
        break;
    case PK_ZONE_DIR:
        rx = board_w - DIR_W;
        ry = DIR_Y;
        rw = DIR_W;
        rh = DIR_H;
        break;
    default:
        return -1;
    }
    if (x) *x = rx;
    if (y) *y = ry;
    if (w) *w = rw < 0 ? 0 : rw;
    if (h) *h = rh < 0 ? 0 : rh;
    return 0;
}

/* ---- a dragged hand card (O9, IOS_DECISIONS I38) ----------------------------- */

int pk_lay_hand_nearest(int n, float width, int max_rows, float cx, float cy)
{
    float cw = 0;
    pk_lay_hand(n, width, max_rows, &cw, NULL, NULL, NULL, NULL, NULL);
    int best = -1;
    float best_d = 0;
    for (int i = 0; i < n; i++) {
        float x, y;
        pk_lay_hand_slot(n, width, max_rows, i, &x, &y);
        float dx = cx - (x + cw / 2), dy = cy - (y + PK_LAY_CARD_H / 2);
        float d = dx * dx + dy * dy;
        if (best < 0 || d < best_d) { best = i; best_d = d; }   /* strict: a tie keeps the lower */
    }
    return best;
}

/* A rect's inside as CGRect.contains has it: the far edges are outside. */
static int inside(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int pk_lay_drop(float board_w, float board_h, float collapse, float hand_box_h, float x, float y)
{
    /* the hand row first: a release still on the cards is a rearrange */
    if (inside(x, y, PK_LAY_HAND_PAD, board_h - hand_box_h, board_w - 2 * PK_LAY_HAND_PAD, hand_box_h))
        return PK_DROP_HAND;
    float rx, ry, rw, rh;
    pk_lay_zone(PK_ZONE_PILE_DROP, board_w, board_h, collapse, hand_box_h, &rx, &ry, &rw, &rh);
    return inside(x, y, rx, ry, rw, rh) ? PK_DROP_PILE : PK_DROP_NONE;
}

/* ---- the auto-collapse's push (A14): uttt_collapse_push, on this kernel's numbers ---- */

float pk_lay_collapse_push(float travel, int t_ms)
{
    if (t_ms <= 0) return travel;
    if (t_ms >= PK_LAY_COLLAPSE_MS) return 0.0f;
    const double w = 2.0 * 3.14159265358979 / PK_LAY_DRAWER_RESPONSE_MS, t = (double)t_ms;
    const double left = (1.0 + w * t) * exp(-w * t);            /* 1 - the host's progress */
    /* THE LAST KEYFRAME IS EXACTLY ZERO and is reached without a step: what
     * the spring still has left at the end (under 0.3%) is faded out linearly
     * over the slide, so removing the animation moves nothing. */
    const double tail = (1.0 + w * PK_LAY_COLLAPSE_MS) * exp(-w * PK_LAY_COLLAPSE_MS);
    return (float)(travel * (left - tail * t / PK_LAY_COLLAPSE_MS));
}

#include "uttt_api.h"
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_draw.h"
#include "../src/uttt_msg.h"
#include "../src/uttt_say.h"
#include <string.h>

/* The host's names for the kernel's numbers. Two lists, so the compiler
 * holds them together: a key renumbered on one side fails the build. */
_Static_assert(UTI_SEAT_SPECTATOR == UTM_SEAT_SPECTATOR, "seat SPECTATOR");
_Static_assert(UTI_SEAT_X == UTM_SEAT_X, "seat X");
_Static_assert(UTI_SEAT_O == UTM_SEAT_O, "seat O");
_Static_assert(UTI_SEAT_WAITING == UTM_SEAT_WAITING, "seat WAITING");
_Static_assert(UTI_SEAT_OPEN == UTM_SEAT_OPEN, "seat OPEN");
_Static_assert(UTI_SEAT_CLOSED == UTM_SEAT_CLOSED, "seat CLOSED");
_Static_assert(UTI_DOOR_NONE == UTM_DOOR_NONE, "door NONE");
_Static_assert(UTI_DOOR_TAKE_BACK == UTM_DOOR_TAKE_BACK, "door TAKE_BACK");
_Static_assert(UTI_DOOR_AGAIN == UTM_DOOR_AGAIN, "door AGAIN");
_Static_assert(UTI_SAY_BUBBLE_HEADLINE == UTTT_SAY_BUBBLE_HEADLINE, "say BUBBLE_HEADLINE");
_Static_assert(UTI_SAY_BUBBLE_PLACE == UTTT_SAY_BUBBLE_PLACE, "say BUBBLE_PLACE");
_Static_assert(UTI_SAY_CAPTION == UTTT_SAY_CAPTION, "say CAPTION");
_Static_assert(UTI_SAY_HEADLINE_PRE == UTTT_SAY_HEADLINE_PRE, "say HEADLINE_PRE");
_Static_assert(UTI_SAY_HEADLINE_POST == UTTT_SAY_HEADLINE_POST, "say HEADLINE_POST");
_Static_assert(UTI_SAY_SUBLINE == UTTT_SAY_SUBLINE, "say SUBLINE");
_Static_assert(UTI_SAY_WATCH_LABEL == UTTT_SAY_WATCH_LABEL, "say WATCH_LABEL");
_Static_assert(UTI_SAY_WATCH_LINE == UTTT_SAY_WATCH_LINE, "say WATCH_LINE");
_Static_assert(UTI_SAY_WAITING_HEADLINE == UTTT_SAY_WAITING_HEADLINE, "say WAITING_HEADLINE");
_Static_assert(UTI_SAY_WAITING_SUBLINE == UTTT_SAY_WAITING_SUBLINE, "say WAITING_SUBLINE");
_Static_assert(UTI_SAY_UNREADABLE_HEADLINE == UTTT_SAY_UNREADABLE_HEADLINE, "say UNREADABLE_HEADLINE");
_Static_assert(UTI_SAY_UNREADABLE_SUBLINE == UTTT_SAY_UNREADABLE_SUBLINE, "say UNREADABLE_SUBLINE");
_Static_assert(UTI_SAY_YOU_ARE_1 == UTTT_SAY_YOU_ARE_1, "say YOU_ARE_1");
_Static_assert(UTI_SAY_YOU_ARE_2 == UTTT_SAY_YOU_ARE_2, "say YOU_ARE_2");
_Static_assert(UTI_SAY_CLOSED_HEADLINE == UTTT_SAY_CLOSED_HEADLINE, "say CLOSED_HEADLINE");
_Static_assert(UTI_SAY_CLOSED_SUBLINE == UTTT_SAY_CLOSED_SUBLINE, "say CLOSED_SUBLINE");
_Static_assert(UTI_SAY_DOOR_TAKE_BACK == UTTT_SAY_DOOR_TAKE_BACK, "say DOOR_TAKE_BACK");
_Static_assert(UTI_SAY_DOOR_AGAIN == UTTT_SAY_DOOR_AGAIN, "say DOOR_AGAIN");
_Static_assert(UTI_SAY_DOOR_AGAIN + 1 == UTTT_SAY_COUNT, "every key has a host name");
_Static_assert(UTI_MSG_TEXT_MAX >= UTM_MAX_TEXT, "the longest link fits the host buffer");

/* The resident game, and the buffers the display list is built into. Sized
 * for a full board with every mark drawn, MEASURED rather than guessed:
 * `./build/uttt_render 81` prints the worst case, which today is 25,624
 * polygons from 166,556 points. Measure it again after touching the pen -
 * raising the flattening from 6 segments a curve to the document's 14 grew
 * this by three quarters, and a display list that runs out does not fail, it
 * quietly stops drawing. Everything below is the measurement plus half. */
#define MAX_PT   260000
#define MAX_POLY  40000

static struct {
    /* THE RESIDENT MESSAGE: the roster and the game together, because a move
     * on this board is a move by somebody in a seat and the kernel is the one
     * that knows which. `m.game` is the board everything draws. */
    UtmMsg    m;
    uint8_t   me[UTM_MAX_ID];       /* this device's identity bytes */
    int       me_n;
    char      said[160];
    int       overflow;
    UtttDL    dl;
    UtttPt    pt[MAX_PT];
    UtttPoly  poly[MAX_POLY];
    int32_t   first[MAX_POLY];
    int32_t   n[MAX_POLY];
    uint32_t  rgba[MAX_POLY];
} S;

void uti_new(int32_t seed)
{
    memset(&S.m, 0, sizeof S.m);
    S.m.seed = seed ? seed : 1;
    uttt_init(&S.m.game);
}

int uti_play(int mv)
{
    if (mv < 0 || mv > 80) return 0;
    return uttt_play(&S.m.game, (uint8_t)mv);
}
int uti_legal(uint8_t *out)   { return uttt_legal(&S.m.game, out); }
int uti_undo(void) { return uttt_undo(&S.m.game); }
int uti_over(void)            { return S.m.game.over; }
int uti_turn(void)            { return S.m.game.turn; }
int uti_forced(void)          { return S.m.game.forced; }
int uti_n_plies(void)         { return S.m.game.n_plies; }
int uti_move_at(int i)        { return (i >= 0 && i < S.m.game.n_plies) ? S.m.game.move[i] : -1; }
int uti_block(int b)          { return (b >= 0 && b < 9) ? uttt_block(&S.m.game, b) : -1; }
int uti_cell(int i)           { return (i >= 0 && i < 81) ? uttt_cell(&S.m.game, i) : -1; }

int uti_active(void) { return uttt_active(&S.m.game); }

int uti_encode(uint8_t *out, int cap) { return uttt_encode(&S.m.game, out, (size_t)cap); }

int uti_decode(const uint8_t *buf, int n, int32_t seed)
{
    UtttGame tmp;
    if (!uttt_decode(&tmp, buf, (size_t)n)) return 0;
    memset(&S.m, 0, sizeof S.m);
    S.m.game = tmp;
    S.m.seed = seed ? seed : 1;
    return 1;
}


/* EVERY DRAW STARTS FROM A FULL INIT, not a reset: a display list that was
 * never pointed at its buffers has a capacity of zero and draws nothing,
 * silently, and "was uti_new called first" is not a question a drawing entry
 * point should depend on. Six stores. */
static void dl_fresh(void)
{
    uttt_dl_init(&S.dl, S.pt, MAX_PT, S.poly, MAX_POLY);
}

static int publish(void)
{
    for (int i = 0; i < S.dl.n_poly; i++) {
        S.first[i] = S.dl.poly[i].first;
        S.n[i]     = S.dl.poly[i].n;
        S.rgba[i]  = S.dl.poly[i].rgba;
    }
    return S.dl.n_poly;
}

int uti_draw(int active, int last, float mark_t, float meta_t)
{
    dl_fresh();
    UtttDrawOpts o = uttt_draw_opts(S.m.seed);
    o.active = active; o.last = last;
    o.mark_t = mark_t; o.meta_t = meta_t;
    S.overflow = uttt_draw_board(&S.dl, &S.m.game, &o) != 0;
    return publish();
}

/* A display list that runs out of room does not fail - it stops appending,
 * and the board comes back with a few marks missing. Nothing on screen says
 * so, which is why it gets its own question. */
int uti_draw_overflow(void) { return S.overflow; }

int uti_draw_one(int mv, float t)
{
    if (mv < 0 || mv > 80) return 0;
    dl_fresh();
    uint8_t at = uttt_cell(&S.m.game, mv);
    uttt_draw_cell(&S.dl, at ? at : S.m.game.turn,
                   mv, S.m.seed, t);
    return publish();
}

int uti_draw_mark(int mark, int32_t seed)
{
    dl_fresh();
    uttt_draw_mark(&S.dl, mark, seed ? seed : 1, 0.f);
    return publish();
}

int uti_draw_rulebook(float w, float h)
{
    dl_fresh();
    uttt_draw_rulebook(&S.dl, w, h);
    return publish();
}

void uti_paper(uint8_t *rgba, int w, int h) { uttt_paper(rgba, w, h); }

/* ----------------------------------------------------------- the bubble */
void uti_bubble_size(float *w, float *h)
{
    UtttBubble b = uttt_bubble();
    if (w) *w = b.w;
    if (h) *h = b.h;
}

void uti_bubble_board(float *x, float *y, float *side)
{
    UtttBubble b = uttt_bubble();
    if (x)    *x    = b.board.x;
    if (y)    *y    = b.board.y;
    if (side) *side = b.board.w;
}

void uti_bubble_text(float *x, float *y, float *w, float *h)
{
    UtttBubble b = uttt_bubble();
    if (x) *x = b.text.x;
    if (y) *y = b.text.y;
    if (w) *w = b.text.w;
    if (h) *h = b.text.h;
}

float uti_bubble_type(int line)
{
    UtttBubble b = uttt_bubble();
    return line ? b.place_pt : b.headline_pt;
}

uint32_t uti_bubble_ink(int line)
{
    UtttBubble b = uttt_bubble();
    return line ? b.place_rgba : b.headline_rgba;
}

float uti_bubble_lead(void) { return uttt_bubble().lead; }

int         uti_rules_count(void)      { return uttt_rules_count(); }
const char *uti_rules_line(int i)      { return uttt_rules_line(i); }
const char *uti_rules_title(void)      { return uttt_rules_title(); }

const char *uti_place_name(int block, int spoken)
{
    return uttt_place_name(block, spoken);
}

const float    *uti_points(void)     { return (const float *)S.dl.pt; }
int             uti_point_count(void){ return S.dl.n_pt; }
const int32_t  *uti_poly_first(void) { return S.first; }
const int32_t  *uti_poly_n(void)     { return S.n; }
const uint32_t *uti_poly_rgba(void)  { return S.rgba; }

/* ---------------------------------------------------------- the message */
static void my_tag(uint8_t out[UTM_TAG_LEN])
{
    utm_tag(S.m.seed, S.me, S.me_n, out);
}

void uti_me(const uint8_t *id, int n)
{
    if (!id || n < 0) n = 0;
    if (n > UTM_MAX_ID) n = UTM_MAX_ID;
    if (n) memcpy(S.me, id, (size_t)n);
    S.me_n = n;
}

int uti_msg_open(int64_t unix_seconds)
{
    uint8_t me[UTM_TAG_LEN];
    int32_t seed = utm_seed_at(unix_seconds);
    utm_tag(seed, S.me, S.me_n, me);
    utm_open(&S.m, seed, me);
    return 1;
}

int uti_msg_read(const char *text)
{
    UtmMsg m;
    int r = utm_text_decode(text, &m);
    if (r != UTM_EOK) return r;
    S.m = m;
    return UTM_EOK;
}

int uti_msg_check(const char *text)
{
    UtmMsg m;
    return utm_text_decode(text, &m);
}

int uti_msg_text(char *out, int cap) { return utm_text_encode(&S.m, out, cap); }

int uti_msg_seat(void)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_seat(&S.m, me);
}

int uti_msg_mark(void)     { return utm_seat_mark(uti_msg_seat()); }
int uti_msg_sealed(void)   { return S.m.sealed; }
int32_t uti_msg_seed(void) { return S.m.seed; }

int uti_msg_can_move(void)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_can_move(&S.m, me);
}

int uti_msg_play(int mv)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_play(&S.m, me, mv);
}

int uti_msg_undo(void)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_undo(&S.m, me);
}

int uti_msg_take_back(void)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_take_back(&S.m, me);
}

int uti_msg_door(int sent)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_door(&S.m, me, sent != 0);
}

int uti_msg_prefer(const char *mine, const char *tapped)
{
    UtmMsg a, b;
    int ra = utm_text_decode(mine, &a), rb = utm_text_decode(tapped, &b);
    if (ra != UTM_EOK && rb != UTM_EOK) return 0;
    if (ra != UTM_EOK) return 1;
    if (rb != UTM_EOK) return -1;
    return utm_prefer(&a, &b);
}

int uti_msg_same_game(const char *a, const char *b)
{
    UtmMsg x, y;
    if (utm_text_decode(a, &x) != UTM_EOK || utm_text_decode(b, &y) != UTM_EOK)
        return 0;
    return utm_same_game(&x, &y);
}

int uti_msg_seat_ids(const uint8_t *o_id, int o_n, const uint8_t *x_id, int x_n)
{
    uint8_t o[UTM_TAG_LEN], x[UTM_TAG_LEN];
    utm_tag(S.m.seed, o_id, o_n, o);
    utm_tag(S.m.seed, x_id, x_n, x);
    if (!memcmp(o, x, UTM_TAG_LEN)) return 0;
    memcpy(S.m.o, o, UTM_TAG_LEN);
    memcpy(S.m.x, x, UTM_TAG_LEN);
    S.m.sealed = 1;
    S.m.taken_back = 0;
    return 1;
}

int uti_hit(float u, float v) { return uttt_hit(u, v); }

/* ----------------------------------------------------------- the words */
const char *uti_say(int key)
{
    if (uttt_say(key, &S.m.game, uti_msg_seat(), S.said, sizeof S.said) < 0)
        S.said[0] = 0;
    return S.said;
}

int uti_say_mark(void) { return uttt_say_headline_mark(&S.m.game, uti_msg_seat()); }

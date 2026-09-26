#include "uttt_api.h"
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_draw.h"
#include "../src/uttt_anim.h"
#include "../src/uttt_msg.h"
#include "../src/uttt_say.h"
#include "../src/uttt_lang.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host's names for the kernel's numbers. Two lists, so the compiler
 * holds them together: a key renumbered on one side fails the build. */
_Static_assert(UTI_SEAT_SPECTATOR == UTM_SEAT_SPECTATOR, "seat SPECTATOR");
_Static_assert(UTI_SEAT_X == UTM_SEAT_X, "seat X");
_Static_assert(UTI_SEAT_O == UTM_SEAT_O, "seat O");
_Static_assert(UTI_SEAT_WAITING == UTM_SEAT_WAITING, "seat WAITING");
_Static_assert(UTI_SEAT_OPEN == UTM_SEAT_OPEN, "seat OPEN");
_Static_assert(UTI_DOOR_NONE == UTM_DOOR_NONE, "door NONE");
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
_Static_assert(UTI_SAY_DOOR_AGAIN == UTTT_SAY_DOOR_AGAIN, "say DOOR_AGAIN");
_Static_assert(UTI_SAY_HEADLINE_SPOKEN == UTTT_SAY_HEADLINE_SPOKEN, "say HEADLINE_SPOKEN");
_Static_assert(UTI_SAY_YOU_ARE_SPOKEN == UTTT_SAY_YOU_ARE_SPOKEN, "say YOU_ARE_SPOKEN");
_Static_assert(UTI_SAY_DOOR_RULES == UTTT_SAY_DOOR_RULES, "say DOOR_RULES");
_Static_assert(UTI_SAY_SEND_HINT == UTTT_SAY_SEND_HINT, "say SEND_HINT");
_Static_assert(UTI_SAY_DOOR_SEND == UTTT_SAY_DOOR_SEND, "say DOOR_SEND");
_Static_assert(UTI_SAY_DOOR_COPY == UTTT_SAY_DOOR_COPY && UTI_SAY_DOOR_COPIED == UTTT_SAY_DOOR_COPIED,
               "say DOOR_COPY, DOOR_COPIED");
_Static_assert(UTI_SAY_WATCH_SPOKEN == UTTT_SAY_WATCH_SPOKEN, "say WATCH_SPOKEN");
_Static_assert(UTI_SAY_WATCH_SPOKEN + 1 == UTTT_SAY_COUNT, "every key has a host name");
_Static_assert(UTI_TAG_LEN == UTM_TAG_LEN, "tag length");
_Static_assert(UTI_SEATS_BYTES == UTM_REC_BYTES, "seat records");
_Static_assert(UTI_BY_NONE == UTM_BY_NONE && UTI_BY_RECORD == UTM_BY_RECORD
            && UTI_BY_TAG == UTM_BY_TAG && UTI_BY_SENDER == UTM_BY_SENDER, "witnesses");
_Static_assert(UTI_CH_STILL == UTTT_CH_STILL && UTI_CH_STAGE == UTTT_CH_STAGE
            && UTI_CH_REPLAY == UTTT_CH_REPLAY && UTI_CH_THEIRS == UTTT_CH_THEIRS
            && UTI_CH_ARRIVAL == UTTT_CH_ARRIVAL && UTI_CH_SETTLE == UTTT_CH_SETTLE
            && UTI_CH_DRAFT == UTTT_CH_DRAFT, "motion channels");
_Static_assert(sizeof(UtiMotion) == sizeof(UtttMotion), "motion plan layout");
_Static_assert(sizeof(UtiFrame) == sizeof(UtttFrame), "motion frame layout");
_Static_assert(offsetof(UtiMotion, outline_fade) == offsetof(UtttMotion, outline_fade)
            && offsetof(UtiFrame, outline) == offsetof(UtttFrame, outline)
            && offsetof(UtiFrame, outline_a) == offsetof(UtttFrame, outline_a), "the promise lines up");
_Static_assert(offsetof(UtiMotion, hush) == offsetof(UtttMotion, hush)
            && offsetof(UtiFrame, words) == offsetof(UtttFrame, words), "the words line up");
_Static_assert(UTI_WORDS_BEFORE == UTTT_WORDS_BEFORE && UTI_WORDS_HUSH == UTTT_WORDS_HUSH
            && UTI_WORDS_NOW == UTTT_WORDS_NOW, "the words' names");
_Static_assert(UTI_MSG_TEXT_MAX >= UTM_MAX_TEXT, "the longest link fits the host buffer");

/* The resident game, and the buffers the display list is built into. Sized
 * for a full board with every mark drawn, MEASURED rather than guessed: over
 * 20,000 random finished games (the last mark doubled) the worst was 354
 * polygons from 42,812 points - one polygon per stroke since the pen outlines
 * each stroke once (TESTFLIGHT_PLAN.md 20); it was 37,664 from 244,816 when a
 * stroke was a quad and a disc per sample. `ios-smoke` fills a board and
 * asserts no overflow; measure again after touching the pen - a display list
 * that runs out does not fail, it quietly stops drawing. Everything below is
 * the measurement plus half. */
#define MAX_PT    65000
#define MAX_POLY    600

/* THE BUFFERS ARE ON THE HEAP, NOT IN THE IMAGE (TESTFLIGHT_PLAN.md 12,
 * memory). A static display list is __DATA: every page a finished board ever
 * touched stayed dirty for the life of the extension, about 1.2 MB of an idle
 * drawer that had drawn one board. Now a draw allocates at full capacity
 * (untouched pages cost nothing), and a whole board is TAKEN by the host
 * (uti_take), which owns it until uti_taken_free - so the idle drawer holds
 * only the few pages the last small stroke used. */
static struct {
    /* THE RESIDENT MESSAGE: the roster and the game together, because a move
     * on this board is a move by somebody in a seat and the kernel is the one
     * that knows which. `m.game` is the board everything draws. */
    UtmMsg    m;
    uint8_t   me[UTM_MAX_ID];       /* this device's identity bytes */
    int       me_n;
    /* THIS DEVICE'S SEAT RECORDS (utm_rec_*), the host's bytes: loaded
     * with uti_seats_load, written back when uti_seats_dirty. */
    uint8_t   rec[UTM_REC_BYTES];
    int       rec_n;
    int       rec_dirty;
    /* THE SENDER FACT (uti_msg_sender): about this one message only. */
    uint8_t   sent_msg[UTM_MAX_BYTES];
    int       sent_n;
    int       sent_dm, sent_mine;
    /* THE WITNESS THAT WROTE THE RECORD, for the game in wrote_seed/wrote_o:
     * once a seat is recorded every later question is answered "by record",
     * and the diagnostics would never show what actually seated me. */
    int       wrote_by;
    int32_t   wrote_seed;
    uint8_t   wrote_o[UTM_TAG_LEN];
    char      why[200];
    char      said[160];
    int       overflow;
    UtttDL    dl;
} S;

_Static_assert(sizeof(UtiPoly) == sizeof(UtttPoly)
            && offsetof(UtiPoly, first) == offsetof(UtttPoly, first)
            && offsetof(UtiPoly, n) == offsetof(UtttPoly, n)
            && offsetof(UtiPoly, rgba) == offsetof(UtttPoly, rgba), "the host reads the kernel's polygons in place");
_Static_assert(sizeof(UtttPt) == 2 * sizeof(float), "a point is two floats");

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
 * point should depend on. The buffers come back after a take; if they cannot,
 * the capacity is zero and the overflow flag says so. */
static void dl_fresh(void)
{
    if (!S.dl.pt)   S.dl.pt   = malloc(sizeof(UtttPt) * MAX_PT);
    if (!S.dl.poly) S.dl.poly = malloc(sizeof(UtttPoly) * MAX_POLY);
    int ok = S.dl.pt && S.dl.poly;
    uttt_dl_init(&S.dl, S.dl.pt, ok ? MAX_PT : 0, S.dl.poly, ok ? MAX_POLY : 0);
    if (!ok) S.overflow = 1;
}

static int publish(void) { return S.dl.n_poly; }

const float   *uti_points(void)      { return (const float *)S.dl.pt; }
int            uti_point_count(void) { return S.dl.n_pt; }
const UtiPoly *uti_polys(void)       { return (const UtiPoly *)S.dl.poly; }

/* The last draw, handed over whole and trimmed to what it used; the next
 * draw starts on fresh buffers. */
UtiTaken uti_take(void)
{
    UtiTaken t = { 0 };
    if (!S.dl.pt || !S.dl.poly || S.dl.n_poly <= 0) return t;
    size_t np = (size_t)(S.dl.n_pt > 0 ? S.dl.n_pt : 1), nq = (size_t)S.dl.n_poly;
    float   *pt = realloc(S.dl.pt, sizeof(UtttPt) * np);
    UtiPoly *pq = realloc(S.dl.poly, sizeof(UtttPoly) * nq);
    t.points = pt ? pt : (float *)S.dl.pt;
    t.polys  = pq ? pq : (UtiPoly *)S.dl.poly;
    t.n_points = S.dl.n_pt;
    t.n_polys  = S.dl.n_poly;
    S.dl.pt = NULL; S.dl.poly = NULL;
    uttt_dl_init(&S.dl, NULL, 0, NULL, 0);
    return t;
}

void uti_taken_free(UtiTaken t)
{
    free((void *)t.points);
    free((void *)t.polys);
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

int uti_draw_bubble(int active, int last)
{
    dl_fresh();
    UtttDrawOpts o = uttt_draw_opts(S.m.seed);
    o.active = active; o.last = last;
    o.reach = uttt_bubble(&S.m.game).reach;
    S.overflow = uttt_draw_board(&S.dl, &S.m.game, &o) != 0;
    return publish();
}

/* A display list that runs out of room does not fail - it stops appending,
 * and the board comes back with a few marks missing. Nothing on screen says
 * so, which is why it gets its own question. */
int uti_draw_overflow(void) { return S.overflow; }

int uti_draw_under(void)
{
    dl_fresh();
    UtttDrawOpts o = uttt_draw_opts(S.m.seed);
    o.active = -1;
    o.last = S.m.game.n_plies ? S.m.game.move[S.m.game.n_plies - 1] : -1;
    o.mark_t = 0.f; o.fall_t = 0.f; o.meta_t = 0.f;
    S.overflow = uttt_draw_board(&S.dl, &S.m.game, &o) != 0;
    return publish();
}

int uti_draw_settle(float fall_t, float line_t)
{
    dl_fresh();
    if (uttt_draw_settle(&S.dl, &S.m.game, S.m.seed, fall_t, line_t) < 0 && S.m.game.n_plies)
        S.overflow = 1;
    return publish();
}

int uti_draw_outline(int block, float t)
{
    dl_fresh();
    if (uttt_draw_outline(&S.dl, block, S.m.seed, t) < 0) S.overflow = 1;
    return publish();
}

int32_t uti_motion_rest_ms(void) { return UTTT_MS_REST; }

float uti_board_reach(void) { return .135f * UTTT_REACH; }

int uti_draw_last(float t)
{
    dl_fresh();
    if (uttt_draw_last(&S.dl, &S.m.game, S.m.seed, t) < 0 && S.m.game.n_plies) S.overflow = 1;
    return publish();
}

/* WHICH DOOR AN OPENED BUBBLE CAME THROUGH is a seat question: my own move
 * replays at my wash's pace (C), anybody else's at theirs (D). */
UtiMotion uti_motion(int ch)
{
    if (ch == UTI_CH_OPEN) {
        const UtttGame *g = &S.m.game;
        int mine = g->n_plies && uti_msg_mark()
                && uttt_cell(g, g->move[g->n_plies - 1]) == uti_msg_mark();
        ch = mine ? UTTT_CH_REPLAY : UTTT_CH_THEIRS;
    }
    UtttMotion m = uttt_motion(&S.m.game, ch);
    UtiMotion u;
    memcpy(&u, &m, sizeof u);
    return u;
}

void uti_motion_at(const UtiMotion *m, int32_t now_ms, UtiFrame *f)
{
    UtttMotion k; UtttFrame fr;
    memcpy(&k, m, sizeof k);
    uttt_motion_at(&k, now_ms, &fr);
    memcpy(f, &fr, sizeof *f);
}

float   uti_collapse_push(float travel, int32_t t_ms) { return uttt_collapse_push(travel, t_ms); }
int32_t uti_collapse_ms(void)    { return UTTT_COLLAPSE_MS; }
int32_t uti_collapse_steps(void) { return UTTT_COLLAPSE_STEPS; }
float   uti_collapse_flip(void)  { return UTTT_COLLAPSE_FLIP; }
float   uti_spring_left(float travel, float mass, float stiffness, float damping,
                        float v0, int32_t t_ms)
{ return uttt_spring_left(travel, mass, stiffness, damping, v0, t_ms); }
float   uti_spring_past(float travel, float mass, float stiffness, float damping, float v0)
{ return uttt_spring_past(travel, mass, stiffness, damping, v0); }

_Static_assert(sizeof(UtiSheetIn) == sizeof(UtttSheetIn), "UtiSheetIn mirrors UtttSheetIn");
_Static_assert(sizeof(UtiSheet) == sizeof(UtttSheet), "UtiSheet mirrors UtttSheet");
_Static_assert(offsetof(UtiSheet, words) == offsetof(UtttSheet, words)
               && offsetof(UtiSheet, words_side) == offsetof(UtttSheet, words_side)
               && offsetof(UtiSheet, band_alpha) == offsetof(UtttSheet, band_alpha)
               && offsetof(UtiSheet, again) == offsetof(UtttSheet, again)
               && offsetof(UtiSheet, sub_alpha) == offsetof(UtttSheet, sub_alpha)
               && offsetof(UtiSheet, copy) == offsetof(UtttSheet, copy)
               && offsetof(UtiSheet, you) == offsetof(UtttSheet, you)
               && offsetof(UtiSheetIn, copy) == offsetof(UtttSheetIn, copy)
               && offsetof(UtiSheetIn, hint) == offsetof(UtttSheetIn, hint)
               && offsetof(UtiSheetIn, words) == offsetof(UtttSheetIn, words), "the sheet's fields line up");
_Static_assert(UTI_SHEET_PLAY == UTTT_SHEET_PLAY && UTI_SHEET_WATCH == UTTT_SHEET_WATCH
               && UTI_SHEET_WAIT == UTTT_SHEET_WAIT, "sheet kinds");

float uti_sheet_hint_top(void) { return UTTT_SHEET_HINT_TOP; }

UtiSheet uti_sheet(UtiSheetIn in)
{
    UtttSheetIn k;
    UtttSheet o;
    UtiSheet r;
    memcpy(&k, &in, sizeof k);
    uttt_sheet(&k, &o);
    memcpy(&r, &o, sizeof r);
    return r;
}

int uti_draw_mark(int mark, int32_t seed, float board)
{
    dl_fresh();
    uttt_draw_mark(&S.dl, mark, seed ? seed : 1, board);
    return publish();
}

int uti_draw_rulebook(float w, float h)
{
    dl_fresh();
    uttt_draw_rulebook(&S.dl, w, h);
    return publish();
}

int uti_draw_door(float w, float h)
{
    dl_fresh();
    S.overflow = uttt_draw_door(&S.dl, w, h) != 0;
    return publish();
}

void uti_paper(uint8_t *rgba, int w, int h) { uttt_paper(rgba, w, h); }

/* THE SAME PAPER in the compositor's own layout - BGRA, rows `stride` bytes
 * apart - so the host paints it straight into the one surface it shows
 * (UtttBitmap) and keeps no second copy. The paper is opaque, so premultiplied
 * and straight are the same bytes. Bytes past each row's pixels are left as
 * they were. */
void uti_paper_bgra(uint8_t *dst, int w, int h, int stride)
{
    if (!dst || w <= 0 || h <= 0 || stride < w * 4) return;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return;
    uttt_paper(px, w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)y * (size_t)w * 4;
        uint8_t *d = dst + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
    free(px);
}

/* ----------------------------------------------------------- the bubble */
void uti_bubble_size(float *w, float *h)
{
    UtttBubble b = uttt_bubble(&S.m.game);
    if (w) *w = b.w;
    if (h) *h = b.h;
}

void uti_bubble_board(float *x, float *y, float *side)
{
    UtttBubble b = uttt_bubble(&S.m.game);
    if (x)    *x    = b.board.x;
    if (y)    *y    = b.board.y;
    if (side) *side = b.board.w;
}

void uti_bubble_text(float *x, float *y, float *w, float *h)
{
    UtttBubble b = uttt_bubble(&S.m.game);
    if (x) *x = b.text.x;
    if (y) *y = b.text.y;
    if (w) *w = b.text.w;
    if (h) *h = b.text.h;
}

float uti_bubble_type(int line)
{
    UtttBubble b = uttt_bubble(&S.m.game);
    return line ? b.place_pt : b.headline_pt;
}

uint32_t uti_bubble_ink(int line)
{
    UtttBubble b = uttt_bubble(&S.m.game);
    return line ? b.place_rgba : b.headline_rgba;
}

float uti_bubble_lead(void) { return uttt_bubble(&S.m.game).lead; }
float uti_bubble_scale(float display) { return uttt_bubble_scale(display); }

int         uti_rules_count(void)      { return uttt_rules_count(); }
const char *uti_rules_line(int i)      { return uttt_rules_line(i); }
const char *uti_rules_title(void)      { return uttt_rules_title(); }
int uti_rules_yellow(int i, int *at, int *len) { return uttt_rules_yellow(i, at, len); }

_Static_assert(sizeof(UtiRulesLook) == sizeof(UtttRulesLook)
            && offsetof(UtiRulesLook, tint) == offsetof(UtttRulesLook, tint)
            && offsetof(UtiRulesLook, word_room) == offsetof(UtttRulesLook, word_room),
               "the rules sheet's look lines up");
UtiRulesLook uti_rules_look(void)
{
    UtttRulesLook L = uttt_rules_look();
    UtiRulesLook o;
    memcpy(&o, &L, sizeof o);
    return o;
}

int uti_draw_rule(int i)
{
    dl_fresh();
    S.overflow = uttt_draw_rule(&S.dl, i) != 0;
    return publish();
}

int uti_draw_rule_box(float w, float h)
{
    dl_fresh();
    S.overflow = uttt_draw_rule_box(&S.dl, w, h) != 0;
    return publish();
}

const char *uti_place_name(int block) { return uttt_place_name(block); }

const char *uti_lang_prefer(const char *tags) { return uttt_lang_code(uttt_lang_prefer(tags)); }


/* ---------------------------------------------------------- the message */

/* The sender fact, if it is about the resident message: 1, 0, or
 * UTM_SENT_UNKNOWN. Bound to the exact bytes, because the resident is often
 * not the bubble that was tapped (a staged draft outranks it, a move lands
 * on it) and a fact about one message is a lie about any other. */
static int sent_fact(int *dm)
{
    uint8_t b[UTM_MAX_BYTES];
    int n = S.sent_n ? utm_encode(&S.m, b, sizeof b) : -1;
    int same = n > 0 && n == S.sent_n && !memcmp(b, S.sent_msg, (size_t)n);
    *dm = same && S.sent_dm;
    return same ? S.sent_mine : UTM_SENT_UNKNOWN;
}

/* MY SEAT ON THE RESIDENT GAME, by utm_resolve's witnesses, and the tag
 * that plays it: a seat's own tag once I am in it, else the hash of my
 * identity (the open seat takes it; a spectator matches nothing). A seat
 * found any way but the record is recorded, so the next question - after a
 * move, when the sender fact no longer describes the board - asks (a).
 * Every seat question goes through here. */
static int resolve(uint8_t tag[UTM_TAG_LEN], int *by)
{
    uint8_t h[UTM_TAG_LEN];
    int dm, b;
    utm_tag(S.m.seed, S.me, S.me_n, h);
    int rec = utm_rec_find(S.rec, S.rec_n, &S.m);
    int sent = sent_fact(&dm);
    int seat = utm_resolve(&S.m, rec, utm_seat(&S.m, h), dm, sent, &b);
    if (b != UTM_BY_RECORD && (seat == UTM_SEAT_X || seat == UTM_SEAT_O || seat == UTM_SEAT_WAITING)) {
        S.rec_n = utm_rec_put(S.rec, S.rec_n, &S.m, seat);
        S.rec_dirty = 1;
        S.wrote_by = b;
        S.wrote_seed = S.m.seed;
        memcpy(S.wrote_o, S.m.o, UTM_TAG_LEN);
    } else if (b == UTM_BY_RECORD && S.wrote_by && S.wrote_seed == S.m.seed
               && !memcmp(S.wrote_o, S.m.o, UTM_TAG_LEN)) {
        b = S.wrote_by;                 /* this session's record: its witness */
    }
    if (by) *by = b;
    if (tag) {
        switch (seat) {
        case UTM_SEAT_X:                       memcpy(tag, S.m.x, UTM_TAG_LEN); break;
        case UTM_SEAT_O: case UTM_SEAT_WAITING: memcpy(tag, S.m.o, UTM_TAG_LEN); break;
        default:                               memcpy(tag, h, UTM_TAG_LEN); break;
        }
    }
    return seat;
}

static void my_tag(uint8_t out[UTM_TAG_LEN]) { resolve(out, NULL); }

void uti_seats_load(const uint8_t *bytes, int n)
{
    if (!bytes || n < 0) n = 0;
    if (n > UTM_REC_BYTES) n = UTM_REC_BYTES;
    n -= n % UTM_REC_LEN;
    if (n) memcpy(S.rec, bytes, (size_t)n);
    S.rec_n = n;
    S.rec_dirty = 0;
    S.wrote_by = 0;
}

int uti_seats_dirty(void) { return S.rec_dirty; }

int uti_seats_save(uint8_t *out, int cap)
{
    if (!out || cap < S.rec_n) return -1;
    memcpy(out, S.rec, (size_t)S.rec_n);
    S.rec_dirty = 0;
    return S.rec_n;
}

void uti_msg_sender(const char *text, int is_dm, int i_sent)
{
    UtmMsg m;
    S.sent_n = 0;
    if (!text || (i_sent != 0 && i_sent != 1) || utm_text_decode(text, &m) != UTM_EOK) return;
    int n = utm_encode(&m, S.sent_msg, sizeof S.sent_msg);
    if (n <= 0) return;
    S.sent_n = n;
    S.sent_dm = is_dm != 0;
    S.sent_mine = i_sent;
}

int uti_msg_record(void) { return utm_rec_find(S.rec, S.rec_n, &S.m); }

int uti_msg_claim(int seat)
{
    int n = utm_rec_put(S.rec, S.rec_n, &S.m, seat);
    if (utm_rec_find(S.rec, n, &S.m) != (seat == UTI_SEAT_WAITING ? UTI_SEAT_O : seat)) return 0;
    S.rec_n = n;
    S.rec_dirty = 1;
    S.wrote_by = 0;
    return 1;
}

void uti_msg_forget(void)
{
    S.rec_n = utm_rec_forget(S.rec, S.rec_n, &S.m);
    S.rec_dirty = 1;
    S.wrote_by = 0;
}

int uti_msg_seat_by(void)
{
    int by;
    resolve(NULL, &by);
    return by;
}

int uti_msg_tag(int which, uint8_t out[UTI_TAG_LEN])
{
    switch (which) {
    case UTI_TAG_ME:     my_tag(out); return 1;
    case UTI_TAG_HASHED: utm_tag(S.m.seed, S.me, S.me_n, out); return 1;
    case UTI_TAG_O:      memcpy(out, S.m.o, UTM_TAG_LEN); return 1;
    case UTI_TAG_X:      memcpy(out, S.m.x, UTM_TAG_LEN); return 1;
    default:             return 0;
    }
}

void uti_msg_tag_of(const uint8_t *id, int n, uint8_t out[UTI_TAG_LEN])
{
    if (!id || n < 0) n = 0;
    if (n > UTM_MAX_ID) n = UTM_MAX_ID;
    utm_tag(S.m.seed, id, n, out);
}

const char *uti_msg_seat_why(void)
{
    static const char *const by_name[] = { "nothing", "the record", "the tag", "the sender" };
    static const char *const seat_name[] = { "spectator", "X", "O", "waiting", "open" };
    uint8_t h[UTM_TAG_LEN];
    int by;
    int seat = resolve(NULL, &by);
    utm_tag(S.m.seed, S.me, S.me_n, h);
    snprintf(S.why, sizeof S.why, "%s by %s; tag alone: %s",
             seat_name[seat], by_name[by], utm_seat_why(&S.m, h));
    return S.why;
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
    S.rec_n = utm_rec_put(S.rec, S.rec_n, &S.m, UTM_SEAT_O);   /* I created it */
    S.rec_dirty = 1;
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
    int joining = !S.m.sealed;
    if (!utm_play(&S.m, me, mv)) return 0;
    if (joining) {                                      /* I took X */
        S.rec_n = utm_rec_put(S.rec, S.rec_n, &S.m, UTM_SEAT_X);
        S.rec_dirty = 1;
    }
    return 1;
}

int uti_msg_undo(void)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_undo(&S.m, me);
}

int uti_msg_can_replace(int mv)
{
    uint8_t me[UTM_TAG_LEN];
    my_tag(me);
    return utm_can_replace(&S.m, me, mv);
}

int uti_msg_door(void) { return utm_door(&S.m); }

int uti_send_hint_ms(void) { return UTM_SEND_HINT_MS; }
int uti_replay_url(char *out, int cap) { return uttt_replay_url(&S.m.game, S.m.seed, out, cap); }

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
int uti_say_watch_mark(void) { return uttt_say_watch_mark(&S.m.game); }

/* THE WORDS WAIT FOR THE INK (UI.html: the headline turns when the mark has
 * landed). Until the motion's frame says `landed`, a screen speaks of the
 * position before the last move - the same seat, one ply back. */
const char *uti_say_before(int key)
{
    UtttGame g = S.m.game;
    uttt_undo(&g);
    if (uttt_say(key, &g, uti_msg_seat(), S.said, sizeof S.said) < 0)
        S.said[0] = 0;
    return S.said;
}

int uti_say_mark_before(void)
{
    UtttGame g = S.m.game;
    uttt_undo(&g);
    return uttt_say_headline_mark(&g, uti_msg_seat());
}

const char *uti_say_by(int key, const char *who)
{
    if (uttt_say_by(key, &S.m.game, uti_msg_seat(), who, S.said, sizeof S.said) < 0)
        S.said[0] = 0;
    return S.said;
}

int uti_say_bubble_mark(void) { return uttt_say_bubble_mark(&S.m.game); }
uint32_t uti_say_headline_ink(void) { return uttt_say_headline_ink(&S.m.game); }

const char *uti_say_cell(int mv)
{
    if (uttt_say_cell(&S.m.game, mv, S.said, sizeof S.said) < 0)
        S.said[0] = 0;
    return S.said;
}

int uti_cell_rect(int mv, float r[4]) { return uttt_cell_rect(mv, r); }

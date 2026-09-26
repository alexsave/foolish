/* The replay page's kernel: uttt.live/<code>, in a browser.
 *
 * THE SAME DRAWING AS THE PHONE. The board on the web page is the display
 * list uttt_draw_board builds for the app - the same polygons, the same seed,
 * so the same napkin and the same wobble in every stroke - and the page only
 * fills them. A second renderer in JavaScript would be a second hand, and the
 * replay would stop looking like the game it replays.
 *
 * ONE GAME AT A TIME, in statics: the page loads a code, seeks to a ply and
 * asks for a frame. Nothing here allocates.
 *
 * NO LIBC. The build is -nostdlib -ffreestanding (Makefile, wasm-web); the
 * few libc and libm functions the drawing kernel calls come from
 * shared/c/wasm, the freestanding library every wasm kernel here links.
 */
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_draw.h"
#include "../src/uttt_anim.h"
#include "../src/uttt_say.h"

#define EXPORT(name) __attribute__((export_name(#name)))

/* ------------------------------------------------------------ the replay */
#define MAX_PT    65000
#define MAX_POLY    600
#define PAPER_MAX   512

static UtttGame full, cur;
static int32_t  seed;
static UtttPt   pt[MAX_PT];
static UtttPoly poly[MAX_POLY];
static UtttDL   dl;
static char     code_in[256];
static UtttMotion motion;       /* the last ply's, uw_motion */
static uint8_t  paper[PAPER_MAX * PAPER_MAX * 4];

/* the page writes the code (letters and digits, NUL-terminated) here */
EXPORT(uw_code_ptr) char *uw_code_ptr(void) { return code_in; }
EXPORT(uw_code_cap) int   uw_code_cap(void) { return (int)sizeof code_in; }

/* Read the code. The whole game's plies, or 0 for a code that is not a game.
 * The position starts at the empty board. */
EXPORT(uw_load) int uw_load(void)
{
    code_in[sizeof code_in - 1] = 0;
    if (!uttt_replay_read(code_in, &full, &seed) || full.n_plies == 0) return 0;
    uttt_init(&cur);
    motion = uttt_motion(&cur, UTTT_CH_STILL);
    return full.n_plies;
}

/* Move the position to after `k` plies of the loaded game, at rest. */
EXPORT(uw_seek) int uw_seek(int k)
{
    if (k < 0) k = 0;
    if (k > full.n_plies) k = full.n_plies;
    uttt_init(&cur);
    for (int i = 0; i < k; i++) uttt_play(&cur, full.move[i]);
    motion = uttt_motion(&cur, UTTT_CH_STILL);
    return cur.n_plies;
}

EXPORT(uw_plies)  int uw_plies(void)  { return full.n_plies; }
EXPORT(uw_move)   int uw_move(int i)  { return i >= 0 && i < full.n_plies ? full.move[i] : -1; }
EXPORT(uw_over)   int uw_over(void)   { return cur.over; }          /* 0, 1 X, 2 O, 3 draw */
EXPORT(uw_turn)   int uw_turn(void)   { return cur.turn; }
EXPORT(uw_active) int uw_active(void) { return uttt_active(&cur); } /* 0..8, 9 anywhere, -1 over */
EXPORT(uw_cell)   int uw_cell(int i)  { return i >= 0 && i < 81 ? uttt_cell(&cur, i) : 0; }
EXPORT(uw_block)  int uw_block(int b) { return b >= 0 && b < 9 ? uttt_block(&cur, b) : 0; }

/* THE MOTION, the app's own (uttt_anim.h): a plan for the last ply and a
 * pure function of the clock, so the page runs one animation-frame loop,
 * asks for the frame at `now_ms` and schedules nothing. A step forward plays
 * the move as the receiver sees one - the mark, the big mark of a block it
 * won, the win line, then the highlighter's travel; anything else rests. */
EXPORT(uw_motion) void uw_motion(int animate)
{
    motion = uttt_motion(&cur, animate ? UTTT_CH_THEIRS : UTTT_CH_STILL);
}

/* How long a settled move rests before the next may start (UTTT_MS_REST). */
EXPORT(uw_rest_ms) int uw_rest_ms(void) { return UTTT_MS_REST; }

/* The board at `now_ms` into the plan, as the app composes it: the
 * highlighter's rect under everything, the board with the last mark, its
 * block's big mark and the win line drawn as far as the frame says, and the
 * promised outline over the top. Returns 1 while anything is still moving.
 * The polygons are read with uw_poly_* and uw_points until the next frame. */
EXPORT(uw_frame) int uw_frame(int now_ms)
{
    UtttFrame f;
    uttt_motion_at(&motion, now_ms, &f);
    uttt_dl_init(&dl, pt, MAX_PT, poly, MAX_POLY);
    if (f.wash[2] > 0 && dl.n_poly < dl.cap_poly && dl.n_pt + 4 <= dl.cap_pt) {
        UtttPoly *p = &dl.poly[dl.n_poly++];
        p->first = dl.n_pt; p->n = 4; p->rgba = f.wash_rgba;
        float x = f.wash[0], y = f.wash[1], w = f.wash[2], h = f.wash[3];
        dl.pt[dl.n_pt++] = (UtttPt){ x, y };
        dl.pt[dl.n_pt++] = (UtttPt){ x + w, y };
        dl.pt[dl.n_pt++] = (UtttPt){ x + w, y + h };
        dl.pt[dl.n_pt++] = (UtttPt){ x, y + h };
    }
    UtttDrawOpts o = uttt_draw_opts(seed);
    o.active = -1;
    o.last   = cur.n_plies ? cur.move[cur.n_plies - 1] : -1;
    o.mark_t = f.mark_t; o.fall_t = f.fall_t; o.meta_t = f.line_t;
    uttt_draw_board(&dl, &cur, &o);
    if (f.outline >= 0 && f.outline_a > 0) uttt_draw_outline(&dl, f.outline, seed, f.outline_t);
    return f.running;
}

/* The frame's polygons, one question each, so the page knows no layout:
 * where its points start in uw_points, how many, and its ink as 0xRRGGBBAA. */
EXPORT(uw_poly_count) int      uw_poly_count(void)     { return dl.n_poly; }
EXPORT(uw_poly_first) int      uw_poly_first(int i)    { return i >= 0 && i < dl.n_poly ? dl.poly[i].first : 0; }
EXPORT(uw_poly_len)   int      uw_poly_len(int i)      { return i >= 0 && i < dl.n_poly ? dl.poly[i].n : 0; }
EXPORT(uw_poly_ink)   uint32_t uw_poly_ink(int i)      { return i >= 0 && i < dl.n_poly ? dl.poly[i].rgba : 0; }
/* x, y floats a point, 0..1 of the board */
EXPORT(uw_points)      float  *uw_points(void)      { return (float *)pt; }
EXPORT(uw_point_count) int     uw_point_count(void) { return dl.n_pt; }

/* THE LINE UNDER THE BOARD, in the kernel's words (uttt_caption, the
 * bubble's own line): "O to play",
 * "X won on the diagonal in 41 moves". Nobody is named - a replay
 * has marks, not people. */
static char caption[128];
EXPORT(uw_caption) const char *uw_caption(void)
{
    /* the empty board's block is 9, anywhere, never -1: that is the
     * invitation's "New game?", and a replay's first frame is a game */
    int n = uttt_caption(cur.over, cur.turn, uttt_active(&cur),
                         uttt_won_line(&cur), cur.n_plies, NULL, caption, sizeof caption);
    if (n < 0) caption[0] = 0;
    return caption;
}

/* The napkin, uttt_paper, RGBA at side x side (at most PAPER_MAX). */
EXPORT(uw_paper) uint8_t *uw_paper(int side)
{
    if (side < 1) side = 1;
    if (side > PAPER_MAX) side = PAPER_MAX;
    uttt_paper(paper, side, side);
    return paper;
}

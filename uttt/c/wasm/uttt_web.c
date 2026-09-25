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
 * NO LIBC. The build is -nostdlib -ffreestanding (Makefile, wasm-web), so the
 * handful of libc and libm functions the drawing kernel calls are here too.
 * The trig, exp, log and pow are ordinary range-reduced series: close to a
 * libm, not bit-identical to Apple's, which a picture can afford.
 */
#include "../src/uttt.h"
#include "../src/uttt_code.h"
#include "../src/uttt_draw.h"
#include "../src/uttt_anim.h"
#include "../src/uttt_say.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define EXPORT(name) __attribute__((export_name(#name)))

/* ------------------------------------------------------------------ libc */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

/* snprintf for the kernel's sentences: %s, %d and %%, nothing else. Like
 * the real one it returns the length it wanted and always terminates. */
int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "";
            while (*s) PUT(*s++);
        } else if (*f == 'd') {
            int v = va_arg(ap, int);
            unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
            char d[12]; int k = 0;
            do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) PUT('-');
            while (k) PUT(d[--k]);
        } else if (*f == '%') {
            PUT('%');
        } else if (!*f) {
            break;
        }
    }
#undef PUT
    if (cap) out[n < cap ? n : cap - 1] = 0;
    va_end(ap);
    return (int)n;
}

/* ------------------------------------------------------------------ libm */
#define PI  3.14159265358979323846
#define LN2 0.69314718055994530942

/* sin on [-pi/4, pi/4] and cos on the same, then quadrant by quadrant */
static double sin_k(double x)
{
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 * (1 - x2 / 156))))));
}
static double cos_k(double x)
{
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 * (1 - x2 / 132)))));
}
static double quad(double x, int *q)
{
    double k = __builtin_floor(x / (PI / 2) + .5);
    *q = (int)((long long)k & 3);
    return x - k * (PI / 2);
}
double sin(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return sin_k(r); case 1: return cos_k(r); case 2: return -sin_k(r); default: return -cos_k(r); }
}
double cos(double x)
{
    int q; double r = quad(x, &q);
    switch (q) { case 0: return cos_k(r); case 1: return -sin_k(r); case 2: return -cos_k(r); default: return sin_k(r); }
}

/* e^x = 2^k e^r, |r| <= ln2/2, and 2^k built straight into the exponent */
double exp(double x)
{
    if (x > 709) return __builtin_inf();
    if (x < -745) return 0;
    double k = __builtin_floor(x / LN2 + .5), r = x - k * LN2, t = 1, s = 1;
    for (int i = 1; i < 14; i++) { t *= r / i; s += t; }
    union { double d; unsigned long long u; } p;
    int e = (int)k;
    /* split a scale that would leave the normal range into two steps */
    if (e < -1000) { s *= 0x1p-1000; e += 1000; }
    if (e > 1000)  { s *= 0x1p1000;  e -= 1000; }
    p.u = (unsigned long long)(e + 1023) << 52;
    return s * p.d;
}

/* ln x = e ln2 + 2 atanh((m-1)/(m+1)), m in [sqrt(1/2), sqrt(2)) */
double log(double x)
{
    if (!(x > 0)) return x == 0 ? -__builtin_inf() : __builtin_nan("");
    union { double d; unsigned long long u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7ff);
    if (e == 0) { v.d = x * 0x1p54; e = (int)((v.u >> 52) & 0x7ff) - 54; }
    e -= 1023;
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m = v.d;
    if (m > 1.41421356237309504880) { m /= 2; e++; }
    double z = (m - 1) / (m + 1), z2 = z * z, t = z, s = 0;
    for (int i = 1; i < 40; i += 2) { s += t / i; t *= z2; }
    return e * LN2 + 2 * s;
}
double log2(double x) { return log(x) / LN2; }

float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float logf(float x) { return (float)log(x); }

float powf(float x, float y)
{
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : __builtin_inff();
    if (x < 0) return __builtin_nanf("");   /* the kernel never asks */
    return (float)exp(y * log(x));
}

/* atan by halving the angle until the series is short, then acos from it */
static double atan_(double x)
{
    int neg = x < 0; if (neg) x = -x;
    int inv = x > 1; if (inv) x = 1 / x;
    int h = 0;
    while (x > .2) { x = x / (1 + __builtin_sqrt(1 + x * x)); h++; }
    double x2 = x * x, t = x, s = 0;
    for (int i = 1; i < 30; i += 2) { s += t / i; t *= -x2; }
    s *= (double)(1 << h);
    if (inv) s = PI / 2 - s;
    return neg ? -s : s;
}
float acosf(float x)
{
    if (x >= 1) return 0;
    if (x <= -1) return (float)PI;
    return (float)(2 * atan_(__builtin_sqrt((1 - (double)x) / (1 + (double)x))));
}

/* libm's NaN rule: a NaN argument loses to the number */
float fminf(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
float fmaxf(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }

long lroundf(float x) { return (long)(x < 0 ? x - .5f : x + .5f); }

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
 * bubble's own line): "O to play, bottom-middle board", "X to play,
 * anywhere", "X won on the diagonal in 41 moves". Nobody is named - a replay
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

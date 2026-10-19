/* The replay page's kernel (wasm/uttt_web.c) and its settled-frame cache.
 *
 * uw_frame is a pure function of the position, the drawing byte and the
 * UtttFrame the motion gives at `now_ms`; the page asks again every animation
 * frame, and a frame that is the same as the last returns the display list it
 * already holds. This test compiles the page's own file (the wasm export
 * attribute is made empty, so it is plain C here) and holds the cache to the
 * rebuild it stands in for:
 *   - a cached frame is byte-identical to one rebuilt with the cache dropped
 *     (cache_ok = 0), for several games, looks, plies and clocks, and the cache
 *     really is what answered (a poisoned point survives a repeat call);
 *   - walking the clock the way the page does (no cache drop between calls)
 *     gives, at every step, the rebuilt list: the key covers every field the
 *     drawing reads;
 *   - uw_seek, uw_load and uw_motion each leave the next frame equal to a
 *     rebuild of the new input.
 * It also times 1000 settled frames with the cache on and with it off.
 *
 * MEASURED (2026-10-18, arm64 Mac, -O3 -flto, a 40-ply game's last position,
 * 1000 settled uw_frame calls): see the line this test prints, "settled frame".
 */
#define export_name(n)
#include "../wasm/uttt_web.c"
#undef export_name
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../../shared/c/rng.h"
#include "uttt_fixture.h"
#define CHECK_PRINT_CAP 1000000
#include "../../../shared/c/test/check.h"

/* Everything uw_frame hands the page, copied. */
typedef struct {
    int      n_poly, n_pt, running;
    UtttPoly poly[MAX_POLY];
    UtttPt   pt[MAX_PT];
} Snap;

static Snap *snap_new(void) { return calloc(1, sizeof(Snap)); }
static void take(Snap *s, int running)
{
    s->running = running;
    s->n_poly = uw_poly_count();
    s->n_pt = uw_point_count();
    memcpy(s->poly, dl.poly, (size_t)s->n_poly * sizeof s->poly[0]);
    memcpy(s->pt, dl.pt, (size_t)s->n_pt * sizeof s->pt[0]);
}
static int same(const Snap *a, const Snap *b)
{
    return a->running == b->running && a->n_poly == b->n_poly && a->n_pt == b->n_pt
        && !memcmp(a->poly, b->poly, (size_t)a->n_poly * sizeof a->poly[0])
        && !memcmp(a->pt, b->pt, (size_t)a->n_pt * sizeof a->pt[0]);
}

/* The frame with the cache dropped first: the rebuild. */
static void rebuilt(Snap *s, int t) { cache_ok = 0; take(s, uw_frame(t)); }

/* Load a game of `n` random plies (or the diagonal fixture for n == 0) with
 * drawing byte `lk`, as the page does: the link text through uw_code_ptr. */
static int load(uint64_t seed, int n, uint8_t lk)
{
    UtttGame g;
    uttt_init(&g);
    if (n == 0) {
        for (unsigned i = 0; i < sizeof diag_fixture; i++) uttt_play(&g, diag_fixture[i]);
    } else {
        uint64_t r = seed;
        for (int i = 0; i < n && !g.over; i++) {
            uint8_t mv[81];
            int k = uttt_legal(&g, mv);
            uttt_play(&g, mv[rng_u32(&r) % (uint32_t)k]);
        }
    }
    char url[256];
    if (uttt_replay_url(&g, lk, url, sizeof url) < 0) return -1;
    strcpy(uw_code_ptr(), url);
    return uw_load();
}

static const int CLOCKS[] = { 0, 60, 150, 300, 700, 1200, 5000 };
#define NCLOCKS ((int)(sizeof CLOCKS / sizeof CLOCKS[0]))

static void test_cached_equals_rebuilt(void)
{
    TEST("a cached frame is the rebuilt frame");
    static const struct { uint64_t seed; int n; uint8_t look; } G[] = {
        { 0, 0, 0 }, { 11, 12, 3 }, { 29, 40, 7 }, { 5, 30, 200 },
    };
    Snap *a = snap_new(), *b = snap_new(), *c = snap_new();
    for (unsigned gi = 0; gi < sizeof G / sizeof G[0]; gi++) {
        int plies = load(G[gi].seed, G[gi].n, G[gi].look);
        OK(plies > 0, "the game loads");
        int ks[] = { 0, 1, plies / 2, plies - 1, plies };
        for (int ki = 0; ki < 5; ki++) {
            for (int animate = 0; animate < 2; animate++) {
                uw_seek(ks[ki]);
                uw_motion(animate);
                for (int ti = 0; ti < NCLOCKS; ti++) {
                    int t = CLOCKS[ti];
                    rebuilt(a, t);
                    OK(a->n_pt > 0, "the board draws something");
                    /* the repeat call is answered by the cache: a poisoned point stays */
                    float keep = dl.pt[0].x;
                    dl.pt[0].x = 12345.f;
                    int run = uw_frame(t);
                    OK(dl.pt[0].x == 12345.f, "the repeat call did not rebuild");
                    dl.pt[0].x = keep;
                    take(b, run);
                    OK(same(a, b), "cached list == rebuilt list");
                    rebuilt(c, t);
                    OK(same(a, c), "a rebuild is repeatable");
                }
            }
        }
    }
    free(a); free(b); free(c);
}

/* The page's loop: the clock only moves forward and nothing drops the cache.
 * Every step must equal the rebuild at that clock. */
static void test_walk_the_clock(void)
{
    TEST("walking the clock never serves a stale frame");
    Snap *w = snap_new(), *r = snap_new();
    int changes = 0;
    for (int gi = 0; gi < 3; gi++) {
        int plies = load(100 + (uint64_t)gi, gi == 0 ? 0 : 25, (uint8_t)(gi * 9));
        for (int k = 1; k <= plies; k += (plies > 10 ? 4 : 1)) {
            uw_seek(k);
            uw_motion(1);
            cache_ok = 0;
            int prev_n = -1;
            for (int t = 0; t <= 2600; t += 10) {
                int run = uw_frame(t);
                take(w, run);
                /* compare against a fresh build, then put the cache state back as the walk had it */
                UtttFrame keepf = cache_f;
                Snap *save = r;
                rebuilt(save, t);
                cache_f = keepf; cache_ok = 1;
                OK(same(w, save), "walking == rebuilt at this clock");
                if (w->n_pt != prev_n) changes++;
                prev_n = w->n_pt;
            }
        }
    }
    OK(changes > 20, "the frames really move while walking");
    free(w); free(r);
}

static void test_inputs_invalidate(void)
{
    TEST("each input that changes the picture drops the cache");
    Snap *a = snap_new(), *b = snap_new();
    int plies = load(77, 30, 4);
    OK(plies > 10, "loaded");

    /* uw_seek: the same motion (at rest), the same frame, another position */
    uw_motion(0);
    uw_seek(5);
    uw_frame(5000);
    take(a, 0);
    uw_seek(10);
    take(b, uw_frame(5000));
    OK(!same(a, b), "another ply is another picture");
    Snap *fresh = snap_new();
    rebuilt(fresh, 5000);
    OK(same(b, fresh), "and it is the rebuild of that ply");

    /* uw_seek, harder: two positions whose UtttFrame is the same (the wash goes to the same
     * block; the frame does not hold the last move) but whose pictures differ. Only dropping
     * the cache in uw_seek tells them apart. */
    {
        enum { MAXK = 41 };
        static UtttFrame F[MAXK];
        static Snap *S[MAXK];
        int n = load(29, 40, 7);
        OK(n > 20 && n < MAXK, "a game for the pair search");
        uw_motion(0);
        for (int k = 1; k <= n && k < MAXK; k++) {
            uw_seek(k);
            S[k] = snap_new();
            rebuilt(S[k], 5000);
            F[k] = cache_f;
        }
        int pairs = 0;
        for (int i = 1; i <= n; i++)
            for (int j = 1; j <= n; j++) {
                if (i == j || memcmp(&F[i], &F[j], sizeof F[i]) || same(S[i], S[j])) continue;
                pairs++;
                uw_seek(i);
                uw_frame(5000);
                uw_seek(j);
                take(b, uw_frame(5000));
                OK(same(b, S[j]), "the next position is drawn, not the cached one");
            }
        OK(pairs > 0, "the search found positions with one frame and two pictures");
        for (int k = 1; k <= n; k++) free(S[k]);
    }

    /* uw_load: another game, another look, at the empty board both times */
    load(1, 6, 1);
    uw_frame(5000);
    take(a, 0);
    load(1, 6, 2);
    take(b, uw_frame(5000));
    OK(!same(a, b), "another drawing byte is another picture");
    rebuilt(fresh, 5000);
    OK(same(b, fresh), "and it is the rebuild of that look");
    /* a load that fails leaves nothing stale: the cache is dropped before the read */
    strcpy(uw_code_ptr(), "not a game");
    OK(uw_load() == 0, "a bad code is refused");
    OK(cache_ok == 0, "and a refused load drops the cache");

    /* uw_motion: a plan in flight is not the page at rest */
    load(77, 30, 4);
    uw_seek(20);
    uw_motion(0);
    uw_frame(150);
    take(a, 0);
    uw_motion(1);
    take(b, uw_frame(150));
    OK(!same(a, b), "mid-ink is not the settled page");
    rebuilt(fresh, 150);
    OK(same(b, fresh), "and it is the rebuild of that plan");
    /* the ink in flight differs from the settled frame, which differs again once the ink is down */
    take(a, uw_frame(0));
    take(b, uw_frame(5000));
    OK(!same(a, b), "the first instant of a move is not its last");
    free(a); free(b); free(fresh);
}

static double now_s(void) { return (double)clock() / (double)CLOCKS_PER_SEC; }

/* 1000 settled frames at the last position of a 40-ply game, cache on and off. */
static void test_cost(void)
{
    TEST("a settled frame is a compare, not a drawing");
    int plies = load(29, 40, 7);
    uw_seek(plies);
    uw_motion(0);
    uw_frame(5000);
    volatile int sink = 0;
    double t0 = now_s();
    for (int i = 0; i < 1000; i++) sink += uw_frame(5000);
    double on = now_s() - t0;
    t0 = now_s();
    for (int i = 0; i < 1000; i++) { cache_ok = 0; sink += uw_frame(5000); }
    double off = now_s() - t0;
    (void)sink;
    printf("settled frame (%d plies): 1000 calls, cache on %.4f s (%.2f us), off %.4f s (%.1f us), %.0fx\n",
           plies, on, on * 1e3, off, off * 1e3, on > 0 ? off / on : 0.0);
    OK(on * 5 < off, "the cached frame is at least 5x cheaper than the rebuild");
}

int main(void)
{
    test_cached_equals_rebuilt();
    test_walk_the_clock();
    test_inputs_invalidate();
    test_cost();
    return report("uttt_web_test");
}

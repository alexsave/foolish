// unit_after_edge.c - the answer-first window (after.h) and the edge cache's
// marks over the app's declared paths (edge.h), with no provider: a deferred
// step's refusal, order and drop, and edge.c's setup, marks, flush and purge
// through a purge function of the test's own.
#include "t.h"

#include "after.h"
#include "edge.h"
#include "util/err.h"
#include "util/log.h"

#include <stdlib.h>

// ---- cb_after_defer -------------------------------------------------------------------------

// Every run and drop, in the order they happened: "r<tag>" and "d<tag>".
static struct { char log[64][24]; int n; } g_a;

static void logged(const char *kind, int tag) {
    if (g_a.n < 64) snprintf(g_a.log[g_a.n++], sizeof g_a.log[0], "%s%d", kind, tag);
}
static void run_a(cb_ctx *ctx, void *arg) { logged("a", *(int *)arg); }
static void run_b(cb_ctx *ctx, void *arg) { logged("b", *(int *)arg); }
static void drop_tag(void *arg) { logged("d", *(int *)arg); }
static void drop_free(void *arg) { logged("f", *(int *)arg); free(arg); }

static int *tag(int v) {
    int *p = malloc(sizeof *p);
    *p = v;
    return p;
}

static void deferred_steps(void) {
    memset(&g_a, 0, sizeof g_a);
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    // Window closed: refused, nothing queued, the caller keeps its arg.
    int one = 1;
    T_CHECK(!cb_after_open());
    T_CHECK(!cb_after_defer(run_a, drop_tag, &one));
    cb_after_run(&ctx);
    T_EQ_INT(g_a.n, 0);
    // Window open: queued, run in call order across kinds, each step's drop
    // right after its own run, and the window closed by the run.
    cb_after_begin();
    T_CHECK(cb_after_open());
    int t1 = 1, t2 = 2, t3 = 3;
    T_CHECK(cb_after_defer(run_a, drop_tag, &t1));
    T_CHECK(cb_after_defer(run_b, NULL, &t2));   // no drop: nothing is called for it
    T_CHECK(cb_after_defer(run_a, drop_free, tag(4)));
    T_CHECK(cb_after_defer(run_b, drop_tag, &t3));
    T_EQ_INT(g_a.n, 0);   // nothing runs before the answer is out
    cb_after_run(&ctx);
    T_CHECK(!cb_after_open());
    const char *want[] = { "a1", "d1", "b2", "a4", "f4", "b3", "d3" };
    T_EQ_INT(g_a.n, 7);
    for (int i = 0; i < 7 && i < g_a.n; i++) T_EQ_STR(g_a.log[i], want[i]);
    // Closed again: a defer after the run is refused, and a second run does nothing.
    T_CHECK(!cb_after_defer(run_a, drop_tag, &t1));
    cb_after_run(&ctx);
    T_EQ_INT(g_a.n, 7);
    // A second window starts empty.
    cb_after_begin();
    T_CHECK(cb_after_defer(run_b, drop_tag, &t2));
    cb_after_run(&ctx);
    T_EQ_INT(g_a.n, 9);
    T_EQ_STR(g_a.log[7], "b2");
    T_EQ_STR(g_a.log[8], "d2");
}

// A step that defers another while it runs: the window is closed by then, so
// it is refused and the step acts at once (after.h: what the queued work
// itself commits acts at once).
static bool g_inner_deferred;
static void run_defers(cb_ctx *ctx, void *arg) {
    static int x = 9;
    g_inner_deferred = cb_after_defer(run_a, NULL, &x);
    logged("o", 0);
}

static void a_step_cannot_defer(void) {
    memset(&g_a, 0, sizeof g_a);
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    cb_after_begin();
    int t = 0;
    T_CHECK(cb_after_defer(run_defers, NULL, &t));
    g_inner_deferred = true;
    cb_after_run(&ctx);
    T_CHECK(!g_inner_deferred);
    T_EQ_INT(g_a.n, 1);
}

// ---- edge.c over declared paths --------------------------------------------------------------------

static struct { int calls, n; const char *paths[CB_EDGE_MAX + 2]; int rc; const cb_config *cfg; } g_p;

static int purge(const cb_config *cfg, const char *const *paths, int n) {
    g_p.calls++;
    g_p.n = n;
    g_p.cfg = cfg;
    for (int i = 0; i < n && i < CB_EDGE_MAX + 2; i++) g_p.paths[i] = paths[i];
    return g_p.rc;
}

static const char *const NINE[] = { "/p0", "/p1", "/p2", "/p3", "/p4", "/p5", "/p6", "/p7", "/p8" };

static void edge_declared_paths(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *out[CB_EDGE_MAX];
    memset(&g_p, 0, sizeof g_p);

    // No paths: nothing can be marked, and a flush sends nothing.
    cb_edge_setup(NINE, 0, purge);
    cb_edge_stale(CB_EDGE_ALL);
    T_EQ_INT(cb_edge_pending(), 0);
    T_EQ_INT(cb_edge_paths(CB_EDGE_ALL, out), 0);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(g_p.calls, 0);
    cb_edge_setup(NULL, 3, purge);   // no array is none, whatever n says
    cb_edge_stale(CB_EDGE_ALL);
    T_EQ_INT(cb_edge_pending(), 0);

    // One path: only bit 0 is declared; a mark past it is ignored, counted nowhere.
    cb_edge_setup(NINE, 1, purge);
    uint64_t m0 = cb_edge_marks(1u << 0), m1 = cb_edge_marks(1u << 1);
    cb_edge_stale(1u << 1);
    T_EQ_INT(cb_edge_pending(), 0);
    T_EQ_INT(cb_edge_marks(1u << 1), m1);
    cb_edge_stale(CB_EDGE_ALL);
    T_EQ_INT(cb_edge_pending(), 1);
    T_EQ_INT(cb_edge_marks(1u << 0), m0 + 1);
    T_EQ_INT(cb_edge_marks(1u << 1), m1);
    T_EQ_INT(cb_edge_paths(CB_EDGE_ALL, out), 1);
    T_EQ_STR(out[0], "/p0");
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(g_p.calls, 1);
    T_EQ_INT(g_p.n, 1);
    T_EQ_STR(g_p.paths[0], "/p0");
    T_CHECK(g_p.cfg == &cfg);   // the purge is handed the flush's config
    T_EQ_INT(cb_edge_pending(), 0);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);   // nothing pending: no purge
    T_EQ_INT(g_p.calls, 1);

    // CB_EDGE_MAX + 1 paths: the first CB_EDGE_MAX are kept, the last is not a path.
    cb_edge_setup(NINE, CB_EDGE_MAX + 1, purge);
    cb_edge_stale(1u << CB_EDGE_MAX);
    T_EQ_INT(cb_edge_pending(), 0);
    T_EQ_INT(cb_edge_marks(1u << CB_EDGE_MAX), 0);   // no counter past the last
    T_EQ_INT(cb_edge_paths(CB_EDGE_ALL, out), CB_EDGE_MAX);
    T_EQ_STR(out[CB_EDGE_MAX - 1], "/p7");
    // Marks merge until a flush; the purge gets them in declared order, once each.
    cb_edge_stale(1u << 5);
    cb_edge_stale(1u << 2);
    cb_edge_stale(1u << 5);
    T_EQ_INT(cb_edge_pending(), (1u << 2) | (1u << 5));
    memset(&g_p, 0, sizeof g_p);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(g_p.calls, 1);
    T_EQ_INT(g_p.n, 2);
    T_EQ_STR(g_p.paths[0], "/p2");
    T_EQ_STR(g_p.paths[1], "/p5");
    cb_edge_stale(CB_EDGE_ALL);
    T_EQ_INT(cb_edge_pending(), CB_EDGE_ALL);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(g_p.n, CB_EDGE_MAX);

    // A purge's error is returned, and its marks are dropped (the s-maxage bounds the staleness).
    g_p.rc = CB_E_IO;
    cb_edge_stale(1u << 3);
    T_EQ_INT(cb_edge_flush(&cfg), CB_E_IO);
    T_EQ_INT(cb_edge_pending(), 0);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);   // nothing left to fail
    g_p.rc = CB_OK;

    // No purge: a flush clears the marks and sends nothing.
    cb_edge_setup(NINE, 2, NULL);
    memset(&g_p, 0, sizeof g_p);
    cb_edge_stale(CB_EDGE_ALL);
    T_EQ_INT(cb_edge_pending(), 3);
    T_EQ_INT(cb_edge_flush(&cfg), CB_OK);
    T_EQ_INT(cb_edge_pending(), 0);
    T_EQ_INT(g_p.calls, 0);
}

// cb_after_run ends with the edge's flush: the marks a deferred step made go
// out once its steps are done.
static void mark_in_step(cb_ctx *ctx, void *arg) { cb_edge_stale(1u << 1); }

static void after_run_flushes_the_edge(void) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    cb_edge_setup(NINE, 2, purge);
    memset(&g_p, 0, sizeof g_p);
    cb_after_begin();
    T_CHECK(cb_after_defer(mark_in_step, NULL, NULL));
    cb_after_run(&ctx);
    T_EQ_INT(g_p.calls, 1);
    T_EQ_INT(g_p.n, 1);
    T_EQ_STR(g_p.paths[0], "/p1");
    T_EQ_INT(cb_edge_pending(), 0);
    cb_edge_setup(NULL, 0, NULL);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    T_RUN(deferred_steps);
    T_RUN(a_step_cannot_defer);
    T_RUN(edge_declared_paths);
    T_RUN(after_run_flushes_the_edge);
    return T_DONE();
}

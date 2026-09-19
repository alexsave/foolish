// The observability endpoints - see metrics.h.
#define _GNU_SOURCE
#include "metrics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "bots.h"
#include "ctl_wire.h"
#include "play.h"
#include "registry.h"

// The one read of every gauge and counter, so /stats and /metrics can never
// report different numbers for the same instant. g_games_count/g_users_count/
// g_free_count are written under g_registry_lock (h_create, game_alloc_slot,
// the reaper); these endpoints are rare and polled, so take the lock briefly
// for a consistent, race-free snapshot rather than reading these ints
// unlocked. "games" is the slot high-water; live == games - free.
static void sample(CtlStats *st) {
    st->bot_decisions     = atomic_load_explicit(&g_bot_decisions, memory_order_relaxed);
    st->octogen_decisions = atomic_load_explicit(&g_octogen_decisions, memory_order_relaxed);
    st->live_connections  = atomic_load_explicit(&g_live_conns, memory_order_relaxed);
    st->moves_applied     = atomic_load_explicit(&g_moves_applied, memory_order_relaxed);
    st->games_reclaimed   = atomic_load_explicit(&g_games_reclaimed, memory_order_relaxed);
    st->max_connections   = g_max_conns;
    pthread_mutex_lock(&g_registry_lock);
    st->games      = g_games_count;
    st->users      = g_users_count;
    st->free_slots = g_free_count;
    pthread_mutex_unlock(&g_registry_lock);
    st->games_live = st->games - st->free_slots;
    if (st->games_live < 0) st->games_live = 0;
}

void h_stats(Req *r, Conn *conn) {
    (void)r;
    CtlStats st;
    sample(&st);
    unsigned char out[CTL_FRAME_MAX];
    int n = ctl_enc_stats(&st, out, (int)sizeof out);
    respond_ctl(conn, 200, out, n);
}

void h_metrics(Req *r, Conn *conn) {
    (void)r;
    CtlStats st;
    sample(&st);
    unsigned long rl = atomic_load_explicit(&g_rate_limited, memory_order_relaxed);
    char out[1200];
    int n = snprintf(out, sizeof out,
        "# TYPE foolish_live_connections gauge\nfoolish_live_connections %d\n"
        "# TYPE foolish_max_connections gauge\nfoolish_max_connections %d\n"
        "# TYPE foolish_games_live gauge\nfoolish_games_live %d\n"
        "# TYPE foolish_free_slots gauge\nfoolish_free_slots %d\n"
        "# TYPE foolish_users gauge\nfoolish_users %d\n"
        "# TYPE foolish_moves_applied_total counter\nfoolish_moves_applied_total %lu\n"
        "# TYPE foolish_games_reclaimed_total counter\nfoolish_games_reclaimed_total %lu\n"
        "# TYPE foolish_bot_decisions_total counter\nfoolish_bot_decisions_total %lu\n"
        "# TYPE foolish_octogen_decisions_total counter\nfoolish_octogen_decisions_total %lu\n"
        "# TYPE foolish_rate_limited_total counter\nfoolish_rate_limited_total %lu\n",
        st.live_connections, st.max_connections, st.games_live, st.free_slots, st.users,
        (unsigned long)st.moves_applied, (unsigned long)st.games_reclaimed,
        (unsigned long)st.bot_decisions, (unsigned long)st.octogen_decisions, rl);
    // One clamp per line: two `if`s sharing a line read as a guard and its
    // body (-Wmisleading-indentation).
    if (n < 0) n = 0;
    if (n > (int)sizeof out) n = (int)sizeof out;
    respond_text(conn, 200, out, n);
}

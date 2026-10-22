// unit_gauges.c - the game server's CPU over a ten-second window (gauges.h), and the league's
// table sizes (league.h be_league_pick_size).
#include "t.h"
#include "gauges.h"
#include "league.h"

#include <string.h>

// A process using a steady quarter of a core reads 25.00% once the window holds two readings,
// and still does when one second of it was a burst: the window, not the last second, is the gauge.
static void cpu_window_is_ten_seconds(void) {
    be_cpu_window w;
    memset(&w, 0, sizeof w);
    uint64_t cpu = 5000000, at = 1000000000;
    T_EQ_INT(be_cpu_window_add(&w, cpu, at), -1);   // one reading: no rate yet
    for (int s = 1; s <= 10; s++) T_EQ_INT(be_cpu_window_add(&w, cpu += 250000, at += 1000000), 2500);
    // A burst: one second at a whole core. The last second alone would read 100.00%; the window
    // reads (9 x 0.25 + 1) / 10 = 32.50%.
    T_EQ_INT(be_cpu_window_add(&w, cpu += 1000000, at += 1000000), 3250);
    // Ten quiet seconds later the burst has left the window.
    int64_t v = 0;
    for (int s = 0; s < 10; s++) v = be_cpu_window_add(&w, cpu += 250000, at += 1000000);
    T_EQ_INT(v, 2500);
    // Another process behind the pid (CPU time went backwards): the window starts over.
    T_EQ_INT(be_cpu_window_add(&w, 1000, at += 1000000), -1);
    T_EQ_INT(be_cpu_window_add(&w, 1000 + 500000, at += 1000000), 5000);
}

static void sizes(void) {
    // Never more seats than free bots, never fewer than two, nothing at all under two free.
    T_EQ_INT(be_league_pick_size(0, 0), 0);
    T_EQ_INT(be_league_pick_size(1, 999), 0);
    for (uint32_t r = 0; r < 1000; r += 7) {
        T_EQ_INT(be_league_pick_size(2, r), 2);
        int n = be_league_pick_size(3, r);
        T_CHECK(n == 2 || n == 3);
    }
    // With every bot free, every size from 2 to 8 is drawn, two-player tables the most, and each
    // size at least 8% of tables.
    int count[9] = { 0 };
    for (uint32_t r = 0; r < 1000; r++) count[be_league_pick_size(49, r)]++;
    T_EQ_INT(count[0] + count[1], 0);
    for (int n = 2; n <= 8; n++) T_CHECK(count[n] >= 80);
    for (int n = 3; n <= 8; n++) T_CHECK(count[2] > count[n]);
    T_EQ_INT(count[2] + count[3] + count[4] + count[5] + count[6] + count[7] + count[8], 1000);
}

int main(void) {
    T_RUN(cpu_window_is_ten_seconds);
    T_RUN(sizes);
    return T_DONE();
}

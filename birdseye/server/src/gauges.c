// gauges.c - gauges.h.
#include "gauges.h"

#include "be_metrics.h"
#include "client.h"
#include "util/clock.h"
#include "world.h"
#include "../../../shared/c/proc.h"

int64_t be_cpu_window_add(be_cpu_window *w, uint64_t cpu_us, uint64_t at_us) {
    if (w->n > 0 && cpu_us < w->cpu_us[w->head]) w->n = 0;   // another process behind the pid
    w->head = (w->head + 1) % (BE_CPU_WINDOW + 1);
    w->cpu_us[w->head] = cpu_us;
    w->at_us[w->head] = at_us;
    if (w->n < BE_CPU_WINDOW + 1) w->n++;
    if (w->n < 2) return -1;
    int oldest = (w->head + BE_CPU_WINDOW + 2 - w->n) % (BE_CPU_WINDOW + 1);
    uint64_t span = at_us - w->at_us[oldest];
    if (span == 0) return -1;
    return (int64_t)((cpu_us - w->cpu_us[oldest]) * 10000 / span);
}

static be_cpu_window g_cpu;

void be_gauges_common(int server_pid) {
    be_world_lock(&be_world_g);
    uint32_t live = be_world_g.live, lobby = be_world_g.lobby, playing = be_world_g.playing;
    be_world_unlock(&be_world_g);
    cb_metrics_set(be_mx, BE_M_TABLES_LIVE, live);
    cb_metrics_set(be_mx, BE_M_TABLES_LOBBY, lobby);
    cb_metrics_set(be_mx, BE_M_TABLES_PLAYING, playing);
    cb_metrics_set(be_mx, BE_M_SOCKETS, be_client_sockets());
    if (!server_pid) return;
    cb_metrics_set(be_mx, BE_M_SERVER_RSS_MB, proc_rss_mb(server_pid));
    uint64_t cpu = proc_cpu_us(server_pid);
    if (!cpu) return;   // not readable (the server restarting): the last value stands
    int64_t cpct = be_cpu_window_add(&g_cpu, cpu, (uint64_t)cb_mono_us());
    if (cpct >= 0) cb_metrics_set(be_mx, BE_M_SERVER_CPU_CPCT, (uint64_t)cpct);
}

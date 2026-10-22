// unit_defs.c - birdseye's metrics table (be_metrics.h) is one the platform's registry takes:
// every name valid and unique, one row per enum value, each kind the enum's comment says.
#include "be_metrics.h"
#include "metrics.h"
#include "t.h"
#include "util/err.h"

static void table_is_a_registry(void) {
    cb_metrics *m = NULL;
    T_EQ_INT(cb_metrics_new(be_metric_defs, BE_M_N, &m), CB_OK);
    T_CHECK(m != NULL);
    if (!m) return;
    T_EQ_INT(cb_metrics_n(m), BE_M_N);
    for (int i = 0; i < BE_M_N; i++) {
        const cb_metric_def *d = cb_metrics_def(m, i);
        T_CHECK(d == &be_metric_defs[i]);
        T_CHECK(d->name && *d->name);
        for (int j = 0; j < i; j++) T_CHECK(strcmp(be_metric_defs[j].name, d->name) != 0);
    }
    // The kinds by band: histograms, rates, counters, gauges, in the enum's order.
    for (int i = BE_M_MOVE_ACK; i <= BE_M_GAME_S; i++) T_EQ_INT(be_metric_defs[i].kind, CB_METRIC_HIST);
    for (int i = BE_M_MOVES; i <= BE_M_REQS; i++) T_EQ_INT(be_metric_defs[i].kind, CB_METRIC_RATE);
    for (int i = BE_M_REFUSED; i <= BE_M_STALLS; i++) T_EQ_INT(be_metric_defs[i].kind, CB_METRIC_COUNTER);
    for (int i = BE_M_TABLES_LIVE; i <= BE_M_SERVER_CPU_CPCT; i++) T_EQ_INT(be_metric_defs[i].kind, CB_METRIC_GAUGE);
    // An update of the wrong kind is ignored, a right one lands, by index.
    cb_metrics_observe(m, BE_M_MOVE_ACK, 250);
    T_EQ_INT(cb_metrics_get(m, BE_M_MOVE_ACK), 1);
    cb_metrics_add(m, BE_M_MOVE_ACK, 5);
    T_EQ_INT(cb_metrics_get(m, BE_M_MOVE_ACK), 1);
    cb_metrics_set(m, BE_M_TABLES_LIVE, 7);
    T_EQ_INT(cb_metrics_get(m, BE_M_TABLES_LIVE), 7);
    cb_metrics_free(m);
}

int main(void) {
    T_RUN(table_is_a_registry);
    return T_DONE();
}

/* T23 instrument description regression: a 1 s probe (D3 bowed, then C2 bowed; body on, all four strings on the bridge,
 * 96 kHz) must be bit-identical to the one recorded BEFORE the Instrument refactor (string_params.c and body_params.c
 * constants moved into instrument_cello.c). The reference is an FNV-1a 64 bit hash over the raw bytes of all channels of
 * the trace, recorded from the pre-refactor code. */
#include "sim.h"
#include "test_common.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* RE-RECORDED deliberately when the instrument default became the diagnosis setting C1 (string Q at 2 kHz 500/500/300/300,
   bridge resistance x3, statistical masses x0.5): the physics of the default instrument changed, so every sample changes. The
   earlier pair (D3 3179987b8cae4f4b, C2 c4533b1282189180) was the bit-identical pre-refactor reference; the refactor itself is
   still proven by it in git history (commit before the C1 default). Below: the original comment.
   recorded from the pre-refactor code (string_params.c and body_params.c), built with -ffp-contract=off: with the default
   contraction the refactor (constants that were compile-time literals are now loaded from the Instrument) changes which
   multiply-adds the compiler fuses and moves the last bit of some samples; the Makefile now builds with -ffp-contract=off so
   the arithmetic does not depend on what the compiler can see as constant */
/* RE-RECORDED again in iteration 2 (a88857656984cdac / 58f68de34d67ee18 before): the bow noise acts on the sliding friction
   coefficient instead of the bow force and its level is 0.01 (was 0.03), and the torsional damping ratio is 0.05 (was 0.01). */
/* re-recorded in richness iteration 1 (deliberate): bow noise 16 kHz corner and level 0.02, body eta exponent 0.15 and mass exponent 0.3 change every probe */
#define REF_D3 0x03a48479d9794aecULL /* the friction curve model (BOW_CURVE): unchanged by the thermal work, which proves the curve path is untouched */
#define REF_C2 0x8460db965389ecd7ULL
/* RECORDED deliberately in thermal iteration 1, when the default bow model became BOW_THERMAL (friction_thermal.h): the physics of the
   default instrument changed, so every sample changes. Same probes as above, the default instrument. */
#define REF_D3_THERMAL 0x645e19137198416dULL
#define REF_C2_THERMAL 0x4e01f691d1a3dd7cULL
static uint64_t hash_trace(const Trace *tr)
{
    uint64_t h = 1469598103934665603ULL;
    for (int c = 0; c < SIM_NCH; c++) {
        const unsigned char *p = (const unsigned char *)tr->ch[c];
        for (size_t i = 0; i < sizeof(double) * (size_t)tr->n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
}

static uint64_t probe(const char *name, double F, BowModel model)
{
    Sim S;
    SimOpts so = {1, 1, 0, NULL};
    Instrument ic = *instrument_cello();
    ic.bow.model = model;
    sim_init_instrument(&S, &ic, 96000.0, &so, name);
    sim_set_bow(&S, 0.1, 0.1, F, 0.03);
    sim_start(&S);
    Trace *tr = sim_run(&S, 1.0, NULL, NULL);
    uint64_t h = hash_trace(tr);
    trace_free(tr);
    sim_free(&S);
    return h;
}

int main(void)
{
    CHECK(instrument_cello()->bow.model == BOW_THERMAL, "the default bow model is not THERMAL");
    uint64_t hd = probe("D3", 0.67082, BOW_CURVE), hc = probe("C2", 1.22127, BOW_CURVE);
    uint64_t td = probe("D3", 0.67082, BOW_THERMAL), tc = probe("C2", 1.22127, BOW_THERMAL);
    printf("T23 curve D3 probe hash %016llx, C2 probe hash %016llx; thermal D3 %016llx, C2 %016llx\n", (unsigned long long)hd, (unsigned long long)hc, (unsigned long long)td,
           (unsigned long long)tc);
    CHECK(td == REF_D3_THERMAL, "thermal D3 probe changed: %016llx, want %016llx", (unsigned long long)td, (unsigned long long)REF_D3_THERMAL);
    CHECK(tc == REF_C2_THERMAL, "thermal C2 probe changed: %016llx, want %016llx", (unsigned long long)tc, (unsigned long long)REF_C2_THERMAL);
    CHECK(hd == REF_D3, "D3 probe changed: %016llx, want %016llx", (unsigned long long)hd, (unsigned long long)REF_D3);
    CHECK(hc == REF_C2, "C2 probe changed: %016llx, want %016llx", (unsigned long long)hc, (unsigned long long)REF_C2);
    TEST_DONE("test_instrument");
}

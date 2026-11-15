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
#define REF_D3 0xa88857656984cdacULL
#define REF_C2 0x58f68de34d67ee18ULL

static uint64_t hash_trace(const Trace *tr)
{
    uint64_t h = 1469598103934665603ULL;
    for (int c = 0; c < SIM_NCH; c++) {
        const unsigned char *p = (const unsigned char *)tr->ch[c];
        for (size_t i = 0; i < sizeof(double) * (size_t)tr->n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
}

static uint64_t probe(const char *name, double F)
{
    Sim S;
    SimOpts so = {1, 1, 0, NULL};
    sim_init_instrument(&S, instrument_cello(), 96000.0, &so, name);
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
    uint64_t hd = probe("D3", 0.67082), hc = probe("C2", 1.22127);
    printf("T23 D3 probe hash %016llx, C2 probe hash %016llx\n", (unsigned long long)hd, (unsigned long long)hc);
    CHECK(hd == REF_D3, "D3 probe changed: %016llx, want %016llx", (unsigned long long)hd, (unsigned long long)REF_D3);
    CHECK(hc == REF_C2, "C2 probe changed: %016llx, want %016llx", (unsigned long long)hc, (unsigned long long)REF_C2);
    TEST_DONE("test_instrument");
}

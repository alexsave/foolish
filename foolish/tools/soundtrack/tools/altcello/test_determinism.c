/* T6 determinism: the same seed gives bit-identical audio; the bow noise draw
 * sequence (logical times and values) is the same at 48 kHz and 96 kHz. */
#include "sim.h"
#include "test_common.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static Trace *run(double fs, BowNoise *noise_out)
{
    Sim S;
    sim_init(&S, instrument_cello(), instrument_string(instrument_cello(), "D3"), fs);
    sim_set_bow(&S, 0.1, 0.1, 0.4, 0.03);
    sim_start(&S);
    Trace *tr = sim_run(&S, 0.3, NULL, NULL);
    if (noise_out) *noise_out = S.noise;
    sim_free(&S);
    return tr;
}

int main(void)
{
    Trace *a = run(96000.0, NULL), *b = run(96000.0, NULL);
    int same = 1;
    for (int c = 0; c < SIM_NCH; c++) same &= memcmp(a->ch[c], b->ch[c], sizeof(double) * (size_t)a->n) == 0;
    CHECK(same, "two runs with the same seed differ");
    double rms = 0;
    for (long i = 0; i < a->n; i++) rms += a->ch[0][i] * a->ch[0][i];
    CHECK(rms > 0, "the run is silent (determinism test would be vacuous)");
    printf("T6 two 96 kHz runs bit-identical over %ld samples x %d channels: %s\n", a->n, SIM_NCH, same ? "yes" : "NO");
    trace_free(a);
    trace_free(b);

    BowNoise n96, n48;
    Trace *c = run(96000.0, &n96), *d = run(48000.0, &n48);
    CHECK(n96.nlog == 1000 && n48.nlog == 1000, "draw log sizes %ld %ld", n96.nlog, n48.nlog);
    int ident = 1;
    for (long i = 0; i < 1000; i++)
        if (n96.log_t[i] != n48.log_t[i] || n96.log_x[i] != n48.log_x[i]) { /* the made-at time may differ by a sample (checked below) */
            ident = 0;
            CHECK(0, "draw %ld differs: t %.9f/%.9f x %.9f/%.9f", i, n96.log_t[i], n48.log_t[i], n96.log_x[i], n48.log_x[i]);
            break;
        }
    /* draw j (j >= 1) is made at the first sample at or after (j - 1) / rate s (the rate 64000 is not a whole number of 96 or 48 kHz samples, so up to one 48 kHz
       sample late); draws 0 and 1 at 0 */
    for (long i = 0; i < 1000; i++) {
        double want = i >= 1 ? (double)(i - 1) / (double)n96.rate : 0.0;
        if (n96.log_now[i] < want - 1e-9 || n96.log_now[i] - want > 1.0 / 96000.0 + 1e-9 || n48.log_now[i] < want - 1e-9 || n48.log_now[i] - want > 1.0 / 48000.0 + 1e-9) {
            CHECK(0, "draw %ld made at %.9f s (96k) %.9f s (48k), want %.9f s", i, n96.log_now[i], n48.log_now[i], want);
            break;
        }
    }
    printf("T6 first 1000 bow-noise draws (t from %.6f s to %.6f s) identical at 48 kHz and 96 kHz: %s\n", n96.log_t[0], n96.log_t[999], ident ? "yes" : "NO");
    CHECK(fabs(n96.log_t[999] - 999.0 / (double)n96.rate) < 1e-12, "logical time of draw 999 is %.9f", n96.log_t[999]);
    trace_free(c);
    trace_free(d);
    TEST_DONE("test_determinism");
}

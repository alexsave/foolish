// playlist: 20 s of each city's loop in ladder order, each excerpt at -16 LUFS (true peak kept under -1 dBTP),
// 0.3 s fade in, 1.5 s fade out, 1.5 s of silence between. usage: playlist out.wav loop1.wav start1 [loop2.wav start2 ...]
#include "mix.h"
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: playlist out.wav a.wav start ...\n"); return 2; }
    long cap = SR * 30 * ((argc - 2) / 2) + SR; float *OL = calloc(cap, sizeof(float)), *OR = calloc(cap, sizeof(float)); long pos = 0;
    for (int a = 2; a + 1 < argc; a += 2) {
        float *L, *R; long n = wav_read(argv[a], &L, &R); long s0 = (long)(atof(argv[a + 1]) * SR), len = 20 * SR;
        float *xl = malloc(len * sizeof(float)), *xr = malloc(len * sizeof(float));
        for (long i = 0; i < len; i++) { long j = (s0 + i) % n; xl[i] = L[j]; xr[i] = R[j]; }   // loops are seamless, so wrap
        double I = lufs(xl, xr, len, 0, 0, 0), g = db2a(-16 - I), tp = true_peak(xl, xr, len) * g;
        if (tp > db2a(-1)) g *= db2a(-1) / tp;
        for (long i = 0; i < len; i++) { double t = (double)i / SR, f = fmin(1, t / 0.3) * fmin(1, (20 - t) / 1.5); OL[pos + i] = (float)(xl[i] * g * f); OR[pos + i] = (float)(xr[i] * g * f); }
        fprintf(stderr, "%-40s from %5.1f s: excerpt %.1f LUFS, gain %+.1f dB\n", argv[a], atof(argv[a + 1]), I, a2db(g));
        pos += len + (long)(1.5 * SR); free(L); free(R); free(xl); free(xr);
    }
    wav_write24(argv[1], OL, OR, pos);
    fprintf(stderr, "wrote %s, %.1f s\n", argv[1], (double)pos / SR);
    return 0;
}

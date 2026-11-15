#include "wav.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

int wav_write_f32(const char *path, const float *x, long n, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    unsigned char h[58];
    memcpy(h, "RIFF", 4);
    put32(h + 4, (uint32_t)(50 + 4 * n));
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 18);
    put16(h + 20, 3); /* IEEE float */
    put16(h + 22, 1);
    put32(h + 24, (uint32_t)rate);
    put32(h + 28, (uint32_t)(rate * 4));
    put16(h + 32, 4);
    put16(h + 34, 32);
    put16(h + 36, 0); /* cbSize */
    memcpy(h + 38, "fact", 4);
    put32(h + 42, 4);
    put32(h + 46, (uint32_t)n);
    memcpy(h + 50, "data", 4);
    put32(h + 54, (uint32_t)(4 * n));
    fwrite(h, 1, 58, f);
    fwrite(x, sizeof(float), (size_t)n, f);
    fclose(f);
    return 0;
}

long wav_decimate(const double *x, long n, int factor, float **out)
{
    const int taps = 511, half = taps / 2;
    double h[511];
    double fc = 0.45 / factor; /* cycles per input sample */
    double sum = 0;
    for (int i = 0; i < taps; i++) {
        int d = i - half;
        double s = d == 0 ? 2.0 * fc : sin(2.0 * M_PI * fc * d) / (M_PI * d);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (taps - 1)) + 0.08 * cos(4.0 * M_PI * i / (taps - 1));
        h[i] = s * w;
        sum += h[i];
    }
    for (int i = 0; i < taps; i++) h[i] /= sum;
    long m = n / factor;
    float *y = malloc(sizeof(float) * (size_t)m);
    for (long j = 0; j < m; j++) {
        long c = j * factor;
        double a = 0;
        for (int i = 0; i < taps; i++) {
            long idx = c + i - half;
            if (idx >= 0 && idx < n) a += h[i] * x[idx];
        }
        y[j] = (float)a;
    }
    *out = y;
    return m;
}

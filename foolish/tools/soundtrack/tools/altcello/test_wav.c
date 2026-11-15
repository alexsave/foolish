/* wav writer and the anti-alias decimator: the header fields read back, a 5 kHz
 * tone survives the 96 -> 48 kHz decimation (within 0.2 dB), a 30 kHz tone that
 * would alias to 18 kHz is removed (more than 60 dB down). */
#include "test_common.h"
#include "wav.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double rms(const float *y, long a, long b)
{
    double s = 0;
    for (long i = a; i < b; i++) s += (double)y[i] * y[i];
    return sqrt(s / (b - a));
}

int main(void)
{
    long n = 96000;
    double *x = malloc(sizeof(double) * (size_t)n);
    float *y;
    for (long i = 0; i < n; i++) x[i] = sin(2 * M_PI * 5000.0 * i / 96000.0);
    long m = wav_decimate(x, n, 2, &y);
    double pass = 20 * log10(rms(y, 2000, m - 2000) / (1.0 / sqrt(2.0)));
    printf("wav: 5 kHz tone through the decimator: %+.3f dB\n", pass);
    CHECK(m == n / 2, "decimated length %ld", m);
    CHECK(fabs(pass) < 0.2, "5 kHz passband gain %.3f dB", pass);
    free(y);
    for (long i = 0; i < n; i++) x[i] = sin(2 * M_PI * 30000.0 * i / 96000.0);
    m = wav_decimate(x, n, 2, &y);
    double rej = 20 * log10(rms(y, 2000, m - 2000) / (1.0 / sqrt(2.0)));
    printf("wav: 30 kHz tone (aliases to 18 kHz) through the decimator: %+.1f dB\n", rej);
    CHECK(rej < -60.0, "30 kHz tone only %.1f dB down", rej);

    float s[4] = {0.5f, -0.25f, 0.0f, 1.0f};
    const char *path = "out/test_wav.wav";
    CHECK(wav_write_f32(path, s, 4, 48000) == 0, "write failed");
    FILE *f = fopen(path, "rb");
    unsigned char h[58];
    float back[4];
    CHECK(f && fread(h, 1, 58, f) == 58 && fread(back, sizeof(float), 4, f) == 4, "read back failed");
    if (f) {
        fclose(f);
        uint32_t rate = (uint32_t)h[24] | (uint32_t)h[25] << 8 | (uint32_t)h[26] << 16;
        CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 50, "data", 4), "chunk ids");
        CHECK(h[20] == 3 && h[22] == 1 && h[34] == 32, "format %d channels %d bits %d", h[20], h[22], h[34]);
        CHECK(rate == 48000, "rate %u", rate);
        CHECK(h[54] == 16 && h[4] == 66, "sizes data %d riff %d", h[54], h[4]);
        CHECK(!memcmp(back, s, sizeof s), "samples differ");
    }
    free(x);
    free(y);
    TEST_DONE("test_wav");
}

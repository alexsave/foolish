/* altcello room: apply the shoebox room and microphone (room.h) to a mono wav (48 kHz). The input file is only read.
 * Writes OUT.wav (float32 mono 48 kHz), OUT_ir.f32 (raw float32 impulse response) and OUT_ir.txt (text header with the geometry
 * and the measured numbers), prints the measured RT60 per octave band and the direct-to-reverberant ratio. */
#include "room_cli.h"
#include "room.h"
#include "wav.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* mono (first channel) of a PCM16/24/32 or float32 RIFF wav; returns samples, 0 on failure */
static long wav_read_mono(const char *path, float **out, int *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)sz);
    if (!b || (long)fread(b, 1, (size_t)sz, f) != sz) { fclose(f); free(b); return 0; }
    fclose(f);
    if (sz < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return 0; }
    int fmt = 0, ch = 1, bits = 16;
    long dpos = -1, dlen = 0;
    for (long p = 12; p + 8 <= sz;) {
        uint32_t len = rd32(b + p + 4);
        if (!memcmp(b + p, "fmt ", 4)) { fmt = rd16(b + p + 8); ch = rd16(b + p + 10); *rate = (int)rd32(b + p + 12); bits = rd16(b + p + 22); }
        if (!memcmp(b + p, "data", 4)) { dpos = p + 8; dlen = len; if (dpos + dlen > sz) dlen = sz - dpos; break; }
        p += 8 + len + (len & 1);
    }
    if (dpos < 0 || ch < 1) { free(b); return 0; }
    long fr = dlen / (ch * bits / 8);
    float *x = malloc(sizeof(float) * (size_t)fr);
    for (long i = 0; i < fr; i++) {
        const unsigned char *q = b + dpos + (long)i * ch * bits / 8;
        if (fmt == 3 && bits == 32) { float v; memcpy(&v, q, 4); x[i] = v; }
        else if (bits == 16) x[i] = (float)((int16_t)rd16(q) / 32768.0);
        else if (bits == 24) x[i] = (float)(((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24)) / 2147483648.0);
        else if (bits == 32) x[i] = (float)((int32_t)rd32(q) / 2147483648.0);
        else { free(b); free(x); return 0; }
    }
    free(b);
    *out = x;
    return fr;
}

int room_cli(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: altcello room IN.wav OUT.wav [--rt60 S] [--dist M] [--pattern omni|cardioid] [--mic X,Y,Z] [--no-proximity] [--drr-check] [--no-noise]\n");
        return 2;
    }
    double rt60 = 0.8, dist = 1.5;
    int pattern = ROOM_CARDIOID, drrcheck = 0, noise = 1, prox = 1, havemic = 0;
    double micp[3] = {0, 0, 0};
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--rt60") && i + 1 < argc) rt60 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dist") && i + 1 < argc) dist = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pattern") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "omni")) pattern = ROOM_OMNI;
            else if (!strcmp(p, "cardioid")) pattern = ROOM_CARDIOID;
            else { fprintf(stderr, "unknown pattern %s\n", p); return 2; }
        } else if (!strcmp(argv[i], "--drr-check")) drrcheck = 1;
        else if (!strcmp(argv[i], "--no-noise")) noise = 0;
        else if (!strcmp(argv[i], "--no-proximity")) prox = 0;
        else if (!strcmp(argv[i], "--mic") && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf,%lf,%lf", &micp[0], &micp[1], &micp[2]) != 3) { fprintf(stderr, "--mic wants X,Y,Z in metres\n"); return 2; }
            havemic = 1;
        }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    float *in;
    int rate = 0;
    long nin = wav_read_mono(argv[2], &in, &rate);
    if (nin <= 0) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    if (rate != 48000) { fprintf(stderr, "%s is %d Hz, the room runs at 48000 Hz\n", argv[2], rate); return 1; }
    RoomSpec s;
    room_spec_default(&s, dist);
    s.rt60 = rt60;
    s.pattern = pattern;
    s.proximity = prox;
    if (havemic) room_spec_set_mic(&s, micp[0], micp[1], micp[2]);
    dist = sqrt(pow(s.src[0] - s.mic[0], 2) + pow(s.src[1] - s.mic[1], 2) + pow(s.src[2] - s.mic[2], 2));
    if (!noise) s.self_noise_dba = 0.0;
    float *ir, *out;
    long nir;
    if (room_ir_build(&s, &ir, &nir)) { fprintf(stderr, "ir build failed\n"); return 1; }
    long nout = room_apply(&s, ir, nir, in, nin, &out);
    if (nout <= 0) { fprintf(stderr, "apply failed\n"); return 1; }
    if (wav_write_f32(argv[3], out, nout, 48000)) { perror(argv[3]); return 1; }
    char path[1024];
    snprintf(path, sizeof path, "%s_ir.f32", argv[3]);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fwrite(ir, sizeof(float), (size_t)nir, f);
    fclose(f);
    static const double FC[7] = {125, 250, 500, 1000, 2000, 4000, 8000};
    double rt[7], dd = room_direct_time(&s), drr = room_drr(ir, nir, s.fs, dd);
    for (int i = 0; i < 7; i++) rt[i] = room_rt60(ir, nir, s.fs, FC[i], NULL);
    double all = room_rt60(ir, nir, s.fs, 0.0, NULL);
    snprintf(path, sizeof path, "%s_ir.txt", argv[3]);
    f = fopen(path, "w");
    fprintf(f, "fs=%.0f n=%ld rt60_target=%.3f dist=%.3f pattern=%s drr_db=%.2f direct_s=%.6f crossover_s=%.5f\n", s.fs, nir, rt60, dist,
            pattern == ROOM_OMNI ? "omni" : "cardioid", drr, dd, room_crossover_time(&s));
    fprintf(f, "room=%.2f,%.2f,%.2f src=%.4f,%.4f,%.4f mic=%.4f,%.4f,%.4f mic_axis=%.4f,%.4f,%.4f front=%.1f,%.1f,%.1f\n", s.dim[0], s.dim[1],
            s.dim[2], s.src[0], s.src[1], s.src[2], s.mic[0], s.mic[1], s.mic[2], s.mic_axis[0], s.mic_axis[1], s.mic_axis[2], s.front[0],
            s.front[1], s.front[2]);
    fprintf(f, "rt60_band_hz=125,250,500,1000,2000,4000,8000\nrt60_measured=");
    for (int i = 0; i < 7; i++) fprintf(f, "%s%.3f", i ? "," : "", rt[i]);
    fprintf(f, "\nrt60_broadband=%.3f\n", all);
    fclose(f);
    printf("room: %ld samples in, %ld out, ir %ld samples (%.2f s), target RT60 %.2f s (mid; low x%.1f, high x%.1f), mic %.2f m %s\n", nin, nout, nir,
           (double)nir / s.fs, rt60, s.rt60_mult[0], s.rt60_mult[2], dist, pattern == ROOM_OMNI ? "omni" : "cardioid");
    printf("RT60 (T20 x 3, Schroeder) per octave band:");
    for (int i = 0; i < 7; i++) printf("  %.0f Hz %.3f s", FC[i], rt[i]);
    printf("\nDRR %.2f dB (direct sound at %.2f ms)\n", drr, dd * 1000.0);
    if (drrcheck) {
        double V = s.dim[0] * s.dim[1] * s.dim[2], S = 2.0 * (s.dim[0] * s.dim[1] + s.dim[0] * s.dim[2] + s.dim[1] * s.dim[2]);
        double al = room_wall_alpha(&s, rt60), R = -S * log(1.0 - al); /* Eyring room constant */
        double r = sqrt(pow(s.src[0] - s.mic[0], 2) + pow(s.src[1] - s.mic[1], 2) + pow(s.src[2] - s.mic[2], 2));
        double th = 10.0 * log10(R / (16.0 * M_PI * r * r));
        printf("DRR check: diffuse-field theory (omni source and mic, wall alpha %.3f, room constant %.1f m2, V %.1f m3, r %.2f m): %.2f dB, critical distance %.2f m\n",
               al, R, V, r, th, 0.25 * sqrt(R / M_PI));
        printf("DRR check: measured %.2f dB, difference %+.2f dB (the cardioid mic lowers the diffuse part by 4.8 dB, so +4.8 dB expected with it)\n", drr, drr - th);
    }
    free(in); free(ir); free(out);
    return 0;
}

// Shared harness for the C-side POC benchmarks: portable state loading,
// FNV-1a checksumming (must match rs/src/lib.rs exactly), monotonic timing,
// and peak-RSS reporting from /proc/self/status.
#ifndef RUSTPOC_BENCH_COMMON_H
#define RUSTPOC_BENCH_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint8_t num_players, power_suit, defender, first_attacker;
    uint8_t status, num_battles, actor, has_flipped, flipped_id, num_eliminated;
    uint32_t good_mask;
    uint16_t discard_len, deck_count;
    uint8_t elim[8];
    uint8_t deck[64];
    uint8_t atk[64];
    uint8_t def[64]; // 255 = uncovered
    uint8_t pstatus[8];
    uint8_t hand_count[8];
    uint8_t hand[8][64];
} PocState;

static uint64_t fnv1a(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t fnv1a_u32(uint64_t h, uint32_t v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return fnv1a(h, b, 4);
}
#define FNV_INIT 1469598103934665603ull

static const unsigned char *rd_bytes(const unsigned char *p, void *dst, size_t n) {
    memcpy(dst, p, n);
    return p + n;
}

// Returns malloc'd array of states; sets *out_n.
static PocState *load_states(const char *path, unsigned *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f);
    const unsigned char *p = buf;
    uint32_t magic, version, count;
    p = rd_bytes(p, &magic, 4); p = rd_bytes(p, &version, 4); p = rd_bytes(p, &count, 4);
    if (magic != 0x434F5046u || version != 1) { fprintf(stderr, "bad states file\n"); exit(1); }
    PocState *st = (PocState *)calloc(count, sizeof(PocState));
    for (uint32_t i = 0; i < count; i++) {
        PocState *s = &st[i];
        s->num_players = *p++; s->power_suit = *p++; s->defender = *p++;
        s->first_attacker = *p++; s->status = *p++; s->num_battles = *p++;
        s->actor = *p++; s->has_flipped = *p++; s->flipped_id = *p++;
        s->num_eliminated = *p++;
        s->good_mask = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); p += 4;
        s->discard_len = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        s->deck_count  = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
        for (int j = 0; j < s->num_eliminated; j++) s->elim[j] = *p++;
        for (int j = 0; j < s->deck_count; j++) s->deck[j] = *p++;
        for (int j = 0; j < s->num_battles; j++) { s->atk[j] = *p++; s->def[j] = *p++; }
        for (int pl = 0; pl < s->num_players; pl++) {
            s->pstatus[pl] = *p++;
            s->hand_count[pl] = *p++;
            for (int j = 0; j < s->hand_count[pl]; j++) s->hand[pl][j] = *p++;
        }
    }
    if (p - buf != sz) { fprintf(stderr, "trailing bytes: parsed %ld of %ld\n", (long)(p - buf), sz); exit(1); }
    free(buf);
    *out_n = count;
    return st;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static long peak_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) { kb = atol(line + 6); break; }
    }
    fclose(f);
    return kb;
}

#endif

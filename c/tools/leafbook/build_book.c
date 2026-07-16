// LEAFBOOK builder (docs/L1_SPEND_PLAN.md §4, step 2).
//
// Enumerates every distinct canonical round-boundary <=K-card 2-player
// deck-empty endgame (both hands non-empty — hand-empty forms are terminal and
// never probed), reconstructs the canonical representative, solves it EXACTLY
// with the bitboard solver at a huge budget, and emits src/leafbook_data.h:
// a key-sorted array of (canonical key -> packed value byte). A book hit at a
// round boundary then terminates the whole subtree with this proven value.
//
// Value byte: bits[5:4] outcome {0=loss,1=draw,2=win} (attacker-to-move's
// perspective), bits[3:0] distance in plies to resolution (<=15). Draw => dist 0.
//
//   cc -O2 -Isrc $(make -s print-core) tools/leafbook/build_book.c \
//      -o build/leafbook_build -lm -DCD_TT_BITS=21
//   ./build/leafbook_build > src/leafbook_data.h
#include "leafbook.h"
#include "cordite_sim.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LEAFBOOK_BUILD_K
#define LEAFBOOK_BUILD_K LEAFBOOK_K
#endif
#define KMAX LEAFBOOK_BUILD_K
#define NCELL (4 * KMAX)

// ---- distinct canonical key collection (same DFS as enumerate.c) ----
#define HBITS 23
#define HSIZE (1u << HBITS)
#define HMASK (HSIZE - 1u)
static uint64_t *hkey;
static uint64_t *keys;      // collected distinct keys (both-nonempty)
static long nkeys = 0, keys_cap = 0;

static void collect(uint64_t key) {
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - HBITS)) & HMASK;
    for (;;) {
        if (hkey[i] == 0) {
            hkey[i] = key;
            if (nkeys == keys_cap) { keys_cap = keys_cap ? keys_cap * 2 : 4096;
                                     keys = realloc(keys, keys_cap * sizeof(uint64_t)); }
            keys[nkeys++] = key;
            return;
        }
        if (hkey[i] == key) return;
        i = (i + 1) & HMASK;
    }
}
static void dfs(int cell, int ncards, uint64_t HA, uint64_t HD) {
    if (cell == NCELL) {
        if (HA && HD) collect(leafbook_key(HA, HD, 3));   // both non-empty only
        return;
    }
    int su = cell / KMAX, r = cell % KMAX;
    uint64_t bit = 1ull << (su * 13 + r);
    dfs(cell + 1, ncards, HA, HD);
    if (ncards < KMAX) {
        dfs(cell + 1, ncards + 1, HA | bit, HD);
        dfs(cell + 1, ncards + 1, HA, HD | bit);
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

// Decode a canonical key back to its representative 52-bit hands (trump=suit 3,
// compacted ranks used directly as card ranks 0..d-1).
static void decode_key(uint64_t key, uint64_t *HA, uint64_t *HD) {
    uint32_t ha24 = (uint32_t)(key >> 24) & 0xFFFFFFu, hd24 = (uint32_t)key & 0xFFFFFFu;
    uint64_t a = 0, d = 0;
    for (int su = 0; su < 4; su++) {
        uint32_t ba = (ha24 >> (su * 6)) & 0x3Fu, bd = (hd24 >> (su * 6)) & 0x3Fu;
        for (int r = 0; r < 6; r++) {
            if (ba & (1u << r)) a |= 1ull << (su * 13 + r);
            if (bd & (1u << r)) d |= 1ull << (su * 13 + r);
        }
    }
    *HA = a; *HD = d;
}

// Build a 2-player deck-empty round-boundary SimState: attacker = player 0
// (hand HA), defender = player 1 (hand HD), trump = power.
void leafbook_make_state(SimState *s, uint64_t HA, uint64_t HD, int power) {
    memset(s, 0, sizeof(*s));
    s->num_players = 2;
    s->power_suit = (uint8_t)power;
    s->defender = 1;
    s->first_attacker = 0;
    s->status = GAME_STATUS_PLAYING;
    s->num_battles = 0;
    s->deck_count = 0;
    s->deck_n = 0;
    s->good_mask = 0;
    s->hand[0] = HA;
    s->hand[1] = HD;
    s->status_p[0] = PLAYER_STATUS_IN;
    s->status_p[1] = PLAYER_STATUS_IN;
    s->in_mask = 0x3u;
    s->out_mask = 0;
    s->covered_mask = 0;
    s->table_vmask = 0;
}

// Solve the endgame from the attacker's (player 0's) perspective; return the
// packed value byte. Sets *ab if the solve aborted (should never for <=K<=6).
uint8_t leafbook_solve_byte(uint64_t HA, uint64_t HD, int power, int *ab) {
    // Fresh TT per solve: the persistent table is only collision-safe when kept
    // small (reset per decision in production). Hammering thousands of
    // independent solves into one table saturates it and pollutes values.
    cd_sim_solve_reset();
    SimState s;
    leafbook_make_state(&s, HA, HD, power);
    int aborted = 0;
    int v = cd_sim_solve(&s, /*me=attacker*/0, -1001, 1001, 100000000L, &aborted);
    *ab = aborted;
    int outcome, dist;
    if (v > 0)      { outcome = 2; dist = 1000 - v; }        // win
    else if (v < 0) { outcome = 0; dist = 1000 + v; }        // loss (v = -(1000-dist))
    else            { outcome = 1; dist = 0; }               // draw
    if (dist < 0) dist = 0;
    if (dist > 15) dist = 15;
    return (uint8_t)((outcome << 4) | dist);
}

// ---- CHD minimal-perfect-hash construction ------------------------------
// Place N keys into a minimal (R=N) value array: bucket by lb_bucket, then for
// each bucket (largest first) find a displacement whose lb_slot maps all its
// keys to distinct free slots. Retries with a fresh global seed on failure.
#define LB_DISP_MAX 60000            // fits uint16; overflow => reseed

static int cmp_bucket_size(const void *pa, const void *pb, void *ctx) {
    const int *sz = (const int *)ctx;
    return sz[*(const int *)pb] - sz[*(const int *)pa];   // descending
}
// portable desc sort of bucket ids by size (no qsort_r dependency)
static int *g_bsz;
static int cmp_bsz(const void *a, const void *b) {
    return g_bsz[*(const int *)b] - g_bsz[*(const int *)a];
}

int main(void) {
    // Trigger cordite_sim's lazy VALUE/SUIT/HIGHER mask init (only fired inside
    // cd_sim_from_game) before we solve hand-built states directly.
    { Game dummy; SimState ds; memset(&dummy, 0, sizeof(dummy)); cd_sim_from_game(&ds, &dummy); }

    hkey = calloc(HSIZE, sizeof(uint64_t));
    if (!hkey) { fprintf(stderr, "oom\n"); return 1; }
    dfs(0, 0, 0, 0);
    qsort(keys, nkeys, sizeof(uint64_t), cmp_u64);   // deterministic key order

    uint8_t *val_of = malloc(nkeys);
    long aborts = 0, wins = 0, losses = 0, draws = 0, maxdist = 0;
    for (long i = 0; i < nkeys; i++) {
        uint64_t HA, HD; decode_key(keys[i], &HA, &HD);
        int ab = 0;
        val_of[i] = leafbook_solve_byte(HA, HD, 3, &ab);
        if (ab) { aborts++; fprintf(stderr, "ABORT at key %llx\n", (unsigned long long)keys[i]); }
        int o = val_of[i] >> 4, d = val_of[i] & 15;
        if (o == 2) wins++; else if (o == 0) losses++; else draws++;
        if (d > maxdist) maxdist = d;
    }
    fprintf(stderr, "leafbook: K=%d  entries=%ld  wins=%ld losses=%ld draws=%ld  maxdist=%ld  aborts=%ld\n",
            KMAX, nkeys, wins, losses, draws, maxdist, aborts);
    if (aborts) { fprintf(stderr, "FATAL: solves aborted; book would be unsound\n"); return 2; }
    (void)cmp_bucket_size;

    uint32_t N = (uint32_t)nkeys, R = N;
    uint32_t M = (N + 4) / 5;                 // ~5 keys/bucket (CHD sweet spot)
    uint16_t *disp = malloc((size_t)M * sizeof(uint16_t));
    uint8_t  *vals = malloc(R);
    uint8_t  *occ  = malloc(R);
    // bucket membership (CSR): head[M], nxt[N]
    int *head = malloc((size_t)M * sizeof(int));
    int *nxt  = malloc((size_t)N * sizeof(int));
    int *bsz  = malloc((size_t)M * sizeof(int));
    int *order = malloc((size_t)M * sizeof(int));
    uint64_t seed = 0x1234567;
    int ok = 0;
    for (int attempt = 0; attempt < 200 && !ok; attempt++) {
        if (attempt) seed = lb_mix(seed) | 1;   // advance only after a failed attempt
        for (uint32_t b = 0; b < M; b++) { head[b] = -1; bsz[b] = 0; }
        for (uint32_t i = 0; i < N; i++) {
            uint32_t b = lb_bucket(keys[i], M, seed);
            nxt[i] = head[b]; head[b] = (int)i; bsz[b]++;
        }
        for (uint32_t b = 0; b < M; b++) order[b] = (int)b;
        g_bsz = bsz; qsort(order, M, sizeof(int), cmp_bsz);
        memset(occ, 0, R);
        ok = 1;
        for (uint32_t oi = 0; oi < M && ok; oi++) {
            int b = order[oi];
            if (bsz[b] == 0) { disp[b] = 0; continue; }
            int placed = 0;
            for (uint32_t d = 0; d <= LB_DISP_MAX && !placed; d++) {
                int good = 1;
                // check all keys land distinct + free
                uint32_t tmp[64]; int tn = 0;
                for (int i = head[b]; i >= 0; i = nxt[i]) {
                    uint32_t s = lb_slot(keys[i], d, R, seed);
                    if (occ[s]) { good = 0; break; }
                    int dup = 0; for (int t = 0; t < tn; t++) if (tmp[t] == s) { dup = 1; break; }
                    if (dup) { good = 0; break; }
                    if (tn < 64) tmp[tn++] = s;
                }
                if (good) {
                    for (int i = head[b]; i >= 0; i = nxt[i]) {
                        uint32_t s = lb_slot(keys[i], d, R, seed);
                        occ[s] = 1; vals[s] = val_of[i];
                    }
                    disp[b] = (uint16_t)d; placed = 1;
                }
            }
            if (!placed) ok = 0;
        }
        if (!ok) fprintf(stderr, "  CHD attempt %d failed; reseeding\n", attempt);
    }
    if (!ok) { fprintf(stderr, "FATAL: CHD construction failed\n"); return 3; }

    // Build-time self-check: the MPH must be a bijection reproducing every value.
    for (uint32_t i = 0; i < N; i++) {
        uint32_t s = lb_slot(keys[i], disp[lb_bucket(keys[i], M, seed)], R, seed);
        if (vals[s] != val_of[i]) { fprintf(stderr, "FATAL: MPH self-check failed at %u\n", i); return 4; }
    }
    long disp_bytes = (long)M * 2, val_bytes = R;
    fprintf(stderr, "leafbook MPH: N=%u M=%u R=%u seed=0x%llx  size=%ld B (disp %ld + vals %ld) = %.1f KiB\n",
            N, M, R, (unsigned long long)seed, disp_bytes + val_bytes, disp_bytes, val_bytes,
            (disp_bytes + val_bytes) / 1024.0);

    // Emit the generated header (value array + displacements; NO keys stored).
    printf("// GENERATED by tools/leafbook/build_book.c — do not edit.\n");
    printf("// LEAFBOOK reach K=%d; docs/L1_SPEND_PLAN.md §4. CHD minimal perfect hash.\n", KMAX);
    printf("#ifndef CNITRO_LEAFBOOK_DATA_H\n#define CNITRO_LEAFBOOK_DATA_H\n#include <stdint.h>\n\n");
    printf("#define LEAFBOOK_DATA_K %d\n", KMAX);
    printf("#define LEAFBOOK_N %ldL\n", nkeys);
    printf("#define LEAFBOOK_M %uu\n", M);
    printf("#define LEAFBOOK_R %uu\n", R);
    printf("#define LEAFBOOK_SEED 0x%llxull\n\n", (unsigned long long)seed);
    printf("static const uint16_t leafbook_disp[LEAFBOOK_M] = {\n");
    for (uint32_t i = 0; i < M; i++) { printf("%u,", disp[i]); if ((i & 15) == 15) printf("\n"); }
    printf("\n};\n\n");
    printf("static const uint8_t leafbook_vals[LEAFBOOK_R] = {\n");
    for (uint32_t i = 0; i < R; i++) { printf("%u,", vals[i]); if ((i & 31) == 31) printf("\n"); }
    printf("\n};\n\n#endif\n");
    return 0;
}

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

int main(void) {
    // Trigger cordite_sim's lazy VALUE/SUIT/HIGHER mask init (only fired inside
    // cd_sim_from_game) before we solve hand-built states directly.
    { Game dummy; SimState ds; memset(&dummy, 0, sizeof(dummy)); cd_sim_from_game(&ds, &dummy); }

    hkey = calloc(HSIZE, sizeof(uint64_t));
    if (!hkey) { fprintf(stderr, "oom\n"); return 1; }
    dfs(0, 0, 0, 0);
    qsort(keys, nkeys, sizeof(uint64_t), cmp_u64);

    uint8_t *vals = malloc(nkeys);
    long aborts = 0, wins = 0, losses = 0, draws = 0, maxdist = 0;
    for (long i = 0; i < nkeys; i++) {
        uint64_t HA, HD; decode_key(keys[i], &HA, &HD);
        int ab = 0;
        vals[i] = leafbook_solve_byte(HA, HD, 3, &ab);
        if (ab) { aborts++; fprintf(stderr, "ABORT at key %llx\n", (unsigned long long)keys[i]); }
        int o = vals[i] >> 4, d = vals[i] & 15;
        if (o == 2) wins++; else if (o == 0) losses++; else draws++;
        if (d > maxdist) maxdist = d;
    }
    fprintf(stderr, "leafbook: K=%d  entries=%ld  wins=%ld losses=%ld draws=%ld  maxdist=%ld  aborts=%ld\n",
            KMAX, nkeys, wins, losses, draws, maxdist, aborts);
    if (aborts) { fprintf(stderr, "FATAL: solves aborted; book would be unsound\n"); return 2; }

    // Emit the generated header.
    printf("// GENERATED by tools/leafbook/build_book.c — do not edit.\n");
    printf("// LEAFBOOK reach K=%d; docs/L1_SPEND_PLAN.md §4. Key-sorted for binary search.\n", KMAX);
    printf("#ifndef CNITRO_LEAFBOOK_DATA_H\n#define CNITRO_LEAFBOOK_DATA_H\n#include <stdint.h>\n\n");
    printf("#define LEAFBOOK_DATA_K %d\n", KMAX);
    printf("#define LEAFBOOK_N %ldL\n\n", nkeys);
    printf("static const uint64_t leafbook_keys[LEAFBOOK_N] = {\n");
    for (long i = 0; i < nkeys; i++) {
        printf("0x%llxull,", (unsigned long long)keys[i]);
        if ((i & 7) == 7) printf("\n");
    }
    printf("\n};\n\n");
    printf("static const uint8_t leafbook_vals[LEAFBOOK_N] = {\n");
    for (long i = 0; i < nkeys; i++) {
        printf("%u,", vals[i]);
        if ((i & 31) == 31) printf("\n");
    }
    printf("\n};\n\n#endif\n");
    return 0;
}

// LEAFBOOK feasibility gate (docs/L1_SPEND_PLAN.md §4, step 1).
//
// Counts the DISTINCT canonical round-boundary ≤K-card 2-player endgame forms —
// the number of book entries a LEAFBOOK of reach K would need. Enumerates every
// placement of ≤K cards on the 4-suit × K-rank grid (suit 3 = trump) with each
// card owned by attacker or defender, canonicalizes via src/leafbook.h, and
// counts distinct keys. Because canonicalization rank-compacts and relabels the
// three non-trump suits, non-canonical placements collapse onto their
// representative, so the distinct-key count is exact.
//
// The §4 decision table: <=16,384 -> ship K (16 KiB @1B/entry); <=32,768 ->
// consider K; more -> drop to K-1.
//
//   cc -O2 -Isrc tools/leafbook/enumerate.c -o build/leafbook_enum && ./build/leafbook_enum
#include "leafbook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KMAX 6                 // enumerate up to 6 cards (grid ranks 0..5)
#define NCELL (4 * KMAX)       // 24 cells: suit*KMAX + rank

// Open-addressed hash set of uint64 keys, remembering each key's total card
// count (1..KMAX) and whether both hands are non-empty (a real book entry;
// a hand-empty position is terminal and never probed).
#define HBITS 23
#define HSIZE (1u << HBITS)
#define HMASK (HSIZE - 1u)
static uint64_t *hkey;         // 0 == empty slot (key is never 0: >=1 card set)
static long cnt_by_c[KMAX + 1];       // distinct forms with exactly c cards
static long cnt_nonempty_by_c[KMAX + 1]; // ... and both hands non-empty

static inline int hand_pop(uint64_t H) { return __builtin_popcountll(H); }

static void hset_add(uint64_t key, int c, int both_nonempty) {
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - HBITS)) & HMASK;
    for (;;) {
        if (hkey[i] == 0) {
            hkey[i] = key;
            cnt_by_c[c]++;
            if (both_nonempty) cnt_nonempty_by_c[c]++;
            return;
        }
        if (hkey[i] == key) return;   // already counted
        i = (i + 1) & HMASK;
    }
}

// DFS over the 24 grid cells; each cell empty / attacker / defender, pruned at
// KMAX cards. HA/HD are 52-bit hands (card (su,r) -> bit su*13+r).
static void dfs(int cell, int ncards, uint64_t HA, uint64_t HD) {
    if (cell == NCELL) {
        if (ncards == 0) return;
        uint64_t key = leafbook_key(HA, HD, /*power=*/3);
        int both = (HA != 0) && (HD != 0);
        hset_add(key, ncards, both);
        return;
    }
    int su = cell / KMAX, r = cell % KMAX;
    uint64_t bit = 1ull << (su * 13 + r);
    // empty
    dfs(cell + 1, ncards, HA, HD);
    if (ncards < KMAX) {
        dfs(cell + 1, ncards + 1, HA | bit, HD);        // attacker owns it
        dfs(cell + 1, ncards + 1, HA, HD | bit);        // defender owns it
    }
}

int main(void) {
    hkey = calloc(HSIZE, sizeof(uint64_t));
    if (!hkey) { fprintf(stderr, "oom\n"); return 1; }
    dfs(0, 0, 0, 0);

    printf("LEAFBOOK canonical-form census (trump fixed, 3 non-trump suits symmetric)\n");
    printf("%3s  %14s  %14s  %14s  %14s\n",
           "K", "forms(=K)", "forms(<=K)", "book(=K)*", "book(<=K)*");
    long cum = 0, cum_ne = 0;
    for (int c = 1; c <= KMAX; c++) {
        cum += cnt_by_c[c];
        cum_ne += cnt_nonempty_by_c[c];
        printf("%3d  %14ld  %14ld  %14ld  %14ld\n",
               c, cnt_by_c[c], cum, cnt_nonempty_by_c[c], cum_ne);
    }
    printf("\n* book() = both hands non-empty (terminal hand-empty forms are never probed).\n");
    printf("  Decision table (book(<=K), @1 byte/entry):\n");
    for (int K = 4; K <= KMAX; K++) {
        long b = 0;
        for (int c = 1; c <= K; c++) b += cnt_nonempty_by_c[c];
        const char *verdict = b <= 16384 ? "SHIP (<=16 KiB, sorted array)"
                            : b <= 32768 ? "consider (<=32 KiB, hashed)"
                            : "DROP to K-1 (> 32 KiB)";
        printf("    K=%d: %ld entries = %.1f KiB  -> %s\n", K, b, b / 1024.0, verdict);
    }
    free(hkey);
    return 0;
}

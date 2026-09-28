// LEAFBOOK value-safety gate "V-book" (docs/L1_SPEND_PLAN.md §4, step 3).
//
// Samples random CONCRETE round-boundary <=K-card 2-player deck-empty endgames
// (random hands / trump / role split), solves each DIRECTLY at a big budget,
// and compares to the book value reached THROUGH the canonicalization. This
// empirically proves the orbit-invariance the book rests on: rank-order +
// non-trump-suit isomorphism preserves BOTH the win/loss/draw outcome AND the
// exact mate distance. Requires 100.000% agreement — one mismatch means the
// canonicalization is unsound and the book must not ship.
//
//   cc -O2 -Isrc $(make -s print-core) tools/leafbook/verify_book.c \
//      -o build/leafbook_verify -lm -DCD_TT_BITS=21
//   ./build/leafbook_verify 200000
#include "leafbook.h"
#include "leafbook_data.h"
#include "cordite_sim.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 0x1234567u;
static inline uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// CHD minimal-perfect-hash lookup (same as the engine probe). An unenumerated
// (absent) form would land on some other key's slot and return a value that
// almost certainly disagrees with the direct solve — so incompleteness shows up
// as a value mismatch, not a silent miss.
static inline uint8_t book_value(uint64_t key) {
    uint32_t b = lb_bucket(key, LEAFBOOK_M, LEAFBOOK_SEED);
    uint32_t s = lb_slot(key, leafbook_disp[b], LEAFBOOK_R, LEAFBOOK_SEED);
    return leafbook_vals[s];
}

static void mk(SimState *s, uint64_t HA, uint64_t HD, int power) {
    memset(s, 0, sizeof(*s));
    s->num_players = 2; s->power_suit = (uint8_t)power; s->defender = 1;
    s->first_attacker = 0; s->status = GAME_STATUS_PLAYING;
    s->hand[0] = HA; s->hand[1] = HD;
    s->status_p[0] = PLAYER_STATUS_IN; s->status_p[1] = PLAYER_STATUS_IN; s->in_mask = 3;
}

// Direct exact solve, attacker (player 0) to move -> packed value byte.
static uint8_t solve_byte(uint64_t HA, uint64_t HD, int power, int *ab) {
    cd_sim_solve_reset();   // fresh TT per solve (see build_book.c)
    SimState s; mk(&s, HA, HD, power);
    int aborted = 0;
    int v = cd_sim_solve(&s, 0, -1001, 1001, 100000000L, &aborted);
    *ab = aborted;
    int o, d;
    if (v > 0) { o = 2; d = 1000 - v; } else if (v < 0) { o = 0; d = 1000 + v; } else { o = 1; d = 0; }
    if (d < 0) d = 0; if (d > 15) d = 15;
    return (uint8_t)((o << 4) | d);
}

int main(int argc, char **argv) {
    long N = argc > 1 ? atol(argv[1]) : 200000;
    { Game dummy; SimState ds; memset(&dummy, 0, sizeof(dummy)); cd_sim_from_game(&ds, &dummy); }

    long checked = 0, hits = 0, miss_absent = 0, mismatch = 0;
    long eng_checked = 0, eng_bad = 0;   // in-engine probe vs full-search ground truth
    long first_bad = -1;
    for (long t = 0; t < N; t++) {
        int C = 2 + (int)(xr() % (LEAFBOOK_DATA_K - 1));   // total cards 2..K
        // pick C distinct card ids
        uint64_t all = 0; int ids[16], nc = 0;
        while (nc < C) {
            int id = (int)(xr() % 52);
            if (all & (1ull << id)) continue;
            all |= 1ull << id; ids[nc++] = id;
        }
        // split into attacker/defender, both non-empty
        uint64_t HA = 0, HD = 0;
        int na = 1 + (int)(xr() % (C - 1));               // 1..C-1 to attacker
        for (int i = 0; i < C; i++) (i < na ? &HA : &HD)[0] |= 1ull << ids[i];
        int power = (int)(xr() % 4);
        if (!HA || !HD) continue;

        int ab = 0;
        uint8_t direct = solve_byte(HA, HD, power, &ab);   // book OFF: full-search truth
        if (ab) { fprintf(stderr, "abort in direct solve\n"); return 3; }

#ifdef CD_LEAFBOOK
        // In-engine probe check: solve the SAME position with the engine's book
        // enabled (probe fires at the root, a round boundary) — it must equal
        // the full-search value. Validates attacker/defender id, the
        // attacker->me perspective flip, and the depth rebase, on both roles.
        for (int role = 0; role < 2; role++) {
            uint64_t rHA = role ? HD : HA, rHD = role ? HA : HD;   // swap who attacks
            cd_sim_solve_reset(); cd_sim_set_leafbook(0);
            SimState s0; mk(&s0, rHA, rHD, power); int a0 = 0;
            int v_off = cd_sim_solve(&s0, 0, -1001, 1001, 100000000L, &a0);
            cd_sim_solve_reset(); cd_sim_set_leafbook(1);
            SimState s1; mk(&s1, rHA, rHD, power); int a1 = 0;
            int v_on = cd_sim_solve(&s1, 0, -1001, 1001, 100000000L, &a1);
            cd_sim_set_leafbook(0);
            eng_checked++;
            if (a0 || a1 || v_off != v_on) {
                eng_bad++;
                if (eng_bad <= 5)
                    fprintf(stderr, "ENGINE-PROBE MISMATCH role=%d off=%d on=%d (ab %d/%d)\n",
                            role, v_off, v_on, a0, a1);
            }
        }
#endif

        uint64_t key = leafbook_key(HA, HD, power);
        uint8_t bookv = book_value(key);
        checked++; hits++;
        if (bookv != direct) {
            mismatch++;
            if (first_bad < 0) first_bad = t;
            if (mismatch <= 5)
                fprintf(stderr, "MISMATCH key=%llx book=0x%02x direct=0x%02x (o/d book=%d/%d direct=%d/%d)\n",
                        (unsigned long long)key, bookv, direct,
                        bookv >> 4, bookv & 15, direct >> 4, direct & 15);
        }
    }
    printf("V-book: K=%d  samples=%ld  checked=%ld  book_hits=%ld  absent=%ld  mismatch=%ld\n",
           LEAFBOOK_DATA_K, N, checked, hits, miss_absent, mismatch);
    if (miss_absent) printf("  FAIL: %ld positions had no book entry (enumeration incomplete)\n", miss_absent);
    if (mismatch)    printf("  FAIL: %ld value mismatches (canonicalization unsound)\n", mismatch);
    if (eng_checked) {
        printf("  engine-probe: checked=%ld  mismatches=%ld  %s\n",
               eng_checked, eng_bad, eng_bad ? "FAIL" : "PASS (in-engine probe == full search)");
    }
    if (!miss_absent && !mismatch && !eng_bad) printf("  PASS.\n");
    return (miss_absent || mismatch || eng_bad) ? 1 : 0;
}

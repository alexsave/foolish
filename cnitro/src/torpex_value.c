// Torpex value net: C inference for the numpy-trained MLP (train_torpex.py).
//
// V(full-information SimState, seat) -> predicted normalized finish [0,1]
// (0 = winner, 1 = durak) under strong (semtex self-play) continuations.
// Replaces the handwritten-policy rollout inside torpex's determinized MC:
// the sampled worlds it evaluates are exactly the full-info states the net
// was trained on (same encoding as src/main_gen.c snapshots, trump-rotated).
//
// Weights: flat float32 file with 16-byte header 'TPX1' + i32 in_dim,h1,h2
// (see train_torpex.py). Loaded lazily from $TORPEX_WEIGHTS (default
// "torpex_weights.bin" next to the working directory).

#include "torpex_value.h"
#include "cordite_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define TX_IN  (7 * 52 + 14)
#define TX_H1  256
#define TX_H2  64

static float *tx_W1, *tx_b1, *tx_W2, *tx_b2, *tx_W3, *tx_b3;
static int tx_loaded = -1;   // -1 unknown, 0 failed, 1 ok

int tx_value_ready(void) {
    if (tx_loaded >= 0) return tx_loaded;
    const char *path = getenv("TORPEX_WEIGHTS");
    if (!path || !path[0]) path = "torpex_weights.bin";
    FILE *f = fopen(path, "rb");
    if (!f) { tx_loaded = 0; return 0; }
    char magic[4]; int32_t dims[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "TPX1", 4) != 0
        || fread(dims, 4, 3, f) != 3
        || dims[0] != TX_IN || dims[1] != TX_H1 || dims[2] != TX_H2) {
        fclose(f); tx_loaded = 0; return 0;
    }
    size_t n1 = (size_t)TX_IN * TX_H1, n2 = (size_t)TX_H1 * TX_H2, n3 = TX_H2;
    tx_W1 = malloc(n1 * 4); tx_b1 = malloc(TX_H1 * 4);
    tx_W2 = malloc(n2 * 4); tx_b2 = malloc(TX_H2 * 4);
    tx_W3 = malloc(n3 * 4); tx_b3 = malloc(4);
    int ok = tx_W1 && tx_b1 && tx_W2 && tx_b2 && tx_W3 && tx_b3
        && fread(tx_W1, 4, n1, f) == n1 && fread(tx_b1, 4, TX_H1, f) == TX_H1
        && fread(tx_W2, 4, n2, f) == n2 && fread(tx_b2, 4, TX_H2, f) == TX_H2
        && fread(tx_W3, 4, n3, f) == n3 && fread(tx_b3, 4, 1, f) == 1;
    fclose(f);
    tx_loaded = ok ? 1 : 0;
    if (!ok) fprintf(stderr, "torpex: failed to load weights from %s\n", path);
    return tx_loaded;
}

static inline void tx_mask_bits(float *dst, uint64_t m, int power) {
    // rotate suits so trump = suit 0, then scatter 52 bits
    while (m) {
        int id = __builtin_ctzll(m); m &= m - 1;
        int suit = id / 13, v = id % 13;
        dst[((suit - power + 4) & 3) * 13 + v] = 1.0f;
    }
}

float tx_value(const SimState *s, int p) {
    static _Thread_local float x[TX_IN], h1[TX_H1], h2[TX_H2];
    memset(x, 0, sizeof(x));
    int power = s->power_suit, np = s->num_players;

    uint64_t opp[3] = {0, 0, 0};
    int cnt[3] = {0, 0, 0};
    int oi = 0;
    for (int step = 1; step < np; step++) {
        int q = (p + step) % np;
        if (s->status_p[q] != PLAYER_STATUS_IN) continue;
        int slot = oi < 2 ? oi : 2;
        opp[slot] |= s->hand[q];
        cnt[slot] += __builtin_popcountll(s->hand[q]);
        oi++;
    }
    uint64_t att_unc = 0, att_cov = 0, def_cov = 0;
    for (int i = 0; i < s->num_battles; i++) {
        if (s->covered_mask & (1ull << i)) {
            att_cov |= 1ull << s->atk[i];
            def_cov |= 1ull << s->def[i];
        } else att_unc |= 1ull << s->atk[i];
    }
    tx_mask_bits(x + 0 * 52, s->hand[p], power);
    tx_mask_bits(x + 1 * 52, opp[0], power);
    tx_mask_bits(x + 2 * 52, opp[1], power);
    tx_mask_bits(x + 3 * 52, opp[2], power);
    tx_mask_bits(x + 4 * 52, att_unc, power);
    tx_mask_bits(x + 5 * 52, att_cov, power);
    tx_mask_bits(x + 6 * 52, def_cov, power);

    int in_c = 0;
    for (int i = 0; i < np; i++) if (s->status_p[i] == PLAYER_STATUS_IN) in_c++;
    float *sc = x + 7 * 52;
    sc[0] = (float)np / 8.0f;
    sc[1] = (float)in_c / 8.0f;
    sc[2] = (float)s->deck_n / 52.0f;
    sc[3] = (float)(s->discard_pile_length > 255 ? 255 : s->discard_pile_length) / 52.0f;
    sc[4] = s->has_flipped ? 1.0f : 0.0f;
    if (s->has_flipped) {
        int id = s->flipped_id, suit = id / 13, v = id % 13;
        int rid = ((suit - power + 4) & 3) * 13 + v;
        sc[5] = (float)(rid % 13) / 13.0f;
        sc[6] = rid < 13 ? 1.0f : 0.0f;
    }
    sc[7] = (s->defender == p) ? 1.0f : 0.0f;
    sc[8] = (float)((s->defender - p + np) % np) / 8.0f;
    sc[9] = (float)((s->first_attacker - p + np) % np) / 8.0f;
    sc[10] = (float)__builtin_popcountll(s->hand[p]) / 18.0f;
    sc[11] = (float)cnt[0] / 18.0f;
    sc[12] = (float)cnt[1] / 18.0f;
    sc[13] = (float)cnt[2] / 18.0f;

    // forward: x is sparse-ish (only set features matter) but a dense GEMV
    // at -O3 is fast enough (~100k MACs).
    for (int j = 0; j < TX_H1; j++) h1[j] = tx_b1[j];
    for (int i = 0; i < TX_IN; i++) {
        float xi = x[i];
        if (xi == 0.0f) continue;
        const float *w = tx_W1 + (size_t)i * TX_H1;
        for (int j = 0; j < TX_H1; j++) h1[j] += xi * w[j];
    }
    for (int j = 0; j < TX_H1; j++) if (h1[j] < 0) h1[j] = 0;
    for (int j = 0; j < TX_H2; j++) h2[j] = tx_b2[j];
    for (int i = 0; i < TX_H1; i++) {
        float hi = h1[i];
        if (hi == 0.0f) continue;
        const float *w = tx_W2 + (size_t)i * TX_H2;
        for (int j = 0; j < TX_H2; j++) h2[j] += hi * w[j];
    }
    float z = tx_b3[0];
    for (int j = 0; j < TX_H2; j++) if (h2[j] > 0) z += h2[j] * tx_W3[j];
    return 1.0f / (1.0f + expf(-z));
}

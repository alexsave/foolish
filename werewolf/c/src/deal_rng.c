// deal_rng — ChaCha20 keystream generator. See deal_rng.h for rationale.
//
// This is the standard ChaCha20 (RFC 8439) block function. We use the full 20
// rounds so the output matches the RFC test vectors bit-for-bit (see the KAT in
// tests.c); the deal consumes only a handful of blocks per game, so rounds are
// not a hot path. Integer-only => identical on x86 and wasm32.

#include "deal_rng.h"

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

// ChaCha quarter-round on four state words.
#define QR(a, b, c, d)                 \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7)

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void deal_rng_block(const uint32_t state_in[16], uint32_t out[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = state_in[i];
    for (int i = 0; i < 10; i++) {          // 10 double-rounds = 20 rounds
        QR(x[0], x[4], x[8],  x[12]);       // column rounds
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);       // diagonal rounds
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + state_in[i];
}

void deal_rng_seed(DealRng *r, const uint8_t seed[32]) {
    // "expand 32-byte k" — the ChaCha sigma constants.
    r->state[0] = 0x61707865u; r->state[1] = 0x3320646eu;
    r->state[2] = 0x79622d32u; r->state[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) r->state[4 + i] = rd_le32(seed + 4 * i);
    r->state[12] = 0; r->state[13] = 0;     // 64-bit block counter
    r->state[14] = 0; r->state[15] = 0;     // 64-bit nonce (fixed 0)
    r->used = 16;                            // force a block on first draw
}

uint32_t deal_rng_u32(DealRng *r) {
    if (r->used >= 16) {
        deal_rng_block(r->state, r->block);
        // Advance the 64-bit block counter (words 12..13).
        if (++r->state[12] == 0) r->state[13]++;
        r->used = 0;
    }
    return r->block[r->used++];
}

uint32_t deal_rng_bounded(DealRng *r, uint32_t n) {
    if (n <= 1) return 0;
    // Reject the incomplete high bucket so every residue is equally likely.
    // threshold = 2^32 mod n; keep values >= threshold, then take mod.
    uint32_t threshold = (uint32_t)(-n) % n;   // (2^32 - n) mod n == 2^32 mod n
    uint32_t v;
    do { v = deal_rng_u32(r); } while (v < threshold);
    return v % n;
}

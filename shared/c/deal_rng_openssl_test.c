/* deal_rng_openssl_test.c - the deal's ChaCha20 (deal_rng.h) against OpenSSL's
 * EVP_chacha20 over random keys, counters and lengths. OpenSSL's 16-byte IV is
 * state words 12..15 little-endian (a 32-bit counter then the 96-bit nonce);
 * deal_rng's is a 64-bit counter and a 64-bit nonce in the same four words, so
 * the two agree on every stream whose counter does not carry out of word 12
 * (this test keeps the counter's low word clear of 2^32 - 1024 blocks) - and the
 * carry itself is deal_rng_test's. A native-only test.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I$OPENSSL/include deal_rng_openssl_test.c deal_rng.c \
 *      -L$OPENSSL/lib -lcrypto -o deal_rng_openssl_test && ./deal_rng_openssl_test
 */
#include <stdint.h>
#include <string.h>
#include <openssl/evp.h>
#include "test/check.h"
#include "deal_rng.h"
#include "rng.h"

static uint64_t rng = 0xc0ffee;
static uint32_t next(void) { return rng_u32(&rng); }

static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* The keystream: ChaCha20 over `n` zero bytes. */
static int stream(const uint8_t key[32], const uint8_t iv[16], uint8_t *out, int n)
{
    static const uint8_t zero[4096 * 4];
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int len = 0, ok = c && EVP_EncryptInit_ex(c, EVP_chacha20(), NULL, key, iv) == 1 &&
                     EVP_EncryptUpdate(c, out, &len, zero, n) == 1 && len == n;
    EVP_CIPHER_CTX_free(c);
    return ok;
}

int main(void)
{
    static uint8_t want[4096 * 4];
    TEST("deal_rng vs OpenSSL: the block function over random states");
    for (int t = 0; t < 500; t++) {
        uint32_t st[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 }, out[16];
        uint8_t key[32], iv[16];
        for (int i = 4; i < 16; i++) st[i] = next();
        for (int i = 0; i < 8; i++) le32(key + 4 * i, st[4 + i]);
        for (int i = 0; i < 4; i++) le32(iv + 4 * i, st[12 + i]);
        deal_rng_block(st, out);
        CHECK(stream(key, iv, want, 64), "openssl streams");
        int ok = 1;
        for (int i = 0; i < 16; i++) {
            uint8_t w[4];
            le32(w, out[i]);
            ok &= memcmp(w, want + 4 * i, 4) == 0;
        }
        CHECK(ok, "state %d: block equals OpenSSL's", t);
    }
    TEST("deal_rng vs OpenSSL: whole streams, random seeds, starts and lengths");
    for (int t = 0; t < 300; t++) {
        uint8_t seed[32], iv[16] = { 0 };
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)next();
        uint32_t words = 1 + next() % 4096;
        uint32_t block = next() % 0xFFFFFC00u, hi = next() % 4;
        le32(iv, block);
        le32(iv + 4, hi);   /* the counter's high word; the nonce words stay zero */
        DealRng r;
        deal_rng_seed_at(&r, seed, ((uint64_t)hi << 32) | block);
        CHECK(stream(seed, iv, want, (int)(words * 4)), "openssl streams");
        int ok = 1;
        for (uint32_t i = 0; i < words; i++) {
            uint8_t w[4];
            le32(w, deal_rng_u32(&r));
            ok &= memcmp(w, want + 4 * i, 4) == 0;
        }
        CHECK(ok, "seed %d: %u words from block %u equal OpenSSL's", t, words, block);
    }
    return report("deal_rng_openssl_test");
}

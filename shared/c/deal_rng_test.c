/* deal_rng_test.c - the deal's ChaCha20 stream (deal_rng.h): RFC 8439 known
 * answers, seeking equal to drawing (across the 32-bit counter carry), and
 * deal_rng_bounded staying below n. Deterministic and bounded.
 *
 *   cc -std=c11 -Wall -Wextra -Werror deal_rng_test.c deal_rng.c -o deal_rng_test && ./deal_rng_test
 *
 * The feed-forward (the block's input added to its output) is what keeps an
 * observer from running the permutation backwards to the key; removing it
 * fails every known answer here. Exits 1 on any failure. */
#include <stdint.h>
#include "test/check.h"
#include "deal_rng.h"

int main(void)
{
    TEST("deal_rng: RFC 8439 2.3.2 block");
    const uint32_t st[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, 0x03020100, 0x07060504,
                              0x0b0a0908, 0x0f0e0d0c, 0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
                              0x00000001, 0x09000000, 0x4a000000, 0x00000000 };
    const uint32_t want[16] = { 0xe4e7f110, 0x15593bd1, 0x1fdd0f50, 0xc47120a3, 0xc7f4d1c7, 0x0368c033,
                                0x9aaa2204, 0x4e6cd4c3, 0x466482d2, 0x09aa9f07, 0x05d7c214, 0xa2028bd9,
                                0xd19c12b5, 0xb94e16de, 0xe883d0cb, 0x4e3c50a2 };
    uint32_t out[16];
    deal_rng_block(st, out);
    for (int i = 0; i < 16; i++) CHECK(out[i] == want[i], "word %d is %08x, want %08x", i, out[i], want[i]);

    TEST("deal_rng: RFC 8439 A.1 vectors 1 and 2 (zero key, counters 0 and 1)");
    static const uint8_t ks1[64] = {
        0x76, 0xb8, 0xe0, 0xad, 0xa0, 0xf1, 0x3d, 0x90, 0x40, 0x5d, 0x6a, 0xe5, 0x53, 0x86, 0xbd, 0x28,
        0xbd, 0xd2, 0x19, 0xb8, 0xa0, 0x8d, 0xed, 0x1a, 0xa8, 0x36, 0xef, 0xcc, 0x8b, 0x77, 0x0d, 0xc7,
        0xda, 0x41, 0x59, 0x7c, 0x51, 0x57, 0x48, 0x8d, 0x77, 0x24, 0xe0, 0x3f, 0xb8, 0xd8, 0x4a, 0x37,
        0x6a, 0x43, 0xb8, 0xf4, 0x15, 0x18, 0xa1, 0x1c, 0xc3, 0x87, 0xb6, 0x69, 0xb2, 0xee, 0x65, 0x86 };
    uint8_t zero[32] = { 0 };
    DealRng r;
    deal_rng_seed(&r, zero);
    for (int i = 0; i < 16; i++) {
        uint32_t w = deal_rng_u32(&r);
        uint32_t e = (uint32_t)ks1[4 * i] | (uint32_t)ks1[4 * i + 1] << 8 | (uint32_t)ks1[4 * i + 2] << 16
                   | (uint32_t)ks1[4 * i + 3] << 24;
        CHECK(w == e, "vector 1 word %d is %08x, want %08x", i, w, e);
    }
    DealRng r1;
    deal_rng_seed_at(&r1, zero, 1);
    CHECK(deal_rng_u32(&r1) == 0xbee7079fu, "vector 2 first word");
    CHECK(deal_rng_u32(&r) == 0xbee7079fu, "vector 2 follows vector 1 in the stream");

    TEST("deal_rng: seeking equals drawing");
    uint8_t seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 37 + 11);
    DealRng a;
    deal_rng_seed(&a, seed);
    for (uint64_t B = 0; B < 256; B++) {
        DealRng b;
        deal_rng_seed_at(&b, seed, B);
        int same = 1;
        for (int j = 0; j < 16; j++) same &= deal_rng_u32(&a) == deal_rng_u32(&b);
        CHECK(same, "block %llu", (unsigned long long)B);
    }
    {
        DealRng p, q;
        deal_rng_seed_at(&p, seed, 0xFFFFFFFFull);
        for (int j = 0; j < 16; j++) deal_rng_u32(&p);
        deal_rng_seed_at(&q, seed, 0x100000000ull);
        int same = 1;
        for (int j = 0; j < 32; j++) same &= deal_rng_u32(&p) == deal_rng_u32(&q);
        CHECK(same, "the counter carries from block 2^32-1 into 2^32");
    }

    TEST("deal_rng: bounded stays below n");
    DealRng g;
    deal_rng_seed(&g, seed);
    CHECK(deal_rng_bounded(&g, 0) == 0 && deal_rng_bounded(&g, 1) == 0, "n 0 and 1 give 0");
    static const uint32_t ns[] = { 2, 3, 6, 7, 52, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu };
    for (int t = 0; t < 8; t++) {
        int ok = 1;
        for (int i = 0; i < 2000; i++) ok &= deal_rng_bounded(&g, ns[t]) < ns[t];
        CHECK(ok, "n %u", ns[t]);
    }
    {
        long c[6] = { 0 };
        for (int i = 0; i < 6000; i++) c[deal_rng_bounded(&g, 6)]++;
        int ok = 1;
        for (int k = 0; k < 6; k++) ok &= c[k] > 850 && c[k] < 1150;
        CHECK(ok, "6000 dice land 850..1150 per face");
    }
    return report("deal_rng_test");
}

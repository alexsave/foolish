/* hand_contract_test.c - the hand's layout (hand_layout.c) must be computed
 * WITHOUT floating-point contraction, in every build tree that links it.
 *
 * The web and Swift compute the same slots with the same expressions, and
 * neither fuses a*b+c into a fused multiply-add. clang on arm64 DOES by default,
 * which moves a slot by a ulp, which moves a card by a pixel on one platform. The
 * rule lives in one place, the build flag -ffp-contract=off (c/Makefile CFLAGS,
 * IOS_CFLAGS, ASAN_CFLAGS, COV_FLAGS, TABLE_HOST_CFLAGS), and a new build tree
 * that forgets it would be silently wrong. This test is built with the lane's
 * own CFLAGS and fails there when the flag is missing. Three things:
 *
 *   1. a CANARY in this file's own translation unit: a*b+c where the product
 *      needs more than 53 bits, so a fused build answers -2^-54 and an unfused
 *      one 0. It reads the flags this file was compiled with;
 *   2. a PIN: an FNV-1a digest of every slot hand_layout() answers over a sweep
 *      of counts and widths (HAND_IOS's metrics), computed with the flag. It reads
 *      the flags hand_layout.c was compiled with (the linked object);
 *   3. TEETH: hand_layout.c compiled a second time with -ffp-contract=on (Makefile
 *      build/obj/fused, symbols renamed) must NOT match the pin on a platform whose
 *      compiler fuses (__FP_FAST_FMA); otherwise (1) and (2) could be vacuous. */
#include <stdint.h>
#include <string.h>
#include "../../../shared/c/fnv.h"
#include "../../../shared/c/test/check.h"
#include "../src/hand_layout.h"

int hand_layout_fused(const HandMetrics *m, int count, double width, HandLayout *out);

__attribute__((noinline)) static double axpb(double a, double b, double c) { return a * b + c; }

typedef int (*layout_fn)(const HandMetrics *, int, double, HandLayout *);

static uint64_t sweep_digest(layout_fn f)
{
    const HandMetrics m = { 52, 22, 72, 4, 6, 8, 34 };   /* HAND_IOS */
    uint64_t h = FNV64_OFFSET;
    for (int n = 1; n <= 40; n++)
        for (int i = 0; i < 2400; i++) {
            const double w = 20 + 0.37 * i;
            HandLayout l;
            memset(&l, 0, sizeof l);
            const int rc = f(&m, n, w, &l);
            h = fnv1a64_step(h, (uint64_t)rc);
            h = fnv1a64(h, &l.rows, sizeof l.rows);
            h = fnv1a64_step(h, (uint64_t)l.n);
            h = fnv1a64(h, l.slot, sizeof(HandRect) * (size_t)l.n);
        }
    return h;
}

/* Computed with -ffp-contract=off; the same on every platform and compiler. */
#define HAND_SWEEP_DIGEST 0xab35cb265c121b11ull

int main(void)
{
    TEST("hand contraction: the canary (this file's flags)");
    volatile double a = 1.0 + 0x1p-27, b = 1.0 - 0x1p-27, c = -1.0;
    CHECK(axpb(a, b, c) == 0.0, "a*b+c fused: this build tree lacks -ffp-contract=off (%g)", axpb(a, b, c));

    TEST("hand contraction: hand_layout.c as linked");
    const uint64_t d = sweep_digest(hand_layout);
    CHECK(d == HAND_SWEEP_DIGEST, "the sweep digest is %016llx, not the pinned unfused one: hand_layout.c was built with contraction on, or its arithmetic moved",
          (unsigned long long)d);

    TEST("hand contraction: the pin has teeth");
#ifdef __FP_FAST_FMA
    const uint64_t f = sweep_digest(hand_layout_fused);
    CHECK(f != HAND_SWEEP_DIGEST, "hand_layout.c built with -ffp-contract=on matches the unfused pin: the sweep cannot see contraction");
#else
    printf("  (this compiler does not fuse a*b+c: the teeth check is skipped)\n");
#endif
    return report("hand_contract_test");
}

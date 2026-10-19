/* util_test.c - util.h: the clamps (bounds not ordered, NaN passes through), the
 * min and the max, the popcounts and ARRAY_LEN. */
#include <math.h>
#include <stdint.h>
#include "util.h"
#include "test/check.h"

int main(void)
{
    TEST("util: ARRAY_LEN");
    int a[7];
    double b[3][5];
    CHECK(ARRAY_LEN(a) == 7 && ARRAY_LEN(b) == 3 && ARRAY_LEN(b[0]) == 5, "the element counts of arrays");

    TEST("util: clamps hold both ends and the inside");
    CHECK(util_clampi(5, 0, 10) == 5 && util_clampi(-1, 0, 10) == 0 && util_clampi(11, 0, 10) == 10, "int");
    CHECK(util_clampi(0, 0, 10) == 0 && util_clampi(10, 0, 10) == 10, "int, on the bounds");
    CHECK(util_clampf(.5f, 0, 1) == .5f && util_clampf(-.1f, 0, 1) == 0 && util_clampf(1.5f, 0, 1) == 1, "float");
    CHECK(util_clampd(.5, 0, 1) == .5 && util_clampd(-.1, 0, 1) == 0 && util_clampd(1.5, 0, 1) == 1, "double");
    CHECK(util_clampd(-0.0, 0, 1) == 0, "double, negative zero equals zero");

    TEST("util: the bounds are not ordered (lo > hi: hi for a value above lo, lo below it)");
    CHECK(util_clampi(5, 10, 0) == 10, "int 5 in [10, 0]: below lo is lo");
    CHECK(util_clampi(15, 10, 0) == 0, "int 15 in [10, 0]: above lo and above hi is hi");
    CHECK(util_clampi(-3, 10, 0) == 10, "int -3 in [10, 0]");

    TEST("util: NaN passes through the float clamps");
    CHECK(isnan(util_clampf(NAN, 0, 1)) && isnan(util_clampd(NAN, 0, 1)), "NaN stays NaN");

    TEST("util: min and max");
    CHECK(util_mini(3, 4) == 3 && util_mini(4, 3) == 3 && util_mini(-2, 2) == -2 && util_mini(7, 7) == 7, "mini");
    CHECK(util_maxi(3, 4) == 4 && util_maxi(4, 3) == 4 && util_maxi(-2, 2) == 2 && util_maxi(7, 7) == 7, "maxi");
    CHECK(util_mind(3.5, 4.5) == 3.5 && util_mind(4.5, 3.5) == 3.5 && util_mind(-1e300, 1e300) == -1e300, "mind");
    CHECK(util_maxd(3.5, 4.5) == 4.5 && util_maxd(4.5, 3.5) == 4.5 && util_maxd(-1e300, 1e300) == 1e300, "maxd");

    TEST("util: popcount");
    CHECK(util_popcount32(0) == 0 && util_popcount32(1) == 1 && util_popcount32(0xffffffffu) == 32, "32, the ends");
    CHECK(util_popcount32(0x80000001u) == 2 && util_popcount32(0x0f0f0f0fu) == 16, "32, patterns");
    CHECK(util_popcount64(0) == 0 && util_popcount64(~0ull) == 64, "64, the ends");
    CHECK(util_popcount64(0x8000000000000001ull) == 2 && util_popcount64(0x100000000ull) == 1, "64, above the low word");
    CHECK(util_popcount64(0xaaaaaaaaaaaaaaaaull) == 32, "64, alternating");
    return report("util_test");
}

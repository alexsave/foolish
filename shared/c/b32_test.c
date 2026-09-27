/* b32_test.c - base32 (b32.h): the RFC 4648 vectors, round trips at the
 * exact capacity and one short of it, and hostile text against every
 * capacity from -4 up. Deterministic and bounded, so `make run` stays fast.
 *
 *   cc -std=c11 -Wall -Wextra -Werror b32_test.c b32.c -o b32_test && ./b32_test
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include "test/check.h"
#include "b32.h"

static uint64_t rng = 88172645463325252ull;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

int main(void)
{
    TEST("b32: RFC 4648 section 10 vectors");
    static const char *const plain[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *const coded[] = { "", "MY", "MZXQ", "MZXW6", "MZXW6YQ", "MZXW6YTB", "MZXW6YTBOI" };
    for (int i = 0; i < 7; i++) {
        char out[16];
        int n = (int)strlen(plain[i]);
        int w = b32_encode((const unsigned char *)plain[i], n, out, sizeof out);
        CHECK(w == (int)strlen(coded[i]) && !strcmp(out, coded[i]), "encode \"%s\" gave \"%s\"", plain[i], out);
        CHECK(w == B32_LEN(n), "B32_LEN(%d) is %d, encode wrote %d", n, B32_LEN(n), w);
        unsigned char back[16];
        CHECK(b32_decode(coded[i], back, sizeof back) == n && !memcmp(back, plain[i], (size_t)n),
              "decode \"%s\"", coded[i]);
    }

    TEST("b32: decode forgives case and strays, and stops at '-'");
    unsigned char b[16];
    CHECK(b32_decode("mzxw6", b, sizeof b) == 3 && !memcmp(b, "foo", 3), "lower case");
    CHECK(b32_decode("MZ.XW 6", b, sizeof b) == 3 && !memcmp(b, "foo", 3), "stray characters");
    CHECK(b32_decode("MZXW6-XYZ", b, sizeof b) == 3 && !memcmp(b, "foo", 3), "a suffix after '-'");
    CHECK(b32_decode(NULL, b, sizeof b) == 0, "NULL text");
    CHECK(b32_decode("", b, sizeof b) == 0, "empty text");

    TEST("b32: B32_LEN does not overflow int");
    CHECK(B32_LEN(300000000) == 480000000, "B32_LEN(300000000) = %d", B32_LEN(300000000));
    volatile int big = 300000000;   /* at run time too, where int overflow would not be diagnosed */
    CHECK(B32_LEN(big) == 480000000, "B32_LEN(big) = %d", B32_LEN(big));
    CHECK(B32_LEN(0) == 0 && B32_LEN(1) == 2 && B32_LEN(5) == 8, "small lengths");

    TEST("b32: round trips at the exact capacity");
    for (int it = 0; it < 3000; it++) {
        unsigned char in[300], back[301];
        char txt[486];
        int n = (int)(next() % 300);
        for (int i = 0; i < n; i++) in[i] = (unsigned char)next();
        int L = B32_LEN(n);
        memset(txt, 0x5a, sizeof txt);
        int w = b32_encode(in, n, txt, L + 1);
        CHECK(w == L && (int)strlen(txt) == L && txt[L + 1] == 0x5a, "encode n %d wrote %d", n, w);
        if (n > 0) CHECK(b32_encode(in, n, txt, L) == -1, "encode n %d accepted cap one short", n);
        memset(back, 0x5a, sizeof back);
        w = b32_encode(in, n, txt, L + 1);
        CHECK(b32_decode(txt, back, n) == n && !memcmp(in, back, (size_t)n) && back[n] == 0x5a,
              "round trip n %d", n);
        if (n > 0) CHECK(b32_decode(txt, back, n - 1) == -1, "decode n %d accepted cap one short", n);
    }

    TEST("b32: hostile text never writes past cap");
    for (int it = 0; it < 3000; it++) {
        char h[401];
        unsigned char o[264];
        int tl = (int)(next() % 400);
        for (int i = 0; i < tl; i++) h[i] = (char)(next() % 255 + 1);
        h[tl] = 0;
        int cap = (int)(next() % 260) - 4;
        memset(o, 0x5a, sizeof o);
        int got = b32_decode(h, o, cap);
        int lim = cap < 0 ? 0 : cap;
        CHECK(got >= -1 && got <= lim, "decode returned %d for cap %d", got, cap);
        for (int i = lim; i < (int)sizeof o; i++)
            if (o[i] != 0x5a) { CHECK(0, "decode wrote byte %d past cap %d", i, cap); break; }
    }
    return report("b32_test");
}

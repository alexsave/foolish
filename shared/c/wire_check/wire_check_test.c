/* wire_check_test.c - the wire check (wire_check.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror ../sha256.c wire_check.c wire_check_test.c -o wire_check_test && ./wire_check_test
 *
 * No -I: the headers are beside this file and one up. Exits 1 on any failure.
 * Two kinds of expected value: the FIPS 180-4 digests of "" and "abc" written
 * out as constants, so the check is pinned to SHA-256 itself and not only to
 * whatever sha256.c computes today; and SHA-256 of the concatenation computed
 * here in one piece, so every split of a message into head and body must land
 * on the same bytes. */
#include <stdio.h>
#include <string.h>
#include "wire_check.h"
#include "../sha256.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)

/* SHA-256("") and SHA-256("abc"), FIPS 180-4 / NIST CSRC examples. */
static const uint8_t EMPTY[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
static const uint8_t ABC[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };

/* `n` bytes written and not one more: the rest of a 40-byte canvas is 0xAA. */
static int wrote(const uint8_t *out, const uint8_t *want, size_t n)
{
    if (memcmp(out, want, n)) return 0;
    for (size_t i = n; i < 40; i++) if (out[i] != 0xAA) return 0;
    return 1;
}

static int split_ok(const uint8_t *msg, size_t len, size_t cut, size_t check_len)
{
    uint8_t whole[32], out[40];
    sha256(msg, len, whole);
    memset(out, 0xAA, sizeof out);
    wire_check(msg, cut, msg + cut, len - cut, out, check_len);
    return wrote(out, whole, check_len < 32 ? check_len : 32);
}

int main(void)
{
    uint8_t out[40];
    const uint8_t *abc = (const uint8_t *)"abc";

    /* KNOWN ANSWERS: the digest's leading bytes, at the three lengths that
     * matter (one byte, the two a product carries today, the whole digest). */
    static const size_t lens[] = { 1, 2, 32 };
    for (int k = 0; k < 3; k++) {
        size_t n = lens[k];
        char what[80];
        memset(out, 0xAA, sizeof out);
        wire_check(abc, 1, abc + 1, 2, out, n);
        snprintf(what, sizeof what, "known answer: \"a\" + \"bc\" at %zu bytes", n);
        OK(wrote(out, ABC, n), what);
        memset(out, 0xAA, sizeof out);
        wire_check(abc, 3, NULL, 0, out, n);
        snprintf(what, sizeof what, "known answer: \"abc\" + empty body at %zu bytes", n);
        OK(wrote(out, ABC, n), what);
        memset(out, 0xAA, sizeof out);
        wire_check(NULL, 0, abc, 3, out, n);
        snprintf(what, sizeof what, "known answer: empty head + \"abc\" at %zu bytes", n);
        OK(wrote(out, ABC, n), what);
        memset(out, 0xAA, sizeof out);
        wire_check(NULL, 0, NULL, 0, out, n);
        snprintf(what, sizeof what, "known answer: both spans empty at %zu bytes", n);
        OK(wrote(out, EMPTY, n), what);
    }

    /* The head comes first: swapping the spans changes the check. */
    memset(out, 0xAA, sizeof out);
    wire_check("bc", 2, "a", 1, out, 32);
    OK(memcmp(out, ABC, 32) != 0, "order: body before head is a different check");

    /* ZERO LENGTH writes nothing; above the digest's length writes 32. */
    memset(out, 0xAA, sizeof out);
    wire_check(abc, 3, NULL, 0, out, 0);
    OK(wrote(out, ABC, 0), "check_len 0 writes nothing");
    memset(out, 0xAA, sizeof out);
    wire_check(abc, 3, NULL, 0, out, 33);
    OK(wrote(out, ABC, 32), "check_len 33 writes the 32-byte digest and no more");

    /* EVERY SPLIT of messages across SHA-256's block edges (55, 56, 63, 64,
     * 65, 119, 128 bytes and more) equals the one-piece digest, at 1, 2 and
     * 32 bytes. */
    static uint8_t msg[300];
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < sizeof msg; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; msg[i] = (uint8_t)x; }
    static const size_t sizes[] = { 0, 1, 2, 31, 55, 56, 57, 63, 64, 65, 119, 120, 128, 129, 200, 300 };
    int split_fails = 0, splits = 0;
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++)
        for (size_t cut = 0; cut <= sizes[s]; cut++)
            for (int k = 0; k < 3; k++) {
                splits++;
                if (!split_ok(msg, sizes[s], cut, lens[k])) {
                    if (split_fails++ < 5)
                        printf("FAIL split: %zu bytes cut at %zu, %zu-byte check\n", sizes[s], cut, lens[k]);
                }
            }
    checks += splits; fails += split_fails;

    /* The last body byte counts: flipping it moves the full-length check. */
    uint8_t a[32], b[32];
    wire_check(msg, 10, msg + 10, 50, a, 32);
    msg[59] ^= 1;
    wire_check(msg, 10, msg + 10, 50, b, 32);
    msg[59] ^= 1;
    OK(memcmp(a, b, 32) != 0, "the body's last byte is in the check");
    msg[0] ^= 1;
    wire_check(msg, 10, msg + 10, 50, b, 32);
    msg[0] ^= 1;
    OK(memcmp(a, b, 32) != 0, "the head's first byte is in the check");

    printf("wire_check: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

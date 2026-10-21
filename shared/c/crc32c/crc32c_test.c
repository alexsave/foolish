/* crc32c_test.c - CRC-32C (crc32c.h) against the published check value and
 * RFC 3720's iSCSI vectors, the instructions against the table over every
 * length and alignment, and a continued CRC against a whole one.
 *
 * Exits 1 on any failure. */
#include <stdint.h>
#include <string.h>
#include "../test/check.h"
#include "crc32c.h"
#include "../rng.h"

int main(void)
{
    TEST("crc32c: the check value and RFC 3720 B.4");
    CHECK(crc32c(0, "123456789", 9) == 0xE3069283u, "\"123456789\"");
    CHECK(crc32c_sw(0, "123456789", 9) == 0xE3069283u, "\"123456789\" (table)");
    uint8_t b[64];
    memset(b, 0, 32);
    CHECK(crc32c(0, b, 32) == 0x8A9136AAu, "32 zeros");
    memset(b, 0xff, 32);
    CHECK(crc32c(0, b, 32) == 0x62A8AB43u, "32 x 0xff");
    for (int i = 0; i < 32; i++) b[i] = (uint8_t)i;
    CHECK(crc32c(0, b, 32) == 0x46DD794Eu, "0..31");
    for (int i = 0; i < 32; i++) b[i] = (uint8_t)(31 - i);
    CHECK(crc32c(0, b, 32) == 0x113FDB5Cu, "31..0");
    CHECK(crc32c(0, b, 0) == 0, "no bytes");

    TEST("crc32c: the instructions are the table, at every length and offset");
    uint8_t r[300];
    uint64_t rng = 2463534242u;
    for (size_t i = 0; i < sizeof r; i++) r[i] = (uint8_t)rng_u32(&rng);
    int same = 0, total = 0;
    for (size_t off = 0; off < 9; off++)
        for (size_t n = 0; off + n <= sizeof r; n += 1 + n / 16, total++)
            same += crc32c(0, r + off, n) == crc32c_sw(0, r + off, n);
    CHECK(same == total, "%d of %d agree (hardware: %d)", same, total, (int)crc32c_hardware());

    TEST("crc32c: a CRC continues over more bytes");
    CHECK(crc32c(crc32c(0, r, 100), r + 100, 200) == crc32c(0, r, 300), "100 then 200");
    CHECK(crc32c(0, r, 300) != crc32c(0, r, 299), "a byte more changes it");

    return report("crc32c");
}

// crc32c.h - CRC-32C (Castagnoli, iSCSI's): a checksum that catches a torn or flipped
// byte, at the cost of a few instructions per 8 bytes where the core has them (arm64's
// CRC32 extension, x86-64's SSE4.2), and a table step per byte where it does not.
//
// crc32c(0, data, n) is the standard CRC-32C of the bytes ("123456789" -> 0xE3069283);
// passing a previous result as crc continues it over more bytes.
#ifndef SHARED_CRC32C_H
#define SHARED_CRC32C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t crc32c(uint32_t crc, const void *data, size_t n);
// The table form alone, the oracle the instructions are tested against.
uint32_t crc32c_sw(uint32_t crc, const void *data, size_t n);
// Whether crc32c uses the core's instructions here.
bool crc32c_hardware(void);

#endif

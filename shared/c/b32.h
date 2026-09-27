// Base32 (RFC 4648 upper-case alphabet, MSB-first bit packing, no padding):
// how a binary payload travels as text in a link.
//
// WHY BASE32 AND NOT BASE64. A link passes through things that fold case,
// re-encode `+` and `/`, and treat `=` as syntax. The 32-letter alphabet is
// letters and digits only, reads back the same in either case, and needs no
// escaping anywhere a URL can go.
//
// Decode accepts lower case, IGNORES characters outside the alphabet, and
// STOPS at '-' so a caller can hang a suffix off a code. Encode writes a
// NUL-terminated string. Both return the count written (bytes or chars, not
// counting the NUL), or -1 when `cap` is too small.
//
// Freestanding: no allocation, no libc.
#ifndef SHARED_B32_H
#define SHARED_B32_H

// The text length b32_encode writes for `n` bytes, not counting the NUL.
#define B32_LEN(n) (((n) * 8 + 4) / 5)

int b32_encode(const unsigned char *in, int n, char *out, int cap);
int b32_decode(const char *s, unsigned char *out, int cap);

#endif

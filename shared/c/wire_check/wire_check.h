// THE WIRE CHECK: the few bytes an envelope carries so a reader can refuse a
// payload that was cut, mangled or hand-edited before it ever parses the body.
//
//     out = the first check_len bytes of SHA-256(head || body)
//
// The check sits BETWEEN the head and the body on the wire, so it is computed
// over the two spans on either side of it, never over itself. A reader
// recomputes it over the same two spans and compares.
//
// THIS IS A WIRE FORMAT. Every byte it writes is in bubbles already sent, so
// the hash, the span order and "the leading bytes of the digest" are frozen;
// a product picks only how many bytes it carries (its own CHECK_LEN).
//
// Freestanding: no allocation, no libc beyond what sha256.c uses, so it
// compiles unchanged into every wasm module and static library that links it.
// In its own directory beside its test so the builds that wildcard
// shared/c/*.c pick up neither.
#ifndef SHARED_WIRE_CHECK_H
#define SHARED_WIRE_CHECK_H

#include <stddef.h>
#include <stdint.h>

// Write the first `check_len` bytes of SHA-256(head[0..head_len) ||
// body[0..body_len)) to `out`. Either span may be empty (its pointer is then
// never read and may be NULL). `check_len` above 32, the digest's length, is
// taken as 32: never more than the digest is written.
void wire_check(const void *head, size_t head_len, const void *body, size_t body_len,
                uint8_t *out, size_t check_len);

#endif

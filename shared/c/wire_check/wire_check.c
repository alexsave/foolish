// The wire check - see wire_check.h.
#include "wire_check.h"
#include "../sha256.h"
#include <string.h>

void wire_check(const void *head, size_t head_len, const void *body, size_t body_len,
                uint8_t *out, size_t check_len)
{
    uint8_t d[SHA256_DIGEST_LEN];
    Sha256 c;
    sha256_init(&c);
    if (head_len > 0) sha256_update(&c, head, head_len);
    if (body_len > 0) sha256_update(&c, body, body_len);
    sha256_final(&c, d);
    memcpy(out, d, check_len < SHA256_DIGEST_LEN ? check_len : SHA256_DIGEST_LEN);
}

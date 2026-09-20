#include "uttt_code.h"
#include <math.h>
#include <string.h>

#define CAP 48                     /* little-endian digits, base 256 */

static int collect(const UtttGame *g, uint8_t *idx, uint8_t *alpha)
{
    UtttGame r; uttt_init(&r);
    uint8_t list[81];
    for (int p = 0; p < g->n_plies; p++) {
        int n = uttt_legal(&r, list);
        if (n <= 0) return -1;
        int k = -1;
        for (int i = 0; i < n; i++) if (list[i] == g->move[p]) { k = i; break; }
        if (k < 0) return -1;
        idx[p]   = (uint8_t)k;
        alpha[p] = (uint8_t)n;
        if (!uttt_play(&r, g->move[p])) return -1;
    }
    return g->n_plies;
}

/* v = v * n + carry, little-endian. Returns 0 if it overflowed CAP. */
static int mul_add(uint8_t *v, int *len, uint32_t n, uint32_t carry)
{
    for (int i = 0; i < *len; i++) {
        uint32_t t = (uint32_t)v[i] * n + carry;
        v[i] = (uint8_t)(t & 0xff);
        carry = t >> 8;
    }
    while (carry) {
        if (*len >= CAP) return 0;
        v[(*len)++] = (uint8_t)(carry & 0xff);
        carry >>= 8;
    }
    return 1;
}

/* v /= n, returning the remainder. */
static uint32_t div_mod(uint8_t *v, int *len, uint32_t n)
{
    uint32_t rem = 0;
    for (int i = *len - 1; i >= 0; i--) {
        uint32_t cur = (rem << 8) | v[i];
        v[i] = (uint8_t)(cur / n);
        rem  = cur % n;
    }
    while (*len > 0 && v[*len - 1] == 0) (*len)--;
    return rem;
}

int uttt_encode(const UtttGame *g, uint8_t *buf, size_t cap)
{
    uint8_t idx[UTTT_MAX_PLIES], alpha[UTTT_MAX_PLIES];
    int np = collect(g, idx, alpha);
    if (np < 0) return -1;

    /* A leading 1 so the number's length is its own, and zero digits at the
     * top cannot be lost. It costs nothing measurable and it is what lets the
     * decoder trust the byte count it was handed. */
    uint8_t v[CAP]; int len = 1; v[0] = 1;

    /* backwards, so the decoder meets the digits in playing order */
    for (int p = np - 1; p >= 0; p--)
        if (!mul_add(v, &len, alpha[p], idx[p])) return -1;

    if ((size_t)len > cap) return -1;
    memcpy(buf, v, (size_t)len);
    return len;
}

int uttt_decode(UtttGame *out, const uint8_t *buf, size_t n)
{
    if (n == 0 || n > CAP) return 0;
    uint8_t v[CAP]; int len = (int)n;
    memcpy(v, buf, n);
    while (len > 0 && v[len - 1] == 0) len--;

    uttt_init(out);
    uint8_t list[81];
    for (;;) {
        int m = uttt_legal(out, list);
        if (m <= 0) break;
        uint32_t i = div_mod(v, &len, (uint32_t)m);
        if (i >= (uint32_t)m) return 0;
        if (!uttt_play(out, list[i])) return 0;
    }
    return len == 1 && v[0] == 1;          /* the sentinel, and nothing else */
}

double uttt_ideal_bits(const UtttGame *g)
{
    uint8_t idx[UTTT_MAX_PLIES], alpha[UTTT_MAX_PLIES];
    int np = collect(g, idx, alpha);
    if (np < 0) return -1;
    double b = 0;
    for (int p = 0; p < np; p++) b += log2((double)alpha[p]);
    return b;
}

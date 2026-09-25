#include "uttt_code.h"
#include "../../../shared/c/b32.h"
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

    /* A leading 1 so the number's length is its own and top zero digits
     * cannot be lost. */
    uint8_t v[CAP]; int len = 1; v[0] = 1;

    /* backwards, so the decoder meets the digits in playing order */
    for (int p = np - 1; p >= 0; p--)
        if (!mul_add(v, &len, alpha[p], idx[p])) return -1;

    /* THE PLY COUNT GOES LAST, so it comes out FIRST.
     *
     * Without it the format is ambiguous, and only for games that have not
     * been played out - which is every game in a live thread. A ply with one
     * legal move carries no information, so it costs no digits; a decoder
     * therefore cannot tell a position that STOPPED at a forced move from one
     * that PLAYED it. Trying to infer the end from the value returning to its
     * sentinel gets the common case right and those two wrong.
     *
     * One digit in base 82 - about six and a half bits, under a byte on
     * twenty-one - buys an unambiguous length. Correctness is worth the byte.
     */
    if (!mul_add(v, &len, UTTT_MAX_PLIES + 1, (uint32_t)np)) return -1;

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
    int np = (int)div_mod(v, &len, UTTT_MAX_PLIES + 1);
    if (np > UTTT_MAX_PLIES) return 0;
    for (int p = 0; p < np; p++) {
        int m = uttt_legal(out, list);
        if (m <= 0) return 0;               /* the length lied */
        /* a forced move cost no digit on the way in, so it takes none out */
        uint32_t i = (m == 1) ? 0 : div_mod(v, &len, (uint32_t)m);
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

/* ---------------------------------------------------------- the link */

#define SEED_BYTES 4

int uttt_replay_url(const UtttGame *g, int32_t seed, char *out, int cap)
{
    static const char pre[] = UTTT_REPLAY_PREFIX;
    const int np = (int)sizeof pre - 1;
    if (!g || g->n_plies == 0 || cap <= np) return -1;
    uint8_t b[SEED_BYTES + 64];
    uint32_t u = (uint32_t)seed;
    b[0] = (uint8_t)(u >> 24); b[1] = (uint8_t)(u >> 16); b[2] = (uint8_t)(u >> 8); b[3] = (uint8_t)u;
    int n = uttt_encode(g, b + SEED_BYTES, sizeof b - SEED_BYTES);
    if (n <= 0) return -1;
    memcpy(out, pre, (size_t)np);
    int w = b32_encode(b, SEED_BYTES + n, out + np, cap - np);
    return w < 0 ? -1 : np + w;
}

int uttt_replay_read(const char *url, UtttGame *out, int32_t *seed)
{
    static const char pre[] = UTTT_REPLAY_PREFIX;
    const size_t np = sizeof pre - 1;
    if (!url) return 0;
    static const char old[] = UTTT_REPLAY_PREFIX_OLD;
    if (strncmp(url, pre, np) == 0) url += np;
    else if (strncmp(url, old, sizeof old - 1) == 0) url += sizeof old - 1;
    /* the code alone: b32_decode skips what is not in its alphabet, so a
     * trailing "/", "?x=1" or "#..." would be read as more code - cut it */
    char code[128];
    size_t k = 0;
    while (url[k] && url[k] != '/' && url[k] != '?' && url[k] != '#') {
        if (k + 1 >= sizeof code) return 0;
        code[k] = url[k];
        k++;
    }
    code[k] = 0;
    if (k == 0) return 0;
    uint8_t b[80];
    int n = b32_decode(code, b, sizeof b);
    /* shorter than the seed is not a link; the seed with nothing after it
     * is refused by uttt_decode (no bytes), which the test holds */
    if (n < SEED_BYTES) return 0;
    if (!uttt_decode(out, b + SEED_BYTES, (size_t)(n - SEED_BYTES))) return 0;
    if (seed)
        *seed = (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
    return 1;
}

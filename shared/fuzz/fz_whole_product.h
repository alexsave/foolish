/* One fuzz target for a product whose message is shared/c/msg_whole's envelope
 * (pickemup, tallybones, chuiniu). The including file defines, before this:
 *   P_TYPE, P_DECODE, P_ENCODE, P_TDECODE, P_TENCODE, P_MAXB, P_MAXT, P_EOK
 * and a `static void fz_seeds_product(const char *dir)`.
 * Properties: a decoder never crashes, and a message it accepts re-encodes to
 * something it accepts again (a host cannot emit a payload it would refuse).
 */
#include "fz.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static P_TYPE m, back;
    static uint8_t out[P_MAXB + 64];
    static char text[P_MAXT + 64], enc[P_MAXT + 64];
    if (n > P_MAXB + 64) n = P_MAXB + 64;
    if (P_DECODE(d, (int)n, &m) == P_EOK) {
        int k = P_ENCODE(&m, out, (int)sizeof out);
        if (k > 0 && P_DECODE(out, k, &back) != P_EOK) abort();
        if (P_TENCODE(&m, enc, (int)sizeof enc) > 0 && P_TDECODE(enc, &back) != P_EOK) abort();
    }
    P_TDECODE(fz_text(d, n, text, sizeof text), &m);
    return 0;
}

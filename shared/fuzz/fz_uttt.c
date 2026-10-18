/* uttt's message bytes (utm_decode, utm_text_decode) and the big-game
 * header and link readers (utb_msg_peek, utb_msg_text_peek, utb_msg_text_bytes,
 * utb_msg_decode against an all-blank board). */
#include "../../uttt/c/src/uttt_big_msg.h"
#include "../../uttt/c/src/uttt_msg.h"
#include "fz.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static UtmMsg m, back;
    static UtbHead head;
    static UtbMsg big;
    static uint8_t out[UTM_MAX_BYTES + UTB_MAX_BYTES + 64], cells[UTB_CELLS], raw[UTB_MAX_BYTES + 64];
    static char text[UTM_MAX_TEXT + UTB_MAX_TEXT + 64], enc[UTM_MAX_TEXT + 64];
    if (n > UTM_MAX_BYTES + UTB_MAX_BYTES + 64) n = UTM_MAX_BYTES + UTB_MAX_BYTES + 64;
    if (utm_decode(d, (int)n, &m) == UTM_EOK) {
        int k = utm_encode(&m, out, (int)sizeof out);
        if (k > 0 && utm_decode(out, k, &back) != UTM_EOK) abort();
        if (utm_text_encode(&m, enc, (int)sizeof enc) > 0 && utm_text_decode(enc, &back) != UTM_EOK) abort();
    }
    fz_text(d, n, text, sizeof text);
    utm_text_decode(text, &m);
    utb_msg_peek(d, (int)n, &head);
    utb_msg_decode(d, (int)n, cells, &big);
    utb_msg_text_peek(text, &head);
    utb_msg_text_decode(text, cells, &big);
    utb_msg_text_bytes(text, raw, (int)sizeof raw);
    return 0;
}

#ifdef FZ_SEEDS
void fz_seeds(const char *dir)
{
    static UtmMsg m;
    uint8_t a[UTM_TAG_LEN], b[UTM_TAG_LEN], buf[UTM_MAX_BYTES];
    int32_t seed = 123457;
    utm_tag(seed, (const uint8_t *)"alex", 4, a);
    utm_tag(seed, (const uint8_t *)"vera", 4, b);
    utm_open(&m, seed, 9, a);
    int k = utm_encode(&m, buf, sizeof buf);
    if (k > 0) fz_put(dir, "invite", buf, k);
    for (int ply = 0; ply < 12 && !m.game.over; ply++) {
        uint8_t list[81];
        int cnt = uttt_legal(&m.game, list);
        if (cnt <= 0 || utm_play(&m, m.game.n_plies % 2 == 0 ? b : a, list[(ply * 7) % cnt])) break;
    }
    if ((k = utm_encode(&m, buf, sizeof buf)) > 0) {
        fz_put(dir, "midgame", buf, k);
        char text[UTM_MAX_TEXT];
        if ((k = utm_text_encode(&m, text, sizeof text)) > 0) fz_put(dir, "midgame_text", text, k);
    }
    static UtbMsg big;
    static uint8_t bb[UTB_MAX_BYTES];
    utb_msg_open(&big, seed, 88, a);
    if ((k = utb_msg_encode(&big, bb, sizeof bb)) > 0) fz_put(dir, "big_invite", bb, k);
}
#endif

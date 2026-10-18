/* shared/c/msg_whole itself, below any product's decoder: the envelope reader
 * (msg_whole_read, under pickemup's spec), the link-text reader and the seat
 * tag lookup. Forwarded bubbles are untrusted, and this is the first code
 * three products run on one. */
#include "../../pickemup/c/src/pk_msg.h"
#include "fz.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static MsgWhole w;
    static uint8_t bytes[PK_MSG_MAX_BYTES + 64], tag[MSG_SEAT_TAG_LEN];
    static char span[PK_MSG_MAX_TEXT + 64], text[PK_MSG_MAX_TEXT + 64], put[PK_MSG_MAX_TEXT + 64];
    int flags = 0, body_at = 0, body_len = 0;
    uint16_t counters[MSG_WHOLE_MAX_COUNTERS] = { 0 };
    if (n > PK_MSG_MAX_BYTES + 64) n = PK_MSG_MAX_BYTES + 64;
    if (msg_whole_read(&PK_MSG_SPEC, d, (int)n, &w, &flags, counters, &body_at, &body_len) == MSG_WHOLE_EOK) {
        if (body_at < 0 || body_len < 0 || body_at + body_len > (int)n) abort();
        memcpy(tag, w.seat[0].tag, sizeof tag);
        (void)msg_whole_seat_of_tag(&w, tag);
        (void)msg_whole_started(&w);
    }
    int k = msg_whole_text_get(fz_text(d, n, text, sizeof text), span, (int)sizeof span, bytes, (int)sizeof bytes);
    if (k > 0 && msg_whole_text_put(bytes, k, put, (int)sizeof put) < 0) { /* ECAP is allowed */ }
    return 0;
}

#ifdef FZ_SEEDS
void fz_seeds(const char *dir)
{
    static PkMsg m;
    uint8_t seed[32], tag[PK_TAG_LEN], buf[PK_MSG_MAX_BYTES];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 11 + 1);
    pk_tag(seed, (const uint8_t *)"id-Alex", 7, tag);
    pk_msg_new(&m, seed, 0, tag, (const uint8_t *)"Alex", 4);
    pk_tag(seed, (const uint8_t *)"id-Vera", 7, tag);
    pk_msg_join(&m, tag, (const uint8_t *)"Vera", 4);
    int k = pk_msg_encode(&m, buf, sizeof buf);
    if (k > 0) fz_put(dir, "lobby", buf, k);
    char text[PK_MSG_MAX_TEXT];
    if (k > 0 && (k = msg_whole_text_put(buf, k, text, sizeof text)) > 0) fz_put(dir, "lobby_text", text, k);
}
#endif

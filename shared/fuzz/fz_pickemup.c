/* pickemup's message bytes and link text (pk_msg_decode / pk_msg_text_decode). */
#include "../../pickemup/c/src/pk_msg.h"
#define P_TYPE    PkMsg
#define P_DECODE  pk_msg_decode
#define P_ENCODE  pk_msg_encode
#define P_TDECODE pk_msg_text_decode
#define P_TENCODE pk_msg_text_encode
#define P_MAXB    PK_MSG_MAX_BYTES
#define P_MAXT    PK_MSG_MAX_TEXT
#define P_EOK     PK_EOK
#include "fz_whole_product.h"

#ifdef FZ_SEEDS
void fz_seeds(const char *dir)
{
    static PkMsg m;
    uint8_t seed[32], tag[PK_TAG_LEN], buf[PK_MSG_MAX_BYTES];
    const char *names[] = { "Alex", "Vera", "Cleo", "Bo" };
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 3);
    for (int n = 2; n <= 4; n += 2) {
        pk_tag(seed, (const uint8_t *)"id-Alex", 7, tag);
        pk_msg_new(&m, seed, n == 2, tag, (const uint8_t *)names[0], 4);
        char name[16];
        for (int s = 1; s < n; s++) {
            snprintf(name, sizeof name, "id-%s", names[s]);
            pk_tag(seed, (const uint8_t *)name, (int)strlen(name), tag);
            pk_msg_join(&m, tag, (const uint8_t *)names[s], (int)strlen(names[s]));
        }
        int k = pk_msg_encode(&m, buf, sizeof buf);
        if (k > 0) fz_put(dir, n == 2 ? "lobby2" : "lobby4", buf, k);
        if (pk_msg_start(&m, 0) == PK_EOK && (k = pk_msg_encode(&m, buf, sizeof buf)) > 0)
            fz_put(dir, n == 2 ? "started2" : "started4", buf, k);
    }
}
#endif

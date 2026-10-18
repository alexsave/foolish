/* tallybones's message bytes and link text (tb_msg_decode / tb_msg_text_decode). */
#include "../../tallybones/c/src/tb_msg.h"
#define P_TYPE    TbMsg
#define P_DECODE  tb_msg_decode
#define P_ENCODE  tb_msg_encode
#define P_TDECODE tb_msg_text_decode
#define P_TENCODE tb_msg_text_encode
#define P_MAXB    TB_MSG_MAX_BYTES
#define P_MAXT    TB_MSG_MAX_TEXT
#define P_EOK     TB_EOK
#include "fz_whole_product.h"

#ifdef FZ_SEEDS
void fz_seeds(const char *dir)
{
    static TbMsg m;
    uint8_t seed[32], tag[TB_TAG_LEN], buf[TB_MSG_MAX_BYTES];
    const char *names[] = { "Alex", "Vera", "Cleo", "Bo" };
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 3);
    for (int n = 2; n <= 4; n += 2) {
        tb_tag(seed, (const uint8_t *)"id-Alex", 7, tag);
        tb_msg_new(&m, seed, n == 2, tag, (const uint8_t *)names[0], 4);
        char name[16];
        for (int s = 1; s < n; s++) {
            snprintf(name, sizeof name, "id-%s", names[s]);
            tb_tag(seed, (const uint8_t *)name, (int)strlen(name), tag);
            tb_msg_join(&m, tag, (const uint8_t *)names[s], (int)strlen(names[s]));
        }
        int k = tb_msg_encode(&m, buf, sizeof buf);
        if (k > 0) fz_put(dir, n == 2 ? "lobby2" : "lobby4", buf, k);
        if (tb_msg_start(&m, 0) == TB_EOK && (k = tb_msg_encode(&m, buf, sizeof buf)) > 0)
            fz_put(dir, n == 2 ? "started2" : "started4", buf, k);
    }
}
#endif

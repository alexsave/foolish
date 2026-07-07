#include "awire.h"
#include "../wasm/wire.h"

int awire_decode(const unsigned char *buf, int len, AwireAction *out) {
    if (len < 2) return 0;
    const int kind = buf[0];
    const int n = buf[1];
    if (kind < AWIRE_ATTACK || kind > AWIRE_GOOD) return 0;
    if (n > AWIRE_MAX_CARDS) return 0;
    if ((kind == AWIRE_PICKUP || kind == AWIRE_GOOD) && n != 0) return 0;
    const int expected = 2 + n * (kind == AWIRE_COVER ? 2 : 1);
    if (len != expected) return 0;
    out->kind = kind;
    out->n = n;
    for (int i = 0; i < n; i++) out->cards[i] = card_from_wire_state(buf[2 + i]);
    if (kind == AWIRE_COVER) {
        for (int i = 0; i < n; i++) out->attacks[i] = card_from_wire_state(buf[2 + n + i]);
    }
    return 1;
}

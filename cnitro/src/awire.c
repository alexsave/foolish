#include "awire.h"
#include "../wasm/wire.h"

// The shared head check: validates kind/n and returns the frame's total length,
// or 0. Both the exact-length decode and the stream walk are this plus a length
// comparison, so the two can never disagree about what a frame is.
static int awire_head_len(const unsigned char *buf, int len) {
    if (len < 2) return 0;
    const int kind = buf[0];
    const int n = buf[1];
    if (kind < AWIRE_ATTACK || kind > AWIRE_GOOD) return 0;
    if (n > AWIRE_MAX_CARDS) return 0;
    if ((kind == AWIRE_PICKUP || kind == AWIRE_GOOD) && n != 0) return 0;
    return 2 + n * (kind == AWIRE_COVER ? 2 : 1);
}

int awire_frame_len(const unsigned char *buf, int len) {
    const int want = awire_head_len(buf, len);
    if (want == 0 || want > len) return 0;
    return want;
}

int awire_decode(const unsigned char *buf, int len, AwireAction *out) {
    const int expected = awire_head_len(buf, len);
    if (expected == 0 || len != expected) return 0;
    const int kind = buf[0];
    const int n = buf[1];
    out->kind = kind;
    out->n = n;
    for (int i = 0; i < n; i++) out->cards[i] = card_from_wire_state(buf[2 + i]);
    if (kind == AWIRE_COVER) {
        for (int i = 0; i < n; i++) out->attacks[i] = card_from_wire_state(buf[2 + n + i]);
    }
    return 1;
}

int awire_encode(const AwireAction *a, unsigned char *buf, int cap) {
    if (a->kind < AWIRE_ATTACK || a->kind > AWIRE_GOOD) return 0;
    if (a->n < 0 || a->n > AWIRE_MAX_CARDS) return 0;
    if ((a->kind == AWIRE_PICKUP || a->kind == AWIRE_GOOD) && a->n != 0) return 0;
    const int want = 2 + a->n * (a->kind == AWIRE_COVER ? 2 : 1);
    if (want > cap) return 0;
    buf[0] = (unsigned char)a->kind;
    buf[1] = (unsigned char)a->n;
    for (int i = 0; i < a->n; i++) buf[2 + i] = wire_from_card(a->cards[i]);
    if (a->kind == AWIRE_COVER) {
        for (int i = 0; i < a->n; i++) buf[2 + a->n + i] = wire_from_card(a->attacks[i]);
    }
    return want;
}

/* foolish's on-device byte readers: the FMSG envelope (msg_decode, then
 * msg_chain_key and the v6 body through replay_decode), the bare replay
 * code, and the action wire (awire_decode). Seeds are the lobby and the
 * started handoff envelopes msg_wire_test encodes. */
#include "../../foolish/c/src/awire.h"
#include "../../foolish/c/src/msg_wire.h"
#include "../../foolish/c/src/replay.h"
#include "fz.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static MsgEnvelope e;
    static MsgChainKey key;
    static AwireAction a;
    static unsigned char dec[1 << 16];
    if (n > 4096) n = 4096;
    if (msg_decode(d, (int)n, &e) == MSG_EOK) {
        (void)msg_chain_key(d, (int)n, &key);
        if (e.actions && e.actions_len > 0) (void)replay_decode(e.actions, e.actions_len, dec, (int)sizeof dec);
    }
    (void)replay_decode(d, (int)n, dec, (int)sizeof dec);
    (void)awire_decode(d, (int)n, &a);
    return 0;
}

#ifdef FZ_SEEDS
static void env(MsgEnvelope *e, int players, int phase)
{
    msg_envelope_init(e);
    e->format = MSG_FORMAT_V6;
    e->phase = (uint8_t)phase;
    e->game_id = 0x0123456789abcdefULL;
    e->n_players = (uint8_t)players;
    for (int i = 0; i < MSG_SEED_LEN; i++) e->seed[i] = (uint8_t)(i * 13 + 5);
    static const char *names[] = { "Alex", "Vera", "Cleo", "Bo", "Dmitri" };
    e->n_joins = (uint8_t)players;
    for (int i = 0; i < players; i++) {
        e->joins[i].seat = (uint8_t)i;
        e->joins[i].name_len = (uint8_t)strlen(names[i % 5]);
        memcpy(e->joins[i].name, names[i % 5], e->joins[i].name_len);
    }
}

void fz_seeds(const char *dir)
{
    MsgEnvelope e;
    unsigned char wire[4096];
    int k;
    env(&e, 1, MSG_PHASE_WAITING);
    if ((k = msg_encode(&e, wire, sizeof wire)) > 0) fz_put(dir, "lobby", wire, k);
    env(&e, 5, MSG_PHASE_LIVE);
    if ((k = msg_encode(&e, wire, sizeof wire)) > 0) fz_put(dir, "live", wire, k);
    env(&e, 2, MSG_PHASE_LIVE);
    if ((k = msg_encode(&e, wire, sizeof wire)) > 0) fz_put(dir, "live2", wire, k);
}
#endif

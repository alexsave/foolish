#ifndef FOOLYARD_CLIENT_H
#define FOOLYARD_CLIENT_H

#include "types.h"
#include "constants.h"

#include "game.h"
#include "awire.h"

typedef struct ClientLink {
    u32 up_us, down_us;   // one-way latency, each direction
    u32 jitter_us;        // uniform [0, jitter_us) per hop; this is what reorders
    u32 loss_pct;
    u32 dup_pct;
    // 1 = a stream (/ws over TCP): frames cannot pass each other, and a late
    // one blocks the ones behind it. 0 = datagrams (the WebTransport front-end
    // pushes state as QUIC DATAGRAMs), where jitter really does reorder.
    u8  ordered;
} ClientLink;

#define CLI_PRIV_BYTES 64
#define CLI_RECONNECT_BACKOFF_US (1500 * 1000ull)

typedef struct ClientState {
    u8  used;
    u8  tier;
    u8  seat;
    u8  connected;
    u8  no_push;    // never subscribes; reads with polls, like the pre-/ws path
    u16 game_id;
    u16 id;

    ClientLink link;
    u64 up_next_us, down_next_us;   // ordered links only: the head-of-line cursor
    u32 think_us;
    u32 think_jitter_us;

    u8  view[VIEW_MAX];
    u16 view_len;
    u32 view_version;
    u32 view_deal;      // which deal the held view belongs to
    u8  have_view;
    u64 last_view_us;

    // The hand the last accepted view showed, for the phantom-loss detector,
    // plus enough context to know when a difference is legitimate: our own
    // outstanding move, or a fresh deal.
    u8  hand[MAX_HAND_SIZE];
    u8  hand_n;
    u32 hand_seq;
    u32 hand_version;
    u32 hand_deal;

    // A move decided but not yet on the wire. Several tiers sit on one: that
    // gap is where a decision goes stale.
    AwireAction pending;
    u8  has_pending;
    u32 pending_version;

    u8  last_frame[64];   // AWIRE_MAX_CARDS*2 + 2, the widest frame there is
    u8  last_frame_len;

    u8  reconnect_pending;
    u32 seq;            // moves sent
    u32 acked_seq;      // last move the server confirmed applied
    u8  wake_armed;

    u64 rng;
    u8  priv[CLI_PRIV_BYTES];
} ClientState;

typedef struct ClientCtx {
    struct World *w;
    ClientState  *cs;
    u64 now_us;

    const Game *view;      // decoded masked view, NULL on a bare wake
    u32  view_version;
    int  seat;
    u8   is_ack;
    u8   ack_ok;

    u8  want_send;
    AwireAction out_move;
    u32 out_version;   // the view the move was decided against; defaults to the
                       // one that just arrived, a delaying tier overrides it
    u8  want_wake;
    u64 wake_delay_us;
} ClientCtx;

typedef struct ClientImpl {
    const char *name;
    void (*settings)(ClientState *cs);
    void (*on_view)(ClientCtx *ctx);
    void (*on_wake)(ClientCtx *ctx);
} ClientImpl;

// One entry per clients/*.c file.
#define CLIENT_TIERS \
    X(wellbehaved) \
    X(laggy) \
    X(reconnect) \
    X(resender) \
    X(stale) \
    X(poller) \
    X(griefer) \
    X(datagram)

#define X(name) extern const ClientImpl client_##name;
CLIENT_TIERS
#undef X

const ClientImpl *client_impl(int tier);
int client_tier_count(void);

// Shared helper: pick a uniformly random legal move for `seat` in `view`.
// Returns 0 if the seat has nothing to do.
int client_pick_legal(const Game *view, int seat, u64 *rng, AwireAction *out);

void client_on_packet(struct World *w, u32 pkt_id);
void client_on_wake(struct World *w, u32 client_id);
void client_submit(struct World *w, ClientState *cs, const AwireAction *a, u32 chosen_at);
void client_retransmit(struct World *w, ClientState *cs);
void client_subscribe(struct World *w, ClientState *cs);   // the /ws upgrade
void client_poll(struct World *w, ClientState *cs);        // a GET /state
void client_disconnect(struct World *w, ClientState *cs);
void client_link_reset(struct World *w, ClientState *cs);   // the socket died

#endif

// wire.h - THE BINARY SURFACE between birdseye and its site: every answer and every feed frame
// is one of these structs, written by memcpy from C and read by the TypeScript structgen
// generates from this very header (web/src/gen/wire.ts), so no TypeScript spells a byte offset.
// Fixed-width little-endian fields only, natural alignment; the layout hash structgen prints is
// compiled into the server (BIRDSEYE_WIRE_HASH) and stamped into every answer's head, so a page built
// from another layout refuses the bytes instead of reading garbage.
//
//   GET /be/tables           be_w_list, then n x be_w_table (every table the sim holds: live and
//                            recently finished)
//   GET /be/table/<gid>      be_w_detail (the table, every known hand, the last events)
//   GET /be/ops              be_w_ops
//   POST /be/session         be_w_session: a token for the stream (dev mode)
//   GET /ws (the platform's stream, shared/c/stream_frame.h): HELLO with that token, then SUB
//                            what NW_SUB_GAME, room "all"; every push is one frame: [BE_F_TABLE]
//                            be_w_table on every change, [BE_F_EVENT] be_w_event as each happens,
//                            [BE_F_OPS] be_w_ops once a second
#ifndef BIRDSEYE_WIRE_H
#define BIRDSEYE_WIRE_H

#include <stdint.h>

#define BE_GID         16   // a game id (12 hex) and room
#define BE_NAME        24   // a seat's name as shown
#define BE_SEATS       8    // the kernel's MAX_PLAYERS
#define BE_BATTLES     12   // battles the birds-eye shows (a bout never exceeds a hand)
#define BE_HAND        52   // the largest hand (the whole 52-card deck)
#define BE_EVENTS      32   // the detail's event tail
#define BE_EVENT_CARDS 8

// be_w_table.phase
#define BE_PHASE_LOBBY    0   // created, seats still arriving or readying
#define BE_PHASE_PLAYING  1
#define BE_PHASE_OVER     2   // the fool is known; the seats linger, then leave or rematch
#define BE_PHASE_GONE     3   // every seat left: the server deleted it (kept a while for the site)

// be_w_event.kind
#define BE_EV_NEW      1    // the table was created
#define BE_EV_DEALT    2    // the game dealt (lobby to playing)
#define BE_EV_ATTACK   3    // seat laid n cards
#define BE_EV_COVER    4
#define BE_EV_PASS     5    // the defender passed the attack on (classic rules)
#define BE_EV_PICKUP   6    // the defender took the table
#define BE_EV_GOOD     7    // the bout closed, the table went to the discard
#define BE_EV_OUT      8    // seat went out
#define BE_EV_OVER     9    // the game ended: seat is the fool (-1: a draw)
#define BE_EV_REMATCH  10   // the table went back to its lobby
#define BE_EV_GONE     11   // the table was deleted
#define BE_EV_REFUSED  12   // the server refused a legitimate move (a sim bug or a server bug)
#define BE_EV_HOSTILE  13   // a hostile move: refused (cards[0].value is the hostile kind), or ACCEPTED when `refused` is 0
#define BE_EV_STALL    14   // nobody moved for a long time
#define BE_EV_RESYNC   15   // a seat refetched its game (a cast it could not apply)

// be_w_event.cards[0].value on BE_EV_HOSTILE
#define BE_HOSTILE_FOREIGN_CARD 1   // a card the seat does not hold
#define BE_HOSTILE_MALFORMED    2   // bytes that are not an action wire
#define BE_HOSTILE_OVERSIZE     3   // an attack of 28 copies of one card (the frame's capacity)
#define BE_HOSTILE_WRONG_TABLE  4   // a move sent for a table the seat is not at
#define BE_HOSTILE_N            4

// be_w_table.seats[i].flags
#define BE_SEAT_HUMAN_SIM 0x1   // one of the sim's fake humans
#define BE_SEAT_BOT       0x2   // one of the server's bots
#define BE_SEAT_TO_MOVE   0x4   // the seat may act on this board (the kernel's turn rule)
#define BE_SEAT_GOOD      0x8   // the seat has said Good this bout
#define BE_SEAT_OUT       0x10  // the seat went out
#define BE_SEAT_FOOL      0x20

// A card: the kernel's suit (0 spades, 1 hearts, 2 clubs, 3 diamonds) and value (1..13, ace
// 13); -1/-1 a hidden card, -2/-2 none.
typedef struct {
    int8_t suit;
    int8_t value;
} be_w_card;

typedef struct {
    uint8_t   name_len;
    char      name[BE_NAME];
    uint8_t   flags;       // BE_SEAT_*
    uint8_t   status;      // PLAYER_STATUS_*
    uint8_t   hand_count;
    uint8_t   brain_len;   // a bot's strategy key, else empty
    char      brain[12];
} be_w_seat;

typedef struct {
    uint8_t   gid_len;
    char      gid[BE_GID];
    uint8_t   phase;          // BE_PHASE_*
    uint8_t   n_seats;        // the seats the sim planned for this table
    uint8_t   num_players;    // seated now
    uint8_t   podkidnoy;      // 1: no passing
    int8_t    power_suit;     // -1 before the deal
    uint8_t   has_flipped;    // the trump card is still under the stock
    be_w_card flipped;
    int8_t    first_attacker;
    int8_t    defender;
    int8_t    fool;           // -1 until the game is over (and for a draw)
    uint8_t   num_battles;    // on the table now (at most BE_BATTLES shown)
    int16_t   deck_count;
    int16_t   discard_count;
    uint8_t   last_kind;      // BE_EV_* of the last event
    int8_t    last_seat;
    uint32_t  version;        // the committed version this row is of
    uint32_t  moves;          // moves the sim's seats made at this table (this game)
    uint32_t  last_act_us;    // the last move's request-to-reply latency
    int64_t   created_ms;
    int64_t   changed_ms;
    int64_t   finished_ms;    // 0 until over
    be_w_card attack[BE_BATTLES];
    be_w_card defense[BE_BATTLES];
    be_w_seat seats[BE_SEATS];
} be_w_table;

typedef struct {
    uint8_t   gid_len;
    char      gid[BE_GID];
    uint8_t   kind;        // BE_EV_*
    int8_t    seat;        // -1 none
    uint8_t   n_cards;
    uint8_t   refused;     // BE_EV_HOSTILE, BE_EV_REFUSED: the server said no
    be_w_card cards[BE_EVENT_CARDS];
    uint32_t  version;
    uint32_t  lat_us;      // a move's request-to-reply latency, 0 when not a move
    int64_t   at_ms;
} be_w_event;

// A hand the sim knows: its own humans' (a bot's is n 0: the server never shows it).
typedef struct {
    uint8_t   n;
    be_w_card cards[BE_HAND];
} be_w_hand;

typedef struct {
    be_w_table table;
    be_w_hand  hands[BE_SEATS];
    uint8_t    n_events;
    be_w_event events[BE_EVENTS];      // oldest first
} be_w_detail;

typedef struct {
    uint32_t hash;
    uint32_t uptime_s;
    uint32_t tables_live;        // lobby + playing + over, still on the server
    uint32_t tables_lobby;
    uint32_t tables_playing;
    uint32_t games_total;        // games finished since start
    uint32_t moves_total;        // moves the sim sent
    uint32_t refused_total;      // legitimate moves the server refused
    uint32_t hostile_total;
    uint32_t hostile_accepted;   // hostile moves the server took (each one is a finding)
    uint32_t resyncs_total;      // games refetched after a cast could not be applied
    uint32_t stalls_total;
    uint32_t players;            // fake humans
    uint32_t players_seated;
    uint32_t sockets;            // open /ws sockets to the game server
    uint32_t bots_seated;
    uint32_t threads;
    uint32_t server_rss_mb;      // the game server's resident set (0 when unknown)
    uint32_t self_rss_mb;        // this process's
    uint32_t server_cpu_pct;     // the game server's CPU over the last second, percent of one core
    float    games_per_s;
    float    moves_per_s;
    float    pushes_per_s;       // frames heard on every socket
    float    push_bytes_per_s;
    float    req_per_s;          // requests sent (moves, asks, creates, reads)
    uint32_t act_p50_us;         // a move: request out to reply in
    uint32_t act_p99_us;
    uint32_t act_max_us;
    uint32_t push_p50_us;        // a move: request out to the mover's own cast of it in
    uint32_t push_p99_us;
    uint32_t cast_p50_us;        // a cast's arrival to applied on the client table (decode cost)
    uint32_t cast_p99_us;
    uint32_t req_p50_us;         // every request that is not a move (asks, creates, reads)
    uint32_t req_p99_us;
    uint32_t game_p50_s;         // a finished game's length, deal to fool
    uint32_t game_p99_s;
    uint32_t act_n;              // samples behind the move percentiles
} be_w_ops;

typedef struct {
    uint32_t hash;
    uint32_t n;
    uint32_t first;  // where the first record starts (sizeof this head)
    uint32_t size;   // bytes per record that follows (sizeof the record struct of this build)
} be_w_list;

// POST /be/session (dev mode only): the site's token for the stream's HELLO, a session of a fixed
// site account on this process's own store, made through the platform's auth.
#define BE_TOKEN_MAX 2048
typedef struct {
    uint32_t hash;
    uint16_t token_len;
    char     token[BE_TOKEN_MAX];
} be_w_session;

// A push on the stream is [SF_V][BE_F_*][the record]: the socket's version byte first, so the
// hub's client tells a push from the socket's own frames (stream_frame.h sf_is_push), then the kind.
#define BE_SF_V    1   // shared/c/stream_frame.h SF_V; a _Static_assert in birdseye_app.c holds them equal
#define BE_F_TABLE 1
#define BE_F_EVENT 2
#define BE_F_OPS   3

// The layout hash structgen printed for this header, compiled in (the Makefile).
#ifndef BIRDSEYE_WIRE_HASH
#define BIRDSEYE_WIRE_HASH 0u
#endif
static inline uint32_t be_wire_hash(void) { return (uint32_t)BIRDSEYE_WIRE_HASH; }

#endif

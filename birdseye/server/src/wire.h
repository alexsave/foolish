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
//   GET /metrics             the platform's report of what birdseye counts (c_base/core/src/metrics.h
//                            cb_metrics_hdr then cb_metric_row x n; the names are be_metrics.h's)
//   POST /be/session         be_w_session: a token for the stream (dev mode)
//   GET /be/league           be_w_league, then n_bots x be_w_bot, then n_results x be_w_result: the
//                            bot league's standings and every rated game it has seen, oldest first
//   GET /ws (the platform's stream, shared/c/stream_frame.h): HELLO with that token, then SUB
//                            what NW_SUB_GAME, room "all"; every push is one frame: [BE_F_TABLE]
//                            be_w_table on every change, [BE_F_EVENT] be_w_event as each happens,
//                            [BE_F_OPS] the same report GET /metrics answers, once a second,
//                            [BE_F_RESULT] be_w_result as a league game is rated, [BE_F_BOT]
//                            be_w_bot when a bot's standing changes, [BE_F_TOUR] be_w_tour
//   GET /be/tournament       be_w_tour: the stored tournament (state BE_TOUR_IDLE when none)
#ifndef BIRDSEYE_WIRE_H
#define BIRDSEYE_WIRE_H

#include <stdint.h>

#include "store/game_id.h"   // the game id rule (foolish/server/src/store): its length is BE_GID

#define BE_GID         CB_GAME_ID_LEN   // a game id (store/game_id.h) and its room
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
    uint32_t n;
    uint32_t first;  // where the first record starts (sizeof this head)
    uint32_t size;   // bytes per record that follows (sizeof the record struct of this build)
} be_w_list;

// ---- the bot league (league.h) --------------------------------------------------------------------
//
// Every number here is the game server's: a rating is a finished game's rating stamp (the kernel's
// table_rate, read from the game's envelope: before and delta by seat) or the server's leaderboard;
// nothing is worked out in birdseye or on the page.

#define BE_LEAGUE_BOTS 64
#define BE_BOT_ID      40
#define BE_BOT_KEY     16

typedef struct {
    uint8_t  id_len;
    char     id[BE_BOT_ID];
    uint8_t  name_len;
    char     name[BE_NAME];
    uint8_t  key_len;      // the strategy family (GET /v1/bots strategy_key)
    char     key[BE_BOT_KEY];
    uint8_t  seated;       // live league tables it sits at now
    int32_t  rating;       // the server's: the last stamp's before + delta, or the leaderboard's
    int32_t  start_rating; // the server's when this league first saw the bot
    uint32_t games;        // the server's games_played
    uint32_t league_games; // rated games the league has seen this bot play
    uint32_t fools;        // of those, the ones it was the fool of
    uint32_t firsts;       // of those, the ones it went out first in
    int16_t  last_delta;   // the last game's change (0 before any)
    int64_t  last_ms;      // when that game was rated (wall clock, 0 before any)
} be_w_bot;

// One rated league game. bot[i] indexes the league's bot list (GET /be/league's order).
typedef struct {
    uint32_t seq;          // 1.. in the order the league saw them rated
    uint8_t  gid_len;
    char     gid[BE_GID];
    uint8_t  n;            // seats
    uint8_t  podkidnoy;
    int8_t   fool;         // seat; -1 a draw
    int8_t   first;        // the seat that went out first; -1 none
    uint8_t  bot[BE_SEATS];
    int32_t  before[BE_SEATS];
    int32_t  after[BE_SEATS];  // the client table's rating_after: before + delta, as the stamp says
    int16_t  delta[BE_SEATS];
    uint32_t dur_s;        // deal to fool, by the league's clock
    int64_t  at_ms;
} be_w_result;

typedef struct {
    uint32_t hash;
    uint32_t n_bots;
    uint32_t bots_at;      // where the first be_w_bot starts
    uint32_t bot_size;
    uint32_t n_results;
    uint32_t results_at;
    uint32_t result_size;
    uint8_t  mode;         // BE_MODE_*
} be_w_league;

#define BE_MODE_LEAGUE     1
#define BE_MODE_MIXED      2
#define BE_MODE_TOURNAMENT 3

// ---- the tournament (tournament.h) -----------------------------------------------------------------
//
// 48 bots, single elimination, two-player best-of-five matches, seeded by the server's ratings: the
// 32 lower seeds play an opening round (17 v 48 ... 32 v 33), whose winners meet the 16 top seeds in
// the round of 32. Every game is a real rated server game; a match is won by the first to three
// games the other was the fool of (a drawn game is played again).

#define BE_TOUR_PLAYERS 48
#define BE_TOUR_ROUNDS  6     // opening, 32, 16, quarter, semi, final
#define BE_TOUR_MATCHES 47
#define BE_TOUR_NONE    255   // no bot (yet), no winner (yet)
#define BE_TOUR_WIN     3     // games to win a match

#define BE_TOUR_IDLE    0     // no tournament
#define BE_TOUR_RUNNING 1
#define BE_TOUR_DONE    2

typedef struct {
    uint8_t  round;          // 0 the opening round .. 5 the final
    uint8_t  slot;           // its place in the round, top to bottom
    uint8_t  a, b;           // league bot indices (GET /be/league's order), BE_TOUR_NONE until known
    uint8_t  seed_a, seed_b; // 1..48, 0 until known
    uint8_t  wins_a, wins_b;
    uint8_t  games;          // games played, draws included
    uint8_t  winner;         // a or b once decided, else BE_TOUR_NONE
    uint8_t  live;           // a game of this match is in play
    uint8_t  gid_len;        // the game in play (or the last one)
    char     gid[BE_GID];
    int64_t  done_ms;        // when it was decided (wall clock), 0 before
} be_w_match;

typedef struct {
    uint32_t hash;
    uint8_t  state;          // BE_TOUR_*
    uint8_t  dropped;        // the bot left out of 48 (league index), BE_TOUR_NONE before a draw
    uint8_t  champion;       // BE_TOUR_NONE until the final is won
    uint8_t  n_matches;
    int32_t  seed_rating[BE_TOUR_PLAYERS];   // each seed's server rating when the field was drawn
    uint8_t  seed_bot[BE_TOUR_PLAYERS];      // seed i+1's league bot index
    int64_t  started_ms, finished_ms;
    uint32_t games;          // games played in the tournament
    be_w_match matches[BE_TOUR_MATCHES];
} be_w_tour;

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
#define BE_F_RESULT 4
#define BE_F_BOT    5
#define BE_F_TOUR   6   // the whole be_w_tour, on every change

// The layout hash structgen printed for this header, compiled in (the Makefile).
#ifndef BIRDSEYE_WIRE_HASH
#define BIRDSEYE_WIRE_HASH 0u
#endif
static inline uint32_t be_wire_hash(void) { return (uint32_t)BIRDSEYE_WIRE_HASH; }

#endif

// Game state, players, logs. Mirrors the TS structures in types.ts and the
// behaviors in common_utils.ts / actions/*.ts. We don't model the production
// fields (animations, ELO, message broadcasting) — only what the bots need.
#ifndef CNITRO_GAME_H
#define CNITRO_GAME_H

#include "card.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_PLAYERS    2          // 1v1 only (matches espresso/random testing).
#define MAX_HAND_SIZE  64         // generous; pickup can stack many cards.
#define MAX_BATTLES    32         // enough for any reasonable round.
#define MAX_DECK       64         // 36 for 2p, with slack.
#define MAX_LOGS       512        // long games never approach this.

#define LOG_GAME_START      0
#define LOG_ATTACK          1
#define LOG_COVER           2
#define LOG_PASS            3
#define LOG_PICKUP          4
#define LOG_GOOD            5
#define LOG_DISCARD         6
#define LOG_DEFENDER_CHANGE 7
#define LOG_PLAYER_OUT      8
#define LOG_DRAW            9

#define PLAYER_STATUS_IDLE  0
#define PLAYER_STATUS_READY 1
#define PLAYER_STATUS_IN    2
#define PLAYER_STATUS_OUT   3

#define GAME_STATUS_WAITING   0
#define GAME_STATUS_PLAYING   1
#define GAME_STATUS_GAME_OVER 2

typedef struct {
    Card attack;
    Card defense;     // {-1,-1} when uncovered
    bool has_defense;
} Battle;

// Each pair has a primary card and an optional target card. For COVER, target
// is the attack card the cover defends; otherwise unused.
#define MAX_LOG_PAIRS 16
typedef struct {
    Card  primary;
    Card  target;
    bool  has_target;
} LogPair;

typedef struct {
    int8_t  log_type;
    int8_t  player_idx;     // -1 = system event
    int8_t  defender_index; // -1 if not a defender_change
    int8_t  num_pairs;
    LogPair pairs[MAX_LOG_PAIRS];
} GameLog;

typedef struct {
    int8_t  status;            // PLAYER_STATUS_*
    int8_t  hand_count;
    bool    awaiting_attack;
    int8_t  strategy_key;      // application-defined
    Card    hand[MAX_HAND_SIZE];
    char    name[24];
    char    player_id[24];
} Player;

typedef struct {
    int8_t  status;
    int8_t  num_players;
    int8_t  power_suit;
    int8_t  first_attacker;
    int8_t  defender;
    int8_t  num_battles;
    int16_t deck_count;
    int16_t discard_pile_length;
    bool    has_flipped;
    Card    flipped;
    Card    deck[MAX_DECK];
    Battle  table_battles[MAX_BATTLES];
    Player  players[MAX_PLAYERS];

    // Elimination order: indices into players[]; length = num_eliminated.
    int8_t  elimination_order[MAX_PLAYERS];
    int8_t  num_eliminated;

    // good_players: bitmask of player indices that have said good.
    uint32_t good_players_mask;
    bool     has_good_timestamp;

    // Logs (append-only).
    int      num_logs;
    GameLog  logs[MAX_LOGS];
} Game;

// ---------- RNG ---------------------------------------------------------

// Mirrors `seededRandom` in common_utils.ts and `setRandomSeed` in
// random_strategy.ts: two independent LCGs with the same recurrence.
void   game_set_seed(uint32_t s);
double game_random(void);            // 0..1
uint32_t game_random_u32(void);

void     random_strategy_set_seed(uint32_t s);
double   random_strategy_random(void);

// ---------- Helpers -----------------------------------------------------

bool can_cover(Card attack, Card defense, int power_suit);
int  get_next_player_index(const Game *g, int current);
int  game_done(const Game *g);   // returns loser index, or -1
void start_game(Game *g);

// In-place game clone (used by collect's `before` snapshot).
void game_clone(Game *dst, const Game *src);

// ---------- Action handlers (return false on validation failure) --------

bool handle_attack(Game *g, int player_idx, const Card *cards, int n_cards);
bool handle_cover(Game *g, int player_idx,
                  const Card *cover_cards, const Card *attack_cards, int n);
bool handle_pass(Game *g, int player_idx, const Card *cards, int n_cards);
bool handle_pickup(Game *g, int player_idx);
bool handle_good(Game *g, int player_idx);

// ---------- Loop helpers ------------------------------------------------

bool should_bot_act(const Game *g, int bot_idx);

#endif

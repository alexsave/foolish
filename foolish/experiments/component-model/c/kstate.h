// A cut-down resident game, shaped like c/src/game.h's Game (same caps), with
// cards kept as their u8 wire value. Both builds (hand-marshalled baseline and
// the component) copy into / out of exactly this struct, so the only thing that
// differs between them is the boundary glue.
#ifndef KSTATE_H
#define KSTATE_H

#include <stdint.h>  // clang's freestanding header, no libc

#define MAX_PLAYERS 8
#define MAX_HAND_SIZE 64
#define MAX_BATTLES 64
#define MAX_DECK 64
#define WIRE_NONE 0xff

typedef struct { uint8_t attack, defense; } KBattle;  // defense WIRE_NONE = none

typedef struct {
    int8_t status;
    uint8_t awaiting;
    int8_t hand_count;
    uint8_t hand[MAX_HAND_SIZE];
} KPlayer;

typedef struct {
    int8_t status, num_players, power_suit, first_attacker, defender;
    int16_t discard;
    uint8_t has_flipped, flipped;
    uint32_t good_mask;
    uint8_t has_good_ts;
    int16_t deck_count;
    uint8_t deck[MAX_DECK];
    int8_t num_battles;
    KBattle battles[MAX_BATTLES];
    KPlayer players[MAX_PLAYERS];
    int8_t num_eliminated;
    int8_t elimination[MAX_PLAYERS];
} KGame;

extern KGame g_game;

#endif

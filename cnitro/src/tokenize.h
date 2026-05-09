// Tokenizer + action vocabulary. Mirrors nitro_nn.ts.
#ifndef CNITRO_TOKENIZE_H
#define CNITRO_TOKENIZE_H

#include "game.h"
#include "card.h"
#include <stdbool.h>

#define NUM_CARDS      40
#define ACTION_PICKUP  40
#define ACTION_STOP    41
#define NUM_ACTIONS    42

#define TOK_PAD             0
#define TOK_CLS             1
#define TOK_ROLE_ATK        2
#define TOK_ROLE_DEF        3
#define TOK_ROLE_FIRST      4
#define TOK_DECK_FULL       5
#define TOK_DECK_MED        6
#define TOK_DECK_LOW        7
#define TOK_DECK_EMPTY      8
#define TOK_SEC_HAND        9
#define TOK_SEC_OPP         10
#define TOK_SEC_DISCARD     11
#define TOK_SEC_TABLE       12
#define TOK_SEC_PROGRESS    13
#define TOK_BATTLE_COVER    14
#define TOK_BATTLE_NEXT     15
#define TOK_SEC_HISTORY     16
#define TOK_PLAYER_SELF     17
#define TOK_PLAYER_OPP      18
#define TOK_MOVE_ATTACK     19
#define TOK_MOVE_COVER      20
#define TOK_MOVE_PASS       21
#define TOK_MOVE_PICKUP     22
#define TOK_MOVE_GOOD       23
#define TOK_MOVE_DRAW       24
#define TOK_MOVE_DISCARD    25
#define TOK_COVER_TARGET    26
#define TOK_CARD_BASE       32
#define VOCAB_SIZE          (TOK_CARD_BASE + NUM_CARDS)  // 72

#define MAX_SEQ_LEN          192
#define MAX_HISTORY_EVENTS   32

// Role of an in-progress (partial) move.
#define INPROG_ATTACK 0
#define INPROG_COVER  1
#define INPROG_PASS   2
#define INPROG_IDLE   3

typedef struct {
    int   role;
    Card  cards_chosen[8];
    int   n_cards_chosen;
} InProgress;

typedef struct {
    int  tokens[MAX_SEQ_LEN];
    int  n_tokens;
} Tokenized;

int  card_token_id(int suit, int value, int trump_suit);
int  card_action_id(int suit, int value, int trump_suit);
void action_id_to_card(int id, int trump_suit, Card *out);

void tokenize(const Game *g, int bot_idx, const InProgress *ip, Tokenized *out);

#endif

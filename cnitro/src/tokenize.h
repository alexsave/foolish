// Tokenizer + action vocabulary. Mirrors nitro_nn.ts.
#ifndef CNITRO_TOKENIZE_H
#define CNITRO_TOKENIZE_H

#include "game.h"
#include "card.h"
#include <stdbool.h>

// Action vocab: 52 card slots (4 suits × 13 values, 2..A) + PICKUP + STOP.
// In 2..5 player games values 2,3,4 are never dealt and the corresponding
// card slots are unused but kept in the vocab so a single trained model can
// play any player count.
#define NUM_CARDS      52
#define ACTION_PICKUP  52
#define ACTION_STOP    53
#define NUM_ACTIONS    54

// Identity / structural
#define TOK_PAD             0
#define TOK_CLS             1
#define TOK_ROLE_ATK        2
#define TOK_ROLE_DEF        3
#define TOK_ROLE_FIRST      4
// Generic "size" bucket vocabulary, reused across deck, discard, and per-opp
// hand-count slots. Position (the preceding section header) disambiguates.
#define TOK_SIZE_FULL       5  // (was TOK_DECK_FULL)
#define TOK_SIZE_MED        6  // (was TOK_DECK_MED)
#define TOK_SIZE_LOW        7  // (was TOK_DECK_LOW)
#define TOK_SIZE_EMPTY      8  // (was TOK_DECK_EMPTY)
#define TOK_SEC_HAND        9
#define TOK_SEC_FLIPPED     10 // was TOK_SEC_OPP — repurposed: flipped trump
#define TOK_SEC_DISCARD_LEN 11 // was TOK_SEC_DISCARD (cards); now length only
#define TOK_SEC_TABLE       12
#define TOK_SEC_PROGRESS    13
#define TOK_BATTLE_COVER    14
#define TOK_BATTLE_NEXT     15
#define TOK_SEC_HISTORY     16
#define TOK_PLAYER_SELF     17
// (slot 18 was TOK_PLAYER_OPP — replaced by per-seat tokens 32..38 in history)
#define TOK_MOVE_ATTACK     19
#define TOK_MOVE_COVER      20
#define TOK_MOVE_PASS       21
#define TOK_MOVE_PICKUP     22
#define TOK_MOVE_GOOD       23
#define TOK_MOVE_DRAW       24
#define TOK_MOVE_DISCARD    25
#define TOK_COVER_TARGET    26
#define TOK_SEC_OPP_SIZES   27 // section header: per-opp hand-count summary
#define TOK_MOVE_OUT        28 // history event: player at seat-N was eliminated
// Per-seat opponent tag. Seat = how many "next active player" hops from self,
// counting only IN players. In 1v1 the lone opp is always seat 1. Designed as
// the N-player superset so the same vocab handles 2..8 player games. Used in
// two contexts: history attribution (who played a logged move) and the
// OPP_SIZES section (size bucket follows the seat token per opponent).
#define MAX_OPPONENTS       7
#define TOK_OPP_SEAT_1      32
#define TOK_OPP_SEAT_2      33
#define TOK_OPP_SEAT_3      34
#define TOK_OPP_SEAT_4      35
#define TOK_OPP_SEAT_5      36
#define TOK_OPP_SEAT_6      37
#define TOK_OPP_SEAT_7      38
#define TOK_CARD_BASE       39
#define VOCAB_SIZE          (TOK_CARD_BASE + NUM_CARDS)  // 79

// Back-compat aliases. The deck section still uses the size buckets — these
// names just preserve the old call sites.
#define TOK_DECK_FULL       TOK_SIZE_FULL
#define TOK_DECK_MED        TOK_SIZE_MED
#define TOK_DECK_LOW        TOK_SIZE_LOW
#define TOK_DECK_EMPTY      TOK_SIZE_EMPTY

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

// Returns seat number (1..MAX_OPPONENTS) of opp_idx relative to bot_idx,
// counting active players only (PLAYER_STATUS_OUT is skipped). Returns 0 if
// opp_idx == bot_idx, opp_idx is OUT, or beyond MAX_OPPONENTS hops.
int  opponent_seat(const Game *g, int bot_idx, int opp_idx);

void tokenize(const Game *g, int bot_idx, const InProgress *ip, Tokenized *out);

#endif

// The Bot Cup interface (~/Dev/botcup/kit/botcup.h), here over the server's kernel: the four
// contest bots in c/botcup/ include it by this name and are compiled against the server's own headers
// and capacities (MAX_BATTLES, MAX_LOG_PAIRS, ...), never the kit's older copy of the engine.
// Only the two engine includes differ from the kit's file. It sits in src/ so the bots' own
// #include "botcup.h" finds it on the kernel's include path.
#ifndef BOTCUP_H
#define BOTCUP_H
#include "game.h"
#include "legal.h"

// Pick a move. `view` is the game as seat `seat` sees it: every other seat's
// hand cards, the deck's cards and other seats' LOG_DRAW cards are replaced by
// the placeholder {suit -1, value -1}, the engine's not-a-card (counts are real).
// It was {suit 0, value 1}, which is the 2 of spades in the 52-card deck that
// 6-8 players use: a bot could not tell that real card from a hidden one. Everything face up is
// real: your hand, the table, the flipped trump, the logs of attacks, covers,
// passes, pickups, discards, goods, player-outs and defender changes.
// `moves` is the legal move menu for your seat. Return an index into it.
// A return outside 0..moves->n-1 forfeits your turn this time round.
typedef int (*BotChooseFn)(const Game *view, int seat, const LegalMoves *moves);

typedef struct {
    const char  *name;
    BotChooseFn  choose;
    // Optional (may be NULL): called once per game before the first move.
    void       (*new_game)(int seat, int num_players);
} BotEntry;

// The card the referee puts wherever a card is hidden from you. Never a real
// card in either deck (a real card has suit 0..3 and value 1..13).
#define BOTCUP_HIDDEN_SUIT  (-1)
#define BOTCUP_HIDDEN_VALUE (-1)
static inline int botcup_is_hidden(Card c) { return c.suit == BOTCUP_HIDDEN_SUIT && c.value == BOTCUP_HIDDEN_VALUE; }

#endif

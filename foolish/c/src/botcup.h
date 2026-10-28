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
// the placeholder {suit 0, value 1} (counts are real). Everything face up is
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

// The card the referee puts wherever a card is hidden from you.
static inline int botcup_is_hidden(Card c) { return c.suit == 0 && c.value == 1; }

#endif

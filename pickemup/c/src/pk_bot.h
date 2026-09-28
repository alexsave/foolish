/* Pick 'Em Up - the bots: a random baseline, a cheap greedy heuristic, and a
 * belief-constrained Monte Carlo player shaped on foolish's octogen.
 *
 * NOT IN THE APP. These files are built only into tests/pk_bot_test.c and
 * tools/pk_arena.c (the Makefile's BOT_SRC), for offline play and
 * measurement, as foolish's arena and ladder are (DECISION D63).
 *
 * EVERY MOVE COMES FROM THE KERNEL'S MENU. A bot never builds an action of
 * its own: it picks an entry of pk_legal / pk_legal_turn, or asks to seal
 * when pk_can_seal says it may, so it cannot return an illegal move and no
 * rule is decided twice.
 *
 * WHAT A STRATEGY DECIDES is the turn: draw, which card (and a wild's suit),
 * pass. "Last card!" and "Caught you!" are one shared rule for every
 * strategy (DECISION D60): say it at the first legal chance (it costs
 * nothing and saves two cards); call out a seat only when the public history
 * proves it exposed (a wrong call costs a card, and a right one is certain).
 *
 * The Monte Carlo player, octogen's shape (RULES_AND_KERNEL.md "Bot"):
 *   - a belief from the public events (pk_belief.h);
 *   - candidates: the turn menu, equivalent cards merged, each suit of a wild
 *     its own candidate, ranked with greedy's choice first;
 *   - worlds sampled from the belief, soft voids obeyed in (soft_mod - 1) of
 *     every soft_mod worlds (octogen's void mixture);
 *   - common random numbers: every candidate is rolled out in the same world
 *     with the same seed, so the comparison is paired;
 *   - successive halving over three stages (w1 worlds for all, w2 more for
 *     the best third, w3 more for the final two);
 *   - the rollout plays greedy for every seat, to the end or `depth` turn
 *     actions, scored 1 for a win and 0 for a loss, or at a cut-off by the
 *     share of 1/cards (fewer cards, likelier winner);
 *   - small taxes at selection only, in octogen's OG_TRUMP_KEEP milli units:
 *     on playing a wild while a suited play exists, and on drawing while a
 *     play exists; ties go to the earlier, greedier candidate (octogen's
 *     tie-break inversion).
 *
 * Deterministic: the same game, seat, knobs and rng state choose the same
 * move. No allocation; no libc beyond memcpy / memset. */
#ifndef PK_BOT_STRATEGY_H
#define PK_BOT_STRATEGY_H

#include "pk.h"

enum { PK_BOT_RANDOM = 0, PK_BOT_GREEDY, PK_BOT_MC, PK_BOT_COUNT };

/* The knobs (octogen's OG_* in one struct). */
enum {
    PK_KNOB_RANDOM_WILD = 1,     /* ablation: a wild's suit is chosen at random  */
    PK_KNOB_NO_BELIEF   = 2,     /* ablation: worlds ignore every void           */
};
typedef struct {
    uint16_t w1, w2, w3;         /* worlds in each stage                          */
    uint16_t depth;              /* rollout turn actions, 0 = to the end          */
    uint16_t wild_keep;          /* milli-wins taxed on a wild while a suited play exists */
    uint16_t draw_keep;          /* milli-wins taxed on a draw while a play exists */
    uint8_t  soft_mod;           /* soft voids in (soft_mod-1)/soft_mod worlds, 0 = never */
    uint8_t  flags;              /* PK_KNOB_*                                     */
    uint8_t  pad0, pad1;
    uint64_t seed;               /* folded into every world seed                  */
} PkBotKnobs;

void pk_bot_knobs_default(PkBotKnobs *k);

/* One thing a bot does: apply `act` for `seat`, or seal the open bubble. */
enum { PK_BOT_NONE = 0, PK_BOT_ACT, PK_BOT_SEAL };
typedef struct {
    uint8_t what;                /* PK_BOT_*                                      */
    uint8_t seat;
    uint8_t pad0, pad1;
    PkAct   act;                 /* with PK_BOT_ACT, an entry of pk_legal         */
} PkBotMove;

/* Room for any menu pk_legal can write. */
#define PK_BOT_MENU_CAP (PK_HAND_CAP * 4 + PK_MAX_SEATS * 2 + 4)

/* What `seat` does next. 1 with a move in `out`, 0 when it has nothing to do
 * (another seat's bubble is open, or out of turn with nothing to say or call).
 * `rng` is the caller's xorshift state; it is advanced. */
int pk_bot_choose(const PkGame *g, int seat, int strategy, const PkBotKnobs *k,
                  uint64_t *rng, PkBotMove *out);

/* Apply a chosen move. 1, or 0 when the kernel refuses it (never, if it came
 * from pk_bot_choose on the same game). */
int pk_bot_apply(PkGame *g, const PkBotMove *m);

/* One bubble for `seat`, chosen and applied move by move until it is sealed.
 * Actions applied (0: nothing to do), or -1 when the kernel refused one. */
int pk_bot_bubble(PkGame *g, int seat, int strategy, const PkBotKnobs *k, uint64_t *rng);

/* THE TABLE, ONE ROUND (the arena's and the tests' driver, bot_drive's role):
 * every seat but the turn seat may send an out-of-turn bubble, starting with
 * the sender of the last bubble (the only seat that can have just been
 * exposed, so it may say "Last card!" before anyone else can catch it, D61),
 * then the turn seat sends its turn. strategy[s] and knobs[s] are seat s's.
 * 1 while the game goes on, 0 once it is over, -1 on a refused move. */
int pk_bot_round(PkGame *g, const uint8_t strategy[PK_MAX_SEATS],
                 const PkBotKnobs knobs[PK_MAX_SEATS], uint64_t *rng);

/* The greedy policy's choice among `n` turn actions (the MC rollout policy).
 * An index into m. */
int pk_bot_greedy(const PkGame *g, int seat, const PkAct *m, int n);

/* The tests' and the arena's randomness: xorshift64*, never the game's. */
uint64_t pk_bot_rand(uint64_t *rng);

extern const char *const PK_BOT_NAME[PK_BOT_COUNT];

#endif

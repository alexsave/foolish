// Strategy interface and registration. Mirrors BotStrategy.chooseMove —
// given a game and the precomputed legal moves, pick one. Index returned.
#ifndef CNITRO_STRATEGY_H
#define CNITRO_STRATEGY_H

#include "game.h"
#include "legal.h"

#define STRAT_RANDOM      0
#define STRAT_ESPRESSO    1
#define STRAT_HANDWRITTEN 2
#define STRAT_ROBUSTA     3
#define STRAT_FIRECRACKER 4
#define STRAT_GUNPOWDER   5
#define STRAT_BLACKPOWDER 6
#define STRAT_CORDITE     7
#define STRAT_ASTROLITE   8
#define STRAT_CORDITE_OLD 9    // cordite with the pre-change (1x) budget; research
// Production bots ported from the TS originals (exact behavioral mirrors).
#define STRAT_SIMPLE_HEURISTIC  10
#define STRAT_CHAMPION          11
#define STRAT_ULTIMATE_CHAMPION 12
#define STRAT_HACKER            13
#define STRAT_FULMINATE         14   // cordite + per-seat opponent profiling
// The production espresso/handwritten BOTS. The un-suffixed C espresso/
// handwritten above are the arena/rollout variants (cordite's rollout policy
// and its cordite_sim.c bitboard mirror are tuned and frozen against them);
// they drifted slightly from the TS production bots, so the production ids
// get their own exact mirrors.
#define STRAT_ESPRESSO_PROD     15
#define STRAT_HANDWRITTEN_PROD  16
// Linear policy distilled from cordite(prod) self-play, with a DL_TAU
// confidence gate that defers uncertain decisions back to cordite.
#define STRAT_DISTILLED         17
// 18 is retired: it was semtex (cordite-derived; octogen's predecessor).
// 19 is retired: it was semtex_oracle (semtex at 6x worlds).
#define STRAT_OCTOGEN           20   // cordite lineage + stage-3 opponent reply tournament
#define STRAT_OCTOGEN_ORACLE    21   // octogen at 6x worlds (research/audit only)
// 22 is retired: it was torpex (a learned value net in place of rollouts).
// 23 is retired: it was novichok (the cheating ceiling probe; read real hands).
// Octogen whose pick a noisy second race may overrule (c/FUSION.md); seeded on
// the website as Moscow, not offline.
#define STRAT_FUSION            24
// Fusion reading its knobs from FUSIONX_* (research, not on the roster): the
// arm of a paired fusion_x-vs-fusion run whose knobs the control does not see.
#define STRAT_FUSION_X          25
//
// THE IDS ARE NOT COMPACTED, and a retired one is never handed out again. A
// STRAT_* id leaves the process: it is a seat's Player.strategy_key, a
// BotRosterRow.strat a host reads across the boundary, and STRAT_OCTOGEN is a
// generated constant the oracle page passes back in (tools/structgen/specs/
// oracle_answer.args). Renumbering the survivors would turn one bot into
// another on the far side. strategy_retired() names the holes.
//
// One past the last id, and the length of the registry (bots.c BOTS[]). A new
// STRAT_* id takes this value and bumps it; its row in bots.c is the rest of
// its registration.
#define STRAT_COUNT             26

// 1 for an id below STRAT_COUNT whose brain was removed: its slot in bots.c is
// empty, so it dispatches to nothing (strategy_choose is -1) and no name parses
// to it.
int strategy_retired(int strat);

// Returns chosen move index in moves->moves[] (0..moves->n-1).
typedef int (*StrategyFn)(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// Run brain `strat` (a STRAT_* id) at its C defaults - the arena's dispatch,
// which every native tool (cnitro_eval, _elo, _gen, _showcase) and the tests
// share: bots.c's dispatch tables at `strat`. -1 for an id that is not a brain, or one
// this build does not link. A named, knob-tuned production bot is
// bot_roster_choose instead.
int strategy_choose(int strat, const Game *g, int bot_idx, const LegalMoves *moves);

// Map a strategy name (or short alias) to its STRAT_* id; -1 if unknown. The
// names are bots.c's, the one list the main programs share.
int parse_strategy(const char *s);

// One per brain, each defined in its src/<name>_strategy.c (two more variants
// live in cordite_strategy.c and octogen_strategy.c), and each named once more,
// in its bots.c row.
int random_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int espresso_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int handwritten_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int robusta_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int firecracker_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int gunpowder_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int blackpowder_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int cordite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int cordite_old_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int astrolite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int simple_heuristic_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int champion_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int ultimate_champion_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int hacker_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int fulminate_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int espresso_prod_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int handwritten_prod_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int distilled_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Oracle octogen (research): 6x world budget + wider candidate survival. For
// loss audits - a decision the oracle changes was compute-limited.
int octogen_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Fusion: octogen with rollout noise (octogen_strategy.c; native builds only).
int fusion_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// fusion_x: fusion with FUSIONX_* knobs (research; native builds only).
int fusion_x_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

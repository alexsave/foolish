// Strategy interface and registration. Mirrors BotStrategy.chooseMove —
// given a game and the precomputed legal moves, pick one. Index returned.
#ifndef CNITRO_STRATEGY_H
#define CNITRO_STRATEGY_H

#include "game.h"
#include "legal.h"
#include <string.h>

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
#define STRAT_SEMTEX            18   // cordite-derived; tuned to beat cordite itself
#define STRAT_SEMTEX_ORACLE     19   // semtex at 6x worlds (research/audit only)
#define STRAT_OCTOGEN           20   // semtex + stage-3 opponent reply tournament
#define STRAT_OCTOGEN_ORACLE    21   // octogen at 6x worlds (research/audit only)
#define STRAT_TORPEX            22   // semtex + learned value net replacing rollouts
#define STRAT_NOVICHOK          23   // CHEATING apex (real hands; research/eval only)
#define STRAT_CL20              24   // octogen's successor (c/CL20.md)
#define STRAT_CL20_ORACLE       25   // cl20 at 6x worlds (research/audit only)

// Returns chosen move index in moves->moves[] (0..moves->n-1).
typedef int (*StrategyFn)(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

int random_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int espresso_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int handwritten_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Direct rollout chooser: writes handwritten's lite-policy move into *out and
// returns true, or returns false to defer to the slow enumerate-then-pick
// path. Behaviorally identical to enumerating calculate_legal_moves_lite then
// calling handwritten_strategy_choose. See handwritten_strategy.c.
bool handwritten_rollout_choose(const Game *g, int bot_idx, LegalMove *out);
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
int semtex_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
// Oracle semtex (research): 6x world budget + wider candidate survival. For
// loss audits — a decision the oracle changes was compute-limited.
int semtex_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int octogen_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int torpex_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int novichok_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int cl20_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);
int cl20_oracle_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

// Map a strategy name (or short alias) to its STRAT_* id; -1 if unknown.
// Single source of truth for the name<->id mapping the main programs share.
static inline int parse_strategy(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "random")      || !strcmp(s, "rand")) return STRAT_RANDOM;
    if (!strcmp(s, "espresso")    || !strcmp(s, "esp"))  return STRAT_ESPRESSO;
    if (!strcmp(s, "handwritten") || !strcmp(s, "hw"))   return STRAT_HANDWRITTEN;
    if (!strcmp(s, "robusta")     || !strcmp(s, "rob"))  return STRAT_ROBUSTA;
    if (!strcmp(s, "firecracker") || !strcmp(s, "fc"))   return STRAT_FIRECRACKER;
    if (!strcmp(s, "gunpowder")   || !strcmp(s, "gp"))   return STRAT_GUNPOWDER;
    if (!strcmp(s, "blackpowder") || !strcmp(s, "bp"))   return STRAT_BLACKPOWDER;
    if (!strcmp(s, "cordite")     || !strcmp(s, "cd"))   return STRAT_CORDITE;
    if (!strcmp(s, "astrolite")   || !strcmp(s, "as"))   return STRAT_ASTROLITE;
    if (!strcmp(s, "cordite_old") || !strcmp(s, "cd0"))  return STRAT_CORDITE_OLD;
    if (!strcmp(s, "simple_heuristic")  || !strcmp(s, "sh")) return STRAT_SIMPLE_HEURISTIC;
    if (!strcmp(s, "champion")          || !strcmp(s, "ch")) return STRAT_CHAMPION;
    if (!strcmp(s, "ultimate_champion") || !strcmp(s, "uc")) return STRAT_ULTIMATE_CHAMPION;
    if (!strcmp(s, "hacker")            || !strcmp(s, "hk")) return STRAT_HACKER;
    if (!strcmp(s, "fulminate")         || !strcmp(s, "fm")) return STRAT_FULMINATE;
    if (!strcmp(s, "espresso_prod")     || !strcmp(s, "ep")) return STRAT_ESPRESSO_PROD;
    if (!strcmp(s, "handwritten_prod")  || !strcmp(s, "hp")) return STRAT_HANDWRITTEN_PROD;
    if (!strcmp(s, "distilled")         || !strcmp(s, "dl")) return STRAT_DISTILLED;
    if (!strcmp(s, "semtex")            || !strcmp(s, "sx"))  return STRAT_SEMTEX;
    if (!strcmp(s, "semtex_oracle")     || !strcmp(s, "sxo")) return STRAT_SEMTEX_ORACLE;
    if (!strcmp(s, "octogen")           || !strcmp(s, "og"))  return STRAT_OCTOGEN;
    if (!strcmp(s, "octogen_oracle")    || !strcmp(s, "ogo")) return STRAT_OCTOGEN_ORACLE;
    if (!strcmp(s, "torpex")            || !strcmp(s, "tx"))  return STRAT_TORPEX;
    if (!strcmp(s, "novichok")          || !strcmp(s, "nv"))  return STRAT_NOVICHOK;
    if (!strcmp(s, "cl20")              || !strcmp(s, "cl"))  return STRAT_CL20;
    if (!strcmp(s, "cl20_oracle")       || !strcmp(s, "clo")) return STRAT_CL20_ORACLE;
    return -1;
}

#endif

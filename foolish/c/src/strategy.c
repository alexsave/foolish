// The arena's one dispatch from a STRAT_* id to its brain (strategy.h).
//
// The native tools used to carry a copy each, and the four copies had drifted:
// cnitro_eval knew every brain, cnitro_gen eight of them, cnitro_elo thirteen,
// and cnitro_showcase quietly played cordite for any id it did not know. A test
// that must see every brain (tests.c, the podkidnoy roster sweep) needs the
// list to be one list.
//
// Not linked into any shipped module: the wasm builds do not compile this file,
// and the iOS archive links per object, so nothing that never calls it carries
// the ladder it names.
#include "strategy.h"

int strategy_choose(int strat, const Game *g, int bot_idx, const LegalMoves *moves) {
    switch (strat) {
        case STRAT_RANDOM:            return random_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_ESPRESSO:          return espresso_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_HANDWRITTEN:       return handwritten_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_ROBUSTA:           return robusta_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_FIRECRACKER:       return firecracker_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_GUNPOWDER:         return gunpowder_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_BLACKPOWDER:       return blackpowder_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_CORDITE:           return cordite_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_ASTROLITE:         return astrolite_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_CORDITE_OLD:       return cordite_old_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_SIMPLE_HEURISTIC:  return simple_heuristic_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_CHAMPION:          return champion_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_ULTIMATE_CHAMPION: return ultimate_champion_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_HACKER:            return hacker_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_FULMINATE:         return fulminate_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_ESPRESSO_PROD:     return espresso_prod_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_HANDWRITTEN_PROD:  return handwritten_prod_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_DISTILLED:         return distilled_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_SEMTEX:            return semtex_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_SEMTEX_ORACLE:     return semtex_oracle_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_OCTOGEN:           return octogen_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_OCTOGEN_ORACLE:    return octogen_oracle_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_TORPEX:            return torpex_strategy_choose(g, bot_idx, moves, NULL);
        case STRAT_NOVICHOK:          return novichok_strategy_choose(g, bot_idx, moves, NULL);
        default:                      return -1;
    }
}

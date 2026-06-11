// Astrolite — successor to cordite (Astrolite G, a far more powerful explosive
// than the cordite propellant). Cordite's belief-constrained determinized
// Monte Carlo, plus explicit card-management heuristics layered on the
// DEFENDER's cover decision — the regime where cordite's terminal-only MC is
// blind (covering a doomed table, stranding the trump king, baiting with junk).
// Public info only.
#ifndef CNITRO_ASTROLITE_STRATEGY_H
#define CNITRO_ASTROLITE_STRATEGY_H

#include "game.h"
#include "legal.h"

int astrolite_strategy_choose(const Game *g, int bot_idx, const LegalMoves *moves, void *ctx);

#endif

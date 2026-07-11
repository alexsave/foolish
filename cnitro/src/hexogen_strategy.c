// Hexogen (STRAT_HEXOGEN) — octogen's chemical sibling. hexogen/RDX is
// octogen/HMX's literal next-of-kin, which is exactly the relationship the two
// bots have here: hexogen IS octogen's brain, run with a modest, iso-latency
// search-budget raise (the "S4" spend in docs/L1_SPEND_PLAN.md §5).
//
// Why a new strategy id and not an octogen edit (iron rule R1,
// docs/L1_SPEND_PLAN.md §0): a bigger world budget changes which lines resolve
// within budget, so it CHANGES HOW THE BOT PLAYS. octogen is pinned to its
// exact TS-oracle mirror by bot_parity; anything that moves its move choice
// must land as a fresh id. hexogen has no TS mirror, so bot_parity does not
// cover it — its gates are the outcome ladder (V4 vs TT22) and the elo arbiter
// (see docs/L1_SPEND_PLAN.md §7).
//
// This wrapper is deliberately thin: it brackets an octogen_strategy_choose
// call with hexogen mode. Every octogen fix therefore flows into hexogen for
// free, and the seven shipped families stay byte-identical (octogen never sees
// hexogen mode set).
//
// STATUS (this session): hexogen is the RESERVED successor scaffold. Its only
// currently-wired lever is the S4 world-raise (HX_PCT, percent of octogen's
// per-decision worlds), and that lever was MEASURED FLAT vs octogen at pc2 and
// pc4 while costing latency (see the L1_SPEND_PLAN appendix). So HX_PCT
// defaults to 100 — hexogen == octogen today, with no unfunded, control-losing
// spend baked in. The real strength levers the plan reserves this id for are
// S3 LEAFBOOK (an offline endgame oracle that SAVES nodes, funding an
// iso-latency raise) and S2 (a bound side-table, after C5-v2). They land here,
// behind this same thin wrapper, so octogen stays pinned to its TS mirror (R1).

#include "hexogen_strategy.h"
#include "octogen_strategy.h"
#include "strategy.h"

int hexogen_strategy_choose(const Game *g, int bot_idx,
                            const LegalMoves *moves, void *ctx) {
    octogen_set_hexogen(1);
    int r = octogen_strategy_choose(g, bot_idx, moves, ctx);
    octogen_set_hexogen(0);
    return r;
}

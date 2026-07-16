// See bot_roster.h. The table below is the single source of truth for what a
// named bot IS; supabase/seed.sql, bot_strategy.ts, ios_api.c and
// wasm_bots_api.c are all consumers of it now.

#include "bot_roster.h"

#include "bot_knobs.h"
#include "strategy.h"

// Force the flag caches to re-read their knobs. Each Monte-Carlo brain reads
// its knobs once and latches them (cd_flags_loaded / og_flags_loaded), which
// is why the knobs must be invalidated between two decisions that carry
// different specs.
void cordite_reload_flags(void);
void og_reload_flags(void);

// The octogen trump-conservation tie-break, in milli-units of mean-finish
// taxed per trump led while the deck is alive (octogen_strategy.c
// OG_TRUMP_KEEP). '40' = on; '0' reverts to the pre-fix behavior. This value
// moved here from bot_strategy.ts, where it was an edge-deployable constant —
// changing it is now a kernel rebuild, deliberately: it is a property of the
// bot named "octogen", not of the server that happens to run it.
#define OCTOGEN_TRUMP_KEEP "40"

// CD_RACE stops a deliberation once the leading candidate is statistically
// separated (validated strength-neutral at C=75: pc4x800 identical, pc2/pc6
// within noise; landslide decisions finish in ~50 worlds instead of ~900).
// CD_BUDGET=prod is the deployed player-count-aware world budget.
#define CORDITE_KNOBS "CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75"

// Tier order is the strength ladder in docs/IOS_BOT_NAMING.md §1 (the offline
// picker renders it as the road to Moscow, weakest first).
//
// Two notes on the strat ids, both of which were live bugs before this table:
//
//  - `handwritten` -> STRAT_HANDWRITTEN_PROD and `espresso` ->
//    STRAT_ESPRESSO_PROD, the exact mirrors of the production TS bots. The
//    un-suffixed STRAT_HANDWRITTEN / STRAT_ESPRESSO are the arena/rollout
//    variants, which drifted from the production bots and stay frozen because
//    cordite's rollout policy is tuned against them (strategy.h §"production
//    bots ported"). ios_api.c used to point the player-facing rungs at the
//    arena variants, so offline Handwritten was not the site's Handwritten.
//
//  - there is no `cordite_max` / `octogen_max`. octogen_max was an alias of
//    octogen (identical knobs). cordite_max was CD_BUDGET=max, a FLAT
//    120/240/168 world budget that only beats the player-count-aware `prod`
//    schedule at 2-4 players and is roughly HALF of it at 6-8
//    (cordite_strategy.c cd_worlds) — i.e. "Max" was weaker than plain
//    Cordite in the larger games. The tier is folded onto the prod budget.
static const BotRosterEntry ROSTER[] = {
    //  key                 strat                    knobs             logs seeded offline tier
    { "random",           STRAT_RANDOM,           "",                   0,   1,     1,     1  },
    { "simple_heuristic", STRAT_SIMPLE_HEURISTIC, "",                   0,   1,     1,     2  },
    { "handwritten",      STRAT_HANDWRITTEN_PROD, "",                   0,   1,     1,     3  },
    { "espresso",         STRAT_ESPRESSO_PROD,    "",                   1,   0,     1,     4  },
    { "robusta",          STRAT_ROBUSTA,          "",                   1,   0,     1,     5  },
    { "firecracker",      STRAT_FIRECRACKER,      "",                   1,   1,     1,     6  },
    { "gunpowder",        STRAT_GUNPOWDER,        "",                   1,   0,     1,     7  },
    { "blackpowder",      STRAT_BLACKPOWDER,      "",                   1,   1,     1,     8  },
    { "cordite",          STRAT_CORDITE,          CORDITE_KNOBS,        1,   1,     1,     9  },
    { "octogen",          STRAT_OCTOGEN,          "OG_TRUMP_KEEP="
                                                  OCTOGEN_TRUMP_KEEP,   1,   1,     1,     10 },
};
static const int ROSTER_N = (int)(sizeof(ROSTER) / sizeof(ROSTER[0]));

const BotRosterEntry *bot_roster(int *count) {
    if (count) *count = ROSTER_N;
    return ROSTER;
}

int bot_roster_count(void) { return ROSTER_N; }

const BotRosterEntry *bot_roster_at(int idx) {
    if (idx < 0 || idx >= ROSTER_N) return 0;
    return &ROSTER[idx];
}

int bot_roster_find_by_strat(int strat) {
    for (int i = 0; i < ROSTER_N; i++)
        if (ROSTER[i].strat == strat) return i;
    return -1;
}

int bot_roster_find(const char *key) {
    if (!key) return -1;
    for (int i = 0; i < ROSTER_N; i++) {
        const char *a = ROSTER[i].key, *b = key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

// The table is already in tier order, so the offline projection is a filter.
int bot_roster_offline_count(void) {
    int n = 0;
    for (int i = 0; i < ROSTER_N; i++) if (ROSTER[i].offline) n++;
    return n;
}

int bot_roster_offline_at(int n) {
    if (n < 0) return -1;
    for (int i = 0; i < ROSTER_N; i++)
        if (ROSTER[i].offline && n-- == 0) return i;
    return -1;
}

// The roster TABLE is universal — every host reads the same keys, knobs, flags
// and tiers. Which brains a given build can actually RUN is a build property,
// and always has been (sdk/c/Makefile's CORE_SRC vs WASM_BOT_SRC).
//
// FOOLISH_SEEDED_BOTS_ONLY is the shipped bots.wasm: it carries the seeded
// ladder and nothing else. espresso/gunpowder are offline-only rungs the server
// can never dispatch (they are not in seed.sql, and the roster-parity e2e holds
// that true), so linking them there would add ~5KB gzip to every edge cold
// start to run bots that never run — the accidental budget bump
// docs/C_CORE_CONSOLIDATION.md §4.7 warns against. robusta stays reachable for
// free: firecracker IS robusta's MC, so its code is already linked.
//
// An unlinked brain returns -1 (never a silent fallback to `random`, which is
// the failure mode seed.sql's comment is about).
static int dispatch(int strat, const Game *g, int seat, const LegalMoves *moves) {
    switch (strat) {
        case STRAT_RANDOM:           return random_strategy_choose(g, seat, moves, 0);
        case STRAT_SIMPLE_HEURISTIC: return simple_heuristic_strategy_choose(g, seat, moves, 0);
        case STRAT_HANDWRITTEN_PROD: return handwritten_prod_strategy_choose(g, seat, moves, 0);
        case STRAT_ROBUSTA:          return robusta_strategy_choose(g, seat, moves, 0);
        case STRAT_FIRECRACKER:      return firecracker_strategy_choose(g, seat, moves, 0);
        case STRAT_BLACKPOWDER:      return blackpowder_strategy_choose(g, seat, moves, 0);
        case STRAT_CORDITE:          return cordite_strategy_choose(g, seat, moves, 0);
        case STRAT_OCTOGEN:          return octogen_strategy_choose(g, seat, moves, 0);
#ifndef FOOLISH_SEEDED_BOTS_ONLY
        case STRAT_ESPRESSO_PROD:    return espresso_prod_strategy_choose(g, seat, moves, 0);
        case STRAT_GUNPOWDER:        return gunpowder_strategy_choose(g, seat, moves, 0);
#endif
        default:                     return -1;
    }
}

// Can THIS build run entry `idx`'s brain? False only for the offline-only rungs
// in a seeded-bots-only build. The offline library can run everything.
int bot_roster_linked(int idx) {
    const BotRosterEntry *e = bot_roster_at(idx);
    if (!e) return 0;
#ifdef FOOLISH_SEEDED_BOTS_ONLY
    if (e->strat == STRAT_ESPRESSO_PROD || e->strat == STRAT_GUNPOWDER) return 0;
#endif
    return 1;
}

int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves) {
    const BotRosterEntry *e = bot_roster_at(idx);
    if (!e || !moves || moves->n <= 0) return -1;

    bot_knobs_set(e->knobs);
    cordite_reload_flags();
    og_reload_flags();

    int chosen = dispatch(e->strat, g, seat, moves);

    // Leave no knobs installed: a *_strategy_choose called directly afterwards
    // (the arena, a rollout policy) must see its own C defaults, not whatever
    // the last named bot happened to want.
    bot_knobs_clear();
    cordite_reload_flags();
    og_reload_flags();

    if (chosen < 0 || chosen >= moves->n) return -1;
    return chosen;
}

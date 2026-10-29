// The bot registry: every brain and every named bot, one row each, in one C
// table (bots.c). Read bots.c's header comment before adding a bot.
#ifndef CNITRO_BOTS_H
#define CNITRO_BOTS_H

#include <stdint.h>

#include "strategy.h"

// A rung fills an eight-seat table on its own: a human plus seven of it. So a
// seeded bot has exactly this many server accounts (tests.c holds every seeded
// row to all seven).
#define BOT_IDS_MAX 7

// One row of the registry, at index = its STRAT_* id (strategy.h).
//
// Plain data, every field: datagen (shared/tools/datagen --columns) reads the
// fields a host needs out of bots.c with clang and writes the iOS app's ladder
// from them, so they must stay literals. The brain's function is not here but
// in bots.c's dispatch table, at the same index (bot_choose), so a module that
// reads a row does not link a brain.
typedef struct {
    // The brain. `name` is what the arena tools take (cnitro_eval --strategy=)
    // and parse_strategy answers to, with `alias` its short form. NULL `name` is
    // a RETIRED id: the slot stays empty forever (strategy.h).
    const char *name;
    const char *alias;
    // Linked only into the native builds, never the browser's modules
    // (FOOLISH_SEEDED_BOTS_ONLY), which carry only the brains a named bot there
    // dispatches; bots.c's dispatch tables mark the same brains NATIVE_ONLY.
    uint8_t     native_only;

    // The named bot, for a row on the roster (bot_roster.h); NULL `key` is a
    // research brain no host can seat.
    const char *key;        // the DB strategy_key / offline roster name
    const char *knobs;      // "CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75", or ""
    uint8_t     uses_logs;  // belief bot: hydrate the session log before choosing
    uint8_t     seeded;     // dealt onto the website: has `city` and BOT_IDS_MAX ids
    uint8_t     offline;    // shown in the offline picker (docs/IOS_BOT_NAMING.md)
    uint8_t     tier;       // strength order, 1 = weakest; the roster is 1..n, dense

    // The rung's city on the road to Moscow (docs/IOS_BOT_NAMING.md): the
    // nickname of each server account ("%Vienna 1") and the offline picker's
    // caption; NULL for a row with no rung.
    const char *city;
    int32_t     km;         // air distance from `city` to the Kremlin
    // The seeded bot's server accounts, in instance order ("%Vienna 1" first).
    // The real production ids, copied from the hosted store, so a fresh store,
    // the tests and local dev seat the same bots prod does. A new bot gets
    // freshly minted v4 ids when it is seeded, never derived ones.
    const char *ids[BOT_IDS_MAX];
} BotDef;

// Row `strat`, or NULL for an id outside 0..STRAT_COUNT-1 or a retired one.
const BotDef *bot_def(int strat);
// The function of named bot brain `strat` (a row with a key), or NULL for a
// research brain, a non-brain, or one this build does not link. Reading it
// links every brain a named bot runs, and no research brain; bot_linked links
// none. strategy_choose (strategy.h) reaches the research brains too.
StrategyFn bot_choose(int strat);
// 1 when this build links brain `strat` (natively every brain; in the browser's
// modules every one not `native_only`), read off the row alone.
int bot_linked(int strat);

#endif

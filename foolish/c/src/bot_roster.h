// The bot roster: key -> brain + knobs + flags. One table, in C.
//
// docs/C_CORE_CONSOLIDATION.md F1/A1. Before this file the same roster was
// written out three times — the TS registry (bot_strategy.ts) with its env
// knobs, the iOS bridge's ROSTER[] with no knobs at all, and the wasm
// dispatch switch — and the three had already drifted apart in three separate
// ways (§3.1, §3 and the arena/prod mix-up below). A bot's identity is
// kernel data, like the rules; hosts look it up, they do not restate it.
//
// Everything a host needs to run a named bot is here: which brain, at which
// tuning, whether it needs the session log hydrated first, whether it is part
// of the seeded site ladder, whether the offline picker shows it, and where
// it sits in strength order.

#ifndef CNITRO_BOT_ROSTER_H
#define CNITRO_BOT_ROSTER_H

#include <stdint.h>

#include "game.h"
#include "legal.h"

typedef struct {
    const char *key;        // the DB strategy_key / offline roster name
    int         strat;      // STRAT_* brain id (strategy.h)
    const char *knobs;      // "CD_BUDGET=prod,CD_RACE=1,CD_RACE_C=75", or ""
    uint8_t     uses_logs;  // belief bot: hydrate the session log before choosing
    uint8_t     seeded;     // seeded as a live bot on the site (server/impls/supabase/seed.sql)
    uint8_t     offline;    // shown in the offline picker (docs/IOS_BOT_NAMING.md §1)
    uint8_t     tier;       // strength order, 1 = weakest; 0 = unranked
} BotRosterEntry;

// The table, in tier order. `*count` receives the entry count.
const BotRosterEntry *bot_roster(int *count);
int                   bot_roster_count(void);

// Look up by key. -1 when unknown. Callers must handle -1 rather than
// substituting a fallback bot: a silent fallback to `random` is exactly the
// failure mode seed.sql's comment warns about (a bot that plays nothing like
// its name).
int bot_roster_find(const char *key);

// Entry `idx`, or NULL when out of range.
const BotRosterEntry *bot_roster_at(int idx);

// Can this build run entry `idx`'s brain? The shipped bots.wasm links only the
// seeded ladder (FOOLISH_SEEDED_BOTS_ONLY), so the offline-only rungs are
// absent there; the native/offline library runs everything. bot_roster_choose
// returns -1 for an unlinked entry rather than substituting another bot.
int bot_roster_linked(int idx);

// Look up by STRAT_* brain id. -1 when no entry runs that brain.
//
// Only sound because the roster maps brains to entries 1:1 — the table is
// asserted to hold no two entries with the same `strat` (tests.c), which is
// what retiring the `_max` tiers bought: `cordite`/`cordite_max` were two
// entries on STRAT_CORDITE separated only by knobs, and this lookup could not
// have told them apart. Anything that reintroduces a knob-only tier needs the
// seat to carry a roster index rather than a brain id.
int bot_roster_find_by_strat(int strat);

// The offline picker's `n`th rung (tier order) as a roster index, or -1.
// fio_strategy_* on iOS is a thin wrapper over these two.
int bot_roster_offline_count(void);
int bot_roster_offline_at(int n);

// Run entry `idx`'s brain over `moves`, with the entry's knobs installed for
// the duration (env still overrides — see bot_knobs.h). Returns the chosen
// index into moves->moves[], or -1 for an unknown idx / no legal moves.
// This is THE way to run a named bot; calling a *_strategy_choose directly
// gets the C defaults and is therefore only for the arena/research paths.
int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves);

#endif

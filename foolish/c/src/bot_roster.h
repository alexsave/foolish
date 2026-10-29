// The bot roster: the named bots of the registry (bots.c), looked up by key, by
// brain and by tier. The rows themselves are bots.c's; this is the view of
// them every host uses.
//
// docs/C_CORE_CONSOLIDATION.md F1/A1. Before this file the same roster was
// written out three times — the TS registry (bot_strategy.ts) with its env
// knobs, the iOS bridge's ROSTER[] with no knobs at all, and the wasm
// dispatch switch — and the three had already drifted apart in three separate
// ways (§3.1, §3 and the arena/prod mix-up below). A bot's identity is
// kernel data, like the rules; hosts look it up, they do not restate it.
//
// Everything a host needs to run a named bot is in its row: which brain, at which
// tuning, whether it needs the session log hydrated first, whether it is part
// of the seeded site ladder, whether the offline picker shows it, and where
// it sits in strength order.

#ifndef CNITRO_BOT_ROSTER_H
#define CNITRO_BOT_ROSTER_H

#include <stdint.h>

#include "game.h"
#include "legal.h"

#include "bots.h"

// The roster is the registry's named bots (bots.c rows with a `key`), in tier
// order: roster index i is the row with tier i + 1.
int bot_roster_count(void);

// Look up by key. -1 when unknown. Callers must handle -1 rather than
// substituting a fallback bot: a silent fallback to `random` is exactly the
// failure mode seed.sql's comment warns about (a bot that plays nothing like
// its name).
int bot_roster_find(const char *key);

// Entry `idx` (its registry row), or NULL when out of range.
const BotDef *bot_roster_at(int idx);
// Entry `idx`'s STRAT_* brain id, or -1 when out of range.
int bot_roster_strat(int idx);

// Can this build run entry `idx`'s brain (bots.h bot_linked, off the row)? The
// browser's modules link only what a seat there can run (bots.c NATIVE_ONLY),
// so the offline-only rungs and fusion are absent there; the native/offline
// library runs everything. bot_roster_choose returns -1 for an unlinked entry
// rather than substituting another bot.
int bot_roster_linked(int idx);

// Look up by STRAT_* brain id. -1 when no entry runs that brain.
//
// Sound by construction: the registry is indexed by brain, so a brain has at
// most one row and so at most one key. That is what retiring the `_max` tiers
// bought: `cordite`/`cordite_max` were two entries on STRAT_CORDITE separated
// only by knobs, and this lookup could not have told them apart. Anything that
// reintroduces a knob-only tier needs the seat to carry a roster index rather
// than a brain id.
int bot_roster_find_by_strat(int strat);

// The offline picker's `n`th rung (tier order) as a roster index, or -1.
// fio_strategy_* on iOS is a thin wrapper over these two.
int bot_roster_offline_count(void);
int bot_roster_offline_at(int n);

// The roster as a VALUE, for a host on the far side of a boundary: the entries
// above hold pointers, which do not cross one. One row per entry, in table
// order, read through the reader structgen writes from this declaration.
#define BOT_ROSTER_MAX     16
#define BOT_ROSTER_KEY_MAX 24
typedef struct {
    int32_t strat;       // STRAT_* brain id (strategy.h)
    bool    linked;      // this build links the brain (bot_roster_linked)
    bool    uses_logs;
    bool    seeded;
    bool    offline;
    uint8_t tier;
    uint8_t key_len;
    char    key[BOT_ROSTER_KEY_MAX];   // key_len bytes, not NUL-terminated
} BotRosterRow;
typedef struct {
    int32_t      n;
    BotRosterRow rows[BOT_ROSTER_MAX];
} BotRosterTable;

// Fills `out` and returns the row count. A table or a key that outgrew the
// struct is -1 with `out->n` 0: a refusal, never a shorter roster that would
// read as a bot going missing.
int bot_roster_table(BotRosterTable *out);

// Run entry `idx`'s brain over `moves`, with the entry's knobs installed for
// the duration (env still overrides — see bot_knobs.h). Returns the chosen
// index into moves->moves[], or -1 for an unknown idx / no legal moves.
// This is THE way to run a named bot; calling a *_strategy_choose directly
// gets the C defaults and is therefore only for the arena/research paths.
int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves);

#endif

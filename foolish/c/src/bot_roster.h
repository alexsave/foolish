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

// ---- a bot's stored name, and how a host shows it ----------------------------
//
// A seeded bot is STORED by its rung's city, in English, behind the reserved
// '%': "%<city> <n>", the nickname of its n-th server account
// (server/src/store/bot_seed.c) and of the promo page's local bot
// (local_table.c). The stored name is the language-neutral fact a replay blob
// carries; a page says the city in its reader's language. Which city a name
// names, and which of its accounts, is this file's answer, so no host parses a
// nickname. The city's words are c/i18n's `bot.<key>`, which no wasm module
// carries: the host says them.
#define BOT_NICK_PREFIX   '%'
#define BOT_NICK_SHOW_MAX 64           // == ROSTER_NAME_MAX: a stored name's bytes
#define BOT_NICK_HUMAN    0            // not a bot's name: show it as stored (`show`)
#define BOT_NICK_CITY     1            // a rung's account: the row's city (bot.<key>), then `instance`
#define BOT_NICK_STORED   2            // a bot's name no row has (an old replay's '%Octogen 1'): show `show`
typedef struct {
    int8_t   kind;                       // BOT_NICK_*
    uint8_t  key_len;                    // BOT_NICK_CITY: the row's key, whose string is bot.<key>
    char     key[BOT_ROSTER_KEY_MAX];
    int16_t  instance;                   // BOT_NICK_CITY: the account's number, 1 first
    uint8_t  show_len;                   // HUMAN and STORED: what to show (the name, minus '%' for a bot)
    char     show[BOT_NICK_SHOW_MAX];
} BotNickname;

// What `name` (len bytes of UTF-8) is: BOT_NICK_*, with `out` filled. A city
// matches a named row's city exactly, and the instance is one or more digits
// after one space; anything else behind '%' is BOT_NICK_STORED.
int bot_nickname_read(const char *name, int len, BotNickname *out);

// The stored name of brain `strat`'s `instance`-th account, "%<city> <n>", into
// `out` (not NUL-terminated): its length, or -1 for a row with no city, an
// instance below 1, or a name over `cap`.
int bot_nickname_write(int strat, int instance, char *out, int cap);

// Run entry `idx`'s brain over `moves`, with the entry's knobs installed for
// the duration (env still overrides — see bot_knobs.h). Returns the chosen
// index into moves->moves[], or -1 for an unknown idx / no legal moves.
// This is THE way to run a named bot; calling a *_strategy_choose directly
// gets the C defaults and is therefore only for the arena/research paths.
int bot_roster_choose(int idx, const Game *g, int seat, const LegalMoves *moves);

#endif

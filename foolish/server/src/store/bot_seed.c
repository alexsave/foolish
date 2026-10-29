// bot_seed.c - THE BOTS foolish seeds (store_import.h cb_bot_seed_rows): the
// accounts of every SEEDED bot in the kernel's registry (foolish/c/src/bots.c),
// in ladder order. A seeded row carries its city and BOT_IDS_MAX ids, and its
// accounts are "%<city> 1" .. "%<city> 7" with those ids, at the start rating.
//
// The ids are production's own: the hosted store minted them (random v4) and
// the migration carried them, and bots.c holds them as data, so a fresh store,
// the tests and local dev seat exactly the bots prod does. That is also what
// makes the seed safe to run against a store that has bots (cb_import_seed_bots
// inserts the ids it lacks and brings a stored id's nickname to its row's
// city, never its rating): ids derived from the names instead would have added
// 49 duplicates beside prod's rows. unit_store.c holds the derived set to
// prod's (id, nickname, strategy key) triples plus the accounts prod lacks.
//
// '%' is the reserved first character of a bot's name (c_base auth/names.c
// refuses it in a human's).
#include "store/store_import.h"

#include "bot_roster.h"
#include "table.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

int cb_bot_seed_rows(cb_bot_row *out, int cap) {
    int n = 0;
    for (int r = 0; r < bot_roster_count(); r++) {
        const BotDef *d = bot_roster_at(r);
        if (!d || !d->seeded) continue;
        for (int i = 0; i < BOT_IDS_MAX; i++) {
            if (!d->city || !d->ids[i]) return -1;   // a seeded row without its accounts (tests.c refuses it)
            if (n == cap) return -1;
            cb_bot_row *b = &out[n++];
            memset(b, 0, sizeof *b);
            cb_strlcpy(b->id, d->ids[i], sizeof b->id);
            snprintf(b->nickname, sizeof b->nickname, "%%%s %d", d->city, i + 1);
            cb_strlcpy(b->strategy_key, d->key, sizeof b->strategy_key);
            b->elo_rating = TABLE_RATING_START;
            b->previous_elo = -1;
        }
    }
    return n;
}

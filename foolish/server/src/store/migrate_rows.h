// migrate_rows.h - DynamoDB's items into the SQL store's tables, for cbase-export
// (foolish/server/migrate/export.c): each item decoded with its family's codec
// (rows_auth.c, rows_foolish.c: the item layout production holds) into its row, the row
// written through the SQL store's own operations, and each row looked up again by its
// natural key and compared field by field.
//
// THE FAMILIES (pk / sk -> table), in the order a copy must take them (cb_mig_rank):
//   0 USER#<id> PROFILE      -> users          (a canonical name already taken fails loudly)
//   0 EMAIL#<email> USER     -> users.email    (checked, not written: the account row holds it)
//   1 USER#<id> RATING       -> ratings
//   1 BOT#<id> BOT           -> bots           (all of them replace the seed, at cb_mig_finish)
//   2 GAME#<id> BOT#<bot>    -> game_bots      (held until the game's ROW, which sorts after)
//   2 GAME#<id> MEMBER#<u>   -> game_members   (likewise)
//   2 GAME#<id> ROW          -> games, and its logs_packed as one game_moves row
//   3 SNAP#<id> PLAYER#<u>   -> replay_players (held until the replay; the summary comes from them)
//   3 SNAP#<id> SNAP         -> replays
//   4 CHAT#<game> <t>#<id>   -> chat
//   5 SESSION#<id> SESSION   -> sessions       (past its end: skipped, as a reader treats it as gone)
//   5 REFRESH#<hash> TOKEN   -> refresh_tokens (likewise)
//   5 AUTHFAIL#<email> FAILS -> signin_fails   (likewise)
//   - SCAN GAME#<id>         skipped: the bot scan's marker and lease, which the SQL store does not keep
// Anything else is unknown, and a failure: nothing is dropped silently.
#ifndef CB_MIGRATE_ROWS_H
#define CB_MIGRATE_ROWS_H

#include "store/rows_foolish.h"

#include <stdio.h>

typedef struct cb_mig cb_mig;

// A copy into dst (the SQL store); `now_ms` decides what has expired. Messages go to `log`.
cb_mig *cb_mig_new(cb_store *dst, int64_t now_ms, FILE *log);
void cb_mig_free(cb_mig *m);

// The family's place in the copy's order (above), -1 for an unknown family. The items go in
// (rank, pk, sk) order.
int cb_mig_rank(const char *pk, const char *sk);

// One item. CB_OK (written, held for its owner, or skipped by rule), or an error (logged, counted).
int cb_mig_put(cb_mig *m, const cb_item *it);
// The held rows written (the bots). CB_OK or an error.
int cb_mig_finish(cb_mig *m);

// Does dst hold what the item says, by the row's natural key? 1 yes, 0 no (logged), or a CB_E_*.
// An item skipped by rule answers 1.
int cb_mig_same(cb_mig *m, const cb_item *it);

// The tallies: per family, the items seen, written, skipped and failed; per table, the rows dst
// holds against the rows the items make. The number of failed checks (0: every count agrees and
// nothing failed).
int cb_mig_report(cb_mig *m, FILE *out);

#endif

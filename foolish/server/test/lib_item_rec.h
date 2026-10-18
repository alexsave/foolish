// lib_item_rec.h - a store that records every item request it is sent and
// forwards it to a real one: the tool for holding what each row operation
// (rows_foolish.c, rows_auth.c) asks of the item layer, request by request.
//
// ir_wrap(inner) is a cb_store whose item layer (store.h item_*) writes one
// text line per request into a log and then forwards it to `inner`; the
// accounts and auth rows are rows_auth.c's own over that item layer, the
// product rows foolish's (cb_foolish_store), so every request they make is
// recorded. Closing it closes `inner`.
//
// A LINE spells a request (values in full, bytes by length):
//   get <pk> <sk>
//   put <pk> <sk> [if <cond>] attrs <name>=<v>,...        (ttl=<ms> when set)
//   update <pk> <sk> [if <cond>] sets <name>=<v>|<name>?=<v>|<name>+=<v>|<name>-,...
//   delete <pk> <sk> [if <cond>]
//   check <pk> <sk> if <cond>
//   txn <n>                    then its writes, each on its own line after "  "
//   query <t|g1|g2> <pk> [^<prefix>] [<<below>] fwd|back [keys] [page=<n>]
//   batch_get <n> [proj <a>,<b>]   then "  <pk> <sk>" a key
//   batch_delete <n>           then "  <pk> <sk>" a key
//   keys
// where <v> is "text" (S), a number (N), true/false (BOOL), b<len> (B), or
// [a;b] (SL), and <cond> is clauses joined by " and ": exists, absent,
// <name>=<v>, <name>!=<v>, <name><<v>, has <name>, <name>=<v>|absent,
// <name><<v>|absent. A write asked for its item on a refusal ends "(old)".
#ifndef CB_LIB_ITEM_REC_H
#define CB_LIB_ITEM_REC_H

#include "store/rows_foolish.h"

#include <stdbool.h>

cb_store *ir_wrap(cb_store *inner);
cb_store *ir_inner(cb_store *s);

// The log since the last take, NUL-terminated lines each ending '\n'; and
// cleared.
const char *ir_log(cb_store *s);
void ir_clear(cb_store *s);
// The number of lines of the log that begin with `prefix` (after any indent
// when `prefix` does not begin with a space).
int ir_count(cb_store *s, const char *prefix);
// The line beginning with `prefix` (as ir_count), or NULL: valid until the
// next request.
const char *ir_line(cb_store *s, const char *prefix);
// Whether some line holds `text`.
bool ir_has(cb_store *s, const char *text);

// The next n transactions are refused without being forwarded, as a
// condition that fails at write `failed` would refuse them (*failed_had_item
// `had`): what another writer's move between the read and the write does.
void ir_refuse_txns(cb_store *s, int n, int failed, bool had);

#endif

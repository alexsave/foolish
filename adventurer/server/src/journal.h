// journal.h - the SQLite side of the world (sql/schema.sql): append a user or a result, and read
// everything back at start. Writes go through the platform's group commit (store_sql.h
// cb_sql_write), so a burst of results shares one transaction and one WAL write.
#ifndef ADV_JOURNAL_H
#define ADV_JOURNAL_H

#include "store/store.h"
#include "wire.h"
#include "world.h"

// The product's statements (cb_store_app sql_stmts), by index.
enum { ADV_SQL_USER_PUT = 0, ADV_SQL_USERS_ALL, ADV_SQL_ACT_PUT, ADV_SQL_ACTS_ALL, ADV_SQL_ACT_MAX, ADV_SQL_N };
extern const char *const adv_sql_stmts[ADV_SQL_N];

int adv_journal_user_put(cb_store *s, const adv_w_user_def *d);
int adv_journal_act_put(cb_store *s, const adv_result *r);
// Reads the roster and every activity into the world, in id order, through adv_world_apply;
// *recovered counts the activities. Also sets the world's next activity id. CB_OK or an error.
int adv_journal_recover(cb_store *s, adv_world *w, uint32_t *recovered);
// Bytes the journal's tables hold on disk (the database file's size; 0 when unknown).
uint64_t adv_journal_bytes(const char *db_path);

#endif

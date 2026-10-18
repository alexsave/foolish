// rows_foolish.h - foolish's rows over the item layer (rows_foolish.c): the
// row types, the operations table every store carries for them (the
// product's half of docs/PLATFORM.md option 1: cb_store_app rows), and the
// account hooks a signup and a delete run.
//
// CALLS go through CB_ROWS_CALL, the twin of store.h's CB_STORE_CALL (the
// same busy mark), on the table the store was opened with: rows_foolish.c's
// over the store's items by default (cb_foolish_store, which cb_foolish_app
// names), a test double's own otherwise.
//
// CONSISTENCY is the same "fake consistent server" the TypeScript backend had,
// rebuilt on a compare-and-swap (DESIGN.md "Why Lambda"):
//   * a game commit carries the version it read, and loses (CB_E_CONFLICT) if
//     the row moved on; the caller reloads and re-runs (app/table_ops.h);
//   * a bot drive holds a lease (lease_acquire/renew/release, 25 s TTL);
//   * nothing else needs ordering: users, chat and snapshots are single
//     writes.
// Game ids are 12 hex characters (6 before). Byte fields are owned by the
// row struct that holds them (malloc'd by the store, freed by the matching
// *_free).
#ifndef CB_ROWS_FOOLISH_H
#define CB_ROWS_FOOLISH_H

#include "store/store.h"

#define CB_GAME_ID_MAX   16    // games.id is 12 hex (app_internal.h CB_GAME_ID_HEX), 6 for older games
#define CB_SEED_LEN      32    // == kernel FOOLISH_SEED_LEN
#define CB_MAX_SEATS     8     // == kernel MAX_PLAYERS

// ---- rows ------------------------------------------------------------------------

// A game as committed: the kernel's bytes plus the scalar columns.
typedef struct {
    char     id[CB_GAME_ID_MAX + 1];
    int      status;            // kernel GAME_STATUS_*
    uint8_t *state;   size_t state_len;    // kernel state blob
    uint8_t *roster;  size_t roster_len;   // kernel durable roster (ROSTER_BYTES)
    uint8_t *logs;    size_t logs_len;     // session log (games.logs_packed); NULL/0 for none
    bool     has_seed;
    uint8_t  seed[CB_SEED_LEN];            // the deal seed, when has_seed
    bool     needs_bots;
    int64_t  version;           // the CAS token; clients gate on it
    int64_t  round_epoch;
    int64_t  created_at, updated_at, last_commit_at;
    // DynamoDB's: the game's seat items are known to be within this row's
    // roster (written by the create or the commit that wrote the row; a seat
    // whose account is gone may be missing, none is extra). A commit fenced on
    // this row may then take the roster as the stored seats instead of reading
    // them (cb_game_commit prev_known). game_create and game_commit leave it
    // true; an imported row, a row written before 2026-10-18 and every SQLite
    // row read as false.
    bool     seats_exact;
} cb_game_row;

void cb_game_row_free(cb_game_row *g);   // frees the byte fields; the struct is the caller's

// One commit of a game: the row as it is to become, written only if the stored
// version is still `expected_version` (the CAS). With it, atomically:
// membership (the humans and bots the roster seats now; anyone not listed is
// removed) and the rating changes the kernel computed (TableCommit.n_rated).
typedef struct {
    const cb_game_row *row;     // row->version is the NEW version (expected + 1)
    int64_t  expected_version;
    int      n_humans;
    cb_uuid  humans[CB_MAX_SEATS];
    int      n_bots;
    cb_uuid  bots[CB_MAX_SEATS];
    // Ratings: by seat id, applied as rating = max(0, rating + delta),
    // games_played + 1, previous_elo = the old rating. A human with no rating
    // row gets one (from TABLE_RATING_START) if the user exists. n_rated 0: none.
    int      n_rated;
    cb_uuid  rated_ids[CB_MAX_SEATS];
    bool     rated_is_bot[CB_MAX_SEATS];
    int32_t  rating_delta[CB_MAX_SEATS];
    // The names the leaderboard shows for the rated humans, by rated index.
    char     rated_names[CB_MAX_SEATS][CB_USERNAME_MAX + 1];
    // The seats the row at expected_version holds (its roster's humans and
    // bots), given when that row's seats_exact was true: the fence makes them
    // the stored seats whenever the commit lands, and DynamoDB writes the
    // difference without first reading them (one Query less, under the
    // game's order lock). prev_known false: the store reads them.
    // prev_needs_bots is that row's needs_bots, read only with prev_known:
    // false there means the game has no SCAN marker, so on DynamoDB a commit
    // that writes no seat, no rating and needs_bots false is one conditional
    // UpdateItem instead of a transaction (store_dynamo.c, DESIGN.md "Store").
    bool     prev_known;
    bool     prev_needs_bots;
    int      n_prev_humans;
    cb_uuid  prev_humans[CB_MAX_SEATS];
    int      n_prev_bots;
    cb_uuid  prev_bots[CB_MAX_SEATS];
} cb_game_commit;

// A dashboard entry: a game a user sits at.
typedef struct {
    char    game_id[CB_GAME_ID_MAX + 1];
    int64_t updated_at;
} cb_member_row;

typedef struct {
    cb_uuid id;
    char    nickname[CB_USERNAME_MAX + 1];   // '%'-prefixed
    char    strategy_key[24];                // kernel bot_roster key
    int32_t elo_rating;
    int32_t previous_elo;                    // -1 for none
    int32_t games_played;
    int64_t created_at, updated_at;
} cb_bot_row;

typedef struct {
    cb_uuid user_id;
    char    username[CB_USERNAME_MAX + 1];   // "" after an account is deleted
    int32_t elo_rating;
    int32_t previous_elo;                    // -1 for none
    int32_t games_played;
    int64_t updated_at;
} cb_rating_row;

// What the history list shows of a replay, cut once when the game ends (the
// kernel's replay_summary_v6 and the extras' gaps) and written into every
// player's pointer, so the list is one Query and no copy holds a name.
typedef struct {
    bool    has;                             // false: a pointer from before the summary, or none given
    int8_t  fool;                            // seat, -1 for none
    int8_t  n_elim;
    int8_t  elim[CB_MAX_SEATS];              // seats in the order they went out; the fool is not one
    int32_t duration_s;                      // -1 when the extras time nothing
} cb_snap_summary;

typedef struct {
    cb_uuid  id;
    char     game_id[CB_GAME_ID_MAX + 1];    // "" once the game is deleted
    int      n_players;
    cb_uuid  player_ids[CB_MAX_SEATS];       // the seated humans, seat order
    uint8_t *moves;  size_t moves_len;       // kernel v6 replay code
    uint8_t *extras; size_t extras_len;      // kernel replay extras
    int64_t  created_at;
    cb_snap_summary sum;                     // on put: the game's; the pointers hold the per-player part below
    // Filled by a query for its viewer (a summary_only row holds no moves,
    // extras or player ids): their seat and rank (1 = first out, n = the fool).
    int      seat, rank;
} cb_snapshot_row;

void cb_snapshot_row_free(cb_snapshot_row *s);

// Seat's rank in a summary: 1 = first out, n_players = the fool (never in the
// elimination order). The one rule the pointer writer and a store's query share.
static inline int cb_snapshot_rank(const cb_snap_summary *m, int n_players, int seat) {
    for (int i = 0; i < m->n_elim; i++)
        if (m->elim[i] == seat) return i + 1;
    return n_players;
}

typedef struct {
    cb_uuid id;
    char    game_id[CB_GAME_ID_MAX + 1];
    cb_uuid user_id;
    char   *message;      // owned, NUL-terminated
    bool    is_system;
    int64_t created_at;
} cb_chat_row;

void cb_chat_row_free(cb_chat_row *c);

// What a snapshot query asks (PostgREST's game_snapshots shapes).
typedef struct {
    const char *viewer_id;   // REQUIRED: only snapshots whose player_ids contain it
    const char *game_id;     // NULL: any
    const char *id;          // NULL: any
    int         limit;       // 0: no limit
    bool        newest_first;
    const char *ref;         // NULL: any; else the game id OR the snapshot id (the history row's id opens its replay)
    bool        summary_only;   // the viewer's pointers alone: no BatchGetItem, no moves/extras/player ids (history)
} cb_snapshot_query;

typedef bool (*cb_member_fn)(const cb_member_row *r, void *ud);
typedef bool (*cb_game_fn)(const cb_game_row *r, void *ud);
typedef bool (*cb_bot_fn)(const cb_bot_row *r, void *ud);
typedef bool (*cb_rating_fn)(const cb_rating_row *r, void *ud);
typedef bool (*cb_snapshot_fn)(const cb_snapshot_row *r, void *ud);
typedef bool (*cb_chat_fn)(const cb_chat_row *r, void *ud);

// ---- the operations ---------------------------------------------------------------

typedef struct cb_rows_ops {
    // games
    // CB_E_NOT_FOUND, or the row (caller frees with cb_game_row_free).
    int (*game_get)(cb_store *s, const char *id, cb_game_row *out);
    // A new game at version 0 with its creator as member. CB_E_CONFLICT when
    // the id is taken (the caller draws another, up to 5 times).
    int (*game_create)(cb_store *s, const cb_game_row *g, const char *creator_id);
    // THE CAS. CB_OK when written, CB_E_CONFLICT when the stored version is not
    // c->expected_version (nothing written), CB_E_NOT_FOUND when the game is
    // gone. The store stamps last_commit_at with row->updated_at (every commit
    // moves the version); row->last_commit_at is not read. A seated human with
    // no account (deleted; the roster keeps the id) is skipped, not refused.
    int (*game_commit)(cb_store *s, const cb_game_commit *c);
    // Deletes the game (the last seat left: TABLE_EMPTY) if still at
    // expected_version: CB_E_CONFLICT when it moved, CB_E_NOT_FOUND when gone.
    // Its seats and chat go with it; its replays stay with game_id "".
    int (*game_delete)(cb_store *s, const char *id, int64_t expected_version);
    // The games a user sits at, most recently updated first (the dashboard);
    // limit 0: all. DynamoDB answers from an index, so a seat taken a moment
    // ago may be missing for a moment, and "updated" there is when the seat
    // was taken (a commit writes only the seats that change): a caller that
    // needs the games' own order sorts the rows it loads, as PostgREST does.
    int (*games_of_user)(cb_store *s, const char *user_id, int limit, cb_member_fn fn, void *ud);
    // The rows of `ids` for a list of views (the dashboard): every column but
    // the session log (logs NULL, logs_len 0), read consistently. fn once per
    // game that exists, in the order of ids (a missing id is skipped); false
    // from fn stops. The number of rows handed to fn, or a CB_E_*. DynamoDB
    // reads them in BatchGetItems of up to 100 keys, not one GetItem each.
    int (*games_get_views)(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud);
    // 1 a member, 0 not, or a CB_E_*.
    int (*is_member)(cb_store *s, const char *game_id, const char *user_id);
    // The humans seated at the game (bots are not members here), read
    // consistently: fn once with each user id, in no particular order; false
    // from fn stops. The number handed to fn, or a CB_E_* (no game is 0). A
    // game's chat is published to each of them (realtime.h rt_publish_chat).
    int (*game_members)(cb_store *s, const char *game_id, cb_name_fn fn, void *ud);
    // The heartbeat's scan: needs_bots, updated_at < stale_before,
    // last_commit_at > active_after, at most `limit`.
    int (*games_needing_bots)(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn, void *ud);
    // The bot lease (today's try_acquire/renew/release_bot_lease): a random
    // token valid until now + ttl_ms. acquire: CB_OK and the token, or
    // CB_E_CONFLICT while another holder's lease is live (CB_E_NOT_FOUND: no
    // such game, or a game with no bot work, which has no lease to take).
    // renew: CB_OK, or CB_E_CONFLICT when the token no longer holds it.
    // release: CB_OK (a lease that is not the token's is left alone). A
    // commit that leaves bot work keeps the lease. acquire and renew move the
    // scan's staleness clock (games_needing_bots' updated_at) to `now`, never
    // last_commit_at, as the update_games_updated_at trigger did, so the scan
    // sees a drive in flight. SQLite keeps that clock on the row, so game_get
    // sees it move; DynamoDB keeps it, and the lease, on the game's small
    // SCAN marker (1 WCU a write), so the row's updated_at is its last commit's.
    int (*lease_acquire)(cb_store *s, const char *game_id, int64_t now, int64_t ttl_ms, cb_uuid token_out);
    int (*lease_renew)(cb_store *s, const char *game_id, const char *token, int64_t now, int64_t ttl_ms);
    int (*lease_release)(cb_store *s, const char *game_id, const char *token);
    // A drive found that NO BOT CAN ACT on the row at `version` (table.h
    // table_bots_due) although the row is in the scan, which a row written
    // before the scan asked that question can be: takes the game out of the
    // scan, unless a commit has moved the row past `version` (that commit
    // wrote the scan's answer itself). SQLite clears the row's needs_bots;
    // DynamoDB deletes the SCAN marker (and the lease on it), fenced on the
    // row version the marker carries, and leaves the row's own needs_bots to
    // its next commit (the row is 4 to 8 KB a write, the marker 1 WCU).
    // CB_OK, or CB_E_CONFLICT when the row has moved on (SQLite: or is gone).
    int (*bots_idle)(cb_store *s, const char *game_id, int64_t version);
    // 1 while some drive's lease on the game is live at `now` (acquire would
    // answer CB_E_CONFLICT), 0 when it is free or there is none (no game, or
    // no bot work), or a CB_E_*. A consistent read: a kick asks it after its
    // commit, so it sees any lease taken before (app/bots.c kick_wanted).
    int (*lease_held)(cb_store *s, const char *game_id, int64_t now);

    // bots and ratings
    int (*bots_list)(cb_store *s, cb_bot_fn fn, void *ud);                 // ladder order
    // EVERY rating row the leaderboard index holds, highest first: the played
    // ones, and on DynamoDB any row still in the layout before the sparse key
    // (which cbase-import --reindex-ratings takes out). A read that grows with
    // the players, for that tool and tests, never for a request.
    int (*ratings_list)(cb_store *s, cb_rating_fn fn, void *ud);
    // The leaderboard's read: the players who have played (games_played > 0),
    // highest rating first, then user id, at most `limit` of them. Bounded
    // whatever the number of sign-ups: on DynamoDB it is one Query of `limit`
    // items on GSI2's rating-ordered key, which only played rows carry
    // (aws_json.h "THE ITEM LAYOUT", DESIGN.md "Edge cache").
    int (*ratings_top)(cb_store *s, int limit, cb_rating_fn fn, void *ud);
    // seat_ratings: ids[i]'s rating (a bot's, else a user's), or -1
    // (TABLE_RATING_NONE) when it has none, into out[i].
    int (*ratings_for)(cb_store *s, const char *const *ids, int n, int32_t *out);
    // The leaderboard name; NULL clears it (delete-account).
    int (*rating_set_username)(cb_store *s, const char *user_id, const char *username);

    // snapshots
    int (*snapshot_put)(cb_store *s, const cb_snapshot_row *r);
    int (*snapshots_query)(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud);
    // Account deletion: one replay's extras replaced (the kernel's
    // replay_extras_redact), on every copy of the row; the moves, player_ids,
    // game and date stay. extras_len 0 clears them. CB_E_NOT_FOUND when the
    // snapshot is gone. A user's snapshots are snapshots_query by viewer_id.
    int (*snapshot_set_extras)(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len);

    // chat
    int (*chat_put)(cb_store *s, const cb_chat_row *r);
    // Oldest first, the newest `limit` (0: all).
    int (*chat_list)(cb_store *s, const char *game_id, int limit, cb_chat_fn fn, void *ud);

    // migration (foolish/server/migrate/import.c): rows written as they are, with no side
    // effects. Nothing but the import calls these.
    // The rating row as given (username "" stores none), insert or replace.
    int (*rating_import)(cb_store *s, const cb_rating_row *r);
    // The row's leaderboard index key brought to the current layout, the
    // values untouched and only while they are still r's (a commit that
    // raced it already wrote the new key): CB_OK, or CB_E_CONFLICT when the
    // row changed, is gone, or already has that key. SQLite has no index
    // key, so every row already has it: CB_E_CONFLICT.
    int (*rating_reindex)(cb_store *s, const cb_rating_row *r);
    // Every bot row replaced by these n (the seeded ones included), atomically.
    int (*bots_replace)(cb_store *s, const cb_bot_row *rows, int n);
    // The game at its own version, with exactly these members, insert or
    // replace. The lease is left free.
    int (*game_import)(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                       int n_bots);

} cb_rows_ops;

// The table a store carries (none: every operation CB_E_NOTIMPL).
const cb_rows_ops *cb_rows_of(const cb_store *s);

//   int rc = CB_ROWS_CALL(ctx->store, game_get, id, &row);
//
// A table that leaves an entry NULL answers CB_E_NOTIMPL for it.
#define CB_ROWS_CALL(s, fn, ...) \
    (cb_store_begin_(), cb_store_end_(cb_rows_of(s)->fn ? cb_rows_of(s)->fn((s), __VA_ARGS__) : CB_E_NOTIMPL))

// foolish on every store: rows_foolish.c's operations, and its part of an
// account (a signup's rating row; a delete's seats, chat and rating row).
extern const cb_rows_ops  cb_rows_foolish;
extern const cb_store_app cb_foolish_store;

// games_get_views for a store with no round trip to save (SQLite, the tests'
// memory store): game_get for each id, its log dropped.
int cb_store_games_get_each(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud);

// The orders the list operations promise. Ladder order (bots_list): the
// kernel's strength tier of the strategy key (bot_roster.h, weakest first;
// an unknown key last), then the nickname. Leaderboard order
// (ratings_list): rating highest first, then user id.
int  cb_store_bot_ladder_cmp(const cb_bot_row *a, const cb_bot_row *b);
void cb_store_bots_sort(cb_bot_row *rows, int n);
void cb_store_ratings_sort(cb_rating_row *rows, int n);

// One write and the storage it points into (the bench and the tests build
// the requests a commit sends without a store).
typedef struct {
    cb_iwrite w;
    cb_iset   sets[16];
    cb_av     attrs[20];
    char      pk[400];
} cb_rows_write;

// The lone commit's write: the CAS on the row (rows_foolish.c "THE CAS").
void cb_rows_cas_write(const cb_game_commit *c, int64_t last_commit_at, cb_rows_write *out);
// The game row, put whole (as a create or an import writes it).
void cb_rows_game_put_write(const cb_game_row *g, cb_rows_write *out);
// An item read into its row (byte fields and a chat's message malloc'd):
// false when it is not one (a required attribute missing or mistyped). The
// store's reads use them; the fuzz targets reach them directly.
bool cb_rows_game_of(const cb_item *it, cb_game_row *g);       // GAME#<id> ROW
bool cb_rows_bot_of(const cb_item *it, cb_bot_row *b);         // BOT#<id> BOT
bool cb_rows_rating_of(const cb_item *it, cb_rating_row *r);   // USER#<id> RATING
bool cb_rows_snapshot_of(const cb_item *it, cb_snapshot_row *s);   // SNAP#<id> SNAP
bool cb_rows_chat_of(const cb_item *it, cb_chat_row *c);       // CHAT#<game> <created>#<id>

int cb_rows_game_get(cb_store *s, const char *id, cb_game_row *out);
int cb_rows_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud);
int cb_rows_game_create(cb_store *s, const cb_game_row *g, const char *creator);
int cb_rows_game_commit(cb_store *s, const cb_game_commit *c);
int cb_rows_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                        int n_bots);
int cb_rows_game_delete(cb_store *s, const char *id, int64_t expected);
int cb_rows_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud);
int cb_rows_is_member(cb_store *s, const char *game, const char *user);
int cb_rows_game_members(cb_store *s, const char *game, cb_name_fn fn, void *ud);
int cb_rows_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                               void *ud);
int cb_rows_lease_acquire(cb_store *s, const char *game, int64_t now, int64_t ttl, cb_uuid token_out);
int cb_rows_lease_renew(cb_store *s, const char *game, const char *token, int64_t now, int64_t ttl);
int cb_rows_lease_release(cb_store *s, const char *game, const char *token);
int cb_rows_bots_idle(cb_store *s, const char *game, int64_t version);
int cb_rows_lease_held(cb_store *s, const char *game, int64_t now);
int cb_rows_bots_list(cb_store *s, cb_bot_fn fn, void *ud);
int cb_rows_ratings_list(cb_store *s, cb_rating_fn fn, void *ud);
int cb_rows_ratings_top(cb_store *s, int limit, cb_rating_fn fn, void *ud);
int cb_rows_ratings_for(cb_store *s, const char *const *ids, int n, int32_t *out);
int cb_rows_rating_set_username(cb_store *s, const char *user, const char *username);
int cb_rows_rating_import(cb_store *s, const cb_rating_row *rt);
int cb_rows_rating_reindex(cb_store *s, const cb_rating_row *rt);
int cb_rows_bots_replace(cb_store *s, const cb_bot_row *rows, int n);
int cb_rows_snapshot_put(cb_store *s, const cb_snapshot_row *sn);
int cb_rows_snapshots_query(cb_store *s, const cb_snapshot_query *q, cb_snapshot_fn fn, void *ud);
int cb_rows_snapshot_set_extras(cb_store *s, const char *id, const uint8_t *extras, size_t extras_len);
int cb_rows_chat_put(cb_store *s, const cb_chat_row *c);
int cb_rows_chat_list(cb_store *s, const char *game, int limit, cb_chat_fn fn, void *ud);


#endif

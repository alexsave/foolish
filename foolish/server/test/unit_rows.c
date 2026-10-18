// unit_rows.c - foolish's rows (store/rows_foolish.c) over a store that records
// every item request (lib_item_rec.h), operation by operation: the keys, the
// conditions and the attributes each row operation sends, and how it reads
// the answers back (the CAS and the lone commit, the lease, seats, ratings,
// the replay and its pointers, chat, the imports, bots_replace, the account's
// signup and cascade), and SQLite's bot seed at open.
//
// The recorded store forwards to a real one (SQLite, or DynamoDB Local with
// CBASE_STORE=dynamo), so each answer is a backend's; unit_store.c holds the
// answers themselves on both backends, this file the requests.
#include "t.h"
#include "lib_item_rec.h"
#include "lib_store_backend.h"

#include "store/rows_foolish.h"
#include "store/store_import.h"
#include "table.h"

#include <stdlib.h>
#include <unistd.h>

#define UA "aaaaaaaa-0000-4000-8000-00000000000a"
#define UB "aaaaaaaa-0000-4000-8000-00000000000b"
#define UC "aaaaaaaa-0000-4000-8000-00000000000c"
#define UZ "aaaaaaaa-0000-4000-8000-00000000000f"   // never an account
#define HASH "$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234"
#define GID "rows01"
#define SN1 "cccccccc-0000-4000-8000-000000000001"
#define CH1 "dddddddd-0000-4000-8000-000000000001"
#define CB_BOT_SEED_FIRST "2c601cde-63d2-5291-8fcb-735f5860b1ec"   // CB_BOT_SEED[0], first in ladder order

static char g_dir[256];
static int g_seq;

static cb_store *open_rec(void) {
    char path[320];
    snprintf(path, sizeof path, "%s/r%d.sqlite", g_dir, ++g_seq);
    cb_store *inner = tsb_open_fresh(path);
    if (!inner) { fprintf(stderr, "FAIL [%s] no store to test against\n", t_current); exit(1); }
    return ir_wrap(inner);
}

static cb_user_row mkuser(const char *id, const char *email, const char *name) {
    cb_user_row u = { 0 };
    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.email, sizeof u.email, "%s", email);
    snprintf(u.password_hash, sizeof u.password_hash, HASH);
    snprintf(u.username, sizeof u.username, "%s", name);
    u.created_at = 1;
    return u;
}

static uint8_t g_state[3] = { 1, 2, 3 }, g_roster[5] = { 9, 8, 7, 6, 5 };

static cb_game_row mkgame(const char *id, int64_t at) {
    cb_game_row g = { 0 };
    snprintf(g.id, sizeof g.id, "%s", id);
    g.state = g_state;
    g.state_len = sizeof g_state;
    g.roster = g_roster;
    g.roster_len = sizeof g_roster;
    g.created_at = g.updated_at = g.last_commit_at = at;
    return g;
}

// The line of the log beginning with prefix (its indent skipped), or "".
static const char *line(cb_store *s, const char *prefix) {
    const char *l = ir_line(s, prefix);
    if (!l) {
        fprintf(stderr, "     [%s] no line \"%s\" in:\n%s", t_current, prefix, ir_log(s));
        return "";
    }
    while (*l == ' ') l++;
    return l;
}

// Where in the log text first appears (-1: nowhere), for an order.
static long at_of(cb_store *s, const char *text) {
    const char *p = strstr(ir_log(s), text);
    return p ? (long)(p - ir_log(s)) : -1;
}

static const char *bot0(void) { return CB_BOT_SEED[0].id; }

// a and b with accounts (and their signup rating rows), the game GID created
// by a at version 0, the log cleared.
static cb_store *table_of_two(bool needs_bots) {
    cb_store *s = open_rec();
    cb_user_row a = mkuser(UA, "a@x.io", "A"), b = mkuser(UB, "b@x.io", "B");
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    cb_game_row g = mkgame(GID, 10);
    g.needs_bots = needs_bots;
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, UA), CB_OK);
    ir_clear(s);
    return s;
}

// ---- the account's part, and a new game ------------------------------------------------------

static void signup_and_create(void) {
    cb_store *s = open_rec();
    cb_user_row a = mkuser(UA, "a@x.io", "A");
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    // The account's two items and foolish's rating row, in one transaction;
    // the rating starts at the start rating and is off the leaderboard index
    // (it has not played).
    T_EQ_INT(ir_count(s, "txn "), 1);
    T_EQ_STR(line(s, "txn "), "txn 3");
    T_EQ_STR(line(s, "put EMAIL#"), "put EMAIL#a@x.io USER if absent attrs user_id=\"" UA "\"");
    T_EQ_STR(line(s, "put USER#" UA " PROFILE"),
             "put USER#" UA " PROFILE if absent attrs gsi2pk=\"USERS\",gsi2sk=\"000000000000001#" UA "\",id=\"" UA
             "\",email=\"a@x.io\",password_hash=\"" HASH "\",username=\"A\",created_at=1");
    T_EQ_STR(line(s, "put USER#" UA " RATING"),
             "put USER#" UA " RATING attrs user_id=\"" UA "\",username=\"A\",elo_rating=1000,previous_elo=1000,"
             "games_played=0,updated_at=1");
    T_CHECK(at_of(s, "put EMAIL#") < at_of(s, "RATING"));   // the account's items first, as the hook promises
    ir_clear(s);

    cb_game_row g = mkgame(GID, 10);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, UA), CB_OK);
    T_EQ_STR(line(s, "txn "), "txn 2");
    T_EQ_STR(line(s, "put GAME#"),
             "put GAME#" GID " ROW if absent attrs id=\"" GID "\",status=0,state=b3,roster=b5,needs_bots=false,version=0,"
             "round_epoch=0,created_at=10,updated_at=10,last_commit_at=10");
    T_EQ_STR(line(s, "update GAME#"),
             "update GAME#" GID " MEMBER#" UA " sets game_id=\"" GID "\",user_id=\"" UA "\",updated_at=10,gsi1pk=\"USERGAMES#" UA
             "\",gsi1sk=\"000000000000010#" GID "\",joined_at?=10");
    T_CHECK(!ir_has(s, "SCAN"));   // no bot work: no marker
    ir_clear(s);

    // A game with bot work gets its SCAN marker, whole, in the same transaction.
    cb_game_row g2 = mkgame("rows02", 11);
    g2.needs_bots = true;
    g2.last_commit_at = 3;   // a new row's last commit is its creation
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g2, UA), CB_OK);
    T_EQ_STR(line(s, "txn "), "txn 3");
    T_EQ_STR(line(s, "put SCAN"), "put SCAN GAME#rows02 attrs game_id=\"rows02\",updated_at=11,last_commit_at=11,row_version=0");
    T_CHECK(ir_has(s, "last_commit_at=11"));
    ir_clear(s);

    // The id taken: the row's condition refused (item 0) is CB_E_CONFLICT; a
    // refusal of any other item is the store's inconsistency.
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, UA), CB_E_CONFLICT);
    ir_refuse_txns(s, 1, 1, false);
    cb_game_row g3 = mkgame("rows03", 12);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g3, UA), CB_E_DB);
    cb_store_close(s);
}

// ---- the CAS -------------------------------------------------------------------------------

static void cas_commit(void) {
    cb_store *s = table_of_two(false);
    cb_game_row row = mkgame(GID, 20);
    row.version = 1;
    cb_game_commit c = { .row = &row, .expected_version = 0, .n_humans = 2 };
    snprintf(c.humans[0], sizeof c.humans[0], UA);
    snprintf(c.humans[1], sizeof c.humans[1], UB);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    // Not given the seats: one Query of them, then the new human's account
    // (and rating) read, then the transaction whose item 0 is the fence.
    T_EQ_STR(line(s, "query"), "query t GAME#" GID " <ROW fwd keys");
    T_EQ_STR(line(s, "batch_get"), "batch_get 2 proj elo_rating");
    T_CHECK(ir_has(s, "  USER#" UB " RATING\n  USER#" UB " PROFILE\n"));
    T_CHECK(at_of(s, "query") < at_of(s, "batch_get") && at_of(s, "batch_get") < at_of(s, "txn"));
    T_EQ_STR(line(s, "txn "), "txn 3");
    const char *cas = "update GAME#" GID " ROW if version=0 sets status=0,state=b3,roster=b5,logs_packed-,game_seed-,"
                      "needs_bots=false,version=1,round_epoch=0,updated_at=20,last_commit_at=20,seats_exact=true (old)";
    T_EQ_STR(line(s, "update GAME#" GID " ROW"), cas);
    T_CHECK(at_of(s, "txn 3\n  update GAME#" GID " ROW") >= 0);   // item 0
    // Only the seat that is new is written; a's is already there.
    T_EQ_INT(ir_count(s, "update GAME#" GID " MEMBER#"), 1);
    T_CHECK(ir_has(s, "update GAME#" GID " MEMBER#" UB " sets"));
    T_EQ_STR(line(s, "delete SCAN"), "delete SCAN GAME#" GID);   // no bot work: the marker goes
    ir_clear(s);

    // The stored version moved: the fence refuses, nothing retried.
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_E_CONFLICT);
    T_EQ_INT(ir_count(s, "txn "), 1);
    ir_clear(s);

    // The fence refused with no item: the game is gone.
    row.version = 2;
    c.expected_version = 1;
    ir_refuse_txns(s, 1, 0, false);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_E_NOT_FOUND);
    T_EQ_INT(ir_count(s, "txn "), 1);
    ir_clear(s);

    // Another item refused (a rating, an account moved underneath): read
    // again and write again, the seats read again too; then it lands.
    ir_refuse_txns(s, 1, 1, true);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(ir_count(s, "txn "), 2);
    T_EQ_INT(ir_count(s, "query t GAME#" GID), 2);
    ir_clear(s);

    // Refused that way every time: five attempts, then CB_E_CONFLICT.
    row.version = 3;
    c.expected_version = 2;
    ir_refuse_txns(s, 99, 1, true);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_E_CONFLICT);
    T_EQ_INT(ir_count(s, "txn "), 5);
    ir_refuse_txns(s, 0, 0, false);
    ir_clear(s);

    // Bad commits ask nothing.
    c.n_humans = CB_MAX_SEATS + 1;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_E_INVALID);
    T_EQ_STR(ir_log(s), "");
    cb_store_close(s);
}

// THE LONE COMMIT: the seats given and unchanged, no rating, no bot work
// before or after: the row's conditional update alone, no transaction.
static void lone_commit(void) {
    cb_store *s = table_of_two(false);
    cb_game_row row = mkgame(GID, 30);
    row.version = 1;
    cb_game_commit c = { .row = &row, .expected_version = 0, .n_humans = 1, .prev_known = true, .n_prev_humans = 1 };
    snprintf(c.humans[0], sizeof c.humans[0], UA);
    snprintf(c.prev_humans[0], sizeof c.prev_humans[0], UA);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_STR(ir_log(s), "update GAME#" GID " ROW if version=0 sets status=0,state=b3,roster=b5,logs_packed-,game_seed-,"
                        "needs_bots=false,version=1,round_epoch=0,updated_at=30,last_commit_at=30,seats_exact=true (old)\n");
    ir_clear(s);
    // Refused with the item back: moved on; refused with none: gone.
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_E_CONFLICT);
    T_EQ_INT(ir_count(s, "update"), 1);
    ir_clear(s);

    // The commit gives the game bot work: a transaction again, the marker
    // made in place (a lease on it would be kept).
    row.version = 2;
    row.needs_bots = true;
    c.expected_version = 1;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_STR(line(s, "txn "), "txn 2");
    T_EQ_STR(line(s, "update SCAN"), "update SCAN GAME#" GID " sets game_id=\"" GID "\",updated_at=30,last_commit_at=30,row_version=2");
    T_CHECK(!ir_has(s, "query"));   // the seats were given
    ir_clear(s);

    // The row it is fenced on had bot work (prev_needs_bots): the marker
    // exists, so even a commit that leaves none is a transaction (it deletes it).
    row.version = 3;
    row.needs_bots = false;
    c.expected_version = 2;
    c.prev_needs_bots = true;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_STR(line(s, "txn "), "txn 2");
    T_EQ_STR(line(s, "delete SCAN"), "delete SCAN GAME#" GID);
    ir_clear(s);

    T_EQ_INT(CB_ROWS_CALL(s, game_delete, GID, 3), CB_OK);
    ir_clear(s);
    row.version = 4;
    c.expected_version = 3;
    c.prev_needs_bots = false;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_E_NOT_FOUND);
    T_EQ_INT(ir_count(s, "update GAME#"), 1);
    T_CHECK(!ir_has(s, "txn"));
    cb_store_close(s);
}

// ---- the lease ------------------------------------------------------------------------------

static void lease(void) {
    cb_store *s = table_of_two(true);
    cb_uuid tok, tok2;
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, GID, 1000, 25000, tok), CB_OK);
    char want[512];
    snprintf(want, sizeof want,
             "update SCAN GAME#" GID " if exists and lease_until<1000|absent sets lease_token=\"%s\",lease_until=26000,updated_at=1000\n",
             tok);
    T_EQ_STR(ir_log(s), want);
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, GID, 2000, 25000, tok2), CB_E_CONFLICT);   // held
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, GID, 2000), 1);
    T_EQ_STR(line(s, "get"), "get SCAN GAME#" GID);
    ir_clear(s);

    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, GID, tok, 3000, 25000), CB_OK);
    snprintf(want, sizeof want, "update SCAN GAME#" GID " if lease_token=\"%s\" sets lease_until=28000,updated_at=3000\n", tok);
    T_EQ_STR(ir_log(s), want);
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, GID, "not-the-token", 3000, 25000), CB_E_CONFLICT);
    // Released by someone else's token: left alone, and no failure.
    T_EQ_INT(CB_ROWS_CALL(s, lease_release, GID, "not-the-token"), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, GID, 3000), 1);
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, lease_release, GID, tok), CB_OK);
    snprintf(want, sizeof want, "update SCAN GAME#" GID " if lease_token=\"%s\" sets lease_until-\n", tok);
    T_EQ_STR(ir_log(s), want);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, GID, 3000), 0);
    ir_clear(s);

    // bots_idle: the marker deleted, fenced on the row version it carries.
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, GID, 5), CB_E_CONFLICT);
    T_EQ_STR(ir_log(s), "delete SCAN GAME#" GID " if row_version=5|absent\n");
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, GID, 0), CB_OK);
    ir_clear(s);
    // No marker now: nothing to lease, nothing held.
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, GID, 4000, 25000, tok2), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, GID, 4000), 0);
    cb_store_close(s);
}

// ---- seats ---------------------------------------------------------------------------------

static void seats(void) {
    cb_store *s = table_of_two(false);
    cb_game_row row = mkgame(GID, 40);
    row.version = 1;
    // a stays, b and the bot sit down, and z (no account) is skipped.
    cb_game_commit c = { .row = &row, .expected_version = 0, .n_humans = 3, .n_bots = 1 };
    snprintf(c.humans[0], sizeof c.humans[0], UA);
    snprintf(c.humans[1], sizeof c.humans[1], UB);
    snprintf(c.humans[2], sizeof c.humans[2], UZ);
    snprintf(c.bots[0], sizeof c.bots[0], "%s", bot0());
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_STR(line(s, "batch_get"), "batch_get 4 proj elo_rating");
    T_CHECK(ir_has(s, "  USER#" UZ " PROFILE\n"));
    T_CHECK(!ir_has(s, "MEMBER#" UZ));   // the account is gone: no seat
    char want[256];
    snprintf(want, sizeof want, "put GAME#" GID " BOT#%s attrs game_id=\"" GID "\",bot_id=\"%s\"", bot0(), bot0());
    T_EQ_STR(line(s, "put GAME#" GID " BOT#"), want);
    T_EQ_INT(ir_count(s, "update GAME#" GID " MEMBER#"), 1);   // b's alone
    T_EQ_STR(line(s, "txn "), "txn 4");
    ir_clear(s);

    // b and the bot stand up: their seat items deleted, nothing written for a.
    row.version = 2;
    c.expected_version = 1;
    c.n_humans = 1;
    c.n_bots = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_STR(line(s, "delete GAME#" GID " MEMBER#"), "delete GAME#" GID " MEMBER#" UB);
    snprintf(want, sizeof want, "delete GAME#" GID " BOT#%s", bot0());
    T_EQ_STR(line(s, "delete GAME#" GID " BOT#"), want);
    T_CHECK(!ir_has(s, "batch_get"));   // nobody new: no account read
    T_CHECK(!ir_has(s, "MEMBER#" UA));
    ir_clear(s);

    // The reads of the seats.
    T_EQ_INT(CB_ROWS_CALL(s, is_member, GID, UA), 1);
    T_EQ_STR(ir_log(s), "get GAME#" GID " MEMBER#" UA "\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, GID, UB), 0);
    cb_store_close(s);
}

static bool count_name(const char *n, void *ud) { (void)n; ++*(int *)ud; return true; }
static bool count_member(const cb_member_row *m, void *ud) { (void)m; ++*(int *)ud; return true; }
static bool count_rating(const cb_rating_row *r, void *ud) { (void)r; ++*(int *)ud; return true; }
static bool count_bot(const cb_bot_row *b, void *ud) { (void)b; ++*(int *)ud; return true; }
static bool count_game(const cb_game_row *g, void *ud) { T_CHECK(g->logs == NULL); ++*(int *)ud; return true; }

static void seat_reads(void) {
    cb_store *s = table_of_two(false);
    int n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_members, GID, count_name, &n), 1);
    T_EQ_STR(ir_log(s), "query t GAME#" GID " <ROW fwd keys\n");
    ir_clear(s);
    n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, UA, 5, count_member, &n), 1);
    T_EQ_STR(ir_log(s), "query g1 USERGAMES#" UA " back page=5\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, UA, 0, count_member, &n), 1);
    T_EQ_STR(ir_log(s), "query g1 USERGAMES#" UA " back\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, 100, 0, 10, count_name, &n), 0);
    T_EQ_STR(ir_log(s), "query t SCAN fwd\n");
    ir_clear(s);
    // The views: one batch read of the rows, every column but the session log.
    const char *ids[2] = { GID, "nogame" };
    n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_get_views, ids, 2, count_game, &n), 1);
    T_EQ_INT(n, 1);
    T_EQ_STR(ir_log(s), "batch_get 2 proj id,status,version,state,roster,game_seed,needs_bots,round_epoch,created_at,"
                        "updated_at,last_commit_at\n  GAME#" GID " ROW\n  GAME#nogame ROW\n");
    cb_store_close(s);
}

// ---- ratings ----------------------------------------------------------------------------------

static void ratings(void) {
    cb_store *s = table_of_two(false);
    cb_user_row cu = mkuser(UC, "c@x.io", "C");
    T_EQ_INT(CB_STORE_CALL(s, user_import, &cu), CB_OK);   // an account with no rating row
    ir_clear(s);
    cb_game_row row = mkgame(GID, 50);
    row.version = 1;
    // a +12, the bot -12, c +5 (no row yet: one from the start rating), and
    // a again (a repeated id is rated once).
    cb_game_commit c = { .row = &row, .expected_version = 0, .n_humans = 2, .n_rated = 4 };
    snprintf(c.humans[0], sizeof c.humans[0], UA);
    snprintf(c.humans[1], sizeof c.humans[1], UC);
    const char *rated[4] = { UA, bot0(), UC, UA };
    const int32_t delta[4] = { 12, -12, 5, 7 };
    for (int i = 0; i < 4; i++) {
        snprintf(c.rated_ids[i], sizeof c.rated_ids[i], "%s", rated[i]);
        c.rating_delta[i] = delta[i];
        c.rated_is_bot[i] = i == 1;
    }
    snprintf(c.rated_names[2], sizeof c.rated_names[2], "C");
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    // The ratings read in one batch, the bot's under its own key.
    char want[512];
    T_EQ_INT(ir_count(s, "batch_get"), 2);   // c's account (a new seat), then the rated rows
    T_CHECK(ir_has(s, "batch_get 7 proj elo_rating\n  USER#" UA " RATING\n  USER#" UA " PROFILE\n"));
    snprintf(want, sizeof want, "  BOT#%s BOT\n  USER#" UC " RATING\n", bot0());
    T_CHECK(ir_has(s, want));
    T_EQ_STR(line(s, "txn "), "txn 7");
    T_EQ_INT(ir_count(s, "update USER#" UA " RATING"), 1);
    T_EQ_STR(line(s, "update USER#" UA " RATING"),
             "update USER#" UA " RATING if elo_rating=1000 sets elo_rating=1012,gsi2pk=\"RATINGS\",gsi2sk=\"E#2147482635#" UA
             "\",previous_elo=1000,updated_at=50,games_played+=1");
    snprintf(want, sizeof want, "update BOT#%s BOT if elo_rating=1000 sets elo_rating=988,previous_elo=1000,updated_at=50,games_played+=1",
             bot0());
    T_EQ_STR(line(s, "update BOT#"), want);
    T_EQ_STR(line(s, "put USER#" UC " RATING"),
             "put USER#" UC " RATING if absent attrs gsi2pk=\"RATINGS\",gsi2sk=\"E#2147482642#" UC "\",user_id=\"" UC
             "\",username=\"C\",elo_rating=1005,previous_elo=1000,games_played=1,updated_at=50");
    T_EQ_STR(line(s, "check USER#" UC), "check USER#" UC " PROFILE if exists");
    ir_clear(s);

    // Never below zero, and the index key follows.
    row.version = 2;
    c.expected_version = 1;
    c.n_rated = 1;
    snprintf(c.rated_ids[0], sizeof c.rated_ids[0], UB);
    c.rated_is_bot[0] = false;
    c.rating_delta[0] = -5000;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_STR(line(s, "update USER#" UB " RATING"),
             "update USER#" UB " RATING if elo_rating=1000 sets elo_rating=0,gsi2pk=\"RATINGS\",gsi2sk=\"E#2147483647#" UB
             "\",previous_elo=1000,updated_at=50,games_played+=1");
    ir_clear(s);

    // seat_ratings: a bot's key and a user's for every id, in one batch.
    const char *ids[2] = { UA, bot0() };
    int32_t got[2];
    T_EQ_INT(CB_ROWS_CALL(s, ratings_for, ids, 2, got), CB_OK);
    T_EQ_INT(got[0], 1012);
    T_EQ_INT(got[1], 988);
    snprintf(want, sizeof want, "batch_get 4 proj elo_rating\n  BOT#" UA " BOT\n  USER#" UA " RATING\n  BOT#%s BOT\n  USER#%s RATING\n",
             bot0(), bot0());
    T_EQ_STR(ir_log(s), want);
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_for, ids, 0, got), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_for, ids, 51, got), CB_E_INVALID);
    T_EQ_STR(ir_log(s), "");

    // The leaderboard name.
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, UA, "NEW"), CB_OK);
    T_EQ_STR(ir_log(s), "update USER#" UA " RATING if exists sets username=\"NEW\"\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, UA, NULL), CB_OK);
    T_EQ_STR(ir_log(s), "update USER#" UA " RATING if exists sets username-\n");
    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, UZ, "X"), CB_OK);   // no row: nothing to rename
    ir_clear(s);

    // The lists: the leaderboard from its index, bounded by the limit.
    int n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 10, count_rating, &n), 3);
    T_EQ_STR(ir_log(s), "query g2 RATINGS ^E# fwd page=10\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 0, count_rating, &n), 0);
    T_EQ_STR(ir_log(s), "");
    T_EQ_INT(CB_ROWS_CALL(s, ratings_list, count_rating, &n), 3);
    T_EQ_STR(ir_log(s), "query g2 RATINGS fwd\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, count_bot, &n), CB_BOT_SEED_N);
    T_EQ_STR(ir_log(s), "query g2 BOTS fwd\n");
    cb_store_close(s);
}

// ---- the replay and its pointers --------------------------------------------------------------

static bool count_snap(const cb_snapshot_row *r, void *ud) { (void)r; ++*(int *)ud; return true; }

static void snapshot_pointers(void) {
    cb_store *s = table_of_two(false);
    uint8_t mv[3] = { 1, 2, 3 }, ex[2] = { 4, 5 }, ex2[4] = { 6, 7, 8, 9 };
    cb_snapshot_row sn = { .id = SN1, .game_id = GID, .n_players = 3, .moves = mv, .moves_len = 3, .extras = ex,
                           .extras_len = 2, .created_at = 60 };
    snprintf(sn.player_ids[0], sizeof sn.player_ids[0], UA);
    snprintf(sn.player_ids[1], sizeof sn.player_ids[1], UB);
    snprintf(sn.player_ids[2], sizeof sn.player_ids[2], UA);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);
    // The replay, the only item with the moves, the extras and the players;
    // and a pointer for each player once, holding no name.
    T_EQ_STR(line(s, "txn "), "txn 3");
    T_EQ_STR(line(s, "put SNAP#" SN1 " SNAP"),
             "put SNAP#" SN1 " SNAP if absent attrs gsi1pk=\"GAMESNAPS#" GID "\",gsi1sk=\"000000000000060#" SN1 "\",id=\"" SN1
             "\",game_id=\"" GID "\",player_ids=[" UA ";" UB ";" UA "],moves=b3,extras=b2,created_at=60");
    T_CHECK(at_of(s, "txn 3\n  put SNAP#" SN1 " SNAP") >= 0);   // item 0: its refusal is the id taken
    T_EQ_INT(ir_count(s, "put SNAP#" SN1 " PLAYER#"), 2);
    T_EQ_STR(line(s, "put SNAP#" SN1 " PLAYER#" UA),
             "put SNAP#" SN1 " PLAYER#" UA " attrs gsi2pk=\"PLAYER#" UA "\",gsi2sk=\"000000000000060#" SN1 "\",id=\"" SN1
             "\",game_id=\"" GID "\",created_at=60");
    T_CHECK(!strstr(line(s, "put SNAP#" SN1 " PLAYER#" UB), "moves"));
    T_CHECK(!strstr(line(s, "put SNAP#" SN1 " PLAYER#" UB), "extras"));
    T_CHECK(!strstr(line(s, "put SNAP#" SN1 " PLAYER#" UB), "player_ids"));
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_E_CONFLICT);   // the id reused
    ir_refuse_txns(s, 1, 2, false);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_E_DB);         // a pointer refused: not the caller's fault
    sn.n_players = CB_MAX_SEATS + 1;
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_E_INVALID);
    T_EQ_STR(ir_log(s), "");

    // A replay with no game is not on the game's index.
    cb_snapshot_row lone = { .id = "cccccccc-0000-4000-8000-000000000002", .n_players = 1, .moves = mv, .moves_len = 3,
                             .created_at = 61 };
    snprintf(lone.player_ids[0], sizeof lone.player_ids[0], UB);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &lone), CB_OK);
    T_CHECK(!ir_has(s, "GAMESNAPS#"));
    T_CHECK(ir_has(s, "player_ids=[" UB "],moves=b3,created_at=61"));   // no extras: none written
    ir_clear(s);

    // "My replays": the pointers' Query, then one batch read of the replays,
    // every attribute.
    int n = 0;
    cb_snapshot_query q = { .viewer_id = UA, .newest_first = true };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, count_snap, &n), 1);
    T_EQ_STR(ir_log(s), "query g2 PLAYER#" UA " back\nbatch_get 1\n  SNAP#" SN1 " SNAP\n");
    ir_clear(s);
    q.viewer_id = UB;
    q.newest_first = false;
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, count_snap, &n), 2);
    T_EQ_STR(line(s, "query"), "query g2 PLAYER#" UB " fwd");
    T_EQ_STR(line(s, "batch_get"), "batch_get 2");
    ir_clear(s);
    // Narrowed by the pointer's own game before any replay is read.
    q.game_id = "zz99zz";
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, count_snap, &n), 0);
    T_EQ_STR(ir_log(s), "query g2 PLAYER#" UB " fwd\n");
    ir_clear(s);

    // Redaction: one update of the replay alone (no copy holds a name).
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, SN1, ex2, sizeof ex2), CB_OK);
    T_EQ_STR(ir_log(s), "query t SNAP#" SN1 " ^PLAYER# fwd\nupdate SNAP#" SN1 " SNAP if exists sets extras=b4\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, SN1, NULL, 0), CB_OK);
    T_EQ_STR(line(s, "update"), "update SNAP#" SN1 " SNAP if exists sets extras-");
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, "cccccccc-0000-4000-8000-0000000000ff", ex2, 4), CB_E_NOT_FOUND);
    cb_store_close(s);
}

// ---- chat -----------------------------------------------------------------------------------

static bool count_chat(const cb_chat_row *r, void *ud) { (void)r; ++*(int *)ud; return true; }

static void chat(void) {
    cb_store *s = table_of_two(false);
    cb_chat_row c = { .id = CH1, .game_id = GID, .user_id = UA, .message = "hi", .created_at = 5 };
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_OK);
    // The message, fenced on the game and the account existing.
    T_EQ_STR(ir_log(s), "txn 3\n"
                        "  put CHAT#" GID " 000000000000005#" CH1 " if absent attrs gsi1pk=\"USERCHAT#" UA "\",gsi1sk=\"000000000000005#" CH1
                        "\",id=\"" CH1 "\",game_id=\"" GID "\",user_id=\"" UA "\",message=\"hi\",is_system=false,created_at=5\n"
                        "  check GAME#" GID " ROW if exists\n"
                        "  check USER#" UA " PROFILE if exists\n");
    // Which item refused says why.
    static const struct { int failed, rc; } WHY[] = { { 0, CB_E_CONFLICT }, { 1, CB_E_NOT_FOUND }, { 2, CB_E_NOT_FOUND }, { 3, CB_E_DB } };
    for (size_t i = 0; i < sizeof WHY / sizeof WHY[0]; i++) {
        ir_refuse_txns(s, 1, WHY[i].failed, false);
        T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), WHY[i].rc);
    }
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_E_CONFLICT);   // the same message again
    c.game_id[0] = 'z';
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_E_NOT_FOUND);  // no such game
    ir_clear(s);

    // Newest first from the store, at most the limit.
    int n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, GID, 2, count_chat, &n), 1);
    T_EQ_STR(ir_log(s), "query t CHAT#" GID " back page=2\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, GID, 0, count_chat, &n), 1);
    T_EQ_STR(ir_log(s), "query t CHAT#" GID " back\n");
    cb_store_close(s);
}

// ---- the imports ----------------------------------------------------------------------------

static void imports(void) {
    cb_store *s = open_rec();
    cb_user_row a = mkuser(UA, "a@x.io", "A");
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    // The account as given: its two items, no condition, no rating row.
    T_EQ_STR(line(s, "txn "), "txn 2");
    T_EQ_STR(line(s, "put EMAIL#"), "put EMAIL#a@x.io USER attrs user_id=\"" UA "\"");
    T_CHECK(strstr(line(s, "put USER#"), " PROFILE attrs gsi2pk=") != NULL);
    T_CHECK(!ir_has(s, "RATING"));
    ir_clear(s);

    // A game at its own version, with its seats, no fence, its marker whole.
    cb_game_row g = mkgame("rows09", 60);
    g.version = 7;
    g.last_commit_at = 55;
    g.needs_bots = true;
    cb_uuid humans[1], bots[1];
    snprintf(humans[0], sizeof humans[0], UA);
    snprintf(bots[0], sizeof bots[0], "%s", bot0());
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, 1, bots, 1), CB_OK);
    T_EQ_STR(line(s, "query"), "query t GAME#rows09 <ROW fwd keys");
    T_EQ_STR(line(s, "txn "), "txn 4");
    T_EQ_STR(line(s, "put GAME#rows09 ROW"),
             "put GAME#rows09 ROW attrs id=\"rows09\",status=0,state=b3,roster=b5,needs_bots=true,version=7,round_epoch=0,"
             "created_at=60,updated_at=60,last_commit_at=55");
    T_EQ_STR(line(s, "put SCAN"), "put SCAN GAME#rows09 attrs game_id=\"rows09\",updated_at=60,last_commit_at=55,row_version=7");
    T_CHECK(ir_has(s, "update GAME#rows09 MEMBER#" UA " sets"));
    T_CHECK(ir_has(s, "put GAME#rows09 BOT#"));
    ir_clear(s);
    // Again: the row replaced, the seats already there.
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, 1, bots, 1), CB_OK);
    T_EQ_STR(line(s, "txn "), "txn 2");
    T_CHECK(!ir_has(s, "MEMBER#") && !ir_has(s, " BOT#"));
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, CB_MAX_SEATS + 1, bots, 1), CB_E_INVALID);
    T_EQ_STR(ir_log(s), "");

    // A rating row as given: on the leaderboard index when it has played.
    cb_rating_row rt = { .user_id = UA, .username = "A", .elo_rating = 1033, .previous_elo = 1020, .games_played = 7,
                         .updated_at = 3000 };
    T_EQ_INT(CB_ROWS_CALL(s, rating_import, &rt), CB_OK);
    T_EQ_STR(ir_log(s), "put USER#" UA " RATING attrs gsi2pk=\"RATINGS\",gsi2sk=\"E#2147482614#" UA "\",user_id=\"" UA
                        "\",username=\"A\",elo_rating=1033,previous_elo=1020,games_played=7,updated_at=3000\n");
    ir_clear(s);
    // Its key already the current one: the reindex is refused, nothing rewritten.
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &rt), CB_E_CONFLICT);
    T_EQ_STR(ir_log(s), "update USER#" UA " RATING if elo_rating=1033 and games_played=7 and gsi2sk!=\"E#2147482614#" UA
                        "\" sets gsi2pk=\"RATINGS\",gsi2sk=\"E#2147482614#" UA "\"\n");
    ir_clear(s);
    cb_rating_row none = { .user_id = UB, .elo_rating = 1000, .previous_elo = -1, .updated_at = 1 };
    T_EQ_INT(CB_ROWS_CALL(s, rating_import, &none), CB_OK);
    T_EQ_STR(ir_log(s), "put USER#" UB " RATING attrs user_id=\"" UB "\",elo_rating=1000,games_played=0,updated_at=1\n");
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &none), CB_E_CONFLICT);   // not on the index: nothing to take out
    T_EQ_STR(ir_log(s), "update USER#" UB " RATING if elo_rating=1000 and games_played=0 and has gsi2pk sets gsi2pk-,gsi2sk-\n");
    cb_store_close(s);
}

static void bots_replace(void) {
    cb_store *s = open_rec();   // the 49 seeded bots
    cb_bot_row rows[2] = { { .elo_rating = 1100, .previous_elo = 1000, .games_played = 3, .created_at = 1, .updated_at = 2 },
                           { .id = "eeeeeeee-0000-4000-8000-000000000001", .nickname = "%New 1", .strategy_key = "random",
                             .elo_rating = 1000, .previous_elo = -1 } };
    snprintf(rows[0].id, sizeof rows[0].id, "%s", CB_BOT_SEED[0].id);
    snprintf(rows[0].nickname, sizeof rows[0].nickname, "%s", CB_BOT_SEED[0].nickname);
    snprintf(rows[0].strategy_key, sizeof rows[0].strategy_key, "%s", CB_BOT_SEED[0].strategy_key);
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, rows, 2), CB_OK);
    // The stored bots read once; every one not kept deleted, every given one
    // put whole, in ONE transaction.
    T_CHECK(strncmp(ir_log(s), "query g2 BOTS fwd\ntxn 50\n", 25) == 0);
    T_EQ_INT(ir_count(s, "txn "), 1);
    T_EQ_INT(ir_count(s, "  delete BOT#"), CB_BOT_SEED_N - 1);
    T_EQ_INT(ir_count(s, "  put BOT#"), 2);
    char want[512];
    snprintf(want, sizeof want,
             "put BOT#%s BOT attrs gsi2pk=\"BOTS\",gsi2sk=\"%s\",id=\"%s\",nickname=\"%%Miami 1\",strategy_key=\"random\","
             "elo_rating=1100,previous_elo=1000,games_played=3,created_at=1,updated_at=2", rows[0].id, rows[0].id, rows[0].id);
    T_EQ_STR(line(s, "put BOT#" CB_BOT_SEED_FIRST), want);
    T_EQ_STR(line(s, "put BOT#eeee"), "put BOT#eeeeeeee-0000-4000-8000-000000000001 BOT attrs gsi2pk=\"BOTS\","
                                       "gsi2sk=\"eeeeeeee-0000-4000-8000-000000000001\",id=\"eeeeeeee-0000-4000-8000-000000000001\","
                                       "nickname=\"%New 1\",strategy_key=\"random\",elo_rating=1000,games_played=0,created_at=0,updated_at=0");
    snprintf(want, sizeof want, "delete BOT#%s BOT", rows[0].id);
    T_CHECK(!ir_has(s, want));
    int n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, count_bot, &n), 2);
    ir_clear(s);

    // More than one transaction holds: refused before anything is sent.
    cb_bot_row *many = calloc(CB_ITEM_TXN_MAX + 1, sizeof *many);
    for (int i = 0; i <= CB_ITEM_TXN_MAX; i++) {
        snprintf(many[i].id, sizeof many[i].id, "eeeeeeee-0000-4000-8000-%012d", 100 + i);
        snprintf(many[i].nickname, sizeof many[i].nickname, "%%Many %d", i);
        snprintf(many[i].strategy_key, sizeof many[i].strategy_key, "random");
    }
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, many, CB_ITEM_TXN_MAX + 1), CB_E_FULL);
    T_CHECK(!ir_has(s, "txn"));
    free(many);
    ir_clear(s);

    // None: every bot deleted; and then none to delete: nothing sent.
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, NULL, 0), CB_OK);
    T_EQ_STR(line(s, "txn "), "txn 2");
    T_EQ_INT(ir_count(s, "  delete BOT#"), 2);
    ir_clear(s);
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, NULL, 0), CB_OK);
    T_EQ_STR(ir_log(s), "query g2 BOTS fwd\n");
    cb_store_close(s);
}

// ---- the account's cascade --------------------------------------------------------------------

static void user_cascade(void) {
    cb_store *s = table_of_two(false);
    cb_chat_row c = { .id = CH1, .game_id = GID, .user_id = UA, .message = "bye", .created_at = 5 };
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_OK);
    cb_refresh_row r = { .token_hash = "t1", .user_id = UA, .session_id = "s" };
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_OK);
    ir_clear(s);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, UA), CB_OK);
    // The account read; foolish's items off it (seats, chat) and the
    // account's tokens, found through GSI1, in one batch delete; then the
    // account with foolish's rating row in one transaction.
    T_EQ_STR(ir_log(s), "get USER#" UA " PROFILE\n"
                        "query g1 USERGAMES#" UA " back\n"
                        "query g1 USERCHAT#" UA " fwd keys\n"
                        "query g1 USERREFRESH#" UA " fwd keys\n"
                        "batch_delete 3\n"
                        "  GAME#" GID " MEMBER#" UA "\n"
                        "  CHAT#" GID " 000000000000005#" CH1 "\n"
                        "  REFRESH#t1 TOKEN\n"
                        "txn 3\n"
                        "  delete USER#" UA " PROFILE\n"
                        "  delete EMAIL#a@x.io USER\n"
                        "  delete USER#" UA " RATING\n");
    ir_clear(s);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, UA), CB_E_NOT_FOUND);
    T_EQ_STR(ir_log(s), "get USER#" UA " PROFILE\n");
    ir_clear(s);
    // An account with nothing hanging off it: no batch delete.
    T_EQ_INT(CB_STORE_CALL(s, user_delete, UB), CB_OK);
    T_CHECK(!ir_has(s, "batch_delete"));
    T_EQ_STR(line(s, "txn "), "txn 3");
    cb_store_close(s);
}

// ---- SQLite seeds its bots at open -------------------------------------------------------------

static int bots_in(cb_store *s) {
    int n = 0;
    int rc = CB_ROWS_CALL(s, bots_list, count_bot, &n);
    T_EQ_INT(rc, n);
    return n;
}

static void sqlite_seeds_at_open(void) {
    if (tsb_dynamo()) return;   // DynamoDB is seeded by a deploy's --seed-bots, never at open
    char path[320];
    snprintf(path, sizeof path, "%s/seed.sqlite", g_dir);
    unlink(path);
    cb_store *raw = NULL;
    // None present (a new file): every seed bot, at the start rating.
    T_EQ_INT(cb_store_open_sqlite(path, &raw), CB_OK);
    T_EQ_INT(bots_in(raw), CB_BOT_SEED_N);
    // All present: seeding again writes nothing (the read alone).
    cb_store *s = ir_wrap(raw);
    T_EQ_INT(cb_import_seed_bots(s), 0);
    T_EQ_STR(ir_log(s), "query g2 BOTS fwd\n");
    // Some present (an import's hosted bots): kept as they are.
    cb_bot_row one = { .id = "eeeeeeee-0000-4000-8000-000000000001", .nickname = "%Hosted 1", .strategy_key = "random",
                       .elo_rating = 1234, .previous_elo = -1 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, &one, 1), CB_OK);
    cb_store_close(s);
    T_EQ_INT(cb_store_open_sqlite(path, &raw), CB_OK);
    T_EQ_INT(bots_in(raw), 1);
    cb_store_close(raw);
    T_EQ_INT(cb_store_open_sqlite(path, &raw), CB_OK);
    T_EQ_INT(bots_in(raw), 1);
    // None again: the next open seeds them all, in one transaction, in ladder order.
    T_EQ_INT(CB_ROWS_CALL(raw, bots_replace, NULL, 0), CB_OK);
    T_EQ_INT(bots_in(raw), 0);
    s = ir_wrap(raw);
    T_EQ_INT(cb_import_seed_bots(s), CB_BOT_SEED_N);
    T_EQ_INT(ir_count(s, "txn "), 1);
    T_EQ_INT(ir_count(s, "  put BOT#"), CB_BOT_SEED_N);
    T_CHECK(strstr(ir_log(s), "  put BOT#") == strstr(ir_log(s), "  put BOT#" CB_BOT_SEED_FIRST));
    T_CHECK(ir_has(s, "nickname=\"%Miami 1\",strategy_key=\"random\",elo_rating=1000,games_played=0"));
    cb_store_close(s);
    T_EQ_INT(cb_store_open_sqlite(path, &raw), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(raw, bots_replace, NULL, 0), CB_OK);
    cb_store_close(raw);
    T_EQ_INT(cb_store_open_sqlite(path, &raw), CB_OK);
    T_EQ_INT(bots_in(raw), CB_BOT_SEED_N);
    cb_store_close(raw);
    unlink(path);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/cbase-unit-rows-XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    T_RUN(signup_and_create);
    T_RUN(cas_commit);
    T_RUN(lone_commit);
    T_RUN(lease);
    T_RUN(seats);
    T_RUN(seat_reads);
    T_RUN(ratings);
    T_RUN(snapshot_pointers);
    T_RUN(chat);
    T_RUN(imports);
    T_RUN(bots_replace);
    T_RUN(user_cascade);
    T_RUN(sqlite_seeds_at_open);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}

// unit_store.c - the store (store/store.h) against a real backend: every
// table round-trips, the CAS, the bot lease, the heartbeat scan's windows, the
// ratings a commit applies, the admin console's reads, the import writes.
//
// SQLite (a real file) by default; DynamoDB Local with CBASE_STORE=dynamo
// (`make check-dynamo`, each test on a fresh table made from providers/aws/ops/cbase.yaml).
// The same assertions hold both backends to one contract; the few that need
// the backend's own query language (SQL or PartiQL) say which they run.
#include "t.h"
#include "lib_store_backend.h"
#include "store/store_sql.h"
#include "store/store_import.h"

#include "store/rows_foolish.h"
#include "bot_roster.h"
#include "bots.h"
#include "table.h"
#include "move_list.h"
#include "../../c/tests/replay_log_oracle.h"
#include "awire.h"
#include "legal.h"
#include "view.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/uuid.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static char g_dir[256];
static int g_seq;

static cb_store *open_fresh(void) {
    char path[320];
    snprintf(path, sizeof path, "%s/s%d.sqlite", g_dir, ++g_seq);
    cb_store *s = tsb_open_fresh(path);
    if (!s) { fprintf(stderr, "FAIL [%s] no store to test against\n", t_current); exit(1); }
    return s;
}

static cb_user_row mkuser(const char *email, const char *name, int64_t at) {
    cb_user_row u = { 0 };
    cb_uuid_v4(u.id);
    snprintf(u.email, sizeof u.email, "%s", email);
    snprintf(u.password_hash, sizeof u.password_hash, "$2a$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
    snprintf(u.username, sizeof u.username, "%s", name);
    u.created_at = at;
    return u;
}

static uint8_t g_state[3] = { 1, 2, 3 }, g_roster[5] = { 9, 8, 7, 6, 5 }, g_logs[2] = { 0xaa, 0xbb };

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

// ---- collectors ------------------------------------------------------------------------

typedef struct { int n; char ids[16][CB_EMAIL_MAX + 1]; int64_t at[16]; } names_t;
static bool collect_name(const char *name, void *ud) {
    names_t *n = ud;
    if (n->n < 16) snprintf(n->ids[n->n++], sizeof n->ids[0], "%s", name);
    return true;
}
static bool collect_one_name(const char *name, void *ud) {
    collect_name(name, ud);
    return false;
}
static bool collect_member(const cb_member_row *r, void *ud) {
    names_t *n = ud;
    if (n->n < 16) { n->at[n->n] = r->updated_at; snprintf(n->ids[n->n++], sizeof n->ids[0], "%s", r->game_id); }
    return true;
}
static bool collect_user(const cb_user_row *r, void *ud) { return collect_name(r->email, ud); }
static bool collect_snap(const cb_snapshot_row *r, void *ud) { return collect_name(r->id, ud); }
static bool collect_chat(const cb_chat_row *r, void *ud) { return collect_name(r->message, ud); }

typedef struct { int n; cb_bot_row rows[64]; } bots_t;
static bool collect_bot(const cb_bot_row *r, void *ud) {
    bots_t *b = ud;
    if (b->n < 64) b->rows[b->n++] = *r;
    return true;
}
typedef struct { int n; cb_rating_row rows[16]; } ratings_t;
static bool collect_rating(const cb_rating_row *r, void *ud) {
    ratings_t *b = ud;
    if (b->n < 16) b->rows[b->n++] = *r;
    return true;
}

static int32_t rating_of(cb_store *s, const char *id) {
    int32_t out = -99;
    const char *ids[1] = { id };
    T_EQ_INT(CB_ROWS_CALL(s, ratings_for, ids, 1, &out), CB_OK);
    return out;
}

// ---- the bots ----------------------------------------------------------------------------

// PRODUCTION'S 49 BOT ACCOUNTS, as (id, nickname, strategy_key): copied by hand
// on 2026-10-28 from a read-only backup of the production SQLite store's
// `bots` table (scratchpad prod/latest.sqlite, `SELECT id, nickname,
// strategy_key FROM bots`, the 16-byte ids written as UUID text). Production
// was never touched. The hosted store minted these ids (random v4) and the
// migration carried them, so they match nothing derivable: they are data.
static const struct { const char *id, *nickname, *key; } PROD_BOTS[] = {
    { "176d94a4-71f2-4542-9510-5603e52058d7", "%Miami 1", "random" },
    { "15b54f52-2240-4955-8055-e27b7d767e3a", "%Miami 2", "random" },
    { "fb86b312-0a92-456a-9d86-dbcf139c5c0c", "%Miami 3", "random" },
    { "cb71817c-55c4-4588-9e50-48f5f14c1fe0", "%Miami 4", "random" },
    { "250cca6c-6e42-49c1-9ddb-f0a892d1bdc8", "%Miami 5", "random" },
    { "127fe4f9-c6ae-4574-a138-6d672398c0b3", "%Miami 6", "random" },
    { "958fe3ae-f1d3-4009-ba13-5497c241c373", "%Miami 7", "random" },
    { "663f2cbf-e8f7-407c-a991-192a1a929a17", "%New York 1", "handwritten" },
    { "a3791b0c-9fe3-4881-846e-d9a36b5ffec5", "%New York 2", "handwritten" },
    { "7615f43f-f937-43df-95bc-5ba7537e5efc", "%New York 3", "handwritten" },
    { "44d3db6c-7ea2-4573-ab6d-59d512a43e00", "%New York 4", "handwritten" },
    { "71afbbc7-8210-43ee-87f2-a553e2850659", "%New York 5", "handwritten" },
    { "aed56295-c1af-42a7-957d-e93bc1c0e8db", "%New York 6", "handwritten" },
    { "5b4bd18e-4c3b-4ffe-b3d7-0190fa756052", "%New York 7", "handwritten" },
    { "7ef1fc0b-3615-4dc8-b3ef-aa8f5d54d974", "%Seoul 1", "robusta" },
    { "8bc76702-5436-4765-9276-06b3cf48c28a", "%Seoul 2", "robusta" },
    { "2fb4b0e0-042f-4e8c-a686-0fe57683a53f", "%Seoul 3", "robusta" },
    { "31e33723-01db-4ed7-91a5-25c7aacadb34", "%Seoul 4", "robusta" },
    { "4b562969-4424-4806-ae2b-211635fd01a0", "%Seoul 5", "robusta" },
    { "d94da892-95c2-4c4a-aa40-b1835293c564", "%Seoul 6", "robusta" },
    { "c423710d-afe3-4ec8-bc7f-6dbd4fb891b8", "%Seoul 7", "robusta" },
    { "8dab2d5c-c44d-4b59-837b-8e412c2ae942", "%Madrid 1", "firecracker" },
    { "323fe1d3-7911-473d-a510-acae44946c0e", "%Madrid 2", "firecracker" },
    { "7e4ded9f-0f9e-4a19-8246-c42202365971", "%Madrid 3", "firecracker" },
    { "34d13cb1-a638-43c8-b7b9-142736ef3dfc", "%Madrid 4", "firecracker" },
    { "10e6bc02-3e6f-4ee5-8388-fcedfaf6c08f", "%Madrid 5", "firecracker" },
    { "4c61fbcc-ddd8-4188-9b46-74ade99681d3", "%Madrid 6", "firecracker" },
    { "7e178642-b839-4a4f-91db-bf6aae3513cb", "%Madrid 7", "firecracker" },
    { "492b48b0-f474-4661-8545-b269ca614325", "%Vienna 1", "blackpowder" },
    { "a541a928-8152-441c-8fb9-ff224bbf7dc7", "%Vienna 2", "blackpowder" },
    { "4201eb28-61ad-40d5-885b-4950111489a0", "%Vienna 3", "blackpowder" },
    { "29107dcb-fb7b-4088-bf44-b41f7fa9b84f", "%Vienna 4", "blackpowder" },
    { "ea14d04e-ebcc-4f73-81de-3ccd86530d4f", "%Vienna 5", "blackpowder" },
    { "c634657e-cb48-4ef6-800a-f76f2250eb43", "%Vienna 6", "blackpowder" },
    { "6e3e3e5d-5466-4d84-8767-6e4d9d5394aa", "%Vienna 7", "blackpowder" },
    { "56959dab-9d74-438b-b0e7-2f08d4524c81", "%St. Petersburg 1", "cordite" },
    { "30532f60-cd98-48bb-8044-e2f8393d5702", "%St. Petersburg 2", "cordite" },
    { "88cc6a4a-400f-40ed-917f-20ec9eb5e16f", "%St. Petersburg 3", "cordite" },
    { "4d1c6b02-a5f7-41e6-b57a-f5aada3db922", "%St. Petersburg 4", "cordite" },
    { "b06f300a-2f9c-4d99-bb50-77580466e4ab", "%St. Petersburg 5", "cordite" },
    { "518a9f06-e7bf-4688-a75e-ce603711ed83", "%St. Petersburg 6", "cordite" },
    { "bc195cef-fac6-45fd-a518-c7d25b42e35e", "%St. Petersburg 7", "cordite" },
    { "4f6f2ebb-1d3e-4761-a5a9-c91313eba087", "%Moscow 1", "octogen" },
    { "f60b53ec-74f6-45fe-9e0f-315e5495bc18", "%Moscow 2", "octogen" },
    { "45f7a264-aedc-4ee4-8473-4681d6e0bcb8", "%Moscow 3", "octogen" },
    { "acc775ae-6d35-41d2-88bc-7dc1599cff82", "%Moscow 4", "octogen" },
    { "df16e0de-4569-4771-804e-98f8d13affd1", "%Moscow 5", "octogen" },
    { "fc185071-a522-424e-a1bd-0c4d34b32d3e", "%Moscow 6", "octogen" },
    { "abe1fcfa-992c-4bf5-b163-32c0b0a21a7b", "%Moscow 7", "octogen" },
};

// FUSION'S SEVEN ACCOUNTS, the ones production does not have yet: minted
// (random v4) for its registry row on 2026-10-29, when it went live as Moscow.
static const char *const FUSION_IDS[] = {
    "2c58c4a2-a254-4a75-a465-db25fce8de79", "a721ac2c-afc4-41d7-87ff-285cef86f1fd",
    "592588e9-ba8f-44b9-be56-2803377705bf", "98385eea-2b92-44f7-a436-11c20337eec3",
    "03f1f377-00a7-4936-b7ea-ed4f2c93b206", "6db6e1cc-fa6c-4e9f-a2e9-9bba574e63d5",
    "efe6f751-912f-48be-a488-5aee56b85bdb",
};

// THE LADDER the registry names, written out here so a city that moves is a
// test that moves with it (2026-10-29: fusion took Moscow, octogen St.
// Petersburg, cordite Warsaw). The rating follows the BOT: an account keeps
// its id and strategy, and only its name says the rung's city.
static const char *ladder_city(const char *key) {
    static const struct { const char *key, *city; } L[] = {
        { "random", "Miami" },        { "handwritten", "New York" }, { "robusta", "Seoul" },
        { "firecracker", "Madrid" },  { "blackpowder", "Vienna" },   { "cordite", "Warsaw" },
        { "octogen", "St. Petersburg" }, { "fusion", "Moscow" },
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) if (strcmp(L[i].key, key) == 0) return L[i].city;
    return NULL;
}

// "%<city of key> <n>", with n the instance number of production's `nickname`
// (its last word): the name the seed gives that account.
static void ladder_name(char *out, size_t cap, const char *key, const char *nickname) {
    const char *sp = strrchr(nickname, ' ');
    snprintf(out, cap, "%%%s %s", ladder_city(key) ? ladder_city(key) : "?", sp ? sp + 1 : "?");
}

// THE SEED IS PRODUCTION'S BOTS, ON THE REGISTRY'S LADDER: the accounts
// bot_seed.c derives from the kernel's registry (foolish/c/src/bots.c: each
// seeded row's city and ids) are the 49 above, id for id and strategy for
// strategy, each named for its bot's city today, plus fusion's seven. This is
// what makes seeding by id safe on prod's store - it finds every one of the 49
// and only renames or adds - and what gives a fresh store, the tests and local
// dev prod's own bots.
//
// Mutations that turned it red, each restored after: one hex digit of
// octogen's seventh id in bots.c (`at >= 0`: that account not seeded); Vienna
// spelled Wien in bots.c (the nickname assertion); nicknames numbered from 0 in
// bot_seed.c (the nickname assertion); bot_seed.c not skipping unseeded rows
// (`n == N + 7`: a row with no city refuses the whole seed); octogen left on
// Moscow in bots.c (the nickname assertion); one of fusion's ids changed in
// bots.c (the fusion assertion); fusion's NAMED dispatch entry NULL in bots.c
// (the linked assertion).
static void bot_seed_is_production(void) {
    enum { N = (int)(sizeof PROD_BOTS / sizeof PROD_BOTS[0]), F = (int)(sizeof FUSION_IDS / sizeof FUSION_IDS[0]) };
    cb_bot_row rows[BOT_ROSTER_MAX * BOT_IDS_MAX];
    const int n = cb_bot_seed_rows(rows, (int)(sizeof rows / sizeof rows[0]));
    T_EQ_INT(N, 49);
    T_EQ_INT(F, BOT_IDS_MAX);
    T_EQ_INT(n, N + F);
    for (int i = 0; i < N; i++) {
        int at = -1;
        for (int j = 0; j < n; j++) if (strcmp(rows[j].id, PROD_BOTS[i].id) == 0) at = j;
        if (at < 0) { fprintf(stderr, "  prod bot %s (%s) is not seeded\n", PROD_BOTS[i].id, PROD_BOTS[i].nickname); T_CHECK(at >= 0); continue; }
        char want[64];
        ladder_name(want, sizeof want, PROD_BOTS[i].key, PROD_BOTS[i].nickname);
        T_EQ_STR(rows[at].nickname, want);
        T_EQ_STR(rows[at].strategy_key, PROD_BOTS[i].key);
        T_EQ_INT(rows[at].elo_rating, TABLE_RATING_START);
        T_EQ_INT(rows[at].previous_elo, -1);
        T_EQ_INT(rows[at].games_played, 0);
    }
    for (int i = 0; i < F; i++) {
        int at = -1;
        for (int j = 0; j < n; j++) if (strcmp(rows[j].id, FUSION_IDS[i]) == 0) at = j;
        T_CHECK(at >= 0);
        if (at < 0) continue;
        char want[32];
        snprintf(want, sizeof want, "%%Moscow %d", i + 1);
        T_EQ_STR(rows[at].nickname, want);
        T_EQ_STR(rows[at].strategy_key, "fusion");
    }
    // The server plays every seeded bot: this binary links the kernel the way
    // the server does (c_base/Makefile KERNEL_SRC with KERNEL_CFLAGS, never
    // FOOLISH_SEEDED_BOTS_ONLY), so every seeded account's brain dispatches
    // here. web.wasm leaves a server-only brain out (bots.c native_only), so
    // the browser's dump is not where this is held (e2e/bot_roster_parity).
    for (int j = 0; j < n; j++) {
        const int ri = bot_roster_find(rows[j].strategy_key);
        T_CHECK(ri >= 0 && bot_roster_linked(ri) && bot_choose(bot_roster_strat(ri)) != NULL);
    }
    // One cap short is a refusal, never a shorter seed.
    T_EQ_INT(cb_bot_seed_rows(rows, N + F - 1), -1);
}

// EVERY SEEDED NICKNAME IS THE KERNEL'S OWN NAME FOR ITS ACCOUNT: each row
// bot_seed.c derives reads back through bot_nickname_read (bot_roster.h, the
// one reader) as BOT_NICK_CITY, naming the row's own strategy and an account
// 1 to BOT_IDS_MAX, each account of a strategy exactly once; and writing that
// (strategy, account) again through bot_nickname_write gives the same bytes.
// So the seed never spells the "%<city> <n>" form itself, and no stored name
// is one the kernel would read as another bot's or as a stranger's.
//
// Mutations that turned it red, each restored after (objects deleted before
// every rebuild, so no stale mutant survived make's same-second check):
// bot_seed.c writing account i + 2 for i + 1 (the account range: instance 8
// read back, and the per-row account set); bot_seed.c writing the next row's
// strategy, bot_roster_strat(r + 1) (the seed fails on the last row, so n > 0,
// and every row's account set); the writer's separator ' ' as '_' in
// bot_roster.c (the kind assertion: BOT_NICK_STORED, and every check after it).
// bot_seed_is_production also goes red on all three.
static void bot_seed_names_read_back(void) {
    cb_bot_row rows[BOT_ROSTER_MAX * BOT_IDS_MAX];
    const int n = cb_bot_seed_rows(rows, (int)(sizeof rows / sizeof rows[0]));
    T_CHECK(n > 0);
    unsigned seen[BOT_ROSTER_MAX] = {0};     // per registry row: a bit per account read back
    for (int j = 0; j < n; j++) {
        BotNickname nk;
        const int len = (int)strlen(rows[j].nickname);
        T_EQ_INT(bot_nickname_read(rows[j].nickname, len, &nk), BOT_NICK_CITY);
        T_EQ_INT(nk.kind, BOT_NICK_CITY);
        char key[BOT_ROSTER_KEY_MAX + 1];
        memcpy(key, nk.key, nk.key_len);
        key[nk.key_len] = 0;
        T_EQ_STR(key, rows[j].strategy_key);
        T_CHECK(nk.instance >= 1 && nk.instance <= BOT_IDS_MAX);
        const int ri = bot_roster_find(rows[j].strategy_key);
        T_CHECK(ri >= 0);
        if (ri < 0 || nk.instance < 1 || nk.instance > BOT_IDS_MAX) continue;
        T_CHECK(!(seen[ri] & (1u << nk.instance)));
        seen[ri] |= 1u << nk.instance;
        char again[BOT_NICK_SHOW_MAX];
        const int alen = bot_nickname_write(bot_roster_strat(ri), nk.instance, again, (int)sizeof again);
        T_EQ_INT(alen, len);
        T_CHECK(alen == len && memcmp(again, rows[j].nickname, (size_t)len) == 0);
    }
    // Every seeded row's seven accounts, none missing.
    for (int r = 0; r < bot_roster_count(); r++) {
        const BotDef *d = bot_roster_at(r);
        if (!d || !d->seeded) continue;
        T_EQ_INT((int)seen[r], (int)(((1u << BOT_IDS_MAX) - 1) << 1));
    }
}

static bool same_bot(const cb_bot_row *a, const cb_bot_row *b) {
    return strcmp(a->id, b->id) == 0 && strcmp(a->nickname, b->nickname) == 0
        && strcmp(a->strategy_key, b->strategy_key) == 0 && a->elo_rating == b->elo_rating
        && a->previous_elo == b->previous_elo && a->games_played == b->games_played
        && a->created_at == b->created_at && a->updated_at == b->updated_at;
}

static const cb_bot_row *find_bot(const bots_t *b, const char *id) {
    for (int i = 0; i < b->n; i++) if (strcmp(b->rows[i].id, id) == 0) return &b->rows[i];
    return NULL;
}

// SEEDING ADDS THE MISSING BOTS AND NAMES THE REST BY THE REGISTRY
// (store_import.c cb_import_seed_bots): on a store that already has bots - two
// seeded ones gone, one played and renamed, one that is no seed's at all - a
// seed run (1) adds exactly the two missing accounts, at the start rating, and
// gives the renamed one its registry name back, so it returns 3; (2) leaves
// every other row as it was, and the renamed one's rating, games, strategy and
// timestamps too; (3) a second run writes nothing; and bots_rename itself is
// all or none. Both backends: SQLite here, DynamoDB Local in check-dynamo.
//
// Mutations that turned it red, each restored after: the old rule, write only
// into an empty store (assertion 1: `cb_import_seed_bots(s) == 3`); bots_replace
// of the missing rows in place of bots_add (assertion 2: every existing row,
// the played one and the stranger, gone); stored_bot always NULL, so every seed
// row reads as missing (a store with bots no longer opens: its seed hits a
// taken id); the renames sent through bots_add (assertion 1: CB_E_CONFLICT,
// nothing renamed or added); the rename comparison
// dropped, so nothing is renamed (assertion 1, and the played row's name); the
// SQL S_BOT_ADD as INSERT OR REPLACE (the all-or-none check: a taken id
// overwritten, `== CB_E_CONFLICT`); the SQL rename's changes() check dropped
// (the rename all-or-none check: CB_OK); S_BOT_RENAME also setting elo_rating
// (assertion 2: the played row's rating). The item store's CB_IF_ABSENT and
// CB_IF_EXISTS dropped are caught by c_base/providers/aws/test/unit_dynamo_ops.c
// (import_seed_and_reindex: the requests lack their conditions).
static void seed_adds_only_missing_bots(void) {
    cb_store *s = open_fresh();
    bots_t all = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &all), 56);

    // The store as a running server would have it: Miami 3 and Moscow 7 gone,
    // Vienna 2 played and renamed, and a bot no seed knows.
    cb_bot_row keep[64];
    int n = 0;
    char gone[2][CB_UUID_LEN + 1] = { "", "" };
    const cb_bot_row *played = NULL;
    for (int i = 0; i < all.n; i++) {
        const cb_bot_row *r = &all.rows[i];
        if (strcmp(r->nickname, "%Miami 3") == 0) { snprintf(gone[0], sizeof gone[0], "%s", r->id); continue; }
        if (strcmp(r->nickname, "%Moscow 7") == 0) { snprintf(gone[1], sizeof gone[1], "%s", r->id); continue; }
        keep[n] = *r;
        if (strcmp(r->nickname, "%Vienna 2") == 0) {
            snprintf(keep[n].nickname, sizeof keep[n].nickname, "%%Vienna Renamed");
            keep[n].elo_rating = 1234;
            keep[n].previous_elo = 1188;
            keep[n].games_played = 17;
            keep[n].updated_at = 777;
            played = &keep[n];
        }
        n++;
    }
    T_CHECK(gone[0][0] && gone[1][0] && played);
    keep[n++] = (cb_bot_row){ .id = "0c0ffee0-0000-4000-8000-00000000c0ff", .nickname = "%0x00C0FFEE",
                              .strategy_key = "random", .elo_rating = 1500, .previous_elo = 1490, .games_played = 99 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, keep, n), CB_OK);
    bots_t before = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &before), 55);

    // (1) exactly the missing two, as the seed writes them, and the one rename.
    T_EQ_INT(cb_import_seed_bots(s), 3);
    bots_t after = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &after), 57);
    for (int g = 0; g < 2; g++) {
        const cb_bot_row *r = find_bot(&after, gone[g]);
        T_CHECK(r != NULL);
        if (!r) continue;
        T_EQ_STR(r->nickname, g ? "%Moscow 7" : "%Miami 3");
        T_EQ_STR(r->strategy_key, g ? "fusion" : "random");
        T_EQ_INT(r->elo_rating, TABLE_RATING_START);
        T_EQ_INT(r->previous_elo, -1);
        T_EQ_INT(r->games_played, 0);
    }
    // (2) every row that was there is there: the played one with its registry
    // name back and everything else of it as it was, the rest (the stranger
    // included) unchanged.
    for (int i = 0; i < before.n; i++) {
        const cb_bot_row *r = find_bot(&after, before.rows[i].id);
        T_CHECK(r != NULL);
        if (!r || strcmp(r->id, played->id) == 0) continue;
        T_CHECK(same_bot(r, &before.rows[i]));
    }
    const cb_bot_row *p = find_bot(&after, played->id);
    T_CHECK(p != NULL);
    if (p) {
        T_EQ_STR(p->nickname, "%Vienna 2");
        T_EQ_STR(p->strategy_key, "blackpowder");
        T_EQ_INT(p->elo_rating, 1234);
        T_EQ_INT(p->previous_elo, 1188);
        T_EQ_INT(p->games_played, 17);
        T_EQ_INT(p->updated_at, 777);
    }

    // (3) a second run finds nothing missing or misnamed and writes nothing.
    T_EQ_INT(cb_import_seed_bots(s), 0);
    bots_t again = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &again), 57);
    for (int i = 0; i < after.n; i++) T_CHECK(same_bot(&again.rows[i], &after.rows[i]));

    // bots_add itself is all or none: a taken id refuses the whole write.
    cb_bot_row two[2] = { { .id = "0c0ffee0-0000-4000-8000-00000000beef", .nickname = "%New 1", .strategy_key = "random",
                            .elo_rating = 1000, .previous_elo = -1 },
                          after.rows[0] };
    T_EQ_INT(CB_ROWS_CALL(s, bots_add, two, 2), CB_E_CONFLICT);
    bots_t last = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &last), 57);
    T_CHECK(find_bot(&last, two[0].id) == NULL);

    // So is bots_rename: an id that is no bot's refuses the whole write, and
    // the stored row before it keeps its name.
    cb_bot_row ren[2] = { after.rows[0], { .id = "0c0ffee0-0000-4000-8000-00000000beef", .nickname = "%Nobody 1" } };
    snprintf(ren[0].nickname, sizeof ren[0].nickname, "%%Elsewhere 1");
    T_EQ_INT(CB_ROWS_CALL(s, bots_rename, ren, 2), CB_E_CONFLICT);
    last.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &last), 57);
    const cb_bot_row *first = find_bot(&last, after.rows[0].id);
    T_CHECK(first && same_bot(first, &after.rows[0]));
    T_CHECK(find_bot(&last, ren[1].id) == NULL);
    cb_store_close(s);
}

// THE GO-LIVE ON PRODUCTION'S OWN STORE (2026-10-29): a store holding exactly
// production's 49 bot rows, named as production named them (octogen on
// Moscow, cordite on St. Petersburg) and played as production played them,
// is brought to the registry's ladder by one seed run: cordite's seven become
// Warsaw 1..7 and octogen's St. Petersburg 1..7, every rating, game count,
// strategy and timestamp untouched (the rating follows the bot), and fusion's
// seven are added as Moscow 1..7 at the start rating. 14 renames + 7 adds = 21
// rows written, and nothing on a second run.
//
// Mutations that turned it red, each restored after: the renames skipped in
// cb_import_seed_bots (`== 21`, and octogen still on Moscow beside fusion);
// the SQL rename also setting elo_rating (the rating assertions); one of
// fusion's ids changed in bots.c (fusion's accounts at the start rating).
static void seed_brings_production_to_the_ladder(void) {
    enum { N = (int)(sizeof PROD_BOTS / sizeof PROD_BOTS[0]) };
    cb_store *s = open_fresh();
    cb_bot_row prod[N];
    for (int i = 0; i < N; i++) {
        prod[i] = (cb_bot_row){ .elo_rating = 900 + 7 * i, .previous_elo = 890 + 7 * i, .games_played = i,
                                .created_at = 1000 + i, .updated_at = 2000 + i };
        snprintf(prod[i].id, sizeof prod[i].id, "%s", PROD_BOTS[i].id);
        snprintf(prod[i].nickname, sizeof prod[i].nickname, "%s", PROD_BOTS[i].nickname);
        snprintf(prod[i].strategy_key, sizeof prod[i].strategy_key, "%s", PROD_BOTS[i].key);
    }
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, prod, N), CB_OK);

    T_EQ_INT(cb_import_seed_bots(s), 21);
    bots_t b = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &b), N + 7);
    for (int i = 0; i < N; i++) {
        const cb_bot_row *r = find_bot(&b, prod[i].id);
        T_CHECK(r != NULL);
        if (!r) continue;
        char want[64];
        ladder_name(want, sizeof want, PROD_BOTS[i].key, PROD_BOTS[i].nickname);
        T_EQ_STR(r->nickname, want);
        T_EQ_STR(r->strategy_key, prod[i].strategy_key);
        T_EQ_INT(r->elo_rating, prod[i].elo_rating);
        T_EQ_INT(r->previous_elo, prod[i].previous_elo);
        T_EQ_INT(r->games_played, prod[i].games_played);
        T_EQ_INT(r->created_at, prod[i].created_at);
        T_EQ_INT(r->updated_at, prod[i].updated_at);
    }
    int moscow = 0, petersburg = 0, warsaw = 0;
    for (int i = 0; i < b.n; i++) {
        const cb_bot_row *r = &b.rows[i];
        if (strncmp(r->nickname, "%Moscow ", 8) == 0) moscow += strcmp(r->strategy_key, "fusion") == 0 ? 1 : 100;
        if (strncmp(r->nickname, "%St. Petersburg ", 16) == 0) petersburg += strcmp(r->strategy_key, "octogen") == 0 ? 1 : 100;
        if (strncmp(r->nickname, "%Warsaw ", 8) == 0) warsaw += strcmp(r->strategy_key, "cordite") == 0 ? 1 : 100;
    }
    T_EQ_INT(moscow, 7);
    T_EQ_INT(petersburg, 7);
    T_EQ_INT(warsaw, 7);
    for (int i = 0; i < 7; i++) {
        const cb_bot_row *r = find_bot(&b, FUSION_IDS[i]);
        T_CHECK(r && r->elo_rating == TABLE_RATING_START && r->games_played == 0);
    }
    T_EQ_INT(cb_import_seed_bots(s), 0);
    cb_store_close(s);
}

// ---- open ----------------------------------------------------------------------------

static void open_applies_schema_and_seeds_bots(void) {
    cb_store *s = open_fresh();
    T_EQ_STR(s->ops->name, tsb_dynamo() ? "dynamo" : "sqlite");
    bots_t b = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &b), 56);
    T_EQ_STR(b.rows[0].nickname, "%Miami 1");
    T_EQ_STR(b.rows[0].strategy_key, "random");
    T_EQ_STR(b.rows[6].nickname, "%Miami 7");
    T_EQ_STR(b.rows[48].nickname, "%St. Petersburg 7");
    T_EQ_STR(b.rows[48].strategy_key, "octogen");
    T_EQ_STR(b.rows[55].nickname, "%Moscow 7");
    T_EQ_STR(b.rows[55].strategy_key, "fusion");
    T_EQ_STR(b.rows[0].id, "176d94a4-71f2-4542-9510-5603e52058d7");   // production's own %Miami 1
    T_EQ_INT(b.rows[0].elo_rating, 1000);
    T_EQ_INT(b.rows[0].previous_elo, -1);
    if (tsb_dynamo()) {
        // A second seed finds the bots there and writes nothing.
        T_EQ_INT(cb_import_seed_bots(s), 0);
        b.n = 0;
        T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &b), 56);
        cb_store_close(s);
        return;
    }
    cb_store_close(s);

    // Reopening applies the (idempotent) schema again and keeps the rows.
    char path[320];
    snprintf(path, sizeof path, "%s/s%d.sqlite", g_dir, g_seq);
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    b.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &b), 56);
    cb_store_close(s);

    // Schema 3 -> 4: the API Gateway connection tables, rows and all, are
    // dropped (realtime keeps no rows since it moved to IoT Core).
    sqlite3 *db;
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "CREATE TABLE rt_conns (conn_id TEXT PRIMARY KEY, user_id TEXT NOT NULL DEFAULT '', "
                              "expires_at INTEGER NOT NULL, ver INTEGER NOT NULL DEFAULT 0); "
                              "CREATE TABLE rt_conn_topics (conn_id TEXT NOT NULL, ord INTEGER NOT NULL, topic TEXT NOT NULL); "
                              "INSERT INTO rt_conns VALUES ('Old=', '', 9999999999999, 1); "
                              "INSERT INTO rt_conn_topics VALUES ('Old=', 0, 'pv-u1'); "
                              "PRAGMA user_version = 3", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    cb_store_close(s);
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    sqlite3_stmt *st;
    T_EQ_INT(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE name LIKE 'rt_conn%'", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_INT(sqlite3_column_int(st, 0), 0);
    sqlite3_finalize(st);
    T_EQ_INT(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_INT(sqlite3_column_int(st, 0), CB_SCHEMA_VERSION);
    sqlite3_finalize(st);
    sqlite3_close(db);

    // Schema 2 -> 6, from a file as version 2 wrote it (its users and
    // refresh_tokens as they were, nothing of sessions). Schema 6 keeps every
    // row as an item and drops the old tables with what a development
    // database had in them: the old account and its token are gone, and the
    // file opens and takes new rows. Built from that version's own
    // statements, not by dropping columns.
    char path2[340];
    snprintf(path2, sizeof path2, "%s/v2-%d.sqlite", g_dir, g_seq);
    unlink(path2);
    T_EQ_INT(sqlite3_open(path2, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "CREATE TABLE users (id TEXT PRIMARY KEY, email TEXT NOT NULL UNIQUE, "
                              "password_hash TEXT NOT NULL, username TEXT, created_at INTEGER NOT NULL, last_sign_in_at INTEGER); "
                              "CREATE TABLE refresh_tokens (token_hash TEXT PRIMARY KEY, "
                              "user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, session_id TEXT NOT NULL, "
                              "created_at INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0, revoked_at INTEGER); "
                              "INSERT INTO users (id, email, password_hash, created_at) "
                              "VALUES ('00000000-0000-4000-8000-0000000000aa', 'old@x.io', 'h', 1); "
                              "INSERT INTO refresh_tokens (token_hash, user_id, session_id, created_at) "
                              "VALUES ('oldtok', '00000000-0000-4000-8000-0000000000aa', 's-old', 5); "
                              "PRAGMA user_version = 2", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    T_EQ_INT(cb_store_open_sqlite(path2, &s), CB_OK);
    if (!s) return;
    cb_user_row old_user;
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "old@x.io", &old_user), CB_E_NOT_FOUND);
    cb_refresh_row old_tok;
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "oldtok", 10, 1000, &old_tok), CB_E_NOT_FOUND);
    cb_user_row fresh = mkuser("old@x.io", "OLD", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &fresh), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "old@x.io", &old_user), CB_OK);
    T_EQ_INT(old_user.session_gen, 0);
    cb_store_close(s);
    // Opened again at 6: nothing is repeated (the file stays openable, the row stays).
    T_EQ_INT(cb_store_open_sqlite(path2, &s), CB_OK);
    if (s) cb_store_close(s);
    unlink(path2);

    // A database from a newer binary is refused.
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "PRAGMA user_version = 99", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    s = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_E_STATE);
    T_CHECK(s == NULL);
}

// ---- users and refresh tokens ------------------------------------------------------------

static void users_round_trip(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("ann@x.io", "Ann", 1000), b = mkuser("bob@x.io", "", 2000), got;
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_STR(got.email, "ann@x.io");
    T_EQ_STR(got.username, "Ann");
    T_EQ_STR(got.password_hash, a.password_hash);
    T_EQ_INT(got.created_at, 1000);
    T_EQ_INT(got.last_sign_in_at, 0);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "bob@x.io", &got), CB_OK);
    T_EQ_STR(got.id, b.id);
    T_EQ_STR(got.username, "");
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "nobody@x.io", &got), CB_E_NOT_FOUND);

    // The email is unique.
    cb_user_row dup = mkuser("ann@x.io", "Other", 3000);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &dup), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, user_get, dup.id, &got), CB_E_NOT_FOUND);
    // The name is unique too, whatever the email (the caller stores it canonical: auth/names.h), and
    // no name at all may repeat.
    cb_user_row twin = mkuser("ann2@x.io", "Ann", 3000), anon1 = mkuser("n1@x.io", "", 3000), anon2 = mkuser("n2@x.io", "", 3000);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &twin), tsb_dynamo() ? CB_OK : CB_E_CONFLICT);   // DynamoDB: the email only
    if (!tsb_dynamo()) T_EQ_INT(CB_STORE_CALL(s, user_get, twin.id, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &anon1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &anon2), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, anon1.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, anon2.id), CB_OK);
    if (tsb_dynamo()) CB_STORE_CALL(s, user_delete, twin.id);

    // The trigger's rating row came with the account, off the leaderboard
    // until they play (the index key is sparse).
    T_EQ_INT(rating_of(s, a.id), TABLE_RATING_START);
    ratings_t r = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, ratings_list, collect_rating, &r), 0);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 100, collect_rating, &r), 0);

    T_EQ_INT(got.session_gen, 0);
    T_EQ_STR(got.kept_session, "");
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, a.id, "$2b$10$new", "33333333-cccc-4ccc-8ccc-333333333333"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_touch_sign_in, a.id, 5555), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_STR(got.password_hash, "$2b$10$new");
    T_EQ_INT(got.last_sign_in_at, 5555);
    T_EQ_INT(got.session_gen, 1);   // the same write ends the other sessions
    T_EQ_STR(got.kept_session, "33333333-cccc-4ccc-8ccc-333333333333");
    T_EQ_INT(CB_STORE_CALL(s, user_end_sessions, a.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "ann@x.io", &got), CB_OK);
    T_EQ_INT(got.session_gen, 2);   // a global sign-out keeps none
    T_EQ_STR(got.kept_session, "");
    T_EQ_INT(CB_STORE_CALL(s, user_set_password, "00000000-0000-4000-8000-000000000000", "x", ""), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_end_sessions, "00000000-0000-4000-8000-000000000000"), CB_E_NOT_FOUND);

    // users_list: newest first, search on email or username, offset and limit.
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 0, 10, collect_user, &n), 2);
    T_EQ_STR(n.ids[0], "bob@x.io");
    T_EQ_STR(n.ids[1], "ann@x.io");
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, users_list, "ANN", 0, 10, collect_user, &n), 1);
    T_EQ_STR(n.ids[0], "ann@x.io");
    n.n = 0;
    T_EQ_INT(CB_STORE_CALL(s, users_list, "", 1, 10, collect_user, &n), 1);
    T_EQ_STR(n.ids[0], "ann@x.io");

    T_EQ_INT(CB_ROWS_CALL(s, rating_set_username, a.id, NULL), CB_OK);
    r.n = 0;
    CB_ROWS_CALL(s, ratings_list, collect_rating, &r);
    for (int i = 0; i < r.n; i++)
        if (strcmp(r.rows[i].user_id, a.id) == 0) T_EQ_STR(r.rows[i].username, "");
    cb_store_close(s);
}

static void refresh_tokens_rotate(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_refresh_row r = { .token_hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .session_id = "44444444-dddd-4ddd-8ddd-444444444444", .created_at = 7, .expires_at = 100000 }, t1 = r, got;
    memcpy(r.user_id, a.id, sizeof r.user_id);
    t1 = r;
    snprintf(t1.token_hash, sizeof t1.token_hash, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &t1), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_put, &r), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 100, 1000, &got), CB_OK);
    T_CHECK(!got.revoked);
    T_EQ_INT(got.revoked_at, 0);
    T_EQ_STR(got.user_id, a.id);
    T_EQ_STR(got.session_id, "44444444-dddd-4ddd-8ddd-444444444444");
    T_EQ_INT(got.created_at, 7);
    T_EQ_INT(got.expires_at, 100000);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 200, 1000, &got), CB_OK);
    T_CHECK(got.revoked);   // a second use is a reuse
    T_EQ_INT(got.revoked_at, 100);   // stamped by the first take
    T_EQ_INT(got.expires_at, 1100);  // a spent token is kept spent_ms after a take
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 300, 1000, &got), CB_OK);
    T_EQ_INT(got.revoked_at, 100);   // and only the first stamps
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 300, 1000, &got), CB_E_NOT_FOUND);
    // Past its time a token is gone, whatever the TTL has swept.
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1300, 1000, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 99999, 1000, &got), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 200000, 1000, &got), CB_E_NOT_FOUND);
    cb_store_close(s);
}

// Sessions (store.h cb_session_row): put, read back consistently, ended by
// one write (an unknown one gets its row, ended), gone past their end.
static void sessions_round_trip(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_session_row r = { .id = "11111111-aaaa-4aaa-8aaa-111111111111", .created_at = 1000, .expires_at = 5000, .gen = 3 }, got;
    memcpy(r.user_id, a.id, sizeof r.user_id);
    T_EQ_INT(CB_STORE_CALL(s, session_get, r.id, 2000, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, session_put, &r), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, session_get, r.id, 2000, &got), CB_OK);
    T_EQ_STR(got.id, r.id);
    T_EQ_STR(got.user_id, a.id);
    T_CHECK(got.created_at == 1000 && got.expires_at == 5000 && got.gen == 3 && !got.revoked);
    T_EQ_INT(CB_STORE_CALL(s, session_revoke, r.id, a.id, 9999), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, session_get, r.id, 2000, &got), CB_OK);
    T_CHECK(got.revoked);
    T_CHECK(got.expires_at == 5000 && got.created_at == 1000 && got.gen == 3);   // the revoke changed nothing else
    T_EQ_INT(CB_STORE_CALL(s, session_get, r.id, 5000, &got), CB_E_NOT_FOUND);   // past its end
    // A session with no row (begun before sessions were stored): revoked, it gets one.
    T_EQ_INT(CB_STORE_CALL(s, session_revoke, "22222222-bbbb-4bbb-8bbb-222222222222", a.id, 7000), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, session_get, "22222222-bbbb-4bbb-8bbb-222222222222", 2000, &got), CB_OK);
    T_CHECK(got.revoked && got.expires_at == 7000);
    T_EQ_STR(got.user_id, a.id);
    cb_store_close(s);
}

// ---- games ---------------------------------------------------------------------------

typedef struct {
    int         n;
    cb_game_row rows[4];   // the byte fields are not kept: state is checked in the walk
    bool        state_ok, logs;
} views_got;

static bool collect_view_row(const cb_game_row *r, void *ud) {
    views_got *v = ud;
    v->state_ok &= r->state_len == sizeof g_state && memcmp(r->state, g_state, sizeof g_state) == 0 &&
                   r->roster_len == sizeof g_roster && memcmp(r->roster, g_roster, sizeof g_roster) == 0;
    v->logs |= r->list != NULL || r->list_len != 0;
    if (v->n < 4) {
        v->rows[v->n] = *r;
        v->rows[v->n].state = v->rows[v->n].roster = v->rows[v->n].list = NULL;
        v->n++;
    }
    return true;
}

static void game_round_trip(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_game_row g = mkgame("ab12cd", 100), got;
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "ab12cd", &got), CB_OK);
    T_EQ_MEM(got.state, got.state_len, g_state, sizeof g_state);
    T_EQ_MEM(got.roster, got.roster_len, g_roster, sizeof g_roster);
    T_CHECK(got.list == NULL && got.list_len == 0);
    T_CHECK(!got.has_seed && !got.needs_bots);
    T_EQ_INT(got.version, 0);
    T_EQ_INT(got.created_at, 100);
    T_EQ_INT(got.last_commit_at, 100);
    cb_game_row_free(&got);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "ab12cd", a.id), 1);
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "ffffff", &got), CB_E_NOT_FOUND);

    // A commit writes every column.
    cb_game_row n = mkgame("ab12cd", 100);
    n.status = 1;
    n.list_from = 1;
    n.has_seed = true;
    for (int i = 0; i < CB_SEED_LEN; i++) n.seed[i] = (uint8_t)(i * 7);
    n.needs_bots = true;
    n.version = 1;
    n.round_epoch = 1;
    n.updated_at = 200;
    n.last_commit_at = 0;   // the store stamps the commit's time itself
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 1, .list_add = g_logs, .list_add_len = sizeof g_logs,
                         .list_clear = true };
    memcpy(c.humans[0], a.id, sizeof c.humans[0]);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "ab12cd", &got), CB_OK);
    T_EQ_INT(got.status, 1);
    T_CHECK(got.list == NULL);
    T_EQ_INT(got.list_from, 1);
    CB_ROWS_CALL(s, game_list, "ab12cd", got.list_from, &got.list, &got.list_len);
    T_EQ_MEM(got.list, got.list_len, g_logs, sizeof g_logs);
    T_CHECK(got.has_seed);
    T_EQ_MEM(got.seed, CB_SEED_LEN, n.seed, CB_SEED_LEN);
    T_CHECK(got.needs_bots);
    T_EQ_INT(got.version, 1);
    T_EQ_INT(got.round_epoch, 1);
    T_EQ_INT(got.created_at, 100);
    T_EQ_INT(got.updated_at, 200);
    T_EQ_INT(got.last_commit_at, 200);
    cb_game_row_free(&got);

    // The dashboard's read: every column but the log, in the order asked, a
    // game that does not exist left out.
    cb_game_row g2 = mkgame("ef34gh", 150);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g2, a.id), CB_OK);
    const char *ids[3] = { "ef34gh", "ffffff", "ab12cd" };
    views_got v = { .state_ok = true };
    T_EQ_INT(CB_ROWS_CALL(s, games_get_views, ids, 3, collect_view_row, &v), 2);
    T_EQ_INT(v.n, 2);
    T_CHECK(v.state_ok);
    T_CHECK(!v.logs);
    T_EQ_STR(v.rows[0].id, "ef34gh");
    T_EQ_INT(v.rows[0].version, 0);
    T_EQ_INT(v.rows[0].created_at, 150);
    T_EQ_STR(v.rows[1].id, "ab12cd");
    T_EQ_INT(v.rows[1].status, 1);
    T_EQ_INT(v.rows[1].version, 1);
    T_CHECK(v.rows[1].has_seed);
    T_EQ_MEM(v.rows[1].seed, CB_SEED_LEN, n.seed, CB_SEED_LEN);
    T_CHECK(v.rows[1].needs_bots);
    T_EQ_INT(v.rows[1].round_epoch, 1);
    T_EQ_INT(v.rows[1].created_at, 100);
    T_EQ_INT(v.rows[1].updated_at, 200);
    T_EQ_INT(v.rows[1].last_commit_at, 200);
    cb_store_close(s);
}

// Two writers read version 0; the first commit wins, the second loses and
// writes nothing, its membership and ratings included.
static void cas_second_writer_loses(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g = mkgame("cas001", 10);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_OK);

    cb_game_row w1 = mkgame("cas001", 10), w2 = mkgame("cas001", 10);
    uint8_t s1[1] = { 0x11 }, s2[1] = { 0x22 };
    w1.state = s1, w1.state_len = 1, w1.version = 1, w1.updated_at = 20;
    w2.state = s2, w2.state_len = 1, w2.version = 1, w2.updated_at = 21;
    cb_game_commit c1 = { .row = &w1, .expected_version = 0, .n_humans = 1 };
    cb_game_commit c2 = { .row = &w2, .expected_version = 0, .n_humans = 2, .n_rated = 1 };
    memcpy(c1.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c2.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c2.humans[1], b.id, sizeof(cb_uuid));
    memcpy(c2.rated_ids[0], b.id, sizeof(cb_uuid));
    c2.rating_delta[0] = 50;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c1), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c2), CB_E_CONFLICT);
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "cas001", &got), CB_OK);
    T_EQ_INT(got.version, 1);
    T_EQ_MEM(got.state, got.state_len, s1, 1);
    T_EQ_INT(got.updated_at, 20);
    cb_game_row_free(&got);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "cas001", b.id), 0);
    T_EQ_INT(rating_of(s, b.id), 1000);

    // Reloaded at version 1, the second writer wins.
    c2.expected_version = 1;
    w2.version = 2;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c2), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "cas001", b.id), 1);
    T_EQ_INT(rating_of(s, b.id), 1050);

    // One transaction: when one write of a commit (a rating) fails, nothing
    // of it stays, the row included. A trigger on the rating item makes it
    // fail (SQLite only, a trigger on the ratings table: DynamoDB has no trigger; unit_dynamo.c cancels the
    // transaction's rating item in flow_rating_moved instead).
    cb_game_row w3 = mkgame("cas001", 10);
    w3.version = 3;
    cb_game_commit c3 = { .row = &w3, .expected_version = 2, .n_humans = 1, .n_rated = 1 };
    memcpy(c3.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c3.rated_ids[0], a.id, sizeof(cb_uuid));
    if (!tsb_dynamo()) {
        T_EQ_INT(CB_STORE_CALL(s, admin_query,
                               "CREATE TRIGGER no_ratings BEFORE UPDATE ON ratings BEGIN SELECT RAISE(ABORT, 'no'); END",
                               true, NULL, NULL), 0);
        T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c3), CB_E_DB);
        T_EQ_INT(CB_ROWS_CALL(s, game_get, "cas001", &got), CB_OK);
        T_EQ_INT(got.version, 2);
        cb_game_row_free(&got);
        T_EQ_INT(CB_ROWS_CALL(s, is_member, "cas001", b.id), 1);   // the seats were not rewritten either
        CB_STORE_CALL(s, admin_query, "DROP TRIGGER no_ratings", true, NULL, NULL);
    }

    // A seat with no account (deleted, its id still in the roster) is skipped,
    // not a reason to refuse the commit.
    c3.n_rated = 0;
    snprintf(c3.humans[0], sizeof c3.humans[0], "00000000-0000-4000-8000-0000000000ff");
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c3), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "cas001", c3.humans[0]), 0);

    // A commit to a game that is gone is NOT_FOUND, not a conflict to retry.
    cb_game_row x = mkgame("nogame", 1);
    x.version = 1;
    cb_game_commit cx = { .row = &x, .expected_version = 0 };
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &cx), CB_E_NOT_FOUND);
    cb_store_close(s);
}

// A move between humans: the row the commit is fenced on vouches for its
// seats and had no bot work, and the commit seats the same players with no
// bot work after, so DynamoDB writes it as one conditional UpdateItem. The
// fence is the same: a second writer from the same version loses and writes
// nothing, a game that is gone is NOT_FOUND and not created, and a commit
// after bot work still takes the SCAN marker away (the transaction).
static void lone_commit_cas(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    const int64_t NOW = 10000000, STALE = NOW - 10000, ACTIVE = NOW - 3600000;
    cb_game_row g = mkgame("lone01", NOW - 20000);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_OK);
    // b sits down (a roster change: the transaction).
    cb_game_row j = mkgame("lone01", NOW - 20000);
    j.version = 1;
    cb_game_commit cj = { .row = &j, .expected_version = 0, .n_humans = 2, .prev_known = true, .n_prev_humans = 1 };
    memcpy(cj.humans[0], a.id, sizeof(cb_uuid));
    memcpy(cj.humans[1], b.id, sizeof(cb_uuid));
    memcpy(cj.prev_humans[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &cj), CB_OK);

    // Two writers read version 1 and move.
    cb_game_row w1 = mkgame("lone01", NOW - 20000), w2 = mkgame("lone01", NOW - 20000);
    uint8_t s1[1] = { 0x11 }, s2[1] = { 0x22 };
    w1.state = s1, w1.state_len = 1, w1.version = 2, w1.updated_at = NOW - 19000;
    w2.state = s2, w2.state_len = 1, w2.version = 2, w2.updated_at = NOW - 18000;
    cb_game_commit c1 = { .row = &w1, .expected_version = 1, .n_humans = 2, .prev_known = true, .n_prev_humans = 2 };
    memcpy(c1.humans, cj.humans, sizeof c1.humans);
    memcpy(c1.prev_humans[0], b.id, sizeof(cb_uuid));   // the stored seats, in any order
    memcpy(c1.prev_humans[1], a.id, sizeof(cb_uuid));
    cb_game_commit c2 = c1;
    c2.row = &w2;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c1), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c2), CB_E_CONFLICT);
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "lone01", &got), CB_OK);
    T_EQ_INT(got.version, 2);
    T_EQ_MEM(got.state, got.state_len, s1, 1);
    T_EQ_INT(got.updated_at, NOW - 19000);
    T_EQ_INT(got.last_commit_at, NOW - 19000);
    T_CHECK(!got.needs_bots);
    T_CHECK(got.seats_exact);
    cb_game_row_free(&got);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "lone01", a.id), 1);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "lone01", b.id), 1);
    names_t nm = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, b.id, 10, collect_member, &nm), 1);

    // Reloaded at version 2, the second writer wins, alone again.
    c2.expected_version = 2;
    w2.version = 3;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c2), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "lone01", &got), CB_OK);
    T_EQ_INT(got.version, 3);
    T_EQ_MEM(got.state, got.state_len, s2, 1);
    cb_game_row_free(&got);

    // A game that is gone: NOT_FOUND, and the update made no row of it.
    cb_game_row x = mkgame("nolone", 1);
    x.version = 1;
    cb_game_commit cx = c1;
    cx.row = &x;
    cx.expected_version = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &cx), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "nolone", &got), CB_E_NOT_FOUND);

    // Bot work at version 4 (the transaction puts the marker) ...
    cb_game_row w4 = mkgame("lone01", NOW - 20000);
    w4.version = 4;
    w4.needs_bots = true;
    cb_game_commit c4 = c1;
    c4.row = &w4;
    c4.expected_version = 3;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c4), CB_OK);
    nm.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 1);
    // ... and none at 5: fenced on a row that had bot work, it is the
    // transaction, and the marker goes.
    cb_game_row w5 = mkgame("lone01", NOW - 20000);
    w5.version = 5;
    cb_game_commit c5 = c1;
    c5.row = &w5;
    c5.expected_version = 4;
    c5.prev_needs_bots = true;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c5), CB_OK);
    nm.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 0);
    cb_uuid tok;
    if (tsb_dynamo()) T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lone01", NOW, 25000, tok), CB_E_NOT_FOUND);   // no marker
    // Then alone again, and still no marker.
    cb_game_row w6 = mkgame("lone01", NOW - 20000);
    w6.version = 6;
    cb_game_commit c6 = c1;
    c6.row = &w6;
    c6.expected_version = 5;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c6), CB_OK);
    nm.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 0);
    cb_store_close(s);
}

static bool first_value(int n, const char *const *names, const char *const *values, void *ud) {
    char *out = ud;
    snprintf(out, 64, "%s", n > 0 && values[0] ? values[0] : "(null)");
    return true;
}

// Membership follows every commit: listed seats added, the rest removed.
static void membership_follows_commits(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g1 = mkgame("game01", 10), g2 = mkgame("game02", 11);
    CB_ROWS_CALL(s, game_create, &g1, a.id);
    CB_ROWS_CALL(s, game_create, &g2, a.id);

    cb_game_row n = mkgame("game01", 10);
    n.version = 1;
    n.updated_at = 50;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 2, .n_bots = 1 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c.humans[1], b.id, sizeof(cb_uuid));
    snprintf(c.bots[0], sizeof c.bots[0], "176d94a4-71f2-4542-9510-5603e52058d7");
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "game01", b.id), 1);
    // The game's members: both humans, never the bot (a chat is published to each).
    names_t who = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "game01", collect_name, &who), 2);
    T_EQ_INT(who.n, 2);
    T_CHECK((strcmp(who.ids[0], a.id) == 0 && strcmp(who.ids[1], b.id) == 0) ||
            (strcmp(who.ids[0], b.id) == 0 && strcmp(who.ids[1], a.id) == 0));
    who.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "game02", collect_name, &who), 1);   // its creator
    T_EQ_STR(who.ids[0], a.id);
    who.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "nogame", collect_name, &who), 0);
    // fn answering false stops after the one it was handed.
    who.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "game01", collect_one_name, &who), 1);
    T_EQ_INT(who.n, 1);

    // a's dashboard. DynamoDB: most recently seated first, which is when the
    // seat was written (seats are not rewritten on every commit), so game01,
    // seated at 10, sorts after game02, seated at 11. SQL: the games' own
    // order, most recently updated first (game01's commit at 50).
    names_t m = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, a.id, 10, collect_member, &m), 2);
    T_EQ_STR(m.ids[0], tsb_dynamo() ? "game02" : "game01");
    T_EQ_INT(m.at[0], tsb_dynamo() ? 11 : 50);
    T_EQ_STR(m.ids[1], tsb_dynamo() ? "game01" : "game02");
    m.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, a.id, 1, collect_member, &m), 1);

    // a leaves game01: the next commit seats only b, and no bot.
    n.version = 2;
    n.updated_at = 60;
    c.expected_version = 1;
    memcpy(c.humans[0], b.id, sizeof(cb_uuid));
    c.n_humans = 1;
    c.n_bots = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "game01", a.id), 0);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "game01", b.id), 1);
    who.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, game_members, "game01", collect_name, &who), 1);   // read consistently: a is gone at once
    T_EQ_STR(who.ids[0], b.id);
    char v[64], q[256];
    if (tsb_dynamo()) {
        // PartiQL: the game's BOT# seat items, of which none is left.
        snprintf(q, sizeof q, "SELECT sk FROM \"%s\" WHERE pk = 'GAME#game01' AND begins_with(sk, 'BOT#')", tsb_table());
        T_EQ_INT(CB_STORE_CALL(s, admin_query, q, false, first_value, v), 0);
        snprintf(q, sizeof q, "SELECT sk FROM \"%s\" WHERE pk = 'GAME#game01' AND begins_with(sk, 'MEMBER#')", tsb_table());
        T_EQ_INT(CB_STORE_CALL(s, admin_query, q, false, first_value, v), 1);
    } else {
        // SQL over the seat tables: the same two kinds of seat.
        (void)q;
        T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT count(*) FROM game_bots WHERE game_id = 'game01'", false, first_value, v), 1);
        T_EQ_STR(v, "0");
        T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT count(*) FROM game_members WHERE game_id = 'game01'", false, first_value, v),
                 1);
        T_EQ_STR(v, "1");
    }
    cb_store_close(s);
}

typedef struct { cb_snapshot_row row; } snapcopy;
static bool copy_snap(const cb_snapshot_row *r, void *ud) {
    snapcopy *c = ud;
    c->row = *r;
    c->row.moves = malloc(r->moves_len);
    memcpy(c->row.moves, r->moves, r->moves_len);
    c->row.extras = r->extras_len ? malloc(r->extras_len) : NULL;
    if (r->extras_len) memcpy(c->row.extras, r->extras, r->extras_len);
    return true;
}

static void game_delete_cascades(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("del001", 10);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    cb_chat_row ch = { .message = "hi", .created_at = 5 };
    cb_uuid_v4(ch.id);
    snprintf(ch.game_id, sizeof ch.game_id, "del001");
    memcpy(ch.user_id, a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &ch), CB_OK);
    uint8_t mv[2] = { 1, 2 };
    cb_snapshot_row sn = { .n_players = 1, .moves = mv, .moves_len = 2, .created_at = 9 };
    cb_uuid_v4(sn.id);
    snprintf(sn.game_id, sizeof sn.game_id, "del001");
    memcpy(sn.player_ids[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);

    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "del001", 3), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "del001", 0), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "del001", 0), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "del001", a.id), 0);
    names_t n = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "del001", 0, collect_chat, &n), 0);
    // The replay outlives the game, its game_id cleared.
    snapcopy c = { 0 };
    cb_snapshot_query q = { .viewer_id = a.id };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_STR(c.row.game_id, "");
    cb_snapshot_row_free(&c.row);
    cb_store_close(s);
}

// user_delete takes the account's tokens, seats, rating and chat with it;
// snapshots keep its id in player_ids (DESIGN.md "Open defects carried over").
static void user_delete_cascades(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g = mkgame("ud0001", 10);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    cb_refresh_row r = { .token_hash = "t1", .session_id = "s" };
    memcpy(r.user_id, a.id, sizeof(cb_uuid));
    CB_STORE_CALL(s, refresh_put, &r);
    cb_chat_row ch = { .message = "bye", .created_at = 5 };
    cb_uuid_v4(ch.id);
    snprintf(ch.game_id, sizeof ch.game_id, "ud0001");
    memcpy(ch.user_id, a.id, sizeof(cb_uuid));
    CB_ROWS_CALL(s, chat_put, &ch);
    uint8_t mv[1] = { 1 };
    cb_snapshot_row sn = { .n_players = 2, .moves = mv, .moves_len = 1, .created_at = 9 };
    cb_uuid_v4(sn.id);
    memcpy(sn.player_ids[0], a.id, sizeof(cb_uuid));
    memcpy(sn.player_ids[1], b.id, sizeof(cb_uuid));
    CB_ROWS_CALL(s, snapshot_put, &sn);

    T_EQ_INT(CB_STORE_CALL(s, user_delete, a.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_delete, a.id), CB_E_NOT_FOUND);
    cb_refresh_row got;
    T_EQ_INT(CB_STORE_CALL(s, refresh_take, "t1", 1, 1000, &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "ud0001", a.id), 0);
    T_EQ_INT(rating_of(s, a.id), TABLE_RATING_NONE);
    names_t n = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "ud0001", 0, collect_chat, &n), 0);
    cb_snapshot_query q = { .viewer_id = b.id };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), 1);
    cb_store_close(s);
}

// ---- identities and one-time claims (store.h cb_identity_row, once_claim) ---------------------

static cb_identity_row mkident(int idp, const char *sub, const char *user_id, int64_t at) {
    cb_identity_row r = { 0 };
    r.idp = idp;
    snprintf(r.subject, sizeof r.subject, "%s", sub);
    snprintf(r.user_id, sizeof r.user_id, "%s", user_id);
    snprintf(r.client_id, sizeof r.client_id, "com.example.app");
    snprintf(r.email, sizeof r.email, "x7k2@privaterelay.appleid.com");
    r.email_verified = true;
    r.private_email = true;
    snprintf(r.grant, sizeof r.grant, "00112233445566778899aabbccddeeff");
    r.created_at = r.updated_at = at;
    return r;
}

static bool count_ident(const cb_identity_row *r, void *ud) {
    (void)r;
    (*(int *)ud)++;
    return true;
}

static void identities_round_trip(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("ia@x.io", "IA", 1), b = mkuser("ib@x.io", "IB", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &b), CB_OK);
    cb_identity_row r = mkident(1, "001234.sub.one", a.id, 5), got;
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &r, true), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &r, true), CB_E_CONFLICT);   // fresh: already there
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.one", &got), CB_OK);
    T_EQ_INT(got.idp, 1);
    T_EQ_STR(got.subject, "001234.sub.one");
    T_EQ_STR(got.user_id, a.id);
    T_EQ_STR(got.client_id, "com.example.app");
    T_EQ_STR(got.email, "x7k2@privaterelay.appleid.com");
    T_CHECK(got.email_verified && got.private_email);
    T_EQ_STR(got.grant, "00112233445566778899aabbccddeeff");
    T_EQ_INT(got.created_at, 5);
    T_EQ_INT(got.updated_at, 5);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 2, "001234.sub.one", &got), CB_E_NOT_FOUND);   // another provider
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.on", &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "", &got), CB_E_NOT_FOUND);
    // Replaced whole when not fresh.
    snprintf(r.email, sizeof r.email, "ada@example.com");
    r.private_email = false;
    r.grant[0] = 0;
    r.updated_at = 9;
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &r, false), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.one", &got), CB_OK);
    T_EQ_STR(got.email, "ada@example.com");
    T_CHECK(!got.private_email);
    T_EQ_STR(got.grant, "");
    T_EQ_INT(got.updated_at, 9);
    // A user's identities.
    cb_identity_row r2 = mkident(1, "001234.sub.two", a.id, 6);
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &r2, true), CB_OK);
    int n = 0;
    T_EQ_INT(CB_STORE_CALL(s, identities_of, a.id, count_ident, &n), 2);
    T_EQ_INT(n, 2);
    n = 0;
    T_EQ_INT(CB_STORE_CALL(s, identities_of, b.id, count_ident, &n), 0);
    T_EQ_INT(n, 0);
    // Refused shapes.
    cb_identity_row bad = mkident(1, "", a.id, 1);
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &bad, true), CB_E_INVALID);
    bad = mkident(1, "s", "not-a-uuid", 1);
    T_EQ_INT(CB_STORE_CALL(s, identity_put, &bad, true), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, identities_of, "not-a-uuid", count_ident, &n), CB_E_INVALID);
    // The account and its identity in one write.
    cb_user_row c = mkuser("ic@x.io", "IC", 2);
    cb_identity_row r3 = mkident(1, "001234.sub.three", c.id, 2);
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &c, &r3), CB_OK);
    cb_user_row cu;
    T_EQ_INT(CB_STORE_CALL(s, user_get, c.id, &cu), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.three", &got), CB_OK);
    T_EQ_STR(got.user_id, c.id);
    // A taken email: neither is written.
    cb_user_row d = mkuser("ic@x.io", "ID", 3);
    cb_identity_row r4 = mkident(1, "001234.sub.four", d.id, 3);
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &d, &r4), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.four", &got), CB_E_NOT_FOUND);
    // A taken identity: neither is written.
    cb_user_row e = mkuser("ie@x.io", "IE", 3);
    cb_identity_row r5 = mkident(1, "001234.sub.one", e.id, 3);
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &e, &r5), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, user_get, e.id, &cu), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, user_by_email, "ie@x.io", &cu), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.one", &got), CB_OK);
    T_EQ_STR(got.user_id, a.id);
    // An identity for another user than the account: refused.
    cb_identity_row r6 = mkident(1, "001234.sub.six", a.id, 3);
    T_EQ_INT(CB_STORE_CALL(s, user_create_identity, &e, &r6), CB_E_INVALID);
    // The account's delete takes its identities.
    T_EQ_INT(CB_STORE_CALL(s, user_delete, a.id), CB_OK);
    n = 0;
    T_EQ_INT(CB_STORE_CALL(s, identities_of, a.id, count_ident, &n), 0);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.one", &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.two", &got), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, identity_get, 1, "001234.sub.three", &got), CB_OK);   // another account's stays
    cb_store_close(s);
}

static void once_claims(void) {
    cb_store *s = open_fresh();
    const char *k1 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *k2 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    int64_t t = 1790000000000LL;
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k1, t + 1000, t), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k1, t + 1000, t + 500), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k1, t + 5000, t + 999), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k2, t + 1000, t + 500), CB_OK);   // another key
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k1, t + 3000, t + 1000), CB_OK);  // past its time: claimed again
    T_EQ_INT(CB_STORE_CALL(s, once_claim, k1, t + 3000, t + 2000), CB_E_CONFLICT);
    T_EQ_INT(CB_STORE_CALL(s, once_claim, "short", t + 1000, t), CB_E_INVALID);
    cb_store_close(s);
}

// ---- the lease -----------------------------------------------------------------------

static void lease_semantics(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("lease1", 10);
    g.needs_bots = true;   // a drive takes the lease only on a game with bot work
    CB_ROWS_CALL(s, game_create, &g, a.id);
    const int64_t T0 = 1000000, TTL = 25000;
    cb_uuid t1, t2;
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "lease1", T0), 0);   // never taken
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0, TTL, t1), CB_OK);
    T_CHECK(cb_uuid_valid(t1, strlen(t1)));
    // Held exactly while an acquire is refused: up to and including its last ms.
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "lease1", T0 + TTL), 1);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "lease1", T0 + TTL + 1), 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "nogame", T0), 0);
    // A second acquire while the lease is live fails, up to and including its last ms.
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + 1, TTL, t2), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + TTL, TTL, t2), CB_E_CONFLICT);
    // Renew needs the holder's token.
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, "lease1", "00000000-0000-4000-8000-000000000000", T0 + 10, TTL), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, "lease1", t1, T0 + 10000, TTL), CB_OK);
    // The renewal moved the expiry: T0 + TTL + 1 is inside it now.
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + TTL + 1, TTL, t2), CB_E_CONFLICT);
    // A release by someone else's token does nothing.
    T_EQ_INT(CB_ROWS_CALL(s, lease_release, "lease1", "00000000-0000-4000-8000-000000000000"), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + 10001, TTL, t2), CB_E_CONFLICT);
    // Released by its holder, it is free at once.
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "lease1", T0 + 10001), 1);
    T_EQ_INT(CB_ROWS_CALL(s, lease_release, "lease1", t1), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, lease_held, "lease1", T0 + 10001), 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + 10002, TTL, t2), CB_OK);
    T_CHECK(strcmp(t1, t2) != 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, "lease1", t1, T0 + 10003, TTL), CB_E_CONFLICT);
    // TTL expiry: a holder that died is replaced once the lease has run out.
    cb_uuid t3;
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + 10002 + TTL + 1, TTL, t3), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, "lease1", t2, T0 + 10002 + TTL + 2, TTL), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, "lease1", t3, T0 + 10002 + TTL + 2, TTL), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "nogame", T0, TTL, t2), CB_E_NOT_FOUND);
    // A game with no bot work has no lease to take.
    cb_game_row idle = mkgame("idle01", 10);
    CB_ROWS_CALL(s, game_create, &idle, a.id);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "idle01", T0, TTL, t2), CB_E_NOT_FOUND);

    // The lease moves the scan's staleness clock (the scan sees a drive in
    // flight), never last_commit_at. The clock is on the SCAN marker the
    // lease lives on, so the row's updated_at stays its last commit's.
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "lease1", &got), CB_OK);
    T_EQ_INT(got.updated_at, 10);
    T_EQ_INT(got.last_commit_at, 10);
    T_EQ_INT(got.version, 0);
    cb_game_row_free(&got);

    // A commit that leaves bot work keeps the drive's lease: still held
    // against a second drive, still the holder's to renew.
    cb_game_row n = mkgame("lease1", 10);
    n.needs_bots = true;
    n.version = 1;
    n.updated_at = T0 + 10002 + TTL + 3;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 1 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", T0 + 10002 + TTL + 4, TTL, t2), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, lease_renew, "lease1", t3, T0 + 10002 + TTL + 5, TTL), CB_OK);
    cb_store_close(s);
}

// A drive that finds no bot can act on a row the scan holds takes the game
// out of the scan (and the lease with it), at the version it read only.
static void bots_idle_semantics(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    const int64_t NOW = 10000000, STALE = NOW - 10000, ACTIVE = NOW - 3600000;
    cb_game_row g = mkgame("idle02", NOW - 20000);
    g.needs_bots = true;
    CB_ROWS_CALL(s, game_create, &g, a.id);
    // A commit moves the row to version 1, still with a bot able to act.
    cb_game_row n = mkgame("idle02", NOW - 20000);
    n.needs_bots = true;
    n.version = 1;
    n.updated_at = NOW - 20000;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 1 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    cb_uuid tok;
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "idle02", STALE - 5000, 1000, tok), CB_OK);
    names_t nm = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 1);
    // A stale read (version 0) changes nothing: the commit's answer stands.
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, "idle02", 0), CB_E_CONFLICT);
    nm.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 1);
    // At the version read: out of the scan, and no lease left to take.
    T_EQ_INT(CB_ROWS_CALL(s, bots_idle, "idle02", 1), CB_OK);
    nm.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 0);
    T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "idle02", NOW, 25000, tok), CB_E_NOT_FOUND);
    // The game itself is untouched.
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "idle02", &got), CB_OK);
    T_EQ_INT(got.version, 1);
    cb_game_row_free(&got);
    // The next commit that leaves a bot able to act puts it back.
    n.version = 2;
    c.expected_version = 1;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    nm.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &nm), 1);
    cb_store_close(s);
}

// ---- the heartbeat scan --------------------------------------------------------------

static void games_needing_bots_windows(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    // now = 100000: stale_before = now - 10 s, active_after = now - 1 h.
    const int64_t NOW = 10000000, STALE = NOW - 10000, ACTIVE = NOW - 3600000;
    struct { const char *id; bool needs; int64_t updated, last_commit; } rows[] = {
        { "due001", true, NOW - 20000, NOW - 20000 },     // stalled: due
        { "due002", true, NOW - 3000000, NOW - 3000000 }, // stalled 50 min: due
        { "fresh1", true, NOW - 5000, NOW - 5000 },       // touched 5 s ago: a drive is on it
        { "edge01", true, STALE, STALE },                 // exactly 10 s: not yet (strict)
        { "aband1", true, NOW - 4000000, NOW - 4000000 }, // no move in over an hour: abandoned
        { "edge02", true, ACTIVE, ACTIVE },               // exactly an hour: abandoned (strict)
        { "nobot1", false, NOW - 20000, NOW - 20000 },    // nothing for bots to do
        // The lease kept updated_at fresh although the last move is old: not stale.
        { "lease1", true, NOW - 1000, NOW - 30000 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        cb_game_row g = mkgame(rows[i].id, rows[i].last_commit);
        g.needs_bots = rows[i].needs;
        // SQL keeps no lease: lease1's row was touched a second ago by a commit that left the last move old.
        if (!tsb_dynamo() && strcmp(rows[i].id, "lease1") == 0) g.updated_at = NOW - 1000;
        cb_uuid none[1];
        T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, none, 0, none, 0), CB_OK);
    }
    // lease1: its last commit is old, but a drive renewed the lease a second ago.
    cb_uuid tok;
    if (tsb_dynamo()) T_EQ_INT(CB_ROWS_CALL(s, lease_acquire, "lease1", NOW - 1000, 25000, tok), CB_OK);
    names_t n = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 100, collect_name, &n), 2);
    T_EQ_STR(n.ids[0], "due002");   // oldest first
    T_EQ_STR(n.ids[1], "due001");
    n.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, games_needing_bots, STALE, ACTIVE, 1, collect_name, &n), 1);
    cb_store_close(s);
}

// ---- ratings ---------------------------------------------------------------------------

static bool has_attr(cb_store *s, const char *pk, const char *sk, const char *name);

// rating_reindex on both backends (the item layer made it real on SQLite in
// stage 6; written to hold the coverage floor, docs/PLATFORM.md section 9):
// a row in the leaderboard index with no games leaves it, a played row at an
// old key gets the current one, and a second run finds nothing to do.
static void ratings_reindexed(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    char pa[64], pb[64];
    snprintf(pa, sizeof pa, "USER#%s", a.id);
    snprintf(pb, sizeof pb, "USER#%s", b.id);
    // a: no games, yet listed (an old layout listed every row); b: played, at an old key.
    const cb_av ra[] = { CB_AVS("gsi2pk", "RATINGS"), CB_AVS("gsi2sk", "old"), CB_AVS("user_id", a.id),
                         CB_AVN("elo_rating", 1000), CB_AVN("games_played", 0) };
    const cb_av rb[] = { CB_AVS("gsi2pk", "RATINGS"), CB_AVS("gsi2sk", "old"), CB_AVS("user_id", b.id),
                         CB_AVN("elo_rating", 1100), CB_AVN("games_played", 3) };
    cb_iwrite wa = { .op = CB_IW_PUT, .key = { pa, "RATING" }, .attrs = ra, .n_attrs = 5 };
    cb_iwrite wb = { .op = CB_IW_PUT, .key = { pb, "RATING" }, .attrs = rb, .n_attrs = 5 };
    T_EQ_INT(s->ops->item_write(s, &wa, NULL), CB_OK);
    T_EQ_INT(s->ops->item_write(s, &wb, NULL), CB_OK);
    cb_rating_row ta = { .elo_rating = 1000, .games_played = 0 }, tb = { .elo_rating = 1100, .games_played = 3 };
    memcpy(ta.user_id, a.id, sizeof ta.user_id);
    memcpy(tb.user_id, b.id, sizeof tb.user_id);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &ta), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &tb), CB_OK);
    T_CHECK(!has_attr(s, pa, "RATING", "gsi2pk"));
    cb_item it;
    cb_item_init(&it);
    char sk[96] = "";
    T_EQ_INT(s->ops->item_get(s, (cb_ikey){ pb, "RATING" }, true, &it), CB_OK);
    T_CHECK(cb_item_s(&it, "gsi2sk", sk, sizeof sk));
    T_CHECK(strncmp(sk, "E#", 2) == 0 && strstr(sk, b.id) != NULL);
    cb_item_free(&it);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &ta), CB_E_CONFLICT);
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &tb), CB_E_CONFLICT);
    // A row a commit moved meanwhile (another elo) is left to the commit.
    tb.elo_rating = 1200;
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &tb), CB_E_CONFLICT);
    cb_store_close(s);
}

static void ratings_applied_by_commit(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_import, &b);   // an account with no rating row yet
    T_EQ_INT(rating_of(s, b.id), TABLE_RATING_NONE);
    cb_game_row g = mkgame("rate01", 10);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    const char *bot = "176d94a4-71f2-4542-9510-5603e52058d7", *gone = "00000000-0000-4000-8000-0000000000aa";
    cb_game_row n = mkgame("rate01", 10);
    n.version = 1;
    n.updated_at = 77;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 2, .n_rated = 4 };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    memcpy(c.humans[1], b.id, sizeof(cb_uuid));
    memcpy(c.rated_ids[0], a.id, sizeof(cb_uuid));
    c.rating_delta[0] = -5000;   // clamped at 0
    memcpy(c.rated_ids[1], b.id, sizeof(cb_uuid));
    c.rating_delta[1] = 12;      // a new row from the start rating
    snprintf(c.rated_names[1], sizeof c.rated_names[1], "Bee");
    snprintf(c.rated_ids[2], sizeof(cb_uuid), "%s", bot);
    c.rated_is_bot[2] = true;
    c.rating_delta[2] = -8;
    snprintf(c.rated_ids[3], sizeof(cb_uuid), "%s", gone);   // a deleted account: skipped
    c.rating_delta[3] = 3;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);

    T_EQ_INT(rating_of(s, a.id), 0);
    T_EQ_INT(rating_of(s, b.id), 1012);
    T_EQ_INT(rating_of(s, bot), 992);
    T_EQ_INT(rating_of(s, gone), TABLE_RATING_NONE);
    ratings_t r = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, ratings_list, collect_rating, &r), 2);
    T_EQ_STR(r.rows[0].user_id, b.id);   // highest first
    T_EQ_STR(r.rows[0].username, "Bee");
    T_EQ_INT(r.rows[0].previous_elo, 1000);
    T_EQ_INT(r.rows[0].games_played, 1);
    T_EQ_INT(r.rows[0].updated_at, 77);
    T_EQ_INT(r.rows[1].elo_rating, 0);
    T_EQ_INT(r.rows[1].previous_elo, 1000);
    bots_t bl = { 0 };
    CB_ROWS_CALL(s, bots_list, collect_bot, &bl);
    T_EQ_STR(bl.rows[0].id, bot);
    T_EQ_INT(bl.rows[0].previous_elo, 1000);
    T_EQ_INT(bl.rows[0].games_played, 1);

    // A second rated commit adds to the row as it stands now.
    n.version = 2;
    c.expected_version = 1;
    c.n_rated = 1;
    memcpy(c.rated_ids[0], b.id, sizeof(cb_uuid));
    c.rating_delta[0] = -2;
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    T_EQ_INT(rating_of(s, b.id), 1010);
    r.n = 0;
    CB_ROWS_CALL(s, ratings_list, collect_rating, &r);
    T_EQ_INT(r.rows[0].previous_elo, 1012);
    T_EQ_INT(r.rows[0].games_played, 2);
    // The leaderboard's read: the played rows, highest first, at most `limit`.
    r.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 100, collect_rating, &r), 2);
    T_EQ_STR(r.rows[0].user_id, b.id);
    T_EQ_INT(r.rows[0].elo_rating, 1010);
    T_EQ_STR(r.rows[1].user_id, a.id);
    T_EQ_INT(r.rows[1].elo_rating, 0);
    r.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 1, collect_rating, &r), 1);
    T_EQ_STR(r.rows[0].user_id, b.id);
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 0, collect_rating, &r), 0);
    // A third player who signed up and never played is not on it.
    cb_user_row z = mkuser("zed@x.io", "Zed", 9);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &z), CB_OK);
    r.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, ratings_top, 100, collect_rating, &r), 2);
    // A row the commit already keyed is not rewritten, and its values stay.
    cb_rating_row same = r.rows[0];
    T_EQ_INT(CB_ROWS_CALL(s, rating_reindex, &same), CB_E_CONFLICT);
    T_EQ_INT(rating_of(s, b.id), 1010);
    cb_store_close(s);
}

// ---- snapshots, chat, connections --------------------------------------------------------

static void snapshots_query_shapes(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1), b = mkuser("b@x.io", "B", 1);
    CB_STORE_CALL(s, user_create, &a);
    CB_STORE_CALL(s, user_create, &b);
    cb_game_row g = mkgame("snap01", 10);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    uint8_t mv[3] = { 7, 8, 9 }, ex[2] = { 1, 1 };
    const char *ids[3] = { "00000000-0000-4000-8000-000000000001", "00000000-0000-4000-8000-000000000002",
                           "00000000-0000-4000-8000-000000000003" };
    for (int i = 0; i < 3; i++) {
        cb_snapshot_row sn = { .n_players = i == 2 ? 1 : 2, .moves = mv, .moves_len = 3, .created_at = 100 + i };
        snprintf(sn.id, sizeof sn.id, "%s", ids[i]);
        if (i == 0) {
            snprintf(sn.game_id, sizeof sn.game_id, "snap01");
            sn.extras = ex;
            sn.extras_len = 2;
        }
        memcpy(sn.player_ids[0], i == 2 ? b.id : a.id, sizeof(cb_uuid));
        memcpy(sn.player_ids[1], b.id, sizeof(cb_uuid));
        T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);
    }
    names_t n = { 0 };
    cb_snapshot_query q = { .viewer_id = a.id, .newest_first = true };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), 2);
    T_EQ_STR(n.ids[0], ids[1]);
    T_EQ_STR(n.ids[1], ids[0]);
    n.n = 0;
    q.viewer_id = b.id;
    q.newest_first = false;
    q.limit = 2;
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), 2);
    T_EQ_STR(n.ids[0], ids[0]);
    n.n = 0;
    q.limit = 0;
    q.game_id = "snap01";
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), 1);
    n.n = 0;
    q.game_id = NULL;
    q.id = ids[2];
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), 1);
    // Not a player of it: not visible.
    n.n = 0;
    q.viewer_id = a.id;
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), 0);
    q.viewer_id = NULL;
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, collect_snap, &n), CB_E_INVALID);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &(cb_snapshot_row){ .id = "00000000-0000-4000-8000-000000000001",
                                                                .moves = mv, .moves_len = 3 }),
             CB_E_CONFLICT);
    cb_store_close(s);
}

static void snapshot_round_trip(void) {
    cb_store *s = open_fresh();
    uint8_t mv[4] = { 1, 2, 3, 4 }, ex[3] = { 5, 6, 7 };
    cb_snapshot_row sn = { .n_players = 3, .moves = mv, .moves_len = 4, .extras = ex, .extras_len = 3, .created_at = 42 };
    cb_uuid_v4(sn.id);
    for (int i = 0; i < 3; i++) cb_uuid_v4(sn.player_ids[i]);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);
    snapcopy c = { 0 };
    cb_snapshot_query q = { .viewer_id = sn.player_ids[2] };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_STR(c.row.id, sn.id);
    T_EQ_STR(c.row.game_id, "");
    T_EQ_INT(c.row.n_players, 3);
    for (int i = 0; i < 3; i++) T_EQ_STR(c.row.player_ids[i], sn.player_ids[i]);
    T_EQ_MEM(c.row.moves, c.row.moves_len, mv, 4);
    T_EQ_MEM(c.row.extras, c.row.extras_len, ex, 3);
    T_EQ_INT(c.row.created_at, 42);
    cb_snapshot_row_free(&c.row);
    cb_store_close(s);
}

// Account deletion's rewrite: the extras change and nothing else does; an
// unknown replay is NOT_FOUND; zero bytes clear them.
static void snapshot_set_extras_rewrites_only_extras(void) {
    cb_store *s = open_fresh();
    uint8_t mv[4] = { 1, 2, 3, 4 }, ex[3] = { 5, 6, 7 }, ex2[5] = { 9, 8, 7, 6, 5 };
    cb_snapshot_row sn = { .n_players = 2, .moves = mv, .moves_len = 4, .extras = ex, .extras_len = 3, .created_at = 42 };
    cb_snapshot_row other = sn;
    cb_uuid_v4(sn.id);
    cb_uuid_v4(other.id);
    for (int i = 0; i < 2; i++) cb_uuid_v4(sn.player_ids[i]);
    memcpy(other.player_ids, sn.player_ids, sizeof sn.player_ids);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &other), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, sn.id, ex2, sizeof ex2), CB_OK);
    snapcopy c = { 0 };
    cb_snapshot_query q = { .viewer_id = sn.player_ids[1], .id = sn.id };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_MEM(c.row.extras, c.row.extras_len, ex2, 5);
    T_EQ_MEM(c.row.moves, c.row.moves_len, mv, 4);
    T_EQ_INT(c.row.created_at, 42);
    T_EQ_STR(c.row.player_ids[0], sn.player_ids[0]);
    cb_snapshot_row_free(&c.row);
    snapcopy o = { 0 };
    cb_snapshot_query qo = { .viewer_id = other.player_ids[0], .id = other.id };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &qo, copy_snap, &o), 1);
    T_EQ_MEM(o.row.extras, o.row.extras_len, ex, 3);   // another replay is untouched
    cb_snapshot_row_free(&o.row);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, "00000000-0000-4000-8000-00000000dead", ex2, 5), CB_E_NOT_FOUND);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, sn.id, NULL, 0), CB_OK);
    memset(&c, 0, sizeof c);
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_INT(c.row.extras_len, 0);
    cb_snapshot_row_free(&c.row);
    cb_store_close(s);
}

// DATA SAFETY (account deletion's promise: no stored replay names a deleted
// player). A player's copy of a replay is a pointer that holds no moves,
// extras or players; one written before the pointers holds the whole replay,
// and redacting the replay slims it, in the same write.
static bool has_attr(cb_store *s, const char *pk, const char *sk, const char *name) {
    cb_item it;
    cb_item_init(&it);
    cb_av v;
    bool has = s->ops->item_get(s, (cb_ikey){ pk, sk }, true, &it) == CB_OK && cb_item_get(&it, name, &v);
    cb_item_free(&it);
    return has;
}

static void replay_copies_hold_no_names(void) {
    cb_store *s = open_fresh();
    uint8_t mv[4] = { 1, 2, 3, 4 }, ex[3] = { 5, 6, 7 }, ex2[2] = { 9, 9 };
    cb_snapshot_row sn = { .n_players = 2, .moves = mv, .moves_len = 4, .extras = ex, .extras_len = 3, .created_at = 42 };
    cb_uuid_v4(sn.id);
    snprintf(sn.game_id, sizeof sn.game_id, "nm0001");
    for (int i = 0; i < 2; i++) cb_uuid_v4(sn.player_ids[i]);
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);
    char pk[64], sk0[64], sk1[64];
    snprintf(pk, sizeof pk, "SNAP#%s", sn.id);
    snprintf(sk0, sizeof sk0, "PLAYER#%s", sn.player_ids[0]);
    snprintf(sk1, sizeof sk1, "PLAYER#%s", sn.player_ids[1]);
    T_CHECK(has_attr(s, pk, "SNAP", "extras"));
    T_CHECK(has_attr(s, pk, "SNAP", "moves"));
    static const char *const NOT_ON_A_POINTER[] = { "extras", "moves", "player_ids", "gsi1pk" };
    for (int i = 0; i < 4; i++) {
        T_CHECK(!has_attr(s, pk, sk0, NOT_ON_A_POINTER[i]));
        T_CHECK(!has_attr(s, pk, sk1, NOT_ON_A_POINTER[i]));
    }
    T_CHECK(has_attr(s, pk, sk0, "game_id"));

    // A copy as the store wrote it before the pointers: the whole replay.
    char g2[64], g2s[64], g1[64];
    snprintf(g2, sizeof g2, "PLAYER#%s", sn.player_ids[1]);
    snprintf(g2s, sizeof g2s, "000000000000042#%s", sn.id);
    snprintf(g1, sizeof g1, "GAMESNAPS#%s", sn.game_id);
    const char *players[2] = { sn.player_ids[0], sn.player_ids[1] };
    const cb_av fat[] = { CB_AVS("gsi1pk", g1), CB_AVS("gsi1sk", g2s), CB_AVS("gsi2pk", g2), CB_AVS("gsi2sk", g2s),
                          CB_AVS("id", sn.id), CB_AVS("game_id", sn.game_id), CB_AVSL("player_ids", players, 2),
                          CB_AVB("moves", mv, 4), CB_AVB("extras", ex, 3), CB_AVN("created_at", 42) };
    cb_iwrite w = { .op = CB_IW_PUT, .key = { pk, sk1 }, .attrs = fat, .n_attrs = 10 };
    T_EQ_INT(s->ops->item_write(s, &w, NULL), CB_OK);
    T_CHECK(has_attr(s, pk, sk1, "extras"));
    // Read compatibly: the viewer still gets the replay, from the replay itself.
    snapcopy c = { 0 };
    cb_snapshot_query q = { .viewer_id = sn.player_ids[1], .id = sn.id };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_MEM(c.row.extras, c.row.extras_len, ex, 3);
    cb_snapshot_row_free(&c.row);
    // Redacted: the replay's extras are the new ones, and the fat copy is a pointer.
    T_EQ_INT(CB_ROWS_CALL(s, snapshot_set_extras, sn.id, ex2, sizeof ex2), CB_OK);
    for (int i = 0; i < 4; i++) T_CHECK(!has_attr(s, pk, sk1, NOT_ON_A_POINTER[i]));
    T_CHECK(has_attr(s, pk, sk1, "gsi2pk"));
    memset(&c, 0, sizeof c);
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, copy_snap, &c), 1);
    T_EQ_MEM(c.row.extras, c.row.extras_len, ex2, 2);
    T_EQ_MEM(c.row.moves, c.row.moves_len, mv, 4);
    cb_snapshot_row_free(&c.row);
    cb_store_close(s);
}

static void chat_newest_oldest_first(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("chat01", 10);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    const char *msgs[4] = { "one", "two", "three", "four" };
    for (int i = 0; i < 4; i++) {
        cb_chat_row c = { .message = (char *)msgs[i], .created_at = 100 + i, .is_system = i == 0 };
        cb_uuid_v4(c.id);
        snprintf(c.game_id, sizeof c.game_id, "chat01");
        memcpy(c.user_id, a.id, sizeof(cb_uuid));
        T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_OK);
    }
    names_t n = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "chat01", 2, collect_chat, &n), 2);
    T_EQ_STR(n.ids[0], "three");
    T_EQ_STR(n.ids[1], "four");
    n.n = 0;
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "chat01", 0, collect_chat, &n), 4);
    T_EQ_STR(n.ids[0], "one");
    // A message for a game that does not exist is refused.
    cb_chat_row bad = { .message = "x", .created_at = 1 };
    cb_uuid_v4(bad.id);
    snprintf(bad.game_id, sizeof bad.game_id, "nogame");
    memcpy(bad.user_id, a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, chat_put, &bad), CB_E_NOT_FOUND);
    cb_store_close(s);
}

// ---- admin -----------------------------------------------------------------------------

typedef struct { int rows; char first[8][64]; char col0[32]; int ncols; } rows_t;
static bool collect_row(int n, const char *const *names, const char *const *values, void *ud) {
    rows_t *r = ud;
    if (r->rows < 8) snprintf(r->first[r->rows], 64, "%s", values[0] ? values[0] : "(null)");
    snprintf(r->col0, sizeof r->col0, "%s", names[0]);
    r->ncols = n;
    r->rows++;
    return true;
}

typedef struct { int rows; char id[64]; } id_rows;
static bool collect_id(int n, const char *const *names, const char *const *values, void *ud) {
    id_rows *r = ud;
    r->rows++;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], "id") == 0 && values[i] && !r->id[0]) snprintf(r->id, sizeof r->id, "%s", values[i]);
    return true;
}

typedef struct { char state[64]; } state_rows;
static bool collect_state(int n, const char *const *names, const char *const *values, void *ud) {
    state_rows *r = ud;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], "state") == 0 && values[i] && !r->state[0]) snprintf(r->state, sizeof r->state, "%s", values[i]);
    return true;
}

static void admin_reads(void) {
    cb_store *s = open_fresh();
    // The collections are the tables, by name.
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, admin_collections, collect_name, &n), 14);
    T_EQ_STR(n.ids[0], "bots");
    T_EQ_STR(n.ids[4], "games");
    T_EQ_STR(n.ids[5], "identities");
    T_EQ_STR(n.ids[13], "users");

    // Pages of 20 over the 56 bots: 20, 20, 16, and the cursor runs out.
    rows_t r = { 0 };
    char cur[64] = "", next[64];
    int pages = 0, total = 0;
    do {
        int k = CB_STORE_CALL(s, admin_browse, "bots", NULL, cur, 20, collect_row, &r, next, sizeof next);
        T_CHECK(k >= 0);
        total += k;
        pages++;
        snprintf(cur, sizeof cur, "%s", next);
    } while (cur[0] && pages < 10);
    T_EQ_INT(total, 56);
    T_EQ_INT(pages, 3);
    // The cursor is the row offset a page ends at, nothing else: strtoll read
    // "3x", " 3" and "+3" as 3, and "x" as the first page.
    static const char *const bad_cursors[] = { "3x", " 3", "+3", "x", "3 ", "-5", "99999999999999999999" };
    for (size_t i = 0; i < sizeof bad_cursors / sizeof bad_cursors[0]; i++)
        T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", NULL, bad_cursors[i], 20, collect_row, &r, next, sizeof next),
                 CB_E_INVALID);
    // A prefix of the first column: a UUID column matches its text (its 16 bytes shown as the UUID).
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", "176d94a4", "", 100, collect_row, &r, next, sizeof next), 1);
    T_EQ_STR(next, "");
    T_EQ_STR(r.col0, "id");
    T_EQ_STR(r.first[0], "176d94a4-71f2-4542-9510-5603e52058d7");
    // "column=value": an equality, a UUID by its text.
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", "id=15b54f52-2240-4955-8055-e27b7d767e3a", "", 100, collect_row, &r, next,
                           sizeof next),
             1);
    T_EQ_STR(r.first[0], "15b54f52-2240-4955-8055-e27b7d767e3a");
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots", "nope=1", "", 10, collect_row, &r, next, sizeof next), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots; DROP TABLE users", NULL, "", 10, collect_row, &r, next, sizeof next),
             CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "BOT", NULL, "", 10, collect_row, &r, next, sizeof next), CB_E_INVALID);

    // Query: SQL over the tables; reads always, writes only when allowed,
    // one statement only.
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT count(*) AS n FROM bots", false, collect_row, &r), 1);
    T_EQ_STR(r.first[0], "56");
    T_EQ_STR(r.col0, "n");
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "DELETE FROM bots", false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "  -- c\n DELETE FROM bots", false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "ATTACH DATABASE ':memory:' AS x", false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT 1; DELETE FROM bots", true, collect_row, &r), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELECT 1;", false, collect_row, &r), 1);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELEKT", false, collect_row, &r), CB_E_INVALID);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "DELETE FROM bots WHERE id = x'176d94a471f2454295105603e52058d7'", true,
                           collect_row, &r),
             0);
    bots_t b = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &b), 55);
    // A blob reads as "\x" hex.
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("adm001", 1);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    r.rows = 0;
    CB_STORE_CALL(s, admin_query, "SELECT state FROM games WHERE id = 'adm001'", false, collect_row, &r);
    T_CHECK(strncmp(r.first[0], "\\x", 2) == 0);
    // CB_ADMIN_GAMES: the games table, whose rows carry the id (the console's
    // Games panel), and its state reads as "\x" hex.
    id_rows ir = { 0 };
    T_CHECK(CB_STORE_CALL(s, admin_browse, CB_ADMIN_GAMES, NULL, "", 100, collect_id, &ir, next, sizeof next) >= 1);
    T_EQ_STR(ir.id, "adm001");
    state_rows sr = { 0 };
    T_CHECK(CB_STORE_CALL(s, admin_browse, CB_ADMIN_GAMES, NULL, "", 100, collect_state, &sr, next, sizeof next) >= 1);
    T_EQ_STR(sr.state, "\\x010203");
    // The console's backup: VACUUM INTO is a write, so only when allowed.
    char vac[512], stmt[600];
    const char *tmp = getenv("TMPDIR");
    snprintf(vac, sizeof vac, "%s/cbase-vacuum-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(vac);
    snprintf(stmt, sizeof stmt, "VACUUM INTO '%s'", vac);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, stmt, false, NULL, NULL), CB_E_FORBIDDEN);
    T_CHECK(access(vac, F_OK) != 0);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, stmt, true, NULL, NULL), 0);
    cb_store *copy = NULL;
    T_EQ_INT(cb_store_open_sqlite(vac, &copy), CB_OK);
    if (copy) {
        cb_game_row back;
        T_EQ_INT(CB_ROWS_CALL(copy, game_get, "adm001", &back), CB_OK);
        cb_game_row_free(&back);
        cb_store_close(copy);
    }
    unlink(vac);
    cb_store_close(s);
}

// The console's reads on DynamoDB: key families as collections, a Scan per
// page, PartiQL for the query box.
static void admin_reads_dynamo(void) {
    cb_store *s = open_fresh();
    names_t n = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, admin_collections, collect_name, &n), 8);
    T_EQ_STR(n.ids[3], "GAME");
    // Pages of 20 over the 56 bots (the only items in a fresh table): 20, 20, 16.
    rows_t r = { 0 };
    char cur[1024] = "", next[1024];
    int pages = 0, total = 0;
    do {
        int k = CB_STORE_CALL(s, admin_browse, "BOT", NULL, cur, 20, collect_row, &r, next, sizeof next);
        T_CHECK(k >= 0);
        total += k > 0 ? k : 0;
        pages++;
        snprintf(cur, sizeof cur, "%s", next);
    } while (cur[0] && pages < 10);
    T_EQ_INT(total, 56);
    T_CHECK(pages >= 3);
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "BOT", "176d94a4", "", 100, collect_row, &r, next, sizeof next), 1);
    T_EQ_INT(CB_STORE_CALL(s, admin_browse, "bots; DROP", NULL, "", 10, collect_row, &r, next, sizeof next), CB_E_INVALID);

    // PartiQL: reads always, writes only when allowed.
    char q[512];
    snprintf(q, sizeof q, "SELECT nickname FROM \"%s\" WHERE pk = 'BOT#176d94a4-71f2-4542-9510-5603e52058d7' AND sk = 'BOT'",
             tsb_table());
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_query, q, false, collect_row, &r), 1);
    T_EQ_STR(r.first[0], "%Miami 1");
    snprintf(q, sizeof q, "DELETE FROM \"%s\" WHERE pk = 'BOT#176d94a4-71f2-4542-9510-5603e52058d7' AND sk = 'BOT'",
             tsb_table());
    T_EQ_INT(CB_STORE_CALL(s, admin_query, q, false, collect_row, &r), CB_E_FORBIDDEN);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, q, true, collect_row, &r), 0);
    bots_t b = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &b), 55);
    T_EQ_INT(CB_STORE_CALL(s, admin_query, "SELEKT", true, collect_row, &r), CB_E_INVALID);

    // A Binary attribute reads as "\x" hex.
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("adm001", 1);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    snprintf(q, sizeof q, "SELECT state FROM \"%s\" WHERE pk = 'GAME#adm001' AND sk = 'ROW'", tsb_table());
    r.rows = 0;
    T_EQ_INT(CB_STORE_CALL(s, admin_query, q, false, collect_row, &r), 1);
    T_EQ_STR(r.first[0], "\\x010203");
    // CB_ADMIN_GAMES: the GAME family; only the ROW item carries the id.
    id_rows ir = { 0 };
    T_CHECK(CB_STORE_CALL(s, admin_browse, CB_ADMIN_GAMES, NULL, "", 100, collect_id, &ir, next, sizeof next) >= 1);
    T_EQ_STR(ir.id, "adm001");
    cb_store_close(s);
}

// ---- import writes -------------------------------------------------------------------------

static void import_writes(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    a.last_sign_in_at = 99;
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(rating_of(s, a.id), TABLE_RATING_NONE);   // no side effects
    snprintf(a.username, sizeof a.username, "A2");
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);   // replace
    cb_user_row got;
    CB_STORE_CALL(s, user_get, a.id, &got);
    T_EQ_STR(got.username, "A2");
    T_EQ_INT(got.last_sign_in_at, 99);
    cb_rating_row r = { .elo_rating = 1234, .previous_elo = 1200, .games_played = 9, .updated_at = 5 };
    memcpy(r.user_id, a.id, sizeof(cb_uuid));
    snprintf(r.username, sizeof r.username, "A2");
    T_EQ_INT(CB_ROWS_CALL(s, rating_import, &r), CB_OK);
    T_EQ_INT(rating_of(s, a.id), 1234);

    cb_bot_row bots[2] = { { .id = "11111111-1111-4111-8111-111111111111", .nickname = "%Seoul 1",
                             .strategy_key = "robusta", .elo_rating = 1100, .previous_elo = 1090, .games_played = 3 },
                           { .id = "22222222-2222-4222-8222-222222222222", .nickname = "%Miami 1",
                             .strategy_key = "random", .elo_rating = 900, .previous_elo = -1 } };
    T_EQ_INT(CB_ROWS_CALL(s, bots_replace, bots, 2), CB_OK);
    bots_t bl = { 0 };
    T_EQ_INT(CB_ROWS_CALL(s, bots_list, collect_bot, &bl), 2);
    T_EQ_STR(bl.rows[0].nickname, "%Miami 1");   // ladder order, not insertion order
    T_EQ_INT(bl.rows[0].previous_elo, -1);
    T_EQ_INT(bl.rows[1].previous_elo, 1090);

    cb_game_row g = mkgame("imp001", 50);
    g.version = 37;
    g.round_epoch = 30;
    g.updated_at = 60;
    g.last_commit_at = 55;
    cb_uuid humans[1], botids[1];
    memcpy(humans[0], a.id, sizeof(cb_uuid));
    snprintf(botids[0], sizeof botids[0], "%s", bots[0].id);
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, 1, botids, 1), CB_OK);
    cb_game_row gg;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "imp001", &gg), CB_OK);
    T_EQ_INT(gg.version, 37);
    T_EQ_INT(gg.round_epoch, 30);
    T_EQ_INT(gg.last_commit_at, 55);
    T_EQ_INT(gg.updated_at, 60);
    cb_game_row_free(&gg);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "imp001", a.id), 1);
    // Imported again with no members: the old seats go.
    T_EQ_INT(CB_ROWS_CALL(s, game_import, &g, humans, 0, botids, 0), CB_OK);
    T_EQ_INT(CB_ROWS_CALL(s, is_member, "imp001", a.id), 0);
    cb_store_close(s);
}

// cbase-import over an account that already exists never lowers its session generation (that would revive the
// sessions a global sign-out ended): the stored one is kept when it is at least the imported one, a higher
// imported one wins, and a new account takes the imported one.
static void import_never_lowers_a_session_generation(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("gen@x.io", "GEN", 1);
    cb_user_row got;
    // A new account takes what is imported (a generation and the session it kept).
    a.session_gen = 3;
    snprintf(a.kept_session, sizeof a.kept_session, "%s", "11111111-1111-4111-8111-111111111111");
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_INT(got.session_gen, 3);
    T_EQ_STR(got.kept_session, "11111111-1111-4111-8111-111111111111");
    // Sessions ended since (a global sign-out: one up, none kept), then the same export is imported again
    // (its generation 3 is older than the stored 4): the stored generation and its empty kept session stay.
    T_EQ_INT(CB_STORE_CALL(s, user_end_sessions, a.id), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_INT(got.session_gen, 4);
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_INT(got.session_gen, 4);
    T_EQ_STR(got.kept_session, "");
    // An equal generation keeps the stored one too (and its kept session).
    a.session_gen = 4;
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_INT(got.session_gen, 4);
    T_EQ_STR(got.kept_session, "");
    // An export taken after more sign-outs raises it, and its kept session comes with it.
    a.session_gen = 9;
    snprintf(a.kept_session, sizeof a.kept_session, "%s", "22222222-2222-4222-8222-222222222222");
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_INT(got.session_gen, 9);
    T_EQ_STR(got.kept_session, "22222222-2222-4222-8222-222222222222");
    // The profile was replaced otherwise: the imported name stands.
    snprintf(a.username, sizeof a.username, "GEN2");
    a.session_gen = 0;
    T_EQ_INT(CB_STORE_CALL(s, user_import, &a), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, user_get, a.id, &got), CB_OK);
    T_EQ_STR(got.username, "GEN2");
    T_EQ_INT(got.session_gen, 9);
    cb_store_close(s);
}

// ---- big items and many pages ----------------------------------------------------------------

typedef struct { int n; int64_t last_at; bool ordered; size_t bytes; } pages_t;
static bool count_snap_pages(const cb_snapshot_row *r, void *ud) {
    pages_t *p = ud;
    if (p->n && r->created_at <= p->last_at) p->ordered = false;
    p->last_at = r->created_at;
    p->bytes += r->moves_len;
    p->n++;
    return true;
}
static bool count_chat_pages(const cb_chat_row *r, void *ud) {
    pages_t *p = ud;
    if (p->n && r->created_at <= p->last_at) p->ordered = false;
    p->last_at = r->created_at;
    p->bytes += strlen(r->message);
    p->n++;
    return true;
}

// Sizes DynamoDB answers in pages (a Query stops at 1 MB) and an item near its
// 400 KB cap: a 250 KB game row round-trips byte for byte, 24 replays of 64 KB
// (1.5 MB through GSI2) all come back in order, and so do 300 chat messages of
// 4 KB (1.2 MB), whole and as the newest 250.
static void big_items_and_pages(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_game_row g = mkgame("big001", 10);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_OK);
    size_t sn = 150000, ln = 100000;
    uint8_t *state = malloc(sn), *logs = malloc(ln);
    for (size_t i = 0; i < sn; i++) state[i] = (uint8_t)(i * 131 + (i >> 8));
    for (size_t i = 0; i < ln; i++) logs[i] = (uint8_t)(255 - i % 251);
    cb_game_row n = mkgame("big001", 10);
    n.state = state, n.state_len = sn, n.list_from = 1, n.version = 1, n.updated_at = 20;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 1, .list_add = logs, .list_add_len = ln, .list_clear = true };
    memcpy(c.humans[0], a.id, sizeof(cb_uuid));
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "big001", &got), CB_OK);
    T_EQ_MEM(got.state, got.state_len, state, sn);
    CB_ROWS_CALL(s, game_list, "big001", got.list_from, &got.list, &got.list_len);
    T_EQ_MEM(got.list, got.list_len, logs, ln);
    cb_game_row_free(&got);

    enum { NSNAP = 24, SNAPLEN = 64000 };
    uint8_t *mv = malloc(SNAPLEN);
    for (int i = 0; i < SNAPLEN; i++) mv[i] = (uint8_t)(i * 7);
    for (int i = 0; i < NSNAP; i++) {
        cb_snapshot_row r = { .n_players = 2, .moves = mv, .moves_len = SNAPLEN, .created_at = 1000 + i };
        cb_uuid_v4(r.id);
        memcpy(r.player_ids[0], a.id, sizeof(cb_uuid));
        cb_uuid_v4(r.player_ids[1]);
        T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &r), CB_OK);
    }
    pages_t p = { .ordered = true };
    cb_snapshot_query q = { .viewer_id = a.id };
    T_EQ_INT(CB_ROWS_CALL(s, snapshots_query, &q, count_snap_pages, &p), NSNAP);
    T_EQ_INT(p.n, NSNAP);
    T_CHECK(p.ordered);
    T_EQ_INT((long long)p.bytes, (long long)NSNAP * SNAPLEN);

    enum { NCHAT = 300, CHATLEN = 4000 };
    char *msg = malloc(CHATLEN + 1);
    memset(msg, 'm', CHATLEN);
    msg[CHATLEN] = 0;
    for (int i = 0; i < NCHAT; i++) {
        cb_chat_row m = { .message = msg, .created_at = 5000 + i };
        cb_uuid_v4(m.id);
        snprintf(m.game_id, sizeof m.game_id, "big001");
        memcpy(m.user_id, a.id, sizeof(cb_uuid));
        T_EQ_INT(CB_ROWS_CALL(s, chat_put, &m), CB_OK);
    }
    p = (pages_t){ .ordered = true };
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "big001", 0, count_chat_pages, &p), NCHAT);
    T_CHECK(p.ordered);
    T_EQ_INT(p.last_at, 5000 + NCHAT - 1);
    p = (pages_t){ .ordered = true };
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "big001", 250, count_chat_pages, &p), 250);
    T_CHECK(p.ordered);
    T_EQ_INT(p.last_at, 5000 + NCHAT - 1);   // the newest 250, oldest of them first
    // A game with that much under it still deletes (its chat in batches of 25).
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, "big001", 1), CB_OK);
    p = (pages_t){ .ordered = true };
    T_EQ_INT(CB_ROWS_CALL(s, chat_list, "big001", 0, count_chat_pages, &p), 0);
    free(msg);
    free(mv);
    free(state);
    free(logs);
    cb_store_close(s);
}

// Failed sign-ins (store.h signin_fails): a count by email, replaced whole,
// cleared, and gone once an hour has passed since its last write. On
// DynamoDB the TTL attribute is in seconds, so the table sweeps it.
static void signin_fails_round_trip(void) {
    cb_store *s = open_fresh();
    const int64_t now = 1760000000123LL;
    cb_signin_fails f = { 0 };
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "ann@x.io", now, &f), CB_E_NOT_FOUND);
    T_EQ_INT(f.n, 0);
    cb_signin_fails w = { .n = 6, .last_at = now - 5, .locked_until = now + 30000 };
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_put, "ann@x.io", &w, now), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "ann@x.io", now, &f), CB_OK);
    T_EQ_INT(f.n, 6);
    T_CHECK(f.last_at == now - 5 && f.locked_until == now + 30000);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "bob@x.io", now, &f), CB_E_NOT_FOUND);
    w.n = 7;
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_put, "ann@x.io", &w, now + 1000), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "ann@x.io", now + 1000, &f), CB_OK);
    T_EQ_INT(f.n, 7);
    // An hour after the last write it reads as none.
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "ann@x.io", now + 1000 + 3600 * 1000LL - 1, &f), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "ann@x.io", now + 1000 + 3600 * 1000LL, &f), CB_E_NOT_FOUND);
    if (tsb_dynamo()) {
        char q[256], v[64];
        snprintf(q, sizeof q, "SELECT expires_at FROM \"%s\" WHERE pk = 'AUTHFAIL#ann@x.io' AND sk = 'FAILS'", tsb_table());
        T_EQ_INT(CB_STORE_CALL(s, admin_query, q, false, first_value, v), 1);
        T_EQ_STR(v, "1760003601");
    }
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_clear, "ann@x.io"), CB_OK);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_get, "ann@x.io", now, &f), CB_E_NOT_FOUND);
    T_EQ_INT(CB_STORE_CALL(s, signin_fails_clear, "ann@x.io"), CB_OK);   // none: still fine
    cb_store_close(s);
}

// ---- concurrency: the CAS under threads ----------------------------------------------------

typedef struct { cb_store *s; int wins, losses; bool lone; cb_uuid user; } racer;

static void *race(void *ud) {
    racer *r = ud;
    for (int k = 0; k < 50; k++) {
        cb_game_row g;
        if (CB_ROWS_CALL(r->s, game_get, "race01", &g) != CB_OK) continue;
        int64_t v = g.version;
        g.version = v + 1;
        g.updated_at = v + 1;
        cb_game_commit c = { .row = &g, .expected_version = v };
        if (r->lone) {   // the seats as the row vouches for them: the lone UpdateItem on DynamoDB
            c.n_humans = c.n_prev_humans = 1;
            memcpy(c.humans[0], r->user, sizeof(cb_uuid));
            memcpy(c.prev_humans[0], r->user, sizeof(cb_uuid));
            c.prev_known = g.seats_exact;
            c.prev_needs_bots = g.needs_bots;
        }
        int rc = CB_ROWS_CALL(r->s, game_commit, &c);
        if (rc == CB_OK) r->wins++;
        else if (rc == CB_E_CONFLICT) r->losses++;
        cb_game_row_free(&g);
    }
    return NULL;
}

// Four threads race read-modify-commit on one game: every win is one version,
// so the final version is exactly the number of wins (no lost update), and no
// commit fails for any reason but the fence. `lone`: every commit keeps the
// creator's seat as the row vouches for it, so on DynamoDB each is the lone
// conditional UpdateItem.
static void race_on(bool lone) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("a@x.io", "A", 1);
    CB_STORE_CALL(s, user_create, &a);
    cb_game_row g = mkgame("race01", 1);
    CB_ROWS_CALL(s, game_create, &g, a.id);
    racer r[4];
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        r[i] = (racer){ .s = s, .lone = lone };
        memcpy(r[i].user, a.id, sizeof(cb_uuid));
        pthread_create(&th[i], NULL, race, &r[i]);
    }
    int wins = 0, total = 0;
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        wins += r[i].wins;
        total += r[i].wins + r[i].losses;
    }
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "race01", &got), CB_OK);
    T_EQ_INT(got.version, wins);
    T_EQ_INT(total, 200);
    T_CHECK(wins >= 50);
    cb_game_row_free(&got);
    if (lone) T_EQ_INT(CB_ROWS_CALL(s, is_member, "race01", a.id), 1);
    cb_store_close(s);
}

static void cas_under_threads(void) { race_on(false); }
static void lone_cas_under_threads(void) { race_on(true); }

// Group commit: threads queue writes at once. Each is (insert my own row, insert a shared row that
// exists at most once); exactly one wins the shared row, the losers answer CB_E_CONFLICT and leave NO
// trace of their own row (their savepoint rolled back), and every winner's own row is there.
typedef struct { cb_store *s; int id; int rc[40]; } gc_arg;
typedef struct { int id, i; } gc_write;

static int gc_put(cb_sql *x, void *ud) {
    gc_write *w = ud;
    char sql[256];
    snprintf(sql, sizeof sql, "INSERT INTO signin_fails VALUES ('gc-%d-%d', 1, 0, 0, 9223372036854775807);"
                              "INSERT INTO signin_fails VALUES ('gcs-%d', 1, 0, 0, 9223372036854775807);", w->id, w->i, w->i);
    return cb_sql_exec(x, sql);
}

static void *gc_run(void *arg) {
    gc_arg *a = arg;
    for (int i = 0; i < 40; i++) {   // forty shared rows, many writers each
        gc_write w = { a->id, i };
        a->rc[i] = cb_sql_write(a->s, gc_put, &w);
    }
    return NULL;
}

static void group_commit_keeps_each_txn_alone(void) {
    cb_store *s = open_fresh();
    enum { T = 8 };
    gc_arg a[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) { a[i].s = s; a[i].id = i; pthread_create(&th[i], NULL, gc_run, &a[i]); }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    for (int k = 0; k < 40; k++) {
        int winners = 0;
        for (int i = 0; i < T; i++) {
            char own[64];
            snprintf(own, sizeof own, "gc-%d-%d", i, k);
            cb_signin_fails f;
            int got = CB_STORE_CALL(s, signin_fails_get, own, 0, &f);
            if (a[i].rc[k] == CB_OK) { winners++; T_EQ_INT(got, CB_OK); }
            else { T_EQ_INT(a[i].rc[k], CB_E_CONFLICT); T_EQ_INT(got, CB_E_NOT_FOUND); }
        }
        T_EQ_INT(winners, 1);
    }
    cb_store_close(s);
}

// cb_item_size reads the lengths the item carries; it must be the sum of
// cb_av_size over the attributes it gives back, for every kind.
static void item_size_is_the_sum_of_its_attributes(void) {
    cb_item it;
    cb_item_init(&it);
    const uint8_t blob[7] = { 1, 2, 3, 4, 5, 6, 7 };
    const char *list[] = { "ab", "", "cde" };
    T_CHECK(cb_item_add(&it, &CB_AVS("name", "value-of-some-length")));
    T_CHECK(cb_item_add(&it, &CB_AVS("empty", "")));
    T_CHECK(cb_item_add(&it, &CB_AVN("count", 12345)));
    T_CHECK(cb_item_add(&it, &CB_AVBOOL("flag", true)));
    T_CHECK(cb_item_add(&it, &CB_AVB("blob", blob, sizeof blob)));
    T_CHECK(cb_item_add(&it, &CB_AVSL("list", list, 3)));
    size_t want = 0;
    for (int i = 0; i < it.n; i++) {
        cb_av v;
        cb_item_at(&it, i, &v);
        want += cb_av_size(&v);
    }
    T_CHECK(want > 0);
    T_EQ_INT(cb_item_size(&it), want);
    cb_item_free(&it);
}

// The backup job uploads only when cb_store_local_changes moved (providers/local/src/store/backup.c).
// Every write path has to move it: the core ops (a user), the product's SQL rows (a game, its commit).
// A write path that skipped the count would leave the database unbacked-up with no error anywhere.
static void every_write_moves_the_change_count(void) {
    cb_store *s = open_fresh();
    unsigned long c0 = cb_store_local_changes(s);
    cb_user_row a = mkuser("count@x.io", "Count", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    unsigned long c1 = cb_store_local_changes(s);
    T_CHECK(c1 > c0);
    cb_game_row g = mkgame("cc11dd", 100);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_OK);
    unsigned long c2 = cb_store_local_changes(s);
    T_CHECK(c2 > c1);
    cb_game_row n = mkgame("cc11dd", 100);
    n.version = 1;
    n.updated_at = 200;
    cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 1 };
    memcpy(c.humans[0], a.id, sizeof c.humans[0]);
    T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    unsigned long c3 = cb_store_local_changes(s);
    T_CHECK(c3 > c2);
    // A read moves nothing.
    cb_game_row got;
    T_EQ_INT(CB_ROWS_CALL(s, game_get, "cc11dd", &got), CB_OK);
    cb_game_row_free(&got);
    T_EQ_INT((long long)cb_store_local_changes(s), (long long)c3);
    cb_store_close(s);
}

// The session's list (games.move_list, move_list.h): a commit appends its chunk, a deal replaces it,
// a clear with no chunk (a lobby) leaves none.
// MUTATION-CHECKED: rows_sql.c list_write skipping a clear with nothing to add turns "a lobby keeps
// no list" red.
static void the_session_list_appends_and_clears(void) {
    cb_store *s = open_fresh();
    cb_user_row a = mkuser("list@x.io", "LIST", 1);
    T_EQ_INT(CB_STORE_CALL(s, user_create, &a), CB_OK);
    cb_game_row g = mkgame("li55st", 100);
    T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, a.id), CB_OK);
    cb_game_row n = mkgame("li55st", 100), got;
    n.list_from = 1;
    const uint8_t one[2] = { 0xaa, 0xbb }, two[1] = { 0xcc }, both[3] = { 0xaa, 0xbb, 0xcc };
    struct { bool clear; const uint8_t *add; size_t n; const uint8_t *want; size_t want_n; } steps[] = {
        { true, one, sizeof one, one, sizeof one },     // a deal: the list is its chunk
        { false, two, sizeof two, both, sizeof both },  // a move: appended
        { false, NULL, 0, both, sizeof both },          // a commit that adds nothing leaves it
        { true, NULL, 0, NULL, 0 },                     // a lobby keeps no list
        { true, two, sizeof two, two, sizeof two },     // the next deal's alone
    };
    for (int i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
        n.version = i + 1;
        cb_game_commit c = { .row = &n, .expected_version = i, .n_humans = 1, .list_add = steps[i].add,
                             .list_add_len = steps[i].n, .list_clear = steps[i].clear };
        memcpy(c.humans[0], a.id, sizeof c.humans[0]);
        T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
        T_EQ_INT(CB_ROWS_CALL(s, game_get, "li55st", &got), CB_OK);
        T_EQ_INT(CB_ROWS_CALL(s, game_list, "li55st", got.list_from, &got.list, &got.list_len), CB_OK);
        if (steps[i].want_n) T_EQ_MEM(got.list, got.list_len, steps[i].want, steps[i].want_n);
        else T_EQ_INT(got.list_len, 0);   // a lobby keeps no list
        cb_game_row_free(&got);
    }
    cb_store_close(s);
}

// ---- schema 9 -> 10: the session logs become move lists ----------------------------------------

#define UP_RS(x) (x), (int)strlen(x)

// A game played through the kernel as the server plays it, one commit per human move or bot cycle,
// up to `max_commits` (0: to its end): its row (version, state, roster, created_at, list_from) and its
// session log as schema 9 stored it, one game_moves row per commit at the version the commit wrote.
typedef struct {
    uint8_t state[TABLE_STATE_MAX], roster[ROSTER_BYTES], log[1 << 16], seed[FOOLISH_SEED_LEN];
    int     state_len, log_len, status, n_rows;
    int64_t version, list_from, created_at, now;
    int64_t seq[2048];
    int     off[2048], len[2048];
} up_game_t;

static void up_play(up_game_t *u, int seed0, int max_commits) {
    static Game g;
    static TableSnaps snaps;
    static uint8_t arena[1 << 18];
    static LegalMoves moves;
    Table t;
    TableCommit c;
    memset(u, 0, sizeof *u);
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) u->seed[i] = (uint8_t)(seed0 * 31 + i * 7 + 1);
    u->created_at = u->now = 1760000000000LL + seed0;
    table_init(&t, &g, &snaps);
    table_create(&t, UP_RS("h"), UP_RS("Human"));
    table_add_bot(&t, UP_RS("h"), UP_RS("b1"), UP_RS("Bot"), UP_RS("random"), u->seed);
    game_set_seed(1);
    table_ready(&t, UP_RS("h"), u->seed);
    for (int n = 0;; n++) {
        u->now += 700 + (n % 5) * 311;
        if (table_commit_products(&t, UP_RS("g"), (uint32_t)u->version + 1, u->now, &c, arena, sizeof arena) < 0) break;
        u->version++;
        memcpy(u->state, arena + c.state.off, (size_t)c.state.len);
        u->state_len = c.state.len;
        memcpy(u->roster, arena + c.roster.off, ROSTER_BYTES);
        u->status = c.status;
        if (c.logs_reset) { u->log_len = 0; u->n_rows = 0; u->list_from = u->version; }
        if (c.logs.len && u->n_rows < 2048 && u->log_len + c.logs.len <= (int)sizeof u->log) {
            u->seq[u->n_rows] = u->version;
            u->off[u->n_rows] = u->log_len;
            u->len[u->n_rows++] = c.logs.len;
            memcpy(u->log + u->log_len, arena + c.logs.off, (size_t)c.logs.len);
            u->log_len += c.logs.len;
        }
        if (c.status != GAME_STATUS_PLAYING || (max_commits && n >= max_commits)) break;
        if (table_load(&t, u->state, u->state_len, u->roster, ROSTER_BYTES) != TABLE_OK) break;
        BotDriveOut drv;
        if (bot_drive_eligible_mask(&g, game_human_mask(&g)) != 0) {
            if (table_bot_drive(&t, 0, 0, 1, &drv) <= 0) break;
        } else {
            calculate_legal_moves(&g, 0, &moves);
            if (moves.n == 0) break;
            const LegalMove *m = &moves.moves[n % moves.n];
            AwireAction a;
            memset(&a, 0, sizeof a);
            a.kind = m->type; a.n = m->n_cards;
            for (int i = 0; i < m->n_cards; i++) { a.cards[i] = m->cards[i]; a.attacks[i] = m->attack_cards[i]; }
            uint8_t wire[256];
            const int wl = awire_encode(&a, wire, sizeof wire);
            if (table_act(&t, UP_RS("h"), wire, wl, -1, 0) != TABLE_APPLIED) break;
        }
    }
}

// The row's state with no bot memory: the v5 blob a server before the memory wrote (the board, then
// the clock and the stamp).
static int up_as_v5(up_game_t *u) {
    const int board = state_blob_board_bytes(u->state, u->state_len), fixed = STATE_BLOB_V3_CLOCK_BYTES + GAME_RATINGS_BYTES;
    if (board < 0) return -1;
    memmove(u->state + board, u->state + u->state_len - fixed, (size_t)fixed);
    u->state[0] = STATE_BLOB_FORMAT_V5;
    u->state_len = board + fixed;
    return 0;
}

static void up_insert(sqlite3 *db, const char *id, const up_game_t *u) {
    sqlite3_stmt *st;
    T_EQ_INT(sqlite3_prepare_v2(db, "INSERT INTO games (id, status, version, state, roster, seed, needs_bots, round_epoch, "
                                    "log_from, created_at, updated_at, last_commit_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 0, 0, "
                                    "?7, ?8, ?9, ?9)", -1, &st, NULL), SQLITE_OK);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, u->status);
    sqlite3_bind_int64(st, 3, u->version);
    sqlite3_bind_blob(st, 4, u->state, u->state_len, SQLITE_STATIC);
    sqlite3_bind_blob(st, 5, u->roster, ROSTER_BYTES, SQLITE_STATIC);
    sqlite3_bind_blob(st, 6, u->seed, FOOLISH_SEED_LEN, SQLITE_STATIC);
    sqlite3_bind_int64(st, 7, u->list_from);
    sqlite3_bind_int64(st, 8, u->created_at);
    sqlite3_bind_int64(st, 9, u->now);
    T_EQ_INT(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    T_EQ_INT(sqlite3_prepare_v2(db, "INSERT INTO game_moves (game_id, seq, action) VALUES (?1, ?2, ?3)", -1, &st, NULL), SQLITE_OK);
    for (int i = 0; i < u->n_rows; i++) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, u->seq[i]);
        sqlite3_bind_blob(st, 3, u->log + u->off[i], u->len[i], SQLITE_STATIC);
        T_EQ_INT(sqlite3_step(st), SQLITE_DONE);
    }
    sqlite3_finalize(st);
}

// MUTATION-CHECKED (each red on the named check, then restored): the upgrade not chunking the log
// ("the list is the log's"), not folding the memory ("carries its bot memory"), not dropping
// game_moves ("game_moves is gone").
static void a_schema_9_database_turns_its_logs_into_move_lists(void) {
    if (tsb_dynamo()) return;
    static up_game_t over, mid;
    static Game g;
    static TableSnaps snaps;
    static uint8_t list[1 << 16], code_a[1 << 16], code_b[1 << 16], ex_a[4096], ex_b[4096], scratch[1 << 20];
    up_play(&over, 3, 0);
    up_play(&mid, 4, 24);
    T_EQ_INT(over.status, GAME_STATUS_GAME_OVER);
    T_EQ_INT(mid.status, GAME_STATUS_PLAYING);
    T_CHECK(over.n_rows > 10 && mid.n_rows > 10);
    T_EQ_INT(up_as_v5(&mid), 0);
    // A schema 9 file: the games table without move_list, game_moves with the sessions' rows.
    char path[320];
    snprintf(path, sizeof path, "%s/v9-%d.sqlite", g_dir, ++g_seq);
    cb_store *s = NULL;
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    if (!s) return;
    cb_store_close(s);
    sqlite3 *db;
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "ALTER TABLE games DROP COLUMN move_list; "
                              "CREATE TABLE game_moves (game_id TEXT NOT NULL, seq INTEGER NOT NULL, action BLOB NOT NULL, "
                              "PRIMARY KEY (game_id, seq)) WITHOUT ROWID; PRAGMA user_version = 9", NULL, NULL, NULL), SQLITE_OK);
    up_insert(db, "over11", &over);
    up_insert(db, "mid222", &mid);
    sqlite3_close(db);

    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    if (!s) return;
    Table t;
    const up_game_t *gs[2] = { &over, &mid };
    const char *ids[2] = { "over11", "mid222" };
    for (int k = 0; k < 2; k++) {
        const up_game_t *u = gs[k];
        cb_game_row row;
        T_EQ_INT(CB_ROWS_CALL(s, game_get, ids[k], &row), CB_OK);
        uint8_t *got = NULL;
        size_t got_len = 0;
        T_EQ_INT(CB_ROWS_CALL(s, game_list, ids[k], row.list_from, &got, &got_len), CB_OK);
        const int want = table_move_list_chunk(u->created_at, u->log, u->log_len, list, sizeof list);
        T_CHECK(want > 0 && (size_t)want == got_len && memcmp(got, list, got_len) == 0);   // the list is the log's
        table_init(&t, &g, &snaps);
        T_EQ_INT(table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len), TABLE_OK);
        if (u == &over) {
            // The finished game's replay from its list is the one its log made.
            T_CHECK(row.state_len == (size_t)u->state_len && memcmp(row.state, u->state, row.state_len) == 0);
            const int ca = oracle_replay_code(&t, u->seed, FOOLISH_SEED_LEN, u->log, u->log_len, code_a, sizeof code_a, scratch, sizeof scratch);
            T_EQ_INT(table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len), TABLE_OK);
            const int cb = table_replay_code_list(&t, u->seed, FOOLISH_SEED_LEN, got, (int)got_len, code_b, sizeof code_b, scratch, sizeof scratch);
            T_CHECK(ca > 0 && cb == ca && memcmp(code_a, code_b, (size_t)ca) == 0);
            const int ea = oracle_replay_extras(&t, u->log, u->log_len, ex_a, sizeof ex_a);
            const int eb = table_replay_extras_list(&t, row.created_at, got, (int)got_len, ex_b, sizeof ex_b);
            T_CHECK(ea > 0 && eb == ea && memcmp(ex_a, ex_b, (size_t)ea) == 0);
        } else {
            // The board being played carries its bot memory now, folded from the log; the board itself is the same.
            T_CHECK(!table_bots_need_logs(&t));   // carries its bot memory
            const int board = state_blob_board_bytes(u->state, u->state_len);
            T_CHECK(board > 0 && row.state_len > (size_t)u->state_len && memcmp(row.state + 1, u->state + 1, (size_t)board - 1) == 0);
        }
        free(got);
        cb_game_row_free(&row);
    }
    cb_store_close(s);
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    sqlite3_stmt *st;
    T_EQ_INT(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master WHERE name = 'game_moves'", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_INT(sqlite3_column_int(st, 0), 0);   // game_moves is gone
    sqlite3_finalize(st);
    sqlite3_close(db);
    // THE COPY BEFORE THE UPGRADE, beside it and kept: a sound schema-9 database with every
    // game_moves row, which the previous binary opens. MUTATION-CHECKED: no copy turns "the copy
    // before the upgrade is there" red.
    char copy[400];
    T_CHECK(cb_store_local_pre_upgrade_path(path, copy, sizeof copy));
    T_CHECK(access(copy, F_OK) == 0);   // the copy before the upgrade is there
    T_EQ_INT(sqlite3_open(copy, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_STR((const char *)sqlite3_column_text(st, 0), "ok");
    sqlite3_finalize(st);
    T_EQ_INT(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_INT(sqlite3_column_int(st, 0), 9);
    sqlite3_finalize(st);
    T_EQ_INT(sqlite3_prepare_v2(db, "SELECT count(*) FROM game_moves", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_INT(sqlite3_column_int(st, 0), over.n_rows + mid.n_rows);
    sqlite3_finalize(st);
    sqlite3_close(db);
    // Opened again at 10: nothing is repeated.
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    if (s) cb_store_close(s);
    unlink(path);

    // A copy that cannot be written refuses the open, and the schema-9 database is as it was.
    // MUTATION-CHECKED: the open going on without its copy turns "a failed copy refuses the open" red.
    snprintf(path, sizeof path, "%s/v9b-%d.sqlite", g_dir, ++g_seq);
    T_EQ_INT(cb_store_open_sqlite(path, &s), CB_OK);
    if (!s) return;
    cb_store_close(s);
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, "ALTER TABLE games DROP COLUMN move_list; "
                              "CREATE TABLE game_moves (game_id TEXT NOT NULL, seq INTEGER NOT NULL, action BLOB NOT NULL, "
                              "PRIMARY KEY (game_id, seq)) WITHOUT ROWID; PRAGMA user_version = 9", NULL, NULL, NULL), SQLITE_OK);
    up_insert(db, "over11", &over);
    sqlite3_close(db);
    T_CHECK(cb_store_local_pre_upgrade_path(path, copy, sizeof copy));
    char blocker[420];
    snprintf(blocker, sizeof blocker, "%s.tmp", copy);
    T_EQ_INT(mkdir(blocker, 0700), 0);   // the copy's file cannot be made
    s = NULL;
    T_CHECK(cb_store_open_sqlite(path, &s) != CB_OK && s == NULL);   // a failed copy refuses the open
    if (s) cb_store_close(s);
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_prepare_v2(db, "SELECT (SELECT user_version FROM pragma_user_version), "
                                    "(SELECT count(*) FROM game_moves)", -1, &st, NULL), SQLITE_OK);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    T_EQ_INT(sqlite3_column_int(st, 0), 9);   // untouched: still schema 9
    T_EQ_INT(sqlite3_column_int(st, 1), over.n_rows);
    sqlite3_finalize(st);
    sqlite3_close(db);
    rmdir(blocker);
    unlink(path);
}

int main(void) {
    snprintf(g_dir, sizeof g_dir, "/tmp/cbase-unit-store-XXXXXX");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    T_RUN(item_size_is_the_sum_of_its_attributes);
    T_RUN(bot_seed_is_production);
    T_RUN(bot_seed_names_read_back);
    T_RUN(open_applies_schema_and_seeds_bots);
    T_RUN(a_schema_9_database_turns_its_logs_into_move_lists);
    T_RUN(seed_adds_only_missing_bots);
    T_RUN(seed_brings_production_to_the_ladder);
    T_RUN(users_round_trip);
    T_RUN(signin_fails_round_trip);
    T_RUN(refresh_tokens_rotate);
    T_RUN(sessions_round_trip);
    T_RUN(game_round_trip);
    T_RUN(the_session_list_appends_and_clears);
    T_RUN(cas_second_writer_loses);
    T_RUN(lone_commit_cas);
    T_RUN(membership_follows_commits);
    T_RUN(game_delete_cascades);
    T_RUN(user_delete_cascades);
    T_RUN(identities_round_trip);
    T_RUN(once_claims);
    if (tsb_dynamo()) T_RUN(lease_semantics);   // the SQL store keeps no lease (serve: app/bots.c, in memory)
    T_RUN(games_needing_bots_windows);
    if (tsb_dynamo()) T_RUN(bots_idle_semantics);
    T_RUN(ratings_applied_by_commit);
    if (tsb_dynamo()) T_RUN(ratings_reindexed);   // the item layout's index key (SQL: an index)
    T_RUN(snapshots_query_shapes);
    T_RUN(snapshot_round_trip);
    T_RUN(snapshot_set_extras_rewrites_only_extras);
    if (tsb_dynamo()) T_RUN(replay_copies_hold_no_names);   // the item layout's pointers (SQL: replay_players holds no name)
    T_RUN(chat_newest_oldest_first);
    if (tsb_dynamo()) T_RUN(admin_reads_dynamo);
    else T_RUN(admin_reads);
    T_RUN(import_writes);
    T_RUN(import_never_lowers_a_session_generation);
    T_RUN(big_items_and_pages);
    T_RUN(cas_under_threads);
    T_RUN(lone_cas_under_threads);
    if (!tsb_dynamo()) T_RUN(group_commit_keeps_each_txn_alone);
    if (!tsb_dynamo()) T_RUN(every_write_moves_the_change_count);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}

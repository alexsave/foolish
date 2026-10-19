// unit_export.c - cbase-export (migrate/export.c) end to end, against DynamoDB Local (`make check-dynamo`,
// CBASE_STORE=dynamo; without it the test skips, saying so).
//
// A table is filled the way production's was, through the DynamoDB store's own operations (accounts with
// their emails and ratings, games with their seats and session logs, replays with their players' history
// rows, chat, sessions, refresh tokens, the seeded bots), the real build/cbase-export is run on it, and the
// SQLite file is read back through its tables: the counts, a game's log, a replay's summary, a message.
// Then the exit codes: a second run into the same file is refused (2) unless --into-existing (0), a bad
// --rate is a usage error (2), and an item of a family the SQL store has no table for is a failure (1)
// that leaves the .partial and makes no file.
//
// argv[1] is the path of build/cbase-export. Run from c_base/.
#include "t.h"

#include "aws/aws_call.h"
#include "lib_store_backend.h"
#include "store/store.h"
#include "store/store_import.h"
#include "store/store_priv.h"
#include "util/err.h"
#include "util/uuid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_bin;
static char g_table[64], g_dir[256];
enum { N_USERS = 12, N_GAMES = 6, N_SNAPS = 4, N_CHAT = 5 };
static cb_uuid g_user[N_USERS];
static int g_bots;

static bool keep_first_bot(const cb_bot_row *r, void *ud);

static void seed(cb_store *s) {
    static const uint8_t state[] = { 1, 2, 3, 4 }, roster[] = { 9, 9 }, log1[] = { 0x10, 0x11 }, log2[] = { 0x20 },
                         moves[] = { 7, 8, 9 }, extras[] = { 5, 6 };
    for (int i = 0; i < N_USERS; i++) {
        cb_user_row u = { .created_at = 1000 + i };
        cb_uuid_v4(u.id);
        snprintf(u.email, sizeof u.email, "u%d@x.io", i);
        snprintf(u.username, sizeof u.username, "USER%d", i);
        snprintf(u.password_hash, sizeof u.password_hash, "$2b$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
        T_EQ_INT(CB_STORE_CALL(s, user_create, &u), CB_OK);
        memcpy(g_user[i], u.id, sizeof g_user[i]);
        cb_session_row ss = { .created_at = 5000, .expires_at = 4102444800000LL, .gen = 0 };
        cb_uuid_v4(ss.id);
        memcpy(ss.user_id, u.id, sizeof ss.user_id);
        T_EQ_INT(CB_STORE_CALL(s, session_put, &ss), CB_OK);
        cb_refresh_row rt = { .created_at = 5000, .expires_at = 4102444800000LL };
        for (int k = 0; k < 64; k++) rt.token_hash[k] = "0123456789abcdef"[(i * 7 + k) % 16];
        memcpy(rt.user_id, u.id, sizeof rt.user_id);
        memcpy(rt.session_id, ss.id, sizeof rt.session_id);
        T_EQ_INT(CB_STORE_CALL(s, refresh_put, &rt), CB_OK);
    }
    g_bots = cb_import_seed_bots(s);
    T_CHECK(g_bots > 0);
    cb_bot_row first_bot_row = { 0 };
    CB_ROWS_CALL(s, bots_list, keep_first_bot, &first_bot_row);
    for (int i = 0; i < N_GAMES; i++) {
        cb_game_row g = { .status = 1, .state = (uint8_t *)state, .state_len = sizeof state, .roster = (uint8_t *)roster,
                          .roster_len = sizeof roster, .created_at = 10, .updated_at = 10 };
        snprintf(g.id, sizeof g.id, "%06d", i);   // all-digit codes, a leading zero
        T_EQ_INT(CB_ROWS_CALL(s, game_create, &g, g_user[i]), CB_OK);
        // A commit seats a second human and a bot and writes the session log; a second one adds to it.
        cb_game_row n = g;
        n.version = 1;
        n.updated_at = 20;
        cb_game_commit c = { .row = &n, .expected_version = 0, .n_humans = 2, .n_bots = 1, .log_add = log1,
                             .log_add_len = sizeof log1, .log_clear = true };
        n.log_from = 1;
        memcpy(c.humans[0], g_user[i], sizeof(cb_uuid));
        memcpy(c.humans[1], g_user[i + 1], sizeof(cb_uuid));
        memcpy(c.bots[0], first_bot_row.id, sizeof(cb_uuid));
        T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
        n.version = 2;
        c.expected_version = 1;
        c.log_add = log2;
        c.log_add_len = sizeof log2;
        c.log_clear = false;
        T_EQ_INT(CB_ROWS_CALL(s, game_commit, &c), CB_OK);
    }
    for (int i = 0; i < N_SNAPS; i++) {
        cb_snapshot_row sn = { .n_players = 2, .moves = (uint8_t *)moves, .moves_len = sizeof moves,
                               .extras = (uint8_t *)extras, .extras_len = sizeof extras, .created_at = 30 + i,
                               .sum = { .has = true, .fool = 0, .n_elim = 1, .elim = { 1 }, .duration_s = 60 + i } };
        cb_uuid_v4(sn.id);
        snprintf(sn.game_id, sizeof sn.game_id, "%06d", i);
        memcpy(sn.player_ids[0], g_user[i], sizeof(cb_uuid));
        memcpy(sn.player_ids[1], g_user[i + 1], sizeof(cb_uuid));
        T_EQ_INT(CB_ROWS_CALL(s, snapshot_put, &sn), CB_OK);
    }
    for (int i = 0; i < N_CHAT; i++) {
        cb_chat_row c = { .created_at = 40 + i, .message = (char *)"it's \"quoted\" \xc3\xa9" };
        cb_uuid_v4(c.id);
        snprintf(c.game_id, sizeof c.game_id, "%06d", 0);
        memcpy(c.user_id, g_user[i % 2], sizeof c.user_id);
        T_EQ_INT(CB_ROWS_CALL(s, chat_put, &c), CB_OK);
    }
}

static bool keep_first_bot(const cb_bot_row *r, void *ud) {
    *(cb_bot_row *)ud = *r;
    return false;
}

// ---- running the binary -------------------------------------------------------------------------

typedef struct {
    int  code;
    char out[16384];
} run;

static void run_export(run *r, const char *a1, const char *a2, const char *a3, const char *a4) {
    int pfd[2];
    T_EQ_INT(pipe(pfd), 0);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        setenv("CBASE_TABLE", g_table, 1);
        const char *argv[8] = { g_bin };
        int n = 1;
        if (a1) argv[n++] = a1;
        if (a2) argv[n++] = a2;
        if (a3) argv[n++] = a3;
        if (a4) argv[n++] = a4;
        argv[n] = NULL;
        execv(g_bin, (char *const *)argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t len = 0;
    ssize_t k;
    char drain[4096];
    while ((k = len < sizeof r->out - 1 ? read(pfd[0], r->out + len, sizeof r->out - 1 - len) : read(pfd[0], drain, sizeof drain)) > 0)
        if (len < sizeof r->out - 1) len += (size_t)k;   // (what does not fit is read and dropped: the child must not get SIGPIPE)
    r->out[len] = 0;
    close(pfd[0]);
    if (getenv("UNIT_EXPORT_TRACE")) fprintf(stderr, "---- cbase-export %s %s ----\n%s\n", a1 ? a1 : "", a2 ? a2 : "", r->out);
    int st = 0;
    waitpid(pid, &st, 0);
    r->code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

// ---- comparing ------------------------------------------------------------------------------------


typedef struct { long n; } first_count;
static bool take_count(int n, const char *const *names, const char *const *values, void *ud) {
    (void)names;
    if (n > 0 && values[0]) ((first_count *)ud)->n = atol(values[0]);
    return false;
}

// One count from the file.
static long count_of(cb_store *s, const char *sql) {
    first_count c = { -1 };
    CB_STORE_CALL(s, admin_query, sql, false, take_count, &c);
    return c.n;
}

static bool raw_call(const char *target, const char *json) {
    const char *ep = getenv("AWS_ENDPOINT_URL_DYNAMODB");
    char host[256], base[64];
    int port;
    bool tls = true;
    if (!ep || cb_aws_url_parse(ep, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK || tls) return false;
    char tg[96];
    snprintf(tg, sizeof tg, "X-Amz-Target: DynamoDB_20120810.%s", target);
    const char *const headers[] = { "Content-Type: application/x-amz-json-1.0", tg };
    cb_https_response r;
    int rc = cb_aws_call("dynamodb", "POST", host, port, false, "/", headers, 2, json, strlen(json), 10000, &r);
    bool ok = rc == CB_OK && r.status == 200;
    cb_https_response_free(&r);
    return ok;
}

static void the_export_end_to_end(void) {
    snprintf(g_table, sizeof g_table, "exp_%d_%d", (int)getpid(), rand() % 100000);
    // The stack table's shape, on demand: the export paces itself to a provisioned table's reads (5 RCU/s here, which a
    // thousand items would take minutes to read), and an on-demand one is not paced.
    char ct[1400];
    snprintf(ct, sizeof ct,
             "{\"TableName\":\"%s\",\"BillingMode\":\"PAY_PER_REQUEST\",\"AttributeDefinitions\":["
             "{\"AttributeName\":\"pk\",\"AttributeType\":\"S\"},{\"AttributeName\":\"sk\",\"AttributeType\":\"S\"},"
             "{\"AttributeName\":\"gsi1pk\",\"AttributeType\":\"S\"},{\"AttributeName\":\"gsi1sk\",\"AttributeType\":\"S\"},"
             "{\"AttributeName\":\"gsi2pk\",\"AttributeType\":\"S\"},{\"AttributeName\":\"gsi2sk\",\"AttributeType\":\"S\"}],"
             "\"KeySchema\":[{\"AttributeName\":\"pk\",\"KeyType\":\"HASH\"},{\"AttributeName\":\"sk\",\"KeyType\":\"RANGE\"}],"
             "\"GlobalSecondaryIndexes\":["
             "{\"IndexName\":\"gsi1\",\"KeySchema\":[{\"AttributeName\":\"gsi1pk\",\"KeyType\":\"HASH\"},{\"AttributeName\":\"gsi1sk\",\"KeyType\":\"RANGE\"}],\"Projection\":{\"ProjectionType\":\"ALL\"}},"
             "{\"IndexName\":\"gsi2\",\"KeySchema\":[{\"AttributeName\":\"gsi2pk\",\"KeyType\":\"HASH\"},{\"AttributeName\":\"gsi2sk\",\"KeyType\":\"RANGE\"}],\"Projection\":{\"ProjectionType\":\"ALL\"}}]}",
             g_table);
    T_CHECK(raw_call("CreateTable", ct));
    cb_store *table = NULL;
    T_EQ_INT(cb_store_open_dynamo(g_table, &table), CB_OK);
    seed(table);

    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/cbase-export-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    T_EQ_INT(mkdir(g_dir, 0700), 0);
    char file[320];
    snprintf(file, sizeof file, "%s/out.sqlite", g_dir);
    static run r;

    // The copy: exit 0, every family written and counted, the second read equal.
    run_export(&r, "--rate", "100", file, NULL);
    if (r.code != 0) fprintf(stderr, "%s\n", r.out);
    T_EQ_INT(r.code, 0);
    T_CHECK(strstr(r.out, "second read: ") != NULL && strstr(r.out, " 0 differ from the file, 0 gone") != NULL);
    T_CHECK(strstr(r.out, "DIFFERENT") == NULL && strstr(r.out, "FAILED") == NULL);
    T_CHECK(strstr(r.out, "0 items failed") != NULL);
    char partial[340];
    snprintf(partial, sizeof partial, "%s.partial", file);
    T_EQ_INT(access(partial, F_OK), -1);
    // The file's tables, by plain SQL and through the rows serve reads.
    cb_store *dst = NULL;
    T_EQ_INT(cb_store_open_sqlite(file, &dst), CB_OK);
    if (!dst) return;
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM users"), N_USERS);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM ratings"), N_USERS);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM bots"), g_bots);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM games"), N_GAMES);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM game_members"), 2 * N_GAMES);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM game_bots"), N_GAMES);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM replays"), N_SNAPS);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM replay_players"), 2 * N_SNAPS);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM chat"), N_CHAT);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM sessions"), N_USERS);
    T_EQ_INT(count_of(dst, "SELECT count(*) FROM refresh_tokens"), N_USERS);
    cb_game_row g;
    T_EQ_INT(CB_ROWS_CALL(dst, game_get, "000001", &g), CB_OK);
    T_EQ_INT(g.version, 2);
    uint8_t *logs = NULL;
    size_t logs_len = 0;
    T_EQ_INT(CB_ROWS_CALL(dst, game_logs, "000001", g.log_from, &logs, &logs_len), CB_OK);
    T_EQ_MEM(logs, logs_len, "\x10\x11\x20", 3);   // the cumulative log, whole
    free(logs);
    cb_game_row_free(&g);
    T_EQ_INT(CB_ROWS_CALL(dst, is_member, "000002", g_user[3]), 1);
    cb_store_close(dst);

    // A second run into the same file is refused; with --into-existing it puts over and agrees.
    run_export(&r, file, NULL, NULL, NULL);
    T_EQ_INT(r.code, 2);
    T_CHECK(strstr(r.out, "already holds") != NULL);
    run_export(&r, "--into-existing", file, NULL, NULL);
    T_EQ_INT(r.code, 0);
    T_CHECK(strstr(r.out, "0 items failed") != NULL);

    // Usage errors: exit 2, nothing read (on a path that does not exist, so a refusal of the file cannot stand in for it).
    char fresh[320];
    snprintf(fresh, sizeof fresh, "%s/usage.sqlite", g_dir);
    static const char *const bad[][2] = { { "--rate", "0" }, { "--rate", "101" }, { "--rate", "x" }, { "--rate", "-5" }, { "--bogus", NULL } };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        run_export(&r, bad[i][0], bad[i][1] ? bad[i][1] : fresh, bad[i][1] ? fresh : NULL, NULL);
        T_EQ_INT(r.code, 2);
    }
    T_EQ_INT(access(fresh, F_OK), -1);   // no file was made for any of them
    run_export(&r, NULL, NULL, NULL, NULL);
    T_EQ_INT(r.code, 2);
    T_CHECK(strstr(r.out, "usage:") != NULL);

    // An item of a family the SQL store has no table for fails the run loudly: exit 1, no file, the .partial
    // left for a look, and a rerun refuses until it goes.
    char json[512];
    snprintf(json, sizeof json, "{\"TableName\":\"%s\",\"Item\":{\"pk\":{\"S\":\"EXP#BAD\"},\"sk\":{\"S\":\"BAD\"}}}", g_table);
    T_CHECK(raw_call("PutItem", json));
    char file2[320], partial2[340];
    snprintf(file2, sizeof file2, "%s/second.sqlite", g_dir);
    snprintf(partial2, sizeof partial2, "%s.partial", file2);
    run_export(&r, "--rate", "100", file2, NULL);
    T_EQ_INT(r.code, 1);
    T_CHECK(strstr(r.out, "EXP#BAD / BAD: an unknown family") != NULL);
    T_EQ_INT(access(file2, F_OK), -1);
    T_EQ_INT(access(partial2, F_OK), 0);
    T_CHECK(strstr(r.out, "not accepted") != NULL);
    run_export(&r, "--rate", "100", file2, NULL);
    T_EQ_INT(r.code, 2);
    T_CHECK(strstr(r.out, "left from an earlier run") != NULL);
    cb_store_close(table);
    snprintf(json, sizeof json, "{\"TableName\":\"%s\"}", g_table);
    raw_call("DeleteTable", json);
}

int main(int argc, char **argv) {
    if (!tsb_dynamo()) {
        printf("skip unit_export.c: no DynamoDB Local (CBASE_STORE=dynamo, make check-dynamo)\n");
        return 0;
    }
    if (argc != 2) { fprintf(stderr, "usage: unit_export <path to cbase-export>\n"); return 2; }
    g_bin = argv[1];
    T_RUN(the_export_end_to_end);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (g_dir[0] && system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}

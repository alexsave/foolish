// export.c - `cbase-export <sqlite file>`: a DynamoDB table copied into a
// SQLite file, for moving c_base from Lambda + DynamoDB to `cbase serve`.
//
//   CBASE_TABLE=cbase build/cbase-export ./cbase.sqlite
//   CBASE_TABLE=cbase build/cbase-export --rate 100 --into-existing ./cbase.sqlite
//
// The table's keys are listed (item_keys), sorted into the order the rows need, and read 100 at a
// time (item_batch_get); each item is decoded by its family into its row and written through the SQL
// store's own operations into the plain tables (store/migrate_rows.h lists the families). Nothing of
// DynamoDB's key shape reaches the file. A username is copied as it is shown (names from before the
// case rule keep their lower case); two accounts whose names compare equal (auth/names.h cb_name_key)
// fail the run loudly, naming both ids. Chat whose game is gone is skipped and counted ("orphaned,
// skipped"). An unknown family and a row the store refuses each fail the run loudly.
//
// READ-ONLY ON DYNAMODB: the Scan, BatchGetItem and DescribeTable calls, and
// nothing that writes. The table is shared with production, so the reads are
// paced in the store's bulk mode (a token bucket on the table's provisioned
// read units, throttles waited out with a jittered backoff, never given up
// on), and held to --rate percent of them (default 25; 100 is all of the
// provisioned capacity, which production then goes without). Only the base table is read (a Scan and
// BatchGetItem, never an index), so reads are paced to the TABLE's read units, not an index's.
//
// THE FREEZE WINDOW: with writes frozen, run it at --rate 100. The run reads the table about three
// times (the key scan, the copy, the verify), so its length is about 3 x the table's read units used
// / its provisioned RCU. To shorten the window, raise the table's ReadCapacityUnits before the freeze
// (aws dynamodb update-table --table-name cbase --provisioned-throughput ReadCapacityUnits=N,
// WriteCapacityUnits=<unchanged>; it applies in seconds and the pacing reads it at start) and lower
// it after (decreases are limited per day).
//
// It then VERIFIES: each family's item count against its table's row count (an expired session,
// token or sign-in count is skipped and counted as such; the bot scan's markers are not kept), then a
// second read of the whole table, each item looked up in the file by its row's natural key (an
// account's id, a game's code, a replay's id, ...) and compared field by field.
//
// A new file is built as <file>.partial and renamed to <file> only when the copy and every
// check passed, so <file> is never half an export; a failed run leaves the .partial for a look
// and the next run refuses until it is removed. A file that already holds accounts or games is
// refused unless --into-existing. A new file's seed (the bots a store is opened with) is
// cleared first: the file ends up holding exactly the table. --no-verify skips the second read.
//
// Credentials and region come from the environment (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY,
// AWS_SESSION_TOKEN, AWS_REGION), as the Lambda gets them; they are never printed.
//
// Exit 0: every item copied, every count equal, every item read back equal. 1: an item failed to
// copy, a count differs or an item differs. 2: a usage or I/O error.
#include "store/store.h"
#include "store/migrate_rows.h"
#include "util/clock.h"
#include "store/store_priv.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../shared/c/num_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *pk, *sk;
} key;

typedef struct {
    key   *k;
    size_t n, cap;
    bool   oom;
} keyset;

static bool keyset_add(const char *pk, const char *sk, void *ud) {
    keyset *s = ud;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        key *k = realloc(s->k, cap * sizeof *k);
        if (!k) { s->oom = true; return false; }
        s->k = k;
        s->cap = cap;
    }
    char *p = strdup(pk), *q = strdup(sk);
    if (!p || !q) { free(p); free(q); s->oom = true; return false; }
    s->k[s->n++] = (key){ p, q };
    return true;
}

static int key_cmp(const void *a, const void *b) {
    const key *x = a, *y = b;
    int c = strcmp(x->pk, y->pk);
    return c ? c : strcmp(x->sk, y->sk);
}

static void keyset_free(keyset *s) {
    for (size_t i = 0; i < s->n; i++) { free(s->k[i].pk); free(s->k[i].sk); }
    free(s->k);
    memset(s, 0, sizeof *s);
}

// Every key of the store, sorted. CB_OK or an error.
static int keys_of(cb_store *st, keyset *out) {
    memset(out, 0, sizeof *out);
    int rc = st->ops->item_keys(st, keyset_add, out);
    if (rc == CB_OK && out->oom) rc = CB_E_NOMEM;
    if (rc == CB_OK) qsort(out->k, out->n, sizeof *out->k, key_cmp);
    return rc;
}

static bool count_cell(int n, const char *const *names, const char *const *values, void *ud) {
    (void)names;
    int64_t v;
    if (n > 0 && values[0] && num_parse_i64_cstr(values[0], 0, INT64_MAX, &v)) *(long *)ud = (long)v;
    return false;
}

// The copy's order: the family's rank (migrate_rows.h), then the key. An unknown family sorts first,
// so it is reported before anything is written.
static int order_cmp(const void *a, const void *b) {
    const key *x = a, *y = b;
    int rx = cb_mig_rank(x->pk, x->sk), ry = cb_mig_rank(y->pk, y->sk);
    return rx != ry ? (rx < ry ? -1 : 1) : key_cmp(a, b);
}

// A deep copy of an item as the read handed it.
static bool item_dup(cb_item *dst, const cb_item *src) {
    *dst = *src;
    dst->store.p = malloc(src->store.len ? src->store.len : 1);
    if (!dst->store.p) return false;
    memcpy(dst->store.p, src->store.p, src->store.len);
    dst->store.cap = src->store.len;
    return true;
}

typedef struct {
    cb_item items[100];
    bool    got[100];
} batch;

static bool keep_one(int i, const cb_item *it, void *ud) {
    batch *b = ud;
    if (item_dup(&b->items[i], it)) b->got[i] = true;
    return true;
}

// Every key in `ks`, read 100 at a time in their order: with `m` and `put`, each put into the SQL store
// (cb_mig_put); else each compared with it (cb_mig_same). The items that failed or differ; *gone the keys
// the table no longer had. -1 when a read failed.
static long walk(cb_store *src, cb_mig *m, const keyset *ks, bool put, long *gone) {
    long bad = 0;
    *gone = 0;
    batch *b = calloc(1, sizeof *b);
    if (!b) return -1;
    for (size_t base = 0; base < ks->n; base += 100) {
        int n = ks->n - base < 100 ? (int)(ks->n - base) : 100;
        cb_ikey k[100];
        for (int i = 0; i < n; i++) k[i] = (cb_ikey){ ks->k[base + i].pk, ks->k[base + i].sk };
        memset(b->got, 0, sizeof b->got);
        if (src->ops->item_batch_get(src, k, n, NULL, 0, keep_one, b) != CB_OK) {
            fprintf(stderr, "cbase-export: reading items %zu-%zu failed\n", base, base + (size_t)n);
            free(b);
            return -1;
        }
        for (int i = 0; i < n; i++) {
            if (!b->got[i]) { (*gone)++; continue; }
            if (put) { if (cb_mig_put(m, &b->items[i]) != CB_OK) bad++; }
            else if (cb_mig_same(m, &b->items[i]) != 1) bad++;
            free(b->items[i].store.p);
        }
        if (put && ((base / 100) % 50 == 49 || base + 100 >= ks->n)) printf("  %zu of %zu read\n", base + (size_t)n, ks->n);
    }
    free(b);
    return bad;
}

// The rows a file holds that a run would put over: accounts, games, replays.
static long held(cb_store *dst) {
    long total = 0;
    static const char *const T[] = { "users", "games", "replays", "chat" };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        char sql[64];
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", T[i]);
        struct { long n; } c = { -1 };
        if (CB_STORE_CALL(dst, admin_query, sql, false, count_cell, &c) != 1) return -1;
        total += c.n;
    }
    return total;
}

static void usage(void) {
    fprintf(stderr,
            "usage: cbase-export [--rate PERCENT] [--creds-cmd COMMAND] [--into-existing] [--no-verify] <sqlite file>\n"
            "  reads the DynamoDB table CBASE_TABLE (default cbase), read-only, at PERCENT (1-100, default 25)\n"
            "  of its provisioned read capacity, and puts every item into the SQLite file\n"
            "  expired temporary credentials are fetched again with COMMAND (default: aws configure export-credentials --format process)\n"
            "  (built as <file>.partial, renamed when every check passed)\n");
}

int main(int argc, char **argv) {
    int64_t pct = 25;
    bool existing = false, verify = true;
    const char *creds_cmd = getenv("CBASE_CREDS_CMD");
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            if (!num_parse_i64_cstr(argv[++i], 1, 100, &pct)) { usage(); return 2; }
        } else if (strcmp(argv[i], "--creds-cmd") == 0 && i + 1 < argc) creds_cmd = argv[++i];
        else if (strcmp(argv[i], "--into-existing") == 0) existing = true;
        else if (strcmp(argv[i], "--no-verify") == 0) verify = false;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { usage(); return 2; }
    }
    if (!path) { usage(); return 2; }
    cb_log_init(CB_LOG_INFO, 0);
    const char *table = getenv("CBASE_TABLE");
    if (!table || !*table) table = "cbase";

    // A file that is new (absent, or empty) gets the schema's seed when it is opened (the 49 bots): that is
    // the store's start, not an earlier export, and the table's own rows are the truth, so the seed is cleared
    // and the file ends up holding exactly the table. A file that held something before is another matter.
    // A new file is built as <file>.partial and renamed to <file> only when every check passed, so the
    // file at <file> is never half an export (the whole copy is one all-or-nothing step). --into-existing
    // works in the file itself.
    struct stat sb;
    bool fresh = stat(path, &sb) != 0 || sb.st_size == 0;
    char work[1100];
    snprintf(work, sizeof work, fresh ? "%s.partial" : "%s", path);
    if (fresh && stat(work, &sb) == 0) {
        fprintf(stderr, "cbase-export: %s is left from an earlier run; remove it first\n", work);
        return 2;
    }

    cb_store *src = NULL, *dst = NULL;
    int rc = cb_store_open_dynamo(table, &src);
    if (rc != CB_OK) { fprintf(stderr, "cbase-export: cannot open the DynamoDB table %s: %s\n", table, cb_strerror(rc)); return 2; }
    rc = cb_store_open_sqlite(work, &dst);
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-export: cannot open %s: %s\n", work, cb_strerror(rc));
        cb_store_close(src);
        return 2;
    }
    // Temporary credentials (an `aws login` export lasts 15 minutes) are fetched again through a credential
    // process when DynamoDB says they expired: --creds-cmd, CBASE_CREDS_CMD, or by default the AWS CLI's own
    // when the environment holds a session token.
    if (!creds_cmd || !*creds_cmd) {
        const char *tok = getenv("AWS_SESSION_TOKEN");
        creds_cmd = tok && *tok ? "aws configure export-credentials --format process" : NULL;
    }
    if (creds_cmd && cb_store_dynamo_creds_process(src, creds_cmd) != CB_OK) {
        fprintf(stderr, "cbase-export: the credential command is too long\n");
        return 2;
    }
    // Paced to the provisioned reads, then held to the share asked for. Without a
    // DescribeTable answer there is no pace to hold, so refuse rather than scan
    // a shared table at full speed.
    if (cb_store_dynamo_bulk(src) != CB_OK) {
        fprintf(stderr, "cbase-export: DescribeTable failed; not reading a shared table unpaced\n");
        cb_store_close(src);
        cb_store_close(dst);
        return 2;
    }
    cb_store_dynamo_read_share(src, (double)pct / 100.0);

    // A new file was seeded with the store's bots: the table's own are the truth, so they go first. A file
    // that already holds accounts or games is put over only with --into-existing.
    if (fresh && CB_ROWS_CALL(dst, bots_replace, NULL, 0) != CB_OK) {
        fprintf(stderr, "cbase-export: cannot clear the new file's seed\n");
        return 2;
    }
    long had = held(dst);
    if (had < 0 || (had > 0 && !existing)) {
        fprintf(stderr, "cbase-export: %s already holds %ld rows; use --into-existing to put over them\n", work, had);
        cb_store_close(src);
        cb_store_close(dst);
        return 2;
    }

    keyset want;
    rc = keys_of(src, &want);
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-export: scanning %s failed: %s\n", table, cb_strerror(rc));
        keyset_free(&want);
        cb_store_close(src);
        cb_store_close(dst);
        return 2;
    }
    qsort(want.k, want.n, sizeof *want.k, order_cmp);
    printf("table %s: %zu items\n", table, want.n);

    // THE COPY: each item decoded by its family into its row, in the order rows need (accounts before
    // the seats that name them, a game's seats before its row, a replay's history rows before it).
    cb_mig *m = cb_mig_new(dst, cb_now_ms(), stderr);
    if (!m) return 2;
    long gone = 0, failed = walk(src, m, &want, true, &gone);
    if (failed >= 0 && cb_mig_finish(m) != CB_OK) failed++;
    if (gone) printf("%ld keys were gone by the time they were read (deleted or expired during the run)\n", gone);
    int exit_code = failed != 0 ? 1 : 0;
    if (failed < 0) exit_code = 2;

    // THE CHECKS: the counts (each family's items against its table's rows), then a second read of the
    // whole table, each item looked up in the file by its row's natural key and compared field by field
    // (at the cutover, writes are frozen: any difference is a failure).
    if (cb_mig_report(m, stdout) != 0) exit_code = exit_code ? exit_code : 1;
    if (verify && exit_code == 0) {
        keyset again;
        if (keys_of(src, &again) != CB_OK) { fprintf(stderr, "cbase-export: the verification scan failed\n"); return 2; }
        qsort(again.k, again.n, sizeof *again.k, order_cmp);
        long gone2 = 0, differ = walk(src, m, &again, false, &gone2);
        printf("second read: %zu items, %ld differ from the file, %ld gone, %zu then %zu keys\n", again.n, differ, gone2, want.n, again.n);
        if (differ != 0 || gone2 || again.n != want.n) exit_code = 1;
        keyset_free(&again);
    }
    printf("%ld items failed\n", failed);
    cb_mig_free(m);
    keyset_free(&want);
    cb_store_close(src);
    cb_store_close(dst);
    if (strcmp(work, path) != 0) {
        // The closed file holds everything (the WAL is checkpointed into it and empty); a WAL that still
        // holds data would be left behind by the rename, so it is refused, and the empty ones are removed.
        char side[1110];
        for (int w = 0; exit_code == 0 && w < 2; w++) {
            snprintf(side, sizeof side, "%s%s", work, w ? "-shm" : "-wal");
            if (stat(side, &sb) == 0 && w == 0 && sb.st_size > 0) {
                fprintf(stderr, "cbase-export: %s still holds data after the close; not renaming\n", side);
                exit_code = 2;
            }
        }
        if (exit_code == 0) {
            snprintf(side, sizeof side, "%s-wal", work);
            unlink(side);
            snprintf(side, sizeof side, "%s-shm", work);
            unlink(side);
        }
        if (exit_code == 0 && rename(work, path) == 0) printf("%s written\n", path);
        else if (exit_code == 0) { fprintf(stderr, "cbase-export: cannot rename %s to %s\n", work, path); exit_code = 2; }
        else printf("not accepted: the copy stays at %s, %s is not made\n", work, path);
    }
    return exit_code;
}

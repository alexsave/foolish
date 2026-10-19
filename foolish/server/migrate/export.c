// export.c - `cbase-export <sqlite file>`: a DynamoDB table copied into a
// SQLite file, for moving c_base from Lambda + DynamoDB to `cbase serve`.
//
//   CBASE_TABLE=cbase build/cbase-export ./cbase.sqlite
//   CBASE_TABLE=cbase build/cbase-export --rate 100 --into-existing ./cbase.sqlite
//
// Both backends keep the same items (core/src/store/items.h), so this is the
// item layer's walk and put: item_keys lists the table's keys, item_batch_get
// reads them 100 at a time, item_write puts each into the SQLite store (which
// the schema creates when the file is new). Every attribute is copied as it
// is, the index keys and the TTL's two attributes among them.
//
// READ-ONLY ON DYNAMODB: the Scan, BatchGetItem and DescribeTable calls, and
// nothing that writes. The table is shared with production, so the reads are
// paced in the store's bulk mode (a token bucket on the table's provisioned
// read units, throttles waited out with a jittered backoff, never given up
// on), and held to --rate percent of them (default 25; 100 is all of the
// provisioned capacity, which production then goes without).
//
// It then VERIFIES: both sides' keys listed again with item_keys (the table
// a second time, at the same pace), counted and compared. A key in the table
// and not in the file is a MISSING item; a key in the file and not in the
// table is an EXTRA (an item deleted from the table, or TTL-expired, during
// the run). Then three more checks (verify_deep): the pk families counted on both sides, every
// item's gsi1 and gsi2 keys queried in the file (each must list its item), and a second read of
// every item compared with the file (an item written in the table since is CHANGED).
//
// Values are also compared as each item is copied: the item is read back from the file right
// after its put and compared attribute by attribute (kind and value, and the same set of
// attributes) with the item as DynamoDB answered it. A difference is a MISMATCH.
//
// A new file is built as <file>.partial and renamed to <file> only when the copy and every
// check passed, so <file> is never half an export; a failed run leaves the .partial for a look
// and the next run refuses until it is removed. A file that already exists is refused unless
// --into-existing (a put replaces an item). A new file's seed (the bots a store is opened
// with) is cleared first: the file ends up holding exactly the table. --no-verify skips the
// second scan and the checks after it.
//
// Credentials and region come from the environment (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY,
// AWS_SESSION_TOKEN, AWS_REGION), as the Lambda gets them; they are never printed.
//
// Exit 0: every item copied and read back equal, and the key sets equal. 1: an
// item failed to copy or read back different, or the key sets differ. 2: a usage or I/O error.
#include "store/store.h"
#include "store/store_copy.h"
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

typedef struct {
    cb_store *dst;
    const key *keys;     // this batch's keys
    long      copied, failed, mismatched;
    bool      seen[100];
    int       base;
} copy_ctx;

static bool put_one(int i, const cb_item *it, void *ud) {
    copy_ctx *c = ud;
    c->seen[i] = true;
    cb_ikey at = { c->keys[i].pk, c->keys[i].sk };
    int rc = cb_copy_put(c->dst, at, it);
    if (rc == CB_OK) {
        c->copied++;
        if (!cb_copy_same(c->dst, at, it)) {
            c->mismatched++;
            fprintf(stderr, "cbase-export: %s / %s reads back different from the table\n", at.pk, at.sk);
        }
    } else {
        c->failed++;
        fprintf(stderr, "cbase-export: cannot put %s / %s: %s\n", at.pk, at.sk, cb_strerror(rc));
    }
    return true;
}

typedef struct {
    cb_store  *dst;
    const key *keys;
    long       differ, seen;
    bool       got[100];
} rescan_ctx;

static bool rescan_one(int i, const cb_item *it, void *ud) {
    rescan_ctx *c = ud;
    c->got[i] = true;
    cb_ikey at = { c->keys[i].pk, c->keys[i].sk };
    if (!cb_copy_same(c->dst, at, it)) {
        if (c->differ++ < 20) printf("CHANGED since it was copied: %s / %s\n", at.pk, at.sk);
    }
    c->seen++;
    return true;
}

// The checks after the key sets agree. Returns the number of failed ones.
//   1. The key families: each pk prefix (USER, GAME, ...) counted in the table and in the file.
//   2. Every index key round-trips: each item's gsi1 and gsi2 keys, queried in the file, list the item.
//   3. A second read of the table's items (the keys of `a`) compared with the file attribute by attribute:
//      an item written in the table since it was copied shows as CHANGED (expected while the app still
//      writes; none at the cutover, when writes are frozen).
static int verify_deep(cb_store *src, cb_store *dst, const keyset *a, const keyset *b) {
    int failed = 0;
    printf("families (table / file):\n");
    for (size_t i = 0; i < a->n;) {
        char fam[64];
        cb_copy_family(a->k[i].pk, fam, sizeof fam);
        long ta = 0, tb = 0;
        size_t j = i;
        while (j < a->n) {
            char f2[64];
            cb_copy_family(a->k[j].pk, f2, sizeof f2);
            if (strcmp(f2, fam) != 0) break;
            ta++, j++;
        }
        for (size_t k = 0; k < b->n; k++) {
            char f2[64];
            cb_copy_family(b->k[k].pk, f2, sizeof f2);
            if (strcmp(f2, fam) == 0) tb++;
        }
        printf("  %-16s %6ld %6ld%s\n", fam, ta, tb, ta == tb ? "" : "   DIFFERENT");
        if (ta != tb) failed++;
        i = j;
    }
    long indexed = 0, bad = 0;
    for (size_t i = 0; i < b->n; i++) {
        int n = 0, miss = cb_copy_index_roundtrip(dst, (cb_ikey){ b->k[i].pk, b->k[i].sk }, &n);
        indexed += n;
        if (miss != 0) {
            if (bad++ < 20) printf("INDEX KEY does not list the item: %s / %s\n", b->k[i].pk, b->k[i].sk);
        }
    }
    printf("index keys: %ld checked, %ld did not round-trip\n", indexed, bad);
    if (bad) failed++;
    rescan_ctx c = { .dst = dst };
    long gone = 0;
    for (size_t base = 0; base < a->n; base += 100) {
        int n = a->n - base < 100 ? (int)(a->n - base) : 100;
        cb_ikey ks[100];
        for (int i = 0; i < n; i++) ks[i] = (cb_ikey){ a->k[base + i].pk, a->k[base + i].sk };
        c.keys = a->k + base;
        memset(c.got, 0, sizeof c.got);
        if (src->ops->item_batch_get(src, ks, n, NULL, 0, rescan_one, &c) != CB_OK) {
            fprintf(stderr, "cbase-export: the second read failed at item %zu\n", base);
            return failed + 1;
        }
        for (int i = 0; i < n; i++) if (!c.got[i]) gone++;
    }
    printf("second read: %ld items compared, %ld changed, %ld gone\n", c.seen, c.differ, gone);
    if (c.differ || gone) failed++;
    return failed;
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

    keyset have;
    if (keys_of(dst, &have) != CB_OK) { fprintf(stderr, "cbase-export: cannot list %s\n", work); return 2; }
    if (fresh) {
        for (size_t i = 0; i < have.n; i++) {
            cb_iwrite del;
            memset(&del, 0, sizeof del);
            del.op = CB_IW_DELETE;
            del.key = (cb_ikey){ have.k[i].pk, have.k[i].sk };
            if (dst->ops->item_write(dst, &del, NULL) != CB_OK) {
                fprintf(stderr, "cbase-export: cannot clear the new file's seed (%s / %s)\n", del.key.pk, del.key.sk);
                keyset_free(&have);
                cb_store_close(src);
                cb_store_close(dst);
                return 2;
            }
        }
    } else if (have.n && !existing) {
        fprintf(stderr, "cbase-export: %s already holds %zu items; use --into-existing to put over them\n", work, have.n);
        keyset_free(&have);
        cb_store_close(src);
        cb_store_close(dst);
        return 2;
    }
    keyset_free(&have);

    keyset want;
    rc = keys_of(src, &want);
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-export: scanning %s failed: %s\n", table, cb_strerror(rc));
        keyset_free(&want);
        cb_store_close(src);
        cb_store_close(dst);
        return 2;
    }
    printf("table %s: %zu items\n", table, want.n);

    copy_ctx c = { .dst = dst };
    long vanished = 0;
    for (size_t base = 0; base < want.n; base += 100) {
        int n = want.n - base < 100 ? (int)(want.n - base) : 100;
        cb_ikey ks[100];
        for (int i = 0; i < n; i++) ks[i] = (cb_ikey){ want.k[base + i].pk, want.k[base + i].sk };
        c.keys = want.k + base;
        memset(c.seen, 0, sizeof c.seen);
        rc = src->ops->item_batch_get(src, ks, n, NULL, 0, put_one, &c);
        if (rc != CB_OK) {
            fprintf(stderr, "cbase-export: reading items %zu-%zu failed: %s\n", base, base + (size_t)n, cb_strerror(rc));
            c.failed += n;
            continue;
        }
        for (int i = 0; i < n; i++) if (!c.seen[i]) vanished++;
        if ((base / 100) % 50 == 49 || base + 100 >= want.n)
            printf("  %ld copied of %zu\n", c.copied, want.n);
    }
    if (vanished) printf("%ld keys were gone by the time they were read (deleted or expired during the run)\n", vanished);

    if (c.mismatched) printf("%ld items read back from the file different from the table\n", c.mismatched);
    int exit_code = c.failed || c.mismatched ? 1 : 0;
    if (verify) {
        keyset a, b;
        if (keys_of(src, &a) != CB_OK || keys_of(dst, &b) != CB_OK) {
            fprintf(stderr, "cbase-export: the verification scan failed\n");
            return 2;
        }
        // Two sorted lists: walk them together.
        size_t i = 0, j = 0;
        long missing = 0, extra = 0;
        while (i < a.n || j < b.n) {
            int d = i == a.n ? 1 : j == b.n ? -1 : key_cmp(&a.k[i], &b.k[j]);
            if (d < 0) {
                if (missing++ < 20) printf("MISSING from the file: %s / %s\n", a.k[i].pk, a.k[i].sk);
                i++;
            } else if (d > 0) {
                if (extra++ < 20) printf("EXTRA in the file:     %s / %s\n", b.k[j].pk, b.k[j].sk);
                j++;
            } else { i++; j++; }
        }
        printf("verify: table %zu keys, file %zu keys, %ld missing, %ld extra\n", a.n, b.n, missing, extra);
        if (missing || extra) exit_code = 1;
        if (verify_deep(src, dst, &a, &b) != 0) exit_code = 1;
        keyset_free(&a);
        keyset_free(&b);
    }
    printf("%ld items copied, %ld failed, %ld read back different\n", c.copied, c.failed, c.mismatched);
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

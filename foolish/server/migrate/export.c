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
// the run). Attribute values are not compared, only keys.
//
// A file that already holds items is refused unless --into-existing: the
// export is meant to start a new store, and a put replaces an item.
// --no-verify skips the second scan.
//
// Exit 0: every item copied and the key sets equal. 1: an item failed to copy
// or the key sets differ. 2: a usage or I/O error.
#include "store/store.h"
#include "store/store_priv.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../shared/c/num_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    long      copied, failed;
    bool      seen[100];
    int       base;
} copy_ctx;

static bool put_one(int i, const cb_item *it, void *ud) {
    copy_ctx *c = ud;
    c->seen[i] = true;
    cb_av attrs[CB_ITEM_ATTRS];
    int na = 0;
    for (int a = 0; a < it->n; a++) {
        cb_av v;
        cb_item_at(it, a, &v);
        if (strcmp(v.name, CB_ITEM_PK) == 0 || strcmp(v.name, CB_ITEM_SK) == 0) continue;
        attrs[na++] = v;
    }
    cb_iwrite w;
    memset(&w, 0, sizeof w);
    w.op = CB_IW_PUT;
    w.key = (cb_ikey){ c->keys[i].pk, c->keys[i].sk };
    w.attrs = attrs;
    w.n_attrs = na;
    int rc = c->dst->ops->item_write(c->dst, &w, NULL);
    if (rc == CB_OK) c->copied++;
    else {
        c->failed++;
        fprintf(stderr, "cbase-export: cannot put %s / %s: %s\n", w.key.pk, w.key.sk, cb_strerror(rc));
    }
    return true;
}

static void usage(void) {
    fprintf(stderr,
            "usage: cbase-export [--rate PERCENT] [--into-existing] [--no-verify] <sqlite file>\n"
            "  reads the DynamoDB table CBASE_TABLE (default cbase), read-only, at PERCENT (1-100, default 25)\n"
            "  of its provisioned read capacity, and puts every item into the SQLite file\n");
}

int main(int argc, char **argv) {
    int64_t pct = 25;
    bool existing = false, verify = true;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            if (!num_parse_i64_cstr(argv[++i], 1, 100, &pct)) { usage(); return 2; }
        } else if (strcmp(argv[i], "--into-existing") == 0) existing = true;
        else if (strcmp(argv[i], "--no-verify") == 0) verify = false;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { usage(); return 2; }
    }
    if (!path) { usage(); return 2; }
    cb_log_init(CB_LOG_INFO, 0);
    const char *table = getenv("CBASE_TABLE");
    if (!table || !*table) table = "cbase";

    cb_store *src = NULL, *dst = NULL;
    int rc = cb_store_open_dynamo(table, &src);
    if (rc != CB_OK) { fprintf(stderr, "cbase-export: cannot open the DynamoDB table %s: %s\n", table, cb_strerror(rc)); return 2; }
    rc = cb_store_open_sqlite(path, &dst);
    if (rc != CB_OK) {
        fprintf(stderr, "cbase-export: cannot open %s: %s\n", path, cb_strerror(rc));
        cb_store_close(src);
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
    if (keys_of(dst, &have) != CB_OK) { fprintf(stderr, "cbase-export: cannot list %s\n", path); return 2; }
    if (have.n && !existing) {
        fprintf(stderr, "cbase-export: %s already holds %zu items; use --into-existing to put over them\n", path, have.n);
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

    int exit_code = c.failed ? 1 : 0;
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
        keyset_free(&a);
        keyset_free(&b);
    }
    printf("%ld items copied, %ld failed\n", c.copied, c.failed);
    keyset_free(&want);
    cb_store_close(src);
    cb_store_close(dst);
    return exit_code;
}

// unit_export.c - cbase-export (migrate/export.c) end to end, against DynamoDB Local (`make check-dynamo`,
// CBASE_STORE=dynamo; without it the test skips, saying so).
//
// A table of about a thousand items is made through the item layer (all five attribute kinds, index keys, the TTL
// pair, a 1024-byte pk, a 300 KB item), the real build/cbase-export is run on it, and every item is read back
// out of the SQLite file with item_get and compared attribute by attribute with the table's. Then the exit codes:
// a second run into the same file is refused (2) unless --into-existing (0), a bad --rate is a usage error (2),
// an extra item in the file is reported (1), and an item the SQLite side cannot hold (a name past the layer's
// limit, put straight into DynamoDB Local) is a failure and a MISSING key (1).
//
// argv[1] is the path of build/cbase-export. Run from c_base/ (tsb_create_table reads providers/aws/ops/).
#include "t.h"

#include "aws/aws_call.h"
#include "lib_store_backend.h"
#include "store/store.h"
#include "store/store_priv.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_bin;
static char g_table[64], g_dir[256];
#define N_ITEMS 1000

// ---- the items ---------------------------------------------------------------------------------

typedef struct {
    char pk[1100], sk[16];
    char s[64], g1p[40], g1s[40], g2p[40], g2s[40];
    uint8_t *bytes;
    size_t   blen;
    const char *list[3];
    char l0[16], l1[16], l2[16];
    cb_av attrs[16];
    int n;
} gen;

static void gen_item(gen *g, int i) {
    memset(g, 0, sizeof *g);
    snprintf(g->pk, sizeof g->pk, "EXP#%04d", i);
    snprintf(g->sk, sizeof g->sk, i % 2 ? "ODD" : "EVEN");
    snprintf(g->s, sizeof g->s, "text %d \xc3\xa9 \"q\" \\ end", i);   // quotes, a backslash, a two-byte character
    snprintf(g->g1p, sizeof g->g1p, "G1#%d", i % 10);
    snprintf(g->g1s, sizeof g->g1s, "%08d", i);
    snprintf(g->g2p, sizeof g->g2p, "G2#%d", i % 7);
    snprintf(g->g2s, sizeof g->g2s, "%08d", 1000 - i);
    g->blen = (size_t)(i % 97) + 1;
    g->bytes = malloc(g->blen);
    for (size_t k = 0; k < g->blen; k++) g->bytes[k] = (uint8_t)(k * 31 + i);
    snprintf(g->l0, sizeof g->l0, "a%d", i);
    snprintf(g->l1, sizeof g->l1, "b");
    snprintf(g->l2, sizeof g->l2, "c,d");
    g->list[0] = g->l0; g->list[1] = g->l1; g->list[2] = g->l2;
    int n = 0;
    g->attrs[n++] = CB_AVS("s", g->s);
    g->attrs[n++] = CB_AVN("n", (int64_t)i * 1000003 - 500000000);
    g->attrs[n++] = CB_AVN("big", i % 3 ? INT64_MAX - i : INT64_MIN + i);
    g->attrs[n++] = CB_AVBOOL("flag", i % 5 == 0);
    g->attrs[n++] = CB_AVB("b", g->bytes, g->blen);
    g->attrs[n++] = CB_AVSL("l", g->list, 3);
    g->attrs[n++] = CB_AVS("gsi1pk", g->g1p);
    g->attrs[n++] = CB_AVS("gsi1sk", g->g1s);
    if (i % 2) {
        g->attrs[n++] = CB_AVS("gsi2pk", g->g2p);
        g->attrs[n++] = CB_AVS("gsi2sk", g->g2s);
    }
    g->attrs[n++] = CB_AVN("expires_at", 4102444800LL + i);   // seconds: year 2100, so DynamoDB's TTL leaves it
    g->attrs[n++] = CB_AVN("expires_ms", 4102444800000LL + i);
    g->n = n;
}

static bool put(cb_store *s, const char *pk, const char *sk, const cb_av *attrs, int n) {
    cb_iwrite w;
    memset(&w, 0, sizeof w);
    w.op = CB_IW_PUT;
    w.key = (cb_ikey){ pk, sk };
    w.attrs = attrs;
    w.n_attrs = n;
    int rc = s->ops->item_write(s, &w, NULL);
    if (rc != CB_OK) fprintf(stderr, "     put %.20s: %s\n", pk, cb_strerror(rc));
    return rc == CB_OK;
}

static int g_special;

static void seed(cb_store *s) {
    for (int i = 0; i < N_ITEMS; i++) {
        gen g;
        gen_item(&g, i);
        T_CHECK(put(s, g.pk, g.sk, g.attrs, g.n));
        free(g.bytes);
    }
    // A pk of the layer's longest (1024 bytes), and an item of about 300 KB.
    static char longpk[1100];
    memset(longpk, 'k', 1024);
    longpk[1024] = 0;
    cb_av one[1] = { CB_AVS("s", "long key") };
    T_CHECK(put(s, longpk, "LONG", one, 1));
    uint8_t *big = malloc(300 * 1024);
    for (size_t k = 0; k < 300 * 1024; k++) big[k] = (uint8_t)(k * 7 + k / 251);
    cb_av ba[2] = { CB_AVB("b", big, 300 * 1024), CB_AVS("s", "big") };
    T_CHECK(put(s, "EXP#BIG", "BIG", ba, 2));
    free(big);
    g_special = 2;
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
    while ((k = read(pfd[0], r->out + len, sizeof r->out - 1 - len)) > 0) len += (size_t)k;
    r->out[len] = 0;
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    r->code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

// ---- comparing ------------------------------------------------------------------------------------

static bool same_item(const cb_item *a, const cb_item *b) {
    if (a->n != b->n) return false;
    for (int i = 0; i < a->n; i++) {
        cb_av x, y;
        cb_item_at(a, i, &x);
        if (!cb_item_get(b, x.name, &y) || !cb_av_same(&x, &y)) return false;
    }
    return true;
}

static int compare_every_item(cb_store *table, cb_store *file) {
    int same = 0;
    for (int i = 0; i < N_ITEMS + g_special; i++) {
        char pk[1100], sk[16];
        if (i < N_ITEMS) {
            snprintf(pk, sizeof pk, "EXP#%04d", i);
            snprintf(sk, sizeof sk, i % 2 ? "ODD" : "EVEN");
        } else if (i == N_ITEMS) {
            memset(pk, 'k', 1024);
            pk[1024] = 0;
            snprintf(sk, sizeof sk, "LONG");
        } else {
            snprintf(pk, sizeof pk, "EXP#BIG");
            snprintf(sk, sizeof sk, "BIG");
        }
        cb_item a, b;
        cb_item_init(&a);
        cb_item_init(&b);
        cb_ikey key = { pk, sk };
        bool ok = table->ops->item_get(table, key, true, &a) == CB_OK && file->ops->item_get(file, key, true, &b) == CB_OK
                  && a.n > 0 && same_item(&a, &b);
        if (!ok) fprintf(stderr, "     item %.24s / %s differs between the table and the file\n", pk, sk);
        same += ok;
        cb_item_free(&a);
        cb_item_free(&b);
    }
    return same;
}

static bool raw_put(const char *json) {
    const char *ep = getenv("AWS_ENDPOINT_URL_DYNAMODB");
    char host[256], base[64];
    int port;
    bool tls = true;
    if (!ep || cb_aws_url_parse(ep, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK || tls) return false;
    static const char *const headers[] = { "Content-Type: application/x-amz-json-1.0", "X-Amz-Target: DynamoDB_20120810.PutItem" };
    cb_https_response r;
    int rc = cb_aws_call("dynamodb", "POST", host, port, false, "/", headers, 2, json, strlen(json), 10000, &r);
    bool ok = rc == CB_OK && r.status == 200;
    cb_https_response_free(&r);
    return ok;
}

static void the_export_end_to_end(void) {
    snprintf(g_table, sizeof g_table, "exp_%d_%d", (int)getpid(), rand() % 100000);
    T_EQ_INT(tsb_create_table(g_table), 0);
    cb_store *table = NULL;
    T_EQ_INT(cb_store_open_dynamo(g_table, &table), CB_OK);
    seed(table);

    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/cbase-export-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    T_EQ_INT(mkdir(g_dir, 0700), 0);
    char file[320];
    snprintf(file, sizeof file, "%s/out.sqlite", g_dir);
    static run r;

    // The copy: exit 0, everything counted, every item equal in the file.
    run_export(&r, "--rate", "100", file, NULL);
    if (r.code != 0) fprintf(stderr, "%s\n", r.out);
    T_EQ_INT(r.code, 0);
    char want[160];
    snprintf(want, sizeof want, "table %s: %d items", g_table, N_ITEMS + g_special);
    T_CHECK(strstr(r.out, want) != NULL);
    snprintf(want, sizeof want, "%d items copied, 0 failed, 0 read back different", N_ITEMS + g_special);
    T_CHECK(strstr(r.out, want) != NULL);
    T_CHECK(strstr(r.out, "0 missing, 0 extra") != NULL);
    cb_store *dst = NULL;
    T_EQ_INT(cb_store_open_sqlite(file, &dst), CB_OK);
    T_EQ_INT(compare_every_item(table, dst), N_ITEMS + g_special);
    // The index keys came with them: a query of one group answers the same on both.
    cb_item probe;
    cb_item_init(&probe);
    cb_ikey k7 = { "EXP#0007", "ODD" };
    T_EQ_INT(dst->ops->item_get(dst, k7, true, &probe), CB_OK);
    char g1[40];
    T_CHECK(cb_item_s(&probe, "gsi1pk", g1, sizeof g1) && strcmp(g1, "G1#7") == 0);
    cb_item_free(&probe);

    // A second run into the same file is refused; with --into-existing it puts over and agrees.
    run_export(&r, file, NULL, NULL, NULL);
    T_EQ_INT(r.code, 2);
    T_CHECK(strstr(r.out, "already holds") != NULL);
    run_export(&r, "--into-existing", file, NULL, NULL);
    T_EQ_INT(r.code, 0);
    T_CHECK(strstr(r.out, "0 failed, 0 read back different") != NULL);

    // Usage errors: exit 2, nothing read.
    run_export(&r, "--rate", "0", file, NULL);
    T_EQ_INT(r.code, 2);
    run_export(&r, "--rate", "101", file, NULL);
    T_EQ_INT(r.code, 2);
    run_export(&r, "--rate", "x", file, NULL);
    T_EQ_INT(r.code, 2);
    run_export(&r, "--bogus", file, NULL, NULL);
    T_EQ_INT(r.code, 2);
    run_export(&r, "--rate", NULL, NULL, NULL);
    T_EQ_INT(r.code, 2);
    run_export(&r, NULL, NULL, NULL, NULL);
    T_EQ_INT(r.code, 2);
    T_CHECK(strstr(r.out, "usage:") != NULL);

    // An item in the file the table does not have (deleted from the table, or expired, during a run) is EXTRA: exit 1.
    cb_av ex[1] = { CB_AVS("s", "only in the file") };
    T_CHECK(put(dst, "EXP#EXTRA", "X", ex, 1));
    run_export(&r, "--into-existing", file, NULL, NULL);
    T_EQ_INT(r.code, 1);
    T_CHECK(strstr(r.out, "EXTRA in the file:     EXP#EXTRA / X") != NULL);
    T_CHECK(strstr(r.out, "0 missing, 1 extra") != NULL);
    cb_iwrite del;
    memset(&del, 0, sizeof del);
    del.op = CB_IW_DELETE;
    del.key = (cb_ikey){ "EXP#EXTRA", "X" };
    T_EQ_INT(dst->ops->item_write(dst, &del, NULL), CB_OK);

    // An item the SQLite side cannot hold (an attribute name past the layer's 30 bytes, put straight into
    // DynamoDB Local) is a failure and a MISSING key: exit 1, the rest still copied.
    char json[512];
    snprintf(json, sizeof json,
             "{\"TableName\":\"%s\",\"Item\":{\"pk\":{\"S\":\"EXP#BAD\"},\"sk\":{\"S\":\"BAD\"},"
             "\"an_attribute_name_far_too_long_for_the_layer\":{\"S\":\"x\"}}}", g_table);
    T_CHECK(raw_put(json));
    char file2[320];
    snprintf(file2, sizeof file2, "%s/second.sqlite", g_dir);
    run_export(&r, "--rate", "100", file2, NULL);
    T_EQ_INT(r.code, 1);
    T_CHECK(strstr(r.out, "MISSING from the file: EXP#BAD / BAD") != NULL);
    T_CHECK(strstr(r.out, "1 missing, 0 extra") != NULL);
    T_CHECK(strstr(r.out, "1 failed") != NULL || strstr(r.out, "reading items") != NULL);
    cb_store_close(dst);
    T_EQ_INT(cb_store_open_sqlite(file2, &dst), CB_OK);
    T_EQ_INT(compare_every_item(table, dst), N_ITEMS + g_special);   // every other item made it
    cb_store_close(dst);
    cb_store_close(table);
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

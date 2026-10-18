// lib_item_rec.c - the recording store (lib_item_rec.h).
#include "lib_item_rec.h"

#include "store/rows_auth.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    cb_store  base;
    cb_store *inner;
    cb_buf    log;
    int       refuse, refuse_at;
    bool      refuse_had;
} rec_store;

#define R(s) ((rec_store *)(s))
#define IN(s) (R(s)->inner)

static void av(cb_buf *b, const cb_av *v) {
    switch (v->kind) {
    case CB_AV_S: cb_buf_printf(b, "\"%s\"", v->s ? v->s : ""); break;
    case CB_AV_N: cb_buf_printf(b, "%" PRId64, v->n); break;
    case CB_AV_BOOL: cb_buf_puts(b, v->b ? "true" : "false"); break;
    case CB_AV_B: cb_buf_printf(b, "b%zu", v->len); break;
    case CB_AV_SL:
        cb_buf_putc(b, '[');
        for (int i = 0; i < v->n_list; i++) cb_buf_printf(b, "%s%s", i ? ";" : "", v->list[i]);
        cb_buf_putc(b, ']');
        break;
    default: cb_buf_puts(b, "?"); break;
    }
}

static void cond(cb_buf *b, const cb_iwrite *w) {
    if (!w->n_cond) return;
    cb_buf_puts(b, " if ");
    for (int i = 0; i < w->n_cond; i++) {
        const cb_iclause *c = &w->cond[i];
        if (i) cb_buf_puts(b, " and ");
        switch (c->check) {
        case CB_C_EXISTS: cb_buf_puts(b, "exists"); break;
        case CB_C_ABSENT: cb_buf_puts(b, "absent"); break;
        case CB_C_EQ: cb_buf_printf(b, "%s=", c->v.name); av(b, &c->v); break;
        case CB_C_NE: cb_buf_printf(b, "%s!=", c->v.name); av(b, &c->v); break;
        case CB_C_LT: cb_buf_printf(b, "%s<", c->v.name); av(b, &c->v); break;
        case CB_C_ATTR_EXISTS: cb_buf_printf(b, "has %s", c->v.name); break;
        case CB_C_ABSENT_OR_EQ: cb_buf_printf(b, "%s=", c->v.name); av(b, &c->v); cb_buf_puts(b, "|absent"); break;
        case CB_C_ABSENT_OR_LT: cb_buf_printf(b, "%s<", c->v.name); av(b, &c->v); cb_buf_puts(b, "|absent"); break;
        default: cb_buf_puts(b, "?"); break;
        }
    }
}

static void write_line(cb_buf *b, const cb_iwrite *w) {
    static const char *const OP[] = { "?", "put", "update", "delete", "check" };
    cb_buf_printf(b, "%s %s %s", w->op >= CB_IW_PUT && w->op <= CB_IW_CHECK ? OP[w->op] : "?", w->key.pk, w->key.sk);
    cond(b, w);
    if (w->op == CB_IW_PUT) {
        cb_buf_puts(b, " attrs ");
        for (int i = 0; i < w->n_attrs; i++) {
            cb_buf_printf(b, "%s%s=", i ? "," : "", w->attrs[i].name);
            av(b, &w->attrs[i]);
        }
    } else if (w->op == CB_IW_UPDATE) {
        cb_buf_puts(b, " sets ");
        for (int i = 0; i < w->n_sets; i++) {
            const cb_iset *x = &w->sets[i];
            cb_buf_printf(b, "%s%s", i ? "," : "", x->v.name);
            if (x->how == CB_SET_REMOVE) { cb_buf_putc(b, '-'); continue; }
            cb_buf_puts(b, x->how == CB_SET_IF_ABSENT ? "?=" : x->how == CB_SET_INCR ? "+=" : "=");
            av(b, &x->v);
        }
    }
    if (w->ttl_ms > 0) cb_buf_printf(b, " ttl=%" PRId64, w->ttl_ms);
    if (w->old_on_fail) cb_buf_puts(b, " (old)");
    cb_buf_putc(b, '\n');
}

static int r_item_get(cb_store *s, cb_ikey key, bool consistent, cb_item *out) {
    cb_buf_printf(&R(s)->log, "get %s %s\n", key.pk, key.sk);
    return IN(s)->ops->item_get(IN(s), key, consistent, out);
}

static int r_item_write(cb_store *s, const cb_iwrite *w, cb_item *old) {
    write_line(&R(s)->log, w);
    return IN(s)->ops->item_write(IN(s), w, old);
}

static int r_item_txn(cb_store *s, const cb_iwrite *w, int n, int *failed, bool *failed_had_item) {
    rec_store *r = R(s);
    cb_buf_printf(&r->log, "txn %d\n", n);
    for (int i = 0; i < n; i++) {
        cb_buf_puts(&r->log, "  ");
        write_line(&r->log, &w[i]);
    }
    if (r->refuse > 0) {
        r->refuse--;
        if (failed) *failed = r->refuse_at;
        if (failed_had_item) *failed_had_item = r->refuse_had;
        return CB_E_CONFLICT;
    }
    return IN(s)->ops->item_txn(IN(s), w, n, failed, failed_had_item);
}

static int r_item_query(cb_store *s, const cb_iquery *q, cb_item_fn fn, void *ud) {
    cb_buf *b = &R(s)->log;
    static const char *const IX[] = { "t", "g1", "g2" };
    cb_buf_printf(b, "query %s %s", q->index >= 0 && q->index <= 2 ? IX[q->index] : "?", q->pk);
    if (q->sk_prefix) cb_buf_printf(b, " ^%s", q->sk_prefix);
    if (q->sk_below) cb_buf_printf(b, " <%s", q->sk_below);
    cb_buf_puts(b, q->forward ? " fwd" : " back");
    if (q->keys_only) cb_buf_puts(b, " keys");
    if (q->page) cb_buf_printf(b, " page=%d", q->page);
    cb_buf_putc(b, '\n');
    return IN(s)->ops->item_query(IN(s), q, fn, ud);
}

static int r_item_batch_get(cb_store *s, const cb_ikey *keys, int n, const char *const *proj, int n_proj,
                            cb_item_at_fn fn, void *ud) {
    cb_buf *b = &R(s)->log;
    cb_buf_printf(b, "batch_get %d", n);
    if (proj) {
        cb_buf_puts(b, " proj ");
        for (int i = 0; i < n_proj; i++) cb_buf_printf(b, "%s%s", i ? "," : "", proj[i]);
    }
    cb_buf_putc(b, '\n');
    for (int i = 0; i < n; i++) cb_buf_printf(b, "  %s %s\n", keys[i].pk, keys[i].sk);
    return IN(s)->ops->item_batch_get(IN(s), keys, n, proj, n_proj, fn, ud);
}

static int r_item_batch_delete(cb_store *s, const cb_ikey *keys, int n) {
    cb_buf *b = &R(s)->log;
    cb_buf_printf(b, "batch_delete %d\n", n);
    for (int i = 0; i < n; i++) cb_buf_printf(b, "  %s %s\n", keys[i].pk, keys[i].sk);
    return IN(s)->ops->item_batch_delete(IN(s), keys, n);
}

static int r_item_keys(cb_store *s, cb_ikey_fn fn, void *ud) {
    cb_buf_puts(&R(s)->log, "keys\n");
    return IN(s)->ops->item_keys(IN(s), fn, ud);
}

static int r_admin_collections(cb_store *s, cb_name_fn fn, void *ud) {
    return IN(s)->ops->admin_collections(IN(s), fn, ud);
}
static int r_admin_browse(cb_store *s, const char *c, const char *prefix, const char *cursor, int limit,
                          cb_admin_row_fn fn, void *ud, char *next, size_t next_cap) {
    return IN(s)->ops->admin_browse(IN(s), c, prefix, cursor, limit, fn, ud, next, next_cap);
}
static int r_admin_query(cb_store *s, const char *statement, bool allow_write, cb_admin_row_fn fn, void *ud) {
    return IN(s)->ops->admin_query(IN(s), statement, allow_write, fn, ud);
}

static void r_close(cb_store *s) {
    cb_store_close(IN(s));
    cb_buf_free(&R(s)->log);
    free(s);
}

static const cb_store_ops REC_OPS = {
    .name = "rec",
    .close = r_close,
    CB_ROWS_AUTH_OPS
    .admin_collections = r_admin_collections,
    .admin_browse = r_admin_browse,
    .admin_query = r_admin_query,
    .item_get = r_item_get,
    .item_write = r_item_write,
    .item_txn = r_item_txn,
    .item_query = r_item_query,
    .item_batch_get = r_item_batch_get,
    .item_batch_delete = r_item_batch_delete,
    .item_keys = r_item_keys,
};

cb_store *ir_wrap(cb_store *inner) {
    rec_store *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->base.ops = &REC_OPS;
    r->base.app = &cb_foolish_store;
    r->inner = inner;
    cb_buf_init(&r->log);
    return &r->base;
}

cb_store *ir_inner(cb_store *s) { return IN(s); }

const char *ir_log(cb_store *s) { return cb_buf_cstr(&R(s)->log); }
void ir_clear(cb_store *s) { cb_buf_reset(&R(s)->log); }

// Each line, its indent skipped unless the prefix asks for it.
static const char *next_match(const char *p, const char *prefix) {
    size_t pl = strlen(prefix);
    while (*p) {
        const char *q = p;
        if (prefix[0] != ' ')
            while (*q == ' ') q++;
        if (strncmp(q, prefix, pl) == 0) return p;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return NULL;
}

int ir_count(cb_store *s, const char *prefix) {
    int n = 0;
    for (const char *p = ir_log(s); (p = next_match(p, prefix)) != NULL; n++) {
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

const char *ir_line(cb_store *s, const char *prefix) {
    static char line[8192];
    const char *p = next_match(ir_log(s), prefix);
    if (!p) return NULL;
    size_t n = strcspn(p, "\n");
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, p, n);
    line[n] = '\0';
    return line;
}

bool ir_has(cb_store *s, const char *text) { return strstr(ir_log(s), text) != NULL; }

void ir_refuse_txns(cb_store *s, int n, int failed, bool had) {
    R(s)->refuse = n;
    R(s)->refuse_at = failed;
    R(s)->refuse_had = had;
}

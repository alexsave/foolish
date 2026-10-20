// journal.c - see journal.h.
#include "store/journal.h"
#include "store/store_sql.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "../../../../shared/c/fnv.h"
#include "../../../../shared/c/num_parse.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOD "journal"
#define SEG_MAGIC "FJNL"
#define SEG_HDR 16
#define FORMAT 1
#define BATCH_MAGIC 'J'
#define OV_BUCKETS 1024
#define IDLE_EVICT_MS 60000

// ---- the slot table ------------------------------------------------------------------------

struct cb_jslots {
    int n, cap;
    char (*ids)[CB_GAME_ID_MAX + 1];
    int64_t *next;    // the version the slot's next record continues from
    int *hash;        // open addressing: slot + 1, 0 empty
    int hcap;
};

cb_jslots *cb_jslots_new(void) { return calloc(1, sizeof(cb_jslots)); }

void cb_jslots_free(cb_jslots *t) {
    if (!t) return;
    free(t->ids);
    free(t->next);
    free(t->hash);
    free(t);
}

int cb_jslots_count(const cb_jslots *t) { return t->n; }

static int slot_find(const cb_jslots *t, const char *id) {
    if (!t->hcap) return -1;
    for (uint32_t h = fnv1a32_str(FNV32_OFFSET, id) & (uint32_t)(t->hcap - 1);; h = (h + 1) & (uint32_t)(t->hcap - 1)) {
        int v = t->hash[h];
        if (!v) return -1;
        if (strcmp(t->ids[v - 1], id) == 0) return v - 1;
    }
}

static void hash_put(cb_jslots *t, int slot) {
    uint32_t h = fnv1a32_str(FNV32_OFFSET, t->ids[slot]) & (uint32_t)(t->hcap - 1);
    while (t->hash[h]) h = (h + 1) & (uint32_t)(t->hcap - 1);
    t->hash[h] = slot + 1;
}

static int slot_add(cb_jslots *t, const char *id, int64_t next) {
    if (t->n + 1 >= REPLAY_CODER_MAX_N) return CB_E_FULL;
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 64;
        void *a = realloc(t->ids, (size_t)cap * sizeof *t->ids), *b = a ? realloc(t->next, (size_t)cap * sizeof *t->next) : NULL;
        if (a) t->ids = a;
        if (!b) return CB_E_NOMEM;
        t->next = b;
        t->cap = cap;
    }
    if ((t->n + 1) * 2 > t->hcap) {
        int hcap = t->hcap ? t->hcap * 2 : 128;
        int *h = calloc((size_t)hcap, sizeof *h);
        if (!h) return CB_E_NOMEM;
        free(t->hash);
        t->hash = h;
        t->hcap = hcap;
        for (int i = 0; i < t->n; i++) hash_put(t, i);
    }
    snprintf(t->ids[t->n], sizeof t->ids[t->n], "%s", id);
    t->next[t->n] = next;
    hash_put(t, t->n);
    return t->n++;
}

// ---- the batch codec -----------------------------------------------------------------------

static uint32_t zigzag(int64_t v) { return (uint32_t)((v << 1) ^ (v >> 63)); }
static int64_t unzigzag(uint32_t z) { return (int64_t)(z >> 1) ^ -(int64_t)(z & 1); }

static void put_u16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

static uint32_t batch_sum(const uint8_t *hdr, const uint8_t *payload, size_t n) {
    return fnv1a32(fnv1a32(FNV32_OFFSET, hdr, 4), payload, n);
}

int cb_journal_batch_encode(cb_jslots *t, int64_t seg_start_ms, const cb_jmove *m, int n, uint8_t *out, int cap) {
    if (n < 1 || n > CB_JOURNAL_BATCH_MAX || cap < CB_JOURNAL_BATCH_HDR) return CB_E_INVALID;
    int64_t base = m[0].now_ms;
    for (int i = 1; i < n; i++) if (m[i].now_ms < base) base = m[i].now_ms;
    const int64_t rel = base - seg_start_ms;
    if (rel > INT32_MAX || rel < INT32_MIN) return CB_E_INVALID;   // a segment lives seconds: the caller starts a new one
    ReplayCoder c;
    if (replay_coder_begin_encode(&c) != 0) return CB_E_NOMEM;
    replay_coder_varint(&c, (uint32_t)n);
    replay_coder_varint(&c, zigzag(rel));
    for (int i = 0; i < n && !c.err; i++) {
        const cb_jmove *r = &m[i];
        if (r->base_version < 0 || r->base_version > UINT32_MAX - 1 || r->now_ms - base > UINT32_MAX) return CB_E_INVALID;
        const int N = t->n;
        int slot = slot_find(t, r->game);
        replay_coder_uniform(&c, N + 1, slot >= 0 ? slot : N);
        if (slot < 0) {
            const int len = (int)strlen(r->game);
            if (len < 1 || len > CB_GAME_ID_MAX) return CB_E_INVALID;
            replay_coder_uniform(&c, CB_GAME_ID_MAX + 1, len);
            for (int k = 0; k < len; k++) replay_coder_uniform(&c, 256, (uint8_t)r->game[k]);
            replay_coder_varint(&c, (uint32_t)r->base_version);
            if ((slot = slot_add(t, r->game, r->base_version)) < 0) return slot;
        } else {
            const bool follows = t->next[slot] == r->base_version;
            replay_coder_uniform(&c, 2, follows ? 0 : 1);
            if (!follows) replay_coder_varint(&c, (uint32_t)r->base_version);
        }
        t->next[slot] = r->base_version + 1;
        replay_coder_varint(&c, (uint32_t)(r->now_ms - base));
        ReplayMove mv = r->mv;
        replay_move_code(&c, &mv);
    }
    int len = replay_coder_finish(&c, out + CB_JOURNAL_BATCH_HDR, cap - CB_JOURNAL_BATCH_HDR);
    if (len < 0 || len > 0xFFFF) return CB_E_FULL;
    out[0] = BATCH_MAGIC;
    out[1] = FORMAT;
    put_u16(out + 2, (uint32_t)len);
    put_u32(out + 4, batch_sum(out, out + CB_JOURNAL_BATCH_HDR, (size_t)len));
    return CB_JOURNAL_BATCH_HDR + len;
}

int cb_journal_batch_decode(cb_jslots *t, int64_t seg_start_ms, const uint8_t *in, size_t len, size_t *used,
                            cb_jmove *out, int cap) {
    if (len < CB_JOURNAL_BATCH_HDR || in[0] != BATCH_MAGIC || in[1] != FORMAT) return -1;
    const size_t plen = get_u16(in + 2);
    if (CB_JOURNAL_BATCH_HDR + plen > len) return -1;   // torn
    if (get_u32(in + 4) != batch_sum(in, in + CB_JOURNAL_BATCH_HDR, plen)) return -1;
    ReplayCoder c;
    if (replay_coder_begin_decode(&c, in + CB_JOURNAL_BATCH_HDR, (int)plen) != 0) return -1;
    const uint32_t n = replay_coder_varint(&c, 0);
    const int64_t base = seg_start_ms + unzigzag(replay_coder_varint(&c, 0));
    if (c.err || n < 1 || n > (uint32_t)cap || n > CB_JOURNAL_BATCH_MAX) return -1;
    for (uint32_t i = 0; i < n; i++) {
        cb_jmove *r = &out[i];
        memset(r, 0, sizeof *r);
        const int N = t->n;
        int slot = replay_coder_uniform(&c, N + 1, -1);
        if (c.err) return -1;
        if (slot == N) {
            const int idlen = replay_coder_uniform(&c, CB_GAME_ID_MAX + 1, -1);
            if (idlen < 1) return -1;
            for (int k = 0; k < idlen; k++) r->game[k] = (char)replay_coder_uniform(&c, 256, -1);
            if (c.err || (int)strlen(r->game) != idlen) return -1;
            r->base_version = replay_coder_varint(&c, 0);
            if (c.err || slot_find(t, r->game) >= 0 || (slot = slot_add(t, r->game, r->base_version)) < 0) return -1;
        } else {
            snprintf(r->game, sizeof r->game, "%s", t->ids[slot]);
            r->base_version = replay_coder_uniform(&c, 2, -1) ? replay_coder_varint(&c, 0) : t->next[slot];
        }
        t->next[slot] = r->base_version + 1;
        r->now_ms = base + replay_coder_varint(&c, 0);
        if (replay_move_code(&c, &r->mv) != 0) return -1;
    }
    if (c.err || !replay_coder_exhausted(&c)) return -1;
    *used = CB_JOURNAL_BATCH_HDR + plen;
    return (int)n;
}

// ---- the overlay: each journaled game's acknowledged state ----------------------------------

typedef struct jgame {
    struct jgame *next;
    cb_game_row   row;           // the newest acknowledged row (no logs)
    int64_t       sql_version;   // the version the tables hold
    cb_game_move *moves;         // the moves since sql_version, in order (each log owned)
    int           n_moves, cap_moves;
    int64_t       touched_ms;    // cb_mono_ms of its last move
} jgame;

// A move waiting for its batch's write.
typedef struct jrec {
    struct jrec *next;
    cb_jmove     m;
    cb_game_row  row;           // the new row (owned)
    uint8_t     *log;
    size_t       log_len;
    int          rc;
    bool         done;
} jrec;

struct cb_journal {
    cb_store       *s;
    cb_journal_opts o;
    char            prefix[1100];   // <db>-journal.
    // the writer: the queue and the one batch in flight
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    jrec           *head, *tail;
    bool            writing;
    int             fd;
    uint64_t        seg_seq;       // the open segment's number
    uint64_t        first_seq;     // the oldest segment on disk
    int64_t         seg_start;
    off_t           seg_len;
    cb_jslots      *slots;
    // the overlay
    pthread_mutex_t ov_mu;
    jgame          *buckets[OV_BUCKETS];
    long            n_waiting;     // moves not in the tables yet
    // the flush: one at a time, and every write of a journaled game around the journal
    pthread_mutex_t flush_mu;
    pthread_t       thr;
    bool            thr_started, stop, kick;
    // stats
    atomic_long     batches, writes, records, bytes, flushes, flushed_games, flushed_moves, recovered;
};

cb_journal *cb_journal_of(const cb_store *s) { return s && s->app_settle == cb_journal_flush ? s->app_state : NULL; }

static jgame **ov_slot(cb_journal *j, const char *id) {
    jgame **p = &j->buckets[fnv1a32_str(FNV32_OFFSET, id) & (OV_BUCKETS - 1)];
    while (*p && strcmp((*p)->row.id, id) != 0) p = &(*p)->next;
    return p;
}

static void game_free(jgame *g) {
    cb_game_row_free(&g->row);
    for (int i = 0; i < g->n_moves; i++) free((void *)g->moves[i].log);
    free(g->moves);
    free(g);
}

static uint8_t *dup_bytes(const void *p, size_t n) {
    uint8_t *d = malloc(n ? n : 1);
    if (d && n) memcpy(d, p, n);
    return d;
}

static int row_dup(cb_game_row *dst, const cb_game_row *src) {
    *dst = *src;
    dst->logs = NULL;
    dst->logs_len = 0;
    dst->state = dup_bytes(src->state, src->state_len);
    dst->roster = dup_bytes(src->roster, src->roster_len);
    if (!dst->state || !dst->roster) {
        cb_game_row_free(dst);
        return CB_E_NOMEM;
    }
    return CB_OK;
}

// A record's row and log become the game's (ov_mu held). The record's buffers are taken.
static int ov_apply(cb_journal *j, jrec *r) {
    jgame **p = ov_slot(j, r->m.game), *g = *p;
    if (!g) {
        if (!(g = calloc(1, sizeof *g))) return CB_E_NOMEM;
        g->sql_version = r->m.base_version;
        *p = g;
    }
    if (g->n_moves == g->cap_moves) {
        int cap = g->cap_moves ? g->cap_moves * 2 : 8;
        cb_game_move *m = realloc(g->moves, (size_t)cap * sizeof *m);
        if (!m) return CB_E_NOMEM;
        g->moves = m;
        g->cap_moves = cap;
    }
    g->moves[g->n_moves++] = (cb_game_move){ r->row.version, r->log, r->log_len };
    r->log = NULL;
    cb_game_row_free(&g->row);
    g->row = r->row;
    memset(&r->row, 0, sizeof r->row);
    g->touched_ms = cb_mono_ms();
    j->n_waiting++;
    return CB_OK;
}

// ---- segments ------------------------------------------------------------------------------

static void seg_path(const cb_journal *j, uint64_t seq, char *out, size_t cap) {
    snprintf(out, cap, "%s%010llu", j->prefix, (unsigned long long)seq);
}

static bool write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

// A new, empty segment after the open one (closed); its header goes with its first batch,
// which gives it its start time. The writer is held.
static int seg_open(cb_journal *j, uint64_t seq) {
    char path[1200];
    seg_path(j, seq, path, sizeof path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        CB_ERROR(MOD, "open %s: %s", path, strerror(errno));
        return CB_E_IO;
    }
    cb_jslots *t = cb_jslots_new();
    if (!t) {
        close(fd);
        unlink(path);
        return CB_E_NOMEM;
    }
    if (j->fd >= 0) close(j->fd);
    cb_jslots_free(j->slots);
    j->fd = fd;
    j->slots = t;
    j->seg_seq = seq;
    j->seg_start = 0;
    j->seg_len = 0;
    return CB_OK;
}

// Codes and writes one batch of records (the writer is held, j->mu is not). Each record's rc set.
static void write_batch(cb_journal *j, jrec *list) {
    int n = 0;
    for (jrec *r = list; r; r = r->next) n++;
    // A batch is at most CB_JOURNAL_BATCH_MAX moves; more go as several, in the same write.
    const size_t cap = SEG_HDR + (size_t)(n / CB_JOURNAL_BATCH_MAX + 1) * (CB_JOURNAL_BATCH_HDR + REPLAY_MAX_INT_BYTES);
    uint8_t *buf = malloc(cap);
    cb_jmove *ms = malloc(sizeof *ms * (size_t)(n < CB_JOURNAL_BATCH_MAX ? n : CB_JOURNAL_BATCH_MAX));
    int rc = buf && ms && j->fd >= 0 ? CB_OK : j->fd < 0 ? CB_E_IO : CB_E_NOMEM;
    size_t len = 0;
    int frames = 0;
    int64_t first = list->m.now_ms;
    for (jrec *r = list; r; r = r->next) if (r->m.now_ms < first) first = r->m.now_ms;
    // A batch's time is coded against its segment's start: one a clock jump put out of
    // reach starts a segment of its own.
    if (rc == CB_OK && j->seg_len > 0 && (first - j->seg_start > INT32_MAX / 2 || first - j->seg_start < INT32_MIN / 2) &&
        seg_open(j, j->seg_seq + 1) != CB_OK)
        rc = CB_E_IO;
    if (rc == CB_OK && j->seg_len == 0) {
        memcpy(buf, SEG_MAGIC, 4);
        put_u32(buf + 4, FORMAT);
        put_u64(buf + 8, (uint64_t)first);
        j->seg_start = first;
        len = SEG_HDR;
    }
    for (jrec *r = list; rc == CB_OK && r;) {
        int k = 0;
        for (; r && k < CB_JOURNAL_BATCH_MAX; r = r->next) ms[k++] = r->m;
        int w = cb_journal_batch_encode(j->slots, j->seg_start, ms, k, buf + len, (int)(cap - len));
        if (w < 0) rc = w;
        else len += (size_t)w, frames++;
    }
    if (rc == CB_OK && !write_all(j->fd, buf, len)) {
        CB_ERROR(MOD, "write: %s", strerror(errno));
        rc = CB_E_IO;
    }
    if (rc == CB_OK && j->o.sync && fdatasync(j->fd) != 0) {
        CB_ERROR(MOD, "fdatasync: %s", strerror(errno));
        rc = CB_E_IO;
    }
    if (rc == CB_OK) {
        j->seg_len += (off_t)len;
        atomic_fetch_add(&j->batches, frames);
        atomic_fetch_add(&j->writes, 1);
        atomic_fetch_add(&j->records, n);
        atomic_fetch_add(&j->bytes, (long)len);
    } else {
        // Nothing of this batch is acknowledged: the segment is cut back to its last whole
        // batch, and the slot table that may have moved on goes with a new segment.
        if (j->fd >= 0 && ftruncate(j->fd, j->seg_len) != 0) CB_ERROR(MOD, "ftruncate: %s", strerror(errno));
        if (j->fd >= 0) (void)lseek(j->fd, j->seg_len, SEEK_SET);
        if (seg_open(j, j->seg_seq + 1) != CB_OK) {
            if (j->fd >= 0) close(j->fd);
            j->fd = -1;   // every later move fails until a flush opens one
        }
    }
    free(buf);
    free(ms);
    pthread_mutex_lock(&j->ov_mu);
    for (jrec *r = list; r; r = r->next) {
        r->rc = rc;
        if (rc == CB_OK && (r->rc = ov_apply(j, r)) != CB_OK) CB_ERROR(MOD, "game %s: overlay out of memory", r->m.game);
    }
    const bool full = j->n_waiting >= j->o.flush_moves;
    pthread_mutex_unlock(&j->ov_mu);
    if (full) {
        pthread_mutex_lock(&j->mu);
        j->kick = true;
        pthread_cond_broadcast(&j->cv);
        pthread_mutex_unlock(&j->mu);
    }
}

// Appends r and returns once its batch is written: the first waiter with no batch in flight
// writes every record queued (group commit).
static int append(cb_journal *j, jrec *r) {
    pthread_mutex_lock(&j->mu);
    if (j->tail) j->tail->next = r;
    else j->head = r;
    j->tail = r;
    while (!r->done) {
        if (!j->writing) {
            jrec *list = j->head;
            j->head = j->tail = NULL;
            j->writing = true;
            pthread_mutex_unlock(&j->mu);
            write_batch(j, list);
            pthread_mutex_lock(&j->mu);
            for (jrec *x = list; x; x = x->next) x->done = true;
            j->writing = false;
            pthread_cond_broadcast(&j->cv);
        } else {
            pthread_cond_wait(&j->cv, &j->mu);
        }
    }
    pthread_mutex_unlock(&j->mu);
    return r->rc;
}

// ---- the flush -----------------------------------------------------------------------------

// The writer, held (no batch in flight while the caller rotates).
static void writer_hold(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    while (j->writing) pthread_cond_wait(&j->cv, &j->mu);
    j->writing = true;
    pthread_mutex_unlock(&j->mu);
}

static void writer_release(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    j->writing = false;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
}

static void delete_segments_before(cb_journal *j, uint64_t seq) {
    char path[1200];
    for (uint64_t k = j->first_seq; k < seq; k++) {
        seg_path(j, k, path, sizeof path);
        (void)unlink(path);
    }
    if (seq > j->first_seq) j->first_seq = seq;
}

typedef struct {
    cb_game_flush *f;
    cb_game_row   *rows;
    int            n;
} snapshot;

static void snapshot_free(snapshot *sn) {
    for (int i = 0; i < sn->n; i++) {
        cb_game_row_free(&sn->rows[i]);
        free((void *)sn->f[i].moves);
    }
    free(sn->f);
    free(sn->rows);
    memset(sn, 0, sizeof *sn);
}

// Every game with moves waiting, copied (ov_mu held): rows and the move lists (the logs stay
// the overlay's; only a flush frees them, and the flush lock is held).
static int snapshot_take(cb_journal *j, snapshot *sn) {
    memset(sn, 0, sizeof *sn);
    int n = 0;
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next) n += g->n_moves > 0;
    if (!n) return CB_OK;
    sn->f = calloc((size_t)n, sizeof *sn->f);
    sn->rows = calloc((size_t)n, sizeof *sn->rows);
    if (!sn->f || !sn->rows) { snapshot_free(sn); return CB_E_NOMEM; }
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next) {
            if (!g->n_moves) continue;
            cb_game_move *m = malloc((size_t)g->n_moves * sizeof *m);
            if (!m || row_dup(&sn->rows[sn->n], &g->row) != CB_OK) { free(m); snapshot_free(sn); return CB_E_NOMEM; }
            memcpy(m, g->moves, (size_t)g->n_moves * sizeof *m);
            sn->f[sn->n] = (cb_game_flush){ &sn->rows[sn->n], g->sql_version, m, g->n_moves };
            sn->n++;
        }
    return CB_OK;
}

// The tables now hold f: its moves leave the game (ov_mu held).
static void ov_flushed(cb_journal *j, jgame *g, const cb_game_flush *f) {
    int k = 0;
    while (k < g->n_moves && g->moves[k].seq <= f->row->version) free((void *)g->moves[k++].log);
    memmove(g->moves, g->moves + k, (size_t)(g->n_moves - k) * sizeof *g->moves);
    g->n_moves -= k;
    j->n_waiting -= k;
    g->sql_version = f->row->version;
}

// Every acknowledged move into the tables, and the segments that held them deleted.
static int flush_all(cb_journal *j) {
    pthread_mutex_lock(&j->flush_mu);
    writer_hold(j);
    snapshot sn;
    pthread_mutex_lock(&j->ov_mu);
    int rc = snapshot_take(j, &sn);
    pthread_mutex_unlock(&j->ov_mu);
    // The segments up to here hold nothing the snapshot does not: a new one takes the moves
    // that come during the transaction.
    uint64_t cut = 0;
    if (rc == CB_OK && (j->seg_len > 0 || j->fd < 0)) {
        if (seg_open(j, j->seg_seq + 1) == CB_OK) cut = j->seg_seq;
        else rc = CB_E_IO;
    }
    writer_release(j);
    if (rc == CB_OK && sn.n) rc = cb_rows_sql_flush(j->s, sn.f, sn.n, NULL, NULL, 0);
    if (rc == CB_OK) {
        long moves = 0;
        const int64_t now = cb_mono_ms();
        pthread_mutex_lock(&j->ov_mu);
        for (int i = 0; i < sn.n; i++) {
            jgame *g = *ov_slot(j, sn.rows[i].id);
            if (g) ov_flushed(j, g, &sn.f[i]);
            moves += sn.f[i].n_moves;
        }
        for (int b = 0; b < OV_BUCKETS; b++)   // a game idle and in the tables leaves the overlay
            for (jgame **p = &j->buckets[b]; *p;) {
                jgame *g = *p;
                if (!g->n_moves && now - g->touched_ms > IDLE_EVICT_MS) { *p = g->next; game_free(g); }
                else p = &g->next;
            }
        pthread_mutex_unlock(&j->ov_mu);
        if (cut) delete_segments_before(j, cut);
        if (sn.n) {
            atomic_fetch_add(&j->flushes, 1);
            atomic_fetch_add(&j->flushed_games, sn.n);
            atomic_fetch_add(&j->flushed_moves, moves);
        }
    } else {
        CB_ERROR(MOD, "flush failed (%s): the moves stay in the journal and the next flush retries", cb_strerror(rc));
    }
    snapshot_free(&sn);
    pthread_mutex_unlock(&j->flush_mu);
    return rc;
}

int cb_journal_flush(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    return j ? flush_all(j) : CB_OK;
}

static void *flusher(void *ud) {
    cb_journal *j = ud;
    pthread_mutex_lock(&j->mu);
    while (!j->stop) {
        // Until the period is up or enough moves wait (the batches' broadcasts wake it too).
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t ns = ts.tv_nsec + (j->o.flush_ms % 1000) * 1000000;
        ts.tv_sec += (time_t)(j->o.flush_ms / 1000 + ns / 1000000000);
        ts.tv_nsec = (long)(ns % 1000000000);
        while (!j->stop && !j->kick && pthread_cond_timedwait(&j->cv, &j->mu, &ts) != ETIMEDOUT) {}
        if (j->stop) break;
        j->kick = false;
        pthread_mutex_unlock(&j->mu);
        (void)flush_all(j);
        pthread_mutex_lock(&j->mu);
    }
    pthread_mutex_unlock(&j->mu);
    return NULL;
}

// ---- the operations the journal answers ------------------------------------------------------

static cb_rows_ops J_OPS;

// A row from the overlay, copied. CB_E_NOT_FOUND: not held.
static int ov_get(cb_journal *j, const char *id, cb_game_row *out) {
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, id);
    int rc = g ? row_dup(out, &g->row) : CB_E_NOT_FOUND;
    pthread_mutex_unlock(&j->ov_mu);
    if (rc == CB_OK) out->seats_exact = true;   // a journaled move never changes the seats
    return rc;
}

static int j_game_get(cb_store *s, const char *id, cb_game_row *out) {
    memset(out, 0, sizeof *out);
    int rc = ov_get(cb_journal_of(s), id, out);
    return rc == CB_E_NOT_FOUND ? cb_rows_sql.game_get(s, id, out) : rc;
}

// The tables' log (every move up to sql_version), then the overlay's: the flush lock keeps a
// flush from moving moves between the two while they are read.
static int j_game_logs(cb_store *s, const char *id, int64_t log_from, uint8_t **out, size_t *out_len) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->flush_mu);
    int rc = cb_rows_sql.game_logs(s, id, log_from, out, out_len);
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = rc == CB_OK && log_from > 0 ? *ov_slot(j, id) : NULL;
    size_t add = 0;
    for (int i = 0; g && i < g->n_moves; i++) if (g->moves[i].seq >= log_from) add += g->moves[i].len;
    if (add) {
        uint8_t *p = realloc(*out, *out_len + add);
        if (!p) rc = CB_E_NOMEM;
        else {
            *out = p;
            for (int i = 0; i < g->n_moves; i++)
                if (g->moves[i].seq >= log_from) {
                    memcpy(p + *out_len, g->moves[i].log, g->moves[i].len);
                    *out_len += g->moves[i].len;
                }
        }
    }
    pthread_mutex_unlock(&j->ov_mu);
    pthread_mutex_unlock(&j->flush_mu);
    if (rc != CB_OK) {
        free(*out);
        *out = NULL;
        *out_len = 0;
    }
    return rc;
}

// A write of one game around the journal: its waiting moves first, in the same transaction,
// and the game leaves the overlay (the row cache and the tables have it). The caller holds
// the game's lock, so no move of it is in flight.
static int around(cb_store *s, const char *id, const cb_game_commit *c, const char *del_id, int64_t del_version) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->flush_mu);
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, id);
    pthread_mutex_unlock(&j->ov_mu);
    // g is stable here: only a flush (held off) or this path frees an entry, and only a move
    // of this game (its lock is the caller's) changes it.
    cb_game_flush f = { 0 };
    if (g) f = (cb_game_flush){ &g->row, g->sql_version, g->moves, g->n_moves };
    int rc = cb_rows_sql_flush(s, &f, g && g->n_moves ? 1 : 0, c, del_id, del_version);
    if (rc == CB_OK && g) {
        pthread_mutex_lock(&j->ov_mu);
        jgame **p = ov_slot(j, id);
        *p = g->next;
        j->n_waiting -= g->n_moves;
        atomic_fetch_add(&j->flushed_moves, g->n_moves);
        pthread_mutex_unlock(&j->ov_mu);
        game_free(g);
    }
    pthread_mutex_unlock(&j->flush_mu);
    return rc;
}

static int j_game_commit(cb_store *s, const cb_game_commit *c) {
    cb_journal *j = cb_journal_of(s);
    if (!c || !c->row) return CB_E_INVALID;
    const cb_game_row *row = c->row;
    if (!c->move) return around(s, row->id, c, NULL, 0);
    // The fence the tables would apply: the game's acknowledged version is the expected one.
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, row->id);
    int64_t held = g ? g->row.version : -1;
    pthread_mutex_unlock(&j->ov_mu);
    if (held < 0) {
        cb_game_row cur;
        int rc = cb_rows_sql.game_get(s, row->id, &cur);
        if (rc != CB_OK) return rc;
        held = cur.version;
        cb_game_row_free(&cur);
    }
    if (held != c->expected_version) return CB_E_CONFLICT;
    jrec r;
    memset(&r, 0, sizeof r);
    snprintf(r.m.game, sizeof r.m.game, "%s", row->id);
    r.m.base_version = c->expected_version;
    r.m.now_ms = row->updated_at;
    r.m.mv = *c->move;
    if (row_dup(&r.row, row) != CB_OK) return CB_E_NOMEM;
    if (c->log_add_len && !(r.log = dup_bytes(c->log_add, c->log_add_len))) {
        cb_game_row_free(&r.row);
        return CB_E_NOMEM;
    }
    r.log_len = c->log_add_len;
    int rc = append(j, &r);
    cb_game_row_free(&r.row);   // taken by the overlay on success
    free(r.log);
    return rc;
}

static int j_game_delete(cb_store *s, const char *id, int64_t expected) { return around(s, id, NULL, id, expected); }

// THE SCAN RULE: a read over the games table (not one game by its id) settles the journal
// first. Its one owner: every scan of the tables the journal can be ahead of comes here.
static int journal_scan(cb_store *s) { return flush_all(cb_journal_of(s)); }

static int j_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                                void *ud) {
    int rc = journal_scan(s);
    return rc != CB_OK ? rc : cb_rows_sql.games_needing_bots(s, stale_before, active_after, limit, fn, ud);
}

static int j_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud) {
    int rc = journal_scan(s);
    return rc != CB_OK ? rc : cb_rows_sql.games_of_user(s, user, limit, fn, ud);
}

static int j_game_import(cb_store *s, const cb_game_row *g, const cb_uuid *humans, int n_humans, const cb_uuid *bots,
                         int n_bots) {
    int rc = journal_scan(s);
    return rc != CB_OK ? rc : cb_rows_sql.game_import(s, g, humans, n_humans, bots, n_bots);
}

typedef struct {
    cb_journal *j;
    cb_game_fn  fn;
    void       *ud;
} views_ud;

static bool view_swap(const cb_game_row *r, void *ud) {
    views_ud *v = ud;
    cb_game_row held;
    if (ov_get(v->j, r->id, &held) != CB_OK) return v->fn(r, v->ud);
    bool more = v->fn(&held, v->ud);
    cb_game_row_free(&held);
    return more;
}

static int j_games_get_views(cb_store *s, const char *const *ids, int n, cb_game_fn fn, void *ud) {
    views_ud v = { cb_journal_of(s), fn, ud };
    return cb_rows_sql.games_get_views(s, ids, n, view_swap, &v);
}

const cb_rows_ops *cb_journal_rows(void) { return &J_OPS; }

// ---- open: recovery ------------------------------------------------------------------------

static int seq_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// The segments on disk, in order (malloc'd).
static int list_segments(const cb_journal *j, uint64_t **out, int *n) {
    *out = NULL;
    *n = 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", j->prefix);
    char *slash = strrchr(dir, '/');
    char name[300];
    snprintf(name, sizeof name, "%s", slash ? j->prefix + (slash - dir) + 1 : j->prefix);
    if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    DIR *d = opendir(dir);
    if (!d) return CB_E_IO;
    const size_t nl = strlen(name);
    int cap = 0;
    struct dirent *e;
    int rc = CB_OK;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, name, nl) != 0) continue;
        const char *num = e->d_name + nl;
        if (strlen(num) != 10 || strspn(num, "0123456789") != 10) continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            uint64_t *p = realloc(*out, (size_t)cap * sizeof *p);
            if (!p) { rc = CB_E_NOMEM; break; }
            *out = p;
        }
        int64_t v;
        if (!num_parse_i64(num, 10, 0, INT64_MAX, &v)) continue;
        (*out)[(*n)++] = (uint64_t)v;
    }
    closedir(d);
    if (*n) qsort(*out, (size_t)*n, sizeof **out, seq_cmp);
    return rc;
}

static int read_file(const char *path, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return CB_E_IO;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return CB_E_IO; }
    uint8_t *p = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (p && got < (size_t)st.st_size) {
        ssize_t r = read(fd, p + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!p) return CB_E_NOMEM;
    *out = p;
    *len = got;
    return CB_OK;
}

// One recovered move onto its game: through the kernel (the app's apply), from the overlay's
// row or the tables'. A move the tables already hold is skipped.
static int recover_move(cb_journal *j, const cb_jmove *m, char *err, size_t cap) {
    cb_game_row cur;
    int rc = ov_get(j, m->game, &cur);
    if (rc == CB_E_NOT_FOUND) rc = cb_rows_sql.game_get(j->s, m->game, &cur);
    if (rc == CB_E_NOT_FOUND) return CB_OK;   // deleted since: its moves went with it
    if (rc != CB_OK) {
        snprintf(err, cap, "journal: game %s does not read (%s)", m->game, cb_strerror(rc));
        return rc;
    }
    if (cur.version > m->base_version) { cb_game_row_free(&cur); return CB_OK; }
    if (cur.version != m->base_version) {
        snprintf(err, cap, "journal: game %s is at version %lld and the journal's next move is from %lld: a move is missing",
                 m->game, (long long)cur.version, (long long)m->base_version);
        cb_game_row_free(&cur);
        return CB_E_STATE;
    }
    jrec r;
    memset(&r, 0, sizeof r);
    r.m = *m;
    rc = j->o.apply(j->o.apply_ud, &cur, m->now_ms, &m->mv, &r.row, &r.log, &r.log_len);
    cb_game_row_free(&cur);
    if (rc == CB_OK && r.row.version != m->base_version + 1) rc = CB_E_STATE;
    if (rc != CB_OK) {
        snprintf(err, cap, "journal: game %s's move from version %lld does not replay (%s)", m->game,
                 (long long)m->base_version, cb_strerror(rc));
        cb_game_row_free(&r.row);
        free(r.log);
        return rc;
    }
    pthread_mutex_lock(&j->ov_mu);
    rc = ov_apply(j, &r);
    pthread_mutex_unlock(&j->ov_mu);
    cb_game_row_free(&r.row);
    free(r.log);
    if (rc == CB_OK) atomic_fetch_add(&j->recovered, 1);
    return rc;
}

static int recover_segment(cb_journal *j, uint64_t seq, char *err, size_t cap) {
    char path[1200];
    seg_path(j, seq, path, sizeof path);
    uint8_t *buf;
    size_t len;
    int rc = read_file(path, &buf, &len);
    if (rc != CB_OK) {
        snprintf(err, cap, "journal: %s does not read", path);
        return rc;
    }
    if (len == 0) {   // opened, never written to
        free(buf);
        return CB_OK;
    }
    if (len < SEG_HDR || memcmp(buf, SEG_MAGIC, 4) != 0 || get_u32(buf + 4) != FORMAT) {
        // A segment cut before its header was whole was never written to.
        CB_WARN(MOD, "%s has no whole header (%zu bytes): skipped", path, len);
        free(buf);
        return CB_OK;
    }
    const int64_t start = (int64_t)get_u64(buf + 8);
    cb_jslots *t = cb_jslots_new();
    cb_jmove *ms = malloc(sizeof *ms * CB_JOURNAL_BATCH_MAX);
    if (!t || !ms) rc = CB_E_NOMEM;
    size_t at = SEG_HDR;
    while (rc == CB_OK && at < len) {
        size_t used = 0;
        int n = cb_journal_batch_decode(t, start, buf + at, len - at, &used, ms, CB_JOURNAL_BATCH_MAX);
        if (n < 0) {
            // A batch cut short by the crash, or one that does not check: it was never
            // acknowledged, and nothing after it was written.
            CB_WARN(MOD, "%s: %zu bytes at %zu are not a whole batch: dropped", path, len - at, at);
            break;
        }
        for (int i = 0; rc == CB_OK && i < n; i++) rc = recover_move(j, &ms[i], err, cap);
        at += used;
    }
    cb_jslots_free(t);
    free(ms);
    free(buf);
    return rc;
}

static void journal_free(cb_journal *j) {
    if (!j) return;
    if (j->fd >= 0) close(j->fd);
    cb_jslots_free(j->slots);
    for (int b = 0; b < OV_BUCKETS; b++)
        while (j->buckets[b]) {
            jgame *g = j->buckets[b];
            j->buckets[b] = g->next;
            game_free(g);
        }
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    pthread_mutex_destroy(&j->ov_mu);
    pthread_mutex_destroy(&j->flush_mu);
    free(j);
}

int cb_journal_open(cb_store *s, const cb_journal_opts *o, char *err, size_t cap) {
    if (!s || !o || !o->path || !*o->path || !o->apply || !cb_store_is_sql(s) || s->app_state) {
        snprintf(err, cap, "journal: needs a SQL store with nothing installed, a path and an apply");
        return CB_E_INVALID;
    }
    cb_journal *j = calloc(1, sizeof *j);
    if (!j) return CB_E_NOMEM;
    j->s = s;
    j->o = *o;
    if (j->o.flush_ms <= 0) j->o.flush_ms = 2000;
    if (j->o.flush_moves <= 0) j->o.flush_moves = 4096;
    j->fd = -1;
    snprintf(j->prefix, sizeof j->prefix, "%s-journal.", o->path);
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    pthread_mutex_init(&j->ov_mu, NULL);
    pthread_mutex_init(&j->flush_mu, NULL);
    if (!J_OPS.game_get) {
        J_OPS = cb_rows_sql;
        J_OPS.game_get = j_game_get;
        J_OPS.game_logs = j_game_logs;
        J_OPS.game_commit = j_game_commit;
        J_OPS.game_delete = j_game_delete;
        J_OPS.games_of_user = j_games_of_user;
        J_OPS.games_get_views = j_games_get_views;
        J_OPS.games_needing_bots = j_games_needing_bots;
        J_OPS.game_import = j_game_import;
    }

    uint64_t *segs = NULL;
    int n = 0;
    int rc = list_segments(j, &segs, &n);
    if (rc != CB_OK) snprintf(err, cap, "journal: the directory of %s does not list", o->path);
    for (int i = 0; rc == CB_OK && i < n; i++) rc = recover_segment(j, segs[i], err, cap);
    if (rc == CB_OK) {
        j->first_seq = n ? segs[0] : 1;
        j->seg_seq = n ? segs[n - 1] : 0;
        // Everything recovered goes into the tables before anything is served, and the
        // segments with it.
        snapshot sn;
        pthread_mutex_lock(&j->ov_mu);
        rc = snapshot_take(j, &sn);
        pthread_mutex_unlock(&j->ov_mu);
        if (rc == CB_OK && sn.n) rc = cb_rows_sql_flush(s, sn.f, sn.n, NULL, NULL, 0);
        snapshot_free(&sn);
        if (rc != CB_OK) snprintf(err, cap, "journal: the recovered moves do not write (%s)", cb_strerror(rc));
    }
    if (rc == CB_OK) {
        for (int b = 0; b < OV_BUCKETS; b++)
            while (j->buckets[b]) {
                jgame *g = j->buckets[b];
                j->buckets[b] = g->next;
                game_free(g);
            }
        j->n_waiting = 0;
        if ((rc = seg_open(j, j->seg_seq + 1)) != CB_OK) snprintf(err, cap, "journal: no segment can be made beside %s", o->path);
    }
    if (rc == CB_OK) delete_segments_before(j, j->seg_seq);
    free(segs);
    if (rc == CB_OK && pthread_create(&j->thr, NULL, flusher, j) != 0) {
        snprintf(err, cap, "journal: no flush thread");
        rc = CB_E_NOMEM;
    }
    if (rc != CB_OK) {
        journal_free(j);
        return rc;
    }
    j->thr_started = true;
    if (j->recovered) CB_INFO(MOD, "recovered %ld move(s) from the journal", (long)j->recovered);
    s->app_state = j;
    s->app_settle = cb_journal_flush;
    return CB_OK;
}

void cb_journal_close(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    if (!j) return;
    pthread_mutex_lock(&j->mu);
    j->stop = true;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (j->thr_started) pthread_join(j->thr, NULL);
    int rc = flush_all(j);
    if (rc == CB_OK) delete_segments_before(j, j->seg_seq + 1);
    else CB_ERROR(MOD, "the last flush failed (%s): the segments stay for the next start", cb_strerror(rc));
    CB_INFO(MOD, "%ld record(s) in %ld batch(es), %ld write(s), %ld byte(s); %ld flush(es) of %ld move(s)",
            (long)j->records, (long)j->batches, (long)j->writes, (long)j->bytes, (long)j->flushes, (long)j->flushed_moves);
    s->app_state = NULL;
    s->app_settle = NULL;
    journal_free(j);
}

void cb_journal_stats_get(const cb_store *s, cb_journal_stats *out) {
    memset(out, 0, sizeof *out);
    cb_journal *j = cb_journal_of(s);
    if (!j) return;
    out->batches = j->batches;
    out->writes = j->writes;
    out->records = j->records;
    out->bytes = j->bytes;
    out->flushes = j->flushes;
    out->flushed_games = j->flushed_games;
    out->flushed_moves = j->flushed_moves;
    out->recovered = j->recovered;
}

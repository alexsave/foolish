// journal.c - see journal.h.
#include "store/journal.h"
#include "store/store_sql.h"

#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "roster.h"
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOD "journal"
#define SEG_MAGIC "FJNL"
#define SEG_HDR 16
#define FORMAT 1
#define BATCH_MAGIC 'J'
#define OV_BUCKETS 1024
#define IDLE_EVICT_MS 60000
#define TICK_MS 1000            // the flush thread checks the flush rule this often

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
        if (r->n_mv < 1 || r->n_mv > (r->bots ? BOT_DRIVE_MAX_ACTIONS : 1)) return CB_E_INVALID;
        replay_coder_uniform(&c, 2, r->bots ? 1 : 0);
        if (r->bots) replay_coder_uniform(&c, BOT_DRIVE_MAX_ACTIONS, r->n_mv - 1);
        for (int k = 0; k < r->n_mv; k++) {
            ReplayMove mv = r->mv[k];
            replay_move_code(&c, &mv);
        }
    }
    int len = replay_coder_finish(&c, out + CB_JOURNAL_BATCH_HDR, cap - CB_JOURNAL_BATCH_HDR);
    if (len < 0 || len > 0xFFFF) return CB_E_FULL;
    // THE COMMIT MARKER: the header goes after the payload, and its magic byte last (a
    // release store), so a segment's mapped pages cut anywhere before it hold a zero where
    // the batch's magic would be, which recovery reads as the segment's end.
    uint8_t h[4] = { BATCH_MAGIC, FORMAT, 0, 0 };
    put_u16(h + 2, (uint32_t)len);
    put_u32(out + 4, batch_sum(h, out + CB_JOURNAL_BATCH_HDR, (size_t)len));
    put_u16(out + 2, (uint32_t)len);
    out[1] = FORMAT;
    __atomic_store_n(&out[0], (uint8_t)BATCH_MAGIC, __ATOMIC_RELEASE);
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
        r->bots = replay_coder_uniform(&c, 2, -1) != 0;
        r->n_mv = r->bots ? replay_coder_uniform(&c, BOT_DRIVE_MAX_ACTIONS, -1) + 1 : 1;
        for (int k = 0; k < r->n_mv; k++)
            if (replay_move_code(&c, &r->mv[k]) != 0) return -1;
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
    long          bytes;         // what the waiting moves hold (move_bytes each)
    size_t        state_cap, roster_cap;   // row.state's and row.roster's buffers
    int64_t       touched_ms;    // cb_mono_ms of its last move
    int64_t       first_ms;      // cb_mono_ms of its oldest waiting move
    uint64_t      first_seg;     // a segment at or before the one holding its oldest waiting move
    int           n_humans;      // the humans its roster seats (the dashboard list's members)
    cb_uuid       humans[CB_MAX_SEATS];
} jgame;

static long move_bytes(const cb_game_move *m) { return (long)(sizeof *m + m->len); }

// A segment file, mapped.
typedef struct {
    int      fd;
    uint8_t *map;
    size_t   size;
    uint64_t seq;
} jseg;

// A move waiting for its batch's write.
typedef struct jrec {
    struct jrec *next;
    long         epoch;         // the journal's epoch when its base version was read
    cb_jmove     m;
    cb_game_row  row;           // the new row (the caller's bytes: ov_apply copies them)
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
    pthread_cond_t  cv;            // the writers: a batch written, the writer free (waiters: how many wait)
    pthread_cond_t  fcv;           // the flush thread: a kick, a segment to make ahead, the stop
    int             waiters;
    jrec           *head, *tail;
    bool            writing;
    jseg            cur, next;     // the open segment, and the one made ahead
    uint64_t        seg_seq;       // the open segment's number
    pthread_mutex_t seg_mu;
    uint64_t       *segs;          // the segments on disk, oldest first
    int             n_segs, cap_segs;
    int64_t         seg_start;
    off_t           seg_len;
    cb_jslots      *slots;
    // the overlay
    pthread_mutex_t ov_mu;
    jgame          *buckets[OV_BUCKETS];
    int             n_games;       // in the overlay, waiting or not
    long            n_waiting;     // moves not in the tables yet
    long            w_bytes;       // their bytes
    // the flush: one at a time, and every write of a journaled game around the journal
    pthread_mutex_t flush_mu;
    pthread_t       thr;
    bool            thr_started, stop, kick, prepare;
    // a backup attached (the backup hooks below): no segment at or after backup_mark is deleted
    bool            pinned;
    bool            restored;      // the database is a restored copy (store.h CB_STORE_RESTORED_SUFFIX)
    uint64_t        backup_mark;
    // bumped by an exclusive section (an admin write): a record whose base was read before it is refused
    atomic_long     epoch;
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

static int grow_copy(uint8_t **p, size_t *cap, const uint8_t *src, size_t n) {
    if (n > *cap) {
        uint8_t *q = realloc(*p, n);
        if (!q) return CB_E_NOMEM;
        *p = q;
        *cap = n;
    }
    if (n) memcpy(*p, src, n);
    return CB_OK;
}

static int row_assign(jgame *g, const cb_game_row *src) {
    uint8_t *state = g->row.state, *roster = g->row.roster;
    if (grow_copy(&state, &g->state_cap, src->state, src->state_len) != CB_OK) { g->row.state = state; return CB_E_NOMEM; }
    g->row.state = state;
    if (grow_copy(&roster, &g->roster_cap, src->roster, src->roster_len) != CB_OK) { g->row.roster = roster; return CB_E_NOMEM; }
    cb_game_row r = *src;
    r.state = state;
    r.roster = roster;
    r.logs = NULL;
    r.logs_len = 0;
    g->row = r;
    return CB_OK;
}

// A record's row and log become the game's (ov_mu held). The log is taken, the row copied.
static int ov_apply(cb_journal *j, jrec *r) {
    jgame **p = ov_slot(j, r->m.game), *g = *p;
    if (!g) {
        if (!(g = calloc(1, sizeof *g))) return CB_E_NOMEM;
        g->sql_version = r->m.base_version;
        // A journaled move never changes the seats: the roster's humans hold while it is here.
        Roster ro;
        if (roster_decode(&ro, r->row.roster, (int)r->row.roster_len) == ROSTER_OK)
            for (int i = 0; i < ro.n && g->n_humans < CB_MAX_SEATS; i++)
                if (ro.seats[i].brain_len == 0)
                    snprintf(g->humans[g->n_humans++], sizeof g->humans[0], "%.*s", (int)ro.seats[i].id_len,
                             (const char *)ro.seats[i].id);
        *p = g;
        j->n_games++;
    }
    if (g->n_moves == g->cap_moves) {
        int cap = g->cap_moves ? g->cap_moves * 2 : 8;
        cb_game_move *m = realloc(g->moves, (size_t)cap * sizeof *m);
        if (!m) return CB_E_NOMEM;
        g->moves = m;
        g->cap_moves = cap;
    }
    const int64_t now = cb_mono_ms();
    if (!g->n_moves) {
        g->first_ms = now;
        g->first_seg = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED);
    }
    // The row's bytes go into the game's own buffers, grown only when a row outgrows them:
    // no allocation per move.
    if (row_assign(g, &r->row) != CB_OK) return CB_E_NOMEM;
    g->moves[g->n_moves] = (cb_game_move){ r->row.version, r->log, r->log_len };
    const long b = move_bytes(&g->moves[g->n_moves++]);
    r->log = NULL;
    g->touched_ms = now;
    g->bytes += b;
    j->w_bytes += b;
    j->n_waiting++;
    return CB_OK;
}

// ---- segments ------------------------------------------------------------------------------

static int segs_add(cb_journal *j, uint64_t seq);

static void seg_path(const cb_journal *j, uint64_t seq, char *out, size_t cap) {
    snprintf(out, cap, "%s%016llu", j->prefix, (unsigned long long)seq);
}

// A segment file of `size` bytes, its blocks allocated (a mapped page past the disk's room
// would be a SIGBUS mid-append; here it is ENOSPC at creation) and mapped shared.
static int seg_make(cb_journal *j, uint64_t seq, size_t size, jseg *out) {
    char path[1200];
    seg_path(j, seq, path, sizeof path);
    memset(out, 0, sizeof *out);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        CB_ERROR(MOD, "open %s: %s", path, strerror(errno));
        return CB_E_IO;
    }
    int e = 0;
#if defined(__linux__)
    e = posix_fallocate(fd, 0, (off_t)size);
#elif defined(F_PREALLOCATE)
    fstore_t fs = { F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, (off_t)size, 0 };
    if (fcntl(fd, F_PREALLOCATE, &fs) != 0) {
        fs.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &fs) != 0) e = errno;
    }
    if (!e && ftruncate(fd, (off_t)size) != 0) e = errno;
#else
    if (ftruncate(fd, (off_t)size) != 0) e = errno;
#endif
    void *m = e ? MAP_FAILED : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (!e && m == MAP_FAILED) e = errno;
    if (e) {
        CB_ERROR(MOD, "%s: %zu bytes do not allocate and map (%s)", path, size, strerror(e));
        close(fd);
        unlink(path);
        return e == ENOSPC ? CB_E_FULL : CB_E_IO;
    }
    *out = (jseg){ .fd = fd, .map = m, .size = size, .seq = seq };
    return CB_OK;
}

// A finished segment: cut to the bytes it holds and unmapped.
static void seg_finish(jseg *g, size_t used) {
    if (!g->map) return;
    munmap(g->map, g->size);
    if (ftruncate(g->fd, (off_t)used) != 0) CB_WARN(MOD, "ftruncate: %s", strerror(errno));
    close(g->fd);
    memset(g, 0, sizeof *g);
}

static void seg_drop(cb_journal *j, jseg *g) {   // made, never written to
    if (!g->map) return;
    char path[1200];
    seg_path(j, g->seq, path, sizeof path);
    munmap(g->map, g->size);
    close(g->fd);
    unlink(path);
    memset(g, 0, sizeof *g);
}

// The bytes a segment is made with: seg_bytes and room for the largest write past it.
static size_t seg_size(const cb_journal *j, size_t need) {
    size_t z = (size_t)j->o.seg_bytes + need;
    const size_t pg = 65536;
    return (z + pg - 1) / pg * pg;
}

static size_t write_need(int n) {   // the most a write of n records takes, its segment header too
    return SEG_HDR + (size_t)(n / CB_JOURNAL_BATCH_MAX + 1) * (CB_JOURNAL_BATCH_HDR + REPLAY_MAX_INT_BYTES);
}

// The next segment made ahead, so the writer's rotation only swaps maps (the flush thread's
// tick and a rotation's kick; seg_mu held by the caller).
static void prepare_next_locked(cb_journal *j) {
    const uint64_t seq = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED) + 1;
    if (j->next.map && j->next.seq == seq) return;
    seg_drop(j, &j->next);
    (void)seg_make(j, seq, seg_size(j, write_need(CB_JOURNAL_BATCH_MAX)), &j->next);
}

static void prepare_next(cb_journal *j) {
    pthread_mutex_lock(&j->seg_mu);
    prepare_next_locked(j);
    pthread_mutex_unlock(&j->seg_mu);
}

// The open segment finished and the next one (made ahead, or now) open, able to take `need`
// bytes; its header goes with its first batch, which gives it its start time. The writer is held.
static int seg_open(cb_journal *j, size_t need) {
    pthread_mutex_lock(&j->seg_mu);
    const uint64_t seq = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED) + 1;
    int rc = CB_OK;
    if (!j->next.map || j->next.seq != seq || j->next.size < need) {
        seg_drop(j, &j->next);
        rc = seg_make(j, seq, seg_size(j, need), &j->next);
    }
    cb_jslots *t = rc == CB_OK ? cb_jslots_new() : NULL;
    if (rc == CB_OK && !t) rc = CB_E_NOMEM;
    if (rc == CB_OK && j->n_segs == j->cap_segs) {
        int cap = j->cap_segs ? j->cap_segs * 2 : 16;
        uint64_t *p = realloc(j->segs, (size_t)cap * sizeof *p);
        if (!p) rc = CB_E_NOMEM;
        else j->segs = p, j->cap_segs = cap;
    }
    if (rc == CB_OK) {
        seg_finish(&j->cur, (size_t)j->seg_len);
        j->cur = j->next;
        memset(&j->next, 0, sizeof j->next);
        j->segs[j->n_segs++] = seq;
        cb_jslots_free(j->slots);
        j->slots = t;
        __atomic_store_n(&j->seg_seq, seq, __ATOMIC_RELAXED);   // ov_apply and the low-water mark read it
        j->seg_start = 0;
        __atomic_store_n(&j->seg_len, 0, __ATOMIC_RELEASE);
    } else {
        cb_jslots_free(t);
    }
    pthread_mutex_unlock(&j->seg_mu);
    if (rc == CB_OK) {   // the flush thread makes the one after
        pthread_mutex_lock(&j->mu);
        j->prepare = true;
        pthread_cond_signal(&j->fcv);
        pthread_mutex_unlock(&j->mu);
    }
    return rc;
}

// Codes and writes one batch of records (the writer is held, j->mu is not). Each record's rc set.
static void write_batch(cb_journal *j, jrec *list) {
    // A record whose base version was read before an exclusive section (an admin write) is
    // refused: the tables may hold another game now. Its caller reads again (a conflict).
    const long epoch = atomic_load(&j->epoch);
    int n = 0;
    for (jrec *r = list; r; r = r->next) {
        r->rc = r->epoch == epoch ? CB_OK : CB_E_CONFLICT;
        n += r->rc == CB_OK;
    }
    if (!n) return;
    // The batches are coded straight into the open segment's mapped pages: no buffer, no
    // write(). A segment is closed once it holds seg_bytes, or when this write might not fit
    // (or after a failed one, below); a batch's time is coded against its segment's start,
    // so one a clock jump put out of reach starts a segment of its own too.
    cb_jmove *ms = malloc(sizeof *ms * (size_t)(n < CB_JOURNAL_BATCH_MAX ? n : CB_JOURNAL_BATCH_MAX));
    int rc = ms ? CB_OK : CB_E_NOMEM;
    const size_t need = write_need(n);
    size_t len = 0;
    int frames = 0;
    int64_t first = INT64_MAX;
    for (jrec *r = list; r; r = r->next) if (r->rc == CB_OK && r->m.now_ms < first) first = r->m.now_ms;
    if (rc == CB_OK && (!j->cur.map || j->seg_len >= j->o.seg_bytes || (size_t)j->seg_len + need > j->cur.size ||
                        (j->seg_len > 0 && (first - j->seg_start > INT32_MAX / 2 || first - j->seg_start < INT32_MIN / 2))))
        rc = seg_open(j, need);
    uint8_t *at = rc == CB_OK ? j->cur.map + j->seg_len : NULL;
    const size_t room = rc == CB_OK ? j->cur.size - (size_t)j->seg_len : 0;
    if (rc == CB_OK && j->seg_len == 0) {
        put_u32(at + 4, FORMAT);
        put_u64(at + 8, (uint64_t)first);
        memcpy(at, SEG_MAGIC, 4);
        j->seg_start = first;
        len = SEG_HDR;
    }
    for (jrec *r = list; rc == CB_OK && r;) {
        int k = 0;
        for (; r && k < CB_JOURNAL_BATCH_MAX; r = r->next) if (r->rc == CB_OK) ms[k++] = r->m;
        if (!k) break;
        int w = cb_journal_batch_encode(j->slots, j->seg_start, ms, k, at + len, (int)(room - len));
        if (w < 0) rc = w;
        else len += (size_t)w, frames++;
    }
    if (rc == CB_OK && j->o.sync) {   // the pages to the disk too: an OS crash keeps the move
        const uintptr_t pg = (uintptr_t)sysconf(_SC_PAGESIZE), a = (uintptr_t)at & ~(pg - 1);
        if (msync((void *)a, (uintptr_t)at + len - a, MS_SYNC) != 0) {
            CB_ERROR(MOD, "msync: %s", strerror(errno));
            rc = CB_E_IO;
        }
    }
    if (rc == CB_OK) {
        __atomic_store_n(&j->seg_len, j->seg_len + (off_t)len, __ATOMIC_RELEASE);   // the backup reads it
        atomic_fetch_add(&j->batches, frames);
        atomic_fetch_add(&j->writes, 1);
        atomic_fetch_add(&j->records, n);
        atomic_fetch_add(&j->bytes, (long)len);
    } else if (at) {
        // Nothing of this write is acknowledged: whatever it put past the segment's last
        // whole batch is zeroed (a batch committed before the failure would replay without an
        // acknowledgement), and the slot table that may have moved on goes with a new segment.
        memset(at, 0, len);
        if (seg_open(j, need) != CB_OK) seg_finish(&j->cur, (size_t)j->seg_len);   // the next write tries again
    }
    free(ms);
    pthread_mutex_lock(&j->ov_mu);
    bool due = false;
    for (jrec *r = list; r; r = r->next) {
        if (r->rc != CB_OK) continue;
        r->rc = rc;
        if (rc == CB_OK && (r->rc = ov_apply(j, r)) != CB_OK) CB_ERROR(MOD, "game %s: overlay out of memory", r->m.game);
        jgame *g = rc == CB_OK ? *ov_slot(j, r->m.game) : NULL;
        if (g && g->bytes >= j->o.game_bytes) due = true;
    }
    if (j->w_bytes >= j->o.total_bytes) due = true;
    pthread_mutex_unlock(&j->ov_mu);
    if (due) {   // the flush thread applies the rule now rather than at its next tick
        pthread_mutex_lock(&j->mu);
        j->kick = true;
        pthread_cond_signal(&j->fcv);
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
            if (j->waiters) pthread_cond_broadcast(&j->cv);   // no wake-up when nobody waits
        } else {
            j->waiters++, pthread_cond_wait(&j->cv, &j->mu), j->waiters--;
        }
    }
    pthread_mutex_unlock(&j->mu);
    return r->rc;
}

// ---- the flush -----------------------------------------------------------------------------

// The writer, held (no batch in flight while the caller rotates).
static void writer_hold(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    while (j->writing) j->waiters++, pthread_cond_wait(&j->cv, &j->mu), j->waiters--;
    j->writing = true;
    pthread_mutex_unlock(&j->mu);
}

static void writer_release(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    j->writing = false;
    if (j->waiters) pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
}

// The segments on disk (seg_mu): seg_open adds, delete_segments_before takes away.
static int segs_add(cb_journal *j, uint64_t seq) {
    pthread_mutex_lock(&j->seg_mu);
    int rc = CB_OK;
    if (j->n_segs == j->cap_segs) {
        int cap = j->cap_segs ? j->cap_segs * 2 : 16;
        uint64_t *p = realloc(j->segs, (size_t)cap * sizeof *p);
        if (!p) rc = CB_E_NOMEM;
        else j->segs = p, j->cap_segs = cap;
    }
    if (rc == CB_OK) j->segs[j->n_segs++] = seq;
    pthread_mutex_unlock(&j->seg_mu);
    return rc;
}

static void delete_segments_before(cb_journal *j, uint64_t seq) {
    char path[1200];
    pthread_mutex_lock(&j->seg_mu);
    int k = 0;
    for (; k < j->n_segs && j->segs[k] < seq; k++) {
        seg_path(j, j->segs[k], path, sizeof path);
        (void)unlink(path);
    }
    memmove(j->segs, j->segs + k, (size_t)(j->n_segs - k) * sizeof *j->segs);
    j->n_segs -= k;
    pthread_mutex_unlock(&j->seg_mu);
}

// The low-water mark: no waiting move is in a segment before it (ov_mu held).
static uint64_t low_water(cb_journal *j) {
    uint64_t m = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED);
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next)
            if (g->n_moves && g->first_seg < m) m = g->first_seg;
    return m;
}

// THE TRIM RULE: a segment goes once every move in it is in the tables and, with a backup
// attached, once a copy that needs it no longer is the last one uploaded (flush_mu held).
static void trim(cb_journal *j) {
    pthread_mutex_lock(&j->ov_mu);
    uint64_t lw = low_water(j);
    pthread_mutex_unlock(&j->ov_mu);
    if (j->pinned && j->backup_mark < lw) lw = j->backup_mark;
    delete_segments_before(j, lw);
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

// THE FLUSH RULE: a game whose oldest waiting move is game_age_ms old, or whose waiting moves
// hold game_bytes, is due; with `all` (every waiting move together over total_bytes, a
// settle, the close) every game with a move waiting is.
static bool flush_due(const cb_journal *j, const jgame *g, bool all, int64_t now) {
    return g->n_moves > 0 && (all || now - g->first_ms >= j->o.game_age_ms || g->bytes >= j->o.game_bytes);
}

// Every game due, copied (ov_mu held): rows and the move lists (the logs stay the overlay's;
// only a flush frees them, and the flush lock is held).
static int snapshot_take(cb_journal *j, snapshot *sn, bool all, int64_t now) {
    memset(sn, 0, sizeof *sn);
    int n = 0;
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next) n += flush_due(j, g, all, now);
    if (!n) return CB_OK;
    sn->f = calloc((size_t)n, sizeof *sn->f);
    sn->rows = calloc((size_t)n, sizeof *sn->rows);
    if (!sn->f || !sn->rows) { snapshot_free(sn); return CB_E_NOMEM; }
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next) {
            if (!flush_due(j, g, all, now)) continue;
            cb_game_move *m = malloc((size_t)g->n_moves * sizeof *m);
            if (!m || row_dup(&sn->rows[sn->n], &g->row) != CB_OK) { free(m); snapshot_free(sn); return CB_E_NOMEM; }
            memcpy(m, g->moves, (size_t)g->n_moves * sizeof *m);
            sn->f[sn->n] = (cb_game_flush){ &sn->rows[sn->n], g->sql_version, m, g->n_moves };
            sn->n++;
        }
    return CB_OK;
}

// The tables now hold f: its moves leave the game (ov_mu held). A move that came after the
// snapshot is in snap_seg or later.
static void ov_flushed(cb_journal *j, jgame *g, const cb_game_flush *f, uint64_t snap_seg, int64_t now) {
    int k = 0;
    long b = 0;
    for (; k < g->n_moves && g->moves[k].seq <= f->row->version; k++) {
        b += move_bytes(&g->moves[k]);
        free((void *)g->moves[k].log);
    }
    memmove(g->moves, g->moves + k, (size_t)(g->n_moves - k) * sizeof *g->moves);
    g->n_moves -= k;
    g->bytes -= b;
    j->w_bytes -= b;
    j->n_waiting -= k;
    g->sql_version = f->row->version;
    if (g->n_moves) {
        g->first_seg = snap_seg;
        g->first_ms = now;
    }
}

static void ov_unlink(cb_journal *j, jgame **p) {
    jgame *g = *p;
    *p = g->next;
    j->n_waiting -= g->n_moves;
    j->w_bytes -= g->bytes;
    j->n_games--;
    game_free(g);
}

// The games due into the tables in one transaction, then the segments trimmed. flush_mu is
// held; the writer is held for the snapshot (a move after it is in a later segment) unless
// the caller holds it already.
static int flush_locked(cb_journal *j, bool all, bool writer_held) {
    if (!writer_held) writer_hold(j);
    snapshot sn;
    const int64_t now = cb_mono_ms();
    pthread_mutex_lock(&j->ov_mu);
    if (j->w_bytes >= j->o.total_bytes) all = true;
    int rc = snapshot_take(j, &sn, all, now);
    const uint64_t snap_seg = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&j->ov_mu);
    if (!writer_held) writer_release(j);
    if (rc == CB_OK && sn.n) rc = cb_rows_sql_flush(j->s, sn.f, sn.n, NULL, NULL, 0);
    if (rc == CB_OK) {
        long moves = 0;
        pthread_mutex_lock(&j->ov_mu);
        for (int i = 0; i < sn.n; i++) {
            jgame *g = *ov_slot(j, sn.rows[i].id);
            if (g) ov_flushed(j, g, &sn.f[i], snap_seg, now);
            moves += sn.f[i].n_moves;
        }
        for (int b = 0; b < OV_BUCKETS; b++)   // a game idle and in the tables leaves the overlay
            for (jgame **p = &j->buckets[b]; *p;) {
                if (!(*p)->n_moves && now - (*p)->touched_ms > IDLE_EVICT_MS) ov_unlink(j, p);
                else p = &(*p)->next;
            }
        pthread_mutex_unlock(&j->ov_mu);
        trim(j);
        if (sn.n) {
            atomic_fetch_add(&j->flushes, 1);
            atomic_fetch_add(&j->flushed_games, sn.n);
            atomic_fetch_add(&j->flushed_moves, moves);
        }
    } else {
        CB_ERROR(MOD, "flush failed (%s): the moves stay in the journal and the next flush retries", cb_strerror(rc));
    }
    snapshot_free(&sn);
    return rc;
}

static int flush_some(cb_journal *j, bool all) {
    pthread_mutex_lock(&j->flush_mu);
    int rc = flush_locked(j, all, false);
    pthread_mutex_unlock(&j->flush_mu);
    return rc;
}

static int flush_all(cb_journal *j) { return flush_some(j, true); }

int cb_journal_flush(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    return j ? flush_all(j) : CB_OK;
}

int cb_journal_flush_due(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    return j ? flush_some(j, false) : CB_OK;
}

static void *flusher(void *ud) {
    cb_journal *j = ud;
    const int64_t tick = j->o.game_age_ms < TICK_MS ? j->o.game_age_ms : TICK_MS;
    pthread_mutex_lock(&j->mu);
    while (!j->stop) {
        // Until the tick or a batch that made a game (or all of them) due.
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t ns = ts.tv_nsec + (tick % 1000) * 1000000;
        ts.tv_sec += (time_t)(tick / 1000 + ns / 1000000000);
        ts.tv_nsec = (long)(ns % 1000000000);
        int w = 0;
        while (!j->stop && !j->kick && !j->prepare && (w = pthread_cond_timedwait(&j->fcv, &j->mu, &ts)) != ETIMEDOUT) {}
        if (j->stop) break;
        const bool flush = j->kick || w == ETIMEDOUT;
        j->kick = j->prepare = false;
        pthread_mutex_unlock(&j->mu);
        prepare_next(j);   // the next segment, made ahead of the rotation that takes it
        if (flush) (void)flush_some(j, false);
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
static int logs_of(cb_journal *j, const char *id, int64_t log_from, uint8_t **out, size_t *out_len) {
    cb_store *s = j->s;
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

static int j_game_logs(cb_store *s, const char *id, int64_t log_from, uint8_t **out, size_t *out_len) {
    return logs_of(cb_journal_of(s), id, log_from, out, out_len);
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
        atomic_fetch_add(&j->flushed_moves, g->n_moves);
        ov_unlink(j, ov_slot(j, id));
        pthread_mutex_unlock(&j->ov_mu);
    }
    pthread_mutex_unlock(&j->flush_mu);
    return rc;
}

static int j_game_commit(cb_store *s, const cb_game_commit *c) {
    cb_journal *j = cb_journal_of(s);
    if (!c || !c->row) return CB_E_INVALID;
    const cb_game_row *row = c->row;
    if (!c->moves || c->n_moves < 1 || c->n_moves > BOT_DRIVE_MAX_ACTIONS) return around(s, row->id, c, NULL, 0);
    // The fence the tables would apply: the game's acknowledged version is the expected one,
    // read in this epoch (an exclusive section after it refuses the record: write_batch).
    const long epoch = atomic_load(&j->epoch);
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
    r.epoch = epoch;
    snprintf(r.m.game, sizeof r.m.game, "%s", row->id);
    r.m.base_version = c->expected_version;
    r.m.now_ms = row->updated_at;
    r.m.bots = c->moves_by_bots;
    r.m.n_mv = c->n_moves;
    memcpy(r.m.mv, c->moves, (size_t)c->n_moves * sizeof *c->moves);
    r.row = *row;   // read by ov_apply while this thread waits in append
    if (c->log_add_len && !(r.log = dup_bytes(c->log_add, c->log_add_len))) return CB_E_NOMEM;
    r.log_len = c->log_add_len;
    int rc = append(j, &r);
    free(r.log);   // NULL once the overlay took it
    return rc;
}

static int j_game_delete(cb_store *s, const char *id, int64_t expected) { return around(s, id, NULL, id, expected); }

// THE SCAN RULE: a read over the games table (not one game by its id) reads the tables and
// then the overlay, which wins for every game it holds; the flush lock keeps a flush from
// moving a game between the two reads. A reader that cannot (an import) settles first.
static int journal_scan(cb_store *s) { return flush_all(cb_journal_of(s)); }

typedef struct {
    char (*ids)[CB_GAME_ID_MAX + 1];
    int  n, cap;
} idvec;

static bool idvec_add(const char *name, void *ud) {
    idvec *v = ud;
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 64;
        void *p = realloc(v->ids, (size_t)cap * sizeof *v->ids);
        if (!p) return false;
        v->ids = p;
        v->cap = cap;
    }
    snprintf(v->ids[v->n++], sizeof v->ids[0], "%s", name);
    return true;
}

// The bot scan: the tables' games the overlay does not hold (asked for `limit` more than the
// overlay holds, so its games dropped leave `limit`), then the overlay's that match, at most
// `limit` in all. The order is the tables' then the overlay's: the scan is the safety net
// behind the kicks, and its limit a cap.
static int j_games_needing_bots(cb_store *s, int64_t stale_before, int64_t active_after, int limit, cb_name_fn fn,
                                void *ud) {
    cb_journal *j = cb_journal_of(s);
    idvec v = { 0 };
    pthread_mutex_lock(&j->flush_mu);
    pthread_mutex_lock(&j->ov_mu);
    const int held = j->n_games;
    pthread_mutex_unlock(&j->ov_mu);
    int rc = cb_rows_sql.games_needing_bots(s, stale_before, active_after, limit > 0 ? limit + held : limit, idvec_add, &v);
    int out = 0;
    if (rc >= 0) {
        rc = CB_OK;
        pthread_mutex_lock(&j->ov_mu);
        int k = 0;
        for (int i = 0; i < v.n; i++)
            if (!*ov_slot(j, v.ids[i])) memmove(v.ids[k++], v.ids[i], sizeof v.ids[0]);
        v.n = k;
        for (int b = 0; b < OV_BUCKETS; b++)
            for (jgame *g = j->buckets[b]; g; g = g->next)
                if (g->row.needs_bots && g->row.updated_at < stale_before && g->row.last_commit_at > active_after &&
                    !idvec_add(g->row.id, &v))
                    rc = CB_E_NOMEM;
        pthread_mutex_unlock(&j->ov_mu);
    }
    pthread_mutex_unlock(&j->flush_mu);
    for (int i = 0; rc == CB_OK && i < v.n && (limit <= 0 || out < limit); i++) {
        out++;
        if (!fn(v.ids[i], ud)) break;
    }
    free(v.ids);
    return rc == CB_OK ? out : rc;
}

typedef struct {
    cb_member_row *rows;
    int            n, cap;
} memvec;

static bool memvec_add(const cb_member_row *r, void *ud) {
    memvec *v = ud;
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 32;
        void *p = realloc(v->rows, (size_t)cap * sizeof *v->rows);
        if (!p) return false;
        v->rows = p;
        v->cap = cap;
    }
    v->rows[v->n++] = *r;
    return true;
}

static int member_cmp(const void *a, const void *b) {   // the tables' ORDER BY updated_at DESC, game_id
    const cb_member_row *x = a, *y = b;
    if (x->updated_at != y->updated_at) return x->updated_at > y->updated_at ? -1 : 1;
    return strcmp(x->game_id, y->game_id);
}

// The dashboard list: the tables' newest `limit`, each game the overlay holds at its
// overlay time, and every overlay game the user sits at added (a move only makes a game
// newer, so a game the tables leave out of their newest `limit` and the overlay does not
// hold is not among the newest), sorted and cut to `limit`.
static int j_games_of_user(cb_store *s, const char *user, int limit, cb_member_fn fn, void *ud) {
    cb_journal *j = cb_journal_of(s);
    memvec v = { 0 };
    pthread_mutex_lock(&j->flush_mu);
    int rc = cb_rows_sql.games_of_user(s, user, limit, memvec_add, &v);
    if (rc >= 0) {
        rc = CB_OK;
        pthread_mutex_lock(&j->ov_mu);
        for (int b = 0; rc == CB_OK && b < OV_BUCKETS; b++)
            for (jgame *g = j->buckets[b]; rc == CB_OK && g; g = g->next) {
                bool sits = false;
                for (int h = 0; h < g->n_humans && !sits; h++) sits = strcmp(g->humans[h], user) == 0;
                if (!sits) continue;
                int i = 0;
                while (i < v.n && strcmp(v.rows[i].game_id, g->row.id) != 0) i++;
                if (i < v.n) v.rows[i].updated_at = g->row.updated_at;
                else {
                    cb_member_row m;
                    snprintf(m.game_id, sizeof m.game_id, "%s", g->row.id);
                    m.updated_at = g->row.updated_at;
                    if (!memvec_add(&m, &v)) rc = CB_E_NOMEM;
                }
            }
        pthread_mutex_unlock(&j->ov_mu);
    }
    pthread_mutex_unlock(&j->flush_mu);
    int out = 0;
    if (rc == CB_OK) {
        if (v.n > 1) qsort(v.rows, (size_t)v.n, sizeof *v.rows, member_cmp);
        for (int i = 0; i < v.n && (limit <= 0 || out < limit); i++) {
            out++;
            if (!fn(&v.rows[i], ud)) break;
        }
    }
    free(v.rows);
    return rc == CB_OK ? out : rc;
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

// ---- an admin's write, and the backup ------------------------------------------------------

// THE ADMIN RULE (store.h cb_store_admin_query): every waiting move into the tables, no
// move acknowledged while fn runs, and then the overlay forgotten and the epoch bumped, so
// no row the journal held before fn (nor a record whose base was read before it) is ever
// written over what fn wrote.
static int j_exclusive(cb_store *s, int (*fn)(void *ud), void *ud) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->flush_mu);
    writer_hold(j);
    int rc = flush_locked(j, true, true);
    if (rc == CB_OK) {
        rc = fn(ud);
        pthread_mutex_lock(&j->ov_mu);
        for (int b = 0; b < OV_BUCKETS; b++)
            while (j->buckets[b]) ov_unlink(j, &j->buckets[b]);
        pthread_mutex_unlock(&j->ov_mu);
        atomic_fetch_add(&j->epoch, 1);
    }
    writer_release(j);
    pthread_mutex_unlock(&j->flush_mu);
    return rc;
}

static int j_backup_mark(cb_store *s, uint64_t *mark) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->flush_mu);
    j->pinned = true;
    pthread_mutex_lock(&j->ov_mu);
    *mark = low_water(j);
    pthread_mutex_unlock(&j->ov_mu);
    pthread_mutex_unlock(&j->flush_mu);
    return CB_OK;
}

static int j_backup_files(cb_store *s, uint64_t mark, cb_store_file **out, int *n) {
    cb_journal *j = cb_journal_of(s);
    *out = NULL;
    *n = 0;
    pthread_mutex_lock(&j->seg_mu);
    int rc = CB_OK;
    cb_store_file *f = malloc(sizeof *f * (size_t)(j->n_segs ? j->n_segs : 1));
    if (!f) rc = CB_E_NOMEM;
    char path[1200];
    for (int i = 0; rc == CB_OK && i < j->n_segs; i++) {
        if (j->segs[i] < mark) continue;
        seg_path(j, j->segs[i], path, sizeof path);
        // The open segment's file is its mapped size: what it holds is seg_len. A finished
        // one was cut to what it holds.
        struct stat st;
        const int64_t size = j->segs[i] == j->cur.seq && j->cur.map ? (int64_t)__atomic_load_n(&j->seg_len, __ATOMIC_ACQUIRE)
                             : stat(path, &st) == 0 ? (int64_t)st.st_size : 0;
        if (size == 0) continue;   // nothing written yet
        snprintf(f[*n].suffix, sizeof f[*n].suffix, "%s", path + strlen(j->o.path));
        f[(*n)++].size = size;
    }
    pthread_mutex_unlock(&j->seg_mu);
    if (rc == CB_OK) *out = f;
    else free(f);
    return rc;
}

static void j_backup_release(cb_store *s, uint64_t mark) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->flush_mu);
    if (mark > j->backup_mark) j->backup_mark = mark;
    trim(j);
    pthread_mutex_unlock(&j->flush_mu);
}

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
    char name[1100];
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
        const size_t nd = strlen(num);   // 16 digits; 10 before 2026-10-20
        if (nd < 10 || nd > 18 || strspn(num, "0123456789") != nd) continue;
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            uint64_t *p = realloc(*out, (size_t)cap * sizeof *p);
            if (!p) { rc = CB_E_NOMEM; break; }
            *out = p;
        }
        int64_t v;
        if (!num_parse_i64(num, nd, 0, INT64_MAX, &v)) continue;
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
    if (cur.version != m->base_version && j->restored) {
        // A restored copy: a write that went to the tables after the copy was taken is not
        // in it, so this game's records from here on have nothing to continue from.
        CB_WARN(MOD, "game %s is at version %lld in the restored copy and its next record is from %lld: skipped",
                m->game, (long long)cur.version, (long long)m->base_version);
        cb_game_row_free(&cur);
        return CB_OK;
    }
    if (cur.version != m->base_version) {
        snprintf(err, cap, "journal: game %s is at version %lld and the journal's next move is from %lld: a move is missing",
                 m->game, (long long)cur.version, (long long)m->base_version);
        cb_game_row_free(&cur);
        return CB_E_STATE;
    }
    jrec r;
    memset(&r, 0, sizeof r);
    r.m = *m;
    uint8_t *slog = NULL;
    size_t slog_len = 0;
    if (m->bots) rc = logs_of(j, m->game, cur.log_from, &slog, &slog_len);   // a bot's moves are seeded from it
    if (rc == CB_OK) rc = j->o.apply(j->o.apply_ud, &cur, slog, slog_len, m, &r.row, &r.log, &r.log_len);
    free(slog);
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
    if (len == 0 || buf[0] == 0) {   // made (ahead, or mapped), never written to
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
    while (rc == CB_OK && at < len && buf[at] != 0) {   // a zero magic: the mapped segment's unwritten rest
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

static const char *restored_marker(const cb_journal *j, char *out, size_t cap) {
    snprintf(out, cap, "%s" CB_STORE_RESTORED_SUFFIX, j->o.path);
    return out;
}

static void journal_free(cb_journal *j) {
    if (!j) return;
    seg_finish(&j->cur, (size_t)j->seg_len);
    seg_drop(j, &j->next);
    cb_jslots_free(j->slots);
    for (int b = 0; b < OV_BUCKETS; b++)
        while (j->buckets[b]) {
            jgame *g = j->buckets[b];
            j->buckets[b] = g->next;
            game_free(g);
        }
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    pthread_cond_destroy(&j->fcv);
    pthread_mutex_destroy(&j->ov_mu);
    pthread_mutex_destroy(&j->flush_mu);
    pthread_mutex_destroy(&j->seg_mu);
    free(j->segs);
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
    if (j->o.game_age_ms <= 0) j->o.game_age_ms = 600000;
    if (j->o.game_bytes <= 0) j->o.game_bytes = 256 << 10;
    if (j->o.total_bytes <= 0) j->o.total_bytes = 64 << 20;
    if (j->o.seg_bytes <= 0) j->o.seg_bytes = 8 << 20;
    snprintf(j->prefix, sizeof j->prefix, "%s-journal.", o->path);
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    pthread_cond_init(&j->fcv, NULL);
    pthread_mutex_init(&j->ov_mu, NULL);
    pthread_mutex_init(&j->flush_mu, NULL);
    pthread_mutex_init(&j->seg_mu, NULL);
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

    char marker[1200];
    j->restored = access(restored_marker(j, marker, sizeof marker), F_OK) == 0;
    uint64_t *segs = NULL;
    int n = 0;
    int rc = list_segments(j, &segs, &n);
    if (rc != CB_OK) snprintf(err, cap, "journal: the directory of %s does not list", o->path);
    for (int i = 0; rc == CB_OK && i < n; i++) rc = segs_add(j, segs[i]);
    for (int i = 0; rc == CB_OK && i < n; i++) rc = recover_segment(j, segs[i], err, cap);
    if (rc == CB_OK) {
        // The first new segment's number is the wall clock's ms (past every old one): a
        // number is never used twice, so a backup's copy of a segment is never another's.
        uint64_t next = (uint64_t)cb_now_ms();
        if (n && segs[n - 1] + 1 > next) next = segs[n - 1] + 1;
        j->seg_seq = next - 1;
        // Everything recovered goes into the tables before anything is served, and the
        // segments with it.
        snapshot sn;
        pthread_mutex_lock(&j->ov_mu);
        rc = snapshot_take(j, &sn, true, cb_mono_ms());
        pthread_mutex_unlock(&j->ov_mu);
        if (rc == CB_OK && sn.n) rc = cb_rows_sql_flush(s, sn.f, sn.n, NULL, NULL, 0);
        snapshot_free(&sn);
        if (rc != CB_OK) snprintf(err, cap, "journal: the recovered moves do not write (%s)", cb_strerror(rc));
    }
    if (rc == CB_OK) {
        for (int b = 0; b < OV_BUCKETS; b++)
            while (j->buckets[b]) ov_unlink(j, &j->buckets[b]);
        if ((rc = seg_open(j, write_need(1))) != CB_OK) snprintf(err, cap, "journal: no segment can be made beside %s", o->path);
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
    s->app_exclusive = j_exclusive;
    s->app_backup_mark = j_backup_mark;
    s->app_backup_files = j_backup_files;
    s->app_backup_release = j_backup_release;
    if (j->restored) (void)unlink(restored_marker(j, marker, sizeof marker));   // replayed and in the tables
    return CB_OK;
}

void cb_journal_close(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    if (!j) return;
    pthread_mutex_lock(&j->mu);
    j->stop = true;
    pthread_cond_signal(&j->fcv);
    pthread_mutex_unlock(&j->mu);
    if (j->thr_started) pthread_join(j->thr, NULL);
    const unsigned long wal = cb_store_local_wal_frames(s);   // what the moves cost the tables before the close's flush
    const long waiting = j->n_waiting;
    int rc = flush_all(j);
    if (rc == CB_OK) delete_segments_before(j, j->seg_seq + 1);
    else CB_ERROR(MOD, "the last flush failed (%s): the segments stay for the next start", cb_strerror(rc));
    CB_INFO(MOD, "%ld record(s) in %ld batch(es), %ld write(s), %ld byte(s); %ld flush(es) of %ld move(s); "
            "%lu WAL frame(s) before the close's flush of %ld move(s)",
            (long)j->records, (long)j->batches, (long)j->writes, (long)j->bytes, (long)j->flushes, (long)j->flushed_moves,
            wal, waiting);
    s->app_state = NULL;
    s->app_settle = NULL;
    s->app_exclusive = NULL;
    s->app_backup_mark = NULL;
    s->app_backup_files = NULL;
    s->app_backup_release = NULL;
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
    pthread_mutex_lock(&j->ov_mu);
    out->waiting = j->n_waiting;
    out->waiting_bytes = j->w_bytes;
    pthread_mutex_unlock(&j->ov_mu);
    pthread_mutex_lock(&j->seg_mu);
    out->segments = j->n_segs;
    pthread_mutex_unlock(&j->seg_mu);
    out->recovered = j->recovered;
}

// journal.c - see journal.h.
#include "store/journal.h"
#include "store/store_sql.h"

#include "app/foolish_app.h"   // CB_EDGE_BOARD: a flushed rated end moved the leaderboard
#include "edge.h"
#include "net/loop.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"
#include "roster.h"
#include "table.h"
#include "../../../../shared/c/clock.h"
#include "../../../../shared/c/crc32c/crc32c.h"
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
#define FORMAT 2   // 2: the fixed record and CRC-32C (1: the replay coder and FNV-1a)
#define BATCH_MAGIC 'J'
#define FULL_MAGIC 'R'           // a whole commit (journal.h "A FULL RECORD")
#define FULL_HDR 12
#define OV_BUCKETS 1024
#define OV_MAX_GAMES 4096       // opts.max_games' default
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

#define SLOT_MAX 0xFFFF   // a record names a slot in a u16

static int slot_add(cb_jslots *t, const char *id, int64_t next) {
    if (t->n >= SLOT_MAX) return CB_E_FULL;
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

static void put_u16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

// A frame's checksum: CRC-32C over its 4 header bytes and its payload.
static uint32_t batch_sum(const uint8_t *hdr, const uint8_t *payload, size_t n) {
    return crc32c(crc32c(0, hdr, 4), payload, n);
}

#define REC_BOTS 1   // a bot cycle
#define REC_NEW  2   // a game the segment has not named: its id follows
#define REC_VER  4   // a version follows: the record does not follow the slot's last one

_Static_assert(CB_JOURNAL_BATCH_BYTES - CB_JOURNAL_BATCH_HDR <= 0xFFFF, "a batch's payload length is a u16");

int cb_journal_batch_encode(cb_jslots *t, int64_t seg_start_ms, const cb_jmove *const *m, int n, uint8_t *out, int cap) {
    if (n < 1 || n > CB_JOURNAL_BATCH_MAX || cap < CB_JOURNAL_BATCH_HDR + 5) return CB_E_INVALID;
    int64_t base = m[0]->now_ms;
    for (int i = 1; i < n; i++) if (m[i]->now_ms < base) base = m[i]->now_ms;
    const int64_t rel = base - seg_start_ms;
    if (rel > INT32_MAX || rel < INT32_MIN) return CB_E_INVALID;   // a segment lives seconds: the caller starts a new one
    // Checked whole before the slot table moves: a refused batch consumes nothing.
    for (int i = 0; i < n; i++) {
        const cb_jmove *r = m[i];
        const size_t len = strlen(r->game);
        if (len < 1 || len > CB_GAME_ID_MAX || r->base_version < 0 || r->base_version > UINT32_MAX - 1 ||
            r->now_ms - base > UINT32_MAX || !r->mv || r->n_mv < 1 || r->n_mv > (r->bots ? EVW_DELTA_MAX_MOVES : 1))
            return CB_E_INVALID;
    }
    uint8_t *const p = out + CB_JOURNAL_BATCH_HDR;
    const int room = cap - CB_JOURNAL_BATCH_HDR;
    // What the batch changes in the slot table, undone if it is refused.
    const int n0 = t->n;
    int undo_slot[CB_JOURNAL_BATCH_MAX];
    int64_t undo_next[CB_JOURNAL_BATCH_MAX];
    int n_undo = 0;
    int at = 0;
    p[at++] = (uint8_t)n;
    put_u32(p + at, (uint32_t)(int32_t)rel), at += 4;
    int rc = CB_OK;
    for (int i = 0; i < n && rc == CB_OK; i++) {
        const cb_jmove *r = m[i];
        int slot = slot_find(t, r->game);
        const bool fresh = slot < 0, follows = !fresh && t->next[slot] == r->base_version;
        const int idl = (int)strlen(r->game);
        if (at + 1 + (fresh ? 1 + idl : 2) + (follows ? 0 : 4) + 4 > room) { rc = CB_E_FULL; break; }
        p[at++] = (uint8_t)((r->bots ? REC_BOTS : 0) | (fresh ? REC_NEW : 0) | (!fresh && !follows ? REC_VER : 0));
        if (fresh) {
            if ((slot = slot_add(t, r->game, r->base_version)) < 0) { rc = slot; break; }
            p[at++] = (uint8_t)idl;
            memcpy(p + at, r->game, (size_t)idl), at += idl;
        } else {
            put_u16(p + at, (uint32_t)slot), at += 2;
            undo_slot[n_undo] = slot;
            undo_next[n_undo++] = t->next[slot];
        }
        if (!follows) put_u32(p + at, (uint32_t)r->base_version), at += 4;
        t->next[slot] = r->base_version + 1;
        put_u32(p + at, (uint32_t)(r->now_ms - base)), at += 4;
        const int w = evwire_moves_put(r->mv, r->n_mv, p + at, room - at);
        if (w < 0) rc = w == EVW_ECAP ? CB_E_FULL : CB_E_INVALID;
        else at += w;
    }
    if (rc != CB_OK) {   // the slots this batch named go with it, and the others' versions come back
        while (n_undo > 0) n_undo--, t->next[undo_slot[n_undo]] = undo_next[n_undo];
        t->n = n0;
        memset(t->hash, 0, (size_t)t->hcap * sizeof *t->hash);
        for (int i = 0; i < t->n; i++) hash_put(t, i);
        return rc;
    }
    // THE COMMIT MARKER: the header goes after the payload, and its magic byte last (a
    // release store), so a segment's mapped pages cut anywhere before it hold a zero where
    // the batch's magic would be, which recovery reads as the segment's end.
    uint8_t h[4] = { BATCH_MAGIC, FORMAT, 0, 0 };
    put_u16(h + 2, (uint32_t)at);
    put_u32(out + 4, batch_sum(h, p, (size_t)at));
    put_u16(out + 2, (uint32_t)at);
    out[1] = FORMAT;
    __atomic_store_n(&out[0], (uint8_t)BATCH_MAGIC, __ATOMIC_RELEASE);
    return CB_JOURNAL_BATCH_HDR + at;
}

int cb_journal_batch_decode(cb_jslots *t, int64_t seg_start_ms, const uint8_t *in, size_t len, size_t *used,
                            cb_jmove *out, EvwMove (*mv)[EVW_DELTA_MAX_MOVES], int cap) {
    if (len < CB_JOURNAL_BATCH_HDR || in[0] != BATCH_MAGIC || in[1] != FORMAT) return -1;
    const size_t plen = get_u16(in + 2);
    if (CB_JOURNAL_BATCH_HDR + plen > len) return -1;   // torn
    if (get_u32(in + 4) != batch_sum(in, in + CB_JOURNAL_BATCH_HDR, plen)) return -1;
    const uint8_t *p = in + CB_JOURNAL_BATCH_HDR;
    const int end = (int)plen;
    if (end < 5) return -1;
    const int n = p[0];
    const int64_t base = seg_start_ms + (int32_t)get_u32(p + 1);
    if (n < 1 || n > cap || n > CB_JOURNAL_BATCH_MAX) return -1;
    int at = 5;
    for (int i = 0; i < n; i++) {
        cb_jmove *r = &out[i];
        memset(r, 0, sizeof *r);
        if (at + 1 > end) return -1;
        const int flags = p[at++];
        if (flags & ~(REC_BOTS | REC_NEW | REC_VER) || (flags & REC_NEW && flags & REC_VER)) return -1;
        int slot;
        if (flags & REC_NEW) {
            if (at + 1 > end) return -1;
            const int idl = p[at++];
            if (idl < 1 || idl > CB_GAME_ID_MAX || at + idl + 4 > end) return -1;
            memcpy(r->game, p + at, (size_t)idl), at += idl;
            if ((int)strlen(r->game) != idl || slot_find(t, r->game) >= 0) return -1;
            r->base_version = get_u32(p + at), at += 4;
            if ((slot = slot_add(t, r->game, r->base_version)) < 0) return -1;
        } else {
            if (at + 2 > end) return -1;
            slot = (int)get_u16(p + at), at += 2;
            if (slot >= t->n) return -1;
            snprintf(r->game, sizeof r->game, "%s", t->ids[slot]);
            r->base_version = t->next[slot];
            if (flags & REC_VER) {
                if (at + 4 > end) return -1;
                r->base_version = get_u32(p + at), at += 4;
            }
        }
        t->next[slot] = r->base_version + 1;
        if (at + 4 > end) return -1;
        r->now_ms = base + get_u32(p + at), at += 4;
        r->bots = flags & REC_BOTS;
        const int w = evwire_moves_read(p + at, end - at, mv[i], EVW_DELTA_MAX_MOVES, &r->n_mv);
        if (w < 0 || (!r->bots && r->n_mv != 1)) return -1;
        r->mv = mv[i];
        at += w;
    }
    if (at != end) return -1;
    *used = CB_JOURNAL_BATCH_HDR + plen;
    return n;
}

// ---- the overlay: each journaled game's acknowledged state ----------------------------------

// A waiting move: its version and where its log delta is in the game's log buffer.
typedef struct {
    int64_t  seq;
    uint32_t off, len;   // the log delta's bytes in jgame.logs
    bool     clear;      // (a whole commit) it clears the session log first
} jmove;

typedef struct jgame {
    struct jgame *next;
    // THE GAME, decoded (table.h TableResident): valid when res_valid. An operation adopts a
    // copy (table_adopt), and only a committed operation replaces it (R1: copy-on-apply).
    TableResident res;
    bool          res_valid;
    // The newest acknowledged row (no logs). Its columns are always this game's; its state and
    // roster bytes are when row_fresh, else the resident's encode is due (row_refresh) before
    // anything reads them (R3).
    cb_game_row   row;
    bool          row_fresh;
    int64_t       sql_version;   // the version the tables hold
    jmove        *moves;         // the moves since sql_version, in order
    int           n_moves, cap_moves;
    // THE WAITING MOVES' LOG DELTAS, one grown buffer: moves[i] is at logs + moves[i].off, the
    // deltas back to back in move order (a flush cuts a prefix). One buffer per game instead of
    // one allocation per move: no malloc on the move path and no allocator header per delta.
    uint8_t      *logs;
    size_t        logs_len, logs_cap;
    // The waiting rated ends (a game rates once per session, so a few at most), each owned, by the
    // version of the commit that rated: the flush writes its rating rows with that commit.
    struct { int64_t seq; cb_game_rated *r; } *rated;
    int           n_rated, cap_rated;
    long          bytes;         // what the waiting moves hold (move_bytes each)
    size_t        state_cap, roster_cap;   // row.state's and row.roster's buffers
    int64_t       touched_ms;    // cb_mono_ms of its last move
    int64_t       first_ms;      // cb_mono_ms of its oldest waiting move
    uint64_t      first_seg;     // a segment at or before the one holding its oldest waiting move
    int           n_humans;      // the humans its roster seats (the dashboard list's members)
    cb_uuid       humans[CB_MAX_SEATS];
    int           n_bots;        // and its bots
    cb_uuid       bots[CB_MAX_SEATS];
    // A waiting commit changed the seats (a whole commit: journal.h): the flush brings the
    // tables' to these. seats_seq: the newest such commit's version.
    bool          seats_dirty;
    int64_t       seats_seq;
} jgame;

static long move_bytes(const jmove *m) { return (long)(sizeof *m + m->len); }

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
    cb_game_row  row;           // the new row (the caller's bytes: ov_apply copies them; only its columns with res)
    const TableResident *res;   // the new game decoded (the caller's), or NULL (a recovered move: row's bytes)
    bool         full;          // a whole commit (FULL_MAGIC): the row's bytes are written, not m's moves
    bool         clear;         // (full) it clears the session log first
    const uint8_t *log;         // the commit's log delta (the caller's: ov_apply copies it)
    const cb_game_rated *rated; // (full) the commit rated the game: its rating changes (the caller's), else NULL
    size_t       log_len;
    int          rc;
    bool         done;
} jrec;

// THE RATINGS' ONE OWNER under serve: a seat's rating and its account's age, as the tables hold
// them plus every rated end the journal acknowledged and has not flushed yet. A rated end reads
// its seats here (cb_journal_seat_ratings), so it runs on the loop with no table read, and its
// changes land here at its commit (ov_apply) and in the tables at its game's flush. An entry is
// read from the tables only when absent (off the loop), and goes only when nothing of it waits
// (pending 0) and it is idle (ov_evict), or at an admin's write.
typedef struct jrating {
    struct jrating *next;
    cb_uuid  id;
    int32_t  rating;       // TABLE_RATING_NONE: no rating row
    int64_t  created_ms;   // the account's created_at; 0: no account (a bot, or deleted)
    int      pending;      // waiting rated ends that change it
    int64_t  touched_ms;
} jrating;

struct cb_journal {
    cb_store       *s;
    cb_journal_opts o;
    char            prefix[1100];   // <db>-journal.
    // the writer: the queue and the one batch in flight
    pthread_mutex_t mu;
    pthread_cond_t  cv;            // the writers: a batch written, the writer free (waiters: how many wait)
    pthread_cond_t  fcv;           // the flush thread: a kick, a segment to make ahead, the stop
    int             waiters;
    const cb_jmove *ms[CB_JOURNAL_BATCH_MAX];   // the writer's (held): one batch's records, borrowed
    jrec           *head, *tail;
    bool            writing;
    size_t          prepare_need;  // the loop's write waits for a segment this big (mu): the flush thread makes it
    atomic_bool     loop_gone;     // the loop refused a post (it stopped): its writer role is over
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
    jrating        *ratings[OV_BUCKETS];   // (ov_mu) THE RATINGS' ONE OWNER, above
    int             n_ratings;
    // the flush: one at a time, and every write of a journaled game around the journal
    pthread_mutex_t flush_mu;
    pthread_t       thr;
    bool            thr_started, stop, kick, prepare, rearm;
    // THE FLUSH THREAD HAS NO TICK: it sleeps until the oldest waiting game is game_age_ms old
    // (armed_ms, cb_mono_ms; INT64_MAX: nothing waits, it sleeps until told), or a kick, a segment
    // to make, a rearm (ov_apply: a game began waiting before armed_ms), the stop. An idle box
    // wakes nobody (OPTIMIZED_DESIGN.md 4.3 E4).
    int64_t         armed_ms;      // (ov_mu)
    bool            rearm_wanted;  // (ov_mu) ov_apply asks write_batch to rearm the flush thread
    atomic_long     flusher_wakes;
    // a backup attached (the backup hooks below): no segment at or after backup_mark is deleted
    bool            pinned;
    bool            restored;      // the database is a restored copy (store.h CB_STORE_RESTORED_SUFFIX)
    uint64_t        backup_mark;
    // bumped by an exclusive section (an admin write): a record whose base was read before it is refused
    atomic_long     epoch;
    // stats
    atomic_long     batches, writes, records, bytes, flushes, flushed_games, flushed_moves, recovered, table_reads, log_reads;
    atomic_long     loop_writes;   // writes made on the loop's thread
    atomic_long     loop_seg_opens;   // seg_open on the loop's thread (a wait for seg_mu, a file made)
    atomic_long     txn_moves_max;    // the most waiting moves one flush transaction carried
    atomic_long     txn_hold_us_max;  // the longest a flush transaction held the flush lock
};

cb_journal *cb_journal_of(const cb_store *s) { return s && s->app_settle == cb_journal_flush ? s->app_state : NULL; }

// On the network loop's thread (journal.h opts.loop), where nothing may wait: every read or write
// of the tables below returns CB_E_WOULD_BLOCK there instead, before it has done anything.
static bool on_loop(const cb_journal *j) { return j->o.loop && cb_loop_on_thread(j->o.loop); }

static jgame **ov_slot(cb_journal *j, const char *id) {
    jgame **p = &j->buckets[fnv1a32_str(FNV32_OFFSET, id) & (OV_BUCKETS - 1)];
    while (*p && strcmp((*p)->row.id, id) != 0) p = &(*p)->next;
    return p;
}

static jrating **rating_slot(cb_journal *j, const char *id) {
    jrating **p = &j->ratings[fnv1a32_str(FNV32_OFFSET, id) & (OV_BUCKETS - 1)];
    while (*p && strcmp((*p)->id, id) != 0) p = &(*p)->next;
    return p;
}

// A rated end's changes, applied where the next rated end reads them (ov_mu held): the same
// arithmetic as the tables' write (rows_sql.c rate_rows: max(0, rating + delta); a seat with no
// row starts at TABLE_RATING_START), once per id. An id the cache does not hold was read through
// it by the commit's own operation moments before (an entry goes only after IDLE_EVICT_MS idle):
// none is the tables' alone, and is read from them after the flush.
static void ratings_apply(cb_journal *j, const cb_game_rated *r, int64_t now) {
    for (int i = 0; i < r->n; i++) {
        bool dup = false;
        for (int k = 0; k < i; k++) dup |= strcmp(r->ids[k], r->ids[i]) == 0;
        jrating *e = dup ? NULL : *rating_slot(j, r->ids[i]);
        if (!e) continue;
        const int32_t base = e->rating == TABLE_RATING_NONE ? TABLE_RATING_START : e->rating;
        if (e->rating == TABLE_RATING_NONE && !r->is_bot[i] && !e->created_ms) { e->pending++; continue; }   // no user: no row made
        const int64_t v = (int64_t)base + r->delta[i];
        e->rating = v < 0 ? 0 : (int32_t)v;
        e->pending++;
        e->touched_ms = now;
    }
}

// A rated end is in the tables now: its entries may go (ov_mu held).
static void ratings_flushed(cb_journal *j, const cb_game_rated *r) {
    for (int i = 0; i < r->n; i++) {
        bool dup = false;
        for (int k = 0; k < i; k++) dup |= strcmp(r->ids[k], r->ids[i]) == 0;
        jrating *e = dup ? NULL : *rating_slot(j, r->ids[i]);
        if (e && e->pending > 0) e->pending--;
    }
}

// Every entry with nothing waiting that is idle (`all`: every one; ov_mu held).
static void ratings_evict(cb_journal *j, int64_t now, bool all) {
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jrating **p = &j->ratings[b]; *p;) {
            jrating *e = *p;
            if (all || (!e->pending && now - e->touched_ms > IDLE_EVICT_MS)) {
                *p = e->next;
                free(e);
                j->n_ratings--;
            } else {
                p = &e->next;
            }
        }
}

static void game_free(jgame *g) {
    for (int i = 0; i < g->n_rated; i++) free(g->rated[i].r);
    free(g->rated);
    cb_game_row_free(&g->row);
    free(g->logs);
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

static int row_assign(jgame *g, const cb_game_row *src);

// The row's columns become src's; its byte buffers stay the game's (row_refresh fills them).
static void row_set_columns(jgame *g, const cb_game_row *src) {
    cb_game_row r = *src;
    r.state = g->row.state;
    r.state_len = g->row.state_len;
    r.roster = g->row.roster;
    r.roster_len = g->row.roster_len;
    r.logs = NULL;
    r.logs_len = 0;
    g->row = r;
}

// The row's bytes brought to the resident's (R3: once per commit, and only when read).
static int row_refresh(jgame *g) {
    if (g->row_fresh || !g->res_valid) return CB_OK;
    if (g->state_cap < TABLE_STATE_MAX) {
        uint8_t *q = realloc(g->row.state, TABLE_STATE_MAX);
        if (!q) return CB_E_NOMEM;
        g->row.state = q;
        g->state_cap = TABLE_STATE_MAX;
    }
    if (g->roster_cap < ROSTER_BYTES) {
        uint8_t *q = realloc(g->row.roster, ROSTER_BYTES);
        if (!q) return CB_E_NOMEM;
        g->row.roster = q;
        g->roster_cap = ROSTER_BYTES;
    }
    int sl = 0, rl = 0;
    if (table_resident_encode(&g->res, g->row.state, (int)g->state_cap, &sl, g->row.roster, (int)g->roster_cap, &rl) != TABLE_OK)
        return CB_E_STATE;
    g->row.state_len = (size_t)sl;
    g->row.roster_len = (size_t)rl;
    g->row_fresh = true;
    return CB_OK;
}

// The game becomes `src` and, when given, its resident (both acknowledged together): with a
// resident only the columns are copied and the bytes encoded when read; without one the bytes
// are copied and the next operation decodes them.
static int game_set(jgame *g, const cb_game_row *src, const TableResident *res) {
    if (res) {
        g->res = *res;
        g->res_valid = true;
        g->row_fresh = false;
        row_set_columns(g, src);
        return CB_OK;
    }
    g->res_valid = false;
    g->row_fresh = true;
    return row_assign(g, src);
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

// The humans a roster seats (the dashboard list's members).
static void humans_of(jgame *g, const Roster *ro) {
    g->n_humans = g->n_bots = 0;
    for (int i = 0; i < ro->n && i < CB_MAX_SEATS; i++) {
        const bool human = ro->seats[i].brain_len == 0;
        snprintf(human ? g->humans[g->n_humans++] : g->bots[g->n_bots++], sizeof(cb_uuid), "%.*s", (int)ro->seats[i].id_len,
                 (const char *)ro->seats[i].id);
    }
}

static void humans_of_roster(jgame *g, const uint8_t *roster, size_t len) {
    Roster ro;
    if (roster_decode(&ro, roster, (int)len) == ROSTER_OK) humans_of(g, &ro);
    else g->n_humans = g->n_bots = 0;
}

// A game made resident at *p (ov_mu held, *p empty): `row` as the tables hold it (and its
// resident when the caller has one).
static jgame *ov_insert(cb_journal *j, jgame **p, const cb_game_row *row, const TableResident *res) {
    jgame *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    if (game_set(g, row, res) != CB_OK) { game_free(g); return NULL; }
    g->sql_version = row->version;
    g->touched_ms = cb_mono_ms();
    if (res) humans_of(g, &res->r);
    else humans_of_roster(g, row->roster, row->roster_len);
    *p = g;
    j->n_games++;
    return g;
}

// The flush thread's pass now rather than at its tick (a game due, or the overlay over its cap).
static void kick_flusher(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    j->kick = true;
    pthread_cond_signal(&j->fcv);
    pthread_mutex_unlock(&j->mu);
}

// A record's row and log become the game's (ov_mu held). Both are copied: the log onto the end
// of the game's log buffer.
static int ov_apply(cb_journal *j, jrec *r) {
    jgame **p = ov_slot(j, r->m.game), *g = *p;
    if (!g) {
        // A game no operation read first (recovery, or one evicted between its read and its
        // commit): its base is the tables'. A journaled move never changes the seats.
        if (!(g = calloc(1, sizeof *g))) return CB_E_NOMEM;
        g->sql_version = r->m.base_version;
        if (r->res) humans_of(g, &r->res->r);
        else humans_of_roster(g, r->row.roster, r->row.roster_len);
        *p = g;
        j->n_games++;
    }
    if (g->n_moves == g->cap_moves) {
        int cap = g->cap_moves ? g->cap_moves * 2 : 8;
        jmove *m = realloc(g->moves, (size_t)cap * sizeof *m);
        if (!m) return CB_E_NOMEM;
        g->moves = m;
        g->cap_moves = cap;
    }
    if (r->log_len > UINT32_MAX - g->logs_len) return CB_E_NOMEM;   // an offset is 32 bits
    if (g->logs_len + r->log_len > g->logs_cap) {
        size_t cap = g->logs_cap ? g->logs_cap : 256;
        while (cap < g->logs_len + r->log_len) cap *= 2;
        uint8_t *q = realloc(g->logs, cap);
        if (!q) return CB_E_NOMEM;
        g->logs = q;
        g->logs_cap = cap;
    }
    cb_game_rated *rated = NULL;
    if (r->rated) {
        if (g->n_rated == g->cap_rated) {
            const int cap = g->cap_rated ? g->cap_rated * 2 : 2;
            void *q = realloc(g->rated, (size_t)cap * sizeof *g->rated);
            if (!q) return CB_E_NOMEM;
            g->rated = q;
            g->cap_rated = cap;
        }
        if (!(rated = malloc(sizeof *rated))) return CB_E_NOMEM;
        *rated = *r->rated;
    }
    const int64_t now = cb_mono_ms();
    if (!g->n_moves) {
        g->first_ms = now;
        g->first_seg = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED);
        if (now + j->o.game_age_ms < j->armed_ms) j->rearm_wanted = true;   // the flush thread sleeps past its age
    }
    // The resident is the game's now (a 2 KB copy; the row's bytes are encoded when read), or a
    // recovered row's bytes go into the game's own buffers, grown only when a row outgrows them:
    // no allocation per move either way.
    if (game_set(g, &r->row, r->res) != CB_OK) { free(rated); return CB_E_NOMEM; }
    if (rated) {   // THE RATINGS' ONE OWNER takes the changes now; the tables at the flush
        g->rated[g->n_rated].seq = r->row.version;
        g->rated[g->n_rated++].r = rated;
        ratings_apply(j, rated, now);
    }
    if (r->full) {   // a whole commit may seat or unseat anyone: the flush brings the tables' seats
        if (r->res) humans_of(g, &r->res->r);
        else humans_of_roster(g, r->row.roster, r->row.roster_len);
        g->seats_dirty = true;
        g->seats_seq = r->row.version;
    }
    g->moves[g->n_moves] = (jmove){ r->row.version, (uint32_t)g->logs_len, (uint32_t)r->log_len, r->full && r->clear };
    if (r->log_len) memcpy(g->logs + g->logs_len, r->log, r->log_len);
    g->logs_len += r->log_len;
    const long b = move_bytes(&g->moves[g->n_moves++]);
    g->touched_ms = now;
    g->bytes += b;
    j->w_bytes += b;
    j->n_waiting++;
    return CB_OK;
}

// A game whose waiting moves hold game_bytes, or that holds a rated end (the leaderboard and the
// profiles read the tables: the flush follows at once): part of THE FLUSH RULE (flush_due), and the
// writer's reason to kick the flush thread at once. (The transaction a flush writes is bounded
// apart, by its one owner snapshot_take: no cap on a game's move count is wanted beside it.)
static bool game_full(const cb_journal *j, const jgame *g) { return g->bytes >= j->o.game_bytes || g->n_rated > 0; }

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

static size_t write_need(int n) {   // the most a write of n records of any size takes, its segment header too
    return SEG_HDR + (size_t)(n / CB_JOURNAL_BATCH_MAX + 1) * CB_JOURNAL_BATCH_BYTES;
}

// The most one move record takes in a batch: its fields with a new game's id and a version,
// and its move list.
static size_t record_need(const cb_jmove *m) {
    size_t n = 1 + 1 + strlen(m->game) + 4 + 4 + 1;
    for (int i = 0; i < m->n_mv; i++) n += 2 + m->mv[i].wire_len;
    return n;
}

// The next segment made ahead, so the writer's rotation only swaps maps (the flush thread's
// tick and a rotation's kick; seg_mu held by the caller).
static void prepare_next_locked(cb_journal *j, size_t need) {
    const uint64_t seq = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED) + 1;
    if (need < write_need(CB_JOURNAL_BATCH_MAX)) need = write_need(CB_JOURNAL_BATCH_MAX);
    if (j->next.map && j->next.seq == seq && j->next.size >= seg_size(j, need)) return;
    seg_drop(j, &j->next);
    (void)seg_make(j, seq, seg_size(j, need), &j->next);
}

// The flush thread's: the next segment made, big enough for what the loop last asked for.
static void prepare_next(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    const size_t need = j->prepare_need;
    j->prepare_need = 0;
    pthread_mutex_unlock(&j->mu);
    pthread_mutex_lock(&j->seg_mu);
    prepare_next_locked(j, need);
    pthread_mutex_unlock(&j->seg_mu);
}

// The open segment finished and the next one (made ahead, or now) open, able to take `need`
// bytes; its header goes with its first batch, which gives it its start time. The writer is held,
// and seg_mu.
static int seg_open_locked(cb_journal *j, size_t need) {
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
    return rc;
}

// The flush thread makes the one after the segment just opened.
static void ask_prepare(cb_journal *j, size_t need) {
    pthread_mutex_lock(&j->mu);
    j->prepare = true;
    if (need > j->prepare_need) j->prepare_need = need;
    pthread_cond_signal(&j->fcv);
    pthread_mutex_unlock(&j->mu);
}

// seg_open_locked under seg_mu (the writer held; never on the loop's thread, which may not wait
// for the flush thread making a segment: loop_write rotates itself, under a seg_mu it got at once).
static int seg_open(cb_journal *j, size_t need) {
    if (on_loop(j)) atomic_fetch_add(&j->loop_seg_opens, 1);   // only after a failed write (write_batch)
    pthread_mutex_lock(&j->seg_mu);
    int rc = seg_open_locked(j, need);
    pthread_mutex_unlock(&j->seg_mu);
    if (rc == CB_OK) ask_prepare(j, 0);
    return rc;
}

// ---- a full record: one whole commit (journal.h) --------------------------------------------

static void put_i64(uint8_t *p, int64_t v) { put_u64(p, (uint64_t)v); }
static int64_t get_i64(const uint8_t *p) { return (int64_t)get_u64(p); }

// A rated end's block (flag 8, after the log): u8 n, then per rated seat u8 id length, the id, u8
// 1 for a bot, i32 LE delta, u8 name length, the name. Its time is the record's ms.
static size_t rated_size(const cb_game_rated *g) {
    size_t n = 1;
    for (int i = 0; g && i < g->n; i++) n += 1 + strlen(g->ids[i]) + 1 + 4 + 1 + strlen(g->names[i]);
    return g ? n : 0;
}

static size_t full_size(const jrec *r) {
    return FULL_HDR + 1 + strlen(r->m.game) + 8 * 2 + 1 + 1 + 8 * 3 + (r->row.has_seed ? CB_SEED_LEN : 0) + 12 +
           r->row.state_len + r->row.roster_len + r->log_len + rated_size(r->rated);
}

// r as a full record at out (cap bytes): its length, or CB_E_FULL. The header goes last, its
// magic byte a release store: the commit marker, as a batch's.
static int full_encode(const jrec *r, uint8_t *out, size_t cap) {
    const size_t n = full_size(r);
    if (n > cap || n - FULL_HDR > UINT32_MAX) return CB_E_FULL;
    uint8_t *q = out + FULL_HDR;
    const size_t idl = strlen(r->m.game);
    const cb_game_row *w = &r->row;
    *q++ = (uint8_t)idl;
    memcpy(q, r->m.game, idl), q += idl;
    put_i64(q, r->m.base_version), q += 8;
    put_i64(q, r->m.now_ms), q += 8;
    *q++ = (uint8_t)((r->clear ? 1 : 0) | (w->has_seed ? 2 : 0) | (w->needs_bots ? 4 : 0) | (r->rated ? 8 : 0));
    *q++ = (uint8_t)(int8_t)w->status;
    put_i64(q, w->log_from), q += 8;
    put_i64(q, w->round_epoch), q += 8;
    put_i64(q, w->created_at), q += 8;
    if (w->has_seed) memcpy(q, w->seed, CB_SEED_LEN), q += CB_SEED_LEN;
    put_u32(q, (uint32_t)w->state_len), q += 4;
    if (w->state_len) memcpy(q, w->state, w->state_len), q += w->state_len;
    put_u32(q, (uint32_t)w->roster_len), q += 4;
    if (w->roster_len) memcpy(q, w->roster, w->roster_len), q += w->roster_len;
    put_u32(q, (uint32_t)r->log_len), q += 4;
    if (r->log_len) memcpy(q, r->log, r->log_len), q += r->log_len;
    if (r->rated) {
        const cb_game_rated *g = r->rated;
        *q++ = (uint8_t)g->n;
        for (int i = 0; i < g->n; i++) {
            const size_t il = strlen(g->ids[i]), nl = strlen(g->names[i]);
            *q++ = (uint8_t)il;
            memcpy(q, g->ids[i], il), q += il;
            *q++ = g->is_bot[i] ? 1 : 0;
            put_u32(q, (uint32_t)g->delta[i]), q += 4;
            *q++ = (uint8_t)nl;
            memcpy(q, g->names[i], nl), q += nl;
        }
    }
    const uint32_t plen = (uint32_t)(n - FULL_HDR);
    uint8_t h[4];
    put_u32(h, plen);
    put_u32(out + 8, plen);
    put_u32(out + 4, batch_sum(h, out + FULL_HDR, plen));
    out[2] = out[3] = 0;
    out[1] = FORMAT;
    __atomic_store_n(&out[0], (uint8_t)FULL_MAGIC, __ATOMIC_RELEASE);
    return (int)n;
}

// The full record at in: *r's game, versions, row (its byte fields point into in) and log, and a
// rated end's changes into *rated (r->rated then points at it). Its length, or -1 for one that is
// torn or corrupt.
static long full_decode(const uint8_t *in, size_t len, jrec *r, cb_game_rated *rated) {
    memset(r, 0, sizeof *r);
    if (len < FULL_HDR || in[0] != FULL_MAGIC || in[1] != FORMAT) return -1;
    const uint32_t plen = get_u32(in + 8);
    if (plen > len - FULL_HDR || get_u32(in + 4) != batch_sum(in + 8, in + FULL_HDR, plen)) return -1;
    const uint8_t *q = in + FULL_HDR, *end = q + plen;
#define NEED(k) do { if ((size_t)(end - q) < (size_t)(k)) return -1; } while (0)
    NEED(1);
    const size_t idl = *q++;
    if (idl < 1 || idl > CB_GAME_ID_MAX) return -1;
    NEED(idl + 8 * 2 + 2 + 8 * 3);
    memcpy(r->m.game, q, idl), q += idl;
    if (strlen(r->m.game) != idl) return -1;
    r->m.base_version = get_i64(q), q += 8;
    r->m.now_ms = get_i64(q), q += 8;
    const uint8_t flags = *q++;
    cb_game_row *w = &r->row;
    cb_strlcpy(w->id, r->m.game, sizeof w->id);
    w->status = (int8_t)*q++;
    w->log_from = get_i64(q), q += 8;
    w->round_epoch = get_i64(q), q += 8;
    w->created_at = get_i64(q), q += 8;
    r->clear = flags & 1;
    w->has_seed = flags & 2;
    w->needs_bots = flags & 4;
    if (w->has_seed) {
        NEED(CB_SEED_LEN);
        memcpy(w->seed, q, CB_SEED_LEN);
        q += CB_SEED_LEN;
    }
    size_t n[3];
    const uint8_t *p[3];
    for (int i = 0; i < 3; i++) {
        NEED(4);
        n[i] = get_u32(q), q += 4;
        NEED(n[i]);
        p[i] = q, q += n[i];
    }
    if (flags & 8) {
        memset(rated, 0, sizeof *rated);
        NEED(1);
        rated->n = *q++;
        if (rated->n < 1 || rated->n > CB_MAX_SEATS) return -1;
        for (int i = 0; i < rated->n; i++) {
            NEED(1);
            const size_t il = *q++;
            if (il < 1 || il >= sizeof rated->ids[i]) return -1;
            NEED(il + 1 + 4 + 1);
            memcpy(rated->ids[i], q, il), q += il;
            if (strlen(rated->ids[i]) != il) return -1;
            rated->is_bot[i] = *q++ != 0;
            rated->delta[i] = (int32_t)get_u32(q), q += 4;
            const size_t nl = *q++;
            if (nl >= sizeof rated->names[i]) return -1;
            NEED(nl);
            memcpy(rated->names[i], q, nl), q += nl;
            if (strlen(rated->names[i]) != nl) return -1;
        }
        rated->at = r->m.now_ms;
        r->rated = rated;
    }
    if (q != end) return -1;
#undef NEED
    w->state = (uint8_t *)p[0], w->state_len = n[0];
    w->roster = (uint8_t *)p[1], w->roster_len = n[1];
    r->log = p[2], r->log_len = n[2];
    w->version = r->m.base_version + 1;
    w->updated_at = w->last_commit_at = r->m.now_ms;
    w->seats_exact = true;
    r->full = true;
    return (long)(FULL_HDR + plen);
}

// Whether a write of `need` bytes, its first record at `first`, must open another segment (the
// writer held): the open one is full, too small for it, or its start out of the batch clock's reach.
static bool rotates(const cb_journal *j, size_t need, int64_t first) {
    return !j->cur.map || j->seg_len >= j->o.seg_bytes || (size_t)j->seg_len + need > j->cur.size ||
           (j->seg_len > 0 && (first - j->seg_start > INT32_MAX / 2 || first - j->seg_start < INT32_MIN / 2));
}

// The most a write of `list` takes (its records with rc CB_OK only, or all of them), and its
// earliest record's time: a whole commit is its own frame, and each one may cut a run of moves
// into another batch.
static size_t batch_need(const jrec *list, bool only_ok, int64_t *first) {
    size_t need = SEG_HDR;
    *first = INT64_MAX;
    for (const jrec *r = list; r; r = r->next) {
        if (only_ok && r->rc != CB_OK) continue;
        // A move record counts a batch's header of its own: the most a run cut by whole
        // commits and the batch limit can take.
        need += r->full ? full_size(r) : CB_JOURNAL_BATCH_HDR + 5 + record_need(&r->m);
        if (r->m.now_ms < *first) *first = r->m.now_ms;
    }
    return need;
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
    const cb_jmove **const ms = j->ms;
    int rc = CB_OK;
    int64_t first;
    const size_t need = batch_need(list, true, &first);
    size_t len = 0;
    int frames = 0;
    if (rc == CB_OK && rotates(j, need, first)) rc = seg_open(j, need);
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
        if (r->rc != CB_OK) { r = r->next; continue; }
        if (r->full) {
            int w = full_encode(r, at + len, room - len);
            if (w < 0) rc = w;
            else len += (size_t)w, frames++;
            r = r->next;
            continue;
        }
        int k = 0;
        for (; r && k < CB_JOURNAL_BATCH_MAX && !(r->rc == CB_OK && r->full); r = r->next) if (r->rc == CB_OK) ms[k++] = &r->m;
        if (!k) continue;
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
        if (on_loop(j)) atomic_fetch_add(&j->loop_writes, 1);
        atomic_fetch_add(&j->records, n);
        atomic_fetch_add(&j->bytes, (long)len);
    } else if (at) {
        // Nothing of this write is acknowledged: whatever it put past the segment's last
        // whole batch is zeroed (a batch committed before the failure would replay without an
        // acknowledgement), and the slot table that may have moved on goes with a new segment.
        memset(at, 0, len);
        if (seg_open(j, need) != CB_OK) seg_finish(&j->cur, (size_t)j->seg_len);   // the next write tries again
    }
    pthread_mutex_lock(&j->ov_mu);
    bool due = false;
    for (jrec *r = list; r; r = r->next) {
        if (r->rc != CB_OK) continue;
        r->rc = rc;
        if (rc == CB_OK && (r->rc = ov_apply(j, r)) != CB_OK) CB_ERROR(MOD, "game %s: overlay out of memory", r->m.game);
        jgame *g = rc == CB_OK ? *ov_slot(j, r->m.game) : NULL;
        if (g && game_full(j, g)) due = true;
    }
    if (j->w_bytes >= j->o.total_bytes) due = true;
    const bool rearm = j->rearm_wanted && !due;
    j->rearm_wanted = false;
    pthread_mutex_unlock(&j->ov_mu);
    if (due) kick_flusher(j);   // the flush thread applies the rule now
    else if (rearm) {           // a game began waiting before the flush thread's alarm: it sets the alarm again
        pthread_mutex_lock(&j->mu);
        j->rearm = true;
        pthread_cond_signal(&j->fcv);
        pthread_mutex_unlock(&j->mu);
    }
}

// ---- the writer ----------------------------------------------------------------------------
//
// THE LOOP IS THE ONLY WRITER while it runs (journal.h opts.loop). A record committed on its thread
// is written there at once, one record (and whatever another thread handed it) per batch; a record
// committed on any other thread (a bot drive, a pooled request) is queued, the loop is told
// (cb_loop_post: one wake per bot cycle, none per human move), and that thread waits for the
// loop's write. The loop itself never waits, and each case is its own guard:
//   - the writer held by the flush thread (a snapshot) or an admin's write: the loop's own record
//     is refused (CB_E_WOULD_BLOCK: the request runs again on the pool) and the queued ones wait
//     for the release, which tells the loop again (writer_release);
//   - a segment to open that the flush thread has not made ahead: the loop makes no file; it asks
//     the flush thread for one, which tells the loop once it is made (flusher), and its own
//     record is refused as above;
//   - each write synced (opts.sync: an msync per batch waits on the disk): the loop writes nothing
//     and every commit on it is refused.
// Where there is no loop, before it runs, once it has stopped (a post refused), or with sync, the
// writer is whichever committing thread finds it free (group commit: append).

static bool loop_writes(const cb_journal *j) {
    return j->o.loop && !j->o.sync && cb_loop_running(j->o.loop) && !atomic_load(&j->loop_gone);
}

static void loop_drain(cb_loop *l, void *ud);

// Tells the loop to write what is queued (any thread). false once it has stopped: the waiters are
// woken, and write their own.
static bool loop_tell(cb_journal *j) {
    if (cb_loop_post(j->o.loop, loop_drain, j) == CB_OK) return true;
    atomic_store(&j->loop_gone, true);
    pthread_mutex_lock(&j->mu);
    if (j->waiters) pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    return false;
}

// The loop's write of `list` (the writer taken, j->mu not held): false, with nothing written, when
// it would open a segment the flush thread has not made yet (it is asked to).
static bool loop_write(cb_journal *j, jrec *list) {
    int64_t first;
    const size_t need = batch_need(list, false, &first);   // every record: at least what write_batch needs
    if (rotates(j, need, first)) {
        // Only into a segment made ahead, and only if the flush thread is not making one now: the
        // rotation is made here under the seg_mu taken at once, so write_batch has none to make.
        bool ready = false;
        if (pthread_mutex_trylock(&j->seg_mu) == 0) {
            const uint64_t seq = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED) + 1;
            ready = j->next.map && j->next.seq == seq && j->next.size >= need && seg_open_locked(j, need) == CB_OK;
            pthread_mutex_unlock(&j->seg_mu);
        }
        ask_prepare(j, ready ? 0 : need);   // the one after it, or the one this write waits for
        if (!ready) return false;
    }
    write_batch(j, list);
    return true;
}

// The writer, taken by the loop with every queued record (j->mu held): NULL when another holds it.
static jrec *loop_take(cb_journal *j, jrec *own) {
    if (j->writing) return NULL;
    jrec *list = j->head;
    if (own) {
        if (j->tail) j->tail->next = own;
        else list = own;
    }
    j->head = j->tail = NULL;
    j->writing = true;
    return list;
}

// The writer given back by the loop (j->mu held): the written records done, the rest queued again
// in front of anything queued since, in their order.
static void loop_give(cb_journal *j, jrec *list, bool written, jrec *own) {
    if (written) {
        for (jrec *x = list; x; x = x->next) x->done = true;
    } else {
        jrec *keep = NULL, **at = &keep, *last = NULL;
        for (jrec *x = list, *nx; x; x = nx) {
            nx = x->next;
            if (x == own) continue;
            x->next = NULL;
            *at = x;
            at = &x->next;
            last = x;
        }
        if (keep) {
            last->next = j->head;
            if (!j->head) j->tail = last;
            j->head = keep;
        }
    }
    j->writing = false;
    if (j->waiters) pthread_cond_broadcast(&j->cv);
}

// A commit on the loop's thread: written now, or CB_E_WOULD_BLOCK with nothing written.
static int loop_append(cb_journal *j, jrec *r) {
    if (!loop_writes(j)) return CB_E_WOULD_BLOCK;   // synced writes wait on the disk
    pthread_mutex_lock(&j->mu);
    jrec *list = loop_take(j, r);
    pthread_mutex_unlock(&j->mu);
    if (!list) return CB_E_WOULD_BLOCK;   // the writer is held
    const bool written = loop_write(j, list);
    pthread_mutex_lock(&j->mu);
    loop_give(j, list, written, r);
    pthread_mutex_unlock(&j->mu);
    return written ? r->rc : CB_E_WOULD_BLOCK;
}

// Posted by loop_tell: the queued records written (on the loop), unless the writer is held or the
// segment is not made yet (each tells the loop again when that is over).
static void loop_drain(cb_loop *l, void *ud) {
    (void)l;
    cb_journal *j = ud;
    pthread_mutex_lock(&j->mu);
    jrec *list = j->head ? loop_take(j, NULL) : NULL;
    pthread_mutex_unlock(&j->mu);
    if (!list) return;
    const bool written = loop_write(j, list);
    pthread_mutex_lock(&j->mu);
    loop_give(j, list, written, NULL);
    pthread_mutex_unlock(&j->mu);
}

// Appends r and returns once its batch is written (any thread but the loop's): the loop writes it
// while it runs; else the first waiter with no batch in flight writes every record queued.
static int append(cb_journal *j, jrec *r) {
    pthread_mutex_lock(&j->mu);
    if (j->tail) j->tail->next = r;
    else j->head = r;
    j->tail = r;
    bool told = false;
    while (!r->done) {
        if (!told && loop_writes(j)) {
            pthread_mutex_unlock(&j->mu);
            told = loop_tell(j);
            pthread_mutex_lock(&j->mu);
            continue;
        }
        if (told && loop_writes(j)) {
            // The loop writes it. A timed wait: a wake lost to a loop stopping mid-way costs a tick.
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000;
            if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
            j->waiters++, pthread_cond_timedwait(&j->cv, &j->mu, &ts), j->waiters--;
            continue;
        }
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

// The writer, held (no batch in flight while the caller rotates). Never the loop's thread: it
// waits for the batch in flight.
static void writer_hold(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    while (j->writing) j->waiters++, pthread_cond_wait(&j->cv, &j->mu), j->waiters--;
    j->writing = true;
    pthread_mutex_unlock(&j->mu);
}

// The writer given back; records queued meanwhile are the loop's to write: it is told.
static void writer_release(cb_journal *j) {
    pthread_mutex_lock(&j->mu);
    j->writing = false;
    if (j->waiters) pthread_cond_broadcast(&j->cv);
    const bool tell = j->head && loop_writes(j);
    pthread_mutex_unlock(&j->mu);
    if (tell) loop_tell(j);
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
    if (k) memmove(j->segs, j->segs + k, (size_t)(j->n_segs - k) * sizeof *j->segs);   // (segs is NULL before the first)
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
    cb_uuid humans[CB_MAX_SEATS], bots[CB_MAX_SEATS];
} seats_copy;

typedef struct {
    cb_game_flush *f;
    cb_game_row   *rows;
    seats_copy    *seats;
    int            n;
} snapshot;

static void snapshot_free(snapshot *sn) {
    for (int i = 0; i < sn->n; i++) {
        cb_game_row_free(&sn->rows[i]);
        free((void *)sn->f[i].moves);
    }
    free(sn->f);
    free(sn->rows);
    free(sn->seats);
    memset(sn, 0, sizeof *sn);
}

// The seats a flush of g brings the tables to, when a waiting commit changed them (ov_mu held,
// or the game's lock and the flush lock). The copy is the caller's.
static void flush_seats(const jgame *g, cb_game_flush *f, seats_copy *c) {
    if (!g->seats_dirty) return;
    memcpy(c->humans, g->humans, sizeof c->humans);
    memcpy(c->bots, g->bots, sizeof c->bots);
    f->seats = true;
    f->n_humans = g->n_humans;
    f->n_bots = g->n_bots;
    f->humans = (const cb_uuid *)c->humans;
    f->bots = (const cb_uuid *)c->bots;
}

// THE FLUSH RULE: a game whose oldest waiting move is game_age_ms old, or whose waiting moves
// hold game_bytes (game_full), is due; with `all` (every waiting move
// together over total_bytes, a settle, the close) every game with a move waiting is.
static bool flush_due(const cb_journal *j, const jgame *g, bool all, int64_t now) {
    return g->n_moves > 0 && (all || now - g->first_ms >= j->o.game_age_ms || game_full(j, g));
}

// g's waiting moves as the tables' write takes them: an array of n moves whose logs point into
// `logs` (g's own buffer when the caller holds g still, else a copy). One allocation: the moves,
// then (copy) the log bytes after them.
static cb_game_move *moves_for_flush(const jgame *g, bool copy) {
    cb_game_move *m = malloc((size_t)g->n_moves * sizeof *m + (copy ? g->logs_len : 0) + 1);
    if (!m) return NULL;
    const uint8_t *logs = g->logs;
    if (copy) {
        uint8_t *b = (uint8_t *)(m + g->n_moves);
        if (g->logs_len) memcpy(b, g->logs, g->logs_len);
        logs = b;
    }
    for (int i = 0; i < g->n_moves; i++)
        m[i] = (cb_game_move){ g->moves[i].seq, logs + g->moves[i].off, g->moves[i].len, g->moves[i].clear, NULL };
    // A rated end's changes ride its commit (g's own: freed only by a flush, under the flush lock).
    for (int k = 0, i = 0; k < g->n_rated; k++) {
        while (i < g->n_moves && g->moves[i].seq < g->rated[k].seq) i++;
        if (i < g->n_moves && g->moves[i].seq == g->rated[k].seq) m[i].rated = g->rated[k].r;
    }
    return m;
}

static int first_ms_cmp(const void *a, const void *b) {
    const int64_t x = (*(jgame *const *)a)->first_ms, y = (*(jgame *const *)b)->first_ms;
    return x < y ? -1 : x > y;
}

// One transaction's worth of the games due, copied (ov_mu held): rows, the move lists and their log bytes
// (a move after the snapshot may grow, and so move, the game's log buffer while the flush writes). THE
// TRANSACTION BUDGET (its one owner): the oldest waiting games first, while their waiting moves
// together stay within opts.txn_moves (one game bigger than that goes alone: its moves and its
// row are one write). Only games that waited since before the pass began (first_ms <= pass_ms)
// are taken, so a pass ends under a steady stream of moves. *more: due games were left for the
// next transaction.
static int snapshot_take(cb_journal *j, snapshot *sn, bool all, int64_t now, int64_t pass_ms, bool *more) {
    memset(sn, 0, sizeof *sn);
    *more = false;
    int n = 0;
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next) n += flush_due(j, g, all, now) && g->first_ms <= pass_ms;
    if (!n) return CB_OK;
    jgame **due = malloc((size_t)n * sizeof *due);
    if (!due) return CB_E_NOMEM;
    int k = 0;
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next)
            if (flush_due(j, g, all, now) && g->first_ms <= pass_ms) due[k++] = g;
    qsort(due, (size_t)n, sizeof *due, first_ms_cmp);
    long budget = 0;
    for (k = 0; k < n && (k == 0 || budget + due[k]->n_moves <= j->o.txn_moves); k++) budget += due[k]->n_moves;
    *more = k < n;
    n = k;
    sn->f = calloc((size_t)n, sizeof *sn->f);
    sn->rows = calloc((size_t)n, sizeof *sn->rows);
    sn->seats = calloc((size_t)n, sizeof *sn->seats);
    if (!sn->f || !sn->rows || !sn->seats) { free(due); snapshot_free(sn); return CB_E_NOMEM; }
    for (int i = 0; i < n; i++) {
        jgame *g = due[i];
        cb_game_move *m = moves_for_flush(g, true);
        // R4: the rows the flush writes are the residents' encodes, made now.
        if (!m || row_refresh(g) != CB_OK || row_dup(&sn->rows[sn->n], &g->row) != CB_OK) {
            free(m);
            free(due);
            snapshot_free(sn);
            return CB_E_NOMEM;
        }
        sn->f[sn->n] = (cb_game_flush){ &sn->rows[sn->n], g->sql_version, m, g->n_moves, false, 0, 0, NULL, NULL };
        flush_seats(g, &sn->f[sn->n], &sn->seats[sn->n]);
        sn->n++;
    }
    free(due);
    return CB_OK;
}

// The tables now hold f: its moves leave the game (ov_mu held). A move that came after the
// snapshot is in snap_seg or later.
static void ov_flushed(cb_journal *j, jgame *g, const cb_game_flush *f, uint64_t snap_seg, int64_t now) {
    int k = 0;
    long b = 0;
    for (; k < g->n_moves && g->moves[k].seq <= f->row->version; k++) b += move_bytes(&g->moves[k]);
    // The flushed deltas are the buffer's prefix: the rest moves down to its start. An emptied
    // buffer is freed (a game between flushes holds no log memory).
    const size_t cut = k < g->n_moves ? g->moves[k].off : g->logs_len;
    if (k) memmove(g->moves, g->moves + k, (size_t)(g->n_moves - k) * sizeof *g->moves);
    g->n_moves -= k;
    for (int i = 0; i < g->n_moves; i++) g->moves[i].off -= (uint32_t)cut;
    if (!g->n_moves) {   // and the move array goes with it: a game between flushes holds neither
        free(g->logs);
        g->logs = NULL;
        g->logs_len = g->logs_cap = 0;
        free(g->moves);
        g->moves = NULL;
        g->cap_moves = 0;
    } else if (cut) {
        memmove(g->logs, g->logs + cut, g->logs_len - cut);
        g->logs_len -= cut;
    }
    // The rated ends among them: in the tables now, and their entries free to go.
    int r = 0;
    for (; r < g->n_rated && g->rated[r].seq <= f->row->version; r++) {
        ratings_flushed(j, g->rated[r].r);
        free(g->rated[r].r);
    }
    if (r) memmove(g->rated, g->rated + r, (size_t)(g->n_rated - r) * sizeof *g->rated);   // (rated is NULL until one)
    g->n_rated -= r;
    g->bytes -= b;
    j->w_bytes -= b;
    j->n_waiting -= k;
    g->sql_version = f->row->version;
    if (g->seats_dirty && g->seats_seq <= f->row->version) g->seats_dirty = false;   // in the tables now
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

static int touched_cmp(const void *a, const void *b) {
    const int64_t x = (*(jgame *const *)a)->touched_ms, y = (*(jgame *const *)b)->touched_ms;
    return x < y ? -1 : x > y;
}

// THE RESIDENCY RULE (ov_mu and flush_mu held; a jgame is freed only under both, so a holder of
// flush_mu may keep a pointer to one across ov_mu): a game idle IDLE_EVICT_MS with nothing
// waiting leaves; then, while more than max_games are resident, the least recently touched
// with nothing waiting do. A game with waiting moves never leaves (only a flush takes them).
static void ov_evict(cb_journal *j, int64_t now) {
    ratings_evict(j, now, false);
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame **p = &j->buckets[b]; *p;) {
            if (!(*p)->n_moves && now - (*p)->touched_ms > IDLE_EVICT_MS) ov_unlink(j, p);
            else p = &(*p)->next;
        }
    if (j->n_games <= j->o.max_games) return;
    jgame **idle = malloc((size_t)j->n_games * sizeof *idle);
    if (!idle) return;   // the next pass tries again
    int n = 0;
    for (int b = 0; b < OV_BUCKETS; b++)
        for (jgame *g = j->buckets[b]; g; g = g->next)
            if (!g->n_moves) idle[n++] = g;
    qsort(idle, (size_t)n, sizeof *idle, touched_cmp);
    for (int i = 0; i < n && j->n_games > j->o.max_games; i++) ov_unlink(j, ov_slot(j, idle[i]->row.id));
    free(idle);
}

// ONE TRANSACTION of a flush pass (flush_mu held): the next games due within the budget
// (snapshot_take), written, then the residency rule and the segments trimmed. The writer is held
// for the snapshot (a move after it is in a later segment) unless the caller holds it already.
static int flush_txn(cb_journal *j, bool all, int64_t pass_ms, bool writer_held, bool *more) {
    if (!writer_held) writer_hold(j);
    snapshot sn;
    const int64_t now = cb_mono_ms();
    pthread_mutex_lock(&j->ov_mu);
    int rc = snapshot_take(j, &sn, all, now, pass_ms, more);
    const uint64_t snap_seg = __atomic_load_n(&j->seg_seq, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&j->ov_mu);
    if (!writer_held) writer_release(j);
    bool rated = false;   // (the snapshot's ratings are the games', alive until ov_flushed below)
    for (int i = 0; i < sn.n; i++)
        for (int k = 0; k < sn.f[i].n_moves; k++) rated |= sn.f[i].moves[k].rated != NULL;
    if (rc == CB_OK && sn.n) rc = cb_rows_sql_flush(j->s, sn.f, sn.n, NULL, NULL, 0);
    // The leaderboard moved in the tables just now: the edge's copy goes (edge.h). A commit written
    // straight to the tables marks it itself (table_ops.c); a journaled rated end marks it here.
    if (rc == CB_OK && rated) cb_edge_stale(CB_EDGE_BOARD);
    if (rc == CB_OK) {
        long moves = 0;
        pthread_mutex_lock(&j->ov_mu);
        for (int i = 0; i < sn.n; i++) {
            jgame *g = *ov_slot(j, sn.rows[i].id);
            if (g) ov_flushed(j, g, &sn.f[i], snap_seg, now);
            moves += sn.f[i].n_moves;
        }
        ov_evict(j, now);
        pthread_mutex_unlock(&j->ov_mu);
        trim(j);
        if (sn.n) {
            atomic_fetch_add(&j->flushes, 1);
            atomic_fetch_add(&j->flushed_games, sn.n);
            atomic_fetch_add(&j->flushed_moves, moves);
            long top = atomic_load(&j->txn_moves_max);
            while (moves > top && !atomic_compare_exchange_weak(&j->txn_moves_max, &top, moves)) {}
        }
    } else {
        *more = false;
        CB_ERROR(MOD, "flush failed (%s): the moves stay in the journal and the next flush retries", cb_strerror(rc));
    }
    snapshot_free(&sn);
    return rc;
}

// A FLUSH PASS: the games due when it began, as bounded transactions (flush_txn), with the flush
// lock given back between them so a write around the journal (a delete, a rated end) or a log
// read waits at most one transaction, never a whole pass. With `all` (a settle, the close, or
// every waiting move together over total_bytes when the pass began) every game waiting then.
static int flush_pass(cb_journal *j, bool all) {
    const int64_t pass_ms = cb_mono_ms();
    pthread_mutex_lock(&j->ov_mu);
    if (j->w_bytes >= j->o.total_bytes) all = true;
    pthread_mutex_unlock(&j->ov_mu);
    int rc;
    bool more;
    do {
        pthread_mutex_lock(&j->flush_mu);
        const int64_t t0 = cb_mono_us();
        rc = flush_txn(j, all, pass_ms, false, &more);
        const long held = (long)(cb_mono_us() - t0);
        pthread_mutex_unlock(&j->flush_mu);
        long top = atomic_load(&j->txn_hold_us_max);
        while (held > top && !atomic_compare_exchange_weak(&j->txn_hold_us_max, &top, held)) {}
    } while (rc == CB_OK && more);
    return rc;
}

// Every waiting move, with the flush lock and the writer already held (an exclusive section:
// nothing is acknowledged meanwhile, so the transactions end).
static int flush_held(cb_journal *j) {
    const int64_t pass_ms = cb_mono_ms();
    int rc;
    bool more;
    do rc = flush_txn(j, true, pass_ms, true, &more);
    while (rc == CB_OK && more);
    return rc;
}

static int flush_all(cb_journal *j) { return flush_pass(j, true); }

int cb_journal_flush(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    return j ? flush_all(j) : CB_OK;
}

int cb_journal_flush_due(cb_store *s) {
    cb_journal *j = cb_journal_of(s);
    return j ? flush_pass(j, false) : CB_OK;
}

// When the oldest waiting game is due by its age (ov_mu held): cb_mono_ms, or INT64_MAX.
static int64_t next_due_ms(const cb_journal *j) {
    int64_t at = INT64_MAX;
    for (int b = 0; b < OV_BUCKETS; b++)
        for (const jgame *g = j->buckets[b]; g; g = g->next)
            if (g->n_moves && g->first_ms + j->o.game_age_ms < at) at = g->first_ms + j->o.game_age_ms;
    return at;
}

static void *flusher(void *ud) {
    cb_journal *j = ud;
    pthread_mutex_lock(&j->mu);
    while (!j->stop) {
        // The alarm: the oldest waiting game's age, or none.
        pthread_mutex_unlock(&j->mu);
        pthread_mutex_lock(&j->ov_mu);
        const int64_t due_at = j->armed_ms = next_due_ms(j);
        pthread_mutex_unlock(&j->ov_mu);
        pthread_mutex_lock(&j->mu);
        int w = 0;
        if (due_at == INT64_MAX) {
            while (!j->stop && !j->kick && !j->prepare && !j->rearm) pthread_cond_wait(&j->fcv, &j->mu);
        } else {
            struct timespec ts;
            int64_t in = due_at - cb_mono_ms();
            host_deadline_after_ms(&ts, in > 0 ? in : 0);
            while (!j->stop && !j->kick && !j->prepare && !j->rearm && (w = pthread_cond_timedwait(&j->fcv, &j->mu, &ts)) != ETIMEDOUT) {}
        }
        if (j->stop) break;
        atomic_fetch_add(&j->flusher_wakes, 1);
        const bool flush = j->kick || w == ETIMEDOUT;
        j->kick = j->prepare = j->rearm = false;
        pthread_mutex_unlock(&j->mu);
        prepare_next(j);   // the next segment, made ahead of the rotation that takes it
        pthread_mutex_lock(&j->mu);
        const bool tell = j->head && !j->writing && loop_writes(j);   // records the loop left for want of it
        pthread_mutex_unlock(&j->mu);
        if (tell) loop_tell(j);
        if (flush) (void)flush_pass(j, false);
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
    int rc = !g ? CB_E_NOT_FOUND : row_refresh(g);
    if (rc == CB_OK) rc = row_dup(out, &g->row);
    pthread_mutex_unlock(&j->ov_mu);
    if (rc == CB_OK) out->seats_exact = true;   // a journaled move never changes the seats
    return rc;
}

static int j_game_get(cb_store *s, const char *id, cb_game_row *out) {
    memset(out, 0, sizeof *out);
    cb_journal *j = cb_journal_of(s);
    int rc = ov_get(j, id, out);
    if (rc == CB_E_NOT_FOUND && on_loop(j)) return CB_E_WOULD_BLOCK;   // the tables' read waits on the disk
    return rc == CB_E_NOT_FOUND ? cb_rows_sql.game_get(s, id, out) : rc;
}

int cb_journal_game_acquire(cb_store *s, const char *id, cb_journal_game_fn fn, void *ud) {
    cb_journal *j = cb_journal_of(s);
    if (!j) return CB_E_STATE;
    for (;;) {
        pthread_mutex_lock(&j->ov_mu);
        jgame *g = *ov_slot(j, id);
        if (g) {
            g->touched_ms = cb_mono_ms();
            int rc = g->res_valid ? fn(ud, &g->row, &g->res) : fn(ud, &g->row, NULL);
            pthread_mutex_unlock(&j->ov_mu);
            return rc;
        }
        pthread_mutex_unlock(&j->ov_mu);
        if (on_loop(j)) return CB_E_WOULD_BLOCK;   // the tables' read waits on the disk
        // Not resident: the tables' row, made resident unless an admin's write (THE ADMIN
        // RULE) came between the read and the insert, which reads again.
        const long epoch = atomic_load(&j->epoch);
        cb_game_row row;
        memset(&row, 0, sizeof row);
        int rc = cb_rows_sql.game_get(s, id, &row);
        if (rc != CB_OK) return rc;
        atomic_fetch_add(&j->table_reads, 1);
        pthread_mutex_lock(&j->ov_mu);
        if (atomic_load(&j->epoch) != epoch) {
            pthread_mutex_unlock(&j->ov_mu);
            cb_game_row_free(&row);
            continue;
        }
        jgame **p = ov_slot(j, id);
        g = *p ? *p : ov_insert(j, p, &row, NULL);
        const bool over = j->n_games > j->o.max_games;
        if (g && g->res_valid) rc = fn(ud, &g->row, &g->res);
        else if (g && row_refresh(g) == CB_OK) rc = fn(ud, &g->row, NULL);
        else rc = fn(ud, &row, NULL);   // out of memory: served from the read, not resident
        pthread_mutex_unlock(&j->ov_mu);
        cb_game_row_free(&row);
        if (over) kick_flusher(j);
        return rc;
    }
}

int cb_journal_game_peek(cb_store *s, const char *id, cb_journal_game_fn fn, void *ud) {
    cb_journal *j = cb_journal_of(s);
    if (!j) return CB_E_NOT_FOUND;
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, id);
    int rc = CB_E_NOT_FOUND;
    if (g && g->res_valid) rc = fn(ud, &g->row, &g->res);
    else if (g && (rc = row_refresh(g)) == CB_OK) rc = fn(ud, &g->row, NULL);
    pthread_mutex_unlock(&j->ov_mu);
    return rc;
}

void cb_journal_game_created(cb_store *s, const cb_game_row *row, const TableResident *res) {
    cb_journal *j = cb_journal_of(s);
    if (!j) return;
    pthread_mutex_lock(&j->ov_mu);
    jgame **p = ov_slot(j, row->id);
    if (!*p) (void)ov_insert(j, p, row, res);   // out of memory: the first operation reads the tables
    const bool over = j->n_games > j->o.max_games;
    pthread_mutex_unlock(&j->ov_mu);
    if (over) kick_flusher(j);
}

void cb_journal_game_decoded(cb_store *s, const char *id, int64_t version, const TableResident *res) {
    cb_journal *j = cb_journal_of(s);
    if (!j) return;
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, id);
    if (g && !g->res_valid && g->row.version == version) {   // the row it was decoded from is still the game's
        g->res = *res;
        g->res_valid = true;   // row_fresh stays: the bytes it was decoded from are its encode
    }
    pthread_mutex_unlock(&j->ov_mu);
}

// The tables' log (every move up to sql_version), then the overlay's: the flush lock keeps a
// flush from moving moves between the two while they are read.
static int logs_of(cb_journal *j, const char *id, int64_t log_from, uint8_t **out, size_t *out_len) {
    cb_store *s = j->s;
    pthread_mutex_lock(&j->flush_mu);
    // The tables hold nothing of a session the overlay began after their version (a deal not
    // flushed yet, and every move since): the overlay alone answers, with no read.
    pthread_mutex_lock(&j->ov_mu);
    const jgame *held = *ov_slot(j, id);
    const bool overlay_only = held && log_from > held->sql_version;
    pthread_mutex_unlock(&j->ov_mu);
    int rc = CB_OK;
    if (overlay_only) *out = NULL, *out_len = 0;
    else rc = cb_rows_sql.game_logs(s, id, log_from, out, out_len), atomic_fetch_add(&j->log_reads, 1);
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
                    memcpy(p + *out_len, g->logs + g->moves[i].off, g->moves[i].len);
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

// A write of one game around the journal: its waiting moves first, in the same transaction.
// The game STAYS RESIDENT at the row the commit wrote (the overlay is its one owner: the next
// operation reads it here, not from the tables); a delete takes it out. The caller holds the
// game's lock, so no move of it is in flight.
static int around(cb_store *s, const char *id, const cb_game_commit *c, const char *del_id, int64_t del_version) {
    cb_journal *j = cb_journal_of(s);
    if (on_loop(j)) return CB_E_WOULD_BLOCK;   // a write of the tables, behind the flush's lock
    pthread_mutex_lock(&j->flush_mu);
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, id);
    int fr = g ? row_refresh(g) : CB_OK;   // the waiting moves' row, written below (R4)
    // Its waiting moves, their logs in place: no move of this game comes while the caller
    // holds its lock, so its log buffer stays where it is.
    cb_game_move *moves = fr == CB_OK && g && g->n_moves ? moves_for_flush(g, false) : NULL;
    if (fr == CB_OK && g && g->n_moves && !moves) fr = CB_E_NOMEM;
    pthread_mutex_unlock(&j->ov_mu);
    if (fr != CB_OK) {
        pthread_mutex_unlock(&j->flush_mu);
        return fr;
    }
    // g is stable here: an entry is freed only under the flush lock (held), and only a move
    // of this game (its lock is the caller's) changes it.
    cb_game_flush f = { 0 };
    seats_copy sc;
    if (g) {
        f = (cb_game_flush){ &g->row, g->sql_version, moves, g->n_moves, false, 0, 0, NULL, NULL };
        flush_seats(g, &f, &sc);
    }
    const bool rated = g && g->n_rated > 0;   // its waiting rated ends go with it
    int rc = cb_rows_sql_flush(s, &f, g && g->n_moves ? 1 : 0, c, del_id, del_version);
    if (rc == CB_OK && rated) cb_edge_stale(CB_EDGE_BOARD);
    pthread_mutex_lock(&j->ov_mu);
    bool over = false;
    if (rc == CB_OK) {
        if (g && g->n_moves) {   // in the tables now
            atomic_fetch_add(&j->flushed_moves, g->n_moves);
            ov_flushed(j, g, &f, 0, 0);
        }
        jgame **p = ov_slot(j, id);
        if (del_id) {
            if (*p) ov_unlink(j, p);
        } else if (c && c->row) {
            if (!*p) g = ov_insert(j, p, c->row, c->resident);
            else if (row_assign(g, c->row) != CB_OK) { ov_unlink(j, p); g = NULL; }   // read again from the tables
            else {
                // The commit's bytes are written; its resident is the same game decoded.
                g->row_fresh = true;
                g->res_valid = c->resident != NULL;
                if (c->resident) g->res = *c->resident;
                g->sql_version = c->row->version;
                g->touched_ms = cb_mono_ms();
                // The commit's seats are the stored ones now.
                g->n_humans = g->n_bots = 0;
                for (int i = 0; i < c->n_humans && i < CB_MAX_SEATS; i++) memcpy(g->humans[g->n_humans++], c->humans[i], sizeof(cb_uuid));
                for (int i = 0; i < c->n_bots && i < CB_MAX_SEATS; i++) memcpy(g->bots[g->n_bots++], c->bots[i], sizeof(cb_uuid));
            }
            over = j->n_games > j->o.max_games;
        }
    } else if (g && !g->n_moves && (rc == CB_E_CONFLICT || rc == CB_E_NOT_FOUND)) {
        // The tables hold another row than the one the overlay held: it goes, and the next
        // operation reads the tables' (a conflict is never built on).
        ov_unlink(j, ov_slot(j, id));
    }
    pthread_mutex_unlock(&j->ov_mu);
    pthread_mutex_unlock(&j->flush_mu);
    free(moves);
    if (over) kick_flusher(j);
    return rc;
}

static int j_game_commit(cb_store *s, const cb_game_commit *c) {
    cb_journal *j = cb_journal_of(s);
    if (!c || !c->row) return CB_E_INVALID;
    const cb_game_row *row = c->row;
    const bool plain = c->moves && c->n_moves >= 1 && c->n_moves <= BOT_DRIVE_MAX_ACTIONS;
    // EVERY COMMIT THROUGH THE JOURNAL: a plain move as its moves, any other (a deal, a lobby
    // edit, an end, a rated one too) whole (a full record). A rated end's changes ride its record:
    // THE RATINGS' ONE OWNER (jrating) takes them at once, and the game is due at once, so the
    // tables' readers (the leaderboard, a profile) see them one flush later.
    if (!plain && (!row->state || !row->roster)) return around(s, row->id, c, NULL, 0);
    // The fence the tables would apply: the game's acknowledged version is the expected one,
    // read in this epoch (an exclusive section after it refuses the record: write_batch).
    const long epoch = atomic_load(&j->epoch);
    pthread_mutex_lock(&j->ov_mu);
    jgame *g = *ov_slot(j, row->id);
    int64_t held = g ? g->row.version : -1;
    pthread_mutex_unlock(&j->ov_mu);
    if (held < 0) {
        if (on_loop(j)) return CB_E_WOULD_BLOCK;   // not resident: the fence is the tables' to read
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
    cb_strlcpy(r.m.game, row->id, sizeof r.m.game);
    r.m.base_version = c->expected_version;
    r.m.now_ms = row->updated_at;
    if (plain) {   // the moves borrowed: this thread waits for the record's write
        r.m.bots = c->moves_by_bots;
        r.m.n_mv = c->n_moves;
        r.m.mv = c->moves;
    } else {
        r.full = true;
        r.clear = c->log_clear;
    }
    cb_game_rated rated;
    if (!plain && c->n_rated > 0) {
        memset(&rated, 0, sizeof rated);
        rated.n = c->n_rated < CB_MAX_SEATS ? c->n_rated : CB_MAX_SEATS;
        memcpy(rated.ids, c->rated_ids, sizeof rated.ids);
        memcpy(rated.is_bot, c->rated_is_bot, sizeof rated.is_bot);
        memcpy(rated.delta, c->rating_delta, sizeof rated.delta);
        memcpy(rated.names, c->rated_names, sizeof rated.names);
        rated.at = row->updated_at;
        r.rated = &rated;
    }
    r.row = *row;   // read by ov_apply while this thread waits in append
    r.res = c->resident;
    r.log = c->log_add_len ? c->log_add : NULL;   // copied by ov_apply while this thread waits
    r.log_len = c->log_add_len;
    return on_loop(j) ? loop_append(j, &r) : append(j, &r);
}

static int j_game_delete(cb_store *s, const char *id, int64_t expected) { return around(s, id, NULL, id, expected); }

// Membership, the overlay's for a game it holds (a whole commit seats and unseats before the
// tables hear of it), else the tables'.
static int j_is_member(cb_store *s, const char *game_id, const char *user_id) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->ov_mu);
    const jgame *g = *ov_slot(j, game_id);
    int rc = -1;
    if (g) {
        rc = 0;
        for (int i = 0; i < g->n_humans && !rc; i++) rc = strcmp(g->humans[i], user_id) == 0;
    }
    pthread_mutex_unlock(&j->ov_mu);
    return rc >= 0 ? rc : cb_rows_sql.is_member(s, game_id, user_id);
}

static int j_game_members(cb_store *s, const char *game_id, cb_name_fn fn, void *ud) {
    cb_journal *j = cb_journal_of(s);
    cb_uuid humans[CB_MAX_SEATS];
    int n = -1;
    pthread_mutex_lock(&j->ov_mu);
    const jgame *g = *ov_slot(j, game_id);
    if (g) memcpy(humans, g->humans, sizeof humans), n = g->n_humans;
    pthread_mutex_unlock(&j->ov_mu);
    if (n < 0) return cb_rows_sql.game_members(s, game_id, fn, ud);
    int out = 0;
    for (int i = 0; i < n; i++) {
        out++;
        if (!fn(humans[i], ud)) break;
    }
    return out;
}

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
    cb_journal *j = cb_journal_of(s);
    int rc = journal_scan(s);
    if (rc == CB_OK) rc = cb_rows_sql.game_import(s, g, humans, n_humans, bots, n_bots);
    // The tables hold the imported row now: a resident copy of the game is not its owner any more.
    pthread_mutex_lock(&j->flush_mu);
    pthread_mutex_lock(&j->ov_mu);
    jgame **p = ov_slot(j, g->id);
    if (*p && !(*p)->n_moves) ov_unlink(j, p);
    pthread_mutex_unlock(&j->ov_mu);
    pthread_mutex_unlock(&j->flush_mu);
    return rc;
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

// ---- the seats' ratings (THE RATINGS' ONE OWNER) ------------------------------------------------

int cb_journal_seat_ratings(cb_store *s, const char *const *ids, int n, int64_t now_ms, int64_t min_age_ms,
                            int32_t *ratings, bool *old_enough) {
    cb_journal *j = cb_journal_of(s);
    if (!j || n < 0 || n > CB_MAX_SEATS) return CB_E_INVALID;
    const int64_t now = cb_mono_ms();
    bool miss[CB_MAX_SEATS] = { false }, any = false;
    pthread_mutex_lock(&j->ov_mu);
    for (int i = 0; i < n; i++) {
        jrating *e = *rating_slot(j, ids[i]);
        if (e) e->touched_ms = now;
        else miss[i] = any = true;
    }
    pthread_mutex_unlock(&j->ov_mu);
    if (any) {
        if (on_loop(j)) return CB_E_WOULD_BLOCK;   // the tables' to read
        // The misses from the tables (outside every lock), then held unless an entry came meanwhile:
        // that one is newer (it may carry a rated end the tables do not have yet).
        const char *mids[CB_MAX_SEATS];
        int32_t mr[CB_MAX_SEATS];
        int64_t mc[CB_MAX_SEATS];
        int m = 0;
        for (int i = 0; i < n; i++) if (miss[i]) mids[m++] = ids[i];
        int rc = cb_rows_of(s)->ratings_for(s, mids, m, mr);
        for (int i = 0; rc == CB_OK && i < m; i++) {
            cb_user_row u;
            const int ur = CB_STORE_CALL(s, user_get, mids[i], &u);
            if (ur == CB_E_NOT_FOUND) mc[i] = 0;   // a bot, or an account deleted: no age to keep
            else if (ur != CB_OK) rc = ur;
            else mc[i] = u.created_at > 0 ? u.created_at : 1;
        }
        if (rc != CB_OK) return rc;
        pthread_mutex_lock(&j->ov_mu);
        for (int i = 0; i < m; i++) {
            jrating **p = rating_slot(j, mids[i]);
            if (*p) continue;
            jrating *e = calloc(1, sizeof *e);
            if (!e) { rc = CB_E_NOMEM; break; }
            cb_strlcpy(e->id, mids[i], sizeof e->id);
            e->rating = mr[i];
            e->created_ms = mc[i];
            e->touched_ms = now;
            *p = e;
            j->n_ratings++;
        }
        pthread_mutex_unlock(&j->ov_mu);
        if (rc != CB_OK) return rc;
    }
    pthread_mutex_lock(&j->ov_mu);
    bool old = true;
    int rc = CB_OK;
    for (int i = 0; i < n; i++) {
        const jrating *e = *rating_slot(j, ids[i]);
        if (!e) { rc = CB_E_WOULD_BLOCK; break; }   // went between the read and here: unreachable within IDLE_EVICT_MS
        ratings[i] = e->rating;
        if (e->created_ms && min_age_ms > 0 && now_ms - e->created_ms < min_age_ms) old = false;
    }
    pthread_mutex_unlock(&j->ov_mu);
    *old_enough = old;
    return rc;
}

// An account's delete names its rating row with NULL (functions_http.c cb_app_delete_account):
// its entry no longer has an account behind it, as a read of the tables would find.
static int j_rating_set_username(cb_store *s, const char *user_id, const char *username) {
    cb_journal *j = cb_journal_of(s);
    const int rc = cb_rows_sql.rating_set_username(s, user_id, username);
    if (!username) {
        pthread_mutex_lock(&j->ov_mu);
        jrating *e = *rating_slot(j, user_id);
        if (e) e->created_ms = 0;
        pthread_mutex_unlock(&j->ov_mu);
    }
    return rc;
}

// ---- an admin's write, and the backup ------------------------------------------------------

// THE ADMIN RULE (store.h cb_store_admin_query): every waiting move into the tables, no
// move acknowledged while fn runs, and then the overlay forgotten and the epoch bumped, so
// no row the journal held before fn (nor a record whose base was read before it) is ever
// written over what fn wrote.
static int j_exclusive(cb_store *s, int (*fn)(void *ud), void *ud) {
    cb_journal *j = cb_journal_of(s);
    pthread_mutex_lock(&j->flush_mu);
    writer_hold(j);
    int rc = flush_held(j);
    if (rc == CB_OK) {
        rc = fn(ud);
        pthread_mutex_lock(&j->ov_mu);
        for (int b = 0; b < OV_BUCKETS; b++)
            while (j->buckets[b]) ov_unlink(j, &j->buckets[b]);
        ratings_evict(j, 0, true);   // fn may have written ratings or accounts: read them again
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
    uint8_t *slog = NULL, *mlog = NULL;
    size_t slog_len = 0;
    if (m->bots) rc = logs_of(j, m->game, cur.log_from, &slog, &slog_len);   // a bot's moves are seeded from it
    if (rc == CB_OK) rc = j->o.apply(j->o.apply_ud, &cur, slog, slog_len, m, &r.row, &mlog, &r.log_len);
    r.log = mlog;
    free(slog);
    cb_game_row_free(&cur);
    if (rc == CB_OK && r.row.version != m->base_version + 1) rc = CB_E_STATE;
    if (rc != CB_OK) {
        snprintf(err, cap, "journal: game %s's move from version %lld does not replay (%s)", m->game,
                 (long long)m->base_version, cb_strerror(rc));
        cb_game_row_free(&r.row);
        free(mlog);
        return rc;
    }
    pthread_mutex_lock(&j->ov_mu);
    rc = ov_apply(j, &r);
    pthread_mutex_unlock(&j->ov_mu);
    cb_game_row_free(&r.row);
    free(mlog);
    if (rc == CB_OK) atomic_fetch_add(&j->recovered, 1);
    return rc;
}

// One recovered whole commit onto its game: the row it wrote becomes the game's (no kernel
// run: the record is the commit's result), from the base it was written on. A commit the
// tables already hold is skipped.
static int recover_full(cb_journal *j, jrec *r, char *err, size_t cap) {
    cb_game_row cur;
    int rc = ov_get(j, r->m.game, &cur);
    if (rc == CB_E_NOT_FOUND) rc = cb_rows_sql.game_get(j->s, r->m.game, &cur);
    if (rc == CB_E_NOT_FOUND) return CB_OK;   // deleted since: its commits went with it
    if (rc != CB_OK) {
        snprintf(err, cap, "journal: game %s does not read (%s)", r->m.game, cb_strerror(rc));
        return rc;
    }
    const int64_t at = cur.version;
    cb_game_row_free(&cur);
    if (at > r->m.base_version) return CB_OK;
    if (at != r->m.base_version) {
        if (j->restored) {
            CB_WARN(MOD, "game %s is at version %lld in the restored copy and its next record is from %lld: skipped",
                    r->m.game, (long long)at, (long long)r->m.base_version);
            return CB_OK;
        }
        snprintf(err, cap, "journal: game %s is at version %lld and the journal's next commit is from %lld: a commit is missing",
                 r->m.game, (long long)at, (long long)r->m.base_version);
        return CB_E_STATE;
    }
    // The record's bytes are the segment buffer's: ov_apply copies its log.
    pthread_mutex_lock(&j->ov_mu);
    rc = ov_apply(j, r);
    pthread_mutex_unlock(&j->ov_mu);
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
    if (len >= SEG_HDR && memcmp(buf, SEG_MAGIC, 4) == 0 && get_u32(buf + 4) != FORMAT) {
        // Another format's moves (an older server's): never skipped, they were acknowledged.
        // The older server flushes every move and deletes its segments when it stops cleanly.
        snprintf(err, cap, "journal: %s is format %u, this server reads %d: stop the server that wrote it cleanly (its close flushes) and start again",
                 path, (unsigned)get_u32(buf + 4), FORMAT);
        free(buf);
        return CB_E_STATE;
    }
    if (len < SEG_HDR || memcmp(buf, SEG_MAGIC, 4) != 0) {
        // A segment cut before its header was whole was never written to.
        CB_WARN(MOD, "%s has no whole header (%zu bytes): skipped", path, len);
        free(buf);
        return CB_OK;
    }
    const int64_t start = (int64_t)get_u64(buf + 8);
    cb_jslots *t = cb_jslots_new();
    cb_jmove *ms = malloc(sizeof *ms * CB_JOURNAL_BATCH_MAX);
    EvwMove (*mv)[EVW_DELTA_MAX_MOVES] = malloc(sizeof *mv * CB_JOURNAL_BATCH_MAX);
    if (!t || !ms || !mv) rc = CB_E_NOMEM;
    size_t at = SEG_HDR;
    while (rc == CB_OK && at < len && buf[at] != 0) {   // a zero magic: the mapped segment's unwritten rest
        if (buf[at] == FULL_MAGIC) {
            jrec r;
            cb_game_rated rated;
            const long used = full_decode(buf + at, len - at, &r, &rated);
            if (used < 0) {
                CB_WARN(MOD, "%s: %zu bytes at %zu are not a whole record: dropped", path, len - at, at);
                break;
            }
            rc = recover_full(j, &r, err, cap);
            at += (size_t)used;
            continue;
        }
        size_t used = 0;
        int n = cb_journal_batch_decode(t, start, buf + at, len - at, &used, ms, mv, CB_JOURNAL_BATCH_MAX);
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
    free(mv);
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
    ratings_evict(j, 0, true);
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
    if (j->o.total_bytes <= 0) j->o.total_bytes = 16 << 20;
    if (j->o.txn_moves <= 0) j->o.txn_moves = 2000;
    if (j->o.seg_bytes <= 0) j->o.seg_bytes = 8 << 20;
    if (j->o.max_games <= 0) j->o.max_games = OV_MAX_GAMES;
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
        J_OPS.is_member = j_is_member;
        J_OPS.game_members = j_game_members;
        J_OPS.rating_set_username = j_rating_set_username;
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
        // Everything recovered goes into the tables before anything is served (in bounded
        // transactions, as any flush: nothing else runs yet, so no lock is wanted), and the
        // segments with it.
        rc = flush_held(j);
        if (rc != CB_OK) snprintf(err, cap, "journal: the recovered moves do not write (%s)", cb_strerror(rc));
    }
    if (rc == CB_OK) {
        for (int b = 0; b < OV_BUCKETS; b++)
            while (j->buckets[b]) ov_unlink(j, &j->buckets[b]);
        ratings_evict(j, 0, true);   // (none is held during recovery; the tables have them all now)
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
    out->games = j->n_games;
    pthread_mutex_unlock(&j->ov_mu);
    pthread_mutex_lock(&j->seg_mu);
    out->segments = j->n_segs;
    pthread_mutex_unlock(&j->seg_mu);
    out->recovered = j->recovered;
    out->table_reads = j->table_reads;
    out->log_reads = j->log_reads;
    out->loop_writes = j->loop_writes;
    out->loop_seg_opens = j->loop_seg_opens;
    out->txn_moves_max = j->txn_moves_max;
    out->txn_hold_us_max = j->txn_hold_us_max;
    out->flusher_wakes = j->flusher_wakes;
}

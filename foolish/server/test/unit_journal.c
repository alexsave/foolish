// unit_journal.c - the move journal (store/journal.h): the batch codec, a torn or corrupt tail,
// recovery after a SIGKILL, the flush's rows against per-move commits, and a backup mid-game.
#include "t.h"
#include "app/app_internal.h"
#include "app/foolish_app.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "cbase/provider.h"
#include "handler.h"
#include "net/router.h"
#include "rate_limits.h"
#include "store/backup.h"
#include "store/journal.h"
#include "store/store_sql.h"
#include "awire.h"
#include "legal.h"
#include "roster.h"
#include "table.h"
#include "util/err.h"
#include "../../../shared/c/fnv.h"
#include "../../../shared/c/rng.h"

#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char UID_A[] = "aaaaaaaa-0000-4000-8000-000000000001";
static const char UID_B[] = "bbbbbbbb-0000-4000-8000-000000000002";

static char g_dir[256];
static cb_config g_cfg;
static cb_ctx g_ctx;
static int64_t g_now = 1760000000000LL;
static int64_t fake_now(void) { return g_now; }
static int no_job(cb_ctx *ctx, const char *job, const char *game_id) { return CB_OK; }

static int fake_verify(cb_ctx *ctx, const struct cb_http_req *req, cb_user *out) {
    const char *b = cb_http_bearer(req);
    memset(out, 0, sizeof *out);
    if (!b) return CB_E_AUTH;
    if (strcmp(b, "A") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_A); snprintf(out->username, sizeof out->username, "ALICE"); return CB_OK; }
    if (strcmp(b, "B") == 0) { snprintf(out->id, sizeof out->id, "%s", UID_B); snprintf(out->username, sizeof out->username, "BOB"); return CB_OK; }
    return CB_E_AUTH;
}

// ---- a world: a SQLite file, its two users, and (journal) the journal on it ----------------

static void add_user(cb_store *s, const char *id, const char *name) {
    cb_user_row u;
    memset(&u, 0, sizeof u);
    snprintf(u.id, sizeof u.id, "%s", id);
    snprintf(u.email, sizeof u.email, "%s@x.io", name);
    snprintf(u.password_hash, sizeof u.password_hash, "$2b$10$abcdefghijklmnopqrstuuabcdefghijklmnopqrstuvwxyz01234");
    snprintf(u.username, sizeof u.username, "%s", name);
    u.created_at = 1;
    CB_STORE_CALL(s, user_create, &u);
}

static cb_store *open_world(const char *path, bool journal, char *err, size_t cap) {
    cb_store *s = NULL;
    if (cb_store_open_sqlite(path, &s) != CB_OK) return NULL;
    add_user(s, UID_A, "ALICE");
    add_user(s, UID_B, "BOB");
    char e[256];
    cb_config_from_env(&g_cfg, e, sizeof e);
    if (g_ctx.router) cb_router_free(g_ctx.router);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.store = s;
    g_ctx.job_start = no_job;
    g_ctx.router = cb_router_new();
    cb_functions_routes(&g_ctx);
    cb_row_cache_clear();
    cb_app_now = fake_now;
    cb_functions_verify = fake_verify;
    cb_limits_reset();
    if (journal) {
        cb_journal_opts o = { .path = path, .flush_ms = 3600000, .flush_moves = 1 << 30, .apply = cb_table_replay_move,
                              .apply_ud = &g_ctx };
        if (cb_journal_open(s, &o, err, cap) != CB_OK) {
            cb_store_close(s);
            return NULL;
        }
    }
    return s;
}

static void close_world(cb_store *s) {
    cb_journal_close(s);
    cb_store_close(s);
    cb_row_cache_clear();
}

// ---- requests (unit_app.c's) --------------------------------------------------------------

typedef struct {
    int     status;
    uint8_t body[16384];
    size_t  len;
} reply;

static void call(const char *fn, const char *bearer, const void *body, size_t n, reply *out) {
    cb_http_req req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "POST");
    char path[64], auth[64];
    snprintf(path, sizeof path, "/v1/%s", fn);
    req.path = path;
    req.query = "";
    snprintf(auth, sizeof auth, "Bearer %s", bearer);
    req.headers[req.n_headers++] = (cb_http_header){ "Authorization", auth };
    req.headers[req.n_headers++] = (cb_http_header){ "Content-Type", "application/octet-stream" };
    req.body = body;
    req.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &req, &res);
    out->status = res.status;
    out->len = res.body.len < sizeof out->body ? res.body.len : sizeof out->body;
    if (out->len) memcpy(out->body, res.body.p, out->len);
    cb_res_free(&res);
}

static void meta(const char *bearer, int kind, const char *gid, reply *out) {
    TableAsk a;
    memset(&a, 0, sizeof a);
    a.kind = (uint8_t)kind;
    a.gid_len = (uint8_t)strlen(gid);
    memcpy(a.gid, gid, a.gid_len);
    uint8_t body[ASK_BYTES_MAX];
    int n = ask_encode(&a, body, sizeof body);
    call("meta", bearer, body, n > 0 ? (size_t)n : 0, out);
}

static void action(const char *bearer, const char *gid, const uint8_t *wire, int wire_len, uint32_t intent, reply *out) {
    uint8_t body[256];
    int gl = (int)strlen(gid);
    memcpy(body, gid, (size_t)gl);
    memcpy(body + gl, wire, (size_t)wire_len);
    int n = table_request_encode(body, sizeof body, gl, wire_len, true, intent);
    call("action", bearer, body, n > 0 ? (size_t)n : 0, out);
}

static cb_game_row stored(cb_store *s, const char *gid) {
    cb_game_row row;
    memset(&row, 0, sizeof row);
    if (CB_ROWS_CALL(s, game_get, gid, &row) == CB_OK) CB_ROWS_CALL(s, game_logs, gid, row.log_from, &row.logs, &row.logs_len);
    return row;
}

static bool first_game(const cb_view_row *row, void *ud) {
    snprintf((char *)ud, CB_GAME_ID_MAX + 1, "%s", row->game_id);
    return false;
}

// A 2-human game dealt on the world's store.
static void two_humans_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    call("create", "A", NULL, 0, &r);
    gid[0] = 0;
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    meta("B", ASK_JOIN, gid, &r);
    meta("A", ASK_START, gid, &r);
    meta("B", ASK_START, gid, &r);
}

static int seat_of(const cb_game_row *row, const char *uid) {
    Roster r;
    int seat = -1;
    if (roster_decode(&r, row->roster, (int)row->roster_len) == ROSTER_OK)
        for (int i = 0; i < r.n; i++) if (strlen(uid) == r.seats[i].id_len && memcmp(r.seats[i].id, uid, r.seats[i].id_len) == 0) seat = i;
    return seat;
}

// A legal move for `seat` on `row` (WAIT excluded) as its action wire, by a seeded stream. 0: none.
static uint64_t g_rng = 12345;
static int pick_move(const cb_game_row *row, int seat, uint8_t *wire, int cap) {
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    LegalMoves *moves = calloc(1, sizeof *moves);
    Table t;
    table_init(&t, g, snaps);
    int n = 0;
    if (table_load(&t, row->state, (int)row->state_len, row->roster, (int)row->roster_len) == TABLE_OK) {
        calculate_legal_moves(g, seat, moves);
        int k = 0;
        for (int i = 0; i < moves->n; i++) k += moves->moves[i].type != MOVE_WAIT;
        if (k > 0) {
            int want = (int)(rng_u32(&g_rng) % (uint32_t)k);
            for (int i = 0; i < moves->n; i++) {
                const LegalMove *m = &moves->moves[i];
                if (m->type == MOVE_WAIT || want--) continue;
                AwireAction a;
                memset(&a, 0, sizeof a);
                a.kind = m->type;
                a.n = m->n_cards;
                for (int c = 0; c < m->n_cards; c++) { a.cards[c] = m->cards[c]; a.attacks[c] = m->attack_cards[c]; }
                // Every third attack of two or more cards goes in another order than the menu's: the
                // journal's escape (the move's bytes whole) is played too.
                if (a.kind == AWIRE_ATTACK && a.n > 1 && rng_u32(&g_rng) % 3 == 0) {
                    Card c0 = a.cards[0];
                    a.cards[0] = a.cards[a.n - 1];
                    a.cards[a.n - 1] = c0;
                }
                n = awire_encode(&a, wire, cap);
                break;
            }
        }
    }
    free(g);
    free(snaps);
    free(moves);
    return n;
}

// One move by whoever has one, at the clock's next tick. The applied version, or -1.
typedef struct { const char *bearer; uint8_t wire[64]; int n; uint32_t intent; } move_t;
static bool next_move(cb_store *s, const char *gid, move_t *m) {
    static const char *const uids[2] = { UID_A, UID_B }, *const bearers[2] = { "A", "B" };
    cb_game_row row = stored(s, gid);
    bool ok = false;
    for (int k = 0; k < 2 && !ok && row.status == GAME_STATUS_PLAYING; k++) {
        int seat = seat_of(&row, uids[k]);
        if (seat < 0 || (m->n = pick_move(&row, seat, m->wire, sizeof m->wire)) <= 0) continue;
        m->bearer = bearers[k];
        m->intent = (uint32_t)row.version;
        ok = true;
    }
    cb_game_row_free(&row);
    return ok;
}

static int64_t play(const char *gid, const move_t *m) {
    g_now += 1237;
    reply r;
    action(m->bearer, gid, m->wire, m->n, m->intent, &r);
    TableResponse resp;
    if (getenv("JDEBUG") && r.status != 200) fprintf(stderr, "play: status %d len %zu\n", r.status, r.len);
    if (r.status != 200 || table_response_decode(r.body, (int)r.len, &resp) != 0 || resp.status != TABLE_STATUS_APPLIED) return -1;
    return resp.version;
}

// What a game is: every column of its row and its session log.
static uint64_t row_hash(cb_store *s, const char *gid) {
    cb_game_row g = stored(s, gid);
    uint64_t h = FNV64_OFFSET;
    int64_t f[] = { g.status, g.version, g.needs_bots, g.round_epoch, g.log_from, g.created_at, g.updated_at, g.last_commit_at, g.has_seed };
    h = fnv1a64(h, f, sizeof f);
    h = fnv1a64(h, g.state, g.state_len);
    h = fnv1a64(h, g.roster, g.roster_len);
    h = fnv1a64(h, g.seed, sizeof g.seed);
    h = fnv1a64(h, g.logs, g.logs_len);
    cb_game_row_free(&g);
    return h;
}

// ---- the codec -----------------------------------------------------------------------------

static void random_move(uint64_t *rng, ReplayMove *m) {
    memset(m, 0, sizeof *m);
    m->seat = (int)(rng_u32(rng) % MAX_PLAYERS);
    m->menu_n = (int)(rng_u32(rng) % 200);
    m->index = (int)(rng_u32(rng) % (uint32_t)(m->menu_n + 1));
    if (m->index == m->menu_n) {
        m->wire_len = (int)(rng_u32(rng) % 12);
        for (int i = 0; i < m->wire_len; i++) m->wire[i] = (uint8_t)rng_u32(rng);
    }
}

static bool same_move(const cb_jmove *a, const cb_jmove *b) {
    return strcmp(a->game, b->game) == 0 && a->base_version == b->base_version && a->now_ms == b->now_ms &&
           a->mv.seat == b->mv.seat && a->mv.menu_n == b->mv.menu_n && a->mv.index == b->mv.index &&
           a->mv.wire_len == b->mv.wire_len && memcmp(a->mv.wire, b->mv.wire, (size_t)a->mv.wire_len) == 0;
}

enum { NB = 3, PER = 20 };
static cb_jmove g_in[NB][PER];

// NB batches of PER moves over 5 games (one jumps its version), encoded back to back into buf.
static size_t encode_batches(uint8_t *buf, size_t cap, size_t ends[NB]) {
    static const char *const games[] = { "a1b2c3d4e5f6", "0123456789ab", "ffeeddccbbaa", "abc123", "9f9f9f9f9f9f" };
    int64_t ver[5] = { 7, 100, 3, 55555, 1 };
    uint64_t rng = 99;
    cb_jslots *t = cb_jslots_new();
    size_t len = 0;
    for (int b = 0; b < NB; b++) {
        for (int i = 0; i < PER; i++) {
            cb_jmove *m = &g_in[b][i];
            memset(m, 0, sizeof *m);
            int k = (int)(rng_u32(&rng) % 5);
            snprintf(m->game, sizeof m->game, "%s", games[k]);
            if (b == 1 && i == 3 && k == 2) ver[k] += 40;   // the game was written around the journal between
            m->base_version = ver[k]++;
            m->now_ms = 1760000000000LL + b * 5000 + (int64_t)(rng_u32(&rng) % 3000);
            random_move(&rng, &m->mv);
        }
        int n = cb_journal_batch_encode(t, 1760000000000LL - 10, g_in[b], PER, buf + len, (int)(cap - len));
        T_CHECK(n > CB_JOURNAL_BATCH_HDR);
        len += n > 0 ? (size_t)n : 0;
        ends[b] = len;
    }
    T_EQ_INT(cb_jslots_count(t), 5);
    cb_jslots_free(t);
    return len;
}

// Decodes buf as far as it goes: the moves read, checked against g_in in order.
static int decode_all(const uint8_t *buf, size_t len) {
    cb_jslots *t = cb_jslots_new();
    cb_jmove out[CB_JOURNAL_BATCH_MAX];
    size_t at = 0;
    int total = 0;
    while (at < len) {
        size_t used = 0;
        int n = cb_journal_batch_decode(t, 1760000000000LL - 10, buf + at, len - at, &used, out, CB_JOURNAL_BATCH_MAX);
        if (n < 0) break;
        for (int i = 0; i < n; i++, total++) T_CHECK(same_move(&out[i], &g_in[total / PER][total % PER]));
        at += used;
    }
    cb_jslots_free(t);
    return total;
}

static void a_batch_round_trips(void) {
    static uint8_t buf[3 * 9000];
    size_t ends[NB];
    size_t len = encode_batches(buf, sizeof buf, ends);
    T_EQ_INT(decode_all(buf, len), NB * PER);
    // A move costs a few bytes: 20 moves of 5 games under a 9-byte-per-move bound, headers included.
    T_CHECK(ends[1] - ends[0] < 9 * PER);
}

static void a_torn_or_corrupt_tail_drops_only_itself(void) {
    static uint8_t buf[3 * 9000];
    size_t ends[NB];
    size_t len = encode_batches(buf, sizeof buf, ends);
    // Cut in the middle of the last batch: the two before it are whole.
    T_EQ_INT(decode_all(buf, ends[1] + (ends[2] - ends[1]) / 2), 2 * PER);
    T_EQ_INT(decode_all(buf, ends[1] + 3), 2 * PER);   // even inside its header
    // A bit flipped in the second batch's payload: the first stays, the rest goes.
    buf[ends[0] + CB_JOURNAL_BATCH_HDR + 2] ^= 0x10;
    T_EQ_INT(decode_all(buf, len), PER);
    buf[ends[0] + CB_JOURNAL_BATCH_HDR + 2] ^= 0x10;
    // And in its checksum.
    buf[ends[0] + 5] ^= 0x01;
    T_EQ_INT(decode_all(buf, len), PER);
}

// ---- recovery after a SIGKILL ---------------------------------------------------------------

typedef struct { int64_t version; uint64_t hash; } ack_t;

// The child: plays until killed, reporting each acknowledged move's version and the game's hash.
static void child_play(const char *path, const char *gid, int fd) {
    char err[256];
    cb_store *s = open_world(path, true, err, sizeof err);
    if (!s) _exit(3);
    move_t m;
    while (next_move(s, gid, &m)) {
        int64_t v = play(gid, &m);
        if (v < 0) continue;
        ack_t a = { v, row_hash(s, gid) };
        if (write(fd, &a, sizeof a) != sizeof a) _exit(4);
    }
    _exit(0);
}

static void every_acknowledged_move_survives_a_sigkill(void) {
    int compared = 0;
    for (int round = 0; round < 6; round++) {
        char path[320], err[256] = "";
        snprintf(path, sizeof path, "%s/k%d.sqlite", g_dir, round);
        cb_store *s = open_world(path, true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) return;
        char gid[CB_GAME_ID_MAX + 1];
        two_humans_dealt(gid);
        cb_game_row r0 = stored(s, gid);
        const int64_t v0 = r0.version;
        cb_game_row_free(&r0);
        close_world(s);

        int p[2];
        T_EQ_INT(pipe(p), 0);
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            child_play(path, gid, p[1]);
        }
        close(p[1]);
        // Kill it after a few acknowledgements, at a different point each round.
        ack_t last = { v0, 0 }, a;
        const int want = 3 + round * 4;
        int got = 0;
        static ack_t acks[4096];
        while (got < want && read(p[0], &a, sizeof a) == sizeof a) acks[got++] = last = a;
        kill(pid, SIGKILL);
        int st;
        waitpid(pid, &st, 0);
        while (read(p[0], &a, sizeof a) == sizeof a && got < 4096) acks[got++] = last = a;   // what it said before it died
        close(p[0]);
        T_CHECK(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) == 0));

        // A torn half-batch at the tail, as a crash in the middle of a write leaves it: dropped.
        char seg[400];
        for (int k = 9999; k >= 1; k--) {
            snprintf(seg, sizeof seg, "%s-journal.%010d", path, k);
            if (access(seg, F_OK) == 0) {
                FILE *f = fopen(seg, "ab");
                static const uint8_t torn[5] = { 'J', 1, 40, 0, 7 };
                if (f) { fwrite(torn, 1, sizeof torn, f); fclose(f); }
                break;
            }
        }

        s = open_world(path, true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) { fprintf(stderr, "reopen: %s\n", err); return; }
        cb_game_row row = stored(s, gid);
        // Every acknowledged move is there (one more may be: written, not yet reported).
        T_CHECK(row.version >= last.version);
        T_CHECK(row.version <= last.version + 1);
        uint64_t h = row_hash(s, gid);
        for (int i = 0; i < got; i++)
            if (acks[i].version == row.version) { T_CHECK(acks[i].hash == h); compared++; }
        cb_game_row_free(&row);
        close_world(s);
        // The recovered moves went into the tables: the tables alone hold the same game.
        s = open_world(path, false, err, sizeof err);
        T_CHECK(s != NULL);
        if (s) { T_CHECK(row_hash(s, gid) == h); close_world(s); }
    }
    T_CHECK(compared > 0);
}

// ---- the flush writes what per-move commits write ------------------------------------------

static void sql_dump(const char *path, const char *gid, uint64_t *games, uint64_t *moves, int *n_moves) {
    sqlite3 *db = NULL;
    *games = *moves = FNV64_OFFSET;
    *n_moves = 0;
    T_EQ_INT(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT * FROM games WHERE id = ?1", -1, &st, NULL);
    sqlite3_bind_text(st, 1, gid, -1, SQLITE_STATIC);
    T_EQ_INT(sqlite3_step(st), SQLITE_ROW);
    for (int c = 0; c < sqlite3_column_count(st); c++) {
        const void *b = sqlite3_column_blob(st, c);
        *games = fnv1a64(*games, b ? b : "", (size_t)sqlite3_column_bytes(st, c));
    }
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "SELECT seq, action FROM game_moves WHERE game_id = ?1 ORDER BY seq", -1, &st, NULL);
    sqlite3_bind_text(st, 1, gid, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t seq = sqlite3_column_int64(st, 0);
        *moves = fnv1a64(*moves, &seq, sizeof seq);
        *moves = fnv1a64(*moves, sqlite3_column_blob(st, 1), (size_t)sqlite3_column_bytes(st, 1));
        (*n_moves)++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void the_flush_writes_the_rows_per_move_commits_write(void) {
    char pa[320], pb[320], err[256] = "";
    snprintf(pa, sizeof pa, "%s/plain.sqlite", g_dir);
    snprintf(pb, sizeof pb, "%s/journaled.sqlite", g_dir);
    // The game, dealt on the plain store, and the same row imported into the journaled one.
    cb_store *a = open_world(pa, false, err, sizeof err);
    T_CHECK(a != NULL);
    if (!a) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_game_row dealt = stored(a, gid);
    close_world(a);
    cb_store *b = open_world(pb, true, err, sizeof err);
    T_CHECK(b != NULL);
    if (!b) return;
    cb_uuid humans[2];
    snprintf(humans[0], sizeof humans[0], "%s", UID_A);
    snprintf(humans[1], sizeof humans[1], "%s", UID_B);
    T_EQ_INT(CB_ROWS_CALL(b, game_import, &dealt, humans, 2, NULL, 0), CB_OK);
    close_world(b);
    cb_game_row_free(&dealt);
    // The same moves at the same times on each, one store open at a time (the row cache is the process's).
    const int64_t t0 = g_now;
    move_t ms[40];
    int n = 0;
    a = open_world(pa, false, err, sizeof err);
    for (; n < 40 && next_move(a, gid, &ms[n]); n++) T_CHECK(play(gid, &ms[n]) > 0);
    uint64_t ha = row_hash(a, gid);
    close_world(a);
    g_now = t0;
    b = open_world(pb, true, err, sizeof err);
    for (int i = 0; i < n; i++) T_CHECK(play(gid, &ms[i]) > 0);
    // Before the flush the journaled store reads the same game (the overlay), and the tables are behind.
    T_CHECK(row_hash(b, gid) == ha);
    uint64_t ga, ma, gb, mb;
    int na, nb;
    sql_dump(pa, gid, &ga, &ma, &na);
    sql_dump(pb, gid, &gb, &mb, &nb);
    T_CHECK(gb != ga);
    cb_journal_stats js;
    cb_journal_stats_get(b, &js);
    T_EQ_INT(js.records, n);
    T_EQ_INT(cb_journal_flush(b), CB_OK);
    sql_dump(pb, gid, &gb, &mb, &nb);
    T_CHECK(gb == ga);
    T_CHECK(mb == ma);
    T_EQ_INT(nb, na);
    T_CHECK(n > 10);
    close_world(b);
}

// ---- a backup mid-game ----------------------------------------------------------------------

static int file_put(void *ud, const char *key, const char *path) {
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "cp '%s' '%s/%s'", path, (const char *)ud, key);
    return system(cmd) == 0 ? CB_OK : CB_E_IO;
}

static void a_backup_mid_game_holds_every_acknowledged_move(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/live.sqlite", g_dir);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    g_cfg.db_path[0] = 0;
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", path);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    move_t m;
    int64_t v = -1;
    for (int i = 0; i < 12 && next_move(s, gid, &m); i++) v = play(gid, &m);
    T_CHECK(v > 0);
    uint64_t h = row_hash(s, gid);
    cb_blob blob = { file_put, NULL, g_dir };
    cb_backup *bk = cb_backup_start(&g_ctx, &blob, "backup.sqlite", 0);
    T_CHECK(bk != NULL);
    T_EQ_INT(cb_backup_once(bk), 1);
    cb_backup_stop(bk);
    close_world(s);
    // The copy alone, opened with no journal: the game at the last acknowledged move.
    char copy[320];
    snprintf(copy, sizeof copy, "%s/backup.sqlite", g_dir);
    s = open_world(copy, false, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    cb_game_row row = stored(s, gid);
    T_EQ_INT(row.version, v);
    cb_game_row_free(&row);
    T_CHECK(row_hash(s, gid) == h);
    close_world(s);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/unit_journal.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) return 1;
    T_RUN(a_batch_round_trips);
    T_RUN(a_torn_or_corrupt_tail_drops_only_itself);
    T_RUN(the_flush_writes_the_rows_per_move_commits_write);
    T_RUN(a_backup_mid_game_holds_every_acknowledged_move);
    if (!getenv("JSKIPKILL")) T_RUN(every_acknowledged_move_survives_a_sigkill);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    (void)system(cmd);
    return T_DONE();
}

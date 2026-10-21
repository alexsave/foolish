// unit_journal.c - the move journal (store/journal.h): the batch codec, a torn or corrupt tail,
// recovery after a SIGKILL, the flush's rows against per-move commits, and a backup mid-game.
#include "oom.h"
#include "t.h"
#include "app/app_internal.h"
#include "app/foolish_app.h"
#include "app/functions_http.h"
#include "app/table_ops.h"
#include "app/bots.h"
#include "cbase/provider.h"
#include "handler.h"
#include "net/router.h"
#include "rate_limits.h"
#include "store/backup.h"
#include "store/journal.h"
#include "../../../shared/c/crc32c/crc32c.h"

// The segment format journal.c writes (journal.h SEGMENT FILE).
#define JOURNAL_FORMAT 2
#include "util/clock.h"
#include "store/store_sql.h"
#include "awire.h"
#include "legal.h"
#include "bot_roster.h"
#include "roster.h"
#include "table.h"
#include "util/err.h"
#include "../../../shared/c/fnv.h"
#include "../../../shared/c/rng.h"

#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
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
static void fake_sleep(int64_t ms) { g_now += ms; }
// Each test runs twice: two humans, and one human against a bot (whose cycles the journal holds too).
static bool g_bots;
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

static cb_journal_opts g_jo;
static bool last_frame(const char *db, int magic, char *seg, size_t cap, long *off);
#define last_batch(db, seg, cap, off) last_frame((db), 'J', (seg), (cap), (off))
static void crash_copy(const char *path, const char *copy);
static bool no_row(int n, const char *const *names, const char *const *values, void *ud);
static bool game_gone(cb_store *s, const char *gid);

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
    cb_app_now = fake_now;
    cb_app_sleep = fake_sleep;
    cb_functions_verify = fake_verify;
    cb_limits_reset();
    if (journal) {
        cb_journal_opts o = g_jo;   // a test's flush rule, else nothing is due for an hour
        if (!o.game_age_ms) o.game_age_ms = 3600000;
        o.path = path;
        o.apply = cb_table_replay_move;
        o.apply_ud = &g_ctx;
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

// A game dealt on the world's store: A and B, or A and a bot (g_bots).
static void two_humans_dealt(char gid[CB_GAME_ID_MAX + 1]) {
    reply r;
    call("create", "A", NULL, 0, &r);
    gid[0] = 0;
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    if (g_bots) {
        meta("A", ASK_ADD_BOT, gid, &r);
        meta("A", ASK_START, gid, &r);
        return;
    }
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
    for (int k = 0; k < (g_bots ? 1 : 2) && !ok && row.status == GAME_STATUS_PLAYING; k++) {
        int seat = seat_of(&row, uids[k]);
        if (seat < 0 || (m->n = pick_move(&row, seat, m->wire, sizeof m->wire)) <= 0) continue;
        m->bearer = bearers[k];
        m->intent = (uint32_t)row.version;
        ok = true;
    }
    cb_game_row_free(&row);
    return ok;
}

static int64_t version_of(cb_store *s, const char *gid) {
    cb_game_row row = stored(s, gid);
    int64_t v = row.status == GAME_STATUS_PLAYING ? row.version : -row.version;
    cb_game_row_free(&row);
    return v;
}

// One step of the game: a human's move when one has a move, else a bot drive (to the next
// human move). m->n == 0 marks a drive. The version after it, or -1 when the game is over.
static int64_t play(const char *gid, const move_t *m);
static int64_t step(cb_store *s, const char *gid, move_t *m) {
    if (next_move(s, gid, m)) return play(gid, m);
    m->n = 0;
    if (!g_bots || version_of(s, gid) < 0) return -1;
    return cb_bots_drive(&g_ctx, gid) == CB_OK ? version_of(s, gid) : -1;
}

static int64_t play(const char *gid, const move_t *m) {
    if (m->n == 0) return cb_bots_drive(&g_ctx, gid) == CB_OK ? version_of(g_ctx.store, gid) : -1;
    g_now += 1237;
    reply r;
    action(m->bearer, gid, m->wire, m->n, m->intent, &r);
    TableResponse resp;
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

// A random awire frame (awire.h) for a random seat: what a move list holds.
static void random_move(uint64_t *rng, EvwMove *m) {
    memset(m, 0, sizeof *m);
    m->seat = (int8_t)(rng_u32(rng) % MAX_PLAYERS);
    AwireAction a;
    memset(&a, 0, sizeof a);
    a.kind = (int)(rng_u32(rng) % 5);
    a.n = a.kind == AWIRE_PICKUP || a.kind == AWIRE_GOOD ? 0 : 1 + (int)(rng_u32(rng) % 6);
    for (int i = 0; i < a.n; i++) {
        a.cards[i] = card_of_id((int)(rng_u32(rng) % 52));
        a.attacks[i] = card_of_id((int)(rng_u32(rng) % 52));
    }
    const int n = awire_encode(&a, m->wire, (int)sizeof m->wire);
    T_CHECK(n >= 2);
    m->wire_len = (uint8_t)n;
}

static bool same_move(const cb_jmove *a, const cb_jmove *b) {
    bool same = strcmp(a->game, b->game) == 0 && a->base_version == b->base_version && a->now_ms == b->now_ms &&
                a->bots == b->bots && a->n_mv == b->n_mv;
    for (int k = 0; same && k < a->n_mv; k++) {
        const EvwMove *x = &a->mv[k], *y = &b->mv[k];
        same = x->seat == y->seat && x->wire_len == y->wire_len && memcmp(x->wire, y->wire, x->wire_len) == 0;
    }
    return same;
}

enum { NB = 3, PER = 20 };
static cb_jmove g_in[NB][PER];
static EvwMove g_in_mv[NB][PER][EVW_DELTA_MAX_MOVES];
static size_t g_want[NB];   // each batch's length as journal.h lays a batch out

// NB batches of PER moves over 5 games (one jumps its version), encoded back to back into buf.
static size_t encode_batches(uint8_t *buf, size_t cap, size_t ends[NB]) {
    static const char *const games[] = { "a1b2c3d4e5f6", "0123456789ab", "ffeeddccbbaa", "abc123", "9f9f9f9f9f9f" };
    int64_t ver[5] = { 7, 100, 3, 55555, 1 };
    bool named[5] = { false };
    uint64_t rng = 99;
    cb_jslots *t = cb_jslots_new();
    size_t len = 0;
    for (int b = 0; b < NB; b++) {
        const cb_jmove *ptr[PER];
        g_want[b] = CB_JOURNAL_BATCH_HDR + 1 + 4;
        for (int i = 0; i < PER; i++) {
            cb_jmove *m = &g_in[b][i];
            memset(m, 0, sizeof *m);
            int k = (int)(rng_u32(&rng) % 5);
            snprintf(m->game, sizeof m->game, "%s", games[k]);
            bool jumped = false;
            if (b == 1 && i == 3 && k == 2) ver[k] += 40, jumped = true;   // the game was written around the journal between
            m->base_version = ver[k]++;
            m->now_ms = 1760000000000LL + b * 5000 + (int64_t)(rng_u32(&rng) % 3000);
            m->bots = rng_u32(&rng) % 3 == 0;   // a third are bot cycles of 1..9 moves
            m->n_mv = m->bots ? 1 + (int)(rng_u32(&rng) % EVW_DELTA_MAX_MOVES) : 1;
            for (int q = 0; q < m->n_mv; q++) random_move(&rng, &g_in_mv[b][i][q]);
            m->mv = g_in_mv[b][i];
            ptr[i] = m;
            // flags, then a new game's id and version or the slot (and a jump's version), the ms, the list
            g_want[b] += 1 + (named[k] ? 2 + (jumped ? 4 : 0) : 1 + strlen(games[k]) + 4) + 4 + 1;
            for (int q = 0; q < m->n_mv; q++) g_want[b] += 2 + m->mv[q].wire_len;
            named[k] = true;
        }
        int n = cb_journal_batch_encode(t, 1760000000000LL - 10, ptr, PER, buf + len, (int)(cap - len));
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
    static cb_jmove out[CB_JOURNAL_BATCH_MAX];
    static EvwMove mv[CB_JOURNAL_BATCH_MAX][EVW_DELTA_MAX_MOVES];
    size_t at = 0;
    int total = 0;
    while (at < len) {
        size_t used = 0;
        int n = cb_journal_batch_decode(t, 1760000000000LL - 10, buf + at, len - at, &used, out, mv, CB_JOURNAL_BATCH_MAX);
        if (n < 0) break;
        for (int i = 0; i < n; i++, total++) T_CHECK(same_move(&out[i], &g_in[total / PER][total % PER]));
        at += used;
    }
    cb_jslots_free(t);
    return total;
}

static void a_batch_round_trips(void) {
    static uint8_t buf[3 * 16000];
    size_t ends[NB];
    size_t len = encode_batches(buf, sizeof buf, ends);
    T_EQ_INT(decode_all(buf, len), NB * PER);
    // Each batch is exactly the fixed layout journal.h gives: nothing coded, nothing padded.
    for (int b = 0; b < NB; b++) T_EQ_INT((int)(ends[b] - (b ? ends[b - 1] : 0)), (int)g_want[b]);
}

// A batch whose checksum holds but whose payload runs past its last record (a byte more, the
// length and the CRC-32C made to match) is not a batch: the decoder reads it whole or not at all.
static void a_batch_with_bytes_past_its_records_is_refused(void) {
    static uint8_t buf[3 * 16000], bad[16000];
    size_t ends[NB];
    encode_batches(buf, sizeof buf, ends);
    const size_t n = ends[0], plen = n - CB_JOURNAL_BATCH_HDR;
    memcpy(bad, buf, n);
    bad[n] = 0;
    bad[2] = (uint8_t)(plen + 1), bad[3] = (uint8_t)((plen + 1) >> 8);
    const uint32_t sum = crc32c(crc32c(0, bad, 4), bad + CB_JOURNAL_BATCH_HDR, plen + 1);
    for (int i = 0; i < 4; i++) bad[4 + i] = (uint8_t)(sum >> (8 * i));
    T_EQ_INT(decode_all(bad, n + 1), 0);
    T_EQ_INT(decode_all(buf, n), PER);   // the batch as written reads
}

// A batch that does not fit its room is refused whole: the slot table is as it was, so the
// next batch names the same games the same way.
static void a_refused_batch_consumes_nothing(void) {
    uint64_t rng = 7;
    static EvwMove mv[2][EVW_DELTA_MAX_MOVES];
    cb_jmove m[2];
    memset(m, 0, sizeof m);
    snprintf(m[0].game, sizeof m[0].game, "a1b2c3d4e5f6");
    snprintf(m[1].game, sizeof m[1].game, "0123456789ab");
    for (int i = 0; i < 2; i++) {
        m[i].base_version = 10 + i;
        m[i].now_ms = 1760000000500LL;
        m[i].n_mv = 1;
        random_move(&rng, &mv[i][0]);
        m[i].mv = mv[i];
    }
    const cb_jmove *ptr[2] = { &m[0], &m[1] };
    cb_jslots *t = cb_jslots_new(), *fresh = cb_jslots_new();
    uint8_t a[512], b[512];
    // Room for the first record only: refused, nothing named.
    const int whole = cb_journal_batch_encode(fresh, 1760000000000LL, ptr, 2, b, sizeof b);
    T_CHECK(whole > 0);
    T_EQ_INT(cb_journal_batch_encode(t, 1760000000000LL, ptr, 2, a, whole - 1), CB_E_FULL);
    T_EQ_INT(cb_jslots_count(t), 0);
    // A game the table held keeps its next version too.
    const cb_jmove *one[1] = { &m[0] };
    cb_jslots_free(fresh);
    fresh = cb_jslots_new();
    T_CHECK(cb_journal_batch_encode(t, 1760000000000LL, one, 1, a, sizeof a) > 0);
    T_CHECK(cb_journal_batch_encode(fresh, 1760000000000LL, one, 1, b, sizeof b) > 0);
    m[0].base_version++;
    const int w2 = cb_journal_batch_encode(fresh, 1760000000000LL, ptr, 2, b, sizeof b);
    T_EQ_INT(cb_journal_batch_encode(t, 1760000000000LL, ptr, 2, a, w2 - 1), CB_E_FULL);
    T_EQ_INT(cb_jslots_count(t), 1);
    T_EQ_INT(cb_journal_batch_encode(t, 1760000000000LL, ptr, 2, a, sizeof a), w2);
    T_CHECK(memcmp(a, b, (size_t)w2) == 0);   // the same bytes a table that never saw the refusal writes
    cb_jslots_free(t);
    cb_jslots_free(fresh);
}

static void a_torn_or_corrupt_tail_drops_only_itself(void) {
    static uint8_t buf[3 * 16000];
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

// ---- a move checks the board it is replayed on ---------------------------------------------

static void a_move_off_its_board_is_refused(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/board.sqlite", g_dir);
    cb_store *s = open_world(path, false, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_game_row row = stored(s, gid);
    int seat = -1, n = 0;
    uint8_t wire[64];
    for (int k = 0; k < 2 && n <= 0; k++) n = pick_move(&row, seat = k, wire, sizeof wire);
    T_CHECK(n > 0);
    EvwMove mv;
    memset(&mv, 0, sizeof mv);
    mv.seat = (int8_t)seat;
    mv.wire_len = (uint8_t)n;
    memcpy(mv.wire, wire, (size_t)n);
    cb_jmove m;
    memset(&m, 0, sizeof m);
    snprintf(m.game, sizeof m.game, "%s", gid);
    m.base_version = row.version;
    m.now_ms = row.updated_at + 1000;
    m.n_mv = 1;
    m.mv = &mv;
    // On its own board the record is the move: the commit at the next version.
    cb_game_row out;
    uint8_t *log = NULL;
    size_t log_len = 0;
    T_EQ_INT(cb_table_replay_move(&g_ctx, &row, NULL, 0, &m, &out, &log, &log_len), CB_OK);
    T_EQ_INT(out.version, row.version + 1);
    cb_game_row_free(&out);
    free(log);
    // The other seat, whose move it is not, is refused: the kernel acts it and says no.
    if (pick_move(&row, 1 - seat, wire, sizeof wire) <= 0) {
        mv.seat = (int8_t)(1 - seat);
        T_CHECK(cb_table_replay_move(&g_ctx, &row, NULL, 0, &m, &out, &log, &log_len) != CB_OK);
        mv.seat = (int8_t)seat;
    }
    // A seat past the roster names no player.
    mv.seat = MAX_PLAYERS - 1;
    T_CHECK(cb_table_replay_move(&g_ctx, &row, NULL, 0, &m, &out, &log, &log_len) != CB_OK);
    free(log);
    cb_game_row_free(&row);
    close_world(s);
}

// ---- a move allocates (almost) nothing --------------------------------------------------------

// A human's move, from the request to the journal's acknowledgement, on the thread that runs it.
// What it may still allocate: the journal's own copy of the move's log delta (the overlay keeps
// it), and this HTTP response's header and body buffers, grown from nothing (a socket's reply
// keeps them with its job, rt_local.c). Every other copy is a kept buffer: the section's row
// buffers, the cache slot's, the arena's casts and envelopes, the spare game locks.
#define MOVE_ALLOCS_MAX 5
static void a_move_allocates_almost_nothing(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/allocs.sqlite", g_dir);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    move_t m;
    int most = -1, played = 0;
    for (int i = 0; i < 16 && next_move(s, gid, &m); i++) {
        if (i < 4) { T_CHECK(play(gid, &m) > 0); continue; }   // the kept buffers grow first
        if (!OOM_AVAILABLE) { play(gid, &m); continue; }
        oom_fail_after(1L << 40);
        int64_t v = play(gid, &m);
        int n = (int)oom_count();
        oom_off();
        T_CHECK(v > 0);
        if (n > most) most = n;
        played++;
    }
    if (OOM_AVAILABLE) {
        T_CHECK(played >= 4);
        if (most > MOVE_ALLOCS_MAX) fprintf(stderr, "     a move allocated %d blocks (at most %d)\n", most, MOVE_ALLOCS_MAX);
        T_CHECK(most <= MOVE_ALLOCS_MAX);
    }
    close_world(s);
}

// ---- recovery after a SIGKILL ---------------------------------------------------------------

typedef struct { int64_t version; uint64_t hash; } ack_t;

// The child: plays until killed, reporting each acknowledged move's version and the game's hash.
// With `hold` >= 0 it waits on that pipe after each report, so a kill lands between two steps
// (a bot drive commits each cycle: killed mid-drive, the game is at a version nobody reported).
static void child_play(const char *path, const char *gid, int fd, int hold) {
    char err[256];
    cb_store *s = open_world(path, true, err, sizeof err);
    if (!s) _exit(3);
    move_t m;
    int64_t v;
    while ((v = step(s, gid, &m)) > 0) {
        ack_t a = { v, row_hash(s, gid) };
        if (write(fd, &a, sizeof a) != sizeof a) _exit(4);
        char go;
        if (hold >= 0 && read(hold, &go, 1) != 1) _exit(0);
    }
    _exit(0);
}

static void every_acknowledged_move_survives_a_sigkill(void) {
    int compared = 0;
    for (int round = 0; round < 6; round++) {
        char path[320], err[256] = "";
        snprintf(path, sizeof path, "%s/k%d%d.sqlite", g_dir, g_bots, round);
        cb_store *s = open_world(path, true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) return;
        char gid[CB_GAME_ID_MAX + 1];
        two_humans_dealt(gid);
        cb_game_row r0 = stored(s, gid);
        const int64_t v0 = r0.version;
        cb_game_row_free(&r0);
        close_world(s);

        int p[2], q[2];
        T_EQ_INT(pipe(p), 0);
        T_EQ_INT(pipe(q), 0);
        // Odd rounds kill anywhere; even ones between two steps, so the recovered game is the
        // last one reported and its hash is compared every time.
        const bool held = round % 2 == 0;
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            close(q[1]);
            child_play(path, gid, p[1], held ? q[0] : -1);
        }
        close(p[1]);
        close(q[0]);
        // Kill it after a few acknowledgements, at a different point each round.
        ack_t last = { v0, 0 }, a;
        const int want = 3 + round * 4;
        int got = 0;
        static ack_t acks[4096];
        while (got < want && read(p[0], &a, sizeof a) == sizeof a) {
            acks[got++] = last = a;
            if (held && got < want && write(q[1], "g", 1) != 1) break;
        }
        kill(pid, SIGKILL);
        close(q[1]);
        int st;
        waitpid(pid, &st, 0);
        while (read(p[0], &a, sizeof a) == sizeof a && got < 4096) acks[got++] = last = a;   // what it said before it died
        close(p[0]);
        T_CHECK(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) == 0));

        // A torn half-batch past the last whole one, as a crash in the middle of a batch's
        // header leaves it: dropped.
        char seg[700];
        long off = 0;
        if (last_batch(path, seg, sizeof seg, &off)) {
            FILE *f = fopen(seg, "r+b");
            uint8_t hd[4];
            if (f && fseek(f, off, SEEK_SET) == 0 && fread(hd, 1, 4, f) == 4) {
                static const uint8_t torn[5] = { 'J', 1, 40, 0, 7 };
                fseek(f, off + 8 + (hd[2] | hd[3] << 8), SEEK_SET);
                fwrite(torn, 1, sizeof torn, f);
            }
            if (f) fclose(f);
        }

        s = open_world(path, true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) { fprintf(stderr, "reopen: %s\n", err); return; }
        cb_game_row row = stored(s, gid);
        // Every acknowledged move is there (one more may be: written, not yet reported; with a
        // bot, one more drive, whose cycles each commit, may have been under way).
        T_CHECK(row.version >= last.version);
        if (!g_bots) T_CHECK(row.version <= last.version + 1);
        if (held) T_EQ_INT(row.version, last.version);
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
    snprintf(pa, sizeof pa, "%s/plain%d.sqlite", g_dir, g_bots);
    snprintf(pb, sizeof pb, "%s/journaled%d.sqlite", g_dir, g_bots);
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
    cb_uuid bots[CB_MAX_SEATS];
    int n_bots = 0;
    Roster ro;
    T_EQ_INT(roster_decode(&ro, dealt.roster, (int)dealt.roster_len), ROSTER_OK);
    for (int i = 0; i < ro.n; i++)
        if (ro.seats[i].brain_len) snprintf(bots[n_bots++], sizeof bots[0], "%.*s", (int)ro.seats[i].id_len, (const char *)ro.seats[i].id);
    const int64_t v_dealt = dealt.version;
    T_EQ_INT(CB_ROWS_CALL(b, game_import, &dealt, humans, g_bots ? 1 : 2, bots, n_bots), CB_OK);
    close_world(b);
    cb_game_row_free(&dealt);
    // The same moves at the same times on each, one store open at a time (the row cache is the process's).
    const int64_t t0 = g_now;
    move_t ms[40];
    int n = 0;
    a = open_world(pa, false, err, sizeof err);
    for (; n < 40 && step(a, gid, &ms[n]) > 0; n++) {}
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
    T_EQ_INT(js.records, version_of(b, gid) - v_dealt);   // every commit since the deal, bot cycles too
    T_EQ_INT(cb_journal_flush(b), CB_OK);
    sql_dump(pb, gid, &gb, &mb, &nb);
    T_CHECK(gb == ga);
    T_CHECK(mb == ma);
    T_EQ_INT(nb, na);
    T_CHECK(n > 10);
    close_world(b);
}


// A flush that races the moves: with game_bytes 1 every move kicks the flush thread, which writes
// what its snapshot took while later moves keep landing in the game's log buffer, so a flush
// often takes a prefix and leaves the rest waiting (ov_flushed's cut). After every move the game
// reads as the plain store's did after the same move, and the tables end the same.
static void a_flush_racing_the_moves_leaves_every_log_whole(void) {
    char pa[320], pb[320], err[256] = "";
    snprintf(pa, sizeof pa, "%s/rplain%d.sqlite", g_dir, g_bots);
    snprintf(pb, sizeof pb, "%s/rjournaled%d.sqlite", g_dir, g_bots);
    cb_store *a = open_world(pa, false, err, sizeof err);
    T_CHECK(a != NULL);
    if (!a) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_game_row dealt = stored(a, gid);
    close_world(a);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.game_bytes = 1;
    cb_store *b = open_world(pb, true, err, sizeof err);
    T_CHECK(b != NULL);
    if (!b) { cb_game_row_free(&dealt); return; }
    cb_uuid humans[2], bots[CB_MAX_SEATS];
    snprintf(humans[0], sizeof humans[0], "%s", UID_A);
    snprintf(humans[1], sizeof humans[1], "%s", UID_B);
    int n_bots = 0;
    Roster ro;
    T_EQ_INT(roster_decode(&ro, dealt.roster, (int)dealt.roster_len), ROSTER_OK);
    for (int i = 0; i < ro.n; i++)
        if (ro.seats[i].brain_len) snprintf(bots[n_bots++], sizeof bots[0], "%.*s", (int)ro.seats[i].id_len, (const char *)ro.seats[i].id);
    T_EQ_INT(CB_ROWS_CALL(b, game_import, &dealt, humans, g_bots ? 1 : 2, bots, n_bots), CB_OK);
    close_world(b);
    cb_game_row_free(&dealt);
    const int64_t t0 = g_now;
    move_t ms[40];
    uint64_t want[40];
    int n = 0;
    a = open_world(pa, false, err, sizeof err);
    for (; n < 40 && step(a, gid, &ms[n]) > 0; n++) want[n] = row_hash(a, gid);
    close_world(a);
    g_now = t0;
    b = open_world(pb, true, err, sizeof err);
    T_CHECK(b != NULL);
    if (!b) { memset(&g_jo, 0, sizeof g_jo); return; }
    int same = 0;
    for (int i = 0; i < n; i++) {
        T_CHECK(play(gid, &ms[i]) > 0);
        same += row_hash(b, gid) == want[i];
    }
    T_EQ_INT(same, n);
    cb_journal_stats js;
    for (int i = 0; i < 300; i++) {   // the kicked flush thread ran: the condition, however long the way
        cb_journal_stats_get(b, &js);
        if (js.flushes > 0) break;
        usleep(10000);
    }
    T_CHECK(js.flushes > 0);
    T_EQ_INT(cb_journal_flush(b), CB_OK);
    close_world(b);
    uint64_t ga, ma, gb, mb;
    int na, nb;
    sql_dump(pa, gid, &ga, &ma, &na);
    sql_dump(pb, gid, &gb, &mb, &nb);
    T_CHECK(gb == ga);
    T_CHECK(mb == ma);
    T_EQ_INT(nb, na);
    T_CHECK(n > 10);
    memset(&g_jo, 0, sizeof g_jo);
}




// The seats' ratings as the tables hold them (no journal in between), by the game's roster.
static int table_ratings(cb_store *s, const char *gid, int32_t out[CB_MAX_SEATS]) {
    cb_game_row row;
    if (cb_rows_sql.game_get(s, gid, &row) != CB_OK) return -1;
    Roster ro;
    int n = -1;
    if (roster_decode(&ro, row.roster, (int)row.roster_len) == ROSTER_OK) {
        cb_uuid ids[CB_MAX_SEATS];
        const char *p[CB_MAX_SEATS];
        for (int i = 0; i < ro.n; i++) {
            snprintf(ids[i], sizeof ids[i], "%.*s", (int)ro.seats[i].id_len, (const char *)ro.seats[i].id);
            p[i] = ids[i];
        }
        n = cb_rows_sql.ratings_for(s, p, ro.n, out) == CB_OK ? ro.n : -1;
    }
    cb_game_row_free(&row);
    return n;
}

// A RATED END IS A JOURNAL RECORD (journal.h): the game's ending move commits with the tables
// write-locked by another connection (a write around the journal would wait 5 s and fail), so
// nothing of it reaches SQLite before its answer; the process dies at once, and the recovery
// writes the end's rating rows from the record: the tables then hold the same ratings and the
// same game as the same moves committed straight to the tables.
static void a_rated_end_is_a_journal_record(void) {
    char pa[320], pb[320], err[256] = "";
    snprintf(pa, sizeof pa, "%s/rated_plain%d.sqlite", g_dir, g_bots);
    snprintf(pb, sizeof pb, "%s/rated_journaled%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *a = open_world(pa, false, err, sizeof err);
    T_CHECK(a != NULL);
    if (!a) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_game_row dealt = stored(a, gid);
    close_world(a);
    cb_store *b = open_world(pb, true, err, sizeof err);
    T_CHECK(b != NULL);
    if (!b) { cb_game_row_free(&dealt); return; }
    cb_uuid humans[2], bots[CB_MAX_SEATS];
    snprintf(humans[0], sizeof humans[0], "%s", UID_A);
    snprintf(humans[1], sizeof humans[1], "%s", UID_B);
    int n_bots = 0;
    Roster ro;
    T_EQ_INT(roster_decode(&ro, dealt.roster, (int)dealt.roster_len), ROSTER_OK);
    for (int i = 0; i < ro.n; i++)
        if (ro.seats[i].brain_len) snprintf(bots[n_bots++], sizeof bots[0], "%.*s", (int)ro.seats[i].id_len, (const char *)ro.seats[i].id);
    T_EQ_INT(CB_ROWS_CALL(b, game_import, &dealt, humans, g_bots ? 1 : 2, bots, n_bots), CB_OK);
    close_world(b);
    cb_game_row_free(&dealt);
    // The game played to its end on the tables alone: the reference.
    const int64_t t0 = g_now;
    const uint64_t rng0 = g_rng;
    static move_t ms[3000];
    int n = 0;
    a = open_world(pa, false, err, sizeof err);
    int32_t r0[CB_MAX_SEATS], want[CB_MAX_SEATS];
    const int seats = table_ratings(a, gid, r0);
    // Every step until the game is over (a bot's drive may be the one that ends it: its step
    // answers no version, so the loop asks the game).
    while (n < 3000 && version_of(a, gid) > 0) (void)step(a, gid, &ms[n++]);
    T_EQ_INT(version_of(a, gid) < 0, 1);          // over
    T_EQ_INT(table_ratings(a, gid, want), seats);
    uint64_t want_hash = row_hash(a, gid);
    close_world(a);
    T_CHECK(seats == 2 && n > 4);
    bool moved = false;
    for (int i = 0; i < seats; i++) moved |= want[i] != r0[i];
    T_CHECK(moved);   // the reference end did rate
    // The same moves, journaled, in a child that dies the moment the end is answered.
    g_now = t0;
    int p[2];
    T_EQ_INT(pipe(p), 0);
    const pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        cb_store *c = open_world(pb, true, err, sizeof err);
        if (!c) _exit(3);
        int k = 0;
        for (; k < n - 1; k++) (void)play(gid, &ms[k]);
        if (version_of(c, gid) <= 0) _exit(4);
        sqlite3 *db = NULL;
        if (sqlite3_open(pb, &db) != SQLITE_OK || sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) _exit(5);
        (void)play(gid, &ms[k]);   // the end: applied with the tables locked
        const int64_t v = -version_of(c, gid);
        int32_t held[CB_MAX_SEATS] = { 0 };
        bool old = false;
        const char *ids[2] = { UID_A, g_bots ? bots[0] : UID_B };
        const int rc = cb_journal_seat_ratings(c, ids, 2, g_now, 0, held, &old);
        const int64_t ok = v > 0 && rc == CB_OK ? 1 : 0;
        if (write(p[1], &ok, sizeof ok) != sizeof ok || write(p[1], held, sizeof held) != sizeof held) _exit(6);
        raise(SIGKILL);
        _exit(7);
    }
    close(p[1]);
    int64_t ok = 0;
    int32_t held[CB_MAX_SEATS] = { 0 };
    T_CHECK(read(p[0], &ok, sizeof ok) == sizeof ok && read(p[0], held, sizeof held) == sizeof held);
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    T_CHECK(WIFSIGNALED(st));
    T_EQ_INT(ok, 1);
    // The ratings the next end reads were the end's at once (the seats in roster order: A first).
    for (int i = 0; i < seats; i++) T_EQ_INT(held[i], want[i]);
    // Recovery writes the end, its ratings with it.
    b = open_world(pb, true, err, sizeof err);
    T_CHECK(b != NULL);
    if (!b) { fprintf(stderr, "reopen: %s\n", err); return; }
    cb_journal_stats js;
    cb_journal_stats_get(b, &js);
    T_CHECK(js.recovered >= 1);
    close_world(b);
    b = open_world(pb, false, err, sizeof err);
    T_CHECK(b != NULL);
    if (!b) return;
    int32_t got[CB_MAX_SEATS];
    T_EQ_INT(table_ratings(b, gid, got), seats);
    for (int i = 0; i < seats; i++) T_EQ_INT(got[i], want[i]);
    T_CHECK(row_hash(b, gid) == want_hash);
    close_world(b);
    g_rng = rng0;
    memset(&g_jo, 0, sizeof g_jo);
}

// ---- a bot cycle on a legacy-deck board, and a cycle the drive would not repeat --------------

// The crash's files as they are now (the database copied, every segment beside the copy).
static void crash_copy(const char *path, const char *copy) {
    T_EQ_INT(cb_backup_copy(path, copy), CB_OK);
    const char *base = strrchr(path, '/') + 1, *cbase = strrchr(copy, '/') + 1;
    char pre[300];
    snprintf(pre, sizeof pre, "%s-journal.", base);
    DIR *d = opendir(g_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, pre, strlen(pre)) == 0) {
            char cmd[1600];
            snprintf(cmd, sizeof cmd, "cp '%s/%s' '%s/%s%s'", g_dir, e->d_name, g_dir, cbase, e->d_name + strlen(base));
            T_EQ_INT(system(cmd), 0);
        }
    if (d) closedir(d);
}

// Rewrites the copy's one segment with its first bot cycle of two or more moves cut by its
// last move. 1 when one was cut.
static int cut_a_cycle(const char *copy) {
    const char *cbase = strrchr(copy, '/') + 1;
    char pre[300], seg[700] = "";
    snprintf(pre, sizeof pre, "%s-journal.", cbase);
    DIR *d = opendir(g_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, pre, strlen(pre)) == 0) {   // the one written to (not the one made ahead)
            char p[700];
            snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
            FILE *x = fopen(p, "rb");
            if (x && fgetc(x) == 'F') snprintf(seg, sizeof seg, "%s", p);
            if (x) fclose(x);
        }
    if (d) closedir(d);
    FILE *f = fopen(seg, "rb");
    if (!f) return 0;
    static uint8_t buf[1 << 20], out[1 << 20];
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int64_t start = 0;
    for (int i = 7; i >= 0; i--) start = start << 8 | buf[8 + i];
    // Frame by frame: a whole commit's record (journal.h, 'R') copied as it is, each batch decoded
    // and coded again, the first bot cycle of two moves or more cut by one.
    cb_jmove *ms = calloc(64, sizeof *ms);
    EvwMove (*mv)[EVW_DELTA_MAX_MOVES] = calloc(64, sizeof *mv);
    const cb_jmove *ptr[64];
    for (int i = 0; i < 64; i++) ptr[i] = &ms[i];
    int cut = 0;
    cb_jslots *t = cb_jslots_new(), *t2 = cb_jslots_new();
    memcpy(out, buf, 16);
    size_t w = 16;
    for (size_t at = 16, used = 0; at < len && buf[at]; at += used) {
        if (buf[at] == 'R') {
            used = 12 + ((size_t)buf[at + 8] | (size_t)buf[at + 9] << 8 | (size_t)buf[at + 10] << 16 | (size_t)buf[at + 11] << 24);
            memcpy(out + w, buf + at, used);
            w += used;
            continue;
        }
        int k = cb_journal_batch_decode(t, start, buf + at, len - at, &used, ms, mv, 64);
        if (k < 0) break;
        for (int i = 0; i < k && !cut; i++)
            if (ms[i].bots && ms[i].n_mv >= 2) { ms[i].n_mv--; cut = 1; }
        int enc = cb_journal_batch_encode(t2, start, ptr, k, out + w, (int)(sizeof out - w));
        T_CHECK(enc > 0);
        if (enc > 0) w += (size_t)enc;
    }
    cb_jslots_free(t);
    cb_jslots_free(t2);
    free(ms);
    free(mv);
    f = fopen(seg, "wb");
    if (f) { fwrite(out, 1, w, f); fclose(f); }
    return cut;
}

static void a_legacy_deck_cycle_recovers_and_a_cut_one_is_refused(void) {
    char path[320], copy[320], cut[320], err[256] = "";
    snprintf(path, sizeof path, "%s/legacy.sqlite", g_dir);
    snprintf(copy, sizeof copy, "%s/legacy_copy.sqlite", g_dir);
    snprintf(cut, sizeof cut, "%s/legacy_cut.sqlite", g_dir);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    // A, two bots: a bot's cycle can end a bout (a draw) and the other bot act after it.
    reply r;
    char gid[CB_GAME_ID_MAX + 1] = "";
    call("create", "A", NULL, 0, &r);
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    meta("A", ASK_ADD_BOT, gid, &r);
    meta("A", ASK_ADD_BOT, gid, &r);
    meta("A", ASK_START, gid, &r);
    // A board from before the seed-dealt deck: a draw takes the draw stream's next number. The
    // row is rewritten the one way a write may go behind the journal: an admin's (THE ADMIN RULE,
    // which flushes and forgets the overlay, so the next move reads this row).
    cb_game_row row = stored(s, gid);
    T_CHECK(row.state_len > 2 && row.status == GAME_STATUS_PLAYING);
    const size_t cap = 2 * row.state_len + 128;
    char *sql = malloc(cap);
    int at = snprintf(sql, cap, "UPDATE games SET state = X'");
    for (size_t i = 0; i < row.state_len; i++)
        at += snprintf(sql + at, cap - (size_t)at, "%02x", i == 1 ? row.state[i] & (uint8_t)~0x01 : row.state[i]);   // STATE_BLOB_FLAG_DETERMINISTIC
    snprintf(sql + at, cap - (size_t)at, "' WHERE id = '%s'", gid);
    T_CHECK(cb_store_admin_query(s, sql, true, no_row, NULL) >= 0);
    free(sql);
    cb_game_row_free(&row);
    cb_journal_stats js0;
    cb_journal_stats_get(s, &js0);
    const long flushes0 = js0.flushes;   // the admin's write flushed the lobby's commits
    const bool was = g_bots;
    g_bots = true;
    move_t m;
    int steps = 0;
    for (; steps < 20 && step(s, gid, &m) > 0; steps++) {}
    g_bots = was;
    T_CHECK(steps > 10);
    const uint64_t h = row_hash(s, gid);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_CHECK(js.waiting > 0 && js.flushes == flushes0);   // the moves are in the journal alone
    crash_copy(path, copy);
    crash_copy(path, cut);
    close_world(s);
    // The crash's files recover the game the live process held.
    s = open_world(copy, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (s) {
        T_CHECK(row_hash(s, gid) == h);
        close_world(s);
    }
    // A cycle record one move short: the drive, handed its moves, goes on to the next, and
    // the start refuses the record rather than write a game the journal did not hold.
    T_EQ_INT(cut_a_cycle(cut), 1);
    s = open_world(cut, true, err, sizeof err);
    T_CHECK(s == NULL);
    if (s) close_world(s);
}


// ---- mapped segments: a crash keeps every committed batch across rotations, and a batch
// whose commit marker (its header's magic, stored last) never landed is not there ----------

// The newest segment of `db` that holds a batch, and the offset of its last batch.
// The newest segment's last frame of kind `magic` ('J' a batch, 'R' a whole commit), walking
// every frame (a batch: 8 + its u16 length; a whole commit: 12 + its u32 length).
static bool last_frame(const char *db, int magic, char *seg, size_t cap, long *off) {
    const char *base = strrchr(db, '/') + 1;
    char pre[300], best[400] = "";
    snprintf(pre, sizeof pre, "%s-journal.", base);
    DIR *d = opendir(g_dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, pre, strlen(pre)) == 0 && strcmp(e->d_name, best) > 0) {
            char p[700];
            snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
            FILE *x = fopen(p, "rb");
            if (x && fgetc(x) == 'F') snprintf(best, sizeof best, "%s", e->d_name);
            if (x) fclose(x);
        }
    if (d) closedir(d);
    if (!best[0]) return false;
    snprintf(seg, cap, "%s/%s", g_dir, best);
    FILE *f = fopen(seg, "rb");
    uint8_t h[12];
    long at = 16, last = -1;
    while (f && fseek(f, at, SEEK_SET) == 0 && fread(h, 1, 12, f) >= 8 && (h[0] == 'J' || h[0] == 'R')) {
        if (h[0] == magic) last = at;
        at += h[0] == 'J' ? 8 + (long)(h[2] | h[3] << 8) : 12 + (long)((uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24);
    }
    if (f) fclose(f);
    *off = last;
    return last > 0;
}

static void poke(const char *path, long off, uint8_t v) {
    FILE *f = fopen(path, "r+b");
    if (f) { fseek(f, off, SEEK_SET); fputc(v, f); fclose(f); }
}

// The byte at off inverted: a byte that always changes (a poke of a constant left the byte as it
// was whenever it already held that constant, 1 in 256 of the runs, and the "torn" record read whole).
static void flip(const char *path, long off) {
    FILE *f = fopen(path, "r+b");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    const int c = fgetc(f);
    T_CHECK(c != EOF);
    fseek(f, off, SEEK_SET);
    fputc((uint8_t)~c, f);
    fclose(f);
}

static void mapped_segments_keep_every_committed_batch(void) {
    char path[320], a[320], b[320], c[320], err[256] = "", seg[700];
    snprintf(path, sizeof path, "%s/mapped%d.sqlite", g_dir, g_bots);
    snprintf(a, sizeof a, "%s/mapped%d_a.sqlite", g_dir, g_bots);
    snprintf(b, sizeof b, "%s/mapped%d_b.sqlite", g_dir, g_bots);
    snprintf(c, sizeof c, "%s/mapped%d_c.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.seg_bytes = 1;   // a full segment at every batch: each rotates
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    move_t m;
    int64_t v[32];
    uint64_t h[32];
    int n = 0;
    // Twelve steps at least, the last one a single commit: a bot drive may commit more than
    // one cycle in a step, and the cut below takes off only the last batch (one record). The
    // game still playing, and a game that ends that soon dealt again. The games end unrated: a
    // rated end makes its game due at once, and the flush thread would write and trim while the
    // copies below are made.
    g_cfg.rated_min_age_s = 1 << 30;
    bool single = false;
    for (int game = 0; game < 4 && !(n >= 12 && single); game++) {
        two_humans_dealt(gid);
        n = 0;
        single = false;
        for (; n < 32 && !(n >= 12 && single) && (v[n] = step(s, gid, &m)) > 0 && version_of(s, gid) > 0; n++) {
            h[n] = row_hash(s, gid);
            single = n > 0 && v[n] == v[n - 1] + 1;
        }
        // A step that ended the game wrote its end last (a whole record, past the batch cut
        // below): that game is dealt again too.
        if (version_of(s, gid) <= 0) single = false;
    }
    T_CHECK(n >= 12 && single);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_CHECK(js.segments >= 12);
    T_EQ_INT(js.flushes, 0);   // nothing was due: nothing moved under the copies
    crash_copy(path, a);
    crash_copy(path, b);
    crash_copy(path, c);
    close_world(s);
    memset(&g_jo, 0, sizeof g_jo);
    // Every batch, across every rotation.
    s = open_world(a, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (s) { T_EQ_INT(version_of(s, gid), v[n - 1]); T_CHECK(row_hash(s, gid) == h[n - 1]); close_world(s); }
    // The last batch's payload and header written, its magic not: the move before it.
    long off = 0;
    T_CHECK(last_batch(b, seg, sizeof seg, &off));
    poke(seg, off, 0);
    s = open_world(b, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (s) { T_EQ_INT(version_of(s, gid), v[n - 2]); T_CHECK(row_hash(s, gid) == h[n - 2]); close_world(s); }
    // A torn header (its checksum not the payload's): the same.
    T_CHECK(last_batch(c, seg, sizeof seg, &off));
    flip(seg, off + 4);
    s = open_world(c, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (s) { T_EQ_INT(version_of(s, gid), v[n - 2]); close_world(s); }
}

// ---- THE ADMIN RULE: an admin's write is never written over by what the journal held ---------

static bool no_row(int n, const char *const *names, const char *const *values, void *ud) { return true; }

// A commit whose base was read before the admin's write, waiting for the writer when it runs.
typedef struct {
    cb_store   *s;
    cb_game_row row;     // the row as read before, at version + 1
    int64_t     base;
    int         rc;
    pthread_t   th;
} racer_t;

static void *racer(void *ud) {
    racer_t *r = ud;
    EvwMove mv;
    memset(&mv, 0, sizeof mv);
    mv.wire[0] = AWIRE_PICKUP;   // a whole frame: kind, no cards
    mv.wire_len = 2;
    cb_game_commit c;
    memset(&c, 0, sizeof c);
    c.row = &r->row;
    c.expected_version = r->base;
    c.moves = &mv;
    c.n_moves = 1;
    r->rc = CB_ROWS_CALL(r->s, game_commit, &c);
    return NULL;
}

typedef struct {
    cb_store   *s;
    const char *sql;
    racer_t    *race;
} admin_t;

static int admin_body(void *ud) {
    admin_t *a = ud;
    if (a->race) {
        pthread_create(&a->race->th, NULL, racer, a->race);
        usleep(100000);   // it read its base and waits for the writer the section holds
    }
    return CB_STORE_CALL(a->s, admin_query, a->sql, true, no_row, NULL);
}

static void an_admin_write_is_never_written_over(void) {
    char path[320], err[256] = "", sql[200];
    snprintf(path, sizeof path, "%s/admin%d.sqlite", g_dir, g_bots);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    move_t m;
    int64_t v = -1;
    for (int i = 0; i < 5 && (v = step(s, gid, &m)) > 0; i++) {}
    T_CHECK(v > 0);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_CHECK(js.waiting > 0);
    // The console's write: the waiting moves go first, and the store reads the admin's row after.
    snprintf(sql, sizeof sql, "UPDATE games SET created_at = 4242 WHERE id = '%s'", gid);
    T_CHECK(cb_store_admin_query(s, sql, true, no_row, NULL) >= 0);
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, 0);
    cb_game_row row = stored(s, gid);
    T_EQ_INT(row.created_at, 4242);
    T_EQ_INT(row.version, v);
    cb_game_row_free(&row);
    // The next move builds on the admin's row, not on a copy the app held from before it.
    T_CHECK((v = step(s, gid, &m)) > 0);
    row = stored(s, gid);
    T_EQ_INT(row.created_at, 4242);
    cb_game_row_free(&row);
    // A move read before an admin's write and acknowledged after it would carry the old row:
    // it is refused (the client asks again), and the moves after build on the admin's row.
    racer_t race;
    memset(&race, 0, sizeof race);
    race.s = s;
    race.row = stored(s, gid);
    race.base = race.row.version++;
    race.row.created_at = 1111;
    snprintf(sql, sizeof sql, "UPDATE games SET created_at = 4343 WHERE id = '%s'", gid);
    admin_t a = { s, sql, &race };
    T_CHECK(s->app_exclusive(s, admin_body, &a) >= 0);
    pthread_join(race.th, NULL);
    T_EQ_INT(race.rc, CB_E_CONFLICT);
    cb_game_row_free(&race.row);
    row = stored(s, gid);
    T_EQ_INT(row.created_at, 4343);
    cb_game_row_free(&row);
    for (int i = 0; i < 3 && step(s, gid, &m) > 0; i++) {}
    close_world(s);
    // The tables alone: the admin's last value, under every move since.
    s = open_world(path, false, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    row = stored(s, gid);
    T_EQ_INT(row.created_at, 4343);
    T_CHECK(row.version > v);
    cb_game_row_free(&row);
    close_world(s);
}

// ---- one owner: the overlay is the only copy of a game in the process --------------------------

static long table_reads(cb_store *s) {
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    return js.table_reads;
}

static int ask_status(const reply *r) {   // 200: the ask committed or answered
    return r->status;
}

// THE GAME IS RESIDENT FROM ITS CREATE ON (a join, the deal, its moves, its bots' cycles):
// every operation reads it, and its session log, from the overlay, never from the tables.
static void a_deal_and_its_moves_read_no_table(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/resident%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    reply r;
    call("create", "A", NULL, 0, &r);
    char gid[CB_GAME_ID_MAX + 1] = "";
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    const long reads0 = table_reads(s);
    if (g_bots) meta("A", ASK_ADD_BOT, gid, &r);
    else meta("B", ASK_JOIN, gid, &r);
    T_EQ_INT(table_reads(s) - reads0, 0);   // resident from its create on
    if (g_bots) meta("A", ASK_START, gid, &r);
    else { meta("A", ASK_START, gid, &r); meta("B", ASK_START, gid, &r); }
    T_EQ_INT(ask_status(&r), 200);
    T_EQ_INT(version_of(s, gid) > 0, 1);   // dealt and playing
    cb_journal_stats js0;
    cb_journal_stats_get(s, &js0);
    const long loads0 = cb_section_loads();
    move_t m;
    int moves = 0, drives = 0;
    for (; moves < 12 && step(s, gid, &m) > 0; moves++) drives += m.n == 0;
    T_EQ_INT(moves, 12);
    T_EQ_INT(table_reads(s) - reads0, 0);
    T_EQ_INT(cb_section_loads() - loads0, 0);   // every move and every cycle adopted the resident: no decode
    // A bot cycle adopts the resident and reads its session log from the overlay: no table.
    cb_journal_stats js1;
    cb_journal_stats_get(s, &js1);
    T_EQ_INT(js1.log_reads - js0.log_reads, 0);
    if (g_bots) T_CHECK(drives > 0);
    // A view on another connection reads the overlay too: the newest version.
    cb_view_row v;
    T_EQ_INT(cb_table_view(&g_ctx, gid, UID_A, &v), CB_OK);
    T_EQ_INT(v.version, version_of(s, gid));
    cb_view_row_free(&v);
    T_EQ_INT(table_reads(s) - reads0, 0);
    close_world(s);
}

// R1 AND R3 THROUGH THE SERVER: a refused move (REJECTED, a stale round) leaves the game exactly
// as it was, the row read after it is byte for byte the row read before; a committed move is
// seen by the next read on any path, its bytes the new board's (the resident's encode, made
// when read), its version the new one.
static void a_refusal_changes_nothing_and_a_move_is_read_at_once(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/refusal%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    move_t m;
    int refusals = 0, moves = 0;
    // The deal is the crypto's: a game may end before four refusals were tried, so games are
    // dealt until both counts are reached.
    for (int game = 0; game < 6 && (refusals <= 3 || moves <= 3); game++) {
        cb_limits_reset();
        g_now += 60000;
        two_humans_dealt(gid);
        for (int i = 0; i < 40; i++) {
            cb_game_row before = stored(s, gid);
            if (before.status != GAME_STATUS_PLAYING) { cb_game_row_free(&before); break; }
            // A pickup by whoever is not defending, at the current round: refused by the kernel.
            reply r;
            const uint8_t pickup[2] = { AWIRE_PICKUP, 0 };
            action(i % 2 ? "B" : "A", gid, pickup, 2, (uint32_t)before.version, &r);
            TableResponse resp;
            if (r.status == 200 && table_response_decode(r.body, (int)r.len, &resp) == 0 && resp.status != TABLE_STATUS_APPLIED) {
                refusals++;
                cb_game_row after = stored(s, gid);
                T_EQ_INT(after.version, before.version);
                T_CHECK(after.state_len == before.state_len && memcmp(after.state, before.state, before.state_len) == 0);
                T_CHECK(after.roster_len == before.roster_len && memcmp(after.roster, before.roster, before.roster_len) == 0);
                cb_game_row_free(&after);
            }
            int64_t v = step(s, gid, &m);
            if (v > 0 && m.n > 0) {
                moves++;
                cb_game_row after = stored(s, gid);
                T_EQ_INT(after.version, v);
                T_CHECK(after.state_len != before.state_len || memcmp(after.state, before.state, before.state_len) != 0);
                cb_game_row_free(&after);
            }
            cb_game_row_free(&before);
            if (v < 0) break;
        }
    }
    T_CHECK(refusals > 3);
    T_CHECK(moves > 3);
    // R4: a flush with no read since the last move writes the resident's encode, not the bytes
    // the last read left in the row's buffers.
    if (next_move(s, gid, &m) && play(gid, &m) > 0) {
        T_EQ_INT(cb_journal_flush(s), CB_OK);
        cb_game_row tables, held = stored(s, gid);
        memset(&tables, 0, sizeof tables);
        T_EQ_INT(cb_rows_sql.game_get(s, gid, &tables), CB_OK);
        T_EQ_INT(tables.version, held.version);
        T_CHECK(tables.state_len == held.state_len && memcmp(tables.state, held.state, held.state_len) == 0);
        cb_game_row_free(&tables);
        cb_game_row_free(&held);
    }
    close_world(s);
}

// A RESIDENT THE TABLES CONTRADICT IS DROPPED, NEVER BUILT ON: a write the journal did not
// see (here a raw UPDATE; in production nothing but the admin's, which forgets the overlay
// itself) makes the next write around the journal (a delete: the last seat leaving) lose its
// fence, and the game leaves the overlay, so the operation after it reads the tables' row
// and lands.
static void a_resident_the_tables_contradict_is_dropped(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/contradict.sqlite", g_dir);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    reply r;
    call("create", "A", NULL, 0, &r);
    char gid[CB_GAME_ID_MAX + 1] = "";
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    meta("A", ASK_SET_RULES, gid, &r);   // resident, its commit in the journal (the table goes podkidnoy)
    T_EQ_INT(ask_status(&r), 200);
    T_EQ_INT(cb_journal_flush(s), CB_OK);   // and in the tables
    sqlite3 *db = NULL;
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    char sql[160];
    snprintf(sql, sizeof sql, "UPDATE games SET version = version + 1 WHERE id = '%s'", gid);
    T_EQ_INT(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    const long reads0 = table_reads(s);
    meta("A", ASK_EXIT, gid, &r);   // the last seat: the delete is fenced on the resident's version, which the tables refuse
    T_CHECK(ask_status(&r) != 200);
    meta("A", ASK_EXIT, gid, &r);   // read from the tables, and deleted there
    T_EQ_INT(ask_status(&r), 200);
    T_EQ_INT(table_reads(s) - reads0, 1);
    T_CHECK(game_gone(s, gid));
    close_world(s);
}

static int no_game(void *ud, const cb_game_row *row, const struct TableResident *res) { return CB_OK; }

static bool count_name(const char *name, void *ud) {
    (void)name;
    (*(int *)ud)++;
    return true;
}

// EVERY COMMIT BUT A RATING THROUGH THE JOURNAL: the join, the readies and the deal write no
// table (each is a whole record in the segment), membership reads the overlay, and a crash
// after the deal's record or after the moves on it recovers the game, its seats into the tables.
static void a_lobby_and_its_deal_write_no_table(void) {
    char path[320], after_deal[320], after_moves[320], err[256] = "";
    snprintf(path, sizeof path, "%s/lobby%d.sqlite", g_dir, g_bots);
    snprintf(after_deal, sizeof after_deal, "%s/lobby%d_deal.sqlite", g_dir, g_bots);
    snprintf(after_moves, sizeof after_moves, "%s/lobby%d_moves.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    reply r;
    call("create", "A", NULL, 0, &r);
    char gid[CB_GAME_ID_MAX + 1] = "";
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    const unsigned long wal0 = cb_store_local_wal_frames(s);
    if (g_bots) {
        meta("A", ASK_ADD_BOT, gid, &r);
        meta("A", ASK_START, gid, &r);
    } else {
        meta("B", ASK_JOIN, gid, &r);
        T_EQ_INT(CB_ROWS_CALL(s, is_member, gid, UID_B), 1);   // seated before the tables hear of it
        T_EQ_INT(CB_ROWS_CALL(s, is_member, gid, "cccccccc-0000-4000-8000-000000000003"), 0);
        int members = 0;
        T_EQ_INT(CB_ROWS_CALL(s, game_members, gid, count_name, &members), 2);
        T_EQ_INT(members, 2);
        meta("A", ASK_START, gid, &r);
        meta("B", ASK_START, gid, &r);
    }
    T_EQ_INT(ask_status(&r), 200);
    T_CHECK(version_of(s, gid) > 0);   // dealt
    T_EQ_INT((long)cb_store_local_wal_frames(s), (long)wal0);
    const uint64_t h_deal = row_hash(s, gid);
    crash_copy(path, after_deal);
    move_t m;
    int moves = 0;
    for (; moves < 8 && step(s, gid, &m) > 0; moves++) {}
    T_EQ_INT(moves, 8);
    T_EQ_INT((long)cb_store_local_wal_frames(s), (long)wal0);
    const uint64_t h_moves = row_hash(s, gid);
    crash_copy(path, after_moves);
    close_world(s);
    const char *copies[2] = { after_deal, after_moves };
    const uint64_t want[2] = { h_deal, h_moves };
    for (int i = 0; i < 2; i++) {
        s = open_world(copies[i], true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) continue;
        T_CHECK(row_hash(s, gid) == want[i]);
        // Recovered into the tables, seats and all; the overlay holds nothing after a start, so the
        // store's membership reads are the tables'.
        T_EQ_INT(cb_rows_sql.is_member(s, gid, UID_A), 1);
        if (!g_bots) T_EQ_INT(cb_rows_sql.is_member(s, gid, UID_B), 1);
        T_EQ_INT(CB_ROWS_CALL(s, is_member, gid, UID_A), 1);
        int members = 0;
        T_EQ_INT(CB_ROWS_CALL(s, game_members, gid, count_name, &members), g_bots ? 1 : 2);
        // An import writes the tables behind the overlay: the resident copy goes with it, and the
        // next read is the imported row.
        T_EQ_INT(cb_journal_game_acquire(s, gid, no_game, NULL), CB_OK);   // resident now
        cb_game_row row = stored(s, gid);
        T_CHECK(row.version > 0);
        row.created_at = 777;
        cb_uuid humans[1];
        snprintf(humans[0], sizeof humans[0], "%s", UID_A);
        T_EQ_INT(CB_ROWS_CALL(s, game_import, &row, (const cb_uuid *)humans, 1, NULL, 0), CB_OK);
        cb_game_row_free(&row);
        row = stored(s, gid);
        T_EQ_INT(row.created_at, 777);
        cb_game_row_free(&row);
        close_world(s);
    }
}

// A WHOLE COMMIT THAT IS NOT WHOLE IS DROPPED: a crash in the middle of a full record's write
// (its payload cut short) or a record that does not check (a flipped byte) was never
// acknowledged: recovery stops before it, at the game the record before it left.
static void a_torn_or_corrupt_whole_commit_is_dropped(void) {
    for (int kind = 0; kind < 2; kind++) {
        char path[320], copy[320], err[256] = "";
        snprintf(path, sizeof path, "%s/tornfull%d.sqlite", g_dir, kind);
        snprintf(copy, sizeof copy, "%s/tornfull%d_crash.sqlite", g_dir, kind);
        memset(&g_jo, 0, sizeof g_jo);
        cb_store *s = open_world(path, true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) return;
        reply r;
        call("create", "A", NULL, 0, &r);
        char gid[CB_GAME_ID_MAX + 1] = "";
        cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
        meta("B", ASK_JOIN, gid, &r);
        const uint64_t h_join = row_hash(s, gid);
        const int64_t v_join = -version_of(s, gid);
        meta("A", ASK_START, gid, &r);   // the last whole commit in the segment
        T_EQ_INT(-version_of(s, gid), v_join + 1);
        crash_copy(path, copy);
        close_world(s);
        char seg[700];
        long off = 0;
        T_CHECK(last_frame(copy, 'R', seg, sizeof seg, &off));
        if (kind == 0) {   // the payload's last byte: the checksum refuses it
            FILE *f = fopen(seg, "rb");
            uint8_t h[128] = { 0 };
            if (f) { fseek(f, off, SEEK_SET); T_CHECK(fread(h, 1, sizeof h, f) == sizeof h); fclose(f); }
            // journal.h A FULL RECORD: the id, two i64, the flags, the status, three i64, the seed
            // when flagged, then the state blob's u32 length: a byte of the board.
            const long flags_at = 12 + 1 + h[12] + 16;
            const long state_at = flags_at + 2 + 24 + ((h[flags_at] & 2) ? 32 : 0) + 4;
            flip(seg, off + state_at + 3);
        }
        else poke(seg, off + 11, 0x40);                 // the length past the file's end: torn
        s = open_world(copy, true, err, sizeof err);
        T_CHECK(s != NULL);
        if (!s) continue;
        T_EQ_INT(-version_of(s, gid), v_join);
        T_CHECK(row_hash(s, gid) == h_join);
        close_world(s);
    }
}

// WHOLE COMMITS AS RECOVERY READS THEM, made by hand: a record whose fields do not add up is
// dropped with what follows it (it was never acknowledged), one for a game that is gone or
// that the tables already hold past it is skipped, and one that continues from a version the
// tables have not reached refuses the start.
static void le(uint8_t *p, uint64_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

// One 'R' frame at out: its length. idl < 0: the id's real length; tail: extra payload bytes.
static size_t full_frame(uint8_t *out, const char *id, int idl, int64_t base, uint8_t flags, uint32_t state_len,
                         uint32_t state_claim, int tail) {
    uint8_t *q = out + 12;
    const size_t n = strlen(id);
    *q++ = (uint8_t)(idl < 0 ? (int)n : idl);
    memcpy(q, id, n), q += n;
    le(q, (uint64_t)base, 8), q += 8;
    le(q, 1760000000000ULL, 8), q += 8;
    *q++ = flags;
    *q++ = 0;   // status: a lobby
    for (int i = 0; i < 3; i++) le(q, 0, 8), q += 8;
    if (flags & 2) memset(q, 7, 32), q += 32;
    le(q, state_claim, 4), q += 4;
    memset(q, 0, state_len), q += state_len;
    le(q, 0, 4), q += 4;   // roster
    le(q, 0, 4), q += 4;   // log
    memset(q, 0, (size_t)tail), q += tail;
    const uint32_t plen = (uint32_t)(q - out - 12);
    out[0] = 'R', out[1] = JOURNAL_FORMAT, out[2] = out[3] = 0;
    le(out + 8, plen, 4);
    le(out + 4, crc32c(crc32c(0, out + 8, 4), out + 12, plen), 4);
    return 12 + plen;
}

static void write_segment_as(const char *db, const uint8_t *frames, size_t n, uint32_t format) {
    char seg[600];
    snprintf(seg, sizeof seg, "%s-journal.%016llu", db, 1000ULL);
    FILE *f = fopen(seg, "wb");
    uint8_t h[16] = { 'F', 'J', 'N', 'L' };
    le(h + 4, format, 4);
    le(h + 8, 1760000000000ULL, 8);
    if (f) { fwrite(h, 1, 16, f); fwrite(frames, 1, n, f); fclose(f); }
}
static void write_segment(const char *db, const uint8_t *frames, size_t n) { write_segment_as(db, frames, n, JOURNAL_FORMAT); }

static void crafted_whole_commits_recover_or_refuse(void) {
    char path[320], err[256] = "";
    // A game the tables hold at version 1 (created, one lobby edit), no journal.
    snprintf(path, sizeof path, "%s/crafted.sqlite", g_dir);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, false, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    reply r;
    call("create", "A", NULL, 0, &r);
    char gid[CB_GAME_ID_MAX + 1] = "";
    cb_table_views_of_user(&g_ctx, UID_A, 1, first_game, gid);
    meta("B", ASK_JOIN, gid, &r);
    T_EQ_INT(-version_of(s, gid), 1);
    close_world(s);
    // The journal's own checks of its arguments.
    s = open_world(path, false, err, sizeof err);
    cb_journal_opts bad = { 0 };
    T_EQ_INT(cb_journal_open(NULL, &bad, err, sizeof err), CB_E_INVALID);
    T_EQ_INT(cb_journal_open(s, NULL, err, sizeof err), CB_E_INVALID);
    T_EQ_INT(cb_journal_open(s, &bad, err, sizeof err), CB_E_INVALID);   // no path
    bad.path = "";
    T_EQ_INT(cb_journal_open(s, &bad, err, sizeof err), CB_E_INVALID);
    bad.path = path;
    T_EQ_INT(cb_journal_open(s, &bad, err, sizeof err), CB_E_INVALID);   // no apply
    bad.apply = cb_table_replay_move;
    bad.apply_ud = &g_ctx;
    T_EQ_INT(cb_journal_open(s, &bad, err, sizeof err), CB_OK);
    T_EQ_INT(cb_journal_open(s, &bad, err, sizeof err), CB_E_INVALID);   // one is installed already
    close_world(s);
    static uint8_t fr[4096];
    struct { const char *id; int idl; int64_t base; uint8_t flags; uint32_t sl, claim; int tail; bool opens; } c[] = {
        { "aaaaaaaaaaaa", 0, 0, 0, 4, 4, 0, true },          // an empty id: dropped
        { "aaaaaaaaaaaa", 17, 0, 0, 4, 4, 0, true },         // an id past CB_GAME_ID_MAX: dropped
        { "aaaaaaaaaaaa", 13, 0, 0, 4, 4, 0, true },         // an id one byte longer than written: dropped
        { "aaaaaaaaaaaa", -1, 0, 0, 4, 400, 0, true },       // a state longer than the record: dropped
        { gid, -1, 1, 0, 4, 4, 3, true },                    // bytes past the last field (else a commit that fits): dropped
        { "aaaaaaaaaaaa", -1, 0, 2, 4, 4, 0, true },         // a game that is gone (seeded): skipped
        { gid, -1, 0, 0, 4, 4, 0, true },                    // a commit the tables hold already: skipped
        { gid, -1, 5, 0, 4, 4, 0, false },                   // from a version the tables have not reached: refused
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        char db[340];
        snprintf(db, sizeof db, "%s/crafted%zu.sqlite", g_dir, i);
        crash_copy(path, db);
        const size_t n = full_frame(fr, c[i].id, c[i].idl, c[i].base, c[i].flags, c[i].sl, c[i].claim, c[i].tail);
        write_segment(db, fr, n);
        s = open_world(db, true, err, sizeof err);
        T_EQ_INT(s != NULL, c[i].opens);
        if (!s) continue;
        T_EQ_INT(-version_of(s, gid), 1);   // the game as the tables hold it
        close_world(s);
    }
    // A segment another format wrote (an older server's, its moves acknowledged) is never
    // skipped: the start is refused, and says how to get past it.
    char db[340];
    snprintf(db, sizeof db, "%s/crafted_old.sqlite", g_dir);
    crash_copy(path, db);
    const size_t n = full_frame(fr, gid, -1, 1, 0, 4, 4, 0);
    write_segment_as(db, fr, n, JOURNAL_FORMAT - 1);
    err[0] = 0;
    s = open_world(db, true, err, sizeof err);
    T_CHECK(s == NULL);
    if (s) close_world(s);
    T_CHECK(strstr(err, "stop the server that wrote it cleanly") != NULL);
    // The same segment in this format is read.
    write_segment(db, fr, n);
    s = open_world(db, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (s) { T_EQ_INT(-version_of(s, gid), 2); close_world(s); }   // its commit adopted (a lobby status)
}

// THE RESIDENCY CAP: past opts.max_games the least recently touched idle games leave at the
// flush thread's pass, and a game with moves waiting never does.
static void the_overlay_keeps_at_most_its_cap(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/cap.sqlite", g_dir);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.max_games = 4;
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) { memset(&g_jo, 0, sizeof g_jo); return; }
    enum { N = 8 };
    char gids[N][CB_GAME_ID_MAX + 1];
    for (int i = 0; i < N; i++) {
        g_now += 1000;
        two_humans_dealt(gids[i]);
    }
    T_EQ_INT(cb_journal_flush(s), CB_OK);   // the lobbies' and deals' whole commits into the tables: every game idle
    move_t m;
    // A move waits in the two oldest games: they stay, whatever else leaves.
    g_now += 1000;
    T_CHECK(step(s, gids[0], &m) > 0);
    T_CHECK(step(s, gids[1], &m) > 0);
    T_EQ_INT(cb_journal_flush_due(s), CB_OK);   // the flush thread's pass (it may have run already: kicked)
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.games, 4);
    T_EQ_INT(js.waiting, 2);
    long reads = table_reads(s);
    T_CHECK(step(s, gids[0], &m) > 0);   // resident: no read
    T_CHECK(step(s, gids[1], &m) > 0);
    T_EQ_INT(table_reads(s) - reads, 0);
    memset(&g_jo, 0, sizeof g_jo);
    close_world(s);
}

// NO SECOND OWNER (R7): no server source keeps a copy of a game's row beside the overlay. The
// row cache that lived in table_ops.c is gone, whole: no source names it.
static bool source_names(const char *path, const char *const *words, int n) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    bool hit = false;
    while (!hit && fgets(line, sizeof line, f))
        for (int i = 0; i < n && !hit; i++)
            if (strstr(line, words[i])) { fprintf(stderr, "%s names %s: %s", path, words[i], line); hit = true; }
    fclose(f);
    return hit;
}

static void no_row_cache_beside_the_overlay(void) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", __FILE__);   // .../foolish/server/test/unit_journal.c
    char *cut = strstr(dir, "/test/unit_journal.c");
    T_CHECK(cut != NULL);
    if (!cut) return;
    *cut = 0;
    static const char *const WORDS[] = { "cb_row_cache", "CB_ROW_CACHE", "g_cache", "cb_row_load", "store_forget" };
    static const char *const SUBS[] = { "src/app", "src/store" };
    int files = 0;
    for (int d = 0; d < 2; d++) {
        char sub[600];
        snprintf(sub, sizeof sub, "%s/%s", dir, SUBS[d]);
        DIR *dp = opendir(sub);
        T_CHECK(dp != NULL);
        struct dirent *e;
        while (dp && (e = readdir(dp))) {
            const size_t n = strlen(e->d_name);
            if (n < 3 || (strcmp(e->d_name + n - 2, ".c") != 0 && strcmp(e->d_name + n - 2, ".h") != 0)) continue;
            char f[900];
            snprintf(f, sizeof f, "%s/%s", sub, e->d_name);
            files++;
            T_CHECK(!source_names(f, WORDS, 5));
        }
        if (dp) closedir(dp);
    }
    T_CHECK(files > 10);
}

// ---- the resident is the row (R3, T9) -------------------------------------------------------
//
// Random games played twice in step, on the kernel directly: once the way the server plays
// them (each operation on a table ADOPTED from the last one's resident), once the way a load
// of the stored row and the journal's recovery do (each on a table LOADED from the resident's
// encoded row). After every operation the two tables are the same board field by field, their
// products are the same bytes, and the resident's encode is the products' row exactly.

typedef struct {
    Game       *g;
    TableSnaps *sn;
    Table       t;
} ktable;

static void kt_open(ktable *k) {
    k->g = calloc(1, sizeof *k->g);
    k->sn = calloc(1, sizeof *k->sn);
    table_init(&k->t, k->g, k->sn);
}

static void kt_close(ktable *k) {
    free(k->g);
    free(k->sn);
}

static bool card_same(Card a, Card b) { return a.suit == b.suit && a.value == b.value; }

// Every field an operation can read, within its counts (state_get leaves the rest alone).
static bool same_board(const Table *a, const Table *b) {
    const Game *x = a->g, *y = b->g;
    bool same = x->status == y->status && x->num_players == y->num_players && x->power_suit == y->power_suit &&
                x->first_attacker == y->first_attacker && x->defender == y->defender && x->num_battles == y->num_battles &&
                x->deck_count == y->deck_count && x->discard_pile_length == y->discard_pile_length &&
                x->has_flipped == y->has_flipped && x->deterministic_deck == y->deterministic_deck && x->rules == y->rules &&
                card_same(x->flipped, y->flipped) && x->num_eliminated == y->num_eliminated &&
                x->good_players_mask == y->good_players_mask && x->has_good_timestamp == y->has_good_timestamp &&
                x->log_cap == y->log_cap && x->log_virt == y->log_virt && x->num_logs == y->num_logs;
    for (int i = 0; same && i < x->deck_count; i++) same = card_same(x->deck[i], y->deck[i]);
    for (int i = 0; same && i < x->num_battles; i++)
        same = card_same(x->table_battles[i].attack, y->table_battles[i].attack) &&
               card_same(x->table_battles[i].defense, y->table_battles[i].defense);
    for (int i = 0; same && i < x->num_eliminated; i++) same = x->elimination_order[i] == y->elimination_order[i];
    for (int p = 0; same && p < x->num_players; p++) {
        const Player *u = &x->players[p], *v = &y->players[p];
        same = u->status == v->status && u->hand_count == v->hand_count && u->awaiting_attack == v->awaiting_attack &&
               u->strategy_key == v->strategy_key;
        for (int i = 0; same && i < u->hand_count; i++) same = card_same(u->hand[i], v->hand[i]);
    }
    uint8_t ra[ROSTER_BYTES], rb[ROSTER_BYTES];
    same = same && roster_encode(&a->r, ra, sizeof ra) == ROSTER_BYTES && roster_encode(&b->r, rb, sizeof rb) == ROSTER_BYTES &&
           memcmp(ra, rb, sizeof ra) == 0;
    same = same && memcmp(&a->clock, &b->clock, sizeof a->clock) == 0 && a->ratings.n == b->ratings.n &&
           a->rng_base == b->rng_base;
    return same;
}

// The fastest linked brains that read no session log (the drive is not handed one here).
static int kt_brains(const char *keys[2]) {
    int n = 0;
    for (int i = 0; i < bot_roster_count() && n < 2; i++) {
        const BotRosterEntry *e = bot_roster_at(i);
        if (bot_roster_linked(i) && !e->uses_logs && e->tier > 0 && e->tier <= 3) keys[n++] = e->key;
    }
    return n;
}

static void the_resident_is_the_row(void) {
    const char *brains[2];
    T_EQ_INT(kt_brains(brains), 2);
    enum { ARENA = 128 * 1024 };
    uint8_t *aa = malloc(ARENA), *ab = malloc(ARENA), state[4096], roster[ROSTER_BYTES + 8];
    static const char GID[] = "0123456789ab";
    uint64_t rng = 99;
    int ops = 0, games = 0;
    for (; games < 40 && ops < 1500; games++) {
        // A lobby: the human, two bots of the two brains, dealt by the human's ready.
        ktable a, b;
        kt_open(&a);
        kt_open(&b);
        uint8_t seed[32];
        for (int i = 0; i < 32; i++) seed[i] = (uint8_t)rng_u32(&rng);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", seed[i]);
        T_EQ_INT(table_create(&a.t, UID_A, (int)strlen(UID_A), "ALICE", 5), TABLE_OK);
        table_add_bot(&a.t, UID_A, (int)strlen(UID_A), "b0000000-0000-4000-8000-000000000001", 36, "%one", 4, brains[0],
                      (int)strlen(brains[0]), seed);
        if (games % 2) table_add_bot(&a.t, UID_A, (int)strlen(UID_A), "b0000000-0000-4000-8000-000000000002", 36, "%two", 4,
                                     brains[1], (int)strlen(brains[1]), seed);
        T_CHECK(table_ready(&a.t, UID_A, (int)strlen(UID_A), seed) >= 0);
        table_set_deal_seed(&a.t, hex, 64);
        TableCommit ca;
        int64_t now = 1760000000000LL;
        uint32_t version = 3;
        T_CHECK(table_commit_products(&a.t, GID, 12, version, now, &ca, aa, ARENA) > 0);
        TableResident res;
        table_resident(&a.t, &ca.clock, &res);
        while (ops < 1500) {
            // The server's table and the load's.
            table_init(&a.t, a.g, a.sn);
            T_EQ_INT(table_adopt(&a.t, &res), TABLE_OK);
            int sl = 0, rl = 0;
            T_EQ_INT(table_resident_encode(&res, state, sizeof state, &sl, roster, sizeof roster, &rl), TABLE_OK);
            table_init(&b.t, b.g, b.sn);
            T_EQ_INT(table_load(&b.t, state, sl, roster, rl), TABLE_OK);
            table_set_deal_seed(&b.t, hex, 64);
            if (!same_board(&a.t, &b.t)) { T_CHECK(!"adopted and loaded boards differ"); break; }
            if (a.g->status != GAME_STATUS_PLAYING) break;
            // One operation on both: the human's random legal move, else a bot cycle.
            int ra = -1, rb = -1;
            LegalMoves *moves = calloc(1, sizeof *moves);
            calculate_legal_moves(a.g, 0, moves);
            int k = 0;
            for (int i = 0; i < moves->n; i++) k += moves->moves[i].type != MOVE_WAIT;
            if (k > 0) {
                int want = (int)(rng_u32(&rng) % (uint32_t)k);
                uint8_t wire[64];
                int n = 0;
                for (int i = 0; i < moves->n; i++) {
                    const LegalMove *m = &moves->moves[i];
                    if (m->type == MOVE_WAIT || want--) continue;
                    AwireAction w;
                    memset(&w, 0, sizeof w);
                    w.kind = m->type;
                    w.n = m->n_cards;
                    for (int c = 0; c < m->n_cards; c++) { w.cards[c] = m->cards[c]; w.attacks[c] = m->attack_cards[c]; }
                    n = awire_encode(&w, wire, sizeof wire);
                    break;
                }
                ra = table_act(&a.t, UID_A, (int)strlen(UID_A), wire, n, -1, 0);
                rb = table_act(&b.t, UID_A, (int)strlen(UID_A), wire, n, -1, 0);
            } else if (table_bots_due(&a.t)) {
                BotDriveOut da, db;
                ra = table_bot_drive(&a.t, NULL, 0, 0, &da);
                rb = table_bot_drive(&b.t, NULL, 0, 0, &db);
            }
            free(moves);
            T_EQ_INT(ra, rb);
            if (ra < 0) break;
            ops++;
            version++;
            now += 1500;
            const int na = table_commit_products(&a.t, GID, 12, version, now, &ca, aa, ARENA);
            TableCommit cb;
            const int nb = table_commit_products(&b.t, GID, 12, version, now, &cb, ab, ARENA);
            T_EQ_INT(na, nb);
            if (na <= 0 || na != nb || memcmp(aa, ab, (size_t)na) != 0) { T_CHECK(!"the products differ"); break; }
            // The products with no row (a plain move's) are the same products less the row.
            TableCommit cn;
            const int nn = table_commit_products_flags(&a.t, GID, 12, version, now, TABLE_PRODUCTS_NO_ROW, &cn, ab, ARENA);
            bool same = nn > 0 && cn.state.len == 0 && cn.roster.len == 0 && memcmp(&cn.clock, &ca.clock, sizeof cn.clock) == 0 &&
                        cn.status == ca.status && cn.needs_bots == ca.needs_bots && cn.ended == ca.ended &&
                        cn.closed_round == ca.closed_round && cn.logs_reset == ca.logs_reset && cn.n_events == ca.n_events &&
                        cn.roster_changed == ca.roster_changed && cn.logs.len == ca.logs.len &&
                        memcmp(ab + cn.logs.off, aa + ca.logs.off, (size_t)ca.logs.len) == 0 &&
                        cn.spectator.len == ca.spectator.len &&
                        memcmp(ab + cn.spectator.off, aa + ca.spectator.off, (size_t)ca.spectator.len) == 0;
            for (int v = 0; same && v < MAX_PLAYERS; v++)
                same = cn.views[v].len == ca.views[v].len && memcmp(ab + cn.views[v].off, aa + ca.views[v].off, (size_t)ca.views[v].len) == 0;
            if (!same) { T_CHECK(!"the products with no row are not the products less the row"); break; }
            // T9: the resident's encode is the products' row, byte for byte.
            table_resident(&a.t, &ca.clock, &res);
            T_EQ_INT(table_resident_encode(&res, state, sizeof state, &sl, roster, sizeof roster, &rl), TABLE_OK);
            if (sl != ca.state.len || memcmp(state, aa + ca.state.off, (size_t)sl) != 0 || rl != ca.roster.len ||
                memcmp(roster, aa + ca.roster.off, (size_t)rl) != 0) {
                T_CHECK(!"the resident's encode is not the products' row");
                break;
            }
        }
        kt_close(&a);
        kt_close(&b);
    }
    T_CHECK(ops >= 1000);
    free(aa);
    free(ab);
}

// ---- the flush rule: a move writes no table until its game is due --------------------------

static int count_segments(const char *path) {
    int n = 0;
    char seg[400];
    DIR *d = opendir(g_dir);
    struct dirent *e;
    const char *base = strrchr(path, '/') + 1;
    snprintf(seg, sizeof seg, "%s-journal.", base);
    while (d && (e = readdir(d))) n += strncmp(e->d_name, seg, strlen(seg)) == 0;
    if (d) closedir(d);
    return n;
}

// A game deleted while moves wait in the journal: the delete goes around the journal (its
// waiting moves leave with it), and a crash after it does not bring the game back from the
// segments that still hold them.
static bool game_gone(cb_store *s, const char *gid) {
    cb_game_row g;
    memset(&g, 0, sizeof g);
    int rc = CB_ROWS_CALL(s, game_get, gid, &g);
    cb_game_row_free(&g);
    return rc == CB_E_NOT_FOUND;
}

static void a_deleted_game_stays_deleted(void) {
    char path[320], copy[320], err[256] = "";
    snprintf(path, sizeof path, "%s/deleted%d.sqlite", g_dir, g_bots);
    snprintf(copy, sizeof copy, "%s/deleted%d_crash.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    move_t m;
    int64_t v = 0;
    for (int k = 0; k < 6; k++) v = step(s, gid, &m);
    T_CHECK(v > 0);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_CHECK(js.waiting > 0);
    T_EQ_INT(CB_ROWS_CALL(s, game_delete, gid, v), CB_OK);
    T_CHECK(game_gone(s, gid));
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, 0);
    crash_copy(path, copy);
    close_world(s);
    s = open_world(copy, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (s) { T_CHECK(game_gone(s, gid)); close_world(s); }
}

static void a_move_writes_no_table_until_its_game_is_due(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/rule%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.seg_bytes = 1;   // a segment per batch: the trim rule shows in the file count
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    const unsigned long wal0 = cb_store_local_wal_frames(s);
    move_t m;
    int moves = 0;
    for (; moves < 10 && step(s, gid, &m) > 0; moves++) {}
    T_CHECK(moves == 10);
    // No frame of the tables' WAL for ten moves, and the tick finds nothing due.
    T_EQ_INT((long)cb_store_local_wal_frames(s), (long)wal0);
    T_EQ_INT(cb_journal_flush_due(s), CB_OK);
    T_EQ_INT((long)cb_store_local_wal_frames(s), (long)wal0);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, js.records);
    T_EQ_INT(js.flushes, 0);
    T_CHECK(js.segments >= 10);   // every segment holds a waiting move: none is trimmed
    const int on_disk = count_segments(path);   // and the next one, made ahead
    T_CHECK(on_disk == js.segments || on_disk == js.segments + 1);
    close_world(s);

    // A game whose waiting moves hold game_bytes is due at the next tick; the segments
    // it was the last waiter in go with it, the open one stays.
    g_jo.game_bytes = 1;
    s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    T_CHECK(step(s, gid, &m) > 0);
    T_CHECK(step(s, gid, &m) > 0);
    T_EQ_INT(cb_journal_flush_due(s), CB_OK);   // (the flush thread, kicked, may have been first)
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, 0);
    T_EQ_INT(js.segments, 1);
    T_CHECK(count_segments(path) <= 2);
    close_world(s);

    // One whose oldest waiting move is game_age_ms old is due.
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.game_age_ms = 30;
    s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    T_CHECK(step(s, gid, &m) > 0);
    cb_journal_stats_get(s, &js);
    const long flushes0 = js.flushes;
    usleep(60000);
    T_EQ_INT(cb_journal_flush_due(s), CB_OK);
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, 0);
    T_CHECK(js.flushes > flushes0);
    close_world(s);

    // Every game once all the waiting moves hold total_bytes.
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.total_bytes = 1;
    s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    T_CHECK(step(s, gid, &m) > 0);
    T_EQ_INT(cb_journal_flush_due(s), CB_OK);
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, 0);
    close_world(s);
    memset(&g_jo, 0, sizeof g_jo);
}

// THE TRANSACTION BUDGET: ten games' waiting moves flush as bounded transactions, the oldest
// games first, the flush lock given back between them: no transaction carries more than
// txn_moves, and the pass is as many transactions as that takes (a write around the journal
// waits one transaction, not the pass). Every move is in the tables after it.
static void a_flush_is_bounded_transactions(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/txn%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.txn_moves = 40;
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    enum { NG = 10, PER_GAME = 25 };
    // The tests after this one play the games the shared move stream and clock give them: both
    // are put back at the end.
    const uint64_t rng0 = g_rng;
    const int64_t now0 = g_now;
    char gid[NG][CB_GAME_ID_MAX + 1];
    for (int i = 0; i < NG; i++) {
        cb_limits_reset();   // the create limit is per user and minute
        g_now += 60000;      // and the newest game is the first one A's list names
        two_humans_dealt(gid[i]);
        for (int k = 0; k < i; k++) T_CHECK(strcmp(gid[i], gid[k]) != 0);
    }
    // Each game's waiting records kept under the budget (a bot's step is a drive of several
    // cycles, each a record), so the one game the budget lets go alone over it never comes up:
    // every transaction carries at most txn_moves.
    move_t m;
    int played = 0, recs[NG] = { 0 };
    for (int r = 0; r < PER_GAME; r++)
        for (int i = 0; i < NG; i++) {
            if (recs[i] >= g_jo.txn_moves / 2) continue;
            cb_journal_stats a, b;
            cb_journal_stats_get(s, &a);
            played += step(s, gid[i], &m) > 0;
            cb_journal_stats_get(s, &b);
            recs[i] += (int)(b.records - a.records);
        }
    for (int i = 0; i < NG; i++) T_CHECK(recs[i] + 4 <= g_jo.txn_moves);   // (a deal's records came before)
    T_CHECK(played > NG * PER_GAME / 4);
    uint64_t want[NG];
    for (int i = 0; i < NG; i++) want[i] = row_hash(s, gid[i]);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    const long waiting = js.waiting, flushes0 = js.flushes;
    T_CHECK(waiting > 3 * g_jo.txn_moves);
    T_EQ_INT(cb_journal_flush(s), CB_OK);
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.waiting, 0);
    T_CHECK(js.txn_moves_max > 0 && js.txn_moves_max <= g_jo.txn_moves);
    T_CHECK(js.flushes - flushes0 >= (waiting + g_jo.txn_moves - 1) / g_jo.txn_moves);
    // (How long a transaction held the lock is the stats' to report, not this test's to time: under
    // a sanitizer on a loaded runner one descheduled transaction can take most of a pass. The
    // count and the size of the transactions are what the budget owns.)
    T_CHECK(js.txn_hold_us_max > 0);   // (a maximum since the open: the setup's flushes count too)
    close_world(s);
    // The tables hold every game as the overlay did.
    memset(&g_jo, 0, sizeof g_jo);
    s = open_world(path, false, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    for (int i = 0; i < NG; i++) T_CHECK(row_hash(s, gid[i]) == want[i]);
    cb_store_close(s);
    g_rng = rng0;
    g_now = now0;
    cb_limits_reset();
}

// THE RESIDENT'S RNG BASE IS ITS ROW'S SEED'S: a deal through the journal, and every move after
// it, leave a resident whose mid-game RNG base is the one a load of the row derives (table_ops.c
// derives it again only for a commit that is not a plain move).
typedef struct { uint32_t base; bool held; } base_seen;
static int resident_base(void *ud, const cb_game_row *row, const struct TableResident *res) {
    base_seen *b = ud;
    b->held = res != NULL;
    if (res) b->base = res->rng_base;
    return CB_OK;
}
static uint32_t row_base(cb_store *s, const char *gid) {
    cb_game_row row = stored(s, gid);
    char hex[2 * CB_SEED_LEN + 1] = "";
    for (int i = 0; row.has_seed && i < CB_SEED_LEN; i++) snprintf(hex + 2 * i, 3, "%02x", row.seed[i]);
    Game *g = calloc(1, sizeof *g);
    TableSnaps *sn = calloc(1, sizeof *sn);
    Table t;
    table_init(&t, g, sn);
    table_set_deal_seed(&t, hex, (int)strlen(hex));
    free(g);
    free(sn);
    cb_game_row_free(&row);
    return t.rng_base;
}
static void the_residents_rng_base_is_its_rows(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/base%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    move_t m;
    int checked = 0;
    for (int i = 0; i < 12; i++) {
        base_seen b = { 0, false };
        T_EQ_INT(cb_journal_game_acquire(s, gid, resident_base, &b), CB_OK);
        if (b.held) { T_EQ_INT((long)b.base, (long)row_base(s, gid)); checked++; }
        if (step(s, gid, &m) <= 0) break;
    }
    T_CHECK(checked >= 2);
    close_world(s);
}

// THE FLUSH THREAD HAS NO TICK: with nothing waiting it sleeps until told, so an idle box wakes
// it never; a move that begins waiting sets its alarm (one wake), and the move is flushed at its
// age (one more).
static void the_flush_thread_has_no_tick(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/tick%d.sqlite", g_dir, g_bots);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.game_age_ms = 200;
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    cb_journal_stats js;
    for (int i = 0; i < 100; i++) {   // the deal's records flushed at their age
        cb_journal_stats_get(s, &js);
        if (!js.waiting) break;
        usleep(20000);
    }
    T_EQ_INT(js.waiting, 0);
    usleep(50000);
    cb_journal_stats_get(s, &js);
    const long w0 = js.flusher_wakes, f0 = js.flushes;
    usleep(1200000);   // idle: nothing waits
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.flusher_wakes - w0, 0);
    move_t m;
    T_CHECK(step(s, gid, &m) > 0);
    for (int i = 0; i < 100; i++) {
        cb_journal_stats_get(s, &js);
        if (!js.waiting) break;
        usleep(20000);
    }
    T_EQ_INT(js.waiting, 0);
    // Each time the game began waiting costs at most two wakes (its alarm set, its age reached),
    // and each ends in a flush: a bot drive whose cycles outlast game_age_ms (a sanitizer's pace)
    // is flushed between them and waits again.
    const long flushes = js.flushes - f0, wakes = js.flusher_wakes - w0;
    T_CHECK(flushes >= 1);
    T_CHECK(wakes >= 1 && wakes <= 2 * flushes);
    close_world(s);
    memset(&g_jo, 0, sizeof g_jo);
}

// ---- the dashboard list and the bot scan read the overlay, and flush nothing ----------------

static bool collect_ids(const cb_member_row *r, void *ud) {
    char (*ids)[CB_GAME_ID_MAX + 1] = ud;
    int i = 0;
    while (ids[i][0]) i++;
    if (i < 3) snprintf(ids[i], sizeof ids[i], "%s", r->game_id);
    return true;
}

typedef struct { char ids[64][CB_GAME_ID_MAX + 1]; int n; } names_t;
static bool collect_name(const char *name, void *ud) {
    names_t *v = ud;
    if (v->n < 64) snprintf(v->ids[v->n++], sizeof v->ids[0], "%s", name);
    return true;
}
static int name_cmp(const void *a, const void *b) { return strcmp(a, b); }

static void the_lists_read_the_overlay_without_a_flush(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/lists%d.sqlite", g_dir, g_bots);
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    char g1[CB_GAME_ID_MAX + 1], g2[CB_GAME_ID_MAX + 1];
    two_humans_dealt(g1);
    g_now += 10000;
    two_humans_dealt(g2);   // the newer in the tables
    T_CHECK(strcmp(g1, g2) != 0);
    g_now += 10000;
    move_t m;
    // g1 is the newer now, in the overlay alone; with a bot, it stops where the bot is to move.
    if (!next_move(s, g1, &m)) T_CHECK(step(s, g1, &m) > 0);
    for (int i = 0; i < (g_bots ? 40 : 3) && next_move(s, g1, &m); i++) T_CHECK(play(g1, &m) > 0);
    const unsigned long wal0 = cb_store_local_wal_frames(s);
    char top[4][CB_GAME_ID_MAX + 1];
    memset(top, 0, sizeof top);
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, UID_A, 1, collect_ids, top), 1);
    T_CHECK(strcmp(top[0], g1) == 0);
    memset(top, 0, sizeof top);
    T_EQ_INT(CB_ROWS_CALL(s, games_of_user, UID_A, 5, collect_ids, top), 2);
    T_CHECK(strcmp(top[0], g1) == 0 && strcmp(top[1], g2) == 0);
    // The bot scan answers what the tables answer once flushed.
    names_t before = { 0 }, after = { 0 };
    T_CHECK(CB_ROWS_CALL(s, games_needing_bots, INT64_MAX, 0, 50, collect_name, &before) >= 0);
    T_EQ_INT((long)cb_store_local_wal_frames(s), (long)wal0);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_CHECK(js.waiting > 0);
    T_EQ_INT(cb_journal_flush(s), CB_OK);
    T_CHECK(cb_rows_sql.games_needing_bots(s, INT64_MAX, 0, 50, collect_name, &after) >= 0);
    qsort(before.ids, (size_t)before.n, sizeof before.ids[0], name_cmp);
    qsort(after.ids, (size_t)after.n, sizeof after.ids[0], name_cmp);
    T_EQ_INT(before.n, after.n);
    T_CHECK(memcmp(before.ids, after.ids, sizeof before.ids[0] * (size_t)before.n) == 0);
    if (g_bots) T_CHECK(after.n > 0);
    close_world(s);
}

// ---- a backup mid-game ----------------------------------------------------------------------

// A bucket in a directory: one file per key.
static int g_bucket_puts, g_db_puts;
static int file_put(void *ud, const char *key, const char *path) {
    char cmd[1400];
    g_bucket_puts++;
    g_db_puts += !strstr(key, "-journal.") && !strstr(key, ".manifest");
    snprintf(cmd, sizeof cmd, "cp '%s' '%s/%s'", path, (const char *)ud, key);
    return system(cmd) == 0 ? CB_OK : CB_E_IO;
}
static int file_get(void *ud, const char *key, const char *path) {
    char src[1200], cmd[2600];
    snprintf(src, sizeof src, "%s/%s", (const char *)ud, key);
    if (access(src, F_OK) != 0) return CB_E_NOT_FOUND;
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", src, path);
    return system(cmd) == 0 ? CB_OK : CB_E_IO;
}
static int file_del(void *ud, const char *key) {
    char src[1200];
    snprintf(src, sizeof src, "%s/%s", (const char *)ud, key);
    return unlink(src) == 0 || errno == ENOENT ? CB_OK : CB_E_IO;
}

// THE RESTORE DRILL: backups mid-game with no flush between them (the database goes up
// once, the segments after it as they grow), the disk lost, and a restore that replays the
// segments onto the copy holds every acknowledged move. Segments go only after a flush AND
// an upload of the copy that holds it.
static void a_restore_mid_game_holds_every_acknowledged_move(void) {
    char path[320], err[256] = "", bucket[320], key[64], rpath[320];
    snprintf(path, sizeof path, "%s/live%d.sqlite", g_dir, g_bots);
    snprintf(bucket, sizeof bucket, "%s/bucket%d", g_dir, g_bots);
    snprintf(key, sizeof key, "backup%d.sqlite", g_bots);
    T_EQ_INT(mkdir(bucket, 0700), 0);
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.seg_bytes = 64;   // several segments
    cb_store *s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", path);
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    move_t m;
    int64_t v = -1;
    for (int i = 0; i < 6 && (v = step(s, gid, &m)) > 0; i++) {}
    cb_blob blob = { file_put, file_get, bucket, file_del };
    cb_backup *bk = cb_backup_start(&g_ctx, &blob, key, 0);
    T_CHECK(bk != NULL);
    const unsigned long wal0 = cb_store_local_wal_frames(s);
    g_bucket_puts = 0;
    T_EQ_INT(cb_backup_once(bk), 1);   // the copy, the segments, the manifest
    const int first_puts = g_bucket_puts;
    T_CHECK(first_puts >= 3);
    for (int i = 0; i < 12 && (v = step(s, gid, &m)) > 0; i++) {}
    T_CHECK(v > 0);
    g_bucket_puts = g_db_puts = 0;
    T_EQ_INT(cb_backup_once(bk), 1);   // only what grew, and the manifest: the database did not change
    T_EQ_INT(g_db_puts, 0);
    char dbobj[400];
    snprintf(dbobj, sizeof dbobj, "%s/%s", bucket, key);
    T_CHECK(g_bucket_puts >= 2);
    for (int i = 0; i < 3 && (v = step(s, gid, &m)) > 0; i++) {}
    T_CHECK(v > 0);
    T_EQ_INT(cb_backup_once(bk), 1);
    const uint64_t h = row_hash(s, gid);
    // No flush happened: every move of the game is in the segments alone.
    T_EQ_INT((long)cb_store_local_wal_frames(s), (long)wal0);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.flushes, 0);
    T_CHECK(js.segments >= 3);
    // What went up is what the segments hold, not the mapped files' full size.
    {
        DIR *bd = opendir(bucket);
        struct dirent *be;
        long up = 0;
        while (bd && (be = readdir(bd))) {
            char bp[1400];
            struct stat bst;
            snprintf(bp, sizeof bp, "%s/%s", bucket, be->d_name);
            if (strstr(be->d_name, "-journal.") && stat(bp, &bst) == 0) up += (long)bst.st_size;
        }
        if (bd) closedir(bd);
        T_CHECK(up > 0 && up < 32768);   // the moves' bytes and the lobby's whole commits (a row each)
    }

    // The disk is lost (the live files stay where they are, unread): a restore beside them.
    snprintf(rpath, sizeof rpath, "%s/restored%d.sqlite", g_dir, g_bots);
    T_EQ_INT(cb_backup_restore(&blob, key, rpath), CB_OK);
    char marker[400];
    snprintf(marker, sizeof marker, "%s" CB_STORE_RESTORED_SUFFIX, rpath);
    T_CHECK(access(marker, F_OK) == 0);
    cb_backup_stop(bk);
    cb_store *live = s;
    cb_journal_close(live);
    cb_store *r = open_world(rpath, true, err, sizeof err);
    T_CHECK(r != NULL);
    if (!r) { fprintf(stderr, "restore: %s\n", err); cb_store_close(live); return; }
    T_CHECK(access(marker, F_OK) != 0);   // replayed: the next start is an ordinary one
    cb_game_row row = stored(r, gid);
    T_EQ_INT(row.version, v);
    cb_game_row_free(&row);
    T_CHECK(row_hash(r, gid) == h);
    close_world(r);

    // A copy that lacks a write the segments continue from (here: its game set one version
    // back): an ordinary start refuses it (a move is missing), a restored one skips the game.
    char rpath2[320], marker2[400];
    snprintf(rpath2, sizeof rpath2, "%s/restored%db.sqlite", g_dir, g_bots);
    snprintf(marker2, sizeof marker2, "%s" CB_STORE_RESTORED_SUFFIX, rpath2);
    T_EQ_INT(cb_backup_restore(&blob, key, rpath2), CB_OK);
    sqlite3 *db = NULL;
    char sql[200];
    snprintf(sql, sizeof sql, "UPDATE games SET version = version - 1 WHERE id = '%s'", gid);
    T_EQ_INT(sqlite3_open(rpath2, &db), SQLITE_OK);
    T_EQ_INT(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
    T_EQ_INT(unlink(marker2), 0);
    r = open_world(rpath2, true, err, sizeof err);
    T_CHECK(r == NULL);
    if (r) close_world(r);
    FILE *mk = fopen(marker2, "wb");
    if (mk) fclose(mk);
    r = open_world(rpath2, true, err, sizeof err);
    T_CHECK(r != NULL);
    if (r) {
        T_CHECK(version_of(r, gid) < v);
        close_world(r);
    }
    cb_store_close(live);

    // Pinned: a flush does not delete what the last uploaded copy needs; the next upload does.
    memset(&g_jo, 0, sizeof g_jo);
    g_jo.seg_bytes = 1;   // a segment per batch
    s = open_world(path, true, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    snprintf(g_cfg.db_path, sizeof g_cfg.db_path, "%s", path);
    two_humans_dealt(gid);   // a new game: the first may be near its end
    bk = cb_backup_start(&g_ctx, &blob, key, 0);
    T_EQ_INT(cb_backup_once(bk), 1);
    for (int i = 0; i < 6 && (v = step(s, gid, &m)) > 0; i++) {}
    T_EQ_INT(cb_backup_once(bk), 1);
    cb_journal_stats_get(s, &js);
    const long segs = js.segments;
    T_CHECK(segs >= 3);
    T_EQ_INT(cb_journal_flush(s), CB_OK);
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.segments, segs);       // flushed, not uploaded: kept
    T_EQ_INT(cb_backup_once(bk), 1);   // the copy that holds the flush goes up
    cb_journal_stats_get(s, &js);
    T_EQ_INT(js.segments, 1);
    // and the bucket no longer holds the segments its manifest stopped naming
    int left = 0;
    DIR *d = opendir(bucket);
    struct dirent *e;
    while (d && (e = readdir(d))) left += strstr(e->d_name, "-journal.") != NULL;
    if (d) closedir(d);
    T_CHECK(left <= 1);
    cb_backup_stop(bk);
    close_world(s);
    memset(&g_jo, 0, sizeof g_jo);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/unit_journal.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) return 1;
    T_RUN(a_batch_round_trips);
    T_RUN(a_refused_batch_consumes_nothing);
    T_RUN(a_batch_with_bytes_past_its_records_is_refused);
    T_RUN(a_torn_or_corrupt_tail_drops_only_itself);
    T_RUN(a_move_off_its_board_is_refused);
    T_RUN(a_move_allocates_almost_nothing);
    T_RUN(a_legacy_deck_cycle_recovers_and_a_cut_one_is_refused);
    T_RUN(the_resident_is_the_row);
    T_RUN(a_resident_the_tables_contradict_is_dropped);
    T_RUN(the_overlay_keeps_at_most_its_cap);
    T_RUN(a_torn_or_corrupt_whole_commit_is_dropped);
    T_RUN(crafted_whole_commits_recover_or_refuse);
    T_RUN(no_row_cache_beside_the_overlay);
    for (int b = 0; b < 2; b++) {
        g_bots = b;
        T_RUN(a_deal_and_its_moves_read_no_table);
        T_RUN(a_refusal_changes_nothing_and_a_move_is_read_at_once);
        T_RUN(a_lobby_and_its_deal_write_no_table);
        g_bots = b;
        T_RUN(the_flush_writes_the_rows_per_move_commits_write);
        T_RUN(a_flush_racing_the_moves_leaves_every_log_whole);
        T_RUN(a_rated_end_is_a_journal_record);
        T_RUN(a_restore_mid_game_holds_every_acknowledged_move);
        T_RUN(every_acknowledged_move_survives_a_sigkill);
        T_RUN(a_move_writes_no_table_until_its_game_is_due);
        T_RUN(a_flush_is_bounded_transactions);
        T_RUN(the_flush_thread_has_no_tick);
        T_RUN(the_residents_rng_base_is_its_rows);
        T_RUN(the_lists_read_the_overlay_without_a_flush);
        T_RUN(an_admin_write_is_never_written_over);
        T_RUN(mapped_segments_keep_every_committed_batch);
        T_RUN(a_deleted_game_stays_deleted);
    }
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", g_dir);
    return T_DONE();
}

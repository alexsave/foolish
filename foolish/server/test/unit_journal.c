// unit_journal.c - the move journal (store/journal.h): the batch codec, a torn or corrupt tail,
// recovery after a SIGKILL, the flush's rows against per-move commits, and a backup mid-game.
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
#include "store/store_sql.h"
#include "awire.h"
#include "legal.h"
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
static bool last_batch(const char *db, char *seg, size_t cap, long *off);
static void crash_copy(const char *path, const char *copy);

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
    bool same = strcmp(a->game, b->game) == 0 && a->base_version == b->base_version && a->now_ms == b->now_ms &&
                a->bots == b->bots && a->n_mv == b->n_mv;
    for (int k = 0; same && k < a->n_mv; k++) {
        const ReplayMove *x = &a->mv[k], *y = &b->mv[k];
        same = x->seat == y->seat && x->menu_n == y->menu_n && x->index == y->index && x->wire_len == y->wire_len &&
               memcmp(x->wire, y->wire, (size_t)x->wire_len) == 0;
    }
    return same;
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
            m->bots = rng_u32(&rng) % 3 == 0;   // a third are bot cycles of 1..9 moves
            m->n_mv = m->bots ? 1 + (int)(rng_u32(&rng) % BOT_DRIVE_MAX_ACTIONS) : 1;
            for (int q = 0; q < m->n_mv; q++) random_move(&rng, &m->mv[q]);
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
    // A move costs a few bytes: under 6 bytes a coded move, a batch's header included.
    int moves = 0;
    for (int i = 0; i < PER; i++) moves += g_in[1][i].n_mv;
    T_CHECK(ends[1] - ends[0] < (size_t)(6 * moves));
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
    Game *g = calloc(1, sizeof *g);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    Table t;
    table_init(&t, g, snaps);
    T_EQ_INT(table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len), TABLE_OK);
    int seat = -1, n = 0;
    uint8_t wire[64], back[REPLAY_MOVE_WIRE_MAX];
    for (int k = 0; k < 2 && n <= 0; k++) n = pick_move(&row, seat = k, wire, sizeof wire);
    T_CHECK(n > 0);
    ReplayMove m;
    T_EQ_INT(replay_move_of(g, seat, wire, n, &m), 0);
    T_CHECK(m.menu_n > 0);
    // On its own board it names the move it was made from.
    int k = replay_move_wire(g, &m, back, sizeof back);
    AwireAction x, y;
    T_CHECK(k > 0 && awire_decode(back, k, &x) && awire_decode(wire, n, &y) && x.kind == y.kind && x.n == y.n);
    // A board whose menu is not the one it was coded against refuses it.
    m.menu_n++;
    T_EQ_INT(replay_move_wire(g, &m, back, sizeof back), -REPLAY_ENOTINMENU);

    // A live move is coded raw (no menu looked up): it codes and decodes to its own bytes,
    // and on its board it names them.
    ReplayMove raw, dec;
    T_EQ_INT(replay_move_raw(seat, wire, n, &raw), 0);
    T_EQ_INT(raw.menu_n, REPLAY_MOVE_RAW);
    ReplayCoder c;
    uint8_t code[64];
    T_EQ_INT(replay_coder_begin_encode(&c), 0);
    T_EQ_INT(replay_move_code(&c, &raw), 0);
    int code_len = replay_coder_finish(&c, code, sizeof code);
    T_CHECK(code_len > 0);
    T_EQ_INT(replay_coder_begin_decode(&c, code, code_len), 0);
    memset(&dec, 0, sizeof dec);
    T_EQ_INT(replay_move_code(&c, &dec), 0);
    T_CHECK(dec.seat == seat && dec.menu_n == REPLAY_MOVE_RAW && dec.wire_len == n && memcmp(dec.wire, wire, (size_t)n) == 0);
    k = replay_move_wire(g, &dec, back, sizeof back);
    T_CHECK(k == n && memcmp(back, wire, (size_t)n) == 0);
    // Its board check: the seat must have a move there. The other seat, with none, is refused.
    int other = 1 - seat;
    if (pick_move(&row, other, back, sizeof back) <= 0) {
        dec.seat = other;
        T_EQ_INT(replay_move_wire(g, &dec, back, sizeof back), -REPLAY_ENOTINMENU);
    }
    // No index is coded for it: 64 raw moves in one coder are smaller than the same moves as
    // the index form's escape (an index over a menu of REPLAY_MOVE_MENU_CAP, about 10 bits each).
    ReplayMove esc = raw;
    esc.menu_n = esc.index = REPLAY_MOVE_MENU_CAP;
    int lens[2];
    for (int form = 0; form < 2; form++) {
        static uint8_t batch[64 * 16];
        T_EQ_INT(replay_coder_begin_encode(&c), 0);
        for (int i = 0; i < 64; i++) {
            ReplayMove one = form ? esc : raw;
            T_EQ_INT(replay_move_code(&c, &one), 0);
        }
        lens[form] = replay_coder_finish(&c, batch, sizeof batch);
    }
    T_CHECK(lens[0] > 0 && lens[0] + 64 < lens[1]);
    // A malformed frame is not coded.
    T_EQ_INT(replay_move_raw(seat, wire, 0, &raw), -REPLAY_EINPUT);
    free(g);
    free(snaps);
    cb_game_row_free(&row);
    close_world(s);
}

// ---- recovery after a SIGKILL ---------------------------------------------------------------

typedef struct { int64_t version; uint64_t hash; } ack_t;

// The child: plays until killed, reporting each acknowledged move's version and the game's hash.
static void child_play(const char *path, const char *gid, int fd) {
    char err[256];
    cb_store *s = open_world(path, true, err, sizeof err);
    if (!s) _exit(3);
    move_t m;
    int64_t v;
    while ((v = step(s, gid, &m)) > 0) {
        ack_t a = { v, row_hash(s, gid) };
        if (write(fd, &a, sizeof a) != sizeof a) _exit(4);
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
    cb_jmove *ms = calloc(8192, sizeof *ms);
    int n = 0, cut = 0;
    cb_jslots *t = cb_jslots_new();
    for (size_t at = 16, used = 0; at < len && n < 8192 - 64; at += used) {
        int k = cb_journal_batch_decode(t, start, buf + at, len - at, &used, ms + n, 64);
        if (k < 0) break;
        n += k;
    }
    cb_jslots_free(t);
    for (int i = 0; i < n && !cut; i++)
        if (ms[i].bots && ms[i].n_mv >= 2) { ms[i].n_mv--; cut = 1; }
    t = cb_jslots_new();
    memcpy(out, buf, 16);
    size_t w = 16;
    for (int i = 0; i < n; i += 64) {
        int k = cb_journal_batch_encode(t, start, ms + i, n - i < 64 ? n - i : 64, out + w, (int)(sizeof out - w));
        T_CHECK(k > 0);
        if (k > 0) w += (size_t)k;
    }
    cb_jslots_free(t);
    free(ms);
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
    // A board from before the seed-dealt deck: a draw takes the draw stream's next number.
    cb_game_row row = stored(s, gid);
    T_CHECK(row.state_len > 2 && row.status == GAME_STATUS_PLAYING);
    sqlite3 *db = NULL;
    T_EQ_INT(sqlite3_open(path, &db), SQLITE_OK);
    sqlite3_stmt *st = NULL;
    uint8_t *state = malloc(row.state_len);
    memcpy(state, row.state, row.state_len);
    state[1] &= (uint8_t)~0x01;   // STATE_BLOB_FLAG_DETERMINISTIC
    sqlite3_prepare_v2(db, "UPDATE games SET state = ?1 WHERE id = ?2", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, state, (int)row.state_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, gid, -1, SQLITE_STATIC);
    T_EQ_INT(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(db);
    free(state);
    cb_game_row_free(&row);
    cb_row_cache_clear();
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
    T_CHECK(js.waiting > 0 && js.flushes == 0);
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


// A cycle across a draw on a legacy-deck board (a draw takes the draw stream's next number):
// the walk that codes it and the walk that decodes it each seed every apply as the drive did,
// so both draw the cards the drive drew, whatever the stream held before.
static void a_cycle_walk_draws_as_the_drive_did(void) {
    char path[320], err[256] = "";
    snprintf(path, sizeof path, "%s/walk.sqlite", g_dir);
    cb_store *s = open_world(path, false, err, sizeof err);
    T_CHECK(s != NULL);
    if (!s) return;
    const bool was = g_bots;
    g_bots = false;
    char gid[CB_GAME_ID_MAX + 1];
    two_humans_dealt(gid);
    g_bots = was;
    cb_game_row row = stored(s, gid);
    Game *g0 = calloc(1, sizeof *g0), *w = calloc(1, sizeof *w), *e = calloc(1, sizeof *e);
    TableSnaps *snaps = calloc(1, sizeof *snaps);
    LegalMoves *moves = calloc(1, sizeof *moves);
    Table t;
    table_init(&t, g0, snaps);
    T_EQ_INT(table_load(&t, row.state, (int)row.state_len, row.roster, (int)row.roster_len), TABLE_OK);
    g0->deterministic_deck = false;
    const uint32_t base = 0x5eed1234u, off = 7;
    int drew = 0, tries = 0;
    for (; !drew && tries < 50; tries++) {
        // A cycle of nine moves by whoever has one, applied as the drive applies (seeded).
        *w = *g0;
        BotDriveAction acts[BOT_DRIVE_MAX_ACTIONS], back[BOT_DRIVE_MAX_ACTIONS];
        ReplayMove mv[BOT_DRIVE_MAX_ACTIONS];
        memset(acts, 0, sizeof acts);
        int n = 0;
        for (; n < BOT_DRIVE_MAX_ACTIONS; n++) {
            int seat = -1, pick = -1;
            for (int p = 0; p < w->num_players && seat < 0; p++) {
                calculate_legal_moves(w, p, moves);
                int k = 0;
                for (int i = 0; i < moves->n; i++) k += moves->moves[i].type != MOVE_WAIT;
                if (!k) continue;
                int want = (int)(rng_u32(&g_rng) % (uint32_t)k);
                for (int i = 0; i < moves->n && pick < 0; i++)
                    if (moves->moves[i].type != MOVE_WAIT && !want--) pick = i;
                seat = p;
            }
            if (seat < 0) break;
            acts[n].seat = (int8_t)seat;
            acts[n].move = moves->moves[pick];
            const int deck = w->deck_count;
            bot_drive_seed_decision(w, base, off, BOT_DRIVE_PHASE_APPLY);
            T_CHECK(legal_move_apply(w, seat, &acts[n].move));
            if (w->deck_count < deck && n + 1 < BOT_DRIVE_MAX_ACTIONS) drew = 1;
            if (game_done(w) >= 0) { n++; break; }
        }
        if (!drew) continue;
        *e = *g0;
        (void)game_random();   // the stream holds something else now
        T_EQ_INT(replay_move_drive_walk(e, base, off, n, acts, mv, true), 0);
        for (int i = 0; i < 5; i++) (void)game_random();
        *e = *g0;
        T_EQ_INT(replay_move_drive_walk(e, base, off, n, back, mv, false), 0);
        bool same = true;
        for (int i = 0; i < n && same; i++) {
            same = back[i].seat == acts[i].seat && back[i].move.type == acts[i].move.type &&
                   back[i].move.n_cards == acts[i].move.n_cards;
            for (int c = 0; same && c < acts[i].move.n_cards; c++) same = card_eq(back[i].move.cards[c], acts[i].move.cards[c]);
        }
        T_CHECK(same);
        T_EQ_INT(e->deck_count, w->deck_count);
    }
    T_CHECK(drew);
    free(g0); free(w); free(e); free(snaps); free(moves);
    cb_game_row_free(&row);
    close_world(s);
}


// ---- mapped segments: a crash keeps every committed batch across rotations, and a batch
// whose commit marker (its header's magic, stored last) never landed is not there ----------

// The newest segment of `db` that holds a batch, and the offset of its last batch.
static bool last_batch(const char *db, char *seg, size_t cap, long *off) {
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
    uint8_t h[8];
    long at = 16, last = -1;
    while (f && fseek(f, at, SEEK_SET) == 0 && fread(h, 1, 8, f) == 8 && h[0] == 'J') {
        last = at;
        at += 8 + (long)(h[2] | h[3] << 8);
    }
    if (f) fclose(f);
    *off = last;
    return last > 0;
}

static void poke(const char *path, long off, uint8_t v) {
    FILE *f = fopen(path, "r+b");
    if (f) { fseek(f, off, SEEK_SET); fputc(v, f); fclose(f); }
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
    two_humans_dealt(gid);
    move_t m;
    int64_t v[32];
    uint64_t h[32];
    int n = 0;
    // Twelve steps at least, the last one a single commit: a bot drive may commit more than
    // one cycle in a step, and the cut below takes off only the last batch (one record).
    bool single = false;
    for (; n < 32 && !(n >= 12 && single) && (v[n] = step(s, gid, &m)) > 0; n++) {
        h[n] = row_hash(s, gid);
        single = n > 0 && v[n] == v[n - 1] + 1;
    }
    T_CHECK(n >= 12 && single);
    cb_journal_stats js;
    cb_journal_stats_get(s, &js);
    T_CHECK(js.segments >= 12);
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
    poke(seg, off + 4, 0x5a);
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
    ReplayMove mv;
    memset(&mv, 0, sizeof mv);
    mv.menu_n = 2;
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
        T_CHECK(up > 0 && up < 4096);
    }

    // The disk is lost (the live files stay where they are, unread): a restore beside them.
    snprintf(rpath, sizeof rpath, "%s/restored%d.sqlite", g_dir, g_bots);
    T_EQ_INT(cb_backup_restore(&blob, key, rpath), CB_OK);
    char marker[400];
    snprintf(marker, sizeof marker, "%s" CB_STORE_RESTORED_SUFFIX, rpath);
    T_CHECK(access(marker, F_OK) == 0);
    cb_backup_stop(bk);
    cb_store *live = s;
    // (one store at a time: the row cache is the process's)
    cb_journal_close(live);
    cb_row_cache_clear();
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
    T_RUN(a_torn_or_corrupt_tail_drops_only_itself);
    T_RUN(a_move_off_its_board_is_refused);
    T_RUN(a_legacy_deck_cycle_recovers_and_a_cut_one_is_refused);
    T_RUN(a_cycle_walk_draws_as_the_drive_did);
    for (int b = 0; b < 2; b++) {
        g_bots = b;
        T_RUN(the_flush_writes_the_rows_per_move_commits_write);
        T_RUN(a_restore_mid_game_holds_every_acknowledged_move);
        T_RUN(every_acknowledged_move_survives_a_sigkill);
        T_RUN(a_move_writes_no_table_until_its_game_is_due);
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

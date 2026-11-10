// unit_auth_sqlite.c - the platform's session routes (auth/auth_http.h) over the real local store,
// SQLite in a fresh file, every request through cb_handle: what core/test/unit_auth_http.c checks
// against the in-RAM store, here with the rows the store really keeps. The clock is auth's
// (auth.h cb_auth_now), moved by the test, so a session's lifetime, a spent token's grace and its
// keeping can be walked through without waiting for them.
//
// Held here, each as a client meets it:
//   - a refresh token is used once: presented again within the grace it is a retry, after it the
//     whole session ends, the newer token with it;
//   - a session past its absolute end stays over, however its last token is presented, an old
//     token with no end of its own and no session row too;
//   - a password change needs the current password, and ends every other session's tokens;
//   - a global sign-out ends every session, a refresh token of each refused after it;
//   - a deleted account's tokens and password are refused, and another account is untouched;
//   - a made-up name is counted and locked as a real one is (the lock tells nothing about whether
//     a name has an account), the right password clears a real one's count, and a made-up name's
//     row is swept once its time is up.
#include "t.h"
#include "cbase/app.h"
#include "cbase/provider.h"

#include "auth/auth.h"
#include "auth/auth_http.h"
#include "auth/auth_wire.h"
#include "handler.h"
#include "net/refuse.h"
#include "net/router.h"
#include "rate_limits.h"
#include "store/store.h"
#include "util/clock.h"
#include "util/err.h"
#include "util/log.h"
#include "util/str.h"

#include "../../../../shared/c/le_bytes.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static cb_config g_cfg;
static cb_ctx g_ctx;
static char g_db[512];
static const cb_app APP = { .name = "unit-auth-sqlite", .account_domain = "accounts.example" };

// Auth's clock, the test's to move.
static int64_t g_now;
static int64_t test_now(void) { return g_now; }
#define DAY (24LL * 3600 * 1000)

static void setup(void) {
    memset(&g_cfg, 0, sizeof g_cfg);
    strcpy(g_cfg.jwt_secret, "unit-auth-sqlite-secret-with-enough-bytes");
    strcpy(g_cfg.allowed_origins, "*");
    strcpy(g_cfg.public_url, "https://api.example/");
    g_cfg.jwt_ttl_s = 3600;
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_local;
    g_ctx.app = &APP;
    const char *tmp = getenv("TMPDIR");
    snprintf(g_db, sizeof g_db, "%s/cbase-auth-sqlite-%d.sqlite", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    unlink(g_db);
    T_EQ_INT(cb_store_open_sqlite(g_db, &g_ctx.store), CB_OK);
    T_EQ_INT(cb_auth_new(&g_ctx, &g_ctx.auth), CB_OK);
    g_ctx.router = cb_router_new();
    T_EQ_INT(cb_auth_routes(&g_ctx), CB_OK);
    cb_limits_reset();
    g_now = 1790000000000LL;   // 2026-09: a fixed start, the same every run
    cb_auth_now = test_now;
}

static void teardown(void) {
    cb_auth_now = cb_now_ms;
    cb_router_free(g_ctx.router);
    cb_auth_free(g_ctx.auth);
    cb_store_close(g_ctx.store);
    char side[600];
    unlink(g_db);
    snprintf(side, sizeof side, "%s-wal", g_db);
    unlink(side);
    snprintf(side, sizeof side, "%s-shm", g_db);
    unlink(side);
    memset(&g_ctx, 0, sizeof g_ctx);
}

// ---- one request (as unit_auth_http.c sends them) -------------------------------------------

typedef struct {
    int     status;
    uint8_t body[4096];
    size_t  len;
} answer;
static answer g_a;

static int post_raw(const char *path, const char *bearer, const uint8_t *body, size_t n) {
    cb_http_req r;
    memset(&r, 0, sizeof r);
    strcpy(r.method, "POST");
    r.path = path;
    r.query = "";
    r.minor = 1;
    char authz[CBW_ACCESS_MAX + 16];
    if (bearer) {
        snprintf(authz, sizeof authz, "Bearer %s", bearer);
        r.headers[r.n_headers++] = (cb_http_header){ "Authorization", authz };
    }
    r.body = body;
    r.body_len = n;
    cb_http_res res;
    cb_res_init(&res);
    cb_handle(&g_ctx, &r, &res);
    memset(&g_a, 0, sizeof g_a);
    g_a.status = res.status;
    g_a.len = res.body.len < sizeof g_a.body ? res.body.len : 0;
    if (g_a.len) memcpy(g_a.body, res.body.p, g_a.len);
    cb_res_free(&res);
    return g_a.status;
}

typedef struct {
    uint8_t b[600];
    size_t  n;
} body;
static void put8(body *b, uint8_t v) { b->b[b->n++] = v; }
static void put_str8(body *b, const char *s) {
    size_t n = strlen(s);
    put8(b, (uint8_t)n);
    memcpy(b->b + b->n, s, n);
    b->n += n;
}
static body creds(uint8_t kind, const char *name, const char *pw) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, kind);
    put_str8(&b, name);
    put_str8(&b, pw);
    return b;
}
static body one_str(uint8_t kind, const char *s) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, kind);
    put_str8(&b, s);
    return b;
}
static body scope(uint8_t sc) {
    body b = { .n = 0 };
    put8(&b, CBW_V);
    put8(&b, CBW_SIGNOUT);
    put8(&b, sc);
    return b;
}
static int post(const char *path, const char *bearer, body b) { return post_raw(path, bearer, b.b, b.n); }

// The last answer is the account refusal `code`.
static bool refused(int code) {
    uint8_t want[CBW_REFUSAL_BYTES] = { CBW_REFUSAL_FORMAT, CBW_WHY_ACCOUNT };
    le_put_u32(want + 2, (uint32_t)code);
    return g_a.len == CBW_REFUSAL_BYTES && memcmp(g_a.body, want, CBW_REFUSAL_BYTES) == 0;
}

typedef struct {
    char access[CBW_ACCESS_MAX + 1], refresh[CBW_REFRESH_MAX + 1], user_id[CBW_ID_MAX + 1];
} session;
static bool read_session(session *s) {
    memset(s, 0, sizeof *s);
    const uint8_t *p = g_a.body;
    size_t n = g_a.len, at = 4;
    if (g_a.status != 200 || n < 4 || p[0] != CBW_V || p[1] != CBW_SESSION) return false;
    size_t an = le_get_u16(p + 2);
    if (an > CBW_ACCESS_MAX || at + an + 1 > n) return false;
    memcpy(s->access, p + at, an);
    at += an;
    size_t rn = p[at++];
    if (rn > CBW_REFRESH_MAX || at + rn + 6 + 1 > n) return false;
    memcpy(s->refresh, p + at, rn);
    at += rn + 6;
    size_t un = p[at++];
    if (un > CBW_ID_MAX || at + un > n) return false;
    memcpy(s->user_id, p + at, un);
    return true;
}

#define SU "/v1/auth/signup"
#define SI "/v1/auth/signin"
#define RF "/v1/auth/refresh"
#define SO "/v1/auth/signout"
#define PW "/v1/auth/password"

static bool refresh(const char *token, session *out) {
    return post(RF, NULL, one_str(CBW_REFRESH, token)) == 200 && read_session(out);
}

// ---- the cases --------------------------------------------------------------------------------

static void a_token_is_used_once(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "ANNA", "correct-horse-1")), 200);
    session a, b, c;
    T_CHECK(read_session(&a));
    g_now += 1000;
    T_CHECK(refresh(a.refresh, &b));
    T_CHECK(strcmp(b.refresh, a.refresh) != 0);
    // The old token again within the grace (a lost answer's retry): a new pair, same account.
    g_now += CB_AUTH_REUSE_GRACE_MS - 1000;
    T_CHECK(refresh(a.refresh, &c));
    T_EQ_STR(c.user_id, a.user_id);
    // After the grace: reuse. The session ends, its newest tokens with it.
    g_now += 2000;
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, a.refresh)), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_REFRESH_USED));
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, b.refresh)), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_REFRESH_USED));
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, c.refresh)), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_REFRESH_USED));
    // Another session of the same account is not this one: it refreshes.
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "ANNA", "correct-horse-1")), 200);
    session d, e;
    T_CHECK(read_session(&d));
    T_CHECK(refresh(d.refresh, &e));
    teardown();
}

// A session's end is absolute (auth.h CB_AUTH_SESSION_MS): its last token, presented past it,
// is refused, the second time as the first.
static void a_session_past_its_end_stays_over(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "BEN", "correct-horse-1")), 200);
    session a, b, c;
    T_CHECK(read_session(&a));
    // Kept alive by refreshes until near its end: the last token minted a day before it.
    g_now += CB_AUTH_SESSION_MS - DAY;
    T_CHECK(refresh(a.refresh, &b));
    g_now += 2 * DAY;
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, b.refresh)), 400);
        T_CHECK(refused(CBW_ACCOUNT_E_BAD_REFRESH));
        g_now += 1000;
    }
    T_CHECK(!refresh(b.refresh, &c));
    // Signing in again is a new session.
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "BEN", "correct-horse-1")), 200);
    T_CHECK(read_session(&c));
    teardown();
}

// A token from before sessions were stored: no end of its own (expires_at 0) and no session row.
// The store cannot refuse it by its time, so auth.c's session end, counted from the token's own
// created_at, is the only thing that does. Written into the file as such a row would be found.
static void old_rows_lose_their_session_row(void) {
    sqlite3 *db = NULL;
    T_EQ_INT(sqlite3_open(g_db, &db), SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    T_EQ_INT(sqlite3_exec(db, "UPDATE refresh_tokens SET expires_at = 0; DELETE FROM sessions;", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
}

static void an_old_token_without_an_end_stops_at_its_sessions_end(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "HUGO", "correct-horse-1")), 200);
    session a, b, c;
    T_CHECK(read_session(&a));
    // Within its session's end: the session row is made from the token, and it refreshes.
    old_rows_lose_their_session_row();
    g_now += DAY;
    T_CHECK(refresh(a.refresh, &b));
    // Past it: refused, however often presented.
    old_rows_lose_their_session_row();
    g_now += CB_AUTH_SESSION_MS;
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, b.refresh)), 400);
        T_CHECK(refused(CBW_ACCOUNT_E_BAD_REFRESH));
        g_now += 1000;
    }
    T_CHECK(!refresh(b.refresh, &c));
    teardown();
}

static void a_password_change_needs_the_password_and_ends_the_others(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "CORA", "correct-horse-1")), 200);
    session a, b, c;
    T_CHECK(read_session(&a));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "CORA", "correct-horse-1")), 200);
    T_CHECK(read_session(&b));
    // Without the current password: refused, nothing changed, both sessions live.
    T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "wrong-horse-1", "correct-horse-9")), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_BAD_LOGIN));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "CORA", "correct-horse-9")), 400);
    g_now += 1000;
    T_CHECK(refresh(b.refresh, &c));
    strcpy(b.refresh, c.refresh);
    strcpy(b.access, c.access);
    // With it: changed; every other session over, the caller's not.
    cb_limits_reset();
    T_EQ_INT(post(PW, a.access, creds(CBW_PASSWORD, "correct-horse-1", "correct-horse-9")), 204);
    g_now += 1000;
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, b.refresh)), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_REFRESH_USED));
    T_EQ_INT(post(SO, b.access, scope(CBW_SCOPE_LOCAL)), 401);
    T_CHECK(refresh(a.refresh, &c));
    // The old password no longer signs in; the new one does.
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "CORA", "correct-horse-1")), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_BAD_LOGIN));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "CORA", "correct-horse-9")), 200);
    teardown();
}

static void a_global_sign_out_ends_every_session(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "DORA", "correct-horse-1")), 200);
    session s[3], n;
    T_CHECK(read_session(&s[0]));
    for (int i = 1; i < 3; i++) {
        T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "DORA", "correct-horse-1")), 200);
        T_CHECK(read_session(&s[i]));
    }
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "EDDA", "correct-horse-1")), 200);
    session other;
    T_CHECK(read_session(&other));
    T_EQ_INT(post(SO, s[1].access, scope(CBW_SCOPE_GLOBAL)), 204);
    g_now += 1000;
    for (int i = 0; i < 3; i++) {
        T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, s[i].refresh)), 400);
        T_CHECK(refused(CBW_ACCOUNT_E_REFRESH_USED));
        T_EQ_INT(post(SO, s[i].access, scope(CBW_SCOPE_LOCAL)), 401);
    }
    // Another account's session is not touched.
    T_CHECK(refresh(other.refresh, &n));
    // A sign-in after it is a live session.
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "DORA", "correct-horse-1")), 200);
    T_CHECK(read_session(&n));
    session m;
    g_now += 1000;
    T_CHECK(refresh(n.refresh, &m));
    teardown();
}

static void a_deleted_account_is_gone(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "FAYE", "correct-horse-1")), 200);
    session a, b, other, n;
    T_CHECK(read_session(&a));
    g_now += 1000;
    T_CHECK(refresh(a.refresh, &b));
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "GIL", "correct-horse-1")), 200);
    T_CHECK(read_session(&other));
    T_EQ_INT(cb_auth_delete_user(g_ctx.auth, a.user_id), CB_OK);
    g_now += 1000;
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, b.refresh)), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_BAD_REFRESH));
    T_EQ_INT(post(RF, NULL, one_str(CBW_REFRESH, a.refresh)), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_BAD_REFRESH));
    T_EQ_INT(post(SO, b.access, scope(CBW_SCOPE_LOCAL)), 401);
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "FAYE", "correct-horse-1")), 400);
    T_CHECK(refused(CBW_ACCOUNT_E_BAD_LOGIN));
    cb_user_row row;
    T_EQ_INT(CB_STORE_CALL(g_ctx.store, user_get, a.user_id, &row), CB_E_NOT_FOUND);
    T_EQ_INT(cb_auth_delete_user(g_ctx.auth, a.user_id), CB_E_NOT_FOUND);
    // GIL is untouched; FAYE's name can be signed up again, as a new account.
    T_CHECK(refresh(other.refresh, &n));
    T_EQ_INT(post(SI, NULL, creds(CBW_SIGNIN, "GIL", "correct-horse-1")), 200);
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "FAYE", "correct-horse-2")), 200);
    T_CHECK(read_session(&n));
    T_CHECK(strcmp(n.user_id, a.user_id) != 0);
    teardown();
}

// ---- guessing a password (auth.h CB_AUTH_FAIL_*) ----------------------------------------------

// The rows signin_fails holds, read from the file itself.
static int fails_rows(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    int n = -1;
    if (sqlite3_open(g_db, &db) == SQLITE_OK && sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM signin_fails", -1, &s, NULL) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(db);
    return n;
}

// One sign-in at `at`, the in-memory limits (rate_limits.h) out of the way: the account's own count
// is what is under test. The answer's status, and its account refusal code (-1: none).
static int sign_in_at(int64_t at, const char *name, const char *pw, int *why) {
    cb_limits_reset();
    g_now = at;
    int st = post(SI, NULL, creds(CBW_SIGNIN, name, pw));
    *why = g_a.len == CBW_REFUSAL_BYTES && g_a.body[1] == CBW_WHY_ACCOUNT ? (int)le_get_u32(g_a.body + 2) : -1;
    return st;
}

// The same guesses at the same times from t0: six wrong passwords (the sixth locks for
// CB_AUTH_LOCK_BASE_MS), the right one and a wrong one inside the lock, a wrong one at its end
// (the lock doubles), and one just inside the doubled lock.
#define WALK (CB_AUTH_FAIL_FREE + 6)
static void walk(int64_t t0, const char *name, int st[WALK], int why[WALK]) {
    int k = 0;
    for (int i = 0; i <= CB_AUTH_FAIL_FREE; i++, k++) st[k] = sign_in_at(t0 + 1000 * (i + 1), name, "wrong-horse-1", &why[k]);
    const int64_t locked = t0 + 1000 * (CB_AUTH_FAIL_FREE + 1);
    st[k] = sign_in_at(locked + 1000, name, "correct-horse-1", &why[k]), k++;
    st[k] = sign_in_at(locked + CB_AUTH_LOCK_BASE_MS - 1, name, "wrong-horse-1", &why[k]), k++;
    st[k] = sign_in_at(locked + CB_AUTH_LOCK_BASE_MS, name, "wrong-horse-1", &why[k]), k++;
    st[k] = sign_in_at(locked + 3 * CB_AUTH_LOCK_BASE_MS - 1, name, "correct-horse-1", &why[k]), k++;
    st[k] = sign_in_at(locked + 3 * CB_AUTH_LOCK_BASE_MS, name, "wrong-horse-1", &why[k]), k++;
}

// The lock tells nothing about whether a name has an account: a made-up name gets the answers a
// real one does, at the same times, and a row as the real one does.
static void a_made_up_name_locks_as_a_real_one_does(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "IVY", "correct-horse-1")), 200);
    const int64_t t0 = g_now;
    int st[2][WALK], why[2][WALK];
    walk(t0, "IVY", st[0], why[0]);
    walk(t0, "NOBODY", st[1], why[1]);
    const int want[WALK] = { 400, 400, 400, 400, 400, 400, 429, 429, 400, 429, 400 };
    for (int k = 0; k < WALK; k++) {
        T_EQ_INT(st[0][k], want[k]);
        T_EQ_INT(why[0][k], want[k] == 429 ? CBW_ACCOUNT_E_TOO_MANY : CBW_ACCOUNT_E_BAD_LOGIN);
        T_EQ_INT(st[1][k], st[0][k]);
        T_EQ_INT(why[1][k], why[0][k]);
    }
    T_EQ_INT(fails_rows(), 2);
    teardown();
}

// The right password clears an account's count: after it, five wrong ones are free again.
static void the_right_password_clears_the_count(void) {
    setup();
    T_EQ_INT(post(SU, NULL, creds(CBW_SIGNUP, "JADE", "correct-horse-1")), 200);
    int64_t t = g_now;
    int why;
    for (int i = 0; i < CB_AUTH_FAIL_FREE; i++) T_EQ_INT(sign_in_at(t += 1000, "JADE", "wrong-horse-1", &why), 400);
    T_EQ_INT(fails_rows(), 1);
    T_EQ_INT(sign_in_at(t += 1000, "JADE", "correct-horse-1", &why), 200);
    T_EQ_INT(fails_rows(), 0);
    for (int i = 0; i < CB_AUTH_FAIL_FREE + 1; i++) T_EQ_INT(sign_in_at(t += 1000, "JADE", "wrong-horse-1", &why), 400);
    T_EQ_INT(sign_in_at(t += 1000, "JADE", "correct-horse-1", &why), 429);
    T_EQ_INT(why, CBW_ACCOUNT_E_TOO_MANY);
    teardown();
}

// A made-up name's row goes when its time does (store.h cb_signin_fails: an hour after its last
// write): any later failed sign-in sweeps it, so names nobody has do not pile up in the file.
#define FAILS_KEPT_MS (3600LL * 1000)
static void a_made_up_names_row_is_swept_after_its_time(void) {
    setup();
    int64_t t = g_now;
    int why;
    T_EQ_INT(sign_in_at(t, "JUNK1", "wrong-horse-1", &why), 400);
    T_EQ_INT(sign_in_at(t + 1000, "JUNK2", "wrong-horse-1", &why), 400);
    T_EQ_INT(fails_rows(), 2);
    // JUNK1's hour is up, JUNK2's not yet: one goes, the other stays, and JUNK3's is new.
    T_EQ_INT(sign_in_at(t + FAILS_KEPT_MS, "JUNK3", "wrong-horse-1", &why), 400);
    T_EQ_INT(fails_rows(), 2);
    T_EQ_INT(sign_in_at(t + FAILS_KEPT_MS + 1000, "JUNK3", "wrong-horse-1", &why), 400);
    T_EQ_INT(fails_rows(), 1);
    teardown();
}

int main(void) {
    T_RUN(a_token_is_used_once);
    T_RUN(a_session_past_its_end_stays_over);
    T_RUN(an_old_token_without_an_end_stops_at_its_sessions_end);
    T_RUN(a_password_change_needs_the_password_and_ends_the_others);
    T_RUN(a_global_sign_out_ends_every_session);
    T_RUN(a_deleted_account_is_gone);
    T_RUN(a_made_up_name_locks_as_a_real_one_does);
    T_RUN(the_right_password_clears_the_count);
    T_RUN(a_made_up_names_row_is_swept_after_its_time);
    return T_DONE();
}

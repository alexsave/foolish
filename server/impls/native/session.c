// Accounts and session tokens - see session.h.
#define _GNU_SOURCE
#include "session.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ctl_wire.h"
#include "persist.h"
#include "sha256.h"     // Bucket A: HMAC-SHA256 for stateless signed session tokens
#include "snapshot.h"   // g_user_table: a new account is persisted write-behind

#define TOKEN_TTL_S   (7 * 24 * 3600)          // 7 days
#define TOKEN_PAYLOAD (ID_LEN + 8 + 32)
static uint8_t g_token_secret[32];

static void hmac_sha256(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    Sha256 c; uint8_t inner[32];
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

static const char B64U[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static int b64url_encode(const uint8_t *in, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int rem = n - i;
        int v = (in[i] << 16) | ((rem > 1 ? in[i + 1] : 0) << 8) | (rem > 2 ? in[i + 2] : 0);
        int chunk = rem > 2 ? 4 : rem + 1;
        for (int j = 0; j < chunk; j++) { if (o >= cap - 1) return -1; out[o++] = B64U[(v >> (18 - 6 * j)) & 0x3f]; }
    }
    out[o] = 0; return o;
}
static int b64url_decode(const char *in, uint8_t *out, int cap) {
    int8_t rev[256]; memset(rev, -1, sizeof rev);
    for (int i = 0; i < 64; i++) rev[(unsigned char)B64U[i]] = (int8_t)i;
    int o = 0, bits = 0; unsigned v = 0;
    for (const char *p = in; *p; p++) {
        int8_t d = rev[(unsigned char)*p]; if (d < 0) return -1;
        v = (v << 6) | (unsigned)d; bits += 6;   // unsigned: defined even if it wrapped; masked below so it can't
        if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)(v >> bits); v &= (1u << bits) - 1u; }
    }
    return o;
}

void make_token(const char *user_id, char *out, int cap) {
    uint8_t p[TOKEN_PAYLOAD];
    memcpy(p, user_id, ID_LEN);
    uint64_t exp = (uint64_t)time(NULL) + TOKEN_TTL_S;
    for (int i = 0; i < 8; i++) p[ID_LEN + i] = (uint8_t)(exp >> (56 - 8 * i));
    hmac_sha256(g_token_secret, 32, p, ID_LEN + 8, p + ID_LEN + 8);
    b64url_encode(p, TOKEN_PAYLOAD, out, cap);
}

bool verify_token(const char *token, char *user_id_out) {
    uint8_t p[TOKEN_PAYLOAD];
    if (b64url_decode(token, p, sizeof p) != TOKEN_PAYLOAD) return false;
    uint8_t mac[32];
    hmac_sha256(g_token_secret, 32, p, ID_LEN + 8, mac);
    uint8_t diff = 0; for (int i = 0; i < 32; i++) diff |= (uint8_t)(mac[i] ^ p[ID_LEN + 8 + i]);
    if (diff) return false;
    uint64_t exp = 0; for (int i = 0; i < 8; i++) exp = (exp << 8) | p[ID_LEN + i];
    if ((uint64_t)time(NULL) >= exp) return false;
    memcpy(user_id_out, p, ID_LEN); user_id_out[ID_LEN] = 0;
    return true;
}

void token_secret_init(void) {
    const char *env = getenv("FOOLISH_TOKEN_SECRET");
    bool ok = env && strlen(env) >= 64;
    for (int i = 0; ok && i < 32; i++) {
        char a = env[2 * i], b = env[2 * i + 1];
        int hi = (a >= '0' && a <= '9') ? a - '0' : (a | 32) >= 'a' && (a | 32) <= 'f' ? (a | 32) - 'a' + 10 : -1;
        int lo = (b >= '0' && b <= '9') ? b - '0' : (b | 32) >= 'a' && (b | 32) <= 'f' ? (b | 32) - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) ok = false; else g_token_secret[i] = (uint8_t)((hi << 4) | lo);
    }
    if (!ok) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0 || read(fd, g_token_secret, 32) != 32)
            for (int i = 0; i < 32; i++) g_token_secret[i] = (uint8_t)(i * 2654435761u);   // last-ditch, never expected
        if (fd >= 0) close(fd);
        fprintf(stderr, "warning: FOOLISH_TOKEN_SECRET unset - using a random per-process token secret "
                        "(sessions won't survive a restart or span instances)\n");
    }
}

User *user_by_token(const char *token) {
    if (!token || !*token) return NULL;
    char user_id[ID_LEN + 1];
    if (!verify_token(token, user_id)) return NULL;   // bad signature or expired
    return user_by_id(user_id);                        // token is authentic; find the user it names
}

void h_signup(Req *r, Conn *conn) {
    if (!ratelimit_allow(client_ip_key(r, conn))) { respond_ctl_error(conn, 429, CTL_ERR_RATE_LIMITED); return; }
    // A username too long for User.username (24 bytes incl. the NUL) is now
    // REFUSED by the decoder rather than silently truncated. The JSON scraper
    // this replaced cut it to 23 characters and signed the account up under
    // the stub - and since dedup is BY username, two different people whose
    // names shared a 23-character prefix landed on the SAME account. The
    // kernel's roster is the looser side here (ROSTER_NAME_MAX is 64); this
    // server's field has always been the tighter one, so this is the one place
    // the packed wire refuses something the scraper used to repair.
    char uname[24] = {0};
    if (!ctl_dec_auth((const unsigned char *)r->body, r->body_len, uname, sizeof uname) || !roster_name_ok(uname)) {
        respond_ctl_error(conn, 400, CTL_ERR_USERNAME); return;
    }
    pthread_mutex_lock(&g_registry_lock);
    // Dedup by username via the O(1) hash (was an O(users) linear scan).
    User *u = user_by_username(uname);
    if (!u) {
        int idx = g_users_count;
        u = (idx < MAX_USERS) ? user_slot_ensure(idx) : NULL;
        if (u) { g_users_count++; u->slot_idx = idx; u->used = true;
                 snprintf(u->username, sizeof u->username, "%s", uname); gen_id(u->user_id, ID_LEN);
                 username_ht_insert(u); userid_ht_insert(u);
                 // Persist the NEW user (Stage 2 write-behind). There is no token
                 // to store - it's a stateless signed blob (see make_token).
                 if (g_user_table) persist_mark_dirty(g_user_table, u->slot_idx); }
    }
    // A fresh signed session token: nothing to store or index - it self-validates
    // by HMAC and carries the user_id + an absolute expiry (see make_token).
    char token[96] = "";
    if (u) make_token(u->user_id, token, sizeof token);
    unsigned char out[CTL_FRAME_MAX];
    int n = 0;
    if (u) n = ctl_enc_session(u->user_id, u->username, token, out, (int)sizeof out);
    pthread_mutex_unlock(&g_registry_lock);
    if (u && n > 0) respond_ctl(conn, 200, out, n);
    else            respond_ctl_error(conn, 400, CTL_ERR_FULL);
}

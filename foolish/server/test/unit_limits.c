// unit_limits.c - the in-memory rate limits (rate_limits.h): a bucket's burst
// and refill, keys and kinds kept apart, the table's fixed memory under a
// flood of keys, and the viewer address a limit is keyed on.
#include "t.h"
#include "cbase/provider.h"

#include "config.h"
#include "rate_limits.h"

static void burst_then_refill(void) {
    cb_limits_reset();
    const cb_limit_rule *r = &cb_limit_rules[CB_LIMIT_SIGNIN_IP];
    const int64_t t0 = 1000000;
    for (int i = 0; i < r->burst; i++) T_CHECK(cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", t0, NULL));
    int retry = -1;
    T_CHECK(!cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", t0, &retry));
    // One token comes back every window/burst ms: the retry says when.
    const int64_t per = r->window_ms / r->burst;
    T_EQ_INT(retry, (int)(per / 1000) + 1);
    T_CHECK(!cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", t0 + per - 1, NULL));
    T_CHECK(cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", t0 + per, NULL));
    T_CHECK(!cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", t0 + per, NULL));
    // A whole window refills to the burst, never past it.
    int64_t later = t0 + 10 * r->window_ms;
    for (int i = 0; i < r->burst; i++) T_CHECK(cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", later, NULL));
    T_CHECK(!cb_limit_take(CB_LIMIT_SIGNIN_IP, "198.51.100.1", later, NULL));
}

static void keys_and_kinds_are_apart(void) {
    cb_limits_reset();
    const int b = cb_limit_rules[CB_LIMIT_SIGNUP_IP].burst;
    for (int i = 0; i < b; i++) T_CHECK(cb_limit_take(CB_LIMIT_SIGNUP_IP, "a", 0, NULL));
    T_CHECK(!cb_limit_take(CB_LIMIT_SIGNUP_IP, "a", 0, NULL));
    T_CHECK(cb_limit_take(CB_LIMIT_SIGNUP_IP, "b", 0, NULL));   // another address
    T_CHECK(cb_limit_take(CB_LIMIT_SIGNIN_IP, "a", 0, NULL));   // another kind, same key
    // No key: a request with no address (the tests in-process) is not limited.
    for (int i = 0; i < 3 * b; i++) T_CHECK(cb_limit_take(CB_LIMIT_SIGNUP_IP, "", 0, NULL));
    T_CHECK(cb_limit_take(CB_LIMIT_SIGNUP_IP, NULL, 0, NULL));
    T_CHECK(cb_limit_take(CB_LIMIT_KINDS, "a", 0, NULL));       // not a kind: nothing to count
}

// A flood of distinct keys cannot grow the table: an evicted bucket is
// forgiven, but a key in use the whole time keeps its count.
static void a_flood_of_keys_stays_bounded(void) {
    cb_limits_reset();
    const int b = cb_limit_rules[CB_LIMIT_CHAT_USER].burst;
    for (int i = 0; i < b; i++) T_CHECK(cb_limit_take(CB_LIMIT_CHAT_USER, "victim", 100, NULL));
    char key[32];
    for (int i = 0; i < 100000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        cb_limit_take(CB_LIMIT_CHAT_USER, key, 100 + i / 1000, NULL);
        // The busy key is heard from all along, so it is never the oldest.
        if (i % 50 == 0) cb_limit_take(CB_LIMIT_CHAT_USER, "victim", 100 + i / 1000, NULL);
    }
    T_CHECK(!cb_limit_take(CB_LIMIT_CHAT_USER, "victim", 200, NULL));
}

static void viewer_key(const char *mode_header, const cb_provider *mode, const char *peer, const char *want) {
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.prov = mode;
    cb_http_req req;
    memset(&req, 0, sizeof req);
    if (mode_header) req.headers[req.n_headers++] = (cb_http_header){ "CloudFront-Viewer-Address", mode_header };
    if (peer) cb_strlcpy(req.peer, peer, sizeof req.peer);
    char out[96];
    cb_viewer_key(&ctx, &req, out, sizeof out);
    T_EQ_STR(out, want);
}

static void the_viewer_address(void) {
    // Under Lambda: CloudFront's header, without its port.
    viewer_key("203.0.113.7:50238", &cb_provider_aws, "130.176.0.1", "203.0.113.7");
    // IPv6: cut to the /64, with or without brackets, with or without a port.
    viewer_key("2001:db8:1:2:3:4:5:6:443", &cb_provider_aws, NULL, "2001:db8:1:2::/64");
    viewer_key("[2001:db8:1:2::9]:443", &cb_provider_aws, NULL, "2001:db8:1:2::/64");
    viewer_key("2001:db8:1:2:ffff::1", &cb_provider_aws, NULL, "2001:db8:1:2::/64");
    // No header: the source address.
    viewer_key(NULL, &cb_provider_aws, "198.51.100.9", "198.51.100.9");
    // Under serve the socket's peer, whatever a client puts in the header.
    viewer_key("203.0.113.7:1", &cb_provider_local, "127.0.0.1", "127.0.0.1");
    // Nothing known: "" (not limited).
    viewer_key(NULL, &cb_provider_local, NULL, "");
    // Text that is no address is still a key, as sent.
    viewer_key("not-an-address", &cb_provider_aws, NULL, "not-an-address");
}

// Behind CloudFront under serve: the header counts only with the origin secret.
static void viewer_key_serve(const char *secret_cfg, const char *secret_sent, const char *addr, const char *xff,
                             const char *want) {
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    if (secret_cfg) cb_strlcpy(cfg.origin_secret, secret_cfg, sizeof cfg.origin_secret);
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.prov = &cb_provider_local;
    ctx.cfg = &cfg;
    cb_http_req req;
    memset(&req, 0, sizeof req);
    if (secret_sent) req.headers[req.n_headers++] = (cb_http_header){ CB_ORIGIN_HEADER, secret_sent };
    if (addr) req.headers[req.n_headers++] = (cb_http_header){ CB_VIEWER_HEADER, addr };
    if (xff) req.headers[req.n_headers++] = (cb_http_header){ "X-Forwarded-For", xff };
    cb_strlcpy(req.peer, "130.176.0.1", sizeof req.peer);
    char out[96];
    cb_viewer_key(&ctx, &req, out, sizeof out);
    T_EQ_STR(out, want);
}

static void the_viewer_address_behind_cloudfront(void) {
    viewer_key_serve("s3cret", "s3cret", "203.0.113.7:50238", NULL, "203.0.113.7");
    viewer_key_serve("s3cret", "s3cret", "[2001:db8:1:2::9]:443", NULL, "2001:db8:1:2::/64");
    // Wrong or no secret: the peer, whatever the header says.
    viewer_key_serve("s3cret", "wrong!", "203.0.113.7:50238", NULL, "130.176.0.1");
    viewer_key_serve("s3cret", NULL, "203.0.113.7:50238", NULL, "130.176.0.1");
    // No secret configured: the header is anyone's, so it is not read.
    viewer_key_serve(NULL, NULL, "203.0.113.7:50238", NULL, "130.176.0.1");
    // Malformed: the peer. X-Forwarded-For is never read.
    viewer_key_serve("s3cret", "s3cret", "203.0.113.7", NULL, "130.176.0.1");
    viewer_key_serve("s3cret", "s3cret", "not-an-address:1", NULL, "130.176.0.1");
    viewer_key_serve("s3cret", "s3cret", NULL, "203.0.113.99", "130.176.0.1");
    viewer_key_serve("s3cret", "s3cret", "203.0.113.7:50238", "203.0.113.99", "203.0.113.7");
}

int main(void) {
    T_RUN(burst_then_refill);
    T_RUN(keys_and_kinds_are_apart);
    T_RUN(a_flood_of_keys_stays_bounded);
    T_RUN(the_viewer_address);
    T_RUN(the_viewer_address_behind_cloudfront);
    return T_DONE();
}

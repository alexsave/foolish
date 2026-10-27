// misses.c - the guessed-id rule (misses.h).
#include "misses.h"

#include "net/refuse.h"
#include "rate_limits.h"
#include "util/clock.h"
#include "util/log.h"

#define KEY_MAX 96

bool cb_miss_may_look(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char key[KEY_MAX];
    cb_viewer_key(ctx, req, key, sizeof key);
    int retry;
    if (cb_limit_peek(CB_LIMIT_ID_MISS_IP, key, cb_now_ms(), &retry)) return true;
    cb_refuse_limited(res, retry, false);
    return false;
}

bool cb_miss_take(const cb_ctx *ctx, const cb_http_req *req, cb_http_res *res) {
    char key[KEY_MAX];
    cb_viewer_key(ctx, req, key, sizeof key);
    const int64_t now = cb_now_ms();
    int retry;
    if (!cb_limit_take(CB_LIMIT_ID_MISS_IP, key, now, &retry)) {
        cb_refuse_limited(res, retry, false);
        return false;
    }
    // One line when an address runs out, not one for every request it sends after.
    if (!cb_limit_peek(CB_LIMIT_ID_MISS_IP, key, now, NULL))
        CB_INFO("v1", "rate limit %s: %s", cb_limit_rules[CB_LIMIT_ID_MISS_IP].name, key);
    return true;
}

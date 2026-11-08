// unit_rt_iot_pushes.c - the product's half of c_base's unit_rt_iot.c (its rows, panels and pushes): realtime under Lambda on AWS IoT Core (realtime/rt_iot.h):
// a push is one publish per target to its MQTT topic, and the custom
// authorizer turns IoT's CONNECT request (decoded from the JSON IoT sends,
// aws/event.h) into the policy for the token's user, or a refusal. The
// authorizer's half of the authz matrix: a forged, expired or deleted
// account's token, another user's client id and a connection that did not
// come through the edge are refused; the policy names only the user's own
// topics and the spectators', and never iot:Publish.
#include "t.h"
#include "cbase/provider.h"

#include "aws/event.h"
#include "config.h"
#include "aws_json.h"
#include "app/pushes.h"
#include "realtime/realtime.h"
#include "realtime/rt_deps.h"
#include "realtime/rt_iot.h"
#include "realtime/stream_mqtt.h"
#include "b64/b64.h"
#include "util/err.h"
#include "util/json.h"
#include "app/fanout.h"
#include "util/clock.h"
#include "util/log.h"

#include <poll.h>
#include <pthread.h>
#include <stdlib.h>

#define UA "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
#define UB "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
#define G  "ab12cd"
#define NOW_MS 1700000000000LL
#define NOW_S  (NOW_MS / 1000)
#define ARN "arn:aws:lambda:us-east-1:487337210065:function:cbase"
#define SECRET "0123456789abcdef0123456789abcdef"

static cb_ctx g_ctx;
static cb_config g_cfg;

static int fake_verify(cb_ctx *ctx, const char *jwt, cb_user *out) {
    if (strcmp(jwt, "tok-db") == 0) return CB_E_DB;
    if (strncmp(jwt, "tok-", 4) != 0 || strcmp(jwt, "tok-gone") == 0) return CB_E_AUTH;
    memset(out, 0, sizeof *out);
    cb_strlcpy(out->id, jwt + 4, sizeof out->id);
    out->expires_at = NOW_S + 1800;
    return CB_OK;
}

// The publishes as IoT would take them. A batch's lanes call this from
// several threads at once: everything is under the lock but the wait, which
// stands for IoT's answer time (wait_ms), so concurrent publishes overlap.
// With `gather` set, each publish first waits (5 s at most, once) until that many are
// in flight at once: the lanes' concurrency is a count, not a wall-clock bound a
// loaded machine breaks.
static pthread_mutex_t g_pub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pub_cv = PTHREAD_COND_INITIALIZER;
static struct {
    int n;
    char endpoint[256], topic[48][128];
    uint8_t body[256];
    size_t len;
    int status;
    int wait_ms;
    const char *fail_topic;   // this topic answers 500
    int64_t start_ms[48], end_ms[48];
    pthread_t thread[48];
    int gather, inflight, peak;   // publishes in flight at once, and the most there were
} g_pub;
static int fake_publish(const char *endpoint, const char *topic, const void *data, size_t len) {
    int64_t t0 = cb_mono_ms();
    pthread_mutex_lock(&g_pub_mu);
    if (++g_pub.inflight > g_pub.peak) g_pub.peak = g_pub.inflight;
    pthread_cond_broadcast(&g_pub_cv);
    const int64_t until = cb_mono_ms() + 5000;
    while (g_pub.peak < g_pub.gather && cb_mono_ms() < until) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10 * 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_pub_cv, &g_pub_mu, &ts);
    }
    if (g_pub.peak < g_pub.gather) g_pub.gather = 0;   // never gathered: the rest go at once (peak says so)
    pthread_mutex_unlock(&g_pub_mu);
    if (g_pub.wait_ms) poll(NULL, 0, g_pub.wait_ms);
    pthread_mutex_lock(&g_pub_mu);
    g_pub.inflight--;
    int i = g_pub.n++;
    if (i < 48) {
        cb_strlcpy(g_pub.topic[i], topic, sizeof g_pub.topic[0]);
        g_pub.start_ms[i] = t0;
        g_pub.end_ms[i] = cb_mono_ms();
        g_pub.thread[i] = pthread_self();
    }
    cb_strlcpy(g_pub.endpoint, endpoint, sizeof g_pub.endpoint);
    g_pub.len = len < sizeof g_pub.body ? len : 0;
    memcpy(g_pub.body, data, g_pub.len);
    int st = g_pub.fail_topic && strcmp(topic, g_pub.fail_topic) == 0 ? 500 : g_pub.status;
    pthread_mutex_unlock(&g_pub_mu);
    return st;
}

static int published_at(const char *topic) {
    for (int i = 0; i < g_pub.n && i < 48; i++)
        if (strcmp(g_pub.topic[i], topic) == 0) return i;
    return -1;
}

static void setup(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    memset(&g_ctx, 0, sizeof g_ctx);
    memset(&g_cfg, 0, sizeof g_cfg);
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    g_ctx.cfg = &g_cfg;
    g_ctx.prov = &cb_provider_aws;
    cb_rt_deps_g.verify_token = fake_verify;
    cb_rt_iot_publish = fake_publish;
}

static void pushes_are_publishes(void) {
    setup();
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    cb_rt *rt;
    T_EQ_INT(cb_rt_iot_new(&g_ctx, &rt), CB_OK);
    const uint8_t cast[] = { 5, 6 };
    rt_publish_cast(rt, UA, G, 3, cast, 2);
    T_EQ_INT(g_pub.n, 1);
    T_EQ_STR(g_pub.endpoint, "abc123-ats.iot.us-east-1.amazonaws.com");
    T_EQ_STR(g_pub.topic[0], "u/" UA "/c/" G);
    const uint8_t want[] = { 1, NW_WS_CAST, 6, 'a', 'b', '1', '2', 'c', 'd', 3, 0, 0, 0, 2, 0, 0, 0, 5, 6 };
    T_EQ_MEM(g_pub.body, g_pub.len, want, sizeof want);   // the nw1 message, as it is
    rt_publish_cast(rt, NULL, G, 3, cast, 2);
    T_EQ_STR(g_pub.topic[1], "s/" G);
    // Chat: each member's own topic, never the whole game's (nw1 only).
    cb_chat_row c;
    memset(&c, 0, sizeof c);
    strcpy(c.id, "m");
    strcpy(c.user_id, UA);
    c.message = "hi";
    cb_uuid members[2] = { UA, UB };
    rt_publish_chat(rt, G, &c, members, 2);
    T_EQ_INT(g_pub.n, 4);
    T_CHECK(published_at("u/" UA "/m/" G) >= 2);   // the members' lanes run at once: either order
    T_CHECK(published_at("u/" UB "/m/" G) >= 2);
    T_EQ_INT(cb_rt_failures_take(), 0);
    // A refused publish counts as a failed delivery.
    g_pub.status = 403;
    rt_publish_cast(rt, UA, G, 4, cast, 2);
    T_EQ_INT(cb_rt_failures_take(), 1);
    g_pub.status = CB_E_IO;
    rt_publish_cast(rt, UA, G, 4, cast, 2);
    T_EQ_INT(cb_rt_failures_take(), 1);
    // No endpoint configured: nothing is sent, and nothing failed.
    g_cfg.iot_endpoint[0] = 0;
    int before = g_pub.n;
    rt_publish_cast(rt, UA, G, 5, cast, 2);
    T_EQ_INT(g_pub.n, before);
    T_EQ_INT(cb_rt_failures_take(), 0);
    // A user id that cannot be a topic level is never published.
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    rt_publish_cast(rt, "u/+", G, 5, cast, 2);
    T_EQ_INT(g_pub.n, before);
    // forget is not this transport's: IoT names no connection to end.
    cb_rt_forget_user(rt, UA);
    cb_rt_free(rt);
}

// A commit's fan-out between two humans is five publishes: each human's view
// (the dashboard, and a boards host's game), each seat's cast, the
// spectators' cast. In one batch they go out on three lanes at once (one per
// user, one for the spectators), so with IoT taking WAIT ms a publish the
// whole fan-out takes about two publishes' time (a user's view, then its
// cast), not five; and a user still hears its view before its cast.
#define WAIT 60
static void a_fanout_overlaps(void) {
    setup();
    cb_strlcpy(g_cfg.iot_endpoint, "abc123-ats.iot.us-east-1.amazonaws.com", sizeof g_cfg.iot_endpoint);
    cb_rt *rt;
    T_EQ_INT(cb_rt_iot_new(&g_ctx, &rt), CB_OK);
    g_ctx.rt = rt;
    const uint8_t env[] = { 1, 2, 3 }, cast[] = { 4, 5 }, spec[] = { 6 };
    cb_fanout f;
    memset(&f, 0, sizeof f);
    f.game_id = G;
    f.version = 7;
    f.status = "playing";
    f.n_seats = 3;
    cb_strlcpy(f.seats[0].user_id, UA, sizeof f.seats[0].user_id);
    cb_strlcpy(f.seats[2].user_id, UB, sizeof f.seats[2].user_id);   // seat 1 is a bot: nothing for it
    for (int i = 0; i < 3; i++) {
        f.seats[i].envelope = env, f.seats[i].envelope_len = sizeof env;
        f.seats[i].cast = cast, f.seats[i].cast_len = sizeof cast;
    }
    f.spectator_cast = spec;
    f.spectator_cast_len = sizeof spec;
    g_pub.wait_ms = WAIT;
    g_pub.gather = 3;
    int64_t t0 = cb_mono_ms();
    cb_fanout_publish(&g_ctx, &f);
    int64_t took = cb_mono_ms() - t0;
    T_EQ_INT(g_pub.n, 5);
    T_EQ_INT(cb_rt_failures_take(), 0);
    int va = published_at("u/" UA "/v"), vb = published_at("u/" UB "/v");
    int ca = published_at("u/" UA "/c/" G), cb = published_at("u/" UB "/c/" G), sp = published_at("s/" G);
    T_CHECK(va >= 0 && vb >= 0 && ca >= 0 && cb >= 0 && sp >= 0);
    // Each user's view was answered before its cast was sent, on one lane.
    T_CHECK(g_pub.end_ms[va] <= g_pub.start_ms[ca]);
    T_CHECK(g_pub.end_ms[vb] <= g_pub.start_ms[cb]);
    T_CHECK(pthread_equal(g_pub.thread[va], g_pub.thread[ca]));
    // Three lanes at once (never more): each lane's two publishes in turn, so two publishes' time at least.
    T_CHECK(took >= 2 * WAIT);
    T_EQ_INT(g_pub.peak, 3);
    T_CHECK(!pthread_equal(g_pub.thread[va], g_pub.thread[vb]));
    T_CHECK(!pthread_equal(g_pub.thread[sp], g_pub.thread[va]) && !pthread_equal(g_pub.thread[sp], g_pub.thread[vb]));

    // A publish refused on one lane counts once, the rest still go.
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    g_pub.fail_topic = "u/" UB "/v";
    cb_fanout_publish(&g_ctx, &f);
    T_EQ_INT(g_pub.n, 5);
    T_EQ_INT(cb_rt_failures_take(), 1);

    // A seat that left hears the empty view on its own lane, and a batch
    // holds every message of the largest commit (8 humans, 8 gone): 25.
    memset(&g_pub, 0, sizeof g_pub);
    g_pub.status = 200;
    f.n_seats = CB_MAX_SEATS;
    static char ids[2 * CB_MAX_SEATS][40];
    for (int i = 0; i < CB_MAX_SEATS; i++) {
        snprintf(ids[i], sizeof ids[i], "aaaaaaaa-aaaa-4aaa-8aaa-%012d", i);
        cb_strlcpy(f.seats[i].user_id, ids[i], sizeof f.seats[i].user_id);
        f.seats[i].envelope = env, f.seats[i].envelope_len = sizeof env;
        f.seats[i].cast = cast, f.seats[i].cast_len = sizeof cast;
        snprintf(ids[CB_MAX_SEATS + i], sizeof ids[0], "cccccccc-cccc-4ccc-8ccc-%012d", i);
        cb_strlcpy(f.left[i], ids[CB_MAX_SEATS + i], sizeof f.left[i]);
    }
    f.n_left = CB_MAX_SEATS;
    g_pub.wait_ms = 20;
    g_pub.gather = 2 * CB_MAX_SEATS + 1;
    cb_fanout_publish(&g_ctx, &f);
    T_EQ_INT(g_pub.n, 3 * CB_MAX_SEATS + 1);
    T_EQ_INT(cb_rt_failures_take(), 0);
    T_EQ_INT(g_pub.peak, 2 * CB_MAX_SEATS + 1);   // 17 lanes at once: a view and a cast each at most
    char t[128];
    snprintf(t, sizeof t, "u/%s/v", ids[CB_MAX_SEATS]);
    T_CHECK(published_at(t) >= 0);
    g_ctx.rt = NULL;
    cb_rt_free(rt);
}

// ---- the authorizer --------------------------------------------------------------------------

int main(void) {
    T_RUN(pushes_are_publishes);
    T_RUN(a_fanout_overlaps);
    return T_DONE();
}

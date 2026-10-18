// unit_rt_core.c - the push stream's provider-free half (realtime/realtime.h):
// a target's topic from its family's pattern, the families' check and boot
// refusing to serve with a bad one, and cb_rt_publish handing 0, 1 and
// several targets to a transport, with the failures it counts. No socket and
// no provider: the transports themselves are the push conformance suite's
// (conformance/push.h), run from each provider's tests.
#include "t.h"

#include "boot.h"
#include "cbase/app.h"
#include "cbase/provider.h"
#include "realtime/realtime.h"
#include "util/err.h"
#include "util/log.h"

#include <pthread.h>
#include <stdlib.h>

#define UA "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
#define UB "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
#define D  "ab12cd"

static const cb_topic_family F_VIEWS = { "views", "u/{user}/v" };
static const cb_topic_family F_SEAT  = { "seat", "u/{user}/c/{doc}" };
static const cb_topic_family F_WATCH = { "watch", "s/{doc}" };
static const cb_topic_family F_ALL   = { "all", "s/all" };

// ---- cb_rt_topic over patterns ---------------------------------------------------------------

static void topics_from_patterns(void) {
    char t[128];
    // A literal-only pattern is its own topic, whatever ids the target carries.
    cb_rt_to lit = { &F_ALL, NULL, NULL };
    T_EQ_INT(cb_rt_topic(&lit, t, sizeof t), 5);
    T_EQ_STR(t, "s/all");
    cb_rt_to lit_ids = { &F_ALL, UA, D };
    T_EQ_INT(cb_rt_topic(&lit_ids, t, sizeof t), 5);
    T_EQ_STR(t, "s/all");
    // Both ids, each in its place.
    cb_rt_to seat = { &F_SEAT, UA, D };
    T_EQ_INT(cb_rt_topic(&seat, t, sizeof t), 2 + 36 + 3 + 6);
    T_EQ_STR(t, "u/" UA "/c/" D);
    // {user} missing, {doc} missing: no topic, and out left empty.
    cb_rt_to no_user = { &F_SEAT, NULL, D };
    strcpy(t, "junk");
    T_EQ_INT(cb_rt_topic(&no_user, t, sizeof t), -1);
    T_EQ_STR(t, "");
    cb_rt_to no_doc = { &F_SEAT, UA, NULL };
    strcpy(t, "junk");
    T_EQ_INT(cb_rt_topic(&no_doc, t, sizeof t), -1);
    T_EQ_STR(t, "");
    cb_rt_to empty_doc = { &F_WATCH, NULL, "" };
    T_EQ_INT(cb_rt_topic(&empty_doc, t, sizeof t), -1);
    // A family that needs only {doc} does not need {user}.
    cb_rt_to watch = { &F_WATCH, NULL, D };
    T_EQ_INT(cb_rt_topic(&watch, t, sizeof t), 8);
    T_EQ_STR(t, "s/" D);
    // A cap one short of the topic and its NUL refuses; the exact cap fits.
    size_t len = strlen("u/" UA "/c/" D);
    strcpy(t, "junk");
    T_EQ_INT(cb_rt_topic(&seat, t, len), -1);
    T_EQ_STR(t, "");
    T_EQ_INT(cb_rt_topic(&seat, t, len + 1), (int)len);
    T_EQ_STR(t, "u/" UA "/c/" D);
    // Short at a literal byte and short inside an id both refuse.
    T_EQ_INT(cb_rt_topic(&seat, t, 3), -1);
    T_EQ_INT(cb_rt_topic(&seat, t, 10), -1);
    // An id of RT_ID_MAX (64) bytes is a level; one byte past is not.
    char id64[65], id65[66];
    memset(id64, 'x', 64);
    id64[64] = 0;
    memset(id65, 'x', 65);
    id65[65] = 0;
    cb_rt_to at = { &F_VIEWS, id64, NULL };
    T_EQ_INT(cb_rt_topic(&at, t, sizeof t), 2 + 64 + 2);
    cb_rt_to past = { &F_VIEWS, id65, NULL };
    T_EQ_INT(cb_rt_topic(&past, t, sizeof t), -1);
    cb_rt_to past_doc = { &F_WATCH, NULL, id65 };
    T_EQ_INT(cb_rt_topic(&past_doc, t, sizeof t), -1);
    // Every byte a level may hold passes; a separator or a wildcard does not.
    cb_rt_to ok_bytes = { &F_WATCH, NULL, "AZaz09-_" };
    T_EQ_INT(cb_rt_topic(&ok_bytes, t, sizeof t), 10);
    const char *bad[] = { "a/b", "a+", "a#", "a b", "a.b", "\xc3\xa9" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        cb_rt_to b = { &F_WATCH, NULL, bad[i] };
        T_EQ_INT(cb_rt_topic(&b, t, sizeof t), -1);
    }
    // No family, no pattern, no room at all.
    cb_rt_to none = { NULL, UA, D };
    T_EQ_INT(cb_rt_topic(&none, t, sizeof t), -1);
    static const cb_topic_family no_pattern = { "x", NULL };
    cb_rt_to np = { &no_pattern, UA, D };
    T_EQ_INT(cb_rt_topic(&np, t, sizeof t), -1);
    T_EQ_INT(cb_rt_topic(&seat, t, 0), -1);
    T_EQ_INT(cb_rt_topic(NULL, t, sizeof t), -1);
}

// ---- cb_rt_families_check, and boot -------------------------------------------------------------

static int check_one(const char *pattern, char *err, size_t cap) {
    cb_topic_family f = { "fam", pattern };
    const cb_topic_family *list[] = { &F_VIEWS, &f };
    err[0] = 0;
    return cb_rt_families_check(list, 2, err, cap);
}

static void families_are_checked(void) {
    char err[256];
    const cb_topic_family *good[] = { &F_VIEWS, &F_SEAT, &F_WATCH, &F_ALL };
    T_EQ_INT(cb_rt_families_check(good, 4, err, sizeof err), CB_OK);
    T_EQ_INT(cb_rt_families_check(good, 0, err, sizeof err), CB_OK);
    T_EQ_INT(check_one("u/{user}/m/{doc}", err, sizeof err), CB_OK);
    T_EQ_INT(check_one("s/{doc}/{user}", err, sizeof err), CB_OK);
    // Outside u/{user}/ and s/.
    const char *refused[] = {
        "x/{user}/v",        // another root
        "u/{user}",          // the user's level, but not under it
        "u/{doc}/v",         // u/ with another level than the user
        "u/abc/v",           // u/ with a fixed user
        "s",                 // no s/ level
        "",                  // nothing
        "s/+",               // a wildcard
        "u/{user}/#",
        "s/{name}",          // an unknown {name}
        "s/{use}",
        "s/{abcd}",          // unknown, as long as {doc}
        "u/{user}/{uxer}",   // unknown, as long as {user}
        "s/{doc",            // a brace not closed
        "s/a}",              // a stray }
        "s/{doc}}",
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        T_EQ_INT(check_one(refused[i], err, sizeof err), CB_E_INVALID);
        T_CHECK(strstr(err, "topic family fam") != NULL);   // the family is named
    }
    // A NULL family or pattern is refused, never read.
    static const cb_topic_family no_pattern = { "np", NULL };
    const cb_topic_family *np[] = { &no_pattern };
    T_EQ_INT(cb_rt_families_check(np, 1, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "topic family np") != NULL);
    const cb_topic_family *nul[] = { &F_VIEWS, NULL };
    T_EQ_INT(cb_rt_families_check(nul, 2, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "topic family ?") != NULL);
}

static int no_rt(cb_ctx *ctx, cb_rt **out) { *out = NULL; return CB_OK; }

// Boot refuses to serve an app with a bad family, before it opens anything.
static void boot_refuses_a_bad_family(void) {
    static const cb_topic_family bad = { "wild", "u/{user}/+" };
    static const cb_topic_family *const topics[] = { &F_VIEWS, &bad };
    cb_app app;
    memset(&app, 0, sizeof app);
    app.name = "rtcore";
    app.topics = topics;
    app.n_topics = 2;
    cb_provider prov;
    memset(&prov, 0, sizeof prov);
    prov.name = "test";
    prov.mode = "test";
    prov.flags = CB_PROV_TEST_CLOCK;
    prov.store = "sqlite";
    prov.rt_open = no_rt;
    cb_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bot_speedup = 1;
    cb_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.cfg = &cfg;
    ctx.prov = &prov;
    ctx.app = &app;
    char err[256] = "";
    T_EQ_INT(cb_boot(&ctx, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "topic family wild") != NULL);
    T_CHECK(ctx.store == NULL && ctx.auth == NULL && ctx.rt == NULL && ctx.router == NULL);
    cb_unboot(&ctx);
}

// ---- cb_rt_publish ------------------------------------------------------------------------------------

// What a transport was handed: each deliver, and each deliver_lanes call.
static struct {
    int  delivers, lane_calls, lane_n;
    char topic[40][128];
    int  lane[40];
    size_t len[40];
    uint8_t first[40];
    int  n;            // targets seen, through either
    int  fail_each;    // what each deliver returns
    int  fail_lanes;   // what deliver_lanes returns
} g_t;

static void note(const cb_rt_to *to, const uint8_t *frame, size_t len, int lane) {
    if (g_t.n >= 40) return;
    cb_rt_topic(to, g_t.topic[g_t.n], sizeof g_t.topic[0]);
    g_t.len[g_t.n] = len;
    g_t.first[g_t.n] = len ? frame[0] : 0;
    g_t.lane[g_t.n] = lane;
    g_t.n++;
}

static int t_deliver(cb_rt *rt, const cb_rt_to *to, const uint8_t *frame, size_t len) {
    g_t.delivers++;
    note(to, frame, len, -1);
    return g_t.fail_each;
}

static int t_deliver_lanes(cb_rt *rt, const cb_rt_to *to, const uint8_t *const *frames, const size_t *lens,
                           const int *lane, int n) {
    g_t.lane_calls++;
    g_t.lane_n = n;
    for (int i = 0; i < n; i++) note(&to[i], frames[i], lens[i], lane[i]);
    return g_t.fail_lanes;
}

static const cb_rt_transport T_LANES = { .name = "lanes", .deliver = t_deliver, .deliver_lanes = t_deliver_lanes };
static const cb_rt_transport T_PLAIN = { .name = "plain", .deliver = t_deliver };

static void publish_zero_one_several(void) {
    cb_rt rt = { &T_LANES, NULL, NULL };
    memset(&g_t, 0, sizeof g_t);
    (void)cb_rt_failures_take();
    const uint8_t msg[] = { 0x5a, 0, 0xff };
    cb_rt_to to[3] = { { &F_VIEWS, UA, NULL }, { &F_VIEWS, UB, NULL }, { &F_WATCH, NULL, D } };
    // n 0: nothing reaches the transport.
    cb_rt_publish(&rt, to, 0, msg, sizeof msg);
    T_EQ_INT(g_t.delivers + g_t.lane_calls, 0);
    // n 1: one deliver, at once, no batch.
    cb_rt_publish(&rt, to, 1, msg, sizeof msg);
    T_EQ_INT(g_t.delivers, 1);
    T_EQ_INT(g_t.lane_calls, 0);
    T_EQ_STR(g_t.topic[0], "u/" UA "/v");
    T_EQ_INT(g_t.len[0], 3);
    // n several: one batch, each target in its own lane (two users, the spectators).
    memset(&g_t, 0, sizeof g_t);
    cb_rt_publish(&rt, to, 3, msg, sizeof msg);
    T_EQ_INT(g_t.delivers, 0);
    T_EQ_INT(g_t.lane_calls, 1);
    T_EQ_INT(g_t.lane_n, 3);
    T_EQ_STR(g_t.topic[0], "u/" UA "/v");
    T_EQ_STR(g_t.topic[1], "u/" UB "/v");
    T_EQ_STR(g_t.topic[2], "s/" D);
    T_EQ_INT(g_t.lane[0], 0);
    T_EQ_INT(g_t.lane[1], 1);
    T_EQ_INT(g_t.lane[2], 2);
    for (int i = 0; i < 3; i++) T_CHECK(g_t.len[i] == 3 && g_t.first[i] == 0x5a);
    // The same user twice shares a lane.
    cb_rt_to same[3] = { { &F_VIEWS, UA, NULL }, { &F_SEAT, UA, D }, { &F_VIEWS, UB, NULL } };
    memset(&g_t, 0, sizeof g_t);
    cb_rt_publish(&rt, same, 3, msg, sizeof msg);
    T_EQ_INT(g_t.lane_calls, 1);
    T_CHECK(g_t.lane[0] == 0 && g_t.lane[1] == 0 && g_t.lane[2] == 1);
    // Inside an open batch a several-target publish joins it and is held to its end.
    memset(&g_t, 0, sizeof g_t);
    cb_rt_batch_begin(&rt);
    cb_rt_publish(&rt, to, 2, msg, sizeof msg);
    cb_rt_publish(&rt, to + 2, 1, msg, sizeof msg);
    T_EQ_INT(g_t.delivers + g_t.lane_calls, 0);
    cb_rt_batch_end(&rt);
    T_EQ_INT(g_t.lane_calls, 1);
    T_EQ_INT(g_t.lane_n, 3);
    // A transport without lanes is handed each target, one deliver each, in order.
    cb_rt plain = { &T_PLAIN, NULL, NULL };
    memset(&g_t, 0, sizeof g_t);
    cb_rt_publish(&plain, to, 3, msg, sizeof msg);
    T_EQ_INT(g_t.delivers, 3);
    T_EQ_INT(g_t.lane_calls, 0);
    T_EQ_STR(g_t.topic[2], "s/" D);
    // No transport, or none to deliver with: nothing, and no crash.
    memset(&g_t, 0, sizeof g_t);
    cb_rt_publish(NULL, to, 3, msg, sizeof msg);
    cb_rt bare = { NULL, NULL, NULL };
    cb_rt_publish(&bare, to, 3, msg, sizeof msg);
    T_EQ_INT(g_t.n, 0);
    T_EQ_INT(cb_rt_failures_take(), 0);
}

static void *other_thread_take(void *arg) {
    *(int *)arg = cb_rt_failures_take();
    return NULL;
}

static void failures_are_counted(void) {
    cb_rt rt = { &T_LANES, NULL, NULL };
    memset(&g_t, 0, sizeof g_t);
    (void)cb_rt_failures_take();
    const uint8_t msg[] = { 1 };
    cb_rt_to to[3] = { { &F_VIEWS, UA, NULL }, { &F_VIEWS, UB, NULL }, { &F_WATCH, NULL, D } };
    // A deliver's failures, a batch's, and a product's own.
    g_t.fail_each = 1;
    cb_rt_publish(&rt, to, 1, msg, 1);
    T_EQ_INT(cb_rt_failures_take(), 1);
    T_EQ_INT(cb_rt_failures_take(), 0);   // a take clears
    g_t.fail_lanes = 2;
    cb_rt_publish(&rt, to, 3, msg, 1);
    T_EQ_INT(cb_rt_failures_take(), 2);
    cb_rt_failures_add(3);
    cb_rt_failures_add(0);
    cb_rt_publish(&rt, to, 1, msg, 1);
    T_EQ_INT(cb_rt_failures_take(), 4);   // added and delivered sum
    // Per thread: another thread's take sees none of this one's.
    cb_rt_failures_add(5);
    int seen = -1;
    pthread_t th;
    T_EQ_INT(pthread_create(&th, NULL, other_thread_take, &seen), 0);
    pthread_join(th, NULL);
    T_EQ_INT(seen, 0);
    T_EQ_INT(cb_rt_failures_take(), 5);
}

int main(void) {
    cb_log_init(CB_LOG_ERROR, 0);
    T_RUN(topics_from_patterns);
    T_RUN(families_are_checked);
    T_RUN(boot_refuses_a_bad_family);
    T_RUN(publish_zero_one_several);
    T_RUN(failures_are_counted);
    return T_DONE();
}

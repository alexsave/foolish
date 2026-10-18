// unit_describe.c - the app's CloudFormation template from its descriptor
// (describe.h): the linked app's template against the skeleton the build uses,
// what each declared kind brings and what an app without it lacks, and every
// refusal of a descriptor or a skeleton.
#include "t.h"

#include "cbase/app.h"
#include "describe.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[256 * 1024];
    *n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[*n] = 0;
    return buf;
}

static int count(const char *hay, const char *needle) {
    int k = 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q++) k++;
    return k;
}

// describe over skel, into a fresh buffer the caller frees; the error in err.
static int run(const cb_app *app, const char *skel, cb_buf *out, char *err, size_t cap) {
    cb_buf_init(out);
    err[0] = 0;
    return cb_aws_describe(app, skel, strlen(skel), out, err, cap);
}

static int never(cb_ctx *ctx, int64_t now) { (void)ctx; (void)now; return 0; }
static int never_job(cb_ctx *ctx, const char *key) { (void)ctx; (void)key; return 0; }

static const cb_deploy DEPLOY = { "demo", "api.example.com", 7, 6, 3, 2 };
static const cb_topic_family FAM_A = { "a", "u/{user}/a" }, FAM_B = { "b", "s/{doc}" }, FAM_C = { "c", "u/{user}/c/{doc}" },
                             FAM_D = { "d", "n/{doc}" };
static const cb_topic_family *const FAMS[] = { &FAM_A, &FAM_B, &FAM_C, &FAM_D };
static const char *const PATHS[] = { "/v1/one", "/v1/two", "/v1/three" };
static const cb_job JOBS[] = { { "work", never_job } };
static const cb_schedule SCHEDS[] = {
    { .name = "tick", .every_ms = 1000, .run = never, .deployed_every_s = 60, .about = "Every minute." },
    { .name = "bot_sweep", .every_ms = 1000, .run = never, .deployed_every_s = 7200, .deployed_name = "sweeper", .about = "Twice a day, more or less." },
};

static cb_app full_app(void) {
    cb_app a = { .name = "demo" };
    a.deploy = &DEPLOY;
    a.jobs = JOBS;
    a.n_jobs = 1;
    a.schedules = SCHEDS;
    a.n_schedules = 2;
    a.topics = FAMS;
    a.n_topics = 4;
    a.cached_paths = PATHS;
    a.n_cached_paths = 3;
    return a;
}

// The linked app (foolish) over the build's skeleton: what production's stack
// must keep. The logical ids and physical names the live stack has, the
// units and the alarms on them, and nothing left unfilled.
static void the_linked_app_writes_its_stack(void) {
    size_t n = 0;
    const char *skel = slurp("providers/aws/ops/app.yaml.in", &n);
    T_CHECK(skel != NULL);
    if (!skel) return;
    cb_buf out;
    char err[256];
    cb_buf_init(&out);
    T_EQ_INT(cb_aws_describe(cb_main_app, skel, n, &out, err, sizeof err), CB_OK);
    const char *y = cb_buf_cstr(&out);
    static const char *const want[] = {
        "  Table:\n    Type: AWS::DynamoDB::Table\n",
        "  Function:\n    Type: AWS::Lambda::Function\n",
        "  IotAuthorizer:\n    Type: AWS::IoT::Authorizer\n",
        "  ScanSchedule:\n    Type: AWS::Scheduler::Schedule\n",
        "      Name: !Sub \"${FunctionName}-bot-scan\"\n",
        "      ScheduleExpression: rate(1 minute)\n",
        "        Input: '\"scan\"'\n",
        "  MqttAuthorizerName:\n    Type: AWS::CloudFront::Function\n",
        "  PublicReadsCachePolicy:\n",
        "        Comment: /v1/leaderboard and /v1/bots, keyed on the path only\n",
        "          - PathPattern: /v1/leaderboard\n",
        "          - PathPattern: /v1/bots\n",
        "            - PathPattern: /mqtt*\n",
        "  Distribution:\n    Type: AWS::CloudFront::Distribution\n",
        "      ProvisionedThroughput: { ReadCapacityUnits: 15, WriteCapacityUnits: 15 }\n",
        "Description: c_base, the foolish backend (Lambda + DynamoDB + CloudFront)\n",
        "    Default: cbase\n",
        "    Default: /cbase/\n",
        "    Default: api.foolish.cards\n",
        "topic/u/*\"\n",
        "topic/s/*\"\n",
        "      Threshold: 3600\n",
        "      Threshold: 1200\n",
        "  EdgeInvalidationPolicy:\n",
        "              - Sid: SelfInvoke",
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        if (!strstr(y, want[i])) printf("  the template lacks: %s\n", want[i]);
        T_CHECK(strstr(y, want[i]) != NULL);
    }
    T_EQ_INT(count(y, "ProvisionedThroughput: { ReadCapacityUnits: 5, WriteCapacityUnits: 5 }"), 2);
    T_EQ_INT(count(y, "topic/"), 2);   // u and s, once each, for four families
    T_EQ_INT(count(y, "@"), 0);
    T_EQ_INT(count(y, "#@"), 0);
    T_EQ_INT(count(y, "Type: AWS::CloudWatch::Alarm"), 10);
    // Twice: the same bytes.
    cb_buf again;
    cb_buf_init(&again);
    T_EQ_INT(cb_aws_describe(cb_main_app, skel, n, &again, err, sizeof err), CB_OK);
    T_EQ_MEM(out.p, out.len, again.p, again.len);
    cb_buf_free(&again);
    cb_buf_free(&out);
}

static const char SKEL[] =
    "A @app@ @stack@ @domain@\n"
    "U @table_read@ @table_write@ @index_read@ @index_write@\n"
    "L @table_read_alarm@ @table_write_alarm@ @index_write_alarm@\n"
    "#@# a comment of the skeleton's\n"
    "#@if jobs\n"
    "JOBS\n"
    "#@end\n"
    "#@if schedules\n"
    "#@schedules\n"
    "#@end\n"
    "#@if topics\n"
    "#@iot_topics\n"
    "#@end\n"
    "#@if cached|topics\n"
    "BEHAVIOURS\n"
    "#@if cached\n"
    "C @cached_list@\n"
    "#@cache_behaviours\n"
    "#@end\n"
    "#@end\n"
    "END\n";

// Every kind declared: each region kept and each generated entry written.
static void each_kind_brings_its_part(void) {
    cb_app a = full_app();
    cb_buf out;
    char err[256];
    T_EQ_INT(run(&a, SKEL, &out, err, sizeof err), CB_OK);
    const char *y = cb_buf_cstr(&out);
    T_CHECK(strstr(y, "A demo demo api.example.com\n") != NULL);
    T_CHECK(strstr(y, "U 7 6 3 2\n") != NULL);
    T_CHECK(strstr(y, "L 1680 1440 480\n") != NULL);   // 80% of 300 s of each side's units
    T_CHECK(strstr(y, "JOBS\n") != NULL);
    T_CHECK(strstr(y, "BEHAVIOURS\n") != NULL);
    T_CHECK(strstr(y, "C /v1/one, /v1/two and /v1/three\n") != NULL);
    T_EQ_INT(count(y, "    Type: AWS::Scheduler::Schedule\n"), 2);
    T_CHECK(strstr(y, "  TickSchedule:\n") != NULL);
    T_CHECK(strstr(y, "      Name: !Sub \"${FunctionName}-tick\"\n") != NULL);
    T_CHECK(strstr(y, "      Description: Every minute.\n") != NULL);
    T_CHECK(strstr(y, "      ScheduleExpression: rate(1 minute)\n") != NULL);
    T_CHECK(strstr(y, "        Input: '\"tick\"'\n") != NULL);
    T_CHECK(strstr(y, "  BotSweepSchedule:\n") != NULL);
    T_CHECK(strstr(y, "      Name: !Sub \"${FunctionName}-sweeper\"\n") != NULL);
    T_CHECK(strstr(y, "      ScheduleExpression: rate(2 hours)\n") != NULL);
    T_CHECK(strstr(y, "        Input: '\"bot_sweep\"'\n") != NULL);
    T_CHECK(strstr(y, "MaximumRetryAttempts: 0   # the next run is the retry\n\n  BotSweepSchedule:\n") != NULL);
    // The prefixes in their first order, each once: u, s, n.
    const char *u = strstr(y, ":topic/u/*\""), *s = strstr(y, ":topic/s/*\""), *nn = strstr(y, ":topic/n/*\"");
    T_CHECK(u && s && nn && u < s && s < nn);
    T_EQ_INT(count(y, ":topic/"), 3);
    T_EQ_INT(count(y, "          - PathPattern: "), 3);
    T_CHECK(strstr(y, "          - PathPattern: /v1/two\n            TargetOriginId: function\n") != NULL);
    T_EQ_INT(count(y, "#@"), 0);
    T_CHECK(strstr(y, "a comment") == NULL);
    cb_buf_free(&out);

    // One cached path and two: the list's two shapes.
    a.n_cached_paths = 1;
    T_EQ_INT(run(&a, SKEL, &out, err, sizeof err), CB_OK);
    T_CHECK(strstr(cb_buf_cstr(&out), "C /v1/one\n") != NULL);
    cb_buf_free(&out);
    a.n_cached_paths = 2;
    T_EQ_INT(run(&a, SKEL, &out, err, sizeof err), CB_OK);
    T_CHECK(strstr(cb_buf_cstr(&out), "C /v1/one and /v1/two\n") != NULL);
    cb_buf_free(&out);
}

// Nothing declared: every region dropped, "a|b" only with neither; a region
// nested in a dropped one stays dropped.
static void an_app_without_a_kind_lacks_its_part(void) {
    cb_app a = { .name = "bare", .deploy = &DEPLOY };
    cb_buf out;
    char err[256];
    T_EQ_INT(run(&a, SKEL, &out, err, sizeof err), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out), "A bare demo api.example.com\nU 7 6 3 2\nL 1680 1440 480\nEND\n");
    cb_buf_free(&out);
    // Topics alone: the either-or region is kept, the cached one in it is not.
    a.topics = FAMS;
    a.n_topics = 1;
    T_EQ_INT(run(&a, SKEL, &out, err, sizeof err), CB_OK);
    const char *y = cb_buf_cstr(&out);
    T_CHECK(strstr(y, "BEHAVIOURS\n") != NULL);
    T_CHECK(strstr(y, "C ") == NULL);
    T_CHECK(strstr(y, "PathPattern") == NULL);
    T_CHECK(strstr(y, "JOBS") == NULL);
    T_CHECK(strstr(y, "Schedule") == NULL);
    T_EQ_INT(count(y, ":topic/u/*"), 1);
    cb_buf_free(&out);
    // Cached alone: the cached region inside the either-or one is kept.
    a.n_topics = 0;
    a.cached_paths = PATHS;
    a.n_cached_paths = 1;
    T_EQ_INT(run(&a, SKEL, &out, err, sizeof err), CB_OK);
    T_CHECK(strstr(cb_buf_cstr(&out), "BEHAVIOURS\nC /v1/one\n") != NULL);
    cb_buf_free(&out);
}

// Each refusal names the problem and writes CB_E_INVALID.
static void a_bad_skeleton_is_refused(void) {
    cb_app a = full_app();
    static const struct { const char *skel, *why; } BAD[] = {
        { "x @nosuch@ y\n", "names no value" },
        { "x @app y\n", "names no value" },
        { "#@nosuch\n", "unknown directive" },
        { "#@if jobs\n", "ends inside" },
        { "#@end\n", "with no" },
        { "#@if nosuch\n#@end\n", "unknown kind" },
        { "#@if jobs|\n#@end\n", "unknown kind" },
        { "#@if |jobs\n#@end\n", "unknown kind" },
        { "#@if jobs|topix\n#@end\n", "unknown kind" },
        { "no newline", "newline" },
        { "", "empty" },
        { "#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n", "nested" },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        cb_buf out;
        char err[256];
        T_EQ_INT(run(&a, BAD[i].skel, &out, err, sizeof err), CB_E_INVALID);
        if (!strstr(err, BAD[i].why)) printf("  skeleton %zu: \"%s\" lacks \"%s\"\n", i, err, BAD[i].why);
        T_CHECK(strstr(err, BAD[i].why) != NULL);
        cb_buf_free(&out);
    }
    // Eight levels are allowed.
    cb_buf out;
    char err[256];
    T_EQ_INT(run(&a,
                 "#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\n#@if jobs\nDEEP\n"
                 "#@end\n#@end\n#@end\n#@end\n#@end\n#@end\n#@end\n#@end\n",
                 &out, err, sizeof err),
             CB_OK);
    T_EQ_STR(cb_buf_cstr(&out), "DEEP\n");
    cb_buf_free(&out);
}

static int refused(const cb_app *a, const char *why) {
    cb_buf out;
    char err[256];
    int rc = run(a, "x\n", &out, err, sizeof err);
    cb_buf_free(&out);
    if (rc == CB_E_INVALID && !strstr(err, why)) printf("  \"%s\" lacks \"%s\"\n", err, why);
    return rc == CB_E_INVALID && strstr(err, why) != NULL;
}

static void a_bad_descriptor_is_refused(void) {
    cb_app a = full_app();
    T_CHECK(refused(NULL, "no app"));
    a.deploy = NULL;
    T_CHECK(refused(&a, "no deployment"));
    cb_deploy d = DEPLOY;
    a.deploy = &d;
    const char *bad_names[] = { "", "Demo", "-demo", "demo-", "de_mo", "a234567890123456789012345678901234567890a" };
    for (size_t i = 0; i < sizeof bad_names / sizeof bad_names[0]; i++) {
        d.stack = bad_names[i];
        T_CHECK(refused(&a, "stack name"));
    }
    d.stack = "a23456789012345678901234567890123456789";   // 40: the most
    cb_buf out;
    char err[256];
    T_EQ_INT(run(&a, "x\n", &out, err, sizeof err), CB_OK);
    cb_buf_free(&out);
    d = DEPLOY;
    const char *bad_hosts[] = { "", ".a.com", "a.com.", "a..com", "A.com", "a_b.com", "a b" };
    for (size_t i = 0; i < sizeof bad_hosts / sizeof bad_hosts[0]; i++) {
        d.domain = bad_hosts[i];
        T_CHECK(refused(&a, "domain"));
    }
    d = DEPLOY;
    d.index_write = 0;
    T_CHECK(refused(&a, "units"));
    d = DEPLOY;
    d.table_read = 40001;
    T_CHECK(refused(&a, "units"));
    d = DEPLOY;
    a.name = "My App";
    T_CHECK(refused(&a, "app's name"));
    a = full_app();

    cb_schedule s[2] = { SCHEDS[0], SCHEDS[1] };
    a.schedules = s;
    const int64_t bad_every[] = { 0, -60, 30, 90, 3601 };
    for (size_t i = 0; i < sizeof bad_every / sizeof bad_every[0]; i++) {
        s[0].deployed_every_s = bad_every[i];
        T_CHECK(refused(&a, "whole number of minutes"));
    }
    s[0] = SCHEDS[0];
    const char *bad_about[] = { NULL, "", "a: b", "a #b", "- a", "#a", "a ", "mail@host", "tab\there", "\"quoted\"" };
    for (size_t i = 0; i < sizeof bad_about / sizeof bad_about[0]; i++) {
        s[0].about = bad_about[i];
        T_CHECK(refused(&a, "about"));
    }
    s[0] = SCHEDS[0];
    s[0].deployed_name = "Bad_Name";
    T_CHECK(refused(&a, "deployed name"));
    s[0] = SCHEDS[0];
    s[0].name = "Bad";
    T_CHECK(refused(&a, "valid name"));
    s[0] = SCHEDS[0];
    s[1].deployed_name = "tick";   // the first one's
    T_CHECK(refused(&a, "one deployed name"));
    s[1] = SCHEDS[1];
    s[1].name = "tick_";   // TickSchedule as well
    s[1].deployed_name = "other";
    T_CHECK(refused(&a, "one logical id"));
    a = full_app();

    const char *bad_paths[] = { "v1/x", "", "/a b", "/a:b", "/a*" };
    const char *paths[1];
    a.cached_paths = paths;
    a.n_cached_paths = 1;
    for (size_t i = 0; i < sizeof bad_paths / sizeof bad_paths[0]; i++) {
        paths[0] = bad_paths[i];
        T_CHECK(refused(&a, "cached path"));
    }
    a = full_app();
    static const cb_topic_family NOSEG = { "x", "/{doc}" }, NOSLASH = { "y", "flat" }, UPPER = { "z", "U/{user}/v" };
    const cb_topic_family *fams[1];
    a.topics = fams;
    a.n_topics = 1;
    fams[0] = &NOSEG;
    T_CHECK(refused(&a, "first segment"));
    fams[0] = &NOSLASH;
    T_CHECK(refused(&a, "first segment"));
    fams[0] = &UPPER;
    T_CHECK(refused(&a, "not [a-z0-9]"));
}

static void settings_are_the_manifests_lines(void) {
    cb_app a = full_app();
    cb_buf out;
    char err[256];
    cb_buf_init(&out);
    T_EQ_INT(cb_aws_settings(&a, &out, err, sizeof err), CB_OK);
    T_EQ_STR(cb_buf_cstr(&out), "app=demo\nstack=demo\ndomain=api.example.com\nunits_read=13\nunits_write=10\n");
    cb_buf_free(&out);
    a.deploy = NULL;
    T_EQ_INT(cb_aws_settings(&a, &out, err, sizeof err), CB_E_INVALID);
    T_EQ_INT(out.len, 0);
    cb_buf_free(&out);
    // The linked app: the free tier's 25 a side, all of it.
    T_EQ_INT(cb_aws_settings(cb_main_app, &out, err, sizeof err), CB_OK);
    T_CHECK(strstr(cb_buf_cstr(&out), "\nunits_read=25\nunits_write=25\n") != NULL);
    T_CHECK(strstr(cb_buf_cstr(&out), "app=foolish\nstack=cbase\n") == cb_buf_cstr(&out));
    cb_buf_free(&out);
}

int main(void) {
    T_RUN(the_linked_app_writes_its_stack);
    T_RUN(each_kind_brings_its_part);
    T_RUN(an_app_without_a_kind_lacks_its_part);
    T_RUN(a_bad_skeleton_is_refused);
    T_RUN(a_bad_descriptor_is_refused);
    T_RUN(settings_are_the_manifests_lines);
    return T_DONE();
}

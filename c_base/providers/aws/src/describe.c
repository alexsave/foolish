// describe.c - the app's CloudFormation template from its descriptor and the
// skeleton (describe.h).
//
// The skeleton is read line by line. A line starting "#@" is a directive and
// is never written: "#@#" is a comment, "#@if a|b" opens a region kept only when the app declares
// a or b (they nest, and a region inside a dropped one is dropped), "#@end"
// closes it, and "#@schedules", "#@cache_behaviours" and "#@iot_topics" are
// replaced by the generated entries. In every other kept line a name between
// two at signs is replaced by its value; an unknown one is refused, so a typo
// in the skeleton cannot reach a stack.
#include "describe.h"

#include "jobs.h"
#include "util/err.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DESCRIBE_DEPTH 8   // nested "#@if"s

static int fail(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return CB_E_INVALID;
}

// 1..max bytes of [a-z0-9-], not starting or ending with '-'.
static bool dashed_ok(const char *s, size_t max) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > max || s[0] == '-' || s[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return false;
    return true;
}

// A host name: 1..253 of [a-z0-9.-], no empty label.
static bool host_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 253 || s[0] == '.' || s[n - 1] == '.' || strstr(s, "..")) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '.')) return false;
    return true;
}

// Text a YAML plain scalar holds as it is: printable ASCII, not starting with
// an indicator, no ": " and no " #" (each would end or change the scalar),
// not ending in a blank, and no at sign (the skeleton's own marker).
static bool plain_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 500 || strchr("-?:,[]{}#&*!|>'\"%@` ", s[0]) || s[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] > 0x7e || s[i] == '@') return false;
    return !strstr(s, ": ") && !strstr(s, " #");
}

// A cached path: '/' and then [A-Za-z0-9/_.-], at most 255 (CloudFront's
// PathPattern takes more, but nothing a client fetches needs it).
static bool path_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 255 || s[0] != '/') return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '/' || s[i] == '_' || s[i] == '.' || s[i] == '-'))
            return false;
    return true;
}

// EventBridge Scheduler's rate expression for a period in seconds, in the
// largest unit that divides it; false when it is not whole minutes.
static bool rate_of(int64_t s, char *out, size_t cap) {
    static const struct { int64_t secs; const char *one; } UNITS[] = { { 86400, "day" }, { 3600, "hour" }, { 60, "minute" } };
    if (s <= 0) return false;
    for (size_t i = 0; i < sizeof UNITS / sizeof UNITS[0]; i++) {
        if (s % UNITS[i].secs) continue;
        int64_t k = s / UNITS[i].secs;
        snprintf(out, cap, "rate(%lld %s%s)", (long long)k, UNITS[i].one, k == 1 ? "" : "s");
        return true;
    }
    return false;
}

// The schedule's logical id: its name in CamelCase and "Schedule" ("scan"
// -> "ScanSchedule", "bot_scan" -> "BotScanSchedule").
static void logical_id(const char *name, char *out, size_t cap) {
    size_t o = 0;
    bool up = true;
    for (const char *p = name; *p && o + 1 < cap; p++) {
        if (*p == '_') { up = true; continue; }
        out[o++] = up && *p >= 'a' && *p <= 'z' ? (char)(*p - 'a' + 'A') : *p;
        up = false;
    }
    out[o] = 0;
    cb_strlcpy(out + o, "Schedule", cap - o);
}

// A schedule's name after the stack's: its own, else the name with '_' as '-'.
static void deployed_name(const cb_schedule *s, char *out, size_t cap) {
    cb_strlcpy(out, s->deployed_name ? s->deployed_name : s->name, cap);
    for (char *p = out; *p; p++)
        if (*p == '_') *p = '-';
}

// The descriptor, checked once before anything is written.
static int check_app(const cb_app *app, char *err, size_t cap) {
    if (!app) return fail(err, cap, "no app");
    const cb_deploy *d = app->deploy;
    if (!d) return fail(err, cap, "the app declares no deployment (cb_app.deploy)");
    if (!plain_ok(app->name) || !dashed_ok(app->name, 64)) return fail(err, cap, "the app's name is not [a-z0-9-]");
    if (!dashed_ok(d->stack, 40)) return fail(err, cap, "the deployment's stack name is not 1 to 40 of [a-z0-9-]");
    if (!host_ok(d->domain)) return fail(err, cap, "the deployment's domain is not a host name");
    if (d->table_read < 1 || d->table_write < 1 || d->index_read < 1 || d->index_write < 1 || d->table_read > 40000 ||
        d->table_write > 40000 || d->index_read > 40000 || d->index_write > 40000)
        return fail(err, cap, "the store's provisioned units must each be 1 to 40000");
    for (int i = 0; i < app->n_schedules; i++) {
        const cb_schedule *s = &app->schedules[i];
        char rate[48], dn[64];
        if (!s->name || !cb_job_name_ok(s->name, strlen(s->name))) return fail(err, cap, "schedule %d has no valid name", i);
        if (!rate_of(s->deployed_every_s, rate, sizeof rate))
            return fail(err, cap, "schedule %s: deployed_every_s %lld is not a whole number of minutes", s->name,
                        (long long)s->deployed_every_s);
        if (s->deployed_name && !dashed_ok(s->deployed_name, 40))
            return fail(err, cap, "schedule %s: its deployed name is not 1 to 40 of [a-z0-9-]", s->name);
        deployed_name(s, dn, sizeof dn);
        if (!dashed_ok(dn, 40)) return fail(err, cap, "schedule %s: its deployed name is not [a-z0-9-]", s->name);
        if (!plain_ok(s->about)) return fail(err, cap, "schedule %s: its about is not one plain sentence", s->name);
        for (int j = 0; j < i; j++) {
            const cb_schedule *o = &app->schedules[j];
            char id[64], oid[64], odn[64];
            logical_id(s->name, id, sizeof id);
            logical_id(o->name, oid, sizeof oid);
            deployed_name(o, odn, sizeof odn);
            if (strcmp(id, oid) == 0) return fail(err, cap, "schedules %s and %s have one logical id", s->name, o->name);
            if (strcmp(dn, odn) == 0) return fail(err, cap, "schedules %s and %s have one deployed name", s->name, o->name);
        }
    }
    for (int i = 0; i < app->n_cached_paths; i++)
        if (!path_ok(app->cached_paths[i])) return fail(err, cap, "cached path %d is not a plain path", i);
    for (int i = 0; i < app->n_topics; i++) {
        const char *p = app->topics[i] ? app->topics[i]->pattern : NULL;
        const char *slash = p ? strchr(p, '/') : NULL;
        if (!slash || slash == p) return fail(err, cap, "topic family %d has no first segment", i);
        for (const char *q = p; q < slash; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9'))) return fail(err, cap, "topic family %d's first segment is not [a-z0-9]", i);
    }
    return CB_OK;
}

// Whether the app declares `what` ("a|b": either).
static int declares(const cb_app *app, const char *what, size_t n, bool *out) {
    *out = false;
    while (n > 0) {
        const char *bar = memchr(what, '|', n);
        size_t len = bar ? (size_t)(bar - what) : n;
        bool v;
        if (len == 4 && !memcmp(what, "jobs", 4)) v = app->n_jobs > 0;
        else if (len == 9 && !memcmp(what, "schedules", 9)) v = app->n_schedules > 0;
        else if (len == 6 && !memcmp(what, "topics", 6)) v = app->n_topics > 0;
        else if (len == 6 && !memcmp(what, "cached", 6)) v = app->n_cached_paths > 0;
        else return CB_E_INVALID;
        *out = *out || v;
        if (!bar) break;
        n -= len + 1;
        what = bar + 1;
        if (n == 0) return CB_E_INVALID;   // a trailing '|'
    }
    return CB_OK;
}

// The value of the name `k` (the text between two at signs), or false.
static bool value_of(const cb_app *app, const char *k, size_t n, char *out, size_t cap) {
    const cb_deploy *d = app->deploy;
#define IS(lit) (n == sizeof(lit) - 1 && memcmp(k, lit, n) == 0)
    // An alarm on 80% of a side's units over its 5-minute period: units x 300 s x 0.8.
    if (IS("app")) snprintf(out, cap, "%s", app->name);
    else if (IS("stack")) snprintf(out, cap, "%s", d->stack);
    else if (IS("domain")) snprintf(out, cap, "%s", d->domain);
    else if (IS("table_read")) snprintf(out, cap, "%d", d->table_read);
    else if (IS("table_write")) snprintf(out, cap, "%d", d->table_write);
    else if (IS("index_read")) snprintf(out, cap, "%d", d->index_read);
    else if (IS("index_write")) snprintf(out, cap, "%d", d->index_write);
    else if (IS("table_read_alarm")) snprintf(out, cap, "%d", d->table_read * 240);
    else if (IS("table_write_alarm")) snprintf(out, cap, "%d", d->table_write * 240);
    else if (IS("index_write_alarm")) snprintf(out, cap, "%d", d->index_write * 240);
    else if (IS("cached_list")) {
        // "a", "a and b", "a, b and c".
        size_t o = 0;
        out[0] = 0;
        for (int i = 0; i < app->n_cached_paths && o < cap; i++) {
            const char *sep = i == 0 ? "" : i == app->n_cached_paths - 1 ? " and " : ", ";
            int w = snprintf(out + o, cap - o, "%s%s", sep, app->cached_paths[i]);
            if (w < 0) return false;
            o += (size_t)w;
        }
        if (o >= cap) return false;
    } else return false;
#undef IS
    return true;
}

static void put_schedules(const cb_app *app, cb_buf *out) {
    for (int i = 0; i < app->n_schedules; i++) {
        const cb_schedule *s = &app->schedules[i];
        char id[64], rate[48], dn[64];
        logical_id(s->name, id, sizeof id);
        rate_of(s->deployed_every_s, rate, sizeof rate);
        deployed_name(s, dn, sizeof dn);
        if (i > 0) cb_buf_putc(out, '\n');
        cb_buf_printf(out,
                      "  %s:\n"
                      "    Type: AWS::Scheduler::Schedule\n"
                      "    Properties:\n"
                      "      Name: !Sub \"${FunctionName}-%s\"\n"
                      "      Description: %s\n"
                      "      ScheduleExpression: %s\n"
                      "      FlexibleTimeWindow: { Mode: \"OFF\" }\n"
                      "      State: ENABLED\n"
                      "      Target:\n"
                      "        Arn: !GetAtt Function.Arn\n"
                      "        RoleArn: !GetAtt SchedulerRole.Arn\n"
                      "        Input: '\"%s\"'\n"
                      "        RetryPolicy:\n"
                      "          MaximumRetryAttempts: 0   # the next run is the retry\n",
                      id, dn, s->about, rate, s->name);
    }
}

static void put_cache_behaviours(const cb_app *app, cb_buf *out) {
    for (int i = 0; i < app->n_cached_paths; i++)
        cb_buf_printf(out,
                      "          - PathPattern: %s\n"
                      "            TargetOriginId: function\n"
                      "            ViewerProtocolPolicy: https-only\n"
                      "            AllowedMethods: [GET, HEAD, OPTIONS]\n"
                      "            CachedMethods: [GET, HEAD]\n"
                      "            Compress: true\n"
                      "            CachePolicyId: !Ref PublicReadsCachePolicy\n"
                      "            OriginRequestPolicyId: 59781a5b-3903-41f3-afcb-af62929ccde1  # Managed-CORS-CustomOrigin\n",
                      app->cached_paths[i]);
}

// One publish right per distinct first segment of the families' patterns, in
// the order they first appear ("u/{user}/v" and "s/{doc}": u, then s).
static void put_iot_topics(const cb_app *app, cb_buf *out) {
    for (int i = 0; i < app->n_topics; i++) {
        const char *p = app->topics[i]->pattern;
        size_t len = (size_t)(strchr(p, '/') - p);
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            const char *q = app->topics[j]->pattern;
            seen = (size_t)(strchr(q, '/') - q) == len && memcmp(p, q, len) == 0;
        }
        if (!seen)
            cb_buf_printf(out,
                          "                    - !Sub \"arn:${AWS::Partition}:iot:${AWS::Region}:${AWS::AccountId}:topic/%.*s/*\"\n",
                          (int)len, p);
    }
}

// One kept line, its values filled in.
static int put_line(const cb_app *app, const char *l, size_t n, int at, cb_buf *out, char *err, size_t cap) {
    size_t i = 0;
    while (i < n) {
        const char *a = memchr(l + i, '@', n - i);
        if (!a) {
            cb_buf_append(out, l + i, n - i);
            break;
        }
        size_t s = (size_t)(a - l);
        cb_buf_append(out, l + i, s - i);
        const char *b = memchr(l + s + 1, '@', n - s - 1);
        char v[1024];
        if (!b || !value_of(app, l + s + 1, (size_t)(b - l) - s - 1, v, sizeof v))
            return fail(err, cap, "skeleton line %d: an at sign that names no value (%.*s)", at, (int)(n > 60 ? 60 : n), l);
        cb_buf_puts(out, v);
        i = (size_t)(b - l) + 1;
    }
    cb_buf_putc(out, '\n');
    return CB_OK;
}

int cb_aws_describe(const cb_app *app, const char *skel, size_t n, cb_buf *out, char *err, size_t cap) {
    int rc = check_app(app, err, cap);
    if (rc != CB_OK) return rc;
    if (!skel || n == 0 || skel[n - 1] != '\n') return fail(err, cap, "the skeleton is empty or does not end in a newline");
    bool keep[DESCRIBE_DEPTH + 1] = { true };
    int depth = 0, at = 0;
    for (size_t i = 0; i < n;) {
        const char *l = skel + i;
        size_t len = (size_t)((const char *)memchr(l, '\n', n - i) - l);
        i += len + 1;
        at++;
        if (len < 2 || l[0] != '#' || l[1] != '@') {
            if (keep[depth] && (rc = put_line(app, l, len, at, out, err, cap)) != CB_OK) return rc;
            continue;
        }
        const char *d = l + 2;
        size_t dn = len - 2;
        if (dn > 3 && memcmp(d, "if ", 3) == 0) {
            bool v;
            if (depth == DESCRIBE_DEPTH) return fail(err, cap, "skeleton line %d: \"#@if\" nested past %d", at, DESCRIBE_DEPTH);
            if (declares(app, d + 3, dn - 3, &v) != CB_OK) return fail(err, cap, "skeleton line %d: \"#@if\" of an unknown kind", at);
            keep[depth + 1] = keep[depth] && v;
            depth++;
        } else if (dn == 3 && memcmp(d, "end", 3) == 0) {
            if (depth == 0) return fail(err, cap, "skeleton line %d: \"#@end\" with no \"#@if\"", at);
            depth--;
        } else if (dn >= 1 && d[0] == '#') {
            // a comment of the skeleton's own
        } else if (dn == 9 && memcmp(d, "schedules", 9) == 0) {
            if (keep[depth]) put_schedules(app, out);
        } else if (dn == 16 && memcmp(d, "cache_behaviours", 16) == 0) {
            if (keep[depth]) put_cache_behaviours(app, out);
        } else if (dn == 10 && memcmp(d, "iot_topics", 10) == 0) {
            if (keep[depth]) put_iot_topics(app, out);
        } else {
            return fail(err, cap, "skeleton line %d: an unknown directive", at);
        }
    }
    if (depth != 0) return fail(err, cap, "the skeleton ends inside \"#@if\"");
    return CB_OK;
}

int cb_aws_settings(const cb_app *app, cb_buf *out, char *err, size_t cap) {
    int rc = check_app(app, err, cap);
    if (rc != CB_OK) return rc;
    const cb_deploy *d = app->deploy;
    cb_buf_printf(out, "app=%s\nstack=%s\ndomain=%s\nunits_read=%d\nunits_write=%d\n", app->name, d->stack, d->domain,
                  d->table_read + 2 * d->index_read, d->table_write + 2 * d->index_write);
    return CB_OK;
}

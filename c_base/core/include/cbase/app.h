// cbase/app.h - what a product hands the platform: ONE const struct, mostly
// data (docs/PLATFORM.md section 2). The product is chosen at link time, as
// the provider is: it defines cb_main_app, which each provider's main serves
// (ctx->app) and every store opened in the process carries (cb_app store);
// boot runs its self-test and registers its routes after the platform's own
// (/health, /admin, the provider's).
//
// It grows stage by stage (docs/PLATFORM.md 8.6): jobs and schedules, topic
// families, cached paths, collections and admin panels join it as the code
// that knows them moves behind the platform.
#ifndef CB_APP_H
#define CB_APP_H

#include "net/router.h"

#include <stddef.h>
#include <stdint.h>

// One module's routes: a table of n of them (router.h cb_route), registered
// in order. Every route the app declares is added to the one router; one that
// shadows a platform route (the same method and pattern) is CB_E_CONFLICT.
typedef struct {
    const cb_route *routes;
    int             n;
} cb_route_list;

// A JOB: work started by name with a key (jobs.h cb_job_start), run where
// the provider runs it (a pool thread, a fresh Lambda invocation). run
// answers CB_OK, CB_E_NOT_FOUND when there is nothing to do for the key (a
// retry cannot help, so it is not a failure), or another CB_E_*.
typedef struct {
    const char *name;   // [a-z0-9_], at most CB_JOB_NAME_MAX: "bot_drive"
    int (*run)(cb_ctx *ctx, const char *key);
} cb_job;

// A SCHEDULE: work run every `every_ms` (EventBridge Scheduler under AWS,
// whose literal input is the name; a loop timer under serve). run answers
// >= 0 when it ran.
typedef struct {
    const char *name;      // [a-z0-9_], at most CB_JOB_NAME_MAX: "scan"
    int64_t     every_ms;  // serve's period (divided by CBASE_BOT_SPEEDUP)
    int (*run)(cb_ctx *ctx, int64_t now_ms);
    // What a deployed schedule is made of (a provider's describe, docs/PLATFORM.md
    // 8.5): its period in seconds (a whole number of minutes, hours or days), its
    // name after the stack's ("bot-scan": [a-z0-9-]; NULL for `name` with '_' as
    // '-') and one sentence about it (no ": " and no " #", so it is plain text in
    // any infrastructure file).
    int64_t     deployed_every_s;
    const char *deployed_name;
    const char *about;
} cb_schedule;

// A TOPIC FAMILY: a set of push topics (realtime.h), as a pattern over
// {user} and {doc}. It must start "u/{user}/" (the user's own: only that
// user may subscribe) or "s/" (anyone signed in), the stream's one policy
// (stream_mqtt.h); boot checks it (cb_rt_families_check).
typedef struct {
    const char *name;      // for the logs: "views"
    const char *pattern;   // "u/{user}/v", "s/{doc}"
} cb_topic_family;

// An ADMIN PANEL's call: /admin/api/<name>, behind the console's token
// (admin/admin.h), answered through admin.h's helpers and core_json.h's
// admin shapes.
typedef struct {
    const char *method;
    const char *name;   // the path after /admin/api/
    cb_handler  fn;
} cb_admin_call;

// WHERE THE APP IS DEPLOYED, for a provider's describe and its deploy
// (docs/PLATFORM.md 8.5): the names a deployment's resources take and the
// store's provisioned units. A provider that bills none ignores the units.
typedef struct {
    const char *stack;    // the deployment's name, and its function's and table's: [a-z0-9-]
    const char *domain;   // the public host the clients use
    int table_read, table_write;   // the store's provisioned units: the table
    int index_read, index_write;   // and each of its two indexes (items.h)
} cb_deploy;

struct cb_app {
    const char          *name;            // "foolish": the log tag, later the stack and the key namespace
    const cb_route_list *const *routes;   // n_route_lists of them
    int                  n_route_lists;
    // Runs once at boot, before anything is served: CB_OK, or a CB_E_* with
    // a sentence in err, and the process must not serve. NULL: none.
    int (*selftest)(char *err, size_t cap);
    const cb_job        *jobs;
    int                  n_jobs;
    const cb_schedule   *schedules;
    int                  n_schedules;
    const cb_topic_family *const *topics;   // what the app publishes to (realtime.h)
    int                  n_topics;
    // The public reads an edge may cache, whose answers the app marks stale
    // (app/edge.h): bit i of a mark is cached_paths[i]. At most CB_EDGE_MAX.
    const char *const   *cached_paths;
    int                  n_cached_paths;
    // Its rows and its part of an account, on every store (store/store.h).
    const struct cb_store_app *store;
    // Delete-account (docs/PLATFORM.md 2.5): the one implementation, which
    // the console's users/delete runs. CB_OK; a refusal's kernel code and
    // detail in *code and *detail. NULL: accounts are not deleted.
    int (*delete_account)(cb_ctx *ctx, const char *user_id, int *code, int *detail);
    // The console: the app's panels after the platform's, and the overview's
    // one counter line (>= 0, or < 0 when the store could not say).
    const cb_admin_call *admin_calls;
    int                  n_admin_calls;
    int (*admin_count)(cb_ctx *ctx, int64_t now_ms);
    // Where it is deployed; NULL: it cannot be (a test app).
    const cb_deploy     *deploy;
};

// The app this binary serves: the product defines it, one per link.
extern const cb_app *const cb_main_app;

#endif

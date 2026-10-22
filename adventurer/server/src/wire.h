// wire.h - THE BINARY SURFACE the web reads: fixed little-endian layouts, no JSON. The C compiler
// owns every offset: tools/wiregen.c prints the TypeScript decoder (web/src/gen/wire.ts) from
// offsetof and sizeof over these structs, and ADV_WIRE_HASH (sg-style, from the same tool) is
// the first field of every answer so a page built from an older layout refuses instead of
// misreading.
//
//   GET  /adv/ops                      adv_w_ops
//   GET  /adv/regions                  adv_w_list (n) then n x adv_w_region
//   GET  /adv/users                    adv_w_list (n) then n x adv_w_user
//   GET  /adv/user/<id>                adv_w_user_stats then n_regions x adv_w_region_progress
//   GET  /adv/leaders/<region>         adv_w_leaders
//   GET  /adv/feed (WebSocket)         binary frames: adv_w_activity (kind 1) or adv_w_ops (kind 2)
//   GET  /adv/tile/<uid>/<z>/<x>/<y>.mvt?t=bike|foot|all   MVT (mvt.h), ETag "<uid>.<version>.<t>"
//   GET  /adv/base/<z>/<x>/<y>.mvt     MVT, the roads layer alone
//   POST /adv/upload                   adv_trace (trace.h); answers adv_w_accepted
//   POST /adv/users                    adv_w_list (n) then n x adv_w_user_def (the generator's roster)
//
// Every struct is laid out so that no padding is implicit: a _Static_assert pins each size.
#ifndef ADV_WIRE_H
#define ADV_WIRE_H

#include "graph.h"

#include <stdint.h>

#define ADV_W_KIND_ACTIVITY 1
#define ADV_W_KIND_OPS      2
#define ADV_W_NAME          32

// A list's head: how many records follow.
typedef struct {
    uint32_t hash;   // ADV_WIRE_HASH
    uint32_t n;
} adv_w_list;

// A user as the generator defines one (POST /adv/users) and as the list answers (GET /adv/users).
enum { ADV_UK_COMMUTER = 1, ADV_UK_EXPLORER = 2, ADV_UK_RUNNER = 3, ADV_UK_WALKER = 4, ADV_UK_TOURIST = 5 };
typedef struct {
    uint32_t id;         // 1..ADV_MAX_USERS
    uint8_t  kind;       // ADV_UK_*
    uint8_t  name_len;
    uint16_t pad;
    int32_t  home_lat, home_lon;
    char     name[ADV_W_NAME];
} adv_w_user_def;

typedef struct {
    uint32_t id;
    uint8_t  kind;
    uint8_t  name_len;
    uint16_t pad;
    uint32_t n_activities;
    uint32_t version;              // the coverage version (the tiles' ETag)
    float    unique_m[ADV_T_N];    // bike, foot, combined
    int32_t  home_lat, home_lon;
    char     name[ADV_W_NAME];
} adv_w_user;

typedef struct {
    uint16_t id;
    uint8_t  name_len;
    uint8_t  level;                // OSM admin_level (graph.h adv_region)
    float    len_m[ADV_T_N];
    int32_t  c_lat, c_lon;
    int32_t  min_lat, min_lon, max_lat, max_lon;
    char     name[ADV_REGION_NAME];
} adv_w_region;

typedef struct {
    uint32_t hash;
    uint32_t id;
    uint32_t version;
    uint32_t n_activities;
    uint32_t n_regions;            // adv_w_region_progress records that follow
    uint32_t n_super_unique_segs;
    float    unique_m[ADV_T_N];
    float    super_unique_m;
    int64_t  last_activity_ms;
    int64_t  first_activity_ms;
} adv_w_user_stats;

// Progress in one region: covered length per type, this month's and this year's new length,
// the achievements earned (bit i of achievements[type]: threshold i of ADV_ACH_PCT) and the
// user's rank (1..3, 0 for none) in the region's monthly and yearly challenge per type.
#define ADV_ACH_N 5
static const uint8_t ADV_ACH_PCT[ADV_ACH_N] = { 25, 50, 75, 90, 99 };
typedef struct {
    uint16_t region;
    uint8_t  achievements[ADV_T_N];
    uint8_t  pad[3];
    float    len_m[ADV_T_N];
    float    month_m[ADV_T_N];
    float    year_m[ADV_T_N];
    uint8_t  rank_month[ADV_T_N];
    uint8_t  rank_year[ADV_T_N];
    uint16_t pad2;
} adv_w_region_progress;

typedef struct {
    uint32_t user;    // 0: an empty slot
    float    m;
} adv_w_leader;
typedef struct {
    uint32_t     hash;
    uint16_t     region;
    uint16_t     pad;
    adv_w_leader month[ADV_T_N][3];
    adv_w_leader year[ADV_T_N][3];
} adv_w_leaders;

// One processed upload, as the feed announces it (kind ADV_W_KIND_ACTIVITY).
#define ADV_AF_BACKFILL 0x1
#define ADV_AF_REJECTED 0x2
#define ADV_AF_TRUTH    0x4   // accuracy and false_m mean something
typedef struct {
    uint8_t  kind;
    uint8_t  type;        // ADV_ACT_*
    uint8_t  flags;       // ADV_AF_*
    uint8_t  reason;      // ADV_MATCH_* when rejected
    uint32_t user;
    uint32_t activity;    // the server's id (the journal's row)
    uint32_t gen_id;
    uint32_t n_points;
    float    matched_m;
    float    new_m[ADV_T_N];
    float    accuracy;    // matched true length / true length, 0..1
    float    false_m;     // matched length not in the truth
    uint32_t cpu_us;      // the match and the merge
    uint32_t e2e_us;      // the upload's arrival to this frame
    uint32_t queue_live;  // uploads waiting, each lane
    uint32_t queue_backfill;
    uint32_t pad;
    int64_t  at_ms;
} adv_w_activity;

// What POST /adv/upload answers (the upload is queued; the feed tells the rest).
typedef struct {
    uint32_t hash;
    uint32_t activity;
    uint32_t queue_live, queue_backfill;
} adv_w_accepted;

// The server's operating numbers (GET /adv/ops, and every second on the feed as kind 2).
typedef struct {
    uint8_t  kind;
    uint8_t  pad[3];
    uint32_t hash;
    int64_t  at_ms;
    uint32_t uptime_s;
    uint32_t users;
    uint32_t activities;          // accepted, ever
    uint32_t rejected;
    uint32_t queue_live, queue_backfill;
    float    act_per_s;           // over the last 10 s
    float    tiles_per_s;
    uint32_t act_cpu_p50_us, act_cpu_p99_us;   // match + merge, since start
    uint32_t tile_p50_us, tile_p99_us;         // render, since start
    uint32_t e2e_p50_us, e2e_p99_us;           // arrival to feed frame, since start
    uint32_t rss_mb;
    uint32_t feed_conns;
    uint32_t http_conns;
    uint32_t pad2;
    uint64_t journal_bytes;
    float    accuracy_avg;        // over every activity with a truth
    uint32_t segs, regions;
    float    total_len_km[ADV_T_N];
    uint32_t threads;
    uint32_t recovered;           // activities replayed from the journal at start
} adv_w_ops;

_Static_assert(sizeof(adv_w_list) == 8, "adv_w_list");
_Static_assert(sizeof(adv_w_user_def) == 48, "adv_w_user_def");
_Static_assert(sizeof(adv_w_user) == 68, "adv_w_user");
_Static_assert(sizeof(adv_w_region) == 88, "adv_w_region");
_Static_assert(sizeof(adv_w_user_stats) == 56, "adv_w_user_stats");
_Static_assert(sizeof(adv_w_region_progress) == 52, "adv_w_region_progress");
_Static_assert(sizeof(adv_w_leaders) == 152, "adv_w_leaders");
_Static_assert(sizeof(adv_w_activity) == 72, "adv_w_activity");
_Static_assert(sizeof(adv_w_accepted) == 16, "adv_w_accepted");
_Static_assert(sizeof(adv_w_ops) == 128, "adv_w_ops");

// The layout hash the generated decoder checks (tools/wiregen.c computes it over every field's
// name, offset and size and prints the constant it used; wire.c holds the same computation so
// the server's copy is the compiler's, never retyped).
uint32_t adv_wire_hash(void);

#endif

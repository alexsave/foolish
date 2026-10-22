// world.h - THE STATE: every user's coverage and stats, every region's progress and leaderboards,
// every segment's owner, authoritative in memory, with the journal (SQLite) as the append-only
// record each result is written to and rebuilt from at start.
//
// ONE OWNER PER FACT. A user's coverage, totals and region progress change only under the user's
// lock (adv_world_user_lock), by adv_world_apply, which is also what recovery calls for each
// journaled activity, so a restart rebuilds exactly what the live path built. The leaderboards
// change under the boards lock, the owners table under relaxed atomics (a racy read by a tile
// is a frame late, never wrong). Readers (the HTTP routes, the tiles) take the user's lock for
// the few microseconds a copy or a render needs; on the loop thread they TRY it and answer
// CB_E_WOULD_BLOCK instead of waiting (router.h CB_ROUTE_INLINE), so a merge in progress never
// stalls the loop.
#ifndef ADV_WORLD_H
#define ADV_WORLD_H

#include "coverage.h"
#include "graph.h"
#include "wire.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define ADV_MAX_USERS   65535
#define ADV_USER_LOCKS  4096   // striped: lock i guards users with id % ADV_USER_LOCKS == i

// Progress in one region, kept only for regions the user has touched (sorted by region).
typedef struct {
    uint16_t region;
    uint8_t  ach[ADV_T_N];     // achievement bits earned (wire.h ADV_ACH_PCT)
    uint8_t  pad;
    float    len_m[ADV_T_N];   // covered length in the region
    float    month_m[ADV_T_N]; // new length in the current month (month_key)
    float    year_m[ADV_T_N];
    uint32_t month_key, year_key;   // the period the sums belong to (adv_month_key / adv_year_key)
} adv_rprog;

typedef struct {
    uint32_t  id;         // 0: no such user
    uint8_t   kind;
    uint8_t   name_len;
    char      name[ADV_W_NAME];
    adv_pt    home;
    adv_cov   cov;
    uint32_t  version;    // bumped by every apply that changed the coverage (the tiles' ETag)
    uint32_t  n_activities;
    float     unique_m[ADV_T_N];
    int64_t   first_ms, last_ms;
    adv_rprog *rp;
    uint16_t  n_rp, cap_rp;
} adv_user;

// A region's challenge boards: top 3 per period and type, for the current period.
typedef struct {
    adv_w_leader month[ADV_T_N][3], year[ADV_T_N][3];
    uint32_t     month_key, year_key;
} adv_board;

// One activity's result, as applied and as journaled.
typedef struct {
    uint32_t  activity;     // the journal's row id (assigned at apply)
    uint32_t  user;
    uint8_t   type;         // ADV_ACT_*
    uint8_t   flags;        // ADV_TRF_*
    int64_t   start_ms;
    uint32_t  gen_id;
    uint32_t  n_points;
    float     matched_m;
    float     accuracy;     // -1: no truth
    uint32_t  cpu_us;
    const adv_hit *hits;
    uint32_t  n_hits;
} adv_result;

typedef struct {
    float new_m[ADV_T_N];   // what the apply credited
} adv_applied;

typedef struct adv_world {
    const adv_graph *g;
    adv_user        *users;         // indexed by id (0 unused)
    uint32_t         max_users;     // ADV_MAX_USERS + 1 slots
    atomic_uint      n_users;
    pthread_mutex_t  user_locks[ADV_USER_LOCKS];
    _Atomic uint32_t *owners;       // per segment: 0, a user id, ADV_OWNER_MANY (mvt.h)
    // EVERYONE (coverage.h adv_all_seg): the union and the user counts per segment, the length
    // nobody had before per type in centimetres (an integer, so an atomic add), the same per
    // region, how many segments have any user per type, and a version bumped by every apply
    // that changed any of it (the composite tiles' ETag).
    adv_all_seg     *all;
    _Atomic uint64_t all_cm[ADV_T_N];
    _Atomic uint64_t *all_region_cm;   // n_regions x ADV_T_N
    _Atomic uint32_t all_segs[ADV_T_N];
    atomic_uint      all_version;
    adv_board       *boards;        // per region
    pthread_mutex_t  boards_lock;
    atomic_uint      next_activity; // the next activity id
    atomic_ullong    activities, rejected;
    // The accuracy average over every activity that carried a truth.
    pthread_mutex_t  acc_lock;
    double           acc_sum;
    uint64_t         acc_n;
} adv_world;

int  adv_world_init(adv_world *w, const adv_graph *g);   // CB_OK or CB_E_NOMEM
void adv_world_free(adv_world *w);

// Users. define: insert or replace (the generator's roster at every start); CB_E_INVALID for an
// id out of range or a bad name. get: NULL for none (read the row under the user's lock to copy it).
int  adv_world_user_define(adv_world *w, const adv_w_user_def *d);
adv_user *adv_world_user(adv_world *w, uint32_t id);
pthread_mutex_t *adv_world_user_lock(adv_world *w, uint32_t id);

// THE APPLY: merges r->hits into the user's coverage and credits new length to the user, the
// region progress, the achievements, the boards and the owners. Under the user's lock (the
// caller holds it; apply takes the boards lock itself). *out the new length per type. CB_OK,
// CB_E_NOT_FOUND for no such user, CB_E_NOMEM.
int adv_world_apply(adv_world *w, adv_user *u, const adv_result *r, adv_applied *out);

// Everyone's length per type in metres, and a region's.
static inline float adv_world_all_m(const adv_world *w, int type) {
    return (float)(atomic_load_explicit((_Atomic uint64_t *)&w->all_cm[type], memory_order_relaxed) / 100.0);
}
static inline float adv_world_all_region_m(const adv_world *w, uint16_t region, int type) {
    return (float)(atomic_load_explicit((_Atomic uint64_t *)&w->all_region_cm[(size_t)region * ADV_T_N + type], memory_order_relaxed) / 100.0);
}
// The ADV_TOP_N (wire.h) most-travelled segments of a type (by distinct users, then length)
// into out: how many were written (fewer when fewer segments have a user). A scan of every segment.
typedef struct { uint32_t seg; uint16_t users; } adv_top_seg;
uint32_t adv_world_top(const adv_world *w, int type, adv_top_seg out[ADV_TOP_N]);

// The super-unique view of a user: segments only this user has travelled (owner == id), their
// length, from the coverage (under the user's lock).
void adv_world_super_unique(const adv_world *w, const adv_user *u, uint32_t *n_segs, float *len_m);

// Period keys from an epoch-ms instant (UTC).
uint32_t adv_month_key(int64_t ms);
uint32_t adv_year_key(int64_t ms);
// A user's rank (1..3, 0 none) on a region's board for a period and type, under the boards lock.
int adv_world_rank(adv_world *w, uint16_t region, bool year, int type, uint32_t user);
void adv_world_board_copy(adv_world *w, uint16_t region, adv_board *out);

#endif

// fake.h - THE FAKE WORLD: the users, routes, GPS samples and hostile uploads adv-gen plays at
// the server in place of Strava. Pure (no sockets, no clock, no allocation): everything is a
// function of the graph, a seed and the caller's scratch, so a unit test replays what the
// generator sent and a restart posts the same roster.
//
// A USER is defined by (seed, id) and the world alone - its home, its kind, its name - so users
// are made in any order and the roster is the same across restarts.
//
// THE WORLD (fake_world) is what the graph says about where people live and where they go, read
// once: a home is drawn with probability proportional to the length of a street people live on
// times its region's DENSITY, the region's population (its OSM boundary's population= tag) over
// that region's length of such streets, so a region's share of homes is its share of the people,
// and inside it homes follow the streets. A region with no population tag gets the mean density
// of those with one (all regions alike when none has one: homes follow street length). A home's
// URBANITY (0 suburb .. 1 dense city) is its region's density on a log scale, and it sets the
// kinds (the city has more runners and walkers, the suburbs more road riders), the distances, and
// how often an activity starts at an ATTRACTOR instead of the door: a path, footway, pedestrian
// street or cycleway near home for a run or a walk (the parks and the greenways), a cycleway or a
// signed cycle route (route=bicycle, graph.h ADV_SEGF_BIKEROUTE) for a ride, drawn by length so
// the big networks pull hardest. Nothing is drawn by hand: every weight is a tag of the map. A ROUTE is a walk on the graph from
// the user's home node: a list of segments with the direction travelled, the fraction of the
// first and the last actually covered, and the TRUTH hits that walk produced (trace.h's
// trailer), which is what scores the matcher. The SAMPLER turns a route into GPS points at one
// per second with the errors a phone makes (an AR(1) drift, dropouts, spikes). THE HOSTILE MAKERS
// produce the uploads the server must refuse or survive, each named in a table so the feed can
// say which.
#ifndef ADV_FAKE_H
#define ADV_FAKE_H

#include "graph.h"
#include "trace.h"
#include "wire.h"

#include <stdbool.h>
#include <stdint.h>

// ---- the PRNG (xorshift64*: one word of state, deterministic across platforms) -------------
typedef struct {
    uint64_t s;
} fake_rng;
void     fake_rng_seed(fake_rng *r, uint64_t seed);
uint64_t fake_rng_u64(fake_rng *r);
uint32_t fake_rng_below(fake_rng *r, uint32_t n);   // 0..n-1 (0 when n is 0)
double   fake_rng_unit(fake_rng *r);                 // [0, 1)
double   fake_rng_range(fake_rng *r, double lo, double hi);
double   fake_rng_normal(fake_rng *r);               // N(0, 1)

// ---- users ----------------------------------------------------------------------------------
// The visited set is what tells an explorer from a commuter; it is small on purpose (a load
// generator holds thousands of users) and stops growing when FAKE_VISITED_FILL entries are in,
// so a probe always ends.
#define FAKE_VISITED_CAP  4096   // a power of two
#define FAKE_VISITED_FILL 3072
#define FAKE_NO_SEG       0xFFFFFFFFu

typedef struct {
    const adv_graph *g;
    double  *home_cum;       // n_segs: running sum of each segment's home weight
    double  *foot_cum;       // n_segs: running sum of its weight as a run or walk attractor
    double  *bike_cum;       // n_segs: and as a ride attractor
    float   *region_urban;   // n_regions: 0 (suburb) .. 1 (dense city)
    float   *region_density; // n_regions: people per km of street lived on (the tag's, or the mean)
    uint32_t n_tagged;       // regions with a population tag
} fake_world;

// The world of g (three arrays of n_segs doubles: 13 MB for New York). CB_OK or CB_E_NOMEM; g
// must outlive it.
int  fake_world_init(fake_world *w, const adv_graph *g);
void fake_world_free(fake_world *w);
// The urbanity of a density (people per km of street lived on): 0 at FAKE_DENSITY_SUBURB and
// below, 1 at FAKE_DENSITY_CITY and above, log-linear between.
#define FAKE_DENSITY_SUBURB 500.0
#define FAKE_DENSITY_CITY   2000.0
float fake_urbanity(double density);

typedef struct {
    adv_w_user_def def;      // the roster record (POST /adv/users)
    const fake_world *w;
    float    urban;          // the home region's urbanity
    uint32_t home_seg;       // the segment whose first point is home
    uint32_t home_node;      // its node_a
    uint32_t start_node;     // where the next route starts and returns to (home, or an attractor)
    uint32_t n_activities;   // gen_id of the next activity is n_activities + 1
    uint32_t n_visited;
    uint32_t visited[FAKE_VISITED_CAP];   // open addressing, FAKE_NO_SEG empty
} fake_user;

// The user (seed, id): a home drawn by the world's weights (the first point of the segment), a
// kind by its urbanity (suburb: 30% commuter, 35% explorer, 15% runner, 10% walker, 10% tourist;
// city: 30% commuter, 10% explorer, 32% runner, 23% walker, 5% tourist; between, in proportion),
// a two-part name. The def has name_len set and the name NUL-terminated. home_seg and urban out
// may be NULL.
void fake_user_def(const fake_world *w, uint64_t seed, uint32_t id, adv_w_user_def *def, uint32_t *home_seg, float *urban);
// Users 1..n into out[0..n).
void fake_users_make(const fake_world *w, uint32_t n, uint64_t seed, adv_w_user_def *out);
// A highway class people live on: residential, living street, tertiary, unclassified, secondary
// (the avenues of a city are lined with homes).
bool fake_home_class(uint8_t hclass);
// A user's play-time state from its definition: the def, the home node, nothing visited.
void fake_user_init(const fake_world *w, fake_user *u, uint64_t seed, uint32_t id);
bool fake_user_visited(const fake_user *u, uint32_t seg);
// The activity type a kind produces (ADV_ACT_*) and the access bit it needs (ADV_ACC_*).
uint8_t fake_kind_act(uint8_t kind);
uint8_t fake_act_access(uint8_t act);
const char *fake_kind_name(uint8_t kind);

// ---- routes ---------------------------------------------------------------------------------
#define FAKE_ROUTE_MAX_SEGS 2048

typedef struct {
    uint32_t seg;
    uint8_t  reversed;   // travelled from node_b to node_a
    uint8_t  pad[3];
} fake_leg;

typedef struct {
    fake_leg  leg[FAKE_ROUTE_MAX_SEGS];
    uint32_t  n;
    double    f0;        // where on the first leg travel starts (0..0.5 of its length, in the travelled direction)
    double    f1;        // where on the last leg it ends (0.5..1); with one leg, f0 < f1 on it
    double    len_m;     // the travelled length
    uint32_t  n_truth;
    adv_truth truth[FAKE_ROUTE_MAX_SEGS];   // sorted, unique: the trailer as written
    bool      stuck;     // the walk ended at a dead end with nowhere left to go
    bool      capped;    // the walk hit FAKE_ROUTE_MAX_SEGS
} fake_route_out;

// A walk of about target_m metres for u with activity type act, from u->start_node (home unless
// fake_activity chose an attractor) and tending back to it past the half-way point: at each node the next segment is drawn among the node's segments
// the type may use, never the one just travelled unless it is the only way (a dead end turns
// around), weighted by the kind (an explorer avoids its visited set, a commuter seeks it, a
// tourist is uniform) and by the road (a ride favours cycleways and signed routes, a run or walk
// paths and footways, both shun primary roads and trunks). The route's segments are then marked
// visited. n is 0 when the home node
// has no segment the type may use.
void fake_route(const adv_graph *g, fake_user *u, fake_rng *r, uint8_t act, double target_m, fake_route_out *out);
// The length target an activity type draws (bike 20-40 km, run 6-12, walk 3-6): about 5000
// points at the type's speed, well under ADV_TRACE_MAX_PTS even with jitter.
double fake_target_m(fake_rng *r, uint8_t act);
// The travelled length of a route (its truth should equal it within a sixteenth per segment).
double fake_route_len_m(const adv_graph *g, const fake_route_out *route);

// ---- the sampler ----------------------------------------------------------------------------
typedef struct {
    double sigma_m;      // the GPS error's stationary standard deviation per axis (4)
    double rho;          // the AR(1) coefficient per second (0.92)
    double dropout_p;    // probability of one dropout of 10..120 s somewhere in the activity (0.3)
    double spike_p;      // per point, a 50..500 m spike (1/400)
} fake_noise;
void fake_noise_default(fake_noise *n);

// GPS points along the route at the type's speed (bike 4-8 m/s, run 2.4-3.6, walk 1.1-1.6, the
// mean drawn per activity, +-15% per second), one per second with up to 200 ms of timestamp
// jitter (non-decreasing), the error applied in the local frame. Stops at cap points: a route
// longer than that ends early and its truth then over-states what was sampled, which the
// accuracy metric counts as missed. The number of points written.
uint32_t fake_sample(const adv_graph *g, const fake_route_out *route, uint8_t act, fake_rng *r,
                     const fake_noise *noise, adv_trace_pt *out, uint32_t cap);

// ---- an activity, whole --------------------------------------------------------------------
// One activity's parts, in a scratch the caller owns (about 800 KB: a per-thread one).
typedef struct {
    adv_trace_hdr hdr;
    uint32_t      n_pts;
    adv_trace_pt  pts[ADV_TRACE_MAX_PTS];
    uint32_t      n_truth;
    adv_truth     truth[FAKE_ROUTE_MAX_SEGS];
    double        route_m;    // the route's travelled length
    bool          truncate;   // a hostile: the sender cuts FAKE_TRUNCATE_BYTES off the body
    int           hostile;    // 0, or the hostile kind (1-based) this is
} fake_act;
#define FAKE_TRUNCATE_BYTES 7

// The type from the user's kind, a target by kind and urbanity, a start (home, or an attractor
// within reach: fake.c START), a route, the samples (noise NULL: the defaults with
// sigma drawn 2-8 m per activity), the truth, the header (user, gen_id, start_ms, flags). False
// when the route is empty (nothing to upload).
bool fake_activity(const adv_graph *g, fake_user *u, fake_rng *r, int64_t start_ms, uint8_t flags,
                   const fake_noise *noise, fake_route_out *route, fake_act *a);
// The body bytes (adv_trace_write): the size, or 0 when it does not fit cap. The size the SENDER
// must put on the wire is fake_act_wire_len (truncation is the sender's act, not the writer's).
size_t fake_act_write(const fake_act *a, uint8_t *out, size_t cap);
static inline size_t fake_act_wire_len(const fake_act *a, size_t written) {
    return a->truncate && written > FAKE_TRUNCATE_BYTES ? written - FAKE_TRUNCATE_BYTES : written;
}
// The body's worst case: every cap at once.
#define FAKE_BODY_CAP (32 + (size_t)ADV_TRACE_MAX_PTS * 12 + 4 + (size_t)ADV_TRACE_MAX_TRUTH * 8)

// ---- the hostile makers --------------------------------------------------------------------
typedef struct {
    const char *name;
    bool parses;     // adv_trace_parse accepts the body as written (the matcher then decides)
    bool truncate;   // the sender cuts FAKE_TRUNCATE_BYTES off the body (and then it must not parse)
} fake_hostile_kind;
#define FAKE_HOSTILE_N 12
extern const fake_hostile_kind FAKE_HOSTILE[FAKE_HOSTILE_N + 1];   // [0] unused: kinds are 1-based

// Hostile upload `kind` (1..FAKE_HOSTILE_N) for u into a, with a->hostile and a->truncate set.
// Kinds built on a good activity (6, 8, 10, 11) walk a route like fake_activity does; false when
// that route is empty.
bool fake_hostile(const adv_graph *g, int kind, fake_user *u, fake_rng *r, int64_t start_ms,
                  fake_route_out *route, fake_act *a);

#endif

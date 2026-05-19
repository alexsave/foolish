// Opponent pool for GRPO self-play.
//
// Each pool member is either the permanent handwritten anchor or a past
// dynamite checkpoint. The anchor is never evicted; non-anchor members are
// LRU-evicted past the cap. Members are sampled by per-game-seat
// weighting, biased toward recently-added entries so newer policies see
// stronger opposition.
//
// Threading: members are read-only after add_*. Sampling and forward use
// only borrowed pointers. Concurrent reads from multiple worker threads
// during game collection are safe.
#ifndef CNITRO_GRPO_POOL_H
#define CNITRO_GRPO_POOL_H

#include "game.h"
#include "legal.h"
#include "grpo_net.h"
#include <stdbool.h>
#include <stdint.h>

#define GRPO_POOL_CAP 32        // hard cap; soft target ~10
#define GRPO_POOL_NAME_LEN 64

typedef enum {
    POOL_HANDWRITTEN = 0,
    POOL_DYNAMITE    = 1,
} PoolMemberKind;

typedef struct {
    PoolMemberKind kind;
    char     name[GRPO_POOL_NAME_LEN];
    char     ckpt_path[256];     // empty if handwritten anchor
    GrpoNet *net;                // borrowed; NULL for handwritten
    bool     is_anchor;          // never evict
    int      iter_added;
} PoolMember;

typedef struct {
    PoolMember members[GRPO_POOL_CAP];
    int        n_members;
    int        soft_cap;
    int        next_iter;        // monotone counter passed to iter_added
    // Sampling weight per member = 1 + recency_bias * iter_added.
    //   0.0 = uniform over members
    //   1.0 = linear-by-recency (default)
    float      recency_bias;
} GrpoPool;

void grpo_pool_init(GrpoPool *pool, int soft_cap);
void grpo_pool_set_recency_bias(GrpoPool *pool, float bias);
// Add the permanent handwritten anchor. Call once at startup.
void grpo_pool_add_handwritten_anchor(GrpoPool *pool);
// Add a dynamite checkpoint. `net` is borrowed — caller keeps ownership
// for the pool's lifetime. Returns the index of the newly added member,
// or -1 on failure (pool full and nothing evictable).
int  grpo_pool_add_dynamite(GrpoPool *pool, const char *name,
                            const char *ckpt_path, GrpoNet *net);
// Drop a non-anchor member by index. Anchors are silently kept.
void grpo_pool_evict_oldest_non_anchor(GrpoPool *pool);

// Sample one pool member index, biased toward recent. RNG is xorshift32
// keyed by the caller-supplied state.
int  grpo_pool_sample(const GrpoPool *pool, uint32_t *rng);

// Dispatch the chosen pool member's policy at a seat. Workspace must be
// preallocated by the caller (per-thread, to avoid contention).
int  grpo_pool_member_choose(const PoolMember *m,
                             const Game *g, int bot_idx,
                             const LegalMoves *moves,
                             GrpoWorkspace *ws);

#endif

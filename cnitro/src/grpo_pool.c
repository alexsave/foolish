// Opponent pool implementation. See grpo_pool.h.

#include "grpo_pool.h"
#include "strategy.h"
#include <string.h>
#include <stdio.h>

void grpo_pool_init(GrpoPool *pool, int soft_cap) {
    memset(pool, 0, sizeof(*pool));
    pool->soft_cap = soft_cap > GRPO_POOL_CAP ? GRPO_POOL_CAP : soft_cap;
    pool->next_iter = 0;
}

void grpo_pool_add_handwritten_anchor(GrpoPool *pool) {
    if (pool->n_members >= GRPO_POOL_CAP) return;
    PoolMember *m = &pool->members[pool->n_members++];
    m->kind = POOL_HANDWRITTEN;
    snprintf(m->name, GRPO_POOL_NAME_LEN, "handwritten");
    m->ckpt_path[0] = 0;
    m->net = NULL;
    m->is_anchor = true;
    m->iter_added = pool->next_iter++;
}

void grpo_pool_evict_oldest_non_anchor(GrpoPool *pool) {
    int oldest = -1;
    int oldest_iter = 0;
    for (int i = 0; i < pool->n_members; i++) {
        if (pool->members[i].is_anchor) continue;
        if (oldest < 0 || pool->members[i].iter_added < oldest_iter) {
            oldest = i; oldest_iter = pool->members[i].iter_added;
        }
    }
    if (oldest < 0) return;
    // Shift down.
    for (int i = oldest; i + 1 < pool->n_members; i++) pool->members[i] = pool->members[i + 1];
    pool->n_members--;
}

int grpo_pool_add_dynamite(GrpoPool *pool, const char *name,
                           const char *ckpt_path, GrpoNet *net) {
    if (pool->n_members >= pool->soft_cap) grpo_pool_evict_oldest_non_anchor(pool);
    if (pool->n_members >= GRPO_POOL_CAP) return -1;
    PoolMember *m = &pool->members[pool->n_members];
    m->kind = POOL_DYNAMITE;
    snprintf(m->name, GRPO_POOL_NAME_LEN, "%s", name ? name : "dynamite");
    snprintf(m->ckpt_path, sizeof(m->ckpt_path), "%s", ckpt_path ? ckpt_path : "");
    m->net = net;
    m->is_anchor = false;
    m->iter_added = pool->next_iter++;
    return pool->n_members++;
}

static inline uint32_t xs32(uint32_t *s) {
    uint32_t x = *s ? *s : 0xA5A5A5A5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

int grpo_pool_sample(const GrpoPool *pool, uint32_t *rng) {
    if (pool->n_members == 0) return -1;
    // Weight = (iter_added + 1) for non-anchors; anchor weighted similarly
    // so it stays a visible presence. The +1 keeps weights nonzero.
    uint64_t total = 0;
    uint32_t weights[GRPO_POOL_CAP];
    for (int i = 0; i < pool->n_members; i++) {
        weights[i] = (uint32_t)(pool->members[i].iter_added + 1);
        total += weights[i];
    }
    uint64_t r = (uint64_t)xs32(rng) % total;
    uint64_t acc = 0;
    for (int i = 0; i < pool->n_members; i++) {
        acc += weights[i];
        if (r < acc) return i;
    }
    return pool->n_members - 1;
}

int grpo_pool_member_choose(const PoolMember *m,
                            const Game *g, int bot_idx,
                            const LegalMoves *moves,
                            GrpoWorkspace *ws) {
    if (moves->n == 0) return -1;
    if (m->kind == POOL_HANDWRITTEN) {
        return handwritten_strategy_choose(g, bot_idx, moves, NULL);
    }
    // Dynamite — explicit per-thread workspace, never via dynamite_strategy's
    // global net (caller may have multiple nets active concurrently).
    if (!m->net) return -1;
    grpo_net_forward(m->net, ws, g, bot_idx, moves);
    int best = 0;
    for (int i = 1; i < moves->n; i++) {
        if (ws->logits[i] > ws->logits[best]) best = i;
    }
    return best;
}

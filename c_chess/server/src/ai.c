/* ai.c - see ai.h. */
#include "ai.h"
#include "engine.h"
#include "journal.h"

#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The levels: Stockfish's "Skill Level" (0..20, which makes it choose weaker moves on purpose) and
 * a node budget, so a level is as strong on a slow core as on a fast one (a time budget is not: on
 * macOS the engines run on the efficiency cores, five times slower); the time cap only bounds a
 * search that a starved engine would stretch. Level 8 is full strength at a million nodes. */
static const struct { int skill; uint64_t nodes; } LEVEL[CC_AI_LEVELS + 1] = {
    { 0, 0 }, { 0, 2000 }, { 3, 5000 }, { 6, 10000 }, { 9, 25000 }, { 12, 50000 }, { 15, 120000 }, { 18, 300000 }, { 20, 1000000 },
};
#define AI_CAP_MS 5000

/* THE EVAL CACHE. A position the computer has answered at a level is answered again from memory:
 * openings repeat across games, and a search is milliseconds to seconds of a core. Keyed by the
 * position's Zobrist key mixed with the level, one entry a slot, newest wins. 2^20 slots, 16 MB. */
#define CACHE_BITS 20
typedef struct { uint64_t key; uint16_t move; int16_t score; uint32_t pad; } cache_slot;
static cache_slot *CACHE;
static uint64_t cache_hits;

static uint64_t cache_key(uint64_t pos, int level) { return pos ^ (0x9e3779b97f4a7c15ULL * (uint64_t)(level + 1)); }

static cache_slot *cache_at(uint64_t k) {
    if (!CACHE && !(CACHE = calloc((size_t)1 << CACHE_BITS, sizeof *CACHE))) return NULL;
    return &CACHE[k >> (64 - CACHE_BITS)];
}

static uint64_t asked, refused;

static void ai_id(int level, char out[CB_UUID_LEN + 1]) {
    snprintf(out, CB_UUID_LEN + 1, "00000000-0000-4000-8000-0000000000%02d", level);
}

cc_player *cc_ai_player(int level) {
    if (level < 1 || level > CC_AI_LEVELS) return NULL;
    char id[CB_UUID_LEN + 1], name[CW_NAME + 1];
    ai_id(level, id);
    snprintf(name, sizeof name, "Stockfish-%d", level);
    return cc_player_of(id, name);
}

int cc_ai_level(const cc_player *p) {
    if (!p || strncmp(p->id, "00000000-0000-4000-8000-0000000000", 34) != 0) return 0;
    int level = atoi(p->id + 34);
    return level >= 1 && level <= CC_AI_LEVELS ? level : 0;
}

typedef struct { uint32_t game; uint16_t ply; uint8_t level; uint64_t key; } ask_ud;

static void answered(void *ud, const cc_engine_answer *a) {
    ask_ud q = *(ask_ud *)ud;
    free(ud);
    pthread_mutex_lock(&cc_lock);
    int64_t now = cc_mono();
    cc_live *l = cc_live_get(q.game);
    if (l) {
        l->ai_pending = 0;
        if (a->best != CH_MOVE_NONE) {
            cache_slot *c = cache_at(q.key);
            if (c) *c = (cache_slot){ q.key, a->best, (int16_t)(a->score_cp > 32000 ? 32000 : a->score_cp < -32000 ? -32000 : a->score_cp), 0 };
        }
        if (!cc_game_over(&l->g) && l->g.ply == q.ply && cc_ai_level(l->p[l->g.pos.stm]) && a->best != CH_MOVE_NONE) {
            bool was_over = cc_game_over(&l->g);
            int rc = cc_game_move(&l->g, l->g.pos.stm, q.ply, a->best, now);
            if (rc == CC_OK) { cc_journal_move(l); cc_live_changed(l, now); }
            else {
                CB_WARN("cc", "ai: game %u ply %u: the engine's move was refused (%d)", q.game, q.ply, rc);
                if (!was_over && cc_game_over(&l->g)) cc_live_changed(l, now);
            }
        }
    }
    pthread_mutex_unlock(&cc_lock);
}

void cc_ai_maybe_move(cc_live *l, int64_t now) {
    if (cc_game_over(&l->g) || l->ai_pending) return;
    int level = cc_ai_level(l->p[l->g.pos.stm]);
    if (!level || !cc_engine_available()) return;
    uint64_t key = cache_key(l->g.pos.key, level);
    cache_slot *c = cache_at(key);
    if (c && c->key == key && ch_is_legal(&l->g.pos, c->move)) {   /* answered before: no search */
        cache_hits++;
        if (cc_game_move(&l->g, l->g.pos.stm, l->g.ply, c->move, now) == CC_OK) { cc_journal_move(l); cc_live_changed(l, now); }
        return;
    }
    ask_ud *ud = malloc(sizeof *ud);
    if (!ud) return;
    ud->game = l->id;
    ud->ply = l->g.ply;
    ud->level = (uint8_t)level;
    ud->key = key;
    cc_engine_ask ask = { l->g.moves, l->g.ply, LEVEL[level].skill, AI_CAP_MS, 0, LEVEL[level].nodes };
    asked++;
    if (cc_engine_search(&ask, answered, ud) != CB_OK) { refused++; free(ud); return; }
    l->ai_pending = 1;
}

void cc_ai_counts(uint64_t *a, uint64_t *r, uint64_t *hits) { *a = asked; *r = refused; *hits = cache_hits; }

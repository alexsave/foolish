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
 * the time it may think. Level 8 is full strength at a second a move. */
static const struct { int skill, movetime_ms; } LEVEL[CC_AI_LEVELS + 1] = {
    { 0, 0 }, { 0, 50 }, { 3, 80 }, { 6, 100 }, { 9, 150 }, { 12, 200 }, { 15, 300 }, { 18, 500 }, { 20, 1000 },
};

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

typedef struct { uint32_t game; uint16_t ply; } ask_ud;

static void answered(void *ud, const cc_engine_answer *a) {
    ask_ud q = *(ask_ud *)ud;
    free(ud);
    pthread_mutex_lock(&cc_lock);
    int64_t now = cc_mono();
    cc_live *l = cc_live_get(q.game);
    if (l) {
        l->ai_pending = 0;
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
    (void)now;
    if (cc_game_over(&l->g) || l->ai_pending) return;
    int level = cc_ai_level(l->p[l->g.pos.stm]);
    if (!level || !cc_engine_available()) return;
    ask_ud *ud = malloc(sizeof *ud);
    if (!ud) return;
    ud->game = l->id;
    ud->ply = l->g.ply;
    cc_engine_ask ask = { l->g.moves, l->g.ply, LEVEL[level].skill, LEVEL[level].movetime_ms, 0, 0 };
    asked++;
    if (cc_engine_search(&ask, answered, ud) != CB_OK) { refused++; free(ud); return; }
    l->ai_pending = 1;
}

void cc_ai_counts(uint64_t *a, uint64_t *r) { *a = asked; *r = refused; }

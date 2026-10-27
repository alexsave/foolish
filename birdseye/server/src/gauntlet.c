// gauntlet.c - gauntlet.h.
#include "gauntlet.h"

#include "tournament.h"

#include <stdio.h>
#include <string.h>

_Static_assert(BE_GAUNT_GAMES == BE_TOUR_MAX_GAMES, "a rung's game cap is the tournament's");

const char *const BE_GAUNT_BRAINS[BE_GAUNT_N_BRAINS] = {
    "random", "firecracker", "robusta", "blackpowder", "octogen", "cordite", "handwritten",
};

// ---- the run -------------------------------------------------------------------------------------

void be_gaunt_init(be_w_gauntlet *g, uint8_t human) {
    memset(g, 0, sizeof *g);
    g->hash = be_wire_hash();
    g->state = BE_GAUNT_IDLE;
    g->human = human;
    for (int r = 0; r < BE_GAUNT_RUNGS; r++) g->rungs[r].bot = BE_TOUR_NONE;
}

// The best run: the highest rung reached (1-based) and the most rungs won, by this run's standing.
static void best(be_w_gauntlet *g) {
    uint8_t won = g->state == BE_GAUNT_WON ? BE_GAUNT_RUNGS : g->rung;
    uint8_t reached = (uint8_t)(g->rung < BE_GAUNT_RUNGS ? g->rung + 1 : BE_GAUNT_RUNGS);
    if (won > g->best_won || reached > g->best_rung) {
        if (won > g->best_won) g->best_won = won;
        if (reached > g->best_rung) g->best_rung = reached;
        g->best_run = g->run;
    }
}

bool be_gaunt_new_run(be_w_gauntlet *g, const int32_t *rating, const char *const *key, int n, uint8_t human, int64_t now_ms) {
    int pick[BE_GAUNT_N_BRAINS];
    for (int b = 0; b < BE_GAUNT_N_BRAINS; b++) {
        pick[b] = -1;
        for (int i = 0; i < n; i++)
            if (i != human && strcmp(key[i], BE_GAUNT_BRAINS[b]) == 0 && (pick[b] < 0 || rating[i] > rating[pick[b]])) pick[b] = i;
        if (pick[b] < 0) return false;
    }
    // Ascending by rating; a tie keeps the brains' own order (an insertion sort is stable).
    int order[BE_GAUNT_N_BRAINS];
    for (int b = 0; b < BE_GAUNT_N_BRAINS; b++) {
        int k = b;
        while (k > 0 && rating[pick[order[k - 1]]] > rating[pick[b]]) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = b;
    }
    uint8_t best_rung = g->best_rung, best_won = g->best_won;
    uint32_t best_run = g->best_run, run = g->run;
    be_gaunt_init(g, human);
    g->best_rung = best_rung;
    g->best_won = best_won;
    g->best_run = best_run;
    g->run = run + 1;
    g->state = BE_GAUNT_RUNNING;
    g->started_ms = now_ms;
    for (int r = 0; r < BE_GAUNT_RUNGS; r++) {
        int b = pick[order[r]];
        be_w_rung *x = &g->rungs[r];
        x->bot = (uint8_t)b;
        x->rating = rating[b];
    }
    best(g);   // rung 1 is reached the moment a run starts
    return true;
}

int be_gaunt_next(const be_w_gauntlet *g, uint8_t seats[2]) {
    if (g->state != BE_GAUNT_RUNNING || g->rung >= BE_GAUNT_RUNGS) return -1;
    const be_w_rung *x = &g->rungs[g->rung];
    if (x->live || x->bot == BE_TOUR_NONE || g->human == BE_TOUR_NONE) return -1;
    bool human_first = x->games % 2 == 0;
    seats[0] = human_first ? g->human : x->bot;
    seats[1] = human_first ? x->bot : g->human;
    return g->rung;
}

void be_gaunt_started(be_w_gauntlet *g, const char *gid, int gid_len, int64_t now_ms) {
    if (g->state != BE_GAUNT_RUNNING || g->rung >= BE_GAUNT_RUNGS) return;
    be_w_rung *x = &g->rungs[g->rung];
    x->live = 1;
    x->gid_len = (uint8_t)(gid_len < BE_GID ? gid_len : BE_GID);
    memcpy(x->gid, gid, x->gid_len);
    if (!x->started_ms) x->started_ms = now_ms;
}

bool be_gaunt_result(be_w_gauntlet *g, bool played, uint8_t fool_bot, int64_t now_ms) {
    if (g->state != BE_GAUNT_RUNNING || g->rung >= BE_GAUNT_RUNGS) return false;
    be_w_rung *x = &g->rungs[g->rung];
    x->live = 0;
    if (!played) return true;
    uint8_t res = fool_bot == x->bot ? BE_GAUNT_G_HUMAN : fool_bot == g->human ? BE_GAUNT_G_BOT : BE_GAUNT_G_DRAW;
    if (x->games < BE_GAUNT_GAMES) x->result[x->games] = res;
    x->games++;
    g->games++;
    if (res == BE_GAUNT_G_HUMAN) x->wins_h++;
    else if (res == BE_GAUNT_G_BOT) x->wins_b++;
    int won = x->wins_h >= BE_TOUR_WIN ? 1 : x->wins_b >= BE_TOUR_WIN ? 0 : -1;
    if (won < 0 && x->games >= BE_GAUNT_GAMES) won = x->wins_h > x->wins_b;   // drawn this often: the one ahead, else the bot
    if (won < 0) return true;
    x->done_ms = now_ms;
    if (won) {
        g->rung++;
        if (g->rung >= BE_GAUNT_RUNGS) {
            g->state = BE_GAUNT_WON;
            g->finished_ms = now_ms;
        }
    } else {
        g->state = BE_GAUNT_LOST;
        g->finished_ms = now_ms;
    }
    best(g);
    return true;
}

int be_gaunt_game_title(const be_w_gauntlet *g, const char *name, int name_len, char *out, int cap) {
    int r = g->rung < BE_GAUNT_RUNGS ? g->rung : BE_GAUNT_RUNGS - 1;
    const int best_of = 2 * BE_TOUR_WIN - 1, game = g->rungs[r].games + 1;
    char tail[24];
    if (game <= best_of) snprintf(tail, sizeof tail, " - Game %d of %d", game, best_of);
    else snprintf(tail, sizeof tail, " - Game %d", game);
    int n = snprintf(out, (size_t)cap, "Gauntlet %d/%d: %.*s%s", r + 1, BE_GAUNT_RUNGS, name_len, name, tail);
    // The lobby's title rule refuses a longer one (roster.c roster_title_keep): the name goes.
    if (n > BE_GAUNT_TITLE_MAX) n = snprintf(out, (size_t)cap, "Gauntlet %d/%d%s", r + 1, BE_GAUNT_RUNGS, tail);
    return n < 0 ? 0 : n >= cap ? cap - 1 : n;
}

// ---- the file ------------------------------------------------------------------------------------
//   "BEGA", u32 version, u32 sizeof(be_w_gauntlet), u32 n ids, n x BE_BOT_ID bytes, be_w_gauntlet.

#define GAUNT_MAGIC 0x41474542u   // "BEGA"

bool be_gaunt_save(const be_w_gauntlet *g, const char *path, const char *const *ids, int n) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint32_t head[4] = { GAUNT_MAGIC, 1, (uint32_t)sizeof *g, (uint32_t)n };
    bool ok = fwrite(head, sizeof head, 1, f) == 1;
    for (int i = 0; ok && i < n; i++) {
        char id[BE_BOT_ID];
        memset(id, 0, sizeof id);
        strncpy(id, ids[i], sizeof id);
        ok = fwrite(id, sizeof id, 1, f) == 1;
    }
    ok = ok && fwrite(g, sizeof *g, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

static uint8_t remap(uint8_t v, const int *map, uint32_t n) { return v == BE_TOUR_NONE || v >= n ? BE_TOUR_NONE : (uint8_t)map[v]; }

bool be_gaunt_load(be_w_gauntlet *g, const char *path, const char *const *ids, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t head[4];
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == GAUNT_MAGIC && head[1] == 1 && head[2] == sizeof *g && head[3] <= 255;
    int map[256];
    for (uint32_t i = 0; ok && i < head[3]; i++) {
        char id[BE_BOT_ID + 1];
        ok = fread(id, BE_BOT_ID, 1, f) == 1;
        id[BE_BOT_ID] = 0;
        map[i] = BE_TOUR_NONE;
        for (int k = 0; ok && k < n; k++)
            if (strcmp(ids[k], id) == 0) map[i] = k;
        if (ok && map[i] == BE_TOUR_NONE) ok = false;   // a row today's list lacks
    }
    be_w_gauntlet x;
    ok = ok && fread(&x, sizeof x, 1, f) == 1;
    fclose(f);
    if (!ok) return false;
    x.hash = be_wire_hash();
    x.human = remap(x.human, map, head[3]);
    for (int r = 0; r < BE_GAUNT_RUNGS; r++) x.rungs[r].bot = remap(x.rungs[r].bot, map, head[3]);
    *g = x;
    return true;
}

// tournament.c - tournament.h.
#include "tournament.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int ROUND_SIZE[BE_TOUR_ROUNDS] = { 16, 16, 8, 4, 2, 1 };
static const int ROUND_AT[BE_TOUR_ROUNDS] = { 0, 16, 32, 40, 44, 46 };

int be_tour_feeds(int m, int *side) {
    if (m < 0 || m >= BE_TOUR_MATCHES - 1) return -1;
    int r = 0;
    while (r + 1 < BE_TOUR_ROUNDS && m >= ROUND_AT[r + 1]) r++;
    int slot = m - ROUND_AT[r];
    if (r == 0) {   // an opening match feeds the lower seed's side of the same round-of-32 slot
        *side = 1;
        return ROUND_AT[1] + slot;
    }
    *side = slot % 2;
    return ROUND_AT[r + 1] + slot / 2;
}

// The standard order of seeds 1..n (n a power of two): neighbours meet first.
static void seed_order(int n, int *out) {
    int len = 1;
    out[0] = 1;
    while (len < n) {
        int tmp[64];
        for (int i = 0; i < len; i++) {
            tmp[2 * i] = out[i];
            tmp[2 * i + 1] = 2 * len + 1 - out[i];
        }
        len *= 2;
        memcpy(out, tmp, (size_t)len * sizeof *out);
    }
}

bool be_tour_draw(be_w_tour *t, const int32_t *rating, const bool *is_random, int n, int64_t now_ms) {
    if (n < BE_TOUR_PLAYERS + 1 || n > 255) return false;
    memset(t, 0, sizeof *t);
    t->hash = be_wire_hash();
    t->state = BE_TOUR_RUNNING;
    t->champion = BE_TOUR_NONE;
    t->n_matches = BE_TOUR_MATCHES;
    t->started_ms = now_ms;
    // The one left out: the lowest-rated random bot, else the lowest-rated of all.
    int drop = -1;
    for (int pass = 0; pass < 2 && drop < 0; pass++)
        for (int i = 0; i < n; i++)
            if ((pass == 1 || is_random[i]) && (drop < 0 || rating[i] < rating[drop])) drop = i;
    t->dropped = (uint8_t)drop;
    // Seeds by rating, highest first (the lower index on a tie); the 48 best of the rest.
    int order[256], k = 0;
    for (int i = 0; i < n; i++)
        if (i != drop) order[k++] = i;
    for (int i = 0; i < k; i++)
        for (int j = i + 1; j < k; j++)
            if (rating[order[j]] > rating[order[i]] || (rating[order[j]] == rating[order[i]] && order[j] < order[i])) {
                int x = order[i];
                order[i] = order[j];
                order[j] = x;
            }
    for (int s = 0; s < BE_TOUR_PLAYERS; s++) {
        t->seed_bot[s] = (uint8_t)order[s];
        t->seed_rating[s] = rating[order[s]];
    }
    for (int r = 0; r < BE_TOUR_ROUNDS; r++)
        for (int j = 0; j < ROUND_SIZE[r]; j++) {
            be_w_match *m = &t->matches[ROUND_AT[r] + j];
            m->round = (uint8_t)r;
            m->slot = (uint8_t)j;
            m->a = m->b = m->winner = BE_TOUR_NONE;
        }
    // The round of 32 in the standard order of seeds 1..32; its lower seed (17..32) is the winner of
    // the opening match s v 65 - s.
    int o[32];
    seed_order(32, o);
    for (int j = 0; j < 16; j++) {
        int hi = o[2 * j] < o[2 * j + 1] ? o[2 * j] : o[2 * j + 1];
        int lo = o[2 * j] + o[2 * j + 1] - hi;
        be_w_match *m = &t->matches[ROUND_AT[1] + j];
        m->seed_a = (uint8_t)hi;
        m->a = t->seed_bot[hi - 1];
        be_w_match *op = &t->matches[j];
        op->seed_a = (uint8_t)lo;
        op->a = t->seed_bot[lo - 1];
        op->seed_b = (uint8_t)(65 - lo);
        op->b = t->seed_bot[65 - lo - 1];
    }
    return true;
}

bool be_tour_swap_seed(be_w_tour *t, int seed, uint8_t who) {
    if (t->state != BE_TOUR_RUNNING || t->games != 0 || seed < 1 || seed > BE_TOUR_PLAYERS || who == BE_TOUR_NONE) return false;
    for (int s = 0; s < BE_TOUR_PLAYERS; s++)
        if (t->seed_bot[s] == who) return false;
    for (int i = 0; i < BE_TOUR_MATCHES; i++)
        if (t->matches[i].live) return false;
    uint8_t old = t->seed_bot[seed - 1];
    t->seed_bot[seed - 1] = who;
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        be_w_match *m = &t->matches[i];
        if (m->a == old) m->a = who;
        if (m->b == old) m->b = who;
    }
    return true;
}

int be_tour_next(const be_w_tour *t, uint8_t seats[2]) {
    if (t->state != BE_TOUR_RUNNING) return -1;
    // The barrier: only the lowest round that still has an undecided match plays.
    int round = BE_TOUR_ROUNDS;
    for (int i = 0; i < BE_TOUR_MATCHES && round == BE_TOUR_ROUNDS; i++)
        if (t->matches[i].winner == BE_TOUR_NONE) round = t->matches[i].round;
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        const be_w_match *m = &t->matches[i];
        if (m->round != round) continue;
        if (m->a == BE_TOUR_NONE || m->b == BE_TOUR_NONE || m->winner != BE_TOUR_NONE || m->live) continue;
        bool hi_a = m->seed_a < m->seed_b;
        uint8_t hi = hi_a ? m->a : m->b, lo = hi_a ? m->b : m->a;
        bool hi_first = m->games % 2 == 0;
        seats[0] = hi_first ? hi : lo;
        seats[1] = hi_first ? lo : hi;
        return i;
    }
    return -1;
}

void be_tour_started(be_w_tour *t, int m, const char *gid, int gid_len) {
    if (m < 0 || m >= BE_TOUR_MATCHES) return;
    be_w_match *x = &t->matches[m];
    x->live = 1;
    x->gid_len = (uint8_t)(gid_len < BE_GID ? gid_len : BE_GID);
    memcpy(x->gid, gid, x->gid_len);
}

static void decide(be_w_tour *t, int m, uint8_t winner, int64_t now_ms) {
    be_w_match *x = &t->matches[m];
    x->winner = winner;
    x->done_ms = now_ms;
    int side = 0;
    int next = be_tour_feeds(m, &side);
    if (next < 0) {
        t->champion = winner;
        t->state = BE_TOUR_DONE;
        t->finished_ms = now_ms;
        return;
    }
    be_w_match *n = &t->matches[next];
    uint8_t seed = winner == x->a ? x->seed_a : x->seed_b;
    if (side == 0) {
        n->a = winner;
        n->seed_a = seed;
    } else {
        n->b = winner;
        n->seed_b = seed;
    }
}

bool be_tour_result(be_w_tour *t, int m, bool played, uint8_t fool_bot, int64_t now_ms) {
    if (m < 0 || m >= BE_TOUR_MATCHES) return false;
    be_w_match *x = &t->matches[m];
    if (x->winner != BE_TOUR_NONE) return false;
    x->live = 0;
    if (!played) return true;
    x->games++;
    t->games++;
    if (fool_bot == x->a) x->wins_b++;
    else if (fool_bot == x->b) x->wins_a++;
    if (x->wins_a >= BE_TOUR_WIN) decide(t, m, x->a, now_ms);
    else if (x->wins_b >= BE_TOUR_WIN) decide(t, m, x->b, now_ms);
    else if (x->games >= BE_TOUR_MAX_GAMES) {
        // Drawn this often: the one ahead, else the higher seed.
        uint8_t w = x->wins_a != x->wins_b ? (x->wins_a > x->wins_b ? x->a : x->b) : (x->seed_a < x->seed_b ? x->a : x->b);
        decide(t, m, w, now_ms);
    }
    return true;
}

// ---- the file ------------------------------------------------------------------------------------
//   "BETR", u32 version, u32 sizeof(be_w_tour), u32 n ids, n x BE_BOT_ID bytes, be_w_tour.

#define TOUR_MAGIC 0x52544542u   // "BETR"

bool be_tour_save(const be_w_tour *t, const char *path, const char *const *ids, int n) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint32_t head[4] = { TOUR_MAGIC, 1, (uint32_t)sizeof *t, (uint32_t)n };
    bool ok = fwrite(head, sizeof head, 1, f) == 1;
    for (int i = 0; ok && i < n; i++) {
        char id[BE_BOT_ID];
        memset(id, 0, sizeof id);
        strncpy(id, ids[i], sizeof id);
        ok = fwrite(id, sizeof id, 1, f) == 1;
    }
    ok = ok && fwrite(t, sizeof *t, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

static uint8_t remap(uint8_t v, const int *map, uint32_t n) { return v == BE_TOUR_NONE || v >= n ? BE_TOUR_NONE : (uint8_t)map[v]; }

bool be_tour_load(be_w_tour *t, const char *path, const char *const *ids, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t head[4];
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == TOUR_MAGIC && head[1] == 1 && head[2] == sizeof *t && head[3] <= 255;
    int map[256];
    for (uint32_t i = 0; ok && i < head[3]; i++) {
        char id[BE_BOT_ID + 1];
        ok = fread(id, BE_BOT_ID, 1, f) == 1;
        id[BE_BOT_ID] = 0;
        map[i] = BE_TOUR_NONE;
        for (int k = 0; ok && k < n; k++)
            if (strcmp(ids[k], id) == 0) map[i] = k;
        if (ok && map[i] == BE_TOUR_NONE) ok = false;   // a bot the server no longer has
    }
    ok = ok && fread(t, sizeof *t, 1, f) == 1;
    fclose(f);
    if (!ok) return false;
    t->hash = be_wire_hash();
    t->dropped = remap(t->dropped, map, head[3]);
    t->champion = remap(t->champion, map, head[3]);
    for (int s = 0; s < BE_TOUR_PLAYERS; s++) t->seed_bot[s] = remap(t->seed_bot[s], map, head[3]);
    for (int i = 0; i < BE_TOUR_MATCHES; i++) {
        be_w_match *m = &t->matches[i];
        m->a = remap(m->a, map, head[3]);
        m->b = remap(m->b, map, head[3]);
        m->winner = remap(m->winner, map, head[3]);
    }
    return true;
}

static const char *const ROUND_NAME[BE_TOUR_ROUNDS] = {
    "Opening round", "Round of 32", "Round of 16", "Quarter-final", "Semi-final", "Final",
};

const char *be_tour_round_name(int r) { return r >= 0 && r < BE_TOUR_ROUNDS ? ROUND_NAME[r] : ""; }

int be_tour_game_title(const be_w_match *m, char *out, int cap) {
    const int best_of = 2 * BE_TOUR_WIN - 1, game = m->games + 1;
    int n = game <= best_of ? snprintf(out, (size_t)cap, "%s - Game %d of %d", be_tour_round_name(m->round), game, best_of)
                            : snprintf(out, (size_t)cap, "%s - Game %d", be_tour_round_name(m->round), game);
    return n < 0 ? 0 : n >= cap ? cap - 1 : n;
}

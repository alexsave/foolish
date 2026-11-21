// gd_bot_seoul - see gd_bot_seoul.h.
#include "gd_bot_seoul.h"
#include "gd_bot_hw.h"
#include "gd_sim.h"
#include "gd_rng.h"

// Section timing, in the profiling build only (Makefile durak_prof.gb):
// [0] candidates, [1] sampling a world, [2] applying the candidate and making
// the rollout state, [3] the playout, summed in gb_prof_sect (tests/gb_prof.c).
#ifdef SEOUL_PROFILE
void gb_prof_mark(uint8_t i);
#define SEOUL_MARK(i) gb_prof_mark(i)
#else
#define SEOUL_MARK(i) ((void)0)
#endif

uint8_t gd_seoul_cands;
uint16_t gd_seoul_playouts;
uint32_t gd_seoul_plies;          // plies the last decision's playouts played, all told

static gd_move cand[SEOUL_MAX_CANDS];
static uint8_t nc;
static uint8_t wins[SEOUL_MAX_CANDS];
static gd_game world;
static gd_sim sim;
static uint16_t world_seed[SEOUL_SAMPLES_MAX];
static uint16_t after_seeds;

// The same move: kind and card set (a cover's pairing aside, which the rules
// make for it).
static uint8_t same_move(const gd_move *a, const gd_move *b) {
    uint8_t i, j, f;
    if (a->type != b->type || a->n != b->n) return 0;
    for (i = 0; i < a->n; i++) {
        f = 0;
        for (j = 0; j < b->n; j++) if (a->cards[i] == b->cards[j]) { f = 1; break; }
        if (!f) return 0;
    }
    return 1;
}

static void add(const gd_game *g, uint8_t seat, const gd_move *m) {
    uint8_t i;
    if (nc >= SEOUL_MAX_CANDS || m->type == 0xFF || !gd_legal(g, seat, m)) return;
    for (i = 0; i < nc; i++) if (same_move(&cand[i], m)) return;
    cand[nc++] = *m;
}

// The candidates: New York's move first (the tie-break), then the plain
// alternatives a strong player weighs: done, pick up, a transfer with each card
// that can, and a one-card attack or throw-in with each card, cheapest first.
static void candidates(const gd_game *g, uint8_t seat) {
    const uint8_t kind = gd_menu_kind(g, seat);
    gd_move m;
    uint8_t c, r, su, trumps;
    nc = 0;
    gd_hw_choose(g, seat, &m);
    add(g, seat, &m);
    m.n = 0;
    if (kind == GD_KIND_ATTACKER) { m.type = GD_GOOD; add(g, seat, &m); }
    if (kind == GD_KIND_DEFENDER) {
        m.type = GD_PICKUP; add(g, seat, &m);
        m.type = GD_PASS; m.n = 1;
        for (c = 0; c < GD_NCARDS; c++) if (GD_HAS(g->hand[seat], c)) { m.cards[0] = c; add(g, seat, &m); }
    }
    if (kind == GD_KIND_FIRST || kind == GD_KIND_ATTACKER) {
        m.type = GD_ATTACK; m.n = 1;
        for (trumps = 0; trumps < 2; trumps++)
            for (r = 0; r < GD_NRANKS; r++)
                for (su = 0; su < GD_NSUITS; su++) {
                    if ((su == g->trump) != trumps) continue;
                    c = (uint8_t)(su * GD_NRANKS + r);
                    if (GD_HAS(g->hand[seat], c)) { m.cards[0] = c; add(g, seat, &m); }
                }
    }
}

// A world consistent with what `me` sees: the cards it cannot place, dealt at
// random to the opponent's unknown cards and the stock.
static void sample_world(const gd_game *g, uint8_t me) {
    const uint8_t opp = (uint8_t)(me ^ 1u);
    uint8_t pool[GD_NCARDS], n = 0, c, i, j, t, need;
    gd_mask seen = g->hand[me];
    for (i = 0; i < 5; i++) seen.b[i] |= g->gone.b[i] | g->known[opp].b[i];
    for (i = 0; i < g->nb; i++) { GD_SET(seen, g->att[i]); if (g->def[i] != GD_NONE) GD_SET(seen, g->def[i]); }
    if (g->has_flipped) GD_SET(seen, g->flipped);
    for (c = 0; c < GD_NCARDS; c++) if (!GD_HAS(seen, c)) pool[n++] = c;
    for (i = n; i > 1; i--) {
        j = gd_rng_small(i);
        t = pool[i - 1]; pool[i - 1] = pool[j]; pool[j] = t;
    }
    world = *g;
    need = (uint8_t)(gd_hand_n(g, opp) - gd_count(&g->known[opp]));
    world.hand[opp] = g->known[opp];
    for (i = 0; i < need && i < n; i++) GD_SET(world.hand[opp], pool[i]);
    for (j = 0; j < g->stock_n && i < n; j++, i++) world.stock[j] = pool[i];
}

void gd_seoul_choose(const gd_game *g, uint8_t seat, gd_move *out) GD_BANKED {
    uint8_t k, s, best, samples;
    out->type = 0xFF;
    out->n = 0;
    gd_seoul_playouts = 0;
    gd_seoul_plies = 0;
    SEOUL_MARK(0);
    candidates(g, seat);
    SEOUL_MARK(1);
    gd_seoul_cands = nc;
    if (nc == 0) return;
    if (nc == 1) { *out = cand[0]; return; }
    for (k = 0; k < nc; k++) wins[k] = 0;
    // The budget of playouts shared by the candidates, two to eight worlds each.
    samples = (uint8_t)(SEOUL_BUDGET / nc);
    if (samples < 2) samples = 2;
    if (samples > SEOUL_SAMPLES_MAX) samples = SEOUL_SAMPLES_MAX;
    // The same worlds, and the same draws in their playouts, for every
    // candidate (common random numbers): the candidates differ only by the move.
    // Each world's seed is a fresh draw (a whole new place in the generator's
    // cycle), so worlds do not share stretches of one stream.
    for (s = 0; s < samples; s++) {
        world_seed[s] = gd_rng_u16();
        if (!world_seed[s]) world_seed[s] = 1;
    }
    after_seeds = gd_rng_get();
    for (k = 0; k < nc; k++)
        for (s = 0; s < samples; s++) {
            gd_rng_set(world_seed[s]);
            SEOUL_MARK(0);
            sample_world(g, seat);
            SEOUL_MARK(2);
            if (!gd_apply(&world, 0, seat, &cand[k])) continue;
            if (world.status != GD_PLAYING) {
                if (gd_result(&world) == (seat ? GD_RESULT_FOOL0 : GD_RESULT_FOOL1)) wins[k]++;
                continue;
            }
            gd_sim_from_game(&sim, &world);
            SEOUL_MARK(3);
            if (gd_sim_playout(&sim, seat, SEOUL_MAX_TURNS) == 1) wins[k]++;
            SEOUL_MARK(4);
            gd_seoul_playouts++;
            gd_seoul_plies += gd_sim_plies;
        }
    best = 0;
    for (k = 1; k < nc; k++) if (wins[k] > wins[best]) best = k;
    *out = cand[best];
    // the game's stream goes on as if Seoul had drawn only the world seeds
    gd_rng_set(after_seeds);
}

// BotMemory's proofs (src/bot_memory.h).
//
//  1. THE EQUIVALENCE ORACLE. Seeded games, 2 to 8 seats, the classic game and
//     podkidnoy, with rematches: before every decision, for EVERY seat as the
//     reader, each belief brain's belief read off a memory folded one MASKED
//     record at a time (bot_mem_fold_masked, what the server's table folds) and
//     bound to the board is held to the log scan it replaced, run over the
//     board's own whole log (tests/bot_memory_oracle.inc, the old code
//     verbatim). Blackpowder, cordite and octogen must agree bit for bit, rows
//     in order; robusta's rows as sets (it now shares the replayed table's rows,
//     whose order is play order, where it kept the pickup record's) and its pool
//     exactly. And the seeds' progress term (bot_mem_records) is the log length.
//  2. CANONICAL BYTES. The incrementally folded block encodes to the same bytes
//     as one folded over the whole masked log at once, and decodes back to it.
//  3. MUTANTS (MUTATIONS.md): every proof names the code it covers; break that
//     code and the named check goes red.
#include "../src/bot_common.h"
#include "../src/bot_memory.h"
#include "../src/bot_play.h"
#include "../src/strategy.h"
#include "../src/game.h"
#include "../src/legal.h"
#include "../src/octogen_strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Card pool[80];
    int  n;
    Card pinned[MAX_PLAYERS][MAX_HAND_SIZE];
    int  pinned_n[MAX_PLAYERS];
} OldPool;
static int old_og_profile = 0;
// The belief probe's two stages (octogen_strategy.h og_belief_probe), as the old
// scan and the product each report them.
typedef struct { BotBelief b; bool tell[MAX_PLAYERS]; int seen; } ProbeAt;
static ProbeAt old_at[2], new_at[2];
static void probe_capture(ProbeAt *at, int stage, const BotBelief *b, const bool *tell) {
    if (stage < 0 || stage > 1) return;
    at[stage].b = *b;
    memcpy(at[stage].tell, tell, sizeof at[stage].tell);
    at[stage].seen = 1;
}
#define OLD_OG_PROBE(stage) probe_capture(old_at, (stage), &B->b, B->mc_tell)
static void new_probe(int stage, const Game *g, int bot_idx, const BotBelief *b, const bool mc_tell[MAX_PLAYERS]) {
    (void)g; (void)bot_idx;
    probe_capture(new_at, stage, b, mc_tell);
}

#include "bot_memory_oracle.inc"

static int fails;
static long checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 20) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } } while (0)

static bool rows_equal(const Card *a, int an, const Card *b, int bn) {
    return an == bn && memcmp(a, b, (size_t)an * sizeof(Card)) == 0;
}
static bool rows_same_set(const Card *a, int an, const Card *b, int bn) {
    if (an != bn) return false;
    for (int i = 0; i < an; i++) if (!bot_set_contains(b, bn, a[i])) return false;
    return true;
}

static bool belief_equal(const BotBelief *x, const BotBelief *y, int np, const char **what) {
    if (x->n != y->n || memcmp(x->pool, y->pool, (size_t)x->n * sizeof(Card))) { *what = "pool"; return false; }
    for (int p = 0; p < np; p++) {
        if (!rows_equal(x->pinned[p], x->pinned_n[p], y->pinned[p], y->pinned_n[p])) { *what = "pinned"; return false; }
        if (!rows_equal(x->voids[p], x->void_n[p], y->voids[p], y->void_n[p])) { *what = "voids"; return false; }
        if (x->floor_v[p] != y->floor_v[p]) { *what = "floor"; return false; }
        if (x->distrust_floor[p] != y->distrust_floor[p]) { *what = "distrust_floor"; return false; }
        if (x->distrust_void[p] != y->distrust_void[p]) { *what = "distrust_void"; return false; }
    }
    return true;
}

// What the run saw, so a run that never exercised a rule cannot pass.
static long n_probed, n_capped, n_decisions, n_flip, n_voids, n_floors, n_tells, n_decl, n_broken, n_viol_pend, n_pinned;

static void compare_all(const Game *g, const BotMemory *mem, int gi) {
    bot_mem_bind(g, mem);
    CHECK(bot_mem_records(g) == (uint32_t)g->num_logs, "game %d: records %u != num_logs %d", gi, bot_mem_records(g), g->num_logs);
    const int np = g->num_players;
    if (bot_mem_flip_seat(mem, g) >= 0) n_flip++;
    for (int s = 0; s < np; s++) {
        if (g->players[s].status != PLAYER_STATUS_IN) continue;
        const char *what = "";
        BotBelief a, b;
        old_bp_belief(g, s, &a);
        bp_belief_of(g, s, &b);
        CHECK(belief_equal(&a, &b, np, &what), "game %d seat %d log %d: blackpowder %s differs", gi, s, g->num_logs, what);
        old_cd_belief(g, s, &a);
        cd_belief_of(g, s, &b);
        CHECK(belief_equal(&a, &b, np, &what), "game %d seat %d log %d: cordite %s differs", gi, s, g->num_logs, what);
        for (int p = 0; p < np; p++) {
            n_voids += a.void_n[p] > 0;
            n_floors += a.floor_v[p] > 0;
            n_pinned += a.pinned_n[p] > 0;
        }
        for (int prof = 0; prof <= 1; prof++) {
            OgBeliefProbe x, y;
            old_og_profile = prof;
            old_og_belief(g, s, &x);
            if (prof) {
                // The product's profile switch is OG_PROFILE (read at reload); the
                // probe runs at the C default (0), so the profiled case is checked
                // on its inputs: the counts the profile reads.
                og_belief_of(g, s, &y);
                for (int p = 0; p < np; p++) {
                    CHECK(x.cards_played[p] == y.cards_played[p] && x.trumps_played[p] == y.trumps_played[p],
                          "game %d seat %d log %d: octogen deck-alive counts differ for %d", gi, s, g->num_logs, p);
                }
                continue;
            }
            memset(old_at, 0, sizeof old_at);
            memset(new_at, 0, sizeof new_at);
            old_og_belief(g, s, &x);
            og_belief_probe = new_probe;
            og_belief_of(g, s, &y);
            og_belief_probe = 0;
            CHECK(belief_equal(&x.b, &y.b, np, &what), "game %d seat %d log %d: octogen %s differs", gi, s, g->num_logs, what);
            for (int st = 0; st < 2; st++) {
                const char *w2 = "";
                CHECK(old_at[st].seen && new_at[st].seen && belief_equal(&old_at[st].b, &new_at[st].b, np, &w2)
                      && memcmp(old_at[st].tell, new_at[st].tell, sizeof old_at[st].tell) == 0,
                      "game %d seat %d log %d: octogen's belief probe, stage %d, reads a different belief (%s)", gi, s, g->num_logs, st, w2);
                n_probed += st;
            }
            for (int p = 0; p < np; p++) {
                CHECK(x.trump_leads[p] == y.trump_leads[p], "game %d seat %d: octogen trump leads differ for %d", gi, s, p);
                CHECK(x.mc_tell[p] == y.mc_tell[p], "game %d seat %d log %d: octogen mc_tell differs for %d", gi, s, g->num_logs, p);
                CHECK(x.cards_played[p] == y.cards_played[p] && x.trumps_played[p] == y.trumps_played[p],
                      "game %d seat %d: octogen counts differ for %d", gi, s, p);
                n_tells += x.mc_tell[p];
                n_viol_pend += x.trump_leads[p] > 0;
            }
        }
        // A PICKUP or DISCARD record lists at most MAX_LOG_PAIRS cards (game.c
        // log_add_card drops the rest), and robusta's scan read the lists: after
        // a wider one it lost cards the memory, replaying the moves, keeps. There
        // its rows hold the scan's and more, and its pool the scan's less those.
        bool capped = false;
        for (int i = 0; i < g->num_logs; i++) capped |= (g->logs[i].log_type == LOG_PICKUP || g->logs[i].log_type == LOG_DISCARD) && g->logs[i].num_pairs >= MAX_LOG_PAIRS;
        OldPool u;
        old_rb_pool(g, s, &u);
        robusta_belief_of(g, s, &b);
        if (!capped) {
            CHECK(u.n == b.n && memcmp(u.pool, b.pool, (size_t)u.n * sizeof(Card)) == 0,
                  "game %d seat %d log %d: robusta pool differs", gi, s, g->num_logs);
            for (int p = 0; p < np; p++)
                CHECK(rows_same_set(u.pinned[p], u.pinned_n[p], b.pinned[p], b.pinned_n[p]),
                      "game %d seat %d log %d: robusta pinned set differs for %d", gi, s, g->num_logs, p);
        } else {
            n_capped++;
            for (int p = 0; p < np; p++)
                for (int k = 0; k < u.pinned_n[p]; k++)
                    CHECK(bot_set_contains(b.pinned[p], b.pinned_n[p], u.pinned[p][k]),
                          "game %d seat %d: robusta lost a pinned card past a capped pickup", gi, s);
            for (int k = 0; k < b.n; k++)
                CHECK(bot_set_contains(u.pool, u.n, b.pool[k]), "game %d seat %d: robusta's pool grew past a capped pickup", gi, s);
        }
    }
    for (int p = 0; p < np; p++) {
        n_decl += mem->decl[p] != 0;
        n_broken += (mem->flags[p] & BOT_MEM_FLOOR_BROKEN) != 0;
    }
    bot_mem_bind(0, 0);
}

// The block a whole stored log folds to, from scratch (a cold backfill).
static void fold_stored(BotMemory *out, const GameLog *stored, int n, const Game *g) {
    bot_mem_clear(out);
    for (int i = 0; i < n; i++) bot_mem_fold(out, &stored[i], g);
}

static GameLog stored[4 * MAX_LOGS];

static void canonical(const Game *g, const BotMemory *mem, int n_stored, int gi) {
    BotMemory cold, back;
    fold_stored(&cold, stored, n_stored, g);
    uint8_t a[BOT_MEM_BYTES_MAX], b[BOT_MEM_BYTES_MAX];
    const int na = bot_mem_encode(mem, a, (int)sizeof a), nb = bot_mem_encode(&cold, b, (int)sizeof b);
    CHECK(na > 0 && na == nb && memcmp(a, b, (size_t)na) == 0, "game %d: the live block and the cold backfill encode apart", gi);
    CHECK(bot_mem_decode(&back, a, na) && bot_mem_equal(&back, mem), "game %d: the block does not decode back to itself", gi);
}

// The record as the store holds it, for the cold backfill.
static void store_masked(const GameLog *rec, const Game *g, bool pre_has_flip, Card pre_flip, int at) {
    bot_mem_mask(rec, g, pre_has_flip, pre_flip, &stored[at]);
}

int main(int argc, char **argv) {
    const int games = argc > 1 ? atoi(argv[1]) : 400;
    static const int brains[] = { STRAT_RANDOM, STRAT_SIMPLE_HEURISTIC, STRAT_HANDWRITTEN_PROD, STRAT_HANDWRITTEN, STRAT_ESPRESSO };
    static Game g;
    BotMemory mem;
    long rematches = 0;
    for (int gi = 0; gi < games; gi++) {
        const int np = 2 + gi % 7;
        game_set_seed(0xB07E0000u + (uint32_t)gi);
        random_strategy_set_seed(0x5EED0000u + (uint64_t)gi);
        memset(&g, 0, sizeof g);
        g.num_players = (int8_t)np;
        if (gi % 3 == 1) g.rules = GAME_RULE_NO_PASS;
        bot_mem_clear(&mem);
        int n_stored = 0;
        // Two games at one table: the second is a rematch, whose GAME_START must
        // reset everything the first left behind.
        for (int round = 0; round < (gi % 4 == 0 ? 2 : 1); round++) {
            if (round) {
                rematches++;
                const int8_t rules = g.rules;
                memset(&g, 0, sizeof g);
                g.num_players = (int8_t)np;
                g.rules = rules;
            }
            for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
            bool pre_has_flip = false;
            Card pre_flip = CARD_NONE;
            start_game(&g);
            n_stored = 0;
            for (int i = 0; i < g.num_logs; i++) {
                store_masked(&g.logs[i], &g, pre_has_flip, pre_flip, n_stored++);
                bot_mem_fold_masked(&mem, &g.logs[i], &g, pre_has_flip, pre_flip);
            }
            BotPlay bp;
            bot_play_init(&bp, &g, 4000, BOT_PLAY_SHUFFLE, NULL);
            for (int seat; (seat = bot_play_next(&bp)) >= 0; ) {
                if (g.num_logs < MAX_LOGS) { compare_all(&g, &mem, gi); n_decisions++; }
                const int idx = strategy_choose(brains[(seat * 3 + gi) % 5], &g, seat, &bp.moves);
                const int before = g.num_logs;
                pre_has_flip = g.has_flipped;
                pre_flip = g.flipped;
                bot_play_apply(&bp, idx);
                for (int i = before; i < g.num_logs; i++) {
                    if (n_stored < (int)(sizeof stored / sizeof stored[0])) store_masked(&g.logs[i], &g, pre_has_flip, pre_flip, n_stored++);
                    bot_mem_fold_masked(&mem, &g.logs[i], &g, pre_has_flip, pre_flip);
                }
            }
            canonical(&g, &mem, n_stored, gi);
        }
    }
    printf("bot_memory_test: %d games (%ld rematches), %ld decisions x every seat; seen: flip pins %ld, voids %ld, floors %ld, "
           "floors broken %ld, declined values %ld, tells %ld, trump leads %ld, pinned rows %ld, robusta past a capped pickup %ld, octogen probe stages compared %ld; %ld checks\n",
           games, rematches, n_decisions, n_flip, n_voids, n_floors, n_broken, n_decl, n_tells, n_viol_pend, n_pinned, n_capped, 2 * n_probed, checks);
    CHECK(n_flip > 0 && n_voids > 0 && n_floors > 0 && n_broken > 0 && n_decl > 0 && n_tells > 0 && n_viol_pend > 0 && n_pinned > 0 && n_probed > 0,
          "the run exercised every rule the memory folds");
    if (fails) { printf("FAIL: %d of %ld checks\n", fails, checks); return 1; }
    printf("ok\n");
    return 0;
}

// See analyse.h. The shape:
//
//   walk      rebuild the game from the code and call back at every decision
//   belief    what the deciding seat could know, and the worlds that admits
//   evaluate  every legal move x every world, paired, played out or solved
//   verdict   the rule that turns the numbers into a word
//   write     the packed result, and the reader beside it
//
// Everything expensive is per (candidate, world), and every (candidate, world)
// is independent of every other, which is why the world loop fans out over
// pthreads natively (AnalyseParams.threads) and why a wasm build could run it
// over shared-memory threads the way oracle-mt does. All scratch is
// thread-local, and the result does not depend on the thread count.

#include "analyse.h"

#include "bot_drive.h"
#include "bot_roster.h"
#include "cordite_sim.h"
#include "replay.h"
#include "replay_steps.h"
#include "strategy.h"

#include <math.h>
#include <string.h>
#ifndef __wasm__
#include <pthread.h>
#include <time.h>
#endif

// ---------- limits ------------------------------------------------------------
#define AN_MAX_NODES   512    // decisions per analysis (a 2p game has ~60 per seat; 8p ~400 in all)
#define AN_MAX_WORLDS  4096   // (world, future) pairs per node
#define AN_MAX_CANDS   ANALYSE_MAX_CANDS
#define AN_SOLVE_DEPTH SOLVE_SCRATCH_DEPTH
#define AN_PLAYOUT_CYCLES 4000

// ---------- small helpers -----------------------------------------------------
static uint32_t an_xorshift(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ? s : 0x9E3779B9u;
}
static uint32_t an_mix(uint32_t a, uint32_t b) {
    uint32_t h = a ^ 0x9E3779B9u;
    h ^= b + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h *= 0x85EBCA77u; h ^= h >> 13; h *= 0xC2B2AE3Du; h ^= h >> 16;
    return h ? h : 1u;
}

static uint32_t an_now_ms(void) {
#ifdef __wasm__
    return 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static int an_in_count(const Game *g) {
    int n = 0;
    for (int i = 0; i < g->num_players; i++)
        if (g->players[i].status == PLAYER_STATUS_IN) n++;
    return n;
}

static int an_card_score(Card c, int trump) {
    return c.value + (c.suit == trump ? 20 : 0);
}

static bool an_apply(Game *g, int seat, const LegalMove *m) {
    switch (m->type) {
        case MOVE_ATTACK: return handle_attack(g, seat, m->cards, m->n_cards);
        case MOVE_COVER:  return handle_cover (g, seat, m->cards, m->attack_cards, m->n_cards);
        case MOVE_PASS:   return handle_pass  (g, seat, m->cards, m->n_cards);
        case MOVE_PICKUP: return handle_pickup(g, seat);
        case MOVE_GOOD:   return handle_good  (g, seat);
        default:          return false;
    }
}

// The seat's finish position on a finished board: 1 = first out, N = the fool.
static int an_finish_of(const Game *g, int seat) {
    for (int i = 0; i < g->num_eliminated; i++)
        if (g->elimination_order[i] == seat) return i + 1;
    return g->num_players;
}

// ---------- the belief --------------------------------------------------------
//
// PUBLIC information only: the seat's own hand, the table, the discard, the
// flip, and the cards it watched opponents pick up (minus the ones they have
// played since). No inference from behaviour - a void, a rank floor - because
// the analyser asks "what was available to you", and that question has an
// exact answer. The table is tracked from the per-move records, never read off
// LOG_PICKUP / LOG_DISCARD card lists, which truncate at MAX_LOG_PAIRS.

static bool an_set_has(const Card *arr, int n, Card c) {
    for (int i = 0; i < n; i++) if (card_eq(arr[i], c)) return true;
    return false;
}

static void an_pinned_remove(AnalyseBelief *B, int p, Card c) {
    for (int q = 0; q < B->pinned_n[p]; q++) {
        if (card_eq(B->pinned[p][q], c)) {
            B->pinned[p][q] = B->pinned[p][B->pinned_n[p] - 1];
            B->pinned_n[p]--;
            return;
        }
    }
}

static void an_pinned_add(AnalyseBelief *B, int p, Card c) {
    if (B->pinned_n[p] >= MAX_HAND_SIZE) return;
    if (an_set_has(B->pinned[p], B->pinned_n[p], c)) return;
    B->pinned[p][B->pinned_n[p]++] = c;
}

void analyse_belief(const Game *g, int seat, AnalyseBelief *B) {
    memset(B, 0, sizeof *B);

    int last_draw = -1;
    for (int i = 0; i < g->num_logs; i++)
        if (g->logs[i].log_type == LOG_DRAW) last_draw = i;
    bool deck_alive = (g->deck_count > 0 || g->has_flipped);
    // The last draw of a dead deck took the flip, and the flip is public.
    int flip_log = deck_alive ? -1 : last_draw;

    Card tbl[MAX_BATTLES * 2];
    int  tbl_n = 0;
    Card disc[MAX_DECK];
    int  disc_n = 0;

    for (int i = 0; i < g->num_logs; i++) {
        const GameLog *L = &g->logs[i];
        int p = L->player_idx;
        switch (L->log_type) {
            case LOG_ATTACK:
            case LOG_PASS:
            case LOG_COVER:
                for (int k = 0; k < L->num_pairs; k++) {
                    Card c = L->pairs[k].primary;
                    if (tbl_n < (int)(sizeof tbl / sizeof tbl[0])) tbl[tbl_n++] = c;
                    if (p >= 0) an_pinned_remove(B, p, c);
                }
                break;
            case LOG_PICKUP:
                if (p >= 0) for (int k = 0; k < tbl_n; k++) an_pinned_add(B, p, tbl[k]);
                tbl_n = 0;
                break;
            case LOG_DISCARD:
                for (int k = 0; k < tbl_n && disc_n < MAX_DECK; k++) disc[disc_n++] = tbl[k];
                tbl_n = 0;
                break;
            case LOG_DRAW:
                if (p >= 0 && i == flip_log && L->num_pairs > 0) {
                    Card fc = L->pairs[L->num_pairs - 1].primary;
                    if (fc.suit >= 0 && fc.value > 0) an_pinned_add(B, p, fc);
                }
                break;
            default:
                break;
        }
    }

    // A seat that left holds nothing; the seat itself locates its whole hand.
    for (int p = 0; p < g->num_players; p++) {
        if (p == seat || g->players[p].status != PLAYER_STATUS_IN) B->pinned_n[p] = 0;
    }

    Card known[MAX_DECK * 2];
    int kn = 0;
    const Player *me = &g->players[seat];
    for (int j = 0; j < me->hand_count && kn < (int)(sizeof known / sizeof known[0]); j++) known[kn++] = me->hand[j];
    for (int b = 0; b < g->num_battles && kn + 2 <= (int)(sizeof known / sizeof known[0]); b++) {
        known[kn++] = g->table_battles[b].attack;
        if (!card_is_none(g->table_battles[b].defense)) known[kn++] = g->table_battles[b].defense;
    }
    if (g->has_flipped && kn < (int)(sizeof known / sizeof known[0])) known[kn++] = g->flipped;
    for (int i = 0; i < disc_n && kn < (int)(sizeof known / sizeof known[0]); i++) known[kn++] = disc[i];
    for (int p = 0; p < g->num_players; p++)
        for (int j = 0; j < B->pinned_n[p] && kn < (int)(sizeof known / sizeof known[0]); j++) known[kn++] = B->pinned[p][j];

    int start_v = min_value_for(g->num_players);
    B->n = 0;
    for (int s = 0; s < NUM_SUITS; s++) {
        for (int v = start_v; v <= ACE_VALUE; v++) {
            Card c = { (int8_t)s, (int8_t)v };
            if (!an_set_has(known, kn, c) && B->n < MAX_DECK) B->pool[B->n++] = c;
        }
    }

    int need = g->deck_count;
    B->deck_n = g->deck_count;
    for (int p = 0; p < g->num_players; p++) {
        B->free_n[p] = 0;
        if (p == seat || g->players[p].status != PLAYER_STATUS_IN) continue;
        B->free_n[p] = g->players[p].hand_count - B->pinned_n[p];
        if (B->free_n[p] < 0) { B->ok = 0; return; }
        need += B->free_n[p];
    }
    B->ok = (need == B->n);
}

void analyse_install_world(Game *g, int seat, const AnalyseBelief *B, const Card *perm) {
    int k = 0;
    for (int p = 0; p < g->num_players; p++) {
        if (p == seat || g->players[p].status != PLAYER_STATUS_IN) continue;
        Player *pl = &g->players[p];
        int n = 0;
        for (int j = 0; j < B->pinned_n[p]; j++) pl->hand[n++] = B->pinned[p][j];
        for (int j = 0; j < B->free_n[p]; j++) pl->hand[n++] = perm[k++];
        pl->hand_count = (int8_t)n;
    }
    for (int i = 0; i < B->deck_n; i++) g->deck[i] = perm[k++];
    g->deck_count = (int16_t)B->deck_n;
    g->deterministic_deck = true;
}

// ---------- the deal report -----------------------------------------------------

static double an_lchoose(int n, int k) {
    if (k < 0 || k > n) return -1e300;
    return lgamma((double)n + 1) - lgamma((double)k + 1) - lgamma((double)(n - k) + 1);
}

double analyse_hypergeom(int deck, int trumps, int k) {
    if (k < 0 || k > CARDS_PER_PLAYER || k > trumps || CARDS_PER_PLAYER - k > deck - trumps) return 0.0;
    double l = an_lchoose(trumps, k) + an_lchoose(deck - trumps, CARDS_PER_PLAYER - k)
             - an_lchoose(deck, CARDS_PER_PLAYER);
    return exp(l);
}

double analyse_hypergeom_at_most(int deck, int trumps, int k) {
    double s = 0.0;
    for (int i = 0; i <= k; i++) s += analyse_hypergeom(deck, trumps, i);
    return s > 1.0 ? 1.0 : s;
}

// ---------- the verdict ----------------------------------------------------------

// A mistake has to be worth at least this much of a finish position to be
// called one; below it the numbers are noise even when the CI agrees. Chosen
// by reproducing the hand analysis of the sample game (docs/POST_GAME_ANALYSER.md):
// its smallest real gap is 0.09, its largest non-mistake 0.02.
#define AN_MIN_LOSS 0.03
#define AN_Z        1.96

int analyse_verdict(int n_cands, int played_best, int all_lost, double loss, double se, int proof) {
    if (n_cands <= 1) return ANALYSE_V_FORCED;
    if (all_lost) return ANALYSE_V_LOST;
    if (played_best || loss < AN_MIN_LOSS) return ANALYSE_V_BEST;
    if (proof) return ANALYSE_V_CHANCE;             // exact over the worlds: no sampling error
    if (loss - AN_Z * se <= 0.0) return ANALYSE_V_DECLINED;
    return ANALYSE_V_CHANCE;
}

// ---------- exact play -----------------------------------------------------------
//
// A world with at most one stock card and two seats in is a game of perfect
// information once the world is fixed, so a candidate can be SOLVED in it.
// Full minimax over the real kernel (calculate_legal_moves + handle_*), with
// the bitboard solver taking the deck-empty tails. Returns +1 (the seat
// escapes), -1 (the seat is the fool) or 0 (not resolved inside the budget).
// The opponent moves with the defender-priority abstraction og_solve uses.

static int an_exact_rec(Game *g, int me, long *budget, int depth) {
    int loser = game_done(g);
    if (loser >= 0) return (loser == me) ? -1 : 1;
    if (an_in_count(g) != 2) return 0;
    if (g->deck_count == 0 && !g->has_flipped) {
        SimState s;
        cd_sim_from_game(&s, g);
        int aborted = 0;
        int v = cd_sim_solve_d(&s, me, -1, 1, budget, depth, &aborted);
        if (aborted || v == 0 || *budget <= 0) return 0;
        return v > 0 ? 1 : -1;
    }
    if (depth >= AN_SOLVE_DEPTH) return 0;
    if (--*budget <= 0) return 0;

    int actor = -1;
    if (should_bot_act(g, g->defender)) actor = g->defender;
    else for (int i = 0; i < g->num_players; i++) if (should_bot_act(g, i)) { actor = i; break; }
    if (actor < 0) return 0;

    SolveMoves *mv = &solve_scratch_mv()[depth];
    legal_set_move_cap(SOLVE_SCRATCH_MOVES);
    calculate_legal_moves(g, actor, (LegalMoves *)mv);
    legal_set_move_cap(0);
    if (mv->n == 0 || mv->n >= SOLVE_SCRATCH_MOVES) return 0;

    bool maximizing = (actor == me);
    bool unknown = false;
    for (int i = 0; i < mv->n; i++) {
        Game *child = solve_scratch_child(depth);
        solve_clone_prefix(child, g);
        if (!an_apply(child, actor, &mv->moves[i])) continue;
        int v = an_exact_rec(child, me, budget, depth + 1);
        if (maximizing && v > 0) return 1;
        if (!maximizing && v < 0) return -1;
        if (v == 0) unknown = true;
        if (*budget <= 0) return 0;
    }
    if (unknown) return 0;
    return maximizing ? -1 : 1;
}

static int an_exact(const Game *g, int me, long budget) {
    if (budget <= 0) return 0;
    int loser = game_done(g);
    if (loser >= 0) return (loser == me) ? -1 : 1;   // the candidate ended the game
    if (an_in_count(g) != 2) return 0;
    if (g->deck_count > 1) return 0;
    Game *root = solve_scratch_root();
    solve_clone_root(root, g);
    cd_sim_solve_reset();
    long b = budget;
    return an_exact_rec(root, me, &b, 0);
}

// ---------- the playout -----------------------------------------------------------

static int an_playout(Game *g, int seat, int strat) {
    for (int p = 0; p < g->num_players; p++) g->players[p].strategy_key = (int8_t)strat;
    BotDriveOut drv;
    for (int it = 0; it < AN_PLAYOUT_CYCLES && game_done(g) < 0; it++) {
        int n = bot_drive(g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &drv);
        if (drv.stop == BOT_STOP_ENDED) break;
        if (n <= 0) return 0;            // nobody can act: a stalled board, not a result
    }
    if (game_done(g) < 0) return 0;
    return an_finish_of(g, seat);
}

// ---------- node results ------------------------------------------------------------

typedef struct {
    uint16_t n, n_fool;
    double   sum_fp;          // over evaluated worlds
    double   diff_sum, diff_sq;   // (this - played), over shared worlds
    uint16_t proven_wins, proven_losses, n_exact;
} AnCandStat;

typedef struct {
    uint16_t step;
    uint8_t  seat, sub;           // sub: which of a ROUND_END's attackers
    uint8_t  verdict, flags;
    uint8_t  n_cands, played, best;
    uint16_t n_worlds;
    uint8_t  unknown, deck;
    double   win_prob, loss, loss_se;
    uint16_t cand_move[AN_MAX_CANDS];   // legal-move indices, in candidate order
    AnCandStat st[AN_MAX_CANDS];
    // deep pass
    uint16_t deep_n_worlds;
    uint8_t  deep_best;
    double   deep_loss, deep_loss_se;
    AnCandStat deep[AN_MAX_CANDS];
} AnNode;

static AnNode g_nodes[AN_MAX_NODES];
static int    g_n_nodes;

// Counters for the cost line.
static uint32_t g_playouts, g_solves;
static int g_replay_err;
int analyse_last_replay_error(void) { return g_replay_err; }

// ---------- candidates ------------------------------------------------------------

static void an_pick_candidates(const Game *g, const LegalMoves *moves, int played, int cap,
                               uint16_t *out, int *n_out, int *capped) {
    *capped = 0;
    if (cap <= 0 || moves->n <= cap) {
        int n = moves->n < AN_MAX_CANDS ? moves->n : AN_MAX_CANDS;
        for (int i = 0; i < n; i++) out[i] = (uint16_t)i;
        if (moves->n > AN_MAX_CANDS) *capped = 1;
        // The played move must be a candidate whatever the cap did.
        if (played >= n) { out[n - 1] = (uint16_t)played; }
        *n_out = n;
        return;
    }
    *capped = 1;
    int trump = g->power_suit;
    // Always: the played move, pickup, good. Then the cheapest of each family,
    // more cards first (a full cover before a partial one), interleaved.
    int n = 0;
    bool taken[MAX_LEGAL_MOVES > 65535 ? 65535 : MAX_LEGAL_MOVES];
    memset(taken, 0, sizeof taken);
    out[n++] = (uint16_t)played; taken[played] = true;
    for (int i = 0; i < moves->n && n < cap; i++) {
        int t = moves->moves[i].type;
        if ((t == MOVE_PICKUP || t == MOVE_GOOD) && !taken[i]) { out[n++] = (uint16_t)i; taken[i] = true; }
    }
    while (n < cap) {
        bool any = false;
        for (int fam = MOVE_ATTACK; fam <= MOVE_PASS && n < cap; fam++) {
            int best = -1; double best_k = 1e300;
            for (int i = 0; i < moves->n; i++) {
                const LegalMove *m = &moves->moves[i];
                if (taken[i] || m->type != fam) continue;
                int sum = 0;
                for (int j = 0; j < m->n_cards; j++) sum += an_card_score(m->cards[j], trump);
                double k = -(double)m->n_cards * 10000.0 + (double)sum;
                if (k < best_k) { best_k = k; best = i; }
            }
            if (best >= 0) { out[n++] = (uint16_t)best; taken[best] = true; any = true; }
        }
        if (!any) break;
    }
    *n_out = n;
}

// ---------- worlds ------------------------------------------------------------------
//
// A world is a permutation of the pool: opponents' free slots first (seat
// order), the stock after, in draw order. Enumeration walks the distinct
// permutations of the LABEL multiset (which slot-owner each pool card goes to),
// so two worlds that differ only in the order of a hand are one world; the
// stock's order within a world is then a FUTURE, fixed when the stock holds at
// most one card and sampled otherwise.

typedef struct {
    int n;                      // pool size
    int n_groups;               // opponents with free slots, then the stock
    int group_size[MAX_PLAYERS + 1];
    int group_seat[MAX_PLAYERS + 1];   // -1 for the stock
    int deck_group;             // index of the stock's group, or -1 when the stock is empty
} AnWorldShape;

static void an_shape(const Game *g, int seat, const AnalyseBelief *B, AnWorldShape *S) {
    S->n = B->n;
    S->n_groups = 0;
    S->deck_group = -1;
    for (int p = 0; p < g->num_players; p++) {
        if (p == seat || B->free_n[p] <= 0) continue;
        S->group_size[S->n_groups] = B->free_n[p];
        S->group_seat[S->n_groups] = p;
        S->n_groups++;
    }
    if (B->deck_n > 0) {
        S->deck_group = S->n_groups;
        S->group_size[S->n_groups] = B->deck_n;
        S->group_seat[S->n_groups] = -1;
        S->n_groups++;
    }
}

// Number of distinct label permutations, or -1 past `cap`.
static long an_world_count(const AnWorldShape *S, long cap) {
    // multinomial(n; sizes) computed as a product of binomials, capped.
    long total = 1;
    int left = S->n;
    for (int gi = 0; gi < S->n_groups; gi++) {
        int k = S->group_size[gi];
        // C(left, k)
        long c = 1;
        for (int i = 1; i <= k; i++) {
            c = c * (left - k + i) / i;
            if (c > cap) return -1;
        }
        if (total > cap / (c ? c : 1)) return -1;
        total *= c;
        if (total > cap) return -1;
        left -= k;
    }
    return total;
}

// Lexicographic next permutation of labels[0..n); false when exhausted.
static bool an_next_perm(uint8_t *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) return false;
    int j = n - 1;
    while (a[j] <= a[i]) j--;
    uint8_t t = a[i]; a[i] = a[j]; a[j] = t;
    for (int l = i + 1, r = n - 1; l < r; l++, r--) { t = a[l]; a[l] = a[r]; a[r] = t; }
    return true;
}

// Turn labels into the perm analyse_install_world takes: group by group, pool
// cards in pool order within a group, and the stock shuffled by `future_seed`
// when it holds more than one card (future_seed == 0: pool order).
static void an_perm_from_labels(const AnalyseBelief *B, const AnWorldShape *S,
                                const uint8_t *labels, uint32_t future_seed, Card *perm) {
    int k = 0;
    for (int gi = 0; gi < S->n_groups; gi++) {
        int start = k;
        for (int i = 0; i < B->n; i++) if (labels[i] == gi) perm[k++] = B->pool[i];
        if (gi == S->deck_group && future_seed && k - start > 1) {
            uint32_t s = future_seed;
            for (int i = k - start - 1; i > 0; i--) {
                s = an_xorshift(s);
                int j = (int)(s % (uint32_t)(i + 1));
                Card t = perm[start + i]; perm[start + i] = perm[start + j]; perm[start + j] = t;
            }
        }
    }
}

void analyse_sample_world(const AnalyseBelief *B, uint32_t seed, Card *perm) {
    for (int i = 0; i < B->n; i++) perm[i] = B->pool[i];
    uint32_t s = seed;
    for (int i = B->n - 1; i > 0; i--) {
        s = an_xorshift(s);
        int j = (int)(s % (uint32_t)(i + 1));
        Card t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
}

// ---------- evaluation --------------------------------------------------------------

// Per-thread scratch: the world being played and the menu at the node.
static _Thread_local Game an_world_g;

typedef struct {
    int8_t  fp;      // finish position of the seat, 0 = unterminated
    int8_t  exact;   // 1 = fp came from the exact solve
} AnCell;

// The (candidate, world) results of the node under evaluation.
static AnCell g_cells[AN_MAX_CANDS][AN_MAX_WORLDS];
// The worlds of the node under evaluation.
static Card    g_perms[AN_MAX_WORLDS][MAX_DECK];
static uint32_t g_wseed[AN_MAX_WORLDS];

// One (world, candidate) cell: install the world, play the candidate, then
// solve it exactly where that is possible and play it out with the engine
// where it is not. Everything it touches is thread-local or its own cell.
typedef struct {
    const Game *g; int seat; const LegalMoves *moves; const AnNode *node;
    const AnalyseBelief *B; const AnalyseParams *p; int strat; int n_worlds;
    bool exact_eligible;
    int tid, nthreads;           // this worker's stride over the worlds
    uint32_t playouts, solves;   // this worker's counts
} AnWork;

static void an_cell(AnWork *W, int w, int ci) {
    const Game *g = W->g;
    Game *wg = &an_world_g;
    game_clone(wg, g);
    analyse_install_world(wg, W->seat, W->B, g_perms[w]);
    // Common random numbers: every candidate at this world sees the same
    // streams, and a cold transposition table, so a budget-bounded solve
    // inside a bot resolves the same way whichever candidate ran first.
    game_rng_set(g_wseed[w]);
    random_strategy_set_seed(an_mix(g_wseed[w], 0xA5A5A5A5u));
    cd_sim_solve_reset();
    AnCell cell = { 0, 0 };
    if (!an_apply(wg, W->seat, &W->moves->moves[W->node->cand_move[ci]])) {
        cell.fp = (int8_t)g->num_players;   // an illegal move here is the fool's
    } else {
        int r = 0;
        if (W->exact_eligible) {
            W->solves++;
            r = an_exact(wg, W->seat, W->p->solve_budget);
        }
        if (r != 0) {
            cell.exact = 1;
            cell.fp = (int8_t)(r > 0 ? g->num_players - 1 : g->num_players);
        } else {
            W->playouts++;
            int fp = an_playout(wg, W->seat, W->strat);
            cell.fp = (int8_t)(fp > 0 ? fp : g->num_players);
        }
    }
    g_cells[ci][w] = cell;
}

static void *an_worker(void *arg) {
    AnWork *W = (AnWork *)arg;
    for (int w = W->tid; w < W->n_worlds; w += W->nthreads)
        for (int ci = 0; ci < W->node->n_cands; ci++) an_cell(W, w, ci);
    return 0;
}

// The world loop, across `p->threads` threads (0 or 1 = this thread only).
// Worlds are dealt to workers by stride and every cell is a pure function of
// its (world, candidate), so the result is the same at any thread count -
// analyse_test holds that true. Thread stacks are sized like the main one: a
// playout runs whole bots, and the arena caps' menus are not small.
#define AN_MAX_THREADS 64
#define AN_THREAD_STACK (8u << 20)

static void an_run_worlds(AnWork *W) {
    int nthreads = W->p->threads;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > AN_MAX_THREADS) nthreads = AN_MAX_THREADS;
    if (nthreads > W->n_worlds) nthreads = W->n_worlds;
    if (nthreads < 1) nthreads = 1;
    W->tid = 0; W->nthreads = nthreads; W->playouts = 0; W->solves = 0;
#ifndef __wasm__
    AnWork    part[AN_MAX_THREADS];
    pthread_t tids[AN_MAX_THREADS];
    int spawned = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AN_THREAD_STACK);
    for (int t = 1; t < nthreads; t++) {
        part[t] = *W;
        part[t].tid = t;
        if (pthread_create(&tids[t], &attr, an_worker, &part[t]) != 0) break;
        spawned = t;
    }
    pthread_attr_destroy(&attr);
    // Whatever could not be spawned this thread does itself, by widening its
    // own stride to cover the missing workers' worlds.
    if (spawned + 1 < nthreads) {
        AnWork rest = *W;
        for (int t = spawned + 1; t < nthreads; t++) { rest.tid = t; an_worker(&rest); }
        W->playouts += rest.playouts; W->solves += rest.solves;
    }
    an_worker(W);
    for (int t = 1; t <= spawned; t++) {
        pthread_join(tids[t], 0);
        W->playouts += part[t].playouts;
        W->solves += part[t].solves;
    }
#else
    W->nthreads = 1;
    an_worker(W);
#endif
    g_playouts += W->playouts;
    g_solves   += W->solves;
}

// Evaluate every candidate of `node` on the world list. `st` receives the
// per-candidate statistics; returns the world count.
static int an_evaluate(const Game *g, int seat, const LegalMoves *moves, AnNode *node,
                       const AnalyseBelief *B, const AnalyseParams *p, int strat,
                       int n_worlds_wanted, AnCandStat *st, uint8_t *flags_out) {
    AnWorldShape S;
    an_shape(g, seat, B, &S);

    int n_worlds = 0;
    uint8_t flags = 0;
    uint32_t base = an_mix(an_mix(p->seed, (uint32_t)node->step * 0x9E3779B9u), (uint32_t)(seat * 131 + node->sub));

    // Enumerate when the hand assignments fit the budget; the deck orders then
    // get whatever is left of it, one at least. Hands before futures, because a
    // hand assignment is what the seat could not know, while a deck order is
    // only which of those unknowns arrives first.
    long count = p->exhaustive_cap > 0 ? an_world_count(&S, p->exhaustive_cap) : -1;
    int futures = 1;
    if (count > 0 && B->deck_n > 1) {
        futures = (int)(p->exhaustive_cap / count);
        if (futures > p->futures) futures = p->futures;
        if (futures < 1) futures = 1;
    }
    if (count >= 0 && count * futures <= AN_MAX_WORLDS) {
        flags |= ANALYSE_NF_EXHAUSTIVE;
        if (B->deck_n > 1) flags |= ANALYSE_NF_FUTURES;
        uint8_t labels[MAX_DECK];
        int k = 0;
        for (int gi = 0; gi < S.n_groups; gi++)
            for (int i = 0; i < S.group_size[gi]; i++) labels[k++] = (uint8_t)gi;
        if (k == 0) {
            // Nothing unknown: one world, the board itself.
            n_worlds = 1;
            g_wseed[0] = an_mix(base, 1u);
        } else {
            do {
                for (int f = 0; f < futures && n_worlds < AN_MAX_WORLDS; f++) {
                    uint32_t fs = (B->deck_n > 1) ? an_mix(base, (uint32_t)(n_worlds + 1) * 0x85EBCA77u) : 0;
                    an_perm_from_labels(B, &S, labels, fs, g_perms[n_worlds]);
                    g_wseed[n_worlds] = an_mix(base, (uint32_t)(n_worlds + 1));
                    n_worlds++;
                }
            } while (n_worlds < AN_MAX_WORLDS && an_next_perm(labels, k));
        }
    } else {
        n_worlds = n_worlds_wanted;
        if (n_worlds < 1) n_worlds = 1;
        if (n_worlds > AN_MAX_WORLDS) n_worlds = AN_MAX_WORLDS;
        for (int w = 0; w < n_worlds; w++) {
            g_wseed[w] = an_mix(base, (uint32_t)(w + 1));
            analyse_sample_world(B, an_mix(g_wseed[w], 0x51AB1E5u), g_perms[w]);
        }
    }

    int nc = node->n_cands;
    AnWork W;
    W.g = g; W.seat = seat; W.moves = moves; W.node = node; W.B = B; W.p = p;
    W.strat = strat; W.n_worlds = n_worlds;
    W.exact_eligible = p->solve_budget > 0 && an_in_count(g) == 2 && B->deck_n <= 1;
    an_run_worlds(&W);

    // Statistics, paired against the played move.
    int pi = node->played;
    memset(st, 0, sizeof(AnCandStat) * (size_t)nc);
    for (int ci = 0; ci < nc; ci++) {
        for (int w = 0; w < n_worlds; w++) {
            const AnCell *c = &g_cells[ci][w];
            st[ci].n++;
            st[ci].sum_fp += c->fp;
            if (c->fp == g->num_players) st[ci].n_fool++;
            if (c->exact) {
                st[ci].n_exact++;
                if (c->fp == g->num_players) st[ci].proven_losses++; else st[ci].proven_wins++;
            }
            double d = (double)c->fp - (double)g_cells[pi][w].fp;
            st[ci].diff_sum += d;
            st[ci].diff_sq  += d * d;
        }
    }
    bool all_exact = true;
    for (int ci = 0; ci < nc; ci++) if (st[ci].n_exact != st[ci].n) all_exact = false;
    if (all_exact && n_worlds > 0) flags |= ANALYSE_NF_PROOF;
    *flags_out = flags;
    return n_worlds;
}

static double an_mean(const AnCandStat *s) { return s->n ? s->sum_fp / s->n : 0.0; }
static double an_diff_mean(const AnCandStat *s) { return s->n ? s->diff_sum / s->n : 0.0; }
static double an_diff_se(const AnCandStat *s) {
    if (s->n < 2) return 0.0;
    double m = s->diff_sum / s->n;
    double var = (s->diff_sq - s->n * m * m) / (s->n - 1);
    if (var < 0) var = 0;
    return sqrt(var / s->n);
}

// Best candidate: lowest mean finish; ties keep the played move, else the
// first. Returns the index.
static int an_best_of(const AnCandStat *st, int nc, int played) {
    int best = played;
    double bv = an_mean(&st[played]);
    for (int ci = 0; ci < nc; ci++) {
        double v = an_mean(&st[ci]);
        if (v < bv - 1e-12) { bv = v; best = ci; }
    }
    return best;
}

static void an_settle(const AnNode *node, const AnCandStat *st, int proof,
                      int *best_out, double *loss_out, double *se_out, int *verdict_out) {
    int nc = node->n_cands;
    int best = an_best_of(st, nc, node->played);
    double loss = -an_diff_mean(&st[best]);
    double se = proof ? 0.0 : an_diff_se(&st[best]);
    bool all_lost = true;
    for (int ci = 0; ci < nc; ci++) if (st[ci].n_fool != st[ci].n) all_lost = false;
    *best_out = best;
    *loss_out = loss;
    *se_out = se;
    *verdict_out = analyse_verdict(nc, best == node->played, all_lost, loss, se, proof);
}

// ---------- the walk ------------------------------------------------------------------

typedef struct {
    ReplayHeader hdr;
    Card         deck[MAX_DECK];
    int          n_deck;
    int          n_acts;
} AnCode;

static ReplayAction g_acts[REPLAY_MAX_ACTIONS];
static Game         g_real;
static LegalMoves   g_menu;

static int an_decode(const unsigned char *code, int code_len, AnCode *C) {
    int r = replay_deal_v6(code, code_len, &C->hdr, C->deck, MAX_DECK, &C->n_deck,
                           g_acts, REPLAY_MAX_ACTIONS, &C->n_acts);
    if (r < 0) { g_replay_err = r; return -ANALYSE_EREPLAY; }
    return 0;
}

// Find the recorded action in the menu. -1 when absent.
static int an_match(const LegalMoves *mv, const ReplayAction *a, int kind_override) {
    int kind = kind_override >= 0 ? kind_override : a->kind;
    int want;
    switch (kind) {
        case REPLAY_ATOM_ATTACK: want = MOVE_ATTACK; break;
        case REPLAY_ATOM_COVER:  want = MOVE_COVER;  break;
        case REPLAY_ATOM_PASS:   want = MOVE_PASS;   break;
        case REPLAY_ATOM_PICKUP: want = MOVE_PICKUP; break;
        case REPLAY_ATOM_GOOD:
        case REPLAY_ATOM_ROUND_END: want = MOVE_GOOD; break;
        default: return -1;
    }
    for (int i = 0; i < mv->n; i++) {
        const LegalMove *m = &mv->moves[i];
        if (m->type != want) continue;
        if (want == MOVE_PICKUP || want == MOVE_GOOD) return i;
        int n = (want == MOVE_COVER) ? 1 : a->n_cards;
        if (m->n_cards != n) continue;
        bool used[REPLAY_MAX_PAIRS] = { false };
        bool ok = true;
        for (int k = 0; k < n && ok; k++) {
            int f = -1;
            for (int j = 0; j < n; j++) {
                if (used[j]) continue;
                if (!card_eq(m->cards[j], a->cards[k])) continue;
                if (want == MOVE_COVER && !card_eq(m->attack_cards[j], a->target)) continue;
                f = j; break;
            }
            if (f < 0) ok = false; else used[f] = true;
        }
        if (ok) return i;
    }
    return -1;
}

// Called at every decision of the walk. `sub` numbers the attackers of one
// ROUND_END. Return 0 to stop the walk.
typedef int (*AnVisit)(void *ctx, const Game *g, int step, int sub, int seat,
                       const LegalMoves *mv, int played);

static int an_walk(const AnCode *C, int want_seat, AnVisit visit, void *ctx) {
    int r = replay_deal_start(&g_real, &C->hdr, C->deck, C->n_deck);
    if (r < 0) { g_replay_err = r; return -ANALYSE_EREPLAY; }
    for (int i = 0; i < C->n_acts; i++) {
        const ReplayAction *a = &g_acts[i];
        if (a->kind == REPLAY_ATOM_ROUND_END) {
            // Every attacker still to declare says good, and each is a decision
            // (throw in, or close the bout). Collect them BEFORE the first good:
            // the last one triggers the transition, after which the seats that
            // follow it in the loop are the next bout's attackers.
            int who[MAX_PLAYERS], n_who = 0;
            for (int s = 0; s < g_real.num_players; s++) {
                if (s == g_real.defender || g_real.players[s].status != PLAYER_STATUS_IN) continue;
                if (should_bot_act(&g_real, s)) who[n_who++] = s;
            }
            for (int k = 0; k < n_who; k++) {
                int s = who[k];
                if ((want_seat < 0 || want_seat == s) && should_bot_act(&g_real, s)) {
                    calculate_legal_moves(&g_real, s, &g_menu);
                    int played = an_match(&g_menu, a, REPLAY_ATOM_GOOD);
                    if (played < 0) return -ANALYSE_EMENU;
                    if (!visit(ctx, &g_real, i, k, s, &g_menu, played)) return 0;
                }
                handle_good(&g_real, s);
            }
            if (g_real.num_battles > 0) engine_run_round_transition(&g_real);
            continue;
        }
        if (want_seat < 0 || want_seat == a->seat) {
            calculate_legal_moves(&g_real, a->seat, &g_menu);
            int played = an_match(&g_menu, a, -1);
            if (played < 0) return -ANALYSE_EMENU;
            if (!visit(ctx, &g_real, i, 0, a->seat, &g_menu, played)) return 0;
        }
        replay_action_apply(&g_real, a);
    }
    return 0;
}

// ---------- pass 1: the scan ------------------------------------------------------------

typedef struct {
    const AnalyseParams *p;
    int strat;
    int err;
} AnScanCtx;

static int an_scan_visit(void *vctx, const Game *g, int step, int sub, int seat,
                         const LegalMoves *mv, int played) {
    AnScanCtx *ctx = (AnScanCtx *)vctx;
    if (g_n_nodes >= AN_MAX_NODES) return 0;
    AnNode *node = &g_nodes[g_n_nodes];
    memset(node, 0, sizeof *node);
    node->step = (uint16_t)step;
    node->sub = (uint8_t)sub;
    node->seat = (uint8_t)seat;
    node->deck = (uint8_t)g->deck_count;

    int capped = 0, nc = 0;
    an_pick_candidates(g, mv, played, ctx->p->max_candidates, node->cand_move, &nc, &capped);
    node->n_cands = (uint8_t)nc;
    if (capped) node->flags |= ANALYSE_NF_CAPPED;
    for (int i = 0; i < nc; i++) if (node->cand_move[i] == played) node->played = (uint8_t)i;
    node->best = node->played;

    AnalyseBelief B;
    analyse_belief(g, seat, &B);
    node->unknown = (uint8_t)B.n;
    if (!B.ok) {
        node->flags |= ANALYSE_NF_BELIEF_FAIL;
        node->verdict = ANALYSE_V_FORCED;
        g_n_nodes++;
        return 1;
    }

    if (nc <= 1) {
        node->verdict = ANALYSE_V_FORCED;
        node->n_worlds = 0;
        g_n_nodes++;
        return 1;
    }

    uint8_t flags = 0;
    node->n_worlds = (uint16_t)an_evaluate(g, seat, mv, node, &B, ctx->p, ctx->strat,
                                           ctx->p->worlds, node->st, &flags);
    node->flags |= flags;
    int best, verdict;
    double loss, se;
    an_settle(node, node->st, (flags & ANALYSE_NF_PROOF) != 0, &best, &loss, &se, &verdict);
    node->best = (uint8_t)best;
    node->loss = loss;
    node->loss_se = se;
    node->verdict = (uint8_t)verdict;
    const AnCandStat *ps = &node->st[node->played];
    node->win_prob = ps->n ? 1.0 - (double)ps->n_fool / ps->n : 0.0;
    g_n_nodes++;
    return 1;
}

// ---------- pass 2: the deep pass -------------------------------------------------------

typedef struct {
    const AnalyseParams *p;
    int strat;
    int idx[AN_MAX_NODES];   // node indices wanting the deep pass
    int n;
} AnDeepCtx;

static int an_deep_visit(void *vctx, const Game *g, int step, int sub, int seat,
                         const LegalMoves *mv, int played) {
    AnDeepCtx *ctx = (AnDeepCtx *)vctx;
    (void)played;
    for (int k = 0; k < ctx->n; k++) {
        AnNode *node = &g_nodes[ctx->idx[k]];
        if (node->step != step || node->sub != sub || node->seat != seat) continue;
        AnalyseBelief B;
        analyse_belief(g, seat, &B);
        if (!B.ok) return 1;
        uint8_t flags = 0;
        node->deep_n_worlds = (uint16_t)an_evaluate(g, seat, mv, node, &B, ctx->p, ctx->strat,
                                                    ctx->p->deep_worlds, node->deep, &flags);
        int best, verdict;
        double loss, se;
        an_settle(node, node->deep, (flags & ANALYSE_NF_PROOF) != 0, &best, &loss, &se, &verdict);
        node->deep_best = (uint8_t)best;
        node->deep_loss = loss;
        node->deep_loss_se = se;
        node->flags |= ANALYSE_NF_DEEP;
        if (best == node->best) node->flags |= ANALYSE_NF_DEEP_AGREES;
        // The deep pass is the better estimate, so it owns the verdict.
        if (node->verdict != ANALYSE_V_FORCED) node->verdict = (uint8_t)verdict;
        return 1;
    }
    return 1;
}

// ---------- the decisive moment -----------------------------------------------------------
//
// The LAST mistake of the fool's seat after which every later decision of that
// seat was LOST or FORCED: the point past which nothing could have changed the
// result. One per analysis, or none.
static int an_decisive(int fool) {
    if (fool < 0) return -1;
    int last_open = -1;   // last node of the fool's seat that was not LOST/FORCED
    for (int i = g_n_nodes - 1; i >= 0; i--) {
        const AnNode *n = &g_nodes[i];
        if (n->seat != fool) continue;
        if (n->verdict == ANALYSE_V_LOST || n->verdict == ANALYSE_V_FORCED) continue;
        last_open = i;
        break;
    }
    if (last_open < 0) return -1;
    return g_nodes[last_open].verdict == ANALYSE_V_CHANCE ? last_open : -1;
}

// ---------- the writer -------------------------------------------------------------------

static inline int an_put8(unsigned char **q, const unsigned char *end, unsigned v) {
    if (*q + 1 > end) return 0;
    *(*q)++ = (unsigned char)v; return 1;
}
static inline int an_put16(unsigned char **q, const unsigned char *end, unsigned v) {
    if (*q + 2 > end) return 0;
    *(*q)++ = (unsigned char)(v & 0xff); *(*q)++ = (unsigned char)((v >> 8) & 0xff); return 1;
}
static inline int an_put32(unsigned char **q, const unsigned char *end, uint32_t v) {
    if (*q + 4 > end) return 0;
    for (int i = 0; i < 4; i++) *(*q)++ = (unsigned char)((v >> (8 * i)) & 0xff);
    return 1;
}
static inline int an_clamp16(double v) {
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int)(v >= 0 ? v + 0.5 : v - 0.5);
}
static inline unsigned an_prob16(double v) {
    if (v < 0) v = 0; if (v > 1) v = 1;
    return (unsigned)(v * 10000.0 + 0.5);
}

static int an_put_stat(unsigned char **q, const unsigned char *end, const AnCandStat *s) {
    return an_put16(q, end, s->n) && an_put16(q, end, s->n_fool)
        && an_put16(q, end, (unsigned)an_clamp16(an_mean(s) * 1000.0) & 0xffff)
        && an_put16(q, end, (unsigned)an_clamp16(an_diff_mean(s) * 1000.0) & 0xffff)
        && an_put16(q, end, (unsigned)an_clamp16(an_diff_se(s) * 1000.0) & 0xffff);
}

typedef struct {
    unsigned char *q;
    const unsigned char *end;
    int node_i;
    int err;
} AnWriteCtx;

static int an_write_visit(void *vctx, const Game *g, int step, int sub, int seat,
                          const LegalMoves *mv, int played) {
    AnWriteCtx *ctx = (AnWriteCtx *)vctx;
    (void)played; (void)g;
    if (ctx->node_i >= g_n_nodes) return 0;
    const AnNode *n = &g_nodes[ctx->node_i];
    if (n->step != step || n->sub != sub || n->seat != seat) return 1;   // a node the scan skipped
    ctx->node_i++;
    unsigned char **q = &ctx->q;
    const unsigned char *end = ctx->end;
    int ok = an_put16(q, end, n->step) && an_put8(q, end, n->seat) && an_put8(q, end, n->verdict)
          && an_put8(q, end, n->flags) && an_put8(q, end, n->n_cands) && an_put8(q, end, n->played)
          && an_put8(q, end, n->best) && an_put16(q, end, n->n_worlds)
          && an_put8(q, end, n->unknown) && an_put8(q, end, n->deck)
          && an_put16(q, end, an_prob16(n->win_prob))
          && an_put16(q, end, (unsigned)an_clamp16(n->loss * 1000.0) & 0xffff)
          && an_put16(q, end, (unsigned)an_clamp16(n->loss_se * 1000.0) & 0xffff);
    for (int ci = 0; ok && ci < n->n_cands; ci++) {
        const LegalMove *m = &mv->moves[n->cand_move[ci]];
        ok = an_put8(q, end, (unsigned)m->type) && an_put8(q, end, (unsigned)m->n_cards);
        for (int k = 0; ok && k < m->n_cards; k++) ok = an_put8(q, end, (unsigned)card_to_id(m->cards[k]));
        for (int k = 0; ok && k < m->n_cards; k++)
            ok = an_put8(q, end, m->type == MOVE_COVER ? (unsigned)card_to_id(m->attack_cards[k]) : 0xFFu);
        const AnCandStat *s = &n->st[ci];
        int proof = ANALYSE_P_NONE;
        if (s->n && s->n_exact == s->n) {
            proof = (s->proven_wins == s->n) ? ANALYSE_P_WIN
                  : (s->proven_losses == s->n) ? ANALYSE_P_LOSS : ANALYSE_P_MIXED;
        }
        ok = ok && an_put_stat(q, end, s) && an_put8(q, end, (unsigned)(uint8_t)(int8_t)proof)
          && an_put16(q, end, s->proven_wins) && an_put16(q, end, s->proven_losses);
    }
    if (ok && (n->flags & ANALYSE_NF_DEEP)) {
        ok = an_put16(q, end, n->deep_n_worlds) && an_put8(q, end, n->deep_best)
          && an_put16(q, end, (unsigned)an_clamp16(n->deep_loss * 1000.0) & 0xffff)
          && an_put16(q, end, (unsigned)an_clamp16(n->deep_loss_se * 1000.0) & 0xffff);
        for (int ci = 0; ok && ci < n->n_cands; ci++) ok = an_put_stat(q, end, &n->deep[ci]);
    }
    if (!ok) { ctx->err = -ANALYSE_ECAP; return 0; }
    return 1;
}

// ---------- the entry ---------------------------------------------------------------------

void analyse_params_default(AnalyseParams *p) {
    memset(p, 0, sizeof *p);
    p->seat = -1;
    p->roster_idx = bot_roster_find("robusta");
    p->worlds = 24;
    p->futures = 4;
    p->exhaustive_cap = 512;
    p->max_candidates = 0;
    p->solve_budget = 200000L;
    p->deep_roster_idx = -1;
    p->deep_nodes = 3;
    p->deep_worlds = 64;
    p->seed = 1u;
    p->threads = 0;
}

int analyse_packed(const unsigned char *code, int code_len, const AnalyseParams *p,
                   unsigned char *out, int out_cap) {
    if (!code || !p || !out || code_len <= 0) return -ANALYSE_EBADARG;
    const BotRosterEntry *e = bot_roster_at(p->roster_idx);
    if (!e) return -ANALYSE_EBADARG;
    const BotRosterEntry *de = p->deep_roster_idx >= 0 ? bot_roster_at(p->deep_roster_idx) : 0;
    if (p->deep_roster_idx >= 0 && !de) return -ANALYSE_EBADARG;
    if (p->worlds < 1) return -ANALYSE_EBADARG;

    uint32_t t0 = an_now_ms();
    g_playouts = g_solves = 0;
    g_replay_err = 0;
    g_n_nodes = 0;

    AnCode C;
    int r = an_decode(code, code_len, &C);
    if (r < 0) return r;
    if (p->seat >= C.hdr.n) return -ANALYSE_EBADARG;

    // Pass 1: the scan.
    AnScanCtx sc = { p, e->strat, 0 };
    r = an_walk(&C, p->seat, an_scan_visit, &sc);
    if (r < 0) return r;
    // g_real is now the finished (or cut) game: the deal report reads it.
    const Game *fin = &g_real;
    int fool = game_done(fin);

    // Pass 2: the deep pass on the K largest losses.
    if (de && p->deep_nodes > 0) {
        AnDeepCtx dc; dc.p = p; dc.strat = de->strat; dc.n = 0;
        bool used[AN_MAX_NODES]; memset(used, 0, sizeof used);
        for (int k = 0; k < p->deep_nodes; k++) {
            int best = -1; double bl = 0.0;
            for (int i = 0; i < g_n_nodes; i++) {
                const AnNode *n = &g_nodes[i];
                if (used[i] || n->verdict == ANALYSE_V_FORCED || n->verdict == ANALYSE_V_LOST) continue;
                if (n->flags & (ANALYSE_NF_PROOF | ANALYSE_NF_BELIEF_FAIL)) continue;
                if (n->loss > bl) { bl = n->loss; best = i; }
            }
            if (best < 0) break;
            used[best] = true;
            dc.idx[dc.n++] = best;
        }
        if (dc.n > 0) {
            r = an_walk(&C, p->seat, an_deep_visit, &dc);
            if (r < 0) return r;
        }
    }

    int decisive = an_decisive(fool);
    if (decisive >= 0) g_nodes[decisive].verdict = ANALYSE_V_DECISIVE;

    // The deal report: opening trumps per seat, and every trump that entered
    // each hand through the deal or a draw. The rebuilt game's log carries the
    // real cards, so this is a count, not an estimate.
    int np = fin->num_players;
    int deck_size = NUM_SUITS * (ACE_VALUE - min_value_for(np) + 1);
    int trumps_in_deck = ACE_VALUE - min_value_for(np) + 1;
    int opening[MAX_PLAYERS] = { 0 }, seen[MAX_PLAYERS] = { 0 };
    {
        Game *d = &an_world_g;
        r = replay_deal_start(d, &C.hdr, C.deck, C.n_deck);
        if (r < 0) { g_replay_err = r; return -ANALYSE_EREPLAY; }
        for (int s = 0; s < np; s++)
            for (int j = 0; j < d->players[s].hand_count; j++)
                if (d->players[s].hand[j].suit == d->power_suit) { opening[s]++; seen[s]++; }
        for (int i = 0; i < fin->num_logs; i++) {
            const GameLog *L = &fin->logs[i];
            if (L->log_type != LOG_DRAW || L->player_idx < 0) continue;
            for (int k = 0; k < L->num_pairs; k++)
                if (L->pairs[k].primary.suit == fin->power_suit) seen[L->player_idx]++;
        }
    }

    // Header, then the nodes by re-walking for the candidate descriptors.
    unsigned char *q = out;
    const unsigned char *end = out + out_cap;
    uint8_t hflags = 0;
    if (p->seat < 0) hflags |= ANALYSE_HF_ALL_SEATS;
    for (int i = 0; i < g_n_nodes; i++) if (g_nodes[i].flags & ANALYSE_NF_BELIEF_FAIL) hflags |= ANALYSE_HF_BELIEF_FAIL;
    uint32_t elapsed = an_now_ms() - t0;
    int ok = an_put8(&q, end, ANALYSE_WIRE_VERSION) && an_put8(&q, end, (unsigned)np)
          && an_put8(&q, end, p->seat < 0 ? 0xFFu : (unsigned)p->seat)
          && an_put8(&q, end, (unsigned)fin->power_suit)
          && an_put8(&q, end, fool < 0 ? 0xFFu : (unsigned)fool)
          && an_put8(&q, end, (unsigned)p->roster_idx)
          && an_put8(&q, end, de ? (unsigned)p->deep_roster_idx : 0xFFu)
          && an_put8(&q, end, hflags)
          && an_put16(&q, end, (unsigned)g_n_nodes)
          && an_put16(&q, end, decisive < 0 ? 0xFFFFu : (unsigned)decisive)
          && an_put32(&q, end, g_playouts) && an_put32(&q, end, g_solves) && an_put32(&q, end, elapsed);
    for (int s = 0; ok && s < np; s++) {
        ok = an_put8(&q, end, (unsigned)opening[s])
          && an_put16(&q, end, an_prob16(analyse_hypergeom(deck_size, trumps_in_deck, opening[s])))
          && an_put16(&q, end, an_prob16(analyse_hypergeom_at_most(deck_size, trumps_in_deck, opening[s])))
          && an_put8(&q, end, (unsigned)(seen[s] > 255 ? 255 : seen[s]));
    }
    if (!ok) return -ANALYSE_ECAP;

    AnWriteCtx wc = { q, end, 0, 0 };
    r = an_walk(&C, p->seat, an_write_visit, &wc);
    if (r < 0) return r;
    if (wc.err) return wc.err;
    if (wc.node_i != g_n_nodes) return -ANALYSE_EMENU;   // the re-walk disagreed with the scan   // the re-walk disagreed with the scan
    return (int)(wc.q - out);
}

// ---------- the reader ---------------------------------------------------------------------

typedef struct { const unsigned char *p, *end; int bad; } AnRd;
static unsigned rd8(AnRd *r)  { if (r->p + 1 > r->end) { r->bad = 1; return 0; } return *r->p++; }
static unsigned rd16(AnRd *r) { if (r->p + 2 > r->end) { r->bad = 1; return 0; } unsigned v = r->p[0] | (r->p[1] << 8); r->p += 2; return v; }
static uint32_t rd32(AnRd *r) { if (r->p + 4 > r->end) { r->bad = 1; return 0; } uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)r->p[i] << (8 * i); r->p += 4; return v; }

int analyse_read_header(const unsigned char *buf, int len, AnalyseHeader *h) {
    if (!buf || !h || len < 0) return -ANALYSE_EBADARG;
    AnRd r = { buf, buf + len, 0 };
    memset(h, 0, sizeof *h);
    h->version = (uint8_t)rd8(&r);
    h->n_players = (uint8_t)rd8(&r);
    h->seat = (uint8_t)rd8(&r);
    h->trump_suit = (uint8_t)rd8(&r);
    h->fool = (uint8_t)rd8(&r);
    h->roster_idx = (uint8_t)rd8(&r);
    h->deep_roster_idx = (uint8_t)rd8(&r);
    h->flags = (uint8_t)rd8(&r);
    h->n_nodes = (uint16_t)rd16(&r);
    h->decisive_node = (uint16_t)rd16(&r);
    h->n_playouts = rd32(&r);
    h->n_solves = rd32(&r);
    h->elapsed_ms = rd32(&r);
    if (r.bad || h->version != ANALYSE_WIRE_VERSION || h->n_players > MAX_PLAYERS) return -ANALYSE_ETRUNC;
    for (int s = 0; s < h->n_players; s++) {
        h->deal[s].opening_trumps = (uint8_t)rd8(&r);
        h->deal[s].p_exact = (uint16_t)rd16(&r);
        h->deal[s].p_at_most = (uint16_t)rd16(&r);
        h->deal[s].trumps_seen = (uint8_t)rd8(&r);
    }
    if (r.bad) return -ANALYSE_ETRUNC;
    return (int)(r.p - buf);
}

int analyse_read_node(const unsigned char *buf, int len, AnalyseNode *n) {
    if (!buf || !n || len < 0) return -ANALYSE_EBADARG;
    AnRd r = { buf, buf + len, 0 };
    memset(n, 0, sizeof *n);
    n->step = (uint16_t)rd16(&r);
    n->seat = (uint8_t)rd8(&r);
    n->verdict = (uint8_t)rd8(&r);
    n->flags = (uint8_t)rd8(&r);
    n->n_cands = (uint8_t)rd8(&r);
    n->played = (uint8_t)rd8(&r);
    n->best = (uint8_t)rd8(&r);
    n->n_worlds = (uint16_t)rd16(&r);
    n->unknown = (uint8_t)rd8(&r);
    n->deck = (uint8_t)rd8(&r);
    n->win_prob = (uint16_t)rd16(&r);
    n->loss = (int16_t)rd16(&r);
    n->loss_se = (int16_t)rd16(&r);
    if (r.bad) return -ANALYSE_ETRUNC;
    if (n->played >= n->n_cands || n->best >= n->n_cands) return -ANALYSE_ETRUNC;
    for (int ci = 0; ci < n->n_cands; ci++) {
        AnalyseCand *c = &n->cands[ci];
        c->type = (uint8_t)rd8(&r);
        c->n_cards = (uint8_t)rd8(&r);
        if (r.bad || c->n_cards > MAX_MOVE_CARDS) return -ANALYSE_ETRUNC;
        for (int k = 0; k < c->n_cards; k++) c->cards[k] = (uint8_t)rd8(&r);
        for (int k = 0; k < c->n_cards; k++) c->targets[k] = (uint8_t)rd8(&r);
        c->n = (uint16_t)rd16(&r);
        c->n_fool = (uint16_t)rd16(&r);
        c->mean_fp = (int16_t)rd16(&r);
        c->paired_diff = (int16_t)rd16(&r);
        c->paired_se = (int16_t)rd16(&r);
        c->proof = (int8_t)rd8(&r);
        c->proven_wins = (uint16_t)rd16(&r);
        c->proven_losses = (uint16_t)rd16(&r);
        if (r.bad) return -ANALYSE_ETRUNC;
    }
    if (n->flags & ANALYSE_NF_DEEP) {
        n->deep_n_worlds = (uint16_t)rd16(&r);
        n->deep_best = (uint8_t)rd8(&r);
        n->deep_loss = (int16_t)rd16(&r);
        n->deep_loss_se = (int16_t)rd16(&r);
        for (int ci = 0; ci < n->n_cands; ci++) {
            AnalyseCand *c = &n->cands[ci];
            c->deep_n = (uint16_t)rd16(&r);
            c->deep_n_fool = (uint16_t)rd16(&r);
            c->deep_mean_fp = (int16_t)rd16(&r);
            c->deep_paired_diff = (int16_t)rd16(&r);
            c->deep_paired_se = (int16_t)rd16(&r);
        }
        if (r.bad || n->deep_best >= n->n_cands) return -ANALYSE_ETRUNC;
    }
    return (int)(r.p - buf);
}

// cnitro_winprob: the win-probability strip of a finished game.
//
//   cnitro_winprob --code=<base32 v6 code or a foolish.cards link>
//                  [--engine=robusta] [--worlds=200] [--belief-worlds=48]
//                  [--view=both|truth|belief] [--threads=N] [--seed=1]
//                  [--tsv=<file>]
//
// The post-game analyser (docs/POST_GAME_ANALYSER.md) answers "was THIS move a
// mistake" and carries a running win probability as a by-product of the
// analysed seat's own column. This asks the other question - "who was winning,
// and when" - for EVERY seat at EVERY step of the game, which is a different
// shape of work: one board per step rather than one per decision, and every
// seat measured at once.
//
// It reuses the analyser's pieces rather than a second copy of them: the real
// rebuild (replay_deal_start / replay_action_apply), analyse_belief, its world
// sampler and installer, and analyse_playout_board - the one "play this board
// out with real bots" there is.
//
// TWO VIEWS, because "chance of winning" is two different questions:
//
//   truth   The position as it really was. The rebuilt board already holds
//           every hidden hand and the real remaining stock, so a playout from
//           it is the true position handed to bots. One playout scores every
//           seat (exactly one of them is the fool), so the seats' fool
//           probabilities sum to 1 at every step. This is the eval bar: an
//           objective observer who can see everything.
//
//   belief  What each seat could honestly know at that moment: its own hand,
//           the table, the discard, the flip and the cards it watched others
//           pick up (analyse_belief - public information only, no inference
//           from behaviour). Worlds are sampled from that and played out, so
//           each curve is that seat's own equity from inside its own
//           information set. These do NOT sum to anything: eight seats holding
//           eight different pictures of the same board is the point.
//
// Both are frequencies against THIS engine's play, not proofs, and the
// document's "what it cannot see" applies here in full: the seats were played
// by people and the playouts are bots.
//
// The result is TSV on stdout (a plotter's input; a human summary goes to
// stderr), because this is a dev tool and nothing ships it.

#include "../src/analyse.h"
#include "../src/bot_roster.h"
#include "../src/cli_util.h"
#include "../src/game.h"
#include "../src/replay.h"
#include "../src/replay_extras.h"
#include "../src/replay_steps.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WP_MAX_POS     1024
#define WP_MAX_TASKS   8192
#define WP_MAX_THREADS 64
#define WP_THREAD_STACK (8u << 20)

static const char *SUIT = "SHCD";
static const char *VAL[14] = { "?", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A" };

static void card_str(char *b, size_t n, Card c, int trump) {
    if (c.suit < 0 || c.value <= 0) { snprintf(b, n, "??"); return; }
    snprintf(b, n, "%s%c%s", VAL[c.value], SUIT[c.suit], c.suit == trump ? "*" : "");
}

static uint32_t wp_mix(uint32_t a, uint32_t b) {
    uint32_t h = a ^ 0x9E3779B9u;
    h ^= b + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h *= 0x85EBCA77u; h ^= h >> 13; h *= 0xC2B2AE3Du; h ^= h >> 16;
    return h ? h : 1u;
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ---------- one position's work ---------------------------------------------
//
// A task is one playout: a world (or the truth), played to the end. Every task
// is a pure function of its seed and writes only its own row, so the tasks of a
// position fan out over threads and the result does not depend on the count.

typedef struct {
    int8_t   seat;        // -1 = the truth, >= 0 = that seat's belief
    uint32_t seed;
} WpTask;

typedef struct {
    const Game        *board;
    const AnalyseBelief *bel;   // [MAX_PLAYERS], only read for seat >= 0 tasks
    const WpTask      *tasks;
    int                n_tasks;
    int                strat;
    int8_t           (*fp)[MAX_PLAYERS];   // [n_tasks][MAX_PLAYERS], 0 = stalled
    int                tid, nthreads;
} WpWork;

static _Thread_local Game wp_world;

static void wp_task_run(WpWork *W, int t) {
    const WpTask *task = &W->tasks[t];
    Game *wg = &wp_world;
    game_clone(wg, W->board);
    if (task->seat >= 0) {
        Card perm[MAX_DECK];
        analyse_sample_world(&W->bel[task->seat], task->seed, perm);
        analyse_install_world(wg, task->seat, &W->bel[task->seat], perm);
    }
    game_rng_set(task->seed);
    random_strategy_set_seed(wp_mix(task->seed, 0xA5A5A5A5u));
    if (!analyse_playout_board(wg, W->strat)) {
        memset(W->fp[t], 0, MAX_PLAYERS);
        return;
    }
    for (int p = 0; p < W->board->num_players; p++)
        W->fp[t][p] = (int8_t)analyse_finish_of(wg, p);
}

static void *wp_worker(void *arg) {
    WpWork *W = (WpWork *)arg;
    for (int t = W->tid; t < W->n_tasks; t += W->nthreads) wp_task_run(W, t);
    return 0;
}

static void wp_run(WpWork *W, int nthreads) {
    if (nthreads < 1) nthreads = 1;
    if (nthreads > WP_MAX_THREADS) nthreads = WP_MAX_THREADS;
    if (nthreads > W->n_tasks) nthreads = W->n_tasks;
    if (nthreads < 1) return;
    W->tid = 0; W->nthreads = nthreads;
    WpWork    part[WP_MAX_THREADS];
    pthread_t tids[WP_MAX_THREADS];
    int spawned = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WP_THREAD_STACK);
    for (int t = 1; t < nthreads; t++) {
        part[t] = *W;
        part[t].tid = t;
        if (pthread_create(&tids[t], &attr, wp_worker, &part[t]) != 0) break;
        spawned = t;
    }
    pthread_attr_destroy(&attr);
    // Whatever could not be spawned, this thread does itself.
    if (spawned + 1 < nthreads) {
        WpWork rest = *W;
        for (int t = spawned + 1; t < nthreads; t++) { rest.tid = t; wp_worker(&rest); }
    }
    wp_worker(W);
    for (int t = 1; t <= spawned; t++) pthread_join(tids[t], 0);
}

// ---------- the walk ---------------------------------------------------------

// One measured position: the board after `acts_done` recorded actions.
typedef struct {
    int  step;                 // the action index this position follows (-1 = the deal)
    int  seat;                 // who played that action (-1 = the deal)
    char move[64];             // what they played
    int  deck;                 // stock cards left
    int  hand[MAX_PLAYERS];
    int  in[MAX_PLAYERS];
    // per seat, per view: fool probability x10000, mean finish position x1000
    int  truth_fool[MAX_PLAYERS], truth_mean[MAX_PLAYERS];
    int  bel_fool[MAX_PLAYERS],   bel_mean[MAX_PLAYERS], bel_n[MAX_PLAYERS];
    int  bel_fail[MAX_PLAYERS];   // the belief broke conservation: no curve here
    int  n_truth;
} WpPos;

static WpPos       g_pos[WP_MAX_POS];
static int         g_n_pos;
static WpTask      g_tasks[WP_MAX_TASKS];
static int8_t      g_fp[WP_MAX_TASKS][MAX_PLAYERS];
static AnalyseBelief g_bel[MAX_PLAYERS];
static ReplayAction g_acts[REPLAY_MAX_ACTIONS];
static Game        g_real;
static uint32_t    g_playouts;

typedef struct {
    int strat;                 // STRAT_* brain id, at every seat of a playout
    const char *engine;        // its roster key, for the header
    int worlds, bworlds, threads, do_truth, do_belief;
    uint32_t seed;
} WpParams;

static void wp_measure(const Game *g, WpPos *P, const WpParams *p, int pos_idx) {
    int np = g->num_players;
    P->deck = g->deck_count;
    for (int s = 0; s < np; s++) {
        P->hand[s] = g->players[s].hand_count;
        P->in[s] = g->players[s].status == PLAYER_STATUS_IN;
    }
    if (game_done(g) >= 0) return;   // a finished board has nothing left to play out

    int n = 0;
    if (p->do_truth)
        for (int w = 0; w < p->worlds && n < WP_MAX_TASKS; w++)
            g_tasks[n++] = (WpTask){ -1, wp_mix(wp_mix(p->seed, (uint32_t)pos_idx), (uint32_t)w) };
    if (p->do_belief) {
        for (int s = 0; s < np; s++) {
            if (!P->in[s]) continue;
            analyse_belief(g, s, &g_bel[s]);
            // |U| == d + sum(f_p) or the sampler is silently wrong; the
            // analyser refuses a verdict there and so does this.
            P->bel_fail[s] = !g_bel[s].ok;
            if (!g_bel[s].ok) continue;
            for (int w = 0; w < p->bworlds && n < WP_MAX_TASKS; w++)
                g_tasks[n++] = (WpTask){ (int8_t)s,
                    wp_mix(wp_mix(p->seed, (uint32_t)(pos_idx * 64 + s + 1)), (uint32_t)w) };
        }
    }
    if (!n) return;

    WpWork W = { g, g_bel, g_tasks, n, p->strat, g_fp, 0, 1 };
    wp_run(&W, p->threads);
    g_playouts += (uint32_t)n;

    // Fold. A truth task scores every seat; a belief task scores only its own.
    long tsum[MAX_PLAYERS] = { 0 }, tfool[MAX_PLAYERS] = { 0 };
    long bsum[MAX_PLAYERS] = { 0 }, bfool[MAX_PLAYERS] = { 0 }, bn[MAX_PLAYERS] = { 0 };
    int tn = 0;
    for (int t = 0; t < n; t++) {
        // A finished playout gives every seat a position in 1..N, so a zero
        // in seat 0 is the stall marker wp_task_run wrote.
        if (!g_fp[t][0]) continue;
        int seat = g_tasks[t].seat;
        if (seat < 0) {
            tn++;
            for (int s = 0; s < np; s++) {
                tsum[s] += g_fp[t][s];
                if (g_fp[t][s] == np) tfool[s]++;
            }
        } else {
            bn[seat]++;
            bsum[seat] += g_fp[t][seat];
            if (g_fp[t][seat] == np) bfool[seat]++;
        }
    }
    P->n_truth = tn;
    for (int s = 0; s < np; s++) {
        if (tn) {
            P->truth_fool[s] = (int)((tfool[s] * 10000 + tn / 2) / tn);
            P->truth_mean[s] = (int)((tsum[s] * 1000 + tn / 2) / tn);
        }
        if (bn[s]) {
            P->bel_n[s] = (int)bn[s];
            P->bel_fool[s] = (int)((bfool[s] * 10000 + bn[s] / 2) / bn[s]);
            P->bel_mean[s] = (int)((bsum[s] * 1000 + bn[s] / 2) / bn[s]);
        } else {
            P->bel_fool[s] = -1;
            P->bel_mean[s] = -1;
        }
    }
}

static void wp_record(const Game *g, int step, int seat, const char *move,
                      const WpParams *p) {
    if (g_n_pos >= WP_MAX_POS) return;
    WpPos *P = &g_pos[g_n_pos];
    memset(P, 0, sizeof *P);
    P->step = step;
    P->seat = seat;
    snprintf(P->move, sizeof P->move, "%s", move);
    for (int s = 0; s < MAX_PLAYERS; s++) { P->bel_fool[s] = -1; P->bel_mean[s] = -1; }
    wp_measure(g, P, p, g_n_pos);
    g_n_pos++;
    fprintf(stderr, "  step %3d %-22s %5u playouts\r", step, move, g_playouts);
    fflush(stderr);
}

static void action_str(char *out, size_t n, const ReplayAction *a, int trump) {
    char cb[8], tb[8];
    const char *kind = "?";
    switch (a->kind) {
        case REPLAY_ATOM_ATTACK: kind = "attack"; break;
        case REPLAY_ATOM_COVER:  kind = "cover";  break;
        case REPLAY_ATOM_PASS:   kind = "pass";   break;
        case REPLAY_ATOM_PICKUP: kind = "pickup"; break;
        case REPLAY_ATOM_GOOD:   kind = "good";   break;
        case REPLAY_ATOM_ROUND_END: kind = "round end"; break;
        default: break;
    }
    snprintf(out, n, "%s", kind);
    for (int k = 0; k < a->n_cards; k++) {
        card_str(cb, sizeof cb, a->cards[k], trump);
        size_t l = strlen(out);
        if (a->kind == REPLAY_ATOM_COVER) {
            card_str(tb, sizeof tb, a->target, trump);
            snprintf(out + l, n - l, " %s>%s", cb, tb);
        } else {
            snprintf(out + l, n - l, " %s", cb);
        }
    }
}

// The rebuild, step by step, measuring the board between every pair of actions.
// The ROUND_END branch is the analyser's (an_walk): every attacker still to
// declare says good in seat order and the transition follows, because ROUND_END
// is not a move and replay_action_apply does not own it.
static int wp_walk(const ReplayHeader *hdr, const Card *deck, int n_deck,
                   int n_acts, int trump, const WpParams *p) {
    int r = replay_deal_start(&g_real, hdr, deck, n_deck);
    if (r < 0) return r;
    wp_record(&g_real, -1, -1, "deal", p);
    char mv[64];
    for (int i = 0; i < n_acts; i++) {
        const ReplayAction *a = &g_acts[i];
        if (a->kind == REPLAY_ATOM_ROUND_END) {
            int who[MAX_PLAYERS], n_who = 0;
            for (int s = 0; s < g_real.num_players; s++) {
                if (s == g_real.defender || g_real.players[s].status != PLAYER_STATUS_IN) continue;
                if (should_bot_act(&g_real, s)) who[n_who++] = s;
            }
            for (int k = 0; k < n_who; k++) handle_good(&g_real, who[k]);
            if (g_real.num_battles > 0) engine_run_round_transition(&g_real);
        } else {
            replay_action_apply(&g_real, a);
        }
        action_str(mv, sizeof mv, a, trump);
        wp_record(&g_real, i, a->seat, mv, p);
    }
    return 0;
}

// ---------- names ------------------------------------------------------------

static char g_names[MAX_PLAYERS][ROSTER_NAME_MAX + 1];

static void load_names(const char *url, int n_players, int moves) {
    for (int s = 0; s < n_players; s++) snprintf(g_names[s], sizeof g_names[s], "P%d", s + 1);
    const char *dash = strchr(url, '-');
    if (!dash || !dash[1]) return;
    static unsigned char blob[4096], packed[8192];
    int blob_len = replay_b32_decode(dash + 1, blob, (int)sizeof blob);
    if (blob_len <= 0) return;
    int n = replay_extras_decode(blob, blob_len, n_players, moves, packed, (int)sizeof packed);
    if (n < 0) { fprintf(stderr, "extras: decode failed (%d), seats stay P1..PN\n", -n); return; }
    ReplayExtras x;
    if (replay_extras_unpack(packed, n, &x) < 0) return;
    for (int s = 0; s < n_players && s < x.n_names; s++) {
        int len = x.names[s].len;
        if (len <= 0) continue;
        if (len > ROSTER_NAME_MAX) len = ROSTER_NAME_MAX;
        memcpy(g_names[s], x.names[s].text, (size_t)len);
        g_names[s][len] = 0;
    }
}

// ---------- output -----------------------------------------------------------

static void emit(FILE *f, const ReplaySummary *sum, const WpParams *p, int elapsed) {
    int np = sum->num_players;
    int trump = sum->trump.suit;
    fprintf(f, "# cnitro_winprob\n");
    fprintf(f, "players\t%d\n", np);
    fprintf(f, "trump\t%c\n", SUIT[trump]);
    fprintf(f, "fool\t%d\n", sum->fool);
    fprintf(f, "engine\t%s\n", p->engine);
    fprintf(f, "worlds\t%d\t%d\n", p->worlds, p->bworlds);
    fprintf(f, "playouts\t%u\n", g_playouts);
    fprintf(f, "elapsed_ms\t%d\n", elapsed);
    for (int s = 0; s < np; s++) fprintf(f, "name\t%d\t%s\n", s, g_names[s]);
    for (int i = 0; i < sum->num_eliminated; i++) fprintf(f, "out\t%d\t%d\n", i + 1, sum->elimination[i]);
    fprintf(f, "# pos\tidx\tstep\tseat\tdeck\tmove\thands...\n");
    fprintf(f, "# wp\tidx\tview\tseat\tp_fool_x10000\tmean_fp_x1000\tn\n");
    for (int i = 0; i < g_n_pos; i++) {
        const WpPos *P = &g_pos[i];
        fprintf(f, "pos\t%d\t%d\t%d\t%d\t%s", i, P->step, P->seat, P->deck, P->move);
        for (int s = 0; s < np; s++) fprintf(f, "\t%d", P->in[s] ? P->hand[s] : -1);
        fprintf(f, "\n");
        for (int s = 0; s < np; s++) {
            if (P->n_truth) fprintf(f, "wp\t%d\ttruth\t%d\t%d\t%d\t%d\n",
                                    i, s, P->truth_fool[s], P->truth_mean[s], P->n_truth);
            if (P->bel_fool[s] >= 0) fprintf(f, "wp\t%d\tbelief\t%d\t%d\t%d\t%d\n",
                                             i, s, P->bel_fool[s], P->bel_mean[s], P->bel_n[s]);
            if (P->bel_fail[s]) fprintf(f, "belieffail\t%d\t%d\n", i, s);
        }
    }
    (void)trump;
}

static void usage(void) {
    fprintf(stderr,
        "usage: cnitro_winprob --code=<base32 v6 code or foolish.cards link>\n"
        "         [--engine=robusta] [--worlds=200] [--belief-worlds=48]\n"
        "         [--view=both|truth|belief] [--threads=N] [--seed=1] [--tsv=<file>]\n");
}

int main(int argc, char **argv) {
    const char *url = get_arg(argc, argv, "code", 0);
    if (!url) { usage(); return 2; }

    WpParams p = { 0 };
    const char *eng = get_arg(argc, argv, "engine", "robusta");
    int roster_idx = bot_roster_find(eng);
    const BotRosterEntry *entry = bot_roster_at(roster_idx);
    if (!entry) { fprintf(stderr, "unknown engine '%s'\n", eng); return 2; }
    // The seat carries a STRAT_* brain id, not the roster index - the same
    // thing analyse_packed hands its playouts (e->strat).
    p.strat = entry->strat;
    p.engine = entry->key;
    p.worlds  = parse_int(get_arg(argc, argv, "worlds", 0), 200);
    p.bworlds = parse_int(get_arg(argc, argv, "belief-worlds", 0), 48);
    p.threads = parse_int(get_arg(argc, argv, "threads", 0), 1);
    p.seed    = (uint32_t)parse_int(get_arg(argc, argv, "seed", 0), 1);
    const char *view = get_arg(argc, argv, "view", "both");
    p.do_truth  = strcmp(view, "belief") != 0;
    p.do_belief = strcmp(view, "truth") != 0;
    const char *tsv = get_arg(argc, argv, "tsv", 0);

    static char code_s[REPLAY_MAX_INT_BYTES * 2];
    int cl = replay_link_parse(url, code_s, (int)sizeof code_s);
    if (cl <= 0) { fprintf(stderr, "not a replay link or code\n"); return 2; }

    static unsigned char code[REPLAY_MAX_INT_BYTES];
    int code_len = replay_b32_decode(code_s, code, (int)sizeof code);
    if (code_len <= 0) { fprintf(stderr, "bad code\n"); return 2; }

    ReplaySummary sum;
    int r = replay_summary_v6(code, code_len, &sum);
    if (r < 0) { fprintf(stderr, "summary failed (%d)\n", -r); return 1; }

    ReplayHeader hdr;
    static Card deck[MAX_DECK];
    int n_deck = 0, n_acts = 0;
    r = replay_deal_v6(code, code_len, &hdr, deck, MAX_DECK, &n_deck,
                       g_acts, REPLAY_MAX_ACTIONS, &n_acts);
    if (r < 0) { fprintf(stderr, "decode failed (%d)\n", -r); return 1; }

    load_names(url, sum.num_players, sum.moves);

    fprintf(stderr, "%d seats, trump %c, %d actions, engine %s, %d truth + %d belief worlds per step\n",
            sum.num_players, SUIT[sum.trump.suit], n_acts, p.engine,
            p.do_truth ? p.worlds : 0, p.do_belief ? p.bworlds : 0);

    uint32_t t0 = now_ms();
    r = wp_walk(&hdr, deck, n_deck, n_acts, sum.trump.suit, &p);
    if (r < 0) { fprintf(stderr, "\nrebuild failed (%d)\n", -r); return 1; }
    int elapsed = (int)(now_ms() - t0);
    fprintf(stderr, "\n%d positions, %u playouts, %d ms\n", g_n_pos, g_playouts, elapsed);

    // The invariant that says the fold is right: one playout has exactly one
    // fool, so a position's truth probabilities sum to 1, up to the rounding
    // of each seat's own count to four places (at most N/2 units of 1e-4, and
    // none at all when the world count divides 10000). A sum that drifts
    // further means the seats were scored from different playout sets.
    if (p.do_truth) {
        int worst = 0;
        for (int i = 0; i < g_n_pos; i++) {
            if (!g_pos[i].n_truth) continue;
            int total = 0;
            for (int s = 0; s < sum.num_players; s++) total += g_pos[i].truth_fool[s];
            if (abs(total - 10000) > abs(worst)) worst = total - 10000;
        }
        fprintf(stderr, "truth check: worst position sums to %.4f (1 +- %d/10000 is right)\n",
                1.0 + worst / 10000.0, sum.num_players / 2);
    }

    FILE *f = stdout;
    if (tsv) {
        f = fopen(tsv, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", tsv); return 1; }
    }
    emit(f, &sum, &p, elapsed);
    if (tsv) fclose(f);
    return 0;
}

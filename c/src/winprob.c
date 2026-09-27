// See winprob.h. The shape:
//
//   walk      rebuild the game from the code and stop at every step
//   measure   the truth playouts and each seat's belief worlds, folded
//   write     the packed result, and the reader beside it
//
// Everything expensive is one playout, and every playout is independent of
// every other, so a step's playouts fan out over pthreads and the result does
// not depend on the thread count. All scratch is thread-local.

#include "winprob.h"

#include "analyse.h"
#include "bot_roster.h"
#include "replay.h"
#include "replay_extras.h"
#include "replay_steps.h"

#include <string.h>
#ifndef __wasm__
#include <pthread.h>
#include <time.h>
#endif

// One step per recorded action, plus the opening position. Sized off the
// decoder's own cap so a code that decodes can always be measured: a silent
// truncation here would be a strip that stops mid-game and says nothing.
#define WP_MAX_STEPS   (REPLAY_MAX_ACTIONS + 1)
#define WP_MAX_TASKS   8192
#define WP_MAX_THREADS 64
#define WP_THREAD_STACK (8u << 20)

// ---------- small helpers ---------------------------------------------------

static uint32_t wp_mix(uint32_t a, uint32_t b) {
    uint32_t h = a ^ 0x9E3779B9u;
    h ^= b + 0x7F4A7C15u + (h << 6) + (h >> 2);
    h *= 0x85EBCA77u; h ^= h >> 13; h *= 0xC2B2AE3Du; h ^= h >> 16;
    return h ? h : 1u;
}

static uint32_t wp_now_ms(void) {
#ifdef __wasm__
    return 0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static unsigned wp_card_id(Card c) {
    if (c.suit < 0 || c.value <= 0) return 0xFFu;
    return (unsigned)(c.suit * 13 + c.value - 1);
}

// ---------- one step's playouts ---------------------------------------------
//
// A task is one playout: a world (or the truth), played to the end. Every task
// is a pure function of its seed and writes only its own row.

typedef struct {
    int8_t   seat;        // -1 = the truth, >= 0 = that seat's belief
    uint32_t seed;
} WpTask;

typedef struct {
    const Game          *board;
    const AnalyseBelief *bel;    // [MAX_PLAYERS], read only for seat >= 0 tasks
    const WpTask        *tasks;
    int                  n_tasks;
    int                  strat;  // STRAT_* brain id, at every seat
    int8_t             (*fp)[MAX_PLAYERS];   // [n_tasks][MAX_PLAYERS], 0 = stalled
    int                  tid, nthreads;
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
#ifndef __wasm__
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
    // Whatever could not be spawned, this thread does itself, by widening its
    // own stride to cover the missing workers' tasks.
    if (spawned + 1 < nthreads) {
        WpWork rest = *W;
        for (int t = spawned + 1; t < nthreads; t++) { rest.tid = t; wp_worker(&rest); }
    }
    wp_worker(W);
    for (int t = 1; t <= spawned; t++) pthread_join(tids[t], 0);
#else
    W->nthreads = 1;
    wp_worker(W);
#endif
}

// ---------- the measured step -----------------------------------------------

typedef struct {
    uint16_t move;
    uint8_t  seat, kind, n_cards, target, deck;
    uint8_t  cards[WINPROB_MOVE_CARDS];
    uint16_t n_truth, n_belief;
    uint16_t truth_fool[MAX_PLAYERS], truth_mean[MAX_PLAYERS];
    uint16_t belief_fool[MAX_PLAYERS], belief_mean[MAX_PLAYERS];
    uint8_t  hand[MAX_PLAYERS];
} WpStep;

static WpStep       g_steps[WP_MAX_STEPS];
static int          g_n_steps;
static WpTask       g_tasks[WP_MAX_TASKS];
static int8_t       g_fp[WP_MAX_TASKS][MAX_PLAYERS];
static AnalyseBelief g_bel[MAX_PLAYERS];
static ReplayAction g_acts[REPLAY_MAX_ACTIONS];
static Game         g_real;
static uint32_t     g_playouts;
static int          g_belief_fail;
static int          g_replay_err;

int winprob_last_replay_error(void) { return g_replay_err; }

static uint16_t wp_prob(long hits, long n) {
    return (uint16_t)((hits * 10000 + n / 2) / n);
}

static void wp_measure(const Game *g, WpStep *S, const WinprobParams *p, int idx) {
    int np = g->num_players;
    for (int s = 0; s < np; s++) {
        // hand_count is an int8_t, so it can never reach the 0xFF that means
        // "this seat is out".
        int hc = g->players[s].hand_count;
        S->hand[s] = g->players[s].status == PLAYER_STATUS_IN
                   ? (uint8_t)(hc < 0 ? 0 : hc) : 0xFFu;
        S->truth_fool[s] = S->truth_mean[s] = WINPROB_NONE;
        S->belief_fool[s] = S->belief_mean[s] = WINPROB_NONE;
    }
    S->deck = (uint8_t)(g->deck_count > 255 ? 255 : g->deck_count);
    if (game_done(g) >= 0) return;   // a finished board has nothing left to play out

    int n = 0;
    for (int w = 0; w < p->worlds && n < WP_MAX_TASKS; w++)
        g_tasks[n++] = (WpTask){ -1, wp_mix(wp_mix(p->seed, (uint32_t)idx), (uint32_t)w) };
    for (int s = 0; s < np && p->belief_worlds > 0; s++) {
        if (S->hand[s] == 0xFF) continue;
        analyse_belief(g, s, &g_bel[s]);
        // |U| == d + sum(f_p) or the sampler is silently wrong. The analyser
        // refuses a verdict there and so does this: the seat carries NONE.
        if (!g_bel[s].ok) { g_belief_fail = 1; continue; }
        for (int w = 0; w < p->belief_worlds && n < WP_MAX_TASKS; w++)
            g_tasks[n++] = (WpTask){ (int8_t)s,
                wp_mix(wp_mix(p->seed, (uint32_t)(idx * (MAX_PLAYERS + 1) + s + 1)), (uint32_t)w) };
    }
    if (!n) return;

    WpWork W = { g, g_bel, g_tasks, n, p->roster_idx, g_fp, 0, 1 };
    wp_run(&W, p->threads);
    g_playouts += (uint32_t)n;

    // Fold. A truth task scores every seat; a belief task scores only its own.
    long tsum[MAX_PLAYERS] = { 0 }, tfool[MAX_PLAYERS] = { 0 };
    long bsum[MAX_PLAYERS] = { 0 }, bfool[MAX_PLAYERS] = { 0 }, bn[MAX_PLAYERS] = { 0 };
    long tn = 0;
    for (int t = 0; t < n; t++) {
        // A finished playout gives every seat a position in 1..N, so a zero in
        // seat 0 is the stall marker wp_task_run wrote.
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
    S->n_truth = (uint16_t)tn;
    for (int s = 0; s < np; s++) {
        if (tn) {
            S->truth_fool[s] = wp_prob(tfool[s], tn);
            S->truth_mean[s] = (uint16_t)((tsum[s] * 1000 + tn / 2) / tn);
        }
        if (bn[s]) {
            S->n_belief = (uint16_t)bn[s];
            S->belief_fool[s] = wp_prob(bfool[s], bn[s]);
            S->belief_mean[s] = (uint16_t)((bsum[s] * 1000 + bn[s] / 2) / bn[s]);
        }
    }
}

// ---------- the walk --------------------------------------------------------

static void wp_record(const Game *g, int move, const ReplayAction *a,
                      const WinprobParams *p) {
    if (g_n_steps >= WP_MAX_STEPS) return;   // cannot happen: see WP_MAX_STEPS
    WpStep *S = &g_steps[g_n_steps];
    memset(S, 0, sizeof *S);
    S->move = (uint16_t)move;
    S->seat = a ? (uint8_t)a->seat : 0xFFu;
    S->kind = a ? (uint8_t)a->kind : 0xFFu;
    S->target = a ? (uint8_t)wp_card_id(a->target) : 0xFFu;
    memset(S->cards, 0xFF, sizeof S->cards);
    if (a) {
        S->n_cards = (uint8_t)a->n_cards;
        for (int k = 0; k < a->n_cards && k < WINPROB_MOVE_CARDS; k++)
            S->cards[k] = (uint8_t)wp_card_id(a->cards[k]);
    }
    wp_measure(g, S, p, g_n_steps);
    g_n_steps++;
}

// The rebuild, step by step, measuring the board between every pair of actions.
// The ROUND_END branch is the analyser's (an_walk in analyse.c): every attacker
// still to declare says good in seat order and the transition follows, because
// ROUND_END is not a move and replay_action_apply does not own it.
static int wp_walk(const ReplayHeader *hdr, const Card *deck, int n_deck,
                   int n_acts, const WinprobParams *p,
                   WinprobProgress progress, void *pctx) {
    int r = replay_deal_start(&g_real, hdr, deck, n_deck);
    if (r < 0) { g_replay_err = r; return -WINPROB_EREPLAY; }
    wp_record(&g_real, 0xFFFF, 0, p);
    if (progress) progress(pctx, 0, n_acts + 1);
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
        wp_record(&g_real, i, a, p);
        if (progress) progress(pctx, i + 1, n_acts + 1);
    }
    return 0;
}

// ---------- the writer ------------------------------------------------------

static void wp_put8(unsigned char **q, unsigned v) { *(*q)++ = (unsigned char)v; }
static void wp_put16(unsigned char **q, unsigned v) {
    *(*q)++ = (unsigned char)(v & 0xFF);
    *(*q)++ = (unsigned char)((v >> 8) & 0xFF);
}
static void wp_put32(unsigned char **q, uint32_t v) {
    for (int i = 0; i < 4; i++) *(*q)++ = (unsigned char)((v >> (8 * i)) & 0xFF);
}

// The two strides, counted off the writer below: 8 u8 + 6 u16 + 2 u32 = 28
// fixed header bytes, then one elim byte and one name slot per seat; and
// 13 + 4 fixed step bytes, then four u16 and one u8 per seat.
static int wp_header_bytes(int n) { return 28 + n * (1 + 1 + WINPROB_NAME_MAX); }
static int wp_step_bytes(int n)   { return 17 + n * 9; }

int winprob_packed_bound(int n_players, int n_steps) {
    if (n_players < 2 || n_players > MAX_PLAYERS || n_steps < 0) return -WINPROB_EBADARG;
    return wp_header_bytes(n_players) + n_steps * wp_step_bytes(n_players);
}

void winprob_params_default(WinprobParams *p) {
    if (!p) return;
    p->roster_idx = bot_roster_find("robusta");
    p->worlds = 300;
    p->belief_worlds = 24;
    p->seed = 1;
    p->threads = 1;
    p->names_b32 = 0;
}

// The seat names, out of the extras half of the link (see WinprobParams).
static void wp_names(const char *extras_b32, int n_players, int move_count,
                     uint8_t *len_out, char (*text_out)[WINPROB_NAME_MAX]) {
    memset(len_out, 0, (size_t)n_players);
    memset(text_out, 0, (size_t)n_players * WINPROB_NAME_MAX);
    if (!extras_b32 || !*extras_b32) return;
    static unsigned char blob[4096], packed[8192];
    int blob_len = replay_b32_decode(extras_b32, blob, (int)sizeof blob);
    if (blob_len <= 0) return;
    int n = replay_extras_decode(blob, blob_len, n_players, move_count, packed, (int)sizeof packed);
    if (n < 0) return;
    ReplayExtras x;
    if (replay_extras_unpack(packed, n, &x) < 0) return;
    for (int s = 0; s < n_players && s < x.n_names; s++) {
        int l = x.names[s].len;
        if (l <= 0) continue;
        if (l > WINPROB_NAME_MAX) l = WINPROB_NAME_MAX;
        len_out[s] = (uint8_t)l;
        memcpy(text_out[s], x.names[s].text, (size_t)l);
    }
}

int winprob_packed(const unsigned char *code, int code_len, const WinprobParams *p,
                   WinprobProgress progress, void *progress_ctx,
                   unsigned char *out, int out_cap) {
    if (!code || !p || !out || code_len <= 0) return -WINPROB_EBADARG;
    const BotRosterEntry *e = bot_roster_at(p->roster_idx);
    if (!e) return -WINPROB_EBADARG;
    if (p->worlds < 0 || p->belief_worlds < 0) return -WINPROB_EBADARG;
    if (p->worlds == 0 && p->belief_worlds == 0) return -WINPROB_EBADARG;

    g_playouts = 0;
    g_n_steps = 0;
    g_belief_fail = 0;
    g_replay_err = 0;

    ReplaySummary sum;
    int r = replay_summary_v6(code, code_len, &sum);
    if (r < 0) { g_replay_err = r; return -WINPROB_EREPLAY; }

    ReplayHeader hdr;
    static Card deck[MAX_DECK];
    int n_deck = 0, n_acts = 0;
    r = replay_deal_v6(code, code_len, &hdr, deck, MAX_DECK, &n_deck,
                       g_acts, REPLAY_MAX_ACTIONS, &n_acts);
    if (r < 0) { g_replay_err = r; return -WINPROB_EREPLAY; }

    int np = sum.num_players;
    if (np < 2 || np > MAX_PLAYERS) return -WINPROB_EREPLAY;
    if (n_acts + 1 > WP_MAX_STEPS) return -WINPROB_ECAP;
    if (winprob_packed_bound(np, n_acts + 1) > out_cap) return -WINPROB_ECAP;

    // The roster's brain, not its index: a seat carries a STRAT_* id.
    WinprobParams run = *p;
    run.roster_idx = e->strat;

    uint32_t t0 = wp_now_ms();
    r = wp_walk(&hdr, deck, n_deck, n_acts, &run, progress, progress_ctx);
    if (r < 0) return r;
    uint32_t elapsed = wp_now_ms() - t0;

    unsigned char *q = out;
    wp_put8(&q, WINPROB_WIRE_VERSION);
    wp_put8(&q, (unsigned)np);
    wp_put8(&q, (unsigned)sum.version);
    wp_put8(&q, (unsigned)sum.trump.suit);
    wp_put8(&q, sum.fool < 0 ? 0xFFu : (unsigned)sum.fool);
    wp_put8(&q, (unsigned)p->roster_idx);
    wp_put8(&q, (unsigned)((p->worlds ? WINPROB_F_TRUTH : 0)
                         | (p->belief_worlds ? WINPROB_F_BELIEF : 0)
                         | (g_belief_fail ? WINPROB_F_BELIEF_FAIL : 0)));
    wp_put8(&q, 0);
    wp_put16(&q, (unsigned)g_n_steps);
    wp_put16(&q, (unsigned)wp_header_bytes(np));
    wp_put16(&q, (unsigned)wp_step_bytes(np));
    wp_put16(&q, (unsigned)p->worlds);
    wp_put16(&q, (unsigned)p->belief_worlds);
    wp_put16(&q, 0);
    wp_put32(&q, g_playouts);
    wp_put32(&q, elapsed);
    for (int i = 0; i < np; i++)
        wp_put8(&q, i < sum.num_eliminated ? (unsigned)sum.elimination[i] : 0xFFu);
    static uint8_t nlen[MAX_PLAYERS];
    static char    ntext[MAX_PLAYERS][WINPROB_NAME_MAX];
    wp_names(p->names_b32, np, sum.moves, nlen, ntext);
    for (int s = 0; s < np; s++) {
        wp_put8(&q, nlen[s]);
        for (int k = 0; k < WINPROB_NAME_MAX; k++) wp_put8(&q, (unsigned char)ntext[s][k]);
    }

    for (int i = 0; i < g_n_steps; i++) {
        const WpStep *S = &g_steps[i];
        wp_put16(&q, S->move);
        wp_put8(&q, S->seat);
        wp_put8(&q, S->kind);
        wp_put8(&q, S->n_cards);
        for (int k = 0; k < WINPROB_MOVE_CARDS; k++) wp_put8(&q, S->cards[k]);
        wp_put8(&q, S->target);
        wp_put8(&q, S->deck);
        wp_put16(&q, S->n_truth);
        wp_put16(&q, S->n_belief);
        for (int s = 0; s < np; s++) wp_put16(&q, S->truth_fool[s]);
        for (int s = 0; s < np; s++) wp_put16(&q, S->truth_mean[s]);
        for (int s = 0; s < np; s++) wp_put16(&q, S->belief_fool[s]);
        for (int s = 0; s < np; s++) wp_put16(&q, S->belief_mean[s]);
        for (int s = 0; s < np; s++) wp_put8(&q, S->hand[s]);
    }
    // The writer and the strides the reader seeks by are two statements of one
    // layout, and a silent disagreement between them misaligns every step
    // rather than failing. So they are held against each other here, on every
    // run, at the one moment the answer is known.
    if ((int)(q - out) != winprob_packed_bound(np, g_n_steps)) return -WINPROB_ELAYOUT;
    return (int)(q - out);
}

// ---------- the reader ------------------------------------------------------

static unsigned wp_rd8(const unsigned char **r) { return *(*r)++; }
static unsigned wp_rd16(const unsigned char **r) {
    unsigned v = (*r)[0] | ((unsigned)(*r)[1] << 8);
    *r += 2;
    return v;
}
static uint32_t wp_rd32(const unsigned char **r) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)(*r)[i] << (8 * i);
    *r += 4;
    return v;
}

int winprob_read_header(const unsigned char *buf, int len, WinprobHeader *h) {
    if (!buf || !h || len < 24) return -WINPROB_ETRUNC;
    const unsigned char *r = buf;
    h->version      = (uint8_t)wp_rd8(&r);
    h->n_players    = (uint8_t)wp_rd8(&r);
    h->code_version = (uint8_t)wp_rd8(&r);
    h->trump_suit   = (uint8_t)wp_rd8(&r);
    h->fool         = (uint8_t)wp_rd8(&r);
    h->roster_idx   = (uint8_t)wp_rd8(&r);
    h->flags        = (uint8_t)wp_rd8(&r);
    (void)wp_rd8(&r);
    h->n_steps      = (uint16_t)wp_rd16(&r);
    h->header_bytes = (uint16_t)wp_rd16(&r);
    h->step_bytes   = (uint16_t)wp_rd16(&r);
    h->worlds       = (uint16_t)wp_rd16(&r);
    h->belief_worlds= (uint16_t)wp_rd16(&r);
    (void)wp_rd16(&r);
    h->playouts     = wp_rd32(&r);
    h->elapsed_ms   = wp_rd32(&r);
    if (h->n_players < 2 || h->n_players > MAX_PLAYERS) return -WINPROB_ETRUNC;
    if (h->header_bytes != wp_header_bytes(h->n_players)) return -WINPROB_ETRUNC;
    if (h->step_bytes != wp_step_bytes(h->n_players)) return -WINPROB_ETRUNC;
    if (len < h->header_bytes) return -WINPROB_ETRUNC;
    memset(h->elim, 0xFF, sizeof h->elim);
    for (int i = 0; i < h->n_players; i++) h->elim[i] = (uint8_t)wp_rd8(&r);
    memset(h->name, 0, sizeof h->name);
    memset(h->name_len, 0, sizeof h->name_len);
    for (int s = 0; s < h->n_players; s++) {
        unsigned l = wp_rd8(&r);
        if (l > WINPROB_NAME_MAX) l = WINPROB_NAME_MAX;
        h->name_len[s] = (uint8_t)l;
        memcpy(h->name[s], r, l);
        h->name[s][l] = 0;
        r += WINPROB_NAME_MAX;
    }
    return h->header_bytes;
}

int winprob_read_step(const unsigned char *buf, int len, const WinprobHeader *h,
                      int i, WinprobStep *out) {
    if (!buf || !h || !out || i < 0 || i >= h->n_steps) return -WINPROB_ETRUNC;
    long at = (long)h->header_bytes + (long)i * h->step_bytes;
    if (at + h->step_bytes > len) return -WINPROB_ETRUNC;
    const unsigned char *r = buf + at;
    int np = h->n_players;
    out->move    = (uint16_t)wp_rd16(&r);
    out->seat    = (uint8_t)wp_rd8(&r);
    out->kind    = (uint8_t)wp_rd8(&r);
    out->n_cards = (uint8_t)wp_rd8(&r);
    for (int k = 0; k < WINPROB_MOVE_CARDS; k++) out->cards[k] = (uint8_t)wp_rd8(&r);
    out->target  = (uint8_t)wp_rd8(&r);
    out->deck    = (uint8_t)wp_rd8(&r);
    out->n_truth = (uint16_t)wp_rd16(&r);
    out->n_belief= (uint16_t)wp_rd16(&r);
    for (int s = 0; s < np; s++) out->truth_fool[s]  = (uint16_t)wp_rd16(&r);
    for (int s = 0; s < np; s++) out->truth_mean[s]  = (uint16_t)wp_rd16(&r);
    for (int s = 0; s < np; s++) out->belief_fool[s] = (uint16_t)wp_rd16(&r);
    for (int s = 0; s < np; s++) out->belief_mean[s] = (uint16_t)wp_rd16(&r);
    for (int s = 0; s < np; s++) out->hand[s]        = (uint8_t)wp_rd8(&r);
    for (int s = np; s < MAX_PLAYERS; s++) {
        out->truth_fool[s] = out->truth_mean[s] = WINPROB_NONE;
        out->belief_fool[s] = out->belief_mean[s] = WINPROB_NONE;
        out->hand[s] = 0xFF;
    }
    return h->step_bytes;
}

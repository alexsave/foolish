// botcup_adapter.c - botcup_adapter.h.
#include "botcup_adapter.h"

#include "botcup.h"
#include "strategy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const BotEntry haiku_bot, sonnet_bot, opus_bot, fable_bot;

// The engine's not-a-card {-1, -1} (botcup.h). It was { 0, 1 }: the 2 of spades at 6-8 players.
static const Card HIDDEN = { BOTCUP_HIDDEN_SUIT, BOTCUP_HIDDEN_VALUE };

void botcup_make_view(Game *v, const Game *g, int seat) {
    game_clone(v, g);
    for (int i = 0; i < v->deck_count; i++) v->deck[i] = HIDDEN;
    for (int p = 0; p < v->num_players; p++) {
        if (p == seat) continue;
        for (int i = 0; i < v->players[p].hand_count; i++) v->players[p].hand[i] = HIDDEN;
    }
    for (int k = 0; k < v->num_logs; k++) {
        GameLog *l = &v->logs[k];
        if (l->log_type != LOG_DRAW || l->player_idx == seat) continue;
        for (int i = 0; i < l->num_pairs; i++) {
            Card c = l->pairs[i].primary;
            // The face-up trump is public: everyone sees who takes it.
            bool is_flip = g->has_flipped && c.suit == g->flipped.suit && c.value == g->flipped.value;
            if (!is_flip) l->pairs[i].primary = HIDDEN;
        }
    }
}

#define N_BOTS (STRAT_BOTCUP_KESTREL - STRAT_BOTCUP_HAIKU + 1)

// THE BOTS KEEP STATE IN STATICS (their scratch games, kestrel's flipped trump per seat), so one
// bot never runs on two threads at once: the server's bot workers take its lock. A bot's
// new_game is called when the game its seat sees is not the one it saw last (the log grew
// shorter, or another trump card): the server never says when a game starts.
typedef struct {
    pthread_mutex_t mu;
    BotcupStats     st;
    int16_t         last_logs[MAX_PLAYERS];
    Card            last_flip[MAX_PLAYERS];
    int8_t          last_n[MAX_PLAYERS];
} Slot;
static Slot g_slot[N_BOTS] = {
    { PTHREAD_MUTEX_INITIALIZER, { 0, 0, 0, 0 }, { 0 }, { { 0, 0 } }, { 0 } },
    { PTHREAD_MUTEX_INITIALIZER, { 0, 0, 0, 0 }, { 0 }, { { 0, 0 } }, { 0 } },
    { PTHREAD_MUTEX_INITIALIZER, { 0, 0, 0, 0 }, { 0 }, { { 0, 0 } }, { 0 } },
    { PTHREAD_MUTEX_INITIALIZER, { 0, 0, 0, 0 }, { 0 }, { { 0, 0 } }, { 0 } },
};

static const BotEntry *entry(int strat) {
    switch (strat) {
        case STRAT_BOTCUP_HAIKU:   return &haiku_bot;
        case STRAT_BOTCUP_SONNET:  return &sonnet_bot;
        case STRAT_BOTCUP_SAPPER:  return &opus_bot;
        case STRAT_BOTCUP_KESTREL: return &fable_bot;
        default:                   return NULL;
    }
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// A move slower than this is logged as it happens (stderr: the game server's log).
#define SLOW_US 250000u

int botcup_run(BotcupChooseFn fn, const Game *g, int seat, const LegalMoves *moves, bool *fault) {
    *fault = false;
    Game *view = malloc(sizeof *view);
    if (!view) return 0;
    botcup_make_view(view, g, seat);
    // A bot may run the engine on its own copies; none of that may move the real game's streams.
    uint64_t r1 = game_rng_get(), r2 = random_strategy_rng_get();
    unsigned char dr[GAME_DEAL_RNG_STATE_MAX];
    game_deal_rng_get(dr);
    int idx = fn(view, seat, moves);
    game_rng_set(r1);
    random_strategy_set_seed(r2);
    game_deal_rng_set(dr);
    free(view);
    if (idx < 0 || idx >= moves->n) {
        *fault = true;
        idx = 0;
    }
    return idx;
}

int botcup_strategy_choose(int strat, const Game *g, int seat, const LegalMoves *moves) {
    const BotEntry *b = entry(strat);
    if (!b || !g || !moves || moves->n <= 0 || seat < 0 || seat >= MAX_PLAYERS) return -1;
    Slot *s = &g_slot[strat - STRAT_BOTCUP_HAIKU];
    pthread_mutex_lock(&s->mu);
    if (b->new_game && (s->last_n[seat] != g->num_players || g->num_logs < s->last_logs[seat]
                        || !card_eq(s->last_flip[seat], g->flipped)))
        b->new_game(seat, g->num_players);
    s->last_n[seat] = g->num_players;
    s->last_logs[seat] = g->num_logs;
    s->last_flip[seat] = g->flipped;
    bool fault;
    uint64_t t0 = now_us();
    int idx = botcup_run(b->choose, g, seat, moves, &fault);
    uint64_t dt = now_us() - t0;
    s->st.decisions++;
    s->st.total_us += dt;
    if (dt > s->st.max_us) s->st.max_us = dt;
    if (fault) s->st.faults++;
    BotcupStats st = s->st;
    pthread_mutex_unlock(&s->mu);
    if (fault || dt > SLOW_US)
        fprintf(stderr, "botcup: %s %s (%llu us, menu %d)\n", b->name, fault ? "FAULT: index outside the menu" : "slow move",
                (unsigned long long)dt, moves->n);
    if (st.decisions % 100 == 0)
        fprintf(stderr, "botcup: %s %ld decisions, %.2f ms a move, max %.1f ms, %ld faults\n", b->name, st.decisions,
                (double)st.total_us / 1000.0 / (double)st.decisions, (double)st.max_us / 1000.0, st.faults);
    return idx;
}

void botcup_stats(int strat, BotcupStats *out) {
    memset(out, 0, sizeof *out);
    if (!entry(strat)) return;
    Slot *s = &g_slot[strat - STRAT_BOTCUP_HAIKU];
    pthread_mutex_lock(&s->mu);
    *out = s->st;
    pthread_mutex_unlock(&s->mu);
}

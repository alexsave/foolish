#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "world.h"

#include "card.h"
#include "legal.h"

#define INV_PRINT_CAP 40

static const char *g_names[FIND_COUNT] = {
    "conservation",
    "mutation_on_reject",
    "stall",
    "phantom_hand_loss",
    "duplicate_applied",
    "view_regression",
    "queue_overflow",
    "seat_mismatch",
};

const char *inv_name(int kind) {
    return (kind >= 0 && kind < FIND_COUNT) ? g_names[kind] : "?";
}

void inv_print(World *w) {
    u64 total = 0;
    for (int i = 0; i < FIND_COUNT; i++) total += w->findings.count[i];

    printf("\nfindings\n");
    for (int i = 0; i < FIND_COUNT; i++)
        printf("  %-20s %llu\n", inv_name(i), (unsigned long long)w->findings.count[i]);

    if (w->findings.printed >= INV_PRINT_CAP)
        printf("  (report output capped at %d lines)\n", INV_PRINT_CAP);
    printf("  %s\n", total ? "FAIL" : "clean");
}

// A live game that has gone quiet for longer than any latency in the sim. In
// Durak someone can always act, so a playing board that stops changing is
// nobody believing it is their turn: a push that never landed, a bot parked on
// a wake that never came, or a move dropped between the two.
void inv_sweep(World *w) {
    u64 now = sch_now_us(&w->sch);

    for (u32 g = 0; g < w->n_games; g++) {
        GameSlot *s = &w->games[g];
        if (!s->used || s->game.status != GAME_STATUS_PLAYING) continue;

        if (now - s->last_change_us < w->knobs.stall_us) { s->stalled = 0; continue; }
        if (s->stalled) continue;
        s->stalled = 1;

        // Who COULD have moved is the whole diagnosis: a seat with a legal
        // move that nobody played is a lost wakeup, while no legal move
        // anywhere would be the kernel itself painted into a corner.
        static LegalMoves lm;
        u32 parked = 0, subs = 0, able = 0;
        for (int i = 0; i < s->game.num_players; i++) {
            if (s->bots[i].waiting) parked |= 1u << i;
            if (s->seat_subscribed[i]) subs |= 1u << i;

            calculate_legal_moves(&s->game, i, &lm);
            for (int m = 0; m < lm.n; m++)
                if (lm.moves[m].type != MOVE_WAIT) { able |= 1u << i; break; }
        }
        inv_report(w, FIND_STALL,
                   "game %u v%u quiet for %.1fs: defender %d, queue %u, could move 0x%x, bots parked 0x%x, subscribed 0x%x",
                   s->id, s->version, (double)(now - s->last_change_us) / 1e6,
                   s->game.defender, s->qn, able, parked, subs);
    }
}

void inv_report(World *w, int kind, const char *fmt, ...) {
    w->findings.count[kind]++;
    if (w->findings.printed >= INV_PRINT_CAP) return;
    w->findings.printed++;

    fprintf(stderr, "[%8.3fs] %-18s ", (double)sch_now_us(&w->sch) / 1e6, inv_name(kind));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// Straight from server/impls/native/sem_fuzz.c's conservation_ok: hands, deck,
// trump and table must be a partition of the dealt pack, and what is left over
// must be exactly the discard pile.
int inv_conservation(const Game *g, char *why, int whycap) {
    i8 seen[52];
    memset(seen, 0, sizeof seen);
    int concrete = 0;

    #define TALLY(card) do { \
        Card _c = (card); \
        if (card_is_none(_c)) break; \
        int _id = card_to_id(_c); \
        if (_id < 0 || _id > 51) { snprintf(why, whycap, "invalid card suit=%d val=%d", _c.suit, _c.value); return 0; } \
        seen[_id]++; concrete++; \
    } while (0)

    for (int p = 0; p < g->num_players; p++) {
        const Player *pl = &g->players[p];
        if (pl->hand_count < 0 || pl->hand_count > MAX_HAND_SIZE) {
            snprintf(why, whycap, "hand_count=%d seat=%d", pl->hand_count, p);
            return 0;
        }
        for (int i = 0; i < pl->hand_count; i++) TALLY(pl->hand[i]);
    }
    if (g->deck_count < 0 || g->deck_count > MAX_DECK) {
        snprintf(why, whycap, "deck_count=%d", g->deck_count);
        return 0;
    }
    for (int i = 0; i < g->deck_count; i++) TALLY(g->deck[i]);
    if (g->has_flipped) TALLY(g->flipped);
    if (g->num_battles < 0 || g->num_battles > MAX_BATTLES) {
        snprintf(why, whycap, "num_battles=%d", g->num_battles);
        return 0;
    }
    for (int i = 0; i < g->num_battles; i++) {
        TALLY(g->table_battles[i].attack);
        TALLY(g->table_battles[i].defense);
    }
    #undef TALLY

    for (int id = 0; id < 52; id++)
        if (seen[id] > 1) { snprintf(why, whycap, "card id=%d appears %d times", id, seen[id]); return 0; }

    int dealt = g->num_players >= 6 ? 52 : 36;
    int total = concrete + g->discard_pile_length;
    if (total != dealt) {
        snprintf(why, whycap, "%d cards (in play %d + discard %d), dealt %d",
                 total, concrete, g->discard_pile_length, dealt);
        return 0;
    }
    return 1;
}

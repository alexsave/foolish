// unit_legal_cover.c - the defender's cover menu (foolish/c/src/legal.c) at the caps the server
// links the kernel with (foolish/c `make print-human-caps`: MAX_MOVE_CARDS=28, MAX_BATTLES=64,
// MAX_LEGAL_MOVES=4096), where a board with twenty-odd uncovered attacks is a legal state and
// one menu runs on the one request worker.
//
//   1. THE MENU IS THE MENU IT WAS. legal.c prunes its walk of attack subsets (a full menu stops it,
//      and an attack the picked ones leave no card for is never picked). Neither may change a
//      single move or its place: the digest of every menu over a fixed set of boards is pinned
//      to the one the unpruned walk produced (origin/main a7bab4e1a).
//   2. A BIG TABLE IS ANSWERED AT ONCE. Before the prunes the walk visited every k-subset: about
//      4x per two more attacks, a second at 26 with a full menu, two with a hand that covers few
//      of them. Each board here must take well under 50 ms (an instrumented build, ASan or
//      coverage, times nothing: its slowness is not the walk's).
#include "t.h"

#include "legal.h"
#include "../../../shared/c/fnv.h"
#include "../../../shared/c/rng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static uint64_t g_rng;
static uint32_t rnd(uint32_t n) { return rng_u32(&g_rng) % n; }

static uint64_t fnv(uint64_t h, int v) { return fnv1a64_step(h, (uint8_t)v); }

static uint64_t menu_digest(uint64_t h, const LegalMoves *lm) {
    h = fnv(fnv(h, lm->n & 0xff), lm->n >> 8);
    for (int i = 0; i < lm->n; i++) {
        const LegalMove *m = &lm->moves[i];
        h = fnv(fnv(h, m->type), m->n_cards);
        for (int c = 0; c < m->n_cards; c++) {
            h = fnv(fnv(h, m->cards[c].suit), m->cards[c].value);
            if (m->type == MOVE_COVER) h = fnv(fnv(h, m->attack_cards[c].suit), m->attack_cards[c].value);
        }
    }
    return h;
}

// Seat 0 attacks seat 1 at a 2-seat table: the menus asked are seat 1's (covers, transfers, pickup).
static void base(Game *g, int power) {
    memset(g, 0, sizeof *g);
    g->num_players = 2;
    g->status = GAME_STATUS_PLAYING;
    g->power_suit = (int8_t)power;
    g->first_attacker = 0;
    g->defender = 1;
    for (int i = 0; i < 2; i++) g->players[i].status = PLAYER_STATUS_IN;
}

static LegalMoves g_lm;

// A random board from a 52-card deck, no card twice: n_atk uncovered attacks (and a few covered
// ones), the defender's hand of n_hand (cut to what the deck has left), the attacker's of 6.
static void random_board(Game *g, int n_atk, int n_hand) {
    Card deck[52];
    int nd = 0;
    for (int s = 0; s < 4; s++)
        for (int v = 1; v <= 13; v++) deck[nd++] = (Card){ (int8_t)s, (int8_t)v };
    for (int i = nd - 1; i > 0; i--) {
        int j = (int)rnd((uint32_t)i + 1);
        Card t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    base(g, (int)rnd(4));
    int at = 0;
    int n_cov = (int)rnd(3);
    for (int i = 0; i < n_atk; i++) g->table_battles[g->num_battles++] = (Battle){ deck[at++], CARD_NONE };
    for (int i = 0; i < n_cov; i++) {
        Battle b = { deck[at], deck[at + 1] };
        at += 2;
        g->table_battles[g->num_battles++] = b;
    }
    if (n_hand > nd - 6 - at) n_hand = nd - 6 - at;
    for (int i = 0; i < n_hand; i++) g->players[1].hand[i] = deck[at++];
    g->players[1].hand_count = (int16_t)n_hand;
    for (int i = 0; i < 6; i++) g->players[0].hand[i] = deck[at++];
    g->players[0].hand_count = 6;
}

// The three shapes that made the walk slow, n uncovered attacks each.
//   dense:  the defender holds every trump, so every subset covers (the menu fills).
//   sparse: two trumps and low cards, so few attacks cover together (the menu stays short).
//   tight:  one plain ace only the single trump covers, last on the table, the trump first in hand.
static void shaped_board(Game *g, int shape, int n) {
    base(g, SUIT_DIAMONDS);
    int nb = 0, h = 0;
    Player *d = &g->players[1];
    if (shape == 0) {
        for (int s = 0; s < 4 && nb < n; s++) {
            if (s == SUIT_DIAMONDS) continue;
            for (int v = 1; v <= 9 && nb < n; v++) g->table_battles[nb++] = (Battle){ (Card){ (int8_t)s, (int8_t)v }, CARD_NONE };
        }
        for (int v = 1; v <= 13; v++) d->hand[h++] = (Card){ SUIT_DIAMONDS, (int8_t)v };
        for (int s = 0; s < 4 && h < nb; s++) {
            if (s == SUIT_DIAMONDS) continue;
            for (int v = 13; v >= 10 && h < nb; v--) d->hand[h++] = (Card){ (int8_t)s, (int8_t)v };
        }
    } else if (shape == 1) {
        for (int s = 0; s < 4 && nb < n; s++) {
            if (s == SUIT_DIAMONDS) continue;
            for (int v = 13; v >= 5 && nb < n; v--) g->table_battles[nb++] = (Battle){ (Card){ (int8_t)s, (int8_t)v }, CARD_NONE };
        }
        d->hand[h++] = (Card){ SUIT_DIAMONDS, 12 };
        d->hand[h++] = (Card){ SUIT_DIAMONDS, 13 };
        for (int s = 0; s < 4; s++) {
            if (s == SUIT_DIAMONDS) continue;
            for (int v = 1; v <= 4; v++) d->hand[h++] = (Card){ (int8_t)s, (int8_t)v };   // lower than every attack
        }
    } else {
        for (int s = 0; s < 4 && nb < n - 1; s++) {
            if (s == SUIT_DIAMONDS) continue;
            for (int v = 1; v <= 6 && nb < n - 1; v++) g->table_battles[nb++] = (Battle){ (Card){ (int8_t)s, (int8_t)v }, CARD_NONE };
        }
        g->table_battles[nb++] = (Battle){ (Card){ SUIT_SPADES, 13 }, CARD_NONE };
        d->hand[h++] = (Card){ SUIT_DIAMONDS, 1 };
        for (int s = 0; s < 4; s++) {
            if (s == SUIT_DIAMONDS) continue;
            for (int v = 7; v <= 12; v++) d->hand[h++] = (Card){ (int8_t)s, (int8_t)v };
        }
    }
    g->num_battles = (int8_t)nb;
    d->hand_count = (int16_t)h;
    for (int i = 0; i < 6; i++) g->players[0].hand[i] = (Card){ SUIT_HEARTS, (int8_t)(8 + i) };
    g->players[0].hand_count = 6;
}

// The digest of every menu the unpruned walk gave on these boards (see the file's head): 3000
// random boards of 1-18 uncovered attacks against hands of up to 30, and the three shapes at
// 4..16 attacks, each asked of both seats.
#define PINNED_DIGEST 0x316309dd9e48776dull

static void the_menu_is_the_menu_it_was(void) {
    uint64_t h = FNV64_OFFSET;
    g_rng = 42;
    Game g;
    long moves = 0;
    for (int i = 0; i < 3000; i++) {
        int n_atk = 1 + (int)rnd(18);
        int n_hand = n_atk + (int)rnd(31 - n_atk > 0 ? (uint32_t)(31 - n_atk) : 1u);
        if (n_hand > 30) n_hand = 30;
        random_board(&g, n_atk, n_hand);
        for (int seat = 0; seat < 2; seat++) {
            calculate_legal_moves(&g, seat, &g_lm);
            moves += g_lm.n;
            h = menu_digest(h, &g_lm);
        }
    }
    for (int shape = 0; shape < 3; shape++)
        for (int n = 4; n <= 16; n++) {
            shaped_board(&g, shape, n);
            for (int seat = 0; seat < 2; seat++) {
                calculate_legal_moves(&g, seat, &g_lm);
                moves += g_lm.n;
                h = menu_digest(h, &g_lm);
            }
        }
    printf("     menus: %ld moves, digest 0x%016llx\n", moves, (unsigned long long)h);
    T_CHECK(h == PINNED_DIGEST);
}

static void a_big_table_is_answered_at_once(void) {
    static const char *const NAME[3] = { "dense", "sparse", "tight" };
    Game g;
    for (int shape = 0; shape < 3; shape++)
        for (int n = 20; n <= 26; n += 2) {
            if (shape == 2 && n > 20) break;   // tight tops out at 19 attacks (one suit's 1..6 three times, and the ace)
            shaped_board(&g, shape, n);
            const double t0 = now_ms();
            calculate_legal_moves(&g, 1, &g_lm);
            const double ms = now_ms() - t0;
            printf("     %-6s %2d attacks, hand %2d: %4d moves in %.2f ms\n", NAME[shape], g.num_battles, g.players[1].hand_count, g_lm.n, ms);
            T_CHECK(g_lm.n > 0);
#ifndef CB_INSTRUMENTED
            T_CHECK(ms < 50.0);
#endif
        }
}

int main(void) {
    T_RUN(the_menu_is_the_menu_it_was);
    T_RUN(a_big_table_is_answered_at_once);
    return T_DONE();
}

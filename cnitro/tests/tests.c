// Smoke tests for the C engine. Cross-checks that mirror specific TS code:
//
//   1. Deck refill yields exactly 36 cards for 2-player.
//   2. start_game leaves both hands at 6, deck at 36-6-6-1=23 (or 22 if a
//      flipped Ace had to be returned + redrawn, etc.).
//   3. legal-move enumeration for the first-attacker on a hand of 6 distinct
//      values yields 6 single-card moves.
//   4. A trivial cover scenario.
//   5. Forward pass produces finite logits.
//   6. Backward pass keeps loss finite over a synthetic minibatch.

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/tokenize.h"
#include "../src/nn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int n_pass = 0;
static int n_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while(0)

static void make_2p_game(Game *g) {
    memset(g, 0, sizeof(*g));
    g->num_players = 2;
    for (int i = 0; i < 2; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
        snprintf(g->players[i].name, sizeof(g->players[i].name), "P%d", i);
    }
}

static void test_start_game(void) {
    game_set_seed(42);
    random_strategy_set_seed(42);
    Game g; make_2p_game(&g);
    start_game(&g);
    CHECK(g.players[0].hand_count == 6, "p0 starts with 6");
    CHECK(g.players[1].hand_count == 6, "p1 starts with 6");
    int total_cards = g.players[0].hand_count + g.players[1].hand_count
                    + g.deck_count + (g.has_flipped ? 1 : 0);
    CHECK(total_cards == 36, "total cards == 36");
    CHECK(g.power_suit >= 0 && g.power_suit < 4, "power_suit in [0,4)");
    // First attacker must be the holder of the lowest trump (or random if none).
}

// Test: with a hand of 6 distinct-value non-trump cards, first attack should
// emit exactly 6 single-card attack moves (no multi-card, since nobody has
// duplicates).
static void test_legal_first_attack(void) {
    Game g; make_2p_game(&g);
    g.status = GAME_STATUS_PLAYING;
    g.num_battles = 0;
    g.first_attacker = 0;
    g.defender = 1;
    g.power_suit = SUIT_DIAMONDS;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    int v[6] = { 5, 6, 7, 8, 9, 10 };
    for (int i = 0; i < 6; i++) {
        g.players[0].hand[i] = (Card){ SUIT_SPADES, v[i] };
    }
    g.players[0].hand_count = 6;
    g.players[1].hand_count = 6;
    LegalMoves moves;
    calculate_legal_moves(&g, 0, &moves);
    CHECK(moves.n == 6, "6 distinct values -> 6 single-card attacks");
    int n_attack = 0;
    for (int i = 0; i < moves.n; i++) if (moves.moves[i].type == MOVE_ATTACK) n_attack++;
    CHECK(n_attack == 6, "all are attacks");
}

// Test: with two cards of the same value, first attack also includes a
// 2-card combination (3 moves total: two singles + one pair).
static void test_legal_first_attack_duplicate(void) {
    Game g; make_2p_game(&g);
    g.status = GAME_STATUS_PLAYING;
    g.num_battles = 0;
    g.first_attacker = 0;
    g.defender = 1;
    g.power_suit = SUIT_DIAMONDS;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    g.players[0].hand[0] = (Card){ SUIT_SPADES, 5 };
    g.players[0].hand[1] = (Card){ SUIT_HEARTS, 5 };
    g.players[0].hand_count = 2;
    g.players[1].hand_count = 6;
    LegalMoves moves;
    calculate_legal_moves(&g, 0, &moves);
    CHECK(moves.n == 3, "dup value yields 2 singles + 1 pair");
}

// Test: cover by trump beats any non-trump attack.
static void test_can_cover(void) {
    Card a = { SUIT_SPADES, 10 };
    Card t = { SUIT_DIAMONDS, 5 };  // diamonds == trump
    CHECK(can_cover(a, t, SUIT_DIAMONDS), "trump covers non-trump");
    Card lower = { SUIT_SPADES, 9 };
    CHECK(!can_cover(a, lower, SUIT_DIAMONDS), "lower same suit doesn't cover");
    Card higher = { SUIT_SPADES, 11 };
    CHECK(can_cover(a, higher, SUIT_DIAMONDS), "higher same suit covers");
}

// Test: forward pass yields finite logits.
static void test_nn_forward(void) {
    NNParams *p = malloc(sizeof(NNParams));
    nn_init_random(p, 7);
    int tokens[5] = { TOK_CLS, TOK_ROLE_ATK, TOK_DECK_FULL, TOK_SEC_HAND, TOK_CARD_BASE + 0 };
    ForwardCache *fc = malloc(sizeof(ForwardCache));
    nn_forward(p, tokens, 5, fc);
    bool finite = true;
    for (int i = 0; i < NUM_ACTIONS; i++) {
        if (!isfinite(fc->logits[i])) { finite = false; break; }
    }
    CHECK(finite, "forward yields finite logits");
    free(fc); free(p);
}

// Test: a few SGD steps reduce loss on a single repeated sample.
static void test_nn_overfit_one_sample(void) {
    NNParams *p = malloc(sizeof(NNParams));
    NNGrads *gr = malloc(sizeof(NNGrads));
    ForwardCache *fc = malloc(sizeof(ForwardCache));
    nn_init_random(p, 11);
    nn_zero_grads(gr);
    int tokens[6] = { TOK_CLS, TOK_ROLE_DEF, TOK_DECK_LOW, TOK_SEC_HAND, TOK_CARD_BASE + 3, TOK_CARD_BASE + 7 };
    bool legal[NUM_ACTIONS] = { false };
    legal[3] = true; legal[7] = true; legal[ACTION_STOP] = true;
    int target = 7;
    float first_loss = 0.f, last_loss = 0.f;
    for (int step = 0; step < 200; step++) {
        nn_forward(p, tokens, 6, fc);
        float loss = nn_accumulate_grads(p, fc, legal, target, gr);
        nn_apply_grads(p, gr, 0.05f, 1);
        if (step == 0) first_loss = loss;
        last_loss = loss;
    }
    CHECK(last_loss < first_loss * 0.5f, "loss drops over 200 steps");
    free(fc); free(gr); free(p);
}

// Test: tokenization on a synthetic state contains the expected section
// tokens in order.
static void test_tokenize_basic(void) {
    Game g; make_2p_game(&g);
    g.status = GAME_STATUS_PLAYING;
    g.power_suit = SUIT_DIAMONDS;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    g.first_attacker = 0; g.defender = 1;
    g.players[0].hand[0] = (Card){ SUIT_SPADES, 5 };
    g.players[0].hand_count = 1;
    g.players[1].hand[0] = (Card){ SUIT_HEARTS, 6 };
    g.players[1].hand_count = 1;
    g.deck_count = 0;
    g.has_flipped = false;
    InProgress ip = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized t;
    tokenize(&g, 0, &ip, &t);
    CHECK(t.n_tokens > 4, "tokenize emits some tokens");
    CHECK(t.tokens[0] == TOK_CLS, "starts with CLS");
    bool has_role = false, has_hand = false;
    for (int i = 0; i < t.n_tokens; i++) {
        if (t.tokens[i] == TOK_ROLE_FIRST) has_role = true;
        if (t.tokens[i] == TOK_SEC_HAND) has_hand = true;
    }
    CHECK(has_role && has_hand, "has role + hand tokens");
}

// Test: the engine runs a full random-vs-random game without crashing or
// looping. Loser must be one of the two players.
static void test_full_game_random(void) {
    game_set_seed(123);
    random_strategy_set_seed(456);
    Game g; make_2p_game(&g);
    start_game(&g);
    int iters = 0;
    while (game_done(&g) < 0 && iters < 2000) {
        iters++;
        int eligible[2]; int n_elig = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) eligible[n_elig++] = i;
        if (n_elig == 0) break;
        // shuffle by Math.random (matches TS Fisher-Yates)
        for (int i = n_elig - 1; i > 0; i--) {
            int j = (int)(game_random() * (i + 1));
            if (j < 0) j = 0; if (j > i) j = i;
            int tmp = eligible[i]; eligible[i] = eligible[j]; eligible[j] = tmp;
        }
        bool acted = false;
        for (int k = 0; k < n_elig; k++) {
            int idx = eligible[k];
            LegalMoves moves;
            calculate_legal_moves(&g, idx, &moves);
            if (moves.n == 0) continue;
            int chosen = random_strategy_choose(&g, idx, &moves, NULL);
            if (chosen < 0) continue;
            const LegalMove *m = &moves.moves[chosen];
            bool ok = false;
            switch (m->type) {
                case MOVE_ATTACK: ok = handle_attack(&g, idx, m->cards, m->n_cards); break;
                case MOVE_COVER:  ok = handle_cover (&g, idx, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   ok = handle_pass  (&g, idx, m->cards, m->n_cards); break;
                case MOVE_PICKUP: ok = handle_pickup(&g, idx); break;
                case MOVE_GOOD:   ok = handle_good  (&g, idx); break;
                default: break;
            }
            if (ok) { acted = true; break; }
        }
        if (!acted) break;
    }
    int loser = game_done(&g);
    CHECK(loser >= 0, "random vs random terminates");
}

int main(void) {
    test_start_game();
    test_legal_first_attack();
    test_legal_first_attack_duplicate();
    test_can_cover();
    test_nn_forward();
    test_nn_overfit_one_sample();
    test_tokenize_basic();
    test_full_game_random();

    printf("\n%d passed, %d failed\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}

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

// Test: vocab size matches our layout. TOK_CARD_BASE (39) + NUM_CARDS (52)
// for the full 52-card deck = 91. Action vocab is NUM_CARDS + PICKUP + STOP
// = 54.
static void test_vocab_size(void) {
    CHECK(VOCAB_SIZE == TOK_CARD_BASE + NUM_CARDS, "VOCAB_SIZE = TOK_CARD_BASE + NUM_CARDS");
    CHECK(NUM_CARDS == 52, "NUM_CARDS == 52 (full 52-card deck)");
    CHECK(NUM_ACTIONS == 54, "NUM_ACTIONS == NUM_CARDS + PICKUP + STOP");
    CHECK(MAX_OPPONENTS == 7, "MAX_OPPONENTS == 7");
    CHECK(TOK_OPP_SEAT_7 - TOK_OPP_SEAT_1 == MAX_OPPONENTS - 1, "seat tokens contiguous");
    CHECK(TOK_CARD_BASE == TOK_OPP_SEAT_7 + 1, "card base sits right after seat tokens");
}

// Test: action_id round-trip for every card in the full 52-card deck.
// Cards 1..13 in 4 suits should cleanly survive card_action_id →
// action_id_to_card.
static void test_action_id_roundtrip(void) {
    int trump = SUIT_HEARTS;
    int n_ok = 0, n_bad = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int v = MIN_VALUE_LARGE; v <= ACE_VALUE; v++) {
            int id = card_action_id(suit, v, trump);
            CHECK(id >= 0 && id < ACTION_PICKUP, "action id in range");
            Card c;
            action_id_to_card(id, trump, &c);
            if (c.suit == suit && c.value == v) n_ok++; else n_bad++;
        }
    }
    CHECK(n_ok == 52 && n_bad == 0, "all 52 cards round-trip cleanly");
}

// Test: opponent_seat helper for a 1v1 game.
static void test_opponent_seat_1v1(void) {
    Game g; make_2p_game(&g);
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    CHECK(opponent_seat(&g, 0, 1) == 1, "1v1: opp 1 seen from 0 is seat 1");
    CHECK(opponent_seat(&g, 1, 0) == 1, "1v1: opp 0 seen from 1 is seat 1");
    CHECK(opponent_seat(&g, 0, 0) == 0, "self has no seat");
    g.players[1].status = PLAYER_STATUS_OUT;
    CHECK(opponent_seat(&g, 0, 1) == 0, "OUT player gets no seat");
}

// Test: tokenize must NOT leak any opponent card id under TOK_SEC_OPP_SIZES.
// All tokens in the OPP_SIZES section should be either seat tokens or size
// buckets. (This is the no-cheating invariant.)
static void test_tokenize_no_opp_cards(void) {
    Game g; make_2p_game(&g);
    g.status = GAME_STATUS_PLAYING;
    g.power_suit = SUIT_DIAMONDS;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    g.first_attacker = 0; g.defender = 1;
    g.players[0].hand[0] = (Card){ SUIT_SPADES, 5 };
    g.players[0].hand_count = 1;
    // Opponent's hand: a known, distinctive card. If the model ever sees its
    // token id, this test catches it.
    g.players[1].hand[0] = (Card){ SUIT_HEARTS, 11 };
    g.players[1].hand[1] = (Card){ SUIT_DIAMONDS, 13 };  // Ace of trump
    g.players[1].hand_count = 2;
    g.deck_count = 5;
    g.has_flipped = false;
    InProgress ip = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized t;
    tokenize(&g, 0, &ip, &t);

    int forbidden_a = card_token_id(SUIT_HEARTS, 11, SUIT_DIAMONDS);
    int forbidden_b = card_token_id(SUIT_DIAMONDS, 13, SUIT_DIAMONDS);
    bool leaked = false;
    bool saw_seat_1 = false;
    bool saw_size_bucket_after_seat = false;
    for (int i = 0; i < t.n_tokens; i++) {
        if (t.tokens[i] == forbidden_a || t.tokens[i] == forbidden_b) leaked = true;
        if (t.tokens[i] == TOK_OPP_SEAT_1) {
            saw_seat_1 = true;
            int next = i + 1 < t.n_tokens ? t.tokens[i + 1] : -1;
            if (next == TOK_SIZE_LOW || next == TOK_SIZE_MED
                || next == TOK_SIZE_FULL || next == TOK_SIZE_EMPTY) {
                saw_size_bucket_after_seat = true;
            }
        }
    }
    CHECK(!leaked, "opp's specific card ids never appear in token stream");
    CHECK(saw_seat_1, "seat-1 token emitted in OPP_SIZES section");
    CHECK(saw_size_bucket_after_seat, "seat-1 followed by a size bucket (hand size 2 → LOW)");
}

// Test: history move attribution uses TOK_OPP_SEAT_k for opponents.
// Build a synthetic log with a single ATTACK by player 1 against bot=0,
// and verify the seat token (not the legacy player-opp token) appears.
static void test_tokenize_history_seat_attribution(void) {
    Game g; make_2p_game(&g);
    g.status = GAME_STATUS_PLAYING;
    g.power_suit = SUIT_DIAMONDS;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    g.first_attacker = 1; g.defender = 0;
    g.players[0].hand[0] = (Card){ SUIT_CLUBS, 7 };
    g.players[0].hand_count = 1;
    g.players[1].hand_count = 5;
    g.deck_count = 10;
    g.has_flipped = false;
    g.num_logs = 1;
    g.logs[0].log_type = LOG_ATTACK;
    g.logs[0].player_idx = 1;
    g.logs[0].defender_index = -1;
    g.logs[0].num_pairs = 1;
    g.logs[0].pairs[0].primary = (Card){ SUIT_SPADES, 9 };
    g.logs[0].pairs[0].has_target = false;
    InProgress ip = { .role = INPROG_IDLE, .n_cards_chosen = 0 };
    Tokenized t;
    tokenize(&g, 0, &ip, &t);

    // Find TOK_SEC_HISTORY, then immediately after we expect:
    //   TOK_OPP_SEAT_1, TOK_MOVE_ATTACK, <card token>
    int hi = -1;
    for (int i = 0; i < t.n_tokens; i++) if (t.tokens[i] == TOK_SEC_HISTORY) { hi = i; break; }
    CHECK(hi >= 0, "history section emitted");
    CHECK(hi + 3 < t.n_tokens, "enough tokens after history header");
    CHECK(t.tokens[hi + 1] == TOK_OPP_SEAT_1, "history: opponent attributed to seat 1");
    CHECK(t.tokens[hi + 2] == TOK_MOVE_ATTACK, "history: move type ATTACK");
}

// Test: a 3-player handwritten-vs-handwritten-vs-handwritten game runs to a
// loser without crashing or looping. Validates that the game engine handles
// 3+ players (loops are all g->num_players-bounded; previous MAX_PLAYERS=2
// only exercised the 2-player path).
static void test_full_game_3p_handwritten(void) {
    game_set_seed(7);
    random_strategy_set_seed(7);
    Game g; memset(&g, 0, sizeof(g));
    g.num_players = 3;
    for (int i = 0; i < 3; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        snprintf(g.players[i].player_id, sizeof(g.players[i].player_id), "p%d", i);
    }
    start_game(&g);
    int iters = 0;
    while (game_done(&g) < 0 && iters < 4000) {
        iters++;
        int eligible[MAX_PLAYERS]; int n_elig = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) eligible[n_elig++] = i;
        if (n_elig == 0) break;
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
            int chosen = handwritten_strategy_choose(&g, idx, &moves, NULL);
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
    CHECK(loser >= 0, "3p hw vs hw vs hw terminates");
    CHECK(g.num_eliminated == g.num_players - 1, "all but one player eliminated");
}

// Test: a full handwritten-vs-handwritten game terminates without crashing.
static void test_full_game_handwritten(void) {
    game_set_seed(99);
    random_strategy_set_seed(99);
    Game g; make_2p_game(&g);
    start_game(&g);
    int iters = 0;
    while (game_done(&g) < 0 && iters < 2000) {
        iters++;
        int eligible[MAX_PLAYERS]; int n_elig = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) eligible[n_elig++] = i;
        if (n_elig == 0) break;
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
            int chosen = handwritten_strategy_choose(&g, idx, &moves, NULL);
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
    CHECK(loser >= 0, "handwritten vs handwritten terminates");
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
        int eligible[MAX_PLAYERS]; int n_elig = 0;
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
    test_vocab_size();
    test_action_id_roundtrip();
    test_opponent_seat_1v1();
    test_tokenize_no_opp_cards();
    test_tokenize_history_seat_attribution();
    test_full_game_handwritten();
    test_full_game_3p_handwritten();

    printf("\n%d passed, %d failed\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}

// Smoke tests for the C engine. Cross-checks that mirror specific TS code:
//
//   1. start_game leaves both hands at 6 and conserves the 36-card deck.
//   2. legal-move enumeration for the first-attacker on a hand of 6 distinct
//      values yields 6 single-card moves.
//   3. A trivial cover scenario.
//   4. Full random / handwritten games (2p and 3p) run to a single loser.

#include "../src/game.h"
#include "../src/deal_rng.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/bot_roster.h"
#include "../src/bot_knobs.h"
#include "../src/bot_drive.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

// ===========================================================================
// Reject-matrix & edge-path tests.
//
// Full random/handwritten games only ever submit LEGAL moves, so every
// validation REJECT branch in the handlers — and a handful of end-of-round
// success sub-branches — go unexercised by the game-playing tests above.
// These drive each handler with a crafted state that trips one specific
// branch, asserting both the false return and the exact engine_last_reject.
// ===========================================================================

static void setup_playing_2p(Game *g) {
    memset(g, 0, sizeof(*g));
    g->num_players = 2;
    g->status = GAME_STATUS_PLAYING;
    g->power_suit = SUIT_DIAMONDS;
    g->first_attacker = 0;
    g->defender = 1;
    for (int i = 0; i < 2; i++) {
        g->players[i].status = PLAYER_STATUS_IN;
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
    }
}

// Fill the deck with `count` well-formed cards so a round-end refill draws
// (avoids the no-cards elimination branch unless a test wants it).
static void fill_deck(Game *g, int count) {
    for (int i = 0; i < count; i++) {
        g->deck[i].suit  = (int8_t)(i % NUM_SUITS);
        g->deck[i].value = (int8_t)(MIN_VALUE_SMALL + (i % 9));
    }
    g->deck_count = (int16_t)count;
    g->has_flipped = false;
}

#define CHECK_REJECT(call, code, msg) do { \
    bool _r = (call); \
    CHECK(!(_r) && engine_last_reject == (code), msg); \
} while (0)

static void test_attack_rejects(void) {
    Card c7s = { SUIT_SPADES, 7 }, c7h = { SUIT_HEARTS, 7 }, c8c = { SUIT_CLUBS, 8 };
    Card notin = { SUIT_DIAMONDS, 9 };

    Game g; setup_playing_2p(&g);
    g.players[0].hand[0] = c7s;
    g.players[0].hand[1] = c7h;
    g.players[0].hand[2] = c8c;
    g.players[0].hand_count = 3;
    g.players[1].hand_count = 6;

    CHECK_REJECT(handle_attack(&g, 0, &c7s, 0), ENGINE_REJECT_EMPTY, "attack: empty");

    g.status = GAME_STATUS_WAITING;
    CHECK_REJECT(handle_attack(&g, 0, &c7s, 1), ENGINE_REJECT_NOT_PLAYING, "attack: not playing");
    g.status = GAME_STATUS_PLAYING;

    CHECK_REJECT(handle_attack(&g, 1, &c7s, 1), ENGINE_REJECT_IS_DEFENDER, "attack: is defender");
    CHECK_REJECT(handle_attack(&g, 0, &notin, 1), ENGINE_REJECT_NOT_IN_HAND, "attack: not in hand");

    Card dup[2] = { c7s, c7s };
    CHECK_REJECT(handle_attack(&g, 0, dup, 2), ENGINE_REJECT_DUPLICATES, "attack: duplicates");

    Card diff[2] = { c7s, c8c };
    CHECK_REJECT(handle_attack(&g, 0, diff, 2), ENGINE_REJECT_NOT_SAME_VALUE, "attack: not same value");

    // first attack by a non-first-attacker seat.
    g.first_attacker = 1;
    CHECK_REJECT(handle_attack(&g, 0, &c7s, 1), ENGINE_REJECT_NOT_FIRST_ATTACKER, "attack: not first attacker");
    g.first_attacker = 0;

    // regular (non-first) attack of a value not on the table.
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 5 };
    g.table_battles[0].defense = CARD_NONE;
    CHECK_REJECT(handle_attack(&g, 0, &c8c, 1), ENGINE_REJECT_VALUE_NOT_ON_TABLE, "attack: value not on table");
    g.num_battles = 0;

    // defender can't absorb the attack (empty defender hand).
    g.players[1].hand_count = 0;
    CHECK_REJECT(handle_attack(&g, 0, &c7s, 1), ENGINE_REJECT_DEFENDER_CAPACITY, "attack: defender capacity");
}

static void test_cover_rejects_and_success(void) {
    Card cov9 = { SUIT_SPADES, 9 }, cov6 = { SUIT_SPADES, 6 };
    Card atk7 = { SUIT_SPADES, 7 };
    Card notin = { SUIT_HEARTS, 10 }, atk_missing = { SUIT_CLUBS, 5 };

    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = atk7;
    g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = cov9;   // covers 7♠ (same suit, higher)
    g.players[1].hand[1] = cov6;   // does NOT cover 7♠ (lower, same suit)
    g.players[1].hand_count = 2;
    g.players[0].hand_count = 6;

    g.status = GAME_STATUS_WAITING;
    CHECK_REJECT(handle_cover(&g, 1, &cov9, &atk7, 1), ENGINE_REJECT_NOT_PLAYING, "cover: not playing");
    g.status = GAME_STATUS_PLAYING;

    CHECK_REJECT(handle_cover(&g, 1, &cov9, &atk7, 0), ENGINE_REJECT_EMPTY, "cover: empty");

    // No-uncovered takes priority over the not-defender check.
    g.table_battles[0].defense = cov9;
    CHECK_REJECT(handle_cover(&g, 1, &cov9, &atk7, 1), ENGINE_REJECT_NO_UNCOVERED, "cover: no uncovered");
    g.table_battles[0].defense = CARD_NONE;

    CHECK_REJECT(handle_cover(&g, 0, &cov9, &atk7, 1), ENGINE_REJECT_NOT_DEFENDER, "cover: not defender");
    CHECK_REJECT(handle_cover(&g, 1, &notin, &atk7, 1), ENGINE_REJECT_NOT_IN_HAND, "cover: not in hand");

    Card cov_dup[2] = { cov9, cov9 };
    Card atk_two[2] = { atk7, atk7 };
    CHECK_REJECT(handle_cover(&g, 1, cov_dup, atk_two, 2), ENGINE_REJECT_DUPLICATES, "cover: duplicate cover cards");

    CHECK_REJECT(handle_cover(&g, 1, &cov9, &atk_missing, 1), ENGINE_REJECT_ATTACK_NOT_ON_TABLE, "cover: attack not on table");

    // distinct covers in hand, duplicated attack card -> attack duplicates.
    Card cov_two[2] = { cov9, cov6 };
    CHECK_REJECT(handle_cover(&g, 1, cov_two, atk_two, 2), ENGINE_REJECT_DUPLICATES, "cover: duplicate attack cards");

    CHECK_REJECT(handle_cover(&g, 1, &cov6, &atk7, 1), ENGINE_REJECT_CANNOT_COVER, "cover: cannot cover");

    // Success: defender covers, hand not cleared -> all-covered branch.
    bool ok = handle_cover(&g, 1, &cov9, &atk7, 1);
    CHECK(ok, "cover: success returns true");
    CHECK(card_eq(g.table_battles[0].defense, cov9), "cover: battle now covered");
    CHECK(g.has_good_timestamp, "cover: all-covered sets good timestamp");
}

// Defender empties their hand on a cover: discard the table, refill from a
// non-empty deck, advance the defender (the common end-of-round branch).
static void test_cover_clears_hand_round_advance(void) {
    Game g; setup_playing_2p(&g);
    fill_deck(&g, 12);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_SPADES, 9 };
    g.players[1].hand_count = 1;   // the single cover card
    g.players[0].hand[0] = (Card){ SUIT_HEARTS, 6 };
    g.players[0].hand_count = 1;

    Card cov9 = { SUIT_SPADES, 9 }, atk7 = { SUIT_SPADES, 7 };
    bool ok = handle_cover(&g, 1, &cov9, &atk7, 1);
    CHECK(ok, "cover-clear: success");
    CHECK(g.num_battles == 0, "cover-clear: table discarded");
    CHECK(g.discard_pile_length == 2, "cover-clear: two cards discarded");
    CHECK(g.players[1].hand_count > 0, "cover-clear: defender refilled from deck");
}

// Defender empties their hand and the stock is empty too -> defender wins,
// first_attacker is pushed out (the rarer end-of-round win sub-branch).
static void test_cover_clears_hand_wins(void) {
    Game g; setup_playing_2p(&g);
    g.deck_count = 0; g.has_flipped = false;   // no stock: no refill possible
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_SPADES, 9 };
    g.players[1].hand_count = 1;
    g.players[0].hand[0] = (Card){ SUIT_HEARTS, 6 };
    g.players[0].hand_count = 1;

    Card cov9 = { SUIT_SPADES, 9 }, atk7 = { SUIT_SPADES, 7 };
    bool ok = handle_cover(&g, 1, &cov9, &atk7, 1);
    CHECK(ok, "cover-win: success");
    CHECK(g.players[1].status == PLAYER_STATUS_OUT, "cover-win: defender out (empty, no stock)");
    CHECK(g.num_eliminated >= 1, "cover-win: elimination recorded");
}

static void test_pass_rejects_and_success(void) {
    Card p7h = { SUIT_HEARTS, 7 }, p8c = { SUIT_CLUBS, 8 };
    Card notin = { SUIT_DIAMONDS, 9 };

    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = p7h;
    g.players[1].hand_count = 1;
    g.players[0].hand_count = 6;

    g.status = GAME_STATUS_WAITING;
    CHECK_REJECT(handle_pass(&g, 1, &p7h, 1), ENGINE_REJECT_NOT_PLAYING, "pass: not playing");
    g.status = GAME_STATUS_PLAYING;

    CHECK_REJECT(handle_pass(&g, 1, &p7h, 0), ENGINE_REJECT_EMPTY, "pass: empty");

    Card mixed[2] = { p7h, p8c };
    CHECK_REJECT(handle_pass(&g, 1, mixed, 2), ENGINE_REJECT_NOT_SAME_VALUE, "pass: not same value");

    Card dup[2] = { p7h, p7h };
    CHECK_REJECT(handle_pass(&g, 1, dup, 2), ENGINE_REJECT_DUPLICATES, "pass: duplicates");

    CHECK_REJECT(handle_pass(&g, 0, &p7h, 1), ENGINE_REJECT_NOT_DEFENDER, "pass: not defender");
    CHECK_REJECT(handle_pass(&g, 1, &notin, 1), ENGINE_REJECT_NOT_IN_HAND, "pass: not in hand");

    // No table cards.
    g.num_battles = 0;
    CHECK_REJECT(handle_pass(&g, 1, &p7h, 1), ENGINE_REJECT_NO_TABLE_CARDS, "pass: no table cards");
    g.num_battles = 1;

    // A covered battle blocks passing.
    g.table_battles[0].defense = (Card){ SUIT_SPADES, 9 };
    CHECK_REJECT(handle_pass(&g, 1, &p7h, 1), ENGINE_REJECT_COVER_PRESENT, "pass: cover present");
    g.table_battles[0].defense = CARD_NONE;

    // Table value doesn't match the pass card.
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 8 };
    CHECK_REJECT(handle_pass(&g, 1, &p7h, 1), ENGINE_REJECT_PASS_VALUES, "pass: values mismatch");
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };

    // Next player can't absorb the passed-to pile.
    g.players[0].hand_count = 0;
    CHECK_REJECT(handle_pass(&g, 1, &p7h, 1), ENGINE_REJECT_PASS_CAPACITY, "pass: capacity");

    // Success: defender passes 7♥ to the next seat, who becomes defender.
    g.players[0].hand_count = 6;
    bool ok = handle_pass(&g, 1, &p7h, 1);
    CHECK(ok, "pass: success returns true");
    CHECK(g.defender == 0, "pass: defender advances to next seat");
    CHECK(g.num_battles == 2, "pass: passed card joins the table");
}

static void test_pickup_rejects_and_success(void) {
    Game g; setup_playing_2p(&g);
    fill_deck(&g, 12);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = (Card){ SUIT_SPADES, 9 };
    g.players[1].hand[0] = (Card){ SUIT_HEARTS, 6 };
    g.players[1].hand_count = 1;
    g.players[0].hand_count = 6;

    g.status = GAME_STATUS_WAITING;
    CHECK_REJECT(handle_pickup(&g, 1), ENGINE_REJECT_NOT_PLAYING, "pickup: not playing");
    g.status = GAME_STATUS_PLAYING;

    CHECK_REJECT(handle_pickup(&g, 0), ENGINE_REJECT_NOT_DEFENDER, "pickup: not defender");

    g.num_battles = 0;
    CHECK_REJECT(handle_pickup(&g, 1), ENGINE_REJECT_NO_TABLE_CARDS, "pickup: no table cards");
    g.num_battles = 1;

    // Success: defender scoops the (covered) battle into hand.
    bool ok = handle_pickup(&g, 1);
    CHECK(ok, "pickup: success returns true");
    CHECK(g.num_battles == 0, "pickup: table cleared");
    CHECK(g.players[1].hand_count >= 3, "pickup: attack+defense scooped (plus refill)");
}

static void test_good_rejects_and_success(void) {
    // Leave the battle uncovered so a lone attacker's 'good' does NOT trigger
    // the all-covered round transition (which would reset the good mask) — the
    // transition itself is exercised by test_good_round_transition below.
    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = CARD_NONE;  // uncovered
    g.players[0].hand_count = 6;
    g.players[1].hand_count = 6;

    g.status = GAME_STATUS_WAITING;
    CHECK_REJECT(handle_good(&g, 0), ENGINE_REJECT_NOT_PLAYING, "good: not playing");
    g.status = GAME_STATUS_PLAYING;

    g.players[0].status = PLAYER_STATUS_OUT;
    CHECK_REJECT(handle_good(&g, 0), ENGINE_REJECT_NOT_IN_STATUS, "good: not IN");
    g.players[0].status = PLAYER_STATUS_IN;

    CHECK_REJECT(handle_good(&g, 1), ENGINE_REJECT_IS_DEFENDER, "good: is defender");

    // First attacker cannot 'good' before opening the round.
    g.num_battles = 0;
    CHECK_REJECT(handle_good(&g, 0), ENGINE_REJECT_FIRST_MUST_ATTACK, "good: first must attack");
    g.num_battles = 1;

    // Success: the attacker says good; a repeat is rejected as already-good.
    bool ok = handle_good(&g, 0);
    CHECK(ok, "good: success returns true");
    CHECK(g.good_players_mask & (1u << 0), "good: mask records the player");
    CHECK_REJECT(handle_good(&g, 0), ENGINE_REJECT_ALREADY_GOOD, "good: already good");
}

// good by all attackers over a fully-covered table triggers the round
// transition (discard + refill + defender rotation).
static void test_good_round_transition(void) {
    Game g; setup_playing_2p(&g);
    fill_deck(&g, 12);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = (Card){ SUIT_SPADES, 9 };  // fully covered
    g.players[0].hand[0] = (Card){ SUIT_HEARTS, 6 };
    g.players[0].hand_count = 1;
    g.players[1].hand[0] = (Card){ SUIT_CLUBS, 8 };
    g.players[1].hand_count = 1;

    bool ok = handle_good(&g, 0);
    CHECK(ok, "good-transition: success");
    CHECK(g.num_battles == 0, "good-transition: table discarded");
    CHECK(g.discard_pile_length == 2, "good-transition: two cards trashed");
    CHECK(g.defender == 0, "good-transition: defender rotates to old first-attacker's successor");
}

static void test_should_bot_act_edges(void) {
    Game g; setup_playing_2p(&g);
    g.status = GAME_STATUS_GAME_OVER;
    CHECK(!should_bot_act(&g, 0), "should_bot_act: false when not playing");
    g.status = GAME_STATUS_PLAYING;
    g.players[0].status = PLAYER_STATUS_OUT;
    CHECK(!should_bot_act(&g, 0), "should_bot_act: false when player not IN");
}

static void test_next_player_and_game_done_edges(void) {
    Game g; setup_playing_2p(&g);
    // Only one player left IN -> rotation is meaningless, returns current.
    g.players[1].status = PLAYER_STATUS_OUT;
    CHECK(get_next_player_index(&g, 0) == 0, "next_player: <=1 IN returns current");
    // game_done: exactly one IN and the rest OUT -> that seat is the loser.
    CHECK(game_done(&g) == 0, "game_done: last IN player reported");
    setup_playing_2p(&g);
    CHECK(game_done(&g) == -1, "game_done: -1 while two remain");
}

// A short-log instance (log_cap > 0, as used by the sampled-world Monte-Carlo
// slots) keeps only LOG_DISCARD entries and drops everything else into a
// throwaway scratch log — exercise both the kept and dropped paths.
static void test_short_log_instance(void) {
    Game g; setup_playing_2p(&g);
    fill_deck(&g, 12);
    g.log_cap = 4;      // short-log slot
    g.log_virt = 0;
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = (Card){ SUIT_SPADES, 9 };  // fully covered
    g.players[0].hand[0] = (Card){ SUIT_HEARTS, 6 };
    g.players[0].hand_count = 1;
    g.players[1].hand[0] = (Card){ SUIT_CLUBS, 8 };
    g.players[1].hand_count = 1;

    int logs_before = g.num_logs;
    bool ok = handle_good(&g, 0);   // GOOD (dropped) + DISCARD (kept) + DRAW/CHANGE (dropped)
    CHECK(ok, "short-log: round transition succeeds");
    CHECK(g.num_logs <= g.log_cap, "short-log: kept logs never exceed the cap");
    CHECK(g.num_logs >= logs_before, "short-log: at least the discard was retained");
    CHECK(g.log_virt > 0, "short-log: virtual counter advanced past dropped appends");
}

// ---- legal.c edge move-generation states -----------------------------------

static void test_legal_not_playing(void) {
    Game g; setup_playing_2p(&g);
    g.status = GAME_STATUS_WAITING;
    LegalMoves moves;
    calculate_legal_moves(&g, 0, &moves);
    CHECK(moves.n == 0, "legal: no moves when not playing");
}

static void test_legal_attacker_good_and_no_match(void) {
    // Non-defender attacker whose hand shares no value with the table:
    // no attack moves, but GOOD is still offered.
    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = CARD_NONE;
    g.players[0].hand[0] = (Card){ SUIT_HEARTS, 8 };
    g.players[0].hand[1] = (Card){ SUIT_CLUBS, 9 };
    g.players[0].hand_count = 2;
    g.players[1].hand_count = 6;

    LegalMoves moves;
    calculate_legal_moves(&g, 0, &moves);
    int n_good = 0, n_attack = 0;
    for (int i = 0; i < moves.n; i++) {
        if (moves.moves[i].type == MOVE_GOOD) n_good++;
        if (moves.moves[i].type == MOVE_ATTACK) n_attack++;
    }
    CHECK(n_attack == 0, "legal: no attack when hand shares no table value");
    CHECK(n_good == 1, "legal: GOOD offered to the attacker");

    // Once that attacker has said good, they get no moves at all.
    g.good_players_mask |= (1u << 0);
    calculate_legal_moves(&g, 0, &moves);
    CHECK(moves.n == 0, "legal: player who said good gets no moves");
}

static void test_legal_defender_cover_pickup_pass(void) {
    // Uncovered same-value table + a matching-value defender card: the
    // defender is offered COVER(s), PICKUP and PASS.
    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_SPADES, 9 };   // can cover
    g.players[1].hand[1] = (Card){ SUIT_HEARTS, 7 };   // can pass (value 7)
    g.players[1].hand_count = 2;
    g.players[0].hand_count = 6;

    LegalMoves moves;
    calculate_legal_moves(&g, 1, &moves);
    int n_cover = 0, n_pickup = 0, n_pass = 0;
    for (int i = 0; i < moves.n; i++) {
        if (moves.moves[i].type == MOVE_COVER)  n_cover++;
        if (moves.moves[i].type == MOVE_PICKUP) n_pickup++;
        if (moves.moves[i].type == MOVE_PASS)   n_pass++;
    }
    CHECK(n_cover >= 1, "legal: defender offered a cover");
    CHECK(n_pickup == 1, "legal: defender offered pickup while uncovered");
    CHECK(n_pass >= 1, "legal: defender offered a pass (matching value)");
}

static void test_legal_defender_all_covered(void) {
    // Fully covered table: no cover targets, no pickup, no pass.
    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 };
    g.table_battles[0].defense = (Card){ SUIT_SPADES, 9 };
    g.players[1].hand[0] = (Card){ SUIT_HEARTS, 7 };
    g.players[1].hand_count = 1;
    g.players[0].hand_count = 6;

    LegalMoves moves;
    calculate_legal_moves(&g, 1, &moves);
    CHECK(moves.n == 0, "legal: defender has no moves on a fully-covered table");
}

static void test_legal_pass_blocked_variants(void) {
    LegalMoves moves;
    // Two uncovered attacks of DIFFERENT values -> pass is not offered.
    Game g; setup_playing_2p(&g);
    g.num_battles = 2;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 }; g.table_battles[0].defense = CARD_NONE;
    g.table_battles[1].attack = (Card){ SUIT_CLUBS, 8 };  g.table_battles[1].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_HEARTS, 7 };
    g.players[1].hand[1] = (Card){ SUIT_HEARTS, 8 };
    g.players[1].hand_count = 2;
    g.players[0].hand_count = 6;
    calculate_legal_moves(&g, 1, &moves);
    int n_pass = 0;
    for (int i = 0; i < moves.n; i++) if (moves.moves[i].type == MOVE_PASS) n_pass++;
    CHECK(n_pass == 0, "legal: no pass across mixed-value attacks");

    // Single uncovered attack, but defender holds no matching value -> no pass.
    setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 }; g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_HEARTS, 10 };
    g.players[1].hand_count = 1;
    g.players[0].hand_count = 6;
    calculate_legal_moves(&g, 1, &moves);
    n_pass = 0;
    for (int i = 0; i < moves.n; i++) if (moves.moves[i].type == MOVE_PASS) n_pass++;
    CHECK(n_pass == 0, "legal: no pass without a matching-value card");
}

static void test_legal_lite_greedy_cover(void) {
    // The lite generator replaces cover enumeration with a single greedy full
    // cover: one MOVE_COVER when the defender can fully cover, none otherwise.
    Game g; setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 7 }; g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_SPADES, 9 };
    g.players[1].hand[1] = (Card){ SUIT_DIAMONDS, 5 };   // trump alternative
    g.players[1].hand_count = 2;
    g.players[0].hand_count = 6;

    LegalMoves moves;
    calculate_legal_moves_lite(&g, 1, &moves);
    int n_cover = 0;
    for (int i = 0; i < moves.n; i++) if (moves.moves[i].type == MOVE_COVER) n_cover++;
    CHECK(n_cover == 1, "legal-lite: exactly one greedy cover");

    // Uncoverable attack -> greedy emits no cover (still pickup).
    setup_playing_2p(&g);
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 12 }; g.table_battles[0].defense = CARD_NONE;
    g.players[1].hand[0] = (Card){ SUIT_SPADES, 6 };   // lower, can't cover
    g.players[1].hand_count = 1;
    g.players[0].hand_count = 6;
    calculate_legal_moves_lite(&g, 1, &moves);
    n_cover = 0;
    int n_pickup = 0;
    for (int i = 0; i < moves.n; i++) {
        if (moves.moves[i].type == MOVE_COVER)  n_cover++;
        if (moves.moves[i].type == MOVE_PICKUP) n_pickup++;
    }
    CHECK(n_cover == 0, "legal-lite: no cover when defender can't fully cover");
    CHECK(n_pickup == 1, "legal-lite: pickup still offered");
}

// ---------------------------------------------------------------------------
// Wide, reproducible, full-universe deal seed (deal_rng.h / game.c).
// ---------------------------------------------------------------------------

// Fixed-order serialization of a full deal, so two deals compare byte-for-byte.
static int deal_fingerprint(const Game *g, unsigned char *out) {
    int k = 0;
    out[k++] = (unsigned char)g->power_suit;
    out[k++] = (unsigned char)g->first_attacker;
    out[k++] = (unsigned char)(g->has_flipped ? 1 : 0);
    out[k++] = (unsigned char)(g->has_flipped ? (g->flipped.suit * 16 + g->flipped.value) : 0);
    for (int p = 0; p < g->num_players; p++) {
        out[k++] = (unsigned char)g->players[p].hand_count;
        for (int i = 0; i < g->players[p].hand_count; i++)
            out[k++] = (unsigned char)(g->players[p].hand[i].suit * 16 + g->players[p].hand[i].value);
    }
    out[k++] = (unsigned char)(g->deck_count & 0xff);
    for (int i = 0; i < g->deck_count; i++)
        out[k++] = (unsigned char)(g->deck[i].suit * 16 + g->deck[i].value);
    return k;
}

// The ChaCha20 core matches RFC 8439 §2.4.2 bit-for-bit (also cross-checked
// against Node's crypto). This anchors the deal stream to the standard.
static void test_deal_rng_kat(void) {
    uint32_t st[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c,
        0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
        0x00000001, 0x09000000, 0x4a000000, 0x00000000,
    };
    uint32_t out[16];
    deal_rng_block(st, out);
    unsigned char got[64];
    for (int i = 0; i < 16; i++)
        for (int b = 0; b < 4; b++) got[i * 4 + b] = (unsigned char)((out[i] >> (8 * b)) & 0xff);
    static const unsigned char exp[64] = {
        0x10,0xf1,0xe7,0xe4, 0xd1,0x3b,0x59,0x15, 0x50,0x0f,0xdd,0x1f, 0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7, 0x33,0xc0,0x68,0x03, 0x04,0x22,0xaa,0x9a, 0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46, 0x07,0x9f,0xaa,0x09, 0x14,0xc2,0xd7,0x05, 0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1, 0xde,0x16,0x4e,0xb9, 0xcb,0xd0,0x83,0xe8, 0xa2,0x50,0x3c,0x4e,
    };
    CHECK(memcmp(got, exp, 64) == 0, "chacha20 RFC 8439 keystream KAT");
}

// Reproducibility (same seed -> same deal), avalanche (1-bit seed change ->
// different deal), and that game_set_seed() reverts to the legacy LCG path.
static void test_deal_wide_reproducible(void) {
    unsigned char seedA[32], seedB[32];
    for (int i = 0; i < 32; i++) { seedA[i] = (unsigned char)(i * 7 + 1); seedB[i] = seedA[i]; }
    seedB[0] ^= 0x01;  // flip exactly one bit

    unsigned char fpA1[512], fpA2[512], fpB[512];
    Game g;

    game_set_deal_seed_bytes(seedA, 32);
    CHECK(game_deal_seed_active() == 1, "wide deal active after set_deal_seed_bytes");
    make_2p_game(&g); start_game(&g);
    int la1 = deal_fingerprint(&g, fpA1);

    game_set_deal_seed_bytes(seedA, 32);
    make_2p_game(&g); start_game(&g);
    int la2 = deal_fingerprint(&g, fpA2);

    game_set_deal_seed_bytes(seedB, 32);
    make_2p_game(&g); start_game(&g);
    int lb = deal_fingerprint(&g, fpB);

    CHECK(la1 == la2 && memcmp(fpA1, fpA2, la1) == 0, "same seed -> identical deal (reproducible)");
    CHECK(!(la1 == lb && memcmp(fpA1, fpB, la1) == 0), "1-bit seed change -> different deal (avalanche)");

    game_set_seed(42);
    CHECK(game_deal_seed_active() == 0, "game_set_seed reverts to legacy LCG deal");
}

// Every wide deal is a valid permutation: 36 distinct, conserved cards.
static void test_deal_wide_permutation(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 31 + 5);
    for (int t = 0; t < 64; t++) {
        seed[0] = (unsigned char)t; seed[1] = (unsigned char)(t >> 8);
        game_set_deal_seed_bytes(seed, 32);
        Game g; make_2p_game(&g); start_game(&g);
        int seen[64]; memset(seen, 0, sizeof(seen));
        int total = 0, dup = 0;
        for (int p = 0; p < g.num_players; p++)
            for (int i = 0; i < g.players[p].hand_count; i++) {
                int id = g.players[p].hand[i].suit * 16 + g.players[p].hand[i].value;
                if (id < 0 || id >= 64 || seen[id]) dup = 1; else { seen[id] = 1; total++; }
            }
        for (int i = 0; i < g.deck_count; i++) {
            int id = g.deck[i].suit * 16 + g.deck[i].value;
            if (id < 0 || id >= 64 || seen[id]) dup = 1; else { seen[id] = 1; total++; }
        }
        if (g.has_flipped) {
            int id = g.flipped.suit * 16 + g.flipped.value;
            if (id < 0 || id >= 64 || seen[id]) dup = 1; else { seen[id] = 1; total++; }
        }
        CHECK(!dup && total == 36, "wide deal is a valid 36-card permutation");
    }
}

// deal_rng_bounded is free of modulo bias: n=7 (not a power of two) stays
// uniform where a naive `u32 % 7` would over-weight the low buckets.
static void test_deal_rng_unbiased(void) {
    unsigned char seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (unsigned char)(0xA5 ^ i);
    DealRng r; deal_rng_seed(&r, seed);
    const int N = 7, trials = 700000;
    int counts[7] = {0};
    for (int i = 0; i < trials; i++) counts[deal_rng_bounded(&r, (uint32_t)N)]++;
    int expv = trials / N, lo = expv - expv / 20, hi = expv + expv / 20;  // ±5%
    int ok = 1;
    for (int b = 0; b < N; b++) if (counts[b] < lo || counts[b] > hi) ok = 0;
    CHECK(ok, "deal_rng_bounded(7) is uniform (no modulo bias)");
}

// Play a full handwritten-vs-handwritten game whose DECK is seed-dealt
// (shuffle once, then pop) and hash the whole trajectory. The LCG (game_random)
// drives only the harness's player-ordering; every engine draw — the deal and
// every mid-game refill — comes from the deterministic deck, so a fixed seed
// must reproduce the entire game bit-for-bit.
static unsigned run_repro_game(const unsigned char *seed) {
    game_set_seed(99);                     // LCG: harness player-ordering only
    random_strategy_set_seed(99);
    game_set_deal_seed_bytes(seed, 32);    // deck: shuffle + pop (stays on all game)
    Game g; make_2p_game(&g);
    start_game(&g);
    unsigned h = 2166136261u;
#define HH(b) do { h ^= (unsigned char)(b); h *= 16777619u; } while (0)
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
        HH(g.deck_count); HH(g.discard_pile_length); HH(g.power_suit);
        for (int p = 0; p < g.num_players; p++) {
            HH(g.players[p].hand_count);
            for (int i = 0; i < g.players[p].hand_count; i++) {
                HH(g.players[p].hand[i].suit); HH(g.players[p].hand[i].value);
            }
        }
    }
    HH(game_done(&g));
    return h;
#undef HH
}

static void test_whole_game_reproducible(void) {
    unsigned char s[32];
    for (int i = 0; i < 32; i++) s[i] = (unsigned char)(i * 13 + 2);
    unsigned a = run_repro_game(s);
    unsigned b = run_repro_game(s);
    CHECK(a == b, "whole game (deal + every refill) reproduces from the seed");
    s[5] ^= 0xFF;
    unsigned c = run_repro_game(s);
    CHECK(a != c, "a different seed yields a different game trajectory");
}

/* ---------------------- bot roster + knobs (F1/A1) ----------------------- */

// The roster is the one place a bot's identity is written down
// (docs/C_CORE_CONSOLIDATION.md §4.1); these pin the invariants that the TS
// registry, seed.sql and ios_api.c used to each restate in their own words.

static void test_bot_roster_table(void) {
    int n = 0;
    const BotRosterEntry *r = bot_roster(&n);
    CHECK(n > 0 && r != NULL, "roster is non-empty");
    CHECK(n == bot_roster_count(), "bot_roster(&n) and bot_roster_count() agree");

    // Keys unique, tiers strictly increasing (the table IS the strength
    // ladder — the offline picker renders it in order).
    int dup = 0, unordered = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++)
            if (!strcmp(r[i].key, r[j].key)) dup = 1;
        if (i && r[i].tier <= r[i - 1].tier) unordered = 1;
    }
    CHECK(!dup, "roster keys are unique");
    CHECK(!unordered, "roster is in strictly increasing tier order");

    // Round-trip every key through find().
    int roundtrip = 1;
    for (int i = 0; i < n; i++) if (bot_roster_find(r[i].key) != i) roundtrip = 0;
    CHECK(roundtrip, "bot_roster_find round-trips every key to its index");
    CHECK(bot_roster_find("nope") == -1, "unknown key is -1, not a silent fallback");
    CHECK(bot_roster_find(NULL) == -1, "NULL key is -1");
    // A prefix of a real key must not match it (the scan is exact).
    CHECK(bot_roster_find("cord") == -1, "a prefix of a key does not match");
    CHECK(bot_roster_at(-1) == NULL && bot_roster_at(n) == NULL, "bot_roster_at bounds-checks");

    // The rungs the site seeds and the phone shows. These are the sets whose
    // drift this table exists to prevent, so they are asserted by name.
    CHECK(bot_roster_find("cordite") >= 0 && bot_roster_find("octogen") >= 0,
          "the top two rungs are present");
    CHECK(bot_roster_find("cordite_max") == -1 && bot_roster_find("octogen_max") == -1,
          "the _max tiers are gone (octogen_max aliased octogen; cordite_max's flat "
          "budget was weaker than prod at 6-8 players)");

    // handwritten/espresso must be the PRODUCTION mirrors, not the arena
    // variants — ios_api.c pointed at the arena ones, so offline Handwritten
    // was not the site's Handwritten (§3).
    const BotRosterEntry *hw = bot_roster_at(bot_roster_find("handwritten"));
    const BotRosterEntry *es = bot_roster_at(bot_roster_find("espresso"));
    CHECK(hw && hw->strat == STRAT_HANDWRITTEN_PROD, "handwritten -> the _PROD mirror");
    CHECK(es && es->strat == STRAT_ESPRESSO_PROD, "espresso -> the _PROD mirror");

    // Knobs: cordite must carry the deployed budget, or the phone silently
    // runs arena-mode cordite (CD_BUDGET's C default is 0 = arena).
    const BotRosterEntry *cd = bot_roster_at(bot_roster_find("cordite"));
    CHECK(cd && strstr(cd->knobs, "CD_BUDGET=prod"), "cordite carries CD_BUDGET=prod");
    CHECK(cd && strstr(cd->knobs, "CD_RACE=1"), "cordite carries CD_RACE=1");
    CHECK(cd && cd->uses_logs, "cordite is a belief bot (needs the session log)");

    const BotRosterEntry *rnd = bot_roster_at(bot_roster_find("random"));
    CHECK(rnd && !rnd->uses_logs, "random needs no session log");

    // The offline projection: every offline entry resolves, in tier order.
    int on = bot_roster_offline_count();
    CHECK(on > 0 && on <= n, "offline count is a subset of the roster");
    int proj_ok = 1, last = -1;
    for (int i = 0; i < on; i++) {
        int idx = bot_roster_offline_at(i);
        const BotRosterEntry *e = bot_roster_at(idx);
        if (!e || !e->offline || idx <= last) proj_ok = 0;
        last = idx;
    }
    CHECK(proj_ok, "offline projection is in-order and only offline entries");
    CHECK(bot_roster_offline_at(on) == -1 && bot_roster_offline_at(-1) == -1,
          "offline projection bounds-check");
}

static void test_bot_knobs_precedence(void) {
    bot_knobs_clear();
    unsetenv("FOOLISH_TEST_KNOB");

    // Nothing set anywhere -> the default.
    CHECK(bot_knob("FOOLISH_TEST_KNOB") == NULL, "unset knob reads NULL");
    CHECK(bot_knob_int("FOOLISH_TEST_KNOB", 7) == 7, "unset knob falls back to the default");

    // Roster spec supplies a value.
    bot_knobs_set("FOOLISH_TEST_KNOB=3,OTHER=xyz");
    CHECK(bot_knob_int("FOOLISH_TEST_KNOB", 7) == 3, "roster spec supplies the value");
    CHECK(!strcmp(bot_knob("OTHER"), "xyz"), "roster spec reads a string value");
    CHECK(bot_knob("MISSING") == NULL, "a key absent from the spec reads NULL");

    // Env overrides the roster — the research-override rule (bot_knobs.h).
    setenv("FOOLISH_TEST_KNOB", "9", 1);
    CHECK(bot_knob_int("FOOLISH_TEST_KNOB", 7) == 9, "env overrides the roster spec");
    unsetenv("FOOLISH_TEST_KNOB");
    CHECK(bot_knob_int("FOOLISH_TEST_KNOB", 7) == 3, "roster value returns once env is gone");

    // Exact key matching: a prefix key must not shadow a longer one.
    bot_knobs_set("CD_RACE=1,CD_RACE_C=75");
    CHECK(bot_knob_int("CD_RACE", 0) == 1, "CD_RACE reads its own value");
    CHECK(bot_knob_int("CD_RACE_C", 0) == 75, "CD_RACE_C is not shadowed by CD_RACE");
    bot_knobs_set("CD_RACE_C=75,CD_RACE=1");
    CHECK(bot_knob_int("CD_RACE", 0) == 1, "order-independent: CD_RACE after CD_RACE_C");
    CHECK(bot_knob_int("CD_RACE_C", 0) == 75, "order-independent: CD_RACE_C first");

    // Flags.
    bot_knobs_set("ON=1,OFF=0");
    CHECK(bot_knob_flag("ON") && !bot_knob_flag("OFF"), "flags read 1/0");
    CHECK(!bot_knob_flag("ABSENT"), "an absent flag is off");

    // Clearing removes the spec.
    bot_knobs_clear();
    CHECK(bot_knob("CD_RACE") == NULL, "clear drops the spec");
    CHECK(bot_knob_int("CD_RACE", 42) == 42, "after clear, defaults apply");
}

// bot_roster_choose must leave no knobs installed behind it: the arena and the
// MC rollout policies call the strategies directly and must see C defaults.
static void test_bot_roster_choose_scopes_knobs(void) {
    Game g;
    make_2p_game(&g);
    start_game(&g);
    LegalMoves m;
    int seat = g.first_attacker;
    calculate_legal_moves(&g, seat, &m);

    bot_knobs_clear();
    int idx = bot_roster_choose(bot_roster_find("cordite"), &g, seat, &m);
    CHECK(idx >= 0 && idx < m.n, "bot_roster_choose returns a legal move index");
    CHECK(bot_knob("CD_BUDGET") == NULL, "choose leaves no knob spec installed");

    CHECK(bot_roster_choose(-1, &g, seat, &m) == -1, "unknown roster index is -1");
    CHECK(bot_roster_choose(bot_roster_count(), &g, seat, &m) == -1, "out-of-range index is -1");

    // Every offline rung must actually dispatch — a roster entry pointing at a
    // brain this build did not link would otherwise fail only at runtime.
    int all_ok = 1;
    for (int i = 0; i < bot_roster_offline_count(); i++) {
        calculate_legal_moves(&g, seat, &m);
        int r = bot_roster_choose(bot_roster_offline_at(i), &g, seat, &m);
        if (r < 0 || r >= m.n) all_ok = 0;
    }
    CHECK(all_ok, "every offline rung dispatches to a linked brain");
}

/* ------------------------- bot drive cycle (F2/F3) ----------------------- */

// An n-player game dealt from a pinned wide seed, so the sweeps below are
// reproducible and every seed is a genuinely different deal.
static void make_seeded_game(Game *g, int n_players, int seed) {
    unsigned char s[32];
    for (int i = 0; i < 32; i++) s[i] = (unsigned char)(i * 29 + seed * 7 + n_players);
    game_set_seed((uint32_t)(seed + 1));
    random_strategy_set_seed((uint32_t)(seed + 1));
    game_set_deal_seed_bytes(s, 32);

    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) {
        g->players[i].status = PLAYER_STATUS_READY;
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
        snprintf(g->players[i].name, sizeof(g->players[i].name), "P%d", i);
    }
    start_game(g);
}

static void test_bot_pacing_table(void) {
    // The one class->ms table. The server's values, which the phone adopts.
    CHECK(bot_pacing_ms(BOT_PACE_MOVE, 1) == 3000, "a visible move with humans waits 3000ms");
    CHECK(bot_pacing_ms(BOT_PACE_MOVE, 0) == 300, "bots-only games pace at 300ms");
    CHECK(bot_pacing_ms(BOT_PACE_ROUND_TRANSITION, 1) == 3000, "a round transition waits like a move");
    CHECK(bot_pacing_ms(BOT_PACE_ROUND_TRANSITION, 0) == 300, "bots-only round transition paces at 300ms");
    // Bundling only pays off if silent actions cost nothing — including with a
    // human watching, where the server currently still burns its full delay.
    CHECK(bot_pacing_ms(BOT_PACE_BUNDLED_PASSIVE, 1) == 0, "a silent action never delays, even with humans");
    CHECK(bot_pacing_ms(BOT_PACE_BUNDLED_PASSIVE, 0) == 0, "a silent action never delays, bots-only");
    CHECK(bot_pacing_ms(BOT_PACE_NONE, 1) == 0 && bot_pacing_ms(BOT_PACE_NONE, 0) == 0,
          "nothing applied, nothing to wait for");
    CHECK(bot_pacing_ms(999, 1) == 0, "an unknown class does not invent a delay");
}

// Every roster entry must name a DISTINCT brain: bot_drive resolves a seat's
// roster entry back from its STRAT_* id, which is only sound 1:1. The old
// cordite/cordite_max pair (two entries, one brain, different knobs) is exactly
// what would break it.
static void test_bot_roster_strat_unique(void) {
    int n = 0;
    const BotRosterEntry *r = bot_roster(&n);
    int dup = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (r[i].strat == r[j].strat) dup = 1;
    CHECK(!dup, "no two roster entries share a brain (bot_roster_find_by_strat is 1:1)");

    int rt = 1;
    for (int i = 0; i < n; i++) if (bot_roster_find_by_strat(r[i].strat) != i) rt = 0;
    CHECK(rt, "bot_roster_find_by_strat round-trips every entry");
    CHECK(bot_roster_find_by_strat(-1) == -1, "an unassigned seat resolves to no entry");
    CHECK(bot_roster_find_by_strat(STRAT_NOVICHOK) == -1, "an unrostered brain resolves to -1");
}

static void seat_all(Game *g, int strat) {
    for (int i = 0; i < g->num_players; i++) g->players[i].strategy_key = (int8_t)strat;
}

static void test_bot_drive_basic(void) {
    Game g;
    make_2p_game(&g);
    start_game(&g);
    seat_all(&g, STRAT_HANDWRITTEN_PROD);

    BotDriveOut out;
    CHECK(bot_drive(NULL, 0, 4, &out) == -1, "bot_drive rejects a NULL game");
    CHECK(bot_drive(&g, 0, 4, NULL) == -1, "bot_drive rejects a NULL out");

    // human_mask covering every seat: nothing for the kernel to drive.
    int n = bot_drive(&g, 0x3, BOT_DRIVE_MAX_ACTIONS, &out);
    CHECK(n == 0 && out.n == 0, "a fully human table applies nothing");
    CHECK(out.stop == BOT_STOP_NO_ELIGIBLE, "...and says so");

    // All-bot: the cycle applies at least one action and stops on something real.
    n = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &out);
    CHECK(n >= 1, "an all-bot table applies at least one action");
    CHECK(out.n == n, "the returned count is out.n");
    CHECK(out.stop == BOT_STOP_EVENTS || out.stop == BOT_STOP_ENDED
          || out.stop == BOT_STOP_NO_ELIGIBLE || out.stop == BOT_STOP_MAX, "stop reason is a known one");
    CHECK(out.actions[0].seat >= 0 && out.actions[0].seat < g.num_players, "the acting seat is real");

    // A drive never drives a masked seat.
    for (int i = 0; i < 40 && game_done(&g) < 0; i++) {
        bot_drive(&g, 0x1, BOT_DRIVE_MAX_ACTIONS, &out);   // seat 0 is "human"
        int touched_human = 0;
        for (int a = 0; a < out.n; a++) if (out.actions[a].seat == 0) touched_human = 1;
        CHECK(!touched_human, "bot_drive never moves a seat in human_mask");
        if (out.n == 0) break;   // only the human can act — a real stop
    }
}

// A cycle stops on the FIRST visible action: everything before it must be
// silent, or the host would render a move it was never told about.
static void test_bot_drive_bundles_only_silent(void) {
    int bad_order = 0, cycles = 0, bundles = 0;
    for (int seed = 0; seed < 12; seed++) {
        Game g;
        make_seeded_game(&g, 6, seed);
        seat_all(&g, STRAT_HANDWRITTEN_PROD);

        BotDriveOut out;
        for (int step = 0; step < 400 && game_done(&g) < 0; step++) {
            int n = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &out);
            if (n == 0) break;
            cycles++;
            if (n > 1) bundles++;
            for (int a = 0; a < out.n - 1; a++)
                if (out.actions[a].pacing_class != BOT_PACE_BUNDLED_PASSIVE) bad_order = 1;
        }
    }
    CHECK(cycles > 0, "the bundling sweep actually drove games");
    CHECK(!bad_order, "only silent actions are bundled; a visible one ends the cycle");
    // The whole point of F3: 6-player games are full of silent goods.
    CHECK(bundles > 0, "silent actions really do bundle at 6 players (the padding F3 removes)");
}

// The divergence F2 exists to kill: a first-eligible seat walk gives low seats
// a systematic tempo advantage. Over many decisions the shuffle must not.
static void test_bot_drive_fairness(void) {
    int first_actor[MAX_PLAYERS] = { 0 };
    int total = 0;
    for (int seed = 0; seed < 60; seed++) {
        Game g;
        make_seeded_game(&g, 4, seed + 1000);
        seat_all(&g, STRAT_RANDOM);

        BotDriveOut out;
        for (int step = 0; step < 200 && game_done(&g) < 0; step++) {
            // Only score cycles where more than one seat COULD have gone first.
            uint32_t elig = bot_drive_eligible_mask(&g, 0);
            int n_elig = 0;
            for (int s = 0; s < g.num_players; s++) if (elig & (1u << s)) n_elig++;

            if (bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &out) == 0) break;
            if (n_elig > 1) { first_actor[out.actions[0].seat]++; total++; }
        }
    }
    CHECK(total > 50, "the fairness sweep saw enough contended cycles");

    // A first-eligible walk would put ~100% on the lowest eligible seat. Assert
    // no seat takes an absurd share; this is a bias detector, not a uniformity
    // proof (the eligible SET differs per cycle, so shares are not equal).
    int worst = 0;
    for (int s = 0; s < 4; s++) if (first_actor[s] > worst) worst = first_actor[s];
    CHECK(worst < (total * 3) / 4, "no seat monopolises the first move (a seat walk would)");
    int silent_seats = 0;
    for (int s = 0; s < 4; s++) if (first_actor[s] == 0) silent_seats++;
    CHECK(silent_seats == 0, "every seat gets to go first sometimes");
}

// The shuffle must be a pure function of public state: replays and the
// differential harness depend on the same game producing the same bot order,
// and it must not disturb the deal/refill RNG stream.
static void test_bot_drive_deterministic(void) {
    unsigned h[2];
    for (int rep = 0; rep < 2; rep++) {
        Game g;
        make_seeded_game(&g, 4, 77);
        seat_all(&g, STRAT_HANDWRITTEN_PROD);

        unsigned acc = 2166136261u;
        BotDriveOut out;
        for (int step = 0; step < 300 && game_done(&g) < 0; step++) {
            if (bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &out) == 0) break;
            for (int a = 0; a < out.n; a++) {
                acc = (acc ^ (unsigned)out.actions[a].seat) * 16777619u;
                acc = (acc ^ (unsigned)out.actions[a].move.type) * 16777619u;
                acc = (acc ^ (unsigned)out.actions[a].pacing_class) * 16777619u;
            }
        }
        h[rep] = acc;
    }
    CHECK(h[0] == h[1], "the same seeded game drives to the same bot order and moves");
}

int main(void) {
    test_bot_pacing_table();
    test_bot_roster_strat_unique();
    test_bot_drive_basic();
    test_bot_drive_bundles_only_silent();
    test_bot_drive_fairness();
    test_bot_drive_deterministic();

    test_bot_roster_table();
    test_bot_knobs_precedence();
    test_bot_roster_choose_scopes_knobs();

    test_deal_rng_kat();
    test_whole_game_reproducible();
    test_deal_wide_reproducible();
    test_deal_wide_permutation();
    test_deal_rng_unbiased();
    test_start_game();
    test_legal_first_attack();
    test_legal_first_attack_duplicate();
    test_can_cover();
    test_full_game_random();
    test_full_game_handwritten();
    test_full_game_3p_handwritten();

    // Reject-matrix & edge-path coverage.
    test_attack_rejects();
    test_cover_rejects_and_success();
    test_cover_clears_hand_round_advance();
    test_cover_clears_hand_wins();
    test_pass_rejects_and_success();
    test_pickup_rejects_and_success();
    test_good_rejects_and_success();
    test_good_round_transition();
    test_short_log_instance();
    test_should_bot_act_edges();
    test_next_player_and_game_done_edges();
    test_legal_not_playing();
    test_legal_attacker_good_and_no_match();
    test_legal_defender_cover_pickup_pass();
    test_legal_defender_all_covered();
    test_legal_pass_blocked_variants();
    test_legal_lite_greedy_cover();

    printf("\n%d passed, %d failed\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}

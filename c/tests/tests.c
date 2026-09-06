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
#include "../src/awire.h"
#include "../src/strategy.h"
#include "../src/bot_roster.h"
#include "../src/bot_knobs.h"
#include "../src/bot_drive.h"
#include "../src/replay.h"
#include "../src/replay_steps.h"
#include "../src/evwire.h"
#include "../src/view.h"
#include "../src/json_out.h"
#include "../src/anim_plan.h"
#include "../src/cordite_sim.h"
#include "../wasm/wire.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

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

// Test: awire_apply is the shared kernel apply-entry (server /action + iOS
// bridge route through it). An attack encoded to the wire, decoded back, and
// applied must change the board exactly as a direct handle_attack would; a
// malformed kind or an out-of-range seat must be rejected, not applied.
static void test_awire_apply_roundtrip(void) {
    Game g; make_2p_game(&g);
    g.status = GAME_STATUS_PLAYING;
    g.num_battles = 0;
    g.first_attacker = 0;
    g.defender = 1;
    g.power_suit = SUIT_DIAMONDS;
    g.players[0].status = PLAYER_STATUS_IN;
    g.players[1].status = PLAYER_STATUS_IN;
    for (int i = 0; i < 6; i++) g.players[0].hand[i] = (Card){ SUIT_SPADES, 5 + i };
    g.players[0].hand_count = 6;
    g.players[1].hand_count = 6;

    AwireAction a = {0};
    a.kind = AWIRE_ATTACK; a.n = 1; a.cards[0] = (Card){ SUIT_SPADES, 7 };
    unsigned char buf[8];
    int len = awire_encode(&a, buf, sizeof buf);
    CHECK(len > 0, "awire_encode wrote the attack frame");
    AwireAction dec;
    CHECK(awire_decode(buf, len, &dec) == 1, "awire_decode read it back");
    CHECK(awire_apply(&g, 0, &dec), "awire_apply applied the attack");
    CHECK(g.num_battles == 1, "awire_apply: one battle on the table");
    CHECK(g.table_battles[0].attack.suit == SUIT_SPADES &&
          g.table_battles[0].attack.value == 7, "awire_apply: the 7s is the attack");
    CHECK(g.players[0].hand_count == 5, "awire_apply: attacker down a card");

    AwireAction bad = dec; bad.kind = 99;
    CHECK(!awire_apply(&g, 0, &bad), "awire_apply: unknown kind rejected");
    CHECK(!awire_apply(&g, 7, &dec), "awire_apply: seat out of range rejected");
}

// Test: the kernel records its OWN game-over. A full game played through the
// apply chokepoint (awire_apply — the native server + iOS path) must leave
// g->status == GAME_OVER when it ends, so no host recomputes game_done to keep a
// status of its own (server-consolidation #3). handle_* alone never touch
// g->status; awire_apply/bot_drive settle it.
static void test_awire_apply_settles_game_over(void) {
    game_set_seed(99);
    random_strategy_set_seed(99);
    Game g; memset(&g, 0, sizeof g);
    g.num_players = 3;
    for (int i = 0; i < 3; i++) {
        g.players[i].status = PLAYER_STATUS_READY;
        snprintf(g.players[i].player_id, sizeof g.players[i].player_id, "p%d", i);
    }
    start_game(&g);
    CHECK(g.status == GAME_STATUS_PLAYING, "settle: a dealt game is PLAYING");

    int iters = 0;
    while (game_done(&g) < 0 && iters++ < 4000) {
        int eligible[MAX_PLAYERS]; int n_elig = 0;
        for (int i = 0; i < g.num_players; i++) if (should_bot_act(&g, i)) eligible[n_elig++] = i;
        if (n_elig == 0) break;
        bool acted = false;
        for (int k = 0; k < n_elig && !acted; k++) {
            int idx = eligible[k];
            LegalMoves moves; calculate_legal_moves(&g, idx, &moves);
            if (moves.n == 0) continue;
            int chosen = handwritten_strategy_choose(&g, idx, &moves, NULL);
            if (chosen < 0) continue;
            const LegalMove *m = &moves.moves[chosen];
            if (m->type > MOVE_GOOD) continue;   // 'wait' is not an awire action
            AwireAction a; memset(&a, 0, sizeof a);
            a.kind = m->type; a.n = m->n_cards;
            for (int c = 0; c < m->n_cards; c++) { a.cards[c] = m->cards[c]; a.attacks[c] = m->attack_cards[c]; }
            acted = awire_apply(&g, idx, &a);
        }
        if (!acted) break;
    }
    CHECK(game_done(&g) >= 0, "settle: the game terminates under awire_apply");
    CHECK(g.status == GAME_STATUS_GAME_OVER, "settle: the kernel flipped g->status to GAME_OVER");
}

// Test: game_human_mask reads the kernel's own per-seat strategy_key — a host
// asks the kernel which seats it must not drive (STRATEGY_KEY_HUMAN) instead of
// keeping an is_ai array of its own (server-consolidation, STRATEGY_KEY_HUMAN).
static void test_game_human_mask(void) {
    Game g; memset(&g, 0, sizeof g);
    g.num_players = 4;
    g.players[0].strategy_key = STRATEGY_KEY_HUMAN;   // human
    g.players[1].strategy_key = 0;                    // bot (roster index 0)
    g.players[2].strategy_key = STRATEGY_KEY_HUMAN;   // human
    g.players[3].strategy_key = 5;                    // bot (roster index 5)
    uint32_t m = game_human_mask(&g);
    CHECK(m == ((1u << 0) | (1u << 2)), "game_human_mask: exactly the two human seats");
    CHECK(!(m & (1u << 1)) && !(m & (1u << 3)), "game_human_mask: bot seats excluded");
}

// Test: game_seat_and_deal is the kernel's one lobby->dealt-board entry — seat
// count + per-seat kind + the deal, so hosts stop hand-rolling it (#5).
static void test_game_seat_and_deal(void) {
    game_set_seed(7);
    // Explicit strategies: seats, kinds, and a real deal in one call.
    Game g; memset(&g, 0, sizeof g);
    int8_t strat[3] = { STRATEGY_KEY_HUMAN, 0, STRATEGY_KEY_HUMAN };
    game_seat_and_deal(&g, strat, 3);
    CHECK(g.num_players == 3, "seat_and_deal: seat count set");
    CHECK(g.status == GAME_STATUS_PLAYING, "seat_and_deal: dealt game is PLAYING");
    CHECK(g.players[0].strategy_key == STRATEGY_KEY_HUMAN && g.players[1].strategy_key == 0 &&
          g.players[2].strategy_key == STRATEGY_KEY_HUMAN, "seat_and_deal: strategy_key wired");
    CHECK(g.players[0].hand_count == 6, "seat_and_deal: hands dealt");
    CHECK(game_human_mask(&g) == ((1u << 0) | (1u << 2)), "seat_and_deal: human mask matches wiring");

    // NULL keeps the seats' existing kinds — the incremental-lobby case.
    Game g2; memset(&g2, 0, sizeof g2);
    g2.num_players = 2;
    g2.players[0].strategy_key = STRATEGY_KEY_HUMAN;
    g2.players[1].strategy_key = 3;
    game_seat_and_deal(&g2, NULL, 2);
    CHECK(g2.players[0].strategy_key == STRATEGY_KEY_HUMAN && g2.players[1].strategy_key == 3,
          "seat_and_deal(NULL): existing kinds preserved");
    CHECK(g2.status == GAME_STATUS_PLAYING && g2.players[1].hand_count == 6,
          "seat_and_deal(NULL): still deals");

    // A bad seat count is a no-op — no deal, no seats.
    Game g3; memset(&g3, 0, sizeof g3);
    game_seat_and_deal(&g3, NULL, 1);
    CHECK(g3.status != GAME_STATUS_PLAYING && g3.num_players == 0, "seat_and_deal: n<2 is a no-op");
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

// ---------- unambiguous_cover (F9, the one-tap-cover resolver) --------------
//
// Trump is DIAMONDS throughout, so spade/heart/club cards never cover across
// suits by accident and the cases test exactly the pairing logic they mean to.
static Battle uc_uncovered(Card attack) {
    Battle b; b.attack = attack; b.defense = CARD_NONE; return b;
}

static void test_unambiguous_cover_one_card_one_attack(void) {
    Battle t[1] = { uc_uncovered((Card){ SUIT_SPADES, 5 }) };
    Card cover[1] = { { SUIT_SPADES, 7 } };
    Card out[1];
    int r = unambiguous_cover(cover, 1, t, 1, SUIT_DIAMONDS, out);
    CHECK(r == 1, "one card covering one attack is unambiguous");
    CHECK(card_eq(out[0], (Card){ SUIT_SPADES, 5 }), "and it names the attack it covers");
}

static void test_unambiguous_cover_rejects_when_it_cannot_cover(void) {
    Battle t[1] = { uc_uncovered((Card){ SUIT_SPADES, 7 }) };
    Card cover[1] = { { SUIT_SPADES, 5 } };  // lower, same suit — cannot cover
    Card out[1];
    CHECK(unambiguous_cover(cover, 1, t, 1, SUIT_DIAMONDS, out) == 0,
          "a card that covers nothing is not a cover");
}

// The core ambiguity: ONE card that can cover EITHER of two attacks. Which one
// did the player mean? Unknowable — must refuse, or the UI silently picks wrong.
static void test_unambiguous_cover_refuses_a_genuine_ambiguity(void) {
    Battle t[2] = {
        uc_uncovered((Card){ SUIT_SPADES, 5 }),
        uc_uncovered((Card){ SUIT_SPADES, 6 }),
    };
    Card cover[1] = { { SUIT_SPADES, 7 } };  // covers both 5s and 6s
    Card out[1];
    CHECK(unambiguous_cover(cover, 1, t, 2, SUIT_DIAMONDS, out) == 0,
          "one card that could cover either attack is ambiguous, not auto-committed");
}

// Two attacks in different suits, each cover card fits exactly one — one valid
// pairing, unambiguous, and the output pairs card i with the attack it covers.
static void test_unambiguous_cover_pairs_by_suit(void) {
    Battle t[2] = {
        uc_uncovered((Card){ SUIT_SPADES, 5 }),
        uc_uncovered((Card){ SUIT_HEARTS, 5 }),
    };
    Card cover[2] = { { SUIT_SPADES, 7 }, { SUIT_HEARTS, 7 } };
    Card out[2];
    int r = unambiguous_cover(cover, 2, t, 2, SUIT_DIAMONDS, out);
    CHECK(r == 1, "two cards each fitting one attack is unambiguous");
    CHECK(card_eq(out[0], (Card){ SUIT_SPADES, 5 }), "cover card 0 covers the spade");
    CHECK(card_eq(out[1], (Card){ SUIT_HEARTS, 5 }), "cover card 1 covers the heart");
}

// The subtle one: two cards, two attacks, BOTH cards can cover BOTH attacks, so
// there are two valid pairings — but they cover the SAME set of attacks, so the
// cover is unambiguous even though the assignment is not unique. A resolver that
// compared PAIRINGS instead of attack SETS would wrongly refuse this.
static void test_unambiguous_cover_same_set_different_pairing_is_ok(void) {
    Battle t[2] = {
        uc_uncovered((Card){ SUIT_SPADES, 5 }),
        uc_uncovered((Card){ SUIT_SPADES, 6 }),
    };
    Card cover[2] = { { SUIT_SPADES, 7 }, { SUIT_SPADES, 8 } };  // both cover both
    Card out[2];
    int r = unambiguous_cover(cover, 2, t, 2, SUIT_DIAMONDS, out);
    CHECK(r == 1, "two pairings covering the same attack set is still unambiguous");
    // Whichever valid pairing it returns, the two attacks covered must be 5s+6s.
    bool set_ok = (card_eq(out[0], (Card){ SUIT_SPADES, 5 }) && card_eq(out[1], (Card){ SUIT_SPADES, 6 }))
               || (card_eq(out[0], (Card){ SUIT_SPADES, 6 }) && card_eq(out[1], (Card){ SUIT_SPADES, 5 }));
    CHECK(set_ok, "and it returns a valid full pairing of the two attacks");
}

static void test_unambiguous_cover_more_cards_than_attacks(void) {
    Battle t[1] = { uc_uncovered((Card){ SUIT_SPADES, 5 }) };
    Card cover[2] = { { SUIT_SPADES, 7 }, { SUIT_SPADES, 8 } };
    Card out[2];
    CHECK(unambiguous_cover(cover, 2, t, 1, SUIT_DIAMONDS, out) == 0,
          "more cover cards than uncovered attacks cannot be a full cover");
}

static void test_unambiguous_cover_trump_over_plain(void) {
    Battle t[1] = { uc_uncovered((Card){ SUIT_SPADES, 5 }) };
    Card cover[1] = { { SUIT_DIAMONDS, 2 } };  // low trump beats a plain card
    Card out[1];
    int r = unambiguous_cover(cover, 1, t, 1, SUIT_DIAMONDS, out);
    CHECK(r == 1, "a trump covers a plain attack");
    CHECK(card_eq(out[0], (Card){ SUIT_SPADES, 5 }), "over the plain attack");
}

// Empty selection / nothing to cover are both "no cover", not a crash.
static void test_unambiguous_cover_degenerate_inputs(void) {
    Battle t[1] = { uc_uncovered((Card){ SUIT_SPADES, 5 }) };
    Card cover[1] = { { SUIT_SPADES, 7 } };
    Card out[1];
    CHECK(unambiguous_cover(cover, 0, t, 1, SUIT_DIAMONDS, out) == 0, "no cover cards selected");
    // A table whose only battle is already covered offers nothing to cover.
    Battle covered[1];
    covered[0].attack = (Card){ SUIT_SPADES, 5 };
    covered[0].defense = (Card){ SUIT_SPADES, 9 };
    CHECK(unambiguous_cover(cover, 1, covered, 1, SUIT_DIAMONDS, out) == 0, "nothing left uncovered");
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

// ---------------------------------------------------------------------------
// THE DEAL ORDER (game.c refill_player_hands): the first attacker draws first,
// then the table clockwise SKIPPING the defender, then the defender last.
// Always - deep stock or shallow, empty defender hand or full.
//
// Nothing else in this suite pins it, and nothing else can: a full game only
// ever sees the order through the cards it happens to receive, so the wrong
// order plays a perfectly legal game and every round-trip test stays green
// (they did - this order shipped wrong for a year). It needs a stocked deck and
// named cards.
// ---------------------------------------------------------------------------
static void setup_playing_np(Game *g, int np) {
    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)np;
    g->status = GAME_STATUS_PLAYING;
    g->power_suit = SUIT_DIAMONDS;
    g->first_attacker = 0;
    g->defender = 1;
    g->deterministic_deck = true;   // draw_index pops deck[0], so the deck is a queue
    for (int i = 0; i < np; i++) {
        g->players[i].status = PLAYER_STATUS_IN;
        snprintf(g->players[i].player_id, sizeof(g->players[i].player_id), "p%d", i);
    }
}

// deck[i] is the (i+1)'th card drawn, and its value says so: 5, 6, 7, ...
static void stock_countable_deck(Game *g, int count) {
    for (int i = 0; i < count; i++) {
        g->deck[i].suit  = (int8_t)(i % NUM_SUITS);
        g->deck[i].value = (int8_t)(MIN_VALUE_SMALL + i);
    }
    g->deck_count = (int16_t)count;
    g->has_flipped = false;
}

// The seats, in the order they were logged as drawing.
static int draw_log_order(const Game *g, int *seats, int cap) {
    int n = 0;
    for (int i = 0; i < g->num_logs && n < cap; i++)
        if (g->logs[i].log_type == LOG_DRAW) seats[n++] = g->logs[i].player_idx;
    return n;
}

// CHECK takes a plain string, and every assertion here wants to name what it
// actually saw, so this formats one first.
static char g_deal_msg[192];
#define DCHECK(cond, ...) do { \
    snprintf(g_deal_msg, sizeof g_deal_msg, __VA_ARGS__); \
    CHECK(cond, g_deal_msg); \
} while (0)

static void test_deal_order(void) {
    int seats[MAX_PLAYERS];

    // 4 players, everyone one card short, stock deep enough for all of them.
    {
        Game g; setup_playing_np(&g, 4);
        stock_countable_deck(&g, 20);
        for (int s = 0; s < 4; s++) {
            for (int i = 0; i < CARDS_PER_PLAYER - 1; i++)
                g.players[s].hand[i] = (Card){ SUIT_SPADES, ACE_VALUE };
            g.players[s].hand_count = CARDS_PER_PLAYER - 1;
        }
        engine_run_refill(&g);

        int n = draw_log_order(&g, seats, MAX_PLAYERS);
        DCHECK(n == 4, "deal order: four seats drew, got %d", n);
        DCHECK(seats[0] == 0 && seats[1] == 2 && seats[2] == 3 && seats[3] == 1,
              "deal order: want 0,2,3,1 (attacker, clockwise past the defender, "
              "defender last), got %d,%d,%d,%d",
              seats[0], seats[1], seats[2], seats[3]);
        // and the cards prove it: the top of the talon went to the attacker,
        // the fourth card to the defender.
        DCHECK(g.players[0].hand[5].value == MIN_VALUE_SMALL,
              "deal order: the first attacker got the top card");
        DCHECK(g.players[1].hand[5].value == MIN_VALUE_SMALL + 3,
              "deal order: the defender got the fourth card, got %d",
              g.players[1].hand[5].value);
    }

    // The point of the rule: a talon too short to go round leaves the DEFENDER
    // empty-handed, not whoever sits before the first attacker.
    {
        Game g; setup_playing_np(&g, 4);
        stock_countable_deck(&g, 2);
        for (int s = 0; s < 4; s++) {
            for (int i = 0; i < CARDS_PER_PLAYER - 1; i++)
                g.players[s].hand[i] = (Card){ SUIT_SPADES, ACE_VALUE };
            g.players[s].hand_count = CARDS_PER_PLAYER - 1;
        }
        engine_run_refill(&g);
        DCHECK(g.players[0].hand_count == 6 && g.players[2].hand_count == 6,
              "short talon: the attacker and the next seat filled");
        DCHECK(g.players[3].hand_count == 5 && g.players[1].hand_count == 5,
              "short talon: it ran out on the last two, defender included");
    }

    // The special case that used to exist in its own right: a clean cover left
    // the defender with no cards. They used to draw FIRST. They draw last.
    {
        Game g; setup_playing_np(&g, 3);
        stock_countable_deck(&g, 12);
        for (int s = 0; s < 3; s++) {
            for (int i = 0; i < CARDS_PER_PLAYER - 2; i++)
                g.players[s].hand[i] = (Card){ SUIT_SPADES, ACE_VALUE };
            g.players[s].hand_count = CARDS_PER_PLAYER - 2;
        }
        g.players[1].hand_count = 0;   // the defender covered with their last card
        engine_run_refill(&g);

        int n = draw_log_order(&g, seats, MAX_PLAYERS);
        DCHECK(n == 3, "empty defender: three seats drew, got %d", n);
        DCHECK(seats[0] == 0 && seats[1] == 2 && seats[2] == 1,
              "empty defender: they draw LAST, got %d,%d,%d", seats[0], seats[1], seats[2]);
        DCHECK(g.players[1].hand[0].value == MIN_VALUE_SMALL + 4,
              "empty defender: their six start at the fifth card, got %d",
              g.players[1].hand[0].value);
    }

    // AFTER A PASS the seat that draws last is the seat that COVERED, not the
    // seat the bout opened against. A transfer moves the defender on one seat
    // and leaves first_attacker where it was, so the cascade reads: opener,
    // clockwise past the NEW defender (which sweeps up the passer in their
    // rotation slot), coverer last.
    //
    // Alice attacks Bob with 6D, Bob passes it back with 6S, Cara covers both
    // and the bout is good. Bob - who was the defender when the cards hit the
    // table - draws in the middle; Cara draws last.
    //
    // Two players cannot pin this: with only one other seat every candidate
    // order collapses to the same two draws. Three can.
    {
        Game g; setup_playing_np(&g, 3);
        g.power_suit = SUIT_CLUBS;      // so 6D/6S/7D/7S are all plain cards
        stock_countable_deck(&g, 24);
        Card ha[4] = {{SUIT_DIAMONDS,6},{SUIT_HEARTS,10},{SUIT_HEARTS,11},{SUIT_HEARTS,12}};
        Card hb[4] = {{SUIT_SPADES,6},  {SUIT_HEARTS,13},{SUIT_HEARTS,14},{SUIT_HEARTS,9}};
        Card hc[4] = {{SUIT_DIAMONDS,7},{SUIT_SPADES,7}, {SUIT_SPADES,13},{SUIT_SPADES,14}};
        memcpy(g.players[0].hand, ha, sizeof ha); g.players[0].hand_count = 4;
        memcpy(g.players[1].hand, hb, sizeof hb); g.players[1].hand_count = 4;
        memcpy(g.players[2].hand, hc, sizeof hc); g.players[2].hand_count = 4;

        Card c6d = { SUIT_DIAMONDS, 6 }, c6s = { SUIT_SPADES, 6 };
        Card cov[2] = { { SUIT_DIAMONDS, 7 }, { SUIT_SPADES, 7 } };
        Card atk[2] = { c6d, c6s };
        CHECK(handle_attack(&g, 0, &c6d, 1), "after a pass: Alice opens with 6D");
        CHECK(handle_pass(&g, 1, &c6s, 1),   "after a pass: Bob transfers with 6S");
        DCHECK(g.defender == 2 && g.first_attacker == 0,
              "after a pass: the defender moves on, the opener does not "
              "(defender %d, first attacker %d)", g.defender, g.first_attacker);
        CHECK(handle_cover(&g, 2, cov, atk, 2), "after a pass: Cara covers both");
        CHECK(handle_good(&g, 0) && handle_good(&g, 1), "after a pass: the bout is good");

        int n = draw_log_order(&g, seats, MAX_PLAYERS);
        DCHECK(n == 3, "after a pass: three seats drew, got %d", n);
        DCHECK(seats[0] == 0 && seats[1] == 1 && seats[2] == 2,
              "after a pass: want 0,1,2 (opener, the passer, the coverer LAST), "
              "got %d,%d,%d", seats[0], seats[1], seats[2]);
        // and the cards prove it: 3 to Alice, 3 to Bob, then 4 to Cara.
        DCHECK(g.players[0].hand[3].value == MIN_VALUE_SMALL,
              "after a pass: the opener got the top card, got %d",
              g.players[0].hand[3].value);
        DCHECK(g.players[1].hand[3].value == MIN_VALUE_SMALL + 3,
              "after a pass: the passer got the fourth card, got %d",
              g.players[1].hand[3].value);
        DCHECK(g.players[2].hand[2].value == MIN_VALUE_SMALL + 6,
              "after a pass: the coverer drew last, starting at the seventh "
              "card, got %d", g.players[2].hand[2].value);
    }

    // Two players: the defender is the only other seat, so the order reads the
    // same either way. Pinned so the two-player game cannot quietly regress.
    {
        Game g; setup_playing_np(&g, 2);
        stock_countable_deck(&g, 3);   // both are two short, so it runs out
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < CARDS_PER_PLAYER - 2; i++)
                g.players[s].hand[i] = (Card){ SUIT_SPADES, ACE_VALUE };
            g.players[s].hand_count = CARDS_PER_PLAYER - 2;
        }
        engine_run_refill(&g);
        int n = draw_log_order(&g, seats, MAX_PLAYERS);
        DCHECK(n == 2 && seats[0] == 0 && seats[1] == 1, "2p deal order: attacker then defender");
        DCHECK(g.players[0].hand_count == 6 && g.players[1].hand_count == 5,
              "2p short talon: the defender is the one left short, got %d and %d",
              g.players[0].hand_count, g.players[1].hand_count);
    }
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

// ---------------------------------------------------------------------------
// PODKIDNOY (GAME_RULE_NO_PASS): the throw-in game, where the defender covers
// or picks up and there is no transfer at all.
//
// The one position is played BOTH WAYS, from the same setup, because that is
// what makes each half of the assertion mean something: the transfer is legal
// here under the classic rules, so "no transfer" is the variant talking and not
// a defective fixture. And the MENU is checked beside the validator - a bot
// searching a move the engine refuses is how a phantom "invalid move" reaches a
// board, and the two live in different files.
// ---------------------------------------------------------------------------
static void setup_transferable(Game *g) {
    setup_playing_2p(g);
    g->num_battles = 1;
    g->table_battles[0].attack  = (Card){ SUIT_SPADES, 7 };
    g->table_battles[0].defense = CARD_NONE;
    g->players[1].hand[0] = (Card){ SUIT_HEARTS, 7 };   // same rank: the transfer
    g->players[1].hand[1] = (Card){ SUIT_SPADES, 9 };   // …and a cover, so the
    g->players[1].hand_count = 2;                       //    defender has a move
    g->players[0].hand_count = 6;
}

static int menu_has(const Game *g, int seat, int type) {
    LegalMoves ml;
    calculate_legal_moves(g, seat, &ml);
    for (int i = 0; i < ml.n; i++) if (ml.moves[i].type == type) return 1;
    return 0;
}

static int menu_lite_has(const Game *g, int seat, int type) {
    LegalMoves ml;
    calculate_legal_moves_lite(g, seat, &ml);
    for (int i = 0; i < ml.n; i++) if (ml.moves[i].type == type) return 1;
    return 0;
}

static void test_podkidnoy(void) {
    Card p7h = { SUIT_HEARTS, 7 };

    // The classic game: the transfer is on the menu and it applies.
    {
        Game g; setup_transferable(&g);
        CHECK(game_pass_allowed(&g), "a zeroed game is the classic passing game");
        CHECK(menu_has(&g, 1, MOVE_PASS), "perevodnoy: the transfer is in the menu");
        CHECK(menu_lite_has(&g, 1, MOVE_PASS), "perevodnoy: …and in the lite menu");
        CHECK(handle_pass(&g, 1, &p7h, 1), "perevodnoy: the transfer applies");
        CHECK(g.defender == 0, "perevodnoy: the transfer moved the defence on");
    }

    // The same position, podkidnoy.
    {
        Game g; setup_transferable(&g);
        g.rules |= GAME_RULE_NO_PASS;
        CHECK(!game_pass_allowed(&g), "podkidnoy: the rule reads back");
        CHECK(!menu_has(&g, 1, MOVE_PASS), "podkidnoy: the menu offers no transfer");
        CHECK(!menu_lite_has(&g, 1, MOVE_PASS), "podkidnoy: nor does the lite menu");
        CHECK_REJECT(handle_pass(&g, 1, &p7h, 1), ENGINE_REJECT_PASS_DISABLED,
                     "podkidnoy: the transfer is refused, and says why");
        CHECK(g.defender == 1 && g.num_battles == 1,
              "podkidnoy: a refused transfer changed nothing");

        // What is LEFT is the whole point: covering and picking up are
        // untouched, so a podkidnoy defender is never stuck.
        CHECK(menu_has(&g, 1, MOVE_COVER), "podkidnoy: covers remain");
        CHECK(menu_has(&g, 1, MOVE_PICKUP), "podkidnoy: pickup remains");
        CHECK(handle_pickup(&g, 1), "podkidnoy: the defender can still pick up");
    }

    // The reason it is a REJECT and not a "wrong cards" answer: the refusal is
    // about the game, not about the hand, so it holds for a transfer that would
    // otherwise be perfect and for one that would not.
    {
        Game g; setup_transferable(&g);
        g.rules |= GAME_RULE_NO_PASS;
        Card wrong = { SUIT_SPADES, 9 };   // in hand, wrong rank for a transfer
        CHECK_REJECT(handle_pass(&g, 1, &wrong, 1), ENGINE_REJECT_PASS_DISABLED,
                     "podkidnoy: the variant answers before the card rules do");
    }
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
    CHECK(bot_drive(NULL, 0, 4, 0, 0, &out) == -1, "bot_drive rejects a NULL game");
    CHECK(bot_drive(&g, 0, 4, 0, 0, NULL) == -1, "bot_drive rejects a NULL out");

    // human_mask covering every seat: nothing for the kernel to drive.
    int n = bot_drive(&g, 0x3, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
    CHECK(n == 0 && out.n == 0, "a fully human table applies nothing");
    CHECK(out.stop == BOT_STOP_NO_ELIGIBLE, "...and says so");

    // All-bot: the cycle applies at least one action and stops on something real.
    n = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
    CHECK(n >= 1, "an all-bot table applies at least one action");
    CHECK(out.n == n, "the returned count is out.n");
    CHECK(out.stop == BOT_STOP_EVENTS || out.stop == BOT_STOP_ENDED
          || out.stop == BOT_STOP_NO_ELIGIBLE || out.stop == BOT_STOP_MAX, "stop reason is a known one");
    CHECK(out.actions[0].seat >= 0 && out.actions[0].seat < g.num_players, "the acting seat is real");

    // A drive never drives a masked seat.
    for (int i = 0; i < 40 && game_done(&g) < 0; i++) {
        bot_drive(&g, 0x1, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);   // seat 0 is "human"
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
            int n = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
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

            if (bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out) == 0) break;
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
            if (bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out) == 0) break;
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

// A strategy's deliberation must not reach the host's animation plan.
//
// The Monte-Carlo bots search by running real handle_* calls over scratch
// games, and engine_snap_hook is global — so a cycle that left it installed
// across the choose hands the host a board full of a rollout's IMAGINARY cards
// (and, since the snapshot ring is small, crowds out the real move's). Hosts
// that choose and apply in separate calls never see this: they reset the
// buffer when they open the apply, after the choose. A cycle must bracket it
// itself, which is what bot_drive does.
//
// Found by e2e/bot_drive_parity.test.ts (a 1-action cycle reported 12 events);
// it hit the server's drive AND fio_bot_drive_json, so the guard lives here,
// where both get it.
static int g_snaps_seen;
static void counting_snap_cb(const Game *g, int tag, int aux) {
    (void)g; (void)tag; (void)aux;
    g_snaps_seen++;
}

static void test_bot_drive_choose_emits_no_snapshots(void) {
    int applied_total = 0, mc_cycles = 0;
    for (int seed = 0; seed < 4; seed++) {
        Game g;
        make_seeded_game(&g, 3, seed + 500);
        // A searching brain: its rollouts apply moves to scratch games.
        seat_all(&g, STRAT_BLACKPOWDER);

        BotDriveOut out;
        for (int step = 0; step < 40 && game_done(&g) < 0; step++) {
            g_snaps_seen = 0;
            engine_snap_hook = counting_snap_cb;
            int n = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
            engine_snap_hook = 0;
            if (n == 0) break;
            applied_total += n;
            mc_cycles++;
            // A real action fires a bounded handful of hooks; a search fires
            // hundreds. The exact count is a rules detail, so assert the
            // property with room: a cycle cannot out-snapshot its own actions
            // by an order of magnitude unless the search leaked in.
            CHECK(g_snaps_seen <= n * 8,
                  "a cycle's snapshots come from its actions, not from the search");
        }
    }
    CHECK(mc_cycles > 0 && applied_total > 0, "the snapshot sweep actually drove games");

    // ...and the hook is left exactly as the host set it, since whether
    // snapshots are wanted at all is the host's call.
    engine_snap_hook = counting_snap_cb;
    Game g;
    make_seeded_game(&g, 3, 909);
    seat_all(&g, STRAT_BLACKPOWDER);
    BotDriveOut out;
    bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
    CHECK(engine_snap_hook == counting_snap_cb, "bot_drive restores the host's snapshot hook");
    engine_snap_hook = 0;
}

// The per-decision seeding hook (bot_drive_pre_action_hook): a host that
// re-seeds per decision must be called once per seat, at each phase, in order —
// CHOOSE before the search, APPLY before the move lands. Seeding once per CYCLE
// instead would shift the stream for every seat after a stream-consuming bot,
// and seeding the draw LCG before the search would feed the search a value the
// single-move path never gave it (both were real, and both changed a bot move
// in e2e/bot_drive_parity.test.ts).
static int g_phase_seq[64];
static int g_phase_n;
static void recording_phase_hook(const Game *g, int seat, int phase) {
    (void)g;
    if (g_phase_n < 64) g_phase_seq[g_phase_n++] = (seat << 4) | phase;
}

static void test_bot_drive_pre_action_hook(void) {
    Game g;
    make_seeded_game(&g, 4, 4242);
    seat_all(&g, STRAT_HANDWRITTEN_PROD);

    CHECK(bot_drive_pre_action_hook == 0, "the hook is NULL unless a host installs it");

    g_phase_n = 0;
    bot_drive_pre_action_hook = recording_phase_hook;
    BotDriveOut out;
    int n = bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out);
    bot_drive_pre_action_hook = 0;

    CHECK(n >= 1, "the hook sweep drove at least one action");
    // Every APPLIED action contributes CHOOSE then APPLY for its own seat. A
    // seat whose move is rejected can add a CHOOSE with no APPLY, so match the
    // applied actions against the tail of the sequence rather than demand equality.
    int k = 0;
    for (int a = 0; a < out.n; a++) {
        const int seat = out.actions[a].seat;
        while (k < g_phase_n && g_phase_seq[k] != ((seat << 4) | BOT_DRIVE_PHASE_CHOOSE)) k++;
        CHECK(k < g_phase_n, "each action was preceded by a CHOOSE for its seat");
        CHECK(k + 1 < g_phase_n && g_phase_seq[k + 1] == ((seat << 4) | BOT_DRIVE_PHASE_APPLY),
              "...and an APPLY for that seat immediately after it, before the move landed");
        k += 2;
    }
    CHECK(g_phase_n >= out.n * 2, "the hook fires per decision, not once per cycle");
}

// The CAS-retry path: a preferred move is reused when still legal, ignored
// when not, and never lets a host smuggle an illegal move past the kernel.
static void test_bot_drive_preferred(void) {
    Game g;
    make_seeded_game(&g, 4, 5);
    seat_all(&g, STRAT_HANDWRITTEN_PROD);

    // What would this cycle do on its own?
    Game base = g;
    BotDriveOut plain;
    bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &plain);
    CHECK(plain.n >= 1, "the baseline cycle acted");
    int seat = plain.actions[0].seat;

    // Offer that seat a DIFFERENT legal move and it must be taken verbatim —
    // that is the retry reusing a decision instead of re-running the search.
    g = base;
    LegalMoves m;
    calculate_legal_moves(&g, seat, &m);
    int other = -1;
    for (int i = 0; i < m.n; i++) {
        if (m.moves[i].type != plain.actions[0].move.type
            || m.moves[i].n_cards != plain.actions[0].move.n_cards) { other = i; break; }
    }
    if (other >= 0) {
        BotDrivePref pref = { (int8_t)seat, m.moves[other] };
        BotDriveOut out;
        bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &pref, 1, &out);
        int used = 0;
        for (int i = 0; i < out.n; i++)
            if (out.actions[i].seat == seat && out.actions[i].move.type == m.moves[other].type
                && out.actions[i].move.n_cards == m.moves[other].n_cards) used = 1;
        CHECK(used, "a still-legal preferred move is reused verbatim");
    }

    // An ILLEGAL preference must be ignored, not applied: legality is never
    // taken on the host's word. Offer a card the seat does not hold.
    g = base;
    LegalMove bogus;
    memset(&bogus, 0, sizeof bogus);
    bogus.type = MOVE_ATTACK;
    bogus.n_cards = 1;
    bogus.cards[0] = g.players[seat].hand[0];
    bogus.cards[0].value = 2;   // not a 36-card-deck value: cannot be in any hand
    BotDrivePref bad = { (int8_t)seat, bogus };
    BotDriveOut out2;
    bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &bad, 1, &out2);
    int played_bogus = 0;
    for (int i = 0; i < out2.n; i++)
        if (out2.actions[i].move.n_cards == 1 && out2.actions[i].move.cards[0].value == 2) played_bogus = 1;
    CHECK(!played_bogus, "an illegal preferred move is never applied");
    CHECK(out2.n >= 1, "...and the seat falls back to choosing normally");

    // A preference for a seat that cannot act changes nothing.
    g = base;
    BotDriveOut out3;
    BotDrivePref none = { (int8_t)((seat + 1) % 4), bogus };
    bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, &none, 1, &out3);
    CHECK(out3.n >= 1, "an irrelevant preference does not stall the cycle");

    // NULL pref is exactly the no-preference cycle.
    g = base;
    BotDriveOut out4;
    bot_drive(&g, 0, BOT_DRIVE_MAX_ACTIONS, 0, 0, &out4);
    CHECK(out4.n == plain.n && out4.actions[0].seat == plain.actions[0].seat,
          "NULL pref reproduces the plain cycle");
}

/* ---------------- A5: replay steps from the kernel ----------------------- */
//
// The invariant, stated once: a v6 replay is the SAME GAME, rebuilt. Not a
// projection that has to be kept in step with one — so what it renders is
// checked against what the engine actually played, not against a mirror.

typedef struct {
    int n_events;
    int n_deals;
    unsigned char last[8192];
    int last_len;
    _Alignas(8) unsigned char last_snap[sizeof(Game)];
    Card deal_hand[MAX_PLAYERS][MAX_HAND_SIZE];
    int  deal_n[MAX_PLAYERS];
} RsTestCtx;

static void rs_test_sink(void *ctx, const EvwEvent *ev) {
    RsTestCtx *c = (RsTestCtx *)ctx;
    c->n_events++;
    // The opening deal, as the replay renders it: cards come from the snapshot,
    // so this is the rebuilt hand itself.
    if (ev->type == EVW_T_DEAL && ev->seat >= 0 && ev->seat < MAX_PLAYERS &&
        c->deal_n[ev->seat] == 0) {
        c->deal_n[ev->seat] = ev->n_cards;
        for (int i = 0; i < ev->n_cards && i < MAX_HAND_SIZE; i++)
            c->deal_hand[ev->seat][i] = ev->cards[i];
        c->n_deals++;
    }
    if (ev->snap) {
        c->last_len = state_put(ev->snap, VIEW_UNMASKED, c->last);
        // Keep the board itself, not just its bytes: the mid-game test reads
        // fields off it. Only prefix fields are touched, which is all a snap has.
        memcpy(c->last_snap, ev->snap, __builtin_offsetof(Game, num_logs));
    }
}

// Every card the deck started with is somewhere: stock, a hand, the table, the
// discard, or the flip. Nothing else is a legal board.
static void rs_check_conservation(const RsTestCtx *c, int np, const char *what) {
    const Game *g = (const Game *)(const void *)c->last_snap;
    const int deck_size = NUM_SUITS * (ACE_VALUE - min_value_for(np) + 1);
    int total = g->deck_count + g->discard_pile_length + (g->has_flipped ? 1 : 0);
    for (int s = 0; s < np; s++) total += g->players[s].hand_count;
    for (int b = 0; b < g->num_battles; b++)
        total += 1 + (card_is_none(g->table_battles[b].defense) ? 0 : 1);
    CHECK(total == deck_size, what);
}

// A seeded game played to the end with the handwritten bot, plus the seed that
// dealt it (replay_encode_v6_from_game re-derives the deal from it).
static bool rs_play_seeded(Game *g, int np, int seed, unsigned char *seed_out) {
    for (int i = 0; i < FOOLISH_SEED_LEN; i++)
        seed_out[i] = (unsigned char)(i * 31 + seed * 13 + np);
    game_set_seed((uint32_t)(seed + 1));
    random_strategy_set_seed((uint32_t)(seed + 1));
    game_set_deal_seed_bytes(seed_out, FOOLISH_SEED_LEN);

    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    start_game(g);

    static LegalMoves moves;
    for (int guard = 0; guard < 20000 && game_done(g) < 0; guard++) {
        bool acted = false;
        for (int pi = 0; pi < np && !acted; pi++) {
            if (!should_bot_act(g, pi)) continue;
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            const LegalMove *m = &moves.moves[handwritten_strategy_choose(g, pi, &moves, 0)];
            switch (m->type) {
                case MOVE_ATTACK: acted = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  acted = handle_cover(g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   acted = handle_pass(g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: acted = handle_pickup(g, pi); break;
                case MOVE_GOOD:   acted = handle_good(g, pi); break;
                default: break;
            }
        }
        if (!acted) return false;
    }
    return game_done(g) >= 0;
}

// The web renders live play from packed evwire frames. A replay must hand it
// the SAME bytes, or "a replay is the game replayed" is only true on the phone.
// This walks the frame stream a code produces and holds it against the events
// the same code produces through the sink — the phone's path — so the two web
// halves and the iOS half cannot drift.
static void test_replay_frames_are_the_replay_events(void) {
    static unsigned char code[1 << 20];
    static unsigned char buf[1 << 16];
    static RsTestCtx ctx;

    for (int np = 2; np <= 4; np++) {
        Game g;
        unsigned char seed[FOOLISH_SEED_LEN];
        if (!rs_play_seeded(&g, np, 500 + np, seed)) { CHECK(0, "seeded game plays out"); continue; }

        int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                             code, (int)sizeof code);
        CHECK(enc > 0, "the played game encodes as v6");
        if (enc <= 0) continue;

        const int steps = replay_steps_count_v6(code, enc, 0);
        CHECK(steps > 0, "a code reports its step count");

        // Pull the whole stream in chunks, exactly as the web bridge must.
        int from = 0, total_frames = 0, guard = 0;
        while (from < steps && ++guard < 4096) {
            int n = 0, next = from;
            int len = replay_steps_frames_v6(code, enc, VIEW_SPECTATOR, from, 0,
                                             buf, (int)sizeof buf, &n, &next);
            CHECK(len >= 0, "a frame chunk serializes");
            if (len < 0) break;
            CHECK(next > from, "a chunk always advances (or the web spins)");
            if (next <= from) break;

            // Every frame must be a whole, length-prefixed evwire frame.
            int q = 0;
            for (int i = 0; i < n; i++) {
                CHECK(q + 2 <= len, "a frame's length header is inside the chunk");
                if (q + 2 > len) break;
                int flen = buf[q] | (buf[q + 1] << 8);
                q += 2;
                CHECK(flen > 0 && q + flen <= len, "a frame's body is inside the chunk");
                if (flen <= 0 || q + flen > len) break;
                q += flen;
            }
            CHECK(q == len, "a chunk is exactly its frames — no trailing slack");
            total_frames += n;
            from = next;
        }
        CHECK(total_frames == steps, "the frame stream covers every step, once");

        // The same code through the phone's path: same steps, same events.
        memset(&ctx, 0, sizeof ctx);
        int r = replay_steps_v6(code, enc, VIEW_SPECTATOR, 0, rs_test_sink, &ctx);
        CHECK(r == REPLAY_EOK, "the same code replays to the event sink");
        CHECK(ctx.n_events > 0, "and the sink saw events");
    }
}

// The step index is what lets a scrubber say "Bot 2 passed" instead of "Bot 2
// attacked": on the wire those are one event type, separated only by a
// reconstructed English sentence, so the web asks the kernel instead of
// pattern-matching prose. That only holds if the index really lines up with the
// frames it describes, and really distinguishes the two.
static void test_replay_step_index_says_what_each_step_is(void) {
    static unsigned char code[1 << 20];
    static unsigned char idx[4096];

    for (int np = 2; np <= 4; np++) {
        Game g;
        unsigned char seed[FOOLISH_SEED_LEN];
        if (!rs_play_seeded(&g, np, 500 + np, seed)) { CHECK(0, "seeded game plays out"); continue; }

        int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                             code, (int)sizeof code);
        CHECK(enc > 0, "the played game encodes as v6");
        if (enc <= 0) continue;

        const int steps = replay_steps_count_v6(code, enc, 0);
        int len = replay_steps_index_v6(code, enc, 0, idx, (int)sizeof idx);
        CHECK(len == steps * RS_INDEX_STRIDE, "one index record per step, no more");
        if (len != steps * RS_INDEX_STRIDE) continue;

        // Step 0 is the deal: nobody's action. DEAL never appears again — it is
        // an atom kind reused for that one step, so a later DEAL would mean an
        // action step was mislabelled as the opening.
        CHECK(idx[0] == REPLAY_ATOM_DEAL, "step 0 is the deal");
        CHECK(idx[1] == RS_SEAT_NONE, "the deal is nobody's action");

        int seen[8] = {0};
        for (int i = 1; i < steps; i++) {
            const int kind = idx[i * RS_INDEX_STRIDE];
            const int seat = idx[i * RS_INDEX_STRIDE + 1];
            CHECK(kind != REPLAY_ATOM_DEAL, "only step 0 is the deal");
            CHECK(kind != REPLAY_ATOM_DRAW, "a draw is never a step of its own");
            CHECK(kind <= REPLAY_ATOM_GOOD, "every step reports a known atom kind");
            if (kind <= REPLAY_ATOM_GOOD) seen[kind]++;
            // A seat acts, or the bout closed on nobody in particular.
            if (kind == REPLAY_ATOM_ROUND_END) {
                CHECK(seat == RS_SEAT_NONE, "a round end is nobody's action");
            } else {
                CHECK(seat < np, "an action's seat is a real seat");
            }
        }

        // The index must agree with the game that was actually played, kind for
        // kind — not merely be self-consistent. Each of these four moves is one
        // log record and one step, so the counts are comparable directly, and
        // any collapse (an index that called every pass an attack, say) shows up
        // here as two counts moving in opposite directions.
        int want[8] = {0};
        for (int i = 0; i < g.num_logs; i++) {
            switch (g.logs[i].log_type) {
                case LOG_ATTACK: want[REPLAY_ATOM_ATTACK]++; break;
                case LOG_COVER:  want[REPLAY_ATOM_COVER]++;  break;
                case LOG_PASS:   want[REPLAY_ATOM_PASS]++;   break;
                case LOG_PICKUP: want[REPLAY_ATOM_PICKUP]++; break;
                default: break;
            }
        }
        CHECK(seen[REPLAY_ATOM_ATTACK] == want[REPLAY_ATOM_ATTACK], "the index counts the game's attacks");
        CHECK(seen[REPLAY_ATOM_COVER]  == want[REPLAY_ATOM_COVER],  "the index counts the game's covers");
        CHECK(seen[REPLAY_ATOM_PASS]   == want[REPLAY_ATOM_PASS],   "the index counts the game's passes");
        CHECK(seen[REPLAY_ATOM_PICKUP] == want[REPLAY_ATOM_PICKUP], "the index counts the game's pickups");
        // ...and the game has to contain the moves that make those checks mean
        // something. A 2p game passes rarely, so this only insists on the two
        // every Durak game has; the pass-bearing case is covered at 3p+ below.
        CHECK(want[REPLAY_ATOM_ATTACK] > 0, "the seeded game attacks");
        CHECK(want[REPLAY_ATOM_COVER] > 0, "the seeded game covers");
    }
}

// The counts above only bite on passes if some seeded game actually passes.
// Find one, so a pass-collapsing index cannot hide behind a pass-free game.
static void test_replay_step_index_tells_a_pass_from_an_attack(void) {
    static unsigned char code[1 << 20];
    static unsigned char idx[4096];

    bool found_pass = false;
    for (int np = 3; np <= 4 && !found_pass; np++) {
        for (int s = 0; s < 40 && !found_pass; s++) {
            Game g;
            unsigned char seed[FOOLISH_SEED_LEN];
            if (!rs_play_seeded(&g, np, 900 + s, seed)) continue;

            int passes = 0, attacks = 0;
            for (int i = 0; i < g.num_logs; i++) {
                if (g.logs[i].log_type == LOG_PASS) passes++;
                if (g.logs[i].log_type == LOG_ATTACK) attacks++;
            }
            if (passes == 0) continue;
            found_pass = true;

            int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                                 code, (int)sizeof code);
            CHECK(enc > 0, "the passing game encodes as v6");
            if (enc <= 0) return;
            const int steps = replay_steps_count_v6(code, enc, 0);
            int len = replay_steps_index_v6(code, enc, 0, idx, (int)sizeof idx);
            CHECK(len == steps * RS_INDEX_STRIDE, "one index record per step");
            if (len != steps * RS_INDEX_STRIDE) return;

            int ip = 0, ia = 0;
            for (int i = 1; i < steps; i++) {
                if (idx[i * RS_INDEX_STRIDE] == REPLAY_ATOM_PASS) ip++;
                if (idx[i * RS_INDEX_STRIDE] == REPLAY_ATOM_ATTACK) ia++;
            }
            CHECK(ip == passes, "a pass reports as a pass, not an attack");
            CHECK(ia == attacks, "and the attacks stay attacks");
        }
    }
    CHECK(found_pass, "found a seeded game containing a pass");
}

// A deal where NOBODY holds a trump has no derivable opening seat: the engine
// rolls for it (determine_lowest_power_index -> deal_index), and that roll is
// not recorded in a replay code. So a replay must take the seat from its header
// on that branch, or it rebuilds the right hands and then picks a different
// opening seat at random.
//
// That was a real break, not a hypothetical: the replay refused the game with
// REPLAY_EHEADER — whose message says "trump not in alphabet", a different fault
// entirely — on ~1.4% of 2p deals (12 cards from 36, 9 of them trumps), and
// whether it refused depended on the RNG state, so it was flaky too. The game
// encoded and decoded perfectly; only its replay was unrenderable.
static void test_replay_steps_replays_a_deal_with_no_trump(void) {
    static unsigned char code[1 << 20];
    unsigned char seed[FOOLISH_SEED_LEN];

    // Find a seeded 2p deal with no trump in either hand. The search MUST use
    // rs_play_seeded's own seed derivation, or the game found is not the game
    // played — which is how the first cut of this test came to pass against the
    // bug it was written to catch.
    int found = -1;
    for (int s = 0; s < 600 && found < 0; s++) {
        for (int i = 0; i < FOOLISH_SEED_LEN; i++)
            seed[i] = (unsigned char)(i * 31 + s * 13 + 2);
        game_set_seed((uint32_t)(s + 1));
        random_strategy_set_seed((uint32_t)(s + 1));
        game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);

        Game d;
        memset(&d, 0, sizeof d);
        d.num_players = 2;
        for (int i = 0; i < 2; i++) d.players[i].status = PLAYER_STATUS_READY;
        start_game(&d);

        int trumps = 0;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < d.players[i].hand_count; j++)
                if (d.players[i].hand[j].suit == d.power_suit) trumps++;
        if (trumps == 0) found = s;
    }
    // The seed search above is what pins this to the branch under test: no
    // trump was dealt, so the opening seat was rolled for, not derived. (It
    // cannot be re-checked off `g` below - that game is played out, and every
    // hand in it is empty.)
    CHECK(found >= 0, "found a 2p deal with no trump in either hand");
    if (found < 0) return;

    Game g;
    if (!rs_play_seeded(&g, 2, found, seed)) { CHECK(0, "the no-trump game plays out"); return; }

    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    CHECK(enc > 0, "a no-trump game still encodes");
    if (enc <= 0) return;

    // The point: it must REPLAY, and replay the same way whatever the RNG is
    // sitting on. Without the override the rebuilt deal re-rolls the opening
    // seat and the replay refuses the game outright (-REPLAY_EHEADER), on a
    // coin flip.
    int first = -1;
    for (int trial = 0; trial < 8; trial++) {
        game_set_seed((uint32_t)(trial * 7919 + 13));
        random_strategy_set_seed((uint32_t)(trial * 104729 + 7));
        int steps = replay_steps_count_v6(code, enc, 0);
        CHECK(steps > 0, "a no-trump game's replay rebuilds its deal (was REPLAY_EHEADER)");
        if (steps <= 0) return;
        if (trial == 0) first = steps;
        else CHECK(steps == first, "and rebuilds it the same way whatever the RNG says");
    }
}

// A buffer too small must say so, not write past it or report a short index as
// a complete one.
static void test_replay_step_index_refuses_a_small_buffer(void) {
    static unsigned char code[1 << 20];
    unsigned char idx[8];

    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 3, 503, seed)) { CHECK(0, "seeded game plays out"); return; }
    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    if (enc <= 0) { CHECK(0, "the played game encodes as v6"); return; }

    unsigned char canary = 0xAB;
    (void)canary;
    int r = replay_steps_index_v6(code, enc, 0, idx, (int)sizeof idx);
    CHECK(r < 0, "an index that does not fit is an error, not a truncation");
}

// ---------- json_out (A8/F7): packed bytes -> JSON --------------------------
//
// The web used to read view.c and evwire.c with hand-written TypeScript that
// shadowed them byte for byte. These pin the C that replaced it. The invariant
// that matters is NOT "the JSON looks plausible" — it is that decoding the
// packed bytes says exactly what the live board says, because that equality is
// the entire reason the mirror could be deleted.

// Identity is deliberately not in the state blob (game.h), so a from-packed
// decode cannot know names or strategy keys and emits ""/0. Strip them from the
// live game so the two sides are comparable on what the blob actually carries.
static void jt_strip_identity(Game *g) {
    for (int i = 0; i < g->num_players; i++) {
        g->players[i].name[0] = 0;
        g->players[i].strategy_key = 0;
    }
}

// A packed masked view must decode to the SAME JSON the live game emits, for
// every viewer and for the spectator. This is the web's whole decode path in one
// assertion: if these two ever disagree, the browser is drawing a board the
// kernel does not have.
static void test_json_view_from_packed_says_what_the_live_board_says(void) {
    static unsigned char blob[65536];
    static char from_packed[65536], from_live[65536];

    for (int np = 2; np <= 6; np++) {
        unsigned char seed[FOOLISH_SEED_LEN];
        for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (unsigned char)(i * 17 + np * 3 + 1);
        game_set_seed((uint32_t)(np * 101 + 7));
        random_strategy_set_seed((uint32_t)(np * 101 + 7));
        game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);

        Game g;
        memset(&g, 0, sizeof g);
        g.num_players = (int8_t)np;
        for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
        start_game(&g);
        jt_strip_identity(&g);

        // Every seat, plus the spectator.
        for (int viewer = -1; viewer < np; viewer++) {
            const int v = (viewer < 0) ? VIEW_SPECTATOR : viewer;
            int n = state_put(&g, v, blob);
            CHECK(n > 0, "the board serializes to a masked blob");

            int a = json_view_from_packed(blob, n, v, from_packed, (int)sizeof from_packed);
            int b = json_state_of(&g, v, from_live, (int)sizeof from_live);
            CHECK(a > 0 && b > 0, "both sides emit JSON");
            if (a <= 0 || b <= 0) return;
            CHECK(a == b && memcmp(from_packed, from_live, (size_t)a) == 0,
                  "a packed view decodes to exactly the live board's JSON");
        }
    }
}

// The masking survives the round trip. A spectator's decoded JSON must carry no
// hand at all: json_state emits "hand":null for every seat that is not the
// viewer, so a real card array anywhere in a spectator decode is a leak.
static void test_json_view_from_packed_leaks_no_hand_to_a_spectator(void) {
    static unsigned char blob[65536];
    static char out[65536];

    unsigned char seed[FOOLISH_SEED_LEN];
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed[i] = (unsigned char)(i * 5 + 41);
    game_set_seed(4242);
    random_strategy_set_seed(4242);
    game_set_deal_seed_bytes(seed, FOOLISH_SEED_LEN);

    Game g;
    memset(&g, 0, sizeof g);
    g.num_players = 4;
    for (int i = 0; i < 4; i++) g.players[i].status = PLAYER_STATUS_READY;
    start_game(&g);

    int n = state_put(&g, VIEW_SPECTATOR, blob);
    int r = json_view_from_packed(blob, n, VIEW_SPECTATOR, out, (int)sizeof out);
    CHECK(r > 0, "a spectator view decodes");
    if (r <= 0) return;
    CHECK(strstr(out, "\"hand\":[") == NULL,
          "a spectator decode contains no hand array — every seat is a count");
    CHECK(strstr(out, "\"hand\":null") != NULL, "and the seats say so explicitly");

    // The viewer's own seat, by contrast, must show its real cards — otherwise
    // the assertion above would pass on a decoder that simply emits nothing.
    n = state_put(&g, 1, blob);
    r = json_view_from_packed(blob, n, 1, out, (int)sizeof out);
    CHECK(r > 0 && strstr(out, "\"hand\":[") != NULL,
          "but seat 1's own decode shows seat 1's real hand");
}

// The packed evwire reader against the real thing: frames the kernel actually
// serialized, from a game the engine actually played. The trailer is the anchor
// — it must be the same final board the live game emits — because a reader that
// drifts by one byte anywhere in the event loop lands the trailer somewhere else
// and cannot reproduce it.
static void test_json_events_from_packed_reads_the_frames_the_kernel_wrote(void) {
    static unsigned char code[1 << 20];
    static unsigned char frames[1 << 18];
    static char out[1 << 18];

    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 3, 71, seed)) { CHECK(0, "seeded game plays out"); return; }
    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    if (enc <= 0) { CHECK(0, "the played game encodes as v6"); return; }

    int n_frames = 0, next = 0;
    int wrote = replay_steps_frames_v6(code, enc, VIEW_SPECTATOR, 0, 0,
                                       frames, (int)sizeof frames, &n_frames, &next);
    CHECK(wrote > 0 && n_frames > 0, "the code replays to packed evwire frames");
    if (wrote <= 0 || n_frames <= 0) return;

    int q = 0, decoded = 0, with_state = 0;
    for (int f = 0; f < n_frames; f++) {
        const int flen = frames[q] | (frames[q + 1] << 8); q += 2;
        int r = json_events_from_packed(frames + q, flen, out, (int)sizeof out);
        CHECK(r > 0, "every frame the kernel wrote, the kernel reads back as JSON");
        if (r <= 0) return;
        // A spectator frame: viewer -1, and the sequence carries its trailer.
        CHECK(strstr(out, "\"viewer\":-1") == out + 1, "the frame's viewer survives the header");
        CHECK(strstr(out, "\"game\":") != NULL, "and the committed final board rides as the trailer");
        // Per-event boards are the whole point of the format (A3/§16.B4).
        if (strstr(out, "\"state\":") != NULL) with_state++;
        q += flen;
        decoded++;
    }
    CHECK(decoded == n_frames, "every frame decoded");
    CHECK(with_state > 0, "and the events carry their per-step boards");
}

// The trailer is the real anchor: decode the LAST frame of a finished game and
// insist its `game` is byte-identical to the JSON the finished game itself
// emits. This is what makes the test above more than a shape check — it pins the
// reader's cursor arithmetic to a value it cannot fake.
static void test_json_events_trailer_is_the_board_the_engine_ended_on(void) {
    static unsigned char code[1 << 20];
    static unsigned char frames[1 << 18];
    static char out[1 << 18];
    static char live[65536];

    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 3, 71, seed)) { CHECK(0, "seeded game plays out"); return; }
    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    if (enc <= 0) { CHECK(0, "the played game encodes as v6"); return; }

    const int steps = replay_steps_count_v6(code, enc, 0);
    CHECK(steps > 0, "the code reports its step count");
    if (steps <= 0) return;

    // Pull the final step's frame.
    int n_frames = 0, next = 0;
    int wrote = replay_steps_frames_v6(code, enc, VIEW_SPECTATOR, steps - 1, 0,
                                       frames, (int)sizeof frames, &n_frames, &next);
    if (wrote <= 0 || n_frames < 1) { CHECK(0, "the last step serializes"); return; }

    const int flen = frames[0] | (frames[1] << 8);
    int r = json_events_from_packed(frames + 2, flen, out, (int)sizeof out);
    CHECK(r > 0, "the last frame decodes");
    if (r <= 0) return;

    jt_strip_identity(&g);
    int b = json_state_of(&g, VIEW_SPECTATOR, live, (int)sizeof live);
    CHECK(b > 0, "the finished game emits its board");
    if (b <= 0) return;

    const char *trailer = strstr(out, "\"game\":");
    CHECK(trailer != NULL, "the decode has a trailer");
    if (!trailer) return;
    trailer += 7; // past "game":
    CHECK(strncmp(trailer, live, (size_t)b) == 0,
          "the trailer is byte-identical to the board the engine actually ended on");
}

// A truncated or foreign payload is UNREADABLE, never a partial parse. The web
// treats a null decode as "cannot read this", and half a sequence rendered as a
// whole one would be worse than no sequence at all.
static void test_json_events_refuses_a_truncated_or_foreign_payload(void) {
    static unsigned char code[1 << 20];
    static unsigned char frames[1 << 18];
    static char out[1 << 18];

    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 3, 71, seed)) { CHECK(0, "seeded game plays out"); return; }
    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    if (enc <= 0) { CHECK(0, "the played game encodes as v6"); return; }

    int n_frames = 0, next = 0;
    int wrote = replay_steps_frames_v6(code, enc, VIEW_SPECTATOR, 0, 0,
                                       frames, (int)sizeof frames, &n_frames, &next);
    if (wrote <= 0 || n_frames < 1) { CHECK(0, "frames serialize"); return; }
    const int flen = frames[0] | (frames[1] << 8);
    unsigned char *frame = frames + 2;

    CHECK(json_events_from_packed(frame, flen, out, (int)sizeof out) > 0,
          "the whole frame reads (the control)");

    // A foreign format version.
    unsigned char v = frame[0];
    frame[0] = (unsigned char)(v + 7);
    CHECK(json_events_from_packed(frame, flen, out, (int)sizeof out) == JSON_EPARSE,
          "a foreign format version is refused, not guessed at");
    frame[0] = v;

    // Every truncation. Not one sampled length — a reader whose bounds check is
    // wrong is usually wrong at one specific offset.
    //
    // Each prefix is copied into an EXACTLY-sized heap buffer, which is the only
    // way this proves anything. Handing the reader `frame` with a short `len`
    // leaves the real bytes sitting right after it, so an over-read walks into
    // the next frame, stays inside the array, and every bound here passes while
    // the reader is in fact reading memory it was not given. Measured: deleting
    // a bounds check does not fail this test on a static buffer. Sized to the
    // byte, an over-read is a heap overflow — caught under ASAN
    // (docs/SECURITY_WASM_BOUNDARY.md is the standard this holds the reader to),
    // and the frames the browser hands us are exactly-sized too.
    for (int len = 1; len < flen; len++) {
        unsigned char *exact = (unsigned char *)malloc((size_t)len);
        CHECK(exact != NULL, "the truncation probe allocates");
        if (!exact) return;
        memcpy(exact, frame, (size_t)len);
        int r = json_events_from_packed(exact, len, out, (int)sizeof out);
        free(exact);
        CHECK(r < 0, "a truncated sequence is unreadable, never a partial parse");
        if (r >= 0) return;
    }

    // And a cap too small to hold the answer says so rather than emitting JSON
    // that would parse as a smaller, wrong sequence.
    char tiny[32];
    CHECK(json_events_from_packed(frame, flen, tiny, (int)sizeof tiny) == JSON_ECAP,
          "an output buffer too small is an error, not truncated JSON");
}


// ---------- the packed evwire READER ----------------------------------------
//
// MUTATION-CHECKED, each mutation applied to c/src/evwire.c on its own:
//
//   dropping EVW_T_CARDS_TO_TRASH from evw_is_settlement  -> 1 failure
//   frame walk reads its length prefix at p + 1           -> 8 failures
//   the frame length bound loosened to `p + flen > len+1` -> 1 failure
//   the cut latches the LAST settlement, not the first    -> 4 failures
//   the per-event snapshot bound removed                  -> 1 failure
//   the card-list/tail bound removed                      -> the binary faults
//   the event-header bound cut to `q + 6 > len`           -> the binary faults
//   the trailer length bound removed                      -> the binary faults
//
// The last three are memory-safety bounds: without them evwire_read still
// RETURNS an error, it just reads past the caller's buffer on the way. They are
// only checkable because the probes below sit against a guard page.

// Every event the writer's own derivation produced, flattened, so the reader can
// be held against it rather than against a second copy of the layout.
#define EVR_MAX_EVENTS 4096
typedef struct {
    int n;
    int type[EVR_MAX_EVENTS], seat[EVR_MAX_EVENTS], msg[EVR_MAX_EVENTS];
    int from[EVR_MAX_EVENTS], to[EVR_MAX_EVENTS];
    int has_target[EVR_MAX_EVENTS], has_battle[EVR_MAX_EVENTS];
    unsigned char target[EVR_MAX_EVENTS];
    int battle[EVR_MAX_EVENTS];
    int n_cards[EVR_MAX_EVENTS];
    unsigned char cards[EVR_MAX_EVENTS][MAX_LOG_PAIRS];
    int snap_len[EVR_MAX_EVENTS];
    int base;       // events already read from earlier frames
    int mismatch;
    // The buffer the reader was handed. Every borrowed pointer it passes on
    // must land inside it - a bound that only stops the RETURN VALUE going
    // wrong still lets a sink dereference bytes nobody gave it.
    const unsigned char *buf;
    int len;
    int escaped;
} EvrLog;

static EvrLog g_evr_walked;   // what evwire_walk derived
static EvrLog g_evr_read;     // what evwire_read got back off the wire

// The walk side: record the derived event in WIRE terms, so the comparison is
// against the format's own vocabulary and not against a decode of it.
static void evr_walk_sink(void *ctx, const EvwEvent *ev) {
    EvrLog *l = (EvrLog *)ctx;
    if (l->n >= EVR_MAX_EVENTS) return;
    const int i = l->n++;
    l->type[i] = ev->type; l->seat[i] = ev->seat; l->msg[i] = ev->msg;
    l->from[i] = ev->from; l->to[i] = ev->to;
    l->has_target[i] = ev->has_target;
    l->target[i] = ev->has_target ? wire_from_card(ev->target) : 0;
    l->has_battle[i] = ev->has_battle;
    l->battle[i] = ev->has_battle ? ev->battle : 0;
    l->n_cards[i] = ev->n_cards;
    for (int c = 0; c < ev->n_cards && c < MAX_LOG_PAIRS; c++)
        l->cards[i][c] = ev->mask_cards ? (unsigned char)WIRE_CARD_HIDDEN
                                        : wire_from_card(ev->cards[c]);
    l->snap_len[i] = 0;
}

static void evr_read_sink(void *ctx, int index, const EvwRead *ev) {
    EvrLog *l = (EvrLog *)ctx;
    if (l->n >= EVR_MAX_EVENTS) return;
    const int i = l->n++;
    // The index is the event's place in ITS OWN sequence, so it restarts per frame.
    if (index != i - l->base) l->mismatch = 1;
    l->type[i] = ev->type; l->seat[i] = ev->seat; l->msg[i] = ev->msg;
    l->from[i] = ev->from; l->to[i] = ev->to;
    l->has_target[i] = ev->has_target;
    l->target[i] = ev->has_target ? ev->target_wire : 0;
    l->has_battle[i] = ev->has_battle;
    l->battle[i] = ev->has_battle ? ev->battle : 0;
    l->n_cards[i] = ev->n_cards;
    if (l->buf) {
        const unsigned char *end = l->buf + l->len;
        if (ev->cards_wire < l->buf || ev->cards_wire + ev->n_cards > end) { l->escaped = 1; return; }
        if (ev->snap_len < 0 || ev->snap < l->buf || ev->snap + ev->snap_len > end) { l->escaped = 1; return; }
    }
    for (int c = 0; c < ev->n_cards && c < MAX_LOG_PAIRS; c++)
        l->cards[i][c] = ev->cards_wire[c];
    l->snap_len[i] = ev->snap_len;
}

// Serialize a real game's whole event stream, read it back, and insist the two
// agree event for event. The reader is only worth having if it recovers what the
// writer put down, and the only honest reference for that is the derivation
// itself (evwire_walk) rather than a second reader.
static void test_evwire_read_round_trips_the_writers_own_events(void) {
    static unsigned char code[1 << 20];
    static unsigned char frames[1 << 18];

    for (int np = 2; np <= 4; np++) {
        Game g;
        unsigned char seed[FOOLISH_SEED_LEN];
        if (!rs_play_seeded(&g, np, 900 + np, seed)) { CHECK(0, "seeded game plays out"); continue; }
        int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                             code, (int)sizeof code);
        if (enc <= 0) { CHECK(0, "the played game encodes as v6"); continue; }

        // Viewer 0, not a spectator: it is the only setting in which the
        // DEAL/REFILL redaction differs per event, so a reader that dropped the
        // masking distinction would show up here.
        memset(&g_evr_walked, 0, sizeof g_evr_walked);
        int rc = replay_steps_v6(code, enc, 0, 0, evr_walk_sink, &g_evr_walked);
        CHECK(rc == REPLAY_EOK, "the code replays to the derivation");
        if (rc != REPLAY_EOK) continue;

        memset(&g_evr_read, 0, sizeof g_evr_read);
        int from = 0, guard = 0;
        const int steps = replay_steps_count_v6(code, enc, 0);
        while (from < steps && ++guard < 4096) {
            int n = 0, next = from;
            int len = replay_steps_frames_v6(code, enc, 0, from, 0,
                                             frames, (int)sizeof frames, &n, &next);
            if (len < 0 || next <= from) { CHECK(0, "a frame chunk serializes"); break; }
            int q = 0;
            for (int i = 0; i < n; i++) {
                const int flen = frames[q] | (frames[q + 1] << 8); q += 2;
                const unsigned char *fin = 0;
                int fin_len = -1;
                EvwHeader h;
                g_evr_read.base = g_evr_read.n;
                g_evr_read.buf = frames + q; g_evr_read.len = flen;
                int got = evwire_read(frames + q, flen, &h, &fin, &fin_len,
                                      evr_read_sink, &g_evr_read);
                CHECK(got >= 0, "every frame the writer wrote, the reader reads");
                CHECK(h.version == EVWIRE_FORMAT_VERSION && h.viewer == 0,
                      "and the header comes back as it was written");
                CHECK(fin != NULL && fin_len >= 0 && fin + fin_len <= frames + q + flen,
                      "the trailer is inside the frame the reader was given");
                q += flen;
            }
            from = next;
        }

            CHECK(g_evr_read.escaped == 0, "no borrowed pointer left the buffer the reader was given");
    CHECK(g_evr_read.mismatch == 0, "the sink indexes each sequence's events in wire order");
        CHECK(g_evr_read.n == g_evr_walked.n && g_evr_read.n > 0,
              "the reader recovers every event the derivation produced");
        if (g_evr_read.n != g_evr_walked.n) continue;

        int same = 1, snaps = 0;
        for (int i = 0; i < g_evr_read.n; i++) {
            const EvrLog *a = &g_evr_walked, *b = &g_evr_read;
            if (a->type[i] != b->type[i] || a->seat[i] != b->seat[i] ||
                a->msg[i] != b->msg[i] || a->from[i] != b->from[i] ||
                a->to[i] != b->to[i] || a->has_target[i] != b->has_target[i] ||
                a->target[i] != b->target[i] ||
                a->has_battle[i] != b->has_battle[i] || a->battle[i] != b->battle[i] ||
                a->n_cards[i] != b->n_cards[i]) { same = 0; break; }
            for (int c = 0; c < a->n_cards[i] && c < MAX_LOG_PAIRS; c++)
                if (a->cards[i][c] != b->cards[i][c]) { same = 0; break; }
            if (!same) break;
            if (b->snap_len[i] > 0) snaps++;
        }
        CHECK(same, "field for field, the wire says what the derivation derived");
        CHECK(snaps == g_evr_read.n, "and every event carries its own board");
    }
}

// THE GUARD PAGE. A short buffer handed to a reader out of a static array proves
// nothing: an over-read walks into the bytes that happen to follow, stays inside
// the array, and every bound in the test passes while the reader is in fact
// reading memory it was not given (measured - deleting a bound does not fail a
// test written that way). A heap copy sized to the byte only catches it under a
// sanitizer, which is not what the default suite runs.
//
// So the probe is placed FLUSH against an unreadable page: the last byte the
// reader may touch is the last byte of a readable page, and the next page is
// PROT_NONE. One byte over the length it was given and the process dies, with no
// tooling and on every host. Three of the reader's four bounds are memory-safety
// bounds - the walk still returns an error without them, it just reads past the
// buffer on the way - and this is what makes them checkable at all.
static unsigned char *jt_guard_base(void) {
    static unsigned char *base = 0;
    static long pg = 0;
    if (!base) {
        pg = sysconf(_SC_PAGESIZE);
        base = (unsigned char *)mmap(0, (size_t)pg * 2, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base == MAP_FAILED) { base = 0; return 0; }
        if (mprotect(base + pg, (size_t)pg, PROT_NONE) != 0) { base = 0; return 0; }
    }
    return base;
}

// Copy `n` bytes so that they END on the last readable byte before the guard.
static const unsigned char *jt_flush_to_guard(const unsigned char *src, int n) {
    unsigned char *base = jt_guard_base();
    const long pg = sysconf(_SC_PAGESIZE);
    if (!base || n < 0 || n > pg) return 0;
    memset(base, 0, (size_t)pg);
    unsigned char *p = base + pg - n;
    if (n) memcpy(p, src, (size_t)n);
    return p;
}

// A sequence that is not whole is UNREADABLE - and is refused without reading a
// single byte past the buffer, which is what the guard page above enforces.
static void test_evwire_read_refuses_a_malformed_sequence(void) {
    static unsigned char code[1 << 20];
    static unsigned char frames[1 << 18];

    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 3, 71, seed)) { CHECK(0, "seeded game plays out"); return; }
    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    if (enc <= 0) { CHECK(0, "the played game encodes as v6"); return; }

    // A step with covers on it: the one shape that exercises the target and
    // battle bytes, so the card-list bound is not the only variable tail here.
    int n_frames = 0, next = 0;
    int wrote = replay_steps_frames_v6(code, enc, VIEW_SPECTATOR, 0, 0,
                                       frames, (int)sizeof frames, &n_frames, &next);
    if (wrote <= 0 || n_frames < 2) { CHECK(0, "frames serialize"); return; }

    // Pick the longest frame in the chunk - the most event tail to truncate.
    int q = 0, best = -1, best_len = 0;
    for (int i = 0; i < n_frames; i++) {
        const int flen = frames[q] | (frames[q + 1] << 8);
        if (flen > best_len) { best_len = flen; best = q + 2; }
        q += 2 + flen;
    }
    if (best < 0) { CHECK(0, "a frame was found"); return; }
    const unsigned char *frame = frames + best;
    const int flen = best_len;

    memset(&g_evr_read, 0, sizeof g_evr_read);
    g_evr_read.buf = frame; g_evr_read.len = flen;
    CHECK(evwire_read(frame, flen, 0, 0, 0, evr_read_sink, &g_evr_read) >= 0,
          "the whole frame reads (the control)");

    // Too short to hold a header at all.
    for (int len = 0; len < 4; len++)
        CHECK(evwire_read(frame, len, 0, 0, 0, 0, 0) == EVW_EBADARG,
              "a buffer with no room for a header is a bad argument");
    CHECK(evwire_read(0, 64, 0, 0, 0, 0, 0) == EVW_EBADARG, "and so is no buffer");

    // A foreign format version.
    {
        static unsigned char copy[1 << 16];
        if (flen > (int)sizeof copy) { CHECK(0, "the version probe fits"); return; }
        memcpy(copy, frame, (size_t)flen);
        copy[0] = (unsigned char)(copy[0] + 7);
        CHECK(evwire_read(copy, flen, 0, 0, 0, 0, 0) == EVW_EPARSE,
              "a foreign format version is refused, not guessed at");
    }

    // Every truncation: a header cut mid-event, a card list cut short, a
    // snapshot cut short, a missing trailer. Against the guard page, so an
    // over-read is a fault rather than a quiet success.
    const int probe_max = (int)sysconf(_SC_PAGESIZE);
    for (int len = 4; len < flen && len <= probe_max; len++) {
        const unsigned char *exact = jt_flush_to_guard(frame, len);
        CHECK(exact != NULL, "the truncation probe maps its guard page");
        if (!exact) return;
        // With the sink attached: an error return is not enough on its own, the
        // events the sink DID see must have pointed inside the buffer.
        memset(&g_evr_read, 0, sizeof g_evr_read);
        g_evr_read.buf = exact; g_evr_read.len = len;
        int r = evwire_read(exact, len, 0, 0, 0, evr_read_sink, &g_evr_read);
        CHECK(r == EVW_EPARSE, "a truncated sequence is unreadable, never a partial parse");
        CHECK(!g_evr_read.escaped, "and hands its sink nothing outside the bytes it was given");
        if (r != EVW_EPARSE || g_evr_read.escaped) return;
    }

    // A LYING SNAPSHOT LENGTH: the bytes are all there, the header is ours, and
    // one event claims a board bigger than the frame. Truncation alone would
    // never produce this, and it is exactly the shape a hostile payload takes.
    {
        // Find the first event's snap_len field: 7 fixed bytes, then the tail.
        const int n_cards = frame[4 + 6];
        const int flags = frame[4 + 5];
        int sp = 4 + 7 + n_cards + ((flags & 1) ? 1 : 0) + ((flags & 2) ? 1 : 0);
        CHECK(sp + 2 <= flen, "the first event's snapshot length is inside the frame");
        if (sp + 2 > flen) return;
        static unsigned char copy[1 << 16];
        if (flen > (int)sizeof copy || flen > probe_max) { CHECK(0, "the lying-length probe fits"); return; }
        memcpy(copy, frame, (size_t)flen);
        copy[sp] = 0xff; copy[sp + 1] = 0xff;   // 65535 bytes of board
        const unsigned char *g = jt_flush_to_guard(copy, flen);
        CHECK(g != NULL, "the lying-length probe maps its guard page");
        if (!g) return;
        memset(&g_evr_read, 0, sizeof g_evr_read);
        g_evr_read.buf = g; g_evr_read.len = flen;
        int r = evwire_read(g, flen, 0, 0, 0, evr_read_sink, &g_evr_read);
        const int escaped = g_evr_read.escaped;
        CHECK(r == EVW_EPARSE, "a snapshot that claims more bytes than it has is refused");
        // The event is refused BEFORE the sink sees it. Refusing only at the end
        // of the walk would still have handed a decoder a pointer past the
        // buffer and a length to read through it - the return value is not the
        // only thing a bound protects.
        CHECK(!escaped, "and is never handed on as a borrowed pointer past the end");
    }

    // A LYING EVENT COUNT: one more event than the bytes hold. The trailer is
    // read as an event header and the walk runs off the end.
    {
        static unsigned char copy[1 << 16];
        if (flen > (int)sizeof copy || flen > probe_max) { CHECK(0, "the lying-count probe fits"); return; }
        memcpy(copy, frame, (size_t)flen);
        copy[3] = 255;
        const unsigned char *g = jt_flush_to_guard(copy, flen);
        CHECK(g != NULL, "the lying-count probe maps its guard page");
        if (!g) return;
        memset(&g_evr_read, 0, sizeof g_evr_read);
        g_evr_read.buf = g; g_evr_read.len = flen;
        int r = evwire_read(g, flen, 0, 0, 0, evr_read_sink, &g_evr_read);
        CHECK(r == EVW_EPARSE, "an event count the bytes cannot back is refused");
        CHECK(!g_evr_read.escaped, "and never walks its sink off the end getting there");
    }
}

// ---------- the settlement cut ----------------------------------------------

// Build a frame stream by hand: one event type per event, no cards, no board.
// The cut is a question about TYPES and ORDER across frames, so a stream that
// carries nothing else is the sharpest way to ask it - and it is the same shape
// the Swift tests pin (ios/FoolishTests/MessageStagedDealTests.swift).
static int evc_frames(const int *const *frames, const int *sizes, int n_frames,
                      unsigned char *out) {
    int w = 0;
    for (int f = 0; f < n_frames; f++) {
        const int n = sizes[f];
        const int flen = 4 + n * 9 + 2;   // header + events (7 fixed + u16 zero snap) + trailer
        out[w++] = (unsigned char)(flen & 0xff);
        out[w++] = (unsigned char)((flen >> 8) & 0xff);
        out[w++] = EVWIRE_FORMAT_VERSION;
        out[w++] = 0;                     // viewer
        out[w++] = 0;                     // actor
        out[w++] = (unsigned char)n;
        for (int i = 0; i < n; i++) {
            out[w++] = (unsigned char)frames[f][i];  // type
            out[w++] = 1;                            // seat
            out[w++] = 0;                            // msg
            out[w++] = EVW_LOC_HAND;
            out[w++] = EVW_LOC_TABLE;
            out[w++] = 0;                            // flags
            out[w++] = 0;                            // n_cards
            out[w++] = 0; out[w++] = 0;              // snap_len 0
        }
        out[w++] = 0; out[w++] = 0;                  // final_len 0
    }
    return w;
}

static int evc_one(const int *types, int n, unsigned char *out) {
    const int *fr[1] = { types };
    const int sz[1] = { n };
    return evc_frames(fr, sz, 1, out);
}

// The seven shapes the extension's contract is written in. A cover that swept
// the table, a double cover, a pickup, a good, and the three that settle
// nothing. Same expectations as MessageStagedDealTests.testTheSettlementCut.
static void test_evwire_frames_settlement_cut(void) {
    static unsigned char buf[4096];

    { const int t[] = { EVW_T_COVER, EVW_T_CARDS_TO_TRASH, EVW_T_REFILL };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 3, buf)) == 1,
            "a cover that swept the table settles at the sweep"); }
    { const int t[] = { EVW_T_COVER, EVW_T_COVER, EVW_T_DISCARD, EVW_T_REFILL };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 4, buf)) == 2,
            "both covers are the player's; the discard is not"); }
    { const int t[] = { EVW_T_PICKUP, EVW_T_REFILL, EVW_T_DEFENDER_MOVE };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 3, buf)) == 1,
            "the sweep into the hand is the move; the refill is not"); }
    { const int t[] = { EVW_T_MAGIC_TRANSITION, EVW_T_DISCARD, EVW_T_REFILL };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 3, buf)) == 0,
            "a good emits no step of its own, so the whole stream is the bout end"); }
    { const int t[] = { EVW_T_ATTACK_PASS };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 1, buf)) == -1,
            "ordinary play settles nothing"); }
    { const int t[] = { EVW_T_COVER, EVW_T_OUT };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 2, buf)) == -1,
            "an OUT with no settlement behind it is not one"); }
    { const int t[] = { EVW_T_ATTACK_PASS };
      CHECK(evwire_frames_settlement_cut(buf, evc_one(t, 0, buf)) == -1,
            "an empty sequence settles nothing"); (void)t; }
    CHECK(evwire_frames_settlement_cut(buf, 0) == -1,
          "and so does an empty stream - a turn that animates nothing ended no bout");

    // ACROSS FRAMES, which is the whole reason this is in C. A staged turn is
    // one frame per action, the clients flatten them, and the cut indexes into
    // the flattened list - so the second frame's settlement is at 2, not at 0.
    { const int a[] = { EVW_T_ATTACK_PASS };
      const int b[] = { EVW_T_COVER, EVW_T_DISCARD, EVW_T_REFILL };
      const int *fr[2] = { a, b };
      const int sz[2] = { 1, 3 };
      CHECK(evwire_frames_settlement_cut(buf, evc_frames(fr, sz, 2, buf)) == 2,
            "the cut counts across frames, not within one"); }
    { const int a[] = { EVW_T_COVER, EVW_T_DISCARD };
      const int b[] = { EVW_T_REFILL };
      const int *fr[2] = { a, b };
      const int sz[2] = { 2, 1 };
      CHECK(evwire_frames_settlement_cut(buf, evc_frames(fr, sz, 2, buf)) == 1,
            "a settlement in the first frame is still the first settlement"); }

    // A stream that is not whole is an ERROR, below -1, and never the -1 that
    // means "no bout ended" - the two must not be confusable, or a corrupt turn
    // would silently animate its own deal.
    {
      const int t[] = { EVW_T_COVER, EVW_T_DISCARD };
      const int n = evc_one(t, 2, buf);
      for (int len = 1; len < n; len++) {
          const unsigned char *exact = jt_flush_to_guard(buf, len);
          CHECK(exact != NULL, "the frame truncation probe maps its guard page");
          if (!exact) return;
          int r = evwire_frames_settlement_cut(exact, len);
          CHECK(r < -1, "a truncated frame stream is an error, not 'no settlement'");
          if (r >= -1) return;
      }
      // Slack after the last whole frame is a malformed stream too: the walk
      // must consume the buffer exactly.
      memcpy(buf + n, "\x00", 1);
      CHECK(evwire_frames_settlement_cut(buf, n + 1) < -1,
            "a trailing byte after the last frame is not a stream we can read");
    }
}

// Play until an attacker has said good and the bout is still open — a state
// only reachable with 3+ players (heads-up, the one attacker's good always
// closes the round). The game is left exactly there, logs ending on the GOOD,
// which is what a device mid-turn actually holds.
static bool rs_play_until_pending_good(Game *g, int np, int seed, unsigned char *seed_out) {
    for (int i = 0; i < FOOLISH_SEED_LEN; i++)
        seed_out[i] = (unsigned char)(i * 31 + seed * 13 + np);
    game_set_seed((uint32_t)(seed + 1));
    random_strategy_set_seed((uint32_t)(seed + 1));
    game_set_deal_seed_bytes(seed_out, FOOLISH_SEED_LEN);

    memset(g, 0, sizeof(*g));
    g->num_players = (int8_t)np;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    start_game(g);

    static LegalMoves moves;
    for (int guard = 0; guard < 20000 && game_done(g) < 0; guard++) {
        bool acted = false;
        for (int pi = 0; pi < np && !acted; pi++) {
            if (!should_bot_act(g, pi)) continue;
            calculate_legal_moves(g, pi, &moves);
            if (moves.n == 0) continue;
            const LegalMove *m = &moves.moves[handwritten_strategy_choose(g, pi, &moves, 0)];
            switch (m->type) {
                case MOVE_ATTACK: acted = handle_attack(g, pi, m->cards, m->n_cards); break;
                case MOVE_COVER:  acted = handle_cover(g, pi, m->cards, m->attack_cards, m->n_cards); break;
                case MOVE_PASS:   acted = handle_pass(g, pi, m->cards, m->n_cards); break;
                case MOVE_PICKUP: acted = handle_pickup(g, pi); break;
                case MOVE_GOOD:   acted = handle_good(g, pi); break;
                default: break;
            }
        }
        if (!acted) return false;
        // A good that did not trigger the transition — stop on it.
        if (g->good_players_mask != 0) return true;
    }
    return false;
}

// A pending good is state the engine holds (good_players_mask) that the action
// stream cannot re-derive: it is cleared by every other handler, so the only
// place one is live is the END of a cut stream — and there it is 47% of 4p
// states (docs/IMESSAGE_BODY_CODEC.md §3). v6 codes it as its own atom.
static void test_replay_v6_carries_a_pending_good(void) {
    static unsigned char code[1 << 20];
    static RsTestCtx ctx;

    int covered = 0;
    for (int np = 3; np <= 6; np++) {
        Game g;
        unsigned char seed[FOOLISH_SEED_LEN];
        if (!rs_play_until_pending_good(&g, np, 1300 + np, seed)) continue;
        covered++;

        int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                             code, (int)sizeof code);
        CHECK(enc > 0, "a game paused on a pending good encodes as v6");
        if (enc <= 0) continue;

        memset(&ctx, 0, sizeof ctx);
        int r = replay_steps_v6(code, enc, VIEW_UNMASKED, 0, rs_test_sink, &ctx);
        CHECK(r == REPLAY_EOK, "a pending-good cut replays through the engine");
        if (r != REPLAY_EOK) continue;

        // The invariant the atom exists for.
        const Game *back = replay_steps_last_game();
        CHECK(back->good_players_mask == g.good_players_mask,
              "the rebuilt game holds the same pending goods");
        CHECK(back->num_battles == g.num_battles,
              "the rebuilt game's bout is still open on the same table");
        rs_check_conservation(&ctx, np, "a pending-good replay conserves the deck");
    }
    CHECK(covered > 0, "some seat count reaches a pending good");
}

static void test_replay_steps_rebuilds_the_played_game(void) {
    static unsigned char code[1 << 20];
    static RsTestCtx ctx;
    static unsigned char want[8192];

    for (int np = 2; np <= 6; np++) {
        Game g;
        unsigned char seed[FOOLISH_SEED_LEN];
        if (!rs_play_seeded(&g, np, 900 + np, seed)) { CHECK(0, "seeded game plays out"); continue; }

        // The truth to beat: the board the engine really finished on.
        const int want_len = state_put(&g, VIEW_UNMASKED, want);
        const int want_fool = game_done(&g);
        Card want_hand[MAX_PLAYERS][MAX_HAND_SIZE];
        int  want_hand_n[MAX_PLAYERS];
        for (int s = 0; s < np; s++) want_hand_n[s] = 0;

        int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 30,
                                             code, (int)sizeof code);
        CHECK(enc > 0, "a played game encodes as v6");
        if (enc <= 0) continue;

        memset(&ctx, 0, sizeof ctx);
        ReplayHeader hdr;
        int r = replay_steps_v6(code, enc, VIEW_UNMASKED, &hdr, rs_test_sink, &ctx);
        CHECK(r == REPLAY_EOK, "a v6 code replays through the engine");
        if (r != REPLAY_EOK) continue;

        CHECK(ctx.n_events > 0, "a replay produces animation events");
        CHECK(ctx.n_deals == np, "every seat's opening deal is an event");
        CHECK(hdr.fool == want_fool, "the rebuilt game finds the same fool the code claims");
        // THE assertion: same final board, byte for byte, as the game that was
        // actually played. Unmasked, so hands are compared too and not hidden.
        CHECK(ctx.last_len == want_len && memcmp(ctx.last, want, (size_t)want_len) == 0,
              "the replay's last board is the board the engine finished on");
        (void)want_hand; (void)want_hand_n;
    }
}

// v6's mid-game cut is where the deck's never-drawn tail earns its keep: a
// FINISHED game has drained the stock, so a short deck still ends at
// deck_count 0 and looks right. Cut the stream early and the stock is still on
// the table, where a missing tail is simply a wrong number on screen.
static void test_replay_steps_mid_game_cut_conserves_the_deck(void) {
    static unsigned char code[1 << 20];
    static RsTestCtx ctx;

    for (int np = 2; np <= 6; np++) {
        Game g;
        unsigned char seed[FOOLISH_SEED_LEN];
        if (!rs_play_seeded(&g, np, 700 + np, seed)) { CHECK(0, "seeded game plays out"); continue; }

        // Few enough atoms that the stock cannot have run out.
        int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 6,
                                             code, (int)sizeof code);
        CHECK(enc > 0, "a mid-game cut encodes as v6");
        if (enc <= 0) continue;

        memset(&ctx, 0, sizeof ctx);
        ReplayHeader hdr;
        int r = replay_steps_v6(code, enc, VIEW_UNMASKED, &hdr, rs_test_sink, &ctx);
        CHECK(r == REPLAY_EOK, "a mid-game v6 cut replays through the engine");
        if (r != REPLAY_EOK) continue;

        CHECK(hdr.fool == -1, "a mid-game cut has no fool yet");
        rs_check_conservation(&ctx, np, "a mid-game replay board holds every card in the deck");

        const Game *last = (const Game *)(const void *)ctx.last_snap;
        CHECK(last->deck_count > 0, "a mid-game cut still has stock left to draw");
    }
}

// v5 hides the deal, so its atoms are not a deck — there is nothing to rebuild
// from and the kernel says so rather than inventing hands.
// A 3+ player game where an attacker's good leaves the bout open: the one step
// kind that only exists because v6 keeps a trailing good. It must come back as
// GOOD and name its seat — a ROUND_END here would replay a bout that never
// ended.
static void test_replay_step_index_reports_a_pending_good(void) {
    static unsigned char code[1 << 20];
    static unsigned char idx[4096];

    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_until_pending_good(&g, 3, 77, seed)) { CHECK(0, "reached a pending good"); return; }

    int enc = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 20,
                                         code, (int)sizeof code);
    CHECK(enc > 0, "the cut game encodes as v6");
    if (enc <= 0) return;

    const int steps = replay_steps_count_v6(code, enc, 0);
    int len = replay_steps_index_v6(code, enc, 0, idx, (int)sizeof idx);
    CHECK(len == steps * RS_INDEX_STRIDE, "one index record per step");
    if (len != steps * RS_INDEX_STRIDE) return;

    const int last = (steps - 1) * RS_INDEX_STRIDE;
    CHECK(idx[last] == REPLAY_ATOM_GOOD, "the cut's last step is the pending good");
    CHECK(idx[last + 1] != RS_SEAT_NONE, "and it names the seat that said it");
    CHECK(idx[last + 1] < 3, "which is a real seat");
}

// A game that outran the log buffer has NO code, and the kernel must say so in
// its own words. It used to answer -REPLAY_EINPUT ("malformed encode input"),
// which is the one thing that had not happened: the bytes were fine, the GAME
// was too long. The misnaming cost a red CI job's worth of hunting through the
// codec (e2e/replay_codec.test.ts, ~0.1% of its `random`-bot games), so the
// refusal has a number of its own now and this pins it.
// MUTATION-CHECKED: putting -REPLAY_EINPUT back in replay.c fails this case
// (tests.c CHECK "a full log buffer refuses as ETOOLONG, not EINPUT"), and so
// does relaxing the cap test there to `> MAX_LOGS`.
static void test_replay_v6_refuses_an_overflowed_log(void) {
    static Game g;
    static unsigned char code[1 << 16];
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 4, 4242, seed)) { CHECK(0, "seeded game plays out"); return; }

    // Encodes as played, so whatever the padded game answers next is about the
    // CAP and not about this game.
    int ok = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 30,
                                        code, (int)sizeof code);
    CHECK(ok > 0, "the played game has a v6 code");

    const int played = g.num_logs;
    CHECK(played > 0 && played < MAX_LOGS, "the fixture starts short of the cap");
    // Fill the array the way a longer game does: log_alloc simply stops
    // appending at MAX_LOGS, so the recorded stream is TRUNCATED and the
    // encoder cannot round-trip what was never written down.
    while (g.num_logs < MAX_LOGS) {
        g.logs[g.num_logs] = g.logs[g.num_logs % played];
        g.num_logs++;
    }
    int r = replay_encode_v6_from_game(&g, seed, FOOLISH_SEED_LEN, 1 << 30,
                                       code, (int)sizeof code);
    CHECK(r == -REPLAY_ETOOLONG, "a full log buffer refuses as ETOOLONG, not EINPUT");
}

static void test_replay_steps_refuses_v5(void) {
    static unsigned char v5[1 << 16];
    static unsigned char in[1 << 16];
    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    if (!rs_play_seeded(&g, 4, 4242, seed)) { CHECK(0, "seeded game plays out"); return; }

    // A real v5 code for the same game: version byte 5 through the v5 encoder.
    int fa = replay_first_attacker_from_logs(g.logs, g.num_logs);
    int pos = 0;
    in[pos++] = 4;
    in[pos++] = (unsigned char)card_to_id(g.flipped);
    in[pos++] = (unsigned char)(fa < 0 ? 0 : fa);
    int n_actions = 0, count_at = pos;
    in[pos++] = 0; in[pos++] = 0;
    for (int i = 0; i < g.num_logs; i++) {
        const GameLog *l = &g.logs[i];
        int kind = -1;
        if (l->log_type == LOG_ATTACK) kind = LOG_ATTACK;
        else if (l->log_type == LOG_COVER) kind = LOG_COVER;
        else if (l->log_type == LOG_PASS) kind = LOG_PASS;
        else if (l->log_type == LOG_PICKUP) kind = LOG_PICKUP;
        else continue;
        in[pos++] = (unsigned char)kind;
        in[pos++] = (unsigned char)(l->player_idx < 0 ? 0xFF : l->player_idx);
        in[pos++] = (unsigned char)l->num_pairs;
        for (int p = 0; p < l->num_pairs; p++) {
            in[pos++] = (unsigned char)card_to_id(l->pairs[p].primary);
            in[pos++] = card_is_none(l->pairs[p].target)
                        ? (unsigned char)REPLAY_CARD_NONE
                        : (unsigned char)card_to_id(l->pairs[p].target);
        }
        n_actions++;
    }
    in[count_at] = (unsigned char)(n_actions & 0xff);
    in[count_at + 1] = (unsigned char)((n_actions >> 8) & 0xff);

    int enc = replay_encode(in, pos, v5, (int)sizeof v5);
    if (enc <= 0) return;  // the v5 oracle is frozen; if it will not encode, nothing to assert

    RsTestCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    int r = replay_steps_v6(v5, enc, VIEW_SPECTATOR, 0, rs_test_sink, &ctx);
    CHECK(r == -REPLAY_EVERSION, "a v5 code is refused: it hides the deal");
    CHECK(ctx.n_events == 0, "a refused code renders nothing");
}

/* ---------------- A6: reset to lobby is one transform -------------------- */

static void test_reset_to_lobby(void) {
    Game g;
    unsigned char seed[FOOLISH_SEED_LEN];
    // A real finished game, so the fields being cleared are genuinely dirty.
    CHECK(rs_play_seeded(&g, 4, 5150, seed), "seeded game plays out");
    CHECK(game_done(&g) >= 0, "the game is over before the reset");

    // Seats 1 and 3 are the bots.
    game_reset_to_lobby(&g, (1u << 1) | (1u << 3));

    CHECK(g.status == GAME_STATUS_WAITING, "reset returns the game to the lobby");
    CHECK(g.players[1].status == PLAYER_STATUS_READY, "a bot seat comes back READY");
    CHECK(g.players[3].status == PLAYER_STATUS_READY, "a bot seat comes back READY");
    CHECK(g.players[0].status == PLAYER_STATUS_IDLE, "a human seat comes back IDLE");
    CHECK(g.players[2].status == PLAYER_STATUS_IDLE, "a human seat comes back IDLE");

    for (int i = 0; i < 4; i++) {
        CHECK(g.players[i].hand_count == 0, "every hand is empty in the lobby");
        CHECK(!g.players[i].awaiting_attack, "nobody is awaiting an attack in the lobby");
    }
    CHECK(g.deck_count == 0, "the deck is cleared");
    CHECK(g.discard_pile_length == 0, "the discard is cleared");
    CHECK(!g.has_flipped, "the trump is cleared");
    CHECK(g.power_suit == 0, "the power suit is cleared");
    CHECK(g.first_attacker == 0, "first_attacker is cleared");
    CHECK(g.defender == 0, "defender is cleared");
    CHECK(g.num_battles == 0, "the table is cleared");
    CHECK(g.num_eliminated == 0, "the elimination order is cleared");
    CHECK(g.good_players_mask == 0, "good presses are cleared");
    CHECK(!g.has_good_timestamp, "the good timestamp is cleared");

    // A reset lobby must be dealable again — the point of the rematch.
    start_game(&g);
    CHECK(g.status == GAME_STATUS_PLAYING, "a reset lobby starts a new game");
    for (int i = 0; i < 4; i++)
        CHECK(g.players[i].hand_count == CARDS_PER_PLAYER, "the rematch deals a full hand");
}


// ---------- the menu wire, and the board rules over it ----------------------
//
// What a gesture on a board MEANS (legal.c's play_*), lifted out of Swift
// (ios/FoolishKit/Boards/CardPlay.swift). The menu these read is the client's
// PUBLISHED one, so the cases build menus by hand exactly as that Swift did -
// a menu is an input here, not something re-derived.
//
// MUTATION-CHECKED, each mutation applied to c/src/legal.c on its own:
//
//   play_resolve answers the HAND target instead of refusing it    ->  2 failures
//   the hand guard applies to the defender alone                   ->  1 failure
//   .table auto-covers whenever ANY cover matches, not exactly one ->  1 failure
//   .battle(i) drops the "covers THIS attack" test                 ->  3 failures
//   .battle(i) accepts an already-covered battle                   ->  1 failure
//   best cover takes the FIRST coverable index, not the highest    -> 11 failures
//   best cover drops the trump term from wire_strength             ->  1 failure
//   best cover breaks ties on the LAST index rather than the first -> 20 failures
//   play_can_say_good drops the all-covered test                   ->  4 failures
//   play_can_say_good treats an empty table as fully covered       ->  1 failure
//   play_human_menu keeps `wait`                                   ->  5 failures
//   play_human_menu keeps `good` unconditionally                   ->  2 failures
//   legal_menu_next reads its n_cards byte at q+2 rather than q+1  -> 58 failures
//   legal_menu_write emits the count BEFORE the entries (so a
//     capped write leaves a header claiming moves it never wrote)  ->  1 failure
//   calculate_legal_moves gates MOVE_GOOD on all_covered           ->  4 failures
//
// The last one is the mistake this whole lift could have made: gating GOOD in
// the ENUMERATION rather than on the way to a board. See the note in legal.c.

static LegalMoves g_pm;
static unsigned char g_pm_wire[1 << 16];
static int g_pm_len;
static unsigned char g_pm_table[2 * 64];
static int g_pm_nb;

static void pm_reset(void) { g_pm.n = 0; g_pm_nb = 0; }

// One menu entry. `attacks` may be NULL (anything but a cover); a cover with
// fewer attack cards than cover cards pads with LEGAL_WIRE_NONE, which is what
// a menu that names one attack for a two-card selection looks like on the wire.
static void pm_add(int type, const Card *cards, int n, const Card *attacks, int n_att) {
    if (g_pm.n >= MAX_LEGAL_MOVES || n > MAX_MOVE_CARDS) { CHECK(0, "menu fixture overflowed"); return; }
    LegalMove *m = &g_pm.moves[g_pm.n++];
    m->type = (int8_t)type;
    m->n_cards = (int8_t)n;
    for (int i = 0; i < n; i++) {
        m->cards[i] = cards[i];
        m->attack_cards[i] = (attacks && i < n_att) ? attacks[i] : CARD_NONE;
    }
}

static void pm_seal(void) {
    g_pm_len = legal_menu_write(&g_pm, 0, -1, g_pm_wire, (int)sizeof g_pm_wire);
}

static void tb_add(Card attack, int covered) {
    g_pm_table[2 * g_pm_nb] = (unsigned char)card_to_id(attack);
    g_pm_table[2 * g_pm_nb + 1] = covered ? (unsigned char)card_to_id(attack) : LEGAL_WIRE_NONE;
    g_pm_nb++;
}

static PlayBoard pm_board(int power_suit, int is_defender) {
    PlayBoard b;
    b.menu = g_pm_wire; b.menu_len = g_pm_len;
    b.table = g_pm_table; b.n_battles = g_pm_nb;
    b.power_suit = power_suit; b.is_defender = is_defender;
    return b;
}

static unsigned char pm_sel[8];
static int pm_sel_n;

static void sel_set(const Card *cards, int n) {
    pm_sel_n = n;
    for (int i = 0; i < n; i++) pm_sel[i] = (unsigned char)card_to_id(cards[i]);
}

// The menu wire is only worth having if what the writer put down is what the
// reader walks back, entry for entry.
static void test_menu_wire_round_trips(void) {
    pm_reset();
    Card a1[1] = { { SUIT_SPADES, 6 } };
    Card a2[2] = { { SUIT_SPADES, 6 }, { SUIT_HEARTS, 6 } };
    Card cov[1] = { { SUIT_CLUBS, 13 } };
    Card att[1] = { { SUIT_HEARTS, 9 } };
    pm_add(MOVE_ATTACK, a1, 1, 0, 0);
    pm_add(MOVE_ATTACK, a2, 2, 0, 0);
    pm_add(MOVE_COVER, cov, 1, att, 1);
    pm_add(MOVE_PICKUP, 0, 0, 0, 0);
    pm_seal();
    CHECK(g_pm_len > 4, "a menu of four moves writes something");

    MenuWalk w;
    MenuMove m;
    CHECK(legal_menu_begin(&w, g_pm_wire, g_pm_len) == 4, "the header counts the entries");
    CHECK(legal_menu_next(&w, &m) == 1 && m.type == MOVE_ATTACK && m.n_cards == 1
          && m.cards[0] == card_to_id(a1[0]), "entry 0 is the one-card attack");
    CHECK(w.index == 0, "and the walk names its index");
    CHECK(legal_menu_next(&w, &m) == 1 && m.n_cards == 2
          && m.cards[0] == card_to_id(a2[0]) && m.cards[1] == card_to_id(a2[1]),
          "entry 1 is the two-card attack, in order");
    CHECK(legal_menu_next(&w, &m) == 1 && m.type == MOVE_COVER
          && m.attacks[0] == card_to_id(att[0]), "entry 2 is the cover, with its attack");
    CHECK(legal_menu_next(&w, &m) == 1 && m.type == MOVE_PICKUP && m.n_cards == 0,
          "entry 3 is the pickup");
    CHECK(legal_menu_next(&w, &m) == 0, "and then the menu ends");

    // A buffer cut short is refused rather than half-walked.
    MenuWalk t;
    legal_menu_begin(&t, g_pm_wire, g_pm_len - 1);
    int rc = 1, seen = 0;
    while ((rc = legal_menu_next(&t, &m)) == 1) seen++;
    CHECK(rc == LEGAL_WIRE_EPARSE, "a truncated menu is a parse error, not a short menu");
    CHECK(seen < 4, "and it does not hand back the entry it could not read");

    // A cap that cannot hold the whole menu leaves NO header claiming it did.
    unsigned char small[8];
    memset(small, 0xAB, sizeof small);
    CHECK(legal_menu_write(&g_pm, 0, -1, small, (int)sizeof small) == LEGAL_WIRE_ECAP,
          "a menu that will not fit is refused");
    MenuWalk s;
    CHECK(legal_menu_begin(&s, small, (int)sizeof small) != 4,
          "and the refused write left no header promising four moves");
}

// The writer is the one the shipped bridges use, so a chunk of it has to be a
// well-formed menu of exactly the moves asked for.
static void test_menu_wire_writes_a_chunk(void) {
    pm_reset();
    Card c[1] = { { SUIT_SPADES, 6 } };
    for (int i = 0; i < 5; i++) { c[0].value = (int8_t)(6 + i); pm_add(MOVE_ATTACK, c, 1, 0, 0); }
    unsigned char buf[64];
    int n = legal_menu_write(&g_pm, 1, 2, buf, (int)sizeof buf);
    CHECK(n > 0, "a chunk writes");
    MenuWalk w;
    MenuMove m;
    CHECK(legal_menu_begin(&w, buf, n) == 2, "a chunk of 2 counts 2");
    CHECK(legal_menu_next(&w, &m) == 1 && m.cards[0] == card_to_id((Card){ SUIT_SPADES, 7 }),
          "and starts at the requested index");
    // Past the end is an empty chunk, not a wrap or a read off the array.
    CHECK(legal_menu_begin(&w, buf, legal_menu_write(&g_pm, 99, 3, buf, (int)sizeof buf)) == 0,
          "a chunk starting past the last move is empty");
}

// ---------- play_resolve ----------------------------------------------------

static void test_play_resolve_attacker(void) {
    Card six_s = { SUIT_SPADES, 6 }, six_h = { SUIT_HEARTS, 6 };
    Card pair[2] = { six_s, six_h };
    pm_reset();
    pm_add(MOVE_ATTACK, &six_s, 1, 0, 0);
    pm_add(MOVE_ATTACK, pair, 2, 0, 0);
    pm_seal();
    PlayBoard b = pm_board(SUIT_DIAMONDS, 0);

    sel_set(&six_s, 1);
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == 0,
          "a single card on open table is the single attack");

    // A selection is a SET: the order it was tapped in must not matter.
    Card reversed[2] = { six_h, six_s };
    sel_set(reversed, 2);
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == 1,
          "the two-card attack matches whichever order the cards were selected in");

    Card ten_c = { SUIT_CLUBS, 10 };
    sel_set(&ten_c, 1);
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == -1,
          "a card no menu entry names resolves to nothing");
    CHECK(play_resolve(&b, pm_sel, 0, PLAY_TARGET_TABLE) == -1, "and an empty selection to nothing");
}

// A DROP BACK IN THE HAND IS A REARRANGE, for the attacker as much as the
// defender. The attacker branch reads only the cards, so a resolver that did
// not answer the hand first would hand back a perfectly good attack for it.
static void test_play_resolve_refuses_the_hand_for_both_roles(void) {
    Card six = { SUIT_SPADES, 6 }, ace = { SUIT_SPADES, 13 }, nine = { SUIT_SPADES, 9 };
    pm_reset();
    pm_add(MOVE_ATTACK, &six, 1, 0, 0);
    pm_seal();
    PlayBoard att = pm_board(SUIT_DIAMONDS, 0);
    sel_set(&six, 1);
    CHECK(play_resolve(&att, pm_sel, pm_sel_n, PLAY_TARGET_HAND) == -1,
          "an attacker dropping back in the hand plays nothing");
    CHECK(play_resolve(&att, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == 0,
          "…while the same cards on the table still attack");

    pm_reset();
    tb_add(nine, 0);
    pm_add(MOVE_COVER, &ace, 1, &nine, 1);
    pm_seal();
    PlayBoard def = pm_board(SUIT_DIAMONDS, 1);
    sel_set(&ace, 1);
    CHECK(play_resolve(&def, pm_sel, pm_sel_n, PLAY_TARGET_HAND) == -1,
          "and a defender dropping back in the hand plays nothing");
    CHECK(play_resolve(&def, pm_sel, pm_sel_n, 0) == 0, "…while the same drop on the battle covers");
}

static void test_play_resolve_defender_onto_a_battle(void) {
    Card nine = { SUIT_SPADES, 9 }, ten = { SUIT_SPADES, 10 }, ace = { SUIT_SPADES, 13 };
    pm_reset();
    tb_add(nine, 0);
    tb_add(ten, 1);              // already covered
    pm_add(MOVE_COVER, &ace, 1, &nine, 1);
    // A menu entry aimed at the COVERED battle too, so the refusal below has to
    // come from the table rather than from the menu happening not to offer it.
    pm_add(MOVE_COVER, &ace, 1, &ten, 1);
    pm_seal();
    PlayBoard b = pm_board(SUIT_DIAMONDS, 1);
    sel_set(&ace, 1);
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, 0) == 0, "a drop on the uncovered attack covers it");
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, 1) == -1, "a drop on an already-covered battle is nothing");
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, 2) == -1, "and a drop on a battle that is not there");
}

// A drop on a named battle only plays a cover that covers THAT attack - the
// menu may hold a cover with the same cards aimed somewhere else.
static void test_play_resolve_battle_target_names_its_attack(void) {
    Card nine = { SUIT_SPADES, 9 }, six = { SUIT_HEARTS, 6 }, ace = { SUIT_DIAMONDS, 13 };
    pm_reset();
    tb_add(nine, 0);
    tb_add(six, 0);
    pm_add(MOVE_COVER, &ace, 1, &six, 1);     // this ace covers the SIX on the menu
    pm_seal();
    PlayBoard b = pm_board(SUIT_DIAMONDS, 1);
    sel_set(&ace, 1);
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, 1) == 0, "the battle the menu entry covers resolves");
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, 0) == -1,
          "the battle it does not cover resolves to nothing, even with the same cards");
}

static void test_play_resolve_open_table_prefers_a_pass(void) {
    Card nine = { SUIT_SPADES, 9 }, nine_h = { SUIT_HEARTS, 9 };
    pm_reset();
    tb_add(nine, 0);
    pm_add(MOVE_COVER, &nine_h, 1, &nine, 1);
    pm_add(MOVE_PASS, &nine_h, 1, 0, 0);
    pm_seal();
    PlayBoard b = pm_board(SUIT_HEARTS, 1);
    sel_set(&nine_h, 1);
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == 1,
          "open table bounces the bout when a pass is legal with these cards");
}

static void test_play_resolve_open_table_auto_covers_only_when_unambiguous(void) {
    Card nine = { SUIT_SPADES, 9 }, ten = { SUIT_SPADES, 10 }, ace = { SUIT_SPADES, 13 };
    pm_reset();
    tb_add(nine, 0);
    pm_add(MOVE_COVER, &ace, 1, &nine, 1);
    pm_seal();
    PlayBoard one = pm_board(SUIT_DIAMONDS, 1);
    sel_set(&ace, 1);
    CHECK(play_resolve(&one, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == 0,
          "one legal cover with this selection auto-targets");

    pm_reset();
    tb_add(nine, 0);
    tb_add(ten, 0);
    pm_add(MOVE_COVER, &ace, 1, &nine, 1);
    pm_add(MOVE_COVER, &ace, 1, &ten, 1);
    pm_seal();
    PlayBoard two = pm_board(SUIT_DIAMONDS, 1);
    CHECK(play_resolve(&two, pm_sel, pm_sel_n, PLAY_TARGET_TABLE) == -1,
          "two of them is a question the drop cannot answer, so it plays nothing");
}

// ---------- coverable battles ----------------------------------------------

static void test_play_coverable_battles(void) {
    Card six = { SUIT_SPADES, 5 }, king = { SUIT_HEARTS, 12 }, nine = { SUIT_DIAMONDS, 8 };
    Card ace = { SUIT_CLUBS, 13 };
    pm_reset();
    tb_add(six, 0); tb_add(king, 0); tb_add(nine, 0);
    pm_add(MOVE_COVER, &ace, 1, &six, 1);
    pm_add(MOVE_COVER, &ace, 1, &king, 1);
    pm_add(MOVE_COVER, &ace, 1, &nine, 1);
    pm_seal();
    PlayBoard b = pm_board(SUIT_CLUBS, 1);
    sel_set(&ace, 1);
    CHECK(play_coverable_battles(&b, pm_sel, pm_sel_n) == 0x7,
          "every coverable attack is offered, so the player may aim at any of them");
    CHECK(play_coverable_battles(&b, pm_sel, 0) == 0, "an empty selection covers nothing");

    // A covered battle is never a drop target, whatever the menu says.
    pm_reset();
    tb_add(six, 1); tb_add(king, 0);
    pm_add(MOVE_COVER, &ace, 1, &six, 1);
    pm_add(MOVE_COVER, &ace, 1, &king, 1);
    pm_seal();
    PlayBoard c = pm_board(SUIT_CLUBS, 1);
    CHECK(play_coverable_battles(&c, pm_sel, pm_sel_n) == 0x2,
          "a battle already covered is not a target");
}

// ---------- the cover BUTTON's choice ---------------------------------------
//
// Owner, round 16: "cover the highest value card that can be covered by that
// card. Trump is higher than non trump. If there are multiple highest value
// cards that can be covered, just choose one." Each case asserts on the CARD
// that ends up covered - an index only means something relative to a table
// order, and which attack it chose is the whole question.

static Card best_covered(const Card *cards, int n, const Card *attacks, int n_att,
                         const int *coverable, int n_cov, int trump) {
    pm_reset();
    for (int i = 0; i < n_att; i++) tb_add(attacks[i], 0);
    for (int i = 0; i < n_cov; i++) pm_add(MOVE_COVER, cards, n, &attacks[coverable[i]], 1);
    pm_seal();
    PlayBoard b = pm_board(trump, 1);
    sel_set(cards, n);
    const int idx = play_best_cover_target(&b, pm_sel, pm_sel_n);
    return idx < 0 ? CARD_NONE : attacks[idx];
}

static void test_best_cover_takes_the_highest_it_can_beat(void) {
    Card ace[1] = { { SUIT_CLUBS, 13 } };
    Card attacks[3] = { { SUIT_SPADES, 5 }, { SUIT_HEARTS, 12 }, { SUIT_DIAMONDS, 8 } };
    int all[3] = { 0, 1, 2 };
    CHECK(card_eq(best_covered(ace, 1, attacks, 3, all, 3, SUIT_CLUBS), attacks[1]),
          "did not spend the ace on the king");
    // The old behaviour, pinned as a change: the leftmost coverable is index 0.
    CHECK(!card_eq(best_covered(ace, 1, attacks, 3, all, 3, SUIT_CLUBS), attacks[0]),
          "the choice is no longer just the leftmost attack");
}

static void test_best_cover_trump_outranks_a_bigger_plain_card(void) {
    Card trump_ace[1] = { { SUIT_SPADES, 13 } };
    Card attacks[2] = { { SUIT_HEARTS, 13 }, { SUIT_SPADES, 5 } };   // A-heart, 6-spade
    int all[2] = { 0, 1 };
    CHECK(card_eq(best_covered(trump_ace, 1, attacks, 2, all, 2, SUIT_SPADES), attacks[1]),
          "an ace off-suit beat a trump six");
    // …and the rule follows the TRUMP SUIT, not whichever suit is listed first.
    Card plain_ace[1] = { { SUIT_CLUBS, 13 } };
    CHECK(card_eq(best_covered(plain_ace, 1, attacks, 2, all, 2, SUIT_CLUBS), attacks[0]),
          "with clubs trump, neither attack is a trump and the ace is the highest");
}

static void test_best_cover_only_considers_what_the_menu_offers(void) {
    Card nine[1] = { { SUIT_CLUBS, 8 } };
    Card attacks[3] = { { SUIT_HEARTS, 6 }, { SUIT_SPADES, 13 }, { SUIT_HEARTS, 7 } };
    int cov[2] = { 0, 2 };   // the ace is on the table but not on the menu
    CHECK(card_eq(best_covered(nine, 1, attacks, 3, cov, 2, SUIT_DIAMONDS), attacks[2]),
          "chose an attack that was not on the legal menu");
    CHECK(card_is_none(best_covered(nine, 1, attacks, 3, cov, 0, SUIT_DIAMONDS)),
          "a selection that covers nothing chooses nothing");
}

// A tie resolves to the leftmost and keeps resolving there. The owner allows
// any of them; a choice that moved between two identical taps reads as a bug.
static void test_best_cover_breaks_ties_the_same_way_every_time(void) {
    Card card[1] = { { SUIT_SPADES, 13 } };
    Card attacks[3] = { { SUIT_HEARTS, 9 }, { SUIT_CLUBS, 9 }, { SUIT_DIAMONDS, 9 } };
    int all[3] = { 0, 1, 2 };
    for (int i = 0; i < 20; i++)
        CHECK(card_eq(best_covered(card, 1, attacks, 3, all, 3, SUIT_SPADES), attacks[0]),
              "the same table chose a different attack on a second tap");
}

// The choice must not depend on the order the attackers happened to throw in.
static void test_best_cover_ignores_table_order(void) {
    Card ace[1] = { { SUIT_CLUBS, 13 } };
    Card base[3] = { { SUIT_SPADES, 5 }, { SUIT_HEARTS, 12 }, { SUIT_DIAMONDS, 8 } };
    const int perms[6][3] = { {0,1,2}, {2,1,0}, {1,0,2}, {2,0,1}, {0,2,1}, {1,2,0} };
    for (int p = 0; p < 6; p++) {
        Card ordered[3];
        for (int i = 0; i < 3; i++) ordered[i] = base[perms[p][i]];
        int all[3] = { 0, 1, 2 };
        CHECK(card_eq(best_covered(ace, 1, ordered, 3, all, 3, SUIT_CLUBS), base[1]),
              "a permutation of the table covered something else");
    }
}

// Whatever it picks has to survive the resolver the tap then runs it through.
static void test_best_cover_target_resolves_to_a_legal_cover(void) {
    Card ace[1] = { { SUIT_CLUBS, 13 } };
    Card attacks[3] = { { SUIT_SPADES, 5 }, { SUIT_HEARTS, 12 }, { SUIT_DIAMONDS, 8 } };
    pm_reset();
    for (int i = 0; i < 3; i++) tb_add(attacks[i], 0);
    for (int i = 0; i < 3; i++) pm_add(MOVE_COVER, ace, 1, &attacks[i], 1);
    pm_seal();
    PlayBoard b = pm_board(SUIT_CLUBS, 1);
    sel_set(ace, 1);
    const int i = play_best_cover_target(&b, pm_sel, pm_sel_n);
    CHECK(i == 1, "the button aims at the king");
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, i) == 1,
          "and the tap that follows plays the cover it aimed at");
}

// A multi-card cover is chosen the same way. The menu may name fewer attack
// cards than cover cards; the padding must never read as a real attack.
static void test_best_cover_multi_card_selection(void) {
    Card cards[2] = { { SUIT_CLUBS, 13 }, { SUIT_CLUBS, 12 } };
    Card attacks[2] = { { SUIT_HEARTS, 5 }, { SUIT_HEARTS, 11 } };
    pm_reset();
    tb_add(attacks[0], 0); tb_add(attacks[1], 0);
    pm_add(MOVE_COVER, cards, 2, &attacks[0], 1);
    pm_add(MOVE_COVER, cards, 2, &attacks[1], 1);
    pm_seal();
    PlayBoard b = pm_board(SUIT_SPADES, 1);
    sel_set(cards, 2);
    CHECK(play_best_cover_target(&b, pm_sel, pm_sel_n) == 1, "did not aim at the jack");
    CHECK(play_resolve(&b, pm_sel, pm_sel_n, 1) == 1, "and the jack is what it plays");
}

// ---------- the buttons -----------------------------------------------------

static void test_play_has_verb(void) {
    Card six = { SUIT_SPADES, 6 }, other = { SUIT_HEARTS, 10 };
    pm_reset();
    pm_add(MOVE_ATTACK, &six, 1, 0, 0);
    pm_add(MOVE_PASS, &other, 1, 0, 0);
    pm_seal();
    PlayBoard b = pm_board(SUIT_DIAMONDS, 0);
    sel_set(&six, 1);
    CHECK(play_has_verb(&b, MOVE_ATTACK, pm_sel, pm_sel_n), "Attack lights for a selection it can attack with");
    CHECK(!play_has_verb(&b, MOVE_PASS, pm_sel, pm_sel_n), "…and Pass does not");
    sel_set(&other, 1);
    CHECK(play_has_verb(&b, MOVE_PASS, pm_sel, pm_sel_n), "Pass lights for the card the menu passes with");
    CHECK(!play_has_verb(&b, MOVE_ATTACK, pm_sel, 0), "an empty selection lights nothing");
}

// THE UI RULE THE KERNEL MENU CONTRADICTS. The menu always offers GOOD (it is
// how an attacker leaves the bot loop's eligible set); a human may not end a
// bout over an attack that is still uncovered.
static void test_play_can_say_good_only_over_a_covered_table(void) {
    Card nine = { SUIT_SPADES, 9 };
    pm_reset();
    tb_add(nine, 0);
    pm_add(MOVE_GOOD, 0, 0, 0, 0);
    pm_seal();
    PlayBoard uncovered = pm_board(SUIT_DIAMONDS, 0);
    CHECK(!play_can_say_good(&uncovered), "an attacker cannot say good over an uncovered attack");

    pm_reset();
    tb_add(nine, 1);
    pm_add(MOVE_GOOD, 0, 0, 0, 0);
    pm_seal();
    PlayBoard covered = pm_board(SUIT_DIAMONDS, 0);
    CHECK(play_can_say_good(&covered), "…and can once every attack is covered");

    pm_reset();
    pm_add(MOVE_GOOD, 0, 0, 0, 0);
    pm_seal();
    PlayBoard empty = pm_board(SUIT_DIAMONDS, 0);
    CHECK(!play_can_say_good(&empty), "an empty table is not a bout to finish");

    pm_reset();
    tb_add(nine, 1);
    pm_add(MOVE_PICKUP, 0, 0, 0, 0);
    pm_seal();
    PlayBoard no_good = pm_board(SUIT_DIAMONDS, 1);
    CHECK(!play_can_say_good(&no_good), "and legality still comes from the menu");
}

static int human_menu_types(const PlayBoard *b, unsigned char *out, int cap, int *n_out) {
    const int n = play_human_menu(b, out, cap);
    MenuWalk w;
    MenuMove m;
    int types = 0;
    *n_out = 0;
    if (n < 0) return -1;
    legal_menu_begin(&w, out, n);
    while (legal_menu_next(&w, &m) == 1) { types |= 1 << m.type; (*n_out)++; }
    return types;
}

static void test_play_human_menu_drops_wait_and_gates_good(void) {
    unsigned char out[512];
    int n = 0;
    Card nine = { SUIT_SPADES, 9 }, queen = { SUIT_SPADES, 11 };

    pm_reset();
    tb_add(nine, 0);
    pm_add(MOVE_GOOD, 0, 0, 0, 0);
    pm_add(MOVE_WAIT, 0, 0, 0, 0);
    pm_add(MOVE_PICKUP, 0, 0, 0, 0);
    pm_add(MOVE_COVER, &queen, 1, &nine, 1);
    pm_seal();
    PlayBoard uncovered = pm_board(SUIT_DIAMONDS, 1);
    int types = human_menu_types(&uncovered, out, (int)sizeof out, &n);
    CHECK(types == ((1 << MOVE_PICKUP) | (1 << MOVE_COVER)),
          "wait is never a move and good is withheld over an uncovered attack");
    CHECK(n == 2, "…and everything else survives the gate untouched");

    pm_reset();
    tb_add(nine, 1);
    pm_add(MOVE_GOOD, 0, 0, 0, 0);
    pm_add(MOVE_WAIT, 0, 0, 0, 0);
    pm_seal();
    PlayBoard covered = pm_board(SUIT_DIAMONDS, 0);
    types = human_menu_types(&covered, out, (int)sizeof out, &n);
    CHECK(types == (1 << MOVE_GOOD) && n == 1, "a fully covered table offers good and nothing else");

    pm_reset();
    pm_add(MOVE_WAIT, 0, 0, 0, 0);
    pm_seal();
    PlayBoard nothing = pm_board(SUIT_DIAMONDS, 0);
    types = human_menu_types(&nothing, out, (int)sizeof out, &n);
    CHECK(types == 0 && n == 0, "a seat whose only offer is wait may do nothing");

    // The move a human menu keeps must still be the move the kernel enumerated,
    // cards and all - a filter that reshaped an entry would play the wrong card.
    pm_reset();
    tb_add(nine, 0);
    pm_add(MOVE_WAIT, 0, 0, 0, 0);
    pm_add(MOVE_COVER, &queen, 1, &nine, 1);
    pm_seal();
    PlayBoard one = pm_board(SUIT_DIAMONDS, 1);
    const int len = play_human_menu(&one, out, (int)sizeof out);
    MenuWalk w;
    MenuMove m;
    legal_menu_begin(&w, out, len);
    CHECK(legal_menu_next(&w, &m) == 1 && m.type == MOVE_COVER && m.n_cards == 1
          && m.cards[0] == card_to_id(queen) && m.attacks[0] == card_to_id(nine),
          "the survivor crosses the filter byte for byte");
}

// ---------- GOOD stays in the ENUMERATED menu -------------------------------
//
// The narrowing above is a rule about a board, and it must never migrate into
// calculate_legal_moves. Saying good is how a non-defender leaves the bot
// loop's eligible set (good_players_mask -> should_bot_act ->
// bot_drive_eligible_mask); gate GOOD on all-covered there and an attacker
// holding a legal throw-in could never decline one, so it would keep attacking
// until it ran out of cards and stay eligible every cycle for as long as that
// lasted.
static void test_good_is_always_enumerated_for_an_attacker(void) {
    Game g;
    memset(&g, 0, sizeof g);
    g.status = GAME_STATUS_PLAYING;
    g.num_players = 3;
    g.power_suit = SUIT_DIAMONDS;
    g.first_attacker = 0;
    g.defender = 1;
    g.deck_count = 10;
    for (int i = 0; i < 3; i++) g.players[i].status = PLAYER_STATUS_IN;

    // An UNCOVERED attack on the table, and seat 2 holding a legal throw-in of
    // the same rank - the exact state a human board hides GOOD in.
    g.num_battles = 1;
    g.table_battles[0].attack = (Card){ SUIT_SPADES, 9 };
    g.table_battles[0].defense = CARD_NONE;
    g.players[0].hand[0] = (Card){ SUIT_HEARTS, 9 };  g.players[0].hand_count = 1;
    // The defender needs room to take everything on the table plus the
    // throw-in, or the enumerator drops the attack and the case tests nothing.
    for (int i = 0; i < 5; i++) g.players[1].hand[i] = (Card){ SUIT_SPADES, (int8_t)(8 + i) };
    g.players[1].hand_count = 5;
    g.players[2].hand[0] = (Card){ SUIT_CLUBS, 9 };   g.players[2].hand_count = 1;

    static LegalMoves lm;
    calculate_legal_moves(&g, 2, &lm);
    int has_good = 0, has_attack = 0;
    for (int i = 0; i < lm.n; i++) {
        if (lm.moves[i].type == MOVE_GOOD) has_good = 1;
        if (lm.moves[i].type == MOVE_ATTACK) has_attack = 1;
    }
    CHECK(has_attack, "the throw-in is legal");
    CHECK(has_good, "and so is declining it - GOOD is in the menu over an uncovered table");

    // …which is what lets the seat leave the drive loop.
    CHECK((bot_drive_eligible_mask(&g, 0) & (1u << 2)) != 0,
          "an attacker that has not said good is eligible every cycle");
    g.good_players_mask |= (1u << 2);
    CHECK((bot_drive_eligible_mask(&g, 0) & (1u << 2)) == 0,
          "and saying good is what drops it out of the eligible set");

    // The board's own narrowing of that same menu still hides it.
    unsigned char wire[4096];
    const int len = legal_menu_write(&lm, 0, -1, wire, (int)sizeof wire);
    unsigned char table[2] = { (unsigned char)card_to_id(g.table_battles[0].attack), LEGAL_WIRE_NONE };
    PlayBoard b;
    b.menu = wire; b.menu_len = len; b.table = table; b.n_battles = 1;
    b.power_suit = SUIT_DIAMONDS; b.is_defender = 0;
    CHECK(!play_can_say_good(&b),
          "the human gate is the only place the same menu loses its GOOD");
}

// ---------- the shape of a sequence (anim_plan.h beats + the role beat) -----
//
// The ordering rules the iMessage board settled by hand and every other client
// would otherwise re-derive, lifted out of Swift
// (ios/FoolishKit/Boards/MessageTableView's parallelGroups / placedOnTable /
// outsWith / holdsAfter / badgeDropsAsCardsLeave / goodsOpening / goodsCleared /
// passHandOff). The stream is an INPUT here, exactly as it is on a board: what a
// bubble hands over, sometimes only the half of it a staged settlement has
// released.
//
// MUTATION-CHECKED, each mutation applied to c/src/anim_plan.c on its own:
//
//   the grouping merges any consecutive pair of the same type       -> 2 failures
//   the grouping merges covers regardless of seat                   -> 1 failure
//   the grouping merges covers that are not adjacent                -> 1 failure
//   holdsAfter treats a notice as a wall (drops the `continue`)     -> 3 failures
//   holdsAfter skips over anything, not only notices                -> 2 failures
//   holdsAfter fires for a beat that is not a cover                 -> 5 failures
//   outsWith drops the "this beat moved something" gate             -> 1 failure
//   outsWith adopts past a beat that is not purely notices          -> 1 failure
//   outsWith stops after the first out-only beat it adopts          -> 1 failure
//   placed_of counts pickups/discards as table placements           -> 3 failures
//   the badge drops as cards LAND (attack/cover -> pickup/refill)   -> 4 failures
//   goods_opening replaces the shown mask instead of adding to it   -> 1 failure
//   goods_cleared replaces the shown mask instead of removing from  -> 1 failure
//   pass_hand_off drops the "the passer is the shown defender" test -> 2 failures
//   pass_hand_off takes first_attacker from the final board         -> 1 failure

#define BT_MAX_IDS 8
static AnimBeatEvent bt_evs[ANIM_MAX_BEATS];
static Card          bt_cards[ANIM_MAX_BEATS][BT_MAX_IDS];
static int           bt_n;

static void bt_reset(void) { bt_n = 0; }

// One event: `n_ids` real card identities (dense ids), and the good mask of the
// board this step committed (ANIM_NO_MASK for a step that carries none).
static void bt_add(int type, int seat, int n_ids, const int *ids, int good) {
    const int i = bt_n;
    for (int k = 0; k < n_ids && k < BT_MAX_IDS; k++) bt_cards[i][k] = card_of_id(ids[k]);
    bt_evs[i].type = type;
    bt_evs[i].seat = seat;
    bt_evs[i].cards = bt_cards[i];
    bt_evs[i].n_cards = n_ids;
    bt_evs[i].mask_cards = 0;
    bt_evs[i].good_mask = good;
    bt_n = i + 1;
}
static void bt_ev(int type, int seat) { bt_add(type, seat, 0, NULL, ANIM_NO_MASK); }
static void bt_card_ev(int type, int seat, int id) {
    const int one[1] = { id };
    bt_add(type, seat, 1, one, ANIM_NO_MASK);
}
static int bt_build(AnimBeats *out) { return anim_build_beats(bt_evs, bt_n, out); }

// The beat spans, as a string like "2,1,1", so a whole grouping compares at once.
static const char *bt_spans(const AnimBeats *b) {
    static char s[256];
    int w = 0;
    for (int i = 0; i < b->n_beats && w < (int)sizeof s - 8; i++)
        w += snprintf(s + w, sizeof s - (size_t)w, i ? ",%d" : "%d", b->beats[i].n_events);
    if (b->n_beats == 0) s[0] = 0;
    return s;
}
// The holds, as "1,0,0".
static const char *bt_holds(const AnimBeats *b) {
    static char s[256];
    int w = 0;
    for (int i = 0; i < b->n_beats && w < (int)sizeof s - 8; i++)
        w += snprintf(s + w, sizeof s - (size_t)w, i ? ",%d" : "%d",
                      (b->beats[i].flags & ANIM_BEAT_HOLDS) ? 1 : 0);
    if (b->n_beats == 0) s[0] = 0;
    return s;
}

// ONLY consecutive covers by ONE seat merge. Everything else keeps its own beat.
static void test_beats_merge_only_same_seat_consecutive_covers(void) {
    AnimBeats b;

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_COVER, 1);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "2") == 0, "two covers by one seat are one beat");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_COVER, 2);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "1,1") == 0, "a different seat is a different beat");

    // NOT adjacent: a bout that closed between two covers puts its discard in
    // the way, and the covers belong to different bouts.
    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_DISCARD, -1); bt_ev(ANIM_EVT_COVER, 1);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "1,1,1") == 0, "a discard between two covers splits them");

    // A cover's own consequences keep their beats - that is what makes the
    // counts settle in the right order.
    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_COVER, 1);
    bt_ev(ANIM_EVT_DISCARD, -1); bt_ev(ANIM_EVT_REFILL, 0);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "2,1,1") == 0, "the sweep and the deal are their own beats");

    // Nothing else groups, however adjacent: an attack or a pass already carries
    // every card of its move in one event, and a deal is per seat.
    bt_reset();
    bt_ev(ANIM_EVT_REFILL, 0); bt_ev(ANIM_EVT_REFILL, 0);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "1,1") == 0, "two refills by one seat stay two beats");

    bt_reset();
    bt_ev(ANIM_EVT_ATTACK_PASS, 0); bt_ev(ANIM_EVT_ATTACK_PASS, 0);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "1,1") == 0, "two attacks by one seat stay two beats");

    bt_reset();
    CHECK(bt_build(&b) == 0 && b.n_beats == 0, "an empty stream has no beats");

    // The lead event is what the beat reports.
    bt_reset();
    bt_ev(ANIM_EVT_COVER, 3); bt_ev(ANIM_EVT_COVER, 3);
    bt_build(&b);
    CHECK(b.beats[0].first == 0 && b.beats[0].type == ANIM_EVT_COVER
          && b.beats[0].seat == 3, "the beat is named by its lead event");
}

// The HOLD: a cover whose bout end follows, over notices only.
static void test_beats_hold_skips_notices_and_stops_at_card_motion(void) {
    AnimBeats b;

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_DISCARD, -1); bt_ev(ANIM_EVT_REFILL, 0);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "1,0,0") == 0, "a cover that closed its bout holds");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_CARDS_TO_TRASH, -1); bt_ev(ANIM_EVT_REFILL, 0);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "1,0,0") == 0, "the trash sweep is a bout end too");

    // The notices: a cover that empties the defender's hand puts their OUT in
    // the way, and the last bout of a game adds a magic transition. Neither
    // moves a card, so neither breaks the pair.
    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_OUT, 1); bt_ev(ANIM_EVT_DISCARD, -1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "1,0,0") == 0, "an out between the cover and the sweep is skipped");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_OUT, 1);
    bt_ev(ANIM_EVT_MAGIC_TRANSITION, -1); bt_ev(ANIM_EVT_CARDS_TO_TRASH, -1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "1,0,0,0") == 0, "so is a transition behind it");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_FLIPPED, -1); bt_ev(ANIM_EVT_DISCARD, -1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "1,0,0") == 0, "and the flipped trump notice");

    // …and ANYTHING that moves a card ends the scan, even with a discard behind
    // it: that discard belongs to a later beat, not to this cover.
    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_REFILL, 0); bt_ev(ANIM_EVT_DISCARD, -1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "0,0,0") == 0, "a refill after the cover ends the scan");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_PICKUP, 0); bt_ev(ANIM_EVT_DISCARD, -1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "0,0,0") == 0, "and so does a pickup");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_ATTACK_PASS, 0);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "0,0") == 0, "a throw-in after the cover reopened the bout");

    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "0") == 0, "a cover with nothing behind it does not hold");

    // A multi-card cover holds ONCE, after the whole beat - not between its cards.
    bt_reset();
    bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_COVER, 1); bt_ev(ANIM_EVT_DISCARD, -1);
    bt_build(&b);
    CHECK(strcmp(bt_spans(&b), "2,1") == 0 && strcmp(bt_holds(&b), "1,0") == 0,
          "two cards, one beat, one hold");

    // THE COMMON BOUT END is not this one: the defender covered and sent, an
    // attacker then said good, and THAT bubble carries the discard with no cover
    // in it. Nothing holds - the table has been readable since the last bubble.
    bt_reset();
    bt_ev(ANIM_EVT_DISCARD, -1); bt_ev(ANIM_EVT_REFILL, 0); bt_ev(ANIM_EVT_REFILL, 1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "0,0,0") == 0, "a good's bubble does not hold");

    bt_reset();
    bt_ev(ANIM_EVT_PICKUP, 0); bt_ev(ANIM_EVT_REFILL, 1);
    bt_build(&b);
    CHECK(strcmp(bt_holds(&b), "0,0") == 0, "nor does a pickup");
}

// The OUTS: only a beat that MOVED something may adopt the notices behind it.
static void test_beats_only_a_beat_that_moved_adopts_the_outs_behind_it(void) {
    AnimBeats b;

    // A cover empties the defender's hand, so the kernel says `out` right after
    // it. The badge collapses WITH the cover, not a beat later.
    bt_reset();
    bt_card_ev(ANIM_EVT_COVER, 1, 9);
    bt_ev(ANIM_EVT_OUT, 1);
    bt_ev(ANIM_EVT_CARDS_TO_TRASH, -1);
    bt_build(&b);
    CHECK(b.beats[0].outs_mask == (1u << 1), "the cover adopts the out that follows it");

    // The lookahead stops at the first beat that moves a card: an out two beats
    // later belongs to the move that caused IT.
    bt_reset();
    bt_card_ev(ANIM_EVT_ATTACK_PASS, 0, 6);
    bt_card_ev(ANIM_EVT_COVER, 1, 22);
    bt_ev(ANIM_EVT_OUT, 1);
    bt_build(&b);
    CHECK(b.beats[0].outs_mask == 0, "seat 1 goes out on the COVER, not the attack before it");
    CHECK(b.beats[1].outs_mask == (1u << 1), "the cover is the beat that carries it");

    // A pickup is card motion too, even though it places nothing.
    bt_reset();
    bt_card_ev(ANIM_EVT_PICKUP, 2, 6);
    bt_ev(ANIM_EVT_OUT, 0);
    bt_build(&b);
    CHECK(b.beats[0].outs_mask == (1u << 0), "a pickup carries the out that follows it");

    // Several seats going out on one move all collapse together…
    bt_reset();
    bt_card_ev(ANIM_EVT_CARDS_TO_TRASH, 1, 6);
    bt_ev(ANIM_EVT_OUT, 1);
    bt_ev(ANIM_EVT_OUT, 3);
    bt_build(&b);
    CHECK(b.beats[0].outs_mask == ((1u << 1) | (1u << 3)),
          "every out on one move collapses together");

    // …and a beat carrying its OWN out still reports it, which is the fallback
    // when nothing that moved cards preceded it.
    bt_reset();
    bt_ev(ANIM_EVT_OUT, 2);
    bt_build(&b);
    CHECK(b.beats[0].outs_mask == (1u << 2), "an orphan out answers for itself");

    // A beat that placed nothing and moved nothing adopts nothing. (An `out`
    // beat is itself flightless, so it may not speak for the one behind it.)
    bt_reset();
    bt_ev(ANIM_EVT_OUT, 2);
    bt_ev(ANIM_EVT_OUT, 3);
    bt_build(&b);
    CHECK(b.beats[0].outs_mask == (1u << 2), "a notice does not adopt the notice behind it");

    // A stream with nobody going out collapses nobody. The obvious wrong
    // implementation - diffing the out set against a final view - answers yes
    // here for every seat that was ALREADY out.
    bt_reset();
    bt_card_ev(ANIM_EVT_ATTACK_PASS, 0, 6);
    bt_card_ev(ANIM_EVT_COVER, 1, 22);
    bt_card_ev(ANIM_EVT_REFILL, 0, 37);
    bt_build(&b);
    for (int i = 0; i < b.n_beats; i++)
        CHECK(b.beats[i].outs_mask == 0, "an ordinary stream collapses nobody");
}

// PLACED: what a stream puts DOWN on the table, and the badge direction.
static void test_beats_placed_and_the_badge_direction(void) {
    AnimBeats b;

    bt_reset();
    bt_card_ev(ANIM_EVT_ATTACK_PASS, 0, 6);
    bt_card_ev(ANIM_EVT_COVER, 1, 22);
    bt_card_ev(ANIM_EVT_DEFENDER_MOVE, 1, 30);
    bt_card_ev(ANIM_EVT_PICKUP, 1, 40);
    bt_card_ev(ANIM_EVT_REFILL, 0, 41);
    bt_card_ev(ANIM_EVT_DISCARD, -1, 42);
    bt_build(&b);
    const uint64_t want = ((uint64_t)1 << 6) | ((uint64_t)1 << 22) | ((uint64_t)1 << 30);
    CHECK(b.placed_ids == want, "only attacks, passes and covers land on the table");
    CHECK(b.beats[0].placed_ids == ((uint64_t)1 << 6), "and each beat names its own");
    CHECK((b.beats[0].flags & ANIM_BEAT_PLACED) != 0, "a placement beat says so");
    CHECK((b.beats[3].flags & ANIM_BEAT_PLACED) == 0, "a pickup places nothing");
    CHECK((b.beats[3].flags & ANIM_BEAT_MOVED) != 0, "but it does move cards");

    // A badge drops as its cards LEAVE, and ticks up only as they land: a badge
    // reading 6 with two of that seat's cards in the air is claiming eight.
    CHECK((b.beats[0].flags & ANIM_BEAT_DROPS) != 0, "an attack empties a hand");
    CHECK((b.beats[1].flags & ANIM_BEAT_DROPS) != 0, "so does a cover");
    CHECK((b.beats[3].flags & ANIM_BEAT_DROPS) == 0, "a pickup fills one");
    CHECK((b.beats[4].flags & ANIM_BEAT_DROPS) == 0, "and so does a refill");
    CHECK(anim_badge_drops_as_cards_leave(ANIM_EVT_DEFENDER_MOVE) == 0,
          "a defender move is the kernel's own bookkeeping, not a hand emptying");
    CHECK(anim_badge_drops_as_cards_leave(-1) == 0, "an unrecognised step moves nobody's hand");

    // A masked back names no identity, so it places nothing even on a placement.
    bt_reset();
    bt_card_ev(ANIM_EVT_ATTACK_PASS, 0, 6);
    bt_evs[0].mask_cards = 1;
    bt_build(&b);
    CHECK(b.placed_ids == 0 && (b.beats[0].flags & ANIM_BEAT_PLACED) == 0,
          "a card back puts no identity on the table");
}

// THE ROLE BEAT: three timings, and they are not the same one.
static void test_role_beat_leads_runs_parallel_and_follows(void) {
    AnimRoles out;

    // A good being SET leads the stream. Only ADDED bits, and only ever added to
    // what is already shown.
    const AnimRoles shown = { 1, 0, 0 };
    CHECK(anim_goods_opening(shown, 0b100, &out) == 1, "a good this stream adds opens it");
    CHECK(out.good_mask == 0b100 && out.defender == 1 && out.first_attacker == 0,
          "the mark turns and nothing changes hands");
    CHECK(anim_goods_opening(shown, 0, &out) == 0, "a stream that adds no good opens nothing");
    CHECK(anim_goods_opening(shown, ANIM_NO_MASK, &out) == 0, "nor does a step with no board");
    const AnimRoles all = { 1, 0, 0b111 };
    CHECK(anim_goods_opening(all, 0, &out) == 0,
          "goods being CLEARED are not this rule - that is the back of the sequence");
    CHECK(anim_goods_opening(all, 0b001, &out) == 0, "a subset adds nothing");
    // ADDED TO WHAT IS ALREADY SHOWN, never replaced by it: a mark the badges
    // are already wearing that this step's board does not name is not taken
    // away here - that is the back of the sequence's job.
    const AnimRoles one_shown = { 1, 0, 0b001 };
    CHECK(anim_goods_opening(one_shown, 0b100, &out) == 1 && out.good_mask == 0b101,
          "the new good joins the one on screen rather than replacing it");

    // A good being CLEARED runs parallel with the throw-in that cleared it. The
    // mirror image, and it only ever REMOVES bits.
    const AnimRoles two_good = { 1, 0, 0b101 };
    CHECK(anim_goods_cleared(two_good, 0, &out) == 1, "a throw-in that clears two goods");
    CHECK(out.good_mask == 0 && out.defender == 1 && out.first_attacker == 0,
          "both marks turn, and nobody moves");
    CHECK(anim_goods_cleared(two_good, 0b100, &out) == 1, "one of the two");
    CHECK(out.good_mask == 0b100, "and the other one stays");
    const AnimRoles none_good = { 1, 0, 0 };
    CHECK(anim_goods_cleared(none_good, 0b100, &out) == 0,
          "a good being SET is the front of the sequence, not this");
    CHECK(anim_goods_cleared(none_good, 0, &out) == 0, "and nothing is nothing");
    CHECK(anim_goods_cleared(two_good, ANIM_NO_MASK, &out) == 0, "a step with no board clears nothing");
    const AnimRoles one_good = { 1, 0, 0b001 };
    CHECK(anim_goods_cleared(one_good, 0b011, &out) == 0,
          "a mask that only ADDS is the opening rule's business");
    // …and a mask that does both only REMOVES here. The bit this step set is
    // goodsOpening's, and taking the step's mask wholesale would turn a mark on
    // at the back of the sequence where it belongs at the front.
    CHECK(anim_goods_cleared(two_good, 0b110, &out) == 1 && out.good_mask == 0b100,
          "only the cleared bit turns; the one this step set is not this rule's");

    // A PASS hands the shield over WITH the transfer card. Seat 1 is defending
    // and laid cards, which a defender may only do as a transfer.
    const AnimRoles passer = { 1, 0, 0b100 };
    CHECK(anim_pass_hand_off(passer, 1u << 1, 2, &out) == 1, "the defender's own throw is a pass");
    CHECK(out.defender == 2, "the shield goes to the seat the final board defends with");
    CHECK(out.first_attacker == 0,
          "the opening sword is NOT taken from the final board - a pass never moves it");
    CHECK(out.good_mask == 0b100, "and the goods are goodsCleared's business, not this one");
    CHECK(anim_pass_hand_off(passer, 1u << 0, 2, &out) == 0,
          "an ATTACKER laying cards is an attack, whatever else the turn did");
    CHECK(anim_pass_hand_off(passer, 0, 2, &out) == 0, "a beat that laid nothing is no transfer");
    CHECK(anim_pass_hand_off(passer, 1u << 1, 1, &out) == 0,
          "and a final board that still defends with the passer is no hand-off");

    // A bout end also changes the defender, and it is NOT a pass: nobody wearing
    // the shield laid a card, so this waits for the closing beat.
    const AnimRoles defending = { 1, 0, 0 };
    CHECK(anim_pass_hand_off(defending, 1u << 0, 0, &out) == 0,
          "a bout end's rotation is the closing beat's, not the transfer card's");
}

// The role beat over a beat the grouping produced: the mask a hand-off reads is
// the beat's own, so the two rules cannot describe different beats.
static void test_role_beat_reads_the_beats_own_attack_pass_seats(void) {
    AnimBeats b;
    bt_reset();
    // Seat 1 is defending and transfers; seat 2 (the new defender) then covers.
    bt_add(ANIM_EVT_ATTACK_PASS, 1, 1, (const int[]){ 8 }, 0b100);
    bt_add(ANIM_EVT_COVER, 2, 1, (const int[]){ 21 }, 0);
    bt_build(&b);
    CHECK(b.beats[0].attack_pass_seats == (1u << 1), "the beat names who laid cards");
    CHECK(b.beats[1].attack_pass_seats == 0, "a cover is not a throw");
    CHECK(b.first_good_mask == 0b100, "the stream opens on its first step's board");
    CHECK(b.beats[1].good_mask == 0, "and a beat settles to its LAST step's board");

    AnimRoles out;
    const AnimRoles shown = { 1, 0, 0b100 };
    CHECK(anim_pass_hand_off(shown, b.beats[0].attack_pass_seats, 2, &out) == 1
          && out.defender == 2, "the transfer beat hands the shield over");
    CHECK(anim_pass_hand_off(shown, b.beats[1].attack_pass_seats, 2, &out) == 0,
          "the cover behind it does not");
    // The good the throw-in cleared turns with that same beat.
    CHECK(anim_goods_cleared(shown, b.beats[1].good_mask, &out) == 1 && out.good_mask == 0,
          "and the good it cleared turns with the card");
}

static void test_beats_refuses_a_stream_it_cannot_hold(void) {
    AnimBeats b;
    AnimBeatEvent one = { ANIM_EVT_OUT, 0, NULL, 0, 0, ANIM_NO_MASK };
    CHECK(anim_build_beats(&one, 1, NULL) == ANIM_EBADARG, "no output, no answer");
    CHECK(anim_build_beats(NULL, 1, &b) == ANIM_EBADARG, "no events, no answer");
    CHECK(anim_build_beats(NULL, 0, &b) == 0, "an empty stream needs no events pointer");
    CHECK(anim_build_beats(&one, -1, &b) == ANIM_EBADARG, "a negative count is not a stream");
    CHECK(anim_build_beats(&one, ANIM_MAX_BEATS + 1, &b) == ANIM_ECAP,
          "a stream longer than the plan can hold is refused, never truncated");
}

// ===========================================================================
// THE COUNT FREEZE, over real games, against the kernel's own boards.
// ===========================================================================
//
// The freeze is the number every badge shows for the first frame of an arriving
// move. Anywhere it is wrong is a badge displaying a board that never existed
// and correcting itself a beat later, which is round 16's report: "I sometimes
// saw the deck suddenly go to 5 cards, then deal, and now I have 6 cards? Is it
// a problem with the flipped card?" It was the flipped card - it lies UNDER the
// deck and is dealt last without ever being in deck_count, so a refill can hand
// out more cards than the deck said it held, and a walk that undoes every event
// puts them all back.
//
// The only honest oracle is the board the kernel actually had before the move,
// so this drives seeded games move by move and holds the plan against it. The
// twin of ios/FoolishTests/MessageCountWindingTests.swift, which drives the same
// property through the Swift board.
//
// MUTATION-CHECKED, each applied to c/src/anim_plan.c on its own. The counts
// are how many of the 1221 moves this sweep drives came out wrong; the sweep
// reports one CHECK per property, with the first bad case named, so a mutation
// shows up as one failure carrying that number rather than as a thousand.
//
//   the freeze walks back from the FINAL board over every event (the rule
//     this stage replaced)                        -> 18 freezes wrong, deck read
//                                                    2 where the board showed 1
//   the freeze anchors but undoes nothing         -> 1138 freezes wrong
//   the freeze undoes the first TWO events        -> 162 freezes wrong
//   a step's post counts derive forward instead
//     of adopting its own board                   -> 18 last steps are not the
//                                                    settled board
//   apply_undo gives a PICKUP the wrong sign      -> 122 freezes wrong
//
// The first mutation is the bug itself, and 18 of 1221 is why it survived in
// the web for so long: it needs the deck down to its last card or two.

#define PF_MAX_SNAPS 48
#define PF_PREFIX (__builtin_offsetof(Game, num_logs))
typedef struct { _Alignas(8) unsigned char bytes[PF_PREFIX]; } PfSlot;
static PfSlot pf_snaps[PF_MAX_SNAPS];
static int    pf_tags[PF_MAX_SNAPS], pf_aux[PF_MAX_SNAPS], pf_n_snaps;

static void pf_snap_cb(const Game *g, int tag, int aux) {
    if (pf_n_snaps >= PF_MAX_SNAPS) return;
    memcpy(pf_snaps[pf_n_snaps].bytes, g, PF_PREFIX);
    pf_tags[pf_n_snaps] = tag;
    pf_aux[pf_n_snaps] = aux;
    pf_n_snaps++;
}

static AnimPlanEvent pf_evs[ANIM_MAX_STEPS];
// Set when an event filled the log's card list exactly, i.e. the compact native
// arena (MAX_LOG_PAIRS 16) may have dropped cards the move really moved. The
// production builds log 64 and never reach it; see game.h.
static int           pf_maybe_truncated;
static Card          pf_cards[ANIM_MAX_STEPS][MAX_LOG_PAIRS];
static int           pf_hand[ANIM_MAX_STEPS][MAX_PLAYERS];
static int           pf_n, pf_np;

// The step's own board comes off the snapshot the kernel took AT that step -
// the same `snap` the wire writes as the event's game_state. That is what makes
// the freeze derivable without ever undoing a refill.
static void pf_sink(void *ctx, const EvwEvent *e) {
    (void)ctx;
    if (pf_n >= ANIM_MAX_STEPS) return;
    const int i = pf_n++;
    const int nc = e->n_cards > MAX_LOG_PAIRS ? MAX_LOG_PAIRS : e->n_cards;
    if (nc >= MAX_LOG_PAIRS) pf_maybe_truncated = 1;
    for (int k = 0; k < nc; k++) pf_cards[i][k] = e->cards[k];
    pf_evs[i].type = e->type;
    pf_evs[i].seat = e->seat;
    pf_evs[i].from = e->from;
    pf_evs[i].to = e->to;
    pf_evs[i].cards = pf_cards[i];
    pf_evs[i].n_cards = nc;
    pf_evs[i].mask_cards = e->mask_cards;
    pf_evs[i].has_counts = e->snap != 0;
    pf_evs[i].deck = e->snap ? e->snap->deck_count : 0;
    pf_evs[i].discard = e->snap ? e->snap->discard_pile_length : 0;
    for (int s = 0; s < pf_np; s++)
        pf_hand[i][s] = e->snap ? e->snap->players[s].hand_count : 0;
    pf_evs[i].hand = pf_hand[i];
}

// CHECK takes a plain string, so a per-case diagnostic is formatted here. Only
// ever reached on a failure (CHECK evaluates its message in the else branch).
static const char *pf_say(const char *fmt, ...) {
    static char b[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    return b;
}

typedef struct { int deck, discard, hand[MAX_PLAYERS]; } PfCounts;
static PfCounts pf_counts_of(const Game *g) {
    PfCounts c;
    c.deck = g->deck_count;
    c.discard = g->discard_pile_length;
    for (int s = 0; s < g->num_players; s++) c.hand[s] = g->players[s].hand_count;
    return c;
}

static void test_the_freeze_is_the_board_before_every_move(void) {
    static LegalMoves moves;
    int checked = 0, flipped_draws = 0, led_with_refill = 0, truncated = 0;
    // A failure is reported ONCE, with the first case that showed it - a sweep
    // that raised a CHECK per move would bury the shape under thousands of them.
    int bad_freeze = 0, bad_settled = 0, bad_forward = 0, refused = 0;
    static char first_bad[256];
    first_bad[0] = 0;

    for (int np = 2; np <= 4; np++) {
        for (int salt = 0; salt < 6; salt++) {
            Game g;
            const int seed = 4400 + salt * 7 + np;
            game_set_seed((uint32_t)seed);
            random_strategy_set_seed((uint32_t)seed);
            unsigned char deal[FOOLISH_SEED_LEN];
            for (int i = 0; i < FOOLISH_SEED_LEN; i++)
                deal[i] = (unsigned char)(i * 31 + seed * 13 + np);
            game_set_deal_seed_bytes(deal, FOOLISH_SEED_LEN);

            memset(&g, 0, sizeof g);
            g.num_players = (int8_t)np;
            for (int i = 0; i < np; i++) g.players[i].status = PLAYER_STATUS_READY;
            start_game(&g);
            pf_np = np;

            for (int guard = 0; guard < 20000 && game_done(&g) < 0; guard++) {
                const PfCounts before = pf_counts_of(&g);
                bool acted = false;
                const int log_start = g.num_logs;
                pf_n_snaps = 0;
                engine_snap_hook = pf_snap_cb;
                for (int pi = 0; pi < np && !acted; pi++) {
                    if (!should_bot_act(&g, pi)) continue;
                    calculate_legal_moves(&g, pi, &moves);
                    if (moves.n == 0) continue;
                    // Cover where it can, so games reach the endgame - an empty
                    // deck and the trump being drawn - through covers rather
                    // than pickup loops.
                    int pick = handwritten_strategy_choose(&g, pi, &moves, 0);
                    for (int m = 0; m < moves.n; m++)
                        if (moves.moves[m].type == MOVE_COVER) { pick = m; break; }
                    const LegalMove *m = &moves.moves[pick];
                    switch (m->type) {
                        case MOVE_ATTACK: acted = handle_attack(&g, pi, m->cards, m->n_cards); break;
                        case MOVE_COVER:  acted = handle_cover(&g, pi, m->cards, m->attack_cards, m->n_cards); break;
                        case MOVE_PASS:   acted = handle_pass(&g, pi, m->cards, m->n_cards); break;
                        case MOVE_PICKUP: acted = handle_pickup(&g, pi); break;
                        case MOVE_GOOD:   acted = handle_good(&g, pi); break;
                        default: break;
                    }
                }
                engine_snap_hook = 0;
                if (!acted) break;

                // Viewer -1 (a spectator) is the strict case: every seat's counts
                // are visible and every card identity is redacted, so a freeze
                // that leant on identities rather than counts fails here.
                EvSnap refs[PF_MAX_SNAPS];
                for (int i = 0; i < pf_n_snaps; i++) {
                    refs[i].g = (const Game *)(const void *)pf_snaps[i].bytes;
                    refs[i].tag = pf_tags[i];
                    refs[i].aux = pf_aux[i];
                }
                pf_n = 0;
                pf_maybe_truncated = 0;
                evwire_walk(refs, pf_n_snaps, g.logs + log_start, g.num_logs - log_start,
                            -1, pf_sink, 0);
                if (pf_n == 0 || pf_n > ANIM_MAX_STEPS) continue;
                // A move whose card list filled the compact native log is not a
                // stream any client ever sees (production logs 64 pairs), and its
                // arithmetic is short by however many cards were dropped. Counted
                // rather than silently skipped.
                if (pf_maybe_truncated) { truncated++; continue; }

                const PfCounts after = pf_counts_of(&g);
                AnimPlan plan;
                if (anim_build_plan(pf_evs, pf_n, np, after.deck, after.discard,
                                    after.hand, &plan) != ANIM_EOK) { refused++; continue; }
                checked++;

                // 1. The freeze IS the board the move started from.
                int same = plan.pre.deck == before.deck && plan.pre.discard == before.discard;
                for (int s = 0; s < np; s++) if (plan.pre.hand[s] != before.hand[s]) same = 0;
                if (!same) {
                    bad_freeze++;
                    if (!first_bad[0])
                        snprintf(first_bad, sizeof first_bad,
                                 "np=%d seed=%d: freeze deck %d discard %d hand[%d] %d, "
                                 "the board was deck %d discard %d hand[%d] %d "
                                 "(lead type %d seat %d x%d)",
                                 np, seed, plan.pre.deck, plan.pre.discard,
                                 pf_evs[0].seat < 0 ? 0 : pf_evs[0].seat,
                                 plan.pre.hand[pf_evs[0].seat < 0 ? 0 : pf_evs[0].seat],
                                 before.deck, before.discard,
                                 pf_evs[0].seat < 0 ? 0 : pf_evs[0].seat,
                                 before.hand[pf_evs[0].seat < 0 ? 0 : pf_evs[0].seat],
                                 pf_evs[0].type, pf_evs[0].seat, pf_evs[0].n_cards);
                }

                // 2. The last step IS the board the move produced - releasing the
                //    overrides there must not jump.
                const AnimPlanStep *last = &plan.steps[plan.n_steps - 1];
                int settled = last->deck == after.deck && last->discard == after.discard;
                for (int s = 0; s < np; s++) if (last->hand[s] != after.hand[s]) settled = 0;
                if (!settled) bad_settled++;

                // 3. The boards only walk FORWARD across a move: a deck that grows
                //    mid-sequence is the "deck suddenly went to 5" report itself.
                int deck = before.deck, discard = before.discard, forward = 1;
                for (int i = 0; i < plan.n_steps; i++) {
                    if (plan.steps[i].deck > deck || plan.steps[i].discard < discard) forward = 0;
                    deck = plan.steps[i].deck; discard = plan.steps[i].discard;
                }
                if (!forward) bad_forward++;

                // The shape this whole test exists for: cards drawn off a deck
                // that did not hold them, which is the flipped trump being dealt.
                int drawn = 0;
                for (int i = 0; i < plan.n_steps; i++)
                    if (pf_evs[i].type == ANIM_EVT_REFILL || pf_evs[i].type == ANIM_EVT_DEAL)
                        drawn += pf_evs[i].n_cards;
                if (drawn > before.deck) flipped_draws++;
                if (pf_evs[0].type == ANIM_EVT_REFILL) led_with_refill++;
            }
        }
    }

    CHECK(bad_freeze == 0,
          pf_say("%d of %d freezes are not the board the move started from - %s",
                 bad_freeze, checked, first_bad));
    CHECK(bad_settled == 0,
          pf_say("%d of %d last steps are not the settled board", bad_settled, checked));
    CHECK(bad_forward == 0,
          pf_say("%d of %d moves stepped their board backwards", bad_forward, checked));
    CHECK(refused == 0, pf_say("the plan refused %d real moves", refused));
    CHECK(checked > 200, pf_say("the sweep drove only %d moves", checked));
    CHECK(truncated * 20 < checked,
          pf_say("%d of %d moves overran the compact native log - the sweep is not "
                 "measuring what it thinks", truncated, checked + truncated));
    // Without this the test above passes by never meeting the bug at all.
    CHECK(flipped_draws > 3,
          pf_say("only %d moves drew the flipped trump - the shape was not tested", flipped_draws));
    // THE INVARIANT THE ONE-UNDO FREEZE RESTS ON: a refill is never the first
    // event of a stream - it is always a bout end's consequence, so a pickup, a
    // trash or a magic transition comes first. If the kernel ever leads with
    // one, it fails here rather than as a one-card drift on somebody's screen.
    CHECK(led_with_refill == 0, pf_say("%d streams led with a refill", led_with_refill));
}

// ===========================================================================
// THE PRE-BOUT TABLE (anim_plan.h anim_pre_bout_table).
// ===========================================================================
//
// The table a bout end sweeps, as it stood before the sweep. What is being
// pinned is the PAIRING: a table of two battles with one covered must come back
// as two battles, not as three single-card cells, because the board renders the
// sweep through the same grid the live table used and a differently shaped
// table animates every card into its new cell before anything flies off it.
//
// MUTATION-CHECKED, each applied to c/src/anim_plan.c on its own:
//   table_accounts_for as a SUBSET test (drop the n != n_cards line)  -> 1 failure
//   the walk stops at the first board WITH OR WITHOUT cards
//     (n_battles < 0 instead of <= 0)                                 -> 3 failures
//   the prior board is taken without the account test                 -> 2 failures
//   the flat reading reports paired = 1                               -> 6 failures
//   the sweep step is the LAST one rather than the first              -> 1 failure
//   the walk CONTINUES past a board that fails the account            -> 1 failure
//
// The REAL-GAME sweep below models what a client can offer as the prior board.
// It reads the frame's TRAILER (the board that step committed), not the first
// event's snapshot, because a step can emit two events or none - see
// MessageEnvelope.lastMoveEventsWithPrior. Mutating it back to "the previous
// frame, only when it holds exactly one event" takes the flat reading from
// 5 sweeps to 26 and the reshaping ones from 5 to 16.

#define PT_MAX 8
static AnimPreEvent   pt_evs[PT_MAX];
static unsigned char  pt_bat[PT_MAX][2 * ANIM_MAX_PRE_BATTLES];
static unsigned char  pt_cards[PT_MAX][ANIM_MAX_PRE_BATTLES];
static int            pt_n;

static void pt_reset(void) { pt_n = 0; }

// One step: its board (`bat`, 2 bytes per battle, or NULL for a step that
// carries none) and the cards it moved.
static void pt_add(int type, const unsigned char *bat, int n_bat,
                   const unsigned char *cards, int n_cards) {
    const int i = pt_n;
    for (int k = 0; k < 2 * n_bat; k++) pt_bat[i][k] = bat[k];
    for (int k = 0; k < n_cards; k++) pt_cards[i][k] = cards[k];
    pt_evs[i].type = type;
    pt_evs[i].n_battles = bat ? n_bat : ANIM_NO_BOARD;
    pt_evs[i].battles = pt_bat[i];
    pt_evs[i].n_cards = n_cards;
    pt_evs[i].cards = pt_cards[i];
    pt_n = i + 1;
}

// A table as "6d+kh,kd" - each battle's attack, and its cover after a '+'.
static const char *pt_str(const AnimPreTable *t) {
    static char s[512];
    int w = 0;
    for (int i = 0; i < t->n_battles && w < (int)sizeof s - 16; i++) {
        const int a = t->battles[2 * i], d = t->battles[2 * i + 1];
        w += snprintf(s + w, sizeof s - (size_t)w, i ? ",%d" : "%d", a);
        if (d != ANIM_TABLE_NONE)
            w += snprintf(s + w, sizeof s - (size_t)w, "+%d", d);
    }
    if (t->n_battles == 0) s[0] = 0;
    return s;
}

static void test_pre_bout_table_keeps_the_real_pairing(void) {
    // The owner's report, as ids: 6 of diamonds covered by the king of hearts,
    // and a bare king of diamonds. Three cards, TWO battles.
    const unsigned char six_d = 3 * 13 + 5, king_d = 3 * 13 + 12, king_h = 1 * 13 + 12;
    const unsigned char real[4] = { six_d, king_h, king_d, ANIM_TABLE_NONE };
    const unsigned char taken[3] = { six_d, king_d, king_h };
    AnimPreTable t;

    // With the prior board: the covered pair stays a pair.
    pt_reset();
    pt_add(ANIM_EVT_PICKUP, NULL, 0, taken, 3);
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 2, real, &t) == 2,
          "a pickup takes its shape from the real prior table");
    CHECK(strcmp(pt_str(&t), "44+25,51") == 0, "…with the cover still on its attack");
    CHECK(t.paired == 1, "…and it says so");

    // Without one there is nothing better than one cell per card - and the
    // answer must NOT pass that off as a table.
    CHECK(anim_pre_bout_table(pt_evs, pt_n, ANIM_NO_BOARD, NULL, &t) == 3,
          "with no board to read, the flat reading is all there is");
    CHECK(t.paired == 0, "…and it is reported as a reconstruction, never as a pairing");
    CHECK(strcmp(pt_str(&t), "44,51,25") == 0, "the flat reading is every card, uncovered");
}

static void test_pre_bout_table_refuses_a_board_from_another_moment(void) {
    const unsigned char six_d = 3 * 13 + 5, king_d = 3 * 13 + 12, king_h = 1 * 13 + 12;
    const unsigned char seven_s = 0 * 13 + 6;
    const unsigned char taken[3] = { six_d, king_d, king_h };
    AnimPreTable t;
    pt_reset();
    pt_add(ANIM_EVT_PICKUP, NULL, 0, taken, 3);

    // One card short.
    const unsigned char shortt[2] = { six_d, king_h };
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 1, shortt, &t) == 3 && t.paired == 0,
          "a table missing a picked-up card is not this table");
    // One card too many - the test is exact account, not overlap.
    const unsigned char extra[4] = { six_d, king_h, king_d, seven_s };
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 2, extra, &t) == 3 && t.paired == 0,
          "a table holding a card the pickup does not take is not this table");
    // The same cards, differently paired, IS this table: the pairing is the one
    // thing the flat list cannot contradict.
    const unsigned char repaired[4] = { king_d, king_h, six_d, ANIM_TABLE_NONE };
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 2, repaired, &t) == 2 && t.paired == 1,
          "the account is over the cards, and the board owns the pairing");
}

static void test_pre_bout_table_walks_back_through_the_stream(void) {
    const unsigned char six_d = 3 * 13 + 5, king_d = 3 * 13 + 12, king_h = 1 * 13 + 12;
    const unsigned char covered[4] = { six_d, king_h, king_d, ANIM_TABLE_NONE };
    const unsigned char taken[3] = { six_d, king_d, king_h };
    const unsigned char none[1] = { 0 };
    AnimPreTable t;

    // A multi-action turn carries the table on its own earlier step, and that
    // beats any prior board - it is nearer the sweep in time.
    pt_reset();
    pt_add(ANIM_EVT_ATTACK_PASS, covered, 2, &king_d, 1);
    pt_add(ANIM_EVT_PICKUP, NULL, 0, taken, 3);
    const unsigned char stale[2] = { six_d, ANIM_TABLE_NONE };
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 1, stale, &t) == 2 && t.paired == 1,
          "an earlier step of the stream outranks the prior board");

    // The discard side: the cover's own board still shows the whole covered
    // table; the trash step's board is already empty.
    pt_reset();
    pt_add(ANIM_EVT_COVER, covered, 2, &king_h, 1);
    pt_add(ANIM_EVT_CARDS_TO_TRASH, none, 0, taken, 3);
    CHECK(anim_pre_bout_table(pt_evs, pt_n, ANIM_NO_BOARD, NULL, &t) == 2 && t.paired == 1,
          "a trash sweeps the last board that still had cards on it");
    CHECK(strcmp(pt_str(&t), "44+25,51") == 0, "…the covered table, not a flat list");

    // A board that does NOT account STOPS the walk. The nearest board with
    // cards on it is the table being swept, so an older one that happens to
    // match the card list matches by coincidence. (A short card list is real:
    // a pickup log truncates at MAX_LOG_PAIRS.)
    const unsigned char seven_s2 = 0 * 13 + 6;
    const unsigned char later[6] = { six_d, king_h, king_d, ANIM_TABLE_NONE,
                                     seven_s2, ANIM_TABLE_NONE };
    pt_reset();
    pt_add(ANIM_EVT_ATTACK_PASS, covered, 2, &king_d, 1);
    pt_add(ANIM_EVT_ATTACK_PASS, later, 3, &seven_s2, 1);
    pt_add(ANIM_EVT_PICKUP, NULL, 0, taken, 3);
    CHECK(anim_pre_bout_table(pt_evs, pt_n, ANIM_NO_BOARD, NULL, &t) == 3 && t.paired == 0,
          "the walk stops at the first board that fails the account");

    // TWO sweeps in one stream: the board is about to play the FIRST of them.
    const unsigned char one_late[2] = { seven_s2, ANIM_TABLE_NONE };
    pt_reset();
    pt_add(ANIM_EVT_COVER, covered, 2, &king_h, 1);
    pt_add(ANIM_EVT_CARDS_TO_TRASH, none, 0, taken, 3);
    pt_add(ANIM_EVT_ATTACK_PASS, one_late, 1, &seven_s2, 1);
    pt_add(ANIM_EVT_PICKUP, NULL, 0, &seven_s2, 1);
    CHECK(anim_pre_bout_table(pt_evs, pt_n, ANIM_NO_BOARD, NULL, &t) == 2
          && strcmp(pt_str(&t), "44+25,51") == 0,
          "the first sweep of a stream is the one it hands back");

    // A stream that ends no bout has no table to hand back at all.
    pt_reset();
    pt_add(ANIM_EVT_ATTACK_PASS, covered, 2, &king_d, 1);
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 2, covered, &t) == 0 && t.n_battles == 0,
          "no sweep step, no pre-bout table");
    // …and a discard whose stream carries no board is nothing, never a guess:
    // a discard names the pile it filled, which is not a table to lay out.
    pt_reset();
    pt_add(ANIM_EVT_DISCARD, NULL, 0, taken, 3);
    CHECK(anim_pre_bout_table(pt_evs, pt_n, 2, covered, &t) == 0,
          "a discard is never reconstructed from its own cards");
}

static void test_pre_bout_table_degenerate_inputs(void) {
    AnimPreTable t;
    const unsigned char one[2] = { 0, ANIM_TABLE_NONE };
    AnimPreEvent e = { ANIM_EVT_PICKUP, ANIM_NO_BOARD, NULL, 1, one };
    CHECK(anim_pre_bout_table(&e, 1, ANIM_NO_BOARD, NULL, NULL) == ANIM_EBADARG,
          "no output, no answer");
    CHECK(anim_pre_bout_table(NULL, 1, ANIM_NO_BOARD, NULL, &t) == ANIM_EBADARG,
          "no events, no answer");
    CHECK(anim_pre_bout_table(NULL, 0, ANIM_NO_BOARD, NULL, &t) == 0,
          "an empty stream needs no events pointer");
    CHECK(anim_pre_bout_table(&e, -1, ANIM_NO_BOARD, NULL, &t) == ANIM_EBADARG,
          "a negative count is not a stream");
    CHECK(anim_pre_bout_table(&e, 1, 2, NULL, &t) == ANIM_EBADARG,
          "a prior board of two battles and no bytes is not a board");
    // A card byte that is not a card: refused rather than written out.
    const unsigned char bad[1] = { 52 };
    AnimPreEvent b = { ANIM_EVT_PICKUP, ANIM_NO_BOARD, NULL, 1, bad };
    CHECK(anim_pre_bout_table(&b, 1, ANIM_NO_BOARD, NULL, &t) == ANIM_EBADARG,
          "a card byte outside the deck is not a card");
}

// ---------------------------------------------------------------------------
// …and over REAL GAMES, against the kernel's own boards.
//
// The synthetic cases above say what the rule is; this says it is true of the
// streams a client actually gets. Every sweep in a played game is held against
// the last board the kernel really had before it: a PAIRED answer must be that
// board exactly, and an unpaired one may only appear where no board was to be
// had. It also counts how often the flat shape differs from the truth, which is
// the size of the defect this rule exists to close.

typedef struct {
    int frame, type, n_cards, n_bat;
    unsigned char cards[ANIM_MAX_PRE_BATTLES];
    unsigned char bat[2 * MAX_BATTLES];
} PtEv;

static PtEv pt_stream[2048];
static int  pt_stream_n;
static int  pt_stream_frame;

// THE BOARD A STEP COMMITTED - the frame's own trailer (evwire.h), one per
// step, which is what a bubble opening at that boundary actually finds. It
// exists even for a step that emitted NO events (a bare good), which is why it
// and not "the previous frame's first event" is the prior board.
typedef struct { int n_bat; unsigned char bat[2 * MAX_BATTLES]; } PtBoard;
static PtBoard pt_final[2048];
static int     pt_final_n;

static void pt_note_final(int step, const unsigned char *snap, int snap_len) {
    if (step < 0 || step >= (int)(sizeof pt_final / sizeof pt_final[0])) return;
    if (step >= pt_final_n) pt_final_n = step + 1;
    PtBoard *b = &pt_final[step];
    b->n_bat = 0;
    if (!snap || snap_len <= 0) return;
    static Game snapg;
    memset(&snapg, 0, sizeof snapg);
    state_get(&snapg, snap, 1);
    b->n_bat = snapg.num_battles > MAX_BATTLES ? MAX_BATTLES : snapg.num_battles;
    for (int i = 0; i < b->n_bat; i++) {
        b->bat[2 * i] = (unsigned char)card_to_id(snapg.table_battles[i].attack);
        b->bat[2 * i + 1] = card_is_none(snapg.table_battles[i].defense)
                            ? ANIM_TABLE_NONE
                            : (unsigned char)card_to_id(snapg.table_battles[i].defense);
    }
}

static void pt_sink(void *ctx, int index, const EvwRead *ev) {
    (void)ctx; (void)index;
    if (pt_stream_n >= (int)(sizeof pt_stream / sizeof pt_stream[0])) return;
    PtEv *e = &pt_stream[pt_stream_n++];
    e->frame = pt_stream_frame;
    e->type = ev->type;
    e->n_cards = ev->n_cards > ANIM_MAX_PRE_BATTLES ? ANIM_MAX_PRE_BATTLES : ev->n_cards;
    for (int i = 0; i < e->n_cards; i++) {
        const Card c = card_from_wire_state(ev->cards_wire[i]);
        e->cards[i] = card_is_none(c) || c.suit < 0 ? 0xFF : (unsigned char)card_to_id(c);
    }
    static Game snap;
    memset(&snap, 0, sizeof snap);
    state_get(&snap, ev->snap, 1);
    e->n_bat = snap.num_battles > MAX_BATTLES ? MAX_BATTLES : snap.num_battles;
    for (int i = 0; i < e->n_bat; i++) {
        e->bat[2 * i] = (unsigned char)card_to_id(snap.table_battles[i].attack);
        e->bat[2 * i + 1] = card_is_none(snap.table_battles[i].defense)
                            ? ANIM_TABLE_NONE
                            : (unsigned char)card_to_id(snap.table_battles[i].defense);
    }
}

static void test_the_pre_bout_table_is_the_board_the_kernel_had(void) {
    static unsigned char code[1 << 20];
    static unsigned char frames[1 << 19];
    long sweeps = 0, paired = 0, flat = 0, flat_would_reshape = 0;
    long bad_paired = 0, bad_flat = 0, bad_set = 0;
    int first_bad = -1;

    for (int np = 2; np <= 5; np++) {
        for (int seed = 1; seed <= 6; seed++) {
            Game g;
            unsigned char sd[FOOLISH_SEED_LEN];
            if (!rs_play_seeded(&g, np, 900 + seed, sd)) continue;
            const int enc = replay_encode_v6_from_game(&g, sd, FOOLISH_SEED_LEN, 1 << 20,
                                                       code, (int)sizeof code);
            if (enc <= 0) { CHECK(0, "the played game encodes as v6"); return; }
            const int steps = replay_steps_count_v6(code, enc, 0);
            for (int viewer = -1; viewer < np; viewer++) {
                pt_stream_n = 0;
                pt_final_n = 0;
                int from = 0, guard = 0;
                while (from < steps && ++guard < 4096) {
                    int nf = 0, next = from;
                    const int w = replay_steps_frames_v6(code, enc, viewer, from, 0,
                                                         frames, (int)sizeof frames, &nf, &next);
                    if (w <= 0 || nf <= 0) break;
                    int q = 0;
                    for (int f = 0; f < nf; f++) {
                        const int flen = frames[q] | (frames[q + 1] << 8);
                        q += 2;
                        pt_stream_frame = from + f;
                        const unsigned char *fin = 0; int fin_len = 0;
                        if (evwire_read(frames + q, flen, 0, &fin, &fin_len, pt_sink, 0) < 0) {
                            CHECK(0, "every frame of a replay decodes"); return;
                        }
                        pt_note_final(from + f, fin, fin_len);
                        q += flen;
                    }
                    from = next;
                }

                for (int i = 0; i < pt_stream_n; i++) {
                    const int t = pt_stream[i].type;
                    if (t != EVW_T_PICKUP && t != EVW_T_DISCARD && t != EVW_T_CARDS_TO_TRASH) continue;
                    sweeps++;
                    // The truth: the last board the kernel had with cards on it.
                    const PtEv *truth = 0;
                    for (int j = i - 1; j >= 0 && !truth; j--)
                        if (pt_stream[j].n_bat > 0) truth = &pt_stream[j];
                    if (!truth) continue;

                    // The bubble is this frame's events; the prior board is the
                    // one the board's own opening rule can offer (the previous
                    // frame, when it holds exactly one step).
                    pt_reset();
                    for (int j = 0; j < pt_stream_n && pt_n < PT_MAX; j++) {
                        if (pt_stream[j].frame != pt_stream[i].frame) continue;
                        pt_add(pt_stream[j].type, pt_stream[j].bat, pt_stream[j].n_bat,
                               pt_stream[j].cards, pt_stream[j].n_cards);
                    }
                    int n_prior = ANIM_NO_BOARD;
                    const unsigned char *prior = 0;
                    {
                        const int pf = pt_stream[i].frame - 1;
                        if (pf >= 0 && pf < pt_final_n && pt_final[pf].n_bat > 0) {
                            n_prior = pt_final[pf].n_bat;
                            prior = pt_final[pf].bat;
                        }
                    }

                    AnimPreTable out;
                    const int r = anim_pre_bout_table(pt_evs, pt_n, n_prior, prior, &out);
                    if (r < 0) { bad_paired++; if (first_bad < 0) first_bad = i; continue; }
                    if (out.paired) {
                        paired++;
                        // A pairing is a claim about a board that existed.
                        if (out.n_battles != truth->n_bat
                            || memcmp(out.battles, truth->bat, (size_t)(2 * out.n_battles)) != 0) {
                            bad_paired++;
                            if (first_bad < 0) first_bad = i;
                        }
                    } else if (r > 0) {
                        flat++;
                        // Only ever a pickup, only ever the cards it took, and
                        // only where no board could be found.
                        if (pt_stream[i].type != EVW_T_PICKUP) bad_flat++;
                        for (int k = 0; k < out.n_battles; k++)
                            if (out.battles[2 * k + 1] != ANIM_TABLE_NONE) bad_flat++;
                        if (out.n_battles != pt_stream[i].n_cards) bad_set++;
                        if (out.n_battles != truth->n_bat) flat_would_reshape++;
                        // A flat reading is only ever allowed where NO board
                        // could account for the cards. With the prior board
                        // taken from the step's committed trailer there is
                        // always one to try, so the only way through is a wire
                        // that under-names the pickup (this build's
                        // MAX_LOG_PAIRS truncates a 20-card table to 16).
                        int truth_cards = 0;
                        for (int k = 0; k < truth->n_bat; k++) {
                            if (truth->bat[2 * k] < 52) truth_cards++;
                            if (truth->bat[2 * k + 1] < 52) truth_cards++;
                        }
                        if (truth_cards == pt_stream[i].n_cards) bad_flat++;
                    }
                }
            }
        }
    }

    CHECK(sweeps > 200, "the sweep drives real games");
    CHECK(paired > 200, "and most sweeps come back as the board the kernel had");
    CHECK(bad_paired == 0, "a PAIRED answer is the kernel's own board, exactly");
    CHECK(bad_flat == 0, "an unpaired answer is a pickup's cards, each in its own cell, "
                         "and only where the wire named fewer cards than the table held");
    CHECK(bad_set == 0, "…all of them");
    // The flat reading is now reachable only where the WIRE cannot name the
    // table: this build's MAX_LOG_PAIRS is 16, so a 20-card pickup arrives
    // naming 16 cards and no board can account for them. (Production builds
    // with -DMAX_LOG_PAIRS=64 do not reach it at all on these games; the
    // reading stays, because a bigger table than 64 cards is still possible.)
    CHECK(flat > 0, "the flat reading is still reachable when the wire under-names a pickup");
    CHECK(flat_would_reshape > 0, "…and it reshapes the table when it fires");
    if (bad_paired || bad_flat || bad_set)
        printf("    (first bad sweep at flattened index %d of %ld)\n", first_bad, sweeps);
    printf("    pre-bout table: %ld sweeps, %ld paired, %ld flat (%ld reshaping)\n",
           sweeps, paired, flat, flat_would_reshape);
}

// ===========================================================================
// THE CONFLICT MODEL (anim_plan.h anim_conflict_*).
// ===========================================================================
//
// When something has to leave a board that no move took off it - a staged move
// an arrival overrides, a sequence a newer arrival supersedes - the board
// REVERSES it in red before the newest chain plays. What is pinned here is the
// trichotomy and, above all, its PRECEDENCE: CLEAR is decided before the
// standing sets, because a card the incoming stream moves may also stand on its
// opening table (a pickup's do by definition) and a red flight first is the
// "someone picked my card up and it flew back to my hand" flicker.
//
// MUTATION-CHECKED, each applied to c/src/anim_plan.c on its own and reverted:
//   CLEAR tested AFTER the standing sets (move the incoming_moved test below
//     the switch)                                                    -> 2 failures
//   the opening table takes only the ATTACK side (drop the cover)    -> 1 failure
//   ANIM_DEST_MY_HAND reads table_at_open instead of my_hand_at_open -> 2 failures
//   ANIM_DEST_POOL reverts instead of keeping                        -> 5 failures
//   a masked back (ANIM_CARD_NONE) reverts instead of keeping        -> 2 failures
//   the reversal walks groups FORWARD (g = 0 .. n_groups)            -> 2 failures
//   an empty group is emitted as a step of silence                   -> 3 failures
//   anim_conflict_dest sends a pickup by ANY seat to my hand         -> 2 failures

// The owner's stock cards, as dense ids.
#define CF_ID(suit, value) ((suit) * 13 + (value) - 1)

// EVERY TEST BELOW IS THE CHAIN TRANSPORT'S, which is the one that reads a
// board it already knows is superseded. The server transport's answers to the
// same inputs are pinned separately, in test_the_transport_is_the_only_thing_
// the_two_clients_disagree_about.
static int cf_verdict(int id, int dest, const AnimConflictFacts *f) {
    anim_set_transport(ANIM_TRANSPORT_CHAIN);
    return anim_conflict_verdict(id, dest, f, 0);
}

static AnimConflictFacts cf_facts(const int *moved, int n_moved,
                                  const unsigned char *table, int n_bat,
                                  const unsigned char *hand, int n_hand) {
    AnimConflictFacts f;
    CHECK(anim_conflict_facts(moved, n_moved, table, n_bat, hand, n_hand, &f) == ANIM_EOK,
          "the facts read the stream and the opening board");
    return f;
}

static void test_conflict_a_chain_that_disowns_a_card_flies_it_home(void) {
    // The canonical conflict: my staged, unsent card. No other device ever saw
    // it, so the arriving chain neither moves it nor shows it. A KEEP here is
    // the silent swap the owner rejected - the card is on the table one frame
    // and gone the next with nothing to say it was mine.
    const int mine = CF_ID(0, 9);
    const unsigned char other[2] = { CF_ID(1, 7), ANIM_TABLE_NONE };
    AnimConflictFacts f = cf_facts(NULL, 0, other, 1, NULL, 0);
    CHECK(cf_verdict(mine, ANIM_DEST_TABLE, &f) == ANIM_CONFLICT_REVERT,
          "a card the newest truth knows nothing of flies home");

    // The MERGE twin: a card standing at its post spot on the arriving chain's
    // OPENING board is vouched for and must not move. Flying it home only for
    // the incoming seed to snap it back is the flicker one board later.
    const unsigned char standing[2] = { mine, ANIM_TABLE_NONE };
    AnimConflictFacts s = cf_facts(NULL, 0, standing, 1, NULL, 0);
    CHECK(cf_verdict(mine, ANIM_DEST_TABLE, &s) == ANIM_CONFLICT_KEEP,
          "a card the newest board vouches for stays put");

    // BOTH SIDES of a battle stand. A cover read as "not on the table" would be
    // false-reverted off a table that is holding it.
    const int cover = CF_ID(2, 11);
    const unsigned char covered[2] = { mine, (unsigned char)cover };
    AnimConflictFacts c = cf_facts(NULL, 0, covered, 1, NULL, 0);
    CHECK(cf_verdict(cover, ANIM_DEST_TABLE, &c) == ANIM_CONFLICT_KEEP,
          "the cover on a standing battle is standing too");
}

static void test_conflict_clear_wins_over_the_standing_check(void) {
    // THE LEG THE CATALOGUE SAYS TO GET RIGHT FIRST, checked with the card ALSO
    // standing on the opening table - which is exactly what a picked-up card
    // does. The arriving replay taking it off the table IS its animation.
    const int seven = CF_ID(2, 7);
    const int moved[1] = { seven };
    const unsigned char table[2] = { (unsigned char)seven, ANIM_TABLE_NONE };
    AnimConflictFacts f = cf_facts(moved, 1, table, 1, NULL, 0);
    CHECK(cf_verdict(seven, ANIM_DEST_TABLE, &f) == ANIM_CONFLICT_CLEAR,
          "a card the arriving replay itself moves never flies home first");
    // …and from the other side too: my staged pickup put it in MY HAND and the
    // arrival is my own sent pickup coming back.
    const unsigned char hand[1] = { (unsigned char)seven };
    AnimConflictFacts h = cf_facts(moved, 1, NULL, 0, hand, 1);
    CHECK(cf_verdict(seven, ANIM_DEST_MY_HAND, &h) == ANIM_CONFLICT_CLEAR,
          "…whichever side of the board the doomed motion put it on");
}

static void test_conflict_the_standing_check_reads_the_side_it_landed_on(void) {
    // A staged pickup put the cards in MY HAND; the arriving chain showing them
    // on its TABLE is their PRE position, not their post one, so they must
    // still fly back out.
    const int picked = CF_ID(1, 11);
    const unsigned char table[2] = { (unsigned char)picked, ANIM_TABLE_NONE };
    AnimConflictFacts t = cf_facts(NULL, 0, table, 1, NULL, 0);
    CHECK(cf_verdict(picked, ANIM_DEST_MY_HAND, &t) == ANIM_CONFLICT_REVERT,
          "the arriving TABLE showing it does not vouch for my HAND holding it");
    const unsigned char hand[1] = { (unsigned char)picked };
    AnimConflictFacts h = cf_facts(NULL, 0, NULL, 0, hand, 1);
    CHECK(cf_verdict(picked, ANIM_DEST_MY_HAND, &h) == ANIM_CONFLICT_KEEP,
          "…and my hand holding it is what vouches for it");
    CHECK(cf_verdict(picked, ANIM_DEST_TABLE, &h) == ANIM_CONFLICT_REVERT,
          "…the mirror holds: a hand does not vouch for a table spot");
}

static void test_conflict_backs_and_pools_are_never_conjured(void) {
    // A chain nobody could read vouches for nothing - the honest default that
    // makes everything REVERT. Which makes this the strictest place to pin the
    // two things that must NOT fly even so.
    AnimConflictFacts none = cf_facts(NULL, 0, NULL, 0, NULL, 0);
    CHECK(cf_verdict(ANIM_CARD_NONE, ANIM_DEST_POOL, &none) == ANIM_CONFLICT_KEEP,
          "a masked back has no identity to conflict on and no view to fly back from");
    CHECK(cf_verdict(ANIM_CARD_NONE, ANIM_DEST_TABLE, &none) == ANIM_CONFLICT_KEEP,
          "…on any destination");
    CHECK(cf_verdict(CF_ID(0, 6), ANIM_DEST_POOL, &none) == ANIM_CONFLICT_KEEP,
          "a card that went into a pile is bookkeeping - no ghost comes back out of it");
    CHECK(cf_verdict(CF_ID(0, 6), ANIM_DEST_TABLE, &none) == ANIM_CONFLICT_REVERT,
          "…while an unvouched-for table card does fly, so the default is not 'keep all'");
}

static void test_conflict_dest_is_the_flight_builders_own_mapping(void) {
    // A wrong mapping sends the standing check to the wrong side of the board.
    CHECK(anim_conflict_dest(ANIM_EVT_ATTACK_PASS, 2, 0) == ANIM_DEST_TABLE, "a placement lands on the table");
    CHECK(anim_conflict_dest(ANIM_EVT_COVER, 2, 0) == ANIM_DEST_TABLE, "…so does a cover");
    CHECK(anim_conflict_dest(ANIM_EVT_DEFENDER_MOVE, 1, 0) == ANIM_DEST_TABLE, "…and a transfer");
    CHECK(anim_conflict_dest(ANIM_EVT_PICKUP, 0, 0) == ANIM_DEST_MY_HAND, "my pickup lands in my hand");
    CHECK(anim_conflict_dest(ANIM_EVT_REFILL, 0, 0) == ANIM_DEST_MY_HAND, "…so does my draw");
    CHECK(anim_conflict_dest(ANIM_EVT_DEAL, 0, 0) == ANIM_DEST_MY_HAND, "…and my deal");
    CHECK(anim_conflict_dest(ANIM_EVT_REFILL, 2, 0) == ANIM_DEST_POOL, "an opponent's draw lands in a badge");
    CHECK(anim_conflict_dest(ANIM_EVT_PICKUP, 2, 0) == ANIM_DEST_POOL, "…and so does their pickup");
    CHECK(anim_conflict_dest(ANIM_EVT_CARDS_TO_TRASH, -1, 0) == ANIM_DEST_POOL, "a sweep goes to the pile");
    CHECK(anim_conflict_dest(ANIM_EVT_OUT, 0, 0) == ANIM_DEST_POOL, "a notice moves nothing anywhere");
    CHECK(anim_conflict_dest(-1, 0, 0) == ANIM_DEST_POOL, "an unrecognised step moves nothing either");
}

static void test_conflict_reversal_flies_back_the_way_it_came(void) {
    anim_set_transport(ANIM_TRANSPORT_CHAIN);
    // "The cards travel back the way they came": last motion first, each group
    // one parallel step. A forward walk would un-happen the sequence in the
    // order it happened, which is not a reversal.
    const int a = CF_ID(0, 6), b = CF_ID(1, 8);
    AnimConflictFacts none = cf_facts(NULL, 0, NULL, 0, NULL, 0);
    AnimConflictMotion m[2] = { { a, ANIM_DEST_TABLE }, { b, ANIM_DEST_TABLE } };
    const int groups[2] = { 1, 1 };
    AnimConflictPlan p;
    CHECK(anim_conflict_reversal(m, 2, groups, 2, &none, &p) == 2, "both groups fly");
    CHECK(p.n_order == 2 && p.step_count[0] == 1 && p.step_count[1] == 1,
          "…one flight each");
    CHECK(p.order[0] == 1, "the LAST motion reverses FIRST");
    CHECK(p.order[1] == 0, "…and the first one last");
}

static void test_conflict_reversal_drops_what_the_verdicts_empty(void) {
    anim_set_transport(ANIM_TRANSPORT_CHAIN);
    // The verdicts gate the flights: in one flown group only the disowned card
    // flies - the one the replay moves (CLEAR) and the one its board vouches
    // for (KEEP) build nothing - and a group left with nothing is dropped
    // rather than played as a beat of silence.
    const int disowned = CF_ID(0, 9), cleared = CF_ID(1, 7), standing = CF_ID(2, 10);
    const int moved[1] = { cleared };
    const unsigned char table[2] = { (unsigned char)standing, ANIM_TABLE_NONE };
    AnimConflictFacts f = cf_facts(moved, 1, table, 1, NULL, 0);

    AnimConflictMotion m[4] = {
        { disowned, ANIM_DEST_TABLE }, { cleared, ANIM_DEST_TABLE },
        { standing, ANIM_DEST_TABLE }, { CF_ID(3, 12), ANIM_DEST_POOL },
    };
    const int groups[2] = { 3, 1 };
    AnimConflictPlan p;
    CHECK(anim_conflict_reversal(m, 4, groups, 2, &f, &p) == 1,
          "the all-keep pool group plays no beat of silence");
    CHECK(p.n_order == 1 && p.order[0] == 0 && p.step_count[0] == 1,
          "…and only the disowned card flies out of the group that does");
    CHECK(p.n_verdicts == 4
          && p.verdicts[0] == ANIM_CONFLICT_REVERT
          && p.verdicts[1] == ANIM_CONFLICT_CLEAR
          && p.verdicts[2] == ANIM_CONFLICT_KEEP
          && p.verdicts[3] == ANIM_CONFLICT_KEEP,
          "the per-motion verdicts come back too - the board asks the same call for both");

    // Nothing at all to reverse is a plan with no steps, not an error: a burst
    // of arrivals that each EXTEND the animating chain lands here for almost
    // every card, which is the common case and must play no theatre.
    const int all_moved[3] = { disowned, cleared, standing };
    AnimConflictFacts vouched = cf_facts(all_moved, 3, NULL, 0, NULL, 0);
    CHECK(anim_conflict_reversal(m, 4, groups, 2, &vouched, &p) == 0 && p.n_order == 0,
          "a chain that vouches for everything reverses nothing");
}

static void test_conflict_degenerate_inputs(void) {
    anim_set_transport(ANIM_TRANSPORT_CHAIN);
    AnimConflictFacts f;
    CHECK(anim_conflict_facts(NULL, 0, NULL, 0, NULL, 0, NULL) == ANIM_EBADARG, "no out, no facts");
    CHECK(anim_conflict_facts(NULL, 1, NULL, 0, NULL, 0, &f) == ANIM_EBADARG, "a count with no array");
    CHECK(anim_conflict_facts(NULL, 0, NULL, 0, NULL, 0, &f) == ANIM_EOK
          && f.incoming_moved == 0 && f.table_at_open == 0 && f.my_hand_at_open == 0,
          "a chain that could not be read vouches for nothing");
    // An out-of-range id in the stream is dropped rather than folded into some
    // other card's bit - a wire this rule cannot name must not name the wrong one.
    const int junk[2] = { 52, -7 };
    CHECK(anim_conflict_facts(junk, 2, NULL, 0, NULL, 0, &f) == ANIM_EOK && f.incoming_moved == 0,
          "an id off the deck names nothing");

    CHECK(cf_verdict(0, ANIM_DEST_TABLE, NULL) == ANIM_EBADARG, "no facts, no verdict");
    CHECK(cf_verdict(52, ANIM_DEST_TABLE, &f) == ANIM_EBADARG, "no card, no verdict");
    CHECK(cf_verdict(0, 99, &f) == ANIM_EBADARG, "no destination, no verdict");

    AnimConflictMotion m[2] = { { 0, ANIM_DEST_TABLE }, { 1, ANIM_DEST_TABLE } };
    int groups[2] = { 1, 1 };
    AnimConflictPlan p;
    CHECK(anim_conflict_reversal(m, 2, groups, 2, &f, NULL) == ANIM_EBADARG, "no out, no plan");
    CHECK(anim_conflict_reversal(m, 2, groups, 2, NULL, &p) == ANIM_EBADARG, "no facts, no plan");
    CHECK(anim_conflict_reversal(NULL, 2, groups, 2, &f, &p) == ANIM_EBADARG, "a count with no motions");
    CHECK(anim_conflict_reversal(m, 0, NULL, 0, &f, &p) == 0, "nothing flown, nothing to reverse");
    CHECK(anim_conflict_reversal(m, 2, groups, 1, &f, &p) == ANIM_EBADARG,
          "groups that leave a motion out describe some other sequence");
    groups[1] = 5;
    CHECK(anim_conflict_reversal(m, 2, groups, 2, &f, &p) == ANIM_EBADARG,
          "…and so do groups that run past the end of it");
    groups[1] = -1;
    CHECK(anim_conflict_reversal(m, 2, groups, 2, &f, &p) == ANIM_EBADARG, "a negative group is not a group");
    groups[1] = 1;
    CHECK(anim_conflict_reversal(m, ANIM_MAX_CONFLICT_MOTIONS + 1, groups, 2, &f, &p) == ANIM_ECAP,
          "a sequence over the cap is refused rather than half-reversed");
    CHECK(anim_conflict_reversal(m, 2, groups, ANIM_MAX_CONFLICT_GROUPS + 1, &f, &p) == ANIM_ECAP,
          "…and so are too many groups");
}

// ---------------------------------------------------------------------------
// THE TRANSPORT, and the exactly-one thing it changes.
//
// Both clients run this rule, and the sets it reads are the same sets. What a
// server broadcast cannot say is that a card it does not mention was REJECTED -
// that card's own confirmation is a separate later broadcast, and reading
// silence as a verdict is the card-out / card-home-in-red / card-out-again
// stutter. A chain has no such gap: it is complete and totally ordered.
//
// So this pins two things at once: that the two transports DISAGREE where they
// must, and that they AGREE everywhere else, which is the whole claim of
// writing the rule once.
//
// MUTATION-CHECKED, each applied to c/src/anim_plan.c on its own:
//   the unset transport falls through to REVERT instead of ANIM_ETRANSPORT
//                                                                  -> 1 failure
//   the server transport reverts too (drop the server_may_still_accept
//     arm)                                   -> 2 here, 4 in anim_plan_test.c
//   server_may_still_accept ignores table_cleared                  -> 1 failure
//   the CLEAR test moves BELOW the standing tests                  -> 4 failures
static void test_the_transport_is_the_only_thing_the_two_clients_disagree_about(void) {
    // The stage-6 case, as ids: my attack is in flight, the newest news shows
    // the rival's card and not mine, and nothing has swept the table.
    const int mine = CF_ID(0, 9), rival = CF_ID(1, 7);
    const unsigned char table[2] = { (unsigned char)rival, ANIM_TABLE_NONE };
    const int moved[1] = { rival };
    AnimConflictFacts f = cf_facts(moved, 1, table, 1, NULL, 0);
    // The defender can still hold my attack, so the server has hope for it.
    AnimServerHope hope = { 0, 0, 1, 6, 1 };

    CHECK(anim_set_transport(ANIM_TRANSPORT_UNSET) == ANIM_EOK, "nothing said yet");
    CHECK(anim_transport() == ANIM_TRANSPORT_UNSET, "…and it reads back, which is the diagnostic");
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, &hope) == ANIM_ETRANSPORT,
          "a verdict that reaches the question with nobody having said is an ERROR, "
          "not a guess at which client this is");

    CHECK(anim_set_transport(ANIM_TRANSPORT_CHAIN) == ANIM_EOK, "iMessage says so");
    CHECK(anim_transport() == ANIM_TRANSPORT_CHAIN, "and can be asked");
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, 0) == ANIM_CONFLICT_REVERT,
          "a chain is complete: a card it does not account for is doomed");

    CHECK(anim_set_transport(ANIM_TRANSPORT_SERVER) == ANIM_EOK, "the web says otherwise");
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, &hope) == ANIM_CONFLICT_KEEP,
          "a broadcast is a partial delta: my card's own confirmation is still in the post");
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, NULL) == ANIM_EBADARG,
          "…and a server that cannot say is not a server that says no");

    // The server's hope is not unconditional. A sweep that took the table did
    // not name this card, so it never got there.
    AnimServerHope swept = hope; swept.table_cleared = 1;
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, &swept) == ANIM_CONFLICT_REVERT,
          "a card the sweep did not name never reached the table");
    AnimServerHope over = hope; over.pending_attacks = 6;
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, &over) == ANIM_CONFLICT_REVERT,
          "…and neither did one the defender could never have held");
    AnimServerHope cover = over; cover.is_cover = 1;
    CHECK(anim_conflict_verdict(mine, ANIM_DEST_TABLE, &f, &cover) == ANIM_CONFLICT_KEEP,
          "the capacity rule is an ATTACK rule; the defender's own play is exempt");

    // EVERYTHING BEFORE THE QUESTION IS ONE IMPLEMENTATION. Each of these is
    // answered before the transport is ever consulted, so both must agree - and
    // the hopeless hope proves the server is not reaching the question at all.
    AnimServerHope none = { 0, 1, 9, 0, 9 };   // says "no" to everything
    const int standing = rival, back = ANIM_CARD_NONE;
    const struct { int id; int dest; int want; const char *what; } same[] = {
        { rival,    ANIM_DEST_TABLE,   ANIM_CONFLICT_CLEAR, "a card the stream itself moves" },
        { standing, ANIM_DEST_TABLE,   ANIM_CONFLICT_CLEAR, "…even where it also stands" },
        { mine,     ANIM_DEST_POOL,    ANIM_CONFLICT_KEEP,  "a card that went into a pile" },
        { back,     ANIM_DEST_TABLE,   ANIM_CONFLICT_KEEP,  "a masked back" },
        { mine,     ANIM_DEST_MY_HAND, ANIM_CONFLICT_REVERT, "a hand the board does not show it in" },
    };
    for (int i = 0; i < (int)(sizeof same / sizeof same[0]); i++) {
        if (same[i].want == ANIM_CONFLICT_REVERT) continue;   // that one IS the question
        anim_set_transport(ANIM_TRANSPORT_CHAIN);
        const int a = anim_conflict_verdict(same[i].id, same[i].dest, &f, 0);
        anim_set_transport(ANIM_TRANSPORT_SERVER);
        const int b = anim_conflict_verdict(same[i].id, same[i].dest, &f, &none);
        if (!(a == same[i].want && b == same[i].want))
            printf("    (diverged: %s -> chain %d, server %d)\n", same[i].what, a, b);
        CHECK(a == same[i].want && b == same[i].want,
              "everything before the transport question is the same answer on both");
    }

    // A reversal is a chain-only question - it is handed motions the caller
    // already knows are doomed, which only a total order can know.
    AnimConflictMotion m[1] = { { mine, ANIM_DEST_TABLE } };
    int groups[1] = { 1 };
    AnimConflictPlan p;
    CHECK(anim_conflict_reversal(m, 1, groups, 1, &f, &p) == ANIM_EBADARG,
          "the server transport has no doomed-motion list to reverse");
    anim_set_transport(ANIM_TRANSPORT_CHAIN);
    CHECK(anim_conflict_reversal(m, 1, groups, 1, &f, &p) == 1, "…and the chain does");

    CHECK(anim_set_transport(7) == ANIM_EBADARG, "a transport nobody defined is refused");
    CHECK(anim_transport() == ANIM_TRANSPORT_CHAIN, "…and does not land");
}


// ---- the board's own sets and small rules ---------------------------------
//
// The last pure statics on MessageTableView, lifted whole. Card sets are u64
// bitsets over dense ids, so the crossing that can go wrong is the ID, and it
// goes wrong invisibly: both sides of every comparison go through the same
// encoder, so an off-by-one is only visible at the LAST card of the deck. Every
// test below that names a card names 51 as well as 0.
//
// MUTATIONS RUN (each applied, rebuilt from scratch, then put back). The
// harness asserts the SUITE ACTUALLY RAN - a "N passed, M failed" line - because
// a build failure prints no failures and had been counted as a pass once
// already, and it re-stamps every C source first because two `make`s in the same
// second in c/build can skip the rebuild and report the last run's numbers.
//
//   1  anim_veil_veiled drops the live-play source            -> 2 failures
//   2  ...ORs hand_before instead of subtracting it           -> 2
//   3  ...honours my_hand without has_hand_before             -> 1
//   4  anim_veil_flying returns `hidden` whole                -> 3
//   5  anim_veil_hand_slot_deferred drops the holdback term   -> 3
//   6  anim_veil_fan returns `veiled` whole                   -> 2
//   7  anim_veil_grid uses `veiled` on the sweeping branch    -> 2
//   8  anim_veil_teardown carries the orphans, not its opens  -> 2
//   9  ...reveals on a superseded sequence                    -> 1
//  10  anim_veil_handover reveals the placing cards too       -> 2
//  11  anim_veil_unstarted_replay ignores replay_pending      -> 1
//  12  anim_holdback_is_mine uses < instead of <=             -> 1
//  13  anim_selection_after_tap skips the stale sweep         -> 2
//  14  ...inserts a card that is not in my hand               -> 3
//  15  anim_is_placement counts a refill                      -> 2
//  16  anim_is_my_placement drops the my_seat < 0 guard       -> 1
//  17  anim_fan_cards appends a held card unconditionally     -> 6
//  18  anim_laid_count counts the kernel hand, not the fan    -> 2
//  19  anim_hand_laid_out places a deferred card from `order` -> 4
//  20  anim_table_covers lets an unnameable cell vanish       -> 3
//  21  ap_bit lets a table sentinel name a card (id < 255)    -> 1
//  22  anim_table_covers tests the sets the other way round   -> 6
//  23  anim_covered_sweep_accepts drops the `paired` test     -> 2
//  24  anim_shown_table prefers the sweep to the live table   -> 1
//  25  anim_finish_rows gives the fool place n_rows           -> 1
//  26  anim_shown_ledger_allows refuses `arming` too          -> 1
//  27  ap_bit accepts id 52 (the last-card off-by-one)        -> 1
//
// ONE SURVIVED AND WAS REMOVED RATHER THAN TESTED. anim_table_card_ids used to
// skip a cover cell equal to ANIM_TABLE_NONE before asking for its bit, which no
// input could tell apart from asking anyway - the sentinel is off the deck, so
// the range check already answers zero. Unkillable code is not defence in depth,
// it is a second rule nobody is checking; the branch is gone and a
// _Static_assert in the header keeps both sentinels off the deck instead.
//
// A dense id, spelled out here rather than borrowed, so a change to the
// encoder anywhere else fails these tests instead of moving with them.
#define BID(suit, value) ((unsigned char)((suit) * 13 + ((value) - 1)))
#define BSET(id) ((uint64_t)1 << (id))

static void test_board_veil_unions_its_three_sources(void) {
    const unsigned char six = BID(0, 6), nine = BID(1, 9), ace = BID(3, 13);

    // Nothing veiled is nothing veiled.
    CHECK(anim_veil_veiled(0, 0, 0, 0, 0, 0) == 0, "an idle board veils nothing");

    // The animator's own set, and an unstarted replay's, both count.
    CHECK(anim_veil_veiled(BSET(six), 0, 0, 0, 0, 0) == BSET(six),
          "a card the animator hid is veiled");
    CHECK(anim_veil_veiled(0, BSET(nine), 0, 0, 0, 0) == BSET(nine),
          "a card an unstarted replay will move is veiled before it starts");
    CHECK(anim_veil_veiled(BSET(six), BSET(nine), 0, 0, 0, 0) == (BSET(six) | BSET(nine)),
          "the sources are UNIONED, not `whichever is up`");

    // Live play: what this move just put in my hand, and nothing else in it.
    const uint64_t before = BSET(six);
    const uint64_t now = BSET(six) | BSET(nine);
    CHECK(anim_veil_veiled(0, 0, 1, before, 1, now) == BSET(nine),
          "a card THIS move put in my hand is veiled; the ones already there are not");
    CHECK(anim_veil_veiled(0, 0, 1, now, 1, now) == 0,
          "a hand that did not change veils nothing");

    // Both halves of the live source are required. A spectator has no fan.
    CHECK(anim_veil_veiled(0, 0, 0, before, 1, now) == 0,
          "with no pre-move hand there is nothing to diff against");
    CHECK(anim_veil_veiled(0, 0, 1, before, 0, 0) == 0,
          "and a board with no hand of its own veils none of it");

    // The last card of the deck, which is where an id off by one bites.
    CHECK(anim_veil_veiled(BSET(ace), 0, 0, 0, 0, 0) == BSET(ace),
          "the last card of the deck veils like every other");
}

static void test_board_veil_flying_is_hidden_minus_pre_hidden(void) {
    const unsigned char six = BID(0, 6), nine = BID(1, 9);
    CHECK(anim_veil_flying(BSET(six) | BSET(nine), BSET(nine)) == BSET(six),
          "a card taken out of pre-hidden alone is the one in the air");
    CHECK(anim_veil_flying(BSET(six), BSET(six)) == 0,
          "a card pre-hidden for a step not yet reached is NOT flying");
    CHECK(anim_veil_flying(BSET(six), 0) == BSET(six), "nothing pre-hidden, everything hidden flies");
    CHECK(anim_veil_flying(0, BSET(six)) == 0, "pre-hidden without hidden cannot happen and flies nothing");
}

static void test_board_veil_the_fan_never_withholds_a_slot_it_draws(void) {
    const unsigned char six = BID(0, 6), nine = BID(1, 9), ace = BID(3, 13);
    const uint64_t veiled = BSET(six) | BSET(nine) | BSET(ace);

    // A held-back card is the one veiled card the fan DOES draw, so it keeps
    // its slot: without that term the hand renders closed.
    CHECK(anim_veil_hand_slot_deferred(veiled, 0, BSET(nine)) == (BSET(six) | BSET(ace)),
          "a held-back card is never deferred");
    CHECK(anim_veil_hand_slot_deferred(veiled, BSET(six), 0) == (BSET(nine) | BSET(ace)),
          "and neither is the card whose flight is playing this instant");
    CHECK(anim_veil_fan(veiled, BSET(nine)) == (BSET(six) | BSET(ace)),
          "the fan draws the held-back card and nothing else that is veiled");

    // THE INVARIANT, over every combination of the three sets that matters:
    // the deferral is a subset of the fan's veil, i.e. the fan never withholds
    // a slot from a card it is drawing.
    int checked = 0, held = 1;
    for (int v = 0; v < 8; v++) {
        for (int f = 0; f < 8; f++) {
            for (int h = 0; h < 8; h++) {
                uint64_t sv = 0, sf = 0, sh = 0;
                const unsigned char ids[3] = { six, nine, ace };
                for (int i = 0; i < 3; i++) {
                    if (v & (1 << i)) sv |= BSET(ids[i]);
                    if (f & (1 << i)) sf |= BSET(ids[i]);
                    if (h & (1 << i)) sh |= BSET(ids[i]);
                }
                const uint64_t deferred = anim_veil_hand_slot_deferred(sv, sf, sh);
                const uint64_t hidden = anim_veil_fan(sv, sh);
                if ((deferred & ~hidden) != 0) held = 0;
                checked++;
            }
        }
    }
    CHECK(checked == 512 && held, "the deferral is always a subset of the fan's veil");
}

static void test_board_veil_grid_answers_off_different_state_when_sweeping(void) {
    const unsigned char six = BID(0, 6), nine = BID(1, 9), ace = BID(3, 13);
    uint64_t hidden = 0, flying = 0;

    anim_veil_grid(0, BSET(six), BSET(nine), BSET(ace), BSET(nine), BSET(six),
                   &hidden, &flying);
    CHECK(hidden == BSET(six) && flying == BSET(six),
          "a live grid reads the hand veil and the cards in the air");

    anim_veil_grid(1, BSET(six), BSET(nine), BSET(ace), BSET(six), BSET(six),
                   &hidden, &flying);
    CHECK(hidden == (BSET(nine) | BSET(ace)) && flying == BSET(six),
          "a sweeping grid reads its OWN two sets, one per end of the sequence");

    // THE PICKUP. The same card is legitimately hidden on one grid and drawn on
    // the other in the same paint, which is why the branches are not one filter:
    // the hand veil hides a picked-up card, and honouring that on the sweeping
    // grid would take the table copy away before its own flight lifted it.
    anim_veil_grid(1, BSET(six), 0, 0, 0, 0, &hidden, &flying);
    CHECK(hidden == 0, "a sweeping grid does not honour the hand veil");
    anim_veil_grid(0, BSET(six), 0, 0, 0, 0, &hidden, &flying);
    CHECK(hidden == BSET(six), "…while the live grid does");

    // Either out may be NULL; a caller that wants one set should not have to
    // find somewhere to put the other.
    anim_veil_grid(0, BSET(six), 0, 0, 0, 0, NULL, NULL);
    CHECK(1, "a NULL out is not a crash");
}

static void test_board_veil_teardown_and_handover(void) {
    const unsigned char a = BID(0, 6), b = BID(1, 9), c = BID(3, 13);
    uint64_t reveal = 0, carry = 0;

    anim_veil_teardown(BSET(a) | BSET(b), BSET(c), 1, &reveal, &carry);
    CHECK(reveal == (BSET(a) | BSET(b) | BSET(c)) && carry == 0,
          "the newest sequence reveals its own opens and every orphan handed to it");

    anim_veil_teardown(BSET(a), BSET(b) | BSET(c), 0, &reveal, &carry);
    CHECK(reveal == 0 && carry == (BSET(a) | BSET(b) | BSET(c)),
          "a superseded one reveals nothing and passes its opens ON");

    // The defect the carry exists for: an opened card is out of the pre-hidden
    // set, so the blanket net can no longer reach it and nothing else would.
    uint64_t held = 0;
    anim_veil_teardown(BSET(a), 0, 0, &reveal, &held);
    anim_veil_teardown(BSET(b), held, 1, &reveal, &carry);
    CHECK(reveal == (BSET(a) | BSET(b)) && carry == 0,
          "an orphan handed on through a superseded teardown is revealed by the last one standing");

    anim_veil_handover(BSET(a), BSET(b), &reveal, &carry);
    CHECK(reveal == BSET(a) && carry == BSET(b),
          "a new play takes the standing veil down and raises its own");
    anim_veil_handover(BSET(a) | BSET(b), BSET(b) | BSET(c), &reveal, &carry);
    CHECK(reveal == BSET(a) && carry == (BSET(b) | BSET(c)),
          "…except the cards it is placing, which revealing would flash back into the fan");
}

static void test_board_veil_small_decisions(void) {
    CHECK(anim_veil_unstarted_replay(1, 3) == 1, "a pending replay with events is unstarted");
    CHECK(anim_veil_unstarted_replay(1, 0) == 0, "a pending flag with no events veils nothing");
    CHECK(anim_veil_unstarted_replay(0, 3) == 0,
          "events with the flag down belong to a replay already running - ignore this and the veil never lifts");
    CHECK(anim_veil_unstarted_replay(0, 0) == 0, "and an idle board has no replay");

    CHECK(anim_holdback_is_mine(7, 7) == 1, "equal epochs ARE the same sequence");
    CHECK(anim_holdback_is_mine(4, 7) == 1, "a holdback armed before this veil is this teardown's");
    CHECK(anim_holdback_is_mine(8, 7) == 0, "one armed after it belongs to the sequence that replaced this one");

    CHECK(anim_shown_ledger_allows(ANIM_CLAIM_SEQUENCE, 1) == 1
          && anim_shown_ledger_allows(ANIM_CLAIM_ARMING, 1) == 1
          && anim_shown_ledger_allows(ANIM_CLAIM_HAND_OFF, 1) == 1,
          "the three shapes of the owner are never refused - guarding them freezes every badge");
    CHECK(anim_shown_ledger_allows(ANIM_CLAIM_BYSTANDER, 1) == 0,
          "a bystander stands down while a sequence is running");
    CHECK(anim_shown_ledger_allows(ANIM_CLAIM_BYSTANDER, 0) == 1,
          "…and writes exactly as it always did on a board at rest");
}

static void test_board_selection_may_only_name_cards_in_my_hand(void) {
    const unsigned char six = BID(0, 6), nine = BID(1, 9), ace = BID(3, 13);
    const uint64_t hand = BSET(six) | BSET(nine) | BSET(ace);

    CHECK(anim_selection_after_tap(0, six, hand) == BSET(six), "a tap selects");
    CHECK(anim_selection_after_tap(BSET(six), six, hand) == 0, "and a second tap deselects");
    CHECK(anim_selection_after_tap(BSET(six), nine, hand) == (BSET(six) | BSET(nine)),
          "two cards can be selected at once");

    // A card that is drawn in the fan but no longer in the kernel hand (the
    // holdback) cannot enter the selection at all - the state the action bar
    // silently gated off is not representable.
    CHECK(anim_selection_after_tap(0, ace, BSET(six)) == 0,
          "tapping a card that is not in my hand selects nothing");
    // …and an identity that has since left sweeps out whatever the tap was for.
    CHECK(anim_selection_after_tap(BSET(nine) | BSET(six), ace, BSET(six)) == BSET(six),
          "a stale selection is swept even by a tap that does nothing");
    CHECK(anim_selection_after_tap(BSET(nine), six, BSET(six)) == BSET(six),
          "…and by one that does");

    // Every card of the deck, both ways: an id off by one is invisible until
    // the last one, where the selection silently stops working.
    int ok = 1;
    for (int id = 0; id < 52; id++) {
        const uint64_t only = BSET(id);
        if (anim_selection_after_tap(0, id, only) != only) ok = 0;
        if (anim_selection_after_tap(only, id, only) != 0) ok = 0;
        if (anim_selection_after_tap(0, id, ~only & 0xFFFFFFFFFFFFFULL) != 0) ok = 0;
    }
    CHECK(ok, "all 52 cards select, deselect and refuse the same way");
    CHECK(anim_selection_after_tap(0, 52, ~(uint64_t)0) == 0,
          "and a card off the deck - a masked back - selects nothing");
    CHECK(anim_selection_after_tap(0, -1, ~(uint64_t)0) == 0, "nor does one with no id at all");
}

static void test_board_placement_is_a_hand_reaching_the_table(void) {
    CHECK(anim_is_placement(ANIM_EVT_ATTACK_PASS) == 1
          && anim_is_placement(ANIM_EVT_DEFENDER_MOVE) == 1
          && anim_is_placement(ANIM_EVT_COVER) == 1,
          "the three ways a card leaves a hand for the table are placements");
    CHECK(anim_is_placement(ANIM_EVT_REFILL) == 0 && anim_is_placement(ANIM_EVT_DEAL) == 0
          && anim_is_placement(ANIM_EVT_PICKUP) == 0 && anim_is_placement(ANIM_EVT_DISCARD) == 0
          && anim_is_placement(ANIM_EVT_OUT) == 0 && anim_is_placement(ANIM_EVT_CARDS_TO_TRASH) == 0
          && anim_is_placement(ANIM_EVT_MAGIC_TRANSITION) == 0
          && anim_is_placement(ANIM_EVT_FLIPPED) == 0 && anim_is_placement(-1) == 0,
          "nothing else is, including a step this board cannot name");

    CHECK(anim_is_my_placement(ANIM_EVT_COVER, 2, 2) == 1, "my own cover takes a card out of my hand");
    CHECK(anim_is_my_placement(ANIM_EVT_COVER, 3, 2) == 0, "another seat's does not");
    CHECK(anim_is_my_placement(ANIM_EVT_REFILL, 2, 2) == 0, "and neither does a step that is no placement");
    // A discard and a bout transition both carry seat -1, and so does a
    // spectator: without the guard a seatless viewer claims them as its own.
    CHECK(anim_is_my_placement(ANIM_EVT_COVER, -1, -1) == 0, "a spectator places nothing");
    CHECK(anim_is_my_placement(ANIM_EVT_DISCARD, -1, -1) == 0, "…including the steps that carry no seat");
}

static void test_board_the_fan_lays_out_the_hand_it_is_given(void) {
    unsigned char out[64];
    const unsigned char hand[3] = { BID(0, 6), BID(1, 9), BID(3, 13) };
    const unsigned char held[2] = { BID(2, 7), BID(1, 9) };

    CHECK(anim_fan_cards(hand, 3, NULL, 0, out, 64) == 3
          && out[0] == hand[0] && out[1] == hand[1] && out[2] == hand[2],
          "nothing held back, the fan lays out the hand unchanged");

    // A held-back card appears ONCE. The fan places by index, so an identity
    // the kernel hand already carries would be two cards in one slot.
    const int n = anim_fan_cards(hand, 3, held, 2, out, 64);
    CHECK(n == 4 && out[3] == BID(2, 7),
          "a card the hand does not hold is appended; one it already holds is not doubled");
    CHECK(anim_fan_cards(NULL, 0, held, 2, out, 64) == 2, "an empty hand lays out the holdback alone");
    CHECK(anim_fan_cards(NULL, 0, NULL, 0, out, 64) == 0, "and an empty board lays out nothing");

    // The count the fan actually draws: the hand it is really given, minus the
    // deals still deferring their slot.
    CHECK(anim_laid_count(hand, 3, held, 2, 0) == 4, "everything laid out is counted");
    CHECK(anim_laid_count(hand, 3, NULL, 0, 0) == 3, "…and the count follows the FAN, not the kernel hand");
    CHECK(anim_laid_count(hand, 3, held, 2, BSET(BID(0, 6)) | BSET(BID(2, 7))) == 2,
          "a deferred card reserves no width");
    CHECK(anim_laid_count(hand, 3, held, 2, BSET(BID(1, 9))) == 3,
          "and a held-back duplicate is still only one card to defer");
}

static void test_board_hand_laid_out_follows_the_local_arrangement(void) {
    unsigned char out[64];
    const unsigned char a = BID(0, 6), b = BID(1, 9), c = BID(3, 13), d = BID(2, 2);
    const unsigned char cards[3] = { a, b, c };

    CHECK(anim_hand_laid_out(cards, 3, 0, NULL, 0, out, 64) == 3
          && out[0] == a && out[1] == b && out[2] == c,
          "with no arrangement the hand keeps kernel order");

    const unsigned char reversed[3] = { c, b, a };
    CHECK(anim_hand_laid_out(cards, 3, 0, reversed, 3, out, 64) == 3
          && out[0] == c && out[1] == b && out[2] == a,
          "the local arrangement decides the order of the cards it knows");

    // A card the arrangement has never seen appends in kernel order, at the
    // right - a pickup or a draw lands rightmost.
    const unsigned char partial[2] = { c, a };
    CHECK(anim_hand_laid_out(cards, 3, 0, partial, 2, out, 64) == 3
          && out[0] == c && out[1] == a && out[2] == b,
          "a card the arrangement does not know appends after the ones it does");

    // Stale ids (a played card the sticky memory still remembers) and repeats
    // drop out by construction rather than conjuring a card into the fan.
    const unsigned char stale[5] = { d, c, c, a, d };
    CHECK(anim_hand_laid_out(cards, 3, 0, stale, 5, out, 64) == 3
          && out[0] == c && out[1] == a && out[2] == b,
          "an arrangement naming cards that are not in the hand places none of them");

    // A deferred card reserves nothing, and the arrangement must not place it
    // either - the round-12 bug was the check and the flights describing
    // different arrays.
    CHECK(anim_hand_laid_out(cards, 3, BSET(b), reversed, 3, out, 64) == 2
          && out[0] == c && out[1] == a,
          "a deferred card is out of the laid-out hand, arrangement or no arrangement");
    CHECK(anim_hand_laid_out(cards, 3, BSET(a) | BSET(b) | BSET(c), reversed, 3, out, 64) == 0,
          "and a wholly deferred hand lays out nothing");
}

static void test_board_the_table_under_the_sweep(void) {
    // 2 bytes per battle: the attack, then its cover or ANIM_TABLE_NONE.
    const unsigned char a = BID(0, 6), b = BID(1, 9), c = BID(3, 13), d = BID(2, 2);
    const unsigned char uncovered[2] = { a, ANIM_TABLE_NONE };
    const unsigned char covered[4] = { a, b, c, ANIM_TABLE_NONE };

    CHECK(anim_table_card_ids(uncovered, 1) == BSET(a), "an uncovered attack names one card");
    CHECK(anim_table_card_ids(covered, 2) == (BSET(a) | BSET(b) | BSET(c)),
          "a covered battle names BOTH sides");
    CHECK(anim_table_card_ids(NULL, 0) == 0, "an empty table names none");
    CHECK(anim_table_card_ids(covered, 0) == 0, "and neither does a table with no battles on it");

    // The subset test, in the direction the two table choices need: does the
    // candidate account for everything the board already holds?
    CHECK(anim_table_covers(covered, 2, uncovered, 1) == 1,
          "a table that adds the cover accounts for the one that did not have it");
    CHECK(anim_table_covers(uncovered, 1, covered, 2) == 0,
          "…and not the other way round, which would drop a covered pair mid-sweep");
    CHECK(anim_table_covers(covered, 2, NULL, 0) == 1, "everything accounts for nothing");

    // A CARD NOBODY CAN NAME. It has no bit, so without a rule of its own it
    // would drop out of the subset test and the swap would be granted over a
    // table that is really losing a card. On the INNER side it refuses; on the
    // outer it simply names nothing, which is at least as conservative.
    const unsigned char unnameable[2] = { ANIM_TABLE_UNKNOWN, ANIM_TABLE_NONE };
    CHECK(anim_table_covers(covered, 2, unnameable, 1) == 0,
          "a card the caller cannot name is never accounted for");
    const unsigned char hidden_cover[2] = { a, ANIM_TABLE_UNKNOWN };
    CHECK(anim_table_covers(covered, 2, hidden_cover, 1) == 0, "…on either side of a battle");
    CHECK(anim_table_covers(unnameable, 1, uncovered, 1) == 0,
          "and a table that holds one accounts for nothing else either");
    CHECK(anim_covered_sweep_accepts(1, covered, 2, unnameable, 1) == 0,
          "so the sweep is refused rather than allowed to drop it");

    // coveredSweep. `paired` is not redundant with the subset test: the flat
    // reading names exactly the same cards in a shape nobody vouched for, and
    // the grid animates every card into its new cell before anything flies off.
    CHECK(anim_covered_sweep_accepts(1, covered, 2, uncovered, 1) == 1,
          "a real board that adds the cover earns the swap");
    CHECK(anim_covered_sweep_accepts(0, covered, 2, uncovered, 1) == 0,
          "the FLAT reading is refused even though it names the same cards");
    CHECK(anim_covered_sweep_accepts(1, NULL, 0, uncovered, 1) == 0, "and so is an empty table");
    CHECK(anim_covered_sweep_accepts(1, uncovered, 1, covered, 2) == 0,
          "a stream that came back short is refused rather than allowed to drop a pair");

    // The one flat table that names the same cards, spelled out: two battles,
    // one covered, read as three uncovered cells.
    const unsigned char flat[6] = { a, ANIM_TABLE_NONE, b, ANIM_TABLE_NONE, c, ANIM_TABLE_NONE };
    CHECK(anim_table_card_ids(flat, 3) == anim_table_card_ids(covered, 2),
          "the flat reading has the right cards…");
    CHECK(anim_covered_sweep_accepts(0, flat, 3, covered, 2) == 0, "…and is still refused");
    (void)d;

    // Which table the grid paints, on emptiness alone.
    int sweeping = -1;
    CHECK(anim_shown_table(2, 3, 4, &sweeping) == ANIM_SHOWN_LIVE && sweeping == 0,
          "the live table wins, and is not a sweep");
    CHECK(anim_shown_table(0, 3, 4, &sweeping) == ANIM_SHOWN_SWEEP && sweeping == 1,
          "then the sweep a move of my own captured");
    CHECK(anim_shown_table(0, 0, 4, &sweeping) == ANIM_SHOWN_PENDING && sweeping == 1,
          "then an arriving replay's pre-bout table, which lands a paint before the sweep is set");
    CHECK(anim_shown_table(0, 0, 0, &sweeping) == ANIM_SHOWN_NONE && sweeping == 0,
          "and an empty board is empty, not sweeping");
    CHECK(anim_shown_table(1, 0, 0, NULL) == ANIM_SHOWN_LIVE, "a NULL out is not a crash");
}

static void test_board_finish_rows_rank_first_out_to_the_fool(void) {
    AnimFinishRow rows[MAX_PLAYERS];
    const unsigned char order[3] = { 2, 0, 3 };

    int n = anim_finish_rows(order, 3, 1, 4, 0, rows, MAX_PLAYERS);
    CHECK(n == 4, "everyone gets a row");
    CHECK(rows[0].place == 1 && rows[0].seat == 2 && rows[0].is_you == 0,
          "rank 1 is the first player out");
    CHECK(rows[1].place == 2 && rows[1].seat == 0 && rows[1].is_you == 1, "and my row is marked");
    CHECK(rows[3].place == 4 && rows[3].seat == 1 && rows[3].is_you == 0,
          "the fool - the one seat still holding cards - takes the LAST place");

    // The last place is the seat count, not the row count. A game with rows
    // missing must not promote the fool.
    n = anim_finish_rows(order, 2, 1, 4, -1, rows, MAX_PLAYERS);
    CHECK(n == 3 && rows[2].place == 4,
          "the fool's place is the seat count even when the list is short");

    // A spectator holds no seat, and the kernel spends -1 on "no seat" too.
    n = anim_finish_rows(order, 3, 1, 4, -1, rows, MAX_PLAYERS);
    CHECK(n == 4 && rows[0].is_you == 0 && rows[1].is_you == 0
          && rows[2].is_you == 0 && rows[3].is_you == 0,
          "no row belongs to a spectator");

    // A game still running has no fool yet.
    n = anim_finish_rows(order, 1, -1, 4, 0, rows, MAX_PLAYERS);
    CHECK(n == 1 && rows[0].place == 1, "a game with nobody out yet ranks only who is out");
    CHECK(anim_finish_rows(NULL, 0, -1, 4, 0, rows, MAX_PLAYERS) == 0, "…and an untouched game, nobody");
}

static void test_board_degenerate_inputs(void) {
    unsigned char out[8];
    AnimFinishRow rows[MAX_PLAYERS];
    const unsigned char ids[2] = { 0, 1 };

    CHECK(anim_fan_cards(ids, 2, NULL, 0, NULL, 8) == ANIM_EBADARG, "no out, no fan");
    CHECK(anim_fan_cards(NULL, 2, NULL, 0, out, 8) == ANIM_EBADARG, "a count with no hand");
    CHECK(anim_fan_cards(ids, 2, NULL, 1, out, 8) == ANIM_EBADARG, "a count with no holdback");
    CHECK(anim_fan_cards(ids, -1, NULL, 0, out, 8) == ANIM_EBADARG, "a negative hand is not a hand");
    CHECK(anim_fan_cards(ids, 2, NULL, 0, out, 1) == ANIM_ECAP, "an output too small for the hand");
    CHECK(anim_fan_cards(ids, 2, ids, 2, out, 8) == 2, "a holdback wholly in the hand adds nothing");

    CHECK(anim_laid_count(NULL, 2, NULL, 0, 0) == ANIM_EBADARG, "a count with no hand counts nothing");
    CHECK(anim_hand_laid_out(ids, 2, 0, NULL, 0, NULL, 8) == ANIM_EBADARG, "no out, no layout");
    CHECK(anim_hand_laid_out(NULL, 2, 0, NULL, 0, out, 8) == ANIM_EBADARG, "a count with no cards");
    CHECK(anim_hand_laid_out(ids, 2, 0, NULL, 1, out, 8) == ANIM_EBADARG, "a count with no arrangement");
    CHECK(anim_hand_laid_out(ids, 2, 0, NULL, 0, out, 1) == ANIM_ECAP, "an output too small to lay out");

    CHECK(anim_table_covers(NULL, 1, NULL, 0) == ANIM_EBADARG, "a count with no table");
    CHECK(anim_table_covers(NULL, 0, NULL, 1) == ANIM_EBADARG, "…on either side");
    CHECK(anim_table_covers(NULL, -1, NULL, 0) == ANIM_EBADARG, "a negative table is not a table");

    CHECK(anim_finish_rows(NULL, 0, 0, 4, 0, NULL, 4) == ANIM_EBADARG, "no out, no rows");
    CHECK(anim_finish_rows(NULL, 1, 0, 4, 0, rows, 4) == ANIM_EBADARG, "a count with no elimination list");
    CHECK(anim_finish_rows(ids, 2, 0, 4, 0, rows, 2) == ANIM_ECAP,
          "a roster that will not fit is refused rather than half-ranked");
}

// EVERY BOUND CHECKED AGAINST A GUARD PAGE. These readers take a pointer and a
// count with no record to parse, so the count IS the bound; placed flush against
// a PROT_NONE page, one byte past it kills the process instead of walking into
// whatever follows in a static array (see jt_flush_to_guard).
static void test_board_reads_nothing_past_what_it_was_given(void) {
    unsigned char out[64];
    const unsigned char hand[3] = { BID(0, 6), BID(1, 9), BID(3, 13) };
    const unsigned char table[4] = { BID(0, 6), BID(1, 9), BID(3, 13), ANIM_TABLE_NONE };

    const unsigned char *h = jt_flush_to_guard(hand, 3);
    if (!h) { CHECK(0, "the guard page maps"); return; }
    CHECK(anim_fan_cards(h, 3, NULL, 0, out, 64) == 3, "the fan reads exactly the hand it was given");
    CHECK(anim_laid_count(h, 3, NULL, 0, 0) == 3, "and so does the count");
    CHECK(anim_hand_laid_out(h, 3, 0, NULL, 0, out, 64) == 3, "and the layout");

    const unsigned char *o = jt_flush_to_guard(hand, 3);
    CHECK(anim_hand_laid_out(hand, 3, 0, o, 3, out, 64) == 3,
          "the arrangement is read to its length and no further");

    const unsigned char *held = jt_flush_to_guard(hand, 2);
    CHECK(anim_fan_cards(hand, 3, held, 2, out, 64) == 3, "and so is the holdback");

    // A table is TWO bytes per battle, which is the bound that is easy to get
    // wrong by one battle.
    const unsigned char *t = jt_flush_to_guard(table, 4);
    CHECK(anim_table_card_ids(t, 2) == (BSET(BID(0, 6)) | BSET(BID(1, 9)) | BSET(BID(3, 13))),
          "a table is read two bytes per battle and not one byte further");
    CHECK(anim_table_covers(t, 2, t, 2) == 1, "…on both sides of the subset test");
    CHECK(anim_covered_sweep_accepts(1, t, 2, t, 2) == 1, "…and through the sweep's decision");

    const unsigned char elim[3] = { 2, 0, 3 };
    const unsigned char *e = jt_flush_to_guard(elim, 3);
    AnimFinishRow rows[MAX_PLAYERS];
    CHECK(anim_finish_rows(e, 3, 1, 4, 0, rows, MAX_PLAYERS) == 4,
          "the elimination list is read to its length");
}

// THE ID CROSSING, ALL 52 CARDS, BOTH DIRECTIONS. A dense-id off-by-one is
// invisible through most of the deck because both sides of every comparison go
// through the same encoder; it bites only at the last card, where a set comes
// back empty and everything silently passes. So walk the whole deck through
// every entry that takes an id, and prove the ace of the last suit behaves like
// the six of the first.
// The exact endgame solver's transposition table is keyed on the POSITION, but
// a solved value only means anything with a seat attached: +990 is "the seat I
// asked about escapes". Nothing resets the table between two asks, so the two
// seats of one endgame share every entry. If the stored value is not brought to
// a common perspective, the second ask reads the first ask's proof unflipped and
// reports the loser as the winner. solver_difftest cannot see this: it resets
// between perspectives. So drive real endgames and ask both seats back to back.
// MUTATION (all three checked): drop BOTH seat flips in sim_solve_rec
// (cordite_sim.c), which is the bug as it shipped -> the warm-vs-cold check
// fails and the zero-sum one still passes, because each cold solve is
// self-consistent. Dropping only the probe flip, or only the store flip, fails
// both: a single-seat search then corrupts its own entries.
static int tt_seat_step(Game *g) {
    for (int i = 0; i < g->num_players; i++) {
        if (!should_bot_act(g, i)) continue;
        LegalMoves mv; calculate_legal_moves(g, i, &mv);
        if (mv.n == 0) continue;
        int idx = handwritten_strategy_choose(g, i, &mv, NULL);
        if (idx < 0 || idx >= mv.n) idx = 0;
        const LegalMove *m = &mv.moves[idx];
        bool ok = false;
        switch (m->type) {
            case MOVE_ATTACK: ok = handle_attack(g, i, m->cards, m->n_cards); break;
            case MOVE_COVER:  ok = handle_cover (g, i, m->cards, m->attack_cards, m->n_cards); break;
            case MOVE_PASS:   ok = handle_pass  (g, i, m->cards, m->n_cards); break;
            case MOVE_PICKUP: ok = handle_pickup(g, i); break;
            case MOVE_GOOD:   ok = handle_good  (g, i); break;
            default: break;
        }
        if (ok) return 1;
    }
    return 0;
}

static void test_solver_tt_value_carries_its_seat(void) {
    int compared = 0, zero_sum = 0, dirty_matches_clean = 0;
    for (int gi = 0; gi < 12 && compared < 40; gi++) {
        game_set_seed((uint32_t)(gi + 1));
        Game g; memset(&g, 0, sizeof g);
        g.num_players = 2;
        for (int i = 0; i < 2; i++) {
            g.players[i].status = PLAYER_STATUS_READY;
            g.players[i].strategy_key = (int8_t)STRAT_HANDWRITTEN;
            snprintf(g.players[i].player_id, sizeof g.players[i].player_id, "p%d", i);
        }
        start_game(&g);
        int it = 0;
        while (game_done(&g) < 0 && it++ < 3000 && compared < 40) {
            int in[MAX_PLAYERS], nin = 0;
            for (int i = 0; i < g.num_players; i++)
                if (g.players[i].status == PLAYER_STATUS_IN) in[nin++] = i;
            if (nin == 2 && g.deck_count == 0 && !g.has_flipped) {
                SimState sa, sb, sc;
                cd_sim_from_game(&sa, &g); sb = sa; sc = sa;
                int aa = 0, ab = 0, ac = 0;
                cd_sim_solve_reset();
                int va       = cd_sim_solve(&sa, in[0], -2000, 2000, 2000000L, &aa);
                int vb_dirty = cd_sim_solve(&sb, in[1], -2000, 2000, 2000000L, &ab);
                cd_sim_solve_reset();
                int vb_clean = cd_sim_solve(&sc, in[1], -2000, 2000, 2000000L, &ac);
                if (!aa && !ab && !ac) {
                    compared++;
                    if (vb_clean == -va)      zero_sum++;
                    if (vb_dirty == vb_clean) dirty_matches_clean++;
                }
            }
            if (!tt_seat_step(&g)) break;
        }
    }
    CHECK(compared >= 20, "solver TT seat check found endgames to compare");
    CHECK(zero_sum == compared, "a resolved endgame is zero-sum across the two seats");
    CHECK(dirty_matches_clean == compared,
          "the second seat's value is the same with a warm table as with a cold one");
}

static void test_board_every_card_of_the_deck_crosses_both_ways(void) {
    unsigned char out[64];
    int veil_ok = 1, fan_ok = 1, table_ok = 1, laid_ok = 1;

    for (int suit = 0; suit < 4; suit++) {
        for (int value = 1; value <= 13; value++) {
            const unsigned char id = BID(suit, value);
            const uint64_t bit = BSET(id);
            if (id != (unsigned char)(suit * 13 + value - 1)) veil_ok = 0;

            // Through each of the veil's four outs.
            if (anim_veil_veiled(bit, 0, 0, 0, 0, 0) != bit) veil_ok = 0;
            if (anim_veil_veiled(0, bit, 0, 0, 0, 0) != bit) veil_ok = 0;
            if (anim_veil_veiled(0, 0, 1, 0, 1, bit) != bit) veil_ok = 0;
            if (anim_veil_flying(bit, 0) != bit) veil_ok = 0;
            if (anim_veil_flying(bit, bit) != 0) veil_ok = 0;
            if (anim_veil_hand_slot_deferred(bit, 0, bit) != 0) veil_ok = 0;
            if (anim_veil_fan(bit, bit) != 0) veil_ok = 0;

            // Through the fan: a held card the hand already holds is not doubled.
            if (anim_fan_cards(&id, 1, &id, 1, out, 64) != 1 || out[0] != id) fan_ok = 0;
            if (anim_laid_count(&id, 1, NULL, 0, bit) != 0) fan_ok = 0;
            if (anim_laid_count(&id, 1, NULL, 0, 0) != 1) fan_ok = 0;

            // Through the layout, ordered by an arrangement that names it.
            if (anim_hand_laid_out(&id, 1, 0, &id, 1, out, 64) != 1 || out[0] != id) laid_ok = 0;
            if (anim_hand_laid_out(&id, 1, bit, &id, 1, out, 64) != 0) laid_ok = 0;

            // Through a table, on both sides of a battle.
            const unsigned char attack[2] = { id, ANIM_TABLE_NONE };
            const unsigned char cover[2] = { BID(0, 6), id };
            if (anim_table_card_ids(attack, 1) != bit) table_ok = 0;
            if (id != BID(0, 6) && anim_table_card_ids(cover, 1) != (bit | BSET(BID(0, 6))))
                table_ok = 0;
        }
    }
    CHECK(veil_ok, "all 52 cards veil, fly and defer the same way");
    CHECK(fan_ok, "all 52 cards fan and count the same way");
    CHECK(laid_ok, "all 52 cards lay out the same way");
    CHECK(table_ok, "all 52 cards stand on a table the same way, attack side and cover side");

    // The sentinels. ANIM_TABLE_NONE is not a card, and neither is anything
    // else off the deck - folding one in would name the wrong card, which is
    // exactly the failure this crossing hides.
    const unsigned char none[2] = { ANIM_TABLE_NONE, ANIM_TABLE_NONE };
    CHECK(anim_table_card_ids(none, 1) == 0, "an empty cell names no card");
    const unsigned char off[2] = { 52, 200 };
    CHECK(anim_table_card_ids(off, 1) == 0, "and neither does a byte off the deck");
    CHECK(anim_fan_cards(NULL, 0, off, 2, out, 64) == 0,
          "a holdback of cards that do not exist adds nothing to the fan");
    CHECK(anim_hand_laid_out(off, 2, 0, NULL, 0, out, 64) == 0, "…and lays out nothing");
}

int main(void) {
    test_reset_to_lobby();
    test_replay_steps_rebuilds_the_played_game();
    test_replay_steps_mid_game_cut_conserves_the_deck();
    test_replay_v6_carries_a_pending_good();
    test_replay_frames_are_the_replay_events();
    test_replay_step_index_says_what_each_step_is();
    test_replay_step_index_tells_a_pass_from_an_attack();
    test_replay_step_index_reports_a_pending_good();
    test_replay_steps_replays_a_deal_with_no_trump();
    test_replay_step_index_refuses_a_small_buffer();
    test_json_view_from_packed_says_what_the_live_board_says();
    test_json_view_from_packed_leaks_no_hand_to_a_spectator();
    test_json_events_from_packed_reads_the_frames_the_kernel_wrote();
    test_json_events_trailer_is_the_board_the_engine_ended_on();
    test_json_events_refuses_a_truncated_or_foreign_payload();
    test_evwire_read_round_trips_the_writers_own_events();
    test_evwire_read_refuses_a_malformed_sequence();
    test_evwire_frames_settlement_cut();
    test_replay_steps_refuses_v5();
    test_replay_v6_refuses_an_overflowed_log();
    test_bot_drive_preferred();
    test_bot_pacing_table();
    test_bot_roster_strat_unique();
    test_bot_drive_basic();
    test_bot_drive_bundles_only_silent();
    test_bot_drive_fairness();
    test_bot_drive_deterministic();
    test_bot_drive_choose_emits_no_snapshots();
    test_bot_drive_pre_action_hook();

    test_bot_roster_table();
    test_bot_knobs_precedence();
    test_bot_roster_choose_scopes_knobs();

    test_deal_rng_kat();
    test_whole_game_reproducible();
    test_deal_wide_reproducible();
    test_deal_wide_permutation();
    test_deal_rng_unbiased();
    test_start_game();
    test_awire_apply_roundtrip();
    test_awire_apply_settles_game_over();
    test_game_human_mask();
    test_game_seat_and_deal();
    test_legal_first_attack();
    test_legal_first_attack_duplicate();
    test_can_cover();
    test_unambiguous_cover_one_card_one_attack();
    test_unambiguous_cover_rejects_when_it_cannot_cover();
    test_unambiguous_cover_refuses_a_genuine_ambiguity();
    test_unambiguous_cover_pairs_by_suit();
    test_unambiguous_cover_same_set_different_pairing_is_ok();
    test_unambiguous_cover_more_cards_than_attacks();
    test_unambiguous_cover_trump_over_plain();
    test_unambiguous_cover_degenerate_inputs();
    test_menu_wire_round_trips();
    test_menu_wire_writes_a_chunk();
    test_play_resolve_attacker();
    test_play_resolve_refuses_the_hand_for_both_roles();
    test_play_resolve_defender_onto_a_battle();
    test_play_resolve_battle_target_names_its_attack();
    test_play_resolve_open_table_prefers_a_pass();
    test_play_resolve_open_table_auto_covers_only_when_unambiguous();
    test_play_coverable_battles();
    test_best_cover_takes_the_highest_it_can_beat();
    test_best_cover_trump_outranks_a_bigger_plain_card();
    test_best_cover_only_considers_what_the_menu_offers();
    test_best_cover_breaks_ties_the_same_way_every_time();
    test_best_cover_ignores_table_order();
    test_best_cover_target_resolves_to_a_legal_cover();
    test_best_cover_multi_card_selection();
    test_play_has_verb();
    test_play_can_say_good_only_over_a_covered_table();
    test_play_human_menu_drops_wait_and_gates_good();
    test_good_is_always_enumerated_for_an_attacker();
    test_full_game_random();
    test_full_game_handwritten();
    test_full_game_3p_handwritten();

    // Reject-matrix & edge-path coverage.
    test_deal_order();
    test_attack_rejects();
    test_cover_rejects_and_success();
    test_cover_clears_hand_round_advance();
    test_cover_clears_hand_wins();
    test_pass_rejects_and_success();
    test_podkidnoy();
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

    // The shape of a sequence (anim_plan.h beats + the role beat).
    test_beats_merge_only_same_seat_consecutive_covers();
    test_beats_hold_skips_notices_and_stops_at_card_motion();
    test_beats_only_a_beat_that_moved_adopts_the_outs_behind_it();
    test_beats_placed_and_the_badge_direction();
    test_role_beat_leads_runs_parallel_and_follows();
    test_role_beat_reads_the_beats_own_attack_pass_seats();
    test_beats_refuses_a_stream_it_cannot_hold();
    test_the_freeze_is_the_board_before_every_move();

    // The pre-bout table (anim_plan.h).
    test_pre_bout_table_keeps_the_real_pairing();
    test_pre_bout_table_refuses_a_board_from_another_moment();
    test_pre_bout_table_walks_back_through_the_stream();
    test_pre_bout_table_degenerate_inputs();
    test_the_pre_bout_table_is_the_board_the_kernel_had();

    // The conflict model (anim_plan.h).
    test_conflict_a_chain_that_disowns_a_card_flies_it_home();
    test_conflict_clear_wins_over_the_standing_check();
    test_conflict_the_standing_check_reads_the_side_it_landed_on();
    test_conflict_backs_and_pools_are_never_conjured();
    test_conflict_dest_is_the_flight_builders_own_mapping();
    test_conflict_reversal_flies_back_the_way_it_came();
    test_conflict_reversal_drops_what_the_verdicts_empty();
    test_conflict_degenerate_inputs();
    test_the_transport_is_the_only_thing_the_two_clients_disagree_about();
    test_board_veil_unions_its_three_sources();
    test_board_veil_flying_is_hidden_minus_pre_hidden();
    test_board_veil_the_fan_never_withholds_a_slot_it_draws();
    test_board_veil_grid_answers_off_different_state_when_sweeping();
    test_board_veil_teardown_and_handover();
    test_board_veil_small_decisions();
    test_board_selection_may_only_name_cards_in_my_hand();
    test_board_placement_is_a_hand_reaching_the_table();
    test_board_the_fan_lays_out_the_hand_it_is_given();
    test_board_hand_laid_out_follows_the_local_arrangement();
    test_board_the_table_under_the_sweep();
    test_board_finish_rows_rank_first_out_to_the_fool();
    test_board_degenerate_inputs();
    test_board_reads_nothing_past_what_it_was_given();
    test_board_every_card_of_the_deck_crosses_both_ways();
    test_solver_tt_value_carries_its_seat();

    printf("\n%d passed, %d failed\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}

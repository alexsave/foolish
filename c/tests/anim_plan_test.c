// anim_plan_test — the native proof of the animation core (c/src/anim_plan.h).
//
// THE OWNER'S HEADLINE ASK: the animation-quality tests that were hardened in
// React now live in C. Each assertion here is the C twin of a TypeScript one:
//
//   e2e/optimistic_animation.test.ts  -> test_optimistic_animation()
//     staleOptimisticKeysOnTable: the version gate must NOT release a card the
//     same broadcast confirms (the "card animates twice" bug), MUST release a
//     lingering on-table card whose confirming broadcast was dropped, and leaves
//     a not-yet-on-table card alone. Plus the dedup-signature identity the second
//     half of that test asserts (createCardEventString parity -> anim_event_key).
//
//   e2e/optimistic_revert.test.ts     -> test_optimistic_revert()
//     resolveUnconfirmedAttackCovers: SCENARIO B (a card the defender picks up is
//     CLEARed, never reverted to my hand) and SCENARIO A (a still-legal in-flight
//     attack is MERGEd, never reverted by a concurrent broadcast) — the two
//     player-reported flickers, one root cause. Plus the capacity-revert positive
//     and the cover-exclusion the pure module documents.
//
//   e2e/reconcile.test.ts             -> test_reconcile()
//     shouldDropStaleSequence + mergeTableBattles: the client converges to the
//     newest authoritative table under ANY delivery order (the broadcast-
//     reordering fix). The e2e version drives real games; the invariant it proves
//     is pure, so we prove it directly over every permutation of a broadcast set.
//
// Plus test_plan_building() — the count-freeze, the per-step timing
// (ANIMATION_TIME), and the veil, which no TS test covered because that
// choreography lived inside AnimationContext's setState machinery.
//
// Usage: anim_plan_test              (no args)
// Modelled on tests/msg_wire_test.c's CHECK harness.

#include "../src/anim_plan.h"
#include "../src/msg_wire.h"
#include "../src/card.h"
#include <stdio.h>
#include <string.h>

static int g_fails = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fails++; } \
} while (0)

static Card C(int suit, int value) { Card c; c.suit = (int8_t)suit; c.value = (int8_t)value; return c; }
static int same(Card a, Card b) { return a.suit == b.suit && a.value == b.value; }

// ======================================================================
// 1. optimistic_animation.test.ts  (staleOptimisticKeysOnTable + dedup key)
// ======================================================================
static void test_optimistic_animation(void) {
    // The card the local player optimistically attacked with.
    const Card card = C(1, 9);
    const int SELF = 0;   // stands in for player_id 'player-self' (per-viewer seat)

    // --- assertion 1: version gate does NOT release a card the same broadcast
    //     confirms (no double-play). opt=[card], table=[card], named=[card]. ---
    {
        Card opt[]   = { card };
        Card table[] = { card };
        Card named[] = { card };   // the broadcast's own attack_pass names `card`
        int rel[8];
        int n = anim_stale_optimistic_on_table(opt, 1, table, 1, named, 1, rel, 8);
        CHECK(n == 0, "release must be empty when the broadcast names the card (got %d)", n);
    }

    // --- the dedup half: the optimistic key and the server's confirming-event
    //     key must be IDENTICAL, so the per-event dedup recognises the card
    //     instead of animating it a second time. ---
    {
        uint64_t opt_key = anim_event_key(ANIM_EVT_ATTACK_PASS, card, ANIM_LOC_HAND, ANIM_LOC_TABLE, SELF);
        uint64_t srv_key = anim_event_key(ANIM_EVT_ATTACK_PASS, card, ANIM_LOC_HAND, ANIM_LOC_TABLE, SELF);
        CHECK(opt_key == srv_key, "optimistic key must equal the confirming-event key");
        // A different card / type / direction must NOT collide.
        CHECK(opt_key != anim_event_key(ANIM_EVT_ATTACK_PASS, C(1, 10), ANIM_LOC_HAND, ANIM_LOC_TABLE, SELF),
              "a different card must key differently");
        CHECK(opt_key != anim_event_key(ANIM_EVT_COVER, card, ANIM_LOC_HAND, ANIM_LOC_TABLE, SELF),
              "a different type must key differently");
    }

    // --- the key crosses to the browser as a DOUBLE (wasm_anim_event_key), so
    //     it is a plain Map key there and no host packs the fields itself. That
    //     only works while every key the kernel can make is exact in a double:
    //     six bytes, bits 0..47, well inside a double's 53. Walk the whole range
    //     of every field - every event type, every location including the NONE
    //     sentinel, every seat a byte can name, every suit and value - and hold
    //     each key both to the 2^48 ceiling and to the round trip through a
    //     double, which is what the browser actually receives. ---
    {
        const int locs[] = { ANIM_LOC_DECK, ANIM_LOC_HAND, ANIM_LOC_TABLE, ANIM_LOC_DISCARD,
                             ANIM_LOC_FLIPPED, ANIM_LOC_NONE };
        int checked = 0, bad_ceiling = 0, bad_round_trip = 0;
        for (int type = 0; type <= ANIM_EVT_REVERT; type++)
            for (int fi = 0; fi < (int)(sizeof locs / sizeof locs[0]); fi++)
                for (int ti = 0; ti < (int)(sizeof locs / sizeof locs[0]); ti++)
                    for (int seat = -1; seat < 8; seat++)
                        for (int suit = -1; suit < 4; suit++)
                            for (int value = -1; value <= 13; value++) {
                                const uint64_t k = anim_event_key(type, C(suit, value), locs[fi], locs[ti], seat);
                                if (k >= (1ull << 48)) bad_ceiling++;
                                if ((uint64_t)(double)k != k) bad_round_trip++;
                                checked++;
                            }
        CHECK(checked > 0, "the key walk covered something");
        CHECK(bad_ceiling == 0, "every key packs into 48 bits (%d of %d did not)", bad_ceiling, checked);
        CHECK(bad_round_trip == 0, "every key survives a double exactly (%d of %d did not)", bad_round_trip, checked);
    }

    // --- assertion 2: version gate DOES release an on-table optimistic card whose
    //     confirming broadcast was dropped (this broadcast names an UNRELATED
    //     card). opt=[card], table=[card], named=[other]. ---
    {
        Card opt[]   = { card };
        Card table[] = { card };
        Card named[] = { C(2, 10) };   // a cover of an unrelated card
        int rel[8];
        int n = anim_stale_optimistic_on_table(opt, 1, table, 1, named, 1, rel, 8);
        CHECK(n == 1 && rel[0] == 0, "must release the lingering entry (dropped-broadcast safety net) (got %d)", n);
    }

    // --- assertion 3: leaves optimistic cards not yet on the authoritative table.
    //     opt=[card], table=[], named=[]. ---
    {
        Card opt[] = { card };
        int rel[8];
        int n = anim_stale_optimistic_on_table(opt, 1, NULL, 0, NULL, 0, rel, 8);
        CHECK(n == 0, "nothing on table yet -> keep the optimistic entry (got %d)", n);
    }
}

// ======================================================================
// 2. optimistic_revert.test.ts  (resolveUnconfirmedAttackCovers)
// ======================================================================
static void test_optimistic_revert(void) {
    // THE SERVER TRANSPORT is what this whole rule is: a broadcast is a partial
    // delta and a card's own confirmation may still be in the post. The kernel
    // refuses to guess that (ANIM_ETRANSPORT), so the host says it once.
    CHECK(anim_set_transport(ANIM_TRANSPORT_SERVER) == ANIM_EOK, "the host names its transport");
    // --- SCENARIO B: a card the defender immediately picks up is NOT reverted to
    //     my hand — it was accepted, then swept off by the pickup, so it CLEARs. ---
    {
        const Card myCard = C(0, 7);
        AnimPending pending[] = { { myCard, 0 } };
        // The pickup broadcast: the table is cleared (server_table empty) and the
        // pickup event names myCard among the swept cards.
        Card pickupCards[] = { myCard };
        AnimEvent events[1];
        memset(events, 0, sizeof(events));
        events[0].type = ANIM_EVT_PICKUP;
        events[0].cards = pickupCards;
        events[0].n_cards = 1;
        AnimFinalState fin; memset(&fin, 0, sizeof(fin));
        fin.defender = 1; fin.n_players = 2; fin.hand_length[1] = 7; fin.final_uncovered_attacks = 0;
        AnimResolve r;
        int rc = anim_resolve_unconfirmed_attack_covers(pending, 1, NULL, 0, events, 1, &fin, &r);
        CHECK(rc == ANIM_EOK, "resolve rc");
        CHECK(r.n_revert == 0, "SCENARIO B: my card must NOT be reverted (got %d reverts)", r.n_revert);
        CHECK(r.n_clear == 1 && r.clear[0] == 0, "SCENARIO B: my card must be CLEARed (swept, no revert)");
    }

    // --- SCENARIO A: a still-legal in-flight attack is NOT reverted by a
    //     concurrent attack broadcast that does not yet name it. The defender can
    //     still hold it (capacity ok), so it MERGEs. ---
    {
        const Card heroCard = C(3, 11);
        AnimPending pending[] = { { heroCard, 0 } };
        // Rival's broadcast: an attack_pass that does NOT name heroCard; the
        // server table shows only the rival's card.
        Card rivalCard[] = { C(3, 6) };
        Card serverTable[] = { C(3, 6) };
        AnimEvent events[1];
        memset(events, 0, sizeof(events));
        events[0].type = ANIM_EVT_ATTACK_PASS;
        events[0].cards = rivalCard;
        events[0].n_cards = 1;
        AnimFinalState fin; memset(&fin, 0, sizeof(fin));
        fin.defender = 0; fin.n_players = 3; fin.hand_length[0] = 6; fin.final_uncovered_attacks = 1;
        AnimResolve r;
        int rc = anim_resolve_unconfirmed_attack_covers(pending, 1, serverTable, 1, events, 1, &fin, &r);
        CHECK(rc == ANIM_EOK, "resolve rc");
        CHECK(r.n_revert == 0, "SCENARIO A: hero's still-valid card must NOT be reverted (got %d)", r.n_revert);
        CHECK(r.n_merge == 1 && same(pending[r.merge[0]].card, heroCard), "SCENARIO A: hero's card must MERGE");
    }

    // --- capacity REVERT (positive): two pending attacks the defender cannot
    //     hold -> both revert. finalUncovered=5, defenderHand=5, 5+2>5. ---
    {
        AnimPending pending[] = { { C(0, 5), 0 }, { C(1, 5), 0 } };
        AnimEvent events[1]; memset(events, 0, sizeof(events));
        events[0].type = ANIM_EVT_ATTACK_PASS;   // no table-clear
        AnimFinalState fin; memset(&fin, 0, sizeof(fin));
        fin.defender = 1; fin.n_players = 2; fin.hand_length[1] = 5; fin.final_uncovered_attacks = 5;
        AnimResolve r;
        int rc = anim_resolve_unconfirmed_attack_covers(pending, 2, NULL, 0, events, 1, &fin, &r);
        CHECK(rc == ANIM_EOK, "resolve rc");
        CHECK(r.n_revert == 2 && r.n_merge == 0, "capacity: both attacks revert (got revert=%d merge=%d)", r.n_revert, r.n_merge);
    }

    // --- cover EXCLUSION: a pending attack + a pending cover, capacity exceeded ON
    //     THE ATTACK. The cover is the defender's own play (no capacity rule) so it
    //     MERGEs while the attack reverts. finalUncovered=5, defenderHand=5. ---
    {
        AnimPending pending[] = { { C(0, 5), 0 /*attack*/ }, { C(2, 9), 1 /*cover*/ } };
        AnimEvent events[1]; memset(events, 0, sizeof(events));
        events[0].type = ANIM_EVT_ATTACK_PASS;
        AnimFinalState fin; memset(&fin, 0, sizeof(fin));
        fin.defender = 1; fin.n_players = 2; fin.hand_length[1] = 5; fin.final_uncovered_attacks = 5;
        AnimResolve r;
        int rc = anim_resolve_unconfirmed_attack_covers(pending, 2, NULL, 0, events, 1, &fin, &r);
        CHECK(rc == ANIM_EOK, "resolve rc");
        CHECK(r.n_revert == 1 && same(pending[r.revert[0]].card, C(0, 5)), "cover-excl: the ATTACK reverts");
        CHECK(r.n_merge == 1 && same(pending[r.merge[0]].card, C(2, 9)), "cover-excl: the COVER merges");
    }

    // --- MIXED, and a deliberate change of answer: the broadcast shows ONE of
    //     my two pending cards. The one it shows is STANDING and needs no
    //     merging into a state that already has it; the other still has to
    //     answer for itself, and the defender can hold it, so it merges. The
    //     rule this replaced short-circuited the WHOLE set on the first
    //     accepted card - a shape AnimationContext cannot produce (it only asks
    //     when none is accepted), and per-card is the question being asked. ---
    {
        AnimPending pending[] = { { C(1, 8), 0 }, { C(2, 4), 0 } };
        Card serverTable[] = { C(1, 8) };
        AnimEvent events[1]; memset(events, 0, sizeof(events));
        events[0].type = ANIM_EVT_ATTACK_PASS;
        AnimFinalState fin; memset(&fin, 0, sizeof(fin));
        fin.defender = 1; fin.n_players = 2; fin.hand_length[1] = 6;
        fin.final_uncovered_attacks = 1;
        AnimResolve r;
        int rc = anim_resolve_unconfirmed_attack_covers(pending, 2, serverTable, 1, events, 1, &fin, &r);
        CHECK(rc == ANIM_EOK, "resolve rc");
        CHECK(r.n_revert == 0 && r.n_clear == 0, "nothing is doomed here");
        CHECK(r.n_merge == 1 && same(pending[r.merge[0]].card, C(2, 4)),
              "only the card the broadcast does NOT show needs merging");
    }

    // --- accepted short-circuit: server already shows my card -> no revert/merge/clear. ---
    {
        AnimPending pending[] = { { C(1, 8), 0 } };
        Card serverTable[] = { C(1, 8) };
        AnimEvent events[1]; memset(events, 0, sizeof(events));
        events[0].type = ANIM_EVT_ATTACK_PASS;
        AnimFinalState fin; memset(&fin, 0, sizeof(fin));
        fin.defender = 1; fin.n_players = 2; fin.hand_length[1] = 6;
        AnimResolve r;
        int rc = anim_resolve_unconfirmed_attack_covers(pending, 1, serverTable, 1, events, 1, &fin, &r);
        CHECK(rc == ANIM_EOK && r.n_revert == 0 && r.n_merge == 0 && r.n_clear == 0,
              "accepted card -> empty verdict (dedup handles it)");
    }
}

// ======================================================================
// 3. reconcile.test.ts  (shouldDropStaleSequence + mergeTableBattles)
// ======================================================================

// The version gate as unit assertions.
static void test_reconcile_gate(void) {
    CHECK(anim_should_drop_stale(1, 5, 1, 5) == 1, "equal version is stale (<=)");
    CHECK(anim_should_drop_stale(1, 5, 1, 4) == 1, "lower version is stale");
    CHECK(anim_should_drop_stale(1, 5, 1, 6) == 0, "higher version applies");
    CHECK(anim_should_drop_stale(0, 0, 1, 3) == 0, "no prior version -> apply");
    CHECK(anim_should_drop_stale(1, 5, 0, 0) == 0, "replay (no version) -> never gated");
}

// The convergence invariant, proven over EVERY permutation of a broadcast set:
// under the gate (drop stale) + trust-incoming merge, the applied table always
// ends at the NEWEST (highest-version) broadcast's table, whatever the order.
// mergeTableBattles is "return incoming" so the applied table token is just the
// last-applied broadcast's token; the gate guarantees that is the max version.
typedef struct { int version; int table_token; } Bcast;

static int apply_reordered(const Bcast *order, int n) {
    int table = -1;          // the client table token; -1 = empty
    int has_last = 0, last = 0;
    for (int i = 0; i < n; i++) {
        if (anim_should_drop_stale(has_last, last, 1, order[i].version)) continue;  // REAL gate
        table = order[i].table_token;   // REAL merge: trust incoming
        has_last = 1; last = order[i].version;
    }
    return table;
}

// Enumerate permutations of [0,n) via Heap's algorithm and check each.
static void perm_check(Bcast *bs, int n, int expect_token) {
    // small n only (n<=6); recursion depth is trivial.
    int idx[8]; for (int i = 0; i < n; i++) idx[i] = i;
    int c[8]; for (int i = 0; i < n; i++) c[i] = 0;
    Bcast order[8];
    for (int i = 0; i < n; i++) order[i] = bs[idx[i]];
    CHECK(apply_reordered(order, n) == expect_token, "converge (identity order)");
    int i = 0;
    while (i < n) {
        if (c[i] < i) {
            int a = (i % 2 == 0) ? 0 : c[i];
            int t = idx[a]; idx[a] = idx[i]; idx[i] = t;
            for (int k = 0; k < n; k++) order[k] = bs[idx[k]];
            if (apply_reordered(order, n) != expect_token) {
                CHECK(0, "diverged under a reordering");
                return;
            }
            c[i]++; i = 0;
        } else { c[i] = 0; i++; }
    }
}

static void test_reconcile(void) {
    test_reconcile_gate();

    // Five broadcasts, versions strictly increasing with distinct table tokens;
    // the highest version (5) carries token 500. Every arrival order must end at
    // 500. (Versions need not be contiguous — the gate only compares magnitude.)
    Bcast bs[] = {
        { 1, 100 }, { 2, 200 }, { 3, 300 }, { 4, 400 }, { 5, 500 },
    };
    perm_check(bs, 5, 500);

    // A gappy set with a duplicate version (a re-delivered broadcast): still
    // converges to the max version's token, and a duplicate never supersedes.
    Bcast bs2[] = {
        { 10, 111 }, { 12, 222 }, { 12, 999 /*dup version, must be dropped once 12 applied*/ }, { 15, 333 },
    };
    // The two v12 entries: whichever lands first wins its token; the second is
    // dropped by <=; v15 is the max and always the final table. So expect 333.
    perm_check(bs2, 4, 333);
}

// ======================================================================
// 4. plan building  (count-freeze / veil / timing)
// ======================================================================
static void test_plan_building(void) {
    // A 2-player bout-end sequence, viewer = seat 0 (the attacker who drew):
    //   step0 DISCARD  table->discard, 4 cards
    //   step1 REFILL   deck->seat0 hand, 2 REAL cards (viewer's own draws)
    //   step2 REFILL   deck->seat1 hand, 2 MASKED backs (opponent's draws)
    // Final board: deck 20, discard 8, hands [6, 6].
    //
    // NO EVENT CARRIES A BOARD here, which is the FALLBACK path: with nothing to
    // anchor on the freeze is the n-event walk back from the final board. The
    // anchored path - the one every real evwire stream takes - is
    // test_plan_anchors_on_the_first_events_own_board below.
    Card refill0[] = { C(0, 5), C(2, 7) };   // ids 4 and 32
    Card refill1[] = { C(-1, -1), C(-1, -1) };  // masked backs

    AnimPlanEvent ev[3];
    memset(ev, 0, sizeof(ev));
    ev[0].type = ANIM_EVT_DISCARD;   ev[0].seat = ANIM_SEAT_NONE; ev[0].from = ANIM_LOC_TABLE; ev[0].to = ANIM_LOC_DISCARD; ev[0].n_cards = 4;
    ev[1].type = ANIM_EVT_REFILL;    ev[1].seat = 0; ev[1].from = ANIM_LOC_DECK; ev[1].to = ANIM_LOC_HAND; ev[1].cards = refill0; ev[1].n_cards = 2;
    ev[2].type = ANIM_EVT_REFILL;    ev[2].seat = 1; ev[2].from = ANIM_LOC_DECK; ev[2].to = ANIM_LOC_HAND; ev[2].cards = refill1; ev[2].n_cards = 2; ev[2].mask_cards = 1;

    int final_hand[2] = { 6, 6 };
    AnimPlan plan;
    int rc = anim_build_plan(ev, 3, 2, /*deck*/20, /*discard*/8, CARD_NONE, final_hand, &plan);
    CHECK(rc == ANIM_EOK, "plan rc");
    CHECK(plan.n_steps == 3, "3 steps");

    // Count-freeze (the fallback walk back from final): pre = {deck 24, discard 4, [4,4]}.
    CHECK(plan.pre.deck == 24, "pre deck 24 (got %d)", plan.pre.deck);
    CHECK(plan.pre.discard == 4, "pre discard 4 (got %d)", plan.pre.discard);
    CHECK(plan.pre.hand[0] == 4 && plan.pre.hand[1] == 4, "pre hands [4,4] (got [%d,%d])", plan.pre.hand[0], plan.pre.hand[1]);

    // Per-step forward counts.
    CHECK(plan.steps[0].discard == 8 && plan.steps[0].deck == 24, "step0 discard grows to 8, deck still 24");
    CHECK(plan.steps[1].deck == 22 && plan.steps[1].hand[0] == 6, "step1 deck 22, seat0 hand 6");
    CHECK(plan.steps[2].deck == 20 && plan.steps[2].hand[1] == 6, "step2 deck 20, seat1 hand 6 (== final)");

    // Timing: every step ANIMATION_TIME; starts staggered by TIME+GAP; total wall.
    CHECK(plan.steps[0].duration_ms == ANIM_TIME_MS, "step duration == ANIMATION_TIME");
    CHECK(plan.steps[0].start_ms == 0, "step0 starts at 0");
    CHECK(plan.steps[1].start_ms == ANIM_TIME_MS + ANIM_GAP_MS, "step1 starts at TIME+GAP (%d)", plan.steps[1].start_ms);
    CHECK(plan.steps[2].start_ms == 2 * (ANIM_TIME_MS + ANIM_GAP_MS), "step2 starts at 2*(TIME+GAP)");
    CHECK(plan.total_ms == 2 * (ANIM_TIME_MS + ANIM_GAP_MS) + ANIM_TIME_MS, "total wall time (got %d)", plan.total_ms);

    // in-flight-from-deck: both refills leave the deck; neither is flipped-bound.
    CHECK(plan.steps[0].in_flight_from_deck == 0, "discard step not from deck");
    CHECK(plan.steps[1].in_flight_from_deck == 2 && plan.steps[1].in_flight_to_flipped == 0, "step1 2 from deck, 0 to flipped");
    CHECK(plan.steps[2].in_flight_from_deck == 2, "step2 2 from deck");

    // Veil: seat0's 2 REAL refill cards are in transit (hide until landed); the
    // masked backs and the discard cards are not veiled.
    CHECK(plan.n_veil == 2, "veil holds the 2 real refill cards (got %d)", plan.n_veil);
    int saw4 = 0, saw32 = 0;
    for (int i = 0; i < plan.n_veil; i++) { if (plan.veil_ids[i] == 4) saw4 = 1; if (plan.veil_ids[i] == 32) saw32 = 1; }
    CHECK(saw4 && saw32, "veil contains card ids 4 and 32");

    // Timing-policy seam: the duration function is the one place pacing is decided.
    CHECK(anim_step_duration_ms(ANIM_EVT_ATTACK_PASS) == ANIM_TIME_MS, "duration policy");
    CHECK(anim_step_duration_ms(ANIM_EVT_REVERT) == ANIM_TIME_MS, "revert paced like a kernel event");

    // An empty sequence is a legal no-op plan.
    AnimPlan empty;
    CHECK(anim_build_plan(NULL, 0, 2, 20, 8, CARD_NONE, final_hand, &empty) == ANIM_EOK && empty.n_steps == 0,
          "empty sequence -> empty plan");
}

// MUTATION-CHECKED against c/src/anim_plan.c: the freeze walking back from the
// final board over every event fails 2 cases here; anchoring and undoing nothing
// fails 1; undoing the first two events fails 3; a step deriving forward instead
// of adopting its own board fails 1.
//
// THE ANCHORED FREEZE, and the shape it exists for. A 2-player bout end off a
// deck of ONE whose next card is the flipped trump: seat 1 takes the table,
// then seat 0 draws TWO. The draw is real - the trump lies under the deck and is
// handed out last without ever being in deck_count - so undoing every event puts
// both cards back and opens the deck badge at 2. Anchoring on the FIRST event's
// own board and undoing exactly one says 1, which is the board that existed.
//
// Round 16, the owner: "I sometimes saw the deck suddenly go to 5 cards, then
// deal, and now I have 6 cards? Is it a problem with the flipped card?"
static void test_plan_anchors_on_the_first_events_own_board(void) {
    Card taken[4] = { C(0, 6), C(1, 7), C(2, 8), C(3, 9) };
    Card drawn[2] = { C(0, 10), C(2, 11) };

    // The board before the move: deck 1, discard 20, hands [3, 5].
    int pickup_hand[2] = { 3, 9 };   // seat 1 has taken the table
    int refill_hand[2] = { 5, 9 };   // seat 0 has drawn its two
    AnimPlanEvent ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = ANIM_EVT_PICKUP; ev[0].seat = 1; ev[0].from = ANIM_LOC_TABLE; ev[0].to = ANIM_LOC_HAND;
    ev[0].cards = taken; ev[0].n_cards = 4;
    ev[0].has_counts = 1; ev[0].deck = 1; ev[0].discard = 20; ev[0].hand = pickup_hand;
    ev[1].type = ANIM_EVT_REFILL; ev[1].seat = 0; ev[1].from = ANIM_LOC_DECK; ev[1].to = ANIM_LOC_HAND;
    ev[1].cards = drawn; ev[1].n_cards = 2;
    ev[1].has_counts = 1; ev[1].deck = 0; ev[1].discard = 20; ev[1].hand = refill_hand;

    int final_hand[2] = { 5, 9 };
    AnimPlan plan;
    CHECK(anim_build_plan(ev, 2, 2, /*deck*/0, /*discard*/20, CARD_NONE, final_hand, &plan) == ANIM_EOK,
          "the anchored plan builds");
    CHECK(plan.pre.deck == 1, "pre deck 1, not 2 (got %d)", plan.pre.deck);
    CHECK(plan.pre.discard == 20, "pre discard 20 (got %d)", plan.pre.discard);
    CHECK(plan.pre.hand[0] == 3 && plan.pre.hand[1] == 5,
          "pre hands [3,5] (got [%d,%d])", plan.pre.hand[0], plan.pre.hand[1]);

    // Each step lands on its OWN board rather than on a forward derivation of
    // it: derived, step 1's deck would be 1 - 2 = -1.
    CHECK(plan.steps[0].deck == 1 && plan.steps[0].hand[1] == 9, "step 0 is its own board");
    CHECK(plan.steps[1].deck == 0 && plan.steps[1].hand[0] == 5, "step 1 is its own board");

    // A step with no board of its own carries the walk forward from the one
    // before it, which is the only thing the delta is still for.
    ev[1].has_counts = 0; ev[1].hand = 0;
    CHECK(anim_build_plan(ev, 2, 2, 0, 20, CARD_NONE, final_hand, &plan) == ANIM_EOK, "mixed plan builds");
    CHECK(plan.pre.deck == 1, "the anchor is the FIRST event's board (got %d)", plan.pre.deck);
    CHECK(plan.steps[1].deck == -1 && plan.steps[1].hand[0] == 5,
          "a boardless step derives forward (got deck %d)", plan.steps[1].deck);

    // Bounds: no output, no final board, and events that were promised but not
    // handed over are each refused rather than read.
    CHECK(anim_build_plan(ev, 2, 2, 0, 20, CARD_NONE, final_hand, 0) == ANIM_EBADARG, "no output, no plan");
    CHECK(anim_build_plan(ev, 2, 2, 0, 20, CARD_NONE, 0, &plan) == ANIM_EBADARG, "no final board, no plan");
    CHECK(anim_build_plan(0, 2, 2, 0, 20, CARD_NONE, final_hand, &plan) == ANIM_EBADARG, "no events, no plan");
    CHECK(anim_build_plan(ev, ANIM_MAX_STEPS + 1, 2, 0, 20, CARD_NONE, final_hand, &plan) == ANIM_ECAP,
          "a sequence over the cap is refused, not truncated");
}

// MUTATION-CHECKED against c/src/anim_plan.c, each on its own:
//   adopt_counts stops copying `flipped`
//     (the freeze reads whatever the final board said)      -> 2 failures
//   apply_undo "restores" the trump on a DEAL/REFILL
//     (undoing the anchor event puts a dealt trump back)    -> 2 failures
//   the boardless fallback ignores `final_flipped`          -> 2 failures
//
// THE TRUMP UNDER THE DECK IS PART OF THE FREEZE. Owner, 1.1(55): "on a bout
// ending good, if the flipped card would've been animated in the resulting
// animation, it DOES NOT SHOW at first in the pile BEFORE the deal animations
// play. The deck shows, but not the flipped card."
//
// The shape: a 2-player bout end whose refill wants MORE cards than the stock
// holds, so the flipped trump goes out with them. Before the move the well is
// four backs with the trump tucked under; after it, the kernel's board has no
// flipped card at all. The freeze must open on the FORMER, or the well draws a
// frozen pile of four standing on nothing - the trump hidden in the place it
// still is, rather than only in the place it is going.
static void test_plan_freezes_the_flipped_trump(void) {
    const Card trump = C(0, 8);              // the 8 of spades, under the deck
    Card trashed[6] = { C(0, 11), C(1, 11), C(2, 12), C(3, 12), C(1, 5), C(2, 5) };
    Card drawn3[3]  = { C(3, 6), C(3, 7), C(0, 9) };
    Card drawn2[2]  = { C(1, 9), C(2, 9) };

    // Three steps of one bout end, each carrying the board it committed:
    //   0 CARDS_TO_TRASH  the table goes to the pile.   deck 4, trump still there
    //   1 REFILL seat 1   three cards off the stock.    deck 1, trump still there
    //   2 REFILL seat 0   the last stock card AND the trump. deck 0, trump GONE
    int h0[2] = { 3, 3 }, h1[2] = { 3, 6 }, h2[2] = { 5, 6 };
    AnimPlanEvent ev[3];
    memset(ev, 0, sizeof(ev));
    ev[0].type = ANIM_EVT_CARDS_TO_TRASH; ev[0].seat = ANIM_SEAT_NONE;
    ev[0].from = ANIM_LOC_TABLE; ev[0].to = ANIM_LOC_DISCARD;
    ev[0].cards = trashed; ev[0].n_cards = 6;
    ev[0].has_counts = 1; ev[0].deck = 4; ev[0].discard = 24; ev[0].flipped = trump;
    ev[0].hand = h0;
    ev[1].type = ANIM_EVT_REFILL; ev[1].seat = 1;
    ev[1].from = ANIM_LOC_DECK; ev[1].to = ANIM_LOC_HAND;
    ev[1].cards = drawn3; ev[1].n_cards = 3;
    ev[1].has_counts = 1; ev[1].deck = 1; ev[1].discard = 24; ev[1].flipped = trump;
    ev[1].hand = h1;
    ev[2].type = ANIM_EVT_REFILL; ev[2].seat = 0;
    ev[2].from = ANIM_LOC_DECK; ev[2].to = ANIM_LOC_HAND;
    ev[2].cards = drawn2; ev[2].n_cards = 2;
    ev[2].has_counts = 1; ev[2].deck = 0; ev[2].discard = 24; ev[2].flipped = CARD_NONE;
    ev[2].hand = h2;

    int final_hand[2] = { 5, 6 };
    AnimPlan plan;
    CHECK(anim_build_plan(ev, 3, 2, /*deck*/0, /*discard*/24, /*flipped*/CARD_NONE,
                          final_hand, &plan) == ANIM_EOK, "the bout-end plan builds");
    // The deck freezes at 4 - and the trump freezes WITH it. A well told
    // "deck 4, no trump" is the defect; five cards were in that stock.
    CHECK(plan.pre.deck == 4, "pre deck 4 (got %d)", plan.pre.deck);
    CHECK(!card_is_none(plan.pre.flipped),
          "the freeze still has a flipped trump");
    CHECK(plan.pre.flipped.suit == trump.suit && plan.pre.flipped.value == trump.value,
          "and it is the RIGHT card (got %d-%d, want %d-%d)",
          plan.pre.flipped.suit, plan.pre.flipped.value, trump.suit, trump.value);

    // …and a stream whose anchor board has already lost the trump freezes
    // WITHOUT one: the rule is "adopt", not "always assume there is one".
    AnimPlanEvent gone[1];
    memset(gone, 0, sizeof(gone));
    gone[0] = ev[2];
    CHECK(anim_build_plan(gone, 1, 2, 0, 24, CARD_NONE, final_hand, &plan) == ANIM_EOK,
          "the trump-less plan builds");
    CHECK(card_is_none(plan.pre.flipped), "no trump on the anchor, none in the freeze");

    // The boardless fallback has nothing to anchor on and says what the FINAL
    // board says - it cannot do better, and anim_plan.h says why.
    AnimPlanEvent bare[1];
    memset(bare, 0, sizeof(bare));
    bare[0].type = ANIM_EVT_REFILL; bare[0].seat = 0;
    bare[0].from = ANIM_LOC_DECK; bare[0].to = ANIM_LOC_HAND; bare[0].n_cards = 1;
    CHECK(anim_build_plan(bare, 1, 2, 3, 24, trump, final_hand, &plan) == ANIM_EOK,
          "the boardless plan builds");
    CHECK(plan.pre.flipped.suit == trump.suit && plan.pre.flipped.value == trump.value,
          "the fallback reports the final board's trump");
}

// ---- the surface plan (1.1(56): "LOBBY DID NOT UPDATE LIVE!") -------------
//
// THE FIVE STREAMS A LOBBY MESSAGE CAN CARRY, which is the whole set: a message
// comes from ONE participant, so they seat themselves or get up once, they must
// hold a seat to move the rules, and Start is always last.
//
//   join / leave        -> snap                (and a lone snap is the adopt)
//   passing moved       -> rotate
//   join + passing      -> snap, rest, rotate  - and it ENDS IN THE LOBBY
//   join + start        -> snap, rest, fade
//   start               -> fade
//
// MUTATION-CHECKED, each against the shape it exists to catch:
//   * dropping the `started` clause (the bug as reported) leaves join+start one
//     collapsed beat and `join_start` fails;
//   * staging a lone snap instead of answering 0 makes join-alone a two-render
//     transition and `join_alone` fails;
//   * spacing beats without ANIM_SURFACE_HOLD_MS makes every start_ms 0 and
//     `rest` fails - which is the "wait a bit" half of the report;
//   * mapping RULES onto a snap instead of a turn fails `rules_alone`.
static const char *kind_name(int k) {
    return k == ANIM_SURFACE_ROSTER ? "roster"
         : k == ANIM_SURFACE_RULES  ? "rules"
         : k == ANIM_SURFACE_BOARD  ? "board"
         : k == ANIM_SURFACE_LOBBY  ? "lobby" : "?";
}

static void test_surface_plan(void) {
    AnimSurfacePlan p;

    // A BOARD takes an arrival the way it always has - the live controller
    // folds it in, and a plan here would be a second opinion about it.
    CHECK(anim_surface_plan(0, 1, 1, 1, 1, 0, &p) == 0,
          "a board is handed no beats at all");

    // 1. JOIN, OR LEAVE: "just snap to the state where they are in the lobby
    //    and do nothing else." No beats - the ordinary adopt IS the snap.
    CHECK(anim_surface_plan(1, 1, 1, 1, 0, 0, &p) == 0, "join_alone: no beats");

    // 2. THE RULES MOVED: one beat, and the checkbox TURNS (owner: "lets do the
    //    'rotate in' or out thing for the checkbox").
    CHECK(anim_surface_plan(1, 0, 1, 0, 0, 0, &p) == 1, "rules_alone: one beat");
    CHECK(p.beats[0].kind == ANIM_SURFACE_RULES, "rules_alone: the rules beat");
    CHECK(p.beats[0].transition == ANIM_TRANSITION_TURN,
          "rules_alone: a rule change TURNS (got %d)", p.beats[0].transition);
    CHECK(p.beats[0].passing == 0, "rules_alone: showing the NEW rule");
    CHECK(p.beats[0].start_ms == 0, "rules_alone: with nothing to wait for");

    // 3. JOIN + THE RULES: a stream that BEGINS with a snap and ENDS IN THE
    //    LOBBY. Assuming a snap is on its way to the table is the trap.
    CHECK(anim_surface_plan(1, 1, 1, 0, 0, 0, &p) == 2, "join_rules: two beats");
    CHECK(p.beats[0].kind == ANIM_SURFACE_ROSTER
          && p.beats[0].transition == ANIM_TRANSITION_SNAP
          && p.beats[0].passing == 1,
          "join_rules: the roster snaps first, still under the OLD rule (%s/%d/%d)",
          kind_name(p.beats[0].kind), p.beats[0].transition, p.beats[0].passing);
    CHECK(p.beats[1].kind == ANIM_SURFACE_RULES && p.beats[1].passing == 0,
          "join_rules: then the checkbox turns to the new rule");
    CHECK(p.beats[1].kind != ANIM_SURFACE_BOARD, "join_rules: and it ends in the LOBBY");
    CHECK(p.beats[1].start_ms == ANIM_SURFACE_HOLD_MS,
          "rest: the second beat waits a REST (got %d, want %d)",
          p.beats[1].start_ms, ANIM_SURFACE_HOLD_MS);

    // 4. JOIN + START in one text - the report: "snap to the state where there
    //    are two or whatever people in the lobby, wait a bit, then fade."
    CHECK(anim_surface_plan(1, 1, 1, 1, 1, 0, &p) == 2, "join_start: two beats");
    CHECK(p.beats[0].kind == ANIM_SURFACE_ROSTER
          && p.beats[0].transition == ANIM_TRANSITION_SNAP,
          "join_start: the roster snaps in first");
    CHECK(p.beats[1].kind == ANIM_SURFACE_BOARD
          && p.beats[1].transition == ANIM_TRANSITION_FADE,
          "join_start: then the table FADES in");
    CHECK(p.beats[1].start_ms == ANIM_SURFACE_HOLD_MS,
          "rest: and it waits a REST first (got %d)", p.beats[1].start_ms);
    CHECK(p.total_ms == ANIM_SURFACE_HOLD_MS + ANIM_TIME_MS,
          "join_start: the whole thing is the rest plus the fade (got %d)", p.total_ms);

    // 5. START, with nobody joining on the way: the fade, and NO snap in front
    //    of it - there is nothing to show first.
    CHECK(anim_surface_plan(1, 0, 1, 1, 1, 0, &p) == 1, "start_alone: one beat");
    CHECK(p.beats[0].kind == ANIM_SURFACE_BOARD
          && p.beats[0].transition == ANIM_TRANSITION_FADE
          && p.beats[0].start_ms == 0,
          "start_alone: it fades immediately, with no roster snap first");

    // THE CONTROLS, and the owner's four cases (see anim_plan.h for his words).
    //
    // A stream that ENDS IN THE LOBBY lets each beat offer what its own state
    // offers: Vera joining really does hand Alex a Start button, and that is
    // where the stream stops, so it appears and stays.
    CHECK(anim_surface_plan(1, 1, 1, 0, 0, 0, &p) == 2, "join+rules is two beats");
    CHECK(p.beats[0].controls == ANIM_SURFACE_CONTROLS_LIVE
          && p.beats[1].controls == ANIM_SURFACE_CONTROLS_LIVE,
          "controls_lobby: a stream that ends in the lobby shows its own controls");
    CHECK(anim_surface_plan(1, 0, 1, 0, 0, 0, &p) == 1, "rules alone is one beat");
    CHECK(p.beats[0].controls == ANIM_SURFACE_CONTROLS_LIVE, "controls_lobby: …and so does a lone rule change");

    // A stream that ends AT THE BOARD touches no control for its whole length.
    // Owner: "we snap her in (NOT AFFECTING ALEXS BUTTONS) and then fade to the
    // game." Flashing Start into existence and dissolving the lobby half a
    // second later is the flicker he first read as a disabled button.
    CHECK(anim_surface_plan(1, 1, 1, 1, 1, 0, &p) == 2, "join+start is two beats");
    CHECK(p.beats[0].controls == ANIM_SURFACE_CONTROLS_HELD,
          "controls_board: the roster snaps but the buttons are HELD");
    CHECK(p.beats[0].kind == ANIM_SURFACE_ROSTER,
          "controls_board: …and it is still the roster beat - she IS snapped in");
    CHECK(anim_surface_plan(1, 1, 1, 0, 1, 0, &p) == 3, "join+rules+start is three beats");
    for (int i = 0; i < 3; i++)
        CHECK(p.beats[i].controls == ANIM_SURFACE_CONTROLS_HELD,
              "controls_board: every beat of it, not just the first (beat %d)", i);

    // ---- THE REVERSALS (1.1(68)) -----------------------------------------
    //
    // Undoing a staged lobby action is this same function asked the other way
    // round - `showing` is what the draft produced, `arriving` is the chain the
    // thread still has - so each of the three comes back wearing the idiom it
    // was made in, with no per-action case anywhere. Owner: "if i leave, it
    // stages a bubble. then if I X on that bubble, it should snap me back in.
    // and same for toggling passing, it should 'unrotate'."

    // R1. UNDOING A START. The only delta a text can never be (rule P ranks a
    //     dealt game above its own invite), and the only one a BOARD has a plan
    //     for at all.
    CHECK(anim_surface_plan(0, 0, 1, 1, 0, 1, &p) == 1, "undo_start: one beat");
    CHECK(p.beats[0].kind == ANIM_SURFACE_LOBBY,
          "undo_start: the board gives way to the LOBBY (got %s)", kind_name(p.beats[0].kind));
    CHECK(p.beats[0].transition == ANIM_TRANSITION_FADE,
          "undo_start: and it FADES, the same idiom the start wore (got %d)",
          p.beats[0].transition);
    CHECK(p.beats[0].start_ms == 0 && p.beats[0].duration_ms == ANIM_TIME_MS,
          "undo_start: immediately, over one beat");
    CHECK(p.beats[0].controls == ANIM_SURFACE_CONTROLS_LIVE,
          "undo_start: the lobby it lands on draws its OWN controls - the stream ends there");
    CHECK(p.settle_ms == p.total_ms,
          "undo_start: nothing may put the surface away before the fade is done (%d vs %d)",
          p.settle_ms, p.total_ms);

    // R2. `ended` OUTRANKS THE REST OF THE DELTA. A board never showed the
    //     roster it is going back to, so there is nothing to snap on the way and
    //     a rule that moved inside the discarded draft is part of the surface
    //     being faded into, not a beat of its own.
    CHECK(anim_surface_plan(0, 1, 0, 1, 0, 1, &p) == 1,
          "undo_start: still ONE beat with a roster and a rule move in the delta");
    CHECK(p.beats[0].kind == ANIM_SURFACE_LOBBY && p.beats[0].passing == 1,
          "undo_start: and it shows the arriving chain's own rule");

    // R3. UNDOING A RULES TOGGLE turns the box back. Identical to an arriving
    //     rule change, because it IS one - only the argument order differs.
    CHECK(anim_surface_plan(1, 0, 0, 1, 0, 0, &p) == 1, "undo_rules: one beat");
    CHECK(p.beats[0].kind == ANIM_SURFACE_RULES
          && p.beats[0].transition == ANIM_TRANSITION_TURN
          && p.beats[0].passing == 1,
          "undo_rules: the checkbox turns back to the rule the table agreed");

    // R4. UNDOING A LEAVE snaps me back in - no beats, because the adopt IS the
    //     snap, but a SETTLE, because the drawer is about to eat it. This is the
    //     pair of lines that issue 2 turns on ("the leave snap happens mid
    //     collapse"): a caller reading only `n` has no length to wait for.
    CHECK(anim_surface_plan(1, 1, 1, 1, 0, 0, &p) == 0, "undo_leave: no beats");
    CHECK(p.settle_ms == ANIM_SURFACE_HOLD_MS,
          "undo_leave: …and one REST to read it in before the drawer moves (got %d, want %d)",
          p.settle_ms, ANIM_SURFACE_HOLD_MS);

    // R5. THE NO-OP. Owner: "if you toggle, then toggle back, then X the staged,
    //     it should detect that the resulting state is the same, and do zero
    //     animation. same if you leave then join AND END UP IN SAME ORDER IN
    //     GAME." Nothing tracks what was tapped; two chains that describe the
    //     same table simply diff to nothing, and a zero settle is what tells the
    //     caller not even to wait.
    CHECK(anim_surface_plan(1, 0, 1, 1, 0, 0, &p) == 0, "noop: no beats");
    CHECK(p.settle_ms == 0, "noop: and NOTHING to wait for either (got %d)", p.settle_ms);

    // R6. THE SWAP: the whole-surface change with no second chain to diff.
    //     Owner, on the audit's U6: "for U6 ... lets prefer fades." The New
    //     game screen becoming a lobby, and that lobby being discarded, were
    //     the only two whole-surface changes that CUT while their mirrors
    //     faded - so they are the same beat, asked for by a caller that has
    //     only one side to show.
    CHECK(anim_surface_swap(1, &p) == 1, "swap: one beat");
    CHECK(p.beats[0].kind == ANIM_SURFACE_LOBBY,
          "swap: it is the LOBBY beat, the same one a discarded start wears (got %s)",
          kind_name(p.beats[0].kind));
    CHECK(p.beats[0].transition == ANIM_TRANSITION_FADE,
          "swap: and it FADES - which is the whole ruling (got %d)",
          p.beats[0].transition);
    CHECK(p.beats[0].duration_ms == ANIM_TIME_MS && p.beats[0].start_ms == 0,
          "swap: immediately, over one beat (%d at %d)",
          p.beats[0].duration_ms, p.beats[0].start_ms);
    CHECK(p.settle_ms == p.total_ms && p.settle_ms == ANIM_TIME_MS,
          "swap: and nothing may put the surface away before it is done (%d/%d)",
          p.settle_ms, p.total_ms);
    CHECK(p.beats[0].passing == 1, "swap: carrying the rule it was handed");
    CHECK(anim_surface_swap(0, &p) == 1 && p.beats[0].passing == 0,
          "swap: …either rule");
    // THE TWO ENTRIES AGREE ABOUT THE IDIOM, which is the thing that would
    // rot: a swap is the same beat a reversal is, and if one of them ever
    // stops being a fade of ANIM_TIME_MS the other must too.
    {
        AnimSurfacePlan q;
        CHECK(anim_surface_plan(0, 0, 1, 1, 0, 1, &q) == 1, "…and a reversal is one beat");
        CHECK(q.beats[0].kind == p.beats[0].kind
              && q.beats[0].transition == p.beats[0].transition
              && q.beats[0].duration_ms == p.beats[0].duration_ms,
              "swap and reversal are the SAME beat (%s/%d/%d vs %s/%d/%d)",
              kind_name(q.beats[0].kind), q.beats[0].transition, q.beats[0].duration_ms,
              kind_name(p.beats[0].kind), p.beats[0].transition, p.beats[0].duration_ms);
    }

    // Every combination, and the invariants that hold across all of them.
    for (int lobby = 0; lobby <= 1; lobby++)
    for (int roster = 0; roster <= 1; roster++)
    for (int pb = 0; pb <= 1; pb++)
    for (int pa = 0; pa <= 1; pa++)
    for (int started = 0; started <= 1; started++)
    for (int ended = 0; ended <= 1; ended++) {
        const int k = anim_surface_plan(lobby, roster, pb, pa, started, ended, &p);
        CHECK(k >= 0 && k <= ANIM_SURFACE_MAX_BEATS, "beat count in range (%d)", k);
        // A reversal is one whole-surface fade and nothing else, whatever else
        // the delta says - it outranks every other clause.
        if (ended) CHECK(k == 1 && p.beats[0].kind == ANIM_SURFACE_LOBBY,
                         "ended: one LOBBY beat and only that (k=%d)", k);
        for (int i = 0; i < k; i++) {
            CHECK(i == 0 || p.beats[i].start_ms
                  >= p.beats[i - 1].start_ms + p.beats[i - 1].duration_ms,
                  "no beat starts before the one before it has finished");
            if (p.beats[i].kind == ANIM_SURFACE_BOARD) {
                CHECK(i == k - 1, "a BOARD beat is always the last one");
                CHECK(p.beats[i].transition == ANIM_TRANSITION_FADE, "and it always fades");
            }
            // The two whole-surface beats are one rule read in both directions:
            // a surface replacing another one fades, going either way.
            if (p.beats[i].kind == ANIM_SURFACE_LOBBY) {
                CHECK(k == 1, "a LOBBY beat is the whole plan");
                CHECK(p.beats[i].transition == ANIM_TRANSITION_FADE, "and it always fades");
            }
            CHECK(p.beats[i].controls == ((started && lobby && !ended)
                                          ? ANIM_SURFACE_CONTROLS_HELD
                                          : ANIM_SURFACE_CONTROLS_LIVE),
                  "the controls are held for exactly the streams that end at the board");
        }
        if (k > 0) {
            CHECK(p.beats[k - 1].passing == pa,
                  "the LAST beat always shows the arriving chain's own rule - "
                  "playing it IS adopting");
            CHECK((p.beats[k - 1].kind == ANIM_SURFACE_BOARD) == (started && lobby && !ended),
                  "the stream ends at the board exactly when the game started");
        }
        // THE SETTLE IS NEVER SHORTER THAN THE BEATS, and it is zero for
        // exactly the deltas that change nothing about the surface. Both halves
        // asserted here rather than only in the rows above, because this is the
        // number a collapsing drawer waits on and "0" is silently the old
        // behaviour.
        CHECK(p.settle_ms >= p.total_ms,
              "the settle covers the beats (settle %d, total %d)", p.settle_ms, p.total_ms);
        const int moved = ended || (lobby && (roster || pa != pb || started));
        CHECK((p.settle_ms > 0) == moved,
              "the settle is zero exactly when the surface does not change "
              "(settle %d, moved %d)", p.settle_ms, moved);
    }
}


// ======================================================================
// 6. THE WHOLE LOBBY ENUMERATION, as one table
// ======================================================================
//
// The owner asked for every scenario he can see - "1:1 vs large group, 2
// players already vs just 1, incoming pass toggle, incoming join, incoming
// leave, incoming start game" - enumerated with its expected animation AND its
// rules, and then ruled on the ones that were open. This is that table, in the
// one place where both halves can be checked against the kernel that decides
// them: `msg_lobby_*` says what a lobby offers whom, `anim_surface_plan` says
// what the arrival looks like.
//
// WHY THE LEGALITY COLUMN IS DERIVED AND NOT DECLARED. The easy version of this
// test writes "join+start in a group: impossible" in a table and asserts the
// table against itself, which proves nothing and would keep passing if the rule
// were deleted. So `sendable` below is COMPUTED by asking the kernel the same
// questions the sender's own screen asks - was she offered Join, may she move
// the rules from that seat, was she offered Start - and the table only records
// what the answer must come out as. Every "impossible" row is therefore a real
// assertion about the rule, and three of them are load-bearing:
//
//   * a group join+start is refused by the M9 anti-lockout gate, not by a
//     special case about group chats (row B3);
//   * a leaver cannot also move the rules, because leaving takes the seat that
//     moving them requires (row A11 - the owner: "no a leaver should not be
//     able to toggle");
//   * the last joiner CAN start when their join fills the table, in both of the
//     ways a table fills (rows A3 and B12 - the owner: "the only times that the
//     last joiner can start the game is if it's a 1:1 and they're the second
//     player, or if them joining brings the game to 8 in a group chat").
//
// ALEX IS SEAT 0 AND CREATED THE GAME. "Vera" is whoever sent the arriving
// text. A capacity of 2 is a 1:1 and 8 is a group chat, which is the only way
// the two differ: there is no chat-kind flag anywhere in the kernel, and every
// 1:1-only behaviour in the owner's enumeration turns out to be a full-table
// behaviour that a 2-seat table simply reaches sooner.

typedef struct {
    const char *name;
    int capacity;          // 2 = a 1:1, 8 = a group
    int seated_before;     // Alex included
    // The arriving text. One participant, so at most one roster action, and
    // `rules`/`start` are what they did after it in the same draft - a draft
    // REPLACES rather than queues, so several taps arrive as one envelope.
    int join, leave, rules, start;
    // Was the actor the last actor on the chain she is acting from? Only asked
    // of somebody already seated: a joiner is the newest by having just joined.
    int actor_newest_before;
    // Could a device really have produced this text? DERIVED below; this is the
    // answer it must come out as.
    int sendable;
    // What Alex sees. `beats` is anim_surface_plan's count; `held` is whether
    // his controls are frozen for the stream (ANIM_SURFACE_CONTROLS_*).
    int beats;
    int held;
    int alex_before, alex_after;   // MSG_LOBBY_*
    int exit_before, exit_after;
} Scen;

#define X 0   // don't-care: unreachable rows carry no expectation

static const Scen SCENS[] = {
// ---- 1:1 (capacity 2) ----------------------------------------------------
{"A1  dm  1p  join",           2,1, 1,0,0,0, 0, 1, 0, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_WAITING, MSG_LOBBY_START,  0,1},
{"A2  dm  1p  join+rules",     2,1, 1,0,1,0, 0, 1, 2, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_WAITING, MSG_LOBBY_START,  0,1},
{"A3  dm  1p  join+start",     2,1, 1,0,0,1, 0, 1, 2, ANIM_SURFACE_CONTROLS_HELD,
                                              MSG_LOBBY_WAITING, MSG_LOBBY_START,  0,1},
{"A4  dm  1p  rules alone",    2,1, 0,0,1,0, 0, 0, X, X, X, X, X, X},
{"A5  dm  1p  leave",          2,1, 0,1,0,0, 0, 0, X, X, X, X, X, X},
{"A6  dm  1p  start alone",    2,1, 0,0,0,1, 0, 0, X, X, X, X, X, X},
{"A7  dm  2p  rules alone",    2,2, 0,0,1,0, 1, 1, 1, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"A8  dm  2p  start alone",    2,2, 0,0,0,1, 1, 1, 1, ANIM_SURFACE_CONTROLS_HELD,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"A9  dm  2p  leave",          2,2, 0,1,0,0, 1, 1, 0, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_INVITE, 1,0},
{"A10 dm  2p  join (full)",    2,2, 1,0,0,0, 0, 0, X, X, X, X, X, X},
{"A11 dm  2p  leave+rules",    2,2, 0,1,1,0, 1, 0, X, X, X, X, X, X},
// ---- group (capacity 8) --------------------------------------------------
{"B1  grp 1p  join",           8,1, 1,0,0,0, 0, 1, 0, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_WAITING, MSG_LOBBY_START,  0,1},
{"B2  grp 1p  join+rules",     8,1, 1,0,1,0, 0, 1, 2, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_WAITING, MSG_LOBBY_START,  0,1},
{"B3  grp 1p  join+start",     8,1, 1,0,0,1, 0, 0, X, X, X, X, X, X},
{"B5  grp 2p  join",           8,2, 1,0,0,0, 0, 1, 0, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"B6  grp 2p  join+rules",     8,2, 1,0,1,0, 0, 1, 2, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"B7  grp 2p  rules alone",    8,2, 0,0,1,0, 1, 1, 1, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"B8a grp 3p  start (not newest)", 8,3, 0,0,0,1, 0, 1, 1, ANIM_SURFACE_CONTROLS_HELD,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"B8b grp 3p  start (newest)", 8,3, 0,0,0,1, 1, 0, X, X, X, X, X, X},
{"B9  grp 3p  leave",          8,3, 0,1,0,0, 1, 1, 0, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
{"B10 grp 2p  leave",          8,2, 0,1,0,0, 1, 1, 0, ANIM_SURFACE_CONTROLS_LIVE,
                                              MSG_LOBBY_START,   MSG_LOBBY_INVITE, 1,0},
{"B11 grp 8p  join (full)",    8,8, 1,0,0,0, 0, 0, X, X, X, X, X, X},
// ---- the rules gate, from the other side: moving the checkbox spends the
// same right to Start that being the newest sender does, and unlike M9 it has
// NO full-table exemption. Owner: "whoever changes the checkbox value cannot
// start the game." A 1:1 is where it earns its keep - the table is full the
// moment both are in, so without this the changer could flip the rules and deal
// them in one breath, and their opponent would first learn of it from a board
// that will not let them pass.
{"A12 dm  2p  rules+start",    2,2, 0,0,1,1, 1, 0, X, X, X, X, X, X},
{"B13 grp 3p  rules+start",    8,3, 0,0,1,1, 0, 0, X, X, X, X, X, X},
{"B14 grp 1p  join+rules+start",8,1, 1,0,1,1, 0, 0, X, X, X, X, X, X},
{"A13 dm  1p  join+rules+start",2,1, 1,0,1,1, 0, 0, X, X, X, X, X, X},
{"B12 grp 7p  join+start (fills it)", 8,7, 1,0,0,1, 0, 1, 2, ANIM_SURFACE_CONTROLS_HELD,
                                              MSG_LOBBY_START,   MSG_LOBBY_START,  1,1},
};

static void test_lobby_scenarios(void) {
    AnimSurfacePlan p;
    for (unsigned s = 0; s < sizeof SCENS / sizeof SCENS[0]; s++) {
        const Scen *t = &SCENS[s];

        // ---- could Vera really have sent this? Asked of the kernel, in the
        // order her own screen would have asked it.
        int vera, seated_mid, sendable = 1;
        if (t->join) {
            // She was a viewer with no seat, and the lobby had to OFFER her one.
            sendable &= msg_lobby_offered(-1, t->seated_before, t->capacity, 0, 0)
                        == MSG_LOBBY_JOIN;
            vera = t->seated_before;          // seats are claimed lowest-free-first
            seated_mid = t->seated_before + 1;
        } else if (t->leave) {
            vera = t->seated_before - 1;      // the most recent joiner
            sendable &= msg_lobby_can_exit(vera, t->seated_before);
            seated_mid = t->seated_before - 1;
        } else {
            // Already seated - and there IS no such person on a table of one,
            // where the only seat is Alex's.
            vera = t->seated_before >= 2 ? t->seated_before - 1 : -1;
            seated_mid = t->seated_before;
        }
        // Leaving spends the seat everything else needs. This is the whole of
        // the owner's A11 ruling, and it needs no clause of its own.
        const int vera_now = t->leave ? -1 : vera;
        if (t->rules) sendable &= msg_lobby_can_set_rules(vera_now);
        if (t->start)
            sendable &= msg_lobby_offered(vera_now, seated_mid, t->capacity,
                                          (t->join || t->rules) ? 1 : t->actor_newest_before,
                                          t->rules)
                        == MSG_LOBBY_START;

        CHECK(!!sendable == !!t->sendable, "%s: sendable is %d, expected %d",
              t->name, !!sendable, t->sendable);
        // A row the rules refuse carries no expectation past this point - its
        // remaining columns are don't-cares. Gated on the EXPECTED verdict as
        // well as the computed one, so a rule that breaks and lets an
        // impossible text through fails on the line above and does not then
        // bury that one signal under a dozen assertions about columns nobody
        // ever filled in. (Learned the hard way: the first cut of this loop
        // made all six mutations below produce the identical failure set.)
        if (!t->sendable || !sendable) continue;

        // ---- and after she leaves she is a viewer again, with nothing but
        // Join. "as soon as they leave, the extension view should be as if they
        // haven't joined, and only show the join button."
        if (t->leave) {
            CHECK(msg_lobby_can_set_rules(vera_now) == 0,
                  "%s: a leaver cannot move the rules", t->name);
            CHECK(msg_lobby_offered(vera_now, seated_mid, t->capacity, 1, 0)
                  == MSG_LOBBY_JOIN,
                  "%s: a leaver is offered Join and nothing else", t->name);
        }

        // ---- what Alex sees. Before the arrival he is the newest actor
        // exactly when he is alone: he created the lobby and sent it. After it
        // he never is - the arriving bubble is Vera's.
        const int alex_before =
            msg_lobby_offered(0, t->seated_before, t->capacity,
                              t->seated_before == 1, 0);
        const int alex_after =
            msg_lobby_offered(0, seated_mid, t->capacity, 0, 0);
        CHECK(alex_before == t->alex_before, "%s: Alex had %d, expected %d",
              t->name, alex_before, t->alex_before);
        CHECK(alex_after == t->alex_after, "%s: Alex ends with %d, expected %d",
              t->name, alex_after, t->alex_after);
        CHECK(msg_lobby_can_exit(0, t->seated_before) == t->exit_before,
              "%s: Alex's Leave before", t->name);
        CHECK(msg_lobby_can_exit(0, seated_mid) == t->exit_after,
              "%s: Alex's Leave after", t->name);

        // ---- and what it looks like arriving.
        const int n = anim_surface_plan(1, t->join || t->leave,
                                        1, t->rules ? 0 : 1, t->start, 0, &p);
        CHECK(n == t->beats, "%s: %d beats, expected %d", t->name, n, t->beats);
        for (int i = 0; i < n; i++)
            CHECK(p.beats[i].controls == t->held,
                  "%s: beat %d controls %d, expected %d",
                  t->name, i, p.beats[i].controls, t->held);
        // The stream ends at the board exactly when the text started the game,
        // and a stream that ends in the lobby never touches the board.
        if (n > 0)
            CHECK((p.beats[n - 1].kind == ANIM_SURFACE_BOARD) == !!t->start,
                  "%s: ends at the board iff it started", t->name);
    }

    // ---- A LEAVER IS NOT THE PLAYER WHO STAYED (1.1(57)).
    //
    // Owner, off a real device: "I was able to leave, and then check the passing
    // box. really not good". He was not toggling as himself - in a 1:1 the
    // kernel handed him SEAT 0, the seat of the player who stayed, so the box
    // was live because the lobby thought he was them.
    //
    // The route in is the DM complement in `msg_seat_resolve`: `leaveLobby`
    // seals with last_actor_seat = joins.count = 1, `1 - 1` is 0, and seat 0 is
    // occupied - so the membership check passed, because it only ever asked
    // whether the seat EXISTS. See msg_wire.c for why a lobby seat is a name.
    //
    // Walked over both chat shapes, every seat the leaver could have held, and
    // both senderIsLocal values, because the failing combination was exactly one
    // corner of that space (capacity 2, senderIsLocal false) and a single
    // hand-picked case is how it survived this long.
    {
        const char *me = "Alex";
        const char *others[3] = { "Zed", "Vera", "Bob" };
        for (int cap = 2; cap <= 8; cap += 6)
        for (int before = 2; before <= (cap == 2 ? 2 : 4); before++)
        for (int myseat = 0; myseat < before; myseat++) {
            // The roster `leaveLobby` seals: everyone else, RENUMBERED compactly.
            MsgJoin after[MSG_MAX_JOINS]; int n = 0;
            for (int st = 0; st < before; st++) {
                if (st == myseat) continue;
                after[n].seat = (uint8_t)n;
                after[n].name_len = (uint8_t)strlen(others[st % 3]);
                memcpy(after[n].name, others[st % 3], after[n].name_len);
                n++;
            }
            for (int local = 0; local <= 1; local++) {
                const int seat = msg_seat_resolve_in_lobby(
                    after, n, /*cached, forgotten on exit*/ -1, local, cap,
                    /*last_actor_seat, what leaveLobby stamps*/ n, cap == 2,
                    me, (int)strlen(me));
                CHECK(seat < 0,
                      "leaver keeps a seat: cap=%d before=%d myseat=%d local=%d -> %d",
                      cap, before, myseat, local, seat);
                CHECK(msg_lobby_can_set_rules(seat) == 0,
                      "…and so the checkbox stays dead (cap=%d local=%d)", cap, local);
            }
        }
        // AND THE CONVERSE, so this is not simply "a lobby never resolves
        // anybody": a device whose name IS on the roster still gets its seat,
        // which is the path every seated player takes.
        MsgJoin r[2];
        r[0].seat = 0; r[0].name_len = 4; memcpy(r[0].name, "Zed\0", 4); r[0].name_len = 3;
        r[1].seat = 1; r[1].name_len = 4; memcpy(r[1].name, "Alex", 4);
        CHECK(msg_seat_resolve_in_lobby(r, 2, -1, 0, 2, 0, 1, me, 4) == 1,
              "a seated player is still found by name");
    }

    // ---- THE STALE SURFACE (C5). A gap is not a queue of messages: the diff is
    // against WHAT IS ON SCREEN, so two texts the human never saw resolve as ONE
    // stream. Owner: "compose some stream of snap/rotate/fade if you need to."
    //
    // This is the sixth stream, and it exists ONLY here: no single text can
    // carry it, because whoever moves the rules cannot also start (row A11's
    // sibling gate). Reachable across two texts - Vera joins and toggles, Bob
    // starts - and the surface that was open and behind must play all three.
    CHECK(anim_surface_plan(1, 1, 1, 0, 1, 0, &p) == 3,
          "gap: join + rules + start composes three beats");
    CHECK(p.beats[0].kind == ANIM_SURFACE_ROSTER
          && p.beats[1].kind == ANIM_SURFACE_RULES
          && p.beats[2].kind == ANIM_SURFACE_BOARD,
          "gap: snap, then rotate, then fade - in the only order they can have happened");
    CHECK(p.beats[2].start_ms == 2 * ANIM_SURFACE_HOLD_MS + ANIM_TIME_MS,
          "gap: each beat still waits a rest (got %d)", p.beats[2].start_ms);
}

#undef X

// ======================================================================
// 8. the plan, RE-ASKED (anim_plan_at)
// ======================================================================
//
// THE POINT OF THE ENTRY, and why a plan alone was not enough. A host with a
// frame loop does not want a chain of timers it has to cancel and rebuild when
// something arrives; it wants to ask, every frame, "where does this stand at
// now_ms", and be told by the one rule. Everything the web kept in React state
// while its setTimeout chain walked - which step is flying, which board to
// commit, which badges to show, which cards are still veiled - is an answer
// here, so a push landing mid-flight is answered by the NEXT call.
//
// The clock is an ARGUMENT. The kernel calls nothing (anim_plan.h's
// import-free rule), so `now_ms` is measured from the sequence's start and the
// host owns the origin.
static void test_plan_sampled_per_frame(void) {
    // The same three-step sequence test_plan_building builds: DISCARD, then
    // seat 0's REFILL of two real cards (ids 4 and 32), then seat 1's masked
    // REFILL. Steps start at 0, 525, 1050; each flies for 500.
    Card refill0[] = { C(0, 5), C(2, 7) };
    Card refill1[] = { C(-1, -1), C(-1, -1) };
    AnimPlanEvent ev[3];
    memset(ev, 0, sizeof(ev));
    ev[0].type = ANIM_EVT_DISCARD; ev[0].seat = ANIM_SEAT_NONE; ev[0].from = ANIM_LOC_TABLE; ev[0].to = ANIM_LOC_DISCARD; ev[0].n_cards = 4;
    ev[1].type = ANIM_EVT_REFILL;  ev[1].seat = 0; ev[1].from = ANIM_LOC_DECK; ev[1].to = ANIM_LOC_HAND; ev[1].cards = refill0; ev[1].n_cards = 2;
    ev[2].type = ANIM_EVT_REFILL;  ev[2].seat = 1; ev[2].from = ANIM_LOC_DECK; ev[2].to = ANIM_LOC_HAND; ev[2].cards = refill1; ev[2].n_cards = 2; ev[2].mask_cards = 1;
    int final_hand[2] = { 6, 6 };
    AnimPlan plan;
    CHECK(anim_build_plan(ev, 3, 2, 20, 8, CARD_NONE, final_hand, &plan) == ANIM_EOK, "the plan builds");

    // THE PER-STEP REVEAL SET, which the whole-sequence veil_ids cannot answer:
    // "is this card still veiled at now_ms" needs to know WHICH step lifts it.
    CHECK(plan.steps[0].reveals == 0, "the discard step reveals nothing");
    CHECK(plan.steps[1].reveals == ((uint64_t)1 << 4 | (uint64_t)1 << 32),
          "the real refill reveals ids 4 and 32");
    CHECK(plan.steps[2].reveals == 0, "a masked refill reveals nothing");

    // ...AND IT REVEALS NOTHING EVEN WHEN IT CARRIES IDENTITIES. The wire's own
    // masked backs are {-1,-1} and have no dense id, so the veil's range check
    // answers for them by accident; `mask_cards` is what answers when a caller
    // hands over real cards it has told us not to trust.
    {
        Card real_but_masked[] = { C(1, 4), C(3, 9) };
        AnimPlanEvent m = ev[2];
        m.cards = real_but_masked;
        AnimPlan mp;
        CHECK(anim_build_plan(&m, 1, 2, 20, 8, CARD_NONE, final_hand, &mp) == ANIM_EOK,
              "the masked-with-identities plan builds");
        CHECK(mp.steps[0].reveals == 0 && mp.n_veil == 0,
              "a step flagged masked veils and reveals nothing it names (got %llu / %d)",
              (unsigned long long)mp.steps[0].reveals, mp.n_veil);
    }

    AnimFrame f;
    // FRAME ZERO. Step 0's flight is playing, nothing has landed, and the
    // badges are the FREEZE - the board before the move, not the first step's.
    CHECK(anim_plan_at(&plan, 0, &f) == ANIM_EOK, "frame 0 samples");
    CHECK(f.step == 0 && f.elapsed_ms == 0, "step 0 is flying at 0 (got %d/%d)", f.step, f.elapsed_ms);
    CHECK(f.landed == 0 && f.done == 0, "nothing has landed at 0");
    CHECK(f.next_ms == ANIM_TIME_MS, "the next answer is step 0 landing (got %d)", f.next_ms);
    CHECK(f.deck == 24 && f.discard == 4 && f.hand[0] == 4 && f.hand[1] == 4,
          "frame 0 shows the freeze (got deck %d discard %d)", f.deck, f.discard);
    CHECK(f.veiled == ((uint64_t)1 << 4 | (uint64_t)1 << 32), "both real refill cards are veiled at 0");
    CHECK(f.in_flight_from_deck == 0, "the discard step takes nothing out of the deck");

    // ONE MILLISECOND BEFORE THE LANDING the answer has not changed yet.
    CHECK(anim_plan_at(&plan, ANIM_TIME_MS - 1, &f) == ANIM_EOK
          && f.step == 0 && f.landed == 0 && f.next_ms == ANIM_TIME_MS,
          "the step is still flying at TIME-1");

    // THE LANDING ITSELF. Step 0 has landed, so its own board is what shows;
    // the gap before step 1 is a moment with NO step playing, which the board
    // needs to know so it does not keep drawing a flight that finished.
    CHECK(anim_plan_at(&plan, ANIM_TIME_MS, &f) == ANIM_EOK, "the landing samples");
    CHECK(f.landed == 1, "step 0 has landed (got %d)", f.landed);
    CHECK(f.step == ANIM_STEP_NONE && f.elapsed_ms == 0, "the gap plays no step (got %d)", f.step);
    CHECK(f.next_ms == ANIM_TIME_MS + ANIM_GAP_MS, "the next answer is step 1 starting (got %d)", f.next_ms);
    CHECK(f.discard == 8 && f.deck == 24, "the landing shows step 0's own board (got %d/%d)", f.deck, f.discard);

    // STEP 1, in flight: its cards have LEFT the deck, which is what the badge
    // must show, and they are still veiled because the flight has not landed.
    CHECK(anim_plan_at(&plan, ANIM_TIME_MS + ANIM_GAP_MS, &f) == ANIM_EOK, "step 1 samples");
    CHECK(f.step == 1 && f.elapsed_ms == 0 && f.landed == 1, "step 1 starts after the gap");
    CHECK(f.in_flight_from_deck == 2 && f.in_flight_to_flipped == 0, "two cards are out of the deck");
    CHECK(f.veiled == ((uint64_t)1 << 4 | (uint64_t)1 << 32), "step 1's cards are veiled while they fly");

    // ...AND WHEN IT LANDS the veil lifts for exactly the cards it carried.
    CHECK(anim_plan_at(&plan, plan.steps[1].start_ms + ANIM_TIME_MS, &f) == ANIM_EOK
          && f.landed == 2 && f.veiled == 0,
          "step 1 landing lifts its own veil (got landed %d veiled %llu)",
          f.landed, (unsigned long long)f.veiled);

    // THE END, and past it. A sequence that is over says so and names no next
    // deadline, so a caller can stop asking; asking anyway is not an error.
    CHECK(anim_plan_at(&plan, plan.total_ms, &f) == ANIM_EOK, "the end samples");
    CHECK(f.done == 1 && f.landed == 3 && f.step == ANIM_STEP_NONE, "the sequence is done at total_ms");
    CHECK(f.next_ms == ANIM_NEVER, "a finished sequence names no deadline (got %d)", f.next_ms);
    CHECK(f.deck == 20 && f.hand[1] == 6, "the end shows the last step's board");
    AnimFrame later;
    CHECK(anim_plan_at(&plan, plan.total_ms + 1000000, &later) == ANIM_EOK
          && later.done == 1 && later.landed == 3 && later.next_ms == ANIM_NEVER,
          "far past the end is the same answer, not an error");

    // AN EMPTY SEQUENCE is done before it starts, which is what makes a caller
    // that always samples safe on a push that animated nothing.
    AnimPlan none;
    CHECK(anim_build_plan(NULL, 0, 2, 20, 8, CARD_NONE, final_hand, &none) == ANIM_EOK, "the empty plan builds");
    CHECK(anim_plan_at(&none, 0, &f) == ANIM_EOK && f.done == 1 && f.landed == 0
          && f.step == ANIM_STEP_NONE && f.next_ms == ANIM_NEVER,
          "an empty sequence is done at 0");

    // Bounds. A clock before the sequence began is a caller bug, not a frame.
    CHECK(anim_plan_at(NULL, 0, &f) == ANIM_EBADARG, "no plan, no frame");
    CHECK(anim_plan_at(&plan, 0, NULL) == ANIM_EBADARG, "no output, no frame");
    CHECK(anim_plan_at(&plan, -1, &f) == ANIM_EBADARG, "a clock before the start is refused");
}

// ======================================================================
// 9. the hand's order WITH FACE-DOWN SLOTS (anim_hand_laid_out_masked)
// ======================================================================
//
// THE DIVERGENCE THIS CLOSES. Three implementations of "what order is a hand
// drawn in" shipped on the web: mergeReplayHandOrder (ReplayScreen.tsx)
// reconciled face-down slots BY COUNT, displayedHand (clientReconcile.ts)
// reconciled BY KEY and could not express a face-down slot at all, and
// anim_hand_laid_out took dense ids only. So the same rearrangement scrubbed
// through a replay and played live produced two different arrays.
//
// One rule, and the slot a caller cannot NAME is ANIM_TABLE_UNKNOWN - the
// sentinel anim_plan.h already reserves for exactly this ("a card that is
// there and has no dense id"), kept off the deck by a static assert.
static void test_hand_order_with_hidden_slots(void) {
    const unsigned char U = ANIM_TABLE_UNKNOWN;
    unsigned char out[64];

    // A replay's revealed hand: two known cards and two face-down slots, in the
    // order the kernel hands them over.
    const unsigned char hand[] = { 10, U, 3, U };
    // What the viewer dragged it into: one known card, a face-down slot, the
    // other known card. It names ONE of the two backs.
    const unsigned char order[] = { 3, U, 10 };
    int n = anim_hand_laid_out_masked(hand, 4, 0, order, 3, out, (int)sizeof out);
    CHECK(n == 4, "every slot is laid out (got %d)", n);
    CHECK(out[0] == 3 && out[1] == U && out[2] == 10 && out[3] == U,
          "the preferred order holds and the unnamed back appends (got %d %d %d %d)",
          out[0], out[1], out[2], out[3]);

    // MORE BACKS IN THE ORDER THAN IN THE HAND: the extras fall out, because a
    // face-down slot has no identity to be stale about and the only thing that
    // can reconcile it is the COUNT. A hand of one back drawn as three is the
    // replay glitch this rule exists to make unrepresentable.
    const unsigned char one_back[] = { 7, U };
    const unsigned char greedy[] = { U, U, U, 7 };
    n = anim_hand_laid_out_masked(one_back, 2, 0, greedy, 4, out, (int)sizeof out);
    CHECK(n == 2 && out[0] == U && out[1] == 7,
          "one back is drawn once whatever the order asks for (got %d: %d %d)", n, out[0], out[1]);

    // A STALE KNOWN ID still drops out by construction, exactly as the
    // dense-id rule does: `order` is a grow-only memory of where cards sat.
    const unsigned char left[] = { U, 3 };
    n = anim_hand_laid_out_masked(left, 2, 0, order, 3, out, (int)sizeof out);
    CHECK(n == 2 && out[0] == 3 && out[1] == U,
          "a card that left the hand is not drawn (got %d: %d %d)", n, out[0], out[1]);

    // A DEFERRED card reserves no slot, and `order` must not place it either.
    n = anim_hand_laid_out_masked(hand, 4, (uint64_t)1 << 3, order, 3, out, (int)sizeof out);
    CHECK(n == 3 && out[0] == U && out[1] == 10 && out[2] == U,
          "a deferred card is laid out nowhere (got %d: %d %d %d)", n, out[0], out[1], out[2]);

    // NO ORDER AT ALL is the kernel's own order, backs included.
    n = anim_hand_laid_out_masked(hand, 4, 0, NULL, 0, out, (int)sizeof out);
    CHECK(n == 4 && out[0] == 10 && out[1] == U && out[2] == 3 && out[3] == U,
          "with no preference the kernel's order stands (got %d)", n);

    // The dense-id entry is the same rule with no backs in it, so the two can
    // never disagree about a hand that has none.
    unsigned char plain[64];
    const unsigned char known[] = { 10, 3 };
    const unsigned char pref[] = { 3, 10 };
    const int a = anim_hand_laid_out(known, 2, 0, pref, 2, plain, (int)sizeof plain);
    const int b = anim_hand_laid_out_masked(known, 2, 0, pref, 2, out, (int)sizeof out);
    CHECK(a == b && a == 2 && plain[0] == out[0] && plain[1] == out[1],
          "a hand with no backs gets one answer from both entries");

    // Bounds.
    CHECK(anim_hand_laid_out_masked(hand, 4, 0, order, 3, NULL, 4) == ANIM_EBADARG, "no output, no layout");
    CHECK(anim_hand_laid_out_masked(NULL, 4, 0, order, 3, out, 4) == ANIM_EBADARG, "no hand, no layout");
    CHECK(anim_hand_laid_out_masked(hand, -1, 0, order, 3, out, 4) == ANIM_EBADARG, "a negative hand is not a hand");
    CHECK(anim_hand_laid_out_masked(hand, 4, 0, NULL, 3, out, 4) == ANIM_EBADARG, "a promised order must be handed over");
    CHECK(anim_hand_laid_out_masked(hand, 4, 0, order, 3, out, 3) == ANIM_ECAP, "a layout that does not fit is refused");
}

int main(void) {
    printf("anim_plan_test\n");
    test_optimistic_animation();
    test_optimistic_revert();
    test_reconcile();
    test_plan_building();
    test_plan_anchors_on_the_first_events_own_board();
    test_plan_freezes_the_flipped_trump();
    test_surface_plan();
    test_lobby_scenarios();
    test_plan_sampled_per_frame();
    test_hand_order_with_hidden_slots();
    if (g_fails == 0) printf("anim_plan_test: OK\n");
    else              printf("anim_plan_test: %d FAILURES\n", g_fails);
    return g_fails ? 1 : 0;
}

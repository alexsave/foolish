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
// Plus test_plan_building() — the count-freeze (iOS preCounts backward walk), the
// per-step timing (ANIMATION_TIME), and the veil, which no TS test covered
// because that choreography lived inside AnimationContext's setState machinery.
//
// Usage: anim_plan_test              (no args)
// Modelled on tests/msg_wire_test.c's CHECK harness.

#include "../src/anim_plan.h"
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
// 4. plan building  (count-freeze / preCounts backward walk / veil / timing)
// ======================================================================
static void test_plan_building(void) {
    // A 2-player bout-end sequence, viewer = seat 0 (the attacker who drew):
    //   step0 DISCARD  table->discard, 4 cards
    //   step1 REFILL   deck->seat0 hand, 2 REAL cards (viewer's own draws)
    //   step2 REFILL   deck->seat1 hand, 2 MASKED backs (opponent's draws)
    // Final board: deck 20, discard 8, hands [6, 6].
    Card refill0[] = { C(0, 5), C(2, 7) };   // ids 4 and 32
    Card refill1[] = { C(-1, -1), C(-1, -1) };  // masked backs

    AnimEvent ev[3];
    memset(ev, 0, sizeof(ev));
    ev[0].type = ANIM_EVT_DISCARD;   ev[0].seat = ANIM_SEAT_NONE; ev[0].from = ANIM_LOC_TABLE; ev[0].to = ANIM_LOC_DISCARD; ev[0].n_cards = 4;
    ev[1].type = ANIM_EVT_REFILL;    ev[1].seat = 0; ev[1].from = ANIM_LOC_DECK; ev[1].to = ANIM_LOC_HAND; ev[1].cards = refill0; ev[1].n_cards = 2;
    ev[2].type = ANIM_EVT_REFILL;    ev[2].seat = 1; ev[2].from = ANIM_LOC_DECK; ev[2].to = ANIM_LOC_HAND; ev[2].cards = refill1; ev[2].n_cards = 2; ev[2].mask_cards = 1;

    int final_hand[2] = { 6, 6 };
    AnimPlan plan;
    int rc = anim_build_plan(ev, 3, 2, /*deck*/20, /*discard*/8, final_hand, &plan);
    CHECK(rc == ANIM_EOK, "plan rc");
    CHECK(plan.n_steps == 3, "3 steps");

    // Count-freeze (backward walk from final): pre = {deck 24, discard 4, [4,4]}.
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
    CHECK(anim_build_plan(NULL, 0, 2, 20, 8, final_hand, &empty) == ANIM_EOK && empty.n_steps == 0,
          "empty sequence -> empty plan");
}

int main(void) {
    printf("anim_plan_test\n");
    test_optimistic_animation();
    test_optimistic_revert();
    test_reconcile();
    test_plan_building();
    if (g_fails == 0) printf("anim_plan_test: OK\n");
    else              printf("anim_plan_test: %d FAILURES\n", g_fails);
    return g_fails ? 1 : 0;
}

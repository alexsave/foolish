/* The plan, the masked view and the beats.
 *
 *   - plan: the start is one ROUND; a bid is one BID; a call is CALL, REVEAL,
 *     LOSE, then OUT when it empties a cup, then OVER or the next ROUND; the
 *     events agree with the state they lead to; a plan of a range is the
 *     concatenation of its moves' plans; cn_plan_move equals the plan of the
 *     move once applied
 *   - view: another seat's dice never reach a seat's view; a spectator sees
 *     counts only; the reveal is public after a call; the menu
 *   - beats: one beat per event (two for a reveal), the frame's board at the
 *     end is the settled game's, the count lights one die at a time */
#include "cn_check.h"
#include "../src/cn_plan.h"
#include "../src/cn_view.h"
#include "../src/cn_beats.h"

static CnGame G, H;
static CnEvent EV[4096], EV2[64];

static void play_to_end(uint32_t k, int n)
{
    uint8_t seed[32];
    seed_wide(seed, k);
    cn_new(&G, seed, n);
    while (G.phase != CN_PH_OVER) cn_apply(&G, G.turn, bot_move(&G));
}

static void test_plan_shape(void)
{
    TEST("plan: the events of each move");
    for (uint32_t k = 0; k < 300; k++) {
        int n = 2 + (int)(k % 5);
        play_to_end(10 + k, n);
        int e = cn_plan(&G, -1, 0, EV, 64);
        CHECK(e == 1 && EV[0].kind == CN_EV_ROUND && EV[0].seat == 0 && EV[0].round == 0 && EV[0].move == 0,
              "game %u: the start is one ROUND", k);
        cn_new(&H, G.seed, n);
        for (int i = 0; i < G.hist_n; i++) {
            CnMove m = G.hist[i];
            int want_seat = H.turn;
            int e1 = cn_plan_move(&H, m, EV2, 64);
            cn_apply(&H, H.turn, m);
            int e2 = cn_plan(&G, i, i + 1, EV, 64);
            CHECK(e1 == e2 && e1 > 0 && !memcmp(EV, EV2, sizeof(CnEvent) * (size_t)e1), "game %u move %d: plan_move is the plan", k, i);
            for (int j = 0; j < e2; j++) CHECK(EV[j].move == i + 1, "game %u move %d: stamped with its move", k, i);
            if (!cn_is_call(m)) {
                CHECK(e2 == 1 && EV[0].kind == CN_EV_BID && EV[0].seat == want_seat && EV[0].q == m.q && EV[0].f == m.f,
                      "game %u move %d: a bid is one BID", k, i);
                continue;
            }
            CHECK(EV[0].kind == CN_EV_CALL && EV[0].seat == want_seat && EV[1].kind == CN_EV_REVEAL && EV[2].kind == CN_EV_LOSE,
                  "game %u move %d: CALL, REVEAL, LOSE", k, i);
            CHECK(EV[1].count == H.call_count && EV[2].seat == H.call_loser && EV[2].count == H.dice_n[H.call_loser],
                  "game %u move %d: the events are the state's", k, i);
            int c = 0;
            for (int s = 0; s < n; s++)
                for (int d = 0; d < EV[1].dice_n[s]; d++) {
                    int v = EV[1].dice[s * 5 + d];
                    c += v == EV[1].f || v == 1;
                }
            CHECK(c == EV[1].count, "game %u move %d: the revealed dice count %d, the event %d", k, i, c, EV[1].count);
            int j = 3;
            if (H.dice_n[H.call_loser] == 0) CHECK(EV[j++].kind == CN_EV_OUT, "game %u move %d: OUT", k, i);
            if (H.phase == CN_PH_OVER)
                CHECK(e2 == j + 1 && EV[j].kind == CN_EV_OVER && EV[j].seat == H.winner, "game %u: OVER last", k);
            else
                CHECK(e2 == j + 1 && EV[j].kind == CN_EV_ROUND && EV[j].seat == H.turn && EV[j].round == H.round,
                      "game %u move %d: the next ROUND, its opener", k, i);
        }
        /* a range is its moves, in order */
        int all = cn_plan(&G, -1, G.hist_n, EV, 4096);
        int at = 1, ok = all > 0 && EV[0].kind == CN_EV_ROUND;
        for (int i = 0; ok && i < G.hist_n; i++) {
            int e3 = cn_plan(&G, i, i + 1, EV2, 64);
            ok = e3 > 0 && at + e3 <= all && !memcmp(EV + at, EV2, sizeof(CnEvent) * (size_t)e3);
            at += e3;
        }
        CHECK(ok && at == all, "game %u: the whole plan is its moves' plans", k);
        CHECK(cn_plan(&G, 0, G.hist_n + 1, EV, 4096) == -1 && cn_plan(&G, 3, 2, EV, 64) == -1, "bad ranges refused");
        CHECK(cn_plan(&G, -1, G.hist_n, EV, 1) == -1, "a small cap refused");
    }
}

static void test_view_mask(void)
{
    TEST("view: masking");
    for (uint32_t k = 0; k < 200; k++) {
        uint8_t seed[32];
        seed_wide(seed, 500 + k);
        int n = 2 + (int)(k % 5);
        cn_new(&G, seed, n);
        for (int step = 0; G.phase != CN_PH_OVER && step < 40; step++) {
            for (int s = -1; s < n; s++) {
                CnView a, b;
                cn_view(&G, s, &a);
                H = G;
                for (int t = 0; t < n; t++)
                    if (t != s)
                        for (int i = 0; i < 5; i++) H.dice[t][i] = (uint8_t)(1 + (H.dice[t][i] % 6));
                cn_view(&H, s, &b);
                CHECK(!memcmp(&a, &b, sizeof a), "game %u seat %d: other cups do not reach the view", k, s);
                CHECK(a.viewer == (s >= 0 ? s : CN_VIEW_SPECTATOR), "viewer");
                if (s < 0) {
                    CHECK(a.my_n == 0 && !a.my_turn && !a.can_raise && !a.can_call, "a spectator has no dice and no menu");
                } else {
                    CHECK(a.my_n == G.dice_n[s], "game %u seat %d: my count", k, s);
                    for (int i = 1; i < a.my_n; i++) CHECK(a.my_dice[i - 1] <= a.my_dice[i], "sorted");
                    CHECK(a.my_turn == (G.turn == s), "my turn");
                }
                for (int i = 0; i < CN_MAX_DICE; i++) CHECK(a.all[i] == 0, "all[] only for CN_VIEW_ALL");
            }
            CnView all;
            cn_view(&G, CN_VIEW_ALL, &all);
            for (int s = 0; s < n; s++) {
                int c = 0;
                for (int i = 0; i < 5; i++) c += all.all[s * 5 + i] != 0;
                CHECK(c == G.dice_n[s], "ALL shows every die of seat %d", s);
            }
            cn_apply(&G, G.turn, bot_move(&G));
        }
    }
}

static void test_view_menu_reveal(void)
{
    TEST("view: the menu and the reveal");
    uint8_t seed[32];
    seed_wide(seed, 77);
    cn_new(&G, seed, 3);
    CnView v;
    cn_view(&G, 0, &v);
    CHECK(v.my_turn && v.can_raise && !v.can_call && v.min_q == 1 && v.min_f == 2 && v.max_q == 15, "the opener's menu");
    CHECK(v.call_seat == CN_SEAT_NONE && !v.revealed, "no call yet");
    cn_view(&G, 1, &v);
    CHECK(!v.my_turn && !v.can_raise && !v.can_call && !v.min_q, "not my turn: no menu");
    cn_apply(&G, 0, bid(4, 6));
    cn_view(&G, 1, &v);
    CHECK(v.can_call && v.can_raise && v.min_q == 5 && v.min_f == 2, "above four 6s is five 2s");
    CHECK(v.bid_q == 4 && v.bid_f == 6 && v.bidder == 0, "the standing bid");
    H = G;
    cn_apply(&G, 1, call_move());
    for (int s = -1; s < 3; s++) {
        cn_view(&G, s, &v);
        CHECK(v.revealed && v.call_seat == 1 && v.call_bidder == 0 && v.call_q == 4 && v.call_f == 6,
              "seat %d: the call is public", s);
        CHECK(v.call_count == cn_count(&H, 6) && v.call_true == (cn_count(&H, 6) >= 4), "seat %d: its count", s);
        for (int t = 0; t < 3; t++) {
            CHECK(v.shown_n[t] == 5, "seat %d sees seat %d's five", s, t);
            uint8_t want[5];
            memcpy(want, H.dice[t], 5);
            for (int i = 1; i < 5; i++)
                for (int j = i; j > 0 && want[j - 1] > want[j]; j--) { uint8_t x = want[j]; want[j] = want[j - 1]; want[j - 1] = x; }
            CHECK(!memcmp(v.shown + t * 5, want, 5), "seat %d sees seat %d's dice as they were", s, t);
        }
    }
    cn_view(&G, G.turn, &v);
    CHECK(v.phase == CN_PH_REVEALED && v.can_raise && !v.can_call, "the next opener bids, and may not call");
    cn_apply(&G, G.turn, bid(1, 2));
    cn_view(&G, 0, &v);
    CHECK(!v.revealed && v.call_seat == 1, "a bid ends the news, the last call stays readable");
    /* the top bid leaves only the call */
    G.bid_q = G.total; G.bid_f = 6;
    cn_view(&G, G.turn, &v);
    CHECK(v.can_call && !v.can_raise, "only the call");
}

static void test_beats(void)
{
    TEST("beats: the plan on a clock");
    for (uint32_t k = 0; k < 200; k++) {
        int n = 2 + (int)(k % 5);
        play_to_end(2000 + k, n);
        for (int i = 0; i < G.hist_n; i++) {
            int e = cn_plan(&G, i, i + 1, EV, 64);
            cn_replay(&H, &G, i);
            CnBeatFrame start, f;
            cn_beats_start(&H, &start);
            static CnBeats b;
            int nb = cn_beats_build(EV, e, &start, &b);
            int reveals = 0;
            for (int j = 0; j < e; j++) reveals += EV[j].kind == CN_EV_REVEAL;
            CHECK(nb == e + reveals, "game %u move %d: one beat an event, two for the reveal (%d)", k, i, nb);
            for (int j = 1; j < nb; j++)
                CHECK(b.beat[j].start_ms == b.beat[j - 1].start_ms + b.beat[j - 1].dur_ms + CN_T_GAP, "one after another");
            CHECK(b.start.bid_q == H.bid_q && !memcmp(b.start.dice_n, H.dice_n, sizeof H.dice_n) && !b.start.done,
                  "game %u move %d: the plan starts from the board before", k, i);
            cn_beats_frame(&b, 0, &f);
            CHECK(!f.done && f.state[0] == CN_BS_ACTIVE && f.prog[0] == 0.0f && (nb < 2 || f.state[1] == CN_BS_PENDING),
                  "game %u move %d: t 0 starts the first beat", k, i);
            cn_beats_frame(&b, b.total_ms, &f);
            cn_replay(&H, &G, i + 1);
            CnBeatFrame settled;
            cn_beats_start(&H, &settled);
            CHECK(f.done && f.next_ms == CN_BEAT_NEVER, "game %u move %d: done at the end", k, i);
            CHECK(f.bid_q == settled.bid_q && f.bid_f == settled.bid_f && f.bidder == settled.bidder
                  && !memcmp(f.dice_n, settled.dice_n, sizeof f.dice_n) && f.winner == settled.winner
                  && f.round == settled.round,
                  "game %u move %d: the end is the settled board", k, i);
            for (int j = 0; j < nb; j++) CHECK(f.state[j] == CN_BS_DONE && f.prog[j] == 1.0f, "every beat done");
            /* the clock only moves forward to the next change */
            uint32_t t = 0;
            int steps = 0;
            while (t != CN_BEAT_NEVER && steps < 200) {
                cn_beats_frame(&b, t, &f);
                CHECK(f.next_ms == CN_BEAT_NEVER || f.next_ms > t, "next is later");
                t = f.next_ms;
                steps++;
            }
            CHECK(steps < 200, "the frames end");
        }
    }
    /* THE COUNT: dice light one at a time, and the loser's count drops at
     * the end of its DROP */
    uint8_t seed[32];
    seed_wide(seed, 31);
    cn_new(&G, seed, 2);
    cn_apply(&G, 0, bid(1, 2));
    cn_apply(&G, 1, call_move());
    int e = cn_plan(&G, 1, 2, EV, 64);
    cn_replay(&H, &G, 1);
    CnBeatFrame start, f;
    cn_beats_start(&H, &start);
    static CnBeats b;
    cn_beats_build(EV, e, &start, &b);
    const CnBeat *count = 0, *drop = 0, *lift = 0;
    for (int j = 0; j < b.n; j++) {
        if (b.beat[j].kind == CN_BK_COUNT) count = &b.beat[j];
        if (b.beat[j].kind == CN_BK_DROP) drop = &b.beat[j];
        if (b.beat[j].kind == CN_BK_LIFT) lift = &b.beat[j];
    }
    CHECK(count && drop && lift && count->count == G.call_count, "a COUNT of %d", G.call_count);
    if (count && drop && lift) {
        cn_beats_frame(&b, lift->start_ms - 1, &f);
        CHECK(!f.cups_up, "cups down before the lift");
        cn_beats_frame(&b, lift->start_ms, &f);
        CHECK(f.cups_up, "cups up at the lift");
        for (uint32_t k = 0; k < count->count; k++) {
            cn_beats_frame(&b, count->start_ms + k * CN_T_COUNT_STEP, &f);
            CHECK(f.highlight_f == 2 && f.highlight_n == k + 1, "die %u lit (%d)", k + 1, f.highlight_n);
        }
        cn_beats_frame(&b, drop->start_ms + drop->dur_ms - 1, &f);
        CHECK(f.dice_n[drop->seat] == 5, "the die is still there until the drop ends");
        cn_beats_frame(&b, drop->start_ms + drop->dur_ms, &f);
        CHECK(f.dice_n[drop->seat] == 4, "and gone at its end");
    }
    CHECK(cn_beats_build(EV, 0, &start, &b) == 0 && b.start.done, "no events, no motion");
}

int main(void)
{
    test_plan_shape();
    test_view_mask();
    test_view_menu_reveal();
    test_beats();
    return report("cn_plan_test");
}

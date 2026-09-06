// The post-game analyser on a REAL recorded game (docs/POST_GAME_ANALYSER.md).
//
// The game is the one the hand analysis was done on: Alex (seat 0) v Eva
// (seat 1), diamonds trump, Eva the fool after 60 actions. It is kept here as
// its DEAL and its MOVES - the source data - and encoded to a v6 code at test
// time through the real encoder, because a frozen code is a fixture that rots
// with the format (house rule) while a deal and a move list do not.
//
// Built at the iOS/production caps (make analyse-test), never at the arena
// caps: the analyser enumerates every legal move, and the arena caps drop
// moves silently.
//
// What is proven here and how each test was MUTATION-CHECKED (broken on
// purpose, seen to fail, put back):
//
//   fixture     the code rebuilds to the recorded game (fool, action count,
//               Eva's 30 decisions). Mutation: swap two cards of the deal -> the
//               encoder rejects the moves (ENOTINMENU) and the test fails.
//   belief      at every decision of BOTH seats: conservation holds, every
//               real hidden card is in the pool, every pinned card is really in
//               that hand. Mutation: drop the pinned-remove on ATTACK/COVER/PASS
//               -> "pinned card not in hand" fails at the first re-played pickup.
//   worlds      every installed world is a legal 36-card board. Mutation: skip
//               the last free slot -> the duplicate/missing check fails.
//   proof       at the cover of the last eight (step 50) the jack cover is a
//               proven loss in every world and the king cover a proven win in
//               at least one of the five the belief admits; two steps later (step 52) every candidate is a
//               proven loss and the node is LOST. These are the brief's
//               ground-truth findings, reproduced by the analyser's exact play,
//               not by its playouts. Mutation: flip the sign an_exact_rec
//               returns for a finished board -> both fail.
//   determinism the same parameters produce the same bytes twice, and a
//               different seed produces different sampled results. Mutation:
//               seed the world shuffle from an uninitialised value -> fails.
//   reader      the bytes read back to the node count the header claims, and
//               every truncation of the buffer is refused with ETRUNC rather
//               than read as a shorter analysis. Mutation: remove the bounds
//               check in rd16 -> a truncated node reads as zeros and the test
//               fails.
//   threads     one thread and four produce the same bytes. Mutation: make
//               bot_drive's menu scratch a plain static -> differs or crashes.
//   deep        the K largest losses carry a deep block and the reader walks
//               it. Mutation: drop ANALYSE_NF_DEEP from the writer -> fails.
//   cost        one playout per engine on a mid-game node is TIMED and printed;
//               nothing asserts on it, it is the number the handoff asked for.
//
// Usage: analyse_test            run the suite (times handwritten and robusta)
//        analyse_test --cost     the suite, then time every engine (minutes)
//        analyse_test --code     print the game's base32 code (for cnitro_analyse)

#include "../src/analyse.h"
#include "../src/bot_roster.h"
#include "../src/replay.h"
#include "../src/replay_steps.h"
#include "../src/strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int n_pass = 0, n_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// ---------- the recorded game ---------------------------------------------------
// Cards as "suit,value" (S=0 H=1 C=2 D=3; 6..A = 5..13), the format the hand
// analysis worked from.

static const char *DEAL_P0  = "1,8 1,10 1,12 1,13 2,13 3,12";
static const char *DEAL_P1  = "0,5 0,10 0,11 1,5 2,6 2,12";
static const char *DEAL_FLIP = "3,10";
static const char *DEAL_DECK = "3,13 2,5 0,6 1,6 3,7 3,8 2,11 1,7 2,8 3,9 0,9 0,12 3,11 2,7 0,13 0,8 3,6 3,5 2,9 1,9 0,7 2,10 1,11";

static const char *MOVES[] = {
    "attack 0 1,8", "pickup 1", "attack 0 1,10", "pickup 1", "attack 0 2,5",
    "cover 1 2,6:2,5", "good 0", "attack 1 1,5", "cover 0 1,12:1,5", "good 1",
    "attack 0 0,6", "cover 1 0,10:0,6", "attack 0 1,6", "cover 1 1,10:1,6", "good 0",
    "attack 1 0,5", "pickup 0", "attack 1 1,8 2,8", "cover 0 1,13:1,8", "cover 0 2,13:2,8",
    "good 1", "attack 0 0,5", "cover 1 0,9:0,5", "good 0", "attack 1 1,7 2,7",
    "pass 0 3,7", "cover 1 2,11:2,7", "pickup 1", "attack 0 0,12", "pickup 1",
    "attack 0 0,13", "cover 1 3,7:0,13", "good 0", "attack 1 1,7 2,7", "pickup 0",
    "attack 1 0,11 2,11", "pickup 0", "attack 1 2,9 1,9", "cover 0 2,11:2,9", "cover 0 3,6:1,9",
    "good 1", "attack 0 1,7", "pickup 1", "attack 0 0,8", "cover 1 0,12:0,8",
    "good 0", "attack 1 0,7 1,7", "pass 0 2,7", "cover 1 3,9:0,7", "cover 1 3,5:1,7",
    "cover 1 2,10:2,7", "good 0", "attack 1 2,12", "cover 0 3,8:2,12", "good 1",
    "attack 0 3,12", "pickup 1", "attack 0 3,13", "pickup 1", "attack 0 3,11 0,11 1,11",
};
static const int N_MOVES = (int)(sizeof MOVES / sizeof MOVES[0]);
// Eva's decisions, as the hand analysis numbered them (0-based move index).
static const int EVA_STEPS[] = { 1, 3, 5, 7, 9, 11, 13, 15, 17, 20, 22, 24, 26, 27, 29,
                                 31, 33, 35, 37, 40, 42, 44, 46, 48, 49, 50, 52, 54, 56, 58 };
static const int N_EVA = (int)(sizeof EVA_STEPS / sizeof EVA_STEPS[0]);

static int parse_cards(const char *s, unsigned char *ids, int cap) {
    int n = 0, su, va, used;
    while (n < cap && sscanf(s, "%d,%d%n", &su, &va, &used) == 2) {
        ids[n++] = (unsigned char)(su * 13 + (va - 1));
        s += used;
        while (*s == ' ') s++;
    }
    return n;
}

// The v6 encode input (replay.h): n, trump, first attacker, counts, reveals
// (seat-major deal, then the stock in draw order, never the flip), actions.
static int build_input(unsigned char *out, int cap) {
    unsigned char p0[6], p1[6], flip[1], deck[40];
    int n0 = parse_cards(DEAL_P0, p0, 6), n1 = parse_cards(DEAL_P1, p1, 6);
    int nf = parse_cards(DEAL_FLIP, flip, 1), nd = parse_cards(DEAL_DECK, deck, 40);
    if (n0 != 6 || n1 != 6 || nf != 1 || nd != 23) return -1;
    int q = 7;
    for (int i = 0; i < 6; i++) out[q++] = p0[i];
    for (int i = 0; i < 6; i++) out[q++] = p1[i];
    for (int i = 0; i < nd; i++) out[q++] = deck[i];
    int n_actions = 0;
    for (int i = 0; i < N_MOVES; i++) {
        char type[12]; int seat, used;
        if (sscanf(MOVES[i], "%11s %d%n", type, &seat, &used) != 2) return -1;
        const char *rest = MOVES[i] + used;
        while (*rest == ' ') rest++;
        if (q + 8 + 2 * 8 > cap) return -1;
        if (!strcmp(type, "good")) {
            // Heads-up, every good closes the bout: the codec's ROUND_END.
            out[q++] = REPLAY_ROUND_END; out[q++] = 0xFF; out[q++] = 0;
        } else if (!strcmp(type, "pickup")) {
            out[q++] = LOG_PICKUP; out[q++] = (unsigned char)seat; out[q++] = 0;
        } else if (!strcmp(type, "cover")) {
            int cs, cv, ts, tv;
            if (sscanf(rest, "%d,%d:%d,%d", &cs, &cv, &ts, &tv) != 4) return -1;
            out[q++] = LOG_COVER; out[q++] = (unsigned char)seat; out[q++] = 1;
            out[q++] = (unsigned char)(cs * 13 + cv - 1);
            out[q++] = (unsigned char)(ts * 13 + tv - 1);
        } else {
            unsigned char ids[8];
            int n = parse_cards(rest, ids, 8);
            if (n <= 0) return -1;
            out[q++] = (unsigned char)(!strcmp(type, "attack") ? LOG_ATTACK : LOG_PASS);
            out[q++] = (unsigned char)seat; out[q++] = (unsigned char)n;
            for (int k = 0; k < n; k++) { out[q++] = ids[k]; out[q++] = (unsigned char)REPLAY_CARD_NONE; }
        }
        n_actions++;
    }
    out[0] = 2;
    out[1] = flip[0];
    out[2] = 0;                           // Alex opened
    out[3] = (unsigned char)(n_actions & 0xff); out[4] = (unsigned char)(n_actions >> 8);
    int nr = 12 + nd;
    out[5] = (unsigned char)(nr & 0xff); out[6] = (unsigned char)(nr >> 8);
    return q;
}

static unsigned char g_code[4096];
static int g_code_len;

static int make_code(void) {
    static unsigned char in[8192];
    int in_len = build_input(in, (int)sizeof in);
    if (in_len < 0) return -1;
    g_code_len = replay_encode_v6(in, in_len, g_code, (int)sizeof g_code);
    return g_code_len;
}

// ---------- a walker for the tests -------------------------------------------------
// The same rebuild the analyser does, in the open: the deal, then every action,
// with a callback at each decision so a test can ask questions of the board.

static ReplayAction g_acts[REPLAY_MAX_ACTIONS];
static Game g_game;

typedef void (*Visit)(const Game *g, int step, int seat, void *u);

static int walk(Visit visit, void *u) {
    ReplayHeader hdr; Card deck[MAX_DECK]; int n_deck = 0, n_acts = 0;
    int r = replay_deal_v6(g_code, g_code_len, &hdr, deck, MAX_DECK, &n_deck,
                           g_acts, REPLAY_MAX_ACTIONS, &n_acts);
    if (r < 0) return r;
    r = replay_deal_start(&g_game, &hdr, deck, n_deck);
    if (r < 0) return r;
    for (int i = 0; i < n_acts; i++) {
        const ReplayAction *a = &g_acts[i];
        if (a->kind == REPLAY_ATOM_ROUND_END) {
            // The attackers still to declare, collected before the first good
            // (the last good runs the transition and re-seats everyone).
            int who[MAX_PLAYERS], n_who = 0;
            for (int s = 0; s < g_game.num_players; s++)
                if (s != g_game.defender && should_bot_act(&g_game, s)) who[n_who++] = s;
            for (int k = 0; k < n_who; k++) {
                if (visit) visit(&g_game, i, who[k], u);
                handle_good(&g_game, who[k]);
            }
            if (g_game.num_battles > 0) engine_run_round_transition(&g_game);
            continue;
        }
        if (visit) visit(&g_game, i, a->seat, u);
        replay_action_apply(&g_game, a);
    }
    return n_acts;
}

// ---------- tests -----------------------------------------------------------------------

static void count_visit(const Game *g, int step, int seat, void *u) {
    (void)g; (void)step;
    int *counts = (int *)u;
    counts[seat]++;
}

static void test_fixture(void) {
    CHECK(g_code_len > 0, "the recorded game encodes to a v6 code");
    int counts[MAX_PLAYERS] = { 0 };
    int n = walk(count_visit, counts);
    CHECK(n == N_MOVES, "the code carries one atom per recorded move");
    CHECK(game_done(&g_game) == 1, "the rebuilt game ends with Eva (seat 1) the fool");
    CHECK(counts[1] == N_EVA, "Eva made exactly the 30 decisions the hand analysis numbered");
    CHECK(g_game.power_suit == SUIT_DIAMONDS, "diamonds are trump");
}

typedef struct { int nodes, bad_conservation, hidden_missing, pinned_wrong, free_wrong; } BeliefStats;

static void belief_visit(const Game *g, int step, int seat, void *u) {
    (void)step;
    BeliefStats *st = (BeliefStats *)u;
    // Every seat's belief at every decision, not only the actor's.
    for (int v = 0; v < g->num_players; v++) {
        AnalyseBelief B;
        analyse_belief(g, v, &B);
        st->nodes++;
        if (!B.ok) { st->bad_conservation++; continue; }
        for (int p = 0; p < g->num_players; p++) {
            if (p == v || g->players[p].status != PLAYER_STATUS_IN) continue;
            int unknown = 0;
            for (int j = 0; j < g->players[p].hand_count; j++) {
                Card c = g->players[p].hand[j];
                bool pinned = false, pooled = false;
                for (int k = 0; k < B.pinned_n[p]; k++) if (card_eq(B.pinned[p][k], c)) pinned = true;
                for (int k = 0; k < B.n; k++) if (card_eq(B.pool[k], c)) pooled = true;
                if (!pinned && !pooled) st->hidden_missing++;
                if (!pinned) unknown++;
            }
            for (int k = 0; k < B.pinned_n[p]; k++) {
                bool in_hand = false;
                for (int j = 0; j < g->players[p].hand_count; j++)
                    if (card_eq(g->players[p].hand[j], B.pinned[p][k])) in_hand = true;
                if (!in_hand) st->pinned_wrong++;
            }
            if (unknown != B.free_n[p]) st->free_wrong++;
        }
        for (int i = 0; i < g->deck_count; i++) {
            bool pooled = false;
            for (int k = 0; k < B.n; k++) if (card_eq(B.pool[k], g->deck[i])) pooled = true;
            if (!pooled) st->hidden_missing++;
        }
    }
    (void)seat;
}

static void test_belief(void) {
    BeliefStats st; memset(&st, 0, sizeof st);
    walk(belief_visit, &st);
    CHECK(st.nodes == 2 * N_MOVES, "a belief was built for both seats at every decision");
    CHECK(st.bad_conservation == 0, "conservation |U| == d + sum(f_p) held at every decision");
    CHECK(st.hidden_missing == 0, "every card really hidden from the seat is in its pool");
    CHECK(st.pinned_wrong == 0, "every pinned card is really in that hand");
    CHECK(st.free_wrong == 0, "the free-slot count is exactly the unpinned part of the hand");
}

typedef struct { int worlds, bad; } WorldStats;

static void world_visit(const Game *g, int step, int seat, void *u) {
    WorldStats *st = (WorldStats *)u;
    AnalyseBelief B;
    analyse_belief(g, seat, &B);
    if (!B.ok) { st->bad++; return; }
    // Three worlds per decision: the pool as is, reversed, and rotated.
    for (int variant = 0; variant < 3; variant++) {
        Card perm[MAX_DECK];
        for (int i = 0; i < B.n; i++)
            perm[i] = variant == 0 ? B.pool[i] : variant == 1 ? B.pool[B.n - 1 - i] : B.pool[(i + 7) % (B.n ? B.n : 1)];
        static Game w;
        game_clone(&w, g);
        analyse_install_world(&w, seat, &B, perm);
        st->worlds++;
        unsigned long long seen = 0; int total = 0, dup = 0;
#define SEE(c) do { int id = card_to_id(c); if (seen & (1ull << id)) dup++; seen |= 1ull << id; total++; } while (0)
        for (int p = 0; p < w.num_players; p++)
            for (int j = 0; j < w.players[p].hand_count; j++) SEE(w.players[p].hand[j]);
        for (int i = 0; i < w.deck_count; i++) SEE(w.deck[i]);
        for (int b = 0; b < w.num_battles; b++) {
            SEE(w.table_battles[b].attack);
            if (!card_is_none(w.table_battles[b].defense)) SEE(w.table_battles[b].defense);
        }
        if (w.has_flipped) SEE(w.flipped);
#undef SEE
        total += w.discard_pile_length;
        if (dup || total != 36 || !w.deterministic_deck || w.deck_count != g->deck_count) st->bad++;
        // Own hand untouched, every opponent hand the same size as before.
        if (memcmp(w.players[seat].hand, g->players[seat].hand, (size_t)g->players[seat].hand_count) != 0
            || w.players[seat].hand_count != g->players[seat].hand_count) st->bad++;
        for (int p = 0; p < w.num_players; p++)
            if (w.players[p].hand_count != g->players[p].hand_count) st->bad++;
    }
    (void)step;
}

static void test_worlds(void) {
    WorldStats st = { 0, 0 };
    walk(world_visit, &st);
    CHECK(st.worlds == 3 * N_MOVES, "three worlds were installed at every decision");
    CHECK(st.bad == 0, "every installed world is a legal 36-card board with the seat's own hand untouched");
}

static unsigned char g_out[1 << 20], g_out2[1 << 20];

static const AnalyseCand *find_cand(const AnalyseNode *n, int type, int card_id, int target_id) {
    for (int c = 0; c < n->n_cands; c++) {
        const AnalyseCand *cd = &n->cands[c];
        if (cd->type != type || cd->n_cards != 1) continue;
        if (cd->cards[0] != card_id) continue;
        if (type == MOVE_COVER && cd->targets[0] != target_id) continue;
        return cd;
    }
    return 0;
}

// Read every node into `nodes` (caller storage); returns the count or -1.
static int read_all(const unsigned char *buf, int len, AnalyseHeader *h, AnalyseNode *nodes, int cap) {
    int q = analyse_read_header(buf, len, h);
    if (q < 0) return -1;
    for (int i = 0; i < h->n_nodes; i++) {
        if (i >= cap) return -1;
        int r = analyse_read_node(buf + q, len - q, &nodes[i]);
        if (r < 0) return -1;
        q += r;
    }
    return q == len ? h->n_nodes : -1;
}

static AnalyseNode g_nodes[256];

static void test_proof_and_reader(void) {
    AnalyseParams p;
    analyse_params_default(&p);
    p.seat = 1;
    p.roster_idx = bot_roster_find("handwritten");
    p.worlds = 8;
    p.exhaustive_cap = 64;
    p.solve_budget = 400000L;
    int n = analyse_packed(g_code, g_code_len, &p, g_out, (int)sizeof g_out);
    CHECK(n > 0, "the recorded game analyses");
    if (n <= 0) { fprintf(stderr, "  analyse_packed = %d (replay %d)\n", n, analyse_last_replay_error()); return; }

    AnalyseHeader h;
    int nn = read_all(g_out, n, &h, g_nodes, 256);
    CHECK(nn == N_EVA, "the bytes read back to exactly Eva's 30 decisions, and to the last byte");
    CHECK(h.fool == 1 && h.n_players == 2 && h.seat == 1, "the header says whose game this was");
    CHECK(h.trump_suit == SUIT_DIAMONDS, "and what was trump");
    CHECK(h.deal[1].opening_trumps == 0, "Eva was dealt no trump");
    CHECK(h.deal[0].opening_trumps == 1, "Alex was dealt one");
    CHECK(h.deal[0].trumps_seen == 6, "Alex drew six of the nine diamonds over the game (deal + draws)");
    // P(0 trumps in 6 from 36 with 9) = C(27,6)/C(36,6) = 296010/1947792 = 0.15197
    CHECK(h.deal[1].p_exact >= 1519 && h.deal[1].p_exact <= 1521, "the no-trump deal's probability is exact");
    if (nn != N_EVA) return;

    for (int i = 0; i < nn; i++) CHECK(g_nodes[i].step == EVA_STEPS[i], "the nodes are Eva's decisions in order");

    // Step 50: the cover of the last eight. Deck = 1, so every world is a
    // perfect-information game and the exact play resolves it.
    const AnalyseNode *n50 = 0, *n52 = 0;
    for (int i = 0; i < nn; i++) { if (g_nodes[i].step == 50) n50 = &g_nodes[i]; if (g_nodes[i].step == 52) n52 = &g_nodes[i]; }
    CHECK(n50 && n52, "the two endgame nodes are present");
    if (!n50 || !n52) return;
    CHECK(n50->deck == 1 && n50->unknown == 5, "at step 50 one card is in the stock and five are unlocated (the queen of spades is pinned)");
    CHECK((n50->flags & ANALYSE_NF_EXHAUSTIVE) && !(n50->flags & ANALYSE_NF_FUTURES), "step 50 enumerated every world, with a fixed stock order");
    CHECK(n50->n_worlds == 5, "five worlds: which unlocated card is the last of the stock");
    const AnalyseCand *jc = find_cand(n50, MOVE_COVER, 2 * 13 + 9, 2 * 13 + 6);   // JC over 8C
    const AnalyseCand *kc = find_cand(n50, MOVE_COVER, 2 * 13 + 11, 2 * 13 + 6);  // KC over 8C
    CHECK(jc && kc, "both covers of the eight are candidates");
    if (jc && kc) {
        CHECK(jc->proof == ANALYSE_P_LOSS && jc->proven_losses == 5, "the jack cover is a proven loss in every world");
        CHECK(kc->proven_wins >= 1 && kc->proven_wins + kc->proven_losses == 5,
              "the king cover is proven in every world and wins at least one");
        CHECK(kc->mean_fp < jc->mean_fp, "so the king cover scores better than the jack");
        CHECK(&n50->cands[n50->played] == jc, "Eva played the jack");
        CHECK(n50->verdict == ANALYSE_V_CHANCE || n50->verdict == ANALYSE_V_DECISIVE, "and it is called a mistake");
        CHECK(n50->flags & ANALYSE_NF_PROOF, "with every (candidate, world) resolved exactly: a proof");
    }
    CHECK(n52->verdict == ANALYSE_V_LOST, "two steps later nothing could be done: LOST");
    CHECK(n52->flags & ANALYSE_NF_PROOF, "and that is a proof too (deck empty, both hands known)");
    for (int c = 0; c < n52->n_cands; c++) CHECK(n52->cands[c].proof == ANALYSE_P_LOSS, "every candidate at step 52 is a proven loss");
    // Nodes 54/56/58 are forced pickups; the last open decision is 52 (LOST),
    // so the decisive moment is the last mistake before it.
    CHECK(h.decisive_node != 0xFFFF && g_nodes[h.decisive_node].verdict == ANALYSE_V_DECISIVE,
          "the analysis names a decisive moment");
    CHECK(h.decisive_node != 0xFFFF && g_nodes[h.decisive_node].step == 50,
          "and it is the cover of the last eight");

    // Determinism, and that the seed matters.
    int n2 = analyse_packed(g_code, g_code_len, &p, g_out2, (int)sizeof g_out2);
    CHECK(n2 == n, "the same parameters produce the same size");
    // The header carries elapsed_ms, which legitimately differs; compare past it.
    int hdr = analyse_read_header(g_out, n, &h);
    CHECK(n2 == n && memcmp(g_out + hdr, g_out2 + hdr, (size_t)(n - hdr)) == 0, "and the same bytes");
    p.seed = 77;
    int n3 = analyse_packed(g_code, g_code_len, &p, g_out2, (int)sizeof g_out2);
    CHECK(n3 > 0 && (n3 != n || memcmp(g_out + hdr, g_out2 + hdr, (size_t)(n - hdr)) != 0),
          "a different seed samples different worlds");

    // Every truncation is refused.
    int refused = 0, accepted = 0;
    for (int cut = 0; cut < n; cut++) {
        AnalyseHeader th;
        int q = analyse_read_header(g_out, cut, &th);
        if (q < 0) { refused++; continue; }
        int ok = 1;
        for (int i = 0; i < th.n_nodes; i++) {
            int r = analyse_read_node(g_out + q, cut - q, &g_nodes[0]);
            if (r < 0) { ok = 0; break; }
            q += r;
        }
        if (ok) accepted++; else refused++;
    }
    CHECK(accepted == 0 && refused == n, "every truncation of the bytes is refused, none reads as a shorter analysis");
    CHECK(analyse_read_header(g_out, n, &h) > 0, "the untruncated bytes still read");
}

// The world loop across threads gives the same bytes as one thread: every
// cell is a function of its (world, candidate) and nothing else, and every bot
// keeps its scratch per thread. Mutation: make bot_drive's menu scratch a plain
// static again -> the bytes differ (or the run crashes) at 4 threads.
static void test_threads_agree(void) {
    AnalyseParams p;
    analyse_params_default(&p);
    p.seat = 0;
    p.roster_idx = bot_roster_find("robusta");   // a real MC bot with scratch of its own
    p.worlds = 12;
    p.exhaustive_cap = 0;
    p.solve_budget = 50000L;
    p.threads = 1;
    int n1 = analyse_packed(g_code, g_code_len, &p, g_out, (int)sizeof g_out);
    p.threads = 4;
    int n4 = analyse_packed(g_code, g_code_len, &p, g_out2, (int)sizeof g_out2);
    CHECK(n1 > 0 && n4 == n1, "one thread and four produce the same size");
    AnalyseHeader h;
    int hdr = analyse_read_header(g_out, n1, &h);
    CHECK(hdr > 0 && n4 == n1 && memcmp(g_out + hdr, g_out2 + hdr, (size_t)(n1 - hdr)) == 0,
          "and the same bytes past the cost line");
}

// The deep pass: the K largest scan losses re-evaluated by a second engine,
// carried in the node's deep block and owning the verdict. Mutation: skip
// writing the deep block -> the reader cannot get to the last byte and the
// count check fails; drop the ANALYSE_NF_DEEP flag -> the block is read as the
// next node and the same check fails.
static void test_deep_pass(void) {
    AnalyseParams p;
    analyse_params_default(&p);
    p.seat = 1;
    p.roster_idx = bot_roster_find("handwritten");
    p.worlds = 16;
    p.exhaustive_cap = 0;
    p.solve_budget = 0;
    p.deep_roster_idx = bot_roster_find("robusta");
    p.deep_nodes = 2;
    p.deep_worlds = 8;
    int n = analyse_packed(g_code, g_code_len, &p, g_out, (int)sizeof g_out);
    CHECK(n > 0, "the deep pass analyses");
    if (n <= 0) return;
    AnalyseHeader h;
    int nn = read_all(g_out, n, &h, g_nodes, 256);
    CHECK(nn == N_EVA, "the bytes with deep blocks read back to the last byte");
    CHECK(h.deep_roster_idx == p.deep_roster_idx, "the header names the deep engine");
    int deep = 0, agree_flag_ok = 1;
    for (int i = 0; i < nn; i++) {
        const AnalyseNode *nd = &g_nodes[i];
        if (!(nd->flags & ANALYSE_NF_DEEP)) continue;
        deep++;
        if (nd->deep_n_worlds != 8) agree_flag_ok = 0;
        for (int c = 0; c < nd->n_cands; c++) if (nd->cands[c].deep_n != 8) agree_flag_ok = 0;
        int agrees = (nd->deep_best == nd->best);
        if (agrees != ((nd->flags & ANALYSE_NF_DEEP_AGREES) != 0)) agree_flag_ok = 0;
    }
    CHECK(deep == 2, "exactly the two largest losses got the deep pass");
    CHECK(agree_flag_ok, "every deep block has its 8 worlds and says whether it agreed with the scan");
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// The handoff's first instruction: measure one playout per engine before
// anything else. Done through the analyser itself on one mid-game node, 4
// worlds, so the number is a real (candidate, world) playout and nothing else.
static void measure_cost(int all) {
    const char *engines[] = { "handwritten", "robusta", "blackpowder", "cordite", "octogen" };
    int n_engines = all ? (int)(sizeof engines / sizeof engines[0]) : 2;
    printf("\ncost per playout (the game cut after 25 actions, Eva's decisions, 4 worlds each, one thread)%s:\n",
           all ? "" : " - pass --cost for every engine");
    for (int e = 0; e < n_engines; e++) {
        AnalyseParams p;
        analyse_params_default(&p);
        p.seat = 1;
        p.roster_idx = bot_roster_find(engines[e]);
        p.worlds = 4;
        p.exhaustive_cap = 0;
        p.solve_budget = 0;
        p.threads = 1;
        // One node only: cap the walk by analysing a code cut after step 25.
        static unsigned char in[8192], cut_code[4096];
        int in_len = build_input(in, (int)sizeof in);
        in[3] = 25; in[4] = 0;                                   // n_actions = 25
        int cut_len = replay_encode_v6(in, in_len, cut_code, (int)sizeof cut_code);
        if (cut_len <= 0) { printf("  %-12s (cut failed %d)\n", engines[e], cut_len); continue; }
        double t0 = now_s();
        int n = analyse_packed(cut_code, cut_len, &p, g_out, (int)sizeof g_out);
        double dt = now_s() - t0;
        if (n <= 0) { printf("  %-12s failed %d\n", engines[e], n); continue; }
        AnalyseHeader h; analyse_read_header(g_out, n, &h);
        printf("  %-12s %6u playouts in %7.0f ms  = %8.2f ms per playout\n",
               engines[e], h.n_playouts, dt * 1000.0, h.n_playouts ? dt * 1000.0 / h.n_playouts : 0.0);
    }
}

int main(int argc, char **argv) {
    if (make_code() <= 0) { fprintf(stderr, "the fixture does not encode (%d)\n", g_code_len); return 1; }
    if (argc > 1 && !strcmp(argv[1], "--code")) {
        static char b32[8192];
        replay_b32_encode(g_code, g_code_len, b32, (int)sizeof b32);
        printf("%s\n", b32);
        return 0;
    }
    test_fixture();
    test_belief();
    test_worlds();
    test_proof_and_reader();
    test_threads_agree();
    test_deep_pass();
    measure_cost(argc > 1 && !strcmp(argv[1], "--cost"));
    printf("\nanalyse_test: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail > 0 ? 1 : 0;
}

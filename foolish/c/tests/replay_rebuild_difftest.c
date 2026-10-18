// A v6 replay is the game REBUILT: replay_steps_v6 deals the code's deck and
// drives the real handlers, so the board it finishes on must be the board the
// engine finished on - every field state_put writes, hands in array order.
//
// This plays random games at 2..8 seats (random and handwritten bots, a random
// seat order each turn, classic and podkidnoy rules), encodes each with
// replay_encode_v6_from_game, rebuilds it, and compares the two final boards
// byte for byte. About one action in sixteen it also cuts the game in progress
// (the iMessage path) and holds the rebuilt board against the live one. A
// mismatch is counted by which part of the board differs.
//
// Two divergences this found on main (about 2% of finished games, 58% of
// mid-game cuts): a dealt hand kept its draw order while a code records it as
// a set (game.c deal_initial now holds it ascending), and a good the next move
// withdrew left the seat's awaiting_attack cleared, though the codec drops such
// a good as dead state (handle_good no longer writes the flag; game.h
// game_seat_awaiting reads the mask).
//
// Usage: replay_rebuild_difftest [games] [seed0]   (defaults 10000, 1)
//   RRD_VERBOSE=1  one line per divergence
//   RRD_TRACE=k    every board game k passed through, live then rebuilt
// `make tests` runs a short pass; `make difftests` the long one.

#include "../src/game.h"
#include "../src/legal.h"
#include "../src/strategy.h"
#include "../src/bot_play.h"
#include "../src/replay.h"
#include "../src/replay_steps.h"
#include "../src/view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../shared/c/rng.h"

#define ENC_CAP (1 << 20)

static unsigned char g_enc[ENC_CAP];
static uint64_t g_rng = 1;
static void no_sink(void *ctx, const EvwEvent *ev) { (void)ctx; (void)ev; }

// RRD_TRACE=k: the boards game k passed through, live and rebuilt.
#define TR_MAX 4096
static int g_trace = 0;
static Game g_live[TR_MAX];
static char g_live_what[TR_MAX][64];
static int g_n_live;
static void tr_live(const Game *g, const char *what) {
    if (!g_trace || g_n_live >= TR_MAX) return;
    g_live[g_n_live] = *g;
    snprintf(g_live_what[g_n_live], 64, "%s", what);
    g_n_live++;
}

static int mid_checks = 0, mid_diverged = 0;
static const char *classify(const Game *a, const Game *b);
static bool same_multiset(const Card *a, const Card *b, int n);

// THE IMESSAGE PATH: a game still in progress is cut into a code (every log so
// far, a pending good included) and the next turn plays on from the board the
// code rebuilds. That board must be the one the game is really on.
static void check_mid_game(const Game *g, const unsigned char *seed32) {
    static unsigned char enc[ENC_CAP], want[RS_BOARD_MAX], got[RS_BOARD_MAX];
    if (g->num_logs >= MAX_LOGS) return;
    mid_checks++;
    const int n = replay_encode_v6_from_game(g, seed32, FOOLISH_SEED_LEN, 1 << 30, enc, ENC_CAP);
    const int r = n > 0 ? replay_steps_v6(enc, n, VIEW_UNMASKED, 0, no_sink, 0) : n;
    const Game *back = r == REPLAY_EOK ? replay_steps_last_game() : 0;
    // The stock's ORDER is the one thing a cut does not carry, by design: the
    // undrawn cards are nobody's knowledge, so a code holds the draws so far
    // and the rebuilt stock is the rest in canonical order (replay_steps.c
    // rs_build_deck). Its contents still have to be the live stock's.
    static Game live;
    live = *g;
    if (back && back->deck_count == live.deck_count
        && same_multiset(live.deck, back->deck, live.deck_count))
        memcpy(live.deck, back->deck, sizeof live.deck);
    const int wl = state_put(&live, VIEW_UNMASKED, want);
    const int gl = back ? state_put(back, VIEW_UNMASKED, got) : -1;
    if (back && gl == wl && !memcmp(got, want, (size_t)wl)) return;
    mid_diverged++;
    if (getenv("RRD_VERBOSE"))
        printf("mid-game diverge at log %d (enc=%d r=%d): %s\n", g->num_logs, n, r,
               back ? classify(&live, back) : "no rebuild");
}

static bool play(Game *g, int np, bool handwritten, int8_t rules, uint64_t seed,
                 unsigned char seed32[FOOLISH_SEED_LEN]) {
    g_rng = seed;
    for (int i = 0; i < FOOLISH_SEED_LEN; i++) seed32[i] = (unsigned char)rng_u32(&g_rng);
    memset(g, 0, sizeof *g);
    g->num_players = (int8_t)np;
    g->rules = rules;
    for (int i = 0; i < np; i++) g->players[i].status = PLAYER_STATUS_READY;
    game_set_seed((uint32_t)seed);
    game_set_deal_seed_bytes(seed32, FOOLISH_SEED_LEN);
    random_strategy_set_seed((uint32_t)(seed >> 32) ^ 0x9e3779b9u);
    start_game(g);
    g_n_live = 0;
    tr_live(g, "deal");

    static BotPlay bp;
    bot_play_init(&bp, g, 100000, BOT_PLAY_SHUFFLE, &g_rng);
    for (int pi; (pi = bot_play_next(&bp)) >= 0; ) {
        const int mi = handwritten ? handwritten_strategy_choose(g, pi, &bp.moves, 0)
                                   : random_strategy_choose(g, pi, &bp.moves, 0);
        if (!bot_play_apply(&bp, mi)) continue;
        if (rng_u32(&g_rng) % 16 == 0) check_mid_game(g, seed32);
        const LegalMove *m = &bp.moves.moves[mi];
        char w[64];
        snprintf(w, 64, "live seat %d move %d n=%d", pi, m->type, m->n_cards);
        tr_live(g, w);
    }
    return game_done(g) >= 0;
}

static bool same_cards(const Card *a, const Card *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i].suit != b[i].suit || a[i].value != b[i].value) return false;
    return true;
}

static bool same_multiset(const Card *a, const Card *b, int n) {
    int ca[64] = {0};
    for (int i = 0; i < n; i++) ca[card_to_id(a[i]) & 63]++;
    for (int i = 0; i < n; i++) ca[card_to_id(b[i]) & 63]--;
    for (int i = 0; i < 64; i++) if (ca[i]) return false;
    return true;
}

// Which part of the board differs, most specific first. Returns a class name.
static const char *classify(const Game *a, const Game *b) {
    if (a->defender != b->defender) return "defender";
    if (a->first_attacker != b->first_attacker) return "first_attacker";
    if (a->num_battles != b->num_battles) return "num_battles";
    if (a->deck_count != b->deck_count || a->discard_pile_length != b->discard_pile_length)
        return "pile counts";
    if (a->num_eliminated != b->num_eliminated
        || memcmp(a->elimination_order, b->elimination_order, (size_t)a->num_eliminated))
        return "elimination order";
    for (int s = 0; s < a->num_players; s++) {
        const Player *p = &a->players[s], *q = &b->players[s];
        if (p->hand_count != q->hand_count) return "hand count";
        if (!same_cards(p->hand, q->hand, p->hand_count))
            return same_multiset(p->hand, q->hand, p->hand_count) ? "hand order" : "hand cards";
        if (p->status != q->status) return "player status";
        if (p->awaiting_attack != q->awaiting_attack) return "awaiting_attack";
    }
    if (a->good_players_mask != b->good_players_mask) return "good mask";
    if (a->has_good_timestamp != b->has_good_timestamp) return "good timestamp";
    if (a->has_flipped != b->has_flipped || a->power_suit != b->power_suit) return "flip";
    if (!same_cards(a->deck, b->deck, a->deck_count))
        return same_multiset(a->deck, b->deck, a->deck_count) ? "stock order" : "stock cards";
    for (int i = 0; i < a->num_battles; i++)
        if (!same_cards(&a->table_battles[i].attack, &b->table_battles[i].attack, 1)
            || !same_cards(&a->table_battles[i].defense, &b->table_battles[i].defense, 1))
            return "table";
    return "other state_put byte";
}

static void dump(const Game *g, const char *what) {
    printf("%-28s def=%d fa=%d st=%d bt=%d dk=%d |", what, g->defender, g->first_attacker,
           g->status, g->num_battles, g->deck_count);
    for (int s = 0; s < g->num_players; s++) {
        const Player *p = &g->players[s];
        printf(" %d%s%c:", s, p->awaiting_attack ? "*" : "", "?RIO"[p->status & 3]);
        for (int i = 0; i < p->hand_count; i++) printf("%d.", card_to_id(p->hand[i]));
    }
    printf("\n");
}

static void trace_rebuild(const unsigned char *code, int len) {
    static ReplayAction acts[REPLAY_MAX_ACTIONS];
    static Game rb;
    ReplayHeader hdr; Card deck[MAX_DECK]; int nd = 0, na = 0;
    if (replay_deal_v6(code, len, &hdr, deck, MAX_DECK, &nd, acts, REPLAY_MAX_ACTIONS, &na) < 0) return;
    replay_deal_start(&rb, &hdr, deck, nd);
    dump(&rb, "rb deal");
    for (int i = 0; i < na; i++) {
        replay_action_apply(&rb, &acts[i]);
        char w[64]; snprintf(w, 64, "rb seat %d atom %d n=%d", acts[i].seat, acts[i].kind, acts[i].n_cards);
        dump(&rb, w);
    }
}

int main(int argc, char **argv) {
    const int games = argc > 1 ? atoi(argv[1]) : 10000;
    const uint64_t seed0 = argc > 2 ? strtoull(argv[2], 0, 10) : 1;
    const bool verbose = getenv("RRD_VERBOSE") != 0;
    const int trace_k = getenv("RRD_TRACE") ? atoi(getenv("RRD_TRACE")) : -1;

    static Game g;
    static unsigned char want[RS_BOARD_MAX], got[RS_BOARD_MAX];
    unsigned char seed32[FOOLISH_SEED_LEN];
    int played = 0, skipped = 0, diverged = 0;
    int per_np[MAX_PLAYERS + 1] = {0}, div_np[MAX_PLAYERS + 1] = {0};
    const char *classes[16]; int class_n[16]; int n_classes = 0;

    for (int k = 0; k < games; k++) {
        if (trace_k >= 0 && k != trace_k) continue;
        g_trace = trace_k >= 0;
        const uint64_t seed = rng_seed64(seed0 * 1000003u + (uint64_t)k);
        const int np = 2 + k % 7;
        const bool hw = (k / 7) % 2;
        const int8_t rules = ((k / 14) % 4 == 3) ? GAME_RULE_NO_PASS : 0;
        if (!play(&g, np, hw, rules, seed, seed32) || g.num_logs >= MAX_LOGS) { skipped++; continue; }
        played++; per_np[np]++;

        const int want_len = state_put(&g, VIEW_UNMASKED, want);
        const int enc = replay_encode_v6_from_game(&g, seed32, FOOLISH_SEED_LEN, 1 << 30,
                                                   g_enc, ENC_CAP);
        int r = enc > 0 ? replay_steps_v6(g_enc, enc, VIEW_UNMASKED, 0, no_sink, 0) : enc;
        const Game *back = r == REPLAY_EOK ? replay_steps_last_game() : 0;
        const int got_len = back ? state_put(back, VIEW_UNMASKED, got) : -1;
        if (g_trace) {
            for (int i = 0; i < g_n_live; i++) dump(&g_live[i], g_live_what[i]);
            trace_rebuild(g_enc, enc);
        }
        const bool same = back && got_len == want_len && !memcmp(got, want, (size_t)want_len)
                          && game_done(back) == game_done(&g);
        if (same) continue;

        diverged++; div_np[np]++;
        const char *cls = !back ? "no rebuild" : classify(&g, back);
        int c = 0;
        while (c < n_classes && strcmp(classes[c], cls)) c++;
        if (c == n_classes && n_classes < 16) { classes[n_classes] = cls; class_n[n_classes++] = 0; }
        if (c < 16) class_n[c]++;
        if (verbose)
            printf("diverge game %d np=%d hw=%d rules=%d enc=%d r=%d: %s\n",
                   k, np, hw, rules, enc, r, cls);
    }

    printf("replay_rebuild_difftest: %d games played (%d skipped), %d diverged; "
           "%d mid-game cuts, %d diverged\n",
           played, skipped, diverged, mid_checks, mid_diverged);
    for (int np = 2; np <= MAX_PLAYERS; np++)
        if (per_np[np]) printf("  %d seats: %d / %d\n", np, div_np[np], per_np[np]);
    for (int c = 0; c < n_classes; c++) printf("  class %-22s %d\n", classes[c], class_n[c]);
    if (played == 0 || mid_checks == 0) { printf("FAIL: nothing checked\n"); return 1; }
    return (diverged || mid_diverged) ? 1 : 0;
}

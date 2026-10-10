// The Oracle's answer (src/oracle_answer.h OgAnswer), read natively against the
// game it was deliberated on.
//
// e2e/oracle_answer.test.ts holds the struct to what the old JSON line said,
// through the wasm module and the generated reader. That proves the transport
// and it is a FIXTURE: it knows thirteen decisions. This file is the other half.
// It plays whole games with octogen in every seat, and after every choose holds
// the answer to things it can check without trusting the code that filled it:
// the real Game (the hand, the table, the counts - and the true hands, which a
// pinned card must really be in), the legal-move menu (every row is one of its
// moves, each at most once, and the chosen row is the move octogen returned),
// libc's snprintf (the row labels are written by a hand-rolled text cursor;
// here they are formatted again with the format strings the JSON emitter used),
// and the answer's own arithmetic (a storyline cannot be told more often than
// the row was sampled).
//
// Built with the two flags an oracle build is made of (make oracle-answer-test),
// natively, so it also runs the native printer (OG_EXPLAIN, into /dev/null).
//
// What each check was MUTATION-CHECKED with is in the commit that added it.

#include "../src/oracle_answer.h"
#include "../src/octogen_strategy.h"
#include "../src/cordite_sim.h"
#include "../src/strategy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int n_pass = 0, n_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { n_pass++; } \
    else { if (n_fail++ < 40) fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// What the suite has to have met for its equalities to mean anything.
static struct {
    int decisions, rows, scored, why, multi_cover, pruned, proven, unknown;
    int pinned, voids, floors, covered, single, short_story, replies;
} seen;

// ---------- the labels, by libc ----------------------------------------------
// The JSON emitter's own format strings (octogen_strategy.c og_ex_move_label,
// before the struct), kept here as the reference the text cursor is held to.
static void ref_card(char *buf, size_t n, Card c, int trump) {
    static const char *VAL[14] = { "?","2","3","4","5","6","7","8","9","10","J","Q","K","A" };
    const char *v = (c.value >= 1 && c.value <= 13) ? VAL[c.value] : "?";
    char s = (c.suit >= 0 && c.suit < 4) ? "SHCD"[(int)c.suit] : '?';
    snprintf(buf, n, "%s%c%s", v, s, (c.suit == trump) ? "*" : "");
}
static void ref_label(char *label, size_t cap, const LegalMove *m, int trump) {
    static const char *MTYPE[6] = { "attack","cover","pass","pickup","good","wait" };
    const char *mt = (m->type >= 0 && m->type < 6) ? MTYPE[m->type] : "?";
    if (m->type == MOVE_COVER && m->n_cards >= 1) {
        int w = snprintf(label, cap, "cover");
        for (int k = 0; k < m->n_cards; k++) {
            char cc[8], tt[8];
            ref_card(cc, sizeof cc, m->cards[k], trump);
            ref_card(tt, sizeof tt, m->attack_cards[k], trump);
            w += snprintf(label + w, cap - (size_t)w, "%s%s->%s", k ? "," : " ", cc, tt);
        }
    } else if (m->type == MOVE_PICKUP) {
        snprintf(label, cap, "pickup");
    } else if (m->type == MOVE_GOOD) {
        snprintf(label, cap, "good");
    } else {
        char cl[160]; cl[0] = 0; int cw = 0;
        for (int c = 0; c < m->n_cards; c++) {
            char cb[8]; ref_card(cb, sizeof cb, m->cards[c], trump);
            cw += snprintf(cl + cw, sizeof(cl) - (size_t)cw, "%s%s", c ? " " : "", cb);
        }
        snprintf(label, cap, "%s %s", mt, cl);
    }
}

static bool row_is_move(const OgAnswerRow *r, const LegalMove *m) {
    if (r->type != (uint8_t)m->type || r->n_cards != m->n_cards) return false;
    for (int i = 0; i < m->n_cards; i++) {
        if (!card_eq(r->cards[i], m->cards[i])) return false;
        if (m->type == MOVE_COVER && !card_eq(r->targets[i], m->attack_cards[i])) return false;
    }
    return true;
}
static bool hand_has(const Player *pl, Card c) {
    for (int i = 0; i < pl->hand_count; i++) if (card_eq(pl->hand[i], c)) return true;
    return false;
}

// ---------- one answer, against the game it is of ----------------------------
static void check_answer(const Game *g, int seat, const LegalMoves *moves, int chosen) {
    const OgAnswer *a = og_answer();
    CHECK(a != NULL, "a choose with a legal move answers");
    if (!a) return;
    seen.decisions++;

    CHECK(a->seat == seat && a->defender == g->defender && a->trump == g->power_suit, "seat, defender, trump");
    CHECK(a->n_players == g->num_players && a->deck == g->deck_count && a->n_logs == g->num_logs, "players, deck, logs");

    const Player *me = &g->players[seat];
    CHECK(a->n_hand == me->hand_count, "the hand's length");
    for (int i = 0; i < a->n_hand && i < me->hand_count; i++) CHECK(card_eq(a->hand[i], me->hand[i]), "the hand, card for card");
    CHECK(a->n_battles == g->num_battles, "the table's length");
    for (int b = 0; b < a->n_battles && b < g->num_battles; b++) {
        bool cov = !card_is_none(g->table_battles[b].defense);
        CHECK(card_eq(a->table[b].attack, g->table_battles[b].attack), "a battle's attack");
        CHECK(a->table[b].covered == cov, "whether a battle is covered");
        if (cov) { CHECK(card_eq(a->table[b].defense, g->table_battles[b].defense), "a battle's defense"); seen.covered++; }
    }

    // The belief. The game handed to octogen natively is the WHOLE game, so a
    // pinned card can be checked against the hand it claims to be in.
    for (int p = 0; p < g->num_players; p++) {
        CHECK(a->hand_count[p] == g->players[p].hand_count, "a seat's hand count");
        CHECK(a->pinned[p].n <= g->players[p].hand_count, "no more pins than cards");
        for (int i = 0; i < a->pinned[p].n; i++) CHECK(hand_has(&g->players[p], a->pinned[p].cards[i]), "a pinned card is really in that hand");
        CHECK(a->voids[p].n <= OG_ANSWER_MAX_VOIDS, "a seat's voids fit");
        CHECK(a->floor[p] >= 0 && a->floor[p] <= 13, "a floor is a rank or none");
        if (p == seat) CHECK(a->pinned[p].n == 0 && a->voids[p].n == 0 && a->floor[p] == 0, "octogen believes nothing about its own hand");
        if (a->pinned[p].n) seen.pinned++;
        if (a->voids[p].n) seen.voids++;
        if (a->floor[p]) seen.floors++;
    }

    // The rows: each one of the menu's moves, none twice, and the one octogen
    // returned marked as chosen.
    static bool taken[MAX_LEGAL_MOVES];
    memset(taken, 0, (size_t)moves->n * sizeof(bool));
    int n_chosen = 0, n_scored = 0;
    bool past_candidates = false;
    CHECK(a->n_rows >= 1 && a->n_rows <= OG_ANSWER_MAX_ROWS, "at least one row");
    if (a->n_rows == 1) seen.single++;
    for (int k = 0; k < a->n_rows; k++) {
        const OgAnswerRow *r = &a->rows[k];
        seen.rows++;
        int mi = -1;
        for (int i = 0; i < moves->n; i++) if (!taken[i] && row_is_move(r, &moves->moves[i])) { mi = i; break; }
        CHECK(mi >= 0, "a row is a legal move not already a row");
        if (mi < 0) continue;
        taken[mi] = true;
        const LegalMove *m = &moves->moves[mi];
        CHECK(r->n_targets == (m->type == MOVE_COVER ? m->n_cards : 0), "only a cover has targets");
        if (m->type == MOVE_COVER && m->n_cards > 1) seen.multi_cover++;

        char want[160];
        ref_label(want, sizeof want, m, g->power_suit);
        CHECK(strlen(want) < OG_ANSWER_LABEL, "the reference label fits the field");
        CHECK(r->label_len == strlen(want) && memcmp(r->label, want, r->label_len) == 0, "the label is what snprintf writes");

        if (r->chosen) { n_chosen++; CHECK(mi == chosen, "the chosen row is the move octogen returned"); }
        if (r->scored) {
            n_scored++; seen.scored++;
            CHECK(r->nsim > 0, "a scored row was sampled");
            CHECK(r->score >= 1.0 && r->score <= (double)g->num_players, "a mean finish is a finish position");
            CHECK(!r->pruned, "a scored row is a candidate");
        }
        if (r->pruned) {
            seen.pruned++; past_candidates = true;
            CHECK(a->solver_applied, "only the exact probe prunes");
            CHECK(!r->scored && !r->alive && r->nsim == 0, "a pruned row was never sampled");
            CHECK(r->verdict == OG_VERDICT_WIN || r->verdict == OG_VERDICT_LOSS || r->verdict == OG_VERDICT_DRAW, "a pruned row is proven");
        } else {
            CHECK(!past_candidates, "the pruned rows come after every candidate");
        }
        switch (r->verdict) {
            case OG_VERDICT_WIN:  CHECK(r->has_verdict_val && r->verdict_val > 0, "a win has a positive value"); seen.proven++; break;
            case OG_VERDICT_LOSS: CHECK(r->has_verdict_val && r->verdict_val < 0, "a loss has a negative value"); seen.proven++; break;
            case OG_VERDICT_DRAW: CHECK(r->has_verdict_val && r->verdict_val == 0, "a draw has a value of zero"); break;
            case OG_VERDICT_UNKNOWN: seen.unknown++; /* fall through */
            case OG_VERDICT_NONE: case OG_VERDICT_ILLEGAL: CHECK(!r->has_verdict_val, "no value without a proof"); break;
            default: CHECK(false, "a verdict is one of the six");
        }
        if (!a->solver_applied) CHECK(r->verdict == OG_VERDICT_NONE, "no verdict unless the probe ran");
    }
    CHECK(n_chosen == 1, "exactly one row is chosen");
    CHECK(a->solver_result <= OG_SOLVER_EVAL && (a->solver_applied || a->solver_result == OG_SOLVER_NONE), "the solver's result");

    // The why table: one entry per scored row, in row order, and its counts
    // cannot exceed the row's.
    CHECK(a->n_why == n_scored, "every scored row has its storylines, and only those");
    int last_row = -1;
    for (int i = 0; i < a->n_why; i++) {
        const OgAnswerWhy *w = &a->why[i];
        seen.why++;
        CHECK(w->row > last_row && w->row < a->n_rows, "the why entries are in row order");
        last_row = w->row;
        if (w->row >= a->n_rows) continue;
        CHECK(a->rows[w->row].scored && w->n == (uint32_t)a->rows[w->row].nsim, "an entry folds exactly its row's playouts");
        CHECK(w->n_paths >= 1 && w->n_paths <= OG_ANSWER_TOP_PATHS && w->n_replies <= OG_ANSWER_TOP_REPLIES, "the entry's counts");
        CHECK(w->mepk >= 0 && w->oppk >= 0 && w->metr >= 0 && w->opptr >= 0 && w->rnds >= 0, "the means are not negative");
        uint32_t told = 0, prev = UINT32_MAX;
        for (int p = 0; p < w->n_paths; p++) {
            const OgAnswerPath *s = &w->paths[p];
            CHECK(s->n >= 1 && s->n <= prev, "storylines are most told first");
            prev = s->n; told += s->n;
            CHECK(s->len <= OG_ANSWER_PATH_ROUNDS, "a storyline is at most four rounds");
            for (int j = 0; j < s->len; j++) CHECK(s->sym[j] >= 1 && s->sym[j] <= 4, "a round's outcome is one of four");
            CHECK(s->fin >= 1.0f && s->fin <= (float)g->num_players, "a storyline's finish is a finish position");
            if (s->len < OG_ANSWER_PATH_ROUNDS) seen.short_story++;
        }
        CHECK(told <= w->n, "the storylines told no more playouts than there were");
        prev = UINT32_MAX; told = 0;
        for (int r = 0; r < w->n_replies; r++) {
            CHECK(w->replies[r].n >= 1 && w->replies[r].n <= prev, "replies are most met first");
            prev = w->replies[r].n; told += w->replies[r].n;
            CHECK(w->replies[r].type <= 5 && w->replies[r].card <= 52, "a reply is a move type and a card");
            seen.replies++;
        }
        CHECK(told <= w->n, "the replies met no more playouts than there were");
    }
}

// ---------- games, octogen in every seat --------------------------------------
static void play(uint32_t seed, int n_players) {
    cd_sim_solve_reset();
    game_set_seed(seed);
    random_strategy_set_seed(seed);
    static Game g;
    memset(&g, 0, sizeof g);
    g.num_players = (int8_t)n_players;
    for (int i = 0; i < n_players; i++) g.players[i].status = PLAYER_STATUS_READY;
    start_game(&g);

    static LegalMoves moves;
    for (int iters = 0; game_done(&g) < 0 && iters < 4000; iters++) {
        bool acted = false;
        for (int pi = 0; pi < g.num_players && !acted; pi++) {
            if (!should_bot_act(&g, pi)) continue;
            calculate_legal_moves(&g, pi, &moves);
            if (moves.n == 0) continue;
            og_answer_reset();
            CHECK(og_answer() == NULL, "a reset forgets the last answer");
            int idx = octogen_strategy_choose(&g, pi, &moves, NULL);
            CHECK(idx >= 0 && idx < moves.n, "octogen chose a legal move");
            if (idx < 0 || idx >= moves.n) return;
            check_answer(&g, pi, &moves, idx);
            acted = legal_move_apply(&g, pi, &moves.moves[idx]);
        }
        if (!acted) break;
    }
    CHECK(game_done(&g) >= 0, "the game was played to its fool");
}

int main(void) {
    // A native build answers only when asked (OG_EXPLAIN), and prints each
    // answer where it is told to; /dev/null runs the printer and keeps quiet.
    setenv("OG_EXPLAIN", "/dev/null", 1);
    // The Oracle's own knobs, small: racing off, a dozen worlds a batch.
    setenv("OG_KEEP1", "26", 1); setenv("OG_KEEP2", "26", 1);
    setenv("OG_W1", "12", 1); setenv("OG_W2", "1", 1); setenv("OG_W3", "0", 1);
    setenv("OG_EXPLAIN_SOLVE_BUDGET", "2000000", 1);
    og_reload_flags();

    CHECK(og_answer() == NULL, "nothing is answered before a choose");
    for (uint32_t seed = 1; seed <= 6; seed++) play(seed, 2);
    play(11, 3);
    play(12, 4);
    play(13, 6);

    // An equality nothing exercised proves nothing.
    CHECK(seen.decisions > 300, "hundreds of decisions");
    CHECK(seen.scored > 1000 && seen.why == seen.scored, "sampled rows, each with its storylines");
    CHECK(seen.multi_cover > 0, "covers of several cards");
    CHECK(seen.proven > 0 && seen.unknown >= 0, "proven verdicts");
    CHECK(seen.pruned > 0, "pruned rows");
    CHECK(seen.pinned > 0 && seen.voids > 0 && seen.floors > 0, "pins, voids and floors");
    CHECK(seen.covered > 0 && seen.single > 0, "covered battles and single-move decisions");
    CHECK(seen.short_story > 0 && seen.replies > 0, "storylines that resolve early, and replies");

    printf("oracle_answer_test: %d decisions, %d rows (%d scored, %d pruned, %d proven); %d checks passed, %d failed\n",
           seen.decisions, seen.rows, seen.scored, seen.pruned, seen.proven, n_pass, n_fail);
    return n_fail ? 1 : 0;
}

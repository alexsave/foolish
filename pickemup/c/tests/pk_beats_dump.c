/* The timeline of two takes, as Markdown tables, for docs/MOTION_REPORT.md:
 * a three-player arrival in which a seat draws three or more cards, the deck
 * is reshuffled and the seat plays; and a three-player deal. Real games from
 * the kernel, laid out by pk_beats exactly as a phone plays them.
 *
 *     make -C pickemup/c beats-dump
 */
#include "pk_check.h"
#include "../src/pk_beats.h"
#include "../src/pk_internal.h"

static PkEvent EV[4096];
static PkBeats B;

static const char *anc(int a)
{
    static const char *n[PK_ANC_COUNT] = { "-", "deck", "stack", "hand", "fan", "slot", "seat", "dir", "strip",
                                           "picker", "scrim", "bury", "results", "board", "row" };
    return a >= 0 && a < PK_ANC_COUNT ? n[a] : "?";
}

static const char *kind(int k)
{
    static const char *n[PK_BK_COUNT] = { "-", "flight", "flip", "gather", "fatten", "riffle", "halo", "band",
                                          "stamp", "slash", "dim", "turn bar", "turn", "fade", "shake", "shrug",
                                          "pulse", "ring", "pop", "collapse", "hold" };
    return k >= 0 && k < PK_BK_COUNT ? n[k] : "?";
}

static const char *ease(int e)
{
    static const char *n[PK_EASE_COUNT] = { "linear", "flight", "card-spring", "stamp", "in", "out", "ease-out" };
    return e >= 0 && e < PK_EASE_COUNT ? n[e] : "?";
}

static const char *event(int k)
{
    switch (k) {
    case PK_EV_LOBBY_START: return "LOBBY_START";
    case PK_EV_SHUFFLE: return "SHUFFLE";
    case PK_EV_DEAL: return "DEAL";
    case PK_EV_FLIP: return "FLIP";
    case PK_EV_BURY: return "BURY";
    case PK_EV_START_CARD: return "START_CARD";
    case PK_EV_TURN_TO: return "TURN_TO";
    case PK_EV_DRAW: return "DRAW";
    case PK_EV_RESHUFFLE_GATHER: return "RESHUFFLE_GATHER";
    case PK_EV_RESHUFFLE_SHUFFLE: return "RESHUFFLE_SHUFFLE";
    case PK_EV_PLAY: return "PLAY";
    case PK_EV_WILD_SUIT: return "WILD_SUIT";
    case PK_EV_SKIP: return "SKIP";
    case PK_EV_REVERSE: return "REVERSE";
    case PK_EV_PENALTY_DRAW: return "PENALTY_DRAW";
    case PK_EV_PASS: return "PASS";
    default: return "-";
    }
}

static void where(char *out, int n, int a, int i)
{
    if (a == PK_ANC_HAND || a == PK_ANC_FAN || a == PK_ANC_SLOT || a == PK_ANC_SEAT || a == PK_ANC_BURY ||
        a == PK_ANC_PICKER || a == PK_ANC_ROW)
        snprintf(out, (size_t)n, "%s.%d", anc(a), i);
    else
        snprintf(out, (size_t)n, "%s", anc(a));
}

static void print_take(const char *title)
{
    printf("### %s\n\n", title);
    printf("%d beats, %u ms end to end.\n\n", B.n, B.total_ms);
    printf("| # | event | beat | start ms | ms | easing | from | to | card | parts |\n");
    printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (int i = 0; i < B.n; i++) {
        const PkBeat *b = &B.beat[i];
        char f[24], t[24], c[16], p[24];
        where(f, sizeof f, b->from, b->from_i);
        where(t, sizeof t, b->to, b->to_i);
        if (b->card == PK_CARD_HIDDEN) snprintf(c, sizeof c, "back");
        else if (b->card == PK_CARD_NONE) snprintf(c, sizeof c, "-");
        else snprintf(c, sizeof c, "%d", b->card);
        if (b->parts > 1) snprintf(p, sizeof p, "%d x %d, %d apart", b->parts, b->part_ms, b->stagger_ms);
        else snprintf(p, sizeof p, "-");
        printf("| %d | %s | %s | %u | %d | %s | %s | %s | %s | %s |\n", i, event(b->ev_kind), kind(b->kind),
               b->start_ms, b->dur_ms, ease(b->ease), f, t, c, p);
    }
    printf("\n");
}

int main(void)
{
    static PkGame g;
    /* take 1: a 3-player bubble with 3+ draws, a reshuffle and a play */
    int found = 0;
    for (uint32_t k = 0; k < 20000 && !found; k++) {
        uint8_t seed[32];
        seed_wide(seed, 31000u + k);
        pk__new(&g, seed, 3, 0, 0);
        RS = 0xabcdefull + k;
        for (int step = 0; step < 3000 && !(g.over && !g.b_open) && !found; step++) {
            int before = g.bubbles;
            if (!bot_step(&g)) break;
            if (g.bubbles == before) continue;
            int n = pk_plan(&g, 0, g.bubbles - 1, g.bubbles, EV, 4096);
            int draws = 0, gag = 0, play = 0, other_seat = 0, turns = 0;
            for (int i = 0; i < n; i++) {
                draws += EV[i].kind == PK_EV_DRAW;
                gag += EV[i].kind == PK_EV_RESHUFFLE_GATHER && EV[i].half == PK_HALF_ACTION;
                play += EV[i].kind == PK_EV_PLAY;
                turns += EV[i].kind == PK_EV_TURN_TO;
                if (EV[i].kind == PK_EV_DRAW && EV[i].seat != 0) other_seat = 1;
            }
            if (draws >= 3 && draws <= 5 && gag == 1 && play == 1 && other_seat && turns == 1) {
                PkBeatFrame f;
                pk_beats_pre(&g, 0, g.bubbles - 1, &f);
                pk_beats_build(EV, n, &f, 0, 3, PK_BEATS_ARRIVAL, 0, 0, 0, &B);
                printf("<!-- seed_wide(%u), bubble %d, viewer seat 0 -->\n", 31000u + k, g.bubbles);
                print_take("Take 1: an arrival, three players: draws, a reshuffle, a play");
                found = 1;
            }
        }
    }
    if (!found) { printf("no such bubble\n"); return 1; }
    /* take 2: a 3-player deal as the dealer (seat 0) sees it on the start bubble */
    uint8_t seed[32];
    seed_wide(seed, 7);
    pk__new(&g, seed, 3, 0, 0);
    int n = pk_plan(&g, 0, -1, 0, EV, 4096);
    pk_beats_build(EV, n, 0, 0, 3, PK_BEATS_OPEN, 0, 0, 0, &B);
    print_take("Take 2: the deal, three players, the dealer's phone, opened");
    return 0;
}

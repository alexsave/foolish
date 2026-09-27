/* pk_play: Pick 'Em Up in a terminal, you against the Monte Carlo bots.
 *
 *     make -C pickemup/c play                        seed 1, five bots, you in seat 1
 *     make -C pickemup/c play PLAY_ARGS="--seed 42"
 *     ./pickemup/c/build/pk_play --seed 42 --bots 5 --seat 0 [--peek]
 *
 * EVERY RULE IS THE KERNEL'S. The menu is pk_legal (plus a Send when
 * pk_can_seal says the bubble may close), the screen is pk_view and the
 * pk_say lines, every move goes through pk_apply / pk_seal, and each sealed
 * bubble is narrated by its own caption (pk_say_caption). The bots are
 * PK_BOT_MC with the default knobs, the arena's strongest.
 *
 * THE TABLE ORDER IS pk_bot_round's (D61): each round every seat but the
 * turn seat may send an out-of-turn bubble, starting from the last sender,
 * then the turn seat sends its turn. You are asked out of turn only when
 * "Last card!" is yours to say; "Caught you!" is on your own turn's menu.
 *
 * Other seats' card counts are not shown (D22: the view has no slot for
 * them). --peek prints them anyway from the kernel's state, for debugging.
 *
 * Type a number and Enter; q or end of input quits. Dev only: stdio is fine. */
#include "../src/pk_bot.h"
#include "../src/pk_say.h"
#include "../src/pk_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const NAMES[PK_MAX_SEATS] = { "Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal" };
static PkGame g_game;
static int g_peek;

static void caption(const PkGame *g, int bubble)
{
    char line[256];
    if (pk_say_caption(g, bubble, NAMES, line, sizeof line) >= 0) printf("  %s\n", line);
}

/* One menu entry in the kernel's words: "Draw", "Play 7 of circles",
 * "Play Wild (squares)", "Pass", "Last card!", "Caught you! Bo". */
static void describe(const PkView *v, PkAct a, char *out, int cap)
{
    char card[64] = "", who[64] = "";
    switch (a.kind) {
    case PK_A_DRAW: snprintf(out, (size_t)cap, "%s", pk_text(PK_K_BTN_DRAW)); return;
    case PK_A_PASS: snprintf(out, (size_t)cap, "%s", pk_text(PK_K_BTN_PASS)); return;
    case PK_A_SAY_IT: snprintf(out, (size_t)cap, "%s", pk_text(PK_K_BTN_SAY)); return;
    case PK_A_CALL_OUT:
        pk_say_seat(NAMES, a.a, who, sizeof who);
        snprintf(out, (size_t)cap, "%s %s", pk_text(PK_K_BTN_CAUGHT), who);
        return;
    case PK_A_PLAY:
        pk_say_card(v->my_hand[a.a], card, sizeof card);
        if (a.b < PK_SUITS)
            snprintf(out, (size_t)cap, "%s %s (%s)", pk_text(PK_K_BTN_PLAY), card, pk_text(PK_K_SUIT_0 + a.b));
        else
            snprintf(out, (size_t)cap, "%s %s", pk_text(PK_K_BTN_PLAY), card);
        return;
    default: snprintf(out, (size_t)cap, "?"); return;
    }
}

static void show_table(const PkGame *g, int me)
{
    PkView v;
    pk_view(g, me, &v);
    char line[256], name[64];
    pk_say_headline(g, me, NAMES, line, sizeof line);
    printf("\n== %s", line);
    if (pk_say_subline(g, me, NAMES, line, sizeof line) > 0) printf(" - %s", line);
    printf(" ==\n");
    for (int s = 0; s < v.n; s++) {
        pk_say_seat(NAMES, s, name, sizeof name);
        printf("  %c %-5s", s == v.turn ? '>' : ' ', name);
        if (s == me) printf(" (you) cards: %d", v.my_n);
        else if (g_peek) printf(" [peek] cards: %d", g->hand_n[s]);
        if (v.said >> s & 1) printf("  %s", pk_text(PK_K_STAMP_LAST));
        printf("\n");
    }
    pk_say_spoken_stack(g, line, sizeof line);
    printf("  %s", line);
    if (pk_say_deck_left(g, line, sizeof line) > 0) printf("; deck %s", line);
    if (v.show_dir && pk_say_dir(g, line, sizeof line) > 0) printf("; %s", line);
    printf("\n  Your hand:");
    for (int p = 0; p < v.my_n; p++) {
        pk_say_card(v.my_hand[p], line, sizeof line);
        printf("%s%s%s", p ? ", " : " ", line, v.my_playable[p] ? "*" : "");
    }
    printf("\n");
}

/* A number from stdin: the choice, or -1 to quit (q, or end of input). */
static int ask(int lo, int hi)
{
    char buf[64];
    for (;;) {
        printf("  choose %d..%d (q quits): ", lo, hi);
        fflush(stdout);
        if (!fgets(buf, sizeof buf, stdin)) { printf("\n"); return -1; }
        if (!isatty(0)) printf("%s%s", buf, strchr(buf, '\n') ? "" : "\n");   /* a piped script reads back */
        if (buf[0] == 'q' || buf[0] == 'Q') return -1;
        char *end;
        long k = strtol(buf, &end, 10);
        if (end != buf && k >= lo && k <= hi) return (int)k;
        printf("  that is not on the menu\n");
    }
}

/* Your bubble: pick from the kernel's menu until it is sealed. 1 sent,
 * 0 nothing to send, -1 quit. Out of turn you are asked only when "Last
 * card!" is on the menu, and 0 lets the moment go. */
static int human_bubble(PkGame *g, int me)
{
    static PkAct m[PK_BOT_MENU_CAP];
    int own = g->turn == me;
    if (!own) {
        int n = pk_legal(g, me, m, PK_BOT_MENU_CAP), say = 0;
        for (int i = 0; i < n; i++) say |= m[i].kind == PK_A_SAY_IT;
        if (!say) return 0;
    }
    for (;;) {
        int n = pk_legal(g, me, m, PK_BOT_MENU_CAP);
        int seal = pk_can_seal(g);
        if (n == 0 && seal) { pk_seal(g); return 1; }
        if (n == 0) return 0;
        show_table(g, me);
        PkView v;
        pk_view(g, me, &v);
        char line[128];
        for (int i = 0; i < n; i++) {
            describe(&v, m[i], line, sizeof line);
            printf("  %2d  %s\n", i + 1, line);
        }
        int zero = seal || (!own && !g->b_open);
        if (seal) printf("   0  %s\n", pk_text(PK_K_SEND_HINT));
        else if (!g->b_open && !own) printf("   0  %s\n", pk_text(PK_K_BTN_CANCEL));
        int k = ask(zero ? 0 : 1, n);
        if (k < 0) return -1;
        if (k == 0) {
            if (!seal) return 0;
            pk_seal(g);
            return 1;
        }
        if (!pk_apply(g, me, m[k - 1])) { printf("  the kernel refused that\n"); return -1; }
    }
}

static void seed_from(uint8_t seed[32], unsigned long long v)
{
    memset(seed, 0, 32);
    for (int i = 0; i < 8; i++) seed[i] = (uint8_t)(v >> (8 * i));
}

int main(int argc, char **argv)
{
    unsigned long long seedv = 1;
    int bots = 5, me = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seedv = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--bots") && i + 1 < argc) bots = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seat") && i + 1 < argc) me = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peek")) g_peek = 1;
        else {
            fprintf(stderr, "usage: pk_play [--seed N] [--bots 1..7] [--seat K] [--peek]\n");
            return 2;
        }
    }
    int n = bots + 1;
    if (n < 2 || n > PK_MAX_SEATS || me < 0 || me >= n) {
        fprintf(stderr, "pk_play: --bots must be 1..%d and --seat 0..bots\n", PK_MAX_SEATS - 1);
        return 2;
    }
    uint8_t seed[32];
    seed_from(seed, seedv);
    PkGame *g = &g_game;
    if (!pk_new(g, seed, n)) return 1;

    PkBotKnobs knobs[PK_MAX_SEATS];
    for (int s = 0; s < n; s++) {
        pk_bot_knobs_default(&knobs[s]);
        knobs[s].seed ^= seedv ^ (uint64_t)s * 0x100000001b3ull;
    }
    uint64_t rng = 0x2545f4914f6cdd1dull ^ seedv;

    char name[64];
    pk_say_seat(NAMES, me, name, sizeof name);
    printf("%s, seed %llu: you are %s, against %d bots (%s)\n", pk_text(PK_K_GAME_NAME), seedv, name,
           bots, PK_BOT_NAME[PK_BOT_MC]);
    caption(g, 0);

    int last = -1, quit = 0;
    while (!g->over && !quit) {
        int from = last < 0 ? g->turn : last;
        /* out of turn, from the last sender round the table, then the turn */
        for (int j = 0; j <= n && !g->over && !quit; j++) {
            int s = j < n ? pk_next(g, from, j) : g->turn;
            if (j < n && s == g->turn) continue;
            int sent;
            if (s == me) {
                sent = human_bubble(g, me);
                if (sent < 0) { quit = 1; break; }
            } else {
                sent = pk_bot_bubble(g, s, PK_BOT_MC, &knobs[s], &rng);
                if (sent < 0) { fprintf(stderr, "pk_play: the kernel refused a bot move\n"); return 1; }
            }
            if (sent > 0) { last = s; caption(g, g->bubbles); }
        }
    }

    if (quit) {
        printf("Quit. Replay this deal with --seed %llu\n", seedv);
        return 0;
    }
    PkView v;
    pk_view(g, me, &v);
    char line[256];
    pk_say_headline(g, me, NAMES, line, sizeof line);
    printf("\n== %s ==\n", line);
    for (int s = 0; s < n; s++) {
        pk_say_seat(NAMES, s, name, sizeof name);
        printf("  %-5s%s cards left: %d\n", name, s == me ? " (you)" : "", v.reveal[s].n);
    }
    printf("%d turns, %d bubbles. Replay this deal with --seed %llu\n", g->turns, g->bubbles, seedv);
    return 0;
}

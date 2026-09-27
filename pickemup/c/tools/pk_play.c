/* pk_play: Pick 'Em Up in a terminal, you against the Monte Carlo bots.
 *
 *     make -C pickemup/c play                        seed 1, five bots, you in seat 1
 *     make -C pickemup/c play PLAY_ARGS="--seed 42"
 *     ./pickemup/c/build/pk_play --seed 42 --bots 5 --seat 0 [--peek] [--fast]
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
 * THE SCREEN IS ONE FRAME, repainted in place (clear and home) after every
 * move: the seats as a ring of boxes round the pile and the deck, the last
 * few captions, your hand as cards with their menu numbers under them, and
 * the other actions. After a bot's bubble it waits 400 ms (--fast: not).
 * Piped into a file it prints each frame after a rule instead.
 *
 * Other seats' card counts are not shown while playing (D22: the view has no
 * slot for them). --peek shows them from the kernel's state, for debugging.
 *
 * Type a number and Enter; q or end of input quits. Dev only: stdio is fine. */
#define _DEFAULT_SOURCE
#include "../src/pk_bot.h"
#include "../src/pk_say.h"
#include "../src/pk_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const NAMES[PK_MAX_SEATS] = { "Ana", "Bo", "Cy", "Di", "Ed", "Flo", "Gus", "Hal" };
static const char *const GLYPH[PK_SUITS + 1] = { "()", "/\\", "[]", "<>", "**" };   /* suit id order */
static PkGame g_game;
static int g_peek, g_fast, g_tty, g_me;
static unsigned long long g_seed;

/* ---- the log strip: the last few captions ---------------------------------- */
#define LOG_N 5
static char g_log[LOG_N][96];
static void log_caption(const PkGame *g, int bubble)
{
    memmove(g_log[0], g_log[1], sizeof g_log[0] * (LOG_N - 1));
    if (pk_say_caption(g, bubble, NAMES, g_log[LOG_N - 1], sizeof g_log[0]) < 0) g_log[LOG_N - 1][0] = 0;
}

/* ---- the canvas -------------------------------------------------------------- */
#define ROWS 16
#define COLS 76
static char scr[ROWS][COLS + 1];
static void put(int r, int c, const char *s)
{
    for (; *s && r >= 0 && r < ROWS && c < COLS; s++, c++)
        if (c >= 0) scr[r][c] = *s;
}
/* A w x h box; `card` rounds its corners. The inside is cleared. */
static void box(int r, int c, int w, int h, int card)
{
    for (int i = 0; i < h; i++)
        for (int j = 0; j < w; j++) {
            int top = i == 0, bot = i == h - 1, side = j == 0 || j == w - 1;
            char ch = ' ';
            if ((top || bot) && side) ch = card ? (top ? '.' : '\'') : '+';
            else if (top || bot) ch = '-';
            else if (side) ch = '|';
            if (r + i < ROWS && c + j < COLS) scr[r + i][c + j] = ch;
        }
}
/* A card's corner index: its number, Skip, Rev, +2, W, W+4. */
static void rank_label(uint8_t c, char *out)
{
    int r = pk_rank(c);
    if (r >= 1 && r <= 9) { out[0] = (char)('0' + r); out[1] = 0; return; }
    const char *s = r == PK_R_SKIP ? "Skip" : r == PK_R_REVERSE ? "Rev" : r == PK_R_PLUS2 ? pk_text(PK_K_RANK_PLUS2) : "W";
    snprintf(out, 8, "%s%s", s, r == PK_R_WILD4 ? pk_text(PK_K_INDEX_PLUS4) : "");
}

/* Seat slots round the pile, clockwise on screen from you at the bottom. */
static const int SLOT_R[7] = { 12, 6, 1, 1, 1, 6, 12 }, SLOT_C[7] = { 2, 2, 2, 30, 58, 58, 58 };
static const char *const PICK[8] = { "", "3", "24", "135", "1245", "12345", "012456", "0123456" };

static void seat_box(const PkGame *g, const PkView *v, int s, int r, int c)
{
    char name[64], l1[32], l2[32], cnt[16] = "";
    pk_say_seat(NAMES, s, name, sizeof name);
    int count = s == g_me ? v->my_n : v->over ? v->reveal[s].n : g_peek ? g->hand_n[s] : -1;
    if (count >= 0) snprintf(cnt, sizeof cnt, "%d card%s", count, count == 1 ? "" : "s");
    const char *stamp = v->over && v->winner == s ? pk_text(PK_K_STAMP_OUT)
                      : v->said >> s & 1 ? pk_text(PK_K_STAMP_LAST) : "";
    snprintf(l1, sizeof l1, "%c %-7.7s%4s", !v->over && v->turn == s ? '>' : ' ', name, s == g_me ? "you" : "");
    snprintf(l2, sizeof l2, "%-4s%10s", stamp, cnt);
    box(r, c, 16, 4, 0);
    put(r + 1, c + 2, l1);
    put(r + 2, c + 1, l2);
}

static void draw_table(const PkGame *g, const PkView *v)
{
    for (int i = 0; i < ROWS; i++) { memset(scr[i], ' ', COLS); scr[i][COLS] = 0; }
    char line[96], lbl[8];
    snprintf(line, sizeof line, "%s   seed %llu", pk_text(PK_K_GAME_NAME), g_seed);
    put(0, 2, line);
    for (int s = 0; s < v->n; s++) {
        int j = (s - g_me + v->n) % v->n;
        if (j == 0) { seat_box(g, v, s, 12, 30); continue; }
        int k = PICK[v->n - 1][j - 1] - '0';
        seat_box(g, v, s, SLOT_R[k], SLOT_C[k]);
    }
    if (v->show_dir) {                               /* which way play goes round */
        int cw = v->dir == PK_DIR_CW;
        pk_say_dir(g, line, sizeof line);
        put(0, 72 - (int)strlen(line), line);
        put(2, 20, cw ? "--->" : "<---");  put(2, 48, cw ? "--->" : "<---");
        put(13, 20, cw ? "<---" : "--->"); put(13, 48, cw ? "<---" : "--->");
        put(5, 9, cw ? "^" : "v");  put(11, 9, cw ? "^" : "v");
        put(5, 65, cw ? "v" : "^"); put(11, 65, cw ? "v" : "^");
    }
    /* the pile: its top card, and the suit to match under it */
    box(6, 25, 9, 5, 1);
    rank_label(v->top, lbl);
    put(7, 26, lbl);
    put(8, 28, GLYPH[pk_suit(v->top)]);
    put(9, 33 - (int)strlen(lbl), lbl);
    snprintf(line, sizeof line, "match %s", GLYPH[v->live_suit < PK_SUITS ? v->live_suit : PK_SUITS]);
    put(11, 25, line);
    /* the deck: two backs, the count under them */
    box(5, 41, 9, 5, 1);
    box(6, 40, 9, 5, 1);
    for (int i = 7; i < 10; i++) put(i, 41, ":::::::");
    pk_say_deck_left(g, line, sizeof line);
    put(11, 40, line);
}

/* ---- the frame --------------------------------------------------------------- */

/* Menu numbers for hand position p: "3", "2-5" for a wild's four suits, "". */
static void card_nums(const PkAct *m, int n, int p, char *out, int cap)
{
    int a = -1, b = -1;
    for (int i = 0; i < n; i++)
        if (m[i].kind == PK_A_PLAY && m[i].a == p) { if (a < 0) a = i + 1; b = i + 1; }
    if (a < 0) out[0] = 0;
    else if (a == b) snprintf(out, (size_t)cap, "%d", a);
    else snprintf(out, (size_t)cap, "%d-%d", a, b);
}

/* Repaint everything. `m`/`n` is the menu (n < 0: none, a bot is moving);
 * `zero` the 0 entry's word, or NULL; `msg` a line under the actions. */
static void paint(const PkGame *g, const PkAct *m, int n, const char *zero, const char *msg)
{
    PkView v;
    pk_view(g, g_me, &v);
    draw_table(g, &v);
    if (g_tty) printf("\033[H\033[2J");          /* clear and home: the frame never scrolls */
    else printf("\n%s\n", "============================================================================");
    for (int i = 0; i < ROWS; i++) {
        int e = COLS;
        while (e > 0 && scr[i][e - 1] == ' ') e--;
        printf("%.*s\n", e, scr[i]);
    }
    printf("\n");
    for (int i = 0; i < LOG_N; i++) printf("  %s %s\n", i == LOG_N - 1 ? ">" : " ", g_log[i]);
    char line[256], sub[256];
    pk_say_headline(g, g_me, NAMES, line, sizeof line);
    printf("\n  %s", line);
    if (pk_say_subline(g, g_me, NAMES, sub, sizeof sub) > 0) printf(" - %s", sub);
    printf("\n");
    /* your hand, twelve cards a row, each with its menu numbers under it */
    for (int r0 = 0; r0 < v.my_n; r0 += 12) {
        int r1 = r0 + 12 < v.my_n ? r0 + 12 : v.my_n;
        char lbl[8], num[16];
        for (int row = 0; row < (n < 0 ? 4 : 5); row++) {
            printf("  ");
            for (int p = r0; p < r1; p++) {
                uint8_t c = v.my_hand[p];
                rank_label(c, lbl);
                card_nums(m, n < 0 ? 0 : n, p, num, sizeof num);
                if (row == 0) printf(".----. ");
                else if (row == 1) printf("|%-4s| ", lbl);
                else if (row == 2) printf("| %s | ", GLYPH[pk_suit(c)]);
                else if (row == 3) printf("'----' ");
                else printf(" %-5s ", num);
            }
            printf("\n");
        }
    }
    if (n >= 0) {
        int col = 2, wild = 0;
        printf("  ");
        for (int i = 0; i <= n; i++) {
            char item[96] = "", who[64];
            if (i == n) { if (zero) snprintf(item, sizeof item, "0 %s", zero); }
            else if (m[i].kind == PK_A_DRAW) snprintf(item, sizeof item, "%d %s", i + 1, pk_text(PK_K_BTN_DRAW));
            else if (m[i].kind == PK_A_PASS) snprintf(item, sizeof item, "%d %s", i + 1, pk_text(PK_K_BTN_PASS));
            else if (m[i].kind == PK_A_SAY_IT) snprintf(item, sizeof item, "%d %s", i + 1, pk_text(PK_K_BTN_SAY));
            else if (m[i].kind == PK_A_CALL_OUT) {
                pk_say_seat(NAMES, m[i].a, who, sizeof who);
                snprintf(item, sizeof item, "%d %s %s", i + 1, pk_text(PK_K_BTN_CAUGHT), who);
            } else if (m[i].b < PK_SUITS) wild = 1;
            if (!item[0]) continue;
            int w = (int)strlen(item) + 3;
            if (col + w > COLS) { printf("\n  "); col = 2; }
            printf("%s   ", item);
            col += w;
        }
        printf("\n");
        if (wild) printf("  a wild's numbers pick the suit, in order: () /\\ [] <>\n");
    }
    if (msg && msg[0]) printf("  %s\n", msg);
    fflush(stdout);
}

/* A number from stdin: the choice, -1 to quit (q, or end of input), -2 for
 * a line that is not on the menu. */
static int ask(int lo, int hi)
{
    char buf[64];
    printf("  choose %d..%d (q quits): ", lo, hi);
    fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) { printf("\n"); return -1; }
    if (!g_tty) printf("%s%s", buf, strchr(buf, '\n') ? "" : "\n");   /* a piped script reads back */
    if (buf[0] == 'q' || buf[0] == 'Q') return -1;
    char *end;
    long k = strtol(buf, &end, 10);
    return end != buf && k >= lo && k <= hi ? (int)k : -2;
}

/* Your bubble: pick from the kernel's menu until it is sealed. 1 sent,
 * 0 nothing to send, -1 quit. Out of turn you are asked only when "Last
 * card!" is on the menu, and 0 lets the moment go. */
static int human_bubble(PkGame *g, int me)
{
    static PkAct m[PK_BOT_MENU_CAP];
    int own = g->turn == me;
    const char *msg = "";
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
        int cancel = !seal && !own && !g->b_open;
        paint(g, m, n, seal ? pk_text(PK_K_SEND_HINT) : cancel ? pk_text(PK_K_BTN_CANCEL) : NULL, msg);
        int k = ask(seal || cancel ? 0 : 1, n);
        if (k == -1) return -1;
        if (k == -2) { msg = "That is not on the menu"; continue; }
        msg = "";
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
    int bots = 5;
    g_seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_seed = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--bots") && i + 1 < argc) bots = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seat") && i + 1 < argc) g_me = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--peek")) g_peek = 1;
        else if (!strcmp(argv[i], "--fast")) g_fast = 1;
        else {
            fprintf(stderr, "usage: pk_play [--seed N] [--bots 1..7] [--seat K] [--peek] [--fast]\n");
            return 2;
        }
    }
    int n = bots + 1, me = g_me;
    if (n < 2 || n > PK_MAX_SEATS || me < 0 || me >= n) {
        fprintf(stderr, "pk_play: --bots must be 1..%d and --seat 0..bots\n", PK_MAX_SEATS - 1);
        return 2;
    }
    g_tty = isatty(1);
    uint8_t seed[32];
    seed_from(seed, g_seed);
    PkGame *g = &g_game;
    if (!pk_new(g, seed, n)) return 1;

    PkBotKnobs knobs[PK_MAX_SEATS];
    for (int s = 0; s < n; s++) {
        pk_bot_knobs_default(&knobs[s]);
        knobs[s].seed ^= g_seed ^ (uint64_t)s * 0x100000001b3ull;
    }
    uint64_t rng = 0x2545f4914f6cdd1dull ^ g_seed;
    log_caption(g, 0);

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
            if (sent > 0) {
                last = s;
                log_caption(g, g->bubbles);
                if (s != me && !g->over) {
                    paint(g, NULL, -1, NULL, NULL);
                    if (!g_fast) usleep(400000);
                }
            }
        }
    }

    if (!quit) paint(g, NULL, -1, NULL, NULL);
    printf("\n%s%d turns, %d bubbles. Replay this deal with --seed %llu\n", quit ? "Quit. " : "",
           g->turns, g->bubbles, g_seed);
    return 0;
}

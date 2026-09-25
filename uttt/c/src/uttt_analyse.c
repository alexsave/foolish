/* The post-game analyser. See uttt_analyse.h. */
#include "uttt_analyse.h"
#include "uttt_bots.h"
#include <string.h>

const char *UTTT_LABEL_NAME[UA_LABELS] =
    { "best", "good", "inaccuracy", "mistake", "blunder", "only move" };

/* Proof searches get this many nodes. uttt_mate_in's line_in_reach gate
 * makes it free wherever no line of blocks is close, which is the opening. */
#define MATE_NODES 20000L
/* The exact solver is hopeless with more empty squares than this. */
#define SOLVE_EMPTIES 30

static const char *NAME[9] = { "NW", "N", "NE", "W", "C", "E", "SW", "S", "SE" };

static uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x ? x : 1;
}

static int n_empty(const UtttGame *g)
{
    int e = 0;
    for (unsigned live = g->live; live; live &= live - 1)
        e += __builtin_popcount(uttt_open_cells(g, __builtin_ctz(live)));
    return e;
}

/* Is the position after our move (they are to move) a PROVED win for us?
 * Every reply must leave us a forced win. Only asked when we had one. */
static int still_won(const UtttGame *c, uint8_t me)
{
    if (c->over) return c->over == me;
    uint8_t list[81], x;
    int n = uttt_legal(c, list);
    for (int i = 0; i < n; i++) {
        UtttGame u = *c;
        uttt_play(&u, list[i]);
        if (u.over) return 0;
        if (!uttt_mate_in(&u, MATE_NODES, &x)) return 0;
    }
    return 1;
}

/* What `mv` is worth to the side moving in `g`, 0..1. `proved` is set when
 * the number is a fact rather than quill's estimate. */
static double move_value(const UtttGame *g, uint8_t mv, int had_mate,
                         long playouts, int *proved)
{
    const uint8_t me = g->turn;
    UtttGame c = *g;
    uttt_play(&c, mv);
    *proved = 1;
    if (c.over) return c.over == UTTT_DRAW ? 0.5 : (c.over == me ? 1.0 : 0.0);

    uttt_bots_forget();
    uint8_t x;
    if (uttt_mate_in(&c, MATE_NODES, &x)) return 0.0;      /* they force it */
    if (had_mate && still_won(&c, me)) return 1.0;
    if (n_empty(&c) <= SOLVE_EMPTIES) {
        int s = uttt_solve(&c, 81);
        if (s != 2) return (1.0 - s) / 2.0;                 /* theirs, flipped */
    }
    uint64_t rs = mix(((uint64_t)g->n_plies << 8 | mv) ^ 0xA11A15Eull);
    int proof;
    double v = uttt_quill_value(&c, playouts, &rs, &proof);
    *proved = proof != 2;
    return 1.0 - v;
}

/* Can the side to move in `c` take a small board right now? */
static int can_take_board(const UtttGame *c)
{
    if (c->over) return 0;
    const int m = c->turn - 1;
    for (unsigned bb = uttt_legal_blocks(c); bb; bb &= bb - 1) {
        int b = __builtin_ctz(bb);
        if (uttt_mask_wins(c->cm[m][b]) & uttt_open_cells(c, b)) return 1;
    }
    return 0;
}

static UtttLabel label_of(double cost)
{
    if (cost <= 0.02 + 1e-9) return UA_BEST;
    if (cost <= 0.05 + 1e-9) return UA_GOOD;
    if (cost <= 0.10 + 1e-9) return UA_INACCURACY;
    if (cost <= 0.20 + 1e-9) return UA_MISTAKE;
    return UA_BLUNDER;
}

int uttt_analyse(const UtttGame *game, long playouts, UtttNote *out,
                 void (*progress)(int ply, int n_plies))
{
    UtttGame g;
    uttt_init(&g);
    for (int k = 0; k < game->n_plies; k++) {
        const uint8_t played = game->move[k];
        UtttNote *n = &out[k];
        memset(n, 0, sizeof *n);
        n->mover = g.turn;
        n->mv = played;

        uint8_t list[81];
        int cnt = uttt_legal(&g, list);
        n->n_legal = (uint8_t)cnt;

        uttt_bots_forget();
        uint8_t x;
        const int had_mate = uttt_mate_in(&g, MATE_NODES, &x) > 0;

        /* Every legal move, the played one included. Ties go to the lower
         * index, which is the kernel's ascending order. */
        n->p_best = -1;
        for (int i = 0; i < cnt; i++) {
            int pr;
            double v = move_value(&g, list[i], had_mate, playouts, &pr);
            if (v > n->p_best + 1e-12) {
                n->p_best = v; n->best = list[i]; n->best_proved = (uint8_t)pr;
            }
            if (list[i] == played) {
                n->p_played = v; n->played_proved = (uint8_t)pr;
            }
        }
        n->cost = n->p_best - n->p_played;

        UtttGame c = g;
        uttt_play(&c, played);
        if (c.over) n->sent = 255;
        else n->sent = (uint8_t)uttt_active(&c);

        if (uttt_block(&c, played / 9) == g.turn) n->tags |= UA_TAG_TAKES;
        if (!c.over && n->sent == 9) n->tags |= UA_TAG_GIFT;
        if (can_take_board(&c)) n->tags |= UA_TAG_HANDS;

        n->label = cnt == 1 ? UA_ONLY : label_of(n->cost);
        if (cnt > 1 && had_mate && !(n->played_proved && n->p_played >= 1.0)) {
            n->tags |= UA_TAG_THROWN | UA_TAG_PROOF;
            n->label = UA_BLUNDER;
        }
        if (n->cost > 1e-9 && n->best_proved && n->played_proved)
            n->tags |= UA_TAG_PROOF;

        n->x_after = n->mover == UTTT_X ? n->p_played : 1.0 - n->p_played;
        g = c;
        if (progress) progress(k + 1, game->n_plies);
    }
    return game->n_plies;
}

/* ------------------------------------------------------------------ report */

static int row_of(uint8_t mv) { return (mv / 9) / 3 * 3 + (mv % 9) / 3 + 1; }
static int col_of(uint8_t mv) { return (mv / 9) % 3 * 3 + (mv % 9) % 3 + 1; }

static void move_str(char *s, size_t cap, uint8_t mv)
{
    snprintf(s, cap, "(%d,%d) %s/%s", row_of(mv), col_of(mv),
             NAME[mv / 9], NAME[mv % 9]);
}

static void tags_str(char *s, size_t cap, uint8_t t)
{
    s[0] = 0;
    if (t & UA_TAG_THROWN) strncat(s, "THREW FORCED WIN ", cap - strlen(s) - 1);
    if (t & UA_TAG_GIFT)   strncat(s, "GIFT ",             cap - strlen(s) - 1);
    if (t & UA_TAG_TAKES)  strncat(s, "TAKES BOARD ",      cap - strlen(s) - 1);
    if (t & UA_TAG_HANDS)  strncat(s, "HANDS BOARD ",      cap - strlen(s) - 1);
    size_t n = strlen(s);
    if (n) s[n - 1] = 0;
}

static const char *sent_str(uint8_t sent)
{
    return sent == 255 ? "-" : sent == 9 ? "any" : NAME[sent];
}

static char mark_ch(uint8_t m) { return m == UTTT_X ? 'X' : m == UTTT_O ? 'O' : '.'; }

/* The position before a move, the sheet as uttt_play prints it without the
 * colour: '+' where the mover may play, '!' the move played, '*' the better
 * one. A decided board is drawn as what it became. */
static void board_print(FILE *f, const UtttGame *g, int played, int better)
{
    static const char *BIG_X = "X X X X X", *BIG_O = "OOOO OOOO",
                      *BIG_D = "#########";
    unsigned live = g->over ? 0u : uttt_legal_blocks(g);
    fprintf(f, "        1 2 3   4 5 6   7 8 9\n");
    fprintf(f, "      +-------+-------+-------+\n");
    for (int r = 0; r < 9; r++) {
        fprintf(f, "    %d | ", r + 1);
        for (int col = 0; col < 9; col++) {
            int mv = ((r / 3) * 3 + col / 3) * 9 + (r % 3) * 3 + col % 3;
            uint8_t won = uttt_block(g, mv / 9), m = uttt_cell(g, mv);
            char ch;
            if (mv == played)      ch = '!';
            else if (mv == better) ch = '*';
            else if (won != UTTT_OPEN) {
                const char *gl = won == UTTT_X ? BIG_X : won == UTTT_O ? BIG_O : BIG_D;
                ch = gl[mv % 9];
            }
            else if (m != UTTT_OPEN) ch = mark_ch(m);
            else ch = ((live >> (mv / 9)) & 1u) ? '+' : '.';
            fprintf(f, "%c ", ch);
            if (col % 3 == 2) fprintf(f, col < 8 ? "| " : "|");
        }
        fprintf(f, "\n");
        if (r % 3 == 2) fprintf(f, "      +-------+-------+-------+\n");
    }
}

/* One line, without the trailing blanks a chart row would otherwise end in. */
static void put_trimmed(FILE *f, char *s, int len)
{
    while (len && s[len - 1] == ' ') len--;
    s[len] = 0;
    fprintf(f, "%s\n", s);
}

static int pct(double v) { return (int)(v * 100.0 + 0.5); }

void uttt_analyse_print(FILE *f, const UtttGame *g, const UtttNote *nt,
                        long playouts)
{
    const int np = g->n_plies;
    char a[32], b[32], t[64];

    fprintf(f, "ULTIMATE TIC-TAC-TOE - POST-GAME ANALYSIS\n");
    fprintf(f, "%d plies, result: %s. quill at %ld playouts a position.\n",
            np, g->over == UTTT_X ? "X wins" : g->over == UTTT_O ? "O wins"
              : g->over == UTTT_DRAW ? "draw" : "unfinished", playouts);
    fprintf(f, "P = the mover's expected score (a draw is half a win); cost in points.\n");
    fprintf(f, "(row,col) is the whole sheet 1..9; board/cell are compass names.\n\n");

    fprintf(f, "ply who move            sends  before after  cost  label        better                  tags\n");
    fprintf(f, "--- --- --------------- -----  ------ -----  ----  -----------  ----------------------  ----\n");
    for (int k = 0; k < np; k++) {
        const UtttNote *n = &nt[k];
        move_str(a, sizeof a, n->mv);
        tags_str(t, sizeof t, n->tags);
        b[0] = 0;
        if (n->label != UA_BEST && n->label != UA_ONLY && n->best != n->mv) {
            char m[24];
            move_str(m, sizeof m, n->best);
            snprintf(b, sizeof b, "%s %d%%", m, pct(n->p_best));
        }
        char lab[16];
        snprintf(lab, sizeof lab, "%s%s", UTTT_LABEL_NAME[n->label],
                 (n->tags & UA_TAG_PROOF) ? "!" : "");
        char line[200];
        snprintf(line, sizeof line,
                "%3d  %c  %-15s %-5s  %5d%% %4d%%  %4.1f  %-11s  %-22s  %s",
                k + 1, mark_ch(n->mover), a, sent_str(n->sent),
                pct(n->p_best), pct(n->p_played), n->cost * 100.0, lab, b, t);
        size_t len = strlen(line);
        while (len && line[len - 1] == ' ') line[--len] = 0;
        fprintf(f, "%s\n", line);
    }
    fprintf(f, "(a label ending in ! is proved, not estimated)\n\n");

    /* ---- per player */
    fprintf(f, "PER PLAYER\n");
    for (int p = UTTT_X; p <= UTTT_O; p++) {
        int moves = 0, decisions = 0, cnt[UA_LABELS] = {0}, gifts = 0,
            hands = 0, takes = 0;
        double sum = 0;
        for (int k = 0; k < np; k++) {
            const UtttNote *n = &nt[k];
            if (n->mover != p) continue;
            moves++;
            cnt[n->label]++;
            if (n->label != UA_ONLY) { decisions++; sum += n->cost; }
            if (n->tags & UA_TAG_GIFT)  gifts++;
            if (n->tags & UA_TAG_HANDS) hands++;
            if (n->tags & UA_TAG_TAKES) takes++;
        }
        double mean = decisions ? sum / decisions * 100.0 : 0.0;
        fprintf(f, "  %c  %d moves (%d only moves)  mean cost %.1f pts  accuracy %.1f%%\n",
                mark_ch((uint8_t)p), moves, cnt[UA_ONLY], mean, 100.0 - mean);
        fprintf(f, "     best %d  good %d  inaccuracy %d  mistake %d  blunder %d"
                   "   gifts %d  hands board %d  takes board %d\n",
                cnt[UA_BEST], cnt[UA_GOOD], cnt[UA_INACCURACY], cnt[UA_MISTAKE],
                cnt[UA_BLUNDER], gifts, hands, takes);
    }
    fprintf(f, "\n");

    /* ---- turning points: the three largest costs, earliest on a tie */
    int top[3] = { -1, -1, -1 };
    for (int k = 0; k < np; k++) {
        if (nt[k].cost <= 1e-9) continue;
        for (int j = 0; j < 3; j++) {
            if (top[j] < 0 || nt[k].cost > nt[top[j]].cost + 1e-12) {
                for (int m = 2; m > j; m--) top[m] = top[m - 1];
                top[j] = k;
                break;
            }
        }
    }
    fprintf(f, "TURNING POINTS\n");
    UtttGame pos;
    for (int j = 0; j < 3 && top[j] >= 0; j++) {
        const int k = top[j];
        const UtttNote *n = &nt[k];
        uttt_init(&pos);
        for (int i = 0; i < k; i++) uttt_play(&pos, g->move[i]);
        move_str(a, sizeof a, n->mv);
        move_str(b, sizeof b, n->best);
        tags_str(t, sizeof t, n->tags);
        fprintf(f, "\n  #%d  ply %d, %c played %s -> sends %s: %d%% -> %d%%, "
                   "cost %.1f (%s%s)\n",
                j + 1, k + 1, mark_ch(n->mover), a, sent_str(n->sent),
                pct(n->p_best), pct(n->p_played), n->cost * 100.0,
                UTTT_LABEL_NAME[n->label], (n->tags & UA_TAG_PROOF) ? ", proved" : "");
        fprintf(f, "      better: %s at %d%%%s%s\n", b, pct(n->p_best),
                t[0] ? "   played move: " : "", t);
        board_print(f, &pos, n->mv, n->best);
    }
    fprintf(f, "  (! played, * better, + where the mover could play)\n\n");

    /* ---- X's curve: the start, then after every ply */
    double xs[UTTT_MAX_PLIES + 1];
    xs[0] = np ? (nt[0].mover == UTTT_X ? nt[0].p_best : 1.0 - nt[0].p_best) : 0.5;
    for (int k = 0; k < np; k++) xs[k + 1] = nt[k].x_after;

    fprintf(f, "X'S WIN PROBABILITY (tenths, start then after each ply)\n  ");
    for (int k = 0; k <= np; k++) {
        int d = (int)(xs[k] * 10.0 + 0.5);
        fputc(d >= 10 ? '^' : '0' + d, f);
    }
    fprintf(f, "\n\n");
    char buf[UTTT_MAX_PLIES + 16];
    for (int row = 10; row >= 0; row--) {
        int l = snprintf(buf, sizeof buf, "  %3d%% |", row * 10);
        for (int k = 0; k <= np; k++) {
            int d = (int)(xs[k] * 10.0 + 0.5);
            buf[l++] = d == row ? '*' : row == 5 ? '-' : ' ';
        }
        put_trimmed(f, buf, l);
    }
    int l = snprintf(buf, sizeof buf, "        ");
    for (int k = 0; k <= np; k++) {
        char ch = ' ';
        if (k > 0) {
            const UtttNote *n = &nt[k - 1];
            if (n->label == UA_BLUNDER) ch = n->mover == UTTT_X ? 'B' : 'b';
            else if (n->label == UA_MISTAKE) ch = n->mover == UTTT_X ? 'M' : 'm';
        }
        buf[l++] = ch;
    }
    put_trimmed(f, buf, l);
    l = snprintf(buf, sizeof buf, "        ");
    for (int k = 0; k <= np; k++) buf[l++] = k % 10 == 0 ? '|' : ' ';
    put_trimmed(f, buf, l);
    fprintf(f, "        ply 0, a tick every 10. B/M: X's blunder/mistake, b/m: O's.\n");
}

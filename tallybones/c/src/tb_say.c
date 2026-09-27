/* Tallybones - which sentence a position says. See tb_say.h. */
#include "tb_say.h"
#include "tb_plan.h"
#include <string.h>

extern const char *const TB_STRINGS_EN[TB_K_COUNT];

#define TB_KEY_MAX_(name, max, empty) max,
static const unsigned char KEY_MAX[TB_K_COUNT] = { TB_KEYS(TB_KEY_MAX_) };
#undef TB_KEY_MAX_
#define TB_KEY_EMPTY_(name, max, empty) empty,
static const unsigned char KEY_EMPTY[TB_K_COUNT] = { TB_KEYS(TB_KEY_EMPTY_) };
#undef TB_KEY_EMPTY_

const char *tb_text(int key)
{
    if (key < 0 || key >= TB_K_COUNT || !TB_STRINGS_EN[key]) return "";
    return TB_STRINGS_EN[key];
}

const char *tb_key_name(int key) { return key >= 0 && key < TB_K_COUNT ? TB_KEY_NAME[key] : ""; }
int tb_key_max(int key) { return key >= 0 && key < TB_K_COUNT ? KEY_MAX[key] : 0; }
int tb_key_may_be_empty(int key) { return key >= 0 && key < TB_K_COUNT && KEY_EMPTY[key]; }

#define T(k) tb_text(TB_K_##k)

/* ---- columns (pk_text_cols's rule) ---------------------------------------------- */

static unsigned next_cp(const unsigned char *s, int *len)
{
    if (s[0] < 0x80) { *len = 1; return s[0]; }
    int n = s[0] >= 0xf0 ? 4 : s[0] >= 0xe0 ? 3 : 2;
    unsigned c = s[0] & (0x3fu >> (n - 1));
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *len = i; return 0xfffd; }
        c = (c << 6) | (s[i] & 0x3f);
    }
    *len = n;
    return c;
}

static int cp_cols(unsigned c)
{
    if ((c >= 0x0300 && c <= 0x036f) || c == 0x200b || c == 0x200d || c == 0x200e
        || c == 0x200f || (c >= 0xfe00 && c <= 0xfe0f))
        return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6) || c >= 0x1f300)
        return 2;
    return 1;
}

int tb_text_cols(const char *s)
{
    int cols = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        int n;
        cols += cp_cols(next_cp(p, &n));
        p += n;
    }
    return cols;
}

/* ---- filling ------------------------------------------------------------------ */

int tb_itoa(int v, char *out, int cap)
{
    char tmp[12];
    int n = 0, neg = v < 0;
    unsigned u = neg ? 0u - (unsigned)v : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    if (neg) tmp[n++] = '-';
    if (!out || n + 1 > cap) return -1;
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static int put(char *out, int cap, const char *s)
{
    int n = (int)strlen(s);
    if (!out || n >= cap) return -1;
    memcpy(out, s, (size_t)n + 1);
    return n;
}

int tb_fill(char *out, int cap, const char *t, const char *const *kv)
{
    if (!out || cap < 1 || !t) return -1;
    int o = 0;
    for (const char *p = t; *p;) {
        const char *v = 0;
        int skip = 1;
        if (*p == '{') {
            const char *e = p + 1;
            while (*e && *e != '}' && *e != '{') e++;
            if (*e == '}') {
                size_t len = (size_t)(e - p - 1);
                for (int i = 0; kv && kv[i]; i += 2)
                    if (len == strlen(kv[i]) && !strncmp(p + 1, kv[i], len)) {
                        v = kv[i + 1] ? kv[i + 1] : "";
                        break;
                    }
                if (!v && len == 4 && !strncmp(p + 1, "game", 4)) v = T(GAME_NAME);
                if (v) skip = (int)(e - p) + 1;
            }
        }
        if (v) {
            int n = (int)strlen(v);
            if (o + n >= cap) return -1;
            memcpy(out + o, v, (size_t)n);
            o += n;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = *p;
        }
        p += skip;
    }
    out[o] = 0;
    return o;
}

static int fill_key(char *out, int cap, int key, const char *const *kv)
{
    return tb_fill(out, cap, tb_text(key), kv);
}

/* ---- things ------------------------------------------------------------------- */

enum { NAME_CAP = 64, LINE_CAP = 256 };

int tb_say_seat(const char *const *names, int seat, char *out, int cap)
{
    if (seat < 0 || seat >= TB_MAX_SEATS) return -1;
    if (names && names[seat] && names[seat][0]) return put(out, cap, names[seat]);
    char num[4];
    tb_itoa(seat + 1, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return fill_key(out, cap, TB_K_SEAT_FALLBACK, kv);
}

int tb_say_cat(int cat, char *out, int cap)
{
    if (cat < 0 || cat >= TB_CATS) return -1;
    return fill_key(out, cap, TB_K_CAT_0 + cat, 0);
}

int tb_say_row_label(int row, char *out, int cap)
{
    if (row < 0 || row >= 16) return -1;
    if (row < 6) return tb_say_cat(row, out, cap);
    if (row == 6) return put(out, cap, T(ROW_UPPER));
    if (row == 7) return put(out, cap, T(ROW_BONUS));
    if (row == 15) return put(out, cap, T(ROW_TOTAL));
    return tb_say_cat(row - 2, out, cap);
}

static const char *num_word(int k)
{
    return k >= 1 && k <= 6 ? tb_text(TB_K_NUM_1 + k - 1) : "";
}

/* ---- the bubble: what it did, as facts ----------------------------------------- */

typedef struct {
    int move;                   /* TB_EV_START, KEEP, SCORE, LEAVE, or 0 */
    int seat, cat, value, mask;
    uint8_t dice[TB_DICE];
    int bonus, bonus_value;
    int next;                   /* the seat to roll, or -1 */
    int over, winners, win_total;
} Facts;

static void facts_of(const TbEvent *ev, int n, Facts *f)
{
    memset(f, 0, sizeof *f);
    f->next = -1;
    for (int i = 0; i < n; i++) {
        const TbEvent *e = &ev[i];
        switch (e->kind) {
        case TB_EV_START: case TB_EV_KEEP: case TB_EV_SCORE: case TB_EV_LEAVE:
            f->move = e->kind;
            f->seat = e->seat;
            f->cat = e->cat;
            f->value = e->value;
            f->mask = e->mask;
            memcpy(f->dice, e->dice, TB_DICE);
            break;
        case TB_EV_BONUS: f->bonus = 1; f->bonus_value = e->value; break;
        case TB_EV_TURN:  f->next = e->seat; break;
        case TB_EV_OVER:  f->over = 1; f->winners = e->mask; f->win_total = e->value; break;
        }
    }
}

/* "3, 3, 5": the kept dice, smallest first */
static int kept_list(const Facts *f, char *out, int cap)
{
    int v[TB_DICE], k = 0;
    for (int i = 0; i < TB_DICE; i++)
        if (f->mask >> i & 1) {
            int x = f->dice[i], j = k++;
            while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
            v[j] = x;
        }
    int o = 0;
    out[0] = 0;
    for (int i = 0; i < k; i++) {
        char num[4];
        if (i && put(out + o, cap - o, T(LIST_SEP)) < 0) return -1;
        o = (int)strlen(out);
        if (tb_itoa(v[i], num, sizeof num) < 0 || put(out + o, cap - o, num) < 0) return -1;
        o = (int)strlen(out);
    }
    return k;
}

static int clause_move(const Facts *f, const char *const *names, char *out, int cap)
{
    char who[NAME_CAP], num[8], cat[NAME_CAP], dice[32], phrase[NAME_CAP] = "";
    if (f->move == TB_EV_START) return fill_key(out, cap, TB_K_CAP_STARTED, 0);
    if (tb_say_seat(names, f->seat, who, sizeof who) < 0) return -1;
    if (f->move == TB_EV_LEAVE) {
        const char *kv[] = { "who", who, 0 };
        return fill_key(out, cap, TB_K_CAP_QUIT, kv);
    }
    if (f->move == TB_EV_KEEP) {
        int k = kept_list(f, dice, sizeof dice);
        if (k < 0) return -1;
        const char *kv[] = { "who", who, "dice", dice, "count", num_word(TB_DICE - k), 0 };
        return fill_key(out, cap, k ? TB_K_CAP_KEEP : TB_K_CAP_KEEP_NONE, kv);
    }
    if (f->move != TB_EV_SCORE) return -1;
    tb_itoa(f->value, num, sizeof num);
    if (tb_say_cat(f->cat, cat, sizeof cat) < 0) return -1;
    if (f->cat >= TB_C_THREE_ALIKE && f->cat <= TB_C_LONG_RUN)
        tb_fill(phrase, sizeof phrase, tb_text(TB_K_PHRASE_6 + f->cat - TB_C_THREE_ALIKE), 0);
    const char *kv[] = { "who", who, "n", num, "cat", cat, "phrase", phrase, 0 };
    int key = f->value == 0                 ? TB_K_CAP_ZERO
            : f->cat < TB_UPPER_CATS        ? TB_K_CAP_SCORED_UPPER
            : f->cat == TB_C_TALLYBONES     ? TB_K_CAP_SCORED_SHOUT
            : f->cat == TB_C_ANY            ? TB_K_CAP_SCORED_ANY
            :                                 TB_K_CAP_SCORED_COMBO;
    return fill_key(out, cap, key, kv);
}

static int clause_bonus(const Facts *f, const char *const *names, char *out, int cap)
{
    char who[NAME_CAP], num[8];
    if (tb_say_seat(names, f->seat, who, sizeof who) < 0) return -1;
    tb_itoa(f->bonus_value, num, sizeof num);
    const char *kv[] = { "who", who, "n", num, 0 };
    return fill_key(out, cap, TB_K_CAP_BONUS, kv);
}

static int clause_next(const Facts *f, const char *const *names, char *out, int cap)
{
    char next[NAME_CAP];
    if (tb_say_seat(names, f->next, next, sizeof next) < 0) return -1;
    const char *kv[] = { "next", next, 0 };
    return fill_key(out, cap, TB_K_CAP_NEXT, kv);
}

/* "Alex" / "Alex and Bo" / "Alex, Bo and Cy": {a} is all but the last */
static int winners_ab(int mask, const char *const *names, char *a, int acap, char *b, int bcap)
{
    int last = -1, k = 0;
    for (int s = 0; s < TB_MAX_SEATS; s++) if (mask >> s & 1) { last = s; k++; }
    a[0] = 0;
    if (last < 0 || tb_say_seat(names, last, b, bcap) < 0) return -1;
    int o = 0, first = 1;
    for (int s = 0; s < last; s++) {
        if (!(mask >> s & 1)) continue;
        char who[NAME_CAP];
        if (tb_say_seat(names, s, who, sizeof who) < 0) return -1;
        if (!first && put(a + o, acap - o, T(LIST_SEP)) < 0) return -1;
        o = (int)strlen(a);
        if (put(a + o, acap - o, who) < 0) return -1;
        o = (int)strlen(a);
        first = 0;
    }
    return k;
}

static int clause_end(const Facts *f, const char *const *names, char *out, int cap)
{
    char a[LINE_CAP], b[NAME_CAP], num[8];
    int k = winners_ab(f->winners, names, a, sizeof a, b, sizeof b);
    if (k < 1) return -1;
    tb_itoa(f->win_total, num, sizeof num);
    const char *kv[] = { "who", b, "a", a, "b", b, "n", num, 0 };
    return fill_key(out, cap, k == 1 ? TB_K_CAP_WON : TB_K_CAP_TIE, kv);
}

int tb_say_of(const TbEvent *ev, int n, const char *const *names, int full, char *out, int cap)
{
    if (!out || cap < 1 || n < 0) return -1;
    Facts f;
    facts_of(ev, n, &f);
    if (!f.move) return put(out, cap, "");
    char c[4][LINE_CAP];
    int nc = 0;
    /* the summary tells it in order; the caption leads with what matters most */
    if (full) {
        if (clause_move(&f, names, c[nc++], LINE_CAP) < 0) return -1;
        if (f.bonus && clause_bonus(&f, names, c[nc++], LINE_CAP) < 0) return -1;
        if (f.over && clause_end(&f, names, c[nc++], LINE_CAP) < 0) return -1;
        else if (!f.over && f.next >= 0 && clause_next(&f, names, c[nc++], LINE_CAP) < 0) return -1;
    } else if (f.over) {
        /* the last bubble's line is the result, alone */
        if (clause_end(&f, names, c[nc++], LINE_CAP) < 0) return -1;
    } else {
        if (clause_move(&f, names, c[nc++], LINE_CAP) < 0) return -1;
        if (f.bonus && clause_bonus(&f, names, c[nc++], LINE_CAP) < 0) return -1;
        if (f.next >= 0 && clause_next(&f, names, c[nc++], LINE_CAP) < 0) return -1;
    }
    if (put(out, cap, c[0]) < 0) return -1;
    int o = (int)strlen(out);
    for (int i = 1; i < nc; i++) {
        const char *join = o > 0 && out[o - 1] == '!' ? T(CAP_JOIN_BANG) : T(CAP_JOIN);
        char line[2 * LINE_CAP + 8];
        size_t jn = strlen(join), cn = strlen(c[i]);
        if ((size_t)o + jn + cn + 1 > sizeof line) return -1;
        memcpy(line, out, (size_t)o);
        memcpy(line + o, join, jn);
        memcpy(line + o + jn, c[i], cn + 1);
        if (!full && tb_text_cols(line) > TB_CAPTION_MAX) break;
        if (put(out, cap, line) < 0) return -1;
        o = (int)strlen(out);
    }
    return o;
}

static int say_bubble(const TbGame *g, int bubble, const char *const *names, int full, char *out, int cap)
{
    TbEvent ev[16];
    if (!g || bubble < 0 || bubble > g->hist_n) return -1;
    int n = tb_plan(g, bubble - 1, bubble, ev, 16);
    if (n < 0) return -1;
    return tb_say_of(ev, n, names, full, out, cap);
}

int tb_say_caption(const TbGame *g, int bubble, const char *const *names, char *out, int cap)
{
    return say_bubble(g, bubble, names, 0, out, cap);
}

int tb_say_summary(const TbGame *g, int bubble, const char *const *names, char *out, int cap)
{
    return say_bubble(g, bubble, names, 1, out, cap);
}

int tb_say_move(const TbGame *g, TbMove m, const char *const *names, int full, char *out, int cap)
{
    TbEvent ev[16];
    int n = tb_plan_move(g, m, ev, 16);
    if (n < 0) return -1;
    return tb_say_of(ev, n, names, full, out, cap);
}

int tb_say_lobby_caption(int which, const char *who, char *out, int cap)
{
    int key = which == TB_SAY_INVITE ? TB_K_CAP_INVITE : which == TB_SAY_JOINED ? TB_K_CAP_JOINED
            : which == TB_SAY_LEFT ? TB_K_CAP_LEFT : -1;
    if (key < 0) return -1;
    const char *kv[] = { "who", who ? who : "", 0 };
    return fill_key(out, cap, key, kv);
}

/* ---- the screen ------------------------------------------------------------------ */

int tb_say_headline(const TbGame *g, int viewer, const char *const *names, char *out, int cap)
{
    char who[NAME_CAP];
    if (g->over) {
        int w = tb_winners(g);
        if (viewer >= 0 && w == (1 << viewer)) return put(out, cap, T(HEAD_YOU_WIN));
        char a[LINE_CAP], b[NAME_CAP];
        int k = winners_ab(w, names, a, sizeof a, b, sizeof b);
        if (k < 1) return -1;
        const char *kv[] = { "who", b, "a", a, "b", b, 0 };
        return fill_key(out, cap, k == 1 ? TB_K_HEAD_WINS : TB_K_HEAD_TIE, kv);
    }
    if (viewer >= 0 && viewer < g->n && !tb_is_in(g, viewer)) return put(out, cap, T(HEAD_LEFT));
    if (g->draft && g->pending.seat == viewer) {
        switch (g->pending.kind) {
        case TB_M_KEEP:  return put(out, cap, T(HEAD_STAGED_KEEP));
        case TB_M_SCORE: return put(out, cap, T(HEAD_STAGED_SCORE));
        default:         return put(out, cap, T(HEAD_STAGED_LEAVE));
        }
    }
    if (g->turn == viewer) return put(out, cap, T(HEAD_YOUR_ROLL));
    if (tb_say_seat(names, g->turn, who, sizeof who) < 0) return -1;
    const char *kv[] = { "who", who, 0 };
    return fill_key(out, cap, TB_K_HEAD_WAITING, kv);
}

int tb_say_subline(const TbGame *g, int viewer, const char *const *names, char *out, int cap)
{
    char who[NAME_CAP], num[8], cat[NAME_CAP];
    if (g->over) return put(out, cap, T(SUB_OVER));
    if (viewer >= 0 && viewer < g->n && !tb_is_in(g, viewer)) return put(out, cap, "");
    if (g->draft && g->pending.seat == viewer) {
        if (g->pending.kind == TB_M_KEEP) return put(out, cap, T(SUB_STAGED_KEEP));
        if (g->pending.kind != TB_M_SCORE) return put(out, cap, "");
        tb_itoa(g->score[viewer][g->pending.arg], num, sizeof num);
        if (tb_say_cat(g->pending.arg, cat, sizeof cat) < 0) return -1;
        const char *kv[] = { "cat", cat, "n", num, 0 };
        return fill_key(out, cap, TB_K_SUB_STAGED_SCORE, kv);
    }
    if (g->turn == viewer) {
        int left = TB_ROLLS - g->roll;
        return put(out, cap, left >= 2 ? T(SUB_ROLLS_2) : left == 1 ? T(SUB_ROLLS_1) : T(SUB_ROLLS_0));
    }
    if (tb_say_seat(names, g->turn, who, sizeof who) < 0) return -1;
    tb_itoa(g->roll, num, sizeof num);
    const char *kv[] = { "who", who, "n", num, 0 };
    return fill_key(out, cap, TB_K_SUB_THEIR_ROLL, kv);
}

int tb_say_spoken_die(const TbGame *g, int i, char *out, int cap)
{
    if (i < 0 || i >= TB_DICE) return -1;
    char num[4];
    tb_itoa(i + 1, num, sizeof num);
    const char *kv[] = { "n", num, "face", num_word(g->dice[i]), 0 };
    int kept = g->roll > 1 && (g->kept >> i & 1);
    int key = !g->dice[i] ? TB_K_SPOKEN_DIE_UNKNOWN : kept ? TB_K_SPOKEN_DIE_KEPT : TB_K_SPOKEN_DIE;
    return fill_key(out, cap, key, kv);
}

int tb_say_rules_title(char *out, int cap) { return fill_key(out, cap, TB_K_RULES_TITLE, 0); }

int tb_say_rule(int i, char *out, int cap)
{
    if (i < 0 || i >= TB_RULES_N) return -1;
    return fill_key(out, cap, TB_K_RULE_1 + i, 0);
}

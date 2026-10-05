/* Chui Niu - which sentence a position says. See cn_say.h. */
#include "cn_say.h"
#include "../../../shared/c/text_util/text_util.h"
#include "cn_msg.h"
#include <string.h>

extern const char *const CN_STRINGS_EN[CN_K_COUNT];

#define CN_KEY_MAX_(name, max, empty) max,
static const unsigned char KEY_MAX[CN_K_COUNT] = { CN_KEYS(CN_KEY_MAX_) };
#undef CN_KEY_MAX_
#define CN_KEY_EMPTY_(name, max, empty) empty,
static const unsigned char KEY_EMPTY[CN_K_COUNT] = { CN_KEYS(CN_KEY_EMPTY_) };
#undef CN_KEY_EMPTY_

const char *cn_text(int key)
{
    if (key < 0 || key >= CN_K_COUNT || !CN_STRINGS_EN[key]) return "";
    return CN_STRINGS_EN[key];
}

const char *cn_key_name(int key) { return key >= 0 && key < CN_K_COUNT ? CN_KEY_NAME[key] : ""; }
int cn_key_max(int key) { return key >= 0 && key < CN_K_COUNT ? KEY_MAX[key] : 0; }
int cn_key_may_be_empty(int key) { return key >= 0 && key < CN_K_COUNT && KEY_EMPTY[key]; }

#define T(k) cn_text(CN_K_##k)

/* ---- columns and filling: shared/c/text_util, named for this kernel's API ----- */

int cn_text_cols(const char *s) { return text_cols(s); }
int cn_itoa(int v, char *out, int cap) { return text_itoa(v, out, cap); }

/* {game} is the game's name wherever a list does not name it itself. */
int cn_fill(char *out, int cap, const char *t, const char *const *kv)
{
    const char *const game[] = { "game", T(GAME_NAME), 0 };
    return text_fill(out, cap, t, kv, game);
}

/* ---- things ------------------------------------------------------------------- */

enum { NAME_CAP = 64, PHRASE_CAP = 64, LINE_CAP = 256 };

static int qty_word(int q, int initial, char *out, int cap)
{
    if (q >= 1 && q <= CN_NUM_WORDS)
        return text_put(out, cap, cn_text((initial ? CN_K_NUMCAP_1 : CN_K_NUM_1) + q - 1));
    return cn_itoa(q, out, cap);
}

/* "four 3s", or with `digits` "4 3s" (a caption's shorter step). */
static int bid_phrase(int q, int f, int initial, int digits, char *out, int cap)
{
    if (q < 1 || q > CN_MAX_DICE || f < 1 || f > CN_FACES) return -1;
    char qty[16], face[4];
    if ((digits ? cn_itoa(q, qty, sizeof qty) : qty_word(q, initial, qty, sizeof qty)) < 0
        || cn_itoa(f, face, sizeof face) < 0)
        return -1;
    const char *kv[] = { "qty", qty, "face", face, 0 };
    return cn_fill(out, cap, q == 1 ? T(BID_ONE) : T(BID_MANY), kv);
}

int cn_say_bid(int q, int f, int initial, char *out, int cap)
{
    return bid_phrase(q, f, initial, 0, out, cap);
}

/* ---- the caption's width (cn_say.h) ------------------------------------------------ */

/* THE SYSTEM FONT AT 17 POINTS, ASCII 0x20 to 0x7E, in eighths of a point:
 * the wider of the regular and the semibold advance, rounded up, as Core
 * Text measures them (docs_pkgY.md has the table's source; BubbleLineTests
 * holds every entry against the phone's own font). */
static const unsigned char ASCII_W[95] = {
     35,  43,  70,  86,  86, 132,  97,  42,  53,  53,  62,  86,  42,  62,  42,  41,  /*  !"#$%&'()*+,-./ */
     87,  64,  83,  86,  88,  85,  88,  78,  89,  88,  42,  42,  86,  86,  86,  71,  /* 0123456789:;<=>? */
    123,  94,  90,  97,  98,  80,  77, 100, 102,  38,  76,  91,  77, 119, 100, 103,  /* @ABCDEFGHIJKLMNO */
     87, 103,  89,  87,  86,  99,  93, 132,  94,  91,  88,  53,  41,  53,  86,  80,  /* PQRSTUVWXYZ[\]^_ */
     65,  75,  84,  76,  84,  77,  50,  83,  81,  34,  34,  76,  35, 121,  80,  80,  /* `abcdefghijklmno */
     83,  83,  54,  72,  51,  80,  74, 109,  73,  76,  73,  53,  36,  53,  86,       /* pqrstuvwxyz{|}~  */
};

/* Any other code point, by the widest glyph of its class (cn_say.h). */
static int cp_width(unsigned c)
{
    if (c >= 0x20 && c <= 0x7E) return ASCII_W[c - 0x20];
    if (text_cp_cols(c) == 0) return 0;                       /* a combining mark, a joiner */
    if ((c >= 0x01C4 && c <= 0x01CC) || (c >= 0x01F1 && c <= 0x01F3)) return 46 * CN_CAP_UNIT;  /* "DŽ" */
    if ((c >= 0x00A0 && c <= 0x024F) || (c >= 0x1E00 && c <= 0x1EFF)) return 19 * CN_CAP_UNIT;  /* Latin */
    if (c >= 0x0370 && c <= 0x04FF) return 22 * CN_CAP_UNIT;                                     /* Greek, Cyrillic */
    if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF)
        || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF01 && c <= 0xFF60)
        || (c >= 0x20000 && c <= 0x3FFFD))
        return 19 * CN_CAP_UNIT;                                                                 /* CJK, kana, Hangul */
    if ((c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF)) return 24 * CN_CAP_UNIT;  /* emoji */
    if (c >= 0x12000 && c <= 0x1254F) return 80 * CN_CAP_UNIT;                                   /* cuneiform */
    if (c == 0xFDFD) return 61 * CN_CAP_UNIT;                                                    /* the bismillah */
    return 46 * CN_CAP_UNIT;                                                                     /* the rest */
}

int cn_cap_width(const char *s)
{
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        int n;
        w += cp_width(text_next_cp(p, &n));
        p += n;
    }
    return w;
}

/* `name` cut to the longest run of whole characters (a base with the marks
 * after it) that fits `room` units together with CAP_CLIP, the spaces
 * before the mark dropped: "Maximilia…". The mark alone when nothing fits. */
static int clip_name(const char *name, int room, char *out, int cap)
{
    const char *mark = T(CAP_CLIP);
    room -= cn_cap_width(mark);
    int keep = 0, w = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p;) {
        int n, cw = 0, len = 0;
        cw += cp_width(text_next_cp(p, &n));
        len += n;
        while (p[len]) {                                      /* the marks ride with their base */
            int m;
            unsigned c = text_next_cp(p + len, &m);
            if (text_cp_cols(c) != 0) break;
            len += m;
        }
        if (w + cw > room) break;
        w += cw;
        p += len;
        keep = (int)(p - (const unsigned char *)name);
    }
    while (keep > 0 && name[keep - 1] == ' ') keep--;
    int b = (int)strlen(mark);
    if (keep + b >= cap) return -1;
    memcpy(out, name, (size_t)keep);
    memcpy(out + keep, mark, (size_t)b + 1);
    return keep + b;
}

/* THE ONE LINE. `forms` (ending in -1) are one sentence from long to
 * short; each is tried with the bid in words, then in digits, with the
 * whole name; the first within CN_CAP_BUDGET is said. Past every form, the
 * last form with the bid in digits clips the name to the room it leaves. A
 * sentence with no bid passes q = 0. */
static int fit_line(const int *forms, const char *who, int q, int f, char *out, int cap)
{
    char bid[PHRASE_CAP] = "", clipped[NAME_CAP];
    const char *kv[] = { "who", who, "bid", bid, 0 };
    int last = forms[0];
    for (int i = 0; forms[i] >= 0; i++) {
        last = forms[i];
        for (int digits = 0; digits <= (q > 0); digits++) {
            if (q > 0 && bid_phrase(q, f, 0, digits, bid, sizeof bid) < 0) return -1;
            int n = cn_fill(out, cap, cn_text(last), kv);
            if (n < 0) return -1;
            if (cn_cap_width(out) <= CN_CAP_BUDGET) return n;
        }
    }
    kv[1] = "";
    if (cn_fill(out, cap, cn_text(last), kv) < 0) return -1;
    if (clip_name(who, CN_CAP_BUDGET - cn_cap_width(out), clipped, sizeof clipped) < 0) return -1;
    kv[1] = clipped;
    return cn_fill(out, cap, cn_text(last), kv);
}

int cn_say_seat(const char *const *names, int seat, char *out, int cap)
{
    if (seat < 0 || seat >= CN_MAX_SEATS) return -1;
    if (names && names[seat] && names[seat][0]) return text_put(out, cap, names[seat]);
    char num[4];
    cn_itoa(seat + 1, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return cn_fill(out, cap, T(SEAT_FALLBACK), kv);
}

int cn_say_dice_n(int n, char *out, int cap)
{
    if (n < 0 || n > CN_MAX_DICE) return -1;
    char num[4];
    cn_itoa(n, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return cn_fill(out, cap, n == 1 ? T(DICE_ONE) : T(DICE_MANY), kv);
}

/* ---- the bubble -------------------------------------------------------------------- */

/* Append `clause` after CAP_JOIN, or start the line with it. */
static int append(char *out, int cap, int *len, const char *clause)
{
    const char *join = *len ? T(CAP_JOIN) : "";
    int a = (int)strlen(join), b = (int)strlen(clause);
    if (*len + a + b >= cap) return -1;
    memcpy(out + *len, join, (size_t)a);
    memcpy(out + *len + a, clause, (size_t)b + 1);
    *len += a + b;
    return 0;
}

/* A CAPTION says what the bubble's sender did and never what a call found,
 * because a staged bubble's caption is on the sender's screen before it is
 * sent (K8). It is ONE ACT: the start's round, a bid or a call (a move's
 * plan has exactly one; were there more, the newest would be said), on one
 * line by fit_line. A later round opens silently. */
static int caption(const CnEvent *ev, int n, const char *const *names, char *out, int cap)
{
    static const int START[] = { CN_K_CAP_START, CN_K_CAP_START_SHORT, -1 };
    static const int BID[] = { CN_K_CAP_BID, -1 };
    static const int CALL[] = { CN_K_CAP_CALL, -1 };
    if (!out || cap < 1 || n < 0) return -1;
    out[0] = 0;
    const CnEvent *act = 0;
    for (int i = 0; i < n; i++)
        if ((ev[i].kind == CN_EV_ROUND && ev[i].move == 0) || ev[i].kind == CN_EV_BID || ev[i].kind == CN_EV_CALL)
            act = &ev[i];
    if (!act) return 0;
    char who[NAME_CAP];
    if (cn_say_seat(names, act->seat, who, sizeof who) < 0) return -1;
    if (act->kind == CN_EV_ROUND) return fit_line(START, who, 0, 0, out, cap);
    return fit_line(act->kind == CN_EV_BID ? BID : CALL, who, act->q, act->f, out, cap);
}

/* THE OUTCOME says what the call found, who went out and who won: a screen
 * line, every clause, so it is not held to the caption's one line. */
static int outcome_line(const CnEvent *ev, int n, const char *const *names, char *out, int cap)
{
    if (!out || cap < 1 || n < 0) return -1;
    out[0] = 0;
    int len = 0;
    char who[NAME_CAP] = "", other[NAME_CAP] = "", bid[PHRASE_CAP] = "", line[LINE_CAP];
    const CnEvent *call = 0;
    for (int i = 0; i < n; i++) {
        const CnEvent *e = &ev[i];
        const char *kv[] = { "who", who, "loser", other, "bid", bid, 0 };
        int w = 0;
        switch (e->kind) {
        case CN_EV_CALL:
            call = e;
            break;
        case CN_EV_LOSE:
            /* THE CALL'S CLAUSE WAITS FOR ITS LOSER: true or false is who lost */
            if (!call) return -1;
            if (cn_say_seat(names, call->seat, who, sizeof who) < 0
                || cn_say_seat(names, e->seat, other, sizeof other) < 0
                || cn_say_bid(call->q, call->f, 1, bid, sizeof bid) < 0)
                return -1;
            w = cn_fill(line, sizeof line, e->seat == call->seat ? T(CAP_CALL_TRUE) : T(CAP_CALL_FALSE), kv);
            if (w < 0 || append(out, cap, &len, line) < 0) return -1;
            break;
        case CN_EV_OUT: {
            /* the winner's clause says it all when this call ended the game */
            int ends = 0;
            for (int j = i + 1; j < n; j++) ends |= ev[j].kind == CN_EV_OVER;
            if (ends) break;
            if (cn_say_seat(names, e->seat, who, sizeof who) < 0) return -1;
            w = cn_fill(line, sizeof line, T(CAP_OUT), kv);
            if (w < 0 || append(out, cap, &len, line) < 0) return -1;
            break;
        }
        case CN_EV_OVER:
            if (cn_say_seat(names, e->seat, who, sizeof who) < 0) return -1;
            w = cn_fill(line, sizeof line, T(CAP_WINS), kv);
            if (w < 0 || append(out, cap, &len, line) < 0) return -1;
            break;
        default:
            break;
        }
    }
    return len;
}

int cn_say_caption_of(const CnEvent *ev, int n, const char *const *names, char *out, int cap)
{
    return caption(ev, n, names, out, cap);
}

int cn_say_outcome_of(const CnEvent *ev, int n, const char *const *names, char *out, int cap)
{
    return outcome_line(ev, n, names, out, cap);
}

int cn_say_outcome(const CnGame *g, const char *const *names, char *out, int cap)
{
    CnEvent ev[CN_EVENTS_PER_MOVE + 1];
    if (!g || !out || cap < 1) return -1;
    if (!g->call_at) { out[0] = 0; return 0; }
    int n = cn_plan(g, g->call_at - 1, g->call_at, ev, (int)(sizeof ev / sizeof ev[0]));
    if (n < 0) return -1;
    return outcome_line(ev, n, names, out, cap);
}

int cn_say_caption(const CnGame *g, int move, const char *const *names, char *out, int cap)
{
    CnEvent ev[CN_EVENTS_PER_MOVE + 1];
    if (!g || move < 0 || move > g->hist_n) return -1;
    int n = move == 0 ? cn_plan(g, -1, 0, ev, (int)(sizeof ev / sizeof ev[0]))
                      : cn_plan(g, move - 1, move, ev, (int)(sizeof ev / sizeof ev[0]));
    if (n < 0) return -1;
    return cn_say_caption_of(ev, n, names, out, cap);
}

int cn_say_lobby_caption(int which, const char *who, char *out, int cap)
{
    static const int INVITE[] = { CN_K_CAP_INVITE, CN_K_CAP_INVITE_SHORT, -1 };
    static const int JOINED[] = { CN_K_CAP_JOINED, -1 };
    static const int LEFT[] = { CN_K_CAP_LEFT, -1 };
    const char *name = who ? who : "";
    switch (which) {
    case CN_SAY_INVITE: return fit_line(INVITE, name, 0, 0, out, cap);
    case CN_SAY_JOINED: return fit_line(JOINED, name, 0, 0, out, cap);
    case CN_SAY_LEFT:   return fit_line(LEFT, name, 0, 0, out, cap);
    default:            return -1;
    }
}

/* ---- the screen --------------------------------------------------------------------- */

int cn_say_headline(const CnGame *g, int viewer, const char *const *names, char *out, int cap)
{
    char who[NAME_CAP];
    const char *kv[] = { "who", who, 0 };
    const int me = viewer >= 0 && viewer < g->n ? viewer : -1;
    if (g->phase == CN_PH_OVER) {
        if (me >= 0 && g->winner == me) return text_put(out, cap, T(HEAD_YOU_WIN));
        if (cn_say_seat(names, g->winner, who, sizeof who) < 0) return -1;
        return cn_fill(out, cap, T(HEAD_WINS), kv);
    }
    if (me >= 0 && g->dice_n[me] == 0) return text_put(out, cap, T(HEAD_YOU_OUT));
    if (me >= 0 && g->turn == me) {
        if (!cn_can_call(g)) return text_put(out, cap, T(HEAD_OPEN));
        if (!cn_min_raise(g, 0, 0)) return text_put(out, cap, T(HEAD_ONLY_CALL));
        return text_put(out, cap, T(HEAD_RAISE_OR_CALL));
    }
    if (cn_say_seat(names, g->turn, who, sizeof who) < 0) return -1;
    return cn_fill(out, cap, T(HEAD_THEIR_TURN), kv);
}

int cn_say_subline(const CnGame *g, const char *const *names, char *out, int cap)
{
    if (g->phase == CN_PH_OVER) return text_put(out, cap, "");
    if (!g->bid_q) return text_put(out, cap, T(SUB_NONE));
    char who[NAME_CAP], bid[PHRASE_CAP];
    if (cn_say_seat(names, g->bidder, who, sizeof who) < 0 || cn_say_bid(g->bid_q, g->bid_f, 0, bid, sizeof bid) < 0)
        return -1;
    const char *kv[] = { "who", who, "bid", bid, 0 };
    return cn_fill(out, cap, T(SUB_STANDING), kv);
}

int cn_say_staged(const CnGame *g, CnMove m, char *out, int cap)
{
    char bid[PHRASE_CAP];
    const char *kv[] = { "bid", bid, 0 };
    if (cn_is_call(m)) {
        if (!g->bid_q || cn_say_bid(g->bid_q, g->bid_f, 0, bid, sizeof bid) < 0) return -1;
        return cn_fill(out, cap, T(HEAD_STAGED_CALL), kv);
    }
    if (cn_say_bid(m.q, m.f, 0, bid, sizeof bid) < 0) return -1;
    return cn_fill(out, cap, T(HEAD_STAGED_BID), kv);
}

int cn_say_table(const CnGame *g, char *out, int cap)
{
    char num[4];
    cn_itoa(g->total, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return cn_fill(out, cap, T(SUB_TABLE), kv);
}

int cn_say_reveal_count(const CnGame *g, char *out, int cap)
{
    if (!g->call_at) return text_put(out, cap, "");
    return cn_say_tally(g->call_count, out, cap);
}

int cn_say_tally(int count, char *out, int cap)
{
    if (count < 0 || count > CN_MAX_DICE) return -1;
    char qty[16];
    if (qty_word(count, 0, qty, sizeof qty) < 0) return -1;
    const char *kv[] = { "qty", qty, 0 };
    return cn_fill(out, cap, T(REVEAL_COUNT), kv);
}

int cn_say_lobby_row(const char *const *names, int seat, int mine, char *out, int cap)
{
    char who[NAME_CAP], num[4];
    if (cn_say_seat(names, seat, who, sizeof who) < 0) return -1;
    cn_itoa(seat + 1, num, sizeof num);
    const char *kv[] = { "n", num, "who", who, 0 };
    return cn_fill(out, cap, mine ? T(LOBBY_ROW_YOU) : T(LOBBY_ROW), kv);
}

int cn_say_error(int code, char *out, int cap)
{
    switch (code) {
    case CN_EFORMAT: return text_put(out, cap, T(ERR_NEWER));
    case CN_ESHORT: case CN_ECHECK: case CN_EGAME: case CN_ETEXT:
              return text_put(out, cap, T(ERR_DAMAGED));
    default:  return code < 0 ? text_put(out, cap, T(ERR_UNREADABLE)) : -1;
    }
}

int cn_say_rules_title(char *out, int cap) { return cn_fill(out, cap, T(RULES_TITLE), 0); }

int cn_say_rule(int i, char *out, int cap)
{
    if (i < 0 || i >= CN_RULES_N) return -1;
    return text_put(out, cap, cn_text(CN_K_RULE_1 + i));
}

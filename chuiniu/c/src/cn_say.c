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

int cn_say_bid(int q, int f, int initial, char *out, int cap)
{
    if (q < 1 || q > CN_MAX_DICE || f < 1 || f > CN_FACES) return -1;
    char qty[16], face[4];
    if (qty_word(q, initial, qty, sizeof qty) < 0 || cn_itoa(f, face, sizeof face) < 0) return -1;
    const char *kv[] = { "qty", qty, "face", face, 0 };
    return cn_fill(out, cap, q == 1 ? T(BID_ONE) : T(BID_MANY), kv);
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

/* THE ONE COMPOSER. A caption (`outcome` 0) says what the bubble's sender
 * did and never what a call found, because a staged bubble's caption is on
 * the sender's screen before it is sent (K8); the outcome line says what the
 * call found, who went out and who won. */
static int compose(const CnEvent *ev, int n, const char *const *names, int outcome, char *out, int cap)
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
        case CN_EV_ROUND:
            if (outcome || e->move != 0) break;                 /* a later round opens silently */
            if (cn_say_seat(names, e->seat, who, sizeof who) < 0) return -1;
            w = cn_fill(line, sizeof line, T(CAP_START), kv);
            if (w < 0 || append(out, cap, &len, line) < 0) return -1;
            break;
        case CN_EV_BID:
            if (outcome) break;
            if (cn_say_seat(names, e->seat, who, sizeof who) < 0 || cn_say_bid(e->q, e->f, 0, bid, sizeof bid) < 0)
                return -1;
            w = cn_fill(line, sizeof line, T(CAP_BID), kv);
            if (w < 0 || append(out, cap, &len, line) < 0) return -1;
            break;
        case CN_EV_CALL:
            call = e;
            if (outcome) break;
            if (cn_say_seat(names, e->seat, who, sizeof who) < 0 || cn_say_bid(e->q, e->f, 0, bid, sizeof bid) < 0)
                return -1;
            w = cn_fill(line, sizeof line, T(CAP_CALL), kv);
            if (w < 0 || append(out, cap, &len, line) < 0) return -1;
            break;
        case CN_EV_LOSE:
            if (!outcome) break;
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
            if (!outcome) break;
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
            if (!outcome) break;
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
    return compose(ev, n, names, 0, out, cap);
}

int cn_say_outcome_of(const CnEvent *ev, int n, const char *const *names, char *out, int cap)
{
    return compose(ev, n, names, 1, out, cap);
}

int cn_say_outcome(const CnGame *g, const char *const *names, char *out, int cap)
{
    CnEvent ev[CN_EVENTS_PER_MOVE + 1];
    if (!g || !out || cap < 1) return -1;
    if (!g->call_at) { out[0] = 0; return 0; }
    int n = cn_plan(g, g->call_at - 1, g->call_at, ev, (int)(sizeof ev / sizeof ev[0]));
    if (n < 0) return -1;
    return compose(ev, n, names, 1, out, cap);
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
    const char *kv[] = { "who", who ? who : "", 0 };
    switch (which) {
    case CN_SAY_INVITE: return cn_fill(out, cap, T(CAP_INVITE), kv);
    case CN_SAY_JOINED: return cn_fill(out, cap, T(CAP_JOINED), kv);
    case CN_SAY_LEFT:   return cn_fill(out, cap, T(CAP_LEFT), kv);
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
    char qty[16];
    if (qty_word(g->call_count, 0, qty, sizeof qty) < 0) return -1;
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

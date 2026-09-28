/* Pick 'Em Up - which sentence a position says. See pk_say.h. */
#include "pk_say.h"
#include "../../../shared/c/text_util/text_util.h"
#include "pk_plan.h"
#include "pk_view.h"
#include <string.h>

extern const char *const PK_STRINGS_EN[PK_K_COUNT];

#define PK_KEY_MAX_(name, max, empty) max,
static const unsigned char KEY_MAX[PK_K_COUNT] = { PK_KEYS(PK_KEY_MAX_) };
#undef PK_KEY_MAX_
#define PK_KEY_EMPTY_(name, max, empty) empty,
static const unsigned char KEY_EMPTY[PK_K_COUNT] = { PK_KEYS(PK_KEY_EMPTY_) };
#undef PK_KEY_EMPTY_

const char *pk_text(int key)
{
    if (key < 0 || key >= PK_K_COUNT || !PK_STRINGS_EN[key]) return "";
    return PK_STRINGS_EN[key];
}

const char *pk_key_name(int key) { return key >= 0 && key < PK_K_COUNT ? PK_KEY_NAME[key] : ""; }
int pk_key_max(int key) { return key >= 0 && key < PK_K_COUNT ? KEY_MAX[key] : 0; }
int pk_key_may_be_empty(int key) { return key >= 0 && key < PK_K_COUNT && KEY_EMPTY[key]; }

#define T(k) pk_text(PK_K_##k)

/* ---- columns and filling: shared/c/text_util, named for this kernel's API ----- */

int pk_text_cols(const char *s) { return text_cols(s); }
int pk_itoa(int v, char *out, int cap) { return text_itoa(v, out, cap); }

/* {game} is the game's name wherever a list does not name it itself. */
int pk_fill(char *out, int cap, const char *t, const char *const *kv)
{
    const char *const game[] = { "game", T(GAME_NAME), 0 };
    return text_fill(out, cap, t, kv, game);
}

/* ---- things ------------------------------------------------------------------- */

enum { NAME_CAP = 64, CARD_CAP = 64 };

static const char *suits_word(int s)
{
    return s >= 0 && s < PK_SUITS ? pk_text(PK_K_SUIT_0 + s) : "";
}

int pk_say_card(uint8_t c, char *out, int cap)
{
    if (c >= PK_DECK) return -1;
    int r = pk_rank(c);
    const char *suits = suits_word(pk_suit(c));
    switch (r) {
    case PK_R_WILD:  return text_put(out, cap, T(CARD_WILD));
    case PK_R_WILD4: return text_put(out, cap, T(CARD_WILD4));
    case PK_R_SKIP:    { const char *kv[] = { "suits", suits, 0 }; return pk_fill(out, cap, T(CARD_SKIP), kv); }
    case PK_R_REVERSE: { const char *kv[] = { "suits", suits, 0 }; return pk_fill(out, cap, T(CARD_REVERSE), kv); }
    case PK_R_PLUS2:   { const char *kv[] = { "suits", suits, 0 }; return pk_fill(out, cap, T(CARD_PLUS2), kv); }
    default: {
        char num[4];
        pk_itoa(r, num, sizeof num);
        const char *kv[] = { "rank", num, "suits", suits, 0 };
        return pk_fill(out, cap, T(CARD_NUMBER), kv);
    }
    }
}

int pk_say_seat(const char *const *names, int seat, char *out, int cap)
{
    if (seat < 0 || seat >= PK_MAX_SEATS) return -1;
    if (names && names[seat] && names[seat][0]) return text_put(out, cap, names[seat]);
    char num[4];
    pk_itoa(seat + 1, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return pk_fill(out, cap, T(SEAT_FALLBACK), kv);
}

int pk_say_deck_n(int deck_n, char *out, int cap)
{
    char num[4];
    if (deck_n < 0 || deck_n > PK_DECK) return -1;
    pk_itoa(deck_n, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return pk_fill(out, cap, T(DECK_LEFT), kv);
}

int pk_say_deck_left(const PkGame *g, char *out, int cap)
{
    return pk_say_deck_n(g->deck_n, out, cap);
}

int pk_say_dir_of(int n_seats, int dir, char *out, int cap)
{
    if (n_seats <= 2) return text_put(out, cap, "");   /* D13: no word at 2 players */
    return text_put(out, cap, dir == PK_DIR_CW ? T(DIR_CW) : T(DIR_ACW));
}

int pk_say_dir(const PkGame *g, char *out, int cap)
{
    return pk_say_dir_of(g->n, g->dir > 0 ? PK_DIR_CW : PK_DIR_ACW, out, cap);
}

/* ---- the caption -------------------------------------------------------------- */

typedef struct {
    int sender, said;
    int hit, miss, caught, catcher, target_of_miss;
    int last;                 /* PK_EV_PLAY, PK_EV_PASS or 0                       */
    int last_card, last_drew, effect, effect_seat, wild_suit;
    int turn_draws, total_draws;
    int reshuffled;
    int over, winner, win_card;
    int next;
} Facts;

static void facts_one(const PkEvent *e, void *ctx)
{
    Facts *f = (Facts *)ctx;
    switch (e->kind) {
    case PK_EV_BUBBLE_BEGIN: f->sender = e->seat; break;
    case PK_EV_SAY_IT:       f->said = 1; break;
    case PK_EV_DRAW:         f->turn_draws++; f->total_draws++; break;
    case PK_EV_PLAY:
        f->last = PK_EV_PLAY;
        f->last_card = e->card;
        f->last_drew = f->turn_draws;
        f->effect = 0;
        f->effect_seat = PK_SEAT_NONE;
        f->wild_suit = e->suit;
        break;
    case PK_EV_WILD_SUIT:    f->wild_suit = e->suit; break;
    case PK_EV_SKIP:
    case PK_EV_REVERSE:
    case PK_EV_REVERSE_AS_SKIP:
        f->effect = e->kind;
        f->effect_seat = e->seat;
        break;
    case PK_EV_PENALTY:
        if (e->i == PK_PEN_PLUS2 || e->i == PK_PEN_WILD4) {
            f->effect = PK_EV_PENALTY;
            f->effect_seat = e->seat;
        }
        break;
    case PK_EV_PASS:
        f->last = PK_EV_PASS;
        f->last_drew = f->turn_draws;
        break;
    case PK_EV_TURN_TO:      f->next = e->seat; f->turn_draws = 0; break;
    case PK_EV_RESHUFFLE_DONE: f->reshuffled = 1; break;
    case PK_EV_CALL_HIT:     f->hit = 1; f->caught = e->seat; f->catcher = e->other; break;
    case PK_EV_CALL_MISS:    f->miss = 1; f->catcher = e->seat; f->target_of_miss = e->other; break;
    case PK_EV_WIN:          f->over = e->i; f->winner = e->seat; f->win_card = e->card; break;
    default: break;
    }
}

/* One clause, or -1. */
typedef struct { int seats; const char *const *names; } Ctx;

static int clause_end(const Ctx *c, const Facts *f, char *out, int cap)
{
    char who[NAME_CAP];
    if (pk_say_seat(c->names, f->winner, who, sizeof who) < 0) return -1;
    const char *kv[] = { "who", who, 0 };
    switch (f->over) {
    case PK_OVER_OUT:
        return pk_fill(out, cap, f->win_card < PK_DECK && pk_is_wild((uint8_t)f->win_card)
                                 ? T(CAP_WON_WILD) : T(CAP_WON), kv);
    case PK_OVER_STUCK: return pk_fill(out, cap, T(CAP_STUCK), kv);
    default:            return pk_fill(out, cap, T(CAP_LONG), kv);
    }
}

static int clause_catch(const Ctx *c, const Facts *f, char *out, int cap)
{
    char who[NAME_CAP], target[NAME_CAP];
    int tseat = f->hit ? f->caught : f->target_of_miss;
    if (pk_say_seat(c->names, f->catcher, who, sizeof who) < 0
        || pk_say_seat(c->names, tseat, target, sizeof target) < 0) return -1;
    const char *kv[] = { "who", who, "target", target, 0 };
    return pk_fill(out, cap, f->hit ? T(CAP_CAUGHT) : T(CAP_WRONG), kv);
}

static int clause_said(const Ctx *c, const Facts *f, char *out, int cap)
{
    char who[NAME_CAP];
    if (pk_say_seat(c->names, f->sender, who, sizeof who) < 0) return -1;
    const char *kv[] = { "who", who, 0 };
    return pk_fill(out, cap, T(CAP_SAID), kv);
}

static int clause_turn(const Ctx *c, const Facts *f, char *out, int cap)
{
    char who[NAME_CAP], target[NAME_CAP] = "", card[CARD_CAP] = "", num[8];
    if (pk_say_seat(c->names, f->sender, who, sizeof who) < 0) return -1;
    if (f->effect_seat != PK_SEAT_NONE && pk_say_seat(c->names, f->effect_seat, target, sizeof target) < 0)
        return -1;
    int drew = f->last ? f->last_drew : f->total_draws;
    pk_itoa(drew, num, sizeof num);
    if (f->last == PK_EV_PLAY) pk_say_card((uint8_t)f->last_card, card, sizeof card);
    const char *kv[] = { "who", who, "target", target, "card", card, "n", num,
                         "suits", suits_word(f->wild_suit), 0 };
    if (f->last == PK_EV_PASS)
        return pk_fill(out, cap, drew ? T(CAP_DREW_AND_PASSED) : T(CAP_PASSED), kv);
    if (f->last == PK_EV_PLAY) {
        switch (pk_rank((uint8_t)f->last_card)) {
        case PK_R_WILD4:   return pk_fill(out, cap, T(CAP_WILD4), kv);
        case PK_R_PLUS2:   return pk_fill(out, cap, T(CAP_PLUS2), kv);
        case PK_R_SKIP:    return pk_fill(out, cap, T(CAP_SKIPPED), kv);
        case PK_R_REVERSE: return pk_fill(out, cap, c->seats > 2 ? T(CAP_REVERSED) : T(CAP_REVERSE_2P), kv);
        case PK_R_WILD:
            return pk_fill(out, cap, drew ? T(CAP_DREW_AND_PLAYED) : T(CAP_PLAYED_WILD), kv);
        default:
            return pk_fill(out, cap, drew ? T(CAP_DREW_AND_PLAYED) : T(CAP_PLAYED), kv);
        }
    }
    if (drew) return pk_fill(out, cap, drew == 1 ? T(CAP_DREW_ONE) : T(CAP_DREW_N), kv);
    return text_put(out, cap, "");
}

static int clause_next(const Ctx *c, int seat, int key, char *out, int cap)
{
    char next[NAME_CAP];
    if (pk_say_seat(c->names, seat, next, sizeof next) < 0) return -1;
    const char *kv[] = { "next", next, 0 };
    return pk_fill(out, cap, pk_text(key), kv);
}

/* Append `clause` after `out` if the line still fits. 1 if it was appended,
 * 0 if it did not fit (and nothing after it is tried), -1 on no room at all.
 * The joint is CAP_JOIN, or CAP_JOIN_BANG after a clause that already ends
 * in its own mark ("Ana: Last card! Bo to play"). */
static int append(char *out, int cap, int *len, const char *clause)
{
    if (!clause[0]) return 1;
    if (!*len) {
        int n = text_put(out, cap, clause);
        if (n < 0) return -1;
        *len = n;
        return 1;
    }
    char end = out[*len - 1];
    const char *join = end == '!' || end == '?' ? T(CAP_JOIN_BANG) : T(CAP_JOIN);
    if (pk_text_cols(out) + pk_text_cols(join) + pk_text_cols(clause) > PK_CAPTION_MAX) return 0;
    int jl = (int)strlen(join), cl = (int)strlen(clause);
    if (*len + jl + cl >= cap) return 0;
    memcpy(out + *len, join, (size_t)jl);
    memcpy(out + *len + jl, clause, (size_t)cl + 1);
    *len += jl + cl;
    return 1;
}

static void facts_init(Facts *f)
{
    memset(f, 0, sizeof *f);
    f->effect_seat = PK_SEAT_NONE;
    f->next = PK_SEAT_NONE;
    f->sender = PK_SEAT_NONE;
}

static int caption(const Facts *fp, int seats, const char *const *names, char *out, int cap)
{
    const Facts f = *fp;
    Ctx c = { seats, names };
    if (f.sender == PK_SEAT_NONE) return -1;
    char clause[256];
    int len = 0, r = 1;
    out[0] = 0;
    if (f.over && r > 0) {
        if (clause_end(&c, &f, clause, sizeof clause) < 0) return -1;
        r = append(out, cap, &len, clause);
    }
    if ((f.hit || f.miss) && r > 0) {
        if (clause_catch(&c, &f, clause, sizeof clause) < 0) return -1;
        r = append(out, cap, &len, clause);
    }
    if (f.said && r > 0) {
        if (clause_said(&c, &f, clause, sizeof clause) < 0) return -1;
        r = append(out, cap, &len, clause);
    }
    if (f.over != PK_OVER_OUT && r > 0) {
        if (clause_turn(&c, &f, clause, sizeof clause) < 0) return -1;
        r = append(out, cap, &len, clause);
    }
    if (f.reshuffled && r > 0) r = append(out, cap, &len, T(CAP_RESHUFFLED));
    if (!f.over && f.next != PK_SEAT_NONE && r > 0) {
        if (clause_next(&c, f.next, PK_K_CAP_NEXT, clause, sizeof clause) < 0) return -1;
        r = append(out, cap, &len, clause);
    }
    return r < 0 || !len ? -1 : len;
}

int pk_say_caption(const PkGame *g, int bubble, const char *const *names, char *out, int cap)
{
    if (!out || cap < 1 || bubble < 0 || bubble > g->bubbles) return -1;
    if (bubble == 0) {
        Ctx c = { g->n, names };
        return clause_next(&c, 1, PK_K_CAP_STARTED, out, cap);
    }
    Facts f;
    facts_init(&f);
    if (pk_plan_each(g, PK_VIEW_ALL, bubble - 1, bubble, facts_one, &f) < 0) return -1;
    return caption(&f, g->n, names, out, cap);
}

int pk_say_caption_of(const PkEvent *ev, int n, int seats, const char *const *names,
                      char *out, int cap)
{
    if (!out || cap < 1 || !ev || n < 1) return -1;
    Facts f;
    facts_init(&f);
    for (int i = 0; i < n; i++) facts_one(&ev[i], &f);
    return caption(&f, seats, names, out, cap);
}

int pk_say_lobby_caption(int which, const char *who, char *out, int cap)
{
    const char *kv[] = { "who", who ? who : "", 0 };
    switch (which) {
    case PK_SAY_INVITE: return pk_fill(out, cap, T(CAP_INVITE), kv);
    case PK_SAY_JOINED: return pk_fill(out, cap, T(CAP_JOINED), kv);
    case PK_SAY_LEFT:   return pk_fill(out, cap, T(CAP_LEFT), kv);
    default:            return -1;
    }
}

/* ---- the screen ----------------------------------------------------------------- */

int pk_say_headline(const PkGame *g, int viewer, const char *const *names, char *out, int cap)
{
    char who[NAME_CAP];
    int me = viewer >= 0 && viewer < g->n ? viewer : -1;
    if (g->over) {
        if (me >= 0 && g->winner == me) return text_put(out, cap, T(HEAD_YOU_WIN));
        if (pk_say_seat(names, g->winner, who, sizeof who) < 0) return -1;
        const char *kv[] = { "who", who, 0 };
        return pk_fill(out, cap, T(HEAD_WINS), kv);
    }
    if (me >= 0 && g->turn == me) return text_put(out, cap, T(HEAD_YOUR_TURN));
    if (pk_say_seat(names, g->turn, who, sizeof who) < 0) return -1;
    const char *kv[] = { "who", who, 0 };
    return pk_fill(out, cap, T(HEAD_WAITING), kv);
}

static int rank_word(uint8_t top, char *out, int cap)
{
    switch (pk_rank(top)) {
    case PK_R_SKIP:    return text_put(out, cap, T(RANK_SKIP));
    case PK_R_REVERSE: return text_put(out, cap, T(RANK_REVERSE));
    case PK_R_PLUS2:   return text_put(out, cap, T(RANK_PLUS2));
    default:           return pk_itoa(pk_rank(top), out, cap);
    }
}

/* Whether a rank word takes SUB_MATCH_AN: it starts with a vowel sound when
 * read aloud. Of the digits only 8 does ("eight"); a word does when it starts
 * with a vowel letter ("ace"). "+2" reads "plus two" and keeps SUB_MATCH. */
static int rank_takes_an(const char *w)
{
    char c = w[0];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return c == '8' || c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

int pk_say_subline(const PkGame *g, int viewer, const char *const *names, char *out, int cap)
{
    int me = viewer >= 0 && viewer < g->n ? viewer : -1;
    if (g->over || me < 0) return text_put(out, cap, "");
    PkView v;
    pk_view(g, me, &v);
    if (v.my_exposed) return text_put(out, cap, T(SUB_ON_ONE));
    if (g->said & (1u << me)) return text_put(out, cap, T(SUB_SAID));
    if (g->turn == me) {
        int any = 0;
        for (int p = 0; p < v.my_n; p++) any |= v.my_playable[p];
        uint8_t top = g->stack[g->stack_n - 1];
        if (any) {
            char rank[16];
            if (rank_word(top, rank, sizeof rank) < 0) return -1;
            const char *kv[] = { "suits", suits_word(g->live_suit), "rank", rank, 0 };
            return pk_fill(out, cap, pk_is_wild(top)       ? T(SUB_MATCH_WILD)
                                   : rank_takes_an(rank) ? T(SUB_MATCH_AN)
                                                         : T(SUB_MATCH), kv);
        }
        return text_put(out, cap, v.can_draw ? T(SUB_PLAYABLE_NONE) : "");
    }
    /* who plays before you: at most the next two (UI.html "Bo, then Cy, then you") */
    char a[NAME_CAP], b[NAME_CAP];
    if (pk_next(g, g->turn, 1) == me) {
        if (pk_say_seat(names, g->turn, a, sizeof a) < 0) return -1;
        const char *kv[] = { "a", a, 0 };
        return pk_fill(out, cap, T(SUB_ORDER_1), kv);
    }
    if (pk_next(g, g->turn, 2) == me) {
        if (pk_say_seat(names, g->turn, a, sizeof a) < 0
            || pk_say_seat(names, pk_next(g, g->turn, 1), b, sizeof b) < 0) return -1;
        const char *kv[] = { "a", a, "b", b, 0 };
        return pk_fill(out, cap, T(SUB_ORDER), kv);
    }
    return text_put(out, cap, "");
}

int pk_say_spoken_card(const PkGame *g, int viewer, int pos, char *out, int cap)
{
    if (viewer < 0 || viewer >= g->n || pos < 0 || pos >= g->hand_n[viewer]) return -1;
    char card[CARD_CAP];
    if (pk_say_card(g->hand[viewer][pos], card, sizeof card) < 0) return -1;
    const char *kv[] = { "card", card, "state",
                         pk_can_play(g, viewer, pos) ? T(SPOKEN_PLAYABLE) : T(SPOKEN_NOT_PLAYABLE), 0 };
    return pk_fill(out, cap, T(SPOKEN_CARD), kv);
}

int pk_say_spoken_fan(const char *const *names, int seat, char *out, int cap)
{
    char who[NAME_CAP];
    if (pk_say_seat(names, seat, who, sizeof who) < 0) return -1;
    const char *kv[] = { "who", who, 0 };
    return pk_fill(out, cap, T(SPOKEN_FAN), kv);
}

int pk_say_spoken_deck(const PkGame *g, char *out, int cap)
{
    char num[4];
    pk_itoa(g->deck_n, num, sizeof num);
    const char *kv[] = { "n", num, 0 };
    return pk_fill(out, cap, T(SPOKEN_DECK), kv);
}

int pk_say_spoken_stack(const PkGame *g, char *out, int cap)
{
    if (!g->stack_n) return -1;
    char card[CARD_CAP];
    if (pk_say_card(g->stack[g->stack_n - 1], card, sizeof card) < 0) return -1;
    const char *kv[] = { "card", card, "suits", suits_word(g->live_suit), 0 };
    return pk_fill(out, cap, T(SPOKEN_STACK), kv);
}

int pk_say_rules_title(char *out, int cap)
{
    return pk_fill(out, cap, T(RULES_TITLE), 0);
}

int pk_say_rule(int i, char *out, int cap)
{
    if (i < 0 || i >= PK_RULES_N) return -1;
    return pk_fill(out, cap, pk_text(PK_K_RULE_1 + i), 0);
}

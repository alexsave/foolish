// botcup.c - botcup.h.
#include "botcup.h"

#include "tournament.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const BE_CUP_KEYS[BE_CUP_BOTS] = { "kestrel", "sapper", "sonnet", "haiku" };

// The round robin, in the order Alex set: by index into bots[], a first.
static const uint8_t ROBIN[BE_CUP_ROBIN][2] = {
    { 0, 3 },   // kestrel v haiku
    { 1, 2 },   // sapper v sonnet
    { 0, 2 },   // kestrel v sonnet
    { 1, 3 },   // sapper v haiku
    { 2, 3 },   // sonnet v haiku
    { 0, 1 },   // kestrel v sapper
};

void be_cup_draw(be_w_cup *c, const uint8_t bots[BE_CUP_BOTS], int64_t now_ms) {
    memset(c, 0, sizeof *c);
    c->hash = be_wire_hash();
    c->state = BE_TOUR_RUNNING;
    c->champion = BE_TOUR_NONE;
    c->started_ms = now_ms;
    memcpy(c->bots, bots, BE_CUP_BOTS);
    for (int i = 0; i < BE_CUP_BOTS; i++) c->order[i] = (uint8_t)i;
    for (int m = 0; m < BE_CUP_MATCHES; m++) {
        be_w_match *x = &c->matches[m];
        x->round = (uint8_t)m;
        x->winner = BE_TOUR_NONE;
        if (m < BE_CUP_ROBIN) {
            x->a = bots[ROBIN[m][0]];
            x->b = bots[ROBIN[m][1]];
            x->seed_a = 1;
            x->seed_b = 2;
        } else x->a = x->b = BE_TOUR_NONE;
    }
}

int be_cup_next(const be_w_cup *c, uint8_t seats[2]) {
    if (c->state != BE_TOUR_RUNNING) return -1;
    for (int m = 0; m < BE_CUP_MATCHES; m++) {
        const be_w_match *x = &c->matches[m];
        if (x->winner != BE_TOUR_NONE) continue;
        // One match at a time: the first undecided one, or nothing.
        if (x->live || x->a == BE_TOUR_NONE || x->b == BE_TOUR_NONE) return -1;
        bool a_first = x->games % 2 == 0;
        seats[0] = a_first ? x->a : x->b;
        seats[1] = a_first ? x->b : x->a;
        return m;
    }
    return -1;
}

void be_cup_started(be_w_cup *c, int m, const char *gid, int gid_len) {
    if (m < 0 || m >= BE_CUP_MATCHES) return;
    be_w_match *x = &c->matches[m];
    x->live = 1;
    x->gid_len = (uint8_t)(gid_len < BE_GID ? gid_len : BE_GID);
    memcpy(x->gid, gid, x->gid_len);
}

static int slot_of(const be_w_cup *c, uint8_t bot) {
    for (int i = 0; i < BE_CUP_BOTS; i++)
        if (c->bots[i] == bot) return i;
    return -1;
}

// Head to head: matches i won against the others of `group` (n of them).
static int h2h(const be_w_cup *c, int i, const int *group, int n) {
    int w = 0;
    for (int m = 0; m < BE_CUP_ROBIN; m++) {
        const be_w_match *x = &c->matches[m];
        if (x->winner != c->bots[i]) continue;
        uint8_t other = x->a == c->bots[i] ? x->b : x->a;
        for (int k = 0; k < n; k++)
            if (c->bots[group[k]] == other) w++;
    }
    return w;
}

static bool level(const be_w_cup *c, int i, int j) {
    return c->mw[i] == c->mw[j] && c->gw[i] == c->gw[j] && c->gw[i] - c->gl[i] == c->gw[j] - c->gl[j];
}

void be_cup_standings(be_w_cup *c) {
    memset(c->mw, 0, sizeof c->mw);
    memset(c->ml, 0, sizeof c->ml);
    memset(c->gw, 0, sizeof c->gw);
    memset(c->gl, 0, sizeof c->gl);
    for (int m = 0; m < BE_CUP_ROBIN; m++) {
        const be_w_match *x = &c->matches[m];
        int a = slot_of(c, x->a), b = slot_of(c, x->b);
        if (a < 0 || b < 0) continue;
        c->gw[a] += x->wins_a;
        c->gl[a] += x->wins_b;
        c->gw[b] += x->wins_b;
        c->gl[b] += x->wins_a;
        if (x->winner == BE_TOUR_NONE) continue;
        if (x->winner == x->a) { c->mw[a]++; c->ml[b]++; }
        else { c->mw[b]++; c->ml[a]++; }
    }
    // Each bot's head-to-head score among those level with it on the first three keys.
    int hh[BE_CUP_BOTS];
    for (int i = 0; i < BE_CUP_BOTS; i++) {
        int group[BE_CUP_BOTS], n = 0;
        for (int j = 0; j < BE_CUP_BOTS; j++)
            if (j != i && level(c, i, j)) group[n++] = j;
        hh[i] = h2h(c, i, group, n);
    }
    int ord[BE_CUP_BOTS] = { 0, 1, 2, 3 };
    for (int i = 1; i < BE_CUP_BOTS; i++)   // an insertion sort: better first, stable on bots[] order
        for (int k = i; k > 0; k--) {
            int p = ord[k - 1], q = ord[k];
            int dp = c->gw[p] - c->gl[p], dq = c->gw[q] - c->gl[q];
            bool q_better = c->mw[q] != c->mw[p] ? c->mw[q] > c->mw[p]
                          : c->gw[q] != c->gw[p] ? c->gw[q] > c->gw[p]
                          : dq != dp           ? dq > dp
                                               : hh[q] > hh[p];
            if (!q_better) break;
            ord[k - 1] = q;
            ord[k] = p;
        }
    for (int i = 0; i < BE_CUP_BOTS; i++) c->order[i] = (uint8_t)ord[i];
}

bool be_cup_result(be_w_cup *c, int m, bool played, uint8_t fool_bot, int64_t now_ms) {
    if (m < 0 || m >= BE_CUP_MATCHES) return false;
    be_w_match *x = &c->matches[m];
    if (x->winner != BE_TOUR_NONE) return false;
    x->live = 0;
    if (!played) return true;
    uint8_t res = fool_bot == x->b ? BE_CUP_G_A : fool_bot == x->a ? BE_CUP_G_B : BE_CUP_G_DRAW;
    if (x->games < BE_TOUR_MAX_GAMES) c->result[m * BE_TOUR_MAX_GAMES + x->games] = res;
    x->games++;
    c->games++;
    if (res == BE_CUP_G_A) x->wins_a++;
    else if (res == BE_CUP_G_B) x->wins_b++;
    uint8_t w = BE_TOUR_NONE;
    if (x->wins_a >= BE_TOUR_WIN) w = x->a;
    else if (x->wins_b >= BE_TOUR_WIN) w = x->b;
    else if (x->games >= BE_TOUR_MAX_GAMES) w = x->wins_b > x->wins_a ? x->b : x->a;
    if (m < BE_CUP_ROBIN) be_cup_standings(c);
    if (w == BE_TOUR_NONE) return true;
    x->winner = w;
    x->done_ms = now_ms;
    if (m < BE_CUP_ROBIN) {
        be_cup_standings(c);
        bool all = true;
        for (int k = 0; k < BE_CUP_ROBIN; k++) all = all && c->matches[k].winner != BE_TOUR_NONE;
        if (all) {
            be_w_match *f = &c->matches[BE_CUP_ROBIN];
            f->a = c->bots[c->order[0]];
            f->b = c->bots[c->order[1]];
            f->seed_a = 1;
            f->seed_b = 2;
        }
    } else {
        c->champion = w;
        c->state = BE_TOUR_DONE;
        c->finished_ms = now_ms;
    }
    return true;
}

static const char *key_of(const be_w_cup *c, uint8_t bot) {
    int i = slot_of(c, bot);
    return i >= 0 ? BE_CUP_KEYS[i] : "tbd";
}

int be_cup_match_id(const be_w_cup *c, int m, char *out, int cap) {
    if (m < 0 || m >= BE_CUP_MATCHES) return 0;
    const be_w_match *x = &c->matches[m];
    int n = m < BE_CUP_ROBIN ? snprintf(out, (size_t)cap, "m%d-%s-v-%s", m + 1, key_of(c, x->a), key_of(c, x->b))
                             : snprintf(out, (size_t)cap, "final-%s-v-%s", key_of(c, x->a), key_of(c, x->b));
    return n < 0 ? 0 : n >= cap ? cap - 1 : n;
}

// ---- the file ------------------------------------------------------------------------------------
//   "BECU", u32 version, u32 sizeof(be_w_cup), u32 n ids, n x BE_BOT_ID bytes, be_w_cup.

#define CUP_MAGIC 0x55434542u   // "BECU"

bool be_cup_save(const be_w_cup *c, const char *path, const char *const *ids, int n) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint32_t head[4] = { CUP_MAGIC, 1, (uint32_t)sizeof *c, (uint32_t)n };
    bool ok = fwrite(head, sizeof head, 1, f) == 1;
    for (int i = 0; ok && i < n; i++) {
        char id[BE_BOT_ID];
        memset(id, 0, sizeof id);
        strncpy(id, ids[i], sizeof id);
        ok = fwrite(id, sizeof id, 1, f) == 1;
    }
    ok = ok && fwrite(c, sizeof *c, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

static uint8_t remap(uint8_t v, const int *map, uint32_t n) { return v == BE_TOUR_NONE || v >= n ? BE_TOUR_NONE : (uint8_t)map[v]; }

bool be_cup_load(be_w_cup *c, const char *path, const char *const *ids, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t head[4];
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == CUP_MAGIC && head[1] == 1 && head[2] == sizeof *c && head[3] <= 255;
    int map[256];
    for (uint32_t i = 0; ok && i < head[3]; i++) {
        char id[BE_BOT_ID + 1];
        ok = fread(id, BE_BOT_ID, 1, f) == 1;
        id[BE_BOT_ID] = 0;
        map[i] = BE_TOUR_NONE;
        for (int k = 0; ok && k < n; k++)
            if (strcmp(ids[k], id) == 0) map[i] = k;
        if (ok && map[i] == BE_TOUR_NONE) ok = false;
    }
    be_w_cup x;
    ok = ok && fread(&x, sizeof x, 1, f) == 1;
    fclose(f);
    if (!ok) return false;
    x.hash = be_wire_hash();
    x.champion = remap(x.champion, map, head[3]);
    for (int i = 0; i < BE_CUP_BOTS; i++) x.bots[i] = remap(x.bots[i], map, head[3]);
    for (int m = 0; m < BE_CUP_MATCHES; m++) {
        be_w_match *y = &x.matches[m];
        y->a = remap(y->a, map, head[3]);
        y->b = remap(y->b, map, head[3]);
        y->winner = remap(y->winner, map, head[3]);
    }
    *c = x;
    return true;
}

// ---- a game's story --------------------------------------------------------------------------

static const char SUIT_CH[4] = { 'S', 'H', 'C', 'D' };   // the kernel's suit order
static const char *const SUIT_NAME[4] = { "spades", "hearts", "clubs", "diamonds" };

static void card_str(int8_t suit, int8_t value, char out[4]) {
    static const char *const V = "??23456789TJQKA";   // value 1..13, ace 13 (2 is value 1 in a 52 deck)
    int v = value + 1;   // 6..13 (6 .. ace) in the 36-card deck: value 5 is a six
    out[0] = v >= 2 && v <= 14 ? V[v] : '?';
    out[1] = suit >= 0 && suit < 4 ? SUIT_CH[suit] : '?';
    out[2] = 0;
}

static void add(char *buf, int *len, const char *fmt, ...) {
    if (*len >= BE_CUP_STORY - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, (size_t)(BE_CUP_STORY - *len), fmt, ap);
    va_end(ap);
    if (n > 0) *len = *len + n < BE_CUP_STORY - 1 ? *len + n : BE_CUP_STORY - 1;
}

void be_cup_story_deal(be_cup_story *s, const char *const names[2], int power_suit, int8_t flip_suit, int8_t flip_value,
                       int leader, int deck_count) {
    memset(s, 0, sizeof *s);
    s->out_order[0] = s->out_order[1] = -1;
    char fc[4];
    card_str(flip_suit, flip_value, fc);
    add(s->text, &s->len, "  Trump: %s (face up: %s). %s leads. Stock %d after the deal.\n",
        power_suit >= 0 && power_suit < 4 ? SUIT_NAME[power_suit] : "?", fc, leader >= 0 && leader < 2 ? names[leader] : "?",
        deck_count);
}

void be_cup_story_event(be_cup_story *s, const char *const names[2], int power_suit, int kind, int seat,
                        const int8_t *suits, const int8_t *values, int n_cards, int deck_count) {
    if (seat < 0 || seat > 1) seat = -1;
    const char *who = seat >= 0 ? names[seat] : "?";
    char cards[3 * 40] = "";
    int cl = 0, n_trump = 0;
    for (int i = 0; i < n_cards && i < 36; i++) {
        char c[4];
        card_str(suits[i], values[i], c);
        cl += snprintf(cards + cl, sizeof cards - (size_t)cl, "%s%s", i ? " " : "", c);
        n_trump += suits[i] == power_suit;
    }
    const char *tag = kind == BE_EV_ATTACK ? "A" : kind == BE_EV_COVER ? "C" : kind == BE_EV_PASS ? "P" : kind == BE_EV_PICKUP ? "T"
                    : kind == BE_EV_GOOD ? "G" : kind == BE_EV_OUT ? "O" : NULL;
    if (!tag) return;
    if (kind != BE_EV_GOOD) {   // a bout to the discard is no one's move
        s->n_moves++;
        if (seat >= 0) s->seat_moves[seat]++;
    }
    add(s->moves, &s->moves_len, "%s%c:%s%s%s", s->moves_len ? " | " : "", seat >= 0 ? who[0] : '?', tag, cl ? " " : "", cards);
    unsigned m = s->n_moves;
    switch (kind) {
    case BE_EV_ATTACK:   // a lead or a throw-in
    case BE_EV_COVER:
        if (seat >= 0) s->trumps[seat] += (uint32_t)n_trump;
        if (n_trump >= 2 || (n_trump >= 1 && kind == BE_EV_ATTACK))
            add(s->text, &s->len, "  move %u: %s %s with %d trump%s (%s), stock %d\n", m, who, kind == BE_EV_ATTACK ? "attacks" : "covers",
                n_trump, n_trump > 1 ? "s" : "", cards, deck_count);
        else if (n_cards >= 3 && kind == BE_EV_ATTACK)
            add(s->text, &s->len, "  move %u: %s attacks with %d cards at once (%s), stock %d\n", m, who, n_cards, cards, deck_count);
        break;
    case BE_EV_PASS:
        if (seat >= 0) s->passes[seat]++;
        add(s->text, &s->len, "  move %u: %s passes the attack on with %s, stock %d\n", m, who, cards, deck_count);
        break;
    case BE_EV_PICKUP:
        if (seat >= 0) { s->pickups[seat]++; s->picked[seat] += (uint32_t)n_cards; }
        add(s->text, &s->len, "  move %u: %s picks up %d card%s%s%s%s, stock %d\n", m, who, n_cards, n_cards == 1 ? "" : "s",
            n_cards >= 4 ? " (a big pickup)" : "", cl ? ": " : "", cards, deck_count);
        break;
    case BE_EV_OUT:
        if (s->n_out < 2 && seat >= 0) s->out_order[s->n_out++] = (int8_t)seat;
        add(s->text, &s->len, "  move %u: %s goes out\n", m, who);
        break;
    default: break;
    }
}

bool be_cup_story_write(be_cup_story *s, const char *const names[2], int fool, int game_no, const char *path) {
    FILE *f = fopen(path, "a");
    if (!f) return false;
    fprintf(f, "Game %d: %s (seat 1) v %s (seat 2)\n", game_no, names[0], names[1]);
    fputs(s->text, f);
    if (fool >= 0 && fool < 2) fprintf(f, "  Fool: %s. Winner: %s.\n", names[fool], names[1 - fool]);
    else fprintf(f, "  A draw: both went out together.\n");
    fprintf(f, "  Moves: %u (%s %u, %s %u). Pickups: %s %u (%u cards), %s %u (%u cards). Passes: %s %u, %s %u. Trumps played: %s %u, %s %u.\n",
            s->n_moves, names[0], s->seat_moves[0], names[1], s->seat_moves[1], names[0], s->pickups[0], s->picked[0], names[1],
            s->pickups[1], s->picked[1], names[0], s->passes[0], names[1], s->passes[1], names[0], s->trumps[0], names[1], s->trumps[1]);
    fprintf(f, "  Every move (A attack or throw-in, C cover, P pass, T take, O out):\n    %s\n\n", s->moves);
    bool ok = !ferror(f);
    return fclose(f) == 0 && ok;
}

bool be_cup_summary_write(const be_w_cup *c, int m, const char *const *names, const char *games_path, const char *out_path) {
    if (m < 0 || m >= BE_CUP_MATCHES) return false;
    const be_w_match *x = &c->matches[m];
    const char *na = names[x->a], *nb = names[x->b];
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", out_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    if (m < BE_CUP_ROBIN) fprintf(f, "Bot Cup, round robin match %d of %d: %s v %s, best of five\n", m + 1, BE_CUP_ROBIN, na, nb);
    else fprintf(f, "Bot Cup FINAL: %s v %s, best of five\n", na, nb);
    fprintf(f, "Two players, passing game (perevodnoy), 36 cards. %s sits first in games 1, 3, 5; %s in games 2, 4.\n\n", na, nb);
    FILE *g = fopen(games_path, "r");
    if (g) {
        char buf[4096];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, g)) > 0) fwrite(buf, 1, k, f);
        fclose(g);
    }
    fprintf(f, "Result: %s %d, %s %d. Winner: %s.\n", na, x->wins_a, nb, x->wins_b, x->winner == x->a ? na : nb);
    fprintf(f, "Game by game:");
    for (int i = 0; i < x->games && i < BE_TOUR_MAX_GAMES; i++) {
        uint8_t r = c->result[m * BE_TOUR_MAX_GAMES + i];
        fprintf(f, " %d:%s", i + 1, r == BE_CUP_G_A ? na : r == BE_CUP_G_B ? nb : "draw");
    }
    fprintf(f, "\n");
    if (m < BE_CUP_ROBIN) {
        fprintf(f, "\nStandings after this match (match wins, game wins-losses):\n");
        for (int i = 0; i < BE_CUP_BOTS; i++) {
            int k = c->order[i];
            fprintf(f, "  %d. %s %d-%d, games %d-%d\n", i + 1, names[c->bots[k]], c->mw[k], c->ml[k], c->gw[k], c->gl[k]);
        }
    }
    bool ok = !ferror(f);
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, out_path) == 0;
}

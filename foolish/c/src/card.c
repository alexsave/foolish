// card.c - card notation (card.h): the one reader of a card written as text.
#include "card.h"

#include <stddef.h>

static bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static bool is_sep(char c) { return is_space(c) || c == ','; }

// The index of c in `set` (lower case letters and digits), matching a letter in
// either case; -1 when absent.
static int index_in(const char *set, char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    for (int i = 0; set[i]; i++)
        if (set[i] == c) return i;
    return -1;
}

// One card starting exactly at s[*at]. 0 with *at just past the suit, or
// CARD_PARSE_E_*; *out is written only on success.
static int read_card(const char *s, int len, int *at, Card *out) {
    int i = *at, value;
    if (i >= len) return CARD_PARSE_E_EMPTY;
    if (s[i] == '1') {
        if (i + 1 >= len || s[i + 1] != '0') return CARD_PARSE_E_RANK;
        value = 9;
        i += 2;
    } else {
        // "2" is value 1 and "a" is ACE_VALUE: the Card numbering is the index.
        const int r = index_in("23456789tjqka", s[i]);
        if (r < 0) return CARD_PARSE_E_RANK;
        value = r + 1;
        i += 1;
    }
    // The suit letters in SUIT_* order.
    const int suit = i < len ? index_in("shcd", s[i]) : -1;
    if (suit < 0) return CARD_PARSE_E_SUIT;
    out->suit = (int8_t)suit;
    out->value = (int8_t)value;
    *at = i + 1;
    return 0;
}

int card_parse(const char *s, int len, Card *out) {
    // One reader: a list of exactly one card, with no comma anywhere.
    for (int i = 0; s && i < len; i++)
        if (s[i] == ',') return CARD_PARSE_E_SYNTAX;
    Card c;
    const int n = card_list_parse(s, len, &c, NULL, 1);
    if (n == 0) return CARD_PARSE_E_EMPTY;
    if (n < 0) return n == CARD_PARSE_E_CAP ? CARD_PARSE_E_SYNTAX : n;
    *out = c;
    return 0;
}

int card_list_parse(const char *s, int len, Card *out, Card *covers, int cap) {
    if (len < 0 || (!s && len > 0)) return CARD_PARSE_E_EMPTY;
    int at = 0, n = 0;
    for (;;) {
        while (at < len && is_sep(s[at])) at++;
        if (at == len) return n;
        if (n >= cap) return CARD_PARSE_E_CAP;
        // An item is one card, or (in a battle list) two joined by '/'.
        Card pair[2] = { CARD_NONE, CARD_NONE };
        for (int k = 0;; k++) {
            if (at == len || is_sep(s[at]) || s[at] == '/') return CARD_PARSE_E_EMPTY;
            const int rc = read_card(s, len, &at, &pair[k]);
            if (rc != 0) return rc;
            if (at == len || s[at] != '/') break;
            if (!covers || k == 1) return CARD_PARSE_E_SYNTAX;
            at++;
        }
        if (at < len && !is_sep(s[at])) return CARD_PARSE_E_SYNTAX;
        out[n] = pair[0];
        if (covers) covers[n] = pair[1];
        n++;
    }
}

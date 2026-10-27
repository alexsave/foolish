/* chat.c - see chat.h. */
#include "chat.h"

#include <stdint.h>

/* ---- a line ---- */

/* A code point that is a control for a chat line (chat.h). */
static bool control(uint32_t c) {
    return c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029 || (c >= 0x202a && c <= 0x202e) ||
           (c >= 0x2066 && c <= 0x2069);
}

int cc_chat_valid(const char *s, int n) {
    if (n <= 0) return CC_CHAT_EMPTY;
    if (n > CC_CHAT_MAX) return CC_CHAT_LONG;
    const uint8_t *p = (const uint8_t *)s;
    bool seen = false;   /* a byte other than a space */
    for (int i = 0; i < n;) {
        uint32_t c = p[i];
        int k;   /* continuation bytes */
        uint32_t min;
        if (c < 0x80) { k = 0; min = 0; }
        else if ((c & 0xe0) == 0xc0) { k = 1; min = 0x80; c &= 0x1f; }
        else if ((c & 0xf0) == 0xe0) { k = 2; min = 0x800; c &= 0x0f; }
        else if ((c & 0xf8) == 0xf0) { k = 3; min = 0x10000; c &= 0x07; }
        else return CC_CHAT_UTF8;
        if (k && i + k >= n) return CC_CHAT_UTF8;   /* cut short */
        for (int j = 1; j <= k; j++) {
            if ((p[i + j] & 0xc0) != 0x80) return CC_CHAT_UTF8;
            c = (c << 6) | (p[i + j] & 0x3f);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return CC_CHAT_UTF8;
        if (control(c)) return CC_CHAT_CONTROL;
        if (c != ' ') seen = true;
        i += 1 + k;
    }
    return seen ? CC_CHAT_OK : CC_CHAT_EMPTY;
}

/* ---- the filter ---- */

void cc_chat_normal(const char *s, int n, char *out) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        switch (c) {
        case '0': c = 'o'; break;
        case '1': c = 'i'; break;
        case '3': c = 'e'; break;
        case '4': c = 'a'; break;
        case '5': c = 's'; break;
        case '7': c = 't'; break;
        case '@': c = 'a'; break;
        case '$': c = 's'; break;
        default: break;
        }
        out[i] = c >= 'a' && c <= 'z' ? c : ' ';
    }
}

enum { WHOLE, START, ANY };   /* where a stem may sit in a word */

/* THE BANNED STEMS. Their purpose is to keep a chat between strangers free of the slurs and of the
 * plain abuse a moderator would remove by hand, and nothing more: a word that is also innocent
 * inside other words sits as WHOLE (or START), so the filter never refuses a line about a class, a
 * cocktail, Scunthorpe or Niger. Written as they are spelled (a double letter needs a double:
 * chat.h); lowercase letters only. */
static const struct { const char *stem; uint8_t where; } BANNED[] = {
    { "fuck", ANY },     { "shit", ANY },     { "nigger", ANY },   { "nigga", ANY },   { "faggot", ANY },
    { "cunt", START },   { "bitch", START },   { "whore", START }, { "slut", START },
    { "retard", START }, { "twat", START },   { "wank", START },   { "tranny", START }, { "asshole", START },
    { "dickhead", START }, { "rapist", START },
    { "ass", WHOLE },    { "dick", WHOLE },   { "dicks", WHOLE }, { "cock", WHOLE },
    { "cocks", WHOLE },  { "fag", WHOLE },    { "fags", WHOLE },   { "kike", WHOLE },  { "kikes", WHOLE },
    { "spic", WHOLE },   { "spics", WHOLE },  { "chink", WHOLE },  { "chinks", WHOLE }, { "kys", WHOLE },
    { "rape", WHOLE },   { "raped", WHOLE },  { "pussy", WHOLE },
};
#define N_BANNED ((int)(sizeof BANNED / sizeof *BANNED))

typedef struct { char c; uint8_t n; } run;   /* a letter and how many times in a row */

static int runs_of(const char *s, int n, run *out) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && out[k - 1].c == s[i]) { if (out[k - 1].n < 255) out[k - 1].n++; }
        else out[k++] = (run){ s[i], 1 };
    }
    return k;
}

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Whether the stem's runs sit in the word's at `at`: each letter the same, at least as many. */
static bool runs_at(const run *w, const run *st, int ns, int at) {
    for (int j = 0; j < ns; j++)
        if (w[at + j].c != st[j].c || w[at + j].n < st[j].n) return false;
    return true;
}

static bool word_banned(const char *w, int n) {
    run wr[CC_CHAT_MAX], sr[16];
    int nw = runs_of(w, n, wr);
    for (int b = 0; b < N_BANNED; b++) {
        int ns = runs_of(BANNED[b].stem, slen(BANNED[b].stem), sr);
        if (ns > nw) continue;
        int last = BANNED[b].where == ANY ? nw - ns : 0;
        if (BANNED[b].where == WHOLE && ns != nw) continue;
        for (int at = 0; at <= last; at++)
            if (runs_at(wr, sr, ns, at)) return true;
    }
    return false;
}

bool cc_chat_banned(const char *s, int n) {
    if (n > CC_CHAT_MAX) n = CC_CHAT_MAX;
    if (n <= 0) return false;
    char t[CC_CHAT_MAX];
    cc_chat_normal(s, n, t);
    for (int i = 0; i < n;) {
        if (t[i] == ' ') { i++; continue; }
        int j = i;
        while (j < n && t[j] != ' ') j++;
        if (word_banned(t + i, j - i)) return true;
        i = j;
    }
    return false;
}

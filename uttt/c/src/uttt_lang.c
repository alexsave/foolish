#include "uttt_lang.h"
#include "../../../shared/c/i18n/languages.h"
#include <string.h>

/* One table per language, each in its own uttt/c/i18n/strings_<code>.c. */
#define UT_TABLE(up) extern const char *const UT_STRINGS_##up[UT_K_COUNT];
UT_TABLE(EN)
#ifndef UTTT_LANG_ENGLISH_ONLY
UT_TABLE(RU) UT_TABLE(KO) UT_TABLE(ZH) UT_TABLE(VI) UT_TABLE(ES)
UT_TABLE(PT) UT_TABLE(FR) UT_TABLE(DE) UT_TABLE(IT) UT_TABLE(JA)
UT_TABLE(PL) UT_TABLE(UK) UT_TABLE(TR) UT_TABLE(ID) UT_TABLE(TH)
UT_TABLE(NL) UT_TABLE(SV) UT_TABLE(DA) UT_TABLE(NO) UT_TABLE(FI)
UT_TABLE(CS) UT_TABLE(RO) UT_TABLE(HE) UT_TABLE(AR)
#endif
#undef UT_TABLE

/* INDEXED BY THE SHARED REGISTRY'S ENUM, so a language added there and not
 * here is a NULL row that tests/uttt_lang_test.c names, not a table that is
 * quietly one short.
 *
 * UTTT_LANG_ENGLISH_ONLY is the replay page's build (uttt/c/Makefile
 * wasm-web): the page is English, and twenty-four tables it never reads
 * would be downloaded by every visitor. Its other rows are NULL and read as
 * English, the fallback every hole takes. */
static const char *const *const TABLE[FS_L_COUNT] = {
    [FS_L_EN] = UT_STRINGS_EN,
#ifndef UTTT_LANG_ENGLISH_ONLY
    [FS_L_RU] = UT_STRINGS_RU, [FS_L_KO] = UT_STRINGS_KO, [FS_L_ZH] = UT_STRINGS_ZH,
    [FS_L_VI] = UT_STRINGS_VI, [FS_L_ES] = UT_STRINGS_ES, [FS_L_PT] = UT_STRINGS_PT,
    [FS_L_FR] = UT_STRINGS_FR, [FS_L_DE] = UT_STRINGS_DE, [FS_L_IT] = UT_STRINGS_IT,
    [FS_L_JA] = UT_STRINGS_JA, [FS_L_PL] = UT_STRINGS_PL, [FS_L_UK] = UT_STRINGS_UK,
    [FS_L_TR] = UT_STRINGS_TR, [FS_L_ID] = UT_STRINGS_ID, [FS_L_TH] = UT_STRINGS_TH,
    [FS_L_NL] = UT_STRINGS_NL, [FS_L_SV] = UT_STRINGS_SV, [FS_L_DA] = UT_STRINGS_DA,
    [FS_L_NO] = UT_STRINGS_NO, [FS_L_FI] = UT_STRINGS_FI, [FS_L_CS] = UT_STRINGS_CS,
    [FS_L_RO] = UT_STRINGS_RO, [FS_L_HE] = UT_STRINGS_HE, [FS_L_AR] = UT_STRINGS_AR,
#endif
};

#define UT_KEY_NAME(name, max, empty) #name,
static const char *const KEY_NAME[UT_K_COUNT] = { UT_KEYS(UT_KEY_NAME) };
#undef UT_KEY_NAME
#define UT_KEY_MAX(name, max, empty) max,
static const unsigned char KEY_MAX[UT_K_COUNT] = { UT_KEYS(UT_KEY_MAX) };
#undef UT_KEY_MAX
#define UT_KEY_EMPTY(name, max, empty) empty,
static const unsigned char KEY_EMPTY[UT_K_COUNT] = { UT_KEYS(UT_KEY_EMPTY) };
#undef UT_KEY_EMPTY

static int current = FS_L_EN;

int  uttt_lang(void) { return current; }
void uttt_lang_set(int lang) { current = lang >= 0 && lang < FS_L_COUNT ? lang : FS_L_EN; }
int  uttt_lang_count(void) { return FS_L_COUNT; }

const char *uttt_lang_code(int lang)
{
    return lang >= 0 && lang < FS_L_COUNT ? FS_LANGUAGES[lang].code : "";
}

int uttt_lang_rtl(int lang)
{
    return lang >= 0 && lang < FS_L_COUNT && FS_LANGUAGES[lang].rtl;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

/* One tag's language subtag (up to the first '-' or '_'), against the
 * registry's codes and the four that are not their own name. */
static int match_one(const char *t, int n)
{
    char sub[4];
    int k = 0;
    while (k < n && t[k] != '-' && t[k] != '_') k++;
    if (k < 2 || k > 3) return -1;
    for (int i = 0; i < k; i++) sub[i] = (char)lower((unsigned char)t[i]);
    sub[k] = 0;
    if (!strcmp(sub, "nb") || !strcmp(sub, "nn")) return FS_L_NO;
    if (!strcmp(sub, "in")) return FS_L_ID;
    if (!strcmp(sub, "iw")) return FS_L_HE;
    for (int l = 0; l < FS_L_COUNT; l++)
        if (!strcmp(sub, FS_LANGUAGES[l].code)) return l;
    return -1;
}

int uttt_lang_prefer(const char *tags)
{
    int hit = -1;
    for (const char *p = tags; p && *p && hit < 0;) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        if (e > p) hit = match_one(p, (int)(e - p));
        p = e;
    }
    uttt_lang_set(hit < 0 ? FS_L_EN : hit);
    return current;
}

const char *uttt_text_in(int lang, int key)
{
    if (lang < 0 || lang >= FS_L_COUNT || key < 0 || key >= UT_K_COUNT || !TABLE[lang]) return 0;
    return TABLE[lang][key];
}

const char *uttt_text(int key)
{
    if (key < 0 || key >= UT_K_COUNT) return "";
    const char *s = uttt_text_in(current, key);
    if (!s) s = UT_STRINGS_EN[key];
    return s ? s : "";
}

const char *uttt_key_name(int key) { return key >= 0 && key < UT_K_COUNT ? KEY_NAME[key] : ""; }
int uttt_key_max(int key) { return key >= 0 && key < UT_K_COUNT ? KEY_MAX[key] : 0; }
int uttt_key_may_be_empty(int key) { return key >= 0 && key < UT_K_COUNT && KEY_EMPTY[key]; }

enum { ONE = 0, FEW = 1, MANY = 2, OTHER = 3 };

int uttt_plural(int lang, int n)
{
    if (n < 0) n = -n;
    int m10 = n % 10, m100 = n % 100;
    int slavic_few = m10 >= 2 && m10 <= 4 && !(m100 >= 12 && m100 <= 14);
    switch (lang) {
    case FS_L_RU: case FS_L_UK:
        if (m10 == 1 && m100 != 11) return ONE;
        return slavic_few ? FEW : MANY;
    case FS_L_PL:
        if (n == 1) return ONE;
        return slavic_few ? FEW : MANY;
    case FS_L_CS:
        if (n == 1) return ONE;
        return n >= 2 && n <= 4 ? FEW : OTHER;
    case FS_L_RO:
        if (n == 1) return ONE;
        return n == 0 || (m100 >= 1 && m100 <= 19) ? FEW : OTHER;   /* 101 mutări, 20 de mutări */
    case FS_L_AR:
        if (n == 1) return ONE;
        if (m100 >= 3 && m100 <= 10) return FEW;
        if (m100 >= 11 && m100 <= 99) return MANY;
        return OTHER;
    case FS_L_FR: case FS_L_PT:
        return n <= 1 ? ONE : OTHER;
    case FS_L_JA: case FS_L_ZH: case FS_L_KO: case FS_L_VI: case FS_L_TH: case FS_L_ID:
        return OTHER;
    default:
        return n == 1 ? ONE : OTHER;
    }
}

/* Decode one UTF-8 character at `s`; its length in `*len`. */
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
    if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x0591 && c <= 0x05c7) ||
        (c >= 0x064b && c <= 0x065f) || c == 0x0670 || c == 0x0e31 ||
        (c >= 0x0e34 && c <= 0x0e3a) || (c >= 0x0e47 && c <= 0x0e4e) ||
        c == 0x200b || c == 0x200e || c == 0x200f)
        return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xfe30 && c <= 0xfe4f) || (c >= 0xff00 && c <= 0xff60) ||
        (c >= 0xffe0 && c <= 0xffe6))
        return 2;
    return 1;
}

int uttt_text_cols(const char *s)
{
    int cols = 0;
    for (const unsigned char *p = (const unsigned char *)s; p && *p;) {
        int n;
        cols += cp_cols(next_cp(p, &n));
        p += n;
    }
    return cols;
}

int uttt_fill(char *out, int cap, const char *t, const char *const *kv)
{
    if (!out || cap < 1 || !t) return -1;
    int o = 0;
    for (const char *p = t; *p;) {
        const char *v = 0;
        int skip = 1;
        if (*p == '{') {
            const char *e = p + 1;
            while (*e && *e != '}' && *e != '{') e++;
            if (*e == '}')
                for (int i = 0; kv && kv[i]; i += 2)
                    if ((size_t)(e - p - 1) == strlen(kv[i]) && !strncmp(p + 1, kv[i], (size_t)(e - p - 1))) {
                        v = kv[i + 1] ? kv[i + 1] : "";
                        skip = (int)(e - p) + 1;
                        break;
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

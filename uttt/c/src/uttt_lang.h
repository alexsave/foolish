/* WHICH LANGUAGE THE KERNEL SPEAKS, and its words by key.
 *
 * The keys and the per-language tables are uttt/c/i18n (keys.h says why they
 * are compiled in); the languages are shared/c/i18n/languages.h's FS_L_*, the
 * sister product's list, so the two cannot carry different sets.
 *
 * ONE LANGUAGE PER PROCESS, set once by the host from the phone's own ordered
 * preference (uttt_lang_prefer) and English until it is. A bubble is composed
 * on the SENDER's phone in the sender's language and read identically on
 * every phone - the same rule the sister product's captions follow - so the
 * kernel never needs a second language at once. */
#ifndef UTTT_LANG_H
#define UTTT_LANG_H

#include "../i18n/keys.h"

int         uttt_lang(void);                /* the current FS_L_*, English (0) by default */
void        uttt_lang_set(int lang);        /* out of range is English                    */
int         uttt_lang_count(void);
const char *uttt_lang_code(int lang);       /* "en", "ru", ...; "" out of range           */
int         uttt_lang_rtl(int lang);

/* THE PHONE DECIDES: `tags` is its ordered preference list, comma separated
 * ("de-CH,fr-CH,en-US"), and the FIRST tag whose language subtag this table
 * carries wins, so a phone set to Catalan then Spanish reads Spanish, not
 * English. English is the floor, not the second choice. The subtags that are
 * not their own name are mapped (nb and nn are Norwegian, in Indonesian, iw
 * Hebrew), exactly as the sister product's resolver does. Sets the language
 * and returns it. */
int         uttt_lang_prefer(const char *tags);

/* Key `key` in the current language; English where a language has a hole
 * (tests/uttt_lang_test.c fails the build on one first); "" for an unknown
 * key. Never NULL. */
const char *uttt_text(int key);

/* The raw table cell: NULL for a hole or out of range. For tests. */
const char *uttt_text_in(int lang, int key);
const char *uttt_key_name(int key);         /* "CAP_NEW_GAME"; "" out of range */
int         uttt_key_max(int key);          /* its width limit in columns, 0 none */
int         uttt_key_may_be_empty(int key);

/* THE PLURAL CATEGORY OF `n` in `lang`, as an offset from UT_K_MOVES_ONE:
 * ONE, FEW, MANY or OTHER (CLDR's cardinal rules for the languages carried;
 * a language's zero and two fall to OTHER, and no game ends in two moves). */
int         uttt_plural(int lang, int n);

/* HOW WIDE A STRING SETS, in columns: a wide East Asian character (CJK,
 * kana, Hangul, full-width forms) is two, a combining mark (Thai vowels and
 * tones, Hebrew points, Arabic harakat, Latin combining accents) none, and
 * every other character one. Bytes are no measure - Cyrillic is two a
 * character and as wide as Latin - and characters are not either. */
int         uttt_text_cols(const char *s);

/* {name} substitution: `kv` is name, value pairs ending in NULL. A
 * placeholder not in `kv` is copied as written. Returns the length, or -1
 * for a buffer too small. */
int         uttt_fill(char *out, int cap, const char *tmpl, const char *const *kv);

#endif

/* chat.h - what a chat line may hold, and the word filter, in the kernel so the browser shows the
 * same answer the server gives before a line is even sent.
 *
 * A LINE is 1..CC_CHAT_MAX bytes of well-formed UTF-8 (no overlong form, no surrogate, nothing past
 * U+10FFFF) with no control character: none of C0 (below U+0020), DEL, C1 (U+0080..U+009F), the
 * line and paragraph separators (U+2028, U+2029) and the bidirectional overrides and isolates
 * (U+202A..U+202E, U+2066..U+2069), which would let a line reorder the text after it on the page.
 * A line of spaces only is empty.
 *
 * THE FILTER looks at each word of a line after normalising it: ASCII letters lowercased, the
 * usual stand-ins read as the letters they stand for (0 o, 1 i, 3 e, 4 a, 5 s, 7 t, @ a, $ s), and
 * everything else (spaces, punctuation, other digits, any non-ASCII byte) a break between words.
 * A repeated letter matches a single one ("fuuuck" is "fuck"), but not the other way round: a
 * banned word spelled with a double letter needs at least two ("ass" is banned, "as" is not, and
 * "niger" is the country). Each banned stem says where it may sit: the whole word only (a stem
 * that is also inside innocent words: "ass" in "class", "cock" in "cocktail"), the start of a word
 * ("cunt" but not "Scunthorpe"), or anywhere inside one. Not exhaustive and not meant to be: it
 * catches the slurs and the common profanity, typed plainly or lightly disguised. Spaced-out
 * letters and look-alike letters from other scripts pass. */
#ifndef C_CHESS_CHAT_H
#define C_CHESS_CHAT_H

#include <stdbool.h>

#define CC_CHAT_MAX 140

/* why a line is not a line: cc_chat_valid's answer */
enum { CC_CHAT_OK = 0, CC_CHAT_EMPTY = 1, CC_CHAT_LONG = 2, CC_CHAT_UTF8 = 3, CC_CHAT_CONTROL = 4 };

/* Whether n bytes at s are a line: CC_CHAT_OK, or why not. */
int  cc_chat_valid(const char *s, int n);
/* Whether a line holds a banned word (the line need not be valid; n at most CC_CHAT_MAX is read). */
bool cc_chat_banned(const char *s, int n);
/* The normal form the filter reads: n bytes of s into out (n bytes, no NUL), each byte a lowercase
 * letter or a space. */
void cc_chat_normal(const char *s, int n, char *out);

#endif

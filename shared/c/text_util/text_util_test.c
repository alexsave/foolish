/* text_util_test.c - the low-level text under a say layer (text_util.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror text_util.c text_util_test.c -o text_util_test && ./text_util_test
 *
 * No -I: the header is beside this file. Exits 1 on any failure. The buffers
 * the writers are handed at their capacity are heap blocks of exactly that
 * size, so a build under ASan catches a write one byte past the end. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "text_util.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", what); } } while (0)

static int cp_is(const char *s, unsigned want, int want_len)
{
    int len = -1;
    return text_next_cp((const unsigned char *)s, &len) == want && len == want_len;
}

/* A fresh heap block of exactly `cap` bytes, filled with 'x'. */
static char *block(int cap)
{
    char *b = malloc((size_t)(cap > 0 ? cap : 1));
    if (!b) { printf("FAIL out of memory\n"); exit(1); }
    memset(b, 'x', (size_t)(cap > 0 ? cap : 1));
    return b;
}

int main(void)
{
    /* STEPPING: one to four bytes, and a cut-short sequence never steps over
     * what cut it. */
    OK(cp_is("A", 'A', 1), "next_cp: ASCII is one byte");
    OK(cp_is("\xc3\xa9", 0xe9, 2), "next_cp: e-acute is two bytes");
    OK(cp_is("\xd0\x96", 0x416, 2), "next_cp: Cyrillic Zhe is two bytes");
    OK(cp_is("\xe4\xb8\xad", 0x4e2d, 3), "next_cp: a CJK ideograph is three bytes");
    OK(cp_is("\xf0\x9f\x98\x80", 0x1f600, 4), "next_cp: an emoji is four bytes");
    OK(cp_is("\xe4\xb8", 0xfffd, 2), "next_cp: a three-byte lead cut by the NUL stops at the NUL");
    OK(cp_is("\xf0\x9f\x98", 0xfffd, 3), "next_cp: a four-byte lead cut by the NUL stops at the NUL");
    OK(cp_is("\xc3", 0xfffd, 1), "next_cp: a lone two-byte lead is U+FFFD, one byte");
    OK(cp_is("\xe4" "A", 0xfffd, 1), "next_cp: a lead followed by ASCII leaves the ASCII unread");
    OK(cp_is("\x80", 0xfffd, 1), "next_cp: a stray continuation byte before the NUL is U+FFFD");

    /* WIDTHS, exactly the table the string limits were checked against. */
    OK(text_cp_cols('a') == 1 && text_cp_cols(' ') == 1, "cp_cols: ASCII is one");
    OK(text_cp_cols(0x416) == 1, "cp_cols: Cyrillic is one");
    OK(text_cp_cols(0x301) == 0 && text_cp_cols(0x300) == 0 && text_cp_cols(0x36f) == 0,
       "cp_cols: a combining mark is none");
    OK(text_cp_cols(0x370) == 1, "cp_cols: the first letter past the combining block is one");
    OK(text_cp_cols(0x200b) == 0 && text_cp_cols(0x200d) == 0 && text_cp_cols(0x200e) == 0 &&
       text_cp_cols(0x200f) == 0, "cp_cols: zero-width space, joiner and the direction marks are none");
    OK(text_cp_cols(0x200c) == 1, "cp_cols: the non-joiner is one, as the table has it");
    OK(text_cp_cols(0xfe0f) == 0 && text_cp_cols(0xfe00) == 0, "cp_cols: a variation selector is none");
    OK(text_cp_cols(0x05b4) == 1, "cp_cols: a Hebrew point is one, as the table has it");
    OK(text_cp_cols(0x1100) == 2 && text_cp_cols(0x115f) == 2 && text_cp_cols(0x1160) == 1,
       "cp_cols: Hangul leading jamo are two, the vowels after them one");
    OK(text_cp_cols(0x4e2d) == 2 && text_cp_cols(0x3042) == 2, "cp_cols: CJK and kana are two");
    OK(text_cp_cols(0xac00) == 2 && text_cp_cols(0xd7a3) == 2 && text_cp_cols(0xd7a4) == 1,
       "cp_cols: Hangul syllables are two, to the block's end");
    OK(text_cp_cols(0xff01) == 2 && text_cp_cols(0xff60) == 2 && text_cp_cols(0xff61) == 1,
       "cp_cols: fullwidth forms are two, halfwidth one");
    OK(text_cp_cols(0x1f600) == 2 && text_cp_cols(0x1f300) == 2 && text_cp_cols(0x1f2ff) == 1,
       "cp_cols: from U+1F300 up is two, below it one");
    OK(text_cp_cols(0xfffd) == 1, "cp_cols: the replacement character is one");

    OK(text_cols("X to play") == 9, "cols: ASCII is one a character");
    OK(text_cols("\xd0\xa5\xd0\xbe\xd0\xb4") == 3, "cols: Cyrillic is one a character, not two bytes");
    OK(text_cols("\xe4\xb8\xad\xe6\x96\x87") == 4, "cols: two ideographs are four");
    OK(text_cols("e\xcc\x81") == 1, "cols: e and a combining acute are one");
    OK(text_cols("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd") == 4, "cols: an emoji and its skin tone are two each");
    OK(text_cols("\xe2\x9d\xa4\xef\xb8\x8f") == 1, "cols: a heart below U+1F300 is one and its selector none");
    OK(text_cols("a\xe4\xb8") == 2, "cols: a cut-short tail is one column");
    OK(text_cols("") == 0 && text_cols(NULL) == 0, "cols: nothing is nothing");

    /* NUMBERS. */
    char n[16];
    OK(text_itoa(0, n, sizeof n) == 1 && !strcmp(n, "0"), "itoa: 0");
    OK(text_itoa(7, n, sizeof n) == 1 && !strcmp(n, "7"), "itoa: one digit");
    OK(text_itoa(-305, n, sizeof n) == 4 && !strcmp(n, "-305"), "itoa: a negative");
    OK(text_itoa(-1, n, sizeof n) == 2 && !strcmp(n, "-1"), "itoa: -1");
    OK(text_itoa(INT_MAX, n, sizeof n) == 10 && !strcmp(n, "2147483647"), "itoa: INT_MAX");
    OK(text_itoa(INT_MIN, n, sizeof n) == 11 && !strcmp(n, "-2147483648"), "itoa: INT_MIN");
    OK(text_itoa(12, n, 2) == -1, "itoa: no room for the NUL is refused");
    OK(text_itoa(12, NULL, 16) == -1, "itoa: no buffer is refused");
    {
        char *b = block(12);
        OK(text_itoa(INT_MIN, b, 12) == 11 && !strcmp(b, "-2147483648"), "itoa: INT_MIN into exactly 12 bytes");
        free(b);
        b = block(11);
        OK(text_itoa(INT_MIN, b, 11) == -1 && b[0] == 'x', "itoa: INT_MIN into 11 bytes is refused untouched");
        free(b);
        b = block(1);
        OK(text_itoa(0, b, 1) == -1, "itoa: 0 into one byte is refused");
        free(b);
        b = block(2);
        OK(text_itoa(0, b, 2) == 1 && !strcmp(b, "0"), "itoa: 0 into exactly two bytes");
        free(b);
    }

    /* COPIES. */
    {
        char *b = block(6);
        OK(text_put(b, 6, "hello") == 5 && !strcmp(b, "hello"), "put: a string into exactly its size");
        free(b);
        b = block(5);
        OK(text_put(b, 5, "hello") == -1 && b[0] == 'x', "put: one byte short is refused untouched");
        free(b);
        b = block(1);
        OK(text_put(b, 1, "") == 0 && b[0] == 0, "put: the empty string into one byte");
        free(b);
        OK(text_put(n, 0, "") == -1, "put: no room at all is refused");
        OK(text_put(NULL, 16, "a") == -1, "put: no buffer is refused");
    }

    /* TEMPLATES. */
    {
        const char *const kv[] = { "a", "xyz", "who", "Ann", "none", NULL, NULL };
        const char *const fb[] = { "game", "Crazy", "a", "never", NULL };
        char s[64];
        OK(text_fill(s, sizeof s, "<{a}{b}>", kv, NULL) == 8 && !strcmp(s, "<xyz{b}>"),
           "fill: a placeholder nobody fills stays written");
        OK(text_fill(s, sizeof s, "{who} plays {game}", kv, fb) == 15 && !strcmp(s, "Ann plays Crazy"),
           "fill: a name the list lacks comes from the fallback");
        OK(text_fill(s, sizeof s, "{a}", kv, fb) == 3 && !strcmp(s, "xyz"), "fill: the list wins over the fallback");
        OK(text_fill(s, sizeof s, "{game}", kv, NULL) == 6 && !strcmp(s, "{game}"),
           "fill: with no fallback an unknown name stays written");
        OK(text_fill(s, sizeof s, "{game}", NULL, fb) == 5 && !strcmp(s, "Crazy"), "fill: a NULL list reads only the fallback");
        OK(text_fill(s, sizeof s, "[{none}]", kv, fb) == 2 && !strcmp(s, "[]"), "fill: a NULL value writes nothing");
        OK(text_fill(s, sizeof s, "{a{who}", kv, NULL) == 5 && !strcmp(s, "{aAnn"),
           "fill: a brace reopened before it closes stays written");
        OK(text_fill(s, sizeof s, "open {a", kv, NULL) == 7 && !strcmp(s, "open {a"), "fill: an unclosed brace stays written");
        OK(text_fill(s, sizeof s, "{}", kv, NULL) == 2 && !strcmp(s, "{}"), "fill: an empty name stays written");
        OK(text_fill(s, sizeof s, "", kv, NULL) == 0 && !strcmp(s, ""), "fill: the empty template");
        OK(text_fill(s, sizeof s, NULL, kv, NULL) == -1, "fill: no template is refused");
        OK(text_fill(NULL, 16, "a", kv, NULL) == -1, "fill: no buffer is refused");
        OK(text_fill(s, 0, "", kv, NULL) == -1, "fill: no room at all is refused");

        char *b = block(8);
        OK(text_fill(b, 8, "<{a}ok>", kv, NULL) == 7 && !strcmp(b, "<xyzok>"), "fill: exactly its size, a value inside");
        free(b);
        b = block(7);
        OK(text_fill(b, 7, "<{a}ok>", kv, NULL) == -1, "fill: one short at a literal is refused");
        free(b);
        b = block(4);
        OK(text_fill(b, 4, "{a}", kv, NULL) == 3 && !strcmp(b, "xyz"), "fill: a value that ends exactly at the end");
        free(b);
        b = block(3);
        OK(text_fill(b, 3, "{a}", kv, NULL) == -1, "fill: a value one short is refused");
        free(b);
        b = block(1);
        OK(text_fill(b, 1, "", kv, NULL) == 0 && b[0] == 0, "fill: the empty template into one byte");
        free(b);
    }

    printf("text_util: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}

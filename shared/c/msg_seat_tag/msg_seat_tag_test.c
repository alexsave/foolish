/* msg_seat_tag_test.c - who sits where (msg_seat_tag.h).
 *
 *   cc -std=c11 -Wall -Wextra -Werror msg_seat_tag.c msg_seat_tag_test.c -o msg_seat_tag_test && ./msg_seat_tag_test
 *
 * No -I: the header is beside this file. Exits 1 on any failure. The strict
 * UTF-8 counter on every sequence length and every way a sequence can be
 * wrong, the nickname verdict at its exact caps, the name comparison, and the
 * resolver's witnesses in order with each fallback and each gate. */
#include <stdio.h>
#include <string.h>
#include "msg_seat_tag.h"

static int fails, checks;
#define OK(c, what) do { checks++; if (!(c)) { fails++; printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

#define U(s) ((const uint8_t *)(s))
static int chars(const char *s) { return msg_seat_utf8_chars(U(s), (int)strlen(s)); }
static int verdict(const char *s) { return msg_seat_name_verdict(U(s), (int)strlen(s)); }

static void test_utf8(void)
{
    OK(chars("") == 0, "empty is zero code points");
    OK(chars("Alex") == 4, "ASCII: one byte each");
    OK(chars("\xC3\xAB") == 1, "two-byte U+00EB");
    OK(chars("\xC2\xA0") == 1, "U+00A0, the first code point past C1, is allowed");
    OK(chars("\xE4\xB8\x80") == 1, "three-byte U+4E00");
    OK(chars("\xE0\xA0\x80") == 1, "three-byte U+0800, the smallest");
    OK(chars("\xEF\xBF\xBF") == 1, "three-byte U+FFFF");
    OK(chars("\xF0\x9F\x98\x80") == 1, "four-byte U+1F600");
    OK(chars("\xF0\x90\x80\x80") == 1, "four-byte U+10000, the smallest");
    OK(chars("\xF4\x8F\xBF\xBF") == 1, "four-byte U+10FFFF, the largest");
    OK(chars("a\xC3\xAB\xE4\xB8\x80\xF0\x9F\x98\x80") == 4, "one of each length");
    /* truncated tails: a lead byte whose continuation runs past len */
    OK(chars("\xC3") == -1, "two-byte lead with no tail");
    OK(chars("\xE4\xB8") == -1, "three-byte sequence cut after two");
    OK(chars("\xF0\x9F\x98") == -1, "four-byte sequence cut after three");
    OK(msg_seat_utf8_chars(U("a\xF0\x9F\x98\x80"), 4) == -1, "a length that cuts the last sequence");
    /* a continuation byte that is not one */
    OK(chars("\xC3\x41") == -1, "two-byte lead then ASCII");
    OK(chars("\xE4\xB8\x41") == -1, "three-byte sequence with an ASCII third byte");
    OK(chars("\x80") == -1, "a bare continuation byte");
    OK(chars("\xBF") == -1, "a bare continuation byte, the highest");
    /* overlong forms */
    OK(chars("\xC0\xAF") == -1, "overlong '/' in two bytes (C0)");
    OK(chars("\xC1\xBF") == -1, "overlong in two bytes (C1)");
    OK(chars("\xE0\x80\xAF") == -1, "overlong '/' in three bytes");
    OK(chars("\xE0\x9F\xBF") == -1, "overlong U+07FF in three bytes");
    OK(chars("\xF0\x8F\xBF\xBF") == -1, "overlong U+FFFF in four bytes");
    /* out of range and surrogates */
    OK(chars("\xF4\x90\x80\x80") == -1, "U+110000, past the last code point");
    OK(chars("\xF5\x80\x80\x80") == -1, "lead byte F5");
    OK(chars("\xFF") == -1, "lead byte FF");
    OK(chars("\xED\xA0\x80") == -1, "U+D800, the first surrogate");
    OK(chars("\xED\xBF\xBF") == -1, "U+DFFF, the last surrogate");
    OK(chars("\xED\x9F\xBF") == 1, "U+D7FF, just below the surrogates");
    OK(chars("\xEE\x80\x80") == 1, "U+E000, just above the surrogates");
    /* control characters */
    OK(msg_seat_utf8_chars(U("a\0b"), 3) == -1, "NUL");
    OK(chars("a\x1F") == -1, "U+001F");
    OK(chars("a\x20") == 2, "U+0020, space, is allowed");
    OK(chars("a\x7F") == -1, "DEL");
    OK(chars("a\x7E") == 2, "U+007E is allowed");
    OK(chars("\xC2\x80") == -1, "U+0080, the first C1");
    OK(chars("\xC2\x9F") == -1, "U+009F, the last C1");
    OK(chars("\xE2\x80\x8B") == 1, "U+200B, a zero-width space, counts as a character");
}

static void test_verdict(void)
{
    static const char CJK16[] =
        "\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80"
        "\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80";
    static const char EMOJI12[] =
        "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"
        "\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80";
    char big[64];

    OK(MSG_SEAT_NAME_MAX_BYTES == 48 && MSG_SEAT_NAME_MAX_CHARS == 16, "the caps are 48 bytes and 16 code points");
    OK(msg_seat_name_verdict(NULL, 4) == MSG_SEAT_NAME_EMPTY, "NULL is empty");
    OK(msg_seat_name_verdict(U("Alex"), 0) == MSG_SEAT_NAME_EMPTY, "zero length is empty");
    OK(msg_seat_name_verdict(U("Alex"), -1) == MSG_SEAT_NAME_EMPTY, "a negative length is empty");
    OK(verdict("a") == MSG_SEAT_NAME_OK, "one character");
    OK(verdict("Alex") == MSG_SEAT_NAME_OK, "a name");
    OK(verdict(" Alex ") == MSG_SEAT_NAME_OK, "spaces are the host's to trim, not a refusal");
    OK(verdict("abcdefghijklmnop") == MSG_SEAT_NAME_OK, "16 code points is the cap");
    OK(verdict("abcdefghijklmnopq") == MSG_SEAT_NAME_TOO_LONG, "17 code points is too long");
    OK(strlen(CJK16) == 48 && verdict(CJK16) == MSG_SEAT_NAME_OK, "16 three-byte code points: 48 bytes, both caps exactly");
    memcpy(big, CJK16, 48); memcpy(big + 48, "a", 2);
    OK(verdict(big) == MSG_SEAT_NAME_TOO_LONG, "49 bytes is too long by bytes");
    OK(strlen(EMOJI12) == 48 && verdict(EMOJI12) == MSG_SEAT_NAME_OK, "12 four-byte code points: 48 bytes");
    memcpy(big, EMOJI12, 48); memcpy(big + 48, "\xF0\x9F\x98\x80", 5);
    OK(verdict(big) == MSG_SEAT_NAME_TOO_LONG, "13 four-byte code points: 52 bytes, too long by bytes");
    /* too long by bytes wins over bad bytes: the byte cap is checked first */
    memset(big, 0xFF, sizeof big);
    OK(msg_seat_name_verdict(U(big), 49) == MSG_SEAT_NAME_TOO_LONG, "49 bad bytes: too long, before any decoding");
    OK(msg_seat_name_verdict(U(big), 48) == MSG_SEAT_NAME_BAD, "48 bad bytes: bad");
    OK(verdict("Al\x01x") == MSG_SEAT_NAME_BAD, "a control character is bad");
    OK(verdict("Al\xC3") == MSG_SEAT_NAME_BAD, "a cut sequence is bad");
    OK(verdict("\xC0\xAF") == MSG_SEAT_NAME_BAD, "an overlong form is bad");
    OK(verdict("\xED\xA0\x80") == MSG_SEAT_NAME_BAD, "a surrogate is bad");
}

static void row(MsgSeat *r, uint8_t tag, const char *name)
{
    memset(r, 0, sizeof *r);
    memset(r->tag, tag, MSG_SEAT_TAG_LEN);
    r->name_len = (uint8_t)strlen(name);
    memcpy(r->name, name, r->name_len);
}

static void test_same_name(void)
{
    MsgSeat r;
    row(&r, 1, "Alex");
    OK(sizeof r == MSG_SEAT_TAG_LEN + 1 + MSG_SEAT_NAME_MAX_BYTES, "a row is 58 bytes, no padding");
    OK(msg_seat_same_name(&r, U("Alex"), 4), "the same bytes");
    OK(!msg_seat_same_name(&r, U("alex"), 4), "case matters");
    OK(!msg_seat_same_name(&r, U("ALEX"), 4), "case matters, all capitals");
    OK(!msg_seat_same_name(&r, U("Alex "), 5), "a trailing space is another name");
    OK(!msg_seat_same_name(&r, U("Ale"), 3), "a prefix is another name");
    OK(!msg_seat_same_name(&r, U("Alez"), 4), "one byte off at the end is another name");
    OK(!msg_seat_same_name(&r, U("Blex"), 4), "one byte off at the start is another name");
    OK(!msg_seat_same_name(&r, U("Alexa"), 5), "a longer name is another name");
    row(&r, 1, "Al\xC3\xABx");
    OK(msg_seat_same_name(&r, U("Al\xC3\xABx"), 5), "the same precomposed bytes");
    OK(!msg_seat_same_name(&r, U("Ale\xCC\x88x"), 6), "no normalization: decomposed is another name");
    row(&r, 1, "");
    OK(!msg_seat_same_name(&r, U(""), 0), "two empty names are never the same name");
}

/* A three-row roster: Alex, Bo, Cleo. */
static MsgSeat R[3];
static int res(int n, int started, int sender, int record, int tag, int dm, int sent, const char *name, int *by)
{
    *by = -9;
    return msg_seat_resolve(R, n, started, sender, record, tag, dm, sent, U(name), name ? (int)strlen(name) : 0, by);
}

static void test_resolve(void)
{
    int by;
    row(&R[0], 1, "Alex"); row(&R[1], 2, "Bo"); row(&R[2], 3, "Cleo");

    /* the order: record, then tag, then sender, then name */
    OK(res(3, 1, 2, 0, 1, 0, 1, "Cleo", &by) == 0 && by == MSG_SEAT_BY_RECORD, "the record beats every other witness");
    OK(res(3, 1, 2, -1, 1, 0, 1, "Cleo", &by) == 1 && by == MSG_SEAT_BY_TAG, "with no record the tag beats sender and name");
    OK(res(3, 1, 2, -1, -1, 0, 1, "Alex", &by) == 2 && by == MSG_SEAT_BY_SENDER, "with no record or tag the sender beats the name");
    OK(res(3, 1, -1, -1, -1, 0, 1, "Bo", &by) == 1 && by == MSG_SEAT_BY_NAME, "with no sender the name decides");
    OK(res(3, 1, -1, -1, -1, 0, 1, "Zed", &by) == -1 && by == MSG_SEAT_BY_NONE, "no witness: not seated");
    OK(res(3, 1, -1, -1, -1, 0, 1, NULL, &by) == -1 && by == MSG_SEAT_BY_NONE, "no name at all: not seated");
    OK(res(3, 1, -1, -1, -1, 0, 1, "", &by) == -1 && by == MSG_SEAT_BY_NONE, "an empty name is no name");
    OK(res(3, 1, -1, -1, -1, 0, 1, "alex", &by) == -1 && by == MSG_SEAT_BY_NONE, "the name witness is case-sensitive");

    /* out-of-range record and tag fall through */
    OK(res(3, 1, 2, 3, 1, 0, 1, NULL, &by) == 1 && by == MSG_SEAT_BY_TAG, "a record past the roster falls to the tag");
    OK(res(3, 1, 2, -1, 3, 0, 1, NULL, &by) == 2 && by == MSG_SEAT_BY_SENDER, "a tag past the roster falls to the sender");
    OK(res(2, 1, 1, 2, 2, 0, 1, NULL, &by) == 1 && by == MSG_SEAT_BY_SENDER, "the bounds are the row count given");

    /* a record that finds no row: only the tag may still speak */
    OK(res(3, 1, 2, MSG_SEAT_REC_GONE, 1, 0, 1, "Cleo", &by) == 1 && by == MSG_SEAT_BY_TAG, "gone record: the tag still counts");
    OK(res(3, 1, 2, MSG_SEAT_REC_GONE, -1, 0, 1, "Cleo", &by) == -1 && by == MSG_SEAT_BY_NONE, "gone record: the sender and the name do not");

    /* the sender witness */
    OK(res(3, 1, 2, -1, -1, 0, MSG_SEAT_SENT_UNKNOWN, NULL, &by) == -1 && by == MSG_SEAT_BY_NONE, "sender unknown: no inference");
    OK(res(3, 1, -1, -1, -1, 0, 1, NULL, &by) == -1 && by == MSG_SEAT_BY_NONE, "I sent a leave: the sender has no seat");
    OK(res(3, 1, 2, -1, -1, 1, 0, NULL, &by) == -1 && by == MSG_SEAT_BY_NONE, "not mine in a DM of three seats: no inference");
    OK(res(2, 1, 0, -1, -1, 1, 0, NULL, &by) == 1 && by == MSG_SEAT_BY_SENDER, "not mine in a two-seat DM: the other seat");
    OK(res(2, 1, 1, -1, -1, 1, 0, NULL, &by) == 0 && by == MSG_SEAT_BY_SENDER, "not mine in a two-seat DM: the other seat, the other way");
    OK(res(2, 1, 1, -1, -1, 0, 0, NULL, &by) == -1 && by == MSG_SEAT_BY_NONE, "not mine in a group of two: no inference");
    OK(res(2, 1, -1, -1, -1, 1, 0, NULL, &by) == -1 && by == MSG_SEAT_BY_NONE, "not mine in a DM, sender unseated: no inference");
    OK(res(2, 1, 0, -1, -1, 1, 0, "Alex", &by) == 1 && by == MSG_SEAT_BY_SENDER, "started: the sender beats my name");

    /* the lobby gate: before the start a sender seat must carry my name */
    OK(res(3, 0, 2, -1, -1, 0, 1, "Cleo", &by) == 2 && by == MSG_SEAT_BY_SENDER, "lobby: the sender seat carries my name");
    OK(res(3, 0, 2, -1, -1, 0, 1, "Bo", &by) == 1 && by == MSG_SEAT_BY_NAME, "lobby: another name gates the sender, the name decides");
    OK(res(3, 0, 2, -1, -1, 0, 1, "Zed", &by) == -1 && by == MSG_SEAT_BY_NONE, "lobby: a name on no row gates the sender out");
    OK(res(3, 0, 2, -1, -1, 0, 1, NULL, &by) == 2 && by == MSG_SEAT_BY_SENDER, "lobby: no name, the sender counts");
    OK(res(3, 0, 2, -1, -1, 0, 1, "", &by) == 2 && by == MSG_SEAT_BY_SENDER, "lobby: an empty name, the sender counts");
    OK(res(3, 1, 2, -1, -1, 0, 1, "Zed", &by) == 2 && by == MSG_SEAT_BY_SENDER, "started: no gate");

    /* duplicate names: the first row that carries it */
    row(&R[2], 3, "Bo");
    OK(res(3, 1, -1, -1, -1, 0, 1, "Bo", &by) == 1 && by == MSG_SEAT_BY_NAME, "two rows named Bo: the first");
    row(&R[2], 3, "Cleo");

    /* *by may be NULL */
    OK(msg_seat_resolve(R, 3, 1, 2, -1, -1, 0, 1, NULL, 0, NULL) == 2, "by may be NULL");
}

int main(void)
{
    test_utf8();
    test_verdict();
    test_same_name();
    test_resolve();
    printf("msg_seat_tag: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}

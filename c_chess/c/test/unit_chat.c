// unit_chat.c - what a chat line may hold (1..140 bytes of UTF-8 with no control character) and
// the word filter (normalised words against the banned stems, each stem a whole word, a word's
// start or anywhere inside one).
#include "chat.h"
#include "t.h"

#include <string.h>

static int valid(const char *s) { return cc_chat_valid(s, (int)strlen(s)); }
static bool banned(const char *s) { return cc_chat_banned(s, (int)strlen(s)); }

static void lengths(void) {
    char buf[CC_CHAT_MAX + 2];
    memset(buf, 'a', sizeof buf);
    TEST("1 to 140 bytes");
    CHECK(cc_chat_valid(buf, 1) == CC_CHAT_OK, "one byte: %d", cc_chat_valid(buf, 1));
    CHECK(cc_chat_valid(buf, CC_CHAT_MAX) == CC_CHAT_OK, "140: %d", cc_chat_valid(buf, CC_CHAT_MAX));
    CHECK(cc_chat_valid(buf, CC_CHAT_MAX + 1) == CC_CHAT_LONG, "141: %d", cc_chat_valid(buf, CC_CHAT_MAX + 1));
    CHECK(cc_chat_valid(buf, 0) == CC_CHAT_EMPTY, "none: %d", cc_chat_valid(buf, 0));
    CHECK(cc_chat_valid(buf, -1) == CC_CHAT_EMPTY, "negative: %d", cc_chat_valid(buf, -1));
    TEST("spaces only are empty");
    CHECK(valid("   ") == CC_CHAT_EMPTY, "spaces: %d", valid("   "));
    CHECK(valid(" a ") == CC_CHAT_OK, "a letter between spaces: %d", valid(" a "));
    TEST("140 bytes of two-byte letters is 70 letters, and fine");
    char e[CC_CHAT_MAX];
    for (int i = 0; i < CC_CHAT_MAX; i += 2) { e[i] = (char)0xc3; e[i + 1] = (char)0xa9; }
    CHECK(cc_chat_valid(e, CC_CHAT_MAX) == CC_CHAT_OK, "70 e-acute: %d", cc_chat_valid(e, CC_CHAT_MAX));
}

static void utf8(void) {
    TEST("well-formed UTF-8 passes");
    CHECK(valid("caf\xc3\xa9") == CC_CHAT_OK, "two bytes");
    CHECK(valid("\xe2\x99\x9e knight") == CC_CHAT_OK, "three bytes (a chess knight)");
    CHECK(valid("gg \xf0\x9f\x91\x8d") == CC_CHAT_OK, "four bytes (thumbs up)");
    CHECK(valid("\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82") == CC_CHAT_OK, "Cyrillic");
    TEST("broken UTF-8 is refused");
    CHECK(valid("a\x80") == CC_CHAT_UTF8, "a lone continuation: %d", valid("a\x80"));
    CHECK(valid("a\xc3") == CC_CHAT_UTF8, "cut short at the end: %d", valid("a\xc3"));
    CHECK(cc_chat_valid("a\xc3\xa9", 2) == CC_CHAT_UTF8, "cut short by the length, its continuation after it: %d", cc_chat_valid("a\xc3\xa9", 2));
    CHECK(valid("\xc3(") == CC_CHAT_UTF8, "a lead without its continuation: %d", valid("\xc3("));
    CHECK(valid("\xc0\xaf") == CC_CHAT_UTF8, "an overlong slash: %d", valid("\xc0\xaf"));
    CHECK(valid("\xe0\x80\xaf") == CC_CHAT_UTF8, "an overlong slash in three: %d", valid("\xe0\x80\xaf"));
    CHECK(valid("\xf0\x80\x80\xaf") == CC_CHAT_UTF8, "an overlong slash in four: %d", valid("\xf0\x80\x80\xaf"));
    CHECK(valid("\xed\xa0\x80") == CC_CHAT_UTF8, "a surrogate: %d", valid("\xed\xa0\x80"));
    CHECK(valid("\xf4\x90\x80\x80") == CC_CHAT_UTF8, "past U+10FFFF: %d", valid("\xf4\x90\x80\x80"));
    CHECK(valid("\xff") == CC_CHAT_UTF8 && valid("\xf8\x88\x80\x80\x80") == CC_CHAT_UTF8, "bytes UTF-8 never uses");
    TEST("the length is the caller's: a NUL inside is a control");
    CHECK(cc_chat_valid("ab\0cd", 5) == CC_CHAT_CONTROL, "NUL: %d", cc_chat_valid("ab\0cd", 5));
}

static void controls(void) {
    TEST("C0, DEL and C1 are refused");
    CHECK(valid("a\tb") == CC_CHAT_CONTROL, "tab: %d", valid("a\tb"));
    CHECK(valid("a\nb") == CC_CHAT_CONTROL, "newline: %d", valid("a\nb"));
    CHECK(valid("a\rb") == CC_CHAT_CONTROL, "return");
    CHECK(valid("\x1b[31mred") == CC_CHAT_CONTROL, "escape");
    CHECK(valid("a\x7f") == CC_CHAT_CONTROL, "DEL: %d", valid("a\x7f"));
    CHECK(valid("a\xc2\x85") == CC_CHAT_CONTROL, "NEL (C1): %d", valid("a\xc2\x85"));
    CHECK(valid("a\xc2\x9f") == CC_CHAT_CONTROL, "the last C1");
    CHECK(valid("a\xc2\xa0") == CC_CHAT_OK, "a no-break space is not a control: %d", valid("a\xc2\xa0"));
    TEST("separators and bidirectional overrides are refused");
    CHECK(valid("a\xe2\x80\xa8") == CC_CHAT_CONTROL, "U+2028");
    CHECK(valid("a\xe2\x80\xa9") == CC_CHAT_CONTROL, "U+2029");
    CHECK(valid("a\xe2\x80\xae") == CC_CHAT_CONTROL, "U+202E right-to-left override: %d", valid("a\xe2\x80\xae"));
    CHECK(valid("a\xe2\x80\xaa") == CC_CHAT_CONTROL, "U+202A");
    CHECK(valid("a\xe2\x81\xa6") == CC_CHAT_CONTROL && valid("a\xe2\x81\xa9") == CC_CHAT_CONTROL, "U+2066..U+2069");
    CHECK(valid("a\xe2\x80\xa6") == CC_CHAT_OK, "an ellipsis (U+2026) is fine: %d", valid("a\xe2\x80\xa6"));
    CHECK(valid("a\xe2\x81\xa5") == CC_CHAT_OK && valid("a\xe2\x81\xaa") == CC_CHAT_OK, "the neighbours of the isolates");
    TEST("a plain line of chess talk passes");
    CHECK(valid("gl hf! 1. e4 e5 2. Nf3 (the Ruy?) 100% #chess @bob ~ {} [] <3") == CC_CHAT_OK, "punctuation");
}

static void normal(void) {
    TEST("the normal form: lowercase, stand-ins read as letters, the rest breaks words");
    char out[64];
    const char *in = "Sh1T$ @r3 4ll 0K, 7op! 2 caf\xc3\xa9";
    int n = (int)strlen(in);
    cc_chat_normal(in, n, out);
    out[n] = 0;
    STREQ(out, "shits are all ok  top    caf  ");
}

static void words(void) {
    TEST("plain profanity and slurs are caught");
    CHECK(banned("fuck"), "fuck");
    CHECK(banned("you are a fucking idiot"), "inside a line");
    CHECK(banned("MOTHERFUCKER"), "inside a word, any case");
    CHECK(banned("shit"), "shit");
    CHECK(banned("bullshit"), "bullshit");
    CHECK(banned("nigger"), "a slur");
    CHECK(banned("faggot"), "a slur");
    CHECK(banned("what a bitch"), "bitch");
    CHECK(banned("kys"), "kys");
    TEST("disguised: stand-ins and repeated letters");
    CHECK(banned("f*ck you") == false, "a star breaks the word (not caught, and not meant to be)");
    CHECK(banned("sh1t"), "1 for i");
    CHECK(banned("$h!t") == false, "! is punctuation, not i");
    CHECK(banned("$hit"), "$ for s");
    CHECK(banned("fuuuuuck"), "a run of u");
    CHECK(banned("FFFUUUCCCKKK"), "every letter repeated");
    CHECK(banned("b1tch"), "1 for i");
    CHECK(banned("@ss"), "@ for a");
    CHECK(banned("455"), "4 for a, 5 for s");
    CHECK(banned("n1gg3r"), "1 and 3");
    CHECK(banned("c0ck"), "0 for o");
    CHECK(banned("7w47"), "7 for t, 4 for a");
    TEST("whole words only: innocent words holding a stem pass");
    CHECK(!banned("class"), "class");
    CHECK(!banned("pass the bishop"), "pass");
    CHECK(!banned("assess the position"), "assess");
    CHECK(!banned("as long as it works"), "as");
    CHECK(banned("ass") && banned("what an ass"), "ass itself");
    CHECK(!banned("cocktail"), "cocktail");
    CHECK(!banned("Dickens wrote it"), "Dickens");
    CHECK(banned("dick"), "dick itself");
    CHECK(!banned("a spicy line"), "spicy");
    TEST("a word's start only: Scunthorpe passes");
    CHECK(!banned("Scunthorpe United"), "Scunthorpe");
    CHECK(banned("cunts"), "the stem with a suffix");
    TEST("a double letter in a stem needs two: the country, the night, the gaggle pass");
    CHECK(!banned("Niger"), "Niger");
    CHECK(!banned("good night"), "night");
    CHECK(!banned("nigel plays the Najdorf"), "Nigel");
    CHECK(!banned("a gaggle of pawns"), "gaggle");
    TEST("ordinary chess talk passes");
    CHECK(!banned("gl hf"), "gl hf");
    CHECK(!banned("gg wp, nice Scotch gambit; 1-0 after Bxf7+ Kxf7"), "notation");
    CHECK(!banned("I hate this hippopotamus opening"), "hippopotamus");
    CHECK(!banned("the Grob is the assassin's choice"), "assassin");
    CHECK(!banned(""), "nothing");
    TEST("the length is the caller's");
    CHECK(cc_chat_banned("fuck", 3) == false, "fuc");
    CHECK(cc_chat_banned("ok fuck", 2) == false, "ok");
}

int main(void) {
    lengths();
    utf8();
    controls();
    normal();
    words();
    return report("unit_chat");
}

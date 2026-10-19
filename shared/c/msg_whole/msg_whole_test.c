/* msg_whole_test.c - the whole-game envelope (msg_whole.h), over a stand-in
 * product with no game: a body is any non-empty bytes.
 *
 *   cc -std=c11 -Wall -Wextra -Werror msg_whole.c msg_whole_test.c ../sha256.c ../b32.c \
 *      ../wire_check/wire_check.c ../msg_seat_tag/msg_seat_tag.c \
 *      ../msg_lobby_roster/msg_lobby_roster.c -o msg_whole_test && ./msg_whole_test
 *
 * Exits 1 on any failure. The salted ids against SHA-256 written out, the
 * lobby kept in step with its rows, the head and roster written and read back
 * for one and two counters, each refusal a reader makes and the order it
 * makes them in, the link text in a URL, the race rule's lobby step and last
 * word, and the seat records with a product's own bytes kept across a put.
 *
 * MUTATION-CHECKED 2026-10-18: sixteen mutations of msg_whole.c, each applied
 * alone to a copy-restored file and rebuilt from a deleted object, each red
 * on the assertion named: a record's own bytes not kept ("a new record's own
 * bytes are zero"), a started-only flag let through ("a started-only flag in
 * a lobby"), the revision's parity ("a revision the count disagrees with"),
 * the link value running past '&' ("&m= in a query"), the digest order
 * flipped ("the smaller SHA-256 wins"), the freed row kept ("the freed row is
 * cleared"), a forget that keeps ("a put goes to the front"), the salt left
 * out ("the tag is SHA-256(salt || seed || id)"), the writer's starter rule
 * ("a lobby with a starter"), a lobby's counters let through ("a lobby with
 * counters"), the oldest record kept ("the store is full"), the seat count
 * left out of the race ("then more seats"), the length cap ("past the longest
 * envelope"), the unknown flags ("an unknown flag"), GONE never reported ("my
 * tag has no row: gone") and the check not compared ("every truncation is refused"). The
 * three products' own suites missed four of these on their own copies. */
#include <stdio.h>
#include <string.h>
#include "msg_whole.h"
#include "../sha256.h"
#include "../le_bytes.h"
/* OK(cond, "what") is check.h's; every failure prints (no per-test cap here). */
#define CHECK_PRINT_CAP 1000000
#include "../test/check.h"


#define REC_LEN (MSG_WHOLE_ID_LEN + MSG_SEAT_TAG_LEN + 1)
#define REC_MAX 4
#define MAX_BYTES (MSG_WHOLE_HEAD_LEN(2) + MSG_WHOLE_MAX_SEATS * MSG_WHOLE_ROW_MAX + MSG_WHOLE_CHECK_LEN + 64)

static const MsgWholeSpec TWO = {
    .seat_salt = "test.seat.1|", .game_salt = "test.game.1|",
    .magic = 0xA1, .format = 3, .flag_dm = 0x01, .flag_left = 0x04,
    .flags_known = 0x07, .flags_live = 0x02, .counters = 2, .group_cap = 5,
    .max_bytes = MAX_BYTES, .rec_len = REC_LEN, .rec_bytes = REC_LEN * REC_MAX,
};
static const MsgWholeSpec ONE = {
    .seat_salt = "one.seat.1|", .game_salt = "one.game.1|",
    .magic = 0xA2, .format = 1, .flag_dm = 0x01, .flag_left = 0x02,
    .flags_known = 0x03, .flags_live = 0, .counters = 1, .group_cap = 8,
    .max_bytes = MAX_BYTES, .rec_len = MSG_WHOLE_ID_LEN + MSG_SEAT_TAG_LEN, .rec_bytes = 17 * 3,
};

static uint8_t SEED[32];
static const uint8_t *U(const char *s) { return (const uint8_t *)s; }
static int L(const char *s) { return (int)strlen(s); }

static void tag_of(const MsgWholeSpec *sp, int who, uint8_t tag[MSG_SEAT_TAG_LEN])
{
    uint8_t id[2] = { (uint8_t)who, 9 };
    msg_whole_tag(sp, SEED, id, 2, tag);
}

/* A lobby of n from a creator, everyone joined by name. */
static void lobby(const MsgWholeSpec *sp, MsgWhole *w, int n, int dm)
{
    static const char *nm[8] = { "Alex", "Bo", "Cy", "Dee", "Eve", "Fin", "Gus", "Hal" };
    uint8_t tag[MSG_SEAT_TAG_LEN];
    tag_of(sp, 0, tag);
    msg_whole_new(w, SEED, dm, tag, U(nm[0]), L(nm[0]));
    for (int s = 1; s < n; s++) {
        tag_of(sp, s, tag);
        msg_whole_join(sp, w, tag, U(nm[s]), L(nm[s]));
    }
}

/* Write w with a body of `bn` bytes (0 in WAITING); the length. */
static int write_env(const MsgWholeSpec *sp, const MsgWhole *w, int extra, const uint16_t *ctr, int bn, uint8_t *buf)
{
    int head = msg_whole_put_head(sp, w, w->phase, extra, ctr, buf);
    for (int i = 0; i < bn; i++) buf[head + MSG_WHOLE_CHECK_LEN + i] = (uint8_t)(0x40 + i);
    return msg_whole_seal(buf, head, bn);
}

static int read_env(const MsgWholeSpec *sp, const uint8_t *in, int n, MsgWhole *w)
{
    int flags, at, bn;
    uint16_t c[MSG_WHOLE_MAX_COUNTERS];
    return msg_whole_read(sp, in, n, w, &flags, c, &at, &bn);
}

static void test_ids(void)
{
    uint8_t tag[MSG_SEAT_TAG_LEN], id[MSG_WHOLE_ID_LEN], d[SHA256_DIGEST_LEN];
    uint8_t who[3] = { 1, 2, 3 };
    msg_whole_tag(&TWO, SEED, who, 3, tag);
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, "test.seat.1|", 12);
    sha256_update(&c, SEED, 32);
    sha256_update(&c, who, 3);
    sha256_final(&c, d);
    OK(!memcmp(tag, d, MSG_SEAT_TAG_LEN), "the tag is SHA-256(salt || seed || id)");
    msg_whole_game_id(&TWO, SEED, id);
    sha256_init(&c);
    sha256_update(&c, "test.game.1|", 12);
    sha256_update(&c, SEED, 32);
    sha256_final(&c, d);
    OK(!memcmp(id, d, MSG_WHOLE_ID_LEN), "the game id is SHA-256(salt || seed)");
    uint8_t other[MSG_WHOLE_ID_LEN];
    msg_whole_game_id(&ONE, SEED, other);
    OK(memcmp(id, other, MSG_WHOLE_ID_LEN), "another product's salt is another id");
}

static void test_lobby(void)
{
    MsgWhole w, before;
    uint8_t tag[MSG_SEAT_TAG_LEN];
    tag_of(&TWO, 0, tag);
    OK(msg_whole_new(&w, SEED, 0, tag, U(""), 0) == MSG_WHOLE_EROSTER, "an empty name creates nothing");
    OK(msg_whole_new(&w, SEED, 0, tag, U("Alex"), 4) == MSG_WHOLE_EOK, "a lobby");
    OK(w.n_seats == 1 && w.starter == MSG_WHOLE_NO_SEAT && !msg_whole_started(&w), "one seat, no starter");
    OK(msg_whole_lobby_sender(&w) == 0, "the creator sent it");
    tag_of(&TWO, 1, tag);
    OK(msg_whole_join(&TWO, &w, tag, U("Alex"), 4) == MSG_WHOLE_EROSTER, "a taken name");
    tag_of(&TWO, 0, tag);
    OK(msg_whole_join(&TWO, &w, tag, U("Bo"), 2) == MSG_WHOLE_EROSTER, "a seated tag");
    before = w;
    tag_of(&TWO, 1, tag);
    OK(msg_whole_join(&TWO, &w, tag, U("Bo"), 2) == 1, "the lowest free seat");
    OK(msg_whole_seat_of_tag(&w, tag) == 1 && w.lobby_rev == 1 && !w.left, "the row, one change");
    MsgLobbyRoster lb, la;
    msg_whole_lobby_pair(&TWO, &before, &w, &lb, &la);
    OK(lb.who[0] == la.who[0] && la.who[1] == 2, "one person, one handle across the two");
    for (int s = 2; s < 5; s++) {
        char nm[4] = { 'P', (char)('0' + s), 0 };
        tag_of(&TWO, s, tag);
        OK(msg_whole_join(&TWO, &w, tag, U(nm), 2) == s, "joins up to the cap");
    }
    tag_of(&TWO, 7, tag);
    OK(msg_whole_join(&TWO, &w, tag, U("Zed"), 3) == MSG_WHOLE_EREFUSED, "full at the group cap");
    OK(msg_whole_offered(&TWO, &w, -1) == MSG_LOBBY_ROSTER_FULL, "a stranger is offered FULL");
    before = w;
    OK(msg_whole_leave(&TWO, &w, 1) == MSG_WHOLE_EOK, "a leave");
    OK(w.n_seats == 4 && w.left && w.lobby_rev == 5 && msg_whole_lobby_sender(&w) == -1, "rows down, a leave sent it");
    OK(!memcmp(w.seat[1].tag, before.seat[2].tag, MSG_SEAT_TAG_LEN), "later rows move down");
    OK(w.seat[4].name_len == 0, "the freed row is cleared");
    OK(msg_whole_leave(&TWO, &w, 9) == MSG_WHOLE_EREFUSED && w.n_seats == 4, "no such seat: untouched");
    msg_whole_mark_started(&w, 2);
    OK(msg_whole_started(&w) && w.starter == 2 && !w.left, "started by seat 2");
    OK(msg_whole_offered(&TWO, &w, 0) == 0, "nothing offered once started");
    tag_of(&TWO, 8, tag);
    OK(msg_whole_join(&TWO, &w, tag, U("Late"), 4) == MSG_WHOLE_EREFUSED, "no join once started");
}

static void test_bytes(void)
{
    static uint8_t buf[MAX_BYTES], cut[MAX_BYTES];
    MsgWhole w, back;
    for (int k = 0; k < 2; k++) {
        const MsgWholeSpec *sp = k ? &ONE : &TWO;
        lobby(sp, &w, 3, 0);
        uint16_t zero[2] = { 0, 0 };
        OK(msg_whole_writable(sp, &w) == MSG_WHOLE_EOK, "a lobby is writable");
        int n = write_env(sp, &w, 0, zero, 0, buf);
        OK(n == MSG_WHOLE_HEAD_LEN(sp->counters) + 3 * 10 + 4 + 2 + 2 + MSG_WHOLE_CHECK_LEN, "the length is the layout's");
        OK(buf[0] == sp->magic && buf[1] == sp->format && buf[2] == MSG_WHOLE_WAITING, "magic, format, phase");
        OK(buf[38 + 2 * sp->counters] == 3 && buf[39 + 2 * sp->counters] == MSG_WHOLE_NO_SEAT, "n_seats, then no starter");
        OK(read_env(sp, buf, n, &back) == MSG_WHOLE_EOK && msg_whole_same(&w, &back), "a lobby reads back the same");

        msg_whole_mark_started(&w, 1);
        uint16_t ctr[2] = { 7, 300 };
        n = write_env(sp, &w, 0, ctr, 5, buf);
        int flags, at, bn;
        uint16_t c[2] = { 0, 0 };
        OK(msg_whole_read(sp, buf, n, &back, &flags, c, &at, &bn) == MSG_WHOLE_EOK, "a started game reads");
        OK(c[0] == 7 && (sp->counters == 1 || c[1] == 300), "the counters");
        OK(bn == 5 && buf[at] == 0x40 && at + bn == n, "the body after the check");
        OK(le_get_u16(buf + 38) == 7, "the first counter after lobby_rev");
        OK(msg_whole_same(&w, &back), "a started game reads back the same");
        back.starter = 0;
        OK(!msg_whole_same(&w, &back), "another starter is another message");

        /* every truncation is a refusal, never a read */
        int all = 1;
        for (int t = 0; t < n; t++) {
            memcpy(cut, buf, (size_t)t);
            if (read_env(sp, cut, t, &back) == MSG_WHOLE_EOK) all = 0;
        }
        OK(all, "every truncation is refused");
        /* every single bit flip is a refusal */
        all = 1;
        for (int i = 0; i < n; i++)
            for (int b = 0; b < 8; b++) {
                memcpy(cut, buf, (size_t)n);
                cut[i] ^= (uint8_t)(1 << b);
                if (read_env(sp, cut, n, &back) == MSG_WHOLE_EOK) all = 0;
            }
        OK(all, "every bit flip is refused");
    }
}

/* The refusals, each alone and in the reader's order. */
static void test_refusals(void)
{
    static uint8_t buf[MAX_BYTES], b2[MAX_BYTES];
    MsgWhole w, back;
    uint16_t zero[2] = { 0, 0 }, ctr[2] = { 1, 1 };
    lobby(&TWO, &w, 2, 0);
    int n = write_env(&TWO, &w, 0, zero, 0, buf);
    OK(read_env(&TWO, 0, n, &back) == MSG_WHOLE_ESHORT, "no bytes");
    OK(read_env(&TWO, buf, 1, &back) == MSG_WHOLE_ESHORT, "one byte");
    memcpy(b2, buf, (size_t)n); b2[0] ^= 1;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EMAGIC, "another magic");
    memcpy(b2, buf, (size_t)n); b2[1] ^= 1; b2[0] ^= 1;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EMAGIC, "the magic is read before the format");
    memcpy(b2, buf, (size_t)n); b2[1] ^= 1;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EFORMAT, "another format");
    OK(read_env(&TWO, buf, MSG_WHOLE_HEAD_LEN(2) - 1, &back) == MSG_WHOLE_ESHORT, "a short head");
    OK(read_env(&TWO, buf, MAX_BYTES + 1, &back) == MSG_WHOLE_EGAME, "past the longest envelope");
    memcpy(b2, buf, (size_t)n); b2[3] |= 0x80;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EFLAGS, "an unknown flag");
    memcpy(b2, buf, (size_t)n); b2[3] |= 0x02;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EFLAGS, "a started-only flag in a lobby");
    memcpy(b2, buf, (size_t)n); b2[3] |= 0x80; b2[42] = 99;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EFLAGS, "flags before the seat count");
    memcpy(b2, buf, (size_t)n); b2[42] = 6;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EROSTER, "past the group cap");
    memcpy(b2, buf, (size_t)n); b2[42] = 0;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EROSTER, "no seats");
    memcpy(b2, buf, (size_t)n); b2[44 + MSG_SEAT_TAG_LEN] = 0;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EROSTER, "an empty row name");
    memcpy(b2, buf, (size_t)n); b2[n - 1] ^= 1;
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_ECHECK, "an edited check");

    /* checked but inconsistent: written by hand and sealed */
    MsgWhole bad = w;
    bad.lobby_rev = 2;
    n = write_env(&TWO, &bad, 0, zero, 0, b2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EROSTER, "a revision the count disagrees with");
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "and nobody writes one");
    bad = w;
    memcpy(bad.seat[1].name, "Alex", 4); bad.seat[1].name_len = 4;
    n = write_env(&TWO, &bad, 0, zero, 0, b2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EROSTER, "a name twice");
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "and nobody writes one");
    bad = w;
    memcpy(bad.seat[1].tag, bad.seat[0].tag, MSG_SEAT_TAG_LEN);
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "a tag twice");
    bad = w;
    bad.starter = 0;
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "a lobby with a starter");
    n = write_env(&TWO, &w, 0, ctr, 0, b2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EGAME, "a lobby with counters");
    n = write_env(&TWO, &w, 0, zero, 3, b2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EGAME, "a lobby with a body");
    bad = w;
    bad.dm = 1;
    tag_of(&TWO, 5, bad.seat[2].tag);
    memcpy(bad.seat[2].name, "Cy", 2); bad.seat[2].name_len = 2; bad.n_seats = 3; bad.lobby_rev = 2;
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "three in a two-person chat");

    msg_whole_mark_started(&w, 1);
    OK(msg_whole_writable(&TWO, &w) == MSG_WHOLE_EOK, "a started game is writable");
    n = write_env(&TWO, &w, 0x02, ctr, 0, b2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EGAME, "a started game with no body");
    bad = w;
    bad.starter = 2;
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "a starter past the seats");
    n = write_env(&TWO, &bad, 0, ctr, 2, b2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EROSTER, "and nobody reads one");
    bad = w;
    bad.n_seats = 1; bad.lobby_rev = 0; bad.starter = 0;
    OK(msg_whole_writable(&TWO, &bad) == MSG_WHOLE_EROSTER, "a started game of one");
    n = write_env(&TWO, &w, 0, ctr, 2, b2);
    b2[3] |= 0x04;
    msg_whole_seal(b2, n - 2 - MSG_WHOLE_CHECK_LEN, 2);
    OK(read_env(&TWO, b2, n, &back) == MSG_WHOLE_EFLAGS, "LEFT in a started game");
}

static void test_text(void)
{
    static uint8_t buf[MAX_BYTES], got[MAX_BYTES];
    static char text[2 * MAX_BYTES], url[2 * MAX_BYTES + 64], span[2 * MAX_BYTES];
    MsgWhole w;
    uint16_t zero[2] = { 0, 0 };
    lobby(&TWO, &w, 4, 0);
    int n = write_env(&TWO, &w, 0, zero, 0, buf);
    int t = msg_whole_text_put(buf, n, text, (int)sizeof text);
    OK(t > 3 && !memcmp(text, "?m=", 3) && text[t] == 0, "?m= and base32");
    OK(msg_whole_text_put(buf, -6, text, (int)sizeof text) == -6, "a refusal is handed back");
    OK(msg_whole_text_put(buf, n, url, 3) == MSG_WHOLE_ECAP, "no room for a character");
    OK(msg_whole_text_put(buf, n, url, t) == MSG_WHOLE_ECAP, "no room for the NUL");
    OK(msg_whole_text_get(text, span, (int)sizeof span, got, (int)sizeof got) == n && !memcmp(got, buf, (size_t)n),
       "the text reads back");
    snprintf(url, sizeof url, "https://h/p?a=1&m=%s&b=AAAA#frag", text + 3);
    OK(msg_whole_text_get(url, span, (int)sizeof span, got, (int)sizeof got) == n && !memcmp(got, buf, (size_t)n),
       "&m= in a query, up to the next parameter");
    snprintf(url, sizeof url, "https://h/p?mm=%s", text + 3);
    OK(msg_whole_text_get(url, span, (int)sizeof span, got, (int)sizeof got) == MSG_WHOLE_ETEXT, "mm= is not m=");
    OK(msg_whole_text_get(0, span, (int)sizeof span, got, (int)sizeof got) == MSG_WHOLE_ETEXT, "no text");
    OK(msg_whole_text_get("?m=", span, (int)sizeof span, got, (int)sizeof got) == MSG_WHOLE_ETEXT, "an empty value");
    OK(msg_whole_text_get(text, span, 8, got, (int)sizeof got) == MSG_WHOLE_ETEXT, "a value longer than the span");
}

static void test_prefer(void)
{
    MsgWhole a, b;
    lobby(&TWO, &a, 3, 0);
    b = a;
    OK(msg_whole_same_game(&a, &b), "one seed, one game");
    OK(msg_whole_more(2, 1) < 0 && msg_whole_more(1, 2) > 0 && msg_whole_more(3, 3) == 0, "the larger wins");
    b.lobby_rev = 3;
    OK(msg_whole_prefer_lobby(&a, &b) > 0 && msg_whole_prefer_lobby(&b, &a) < 0, "the higher revision");
    b = a;
    b.n_seats = 4;
    OK(msg_whole_prefer_lobby(&a, &b) > 0, "then more seats");
    OK(msg_whole_prefer_lobby(&a, &a) == 0, "a tie goes on");
    const uint8_t x[3] = { 1, 2, 3 }, y[3] = { 1, 2, 4 };
    uint8_t dx[32], dy[32];
    sha256(x, 3, dx);
    sha256(y, 3, dy);
    int want = memcmp(dx, dy, 32) < 0 ? -1 : 1;
    OK(msg_whole_prefer_bytes(x, 3, y, 3) == want && msg_whole_prefer_bytes(y, 3, x, 3) == -want,
       "the smaller SHA-256 wins, either way round");
    OK(msg_whole_prefer_bytes(x, 3, x, 3) == 0, "the same bytes tie");
    OK(msg_whole_prefer_bytes(x, -6, y, 3) == 1 && msg_whole_prefer_bytes(x, 3, y, -6) == -1, "unwritable loses");
    OK(msg_whole_prefer_bytes(x, -6, y, -7) == 0, "two unwritable tie");
}

static void test_records(void)
{
    static uint8_t recs[REC_LEN * REC_MAX];
    MsgWhole w[6];
    int n = 0;
    for (int k = 0; k < 6; k++) {
        SEED[0] = (uint8_t)(100 + k);
        lobby(&TWO, &w[k], 3, 0);
    }
    OK(msg_whole_rec_find(&TWO, 0, 99, &w[0]) == -1, "no records");
    n = msg_whole_rec_put(&TWO, recs, n, &w[0], 2);
    OK(n == REC_LEN && msg_whole_rec_find(&TWO, recs, n, &w[0]) == 2, "a record finds its seat");
    OK(msg_whole_rec_find(&TWO, recs, n, &w[1]) == -1, "another game has none");
    OK(msg_whole_rec_put(&TWO, recs, n, &w[0], 3) == n && msg_whole_rec_put(&TWO, recs, n, &w[0], -1) == n,
       "no seat, no record");
    recs[REC_LEN - 1] = 0x5c;                          /* the product's own byte */
    n = msg_whole_rec_put(&TWO, recs, n, &w[1], 0);
    n = msg_whole_rec_put(&TWO, recs, n, &w[0], 1);
    OK(n == 2 * REC_LEN && msg_whole_rec_at(&TWO, recs, n, &w[0]) == 0, "a put goes to the front");
    OK(recs[REC_LEN - 1] == 0x5c, "and keeps the record's own bytes");
    OK(recs[2 * REC_LEN - 1] == 0, "a new record's own bytes are zero");
    OK(msg_whole_rec_find(&TWO, recs, n, &w[0]) == 1, "the newest seat");
    MsgWhole moved = w[0];
    msg_whole_leave(&TWO, &moved, 1);
    OK(msg_whole_rec_find(&TWO, recs, n, &moved) == MSG_SEAT_REC_GONE, "my tag has no row: gone");
    for (int k = 2; k < 6; k++) n = msg_whole_rec_put(&TWO, recs, n, &w[k], 0);
    OK(n == REC_LEN * REC_MAX, "the store is full");
    OK(msg_whole_rec_find(&TWO, recs, n, &w[0]) == -1 && msg_whole_rec_find(&TWO, recs, n, &w[1]) == -1,
       "the oldest fell off");
    OK(msg_whole_rec_at(&TWO, recs, n, &w[5]) == 0, "the newest is first");
    n = msg_whole_rec_forget(&TWO, recs, n, &w[3]);
    OK(n == REC_LEN * (REC_MAX - 1) && msg_whole_rec_find(&TWO, recs, n, &w[3]) == -1, "a forget");
    OK(msg_whole_rec_find(&TWO, recs, n, &w[2]) == 0 && msg_whole_rec_find(&TWO, recs, n, &w[4]) == 0,
       "the others stay");
    OK(msg_whole_rec_n(&TWO, -4) == 0 && msg_whole_rec_n(&TWO, REC_LEN + 3) == REC_LEN
       && msg_whole_rec_n(&TWO, 9999) == REC_LEN * REC_MAX, "a count is whole records within the store");
    SEED[0] = 0;
}

static void test_resolve(void)
{
    MsgWhole w;
    lobby(&TWO, &w, 3, 0);
    int by = -1;
    OK(msg_whole_resolve(&w, msg_whole_lobby_sender(&w), -1, 1, 0, MSG_SEAT_SENT_UNKNOWN, 0, 0, &by) == 1
       && by == MSG_SEAT_BY_TAG, "the tag witness");
    OK(msg_whole_resolve(&w, 2, -1, -1, 0, MSG_SEAT_SENT_UNKNOWN, U("Bo"), 2, &by) == 1
       && by == MSG_SEAT_BY_NAME, "the name witness");
}

int main(void)
{
    for (int i = 0; i < 32; i++) SEED[i] = (uint8_t)(i * 7 + 3);
    SEED[0] = 0;
    test_ids();
    test_lobby();
    test_bytes();
    test_refusals();
    test_text();
    test_prefer();
    test_records();
    test_resolve();
    return report("msg_whole");
}

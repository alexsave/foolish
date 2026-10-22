// unit_lila_wire.c - lila's Redis lines (lila_wire.h): the reads of lila's lines, the writes of
// ours, and the round-robin lane rule (Java's String.hashCode, Scala's Int.abs).
#include "lila_wire.h"
#include "t.h"

static void parse(const char *s, lo_msg *m) { lo_parse(s, strlen(s), m); }
static bool span_is(cb_span s, const char *want) { return s.n == strlen(want) && memcmp(s.p, want, s.n) == 0; }

static void test_parse(void) {
    lo_msg m;
    parse("r/ver abcdefgh 12 Wt move {\"uci\":\"e2 e4\"}", &m);
    T_CHECK(m.kind == LO_R_VER && m.n == 5);
    T_CHECK(span_is(m.arg[0], "abcdefgh") && span_is(m.arg[1], "12") && span_is(m.arg[2], "Wt") && span_is(m.arg[3], "move"));
    T_CHECK(span_is(m.arg[4], "{\"uci\":\"e2 e4\"}"));   // the payload keeps its spaces
    parse("r/ver abcdefgh 12", &m); T_CHECK(m.kind == LO_R_VER && m.n < m.want);
    parse("tell/users a,b {\"t\":\"x\"}", &m); T_CHECK(m.kind == LO_TELL_USERS && m.n == 2 && span_is(m.arg[0], "a,b"));
    parse("tell/all {\"t\":\"x\",\"d\":\"a b\"}", &m); T_CHECK(m.kind == LO_TELL_ALL && span_is(m.arg[0], "{\"t\":\"x\",\"d\":\"a b\"}"));
    parse("boot", &m); T_CHECK(m.kind == LO_BOOT && m.n == 0 && m.want == 0);
    parse("r/versioning-ready", &m); T_CHECK(m.kind == LO_VERSIONING_READY);
    parse("announce/update", &m); T_CHECK(m.kind == LO_ANNOUNCE_UPDATE && m.n == 1 && m.arg[0].n == 0);
    parse("announce/update {\"msg\":\"hi\"}", &m); T_CHECK(m.kind == LO_ANNOUNCE_UPDATE && span_is(m.arg[0], "{\"msg\":\"hi\"}"));
    parse("mod/troll/set bob +", &m); T_CHECK(m.kind == LO_SET_TROLL && lo_bool(m.arg[1]));
    parse("r/finish abcdefgh - alice,bob", &m); T_CHECK(m.kind == LO_R_FINISH && lo_absent(m.arg[1]) && span_is(m.arg[2], "alice,bob"));
    parse("lobby/pairings s1:g1wwww,s2:g1bbbb", &m); T_CHECK(m.kind == LO_LOBBY_PAIRINGS);
    parse("nothing/known a b", &m); T_CHECK(m.kind == LO_UNKNOWN);
    parse("", &m); T_CHECK(m.kind == LO_UNKNOWN);
    parse("pong 123456", &m);
    long long v;
    T_CHECK(m.kind == LO_PONG && lo_i64(m.arg[0], &v) && v == 123456);
    int i;
    T_CHECK(!lo_int(cb_span_of("12x", 3), &i) && lo_int(cb_span_of("-7", 2), &i) && i == -7);
    char item[16];
    size_t it = 0;
    cb_span list = cb_span_of("a,bb,ccc", 8);
    T_CHECK(lo_commas(list, &it, item, sizeof item) && strcmp(item, "a") == 0);
    T_CHECK(lo_commas(list, &it, item, sizeof item) && strcmp(item, "bb") == 0);
    T_CHECK(lo_commas(list, &it, item, sizeof item) && strcmp(item, "ccc") == 0);
    T_CHECK(!lo_commas(list, &it, item, sizeof item));
    it = 0;
    T_CHECK(!lo_commas(cb_span_of("-", 1), &it, item, sizeof item));
}

static void test_lane(void) {
    // Java: "boot".hashCode() == 3029746, "r/move abcdefghwwww e2e4 - - - -".hashCode() computed the same way
    T_CHECK(li_round_lane("boot", 4) == 3029746 % 16);
    T_CHECK(li_round_lane("", 0) == 0);
    T_CHECK(li_round_lane("a", 1) == 97 % 16);
    // "polygenelubricants".hashCode() == Integer.MIN_VALUE, whose Scala abs stays negative: lane 0
    T_CHECK(li_round_lane("polygenelubricants", 18) == 0);
    // a hash that wraps negative: abs then modulo. "ping 1879".hashCode() is negative on the JVM.
    const char *s = "ping 1879";
    uint32_t h = 0;
    for (size_t i = 0; i < strlen(s); i++) h = 31u * h + (uint8_t)s[i];
    int32_t sh = (int32_t)h;
    T_CHECK(sh < 0);
    T_CHECK(li_round_lane(s, strlen(s)) == (-sh) % 16);
}

static void test_write(void) {
    cb_buf b;
    cb_buf_init(&b);
    li_tell_sri(&b, "sri1", "alice", "{\"t\":1}", 7); T_STREQ(cb_buf_cstr(&b), "tell/sri sri1 alice {\"t\":1}"); cb_buf_reset(&b);
    li_tell_sri(&b, "sri1", NULL, "{}", 2); T_STREQ(cb_buf_cstr(&b), "tell/sri sri1 - {}"); cb_buf_reset(&b);
    li_tell_user(&b, "bob", "{}", 2); T_STREQ(cb_buf_cstr(&b), "tell/user bob {}"); cb_buf_reset(&b);
    li_notified_batch(&b, ""); T_STREQ(cb_buf_cstr(&b), "notified/batch -"); cb_buf_reset(&b);
    li_lags(&b, "a:1,b:2"); T_STREQ(cb_buf_cstr(&b), "lags a:1,b:2"); cb_buf_reset(&b);
    li_connect_user(&b, "alice"); T_STREQ(cb_buf_cstr(&b), "connect/user alice"); cb_buf_reset(&b);
    li_disconnect_users(&b, NULL); T_STREQ(cb_buf_cstr(&b), "disconnect/users -"); cb_buf_reset(&b);
    li_boot(&b); T_STREQ(cb_buf_cstr(&b), "boot"); cb_buf_reset(&b);
    li_ping(&b, 12345); T_STREQ(cb_buf_cstr(&b), "ping 12345"); cb_buf_reset(&b);
    li_connect_sris(&b, "s1 alice,s2"); T_STREQ(cb_buf_cstr(&b), "connect/sris s1 alice,s2"); cb_buf_reset(&b);
    li_disconnect_sris(&b, "s1"); T_STREQ(cb_buf_cstr(&b), "disconnect/sris s1"); cb_buf_reset(&b);
    li_counters(&b, 10, 3); T_STREQ(cb_buf_cstr(&b), "counters 10 3"); cb_buf_reset(&b);
    li_chat_say(&b, "abcdefgh", "w", "hello there"); T_STREQ(cb_buf_cstr(&b), "chat/say abcdefgh w hello there"); cb_buf_reset(&b);
    li_chat_say_w(&b, "abcdefgh", "bob", "hi"); T_STREQ(cb_buf_cstr(&b), "chat/say/w abcdefgh bob hi"); cb_buf_reset(&b);
    li_chat_timeout(&b, "abcdefgh", "mod", "troll", "spam", "buy now"); T_STREQ(cb_buf_cstr(&b), "chat/timeout abcdefgh mod troll spam buy now"); cb_buf_reset(&b);
    li_room_versions(&b, "a:1,b:2"); T_STREQ(cb_buf_cstr(&b), "room/versions a:1,b:2"); cb_buf_reset(&b);
    li_r_do(&b, "abcdefghwwww", "{\"t\":\"resign\"}", 14); T_STREQ(cb_buf_cstr(&b), "r/do abcdefghwwww {\"t\":\"resign\"}"); cb_buf_reset(&b);
    li_r_move(&b, "abcdefghwwww", "e2e4", true, 3, -1, 12); T_STREQ(cb_buf_cstr(&b), "r/move abcdefghwwww e2e4 + 3 - 12"); cb_buf_reset(&b);
    li_r_move(&b, "abcdefghwwww", "e2e4", false, -1, -1, -1); T_STREQ(cb_buf_cstr(&b), "r/move abcdefghwwww e2e4 - - - -"); cb_buf_reset(&b);
    li_r_berserk(&b, "abcdefgh", "bob"); T_STREQ(cb_buf_cstr(&b), "r/berserk abcdefgh bob"); cb_buf_reset(&b);
    li_r_hold(&b, "abcdefghwwww", "1.2.3.4", 200, 10); T_STREQ(cb_buf_cstr(&b), "r/hold abcdefghwwww 1.2.3.4 200 10"); cb_buf_reset(&b);
    li_r_report(&b, "abcdefghwwww", "1.2.3.4", NULL, "shift"); T_STREQ(cb_buf_cstr(&b), "r/report abcdefghwwww 1.2.3.4 - shift"); cb_buf_reset(&b);
    li_r_flag(&b, "abcdefgh", 1, "wwww"); T_STREQ(cb_buf_cstr(&b), "r/flag abcdefgh b wwww"); cb_buf_reset(&b);
    li_r_flag(&b, "abcdefgh", 0, NULL); T_STREQ(cb_buf_cstr(&b), "r/flag abcdefgh w -"); cb_buf_reset(&b);
    li_r_bye(&b, "abcdefghwwww"); T_STREQ(cb_buf_cstr(&b), "r/bye abcdefghwwww"); cb_buf_reset(&b);
    li_r_ons(&b, "abcdefgh+-,ijklmnop"); T_STREQ(cb_buf_cstr(&b), "r/ons abcdefgh+-,ijklmnop"); cb_buf_reset(&b);
    li_r_latency(&b, 4); T_STREQ(cb_buf_cstr(&b), "r/latency 4"); cb_buf_reset(&b);
    li_req_response(&b, 7, "See you"); T_STREQ(cb_buf_cstr(&b), "req/response 7 See you"); cb_buf_reset(&b);
    cb_buf_free(&b);
}

int main(void) {
    test_parse();
    test_lane();
    test_write();
    return t_done("unit_lila_wire");
}
